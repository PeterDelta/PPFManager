# Compare two binary files and report differing ranges
param(
    [string]$ref = "out_ref.ppf",
    [string]$gui = "out_gui.ppf",
    [int]$maxRanges = 50
)
$testsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$refPath = Join-Path $testsDir $ref
# Prefer GUI outputs in tests/logs if present
if ([System.IO.Path]::IsPathRooted($gui)) { $guiPath = $gui } elseif (Test-Path (Join-Path $testsDir "logs\$gui")) { $guiPath = Join-Path $testsDir "logs\$gui" } else { $guiPath = Join-Path $testsDir $gui }
if (-not (Test-Path $refPath)) { Write-Error "Reference file not found: $refPath"; exit 2 }
if (-not (Test-Path $guiPath)) { Write-Error "GUI file not found: $guiPath"; exit 2 }
$a = [System.IO.File]::ReadAllBytes($refPath)
$b = [System.IO.File]::ReadAllBytes($guiPath)
$len = [math]::Min($a.Length,$b.Length)
$ranges = @()
$i = 0
while ($i -lt $len) {
    if ($a[$i] -ne $b[$i]) {
        $start = $i
        while ($i -lt $len -and $a[$i] -ne $b[$i]) { $i++ }
        $end = $i - 1
        $ranges += [pscustomobject]@{ Start = $start; End = $end; Len = $end - $start + 1 }
    } else { $i++ }
}
if ($a.Length -ne $b.Length) {
    $ranges += [pscustomobject]@{ Start = $len; End = [math]::Max($a.Length,$b.Length)-1; Len = ([math]::Max($a.Length,$b.Length) - $len) }
}
$total = ($ranges | Measure-Object Len -Sum).Sum
Write-Host "Ref size: $($a.Length) bytes; Gui size: $($b.Length) bytes"
Write-Host "Total differing bytes (sum of ranges): $total"
if ($ranges.Count -eq 0) { Write-Host "No differences in overlapping region"; exit 0 }
Write-Host "First $maxRanges differing ranges (Start, End, Len):"
$ranges | Select-Object -First $maxRanges | Format-Table -AutoSize
# Show hex dump around first few ranges for context
function HeXDUMP([byte[]]$arr, [long]$start, [int]$context) {
    $s = [math]::Max(0, $start - $context)
    $e = [math]::Min($arr.Length-1, $start + $context)
    for ($i = $s; $i -le $e; $i += 16) {
        $line = ($arr[$i..[math]::Min($e, $i+15)] | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Host ("{0:X8}: {1}" -f $i, $line)
    }
}
Write-Host "\nHex around first differences (context 64 bytes):"
$idx = 0
foreach ($r in $ranges | Select-Object -First $maxRanges) {
    Write-Host "Range #$($idx): $($r.Start)-$($r.End) (len=$($r.Len))"
    Write-Host "--- ref ---"
    HeXDUMP $a $r.Start 64
    Write-Host "--- gui ---"
    HeXDUMP $b $r.Start 64
    $idx++
}
exit 0