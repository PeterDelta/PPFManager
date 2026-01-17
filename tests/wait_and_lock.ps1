param(
    [string]$target,
    [string]$waitFile,
    [int]$lockSeconds = 10
)
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$targetPath = Join-Path $PSScriptRoot $target
$waitPath = Join-Path $PSScriptRoot $waitFile
Write-Host "Watcher: waiting for $waitFile to appear..."
$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path $waitPath) -and $sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path $waitPath)) { Write-Host "Watcher: timeout waiting for $waitFile"; exit 2 }
Write-Host "Watcher: $waitFile detected; acquiring exclusive lock on $target for $lockSeconds seconds"
$fs = [IO.File]::Open($targetPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
Start-Sleep -Seconds $lockSeconds
$fs.Close()
Write-Host "Watcher: released lock"