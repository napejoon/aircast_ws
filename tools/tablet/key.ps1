# Press one key in the Kagami window without taking focus.
#   key.ps1                     F11
#   key.ps1 -Vk 0x52 -Scan 0x13 R (record)
#   key.ps1 -Vk 0x44 -Scan 0x20 D (fold the strip)
# PostMessage with the scan code in lParam, because on a machine whose layout is
# Thai an injected R arrives as a Thai letter, and keybd_event after
# SetForegroundWindow is refused as often as not. The receiver matches letters
# by key position (gdk_key_event_matches), so the scan code is what counts.
param([int]$Vk = 0x7A, [int]$Scan = 0x57)
$sig = '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);'
Add-Type -MemberDefinition $sig -Name Key -Namespace KagamiTablet | Out-Null
$h = (Get-Process kagami | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1).MainWindowHandle
$down = 1 -bor ($Scan -shl 16)
$up = [int64]$down -bor 0xC0000000
[KagamiTablet.Key]::PostMessage($h, 0x0100, [IntPtr]$Vk, [IntPtr]$down) | Out-Null
Start-Sleep -Milliseconds 80
[KagamiTablet.Key]::PostMessage($h, 0x0101, [IntPtr]$Vk, [IntPtr]$up) | Out-Null
"sent vk $Vk scan $Scan"
