// AnkhCore 2.5 — GPU utilization from Windows' "GPU Engine" counters (pure, unit-tested).
// Windows publishes one counter instance per process, adapter and engine:
//   pid_1234_luid_0x00000000_0x0000E3C5_phys_0_eng_0_engtype_3D
// An engine's utilization is the sum over processes; an adapter's utilization is its busiest engine.
// That is the figure Task Manager shows for a GPU. Adapters are identified by LUID, never by position.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace ak {

struct GpuLuid {
    uint32_t hi = 0, lo = 0;
    bool operator==(const GpuLuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator<(const GpuLuid& o) const { return std::tie(hi, lo) < std::tie(o.hi, o.lo); }
};
struct GpuCounterSample { std::wstring instance; double value = 0; };
struct GpuAdapterUtil { GpuLuid luid; double util = 0, util3d = 0; int engines = 0; };   // util: busiest engine; util3d: the 3D engines only

namespace gpu_detail {
inline std::wstring Lc(std::wstring s) { for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; }
// Reads "0x<hex>" at `pos`; moves `pos` past it.
inline bool Hex(const std::wstring& s, size_t& pos, uint32_t& out) {
    if (pos + 2 >= s.size() || s[pos] != L'0' || s[pos + 1] != L'x') return false;
    size_t i = pos + 2; uint64_t v = 0; int n = 0;
    for (; i < s.size() && std::iswxdigit(s[i]) && n < 9; ++i, ++n) v = v * 16 + static_cast<uint64_t>(s[i] <= L'9' ? s[i] - L'0' : s[i] - L'a' + 10);
    if (n == 0 || n > 8) return false;
    out = static_cast<uint32_t>(v); pos = i; return true;
}
inline bool Dec(const std::wstring& s, size_t& pos, int& out) {
    size_t i = pos; long v = 0; int n = 0;
    for (; i < s.size() && s[i] >= L'0' && s[i] <= L'9' && n < 7; ++i, ++n) v = v * 10 + (s[i] - L'0');
    if (n == 0) return false;
    out = static_cast<int>(v); pos = i; return true;
}
inline bool Eat(const std::wstring& s, size_t& pos, const wchar_t* lit) { const size_t n = wcslen(lit); if (s.compare(pos, n, lit) != 0) return false; pos += n; return true; }
// "luid_0x........_0x........" starting at `pos`.
inline bool Luid(const std::wstring& s, size_t& pos, GpuLuid& l) { return Eat(s, pos, L"luid_") && Hex(s, pos, l.hi) && Eat(s, pos, L"_") && Hex(s, pos, l.lo); }
}  // namespace gpu_detail

// "pid_N_luid_0xH_0xL_phys_P_eng_E_engtype_T" (any letter case). False for anything else.
inline bool GpuParseEngine(const std::wstring& instance, GpuLuid& luid, int& phys, int& eng, std::wstring& type) {
    using namespace gpu_detail;
    const std::wstring s = Lc(instance); size_t p = 0; int pid = 0;
    if (!Eat(s, p, L"pid_") || !Dec(s, p, pid) || !Eat(s, p, L"_") || !Luid(s, p, luid)) return false;
    if (!Eat(s, p, L"_phys_") || !Dec(s, p, phys) || !Eat(s, p, L"_eng_") || !Dec(s, p, eng) || !Eat(s, p, L"_engtype_")) return false;
    type = s.substr(p);
    return !type.empty();
}
// "luid_0xH_0xL_phys_P" (the "GPU Adapter Memory" instances).
inline bool GpuParseAdapter(const std::wstring& instance, GpuLuid& luid) {
    using namespace gpu_detail;
    const std::wstring s = Lc(instance); size_t p = 0; int phys = 0;
    return Luid(s, p, luid) && Eat(s, p, L"_phys_") && Dec(s, p, phys) && p == s.size();
}
// One entry per adapter that has at least one engine instance. Values that are not finite or are negative
// count as 0; an engine never exceeds 100 %.
inline std::vector<GpuAdapterUtil> GpuAggregate(const std::vector<GpuCounterSample>& samples) {
    std::map<std::tuple<GpuLuid, int, int>, std::pair<double, bool>> engines;   // (adapter, phys, engine) -> (sum, is 3D)
    for (const GpuCounterSample& c : samples) {
        GpuLuid l; int phys = 0, eng = 0; std::wstring type;
        if (!GpuParseEngine(c.instance, l, phys, eng, type)) continue;
        auto& e = engines[std::make_tuple(l, phys, eng)];
        if (c.value == c.value && c.value > 0 && c.value < 1e6) e.first += c.value;
        e.second = type == L"3d";
    }
    std::map<GpuLuid, GpuAdapterUtil> by;
    for (const auto& kv : engines) {
        GpuAdapterUtil& a = by[std::get<0>(kv.first)]; a.luid = std::get<0>(kv.first); ++a.engines;
        const double v = std::min(100.0, kv.second.first);
        a.util = std::max(a.util, v);
        if (kv.second.second) a.util3d = std::max(a.util3d, v);
    }
    std::vector<GpuAdapterUtil> out; for (const auto& kv : by) out.push_back(kv.second);
    return out;
}

// The process an engine instance belongs to ("pid_N_..."); -1 when the name is not an engine instance. The match is exact:
// pid 10 never matches "pid_100_...".
inline long long GpuInstancePid(const std::wstring& instance) {
    GpuLuid l; int phys = 0, eng = 0; std::wstring type;
    if (!GpuParseEngine(instance, l, phys, eng, type)) return -1;
    const std::wstring s = gpu_detail::Lc(instance); size_t p = 4; int pid = 0;   // after "pid_" (GpuParseEngine has verified the whole name)
    return gpu_detail::Dec(s, p, pid) ? pid : -1;
}
// GPU utilization of ONE process (M-2.6-GPU1: the rule the M-GPU2 baseline used, which had no code in the repository and is the one
// tools\gpu_perf.ps1 applies): only the process's own engine instances; instances of the same engine are summed; the process figure is its
// busiest engine over all adapters (util), or over its 3D engines (util3d); an engine never exceeds 100 %. A process with no instance
// is 0 with engines == 0 (the caller knows whether the counters work at all).
inline GpuAdapterUtil GpuProcessUtil(const std::vector<GpuCounterSample>& samples, uint32_t pid) {
    std::vector<GpuCounterSample> mine;
    for (const GpuCounterSample& c : samples) if (GpuInstancePid(c.instance) == static_cast<long long>(pid)) mine.push_back(c);
    GpuAdapterUtil r;
    for (const GpuAdapterUtil& a : GpuAggregate(mine)) { r.util = std::max(r.util, a.util); r.util3d = std::max(r.util3d, a.util3d); r.engines += a.engines; }
    return r;
}

// What the engine publishes for one adapter (DXGI identity joined to the counters by LUID).
struct GpuAdapterView {
    std::wstring name; GpuLuid luid; bool software = false;   // software: Microsoft Basic Render Driver and the like
    double util = -1;                                         // %, -1 = Windows gives no reading for this adapter
    uint64_t dedicatedUsed = 0, sharedUsed = 0, dedicatedTotal = 0;
};
// Joins the adapters Windows lists with what the counters say. An adapter with no engine instance is idle
// (0 %) when the counters work at all (`countersOk`), and unknown (-1) when they do not.
inline void GpuJoin(std::vector<GpuAdapterView>& adapters, const std::vector<GpuAdapterUtil>& utils, bool countersOk) {
    for (GpuAdapterView& a : adapters) {
        a.util = countersOk ? 0.0 : -1.0;
        if (countersOk) for (const GpuAdapterUtil& u : utils) if (u.luid == a.luid) a.util = u.util;
    }
}
// The adapter the single "GPU usage" figure follows: the busiest hardware adapter (ties: the first listed).
// -1 when no hardware adapter has a reading. Each adapter's own figure is always available beside it.
inline int GpuBusiest(const std::vector<GpuAdapterView>& v) {
    int best = -1;
    for (size_t i = 0; i < v.size(); ++i) if (!v[i].software && v[i].util >= 0 && (best < 0 || v[i].util > v[static_cast<size_t>(best)].util)) best = static_cast<int>(i);
    return best;
}
inline double GpuHeadline(const std::vector<GpuAdapterView>& v) { const int i = GpuBusiest(v); return i < 0 ? -1.0 : v[static_cast<size_t>(i)].util; }

}  // namespace ak
