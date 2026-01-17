param(
    [Parameter(Mandatory=$true)][string]$path
)

if (-not (Test-Path $path)) { Write-Error "File not found: $path"; exit 2 }
$bytes = [System.IO.File]::ReadAllBytes($path)
if ($bytes.Length -lt 60) { Write-Error "File too small to be PPF"; exit 2 }
$magic = [System.Text.Encoding]::ASCII.GetString($bytes[0..4])
$method = $bytes[5]
$descBytes = $bytes[6..55]
# trim trailing spaces and nulls
$desc = ([System.Text.Encoding]::UTF8.GetString($descBytes)).TrimEnd([char]0, ' ')
$imagetype = $bytes[56]
$bcheck = $bytes[57]
$udata = $bytes[58]
$dummy = $bytes[59]
$result = @()
$result += "magic:$magic"
$result += "method:$method"
$result += "description:$desc"
$result += "imagetype:$imagetype"
$result += "validation:$bcheck"
$result += "undo:$udata"
$result += "dummy:$dummy"
if ($bcheck -ne 0 -and $bytes.Length -ge 1084) {
    $binblock = $bytes[60..(60+1024-1)]
    $sha = (Get-FileHash -InputStream (New-Object System.IO.MemoryStream(,$binblock)) -Algorithm SHA256).Hash
    $result += "binblock_sha256:$sha"
} else {
    $result += "binblock_sha256:"
}
$result | ForEach-Object { Write-Output $_ }
