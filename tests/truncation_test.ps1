# Test truncation handling: pass a very long description to MakePPF and ensure no unterminated strings or crashes
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
Remove-Item -Path (Join-Path $LogDir 'trunc.*') -ErrorAction SilentlyContinue
# Build a long description
$long = 'X' * 1024
$ppfPath = Join-Path $LogDir 'trunc.ppf'
$args = @('c','-u','-i','0','-d',$long,'tests\original.bin','tests\modified.bin',$ppfPath)
Write-Host "Running: $exe $($args -join ' ')"
& $exe @args | Out-File -FilePath (Join-Path $LogDir 'trunc_run.txt') -Encoding UTF8
# Check that PPF was created
$exists = Test-Path $ppfPath
if (-not $exists) { Write-Host "FAIL: ppf not created"; exit 1 }
# Parse header and validate description is at most 50 chars (header field spec)
$hdrOut = & (Join-Path $PSScriptRoot 'parse_ppf_header.ps1') -path $ppfPath | Out-String
$hdrOut | Out-File -FilePath (Join-Path $LogDir 'trunc_hdr.txt') -Encoding UTF8
$descLine = ($hdrOut -split "\r?\n" | Where-Object { $_ -like 'description:*' }) -replace '^description:',''
$descLine = $descLine.Trim()
if ($descLine.Length -gt 50) { Write-Host "FAIL: description too long ($($descLine.Length))"; exit 1 }
if ($descLine -match "\0") { Write-Host "FAIL: description contains NUL bytes"; exit 1 }
Write-Host "OK: truncation test passed (description length=$($descLine.Length))"; exit 0