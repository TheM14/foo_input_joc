# Fetches the official foobar2000 SDK into <project>\SDK and pins the target API level.
#
#   pwsh -File tools/setup_sdk.ps1                 # latest SDK, target 80 (foobar2000 1.5/1.6)
#   pwsh -File tools/setup_sdk.ps1 -TargetVersion 81
#
# The SDK is a .7z from the official site (https://www.foobar2000.org/SDK).  Its
# foobar2000-versions.h carries a commented-out FOOBAR2000_TARGET_VERSION 80 line:
# switching to it is the SDK's documented way of declaring a 1.6-compatible
# component, so this script performs exactly that edit and verifies it.
#
# The archive itself is only a download cache and is kept outside the project (in
# the system temporary directory): the build needs the extracted SDK, never the
# archive.  Delete the cache at any time to force a fresh download.
[CmdletBinding()]
param(
    [string]$Version = '2026-09-17',
    [int]$TargetVersion = 80,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkDir = Join-Path $projectRoot 'SDK'
$dlDir = Join-Path ([System.IO.Path]::GetTempPath()) 'foo_input_joc-sdk'
$archive = Join-Path $dlDir "SDK-$Version.7z"
$url = "https://www.foobar2000.org/downloads/SDK-$Version.7z"

function Find-SevenZip {
    $candidates = @(
        (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
        (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $command = Get-Command 7z -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'no 7-Zip found; install it or unpack the SDK manually into <project>\SDK'
}

New-Item -ItemType Directory -Force -Path $dlDir | Out-Null

if ((Test-Path -LiteralPath (Join-Path $sdkDir 'foobar2000\SDK\foobar2000-versions.h')) -and -not $Force) {
    Write-Host "SDK already present in $sdkDir (pass -Force to re-fetch)"
} else {
    if (-not (Test-Path -LiteralPath $archive)) {
        Write-Host "downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing -TimeoutSec 300
    }
    Write-Host ("archive {0} ({1} bytes)" -f $archive, (Get-Item -LiteralPath $archive).Length)
    if (Test-Path -LiteralPath $sdkDir) { Remove-Item -LiteralPath $sdkDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $sdkDir | Out-Null
    & (Find-SevenZip) x $archive "-o$sdkDir" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE" }
}

$versionsHeader = Join-Path $sdkDir 'foobar2000\SDK\foobar2000-versions.h'
if (-not (Test-Path -LiteralPath $versionsHeader)) { throw "missing $versionsHeader" }

# Only the Windows section is touched: it is the one carrying the "// 2.0" marker.
$text = Get-Content -LiteralPath $versionsHeader -Raw
$patched = $text
if ($TargetVersion -eq 80) {
    $patched = $patched -replace '(?m)^//\s*#define FOOBAR2000_TARGET_VERSION 80 // 1\.5, 1\.6', '#define FOOBAR2000_TARGET_VERSION 80 // 1.5, 1.6'
    $patched = $patched -replace '(?m)^#define FOOBAR2000_TARGET_VERSION 81 // 2\.0', '// #define FOOBAR2000_TARGET_VERSION 81 // 2.0'
} else {
    $patched = $patched -replace '(?m)^#define FOOBAR2000_TARGET_VERSION 80 // 1\.5, 1\.6', '// #define FOOBAR2000_TARGET_VERSION 80 // 1.5, 1.6'
    $patched = $patched -replace '(?m)^//\s*#define FOOBAR2000_TARGET_VERSION 81 // 2\.0', '#define FOOBAR2000_TARGET_VERSION 81 // 2.0'
}
if ($patched -ne $text) {
    Set-Content -LiteralPath $versionsHeader -Value $patched -NoNewline
    Write-Host "patched $versionsHeader for target $TargetVersion"
}

# The active Windows-level define must be the one we asked for.
$win = [regex]::Match((Get-Content -LiteralPath $versionsHeader -Raw),
                      '(?s)#ifdef _WIN32(.*?)#else // _WIN32').Groups[1].Value
if ($win -notmatch "(?m)^#define FOOBAR2000_TARGET_VERSION $TargetVersion") {
    throw "could not pin FOOBAR2000_TARGET_VERSION to $TargetVersion; check `$versionsHeader"
}
Write-Host "OK: SDK $Version in $sdkDir, FOOBAR2000_TARGET_VERSION=$TargetVersion"
