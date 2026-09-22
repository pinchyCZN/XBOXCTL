@echo off
setlocal enabledelayedexpansion
rem ----------------------------------------------------------------------
rem mktestcert.cmd - make the test-signing certificate. RUN THIS ONCE.
rem
rem Produces two files under src_drv\build\sign\, which is inside the
rem gitignored build tree because one of them holds a PRIVATE KEY:
rem
rem     xboxctl-test.pfx   the key, used by tools\signdriver.cmd
rem     xboxctl-test.cer   the public half, to install on the test VM
rem
rem THIS IS A SELF-SIGNED TEST CERTIFICATE AND NOTHING ELSE. It will not
rem satisfy a production machine; it exists so that a VM with test signing
rem turned on will load the driver. Do not treat it as a signing identity.
rem
rem THIS PROJECT DOES NOT USE A CERTIFICATE OF ITS OWN. src_drv\build\sign
rem holds a COPY of one shared test identity, CN=Adaptoid Test Signing,
rem so every driver built here is signed by the same publisher and a test
rem machine needs exactly one certificate trusted rather than one per
rem project. The files carry this project's name; the certificate inside
rem them does not, and renaming a file does not rename what it contains -
rem which is why trustcert.cmd verifies by SUBJECT.
rem
rem So this script is here for the day a fresh identity IS wanted. It
rem refuses to run while a .pfx is present, which is the normal state.
rem A NEW CERTIFICATE MEANS RE-TRUSTING IT ON EVERY TEST MACHINE and
rem re-signing every driver that the old one signed.
rem
rem New-SelfSignedCertificate rather than the WDK's makecert.exe: makecert
rem is deprecated and its 2009 build defaults to SHA-1, which Windows 10
rem does not accept for a kernel-mode signature.
rem ----------------------------------------------------------------------

set "OUT=%~dp0..\src_drv\build\sign"
set "SUBJECT=CN=XBOXCTL Test Signing"
set "PFXPASS=xboxctl"

if not "%~1"=="" set "PFXPASS=%~1"

if not exist "%OUT%" mkdir "%OUT%" >nul 2>&1

if exist "%OUT%\xboxctl-test.pfx" (
    echo.
    echo   A certificate already exists:
    echo     !OUT!\xboxctl-test.pfx
    echo.
    echo   Delete it first if you really want a new one. Replacing it means
    echo   re-installing the .cer on every test machine that trusts it.
    exit /b 1
)

echo Creating a code-signing certificate, SHA-256, valid ten years...

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$c = New-SelfSignedCertificate -Type CodeSigningCert" ^
  " -Subject '%SUBJECT%' -CertStoreLocation Cert:\CurrentUser\My" ^
  " -HashAlgorithm SHA256 -KeyExportPolicy Exportable" ^
  " -NotAfter (Get-Date).AddYears(10);" ^
  "$p = ConvertTo-SecureString -String '%PFXPASS%' -Force -AsPlainText;" ^
  "Export-PfxCertificate -Cert $c -FilePath '%OUT%\xboxctl-test.pfx'" ^
  " -Password $p | Out-Null;" ^
  "Export-Certificate -Cert $c -FilePath '%OUT%\xboxctl-test.cer'" ^
  " | Out-Null;" ^
  "Write-Host ('  thumbprint  ' + $c.Thumbprint)"

if errorlevel 1 (
    echo.
    echo   FAILED. New-SelfSignedCertificate needs Windows 8 or later. An
    echo   elevated shell is not required, but an execution policy that
    echo   blocks -Command is.
    exit /b 1
)

echo.
echo   wrote  %OUT%\xboxctl-test.pfx   (private key - never commit this)
echo   wrote  %OUT%\xboxctl-test.cer   (public half - copy to the VM)
echo.
echo   NEXT, ON THE TEST MACHINE, and all three are needed:
echo.
echo     1. Trust the certificate. From an ELEVATED prompt, with the .cer
echo        copied over - or just run tools\trustcert.cmd, which does both
echo        stores and checks the other preconditions:
echo.
echo          certutil -addstore -f Root             xboxctl-test.cer
echo          certutil -addstore -f TrustedPublisher xboxctl-test.cer
echo.
echo        Root alone is not enough: the driver loads but the INF install
echo        still prompts, because PnP checks TrustedPublisher.
echo.
echo     2. Allow test-signed drivers, then REBOOT:
echo.
echo          bcdedit /set testsigning on
echo.
echo     3. Sign each build with tools\signdriver.cmd before copying it.
echo.
endlocal
exit /b 0
