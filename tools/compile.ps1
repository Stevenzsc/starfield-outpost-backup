# Compile papyrus\*.psc into the game Data\Scripts. Usage:
#   powershell -ExecutionPolicy Bypass -File tools\compile.ps1 [-Game "C:\...\Starfield"]
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield")
$ErrorActionPreference = "Stop"
$compiler = Get-ChildItem $Game -Recurse -Filter PapyrusCompiler.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $compiler) { throw "PapyrusCompiler.exe not found - is the Creation Kit installed?" }
$flags = Get-ChildItem $Game -Recurse -Filter "*Papyrus_Flags.flg" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $flags) { throw "Starfield_Papyrus_Flags.flg not found" }
$src = Join-Path $PSScriptRoot "..\papyrus" | Resolve-Path
$imports = @("$src", "$Game\Data\Scripts\Source", "$Game\Scripts\Source", "$Game\Data\Source") | Where-Object { Test-Path $_ }
$out = "$Game\Data\Scripts"
Write-Host "compiler: $($compiler.FullName)"
Write-Host "flags:    $($flags.FullName)"
Get-ChildItem $src -Filter *.psc | ForEach-Object {
  & $compiler.FullName $_.FullName "-f=$($flags.Name)" "-i=$(($imports + $flags.DirectoryName) -join ';')" "-o=$out"
  if ($LASTEXITCODE -ne 0) { throw "Compile failed: $($_.Name)" }
}
Write-Host "Done, output: $out"
