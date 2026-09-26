/*
 * main.c - the configurator's main window.
 *
 * EVERYTHING LIVES BESIDE THE EXECUTABLE. Profiles are .txt files in a
 * profiles/ folder next to xbconfig.exe and the window position is in
 * xbconfig.ini next to it as well. Nothing is written to the registry,
 * to AppData, or to the working directory, so the whole thing can be
 * copied to a stick and carried to the test machine.
 *
 * A CONFIGURATION IS NAMED BY ITS FILE. profiles/arcade.txt is the
 * configuration called "arcade"; there is no name stored inside the
 * file that could disagree with it.
 *
 * THERE IS NO SAVE BUTTON. Clicking OK in a binding dialog writes the
 * profile. A configurator that can be left with unsaved work is a
 * configurator that loses it.
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbconfig.h"
#include "resource.h"

#define XB_STANDARD_LABEL   "< use standard configuration >"

static HINSTANCE    g_Instance;
static char         g_ExeDir[XB_MAX_PATH];
static char         g_ProfileDir[XB_MAX_PATH];
static char         g_IniPath[XB_MAX_PATH];

static xb_profile   g_Profile;          /* the one being edited      */
static char         g_ProfilePath[XB_MAX_PATH];
static int          g_ProfileLoaded;    /* 0 while "standard" is up  */
static u32          g_Layer;            /* which layer the pad edits */

/* Laid out by xb_layout - see RESIZING below. */
static int  g_BaseW;        /* client size at creation                */
static int  g_BaseH;
static int  g_PadX0;        /* the grid, in pixels                    */
static int  g_PadY0;
static int  g_PadW;
static int  g_PadH;
static int  g_PadDX;
static int  g_PadDY;
static RECT g_GroupBase;    /* client coordinates                     */
static RECT g_StatusBase;
static HWND g_Grip;


/*
 * THE PAD, LAID OUT AS A PAD. Three columns: the D-pad and the face
 * buttons on the left, the left stick and the middle buttons next, the
 * right stick and the shoulders last. The grid below IS the dialog - it
 * is walked to create the buttons and walked again to label them, so
 * moving a control means moving one entry.
 *
 * GUIDE IS NOT HERE ON PURPOSE. core.c pins that semiaxis to zero
 * because it is a 360 control and the original pad has no equivalent, so
 * a button for it could never do anything. A control an application can
 * see and nothing can press is a dead row.
 *
 * UP IS THE NEGATIVE SEMIAXIS. The decode negates Y so the value is
 * down-positive the way HID wants, which leaves a physical up push in
 * YNEG. Reading these the other way round puts every stick direction on
 * the wrong thumb movement.
 */
#define XB_CELL_NONE        0xF9
#define XB_CELL_CHORD_1     0xFA
#define XB_CELL_CHORD_2     0xFB
#define XB_CELL_CYCLE       0xFC
#define XB_CELL_HOLD        0xFD
#define XB_CELL_STICK_L     0xFE
#define XB_CELL_STICK_R     0xFF

#define XB_PAD_ROWS         10
#define XB_PAD_COLS         3
#define XB_PAD_CELLS        (XB_PAD_ROWS * XB_PAD_COLS)

static const u8 XB_PAD_GRID[XB_PAD_ROWS][XB_PAD_COLS] = {
	{ CORE_SA_DPAD_UP,    CORE_SA_LSTICK_YNEG, CORE_SA_RSTICK_YNEG },
	{ CORE_SA_DPAD_DOWN,  CORE_SA_LSTICK_YPOS, CORE_SA_RSTICK_YPOS },
	{ CORE_SA_DPAD_LEFT,  CORE_SA_LSTICK_XNEG, CORE_SA_RSTICK_XNEG },
	{ CORE_SA_DPAD_RIGHT, CORE_SA_LSTICK_XPOS, CORE_SA_RSTICK_XPOS },
	{ CORE_SA_A,          CORE_SA_WHITE,       CORE_SA_LTRIGGER    },
	{ CORE_SA_B,          CORE_SA_BLACK,       CORE_SA_RTRIGGER    },
	{ CORE_SA_X,          CORE_SA_START,       CORE_SA_LTHUMB      },
	{ CORE_SA_Y,          CORE_SA_BACK,        CORE_SA_RTHUMB      },
	{ XB_CELL_STICK_L,    XB_CELL_CYCLE,       XB_CELL_CHORD_1     },
	{ XB_CELL_STICK_R,    XB_CELL_HOLD,        XB_CELL_CHORD_2     }
};

int  xb_bind_dialog(HWND parent, xb_profile *p, u32 layer, u8 source);
int  xb_stick_dialog(HWND parent, xb_profile *p, u32 which);
int  xb_layer_dialog(HWND parent, xb_profile *p, u8 action);
void xb_layer_summary(const core_config *cfg, u8 action,
                      char *out, u32 out_bytes);
int  xb_chord_dialog(HWND parent, xb_profile *p, u32 layer, u32 slot);
void xb_chord_summary(const core_config *cfg, u32 layer, u32 slot,
                      char *out, u32 out_bytes);

/*
 * The module handle, for the dialogs in bind.c.
 *
 * REACHED THROUGH A CALL RATHER THAN DECLARED IN xbconfig.h, because
 * that header is included by profile.c and names.c and naming HINSTANCE
 * there would put windows.h in front of the parser.
 */
HINSTANCE xb_instance(void)
{
	return g_Instance;
}

/* ======================================================================
 * PATHS
 * ====================================================================== */

static void xb_paths_init(void)
{
	char *slash;

	GetModuleFileNameA(NULL, g_ExeDir, sizeof(g_ExeDir));
	slash = strrchr(g_ExeDir, '\\');
	if (slash != NULL) {
		slash[1] = 0;
	}
	snprintf(g_ProfileDir, sizeof(g_ProfileDir), "%sprofiles", g_ExeDir);
	snprintf(g_IniPath, sizeof(g_IniPath), "%sxbconfig.ini", g_ExeDir);
	CreateDirectoryA(g_ProfileDir, NULL);
}

/*
 * WHICH CONFIGURATION WAS OPEN, BY NAME AND NOT BY POSITION.
 *
 * The list is the profiles folder as the filesystem hands it over, so a
 * new file, a rename or a decision to sort the list differently moves
 * every entry after it. An index remembered across that reopens whatever
 * happens to sit in the slot now, which is a silently wrong
 * configuration rather than an error anybody would notice.
 *
 * A name that no longer names a file is not an error either: the file
 * was renamed or deleted between runs, which is an ordinary thing to do.
 * xb_list_fill falls back to the standard entry.
 */
static void xb_selected_save(const char *name)
{
	WritePrivateProfileStringA("profile", "name",
	                           name != NULL ? name : "", g_IniPath);
}

static void xb_selected_load(char *name, u32 bytes)
{
	GetPrivateProfileStringA("profile", "name", "", name, bytes,
	                         g_IniPath);
}

/* ======================================================================
 * THE PROFILE LIST
 * ====================================================================== */

static void xb_list_fill(HWND dlg, const char *select)
{
	HWND            list = GetDlgItem(dlg, IDC_PROFILES);
	WIN32_FIND_DATAA fd;
	HANDLE          find;
	char            pattern[XB_MAX_PATH];
	int             pick = 0;

	SendMessageA(list, LB_RESETCONTENT, 0, 0);
	SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)XB_STANDARD_LABEL);

	snprintf(pattern, sizeof(pattern), "%s\\*.txt", g_ProfileDir);
	find = FindFirstFileA(pattern, &fd);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			char  name[XB_MAX_NAME];
			char *dot;

			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				continue;
			}
			snprintf(name, sizeof(name), "%s", fd.cFileName);
			dot = strrchr(name, '.');
			if (dot != NULL) {
				*dot = 0;       /* THE NAME IS THE FILE, MINUS .txt */
			}
			SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)name);
		} while (FindNextFileA(find, &fd));
		FindClose(find);
	}

	if (select != NULL) {
		int found = (int)SendMessageA(list, LB_FINDSTRINGEXACT,
		                              (WPARAM)-1, (LPARAM)select);
		if (found != LB_ERR) {
			pick = found;
		}
	}
	SendMessageA(list, LB_SETCURSEL, (WPARAM)pick, 0);
}

static void xb_status(HWND dlg, const char *text)
{
	SetDlgItemTextA(dlg, IDC_STATUS, text);
}

/* The cell at a flat index, which is also its control id offset. */
static u8 xb_cell(u32 index)
{
	if (index >= XB_PAD_CELLS) {
		return XB_CELL_NONE;
	}
	return XB_PAD_GRID[index / XB_PAD_COLS][index % XB_PAD_COLS];
}

/* Refresh every pad button's caption with what it is bound to. */
static void xb_pad_refresh(HWND dlg)
{
	u32 i;

	for (i = 0; i < XB_PAD_CELLS; i++) {
		u8   cell = xb_cell(i);
		HWND w    = GetDlgItem(dlg, (int)(IDC_PAD_BASE + i));
		char caption[128];

		if (cell == XB_CELL_NONE || w == NULL) {
			continue;
		}

		if (cell == XB_CELL_STICK_L || cell == XB_CELL_STICK_R) {
			/* A STICK SHOWS ITS MODE, not a binding: it has none. */
			u32         which = (cell == XB_CELL_STICK_L) ? 0u : 1u;
			const char *mode  = xb_value_to_name(
			                        XB_STICK_MODES,
			                        g_Profile.cfg.stick[which].mode);

			snprintf(caption, sizeof(caption), "%s Stick : %s...",
			         which == 0 ? "Left" : "Right",
			         mode != NULL ? mode : "off");
		} else if (cell == XB_CELL_CHORD_1 ||
		           cell == XB_CELL_CHORD_2) {
			/* A CHORD IS PER LAYER, so this one does follow the
			 * layer the grid is showing. */
			u32  slot = (cell == XB_CELL_CHORD_1)
			            ? XB_CHORD_SLOT_1 : XB_CHORD_SLOT_2;
			char who[128];

			xb_chord_summary(&g_Profile.cfg, g_Layer, slot,
			                 who, sizeof(who));
			snprintf(caption, sizeof(caption), "Chord %u : %s...",
			         (unsigned)(slot + 1), who);
		} else if (cell == XB_CELL_CYCLE || cell == XB_CELL_HOLD) {
			/*
			 * A LAYER CONTROL IS NOT PER LAYER, so it reads the same
			 * whichever layer the grid is showing - which is the
			 * point of it living here instead of on a control.
			 */
			u8   act = (cell == XB_CELL_CYCLE)
			           ? (u8)CORE_ACT_LAYER_CYCLE
			           : (u8)CORE_ACT_LAYER_HOLD;
			char who[64];

			xb_layer_summary(&g_Profile.cfg, act, who, sizeof(who));
			snprintf(caption, sizeof(caption), "Layer %s : %s...",
			         cell == XB_CELL_CYCLE ? "Cycle" : "Hold", who);
		} else {
			const char *label = xb_source_label(cell);
			u32         k;
			int         bound = 0;

			if (g_ProfileLoaded) {
				const core_layout *lay =
				    &g_Profile.cfg.layout[g_Layer];

				for (k = 0; k < CORE_MAX_BINDINGS; k++) {
					const core_binding *b = &lay->binding[k];
					char                text[256];
					const char         *arrow;

					if (b->action == CORE_ACT_NONE ||
					    b->source != cell) {
						continue;
					}
					xb_binding_text(&g_Profile.cfg, g_Layer,
					                b, text, sizeof(text));
					arrow = strstr(text, "-> ");
					snprintf(caption, sizeof(caption), "%s : %s",
					         label,
					         arrow != NULL ? arrow + 3 : text);
					bound = 1;
					break;
				}
			}
			if (!bound) {
				snprintf(caption, sizeof(caption), "%s", label);
			}
		}
		SetWindowTextA(w, caption);
		EnableWindow(w, g_ProfileLoaded ? TRUE : FALSE);
	}
}

/*
 * Build the pad buttons.
 *
 * FROM A TABLE KEYED BY core.h, not from the resource script. A control
 * that exists on this dialog but not in the source enumeration could
 * not be bound to anything, and the compiler would never say so.
 */
static void xb_pad_create(HWND dlg)
{
	const int X0   = 145;
	const int Y0   = 16;
	const int W    = 105;
	const int H    = 15;
	const int DX   = 109;
	const int DY   = 17;
	HFONT     font = (HFONT)SendMessageA(dlg, WM_GETFONT, 0, 0);
	u32       i;

	/*
	 * THE PIXEL METRICS ARE TAKEN ONCE, HERE. MapDialogRect is the only
	 * thing that knows the font, and the resize path needs numbers it can
	 * hand to MoveWindow.
	 */
	{
		RECT one;
		RECT step;

		one.left   = X0;
		one.top    = Y0;
		one.right  = X0 + W;
		one.bottom = Y0 + H;
		MapDialogRect(dlg, &one);
		g_PadX0 = one.left;
		g_PadY0 = one.top;
		g_PadW  = one.right - one.left;
		g_PadH  = one.bottom - one.top;

		step.left   = 0;
		step.top    = 0;
		step.right  = DX;
		step.bottom = DY;
		MapDialogRect(dlg, &step);
		g_PadDX = step.right;
		g_PadDY = step.bottom;
	}

	for (i = 0; i < XB_PAD_CELLS; i++) {
		RECT r;
		HWND b;

		if (xb_cell(i) == XB_CELL_NONE) {
			continue;
		}

		r.left   = X0 + (int)(i % XB_PAD_COLS) * DX;
		r.top    = Y0 + (int)(i / XB_PAD_COLS) * DY;
		r.right  = r.left + W;
		r.bottom = r.top + H;
		MapDialogRect(dlg, &r);

		b = CreateWindowExA(0, "BUTTON", "",
		                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		                    BS_PUSHBUTTON | BS_LEFT,
		                    r.left, r.top, r.right - r.left,
		                    r.bottom - r.top,
		                    dlg, (HMENU)(INT_PTR)(IDC_PAD_BASE + i),
		                    g_Instance, NULL);
		if (b != NULL && font != NULL) {
			SendMessageA(b, WM_SETFONT, (WPARAM)font, TRUE);
		}
	}
}

/* ======================================================================
 * RESIZING, WIDTH ONLY
 *
 * WHAT THE EXTRA WIDTH IS FOR: the grid captions are "control : what it
 * does", and the long ones - a chord of three, or a binding carrying
 * repeat and hard - do not fit however wide the dialog ships. Rather
 * than pick one width for everybody, the buttons take whatever the
 * window is given.
 *
 * HEIGHT IS PINNED. There are exactly ten rows and nothing below them
 * that could use the room, so a taller window would only add grey.
 * WM_GETMINMAXINFO does that by setting the min and max track height to
 * the same number, which also stops the drag rather than letting it
 * happen and snapping back.
 *
 * EVERYTHING IS IN PIXELS FROM HERE ON. Dialog units are what the
 * resource is written in; MapDialogRect converts once at creation and
 * the deltas afterwards are device units, because that is what
 * MoveWindow speaks.
 * ====================================================================== */

/* A child's rectangle in its parent's client coordinates. */
static void xb_child_rect(HWND dlg, int id, RECT *r)
{
	GetWindowRect(GetDlgItem(dlg, id), r);
	MapWindowPoints(NULL, dlg, (POINT *)r, 2);
}

static void xb_layout(HWND dlg)
{
	RECT client;
	int  extra;
	int  w;
	int  dx;
	u32  i;

	if (g_BaseW == 0) {
		return;                 /* not built yet */
	}

	GetClientRect(dlg, &client);
	extra = (client.right - client.left) - g_BaseW;
	if (extra < 0) {
		extra = 0;
	}

	/*
	 * A THIRD OF THE EXTRA EACH, because there are three columns. The
	 * GAP is what is left alone - widening it too would spend the room
	 * on space between the buttons instead of inside them.
	 */
	w  = g_PadW + extra / 3;
	dx = w + (g_PadDX - g_PadW);

	for (i = 0; i < XB_PAD_CELLS; i++) {
		HWND b = GetDlgItem(dlg, (int)(IDC_PAD_BASE + i));

		if (b == NULL) {
			continue;       /* an empty cell has no button */
		}
		MoveWindow(b, g_PadX0 + (int)(i % XB_PAD_COLS) * dx,
		           g_PadY0 + (int)(i / XB_PAD_COLS) * g_PadDY,
		           w, g_PadH, TRUE);
	}

	/* The box round them, and the status line under it. */
	MoveWindow(GetDlgItem(dlg, IDC_PADGROUP),
	           g_GroupBase.left, g_GroupBase.top,
	           (g_GroupBase.right - g_GroupBase.left) + extra,
	           g_GroupBase.bottom - g_GroupBase.top, TRUE);
	MoveWindow(GetDlgItem(dlg, IDC_STATUS),
	           g_StatusBase.left, g_StatusBase.top,
	           (g_StatusBase.right - g_StatusBase.left) + extra,
	           g_StatusBase.bottom - g_StatusBase.top, TRUE);

	/*
	 * THE CLOSE BUTTON DOES NOT MOVE. It is where it is on purpose, and
	 * chasing the right edge would put it somewhere different every time
	 * the window changed size.
	 */

	if (g_Grip != NULL) {
		int gw = GetSystemMetrics(SM_CXVSCROLL);
		int gh = GetSystemMetrics(SM_CYHSCROLL);

		MoveWindow(g_Grip, client.right - gw, client.bottom - gh,
		           gw, gh, TRUE);
	}

	/* The group box paints its frame; moving it leaves the old one. */
	InvalidateRect(dlg, NULL, TRUE);
}

/*
 * Record what the dialog looked like at creation, and put the grip in.
 *
 * THE GRIP IS BUILT HERE RATHER THAN IN THE RESOURCE because it has to
 * follow the corner, and a control the layout code creates is a control
 * the layout code can move without the two descriptions of where it
 * lives disagreeing. It is a SCROLLBAR with SBS_SIZEGRIP - the same
 * triangle a status bar draws.
 */
static void xb_resize_init(HWND dlg)
{
	RECT c;
	int  gw = GetSystemMetrics(SM_CXVSCROLL);
	int  gh = GetSystemMetrics(SM_CYHSCROLL);

	GetClientRect(dlg, &c);
	g_BaseW = c.right - c.left;
	g_BaseH = c.bottom - c.top;

	xb_child_rect(dlg, IDC_PADGROUP, &g_GroupBase);
	xb_child_rect(dlg, IDC_STATUS, &g_StatusBase);

	g_Grip = CreateWindowExA(0, "SCROLLBAR", NULL,
	                         WS_CHILD | WS_VISIBLE | SBS_SIZEGRIP |
	                         SBS_SIZEBOXBOTTOMRIGHTALIGN,
	                         c.right - gw, c.bottom - gh, gw, gh,
	                         dlg, NULL, g_Instance, NULL);
}

/* ======================================================================
 * LOADING AND SAVING
 * ====================================================================== */

static void xb_selection_changed(HWND dlg)
{
	HWND list = GetDlgItem(dlg, IDC_PROFILES);
	int  sel  = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
	char name[XB_MAX_NAME];

	if (sel == LB_ERR || sel == 0) {
		g_ProfileLoaded  = 0;
		g_ProfilePath[0] = 0;
		xb_selected_save("");
		xb_profile_defaults(&g_Profile);
		xb_status(dlg, "The standard configuration - the pad as Windows "
		               "sees it with nothing remapped. Apply it to undo "
		               "a profile.");
		xb_pad_refresh(dlg);
		return;
	}

	SendMessageA(list, LB_GETTEXT, (WPARAM)sel, (LPARAM)name);
	snprintf(g_ProfilePath, sizeof(g_ProfilePath), "%s\\%s.txt",
	         g_ProfileDir, name);

	/*
	 * WRITTEN AS SOON AS IT CHANGES, not on the way out. A program that
	 * only records its state while closing tidily loses it to a crash,
	 * and this costs one small file write per click.
	 */
	xb_selected_save(name);

	if (!xb_profile_load(&g_Profile, g_ProfilePath)) {
		char msg[512];

		snprintf(msg, sizeof(msg), "%s line %d: %s", name,
		         g_Profile.error_line, g_Profile.error);
		xb_status(dlg, msg);
		g_ProfileLoaded = 0;
		xb_pad_refresh(dlg);
		return;
	}
	g_ProfileLoaded = 1;
	xb_status(dlg, "");
	xb_pad_refresh(dlg);
}

/*
 * Reread the folder, keeping the selection if it still exists.
 */
static void xb_refresh(HWND dlg)
{
	HWND list = GetDlgItem(dlg, IDC_PROFILES);
	int  sel  = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
	char name[XB_MAX_NAME];

	name[0] = 0;
	if (sel != LB_ERR && sel != 0) {
		SendMessageA(list, LB_GETTEXT, (WPARAM)sel, (LPARAM)name);
	}

	xb_list_fill(dlg, name[0] != 0 ? name : NULL);
	xb_selection_changed(dlg);
}

static void xb_profile_write(HWND dlg)
{
	if (!g_ProfileLoaded || g_ProfilePath[0] == 0) {
		return;
	}
	if (!xb_profile_save(&g_Profile, g_ProfilePath)) {
		xb_status(dlg, "Could not write the profile. Is it read-only?");
	}
}

/* ======================================================================
 * WINDOW POSITION
 * ====================================================================== */

static void xb_window_restore(HWND dlg)
{
	int x = GetPrivateProfileIntA("window", "x", -32000, g_IniPath);
	int y = GetPrivateProfileIntA("window", "y", -32000, g_IniPath);
	RECT work;

	if (x == -32000 || y == -32000) {
		return;
	}
	/* A SAVED POSITION CAN BE OFF SCREEN - a monitor was unplugged, or
	 * the layout changed. Put it back on the primary rather than
	 * opening a window nobody can reach. */
	SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
	if (x < work.left - 100 || x > work.right - 50 ||
	    y < work.top - 50 || y > work.bottom - 50) {
		return;
	}
	SetWindowPos(dlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static void xb_window_save(HWND dlg)
{
	RECT r;
	char text[32];

	GetWindowRect(dlg, &r);
	snprintf(text, sizeof(text), "%d", (int)r.left);
	WritePrivateProfileStringA("window", "x", text, g_IniPath);
	snprintf(text, sizeof(text), "%d", (int)r.top);
	WritePrivateProfileStringA("window", "y", text, g_IniPath);
}

/* ======================================================================
 * COMMANDS
 * ====================================================================== */

static char g_NewName[XB_MAX_NAME];

static INT_PTR CALLBACK xb_newname_proc(HWND dlg, UINT msg, WPARAM wp,
                                        LPARAM lp)
{
	(void)lp;
	switch (msg) {
	case WM_INITDIALOG:
		SendDlgItemMessageA(dlg, IDC_NEWNAME, EM_LIMITTEXT,
		                    XB_MAX_NAME - 8, 0);
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wp) == IDOK) {
			GetDlgItemTextA(dlg, IDC_NEWNAME, g_NewName,
			                sizeof(g_NewName));
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

/* A name that is safe as a file name and is not already taken. */
static int xb_name_ok(HWND dlg, const char *name)
{
	char path[XB_MAX_PATH];
	u32  i;

	if (name[0] == 0) {
		xb_status(dlg, "A configuration needs a name.");
		return 0;
	}
	for (i = 0; name[i] != 0; i++) {
		if (strchr("\\/:*?\"<>|", name[i]) != NULL) {
			xb_status(dlg, "A name cannot contain \\ / : * ? \" < > or |.");
			return 0;
		}
	}
	snprintf(path, sizeof(path), "%s\\%s.txt", g_ProfileDir, name);
	if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
		xb_status(dlg, "There is already a configuration with that name.");
		return 0;
	}
	return 1;
}

static void xb_add_config(HWND dlg)
{
	char       path[XB_MAX_PATH];
	xb_profile fresh;

	g_NewName[0] = 0;
	if (!DialogBoxParamA(g_Instance, MAKEINTRESOURCEA(IDD_NEWNAME), dlg,
	                     xb_newname_proc, 0)) {
		return;
	}
	if (!xb_name_ok(dlg, g_NewName)) {
		return;
	}

	/* A NEW CONFIGURATION STARTS AS THE STANDARD ONE, so a pad that has
	 * just had one applied behaves exactly as it did before anything
	 * was bound. */
	xb_profile_defaults(&fresh);
	snprintf(path, sizeof(path), "%s\\%s.txt", g_ProfileDir, g_NewName);
	if (!xb_profile_save(&fresh, path)) {
		xb_status(dlg, "Could not create the file.");
		return;
	}
	xb_list_fill(dlg, g_NewName);
	xb_selection_changed(dlg);
}

static void xb_apply(HWND dlg)
{
	char why[512];

	if (!g_ProfileLoaded) {
		if (xb_driver_reset(0, why, sizeof(why))) {
			xb_status(dlg, "The pad is back on the standard "
			               "configuration.");
		} else {
			xb_status(dlg, why);
		}
		return;
	}
	if (xb_driver_push(&g_Profile.cfg, 0, why, sizeof(why))) {
		xb_status(dlg, "Applied to the pad.");
	} else {
		xb_status(dlg, why);
	}
}

/* ======================================================================
 * THE MAIN DIALOG
 * ====================================================================== */

static INT_PTR CALLBACK xb_main_proc(HWND dlg, UINT msg, WPARAM wp,
                                     LPARAM lp)
{
	(void)lp;

	switch (msg) {
	case WM_INITDIALOG:
	{
		HWND layer = GetDlgItem(dlg, IDC_LAYER);
		u32  i;

		for (i = 0; i < CORE_MAX_LAYOUTS; i++) {
			char text[32];

			snprintf(text, sizeof(text), "Layer %u", (unsigned)(i + 1));
			SendMessageA(layer, CB_ADDSTRING, 0, (LPARAM)text);
		}
		SendMessageA(layer, CB_SETCURSEL, 0, 0);
		g_Layer = 0;

		xb_pad_create(dlg);
		xb_resize_init(dlg);
		{
			char last[XB_MAX_NAME];

			xb_selected_load(last, sizeof(last));
			xb_list_fill(dlg, last[0] != 0 ? last : NULL);
		}
		xb_selection_changed(dlg);
		xb_window_restore(dlg);
		return TRUE;
	}

	case WM_COMMAND:
	{
		int id   = LOWORD(wp);
		int code = HIWORD(wp);

		if (id >= IDC_PAD_BASE &&
			id < IDC_PAD_BASE + (int)XB_PAD_CELLS) {
			u8 cell = xb_cell((u32)(id - IDC_PAD_BASE));

			if (!g_ProfileLoaded || cell == XB_CELL_NONE) {
				return TRUE;
			}
			/* NO SAVE BUTTON: OK in any of them is the save. */
			if (cell == XB_CELL_STICK_L || cell == XB_CELL_STICK_R) {
				u32 which = (cell == XB_CELL_STICK_L) ? 0u : 1u;

				if (xb_stick_dialog(dlg, &g_Profile, which)) {
					xb_profile_write(dlg);
					xb_pad_refresh(dlg);
				}
			} else if (cell == XB_CELL_CHORD_1 ||
				       cell == XB_CELL_CHORD_2) {
				u32 slot = (cell == XB_CELL_CHORD_1)
					       ? XB_CHORD_SLOT_1 : XB_CHORD_SLOT_2;

				if (xb_chord_dialog(dlg, &g_Profile, g_Layer, slot)) {
					xb_profile_write(dlg);
					xb_pad_refresh(dlg);
				}
			} else if (cell == XB_CELL_CYCLE || cell == XB_CELL_HOLD) {
				u8 act = (cell == XB_CELL_CYCLE)
					     ? (u8)CORE_ACT_LAYER_CYCLE
					     : (u8)CORE_ACT_LAYER_HOLD;

				if (xb_layer_dialog(dlg, &g_Profile, act)) {
					xb_profile_write(dlg);
					xb_pad_refresh(dlg);
				}
			} else if (xb_bind_dialog(dlg, &g_Profile, g_Layer, cell)) {
				xb_profile_write(dlg);
				xb_pad_refresh(dlg);
			}
			return TRUE;
		}

		switch (id) {
		case IDC_PROFILES:
			if (code == LBN_SELCHANGE) {
				xb_selection_changed(dlg);
			}
			return TRUE;

		case IDC_LAYER:
			if (code == CBN_SELCHANGE) {
				int sel = (int)SendDlgItemMessageA(dlg, IDC_LAYER,
					                               CB_GETCURSEL, 0, 0);
				if (sel != CB_ERR) {
					g_Layer = (u32)sel;
					xb_pad_refresh(dlg);
				}
			}
			return TRUE;

		case IDC_TOGGLELAYER:
		{
			int sel = (int)SendDlgItemMessageA(dlg, IDC_LAYER,
					                           CB_GETCURSEL, 0, 0);

			if (sel == CB_ERR) {
				sel = 0;
			}
			sel = (sel + 1) % (int)CORE_MAX_LAYOUTS;
			SendDlgItemMessageA(dlg, IDC_LAYER, CB_SETCURSEL,
					            (WPARAM)sel, 0);

			g_Layer = (u32)sel;
			xb_pad_refresh(dlg);
			return TRUE;
		}

		case IDC_REFRESH:
			xb_refresh(dlg);
			return TRUE;

		case IDC_ADD:
			xb_add_config(dlg);
			return TRUE;

		case IDC_APPLY:
			xb_apply(dlg);
			return TRUE;

		case IDCANCEL:
			xb_window_save(dlg);
			DestroyWindow(dlg);
			return TRUE;
		}
		break;
	}

	case WM_GETMINMAXINFO:
	{
		MINMAXINFO *mmi = (MINMAXINFO *)lp;
		RECT        win;
		RECT        cli;
		int         frame_w;
		int         frame_h;

		/* This can arrive before the dialog is built. */
		if (g_BaseW == 0) {
			break;
		}

		/*
		 * TRACK SIZE IS THE WINDOW, NOT THE CLIENT, so the frame and
		 * caption have to be added back on or the clamp would be off by
		 * however thick the border happens to be on this machine.
		 */
		GetWindowRect(dlg, &win);
		GetClientRect(dlg, &cli);
		frame_w = (win.right - win.left) - (cli.right - cli.left);
		frame_h = (win.bottom - win.top) - (cli.bottom - cli.top);

		mmi->ptMinTrackSize.x = g_BaseW + frame_w;
		mmi->ptMaxTrackSize.x = g_BaseW * 2 + frame_w;

		/* Min and max the same: the height cannot be dragged at all. */
		mmi->ptMinTrackSize.y = g_BaseH + frame_h;
		mmi->ptMaxTrackSize.y = g_BaseH + frame_h;
		return TRUE;
	}

	case WM_SIZE:
		xb_layout(dlg);
		break;

	case WM_CLOSE:
		xb_window_save(dlg);
		DestroyWindow(dlg);
		return TRUE;

	/* MODELESS, so nothing else ends the loop. */
	case WM_DESTROY:
		PostQuitMessage(0);
		return TRUE;
	}
	return FALSE;
}

/*
 * A COMMAND LINE, SO THE PARSER CAN BE TESTED WITHOUT A MOUSE.
 *
 *     xbconfig --compile in.txt out.bin
 *     xbconfig --rewrite in.txt out.txt
 *
 * The first is what proves this program and tools/mkconfig.py agree: the
 * same profile through both has to produce the same bytes, the same way
 * the harness cross-checks core.c against that script. The second is the
 * round trip - a profile written back out must parse to the same thing.
 *
 * Errors go to the parent console when there is one. A Windows-subsystem
 * program has no stdout of its own, and giving it one would put a black
 * rectangle in front of a dialog program every time it started.
 */
static int xb_command_line(int argc, char **argv)
{
	xb_profile p;
	FILE      *out;
	u8         blob[4096];
	u32        len;

	AttachConsole(ATTACH_PARENT_PROCESS);

	if (argc < 4) {
		return 2;
	}

	if (!xb_profile_load(&p, argv[2])) {
		char msg[600];
		DWORD written = 0;
		HANDLE h = GetStdHandle(STD_ERROR_HANDLE);

		snprintf(msg, sizeof(msg), "%s line %d: %s\r\n", argv[2],
		         p.error_line, p.error);
		if (h != INVALID_HANDLE_VALUE) {
			WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
		}
		return 1;
	}

	if (strcmp(argv[1], "--rewrite") == 0) {
		return xb_profile_save(&p, argv[3]) ? 0 : 1;
	}

	if (strcmp(argv[1], "--compile") != 0) {
		return 2;
	}

	len = core_config_save(&p.cfg, blob, (u32)sizeof(blob));
	if (len == 0) {
		return 1;
	}
	out = fopen(argv[3], "wb");
	if (out == NULL) {
		return 1;
	}
	fwrite(blob, 1, len, out);
	fclose(out);
	return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	(void)prev;
	(void)cmd;
	(void)show;

	g_Instance = inst;

	if (__argc > 1) {
		return xb_command_line(__argc, __argv);
	}

	xb_paths_init();
	InitCommonControls();

	{
		HWND   dlg;
		HACCEL acc;
		MSG    msg;

		dlg = CreateDialogParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), NULL,
		                         xb_main_proc, 0);
		if (dlg == NULL) {
			return 1;
		}

		ShowWindow(dlg, SW_SHOW);

		acc = LoadAcceleratorsA(inst, MAKEINTRESOURCEA(IDA_MAIN));

		while (GetMessageA(&msg, NULL, 0, 0) > 0) {
			if (acc != NULL &&
			    TranslateAcceleratorA(dlg, acc, &msg)) {
				continue;
			}
			if (IsDialogMessageA(dlg, &msg)) {
				continue;
			}
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	return 0;
}
