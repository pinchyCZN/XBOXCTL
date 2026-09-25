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
        xbctl.ps1 rumble 200 100       shake it: left, right, 0..255
        xbctl.ps1 rumble 0 0           stop
        xbctl.ps1 trace                the last 32 mouse reports emitted
        xbctl.ps1 raw                  the last packet, untranslated
        xbctl.ps1 raw -Watch           and again every time it changes

    RAW IS THE PAD BEFORE ANYTHING IS DONE TO IT. No deadzone, no curve,
    no binding, no suppression. It is how a configurator asks "which
    control did you just press", and it works for a control already
    bound to a key - which the gamepad report does not, because that
    report is emitted only when the MAPPED state changes and a bound
    control changes nothing an application can see.

    TRACE IS THE ONLY VIEW OF THE POINTER PATH on a live system. mouhid
    opens the mouse collection exclusively, so reading those reports from
    user mode returns ACCESS_DENIED and there is no way around it. The
    driver records what it emitted instead.

    RUMBLE IS A LEVEL, NOT A PULSE. It runs until something sets it back
    to zero, this script included, so "rumble 0 0" is how it stops.

    NO GAME WILL EVER DRIVE THIS. A vendor HID output report can only be
    sent by software written for this exact report; getting rumble out of
    an existing game needs HID PID force feedback, which is a project of
    its own. See ../docs/driver-plan.txt section 8.

    Build a blob on the host with tools/mkconfig.py, copy it over, and
    push it with "set". Nothing here needs a replug: SET_CONFIG swaps the
    whole configuration and releases anything the old one was holding.
#>
param(
    [Parameter(Position = 0)]
    [ValidateSet('version', 'devices', 'stats', 'get', 'set', 'reset',
                 'rumble', 'trace', 'raw')]
    [string]$Command = 'version',

    # TAKEN AS TEXT, NOT AS NUMBERS. Typing a command where a level
    # belongs - "set rumble 200 200" - would otherwise fail inside
    # PowerShell's parameter binder, which reports a type conversion
    # error naming a parameter the caller never mentioned. Collect
    # whatever arrives and say something useful about it below.
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Rest,

    [string]$Path,
    [int]$Index = 0,

    # 'raw -Watch' keeps printing as the pad changes, which is what
    # makes it usable for "press the control you want to bind".
    [switch]$Watch
)

$ErrorActionPreference = 'Stop'

function Show-Usage
{
    Write-Host "  usage:"
    Write-Host "    xbctl.ps1 version"
    Write-Host "    xbctl.ps1 devices"
    Write-Host "    xbctl.ps1 stats   [-Index 0]"
    Write-Host "    xbctl.ps1 get     -Path out.bin"
    Write-Host "    xbctl.ps1 set     -Path in.bin"
    Write-Host "    xbctl.ps1 reset"
    Write-Host "    xbctl.ps1 rumble  <left 0-255> <right 0-255>"
    Write-Host "    xbctl.ps1 trace   [-Index 0]"
    Write-Host "    xbctl.ps1 raw     [-Index 0] [-Watch]"
}

# Check the arguments before opening anything, so a typo does not need a
# handle to the driver to be told about.
if ($Command -eq 'rumble') {
    if ($null -eq $Rest -or $Rest.Count -ne 2) {
        Write-Host "  rumble takes two levels, 0 to 255."
        Write-Host "  'rumble 200 200' to shake, 'rumble 0 0' to stop."
        Show-Usage
        exit 2
    }
    $parsedLeft = 0
    $parsedRight = 0
    if (-not [int]::TryParse($Rest[0], [ref]$parsedLeft) -or
        -not [int]::TryParse($Rest[1], [ref]$parsedRight)) {
        Write-Host ("  '{0} {1}' is not a pair of numbers." -f
                    $Rest[0], $Rest[1])
        Show-Usage
        exit 2
    }
    if ($parsedLeft -lt 0 -or $parsedLeft -gt 255 -or
        $parsedRight -lt 0 -or $parsedRight -gt 255) {
        Write-Host "  actuator levels are 0 to 255."
        exit 2
    }
} elseif ($null -ne $Rest -and $Rest.Count -gt 0) {
    Write-Host ("  '{0}' takes no extra arguments, but got: {1}" -f
                $Command, ($Rest -join ' '))
    if ($Rest -contains 'rumble') {
        Write-Host "  Did you mean just 'rumble 200 200'? 'set' pushes a"
        Write-Host "  configuration blob and wants -Path."
    }
    Show-Usage
    exit 2
}

if (($Command -eq 'set' -or $Command -eq 'get') -and -not $Path) {
    Write-Host ("  {0} needs -Path" -f $Command)
    Show-Usage
    exit 2
}

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
    rumble  = Ctl (0x800 + 6) $FILE_WRITE_ACCESS
    trace   = Ctl (0x800 + 7) $FILE_ANY_ACCESS
    raw     = Ctl (0x800 + 8) $FILE_ANY_ACCESS
}

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeCursor
{
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")]
    public static extern bool GetCursorPos(out POINT p);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    // SPI_GETMOUSE / SPI_SETMOUSE take int[3]:
    //   threshold1, threshold2, acceleration
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SystemParametersInfo(uint action, uint param,
        int[] pv, uint winIni);
}
"@

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
        if ($err -eq 50) {
            throw ("the pad reports no OUT endpoint, so it cannot" +
                   " rumble (error 50)")
        }
        throw "DeviceIoControl failed: error $err"
    }
    return ,@($outBuf, $returned)
}

function Index-Bytes([int]$i) { return [BitConverter]::GetBytes([uint32]$i) }

# How many mouse reports the driver has emitted in total. Comparing this
# across a measurement window says whether the cursor moved because of
# ONE report or several - which distinguishes a misread report from a
# report nobody meant to send.
function Get-Emitted([uint32]$code, [int]$idx)
{
    $r = Invoke-Ctl $code (Index-Bytes $idx) 1024
    return [BitConverter]::ToUInt32($r[0], 4)
}

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
        # 256, not 64. XC_STATS grew when the per-report-id counters
        # were added, and a buffer sized to the old struct comes back
        # as error 122 - which reads like a dead device, not a short
        # buffer.
        $r = Invoke-Ctl $IOCTL.stats (Index-Bytes $Index) 256
        $b = $r[0]
        $names = @('index', 'packets accepted', 'packets rejected',
                   'reports emitted', 'reports dropped', 'poll errors',
                   'pipe resets', 'poll restarts', 'pending reads', 'layer',
                   'rumble sent', 'rumble errors')
        for ($i = 0; $i -lt $names.Length; $i++) {
            Write-Host ("  {0,-18} {1}" -f $names[$i],
                        [BitConverter]::ToUInt32($b, $i * 4))
        }
        # Reports emitted per report ID: 1 gamepad, 2 keyboard, 3 mouse.
        Write-Host "  reports by id:"
        $idn = @('-', 'gamepad', 'keyboard', 'mouse', 'rumble',
                 '5', '6', '7')
        for ($i = 1; $i -lt 8; $i++) {
            $v = [BitConverter]::ToUInt32($b, (12 + $i) * 4)
            if ($v -ne 0) {
                Write-Host ("    id {0} {1,-9} {2}" -f $i, $idn[$i], $v)
            }
        }
    }

    'get' {
        $r = Invoke-Ctl $IOCTL.get (Index-Bytes $Index) 8192
        [IO.File]::WriteAllBytes($Path, $r[0][0..($r[1] - 1)])
        Write-Host ("  {0}, {1} bytes" -f $Path, $r[1])
    }

    'set' {
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

    'trace' {
        # XC_TRACE: u32 count, u32 emitted, then 32 entries of
        # { s16 dx, s16 dy, u8 buttons, 3 reserved }.
        $r = Invoke-Ctl $IOCTL.trace (Index-Bytes $Index) 1024
        $b = $r[0]
        $count = [BitConverter]::ToUInt32($b, 0)
        $emitted = [BitConverter]::ToUInt32($b, 4)
        Write-Host ("  {0} mouse report(s) emitted in total" -f $emitted)
        if ($count -eq 0) {
            Write-Host "  nothing recorded - move the stick, then try again"
        } else {
            Write-Host "     dx      dy  buttons"
            $sumX = 0
            $sumY = 0
            for ($i = 0; $i -lt $count; $i++) {
                $at = 8 + $i * 8
                $dx = [BitConverter]::ToInt16($b, $at)
                $dy = [BitConverter]::ToInt16($b, $at + 2)
                $btn = $b[$at + 4]
                $sumX += $dx
                $sumY += $dy
                Write-Host ("  {0,5}  {1,6}     0x{2:x2}" -f $dx, $dy, $btn)
            }
            Write-Host ("  ----- oldest first. sum dx {0}, dy {1}" -f
                        $sumX, $sumY)
        }
    }

    'rumble' {
        # XC_RUMBLE_REQUEST: u32 index, u8 left, u8 right, 2 reserved.
        $buf = New-Object byte[] 8
        [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
        $buf[4] = [byte]$parsedLeft
        $buf[5] = [byte]$parsedRight
        $null = Invoke-Ctl $IOCTL.rumble $buf 16
        if ($parsedLeft -eq 0 -and $parsedRight -eq 0) {
            Write-Host "  stopped"
        } else {
            Write-Host ("  left {0}, right {1}" -f $parsedLeft, $parsedRight)
            Write-Host "  runs until you set it back to 0 0"
        }
    }

    'raw' {
        # XC_RAW_INFO: u32 index, u32 valid, u32 sequence, u32 layer,
        # u64 when_100ns, then the 20 packet bytes and 4 reserved.
        #
        # THE PACKET AS THE PAD SENT IT. Nothing here has been through a
        # deadzone, a curve or a binding, and a control bound to a key
        # shows up exactly the same as one that is not - which is the
        # whole reason this exists rather than reading the raw tail off
        # the gamepad report.
        function Show-Raw
        {
            $r = Invoke-Ctl $IOCTL.raw (Index-Bytes $Index) 64
            $b = $r[0]
            $valid = [BitConverter]::ToUInt32($b, 4)
            $seq   = [BitConverter]::ToUInt32($b, 8)
            $layer = [BitConverter]::ToUInt32($b, 12)
            if ($valid -eq 0) {
                Write-Host "  no packet decoded yet - is the pad plugged in?"
                return $null
            }
            $hex = ''
            for ($i = 0; $i -lt 20; $i++) {
                $hex += '{0:x2} ' -f $b[24 + $i]
            }
            Write-Host ("  seq {0,-6} layer {1}   {2}" -f $seq, $layer,
                        $hex.TrimEnd())
            return $seq
        }

        if ($Watch) {
            Write-Host "  press controls on the pad. Ctrl-C to stop."
            Write-Host ""
            $last = -1
            while ($true) {
                $r = Invoke-Ctl $IOCTL.raw (Index-Bytes $Index) 64
                $seq = [BitConverter]::ToUInt32($r[0], 8)
                if ($seq -ne $last) {
                    $null = Show-Raw
                    $last = $seq
                }
                Start-Sleep -Milliseconds 30
            }
        } else {
            $null = Show-Raw
        }
    }

    }
} finally {
    $null = [NativeCtl]::CloseHandle($handle)
}
