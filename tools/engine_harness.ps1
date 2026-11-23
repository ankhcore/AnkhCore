# Black-box lifecycle checks of the engine process. Touches only AnkhCore_TEST.exe processes.
Set-Location D:\Ankhcore\ankhcore-src
$fails = 0
function Chk($ok, $t) { if ($ok) { "PASS  $t" } else { "FAIL  $t"; $script:fails++ } }
function Procs { @(Get-CimInstance Win32_Process -Filter "Name='AnkhCore_TEST.exe'") }
function Eng { @(Procs | Where-Object { $_.CommandLine -match '--engine' }) }
function Ui { @(Procs | Where-Object { $_.CommandLine -notmatch '--engine' }) }
Add-Type -Name U -Namespace H -MemberDefinition '[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string c,string t); [DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h,uint m,System.IntPtr w,System.IntPtr l);'
Chk (@(Procs).Count -eq 0) 'nothing left running from an earlier run'
$prod0 = @(Get-Process AnkhCore -ErrorAction SilentlyContinue).Id -join ','
Start-Process build-test\out\AnkhCore_TEST.exe | Out-Null; Start-Sleep 7
Chk (@(Eng).Count -eq 1) 'one engine process started by the UI'
Chk (@(Ui).Count -eq 1) 'one UI process'
$e1 = @(Eng)[0].ProcessId
# a foreign program tries the pipe
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$foreign = $false
try {
  $c = New-Object IO.Pipes.NamedPipeClientStream('.', "AnkhCore.Engine.Test.$sid", [IO.Pipes.PipeDirection]::InOut)
  # the UI holds the single instance: a connect is expected to time out
  $c.Connect(1500); $foreign = $true; $c.Dispose()
} catch { }
Chk (-not $foreign) 'a foreign program cannot reach the pipe while the UI is connected'
# kill the UI: the engine must survive
Stop-Process -Id @(Ui)[0].ProcessId -Force; Start-Sleep 4
Chk (@(Ui).Count -eq 0) 'UI killed'
Chk ((@(Eng).Count -eq 1) -and (@(Eng)[0].ProcessId -eq $e1)) 'engine survives a UI crash'
# with no UI connected, a foreign client connects but must receive nothing
$got = -2
try {
  $c = New-Object IO.Pipes.NamedPipeClientStream('.', "AnkhCore.Engine.Test.$sid", [IO.Pipes.PipeDirection]::InOut)
  $c.Connect(2000); $b = New-Object byte[] 12; $t = $c.ReadAsync($b, 0, 12)
  if ($t.Wait(2500)) { $got = $t.Result } else { $got = -1 }
  $c.Dispose()
} catch { $got = 0 }
Chk ($got -le 0) "a foreign client receives no data (read returned $got)"
Start-Sleep 1
Chk ((@(Eng).Count -eq 1) -and (@(Eng)[0].ProcessId -eq $e1)) 'the refused client did not disturb the engine'
# a new UI reconnects to the same engine
Start-Process build-test\out\AnkhCore_TEST.exe | Out-Null; Start-Sleep 6
Chk ((@(Eng).Count -eq 1) -and (@(Eng)[0].ProcessId -eq $e1)) 'a new UI reconnects to the same engine; no duplicate'
# X button: hidden, nothing exits
$h = [H.U]::FindWindowW('AnkhCoreWindow.Test', [NullString]::Value)
Chk ($h -ne [IntPtr]::Zero) 'UI window found'
[void][H.U]::PostMessageW($h, 0x10, [IntPtr]0, [IntPtr]0); Start-Sleep 2
Chk ((@(Ui).Count -eq 1) -and (@(Eng).Count -eq 1)) 'X button (WM_CLOSE): UI and engine keep running'
# tray Exit
[void][H.U]::PostMessageW($h, 0x111, [IntPtr]103, [IntPtr]0)
for ($i = 0; $i -lt 40 -and @(Procs).Count -gt 0; $i++) { Start-Sleep -Milliseconds 300 }
Chk (@(Procs).Count -eq 0) 'tray Exit: no UI or engine process remains (D26)'
# the engine saves the per-adapter network totals on its way out
$nc = Get-Item "$env:LOCALAPPDATA\AnkhCore-Test\net-counters.txt" -ErrorAction SilentlyContinue
Chk (($null -ne $nc) -and (((Get-Date) - $nc.LastWriteTime).TotalSeconds -lt 30) -and ((Get-Content $nc.FullName -TotalCount 1) -eq 'ankh-net 2') -and ((Get-Content $nc.FullName -Tail 1) -eq 'end') -and (-not (Test-Path "$($nc.FullName).tmp"))) 'tray Exit: network totals saved whole (no temp file left)'
# sign-in start: hidden, with its engine
Add-Type -Name V -Namespace H -MemberDefinition '[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);'
Start-Process build-test\out\AnkhCore_TEST.exe -ArgumentList '--startup' | Out-Null; Start-Sleep 7
$h2 = [H.U]::FindWindowW('AnkhCoreWindow.Test', [NullString]::Value)
Chk ((@(Ui).Count -eq 1) -and (@(Eng).Count -eq 1)) 'sign-in start (--startup): UI and engine running'
Chk (($h2 -ne [IntPtr]::Zero) -and (-not [H.V]::IsWindowVisible($h2))) 'sign-in start: the window stays hidden in the tray'
# the Exit command reaching an engine with no UI
Stop-Process -Id @(Ui)[0].ProcessId -Force; Start-Sleep 2
$eh = [H.U]::FindWindowW('AnkhCoreEngineWindow.Test', [NullString]::Value)
Chk ($eh -ne [IntPtr]::Zero) 'engine window found with no UI'
[void][H.U]::PostMessageW($eh, 0x111, [IntPtr]103, [IntPtr]0)
for ($i = 0; $i -lt 40 -and @(Procs).Count -gt 0; $i++) { Start-Sleep -Milliseconds 300 }
Chk (@(Procs).Count -eq 0) 'the Exit command stops an engine left without a UI'
$prod1 = @(Get-Process AnkhCore -ErrorAction SilentlyContinue).Id -join ','
Chk ($prod0 -eq $prod1) "installed AnkhCore untouched (pids $prod0)"
Procs | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
"HARNESS failures=$fails"
