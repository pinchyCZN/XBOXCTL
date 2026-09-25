/*
 * chord.c - the two chord buttons.
 *
 * A COPY OF THE BINDING DIALOG, NOT THE BINDING DIALOG WITH THINGS
 * HIDDEN. Three of that dialog's controls are provable no-ops on a
 * chord, because a chord's value is BINARY - zero, or full scale when
 * every member is down:
 *
 *   analog       passes the magnitude instead of thresholding it, and
 *                the magnitude IS full scale. Same output either way.
 *   hard         gates autofire on pressure. The value is already the
 *                maximum, so the gate can never hold.
 *   passthrough  gives a control back its default output. A chord
 *                source never enters the suppression mask to begin
 *                with, so there is nothing to give back.
 *
 * A greyed-out control still asks to be understood. These are simply
 * absent.
 *
 * THE SLOT IS FIXED, NOT SEARCHED FOR. Chord 1 is slot 0 and Chord 2 is
 * slot 1, in whichever layer the main dialog is showing, which is what
 * lets a button on a dialog point at one and what the profile section
 * [layer 1 chord 1] names.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbconfig.h"
#include "resource.h"

extern HINSTANCE xb_instance(void);
extern int       xb_capture_key(HWND parent, u16 *usage);
extern int       xb_action_is_layer(u8 action);

static xb_profile *g_CP;
static u32         g_CLayer;
static u32         g_CSlot;

/* Every control, in the order the layer dialog lists them. */
extern const u8 *xb_choice_order(u32 *count);

static void xb_ch_fill(HWND dlg, int id, u8 selected)
{
	u32        n = 0;
	const u8  *order = xb_choice_order(&n);
	u32        i;
	int        pick = 0;

	SendDlgItemMessageA(dlg, id, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(dlg, id, CB_ADDSTRING, 0, (LPARAM)"(none)");
	for (i = 0; i < n; i++) {
		int at = (int)SendDlgItemMessageA(dlg, id, CB_ADDSTRING, 0,
		             (LPARAM)xb_source_label(order[i]));

		SendDlgItemMessageA(dlg, id, CB_SETITEMDATA, (WPARAM)at,
		                    (LPARAM)order[i]);
		if (order[i] == selected) {
			pick = at;
		}
	}
	SendDlgItemMessageA(dlg, id, CB_SETCURSEL, (WPARAM)pick, 0);
}

static u8 xb_ch_source(HWND dlg, int id)
{
	int sel = (int)SendDlgItemMessageA(dlg, id, CB_GETCURSEL, 0, 0);

	/* Item 0 is "(none)", and the INDEX says so - D-pad up is source
	 * 0, so the item data cannot be tested for zero. */
	if (sel == CB_ERR || sel == 0) {
		return CORE_SA_NONE;
	}
	return (u8)SendDlgItemMessageA(dlg, id, CB_GETITEMDATA,
	                               (WPARAM)sel, 0);
}

static u8 xb_ch_action(HWND dlg)
{
	int sel = (int)SendDlgItemMessageA(dlg, IDC_CH_ACTION, LB_GETCURSEL,
	                                   0, 0);

	if (sel == LB_ERR) {
		return CORE_ACT_NONE;
	}
	return (u8)SendDlgItemMessageA(dlg, IDC_CH_ACTION, LB_GETITEMDATA,
	                               (WPARAM)sel, 0);
}

/* The members as the dialog has them, collapsed and packed. */
static u32 xb_ch_members(HWND dlg, u8 out[CORE_CHORD_MEMBERS])
{
	static const int ID[CORE_CHORD_MEMBERS] = {
		IDC_CH_M1, IDC_CH_M2, IDC_CH_M3
	};
	u32 n = 0;
	u32 i;
	u32 k;

	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		out[i] = CORE_SA_NONE;
	}
	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		u8  src = xb_ch_source(dlg, ID[i]);
		int seen = 0;

		if (src == CORE_SA_NONE) {
			continue;
		}
		/* THE SAME CONTROL TWICE IS ONE CONTROL. core_member_down
		 * would otherwise want it pressed twice over. */
		for (k = 0; k < n; k++) {
			if (out[k] == src) {
				seen = 1;
			}
		}
		if (!seen) {
			out[n++] = src;
		}
	}
	return n;
}

static void xb_ch_binding(HWND dlg, core_binding *b)
{
	char text[128];
	u32  v;

	memset(b, 0, sizeof(*b));
	b->source = (u8)(CORE_SA_CHORD_BASE + g_CSlot);
	b->action = xb_ch_action(dlg);
	if (b->action == CORE_ACT_NONE) {
		return;
	}

	GetDlgItemTextA(dlg, IDC_CH_CODE, text, sizeof(text));
	if (b->action == CORE_ACT_KEY) {
		if (xb_name_to_value(xb_keys(), text, &v)) {
			b->code = (u16)v;
		} else {
			b->code = (u16)strtol(text, NULL, 0);
		}
	} else if (b->action == CORE_ACT_MOUSE_BUTTON &&
	           xb_name_to_value(XB_MOUSE_BUTTONS, text, &v)) {
		b->code = (u16)v;
	} else {
		b->code = (u16)strtol(text, NULL, 0);
	}

	if (IsDlgButtonChecked(dlg, IDC_CH_TOGGLE) == BST_CHECKED) {
		b->flags |= CORE_BF_TOGGLE;
	}
	if (IsDlgButtonChecked(dlg, IDC_CH_REPEAT) == BST_CHECKED) {
		b->flags |= CORE_BF_REPEAT;
		GetDlgItemTextA(dlg, IDC_CH_HZ, text, sizeof(text));
		b->repeat_hz = (u8)strtol(text, NULL, 10);
		if (b->repeat_hz == 0) {
			b->repeat_hz = 12;
		}
		if (b->repeat_hz > CORE_MAX_REPEAT_HZ) {
			b->repeat_hz = (u8)CORE_MAX_REPEAT_HZ;
		}
	}
	GetDlgItemTextA(dlg, IDC_CH_DELAY, text, sizeof(text));
	b->repeat_delay_ms = (u16)strtol(text, NULL, 10);
}

static void xb_ch_preview(HWND dlg)
{
	u8           members[CORE_CHORD_MEMBERS];
	u32          n = xb_ch_members(dlg, members);
	core_binding b;
	char         who[96];
	char         act[192];
	char         line[320];
	u32          i;

	xb_ch_binding(dlg, &b);

	if (n < 2) {
		SetDlgItemTextA(dlg, IDC_CH_PREVIEW,
		                "A chord needs two controls or more. With one,"
		                " bind it on the pad instead.");
		return;
	}
	if (b.action == CORE_ACT_NONE) {
		SetDlgItemTextA(dlg, IDC_CH_PREVIEW,
		                "No action - the chord will do nothing.");
		return;
	}

	who[0] = 0;
	for (i = 0; i < n; i++) {
		const char *s = xb_value_to_name(XB_SOURCES, members[i]);

		if (who[0] != 0) {
			strcat(who, "+");
		}
		strcat(who, s != NULL ? s : "?");
	}
	xb_action_text(&b, act, sizeof(act));
	snprintf(line, sizeof(line), "members = %s     action = %s",
	         who, act);
	SetDlgItemTextA(dlg, IDC_CH_PREVIEW, line);
}

static void xb_ch_enable(HWND dlg)
{
	u8  action = xb_ch_action(dlg);
	int repeating = (IsDlgButtonChecked(dlg, IDC_CH_REPEAT)
	                 == BST_CHECKED);

	EnableWindow(GetDlgItem(dlg, IDC_CH_CAPTURE),
	             action == CORE_ACT_KEY ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_CH_CODE),
	             action != CORE_ACT_NONE ? TRUE : FALSE);
	EnableWindow(GetDlgItem(dlg, IDC_CH_HZ), repeating ? TRUE : FALSE);
}

static INT_PTR CALLBACK xb_chord_proc(HWND dlg, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
	(void)lp;

	switch (msg) {
	case WM_INITDIALOG:
	{
		static const int ID[CORE_CHORD_MEMBERS] = {
			IDC_CH_M1, IDC_CH_M2, IDC_CH_M3
		};
		HWND         list = GetDlgItem(dlg, IDC_CH_ACTION);
		u8           members[CORE_CHORD_MEMBERS];
		core_binding b;
		char         title[160];
		u32          i;

		snprintf(title, sizeof(title),
			     "Chord %u, layer %u - the controls it takes, and what"
			     " it does", (unsigned)(g_CSlot + 1),
			     (unsigned)(g_CLayer + 1));
		SetDlgItemTextA(dlg, IDC_CH_WHICH, title);
		snprintf(title, sizeof(title), "Chord %u",
			     (unsigned)(g_CSlot + 1));
		SetWindowTextA(dlg, title);

		xb_chord_get(&g_CP->cfg, g_CLayer, g_CSlot, members, &b);
		for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
			xb_ch_fill(dlg, ID[i], members[i]);
		}

		/*
		 * THE LAYER ACTIONS ARE NOT HERE EITHER. Layer Cycle and
		 * Layer Hold own their own buttons and write themselves into
		 * every layer; one put here would work in this layer alone.
		 */
		for (i = 0; XB_ACTIONS[i].name != NULL; i++) {
			u8  act = (u8)XB_ACTIONS[i].value;
			int at;

			if (xb_action_is_layer(act)) {
				continue;
			}
			at = (int)SendMessageA(list, LB_ADDSTRING, 0,
				                   (LPARAM)XB_ACTIONS[i].name);
			SendMessageA(list, LB_SETITEMDATA, (WPARAM)at,
				         (LPARAM)act);
		}

		SendMessageA(list, LB_SETCURSEL, 0, 0);
		SetDlgItemTextA(dlg, IDC_CH_HZ, "12");

		if (b.action != CORE_ACT_NONE) {
			int rows = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
			int r;
			char text[64];

			for (r = 0; r < rows; r++) {
				if ((u8)SendMessageA(list, LB_GETITEMDATA,
					                 (WPARAM)r, 0) == b.action) {
					SendMessageA(list, LB_SETCURSEL,
						         (WPARAM)r, 0);
					break;
				}
			}
			if (b.action == CORE_ACT_KEY) {
				const char *k = xb_value_to_name(xb_keys(),
					                             b.code);

				if (k != NULL) {
					snprintf(text, sizeof(text), "%s", k);
				} else {
					snprintf(text, sizeof(text), "0x%02X",
						     (unsigned)b.code);
				}
			} else {
				snprintf(text, sizeof(text), "%d",
					     (int)(s16)b.code);
			}
			SetDlgItemTextA(dlg, IDC_CH_CODE, text);

			CheckDlgButton(dlg, IDC_CH_TOGGLE,
				           (b.flags & CORE_BF_TOGGLE)
				           ? BST_CHECKED : BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_CH_REPEAT,
				           (b.flags & CORE_BF_REPEAT)
				           ? BST_CHECKED : BST_UNCHECKED);
			if (b.repeat_hz != 0) {
				snprintf(text, sizeof(text), "%u",
					     (unsigned)b.repeat_hz);
				SetDlgItemTextA(dlg, IDC_CH_HZ, text);
			}
			if (b.repeat_delay_ms != 0) {
				snprintf(text, sizeof(text), "%u",
					     (unsigned)b.repeat_delay_ms);
				SetDlgItemTextA(dlg, IDC_CH_DELAY, text);
			}
		}

		xb_ch_enable(dlg);
		xb_ch_preview(dlg);
		return TRUE;
	}

	case WM_COMMAND:
	{
		int id   = LOWORD(wp);
		int code = HIWORD(wp);

		if ((code == CBN_SELCHANGE &&
			 (id == IDC_CH_M1 || id == IDC_CH_M2 || id == IDC_CH_M3)) ||
			(code == LBN_SELCHANGE && id == IDC_CH_ACTION) ||
			(code == EN_CHANGE &&
			 (id == IDC_CH_CODE || id == IDC_CH_HZ ||
			  id == IDC_CH_DELAY)) ||
			id == IDC_CH_TOGGLE || id == IDC_CH_REPEAT) {
			xb_ch_enable(dlg);
			xb_ch_preview(dlg);
			if (id != IDOK && id != IDCANCEL &&
				id != IDC_CH_CAPTURE && id != IDC_CH_CLEAR) {
				return TRUE;
			}
		}

		switch (id) {
		case IDC_CH_CAPTURE:
		{
			u16 usage = 0;

			if (xb_capture_key(dlg, &usage)) {
				const char *k = xb_value_to_name(xb_keys(),
						                         usage);
				char        text[32];

				if (k != NULL) {
					snprintf(text, sizeof(text), "%s", k);
				} else {
					snprintf(text, sizeof(text), "0x%02X",
							 (unsigned)usage);
				}
				SetDlgItemTextA(dlg, IDC_CH_CODE, text);
				xb_ch_preview(dlg);
			}
			return TRUE;
		}

		case IDC_CH_CLEAR:
			SendDlgItemMessageA(dlg, IDC_CH_M1, CB_SETCURSEL, 0, 0);
			SendDlgItemMessageA(dlg, IDC_CH_M2, CB_SETCURSEL, 0, 0);
			SendDlgItemMessageA(dlg, IDC_CH_M3, CB_SETCURSEL, 0, 0);
			SendDlgItemMessageA(dlg, IDC_CH_ACTION, LB_SETCURSEL,
				                0, 0);
			SetDlgItemTextA(dlg, IDC_CH_CODE, "");
			CheckDlgButton(dlg, IDC_CH_TOGGLE, BST_UNCHECKED);
			CheckDlgButton(dlg, IDC_CH_REPEAT, BST_UNCHECKED);
			SetDlgItemTextA(dlg, IDC_CH_DELAY, "");
			xb_ch_enable(dlg);
			xb_ch_preview(dlg);
			return TRUE;

		case IDOK:
		{
			u8           members[CORE_CHORD_MEMBERS];
			u32          n = xb_ch_members(dlg, members);
			core_binding b;
			u8           cyc[2];
			u8           hld[2];

			if (n == 0) {
				xb_chord_set(&g_CP->cfg, g_CLayer, g_CSlot,
						     NULL, NULL);
				core_config_suppress(&g_CP->cfg);
				EndDialog(dlg, 1);
				return TRUE;
			}
			if (n < 2) {
				MessageBoxA(dlg,
					"A chord needs two controls or more. One "
					"control on its own is an ordinary binding "
					"- set it from the pad button instead.",
					"XBOXCTL", MB_OK | MB_ICONWARNING);
				return TRUE;
			}

			/*
			 * NOT THE CONTROLS A LAYER BUTTON ALREADY OWNS. Both
			 * would fire, which nobody means, and the layer
			 * control is the one with the invariant to protect.
			 */
			xb_layer_binding_get(&g_CP->cfg, CORE_ACT_LAYER_CYCLE,
					             cyc);
			xb_layer_binding_get(&g_CP->cfg, CORE_ACT_LAYER_HOLD,
					             hld);
			if ((n == 2 && members[0] == cyc[0] &&
				 members[1] == cyc[1]) ||
				(n == 2 && members[0] == hld[0] &&
				 members[1] == hld[1])) {
				MessageBoxA(dlg,
					"Those controls already change layer. Pick "
					"others here, or change the Layer Cycle or "
					"Layer Hold button.",
					"XBOXCTL", MB_OK | MB_ICONWARNING);
				return TRUE;
			}

			xb_ch_binding(dlg, &b);
			xb_chord_set(&g_CP->cfg, g_CLayer, g_CSlot, members,
					     b.action != CORE_ACT_NONE ? &b : NULL);
			core_config_suppress(&g_CP->cfg);
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

int xb_chord_dialog(HWND parent, xb_profile *p, u32 layer, u32 slot)
{
	g_CP     = p;
	g_CLayer = layer;
	g_CSlot  = slot;
	return (int)DialogBoxParamA(xb_instance(),
	                            MAKEINTRESOURCEA(IDD_CHORD), parent,
	                            xb_chord_proc, 0);
}

/* What the main dialog shows on the button. */
void xb_chord_summary(const core_config *cfg, u32 layer, u32 slot,
                      char *out, u32 out_bytes)
{
	u8           members[CORE_CHORD_MEMBERS];
	core_binding b;
	char         who[96];
	char         act[160];
	u32          i;

	if (!xb_chord_get(cfg, layer, slot, members, &b)) {
		snprintf(out, out_bytes, "(not set)");
		return;
	}

	who[0] = 0;
	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		const char *s;

		if (members[i] == CORE_SA_NONE) {
			continue;
		}
		s = xb_value_to_name(XB_SOURCES, members[i]);
		if (who[0] != 0) {
			strcat(who, "+");
		}
		strcat(who, s != NULL ? s : "?");
	}

	if (b.action == CORE_ACT_NONE) {
		snprintf(out, out_bytes, "%s", who);
		return;
	}
	xb_action_text(&b, act, sizeof(act));
	snprintf(out, out_bytes, "%s = %s", who, act);
}
