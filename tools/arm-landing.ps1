# Arm the OSBLanding SFSE plugin: the next star-map landing goes exactly to the backed-up landing center.
# Landing on the same center as the original save makes the old local coordinates valid again, so the
# old beacon spot is exactly where it was (bat OSB_Guide walks you there; Absolute restore also works).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\arm-landing.ps1 [-Meta path\to\backup.json]
# Then in game: open the star map on the same planet, click anywhere, land.
param(
  [string]$Meta = "",
  [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield"
)
$ErrorActionPreference = "Stop"
$inv = [Globalization.CultureInfo]::InvariantCulture
if (-not $Meta) { $Meta = Join-Path $Game "OSB_Restore.json" }
if (-not (Test-Path $Meta)) { throw "No backup location file: $Meta" }
$m = Get-Content $Meta -Raw | ConvertFrom-Json

$target = Join-Path $Game "OSB_LandTarget.txt"
$line = "{0} {1}" -f ([double]$m.originLat).ToString("R", $inv), ([double]$m.originLon).ToString("R", $inv)
[IO.File]::WriteAllText($target, $line + "`r`n", [Text.Encoding]::ASCII)

# The beacon was at (beaconX, beaconY) meters from that center: write the guide too.
$guide = Join-Path $Game "OSB_Guide.txt"
[IO.File]::WriteAllLines($guide, [string[]]@("cgf `"OSB.Guide`" $(([double]$m.beaconX).ToString($inv)) $(([double]$m.beaconY).ToString($inv)) $(([double]$m.beaconZ).ToString($inv)) 300"), [Text.Encoding]::ASCII)

Write-Host "Armed: next landing goes to lat $($m.originLat) lon $($m.originLon) (planet $($m.planet))"
Write-Host "Wrote $target"
Write-Host "Wrote $guide"
Write-Host "In game: star map on planet $($m.planet), click anywhere, land; then bat OSB_Guide, place the beacon, bat OSB_Restore"
