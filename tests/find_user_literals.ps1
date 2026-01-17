<#
Scans C source files for obvious user-visible string literals used in printf/fprintf/fputs/wprintf/fwprintf/puts.
Outputs a deduplicated list to translation_candidates.txt for manual review.
#>
$root = Resolve-Path ".." -Relative | Resolve-Path -Relative
$pattern = 'printf\s*\(|fprintf\s*\(|fwprintf\s*\(|wprintf\s*\(|fputs\s*\(|puts\s*\('
$files = Get-ChildItem -Path .. -Recurse -Include *.c,*.h | Where-Object { -not $_.FullName.ToLower().Contains('tests\') }
$candidates = [System.Collections.Generic.HashSet[string]]::new()
foreach ($f in $files) {
    $lines = Get-Content $f.FullName -Raw
    # Find occurrences of the functions and extract the first quoted string following
    [regex]$re = '(?:printf|fprintf|fwprintf|wprintf|fputs|puts)\s*\(\s*@?"((?:[^"]|""|\\")*)"'
    $matches = $re.Matches($lines)
    foreach ($m in $matches) {
        $lit = $m.Groups[1].Value -replace '""','"' -replace '\\"','\"'
        $full = $f.FullName
        $repoRoot = (Resolve-Path '..').Path
        $relative = $full
        if ($full.StartsWith($repoRoot)) { $relative = $full.Substring($repoRoot.Length + 1) }
        $entry = "${relative}: $lit"
        $candidates.Add($entry) | Out-Null
    }
}
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir = Join-Path $scriptDir 'logs'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$outPath = Join-Path $outDir 'translation_candidates.txt'
Set-Content -Path $outPath -Value ($candidates | Sort-Object) -Encoding UTF8
Write-Host "Wrote $($candidates.Count) candidate literals to $outPath"