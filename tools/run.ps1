# Runs a portable test bed unattended: start it, let it work, exit cleanly, dump the log.
#
#   pwsh -File tools/run.ps1 -TestBed ..\fb2k-test -Play ..\sample.eac3
#   pwsh -File tools/run.ps1 -TestBed D:\fb2k -Platform x64 -Play D:\x.eac3 -WaitSeconds 25
#
# The test bed is an existing portable foobar2000 installation with the component
# deployed into it (tools/deploy.ps1).  Two things this script exists to get right:
#
#   * foobar2000 is closed with /exit, never force-killed.  A force-killed instance
#     leaves <profile>\running behind, and every later start then reports "another
#     foobar2000 instance appears to be running with the same configuration data"
#     and idles without loading any user component.
#   * the component's log is written next to the DLL, so it is read from wherever
#     the component was deployed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TestBed,
    [ValidateSet('Win32', 'x64')][string]$Platform = 'Win32',
    [string[]]$Play,
    [int]$WaitSeconds = 15,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath (Join-Path $TestBed 'foobar2000.exe'))) {
    throw "no foobar2000.exe in $TestBed"
}
$exe = Join-Path $TestBed 'foobar2000.exe'

# A leftover "running" marker makes the next start refuse to load components.
$running = Join-Path $TestBed 'profile\running'
if (Test-Path -LiteralPath $running) {
    if (Get-Process foobar2000 -ErrorAction SilentlyContinue) {
        throw 'a foobar2000 process is running; close it (or /exit) before starting a run'
    }
    Remove-Item -LiteralPath $running -Force
    Write-Host 'cleared a stale profile\running marker'
}

$arguments = @()
if ($Play) { $arguments += $Play }
$process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru
Write-Host "started pid=$($process.Id) $($Play -join ', ')"
Start-Sleep -Seconds $WaitSeconds

if (-not $KeepRunning) {
    & $exe /exit | Out-Null
    Start-Sleep -Seconds 6
    $left = Get-Process foobar2000 -ErrorAction SilentlyContinue
    if ($left) {
        # A component that still holds a decode thread can keep /exit from
        # finishing.  Kill it, and clear the marker that the kill leaves behind,
        # or the next start will refuse to load user components.
        Write-Host 'exit did not finish; terminating and clearing the profile marker' -ForegroundColor Yellow
        $left | Stop-Process -Force
        Start-Sleep -Seconds 2
        if (Test-Path -LiteralPath $running) { Remove-Item -LiteralPath $running -Force }
    }
}

Write-Host '--- component log ---' -ForegroundColor Cyan
$logs = Get-ChildItem $TestBed -Recurse -Filter 'joc_decoder.log' -ErrorAction SilentlyContinue
if (-not $logs) {
    Write-Host 'no joc_decoder.log found: the component never ran' -ForegroundColor Yellow
}
foreach ($log in $logs) {
    Write-Host "# $($log.FullName)"
    Get-Content -LiteralPath $log.FullName
}
