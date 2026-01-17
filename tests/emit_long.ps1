# Emit a single very long line (> 4096 chars) and a newline for testing truncation handling
$s = 'A' * 5000
Write-Output $s
