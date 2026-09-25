# Builds foo_input_joc.dll with the SDK's own MSBuild projects.
#
#   pwsh -File tools/build.ps1                    # Win32 (foobar2000 1.6 and 2.x 32-bit)
#   pwsh -File tools/build.ps1 -Platform x64      # x64 (foobar2000 2.x 64-bit)
#
# Release-Static is the SDK configuration that uses the static CRT, which is what
# this component requires: no runtime redistributable, and the same CRT model as
# the rendering core it links.
[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64')][string]$Platform = 'Win32',
    [string]$Configuration = 'Release-Static'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$project = Join-Path $projectRoot 'foo_input_joc.vcxproj'

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
        if ($found) { return ($found | Select-Object -First 1) }
    }
    foreach ($edition in @('Community', 'Professional', 'Enterprise', 'BuildTools')) {
        $candidate = Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\$edition\MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'MSBuild.exe not found; install the Visual Studio C++ workload'
}

if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'SDK\foobar2000\SDK\foobar2000-versions.h'))) {
    throw 'SDK missing; run tools/setup_sdk.ps1 first'
}

$msbuild = Find-MSBuild
$logDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir "msbuild.$Platform.log"

# Redirection (not a pipeline) keeps msbuild's own exit code in $LASTEXITCODE.
& $msbuild $project /nologo /m /v:minimal "/p:Configuration=$Configuration" "/p:Platform=$Platform" *> $log
$code = $LASTEXITCODE
Get-Content -LiteralPath $log -Tail 40

if ($code -ne 0) {
    Write-Host "BUILD FAILED: msbuild exit code $code (full log: $log)" -ForegroundColor Red
    exit $code
}

$dll = Join-Path $projectRoot "build\$Platform\foo_input_joc.dll"
if (-not (Test-Path -LiteralPath $dll)) {
    Write-Host "BUILD FAILED: msbuild reported success but $dll is missing" -ForegroundColor Red
    exit 1
}
Write-Host ("BUILD OK: {0} ({1} bytes)" -f $dll, (Get-Item -LiteralPath $dll).Length) -ForegroundColor Green
