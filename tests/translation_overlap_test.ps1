# Test that overlapping replacements are handled correctly and idempotently
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$root = Resolve-Path "$PSScriptRoot\.."
$tests = $PSScriptRoot
$LogDir = Join-Path $tests 'logs'
$tool = Join-Path $tests 'translator_tool.exe'
$src = Join-Path $tests 'translator_tool.c'

# Compile translator tool if missing or outdated
if (-not (Test-Path $tool) -or (Get-Item $src).LastWriteTime -gt (Get-Item $tool).LastWriteTime) {
    Write-Host "Compiling translator tool..."
    gcc -O2 -o $tool $src 2>&1 | Write-Host
}

$input = "Progress: 100.00 % (2 entries found)."
$out = ("$input" | & $tool) -join "`n"
if ($out -notmatch 'entradas\s+encontradas') { Write-Host "FAIL: overlap translation did not produce 'entradas encontradas' (got: $out)"; exit 1 }
# Idempotence: feeding translated output back should not change
$out2 = ("$out" | & $tool) -join "`n"
if ($out2 -ne $out) { Write-Host "FAIL: repeat translation changed output (was: $out, now: $out2)"; exit 1 }
Write-Host "OK: overlap translation passed"
exit 0
