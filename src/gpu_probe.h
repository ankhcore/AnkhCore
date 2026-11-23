// AnkhCore 2.5 — GPU utilization and memory per adapter, for the engine thread only (no poller of its own).
// Source: Windows' "GPU Engine" and "GPU Adapter Memory" performance counters, the ones Task Manager uses.
// Adapters come from DXGI and are matched to the counters by LUID. The sensor helper's GPU.LOAD is not
// used for utilization: it reports only the first GPU it lists, which on a hybrid laptop is the idle one.
#pragma once
#include "base.h"
#include "gpu_util.h"
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

namespace ak {

class GpuProbe {
public:
    GpuProbe() {
        if (PdhOpenQueryW(nullptr, 0, &q_) != ERROR_SUCCESS) { q_ = nullptr; return; }
        if (PdhAddEnglishCounterW(q_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &util_) != ERROR_SUCCESS) util_ = nullptr;
        if (PdhAddEnglishCounterW(q_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &ded_) != ERROR_SUCCESS) ded_ = nullptr;
        if (PdhAddEnglishCounterW(q_, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &shr_) != ERROR_SUCCESS) shr_ = nullptr;
        PdhCollectQueryData(q_);   // utilization is a rate: the first real value comes with the second collection
        Enumerate();
    }
    ~GpuProbe() { if (q_) PdhCloseQuery(q_); }
    GpuProbe(const GpuProbe&) = delete; GpuProbe& operator=(const GpuProbe&) = delete;

    // Every adapter with its utilization this tick. A reading is never carried over from an earlier tick:
    // if the counters fail, the adapters come back with -1 ("no reading"), not with an old number.
    std::vector<GpuAdapterView> Sample() {
        ++tick_;
        std::vector<GpuCounterSample> engines, ded, shr;
        const bool collected = q_ && PdhCollectQueryData(q_) == ERROR_SUCCESS;
        const bool ok = collected && util_ && Read(util_, engines);
        if (collected) { if (ded_) Read(ded_, ded); if (shr_) Read(shr_, shr); }
        const std::vector<GpuAdapterUtil> utils = GpuAggregate(engines);
        bool unknown = false;   // a counter names an adapter DXGI did not list: a GPU was added or the driver restarted
        for (const GpuAdapterUtil& u : utils) { bool have = false; for (const GpuAdapterView& a : adapters_) have |= a.luid == u.luid; unknown |= !have; }
        if ((unknown && tick_ - lastEnum_ >= 5) || tick_ - lastEnum_ >= 60) Enumerate();
        std::vector<GpuAdapterView> out = adapters_;
        GpuJoin(out, utils, ok);
        for (GpuAdapterView& a : out) {
            for (const GpuCounterSample& c : ded) { GpuLuid l; if (GpuParseAdapter(c.instance, l) && l == a.luid && c.value >= 0) a.dedicatedUsed = static_cast<uint64_t>(c.value); }
            for (const GpuCounterSample& c : shr) { GpuLuid l; if (GpuParseAdapter(c.instance, l) && l == a.luid && c.value >= 0) a.sharedUsed = static_cast<uint64_t>(c.value); }
        }
        return out;
    }

private:
    void Enumerate() {
        lastEnum_ = tick_;
        IDXGIFactory1* f = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return;   // keep the list we have
        std::vector<GpuAdapterView> found;
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; i < 16 && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            if (a && SUCCEEDED(a->GetDesc1(&d))) {
                GpuAdapterView v; v.name = d.Description; v.luid.hi = static_cast<uint32_t>(d.AdapterLuid.HighPart); v.luid.lo = d.AdapterLuid.LowPart;
                v.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0; v.dedicatedTotal = d.DedicatedVideoMemory;
                bool dup = false; for (const GpuAdapterView& o : found) dup |= o.luid == v.luid;
                if (!dup) found.push_back(v);
            }
            if (a) { a->Release(); a = nullptr; }
        }
        f->Release();
        adapters_ = std::move(found);
    }
    // All instances of a wildcard counter. False when the counter cannot be read at all.
    bool Read(PDH_HCOUNTER c, std::vector<GpuCounterSample>& out) {
        DWORD sz = 0, n = 0;
        const PDH_STATUS s = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &n, nullptr);
        if (s == ERROR_SUCCESS) return true;   // no instance at all right now
        if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return false;
        buf_.resize(sz);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &n, items) != ERROR_SUCCESS) return false;
        out.reserve(n);
        for (DWORD i = 0; i < n; ++i) {
            if (!items[i].szName || items[i].FmtValue.CStatus > 1) continue;   // 0 valid, 1 new data; a process that just appeared has no rate yet
            out.push_back({items[i].szName, items[i].FmtValue.doubleValue});
        }
        return true;
    }
    PDH_HQUERY q_ = nullptr; PDH_HCOUNTER util_ = nullptr, ded_ = nullptr, shr_ = nullptr;
    std::vector<GpuAdapterView> adapters_; std::vector<uint8_t> buf_;
    uint64_t tick_ = 0, lastEnum_ = 0;
};

}  // namespace ak
