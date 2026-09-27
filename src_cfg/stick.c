/*
 * stick.c - the dialog behind the two stick buttons.
 *
 * A STICK'S MODE IS NOT A BINDING. Every other control on the main
 * dialog maps to one 12-byte binding record; a stick carries an 88-byte
 * record of its own, it is GLOBAL rather than per layer, and it is what
 * turns the stick into a pointer. That is why the two stick cells open
 * this instead of the binding dialog.
 *
 * The stick's four DIRECTIONS are still ordinary sources and are bound
 * from the grid like any button. Both can be live at once: a stick in
 * mouse mode with its up direction bound to a key will move the pointer
 * and type, from one push.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbconfig.h"
#include "resource.h"

extern HINSTANCE xb_instance(void);

static xb_profile *g_SP;
static u32         g_Which;     /* 0 left, 1 right */

static core_config g_StickBefore;
static int         g_StickPushed;

/* Defined below, past the helpers it needs. */
static void xb_graph_update(HWND dlg);

static void xb_set_num(HWND dlg, int id, u32 value)
{
	char text[32];

	snprintf(text, sizeof(text), "%u", (unsigned)value);
	SetDlgItemTextA(dlg, id, text);
}

static u32 xb_get_num(HWND dlg, int id, u32 limit)
{
	char text[32];
	long v;

	GetDlgItemTextA(dlg, id, text, sizeof(text));
	v = strtol(text, NULL, 10);
	if (v < 0) {
		v = 0;
	}
	if ((u32)v > limit) {
		v = (long)limit;
	}
	return (u32)v;
}

/*
 * max_speed MEANS TWO DIFFERENT THINGS and the dialog has to say which.
 * A pointer wants thousands of pixels a second; a wheel wants ten or
 * twenty detents. The same field, a factor of a hundred apart, and a
 * profile that forgets will scroll a document into next week.
 */
static void xb_stick_units(HWND dlg)
{
	int  sel = (int)SendDlgItemMessageA(dlg, IDC_ST_MODE, CB_GETCURSEL,
	                                   0, 0);
	u8   mode = (u8)(sel == CB_ERR ? 0 : XB_STICK_MODES[sel].value);
	int  live = (mode != CORE_STICK_OFF);
	int  rate = (mode == CORE_STICK_MOUSE || mode == CORE_STICK_WHEEL);

	if (mode == CORE_STICK_WHEEL) {
		SetDlgItemTextA(dlg, IDC_ST_SPEEDUNIT,
		                "DETENTS a second, not pixels");
		SetDlgItemTextA(dlg, IDC_ST_HINT,
		                "Wheel is vertical only - sideways does nothing,"
		                " and the deadzone is on Y alone.");
	} else if (mode == CORE_STICK_MOUSE) {
		SetDlgItemTextA(dlg, IDC_ST_SPEEDUNIT, "pixels a second at full");
		SetDlgItemTextA(dlg, IDC_ST_HINT,
		                "The deadzone is what stops a resting stick"
		                " drifting. This pad rests some way off centre.");
	} else if (mode == CORE_STICK_JOY) {
		SetDlgItemTextA(dlg, IDC_ST_SPEEDUNIT, "(not used in joy mode)");
		SetDlgItemTextA(dlg, IDC_ST_HINT,
		                "Joy mode leaves the stick driving the gamepad"
		                " axes, which is what it does by default.");
	} else {
		SetDlgItemTextA(dlg, IDC_ST_SPEEDUNIT, "");
		SetDlgItemTextA(dlg, IDC_ST_HINT,
		                "Off leaves the stick as a plain gamepad axis."
		                " Its directions can still be bound to keys.");
	}

	EnableWindow(GetDlgItem(dlg, IDC_ST_MAXSPEED), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_CURVE), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_GAINX), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_GAINY), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_SMOOTH), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_ARATE), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_AMAX), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_ADECAY), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_ATHRESH), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_DEADZONE), live ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_OUTER), live ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_INVX), rate ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_ST_INVY), rate ? TRUE : FALSE);

	/* HIDDEN, NOT GREYED. An empty black box invites the question
	 * of what it is failing to show. */
	ShowWindow(GetDlgItem(dlg, IDC_ST_GRAPH),
	           rate ? SW_SHOW : SW_HIDE);
	xb_graph_update(dlg);
}

/*
 * How many curve presets there are.
 *
 * COUNTED HERE RATHER THAN THROUGH xb_name_count, because XB_CURVES is
 * an xb_curve and not an xb_name: the two have different layouts and
 * casting one to the other would read a double as a u32.
 */
static u32 xb_curve_count(void)
{
	u32 n = 0;

	while (XB_CURVES[n].name != NULL) {
		n++;
	}
	return n;
}

/* Which preset produced this table, if one did. */
static int xb_curve_index(const u16 curve[CORE_CURVE_POINTS])
{
	u32 i;

	for (i = 0; XB_CURVES[i].name != NULL; i++) {
		u16 test[CORE_CURVE_POINTS];

		xb_curve_build(XB_CURVES[i].y1, XB_CURVES[i].y2, test);
		if (memcmp(test, curve, sizeof(test)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* ======================================================================
 * THE CURVE, DRAWN
 *
 * WHAT IS PLOTTED IS THE WHOLE RESPONSE, not the Bezier alone:
 * deflection across, speed up, with the deadzone and the outer clip in
 * it. That is why it changes when any of those numbers change and not
 * only when the preset does - the flat run at the left IS the deadzone,
 * and the corner at the right IS outer.
 *
 * SMOOTHING AND ACCELERATION ARE ABSENT because neither is a function of
 * deflection. One is a filter over time and the other grows with time
 * held, so there is no honest way to put either on this axis.
 *
 * The maths is core_stick_speed's, step for step, so the picture cannot
 * drift from what the driver will do.
 * ====================================================================== */

/* The table the dialog is currently describing. */
static void xb_graph_curve(HWND dlg, const core_stick *st,
                           u16 out[CORE_CURVE_POINTS])
{
	int sel = (int)SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_GETCURSEL,
	                                   0, 0);

	if (sel != CB_ERR && sel < (int)xb_curve_count()) {
		xb_curve_build(XB_CURVES[sel].y1, XB_CURVES[sel].y2, out);
		return;
	}
	/* "(custom)" - the stored table is the only description of it. */
	memcpy(out, st->curve, sizeof(u16) * CORE_CURVE_POINTS);
}

static void xb_graph_paint(HWND dlg, const DRAWITEMSTRUCT *di)
{
	const core_stick *st = &g_SP->cfg.stick[g_Which];
	u16      curve[CORE_CURVE_POINTS];
	HBRUSH   back;
	HPEN     pen;
	HPEN     old_pen;
	RECT     r = di->rcItem;
	int      w = r.right - r.left;
	int      h = r.bottom - r.top;
	s32      deadzone;
	s32      outer;
	s32      max_speed;
	int      px;

	back = CreateSolidBrush(RGB(0, 0, 0));
	FillRect(di->hDC, &r, back);
	DeleteObject(back);

	if (w < 4 || h < 4) {
		return;
	}

	/*
	 * READ FROM THE FIELDS, NOT FROM THE CONFIG, so the line follows
	 * what is being typed rather than what was last saved.
	 */
	deadzone  = (s32)xb_get_num(dlg, IDC_ST_DEADZONE, CORE_MAX_VALUE);
	outer     = (s32)xb_get_num(dlg, IDC_ST_OUTER, CORE_MAX_VALUE);
	max_speed = (s32)xb_get_num(dlg, IDC_ST_MAXSPEED, 65535);
	if (outer <= deadzone) {
		outer = deadzone + 1;
	}
	if (max_speed <= 0) {
		max_speed = 1;
	}

	xb_graph_curve(dlg, st, curve);

	pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 0));
	old_pen = (HPEN)SelectObject(di->hDC, pen);

	for (px = 0; px < w; px++) {
		s32 magnitude = (s32)(((s64)px * CORE_MAX_VALUE) / (w - 1));
		s32 speed = 0;
		int y;

		if (magnitude > deadzone) {
			s32 rc = (magnitude < outer) ? magnitude : outer;
			s32 u  = (s32)(((s64)(rc - deadzone) * 65535) /
			               (outer - deadzone));
			s32 i  = u >> 11;
			s32 f  = u & 0x7FF;
			s32 g;

			if (i >= CORE_CURVE_POINTS - 1) {
				i = CORE_CURVE_POINTS - 2;
				f = 0x7FF;
			}
			g = (s32)curve[i] +
			    (((s32)curve[i + 1] - (s32)curve[i]) * f >> 11);
			speed = (s32)(((s64)max_speed * g) >> 16);
		}

		y = r.bottom - 1 -
		    (int)(((s64)speed * (h - 1)) / max_speed);
		if (y < r.top) {
			y = r.top;
		}
		if (y > r.bottom - 1) {
			y = r.bottom - 1;
		}

		if (px == 0) {
			MoveToEx(di->hDC, r.left, y, NULL);
		} else {
			LineTo(di->hDC, r.left + px, y);
		}
	}

	SelectObject(di->hDC, old_pen);
	DeleteObject(pen);
}

static void xb_graph_update(HWND dlg)
{
	HWND g = GetDlgItem(dlg, IDC_ST_GRAPH);

	if (g != NULL) {
		InvalidateRect(g, NULL, TRUE);
	}
}

/*
 * Put every field back to the driver's built-in default.
 *
 * READ OUT OF core_config_defaults, NOT LISTED AGAIN HERE. A second copy
 * of the defaults is a second thing to forget when one of them moves,
 * and the deadzone has already moved twice.
 *
 * THE MODE IS LEFT ALONE. It is the one field that says what the stick
 * is FOR, and somebody resetting the numbers has not asked to stop using
 * the stick as a pointer.
 */
static void xb_stick_defaults(HWND dlg)
{
	static core_config fresh;
	const core_stick  *d;
	int                pick;

	core_config_defaults(&fresh);
	d = &fresh.stick[g_Which];

	xb_set_num(dlg, IDC_ST_DEADZONE, d->deadzone);
	xb_set_num(dlg, IDC_ST_OUTER,    d->outer);
	xb_set_num(dlg, IDC_ST_MAXSPEED, d->max_speed);
	xb_set_num(dlg, IDC_ST_GAINX,    d->gain_x);
	xb_set_num(dlg, IDC_ST_GAINY,    d->gain_y);
	xb_set_num(dlg, IDC_ST_SMOOTH,   d->smooth_ms);
	xb_set_num(dlg, IDC_ST_ATHRESH,  d->accel_threshold);
	xb_set_num(dlg, IDC_ST_ARATE,    d->accel_rate);
	xb_set_num(dlg, IDC_ST_AMAX,     d->accel_max);
	xb_set_num(dlg, IDC_ST_ADECAY,   d->accel_decay);

	CheckDlgButton(dlg, IDC_ST_INVX,
	               d->invert_x ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(dlg, IDC_ST_INVY,
	               d->invert_y ? BST_CHECKED : BST_UNCHECKED);

	/* The default table IS a preset, so the list can show which. */
	pick = xb_curve_index(d->curve);
	if (pick >= 0) {
		SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_SETCURSEL,
		                    (WPARAM)pick, 0);
	}

	xb_stick_units(dlg);            /* redraws the graph as well */
}

/*
 * The dialog's fields as a stick record.
 *
 * SHARED BY OK AND TEST, because they have to agree exactly. Reading the
 * controls twice in two places is how Test ends up trying something
 * subtly different from what OK would save.
 */
static void xb_stick_read(HWND dlg, core_stick *out)
{
	int sel;

	sel = (int)SendDlgItemMessageA(dlg, IDC_ST_MODE, CB_GETCURSEL, 0, 0);
	if (sel != CB_ERR) {
		out->mode = (u8)XB_STICK_MODES[sel].value;
	}

	out->deadzone  = (u16)xb_get_num(dlg, IDC_ST_DEADZONE,
	                                 CORE_MAX_VALUE);
	out->outer     = (u16)xb_get_num(dlg, IDC_ST_OUTER, CORE_MAX_VALUE);
	out->max_speed = (u16)xb_get_num(dlg, IDC_ST_MAXSPEED, 65535);
	out->gain_x    = (u8)xb_get_num(dlg, IDC_ST_GAINX, 255);
	out->gain_y    = (u8)xb_get_num(dlg, IDC_ST_GAINY, 255);
	out->smooth_ms = (u8)xb_get_num(dlg, IDC_ST_SMOOTH, 255);
	out->accel_threshold = (u16)xb_get_num(dlg, IDC_ST_ATHRESH, 65535);
	out->accel_rate      = (u16)xb_get_num(dlg, IDC_ST_ARATE, 65535);
	out->accel_max       = (u16)xb_get_num(dlg, IDC_ST_AMAX, 65535);
	out->accel_decay     = (u16)xb_get_num(dlg, IDC_ST_ADECAY, 65535);
	out->invert_x = (u8)(IsDlgButtonChecked(dlg, IDC_ST_INVX)
	                     == BST_CHECKED ? 1 : 0);
	out->invert_y = (u8)(IsDlgButtonChecked(dlg, IDC_ST_INVY)
	                     == BST_CHECKED ? 1 : 0);

	/*
	 * OUTER ABOVE DEADZONE, or the rescale divides by zero. The driver
	 * repairs this; repairing it here means the profile on disk says what
	 * the pad will actually do.
	 */
	if (out->outer <= out->deadzone) {
		out->outer = (u16)(out->deadzone + 1);
	}

	sel = (int)SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_GETCURSEL, 0, 0);
	if (sel != CB_ERR && sel < (int)xb_curve_count()) {
		xb_curve_build(XB_CURVES[sel].y1, XB_CURVES[sel].y2, out->curve);
	}
	/* "(custom)" leaves the table alone - see WM_INITDIALOG. */
}

static const char XB_STICK_HELP[] =
    "STICK SETTINGS\r\n"
    "\r\n"
    "Mode\r\n"
    "    off     A plain gamepad axis. Directions can still be bound to keys,\r\n"
    "            and the deadzone below is what those are tested against.\r\n"
    "    mouse   Drives the pointer.\r\n"
    "    joy     A gamepad axis, explicitly.\r\n"
    "    wheel   Scrolls. Vertical only, and Max speed then means DETENTS per\r\n"
    "            second - you want ten or twenty, not thousands.\r\n"
    "\r\n"
    "Deadzone\r\n"
    "    How far the stick must move before anything happens, 0-35000. This is\r\n"
    "    a measurement, not a taste: a stick at rest does not sit at centre,\r\n"
    "    and this pad rests 1400-1900 units off it. Set it lower than the\r\n"
    "    resting offset and the pointer drifts on its own.\r\n"
    "\r\n"
    "Outer\r\n"
    "    Deflection treated as maximum, 0-35000. Beyond it is full speed.\r\n"
    "    Lower it if the stick cannot physically reach its corners.\r\n"
    "\r\n"
    "Max speed\r\n"
    "    Speed at full deflection: pixels per second in mouse mode, detents\r\n"
    "    per second in wheel mode.\r\n"
    "\r\n"
    "Curve\r\n"
    "    The shape between Deadzone and Outer, drawn in the graph. Linear is a\r\n"
    "    straight line. quad and cubic start gently and finish fast, buying\r\n"
    "    fine control near centre at the cost of top-end response.\r\n"
    "\r\n"
    "Gain X / Gain Y\r\n"
    "    Per-axis scale in 128ths, 128 being 1.0. Y defaults to 54, about\r\n"
    "    0.42, because a pointer wants less vertical travel than horizontal.\r\n"
    "\r\n"
    "Invert X / Invert Y\r\n"
    "    Reverse a direction. A preference, not a fix - the pad's Y is already\r\n"
    "    corrected before this point.\r\n"
    "\r\n"
    "Smooth\r\n"
    "    Milliseconds of smoothing, applied ONLY while speed is rising. Takes\r\n"
    "    the edge off stick noise without adding lag when you stop.\r\n"
    "\r\n"
    "\r\n"
    "TURN ACCELERATION\r\n"
    "\r\n"
    "Everything above maps POSITION to speed: stick here, pointer moves that\r\n"
    "fast, always. This maps TIME HELD to speed. Hold the stick at the edge\r\n"
    "and the turn keeps building, so small corrections stay slow and precise\r\n"
    "while a 180 takes a beat and then whips round. No curve of any shape can\r\n"
    "do this, because it is a function of time, not of position.\r\n"
    "\r\n"
    "A hidden boost value builds while you hold past Threshold and bleeds away\r\n"
    "when you do not:\r\n"
    "\r\n"
    "    past Threshold:   boost = boost + Rate  per second\r\n"
    "    below it:         boost = boost - Decay per second\r\n"
    "    boost is clamped to 0 .. Max\r\n"
    "    speed = normal speed * (256 + boost) / 256\r\n"
    "\r\n"
    "Rate, Max and Decay are all in 256ths, where 256 means 1.0x.\r\n"
    "\r\n"
    "Rate\r\n"
    "    Boost gained per second held. 256 adds +1.0x of speed every second.\r\n"
    "    RATE 0 SWITCHES THE WHOLE BOX OFF - which is why the other three\r\n"
    "    grey out.\r\n"
    "\r\n"
    "Max\r\n"
    "    Ceiling on the boost. 512 is +2.0x, so 3.0x top speed. Time to reach\r\n"
    "    it is Max / Rate seconds, so 512 at Rate 256 takes 2 seconds.\r\n"
    "\r\n"
    "Decay\r\n"
    "    Boost lost per second while below Threshold. 1024 is 4.0x per second,\r\n"
    "    so a full 512 bleeds off in half a second.\r\n"
    "    BLEED IT OFF FAST. Slower than about half a second and your next\r\n"
    "    small correction inherits the last turn's boost, which feels broken\r\n"
    "    in a way that is very hard to diagnose.\r\n"
    "\r\n"
    "Threshold\r\n"
    "    Where boost starts building - and this one is on a 0-65535 scale, NOT\r\n"
    "    the 35000 scale Deadzone and Outer use. It is measured after the\r\n"
    "    deadzone is removed and what is left restretched:\r\n"
    "        0      = sitting exactly on the deadzone edge\r\n"
    "        65535  = at Outer, full deflection\r\n"
    "    The default 58000 is 88.5% of the way from the deadzone edge out to\r\n"
    "    Outer, and it MOVES when you change Deadzone or Outer, because those\r\n"
    "    two are its endpoints.\r\n"
    "\r\n"
    "Worth trying:   Rate 256   Max 512   Decay 1024   Threshold 58000\r\n"
    "Builds to 3x over two seconds near full deflection and drops back within\r\n"
    "half a second of easing off.\r\n"
    "\r\n"
    "Turn acceleration applies in mouse and wheel modes only.\r\n";

static WNDPROC g_HelpEditProc;

static LRESULT CALLBACK xb_help_edit_proc(HWND w, UINT msg, WPARAM wp,
                                          LPARAM lp)
{
	switch (msg) {
	case WM_KEYDOWN:
		if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
			/* 0 to -1 is the whole buffer. */
			SendMessageA(w, EM_SETSEL, 0, -1);
			return 0;
		}
		break;

	case WM_CHAR:
		if (wp == 1) {
			return 0;
		}
		break;

	default:
		break;
	}
	return CallWindowProcA(g_HelpEditProc, w, msg, wp, lp);
}

static INT_PTR CALLBACK xb_stick_help_proc(HWND dlg, UINT msg, WPARAM wp,
                                           LPARAM lp)
{
	static HFONT font;

	(void)lp;

	switch (msg) {
	case WM_INITDIALOG:
	{
		HWND edit = GetDlgItem(dlg, IDC_SH_TEXT);

		font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
			               ANSI_CHARSET, OUT_DEFAULT_PRECIS,
			               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			               FIXED_PITCH | FF_MODERN, "Consolas");
		if (font != NULL) {
			SendMessageA(edit, WM_SETFONT, (WPARAM)font, TRUE);
		}

		SetWindowTextA(edit, XB_STICK_HELP);

		g_HelpEditProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrA(
			    edit, GWLP_WNDPROC, (LONG_PTR)xb_help_edit_proc);

		SetFocus(edit);
		SendMessageA(edit, EM_SETSEL, 0, 0);
		return FALSE;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;

	case WM_DESTROY:
		if (g_HelpEditProc != NULL) {
			SetWindowLongPtrA(GetDlgItem(dlg, IDC_SH_TEXT),
			                  GWLP_WNDPROC,
			                  (LONG_PTR)g_HelpEditProc);
			g_HelpEditProc = NULL;
		}
		if (font != NULL) {
			DeleteObject(font);
			font = NULL;
		}
		break;

	default:
		break;
	}
	return FALSE;
}

static INT_PTR CALLBACK xb_stick_proc(HWND dlg, UINT msg, WPARAM wp,
                                     LPARAM lp)
{
	core_stick *st = &g_SP->cfg.stick[g_Which];


	switch (msg) {
	case WM_INITDIALOG:
	{
		char title[128];
		u32  i;
		int  pick;

		snprintf(title, sizeof(title), "%s stick",
			     g_Which == 0 ? "Left" : "Right");
		SetDlgItemTextA(dlg, IDC_ST_WHICH, title);
		SetWindowTextA(dlg, title);

		for (i = 0; XB_STICK_MODES[i].name != NULL; i++) {
			SendDlgItemMessageA(dlg, IDC_ST_MODE, CB_ADDSTRING, 0,
				                (LPARAM)XB_STICK_MODES[i].name);
			if (XB_STICK_MODES[i].value == st->mode) {
				SendDlgItemMessageA(dlg, IDC_ST_MODE, CB_SETCURSEL,
					                (WPARAM)i, 0);
			}
		}

		for (i = 0; XB_CURVES[i].name != NULL; i++) {
			SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_ADDSTRING, 0,
				                (LPARAM)XB_CURVES[i].name);
		}
		/*
		 * A CURVE THAT IS NOT A PRESET IS LEFT ALONE. A profile may
		 * carry two arbitrary control points, and picking the nearest
		 * preset for the list would quietly replace it on OK.
		 */
		pick = xb_curve_index(st->curve);
		if (pick >= 0) {
			SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_SETCURSEL,
				                (WPARAM)pick, 0);
		} else {
			/* "(custom)" lands after the presets, so its index is
			 * the preset count, and OK leaves the table alone. */
			SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_ADDSTRING, 0,
				                (LPARAM)"(custom)");
			SendDlgItemMessageA(dlg, IDC_ST_CURVE, CB_SETCURSEL,
				                (WPARAM)xb_curve_count(), 0);
		}

		xb_set_num(dlg, IDC_ST_DEADZONE, st->deadzone);
		xb_set_num(dlg, IDC_ST_OUTER,    st->outer);
		xb_set_num(dlg, IDC_ST_MAXSPEED, st->max_speed);
		xb_set_num(dlg, IDC_ST_GAINX,    st->gain_x);
		xb_set_num(dlg, IDC_ST_GAINY,    st->gain_y);
		xb_set_num(dlg, IDC_ST_SMOOTH,   st->smooth_ms);
		xb_set_num(dlg, IDC_ST_ATHRESH,  st->accel_threshold);
		xb_set_num(dlg, IDC_ST_ARATE,    st->accel_rate);
		xb_set_num(dlg, IDC_ST_AMAX,     st->accel_max);
		xb_set_num(dlg, IDC_ST_ADECAY,   st->accel_decay);
		CheckDlgButton(dlg, IDC_ST_INVX,
			           st->invert_x ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(dlg, IDC_ST_INVY,
			           st->invert_y ? BST_CHECKED : BST_UNCHECKED);

		xb_stick_units(dlg);

		g_StickBefore  = g_SP->cfg;
		g_StickPushed  = 0;
		return TRUE;
	}

	case WM_HELP:
		DialogBoxParamA(xb_instance(),
		                MAKEINTRESOURCEA(IDD_STICKHELP), dlg,
		                xb_stick_help_proc, 0);
		return TRUE;

	case WM_DRAWITEM:
		if (wp == IDC_ST_GRAPH) {
			xb_graph_paint(dlg, (const DRAWITEMSTRUCT *)lp);
			return TRUE;
		}
		break;

	case WM_COMMAND:
		if (LOWORD(wp) == IDC_ST_MODE && HIWORD(wp) == CBN_SELCHANGE) {
			xb_stick_units(dlg);
			return TRUE;
		}
		/* THE LINE FOLLOWS THE TYPING. Every field the shape
		 * depends on redraws it. */
		if (LOWORD(wp) == IDC_ST_CURVE &&
		    HIWORD(wp) == CBN_SELCHANGE) {
			xb_graph_update(dlg);
			return TRUE;
		}
		if (HIWORD(wp) == EN_CHANGE &&
		    (LOWORD(wp) == IDC_ST_DEADZONE ||
		     LOWORD(wp) == IDC_ST_OUTER ||
		     LOWORD(wp) == IDC_ST_MAXSPEED)) {
			xb_graph_update(dlg);
			return TRUE;
		}
		if (LOWORD(wp) == IDC_ST_TEST) {
			static core_config trial;
			char               why[512];

			trial = g_SP->cfg;
			xb_stick_read(dlg, &trial.stick[g_Which]);

			if (xb_driver_push(&trial, 0, why, sizeof(why))) {
				g_StickPushed = 1;
				SetDlgItemTextA(dlg, IDC_ST_HINT,
				    "On the pad now, and saved nowhere. Cancel puts"
				    " the pad back to what it had.");
			} else {
				SetDlgItemTextA(dlg, IDC_ST_HINT, why);
			}
			return TRUE;
		}
		if (LOWORD(wp) == IDC_ST_OFF) {
			u32 i;
			for (i = 0; XB_STICK_MODES[i].name != NULL; i++) {
				if (XB_STICK_MODES[i].value == CORE_STICK_OFF) {
					SendDlgItemMessageA(dlg, IDC_ST_MODE,
					                    CB_SETCURSEL, (WPARAM)i, 0);
					break;
				}
			}
			xb_stick_units(dlg);
			return TRUE;
		}
		if (LOWORD(wp) == IDC_ST_DEFAULT) {
			xb_stick_defaults(dlg);
			return TRUE;
		}
		if (LOWORD(wp) == IDOK) {
			xb_stick_read(dlg, st);

			/*
			 * ONLY IF TEST ALREADY WORKED. The pad is holding a trial
			 * and has to be brought in line with what was accepted -
			 * but a dialog that cannot be closed with OK because no pad
			 * is plugged in would be worse than the mismatch. Having
			 * pushed once, a failure now is worth stopping for.
			 */
			if (g_StickPushed) {
				char why[512];

				if (!xb_driver_push(&g_SP->cfg, 0, why,
				                    sizeof(why))) {
					SetDlgItemTextA(dlg, IDC_ST_HINT, why);
					return TRUE;
				}
			}

			EndDialog(dlg, 1);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) {
			if (g_StickPushed) {
				char why[512];

				(void)xb_driver_push(&g_StickBefore, 0, why,
				                     sizeof(why));
			}
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

int xb_stick_dialog(HWND parent, xb_profile *p, u32 which)
{
	g_SP    = p;
	g_Which = which;
	return (int)DialogBoxParamA(xb_instance(),
	                            MAKEINTRESOURCEA(IDD_STICK), parent,
	                            xb_stick_proc, 0);
}
