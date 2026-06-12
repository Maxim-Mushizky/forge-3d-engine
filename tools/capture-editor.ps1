param(
    # NOTE: do not name this $Args — PowerShell's -File parser eats elements
    # starting with "--" when the param is called Args.
    [string[]]$AppArgs = @(),
    [int]$WaitSeconds = 8,
    [string]$OutFile = "capture.png"
)
# Launches ForgeEditor, captures its window via PrintWindow (works when obscured), kills it.

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class FECap {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public static IntPtr Find(uint pid, string title) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); if (sb.ToString() == title) { found = h; return false; } } return true; }, IntPtr.Zero);
    return found;
  }
}
'@

$p = if ($AppArgs.Count -gt 0) {
    Start-Process -FilePath ".\build\release\bin\ForgeEditor.exe" -ArgumentList $AppArgs -PassThru
} else {
    Start-Process -FilePath ".\build\release\bin\ForgeEditor.exe" -PassThru
}
Start-Sleep -Seconds $WaitSeconds

try {
    $h = [FECap]::Find([uint32]$p.Id, "Forge Editor")
    if ($h -eq [IntPtr]::Zero) { Write-Output "window not found (alive: $(-not $p.HasExited))"; exit 1 }
    [FECap]::SetForegroundWindow($h) | Out-Null # DWM only composites visible windows; obscured = stale capture
    Start-Sleep -Milliseconds 1200
    $r = New-Object FECap+RECT
    [FECap]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [FECap]::PrintWindow($h, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save((Join-Path $repo $OutFile), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "captured $OutFile (${w}x${hh})"
} finally {
    Stop-Process -Id $p.Id -Force -Confirm:$false -ErrorAction SilentlyContinue
}
