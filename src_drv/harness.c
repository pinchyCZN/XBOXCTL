/*
 * harness.c - main() and the tests.
 *
 * Builds core.c and wdm.c as an ordinary console program with
 * XBOXCTL_USERMODE defined, supplies the kernel routines kstub.h declares,
 * and drives the engine with the clock as an argument.
 *
 * That last part is the point. Autofire deadlines, the mouse velocity
 * accumulator and the acceleration ramp are all functions of elapsed time;
 * testing them means stepping the clock by hand, which a virtual machine
 * cannot offer and a kernel debugger makes miserable.
 *
 * No <windows.h> here or anywhere the harness reaches: kstub.h declares its
 * own NTSTATUS, ULONG and the rest, and the real ones would collide.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wdm.h"

/* ======================================================================
 * THE KERNEL STUBS
 * ====================================================================== */

/*
 * Declared before the stubs because one of them reports a failure: a wait
 * on an unsignalled event would hang a real driver, and the harness has to
 * say so rather than sail past it.
 */
static int g_Checks = 0;
static int g_Failures = 0;


static ULONGLONG g_InterruptTime = 0;

ULONGLONG KeQueryInterruptTime(void)
{
	return g_InterruptTime;
}

void KstubSetInterruptTime(ULONGLONG Now100ns)
{
	g_InterruptTime = Now100ns;
}

void KstubAdvanceMs(ULONG Milliseconds)
{
	g_InterruptTime += (ULONGLONG)Milliseconds * 10000u;
}

PLIST_ENTRY RemoveHeadList(PLIST_ENTRY Head)
{
	PLIST_ENTRY entry = Head->Flink;

	Head->Flink = entry->Flink;
	entry->Flink->Blink = Head;
	return entry;
}

/* Single threaded, so a spin lock is a no-op that still type-checks. */
void KeInitializeSpinLock(PKSPIN_LOCK Lock)          { *Lock = 0; }
void KeAcquireSpinLock(PKSPIN_LOCK Lock, PKIRQL Old) { (void)Lock; *Old = 0; }
void KeReleaseSpinLock(PKSPIN_LOCK Lock, KIRQL New)  { (void)Lock; (void)New; }

void IoAcquireCancelSpinLock(PKIRQL Old) { *Old = 0; }
void IoReleaseCancelSpinLock(KIRQL Irql) { (void)Irql; }

void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
	Dpc->Routine = Routine;
	Dpc->Context = Context;
}

static PKDPC g_TimerDpc = NULL;

void KeInitializeTimerEx(PKTIMER Timer, int Type)
{
	(void)Type;
	Timer->Armed = FALSE;
	Timer->PeriodMs = 0;
}

BOOLEAN KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER Due, LONG Period, PKDPC Dpc)
{
	(void)Due;
	Timer->Armed = TRUE;
	Timer->PeriodMs = Period;
	g_TimerDpc = Dpc;
	return FALSE;
}

BOOLEAN KeCancelTimer(PKTIMER Timer)
{
	BOOLEAN was = Timer->Armed;

	Timer->Armed = FALSE;
	return was;
}

void KstubFireTimer(PKTIMER Timer)
{
	if (Timer->Armed && g_TimerDpc != NULL && g_TimerDpc->Routine != NULL) {
		g_TimerDpc->Routine(g_TimerDpc, g_TimerDpc->Context, NULL, NULL);
	}
}

void KeFlushQueuedDpcs(void)
{
	/* Single threaded: no DPC can be running. */
}

void KeInitializeEvent(PKEVENT Event, int Type, BOOLEAN State)
{
	(void)Type;
	Event->Signalled = State ? 1 : 0;
}

LONG KeSetEvent(PKEVENT Event, int Increment, BOOLEAN Wait)
{
	LONG was = Event->Signalled;

	(void)Increment;
	(void)Wait;
	Event->Signalled = 1;
	return was;
}

/*
 * A wait that cannot block is the honest stub here. The harness is single
 * threaded, so anything the driver waits for has already happened by the
 * time it waits - and a wait that blocked would simply deadlock rather than
 * model anything. A test asserts on the COUNT reaching zero instead, which
 * is the property the wait exists to guarantee.
 */
NTSTATUS KeWaitForSingleObject(PVOID Object, int Reason, int Mode,
                               BOOLEAN Alertable, PVOID Timeout)
{
	PKEVENT event = (PKEVENT)Object;

	(void)Reason;
	(void)Mode;
	(void)Alertable;
	(void)Timeout;

	if (event != NULL && !event->Signalled) {
		printf("  FAIL  waited on an unsignalled event - a real driver "
		       "would hang here\n");
		g_Failures++;
	}
	return STATUS_SUCCESS;
}

PVOID ExAllocatePoolWithTag(POOL_TYPE Type, ULONG Bytes, ULONG Tag)
{
	(void)Type;
	(void)Tag;
	return malloc(Bytes);
}

void ExFreePoolWithTag(PVOID P, ULONG Tag)
{
	(void)Tag;
	free(P);
}

void RtlZeroMemory(PVOID Dst, ULONG Length)
{
	memset(Dst, 0, Length);
}

void RtlCopyMemory(PVOID Dst, const void *Src, ULONG Length)
{
	memcpy(Dst, Src, Length);
}

PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp)
{
	return Irp->KstubStack;
}

void IoCompleteRequest(PIRP Irp, CHAR PriorityBoost)
{
	(void)PriorityBoost;
	Irp->KstubCompleted = TRUE;
}

void IoMarkIrpPending(PIRP Irp)
{
	Irp->KstubPending = TRUE;
}

PDRIVER_CANCEL IoSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL CancelRoutine)
{
	PDRIVER_CANCEL old = (PDRIVER_CANCEL)Irp->CancelRoutine;

	Irp->CancelRoutine = (PVOID)CancelRoutine;
	return old;
}

NTSTATUS HidRegisterMinidriver(PHID_MINIDRIVER_REGISTRATION Registration)
{
	/*
	 * The real hidclass overwrites the dispatch table here. Nothing in the
	 * harness depends on that, so this only records that the contract was
	 * honoured: a registration with no AddDevice installed would enumerate
	 * nothing, and DevicesArePolled TRUE would mean the driver can never
	 * originate a report.
	 */
	if (Registration->Revision != HID_REVISION) {
		return STATUS_INVALID_PARAMETER;
	}
	if (Registration->DevicesArePolled) {
		printf("  FAIL: DevicesArePolled must be FALSE\n");
		return STATUS_INVALID_PARAMETER;
	}
	return STATUS_SUCCESS;
}

/* ======================================================================
 * TEST SCAFFOLDING
 * ====================================================================== */

static void check(int condition, const char *what)
{
	g_Checks++;
	if (!condition) {
		g_Failures++;
		printf("  FAIL  %s\n", what);
	}
}

static void check_eq(long got, long want, const char *what)
{
	g_Checks++;
	if (got != want) {
		g_Failures++;
		printf("  FAIL  %s: got %ld, want %ld\n", what, got, want);
	}
}

/* ---- a recording sink, so a test can assert on the wire bytes ------- */

#define SINK_MAX 64

typedef struct _sink_record {
	u8  id;
	u32 len;
	u8  payload[CORE_REPORT_MAX_BYTES];
} sink_record;

static sink_record g_Sink[SINK_MAX];
static u32         g_SinkCount;

static void sink_reset(void)
{
	g_SinkCount = 0;
}

static void recording_sink(void *ctx, u8 id, const u8 *payload, u32 len)
{
	(void)ctx;
	if (g_SinkCount >= SINK_MAX) {
		return;
	}
	g_Sink[g_SinkCount].id = id;
	g_Sink[g_SinkCount].len = len;
	memcpy(g_Sink[g_SinkCount].payload, payload, len);
	g_SinkCount++;
}

/* ======================================================================
 * THE DESCRIPTOR
 *
 * A transcription slip in a report descriptor does not fail loudly; it
 * produces a device that enumerates as something subtly wrong. So the
 * descriptor is parsed here rather than eyeballed.
 * ====================================================================== */

static int test_descriptor(void)
{
	u32       len = 0;
	const u8 *d = core_hid_descriptor(&len);
	u32       i = 0;
	int       depth = 0;
	int       top_level = 0;
	int       report_ids[8];
	int       n_ids = 0;
	u8        top_usages[8];
	int       n_top = 0;
	u8        pending_usage = 0;
	int       have_usage = 0;

	printf("descriptor: %u bytes\n", len);

	while (i < len) {
		u8  item = d[i];
		u32 size = (u32)(item & 0x03);
		u8  tag_type = (u8)(item & 0xFC);

		if (size == 3) {
			size = 4;
		}
		if (i + 1 + size > len) {
			check(0, "descriptor item runs past the end");
			return 1;
		}

		/* Usage (local, tag 0x08) at depth 0 names the next collection. */
		if (tag_type == 0x08 && depth == 0 && size >= 1) {
			pending_usage = d[i + 1];
			have_usage = 1;
		}

		/* Collection (main, tag 0xA0). */
		if (tag_type == 0xA0) {
			if (depth == 0) {
				top_level++;
				if (have_usage && n_top < 8) {
					top_usages[n_top++] = pending_usage;
				}
				have_usage = 0;
			}
			depth++;
		}

		/* End Collection (main, tag 0xC0). */
		if (tag_type == 0xC0) {
			depth--;
			if (depth < 0) {
				check(0, "unbalanced End Collection");
				return 1;
			}
		}

		/* Report ID (global, tag 0x84). */
		if (tag_type == 0x84 && size >= 1 && n_ids < 8) {
			int j;
			int seen = 0;

			for (j = 0; j < n_ids; j++) {
				if (report_ids[j] == (int)d[i + 1]) {
					seen = 1;
				}
			}
			if (!seen) {
				report_ids[n_ids++] = (int)d[i + 1];
			}
		}

		i += 1 + size;
	}

	check_eq(depth, 0, "collections balance");
	check_eq(top_level, 3, "three top-level collections");

	/*
	 * The three usages Windows derives the child devnodes from, in
	 * descriptor order. Gamepad first, so it lands on &Col01.
	 */
	check_eq(n_top, 3, "three named top-level collections");
	if (n_top == 3) {
		check_eq(top_usages[0], 0x05, "Col01 is a Gamepad");
		check_eq(top_usages[1], 0x06, "Col02 is a Keyboard");
		check_eq(top_usages[2], 0x02, "Col03 is a Mouse");
	}

	check_eq(n_ids, 6, "six distinct report IDs");

	return 0;
}

/* ======================================================================
 * THE DECODE
 * ====================================================================== */

static void make_packet(u8 *p)
{
	memset(p, 0, CORE_RAW_PACKET_BYTES);
	p[CORE_RAW_TYPE] = CORE_RAW_TYPE_INPUT;
	p[CORE_RAW_LENGTH] = CORE_RAW_PACKET_BYTES;
}

static void put_le16(u8 *at, int v)
{
	at[0] = (u8)(v & 0xFF);
	at[1] = (u8)((v >> 8) & 0xFF);
}

static int test_decode(void)
{
	core_state cs;
	u8         packet[CORE_RAW_PACKET_BYTES];

	core_init(&cs, recording_sink, NULL);
	sink_reset();

	/* A short transfer must be rejected, not decoded. */
	make_packet(packet);
	core_on_packet(&cs, packet, 19, 1000);
	check_eq((long)cs.packets_rejected, 1, "short packet rejected");
	check_eq((long)g_SinkCount, 0, "short packet emits nothing");

	/* Digital buttons and a fully pressed analog A. */
	make_packet(packet);
	packet[CORE_RAW_DIGITAL] = CORE_DIG_START | CORE_DIG_DPAD_UP;
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;     /* A */
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 2000);

	check_eq((long)cs.packets_accepted, 1, "packet accepted");
	check_eq(cs.semiaxis[CORE_SA_START], CORE_MAX_VALUE, "Start decoded");
	check_eq(cs.semiaxis[CORE_SA_DPAD_UP], CORE_MAX_VALUE, "D-up decoded");
	check_eq(cs.semiaxis[CORE_SA_A], CORE_MAX_VALUE, "A at full pressure");
	check_eq(cs.semiaxis[CORE_SA_B], 0, "B not pressed");
	check_eq((long)g_SinkCount, 1, "one gamepad report");
	check_eq(g_Sink[0].id, CORE_REPORT_ID_GAMEPAD, "it is the gamepad");
	check_eq(g_Sink[0].len, CORE_GAMEPAD_PAYLOAD, "gamepad payload size");
	check_eq(g_Sink[0].payload[CORE_GP_HAT], 0, "hat reads North");
	check(g_Sink[0].payload[CORE_GP_BUTTONS] & 0x01, "button 1 is A");

	/* An identical packet must not emit again. */
	sink_reset();
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 3000);
	check_eq((long)g_SinkCount, 0, "unchanged state emits nothing");

	/* Half pressure on the analog A button. */
	packet[CORE_RAW_ANALOG_BASE + 0] = 128;
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 4000);
	check(cs.semiaxis[CORE_SA_A] > CORE_MAX_VALUE / 2 - 200 &&
	      cs.semiaxis[CORE_SA_A] < CORE_MAX_VALUE / 2 + 200,
	      "A at half pressure reads about half scale");

	/*
	 * The pad reports Y up-positive and HID is down-positive, so a stick
	 * pushed up must produce a NEGATIVE Y semiaxis pair - that is, the
	 * "negative" half carries the magnitude.
	 */
	make_packet(packet);
	put_le16(&packet[CORE_RAW_LSTICK_Y], 32767);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 5000);
	check(cs.semiaxis[CORE_SA_LSTICK_YNEG] > CORE_MAX_VALUE - 100,
	      "stick up gives Y-negative, HID convention");
	check_eq(cs.semiaxis[CORE_SA_LSTICK_YPOS], 0, "and nothing positive");

	/* The hat table: opposing directions cancel to centred. */
	make_packet(packet);
	packet[CORE_RAW_DIGITAL] = CORE_DIG_DPAD_LEFT | CORE_DIG_DPAD_RIGHT;
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 6000);
	sink_reset();
	packet[CORE_RAW_DIGITAL] = CORE_DIG_DPAD_UP | CORE_DIG_DPAD_LEFT;
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, 7000);
	check_eq(g_Sink[0].payload[CORE_GP_HAT], 7, "up+left reads North-West");

	return 0;
}

/* ======================================================================
 * THE RADIAL MATH
 *
 * Two axes at CORE_MAX_VALUE give a sum of squares of 2.45e9, which does
 * not fit a signed 32-bit integer. Computing it in 32 bits wraps negative
 * and yields a radius near 42900 instead of 49497. This is the exact case.
 * ====================================================================== */

static int test_radial(void)
{
	s32 x;
	s32 y;
	u32 r;

	check_eq((long)core_isqrt64(0), 0, "isqrt(0)");
	check_eq((long)core_isqrt64(1), 1, "isqrt(1)");
	check_eq((long)core_isqrt64(144), 12, "isqrt(144)");
	check_eq((long)core_isqrt64(145), 12, "isqrt truncates");

	r = core_isqrt64((u64)CORE_MAX_VALUE * CORE_MAX_VALUE * 2);
	check(r >= 49495 && r <= 49498, "full diagonal radius is about 49497");

	/* A full diagonal is outside the circle and must be pulled onto it. */
	x = CORE_MAX_VALUE;
	y = CORE_MAX_VALUE;
	core_crop_vector(&x, &y);
	check(x > 24000 && x < 24900, "cropped diagonal X is about 24749");
	check_eq(x, y, "crop preserves a 45 degree direction");

	/* A vector already inside the circle is untouched. */
	x = 1000;
	y = -2000;
	core_crop_vector(&x, &y);
	check_eq(x, 1000, "inside the circle, X unchanged");
	check_eq(y, -2000, "inside the circle, Y unchanged");

	return 0;
}

/* ======================================================================
 * THE KEYBOARD STATE MACHINE
 * ====================================================================== */

static int test_keyboard(void)
{
	core_state cs;
	int        i;

	core_init(&cs, recording_sink, NULL);
	sink_reset();

	core_key_event(&cs, 0x1A, 1);           /* W down */
	check_eq((long)g_SinkCount, 1, "W down emits one report");
	check_eq(g_Sink[0].id, CORE_REPORT_ID_KEYBOARD, "it is the keyboard");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS], 0x1A, "W in the first slot");

	/* A press of a key already held is not news. */
	core_key_event(&cs, 0x1A, 1);
	check_eq((long)g_SinkCount, 1, "repeat press emits nothing");

	/* A release of a key that is not held is not news either. */
	core_key_event(&cs, 0x1B, 0);
	check_eq((long)g_SinkCount, 1, "spurious release emits nothing");

	/* Modifiers are a bitmask, not array entries. */
	sink_reset();
	core_key_event(&cs, 0xE1, 1);           /* LeftShift down */
	check_eq((long)g_SinkCount, 1, "shift emits");
	check_eq(g_Sink[0].payload[CORE_KB_MODIFIERS], 0x02, "LeftShift bit");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS], 0x1A, "W still held");

	/*
	 * THE ARRAY IS ORDERED BY PRESS TIME AND STAYS DENSE. Releasing the
	 * middle key must shift the tail down, not swap the last entry into
	 * the gap - the order is what makes the reported slots the OLDEST
	 * keys held rather than an arbitrary selection.
	 */
	core_init(&cs, recording_sink, NULL);
	core_key_event(&cs, 0x04, 1);           /* A */
	core_key_event(&cs, 0x05, 1);           /* B */
	core_key_event(&cs, 0x06, 1);           /* C */
	sink_reset();
	core_key_event(&cs, 0x05, 0);           /* release B */
	check_eq((long)g_SinkCount, 1, "release emits");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS + 0], 0x04, "A stays first");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS + 1], 0x06, "C shifts down");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS + 2], 0x00, "and the tail clears");

	/*
	 * Above six keys every slot carries ErrorRollOver while the real list
	 * keeps being tracked, so releasing back to six restores it with no
	 * fresh press.
	 */
	core_init(&cs, recording_sink, NULL);
	for (i = 0; i < 7; i++) {
		core_key_event(&cs, (u8)(0x04 + i), 1);
	}
	sink_reset();
	core_key_event(&cs, (u8)(0x04 + 7), 1); /* an eighth */
	check_eq(g_Sink[0].payload[CORE_KB_KEYS + 0], CORE_KEY_ROLLOVER,
	         "over six keys reports ErrorRollOver");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS + 5], CORE_KEY_ROLLOVER,
	         "in every slot");

	sink_reset();
	core_key_event(&cs, (u8)(0x04 + 7), 0);
	core_key_event(&cs, (u8)(0x04 + 6), 0); /* back down to six */
	check_eq(g_Sink[g_SinkCount - 1].payload[CORE_KB_KEYS + 0], 0x04,
	         "the real list comes back without a fresh press");

	/* Usage 0 releases everything, modifiers included. */
	sink_reset();
	core_key_event(&cs, 0xE0, 1);
	sink_reset();
	core_key_event(&cs, 0, 0);
	check_eq((long)g_SinkCount, 1, "release-all emits once");
	check_eq(g_Sink[0].payload[CORE_KB_MODIFIERS], 0, "modifiers cleared");
	check_eq(g_Sink[0].payload[CORE_KB_KEYS], 0, "keys cleared");

	/* And a second release-all is not news. */
	sink_reset();
	core_key_event(&cs, 0, 0);
	check_eq((long)g_SinkCount, 0, "release-all when idle emits nothing");

	/* Usages outside both accepted ranges are ignored outright. */
	sink_reset();
	core_key_event(&cs, 0xF0, 1);
	core_key_event(&cs, 0x02, 1);
	check_eq((long)g_SinkCount, 0, "out-of-range usages ignored");

	return 0;
}

/* ======================================================================
 * THE MOUSE
 * ====================================================================== */

static int test_mouse(void)
{
	core_state cs;

	core_init(&cs, recording_sink, NULL);
	sink_reset();

	/*
	 * NEVER EMIT AN ALL-ZERO MOVE. At the tick rate this driver runs at,
	 * doing so is hundreds of pointless reports a second through mouclass
	 * for as long as it is loaded.
	 */
	core_mouse_move(&cs, 0, 0);
	check_eq((long)g_SinkCount, 0, "zero motion emits nothing");

	core_mouse_move(&cs, 12, -3);
	check_eq((long)g_SinkCount, 1, "motion emits");
	check_eq(g_Sink[0].id, CORE_REPORT_ID_MOUSE, "it is the mouse");
	check_eq(g_Sink[0].len, CORE_MOUSE_PAYLOAD, "mouse payload size");
	check_eq((s16)(g_Sink[0].payload[CORE_MS_X] |
	               (g_Sink[0].payload[CORE_MS_X + 1] << 8)), 12, "dx");
	check_eq((s16)(g_Sink[0].payload[CORE_MS_Y] |
	               (g_Sink[0].payload[CORE_MS_Y + 1] << 8)), -3, "dy");

	/* Motion is relative: once reported it is spent. */
	sink_reset();
	core_mouse_button(&cs, 1, 1);
	check_eq((long)g_SinkCount, 1, "button emits");
	check_eq(g_Sink[0].payload[CORE_MS_BUTTONS], 0x01, "button 1 down");
	check_eq(g_Sink[0].payload[CORE_MS_X], 0, "motion was not repeated");

	/* A press of a button already down is not news. */
	sink_reset();
	core_mouse_button(&cs, 1, 1);
	check_eq((long)g_SinkCount, 0, "repeat press emits nothing");

	/* 16-bit axes carry what an 8-bit one would saturate. */
	sink_reset();
	core_mouse_move(&cs, 5000, -5000);
	check_eq((s16)(g_Sink[0].payload[CORE_MS_X] |
	               (g_Sink[0].payload[CORE_MS_X + 1] << 8)), 5000,
	         "a 5000 pixel delta survives the wire");

	/* Buttons outside the declared range are ignored, not clamped. */
	sink_reset();
	core_mouse_button(&cs, 9, 1);
	check_eq((long)g_SinkCount, 0, "button 9 ignored");

	return 0;
}

/* ======================================================================
 * THE REPORT QUEUE
 * ====================================================================== */

static int test_queue(void)
{
	static XC_DEVEXT devext;
	XC_REPORT_NODE   node;
	u8               payload[CORE_GAMEPAD_PAYLOAD];
	int              i;

	XcDevExtInit(&devext);
	memset(payload, 0, sizeof(payload));

	/*
	 * GAMEPAD REPORTS COALESCE. Without this, an application opening the
	 * collection after a quiet period is handed a burst of history before
	 * it ever sees the present.
	 */
	for (i = 0; i < 10; i++) {
		payload[0] = (u8)i;
		XcQueueReport(&devext, CORE_REPORT_ID_GAMEPAD, payload,
		              CORE_GAMEPAD_PAYLOAD);
	}
	check_eq((long)devext.ReportCount, 1, "ten gamepad reports coalesce");
	check(XcDequeueReport(&devext, &node), "and it dequeues");
	check_eq(node.Data[1], 9, "the survivor is the NEWEST state");
	check_eq((long)devext.ReportCount, 0, "queue drains");

	/*
	 * KEYBOARD REPORTS DO NOT COALESCE. A down and its up carry meaning
	 * only in sequence; merging them loses the keystroke entirely.
	 */
	XcDevExtInit(&devext);
	for (i = 0; i < 4; i++) {
		payload[0] = (u8)i;
		XcQueueReport(&devext, CORE_REPORT_ID_KEYBOARD, payload,
		              CORE_KEYBOARD_PAYLOAD);
	}
	check_eq((long)devext.ReportCount, 4, "four keyboard reports stay four");
	check(XcDequeueReport(&devext, &node), "first dequeues");
	check_eq(node.Data[1], 0, "in order, oldest first");

	/*
	 * A FULL QUEUE DROPS THE NEWEST. Older transitions are already
	 * committed and dropping one desynchronises the host's idea of what
	 * is held; the newest state can be re-derived next packet.
	 */
	XcDevExtInit(&devext);
	for (i = 0; i < XC_REPORT_QUEUE_MAX + 5; i++) {
		payload[0] = (u8)i;
		XcQueueReport(&devext, CORE_REPORT_ID_KEYBOARD, payload,
		              CORE_KEYBOARD_PAYLOAD);
	}
	check_eq((long)devext.ReportCount, XC_REPORT_QUEUE_MAX, "queue is full");
	check_eq((long)devext.ReportsDropped, 5, "the overflow was dropped");
	check(XcDequeueReport(&devext, &node), "oldest still present");
	check_eq(node.Data[1], 0, "and it is the OLDEST, not the newest");

	return 0;
}

/* ======================================================================
 * END TO END
 *
 * A packet arriving through the transport path must reach the queue as a
 * gamepad report, with the raw bytes riding along in the diagnostic tail.
 * ====================================================================== */

static int test_transport(void)
{
	static XC_DEVEXT devext;
	XC_REPORT_NODE   node;
	u8               packet[CORE_RAW_PACKET_BYTES];
	NTSTATUS         status;

	XcDevExtInit(&devext);
	status = XcStartDevice((PDEVICE_OBJECT)&devext, NULL);

	/*
	 * NEVER STATUS_PENDING. This value is what XcPnp writes into the
	 * start IRP before completing it, and an IRP completed with
	 * STATUS_PENDING makes the PnP manager wait forever for a
	 * completion that already happened - which stops every device
	 * operation on the machine, because PnP is serialised. Verifier
	 * calls it 0xC9 arg1=6; without Verifier the guest simply freezes
	 * the moment the pad is plugged in.
	 */
	check(status != STATUS_PENDING,
	      "StartDevice never reports STATUS_PENDING");
	check_eq((long)status, (long)STATUS_SUCCESS,
	         "StartDevice reports plain success");

	check(devext.Started, "device started");
	check_eq((long)devext.PollStopMask, 0, "no stop reason outstanding");
	check(devext.Poll[0].Active && devext.Poll[1].Active,
	      "BOTH poll slots are in flight");

	/* A failed transfer is counted, not decoded. */
	make_packet(packet);
	XcOnTransfer(&devext, STATUS_UNSUCCESSFUL, packet,
	             CORE_RAW_PACKET_BYTES, 1000);
	check_eq((long)devext.PollErrors, 1, "failed transfer counted");
	check_eq((long)devext.ReportCount, 0, "and produced nothing");

	/* So is a short one, even with a successful status. */
	XcOnTransfer(&devext, STATUS_SUCCESS, packet, 12, 2000);
	check_eq((long)devext.PollErrors, 2, "short transfer counted");
	check_eq((long)devext.ReportCount, 0, "and produced nothing");

	/*
	 * A FAILED TRANSFER MUST NOT BE RESUBMITTED ON THE CALLER'S STACK.
	 * The bus driver fails a submit to a departed device synchronously, so
	 * resubmitting from the completion routine re-enters it and does not
	 * stop. Recovery is asked for and happens elsewhere.
	 */
	{
		ULONG before = devext.PollRestartRequests;

		XcPollFinish(&devext, 0, STATUS_DEVICE_NOT_CONNECTED);
		check(!devext.Poll[0].Active,
		      "a failed slot goes inactive, not straight back out");
		check_eq((long)(devext.PollRestartRequests - before), 1,
		         "and recovery is REQUESTED rather than done inline");
	}

	/*
	 * THE PIPE IS ONLY RESET WHILE NOTHING IS ON IT. Slot 1 is still
	 * in flight at this point, so the reset has to wait; resetting a
	 * pipe with an outstanding transfer is not a meaningful request.
	 */
	check(!XcPollAllIdle(&devext),
	      "one slot down does not make the pipe idle");
	XcPollFinish(&devext, 1, STATUS_DEVICE_NOT_CONNECTED);
	check(XcPollAllIdle(&devext), "both slots down does");
	check_eq((long)devext.PipeResets, 0,
	         "and nothing has reset the pipe on the failure path");

	/* A good packet reaches the queue. */
	packet[CORE_RAW_DIGITAL] = CORE_DIG_BACK;
	XcOnTransfer(&devext, STATUS_SUCCESS, packet,
	             CORE_RAW_PACKET_BYTES, 3000);
	check_eq((long)devext.ReportCount, 1, "one report queued");
	check(XcDequeueReport(&devext, &node), "it dequeues");
	check_eq(node.Data[0], CORE_REPORT_ID_GAMEPAD, "report ID leads");
	check_eq(node.Length, CORE_GAMEPAD_PAYLOAD + 1, "ID plus payload");
	check_eq(node.Data[1 + CORE_GP_LAYOUT], 1, "layout is 1-based");
	check_eq(node.Data[1 + CORE_GP_RAW + CORE_RAW_DIGITAL], CORE_DIG_BACK,
	         "the raw packet rides along in the diagnostic tail");

	/*
	 * AXIS ORDER IS X, Y, Rx, Ry, Z, Rz. The right stick must land on
	 * Rx/Ry - anything that auto-maps a gamepad expects it there - and
	 * the triggers on Z/Rz, which is the only way their pressure
	 * reaches an application at all.
	 */
	make_packet(packet);
	put_le16(&packet[CORE_RAW_RSTICK_X], 32767);
	packet[CORE_RAW_ANALOG_BASE + 6] = 255;       /* left trigger  */
	packet[CORE_RAW_ANALOG_BASE + 7] = 128;       /* right trigger */
	XcOnTransfer(&devext, STATUS_SUCCESS, packet,
	             CORE_RAW_PACKET_BYTES, 4000);
	check(XcDequeueReport(&devext, &node), "report queued");
	{
		const u8 *ax = &node.Data[1 + CORE_GP_AXES];
		s16 x  = (s16)(ax[0] | (ax[1] << 8));
		s16 rx = (s16)(ax[4] | (ax[5] << 8));
		s16 z  = (s16)(ax[8] | (ax[9] << 8));
		s16 rz = (s16)(ax[10] | (ax[11] << 8));

		check(rx > 32000, "right stick X drives Rx");
		check_eq(x, 0, "and leaves the left stick alone");
		check(z > 32000, "left trigger drives Z, full scale");
		check(rz > 15000 && rz < 17500,
		      "right trigger drives Rz, about half");
		check(z >= 0 && rz >= 0,
		      "a trigger is unipolar - it never goes negative");
	}

	/* Buttons follow XBCD numbering: Start is 7, the triggers 11 and
	 * 12, and the D-pad is not a button at all. */
	make_packet(packet);
	packet[CORE_RAW_DIGITAL] = CORE_DIG_START | CORE_DIG_DPAD_LEFT;
	packet[CORE_RAW_ANALOG_BASE + 6] = 255;       /* left trigger  */
	XcOnTransfer(&devext, STATUS_SUCCESS, packet,
	             CORE_RAW_PACKET_BYTES, 5000);
	check(XcDequeueReport(&devext, &node), "report queued");
	{
		const u8 *b = &node.Data[1 + CORE_GP_BUTTONS];
		u16 mask = (u16)(b[0] | (b[1] << 8));

		check(mask & (1u << 6),  "Start is button 7");
		check(mask & (1u << 10), "left trigger is button 11");
		check_eq(mask & 0xF000, 0, "buttons 13..16 stay spare");
		check_eq(node.Data[1 + CORE_GP_HAT], 6,
		         "D-pad left reads as hat West");
	}

	/* Stopping releases the engine and disarms the tick. */
	XcStopDevice(&devext);
	check(!devext.Started, "device stopped");
	check(!devext.TickArmed, "tick disarmed");

	return 0;
}

/* ======================================================================
 * TEARDOWN
 *
 * IoCancelIrp only ASKS; the bus driver completes the transfer some time
 * later. Freeing the IRP on the strength of having called it means the
 * completion lands in freed memory - a bugcheck on unplug, which is the
 * single most likely thing to happen during testing.
 * ====================================================================== */

static int test_teardown(void)
{
	static XC_DEVEXT devext;

	XcDevExtInit(&devext);
	check_eq(devext.IoCount, 1, "one reference, held by the device");

	XcStartDevice((PDEVICE_OBJECT)&devext, NULL);
	check_eq(devext.IoCount, 3, "plus one per transfer in flight");

	XcRemoveDevice(&devext);
	check_eq(devext.IoCount, 0, "teardown waits for every one of them");
	check(devext.IoDraining, "and refuses new ones");
	check(devext.Removed, "device marked removed");

	/* A completion arriving mid-teardown must not restart polling. */
	check(!XcIoAcquire(&devext), "a late submit is refused");
	check_eq((long)XcPollSubmit(&devext, 0), (long)STATUS_DELETE_PENDING,
	         "and XcPollSubmit says why");
	check(!devext.Poll[0].Active, "the slot stays inactive");

	/* Draining twice must not double-decrement the device's reference. */
	XcIoDrainAndWait(&devext);
	check_eq(devext.IoCount, 0, "a second drain is a no-op");

	return 0;
}

/* ======================================================================
 * POWER
 *
 * A pad that suspends mid-keypress must not leave the keyboard holding
 * that key. Nothing else can release it: the release would have come from
 * a packet, and no more packets are coming.
 * ====================================================================== */

static int test_power(void)
{
	static XC_DEVEXT devext;
	XC_REPORT_NODE   node;

	XcDevExtInit(&devext);
	XcStartDevice((PDEVICE_OBJECT)&devext, NULL);
	check_eq(devext.IoCount, 3, "two transfers in flight");

	/* Hold a key down, and clear the reports that produced. */
	core_key_event(&devext.Core, 0x1A, 1);          /* W down */
	while (XcDequeueReport(&devext, &node)) {
		/* drain */
	}

	XcPowerDown(&devext);
	check(devext.PollStopMask & XC_STOP_POWER_DOWN,
	      "suspending stops the poll engine");
	check(XcPollAllIdle(&devext), "with no transfer left in flight");
	check_eq(devext.IoCount, 1, "and their references released");

	check(XcDequeueReport(&devext, &node),
	      "suspending emits a report of its own");
	check_eq(node.Data[0], CORE_REPORT_ID_KEYBOARD, "a keyboard report");
	check_eq(node.Data[1 + CORE_KB_KEYS], 0, "WITH THE HELD KEY RELEASED");

	XcPowerUp(&devext);
	check_eq((long)devext.PollStopMask, 0, "resuming restarts polling");
	check_eq(devext.IoCount, 3, "both slots back in flight");

	/* And a suspend with nothing held is not news. */
	XcPowerDown(&devext);
	while (XcDequeueReport(&devext, &node)) {
		/* drain */
	}
	XcPowerUp(&devext);
	XcPowerDown(&devext);
	check_eq((long)devext.ReportCount, 0,
	         "suspending with nothing held emits nothing");

	return 0;
}

/* ======================================================================
 * THE REGISTRATION CONTRACT
 * ====================================================================== */

static int test_driver_entry(void)
{
	static DRIVER_OBJECT driver;
	NTSTATUS             status;

	memset(&driver, 0, sizeof(driver));
	status = DriverEntry(&driver, NULL);

	check(NT_SUCCESS(status), "DriverEntry succeeds");
	check(driver.MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] != NULL,
	      "the HID minidriver dispatch is installed");
	check(driver.MajorFunction[IRP_MJ_PNP] != NULL, "PnP is installed");
	check(driver.MajorFunction[IRP_MJ_POWER] != NULL, "power is installed");
	check(driver.DriverUnload != NULL, "unload is installed");

	return 0;
}

/* ====================================================================== */

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("xboxctl harness\n");
	printf("---------------\n");

	printf("[descriptor]\n");   test_descriptor();
	printf("[radial]\n");       test_radial();
	printf("[decode]\n");       test_decode();
	printf("[keyboard]\n");     test_keyboard();
	printf("[mouse]\n");        test_mouse();
	printf("[queue]\n");        test_queue();
	printf("[transport]\n");    test_transport();
	printf("[teardown]\n");     test_teardown();
	printf("[power]\n");        test_power();
	printf("[registration]\n"); test_driver_entry();

	printf("---------------\n");
	printf("%d checks, %d failure(s)\n", g_Checks, g_Failures);

	return (g_Failures == 0) ? 0 : 1;
}
