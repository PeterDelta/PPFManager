Set-Location -LiteralPath $PSScriptRoot
$files=@('out_ref_create.txt','out_ref_add.txt','out_ref_show.txt','out_ref_iso.txt','out_ref_redirect_named.txt','out_ref_redirect_oom.txt','out_ref_long_output.txt')
$LogDir = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $LogDir)) { New-Item -Path $LogDir -ItemType Directory -Force | Out-Null }
foreach($f in $files) {
    if (Test-Path $f) { Move-Item -Force $f -Destination (Join-Path $LogDir $f); Write-Host "Moved $f -> logs\$f" } else { Write-Host "Not found: $f" }
}
Write-Host 'Done.'
