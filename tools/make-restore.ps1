# Turn the latest OSB backup in the Papyrus user log into a console batch file.
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\make-restore.ps1 [-Game "C:\...\Starfield"] [-Name "my-outpost"]
# Then in game, standing inside the target outpost:  bat OSB_Restore
param(
  [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield",
  [string]$Name = ""
)
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
# Item lines look like:  [time] OSB-ITEM [Form < (0006AC92)>] x y z ax ay az scale
# The script cannot call GetFormID() safely in game, so the hex ID is parsed here.
$cmds = New-Object System.Collections.Generic.List[string]
$skipped = 0
foreach ($l in $lines[($begin + 1)..($end - 1)]) {
  if ($l -notmatch 'OSB-ITEM .*?\(([0-9A-Fa-f]{8})\)>\]\s+(.+)$') { continue }
  $hex = [Convert]::ToUInt32($Matches[1], 16)
  if ($hex -lt 0x800) { $skipped++; continue }                       # engine-reserved forms
  $id = [BitConverter]::ToInt32([BitConverter]::GetBytes($hex), 0)  # Papyrus int is signed 32-bit
  $cmds.Add("cgf `"OSB.R`" $id $($Matches[2].Trim())")
}
if ($cmds.Count -eq 0) { throw "Backup block has no objects." }
$cmds.Add("cgf `"OSB.Done`" $($cmds.Count)")

$out = Join-Path $Game "OSB_Restore.txt"
[IO.File]::WriteAllLines($out, [string[]]$cmds, [Text.Encoding]::ASCII)

# keep a dated copy next to the repo so the backup survives other saves / reinstalls
$backups = Join-Path $PSScriptRoot "..\backups"
New-Item -ItemType Directory -Force $backups | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$tag = if ($Name) { "$Name-$stamp" } else { $stamp }
$copy = Join-Path $backups "OSB_Restore-$tag.txt"
[IO.File]::WriteAllLines($copy, [string[]]$cmds, [Text.Encoding]::ASCII)

Write-Host "Source: $($log.FullName)"
Write-Host "Backup: $header"
Write-Host "Objects: $($cmds.Count - 1)  (engine forms skipped: $skipped)"
Write-Host "Wrote:  $out"
Write-Host "Copy:   $copy"
Write-Host 'In game, stand inside the target outpost and run:  bat OSB_Restore'
