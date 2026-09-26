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
void xb_profile_empty(xb_profile *p);

/* Read and write the .txt form. Both return 1 on success; on a failed
 * read p->error and p->error_line say why. */
int xb_profile_load(xb_profile *p, const char *path);
int xb_profile_save(const xb_profile *p, const char *path);

/* ======================================================================
 * CHORD SLOTS ARE RESERVED, NOT ALLOCATED
 *
 * The blob carries eight per layer and the configurator exposes four of
 * them at fixed indices, because the thing addressing a slot is a BUTTON
 * ON A DIALOG and a button cannot point at "whichever slot was free".
 * First-come allocation would let a layer control and a chord swap
 * places between one save and the next, and the profile text names the
 * slot outright - [layer 1 chord 1] IS slot 0.
 *
 * Four sit idle. That is 16 bytes a layer for an arrangement nobody has
 * to reason about.
 * ====================================================================== */

#define XB_CHORD_SLOT_1         0   /* the Chord 1 button, per layer   */
#define XB_CHORD_SLOT_2         1   /* the Chord 2 button, per layer   */
#define XB_CHORD_SLOT_CYCLE     6   /* Layer Cycle, same in every layer */
#define XB_CHORD_SLOT_HOLD      7   /* Layer Hold, same in every layer  */

#define XB_CHORD_BUTTONS        2   /* how many the main dialog shows  */

/* The members and the action of one chord slot in one layer. Returns 0
 * when the slot is empty; *binding is the slot's binding, if any. */
int  xb_chord_get(const core_config *cfg, u32 layer, u32 slot,
                  u8 members[CORE_CHORD_MEMBERS], core_binding *binding);

/* Write both. members[0] of CORE_SA_NONE clears the slot. */
void xb_chord_set(core_config *cfg, u32 layer, u32 slot,
                  const u8 members[CORE_CHORD_MEMBERS],
                  const core_binding *binding);

/* ======================================================================
 * LAYER CONTROLS
 *
 * A control that changes layer is not an ordinary binding: the same
 * source has to appear in EVERY layer, because a binding only fires in
 * the layer it sits in and a chord is only scanned from the layer that
 * is live. Getting that wrong means a layer you can enter and not leave,
 * so it is done here rather than left to whoever edits a binding.
 * ====================================================================== */

/* Free every chord slot no binding names. An orphan is not inert - it
 * still holds off its members' own bindings. */
void xb_chords_gc(core_config *cfg);

/* members[1] may be CORE_SA_NONE for a single control; members[0] too,
 * which clears. Returns 0 if there is no room for it. */
int  xb_layer_binding_set(core_config *cfg, u8 action, u16 code,
                          const u8 members[2]);
void xb_layer_binding_clear(core_config *cfg, u8 action);
void xb_layer_binding_get(const core_config *cfg, u8 action,
                          u8 members[2]);

/* One binding as it is written in a profile, e.g.
 * "a -> key space repeat 12 hard 75%". Used by the save path and by the
 * binding dialog to show what it is about to write. */
/* Just the action half: "key f1 repeat 12". */
void xb_action_text(const core_binding *b, char *out, u32 out_bytes);

/* The members of a chord source, as "a+b+x". */
void xb_members_text(const core_config *cfg, u32 layer, u8 source,
                     char *out, u32 out_bytes);

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
