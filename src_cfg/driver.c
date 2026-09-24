/*
 * driver.c - talking to xboxctl.sys over its private control device.
 *
 * THE DRIVER READS NO REGISTRY KEY AND NO FILE. A configuration reaches
 * it one way: this program opens \\.\xboxctl and hands over a blob. The
 * codes are built exactly as wdm.h builds them.
 *
 * \\.\xboxctl DOES NOT EXIST WHILE NO PAD IS PLUGGED IN. The control
 * device is created with the first pad and torn down with the last, so
 * "cannot open" is the ordinary state of an unplugged machine rather
 * than a fault worth alarming anyone about.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "xbconfig.h"

/*
 * CTL_CODE(type, fn, method, access)
 *   = (type << 16) | (access << 14) | (fn << 2) | method
 *
 * METHOD_BUFFERED is 0, so it does not appear.
 */
#define XC_DEVICE_TYPE      0xB9C0u
#define XC_FILE_ANY         0u
#define XC_FILE_WRITE       2u

#define XC_CTL(fn, access) \
    (((ULONG)XC_DEVICE_TYPE << 16) | ((ULONG)(access) << 14) | \
     ((ULONG)(fn) << 2))

#define IOCTL_XC_SET_CONFIG     XC_CTL(0x800 + 4, XC_FILE_WRITE)
#define IOCTL_XC_RESET_CONFIG   XC_CTL(0x800 + 5, XC_FILE_WRITE)
#define IOCTL_XC_GET_VERSION    XC_CTL(0x800 + 0, XC_FILE_ANY)

/*
 * The largest blob core_config_save can emit. Computed from the same
 * structures rather than copied from wdm.h, which names DDK types this
 * program has no business including.
 */
#define XB_BLOB_MAX     2048

/* Fails to compile if the blob ever outgrows that. */
typedef char xb_blob_fits[(sizeof(core_config_header) +
                           sizeof(core_stick) * CORE_STICK_COUNT +
                           sizeof(core_layout) * CORE_MAX_LAYOUTS)
                          <= XB_BLOB_MAX ? 1 : -1];

static HANDLE xb_open(void)
{
	return CreateFileA("\\\\.\\xboxctl",
	                   GENERIC_READ | GENERIC_WRITE,
	                   FILE_SHARE_READ | FILE_SHARE_WRITE,
	                   NULL, OPEN_EXISTING, 0, NULL);
}

static void xb_why(char *why, u32 why_bytes, DWORD err, const char *what)
{
	if (why == NULL || why_bytes == 0) {
		return;
	}
	if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
		snprintf(why, why_bytes,
		         "The driver is not there. \\\\.\\xboxctl only exists "
		         "while a pad is plugged in - plug one in and try "
		         "again.");
	} else if (err == ERROR_ACCESS_DENIED) {
		snprintf(why, why_bytes,
		         "Access denied opening \\\\.\\xboxctl. Run this as "
		         "administrator.");
	} else {
		snprintf(why, why_bytes, "%s failed, error %lu.", what,
		         (unsigned long)err);
	}
}

int xb_driver_present(void)
{
	HANDLE h = xb_open();

	if (h == INVALID_HANDLE_VALUE) {
		return 0;
	}
	CloseHandle(h);
	return 1;
}

int xb_driver_push(const core_config *cfg, u32 index,
                   char *why, u32 why_bytes)
{
	HANDLE  h;
	u8      blob[XB_BLOB_MAX];
	u8     *buf;
	u32     len;
	DWORD   returned = 0;
	BOOL    ok;
	DWORD   err;

	len = core_config_save(cfg, blob, (u32)sizeof(blob));
	if (len == 0) {
		snprintf(why, why_bytes,
		         "The configuration would not serialise, so nothing was "
		         "sent.");
		return 0;
	}

	/* The request is the pad index, then the blob. */
	buf = (u8 *)malloc(4 + len);
	if (buf == NULL) {
		snprintf(why, why_bytes, "Out of memory.");
		return 0;
	}
	memcpy(buf, &index, 4);
	memcpy(buf + 4, blob, len);

	h = xb_open();
	if (h == INVALID_HANDLE_VALUE) {
		xb_why(why, why_bytes, GetLastError(), "CreateFile");
		free(buf);
		return 0;
	}

	ok  = DeviceIoControl(h, IOCTL_XC_SET_CONFIG, buf, 4 + len,
	                      buf, 16, &returned, NULL);
	err = GetLastError();
	CloseHandle(h);
	free(buf);

	if (!ok) {
		xb_why(why, why_bytes, err, "SET_CONFIG");
		return 0;
	}
	return 1;
}

int xb_driver_reset(u32 index, char *why, u32 why_bytes)
{
	HANDLE h;
	u32    req = index;
	u8     out[16];
	DWORD  returned = 0;
	BOOL   ok;
	DWORD  err;

	h = xb_open();
	if (h == INVALID_HANDLE_VALUE) {
		xb_why(why, why_bytes, GetLastError(), "CreateFile");
		return 0;
	}
	ok  = DeviceIoControl(h, IOCTL_XC_RESET_CONFIG, &req, sizeof(req),
	                      out, sizeof(out), &returned, NULL);
	err = GetLastError();
	CloseHandle(h);

	if (!ok) {
		xb_why(why, why_bytes, err, "RESET_CONFIG");
		return 0;
	}
	return 1;
}
