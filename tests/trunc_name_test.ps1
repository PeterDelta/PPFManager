# Test creation with long ppf filename to ensure snprintf truncation paths are safe
param(
    [string]$exe = ''
)
if (-not $exe) {
    if (Test-Path '.\MakePPF.exe') { $exe = Join-Path $PWD 'MakePPF.exe' }
    elseif (Test-Path '.\PPFManager.exe') { $exe = Join-Path $PWD 'PPFManager.exe' }
    else { Write-Error "No MakePPF/PPFManager executable found in repo root"; exit 2 }
}
$LogDir = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $LogDir)) { New-Item -Path $LogDir -ItemType Directory -Force | Out-Null }
# cleanup
Remove-Item -Path (Join-Path $LogDir 'longname_*.ppf') -ErrorAction SilentlyContinue
# Create long ppf filename within reasonable limits (e.g., 200 chars)
$base = 'a' * 180
$ppfname = Join-Path $LogDir ("longname_$base.ppf")
$args = @('c','-u','-i','0','-d','trunc-name-test','tests\original.bin','tests\modified.bin',$ppfname)
Write-Host "Running: $exe $($args -join ' ')"
& $exe @args | Out-File -FilePath (Join-Path $LogDir 'trunc_name_run.txt') -Encoding UTF8
if (Test-Path $ppfname) { Write-Host "OK: long ppf file created: $ppfname"; exit 0 } else { Write-Host "FAIL: long ppf file not created"; exit 1 }