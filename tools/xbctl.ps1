<#
    xbctl.ps1 - talk to the xboxctl control device.
    GUEST side. Run ELEVATED: the control device's default ACL does not
    grant write access to ordinary users, and that ACL is the only thing
    standing between a pad's configuration and anyone logged in.

        xbctl.ps1 version              what the driver speaks
        xbctl.ps1 devices              which pads it can see
        xbctl.ps1 stats [-Index 0]     packet and report counters
        xbctl.ps1 get   -Path out.bin  read the live blob back
        xbctl.ps1 set   -Path in.bin   install a blob
        xbctl.ps1 reset                back to the built-in default

    Build a blob on the host with tools/mkconfig.py, copy it over, and
    push it with "set". Nothing here needs a replug: SET_CONFIG swaps the
    whole configuration and releases anything the old one was holding.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('version', 'devices', 'stats', 'get', 'set', 'reset')]
    [string]$Command = 'version',
    [string]$Path,
    [int]$Index = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NativeCtl
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(string name, uint access,
        uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(IntPtr handle, uint code,
        byte[] inBuf, int inLen, byte[] outBuf, int outLen,
        out int returned, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

# The codes, built the way wdm.h builds them:
#   CTL_CODE(type, fn, method, access)
#     = (type << 16) | (access << 14) | (fn << 2) | method
#
# INT64 THROUGHOUT, THEN CAST. The device type shifted left 16 is
# 0xB9C00000, which does not fit a signed Int32 - PowerShell's default
# for an integer literal - so doing this arithmetic in Int32 yields a
# negative number and the cast to uint32 then throws outright. Forcing
# the first operand to Int64 keeps every intermediate in range.
$XC_DEVICE_TYPE = [int64]0xB9C0
function Ctl([int]$fn, [int]$access)
{
    return [uint32](([int64]$XC_DEVICE_TYPE -shl 16) -bor
                    ([int64]$access -shl 14) -bor
                    ([int64]$fn -shl 2))
}

$FILE_ANY_ACCESS   = 0
$FILE_WRITE_ACCESS = 2

$IOCTL = @{
    version = Ctl (0x800 + 0) $FILE_ANY_ACCESS
    devices = Ctl (0x800 + 1) $FILE_ANY_ACCESS
    get     = Ctl (0x800 + 2) $FILE_ANY_ACCESS
    stats   = Ctl (0x800 + 3) $FILE_ANY_ACCESS
    set     = Ctl (0x800 + 4) $FILE_WRITE_ACCESS
    reset   = Ctl (0x800 + 5) $FILE_WRITE_ACCESS
}

# DECIMAL ON PURPOSE. PowerShell types the literal 0x80000000 as a SIGNED
# Int32, so it arrives as -2147483648 and the marshaller refuses it, and
# casting does not help because the literal is negative before the cast
# runs. These exceed Int32, so PowerShell types them Int64 and the cast
# down to uint32 is exact.
$GENERIC_READ  = [uint32]2147483648     # 0x80000000
$GENERIC_WRITE = [uint32]1073741824     # 0x40000000
$FILE_SHARE_RW = [uint32]3
$OPEN_EXISTING = [uint32]3

$handle = [NativeCtl]::CreateFile("\\.\xboxctl",
                                  $GENERIC_READ -bor $GENERIC_WRITE,
                                  $FILE_SHARE_RW, [IntPtr]::Zero,
                                  $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($handle -eq [IntPtr](-1)) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "  cannot open \\.\xboxctl: error $err"
    if ($err -eq 2) {
        Write-Host "  THE CONTROL DEVICE EXISTS ONLY WHILE A PAD IS"
        Write-Host "  PLUGGED IN. It arrives with the first pad and goes"
        Write-Host "  with the last, so that unplugging lets the driver"
        Write-Host "  unload and the next deploy.cmd maps a new build."
        Write-Host "  Plug the pad in, or run deploy.cmd if it is in."
    } elseif ($err -eq 5) {
        Write-Host "  Access denied - run this from an ELEVATED prompt."
    }
    exit 1
}

function Invoke-Ctl([uint32]$code, [byte[]]$inBuf, [int]$outLen)
{
    if ($null -eq $inBuf) { $inBuf = New-Object byte[] 0 }
    $outBuf = New-Object byte[] $outLen
    $returned = 0
    $ok = [NativeCtl]::DeviceIoControl($handle, $code, $inBuf, $inBuf.Length,
                                       $outBuf, $outLen, [ref]$returned,
                                       [IntPtr]::Zero)
    if (-not $ok) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "DeviceIoControl failed: error $err"
    }
    return ,@($outBuf, $returned)
}

function Index-Bytes([int]$i) { return [BitConverter]::GetBytes([uint32]$i) }

try {
    switch ($Command) {

    'version' {
        $r = Invoke-Ctl $IOCTL.version $null 64
        $b = $r[0]
        $sig = [BitConverter]::ToUInt32($b, 0)
        Write-Host ("  signature      0x{0:X8} {1}" -f $sig,
                    $(if ($sig -eq 0x4C435458) { "ok" } else { "UNEXPECTED" }))
        Write-Host ("  driver         {0}.{1}.{2}" -f
                    [BitConverter]::ToUInt16($b, 4),
                    [BitConverter]::ToUInt16($b, 6),
                    [BitConverter]::ToUInt16($b, 8))
        Write-Host ("  blob version   {0}" -f [BitConverter]::ToUInt16($b, 10))
        Write-Host ("  ceilings       {0} layouts, {1} bindings, {2} chords" -f
                    [BitConverter]::ToUInt16($b, 12),
                    [BitConverter]::ToUInt16($b, 14),
                    [BitConverter]::ToUInt16($b, 16))
        Write-Host ("  blob size      {0} bytes" -f
                    [BitConverter]::ToUInt16($b, 18))
    }

    'devices' {
        $r = Invoke-Ctl $IOCTL.devices $null 256
        $b = $r[0]
        $count = [BitConverter]::ToUInt32($b, 0)
        Write-Host ("  {0} pad(s)" -f $count)
        for ($i = 0; $i -lt $count; $i++) {
            $at = 4 + $i * 12
            Write-Host ("    index {0}  VID_{1:X4}&PID_{2:X4}  {3}" -f
                        [BitConverter]::ToUInt32($b, $at),
                        [BitConverter]::ToUInt16($b, $at + 4),
                        [BitConverter]::ToUInt16($b, $at + 6),
                        $(if ($b[$at + 8] -ne 0) { "started" }
                          else { "not started" }))
        }
    }

    'stats' {
        $r = Invoke-Ctl $IOCTL.stats (Index-Bytes $Index) 64
        $b = $r[0]
        $names = @('index', 'packets accepted', 'packets rejected',
                   'reports emitted', 'reports dropped', 'poll errors',
                   'pipe resets', 'poll restarts', 'pending reads', 'layer')
        for ($i = 0; $i -lt $names.Length; $i++) {
            Write-Host ("  {0,-18} {1}" -f $names[$i],
                        [BitConverter]::ToUInt32($b, $i * 4))
        }
    }

    'get' {
        if (-not $Path) { throw "get needs -Path" }
        $r = Invoke-Ctl $IOCTL.get (Index-Bytes $Index) 8192
        [IO.File]::WriteAllBytes($Path, $r[0][0..($r[1] - 1)])
        Write-Host ("  {0}, {1} bytes" -f $Path, $r[1])
    }

    'set' {
        if (-not $Path) { throw "set needs -Path" }
        $blob = [IO.File]::ReadAllBytes($Path)
        $buf = New-Object byte[] (4 + $blob.Length)
        [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
        [Array]::Copy($blob, 0, $buf, 4, $blob.Length)
        $null = Invoke-Ctl $IOCTL.set $buf 16
        Write-Host ("  installed {0} ({1} bytes) on index {2}" -f
                    $Path, $blob.Length, $Index)
    }

    'reset' {
        $null = Invoke-Ctl $IOCTL.reset (Index-Bytes $Index) 16
        Write-Host ("  index {0} is back on the built-in default" -f $Index)
    }

    }
} finally {
    $null = [NativeCtl]::CloseHandle($handle)
}
