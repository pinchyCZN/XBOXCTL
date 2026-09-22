/*
 * core.c - OS-free logic for the XBOXCTL driver.
 *
 * See core.h for the rule this file obeys: no Windows header, no kernel call,
 * no Windows type. The clock arrives as an argument and every report leaves
 * through the sink.
 */
#include "core.h"

/* ======================================================================
 * THE COMPOSITE HID DESCRIPTOR
 *
 * Three top-level application collections, in this order:
 *
 *     Col01   Gamepad,  report ID 1     24 buttons, 7 axes, a hat
 *     Col02   Keyboard, report ID 2     8 modifiers, 6 key slots
 *     Col03   Mouse,    report ID 3     5 buttons, 16-bit X/Y, wheel, pan
 *
 * THE GAMEPAD GOES FIRST because it is the collection a user re-binds
 * inside a game, and hidclass numbers the child devnodes in descriptor
 * order.
 *
 * The rumble output and the two feature reports live INSIDE the gamepad's
 * application collection rather than in a fourth top-level one. A fourth
 * collection would make hidclass create a fourth devnode for something that
 * is not an input device.
 * ====================================================================== */

static const u8 CORE_HID_DESCRIPTOR_BYTES[] = {
	/* ---- Gamepad, report ID 1 --------------------------- 135 bytes */
	0x05, 0x01,             /* Usage Page (Generic Desktop)            */
	0x09, 0x05,             /* Usage (Gamepad)                         */
	0xA1, 0x01,             /* Collection (Application)                */
	0x85, 0x01,             /*   Report ID (1)                         */

	0x05, 0x09,             /*   Usage Page (Button)                   */
	0x19, 0x01,             /*   Usage Minimum (1)                     */
	0x29, 0x18,             /*   Usage Maximum (24)                    */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x25, 0x01,             /*   Logical Maximum (1)                   */
	0x75, 0x01,             /*   Report Size (1)                       */
	0x95, 0x18,             /*   Report Count (24)                     */
	0x81, 0x02,             /*   Input (Data,Var,Abs) - 3 bytes        */

	0x05, 0x01,             /*   Usage Page (Generic Desktop)          */
	0x09, 0x01,             /*   Usage (Pointer)                       */
	0xA1, 0x00,             /*   Collection (Physical)                 */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767)            */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767)             */
	0x75, 0x10,             /*     Report Size (16)                    */
	0x95, 0x07,             /*     Report Count (7)                    */
	0x09, 0x30,             /*     Usage (X)                           */
	0x09, 0x31,             /*     Usage (Y)                           */
	0x09, 0x32,             /*     Usage (Z)                           */
	0x09, 0x33,             /*     Usage (Rx)                          */
	0x09, 0x34,             /*     Usage (Ry)                          */
	0x09, 0x35,             /*     Usage (Rz)                          */
	0x09, 0x36,             /*     Usage (Slider)                      */
	0x81, 0x02,             /*     Input (Data,Var,Abs) - 14 bytes     */
	0xC0,                   /*   End Collection                        */

	/*
	 * The hat. Logical range is 0..7 and the NULL STATE bit in the Input
	 * item is what makes an out-of-range value mean "centred" - so 8 is a
	 * legal way to say no direction rather than an out-of-range 0..7.
	 * Without Null the same byte is simply invalid.
	 */
	0x05, 0x01,             /*   Usage Page (Generic Desktop)          */
	0x09, 0x39,             /*   Usage (Hat switch)                    */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x25, 0x07,             /*   Logical Maximum (7)                   */
	0x35, 0x00,             /*   Physical Minimum (0)                  */
	0x46, 0x3B, 0x01,       /*   Physical Maximum (315)                */
	0x65, 0x14,             /*   Unit (Eng Rot: Degrees)               */
	0x75, 0x04,             /*   Report Size (4)                       */
	0x95, 0x01,             /*   Report Count (1)                      */
	0x81, 0x42,             /*   Input (Data,Var,Abs,NULL STATE)       */

	0x65, 0x00,             /*   Unit (None) - do not leak the unit    */
	0x75, 0x04,             /*   Report Size (4)                       */
	0x95, 0x01,             /*   Report Count (1)                      */
	0x81, 0x01,             /*   Input (Const) - pad the hat byte      */

	/*
	 * The diagnostic tail: the active layout, then the raw 20-byte packet
	 * verbatim. A configurator gets the mapped output and the physical
	 * truth from one HID read, with no second channel to open.
	 */
	0x09, 0x3A,             /*   Usage (Counted Buffer)                */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                 */
	0x75, 0x08,             /*   Report Size (8)                       */
	0x95, 0x15,             /*   Report Count (21)                     */
	0x81, 0x02,             /*   Input (Data,Var,Abs)                  */

	/*
	 * Output and feature reports, inside this collection so no extra
	 * devnode is created for them.
	 *
	 *     ID 4  Output   2 bytes   left and right rumble actuator
	 *     ID 5  Feature  4096      the configuration blob
	 *     ID 6  Feature  7 bytes   signature and driver version
	 */
	0x06, 0x00, 0xFF,       /*   Usage Page (Vendor Defined FF00)      */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                 */
	0x75, 0x08,             /*   Report Size (8)                       */

	0x85, 0x04,             /*   Report ID (4)                         */
	0x09, 0x01,             /*   Usage (Vendor 1)                      */
	0x95, 0x02,             /*   Report Count (2)                      */
	0x91, 0x02,             /*   Output (Data,Var,Abs) - rumble        */

	0x85, 0x05,             /*   Report ID (5)                         */
	0x09, 0x02,             /*   Usage (Vendor 2)                      */
	0x96, 0x00, 0x10,       /*   Report Count (4096)                   */
	0xB1, 0x02,             /*   Feature (Data,Var,Abs) - config blob  */

	0x85, 0x06,             /*   Report ID (6)                         */
	0x09, 0x03,             /*   Usage (Vendor 3)                      */
	0x95, 0x07,             /*   Report Count (7)                      */
	0xB1, 0x02,             /*   Feature (Data,Var,Abs) - version      */
	0xC0,                   /* End Collection                          */

	/* ---- Keyboard, report ID 2 --------------------------- 67 bytes */
	0x05, 0x01,             /* Usage Page (Generic Desktop)            */
	0x09, 0x06,             /* Usage (Keyboard)                        */
	0xA1, 0x01,             /* Collection (Application)                */
	0x85, 0x02,             /*   Report ID (2)                         */
	0x05, 0x07,             /*   Usage Page (Keyboard/Keypad)          */
	0x19, 0xE0,             /*   Usage Minimum (0xE0, LeftControl)     */
	0x29, 0xE7,             /*   Usage Maximum (0xE7, RightGUI)        */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x25, 0x01,             /*   Logical Maximum (1)                   */
	0x75, 0x01,             /*   Report Size (1)                       */
	0x95, 0x08,             /*   Report Count (8)                      */
	0x81, 0x02,             /*   Input (Data,Var,Abs) - modifier byte  */
	0x95, 0x01,             /*   Report Count (1)                      */
	0x75, 0x08,             /*   Report Size (8)                       */
	0x81, 0x01,             /*   Input (Const) - the reserved byte     */

	/*
	 * The LED output report is genuine: Windows really does send Num Lock
	 * and Caps Lock changes down. They are accepted and discarded, which
	 * matches the hardware. Refusing them makes Windows decide the
	 * keyboard is broken.
	 */
	0x95, 0x05,             /*   Report Count (5)                      */
	0x75, 0x01,             /*   Report Size (1)                       */
	0x05, 0x08,             /*   Usage Page (LED)                      */
	0x19, 0x01,             /*   Usage Minimum (1, NumLock)            */
	0x29, 0x05,             /*   Usage Maximum (5, Kana)               */
	0x91, 0x02,             /*   Output (Data,Var,Abs) - five LEDs     */
	0x95, 0x01,             /*   Report Count (1)                      */
	0x75, 0x03,             /*   Report Size (3)                       */
	0x91, 0x01,             /*   Output (Const) - pad the LED byte     */

	0x95, 0x06,             /*   Report Count (6) - SIX key slots      */
	0x75, 0x08,             /*   Report Size (8)                       */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xA4, 0x00,       /*   Logical Maximum (0xA4)                */
	0x05, 0x07,             /*   Usage Page (Keyboard/Keypad)          */
	0x19, 0x00,             /*   Usage Minimum (0)                     */
	0x2A, 0xA4, 0x00,       /*   Usage Maximum (0xA4)                  */
	0x81, 0x00,             /*   Input (Data,ARRAY)                    */
	0xC0,                   /* End Collection                          */

	/* ---- Mouse, report ID 3 ------------------------------ 77 bytes */
	0x05, 0x01,             /* Usage Page (Generic Desktop)            */
	0x09, 0x02,             /* Usage (Mouse)                           */
	0xA1, 0x01,             /* Collection (Application)                */
	0x09, 0x01,             /*   Usage (Pointer)                       */
	0xA1, 0x00,             /*   Collection (Physical)                 */
	0x85, 0x03,             /*     Report ID (3)                       */
	0x05, 0x09,             /*     Usage Page (Button)                 */
	0x19, 0x01,             /*     Usage Minimum (1)                   */
	0x29, 0x05,             /*     Usage Maximum (5)                   */
	0x15, 0x00,             /*     Logical Minimum (0)                 */
	0x25, 0x01,             /*     Logical Maximum (1)                 */
	0x75, 0x01,             /*     Report Size (1)                     */
	0x95, 0x05,             /*     Report Count (5) - FIVE buttons     */
	0x81, 0x02,             /*     Input (Data,Var,Abs)                */
	0x75, 0x03,             /*     Report Size (3)                     */
	0x95, 0x01,             /*     Report Count (1)                    */
	0x81, 0x01,             /*     Input (Const) - pad to a byte       */

	0x05, 0x01,             /*     Usage Page (Generic Desktop)        */
	0x09, 0x30,             /*     Usage (X)                           */
	0x09, 0x31,             /*     Usage (Y)                           */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767)            */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767)             */
	0x75, 0x10,             /*     Report Size (16) - SIXTEEN BITS     */
	0x95, 0x02,             /*     Report Count (2)                    */
	0x81, 0x06,             /*     Input (Data,Var,REL)                */

	0x09, 0x38,             /*     Usage (Wheel)                       */
	0x15, 0x81,             /*     Logical Minimum (-127)              */
	0x25, 0x7F,             /*     Logical Maximum (127)               */
	0x75, 0x08,             /*     Report Size (8)                     */
	0x95, 0x01,             /*     Report Count (1)                    */
	0x81, 0x06,             /*     Input (Data,Var,REL)                */

	0x05, 0x0C,             /*     Usage Page (Consumer)               */
	0x0A, 0x38, 0x02,       /*     Usage (AC Pan)                      */
	0x75, 0x08,             /*     Report Size (8)                     */
	0x95, 0x01,             /*     Report Count (1)                    */
	0x81, 0x06,             /*     Input (Data,Var,REL)                */
	0xC0,                   /*   End Collection                        */
	0xC0                    /* End Collection                          */
};

const u8 *core_hid_descriptor(u32 *length)
{
	if (length != 0) {
		*length = (u32)sizeof(CORE_HID_DESCRIPTOR_BYTES);
	}
	return CORE_HID_DESCRIPTOR_BYTES;
}

/* ======================================================================
 * SMALL HELPERS
 * ====================================================================== */

static void core_zero(void *dst, u32 len)
{
	u8 *p = (u8 *)dst;
	u32 i;

	for (i = 0; i < len; i++) {
		p[i] = 0;
	}
}

static void core_copy(void *dst, const void *src, u32 len)
{
	u8       *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;
	u32       i;

	for (i = 0; i < len; i++) {
		d[i] = s[i];
	}
}

static int core_differs(const void *a, const void *b, u32 len)
{
	const u8 *x = (const u8 *)a;
	const u8 *y = (const u8 *)b;
	u32       i;

	for (i = 0; i < len; i++) {
		if (x[i] != y[i]) {
			return 1;
		}
	}
	return 0;
}

static s32 core_clamp(s32 v, s32 lo, s32 hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/* Little-endian signed 16-bit store. */
static void core_put16(u8 *at, s32 value)
{
	at[0] = (u8)(value & 0xFF);
	at[1] = (u8)((value >> 8) & 0xFF);
}

/* Little-endian signed 16-bit load. */
static s32 core_get16(const u8 *at)
{
	return (s32)(s16)((u16)at[0] | ((u16)at[1] << 8));
}

/*
 * Integer square root of a 64-bit value, by bit-by-bit restoring division.
 *
 * SIXTY-FOUR BITS IS NOT OPTIONAL HERE. Two axes at CORE_MAX_VALUE give
 * x*x + y*y = 2.45e9, which does not fit a signed 32-bit integer; computing
 * the sum in 32 bits wraps negative and yields a radius near 42900 instead
 * of 49497, so every radial test near full diagonal deflection is wrong.
 */
u32 core_isqrt64(u64 value)
{
	u64 rem = 0;
	u64 root = 0;
	int i;

	for (i = 0; i < 32; i++) {
		root <<= 1;
		rem = (rem << 2) | (value >> 62);
		value <<= 2;
		if (root < rem) {
			root++;
			rem -= root;
			root++;
		}
	}
	return (u32)(root >> 1);
}

/*
 * Clamp a stick pair to a circle of radius CORE_MAX_VALUE, preserving the
 * direction. A real stick's envelope is not perfectly round and the decode
 * scales each axis independently, so a diagonal can land outside the circle;
 * without this the corners read as further than full deflection.
 */
void core_crop_vector(s32 *x, s32 *y)
{
	s64 xs = (s64)(*x) * (s64)(*x);
	s64 ys = (s64)(*y) * (s64)(*y);
	u32 radius = core_isqrt64((u64)(xs + ys));

	if (radius > (u32)CORE_MAX_VALUE) {
		*x = (s32)(((s64)(*x) * CORE_MAX_VALUE) / (s64)radius);
		*y = (s32)(((s64)(*y) * CORE_MAX_VALUE) / (s64)radius);
	}
}

/* ======================================================================
 * DECODE - RAW PACKET TO SEMIAXES
 * ====================================================================== */

/*
 * Scale a signed 16-bit stick axis into CORE_MAX_VALUE units.
 *
 * The two directions divide by different constants so that BOTH extremes
 * land on CORE_MAX_VALUE in magnitude, despite the signed range being
 * asymmetric.
 */
static s32 core_scale_axis(s32 raw)
{
	if (raw < 0) {
		return (s32)(((s64)CORE_MAX_VALUE * raw) / 32768);
	}
	return (s32)(((s64)CORE_MAX_VALUE * raw) / 32767);
}

/* Split one bipolar axis into the negative and positive semiaxis pair. */
static void core_split_axis(s32 *sa, int index_neg, s32 value)
{
	sa[index_neg]     = (value < 0) ? -value : 0;
	sa[index_neg + 1] = (value > 0) ?  value : 0;
}

static void core_decode_stick(core_state *cs, u32 raw_x_at, u32 raw_y_at,
                              int sa_xneg, int sa_yneg)
{
	s32 x = core_scale_axis(core_get16(&cs->raw[raw_x_at]));
	s32 y = core_scale_axis(core_get16(&cs->raw[raw_y_at]));

	/*
	 * NEGATE Y ONCE, HERE. The pad reports up-positive and HID is
	 * down-positive. Doing it at the point of decode means nothing below
	 * this line has to remember which convention it is looking at.
	 */
	y = -y;

	core_crop_vector(&x, &y);

	x = core_clamp(x, -CORE_MAX_VALUE, CORE_MAX_VALUE);
	y = core_clamp(y, -CORE_MAX_VALUE, CORE_MAX_VALUE);

	core_split_axis(cs->semiaxis, sa_xneg, x);
	core_split_axis(cs->semiaxis, sa_yneg, y);
}

/* A digital button is either off or at full scale; there is nothing between. */
static s32 bit(u8 digital, u8 mask)
{
	return (digital & mask) ? CORE_MAX_VALUE : 0;
}

static void core_decode(core_state *cs)
{
	u8  digital = cs->raw[CORE_RAW_DIGITAL];
	int i;

	cs->semiaxis[CORE_SA_DPAD_UP]    = bit(digital, CORE_DIG_DPAD_UP);
	cs->semiaxis[CORE_SA_DPAD_DOWN]  = bit(digital, CORE_DIG_DPAD_DOWN);
	cs->semiaxis[CORE_SA_DPAD_LEFT]  = bit(digital, CORE_DIG_DPAD_LEFT);
	cs->semiaxis[CORE_SA_DPAD_RIGHT] = bit(digital, CORE_DIG_DPAD_RIGHT);
	cs->semiaxis[CORE_SA_START]      = bit(digital, CORE_DIG_START);
	cs->semiaxis[CORE_SA_BACK]       = bit(digital, CORE_DIG_BACK);
	cs->semiaxis[CORE_SA_LTHUMB]     = bit(digital, CORE_DIG_LTHUMB);
	cs->semiaxis[CORE_SA_RTHUMB]     = bit(digital, CORE_DIG_RTHUMB);

	/*
	 * A B X Y Black White and both triggers are ANALOG on this pad,
	 * 0..255 of real pressure. They occupy eight consecutive bytes and
	 * eight consecutive semiaxes, in the same order.
	 */
	for (i = 0; i < 8; i++) {
		u32 raw = cs->raw[CORE_RAW_ANALOG_BASE + i];
		cs->semiaxis[CORE_SA_A + i] = (s32)((CORE_MAX_VALUE * raw) / 255);
	}

	core_decode_stick(cs, CORE_RAW_LSTICK_X, CORE_RAW_LSTICK_Y,
	                  CORE_SA_LSTICK_XNEG, CORE_SA_LSTICK_YNEG);
	core_decode_stick(cs, CORE_RAW_RSTICK_X, CORE_RAW_RSTICK_Y,
	                  CORE_SA_RSTICK_XNEG, CORE_SA_RSTICK_YNEG);

	/* Guide is a 360 control; the original pad has no equivalent. */
	cs->semiaxis[CORE_SA_GUIDE] = 0;
}

/* ======================================================================
 * THE GAMEPAD REPORT
 *
 * Until the binding table exists this is a fixed pass-through map. It is
 * what makes the pad work as an ordinary game controller with no
 * configuration at all, and it is the behaviour the binding engine will
 * install as its default layout.
 * ====================================================================== */

/*
 * The four D-pad direction bits index this table.
 *
 * Seven of the sixteen combinations are impossible or contradictory - up and
 * down together, three directions at once - and every one maps to centred.
 * Resolving them in a table rather than in code means a different resolution
 * is an edit to sixteen numbers.
 *
 *     index = up | down<<1 | left<<2 | right<<3
 */
static const u8 CORE_HAT_TABLE[16] = {
	8, 0, 4, 8, 6, 7, 5, 8,
	2, 1, 3, 8, 8, 8, 8, 8
};

/* Activation point for an analog control used as a button. */
#define CORE_DEFAULT_BUTTON_ON  ((CORE_MAX_VALUE * 10) / 255)

/*
 * Semiaxis to HID button, for the default map. Index is the HID button
 * number minus one; the value is the semiaxis that drives it.
 */
static const u8 CORE_DEFAULT_BUTTONS[12] = {
	CORE_SA_A,        CORE_SA_B,        CORE_SA_X,        CORE_SA_Y,
	CORE_SA_BLACK,    CORE_SA_WHITE,    CORE_SA_LTRIGGER, CORE_SA_RTRIGGER,
	CORE_SA_START,    CORE_SA_BACK,     CORE_SA_LTHUMB,   CORE_SA_RTHUMB
};

/*
 * Each output axis is the difference of a semiaxis pair. The table gives
 * the NEGATIVE half; the positive half is the next index. CORE_SA_GUIDE
 * marks an axis nothing drives, which reads as centred.
 */
static const u8 CORE_DEFAULT_AXES[CORE_GP_AXIS_COUNT] = {
	CORE_SA_LSTICK_XNEG,    /* X      */
	CORE_SA_LSTICK_YNEG,    /* Y      */
	CORE_SA_RSTICK_XNEG,    /* Z      */
	CORE_SA_RSTICK_YNEG,    /* Rx     */
	CORE_SA_GUIDE,          /* Ry     */
	CORE_SA_GUIDE,          /* Rz     */
	CORE_SA_GUIDE           /* Slider */
};

static void core_build_gamepad(core_state *cs, u8 *payload)
{
	int i;
	int hat_index;

	core_zero(payload, CORE_GAMEPAD_PAYLOAD);

	for (i = 0; i < 12; i++) {
		if (cs->semiaxis[CORE_DEFAULT_BUTTONS[i]] >= CORE_DEFAULT_BUTTON_ON) {
			payload[CORE_GP_BUTTONS + (i >> 3)] |= (u8)(1u << (i & 7));
		}
	}

	for (i = 0; i < CORE_GP_AXIS_COUNT; i++) {
		s32 value = 0;

		if (CORE_DEFAULT_AXES[i] != CORE_SA_GUIDE) {
			s32 neg = cs->semiaxis[CORE_DEFAULT_AXES[i]];
			s32 pos = cs->semiaxis[CORE_DEFAULT_AXES[i] + 1];
			value = (s32)(((s64)(pos - neg) * CORE_OUT_AXIS_SCALE)
			              / CORE_MAX_VALUE);
			value = core_clamp(value, -CORE_OUT_AXIS_SCALE,
			                   CORE_OUT_AXIS_SCALE);
		}
		core_put16(&payload[CORE_GP_AXES + i * 2], value);
	}

	hat_index = 0;
	if (cs->semiaxis[CORE_SA_DPAD_UP] > 0)    { hat_index |= 1; }
	if (cs->semiaxis[CORE_SA_DPAD_DOWN] > 0)  { hat_index |= 2; }
	if (cs->semiaxis[CORE_SA_DPAD_LEFT] > 0)  { hat_index |= 4; }
	if (cs->semiaxis[CORE_SA_DPAD_RIGHT] > 0) { hat_index |= 8; }
	payload[CORE_GP_HAT] = CORE_HAT_TABLE[hat_index];

	payload[CORE_GP_LAYOUT] = (u8)(cs->layout + 1);

	core_copy(&payload[CORE_GP_RAW], cs->raw, CORE_RAW_PACKET_BYTES);
}

static void core_emit(core_state *cs, u8 report_id, const u8 *payload, u32 len)
{
	cs->reports_emitted++;
	if (cs->sink != 0) {
		cs->sink(cs->sink_ctx, report_id, payload, len);
	}
}

/*
 * EMIT ONLY ON CHANGE. The raw tail is part of the comparison, so a packet
 * whose mapped output is identical but whose stick moved by one count still
 * produces a report - which is what a configurator watching the raw bytes
 * needs.
 */
static void core_emit_gamepad(core_state *cs)
{
	u8 payload[CORE_GAMEPAD_PAYLOAD];

	core_build_gamepad(cs, payload);

	if (cs->gp_last_valid &&
	    !core_differs(payload, cs->gp_last, CORE_GAMEPAD_PAYLOAD)) {
		return;
	}

	core_copy(cs->gp_last, payload, CORE_GAMEPAD_PAYLOAD);
	cs->gp_last_valid = 1;

	core_emit(cs, CORE_REPORT_ID_GAMEPAD, payload, CORE_GAMEPAD_PAYLOAD);
}

/* ======================================================================
 * THE KEYBOARD STATE MACHINE
 *
 * A HID keyboard report carries the set of keys CURRENTLY HELD, so a press
 * and a release inside one report are invisible: the host sees the before
 * state and the after state and they are identical. Every transition has to
 * be its own submitted report, which is why this emits per event rather
 * than once per tick.
 * ====================================================================== */

static void core_emit_keyboard(core_state *cs)
{
	u8  payload[CORE_KEYBOARD_PAYLOAD];
	int i;

	core_zero(payload, CORE_KEYBOARD_PAYLOAD);
	payload[CORE_KB_MODIFIERS] = cs->kb.modifiers;

	if (cs->kb.count > CORE_KEY_SLOTS) {
		/*
		 * Over six keys held, every slot carries ErrorRollOver while
		 * the real list keeps being tracked internally. Releasing back
		 * down to six restores it with no fresh press needed.
		 */
		for (i = 0; i < CORE_KEY_SLOTS; i++) {
			payload[CORE_KB_KEYS + i] = CORE_KEY_ROLLOVER;
		}
	} else {
		for (i = 0; i < (int)cs->kb.count; i++) {
			payload[CORE_KB_KEYS + i] = cs->kb.keys[i];
		}
	}

	core_emit(cs, CORE_REPORT_ID_KEYBOARD, payload, CORE_KEYBOARD_PAYLOAD);
}

void core_key_event(core_state *cs, u8 usage, int down)
{
	int i;
	int at;

	/* Usage 0 releases everything. */
	if (usage == 0) {
		if (cs->kb.modifiers == 0 && cs->kb.count == 0) {
			return;
		}
		cs->kb.modifiers = 0;
		cs->kb.count = 0;
		core_emit_keyboard(cs);
		return;
	}

	if (usage >= CORE_KEY_MOD_FIRST && usage <= CORE_KEY_MOD_LAST) {
		u8 bit = (u8)(1u << (usage - CORE_KEY_MOD_FIRST));
		u8 want = down ? bit : 0;

		if ((cs->kb.modifiers & bit) == want) {
			return;             /* already in that state */
		}
		cs->kb.modifiers = (u8)((cs->kb.modifiers & ~bit) | want);
		core_emit_keyboard(cs);
		return;
	}

	if (usage < CORE_KEY_FIRST || usage > CORE_KEY_LAST) {
		return;                 /* outside both accepted ranges */
	}

	at = -1;
	for (i = 0; i < (int)cs->kb.count; i++) {
		if (cs->kb.keys[i] == usage) {
			at = i;
			break;
		}
	}

	if (down) {
		if (at >= 0) {
			return;             /* already held */
		}
		if (cs->kb.count >= CORE_KEY_TRACK_MAX) {
			return;             /* cannot happen: see core.h */
		}
		cs->kb.keys[cs->kb.count] = usage;
		cs->kb.count++;
	} else {
		if (at < 0) {
			return;             /* not held */
		}
		/*
		 * KEEP THE ARRAY DENSE AND ORDERED BY PRESS TIME: shift the
		 * tail down over the gap. Swapping the last entry into it is
		 * the obvious implementation and it is the wrong one - the
		 * order is what makes the six reported slots the six OLDEST
		 * keys held rather than an arbitrary six.
		 */
		for (i = at; i < (int)cs->kb.count - 1; i++) {
			cs->kb.keys[i] = cs->kb.keys[i + 1];
		}
		cs->kb.count--;
	}

	core_emit_keyboard(cs);
}

/* ======================================================================
 * THE MOUSE
 * ====================================================================== */

static void core_emit_mouse(core_state *cs)
{
	u8 payload[CORE_MOUSE_PAYLOAD];

	core_zero(payload, CORE_MOUSE_PAYLOAD);
	payload[CORE_MS_BUTTONS] = cs->ms.buttons;
	core_put16(&payload[CORE_MS_X], core_clamp(cs->ms.dx, -CORE_OUT_AXIS_SCALE,
	                                           CORE_OUT_AXIS_SCALE));
	core_put16(&payload[CORE_MS_Y], core_clamp(cs->ms.dy, -CORE_OUT_AXIS_SCALE,
	                                           CORE_OUT_AXIS_SCALE));
	payload[CORE_MS_WHEEL] = (u8)(s8)core_clamp(cs->ms.wheel, -127, 127);
	payload[CORE_MS_PAN]   = (u8)(s8)core_clamp(cs->ms.pan, -127, 127);

	/* Movement is relative: once reported it is spent. */
	cs->ms.dx = 0;
	cs->ms.dy = 0;
	cs->ms.wheel = 0;
	cs->ms.pan = 0;

	core_emit(cs, CORE_REPORT_ID_MOUSE, payload, CORE_MOUSE_PAYLOAD);
}

void core_mouse_button(core_state *cs, u8 button, int down)
{
	u8 bit;
	u8 want;

	if (button < 1 || button > CORE_MOUSE_BUTTONS) {
		return;
	}
	bit  = (u8)(1u << (button - 1));
	want = down ? bit : 0;

	if ((cs->ms.buttons & bit) == want) {
		return;
	}
	cs->ms.buttons = (u8)((cs->ms.buttons & ~bit) | want);
	core_emit_mouse(cs);
}

void core_mouse_move(core_state *cs, s32 dx, s32 dy)
{
	/*
	 * NEVER EMIT AN ALL-ZERO MOVE REPORT. A tick with no motion and no
	 * button change is not news, and at 250 Hz emitting one anyway is 250
	 * pointless reports a second through mouclass for as long as the
	 * driver is loaded.
	 */
	if (dx == 0 && dy == 0) {
		return;
	}
	cs->ms.dx += dx;
	cs->ms.dy += dy;
	core_emit_mouse(cs);
}

void core_mouse_wheel(core_state *cs, s32 detents, s32 pan)
{
	if (detents == 0 && pan == 0) {
		return;
	}
	cs->ms.wheel += detents;
	cs->ms.pan   += pan;
	core_emit_mouse(cs);
}

/* ======================================================================
 * LIFECYCLE
 * ====================================================================== */

void core_init(core_state *cs, core_report_fn sink, void *sink_ctx)
{
	core_zero(cs, (u32)sizeof(*cs));
	cs->sink = sink;
	cs->sink_ctx = sink_ctx;
}

void core_release_all(core_state *cs)
{
	int i;

	/*
	 * Release everything that is being held, and emit the reports that
	 * implies. Anywhere a binding holding a key could vanish underneath it
	 * - a layer change, a configuration swap, device stop - must come
	 * through here, or the key stays down forever with nothing left to
	 * release it.
	 */
	core_key_event(cs, 0, 0);

	for (i = 1; i <= CORE_MOUSE_BUTTONS; i++) {
		core_mouse_button(cs, (u8)i, 0);
	}

	cs->ms.dx = 0;
	cs->ms.dy = 0;
	cs->ms.wheel = 0;
	cs->ms.pan = 0;
}

void core_on_packet(core_state *cs, const u8 *raw, u32 len, u64 now_100ns)
{
	if (len != CORE_RAW_PACKET_BYTES ||
	    raw[CORE_RAW_TYPE] != CORE_RAW_TYPE_INPUT) {
		cs->packets_rejected++;
		return;
	}

	core_copy(cs->raw, raw, CORE_RAW_PACKET_BYTES);
	cs->raw_valid = 1;
	cs->packets_accepted++;

	cs->last_packet_100ns = now_100ns;
	if (!cs->clock_valid) {
		cs->last_tick_100ns = now_100ns;
		cs->clock_valid = 1;
	}

	core_decode(cs);
	core_emit_gamepad(cs);
}

void core_tick(core_state *cs, u64 now_100ns)
{
	u64 elapsed;
	u32 elapsed_ms;

	if (!cs->clock_valid) {
		cs->last_tick_100ns = now_100ns;
		cs->clock_valid = 1;
		return;
	}

	elapsed = now_100ns - cs->last_tick_100ns;
	cs->last_tick_100ns = now_100ns;

	elapsed_ms = (u32)(elapsed / CORE_100NS_PER_MS);

	/*
	 * CLAMP THE INTERVAL. A resume from sleep, a debugger break or DPC
	 * starvation can hand this a gap of seconds; multiplying that by a
	 * pointer velocity flings the cursor across the desktop. Anything
	 * longer than the clamp is a discontinuity, not motion.
	 */
	if (elapsed_ms > CORE_MAX_TICK_MS) {
		elapsed_ms = CORE_MAX_TICK_MS;
	}

	/*
	 * Autofire deadlines and the mouse velocity accumulator both live
	 * here. Neither exists until the binding table does; see
	 * ../docs/mapping-engine.txt section 6 and
	 * ../docs/analog-to-mouse.txt section 3.2.
	 */
	(void)elapsed_ms;
}
