@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem state.cmd - what is actually installed and bound right now. GUEST side.
rem Read only. Answers "did my build even land" in one step.
rem
rem     state.cmd [output-file]
rem
rem With no argument it writes xboxctl-state.txt BESIDE THIS SCRIPT.
rem
rem A PAD WITH NO DRIVER IS AN UNKNOWN DEVICE, NOT A GAME CONTROLLER.
rem The original Xbox controller is not a HID device - its compatible
rem IDs are USB\Class_58&SubClass_42&Prot_00 and shorter forms of the
rem same, and 0x58 is not HID class 3 - so nothing inbox claims it and
rem there is no fallback driver. Device Manager shows "Unknown device"
rem with a yellow mark until ours binds, which makes a failed install
rem unambiguous rather than something to squint at.
rem ----------------------------------------------------------------------

rem ONE DESTINATION, WRITTEN DIRECTLY. %~dp0 is this script's own folder
rem and ALREADY ENDS IN A BACKSLASH, so %~dp0xboxctl-state.txt needs no
rem separator of its own. Run from a share and the report lands in the
rem same folder the other side reads.
rem
rem A READ-ONLY DESTINATION FAILS LOUDLY HERE RATHER THAN QUIETLY BELOW.
rem A VirtualBox shared folder is read-only unless it was explicitly made
rem writable, and redirecting onto one fails EVERY line in this script
rem with "The system cannot find the path specified" - dozens of identical
rem errors that read as the registry queries failing rather than as the
rem output file never having been created. One probe up front turns that
rem into a single sentence naming the real problem.
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0xboxctl-state.txt"

break > "%OUT%" 2>nul
if not exist "%OUT%" (
    echo   Cannot write "%OUT%"
    echo.
    echo   That folder is read-only. Either make it writable, or give
    echo   this script a path that is not:
    echo       state.cmd %%TEMP%%\xboxctl-state.txt
    exit /b 1
)

set "SVC=xboxctl"

echo XBOXCTL installation report > "%OUT%"
echo Collected %DATE% %TIME% >> "%OUT%"
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 1. OUR STAGED DRIVER PACKAGES                          == >> "%OUT%"
echo ==    MORE THAN ONE means old copies are still ranked      == >> "%OUT%"
echo ==    against the new one. deploy.cmd clears them.         == >> "%OUT%"
echo ==    Signer Name must NOT read Unknown - see trustcert.   == >> "%OUT%"
echo ============================================================ >> "%OUT%"
pnputil /enum-drivers >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 2. THE SERVICE                                         == >> "%OUT%"
echo ==    START_TYPE 4 DISABLED is the trap: pnputil sets it   == >> "%OUT%"
echo ==    when it uninstalls a package and nothing puts it     == >> "%OUT%"
echo ==    back, so the driver can never be selected again.     == >> "%OUT%"
echo ============================================================ >> "%OUT%"
sc query %SVC% >> "%OUT%" 2>&1
echo. >> "%OUT%"
sc qc %SVC% >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 3. WHICH DRIVER OWNS THE DEVICE                        == >> "%OUT%"
echo ==    Service should read xboxctl. NO Service at all means == >> "%OUT%"
echo ==    nothing claimed the pad - our INF did not match, or  == >> "%OUT%"
echo ==    the driver refused to start. Nothing inbox claims a  == >> "%OUT%"
echo ==    Class_58 device, so there is no other candidate.     == >> "%OUT%"
echo ==    A key with NO Service at all is a GHOST node from a  == >> "%OUT%"
echo ==    pad that was plugged in once and never got a driver. == >> "%OUT%"
echo ============================================================ >> "%OUT%"
call :eachHwid :dumpService
echo. >> "%OUT%"
call :eachHwid :dumpClass
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 4. THE HID CHILDREN hidclass CREATED                   == >> "%OUT%"
echo ==    The composite descriptor has three top-level         == >> "%OUT%"
echo ==    collections, so expect THREE:                        == >> "%OUT%"
echo ==      Col01 gamepad, Col02 keyboard, Col03 mouse         == >> "%OUT%"
echo ==    NO children at all means our driver never started -  == >> "%OUT%"
echo ==    nothing else can create them for a Class_58 device.  == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\HID" /s /f "045E" >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 5. THE DRIVER'S OWN CONFIGURATION                      == >> "%OUT%"
echo ==    XbcdConfig ABSENT IS CORRECT until a configurator    == >> "%OUT%"
echo ==    writes one - the driver falls back to its built-in   == >> "%OUT%"
echo ==    default map. It lives on the DEVICE's hardware key,  == >> "%OUT%"
echo ==    not under Software, which is what keeps it clear of  == >> "%OUT%"
echo ==    WOW64 redirection.                                   == >> "%OUT%"
echo ============================================================ >> "%OUT%"
call :eachHwid :dumpConfig
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 6. THE DIRECTINPUT BUTTON NAMES                        == >> "%OUT%"
echo ==    Proves the INF's AddReg section ran. Registered for  == >> "%OUT%"
echo ==    PID_0289 only; the key is per VID and PID.           == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\Joystick\OEM\VID_045E&PID_0289\Buttons" /s >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 7. IS IT LOADED                                        == >> "%OUT%"
echo ============================================================ >> "%OUT%"
driverquery /v | findstr /i "%SVC%" >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo ============================================================ >> "%OUT%"
echo == 8. EVERY XBOX DEVICE NODE, LIVE OR GHOST               == >> "%OUT%"
echo ============================================================ >> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB" /s /f "045E" /k >> "%OUT%" 2>&1
echo. >> "%OUT%"

echo.
echo ============================================================
echo == THE THREE ANSWERS THAT MATTER
echo ==   Service should read xboxctl. Absent means nothing
echo ==   claimed the pad: no match, or it refused to start.
echo ==   START_TYPE must not be 4 DISABLED.
echo ==   Expect THREE HID children: Col01 Col02 Col03.
echo ============================================================
rem PIPE THE BANNERS OUT. The section headers above contain the very
rem words being searched for - "install state", "START_TYPE 4 DISABLED" -
rem so an unfiltered findstr reports its own commentary as though it were
rem a result. Every banner line contains "==" and no real result does.
findstr /i /c:"SERVICE_NAME" /c:"STATE" /c:"START_TYPE" "%OUT%" | findstr /v "=="
findstr /i /c:"    Service    REG_SZ" "%OUT%" | findstr /v "=="
findstr /i /c:"HID\VID_045E" "%OUT%" | findstr /v "=="
echo.

echo   Full report at %OUT%
goto :done

rem ----------------------------------------------------------------------
rem THE HARDWARE ID LIST LIVES HERE AND ONLY HERE, AND IT CANNOT LIVE IN A
rem VARIABLE. cmd finishes expanding %VAR% and !VAR! before it finishes
rem tokenising the line, so an ampersand that arrives from a variable is
rem re-read as a command separator: a for whose set comes from one fails
rem with 'PID_0202" "VID_045E' is not recognized as an internal or
rem external command, once per entry, and the body never runs. Quoted
rem literals written in the for itself are tokenised with their quotes
rem already in place, so the ampersand stays text. Measured both ways.
rem ----------------------------------------------------------------------
:eachHwid
for %%H in (
    "VID_045E&PID_0202"
    "VID_045E&PID_0285"
    "VID_045E&PID_0287"
    "VID_045E&PID_0288"
    "VID_045E&PID_0289"
) do call %1 "%%~H"
goto :eof

:dumpService
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\%~1" /s /v Service >> "%OUT%" 2>&1
goto :eof

:dumpClass
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\%~1" /s /v ClassGUID >> "%OUT%" 2>&1
goto :eof

:dumpConfig
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\%~1" /s /v XbcdConfig >> "%OUT%" 2>&1
goto :eof

:done
endlocal
