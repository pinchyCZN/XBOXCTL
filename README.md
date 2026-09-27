# XBOXCTL

A configurable HID minidriver for the **original Xbox controller** on
Windows.

The pad is presented to Windows as a **composite device - a gamepad, a
keyboard and a mouse from one USB endpoint** - so any control on it can be
remapped to a real keystroke or mouse event.

```
  Xbox pad --USB--> xboxctl.sys --+--> gamepad   --> DirectInput / XInput
                                  |
                                  +--> keyboard  --> kbdclass --> every app
                                  |
                                  +--> mouse     --> mouclass --> every app
```

## Why bother, when user-mode remappers exist

Because they are not the same thing.

A user-mode remapper calls `SendInput`. Games reading DirectInput, Raw
Input or `GetAsyncKeyState` frequently **do not see injected input at
all**, and there is nothing the remapper can do about it.

XBOXCTL's keystrokes arrive through `kbdclass` exactly as a real keyboard's
do, because as far as Windows is concerned a real keyboard is what produced
them. Every application sees them, including the ones that deliberately
ignore synthetic input.

That property is the entire reason this is a driver and not a tray
application.

## Where it comes from

Two ancestors, and it takes a different thing from each.

**XBCD** (2005) was the original third-party Xbox pad driver for Windows.
XBOXCTL is a **from-scratch reimplementation**, not a fork and not a port.
The one thing taken from XBCD is a fact about hardware: the layout of the
20-byte packet the pad emits, and how to decode it. **No XBCD code is
present in this repository** - see [Licence](#licence).

**The Adaptoid** was an N64-to-USB adapter whose driver did genuinely
sophisticated remapping, including analog-stick-to-mouse. Its architecture is
the model here: own the device, speak its protocol, manufacture a composite
HID descriptor, push reports.

**Minus the Adaptoid's bytecode scripting engine.** That is the deliberate
divergence. The Adaptoid compiled user scripts to bytecode and ran a VM in
the driver. XBOXCTL's remapping is instead a **table of fixed-size
parameter records** - 12 bytes per binding - pushed to the driver as a
1048-byte blob. The driver interprets parameters, never a program.

The bet is that a well-chosen parameter set covers essentially every
remapping anyone actually wants, without putting an interpreter in kernel
mode. The configuration is data, it is bounded, it is validated at the
boundary, and a malformed profile is a rejected IOCTL rather than a machine
check.

## What you can map

Any control - buttons, triggers, D-pad, stick directions - to any of:

| Action | |
| --- | --- |
| `key` | a real keystroke, any HID usage |
| `mouse_button` | left, right, middle, and both side buttons |
| `mouse_wheel` | wheel detents |
| `joy_button` | a gamepad button |
| `joy_axis`, `joy_pov` | a gamepad axis or hat direction |
| `layer_hold`, `layer_set`, `layer_cycle` | switch between layouts |

Plus, per binding: **autofire** at a chosen rate, a **pressure threshold**
so an analog trigger can do one thing pressed and another squeezed hard,
and **passthrough** so a remapped control also keeps its gamepad function.

Either analog stick can be handed to the mouse pointer instead of acting as
a joystick axis, with a shaping curve and acceleration. That part has real
depth to it and is documented separately in
[`docs/analog-to-mouse.txt`](docs/analog-to-mouse.txt).

**Two layers**, and **two chords per layer**. A chord is two or three
controls that do something only when held together.

## Status

Working. The engine has an offline test harness that passes **412 checks**,
and the driver runs on hardware.

Supported hardware IDs are `VID_045E` with PID `0202`, `0285`, `0287`,
`0288` and `0289` - the original Xbox pads and the S controller.

This is a personal project, developed against a VM with a kernel debugger
attached. It is not signed for public distribution: see
[Deploying](#deploying).

## Building

You need **Visual Studio 2017** and, for the driver only, the **WDK 7.1**.

The driver uses the WDK 7.1 toolchain (`cl` 15.00, `link` 9.00) because
that is what its kernel headers were written against. Everything else
builds with the ordinary v141 toolset. `src_drv/common.props` is the only
place the WDK location is named.

```bat
rem the driver, plus the harness that tests its engine
msbuild src_drv\xboxctl.sln /p:Configuration=Release /p:Platform=x64

rem the configurator
msbuild src_cfg\xbconfig.sln /p:Configuration=Release /p:Platform=Win32
```

Artefacts land under `build\<Platform>\<Configuration>\`.

Run the harness before trusting a build. It exercises the packet decode,
binding evaluation, layers, chords, autofire, the stick pipeline and the
configuration blob, with no driver loaded and no hardware attached:

```bat
src_drv\build\x64\Release\xboxctl.exe
```

`tools\package.cmd` does the whole release loop in one step - build, run
the harness, stamp `DriverVer`, sign, build the catalogue, and stage the
package to a folder. **It aborts if any harness check fails**, so a staged
package is always one that passed.

## Deploying

> **Windows 10 and 11 on x64 will not load an unsigned driver.** For
> development that means test signing plus a self-signed certificate. For
> anything you intend to share, attestation signing through Partner Center.

Once, on the machine you build on:

```bat
tools\mktestcert.cmd
```

Once, on the machine you test on, from an **elevated** prompt - installs the
certificate and checks testsigning and Secure Boot:

```bat
trustcert.cmd
```

Then per build, on the test machine, elevated:

```bat
deploy.cmd             rem remove old nodes and packages, install, rescan
state.cmd              rem what is actually installed and bound
undeploy.cmd           rem back to a clean slate
```

`package.cmd` copies those scripts and the certificate next to the staged
package, so the test machine needs no view of the source tree.

Installing by hand instead is just
`pnputil /add-driver xboxctl.inf /install`.

**Expect three child device nodes.** `hidclass` creates one per top-level
collection, in descriptor order:

```
HID\VID_045E&PID_0289&Col01    mouse
HID\VID_045E&PID_0289&Col02    keyboard
HID\VID_045E&PID_0289&Col03    gamepad
```

An *unsuffixed* node is what a single-collection device produces. If you see
one after installing this driver, it is a leftover from whatever owned the
pad before, and it will quietly outrank the new package - driver selection
is sticky per node. `deploy.cmd` removes the ones it can find; Device
Manager with **Show hidden devices** turned on shows the rest.

## The configurator

`xbconfig.exe` is where you set the mapping. It is a plain Win32 dialog,
written in C, that calls no interpreter and reads no registry key.

It shows a **button for every control on the pad**. Click one and you get a
dialog for what that control should do - pick an action, and for a keystroke
press **Capture** and then press the key you want. Capture reads Raw Input,
so it can tell left Shift from right Shift, and the keypad from the number
row.

There is **no Save button**. Clicking OK in a binding dialog writes the
profile. A configurator that can be left holding unsaved work is one that
loses it.

Alongside the grid: a list of profiles to switch between, a layer selector,
and buttons to push the current profile to the pad or clear the pad back to
its built-in default.

| Key | |
| --- | --- |
| `F1` | this list |
| `F2` | switch layer |
| `F3` | push the configuration to the pad |
| `F4` | clear the pad - back to the driver's default, profile untouched |
| `F5` | reread the profiles folder |

**Everything lives beside the executable.** Profiles are `.txt` files in a
`profiles\` folder next to `xbconfig.exe`, and `xbconfig.ini` holds the
window position and which profile was open. Nothing goes to the registry or
to AppData, so the whole thing can be copied to a stick and carried to the
test machine.

Copy the executable to where you want the profiles to live - **not** the
build directory, which the next build empties without warning.

## Profiles

A profile is a text file. The configurator reads and writes it, and you can
edit it by hand. This is `profiles/arcade.txt`, in full:

```ini
[stick left]
mode = off
deadzone = 8000

[stick right]
mode = off
deadzone = 8000
curve = linear          # linear | quad | cubic | smoothstep | <y1> <y2>

[layer cycle]
members = start+back

[layer 1]
a -> key w

[layer 2]
a -> joy_button 1 repeat 12
b -> joy_button 2 repeat 12
x -> joy_button 3 repeat 12
y -> joy_button 4 repeat 12
```

Layer 1 is the plain pad with `A` also typing `w`. Layer 2 is the same pad
with the four face buttons autofiring at 12 Hz. Start plus Back flips
between them.

**A control you do not name keeps its default gamepad function.** Binding
one replaces that default; add `passthrough` to a binding to get both.

The profile text format is the configurator's own - the driver never sees
it. What reaches the driver is the compiled blob.

## Command-line tools

The configurator has a command line, which is how its parser is tested:

```bat
xbconfig --compile in.txt out.bin      rem profile -> blob
xbconfig --rewrite in.txt out.txt      rem round trip
```

`tools\xbctl.ps1` talks to the driver's control device directly - useful for
seeing what the pad is doing without a debugger:

```
xbctl.ps1 version              what the driver speaks
xbctl.ps1 devices              which pads it can see
xbctl.ps1 stats                packet and report counters
xbctl.ps1 get  -Path out.bin   read the live configuration back
xbctl.ps1 set  -Path in.bin    install a configuration
xbctl.ps1 reset                back to the built-in default
xbctl.ps1 rumble 200 100       shake it: left, right, 0..255
xbctl.ps1 rumble 0 0           stop
xbctl.ps1 trace                the last 32 mouse reports emitted
xbctl.ps1 raw [-Watch]         the last packet, untranslated
```

`raw` is the pad **before anything is done to it** - no deadzone, no curve,
no binding. It is how you find out which control you just pressed, and it
works even for a control already bound to a key.

`trace` is the only view of the pointer path on a live system: `mouhid`
opens the mouse collection exclusively, so reading those reports from user
mode returns `ACCESS_DENIED`. The driver records what it emitted instead.

## Repository layout

| Path | |
| --- | --- |
| `docs/` | Design and analysis, RFC-style plain ASCII |
| `src_drv/` | The driver and its test harness, one VS2017 solution |
| `src_drv/orig/` | Empty. Where a local XBCD reference copy goes |
| `src_cfg/` | The configurator, one VS2017 solution |
| `profiles/` | Example profiles |
| `tools/` | Build, sign, deploy, debug and formatting scripts |

The driver's engine - packet decode, bindings, curves, autofire, report
state - lives in `core.c` and `core.h`, which **may not include a Windows or
DDK header or name a Windows type**. That is what lets the same source build
into both `xboxctl.sys` and the offline harness, and it is why the autofire
timing and the mouse accumulator can be stepped through in a console
program instead of guessed at against a kernel debugger.

Start with [`docs/README.txt`](docs/README.txt), which indexes the rest.

## Licence

**GNU General Public License, version 3.** The full text is in
[`COPYING`](COPYING).

    Copyright (C) 2026 pinchyCZN

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

### On XBCD

XBOXCTL contains **no XBCD code**. What was taken from XBCD is a set of
facts about the hardware - packet offsets, register values and protocol
constants - which are not copyrightable expression. This is not a
derivative work of it.

Even if that were arguable, it would not be a difficult question: XBCD is
offered under the GNU GPL, *"either version 2 of the License, or (at your
option) any later version"*. Version 3 is one of those later versions.

`src_drv/orig/` is where a local copy of the XBCD 0.2.6 source can be
unpacked in order to read it as a hardware reference. Nothing in that
folder is committed, compiled, linked or shipped.
