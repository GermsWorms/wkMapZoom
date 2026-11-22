# Builds wkMapZoom.dll (a 32-bit WormKit module for Worms Armageddon 3.8.1) with MinGW (32-bit gcc), after running the
# arithmetic and readout tests. Usage:  .\build.ps1 [-Gcc C:\MinGW\bin\gcc.exe]
param([string]$Gcc = 'C:\MinGW\bin\gcc.exe')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mh = Join-Path $here 'third_party\minhook'
$opt = @('-O2', '-Wall', '-Wextra', '-std=c99', '-msse2', '-mfpmath=sse')
foreach ($t in 'zoom_math_test', 'zoom_badge_test') {
    $exe = Join-Path $env:TEMP "wkmapzoom_$t.exe"
    & $Gcc @opt (Join-Path $here "tests\$t.c") -lgdi32 -o $exe
    if ($LASTEXITCODE -ne 0) { throw "$t build failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$t failed" }
}
$out = Join-Path $here 'wkMapZoom.dll'
$sources = @((Join-Path $here 'src\wkMapZoom.c'), (Join-Path $mh 'src\buffer.c'), (Join-Path $mh 'src\hook.c'),
             (Join-Path $mh 'src\trampoline.c'), (Join-Path $mh 'src\hde\hde32.c'))
$buildArgs = @('-shared') + $opt + @('-mstackrealign', '-static-libgcc', '-s', "-I$mh\include", "-I$here\src") + $sources +
             @('-lgdi32', '-o', $out)
$ErrorActionPreference = 'Continue'   # (compiler warnings on stderr are not build failures)
& $Gcc @buildArgs
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw 'wkMapZoom build failed' }
Get-FileHash -LiteralPath $out -Algorithm SHA256
