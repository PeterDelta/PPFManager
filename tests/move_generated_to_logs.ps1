# Move generated test artifacts into tests\logs
Set-Location -LiteralPath $PSScriptRoot
$LogDir = Join-Path $PSScriptRoot 'logs'
if (-not (Test-Path $LogDir)) { New-Item -Path $LogDir -ItemType Directory -Force | Out-Null }
$patterns = @('det1.ppf','det2.ppf','longname_*.ppf','out_gui_applied_*.bin','out_gui_apply_*.txt','out_ref_applied_*.bin','out_ref_apply_*.txt','trunc.ppf','tmp_gui_apply.bin','rt_apply_target.bin.ppf*','rt_*.ppf_tmp*','interrupt_original.bin.ppf*','out_gui_applied_long-desc.bin')
foreach ($p in $patterns) {
    Get-ChildItem -Path $PSScriptRoot -Filter $p -File -ErrorAction SilentlyContinue | ForEach-Object {
        $dest = Join-Path $LogDir ($_.Name)
        Try { Move-Item -Force $_.FullName $dest -ErrorAction Stop; Write-Host "Moved: $($_.Name) -> $dest" } Catch { Write-Host "Error moving $($_.Name): $_" }
    }
}
Write-Host 'Move completed.'
