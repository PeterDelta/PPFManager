# Sanitize test logs and reference text files by removing obsolete header lines
$patterns = @('^MakePPF', '^=Icarus/Paradox=', '^PPF Manager')
$files = Get-ChildItem -Path $PSScriptRoot -Include *.txt -Recurse -File | Where-Object { $_.FullName -match "\\b(?:logs|tests)\\b" -or $_.DirectoryName -match "\\btests\\b" }
foreach ($f in $files) {
    try {
        $orig = Get-Content -Path $f.FullName -Raw -Encoding UTF8
    } catch {
        Write-Host "Skipping unreadable: $($f.FullName)"; continue
    }
    $lines = $orig -split "`r?`n"
    $filtered = $lines | Where-Object { $line = $_; -not ($patterns | ForEach-Object { $line -match $_ } ) }
    if ($filtered.Length -ne $lines.Length) {
        # Write only if changed, preserve UTF8
        $filtered -join "`r`n" | Set-Content -Path $f.FullName -Encoding UTF8
        Write-Host "Sanitized header lines in: $($f.FullName)"
    }
}
Write-Host "Sanitization complete."