# Repro script: start PPFManager, post WM_COMMAND id=113 repeatedly, then tail debug log
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace Win {
    public static class Api {
        [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
        public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);
        [DllImport("user32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    }
}
"@ -Language CSharp

$exe = Join-Path $PSScriptRoot '..\PPFManager.exe'
Write-Host "Starting $exe"
$proc = Start-Process -FilePath $exe -PassThru

# wait for main window
$hwnd = [IntPtr]::Zero
for ($i=0; $i -lt 60; $i++) {
    $hwnd = [Win.Api]::FindWindowW($null, 'PPF Manager')
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 200
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host 'Main window not found'; exit 2 }
Write-Host "Found main window: $hwnd"

$WM_COMMAND = 0x0111
$id = 113
# Aggressive reproduction: rapid repeated clicks and occasional longer pause to provoke race conditions
for ($j=1; $j -le 80; $j++) {
    Write-Host "Posting WM_COMMAND id=$id - attempt $j"
    [Win.Api]::PostMessageW($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    if ($j % 10 -eq 0) { Start-Sleep -Milliseconds 1500 } else { Start-Sleep -Milliseconds 200 }
}
# Also simulate a second burst after short delay
Start-Sleep -Seconds 1
for ($j=1; $j -le 40; $j++) {
    [Win.Api]::PostMessageW($hwnd, $WM_COMMAND, [IntPtr]$id, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}

Write-Host 'Posted clicks; waiting 30s for timeouts/procdump...'
Start-Sleep -Seconds 30

Write-Host 'Searching for .dmp files (repo and exedir)'
Get-ChildItem -Path $PSScriptRoot -Filter *.dmp -Recurse -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "FOUND DMP: $($_.FullName)" }
Get-ChildItem -Path (Split-Path -Path $exe) -Filter *.dmp -Recurse -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "FOUND DMP: $($_.FullName)" }

Write-Host 'Last lines of ppfmanager_ui_debug.log:'
Get-Content -Path (Join-Path (Split-Path -Path $exe) 'ppfmanager_ui_debug.log') -Tail 200

Write-Host 'Script finished'
