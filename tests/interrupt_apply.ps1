# Simulate interruption during ApplyPPF and verify atomic behavior
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$root = Resolve-Path "$PSScriptRoot\.."
$make = Join-Path $root 'MakePPF.exe'
if (-not (Test-Path $make)) { $make = Join-Path $PSScriptRoot 'MakePPF.exe' }
$apply = Join-Path $root 'ApplyPPF.exe'
if (-not (Test-Path $apply)) { $apply = Join-Path $PSScriptRoot 'ApplyPPF.exe' }

$orig = Join-Path $PSScriptRoot 'interrupt_original.bin'
$mod = Join-Path $PSScriptRoot 'interrupt_modified.bin'
$ppf = Join-Path $PSScriptRoot 'interrupt_test.ppf'

# Create a reasonably large file to make apply take some time (32MB)
$sizeMB = 32
$buf = [byte[]]@(0..255)
$stream = [System.IO.File]::OpenWrite($orig)
try {
    for ($i=0; $i -lt ($sizeMB * 1024 * 1024 / $buf.Length); $i++) {
        $stream.Write($buf, 0, $buf.Length)
    }
} finally { $stream.Close() }

# Create modified: copy and change bytes at intervals
Copy-Item -Force $orig $mod
$fs = [System.IO.File]::Open($mod, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    for ($offset = 4096; $offset -lt $fs.Length; $offset += 65536) {
        $fs.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $b = $fs.ReadByte()
        if ($b -ge 0) { $fs.Seek(-1, [System.IO.SeekOrigin]::Current) | Out-Null; $fs.WriteByte((255 - $b)) }
    }
} finally { $fs.Close() }

# Create patch
& $make c -u -i 0 -d "interrupt test" $orig $mod $ppf | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: MakePPF failed"; exit 1 }

$origHash = (Get-FileHash -Path $orig -Algorithm SHA256).Hash
$modHash = (Get-FileHash -Path $mod -Algorithm SHA256).Hash

# Start apply process
$proc = Start-Process -FilePath $apply -ArgumentList ('a', "`"$orig`"", "`"$ppf`"") -PassThru -NoNewWindow
Start-Sleep -Milliseconds 500
if (-not $proc.HasExited) {
    try { $proc.Kill() } catch {}
    $proc.WaitForExit()
}
$exit = $proc.ExitCode
$afterHash = (Get-FileHash -Path $orig -Algorithm SHA256).Hash

# Accept both successful apply or interrupted-but-unchanged as OK
if ($afterHash -eq $modHash) {
    Write-Output "OK: apply completed and file matches modified"; exit 0
} elseif ($afterHash -eq $origHash) {
    Write-Output "OK: apply was interrupted and original file unchanged"; exit 0
} else {
    Write-Output "FAIL: original file inconsistent after interruption"; exit 1
}