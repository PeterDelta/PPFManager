# Strip MakePPF / Icarus header lines from specific test text outputs
# Recognize additional lines that appear in MakePPF/PPFManager outputs
$patterns = '^(MakePPF|=Icarus/Paradox=|PPF Manager|Writing header|Finding differences|Progress:)'
$globs = @('out_ref_*.txt','out_gui_*.txt','process_out_*.txt')
$files = @()
foreach ($g in $globs) {
    $files += Get-ChildItem -Path $PSScriptRoot -Include $g -Recurse -File -ErrorAction SilentlyContinue
}
$files = $files | Sort-Object -Unique
if ($files.Count -eq 0) { Write-Host "No matching files found."; exit 0 }
foreach ($f in $files) {
    try { $text = Get-Content -Path $f.FullName -Raw -Encoding UTF8 } catch { Write-Host "Cannot read: $($f.FullName)"; continue }
    $lines = $text -split "`r?`n"
    $filtered = $lines | Where-Object { -not ($_ -match $patterns) }
    if ($filtered.Length -ne $lines.Length) {
        $filtered -join "`r`n" | Set-Content -Path $f.FullName -Encoding UTF8
        Write-Host "Sanitized header in: $($f.FullName)"
    } else {
        # No legacy header found — quiet by default, use -Verbose to inspect
        Write-Verbose "No legacy header found in: $($f.FullName)"
    }
}
Write-Host "Done."