# Repro test: create the same PPF twice and compare hashes
param(
    [string]$exe = '',
    [string]$out1 = 'logs\det1.ppf',
    [string]$out2 = 'logs\det2.ppf'
)
# Ensure logs directory exists and move default outputs into it
$LogDir = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $LogDir)) { New-Item -Path $LogDir -ItemType Directory -Force | Out-Null }
if ($out1 -like 'tests\*') { $out1 = Join-Path $LogDir (Split-Path $out1 -Leaf) }
if ($out2 -like 'tests\*') { $out2 = Join-Path $LogDir (Split-Path $out2 -Leaf) }
# If someone passed 'logs\...' or 'logs' relative path, resolve it under tests\logs so it's valid
if ($out1 -like 'logs\*' -or ($out1 -notmatch '[:\\]')) { $out1 = Join-Path $LogDir (Split-Path $out1 -Leaf) }
if ($out2 -like 'logs\*' -or ($out2 -notmatch '[:\\]')) { $out2 = Join-Path $LogDir (Split-Path $out2 -Leaf) }
if (-not $exe) {
    if (Test-Path '.\MakePPF.exe') { $exe = Join-Path $PWD 'MakePPF.exe' }
    elseif (Test-Path '.\PPFManager.exe') { $exe = Join-Path $PWD 'PPFManager.exe' }
    else { Write-Error "No MakePPF/PPFManager executable found in repo root"; exit 2 }
}
# Remove previous outputs
Remove-Item -ErrorAction SilentlyContinue $out1, $out2, tests\logs\deterministic_* 
# Use fixed args (pass as array to avoid quoting/argument-splitting issues)
$args1 = @('c','-u','-i','0','-d','deterministic test','tests\original.bin','tests\modified.bin',$out1)
Write-Host "Running: $exe $($args1 -join ' ')"
& $exe @args1 | Out-File -FilePath tests\logs\deterministic_run1.txt -Encoding UTF8
Start-Sleep -Milliseconds 200
$args2 = @('c','-u','-i','0','-d','deterministic test','tests\original.bin','tests\modified.bin',$out2)
Write-Host "Running: $exe $($args2 -join ' ')"
& $exe @args2 | Out-File -FilePath tests\logs\deterministic_run2.txt -Encoding UTF8
# Compute hashes: wait briefly for files to appear (to avoid races with the writer)
$maxWaitMs = 3000
$waited = 0
while ($waited -lt $maxWaitMs -and (-not (Test-Path $out1 -PathType Leaf -ErrorAction SilentlyContinue))) { Start-Sleep -Milliseconds 100; $waited += 100 }
$waited = 0
while ($waited -lt $maxWaitMs -and (-not (Test-Path $out2 -PathType Leaf -ErrorAction SilentlyContinue))) { Start-Sleep -Milliseconds 100; $waited += 100 }

$h1 = ''
$h2 = ''
if (Test-Path $out1) { $h1 = (Get-FileHash -Path $out1 -Algorithm SHA256).Hash }
if (Test-Path $out2) { $h2 = (Get-FileHash -Path $out2 -Algorithm SHA256).Hash }
Write-Host "Hash1: $h1"
Write-Host "Hash2: $h2"

if (($h1 -ne '') -and ($h2 -ne '') -and ($h1 -eq $h2)) { Write-Host "OK: hashes identical"; exit 0 }

# If different, produce diagnostics
Write-Host "DIFFER: producing diagnostics..."
$hdr1 = Join-Path 'tests\logs' 'deterministic_hdr1.txt'
$hdr2 = Join-Path 'tests\logs' 'deterministic_hdr2.txt'
if (Test-Path $out1) { & (Join-Path $PSScriptRoot 'parse_ppf_header.ps1') -path $out1 | Set-Content -Path $hdr1 -Encoding UTF8 } else { Set-Content -Path $hdr1 -Value "File not found: $out1" -Encoding UTF8 }
if (Test-Path $out2) { & (Join-Path $PSScriptRoot 'parse_ppf_header.ps1') -path $out2 | Set-Content -Path $hdr2 -Encoding UTF8 } else { Set-Content -Path $hdr2 -Value "File not found: $out2" -Encoding UTF8 }

# Hex dumps using Format-Hex limited to first 1024 bytes and last 1024 bytes
$outHex1 = Join-Path 'tests\logs' 'deterministic_hex1.txt'
$outHex2 = Join-Path 'tests\logs' 'deterministic_hex2.txt'
if (Test-Path $out1) { Format-Hex -Path $out1 -Count 256 | Out-File -FilePath $outHex1 -Encoding UTF8 } else { Set-Content -Path $outHex1 -Value "File not found: $out1" -Encoding UTF8 }
if (Test-Path $out2) { Format-Hex -Path $out2 -Count 256 | Out-File -FilePath $outHex2 -Encoding UTF8 } else { Set-Content -Path $outHex2 -Value "File not found: $out2" -Encoding UTF8 }

# Produce byte-diff via fc /b (use quoted paths)
$diffBin = Join-Path 'tests\logs' 'deterministic_bin_diff.txt'
if ((Test-Path $out1) -and (Test-Path $out2)) { cmd /c "fc /b `"$out1`" `"$out2`" > `"$diffBin`"" } else { Set-Content -Path $diffBin -Value "fc not run: missing files" -Encoding UTF8 }

Write-Host "Diagnostics saved to tests\logs\deterministic_*.txt"
exit 1