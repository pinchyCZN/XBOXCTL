@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem undeploy.cmd - remove the XBOXCTL driver completely. GUEST side, MUST
rem BE ELEVATED. For when you want a genuinely clean slate without
rem restoring a snapshot.
rem
rem     undeploy.cmd
rem
rem Removes, in this order: the device nodes, then every staged copy of
rem our package, then the service if anything left it behind.
rem
rem ORDER MATTERS. Deleting the package while a device is still bound to
rem it leaves the devnode pointing at a service whose binary is gone.
rem Removing the devices first avoids that.
rem
rem AFTER THIS THE PAD STOPS WORKING ENTIRELY, and that is the expected
rem end state rather than a problem. The original Xbox controller is a
rem Class_58 device, not HID, so nothing inbox claims it: with our
rem package gone it reverts to an Unknown device in Device Manager.
rem
rem ENGLISH WINDOWS ONLY, same pnputil label caveat as deploy.cmd.
rem ----------------------------------------------------------------------

set "INFNAME=xboxctl.inf"
set "SVC=xboxctl"

net session >nul 2>&1
if errorlevel 1 (
    echo   NOT ELEVATED. Run this from an Administrator command prompt.
    exit /b 1
)

echo ============================================================
echo == 1. REMOVING DEVICE NODES
echo ============================================================
set "GONE=0"
call :eachHwid :removeNodes
echo   %GONE% device node^(s^) removed.
echo.

echo ============================================================
echo == 2. REMOVING EVERY STAGED COPY OF OUR PACKAGE
echo ============================================================
set "TMPF=%TEMP%\xboxctl-undrv.txt"
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
echo   %REMOVED% package^(s^) removed.
echo.

echo ============================================================
echo == 3. THE SERVICE, IF ANYTHING LEFT IT BEHIND
echo ============================================================
sc query %SVC% >nul 2>&1
if errorlevel 1 (
    echo   no %SVC% service, which is the expected end state.
) else (
    echo   service still registered; deleting.
    sc stop %SVC% >nul 2>&1
    sc delete %SVC%
)
echo.

echo ============================================================
echo == 4. WHAT IS LEFT
echo ============================================================
call :eachHwid :showService
echo.
echo   Expect no Service value at all, or the keys to be gone.
echo.
echo   A KEY WITH NO Service AND NO DeviceDesc IS A GHOST - a pad that
echo   was plugged in once and never got a driver. It is harmless but it
echo   keeps a stale binding decision alive, so clear it before reading
echo   a test result. Device Manager with "Show hidden devices" on will
echo   show them; removing them there is the reliable way.
echo.
echo   Unplug and replug the controller to settle the enumeration.
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
        pnputil /remove-device "!HWID!\!INST!" 2>nul
        if errorlevel 1 (
            echo     pnputil /remove-device unavailable or refused;
            echo     remove it from Device Manager instead.
        ) else (
            set /a GONE+=1
        )
    )
)
goto :eof

:showService
reg query "HKLM\SYSTEM\CurrentControlSet\Enum\USB\%~1" /s /v Service 2>nul
goto :eof

:done
endlocal
