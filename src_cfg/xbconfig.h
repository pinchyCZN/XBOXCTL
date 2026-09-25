/*
 * xbconfig.h - the configurator, shared declarations.
 *
 * WHAT THIS PROGRAM IS. It edits a profile - a text file under
 * profiles/ next to the executable - and pushes the compiled binary
 * form to the driver over \.\xboxctl. It is the C replacement for
 * tools/mkconfig.py and it owns exactly what that script owned: the
 * keyboard layout, the name tables and the curve maths. The driver
 * still sees only a 33-entry lookup table and a HID usage.
 *
 * THE BLOB FORMAT IS NOT REIMPLEMENTED HERE. This project compiles the
 * driver's own core.c, so core_config_save is literally the same code
 * that runs in the kernel. A second implementation of a wire format
 * drifts; there is already one in mkconfig.py and the harness has to
 * cross-check it byte for byte. There will not be a third.
 *
 * profile.c and names.c NAME NO WINDOWS TYPE, for the same reason
 * core.c does not: the parser and the tables are the part worth testing
 * without a message loop in the way.
 */

#ifndef XBCONFIG_H
#define XBCONFIG_H

#include "../src_drv/core.h"

#define XB_MAX_PATH         512
#define XB_MAX_NAME         64
#define XB_MAX_PROFILES     128

/* ======================================================================
 * NAMES - names.c
 *
 * Every symbolic name the profile text uses, in both directions. The
 * tables are the single place a name and a number are tied together.
 * ====================================================================== */

typedef struct _xb_name {
	const char *name;
	u32         value;
} xb_name;

extern const xb_name XB_SOURCES[];
extern const xb_name XB_ACTIONS[];
extern const xb_name XB_FLAGS[];
extern const xb_name XB_STICK_MODES[];
extern const xb_name XB_MOUSE_BUTTONS[];

/*
 * CURVE PRESETS CARRY THEIR CONTROL POINTS EXACTLY, not squeezed into
 * an xb_name's integer. Two of the four presets are thirds, and
 * rounding 1/3 to three decimals moves every point of the 33-entry
 * table - enough to make this program and tools/mkconfig.py disagree
 * byte for byte on the same profile.
 */
typedef struct _xb_curve {
	const char *name;
	double      y1;
	double      y2;
} xb_curve;

extern const xb_curve XB_CURVES[];

int xb_curve_find(const char *name, double *y1, double *y2);

/* The key table is generated rather than listed - letters, digits and
 * function keys are contiguous - so it is reached through a call. */
const xb_name *xb_keys(void);

/* Lookup by name, case-insensitive. Returns 0 when there is no match
 * and leaves *value alone. */
int xb_name_to_value(const xb_name *table, const char *name, u32 *value);

/* Lookup by value. Returns NULL when nothing in the table has it. */
const char *xb_value_to_name(const xb_name *table, u32 value);

/* How many entries a table has, for filling a list box. */
u32 xb_name_count(const xb_name *table);

/* The label a pad control is shown under, e.g. CORE_SA_A -> "A". */
const char *xb_source_label(u8 source);

/* ======================================================================
 * CURVES
 *
 * The configurator owns the Bezier and the driver gets the table. X is
 * pinned at 1/3 and 2/3, which makes x(t) == t exactly, so g(u) is a
 * direct cubic evaluation and no root finding is needed.
 * ====================================================================== */

void xb_curve_build(double y1, double y2, u16 out[CORE_CURVE_POINTS]);

/* ======================================================================
 * PROFILE TEXT - profile.c
 * ====================================================================== */

typedef struct _xb_profile {
	core_config cfg;
	char        error[256];     /* why a load failed */
	int         error_line;
} xb_profile;

/* Start from the built-in default - a stock pad with nothing remapped. */
void xb_profile_defaults(xb_profile *p);

/* Read and write the .txt form. Both return 1 on success; on a failed
 * read p->error and p->error_line say why. */
int xb_profile_load(xb_profile *p, const char *path);
int xb_profile_save(const xb_profile *p, const char *path);

/* One binding as it is written in a profile, e.g.
 * "a -> key space repeat 12 hard 75%". Used by the save path and by the
 * binding dialog to show what it is about to write. */
void xb_binding_text(const core_config *cfg, u32 layer,
                     const core_binding *b, char *out, u32 out_bytes);

/* ======================================================================
 * THE DRIVER - driver.c
 * ====================================================================== */

/* Compile the configuration and push it to the pad at index. Returns 1
 * on success; on failure *why is a sentence the user can act on. */
int xb_driver_push(const core_config *cfg, u32 index,
                   char *why, u32 why_bytes);

/* Put the pad back on the driver's built-in default. */
int xb_driver_reset(u32 index, char *why, u32 why_bytes);

/* Is the driver there at all? Used to grey out the push. */
int xb_driver_present(void);

#endif  /* XBCONFIG_H */
