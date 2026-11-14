# Find the old beacon spot in the landing zone you are standing in now (any save), and write
# <Starfield>\OSB_Guide.txt so that, in game, "bat OSB_Guide" walks you there.
#
# Needs: a backup made with make-restore.ps1 while the game was running (OSB_Restore.json),
#        and the game running now, landed on the same planet within ~2 km of the old outpost.
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\make-guide.ps1 [-Meta path\to\backup.json]
#
# Math: local coordinates are meters from the landing-zone center. The old beacon's planet position
# is (old center lat/lon) + (old local offset); its local position in the new landing zone is that
# minus the new center. Uses a local flat-earth approximation, fine within a few km.
# Axis assumption (to be confirmed in game): +X = east, +Y = north.
param(
  [string]$Meta = "",
  [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield",
  [int]$Seconds = 300
)
$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
function Fmt([double]$d) { $d.ToString("0.###", $inv) }

if (-not $Meta) { $Meta = Join-Path $Game "OSB_Restore.json" }
if (-not (Test-Path $Meta)) { throw "No backup location file: $Meta (run make-restore.ps1 while the game is running)" }
$m = Get-Content $Meta -Raw | ConvertFrom-Json

$sfgps = Join-Path $PSScriptRoot "memscan\sfgps.exe"
$gps = & $sfgps
if ($LASTEXITCODE -ne 0) { throw "sfgps failed (is Starfield running and are you on a planet surface?)" }
$g = "$gps".Trim() -split '\s+'
$lat1 = [double]::Parse($g[0], $inv); $lon1 = [double]::Parse($g[1], $inv)
$radiusKm = [double]::Parse($g[2], $inv); $planet = $g[3]

if ($planet -ne $m.planet) { throw "You are on planet $planet, the backup is from planet $($m.planet)." }

$R = $radiusKm * 1000.0
$toRad = [Math]::PI / 180.0
$dLon = $m.originLon - $lon1
if ($dLon -gt 180) { $dLon -= 360 } elseif ($dLon -lt -180) { $dLon += 360 }
$meanLat = ($m.originLat + $lat1) / 2.0 * $toRad
$east  = $dLon * $toRad * $R * [Math]::Cos($meanLat)   # old center relative to new center, meters
$north = ($m.originLat - $lat1) * $toRad * $R

$tx = $m.beaconX + $east
$ty = $m.beaconY + $north
$tz = $m.beaconZ
$dist = [Math]::Sqrt($tx * $tx + $ty * $ty)

$out = Join-Path $Game "OSB_Guide.txt"
[IO.File]::WriteAllLines($out, [string[]]@("cgf `"OSB.Guide`" $(Fmt $tx) $(Fmt $ty) $(Fmt $tz) $Seconds"), [Text.Encoding]::ASCII)

Write-Host ("Planet {0}, radius {1} km" -f $planet, $radiusKm)
Write-Host ("Old landing center {0:F6}, {1:F6}   new {2:F6}, {3:F6}" -f $m.originLat, $m.originLon, $lat1, $lon1)
Write-Host ("Old beacon spot in this landing zone: x={0:F1} y={1:F1} m ({2:F0} m from the landing center)" -f $tx, $ty, $dist)
if ($dist -gt 2000) { Write-Warning "That is more than 2 km from this landing center; it may be outside the playable area. Land closer." }
Write-Host "Wrote $out"
Write-Host 'In game: bat OSB_Guide, follow the messages, place the beacon, then bat OSB_Restore'
