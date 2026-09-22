/*
 * wdm.h - the OS-facing layer of xboxctl.
 *
 * This is the half of the driver that is ABOUT Windows: entry points, IRPs,
 * PnP, power, URBs and the HID minidriver contract. It is the only place that
 * may name a kernel type.
 *
 * It compiles twice. In the driver build it sees the real DDK headers. In the
 * harness build it sees kstub.h instead, and harness.c supplies the bodies of
 * the kernel routines it calls.
 */
#ifndef XBOXCTL_WDM_H
#define XBOXCTL_WDM_H

#ifdef XBOXCTL_USERMODE
#include "kstub.h"
#else
#include <ntddk.h>
#include <hidport.h>
/* The USB layer. usbdi.h must come before usbdlib.h, and usbioctl.h brings
 * the IOCTL_INTERNAL_USB_* codes. */
#include <usbdi.h>
#include <usbdlib.h>
#include <usbioctl.h>
#endif

#include "core.h"

/* ======================================================================
 * IDENTITY
 * ====================================================================== */

#define XC_POOL_TAG             'ctbX'      /* "Xbtc" reading backwards */

#define XC_VERSION_MAJOR        0
#define XC_VERSION_MINOR        1
#define XC_VERSION_PATCH        0

/* Guards the configuration feature report. Not a security boundary -
 * nothing on this interface authenticates a caller - but it is a cheap
 * guard against a malformed or foreign write. */
#define XC_CONFIG_SIGNATURE     0x4C435458ul    /* "XTCL" little endian */

/* ======================================================================
 * THE POLL ENGINE
 *
 * TWO SLOTS, BOTH ALWAYS SUBMITTED. An interrupt endpoint is in the host
 * controller's periodic schedule only while a transfer is queued on it, so
 * a single-IRP design leaves the endpoint unqueued for the whole window
 * between completion and resubmit - and that window is where this driver
 * does its work. With two, the second is already armed when the first
 * completes and the processing overlaps the next transfer.
 *
 * Polling runs whenever the device is started. There is no keep-alive and
 * no dependency on anybody reading: a driver whose job is to emit
 * keystrokes has no reader to keep it alive, because kbdclass does not
 * poll, it waits.
 * ====================================================================== */

#define XC_POLL_SLOTS           2

typedef struct _XC_POLL_SLOT {
	PIRP        Irp;
	PURB        Urb;
	u8          Buffer[CORE_RAW_PACKET_BYTES];
	BOOLEAN     Active;
	ULONG       Index;
	PVOID       DevExt;         /* back pointer, for the completion */
} XC_POLL_SLOT;

/* Bits of DevExt->PollStopMask. Polling runs only while the mask is zero. */
#define XC_STOP_NOT_STARTED     0x01
#define XC_STOP_REMOVING        0x02
#define XC_STOP_POWER_DOWN      0x04

/* ======================================================================
 * THE REPORT QUEUE
 *
 * With DevicesArePolled FALSE the minidriver is the only source of reports:
 * hidclass sends IOCTL_HID_READ_REPORT down and it is held pending until
 * something is available. So every report, the gamepad's included, is
 * produced here and completed into a waiting read.
 *
 * ORDER MATTERS ON BOTH SIDES. A read finds a queued report first, because
 * anything already queued is older than the request. A report finds a
 * waiting read first, for the same reason.
 *
 * GAMEPAD REPORTS COALESCE; KEYBOARD AND MOUSE ONES DO NOT. A gamepad
 * report is pure state, so a queued one is superseded by the next and only
 * the newest matters - without that, an application opening the collection
 * after a quiet period is handed a burst of history before it sees the
 * present. A keyboard or mouse report is a transition: a down and its up
 * carry meaning only in sequence, and merging them loses the keystroke.
 * ====================================================================== */

#define XC_REPORT_QUEUE_MAX     16

typedef struct _XC_REPORT_NODE {
	UCHAR       Length;                         /* including the ID byte */
	UCHAR       Data[CORE_REPORT_MAX_BYTES];
} XC_REPORT_NODE;

/* ======================================================================
 * THE DEVICE EXTENSION
 *
 * hidclass allocates this, sized from DeviceExtensionSize in the
 * registration block, and hands it back as MiniDeviceExtension.
 * ====================================================================== */

typedef struct _XC_DEVEXT {
	PDEVICE_OBJECT      Fdo;
	PDEVICE_OBJECT      LowerDeviceObject;
	PDEVICE_OBJECT      Pdo;

	BOOLEAN             Started;
	BOOLEAN             Removed;

	/* --- USB --- */
	USB_DEVICE_DESCRIPTOR       DeviceDescriptor;
	PUSB_CONFIGURATION_DESCRIPTOR ConfigDescriptor;
	USBD_CONFIGURATION_HANDLE   ConfigHandle;
	USBD_PIPE_HANDLE            InPipe;
	USBD_PIPE_HANDLE            OutPipe;
	BOOLEAN                     HasOutPipe;
	UCHAR                       PollInterval;   /* bInterval, as reported */

	/* --- the poll engine --- */
	XC_POLL_SLOT        Poll[XC_POLL_SLOTS];
	ULONG               PollStopMask;
	KSPIN_LOCK          PollLock;

	/* --- the report queue and the pending reads --- */
	XC_REPORT_NODE      ReportQueue[XC_REPORT_QUEUE_MAX];
	ULONG               ReportHead;
	ULONG               ReportCount;
	ULONG               ReportsDropped;
	LIST_ENTRY          PendingReads;
	ULONG               PendingReadCount;
	KSPIN_LOCK          QueueLock;

	/* --- the engine --- */
	core_state          Core;
	KSPIN_LOCK          CoreLock;

	/* --- the periodic tick, which serves autofire and the mouse --- */
	KTIMER              Tick;
	KDPC                TickDpc;
	BOOLEAN             TickArmed;

	/* --- statistics, read by the harness --- */
	ULONG               PollErrors;
} XC_DEVEXT, *PXC_DEVEXT;

/*
 * The two-level hop from an FDO to our own extension. hidclass owns
 * DeviceObject->DeviceExtension and keeps ours behind MiniDeviceExtension;
 * confusing the two makes every field wrong.
 */
#define XC_GET_DEVEXT(DO) \
    ((PXC_DEVEXT)(((PHID_DEVICE_EXTENSION)((DO)->DeviceExtension)) \
                  ->MiniDeviceExtension))

#define XC_GET_LOWER(DO) \
    (((PHID_DEVICE_EXTENSION)((DO)->DeviceExtension))->NextDeviceObject)

#define XC_GET_PDO(DO) \
    (((PHID_DEVICE_EXTENSION)((DO)->DeviceExtension))->PhysicalDeviceObject)

/* ======================================================================
 * ENTRY POINTS
 * ====================================================================== */

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath);

NTSTATUS NTAPI XcAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT Fdo);
NTSTATUS NTAPI XcPnp(PDEVICE_OBJECT Fdo, PIRP Irp);
NTSTATUS NTAPI XcPower(PDEVICE_OBJECT Fdo, PIRP Irp);
NTSTATUS NTAPI XcInternalDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp);
NTSTATUS NTAPI XcDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp);
NTSTATUS NTAPI XcCreate(PDEVICE_OBJECT Fdo, PIRP Irp);
NTSTATUS NTAPI XcClose(PDEVICE_OBJECT Fdo, PIRP Irp);
VOID     NTAPI XcUnload(PDRIVER_OBJECT DriverObject);

/* ======================================================================
 * INTERNALS, EXPOSED SO THE HARNESS CAN DRIVE THEM
 * ====================================================================== */

void     XcDevExtInit(PXC_DEVEXT DevExt);

/* The report sink. core_emit hands it a payload and an ID; this is where a
 * report meets the OS. */
void     XcReportSink(void *ctx, u8 report_id, const u8 *payload, u32 len);

/* Queue management, split out so both halves are testable without an IRP. */
void     XcQueueReport(PXC_DEVEXT DevExt, u8 report_id,
                       const u8 *payload, u32 len);
int      XcDequeueReport(PXC_DEVEXT DevExt, XC_REPORT_NODE *out);

NTSTATUS XcStartDevice(PDEVICE_OBJECT Fdo, PIRP Irp);
void     XcStopDevice(PXC_DEVEXT DevExt);

NTSTATUS XcPollStart(PXC_DEVEXT DevExt, ULONG Reason);
void     XcPollStop(PXC_DEVEXT DevExt, ULONG Reason);
NTSTATUS XcPollSubmit(PXC_DEVEXT DevExt, ULONG SlotIndex);

/*
 * Accept one raw transfer. Separated from the completion routine so the
 * harness can feed packets without an IRP. Status and length are what the
 * transfer actually reported.
 */
void     XcOnTransfer(PXC_DEVEXT DevExt, NTSTATUS Status,
                      const u8 *Buffer, ULONG Length, u64 Now100ns);

/*
 * Installing AddDevice writes through DriverObject->DriverExtension, which
 * the harness does not model. One seam rather than an #ifdef at the call
 * site.
 */
void     XcSetAddDevice(PDRIVER_OBJECT DriverObject,
                        PDRIVER_ADD_DEVICE AddDevice);

#endif /* XBOXCTL_WDM_H */
