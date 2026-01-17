<#
Large-file roundtrip test (external only, does not modify product code).
Creates a large original file (sparse via SetLength), alters one byte, runs MakePPF and ApplyPPF, and compares hashes.
Usage:
  powershell -ExecutionPolicy Bypass -File .\large_file_roundtrip.ps1 [-SizeGB 5] [-Keep]

Default SizeGB = 5 (adjust to smaller value if disk space is limited).
Note: This test can take time and disk space. It is *not* run by default in run_regression.ps1.
#>
param(
    [int]$SizeGB = 5,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$tests = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path "$tests\..").Path
$make = Join-Path $repoRoot 'MakePPF.exe'
$apply = Join-Path $repoRoot 'ApplyPPF.exe'

$orig = Join-Path $tests 'original_big.bin'
$mod = Join-Path $tests 'modified_big.bin'
$outppf = Join-Path $tests 'out_big.ppf'
$applyTarget = Join-Path $tests 'apply_target.bin'

Write-Host "Large-file roundtrip test: Size = ${SizeGB}GB"
$size = [int64]$SizeGB * 1GB

# Create original large file (sparse via SetLength)
Write-Host "Creating original file: $orig (size $size bytes)"
$fs = [System.IO.File]::Open($orig, [System.IO.FileMode]::Create)
try { $fs.SetLength($size) } finally { $fs.Close() }

# Create modified file and change first byte
Write-Host "Creating modified file: $mod (copy and modify)"
Copy-Item -Force $orig $mod
$fs = [System.IO.File]::Open($mod, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try { $fs.Seek(0, 'Begin') | Out-Null; $b = $fs.ReadByte(); $fs.Seek(0,'Begin') | Out-Null; $fs.WriteByte(([byte](([int]$b + 1) -band 0xFF))) } finally { $fs.Close() }

# Run MakePPF
Write-Host "Running MakePPF to create: $outppf"
$makeOut = & $make c -u -i 0 -d "large test" $orig $mod $outppf 2>&1 | Tee-Object -Variable makeOut
if ($LASTEXITCODE -ne 0) { Write-Host "MakePPF failed (exit $LASTEXITCODE)"; Write-Host $makeOut; exit 1 }
if (-not (Test-Path $outppf)) { Write-Host "MakePPF did not produce patch file"; exit 2 }

# Prepare target and apply (work on a copy of original)
Copy-Item -Force $orig $applyTarget
Write-Host "Applying patch to $applyTarget"
$applyOut = & $apply a $outppf $applyTarget 2>&1 | Tee-Object -Variable applyOut
if ($LASTEXITCODE -ne 0) { Write-Host "ApplyPPF failed (exit $LASTEXITCODE)"; Write-Host $applyOut; exit 3 }

# Compare hashes
Write-Host "Computing hashes (may take time)"
$h1 = Get-FileHash -Path $applyTarget -Algorithm SHA256
$h2 = Get-FileHash -Path $mod -Algorithm SHA256
if ($h1.Hash -eq $h2.Hash) {
    Write-Host "OK: roundtrip matches (SHA256)"
    if (-not $Keep) { Remove-Item -Force $orig, $mod, $outppf, $applyTarget }
    exit 0
} else {
    Write-Host "FAIL: hashes differ"
    Write-Host "Apply target: $($h1.Hash)"
    Write-Host "Expected   : $($h2.Hash)"
    Write-Host "Leaving artifacts for inspection (use -Keep to preserve)"
    exit 4
}