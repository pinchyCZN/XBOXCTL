/*
 * bind.c - the dialog behind every pad control button.
 *
 * IT SHOWS THE LINE IT IS ABOUT TO WRITE. The preview at the bottom is
 * built by the same xb_binding_text the profile writer uses, so what
 * the dialog promises and what lands in the .txt cannot disagree.
 *
 * CAPTURE READS A HID USAGE, NOT A CHARACTER. The driver sends usages,
 * so pressing Z on a QWERTZ keyboard has to record the usage that key
 * carries rather than the letter it produces here.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbconfig.h"
#include "resource.h"

extern HINSTANCE xb_instance(void);

static xb_profile *g_P;
static u32         g_Layer;
static u8          g_Source;

/*
 * THE LAYER ACTIONS ARE NOT OFFERED HERE. A control that changes layer
 * has to be written into every layer at once or it leaves a layer with
 * no way out, so the two layer cells on the main dialog own that and
 * this list has neither. It also spares every control three actions that
 * most of the flags below do not apply to.
 *
 * AN EXISTING ONE IS STILL SHOWN, because a profile written by hand or
 * by an older build may have put a layer action on a control, and a list
 * that could not represent it would quietly destroy it on OK.
 */
int xb_action_is_layer(u8 action)
{
	return action == CORE_ACT_LAYER_HOLD ||
	       action == CORE_ACT_LAYER_SET ||
	       action == CORE_ACT_LAYER_CYCLE;
}

/*
 * The action a list row stands for.
 *
 * READ FROM THE ROW'S ITEM DATA, not from its index. The list is
 * filtered, so a row number is no longer an index into XB_ACTIONS.
 */
static u8 xb_list_action(HWND dlg)
{
	int sel = (int)SendDlgItemMessageA(dlg, IDC_ACTION, LB_GETCURSEL,
	                                   0, 0);

	if (sel == LB_ERR) {
		return CORE_ACT_NONE;
	}
	return (u8)SendDlgItemMessageA(dlg, IDC_ACTION, LB_GETITEMDATA,
	                               (WPARAM)sel, 0);
}

/* ======================================================================
 * CAPTURE
 * ====================================================================== */

/*
 * SCAN CODE TO HID USAGE, not virtual key to HID usage.
 *
 * A VIRTUAL KEY CANNOT TELL THESE APART and the pad has to: keypad
 * Enter from the Enter above it, keypad 4 from the 4 on the number row,
 * right Alt from left. Windows folds all of those into one VK. The scan
 * code does not, and the E0 prefix is exactly what separates them.
 *
 * These are PS/2 set 1 codes, which is what RAWKEYBOARD.MakeCode
 * carries whatever the keyboard is physically wired as.
 *
 * A KEY WITH NO USAGE RETURNS 0 AND IS IGNORED. Media and browser keys
 * are real keys with nothing to send, and binding one to a usage it
 * does not have would be inventing a keystroke.
 */
static u16 xb_scan_to_usage(u16 make, int e0, int e1)
{
	/* Pause arrives as E1 1D 45 and is the only E1 sequence there is. */
	static const u16 PLAIN[] = {
	/* 00 */ 0,    0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
	/* 08 */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
	/* 10 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C,
	/* 18 */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
	/* 20 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33,
	/* 28 */ 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
	/* 30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55,
	/* 38 */ 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
	/* 40 */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F,
	/* 48 */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
	/* 50 */ 0x5A, 0x5B, 0x62, 0x63, 0,    0,    0x64, 0x44,
	/* 58 */ 0x45
	};
	static const struct { u16 make; u16 usage; } EXTENDED[] = {
		{ 0x1C, 0x58 },     /* keypad Enter    */
		{ 0x1D, 0xE4 },     /* right Ctrl      */
		{ 0x35, 0x54 },     /* keypad /        */
		{ 0x37, 0x46 },     /* Print Screen    */
		{ 0x38, 0xE6 },     /* right Alt       */
		{ 0x47, 0x4A },     /* Home            */
		{ 0x48, 0x52 },     /* Up              */
		{ 0x49, 0x4B },     /* Page Up         */
		{ 0x4B, 0x50 },     /* Left            */
		{ 0x4D, 0x4F },     /* Right           */
		{ 0x4F, 0x4D },     /* End             */
		{ 0x50, 0x51 },     /* Down            */
		{ 0x51, 0x4E },     /* Page Down       */
		{ 0x52, 0x49 },     /* Insert          */
		{ 0x53, 0x4C },     /* Delete          */
		{ 0x5B, 0xE3 },     /* left Windows    */
		{ 0x5C, 0xE7 },     /* right Windows   */
		{ 0x5D, 0x65 },     /* Application     */
		{ 0, 0 }
	};
	u32 i;

	if (e1) {
		return (make == 0x1D || make == 0x45) ? (u16)0x48 : (u16)0;
	}
	if (e0) {
		for (i = 0; EXTENDED[i].make != 0; i++) {
			if (EXTENDED[i].make == make) {
				return EXTENDED[i].usage;
			}
		}
		return 0;
	}
	if (make < sizeof(PLAIN) / sizeof(PLAIN[0])) {
		return PLAIN[make];
	}
	return 0;
}

static u16 g_Captured;

/* Defined below, with the rest of the dialog entry points. */
int xb_capture_key(HWND parent, u16 *usage);

static void xb_capture_listen(HWND dlg, int on)
{
	RAWINPUTDEVICE rid;

	rid.usUsagePage = 0x01;     /* generic desktop */
	rid.usUsage     = 0x06;     /* keyboard        */
	rid.dwFlags     = on ? 0 : RIDEV_REMOVE;
	rid.hwndTarget  = on ? dlg : NULL;
	RegisterRawInputDevices(&rid, 1, (UINT)sizeof(rid));
}

static INT_PTR CALLBACK xb_capture_proc(HWND dlg, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG:
		g_Captured = 0;
		xb_capture_listen(dlg, 1);

		/*
		 * NO CONTROL GETS THE FOCUS. With the Cancel button focused,
		 * Enter activates it and the keystroke never arrives - which
		 * is the whole reason Enter could not be captured. Focused on
		 * the dialog itself, Enter reaches nothing and Escape still
		 * cancels, because the dialog manager maps it whatever has
		 * focus.
		 */
		SetFocus(dlg);
		return FALSE;       /* FALSE: the focus is already set */

	case WM_INPUT:
	{
		RAWINPUT ri;
		UINT     size = (UINT)sizeof(ri);
		u16      usage;

		if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
			                (UINT)sizeof(RAWINPUTHEADER)) == (UINT)-1) {
			break;
		}
		if (ri.header.dwType != RIM_TYPEKEYBOARD) {
			break;
		}
		/*
		 * MAKES ONLY, WHICH ALSO SOLVES THE SELF-CAPTURE RACE. The
		 * key that opened this dialog was pressed before the
		 * registration, so only its BREAK arrives here - and a break
		 * is not a capture.
		 */
		if (ri.data.keyboard.Flags & RI_KEY_BREAK) {
			break;
		}

		usage = xb_scan_to_usage(ri.data.keyboard.MakeCode,
			        (ri.data.keyboard.Flags & RI_KEY_E0) != 0,
			        (ri.data.keyboard.Flags & RI_KEY_E1) != 0);

		/* A key with no usage is ignored, and so is Escape - which
		 * cancels instead, and can be typed in by name. */
		if (usage == 0 || usage == 0x29) {
			break;
		}

		g_Captured = usage;
		xb_capture_listen(dlg, 0);
		EndDialog(dlg, 1);
		return TRUE;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDCANCEL) {
			xb_capture_listen(dlg, 0);
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;

	case WM_DESTROY:
		xb_capture_listen(dlg, 0);
		break;
	}
	return FALSE;
}

/* ======================================================================
 * THE BINDING DIALOG
 * ====================================================================== */

/* The slot this source occupies in the layer, or -1. */
static int xb_find_binding(const xb_profile *p, u32 layer, u8 source)
{
	u32 i;

	for (i = 0; i < CORE_MAX_BINDINGS; i++) {
		const core_binding *b = &p->cfg.layout[layer].binding[i];

		if (b->action != CORE_ACT_NONE && b->source == source) {
			return (int)i;
		}
	}
	return -1;
}

/* Read the dialog into a binding. */
static void xb_dialog_to_binding(HWND dlg, core_binding *b)
{
	char text[128];
	u32  v;

	memset(b, 0, sizeof(*b));
	b->source = g_Source;

	b->action = xb_list_action(dlg);
	if (b->action == CORE_ACT_NONE) {
		return;
	}

	GetDlgItemTextA(dlg, IDC_CODE, text, sizeof(text));
	if (b->action == CORE_ACT_KEY) {
		if (xb_name_to_value(xb_keys(), text, &v)) {
			b->code = (u16)v;
		} else {
			b->code = (u16)strtol(text, NULL, 0);
		}
	} else if (b->action == CORE_ACT_MOUSE_BUTTON &&
	           xb_name_to_value(XB_MOUSE_BUTTONS, text, &v)) {
		b->code = (u16)v;
	} else if (b->action == CORE_ACT_LAYER_HOLD ||
	           b->action == CORE_ACT_LAYER_SET) {
		long n = strtol(text, NULL, 0);

		if (n < 1) { n = 1; }
		if (n > CORE_MAX_LAYOUTS) { n = CORE_MAX_LAYOUTS; }
		b->code = (u16)(n - 1);     /* written 1-based, stored 0-based */
	} else {
		b->code = (u16)strtol(text, NULL, 0);
	}

	if (IsDlgButtonChecked(dlg, IDC_REPEAT) == BST_CHECKED) {
		b->flags |= CORE_BF_REPEAT;
	}
	if (IsDlgButtonChecked(dlg, IDC_TOGGLE) == BST_CHECKED) {
		b->flags |= CORE_BF_TOGGLE;
	}
	if (IsDlgButtonChecked(dlg, IDC_ANALOG) == BST_CHECKED) {
		b->flags |= CORE_BF_ANALOG;
	}
	if (IsDlgButtonChecked(dlg, IDC_PASSTHROUGH) == BST_CHECKED) {
		b->flags |= CORE_BF_PASSTHROUGH;
	}

	if (b->flags & CORE_BF_REPEAT) {
		GetDlgItemTextA(dlg, IDC_HZ, text, sizeof(text));
		b->repeat_hz = (u8)strtol(text, NULL, 10);
		if (b->repeat_hz == 0) {
			b->repeat_hz = 12;
		}
		if (b->repeat_hz > CORE_MAX_REPEAT_HZ) {
			b->repeat_hz = (u8)CORE_MAX_REPEAT_HZ;
		}

		GetDlgItemTextA(dlg, IDC_HARD, text, sizeof(text));
		if (text[0] != 0) {
			long pct = strtol(text, NULL, 10);

			if (pct < 0)   { pct = 0; }
			if (pct > 100) { pct = 100; }
			b->hard_at = (u16)((u32)CORE_MAX_VALUE * (u32)pct / 100u);
		}
	}

	GetDlgItemTextA(dlg, IDC_DELAY, text, sizeof(text));
	b->repeat_delay_ms = (u16)strtol(text, NULL, 10);
}

static void xb_update_preview(HWND dlg)
{
	core_binding b;
	char         text[256];
	u8           action;
	int          is_key;
	int          repeating;

	xb_dialog_to_binding(dlg, &b);
	xb_binding_text(&g_P->cfg, g_Layer, &b, text, sizeof(text));
	SetDlgItemTextA(dlg, IDC_PREVIEW,
	                text[0] != 0 ? text : "(nothing - this control keeps"
	                                      " its normal pad behaviour)");

	action = xb_list_action(dlg);
	is_key = (action == CORE_ACT_KEY);
	repeating = (IsDlgButtonChecked(dlg, IDC_REPEAT) == BST_CHECKED);

	/* Only a key can be captured, and only a repeat has a rate or a
	 * pressure to reach before it starts. */
	EnableWindow(GetDlgItem(dlg, IDC_CAPTURE), is_key ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_HZ), repeating ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_HARD), repeating ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_CODE),
	             action != CORE_ACT_NONE ? TRUE : FALSE);
}

static INT_PTR CALLBACK xb_bind_proc(HWND dlg, UINT msg, WPARAM wp,
                                     LPARAM lp)
{
	(void)lp;

	switch (msg) {
	case WM_INITDIALOG:
	{
		HWND list = GetDlgItem(dlg, IDC_ACTION);
		char title[128];
		int  slot;
		u32  i;

		snprintf(title, sizeof(title), "%s, layer %u",
			     xb_source_label(g_Source), (unsigned)(g_Layer + 1));
		SetDlgItemTextA(dlg, IDC_CONTROL, title);

		slot = xb_find_binding(g_P, g_Layer, g_Source);

		for (i = 0; XB_ACTIONS[i].name != NULL; i++) {
			u8  act = (u8)XB_ACTIONS[i].value;
			int at;

			if (xb_action_is_layer(act) &&
				(slot < 0 ||
				 g_P->cfg.layout[g_Layer].binding[slot].action
				 != act)) {
				continue;
			}
			at = (int)SendMessageA(list, LB_ADDSTRING, 0,
				                   (LPARAM)XB_ACTIONS[i].name);
			SendMessageA(list, LB_SETITEMDATA, (WPARAM)at,
				         (LPARAM)act);
		}
		if (slot < 0) {
			SendMessageA(list, LB_SETCURSEL, 0, 0);
			SetDlgItemTextA(dlg, IDC_HZ, "12");
		} else {
			const core_binding *b =
				&g_P->cfg.layout[g_Layer].binding[slot];
			char text[64];

			{
				int rows = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
				int r;

				for (r = 0; r < rows; r++) {
					if ((u8)SendMessageA(list, LB_GETITEMDATA,
						                 (WPARAM)r, 0) == b->action) {
						SendMessageA(list, LB_SETCURSEL, (WPARAM)r, 0);
						break;
					}
				}
			}

			if (b->action == CORE_ACT_KEY) {
				const char *k = xb_value_to_name(xb_keys(), b->code);

				snprintf(text, sizeof(text), "%s",
					     k != NULL ? k : "");
				if (k == NULL) {
					snprintf(text, sizeof(text), "0x%02X",
						     (unsigned)b->code);
				}
			} else if (b->action == CORE_ACT_LAYER_HOLD ||
				       b->action == CORE_ACT_LAYER_SET) {
				snprintf(text, sizeof(text), "%u",
					     (unsigned)(b->code + 1));
			} else {
				snprintf(text, sizeof(text), "%d", (int)(s16)b->code);
			}
			SetDlgItemTextA(dlg, IDC_CODE, text);

			CheckDlgButton(dlg, IDC_REPEAT,
				           (b->flags & CORE_BF_REPEAT) ? BST_CHECKED
				                                       : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_TOGGLE,
				           (b->flags & CORE_BF_TOGGLE) ? BST_CHECKED
				                                       : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_ANALOG,
				           (b->flags & CORE_BF_ANALOG) ? BST_CHECKED
				                                       : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_PASSTHROUGH,
				           (b->flags & CORE_BF_PASSTHROUGH)
				           ? BST_CHECKED : BST_UNCHECKED);

			snprintf(text, sizeof(text), "%u",
				     (unsigned)(b->repeat_hz != 0 ? b->repeat_hz : 12));
			SetDlgItemTextA(dlg, IDC_HZ, text);
			if (b->repeat_delay_ms != 0) {
				snprintf(text, sizeof(text), "%u",
					     (unsigned)b->repeat_delay_ms);
				SetDlgItemTextA(dlg, IDC_DELAY, text);
			}
			if (b->hard_at != 0) {
				snprintf(text, sizeof(text), "%u",
					     (unsigned)(((u32)b->hard_at * 100u +
					                 CORE_MAX_VALUE / 2) /
					                CORE_MAX_VALUE));
				SetDlgItemTextA(dlg, IDC_HARD, text);
			}
		}
		xb_update_preview(dlg);
		return TRUE;
	}

	case WM_COMMAND:
	{
		int id   = LOWORD(wp);
		int code = HIWORD(wp);

		if ((id == IDC_ACTION && code == LBN_SELCHANGE) ||
			(id == IDC_CODE && code == EN_CHANGE) ||
			(id == IDC_HZ && code == EN_CHANGE) ||
			(id == IDC_DELAY && code == EN_CHANGE) ||
			(id == IDC_HARD && code == EN_CHANGE) ||
			id == IDC_REPEAT || id == IDC_TOGGLE ||
			id == IDC_ANALOG || id == IDC_PASSTHROUGH) {
			xb_update_preview(dlg);
			if (id != IDC_ACTION && code == LBN_SELCHANGE) {
				return TRUE;
			}
		}

		switch (id) {
		case IDC_CAPTURE:
		{
			u16 usage = 0;

			/* SHARED WITH THE CHORD DIALOG, so both get the same key
			 * table and the same tidy-up afterwards. */
			if (xb_capture_key(dlg, &usage)) {
				const char *k = xb_value_to_name(xb_keys(), usage);
				char        text[32];

				if (k != NULL) {
					snprintf(text, sizeof(text), "%s", k);
				} else {
					snprintf(text, sizeof(text), "0x%02X",
							 (unsigned)usage);
				}
				SetDlgItemTextA(dlg, IDC_CODE, text);
				xb_update_preview(dlg);
			}
			return TRUE;
		}

		case IDC_CLEAR:
			SendDlgItemMessageA(dlg, IDC_ACTION, LB_SETCURSEL, 0, 0);
			SetDlgItemTextA(dlg, IDC_CODE, "");
			CheckDlgButton(dlg, IDC_REPEAT, BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_TOGGLE, BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_ANALOG, BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_PASSTHROUGH, BST_UNCHECKED);
			SetDlgItemTextA(dlg, IDC_DELAY, "");
			SetDlgItemTextA(dlg, IDC_HARD, "");
			xb_update_preview(dlg);
			return TRUE;

		case IDOK:
		{
			core_binding b;
			int          slot;

			if (GetFocus() == GetDlgItem(dlg, IDC_ACTION) &&
				xb_list_action(dlg) == CORE_ACT_KEY) {
				SendMessageA(dlg, WM_NEXTDLGCTL,
						     (WPARAM)GetDlgItem(dlg,
						                        IDC_CAPTURE),
						     TRUE);
				return TRUE;
			}

			xb_dialog_to_binding(dlg, &b);
			slot = xb_find_binding(g_P, g_Layer, g_Source);

			if (b.action == CORE_ACT_NONE) {
				if (slot >= 0) {
					core_binding *t =
						&g_P->cfg.layout[g_Layer]
						 .binding[slot];

					memset(t, 0, sizeof(*t));
					t->source = CORE_SA_NONE;
					t->action = CORE_ACT_NONE;
				}
			} else if (slot >= 0) {
				g_P->cfg.layout[g_Layer].binding[slot] = b;
			} else {
				u32 i;

			/*
			 * ENTER ON THE ACTION LIST MOVES TO CAPTURE, it does not
			 * accept the dialog. Picking "key" and pressing Enter reads
			 * as "yes, that one", and closing there would leave the value
			 * box empty - the one thing the choice needs next.
			 *
			 * WM_NEXTDLGCTL, NOT SetFocus. Which button Enter presses is
			 * the dialog's DEFAULT, which is a separate thing from what
			 * has the focus: SetFocus moves the caret and leaves OK still
			 * the default, so Enter would close the dialog anyway. Only
			 * the dialog manager moves both, and this is how it is asked.
			 */
			if (GetFocus() == GetDlgItem(dlg, IDC_ACTION) &&
				xb_list_action(dlg) == CORE_ACT_KEY) {
				SendMessageA(dlg, WM_NEXTDLGCTL,
							 (WPARAM)GetDlgItem(dlg, IDC_CAPTURE),
							 TRUE);
				return TRUE;
			}

				for (i = 0; i < CORE_MAX_BINDINGS; i++) {
					core_binding *t =
						&g_P->cfg.layout[g_Layer].binding[i];

					if (t->action == CORE_ACT_NONE) {
						*t = b;
						break;
					}
				}
				if (i == CORE_MAX_BINDINGS) {
					MessageBoxA(dlg, "This layer is full - it already "
							         "has the most bindings the driver "
							         "accepts.",
							    "XBOXCTL", MB_OK | MB_ICONWARNING);
					return TRUE;
				}
			}

			/* THE SUPPRESSION MASK IS DERIVED, NEVER EDITED. It says
			 * which controls a binding has taken over from the default
			 * map, and it has to be rebuilt whenever one changes. */
			core_config_suppress(&g_P->cfg);
			EndDialog(dlg, 1);
			return TRUE;
		}

		case IDCANCEL:
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

/*
 * Ask for a key and give back its HID usage. Shared with the chord
 * dialog, which needs the same thing and should not carry a second
 * virtual-key table to get it.
 */
int xb_capture_key(HWND parent, u16 *usage)
{
	INT_PTR took;
	MSG     msg;

	took = DialogBoxParamA(xb_instance(), MAKEINTRESOURCEA(IDD_CAPTURE),
	                       parent, xb_capture_proc, 0);

	/*
	 * THROW AWAY WHAT IS STILL IN FLIGHT. The dialog closes on the key
	 * going DOWN, so its release - and any auto-repeat while a finger
	 * stays on it - is still queued. Capture Enter and those repeats
	 * would land on the OK button below and close this dialog too, from
	 * one keypress.
	 */
	while (PeekMessageA(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {
		/* discarded on purpose */
	}

	/*
	 * AND THEN ENTER MEANS OK. Leaving the focus on the Capture button
	 * makes Enter reopen capture, which is the last thing anybody wants
	 * having just finished with it.
	 *
	 * WM_NEXTDLGCTL AGAIN, for the same reason: it moves the default
	 * button as well as the focus. SetFocus happens to work here only
	 * because OK is already the default, which is a coincidence and not
	 * a reason.
	 */
	SendMessageA(parent, WM_NEXTDLGCTL,
	             (WPARAM)GetDlgItem(parent, IDOK), TRUE);

	if (!took) {
		return 0;
	}
	*usage = g_Captured;
	return 1;
}

int xb_bind_dialog(HWND parent, xb_profile *p, u32 layer, u8 source)
{
	g_P      = p;
	g_Layer  = layer;
	g_Source = source;
	return (int)DialogBoxParamA(xb_instance(),
	                            MAKEINTRESOURCEA(IDD_BIND), parent,
	                            xb_bind_proc, 0);
}
