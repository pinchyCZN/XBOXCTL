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

/*
 * A FAILED TRANSFER IS NEVER RESUBMITTED FROM THE COMPLETION ROUTINE.
 *
 * When the device is gone the bus driver fails a submit SYNCHRONOUSLY:
 * IoCallDriver does not return until the completion routine has already
 * run, so resubmitting from inside it re-enters it on the same stack.
 * With a device that fails every time, that recursion is unbounded and
 * exhausts the DPC stack. Measured on a live unplug: twenty nested
 * XcPollComplete frames through usbhub and USBPORT, and a guest that
 * froze and then bugchecked.
 *
 * The success path still resubmits inline, and is safe because it is
 * rate-limited by the endpoint: a transfer that carried data cannot
 * have its successor complete instantly. Only the failure path hands
 * off, to a work item that retries at PASSIVE_LEVEL and gives up.
 */
#define XC_POLL_RETRY_MAX       3
#define XC_POLL_RETRY_MS        50

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

	/*
	 * IN-FLIGHT TRANSFER COUNT, and it is what makes teardown safe.
	 *
	 * IoCancelIrp is asynchronous: it asks, and the bus driver completes
	 * the transfer some time later. Freeing the IRP on the strength of
	 * having called it means the completion lands in freed memory. The
	 * count starts at one - a reference held by the device itself - and
	 * each submitted transfer takes another; teardown drops the device's
	 * reference and waits for the rest.
	 */
	LONG                IoCount;
	BOOLEAN             IoDraining;
	KEVENT              IoIdle;

	/* Error recovery. RestartWorkItem is a PIO_WORKITEM, held as PVOID
	 * so the harness needs no stub for a type it never dereferences. */
	PVOID               RestartWorkItem;
	LONG                RestartQueued;
	ULONG               PollRetries;
	ULONG               PollRestartRequests;
	ULONG               PipeResets;

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
 * THE CONTROL DEVICE
 *
 * Configuration does not arrive over HID. User mode opens \\.\xboxctl
 * with CreateFile and drives it with DeviceIoControl, which is the
 * Adaptoid's arrangement and is specified in ../docs/driver-plan.txt
 * section 7.
 *
 * EVERY CODE IS METHOD_BUFFERED, so the I/O manager copies the payload
 * into system memory and a user-mode pointer is never dereferenced. Note
 * that buffered I/O gives ONE buffer for both directions: the output
 * overwrites the input, so a handler must finish reading before it
 * starts writing.
 *
 * THE SECURITY BOUNDARY IS THE DEVICE ACL, NOTHING ELSE. Nothing in the
 * payloads authenticates a caller. The mutating codes ask for
 * FILE_WRITE_ACCESS so a read-only handle cannot reconfigure a pad.
 * ====================================================================== */

#define XC_DEVICE_TYPE          0xB9C0

#define XC_IOCTL_READ(fn) \
    CTL_CODE(XC_DEVICE_TYPE, 0x800 + (fn), METHOD_BUFFERED, \
             FILE_ANY_ACCESS)
#define XC_IOCTL_WRITE(fn) \
    CTL_CODE(XC_DEVICE_TYPE, 0x800 + (fn), METHOD_BUFFERED, \
             FILE_WRITE_ACCESS)

#define IOCTL_XC_GET_VERSION    XC_IOCTL_READ(0)
#define IOCTL_XC_GET_DEVICES    XC_IOCTL_READ(1)
#define IOCTL_XC_GET_CONFIG     XC_IOCTL_READ(2)
#define IOCTL_XC_GET_STATS      XC_IOCTL_READ(3)
#define IOCTL_XC_SET_CONFIG     XC_IOCTL_WRITE(4)
#define IOCTL_XC_RESET_CONFIG   XC_IOCTL_WRITE(5)

/* More pads than anyone has. The array is walked, not searched. */
#define XC_MAX_DEVICES          8

/*
 * The largest blob this build will accept. Sized to what it emits,
 * with nothing spare: a bigger buffer only buys the chance to copy a
 * bigger mistake.
 */
#define XC_CONFIG_BLOB_MAX \
    ((ULONG)(sizeof(core_config_header) + \
             sizeof(core_stick) * CORE_STICK_COUNT + \
             sizeof(core_layout) * CORE_MAX_LAYOUTS))

/*
 * GET_VERSION. THE CONFIGURATOR ASKS BEFORE IT SENDS, so a build that
 * accepts fewer layouts or a different blob version says so rather than
 * rejecting the push with nothing to explain it.
 */
typedef struct _XC_VERSION_INFO {   /* 20 bytes */
	u32 signature;          /* XC_CONFIG_SIGNATURE                  */
	u16 driver_major;
	u16 driver_minor;
	u16 driver_patch;
	u16 config_version;     /* CORE_CFG_VERSION                     */
	u16 max_layouts;
	u16 max_bindings;
	u16 max_chords;
	u16 blob_bytes;         /* the blob size this build emits       */
} XC_VERSION_INFO;

typedef struct _XC_DEVICE_ENTRY {   /* 12 bytes */
	u32 index;              /* what the other codes take            */
	u16 vendor_id;
	u16 product_id;
	u8  started;
	u8  reserved[3];
} XC_DEVICE_ENTRY;

typedef struct _XC_DEVICE_LIST {
	u32             count;
	XC_DEVICE_ENTRY device[XC_MAX_DEVICES];
} XC_DEVICE_LIST;

typedef struct _XC_STATS {          /* 40 bytes */
	u32 index;
	u32 packets_accepted;
	u32 packets_rejected;
	u32 reports_emitted;
	u32 reports_dropped;
	u32 poll_errors;
	u32 pipe_resets;
	u32 poll_restarts;
	u32 pending_reads;
	u32 layer;              /* the live layer, 1-based              */
} XC_STATS;

/*
 * SET_CONFIG, GET_CONFIG and RESET_CONFIG all begin with the index.
 * SET_CONFIG carries the blob immediately after it; its length is
 * InputBufferLength minus this header, NOT a field, because a length
 * that can disagree with the buffer is a length that eventually does.
 */
typedef struct _XC_CONFIG_REQUEST {
	u32 index;
} XC_CONFIG_REQUEST;

/*
 * The commands, with the IRP plumbing stripped off. buffer is the single
 * METHOD_BUFFERED buffer, in_len what arrived in it and out_len how much
 * room there is for a reply; written receives the reply size.
 *
 * SPLIT OUT SO THE HARNESS CAN DRIVE EVERY COMMAND without an IRP, a
 * device object or a symbolic link.
 */
NTSTATUS XcControlCommand(ULONG code, void *buffer, ULONG in_len,
                          ULONG out_len, ULONG *written);

/* The registry of live pads, which is what an index indexes. */
void     XcDeviceRegister(PXC_DEVEXT DevExt);
void     XcDeviceUnregister(PXC_DEVEXT DevExt);
void     XcDeviceRegistryReset(void);

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

/*
 * Teardown. Stops polling, waits for every transfer already in flight to
 * come back, and only then frees the IRPs they would complete into.
 */
void     XcRemoveDevice(PXC_DEVEXT DevExt);

/* The in-flight count of section XC_DEVEXT. Acquire refuses once draining
 * has begun, which is what stops a completion resubmitting into teardown. */
BOOLEAN  XcIoAcquire(PXC_DEVEXT DevExt);
void     XcIoRelease(PXC_DEVEXT DevExt);
void     XcIoDrainAndWait(PXC_DEVEXT DevExt);

/*
 * Ask for polling to be restarted after a failed transfer. In the
 * driver this queues a work item; the harness only counts the request.
 * Either way it must NOT submit anything on the caller's stack - see
 * the note above XC_POLL_RETRY_MAX.
 */
void     XcRequestPollRestart(PXC_DEVEXT DevExt);

/*
 * Clear a halted interrupt IN pipe.
 *
 * A HALTED ENDPOINT STAYS HALTED UNTIL SOMETHING CLEARS IT, so a retry
 * loop on its own cannot recover from a stall: every resubmit onto a
 * stalled pipe fails exactly as the first one did, the attempts are
 * exhausted, and polling stops for good with the device still present.
 * Only a replug would bring it back.
 *
 * PASSIVE_LEVEL only, which is why this belongs to the work item and
 * not to the completion routine.
 *
 * In this WDK URB_FUNCTION_RESET_PIPE IS URB_FUNCTION_SYNC_RESET_PIPE_
 * AND_CLEAR_STALL - usb.h defines the first as the second, both 0x1E -
 * so the reset clears the halt on the DEVICE as well as the host's
 * view of it. There is no weaker variant to choose by mistake.
 */
void     XcResetPipe(PXC_DEVEXT DevExt);

/* True when no slot has a transfer outstanding. Resetting a pipe with
 * one in flight is not meaningful; see XcPollRestartWorker. */
BOOLEAN  XcPollAllIdle(PXC_DEVEXT DevExt);

/*
 * Device power transitions.
 *
 * THE ORDER IS ASYMMETRIC AND BOTH HALVES MATTER. Going down, the work
 * happens BEFORE the IRP is forwarded: once the bus has taken the
 * device's power away, submitting to it is meaningless. Coming up, the
 * work happens AFTER, on the way back through a completion routine,
 * because the hardware is not powered until the stack below has
 * finished with the IRP.
 *
 * RELEASING WHAT IS HELD IS THE HALF THAT IS EASY TO FORGET. A key or
 * a mouse button asserted at the moment the device suspends has
 * nothing to release it: no packet will ever arrive saying the control
 * came back up, so the host keeps it down for the whole of the suspend
 * and after it. A pad that sleeps mid-keypress must not leave the
 * keyboard holding that key.
 */
void     XcPowerDown(PXC_DEVEXT DevExt);
void     XcPowerUp(PXC_DEVEXT DevExt);


/*
 * What to do with a slot whose transfer has come back: resubmit,
 * stand down, or ask for recovery. Split out of the completion routine
 * so the decision is the same in both builds and can be tested without
 * a USB stack - it is the decision that froze a guest when it was
 * wrong. Consumes the reference the submit took.
 */
void     XcPollFinish(PXC_DEVEXT DevExt, ULONG SlotIndex,
                      NTSTATUS Status);

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
