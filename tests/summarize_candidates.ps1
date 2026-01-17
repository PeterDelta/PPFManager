$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $scriptDir 'logs'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$in = Join-Path $outDir 'translation_candidates.txt'
$out = Join-Path $outDir 'translation_candidates_summary.md'
$lines = Get-Content $in -ErrorAction Stop
# Extract literal (after ': ')
$map = @{}
foreach ($l in $lines) {
    $parts = $l -split ': ',2
    if ($parts.Length -lt 2) { continue }
    $lit = $parts[1].Trim()
    if (-not $map.ContainsKey($lit)) { $map[$lit] = @() }
    $map[$lit] += $parts[0]
}
# Sort by count desc
$sorted = $map.GetEnumerator() | Sort-Object @{Expression = { $_.Value.Count }; Descending = $true }
$sb = New-Object System.Text.StringBuilder
$sb.AppendLine('# Translation Candidates Summary') | Out-Null
$sb.AppendLine('Generated: ' + (Get-Date).ToString('u')) | Out-Null
$sb.AppendLine('') | Out-Null
$sb.AppendLine('Top candidates (literal, count, sample file):') | Out-Null
$top=0
foreach ($e in $sorted) {
    $top++
    $count = $e.Value.Count
    $sample = $e.Value[0]
    $lit = $e.Key -replace '\\n','\\n'
    $sb.AppendLine("- " + $lit + " - " + $count + " occurrences (example: " + $sample + ")") | Out-Null
    if ($top -ge 80) { break }
}
$sb.AppendLine('') | Out-Null
$sb.AppendLine('Full list grouped by literal (literal: count)') | Out-Null
foreach ($e in $sorted) {
    $lit = $e.Key -replace '\\n','\\n'
    $sb.AppendLine('') | Out-Null
    $count2 = $e.Value.Count
    $sb.AppendLine('## ' + $lit + ' - ' + $count2) | Out-Null
    $samples = $e.Value | Select-Object -First 8
    foreach ($s in $samples) { $sb.AppendLine('- ' + $s) | Out-Null }
}
Set-Content -Path $out -Value $sb.ToString() -Encoding UTF8
Write-Host "Wrote summary to $out"