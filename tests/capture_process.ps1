$ErrorActionPreference = 'Stop'
$logDir = $env:PPFMANAGER_TEST_LOGDIR
if (-not $logDir -or -not (Test-Path $logDir)) { $logDir = Join-Path $PSScriptRoot 'logs' }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$PSScriptRoot\..\PPFManager.exe"
$psi.Arguments = 's "' + (Join-Path $logDir 'out_gui.ppf') + '"'
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.UseShellExecute = $false
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$p.Start() | Out-Null
$std = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
$p.WaitForExit()
$outPath = Join-Path $logDir 'out_gui_show_processcapture.txt'
Set-Content -Path $outPath -Value $std -Encoding UTF8
Write-Host "Captured len: $($std.Length)"
