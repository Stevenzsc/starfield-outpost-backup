# 编译 papyrus\*.psc 到游戏的 Data\Scripts。用法：
#   powershell -ExecutionPolicy Bypass -File tools\compile.ps1 [-Game "C:\...\Starfield"]
param([string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Starfield")
$ErrorActionPreference = "Stop"
$compiler = Get-ChildItem $Game -Recurse -Filter PapyrusCompiler.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $compiler) { throw "找不到 PapyrusCompiler.exe，请确认已安装 Creation Kit" }
$flags = Get-ChildItem $Game -Recurse -Filter "*Papyrus_Flags.flg" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $flags) { throw "找不到 Starfield_Papyrus_Flags.flg" }
$src = Join-Path $PSScriptRoot "..\papyrus" | Resolve-Path
$imports = @("$src", "$Game\Data\Scripts\Source", "$Game\Scripts\Source", "$Game\Data\Source") | Where-Object { Test-Path $_ }
$out = "$Game\Data\Scripts"
Write-Host "compiler: $($compiler.FullName)"
Write-Host "flags:    $($flags.FullName)"
Get-ChildItem $src -Filter *.psc | ForEach-Object {
  & $compiler.FullName $_.FullName "-f=$($flags.Name)" "-i=$(($imports + $flags.DirectoryName) -join ';')" "-o=$out"
  if ($LASTEXITCODE -ne 0) { throw "编译失败: $($_.Name)" }
}
Write-Host "完成，输出到 $out"
