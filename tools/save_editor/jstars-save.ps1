# J-Stars Victory VS+ save editor for JStarsReborn (Windows PowerShell 5.1 or later).
#
# Edits the unencrypted save the runtime writes to
#   %LOCALAPPDATA%\JStarsReborn\user\savedata\1\CUSA01593\SYSTEM-DATA\SAVE-DATA.DAT
# and recomputes its checksum. A timestamped backup is written next to the save before every change.
#
# Checksum: S = sum of little-endian u16 words from offset 0x8 to the end of the file.
#           u32 at 0x0 = S >> 8, u32 at 0x4 = S & 0xFF. (Confirmed on saves written by the game.)
#
# Usage:  .\jstars-save.ps1 -Action check      report whether the checksum is valid (default, changes nothing)
#         .\jstars-save.ps1 -Action all        unlock + owned: every character playable, J-Points maxed
#         .\jstars-save.ps1 -Action unlock     unlock flags and J-Points
#         .\jstars-save.ps1 -Action owned      owned-character flags (characters bought in the J-Points shop)
#         .\jstars-save.ps1 -Action gauges     fill the three shop gauges
#         .\jstars-save.ps1 -Action fix        only recompute the checksum
param(
    [ValidateSet('check', 'all', 'unlock', 'owned', 'gauges', 'fix')] [string]$Action = 'check',
    [int]$OwnedBits = 46,   # 46 = every battle character (confirmed in game)
    [int]$GaugeXP = 99999,
    [int]$JPoints = 999999,
    [string]$Save = "$env:LOCALAPPDATA\JStarsReborn\user\savedata\1\CUSA01593\SYSTEM-DATA\SAVE-DATA.DAT"
)
$ErrorActionPreference = 'Stop'

function Get-Sum([byte[]]$b) {
    [uint64]$s = 0
    for ($i = 8; $i -lt $b.Length; $i += 2) { $s += [uint64]$b[$i] + 256 * [uint64]$b[$i + 1] }
    return $s
}
function Set-Bits([byte[]]$b, [int]$offset, [int]$count) {
    # Sets bits 0..count-1 starting at offset (little-endian bit order, as the game stores them).
    for ($n = 0; $n -lt $count; $n++) {
        $i = $offset + [math]::Floor($n / 8)
        $b[$i] = $b[$i] -bor (1 -shl ($n % 8))
    }
}

if (-not (Test-Path -LiteralPath $Save)) { throw "Save not found: $Save. Start the game once so it creates its save, then close it." }
if (Get-Process jstars-runtime -ErrorAction SilentlyContinue) { throw 'Close the game first: it would overwrite the save.' }

$b = [IO.File]::ReadAllBytes($Save)
if ($b.Length -lt 0x269C0) { throw "Unexpected save size ($($b.Length) bytes); refusing to edit." }
$s = Get-Sum $b
$ok = ([BitConverter]::ToUInt32($b, 0) -eq ($s -shr 8)) -and ([BitConverter]::ToUInt32($b, 4) -eq ($s -band 0xFF))
"Checksum before: $(if ($ok) { 'valid' } else { 'INVALID' })"
if ($Action -eq 'check') { return }

$backup = "$Save.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
Copy-Item -LiteralPath $Save -Destination $backup
"Backup: $backup"

if ($Action -in 'unlock', 'all') {
    Set-Bits $b 0x5C40 52      # character flags (all 52)
    Set-Bits $b 0x5C48 13      # support character flags
    Set-Bits $b 0x269B8 13     # second support block
    [BitConverter]::GetBytes([uint32]$JPoints).CopyTo($b, 0x5C5C)   # current J-Points
    if ([BitConverter]::ToUInt32($b, 0x5C58) -lt $JPoints) { [BitConverter]::GetBytes([uint32]$JPoints).CopyTo($b, 0x5C58) } # total earned
}

if ($Action -in 'owned', 'all') {
    # Owned-character flags; buying a character in the shop sets its bit here.
    Set-Bits $b 0x5C6C $OwnedBits
}

if ($Action -eq 'gauges') {
    # The three shop gauges (star / fist / heart), stored twice: 0x5C60 and 0x269A0, three u32 each.
    foreach ($base in 0x5C60, 0x269A0) {
        for ($k = 0; $k -lt 3; $k++) { [BitConverter]::GetBytes([uint32]$GaugeXP).CopyTo($b, $base + 4 * $k) }
    }
}

$s = Get-Sum $b
[BitConverter]::GetBytes([uint32]($s -shr 8)).CopyTo($b, 0)
[BitConverter]::GetBytes([uint32]($s -band 0xFF)).CopyTo($b, 4)
[IO.File]::WriteAllBytes($Save, $b)
"Written. Checksum: S=0x{0:x}" -f $s
