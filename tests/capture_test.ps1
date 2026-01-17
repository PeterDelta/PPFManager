$p = New-Object System.Diagnostics.ProcessStartInfo
$logDir = $env:PPFMANAGER_TEST_LOGDIR
if (-not $logDir -or -not (Test-Path $logDir)) { $logDir = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'logs' }
$p.FileName = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\PPFManager.exe'
$p.Arguments = 's ' + (Join-Path $logDir 'out_gui.ppf')
$p.RedirectStandardOutput = $true
$p.RedirectStandardError = $true
$p.UseShellExecute = $false
$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $p
$proc.Start() | Out-Null
$std = $proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd()
$proc.WaitForExit()
Set-Content -Path (Join-Path $logDir 'out_manual_gui_show_capture_proc.txt') -Value $std -Encoding UTF8
Write-Host "Captured len: $($std.Length) lines: $($std -split '\r?\n' | Measure-Object).Count"