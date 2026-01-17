param(
    [string]$path = "out_gui.ppf",
    [int]$max = 50
)
if ([System.IO.Path]::IsPathRooted($path)) { $full = $path } else { $full = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) $path }
if (-not (Test-Path $full)) { Write-Error "File not found: $full"; exit 2 }
$b = [IO.File]::ReadAllBytes($full)
if ($b.Length -lt 60) { Write-Error "File too small"; exit 2 }
$magic = -join ($b[0..4] | ForEach-Object { [char]$_ })
$method = $b[5]
$desc = -join ($b[6..55] | ForEach-Object { if ($_ -eq 0x20) { ' ' } else { [char]$_ } })
$imagetype = $b[56]
$blockcheck = $b[57]
$undo = $b[58]
Write-Host "File: $path  size=$($b.Length) magic=$magic method=$method imagetype=$imagetype blockcheck=$blockcheck undo=$undo"
$pos = 60
if ($blockcheck -ne 0) { $pos += 1024; Write-Host "Skipped binblock, new pos=$pos" }
$entries = @()
$i = 0
while ($pos -lt $b.Length - 9 - 1 - 0) {
    if ($pos + 8 -gt $b.Length - 1) { break }
    $offset = [BitConverter]::ToInt64($b,$pos)
    $pos += 8
    if ($pos -gt $b.Length - 1) { break }
    $k = $b[$pos]; $pos++
    if ($pos + $k - 1 -gt $b.Length - 1) { break }
    $data = $b[$pos..($pos + $k - 1)]; $pos += $k
    $undoData = $null
    if ($undo -ne 0) {
        if ($pos + $k - 1 -gt $b.Length - 1) { break }
        $undoData = $b[$pos..($pos + $k - 1)]; $pos += $k
    }
    $entries += [pscustomobject]@{Offset=$offset;K=$k;DataHash=([System.BitConverter]::ToString([System.Security.Cryptography.SHA1]::Create().ComputeHash($data))).Replace('-','') }
    $i++
    if ($i -ge $max) { break }
}
Write-Host "Parsed $($entries.Count) entries (showing up to $max):"
$entries | Format-Table -AutoSize
exit 0