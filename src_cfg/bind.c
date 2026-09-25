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

/* ======================================================================
 * CAPTURE
 * ====================================================================== */

/*
 * Virtual key to HID usage.
 *
 * Letters, digits and function keys are computed - they are contiguous
 * in both encodings - and the rest is a table. A key with no usage
 * returns 0 rather than guessing.
 */
static u16 xb_vk_to_usage(int vk)
{
	static const struct { int vk; u16 usage; } NAMED[] = {
		{ VK_RETURN, 0x28 }, { VK_ESCAPE, 0x29 }, { VK_BACK, 0x2A },
		{ VK_TAB, 0x2B },    { VK_SPACE, 0x2C },
		{ VK_OEM_MINUS, 0x2D }, { VK_OEM_PLUS, 0x2E },
		{ VK_OEM_4, 0x2F },  { VK_OEM_6, 0x30 }, { VK_OEM_5, 0x31 },
		{ VK_OEM_1, 0x33 },  { VK_OEM_7, 0x34 }, { VK_OEM_3, 0x35 },
		{ VK_OEM_COMMA, 0x36 }, { VK_OEM_PERIOD, 0x37 },
		{ VK_OEM_2, 0x38 },  { VK_CAPITAL, 0x39 },
		{ VK_SNAPSHOT, 0x46 }, { VK_SCROLL, 0x47 }, { VK_PAUSE, 0x48 },
		{ VK_INSERT, 0x49 }, { VK_HOME, 0x4A }, { VK_PRIOR, 0x4B },
		{ VK_DELETE, 0x4C }, { VK_END, 0x4D },  { VK_NEXT, 0x4E },
		{ VK_RIGHT, 0x4F },  { VK_LEFT, 0x50 }, { VK_DOWN, 0x51 },
		{ VK_UP, 0x52 },
		{ VK_LCONTROL, 0xE0 }, { VK_LSHIFT, 0xE1 }, { VK_LMENU, 0xE2 },
		{ VK_LWIN, 0xE3 },
		{ VK_RCONTROL, 0xE4 }, { VK_RSHIFT, 0xE5 }, { VK_RMENU, 0xE6 },
		{ VK_RWIN, 0xE7 },
		{ 0, 0 }
	};
	int i;

	if (vk >= 'A' && vk <= 'Z') {
		return (u16)(0x04 + (vk - 'A'));
	}
	if (vk >= '1' && vk <= '9') {
		return (u16)(0x1E + (vk - '1'));
	}
	if (vk == '0') {
		return 0x27;
	}
	if (vk >= VK_F1 && vk <= VK_F12) {
		return (u16)(0x3A + (vk - VK_F1));
	}
	for (i = 0; NAMED[i].vk != 0; i++) {
		if (NAMED[i].vk == vk) {
			return NAMED[i].usage;
		}
	}
	return 0;
}

static u16 g_Captured;

static INT_PTR CALLBACK xb_capture_proc(HWND dlg, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
	switch (msg) {
	case WM_INITDIALOG:
		g_Captured = 0;
		/* THE DIALOG ITSELF HAS TO SEE THE KEY. Without this the edit
		 * and button controls swallow it first. */
		SetTimer(dlg, 1, 20, NULL);
		return TRUE;

	case WM_TIMER:
	{
		int vk;

		for (vk = 8; vk < 256; vk++) {
			if (vk == VK_LBUTTON || vk == VK_RBUTTON ||
				vk == VK_MBUTTON) {
				continue;
			}
			if ((GetAsyncKeyState(vk) & 0x8000) == 0) {
				continue;
			}
			if (vk == VK_ESCAPE) {
				KillTimer(dlg, 1);
				EndDialog(dlg, 0);
				return TRUE;
			}
			g_Captured = xb_vk_to_usage(vk);
			if (g_Captured != 0) {
				KillTimer(dlg, 1);
				EndDialog(dlg, 1);
				return TRUE;
			}
		}
		return TRUE;
	}

	case WM_COMMAND:
		if (LOWORD(wp) == IDCANCEL) {
			KillTimer(dlg, 1);
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}
	(void)lp;
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
	int  sel;
	u32  v;

	memset(b, 0, sizeof(*b));
	b->source = g_Source;

	sel = (int)SendDlgItemMessageA(dlg, IDC_ACTION, LB_GETCURSEL, 0, 0);
	if (sel == LB_ERR) {
		sel = 0;
	}
	b->action = (u8)XB_ACTIONS[sel].value;
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
	int          sel;
	u8           action;
	int          is_key;
	int          repeating;

	xb_dialog_to_binding(dlg, &b);
	xb_binding_text(&g_P->cfg, &b, text, sizeof(text));
	SetDlgItemTextA(dlg, IDC_PREVIEW,
	                text[0] != 0 ? text : "(nothing - this control keeps"
	                                      " its normal pad behaviour)");

	sel = (int)SendDlgItemMessageA(dlg, IDC_ACTION, LB_GETCURSEL, 0, 0);
	action = (u8)(sel == LB_ERR ? 0 : XB_ACTIONS[sel].value);
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

		for (i = 0; XB_ACTIONS[i].name != NULL; i++) {
			SendMessageA(list, LB_ADDSTRING, 0,
				         (LPARAM)XB_ACTIONS[i].name);
		}

		slot = xb_find_binding(g_P, g_Layer, g_Source);
		if (slot < 0) {
			SendMessageA(list, LB_SETCURSEL, 0, 0);
			SetDlgItemTextA(dlg, IDC_HZ, "12");
		} else {
			const core_binding *b =
				&g_P->cfg.layout[g_Layer].binding[slot];
			char text[64];

			for (i = 0; XB_ACTIONS[i].name != NULL; i++) {
				if (XB_ACTIONS[i].value == b->action) {
					SendMessageA(list, LB_SETCURSEL, (WPARAM)i, 0);
					break;
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
			if (DialogBoxParamA(xb_instance(),
				                MAKEINTRESOURCEA(IDD_CAPTURE), dlg,
				                xb_capture_proc, 0)) {
				const char *k = xb_value_to_name(xb_keys(), g_Captured);
				char        text[32];

				if (k != NULL) {
					snprintf(text, sizeof(text), "%s", k);
				} else {
					snprintf(text, sizeof(text), "0x%02X",
						     (unsigned)g_Captured);
				}
				SetDlgItemTextA(dlg, IDC_CODE, text);
				xb_update_preview(dlg);
			}
			return TRUE;

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

			xb_dialog_to_binding(dlg, &b);
			slot = xb_find_binding(g_P, g_Layer, g_Source);

			if (b.action == CORE_ACT_NONE) {
				if (slot >= 0) {
					core_binding *t =
						&g_P->cfg.layout[g_Layer].binding[slot];

					memset(t, 0, sizeof(*t));
					t->source = CORE_SA_NONE;
					t->action = CORE_ACT_NONE;
				}
			} else if (slot >= 0) {
				g_P->cfg.layout[g_Layer].binding[slot] = b;
			} else {
				u32 i;

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

int xb_bind_dialog(HWND parent, xb_profile *p, u32 layer, u8 source)
{
	g_P      = p;
	g_Layer  = layer;
	g_Source = source;
	return (int)DialogBoxParamA(xb_instance(),
	                            MAKEINTRESOURCEA(IDD_BIND), parent,
	                            xb_bind_proc, 0);
}
