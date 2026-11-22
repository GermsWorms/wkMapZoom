# Copies wkMapZoom.dll (and wkMapZoom.ini, if there is none yet) into the Worms Armageddon folder, or removes the module
# with -Remove. WA loads it at its next start when Options > Advanced > "Load WormKit modules" is on.
param([switch]$Remove, [string]$Game = 'C:\Program Files (x86)\Steam\steamapps\common\Worms Armageddon')
$ErrorActionPreference = 'Stop'
$target = Join-Path $Game 'wkMapZoom.dll'
if ($Remove) {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target; "removed $target" } else { 'not installed' }
    return
}
if (-not (Test-Path -LiteralPath (Join-Path $Game 'WA.exe'))) { throw "WA.exe not found in $Game (use -Game <your WA folder>)" }
$hash = (Get-FileHash -LiteralPath (Join-Path $Game 'WA.exe')).Hash
if ($hash -ne 'D6C449C75916A0941D1163AFC2A91F1E72A31600609BB9B766F2302CA80164EB') { throw 'This module is for WA 3.8.1 only' }
$dll = Join-Path $PSScriptRoot 'wkMapZoom.dll'
if (-not (Test-Path -LiteralPath $dll)) { $dll = Join-Path $PSScriptRoot 'release\wkMapZoom.dll' }
Copy-Item -LiteralPath $dll -Destination $target -Force
"installed $target (WA loads it at its next start)"
$ini = Join-Path $Game 'wkMapZoom.ini'
if (-not (Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'wkMapZoom.ini') -Destination $ini }
$opt = Get-ItemProperty -Path 'HKCU:\Software\Team17SoftwareLTD\WormsArmageddon\Options' -ErrorAction SilentlyContinue
if (-not $opt -or $opt.LoadWormKitModules -ne 1) { 'Turn on Options > Advanced > "Load WormKit modules" in WA (if it is not on yet)' }
