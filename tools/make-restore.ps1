# Turn the latest OSB backup in the Papyrus user log into a console batch file.
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\make-restore.ps1 [-Mode Relative|Absolute] [-Name "my-outpost"] [-Game "C:\...\Starfield"]
# Then in game, standing inside the target outpost:  bat OSB_Restore
#
# -Mode Relative (default): the layout is stored relative to the old beacon and rebuilt around the
#   beacon of the outpost you stand in, following its position and heading. Use this when the new
#   beacon is not at exactly the same spot (another save, rebuilt outpost).
# -Mode Absolute: objects go back to their exact world coordinates (same save, same beacon).
# -BeaconYaw: only for old backups whose OSB-BEGIN line has no yaw= field.
param(
  [ValidateSet("Relative", "Absolute")][string]$Mode = "Relative",
  [string]$Name = "",
  [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield",
  [Nullable[double]]$BeaconYaw = $null
)
$inv = [Globalization.CultureInfo]::InvariantCulture
function Num([string]$s) { [double]::Parse($s, $inv) }
function Fmt([double]$d) { $d.ToString("0.######", $inv) }
$ErrorActionPreference = "Stop"
$logDir = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Starfield\Logs\Script\User"
$log = Get-ChildItem $logDir -Filter "OSB_Backup.*.log" -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $log) { throw "No OSB_Backup log in $logDir - run cgf `"OSB.Backup`" <beaconID> in game first." }

$lines = Get-Content $log.FullName
# keep only the last OSB-BEGIN ... OSB-END block
$begin = -1; $end = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -match 'OSB-BEGIN') { $begin = $i; $end = -1 }
  elseif ($lines[$i] -match 'OSB-END' -and $begin -ge 0) { $end = $i }
}
if ($begin -lt 0 -or $end -lt 0) { throw "No complete OSB-BEGIN/OSB-END block in $($log.FullName)" }

$header = $lines[$begin]
if ($Mode -eq "Relative") {
  if ($header -notmatch 'pos=\(([-0-9.eE]+),([-0-9.eE]+),([-0-9.eE]+)\)') { throw "OSB-BEGIN line has no beacon pos=: $header" }
  $bx = Num $Matches[1]; $by = Num $Matches[2]; $bz = Num $Matches[3]
  if ($header -match 'yaw=([-0-9.eE]+)') { $byaw = Num $Matches[1] }
  elseif ($null -ne $BeaconYaw) { $byaw = [double]$BeaconYaw }
  else { throw "This backup has no beacon yaw. Back up again with the current OSB.pex, pass -BeaconYaw <degrees>, or use -Mode Absolute." }
  $rad = $byaw * [Math]::PI / 180.0
  $c = [Math]::Cos($rad); $s = [Math]::Sin($rad)
}

# Item lines look like:  [time] OSB-ITEM [Form < (0006AC92)>] x y z ax ay az scale
# The script cannot call GetFormID() safely in game, so the hex ID is parsed here.
$cmds = New-Object System.Collections.Generic.List[string]
$skipped = 0
foreach ($l in $lines[($begin + 1)..($end - 1)]) {
  if ($l -notmatch 'OSB-ITEM .*?\(([0-9A-Fa-f]{8})\)>\]\s+(.+)$') { continue }
  $hex = [Convert]::ToUInt32($Matches[1], 16)
  if ($hex -lt 0x800) { $skipped++; continue }                       # engine-reserved forms
  $id = [BitConverter]::ToInt32([BitConverter]::GetBytes($hex), 0)  # Papyrus int is signed 32-bit
  $v = $Matches[2].Trim() -split '\s+'
  if ($v.Count -ne 7) { Write-Warning "Skipping malformed line: $l"; continue }
  if ($Mode -eq "Absolute") {
    $cmds.Add("cgf `"OSB.R`" $id $($v -join ' ')")
  } else {
    # world offset -> old beacon's local frame (inverse of OSB.RR: heading is clockwise from +Y)
    $dx = (Num $v[0]) - $bx; $dy = (Num $v[1]) - $by; $dz = (Num $v[2]) - $bz
    $lx = $dx * $c - $dy * $s
    $ly = $dx * $s + $dy * $c
    $relaz = (Num $v[5]) - $byaw
    $cmds.Add("cgf `"OSB.RR`" $id $(Fmt $lx) $(Fmt $ly) $(Fmt $dz) $($v[3]) $($v[4]) $(Fmt $relaz) $($v[6])")
  }
}
if ($cmds.Count -eq 0) { throw "Backup block has no objects." }
$cmds.Add("cgf `"OSB.Done`" $($cmds.Count)")

$out = Join-Path $Game "OSB_Restore.txt"
[IO.File]::WriteAllLines($out, [string[]]$cmds, [Text.Encoding]::ASCII)

# Where on the planet the beacon was: landing-zone center lat/lon (read from game memory by sfgps.exe)
# + beacon position in that landing zone. make-guide.ps1 uses this to find the spot in another save.
# The game must still be running in the same landing zone as when OSB.Backup ran.
$meta = $null
$sfgps = Join-Path $PSScriptRoot "memscan\sfgps.exe"
if ($header -match 'pos=\(([-0-9.eE]+),([-0-9.eE]+),([-0-9.eE]+)\)') {
  $mx = $Matches[1]; $my = $Matches[2]; $mz = $Matches[3]
  $myaw = if ($header -match 'yaw=([-0-9.eE]+)') { $Matches[1] } elseif ($null -ne $BeaconYaw) { "$BeaconYaw" } else { "0" }
  $gps = if (Test-Path $sfgps) { & $sfgps 2>$null } else { $null }
  if ($LASTEXITCODE -eq 0 -and $gps) {
    $g = "$gps".Trim() -split '\s+'
    $meta = [ordered]@{
      planet = $g[3]; radiusKm = Num $g[2]
      originLat = Num $g[0]; originLon = Num $g[1]
      beaconX = Num $mx; beaconY = Num $my; beaconZ = Num $mz; beaconYaw = Num $myaw
      created = (Get-Date -Format "yyyy-MM-dd HH:mm:ss")
    }
  } else {
    Write-Warning "Could not read lat/lon from the game (is Starfield running, on the same landing zone?). Restore works, but make-guide.ps1 will not."
  }
}

# keep a dated copy next to the repo so the backup survives other saves / reinstalls
$backups = Join-Path $PSScriptRoot "..\backups"
New-Item -ItemType Directory -Force $backups | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$tag = if ($Name) { "$Name-$stamp" } else { $stamp }
$copy = Join-Path $backups "OSB_Restore-$tag.txt"
[IO.File]::WriteAllLines($copy, [string[]]$cmds, [Text.Encoding]::ASCII)
if ($meta) {
  $json = $meta | ConvertTo-Json
  [IO.File]::WriteAllText([IO.Path]::ChangeExtension($copy, ".json"), $json, [Text.Encoding]::ASCII)
  [IO.File]::WriteAllText((Join-Path $Game "OSB_Restore.json"), $json, [Text.Encoding]::ASCII)
}

Write-Host "Source: $($log.FullName)"
Write-Host "Backup: $header"
Write-Host "Mode:   $Mode"
Write-Host "Objects: $($cmds.Count - 1)  (engine forms skipped: $skipped)"
Write-Host "Wrote:  $out"
Write-Host "Copy:   $copy"
if ($meta) { Write-Host ("Where:  planet {0}, beacon at lat/lon origin {1:F6}, {2:F6} + ({3:F1}, {4:F1}) m" -f $meta.planet, $meta.originLat, $meta.originLon, $meta.beaconX, $meta.beaconY) }
Write-Host 'In game, stand inside the target outpost and run:  bat OSB_Restore'
