/*
 * kstub.h - a fake kernel ABI, just large enough to compile wdm.c in user
 * mode.
 *
 * Included by wdm.h when XBOXCTL_USERMODE is defined. The implementations
 * live in harness.c. This is NOT an emulation of Windows: it is the smallest
 * set of types and entry points that lets the OS-facing layer compile and be
 * driven from a console program.
 *
 * Scope rule: declare what a caller actually needs, nothing more. Add to this
 * file when a caller appears, not in anticipation.
 *
 * The USB enumeration path is NOT stubbed. Fetching descriptors and selecting
 * a configuration is pure Windows plumbing with no logic in it, so wdm.c
 * excludes it from the harness build rather than modelling a USB stack. What
 * the harness does drive is everything downstream of a completed transfer,
 * which is where the logic lives.
 *
 * Deliberately does NOT include <windows.h>. Nothing in the harness may, or
 * these definitions collide with the real ones.
 */
#ifndef XBOXCTL_KSTUB_H
#define XBOXCTL_KSTUB_H

/* ---- scalar types ------------------------------------------------- */

typedef long                NTSTATUS;
typedef unsigned long       ULONG;
typedef long                LONG;
typedef unsigned short      USHORT;
typedef short               SHORT;
typedef unsigned char       UCHAR;
typedef unsigned char       BOOLEAN;
typedef void               *PVOID;
typedef unsigned short      WCHAR;
typedef WCHAR              *PWSTR;
typedef const WCHAR        *PCWSTR;
typedef char                CHAR;
typedef void                VOID;

#if defined(_MSC_VER)
typedef unsigned __int64    ULONGLONG;
typedef signed   __int64    LONGLONG;
#else
typedef unsigned long long  ULONGLONG;
typedef signed   long long  LONGLONG;
#endif

#if defined(_WIN64)
typedef unsigned __int64    ULONG_PTR;
#else
typedef unsigned long       ULONG_PTR;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define IN
#define OUT
#define OPTIONAL

/*
 * The kernel calls DriverEntry and every dispatch routine as __stdcall. On
 * x86 that is not cosmetic - a cdecl callee would leave the caller's
 * arguments on the kernel stack. Keep the convention identical in both builds
 * so the harness exercises the same signatures the kernel will use.
 */
#ifndef NTAPI
#define NTAPI __stdcall
#endif

#define CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - (char *)(&((type *)0)->field)))

/* ---- status codes ------------------------------------------------- */

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)
#define STATUS_DEVICE_BUSY              ((NTSTATUS)0x80000011L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_DELETE_PENDING           ((NTSTATUS)0xC0000056L)
#define STATUS_DEVICE_NOT_CONNECTED     ((NTSTATUS)0xC000009DL)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182L)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_DEVICE_DOES_NOT_EXIST    ((NTSTATUS)0xC00000C0L)
#define STATUS_REVISION_MISMATCH        ((NTSTATUS)0xC0000059L)

#define NT_SUCCESS(s)                   (((NTSTATUS)(s)) >= 0)

/* ---- lists -------------------------------------------------------- */

typedef struct _LIST_ENTRY {
	struct _LIST_ENTRY *Flink;
	struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

#define InitializeListHead(h) \
    ((h)->Flink = (h), (h)->Blink = (h))

#define IsListEmpty(h)  ((h)->Flink == (h))

#define InsertTailList(h, e) \
    ((e)->Flink = (h), (e)->Blink = (h)->Blink, \
     (h)->Blink->Flink = (e), (h)->Blink = (e))

#define RemoveEntryList(e) \
    ((e)->Blink->Flink = (e)->Flink, (e)->Flink->Blink = (e)->Blink)

PLIST_ENTRY RemoveHeadList(PLIST_ENTRY Head);

/* ---- synchronisation ---------------------------------------------- */

typedef ULONG_PTR   KSPIN_LOCK, *PKSPIN_LOCK;
typedef UCHAR       KIRQL, *PKIRQL;

void KeInitializeSpinLock(PKSPIN_LOCK Lock);
void KeAcquireSpinLock(PKSPIN_LOCK Lock, PKIRQL OldIrql);
void KeReleaseSpinLock(PKSPIN_LOCK Lock, KIRQL NewIrql);

/* ---- time --------------------------------------------------------- */

typedef union _LARGE_INTEGER {
	struct { ULONG LowPart; LONG HighPart; } u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

ULONGLONG KeQueryInterruptTime(void);

/*
 * The harness drives the clock. Nothing in the driver build has this; it is
 * the one place the two builds deliberately differ, and it is what makes
 * autofire timing and the mouse accumulator deterministic in a test.
 */
void KstubSetInterruptTime(ULONGLONG Now100ns);
void KstubAdvanceMs(ULONG Milliseconds);

/* ---- events ------------------------------------------------------- */

typedef struct _KEVENT {
	LONG Signalled;
} KEVENT, *PKEVENT;

#define NotificationEvent   0
#define SynchronizationEvent 1
#define Executive           0
#define KernelMode          0

void     KeInitializeEvent(PKEVENT Event, int Type, BOOLEAN State);
LONG     KeSetEvent(PKEVENT Event, int Increment, BOOLEAN Wait);
NTSTATUS KeWaitForSingleObject(PVOID Object, int Reason, int Mode,
                               BOOLEAN Alertable, PVOID Timeout);

/* ---- DPCs and timers ---------------------------------------------- */

struct _KDPC;
struct _KTIMER;

typedef void (NTAPI *PKDEFERRED_ROUTINE)(struct _KDPC *Dpc, PVOID Context,
                                         PVOID Arg1, PVOID Arg2);

typedef struct _KDPC {
	PKDEFERRED_ROUTINE Routine;
	PVOID              Context;
} KDPC, *PKDPC;

typedef struct _KTIMER {
	BOOLEAN Armed;
	LONG    PeriodMs;
} KTIMER, *PKTIMER;

void    KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context);
void    KeInitializeTimerEx(PKTIMER Timer, int Type);
BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period,
                     PKDPC Dpc);
BOOLEAN KeCancelTimer(PKTIMER Timer);

#define NotificationTimer   0
#define SynchronizationTimer 1

/* Fire the timer DPC by hand. Harness only. */
void KstubFireTimer(PKTIMER Timer);

/* The driver build waits for any DPC already running to finish before it
 * frees what one might touch. The harness has no second thread. */
void KeFlushQueuedDpcs(void);

/* ---- memory ------------------------------------------------------- */

typedef enum _POOL_TYPE { NonPagedPool = 0, PagedPool = 1 } POOL_TYPE;

PVOID ExAllocatePoolWithTag(POOL_TYPE Type, ULONG Bytes, ULONG Tag);
void  ExFreePoolWithTag(PVOID P, ULONG Tag);

void RtlZeroMemory(PVOID Dst, ULONG Length);
void RtlCopyMemory(PVOID Dst, const void *Src, ULONG Length);

/* ---- strings ------------------------------------------------------ */

typedef struct _UNICODE_STRING {
	USHORT  Length;
	USHORT  MaximumLength;
	PWSTR   Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

/* ---- device and driver objects ------------------------------------ */

struct _IRP;
struct _DEVICE_OBJECT;
struct _DRIVER_OBJECT;

typedef struct _DEVICE_OBJECT {
	ULONG   Flags;
	ULONG   StackSize;
	PVOID   DeviceExtension;
	struct _DRIVER_OBJECT *DriverObject;
} DEVICE_OBJECT, *PDEVICE_OBJECT;

typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(PDEVICE_OBJECT, struct _IRP *);
typedef NTSTATUS (NTAPI *PDRIVER_ADD_DEVICE)(struct _DRIVER_OBJECT *,
                                             PDEVICE_OBJECT);
typedef void     (NTAPI *PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *);

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

typedef struct _DRIVER_OBJECT {
	PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
	PDRIVER_UNLOAD   DriverUnload;
	PVOID            DriverExtension;
} DRIVER_OBJECT, *PDRIVER_OBJECT;

#define DO_POWER_PAGABLE        0x00002000
#define DO_DIRECT_IO            0x00000010
#define DO_DEVICE_INITIALIZING  0x00000080

/* ---- IRPs --------------------------------------------------------- */

typedef struct _IO_STATUS_BLOCK {
	NTSTATUS  Status;
	ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _IO_STACK_LOCATION {
	UCHAR MajorFunction;
	UCHAR MinorFunction;
	union {
		struct {
			ULONG OutputBufferLength;
			ULONG InputBufferLength;
			ULONG IoControlCode;
			PVOID Type3InputBuffer;
		} DeviceIoControl;
		struct {
			PVOID Argument1;
			PVOID Argument2;
			PVOID Argument3;
			PVOID Argument4;
		} Others;
	} Parameters;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/*
 * The Tail.Overlay.ListEntry nesting is copied from the real IRP rather than
 * flattened, so wdm.c queues an IRP the same way in both builds.
 */
typedef struct _IRP {
	IO_STATUS_BLOCK IoStatus;
	PVOID           UserBuffer;
	BOOLEAN         Cancel;
	KIRQL           CancelIrql;
	PVOID           CancelRoutine;
	struct {
		struct {
			LIST_ENTRY ListEntry;
		} Overlay;
	} Tail;
	PIO_STACK_LOCATION KstubStack;
	BOOLEAN            KstubCompleted;
	BOOLEAN            KstubPending;
} IRP, *PIRP;

#define IO_NO_INCREMENT 0

typedef void (NTAPI *PDRIVER_CANCEL)(PDEVICE_OBJECT, struct _IRP *);

PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp);
void               IoCompleteRequest(PIRP Irp, CHAR PriorityBoost);
void               IoMarkIrpPending(PIRP Irp);
PDRIVER_CANCEL     IoSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL CancelRoutine);
void               IoAcquireCancelSpinLock(PKIRQL OldIrql);
void               IoReleaseCancelSpinLock(KIRQL Irql);

/* ---- HID minidriver contract -------------------------------------- */

#define HID_REVISION 0x00000001

typedef struct _HID_DEVICE_EXTENSION {
	PDEVICE_OBJECT PhysicalDeviceObject;
	PDEVICE_OBJECT NextDeviceObject;
	PVOID          MiniDeviceExtension;
} HID_DEVICE_EXTENSION, *PHID_DEVICE_EXTENSION;

typedef struct _HID_MINIDRIVER_REGISTRATION {
	ULONG           Revision;
	PDRIVER_OBJECT  DriverObject;
	PUNICODE_STRING RegistryPath;
	ULONG           DeviceExtensionSize;
	BOOLEAN         DevicesArePolled;
	UCHAR           Reserved[3];
} HID_MINIDRIVER_REGISTRATION, *PHID_MINIDRIVER_REGISTRATION;

NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION Registration);

#pragma pack(push, 1)
typedef struct _HID_DESCRIPTOR {
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	USHORT bcdHID;
	UCHAR  bCountry;
	UCHAR  bNumDescriptors;
	struct _HID_DESCRIPTOR_DESC_LIST {
		UCHAR  bReportType;
		USHORT wReportLength;
	} DescriptorList[1];
} HID_DESCRIPTOR, *PHID_DESCRIPTOR;
#pragma pack(pop)

typedef struct _HID_DEVICE_ATTRIBUTES {
	ULONG  Size;
	USHORT VendorID;
	USHORT ProductID;
	USHORT VersionNumber;
	USHORT Reserved[11];
} HID_DEVICE_ATTRIBUTES, *PHID_DEVICE_ATTRIBUTES;

typedef struct _HID_XFER_PACKET {
	UCHAR *reportBuffer;
	ULONG  reportBufferLen;
	UCHAR  reportId;
} HID_XFER_PACKET, *PHID_XFER_PACKET;

#define HID_HID_DESCRIPTOR_TYPE     0x21
#define HID_REPORT_DESCRIPTOR_TYPE  0x22

/*
 * CTL_CODE and its arguments, so wdm.h can spell the private control codes
 * the same way in both builds rather than hard-coding two copies.
 */
#define METHOD_BUFFERED     0
#define FILE_ANY_ACCESS     0
#define FILE_READ_ACCESS    1
#define FILE_WRITE_ACCESS   2
#define CTL_CODE(DeviceType, Function, Method, Access)     (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

/*
 * A fast mutex guards the device registry. The harness is single threaded,
 * so these are shape only - but the shape has to exist, because the code
 * that takes it is the code the driver runs.
 */
typedef struct _FAST_MUTEX { LONG Held; } FAST_MUTEX, *PFAST_MUTEX;
void ExInitializeFastMutex(PFAST_MUTEX Mutex);
void ExAcquireFastMutex(PFAST_MUTEX Mutex);
void ExReleaseFastMutex(PFAST_MUTEX Mutex);

/*
 * The real codes are CTL_CODE(FILE_DEVICE_KEYBOARD, fn, METHOD_NEITHER,
 * FILE_ANY_ACCESS), which flattens to 0x000B0000 | (fn << 2) | 3. Spelled out
 * rather than recomputed so the harness and the driver agree by construction.
 */
#define IOCTL_HID_GET_DEVICE_DESCRIPTOR 0x000B0003
#define IOCTL_HID_GET_REPORT_DESCRIPTOR 0x000B0007
#define IOCTL_HID_READ_REPORT           0x000B000B
#define IOCTL_HID_WRITE_REPORT          0x000B000F
#define IOCTL_HID_GET_STRING            0x000B0013
#define IOCTL_HID_ACTIVATE_DEVICE       0x000B001F
#define IOCTL_HID_DEACTIVATE_DEVICE     0x000B0023
#define IOCTL_HID_GET_DEVICE_ATTRIBUTES 0x000B0027
#define IOCTL_HID_SET_FEATURE           0x000B0191
#define IOCTL_HID_GET_FEATURE           0x000B0192

/* ---- USB, types only ---------------------------------------------- */

typedef PVOID USBD_CONFIGURATION_HANDLE;
typedef PVOID USBD_PIPE_HANDLE;
typedef PVOID PURB;

#pragma pack(push, 1)
typedef struct _USB_DEVICE_DESCRIPTOR {
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	USHORT bcdUSB;
	UCHAR  bDeviceClass;
	UCHAR  bDeviceSubClass;
	UCHAR  bDeviceProtocol;
	UCHAR  bMaxPacketSize0;
	USHORT idVendor;
	USHORT idProduct;
	USHORT bcdDevice;
	UCHAR  iManufacturer;
	UCHAR  iProduct;
	UCHAR  iSerialNumber;
	UCHAR  bNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

typedef struct _USB_CONFIGURATION_DESCRIPTOR {
	UCHAR  bLength;
	UCHAR  bDescriptorType;
	USHORT wTotalLength;
	UCHAR  bNumInterfaces;
	UCHAR  bConfigurationValue;
	UCHAR  iConfiguration;
	UCHAR  bmAttributes;
	UCHAR  MaxPower;
} USB_CONFIGURATION_DESCRIPTOR, *PUSB_CONFIGURATION_DESCRIPTOR;
#pragma pack(pop)

#endif /* XBOXCTL_KSTUB_H */
