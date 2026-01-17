# Repro script: compare PPF description bytes when creating patches via ppfmanager.exe vs MakePPF.exe
$work = Join-Path $PSScriptRoot "work_cmp"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null
$orig = Join-Path $work "orig.bin"
$mod = Join-Path $work "mod.bin"
$ppf1 = Join-Path $work "out_ppfmanager.ppf"
$ppf2 = Join-Path $work "out_makeppf.ppf"
# Create small test files
$bytes = New-Object byte[] 1024
[IO.File]::WriteAllBytes($orig, $bytes)
$bytes[0] = 1
[IO.File]::WriteAllBytes($mod, $bytes)
# Description with accented chars and non-ascii
# Build description explicitly from Unicode codepoints to avoid console encoding confusion
$desc = "Prueba " + [char]0x00F1 + [char]0x00C1 + [char]0x00E9 + " " + [char]0x20AC
# Resolve executable paths
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$ppfmanagerExe = Join-Path $root "PPFManager.exe"
$makeppfExe = Join-Path $root "MakePPF.exe"

# Run ppfmanager.exe (console)
Write-Host "Running: $ppfmanagerExe c -d '$desc' $orig $mod $ppf1"
$ppfm_out = & $ppfmanagerExe c -d $desc $orig $mod $ppf1 2>&1
Write-Host "PPFManager output:\n$ppfm_out"
# Run MakePPF.exe
Write-Host "Running: $makeppfExe c -d '$desc' $orig $mod $ppf2"
$make_out = & $makeppfExe c -d $desc $orig $mod $ppf2 2>&1
Write-Host "MakePPF output:\n$make_out"

# Helper: dump nearby bytes where description appears
function Dump-DescBytes($filePath) {
    Write-Host "\nInspecting: $filePath"
    $b = [IO.File]::ReadAllBytes($filePath)
    # Search for UTF8 occurrence
    $utf8 = [System.Text.Encoding]::UTF8.GetBytes($desc)
    $acp = [System.Text.Encoding]::GetEncoding([System.Console]::OutputEncoding.CodePage).GetBytes($desc)
    $iUtf = [Array]::IndexOf($b, $utf8[0])
    # Find match of whole sequence
    function FindSeq($a,$seq){ for ($i=0;$i -le $a.Length-$seq.Length;$i++){ $ok=$true; for ($j=0;$j -lt $seq.Length;$j++){ if ($a[$i+$j] -ne $seq[$j]) { $ok=$false; break } } if ($ok) { return $i } } return -1 }
    $posUtf = FindSeq $b $utf8
    $posAcp = FindSeq $b $acp
    Write-Host "desc bytes (UTF8):" ( -join ($utf8 | ForEach-Object { '{0:X2}' -f $_ }) )
    Write-Host "desc bytes (ACP : codepage $([System.Console]::OutputEncoding.CodePage)):" ( -join ($acp | ForEach-Object { '{0:X2}' -f $_ }) )
    Write-Host "Found UTF8 at: $posUtf ; Found ACP at: $posAcp"
    if ($posUtf -ge 0) { $start=[math]::Max(0,$posUtf-10); $len=[math]::Min(80,$b.Length-$start); Write-Host "Context around UTF8 match:"; $b[$start..($start+$len-1)] | ForEach-Object -Begin { $i=$start } -Process { Write-Host ("{0,8:X6}: {1:X2}" -f $i,$_); $i++ } }
    if ($posAcp -ge 0) { $start=[math]::Max(0,$posAcp-10); $len=[math]::Min(80,$b.Length-$start); Write-Host "Context around ACP match:"; $b[$start..($start+$len-1)] | ForEach-Object -Begin { $i=$start } -Process { Write-Host ("{0,8:X6}: {1:X2}" -f $i,$_); $i++ } }
}

Dump-DescBytes $ppf1
Dump-DescBytes $ppf2

# Compare files
$sha1_1 = Get-FileHash -Algorithm SHA1 $ppf1
$sha1_2 = Get-FileHash -Algorithm SHA1 $ppf2
Write-Host "\nSHA1 ppfmanager: $($sha1_1.Hash)\nSHA1 MakePPF   : $($sha1_2.Hash)"
if ($sha1_1.Hash -eq $sha1_2.Hash) {
    Write-Host "Files identical."; exit 0
} else {
    Write-Host "Files differ. Doing detailed binary diff..."
    $b1 = [IO.File]::ReadAllBytes($ppf1)
    $b2 = [IO.File]::ReadAllBytes($ppf2)
    $min = [Math]::Min($b1.Length,$b2.Length)
    $first = -1
    for ($i=0;$i -lt $min;$i++) { if ($b1[$i] -ne $b2[$i]) { $first = $i; break } }
    if ($first -eq -1) {
        Write-Host "Files are identical up to the length of the shorter file. Lengths: $($b1.Length) vs $($b2.Length)"
        exit 2
    }
    Write-Host "First difference at offset: $first (0x{0:X} - decimal)" -f $first
    $start=[Math]::Max(0,$first-32)
    $end=[Math]::Min($first+32,$min-1)
    Write-Host "Context around difference (hex):"
    for ($j=$start;$j -le $end;$j++) { if ($j -eq $first) { $marker = '>>' } else { $marker = '  ' } ; Write-Host ("{0} {1:X8}: {2:X2} | {3:X2}" -f $marker, $j, $b1[$j], $b2[$j]) }
    # Also dump the header start for inspection (first 256 bytes)
    Write-Host "\nHeader (first 256 bytes) for file1 (ppfmanager):"
    for ($j=0;$j -lt [Math]::Min(256,$b1.Length); $j+=16) { $row = $b1[$j..[Math]::Min($j+15,$b1.Length-1)] -join ' '; Write-Host ("{0:X4}: {1}" -f $j,$row) }
    Write-Host "\nHeader (first 256 bytes) for file2 (MakePPF):"
    for ($j=0;$j -lt [Math]::Min(256,$b2.Length); $j+=16) { $row = $b2[$j..[Math]::Min($j+15,$b2.Length-1)] -join ' '; Write-Host ("{0:X4}: {1}" -f $j,$row) }
    # Dump description bytes area (6..55) explicitly
    $desc1 = $b1[6..55]
    $desc2 = $b2[6..55]
    Write-Host "\nDescription area file1 (6..55):" ( -join ($desc1 | ForEach-Object { '{0:X2}' -f $_ }))
    Write-Host "Description area file2 (6..55):" ( -join ($desc2 | ForEach-Object { '{0:X2}' -f $_ }))
    exit 2
}
