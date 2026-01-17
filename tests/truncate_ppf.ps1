param([string]$file = 'out_ref_new.ppf', [int]$bytes = 30)
$full = Join-Path $PSScriptRoot $file
if (-not (Test-Path $full)) { Write-Error "File missing: $full"; exit 2 }
$fs = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite)
try { $fs.SetLength([Math]::Max(0, $fs.Length - $bytes)) } finally { $fs.Close() }
Write-Host "Truncated $file by $bytes bytes."