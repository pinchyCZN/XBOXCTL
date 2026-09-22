/*
 * wdm.c - the OS-facing layer of xboxctl.
 *
 * Entry points, the HID minidriver contract, the poll engine, the report
 * queue and the pending-read list. The only file that may name a kernel type.
 *
 * It compiles twice. The harness build replaces the DDK headers with kstub.h
 * and stubs out the four paths that are pure Windows plumbing with no logic
 * in them: USB enumeration, URB submission, PnP and power. Everything
 * downstream of a completed transfer compiles and runs identically in both,
 * which is the half worth testing.
 */
#include "wdm.h"

/*
 * The periodic tick is a FLOOR, not the clock.
 *
 * Autofire deadlines and the mouse velocity accumulator both need a clock
 * independent of the caller, and the natural one is packet arrival: the pad
 * delivers at its endpoint interval whether or not anything changed, so
 * XcOnTransfer runs the tick on every accepted packet and that is the rate
 * the engine actually sees.
 *
 * A KTIMER cannot supply that rate. Its resolution is the system clock tick,
 * nominally 15.6ms, so asking for 4ms gets 15.6ms. This timer exists only to
 * keep time advancing if packets stop arriving - a stalled pipe, a pad
 * unplugged mid-autofire - so that whatever is held gets released.
 */
#define XC_TICK_PERIOD_MS       16

/* ======================================================================
 * SEAMS WIRED ONLY IN ONE BUILD
 * ====================================================================== */

#ifndef XBOXCTL_USERMODE

void XcSetAddDevice(PDRIVER_OBJECT DriverObject, PDRIVER_ADD_DEVICE AddDevice)
{
	DriverObject->DriverExtension->AddDevice = AddDevice;
}

#else

void XcSetAddDevice(PDRIVER_OBJECT DriverObject, PDRIVER_ADD_DEVICE AddDevice)
{
	(void)DriverObject;
	(void)AddDevice;
}

#endif

/* ======================================================================
 * THE REPORT QUEUE
 * ====================================================================== */

/*
 * Coalescing policy, by report kind. This is a property of the data, not a
 * tuning choice:
 *
 *     gamepad    pure state - a queued report is superseded by the next,
 *                and only the newest matters. Without this, an application
 *                opening the collection after a quiet period is handed a
 *                burst of history before it sees the present.
 *     keyboard   a transition. A down and its up carry meaning only in
 *                sequence; merging them loses the keystroke entirely.
 *     mouse      the same for buttons. Motion could in principle be summed,
 *                but not across a button change, so it is not merged here.
 */
static int XcReportCoalesces(u8 report_id)
{
	return (report_id == CORE_REPORT_ID_GAMEPAD);
}

static void XcCompleteRead(PIRP Irp, const UCHAR *Data, UCHAR Length)
{
	if (Irp->UserBuffer != NULL) {
		RtlCopyMemory(Irp->UserBuffer, Data, Length);
	}
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = Length;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/*
 * Pull the oldest waiting read off the list, or NULL. The caller must not
 * hold QueueLock: the cancel spin lock is acquired here and the two must
 * always be taken in this order.
 */
static PIRP XcDequeueRead(PXC_DEVEXT DevExt)
{
	KIRQL       cancel_irql;
	KIRQL       irql;
	PIRP        irp = NULL;
	PLIST_ENTRY entry;

	IoAcquireCancelSpinLock(&cancel_irql);
	KeAcquireSpinLock(&DevExt->QueueLock, &irql);

	if (!IsListEmpty(&DevExt->PendingReads)) {
		entry = RemoveHeadList(&DevExt->PendingReads);
		DevExt->PendingReadCount--;
		irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
		IoSetCancelRoutine(irp, NULL);
	}

	KeReleaseSpinLock(&DevExt->QueueLock, irql);
	IoReleaseCancelSpinLock(cancel_irql);
	return irp;
}

int XcDequeueReport(PXC_DEVEXT DevExt, XC_REPORT_NODE *out)
{
	KIRQL irql;
	int   have = 0;

	KeAcquireSpinLock(&DevExt->QueueLock, &irql);
	if (DevExt->ReportCount > 0) {
		*out = DevExt->ReportQueue[DevExt->ReportHead];
		DevExt->ReportHead =
		        (DevExt->ReportHead + 1) % XC_REPORT_QUEUE_MAX;
		DevExt->ReportCount--;
		have = 1;
	}
	KeReleaseSpinLock(&DevExt->QueueLock, irql);
	return have;
}

void XcQueueReport(PXC_DEVEXT DevExt, u8 report_id, const u8 *payload, u32 len)
{
	KIRQL irql;
	ULONG slot;
	ULONG i;
	ULONG scan;

	if (len + 1 > CORE_REPORT_MAX_BYTES) {
		return;
	}

	KeAcquireSpinLock(&DevExt->QueueLock, &irql);

	if (XcReportCoalesces(report_id)) {
		/* Replace a queued report of the same ID rather than append. */
		for (i = 0; i < DevExt->ReportCount; i++) {
			scan = (DevExt->ReportHead + i) % XC_REPORT_QUEUE_MAX;
			if (DevExt->ReportQueue[scan].Data[0] == report_id) {
				DevExt->ReportQueue[scan].Length = (UCHAR)(len + 1);
				RtlCopyMemory(&DevExt->ReportQueue[scan].Data[1],
				              payload, len);
				KeReleaseSpinLock(&DevExt->QueueLock, irql);
				return;
			}
		}
	}

	if (DevExt->ReportCount >= XC_REPORT_QUEUE_MAX) {
		/*
		 * A FULL QUEUE DROPS THE NEWEST, not the oldest. Older
		 * transitions are already committed and dropping one
		 * desynchronises the host's idea of what is held; the newest
		 * state can be re-derived on the next packet.
		 */
		DevExt->ReportsDropped++;
		KeReleaseSpinLock(&DevExt->QueueLock, irql);
		return;
	}

	slot = (DevExt->ReportHead + DevExt->ReportCount) % XC_REPORT_QUEUE_MAX;
	DevExt->ReportQueue[slot].Length = (UCHAR)(len + 1);
	DevExt->ReportQueue[slot].Data[0] = report_id;
	RtlCopyMemory(&DevExt->ReportQueue[slot].Data[1], payload, len);
	DevExt->ReportCount++;

	KeReleaseSpinLock(&DevExt->QueueLock, irql);
}

/*
 * The report sink. core_emit has already decided the report should exist;
 * this is where it meets the OS.
 *
 * Hand it straight to a waiting read if there is one, otherwise queue it.
 * The order is deliberate: a read that is already parked is older than this
 * report, so serving it first keeps reports in order.
 */
void XcReportSink(void *ctx, u8 report_id, const u8 *payload, u32 len)
{
	PXC_DEVEXT DevExt = (PXC_DEVEXT)ctx;
	PIRP       irp;
	UCHAR      buf[CORE_REPORT_MAX_BYTES];

	if (len + 1 > CORE_REPORT_MAX_BYTES) {
		return;
	}

	irp = XcDequeueRead(DevExt);
	if (irp != NULL) {
		buf[0] = report_id;
		RtlCopyMemory(&buf[1], payload, len);
		XcCompleteRead(irp, buf, (UCHAR)(len + 1));
		return;
	}

	XcQueueReport(DevExt, report_id, payload, len);
}

static void NTAPI XcCancelRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PXC_DEVEXT DevExt = XC_GET_DEVEXT(DeviceObject);
	KIRQL      irql;

	/* IoAcquireCancelSpinLock was taken by the I/O manager on our behalf;
	 * releasing it is this routine's job. */
	KeAcquireSpinLock(&DevExt->QueueLock, &irql);
	RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
	DevExt->PendingReadCount--;
	KeReleaseSpinLock(&DevExt->QueueLock, irql);

	IoReleaseCancelSpinLock(Irp->CancelIrql);

	Irp->IoStatus.Status = STATUS_CANCELLED;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static NTSTATUS XcQueueRead(PXC_DEVEXT DevExt, PIRP Irp)
{
	KIRQL cancel_irql;
	KIRQL irql;

	IoAcquireCancelSpinLock(&cancel_irql);

	if (Irp->Cancel) {
		IoReleaseCancelSpinLock(cancel_irql);
		Irp->IoStatus.Status = STATUS_CANCELLED;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_CANCELLED;
	}

	KeAcquireSpinLock(&DevExt->QueueLock, &irql);
	InsertTailList(&DevExt->PendingReads, &Irp->Tail.Overlay.ListEntry);
	DevExt->PendingReadCount++;
	KeReleaseSpinLock(&DevExt->QueueLock, irql);

	IoSetCancelRoutine(Irp, XcCancelRead);
	IoMarkIrpPending(Irp);

	IoReleaseCancelSpinLock(cancel_irql);
	return STATUS_PENDING;
}

/*
 * Complete every parked read. Device stop and removal must come through
 * here: a pending IRP left on the list hangs the stack teardown and leaks
 * the IRP with it.
 */
static void XcCancelPendingReads(PXC_DEVEXT DevExt)
{
	PIRP irp;

	for (;;) {
		irp = XcDequeueRead(DevExt);
		if (irp == NULL) {
			break;
		}
		irp->IoStatus.Status = STATUS_DELETE_PENDING;
		irp->IoStatus.Information = 0;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
	}
}

/* ======================================================================
 * THE IN-FLIGHT COUNT
 *
 * A hand-rolled remove lock, scoped to the one thing that needs it: the
 * transfers the poll engine has outstanding.
 *
 * The count starts at one, a reference held by the device itself. Each
 * submitted transfer takes another and its completion drops it.
 * XcIoDrainAndWait stops further acquires, drops the device's reference and
 * waits for the rest, so by the time it returns nothing can complete into
 * memory that is about to be freed.
 * ====================================================================== */

BOOLEAN XcIoAcquire(PXC_DEVEXT DevExt)
{
	KIRQL   irql;
	BOOLEAN ok = FALSE;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	if (!DevExt->IoDraining) {
		DevExt->IoCount++;
		ok = TRUE;
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);
	return ok;
}

void XcIoRelease(PXC_DEVEXT DevExt)
{
	KIRQL irql;
	LONG  remaining;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	remaining = --DevExt->IoCount;
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	if (remaining == 0) {
		KeSetEvent(&DevExt->IoIdle, IO_NO_INCREMENT, FALSE);
	}
}

void XcIoDrainAndWait(PXC_DEVEXT DevExt)
{
	KIRQL irql;
	LONG  remaining;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	if (DevExt->IoDraining) {
		/* Already drained. Draining twice would decrement a reference
		 * that is no longer held. */
		KeReleaseSpinLock(&DevExt->PollLock, irql);
		return;
	}
	DevExt->IoDraining = TRUE;
	remaining = --DevExt->IoCount;
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	if (remaining > 0) {
		KeWaitForSingleObject(&DevExt->IoIdle, Executive, KernelMode,
		                      FALSE, NULL);
	}
}

/* ======================================================================
 * THE POLL ENGINE
 * ====================================================================== */

/*
 * Accept one completed transfer. Everything above the URB, and nothing
 * below it - which is why the harness can drive this directly.
 */
void XcOnTransfer(PXC_DEVEXT DevExt, NTSTATUS Status, const u8 *Buffer,
                  ULONG Length, u64 Now100ns)
{
	KIRQL irql;

	if (!NT_SUCCESS(Status) || Length != CORE_RAW_PACKET_BYTES) {
		DevExt->PollErrors++;
		return;
	}

	KeAcquireSpinLock(&DevExt->CoreLock, &irql);
	core_on_packet(&DevExt->Core, Buffer, Length, Now100ns);
	core_tick(&DevExt->Core, Now100ns);
	KeReleaseSpinLock(&DevExt->CoreLock, irql);
}

#ifndef XBOXCTL_USERMODE

static NTSTATUS XcPollComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                               PVOID Context)
{
	XC_POLL_SLOT *slot = (XC_POLL_SLOT *)Context;
	PXC_DEVEXT    DevExt = (PXC_DEVEXT)slot->DevExt;
	ULONG         transferred = 0;
	NTSTATUS      status = Irp->IoStatus.Status;

	(void)DeviceObject;

	/*
	 * VALIDATE ALL THREE, not just the IRP status. A short transfer with
	 * a successful status leaves stale bytes from the previous packet in
	 * the buffer and the decode cannot tell.
	 */
	if (NT_SUCCESS(status)) {
		PURB urb = slot->Urb;
		if (USBD_ERROR(urb->UrbHeader.Status)) {
			status = STATUS_UNSUCCESSFUL;
		} else {
			transferred =
			  urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
		}
	}

	XcOnTransfer(DevExt, status, slot->Buffer, transferred,
	             (u64)KeQueryInterruptTime());

	/*
	 * Resubmit this slot immediately. The other slot is still in flight,
	 * so the endpoint is never left unqueued.
	 *
	 * RESUBMIT BEFORE RELEASING. The new transfer takes its own reference
	 * while this one still holds its own, so the count never touches zero
	 * while polling is meant to continue - and a drain that runs in
	 * between cannot conclude the device is idle when it is not.
	 */
	if (DevExt->PollStopMask == 0 && !DevExt->Removed) {
		XcPollSubmit(DevExt, slot->Index);
	} else {
		slot->Active = FALSE;
	}

	XcIoRelease(DevExt);

	/* The IRP is ours and is reused, so the I/O manager must not touch
	 * it after this returns. */
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS XcPollSubmit(PXC_DEVEXT DevExt, ULONG SlotIndex)
{
	XC_POLL_SLOT      *slot = &DevExt->Poll[SlotIndex];
	PIO_STACK_LOCATION stack;

	if (slot->Irp == NULL || slot->Urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	/* A refused reference means teardown has begun; do not start work
	 * that would complete into memory about to be freed. */
	if (!XcIoAcquire(DevExt)) {
		slot->Active = FALSE;
		return STATUS_DELETE_PENDING;
	}

	UsbBuildInterruptOrBulkTransferRequest(
	        slot->Urb,
	        (USHORT)sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
	        DevExt->InPipe,
	        slot->Buffer,
	        NULL,
	        CORE_RAW_PACKET_BYTES,
	        USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK,
	        NULL);

	IoSetCompletionRoutine(slot->Irp, XcPollComplete, slot, TRUE, TRUE, TRUE);

	stack = IoGetNextIrpStackLocation(slot->Irp);
	stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	stack->Parameters.DeviceIoControl.IoControlCode =
	        IOCTL_INTERNAL_USB_SUBMIT_URB;
	stack->Parameters.Others.Argument1 = slot->Urb;

	/* The IRP is reused across completions and may carry a stale cancel
	 * from a previous teardown. */
	slot->Irp->Cancel = FALSE;
	slot->Active = TRUE;

	return IoCallDriver(DevExt->LowerDeviceObject, slot->Irp);
}

static NTSTATUS XcPollAllocate(PXC_DEVEXT DevExt)
{
	ULONG i;

	for (i = 0; i < XC_POLL_SLOTS; i++) {
		DevExt->Poll[i].Index = i;
		DevExt->Poll[i].DevExt = DevExt;
		DevExt->Poll[i].Irp =
		        IoAllocateIrp(DevExt->LowerDeviceObject->StackSize, FALSE);
		if (DevExt->Poll[i].Irp == NULL) {
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		DevExt->Poll[i].Urb = (PURB)ExAllocatePoolWithTag(
		        NonPagedPool,
		        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
		        XC_POOL_TAG);
		if (DevExt->Poll[i].Urb == NULL) {
			return STATUS_INSUFFICIENT_RESOURCES;
		}
	}
	return STATUS_SUCCESS;
}

static void XcPollFree(PXC_DEVEXT DevExt)
{
	ULONG i;

	for (i = 0; i < XC_POLL_SLOTS; i++) {
		if (DevExt->Poll[i].Urb != NULL) {
			ExFreePoolWithTag(DevExt->Poll[i].Urb, XC_POOL_TAG);
			DevExt->Poll[i].Urb = NULL;
		}
		if (DevExt->Poll[i].Irp != NULL) {
			IoFreeIrp(DevExt->Poll[i].Irp);
			DevExt->Poll[i].Irp = NULL;
		}
	}
}

#else   /* XBOXCTL_USERMODE */

NTSTATUS XcPollSubmit(PXC_DEVEXT DevExt, ULONG SlotIndex)
{
	if (!XcIoAcquire(DevExt)) {
		DevExt->Poll[SlotIndex].Active = FALSE;
		return STATUS_DELETE_PENDING;
	}
	DevExt->Poll[SlotIndex].Active = TRUE;
	return STATUS_SUCCESS;
}

#endif  /* XBOXCTL_USERMODE */

/*
 * Polling runs only while every stop reason has been cleared. A bitmask
 * rather than a flag because the reasons are independent: a power-down and a
 * pending removal can both be outstanding and whichever clears first must
 * not restart the other's.
 */
NTSTATUS XcPollStart(PXC_DEVEXT DevExt, ULONG Reason)
{
	KIRQL    irql;
	ULONG    i;
	NTSTATUS status = STATUS_SUCCESS;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->PollStopMask &= ~Reason;
	if (DevExt->PollStopMask != 0) {
		KeReleaseSpinLock(&DevExt->PollLock, irql);
		return STATUS_SUCCESS;
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	for (i = 0; i < XC_POLL_SLOTS; i++) {
		if (!DevExt->Poll[i].Active) {
			status = XcPollSubmit(DevExt, i);
			if (!NT_SUCCESS(status) && status != STATUS_PENDING) {
				break;
			}
		}
	}
	return status;
}

void XcPollStop(PXC_DEVEXT DevExt, ULONG Reason)
{
	KIRQL irql;
	ULONG i;

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->PollStopMask |= Reason;
	KeReleaseSpinLock(&DevExt->PollLock, irql);

#ifndef XBOXCTL_USERMODE
	for (i = 0; i < XC_POLL_SLOTS; i++) {
		if (DevExt->Poll[i].Active && DevExt->Poll[i].Irp != NULL) {
			IoCancelIrp(DevExt->Poll[i].Irp);
		}
	}
#else
	/* The driver build drops each transfer's reference from
	 * XcPollComplete when the cancelled transfer comes back. There is no
	 * completion here, so the cancel stands in for it. */
	for (i = 0; i < XC_POLL_SLOTS; i++) {
		if (DevExt->Poll[i].Active) {
			DevExt->Poll[i].Active = FALSE;
			XcIoRelease(DevExt);
		}
	}
#endif
}

/*
 * Teardown, in the one order that is safe: stop asking, wait for what was
 * already asked to come back, and only then free what it completes into.
 */
void XcRemoveDevice(PXC_DEVEXT DevExt)
{
	DevExt->Removed = TRUE;

	XcStopDevice(DevExt);
	XcPollStop(DevExt, XC_STOP_REMOVING);
	XcIoDrainAndWait(DevExt);

#ifndef XBOXCTL_USERMODE
	XcPollFree(DevExt);
#endif
}

/* ======================================================================
 * THE TICK
 * ====================================================================== */

static void NTAPI XcTickDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
	PXC_DEVEXT DevExt = (PXC_DEVEXT)Context;
	KIRQL      irql;

	(void)Dpc;
	(void)Arg1;
	(void)Arg2;

	if (DevExt == NULL || DevExt->Removed) {
		return;
	}

	KeAcquireSpinLock(&DevExt->CoreLock, &irql);
	core_tick(&DevExt->Core, (u64)KeQueryInterruptTime());
	KeReleaseSpinLock(&DevExt->CoreLock, irql);
}

/* ======================================================================
 * THE HID MINIDRIVER IOCTL SURFACE
 *
 * hidclass.sys calls this; user mode never does.
 * ====================================================================== */

NTSTATUS NTAPI XcInternalDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	PXC_DEVEXT         DevExt = XC_GET_DEVEXT(Fdo);
	NTSTATUS           status = STATUS_SUCCESS;
	ULONG              info = 0;

	if (DevExt->Removed) {
		Irp->IoStatus.Status = STATUS_DELETE_PENDING;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return STATUS_DELETE_PENDING;
	}

	switch (stack->Parameters.DeviceIoControl.IoControlCode) {

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
		PHID_DESCRIPTOR hid = (PHID_DESCRIPTOR)Irp->UserBuffer;
		u32             desc_len = 0;

		if (stack->Parameters.DeviceIoControl.OutputBufferLength <
			sizeof(HID_DESCRIPTOR)) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		(void)core_hid_descriptor(&desc_len);

		RtlZeroMemory(hid, sizeof(HID_DESCRIPTOR));
		hid->bLength = (UCHAR)sizeof(HID_DESCRIPTOR);
		hid->bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
		hid->bcdHID = 0x0100;
		hid->bCountry = 0;
		hid->bNumDescriptors = 1;
		hid->DescriptorList[0].bReportType = HID_REPORT_DESCRIPTOR_TYPE;
		hid->DescriptorList[0].wReportLength = (USHORT)desc_len;
		info = sizeof(HID_DESCRIPTOR);
		break;
	}

	case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
		u32       desc_len = 0;
		const u8 *desc = core_hid_descriptor(&desc_len);

		if (stack->Parameters.DeviceIoControl.OutputBufferLength <
			desc_len) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		RtlCopyMemory(Irp->UserBuffer, desc, desc_len);
		info = desc_len;
		break;
	}

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
		PHID_DEVICE_ATTRIBUTES attr =
			    (PHID_DEVICE_ATTRIBUTES)Irp->UserBuffer;

		if (stack->Parameters.DeviceIoControl.OutputBufferLength <
			sizeof(HID_DEVICE_ATTRIBUTES)) {
			status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		RtlZeroMemory(attr, sizeof(HID_DEVICE_ATTRIBUTES));
		attr->Size = sizeof(HID_DEVICE_ATTRIBUTES);
		attr->VendorID = DevExt->DeviceDescriptor.idVendor;
		attr->ProductID = DevExt->DeviceDescriptor.idProduct;
		attr->VersionNumber = DevExt->DeviceDescriptor.bcdDevice;
		info = sizeof(HID_DEVICE_ATTRIBUTES);
		break;
	}

	/*
	 * With DevicesArePolled FALSE this does NOT answer from current
	 * state. It takes a queued report if there is one and otherwise
	 * parks, to be completed when the engine produces something.
	 */
	case IOCTL_HID_READ_REPORT: {
		XC_REPORT_NODE node;

		if (XcDequeueReport(DevExt, &node)) {
			if (stack->Parameters.DeviceIoControl.OutputBufferLength <
				node.Length) {
				status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			XcCompleteRead(Irp, node.Data, node.Length);
			return STATUS_SUCCESS;
		}
		return XcQueueRead(DevExt, Irp);
	}

	/*
	 * Report ID 4 is the rumble actuator pair and report ID 2 is the
	 * keyboard LED state. Both are accepted; neither is acted on yet.
	 * Reporting success on the LED write is what stops Windows deciding
	 * the keyboard is broken.
	 */
	case IOCTL_HID_WRITE_REPORT:
		info = stack->Parameters.DeviceIoControl.InputBufferLength;
		break;

	case IOCTL_HID_SET_FEATURE:
	case IOCTL_HID_GET_FEATURE:
		status = STATUS_NOT_SUPPORTED;
		break;

	/*
	 * hidclass tracks collection state itself and never reads back what
	 * it told us, so acknowledging is the whole job.
	 */
	case IOCTL_HID_ACTIVATE_DEVICE:
	case IOCTL_HID_DEACTIVATE_DEVICE:
		break;

	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = info;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return status;
}

/* ======================================================================
 * DEVICE LIFECYCLE
 * ====================================================================== */

void XcDevExtInit(PXC_DEVEXT DevExt)
{
	RtlZeroMemory(DevExt, sizeof(*DevExt));

	KeInitializeSpinLock(&DevExt->PollLock);
	KeInitializeSpinLock(&DevExt->QueueLock);
	KeInitializeSpinLock(&DevExt->CoreLock);
	InitializeListHead(&DevExt->PendingReads);

	/* One reference, held by the device. XcIoDrainAndWait drops it. */
	DevExt->IoCount = 1;
	DevExt->IoDraining = FALSE;
	KeInitializeEvent(&DevExt->IoIdle, NotificationEvent, FALSE);

	/* Every stop reason starts asserted; StartDevice clears the first. */
	DevExt->PollStopMask = XC_STOP_NOT_STARTED;

	core_init(&DevExt->Core, XcReportSink, DevExt);
}

void XcStopDevice(PXC_DEVEXT DevExt)
{
	KIRQL irql;

	XcPollStop(DevExt, XC_STOP_NOT_STARTED);

	if (DevExt->TickArmed) {
		KeCancelTimer(&DevExt->Tick);
		DevExt->TickArmed = FALSE;
		/*
		 * CANCELLING A TIMER DOES NOT WAIT FOR A DPC ALREADY RUNNING.
		 * Its routine touches the engine, so it has to be known
		 * finished before anything below frees or resets it.
		 */
		KeFlushQueuedDpcs();
	}

	/*
	 * RELEASE EVERYTHING BEFORE THE READS GO. A key held by a binding
	 * that is about to vanish has to be let go while there is still
	 * somewhere for the release report to land.
	 */
	KeAcquireSpinLock(&DevExt->CoreLock, &irql);
	core_release_all(&DevExt->Core);
	KeReleaseSpinLock(&DevExt->CoreLock, irql);

	XcCancelPendingReads(DevExt);
	DevExt->Started = FALSE;
}

#ifndef XBOXCTL_USERMODE

/* Send a URB down and wait for it. Enumeration only; never on the poll path. */
static NTSTATUS XcSendUrb(PXC_DEVEXT DevExt, PURB Urb)
{
	KEVENT             event;
	IO_STATUS_BLOCK    iostatus;
	PIRP               irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS           status;

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
	                                    DevExt->LowerDeviceObject,
	                                    NULL, 0, NULL, 0, TRUE,
	                                    &event, &iostatus);
	if (irp == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	stack = IoGetNextIrpStackLocation(irp);
	stack->Parameters.Others.Argument1 = Urb;

	status = IoCallDriver(DevExt->LowerDeviceObject, irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iostatus.Status;
	}
	return status;
}

NTSTATUS XcStartDevice(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT                    DevExt = XC_GET_DEVEXT(Fdo);
	NTSTATUS                      status;
	PURB                          urb;
	PURB                          select;
	USB_CONFIGURATION_DESCRIPTOR  probe;
	PUSB_CONFIGURATION_DESCRIPTOR config = NULL;
	PUSB_INTERFACE_DESCRIPTOR     iface;
	USBD_INTERFACE_LIST_ENTRY     list[2];
	PUSBD_INTERFACE_INFORMATION   info;
	LARGE_INTEGER                 due;
	ULONG                         i;

	(void)Irp;

	urb = (PURB)ExAllocatePoolWithTag(
	        NonPagedPool, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
	        XC_POOL_TAG);
	if (urb == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	UsbBuildGetDescriptorRequest(
	        urb, (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
	        USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
	        &DevExt->DeviceDescriptor, NULL,
	        sizeof(DevExt->DeviceDescriptor), NULL);
	status = XcSendUrb(DevExt, urb);
	if (!NT_SUCCESS(status)) {
		goto done;
	}

	/* Two passes: the fixed part gives wTotalLength, then the whole thing
	 * with its interface and endpoint descriptors behind it. */
	UsbBuildGetDescriptorRequest(
	        urb, (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
	        USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
	        &probe, NULL, sizeof(probe), NULL);
	status = XcSendUrb(DevExt, urb);
	if (!NT_SUCCESS(status)) {
		goto done;
	}

	config = (PUSB_CONFIGURATION_DESCRIPTOR)ExAllocatePoolWithTag(
	        NonPagedPool, probe.wTotalLength, XC_POOL_TAG);
	if (config == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}

	UsbBuildGetDescriptorRequest(
	        urb, (USHORT)sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
	        USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
	        config, NULL, probe.wTotalLength, NULL);
	status = XcSendUrb(DevExt, urb);
	if (!NT_SUCCESS(status)) {
		goto done;
	}

	iface = USBD_ParseConfigurationDescriptorEx(config, config,
	                                            0, -1, -1, -1, -1);
	if (iface == NULL) {
		status = STATUS_DEVICE_CONFIGURATION_ERROR;
		goto done;
	}

	list[0].InterfaceDescriptor = iface;
	list[0].Interface = NULL;
	list[1].InterfaceDescriptor = NULL;
	list[1].Interface = NULL;

	select = USBD_CreateConfigurationRequestEx(config, list);
	if (select == NULL) {
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}

	status = XcSendUrb(DevExt, select);
	if (NT_SUCCESS(status)) {
		info = list[0].Interface;
		DevExt->ConfigHandle =
		        select->UrbSelectConfiguration.ConfigurationHandle;

		/*
		 * PICK THE PIPES BY DIRECTION, not by index. Taking Pipes[0]
		 * as IN and Pipes[1] as OUT happens to hold on the pads this
		 * driver targets; it is not a property of USB.
		 */
		for (i = 0; i < info->NumberOfPipes; i++) {
			UCHAR addr = info->Pipes[i].EndpointAddress;

			if (info->Pipes[i].PipeType != UsbdPipeTypeInterrupt) {
				continue;
			}
			if (addr & 0x80) {
				DevExt->InPipe = info->Pipes[i].PipeHandle;
				DevExt->PollInterval = info->Pipes[i].Interval;
			} else {
				DevExt->OutPipe = info->Pipes[i].PipeHandle;
				DevExt->HasOutPipe = TRUE;
			}
		}
		if (DevExt->InPipe == NULL) {
			status = STATUS_DEVICE_CONFIGURATION_ERROR;
		}
	}
	ExFreePoolWithTag(select, 0);

done:
	if (config != NULL) {
		ExFreePoolWithTag(config, XC_POOL_TAG);
	}
	ExFreePoolWithTag(urb, XC_POOL_TAG);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = XcPollAllocate(DevExt);
	if (!NT_SUCCESS(status)) {
		XcPollFree(DevExt);
		return status;
	}

	KeInitializeDpc(&DevExt->TickDpc, XcTickDpc, DevExt);
	KeInitializeTimerEx(&DevExt->Tick, NotificationTimer);
	due.QuadPart = -((LONGLONG)XC_TICK_PERIOD_MS * 10000);
	KeSetTimerEx(&DevExt->Tick, due, XC_TICK_PERIOD_MS, &DevExt->TickDpc);
	DevExt->TickArmed = TRUE;

	DevExt->Started = TRUE;
	return XcPollStart(DevExt, XC_STOP_NOT_STARTED);
}

static NTSTATUS XcForwardComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                  PVOID Context)
{
	(void)DeviceObject;
	(void)Irp;
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Send an IRP down and wait for the stack below to finish with it, leaving
 * it intact so the caller can still act on it and complete it itself.
 */
static NTSTATUS XcForwardAndWait(PXC_DEVEXT DevExt, PIRP Irp)
{
	KEVENT   event;
	NTSTATUS status;

	KeInitializeEvent(&event, NotificationEvent, FALSE);

	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(Irp, XcForwardComplete, &event,
	                       TRUE, TRUE, TRUE);

	status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
	if (status == STATUS_PENDING) {
		KeWaitForSingleObject(&event, Executive, KernelMode,
		                      FALSE, NULL);
		status = Irp->IoStatus.Status;
	}
	return status;
}

NTSTATUS NTAPI XcPnp(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT         DevExt = XC_GET_DEVEXT(Fdo);
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS           status;

	switch (stack->MinorFunction) {

	case IRP_MN_START_DEVICE:
		/*
		 * The bus driver has to start the device before its pipes
		 * exist, so this is one of the few IRPs that must go DOWN
		 * first and be acted on afterwards.
		 */
		status = XcForwardAndWait(DevExt, Irp);
		if (NT_SUCCESS(status)) {
			status = XcStartDevice(Fdo, Irp);
		}
		Irp->IoStatus.Status = status;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;

	case IRP_MN_STOP_DEVICE:
	case IRP_MN_SURPRISE_REMOVAL:
		XcStopDevice(DevExt);
		break;

	case IRP_MN_REMOVE_DEVICE:
		XcRemoveDevice(DevExt);
		break;

	case IRP_MN_QUERY_REMOVE_DEVICE:
	case IRP_MN_CANCEL_REMOVE_DEVICE:
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_CANCEL_STOP_DEVICE:
		Irp->IoStatus.Status = STATUS_SUCCESS;
		break;

	default:
		break;
	}

	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS NTAPI XcPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT DevExt = XC_GET_DEVEXT(Fdo);

	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return PoCallDriver(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS NTAPI XcDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT DevExt = XC_GET_DEVEXT(Fdo);

	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DevExt->LowerDeviceObject, Irp);
}

#else   /* XBOXCTL_USERMODE */

/*
 * PnP, power and USB enumeration are pure Windows plumbing with no logic in
 * them, so they are not modelled. The harness starts a device by calling
 * XcStartDevice, which here just releases the poll engine.
 */
NTSTATUS XcStartDevice(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT DevExt = (PXC_DEVEXT)Fdo;
	LARGE_INTEGER due;

	(void)Irp;

	KeInitializeDpc(&DevExt->TickDpc, XcTickDpc, DevExt);
	KeInitializeTimerEx(&DevExt->Tick, NotificationTimer);
	due.QuadPart = -((LONGLONG)XC_TICK_PERIOD_MS * 10000);
	KeSetTimerEx(&DevExt->Tick, due, XC_TICK_PERIOD_MS, &DevExt->TickDpc);
	DevExt->TickArmed = TRUE;

	DevExt->Started = TRUE;
	return XcPollStart(DevExt, XC_STOP_NOT_STARTED);
}

NTSTATUS NTAPI XcPnp(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	(void)Fdo;
	(void)Irp;
	return STATUS_NOT_SUPPORTED;
}

NTSTATUS NTAPI XcPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	(void)Fdo;
	(void)Irp;
	return STATUS_NOT_SUPPORTED;
}

NTSTATUS NTAPI XcDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	(void)Fdo;
	(void)Irp;
	return STATUS_NOT_SUPPORTED;
}

#endif  /* XBOXCTL_USERMODE */

/* ======================================================================
 * ENTRY POINTS
 * ====================================================================== */

NTSTATUS NTAPI XcAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Fdo)
{
	PXC_DEVEXT DevExt = XC_GET_DEVEXT(Fdo);

	(void)DriverObject;

	XcDevExtInit(DevExt);

	DevExt->Fdo = Fdo;
	DevExt->Pdo = XC_GET_PDO(Fdo);
	DevExt->LowerDeviceObject = XC_GET_LOWER(Fdo);

	Fdo->Flags |= DO_POWER_PAGABLE | DO_DIRECT_IO;
	Fdo->Flags &= ~DO_DEVICE_INITIALIZING;

	return STATUS_SUCCESS;
}

NTSTATUS NTAPI XcCreate(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	(void)Fdo;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI XcClose(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	(void)Fdo;
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

VOID NTAPI XcUnload(PDRIVER_OBJECT DriverObject)
{
	(void)DriverObject;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath)
{
	HID_MINIDRIVER_REGISTRATION reg;

	DriverObject->MajorFunction[IRP_MJ_CREATE] = XcCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = XcClose;
	DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] =
	        XcInternalDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = XcDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = XcDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_PNP] = XcPnp;
	DriverObject->MajorFunction[IRP_MJ_POWER] = XcPower;
	DriverObject->DriverUnload = XcUnload;

	/*
	 * WITHOUT THIS NOTHING EVER ENUMERATES. hidclass calls AddDevice
	 * through the driver extension, and a null there means the driver
	 * loads and never sees a device.
	 */
	XcSetAddDevice(DriverObject, XcAddDevice);

	RtlZeroMemory(&reg, sizeof(reg));
	reg.Revision = HID_REVISION;
	reg.DriverObject = DriverObject;
	reg.RegistryPath = RegistryPath;
	reg.DeviceExtensionSize = (ULONG)sizeof(XC_DEVEXT);

	/*
	 * FALSE, and this is the single most consequential line in the driver.
	 *
	 * TRUE would mean hidclass polls us with IOCTL_HID_READ_REPORT and
	 * expects each one answered from current state - the minidriver never
	 * originates anything. A driver that must deliver a keystroke the
	 * instant a button goes down cannot work that way, and neither can one
	 * that emits mouse deltas on its own schedule.
	 *
	 * FALSE is also the ordinary case: hidusb itself works this way.
	 */
	reg.DevicesArePolled = FALSE;

	return HidRegisterMinidriver(&reg);
}
