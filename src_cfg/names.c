/*
 * names.c - every symbolic name the profile text uses.
 *
 * THE DRIVER NEVER SEES A CHARACTER. 'w' is HID usage 0x1A, not ASCII
 * 0x77, and the keyboard layout lives here rather than in the kernel
 * for the same reason the curve does: it is a property of the person
 * using the pad, not of the pad.
 *
 * Each table ends with a NULL name. Nothing here includes a Windows
 * header, so the tables can be checked without a message loop.
 */

#include <string.h>
#include "xbconfig.h"

/* Defined below, beside the other lookups. */
static int xb_ieq(const char *a, const char *b);

/* In the order core.h declares them. */
const xb_name XB_SOURCES[] = {
	{ "dup",      CORE_SA_DPAD_UP },
	{ "ddown",    CORE_SA_DPAD_DOWN },
	{ "dleft",    CORE_SA_DPAD_LEFT },
	{ "dright",   CORE_SA_DPAD_RIGHT },
	{ "start",    CORE_SA_START },
	{ "back",     CORE_SA_BACK },
	{ "lthumb",   CORE_SA_LTHUMB },
	{ "rthumb",   CORE_SA_RTHUMB },
	{ "a",        CORE_SA_A },
	{ "b",        CORE_SA_B },
	{ "x",        CORE_SA_X },
	{ "y",        CORE_SA_Y },
	{ "black",    CORE_SA_BLACK },
	{ "white",    CORE_SA_WHITE },
	{ "ltrigger", CORE_SA_LTRIGGER },
	{ "rtrigger", CORE_SA_RTRIGGER },
	/*
	 * UP IS THE NEGATIVE SEMIAXIS, AND THAT IS NOT A TYPO. The pad
	 * reports Y up-positive and the decode negates it once so the
	 * value is down-positive the way HID wants, which leaves a
	 * physical UP push in the semiaxis named YNEG. Naming these the
	 * other way round reads correctly against the struct and is wrong
	 * against the thumb.
	 */
	{ "lstick_left",  CORE_SA_LSTICK_XNEG },
	{ "lstick_right", CORE_SA_LSTICK_XPOS },
	{ "lstick_up",    CORE_SA_LSTICK_YNEG },
	{ "lstick_down",  CORE_SA_LSTICK_YPOS },
	{ "rstick_left",  CORE_SA_RSTICK_XNEG },
	{ "rstick_right", CORE_SA_RSTICK_XPOS },
	{ "rstick_up",    CORE_SA_RSTICK_YNEG },
	{ "rstick_down",  CORE_SA_RSTICK_YPOS },
	{ "guide",    CORE_SA_GUIDE },
	{ NULL, 0 }
};

/* What each control is called on the dialog, in the same order. */
static const char *XB_SOURCE_LABELS[] = {
	"D-Up", "D-Down", "D-Left", "D-Right",
	"Start", "Back", "L-Thumb", "R-Thumb",
	"A", "B", "X", "Y",
	"Black", "White", "L-Trigger", "R-Trigger",
	"L-Stick Left", "L-Stick Right", "L-Stick Up", "L-Stick Down",
	"R-Stick Left", "R-Stick Right", "R-Stick Up", "R-Stick Down",
	"Guide"
};

const xb_name XB_ACTIONS[] = {
	{ "none",         CORE_ACT_NONE },
	{ "key",          CORE_ACT_KEY },
	{ "mouse_button", CORE_ACT_MOUSE_BUTTON },
	{ "mouse_wheel",  CORE_ACT_MOUSE_WHEEL },
	{ "joy_button",   CORE_ACT_JOY_BUTTON },
	{ "joy_axis",     CORE_ACT_JOY_AXIS },
	{ "joy_pov",      CORE_ACT_JOY_POV },
	{ "mouse_pulse",  CORE_ACT_MOUSE_PULSE },
	{ "layer_hold",   CORE_ACT_LAYER_HOLD },
	{ "layer_set",    CORE_ACT_LAYER_SET },
	{ "layer_cycle",  CORE_ACT_LAYER_CYCLE },
	{ NULL, 0 }
};

const xb_name XB_FLAGS[] = {
	{ "repeat",      CORE_BF_REPEAT },
	{ "toggle",      CORE_BF_TOGGLE },
	{ "analog",      CORE_BF_ANALOG },
	{ "passthrough", CORE_BF_PASSTHROUGH },
	{ NULL, 0 }
};

const xb_name XB_STICK_MODES[] = {
	{ "off",      CORE_STICK_OFF },
	{ "mouse",    CORE_STICK_MOUSE },
	{ "joy",      CORE_STICK_JOY },
	{ "wheel",    CORE_STICK_WHEEL },
	{ NULL, 0 }
};

const xb_name XB_MOUSE_BUTTONS[] = {
	{ "left", 1 }, { "right", 2 }, { "middle", 3 },
	{ "x1", 4 }, { "x2", 5 },
	{ NULL, 0 }
};

/*
 * CURVE PRESETS. Two control points describe the whole curve - see
 * ../docs/analog-to-mouse.txt section 4.1.
 *
 * FULL PRECISION, NOT ROUNDED. Two of these are thirds, and carrying
 * them to three decimals moves every entry of the 33-point table by
 * enough to disagree with tools/mkconfig.py on the same profile.
 */
const xb_curve XB_CURVES[] = {
	{ "linear",     1.0 / 3.0, 2.0 / 3.0 },
	{ "quad",       0.0,       1.0 / 3.0 },
	{ "cubic",      0.0,       0.0 },
	{ "smoothstep", 0.0,       1.0 },
	{ NULL, 0.0, 0.0 }
};

int xb_curve_find(const char *name, double *y1, double *y2)
{
	u32 i;

	for (i = 0; XB_CURVES[i].name != NULL; i++) {
		if (xb_ieq(XB_CURVES[i].name, name)) {
			*y1 = XB_CURVES[i].y1;
			*y2 = XB_CURVES[i].y2;
			return 1;
		}
	}
	return 0;
}

/*
 * HID usages. Letters, digits and function keys are generated rather
 * than listed - they are contiguous and a typed-out table is 48 more
 * chances to transpose a number.
 */
static char XB_KEY_NAMES[26 + 10 + 12][4];
static xb_name XB_KEYS_TABLE[26 + 10 + 12 + 80];
static int XB_KEYS_BUILT;

static const xb_name XB_KEYS_NAMED[] = {
	{ "enter", 0x28 }, { "escape", 0x29 }, { "esc", 0x29 },
	{ "backspace", 0x2A }, { "tab", 0x2B }, { "space", 0x2C },
	{ "minus", 0x2D }, { "equals", 0x2E }, { "lbracket", 0x2F },
	{ "rbracket", 0x30 }, { "backslash", 0x31 }, { "semicolon", 0x33 },
	{ "quote", 0x34 }, { "grave", 0x35 }, { "comma", 0x36 },
	{ "period", 0x37 }, { "slash", 0x38 }, { "capslock", 0x39 },
	{ "printscreen", 0x46 }, { "scrolllock", 0x47 }, { "pause", 0x48 },
	{ "insert", 0x49 }, { "home", 0x4A }, { "pageup", 0x4B },
	{ "delete", 0x4C }, { "end", 0x4D }, { "pagedown", 0x4E },
	{ "right", 0x4F }, { "left", 0x50 }, { "down", 0x51 }, { "up", 0x52 },
	{ "lctrl", 0xE0 }, { "lshift", 0xE1 }, { "lalt", 0xE2 },
	{ "lgui", 0xE3 }, { "rctrl", 0xE4 }, { "rshift", 0xE5 },
	{ "ralt", 0xE6 }, { "rgui", 0xE7 },

	/*
	 * THE KEYPAD IS ITS OWN SET OF USAGES. Keypad Enter is 0x58 and the
	 * Enter above it is 0x28; keypad 4 is 0x5C and the 4 on the number
	 * row is 0x21. A scan code tells them apart, so they need names to
	 * be written back as.
	 */
	{ "numlock", 0x53 },
	{ "kpslash", 0x54 }, { "kpstar", 0x55 },
	{ "kpminus", 0x56 }, { "kpplus", 0x57 }, { "kpenter", 0x58 },
	{ "kp1", 0x59 }, { "kp2", 0x5A }, { "kp3", 0x5B },
	{ "kp4", 0x5C }, { "kp5", 0x5D }, { "kp6", 0x5E },
	{ "kp7", 0x5F }, { "kp8", 0x60 }, { "kp9", 0x61 },
	{ "kp0", 0x62 }, { "kpperiod", 0x63 },
	{ "nonusbackslash", 0x64 }, { "menu", 0x65 },
	{ NULL, 0 }
};

static void xb_keys_build(void)
{
	int n = 0;
	int i;

	if (XB_KEYS_BUILT) {
		return;
	}

	for (i = 0; i < 26; i++) {
		XB_KEY_NAMES[n][0] = (char)('a' + i);
		XB_KEY_NAMES[n][1] = 0;
		XB_KEYS_TABLE[n].name  = XB_KEY_NAMES[n];
		XB_KEYS_TABLE[n].value = (u32)(0x04 + i);
		n++;
	}
	for (i = 0; i < 10; i++) {
		/* 1..9 then 0, which is the order the usages run in. */
		XB_KEY_NAMES[n][0] = (char)(i == 9 ? '0' : '1' + i);
		XB_KEY_NAMES[n][1] = 0;
		XB_KEYS_TABLE[n].name  = XB_KEY_NAMES[n];
		XB_KEYS_TABLE[n].value = (u32)(0x1E + i);
		n++;
	}
	for (i = 1; i <= 12; i++) {
		XB_KEY_NAMES[n][0] = 'f';
		if (i < 10) {
			XB_KEY_NAMES[n][1] = (char)('0' + i);
			XB_KEY_NAMES[n][2] = 0;
		} else {
			XB_KEY_NAMES[n][1] = '1';
			XB_KEY_NAMES[n][2] = (char)('0' + i - 10);
			XB_KEY_NAMES[n][3] = 0;
		}
		XB_KEYS_TABLE[n].name  = XB_KEY_NAMES[n];
		XB_KEYS_TABLE[n].value = (u32)(0x3A + i - 1);
		n++;
	}
	for (i = 0; XB_KEYS_NAMED[i].name != NULL; i++) {
		XB_KEYS_TABLE[n++] = XB_KEYS_NAMED[i];
	}
	XB_KEYS_TABLE[n].name  = NULL;
	XB_KEYS_TABLE[n].value = 0;
	XB_KEYS_BUILT = 1;
}

const xb_name *xb_keys(void)
{
	xb_keys_build();
	return XB_KEYS_TABLE;
}

static int xb_ieq(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = *a;
		char cb = *b;

		if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca - 'A' + 'a'); }
		if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb - 'A' + 'a'); }
		if (ca != cb) {
			return 0;
		}
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int xb_name_to_value(const xb_name *table, const char *name, u32 *value)
{
	u32 i;

	for (i = 0; table[i].name != NULL; i++) {
		if (xb_ieq(table[i].name, name)) {
			*value = table[i].value;
			return 1;
		}
	}
	return 0;
}

const char *xb_value_to_name(const xb_name *table, u32 value)
{
	u32 i;

	for (i = 0; table[i].name != NULL; i++) {
		if (table[i].value == value) {
			return table[i].name;
		}
	}
	return NULL;
}

u32 xb_name_count(const xb_name *table)
{
	u32 i = 0;

	while (table[i].name != NULL) {
		i++;
	}
	return i;
}

const char *xb_source_label(u8 source)
{
	if (source < CORE_SEMIAXIS_COUNT) {
		return XB_SOURCE_LABELS[source];
	}
	return "?";
}
