@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem package.cmd - build, stamp, sign and stage a driver package. HOST side.
rem
rem     tools\package.cmd [dest] [nostamp]
rem
rem Default dest is B:\xboxctl\pkg, a folder of our own on the share
rem the guest reads. One command turns a source tree into something the
rem guest's deploy.cmd can install.
rem
rem WHY IT STAMPS DriverVer. Windows ranks driver packages and a stale
rem staged copy with an equal or higher version can win, so a rebuilt
rem driver silently does not deploy. deploy.cmd deletes our old packages
rem first, which also solves it, but the two together mean a partial
rem delete cannot leave an old build in charge. The stamp is written into
rem src_drv\xboxctl.inf IN PLACE, because the version shipped has to be
rem the version in the source - pass nostamp to leave it alone.
rem
rem The version is 0.1.MMdd.HHmm. That rises through a development period
rem but resets at a year boundary; if that ever matters, widen it.
rem ----------------------------------------------------------------------

rem A FOLDER OF OUR OWN ON THE SHARE. Step 6 copies the guest-side
rem scripts to %DEST%\.., so staging to B:\pkg would write
rem deploy.cmd and state.cmd into B:\ root - where another
rem project's copies of those same file names already live. They would be
rem silently replaced, and the next person to run B:\deploy.cmd
rem would install the wrong driver.
set "DEST=%~1"
if "%DEST%"=="" set "DEST=B:\xboxctl\pkg"
set "STAMP=1"
if /i "%~2"=="nostamp" set "STAMP="
if /i "%~1"=="nostamp" (set "STAMP=" & set "DEST=B:\xboxctl\pkg")

set "ROOT=%~dp0.."
set "INF=%ROOT%\src_drv\xboxctl.inf"
set "PKG=%ROOT%\src_drv\build\x64\Release\package"

rem ---- 1. the version stamp ------------------------------------------
if defined STAMP (
    rem THE SLASHES HAVE TO BE FORCED. In a .NET format string "/" is the
    rem CULTURE'S date separator, not a literal - on a machine whose short
    rem date uses dashes, -Format 'MM/dd/yyyy' yields 09-22-2026 and
    rem Inf2Cat rejects it with "DriverVer missing or in incorrect format".
    rem InvariantCulture is what pins it to slashes.
    for /f "usebackq delims=" %%D in (
        `powershell -NoProfile -Command "(Get-Date).ToString('MM/dd/yyyy',[Globalization.CultureInfo]::InvariantCulture)"`
    ) do set "DVDATE=%%D"
    for /f "usebackq delims=" %%V in (
        `powershell -NoProfile -Command "Get-Date -Format 'MMdd.HHmm'"`
    ) do set "DVVER=%%V"
    set "DRIVERVER=!DVDATE!,0.1.!DVVER!"
    echo   stamping DriverVer = !DRIVERVER!
    powershell -NoProfile -Command ^
      "$p='%INF%'; $t=[IO.File]::ReadAllText($p);" ^
      "$t=[Text.RegularExpressions.Regex]::Replace($t," ^
      "'(?m)^DriverVer\s*=.*$','DriverVer   = !DRIVERVER!');" ^
      "[IO.File]::WriteAllText($p,$t)"
    if errorlevel 1 (
        echo   FAILED to stamp the INF.
        exit /b 1
    )
)

rem ---- 2. build --------------------------------------------------------
set "MSBUILD="
for %%E in (Professional Enterprise Community BuildTools) do (
    if not defined MSBUILD (
        set "C=C:\Program Files (x86)\Microsoft Visual Studio\2017\%%E\MSBuild\15.0\Bin\MSBuild.exe"
        if exist "!C!" set "MSBUILD=!C!"
    )
)
if not defined MSBUILD (
    echo   No VS2017 MSBuild found.
    exit /b 1
)

echo   building x64 Release ...
"%MSBUILD%" "%ROOT%\src_drv\xboxctl.sln" /p:Configuration=Release ^
    /p:Platform=x64 /v:minimal /nologo
if errorlevel 1 (
    echo   BUILD FAILED.
    exit /b 1
)

rem ---- 3. the harness, because a failing engine is not worth deploying --
rem
rem IT EXITS NON-ZERO ON ANY FAILED CHECK, which is the whole reason it is
rem wired in here rather than left to be remembered. Note what it does NOT
rem cover: enumeration, PnP and teardown against real hardware are stubbed
rem in the harness build, so a pass says the engine is sound and nothing
rem about whether the device will start. See src_drv\README.txt section 7.
echo   running the harness ...
"%ROOT%\src_drv\build\x64\Release\xboxctl.exe"
if errorlevel 1 (
    echo.
    echo   HARNESS FAILED. Fix that before deploying - every check it runs
    echo   is cheaper to diagnose here than in a kernel debugger.
    exit /b 1
)

rem ---- 4. sign, which also assembles %PKG% and builds the catalogue ----
echo   signing ...
call "%~dp0signdriver.cmd" x64 Release
if errorlevel 1 (
    echo   SIGNING FAILED.
    exit /b 1
)

rem ---- 5. stage for the guest -----------------------------------------
rem Wipe the destination rather than copying over it, so a file that a
rem build stopped producing cannot linger and get installed.
if exist "%DEST%" rmdir /s /q "%DEST%"
mkdir "%DEST%" 2>nul
copy /y "%PKG%\*" "%DEST%\" >nul
if errorlevel 1 (
    echo   could not stage to !DEST!
    exit /b 1
)

rem ---- 6. the guest-side scripts ---------------------------------------
rem The guest cannot see the source tree, only the share, so the scripts
rem it has to run have to travel with the package. They go BESIDE the
rem package folder rather than inside it, to keep that folder to exactly
rem the files the catalogue covers.
for %%S in (deploy.cmd state.cmd undeploy.cmd trustcert.cmd) do (
    if exist "%~dp0%%S" copy /y "%~dp0%%S" "%DEST%\..\" >nul
)
rem THE CERTIFICATE HAS TO TRAVEL TOO. Without it in the guest's Root and
rem TrustedPublisher stores the package stages but reports
rem "Signer Name: Unknown", the driver is refused as unsigned, and the
rem pad is left as an Unknown device - nothing inbox claims a Class_58
rem device, so there is no fallback to mistake for success.
copy /y "%ROOT%\src_drv\build\sign\xboxctl-test.cer" "%DEST%\..\" >nul 2>&1
if errorlevel 1 echo   WARNING: no xboxctl-test.cer to stage.

echo.
echo   staged to %DEST%
dir /b "%DEST%"
echo.
echo   In the guest, from an ELEVATED prompt:
echo       %DEST%\..\trustcert.cmd      (once per VM)
echo       %DEST%\..\deploy.cmd
echo       %DEST%\..\state.cmd
endlocal
