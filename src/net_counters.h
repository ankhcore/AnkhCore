// AnkhCore 2.5 — cumulative network traffic accounting (pure, unit-tested). See docs/design/SIDEBAR_ADVANCED.md.
// One record per adapter GUID. "All adapters" is only ever computed at display time by adding records, so no
// total is stored twice. Every doubtful sample adds nothing: an undercount is possible, an overcount is not.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ak {

struct IfSample {   // type: IF_TYPE_* (6 Ethernet, 71 Wi-Fi)
    std::wstring guid, name; int type = 0; uint64_t rx = 0, tx = 0, linkBps = 0;
    bool up = true, isDefault = false; int signal = -1;   // link up; carries the default route; Wi-Fi signal % (-1 = none)
};
// Wi-Fi signal strength in dBm to the 0-100 % Windows itself shows: -100 dBm and below is 0 %, -50 dBm and
// above is 100 %, linear in between. A value that cannot be a received power (0 or positive) is "no reading".
constexpr int kWlanOpcodeRssi = 0x10000102;   // wlan_intf_opcode_rssi: names no network, so it is not location access
inline int WlanQualityFromRssi(long dbm) { return dbm >= 0 ? -1 : dbm <= -100 ? 0 : dbm >= -50 ? 100 : static_cast<int>(2 * (dbm + 100)); }
struct NetTotals { uint64_t rx = 0, tx = 0; void Add(uint64_t r, uint64_t t) { rx += r; tx += t; } };
enum class NetPeriod { Session, Today, Boot, Lifetime };
enum NetMode { NetAuto = 0, NetAll = 1, NetManual = 2 };

// What the UI gets for one adapter (also what travels in the engine frame). Rates are bytes per second, -1 = unknown.
struct NetAdapterView {
    std::wstring guid, name; int type = 0;
    bool present = false, up = false, isDefault = false; int signal = -1; uint64_t linkBps = 0;
    double down = -1, upRate = -1;
    NetTotals totals[4];   // indexed by NetPeriod
};

struct NetAdapterRecord {
    std::wstring name; int type = 0;
    NetTotals session, today, boot, life;
    uint64_t lastRx = 0, lastTx = 0;
    bool baseline = false;     // lastRx/lastTx are valid in this run
    bool savedValid = false;   // lastRx/lastTx came from the file, same boot: the gap may be added once
    bool present = false;
    bool up = false, isDefault = false; int signal = -1; uint64_t linkBps = 0;
    double rateRx = -1, rateTx = -1;   // bytes per second from the last accepted delta; -1 = unknown this tick
};

class NetCounters {
public:
    static constexpr uint64_t kFallbackBps = 10000000000ull;   // link speed unknown: 10 Gb/s ceiling
    std::map<std::wstring, NetAdapterRecord> adapters;

    // bootId: identifies this boot. day: local date key, e.g. 20261109. nowSec: wall clock seconds.
    // monoMs (optional): a monotonic millisecond clock, used only to make the displayed rates precise.
    void Tick(const std::vector<IfSample>& samples, uint64_t bootId, int day, uint64_t nowSec, uint64_t monoMs = 0) {
        if (boot_ != bootId) {          // a different boot: since-boot totals and saved octets no longer apply
            for (auto& kv : adapters) { kv.second.boot = NetTotals{}; kv.second.savedValid = false; kv.second.baseline = false; }
            boot_ = bootId;
        }
        if (day_ != day) { if (day_ != 0) for (auto& kv : adapters) kv.second.today = NetTotals{}; day_ = day; }
        const uint64_t elapsed = now_ && nowSec > now_ ? nowSec - now_ : 1;
        const double rateSec = monoMs && mono_ && monoMs > mono_ ? (monoMs - mono_) / 1000.0 : static_cast<double>(elapsed);
        for (auto& kv : adapters) { NetAdapterRecord& a = kv.second; a.present = a.up = a.isDefault = false; a.signal = -1; a.rateRx = a.rateTx = -1; }
        for (const IfSample& s : samples) {
            NetAdapterRecord& a = adapters[s.guid];
            a.name = s.name; a.type = s.type; a.present = true; a.up = s.up; a.isDefault = s.isDefault; a.signal = s.signal; a.linkBps = s.linkBps;
            uint64_t dt = elapsed;
            if (a.savedValid && !a.baseline) dt = nowSec > saved_ ? nowSec - saved_ : 1;   // the gap since the last save
            if (a.baseline || a.savedValid) {
                if (s.rx >= a.lastRx && s.tx >= a.lastTx) {
                    const uint64_t dr = s.rx - a.lastRx, dx = s.tx - a.lastTx;
                    const uint64_t cap = (s.linkBps ? s.linkBps : kFallbackBps) / 8 * dt * 3 / 2;
                    if (dr <= cap && dx <= cap) {
                        a.today.Add(dr, dx); a.boot.Add(dr, dx); a.life.Add(dr, dx);
                        if (a.baseline) { a.session.Add(dr, dx); a.rateRx = dr / rateSec; a.rateTx = dx / rateSec; }
                    }
                }
                // a counter that went backwards or jumped: new baseline, nothing added
            }
            a.lastRx = s.rx; a.lastTx = s.tx; a.baseline = true; a.savedValid = false;
        }
        for (auto& kv : adapters) if (!kv.second.present) { kv.second.baseline = false; kv.second.savedValid = false; }   // back later = baseline only
        now_ = nowSec; mono_ = monoMs;
    }
    NetTotals Get(const std::wstring& guid, NetPeriod p) const { auto it = adapters.find(guid); return it == adapters.end() ? NetTotals{} : Of(it->second, p); }
    NetTotals All(NetPeriod p) const { NetTotals t; for (const auto& kv : adapters) { const NetTotals o = Of(kv.second, p); t.Add(o.rx, o.tx); } return t; }
    uint64_t BootId() const { return boot_; }
    std::vector<NetAdapterView> Views() const {
        std::vector<NetAdapterView> out;
        for (const auto& kv : adapters) {
            const NetAdapterRecord& a = kv.second; NetAdapterView v;
            v.guid = kv.first; v.name = a.name; v.type = a.type; v.present = a.present; v.up = a.up; v.isDefault = a.isDefault; v.signal = a.signal; v.linkBps = a.linkBps;
            v.down = a.rateRx; v.upRate = a.rateTx;
            v.totals[0] = a.session; v.totals[1] = a.today; v.totals[2] = a.boot; v.totals[3] = a.life;
            out.push_back(v);
        }
        return out;
    }

    // Format 2 adds the adapter name (hex UTF-16) so an absent adapter still has a name; format 1 files load.
    std::string Save(uint64_t nowSec) const {
        std::ostringstream o;
        o << "ankh-net 2\n" << "boot " << boot_ << "\nday " << day_ << "\nat " << nowSec << "\n";
        for (const auto& kv : adapters) {
            const NetAdapterRecord& a = kv.second;
            o << "a " << Narrow(kv.first) << ' ' << a.type << ' ' << a.today.rx << ' ' << a.today.tx << ' ' << a.boot.rx << ' ' << a.boot.tx << ' ' << a.life.rx << ' ' << a.life.tx
              << ' ' << (a.baseline ? 1 : 0) << ' ' << a.lastRx << ' ' << a.lastTx << ' ' << Hex(a.name) << "\n";
        }
        o << "end\n";
        return o.str();
    }
    // False = damaged or unknown: the caller sets the file aside and starts from zero.
    bool Load(const std::string& text) {
        std::istringstream in(text); std::string tag; int ver = 0;
        if (!(in >> tag >> ver) || tag != "ankh-net" || (ver != 1 && ver != 2)) return false;
        uint64_t boot = 0, at = 0; int day = 0; std::map<std::wstring, NetAdapterRecord> m; bool ended = false;
        while (in >> tag) {
            if (tag == "boot") { if (!(in >> boot)) return false; }
            else if (tag == "day") { if (!(in >> day)) return false; }
            else if (tag == "at") { if (!(in >> at)) return false; }
            else if (tag == "end") { ended = true; break; }
            else if (tag == "a") {
                std::string g; NetAdapterRecord a; int b = 0;
                if (!(in >> g >> a.type >> a.today.rx >> a.today.tx >> a.boot.rx >> a.boot.tx >> a.life.rx >> a.life.tx >> b >> a.lastRx >> a.lastTx)) return false;
                if (ver >= 2) { std::string hx; if (!(in >> hx) || !Unhex(hx, a.name)) return false; }
                a.savedValid = b != 0; m[std::wstring(g.begin(), g.end())] = a;
            } else return false;
        }
        if (!ended) return false;   // cut short
        adapters = std::move(m); boot_ = boot; day_ = day; saved_ = at; now_ = at; mono_ = 0;
        return true;
    }
private:
    static NetTotals Of(const NetAdapterRecord& a, NetPeriod p) { return p == NetPeriod::Session ? a.session : p == NetPeriod::Today ? a.today : p == NetPeriod::Boot ? a.boot : a.life; }
    static std::string Narrow(const std::wstring& s) { std::string o; for (wchar_t c : s) o += c < 128 && c > 32 ? static_cast<char>(c) : '_'; return o; }
    static std::string Hex(const std::wstring& s) {
        if (s.empty()) return "-";
        static const char* d = "0123456789abcdef"; std::string o;
        for (wchar_t c : s) { const unsigned v = static_cast<unsigned>(c) & 0xFFFF; o += d[v >> 12]; o += d[(v >> 8) & 15]; o += d[(v >> 4) & 15]; o += d[v & 15]; }
        return o;
    }
    static bool Unhex(const std::string& h, std::wstring& out) {
        out.clear(); if (h == "-") return true;
        if (h.size() % 4 || h.size() > 4 * 256) return false;
        for (size_t i = 0; i < h.size(); i += 4) {
            unsigned v = 0;
            for (size_t k = 0; k < 4; ++k) { const char c = h[i + k]; const int n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (n < 0) return false; v = v * 16 + static_cast<unsigned>(n); }
            out += static_cast<wchar_t>(v);
        }
        return true;
    }
    uint64_t boot_ = 0, now_ = 0, saved_ = 0, mono_ = 0; int day_ = 0;
};

// What a view shows for the chosen mode. Pure: the engine uses it for the history series, the UI for display.
struct NetSelection {
    bool valid = false;       // adapter data exists at all (false = no frame yet, or the engine is gone)
    bool connected = false;   // the selection has a live link right now
    int mode = NetAuto, count = 0, type = 0, signal = -1; uint64_t linkBps = 0;
    std::wstring guid, name;  // "All adapters" has no guid
    double down = -1, up = -1;
    NetTotals totals[4];
};
inline const wchar_t* NetTypeName(int ifType) { return ifType == 71 ? L"Wi-Fi" : ifType == 6 ? L"Ethernet" : ifType == 243 || ifType == 244 ? L"Cellular" : L"Network"; }
inline NetSelection NetSelect(const std::vector<NetAdapterView>& v, int mode, const std::wstring& manualGuid) {
    NetSelection s; s.mode = mode < NetAuto || mode > NetManual ? NetAuto : mode; s.valid = !v.empty();
    if (s.mode == NetAll) {
        s.name = L"All adapters";
        for (const auto& a : v) {
            for (int p = 0; p < 4; ++p) s.totals[p].Add(a.totals[p].rx, a.totals[p].tx);   // the history of an absent adapter still counts
            if (!a.present || !a.up) continue;
            ++s.count; s.connected = true;
            if (a.down >= 0) { s.down = (s.down < 0 ? 0 : s.down) + a.down; s.up = (s.up < 0 ? 0 : s.up) + a.upRate; }
        }
        return s;
    }
    const NetAdapterView* pick = nullptr;
    if (s.mode == NetManual) { s.guid = manualGuid; for (const auto& a : v) if (a.guid == manualGuid) pick = &a; }   // never substitutes another adapter
    else for (const auto& a : v) if (a.present && a.up && a.isDefault) { pick = &a; break; }
    if (!pick) { s.name = s.mode == NetManual ? L"Adapter not found" : L"No active connection"; return s; }
    s.guid = pick->guid; s.name = pick->name; s.type = pick->type; s.count = 1;
    for (int p = 0; p < 4; ++p) s.totals[p] = pick->totals[p];
    s.connected = pick->present && pick->up;
    if (s.connected) { s.signal = pick->signal; s.linkBps = pick->linkBps; s.down = pick->down; s.up = pick->upRate; }
    return s;
}

}  // namespace ak
