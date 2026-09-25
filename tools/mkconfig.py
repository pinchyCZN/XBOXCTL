"""Build an XBOXCTL configuration blob from a text profile.

There is no configurator UI yet. This is the stand-in: it owns the keyboard
layout, the name-to-usage table and the curve maths, exactly as the real
configurator will, and emits the fixed-size binary the driver parses.

    python tools/mkconfig.py --default -o arcade.bin
    python tools/mkconfig.py profiles/arcade.txt -o arcade.bin
    python tools/mkconfig.py --dump arcade.bin

The driver never sees a curve, a key name or a Bezier control point. It sees
a 33-entry lookup table and a HID usage. Everything symbolic is resolved
here. See ../docs/mapping-engine.txt section 10 and
../docs/analog-to-mouse.txt sections 4.3 and 7.
"""

import argparse
import struct
import sys

# --------------------------------------------------------------------------
# Wire format. These sizes are asserted at compile time in core.c; the check
# here catches a drift in this script rather than in the driver.
# --------------------------------------------------------------------------

SIGNATURE = 0x55534258          # 'XBSU' in byte order
VERSION = 1

MAX_LAYOUTS = 2
MAX_BINDINGS = 32
MAX_CHORDS = 8
CHORD_MEMBERS = 3
CURVE_POINTS = 33
STICK_COUNT = 2

HDR_FMT = "<IHHHHBBBB16s"
BINDING_FMT = "<BBHHBBHH"
CHORD_FMT = "<BBBB"
STICK_FMT = "<7H6B2x33H"

HDR_SIZE = struct.calcsize(HDR_FMT)
BINDING_SIZE = struct.calcsize(BINDING_FMT)
CHORD_SIZE = struct.calcsize(CHORD_FMT)
STICK_SIZE = struct.calcsize(STICK_FMT)
LAYOUT_SIZE = MAX_BINDINGS * BINDING_SIZE + MAX_CHORDS * CHORD_SIZE + 4

assert HDR_SIZE == 32, HDR_SIZE
assert BINDING_SIZE == 12, BINDING_SIZE
assert CHORD_SIZE == 4, CHORD_SIZE
assert STICK_SIZE == 88, STICK_SIZE
assert LAYOUT_SIZE == 420, LAYOUT_SIZE

# CHORD SLOTS ARE RESERVED, NOT ALLOCATED. The configurator addresses
# them from buttons, and the profile text names them outright, so
# [layer 1 chord 1] IS slot 0 and nothing can move between saves.
CHORD_SLOT_1 = 0
CHORD_SLOT_2 = 1
CHORD_SLOT_CYCLE = 6
CHORD_SLOT_HOLD = 7
CHORD_BUTTONS = 2

MAX_VALUE = 35000
SA_NONE = 0xFF
SA_CHORD_BASE = 64

# --------------------------------------------------------------------------
# Sources, in the order core.h declares them.
# --------------------------------------------------------------------------

SOURCES = {
    "dup": 0, "ddown": 1, "dleft": 2, "dright": 3,
    "start": 4, "back": 5, "lthumb": 6, "rthumb": 7,
    "a": 8, "b": 9, "x": 10, "y": 11,
    "black": 12, "white": 13, "ltrigger": 14, "rtrigger": 15,
# UP IS THE NEGATIVE SEMIAXIS, AND THAT IS NOT A TYPO. The pad reports
# Y up-positive and the decode negates it once so the value is
# down-positive the way HID wants, which leaves a physical UP push in
# the semiaxis named YNEG. Naming these the other way round reads
# correctly against the struct and is wrong against the thumb: a
# profile saying "lstick_up" would fire on pushing down.
    "lstick_left": 16, "lstick_right": 17,
    "lstick_up": 18, "lstick_down": 19,
    "rstick_left": 20, "rstick_right": 21,
    "rstick_up": 22, "rstick_down": 23,
    "guide": 24,
}

ACTIONS = {
    "none": 0, "key": 1, "mouse_button": 2, "mouse_wheel": 3,
    "joy_button": 4, "joy_axis": 5, "joy_pov": 6, "mouse_pulse": 7,
    "layer_hold": 8, "layer_set": 9, "layer_cycle": 10,
}

FLAGS = {
    "repeat": 0x01, "toggle": 0x02, "analog": 0x04,
    "passthrough": 0x10,
}

STICK_MODES = {
    "off": 0, "mouse": 1, "absolute": 2, "joy": 3, "wheel": 4,
}

# --------------------------------------------------------------------------
# HID usages. THE DRIVER NEVER SEES A CHARACTER - 'w' is usage 0x1A, not
# ASCII 0x77. Keeping layout knowledge here is deliberate; see
# mapping-engine.txt section 3.
# --------------------------------------------------------------------------

KEYS = {}
for _i, _c in enumerate("abcdefghijklmnopqrstuvwxyz"):
    KEYS[_c] = 0x04 + _i
for _i, _c in enumerate("1234567890"):
    KEYS[_c] = 0x1E + _i
for _i in range(1, 13):
    KEYS["f%d" % _i] = 0x3A + _i - 1
KEYS.update({
    "enter": 0x28, "escape": 0x29, "esc": 0x29, "backspace": 0x2A,
    "tab": 0x2B, "space": 0x2C, "minus": 0x2D, "equals": 0x2E,
    "lbracket": 0x2F, "rbracket": 0x30, "backslash": 0x31,
    "semicolon": 0x33, "quote": 0x34, "grave": 0x35, "comma": 0x36,
    "period": 0x37, "slash": 0x38, "capslock": 0x39,
    "printscreen": 0x46, "scrolllock": 0x47, "pause": 0x48,
    "insert": 0x49, "home": 0x4A, "pageup": 0x4B, "delete": 0x4C,
    "end": 0x4D, "pagedown": 0x4E,
    "right": 0x4F, "left": 0x50, "down": 0x51, "up": 0x52,
    "lctrl": 0xE0, "lshift": 0xE1, "lalt": 0xE2, "lgui": 0xE3,
    "rctrl": 0xE4, "rshift": 0xE5, "ralt": 0xE6, "rgui": 0xE7,
    # THE KEYPAD IS ITS OWN SET OF USAGES: keypad Enter is 0x58
    # and the Enter above it is 0x28. A scan code tells them
    # apart, so they need names to be written back as.
    "numlock": 0x53, "kpslash": 0x54, "kpstar": 0x55,
    "kpminus": 0x56, "kpplus": 0x57, "kpenter": 0x58,
    "kp1": 0x59, "kp2": 0x5A, "kp3": 0x5B, "kp4": 0x5C,
    "kp5": 0x5D, "kp6": 0x5E, "kp7": 0x5F, "kp8": 0x60,
    "kp9": 0x61, "kp0": 0x62, "kpperiod": 0x63,
    "nonusbackslash": 0x64, "menu": 0x65,
})

MOUSE_BUTTONS = {"left": 1, "right": 2, "middle": 3, "x1": 4, "x2": 5}

# --------------------------------------------------------------------------
# Curves. The configurator owns the Bezier; the driver gets the table.
#
# X IS PINNED AT 1/3 AND 2/3, WHICH MAKES x(t) == t EXACTLY. That is the
# whole reason two numbers are enough: no root-finding is needed to invert
# the parametric form, so g(u) is a direct cubic evaluation.
# --------------------------------------------------------------------------

CURVE_PRESETS = {
    "linear": (1.0 / 3.0, 2.0 / 3.0),
    "quad": (0.0, 1.0 / 3.0),
    "cubic": (0.0, 0.0),
    "smoothstep": (0.0, 1.0),
}


def bezier_curve(y1, y2):
    """33 points of g(u) = 3(1-u)^2 u y1 + 3(1-u) u^2 y2 + u^3."""
    table = []
    for i in range(CURVE_POINTS):
        u = float(i) / (CURVE_POINTS - 1)
        v = (3.0 * (1.0 - u) ** 2 * u * y1 +
             3.0 * (1.0 - u) * u ** 2 * y2 +
             u ** 3)
        # HALF-UP, NOT round(): Python rounds halves to even, which
        # disagrees with the driver's integer default at u = 0.5.
        table.append(max(0, min(65535, int(v * 65535.0 + 0.5))))
    # The driver repairs a non-monotone table; do not hand it one.
    for i in range(1, CURVE_POINTS):
        if table[i] < table[i - 1]:
            table[i] = table[i - 1]
    return table


# --------------------------------------------------------------------------
# Model
# --------------------------------------------------------------------------

class Binding(object):
    def __init__(self):
        self.source = SA_NONE
        self.action = 0
        self.code = 0
        self.hard_at = 0
        self.flags = 0
        self.repeat_hz = 0
        self.repeat_delay_ms = 0

    def pack(self):
        return struct.pack(BINDING_FMT, self.source, self.action, self.code,
                           self.hard_at, self.flags, self.repeat_hz,
                           self.repeat_delay_ms, 0)


class Stick(object):
    def __init__(self):
        self.deadzone = 4000
        self.outer = 33000
        self.max_speed = 2800
        self.accel_threshold = 58000
        self.accel_rate = 0
        self.accel_max = 512
        self.accel_decay = 1024
        self.mode = 0
        self.gain_x = 128
        self.gain_y = 54
        self.invert_x = 0
        # 0: the decode already turns the pad's up-positive Y into
        # HID's down-positive one. Set 1 only for inverted aiming.
        self.invert_y = 0
        self.smooth_ms = 8
        self.curve = bezier_curve(*CURVE_PRESETS["linear"])

    def pack(self):
        return struct.pack(STICK_FMT, self.deadzone, self.outer,
                           self.max_speed, self.accel_threshold,
                           self.accel_rate, self.accel_max,
                           self.accel_decay, self.mode, self.gain_x,
                           self.gain_y, self.invert_x, self.invert_y,
                           self.smooth_ms, *self.curve)


class Layout(object):
    def __init__(self, index):
        self.bindings = [Binding() for _ in range(MAX_BINDINGS)]
        self.chords = [[SA_NONE] * CHORD_MEMBERS for _ in range(MAX_CHORDS)]
        self.led = index
        self.next_free = 0

    def add(self, binding):
        if self.next_free >= MAX_BINDINGS:
            raise ValueError("more than %d bindings in one layer"
                             % MAX_BINDINGS)
        self.bindings[self.next_free] = binding
        self.next_free += 1

    def pack(self):
        out = b"".join(b.pack() for b in self.bindings)
        for members in self.chords:
            out += struct.pack(CHORD_FMT, members[0], members[1],
                               members[2], 0)
        out += struct.pack("<B3x", self.led)
        return out


class Config(object):
    def __init__(self):
        self.collections = 0x07
        self.sticks = [Stick(), Stick()]
        self.layouts = [Layout(i) for i in range(MAX_LAYOUTS)]

    def pack(self):
        hdr = struct.pack(HDR_FMT, SIGNATURE, VERSION, HDR_SIZE,
                          LAYOUT_SIZE, STICK_SIZE, MAX_LAYOUTS,
                          MAX_BINDINGS, MAX_CHORDS, self.collections,
                          b"\0" * 16)
        out = hdr
        for st in self.sticks:
            out += st.pack()
        for lay in self.layouts:
            out += lay.pack()
        return out


# --------------------------------------------------------------------------
# The default profile, matching core_config_defaults() byte for byte.
# --------------------------------------------------------------------------

def default_config():
    cfg = Config()
    for index, lay in enumerate(cfg.layouts):
        # Chord 0 is Start plus Back - XBCD's gesture, and the one
        # combination no game claims.
        lay.chords[0] = [SOURCES["start"], SOURCES["back"], SA_NONE]

        # THE CYCLE BINDING GOES IN EVERY LAYOUT, or there is no way back.
        cycle = Binding()
        cycle.source = SA_CHORD_BASE + 0
        cycle.action = ACTIONS["layer_cycle"]
        cycle.code = 1
        lay.add(cycle)

        if index == 1:
            for slot, name in enumerate(("a", "b", "x", "y")):
                b = Binding()
                b.source = SOURCES[name]
                b.action = ACTIONS["joy_button"]
                b.code = slot + 1
                b.flags = FLAGS["repeat"]
                b.repeat_hz = 12
                lay.add(b)
    return cfg


# --------------------------------------------------------------------------
# Profile text
# --------------------------------------------------------------------------

def parse_code(action, word):
    # LAYERS ARE WRITTEN THE WAY THEY ARE DECLARED. A profile says
    # [layer 1] and [layer 2], so layer_hold and layer_set take those
    # same numbers; the driver indexes from zero. Without this the
    # obvious "layer_hold 2" silently clamps to the layer below and the
    # hold appears to do nothing. layer_cycle is NOT converted - its
    # argument is a signed step, not an index.
    if action in (ACTIONS["layer_hold"], ACTIONS["layer_set"]):
        n = int(word, 0)
        if n < 1 or n > MAX_LAYOUTS:
            raise ValueError("layer must be 1..%d" % MAX_LAYOUTS)
        return n - 1

    if action == ACTIONS["key"]:
        key = KEYS.get(word.lower())
        if key is not None:
            return key
        # A BARE USAGE IS ACCEPTED TOO. Capture can reach keys the name
        # table does not carry, and the writer already emits "key 0x54"
        # for them; without this a profile could be saved and not read
        # back. The range is the driver's - core_cfg_key_ok.
        try:
            n = int(word, 0)
        except ValueError:
            n = -1
        if 0x04 <= n <= 0xA4 or 0xE0 <= n <= 0xE7:
            return n
        raise ValueError("'%s' is not a key name or a usage in"
                         " 0x04..0xA4 or 0xE0..0xE7" % word)
    if action == ACTIONS["mouse_button"]:
        if word.lower() in MOUSE_BUTTONS:
            return MOUSE_BUTTONS[word.lower()]
    return int(word, 0) & 0xFFFF


def parse_members(text):
    """'a+b+x' -> a padded list of source indices."""
    members = []
    for part in text.split("+"):
        name = part.strip().lower()
        if not name:
            continue
        if name not in SOURCES:
            raise ValueError("unknown control '%s'" % name)
        members.append(SOURCES[name])
    if not members:
        raise ValueError("members names no control")
    if len(members) > CHORD_MEMBERS:
        raise ValueError("a chord takes at most %d controls" % CHORD_MEMBERS)
    return members + [SA_NONE] * (CHORD_MEMBERS - len(members))


def parse_binding(text, chords):
    """'a -> key w repeat 12 delay 300'"""
    left, _, right = text.partition("->")
    source_name = left.strip().lower()
    words = right.split()
    if not words:
        raise ValueError("no action in '%s'" % text)

    b = Binding()

    # A CHORD IS NOT WRITTEN ON A BINDING LINE. It has a section of its
    # own, which is what gives the two chord buttons a slot they can
    # count on.
    if "+" in source_name:
        raise ValueError("a chord goes in its own section now - put"
                         " 'members = %s' under [layer N chord 1]"
                         % source_name)
    if source_name not in SOURCES:
        raise ValueError("unknown source '%s'" % source_name)
    b.source = SOURCES[source_name]
    return parse_action(words, b)


def parse_action(words, b):
    """The right-hand side, shared with a chord section's 'action ='."""
    if not words:
        raise ValueError("no action")

    action_name = words[0].lower()
    if action_name not in ACTIONS:
        raise ValueError("unknown action '%s'" % action_name)
    b.action = ACTIONS[action_name]

    i = 1
    if b.action != ACTIONS["none"]:
        if i >= len(words):
            raise ValueError("action '%s' needs a value" % action_name)
        b.code = parse_code(b.action, words[i])
        i += 1

    while i < len(words):
        word = words[i].lower()
        if word in FLAGS and word != "repeat":
            b.flags |= FLAGS[word]
            i += 1
        elif word == "repeat":
            b.flags |= FLAGS["repeat"]
            b.repeat_hz = int(words[i + 1], 0)
            i += 2
        elif word == "delay":
            b.repeat_delay_ms = int(words[i + 1], 0)
            i += 2
        elif word == "hard":
            # How hard to press before the repeat runs. Only means
            # anything alongside "repeat".
            #
            # A PERCENTAGE IS THE SANE WAY TO WRITE THIS. The raw unit
            # is the 0..MAX_VALUE scale every analog value is decoded
            # onto, NOT the 0..255 byte the pad sends, and the two are
            # off by a factor of 137. Writing the byte you meant gives
            # a hard point down near zero, which does not fail - it
            # autofires from the lightest touch and looks like 'hard'
            # being ignored.
            arg = words[i + 1]
            if arg.endswith("%"):
                pct = float(arg[:-1])
                if not 0.0 <= pct <= 100.0:
                    raise ValueError("hard must be 0%..100%")
                b.hard_at = int(round(MAX_VALUE * pct / 100.0))
            else:
                b.hard_at = int(arg, 0)
                if b.hard_at > MAX_VALUE:
                    raise ValueError("hard must be 0..%d or a percentage"
                                     % MAX_VALUE)
            i += 2
        else:
            raise ValueError("unknown option '%s'" % words[i])

    if b.hard_at and not (b.flags & FLAGS["repeat"]):
        raise ValueError("'hard' needs 'repeat' - it is the pressure the"
                         " autofire starts at, not an activation point")
    return b


def parse_profile(text):
    cfg = Config()
    section = None
    stick = None
    layout = None
    ch_layer = 0
    ch_slot = 0
    ch_all = False
    ch_action = None

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#")[0].strip()
        if not line:
            continue
        try:
            if line.startswith("[") and line.endswith("]"):
                parts = [w.lower() for w in line[1:-1].split()]
                section = parts[0] if parts else ""

                # [layer cycle] and [layer hold] ARE NOT PER LAYER. A
                # control that changes layer has to exist in the layer
                # it lands in or there is no way back, and a section
                # that cannot be written for one layer alone cannot get
                # that wrong.
                if (len(parts) == 2 and parts[0] == "layer" and
                        parts[1] in ("cycle", "hold")):
                    ch_all = True
                    ch_layer = 0
                    ch_action = (ACTIONS["layer_cycle"]
                                 if parts[1] == "cycle"
                                 else ACTIONS["layer_hold"])
                    ch_slot = (CHORD_SLOT_CYCLE if parts[1] == "cycle"
                               else CHORD_SLOT_HOLD)
                    section = "chord"
                    continue

                # [layer N chord M] - M names the slot outright.
                if (len(parts) == 4 and parts[0] == "layer" and
                        parts[2] == "chord"):
                    ch_layer = int(parts[1]) - 1
                    if not 0 <= ch_layer < MAX_LAYOUTS:
                        raise ValueError("layer must be 1..%d" % MAX_LAYOUTS)
                    m = int(parts[3])
                    if not 1 <= m <= CHORD_BUTTONS:
                        raise ValueError("chord must be 1..%d"
                                         % CHORD_BUTTONS)
                    ch_slot = m - 1
                    ch_all = False
                    ch_action = None
                    section = "chord"
                    continue

                if section == "layer":
                    index = int(parts[1]) - 1
                    if not 0 <= index < MAX_LAYOUTS:
                        raise ValueError("layer must be 1..%d" % MAX_LAYOUTS)
                    layout = cfg.layouts[index]
                elif section == "stick":
                    stick = cfg.sticks[0 if parts[1] == "left" else 1]
                elif section != "global":
                    raise ValueError("unknown section '%s'" % section)
                continue

            if section == "chord":
                key, _, value = line.partition("=")
                key = key.strip().lower()
                value = value.strip()
                targets = (range(MAX_LAYOUTS) if ch_all else [ch_layer])

                if key == "members":
                    members = parse_members(value)
                    named = [m for m in members if m != SA_NONE]
                    for li in targets:
                        lay = cfg.layouts[li]
                        # ONE CONTROL IS NOT A CHORD: binding it through
                        # a slot would make the driver wait for a second
                        # control that does not exist.
                        if len(named) > 1:
                            lay.chords[ch_slot] = members
                        if ch_action is not None:
                            b = Binding()
                            b.source = (SA_CHORD_BASE + ch_slot
                                        if len(named) > 1 else named[0])
                            b.action = ch_action
                            b.code = (1 if ch_action == ACTIONS["layer_cycle"]
                                      else MAX_LAYOUTS - 1)
                            lay.add(b)
                elif key == "action":
                    named = [m for m in cfg.layouts[ch_layer].chords[ch_slot]
                             if m != SA_NONE]
                    if len(named) < 2:
                        raise ValueError("'members' has to come before"
                                         " 'action'")
                    src = SA_CHORD_BASE + ch_slot
                    lay = cfg.layouts[ch_layer]
                    b = Binding()
                    b.source = src
                    lay.add(parse_action(value.split(), b))
                else:
                    raise ValueError("a chord section takes 'members'"
                                     " and 'action'")
            elif section == "global":
                # [global] HAS NO SETTINGS. An empty section header still
                # parses, so an existing profile is not broken by its
                # presence, but a key in it is an error rather than a
                # silent no-op: the tick period is a property of the
                # driver, not of a profile.
                key, _, _value = line.partition("=")
                raise ValueError("unknown setting '%s' - [global] has no"
                                 " settings" % key.strip())
            elif section == "stick":
                key, _, value = line.partition("=")
                key = key.strip().lower()
                value = value.strip()
                if key == "mode":
                    stick.mode = STICK_MODES[value.lower()]
                elif key == "curve":
                    words = value.split()
                    if words[0].lower() in CURVE_PRESETS:
                        y1, y2 = CURVE_PRESETS[words[0].lower()]
                    else:
                        y1, y2 = float(words[0]), float(words[1])
                    stick.curve = bezier_curve(y1, y2)
                else:
                    setattr(stick, key, int(value, 0))
            elif section == "layer":
                layout.add(parse_binding(line, layout.chords))
            else:
                raise ValueError("line outside any section")
        except Exception as exc:
            raise SystemExit("%d: %s\n    %s" % (lineno, exc, raw.strip()))
    return cfg


# --------------------------------------------------------------------------
# Dump, for reading a blob back
# --------------------------------------------------------------------------

def dump(blob):
    names = dict((v, k) for k, v in SOURCES.items())
    actions = dict((v, k) for k, v in ACTIONS.items())
    keys = dict((v, k) for k, v in KEYS.items())

    hdr = struct.unpack(HDR_FMT, blob[:HDR_SIZE])
    (sig, ver, hbytes, lbytes, sbytes, lcount, bcount, ccount,
     coll, _) = hdr
    print("signature   %08X %s" % (sig, "ok" if sig == SIGNATURE else "BAD"))
    print("version     %d" % ver)
    print("strides     header %d  layout %d  stick %d"
          % (hbytes, lbytes, sbytes))
    print("counts      layouts %d  bindings %d  chords %d"
          % (lcount, bcount, ccount))
    print("collections %s%s%s"
          % ("gamepad " if coll & 1 else "",
             "keyboard " if coll & 2 else "",
             "mouse" if coll & 4 else ""))
    print("total       %d bytes" % len(blob))

    off = hbytes
    for i in range(STICK_COUNT):
        st = struct.unpack(STICK_FMT, blob[off:off + STICK_SIZE])
        mode = dict((v, k) for k, v in STICK_MODES.items())[st[7]]
        print("\n[stick %s] mode %s  deadzone %d  outer %d  speed %d"
              % ("left" if i == 0 else "right", mode, st[0], st[1], st[2]))
        print("    curve %s ... %s" % (st[13:16], st[-3:]))
        off += sbytes

    for layer in range(lcount):
        print("\n[layer %d]" % (layer + 1))
        base = off
        chord_at = base + MAX_BINDINGS * BINDING_SIZE
        chords = []
        for c in range(MAX_CHORDS):
            at = chord_at + c * CHORD_SIZE
            chords.append(struct.unpack(CHORD_FMT, blob[at:at + CHORD_SIZE]))
        for i in range(MAX_BINDINGS):
            at = base + i * BINDING_SIZE
            (src, act, code, hard_at, flags, hz,
             delay, _rsv) = struct.unpack(BINDING_FMT,
                                          blob[at:at + BINDING_SIZE])
            if act == 0:
                continue
            if src >= SA_CHORD_BASE:
                members = chords[src - SA_CHORD_BASE]
                label = "+".join(names.get(m, "?") for m in members[:3]
                                 if m != SA_NONE)
            else:
                label = names.get(src, "?%d" % src)
            aname = actions.get(act, "?")
            if act == ACTIONS["key"]:
                value = keys.get(code, "0x%02X" % code)
            elif act in (ACTIONS["layer_hold"], ACTIONS["layer_set"]):
                value = str(code + 1)       # shown as written
            else:
                value = str(code)
            extra = ""
            for fname, bit in sorted(FLAGS.items()):
                if flags & bit and fname != "repeat":
                    extra += " " + fname
            if flags & FLAGS["repeat"]:
                extra += " repeat %d" % hz
            if delay:
                extra += " delay %d" % delay
            if hard_at:
                extra += " hard %.0f%%" % (hard_at * 100.0 / MAX_VALUE)
            print("    %-14s -> %s %s%s" % (label, aname, value, extra))
        off += lbytes


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("profile", nargs="?", help="profile text to compile")
    ap.add_argument("-o", "--out", help="write the blob here")
    ap.add_argument("--default", action="store_true",
                    help="emit the built-in default configuration")
    ap.add_argument("--dump", metavar="BLOB", help="print a blob as text")
    args = ap.parse_args()

    if args.dump:
        with open(args.dump, "rb") as handle:
            dump(handle.read())
        return 0

    if args.default:
        cfg = default_config()
    elif args.profile:
        with open(args.profile, "r") as handle:
            cfg = parse_profile(handle.read())
    else:
        ap.print_help()
        return 2

    blob = cfg.pack()
    expected = HDR_SIZE + STICK_COUNT * STICK_SIZE + MAX_LAYOUTS * LAYOUT_SIZE
    assert len(blob) == expected, (len(blob), expected)

    if args.out:
        with open(args.out, "wb") as handle:
            handle.write(blob)
        print("  %s, %d bytes" % (args.out, len(blob)))
    else:
        dump(blob)
    return 0


if __name__ == "__main__":
    sys.exit(main())
