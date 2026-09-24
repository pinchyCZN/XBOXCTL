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

/* The control each pad button stands for, in core.h order. */
static u8           g_PadSource[CORE_SEMIAXIS_COUNT];
static u32          g_PadCount;

int xb_bind_dialog(HWND parent, xb_profile *p, u32 layer, u8 source);
int xb_stick_dialog(HWND parent, xb_profile *p);

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

/* Refresh every pad button's caption with what it is bound to. */
static void xb_pad_refresh(HWND dlg)
{
	u32 i;

	for (i = 0; i < g_PadCount; i++) {
		u8          src = g_PadSource[i];
		const char *label = xb_source_label(src);
		char        caption[128];
		u32         k;
		int         bound = 0;

		if (g_ProfileLoaded) {
			const core_layout *lay = &g_Profile.cfg.layout[g_Layer];

			for (k = 0; k < CORE_MAX_BINDINGS; k++) {
				const core_binding *b = &lay->binding[k];
				char                text[256];
				const char         *arrow;

				if (b->action == CORE_ACT_NONE || b->source != src) {
					continue;
				}
				xb_binding_text(&g_Profile.cfg, b, text, sizeof(text));
				arrow = strstr(text, "-> ");
				snprintf(caption, sizeof(caption), "%s : %s", label,
				         arrow != NULL ? arrow + 3 : text);
				bound = 1;
				break;
			}
		}
		if (!bound) {
			snprintf(caption, sizeof(caption), "%s", label);
		}
		SetDlgItemTextA(dlg, (int)(IDC_PAD_BASE + i), caption);
		EnableWindow(GetDlgItem(dlg, (int)(IDC_PAD_BASE + i)),
		             g_ProfileLoaded ? TRUE : FALSE);
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
	const int COLS  = 3;
	const int X0    = 145;
	const int Y0    = 16;
	const int W     = 84;
	const int H     = 15;
	const int DX    = 88;
	const int DY    = 17;
	HFONT     font  = (HFONT)SendMessageA(dlg, WM_GETFONT, 0, 0);
	u32       i;

	g_PadCount = 0;
	for (i = 0; i < CORE_SEMIAXIS_COUNT; i++) {
		g_PadSource[g_PadCount++] = (u8)i;
	}

	for (i = 0; i < g_PadCount; i++) {
		RECT r;
		HWND b;

		r.left   = X0 + (int)(i % COLS) * DX;
		r.top    = Y0 + (int)(i / COLS) * DY;
		r.right  = r.left + W;
		r.bottom = r.top + H;
		MapDialogRect(dlg, &r);

		b = CreateWindowExA(0, "BUTTON", xb_source_label(g_PadSource[i]),
		                    WS_CHILD | WS_VISIBLE | WS_TABSTOP |
		                    BS_PUSHBUTTON,
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
		xb_list_fill(dlg, NULL);
		xb_selection_changed(dlg);
		xb_window_restore(dlg);
		return TRUE;
	}

	case WM_COMMAND:
	{
		int id   = LOWORD(wp);
		int code = HIWORD(wp);

		if (id >= IDC_PAD_BASE && id < IDC_PAD_BASE + (int)g_PadCount) {
			u8 src = g_PadSource[id - IDC_PAD_BASE];

			if (!g_ProfileLoaded) {
				return TRUE;
			}
			if (xb_bind_dialog(dlg, &g_Profile, g_Layer, src)) {
				/* NO SAVE BUTTON: OK in the binding dialog is the save. */
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

		case IDC_ADD:
			xb_add_config(dlg);
			return TRUE;

		case IDC_APPLY:
			xb_apply(dlg);
			return TRUE;

		case IDC_STICKS:
			if (!g_ProfileLoaded) {
				return TRUE;
			}
			if (xb_stick_dialog(dlg, &g_Profile)) {
				xb_profile_write(dlg);
			}
			return TRUE;

		case IDCANCEL:
			xb_window_save(dlg);
			EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}

	case WM_CLOSE:
		xb_window_save(dlg);
		EndDialog(dlg, 0);
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
	DialogBoxParamA(inst, MAKEINTRESOURCEA(IDD_MAIN), NULL,
	                xb_main_proc, 0);
	return 0;
}
