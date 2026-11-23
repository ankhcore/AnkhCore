# Reproducible per-process GPU / CPU / paint measurement of AnkhCore_TEST.exe (M-2.6-GPU1; the method of M-GPU2,
# docs/test-logs/2026-11-14_M-GPU2.md, which had no script in the repository).
#
# What it does, per scenario: backs up the TEST settings file, writes a minimal one ({"nav.page": N} plus optional keys; every
# other key keeps its default, so the Sidebar and Mini Monitor stay off), starts build-test\out\AnkhCore_TEST.exe with ANKH_PERF=1,
# waits $SettleSeconds, then samples $SampleSeconds one-second intervals of
#   - GPU: \GPU Engine(*)\Utilization Percentage, only the instances of the UI process and (separately) of its --engine child; per
#     (luid, phys, eng, engtype) the instances are summed, the busiest engine is the process figure (the rule of src\gpu_util.h);
#   - CPU: process time of the UI process and (separately) of the engine child (percent of one core, and of all logical processors);
#   - paints: the "paints=" counter that the test build writes once a second to %LOCALAPPDATA%\AnkhCore-Test\perf.txt, and lastMs.
# It touches only the processes it started (and their engine child). It changes no system setting; the test build refuses those.
# It does show an AnkhCore_TEST window on the current desktop for about SettleSeconds+SampleSeconds per scenario, so do not run it
# while the window matters to you. Needs: tools\build_test.cmd. Counters are Windows' own, the numbers are whatever this machine gives:
# compare runs only on the same machine, same state, and say so.
# Usage: powershell -ExecutionPolicy Bypass -File tools\gpu_perf.ps1 [-Scenarios about,dashboard,hardware] [-OutFile path.md]
param(
    [string[]]$Scenarios = @('about', 'dashboard', 'hardware', 'network', 'backup', 'rechistory'),
    [int]$SettleSeconds = 5, [int]$SampleSeconds = 10, [string]$OutFile = ''
)
Set-Location (Split-Path $PSScriptRoot -Parent)
$Scenarios = @($Scenarios | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # `-File` passes a comma list as one string
$exe = Join-Path (Get-Location) 'build-test\out\AnkhCore_TEST.exe'
if (-not (Test-Path $exe)) { "Missing $exe. Run tools\build_test.cmd first."; exit 2 }
if (@(Get-Process AnkhCore_TEST -ErrorAction SilentlyContinue).Count) { 'AnkhCore_TEST is already running; close it first.'; exit 2 }
$data = "$env:LOCALAPPDATA\AnkhCore-Test"; $cfg = "$data\settings.json"; $bak = "$cfg.gpuperfbak"; $perf = "$data\perf.txt"
$nCpu = [Environment]::ProcessorCount
# nav.page values = the `enum Page` order of src\app.cpp (PDashboard 0 ... PAbout 10 ... PBackup 28, PRecHistory 29, PHardware 32, PNetwork 33)
$pages = @{ dashboard = 0; monitor = 1; about = 10; backup = 28; rechistory = 29; hardware = 32; network = 33 }

function Get-EngineFigure($samples, [int[]]$pids) {
    $eng = @{}
    foreach ($c in $samples) {
        if ($c.InstanceName -match '^pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)_phys_(\d+)_eng_(\d+)_engtype_(.+)$' -and ($pids -contains [int]$Matches[1])) {
            $k = "$($Matches[2])|$($Matches[3])|$($Matches[4])|$($Matches[5])"; $eng[$k] = [double]$eng[$k] + $c.CookedValue
        }
    }
    $best = 0.0; foreach ($v in $eng.Values) { $v = [math]::Min(100.0, $v); if ($v -gt $best) { $best = $v } }
    $best
}
function Get-LastPerf { if (Test-Path $perf) { $l = Get-Content $perf -Tail 1 -ErrorAction SilentlyContinue; if ($l -match 'paints=(\d+).*lastMs=([\d.]+).*fg=(\d)') { return @{ paints = [int64]$Matches[1]; lastMs = [double]$Matches[2]; fg = [int]$Matches[3] } } } $null }

$rows = @(); $hadCfg = Test-Path $cfg
if ($hadCfg) { Copy-Item $cfg $bak -Force }
try {
    foreach ($s in $Scenarios) {
        if (-not $pages.ContainsKey($s)) { "Unknown scenario '$s' (known: $($pages.Keys -join ', '))"; continue }
        Set-Content -Path $cfg -Value ('{ "nav.page": ' + $pages[$s] + ' }') -Encoding UTF8
        Remove-Item $perf -ErrorAction SilentlyContinue
        $env:ANKH_PERF = '1'
        $p = Start-Process $exe -PassThru
        Remove-Item Env:\ANKH_PERF
        Start-Sleep -Seconds $SettleSeconds
        $eng = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq 'AnkhCore_TEST.exe' })
        $pids = @($p.Id) + @($eng | ForEach-Object { [int]$_.ProcessId })
        $procs = @($pids | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue })
        $cpu0 = @{}; foreach ($q in $procs) { $cpu0[$q.Id] = $q.TotalProcessorTime.TotalSeconds }
        $perf0 = Get-LastPerf; $t0 = Get-Date; $gpuUi = @(); $gpuEng = @(); $enginePids = @($pids | Where-Object { $_ -ne $p.Id })
        for ($i = 0; $i -lt $SampleSeconds; ++$i) {
            $c = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction SilentlyContinue).CounterSamples
            $gpuUi += Get-EngineFigure $c @($p.Id); $gpuEng += Get-EngineFigure $c $enginePids
        }
        $wall = ((Get-Date) - $t0).TotalSeconds; $perf1 = Get-LastPerf
        $cpuUi = 0.0; $cpuEng = 0.0
        foreach ($q in $procs) { $q.Refresh(); if (-not $q.HasExited) { $d = $q.TotalProcessorTime.TotalSeconds - $cpu0[$q.Id]; if ($q.Id -eq $p.Id) { $cpuUi += $d } else { $cpuEng += $d } } }
        $paints = if ($perf0 -and $perf1) { $perf1.paints - $perf0.paints } else { $null }
        $rows += [pscustomobject]@{
            scenario = $s; pids = ($pids -join '/'); gpuAvg = [math]::Round(($gpuUi | Measure-Object -Average).Average, 2); gpuMax = [math]::Round(($gpuUi | Measure-Object -Maximum).Maximum, 2); engGpu = [math]::Round(($gpuEng | Measure-Object -Maximum).Maximum, 2)
            cpuOneCore = [math]::Round(100 * $cpuUi / $wall, 2); cpuTotal = [math]::Round(100 * $cpuUi / $wall / $nCpu, 3); engCpu = [math]::Round(100 * $cpuEng / $wall, 2)
            paints = $paints; paintsPerSec = if ($null -ne $paints) { [math]::Round($paints / $wall, 2) } else { $null }
            lastMs = if ($perf1) { $perf1.lastMs } else { $null }; fg = if ($perf1) { $perf1.fg } else { $null }; engineSeen = ($pids.Count -ge 2)
        }
        # stop only what this run started: the UI window first (graceful), then anything of ours that is left
        [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(6000)) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
        foreach ($e in $eng) { Stop-Process -Id ([int]$e.ProcessId) -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 2
    }
}
finally {
    if ($hadCfg) { Move-Item $bak $cfg -Force } else { Remove-Item $cfg -ErrorAction SilentlyContinue }
    Remove-Item Env:\ANKH_PERF -ErrorAction SilentlyContinue
}
$md = @("| scenario | UI/engine PID | UI GPU % avg | UI GPU % max | UI CPU % of 1 core | UI CPU % of all $nCpu | engine GPU % max | engine CPU % of 1 core | paints (window) | paints/s | last frame ms | fg | engine seen |", '|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|')
foreach ($r in $rows) { $md += "| $($r.scenario) | $($r.pids) | $($r.gpuAvg) | $($r.gpuMax) | $($r.cpuOneCore) | $($r.cpuTotal) | $($r.engGpu) | $($r.engCpu) | $($r.paints) | $($r.paintsPerSec) | $($r.lastMs) | $($r.fg) | $($r.engineSeen) |" }
$md
if ($OutFile) { $md | Set-Content -Path $OutFile -Encoding UTF8 }
