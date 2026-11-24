# Builds wkMapZoom.dll (a 32-bit WormKit module for Worms Armageddon 3.8.1) with MinGW (32-bit gcc), after running the
# arithmetic, readout and WA.exe check tests. Usage:  .\build.ps1 [-Gcc C:\MinGW\bin\gcc.exe] [-Shaders]
# (-Shaders: src\shaders.h made again from tools\mkshaders.c; needs Windows' d3dcompiler_47.dll)
param([string]$Gcc = 'C:\MinGW\bin\gcc.exe', [switch]$Shaders)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$mh = Join-Path $here 'third_party\minhook'
$opt = @('-O2', '-Wall', '-Wextra', '-std=gnu99', '-msse2', '-mfpmath=sse', '-fno-math-errno', '-fno-trapping-math')   # (floor / ceil
                                                                                                                         #  inline; same values)
if ($Shaders) {
    $ErrorActionPreference = 'Continue'
    & $Gcc -O2 -Wall (Join-Path $here 'tools\mkshaders.c') -o (Join-Path $env:TEMP 'wkmapzoom_mkshaders.exe')
    if ($LASTEXITCODE -ne 0) { throw 'mkshaders build failed' }
    & (Join-Path $env:TEMP 'wkmapzoom_mkshaders.exe') (Join-Path $here 'src\shaders.h')
    if ($LASTEXITCODE -ne 0) { throw 'shaders failed' }
    $ErrorActionPreference = 'Stop'
}
$tests = @('zoom_math_test', 'zoom_badge_test', 'guard_test')
foreach ($t in $tests) {
    $exe = Join-Path $env:TEMP "wkmapzoom_$t.exe"
    $ErrorActionPreference = 'Continue'
    & $Gcc @opt (Join-Path $here "tests\$t.c") -lgdi32 -o $exe
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) { throw "$t build failed" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$t failed" }
}
$out = Join-Path $here 'wkMapZoom.dll'
$sources = @((Join-Path $here 'src\wkMapZoom.c'), (Join-Path $here 'src\gpu.c'), (Join-Path $mh 'src\buffer.c'), (Join-Path $mh 'src\hook.c'),
             (Join-Path $mh 'src\trampoline.c'), (Join-Path $mh 'src\hde\hde32.c'))
$buildArgs = @('-shared') + $opt + @('-mstackrealign', '-static-libgcc', '-s', "-I$mh\include", "-I$here\src") + $sources +
             @('-lgdi32', '-o', $out)
$ErrorActionPreference = 'Continue'   # (compiler warnings on stderr are not build failures)
& $Gcc @buildArgs
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0) { throw 'wkMapZoom build failed' }
Get-FileHash -LiteralPath $out -Algorithm SHA256
