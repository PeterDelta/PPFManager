Set-Location -LiteralPath $PSScriptRoot
if (Test-Path 'logs1') {
    Get-ChildItem -Path 'logs1' -File | ForEach-Object {
        Move-Item -Force $_.FullName (Join-Path 'logs' $_.Name)
        Write-Host "Moved logs1\$($_.Name) -> logs\$($_.Name)"
    }
    Remove-Item -Recurse -Force 'logs1'
    Write-Host 'logs1 removed'
} else { Write-Host 'no logs1' }
