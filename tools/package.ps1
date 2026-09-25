# Packages the component the way foobar2000 expects to install it.
#
#   pwsh -File tools/package.ps1                 # both architectures
#   pwsh -File tools/package.ps1 -Platform x64
#
# A ".fb2k-component" file is a plain ZIP archive holding the component DLL; the
# user drags it onto foobar2000 (or uses Preferences -> Components -> Install) and
# the installer unpacks it into a subfolder of the profile's user-components.  The
# archive must therefore contain the DLL at its root, and the architecture matters:
# foobar2000 1.6 is 32-bit, 2.x ships both, and a wrong-architecture DLL is
# silently ignored -- so x86 and x64 get separate packages.
#
# A readme travels inside the archive so the file explains itself once installed.
[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64', 'both')][string]$Platform = 'both',
    [string]$Configuration = 'Release-Static'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $projectRoot 'dist'

# The version the component reports comes from main.cpp; read it from the source so
# the package name cannot drift from what foobar2000 shows.
$version = '0.0.0'
$main = Get-Content (Join-Path $projectRoot 'src\main.cpp') -Raw
if ($main -match '#define\s+JOC_VERSION\s+"([^"]+)"') {
    $version = $Matches[1]
} elseif ($main -match 'DECLARE_COMPONENT_VERSION\s*\(\s*"[^"]*"\s*,\s*"([^"]+)"') {
    $version = $Matches[1]
}

$targets = if ($Platform -eq 'both') { @('Win32', 'x64') } else { @($Platform) }
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
# Packages of earlier versions would otherwise pile up and be uploaded as well.
Get-ChildItem $distDir -Filter '*.fb2k-component' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notlike "*$version*" } |
    Remove-Item -Force

foreach ($target in $targets) {
    $architecture = if ($target -eq 'Win32') { 'x86' } else { 'x64' }
    Write-Host "building $target ($architecture)..." -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1') -Platform $target -Configuration $Configuration | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed for $target" }

    $dll = Join-Path $projectRoot "build\$target\foo_input_joc.dll"
    if (-not (Test-Path -LiteralPath $dll)) { throw "missing $dll" }

    # Stage exactly what should end up in the user's profile.
    $stage = Join-Path $distDir "stage-$architecture"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'foo_input_joc.dll') -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $stage -Force

    $archive = Join-Path $distDir "foo_input_joc-$version-$architecture.fb2k-component"
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal

    Remove-Item -LiteralPath $stage -Recurse -Force
    Write-Host ("  {0}  ({1:N0} bytes)" -f $archive, (Get-Item -LiteralPath $archive).Length) -ForegroundColor Green
}

Write-Host ''
Get-ChildItem $distDir -Filter '*.fb2k-component' | ForEach-Object {
    Write-Host ("{0}  sha256={1}" -f $_.Name, (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())
}
