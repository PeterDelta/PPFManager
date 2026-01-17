# Lock a target file and run ApplyPPF to verify fallback when MoveFileEx fails
Param(
    [string]$target = 'tmp_lock.bin',
    [int]$lockSeconds = 8,
    [string]$ppf = 'out_gui_new.ppf'
)
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$targetPath = Join-Path $PSScriptRoot $target
$ppfPath = Join-Path $PSScriptRoot $ppf

if (-not (Test-Path $ppfPath)) { Write-Error "PPF missing: $ppfPath"; exit 2 }
Copy-Item -Force (Join-Path $PSScriptRoot 'original.bin') $targetPath

# Start a watcher that will lock the original file when the temporary file appears
$watcherScript = Join-Path $PSScriptRoot 'wait_and_lock.ps1'
$tmpName = $target + '.ppf_tmp'
$watcher = Start-Process -FilePath powershell -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',$watcherScript,$target,$tmpName,$lockSeconds) -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 250
Write-Host "Watcher started (Id=$($watcher.Id)); applying patch..."

# Try to apply patch (pipe 'y' via cmd); capture stdout/stderr via Start-Process redirection
$applyExe = Join-Path (Resolve-Path "$PSScriptRoot\..") 'ApplyPPF.exe'
$outFile = Join-Path $PSScriptRoot 'tmp_lock_apply_stdout.txt'
$errFile = Join-Path $PSScriptRoot 'tmp_lock_apply_stderr.txt'
if (Test-Path $outFile) { Remove-Item $outFile -Force }
if (Test-Path $errFile) { Remove-Item $errFile -Force }
$applyCmd = "echo y | `"$applyExe`" a `"$targetPath`" `"$ppfPath`""
$sp = Start-Process -FilePath cmd -ArgumentList @('/c', $applyCmd) -NoNewWindow -Wait -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru
$applyOutput = ''
if (Test-Path $outFile) { $applyOutput += Get-Content $outFile -Raw }
if (Test-Path $errFile) { $applyOutput += "`nERR:`n" + (Get-Content $errFile -Raw) }
Write-Host "Apply output:`n$applyOutput"

# Wait for watcher to finish
$watcher | Wait-Process -Timeout ($lockSeconds + 10)
if (!$watcher.HasExited) { Write-Warning "Watcher still running; killing"; $watcher.Kill(); }

# Check file hashes and temp file presence
$targetHash = (Get-FileHash -Path $targetPath -Algorithm SHA256).Hash
$modHash = (Get-FileHash -Path (Join-Path $PSScriptRoot 'modified.bin') -Algorithm SHA256).Hash
Write-Host "TargetHash: $targetHash"
Write-Host "ModifiedHash: $modHash"

# Temp name convention used by ApplyPPF: "$target.ppf_tmp" or with numeric suffix
$attempts = @((Join-Path $PSScriptRoot ($target + '.ppf_tmp')))
for ($i = 1; $i -lt 100; $i++) { $attempts += (Join-Path $PSScriptRoot ($target + ".ppf_tmp{0:D3}" -f $i)) }
$existingTmp = $attempts | Where-Object { Test-Path $_ }
if ($existingTmp) { Write-Host "Temporary files found:"; $existingTmp | ForEach-Object { Write-Host " - $_" } } else { Write-Host "No temporary files found" }

if ($targetHash -eq $modHash) { Write-Host "RESULT: target matches modified (apply succeeded or fallback applied)"; exit 0 } else { Write-Host "RESULT: target differs from modified (apply failed to replace)"; exit 1 }