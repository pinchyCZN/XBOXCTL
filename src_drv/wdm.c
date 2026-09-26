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
/*
 * 8ms, 125 Hz, which is the rate the Adaptoid polls at and the rate
 * its pointer is smooth at.
 *
 * THIS IS THE POINTER'S CLOCK, NOT A HOUSEKEEPING INTERVAL. The pad
 * reports only when something changes - five to ten times a second
 * with a stick held - so the tick is what the stick pipeline
 * integrates on. At 16ms the pointer stepped 62 times a second and
 * looked it.
 */
#define XC_TICK_PERIOD_MS       CORE_TICK_MS

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

/*
 * How much room a pending read offers.
 *
 * HIDCLASS CREATES ONE CHILD DEVICE PER TOP-LEVEL COLLECTION AND SIZES
 * EACH CHILD'S READS TO THAT COLLECTION'S LARGEST REPORT. The mouse
 * child asks for five bytes, the keyboard for eight, the gamepad for
 * thirty-seven. They all arrive here as IOCTL_HID_READ_REPORT on one
 * device and nothing in the IRP says which child sent it.
 */
static ULONG XcReadCapacity(PIRP Irp)
{
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

	return stack->Parameters.DeviceIoControl.OutputBufferLength;
}

static void XcCompleteRead(PIRP Irp, const UCHAR *Data, UCHAR Length)
{
	/*
	 * NEVER WRITE MORE THAN THE CALLER ASKED FOR. The callers already
	 * choose a read that fits; this is the backstop, because the cost
	 * of being wrong here is corrupting another driver's memory rather
	 * than failing an IRP.
	 */
	if (XcReadCapacity(Irp) < Length) {
		Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return;
	}

	if (Irp->UserBuffer != NULL) {
		/*
		 * ZERO THE WHOLE BUFFER FIRST.
		 *
		 * hidclass parks reads whose buffer is sized to the LARGEST
		 * report of any collection - thirty-seven bytes here, the
		 * gamepad's - and reuses that buffer for every report. A
		 * five-byte mouse report therefore leaves thirty-two bytes of
		 * the previous gamepad report in place behind it. Anything
		 * that reads past the length we declare finds stick positions
		 * and button bits where it expects nothing, and acts on them.
		 *
		 * Writing only what we have is correct and not sufficient.
		 */
		RtlZeroMemory(Irp->UserBuffer, XcReadCapacity(Irp));
		RtlCopyMemory(Irp->UserBuffer, Data, Length);
	}
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = Length;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/*
 * Take a pending read that can hold Need bytes.
 *
 * TAKING THE FIRST READ REGARDLESS IS A KERNEL BUFFER OVERFLOW. A
 * 37-byte gamepad report completed into the mouse child's five-byte
 * buffer writes thirty-two bytes past its end, into hidclass's own
 * memory. It does not fault - it corrupts, and the damage surfaces as
 * input nobody generated: phantom keystrokes, wheel events, windows
 * minimising. It looks like a driver computing the wrong values and it
 * is not; it is a driver writing outside the buffer it was given.
 */
static PIRP XcDequeueRead(PXC_DEVEXT DevExt, ULONG Need)
{
	KIRQL       cancel_irql;
	KIRQL       irql;
	PIRP        irp = NULL;
	PLIST_ENTRY entry;

	IoAcquireCancelSpinLock(&cancel_irql);
	KeAcquireSpinLock(&DevExt->QueueLock, &irql);

	for (entry = DevExt->PendingReads.Flink;
	     entry != &DevExt->PendingReads;
	     entry = entry->Flink) {
		PIRP candidate = CONTAINING_RECORD(entry, IRP,
		                                   Tail.Overlay.ListEntry);

		if (XcReadCapacity(candidate) >= Need) {
			RemoveEntryList(entry);
			DevExt->PendingReadCount--;
			IoSetCancelRoutine(candidate, NULL);
			irp = candidate;
			break;
		}
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

	/*
	 * RECORD WHAT THE POINTER PATH PUT ON THE WIRE. This is the only
	 * view of it on a live system: mouhid owns the mouse collection
	 * exclusively, so nothing in user mode can read those reports back.
	 */
	if (report_id < 8) {
		DevExt->ReportsById[report_id]++;
	}

	if (report_id == CORE_REPORT_ID_MOUSE && len > CORE_MS_Y + 1) {
		KIRQL irql;

		KeAcquireSpinLock(&DevExt->QueueLock, &irql);
		DevExt->Trace[DevExt->TraceHead].dx =
		        (s16)(payload[CORE_MS_X] | (payload[CORE_MS_X + 1] << 8));
		DevExt->Trace[DevExt->TraceHead].dy =
		        (s16)(payload[CORE_MS_Y] | (payload[CORE_MS_Y + 1] << 8));
		DevExt->Trace[DevExt->TraceHead].buttons = payload[CORE_MS_BUTTONS];
		DevExt->TraceHead = (DevExt->TraceHead + 1) % XC_TRACE_MAX;
		if (DevExt->TraceCount < XC_TRACE_MAX) {
			DevExt->TraceCount++;
		}
		DevExt->TraceEmitted++;
		KeReleaseSpinLock(&DevExt->QueueLock, irql);
	}

	/*
	 * A READ THAT CANNOT HOLD THIS REPORT IS NOT A READ FOR THIS
	 * REPORT. Queue it instead and let the child that can take it come
	 * and ask - which it will, because hidclass keeps its reads
	 * outstanding.
	 */
	irp = XcDequeueRead(DevExt, len + 1);
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

NTSTATUS XcQueueRead(PXC_DEVEXT DevExt, PIRP Irp)
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
		/* Zero: any read will do, they are all being failed. */
		irp = XcDequeueRead(DevExt, 0);
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

BOOLEAN XcPollAllIdle(PXC_DEVEXT DevExt)
{
	ULONG i;

	for (i = 0; i < XC_POLL_SLOTS; i++) {
		if (DevExt->Poll[i].Active) {
			return FALSE;
		}
	}
	return TRUE;
}

void XcPollFinish(PXC_DEVEXT DevExt, ULONG SlotIndex, NTSTATUS Status)
{
	/*
	 * ONLY A TRANSFER THAT SUCCEEDED IS RESUBMITTED HERE. The bus
	 * driver fails a submit to a departed device SYNCHRONOUSLY, so
	 * resubmitting from the completion routine re-enters it on the same
	 * stack, and with a device that fails every time that recursion
	 * does not stop. A failure is handed to the work item instead.
	 *
	 * RESUBMIT BEFORE RELEASING. The new transfer takes its own
	 * reference while this one still holds its own, so the count never
	 * touches zero while polling is meant to continue - and a drain
	 * that runs in between cannot conclude the device is idle when it
	 * is not.
	 */
	if (!NT_SUCCESS(Status)) {
		DevExt->Poll[SlotIndex].Active = FALSE;
		XcRequestPollRestart(DevExt);
	} else if (DevExt->PollStopMask == 0 && !DevExt->Removed) {
		DevExt->PollRetries = 0;
		XcPollSubmit(DevExt, SlotIndex);
	} else {
		DevExt->Poll[SlotIndex].Active = FALSE;
	}

	XcIoRelease(DevExt);
}

#ifndef XBOXCTL_USERMODE

/*
 * Monotonic 100ns time, at a resolution the poll rate deserves.
 *
 * NOT KeQueryInterruptTime. It reports 100ns units but only advances on
 * the system timer tick - roughly every 15.6ms - so at a 4ms poll rate
 * most packets measure zero elapsed time and every fourth measures the
 * whole tick. Anything integrating over that interval, the pointer
 * accumulator above all, then moves in lumps at the timer rate rather
 * than smoothly at the packet rate: the same velocity, delivered in
 * 44-pixel jumps instead of 11-pixel steps, which is felt as violent.
 *
 * THE CONVERSION IS SPLIT SO IT CANNOT OVERFLOW. Multiplying a raw
 * counter delta by ten million overflows 64 bits after a few weeks of
 * uptime; taking the whole seconds first keeps every intermediate small.
 */
static u64 XcNow100ns(PXC_DEVEXT DevExt)
{
	LARGE_INTEGER now;
	LARGE_INTEGER freq;
	u64           delta;

	now = KeQueryPerformanceCounter(&freq);

	if (DevExt->ClockFreq == 0) {
		if (freq.QuadPart <= 0) {
			/* No usable counter. The coarse clock is wrong for the
			 * pointer but right for everything else, and a driver
			 * that refuses to run is worse than a lumpy one. */
			return (u64)KeQueryInterruptTime();
		}
		DevExt->ClockFreq = (u64)freq.QuadPart;
		DevExt->ClockBase = (u64)now.QuadPart;
	}

	delta = (u64)now.QuadPart - DevExt->ClockBase;

	return (delta / DevExt->ClockFreq) * 10000000u +
	       ((delta % DevExt->ClockFreq) * 10000000u) / DevExt->ClockFreq;
}

#else   /* XBOXCTL_USERMODE */

/*
 * The harness passes the engine its own timestamps, so this exists
 * only to satisfy the shared tick path.
 */
static u64 XcNow100ns(PXC_DEVEXT DevExt)
{
	(void)DevExt;
	return 0;
}

#endif  /* XBOXCTL_USERMODE */

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
	             XcNow100ns(DevExt));

	XcPollFinish(DevExt, slot->Index, status);

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

/*
 * Error recovery, at PASSIVE_LEVEL and off the completion stack.
 *
 * Retries the inactive slots a bounded number of times with a delay
 * between attempts. A device that is genuinely gone fails all of them
 * and polling stops; PnP removal follows and the drain collects what
 * is left. A transient stall recovers on the first or second attempt.
 */
static VOID NTAPI XcPollRestartWorker(PDEVICE_OBJECT DeviceObject,
                                      PVOID Context)
{
	PXC_DEVEXT    DevExt = (PXC_DEVEXT)Context;
	LARGE_INTEGER delay;
	ULONG         attempt;
	ULONG         i;

	(void)DeviceObject;

	for (attempt = 0; attempt < XC_POLL_RETRY_MAX; attempt++) {
		int failed = 0;

		if (DevExt->Removed || DevExt->PollStopMask != 0) {
			break;
		}

		/*
		 * CLEAR THE PIPE FIRST, BUT ONLY WHILE NOTHING IS ON IT. A
		 * stall is the failure a retry alone cannot fix, and resetting
		 * a pipe that still has a transfer outstanding is not a
		 * meaningful request. One slot can fail while the other is
		 * still in flight; that one is about to fail the same way, and
		 * the next attempt through this loop finds both idle.
		 */
		if (XcPollAllIdle(DevExt)) {
			XcResetPipe(DevExt);
		}

		for (i = 0; i < XC_POLL_SLOTS; i++) {
			NTSTATUS st;

			if (DevExt->Poll[i].Active) {
				continue;
			}
			st = XcPollSubmit(DevExt, i);
			if (!NT_SUCCESS(st) && st != STATUS_PENDING) {
				failed = 1;
			}
		}
		if (!failed) {
			break;
		}

		DevExt->PollRetries++;
		delay.QuadPart = -((LONGLONG)XC_POLL_RETRY_MS * 10000);
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
	}

	InterlockedExchange(&DevExt->RestartQueued, 0);
	XcIoRelease(DevExt);
}

void XcRequestPollRestart(PXC_DEVEXT DevExt)
{
	DevExt->PollRestartRequests++;

	if (DevExt->RestartWorkItem == NULL) {
		return;
	}
	/* One outstanding at a time. */
	if (InterlockedCompareExchange(&DevExt->RestartQueued, 1, 0) != 0) {
		return;
	}
	/* A refused reference means teardown has begun and owns the
	 * recovery; the drain must not be left waiting on a worker that
	 * is about to be queued. */
	if (!XcIoAcquire(DevExt)) {
		InterlockedExchange(&DevExt->RestartQueued, 0);
		return;
	}

	IoQueueWorkItem((PIO_WORKITEM)DevExt->RestartWorkItem,
	                XcPollRestartWorker, DelayedWorkQueue, DevExt);
}

static NTSTATUS XcPollAllocate(PXC_DEVEXT DevExt)
{
	ULONG i;

	DevExt->RestartWorkItem = IoAllocateWorkItem(DevExt->Fdo);
	if (DevExt->RestartWorkItem == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	for (i = 0; i < XC_POLL_SLOTS; i++) {
		DevExt->Poll[i].Index = i;
		DevExt->Poll[i].DevExt = DevExt;
		DevExt->Poll[i].Irp =
		        IoAllocateIrp(DevExt->LowerDeviceObject->StackSize, FALSE);
		if (DevExt->Poll[i].Irp == NULL) {
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		DevExt->Poll[i].Urb = (PURB)ExAllocatePoolWithTag(
		        XC_POOL_NX,
		        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
		        XC_POOL_TAG);
		if (DevExt->Poll[i].Urb == NULL) {
			return STATUS_INSUFFICIENT_RESOURCES;
		}
	}

	/*
	 * THE RUMBLE TRANSFER IS NOT FATAL IF IT CANNOT BE ALLOCATED. A pad
	 * that reads and maps but does not shake is a working pad; failing
	 * the start would leave it with no driver at all.
	 */
	DevExt->Rumble.DevExt = DevExt;
	DevExt->Rumble.Irp =
	        IoAllocateIrp(DevExt->LowerDeviceObject->StackSize, FALSE);
	if (DevExt->Rumble.Irp != NULL) {
		DevExt->Rumble.Urb = (PURB)ExAllocatePoolWithTag(
		        XC_POOL_NX,
		        sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
		        XC_POOL_TAG);
	}

	return STATUS_SUCCESS;
}

static void XcPollFree(PXC_DEVEXT DevExt)
{
	ULONG i;

	/* Safe only after the drain: a queued worker holds a reference, so
	 * by the time the count reaches zero none can still be pending. */
	if (DevExt->RestartWorkItem != NULL) {
		IoFreeWorkItem((PIO_WORKITEM)DevExt->RestartWorkItem);
		DevExt->RestartWorkItem = NULL;
	}

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

	if (DevExt->Rumble.Urb != NULL) {
		ExFreePoolWithTag(DevExt->Rumble.Urb, XC_POOL_TAG);
		DevExt->Rumble.Urb = NULL;
	}
	if (DevExt->Rumble.Irp != NULL) {
		IoFreeIrp(DevExt->Rumble.Irp);
		DevExt->Rumble.Irp = NULL;
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

	/*
	 * STATUS_PENDING, BECAUSE THAT IS WHAT IoCallDriver RETURNS for a
	 * transfer it has queued. Returning STATUS_SUCCESS here would make
	 * the harness unable to see the one status that must never reach a
	 * PnP IRP.
	 */
	return STATUS_PENDING;
}

/*
 * The harness has no work item and no second thread, so it records the
 * request and stops. What is worth testing is that the completion
 * routine ASKS rather than resubmitting on its own stack.
 */
void XcRequestPollRestart(PXC_DEVEXT DevExt)
{
	DevExt->PollRestartRequests++;
}

/* No USB stack here; the count is what the tests assert on. */
void XcResetPipe(PXC_DEVEXT DevExt)
{
	DevExt->PipeResets++;
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

	/*
	 * NORMALISE STATUS_PENDING TO SUCCESS. IoCallDriver returns it for
	 * a transfer that was queued and will complete later, which is
	 * precisely what starting to poll means - but this value is
	 * returned all the way out to XcStartDevice and from there into a
	 * PnP IRP's IoStatus.Status, and COMPLETING AN IRP WITH
	 * STATUS_PENDING IS A CONTRACT VIOLATION.
	 *
	 * Driver Verifier bugchecks 0xC9 arg1=6 on it and names the driver.
	 * WITHOUT VERIFIER IT IS WORSE AND SILENT: the PnP manager reads
	 * the start IRP as still outstanding and waits for a completion
	 * that already happened, and because PnP is serialised every
	 * device operation on the machine stops behind it. The symptom is
	 * a guest that freezes the moment the pad is plugged in, with
	 * nothing in the driver looking wrong.
	 *
	 * The status this function owes its caller is whether polling
	 * STARTED, not what the I/O manager thought of one submit.
	 */
	if (status == STATUS_PENDING) {
		status = STATUS_SUCCESS;
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

	/* THE RUMBLE TRANSFER IS COUNTED TOO, so the drain waits on it. A
	 * write left outstanding on a pad being unplugged would hold the
	 * teardown open until the bus gave up on it. */
	if (DevExt->Rumble.Active && DevExt->Rumble.Irp != NULL) {
		IoCancelIrp(DevExt->Rumble.Irp);
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
	if (DevExt->Rumble.Active) {
		DevExt->Rumble.Active = FALSE;
		DevExt->Rumble.Dirty = FALSE;
		XcIoRelease(DevExt);
	}
#endif
}

void XcPowerDown(PXC_DEVEXT DevExt)
{
	KIRQL irql;

	XcPollStop(DevExt, XC_STOP_POWER_DOWN);

	/*
	 * LET GO OF EVERYTHING BEING HELD. See the note in wdm.h: nothing else
	 * will, because the release would have come from a packet and no more
	 * packets are coming.
	 *
	 * NOT A DRAIN. The IRPs are cancelled but not freed and the device
	 * object survives, so there is nothing here that a late completion
	 * could land in. Waiting would also be wrong: a power IRP can arrive
	 * at DISPATCH_LEVEL, where a wait is not allowed.
	 */
	KeAcquireSpinLock(&DevExt->CoreLock, &irql);
	core_release_all(&DevExt->Core);
	KeReleaseSpinLock(&DevExt->CoreLock, irql);
}

void XcPowerUp(PXC_DEVEXT DevExt)
{
	if (DevExt->Removed) {
		return;
	}

	/*
	 * A SLOT WHOSE CANCEL HAS NOT LANDED YET IS STILL MARKED ACTIVE, so
	 * XcPollStart skips it and it does not restart here. That is not a
	 * leak: its completion arrives with STATUS_CANCELLED, which is a
	 * failure, so it asks the recovery worker for a restart and the worker
	 * resubmits it. Polling resumes either way.
	 */
	(void)XcPollStart(DevExt, XC_STOP_POWER_DOWN);
}

/*
 * Teardown, in the one order that is safe: stop asking, wait for what was
 * already asked to come back, and only then free what it completes into.
 */
void XcRemoveDevice(PXC_DEVEXT DevExt)
{
	/*
	 * OUT OF THE REGISTRY BEFORE ANYTHING ELSE, and the unregister
	 * blocks until any command holding the device lock has finished
	 * with this extension. After it returns no new command can find
	 * this device, so the teardown below has the field to itself.
	 */
	XcDeviceUnregister(DevExt);

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
	core_tick(&DevExt->Core, XcNow100ns(DevExt));
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
	/*
	 * THE HID OUTPUT REPORT. hidclass hands it over as a HID_XFER_PACKET
	 * whose reportBuffer begins with the report ID, so two actuator
	 * levels arrive as three bytes.
	 */
	case IOCTL_HID_WRITE_REPORT:
	{
		PHID_XFER_PACKET packet = (PHID_XFER_PACKET)Irp->UserBuffer;

		info = stack->Parameters.DeviceIoControl.InputBufferLength;

		if (packet != NULL && packet->reportBuffer != NULL &&
			packet->reportId == CORE_REPORT_ID_RUMBLE &&
			packet->reportBufferLen >= 3) {
			XcRumbleSet(DevExt, packet->reportBuffer[1],
				        packet->reportBuffer[2]);
		}
		break;
	}

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

void XcResetPipe(PXC_DEVEXT DevExt)
{
	struct _URB_PIPE_REQUEST *urb;

	if (DevExt->InPipe == NULL || DevExt->Removed) {
		return;
	}

	urb = (struct _URB_PIPE_REQUEST *)ExAllocatePoolWithTag(
	        XC_POOL_NX, sizeof(struct _URB_PIPE_REQUEST),
	        XC_POOL_TAG);
	if (urb == NULL) {
		return;
	}

	RtlZeroMemory(urb, sizeof(struct _URB_PIPE_REQUEST));
	urb->Hdr.Length = (USHORT)sizeof(struct _URB_PIPE_REQUEST);
	urb->Hdr.Function = URB_FUNCTION_RESET_PIPE;
	urb->PipeHandle = DevExt->InPipe;

	DevExt->PipeResets++;
	(void)XcSendUrb(DevExt, (PURB)urb);

	/* The result is deliberately ignored. A reset that fails leaves us
	 * exactly where we were - about to retry a submit that will fail -
	 * and the retry count is what ends the attempt either way. */
	ExFreePoolWithTag(urb, XC_POOL_TAG);
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
	        XC_POOL_NX, sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST),
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
	        XC_POOL_NX, probe.wTotalLength, XC_POOL_TAG);
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

	/* Visible to the configurator only now that it can be configured. */
	XcDeviceRegister(DevExt);

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
		/*
		 * A BACKSTOP, NOT THE FIX. XcPollStart already normalises
		 * this; the guard is here because the cost of the status
		 * being STATUS_PENDING is not a failed start but a wedged
		 * machine, and a future caller of XcStartDevice should not be
		 * able to reintroduce that from somewhere else.
		 */
		if (status == STATUS_PENDING) {
			status = STATUS_SUCCESS;
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

static NTSTATUS XcPowerUpComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                  PVOID Context)
{
	PXC_DEVEXT DevExt = (PXC_DEVEXT)Context;

	(void)DeviceObject;

	/* PROPAGATE PENDING. A completion routine that swallows it leaves the
	 * caller believing the IRP finished synchronously. */
	if (Irp->PendingReturned) {
		IoMarkIrpPending(Irp);
	}

	if (NT_SUCCESS(Irp->IoStatus.Status)) {
		XcPowerUp(DevExt);
	}
	return STATUS_SUCCESS;
}

NTSTATUS NTAPI XcPower(PDEVICE_OBJECT Fdo, PIRP Irp)
{
	PXC_DEVEXT         DevExt = XC_GET_DEVEXT(Fdo);
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

	if (stack->MinorFunction == IRP_MN_SET_POWER &&
	    stack->Parameters.Power.Type == DevicePowerState) {

		if (stack->Parameters.Power.State.DeviceState == PowerDeviceD0) {
			/* UP: act on the way back, once the stack below has
			 * powered the hardware. */
			IoCopyCurrentIrpStackLocationToNext(Irp);
			IoSetCompletionRoutine(Irp, XcPowerUpComplete, DevExt,
			                       TRUE, TRUE, TRUE);
			PoStartNextPowerIrp(Irp);
			return PoCallDriver(DevExt->LowerDeviceObject, Irp);
		}

		/* DOWN, to any of D1, D2 or D3: stop touching the hardware and
		 * let go of what is held, before the bus removes power. */
		XcPowerDown(DevExt);
	}

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

	/* The real start finds this by walking the pipe list; a pad without
	 * an OUT endpoint simply does not rumble. */
	DevExt->HasOutPipe = TRUE;

	/* Visible to the configurator only now that it can be configured. */
	XcDeviceRegister(DevExt);

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

/* ======================================================================
 * RUMBLE
 *
 * wdm.h gives the packet. The state machine here exists because rumble is
 * a level rather than a message: a request arriving while a transfer is in
 * flight must replace the pending one, not queue behind it.
 * ====================================================================== */

#ifndef XBOXCTL_USERMODE

static NTSTATUS XcRumbleSubmit(PXC_DEVEXT DevExt);

static NTSTATUS NTAPI XcRumbleComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp,
                                       PVOID Context)
{
	PXC_DEVEXT DevExt = (PXC_DEVEXT)Context;
	KIRQL      irql;
	BOOLEAN    again = FALSE;

	(void)DeviceObject;

	if (!NT_SUCCESS(Irp->IoStatus.Status)) {
		DevExt->Rumble.Errors++;
	}

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->Rumble.Active = FALSE;
	if (DevExt->Rumble.Dirty && !DevExt->Removed) {
		DevExt->Rumble.Dirty = FALSE;
		again = TRUE;
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	/*
	 * RESUBMIT AFTER RELEASING THE REFERENCE, not before. Submitting
	 * while this transfer's reference is still held would let the count
	 * never reach zero under a caller that keeps changing the level, and
	 * teardown waits on that count.
	 */
	XcIoRelease(DevExt);

	if (again) {
		(void)XcRumbleSubmit(DevExt);
	}

	/* The IRP is ours and is reused; the I/O manager must not touch it. */
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS XcRumbleSubmit(PXC_DEVEXT DevExt)
{
	XC_RUMBLE         *r = &DevExt->Rumble;
	PIO_STACK_LOCATION stack;
	KIRQL              irql;

	if (r->Irp == NULL || r->Urb == NULL || !DevExt->HasOutPipe) {
		return STATUS_NOT_SUPPORTED;
	}

	if (!XcIoAcquire(DevExt)) {
		return STATUS_DELETE_PENDING;
	}

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	r->Active    = TRUE;
	r->SentLeft  = r->Left;
	r->SentRight = r->Right;
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	r->Buffer[0] = 0x00;
	r->Buffer[1] = XC_RUMBLE_PACKET_BYTES;
	r->Buffer[2] = 0x00;
	r->Buffer[3] = r->SentLeft;
	r->Buffer[4] = 0x00;
	r->Buffer[5] = r->SentRight;

	UsbBuildInterruptOrBulkTransferRequest(
	        r->Urb,
	        (USHORT)sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER),
	        DevExt->OutPipe,
	        r->Buffer,
	        NULL,
	        XC_RUMBLE_PACKET_BYTES,
	        USBD_TRANSFER_DIRECTION_OUT | USBD_SHORT_TRANSFER_OK,
	        NULL);

	IoSetCompletionRoutine(r->Irp, XcRumbleComplete, DevExt, TRUE, TRUE, TRUE);

	stack = IoGetNextIrpStackLocation(r->Irp);
	stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	stack->Parameters.DeviceIoControl.IoControlCode =
	        IOCTL_INTERNAL_USB_SUBMIT_URB;
	stack->Parameters.Others.Argument1 = r->Urb;

	r->Irp->Cancel = FALSE;
	r->Sent++;

	return IoCallDriver(DevExt->LowerDeviceObject, r->Irp);
}

#else   /* XBOXCTL_USERMODE */

/*
 * The harness has no bus to submit to, so the transfer completes at once.
 * What it does model is the part worth testing: whether a level that
 * arrives mid-transfer reaches the pad afterwards, or is lost.
 */
static NTSTATUS XcRumbleSubmit(PXC_DEVEXT DevExt)
{
	XC_RUMBLE *r = &DevExt->Rumble;

	if (!DevExt->HasOutPipe) {
		return STATUS_NOT_SUPPORTED;
	}
	if (!XcIoAcquire(DevExt)) {
		return STATUS_DELETE_PENDING;
	}

	r->Active    = TRUE;
	r->SentLeft  = r->Left;
	r->SentRight = r->Right;

	r->Buffer[0] = 0x00;
	r->Buffer[1] = XC_RUMBLE_PACKET_BYTES;
	r->Buffer[2] = 0x00;
	r->Buffer[3] = r->SentLeft;
	r->Buffer[4] = 0x00;
	r->Buffer[5] = r->SentRight;
	r->Sent++;

	return STATUS_SUCCESS;
}

/* Stands in for the completion the bus would deliver. */
void XcRumbleCompleteForTest(PXC_DEVEXT DevExt)
{
	BOOLEAN again;

	if (!DevExt->Rumble.Active) {
		return;
	}
	DevExt->Rumble.Active = FALSE;
	again = DevExt->Rumble.Dirty;
	DevExt->Rumble.Dirty = FALSE;

	XcIoRelease(DevExt);

	if (again && !DevExt->Removed) {
		(void)XcRumbleSubmit(DevExt);
	}
}

#endif  /* XBOXCTL_USERMODE */

void XcRumbleSet(PXC_DEVEXT DevExt, u8 Left, u8 Right)
{
	BOOLEAN submit = FALSE;
	KIRQL   irql;

	if (DevExt->Removed || !DevExt->HasOutPipe) {
		return;
	}

	KeAcquireSpinLock(&DevExt->PollLock, &irql);
	DevExt->Rumble.Left  = Left;
	DevExt->Rumble.Right = Right;

	if (DevExt->Rumble.Active) {
		/* THE NEWEST LEVEL WINS. Dirty is a flag and not a queue on
		 * purpose; the values it refers to have already been
		 * overwritten above. */
		DevExt->Rumble.Dirty = TRUE;
	} else if (Left != DevExt->Rumble.SentLeft ||
	           Right != DevExt->Rumble.SentRight ||
	           DevExt->Rumble.Sent == 0) {
		submit = TRUE;
	}
	KeReleaseSpinLock(&DevExt->PollLock, irql);

	if (submit) {
		(void)XcRumbleSubmit(DevExt);
	}
}

/* ======================================================================
 * THE CONTROL DEVICE
 *
 * ../docs/driver-plan.txt section 7. One control device for the whole
 * driver, a registry of the pads behind it, and six commands.
 * ====================================================================== */

static PXC_DEVEXT      g_Devices[XC_MAX_DEVICES];
static FAST_MUTEX      g_DeviceLock;
static BOOLEAN         g_DeviceLockReady;
static PDRIVER_OBJECT  g_DriverObject;

static NTSTATUS XcControlDeviceCreate(void);
static void     XcControlDeviceDelete(void);

/* Caller holds the lock. */
static ULONG XcDeviceCount(void)
{
	ULONG i, n = 0;

	for (i = 0; i < XC_MAX_DEVICES; i++) {
		if (g_Devices[i] != NULL) {
			n++;
		}
	}
	return n;
}

/*
 * A FAST MUTEX, NOT A SPIN LOCK, and the reason is what runs underneath it.
 * A command may install a configuration, which releases every asserted
 * output and emits the reports that implies - work that takes other locks
 * and is not welcome at DISPATCH_LEVEL. Both sides of this lock, a
 * DeviceIoControl and a PnP remove, arrive at PASSIVE_LEVEL, so a mutex is
 * both sufficient and correct.
 */
static void XcSetDriverObject(PDRIVER_OBJECT DriverObject)
{
	g_DriverObject = DriverObject;
}

static void XcDeviceLockInit(void)
{
	if (!g_DeviceLockReady) {
		ExInitializeFastMutex(&g_DeviceLock);
		g_DeviceLockReady = TRUE;
	}
}

void XcDeviceRegistryReset(void)
{
	ULONG i;

	for (i = 0; i < XC_MAX_DEVICES; i++) {
		g_Devices[i] = NULL;
	}
	g_DeviceLockReady = FALSE;
	XcDeviceLockInit();
}

/*
 * THE CONTROL DEVICE LIVES EXACTLY AS LONG AS A PAD DOES, and that is not
 * a detail. A driver object that owns a device object is never unloaded,
 * so a control device created at DriverEntry pins the image in memory for
 * the life of the boot - and then installing a new build does nothing,
 * however loudly the installer reports success, because the kernel goes
 * on running the old one until a reboot.
 *
 * Created on the first arrival and deleted with the last removal, the
 * driver unloads when the pad is unplugged and the next deploy maps the
 * new image. The Adaptoid reference counts its control device the same
 * way - ADAPTOID/docs/ioctl-surface.txt section 1.2.
 *
 * The cost is that \\.\xboxctl does not exist while no pad is plugged
 * in. There is nothing to configure then anyway.
 */
void XcDeviceRegister(PXC_DEVEXT DevExt)
{
	ULONG i;
	int   first = 0;

	XcDeviceLockInit();
	ExAcquireFastMutex(&g_DeviceLock);

	for (i = 0; i < XC_MAX_DEVICES; i++) {
		if (g_Devices[i] == DevExt) {
			break;              /* already in, do not double-add */
		}
		if (g_Devices[i] == NULL) {
			g_Devices[i] = DevExt;
			first = (XcDeviceCount() == 1);
			break;
		}
	}

	ExReleaseFastMutex(&g_DeviceLock);

	/*
	 * OUTSIDE THE LOCK, AND THAT IS NOT TIDINESS. ExAcquireFastMutex
	 * raises IRQL to APC_LEVEL; IoCreateDevice and IoCreateSymbolicLink
	 * both require PASSIVE_LEVEL. Creating the control device while
	 * holding this mutex is an IRQL violation that Driver Verifier
	 * bugchecks on the spot - and because the pad is started during
	 * boot, that is a bugcheck on every boot and a machine that will
	 * not come up until the pad is unplugged.
	 *
	 * Only the caller that took the count from nothing to one creates,
	 * so two pads arriving together still produce one control device.
	 */
	if (first) {
		(void)XcControlDeviceCreate();
	}
}

/*
 * UNREGISTER TAKES THE SAME LOCK A COMMAND HOLDS, which is the whole point
 * of it being a lock rather than an array. A remove that arrives while a
 * command is configuring that device waits for the command to finish
 * instead of pulling the extension out from under it.
 */
void XcDeviceUnregister(PXC_DEVEXT DevExt)
{
	ULONG i;
	int   last;

	XcDeviceLockInit();
	ExAcquireFastMutex(&g_DeviceLock);

	for (i = 0; i < XC_MAX_DEVICES; i++) {
		if (g_Devices[i] == DevExt) {
			g_Devices[i] = NULL;
		}
	}
	last = (XcDeviceCount() == 0);

	ExReleaseFastMutex(&g_DeviceLock);

	/* PASSIVE_LEVEL only, for the reason given in XcDeviceRegister. */
	if (last) {
		XcControlDeviceDelete();
	}
}

/* Caller holds the lock. */
static PXC_DEVEXT XcDeviceAt(ULONG index)
{
	if (index >= XC_MAX_DEVICES) {
		return NULL;
	}
	return g_Devices[index];
}

static NTSTATUS XcCmdGetVersion(void *buffer, ULONG out_len, ULONG *written)
{
	XC_VERSION_INFO info;

	if (out_len < sizeof(info)) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(&info, sizeof(info));
	info.signature      = XC_CONFIG_SIGNATURE;
	info.driver_major   = XC_VERSION_MAJOR;
	info.driver_minor   = XC_VERSION_MINOR;
	info.driver_patch   = XC_VERSION_PATCH;
	info.config_version = CORE_CFG_VERSION;
	info.max_layouts    = CORE_MAX_LAYOUTS;
	info.max_bindings   = CORE_MAX_BINDINGS;
	info.max_chords     = CORE_MAX_CHORDS;
	info.blob_bytes     = (u16)(sizeof(core_config_header) +
	                            sizeof(core_stick) * CORE_STICK_COUNT +
	                            sizeof(core_layout) * CORE_MAX_LAYOUTS);

	RtlCopyMemory(buffer, &info, sizeof(info));
	*written = (ULONG)sizeof(info);
	return STATUS_SUCCESS;
}

static NTSTATUS XcCmdGetDevices(void *buffer, ULONG out_len, ULONG *written)
{
	XC_DEVICE_LIST list;
	ULONG          i;

	if (out_len < sizeof(list)) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(&list, sizeof(list));
	for (i = 0; i < XC_MAX_DEVICES; i++) {
		PXC_DEVEXT dev = g_Devices[i];

		if (dev == NULL) {
			continue;
		}
		list.device[list.count].index      = i;
		list.device[list.count].vendor_id  =
		        dev->DeviceDescriptor.idVendor;
		list.device[list.count].product_id =
		        dev->DeviceDescriptor.idProduct;
		list.device[list.count].started    = (u8)(dev->Started ? 1 : 0);
		list.count++;
	}

	RtlCopyMemory(buffer, &list, sizeof(list));
	*written = (ULONG)sizeof(list);
	return STATUS_SUCCESS;
}

static NTSTATUS XcCmdGetTrace(PXC_DEVEXT dev, void *buffer,
                              ULONG out_len, ULONG *written)
{
	static XC_TRACE trace;
	KIRQL           irql;
	ULONG           i, at;

	if (out_len < sizeof(trace)) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(&trace, sizeof(trace));

	KeAcquireSpinLock(&dev->QueueLock, &irql);
	trace.count   = dev->TraceCount;
	trace.emitted = dev->TraceEmitted;

	/* Oldest first, so the reader sees them in the order they went out. */
	at = (dev->TraceHead + XC_TRACE_MAX - dev->TraceCount) % XC_TRACE_MAX;
	for (i = 0; i < dev->TraceCount; i++) {
		trace.entry[i] = dev->Trace[(at + i) % XC_TRACE_MAX];
	}
	KeReleaseSpinLock(&dev->QueueLock, irql);

	RtlCopyMemory(buffer, &trace, sizeof(trace));
	*written = (ULONG)sizeof(trace);
	return STATUS_SUCCESS;
}

static NTSTATUS XcCmdGetStats(PXC_DEVEXT dev, ULONG index, void *buffer,
                              ULONG out_len, ULONG *written)
{
	XC_STATS stats;

	if (out_len < sizeof(stats)) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(&stats, sizeof(stats));
	stats.index            = index;
	stats.packets_accepted = dev->Core.packets_accepted;
	stats.packets_rejected = dev->Core.packets_rejected;
	stats.reports_emitted  = dev->Core.reports_emitted;
	stats.reports_dropped  = dev->ReportsDropped;
	stats.poll_errors      = dev->PollErrors;
	stats.pipe_resets      = dev->PipeResets;
	stats.poll_restarts    = dev->PollRestartRequests;
	stats.pending_reads    = dev->PendingReadCount;
	stats.layer            = (u32)dev->Core.layout + 1;
	stats.rumble_sent      = dev->Rumble.Sent;
	stats.rumble_errors    = dev->Rumble.Errors;
	RtlCopyMemory(stats.by_id, dev->ReportsById, sizeof(stats.by_id));

	RtlCopyMemory(buffer, &stats, sizeof(stats));
	*written = (ULONG)sizeof(stats);
	return STATUS_SUCCESS;
}

/*
 * The last packet, untranslated.
 *
 * TAKEN UNDER THE CORE LOCK because the poll completion writes it at
 * DISPATCH_LEVEL, and a twenty-byte copy torn across an update would
 * read as the pad doing something it never did.
 */
static NTSTATUS XcCmdGetRaw(PXC_DEVEXT dev, ULONG index, void *buffer,
                            ULONG out_len, ULONG *written)
{
	XC_RAW_INFO info;
	KIRQL       irql;

	if (out_len < sizeof(info)) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	RtlZeroMemory(&info, sizeof(info));
	info.index = index;

	KeAcquireSpinLock(&dev->CoreLock, &irql);
	info.valid      = (u32)(dev->Core.raw_valid ? 1 : 0);
	info.sequence   = dev->Core.packets_accepted;
	info.layer      = (u32)dev->Core.layout + 1;
	info.when_100ns = dev->Core.last_packet_100ns;
	RtlCopyMemory(info.packet, dev->Core.raw, CORE_RAW_PACKET_BYTES);
	KeReleaseSpinLock(&dev->CoreLock, irql);

	RtlCopyMemory(buffer, &info, sizeof(info));
	*written = (ULONG)sizeof(info);
	return STATUS_SUCCESS;
}

static NTSTATUS XcCmdGetConfig(PXC_DEVEXT dev, void *buffer, ULONG out_len,
                               ULONG *written)
{
	u32 len;

	len = core_config_save(&dev->Core.cfg, (u8 *)buffer, out_len);
	if (len == 0) {
		return STATUS_BUFFER_TOO_SMALL;
	}
	*written = len;
	return STATUS_SUCCESS;
}

/*
 * Map a parse failure onto something a caller can act on. A malformed blob
 * is the caller's fault and says so; anything else would have the
 * configurator retrying a push that can never succeed.
 */
static NTSTATUS XcConfigStatus(int rc)
{
	switch (rc) {
	case CORE_CFG_OK:
		return STATUS_SUCCESS;
	case CORE_CFG_ERR_SHORT:
	case CORE_CFG_ERR_TRUNCATED:
		return STATUS_BUFFER_TOO_SMALL;
	case CORE_CFG_ERR_VERSION:
		return STATUS_REVISION_MISMATCH;
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

NTSTATUS XcControlCommand(ULONG code, void *buffer, ULONG in_len,
                          ULONG out_len, ULONG *written)
{
	XC_CONFIG_REQUEST request;
	static u8         blob[XC_CONFIG_BLOB_MAX];
	PXC_DEVEXT        dev;
	NTSTATUS          status;
	ULONG             blob_len;
	u32               repaired;
	int               rc;

	*written = 0;
	if (buffer == NULL) {
		return STATUS_INVALID_PARAMETER;
	}

	XcDeviceLockInit();
	ExAcquireFastMutex(&g_DeviceLock);

	switch (code) {
	case IOCTL_XC_GET_VERSION:
		status = XcCmdGetVersion(buffer, out_len, written);
		break;

	case IOCTL_XC_GET_DEVICES:
		status = XcCmdGetDevices(buffer, out_len, written);
		break;

	case IOCTL_XC_GET_CONFIG:
	case IOCTL_XC_GET_STATS:
	case IOCTL_XC_GET_TRACE:
	case IOCTL_XC_RESET_CONFIG:
	case IOCTL_XC_SET_CONFIG:
	case IOCTL_XC_SET_RUMBLE:
	case IOCTL_XC_GET_RAW:
		if (in_len < sizeof(request)) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		/*
		 * READ THE INPUT OUT BEFORE WRITING A REPLY. Buffered I/O
		 * hands over one buffer for both directions, so the first
		 * byte of output destroys the request that asked for it.
		 */
		RtlCopyMemory(&request, buffer, sizeof(request));

		dev = XcDeviceAt(request.index);
		if (dev == NULL) {
			status = STATUS_DEVICE_DOES_NOT_EXIST;
			break;
		}

		if (code == IOCTL_XC_GET_TRACE) {
			status = XcCmdGetTrace(dev, buffer, out_len, written);
			break;
		}
		if (code == IOCTL_XC_GET_STATS) {
			status = XcCmdGetStats(dev, request.index, buffer,
			                       out_len, written);
			break;
		}
		if (code == IOCTL_XC_GET_CONFIG) {
			status = XcCmdGetConfig(dev, buffer, out_len,
			                        written);
			break;
		}
		if (code == IOCTL_XC_RESET_CONFIG) {
			core_set_config_default(&dev->Core);
			status = STATUS_SUCCESS;
			break;
		}
		if (code == IOCTL_XC_GET_RAW) {
			status = XcCmdGetRaw(dev, request.index, buffer,
			                     out_len, written);
			break;
		}
		if (code == IOCTL_XC_SET_RUMBLE) {
			XC_RUMBLE_REQUEST rr;

			if (in_len < sizeof(rr)) {
				status = STATUS_INVALID_PARAMETER;
				break;
			}
			RtlCopyMemory(&rr, buffer, sizeof(rr));
			if (!dev->HasOutPipe) {
				/* The pad has no OUT endpoint, which some third
				 * party pads genuinely do not. */
				status = STATUS_NOT_SUPPORTED;
				break;
			}
			XcRumbleSet(dev, rr.left, rr.right);
			status = STATUS_SUCCESS;
			break;
		}

		/* SET_CONFIG: the blob follows the request header. */
		blob_len = in_len - (ULONG)sizeof(request);
		if (blob_len == 0 || blob_len > XC_CONFIG_BLOB_MAX) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		/*
		 * COPY IT OUT OF THE SHARED BUFFER FIRST. The parse writes
		 * nothing into it, but core_set_config releases outputs and
		 * emits reports part way through, and nothing should be
		 * reading a caller-shared buffer across that.
		 */
		RtlCopyMemory(blob, (u8 *)buffer + sizeof(request), blob_len);

		repaired = 0;
		rc = core_set_config(&dev->Core, blob, blob_len, &repaired);
		status = XcConfigStatus(rc);
		break;

	default:
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}

	ExReleaseFastMutex(&g_DeviceLock);
	return status;
}

/* ----------------------------------------------------------------------
 * The device object, and getting IRPs to it.
 *
 * HidRegisterMinidriver OVERWRITES THE DISPATCH TABLE. One driver object
 * serves the control device and every HID device, so the entry points set
 * before registration are gone by the time it returns. The only way to see
 * an IRP for our own device is to save what hidclass installed and wrap it.
 * ---------------------------------------------------------------------- */

#ifndef XBOXCTL_USERMODE

static PDEVICE_OBJECT    g_ControlDevice;
static PDRIVER_DISPATCH  g_HidCreate;
static PDRIVER_DISPATCH  g_HidClose;
static PDRIVER_DISPATCH  g_HidDeviceControl;
static PDRIVER_UNLOAD    g_HidUnload;

static const WCHAR XC_CONTROL_NAME[] = L"\\Device\\xboxctl";
static const WCHAR XC_CONTROL_LINK[] = L"\\DosDevices\\xboxctl";

/*
 * IS THIS OURS? A pointer comparison, and deliberately not the Adaptoid's
 * trick of claiming opens of the HID path with a "\q" suffix - that works
 * only because it sees IRP_MJ_CREATE before hidclass and tests a single
 * character of a name it never fully checks.
 */
static BOOLEAN XcIsControlDevice(PDEVICE_OBJECT DeviceObject)
{
	return (BOOLEAN)(g_ControlDevice != NULL &&
	                 DeviceObject == g_ControlDevice);
}

static NTSTATUS XcCompleteControl(PIRP Irp, NTSTATUS Status, ULONG Written)
{
	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = Written;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}

static NTSTATUS NTAPI XcCreateWrapper(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (XcIsControlDevice(DeviceObject)) {
		return XcCompleteControl(Irp, STATUS_SUCCESS, 0);
	}
	if (g_HidCreate != NULL) {
		return g_HidCreate(DeviceObject, Irp);
	}
	return XcCompleteControl(Irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI XcCloseWrapper(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	if (XcIsControlDevice(DeviceObject)) {
		return XcCompleteControl(Irp, STATUS_SUCCESS, 0);
	}
	if (g_HidClose != NULL) {
		return g_HidClose(DeviceObject, Irp);
	}
	return XcCompleteControl(Irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS NTAPI XcDeviceControlWrapper(PDEVICE_OBJECT DeviceObject,
                                             PIRP Irp)
{
	PIO_STACK_LOCATION stack;
	NTSTATUS           status;
	ULONG              written = 0;

	if (!XcIsControlDevice(DeviceObject)) {
		if (g_HidDeviceControl != NULL) {
			return g_HidDeviceControl(DeviceObject, Irp);
		}
		return XcCompleteControl(Irp, STATUS_NOT_SUPPORTED, 0);
	}

	stack = IoGetCurrentIrpStackLocation(Irp);

	/*
	 * METHOD_BUFFERED, so AssociatedIrp.SystemBuffer is a kernel copy and
	 * no user-mode address is ever touched. It is also ONE buffer for both
	 * directions, which the command handler is written to expect.
	 */
	status = XcControlCommand(
	        stack->Parameters.DeviceIoControl.IoControlCode,
	        Irp->AssociatedIrp.SystemBuffer,
	        stack->Parameters.DeviceIoControl.InputBufferLength,
	        stack->Parameters.DeviceIoControl.OutputBufferLength,
	        &written);

	return XcCompleteControl(Irp, status, written);
}

static NTSTATUS XcControlDeviceCreate(void)
{
	UNICODE_STRING name;
	UNICODE_STRING link;
	NTSTATUS       status;

	if (g_ControlDevice != NULL || g_DriverObject == NULL) {
		return STATUS_SUCCESS;
	}

	RtlInitUnicodeString(&name, XC_CONTROL_NAME);
	RtlInitUnicodeString(&link, XC_CONTROL_LINK);

	/*
	 * NO EXTENSION AND NO EXCLUSIVITY. There is nothing per-handle to
	 * remember, and two tools looking at the same driver is a reasonable
	 * thing to want.
	 */
	status = IoCreateDevice(g_DriverObject, 0, &name, XC_DEVICE_TYPE,
	                        FILE_DEVICE_SECURE_OPEN, FALSE,
	                        &g_ControlDevice);
	if (!NT_SUCCESS(status)) {
		g_ControlDevice = NULL;
		return status;
	}

	status = IoCreateSymbolicLink(&link, &name);
	if (!NT_SUCCESS(status)) {
		IoDeleteDevice(g_ControlDevice);
		g_ControlDevice = NULL;
		return status;
	}

	g_ControlDevice->Flags |= DO_BUFFERED_IO;
	g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

static void XcControlDeviceDelete(void)
{
	UNICODE_STRING link;

	if (g_ControlDevice == NULL) {
		return;
	}
	RtlInitUnicodeString(&link, XC_CONTROL_LINK);
	IoDeleteSymbolicLink(&link);
	IoDeleteDevice(g_ControlDevice);
	g_ControlDevice = NULL;
}

/*
 * Called after HidRegisterMinidriver has had its way with the table.
 */
static void XcInstallWrappers(PDRIVER_OBJECT DriverObject)
{
	g_HidCreate        = DriverObject->MajorFunction[IRP_MJ_CREATE];
	g_HidClose         = DriverObject->MajorFunction[IRP_MJ_CLOSE];
	g_HidDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
	g_HidUnload        = DriverObject->DriverUnload;

	DriverObject->MajorFunction[IRP_MJ_CREATE]  = XcCreateWrapper;
	DriverObject->MajorFunction[IRP_MJ_CLOSE]   = XcCloseWrapper;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
	        XcDeviceControlWrapper;
	DriverObject->DriverUnload = XcUnload;
}

#else   /* XBOXCTL_USERMODE */

/*
 * The harness drives XcControlCommand directly; there is no device object
 * and no dispatch table to wrap. The COUNT is kept, though, because the
 * lifetime is the thing worth testing: a control device that outlives the
 * last pad pins the driver in memory and the next install silently does
 * nothing.
 */
/*
 * AND WHETHER IT WAS EVER DONE UNDER THE LOCK. ExAcquireFastMutex raises
 * IRQL to APC_LEVEL and IoCreateDevice demands PASSIVE_LEVEL, so doing
 * this inside the device lock is an IRQL violation - one that bugchecks
 * during boot, where the pad is started, and leaves a machine that will
 * not come up. The harness cannot raise IRQL, but it can count.
 */
LONG g_ControlDeviceAlive;
LONG g_ControlDeviceUnderLock;
extern long g_MutexDepth;

static NTSTATUS XcControlDeviceCreate(void)
{
	if (g_MutexDepth != 0) {
		g_ControlDeviceUnderLock = 1;
	}
	g_ControlDeviceAlive = 1;
	return STATUS_SUCCESS;
}

static void XcControlDeviceDelete(void)
{
	if (g_MutexDepth != 0) {
		g_ControlDeviceUnderLock = 1;
	}
	g_ControlDeviceAlive = 0;
}

static void XcInstallWrappers(PDRIVER_OBJECT DriverObject)
{
	(void)DriverObject;
}

#endif  /* XBOXCTL_USERMODE */

VOID NTAPI XcUnload(PDRIVER_OBJECT DriverObject)
{
	XcControlDeviceDelete();

#ifndef XBOXCTL_USERMODE
	/*
	 * CHAIN, DO NOT REPLACE. hidclass installed its own unload routine
	 * and still has devices and allocations to let go of; dropping it
	 * leaks all of them.
	 */
	if (g_HidUnload != NULL) {
		g_HidUnload(DriverObject);
	}
#else
	(void)DriverObject;
#endif
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath)
{
	HID_MINIDRIVER_REGISTRATION reg;
	NTSTATUS                    status;

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

	status = HidRegisterMinidriver(&reg);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	/*
	 * ORDER MATTERS AND THERE IS ONLY ONE THAT WORKS. The registration
	 * above replaces every entry point set before it, so the wrappers go
	 * on afterwards; and the control device is created only once they
	 * are in place, because an open that arrives between the two would
	 * reach hidclass with a device object it has never heard of.
	 */
	XcDeviceRegistryReset();
	XcInstallWrappers(DriverObject);

	/*
	 * THE CONTROL DEVICE IS NOT CREATED HERE. It appears with the first
	 * pad and goes with the last, so the driver can unload - see the
	 * note above XcDeviceRegister.
	 */
	XcSetDriverObject(DriverObject);

	return STATUS_SUCCESS;
}
