# Installs the built component into a portable foobar2000 test bed.
#
#   pwsh -File tools/deploy.ps1 -TestBed ..\fb2k-test
#   pwsh -File tools/deploy.ps1 -TestBed D:\fb2k -Platform x64
#
# Where the component has to go depends on the foobar2000 layout, and getting it
# wrong is silent: foobar2000 simply never calls LoadLibrary on the file.
#
#   1.6         <app>\profile\user-components\<name>\<name>.dll
#               (portable mode; without it the profile is in %APPDATA%)
#   2.0 and up  <app>\user-components\<name>\<name>.dll
#               even in portable mode, whose profile is <app>\profile
#   older 1.6   <app>\user-components\<name>\<name>.dll
#               repacked installs whose profile is <app>\configuration
#
# The per-component subdirectory is required in every case: a DLL lying directly in
# user-components\ is not scanned.  Installing a .fb2k-component package through
# foobar2000 itself ends up doing the same thing.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TestBed,
    [ValidateSet('Win32', 'x64')][string]$Platform = 'Win32',
    [string]$Target
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot

$dll = Join-Path $projectRoot "build\$Platform\foo_input_joc.dll"
if (-not (Test-Path -LiteralPath $dll)) { throw "build the $Platform DLL first: $dll is missing" }
if (-not (Test-Path -LiteralPath (Join-Path $TestBed 'foobar2000.exe'))) {
    throw "no foobar2000.exe in $TestBed"
}

# The core version decides the layout, and its own profile records it.
$versionFile = Join-Path $TestBed 'profile\version.txt'
$version = if (Test-Path -LiteralPath $versionFile) { (Get-Content -LiteralPath $versionFile -Raw).Trim() } else { '' }

if (-not $Target) {
    $appRelative = Test-Path -LiteralPath (Join-Path $TestBed 'configuration')
    if ($version -match 'v(\d+)\.') { $appRelative = ([int]$Matches[1] -ge 2) }
    $root = if ($appRelative) { $TestBed } else { Join-Path $TestBed 'profile' }
    $Target = Join-Path $root 'user-components'
    Write-Host ("layout: {0} ({1}){2}" -f $(if ($appRelative) { 'app-relative' } else { 'profile-relative' }), $root, $(if ($version) { ", $version" } else { '' }))
}

# Portable profile: config stays inside the test bed instead of %APPDATA%.
New-Item -ItemType File -Force -Path (Join-Path $TestBed 'portable_mode_enabled') | Out-Null

$componentDir = Join-Path $Target 'foo_input_joc'
New-Item -ItemType Directory -Force -Path $componentDir | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $componentDir 'foo_input_joc.dll') -Force

# Drop the log of a previous run so what gets read next is this run only.
$log = Join-Path $componentDir 'joc_decoder.log'
if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }

Write-Host ("deployed {0} -> {1}" -f $dll, (Join-Path $componentDir 'foo_input_joc.dll')) -ForegroundColor Green
Write-Host "log will be written to: $log"
