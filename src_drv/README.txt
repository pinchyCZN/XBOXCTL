XBOXCTL                                                     Source Note
src_drv/                                              build and structure


                    THE DRIVER AND ITS HARNESS


Abstract

   One Visual Studio 2017 solution, two projects, one set of C sources.
   driver.vcxproj builds xboxctl.sys; harness.vcxproj builds xboxctl.exe
   from the same core.c and wdm.c with XBOXCTL_USERMODE defined.

   This note records how the tree is laid out, how it builds, what each
   file holds and which seams are wired in only one of the two builds.

   The design the code implements is in ../docs/driver-plan.txt. This
   note is about the code and the build, not about the design.

Table of Contents

   1.  Layout
   2.  Building
   3.  The Core Seam
   4.  Why The Driver Uses The WDK 7.1 Toolchain
   5.  Settings That Are Not Obvious
   6.  Installing It
   7.  What Is Built And What Is Stubbed
   8.  orig/

1.  Layout

   +----------------+-----------------------------------------------------+
   | File           | Holds                                               |
   +================+=====================================================+
   | core.h, core.c | The engine. Packet decode into semiaxes, the radial |
   |                | math, the composite HID descriptor, the report      |
   |                | builders and the keyboard and mouse state machines. |
   |                | NO OS CALLS AND NO WINDOWS TYPES.                   |
   +----------------+-----------------------------------------------------+
   | wdm.h, wdm.c   | The OS-facing layer. DriverEntry, AddDevice, PnP,   |
   |                | power, the HID minidriver IOCTL surface, the        |
   |                | two-slot poll engine, the report queue and the      |
   |                | pending-read list. The only file that may name a    |
   |                | kernel type.                                        |
   +----------------+-----------------------------------------------------+
   | kstub.h        | A fake kernel ABI, just large enough to compile     |
   |                | wdm.c in user mode. Included by wdm.h when          |
   |                | XBOXCTL_USERMODE is defined.                        |
   +----------------+-----------------------------------------------------+
   | harness.c      | main(), the kstub implementations and the tests.    |
   +----------------+-----------------------------------------------------+
   | xboxctl.inf    | Installs the driver as a HIDClass minidriver.       |
   +----------------+-----------------------------------------------------+
   | common.props   | Shared MSBuild settings. The only place the WDK     |
   |                | location is named.                                  |
   +----------------+-----------------------------------------------------+
   | orig/          | XBCD 0.2.6 source. Reference only; see section 8.   |
   +----------------+-----------------------------------------------------+

   THE FILE SET IS FIXED AT FOUR C SOURCES plus two headers. The binding
   table, the curve pipeline and the configuration blob all have a home in
   core.c, and the configuration transport has one in wdm.c. Nothing
   outside this list is to be created without a deliberate decision to
   change the plan - a driver of this size drifts into a file per
   subsystem if each one is decided on its own.

2.  Building

   Open xboxctl.sln in Visual Studio 2017, or from a command line:

       msbuild xboxctl.sln /p:Configuration=Release /p:Platform=x64
       msbuild xboxctl.sln /p:Configuration=Release /p:Platform=Win32

   Four configurations exist for each project: Debug and Release, Win32
   and x64. All eight combinations build.

   Artefacts land under build/:

       build\<Platform>\<Configuration>\xboxctl.sys    the driver
       build\<Platform>\<Configuration>\xboxctl.exe    the harness
       build\obj\<project>\<Platform>\<Configuration>  intermediates

   Run the harness to exercise the engine. It exits non-zero if any check
   fails, so it drops straight into a build script:

       build\x64\Release\xboxctl.exe

   THE WDK LOCATION IS OVERRIDABLE. common.props defaults WdkRoot to
   E:\DEV\WinDDK. Override it with /p:WdkRoot=... or the WDK71_ROOT
   environment variable. A wrong path otherwise shows up as a thousand
   "cannot open include file wdm.h" lines; common.props checks for the
   compiler and the libraries first and fails once, with the reason.

3.  The Core Seam

   core.c takes input and emits output through function pointers and takes
   its clock as an argument:

       typedef void (*core_report_fn)(void *ctx, u8 report_id,
                                      const u8 *payload, u32 len);

       void core_init(core_state *cs, core_report_fn sink, void *ctx);
       void core_on_packet(core_state *cs, const u8 *raw, u32 len,
                           u64 now_100ns);
       void core_tick(core_state *cs, u64 now_100ns);

   In the driver the sink completes a pending HID read or queues the
   report; in the harness it records it so a test can assert on the bytes.
   Cutting here removes every IRP and USB routine from the dependency
   closure of the logic.

   PASSING TIME IN RATHER THAN CALLING KeQueryInterruptTime IS THE MOST
   VALUABLE PART OF THE ARRANGEMENT. Autofire deadlines, the mouse
   velocity accumulator and the acceleration ramp are all functions of
   elapsed time. Testing them means advancing the clock by hand, feeding a
   synthetic stick sweep and asserting on the exact byte sequence that
   comes out. A virtual machine cannot offer that and a kernel debugger
   makes it miserable.

4.  Why The Driver Uses The WDK 7.1 Toolchain

   No modern WDK is installed on this machine. Windows Kits 10 is present
   but SDK-only: it has um and ucrt headers, no km headers, no kernel
   libraries and no WindowsDriver MSBuild targets. The Visual Studio
   Driver project type is therefore unavailable.

   driver.vcxproj keeps the v141 project system for build plumbing and
   points the CL and Link tasks at the compiler and linker inside WDK 7.1
   - cl 15.00 and link 9.00, the VS2008 SP1 toolchain those headers were
   written against. Flags follow the DDK build system rather than being
   invented: $(WdkRoot)\bin\makefile.new, i386mk.inc and amd64mk.inc are
   the reference.

   The harness has no such constraint and uses v141 normally.

   WITH A MODERN WDK INSTALLED the right move is to convert
   driver.vcxproj to the Driver project type and delete most of section 5.
   The source does not change; every kernel API it uses is still current.

5.  Settings That Are Not Obvious

   Each of these was necessary to make link 9.00 accept what the v141
   targets emit by default. They read as arbitrary and are not.

   UseDebugLibraries IS FALSE IN EVERY CONFIGURATION, DEBUG INCLUDED.
   Four separate v141 defaults key off that one property: /MDd, /RTC1,
   /Od and /DEBUG:FASTLINK. link 9.00 rejects /DEBUG:FASTLINK outright,
   and the /RTC switches emit _RTC_InitBase and friends, which do not
   exist in the 2009 kernel libraries. Debug-ness is expressed explicitly
   instead - Optimization Disabled and DBG=1.

   DebugInformationFormat IS OldStyle (/Z7), NOT ProgramDatabase (/Zi).
   mspdbsrv.exe is absent from the DDK bin tree, so the PDB server cannot
   be spawned. /Z7 keeps debug information in the .obj and is the DDK
   default in any case.

   GenerateDebugInformation IS true, NOT DebugFull. link 9.00 ignores
   /DEBUG:FULL with LNK4224 and silently produces no PDB at all.

   RuntimeLibrary IS EMPTY. Any value emits /MT or /MD and pulls in a CRT
   that does not exist in kernel mode.

   THE ENTRY POINT IS GsDriverEntry, NOT DriverEntry. /GS makes
   BufferOverflowK.lib::GsDriverEntry the real entry point; it initialises
   the stack cookie and then calls ours. x86 decorates the stdcall name as
   GsDriverEntry@8, x64 does not.

   RandomizedBaseAddress AND DataExecutionPrevention ARE BOTH EMPTY.
   link 9.00 rejects /DYNAMICBASE in EITHER direction alongside /DRIVER;
   driver ASLR arrived with the Win8 WDK. Empty means no switch is
   emitted at all.

   NonCoreWin IS true. Without it Microsoft.Cpp.CoreWin.props prepends
   kernel32.lib, user32.lib, gdi32.lib and nine others to the link line.
   None of those belong in a kernel image.

   INCREMENTAL LINKING IS OFF FOR BOTH PROJECTS. Both emit xboxctl.*,
   separated only by the extension, so two .ilk files would collide. The
   linker PDB name is overridden per project for the same reason.

   THE INCLUDE AND LIBRARY PATHS ARE FULLY OVERRIDDEN, NOT APPENDED TO.
   Mixing the UCRT and MSVC headers with DDK headers is the classic
   failure: the DDK ships its own SAL 1 sal.h in inc\api and it must be
   the one that wins.

6.  Installing It

   WINDOWS 10 AND 11 ON x64 WILL NOT LOAD AN UNSIGNED DRIVER. For
   development, test signing plus a self-signed certificate in both the
   Trusted Root and Trusted Publishers stores. For anything shared,
   attestation signing through Partner Center covers a non-WHQL driver.

   ../tools carries the whole loop. Host side, once:

       tools\mktestcert.cmd          creates the test certificate

   Host side, every build:

       tools\package.cmd [dest]      build, run the harness, stamp
                                     DriverVer, sign, build and sign the
                                     catalogue, stage to the share

   Guest side, from an ELEVATED prompt. Once per VM:

       trustcert.cmd                 both certificate stores, and checks
                                     testsigning and Secure Boot

   Guest side, every build:

       deploy.cmd                    remove nodes, remove stale packages,
                                     install, rescan
       state.cmd                     what is actually installed and bound
       undeploy.cmd                  back to a clean slate

   package.cmd copies the four guest-side scripts and the .cer next to
   the staged package, so the guest needs no view of the source tree.

   Doing it by hand instead:

       pnputil /add-driver xboxctl.inf /install

   THE DEVICE ACQUIRES THREE CHILD DEVNODES. hidclass creates one per
   top-level collection and numbers them in descriptor order:

       HID\VID_045E&PID_0289&Col01    gamepad
       HID\VID_045E&PID_0289&Col02    keyboard
       HID\VID_045E&PID_0289&Col03    mouse

   An unsuffixed node is what a single-collection device produces. If one
   is present after installing this driver it is a leftover from whatever
   owned the pad before.

   A KERNEL DEBUGGER IS NOT OPTIONAL for bring-up. A fault in a HID
   minidriver takes the machine down with it, and the input stack is not
   something a target can be debugged over.

   A GHOST DEVICE NODE OUTRANKS GOOD INTENTIONS. A key under
   Enum\USB\VID_045E&PID_xxxx with no Service and no DeviceDesc is a pad
   that was plugged in once and never got a driver. Driver selection is
   sticky per node, so a leftover keeps its old decision and a freshly
   installed package is never reconsidered. deploy.cmd removes the nodes
   it can find; Device Manager with "Show hidden devices" turned on shows
   the rest.

6.1.  The Ampersand Trap In The Scripts

   Three of the guest-side scripts loop over the five hardware IDs, and
   the list CANNOT live in a variable.

   cmd finishes expanding %VAR% and !VAR! before it finishes tokenising
   the line, so an ampersand arriving from a variable is re-read as a
   command separator. A for whose set comes from one fails with

       'PID_0202" "VID_045E' is not recognized as an internal or
       external command

   once per entry, and the loop body never runs - while the script as a
   whole carries on and reports success. Delayed expansion does not help;
   measured, it fails identically.

   Quoted literals written into the for itself are tokenised with their
   quotes already in place, so the ampersand stays text. Each script
   therefore keeps its list in one :eachHwid subroutine that calls a
   handler per id, and passes the id on as a quoted argument - safe for
   the same reason, since %~1 is read after tokenising is done.

7.  What Is Built And What Is Stubbed

   The harness compiles the same wdm.c the driver does, so the split runs
   through that file rather than around it.

   +-------------------------+------------------+----------------------+
   | Path                    | Driver build     | Harness build        |
   +=========================+==================+======================+
   | core.c, all of it       | real             | real                 |
   | report queue            | real             | real                 |
   | pending-read list       | real             | real                 |
   | XcOnTransfer            | real             | real                 |
   | XcPollStart / Stop      | real             | real                 |
   | DriverEntry             | real             | real                 |
   | USB enumeration         | real             | absent               |
   | URB submission          | real             | records the slot     |
   | PnP and power dispatch  | real             | STATUS_NOT_SUPPORTED |
   +-------------------------+------------------+----------------------+

   WHAT IS STUBBED IS WHAT HAS NO LOGIC IN IT. Fetching descriptors,
   selecting a configuration and forwarding a power IRP are Windows
   plumbing with no decisions to get wrong, and modelling a USB stack to
   test them would be modelling the test. Everything downstream of a
   completed transfer - which is where the decisions are - compiles and
   runs identically in both builds.

   The consequence to keep in mind: A PASSING HARNESS SAYS NOTHING ABOUT
   ENUMERATION, PnP OR TEARDOWN. Those are verified on hardware with a
   kernel debugger, and nowhere else.

8.  orig/

   src_drv/orig is the XBCD 0.2.6 source, and it is REFERENCE ONLY.

   Never modify, move, rename or rebuild anything under it. It is here for
   one purpose: it documents the 20-byte packet the pad emits, and that is
   a fact about hardware. The analysis of it is in
   ../docs/xbcd-architecture.txt.

   Do not paste XBCD code into this tree. Data layouts, packet offsets and
   protocol constants may be used freely; its expression - functions,
   structures, control flow, comments - may not. XBCD is GPLv2, and
   copying its code makes this project GPLv2 too. See ../CLAUDE.md.
