/*
 * core.h - OS-free logic for the XBOXCTL driver.
 *
 * NOTHING in this file or in core.c may include a Windows or DDK header, call
 * a kernel API, or name a Windows type. Everything the logic needs from the
 * outside world arrives through the seams declared below: a report sink and a
 * clock passed in as an argument.
 *
 * That rule is what lets the same source build into xboxctl.sys and into the
 * xboxctl.exe harness. It is load-bearing, not stylistic - the autofire
 * timing, the mouse velocity accumulator and the acceleration ramp are all
 * functions of elapsed time, and testing them means driving the clock by hand.
 *
 * Behavioural reference: ../docs/driver-plan.txt (descriptor and report
 * layouts), ../docs/mapping-engine.txt (bindings), ../docs/analog-to-mouse.txt
 * (the stick pipeline), ../docs/xbcd-architecture.txt section 6 (the raw
 * packet, which is where the layout constants below come from).
 */
#ifndef XBOXCTL_CORE_H
#define XBOXCTL_CORE_H

/*
 * Fixed-width types, declared here rather than pulled from <stdint.h>, so the
 * header stays valid in kernel mode where the CRT headers are not available.
 */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
#if defined(_MSC_VER)
typedef unsigned __int64    u64;
typedef signed   __int64    s64;
#else
typedef unsigned long long  u64;
typedef signed   long long  s64;
#endif

/* ======================================================================
 * THE RAW PACKET
 *
 * Twenty bytes off the interrupt IN endpoint. This is a vendor format, not
 * HID; the pad ships no report descriptor of its own, which is why a driver
 * has to exist at all.
 *
 *     +0      0x00, report type
 *     +1      0x14, the length of this report
 *     +2      digital buttons, a bitmask
 *     +3      0x00, unused
 *     +4..11  A B X Y Black White LTrigger RTrigger, ANALOG 0..255
 *     +12..13 left stick X,  signed 16-bit little endian
 *     +14..15 left stick Y
 *     +16..17 right stick X
 *     +18..19 right stick Y
 *
 * Y IS UP-POSITIVE, the opposite of the HID convention. The decode negates
 * it once, here, rather than leaving the sign to be fixed further down.
 * ====================================================================== */

#define CORE_RAW_PACKET_BYTES   20
#define CORE_RAW_TYPE           0
#define CORE_RAW_LENGTH         1
#define CORE_RAW_DIGITAL        2
#define CORE_RAW_ANALOG_BASE    4
#define CORE_RAW_LSTICK_X       12
#define CORE_RAW_LSTICK_Y       14
#define CORE_RAW_RSTICK_X       16
#define CORE_RAW_RSTICK_Y       18

#define CORE_RAW_TYPE_INPUT     0x00

/* Bits of the digital button byte at +2. */
#define CORE_DIG_DPAD_UP        0x01
#define CORE_DIG_DPAD_DOWN      0x02
#define CORE_DIG_DPAD_LEFT      0x04
#define CORE_DIG_DPAD_RIGHT     0x08
#define CORE_DIG_START          0x10
#define CORE_DIG_BACK           0x20
#define CORE_DIG_LTHUMB         0x40
#define CORE_DIG_RTHUMB         0x80

/* ======================================================================
 * SEMIAXES - THE INPUT SIDE
 *
 * The decode flattens the packet into 25 UNIPOLAR magnitudes, each 0..
 * CORE_MAX_VALUE. A bidirectional stick axis becomes two of them, one per
 * direction, each carrying the magnitude in that direction and zero
 * otherwise. Everything above this line is pad-specific; everything below
 * it addresses semiaxes and knows nothing about Xbox hardware.
 *
 * CORE_MAX_VALUE IS DELIBERATELY WIDER THAN THE 32767 OUTPUT RANGE. A stick
 * at full deflection on one axis still reads well under full scale on the
 * other, so a value pair on the physical circle never reaches the corner of
 * the logical square. Decoding into a range about 7 per cent wider and
 * clamping at the end lets a real full deflection reach real full scale.
 * ====================================================================== */

#define CORE_MAX_VALUE          35000
#define CORE_OUT_AXIS_SCALE     32767

#define CORE_SEMIAXIS_COUNT     25

#define CORE_SA_DPAD_UP         0
#define CORE_SA_DPAD_DOWN       1
#define CORE_SA_DPAD_LEFT       2
#define CORE_SA_DPAD_RIGHT      3
#define CORE_SA_START           4
#define CORE_SA_BACK            5
#define CORE_SA_LTHUMB          6
#define CORE_SA_RTHUMB          7
#define CORE_SA_A               8
#define CORE_SA_B               9
#define CORE_SA_X               10
#define CORE_SA_Y               11
#define CORE_SA_BLACK           12
#define CORE_SA_WHITE           13
#define CORE_SA_LTRIGGER        14
#define CORE_SA_RTRIGGER        15
#define CORE_SA_LSTICK_XNEG     16
#define CORE_SA_LSTICK_XPOS     17
#define CORE_SA_LSTICK_YNEG     18
#define CORE_SA_LSTICK_YPOS     19
#define CORE_SA_RSTICK_XNEG     20
#define CORE_SA_RSTICK_XPOS     21
#define CORE_SA_RSTICK_YNEG     22
#define CORE_SA_RSTICK_YPOS     23
#define CORE_SA_GUIDE           24

/* Not a semiaxis. Marks an axis half that nothing drives. */
#define CORE_SA_NONE            0xFF

/* ======================================================================
 * THE COMPOSITE HID DESCRIPTOR
 *
 * Three top-level application collections from one USB endpoint: a gamepad,
 * a keyboard and a mouse. hidclass creates one child devnode per collection,
 * so Windows sees three real input devices. THAT is what makes a remapped
 * button arrive through kbdclass as a genuine keystroke rather than as an
 * injected event an application is free to ignore.
 *
 * ALL THREE ARE ALWAYS PUBLISHED, whether or not anything is bound to them.
 * A descriptor is consumed when hidclass starts the device, so making a
 * collection conditional would mean a replug every time the first key
 * binding is added.
 *
 * The descriptor is the contract. Changing an item changes the device's
 * identity to every application and invalidates saved bindings.
 * ====================================================================== */

#define CORE_REPORT_ID_GAMEPAD  1
#define CORE_REPORT_ID_KEYBOARD 2
#define CORE_REPORT_ID_MOUSE    3
#define CORE_REPORT_ID_RUMBLE   4   /* output  */
#define CORE_REPORT_ID_CONFIG   5   /* feature */
#define CORE_REPORT_ID_VERSION  6   /* feature */

/*
 * Payload sizes, EXCLUDING the leading report ID byte. The sink is handed a
 * payload and an ID; whoever writes the wire format prepends the ID.
 */
#define CORE_GAMEPAD_PAYLOAD    36
#define CORE_KEYBOARD_PAYLOAD   8
#define CORE_MOUSE_PAYLOAD      7

/* The largest report the sink will ever be handed, ID byte included. */
#define CORE_REPORT_MAX_BYTES   (CORE_GAMEPAD_PAYLOAD + 1)

/*
 * Gamepad payload layout, byte offsets past the report ID.
 *
 *     0..1    buttons 1..16
 *     2..13   X Y Rx Ry Z Rz, signed 16-bit little endian
 *     14      hat in the low nibble, four pad bits
 *     15      active layout, 1-based
 *     16..35  the raw 20-byte packet, verbatim
 *
 * SIXTEEN BUTTONS IS TWO BYTES WITH NO PADDING, and it is the count the
 * pad justifies plus a little. Twelve controls map one to one - the six
 * analog face buttons, both triggers, Start, Back and the two thumb
 * clicks - and the D-pad is the hat rather than four more. The spare four
 * exist so a layer or a chord has somewhere to put a gamepad button that
 * no single control produces.
 *
 * DECLARING MORE IS NOT FREE. Every button an application can see but
 * never press is a dead row in its binding UI, and the count cannot be
 * changed later without changing the device's identity and making every
 * game re-bind.
 *
 * THE RAW PACKET RIDES ALONG ON PURPOSE. A configurator asking "press the
 * control you want to bind" gets the mapped output and the physical truth
 * from one HID read, with no second channel to open.
 */
#define CORE_GP_BUTTONS         0
#define CORE_GP_AXES            2
#define CORE_GP_HAT             14
#define CORE_GP_LAYOUT          15
#define CORE_GP_RAW             16

/*
 * SIX AXES, IN TWO KINDS. The sticks are bipolar and rest at centre; the
 * triggers are unipolar and rest at zero, so they are declared as a
 * separate item with their own logical range rather than sharing the
 * sticks'. A trigger on a bipolar axis rests in the middle of its bar and
 * wastes half of it.
 *
 * DirectInput recognises eight axes in total - X, Y, Z, Rx, Ry, Rz and two
 * sliders - so six leaves two spare. Nothing drives a slider, so none is
 * declared: an axis an application can see and nothing can move is a dead
 * bar in every properties page.
 */
#define CORE_GP_AXIS_COUNT      6
#define CORE_GP_STICK_AXES      4       /* bipolar, declared first */
#define CORE_GP_BUTTON_COUNT    16
#define CORE_HAT_CENTRED        8

/*
 * Keyboard payload: modifier bits, a reserved byte, then SIX key slots.
 *
 * Six, not the ten the Adaptoid uses. Six is the boot-protocol size, it is
 * what every host handles without question, and with 25 sources more than
 * six simultaneous keys is not a case worth designing for.
 */
#define CORE_KEY_SLOTS          6
#define CORE_KB_MODIFIERS       0
#define CORE_KB_RESERVED        1
#define CORE_KB_KEYS            2

/* Accepted usage ranges. Everything else is ignored outright. */
#define CORE_KEY_MOD_FIRST      0xE0
#define CORE_KEY_MOD_LAST       0xE7
#define CORE_KEY_FIRST          0x04
#define CORE_KEY_LAST           0xA4
#define CORE_KEY_ROLLOVER       0x01    /* HID ErrorRollOver */

/*
 * THE HELD-KEY ARRAY IS SIZED TO THE KEYCODE RANGE, NOT TO THE REPORT. A
 * usage already present is never appended twice, so the count cannot exceed
 * the number of distinct accepted codes and the array cannot overflow.
 * Sizing it to the six report slots instead would be wrong.
 */
#define CORE_KEY_TRACK_MAX      (CORE_KEY_LAST - CORE_KEY_FIRST + 1)

/*
 * Mouse payload: buttons, 16-bit relative X and Y, wheel, horizontal pan.
 *
 * 16-BIT AXES, NOT THE USUAL 8. At high pointer speeds and a low emission
 * rate a single tick can owe more than 127 pixels; an 8-bit axis saturates
 * and the motion is silently lost. Two extra bytes removes the failure mode.
 */
#define CORE_MOUSE_BUTTONS      5
#define CORE_MS_BUTTONS         0
#define CORE_MS_X               1
#define CORE_MS_Y               3
#define CORE_MS_WHEEL           5
#define CORE_MS_PAN             6

/* ======================================================================
 * THE SEAMS
 * ====================================================================== */

/*
 * Every report the engine produces leaves through here. In the driver this
 * completes a pending HID read or queues the report; in the harness it
 * prints. The payload excludes the report ID, which is passed separately.
 */
typedef void (*core_report_fn)(void *ctx, u8 report_id,
                               const u8 *payload, u32 len);

/*
 * Time is an argument, never a call. Units are 100ns, matching the kernel's
 * interrupt time, but the engine only ever takes differences so the epoch
 * does not matter.
 */
#define CORE_100NS_PER_MS       10000u

/* An elapsed interval longer than this is a discontinuity - a resume, a
 * debugger break, DPC starvation - not motion. Clamp rather than multiply a
 * velocity by it and fling the pointer across the desktop. */
#define CORE_MAX_TICK_MS        50

/* ======================================================================
 * ENGINE STATE
 * ====================================================================== */

typedef struct _core_keyboard_state {
	u8  modifiers;
	u8  count;
	u8  keys[CORE_KEY_TRACK_MAX];
} core_keyboard_state;

typedef struct _core_mouse_state {
	u8  buttons;
	s32 dx;             /* accumulated since the last report */
	s32 dy;
	s32 wheel;
	s32 pan;
} core_mouse_state;

typedef struct _core_state {
	/* --- seams --- */
	core_report_fn  sink;
	void           *sink_ctx;

	/* --- clock --- */
	u64             last_packet_100ns;
	u64             last_tick_100ns;
	int             clock_valid;

	/* --- decoded input --- */
	s32             semiaxis[CORE_SEMIAXIS_COUNT];
	u8              raw[CORE_RAW_PACKET_BYTES];
	int             raw_valid;

	/* --- output state --- */
	u8                   layout;
	core_keyboard_state  kb;
	core_mouse_state     ms;

	/* Last gamepad payload submitted, for the emit-on-change test. */
	u8              gp_last[CORE_GAMEPAD_PAYLOAD];
	int             gp_last_valid;

	/* --- counters, read by the harness --- */
	u32             packets_accepted;
	u32             packets_rejected;
	u32             reports_emitted;
} core_state;

/* ======================================================================
 * API
 * ====================================================================== */

/* Zero the state and install the seam. Safe to call again to reset. */
void core_init(core_state *cs, core_report_fn sink, void *sink_ctx);

/* The composite descriptor. Returns the array and writes its length. */
const u8 *core_hid_descriptor(u32 *length);

/*
 * One controller packet. len is what the transfer actually delivered; a
 * packet that is not exactly CORE_RAW_PACKET_BYTES, or whose type byte is
 * not an input report, is counted and discarded.
 */
void core_on_packet(core_state *cs, const u8 *raw, u32 len, u64 now_100ns);

/*
 * The periodic tick. Drives everything that is a function of time rather
 * than of packet arrival: autofire deadlines and the mouse velocity
 * accumulator. Harmless to call with no packet yet received.
 */
void core_tick(core_state *cs, u64 now_100ns);

/*
 * Release every asserted output and emit the reports that implies. Called
 * on a layer change, on a configuration swap and on device stop - anywhere a
 * binding that is holding something could vanish underneath it.
 */
void core_release_all(core_state *cs);

/* --- radial helpers, exposed for the harness ------------------------- */

/*
 * Integer square root of a 64-bit value.
 *
 * SIXTY-FOUR BITS IS NOT OPTIONAL. Two axes at CORE_MAX_VALUE give a sum of
 * squares of 2.45e9, which does not fit a signed 32-bit integer; computing
 * it in 32 bits wraps negative and gives a radius near 42900 instead of
 * 49497, so every radial test near full diagonal deflection is wrong.
 */
u32 core_isqrt64(u64 value);

/* Clamp a stick pair to a circle of radius CORE_MAX_VALUE, preserving the
 * direction. */
void core_crop_vector(s32 *x, s32 *y);

/* --- keyboard state machine, exposed for the harness ----------------- */

/*
 * down != 0 presses, 0 releases. A usage of 0 releases everything. A press
 * of a key already held, or a release of one that is not, produces NO
 * report - HID reports carry state, and an unchanged state is not news.
 */
void core_key_event(core_state *cs, u8 usage, int down);

/* --- mouse, exposed for the harness ---------------------------------- */

void core_mouse_button(core_state *cs, u8 button, int down);
void core_mouse_move(core_state *cs, s32 dx, s32 dy);
void core_mouse_wheel(core_state *cs, s32 detents, s32 pan);

#endif /* XBOXCTL_CORE_H */
