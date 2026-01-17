# Test that 'Done.' is translated to 'Completado.' and that translation is idempotent
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$tests = $PSScriptRoot
$tool = Join-Path $tests 'translator_tool.exe'
$src = Join-Path $tests 'translator_tool.c'

# Compile translator tool if missing or outdated
if (-not (Test-Path $tool) -or (Get-Item $src).LastWriteTime -gt (Get-Item $tool).LastWriteTime) {
    Write-Host "Compiling translator tool..."
    gcc -O2 -o $tool $src 2>&1 | Write-Host
}

$input = 'Done.'
$out = ("$input" | & $tool) -join "`n"
if ($out -ne 'Completado.') { Write-Host "FAIL: Done. did not translate to Completado. (got: $out)"; exit 1 }
# Idempotence
$out2 = ("$out" | & $tool) -join "`n"
if ($out2 -ne $out) { Write-Host "FAIL: repeat translation changed output (was: $out, now: $out2)"; exit 1 }
Write-Host "OK: done translation idempotent"
exit 0
