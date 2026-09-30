# Capture the Kagami window with PrintWindow, which draws it without bringing it
# to the front (SetForegroundWindow is refused for a background process, so a
# screen grab after it catches whatever window is on top). Also prints the
# window's rectangle against the work area -- the fullscreen-restore check.
param([string]$Out = 'cap.png')
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
$sig = @'
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
public struct RECT { public int L, T, R, B; }
'@
Add-Type -MemberDefinition $sig -Name C2 -Namespace KagamiCap -PassThru | Out-Null
$p = Get-Process kagami -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { 'no window'; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object KagamiCap.C2+RECT
[KagamiCap.C2]::GetWindowRect($h, [ref]$r) | Out-Null
$scr = [System.Windows.Forms.Screen]::PrimaryScreen
"window   : $($r.L),$($r.T) -> $($r.R),$($r.B)  ($($r.R-$r.L)x$($r.B-$r.T))"
"screen   : $($scr.Bounds.Width)x$($scr.Bounds.Height)"
"workarea : $($scr.WorkingArea.Width)x$($scr.WorkingArea.Height)  bottom=$($scr.WorkingArea.Bottom)"
"maximized: $([KagamiCap.C2]::IsZoomed($h))"
"below workarea by: $($r.B - $scr.WorkingArea.Bottom) px"
$bmp = New-Object System.Drawing.Bitmap ($r.R-$r.L), ($r.B-$r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc(); [KagamiCap.C2]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
$bmp.Save((Join-Path (Get-Location) $Out))
