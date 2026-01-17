param(
    [string]$File = 'out_gui.ppf'
)
$exe = Join-Path $PSScriptRoot "..\PPFManager.exe"
if (-not (Test-Path $exe)) { Write-Host "Executable not found: $exe"; exit 1 }
# Prefer logs directory for default out_gui.ppf
if ($File -eq 'out_gui.ppf') {
    $logDir = $env:PPFMANAGER_TEST_LOGDIR
    if (-not $logDir -or -not (Test-Path $logDir)) { $logDir = Join-Path $PSScriptRoot 'logs' }
    $File = Join-Path $logDir $File
}
$p = Start-Process -FilePath $exe -ArgumentList 's',$File -PassThru -WindowStyle Hidden
Write-Host "Started PID=$($p.Id) for file $File"
for ($i=0; $i -lt 120; $i++) {
    Start-Sleep -Seconds 1
    $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if (-not $proc) { Write-Host "Process exited"; break }
    Write-Host "Time ${i}s: WS=$([math]::Round($proc.WorkingSet64/1MB,2))MB, CPU=$([math]::Round($proc.CPU,2))%"
}
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) {
    Write-Host "Timeout reached; killing process"; Stop-Process -Id $p.Id -Force
}
