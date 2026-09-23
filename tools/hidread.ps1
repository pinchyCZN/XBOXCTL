<#
    hidread.ps1 - open one of our HID collections and read reports from it.
    GUEST side. No elevation needed.

        powershell -ExecutionPolicy Bypass -File hidread.ps1            gamepad
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -Col 2     keyboard
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -Quiet     count only
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -NoSticks

    -NoSticks PRINTS THE WHOLE REPORT, exactly as the default does, but
    only when something OTHER THAN THE TWO ANALOG STICKS changes. The
    sticks rest a count or two either side of centre and never settle, so
    a raw dump scrolls continuously and whatever you wanted to watch is
    off the top of the window before you can read it.

    EVERYTHING ELSE STILL COUNTS AS A CHANGE - buttons, the hat, the
    layer, and both TRIGGERS, which are axes but are not sticks. Moving
    a stick updates the numbers on the next line that prints; it just
    does not cause a line to print by itself.

    WHY THIS EXISTS. It shows the wire bytes, which is the quickest way to
    see what the driver is actually emitting - report ID first, then the
    payload as ../docs/driver-plan.txt section 4.2 lays it out, then a '|'
    and the raw 20-byte packet the pad sent.

    IT DOES NOT CHANGE PendingReadCount. hidclass keeps its own fixed
    two-deep read pump against the minidriver and copies each report out to
    every open file object, so PendingReadCount reads 2 whether nothing is
    listening or five clients are. Measured on hardware.
#>
param(
    [int]$Col = 1,
    [switch]$Quiet,
    [switch]$NoSticks,
    [int]$Seconds = 0
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class NativeHid
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

    // The detail buffer is variable length. Rather than model the struct,
    // take the raw bytes: cbSize is the first 4, the path follows at +4.
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr devInfo,
        ref SP_DEVICE_INTERFACE_DATA interfaceData, IntPtr detail,
        int detailSize, ref int required, IntPtr devInfoData);

    [DllImport("setupapi.dll")]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr devInfo);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(string name, uint access,
        uint share, IntPtr sec, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadFile(IntPtr handle, byte[] buffer,
        int toRead, out int read, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
"@

function Find-CollectionPath([int]$col)
{
    $guid = [Guid]::Empty
    [NativeHid]::HidD_GetHidGuid([ref]$guid)

    # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    $set = [NativeHid]::SetupDiGetClassDevs([ref]$guid, [IntPtr]::Zero,
                                            [IntPtr]::Zero, 0x12)
    if ($set -eq [IntPtr](-1)) { throw "SetupDiGetClassDevs failed" }

    # Our hardware ID, with the collection suffix hidclass appends.
    $want = "vid_045e&pid_0285&col{0:d2}" -f $col
    $found = $null
    try {
        $i = 0
        while ($true) {
            $ifd = New-Object NativeHid+SP_DEVICE_INTERFACE_DATA
            # 32 on x64, 28 on x86: 4 + 16 + 4 + pointer, with padding.
            $ifd.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ifd)
            if (-not [NativeHid]::SetupDiEnumDeviceInterfaces($set,
                     [IntPtr]::Zero, [ref]$guid, $i, [ref]$ifd)) { break }

            # First call sizes the buffer, second fills it.
            $need = 0
            $null = [NativeHid]::SetupDiGetDeviceInterfaceDetail($set,
                        [ref]$ifd, [IntPtr]::Zero, 0, [ref]$need, [IntPtr]::Zero)
            if ($need -gt 0) {
                $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal($need)
                try {
                    # cbSize of the DETAIL_DATA header, NOT the whole buffer:
                    # 8 on x64, 6 on x86. Getting this wrong fails with
                    # ERROR_INVALID_USER_BUFFER and nothing else explains it.
                    $hdr = if ([IntPtr]::Size -eq 8) { 8 } else { 6 }
                    [Runtime.InteropServices.Marshal]::WriteInt32($buf, 0, $hdr)
                    if ([NativeHid]::SetupDiGetDeviceInterfaceDetail($set,
                            [ref]$ifd, $buf, $need, [ref]$need, [IntPtr]::Zero)) {
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
        $null = [NativeHid]::SetupDiDestroyDeviceInfoList($set)
    }
    return $found
}

$path = Find-CollectionPath $Col
if (-not $path) {
    Write-Host "  No collection $Col found for VID_045E&PID_0285."
    Write-Host "  Is the pad attached and is xboxctl bound? Run state.cmd."
    exit 1
}

Write-Host "  $path"

# SHARING BOTH WAYS IS WHAT MAKES CONCURRENT READERS POSSIBLE - an
# exclusive open would lock out joy.cpl and the second instance.
#
# GENERIC_READ IS WRITTEN IN DECIMAL ON PURPOSE. PowerShell types the
# literal 0x80000000 as a SIGNED Int32, so it arrives as -2147483648 and
# the marshaller refuses it. Casting does not help either: [uint32]0x80000000
# fails for the same reason, because the literal is already negative before
# the cast runs. 2147483648 exceeds Int32, so PowerShell types it Int64 and
# the cast to uint32 is then exact.
$GENERIC_READ  = [uint32]2147483648     # 0x80000000
$FILE_SHARE_RW = [uint32]3              # READ | WRITE
$OPEN_EXISTING = [uint32]3

$h = [NativeHid]::CreateFile($path, $GENERIC_READ, $FILE_SHARE_RW,
                             [IntPtr]::Zero, $OPEN_EXISTING, 0, [IntPtr]::Zero)
if ($h -eq [IntPtr](-1)) {
    Write-Host "  CreateFile failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    exit 1
}

Write-Host "  reading - Ctrl+C to stop"
$buf = New-Object byte[] 64
$n = 0
$start = Get-Date

# THE STICK BYTES, and only those, are excluded from the change test.
# driver-plan.txt section 4.2: bytes 3..10 are X, Y, Rx and Ry in the
# mapped payload, and bytes 29..36 are the same four axes in the raw
# tail. Bytes 11..14 are the TRIGGERS and are deliberately kept - they
# are axes, but they do not drift.
$STICK_BYTES = @(3, 4, 5, 6, 7, 8, 9, 10,
                 29, 30, 31, 32, 33, 34, 35, 36)
$lastSignificant = $null
try {
    while ($true) {
        $got = 0
        if (-not [NativeHid]::ReadFile($h, $buf, $buf.Length, [ref]$got,
                                       [IntPtr]::Zero)) {
            Write-Host "  ReadFile failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
            break
        }
        $n++
        if ($n -eq 1) { Write-Host ("  report length {0} bytes" -f $got) }

        $show = $true
        if ($NoSticks) {
            # Build a key from every byte that is not a stick axis, and
            # print only when that key changes.
            $sb = New-Object Text.StringBuilder
            for ($j = 0; $j -lt $got; $j++) {
                if ($STICK_BYTES -notcontains $j) {
                    $null = $sb.Append('{0:x2}' -f $buf[$j])
                }
            }
            $key = $sb.ToString()
            if ($key -eq $lastSignificant) {
                $show = $false
            } else {
                $lastSignificant = $key
            }
        }

        if ($show -and -not $Quiet) {
            # PRINT THE WHOLE REPORT. A truncated dump hides the raw tail,
            # and the tail is where the analog pressures are: a button that
            # reads one bit in the mapped payload has a full 0..255 value
            # there, and only the tail shows it.
            $parts = @()
            for ($j = 0; $j -lt $got; $j++) {
                # Mark where the mapped payload ends and the raw 20-byte
                # packet begins - ../docs/driver-plan.txt section 4.2.
                if ($j -eq 17) { $parts += '|' }
                $parts += ('{0:x2}' -f $buf[$j])
            }
            Write-Host ("{0,6}  {1}" -f $n, ($parts -join ' '))
        } elseif (($n % 250) -eq 0) {
            Write-Host ("  {0} reports" -f $n)
        }
        if ($Seconds -gt 0 -and
            ((Get-Date) - $start).TotalSeconds -ge $Seconds) { break }
    }
} finally {
    $null = [NativeHid]::CloseHandle($h)
    Write-Host ("  {0} reports total" -f $n)
}
