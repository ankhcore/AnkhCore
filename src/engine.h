// AnkhCore 2.5 — telemetry engine (background architecture, D25). Milestone 1: it runs inside the app on its own
// thread, independent of the main window and its timers. Later milestones host it in `AnkhCore.exe --engine`.
// It owns the once-a-second pipeline: Windows counters, the SensorHub snapshot, history and alerts.
// SensorHub keeps its single poll thread, so there is still exactly one reader of the helper's readings file.
// The UI never samples: it copies the latest frame when the engine says one is ready.
#pragma once
#include "base.h"
#include "eng_system.h"
#include "eng_sensors.h"
#include "monitor_store.h"
#include "net_probe.h"
#include "sys_probe.h"
#include "gpu_probe.h"
#include <atomic>

namespace ak {

struct EngineFrame {
    uint64_t seq = 0;
    Usage usage;
    Readings read;
    std::vector<AlertEvent> alerts;   // fired since the last frame was taken
    MonitorSample sample; uint64_t sampleSec = 0;   // this tick's history point, so a remote copy of the history stays in step
    std::vector<NetAdapterView> net;   // every physical adapter with its rates and totals (Windows counters, always live)
    SysExtra sys;                      // per-core usage, memory detail and each fixed drive (Windows counters, always live)
    std::vector<GpuAdapterView> gpus;  // every graphics adapter with its utilization and memory (Windows counters, always live)
};

// This tick's history point. Pure, so the rules are unit-tested: Windows counters (CPU, memory, GPU
// utilization, network) are recorded whether or not the sensor helper is running; helper values are gaps
// when its data is stale. GPU utilization is Usage::gpu (Windows, per adapter, busiest one) and never the
// helper's GPU.LOAD, which reports only the first GPU it lists.
inline MonitorSample EngineSample(const Usage& u, const Readings& r, double netDown, double netUp, bool* fanLiveOut = nullptr) {
    const auto present = [](double v) { return v >= 0 ? v : kNoValue; };
    const bool live = r.present;
    MonitorSample ms; ms.fresh = true;
    ms.v[SCpuLoad] = u.cpu; ms.v[SRam] = u.ram;
    ms.v[SGpuLoad] = present(u.gpu);
    ms.v[SCpuTemp] = live ? present(r.cpuTemp) : kNoValue;
    ms.v[SGpuTemp] = live ? present(r.gpuTemp) : kNoValue;
    ms.v[SNetDown] = present(netDown); ms.v[SNetUp] = present(netUp);
    double fan = kNoValue; bool fanLive = false;
    if (live) for (const auto& f : r.fans) if (f.rpm >= 0 && f.live) { fanLive = true; fan = HasValue(fan) ? std::max(fan, f.rpm) : f.rpm; }
    ms.v[SFan] = fan;
    if (fanLiveOut) *fanLiveOut = fanLive;
    return ms;
}

class TelemetryEngine {
public:
    TelemetryEngine() { InitializeCriticalSection(&cs_); }
    bool Running() const { return running_.load(); }
    DWORD ThreadId() const { return threadId_.load(); }
    uint64_t Ticks() const { return ticks_.load(); }

    void Start(HWND notify, UINT msg) {
        if (running_.exchange(true)) return;
        notify_ = notify; msg_ = msg;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { static_cast<TelemetryEngine*>(p)->Loop(); return 0; }, this, 0, nullptr);
    }
    // Ends the engine thread only. Monitoring itself (SensorHub, helper) is ended by Shutdown().
    void Stop() {
        if (!running_.exchange(false)) return;
        SetEvent(stop_);
        if (thread_) { WaitForSingleObject(thread_, 5000); CloseHandle(thread_); thread_ = nullptr; }
        CloseHandle(stop_); stop_ = nullptr; threadId_ = 0;
    }
    // Whole lifecycle end: the engine, the poller and the helper (which hands fan control back on "quit").
    void Shutdown() { Stop(); Hub().Stop(); Hub().StopHelper(); }

    void Configure(int intervalSec, int historySec, const std::vector<AlertRule>& rules) {
        EnterCriticalSection(&cs_);
        store_.Configure(intervalSec, historySec);
        alerts_.rules = rules;
        LeaveCriticalSection(&cs_);
    }
    // Which adapter the network history series follows (the views pick for themselves from the same frame).
    void SetNetSelection(int mode, const std::wstring& guid) {
        EnterCriticalSection(&cs_); netMode_ = mode < NetAuto || mode > NetManual ? NetAuto : mode; netGuid_ = guid.substr(0, 64); LeaveCriticalSection(&cs_);
    }
    void CopyHistory(MonitorStore& out) { EnterCriticalSection(&cs_); out = store_; LeaveCriticalSection(&cs_); }
    // Copies the latest frame and the history for the UI; alert events are handed over once.
    void Take(EngineFrame& out, MonitorStore& history) {
        EnterCriticalSection(&cs_);
        out = frame_; history = store_;
        frame_.alerts.clear();
        LeaveCriticalSection(&cs_);
        posted_ = false;
    }

private:
    void Loop() {
        threadId_ = GetCurrentThreadId();
        Monitor mon;
        GpuProbe gpu;                    // GPU utilization per adapter (PDH query: created and used on this thread only)
        NetAccounting net; net.Load();   // per-adapter traffic: sampled here, on this thread, once per tick
        SysProbe sys;                    // per-core usage, memory detail, drives: same thread, same tick
        DWORD wait = 250;   // the first frame goes out quickly, so a view is not left empty for a second after a (re)start
        while (WaitForSingleObject(stop_, wait) == WAIT_TIMEOUT) {
            wait = 1000;
            Usage u = mon.Sample();
            std::vector<GpuAdapterView> gpus = gpu.Sample();
            u.gpu = GpuHeadline(gpus);   // the busiest hardware adapter; each adapter's own figure travels beside it
            const Readings r = Hub().Get();
            Hub().Reconcile(r);
            Tick(u, r, net.Tick(), sys.Sample(), std::move(gpus));
            Hub().RenewLease();
            ++ticks_;
            if (notify_ && !posted_.exchange(true)) PostMessageW(notify_, msg_, 0, 0);   // a busy UI gets one message, not a queue
        }
        if (!net.Save()) LogW(L"Network totals could not be saved.");   // Exit, session end and engine stop all come through here
    }
    void Tick(const Usage& u, const Readings& r, std::vector<NetAdapterView> net, SysExtra sys, std::vector<GpuAdapterView> gpus) {
        const uint64_t sec = GetTickCount64() / 1000;
        EnterCriticalSection(&cs_);
        const NetSelection sel = NetSelect(net, netMode_, netGuid_);   // Windows counters: independent of the sensor helper
        bool fanLive = false;
        MonitorSample ms = EngineSample(u, r, sel.down, sel.up, &fanLive);
        if (sys.diskRead >= 0 || sys.diskWrite >= 0)
            ms.v[SDiskIo] = std::max(0.0, sys.diskRead) + std::max(0.0, sys.diskWrite);
        AlertInput ai; ai.fresh = r.present; ai.cpuTemp = ms.v[SCpuTemp]; ai.gpuTemp = ms.v[SGpuTemp]; ai.fanLive = fanLive; ai.fanRpm = ms.v[SFan];
        ai.ramPct = ms.v[SRam]; ai.cpuLoad = ms.v[SCpuLoad];
        if (HasValue(sel.down) || HasValue(sel.up))
            ai.netMbps = (std::max(0.0, HasValue(sel.down) ? sel.down : 0.0) + std::max(0.0, HasValue(sel.up) ? sel.up : 0.0)) * 8.0 / 1e6;
        double minFree = kNoValue;
        for (const auto& d : sys.drives) {
            if (d.total == 0) continue;
            const double pct = 100.0 * static_cast<double>(d.free) / static_cast<double>(d.total);
            minFree = HasValue(minFree) ? std::min(minFree, pct) : pct;
        }
        ai.diskFreePct = minFree;
        store_.Tick(sec, ms);
        for (const auto& ev : alerts_.Tick(sec, ai)) frame_.alerts.push_back(ev);
        ++frame_.seq; frame_.usage = u; frame_.read = r; frame_.sample = ms; frame_.sampleSec = sec; frame_.net = std::move(net); frame_.sys = std::move(sys); frame_.gpus = std::move(gpus);
        LeaveCriticalSection(&cs_);
    }

    CRITICAL_SECTION cs_;
    MonitorStore store_; AlertEngine alerts_; EngineFrame frame_;
    int netMode_ = NetAuto; std::wstring netGuid_;
    HWND notify_ = nullptr; UINT msg_ = 0;
    HANDLE thread_ = nullptr, stop_ = nullptr;
    std::atomic<bool> running_{false}, posted_{false};
    std::atomic<DWORD> threadId_{0};
    std::atomic<uint64_t> ticks_{0};
};

}  // namespace ak
