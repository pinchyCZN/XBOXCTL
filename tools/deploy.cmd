@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem deploy.cmd - clean-slate reinstall of the XBOXCTL driver. GUEST side,
rem MUST BE ELEVATED. No reboot.
rem
rem     deploy.cmd [package-folder]
rem
rem Default package folder is the pkg\ beside this script, which is what
rem tools\package.cmd stages.
rem
rem WHY IT DELETES FIRST. Every install stages a copy into the driver
rem store under its own hash and publishes a new oemNN.inf; the old copies
rem stay forever. Windows then RANKS all of them, so a stale package can
rem outrank a freshly installed one and the new driver silently does not
rem load. Deleting every copy of ours before installing is what keeps a
rem tweak loop honest.
rem
rem YOU CANNOT SHORTCUT THIS BY OVERWRITING THE .SYS. The INF sets
rem PnpLockdown=1, so %windir%\System32\drivers\xboxctl.sys is protected
rem and a hand copy is refused. Package reinstall is the only path.
rem
rem ENGLISH WINDOWS ONLY. pnputil's field labels are localised and this
rem parses "Published Name" and "Original Name" literally.
rem ----------------------------------------------------------------------

set "PKG=%~1"
if "%PKG%"=="" set "PKG=%~dp0pkg"
set "INFNAME=xboxctl.inf"
set "SVC=xboxctl"

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)

rem WAS THE OLD IMAGE ALREADY RESIDENT WHEN WE STARTED? That, not the
rem state afterwards, is what decides whether this deploy can take. A
rem running service AFTER the install is normal and expected - the device
rem was rebound and the driver loaded. A running service BEFORE it means
rem the previous image is still mapped, and Windows does not replace a
rem mapped image, so the install below will stage the new file and leave
rem the kernel executing the old one. Every step still reports success.
rem
rem xboxctl has no control device, so it unloads once its last device
rem object goes. Removing the device nodes in step 1 is normally enough;
rem this catches the case where something still holds one open.
set "WASRESIDENT="
sc query %SVC% 2>nul | find /i "RUNNING" >nul
if not errorlevel 1 set "WASRESIDENT=1"

rem USE !VAR! INSIDE A BLOCK, NOT %VAR%. cmd expands %VAR% while it PARSES
rem the whole parenthesised block, before running any of it, so a value
rem containing a closing paren ends the block early and the rest of the
rem path becomes a stray token. Run from "C:\Program Files (x86)\..." that
rem fails with "\... was unexpected at this time" - and it fails even when
rem the condition is FALSE, because parsing happens first.
if not exist "%PKG%\%INFNAME%" (
    echo   No !INFNAME! in !PKG!
    echo   Run tools\package.cmd on the host first.
    exit /b 1
)

echo ============================================================
echo == 1. REMOVING THE DEVICE NODES
echo ==    DRIVER SELECTION IS STICKY. Once a device node has a
echo ==    driver bound, replugging does NOT reconsider it - PnP
echo ==    just restarts whatever was chosen the first time.
echo ==    Staging a better package changes nothing on its own, so
echo ==    a rebuild silently keeps running the old driver, or
echo ==    nothing at all. Deleting the node is what makes the
echo ==    rescan in step 4 run a real selection.
echo ============================================================
set "GONE=0"
call :eachHwid :removeNodes
echo   %GONE% device node^(s^) removed.
echo.

echo ============================================================
echo == 2. REMOVING EVERY STAGED COPY OF OUR PACKAGE
echo ============================================================
set "TMPF=%TEMP%\xboxctl-drv.txt"
pnputil /enum-drivers > "%TMPF%" 2>nul

set "LAST="
set "REMOVED=0"
for /f "usebackq delims=" %%L in ("%TMPF%") do (
    set "LINE=%%L"
    if /i "!LINE:~0,15!"=="Published Name:" (
        for /f "tokens=2 delims=:" %%V in ("!LINE!") do (
            set "LAST=%%V"
            set "LAST=!LAST: =!"
        )
    )
    if /i "!LINE:~0,14!"=="Original Name:" (
        for /f "tokens=2 delims=:" %%V in ("!LINE!") do (
            set "ORIG=%%V"
            set "ORIG=!ORIG: =!"
            if /i "!ORIG!"=="%INFNAME%" (
                if defined LAST (
                    echo   deleting !LAST!
                    pnputil /delete-driver !LAST! /uninstall /force
                    set /a REMOVED+=1
                    set "LAST="
                )
            )
        )
    )
)
del "%TMPF%" >nul 2>&1
echo   %REMOVED% stale package^(s^) removed.
echo.

echo ============================================================
echo == 3. INSTALLING THE NEW PACKAGE
echo ============================================================
pnputil /add-driver "%PKG%\%INFNAME%" /install
if errorlevel 1 (
    echo.
    echo   INSTALL FAILED. Usual causes, in order of likelihood:
    echo     - the catalogue is unsigned, or the test certificate is not
    echo       in BOTH Trusted Root and Trusted Publishers - trustcert.cmd
    echo     - test signing is off:  bcdedit /set testsigning on  + reboot
    echo     - on Win10/11, Secure Boot or Memory Integrity is on
    exit /b 1
)
echo.

echo ============================================================
echo == 4. RESCANNING SO A PRESENT DEVICE PICKS IT UP
echo ============================================================
rem RE-ENABLE THE SERVICE FIRST. pnputil /delete-driver /uninstall sets
rem our service's start type to 4, DISABLED, as part of detaching it from
rem its devices - and nothing puts it back. A disabled service can never
rem be selected for a device, and the symptom is a correctly staged,
rem correctly signed driver that simply never binds while the pad sits
rem in Device Manager as an Unknown device. sc config on a service that
rem does not exist is harmless, so this is unconditional.
sc config %SVC% start= demand >nul 2>&1

pnputil /scan-devices
echo.

echo ============================================================
echo == 5. RESULT
echo ============================================================
sc query %SVC% 2>nul | findstr /i "SERVICE_NAME STATE"
call :eachHwid :showService
echo.
if defined WASRESIDENT (
    echo   ------------------------------------------------------------
    echo   THE OLD DRIVER WAS ALREADY LOADED WHEN THIS STARTED, so the
    echo   new file is staged but the kernel may still be running the
    echo   previous build. Everything above still reports success; that
    echo   is what makes this worth saying out loud.
    echo.
    echo   If the behaviour you expected has not changed, REBOOT THE
    echo   GUEST. That is the only step that reliably maps a new image,
    echo   and it costs less than diagnosing a fix that appears not to
    echo   work.
    echo.
    echo   To confirm from the host debugger:
    echo       lm vm xboxctl
    echo   The load address and the PDB GUID both change on every build.
    echo   If neither moved, the old image is still running.
    echo   ------------------------------------------------------------
    echo.
)
echo   For the full picture run state.cmd.
echo   Expect THREE HID children - Col01 gamepad, Col02 keyboard,
echo   Col03 mouse. Fewer means the descriptor was rejected.
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
rem
rem Passing each id on to a subroutine as a quoted argument is safe for
rem the same reason: %~1 is read after tokenising is done.
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

:removeNodes
set "HWID=USB\%~1"
set "HWLEAF=%~1"
for /f "usebackq tokens=*" %%K in (
    `reg query "HKLM\SYSTEM\CurrentControlSet\Enum\%HWID%" 2^>nul`
) do (
    set "KEY=%%K"
    rem SKIP THE PARENT KEY. reg query prints the queried key itself as
    rem well as its subkeys, and taking its last component yields the
    rem hardware id rather than an instance id - a removal that always
    rem fails and reports itself as pnputil being unavailable.
    set "INST="
    for %%I in ("!KEY!") do set "INST=%%~nxI"
    if /i "!INST!"=="!HWLEAF!" set "INST="
    if defined INST (
        echo   removing !HWID!\!INST!
        pnputil /remove-device "!HWID!\!INST!" >nul 2>&1
        if not errorlevel 1 set /a GONE+=1
    )
)
goto :eof

:showService
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\%~1" /s /v Service 2>nul | findstr /i "Service"
goto :eof

:done
endlocal
