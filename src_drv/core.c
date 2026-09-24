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
 *     Col01   Gamepad,  report ID 1     16 buttons, 6 axes, a hat
 *     Col02   Keyboard, report ID 2     8 modifiers, 6 key slots
 *     Col03   Mouse,    report ID 3     5 buttons, X, Y and wheel
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
	0x75, 0x10,             /*     Report Size (16)                    */
	0x95, 0x02,             /*     Report Count (2)                    */
	0x81, 0x06,             /*     Input (Data,Var,REL)                */

	0x09, 0x38,             /*     Usage (Wheel)                       */
	0x15, 0x81,             /*     Logical Minimum (-127)              */
	0x25, 0x7F,             /*     Logical Maximum (127)               */
	0x75, 0x08,             /*     Report Size (8)                     */
	0x95, 0x01,             /*     Report Count (1)                    */
	0x81, 0x06,             /*     Input (Data,Var,REL)                */
	0xC0,                   /*   End Collection                        */
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

	/* ---- Gamepad, report ID 1 --------------------------- 135 bytes */
	0x05, 0x01,             /* Usage Page (Generic Desktop)            */
	0x09, 0x05,             /* Usage (Gamepad)                         */
	0xA1, 0x01,             /* Collection (Application)                */
	0x85, 0x01,             /*   Report ID (1)                         */

	0x05, 0x09,             /*   Usage Page (Button)                   */
	0x19, 0x01,             /*   Usage Minimum (1)                     */
	0x29, 0x10,             /*   Usage Maximum (16)                    */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x25, 0x01,             /*   Logical Maximum (1)                   */
	0x75, 0x01,             /*   Report Size (1)                       */
	0x95, 0x10,             /*   Report Count (16) - EXACTLY 2 bytes   */
	0x81, 0x02,             /*   Input (Data,Var,Abs)                  */

	0x05, 0x01,             /*   Usage Page (Generic Desktop)          */
	0x09, 0x01,             /*   Usage (Pointer)                       */
	0xA1, 0x00,             /*   Collection (Physical)                 */
	0x16, 0x01, 0x80,       /*     Logical Minimum (-32767)            */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767)             */
	0x75, 0x10,             /*     Report Size (16)                    */
	0x95, 0x04,             /*     Report Count (4)                    */
	0x09, 0x30,             /*     Usage (X)  - left stick horizontal  */
	0x09, 0x31,             /*     Usage (Y)  - left stick vertical    */
	0x09, 0x33,             /*     Usage (Rx) - right stick horizontal */
	0x09, 0x34,             /*     Usage (Ry) - right stick vertical   */
	0x81, 0x02,             /*     Input (Data,Var,Abs) - 8 bytes      */
	0xC0,                   /*   End Collection                        */

	/*
	 * THE TRIGGERS, AS A SEPARATE ITEM WITH THEIR OWN RANGE. They are
	 * unipolar - released is zero, not centre - so sharing the sticks
	 * range of -32767..32767 would rest them in the middle of their bar
	 * and waste half of it. A second item costs ten bytes and reads the
	 * way a trigger should.
	 */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xFF, 0x7F,       /*   Logical Maximum (32767)               */
	0x75, 0x10,             /*   Report Size (16)                      */
	0x95, 0x02,             /*   Report Count (2)                      */
	0x09, 0x32,             /*   Usage (Z)  - left trigger             */
	0x09, 0x35,             /*   Usage (Rz) - right trigger            */
	0x81, 0x02,             /*   Input (Data,Var,Abs) - 4 bytes        */

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
	 * The rumble actuators, inside this collection so no extra devnode
	 * is created for them.
	 *
	 *     ID 4  Output   2 bytes   left and right rumble actuator
	 *
	 * NO FEATURE REPORTS ARE DECLARED. Configuration does not travel
	 * over HID at all: it arrives on the private control device of
	 * ../docs/driver-plan.txt section 7. One channel carries it, so
	 * there is one place to validate it.
	 */
	0x06, 0x00, 0xFF,       /*   Usage Page (Vendor Defined FF00)      */
	0x15, 0x00,             /*   Logical Minimum (0)                   */
	0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                 */
	0x75, 0x08,             /*   Report Size (8)                       */

	0x85, 0x04,             /*   Report ID (4)                         */
	0x09, 0x01,             /*   Usage (Vendor 1)                      */
	0x95, 0x02,             /*   Report Count (2)                      */
	0x91, 0x02,             /*   Output (Data,Var,Abs) - rumble        */
	0xC0,                   /* End Collection                          */
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

/*
 * Semiaxis to HID button, for the default map. Index is the HID button
 * number minus one; the value is the semiaxis that drives it.
 */
/*
 * THE NUMBERING IS XBCD'S, so a profile or a habit built on that driver
 * transfers unchanged. It is not the order the controls sit in the raw
 * packet: Start and Back come before the thumb clicks, and the triggers
 * come last.
 *
 * THE TRIGGERS ARE HERE AS WELL AS ON Z AND Rz. The axes are what carry
 * their pressure, but plenty of older titles read only buttons, and one
 * source driving two destinations costs nothing.
 *
 * THE D-PAD IS DELIBERATELY ABSENT. It is the hat; declaring it twice
 * makes a game bind one press to two different things.
 */
static const u8 CORE_DEFAULT_BUTTONS[12] = {
	CORE_SA_A,        CORE_SA_B,      CORE_SA_X,        CORE_SA_Y,
	CORE_SA_BLACK,    CORE_SA_WHITE,  CORE_SA_START,    CORE_SA_BACK,
	CORE_SA_LTHUMB,   CORE_SA_RTHUMB, CORE_SA_LTRIGGER, CORE_SA_RTRIGGER
};

/*
 * Each output axis is the difference of a semiaxis pair. The table gives
 * the NEGATIVE half; the positive half is the next index. CORE_SA_GUIDE
 * marks an axis nothing drives, which reads as centred.
 */
/*
 * One entry per declared axis, in descriptor order. An axis is the
 * difference of two semiaxes; a neg of CORE_SA_NONE makes it unipolar,
 * which is what a trigger is.
 *
 * X/Y FOR THE LEFT STICK AND Rx/Ry FOR THE RIGHT is the convention every
 * DirectInput title that auto-maps a gamepad expects. Putting the right
 * stick anywhere else - on Z, say - leaves it unreachable in anything
 * that does not offer manual binding.
 *
 * Z AND Rz CARRY THE TRIGGERS, which is what makes the pad's pressure
 * reach an application at all: a HID button is one bit and has nothing
 * to say about how hard it was pressed.
 */
static const struct {
	u8 neg;
	u8 pos;
} CORE_DEFAULT_AXES[CORE_GP_AXIS_COUNT] = {
	{ CORE_SA_LSTICK_XNEG, CORE_SA_LSTICK_XPOS },   /* X  left stick    */
	{ CORE_SA_LSTICK_YNEG, CORE_SA_LSTICK_YPOS },   /* Y  left stick    */
	{ CORE_SA_RSTICK_XNEG, CORE_SA_RSTICK_XPOS },   /* Rx right stick   */
	{ CORE_SA_RSTICK_YNEG, CORE_SA_RSTICK_YPOS },   /* Ry right stick   */
	{ CORE_SA_NONE,        CORE_SA_LTRIGGER    },   /* Z  left trigger  */
	{ CORE_SA_NONE,        CORE_SA_RTRIGGER    }    /* Rz right trigger */
};

/* ======================================================================
 * EVALUATION
 *
 * Once per packet: fold the default map for every source no binding has
 * claimed, then walk the bindings. ../docs/mapping-engine.txt section 9
 * gives the order and says why it is that order.
 * ====================================================================== */

/*
 * Nothing may index cs->semiaxis with a value that came off the wire
 * without this check - the enumeration stops at CORE_SEMIAXIS_COUNT and
 * chord indices start well past it.
 *
 * A HELD SOURCE READS AS RELEASED rather than being skipped, so every
 * binding on it deactivates by the ordinary path and lets go of whatever
 * it was asserting. Skipping would freeze it mid-press instead.
 */
/*
 * A SEMIAXIS AS A BINDING SEES IT: the stick deadzone applied, and
 * nothing else applied at all.
 *
 * THE DEADZONE LIVES ON THE STICK BECAUSE THE STICK IS THE ONLY THING
 * THAT NEEDS ONE. A stick rests wherever its centring springs leave it -
 * several thousand units off centre on this pad - so a stick direction
 * bound to a key would type that key forever with nobody touching it.
 * A button rests at exactly zero and needs no such help.
 *
 * So there is one deadzone, it is per stick, it is set where every other
 * property of that stick is set, and it applies to whatever reads the
 * stick. Bindings have none of their own.
 *
 * THE POINTER PIPELINE DOES NOT COME THROUGH HERE. It applies the same
 * deadzone RADIALLY, against the distance from centre, which is the only
 * correct shape for a pointer; per-axis here would square off the corner
 * and is only ever asking whether a direction is pressed. The gamepad
 * axes do not come through here either - they report the stick where it
 * actually is, because an application doing its own calibration needs
 * the truth rather than our idea of centre.
 */
static s32 core_semiaxis_value(const core_state *cs, u8 source)
{
	s32 v;

	if (source >= CORE_SEMIAXIS_COUNT) {
		return 0;
	}
	v = cs->semiaxis[source];

	if (source >= CORE_SA_LSTICK_XNEG && source <= CORE_SA_RSTICK_YPOS) {
		u32 stick = (u32)(source - CORE_SA_LSTICK_XNEG) / 4u;

		if (v <= (s32)cs->cfg.stick[stick].deadzone) {
			return 0;
		}
	}
	return v;
}

static s32 core_source_value(const core_state *cs, u8 source)
{
	if (source < CORE_SEMIAXIS_COUNT) {
		if (cs->hold_mask & (1u << source)) {
			return 0;
		}
		return core_semiaxis_value(cs, source);
	}
	if (source >= CORE_SA_CHORD_BASE &&
	    source < CORE_SA_CHORD_BASE + CORE_MAX_CHORDS) {
		return cs->chord_value[source - CORE_SA_CHORD_BASE];
	}
	return 0;
}

static int core_suppressed(u32 mask, u8 source)
{
	if (source >= CORE_SEMIAXIS_COUNT) {
		return 0;
	}
	return (mask & (1u << source)) != 0;
}

/*
 * ======================================================================
 * CHORDS
 *
 * ../docs/mapping-engine.txt section 2.2. A chord is its own source, so
 * naming Start and Back as members does not bind either of them; they
 * keep their own controls until the chord they belong to takes them.
 * ====================================================================== */

/* A member is down when it reads non-zero, like everything else. */
static int core_member_down(const core_state *cs, u8 source)
{
	if (source >= CORE_SEMIAXIS_COUNT) {
		return 0;
	}
	return core_semiaxis_value(cs, source) > 0;
}

/*
 * Chord values and the hold mask, both before any binding is looked at.
 *
 * A MEMBER IS SUPPRESSED ONLY WHILE ITS CHORD IS COMPLETE. Pressing
 * Start on the way to Start+Back therefore reaches the gamepad for the
 * few packets before Back lands, which is what XBCD does and is a
 * price worth paying: the alternative, holding a member back in case a
 * chord is coming, DELAYS a press rather than deferring it, so any tap
 * shorter than the window is swallowed whole - and Start is a button
 * people tap.
 */
static void core_chords_evaluate(core_state *cs, u32 layer)
{
	const core_layout *lay = &cs->cfg.layout[layer];
	u32 hold = 0;
	u32 i;
	int m;

	for (i = 0; i < CORE_MAX_CHORDS; i++) {
		const core_chord *ch = &lay->chord[i];
		u32 mask = 0;
		int named = 0;
		int all_down = 1;

		for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
			u8 src = ch->member[m];

			if (src == CORE_SA_NONE) {
				continue;
			}
			named++;
			if (src < CORE_SEMIAXIS_COUNT) {
				mask |= 1u << src;
			}
			if (!core_member_down(cs, src)) {
				all_down = 0;
			}
		}

		/* A chord naming nothing is not a chord that is always on. */
		if (named == 0) {
			cs->chord_value[i] = 0;
			continue;
		}

		if (all_down) {
			cs->chord_value[i] = CORE_MAX_VALUE;
			hold |= mask;
		} else {
			cs->chord_value[i] = 0;
		}
	}

	cs->hold_mask = hold;
}

/*
 * ======================================================================
 * LAYERS
 * ====================================================================== */

static int core_layer_change(core_state *cs, u8 to)
{
	if (to >= cs->cfg.layout_count || to == cs->layout) {
		return 0;
	}

	/*
	 * RELEASE EVERYTHING THE OLD LAYER ASSERTED, before the new one is
	 * live. A layer change that leaves W held because the incoming
	 * layer has nothing bound to that source is the classic failure of
	 * every layered remapper, and this is the whole of the fix.
	 */
	core_release_all(cs);
	cs->layout = to;
	return 1;
}

/*
 * Seed the incoming layer's bindings from what is ALREADY held, marking
 * them active without firing anything.
 *
 * WITHOUT THIS A LAYER CHANGE RE-TRIGGERS ITSELF. The control that asked
 * for the change is still down when the new layer arrives, and the new
 * layer has its own copy of the binding with its own state. That copy
 * would see a source going from released to held - a rising edge - and
 * cycle straight back, one layer per packet, for as long as the chord
 * was held.
 */
static void core_layer_prime(core_state *cs, u32 layer)
{
	u32 i;

	for (i = 0; i < cs->cfg.binding_count; i++) {
		core_binding    *b  = &cs->cfg.layout[layer].binding[i];
		core_bind_state *st = &cs->bind[layer][i];
		s32              value;

		if (b->action == CORE_ACT_NONE) {
			continue;
		}
		value = core_source_value(cs, b->source);
		st->active  = (u8)(value > 0);
		st->latched = 0;
	}
}

/*
 * The pad's own controls, for every source the layout has not claimed.
 * mapping-engine.txt section 4.1: a binding REPLACES the default rather
 * than adding to it, unless it carries PASSTHROUGH.
 */
static void core_fold_default(core_state *cs, u32 suppress)
{
	static const u8 DPAD[4] = {
		CORE_SA_DPAD_UP, CORE_SA_DPAD_DOWN,
		CORE_SA_DPAD_LEFT, CORE_SA_DPAD_RIGHT
	};
	int i;

	for (i = 0; i < 12; i++) {
		u8 src = CORE_DEFAULT_BUTTONS[i];

		if (core_suppressed(suppress, src)) {
			continue;
		}
		/* PRESSED MEANS NON-ZERO, and it means that here for the same
		 * reason it does for a binding: the analog buttons rest at
		 * exactly zero, so a threshold only adds required force. The
		 * gamepad button and the key a binding sends now come on at
		 * the same point, which they did not when this had a
		 * threshold of its own. */
		if (cs->semiaxis[src] > 0) {
			cs->gp.buttons |= (u16)(1u << i);
		}
	}

	for (i = 0; i < CORE_GP_AXIS_COUNT; i++) {
		u8  pos_src = CORE_DEFAULT_AXES[i].pos;
		u8  neg_src = CORE_DEFAULT_AXES[i].neg;
		s32 pos = 0;
		s32 neg = 0;

		if (pos_src != CORE_SA_NONE &&
		    !core_suppressed(suppress, pos_src)) {
			pos = cs->semiaxis[pos_src];
		}
		if (neg_src != CORE_SA_NONE &&
		    !core_suppressed(suppress, neg_src)) {
			neg = cs->semiaxis[neg_src];
		}
		cs->gp.axis[i] += pos - neg;
	}

	for (i = 0; i < 4; i++) {
		if (core_suppressed(suppress, DPAD[i])) {
			continue;
		}
		if (cs->semiaxis[DPAD[i]] > 0) {
			cs->gp.hat_index |= (u8)(1u << i);
		}
	}
}

/*
 * on says whether the binding is asserting; edge says whether it became so
 * in this packet. Held actions use the first, pulse actions the second.
 */
static void core_apply_action(core_state *cs, const core_binding *b,
                              s32 value, int on, int edge)
{
	u32 axis;
	s32 magnitude;

	switch (b->action) {
	case CORE_ACT_KEY:
		core_key_event(cs, (u8)b->code, on);
		break;

	case CORE_ACT_MOUSE_BUTTON:
		core_mouse_button(cs, (u8)b->code, on);
		break;

	case CORE_ACT_JOY_BUTTON:
		if (on && b->code >= 1 && b->code <= CORE_GP_BUTTON_COUNT) {
			cs->gp.buttons |= (u16)(1u << (b->code - 1));
		}
		break;

	case CORE_ACT_JOY_AXIS:
		axis = (u32)(b->code & 0x7FFF);
		if (axis >= CORE_GP_AXIS_COUNT) {
			break;
		}
		/*
		 * ANALOG PASSES THE PRESSURE THROUGH instead of thresholding
		 * it, which is what turns a trigger into an axis rather than
		 * into a button that happens to be analog.
		 */
		if (b->flags & CORE_BF_ANALOG) {
			magnitude = value;
		} else {
			magnitude = on ? CORE_MAX_VALUE : 0;
		}
		if (b->code & 0x8000) {
			magnitude = -magnitude;
		}
		cs->gp.axis[axis] += magnitude;
		break;

	case CORE_ACT_JOY_POV:
		if (on && b->code <= 3) {
			cs->gp.hat_index |= (u8)(1u << b->code);
		}
		break;

	case CORE_ACT_MOUSE_WHEEL:
		/* A DETENT IS AN EVENT, NOT A STATE. Emitting one per packet
		 * while the source is held would scroll at the packet rate. */
		if (edge) {
			core_mouse_wheel(cs, (s32)(s16)b->code, 0);
		}
		break;

	case CORE_ACT_MOUSE_PULSE:
		if (edge) {
			core_mouse_move(cs, (s32)(s8)(b->code & 0xFF),
			                (s32)(s8)(b->code >> 8));
		}
		break;

	/*
	 * A LAYER CHANGE IS DEFERRED BY ONE PACKET. Applying it here would
	 * leave the rest of this walk running against a layout that is no
	 * longer live, and the binding that asked for the change would be
	 * re-evaluated under the layer it just switched to.
	 */
	case CORE_ACT_LAYER_SET:
		if (edge) {
			cs->layer_pending = (u8)b->code;
			cs->layer_pending_valid = 1;
		}
		break;

	case CORE_ACT_LAYER_CYCLE:
		if (edge) {
			s32 step = (s32)(s16)b->code;
			s32 count = (s32)cs->cfg.layout_count;
			s32 next;

			if (count > 0) {
				/* WRAP BOTH WAYS. A step of -1 from layer 0
				 * must reach the last layer, and C's % keeps
				 * the sign of its left operand. */
				next = ((s32)cs->layer_base + step) % count;
				if (next < 0) {
					next += count;
				}
				cs->layer_pending = (u8)next;
				cs->layer_pending_valid = 1;
			}
		}
		break;

	default:
		break;
	}
}

/*
 * ======================================================================
 * REPEAT, ALSO KNOWN AS AUTOFIRE
 *
 * ../docs/mapping-engine.txt section 6. The action is asserted at once,
 * held for repeat_delay_ms, and then alternates every half period until
 * the binding deactivates.
 *
 * A HID REPORT CARRIES STATE, NOT EVENTS, so a press and a release inside
 * one report are invisible: the host compares before with after and they
 * are identical. Autofire therefore cannot be a burst of presses inside a
 * single report - it has to be an alternating output that the ordinary
 * emit-on-change path turns into reports.
 *
 * DEADLINES ARE DRIVEN BY WHICHEVER CLOCK ARRIVES FIRST, the packet or
 * the tick. Packets come every 4ms and the tick only every 16, so at any
 * usable rate the packets do the work and the tick is what keeps a cycle
 * moving if they are late.
 *
 * Returns whether the action fires THIS pass - the initial assertion or
 * a repeat re-assertion - which is what the actions with no held state,
 * the wheel and the pointer nudge, are driven by.
 */
static int core_repeat(const core_binding *b, core_bind_state *st,
                       s32 value, int edge, int *asserted, u64 now_100ns)
{
	u64 half;
	int fired = edge;

	if (!(b->flags & CORE_BF_REPEAT) || b->repeat_hz == 0) {
		return fired;
	}

	/*
	 * PRESS HARDER TO AUTOFIRE. With hard_at set, the binding is an
	 * ordinary hold until the control is pushed past it, and the repeat
	 * runs from there; ease off and it goes back to a hold. This is the
	 * only thing an analog button's pressure is read for, and it is why
	 * the pad has analog buttons at all.
	 *
	 * The cycle resets on the way out so easing off and pressing hard
	 * again starts a fresh one rather than resuming mid-flight.
	 */
	if (b->hard_at != 0 && value < (s32)b->hard_at) {
		st->repeat_on = 0;
		st->repeat_at = 0;
		return fired;           /* still held, just not repeating */
	}

	if (!*asserted) {
		/* RESET ON RELEASE, whatever half the cycle was in. A cycle
		 * left mid-flight would resume from where it stopped the next
		 * time the control was touched. */
		st->repeat_on = 0;
		st->repeat_at = 0;
		return 0;
	}

	/* Half a period, in 100ns units. One second is 10,000,000 of them. */
	half = 10000000u / ((u64)b->repeat_hz * 2u);
	if (half == 0) {
		half = 1;
	}

	if (edge || st->repeat_at == 0) {
		/*
		 * THE FIRST INTERVAL IS THE DELAY, IF THERE IS ONE. That is
		 * what makes a held control behave like a keyboard: one
		 * press, a pause, then repetition.
		 */
		st->repeat_on = 1;
		st->repeat_at = now_100ns +
		        (b->repeat_delay_ms
		         ? (u64)b->repeat_delay_ms * CORE_100NS_PER_MS
		         : half);
		fired = 1;
	} else if (now_100ns >= st->repeat_at) {
		st->repeat_on = (u8)(!st->repeat_on);
		st->repeat_at = now_100ns + half;
		if (st->repeat_on) {
			fired = 1;
		}
	}

	*asserted = st->repeat_on;
	return fired;
}

static void core_apply_binding(core_state *cs, const core_binding *b,
                               core_bind_state *st, u64 now_100ns)
{
	s32 value;
	int active;
	int edge;
	int asserted;

	if (b->action == CORE_ACT_NONE) {
		return;
	}

	value = core_source_value(cs, b->source);

	/* NON-ZERO IS PRESSED. core.h says why there is no threshold. */
	active = (value > 0);

	edge = (active && !st->active);
	if (edge && (b->flags & CORE_BF_TOGGLE)) {
		st->latched = (u8)(!st->latched);
	}
	st->active = (u8)active;

	asserted = (b->flags & CORE_BF_TOGGLE) ? st->latched : active;

	edge = core_repeat(b, st, value, edge, &asserted, now_100ns);

	core_apply_action(cs, b, value, asserted, edge);
}

/*
 * ======================================================================
 * THE STICK PIPELINE
 *
 * ../docs/analog-to-mouse.txt section 8, step for step. Steps 1 to 3 -
 * the raw reads, the Y negate and the asymmetric scale - already happened
 * in core_decode, so this starts from a pair of semiaxes in MAX_VALUE
 * units with Y already down-positive.
 * ====================================================================== */

/* The four semiaxes of one stick, negative half first. */
static const struct {
	u8 xneg, xpos, yneg, ypos;
} CORE_STICK_AXES[CORE_STICK_COUNT] = {
	{ CORE_SA_LSTICK_XNEG, CORE_SA_LSTICK_XPOS,
	  CORE_SA_LSTICK_YNEG, CORE_SA_LSTICK_YPOS },
	{ CORE_SA_RSTICK_XNEG, CORE_SA_RSTICK_XPOS,
	  CORE_SA_RSTICK_YNEG, CORE_SA_RSTICK_YPOS }
};

/*
 * Deflection to speed: deadzone, rescale, smoothing, curve, acceleration.
 *
 * SHARED BY EVERY MODE THAT PRODUCES A RATE. The pointer and the wheel
 * differ only in what they do with the number and which axes feed it;
 * everything up to here is the same machinery and the same parameters.
 *
 * Returns 0 when the stick is inside the deadzone, having reset the
 * state that only means something while it is outside.
 */
static s32 core_stick_speed(core_state *cs, u32 index, s32 magnitude,
                            u32 dt_us)
{
	const core_stick *cfg = &cs->cfg.stick[index];
	core_stick_state *ss  = &cs->stick_state[index];
	s32 rc, u, g, speed;
	s32 i, f;

	if (magnitude <= (s32)cfg->deadzone) {
		ss->u_prev  = 0;
		ss->boost   = 0;
		ss->accum_x = 0;
		ss->accum_y = 0;
		return 0;
	}

	rc = (magnitude < (s32)cfg->outer) ? magnitude : (s32)cfg->outer;
	u  = (s32)(((s64)(rc - cfg->deadzone) * 65535) /
	           ((s32)cfg->outer - (s32)cfg->deadzone));

	if (cfg->smooth_ms != 0 && u > ss->u_prev) {
		u32 tau_us = (u32)cfg->smooth_ms * 1000u;
		u32 alpha  = (u32)(((u64)dt_us << 16) / (tau_us + dt_us));

		u = ss->u_prev +
		    (s32)((((s64)(u - ss->u_prev)) * alpha) >> 16);
	}
	ss->u_prev = u;

	i = u >> 11;
	f = u & 0x7FF;
	if (i >= CORE_CURVE_POINTS - 1) {
		i = CORE_CURVE_POINTS - 2;
		f = 0x7FF;
	}
	g = (s32)cfg->curve[i] +
	    (s32)((((s32)cfg->curve[i + 1] - (s32)cfg->curve[i]) * f) >> 11);

	speed = (s32)(((s64)cfg->max_speed * g) >> 16);

	if (cfg->accel_rate != 0) {
		if (u >= (s32)cfg->accel_threshold) {
			ss->boost += (s32)(((s64)cfg->accel_rate * dt_us) /
			                   1000000);
		} else {
			ss->boost -= (s32)(((s64)cfg->accel_decay * dt_us) /
			                   1000000);
		}
		ss->boost = core_clamp(ss->boost, 0, (s32)cfg->accel_max);
		speed = (s32)(((s64)speed * (256 + ss->boost)) >> 8);
	}

	return speed;
}

/*
 * A stick that scrolls.
 *
 * VERTICAL ONLY, AND NOT RADIAL. A wheel has one axis, so the deadzone
 * applies to the Y deflection alone rather than to the distance from
 * centre - otherwise pushing sideways would consume the deadzone and a
 * diagonal would scroll at a rate that depended on the horizontal
 * component, which is not what anybody means by a stick that scrolls.
 *
 * max_speed IS DETENTS PER SECOND HERE, NOT PIXELS. A pointer wants
 * thousands; a wheel wants ten or twenty. The same field means a very
 * different number in this mode and a profile that forgets it will
 * scroll a document into next week.
 */
static void core_stick_wheel(core_state *cs, u32 index, u32 dt_us)
{
	const core_stick *cfg = &cs->cfg.stick[index];
	core_stick_state *ss  = &cs->stick_state[index];
	s32 y, mag, speed, step;

	y = cs->semiaxis[CORE_STICK_AXES[index].ypos] -
	    cs->semiaxis[CORE_STICK_AXES[index].yneg];
	mag = (y < 0) ? -y : y;

	speed = core_stick_speed(cs, index, mag, dt_us);
	if (speed == 0) {
		return;
	}

	/*
	 * PUSH UP, SCROLL UP. The decode leaves Y down-positive, and a
	 * positive wheel value scrolls away from the reader, so the sign
	 * is flipped once here. invert_y flips it back for anyone who
	 * wants the other convention.
	 */
	if (y > 0) {
		speed = -speed;
	}
	speed = (s32)(((s64)speed * cfg->gain_y) >> 7);
	if (cfg->invert_y) {
		speed = -speed;
	}

	ss->accum_y += (s64)speed * dt_us;
	step = (s32)(ss->accum_y / 1000000);
	ss->accum_y -= (s64)step * 1000000;

	core_mouse_wheel(cs, step, 0);
}

static void core_stick_run(core_state *cs, u32 index, u32 dt_us)
{
	const core_stick *cfg = &cs->cfg.stick[index];
	core_stick_state *ss  = &cs->stick_state[index];
	s32 x, y;
	s32 r;
	s32 speed, vx, vy;
	s32 step_x, step_y;

	if (cfg->mode == CORE_STICK_WHEEL) {
		core_stick_wheel(cs, index, dt_us);
		return;
	}
	if (cfg->mode != CORE_STICK_MOUSE) {
		return;
	}

	x = cs->semiaxis[CORE_STICK_AXES[index].xpos] -
	    cs->semiaxis[CORE_STICK_AXES[index].xneg];
	y = cs->semiaxis[CORE_STICK_AXES[index].ypos] -
	    cs->semiaxis[CORE_STICK_AXES[index].yneg];

	/*
	 * STEP 4. SIXTY-FOUR BITS IS NOT OPTIONAL. Two axes at full scale
	 * sum to 2.45e9, which does not fit a signed 32-bit integer; in 32
	 * bits it wraps negative and the radius near full diagonal comes out
	 * around 42900 instead of 49497.
	 */
	r = (s32)core_isqrt64((u64)((s64)x * x + (s64)y * y));

	/* STEPS 5 TO 8. Radial deflection to pixels per second, shared with
	 * every other mode that produces a rate. */
	speed = core_stick_speed(cs, index, r, dt_us);
	if (speed == 0) {
		return;
	}

	/*
	 * STEP 9. PROJECT ALONG THE STICK, IN 64 BITS - speed times a full
	 * scale axis reaches 6e10. Dividing by the radius is what makes this
	 * radial: the speed is set by how far the stick is pushed and the
	 * direction by where it points, so a diagonal is no faster than a
	 * cardinal.
	 */
	vx = (s32)(((s64)speed * x) / r);
	vy = (s32)(((s64)speed * y) / r);

	/* STEP 10. */
	vx = (s32)(((s64)vx * cfg->gain_x) >> 7);
	vy = (s32)(((s64)vy * cfg->gain_y) >> 7);
	if (cfg->invert_x) {
		vx = -vx;
	}
	if (cfg->invert_y) {
		vy = -vy;
	}

	/*
	 * STEP 11. Carry the remainder rather than throwing it away.
	 *
	 * TRUNCATE TOWARD ZERO, WHICH IS WHAT C DIVISION DOES. A shift
	 * would floor instead, and floor biases negative motion by one
	 * count every tick - a pointer that drifts left and up.
	 */
	ss->accum_x += (s64)vx * dt_us;
	ss->accum_y += (s64)vy * dt_us;

	step_x = (s32)(ss->accum_x / 1000000);
	step_y = (s32)(ss->accum_y / 1000000);

	ss->accum_x -= (s64)step_x * 1000000;
	ss->accum_y -= (s64)step_y * 1000000;

	/* STEP 12. core_mouse_move emits nothing for a zero move. */
	core_mouse_move(cs, step_x, step_y);
}

static void core_sticks(core_state *cs, u64 now_100ns)
{
	u64 elapsed;
	u32 dt_us;
	u32 i;

	if (cs->last_stick_100ns == 0 || now_100ns < cs->last_stick_100ns) {
		cs->last_stick_100ns = now_100ns;
		return;
	}

	elapsed = now_100ns - cs->last_stick_100ns;
	if (elapsed == 0) {
		return;             /* two callers in the same instant */
	}
	cs->last_stick_100ns = now_100ns;

	/*
	 * CLAMP THE INTERVAL. A resume, a debugger break or DPC starvation
	 * can hand this a gap of seconds, and multiplying that by a pointer
	 * velocity flings the cursor across the desktop. Longer than the
	 * clamp is a discontinuity, not motion.
	 */
	dt_us = (u32)(elapsed / 10);
	if (dt_us > CORE_MAX_TICK_MS * 1000u) {
		dt_us = CORE_MAX_TICK_MS * 1000u;
	}

	for (i = 0; i < CORE_STICK_COUNT; i++) {
		core_stick_run(cs, i, dt_us);
	}
}

/*
 * LAYER_HOLD IS RESOLVED AGAINST THE BASE LAYER, and that ordering is the
 * point rather than an accident: a hold binding evaluated in the layer it
 * switches to could be shadowed by that layer, and the pad would be stuck
 * there with nothing left holding the way out.
 */
static u32 core_resolve_hold(core_state *cs, u32 base, u64 now_100ns)
{
	u32 effective = base;
	u32 i;

	for (i = 0; i < cs->cfg.binding_count; i++) {
		core_binding *b = &cs->cfg.layout[0].binding[i];

		if (b->action != CORE_ACT_LAYER_HOLD) {
			continue;
		}
		core_apply_binding(cs, b, &cs->bind[0][i], now_100ns);

		if (cs->bind[0][i].active && b->code < cs->cfg.layout_count) {
			/* Highest-numbered active hold wins. Arbitrary, but
			 * deterministic, and simpler to explain than a stack. */
			if (b->code >= effective) {
				effective = b->code;
			}
		}
	}
	return effective;
}

static void core_evaluate(core_state *cs, u64 now_100ns)
{
	u32 layer;
	u32 i;
	int changed = 0;

	/* A change asked for by the previous packet lands here, before
	 * anything in this one is looked at. */
	if (cs->layer_pending_valid) {
		cs->layer_pending_valid = 0;
		cs->layer_base = cs->layer_pending;
		changed |= core_layer_change(cs, cs->layer_pending);
	}

	if (cs->layer_base >= cs->cfg.layout_count) {
		cs->layer_base = 0;
	}

	layer = cs->layout;
	if (layer >= cs->cfg.layout_count) {
		layer = 0;
		cs->layout = 0;
	}

	/*
	 * CHORDS BEFORE ANYTHING READS A SOURCE. They decide the hold mask,
	 * and the hold mask is what every later lookup goes through - the
	 * layer-hold pass below included.
	 */
	core_chords_evaluate(cs, layer);

	layer = core_resolve_hold(cs, cs->layer_base, now_100ns);
	if (core_layer_change(cs, (u8)layer)) {
		changed = 1;
	}
	layer = cs->layout;
	if (layer >= cs->cfg.layout_count) {
		layer = 0;
	}

	if (changed) {
		core_layer_prime(cs, layer);
	}

	core_zero(&cs->gp, (u32)sizeof(cs->gp));

	core_fold_default(cs, cs->cfg.suppress[layer] | cs->hold_mask |
	                      cs->cfg.stick_claim);

	for (i = 0; i < cs->cfg.binding_count; i++) {
		core_binding *b = &cs->cfg.layout[layer].binding[i];

		/* Already evaluated, against the base layer. */
		if (b->action == CORE_ACT_LAYER_HOLD && layer == 0) {
			continue;
		}
		core_apply_binding(cs, b, &cs->bind[layer][i], now_100ns);
	}
}

static void core_build_gamepad(core_state *cs, u8 *payload)
{
	int i;

	core_zero(payload, CORE_GAMEPAD_PAYLOAD);

	payload[CORE_GP_BUTTONS]     = (u8)(cs->gp.buttons & 0xFF);
	payload[CORE_GP_BUTTONS + 1] = (u8)(cs->gp.buttons >> 8);

	for (i = 0; i < CORE_GP_AXIS_COUNT; i++) {
		s32 value = core_clamp(cs->gp.axis[i], -CORE_MAX_VALUE,
		                       CORE_MAX_VALUE);

		value = (s32)(((s64)value * CORE_OUT_AXIS_SCALE)
		              / CORE_MAX_VALUE);
		core_put16(&payload[CORE_GP_AXES + i * 2], value);
	}

	payload[CORE_GP_HAT] = CORE_HAT_TABLE[cs->gp.hat_index & 0x0F];

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
 * EMIT ONLY ON CHANGE, AND THE RAW TAIL IS NOT A CHANGE.
 *
 * THE TAIL MOVES ON ALMOST EVERY PACKET. The sticks rest a count or two
 * either side of centre and never settle, so including the tail in the
 * comparison makes 82 per cent of packets produce a report - two hundred
 * a second, measured - for a collection that usually has nothing reading
 * it at all.
 *
 * THAT FLOOD IS NOT FREE. hidclass keeps two reads outstanding on this
 * driver and every report consumes one, so a gamepad report nobody wants
 * delays the mouse report somebody does. The pointer then moves in
 * bursts: smooth for a moment, stalled, smooth again.
 *
 * The tail still ships in every report that goes out. It simply stops
 * being a reason to send one.
 */
static void core_emit_gamepad(core_state *cs)
{
	u8 payload[CORE_GAMEPAD_PAYLOAD];

	core_build_gamepad(cs, payload);

	if (cs->gp_last_valid &&
	    !core_differs(payload, cs->gp_last, CORE_GP_RAW)) {
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
	u8  payload[CORE_MOUSE_PAYLOAD];
	s32 dx, dy, wheel;

	/*
	 * CARRY WHAT WILL NOT FIT, DO NOT DROP IT. One byte holds 127
	 * counts; anything beyond that stays in the accumulator and goes
	 * out on the next report, so a fast flick travels the right
	 * distance and only takes an extra few milliseconds to do it.
	 */
	dx    = core_clamp(cs->ms.dx, -CORE_MS_STEP_MAX, CORE_MS_STEP_MAX);
	dy    = core_clamp(cs->ms.dy, -CORE_MS_STEP_MAX, CORE_MS_STEP_MAX);
	wheel = core_clamp(cs->ms.wheel, -CORE_MS_WHEEL_MAX, CORE_MS_WHEEL_MAX);

	core_zero(payload, CORE_MOUSE_PAYLOAD);
	payload[CORE_MS_BUTTONS] = cs->ms.buttons;
	core_put16(&payload[CORE_MS_X], dx);
	core_put16(&payload[CORE_MS_Y], dy);
	payload[CORE_MS_WHEEL]   = (u8)(s8)wheel;

	cs->ms.dx    -= dx;
	cs->ms.dy    -= dy;
	cs->ms.wheel -= wheel;
	cs->ms.pan    = 0;      /* nothing carries it to the host */

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

	/* THE ENGINE IS NEVER WITHOUT A CONFIGURATION. Evaluation
	 * indexes the layout array on every packet, and a zeroed one
	 * would have no layers at all. */
	core_config_defaults(&cs->cfg);
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

	/*
	 * AND FORGET EVERY BINDING'S OWN STATE. Leaving active set means
	 * the next packet sees no rising edge and never re-asserts;
	 * leaving a TOGGLE latched means the output comes back by itself
	 * one packet later, having just been released on purpose.
	 */
	core_zero(cs->bind, (u32)sizeof(cs->bind));
	core_zero(&cs->gp, (u32)sizeof(cs->gp));

	/* And the chord machinery, so a member that was down when this
	 * was called does not come back mid hold-off. */
	core_zero(cs->chord_value, (u32)sizeof(cs->chord_value));
	cs->hold_mask = 0;

	/* AND THE PENDING SUB-PIXEL MOTION. A remainder carried across a
	 * configuration swap would move the pointer by something the new
	 * configuration never asked for. */
	core_zero(cs->stick_state, (u32)sizeof(cs->stick_state));
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
	core_evaluate(cs, now_100ns);

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

	(void)elapsed_ms;

	if (!cs->raw_valid) {
		return;         /* nothing decoded yet, nothing to re-run */
	}

	/*
	 * THE TICK IS THE CLOCK FOR EVERYTHING THAT IS A FUNCTION OF TIME,
	 * AND THAT INCLUDES THE POINTER.
	 *
	 * THE PAD ONLY SPEAKS WHEN SOMETHING CHANGES. Measured on hardware:
	 * about five reports a second with nothing touched, and seven to
	 * ten with a stick held hard against its stop - not the 250 a
	 * second the endpoint interval suggests. A stick held still is
	 * silent, because nothing about it is changing.
	 *
	 * So advancing the pointer only when a packet arrives makes it
	 * update seven times a second while a stick is held, which is felt
	 * as heavy choppiness. Integrating here instead gives it the tick
	 * rate regardless of what the pad has to say.
	 *
	 * SILENCE IS THEREFORE NORMAL AND MUST NOT BE TREATED AS A FAULT.
	 * Anything that releases held output after a timeout would fire
	 * constantly between ordinary packets and wipe the sub-pixel
	 * accumulator with it. Outputs are released on real events -
	 * removal, stop, power down, a configuration swap - and on nothing
	 * else.
	 */
	core_evaluate(cs, now_100ns);
	core_sticks(cs, now_100ns);
	core_emit_gamepad(cs);
}

/* ======================================================================
 * CONFIGURATION
 *
 * Parsing, validation and the built-in default map.
 * ../docs/mapping-engine.txt section 10 specifies the blob.
 * ====================================================================== */

/*
 * THE STRUCTURES MUST BE THE SIZES THE DOCUMENT STATES, on all three of the
 * compilers this source is built by. A compiler that inserts padding changes
 * the wire format silently, and the first symptom is a configurator that
 * works on one build and corrupts bindings on another. Fail at compile time
 * instead: this declares an array of length -1 if any size is wrong.
 */
typedef char core_cfg_size_check[
    (sizeof(core_binding)       == 12  &&
     sizeof(core_chord)         == 4   &&
     sizeof(core_stick)         == 88  &&
     sizeof(core_layout)        == 420 &&
     sizeof(core_config_header) == 32) ? 1 : -1];

#define CORE_CFG_DEFAULT_TICK_HZ    250
#define CORE_CFG_DEFAULT_AUTOFIRE   12      /* Hz */

static u32 core_cfg_blob_bytes(u32 layout_count)
{
	return (u32)sizeof(core_config_header) +
	       (u32)sizeof(core_stick) * CORE_STICK_COUNT +
	       (u32)sizeof(core_layout) * layout_count;
}

/* --- defaults -------------------------------------------------------- */

static void core_stick_defaults(core_stick *st)
{
	int i;

	core_zero(st, (u32)sizeof(*st));

	/*
	 * MODE OFF MEANS THE STICK IS NOT HIJACKED, not that it is dead. The
	 * default gamepad map still drives X/Y and Rx/Ry from it; mode only
	 * decides whether the pointer pipeline claims it instead.
	 */
	st->mode            = CORE_STICK_OFF;
	st->deadzone        = 2000;
	st->outer           = 33000;
	st->max_speed       = 2800;
	st->gain_x          = 128;
	st->gain_y          = 54;       /* 54/128 = 0.42, the shipped ratio */
	st->invert_x        = 0;

	/*
	 * ZERO, AND THE OBVIOUS ARGUMENT FOR ONE IS ALREADY SPENT. The
	 * pad reports Y up-positive and HID wants it down-positive, but
	 * core_decode does that negation; doing it again here would aim
	 * the wrong way out of the box. The flag stays for players who
	 * want inverted aiming, which is a preference, not a fix.
	 */
	st->invert_y        = 0;
	st->smooth_ms       = 8;
	st->accel_threshold = 58000;
	st->accel_rate      = 0;        /* off */
	st->accel_max       = 512;
	st->accel_decay     = 1024;

	/* Linear, so a stick that is not tuned behaves like one that is not
	 * curved rather than like one that is broken. */
	for (i = 0; i < CORE_CURVE_POINTS; i++) {
		/* ROUND, DO NOT TRUNCATE. tools/mkconfig.py computes the
		 * same table in floating point and rounds; truncating here
		 * puts the two one LSB apart and the cross-check in the
		 * harness stops meaning anything. */
		st->curve[i] = (u16)(((u32)i * 65535u +
		                      (CORE_CURVE_POINTS - 1) / 2) /
		                     (CORE_CURVE_POINTS - 1));
	}
}

static void core_binding_clear(core_binding *b)
{
	core_zero(b, (u32)sizeof(*b));
	b->source = CORE_SA_NONE;
	b->action = CORE_ACT_NONE;
}

void core_config_defaults(core_config *cfg)
{
	static const u8 FACE[4] = {
		CORE_SA_A, CORE_SA_B, CORE_SA_X, CORE_SA_Y
	};
	u32 l, i;

	core_zero(cfg, (u32)sizeof(*cfg));

	cfg->valid         = 1;
	cfg->layout_count  = CORE_MAX_LAYOUTS;
	cfg->binding_count = CORE_MAX_BINDINGS;
	cfg->chord_count   = CORE_MAX_CHORDS;
	cfg->collections   = 0x07;      /* gamepad, keyboard, mouse */
	cfg->tick_hz       = CORE_CFG_DEFAULT_TICK_HZ;

	core_stick_defaults(&cfg->stick[0]);
	core_stick_defaults(&cfg->stick[1]);

	for (l = 0; l < CORE_MAX_LAYOUTS; l++) {
		core_layout *lay = &cfg->layout[l];

		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			core_binding_clear(&lay->binding[i]);
		}
		for (i = 0; i < CORE_MAX_CHORDS; i++) {
			lay->chord[i].member[0] = CORE_SA_NONE;
			lay->chord[i].member[1] = CORE_SA_NONE;
			lay->chord[i].member[2] = CORE_SA_NONE;
			lay->chord[i].flags     = 0;
		}
		lay->led = (u8)l;

		/*
		 * Chord 0 is Start plus Back - XBCD's own gesture, and the
		 * one control combination no game claims.
		 */
		lay->chord[0].member[0] = CORE_SA_START;
		lay->chord[0].member[1] = CORE_SA_BACK;

		/*
		 * THE CYCLE BINDING GOES IN EVERY LAYOUT. It is the only way
		 * back; a layer without it is a layer you cannot leave
		 * without unplugging the pad.
		 */
		lay->binding[0].source = CORE_SA_CHORD_BASE + 0;
		lay->binding[0].action = CORE_ACT_LAYER_CYCLE;
		lay->binding[0].code   = 1;
	}

	/*
	 * Layer 2 is layer 1 with the four face buttons autofiring. Binding
	 * them to the buttons they already drive means the suppression rule
	 * of section 4.1 replaces the steady press with the pulsing one
	 * rather than producing both.
	 */
	for (i = 0; i < 4; i++) {
		core_binding *b = &cfg->layout[1].binding[1 + i];

		b->source    = FACE[i];
		b->action    = CORE_ACT_JOY_BUTTON;
		b->code      = (u16)(1 + i);   /* buttons 1..4, XBCD order */
		b->flags     = CORE_BF_REPEAT;
		b->repeat_hz = CORE_CFG_DEFAULT_AUTOFIRE;
	}

	core_config_suppress(cfg);
}

/* --- the suppression mask, mapping-engine.txt section 4.1 ------------ */

void core_config_suppress(core_config *cfg)
{
	u32 l, i;

	for (l = 0; l < CORE_MAX_LAYOUTS; l++) {
		u32 mask = 0;
		u32 members = 0;
		u32 c;
		int m;

		if (l < cfg->layout_count) {
			for (i = 0; i < cfg->binding_count; i++) {
				const core_binding *b =
				    &cfg->layout[l].binding[i];

				if (b->action == CORE_ACT_NONE) {
					continue;
				}
				if (b->flags & CORE_BF_PASSTHROUGH) {
					continue;
				}
				/*
				 * A CHORD SOURCE SUPPRESSES NOTHING. Its
				 * members are held back only while the
				 * chord is active - section 2.2 - and keep
				 * their own defaults otherwise.
				 */
				if (b->source < CORE_SEMIAXIS_COUNT) {
					mask |= 1u << b->source;
				}
			}

			/* Every source any chord in this layout names. */
			for (c = 0; c < cfg->chord_count &&
			            c < CORE_MAX_CHORDS; c++) {
				const core_chord *ch = &cfg->layout[l].chord[c];

				for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
					u8 src = ch->member[m];

					if (src < CORE_SEMIAXIS_COUNT) {
						members |= 1u << src;
					}
				}
			}
		}
		cfg->suppress[l]      = mask;
		cfg->chord_members[l] = members;
	}

	/*
	 * A stick driving the pointer stops driving the gamepad axes.
	 * Sticks are global, so this mask is too.
	 */
	cfg->stick_claim = 0;
	for (i = 0; i < CORE_STICK_COUNT; i++) {
		if (cfg->stick[i].mode == CORE_STICK_OFF ||
		    cfg->stick[i].mode == CORE_STICK_JOY) {
			continue;
		}
		cfg->stick_claim |= 1u << CORE_STICK_AXES[i].xneg;
		cfg->stick_claim |= 1u << CORE_STICK_AXES[i].xpos;
		cfg->stick_claim |= 1u << CORE_STICK_AXES[i].yneg;
		cfg->stick_claim |= 1u << CORE_STICK_AXES[i].ypos;
	}
}

/* --- validation ------------------------------------------------------ */

static int core_cfg_key_ok(u16 code)
{
	return (code >= CORE_KEY_FIRST && code <= CORE_KEY_LAST) ||
	       (code >= CORE_KEY_MOD_FIRST && code <= CORE_KEY_MOD_LAST);
}

/* Clamp v into 1..hi. Returns non-zero if it had to change anything. */
static int core_cfg_clamp16(u16 *v, u16 lo, u16 hi)
{
	if (*v < lo) {
		*v = lo;
		return 1;
	}
	if (*v > hi) {
		*v = hi;
		return 1;
	}
	return 0;
}

static u32 core_cfg_fix_binding(core_binding *b, u8 layout_count,
                                u8 chord_count, u16 tick_hz)
{
	u32 fixed = 0;
	u16 axis;

	if (b->action >= CORE_ACT_COUNT) {
		b->action = CORE_ACT_NONE;
		fixed++;
	}

	/* A source that names nothing real can only misfire. */
	if (b->source >= CORE_SEMIAXIS_COUNT &&
	    !(b->source >= CORE_SA_CHORD_BASE &&
	      b->source <  CORE_SA_CHORD_BASE + chord_count)) {
		if (b->action != CORE_ACT_NONE) {
			b->action = CORE_ACT_NONE;
			fixed++;
		}
	}

	if (b->flags & ~(u8)CORE_BF_KNOWN) {
		b->flags &= (u8)CORE_BF_KNOWN;
		fixed++;
	}

	/* A hard point past full scale could never be reached, so it would
	 * silently disable the autofire it was asked for. */
	if (b->hard_at > CORE_MAX_VALUE) {
		b->hard_at = CORE_MAX_VALUE;
		fixed++;
	}

	/*
	 * THE CODE IS BOUNDED BY THE DESCRIPTOR, NOT BY ITS FIELD WIDTH. A
	 * JOY_BUTTON of 900 written into the report bitmap is an out-of-range
	 * write at DISPATCH_LEVEL, which is a bugcheck at best.
	 */
	switch (b->action) {
	case CORE_ACT_KEY:
		if (!core_cfg_key_ok(b->code)) {
			b->action = CORE_ACT_NONE;
			fixed++;
		}
		break;
	case CORE_ACT_MOUSE_BUTTON:
		fixed += (u32)core_cfg_clamp16(&b->code, 1, CORE_MOUSE_BUTTONS);
		break;
	case CORE_ACT_JOY_BUTTON:
		fixed += (u32)core_cfg_clamp16(&b->code, 1,
		                               CORE_GP_BUTTON_COUNT);
		break;
	case CORE_ACT_JOY_AXIS:
		axis = (u16)(b->code & 0x7FFF);
		if (axis >= CORE_GP_AXIS_COUNT) {
			b->code = (u16)((b->code & 0x8000) |
			                (CORE_GP_AXIS_COUNT - 1));
			fixed++;
		}
		break;
	case CORE_ACT_JOY_POV:
		if (b->code > 3) {
			b->code = 3;
			fixed++;
		}
		break;
	case CORE_ACT_LAYER_HOLD:
	case CORE_ACT_LAYER_SET:
		if (b->code >= layout_count) {
			b->code = (u16)(layout_count - 1);
			fixed++;
		}
		break;
	default:
		break;
	}

	/*
	 * A REPEAT CYCLE NEEDS A TICK TO ASSERT AND A TICK TO RELEASE, so a
	 * rate above half the tick rate cannot be represented and would
	 * silently become something else.
	 */
	if (b->repeat_hz > tick_hz / 2) {
		b->repeat_hz = (u8)(tick_hz / 2);
		fixed++;
	}

	return fixed;
}

static u32 core_cfg_fix_stick(core_stick *st)
{
	u32 fixed = 0;
	int i;

	if (st->mode >= CORE_STICK_MODE_COUNT ||
	    st->mode == CORE_STICK_ABSOLUTE) {
		/* ABSOLUTE IS NOT IMPLEMENTED AND IS REPAIRED RATHER THAN
		 * IGNORED, so a configurator offering it hears about it
		 * through the repaired count instead of silently doing
		 * nothing. core.h says what it needs. */
		st->mode = CORE_STICK_OFF;
		fixed++;
	}

	/*
	 * THE PIPELINE DIVIDES BY (outer - deadzone). A configurator that
	 * sends them equal, or inverted, would divide by zero or by a
	 * negative in the rescale of analog-to-mouse.txt section 8 step 5.
	 */
	if (st->deadzone >= CORE_MAX_VALUE) {
		st->deadzone = CORE_MAX_VALUE - 1;
		fixed++;
	}
	if (st->outer <= st->deadzone) {
		st->outer = (u16)(st->deadzone + 1);
		fixed++;
	}

	if (st->gain_x == 0 && st->gain_y == 0) {
		st->gain_x = 128;
		st->gain_y = 128;
		fixed++;
	}

	/*
	 * REPAIR A NON-MONOTONE CURVE RATHER THAN TRUSTING IT. A table that
	 * dips backwards makes the pointer reverse mid-deflection, which
	 * reads as a hardware fault and is impossible to diagnose from the
	 * outside.
	 */
	for (i = 1; i < CORE_CURVE_POINTS; i++) {
		if (st->curve[i] < st->curve[i - 1]) {
			st->curve[i] = st->curve[i - 1];
			fixed++;
		}
	}

	return fixed;
}

/* --- load ------------------------------------------------------------ */

int core_config_load(core_config *cfg, const u8 *blob, u32 len,
                     u32 *repaired)
{
	core_config_header hdr;
	u32 fixed = 0;
	u32 need, off, l, i;

	if (repaired) {
		*repaired = 0;
	}
	if (!cfg || !blob || len < sizeof(core_config_header)) {
		return CORE_CFG_ERR_SHORT;
	}

	/* COPY, DO NOT CAST. The blob arrives as bytes from user mode and
	 * nothing promises it is aligned for a u32 read. */
	core_copy(&hdr, blob, (u32)sizeof(hdr));

	if (hdr.signature != CORE_CFG_SIGNATURE) {
		return CORE_CFG_ERR_SIGNATURE;
	}

	/*
	 * A NEWER BLOB IS FINE, AN OLDER ONE IS NOT. Forward compatibility
	 * runs on the strides: a configurator built against a later version
	 * grows a structure, this walks it by the stride in the header and
	 * ignores the tail. A stride SMALLER than the structure means fields
	 * this build needs are simply absent, and there is nothing to read.
	 */
	if (hdr.version < CORE_CFG_VERSION) {
		return CORE_CFG_ERR_VERSION;
	}
	if (hdr.header_bytes < sizeof(core_config_header) ||
	    hdr.layout_bytes < sizeof(core_layout) ||
	    hdr.stick_bytes  < sizeof(core_stick)) {
		return CORE_CFG_ERR_STRIDE;
	}

	/*
	 * COUNTS ARE REJECTED, NOT CLAMPED. Clamping eight layouts to two
	 * would load silently and throw away six the user can still see in
	 * their configurator.
	 */
	if (hdr.layout_count < 1 || hdr.layout_count > CORE_MAX_LAYOUTS ||
	    hdr.binding_count > CORE_MAX_BINDINGS ||
	    hdr.chord_count > CORE_MAX_CHORDS) {
		return CORE_CFG_ERR_COUNT;
	}

	/* Does the blob actually contain what the header describes? Built up
	 * in steps so the arithmetic cannot overflow past the check. */
	need = hdr.header_bytes;
	if (need > len) {
		return CORE_CFG_ERR_TRUNCATED;
	}
	for (i = 0; i < CORE_STICK_COUNT; i++) {
		if (hdr.stick_bytes > len - need) {
			return CORE_CFG_ERR_TRUNCATED;
		}
		need += hdr.stick_bytes;
	}
	for (i = 0; i < hdr.layout_count; i++) {
		if (hdr.layout_bytes > len - need) {
			return CORE_CFG_ERR_TRUNCATED;
		}
		need += hdr.layout_bytes;
	}

	/*
	 * EVERY STRUCTURAL CHECK IS NOW PAST, so from here nothing can fail
	 * and cfg may be written directly. That is what keeps the promise
	 * that a rejected blob leaves a running configuration untouched,
	 * without staging a kilobyte of copy on a DISPATCH_LEVEL stack.
	 */
	core_zero(cfg, (u32)sizeof(*cfg));

	cfg->layout_count  = hdr.layout_count;
	cfg->binding_count = hdr.binding_count;
	cfg->chord_count   = hdr.chord_count;
	cfg->collections   = (u8)(hdr.collections & 0x07);
	cfg->tick_hz       = hdr.tick_hz;

	if (cfg->tick_hz < 8 || cfg->tick_hz > 1000) {
		cfg->tick_hz = CORE_CFG_DEFAULT_TICK_HZ;
		fixed++;
	}

	off = hdr.header_bytes;
	for (i = 0; i < CORE_STICK_COUNT; i++) {
		core_copy(&cfg->stick[i], blob + off, (u32)sizeof(core_stick));
		off += hdr.stick_bytes;
		fixed += core_cfg_fix_stick(&cfg->stick[i]);
	}

	for (l = 0; l < hdr.layout_count; l++) {
		core_layout *lay = &cfg->layout[l];

		core_copy(lay, blob + off, (u32)sizeof(core_layout));
		off += hdr.layout_bytes;

		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			if (i >= cfg->binding_count) {
				core_binding_clear(&lay->binding[i]);
				continue;
			}
			fixed += core_cfg_fix_binding(&lay->binding[i],
			                              cfg->layout_count,
			                              cfg->chord_count,
			                              cfg->tick_hz);
		}
		for (i = 0; i < CORE_MAX_CHORDS; i++) {
			core_chord *ch = &lay->chord[i];
			int m;

			if (i >= cfg->chord_count) {
				ch->member[0] = CORE_SA_NONE;
				ch->member[1] = CORE_SA_NONE;
				ch->member[2] = CORE_SA_NONE;
				ch->flags     = 0;
				continue;
			}
			for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
				if (ch->member[m] >= CORE_SEMIAXIS_COUNT &&
				    ch->member[m] != CORE_SA_NONE) {
					ch->member[m] = CORE_SA_NONE;
					fixed++;
				}
			}
		}
	}

	cfg->valid = 1;
	core_config_suppress(cfg);

	if (repaired) {
		*repaired = fixed;
	}
	return CORE_CFG_OK;
}

/* --- save, for the harness round trip -------------------------------- */

u32 core_config_save(const core_config *cfg, u8 *blob, u32 len)
{
	core_config_header hdr;
	u32 need, off, i, l;

	if (!cfg || !blob) {
		return 0;
	}

	need = core_cfg_blob_bytes(cfg->layout_count);
	if (len < need) {
		return 0;
	}

	core_zero(&hdr, (u32)sizeof(hdr));
	hdr.signature    = CORE_CFG_SIGNATURE;
	hdr.version      = CORE_CFG_VERSION;
	hdr.header_bytes = (u16)sizeof(core_config_header);
	hdr.layout_bytes = (u16)sizeof(core_layout);
	hdr.stick_bytes  = (u16)sizeof(core_stick);
	hdr.layout_count = cfg->layout_count;
	hdr.binding_count = cfg->binding_count;
	hdr.chord_count  = cfg->chord_count;
	hdr.collections  = cfg->collections;
	hdr.tick_hz      = cfg->tick_hz;

	core_copy(blob, &hdr, (u32)sizeof(hdr));
	off = (u32)sizeof(hdr);

	for (i = 0; i < CORE_STICK_COUNT; i++) {
		core_copy(blob + off, &cfg->stick[i],
		          (u32)sizeof(core_stick));
		off += (u32)sizeof(core_stick);
	}
	for (l = 0; l < cfg->layout_count; l++) {
		core_copy(blob + off, &cfg->layout[l],
		          (u32)sizeof(core_layout));
		off += (u32)sizeof(core_layout);
	}

	return off;
}

/*
 * Installing a configuration is not the same as parsing one. The parse may
 * fail and must leave the running map alone; the install always succeeds
 * and must first let go of everything the outgoing map was holding.
 */
static void core_config_installed(core_state *cs)
{
	/*
	 * RELEASE BEFORE, NOT AFTER. The bindings that are holding a key
	 * down may not exist in the new table, and once it is installed
	 * there is nothing left that knows the key was ever pressed.
	 */
	core_release_all(cs);

	/*
	 * A LAYER THAT NO LONGER EXISTS WOULD INDEX PAST THE ARRAY. The
	 * incoming configuration may carry fewer layouts than the one being
	 * replaced, and the live layer is not part of the blob.
	 */
	if (cs->layout >= cs->cfg.layout_count) {
		cs->layout = 0;
	}

	/* The next packet must look like the first one, or an unchanged
	 * payload under a new map would be suppressed as "not news". */
	cs->gp_last_valid = 0;
}

int core_set_config(core_state *cs, const u8 *blob, u32 len, u32 *repaired)
{
	int rc;

	if (!cs) {
		return CORE_CFG_ERR_SHORT;
	}

	rc = core_config_load(&cs->cfg, blob, len, repaired);
	if (rc != CORE_CFG_OK) {
		return rc;
	}

	core_config_installed(cs);
	return CORE_CFG_OK;
}

void core_set_config_default(core_state *cs)
{
	if (!cs) {
		return;
	}
	core_config_defaults(&cs->cfg);
	core_config_installed(cs);
}
