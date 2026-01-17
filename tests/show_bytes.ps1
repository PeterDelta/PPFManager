param(
    [string]$file = 'original.bin',
    [int]$offset = 0x9320,
    [int]$count = 128
)
$full = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $file
if (-not (Test-Path $full)) { Write-Error "File missing: $full"; exit 2 }
$b=[IO.File]::ReadAllBytes($full)
$end=[math]::Min($b.Length-1,$offset+$count-1)
for($i=$offset; $i -le $end; $i+=16){ $chunk = $b[$i..[math]::Min($end,$i+15)]; $line = ($chunk | ForEach-Object { $_.ToString('X2') }) -join ' '; Write-Host ("{0:X8}: {1}" -f $i, $line) }