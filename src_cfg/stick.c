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

static INT_PTR CALLBACK xb_stick_proc(HWND dlg, UINT msg, WPARAM wp,
                                     LPARAM lp)
{
	core_stick *st = &g_SP->cfg.stick[g_Which];

	(void)lp;

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
		return TRUE;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDC_ST_MODE && HIWORD(wp) == CBN_SELCHANGE) {
			xb_stick_units(dlg);
			return TRUE;
		}
		if (LOWORD(wp) == IDOK) {
			int sel;

			sel = (int)SendDlgItemMessageA(dlg, IDC_ST_MODE,
			                               CB_GETCURSEL, 0, 0);
			if (sel != CB_ERR) {
				st->mode = (u8)XB_STICK_MODES[sel].value;
			}

			st->deadzone  = (u16)xb_get_num(dlg, IDC_ST_DEADZONE,
			                                CORE_MAX_VALUE);
			st->outer     = (u16)xb_get_num(dlg, IDC_ST_OUTER,
			                                CORE_MAX_VALUE);
			st->max_speed = (u16)xb_get_num(dlg, IDC_ST_MAXSPEED,
			                                65535);
			st->gain_x    = (u8)xb_get_num(dlg, IDC_ST_GAINX, 255);
			st->gain_y    = (u8)xb_get_num(dlg, IDC_ST_GAINY, 255);
			st->smooth_ms = (u8)xb_get_num(dlg, IDC_ST_SMOOTH, 255);
			st->accel_threshold =
			    (u16)xb_get_num(dlg, IDC_ST_ATHRESH, 65535);
			st->accel_rate  = (u16)xb_get_num(dlg, IDC_ST_ARATE, 65535);
			st->accel_max   = (u16)xb_get_num(dlg, IDC_ST_AMAX, 65535);
			st->accel_decay = (u16)xb_get_num(dlg, IDC_ST_ADECAY, 65535);
			st->invert_x = (u8)(IsDlgButtonChecked(dlg, IDC_ST_INVX)
			                    == BST_CHECKED ? 1 : 0);
			st->invert_y = (u8)(IsDlgButtonChecked(dlg, IDC_ST_INVY)
			                    == BST_CHECKED ? 1 : 0);

			/*
			 * OUTER ABOVE DEADZONE, or the rescale divides by zero.
			 * The driver repairs this; repairing it here means the
			 * profile on disk says what the pad will actually do.
			 */
			if (st->outer <= st->deadzone) {
				st->outer = (u16)(st->deadzone + 1);
			}

			sel = (int)SendDlgItemMessageA(dlg, IDC_ST_CURVE,
			                               CB_GETCURSEL, 0, 0);
			if (sel != CB_ERR && sel < (int)xb_curve_count()) {
				xb_curve_build(XB_CURVES[sel].y1, XB_CURVES[sel].y2,
				               st->curve);
			}

			EndDialog(dlg, 1);
			return TRUE;
		}
		if (LOWORD(wp) == IDCANCEL) {
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
