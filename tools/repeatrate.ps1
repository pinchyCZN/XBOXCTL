<#
    repeatrate.ps1 - measure the autofire rate the pad is actually producing.

    GUEST side. No elevation needed: it only reads.

        repeatrate.ps1 -Button 1          watch gamepad button 1
        repeatrate.ps1                    watch whichever button toggles
        repeatrate.ps1 -Button 3 -Seconds 10
        repeatrate.ps1 -Key b             watch the 'b' key instead

    -Button TAKES THE SAME NUMBER THE PROFILE DOES. "a -> joy_button 3
    repeat 20" is measured with -Button 3, and the reading should settle on
    20. Hold the control down; the figure is live and updates per press.

    THE GAMEPAD IS THE ACCURATE MODE, and it is the default. Read section
    "Why the gamepad" below before trusting a -Key figure.

    WHAT IS MEASURED IS FULL CYCLES PER SECOND, matching what "repeat N"
    means in a profile: N presses per second, so N down edges and N up
    edges. The driver toggles on a half period, so the report rate is twice
    the figure printed here.

    Why the gamepad
    ---------------
    THE GAMEPAD IS THE ONLY COLLECTION ANYTHING CAN OPEN FOR READING.
    mouhid and kbdhid hold the mouse and keyboard collections exclusively,
    so there is no way to see the driver's keyboard output directly and
    -Key has to go through the Windows input stack instead.

    A GAMEPAD REPORT IS EMITTED ONLY WHEN THE MAPPED STATE CHANGES -
    core_emit_gamepad compares the first CORE_GP_RAW bytes and returns
    early if nothing moved. So a blocking read wakes once per edge, every
    edge, with no polling and no sample rate to outrun. The timestamp is
    taken the moment the read returns.

    -Key SAMPLES, AND SAMPLING CAN MISS. It spins on GetAsyncKeyState and
    counts down transitions, so it cannot see a press and release that both
    land between two samples. The achieved sample rate is printed, and the
    script says so when that rate is too close to what it is measuring to
    be trusted. It also burns a core while it runs. Use it to confirm the
    keyboard path fires at roughly the right rate, not to measure it.

    WHY BUTTON BITS AND NOT THE REPORT COUNTER. xbctl.ps1 stats counts
    every gamepad report, and the gamepad axes are not deadzoned - a
    resting stick generates reports of its own. Keying on one button bit
    ignores all of that.

    EXPECT PHASE JITTER OF UP TO ONE 8ms TICK. The driver advances the next
    deadline from the last one rather than from now, so the average rate is
    right and individual intervals land on tick boundaries. The spread
    printed alongside is that jitter, and it is not a fault.
#>

param(
    # 0 means "latch onto the first button that toggles".
    [int]$Button = 0,

    # A single character, a name from $VK_NAMES, or 0xNN. Selects the
    # sampled keyboard mode instead of the gamepad.
    [string]$Key = '',

    [Alias('Vid')]
    [string]$VendorId = '045e',
    [Alias('Pid')]
    [string]$ProductId = '0285',

    # Presses averaged for the live figure. Smaller reacts faster, larger
    # reads steadier.
    [int]$Window = 8,

    # 0 runs until Ctrl+C.
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'

# THE GAMEPAD IS Col03. The collections are declared mouse, keyboard,
# gamepad - see src_drv/core.c, the Usage items at the head of each - and
# hidclass numbers the child devnodes in descriptor order.
$GAMEPAD_COL = 3

# Payload offsets, past the one-byte report ID. CORE_GP_BUTTONS is 0, so
# the sixteen button bits are report bytes 1 and 2, little endian.
$BUTTON_LO = 1

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NativeRr
{
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA
    {
        public int cbSize;
        public Guid InterfaceClassGuid;
        public int Flags;
        public IntPtr Reserved;
    }

    [DllImport("hid.dll")]
    public static extern void HidD_GetHidGuid(out Guid hidGuid);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid,
        IntPtr enumerator, IntPtr hwndParent, int flags);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr devInfo,
        IntPtr devInfoData, ref Guid interfaceClassGuid, int memberIndex,
        ref SP_DEVICE_INTERFACE_DATA interfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr devInfo,
        ref SP_DEVICE_INTERFACE_DATA interfaceData, IntPtr detail,
        int detailSize, ref int required, IntPtr devInfoData);

    [DllImport("setupapi.dll")]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr devInfo);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(string path, uint access,
        uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    // OVERLAPPED AS IntPtr, NOT ref. A ref struct is marshalled into a
    // temporary that is freed when the call returns, and an overlapped read
    // that returns ERROR_IO_PENDING leaves the kernel holding that pointer
    // and writing to it later. It segfaults the host. The caller allocates
    // the structure instead and keeps it alive across the wait.
    public static extern bool ReadFile(IntPtr handle, byte[] buffer,
        int toRead, IntPtr read, IntPtr ov);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr CreateEvent(IntPtr sec, bool manualReset,
        bool initial, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr handle, uint ms);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ResetEvent(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetOverlappedResult(IntPtr handle,
        IntPtr ov, out int transferred, bool wait);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CancelIo(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);

    [DllImport("user32.dll")]
    public static extern short GetAsyncKeyState(int vk);
}
"@

# GENERIC_READ IN DECIMAL ON PURPOSE. PowerShell types 0x80000000 as a
# signed Int32, so it arrives negative and the marshaller refuses it;
# casting does not help because the literal is negative before the cast.
$GENERIC_READ   = [uint32]2147483648
$FILE_SHARE_RW  = [uint32]3
$OPEN_EXISTING  = [uint32]3
$FLAG_OVERLAPPED = [uint32]1073741824   # 0x40000000

$ERROR_IO_PENDING = 997
$WAIT_OBJECT_0    = 0
$WAIT_TIMEOUT     = 258

$VK_NAMES = @{
    'space' = 0x20; 'enter' = 0x0D; 'return' = 0x0D; 'tab' = 0x09
    'esc' = 0x1B; 'escape' = 0x1B; 'back' = 0x08; 'backspace' = 0x08
    'left' = 0x25; 'up' = 0x26; 'right' = 0x27; 'down' = 0x28
    'lshift' = 0xA0; 'rshift' = 0xA1; 'lctrl' = 0xA2; 'rctrl' = 0xA3
    'lalt' = 0xA4; 'ralt' = 0xA5
    'f1' = 0x70; 'f2' = 0x71; 'f3' = 0x72; 'f4' = 0x73; 'f5' = 0x74
    'f6' = 0x75; 'f7' = 0x76; 'f8' = 0x77; 'f9' = 0x78; 'f10' = 0x79
    'f11' = 0x7A; 'f12' = 0x7B
}

function Resolve-Vk([string]$s)
{
    $t = $s.Trim().ToLower()
    if ($VK_NAMES.ContainsKey($t)) { return $VK_NAMES[$t] }
    if ($t -match '^0x[0-9a-f]{1,2}$') { return [Convert]::ToInt32($t, 16) }
    if ($t.Length -eq 1) {
        $c = $t.ToUpper()[0]
        if (($c -ge 'A' -and $c -le 'Z') -or ($c -ge '0' -and $c -le '9')) {
            return [int]$c
        }
    }
    return -1
}

function Find-CollectionPath([int]$col)
{
    $guid = [Guid]::Empty
    [NativeRr]::HidD_GetHidGuid([ref]$guid)

    # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    $set = [NativeRr]::SetupDiGetClassDevs([ref]$guid, [IntPtr]::Zero,
                                           [IntPtr]::Zero, 0x12)
    if ($set -eq [IntPtr](-1)) { throw "SetupDiGetClassDevs failed" }

    $want = ("vid_{0}&pid_{1}&col{2:d2}" -f $VendorId, $ProductId,
                                             $col).ToLower()
    $found = $null
    try {
        $i = 0
        while ($true) {
            $ifd = New-Object NativeRr+SP_DEVICE_INTERFACE_DATA
            $ifd.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ifd)
            if (-not [NativeRr]::SetupDiEnumDeviceInterfaces($set,
                     [IntPtr]::Zero, [ref]$guid, $i, [ref]$ifd)) { break }

            $need = 0
            $null = [NativeRr]::SetupDiGetDeviceInterfaceDetail($set,
                        [ref]$ifd, [IntPtr]::Zero, 0, [ref]$need,
                        [IntPtr]::Zero)
            if ($need -gt 0) {
                $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($need)
                try {
                    # cbSize of the DETAIL header, not of the buffer: 8 on
                    # x64, 6 on x86. Wrong gives ERROR_INVALID_USER_BUFFER
                    # and nothing else explains it.
                    $hdr = if ([IntPtr]::Size -eq 8) { 8 } else { 6 }
                    [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, $hdr)
                    if ([NativeRr]::SetupDiGetDeviceInterfaceDetail($set,
                            [ref]$ifd, $buf, $need, [ref]$need,
                            [IntPtr]::Zero)) {
                        $path = [Runtime.InteropServices.Marshal]::PtrToStringUni(
                                    [IntPtr]($buf.ToInt64() + 4))
                        if ($path -and $path.ToLower().Contains($want)) {
                            $found = $path
                        }
                    }
                } finally {
                    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
                }
            }
            if ($found) { break }
            $i++
        }
    } finally {
        $null = [NativeRr]::SetupDiDestroyDeviceInfoList($set)
    }
    return $found
}

# ----------------------------------------------------------------------
# The live readout. One line, rewritten in place.
# ----------------------------------------------------------------------

$script:presses = New-Object 'System.Collections.Generic.Queue[double]'
$script:total   = 0
$script:firstMs = 0.0
$script:lastMs  = 0.0

function Add-Press([double]$ms)
{
    if ($script:total -eq 0) { $script:firstMs = $ms }
    $script:lastMs = $ms
    $script:total++

    $script:presses.Enqueue($ms)
    while ($script:presses.Count -gt $Window) {
        $null = $script:presses.Dequeue()
    }
}

function Show-Rate([string]$what, [string]$extra)
{
    $a = $script:presses.ToArray()
    $live = 0.0
    $lo = 0.0
    $hi = 0.0
    if ($a.Count -ge 2) {
        $span = $a[$a.Count - 1] - $a[0]
        if ($span -gt 0) { $live = ($a.Count - 1) * 1000.0 / $span }
        $lo = [double]::MaxValue
        $hi = 0.0
        for ($i = 1; $i -lt $a.Count; $i++) {
            $d = $a[$i] - $a[$i - 1]
            if ($d -lt $lo) { $lo = $d }
            if ($d -gt $hi) { $hi = $d }
        }
    }
    $avg = 0.0
    $span = $script:lastMs - $script:firstMs
    if ($script:total -ge 2 -and $span -gt 0) {
        $avg = ($script:total - 1) * 1000.0 / $span
    }

    $line = "  {0}  {1,6:f2} Hz live   {2,6:f2} Hz avg   {3,5} presses" -f `
            $what, $live, $avg, $script:total
    if ($a.Count -ge 2) {
        $line += "   interval {0,5:f1}-{1,5:f1} ms" -f $lo, $hi
    }
    if ($extra) { $line += "   $extra" }
    Write-Host ("`r" + $line.PadRight(100)) -NoNewline
}

# ----------------------------------------------------------------------
# Keyboard: sampled
# ----------------------------------------------------------------------

if ($Key -ne '') {
    $vk = Resolve-Vk $Key
    if ($vk -lt 0) {
        Write-Host "  Do not know the key '$Key'."
        Write-Host "  Give a single letter or digit, 0xNN, or one of:"
        Write-Host ("    " + (($VK_NAMES.Keys | Sort-Object) -join ' '))
        exit 2
    }

    Write-Host ""
    Write-Host ("  Sampling VK 0x{0:X2} ('{1}'). SAMPLED, NOT EXACT - see the" -f $vk, $Key)
    Write-Host  "  header. Hold the control down. Ctrl+C to stop."
    Write-Host ""

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $prev = $false
    $samples = 0
    $nextShow = 0.0
    $stopAt = if ($Seconds -gt 0) { $Seconds * 1000.0 } else { [double]::MaxValue }

    while ($sw.Elapsed.TotalMilliseconds -lt $stopAt) {
        $down = ([NativeRr]::GetAsyncKeyState($vk) -band 0x8000) -ne 0
        $samples++
        if ($down -and -not $prev) {
            Add-Press $sw.Elapsed.TotalMilliseconds
        }
        $prev = $down

        $now = $sw.Elapsed.TotalMilliseconds
        if ($now -ge $nextShow) {
            $nextShow = $now + 200
            $sps = if ($now -gt 0) { $samples * 1000.0 / $now } else { 0 }
            $warn = ''
            # A rate within an eighth of the sample rate is not measured,
            # it is aliased.
            $a = $script:presses.ToArray()
            if ($a.Count -ge 2) {
                $span = $a[$a.Count - 1] - $a[0]
                if ($span -gt 0) {
                    $live = ($a.Count - 1) * 1000.0 / $span
                    if ($live * 8 -gt $sps) { $warn = 'SAMPLE RATE TOO LOW' }
                }
            }
            Show-Rate ("key 0x{0:X2}" -f $vk) `
                      ("{0,5:f0} samples/s {1}" -f $sps, $warn)
        }
    }
    Write-Host ""
    Write-Host ""
    exit 0
}

# ----------------------------------------------------------------------
# Gamepad: every edge, exactly
# ----------------------------------------------------------------------

if ($Button -lt 0 -or $Button -gt 16) {
    Write-Host "  -Button takes 1 to 16, or 0 to latch onto the first that moves."
    exit 2
}

$path = Find-CollectionPath $GAMEPAD_COL
if (-not $path) {
    Write-Host "  No collection $GAMEPAD_COL for VID_$VendorId&PID_$ProductId."
    Write-Host "  Is the pad plugged in and is xboxctl bound? Run state.cmd."
    exit 1
}

$h = [NativeRr]::CreateFile($path, $GENERIC_READ, $FILE_SHARE_RW,
                            [IntPtr]::Zero, $OPEN_EXISTING,
                            $FLAG_OVERLAPPED, [IntPtr]::Zero)
if ($h -eq [IntPtr](-1)) {
    $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Write-Host "  CreateFile failed: $e"
    if ($e -eq 32) { Write-Host "  Something holds it exclusively." }
    exit 1
}

# MANUAL RESET, AND RESET BY HAND BEFORE EACH READ. An auto-reset event
# left signalled by a cancelled read makes the next wait return at once.
$ev = [NativeRr]::CreateEvent([IntPtr]::Zero, $true, $false, $null)

# Internal, InternalHigh, Offset, OffsetHigh, hEvent: 32 bytes on x64 and
# 20 on x86. hEvent sits after the two pointers and the two DWORDs.
$OV_SIZE   = 2 * [IntPtr]::Size + 8 + [IntPtr]::Size
$OV_HEVENT = 2 * [IntPtr]::Size + 8
$ovp = [Runtime.InteropServices.Marshal]::AllocHGlobal($OV_SIZE)

Write-Host ""
if ($Button -eq 0) {
    Write-Host "  Waiting for a button to move. Hold the control down."
} else {
    Write-Host "  Watching gamepad button $Button. Hold the control down."
}
Write-Host "  Ctrl+C to stop."
Write-Host ""

$buf = New-Object byte[] 64
$sw = [Diagnostics.Stopwatch]::StartNew()
$prevMask = $null
$watch = $Button
$stopAt = if ($Seconds -gt 0) { $Seconds * 1000.0 } else { [double]::MaxValue }
$nextShow = 0.0
$idle = 0

try {
    while ($sw.Elapsed.TotalMilliseconds -lt $stopAt) {
        # RESET BEFORE EVERY READ, both the event and the status words. A
        # manual-reset event left signalled by the previous read makes the
        # next wait return at once, and stale Internal fields make
        # GetOverlappedResult answer about the read before this one.
        for ($z = 0; $z -lt $OV_SIZE; $z++) {
            [Runtime.InteropServices.Marshal]::WriteByte($ovp, $z, 0)
        }
        [Runtime.InteropServices.Marshal]::WriteIntPtr($ovp, $OV_HEVENT, $ev)
        $null = [NativeRr]::ResetEvent($ev)

        $ok = [NativeRr]::ReadFile($h, $buf, $buf.Length, [IntPtr]::Zero,
                                   $ovp)
        if (-not $ok) {
            $e = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            if ($e -ne $ERROR_IO_PENDING) {
                Write-Host ""
                Write-Host "  ReadFile failed: $e"
                break
            }
            # WAIT WITH A TIMEOUT SO Ctrl+C IS REACHABLE. A blocking read
            # on a pad that has stopped changing never returns, and the
            # script would look frozen rather than idle.
            $w = [NativeRr]::WaitForSingleObject($ev, 250)
            if ($w -eq $WAIT_TIMEOUT) {
                $null = [NativeRr]::CancelIo($h)
                # WAIT FOR THE CANCEL TO LAND before reusing the structure.
                # The read is still outstanding until this returns.
                $got = 0
                $null = [NativeRr]::GetOverlappedResult($h, $ovp,
                                                        [ref]$got, $true)
                $idle++
                $label = if ($watch -gt 0) { "button $watch" } else { 'button ?' }
                if ($script:total -gt 0) {
                    Show-Rate $label 'idle'
                } elseif ($idle % 8 -eq 0) {
                    $msg = "`r  nothing moving yet..."
                    Write-Host $msg.PadRight(60) -NoNewline
                }
                continue
            }
        }

        $got = 0
        if (-not [NativeRr]::GetOverlappedResult($h, $ovp, [ref]$got,
                                                 $false)) {
            continue
        }
        $ms = $sw.Elapsed.TotalMilliseconds
        if ($got -lt 3) { continue }

        $mask = [BitConverter]::ToUInt16($buf, $BUTTON_LO)

        if ($null -ne $prevMask) {
            $changed = $mask -bxor $prevMask
            if ($watch -eq 0 -and $changed -ne 0) {
                # LATCH ONTO THE LOWEST BIT THAT MOVED.
                for ($b = 1; $b -le 16; $b++) {
                    if ($changed -band (1 -shl ($b - 1))) { $watch = $b; break }
                }
                $msg = ("`r  latched onto button {0}." -f $watch)
                Write-Host $msg.PadRight(60)
            }
            if ($watch -gt 0) {
                $bit = 1 -shl ($watch - 1)
                # COUNT DOWN EDGES ONLY. One press is one cycle, which is
                # what "repeat N" counts; edges of both kinds would read
                # double.
                if (($changed -band $bit) -and ($mask -band $bit)) {
                    Add-Press $ms
                }
            }
        }
        $prevMask = $mask

        if ($ms -ge $nextShow -and $script:total -gt 0) {
            $nextShow = $ms + 150
            Show-Rate ("button {0}" -f $watch) ''
        }
    }
} finally {
    $null = [NativeRr]::CancelIo($h)
    $null = [NativeRr]::CloseHandle($ev)
    $null = [NativeRr]::CloseHandle($h)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($ovp)
    Write-Host ""
    Write-Host ""
    if ($script:total -ge 2) {
        $span = $script:lastMs - $script:firstMs
        $avg = ($script:total - 1) * 1000.0 / $span
        Write-Host ("  {0} presses over {1:f2} s = {2:f2} Hz average." -f
                    $script:total, ($span / 1000.0), $avg)
        Write-Host  "  Compare against the repeat value in the profile."
    } else {
        Write-Host "  Saw no repeats. Is the control bound with a repeat, and held?"
    }
}
