# Log watcher: monitor ppfmanager_ui_debug.log for browse failures and collect evidence
param(
    [string]$LogPath = (Join-Path (Split-Path -Path $PSScriptRoot -Parent) 'ppfmanager_ui_debug.log'),
    [int]$TailLines = 400,
    [switch]$AutoLaunch,
    [int]$StartupTimeoutSec = 30
)

function Save-Evidence($reason, $line) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $outdir = Join-Path (Split-Path -Path $LogPath -Parent) "evidence_$stamp"
    New-Item -Path $outdir -ItemType Directory -Force | Out-Null
    "Reason: $reason" | Out-File (Join-Path $outdir "trigger.txt") -Encoding utf8
    "Match line: $line" | Out-File (Join-Path $outdir "match.txt") -Encoding utf8

    # tail of debug log
    Get-Content -Path $LogPath -Tail $TailLines | Out-File (Join-Path $outdir "ppfmanager_ui_debug_tail.log") -Encoding utf8

    # copy appended persistent log if exists
    $appended = Join-Path (Split-Path -Path $LogPath -Parent) 'ppfmanager_gui_appended.log'
    if (Test-Path $appended) { Copy-Item $appended -Destination (Join-Path $outdir 'ppfmanager_gui_appended.log') -Force }

    # copy any ppfmanager_aplicar_output_dump from temp
    $tempDump = Join-Path $env:TEMP 'ppfmanager_aplicar_output_dump.log'
    if (Test-Path $tempDump) { Copy-Item $tempDump -Destination (Join-Path $outdir 'ppfmanager_aplicar_output_dump.log') -Force }

    # copy any existing copied apply outputs
    Get-ChildItem -Path (Split-Path -Path $LogPath -Parent) -Filter 'ppfmanager_aplicar_output_copy_*.log' -ErrorAction SilentlyContinue | ForEach-Object { Copy-Item $_.FullName -Destination (Join-Path $outdir $_.Name) -Force }

    # process list & window titles
    Get-Process | Where-Object { $_.MainWindowTitle -ne '' } | Select-Object Id, ProcessName, @{Name='Title';Expression={$_.MainWindowTitle}} | Out-File (Join-Path $outdir 'process_windows.txt') -Encoding utf8
    Get-Process | Select-Object Id, ProcessName, CPU, WS | Out-File (Join-Path $outdir 'process_list.txt') -Encoding utf8

    # event log snippet (last 30 entries)
    try { Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=(Get-Date).AddMinutes(-30)} -MaxEvents 200 | Out-File (Join-Path $outdir 'application_events.txt') -Encoding utf8 } catch { "Could not read event log: $_" | Out-File (Join-Path $outdir 'application_events.txt') -Encoding utf8 }

    # mark done
    "Evidence saved to $outdir" | Out-File (Join-Path $outdir 'done.txt') -Encoding utf8
}

Write-Host "Watching $LogPath for failure patterns... (CTRL+C to stop)"
# If the log file isn't present yet, optionally auto-launch PPFManager and repro script
$startedApp = $false
$startTime = Get-Date
$lastMsg = Get-Date '1/1/2000'
$AutoLaunchEnabled = $AutoLaunch.IsPresent
while (-not (Test-Path $LogPath)) {
    $now = Get-Date
    if (($now - $lastMsg).TotalSeconds -ge 3) { Write-Host "Log not found: $LogPath -- waiting..."; $lastMsg = $now }

    if ($AutoLaunchEnabled -and -not $startedApp) {
        # Start PPFManager if not running
        $proc = Get-Process -Name PPFManager -ErrorAction SilentlyContinue
        if (-not $proc) {
            $exe = Join-Path (Split-Path -Path $PSScriptRoot -Parent) 'PPFManager.exe'
            if (Test-Path $exe) {
                Write-Host "Auto-launching $exe"
                Start-Process -FilePath $exe | Out-Null
                $startedApp = $true
                $appStartTime = Get-Date
            } else {
                Write-Host "Auto-launch requested but $exe not found"
                $AutoLaunchEnabled = $false
            }
        } else {
            Write-Host "PPFManager already running (pid=$($proc.Id))"
            $startedApp = $true
        }
    }

    if (($now - $startTime).TotalSeconds -ge $StartupTimeoutSec -and -not (Test-Path $LogPath)) {
        Write-Host "Startup timeout reached ($StartupTimeoutSec s) waiting for $LogPath"; break
    }
    Start-Sleep -Milliseconds 500
}

# If we started the app and there's a repro script, start it to provoke the failure
if ($startedApp) {
    $repro = Join-Path (Split-Path -Path $PSScriptRoot -Parent) 'tests\repro_browse.ps1'
    if (Test-Path $repro) {
        Write-Host "Starting repro script $repro to provoke failure"
        Start-Process -FilePath 'powershell' -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File', $repro | Out-Null
    }
}

$patterns = @(
    'BM_CLICK SendMessageTimeout failed',
    'BM_CLICK SendMessageTimeout failed/timeout',
    'WM_CHECK_BROWSE_CLICK: .*not',
    'ShowOpenFileDialog_COM failed',
    'ShowOpenFileDialog_COM_WithTimeout: timeout',
    'BrowseBtn WM_LBUTTONDOWN',
    'BrowseBtn WM_LBUTTONUP',
    'Ping TIMEOUT',
    'Browse handle changed',
    'BrowseBtn clicked but control not visible/enabled'
) | ForEach-Object { [regex]::new($_) }

# Tail the log
Get-Content -Path $LogPath -Wait -Tail 0 | ForEach-Object {
    $line = $_
    foreach ($r in $patterns) {
        if ($r.IsMatch($line)) {
            Write-Host "Pattern matched: $($r) -> $line"
            Save-Evidence -reason $r.ToString() -line $line
            # optionally continue monitoring; allow repeated captures but throttle
            Start-Sleep -Seconds 1
            break
        }
    }
}
