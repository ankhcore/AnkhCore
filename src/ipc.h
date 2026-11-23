// AnkhCore 2.5 — UI <-> engine protocol (pure, unit-tested). Messages are a 12-byte header plus a payload.
// Every read is bounds-checked: a malformed message fails cleanly and the connection is dropped by the caller.
#pragma once
#include "engine.h"

namespace ak {

constexpr uint32_t kIpcMagic = 0x31434B41, kIpcVersion = 4, kIpcMaxPayload = 1u << 20;
enum IpcType : uint32_t { IpcHello = 1, IpcFrame = 2, IpcHistory = 3, IpcCmdConfigure = 10, IpcCmdMonitor = 11, IpcCmdFan = 12, IpcCmdShutdown = 13, IpcCmdNet = 14 };

class IpcWriter {
public:
    std::vector<uint8_t> b;
    void U32(uint32_t v) { Raw(&v, 4); } void U64(uint64_t v) { Raw(&v, 8); } void D(double v) { Raw(&v, 8); } void B(bool v) { uint8_t x = v ? 1 : 0; Raw(&x, 1); }
    void S(const std::wstring& s) { U32(static_cast<uint32_t>(s.size())); if (!s.empty()) Raw(s.data(), s.size() * sizeof(wchar_t)); }
    void A(const std::string& s) { U32(static_cast<uint32_t>(s.size())); if (!s.empty()) Raw(s.data(), s.size()); }
private:
    void Raw(const void* p, size_t n) { const auto* c = static_cast<const uint8_t*>(p); b.insert(b.end(), c, c + n); }
};
class IpcReader {
public:
    IpcReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool Ok() const { return ok_; }
    bool Done() const { return ok_ && p_ == n_; }
    uint32_t U32() { uint32_t v = 0; Raw(&v, 4); return v; } uint64_t U64() { uint64_t v = 0; Raw(&v, 8); return v; } double D() { double v = 0; Raw(&v, 8); return v; } bool B() { uint8_t v = 0; Raw(&v, 1); return v != 0; }
    std::wstring S() { const uint32_t c = U32(); if (!ok_ || c > (n_ - p_) / sizeof(wchar_t)) { ok_ = false; return {}; } std::wstring s(c, L'\0'); if (c) Raw(&s[0], c * sizeof(wchar_t)); return s; }
    std::string A() { const uint32_t c = U32(); if (!ok_ || c > n_ - p_) { ok_ = false; return {}; } std::string s(c, '\0'); if (c) Raw(&s[0], c); return s; }
    uint32_t Count(uint32_t maxN) { const uint32_t c = U32(); if (c > maxN) ok_ = false; return ok_ ? c : 0; }
private:
    void Raw(void* o, size_t k) { if (!ok_ || k > n_ - p_) { ok_ = false; return; } memcpy(o, d_ + p_, k); p_ += k; }
    const uint8_t* d_; size_t n_, p_ = 0; bool ok_ = true;
};

inline std::vector<uint8_t> IpcPack(uint32_t type, const IpcWriter& w) {
    IpcWriter h; h.U32(kIpcMagic); h.U32(type); h.U32(static_cast<uint32_t>(w.b.size()));
    h.b.insert(h.b.end(), w.b.begin(), w.b.end()); return h.b;
}
inline bool IpcHeaderOk(const uint8_t* h, uint32_t& type, uint32_t& len) {
    IpcReader r(h, 12); const uint32_t m = r.U32(); type = r.U32(); len = r.U32();
    return r.Ok() && m == kIpcMagic && len <= kIpcMaxPayload;
}

inline void IpcPut(IpcWriter& w, const Usage& u) { w.D(u.cpu); w.D(u.ram); w.D(u.gpu); w.D(u.disk); w.U64(u.ramUsed); w.U64(u.ramTotal); w.U64(u.diskFree); w.U64(u.diskTotal); w.U64(u.uptimeMs); }
inline void IpcGet(IpcReader& r, Usage& u) { u.cpu = r.D(); u.ram = r.D(); u.gpu = r.D(); u.disk = r.D(); u.ramUsed = r.U64(); u.ramTotal = r.U64(); u.diskFree = r.U64(); u.diskTotal = r.U64(); u.uptimeMs = r.U64(); }
inline void IpcPut(IpcWriter& w, const Readings& x) {
    w.B(x.present); w.B(x.everSeen); w.D(x.ageSec); w.U32(x.stamp.wHour); w.U32(x.stamp.wMinute); w.U32(x.stamp.wSecond); w.S(x.time);
    for (double v : {x.cpuTemp, x.cpuLoad, x.cpuClock, x.cpuPower, x.gpuTemp, x.gpuHotspot, x.gpuLoad, x.gpuClock, x.gpuPower, x.gpuVramUsed, x.gpuVramLoad, x.ramLoad, x.diskTemp, x.diskLoad, x.netDown, x.netUp, x.fanRequested, x.fanReadback}) w.D(v);
    w.S(x.gpuName); w.U32(static_cast<uint32_t>(x.controls)); w.S(x.fanMode); w.S(x.fanReason); w.S(x.fanError); w.S(x.model); w.S(x.nbfcMatch); w.S(x.ecState);
    w.U32(static_cast<uint32_t>(x.fans.size()));
    for (const auto& f : x.fans) { w.S(f.name); w.D(f.rpm); w.B(f.controllable); w.D(f.percent); w.S(f.source); w.B(f.live); }
}
inline void IpcGet(IpcReader& r, Readings& x) {
    x.present = r.B(); x.everSeen = r.B(); x.ageSec = r.D(); x.stamp.wHour = static_cast<WORD>(r.U32()); x.stamp.wMinute = static_cast<WORD>(r.U32()); x.stamp.wSecond = static_cast<WORD>(r.U32()); x.time = r.S();
    for (double* v : {&x.cpuTemp, &x.cpuLoad, &x.cpuClock, &x.cpuPower, &x.gpuTemp, &x.gpuHotspot, &x.gpuLoad, &x.gpuClock, &x.gpuPower, &x.gpuVramUsed, &x.gpuVramLoad, &x.ramLoad, &x.diskTemp, &x.diskLoad, &x.netDown, &x.netUp, &x.fanRequested, &x.fanReadback}) *v = r.D();
    x.gpuName = r.S(); x.controls = static_cast<int>(r.U32()); x.fanMode = r.S(); x.fanReason = r.S(); x.fanError = r.S(); x.model = r.S(); x.nbfcMatch = r.S(); x.ecState = r.S();
    const uint32_t n = r.Count(64); x.fans.clear();
    for (uint32_t i = 0; i < n && r.Ok(); ++i) { FanReading f; f.name = r.S(); f.rpm = r.D(); f.controllable = r.B(); f.percent = r.D(); f.source = r.S(); f.live = r.B(); x.fans.push_back(f); }
}
struct IpcStatus { uint64_t polls = 0; bool hubRun = false; uint32_t pid = 0; };
inline std::vector<uint8_t> IpcEncodeFrame(const EngineFrame& f, const IpcStatus& st) {
    IpcWriter w; w.U64(f.seq); IpcPut(w, f.usage); IpcPut(w, f.read);
    w.U32(static_cast<uint32_t>(f.alerts.size()));
    for (const auto& a : f.alerts) { w.A(a.id); w.D(a.value); w.U64(a.atSec); }
    w.B(f.sample.fresh); for (double v : f.sample.v) w.D(v); w.U64(f.sampleSec);
    w.U64(st.polls); w.B(st.hubRun); w.U32(st.pid);
    w.U32(static_cast<uint32_t>(f.net.size()));
    for (const auto& a : f.net) {
        w.S(a.guid); w.S(a.name); w.U32(static_cast<uint32_t>(a.type)); w.B(a.present); w.B(a.up); w.B(a.isDefault); w.D(a.signal); w.U64(a.linkBps); w.D(a.down); w.D(a.upRate);
        for (const NetTotals& t : a.totals) { w.U64(t.rx); w.U64(t.tx); }
    }
    const SysExtra& x = f.sys;
    w.U32(static_cast<uint32_t>(x.cores.size())); for (double v : x.cores) w.D(v);
    w.U32(static_cast<uint32_t>(x.physCores)); w.U32(static_cast<uint32_t>(x.logical)); w.U64(x.memAvail); w.U64(x.memCached); w.U64(x.commitUsed); w.U64(x.commitLimit); w.D(x.diskRead); w.D(x.diskWrite);
    w.U32(static_cast<uint32_t>(x.drives.size()));
    for (const auto& d : x.drives) { w.U32(d.letter); w.U64(d.total); w.U64(d.free); w.D(d.readBps); w.D(d.writeBps); }
    w.U32(static_cast<uint32_t>(f.gpus.size()));
    for (const auto& g : f.gpus) { w.S(g.name); w.U32(g.luid.hi); w.U32(g.luid.lo); w.B(g.software); w.D(g.util); w.U64(g.dedicatedUsed); w.U64(g.sharedUsed); w.U64(g.dedicatedTotal); }
    return w.b;
}
inline bool IpcDecodeFrame(const uint8_t* d, size_t n, EngineFrame& f, IpcStatus& st) {
    IpcReader r(d, n); f.seq = r.U64(); IpcGet(r, f.usage); IpcGet(r, f.read);
    const uint32_t c = r.Count(64); f.alerts.clear();
    for (uint32_t i = 0; i < c && r.Ok(); ++i) { AlertEvent a; a.id = r.A(); a.value = r.D(); a.atSec = r.U64(); f.alerts.push_back(a); }
    f.sample.fresh = r.B(); for (double& v : f.sample.v) v = r.D(); f.sampleSec = r.U64();
    st.polls = r.U64(); st.hubRun = r.B(); st.pid = r.U32();
    const uint32_t na = r.Count(64); f.net.clear();
    for (uint32_t i = 0; i < na && r.Ok(); ++i) {
        NetAdapterView a; a.guid = r.S(); a.name = r.S(); a.type = static_cast<int>(r.U32()); a.present = r.B(); a.up = r.B(); a.isDefault = r.B(); a.signal = static_cast<int>(r.D()); a.linkBps = r.U64(); a.down = r.D(); a.upRate = r.D();
        for (NetTotals& t : a.totals) { t.rx = r.U64(); t.tx = r.U64(); }
        f.net.push_back(a);
    }
    SysExtra& x = f.sys; x = SysExtra();
    const uint32_t nc = r.Count(256); for (uint32_t i = 0; i < nc && r.Ok(); ++i) x.cores.push_back(r.D());
    x.physCores = static_cast<int>(r.U32()); x.logical = static_cast<int>(r.U32()); x.memAvail = r.U64(); x.memCached = r.U64(); x.commitUsed = r.U64(); x.commitLimit = r.U64(); x.diskRead = r.D(); x.diskWrite = r.D();
    const uint32_t nd = r.Count(26);
    for (uint32_t i = 0; i < nd && r.Ok(); ++i) { DriveInfo d; d.letter = static_cast<wchar_t>(r.U32()); d.total = r.U64(); d.free = r.U64(); d.readBps = r.D(); d.writeBps = r.D(); x.drives.push_back(d); }
    const uint32_t ng = r.Count(16); f.gpus.clear();
    for (uint32_t i = 0; i < ng && r.Ok(); ++i) { GpuAdapterView g; g.name = r.S(); g.luid.hi = r.U32(); g.luid.lo = r.U32(); g.software = r.B(); g.util = r.D(); g.dedicatedUsed = r.U64(); g.sharedUsed = r.U64(); g.dedicatedTotal = r.U64(); f.gpus.push_back(g); }
    return r.Done();
}
// History: replayed into the client's own MonitorStore, one interval per point.
inline std::vector<uint8_t> IpcEncodeHistory(const MonitorStore& s) {
    IpcWriter w; w.U32(static_cast<uint32_t>(s.Interval())); w.U32(static_cast<uint32_t>(s.History()));
    const size_t n = s.Get(static_cast<Series>(0)).Size(); w.U32(static_cast<uint32_t>(n));
    for (int k = 0; k < SeriesCount; ++k) { const Ring& g = s.Get(static_cast<Series>(k)); for (size_t i = 0; i < n; ++i) w.D(i < g.Size() ? g.At(i) : kNoValue); }
    return w.b;
}
inline bool IpcDecodeHistory(const uint8_t* d, size_t n, MonitorStore& out) {
    IpcReader r(d, n); const uint32_t iv = r.U32(), hs = r.U32(), c = r.Count(MonitorStore::kMaxPoints);
    if (!r.Ok()) return false;
    std::vector<double> v(static_cast<size_t>(c) * SeriesCount);
    for (double& x : v) x = r.D();
    if (!r.Done() || iv == 0) return false;
    MonitorStore m; m.Configure(static_cast<int>(iv), static_cast<int>(hs));
    for (uint32_t i = 0; i < c; ++i) { MonitorSample ms; ms.fresh = true; for (int k = 0; k < SeriesCount; ++k) ms.v[k] = v[static_cast<size_t>(i) + static_cast<size_t>(k) * c]; m.Tick(1000ull + static_cast<uint64_t>(i) * iv, ms); }
    out = m; return true;
}
inline std::vector<uint8_t> IpcEncodeConfigure(int iv, int hs, const std::vector<AlertRule>& rules) {
    IpcWriter w; w.U32(static_cast<uint32_t>(iv)); w.U32(static_cast<uint32_t>(hs)); w.U32(static_cast<uint32_t>(rules.size()));
    for (const auto& a : rules) { w.A(a.id); w.B(a.enabled); w.D(a.threshold); w.U32(static_cast<uint32_t>(a.holdSec)); w.D(a.hysteresis); w.U32(static_cast<uint32_t>(a.cooldownSec)); }
    return w.b;
}
inline bool IpcDecodeConfigure(const uint8_t* d, size_t n, int& iv, int& hs, std::vector<AlertRule>& rules) {
    IpcReader r(d, n); iv = static_cast<int>(r.U32()); hs = static_cast<int>(r.U32()); const uint32_t c = r.Count(32); rules.clear();
    for (uint32_t i = 0; i < c && r.Ok(); ++i) { AlertRule a; a.id = r.A(); a.enabled = r.B(); a.threshold = r.D(); a.holdSec = static_cast<int>(r.U32()); a.hysteresis = r.D(); a.cooldownSec = static_cast<int>(r.U32()); rules.push_back(a); }
    return r.Done();
}

}  // namespace ak
