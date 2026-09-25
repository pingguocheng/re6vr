param([int]$TargetPid = 0, [int]$WaitSeconds = 25)

# read_dialog.ps1 - launch BH6.exe, wait for its "Fatal error." dialog, and dump
# every child-window text so the actual error message is visible without a
# screenshot.
$sig = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class WinText {
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr hWnd, EnumProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);

    delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    public static List<string> Dump(uint targetPid) {
        var outp = new List<string>();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            var t = new StringBuilder(1024); GetWindowTextW(h, t, 1024);
            var c = new StringBuilder(256); GetClassNameW(h, c, 256);
            outp.Add("WINDOW class=" + c + " visible=" + IsWindowVisible(h) + " text=" + t);
            EnumChildWindows(h, (ch, l2) => {
                var ct = new StringBuilder(2048); GetWindowTextW(ch, ct, 2048);
                var cc = new StringBuilder(256); GetClassNameW(ch, cc, 256);
                if (ct.Length > 0 || cc.Length > 0)
                    outp.Add("   child class=" + cc + " text=" + ct);
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
'@
Add-Type -TypeDefinition $sig -ErrorAction Stop

$game = "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 6"
$proc = if ($TargetPid -gt 0) { Get-Process -Id $TargetPid -ErrorAction SilentlyContinue }
        else { Start-Process -FilePath "$game\BH6.exe" -WorkingDirectory $game -PassThru }

if (-not $proc) { Write-Host "no process"; exit 1 }
Write-Host "watching pid $($proc.Id)"
$deadline = (Get-Date).AddSeconds($WaitSeconds)
$dumped = $false
while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if (-not $p) { Write-Host "process exited"; break }
    if ($p.MainWindowTitle -match 'Fatal|error|Error') {
        Start-Sleep -Milliseconds 400
        Write-Host "=== window text dump ==="
        [WinText]::Dump([uint32]$proc.Id) | ForEach-Object { Write-Host $_ }
        $dumped = $true
        break
    }
    Start-Sleep -Milliseconds 300
}
if (-not $dumped) {
    $p = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if ($p) {
        Write-Host "no fatal dialog seen; title='$($p.MainWindowTitle)'; dumping anyway"
        [WinText]::Dump([uint32]$proc.Id) | ForEach-Object { Write-Host $_ }
    }
}
