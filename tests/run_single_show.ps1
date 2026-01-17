$env:PPFMANAGER_TEST='1'
$env:PPFMANAGER_DEBUG='1'
$LogDir = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $LogDir)) { New-Item -Path $LogDir -ItemType Directory -Force | Out-Null }
$env:PPFMANAGER_TEST_CAPTURE_GUI = (Join-Path $LogDir 'out_gui_show_pipe.txt')
$env:PPFMANAGER_TEST_CAPTURE_RAW = (Join-Path $LogDir 'out_gui_raw.bin')
$env:PPFMANAGER_TEST_FILTER_DEBUG = (Join-Path $LogDir 'out_gui_filter_debug.txt')
$env:PPFMANAGER_TEST_DUMP_CONTROL = (Join-Path $LogDir 'out_gui_control_text.txt')
# Additional diagnostics for capture issues
$env:PPFMANAGER_TEST_INJECT_STDOUT_MARKERS='1'
$env:PPFMANAGER_TEST_DUMP_TMP='1'

Remove-Item (Join-Path $LogDir 'out_gui_filter_debug.txt') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $LogDir 'out_gui_show_pipe.txt') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $LogDir 'out_gui_raw.bin') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $LogDir 'out_gui_control_text.txt') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $LogDir 'cli_out_run3.txt') -Force -ErrorAction SilentlyContinue

Start-Process -FilePath .\PPFManager.exe
Start-Sleep -Milliseconds 2000
.\PPFManager.exe s 'Apocalypse (Europe).ppf' 2>&1 | Tee-Object (Join-Path $LogDir 'cli_out_run3.txt')
Start-Sleep -Milliseconds 2000

if (Test-Path (Join-Path $LogDir 'out_gui_control_text.txt')) { Write-Output '--- CONTROL DUMP ---'; Get-Content (Join-Path $LogDir 'out_gui_control_text.txt') | Select-Object -First 400 } else { Write-Output 'No control dump' }
if (Test-Path (Join-Path $LogDir 'out_gui_show_pipe.txt')) { Write-Output '--- GUI CAPTURE FILE ---'; Get-Content (Join-Path $LogDir 'out_gui_show_pipe.txt') | Select-Object -First 400 } else { Write-Output 'No capture file' }
if (Test-Path (Join-Path $LogDir 'out_gui_filter_debug.txt')) { Write-Output '--- FILTER DEBUG ---'; Get-Content (Join-Path $LogDir 'out_gui_filter_debug.txt') | Select-Object -First 400 } else { Write-Output 'No filter debug file' }
if (Test-Path (Join-Path $LogDir 'out_gui_raw.bin')) { Write-Output '--- RAW FILE SIZE ---'; Get-Item (Join-Path $LogDir 'out_gui_raw.bin') | Select-Object Length } else { Write-Output 'No raw file' }
Get-Content (Join-Path $LogDir 'cli_out_run3.txt') | Select-Object -First 200
