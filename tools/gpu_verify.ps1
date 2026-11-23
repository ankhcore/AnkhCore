# Live check of GPU utilization through the whole pipeline (engine -> IPC -> pages, Sidebar, Mini), under real
# GPU load, against Windows' own "GPU Engine" counters (the source Task Manager uses).
# Touches only AnkhCore_TEST.exe and tools\gpu_load.cpp's off-screen workload. Needs: tools\build_test.cmd and
# tools\build_gpu_load.cmd. Usage: powershell -ExecutionPolicy Bypass -File tools\gpu_verify.ps1 [-LoadSeconds 16]
param([int]$LoadSeconds = 16, [int]$IdleSeconds = 10, [int]$RestSeconds = 9)
Set-Location (Split-Path $PSScriptRoot -Parent)
$fails = 0
function Chk($ok, $t) { if ($ok) { "PASS  $t" } else { "FAIL  $t"; $script:fails++ } }
$data = "$env:LOCALAPPDATA\AnkhCore-Test"; $cfg = "$data\settings.json"; $log = "$data\gpu-log.txt"; $bak = "$cfg.gpubak"
if (@(Get-Process AnkhCore_TEST -ErrorAction SilentlyContinue).Count) { "AnkhCore_TEST is already running; close it first."; exit 2 }
$adapters = @(& build-test\gpu\gpu_load.exe --list | ForEach-Object { if ($_ -match '^(\d+) \| luid (0x[0-9a-f]+_0x[0-9a-f]+) \| software (\d) \| (.+)$') { [pscustomobject]@{ index = [int]$Matches[1]; luid = $Matches[2]; software = $Matches[3] -eq '1'; name = $Matches[4].Trim() } } })
$hw = @($adapters | Where-Object { -not $_.software })
Chk ($hw.Count -ge 1) "hardware adapters found by DXGI: $(($hw | ForEach-Object name) -join ', ')"

# Windows' figure per adapter: sum each engine over processes, take the adapter's busiest engine.
function Ref {
    $s = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples
    $eng = @{}
    foreach ($c in $s) { if ($c.InstanceName -match 'luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_') { $k = "$($Matches[1])|$($Matches[2])|$($Matches[3])"; $eng[$k] = [double]$eng[$k] + $c.CookedValue } }
    $out = @{}; foreach ($a in $adapters) { $out[$a.luid] = 0.0 }
    foreach ($k in $eng.Keys) { $l = $k.Split('|')[0]; $v = [math]::Min(100.0, $eng[$k]); if ($v -gt [double]$out[$l]) { $out[$l] = $v } }
    $out
}
function Phase($name, $seconds, $loadIndex) {
    $p = $null
    if ($loadIndex -ge 0) { $p = Start-Process build-test\gpu\gpu_load.exe -ArgumentList "$loadIndex $seconds" -WindowStyle Hidden -PassThru; Start-Sleep 3 }   # let the load settle before sampling
    $t0 = Get-Date; $refs = @()
    $until = if ($loadIndex -ge 0) { $seconds - 5 } else { $seconds }
    while (((Get-Date) - $t0).TotalSeconds -lt $until) { $refs += , (Ref) }
    $t1 = Get-Date
    if ($p) { $p.WaitForExit(($seconds + 10) * 1000) | Out-Null }
    [pscustomobject]@{ name = $name; from = $t0; to = $t1; refs = $refs; load = $loadIndex }
}

Copy-Item $cfg $bak -Force -ErrorAction SilentlyContinue
try {
    $j = if (Test-Path $bak) { Get-Content $bak -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    foreach ($k in 'sidebar.enabled', 'mini.enabled') { $j | Add-Member -NotePropertyName $k -NotePropertyValue $true -Force }
    $j | Add-Member -NotePropertyName 'mini.metrics' -NotePropertyValue 255 -Force
    $j | Add-Member -NotePropertyName 'sidebar.layout' -NotePropertyValue 1 -Force
    New-Item -ItemType Directory -Force $data | Out-Null
    [IO.File]::WriteAllText($cfg, ($j | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
    Remove-Item $log -ErrorAction SilentlyContinue
    $env:ANKH_GPULOG = '1'
    Start-Process build-test\out\AnkhCore_TEST.exe | Out-Null
    Start-Sleep 12
    $phases = @(Phase 'idle' $IdleSeconds -1)
    foreach ($a in $hw) { $phases += Phase "load on $($a.name)" $LoadSeconds $a.index; $phases += Phase "rest after $($a.name)" $RestSeconds -1 }
    Add-Type -Name G -Namespace H -MemberDefinition '[DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string c,string t); [DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h,uint m,System.IntPtr w,System.IntPtr l);'
    $h = [H.G]::FindWindowW('AnkhCoreWindow.Test', [NullString]::Value); [void][H.G]::PostMessageW($h, 0x111, [IntPtr]103, [IntPtr]0)
    for ($i = 0; $i -lt 40 -and @(Get-Process AnkhCore_TEST -ErrorAction SilentlyContinue).Count -gt 0; $i++) { Start-Sleep -Milliseconds 300 }
} finally {
    Remove-Item Env:\ANKH_GPULOG -ErrorAction SilentlyContinue
    if (Test-Path $bak) { Copy-Item $bak $cfg -Force; Remove-Item $bak }
}

# AnkhCore's own record: one line per frame with what each surface showed
$lines = @(Get-Content $log -Encoding UTF8 -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_ -match '^(\d\d:\d\d:\d\d) seq=(\d+) head=([-\d.]+) history=([-\d.]+) sidebar=([-\d.]+) mini=(\S+) helperGpuLoad=(\S*)(.*)$') {
        $m = $Matches; $per = @{}   # the inner -match below replaces $Matches
        foreach ($part in $m[8].Split('|')) { if ($part -match '^\s*(.+)=([-\d.]+)\s*$') { $per[$Matches[1].Trim()] = [double]$Matches[2] } }
        [pscustomobject]@{ t = [datetime]::ParseExact($m[1], 'HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture); head = [double]$m[3]; history = [double]$m[4]; sidebar = [double]$m[5]; mini = $m[6]; helper = $m[7]; per = $per }
    } })
Chk ($lines.Count -ge 30) "AnkhCore recorded $($lines.Count) frames"
function Avg($xs) { if (@($xs).Count) { [math]::Round((@($xs) | Measure-Object -Average).Average, 1) } else { -1 } }
"";  "{0,-34} {1,8} {2,8}   {3}" -f 'phase', 'AnkhCore', 'Windows', 'per adapter: AnkhCore / Windows'
$summary = @{}
foreach ($ph in $phases) {
    $fr = @($lines | Where-Object { $_.t.TimeOfDay -ge $ph.from.TimeOfDay.Add([timespan]::FromSeconds(1)) -and $_.t.TimeOfDay -le $ph.to.TimeOfDay })
    $perTxt = @(); $row = @{ frames = $fr.Count; head = (Avg ($fr | ForEach-Object head)) }
    $refHead = Avg ($ph.refs | ForEach-Object { ($hw | ForEach-Object { $_r = $_; 0 }) | Out-Null; $m = 0.0; foreach ($a in $hw) { if ($_[$a.luid] -gt $m) { $m = $_[$a.luid] } }; $m })
    foreach ($a in $hw) {
        $app = Avg ($fr | ForEach-Object { $_.per[$a.name] }); $ref = Avg ($ph.refs | ForEach-Object { $_[$a.luid] })
        $row[$a.name] = $app; $row["ref:$($a.name)"] = $ref; $perTxt += "$($a.name) $app / $ref"
    }
    $row.refHead = $refHead; $summary[$ph.name] = $row
    "{0,-34} {1,8} {2,8}   {3}" -f $ph.name, $row.head, $refHead, ($perTxt -join '   ')
}
""
$idle = $summary['idle']
Chk ($idle.frames -ge 5 -and $idle.head -ge 0) "idle: AnkhCore reads $($idle.head) % (Windows $($idle.refHead) %)"
foreach ($a in $hw) {
    $ld = $summary["load on $($a.name)"]; $rs = $summary["rest after $($a.name)"]
    Chk ($ld[$a.name] -ge $idle[$a.name] + 20) "$($a.name): rises under sustained load ($($idle[$a.name]) % idle -> $($ld[$a.name]) %)"
    Chk ([math]::Abs($ld[$a.name] - $ld["ref:$($a.name)"]) -le 15) "$($a.name): under load AnkhCore $($ld[$a.name]) % vs Windows $($ld["ref:$($a.name)"]) % (within 15 points)"
    Chk ($ld.head -ge $ld[$a.name] - 3) "$($a.name): the single GPU figure follows it while it is the busiest ($($ld.head) %)"
    Chk ($rs[$a.name] -le $ld[$a.name] - 20) "$($a.name): falls again when the load stops ($($rs[$a.name]) %)"
    foreach ($o in @($hw | Where-Object { $_.luid -ne $a.luid })) { Chk ([math]::Abs($ld[$o.name] - $ld["ref:$($o.name)"]) -le 10 -and [math]::Abs($ld[$o.name] - $idle[$o.name]) -le 15) "$($a.name) loaded: $($o.name) keeps its own reading ($($ld[$o.name]) %, Windows $($ld["ref:$($o.name)"]) %, idle $($idle[$o.name]) %); the load is not mixed into it" }
}
$valid = @($lines | Where-Object { $_.head -ge 0 })
Chk (@($valid | Where-Object { $_.history -ne $_.head }).Count -eq 0) 'history graph: same value as the pages in every frame'
Chk (@($valid | Where-Object { $_.sidebar -gt -2 -and $_.sidebar -ne $_.head }).Count -eq 0 -and @($valid | Where-Object { $_.sidebar -gt -2 }).Count -ge 30) 'Sidebar: same value as the pages in every frame'
Chk (@($valid | Where-Object { $_.mini -ne '-' -and [math]::Abs([double]($_.mini.TrimEnd('%')) - $_.head) -gt 0.51 }).Count -eq 0 -and @($valid | Where-Object { $_.mini -ne '-' }).Count -ge 30) 'Mini Monitor: same value as the pages in every frame'
$distinct = @($valid | ForEach-Object head | Sort-Object -Unique).Count
Chk ($distinct -ge 10) "the reading changes dynamically ($distinct different values)"
$helper = @($lines | ForEach-Object helper | Sort-Object -Unique) -join ','
"INFO  sensor helper GPU.LOAD during the run: $helper (no longer used for utilization)"
Chk (@(Get-Process AnkhCore_TEST -ErrorAction SilentlyContinue).Count -eq 0) 'no test process left running'
"GPU VERIFY failures=$fails"
