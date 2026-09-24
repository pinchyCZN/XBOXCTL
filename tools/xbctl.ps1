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
        xbctl.ps1 nudge 100 0          move the pointer exactly that far
        xbctl.ps1 nudgetest            send known deltas, MEASURE the result
        xbctl.ps1 drift                send ONE tiny report, then watch

    DRIFT ANSWERS ONE QUESTION: after a single mouse report, does the
    cursor jump once and stop, or does it keep moving? A one-shot jump
    means something reacts to our report; continued movement means
    something starts and does not stop. They need looking for in
    completely different places.

    NUDGETEST TAKES THE EYE OUT OF IT. It parks the cursor, sends one
    report with a known delta, reads the cursor back, and prints what
    Windows actually did with it. DO NOT TOUCH THE PAD while it runs.

    NUDGE SEPARATES TWO FAULTS THAT LOOK THE SAME. The numbers go
    straight into one mouse report - no stick, no curve, no accumulator
    - so if the pointer does not do what the numbers say, the fault is
    above this driver rather than in it.

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
                 'rumble', 'trace', 'nudge', 'nudgetest', 'drift',
                 'zerotest')]
    [string]$Command = 'version',

    # TAKEN AS TEXT, NOT AS NUMBERS. Typing a command where a level
    # belongs - "set rumble 200 200" - would otherwise fail inside
    # PowerShell's parameter binder, which reports a type conversion
    # error naming a parameter the caller never mentioned. Collect
    # whatever arrives and say something useful about it below.
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Rest,

    [string]$Path,
    [int]$Index = 0
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
    Write-Host "    xbctl.ps1 nudge   <dx> <dy>   (-32767..32767)"
    Write-Host "    xbctl.ps1 nudgetest"
    Write-Host "    xbctl.ps1 drift"
    Write-Host "    xbctl.ps1 zerotest"
}

# Check the arguments before opening anything, so a typo does not need a
# handle to the driver to be told about.
if ($Command -eq 'nudge') {
    if ($null -eq $Rest -or $Rest.Count -ne 2) {
        Write-Host "  nudge takes two deltas, for example 'nudge 100 0'."
        Show-Usage
        exit 2
    }
    $parsedLeft = 0
    $parsedRight = 0
    if (-not [int]::TryParse($Rest[0], [ref]$parsedLeft) -or
        -not [int]::TryParse($Rest[1], [ref]$parsedRight)) {
        Write-Host ("  '{0} {1}' is not a pair of numbers." -f
                    $Rest[0], $Rest[1])
        exit 2
    }
    if ($parsedLeft -lt -32767 -or $parsedLeft -gt 32767 -or
        $parsedRight -lt -32767 -or $parsedRight -gt 32767) {
        Write-Host "  deltas are -32767 to 32767."
        exit 2
    }
} elseif ($Command -eq 'rumble') {
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
    nudge   = Ctl (0x800 + 8) $FILE_WRITE_ACCESS
    rawms   = Ctl (0x800 + 9) $FILE_WRITE_ACCESS
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
        $r = Invoke-Ctl $IOCTL.stats (Index-Bytes $Index) 64
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

    'zerotest' {
        # Does an ALL-ZERO mouse report move the cursor? If it does, the
        # payload is irrelevant and the fault is in delivery, not content.
        Write-Host "  DO NOT TOUCH THE PAD or the mouse."
        Write-Host ""
        Write-Host "  payload sent      cursor moved   gp  kb  ms   note"
        $cases = @(
            @(@(0,0,0,0),   'all zero - should move NOTHING'),
            @(@(0,1,0,0),   'dx=1'),
            @(@(0,0,1,0),   'dy=1'),
            @(@(0,0,0,0),   'all zero again')
        )
        foreach ($c in $cases) {
            $null = [NativeCursor]::SetCursorPos(700, 400)
            Start-Sleep -Milliseconds 250
            $b1 = New-Object NativeCursor+POINT
            $null = [NativeCursor]::GetCursorPos([ref]$b1)

            $buf = New-Object byte[] 8
            [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
            $buf[4] = [byte]$c[0][0]
            $buf[5] = [byte]$c[0][1]
            $buf[6] = [byte]$c[0][2]
            $buf[7] = [byte]$c[0][3]
            $null = Invoke-Ctl $IOCTL.rawms $buf 16
            Start-Sleep -Milliseconds 400

            $a1 = New-Object NativeCursor+POINT
            $null = [NativeCursor]::GetCursorPos([ref]$a1)
            # WHAT ELSE WENT OUT. A keyboard report emitted while only
            # a mouse report was asked for is the whole answer.
            $st = Invoke-Ctl $IOCTL.stats (Index-Bytes $Index) 128
            $gp = [BitConverter]::ToUInt32($st[0], 13 * 4)
            $kb = [BitConverter]::ToUInt32($st[0], 14 * 4)
            $ms = [BitConverter]::ToUInt32($st[0], 15 * 4)
            $bytes = '{0:x2} {1:x2} {2:x2} {3:x2}' -f $c[0][0], $c[0][1], $c[0][2], $c[0][3]
            $line = "  {0}       {1,5},{2,-5}   {3,3} {4,3} {5,3}   {6}" -f $bytes, ($a1.X - $b1.X), ($a1.Y - $b1.Y), $gp, $kb, $ms, $c[1]
            Write-Host $line
        }
        Write-Host ""
        Write-Host "  An all-zero report that moves the cursor proves the"
        Write-Host "  content is irrelevant - something reacts to the"
        Write-Host "  report arriving, not to what is in it."
    }

    'drift' {
        Write-Host "  DO NOT TOUCH THE PAD or the mouse while this runs."
        Write-Host ""

        $SPI_GETMOUSE = 3
        $SPI_SETMOUSE = 4
        $savedAccel = New-Object int[] 3
        $haveSaved = [NativeCursor]::SystemParametersInfo($SPI_GETMOUSE, 0,
                                                          $savedAccel, 0)
        if ($haveSaved) {
            $off = New-Object int[] 3
            $null = [NativeCursor]::SystemParametersInfo($SPI_SETMOUSE, 0,
                                                         $off, 0)
        }

        # PARK SOMEWHERE DIFFERENT EACH TIME.
        #
        # A single starting point cannot tell a constant PUSH from a fixed
        # DESTINATION: parking at 700,400 and landing at 857,557 fits both
        # "add 157" and "go to 857,557". Three starts settle it - a push
        # gives three different landings, a destination gives one.
        Write-Host "  start        after one dx=1 report   difference"
        foreach ($start in @(@(200, 150), @(700, 400), @(1100, 700))) {
            $null = [NativeCursor]::SetCursorPos($start[0], $start[1])
            Start-Sleep -Milliseconds 300
            $q = New-Object NativeCursor+POINT
            $null = [NativeCursor]::GetCursorPos([ref]$q)
            $sx = $q.X
            $sy = $q.Y

            $b2 = New-Object byte[] 8
            [Array]::Copy((Index-Bytes $Index), 0, $b2, 0, 4)
            [Array]::Copy([BitConverter]::GetBytes([int16]1), 0, $b2, 4, 2)
            [Array]::Copy([BitConverter]::GetBytes([int16]0), 0, $b2, 6, 2)
            $null = Invoke-Ctl $IOCTL.nudge $b2 16
            Start-Sleep -Milliseconds 400

            $null = [NativeCursor]::GetCursorPos([ref]$q)
            $line = "  {0,5},{1,-5}  {2,5},{3,-5}          {4,5},{5}" -f $sx, $sy, $q.X, $q.Y, ($q.X - $sx), ($q.Y - $sy)
            Write-Host $line
        }
        Write-Host ""
        Write-Host "  Same landing point from all three starts means the"
        Write-Host "  cursor is being PLACED, not pushed. Three different"
        Write-Host "  landings with the same difference means it is being"
        Write-Host "  pushed by a constant."
        Write-Host ""

        $null = [NativeCursor]::SetCursorPos(700, 400)
        Start-Sleep -Milliseconds 300

        $p = New-Object NativeCursor+POINT
        $null = [NativeCursor]::GetCursorPos([ref]$p)
        Write-Host "  now watching over time, parked at $($p.X),$($p.Y)"

        # A quarter second of quiet first, to show the cursor is still.
        Write-Host "  ms   cursor      moved since parking   note"
        for ($t = 1; $t -le 3; $t++) {
            Start-Sleep -Milliseconds 100
            $null = [NativeCursor]::GetCursorPos([ref]$p)
            $ms = $t * 100
            $line = "  {0,4} {1,5},{2,-5} {3,6},{4,-6}   quiet" -f $ms, $p.X, $p.Y, ($p.X - 700), ($p.Y - 400)
            Write-Host $line
        }

        # ONE report, as small as the engine will emit. A zero move is
        # refused on purpose - an all-zero mouse report is not news - so
        # one count is the smallest thing that can be sent.
        $buf = New-Object byte[] 8
        [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
        [Array]::Copy([BitConverter]::GetBytes([int16]1), 0, $buf, 4, 2)
        [Array]::Copy([BitConverter]::GetBytes([int16]0), 0, $buf, 6, 2)
        $null = Invoke-Ctl $IOCTL.nudge $buf 16
        Write-Host "  ---- sent ONE report: dx 1, dy 0 ----"

        for ($t = 1; $t -le 12; $t++) {
            Start-Sleep -Milliseconds 100
            $null = [NativeCursor]::GetCursorPos([ref]$p)
            $ms = $t * 100
            $line = "  {0,4} {1,5},{2,-5} {3,6},{4,-6}" -f $ms, $p.X, $p.Y, ($p.X - 700), ($p.Y - 400)
            Write-Host $line
        }

        if ($haveSaved) {
            $null = [NativeCursor]::SystemParametersInfo($SPI_SETMOUSE, 0,
                                                         $savedAccel, 0)
        }
        Write-Host ""
        Write-Host "  ONE count was sent. If the cursor settles about one"
        Write-Host "  pixel right, everything is correct. If it jumps once"
        Write-Host "  and stops, something reacts to our report. If it"
        Write-Host "  keeps moving, something started and did not stop."
    }

    'nudgetest' {
        Write-Host "  DO NOT TOUCH THE PAD while this runs."
        Write-Host ""

        # TURN POINTER ACCELERATION OFF FOR THE DURATION.
        #
        # With it on, Windows multiplies a delta by a curve keyed to how
        # fast the pointer is moving, so 100 counts can become 400-odd
        # pixels and the number means nothing. With it off the mapping
        # is one count to one pixel and a wrong answer is unambiguous.
        $SPI_GETMOUSE = 3
        $SPI_SETMOUSE = 4
        $savedAccel = New-Object int[] 3
        $haveSaved = [NativeCursor]::SystemParametersInfo($SPI_GETMOUSE, 0,
                                                          $savedAccel, 0)
        if ($haveSaved) {
            $accelWas = $savedAccel[2]
            $t1 = $savedAccel[0]
            $t2 = $savedAccel[1]
            Write-Host "  pointer acceleration was $accelWas, thresholds $t1 $t2"
            $off = New-Object int[] 3
            $null = [NativeCursor]::SystemParametersInfo($SPI_SETMOUSE, 0,
                                                         $off, 0)
            Write-Host "  turned off for this test, restored at the end"
        } else {
            Write-Host "  could not read the acceleration setting;"
            Write-Host "  magnitudes below may be scaled by it"
        }
        Write-Host ""

        # MEASURE THE NOISE FLOOR FIRST. If the cursor moves while
        # nothing is being sent, every measurement below is that drift
        # plus whatever we sent, and reading them as a response to our
        # reports would blame this driver for something else entirely.
        $null = [NativeCursor]::SetCursorPos(700, 400)
        Start-Sleep -Milliseconds 250
        $b0 = New-Object NativeCursor+POINT
        $null = [NativeCursor]::GetCursorPos([ref]$b0)
        Start-Sleep -Milliseconds 400
        $a0 = New-Object NativeCursor+POINT
        $null = [NativeCursor]::GetCursorPos([ref]$a0)
        $driftX = $a0.X - $b0.X
        $driftY = $a0.Y - $b0.Y

        Write-Host ("  baseline, nothing sent: cursor moved {0}, {1}" -f
                    $driftX, $driftY)
        if ([Math]::Abs($driftX) -gt 2 -or [Math]::Abs($driftY) -gt 2) {
            Write-Host ""
            Write-Host "  THE CURSOR IS MOVING ON ITS OWN, with this"
            Write-Host "  driver sending nothing at all. Whatever is"
            Write-Host "  driving it is not xboxctl, and no measurement"
            Write-Host "  below can be read as a response to our reports."
            Write-Host ""
            Write-Host "  Most likely VirtualBox mouse integration: the"
            Write-Host "  Guest Additions supply an absolute pointing"
            Write-Host "  device and the host drags the guest cursor"
            Write-Host "  toward wherever the host pointer sits."
            Write-Host "  Turn it off with Input -> Mouse Integration"
            Write-Host "  (Host+I), then run this again."
            Write-Host ""
        }
        Write-Host ""
        Write-Host "  sent dx  sent dy |  moved dx  moved dy | reports | verdict"
        Write-Host "  ----------------+--------------------+---------+--------"
        $cases = @(@(100, 0), @(-100, 0), @(0, 100), @(0, -100))
        foreach ($c in $cases) {
            # Park the cursor well away from every screen edge, so a
            # move in any direction has room and nothing is clamped.
            $null = [NativeCursor]::SetCursorPos(700, 400)
            Start-Sleep -Milliseconds 250
            $before = New-Object NativeCursor+POINT
            $null = [NativeCursor]::GetCursorPos([ref]$before)
            $emitBefore = Get-Emitted $IOCTL.trace $Index

            $buf = New-Object byte[] 8
            [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
            [Array]::Copy([BitConverter]::GetBytes([int16]$c[0]), 0,
                          $buf, 4, 2)
            [Array]::Copy([BitConverter]::GetBytes([int16]$c[1]), 0,
                          $buf, 6, 2)
            $null = Invoke-Ctl $IOCTL.nudge $buf 16

            Start-Sleep -Milliseconds 400
            $after = New-Object NativeCursor+POINT
            $null = [NativeCursor]::GetCursorPos([ref]$after)
            $emitAfter = Get-Emitted $IOCTL.trace $Index
            $reports = $emitAfter - $emitBefore

            $mx = $after.X - $before.X
            $my = $after.Y - $before.Y

            # Right sign and roughly the right size is a pass; Windows
            # pointer ballistics scale the magnitude, so only the sign
            # and the absence of cross-axis motion are judged here.
            $verdict = 'ok'
            if ($mx -eq 0 -and $my -eq 0) {
                $verdict = 'NOTHING MOVED'
            } else {
                if ($c[0] -ne 0 -and
                    [Math]::Sign($mx) -ne [Math]::Sign($c[0])) {
                    $verdict = 'X WRONG WAY'
                } elseif ($c[1] -ne 0 -and
                          [Math]::Sign($my) -ne [Math]::Sign($c[1])) {
                    $verdict = 'Y WRONG WAY'
                } elseif ($c[0] -eq 0 -and [Math]::Abs($mx) -gt 5) {
                    $verdict = 'X MOVED TOO'
                } elseif ($c[1] -eq 0 -and [Math]::Abs($my) -gt 5) {
                    $verdict = 'Y MOVED TOO'
                } elseif ($haveSaved -and
                          ([Math]::Abs([Math]::Abs($mx) -
                                       [Math]::Abs($c[0])) -gt 20 -or
                           [Math]::Abs([Math]::Abs($my) -
                                       [Math]::Abs($c[1])) -gt 20)) {
                    $verdict = 'WRONG DISTANCE'
                }
            }

            if ($reports -ne 1) { $verdict = "$verdict (not 1 report)" }
            Write-Host ("  {0,7}  {1,7} | {2,9}  {3,8} | {4,7} | {5}" -f
                        $c[0], $c[1], $mx, $my, $reports, $verdict)
        }
        if ($haveSaved) {
            $null = [NativeCursor]::SystemParametersInfo($SPI_SETMOUSE, 0,
                                                         $savedAccel, 0)
            Write-Host ""
            Write-Host "  acceleration restored."
            Write-Host ""
            Write-Host "  WITH ACCELERATION OFF, one count is one pixel."
            Write-Host "  A send of 100 that does not move about 100 is a"
            Write-Host "  real discrepancy, not a scaled one."
        }
    }

    'nudge' {
        # XC_NUDGE_REQUEST: u32 index, s16 dx, s16 dy.
        $buf = New-Object byte[] 8
        [Array]::Copy((Index-Bytes $Index), 0, $buf, 0, 4)
        [Array]::Copy([BitConverter]::GetBytes([int16]$parsedLeft), 0,
                      $buf, 4, 2)
        [Array]::Copy([BitConverter]::GetBytes([int16]$parsedRight), 0,
                      $buf, 6, 2)
        $null = Invoke-Ctl $IOCTL.nudge $buf 16
        Write-Host ("  sent dx {0}, dy {1}" -f $parsedLeft, $parsedRight)
        Write-Host "  negative dx is LEFT, negative dy is UP"
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

    }
} finally {
    $null = [NativeCtl]::CloseHandle($handle)
}
