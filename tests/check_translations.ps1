$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$managerPath = Join-Path $root 'PPFManager.c'
$headerPath = Join-Path $root 'ppfmanager.h'
$sourcePaths = @($managerPath, (Join-Path $root 'MakePPF.c'), (Join-Path $root 'ApplyPPF.c'), $headerPath)
$managerText = Get-Content -Raw -Encoding UTF8 $managerPath
$allText = ($sourcePaths | ForEach-Object { Get-Content -Raw -Encoding UTF8 $_ }) -join "`n"

function Get-UniqueMatches([string]$text, [string]$pattern) {
    return @([regex]::Matches($text, $pattern) | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
}

$errors = @()
$warnings = @()

$uiTableMatch = [regex]::Match($managerText, 'static const UI_TEXT_ENTRY UI_TEXTS\[\] = \{(?s)(.*?)\n\};')
if (-not $uiTableMatch.Success) { $errors += 'UI_TEXTS table not found' }
$uiDefinitionMatches = [regex]::Matches($uiTableMatch.Groups[1].Value, '\{L"([^\"]+)"\s*,')
$uiDefinitions = @($uiDefinitionMatches | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$uiDuplicates = @($uiDefinitionMatches | ForEach-Object { $_.Groups[1].Value } | Group-Object | Where-Object Count -gt 1 | Select-Object -ExpandProperty Name)
if ($uiDuplicates.Count) { $errors += "Duplicate UI keys: $($uiDuplicates -join ', ')" }
$uiUses = Get-UniqueMatches $allText 'T\(L"([^\"]+)"\)'
$sharedDefinitions = Get-UniqueMatches (Get-Content -Raw -Encoding UTF8 $headerPath) '\{L"([^\"]+)"\s*,\s*L"[^\"]*"\s*,\s*L"[^\"]*"\}'
$sharedDefinitionMatches = [regex]::Matches((Get-Content -Raw -Encoding UTF8 $headerPath), '\{L"([^\"]+)"\s*,\s*L"[^\"]*"\s*,\s*L"[^\"]*"\}')
$sharedDuplicates = @($sharedDefinitionMatches | ForEach-Object { $_.Groups[1].Value } | Group-Object | Where-Object Count -gt 1 | Select-Object -ExpandProperty Name)
if ($sharedDuplicates.Count) { $errors += "Duplicate shared keys: $($sharedDuplicates -join ', ')" }
$knownUiKeys = @($uiDefinitions + $sharedDefinitions | Sort-Object -Unique)
$missingUi = @($uiUses | Where-Object { $_ -notin $knownUiKeys })
if ($missingUi.Count) { $errors += "UI keys used but undefined: $($missingUi -join ', ')" }

$twDefinitions = @([regex]::Matches($managerText, 'strcmp\(key,\s*"([^"]+)"\)\s*==\s*0') | ForEach-Object { $_.Groups[1].Value })
$twDuplicates = @($twDefinitions | Group-Object | Where-Object Count -gt 1 | Select-Object -ExpandProperty Name)
if ($twDuplicates.Count) { $errors += "Duplicate tw keys: $($twDuplicates -join ', ')" }
$twUses = Get-UniqueMatches $allText '(?:tw|ConsolePrintfKey|ConsolePrintfKeyMB)\("([^"]+)"\)'
$missingTw = @($twUses | Where-Object { $_ -notin $twDefinitions })
if ($missingTw.Count) { $errors += "tw keys used but undefined: $($missingTw -join ', ')" }
$unusedTw = @($twDefinitions | Sort-Object -Unique | Where-Object { $_ -notin $twUses })
if ($unusedTw.Count) { $warnings += "tw keys defined but unused: $($unusedTw -join ', ')" }

$ruleMatches = [regex]::Matches($managerText, '\{L"([^\"]+)"\s*,\s*L"([^\"]*)"\}')
$emptyRules = @($ruleMatches | Where-Object { $_.Groups[2].Value.Length -eq 0 } | ForEach-Object { $_.Groups[1].Value })
if ($emptyRules.Count) { $errors += "Translation rules without output: $($emptyRules -join ', ')" }
$duplicateRules = @($ruleMatches | ForEach-Object { $_.Groups[1].Value } | Group-Object | Where-Object Count -gt 1 | Select-Object -ExpandProperty Name)
if ($duplicateRules.Count) { $errors += "Duplicate translation rules: $($duplicateRules -join ', ')" }

# Detect the most common stale help markers in the active MakePPF help.
$makeText = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'MakePPF.c')
if ($makeText -match 'patchinfomation') { $errors += 'MakePPF help contains obsolete spelling: patchinfomation' }
if ($makeText -match 'c : create PPF3\.0 patch\s+a : add') { $errors += 'MakePPF help advertises obsolete add command: a' }
if ($makeText -match '-i \[0/1\]') { $errors += 'MakePPF help omits ISO image type 2' }

$warnings | ForEach-Object { Write-Warning $_ }
if ($errors.Count) {
    $errors | ForEach-Object { Write-Error $_ }
    exit 1
}
Write-Host 'OK: translation catalog references and rules are consistent'
