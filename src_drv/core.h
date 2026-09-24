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

/*
 * THERE IS NO FEATURE REPORT. Configuration arrives on the private control
 * device - ../docs/driver-plan.txt section 7 - not over HID, so IDs 5 and 6
 * are unassigned and the descriptor declares no feature item at all.
 */

/*
 * Payload sizes, EXCLUDING the leading report ID byte. The sink is handed a
 * payload and an ID; whoever writes the wire format prepends the ID.
 */
#define CORE_GAMEPAD_PAYLOAD    36
#define CORE_KEYBOARD_PAYLOAD   8
#define CORE_MOUSE_PAYLOAD      6

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
 * Mouse payload: buttons, 16-bit relative X and Y, then an 8-bit wheel.
 *
 * SIXTEEN BITS PER AXIS. A single report can owe more than 127 counts at
 * a high pointer speed, and a wider axis carries it in one go rather
 * than over several reports.
 *
 * WHAT IS LEFT OVER IS CARRIED, NOT CLAMPED, whatever the width. A
 * report owing more than the axis holds emits what fits and keeps the
 * rest for the next one, so the pointer travels the whole distance and
 * only the delivery is spread.
 *
 * THERE IS NO HORIZONTAL PAN. It sat on the Consumer usage page inside
 * a mouse collection, nothing could drive it, and it is one fewer thing
 * between this descriptor and the ones known to work.
 */
#define CORE_MOUSE_BUTTONS      5
#define CORE_MS_BUTTONS         0
#define CORE_MS_X               1
#define CORE_MS_Y               3
#define CORE_MS_WHEEL           5
#define CORE_MS_STEP_MAX        32767
#define CORE_MS_WHEEL_MAX       127

/* ======================================================================
 * CONFIGURATION
 *
 * The wire format the configurator writes and the driver reads. Layouts
 * are in ../docs/mapping-engine.txt section 10; the stick parameters are
 * in ../docs/analog-to-mouse.txt section 7.
 *
 * FIXED WIDTH THROUGHOUT, NO BITFIELDS, NO ENUMS, NO POINTERS. A 32-bit
 * configurator writes this and a 64-bit driver reads it, and the harness
 * compiles it a third way. Every struct below is laid out so that no
 * compiler needs to insert padding to satisfy alignment.
 * ====================================================================== */

/* 'XBSU' as it appears in the byte stream, which is how XBCD wrote it. */
#define CORE_CFG_SIGNATURE      0x55534258u
#define CORE_CFG_VERSION        1

#define CORE_MAX_LAYOUTS        2       /* mapping-engine.txt section 7 */
#define CORE_MAX_BINDINGS       32      /* per layout */
#define CORE_MAX_CHORDS         8
#define CORE_CHORD_MEMBERS      3
#define CORE_CURVE_POINTS       33
#define CORE_STICK_COUNT        2

/* Chord N is visible to the binding table as this source index plus N. */
#define CORE_SA_CHORD_BASE      64

/* --- actions, mapping-engine.txt section 3 --------------------------- */
#define CORE_ACT_NONE           0
#define CORE_ACT_KEY            1   /* code = HID usage                   */
#define CORE_ACT_MOUSE_BUTTON   2   /* code = 1..5                        */
#define CORE_ACT_MOUSE_WHEEL    3   /* code = signed detents              */
#define CORE_ACT_JOY_BUTTON     4   /* code = 1..CORE_GP_BUTTON_COUNT     */
#define CORE_ACT_JOY_AXIS       5   /* code = axis in low bits, sign high */
#define CORE_ACT_JOY_POV        6   /* code = direction 0..3              */
#define CORE_ACT_MOUSE_PULSE    7   /* code = dx low byte, dy high byte   */
#define CORE_ACT_LAYER_HOLD     8   /* code = layer index                 */
#define CORE_ACT_LAYER_SET      9   /* code = layer index                 */
#define CORE_ACT_LAYER_CYCLE    10  /* code = signed step                 */
#define CORE_ACT_COUNT          11

/* --- binding flags, mapping-engine.txt sections 4, 4.1, 5 and 6 ------ */
#define CORE_BF_REPEAT          0x01
#define CORE_BF_TOGGLE          0x02
#define CORE_BF_ANALOG          0x04
#define CORE_BF_NO_REPEAT_FIRST 0x08
#define CORE_BF_PASSTHROUGH     0x10    /* keep the default gamepad out */
#define CORE_BF_KNOWN           0x1F

/* --- stick modes, analog-to-mouse.txt section 7 ---------------------- */
#define CORE_STICK_OFF          0
#define CORE_STICK_MOUSE        1   /* relative pointer              */
#define CORE_STICK_ABSOLUTE     2   /* RESERVED - see below          */
#define CORE_STICK_JOY          3   /* drives the gamepad axes       */
#define CORE_STICK_WHEEL        4   /* vertical scroll               */
#define CORE_STICK_MODE_COUNT   5

/*
 * ABSOLUTE IS RESERVED AND BEHAVES AS OFF, and the reason is the
 * descriptor rather than the arithmetic.
 *
 * An absolute pointer reports WHERE IT IS, not how far it moved. The
 * mouse collection declares relative axes, and a relative device cannot
 * reach a position it has no way of observing - it does not know where
 * the cursor is or how large the screen is. Absolute needs a fourth
 * top-level collection declaring absolute axes.
 *
 * That is a descriptor change, and the descriptor is the one part of
 * this driver where a change cost an entire debugging session: the
 * collection ORDER turned out to be load-bearing. The number is kept so
 * the wire format does not shift when it is implemented.
 * analog-to-mouse.txt section 10 has the behaviour it should have.
 */

typedef struct _core_binding {      /* 12 bytes */
	u8  source;             /* semiaxis, or CORE_SA_CHORD_BASE + N  */
	u8  action;             /* CORE_ACT_*                           */
	u16 code;               /* per action                           */
	u16 on_at;              /* activation, CORE_MAX_VALUE units     */
	u16 off_at;             /* release, <= on_at                    */
	u8  flags;              /* CORE_BF_*                            */
	u8  repeat_hz;          /* 0 = no repeat                        */
	u16 repeat_delay_ms;    /* before the first repeat              */
} core_binding;

typedef struct _core_chord {        /* 4 bytes */
	u8  member[CORE_CHORD_MEMBERS]; /* sources, CORE_SA_NONE unused */
	u8  flags;
} core_chord;

/*
 * THE u16 FIELDS COME FIRST so the curve table lands on an even offset
 * without padding on any of the three compilers this is built by.
 */
typedef struct _core_stick {        /* 88 bytes */
	u16 deadzone;           /* radial, CORE_MAX_VALUE units         */
	u16 outer;              /* deflection treated as full           */
	u16 max_speed;          /* px/s at full deflection              */
	u16 accel_threshold;    /* u above which boost accrues, Q16     */
	u16 accel_rate;         /* boost per second, Q8; 0 = off        */
	u16 accel_max;          /* boost ceiling, Q8                    */
	u16 accel_decay;        /* boost lost per second, Q8            */
	u8  mode;               /* CORE_STICK_*                         */
	u8  gain_x;             /* per-axis trim, 128 = 1.0             */
	u8  gain_y;
	u8  invert_x;
	u8  invert_y;
	u8  smooth_ms;          /* rise-only one-pole time constant     */
	u8  reserved[2];
	u16 curve[CORE_CURVE_POINTS];   /* g sampled at 33 points       */
} core_stick;

typedef struct _core_layout {       /* 420 bytes */
	core_binding binding[CORE_MAX_BINDINGS];
	core_chord   chord[CORE_MAX_CHORDS];
	u8           led;       /* 360 LED pattern for this layer       */
	u8           reserved[3];
} core_layout;

typedef struct _core_config_header {    /* 32 bytes */
	u32 signature;
	u16 version;
	u16 header_bytes;
	u16 layout_bytes;       /* stride of one layout                 */
	u16 stick_bytes;        /* stride of one stick                  */
	u8  layout_count;       /* 1..CORE_MAX_LAYOUTS                  */
	u8  binding_count;      /* per layout                           */
	u8  chord_count;
	u8  collections;        /* bit 0 gamepad, 1 keyboard, 2 mouse   */
	u16 tick_hz;
	u8  reserved[14];
} core_config_header;

/*
 * The parsed, validated, in-memory form. NOT the wire format: the blob is
 * walked by the strides in its header, which a newer configurator may
 * have grown, and copied field by field into this.
 */
typedef struct _core_config {
	u8          valid;
	u8          layout_count;
	u8          binding_count;
	u8          chord_count;
	u8          collections;
	u16         tick_hz;
	core_stick  stick[CORE_STICK_COUNT];    /* global, not per layer */
	core_layout layout[CORE_MAX_LAYOUTS];

	/*
	 * One bit per source whose default gamepad output this layout
	 * replaces - mapping-engine.txt section 4.1. Built once, here, so
	 * evaluation costs one AND rather than a search.
	 */
	u32         suppress[CORE_MAX_LAYOUTS];

	/*
	 * One bit per source any chord in this layout names. Held back
	 * while the chord it belongs to is live, and during the hold-off
	 * that keeps a chord from being preceded by its own members.
	 */
	u32         chord_members[CORE_MAX_LAYOUTS];

	/*
	 * The semiaxes a stick has taken for the pointer. A stick driving
	 * the mouse must stop driving the gamepad axes as well, or every
	 * aim moves the crosshair twice - section 4.1's rule, arrived at
	 * from the other direction.
	 */
	u32         stick_claim;
} core_config;

/*
 * Load outcomes.
 *
 * STRUCTURE IS REJECTED, VALUES ARE REPAIRED. A blob whose signature,
 * version or strides do not describe something walkable is refused
 * outright, because guessing at its shape reads memory that is not there.
 * A blob that is shaped correctly but holds a nonsense threshold or an
 * out-of-range key is loaded with that field clamped, because the
 * alternative is that one bad byte costs the user every binding they have.
 */
#define CORE_CFG_OK             0
#define CORE_CFG_ERR_SHORT      1   /* smaller than a header            */
#define CORE_CFG_ERR_SIGNATURE  2
#define CORE_CFG_ERR_VERSION    3
#define CORE_CFG_ERR_STRIDE     4   /* a stride smaller than its struct */
#define CORE_CFG_ERR_COUNT      5   /* a count past its ceiling         */
#define CORE_CFG_ERR_TRUNCATED  6   /* strides and counts overrun len   */

/*
 * Parse and validate a blob into cfg. On any non-zero return cfg is left
 * untouched, so a rejected push never disturbs a running map. repaired,
 * if given, receives the number of fields that were clamped.
 */
int core_config_load(core_config *cfg, const u8 *blob, u32 len,
                     u32 *repaired);

/*
 * The built-in configuration: layer 1 the plain pad, layer 2 the same
 * with the four face buttons autofiring, and Start plus Back cycling
 * between them.
 *
 * THIS IS WHAT THE PAD RUNS UNTIL A CONFIGURATOR PUSHES SOMETHING ELSE.
 * The driver reads no store of its own - see ../docs/driver-plan.txt
 * section 7 - so this is also what is live during boot and at the logon
 * screen, where no user-mode process is running to push a profile.
 */
void core_config_defaults(core_config *cfg);

/*
 * Rebuild the per-layout suppression masks. Called by load and by
 * defaults; exposed so a caller that edits a binding in place can
 * refresh them without a round trip through the blob.
 */
void core_config_suppress(core_config *cfg);

/*
 * Serialise cfg into a blob. Returns the byte count written, or 0 if len
 * is too small. The harness uses it to prove load(save(x)) == x.
 */
u32 core_config_save(const core_config *cfg, u8 *blob, u32 len);

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

/*
 * Per-binding runtime state. One of these for every slot in every
 * layout, so a binding that is holding something keeps holding it while
 * another layer is live and can be released deliberately.
 */
typedef struct _core_bind_state {
	u8  active;             /* the source is past its threshold  */
	u8  latched;            /* TOGGLE output, independent of it  */
	u8  repeat_on;          /* the asserted half of the cycle    */
	u8  repeat_done;        /* NO_REPEAT_FIRST has had its turn  */
	u64 repeat_at;          /* when the next flip falls due      */
} core_bind_state;

/*
 * The gamepad the bindings and the default map fold into, before it is
 * serialised into a report.
 *
 * IT IS AN ACCUMULATOR, NOT A COPY OF THE PAD. Two bindings may drive
 * one button, and a binding may drive an axis the default map is also
 * driving, so axes sum and buttons OR rather than overwrite.
 */
typedef struct _core_gamepad_out {
	s32 axis[CORE_GP_AXIS_COUNT];   /* CORE_MAX_VALUE units      */
	u16 buttons;
	u8  hat_index;          /* bit 0 up, 1 down, 2 left, 3 right */
	u8  reserved;
} core_gamepad_out;

/*
 * What one stick carries between ticks. All three of these are why the
 * pipeline needs a clock rather than just a deflection.
 */
typedef struct _core_stick_state {
	s32 u_prev;             /* smoothing, 0..65535               */
	s32 boost;              /* acceleration, Q8, added to 256    */

	/*
	 * THE SUB-PIXEL REMAINDER, and it is the whole reason the
	 * Adaptoid needed threads. Velocity times elapsed time is
	 * almost never a whole number of pixels; carrying what is left
	 * over into the next tick makes the long-term velocity error
	 * exactly zero instead of up to twenty per cent.
	 *
	 * Units are pixels times microseconds, so it takes 64 bits.
	 */
	s64 accum_x;
	s64 accum_y;
} core_stick_state;

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

	/* --- layers --- */
	u8              layout;         /* the EFFECTIVE layer      */
	u8              layer_base;     /* what CYCLE and SET latch */
	u8              layer_pending;
	u8              layer_pending_valid;

	/* --- the stick pipeline --- */
	core_stick_state stick_state[CORE_STICK_COUNT];

	/*
	 * WHEN THE STICKS WERE LAST ADVANCED, which is not the same as
	 * when the last packet arrived. Both the packet path and the
	 * periodic tick run the pipeline, and measuring from here rather
	 * than from either clock is what stops the motion being counted
	 * twice when both fire between one report and the next.
	 */
	u64             last_stick_100ns;

	/* --- chords --- */
	s32             chord_value[CORE_MAX_CHORDS];

	/*
	 * Sources held back this packet, so a chord is not preceded by
	 * its own members. Folded into the source lookup rather than
	 * checked at each use, so a held source reads as released and
	 * every binding on it deactivates the ordinary way.
	 */
	u32             hold_mask;

	/* --- output state --- */
	core_keyboard_state  kb;
	core_mouse_state     ms;

	/* --- configuration and the state it drives --- */
	core_config     cfg;
	core_bind_state bind[CORE_MAX_LAYOUTS][CORE_MAX_BINDINGS];
	core_gamepad_out gp;

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

/*
 * Install a configuration from a blob. Returns a CORE_CFG_* code; on
 * anything but CORE_CFG_OK the running configuration is untouched.
 *
 * A SUCCESSFUL SWAP RELEASES EVERY ASSERTED OUTPUT FIRST. The bindings
 * that were holding a key down may not exist in the new table, and
 * nothing else would ever release it.
 */
int core_set_config(core_state *cs, const u8 *blob, u32 len,
                    u32 *repaired);

/* The built-in configuration, installed the same way. */
void core_set_config_default(core_state *cs);

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
