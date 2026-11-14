# Capture latitude/longitude from the Celestial Positioning System (CPS) overlay.
# CPS (Nexus mod 17020) shows "LAT: x | LONG: y" in a desktop window of class CPSStableOverlayClass
# for 5 seconds after you open the Starmap / click a planet. It writes no file, so this script reads
# the window text and appends each new reading to backups\gps-log.txt.
#
# Usage (leave it running while you play, Ctrl+C to stop):
#   powershell -ExecutionPolicy Bypass -File tools\read-gps.ps1 [-Label "my-outpost"]
# Then in game open the Starmap while standing at the outpost (or click the landing target).
param(
  [string]$Label = "",
  [int]$PollMs = 300
)
$ErrorActionPreference = "Stop"

Add-Type -Namespace OSB -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hWnd, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumProc cb, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumChildWindows(System.IntPtr parent, EnumProc cb, System.IntPtr lParam);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hWnd, System.Text.StringBuilder buf, int max);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessage(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.Text.StringBuilder lParam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern System.IntPtr SendMessage(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam);
'@

function Get-ClassName([IntPtr]$h) {
  $sb = New-Object System.Text.StringBuilder 256
  [void][OSB.Win]::GetClassName($h, $sb, 256)
  $sb.ToString()
}
function Get-Text([IntPtr]$h) {
  # WM_GETTEXTLENGTH = 0x0E, WM_GETTEXT = 0x0D (works across processes, unlike GetWindowText for controls)
  $len = [int][OSB.Win]::SendMessage($h, 0x0E, [IntPtr]::Zero, [IntPtr]::Zero)
  if ($len -le 0) { return "" }
  $sb = New-Object System.Text.StringBuilder ($len + 1)
  [void][OSB.Win]::SendMessage($h, 0x0D, [IntPtr]($len + 1), $sb)
  $sb.ToString()
}
function Read-Overlay {
  $texts = New-Object System.Collections.Generic.List[string]
  $cb = [OSB.Win+EnumProc]{
    param($h, $l)
    if ((Get-ClassName $h) -eq "CPSStableOverlayClass") {
      $t = Get-Text $h
      if ($t) { $texts.Add($t) }
      $child = [OSB.Win+EnumProc]{ param($c, $l2) $t2 = Get-Text $c; if ($t2) { $texts.Add($t2) }; $true }
      [void][OSB.Win]::EnumChildWindows($h, $child, [IntPtr]::Zero)
    }
    $true
  }
  [void][OSB.Win]::EnumWindows($cb, [IntPtr]::Zero)
  $texts -join " "
}

$out = Join-Path $PSScriptRoot "..\backups"
New-Item -ItemType Directory -Force $out | Out-Null
$logFile = Join-Path $out "gps-log.txt"
Write-Host "Watching for the CPS overlay (Ctrl+C to stop). Readings go to $logFile"

$last = ""
while ($true) {
  $text = Read-Overlay
  if ($text -match 'LAT:\s*(-?[0-9.]+)[^|]*\|\s*LONG:\s*(-?[0-9.]+)') {
    $reading = "$($Matches[1]) $($Matches[2])"
    if ($reading -ne $last) {
      $last = $reading
      $line = "{0}  lat={1}  lon={2}  {3}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Matches[1], $Matches[2], $Label
      Add-Content -Path $logFile -Value $line -Encoding ASCII
      Write-Host $line
    }
  }
  Start-Sleep -Milliseconds $PollMs
}
