// AnkhCore 2.5 — per-adapter network samples from Windows, for the engine thread only (no poller of its own).
// Read-only: GetIfTable2 (64-bit octets per interface), the routing table (which adapter carries the default
// route; a local lookup that sends nothing) and WLAN signal strength. The accounting itself is net_counters.h.
// PRIVACY: nothing here may ask Windows for the Wi-Fi network's identity. Since Windows 11 24H2 the calls that
// return a BSSID (WlanQueryInterface with wlan_intf_opcode_current_connection, WlanGetAvailableNetworkList,
// WlanGetNetworkBssList, WlanScan) are location access: each call is recorded under Privacy > Location and
// shows the location icon. Signal strength comes from the RSSI query, which names no network and needs no
// consent. tools/check_privacy.py fails the build gate if a location-gated interface comes back.
#pragma once
#include "base.h"
#include "net_counters.h"
#include <ws2ipdef.h>   // iphlpapi.h declares the interface and route tables only when this came first
#include <iphlpapi.h>
#include <ctime>
#pragma comment(lib, "iphlpapi.lib")

namespace ak {

inline std::wstring NetCountersFile() { return DataDir() + L"\\net-counters.txt"; }

class NetProbe {
public:
    // Physical adapters only: a hardware interface is never added together with its filter drivers. Devices
    // Windows remembers but that are not plugged in (old USB tethering entries) are left out.
    // The whole interface table and the routes are read every fifth tick; in between only the known
    // adapters are re-read (GetIfEntry2), which costs a fraction of the full table.
    std::vector<IfSample> Sample() {
        const bool every5 = (tick_++ % 5) == 0;   // also the pace for signal quality, which changes slowly
        bool scan = every5 || rows_.empty();
        if (!scan) for (MIB_IF_ROW2& r : rows_) {
            MIB_IF_ROW2 n{}; n.InterfaceLuid = r.InterfaceLuid;
            if (GetIfEntry2(&n) != NO_ERROR || !Physical(n)) { scan = true; break; }   // an adapter left: look again
            r = n;
        }
        if (scan) Scan();
        std::vector<IfSample> out;
        for (const MIB_IF_ROW2& r : rows_) {
            IfSample s; s.guid = GuidText(r.InterfaceGuid); s.name = r.Alias; s.type = static_cast<int>(r.Type);
            s.rx = r.InOctets; s.tx = r.OutOctets; s.up = r.OperStatus == IfOperStatusUp;
            s.linkBps = s.up ? std::max(r.ReceiveLinkSpeed, r.TransmitLinkSpeed) : 0;
            if (s.linkBps == ~0ull) s.linkBps = 0;   // "unknown"
            s.isDefault = s.up && def_ && r.InterfaceIndex == def_;
            if (r.Type == IF_TYPE_IEEE80211) { if (every5) signal_[s.guid] = s.up ? WlanSignal(r.InterfaceGuid) : -1; auto it = signal_.find(s.guid); s.signal = it == signal_.end() || !s.up ? -1 : it->second; }
            out.push_back(std::move(s));
        }
        return out;
    }
    ~NetProbe() { if (wlanH_ && wlanClose_) wlanClose_(wlanH_, nullptr); if (wlanDll_) FreeLibrary(wlanDll_); }

private:
    static bool Physical(const MIB_IF_ROW2& r) {
        return r.InterfaceAndOperStatusFlags.HardwareInterface && !r.InterfaceAndOperStatusFlags.FilterInterface && r.Type != IF_TYPE_SOFTWARE_LOOPBACK && r.OperStatus != IfOperStatusNotPresent;
    }
    void Scan() {
        rows_.clear(); def_ = 0;
        MIB_IF_TABLE2* t = nullptr;
        if (GetIfTable2(&t) != NO_ERROR || !t) return;
        for (ULONG i = 0; i < t->NumEntries; ++i) if (Physical(t->Table[i])) rows_.push_back(t->Table[i]);
        FreeMibTable(t);
        def_ = DefaultRouteIndex();
    }
    static std::wstring GuidText(const GUID& g) { wchar_t b[48] = L""; StringFromGUID2(g, b, 48); return b; }
    // The adapter Windows would use to reach the Internet. When that is a virtual interface (a VPN), the
    // physical adapter with the best default route is used instead: it is the one carrying the traffic.
    NET_IFINDEX DefaultRouteIndex() const {
        auto physical = [&](NET_IFINDEX idx) { for (const MIB_IF_ROW2& r : rows_) if (r.InterfaceIndex == idx) return r.OperStatus == IfOperStatusUp; return false; };
        sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_addr.S_un.S_addr = 0x08080808;   // any public address: only the route is looked up
        DWORD best = 0;
        if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&dst), &best) == NO_ERROR && physical(best)) return best;
        NET_IFINDEX pick = 0; ULONG pickMetric = ~0ul;
        MIB_IPFORWARD_TABLE2* rt = nullptr;
        if (GetIpForwardTable2(AF_UNSPEC, &rt) == NO_ERROR && rt) {
            for (ULONG i = 0; i < rt->NumEntries; ++i) {
                const MIB_IPFORWARD_ROW2& r = rt->Table[i];
                if (r.DestinationPrefix.PrefixLength != 0 || !physical(r.InterfaceIndex)) continue;
                if (r.Metric < pickMetric) { pickMetric = r.Metric; pick = r.InterfaceIndex; }
            }
            FreeMibTable(rt);
        }
        return pick;
    }
    // wlanapi.dll is loaded on demand: it is absent on systems without the WLAN feature.
    int WlanSignal(const GUID& g) {
        if (!wlanTried_) {
            wlanTried_ = true;
            wlanDll_ = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (wlanDll_) {
                const auto open = reinterpret_cast<DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE)>(GetProcAddress(wlanDll_, "WlanOpenHandle"));
                wlanClose_ = reinterpret_cast<DWORD(WINAPI*)(HANDLE, PVOID)>(GetProcAddress(wlanDll_, "WlanCloseHandle"));
                wlanQuery_ = reinterpret_cast<DWORD(WINAPI*)(HANDLE, const GUID*, int, PVOID, PDWORD, PVOID*, int*)>(GetProcAddress(wlanDll_, "WlanQueryInterface"));
                wlanFree_ = reinterpret_cast<void(WINAPI*)(PVOID)>(GetProcAddress(wlanDll_, "WlanFreeMemory"));
                DWORD ver = 0;
                if (!open || !wlanClose_ || !wlanQuery_ || !wlanFree_ || open(2, nullptr, &ver, &wlanH_) != ERROR_SUCCESS) wlanH_ = nullptr;
            }
        }
        if (!wlanH_) return -1;
        // The received signal strength in dBm (a LONG). Unlike the current-connection query this returns no
        // SSID or BSSID, so Windows does not treat it as location access (verified with Location turned off).
        DWORD size = 0; PVOID data = nullptr; int kind = 0;
        if (wlanQuery_(wlanH_, &g, kWlanOpcodeRssi, nullptr, &size, &data, &kind) != ERROR_SUCCESS || !data) return -1;
        int q = -1;
        if (size >= sizeof(LONG)) q = WlanQualityFromRssi(*static_cast<const LONG*>(data));
        wlanFree_(data);
        return q;
    }
    unsigned tick_ = 0; std::map<std::wstring, int> signal_;
    std::vector<MIB_IF_ROW2> rows_; NET_IFINDEX def_ = 0;   // the physical adapters and the default-route one, as of the last full read
    bool wlanTried_ = false; HMODULE wlanDll_ = nullptr; HANDLE wlanH_ = nullptr;
    DWORD(WINAPI* wlanClose_)(HANDLE, PVOID) = nullptr;
    DWORD(WINAPI* wlanQuery_)(HANDLE, const GUID*, int, PVOID, PDWORD, PVOID*, int*) = nullptr;
    void(WINAPI* wlanFree_)(PVOID) = nullptr;
};

// The engine's network accounting: the probe, the counters and their file. Used on the engine thread only.
class NetAccounting {
public:
    // A damaged file is set aside (never half-trusted) and counting restarts from zero.
    void Load() {
        const std::wstring f = NetCountersFile();
        if (FileExists(f)) {
            if (!counters_.Load(ReadText(f))) {
                counters_ = NetCounters();
                MoveFileExW(f.c_str(), (f + L".damaged").c_str(), MOVEFILE_REPLACE_EXISTING);
                LogW(L"Network totals: the saved file was damaged and has been set aside; counting restarts from zero.");
            }
        }
        // This boot's id: the boot time in seconds. It is computed from two clocks, so a saved id within three
        // minutes is the same boot; anything else is a new boot and nothing is carried over.
        const uint64_t now = static_cast<uint64_t>(time(nullptr)), bootSec = now - GetTickCount64() / 1000, saved = counters_.BootId();
        bootId_ = saved && (saved > bootSec ? saved - bootSec : bootSec - saved) <= 180 ? saved : bootSec;
    }
    std::vector<NetAdapterView> Tick() {
        SYSTEMTIME st; GetLocalTime(&st);
        counters_.Tick(probe_.Sample(), bootId_, st.wYear * 10000 + st.wMonth * 100 + st.wDay, static_cast<uint64_t>(time(nullptr)), GetTickCount64());
        if (++sinceSave_ >= 60) Save();
        return counters_.Views();
    }
    bool Save() {
        sinceSave_ = 0;
        const std::string text = counters_.Save(static_cast<uint64_t>(time(nullptr)));
        return WriteAtomic(NetCountersFile(), reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }
private:
    NetProbe probe_; NetCounters counters_; uint64_t bootId_ = 0; int sinceSave_ = 0;
};

}  // namespace ak
