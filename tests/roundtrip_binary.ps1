# Binary round-trip test: create pattern file (0..255) and ensure apply restores exact bytes
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
$root = Resolve-Path "$PSScriptRoot\.."
$make = Join-Path $root 'MakePPF.exe'
if (-not (Test-Path $make)) { $make = Join-Path $PSScriptRoot 'MakePPF.exe' }
$apply = Join-Path $root 'ApplyPPF.exe'
if (-not (Test-Path $apply)) { $apply = Join-Path $PSScriptRoot 'ApplyPPF.exe' }

$orig = Join-Path $PSScriptRoot 'rt_original.bin'
$mod = Join-Path $PSScriptRoot 'rt_modified.bin'
$ppf = Join-Path $PSScriptRoot 'rt_test.ppf'
$target = Join-Path $PSScriptRoot 'rt_apply_target.bin'

# Create pattern file (~2MB)
$repeat = 8192 # 256 * 8192 = 2,097,152 bytes
$buf = [byte[]]@(0..255)
$stream = [System.IO.File]::OpenWrite($orig)
try { for ($i=0; $i -lt $repeat; $i++) { $stream.Write($buf, 0, $buf.Length) } } finally { $stream.Close() }

# Create modified file by flipping bytes at some offsets
Copy-Item -Force $orig $mod
$fs = [System.IO.File]::Open($mod, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite)
try {
    for ($offset = 1024; $offset -lt $fs.Length; $offset += 50000) {
        $fs.Seek($offset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $b = $fs.ReadByte()
        if ($b -ge 0) { $fs.Seek(-1, [System.IO.SeekOrigin]::Current) | Out-Null; $fs.WriteByte((255 - $b)) }
    }
} finally { $fs.Close() }

# Create patch
& $make c -u -i 0 -d "roundtrip test" $orig $mod $ppf | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: MakePPF failed"; exit 1 }

# Apply patch to a fresh copy of original
Copy-Item -Force $orig $target
& $apply a $target $ppf | Out-Null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: ApplyPPF failed"; exit 1 }

$targetHash = (Get-FileHash -Path $target -Algorithm SHA256).Hash
$modHash = (Get-FileHash -Path $mod -Algorithm SHA256).Hash

if ($targetHash -eq $modHash) { Write-Output "OK: round-trip binary integrity verified"; exit 0 } else { Write-Output "FAIL: round-trip mismatch"; exit 1 }