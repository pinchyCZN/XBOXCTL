XBOXCTL                                                            Index
docs/


                          DOCUMENT INDEX


   XBOXCTL is a configurable HID minidriver for the original Xbox
   controller. The pad is presented to Windows as a COMPOSITE DEVICE -
   a gamepad, a keyboard and a mouse from one USB endpoint - so any
   control can be remapped to a real keystroke or mouse event that every
   application sees as genuine hardware.

   The architecture is the Adaptoid's (../../ADAPTOID), minus its bytecode
   scripting engine: remapping is a table of fixed-size parameter records
   pushed to the driver, not a program. XBCD, the 2005 GPL Xbox pad
   driver, is reference only - it documents the 20-byte packet the pad
   emits, and that is the one thing taken from it.

Read In This Order

   1.  driver-plan.txt

       The architecture decision, the composite descriptor as built with
       its three collections and six report IDs, the report layouts, the
       push model - DevicesArePolled FALSE, two poll slots, one report
       queue with gamepad coalescing - the tick, the configuration
       transport, the division of work with a configurator, what is built
       now, the work order and the risks.

       Read this first if the question is about the driver.

   2.  analog-to-mouse.txt

       The hard part. Takes the Adaptoid's stick handlers apart,
       establishes that they decompose into a configurable shaping curve
       and a fixed velocity-to-deltas quantiser, and specifies a
       replacement for both. Contains the Bezier answer - two numbers
       reproduce every curve the Adaptoid library ships, exactly - the
       residual accumulator that replaces the quantiser and the thread
       scheduler behind it, the acceleration and smoothing parameters no
       curve can express, the argument for working radially rather than
       per axis, the fixed-point pipeline, and a table reproducing all
       ten shipped profiles in the new parameters.

   3.  mapping-engine.txt

       Everything that is not the stick: the source enumeration, the
       action set, the twelve-byte binding record, activation with
       hysteresis, autofire, layers, the three output state machines, the
       evaluation order and the configuration blob. Ends with the
       boundary - what the model deliberately cannot express and what the
       smallest honest extension would be.

   4.  xbcd-architecture.txt

       What XBCD does, file by file and data path by data path. It is the
       datasheet for the hardware: section 6 has the 20-byte packet
       formats and section 7.1 the semiaxis decode, which are the two
       things this project takes from it. The rest is there so the
       reasoning behind the design is checkable - what its five-second
       keep-alive timer does, why DevicesArePolled TRUE blocks a driver
       that must push, and the 64-bit overflow in its radial math.

   ../src_drv/README.txt is the fifth document and covers the code rather
   than the design: layout, build, the core seam, the WDK 7.1 toolchain,
   installing, and which paths are stubbed in the harness build.

Conventions

   Documents are plain ASCII, laid out like an old RFC: three-space body
   indent wrapped at 80 columns, four-or-more-space blocks preformatted,
   tables drawn with +---+ borders. Three tools enforce it:

       python tools/asciify.py --check     ASCII, width, table alignment
       python tools/asciify.py --fix       transliterate and rewrap
       python tools/tblfix.py PATH         re-align a table block
       python tools/retab.py --check       source indentation, src_drv/

   A document states the CURRENT FACTS. It is not a development log:
   there are no corrections, no retractions and no before-and-after. When
   a finding turns out to be wrong, the wrong statement is deleted and the
   right one written in its place. The single exception is a trap -
   something that looks correct, was tried, and failed for a reason not
   visible from the result.

   Claims are backed by evidence: a file and a line in src_drv, a
   descriptor byte, a quotation from an installed Adaptoid script, or a
   citation into ../../ADAPTOID/docs. Anything not directly supported is
   marked INFERENCE.

   ../src_drv/orig is the ORIGINAL XBCD SOURCE and is reference only.
   Never modify, move, rename or rebuild it, and do not paste its code
   into the driver. See ../CLAUDE.md.
