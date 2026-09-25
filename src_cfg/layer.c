/*
 * layer.c - the two layer controls.
 *
 * WHY THESE ARE NOT ORDINARY BINDINGS. A control that changes layer has
 * to exist in EVERY layer: a binding only fires in the layer it sits in,
 * and a chord is only scanned from the table of the layer that is live.
 * Put a cycle in layer 1 alone and you can enter layer 2 and never
 * leave. Offering layer_cycle in the ordinary binding dialog made that
 * mistake one click away and cluttered every control with three actions
 * that most of the flags do not apply to.
 *
 * So there are two buttons for it, each writing the same source into
 * both layers, and the binding dialog has no layer actions at all.
 *
 * ONE CONTROL OR TWO. A single button is a plain source; two make a
 * chord, which takes a slot in every layer's table.
 *
 * layer_set IS DELIBERATELY ABSENT. It latches a layer with no way back
 * from that control alone, which is a footgun with no use a cycle does
 * not cover. The wire format still carries it, so a profile that has one
 * keeps working.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "xbconfig.h"
#include "resource.h"

extern HINSTANCE xb_instance(void);

static xb_profile *g_LP;
static u8          g_Action;

/* The controls offered, in core.h order, guide excluded. */
static u8  g_Choice[CORE_SEMIAXIS_COUNT];
static u32 g_ChoiceCount;

static void xb_choices_build(void)
{
	u32 i;

	g_ChoiceCount = 0;
	for (i = 0; XB_SOURCES[i].name != NULL; i++) {
		u8 src = (u8)XB_SOURCES[i].value;

		/*
		 * GUIDE IS PINNED TO ZERO by core.c - it is a 360 control and
		 * this pad has no equivalent - so it could never complete a
		 * chord. Offering it would be offering a dead choice.
		 */
		if (src == CORE_SA_GUIDE) {
			continue;
		}
		g_Choice[g_ChoiceCount++] = src;
	}
}

/* Fill a combo with "(none)" first, then every control. */
static void xb_fill_combo(HWND dlg, int id, int allow_none, u8 selected)
{
	u32 i;
	int pick = 0;

	SendDlgItemMessageA(dlg, id, CB_RESETCONTENT, 0, 0);
	if (allow_none) {
		SendDlgItemMessageA(dlg, id, CB_ADDSTRING, 0,
		                    (LPARAM)"(none)");
	}
	for (i = 0; i < g_ChoiceCount; i++) {
		int at = (int)SendDlgItemMessageA(dlg, id, CB_ADDSTRING, 0,
		             (LPARAM)xb_source_label(g_Choice[i]));

		SendDlgItemMessageA(dlg, id, CB_SETITEMDATA, (WPARAM)at,
		                    (LPARAM)g_Choice[i]);
		if (g_Choice[i] == selected) {
			pick = at;
		}
	}
	SendDlgItemMessageA(dlg, id, CB_SETCURSEL, (WPARAM)pick, 0);
}

/* What a combo is showing, or CORE_SA_NONE for "(none)". */
static u8 xb_combo_source(HWND dlg, int id)
{
	int sel = (int)SendDlgItemMessageA(dlg, id, CB_GETCURSEL, 0, 0);

	/*
	 * ITEM 0 IS ALWAYS "(none)", and the INDEX is what says so. Reading
	 * the item data and testing it for zero would be ambiguous: D-pad up
	 * is source 0, so zero is a real answer.
	 */
	if (sel == CB_ERR || sel == 0) {
		return CORE_SA_NONE;
	}
	return (u8)SendDlgItemMessageA(dlg, id, CB_GETITEMDATA,
	                               (WPARAM)sel, 0);
}

static void xb_layer_preview(HWND dlg)
{
	u8   a = xb_combo_source(dlg, IDC_LY_BTN1);
	u8   b = xb_combo_source(dlg, IDC_LY_BTN2);
	char line[256];
	const char *act = (g_Action == CORE_ACT_LAYER_CYCLE)
	                  ? "layer_cycle 1" : "layer_hold 2";
	const char *an;
	const char *bn;

	if (a == CORE_SA_NONE) {
		SetDlgItemTextA(dlg, IDC_LY_PREVIEW,
		                "Nothing changes layer. Layer 2 will be"
		                " unreachable.");
		return;
	}

	an = xb_value_to_name(XB_SOURCES, a);
	bn = (b == CORE_SA_NONE) ? NULL : xb_value_to_name(XB_SOURCES, b);

	if (b != CORE_SA_NONE && b != a) {
		snprintf(line, sizeof(line),
		         "%s+%s -> %s     (in both layers)",
		         an != NULL ? an : "?", bn != NULL ? bn : "?", act);
	} else {
		snprintf(line, sizeof(line),
		         "%s -> %s     (in both layers)",
		         an != NULL ? an : "?", act);
	}
	SetDlgItemTextA(dlg, IDC_LY_PREVIEW, line);
}

static INT_PTR CALLBACK xb_layer_proc(HWND dlg, UINT msg, WPARAM wp,
                                      LPARAM lp)
{
	(void)lp;

	switch (msg) {
	case WM_INITDIALOG:
	{
		u8 members[2];

		xb_choices_build();
		xb_layer_binding_get(&g_LP->cfg, g_Action, members);

		if (g_Action == CORE_ACT_LAYER_CYCLE) {
			SetWindowTextA(dlg, "Layer Cycle");
			SetDlgItemTextA(dlg, IDC_LY_WHICH,
				"Which control steps to the next layer. Press it "
				"again to come back.");
			SetDlgItemTextA(dlg, IDC_LY_HINT,
				"Pick one control, or two to make a chord that "
				"will not fire by accident. The same control is "
				"written into both layers, because a binding only "
				"works in the layer it is in.");
		} else {
			SetWindowTextA(dlg, "Layer Hold");
			SetDlgItemTextA(dlg, IDC_LY_WHICH,
				"Which control makes layer 2 live while it is "
				"held. Release it and layer 1 comes back.");
			SetDlgItemTextA(dlg, IDC_LY_HINT,
				"Pick one control, or two to make a chord. It goes "
				"into both layers: without the copy in layer 2, "
				"holding it would drop straight back.");
		}

		xb_fill_combo(dlg, IDC_LY_BTN1, 1, members[0]);
		xb_fill_combo(dlg, IDC_LY_BTN2, 1, members[1]);
		xb_layer_preview(dlg);
		return TRUE;
	}

	case WM_COMMAND:
	{
		int id   = LOWORD(wp);
		int code = HIWORD(wp);

		if ((id == IDC_LY_BTN1 || id == IDC_LY_BTN2) &&
			code == CBN_SELCHANGE) {
			xb_layer_preview(dlg);
			return TRUE;
		}

		switch (id) {
		case IDC_LY_CLEAR:
			SendDlgItemMessageA(dlg, IDC_LY_BTN1, CB_SETCURSEL, 0, 0);
			SendDlgItemMessageA(dlg, IDC_LY_BTN2, CB_SETCURSEL, 0, 0);
			xb_layer_preview(dlg);
			return TRUE;

		case IDOK:
		{
			u8  members[2];
			u16 code_value;

			members[0] = xb_combo_source(dlg, IDC_LY_BTN1);
			members[1] = xb_combo_source(dlg, IDC_LY_BTN2);

			/*
			 * THE SAME CONTROL TWICE IS ONE CONTROL, not a chord
			 * that can never be down: core_member_down would want
			 * it pressed twice over.
			 */
			if (members[1] == members[0]) {
				members[1] = CORE_SA_NONE;
			}
			/* Second filled but first empty is still one control. */
			if (members[0] == CORE_SA_NONE &&
				members[1] != CORE_SA_NONE) {
				members[0] = members[1];
				members[1] = CORE_SA_NONE;
			}

			/*
			 * CYCLE TAKES A STEP, HOLD TAKES A LAYER. One is not an
			 * index and the other is, which is why they are not the
			 * same number.
			 */
			code_value = (g_Action == CORE_ACT_LAYER_CYCLE)
					     ? (u16)1
					     : (u16)(CORE_MAX_LAYOUTS - 1);

			if (!xb_layer_binding_set(&g_LP->cfg, g_Action,
					                  code_value, members)) {
				MessageBoxA(dlg,
					"There is no room left - every binding slot "
					"or every chord slot is taken in one of the "
					"layers.",
					"XBOXCTL", MB_OK | MB_ICONWARNING);
				return TRUE;
			}
			core_config_suppress(&g_LP->cfg);
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

int xb_layer_dialog(HWND parent, xb_profile *p, u8 action)
{
	g_LP     = p;
	g_Action = action;
	return (int)DialogBoxParamA(xb_instance(),
	                            MAKEINTRESOURCEA(IDD_LAYER), parent,
	                            xb_layer_proc, 0);
}

/* What the main dialog shows on the button. */
void xb_layer_summary(const core_config *cfg, u8 action,
                      char *out, u32 out_bytes)
{
	u8          members[2];
	const char *a;
	const char *b;

	xb_layer_binding_get(cfg, action, members);

	if (members[0] == CORE_SA_NONE) {
		snprintf(out, out_bytes, "(not set)");
		return;
	}
	a = xb_source_label(members[0]);
	if (members[1] == CORE_SA_NONE) {
		snprintf(out, out_bytes, "%s", a);
		return;
	}
	b = xb_source_label(members[1]);
	snprintf(out, out_bytes, "%s+%s", a, b);
}
