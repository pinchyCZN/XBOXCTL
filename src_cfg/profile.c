/*
 * profile.c - the .txt profile format, read and written.
 *
 * ONE FORMAT, TWO DIRECTIONS, AND THEY MUST AGREE. Everything this
 * writes it has to read back into the same configuration. That is the
 * property worth testing and it is why the writer sits beside the
 * parser rather than being scattered through the dialog code.
 *
 * NAMES NO WINDOWS TYPE, for the same reason core.c does not: the
 * parser is the part most worth exercising without a message loop in
 * front of it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "xbconfig.h"

/* ======================================================================
 * CURVES
 *
 * The configurator owns the Bezier and the driver gets the table. X is
 * pinned at 1/3 and 2/3, which makes x(t) == t exactly, so g(u) is a
 * direct cubic evaluation with no root finding.
 * ====================================================================== */

void xb_curve_build(double y1, double y2, u16 out[CORE_CURVE_POINTS])
{
	int i;

	for (i = 0; i < CORE_CURVE_POINTS; i++) {
		double u = (double)i / (double)(CORE_CURVE_POINTS - 1);
		double v = 3.0 * (1.0 - u) * (1.0 - u) * u * y1 +
		           3.0 * (1.0 - u) * u * u * y2 +
		           u * u * u;
		double s = v * 65535.0 + 0.5;   /* HALF UP, to match the driver */

		if (s < 0.0)     { s = 0.0; }
		if (s > 65535.0) { s = 65535.0; }
		out[i] = (u16)s;
	}

	/* The driver repairs a non-monotone table; do not hand it one. */
	for (i = 1; i < CORE_CURVE_POINTS; i++) {
		if (out[i] < out[i - 1]) {
			out[i] = out[i - 1];
		}
	}
}



/* ======================================================================
 * DEFAULTS
 * ====================================================================== */

void xb_profile_defaults(xb_profile *p)
{
	memset(p, 0, sizeof(*p));
	core_config_defaults(&p->cfg);
	core_config_suppress(&p->cfg);
}

/* ======================================================================
 * PARSING
 * ====================================================================== */

static void xb_fail(xb_profile *p, int line, const char *fmt, ...)
{
	va_list ap;

	if (p->error[0] != 0) {
		return;                 /* keep the FIRST failure, not the last */
	}
	p->error_line = line;
	va_start(ap, fmt);
	vsnprintf(p->error, sizeof(p->error), fmt, ap);
	va_end(ap);
}

/* Trim in place, and cut everything from the first comment mark. */
static char *xb_clean(char *s)
{
	char *end;
	char *hash = strchr(s, '#');

	if (hash != NULL) {
		*hash = 0;
	}
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
	                   end[-1] == '\r' || end[-1] == '\n')) {
		*--end = 0;
	}
	return s;
}

/* Split on whitespace, in place. Returns how many words were found. */
static int xb_split(char *s, char *words[], int max)
{
	int n = 0;

	while (*s != 0 && n < max) {
		while (*s == ' ' || *s == '\t') {
			s++;
		}
		if (*s == 0) {
			break;
		}
		words[n++] = s;
		while (*s != 0 && *s != ' ' && *s != '\t') {
			s++;
		}
		if (*s != 0) {
			*s++ = 0;
		}
	}
	return n;
}

static int xb_number(const char *s, s32 *out)
{
	char *end = NULL;
	long  v;

	if (*s == 0) {
		return 0;
	}
	v = strtol(s, &end, 0);
	if (end == s || *end != 0) {
		return 0;
	}
	*out = (s32)v;
	return 1;
}

/*
 * The value that follows an action name.
 *
 * LAYERS ARE WRITTEN THE WAY THEY ARE DECLARED. A profile says
 * [layer 1] and [layer 2], so layer_hold and layer_set take those same
 * numbers while the driver indexes from zero. layer_cycle is NOT
 * converted - its argument is a signed step, not an index.
 */
static int xb_parse_code(xb_profile *p, int line, u8 action,
                         const char *word, u16 *out)
{
	u32 v;
	s32 n;

	if (action == CORE_ACT_LAYER_HOLD || action == CORE_ACT_LAYER_SET) {
		if (!xb_number(word, &n) || n < 1 || n > CORE_MAX_LAYOUTS) {
			xb_fail(p, line, "layer must be 1..%d", CORE_MAX_LAYOUTS);
			return 0;
		}
		*out = (u16)(n - 1);
		return 1;
	}
	if (action == CORE_ACT_KEY) {
		if (xb_name_to_value(xb_keys(), word, &v)) {
			*out = (u16)v;
			return 1;
		}
		/*
		 * A BARE USAGE IS ACCEPTED TOO, because capture can reach keys
		 * the name table does not carry and the writer already emits
		 * "key 0x54" for them. Without this the configurator could save
		 * a profile it refused to read back.
		 *
		 * The range is the driver's: core_cfg_key_ok drops anything
		 * outside it, and a binding silently disarmed is worse than a
		 * profile that says why.
		 */
		if (xb_number(word, &n) &&
		    ((n >= 0x04 && n <= 0xA4) || (n >= 0xE0 && n <= 0xE7))) {
			*out = (u16)n;
			return 1;
		}
		xb_fail(p, line,
		        "'%s' is not a key name or a usage in 0x04..0xA4 or"
		        " 0xE0..0xE7", word);
		return 0;
	}
	if (action == CORE_ACT_MOUSE_BUTTON &&
	    xb_name_to_value(XB_MOUSE_BUTTONS, word, &v)) {
		*out = (u16)v;
		return 1;
	}
	if (!xb_number(word, &n)) {
		xb_fail(p, line, "'%s' is not a number", word);
		return 0;
	}
	*out = (u16)n;
	return 1;
}

/*
 * The members of one chord, from "a+b+x".
 *
 * NO SLOT IS CHOSEN HERE. A slot is named by the section that carries
 * the members - [layer 1 chord 1] is slot 0 - so there is nothing to
 * search for and nothing that can move between one save and the next.
 */
static int xb_parse_members(xb_profile *p, int line, char *text,
                            u8 out[CORE_CHORD_MEMBERS])
{
	u32   i;
	u32   n = 0;
	char *tok = text;

	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		out[i] = CORE_SA_NONE;
	}

	while (tok != NULL && *tok != 0) {
		char *plus = strchr(tok, '+');
		char *name;
		u32   v;

		if (plus != NULL) {
			*plus = 0;
		}
		name = xb_clean(tok);
		if (*name != 0) {
			if (n >= CORE_CHORD_MEMBERS) {
				xb_fail(p, line, "a chord takes at most %d controls",
				        CORE_CHORD_MEMBERS);
				return 0;
			}
			if (!xb_name_to_value(XB_SOURCES, name, &v)) {
				xb_fail(p, line, "unknown control '%s'", name);
				return 0;
			}
			out[n++] = (u8)v;
		}
		tok = (plus != NULL) ? plus + 1 : NULL;
	}

	if (n == 0) {
		xb_fail(p, line, "members names no control");
		return 0;
	}
	return 1;
}

/*
 * The right-hand side of a binding: the action, its value and its
 * options. b->source is left alone.
 *
 * SHARED, BECAUSE A CHORD SECTION'S "action =" IS THE SAME GRAMMAR. Two
 * readers of one syntax drift, and the drift shows up as an option that
 * works on a button and not on a chord.
 */
static int xb_parse_action(xb_profile *p, int line, char *right,
                           core_binding *b)
{
	char *words[24];
	int   n;
	int   i;
	u32   v;
	s32   num;

	n = xb_split(right, words, 24);
	if (n < 1) {
		xb_fail(p, line, "no action");
		return 0;
	}
	if (!xb_name_to_value(XB_ACTIONS, words[0], &v)) {
		xb_fail(p, line, "unknown action '%s'", words[0]);
		return 0;
	}
	b->action = (u8)v;

	i = 1;
	if (b->action != CORE_ACT_NONE) {
		if (i >= n) {
			xb_fail(p, line, "action '%s' needs a value", words[0]);
			return 0;
		}
		if (!xb_parse_code(p, line, b->action, words[i], &b->code)) {
			return 0;
		}
		i++;
	}

	while (i < n) {
		if (xb_name_to_value(XB_FLAGS, words[i], &v)) {
			if (v == CORE_BF_REPEAT) {
				if (i + 1 >= n || !xb_number(words[i + 1], &num)) {
					xb_fail(p, line, "'repeat' needs a rate in Hz");
					return 0;
				}
				b->flags |= CORE_BF_REPEAT;
				b->repeat_hz = (u8)num;
				i += 2;
			} else {
				b->flags |= (u8)v;
				i++;
			}
		} else if (strcmp(words[i], "delay") == 0) {
			if (i + 1 >= n || !xb_number(words[i + 1], &num)) {
				xb_fail(p, line, "'delay' needs milliseconds");
				return 0;
			}
			b->repeat_delay_ms = (u16)num;
			i += 2;
		} else if (strcmp(words[i], "hard") == 0) {
			/*
			 * A PERCENTAGE IS THE SANE WAY TO WRITE THIS. The raw unit
			 * is the 0..CORE_MAX_VALUE scale every analog value is
			 * decoded onto, NOT the 0..255 byte the pad sends, and the
			 * two are off by a factor of 137. Writing the byte gives a
			 * hard point near zero, which does not fail: it autofires
			 * from the lightest touch and reads as 'hard' being
			 * ignored.
			 */
			char *arg;
			u32   arglen;

			if (i + 1 >= n) {
				xb_fail(p, line, "'hard' needs a pressure");
				return 0;
			}
			arg    = words[i + 1];
			arglen = (u32)strlen(arg);
			if (arglen > 0 && arg[arglen - 1] == '%') {
				double pct;

				arg[arglen - 1] = 0;
				pct = atof(arg);
				if (pct < 0.0 || pct > 100.0) {
					xb_fail(p, line, "hard must be 0 to 100 per cent");
					return 0;
				}
				b->hard_at = (u16)((double)CORE_MAX_VALUE * pct / 100.0 +
				                  0.5);
			} else {
				if (!xb_number(arg, &num) || num < 0 ||
				    num > CORE_MAX_VALUE) {
					xb_fail(p, line,
					        "hard must be 0..%d or a percentage",
					        CORE_MAX_VALUE);
					return 0;
				}
				b->hard_at = (u16)num;
			}
			i += 2;
		} else {
			xb_fail(p, line, "unknown option '%s'", words[i]);
			return 0;
		}
	}

	if (b->hard_at != 0 && !(b->flags & CORE_BF_REPEAT)) {
		xb_fail(p, line, "'hard' needs 'repeat' - it is the pressure the"
		                 " autofire starts at, not an activation point");
		return 0;
	}

	return 1;
}

static int xb_parse_binding(xb_profile *p, int line, char *text,
                            core_layout *lay)
{
	char        *arrow;
	char        *left;
	u32          v;
	core_binding b;
	u32          slot;

	arrow = strstr(text, "->");
	if (arrow == NULL) {
		xb_fail(p, line, "a binding needs '->'");
		return 0;
	}
	*arrow = 0;
	left   = xb_clean(text);

	memset(&b, 0, sizeof(b));

	/*
	 * A CHORD IS NOT WRITTEN ON A BINDING LINE. It has a section of its
	 * own, which is what gives the two chord buttons a slot they can
	 * count on. Saying so beats "unknown source 'a+b'".
	 */
	if (strchr(left, '+') != NULL) {
		xb_fail(p, line,
		        "a chord goes in its own section now - put"
		        " 'members = %s' under [layer N chord 1]", left);
		return 0;
	}
	if (!xb_name_to_value(XB_SOURCES, left, &v)) {
		xb_fail(p, line, "unknown source '%s'", left);
		return 0;
	}
	b.source = (u8)v;

	if (!xb_parse_action(p, line, arrow + 2, &b)) {
		return 0;
	}

	for (slot = 0; slot < CORE_MAX_BINDINGS; slot++) {
		if (lay->binding[slot].action == CORE_ACT_NONE) {
			lay->binding[slot] = b;
			return 1;
		}
	}
	xb_fail(p, line, "more than %d bindings in one layer",
	        CORE_MAX_BINDINGS);
	return 0;
}

/*
 * A line inside a chord section. slot is where the members go; layers
 * says how many layers get them - one for a chord, all of them for a
 * layer control, which is what stops a layer you cannot leave.
 */
static int xb_parse_chord_line(xb_profile *p, int line, char *text,
                               u32 layer, u32 slot, int all_layers,
                               u8 action)
{
	char *eq = strchr(text, '=');
	char *key;
	char *value;
	u32   lay;

	if (eq == NULL) {
		xb_fail(p, line, "a chord setting needs '='");
		return 0;
	}
	*eq   = 0;
	key   = xb_clean(text);
	value = xb_clean(eq + 1);

	if (strcmp(key, "members") == 0) {
		u8 members[CORE_CHORD_MEMBERS];
		u32 named = 0;
		u32 m;

		if (!xb_parse_members(p, line, value, members)) {
			return 0;
		}
		for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
			if (members[m] != CORE_SA_NONE) {
				named++;
			}
		}

		for (lay = 0; lay < CORE_MAX_LAYOUTS; lay++) {
			if (!all_layers && lay != layer) {
				continue;
			}
			/*
			 * ONE CONTROL IS NOT A CHORD. A layer control named by a
			 * single button binds that button directly; taking a
			 * chord slot for it would make core_member_down wait for
			 * a second control that does not exist.
			 */
			if (named > 1) {
				memcpy(p->cfg.layout[lay].chord[slot].member,
				       members, CORE_CHORD_MEMBERS);
			}
			if (action != CORE_ACT_NONE) {
				u32 i;

				for (i = 0; i < CORE_MAX_BINDINGS; i++) {
					core_binding *b =
					    &p->cfg.layout[lay].binding[i];

					if (b->action != CORE_ACT_NONE) {
						continue;
					}
					memset(b, 0, sizeof(*b));
					b->source = (named > 1)
					            ? (u8)(CORE_SA_CHORD_BASE + slot)
					            : members[0];
					b->action = action;
					b->code   = (action == CORE_ACT_LAYER_CYCLE)
					            ? (u16)1
					            : (u16)(CORE_MAX_LAYOUTS - 1);
					break;
				}
			}
		}
		return 1;
	}

	if (strcmp(key, "action") == 0) {
		core_binding *b = NULL;
		u8            source;
		u32           named = 0;
		u32           i;

		/*
		 * MEMBERS FIRST. The action has to be bound to SOMETHING, and
		 * what that is comes from the members line - the chord slot if
		 * it names two controls or more, the control itself if one.
		 * Guessing would silently bind to whatever the slot held last.
		 */
		for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
			if (p->cfg.layout[layer].chord[slot].member[i] !=
			    CORE_SA_NONE) {
				named++;
			}
		}
		if (named > 1) {
			source = (u8)(CORE_SA_CHORD_BASE + slot);
		} else {
			xb_fail(p, line,
			        "'members' has to come before 'action'");
			return 0;
		}

		/* Reuse the slot's binding if members already made one. */
		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			core_binding *c = &p->cfg.layout[layer].binding[i];

			if (c->action != CORE_ACT_NONE && c->source == source) {
				b = c;
				break;
			}
		}
		if (b == NULL) {
			for (i = 0; i < CORE_MAX_BINDINGS; i++) {
				core_binding *c =
				    &p->cfg.layout[layer].binding[i];

				if (c->action == CORE_ACT_NONE) {
					b = c;
					break;
				}
			}
		}
		if (b == NULL) {
			xb_fail(p, line, "no room for another binding");
			return 0;
		}
		memset(b, 0, sizeof(*b));
		b->source = source;
		return xb_parse_action(p, line, value, b);
	}

	xb_fail(p, line, "a chord section takes 'members' and 'action'");
	return 0;
}

static int xb_parse_stick(xb_profile *p, int line, char *text,
                          core_stick *st)
{
	char *eq = strchr(text, '=');
	char *key;
	char *value;
	u32   v;
	s32   num;

	if (eq == NULL) {
		xb_fail(p, line, "a stick setting needs '='");
		return 0;
	}
	*eq   = 0;
	key   = xb_clean(text);
	value = xb_clean(eq + 1);

	if (strcmp(key, "mode") == 0) {
		if (!xb_name_to_value(XB_STICK_MODES, value, &v)) {
			xb_fail(p, line, "unknown stick mode '%s'", value);
			return 0;
		}
		st->mode = (u8)v;
		return 1;
	}
	if (strcmp(key, "curve") == 0) {
		char  *words[4];
		int    n = xb_split(value, words, 4);
		double y1;
		double y2;

		if (n >= 1 && xb_curve_find(words[0], &y1, &y2)) {
			xb_curve_build(y1, y2, st->curve);
			return 1;
		}
		if (n >= 2) {
			xb_curve_build(atof(words[0]), atof(words[1]), st->curve);
			return 1;
		}
		xb_fail(p, line, "curve takes a preset name or two numbers");
		return 0;
	}

	if (!xb_number(value, &num)) {
		xb_fail(p, line, "'%s' is not a number", value);
		return 0;
	}

	if      (strcmp(key, "deadzone")  == 0) { st->deadzone  = (u16)num; }
	else if (strcmp(key, "outer")     == 0) { st->outer     = (u16)num; }
	else if (strcmp(key, "max_speed") == 0) { st->max_speed = (u16)num; }
	else if (strcmp(key, "gain_x")    == 0) { st->gain_x    = (u8)num; }
	else if (strcmp(key, "gain_y")    == 0) { st->gain_y    = (u8)num; }
	else if (strcmp(key, "invert_x")  == 0) { st->invert_x  = (u8)num; }
	else if (strcmp(key, "invert_y")  == 0) { st->invert_y  = (u8)num; }
	else if (strcmp(key, "smooth_ms") == 0) { st->smooth_ms = (u8)num; }
	else if (strcmp(key, "accel_threshold") == 0) {
		st->accel_threshold = (u16)num;
	} else if (strcmp(key, "accel_rate") == 0) {
		st->accel_rate = (u16)num;
	} else if (strcmp(key, "accel_max") == 0) {
		st->accel_max = (u16)num;
	} else if (strcmp(key, "accel_decay") == 0) {
		st->accel_decay = (u16)num;
	} else {
		xb_fail(p, line, "unknown stick setting '%s'", key);
		return 0;
	}
	return 1;
}

int xb_profile_load(xb_profile *p, const char *path)
{
	FILE        *f;
	char         raw[512];
	char        *s;
	int          line = 0;
	int          section = 0;   /* 0 none, 1 layer, 2 stick, 3 global,
	                             * 4 a chord section */
	core_layout *lay = NULL;
	core_stick  *st = NULL;
	u32          ch_layer = 0;
	u32          ch_slot = 0;
	int          ch_all = 0;
	u8           ch_action = CORE_ACT_NONE;
	u32          i;
	u32          k;

	memset(p, 0, sizeof(*p));
	core_config_defaults(&p->cfg);

	/*
	 * START FROM AN EMPTY LAYOUT. A profile describes the whole
	 * configuration rather than a patch to the built-in one, and the
	 * default carries bindings of its own that would otherwise survive
	 * into a profile that never mentioned them.
	 */
	for (i = 0; i < CORE_MAX_LAYOUTS; i++) {
		for (k = 0; k < CORE_MAX_BINDINGS; k++) {
			core_binding *b = &p->cfg.layout[i].binding[k];

			memset(b, 0, sizeof(*b));
			b->source = CORE_SA_NONE;
			b->action = CORE_ACT_NONE;
		}
		for (k = 0; k < CORE_MAX_CHORDS; k++) {
			memset(p->cfg.layout[i].chord[k].member, CORE_SA_NONE,
			       CORE_CHORD_MEMBERS);
		}
	}

	f = fopen(path, "rb");
	if (f == NULL) {
		xb_fail(p, 0, "cannot open %s", path);
		return 0;
	}

	while (fgets(raw, sizeof(raw), f) != NULL) {
		line++;
		s = xb_clean(raw);
		if (*s == 0) {
			continue;
		}

		if (s[0] == '[') {
			char *close = strchr(s, ']');
			char *words[4];
			int   n;
			s32   num;

			if (close == NULL) {
				xb_fail(p, line, "unclosed section header");
				break;
			}
			*close = 0;
			n = xb_split(s + 1, words, 4);

			/*
			 * [layer cycle] AND [layer hold] ARE NOT PER LAYER, and
			 * the format says so rather than trusting anyone to write
			 * the same line under both. A control that changes layer
			 * has to exist in the layer it lands in or there is no way
			 * back, and a section that cannot be written for one layer
			 * alone cannot get that wrong.
			 */
			if (n == 2 && strcmp(words[0], "layer") == 0 &&
			    (strcmp(words[1], "cycle") == 0 ||
			     strcmp(words[1], "hold") == 0)) {
				ch_all    = 1;
				ch_layer  = 0;
				ch_action = (u8)(strcmp(words[1], "cycle") == 0
				                 ? CORE_ACT_LAYER_CYCLE
				                 : CORE_ACT_LAYER_HOLD);
				ch_slot   = (ch_action == CORE_ACT_LAYER_CYCLE)
				            ? XB_CHORD_SLOT_CYCLE
				            : XB_CHORD_SLOT_HOLD;
				section   = 4;
				continue;
			}

			/* [layer N chord M] - M names the slot outright. */
			if (n == 4 && strcmp(words[0], "layer") == 0 &&
			    strcmp(words[2], "chord") == 0) {
				s32 m;

				if (!xb_number(words[1], &num) ||
				    num < 1 || num > CORE_MAX_LAYOUTS) {
					xb_fail(p, line, "layer must be 1..%d",
					        CORE_MAX_LAYOUTS);
					break;
				}
				if (!xb_number(words[3], &m) ||
				    m < 1 || m > XB_CHORD_BUTTONS) {
					xb_fail(p, line, "chord must be 1..%d",
					        XB_CHORD_BUTTONS);
					break;
				}
				ch_layer  = (u32)(num - 1);
				ch_slot   = (u32)(m - 1);
				ch_all    = 0;
				ch_action = CORE_ACT_NONE;
				section   = 4;
				continue;
			}

			if (n >= 1 && strcmp(words[0], "layer") == 0) {
				if (n < 2 || !xb_number(words[1], &num) ||
				    num < 1 || num > CORE_MAX_LAYOUTS) {
					xb_fail(p, line, "layer must be 1..%d",
					        CORE_MAX_LAYOUTS);
					break;
				}
				lay     = &p->cfg.layout[num - 1];
				section = 1;
			} else if (n >= 1 && strcmp(words[0], "stick") == 0) {
				if (n < 2) {
					xb_fail(p, line, "stick must be left or right");
					break;
				}
				st = &p->cfg.stick[strcmp(words[1], "left") == 0 ? 0 : 1];
				section = 2;
			} else if (n >= 1 && strcmp(words[0], "global") == 0) {
				section = 3;
			} else {
				xb_fail(p, line, "unknown section");
				break;
			}
			continue;
		}

		if (section == 1) {
			if (!xb_parse_binding(p, line, s, lay)) {
				break;
			}
		} else if (section == 2) {
			if (!xb_parse_stick(p, line, s, st)) {
				break;
			}
		} else if (section == 4) {
			if (!xb_parse_chord_line(p, line, s, ch_layer, ch_slot,
			                         ch_all, ch_action)) {
				break;
			}
		} else if (section == 3) {
			/* [global] HAS NO SETTINGS. The empty header still parses so
			 * an older profile is not broken by its presence, but a key
			 * in it is an error rather than a silent no-op. */
			xb_fail(p, line, "[global] has no settings");
			break;
		} else {
			xb_fail(p, line, "line outside any section");
			break;
		}
	}
	fclose(f);

	if (p->error[0] != 0) {
		return 0;
	}

	core_config_suppress(&p->cfg);
	p->cfg.valid = 1;
	return 1;
}

/* ======================================================================
 * CHORD SLOTS
 * ====================================================================== */

/*
 * Free every chord slot no binding names any more.
 *
 * AN ORPHANED CHORD IS NOT INERT. core_chords_evaluate walks the whole
 * table, and a slot that still has members sets the HOLD MASK whenever
 * they are all down - which stops those controls' own bindings firing.
 * So a chord left behind by a deleted binding silently swallows presses
 * of the buttons that used to make it up.
 *
 * Called after anything removes a binding. Per layer, because the table
 * is per layer.
 */
void xb_layer_binding_clear(core_config *cfg, u8 action);

void xb_chords_gc(core_config *cfg)
{
	u32 lay;

	for (lay = 0; lay < CORE_MAX_LAYOUTS; lay++) {
		core_layout *l = &cfg->layout[lay];
		u32          c;

		for (c = 0; c < CORE_MAX_CHORDS; c++) {
			u32 i;
			int used = 0;

			for (i = 0; i < CORE_MAX_BINDINGS; i++) {
				if (l->binding[i].action != CORE_ACT_NONE &&
				    l->binding[i].source ==
				    (u8)(CORE_SA_CHORD_BASE + c)) {
					used = 1;
					break;
				}
			}
			if (!used) {
				memset(l->chord[c].member, CORE_SA_NONE,
				       CORE_CHORD_MEMBERS);
			}
		}
	}
}

/*
 * Put a binding on the same source into EVERY layer.
 *
 * WHICH IS WHAT A LAYER CONTROL NEEDS. A chord or button that changes
 * layer has to exist in the layer it lands in as well, or there is no
 * way back: core_chords_evaluate only scans the live layer's table, and
 * a binding only fires in the layer it sits in.
 *
 * members[1] may be CORE_SA_NONE for a single control. Returns 0 if
 * there is no room, having changed nothing.
 */
int xb_layer_binding_set(core_config *cfg, u8 action, u16 code,
                         const u8 members[2])
{
	u32 lay;
	int chord_slot = -1;

	xb_layer_binding_clear(cfg, action);

	if (members[0] == CORE_SA_NONE) {
		return 1;               /* cleared, and nothing to add */
	}

	if (members[1] != CORE_SA_NONE) {
		/*
		 * A RESERVED SLOT, NOT A FREE ONE. The two layer controls own
		 * slots 6 and 7 in every layer, so the button on the dialog
		 * has something it can point at. Searching for a free slot
		 * would let a layer control and a chord swap places between
		 * one save and the next.
		 */
		chord_slot = (action == CORE_ACT_LAYER_CYCLE)
		             ? XB_CHORD_SLOT_CYCLE
		             : XB_CHORD_SLOT_HOLD;
	}

	for (lay = 0; lay < CORE_MAX_LAYOUTS; lay++) {
		core_layout *l = &cfg->layout[lay];
		u32          i;
		int          placed = 0;

		if (chord_slot >= 0) {
			u32 m;

			/* Any member past the second is cleared, so reusing a
			 * slot cannot leave a third control behind. */
			for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
				l->chord[chord_slot].member[m] =
				    (m < 2) ? members[m] : (u8)CORE_SA_NONE;
			}
		}

		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			core_binding *b = &l->binding[i];

			if (b->action != CORE_ACT_NONE) {
				continue;
			}
			memset(b, 0, sizeof(*b));
			b->source = (chord_slot >= 0)
			            ? (u8)(CORE_SA_CHORD_BASE + chord_slot)
			            : members[0];
			b->action = action;
			b->code   = code;
			placed = 1;
			break;
		}
		if (!placed) {
			return 0;
		}
	}
	return 1;
}

/* Take this action out of every layer, and free any chord it leaves. */
void xb_layer_binding_clear(core_config *cfg, u8 action)
{
	u32 lay;

	for (lay = 0; lay < CORE_MAX_LAYOUTS; lay++) {
		u32 i;

		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			core_binding *b = &cfg->layout[lay].binding[i];

			if (b->action == action) {
				memset(b, 0, sizeof(*b));
				b->source = CORE_SA_NONE;
				b->action = CORE_ACT_NONE;
			}
		}
	}
	xb_chords_gc(cfg);
}

/*
 * What drives this action now: members[0] is CORE_SA_NONE when nothing
 * does, and members[1] is CORE_SA_NONE for a single control.
 */
void xb_layer_binding_get(const core_config *cfg, u8 action, u8 members[2])
{
	u32 i;

	members[0] = CORE_SA_NONE;
	members[1] = CORE_SA_NONE;

	for (i = 0; i < CORE_MAX_BINDINGS; i++) {
		const core_binding *b = &cfg->layout[0].binding[i];

		if (b->action != action) {
			continue;
		}
		if (b->source >= CORE_SA_CHORD_BASE &&
		    b->source < CORE_SA_CHORD_BASE + CORE_MAX_CHORDS) {
			const core_chord *c =
			    &cfg->layout[0].chord[b->source -
			                          CORE_SA_CHORD_BASE];

			members[0] = c->member[0];
			members[1] = c->member[1];
		} else {
			members[0] = b->source;
		}
		return;
	}
}

int xb_chord_get(const core_config *cfg, u32 layer, u32 slot,
                 u8 members[CORE_CHORD_MEMBERS], core_binding *binding)
{
	const core_chord *c;
	u32               i;
	int               named = 0;

	memset(members, CORE_SA_NONE, CORE_CHORD_MEMBERS);
	if (binding != NULL) {
		memset(binding, 0, sizeof(*binding));
		binding->source = CORE_SA_NONE;
		binding->action = CORE_ACT_NONE;
	}
	if (layer >= CORE_MAX_LAYOUTS || slot >= CORE_MAX_CHORDS) {
		return 0;
	}

	c = &cfg->layout[layer].chord[slot];
	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		members[i] = c->member[i];
		if (c->member[i] != CORE_SA_NONE) {
			named++;
		}
	}
	if (named == 0) {
		return 0;
	}

	if (binding != NULL) {
		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			const core_binding *b = &cfg->layout[layer].binding[i];

			if (b->action != CORE_ACT_NONE &&
			    b->source == (u8)(CORE_SA_CHORD_BASE + slot)) {
				*binding = *b;
				break;
			}
		}
	}
	return 1;
}

void xb_chord_set(core_config *cfg, u32 layer, u32 slot,
                  const u8 members[CORE_CHORD_MEMBERS],
                  const core_binding *binding)
{
	core_layout *l;
	u32          i;

	if (layer >= CORE_MAX_LAYOUTS || slot >= CORE_MAX_CHORDS) {
		return;
	}
	l = &cfg->layout[layer];

	/* Whatever was on this slot goes, members and binding together. */
	for (i = 0; i < CORE_MAX_BINDINGS; i++) {
		core_binding *b = &l->binding[i];

		if (b->action != CORE_ACT_NONE &&
		    b->source == (u8)(CORE_SA_CHORD_BASE + slot)) {
			memset(b, 0, sizeof(*b));
			b->source = CORE_SA_NONE;
			b->action = CORE_ACT_NONE;
		}
	}
	memset(l->chord[slot].member, CORE_SA_NONE, CORE_CHORD_MEMBERS);

	if (members == NULL || members[0] == CORE_SA_NONE) {
		return;
	}
	memcpy(l->chord[slot].member, members, CORE_CHORD_MEMBERS);

	if (binding == NULL || binding->action == CORE_ACT_NONE) {
		return;
	}
	for (i = 0; i < CORE_MAX_BINDINGS; i++) {
		core_binding *b = &l->binding[i];

		if (b->action == CORE_ACT_NONE) {
			*b = *binding;
			b->source = (u8)(CORE_SA_CHORD_BASE + slot);
			return;
		}
	}
}

/*
 * Just the action: "key f1 repeat 12".
 *
 * SHARED WITH THE CHORD SECTIONS, whose "action =" is the same grammar
 * the parser reads. One writer and one reader for one syntax, or an
 * option ends up working on a button and not on a chord.
 */
void xb_action_text(const core_binding *b, char *out, u32 out_bytes)
{
	const char *aname;
	char        value[32];
	char        extra[128];
	u32         i;

	out[0] = 0;
	if (b->action == CORE_ACT_NONE) {
		return;
	}

	aname = xb_value_to_name(XB_ACTIONS, b->action);

	if (b->action == CORE_ACT_KEY) {
		const char *k = xb_value_to_name(xb_keys(), b->code);

		if (k != NULL) {
			snprintf(value, sizeof(value), "%s", k);
		} else {
			snprintf(value, sizeof(value), "0x%02X",
			         (unsigned)b->code);
		}
	} else if (b->action == CORE_ACT_LAYER_HOLD ||
	           b->action == CORE_ACT_LAYER_SET) {
		snprintf(value, sizeof(value), "%u", (unsigned)(b->code + 1));
	} else if (b->action == CORE_ACT_MOUSE_PULSE) {
		snprintf(value, sizeof(value), "0x%04X", (unsigned)b->code);
	} else {
		snprintf(value, sizeof(value), "%d", (int)(s16)b->code);
	}

	extra[0] = 0;
	for (i = 0; XB_FLAGS[i].name != NULL; i++) {
		if (XB_FLAGS[i].value == CORE_BF_REPEAT) {
			continue;
		}
		if (b->flags & XB_FLAGS[i].value) {
			strcat(extra, " ");
			strcat(extra, XB_FLAGS[i].name);
		}
	}
	if (b->flags & CORE_BF_REPEAT) {
		char t[32];

		snprintf(t, sizeof(t), " repeat %u", (unsigned)b->repeat_hz);
		strcat(extra, t);
	}
	if (b->repeat_delay_ms != 0) {
		char t[32];

		snprintf(t, sizeof(t), " delay %u",
		         (unsigned)b->repeat_delay_ms);
		strcat(extra, t);
	}
	if (b->hard_at != 0) {
		char t[32];

		/*
		 * WRITTEN BACK AS A PERCENTAGE, ROUNDED TO WHOLE UNITS OF
		 * THE SCALE. The pad's byte is coarser than one per cent,
		 * so a whole number always reads back to the same byte.
		 */
		snprintf(t, sizeof(t), " hard %u%%",
		         (unsigned)(((u32)b->hard_at * 100u +
		                     CORE_MAX_VALUE / 2) / CORE_MAX_VALUE));
		strcat(extra, t);
	}

	snprintf(out, out_bytes, "%s %s%s",
	         aname != NULL ? aname : "?", value, extra);
}

/* The members of a chord source, as "a+b+x". */
void xb_members_text(const core_config *cfg, u32 layer, u8 source,
                     char *out, u32 out_bytes)
{
	const core_chord *c;
	u32               i;

	out[0] = 0;
	if (source < CORE_SA_CHORD_BASE ||
	    source >= CORE_SA_CHORD_BASE + CORE_MAX_CHORDS) {
		const char *m = xb_value_to_name(XB_SOURCES, source);

		snprintf(out, out_bytes, "%s", m != NULL ? m : "?");
		return;
	}

	/* FROM THIS LAYER'S TABLE. Slot N can name different controls in
	 * each layer, so layer 0 is not the answer. */
	c = &cfg->layout[layer].chord[source - CORE_SA_CHORD_BASE];
	for (i = 0; i < CORE_CHORD_MEMBERS; i++) {
		const char *m;

		if (c->member[i] == CORE_SA_NONE) {
			continue;
		}
		m = xb_value_to_name(XB_SOURCES, c->member[i]);
		if (out[0] != 0) {
			strcat(out, "+");
		}
		strcat(out, m != NULL ? m : "?");
	}
}

void xb_binding_text(const core_config *cfg, u32 layer,
                     const core_binding *b, char *out, u32 out_bytes)
{
	char source[80];
	char action[192];

	out[0] = 0;
	if (b->action == CORE_ACT_NONE) {
		return;
	}

	xb_members_text(cfg, layer, b->source, source, sizeof(source));
	xb_action_text(b, action, sizeof(action));
	snprintf(out, out_bytes, "%s -> %s", source, action);
}

/* The curve as the preset name that produced it, when one did. */
static const char *xb_curve_name(const u16 curve[CORE_CURVE_POINTS])
{
	u32 i;

	for (i = 0; XB_CURVES[i].name != NULL; i++) {
		u16 test[CORE_CURVE_POINTS];

		xb_curve_build(XB_CURVES[i].y1, XB_CURVES[i].y2, test);
		if (memcmp(test, curve, sizeof(test)) == 0) {
			return XB_CURVES[i].name;
		}
	}
	return NULL;
}

int xb_profile_save(const xb_profile *p, const char *path)
{
	FILE *f;
	u32   lay;
	u32   i;

	f = fopen(path, "wb");
	if (f == NULL) {
		return 0;
	}

	fprintf(f, "# XBOXCTL profile, written by xbconfig.\n");

	for (i = 0; i < CORE_STICK_COUNT; i++) {
		const core_stick *st = &p->cfg.stick[i];
		const char       *cname;
		const char       *mname;

		mname = xb_value_to_name(XB_STICK_MODES, st->mode);
		fprintf(f, "\n[stick %s]\n", i == 0 ? "left" : "right");
		fprintf(f, "mode = %s\n", mname != NULL ? mname : "off");

		/*
		 * THE DEADZONE IS WRITTEN WHATEVER THE MODE IS. It is not part
		 * of the pointer pipeline alone: a stick DIRECTION bound to a
		 * key is tested against it too, so it means something on a
		 * stick whose mode is off. Skipping it there loses a value the
		 * profile set, silently, on the next save.
		 */
		fprintf(f, "deadzone = %u\n", (unsigned)st->deadzone);
		fprintf(f, "outer = %u\n", (unsigned)st->outer);

		/* The rest only means something to a mode that makes a rate. */
		if (st->mode != CORE_STICK_MOUSE && st->mode != CORE_STICK_WHEEL) {
			continue;
		}
		fprintf(f, "max_speed = %u\n", (unsigned)st->max_speed);
		cname = xb_curve_name(st->curve);
		if (cname != NULL) {
			fprintf(f, "curve = %s\n", cname);
		}
		fprintf(f, "gain_x = %u\n", (unsigned)st->gain_x);
		fprintf(f, "gain_y = %u\n", (unsigned)st->gain_y);
		fprintf(f, "invert_x = %u\n", (unsigned)st->invert_x);
		fprintf(f, "invert_y = %u\n", (unsigned)st->invert_y);
		fprintf(f, "smooth_ms = %u\n", (unsigned)st->smooth_ms);
		if (st->accel_rate != 0) {
			fprintf(f, "accel_threshold = %u\n",
			        (unsigned)st->accel_threshold);
			fprintf(f, "accel_rate = %u\n", (unsigned)st->accel_rate);
			fprintf(f, "accel_max = %u\n", (unsigned)st->accel_max);
			fprintf(f, "accel_decay = %u\n", (unsigned)st->accel_decay);
		}
	}

	/*
	 * THE LAYER CONTROLS FIRST, AND NOT PER LAYER. They are written
	 * into every layer by the loader, so writing them once is the only
	 * form that cannot describe a layer you get into and not out of.
	 */
	for (i = 0; i < 2; i++) {
		u8          act = (u8)(i == 0 ? CORE_ACT_LAYER_CYCLE
		                              : CORE_ACT_LAYER_HOLD);
		const char *name = (i == 0) ? "cycle" : "hold";
		u8          members[2];
		const char *m0;
		const char *m1;

		xb_layer_binding_get(&p->cfg, act, members);
		if (members[0] == CORE_SA_NONE) {
			continue;
		}
		m0 = xb_value_to_name(XB_SOURCES, members[0]);
		fprintf(f, "\n[layer %s]\n", name);
		if (members[1] != CORE_SA_NONE) {
			m1 = xb_value_to_name(XB_SOURCES, members[1]);
			fprintf(f, "members = %s+%s\n",
			        m0 != NULL ? m0 : "?", m1 != NULL ? m1 : "?");
		} else {
			fprintf(f, "members = %s\n", m0 != NULL ? m0 : "?");
		}
	}

	for (lay = 0; lay < CORE_MAX_LAYOUTS; lay++) {
		fprintf(f, "\n[layer %u]\n", (unsigned)(lay + 1));
		for (i = 0; i < CORE_MAX_BINDINGS; i++) {
			const core_binding *b = &p->cfg.layout[lay].binding[i];
			char                text[256];

			if (b->action == CORE_ACT_NONE) {
				continue;
			}
			/*
			 * A CHORD HAS ITS OWN SECTION, and so does a layer
			 * control. Writing them here too would load them twice -
			 * once into the slot they name and once into a second
			 * binding nobody asked for.
			 */
			if (b->source >= CORE_SA_CHORD_BASE) {
				continue;
			}
			if (b->action == CORE_ACT_LAYER_CYCLE ||
			    b->action == CORE_ACT_LAYER_HOLD) {
				continue;
			}
			xb_binding_text(&p->cfg, lay, b, text, sizeof(text));
			fprintf(f, "%s\n", text);
		}

		/* The two chord slots this layer exposes. */
		for (i = 0; i < XB_CHORD_BUTTONS; i++) {
			u8           members[CORE_CHORD_MEMBERS];
			core_binding b;
			char         text[192];
			char         who[96];
			u32          m;

			if (!xb_chord_get(&p->cfg, lay, i, members, &b)) {
				continue;
			}
			who[0] = 0;
			for (m = 0; m < CORE_CHORD_MEMBERS; m++) {
				const char *s;

				if (members[m] == CORE_SA_NONE) {
					continue;
				}
				s = xb_value_to_name(XB_SOURCES, members[m]);
				if (who[0] != 0) {
					strcat(who, "+");
				}
				strcat(who, s != NULL ? s : "?");
			}
			fprintf(f, "\n[layer %u chord %u]\n",
			        (unsigned)(lay + 1), (unsigned)(i + 1));
			fprintf(f, "members = %s\n", who);
			if (b.action != CORE_ACT_NONE) {
				xb_action_text(&b, text, sizeof(text));
				fprintf(f, "action = %s\n", text);
			}
		}
	}

	fclose(f);
	return 1;
}
