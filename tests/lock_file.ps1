param(
    [string]$file,
    [int]$seconds = 10
)
$full = Join-Path $PSScriptRoot $file
if (-not (Test-Path $full)) { Write-Error "File missing: $full"; exit 2 }
$fs = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
Write-Host "Locked $file for $seconds seconds (PID=$PID)"
Start-Sleep -Seconds $seconds
$fs.Close()
Write-Host "Released lock on $file"