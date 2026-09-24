XBOXCTL                                                    Configurator
src_cfg                                                        xbconfig


                        THE CONFIGURATOR


Abstract

   xbconfig.exe edits a profile and pushes it to the driver. It is the
   C replacement for tools/mkconfig.py and it owns exactly what that
   script owned: the keyboard layout, the name tables and the curve
   maths. It calls no interpreter and reads no registry key.


1.  What Is Where

   +---------------+-------------------------------------------------+
   | File          | What it is                                      |
   +===============+=================================================+
   | xbconfig.h    | Shared declarations. Names no Windows type.     |
   | names.c       | Every symbolic name, both directions.           |
   | profile.c     | The .txt format, read and written.              |
   | driver.c      | \\.\xboxctl and the IOCTLs.                     |
   | main.c        | The main dialog, and a command line.            |
   | bind.c        | The per-control binding dialog, and capture.    |
   | xbconfig.rc   | The dialog templates.                           |
   +---------------+-------------------------------------------------+

   names.c and profile.c NAME NO WINDOWS TYPE, for the same reason
   core.c does not: the parser and the tables are the part worth
   exercising without a message loop in front of them.


2.  The Blob Format Is Not Reimplemented Here

   This project compiles ../src_drv/core.c, so core_config_save is
   literally the code that runs in the kernel. A second implementation
   of a wire format drifts; there is already one in tools/mkconfig.py
   and the harness has to cross-check it byte for byte. There is not a
   third.

   What this project does add is the profile TEXT format, which the
   driver never sees, and the Bezier, which the driver never evaluates.


3.  Everything Lives Beside The Executable

   Profiles are .txt files in a profiles/ folder next to xbconfig.exe,
   and the window position is in xbconfig.ini next to it as well.
   Nothing is written to the registry, to AppData or to the working
   directory, so the program can be copied to a stick and carried to the
   test machine.

   A CONFIGURATION IS NAMED BY ITS FILE. profiles/arcade.txt is the
   configuration called "arcade"; no name is stored inside the file that
   could disagree with the one on it.

   THE EXECUTABLE FINDS ITS PROFILES, NOT THE REPOSITORY'S. Running it
   out of the build directory gives it that directory's profiles folder,
   which is a different set of files from the repository's profiles/.
   Copy the executable to where the profiles are meant to live.


4.  There Is No Save Button

   Clicking OK in a binding dialog writes the profile. A configurator
   that can be left holding unsaved work is a configurator that loses
   it.


5.  The Command Line, Which Is How The Parser Is Tested

       xbconfig --compile in.txt out.bin
       xbconfig --rewrite in.txt out.txt

   THE FIRST IS THE CROSS-CHECK. The same profile through this program
   and through tools/mkconfig.py has to produce the same bytes, the way
   the harness cross-checks core.c against that script. Two parsers of
   one format drift silently otherwise, and the symptom is a binding
   that works from one tool and not the other.

       xbconfig --compile ..\..\..\..\profiles\fps.txt fps.bin
       python tools\mkconfig.py profiles\fps.txt -o py.bin
       fc /b fps.bin py.bin

   The second is the round trip: a profile written back out has to parse
   to the same configuration it came from.


6.  Building

   A single VS2017 solution, v141, no DDK toolchain - core.c names no
   Windows type and no kernel API, so it builds against the ordinary CRT
   headers with nothing special defined.

       msbuild src_cfg\xbconfig.vcxproj /p:Configuration=Release ^
               /p:Platform=x64
