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

/*
 * The harness is single threaded, so a fast mutex has nothing to
 * exclude. It still counts, because a handler that returns while
 * holding it is a deadlock in the driver and a silent pass here
 * otherwise - test_control asserts the count is zero at the end.
 */
long g_MutexDepth;
void ExInitializeFastMutex(PFAST_MUTEX M) { M->Held = 0; }
void ExAcquireFastMutex(PFAST_MUTEX M)    { M->Held = 1; g_MutexDepth++; }
void ExReleaseFastMutex(PFAST_MUTEX M)    { M->Held = 0; g_MutexDepth--; }

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

/*
 * TOTALS THAT THE RING CANNOT LOSE, and a count of what it did.
 *
 * g_Sink holds SINK_MAX records and silently drops the rest. A test that
 * runs for a second at 250 Hz produces far more than that, so summing the
 * ring measures the first 64 reports and calls it a second - it does not
 * fail, it quietly reports a quarter of the right answer. Anything
 * measuring a total over a long run must use these.
 */
static long g_MouseTotalX;
static long g_MouseTotalY;
static int  g_SinkDropped;

static void sink_reset(void)
{
	g_MouseTotalX = 0;
	g_MouseTotalY = 0;
	g_SinkDropped = 0;
	g_SinkCount = 0;
}

static void recording_sink(void *ctx, u8 id, const u8 *payload, u32 len)
{
	(void)ctx;

	if (id == CORE_REPORT_ID_MOUSE && len > CORE_MS_Y + 1) {
		g_MouseTotalX += (s16)(payload[CORE_MS_X] |
		                       (payload[CORE_MS_X + 1] << 8));
		g_MouseTotalY += (s16)(payload[CORE_MS_Y] |
		                       (payload[CORE_MS_Y + 1] << 8));
	}

	if (g_SinkCount >= SINK_MAX) {
		g_SinkDropped++;
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
		/*
		 * THE MOUSE COMES FIRST, AND THE ORDER IS LOAD-BEARING.
		 * With the gamepad declared first, this same mouse
		 * collection - byte for byte - made the pointer jump 157
		 * pixels diagonally on an all-zero report and produced
		 * phantom keystrokes and wheel events. Mouse first is the
		 * order the Adaptoid uses and the order that works.
		 */
		check_eq(top_usages[0], 0x02, "Col01 is a Mouse");
		check_eq(top_usages[1], 0x06, "Col02 is a Keyboard");
		check_eq(top_usages[2], 0x05, "Col03 is a Gamepad");
	}

	/*
	 * FOUR, NOT SIX. Configuration does not travel over HID, so the
	 * descriptor declares no feature report: 1 gamepad, 2 keyboard,
	 * 3 mouse, 4 rumble, and nothing else.
	 */
	check_eq(n_ids, 4, "four distinct report IDs");

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

	/*
	 * A DELTA LARGER THAN ONE BYTE IS CARRIED, NOT CLAMPED. One report
	 * takes 127 counts and the rest waits for the next one, so the
	 * pointer travels the whole distance; only the delivery is spread.
	 */
	sink_reset();
	core_mouse_move(&cs, 40000, -40000);
	check_eq((s16)(g_Sink[0].payload[CORE_MS_X] |
	               (g_Sink[0].payload[CORE_MS_X + 1] << 8)),
	         CORE_MS_STEP_MAX,
	         "a delta wider than the axis fills one report to the brim");
	check_eq((long)cs.ms.dx, 40000 - CORE_MS_STEP_MAX,
	         "AND THE REMAINDER IS KEPT");

	sink_reset();
	core_mouse_move(&cs, 0, 0);
	check_eq((long)g_SinkCount, 0,
	         "a zero move still emits nothing, even with a remainder"
	         " pending - the next real report carries it");
	sink_reset();
	core_mouse_move(&cs, 1, -1);
	check_eq((s16)(g_Sink[0].payload[CORE_MS_X] |
	               (g_Sink[0].payload[CORE_MS_X + 1] << 8)),
	         40000 - CORE_MS_STEP_MAX + 1,
	         "and the next report carries the whole remainder, which now"
	         " fits");
	check_eq((long)cs.ms.dx, 0, "leaving nothing owed");

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
 * CONFIGURATION
 *
 * The blob arrives from user mode and is parsed at DISPATCH_LEVEL. There is
 * no second chance and no way to ask the sender what it meant, so every
 * check below is a check the driver makes before it trusts a byte.
 * ====================================================================== */

static u32 cfg_blob_len(const core_config *cfg)
{
	(void)cfg;
	return (u32)sizeof(core_config_header) +
	       (u32)sizeof(core_stick) * CORE_STICK_COUNT +
	       (u32)sizeof(core_layout) * CORE_MAX_LAYOUTS;
}

static int test_config(void)
{
	static core_config cfg;
	static core_config back;
	static u8          blob[4096];
	u32                len, repaired;
	int                rc;
	u32                i;

	/* --- the wire format is the size the document states --------- */
	check_eq((long)sizeof(core_binding), 12, "BINDING is 12 bytes");
	check_eq((long)sizeof(core_chord), 4, "CHORD is 4 bytes");
	check_eq((long)sizeof(core_stick), 88, "STICK is 88 bytes");
	check_eq((long)sizeof(core_layout), 420, "LAYOUT is 420 bytes");
	check_eq((long)sizeof(core_config_header), 32, "HEADER is 32 bytes");

	/* --- defaults ------------------------------------------------ */
	core_config_defaults(&cfg);
	check(cfg.valid, "the built-in configuration is valid");
	check_eq(cfg.layout_count, 2, "two layouts, not eight");

	check_eq(cfg.layout[0].chord[0].member[0], CORE_SA_START,
	         "chord 0 is Start");
	check_eq(cfg.layout[0].chord[0].member[1], CORE_SA_BACK,
	         "plus Back");
	check_eq(cfg.layout[0].binding[0].action, CORE_ACT_LAYER_CYCLE,
	         "which cycles the layer");
	check_eq(cfg.layout[1].binding[0].action, CORE_ACT_LAYER_CYCLE,
	         "IN BOTH LAYOUTS, or there is no way back");

	check_eq(cfg.layout[1].binding[1].source, CORE_SA_A,
	         "layer 2 binds A");
	check(cfg.layout[1].binding[1].flags & CORE_BF_REPEAT,
	      "with autofire");
	check_eq(cfg.layout[1].binding[1].repeat_hz, 12, "at 12 Hz");

	/* --- the suppression mask, section 4.1 ------------------------ */
	check_eq((long)cfg.suppress[0], 0,
	         "layer 1 suppresses nothing - the pad is stock");
	check(cfg.suppress[1] & (1u << CORE_SA_A),
	      "layer 2 suppresses A, whose binding replaces its default");
	check(!(cfg.suppress[1] & (1u << CORE_SA_START)),
	      "BUT NOT START: a chord member keeps its own button");
	check(!(cfg.suppress[1] & (1u << CORE_SA_BLACK)),
	      "nor Black, which nothing binds");

	/* PASSTHROUGH takes a source back out of the mask. */
	cfg.layout[1].binding[1].flags |= CORE_BF_PASSTHROUGH;
	core_config_suppress(&cfg);
	check(!(cfg.suppress[1] & (1u << CORE_SA_A)),
	      "PASSTHROUGH keeps the default gamepad output as well");
	cfg.layout[1].binding[1].flags &= (u8)~CORE_BF_PASSTHROUGH;
	core_config_suppress(&cfg);

	/* --- round trip ----------------------------------------------- */
	len = core_config_save(&cfg, blob, sizeof(blob));
	check_eq((long)len, (long)cfg_blob_len(&cfg),
	         "a saved blob is 1048 bytes");
	check_eq((long)core_config_save(&cfg, blob, len - 1), 0,
	         "and refuses to write into a buffer one byte short");

	rc = core_config_load(&back, blob, len, &repaired);
	check_eq(rc, CORE_CFG_OK, "it loads again");
	check_eq((long)repaired, 0, "with nothing to repair");
	check_eq((long)back.suppress[1], (long)cfg.suppress[1],
	         "and the mask is rebuilt identically");
	check_eq(back.layout[1].binding[1].repeat_hz, 12,
	         "load(save(x)) == x");

	/* --- structural rejection ------------------------------------- */
	check_eq(core_config_load(&back, blob, 8, NULL), CORE_CFG_ERR_SHORT,
	         "a blob shorter than a header is refused");

	blob[0] ^= 0xFF;
	check_eq(core_config_load(&back, blob, len, NULL),
	         CORE_CFG_ERR_SIGNATURE, "a bad signature is refused");
	blob[0] ^= 0xFF;

	blob[4] = 0; blob[5] = 0;              /* version 0 */
	check_eq(core_config_load(&back, blob, len, NULL),
	         CORE_CFG_ERR_VERSION, "an older version is refused");
	blob[4] = CORE_CFG_VERSION;

	blob[8] = 1; blob[9] = 0;              /* layout_bytes = 1 */
	check_eq(core_config_load(&back, blob, len, NULL),
	         CORE_CFG_ERR_STRIDE, "a stride below the structure is refused");
	blob[8] = (u8)(sizeof(core_layout) & 0xFF);
	blob[9] = (u8)(sizeof(core_layout) >> 8);

	blob[12] = 8;                          /* layout_count = 8 */
	check_eq(core_config_load(&back, blob, len, NULL),
	         CORE_CFG_ERR_COUNT,
	         "eight layouts is REFUSED, not silently clamped to two");
	blob[12] = 2;

	check_eq(core_config_load(&back, blob, len - 1, NULL),
	         CORE_CFG_ERR_TRUNCATED,
	         "a blob that ends before its own header says is refused");

	/*
	 * A REJECTED BLOB MUST NOT DISTURB A RUNNING MAP. back still holds
	 * the good configuration from the round trip above.
	 */
	check_eq(back.layout[1].binding[1].repeat_hz, 12,
	         "and none of those rejections touched the loaded config");

	/* --- a newer blob still loads, by its strides ------------------ */
	memset(blob, 0, sizeof(blob));
	len = core_config_save(&cfg, blob, sizeof(blob));
	blob[4] = 99;                          /* version 99 */
	check_eq(core_config_load(&back, blob, len, &repaired), CORE_CFG_OK,
	         "A NEWER VERSION LOADS: forward compatibility runs on the"
	         " strides, not on the version");

	/* --- value repair, not rejection ------------------------------- */
	core_config_defaults(&cfg);
	cfg.layout[0].binding[2].source    = CORE_SA_A;
	cfg.layout[0].binding[2].action    = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[2].code      = 900;      /* only 16 buttons */
	cfg.layout[0].binding[3].source    = CORE_SA_B;
	cfg.layout[0].binding[3].action    = CORE_ACT_KEY;
	cfg.layout[0].binding[3].code      = 0x0300;   /* not a usage */
	cfg.layout[0].binding[4].source    = CORE_SA_X;
	cfg.layout[0].binding[4].action    = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[4].code      = 1;
	cfg.layout[0].binding[4].flags     = CORE_BF_REPEAT;
	cfg.layout[0].binding[4].repeat_hz = 10;
	cfg.layout[0].binding[4].hard_at   = 60000;    /* past full scale */
	cfg.layout[0].binding[5].source    = CORE_SA_Y;
	cfg.layout[0].binding[5].action    = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[5].code      = 1;
	cfg.layout[0].binding[5].repeat_hz = 200;      /* above tick/2 */
	cfg.stick[0].outer                 = 1000;
	cfg.stick[0].deadzone              = 2000;     /* inverted */
	cfg.stick[1].curve[10]             = 0;        /* non-monotone */

	len = core_config_save(&cfg, blob, sizeof(blob));
	rc  = core_config_load(&back, blob, len, &repaired);
	check_eq(rc, CORE_CFG_OK, "a blob full of bad VALUES still loads");
	check(repaired >= 6, "with every one of them repaired");

	check_eq(back.layout[0].binding[2].code, CORE_GP_BUTTON_COUNT,
	         "a button of 900 is clamped to the descriptor's 16");
	check_eq(back.layout[0].binding[3].action, CORE_ACT_NONE,
	         "a key that is not a usage is dropped, not clamped");
	check_eq(back.layout[0].binding[4].hard_at, CORE_MAX_VALUE,
	         "a hard point past full scale is clamped to it - left"
	         " alone it could never be reached, so the autofire it was"
	         " asked for would silently never run");
	check_eq(back.layout[0].binding[5].repeat_hz, CORE_MAX_REPEAT_HZ,
	         "autofire is capped at half the TICK rate, which is what"
	         " the engine can actually produce - a cycle needs one tick"
	         " to assert and one to release");
	check(back.stick[0].outer > back.stick[0].deadzone,
	      "outer is pushed above deadzone so the rescale cannot"
	      " divide by zero");
	for (i = 1; i < CORE_CURVE_POINTS; i++) {
		if (back.stick[1].curve[i] < back.stick[1].curve[i - 1]) {
			break;
		}
	}
	check_eq((long)i, CORE_CURVE_POINTS,
	         "and the curve is monotone again");

	/* An out-of-range source cannot be left able to fire. */
	core_config_defaults(&cfg);
	cfg.layout[0].binding[2].source = 200;
	cfg.layout[0].binding[2].action = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[2].code   = 1;
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_config_load(&back, blob, len, &repaired);
	check_eq(back.layout[0].binding[2].action, CORE_ACT_NONE,
	         "a source that names nothing real is disarmed");

	return 0;
}

/* ======================================================================
 * BINDINGS
 *
 * mapping-engine.txt sections 4.1, 5 and 9. The pad's face buttons are
 * analog, so the tests below press them to a PRESSURE out of 255 rather
 * than to an on or an off, which is also how hard_at is reached.
 * ====================================================================== */

/* Install a configuration built in memory rather than parsed from a blob,
 * so a test can state exactly one binding and nothing else. */
static void bind_install(core_state *cs, core_config *cfg)
{
	u8  blob[4096];
	u32 len;

	core_config_suppress(cfg);
	len = core_config_save(cfg, blob, sizeof(blob));
	check(len > 0, "the test configuration serialises");
	check_eq(core_set_config(cs, blob, len, NULL), CORE_CFG_OK,
	         "and installs");
}

/* A configuration with one layout, no chords and a single binding. */
static void bind_one(core_config *cfg, u8 source, u8 action, u16 code,
                     u8 flags)
{
	core_config_defaults(cfg);
	cfg->layout[0].binding[0].source = source;
	cfg->layout[0].binding[0].action = action;
	cfg->layout[0].binding[0].code   = code;
	cfg->layout[0].binding[0].flags  = flags;
}

/*
 * Total wheel movement across every mouse report since the last
 * sink_reset. IT CANNOT BE READ OFF cs.ms.wheel: movement is relative,
 * so the accumulator is zeroed the instant it is reported.
 */
static long sink_wheel_total(void)
{
	long total = 0;
	int  i;

	for (i = 0; i < g_SinkCount; i++) {
		if (g_Sink[i].id == CORE_REPORT_ID_MOUSE) {
			total += (s8)g_Sink[i].payload[CORE_MS_WHEEL];
		}
	}
	return total;
}

/* Drive one analog face button to a pressure and deliver the packet. */
static void bind_press(core_state *cs, u8 *packet, int analog_index,
                       int pressure, u64 when)
{
	packet[CORE_RAW_ANALOG_BASE + analog_index] = (u8)pressure;
	core_on_packet(cs, packet, CORE_RAW_PACKET_BYTES, when);
}

static int test_bindings(void)
{
	static core_config cfg;
	core_state         cs;
	u8                 packet[CORE_RAW_PACKET_BYTES];
	u64                t = 1000;

	/* --- a key binding fires on ANY pressure at all ---------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_KEY, 0x1A, 0);   /* A -> W */
	bind_install(&cs, &cfg);

	make_packet(packet);
	sink_reset();
	bind_press(&cs, packet, 0, 0, t += 4000);
	check_eq(cs.kb.count, 0, "at rest no key is held");

	/*
	 * THE LIGHTEST TOUCH THE PAD CAN REPORT TYPES THE KEY. One count in
	 * 255 is the smallest non-zero pressure there is, and a threshold
	 * of any size at all would swallow it. These buttons rest at
	 * exactly zero, so there is no noise for a threshold to reject -
	 * it would only make the button need a shove.
	 */
	bind_press(&cs, packet, 0, 1, t += 4000);
	check_eq(cs.kb.count, 1, "one count of 255 already types it");
	check_eq(cs.kb.keys[0], 0x1A, "and it is W");

	bind_press(&cs, packet, 0, 200, t += 4000);
	check_eq(cs.kb.count, 1, "pressing harder changes nothing");

	bind_press(&cs, packet, 0, 0, t += 4000);
	check_eq(cs.kb.count, 0, "and only zero releases it");

	/* --- WHICH SEMIAXIS EACH PHYSICAL PUSH LANDS IN --------------- */
	{
		/*
		 * PINNED, because the answer is counter-intuitive and a name
		 * table has already been written against the wrong one. The pad
		 * reports Y up-positive; the decode negates it once so the value
		 * is down-positive the way HID wants. A physical UP push
		 * therefore lands in the semiaxis named YNEG, and anything
		 * calling YPOS "up" fires on the wrong thumb direction.
		 */
		core_init(&cs, recording_sink, NULL);
		core_config_defaults(&cfg);
		bind_install(&cs, &cfg);
		make_packet(packet);

		put_le16(&packet[CORE_RAW_RSTICK_Y], 32767);
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
		check(cs.semiaxis[CORE_SA_RSTICK_YNEG] > CORE_MAX_VALUE - 200,
		      "PUSHING UP FILLS YNEG, not YPOS - the decode negates Y so"
		      " the value is down-positive, which is what HID wants and"
		      " the opposite of what the name suggests");
		check_eq(cs.semiaxis[CORE_SA_RSTICK_YPOS], 0, "and YPOS stays 0");

		put_le16(&packet[CORE_RAW_RSTICK_Y], -32767);
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
		check(cs.semiaxis[CORE_SA_RSTICK_YPOS] > CORE_MAX_VALUE - 200,
		      "pushing DOWN fills YPOS");
		check_eq(cs.semiaxis[CORE_SA_RSTICK_YNEG], 0, "and YNEG stays 0");
		put_le16(&packet[CORE_RAW_RSTICK_Y], 0);

		/* X NEEDS NO SUCH WARNING: right is XPOS, as it reads. */
		put_le16(&packet[CORE_RAW_RSTICK_X], 32767);
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
		check(cs.semiaxis[CORE_SA_RSTICK_XPOS] > CORE_MAX_VALUE - 200,
		      "pushing RIGHT fills XPOS, which needs no explaining");
		check_eq(cs.semiaxis[CORE_SA_RSTICK_XNEG], 0, "and XNEG stays 0");
		put_le16(&packet[CORE_RAW_RSTICK_X], 0);
	}

	/* --- A STICK IS THE ONE THING THAT STILL NEEDS A DEADZONE ----- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_RSTICK_XPOS, CORE_ACT_KEY, 0x07, 0); /* D */
	cfg.stick[1].deadzone = 7000;
	bind_install(&cs, &cfg);
	make_packet(packet);

	/*
	 * A STICK AT REST IS NOT AT ZERO. This pad's right stick sits some
	 * thousands of units off centre with nothing touching it, so a
	 * direction bound to a key would type it forever if non-zero alone
	 * meant pressed. The stick's own deadzone is what makes that false,
	 * and it is the only deadzone left anywhere.
	 */
	put_le16(&packet[CORE_RAW_RSTICK_X], 3900);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
	check_eq(cs.kb.count, 0,
	         "a stick resting off centre types nothing");

	put_le16(&packet[CORE_RAW_RSTICK_X], 32767);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
	check_eq(cs.kb.count, 1, "and pushing it past the deadzone types D");

	put_le16(&packet[CORE_RAW_RSTICK_X], 3900);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
	check_eq(cs.kb.count, 0, "letting go releases it again");
	put_le16(&packet[CORE_RAW_RSTICK_X], 0);

	/* THE GAMEPAD BUTTON COMES ON AT THE SAME POINT. A binding and the
	 * default map used to disagree by a factor of ten, so the same
	 * press that lit the button in joy.cpl was not enough to type. */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	core_config_suppress(&cfg);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 1, t += 4000);
	check(cs.gp.buttons & 0x0001,
	      "one count of 255 lights the gamepad button too");
	bind_press(&cs, packet, 0, 0, t += 4000);
	check(!(cs.gp.buttons & 0x0001), "and zero puts it out");

	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_KEY, 0x1A, 0);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 255, t += 4000);

	/* --- suppression, section 4.1 ---------------------------------- */
	check(!(cs.gp.buttons & 0x0001),
	      "A drives no gamepad button while bound to a key");
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.kb.count, 1, "A at full pressure types W");
	check(!(cs.gp.buttons & 0x0001),
	      "AND STILL DRIVES NO BUTTON - the binding replaced the"
	      " default, it did not add to it");
	check(cs.gp.buttons == 0, "nothing else is pressed either");

	/* --- PASSTHROUGH puts the default back ------------------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_KEY, 0x1A, CORE_BF_PASSTHROUGH);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.kb.count, 1, "PASSTHROUGH still types the key");
	check(cs.gp.buttons & 0x0001, "and keeps gamepad button 1 as well");

	/* --- an unbound source is untouched ---------------------------- */
	bind_press(&cs, packet, 1, 255, t += 4000);     /* B */
	check(cs.gp.buttons & 0x0002,
	      "B, which nothing binds, drives button 2 as always");

	/* --- remapping one gamepad button to another ------------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_JOY_BUTTON, 5, 0);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 255, t += 4000);
	check(cs.gp.buttons & 0x0010, "A pressed reads as button 5");
	check(!(cs.gp.buttons & 0x0001), "and no longer as button 1");

	/* --- TOGGLE latches on the rising edge ------------------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_KEY, 0x1A, CORE_BF_TOGGLE);
	bind_install(&cs, &cfg);
	make_packet(packet);

	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.kb.count, 1, "a toggle press latches the key down");
	bind_press(&cs, packet, 0, 0, t += 4000);
	check_eq(cs.kb.count, 1, "AND RELEASING THE BUTTON DOES NOT LIFT IT");
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.kb.count, 0, "the second press lets go");

	/* --- a wheel detent is an event, not a state ------------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_MOUSE_WHEEL, 1, 0);
	bind_install(&cs, &cfg);
	make_packet(packet);
	sink_reset();

	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(sink_wheel_total(), 1, "holding A scrolls one detent");
	bind_press(&cs, packet, 0, 254, t += 4000);
	bind_press(&cs, packet, 0, 253, t += 4000);
	check_eq(sink_wheel_total(), 1,
	         "AND KEEPS SCROLLING NO FURTHER while it is held - a"
	         " per-packet detent would scroll at 250 Hz");
	bind_press(&cs, packet, 0, 0, t += 4000);
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(sink_wheel_total(), 2,
	         "releasing and pressing scrolls again");

	/* --- a mouse button is held, not pulsed ------------------------ */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_MOUSE_BUTTON, 1, 0);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.ms.buttons, 1, "A holds the left mouse button");
	bind_press(&cs, packet, 0, 0, t += 4000);
	check_eq(cs.ms.buttons, 0, "and releases it");

	/* --- a trigger driving an axis, pressure and all --------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_LTRIGGER, CORE_ACT_JOY_AXIS, 0,
	         CORE_BF_ANALOG);
	bind_install(&cs, &cfg);
	make_packet(packet);
	packet[CORE_RAW_ANALOG_BASE + 6] = 255;         /* left trigger */
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += 4000);
	check(cs.gp.axis[0] > CORE_MAX_VALUE - 200,
	      "ANALOG passes the trigger's pressure to the axis");

	/* --- a configuration swap lets go of what was held ------------- */
	core_init(&cs, recording_sink, NULL);
	bind_one(&cfg, CORE_SA_A, CORE_ACT_KEY, 0x1A, 0);
	bind_install(&cs, &cfg);
	make_packet(packet);
	bind_press(&cs, packet, 0, 255, t += 4000);
	check_eq(cs.kb.count, 1, "a key is held under the old map");

	sink_reset();
	bind_one(&cfg, CORE_SA_B, CORE_ACT_KEY, 0x04, 0);
	bind_install(&cs, &cfg);
	check_eq(cs.kb.count, 0,
	         "INSTALLING A NEW MAP RELEASES IT - no binding in the new"
	         " table has ever heard of that key");

	/* --- a rejected blob changes nothing --------------------------- */
	bind_press(&cs, packet, 1, 255, t += 4000);     /* B -> A key */
	check_eq(cs.kb.count, 1, "the new map is live");
	check_eq(core_set_config(&cs, packet, 8, NULL), CORE_CFG_ERR_SHORT,
	         "a rubbish blob is refused");
	check_eq(cs.kb.count, 1,
	         "and the refusal did not disturb what was held");

	return 0;
}

/* ======================================================================
 * THE CONTROL DEVICE
 *
 * ../docs/driver-plan.txt section 7. The IRP plumbing is not exercised
 * here - there is no device object in this build - but every command is,
 * and so is the one trap buffered I/O sets: input and output share a
 * buffer, so a reply destroys the request that asked for it.
 * ====================================================================== */

extern long g_MutexDepth;
extern LONG g_ControlDeviceAlive;
extern LONG g_ControlDeviceUnderLock;

static int test_control(void)
{
	static XC_DEVEXT devext;
	static u8        buffer[8192];
	static u8        blob[4096];
	static core_config cfg;
	XC_VERSION_INFO  version;
	XC_DEVICE_LIST   list;
	XC_STATS         stats;
	XC_CONFIG_REQUEST request;
	ULONG            written;
	u32              blob_len;
	NTSTATUS         status;

	XcDeviceRegistryReset();
	check_eq(g_MutexDepth, 0, "the device lock starts free");

	/* --- GET_VERSION works with no device attached ---------------- */
	written = 0xFFFFFFFF;
	status = XcControlCommand(IOCTL_XC_GET_VERSION, buffer, 0,
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS,
	         "GET_VERSION answers with no pad plugged in");
	check_eq((long)written, (long)sizeof(XC_VERSION_INFO),
	         "and writes one version structure");
	memcpy(&version, buffer, sizeof(version));
	check_eq((long)version.signature, (long)XC_CONFIG_SIGNATURE,
	         "the signature identifies the driver");
	check_eq(version.config_version, CORE_CFG_VERSION,
	         "it names the blob version it speaks");
	check_eq(version.max_layouts, CORE_MAX_LAYOUTS, "and its ceilings");
	check_eq(version.blob_bytes, 1048, "and the blob size it emits");

	/* A buffer too small to hold the answer is refused, not truncated. */
	status = XcControlCommand(IOCTL_XC_GET_VERSION, buffer, 0, 4, &written);
	check_eq(status, STATUS_BUFFER_TOO_SMALL,
	         "a short output buffer is REFUSED, never half-filled");
	check_eq((long)written, 0, "and nothing is reported as written");

	/* --- an unknown code is not a crash --------------------------- */
	status = XcControlCommand(0xDEADBEEF, buffer, 0, sizeof(buffer),
	                          &written);
	check_eq(status, STATUS_INVALID_DEVICE_REQUEST,
	         "an unknown control code is rejected");

	/* --- with no devices, the list is empty and indexes fail ------ */
	status = XcControlCommand(IOCTL_XC_GET_DEVICES, buffer, 0,
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "GET_DEVICES answers when empty");
	memcpy(&list, buffer, sizeof(list));
	check_eq((long)list.count, 0, "with a count of zero");

	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_GET_STATS, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_DEVICE_DOES_NOT_EXIST,
	         "and an index nothing occupies is refused");

	/* --- bring a device up ---------------------------------------- */
	XcDevExtInit(&devext);
	XcStartDevice((PDEVICE_OBJECT)&devext, NULL);

	status = XcControlCommand(IOCTL_XC_GET_DEVICES, buffer, 0,
	                          sizeof(buffer), &written);
	memcpy(&list, buffer, sizeof(list));
	check_eq((long)list.count, 1, "a started device appears in the list");
	check_eq((long)list.device[0].index, 0, "at index 0");
	check_eq(list.device[0].started, 1, "and reads as started");

	/* --- GET_RAW SEES WHAT THE PAD SENT, MAPPED OR NOT ------------- */
	{
		XC_RAW_INFO info;
		u8          packet[CORE_RAW_PACKET_BYTES];
		u32         k;
		u32         first_seq;

		request.index = 0;
		memcpy(buffer, &request, sizeof(request));
		status = XcControlCommand(IOCTL_XC_GET_RAW, buffer,
		                          sizeof(request), sizeof(buffer),
		                          &written);
		check_eq(status, STATUS_SUCCESS, "GET_RAW answers");
		check_eq((long)written, (long)sizeof(XC_RAW_INFO),
		         "with the whole structure");
		memcpy(&info, buffer, sizeof(info));
		check_eq((long)info.valid, 0,
		         "and says NOT VALID before any packet has arrived, rather"
		         " than handing back a zeroed packet that reads as a pad"
		         " sitting at rest");

		/*
		 * A CONTROL BOUND TO A KEY IS INVISIBLE ON THE GAMEPAD REPORT.
		 * The emit comparison stops short of the raw tail, so a press
		 * that changes nothing an application can see emits nothing at
		 * all - which is exactly the case a configurator asking "press
		 * the control you want to bind" runs into. GET_RAW is the answer
		 * and this is the test that it is.
		 */
		make_packet(packet);
		packet[CORE_RAW_ANALOG_BASE + 0] = 200;         /* A, hard */
		core_set_config_default(&devext.Core);
		{
			static core_config keycfg;
			u8                 keyblob[4096];
			u32                keylen;

			core_config_defaults(&keycfg);
			keycfg.layout[0].binding[1].source = CORE_SA_A;
			keycfg.layout[0].binding[1].action = CORE_ACT_KEY;
			keycfg.layout[0].binding[1].code   = 0x2C;      /* space */
			core_config_suppress(&keycfg);
			keylen = core_config_save(&keycfg, keyblob,
			                          (u32)sizeof(keyblob));
			core_set_config(&devext.Core, keyblob, keylen, NULL);
		}
		devext.ReportCount = 0;
		devext.ReportHead  = 0;
		core_on_packet(&devext.Core, packet, CORE_RAW_PACKET_BYTES,
		               1000000);

		memcpy(buffer, &request, sizeof(request));
		status = XcControlCommand(IOCTL_XC_GET_RAW, buffer,
		                          sizeof(request), sizeof(buffer),
		                          &written);
		memcpy(&info, buffer, sizeof(info));
		check_eq((long)info.valid, 1, "after a packet it reads valid");
		for (k = 0; k < CORE_RAW_PACKET_BYTES; k++) {
			if (info.packet[k] != packet[k]) {
				break;
			}
		}
		check_eq((long)k, (long)CORE_RAW_PACKET_BYTES,
		         "and every byte is the packet AS THE PAD SENT IT - no"
		         " deadzone, no curve, no suppression");
		check_eq(info.packet[CORE_RAW_ANALOG_BASE], 200,
		         "including a control bound to a key, whose press reaches"
		         " no gamepad report at all");
		check_eq((long)info.layer, 1, "and it says which layer is live");
		first_seq = info.sequence;
		check(first_seq > 0, "the sequence counts packets");

		/* AN IDENTICAL PACKET IS STILL A PACKET. */
		core_on_packet(&devext.Core, packet, CORE_RAW_PACKET_BYTES,
		               2000000);
		memcpy(buffer, &request, sizeof(request));
		status = XcControlCommand(IOCTL_XC_GET_RAW, buffer,
		                          sizeof(request), sizeof(buffer),
		                          &written);
		memcpy(&info, buffer, sizeof(info));
		check(info.sequence > first_seq,
		      "and a repeat of the same packet still advances it, so a"
		      " poller can tell a fresh report from a held control");
		check_eq((long)info.when_100ns, 2000000,
		         "with the arrival time of the newest one");

		/* A short request is refused, not padded. */
		status = XcControlCommand(IOCTL_XC_GET_RAW, buffer,
		                          sizeof(request) - 1, sizeof(buffer),
		                          &written);
		check_eq(status, STATUS_INVALID_PARAMETER,
		         "a short GET_RAW request is refused");

		/* And a buffer too small to hold the answer is refused. */
		memcpy(buffer, &request, sizeof(request));
		status = XcControlCommand(IOCTL_XC_GET_RAW, buffer,
		                          sizeof(request),
		                          sizeof(XC_RAW_INFO) - 1, &written);
		check_eq(status, STATUS_BUFFER_TOO_SMALL,
		         "and so is an output buffer that cannot hold it");

		core_set_config_default(&devext.Core);
	}

	/* --- GET_STATS ------------------------------------------------ */
	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_GET_STATS, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "GET_STATS answers");
	check_eq((long)written, (long)sizeof(XC_STATS), "with the counters");
	memcpy(&stats, buffer, sizeof(stats));
	check_eq((long)stats.layer, 1, "reporting layer 1 as live");

	/* --- GET_CONFIG returns something SET_CONFIG would accept ----- */
	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_GET_CONFIG, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "GET_CONFIG answers");
	check_eq((long)written, 1048, "with the whole blob");
	check_eq(core_config_load(&cfg, buffer, written, NULL), CORE_CFG_OK,
	         "AND WHAT IT RETURNS PARSES - the read-back is a blob, not a"
	         " debug dump");

	/* --- SET_CONFIG ------------------------------------------------ */
	core_config_defaults(&cfg);
	cfg.layout[0].binding[2].source    = CORE_SA_A;
	cfg.layout[0].binding[2].action    = CORE_ACT_KEY;
	cfg.layout[0].binding[2].code      = 0x1A;      /* W */
	blob_len = core_config_save(&cfg, blob, sizeof(blob));

	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	memcpy(buffer + sizeof(request), blob, blob_len);
	status = XcControlCommand(IOCTL_XC_SET_CONFIG, buffer,
	                          (ULONG)(sizeof(request) + blob_len),
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "SET_CONFIG installs a blob");
	check_eq(devext.Core.cfg.layout[0].binding[2].code, 0x1A,
	         "and the engine is running it");
	check(devext.Core.cfg.suppress[0] & (1u << CORE_SA_A),
	      "with the suppression mask rebuilt for it");

	/* --- a malformed blob is refused and changes nothing ---------- */
	memcpy(buffer, &request, sizeof(request));
	memcpy(buffer + sizeof(request), blob, blob_len);
	buffer[sizeof(request)] ^= 0xFF;                /* break the signature */
	status = XcControlCommand(IOCTL_XC_SET_CONFIG, buffer,
	                          (ULONG)(sizeof(request) + blob_len),
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_INVALID_PARAMETER,
	         "a bad signature is refused");
	check_eq(devext.Core.cfg.layout[0].binding[2].code, 0x1A,
	         "and the refusal left the running map alone");

	/* An older blob version says so specifically, so a configurator can
	 * tell "you are out of date" from "that is rubbish". */
	memcpy(buffer, &request, sizeof(request));
	memcpy(buffer + sizeof(request), blob, blob_len);
	buffer[sizeof(request) + 4] = 0;
	buffer[sizeof(request) + 5] = 0;
	status = XcControlCommand(IOCTL_XC_SET_CONFIG, buffer,
	                          (ULONG)(sizeof(request) + blob_len),
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_REVISION_MISMATCH,
	         "an old blob version is distinguishable from a bad one");

	/* A request with no blob behind it is not a zero-length install. */
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_SET_CONFIG, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_INVALID_PARAMETER,
	         "SET_CONFIG with no blob is refused");

	/* And a truncated request cannot even name a device. */
	status = XcControlCommand(IOCTL_XC_SET_CONFIG, buffer, 2,
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_INVALID_PARAMETER,
	         "a request too short to hold an index is refused");

	/* --- RESET_CONFIG --------------------------------------------- */
	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_RESET_CONFIG, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "RESET_CONFIG succeeds");
	check_eq(devext.Core.cfg.layout[0].binding[2].action, CORE_ACT_NONE,
	         "and the pushed binding is gone");

	/* --- THE CONTROL DEVICE LIVES ONLY WHILE A PAD DOES ----------- */
	check_eq(g_ControlDeviceAlive, 1,
	         "a pad arriving brings the control device up");

	/* --- removal takes it back out of the registry ---------------- */
	XcRemoveDevice(&devext);
	check_eq(g_ControlDeviceAlive, 0,
	         "AND THE LAST PAD LEAVING TAKES IT DOWN. A driver object"
	         " that still owns a device is never unloaded, so one that"
	         " outlived the pad would pin the old image in memory and"
	         " make the next install do nothing at all");
	status = XcControlCommand(IOCTL_XC_GET_DEVICES, buffer, 0,
	                          sizeof(buffer), &written);
	memcpy(&list, buffer, sizeof(list));
	check_eq((long)list.count, 0, "a removed device leaves the list");

	request.index = 0;
	memcpy(buffer, &request, sizeof(request));
	status = XcControlCommand(IOCTL_XC_GET_STATS, buffer,
	                          sizeof(request), sizeof(buffer), &written);
	check_eq(status, STATUS_DEVICE_DOES_NOT_EXIST,
	         "AND ITS INDEX STOPS RESOLVING - a command must not reach an"
	         " extension PnP has finished with");

	/*
	 * EVERY PATH RELEASES THE LOCK. A handler that returns while holding
	 * it wedges every later command and every device removal, and the
	 * symptom would be a hang with nothing in it to point here.
	 */
	check_eq(g_MutexDepth, 0,
	         "the device lock is free after every command above");

	/*
	 * THE CONTROL DEVICE IS NEVER CREATED OR DELETED UNDER THE LOCK.
	 * The lock raises IRQL to APC_LEVEL and IoCreateDevice needs
	 * PASSIVE_LEVEL, so holding it across either call bugchecks under
	 * Driver Verifier - during boot, where the pad is started, which
	 * means a machine that will not come up until the pad is out.
	 */
	check_eq(g_ControlDeviceUnderLock, 0,
	         "the control device is created and deleted at"
	         " PASSIVE_LEVEL, never under the device lock");

	return 0;
}

/* ======================================================================
 * CHORDS AND LAYERS
 *
 * ../docs/mapping-engine.txt sections 2.2 and 7. The hard part is not
 * making a chord fire; it is making sure its members do not fire on the
 * way in. A packet is 4ms and two buttons pressed together land tens of
 * milliseconds apart, so "suppress while the chord is complete" is not
 * enough on its own.
 * ====================================================================== */

/* Start and Back are digital, and live in the packet's button bitmask. */
static void chord_buttons(core_state *cs, u8 *packet, u8 digital, u64 when)
{
	packet[CORE_RAW_DIGITAL] = digital;
	core_on_packet(cs, packet, CORE_RAW_PACKET_BYTES, when);
}

static int test_chords(void)
{
	static core_config cfg;
	core_state         cs;
	u8                 packet[CORE_RAW_PACKET_BYTES];
	/* 100ns units: CORE_100NS_PER_MS is 10000, so one
	 * packet at 250 Hz is 40000 and not 4000. */
	u64                t = 1000;
	const u64          PACKET = 4 * CORE_100NS_PER_MS;
	u8                 blob[4096];
	u32                len;

	/* --- the shipped arrangement: Start+Back cycles the layer ----- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	check_eq(core_set_config(&cs, blob, len, NULL), CORE_CFG_OK,
	         "the built-in configuration installs");
	check(cs.cfg.chord_members[0] & (1u << CORE_SA_START),
	      "Start is known to be a chord member");
	check(cs.cfg.chord_members[0] & (1u << CORE_SA_BACK),
	      "and so is Back");
	check(!(cs.cfg.suppress[0] & (1u << CORE_SA_START)),
	      "BUT NEITHER IS SUPPRESSED - naming a source in a chord does"
	      " not bind it, so Start keeps its own button");

	make_packet(packet);
	chord_buttons(&cs, packet, 0, t += PACKET);
	check_eq(cs.layout, 0, "the pad starts on layer 1");

	/* --- the members reach the gamepad on their own ---------------- */
	chord_buttons(&cs, packet, CORE_DIG_START, t += PACKET);
	check(cs.gp.buttons & 0x0040,
	      "Start alone drives its own button - nothing waits on the"
	      " chance that a chord might be coming");
	chord_buttons(&cs, packet, 0, t += PACKET);

	/* --- a short tap of a member still reaches the gamepad -------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);
	make_packet(packet);
	chord_buttons(&cs, packet, 0, t += PACKET);
	{
		int saw_start = 0;
		int k;

		/* Press Start for 20ms - an ordinary tap - then release, and
		 * watch every packet for five more packets. */
		for (k = 0; k < 5; k++) {
			chord_buttons(&cs, packet, CORE_DIG_START, t += PACKET);
			if (cs.gp.buttons & 0x0040) { saw_start = 1; }
		}
		for (k = 0; k < 20; k++) {
			chord_buttons(&cs, packet, 0, t += PACKET);
			if (cs.gp.buttons & 0x0040) { saw_start = 1; }
		}
		check(saw_start, "A SHORT TAP OF START REACHES THE GAMEPAD");
	}

	/* --- a layer change releases what the old layer was holding --- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source = CORE_SA_A;
	cfg.layout[0].binding[1].action = CORE_ACT_KEY;
	cfg.layout[0].binding[1].code   = 0x1A;         /* W */
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;         /* hold A */
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	check_eq(cs.kb.count, 1, "A holds W down on layer 1");

	chord_buttons(&cs, packet, (u8)(CORE_DIG_START | CORE_DIG_BACK),
	              t += PACKET);
	chord_buttons(&cs, packet, (u8)(CORE_DIG_START | CORE_DIG_BACK),
	              t += PACKET);
	check_eq(cs.layout, 1, "the chord switches layer with A still held");
	check_eq(cs.kb.count, 0,
	         "AND W IS RELEASED. Layer 2 has nothing bound to A, so"
	         " nothing there would ever have let go of it");

	/* --- an empty chord is not a chord that is always on ---------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].chord[0].member[0] = CORE_SA_NONE;
	cfg.layout[0].chord[0].member[1] = CORE_SA_NONE;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	chord_buttons(&cs, packet, 0, t += PACKET);
	check_eq((long)cs.chord_value[0], 0,
	         "A CHORD NAMING NOTHING NEVER FIRES - an all-members-down"
	         " test over an empty set is vacuously true");
	chord_buttons(&cs, packet, 0, t += PACKET);
	check_eq(cs.layout, 0, "so it cannot cycle the layer either");

	return 0;
}

/* ======================================================================
 * AUTOFIRE
 *
 * ../docs/mapping-engine.txt section 6. A HID report carries state, not
 * events, so a press and a release inside one report are invisible to the
 * host. Autofire has to be an alternating output that the ordinary
 * emit-on-change path turns into reports, not a burst inside one.
 * ====================================================================== */

/* How many times a gamepad button went from clear to set across the
 * reports collected since the last sink_reset. */
static long sink_button_presses(u16 bit)
{
	long presses = 0;
	int  was = 0;
	int  i;

	for (i = 0; i < g_SinkCount; i++) {
		const u8 *p;
		int       now;

		if (g_Sink[i].id != CORE_REPORT_ID_GAMEPAD) {
			continue;
		}
		p = &g_Sink[i].payload[CORE_GP_BUTTONS];
		now = ((u16)(p[0] | (p[1] << 8)) & bit) ? 1 : 0;
		if (now && !was) {
			presses++;
		}
		was = now;
	}
	return presses;
}

static int test_autofire(void)
{
	static core_config cfg;
	core_state         cs;
	u8                 packet[CORE_RAW_PACKET_BYTES];
	u8                 blob[4096];
	u32                len;
	u64                t = 1000;
	const u64          PACKET = 4 * CORE_100NS_PER_MS;
	int                k;

	/* --- A at 10 Hz, no delay ------------------------------------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source    = CORE_SA_A;
	cfg.layout[0].binding[1].action    = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[1].code      = 1;
	cfg.layout[0].binding[1].flags     = CORE_BF_REPEAT;
	cfg.layout[0].binding[1].repeat_hz = 10;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	check_eq(core_set_config(&cs, blob, len, NULL), CORE_CFG_OK,
	         "an autofire binding installs");

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	sink_reset();

	/* Hold A for one second: 250 packets at 4ms. */
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	{
		long presses = sink_button_presses(0x0001);

		check_eq(g_SinkDropped, 0,
		         "the report ring did not overflow, so the count below"
		         " is of everything that happened");
		check(presses >= 9 && presses <= 11,
		      "holding A for a second at 10 Hz presses button 1 about"
		      " ten times");
	}

	/* --- and releasing lets go, FROM EVERY PHASE OF THE CYCLE ----- */
	{
		int stuck = -1;
		int hold;

		/*
		 * A 10 Hz cycle is 100ms, or 25 packets. Letting go at one
		 * arbitrary moment tests one phase and passes by luck; walk
		 * the whole period and every phase is covered, the asserted
		 * half included.
		 */
		for (hold = 1; hold <= 30 && stuck < 0; hold++) {
			packet[CORE_RAW_ANALOG_BASE + 0] = 0;
			for (k = 0; k < 40; k++) {
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t += PACKET);
			}

			packet[CORE_RAW_ANALOG_BASE + 0] = 255;
			for (k = 0; k < hold; k++) {
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t += PACKET);
			}

			packet[CORE_RAW_ANALOG_BASE + 0] = 0;
			core_on_packet(&cs, packet,
			               CORE_RAW_PACKET_BYTES, t += PACKET);
			if (cs.gp.buttons & 0x0001) {
				stuck = hold;
			}
		}
		check_eq(stuck, -1,
		         "RELEASING ALWAYS RELEASES, from any phase. A binding"
		         " that deactivates during the asserted half must not"
		         " leave the button held");
	}

	/* --- the same, on a key, which is where it would be worst ----- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source    = CORE_SA_A;
	cfg.layout[0].binding[1].action    = CORE_ACT_KEY;
	cfg.layout[0].binding[1].code      = 0x1A;      /* W */
	cfg.layout[0].binding[1].flags     = CORE_BF_REPEAT;
	cfg.layout[0].binding[1].repeat_hz = 10;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	{
		int stuck = -1;
		int hold;

		for (hold = 1; hold <= 30 && stuck < 0; hold++) {
			packet[CORE_RAW_ANALOG_BASE + 0] = 0;
			for (k = 0; k < 40; k++) {
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t += PACKET);
			}

			packet[CORE_RAW_ANALOG_BASE + 0] = 255;
			for (k = 0; k < hold; k++) {
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t += PACKET);
			}

			packet[CORE_RAW_ANALOG_BASE + 0] = 0;
			core_on_packet(&cs, packet,
			               CORE_RAW_PACKET_BYTES, t += PACKET);
			if (cs.kb.count != 0) {
				stuck = hold;
			}
		}
		check_eq(stuck, -1,
		         "AND ON A KEY TOO - a key left held by an interrupted"
		         " autofire cycle types forever");
	}

	/* --- PRESS HARDER TO AUTOFIRE --------------------------------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source    = CORE_SA_A;
	cfg.layout[0].binding[1].action    = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[1].code      = 1;
	cfg.layout[0].binding[1].flags     = CORE_BF_REPEAT;
	cfg.layout[0].binding[1].repeat_hz = 10;
	cfg.layout[0].binding[1].hard_at   = CORE_MAX_VALUE / 2;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);

	/* A light press - held, and only held. */
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 100;         /* ~39 per cent */
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	check_eq(sink_button_presses(0x0001), 1,
	         "A PRESS SHORT OF hard_at IS AN ORDINARY HOLD - pressed"
	         " once, and still down a second later");
	check(cs.gp.buttons & 0x0001, "with the button down at the end of it");

	/* Lean on it and the same unbroken press starts repeating. */
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	{
		long presses = sink_button_presses(0x0001);

		check(presses >= 9 && presses <= 11,
		      "PUSHING PAST hard_at AUTOFIRES at about ten a second,"
		      " without the control ever being let go of");
	}

	/* Ease off and it is a plain hold again. */
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 100;
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	check(sink_button_presses(0x0001) <= 1,
	      "and easing off stops the repeat");
	check(cs.gp.buttons & 0x0001,
	      "leaving the button held, not stuck in the released half");

	/* --- repeat_delay_ms: one press, a pause, then repetition ----- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source          = CORE_SA_A;
	cfg.layout[0].binding[1].action          = CORE_ACT_JOY_BUTTON;
	cfg.layout[0].binding[1].code            = 1;
	cfg.layout[0].binding[1].flags           = CORE_BF_REPEAT;
	cfg.layout[0].binding[1].repeat_hz       = 20;
	cfg.layout[0].binding[1].repeat_delay_ms = 300;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;

	/* 250ms - inside the delay. */
	for (k = 0; k < 62; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	check_eq(sink_button_presses(0x0001), 1,
	         "inside repeat_delay_ms the button is pressed exactly once");
	check(cs.gp.buttons & 0x0001, "and is still held");

	/* Another 500ms - past it, and repeating at 20 Hz. */
	for (k = 0; k < 125; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	check(sink_button_presses(0x0001) > 5,
	      "past the delay it repeats - the keyboard behaviour, where a"
	      " held key types once, pauses, then runs");

	/* --- THE RATE IS THE RATE, ON THE TICK ------------------------ */
	{
		/*
		 * DRIVEN BY THE TICK AND NOT BY PACKETS, because that is what
		 * the hardware does: the pad reports only when something
		 * changes, so a held button is silent and every flip below has
		 * to come from the timer.
		 *
		 * A deadline is noticed at the first tick AT OR AFTER it, never
		 * exactly on it. Measuring the next half period from that
		 * moment rounds every half up to a whole tick and pays the
		 * error again every half, which quantises the rate instead of
		 * jittering it - 20 Hz ran at 15. The deadline therefore
		 * advances by exactly half a period.
		 */
		static const int RATES[] = { 5, 10, 12, 20, 30 };
		int r;

		for (r = 0; r < 5; r++) {
			core_init(&cs, recording_sink, NULL);
			core_config_defaults(&cfg);
			cfg.layout[0].binding[1].source    = CORE_SA_A;
			cfg.layout[0].binding[1].action    = CORE_ACT_JOY_BUTTON;
			cfg.layout[0].binding[1].code      = 1;
			cfg.layout[0].binding[1].flags     = CORE_BF_REPEAT;
			cfg.layout[0].binding[1].repeat_hz = (u8)RATES[r];
			core_config_suppress(&cfg);
			len = core_config_save(&cfg, blob, sizeof(blob));
			core_set_config(&cs, blob, len, NULL);

			make_packet(packet);
			packet[CORE_RAW_ANALOG_BASE + 0] = 255;
			t += PACKET;
			core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t);
			sink_reset();

			/* One second of ticks, and not one packet. */
			for (k = 0; k < 1000 / CORE_TICK_MS; k++) {
				t += CORE_TICK_MS * CORE_100NS_PER_MS;
				core_tick(&cs, t);
			}
			check_eq(g_SinkDropped, 0,
			         "the ring held every report, so the count is real");
			check_eq(sink_button_presses(0x0001), RATES[r],
			         "repeat_hz presses EXACTLY that many times a second");
		}
	}

	/* --- a wheel with REPEAT scrolls once per period -------------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source    = CORE_SA_A;
	cfg.layout[0].binding[1].action    = CORE_ACT_MOUSE_WHEEL;
	cfg.layout[0].binding[1].code      = 1;
	cfg.layout[0].binding[1].flags     = CORE_BF_REPEAT;
	cfg.layout[0].binding[1].repeat_hz = 10;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	{
		long detents = sink_wheel_total();

		check(detents >= 9 && detents <= 11,
		      "AN ACTION WITH NO HELD STATE REPEATS DIFFERENTLY - the"
		      " wheel emits one detent per period rather than"
		      " alternating");
	}

	/* --- the shipped arcade layer, end to end --------------------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	make_packet(packet);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);

	/* Start+Back to layer 2, then hold A. */
	packet[CORE_RAW_DIGITAL] = (u8)(CORE_DIG_START | CORE_DIG_BACK);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	check_eq(cs.layout, 1, "Start+Back reaches the autofire layer");

	packet[CORE_RAW_DIGITAL] = 0;
	core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	sink_reset();
	packet[CORE_RAW_ANALOG_BASE + 0] = 255;
	for (k = 0; k < 250; k++) {
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t += PACKET);
	}
	{
		long presses = sink_button_presses(0x0001);

		check(presses >= 11 && presses <= 13,
		      "and holding A there fires button 1 at the shipped 12 Hz");
	}

	return 0;
}

/* ======================================================================
 * THE STICK PIPELINE
 *
 * ../docs/analog-to-mouse.txt section 8. Every check here is aimed at one
 * of the fixed-point traps that note names, because each of them produces
 * motion that looks plausible and is wrong.
 * ====================================================================== */

/*
 * Total pointer motion since the last sink_reset, taken from the running
 * totals rather than the ring - see the note on g_MouseTotalX.
 */
static void sink_mouse_total(long *dx, long *dy)
{
	*dx = g_MouseTotalX;
	*dy = g_MouseTotalY;
}

/* A configuration with the right stick driving the pointer, and nothing
 * smoothed or accelerated, so the arithmetic is the only variable. */
static void stick_config(core_config *cfg)
{
	core_config_defaults(cfg);
	cfg->stick[1].mode        = CORE_STICK_MOUSE;
	cfg->stick[1].smooth_ms   = 0;
	cfg->stick[1].accel_rate  = 0;
	cfg->stick[1].gain_x      = 128;
	cfg->stick[1].gain_y      = 128;
	cfg->stick[1].invert_x    = 0;
	cfg->stick[1].invert_y    = 0;
	core_config_suppress(cfg);
}

/*
 * Hold the right stick at (x, y) for a number of ticks.
 *
 * ONE PACKET, THEN TICKS, BECAUSE THAT IS WHAT THE PAD DOES. It reports
 * only when something changes - measured at five reports a second with
 * nothing touched and seven to ten with a stick held against its stop -
 * so a held stick is silent. Feeding a packet per tick would test a
 * device that does not exist and would hide the very starvation this
 * arrangement exists to prevent.
 */
static void stick_hold(core_state *cs, s16 x, s16 y, u64 *t, int ticks)
{
	u8  packet[CORE_RAW_PACKET_BYTES];
	int k;

	make_packet(packet);
	put_le16(&packet[CORE_RAW_RSTICK_X], x);
	put_le16(&packet[CORE_RAW_RSTICK_Y], y);

	/* The packet costs no time of its own, so the elapsed interval is
	 * exactly the ticks that follow and the arithmetic below is exact. */
	core_on_packet(cs, packet, CORE_RAW_PACKET_BYTES, *t);

	for (k = 0; k < ticks; k++) {
		*t += 8 * CORE_100NS_PER_MS;
		core_tick(cs, *t);
	}
}

static int test_sticks(void)
{
	static core_config cfg;
	core_state         cs;
	u8                 blob[4096];
	u32                len;
	u64                t = 1000;
	long               dx, dy, dx2, dy2;

	stick_config(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));

	/* --- the stick takes the axes it drives ----------------------- */
	core_init(&cs, recording_sink, NULL);
	check_eq(core_set_config(&cs, blob, len, NULL), CORE_CFG_OK,
	         "a pointer configuration installs");
	check(cs.cfg.stick_claim & (1u << CORE_SA_RSTICK_XPOS),
	      "a stick driving the pointer claims its own semiaxes");
	check(!(cs.cfg.stick_claim & (1u << CORE_SA_LSTICK_XPOS)),
	      "and leaves the other stick alone");

	stick_hold(&cs, 32767, 0, &t, 2);
	check_eq(cs.gp.axis[2], 0,
	         "AND STOPS DRIVING Rx. A stick that aims and also moves the"
	         " gamepad axis moves the crosshair twice");
	check(cs.gp.axis[0] == 0, "the left stick is still centred");

	/* --- the deadzone -------------------------------------------- */
	core_init(&cs, recording_sink, NULL);
	core_set_config(&cs, blob, len, NULL);
	sink_reset();
	stick_hold(&cs, 1200, 0, &t, 50);      /* about 1280 units */
	sink_mouse_total(&dx, &dy);
	check_eq(dx, 0, "inside the deadzone the pointer does not move");
	check_eq(dy, 0, "on either axis");

	/* --- full deflection runs at max_speed ------------------------ */
	core_init(&cs, recording_sink, NULL);
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, 32767, 0, &t, 2);       /* prime the clock */
	sink_reset();
	stick_hold(&cs, 32767, 0, &t, 125);     /* one second */
	sink_mouse_total(&dx, &dy);
	check(dx > 2750 && dx < 2810,
	      "a second at full deflection moves about max_speed pixels");
	check_eq(dy, 0, "and nothing sideways");

	/* --- THE ACCUMULATOR: no drift over time ---------------------- */
	sink_reset();
	stick_hold(&cs, 32767, 0, &t, 500);    /* four seconds */
	sink_mouse_total(&dx, &dy);
	{
		long expected = 4 * dx / 4;     /* silence unused warnings */

		(void)expected;
		check(dx > 4 * 2750 && dx < 4 * 2810,
		      "FOUR SECONDS MOVES FOUR TIMES AS FAR. 11.2 pixels per"
		      " tick truncated to 11 would lose 1.75 per cent, which"
		      " is what carrying the remainder prevents");
	}

	/* --- truncation toward zero, not floor ------------------------ */
	core_init(&cs, recording_sink, NULL);
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, -32767, 0, &t, 2);
	sink_reset();
	stick_hold(&cs, -32767, 0, &t, 500);
	sink_mouse_total(&dx2, &dy2);
	check(dx2 < 0 && (-dx2 > dx - 8) && (-dx2 < dx + 8),
	      "LEFT MOVES EXACTLY AS FAR AS RIGHT. A shift instead of a"
	      " divide floors rather than truncating, and biases negative"
	      " motion by one count every tick");

	/* --- radial, not per-axis ------------------------------------- */
	core_init(&cs, recording_sink, NULL);
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, 32767, 32767, &t, 2);
	sink_reset();
	stick_hold(&cs, 32767, 32767, &t, 125);
	sink_mouse_total(&dx2, &dy2);
	{
		/* The magnitude of the diagonal, times 1000 to stay integer. */
		long mag = (long)core_isqrt64((u64)((s64)dx2 * dx2 +
		                                    (s64)dy2 * dy2));

		check(mag > 2750 && mag < 2810,
		      "A DIAGONAL IS NO FASTER THAN A CARDINAL. Per-axis"
		      " deadzones and per-axis speed are what give the shipped"
		      " Adaptoid profiles their dead cross and fast diagonals");
		check(dx2 > 1900 && dx2 < 2050, "and it is evenly split");
	}

	/* --- the curve is consulted ----------------------------------- */
	core_init(&cs, recording_sink, NULL);
	stick_config(&cfg);
	cfg.stick[1].deadzone = 0;
	cfg.stick[1].outer    = CORE_MAX_VALUE;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, 16383, 0, &t, 2);
	sink_reset();
	stick_hold(&cs, 16383, 0, &t, 125);
	sink_mouse_total(&dx, &dy);
	check(dx > 1330 && dx < 1470,
	      "with a linear curve, half deflection is half speed");

	{
		/* A quadratic curve: g = u*u, so half deflection is a
		 * quarter of the speed. Built here the way the configurator
		 * would, since the driver never evaluates one. */
		int i;

		for (i = 0; i < CORE_CURVE_POINTS; i++) {
			long uu = (long)i * 65535 / (CORE_CURVE_POINTS - 1);

			cfg.stick[1].curve[i] = (u16)((uu * uu) / 65535);
		}
		core_config_suppress(&cfg);
		len = core_config_save(&cfg, blob, sizeof(blob));

		core_init(&cs, recording_sink, NULL);
		core_set_config(&cs, blob, len, NULL);
		stick_hold(&cs, 16383, 0, &t, 2);
		sink_reset();
		stick_hold(&cs, 16383, 0, &t, 125);
		sink_mouse_total(&dx, &dy);
		check(dx > 640 && dx < 780,
		      "and with a quadratic one it is a QUARTER - the table is"
		      " read, not ignored");
	}

	/* --- which way is up ------------------------------------------ */
	core_init(&cs, recording_sink, NULL);
	stick_config(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, 0, 32767, &t, 2);       /* pad Y is up-positive */
	sink_reset();
	stick_hold(&cs, 0, 32767, &t, 125);
	sink_mouse_total(&dx, &dy);
	check(dy < 0,
	      "PUSHING THE STICK UP MOVES THE POINTER UP. Screen Y grows"
	      " downward, so up is negative");

	/* --- and which way is up WITH THE SHIPPED DEFAULTS ------------ */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.stick[1].mode = CORE_STICK_MOUSE;   /* and nothing else */
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);
	stick_hold(&cs, 0, 32767, &t, 2);
	sink_reset();
	stick_hold(&cs, 0, 32767, &t, 125);
	sink_mouse_total(&dx, &dy);
	check(dy < 0,
	      "UP IS UP UNDER THE BUILT-IN CONFIGURATION TOO. The decode"
	      " already turns the pad's up-positive Y into HID's"
	      " down-positive one, so inverting again here would aim the"
	      " wrong way out of the box");

	/* --- THE TICK IS WHAT MOVES THE POINTER ----------------------- */
	core_init(&cs, recording_sink, NULL);
	stick_config(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	/*
	 * ONE packet puts the stick over, and then nothing else arrives -
	 * which is exactly what the hardware does with a stick held still.
	 * Every pixel below comes from the tick.
	 */
	{
		u8  packet[CORE_RAW_PACKET_BYTES];
		int k;

		make_packet(packet);
		put_le16(&packet[CORE_RAW_RSTICK_X], 32767);
		t += 4 * CORE_100NS_PER_MS;
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t);
		t += 8 * CORE_100NS_PER_MS;
		core_tick(&cs, t);
		sink_reset();

		for (k = 0; k < 125; k++) {     /* one second of ticks */
			t += 8 * CORE_100NS_PER_MS;
			core_tick(&cs, t);
		}
	}
	sink_mouse_total(&dx, &dy);
	check(dx > 2750 && dx < 2810,
	      "A HELD STICK KEEPS MOVING THE POINTER WITH NO FURTHER"
	      " PACKETS. The pad reports only on change, so a stick held"
	      " still is silent; a pointer that advanced only on packets"
	      " would step about seven times a second");
	check_eq(dy, 0, "and only on the axis that was pushed");

	/* --- and silence is not a fault ------------------------------- */
	core_init(&cs, recording_sink, NULL);
	core_config_defaults(&cfg);
	cfg.layout[0].binding[1].source = CORE_SA_A;
	cfg.layout[0].binding[1].action = CORE_ACT_KEY;
	cfg.layout[0].binding[1].code   = 0x1A;         /* W */
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);
	{
		u8  packet[CORE_RAW_PACKET_BYTES];
		int k;

		make_packet(packet);
		packet[CORE_RAW_ANALOG_BASE + 0] = 255;         /* hold A */
		t += 4 * CORE_100NS_PER_MS;
		core_on_packet(&cs, packet, CORE_RAW_PACKET_BYTES, t);
		check_eq(cs.kb.count, 1, "A is holding W down");

		/* Seconds of ticks with no packet at all. */
		for (k = 0; k < 500; k++) {
			t += 8 * CORE_100NS_PER_MS;
			core_tick(&cs, t);
		}
		check_eq(cs.kb.count, 1,
		         "AND FOUR SECONDS OF SILENCE DOES NOT RELEASE IT. A pad"
		         " that speaks only on change is silent whenever nothing"
		         " is moving, so a timeout would fire between ordinary"
		         " packets and drop what is genuinely held");
	}

	/* --- a stick that scrolls ------------------------------------- */
	core_init(&cs, recording_sink, NULL);
	stick_config(&cfg);
	cfg.stick[1].mode      = CORE_STICK_WHEEL;
	cfg.stick[1].max_speed = 10;    /* DETENTS a second, not pixels */
	cfg.stick[1].deadzone  = 2000;
	cfg.stick[1].outer     = 32000;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	sink_reset();
	stick_hold(&cs, 0, 32767, &t, 125);     /* stick up, one second */
	/* Eight or nine, not ten: the last partial detent is still in the
	 * accumulator, which is the carry working as intended. */
	check(sink_wheel_total() >= 8 && sink_wheel_total() <= 10,
	      "PUSHING UP SCROLLS UP at about max_speed detents a second");
	sink_mouse_total(&dx, &dy);
	check_eq(dx, 0, "and moves the pointer not at all");
	check_eq(dy, 0, "on either axis");

	sink_reset();
	stick_hold(&cs, 0, -32767, &t, 125);    /* stick down */
	check(sink_wheel_total() <= -8 && sink_wheel_total() >= -10,
	      "and pushing down scrolls down");

	/* SIDEWAYS IS NOT SCROLL, and the deadzone is vertical only - a
	 * radial one would let a horizontal push eat it. */
	sink_reset();
	stick_hold(&cs, 32767, 0, &t, 125);
	check_eq(sink_wheel_total(), 0,
	         "a purely sideways push scrolls nothing");

	/* --- absolute is reserved, and says so ------------------------ */
	stick_config(&cfg);
	cfg.stick[1].mode = CORE_STICK_ABSOLUTE;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	{
		u32 repaired = 0;

		core_init(&cs, recording_sink, NULL);
		core_set_config(&cs, blob, len, &repaired);
		check_eq(cs.cfg.stick[1].mode, CORE_STICK_OFF,
		         "ABSOLUTE IS REPAIRED TO OFF, not silently inert - it"
		         " needs a collection that declares absolute axes");
		check(repaired >= 1, "and the repair is counted");
	}

	/* --- acceleration builds with time held ----------------------- */
	core_init(&cs, recording_sink, NULL);
	stick_config(&cfg);
	cfg.stick[1].accel_threshold = 30000;
	cfg.stick[1].accel_rate      = 256;     /* +1.0x per second */
	cfg.stick[1].accel_max       = 512;
	core_config_suppress(&cfg);
	len = core_config_save(&cfg, blob, sizeof(blob));
	core_set_config(&cs, blob, len, NULL);

	stick_hold(&cs, 32767, 0, &t, 2);
	sink_reset();
	stick_hold(&cs, 32767, 0, &t, 125);     /* first second */
	sink_mouse_total(&dx, &dy);
	sink_reset();
	stick_hold(&cs, 32767, 0, &t, 125);     /* second second */
	sink_mouse_total(&dx2, &dy2);
	check(dx2 > dx,
	      "HELD AT THE EDGE, THE TURN KEEPS BUILDING. No curve of any"
	      " shape can do this - it is a function of time held, not of"
	      " deflection");

	return 0;
}

/* ======================================================================
 * RUMBLE
 *
 * ../docs/driver-plan.txt section 8. Rumble is a LEVEL, not a message, so
 * the interesting case is not sending one packet - it is what happens to
 * a level that arrives while a transfer is already in flight.
 * ====================================================================== */

void XcRumbleCompleteForTest(PXC_DEVEXT DevExt);

static int test_rumble(void)
{
	static XC_DEVEXT  devext;
	static u8         buffer[256];
	XC_RUMBLE_REQUEST rr;
	XC_STATS          stats;
	ULONG             written;
	NTSTATUS          status;

	XcDeviceRegistryReset();
	XcDevExtInit(&devext);
	XcStartDevice((PDEVICE_OBJECT)&devext, NULL);

	/* --- the packet the pad expects ------------------------------- */
	XcRumbleSet(&devext, 200, 100);
	check(devext.Rumble.Active, "a level submits a transfer");
	check_eq(devext.Rumble.Buffer[0], 0x00, "byte 0 is the report type");
	check_eq(devext.Rumble.Buffer[1], XC_RUMBLE_PACKET_BYTES,
	         "byte 1 is the length, six");
	check_eq(devext.Rumble.Buffer[2], 0x00, "byte 2 is zero");
	check_eq(devext.Rumble.Buffer[3], 200, "byte 3 is the left actuator");
	check_eq(devext.Rumble.Buffer[4], 0x00, "byte 4 is zero");
	check_eq(devext.Rumble.Buffer[5], 100, "byte 5 is the right actuator");
	check_eq((long)devext.Rumble.Sent, 1, "one transfer so far");

	/* --- THE NEWEST LEVEL WINS -------------------------------------- */
	XcRumbleSet(&devext, 10, 10);
	XcRumbleSet(&devext, 20, 20);
	XcRumbleSet(&devext, 30, 30);
	check_eq((long)devext.Rumble.Sent, 1,
	         "levels arriving during a transfer do not queue behind it");
	check(devext.Rumble.Dirty, "they are remembered");
	check_eq(devext.Rumble.Buffer[3], 200,
	         "and the transfer in flight is left alone");

	XcRumbleCompleteForTest(&devext);
	check_eq((long)devext.Rumble.Sent, 2, "the completion sends again");
	check_eq(devext.Rumble.Buffer[3], 30,
	         "WITH THE NEWEST LEVEL, not the oldest. Queueing would"
	         " replay a burst the caller has already moved on from");
	check_eq(devext.Rumble.Buffer[5], 30, "on both actuators");

	XcRumbleCompleteForTest(&devext);
	check(!devext.Rumble.Active,
	      "and with nothing new pending it stops");
	check_eq((long)devext.Rumble.Sent, 2, "having sent exactly twice");

	/* --- an unchanged level is not news --------------------------- */
	XcRumbleSet(&devext, 30, 30);
	check_eq((long)devext.Rumble.Sent, 2,
	         "setting the level it is already at sends nothing");

	/* --- stop is a level like any other --------------------------- */
	XcRumbleSet(&devext, 0, 0);
	check_eq((long)devext.Rumble.Sent, 3, "stopping is a transfer too");
	check_eq(devext.Rumble.Buffer[3], 0, "with both actuators at zero");
	XcRumbleCompleteForTest(&devext);

	/* --- the control device drives the same path ------------------ */
	rr.index = 0;
	rr.left  = 77;
	rr.right = 88;
	memcpy(buffer, &rr, sizeof(rr));
	status = XcControlCommand(IOCTL_XC_SET_RUMBLE, buffer, sizeof(rr),
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_SUCCESS, "SET_RUMBLE is accepted");
	check_eq(devext.Rumble.Buffer[3], 77,
	         "and reaches the same transfer the HID report does");
	check_eq(devext.Rumble.Buffer[5], 88, "on both actuators");
	XcRumbleCompleteForTest(&devext);

	/* A request too short to hold the levels is refused. */
	memcpy(buffer, &rr, sizeof(rr));
	status = XcControlCommand(IOCTL_XC_SET_RUMBLE, buffer, 4,
	                          sizeof(buffer), &written);
	check_eq(status, STATUS_INVALID_PARAMETER,
	         "a truncated SET_RUMBLE is refused");

	/* --- the counters reach GET_STATS ----------------------------- */
	{
		XC_CONFIG_REQUEST req;

		req.index = 0;
		memcpy(buffer, &req, sizeof(req));
		XcControlCommand(IOCTL_XC_GET_STATS, buffer, sizeof(req),
		                 sizeof(buffer), &written);
		memcpy(&stats, buffer, sizeof(stats));
		check_eq((long)stats.rumble_sent, (long)devext.Rumble.Sent,
		         "GET_STATS reports the transfers sent");
		check_eq((long)stats.rumble_errors, 0, "and none failed");
	}

	/* --- teardown does not strand a transfer ---------------------- */
	XcRumbleSet(&devext, 255, 255);
	check(devext.Rumble.Active, "a transfer is in flight");
	XcRemoveDevice(&devext);
	check(!devext.Rumble.Active,
	      "REMOVAL ACCOUNTS FOR IT. A write left outstanding holds the"
	      " drain open until the bus gives up on a pad that has gone");
	check_eq(devext.IoCount, 0, "and the reference count reaches zero");

	/* --- a pad with no OUT endpoint just does not rumble ---------- */
	XcDevExtInit(&devext);
	XcStartDevice((PDEVICE_OBJECT)&devext, NULL);
	devext.HasOutPipe = FALSE;
	devext.Rumble.Sent = 0;
	XcRumbleSet(&devext, 200, 200);
	check_eq((long)devext.Rumble.Sent, 0,
	         "a pad with no OUT endpoint is not asked to rumble");
	XcRemoveDevice(&devext);

	return 0;
}

/* ======================================================================
 * READS ARE SIZED PER COLLECTION
 *
 * hidclass makes one child device per top-level collection and sizes each
 * child's reads to that collection's largest report: five bytes for the
 * mouse, eight for the keyboard, thirty-seven for the gamepad. They all
 * arrive as IOCTL_HID_READ_REPORT on one device and nothing in the IRP
 * says which child sent it.
 *
 * COMPLETING THE WRONG ONE IS A KERNEL BUFFER OVERFLOW, not a wrong
 * value. A gamepad report written into the mouse child's buffer goes
 * thirty-two bytes past its end into hidclass's memory, and the damage
 * comes back as input nobody generated.
 * ====================================================================== */

/*
 * An IRP shaped like the one hidclass parks on us for a read: a buffer
 * and, crucially, the length of that buffer in the stack location.
 */
static IO_STACK_LOCATION g_ReadStack[4];
static int               g_ReadStackNext;

static void harness_make_read(PIRP Irp, UCHAR *Buffer, ULONG Length)
{
	PIO_STACK_LOCATION stack = &g_ReadStack[g_ReadStackNext++ % 4];

	memset(Irp, 0, sizeof(*Irp));
	memset(stack, 0, sizeof(*stack));
	stack->Parameters.DeviceIoControl.OutputBufferLength = Length;
	Irp->KstubStack = stack;
	Irp->UserBuffer = Buffer;
}

static int test_readfit(void)
{
	static XC_DEVEXT devext;
	static UCHAR     small[8];
	static UCHAR     large[64];
	static IRP       irp_small;
	static IRP       irp_large;
	u8               payload[CORE_GAMEPAD_PAYLOAD];
	int              i;

	XcDevExtInit(&devext);

	/* Two parked reads: a mouse-sized one first, then a gamepad-sized
	 * one, which is the order that produces the overflow. */
	for (i = 0; i < (int)sizeof(small); i++)  { small[i] = 0xAA; }
	for (i = 0; i < (int)sizeof(large); i++)  { large[i] = 0xAA; }

	harness_make_read(&irp_small, small, CORE_MOUSE_PAYLOAD + 1);
	harness_make_read(&irp_large, large, CORE_GAMEPAD_PAYLOAD + 1);
	XcQueueRead(&devext, &irp_small);
	XcQueueRead(&devext, &irp_large);
	check_eq((long)devext.PendingReadCount, 2, "two reads parked");

	/* A gamepad report must not take the five-byte read. */
	for (i = 0; i < CORE_GAMEPAD_PAYLOAD; i++) { payload[i] = (u8)i; }
	XcReportSink(&devext, CORE_REPORT_ID_GAMEPAD, payload,
	             CORE_GAMEPAD_PAYLOAD);

	check_eq(small[CORE_MOUSE_PAYLOAD + 1], 0xAA,
	         "A 37-BYTE REPORT DID NOT OVERRUN THE 5-BYTE READ. Taking"
	         " whichever read is at the head of the list writes 32 bytes"
	         " into hidclass's memory, and the damage returns as"
	         " keystrokes and wheel events nobody generated");
	check_eq(large[0], CORE_REPORT_ID_GAMEPAD,
	         "it went to the read that could hold it");
	check_eq(devext.PendingReadCount, 1,
	         "and only that read was consumed");

	/* The small read is still parked, and a mouse report fits it. */
	XcReportSink(&devext, CORE_REPORT_ID_MOUSE, payload,
	             CORE_MOUSE_PAYLOAD);
	check_eq(small[0], CORE_REPORT_ID_MOUSE,
	         "and a report that fits still reaches it");
	check_eq(devext.PendingReadCount, 0, "both reads are now used");

	/*
	 * WITH NO READ BIG ENOUGH, THE REPORT QUEUES rather than being
	 * forced into a buffer that cannot hold it.
	 */
	XcDevExtInit(&devext);
	for (i = 0; i < (int)sizeof(small); i++) { small[i] = 0xAA; }
	harness_make_read(&irp_small, small, CORE_MOUSE_PAYLOAD + 1);
	XcQueueRead(&devext, &irp_small);

	XcReportSink(&devext, CORE_REPORT_ID_GAMEPAD, payload,
	             CORE_GAMEPAD_PAYLOAD);
	check_eq(small[0], 0xAA, "the undersized read was left alone");
	check_eq((long)devext.PendingReadCount, 1, "and left parked");
	check_eq((long)devext.ReportCount, 1, "the report waits in the queue");

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

/*
 * --save-default FILE writes the built-in configuration as a blob.
 *
 * IT EXISTS TO CROSS-CHECK tools/mkconfig.py. Two independent
 * implementations of one wire format drift silently; comparing the
 * bytes they each produce for the same configuration catches a padding
 * or field-order difference immediately, and that is exactly the class
 * of bug that would otherwise surface as corrupted bindings on one
 * build only.
 */
static int save_default(const char *path)
{
	static core_config cfg;
	static u8          blob[4096];
	u32                len;
	FILE              *out;

	core_config_defaults(&cfg);
	len = core_config_save(&cfg, blob, (u32)sizeof(blob));
	if (len == 0) {
		printf("  core_config_save refused\n");
		return 1;
	}

	out = fopen(path, "wb");
	if (!out) {
		printf("  cannot write %s\n", path);
		return 1;
	}
	fwrite(blob, 1, len, out);
	fclose(out);

	printf("  %s, %u bytes\n", path, (unsigned)len);
	return 0;
}

/*
 * --load FILE runs a blob through the real validator and says what it
 * made of it. This is how a profile is checked before it is pushed to
 * a driver that parses it at DISPATCH_LEVEL with no way to complain.
 */
static int load_blob(const char *path)
{
	static core_config cfg;
	static u8          blob[8192];
	static const char *WHY[] = {
		"ok", "shorter than a header", "bad signature",
		"version older than this build", "stride below the structure",
		"a count past its ceiling", "ends before its header says"
	};
	u32   len, repaired = 0;
	int   rc;
	u32   l, i, active;
	FILE *in;

	in = fopen(path, "rb");
	if (!in) {
		printf("  cannot read %s\n", path);
		return 1;
	}
	len = (u32)fread(blob, 1, sizeof(blob), in);
	fclose(in);

	rc = core_config_load(&cfg, blob, len, &repaired);
	if (rc != CORE_CFG_OK) {
		printf("  REJECTED: %s (%d)\n", WHY[rc], rc);
		return 1;
	}

	printf("  %s: %u bytes, %u layout(s)\n",
	       path, (unsigned)len, cfg.layout_count);
	if (repaired) {
		printf("  %u field(s) REPAIRED - the blob was accepted but\n"
		       "  not as written; dump it to see what changed\n",
		       (unsigned)repaired);
	}

	for (l = 0; l < cfg.layout_count; l++) {
		active = 0;
		for (i = 0; i < cfg.binding_count; i++) {
			if (cfg.layout[l].binding[i].action !=
			    CORE_ACT_NONE) {
				active++;
			}
		}
		printf("  layer %u: %u binding(s), suppress mask %08X\n",
		       (unsigned)l + 1, (unsigned)active,
		       (unsigned)cfg.suppress[l]);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--save-default") == 0) {
		return save_default(argv[2]);
	}
	if (argc == 3 && strcmp(argv[1], "--load") == 0) {
		return load_blob(argv[2]);
	}
	if (argc == 3 && strcmp(argv[1], "--mouse") == 0) {
		/* Replay a blob against a few stick positions and print
		 * what the pointer path actually emits. */
		static core_config cfg;
		static core_state  cs;
		static u8          blob[8192];
		u8                 packet[CORE_RAW_PACKET_BYTES];
		u32                len;
		FILE              *in;
		int                rc, i, k;
		u64                t = 1000;
		static const struct { const char *name; s16 x, y; } POS[] = {
			{ "centre     ",      0,      0 },
			{ "right      ",  32767,      0 },
			{ "left       ", -32767,      0 },
			{ "up         ",      0,  32767 },
			{ "down       ",      0, -32767 },
			{ "up-right   ",  32767,  32767 },
			{ "half right ",  16383,      0 }
		};

		in = fopen(argv[2], "rb");
		if (!in) {
			printf("  cannot read %s\n", argv[2]);
			return 1;
		}
		len = (u32)fread(blob, 1, sizeof(blob), in);
		fclose(in);

		core_init(&cs, recording_sink, NULL);
		rc = core_set_config(&cs, blob, len, NULL);
		if (rc != CORE_CFG_OK) {
			printf("  blob rejected, code %d\n", rc);
			return 1;
		}

		cfg = cs.cfg;
		printf("  stick 0 mode %u   stick 1 mode %u\n",
		       cfg.stick[0].mode, cfg.stick[1].mode);
		printf("  claim %08lX   deadzone %u/%u  speed %u"
		       "  gain %u/%u  invert %u/%u\n",
		       (unsigned long)cfg.stick_claim,
		       cfg.stick[1].deadzone, cfg.stick[1].outer,
		       cfg.stick[1].max_speed,
		       cfg.stick[1].gain_x, cfg.stick[1].gain_y,
		       cfg.stick[1].invert_x, cfg.stick[1].invert_y);
		printf("\n  position      dx      dy   (one second of 4ms packets)\n");

		for (i = 0; i < (int)(sizeof(POS) / sizeof(POS[0])); i++) {
			long dx, dy;

			make_packet(packet);
			put_le16(&packet[CORE_RAW_RSTICK_X], POS[i].x);
			put_le16(&packet[CORE_RAW_RSTICK_Y], POS[i].y);
			for (k = 0; k < 3; k++) {
				t += 4 * CORE_100NS_PER_MS;
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t);
			}
			sink_reset();
			for (k = 0; k < 250; k++) {
				t += 4 * CORE_100NS_PER_MS;
				core_on_packet(&cs, packet,
				               CORE_RAW_PACKET_BYTES, t);
			}
			sink_mouse_total(&dx, &dy);
			printf("  %s %7ld %7ld\n",
			       POS[i].name, dx, dy);
		}
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--ioctls") == 0) {
		/* THE CODES AS THE MACRO BUILDS THEM. A user-mode
		 * tool computes the same arithmetic by hand, and a
		 * mismatch shows up only as a command the driver
		 * says it has never heard of. Print them so the two
		 * can be compared instead of assumed. */
		printf("GET_VERSION   0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_VERSION);
		printf("GET_DEVICES   0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_DEVICES);
		printf("GET_CONFIG    0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_CONFIG);
		printf("GET_STATS     0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_STATS);
		printf("SET_CONFIG    0x%08lX\n",
		       (unsigned long)IOCTL_XC_SET_CONFIG);
		printf("RESET_CONFIG  0x%08lX\n",
		       (unsigned long)IOCTL_XC_RESET_CONFIG);
		printf("SET_RUMBLE    0x%08lX\n",
		       (unsigned long)IOCTL_XC_SET_RUMBLE);
		printf("GET_TRACE     0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_TRACE);
		printf("GET_RAW       0x%08lX\n",
		       (unsigned long)IOCTL_XC_GET_RAW);
		return 0;
	}

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
	printf("[config]\n");       test_config();
	printf("[bindings]\n");     test_bindings();
	printf("[control]\n");      test_control();
	printf("[chords]\n");       test_chords();
	printf("[autofire]\n");     test_autofire();
	printf("[sticks]\n");       test_sticks();
	printf("[rumble]\n");       test_rumble();
	printf("[readfit]\n");      test_readfit();
	printf("[registration]\n"); test_driver_entry();

	printf("---------------\n");
	printf("%d checks, %d failure(s)\n", g_Checks, g_Failures);

	return (g_Failures == 0) ? 0 : 1;
}
