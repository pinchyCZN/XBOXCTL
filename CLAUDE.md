# XBOXCTL

A configurable HID minidriver for the original Xbox controller. The pad is
presented to Windows as a **composite device - keyboard, mouse and gamepad in
one** - so any control can be remapped to a real keystroke or mouse event that
every application sees as genuine hardware.

The architecture is the Adaptoid's (`../ADAPTOID`): own the device, speak its
vendor protocol, manufacture a composite HID descriptor, push reports.
**Without the Adaptoid's bytecode scripting engine.** Remapping is a table of
fixed-size parameter records pushed to the driver, not a program.

## The property the whole project exists for

Remapped output must be **indistinguishable from real hardware** to every
application, including ones that ignore injected input. User-mode remappers
call `SendInput`; games reading DirectInput, Raw Input or `GetAsyncKeyState`
frequently do not see that at all. Our keystrokes arrive through kbdclass
exactly as a real keyboard's do.

Anything that trades this away is the wrong answer, however much simpler.

## Layout

| Path | What it is |
| --- | --- |
| `docs/` | Design and analysis, RFC-style ASCII `.txt` |
| `src_drv/` | The driver and its test harness, one VS2017 solution |
| `src_drv/orig/` | **XBCD 0.2.6 source. Reference only - see below.** |
| `tools/` | Documentation and source formatters |

## src_drv/orig is reference, not a base

XBCD is the 2005 GPL Xbox pad driver. We are **not** modifying it and **not**
porting it. It is here for one purpose: it documents the 20-byte packet the pad
emits, and that is a fact about hardware.

- **Never modify, move, rename or rebuild anything under `src_drv/orig/`.**
- **Do not paste XBCD code into the replacement.** Data layouts, register
  values, packet offsets and protocol constants are facts and may be used
  freely. Its *expression* - functions, structures, control flow, comments -
  may not.
- XBCD is GPLv2. Copying its code makes this project GPLv2. Keeping to facts
  keeps the licence question open. This is a project constraint to be decided
  deliberately, not something to drift into.

## The core seam

`core.c` and `core.h` are the engine: packet decode, bindings, curves,
autofire, report state. **Nothing in them may include a Windows or DDK header,
call a kernel API, or name a Windows type.** Everything from the outside world
arrives through the declared seams - a report sink and a clock passed in as an
argument.

That rule is load-bearing, not stylistic. It is what lets the same source build
into `xboxctl.sys` and into the `xboxctl.exe` harness, and what makes autofire
timing, the mouse accumulator and the acceleration ramp deterministic and
steppable in a console program rather than guesswork against a kernel debugger.

`wdm.c` is the only file that may name a kernel type. It compiles twice: against
the real DDK headers for the driver, and against `kstub.h` for the harness.

## Git - read only

Allowed: `git log`, `git show`, `git diff`, `git status`, `git blame`, and
reading files.

Do **not** run: `commit`, `add`, `push`, `pull`, `fetch`, `merge`, `rebase`,
`reset`, `checkout`/`switch`, `branch`, `tag`, `stash`, `clean`, or anything
that writes to `.git/`. The user drives all git operations.

Creating and editing files in the working tree is fine - that is not a git
operation.

## Documentation: plain ASCII, RFC style

Documents live in `docs/` and `src_drv/README.txt` as `.txt`. `CLAUDE.md` is the
single exception - Claude Code requires that name, and markdown is correct here
because the harness renders it.

**Plain ASCII only.** Every byte in every document, source file and comment must
be `<= 0x7F`. No em dashes, curly quotes, arrows, ellipsis glyphs, accented
letters or box-drawing characters. Write `->` not an arrow, `-` not an em dash,
`...` not an ellipsis, and use `+` `-` `|` for diagrams.

**Laid out like an old RFC, not like markdown:**

```
XBOXCTL                                                       Design Note
subject                                                       qualifier


                          DOCUMENT TITLE


Abstract

   Body text indented three spaces, wrapped at 80 columns.

1.  First Section

   +--------------+-------+
   | Column       | Count |
   +==============+=======+
   | aligned with | 2542  |
   +--------------+-------+
```

- No markdown syntax in a `.txt` file: no `#` headings, no `**bold**`, no
  backticks, no `[text](link)`, no `|`-delimited markdown tables. If it needs a
  renderer, it is wrong.
- Tables are drawn with `+---+` borders and space-aligned columns, `+===+`
  under the header row. Every `|` row must be exactly as wide as the border.
- Numbered sections (`1.`, `1.1.`) with a Table of Contents once a document
  warrants one.
- **Body text indented three spaces, wrapped at 80 columns. Tables may run to
  120** including their indent.
- Blocks indented four or more spaces are preformatted - descriptor hex dumps,
  register maps, diagrams - and are never rewrapped.

```
python tools/asciify.py --check    ASCII, line width, table alignment
python tools/asciify.py --fix      transliterate and rewrap in place
python tools/tblfix.py PATH        re-align a table block to its widest row
```

Run `asciify.py --check` before treating any document as finished.

### Documents state the current facts. They are not a development log.

A document describes **what is true now**. Nothing in it records how the
understanding got there: no "CORRECTION", no "RETRACTED", no "an earlier
revision said", no "this was previously recorded as", no before-and-after, no
"we decided to change X to Y".

When something turns out to be wrong, **delete the wrong statement and write
the right one**. The old claim does not get a farewell paragraph. One fact has
one home; everything else points at it rather than restating it.

**The single exception is a trap.** History may be recorded only when a reader
who does not know it would repeat the mistake -- an approach that looks correct,
was tried, and failed for a reason that is not visible from the result. The test
is whether the note prevents a future error, not whether it is interesting:

- "The pad reports Y up-positive, so HID needs it negated" -- a fact. State it,
  no history.
- "`link 9.00` rejects `/DEBUG:FASTLINK`, which four separate v141 defaults key
  off `UseDebugLibraries`, so that property is false even in Debug" -- a trap.
  Someone would set it back and get an unreadable error. Keep it.
- "This field was originally called LayoutNr" -- not a trap. The field is named
  correctly now and nothing leads a reader back to the old name. Delete it.

A retraction is only ever warranted for a claim **that still looks true from
the evidence** -- where a reader examining the same code would reach the
discarded conclusion again. Write that as a positive statement of why the
obvious reading is wrong, not as a record that someone once believed it.

**The same applies to source comments.** A comment says what the code does and
why. It never says what the code used to do, what was tried first, or when
something changed.

## Source code: tabs for indentation, spaces for alignment

**In `src_drv/` only, excluding `orig/`.** One indent level is one TAB,
displayed as four columns. Everything that is not structural indentation stays
SPACES:

- a continuation line lined up under an open paren;
- the `* ` of a block comment;
- an ASCII diagram or a hex dump inside a comment;
- columns in an initialiser or a run of aligned assignments.

That split is the whole point - it makes the source render correctly at any tab
width instead of only at four.

```
python tools/retab.py --check    report, exit 1 if anything drifted
python tools/retab.py --fix      convert in place
```

**`docs/`, `src_drv/README.txt`, `src_drv/orig/` and `tools/` do NOT change.**
The RFC documents are specified in spaces and `asciify.py` counts their display
columns; the Python is PEP 8. `.editorconfig` encodes every case.

Two traps, both already hit on the projects this convention comes from:

- **`asciify.py` measures DISPLAY columns**, expanding tabs to four. Counting
  characters instead would let a deeply nested line run well past 80 real
  columns and still pass.
- **A generator that emits spaces silently undoes this.** Check any new script
  that writes into `src_drv/` the same way.

## Fixed-width types everywhere

The configuration blob is written by a 32-bit configurator and read by a 64-bit
driver, and the harness compiles the same structures a third way. Use `u8`,
`u16`, `u32`, `s16`, `s32`, `u64` from `core.h` with explicit padding.

**Never a bitfield, never an `enum`, never a pointer, never a plain `int`** in
anything that crosses a boundary - a report, the config blob, or an IOCTL
buffer.

## After a driver fix, repackage and stage it. Every time.

**Any change under `src_drv/` that reaches the driver ends with
`tools/package.cmd`** - build, harness, DriverVer stamp, sign, catalogue,
stage to `B:\xboxctl\pkg`. Do not wait to be asked, and do not spend a turn
asking whether to.

The user tests in a VM that reads `B:`. A fix sitting in the build tree is a
fix nobody can try, and "shall I package it?" costs a round trip for a
command that takes seconds and is safe to run at any time.

```
python tools/kdc.py ".reload /u xboxctl.sys"    release the PDB first
tools\package.cmd                                build, test, sign, stage
```

**Unload the symbols first if the debugger has been used.** kd holds
`xboxctl.sys.pdb` open and the linker then fails with `LNK1201`, whose text
blames disk space, path and privilege and names none of the real cause.

`package.cmd` runs the harness and **aborts on any failed check**, so a
staged package is always one that passed. Say what was staged and what the
user has to do next in the guest - usually `deploy.cmd` then `state.cmd`,
and a replug when the descriptor changed.

## Working rules

- **State evidence, not guesses.** Back a claim with a file and line, a
  descriptor byte, a header, or a citation into `../ADAPTOID/docs`. Mark
  inference as inference.
- **The descriptor is the contract.** Changing an item in the report descriptor
  changes the device's identity to every application and invalidates saved
  bindings. Treat it as frozen once released.
- **No floating point in the driver.** Everything is integer fixed point on a
  35000-unit scale. Curves arrive as a precomputed table, so the driver never
  evaluates one.
- **Build both projects before calling anything done.** The harness catches what
  a kernel debugger would otherwise have to.
