# Builds the headless test tools.
#
#   pwsh -File tools/build_tests.ps1                 # x64 (matches the core's own build)
#   pwsh -File tools/build_tests.ps1 -Platform Win32 # x86 (the architecture foobar2000 1.6 uses)
#
# They link the kernel archive that the component itself links
# (kernel\joc_kernel.vcxproj -> build\kernel-<Platform>\joc_kernel.lib), so what
# they exercise is the same code the component plays with.
[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64')][string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community'

$vcvars = if ($Platform -eq 'x64') {
    Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
} else {
    Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars32.bat'
}
$coreLib = Join-Path $projectRoot "build\kernel-$Platform\joc_kernel.lib"
foreach ($tool in @($vcvars, $coreLib)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "missing: $tool  (build kernel\joc_kernel.vcxproj for $Platform first)"
    }
}

$outDir = Join-Path $projectRoot "build\tests-$Platform"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$bat = Join-Path $outDir 'build.bat'
# JOC_STATIC / EJOC_STATIC: the kernel headers must not decorate their entry
# points as imported, because the tools link the archive directly.
$common = "/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DJOC_STATIC /DEJOC_STATIC /I src /I kernel\include /I kernel\src"
$lines = @(
    '@echo off',
    "call `"$vcvars`" >nul",
    "cd /d `"$projectRoot`"",
    # log.cpp comes along because the engine writes its diagnostics through it.
    "cl $common /Fe:`"$outDir\container_scan_test.exe`" /Fo:`"$outDir\\`" tests\container_scan_test.cpp src\container_scan.cpp src\log.cpp",
    "cl $common /Fe:`"$outDir\scan_selftest.exe`" /Fo:`"$outDir\\`" tests\scan_selftest.cpp src\eac3_scan.cpp",
    "cl $common /Fe:`"$outDir\prefs_layout_check.exe`" /Fo:`"$outDir\\`" tests\prefs_layout_check.cpp user32.lib gdi32.lib",
    "cl $common /Fe:`"$outDir\scan_crosscheck.exe`" /Fo:`"$outDir\\`" tests\scan_crosscheck.cpp src\eac3_scan.cpp `"$coreLib`" shell32.lib",
    "cl $common /Fe:`"$outDir\render_harness.exe`" /Fo:`"$outDir\\`" tests\render_harness.cpp src\joc_decode.cpp src\eac3_scan.cpp src\container_scan.cpp src\log.cpp `"$coreLib`" shell32.lib",
    'if errorlevel 1 exit /b 1',
    'exit /b 0'
)
Set-Content -LiteralPath $bat -Value $lines -Encoding ASCII
& cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "test build failed with exit code $LASTEXITCODE" }

Get-ChildItem $outDir -Filter '*.exe' | ForEach-Object {
    Write-Host ("built {0} ({1} bytes)" -f $_.Name, $_.Length) -ForegroundColor Green
}
