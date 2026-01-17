# Apply small-file test: create PPF from small original->modified and ensure apply actually changes the target
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$root = Resolve-Path "$PSScriptRoot\.."
$ppfman = Join-Path $root 'PPFManager.exe'
if (-not (Test-Path $ppfman)) { $ppfman = Join-Path $PSScriptRoot 'PPFManager.exe' }

$orig = Join-Path $PSScriptRoot 'original.bin'
$mod  = Join-Path $PSScriptRoot 'modified.bin'
$ppf  = Join-Path $PSScriptRoot 'logs\apply_small_test.ppf'
$target = Join-Path $PSScriptRoot 'logs\apply_small_target.bin'

# Ensure clean target
Copy-Item -Force $orig $target

# Create the patch
$env:PPFMANAGER_AUTO_YES = '1'
$env:PPFMANAGER_TEST = '1'  # enable extra debug output for troubleshooting
& $ppfman c -u -i 0 -d "apply-small test" $orig $mod $ppf | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: MakePPF failed"; exit 1 }

# Apply patch to the target copy
& $ppfman a $target $ppf
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: ApplyPPF reported failure"; exit 1 }

# Verify target matches modified
$targetHash = (Get-FileHash -Path $target -Algorithm SHA256).Hash
$modHash = (Get-FileHash -Path $mod -Algorithm SHA256).Hash
if ($targetHash -eq $modHash) { Write-Output "OK: apply succeeded"; exit 0 } else { Write-Output "FAIL: apply did not modify target"; exit 1 }