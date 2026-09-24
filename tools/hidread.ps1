<#
    hidread.ps1 - open one of our HID collections and read reports from it.
    GUEST side. No elevation needed.

        powershell -ExecutionPolicy Bypass -File hidread.ps1            gamepad
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -Col 2     keyboard
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -Quiet     count only
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -NoSticks
        powershell -ExecutionPolicy Bypass -File hidread.ps1 -Caps -Col 3

    -Caps SAYS WHAT WINDOWS THINKS THE REPORTS LOOK LIKE, which is not
    always what the driver is sending. Windows reads the report
    descriptor once, when the device starts; changing the descriptor and
    reloading the driver is not enough, because the device has to be
    restarted for the new one to be read. Until it is, the old layout is
    applied to the new reports and the fields land in the wrong places.

    IT WORKS ON THE MOUSE AND KEYBOARD, which -Col 2 and -Col 3 cannot
    otherwise be read at all: the handle is opened with NO access rights,
    which HID allows for queries even when mouhid and kbdhid hold the
    collection exclusively for reading.

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
    # DEFAULTS TO THE XBOX PAD. Pass another to compare a device that
    # works - the Adaptoid is 06F7:0001 and its mouse is Col01.
    #
    # NOT $Pid. THAT IS AN AUTOMATIC READ-ONLY VARIABLE - the process id -
    # and a parameter of that name fails at bind time with an error that
    # mentions neither the script nor the parameter. The short spellings
    # survive as aliases.
    [Alias('Vid')]
    [string]$VendorId = '045e',
    [Alias('Pid')]
    [string]$ProductId = '0285',
    [int]$Col = 1,
    [switch]$Quiet,
    [switch]$NoSticks,
    [switch]$Caps,
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

    [DllImport("hid.dll", SetLastError = true)]
    public static extern bool HidD_GetPreparsedData(IntPtr handle,
        out IntPtr preparsed);

    [DllImport("hid.dll")]
    public static extern bool HidD_FreePreparsedData(IntPtr preparsed);

    [DllImport("hid.dll")]
    public static extern int HidP_GetCaps(IntPtr preparsed, byte[] caps);

    // HidP_Input = 0. caps is an array of HIDP_VALUE_CAPS.
    [DllImport("hid.dll")]
    public static extern int HidP_GetValueCaps(int reportType,
        byte[] caps, ref ushort capsLength, IntPtr preparsed);

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
    $want = ("vid_{0}&pid_{1}&col{2:d2}" -f $VendorId, $ProductId,
                                              $col).ToLower()
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
    Write-Host "  No collection $Col found for VID_$VendorId&PID_$ProductId."
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

if ($Caps) {
    # ZERO ACCESS RIGHTS. HID allows a query-only handle even where
    # mouhid or kbdhid holds the collection exclusively for reading, and
    # that is the only way to see the mouse and keyboard collections.
    $q = [NativeHid]::CreateFile($path, 0, $FILE_SHARE_RW, [IntPtr]::Zero,
                                 $OPEN_EXISTING, 0, [IntPtr]::Zero)
    if ($q -eq [IntPtr](-1)) {
        Write-Host ("  CreateFile failed: {0}" -f
                    [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        exit 1
    }
    $q2 = $q
    $pp = [IntPtr]::Zero
    if (-not [NativeHid]::HidD_GetPreparsedData($q, [ref]$pp)) {
        Write-Host "  HidD_GetPreparsedData failed"
        $null = [NativeHid]::CloseHandle($q)
        exit 1
    }
    # HIDP_CAPS begins: Usage, UsagePage, InputReportByteLength,
    # OutputReportByteLength, FeatureReportByteLength - five USHORTs.
    # NOT $caps - PowerShell variable names are case insensitive, so
    # that IS the -Caps switch parameter, and assigning a byte array
    # to a switch fails at run time with a type error that names
    # neither the variable nor the parameter.
    $capsBuf = New-Object byte[] 256
    $st = [NativeHid]::HidP_GetCaps($pp, $capsBuf)
    $null = [NativeHid]::HidD_FreePreparsedData($pp)
    if ($st -ne 0x00110000) {
        Write-Host ("  HidP_GetCaps returned 0x{0:X8}" -f $st)
        exit 1
    }
    Write-Host ("  usage page 0x{0:X2}  usage 0x{1:X2}" -f
                [BitConverter]::ToUInt16($capsBuf, 2),
                [BitConverter]::ToUInt16($capsBuf, 0))
    Write-Host ("  input report   {0} bytes" -f
                [BitConverter]::ToUInt16($capsBuf, 4))
    Write-Host ("  output report  {0} bytes" -f
                [BitConverter]::ToUInt16($capsBuf, 6))
    Write-Host ("  feature report {0} bytes" -f
                [BitConverter]::ToUInt16($capsBuf, 8))

    # THE FIELD THAT DECIDES EVERYTHING FOR A POINTER. Windows records
    # per axis whether it is absolute or relative. A relative axis is
    # applied as a delta; an absolute one has LogicalMinimum subtracted
    # first, which on a mouse adds a fixed push to every report.
    $ppd = [IntPtr]::Zero
    $null = [NativeHid]::HidD_GetPreparsedData($q2, [ref]$ppd)
    $n = [uint16]64
    $vc = New-Object byte[] (64 * 72)
    $vs = [NativeHid]::HidP_GetValueCaps(0, $vc, [ref]$n, $ppd)
    $null = [NativeHid]::HidD_FreePreparsedData($ppd)
    $null = [NativeHid]::CloseHandle($q2)
    if ($vs -eq 0x00110000) {
        Write-Host ""
        Write-Host "  axis usages Windows found in the input reports:"
        Write-Host "  rpt  usage  bits  logical min..max   absolute?"
        for ($k = 0; $k -lt $n; $k++) {
            $b = $k * 72
            $rid  = $vc[$b + 2]
            $abs  = $vc[$b + 15]
            $bits = [BitConverter]::ToUInt16($vc, $b + 18)
            $lmin = [BitConverter]::ToInt32($vc, $b + 40)
            $lmax = [BitConverter]::ToInt32($vc, $b + 44)
            $us   = [BitConverter]::ToUInt16($vc, $b + 56)
            $flag = if ($abs -ne 0) { 'ABSOLUTE' } else { 'relative' }
            $line = "  {0,3}  0x{1:x2}   {2,4}  {3,7}..{4,-7}  {5}" -f $rid, $us, $bits, $lmin, $lmax, $flag
            Write-Host $line
        }
        Write-Host ""
        Write-Host "  A mouse axis marked ABSOLUTE is the fault: Windows"
        Write-Host "  subtracts logical min from every value, which adds a"
        Write-Host "  fixed push of that size to every report."
    } else {
        $line = "  HidP_GetValueCaps returned 0x{0:X8}" -f $vs
        Write-Host $line
    }
    exit 0
}

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
