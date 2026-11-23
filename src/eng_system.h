// AnkhCore — system engines: Windhawk mods, services, tweaks, startup apps,
// cleanup, tools, repair, monitoring, health checks, restore points and the
// install helpers (scheduled task, shortcuts, uninstall entry).
#pragma once
#include "base.h"
#include "install.h"   // RemoveReport
#include <atomic>
#include <functional>
#include <mutex>
#include <pdh.h>
#include <pdhmsg.h>
#include <thread>
#include <wbemidl.h>
#include <winioctl.h>

namespace ak {

// ---------------------------------------------------------------- WMI (small helper)
using WmiRow = std::map<std::wstring, std::wstring>;
inline std::vector<WmiRow> WmiQuery(const wchar_t* ns, const wchar_t* query, const std::vector<std::wstring>& props) {
    std::vector<WmiRow> rows;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    static bool sec = (CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                                            nullptr, EOAC_NONE, nullptr), true);
    (void)sec;
    IWbemLocator* loc = nullptr; IWbemServices* svc = nullptr; IEnumWbemClassObject* en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&loc)))) return rows;
    BSTR bns = SysAllocString(ns);
    if (SUCCEEDED(loc->ConnectServer(bns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
        CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
        if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en))) {
            IWbemClassObject* obj = nullptr; ULONG n = 0;
            while (en->Next(5000, 1, &obj, &n) == S_OK && n) {
                WmiRow r;
                for (const auto& p : props) {
                    VARIANT v; VariantInit(&v);
                    if (SUCCEEDED(obj->Get(p.c_str(), 0, &v, nullptr, nullptr))) {
                        if (v.vt == VT_BSTR && v.bstrVal) r[p] = v.bstrVal;
                        else if (v.vt != VT_NULL && v.vt != VT_EMPTY && SUCCEEDED(VariantChangeType(&v, &v, 0, VT_BSTR))) r[p] = v.bstrVal;
                    }
                    VariantClear(&v);
                }
                rows.push_back(r);
                obj->Release();
            }
            en->Release();
        }
        SysFreeString(lang); SysFreeString(q);
        svc->Release();
    }
    SysFreeString(bns);
    loc->Release();
    return rows;
}

// ---------------------------------------------------------------- Windhawk
inline const wchar_t* WhModsKey() { return L"SOFTWARE\\Windhawk\\Engine\\Mods"; }
inline std::wstring WindhawkExePath() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\Windhawk\\windhawk.exe"; }
inline std::wstring WindhawkData() { return KnownFolder(FOLDERID_ProgramData) + L"\\Windhawk"; }
inline DWORD ServiceState(const wchar_t* name) {
    DWORD st = 0;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE s = OpenServiceW(scm, name, SERVICE_QUERY_STATUS)) { SERVICE_STATUS ss{}; if (QueryServiceStatus(s, &ss)) st = ss.dwCurrentState; CloseServiceHandle(s); }
        CloseServiceHandle(scm);
    }
    return st;
}
inline bool ServiceControlWait(const wchar_t* name, bool start) {
    bool ok = false;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE s = OpenServiceW(scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS)) {
            SERVICE_STATUS ss{};
            if (start) ok = StartServiceW(s, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            else ok = ControlService(s, SERVICE_CONTROL_STOP, &ss) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
            const DWORD want = start ? SERVICE_RUNNING : SERVICE_STOPPED;
            for (int i = 0; i < 80 && QueryServiceStatus(s, &ss) && ss.dwCurrentState != want; ++i) Sleep(125);
            CloseServiceHandle(s);
        }
        CloseServiceHandle(scm);
    }
    return ok;
}
inline bool WindhawkInstalled() { return FileExists(WindhawkExePath()); }
inline bool WindhawkRunning() { return ServiceState(L"Windhawk") == SERVICE_RUNNING; }
inline void StopWindhawk() { ServiceControlWait(L"Windhawk", false); for (int i = 0; i < 3; ++i) KillProcess(L"windhawk.exe", 400); }
inline void StartWindhawk() { ServiceControlWait(L"Windhawk", true); }
inline bool RestartWindhawk() {
    LogI(L"Restarting Windhawk...");
    ServiceControlWait(L"Windhawk", false);
    const bool ok = ServiceControlWait(L"Windhawk", true);
    if (ok) LogOk(L"Windhawk restarted."); else LogE(L"Windhawk could not be restarted.");
    return ok;
}
struct WhMod { std::wstring id, name, version, author; bool enabled = true; };
inline std::wstring ModMeta(const std::string& src, const char* tag) {
    const std::string key = std::string("@") + tag;
    const size_t p = src.find(key + " ");
    if (p == std::string::npos) return {};
    size_t a = p + key.size(); while (a < src.size() && src[a] == ' ') ++a;
    size_t e = src.find_first_of("\r\n", a);
    return Utf8ToW(src.substr(a, e == std::string::npos ? std::string::npos : e - a));
}
inline std::vector<WhMod> WindhawkMods() {
    std::vector<WhMod> v;
    for (const auto& id : RegSubkeys(HKEY_LOCAL_MACHINE, WhModsKey())) {
        WhMod m; m.id = id;
        const std::wstring k = std::wstring(WhModsKey()) + L"\\" + id;
        m.version = RegStr(HKEY_LOCAL_MACHINE, k, L"Version");
        m.enabled = RegDword(HKEY_LOCAL_MACHINE, k, L"Disabled", 0) == 0;
        const std::string src = ReadText(WindhawkData() + L"\\ModsSource\\" + id + L".wh.cpp").substr(0, 8192);
        m.name = ModMeta(src, "name"); m.author = ModMeta(src, "author");
        if (m.version.empty()) m.version = ModMeta(src, "version");
        if (m.name.empty()) m.name = id;
        v.push_back(m);
    }
    std::sort(v.begin(), v.end(), [](const WhMod& a, const WhMod& b) { return a.id.find(L"anubis") != std::wstring::npos ? true :
                                                                       b.id.find(L"anubis") != std::wstring::npos ? false : a.name < b.name; });
    return v;
}
inline bool SetModEnabled(const std::wstring& id, bool on, const std::wstring& name) {
    const std::wstring k = std::wstring(WhModsKey()) + L"\\" + id;
    if (!RegKeyExists(HKEY_LOCAL_MACHINE, k)) { LogE(L"Mod not found: " + id); return false; }
    // SEC-3: the setting is read back, and a Windhawk that could not be restarted is a failure (the mod keeps its old state until it restarts).
    const bool wrote = JRegDword(L"windhawk", L"Windhawk mod " + std::wstring(on ? L"enabled: " : L"disabled: ") + name, HKEY_LOCAL_MACHINE, k, L"Disabled", on ? 0 : 1);
    if (!wrote || RegDword(HKEY_LOCAL_MACHINE, k, L"Disabled", on ? 1 : 0) != static_cast<DWORD>(on ? 0 : 1)) { LogE(name + L": the mod setting could not be changed."); return false; }
    if (WindhawkRunning() && !RestartWindhawk()) { LogE(name + (on ? L" is set to enabled" : L" is set to disabled") + L", but Windhawk did not restart: the change takes effect when Windhawk next starts."); return false; }
    LogOk(name + (on ? L" enabled." : L" disabled."));
    return true;
}

// ---------------------------------------------------------------- services
struct SvcDef { const wchar_t* name; const wchar_t* title; const wchar_t* when; DWORD offStart; bool recommended; };
inline const std::vector<SvcDef>& Services() {
    static const std::vector<SvcDef> v = {
        {L"DiagTrack", L"Telemetry (Connected User Experiences)", L"Sends diagnostic data to Microsoft.", SERVICE_DISABLED, true},
        {L"dmwappushservice", L"Device Management WAP Push", L"Telemetry routing; not needed at home.", SERVICE_DISABLED, true},
        {L"Fax", L"Fax", L"Only if you never send or receive faxes.", SERVICE_DISABLED, true},
        {L"RetailDemo", L"Retail Demo", L"Shop display mode; never needed at home.", SERVICE_DISABLED, true},
        {L"RemoteRegistry", L"Remote Registry", L"Lets other PCs edit this registry. Safer off.", SERVICE_DISABLED, true},
        {L"WMPNetworkSvc", L"Media Player Network Sharing", L"Only if you don't share media on your network.", SERVICE_DISABLED, true},
        {L"MapsBroker", L"Downloaded Maps Manager", L"Starts on demand when set to Manual.", SERVICE_DEMAND_START, true},
        {L"CscService", L"Offline Files", L"Only for offline copies of network shares.", SERVICE_DISABLED, true},
        {L"WorkFolderssvc", L"Work Folders", L"Company file sync; not used at home.", SERVICE_DISABLED, true},
        {L"wisvc", L"Windows Insider", L"Only if you are not in the Insider Program.", SERVICE_DISABLED, true},
        {L"XblAuthManager", L"Xbox Live Auth Manager", L"Off only if you don't play Xbox / Game Pass games.", SERVICE_DISABLED, false},
        {L"XblGameSave", L"Xbox Live Game Save", L"Off only if you don't play Xbox / Game Pass games.", SERVICE_DISABLED, false},
        {L"XboxNetApiSvc", L"Xbox Live Networking", L"Off only if you don't play Xbox / Game Pass games.", SERVICE_DISABLED, false},
        {L"XboxGipSvc", L"Xbox Accessory Management", L"Off only if you don't use Xbox controllers.", SERVICE_DISABLED, false},
        {L"Spooler", L"Print Spooler", L"Off only with no printer. Microsoft Print to PDF stops too.", SERVICE_DISABLED, false},
        {L"SCardSvr", L"Smart Card", L"Off only if you don't use smart cards.", SERVICE_DISABLED, false},
        {L"ScDeviceEnum", L"Smart Card Device Enumeration", L"Off only if you don't use smart cards.", SERVICE_DISABLED, false},
        {L"SCPolicySvc", L"Smart Card Removal Policy", L"Off only if you don't use smart cards.", SERVICE_DISABLED, false},
        {L"TermService", L"Remote Desktop Services", L"Manual: starts only when Remote Desktop is used.", SERVICE_DEMAND_START, false},
        {L"PhoneSvc", L"Phone Service", L"Manual: starts only for calling apps.", SERVICE_DEMAND_START, false},
        {L"WerSvc", L"Windows Error Reporting", L"Manual: crash reports only when needed.", SERVICE_DEMAND_START, false},
        {L"lfsvc", L"Geolocation", L"Off if no app needs your location.", SERVICE_DISABLED, false},
    };
    return v;
}
struct SvcInfo { bool exists = false; DWORD start = 0; bool delayed = false; };
inline SvcInfo GetSvc(const wchar_t* name) {
    SvcInfo i;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE s = OpenServiceW(scm, name, SERVICE_QUERY_CONFIG)) {
            i.exists = true;
            DWORD need = 0; QueryServiceConfigW(s, nullptr, 0, &need);
            std::vector<uint8_t> b(need + 16);
            if (QueryServiceConfigW(s, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(b.data()), static_cast<DWORD>(b.size()), &need))
                i.start = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(b.data())->dwStartType;
            SERVICE_DELAYED_AUTO_START_INFO d{}; DWORD n = 0;
            if (QueryServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<BYTE*>(&d), sizeof(d), &n)) i.delayed = d.fDelayedAutostart;
            CloseServiceHandle(s);
        }
        CloseServiceHandle(scm);
    }
    return i;
}
inline bool SetSvcStart(const wchar_t* name, DWORD start, bool delayed) {
    bool ok = false;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE s = OpenServiceW(scm, name, SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_START)) {
            ok = ChangeServiceConfigW(s, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            if (ok && start == SERVICE_AUTO_START) { SERVICE_DELAYED_AUTO_START_INFO d{delayed}; ChangeServiceConfig2W(s, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &d); }
            SERVICE_STATUS ss{};
            if (ok && start == SERVICE_DISABLED) ControlService(s, SERVICE_CONTROL_STOP, &ss);
            if (ok && start == SERVICE_AUTO_START) StartServiceW(s, 0, nullptr);
            CloseServiceHandle(s);
        }
        CloseServiceHandle(scm);
    }
    return ok;
}
// "On" = the service runs as Windows set it; "Off" = AnkhCore's lighter setting.
inline bool SvcIsOn(const SvcDef& d) { const SvcInfo i = GetSvc(d.name); return i.exists && i.start != d.offStart && i.start != SERVICE_DISABLED; }
inline bool SetSvcOn(const SvcDef& d, bool on) {
    const SvcInfo i = GetSvc(d.name);
    if (!i.exists) { LogW(std::wstring(d.title) + L": not on this PC."); return false; }
    if (!on) {
        JournalAdd(std::wstring(L"svc:") + d.name, L"SVC", std::wstring(L"Service off: ") + d.title, {d.name, std::to_wstring(i.start), i.delayed ? L"1" : L"0"});
        const bool ok = SetSvcStart(d.name, d.offStart, false) && GetSvc(d.name).start == d.offStart;   // read back (SEC-3)
        if (ok) LogOk(std::wstring(d.title) + (d.offStart == SERVICE_DISABLED ? L" disabled." : L" set to Manual.")); else LogE(std::wstring(L"Could not change ") + d.title);
        return ok;
    }
    const UndoCount u = JournalUndoEx([&](const JEntry& e) { return e.group == std::wstring(L"svc:") + d.name; });
    if (u.Nothing()) {   // not switched off by AnkhCore: its Windows setting is not recorded, Manual is the closest safe one
        const bool ok = SetSvcStart(d.name, SERVICE_DEMAND_START, false) && GetSvc(d.name).start == SERVICE_DEMAND_START;
        if (ok) LogOk(std::wstring(d.title) + L" set to Manual (its earlier Windows setting was not recorded by AnkhCore)."); else LogE(std::wstring(L"Could not change ") + d.title);
        return ok;
    }
    if (!u.Success()) { LogE(std::wstring(d.title) + L": its Windows setting could not be put back."); return false; }
    LogOk(std::wstring(d.title) + L" back to its Windows setting.");
    return true;
}

// ---------------------------------------------------------------- tweaks
struct RegSpec { HKEY root; const wchar_t* key; const wchar_t* name; DWORD type; DWORD dw; const wchar_t* sz; };
struct Tweak {
    std::wstring id, title, desc, category = L"Performance"; bool restart = false; bool laptopWarn = false; bool confirm = false;
    std::function<bool()> isOn; std::function<bool()> on; std::function<bool()> off;
};
inline bool RegSpecMatches(const RegSpec& s) {
    const RegValue v = RegGet(s.root, s.key, s.name);
    if (!v.exists) return false;
    if (s.type == REG_DWORD) return v.data.size() >= 4 && *reinterpret_cast<const DWORD*>(v.data.data()) == s.dw;
    return _wcsicmp(RegStr(s.root, s.key, s.name).c_str(), s.sz) == 0;
}
inline Tweak RegTweak(const std::wstring& id, const std::wstring& title, const std::wstring& desc, std::vector<RegSpec> specs, bool restart = false, const std::wstring& category = L"Performance", bool confirm = false) {
    Tweak t; t.id = id; t.title = title; t.desc = desc; t.category = category; t.restart = restart; t.confirm = confirm;
    t.isOn = [specs] { for (const auto& s : specs) if (!RegSpecMatches(s)) return false; return true; };
    // SEC-3: every value must be written and read back; one that is refused makes the tweak fail (what was
    // written stays in the journal, so Undo puts it back).
    t.on = [specs, id, title] {
        OpTally tally;
        JournalOpScope jscope(L"Tweak: " + title);   // FT-RH: all values of one tweak are one operation in the journal
        for (const auto& s : specs) {
            const bool wrote = s.type == REG_DWORD ? JRegDword(L"tweak:" + id, L"Tweak: " + title, s.root, s.key, s.name, s.dw)
                                                   : JRegStr(L"tweak:" + id, L"Tweak: " + title, s.root, s.key, s.name, s.sz);
            tally.Add(wrote && RegSpecMatches(s));
        }
        if (!tally.Success()) LogE(title + L": " + tally.Text(L"setting", L"settings", L"written"));
        return tally.Success();
    };
    t.off = [id, title] {
        const UndoCount u = JournalUndoEx([&](const JEntry& e) { return e.group == L"tweak:" + id; });
        if (u.Nothing()) { LogW(title + L": AnkhCore did not apply this tweak, so it has no recorded values to put back. Nothing was changed."); return false; }
        if (!u.Success()) LogE(title + L": " + u.Text());
        return u.Success();
    };
    return t;
}
inline std::wstring ActivePowerScheme() {
    std::wstring out;
    RunCapture(SysDir() + L"\\powercfg.exe", L"/getactivescheme", [&](const std::wstring& l) {
        const size_t a = l.find(L':'); if (a != std::wstring::npos) { const size_t b = l.find(L'(', a); out = l.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1); }
    });
    out.erase(0, out.find_first_not_of(L' ')); while (!out.empty() && out.back() == L' ') out.pop_back();
    return Lower(out);
}
inline bool IsLaptop() { SYSTEM_POWER_STATUS ps{}; return GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255; }
inline const std::vector<Tweak>& Tweaks() {
    static std::vector<Tweak> v;
    if (!v.empty()) return v;
    const HKEY CU = HKEY_CURRENT_USER, LM = HKEY_LOCAL_MACHINE;
    {   // power plan
        Tweak t; t.id = L"power"; t.title = L"Ultimate Performance power plan";
        t.desc = L"Keeps the CPU at full speed. Uses more power."; t.laptopWarn = true;
        t.isOn = [] { const std::wstring s = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore", L"PowerScheme"); return !s.empty() && ActivePowerScheme() == s; };
        t.on = [] {
            const std::wstring prev = ActivePowerScheme();
            std::wstring guid;
            RunCapture(SysDir() + L"\\powercfg.exe", L"-duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61", [&](const std::wstring& l) {
                const size_t a = l.find(L':'); if (a != std::wstring::npos && guid.empty()) { std::wstring g = l.substr(a + 1); const size_t b = g.find(L'('); if (b != std::wstring::npos) g.resize(b); g.erase(0, g.find_first_not_of(L' ')); while (!g.empty() && g.back() == L' ') g.pop_back(); guid = Lower(g); }
            });
            if (guid.size() != 36) guid = L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";   // High performance
            if (Run(SysDir() + L"\\powercfg.exe", L"/setactive " + guid, true, 15000) != 0) { LogE(L"Power plan could not be set."); return false; }
            JournalAdd(L"tweak:power", L"POWER", L"Tweak: Ultimate Performance power plan", {prev});
            RegSetStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore", L"PowerScheme", guid);
            return true;
        };
        t.off = [] { const UndoCount u = JournalUndoEx([](const JEntry& e) { return e.group == L"tweak:power"; });
                     bool ok = u.Success();   // SEC-3: the plan must really have been switched back
                     if (u.Nothing()) ok = Run(SysDir() + L"\\powercfg.exe", L"/setactive 381b4222-f694-41f0-9685-ff5bb260df2e", true, 15000) == 0;   // Balanced
                     if (!ok) { LogE(L"The previous power plan could not be restored."); return false; }
                     return RegDel(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore", L"PowerScheme"); };
        v.push_back(t);
    }
    v.push_back(RegTweak(L"bgapps", L"Stop background apps", L"Store apps don't run in the background.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications", L"GlobalUserDisabled", REG_DWORD, 1, nullptr},
         {LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy", L"LetAppsRunInBackground", REG_DWORD, 2, nullptr}}));
    v.push_back(RegTweak(L"gamemode", L"Game Mode on", L"Windows gives games priority.",
        {{CU, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", REG_DWORD, 1, nullptr},
         {CU, L"Software\\Microsoft\\GameBar", L"AllowAutoGameMode", REG_DWORD, 1, nullptr}}, false, L"Gaming"));
    v.push_back(RegTweak(L"gamedvr", L"Game DVR / background recording off", L"Stops Xbox Game Bar from recording in the background.",
        {{CU, L"System\\GameConfigStore", L"GameDVR_Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", REG_DWORD, 0, nullptr},
         {LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", L"AllowGameDVR", REG_DWORD, 0, nullptr}}, false, L"Gaming"));
    v.push_back(RegTweak(L"hags", L"Hardware-accelerated GPU scheduling", L"Lets the graphics card manage its own memory. Needs a recent GPU driver.",
        {{LM, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", REG_DWORD, 2, nullptr}}, true, L"Gaming"));
    v.push_back(RegTweak(L"ads", L"Ads, tips and suggestions off", L"No suggested apps, tips or ads in Start, Settings and the lock screen.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338388Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-338389Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-353694Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SubscribedContent-353696Enabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SystemPaneSuggestionsEnabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SilentInstalledAppsEnabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", L"SoftLandingEnabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_IrisRecommendations", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowSyncProviderNotifications", REG_DWORD, 0, nullptr}}, false, L"Privacy"));
    v.push_back(RegTweak(L"bing", L"Web results in Start search off", L"Start search shows only your PC.",
        {{CU, L"Software\\Policies\\Microsoft\\Windows\\Explorer", L"DisableSearchBoxSuggestions", REG_DWORD, 1, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"BingSearchEnabled", REG_DWORD, 0, nullptr}}, false, L"Privacy"));
    v.push_back(RegTweak(L"telemetry", L"Telemetry to minimum", L"Sends the least diagnostic data your edition allows.",
        {{LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection", L"AllowTelemetry", REG_DWORD, 0, nullptr}}, false, L"Privacy"));
    v.push_back(RegTweak(L"menus", L"Faster menus", L"Menus open after 100 ms instead of 400 ms.",
        {{CU, L"Control Panel\\Desktop", L"MenuShowDelay", REG_SZ, 0, L"100"}}));
    v.push_back(RegTweak(L"startupdelay", L"No startup delay", L"Startup apps launch right away after sign-in.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Serialize", L"StartupDelayInMSec", REG_DWORD, 0, nullptr}}));
    {
        MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms);
        const DWORD kb = static_cast<DWORD>(std::min<uint64_t>(ms.ullTotalPhys / 1024, 0xFFFFFFFFull));
        v.push_back(RegTweak(L"svchost", L"Fewer service host processes", L"Groups Windows services by your RAM size.",
            {{LM, L"SYSTEM\\CurrentControlSet\\Control", L"SvcHostSplitThresholdInKB", REG_DWORD, kb, nullptr}}, true));
    }
    {
        Tweak t; t.id = L"hiber"; t.title = L"Hibernation off"; t.desc = L"Frees disk space (hiberfil.sys). Also turns off Fast Startup."; t.category = L"Power & Storage";
        t.isOn = [] { return RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", 1) == 0; };
        t.on = [] { JournalAdd(L"tweak:hiber", L"HIBER", L"Tweak: Hibernation off", {L"1"}); return Run(SysDir() + L"\\powercfg.exe", L"-h off", true, 30000) == 0; };
        t.off = [] {   // SEC-3
            const UndoCount u = JournalUndoEx([](const JEntry& e) { return e.group == L"tweak:hiber"; });
            if (u.Nothing()) { LogW(L"Hibernation off: AnkhCore did not turn hibernation off, so it has nothing recorded to put back. Nothing was changed."); return false; }
            if (!u.Success()) LogE(L"Hibernation could not be turned back on.");
            return u.Success(); };
        v.push_back(t);
    }
    // WT-01: Curated, reversible Windows 11 tweaks (MIT-adapted from WinUtil & Sophia Script)
    v.push_back(RegTweak(L"tailored_exp", L"Tailored diagnostic experiences & speech privacy off",
        L"Stops Windows from using diagnostic data for personalized tips and disables online speech recognition.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy", L"TailoredExperiencesWithDiagnosticDataEnabled", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Speech_OneCore\\Settings\\OnlineSpeechPrivacy", L"HasAccepted", REG_DWORD, 0, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"activity_hist", L"Activity history upload off",
        L"Stops Windows from publishing or uploading your activity timeline to Microsoft.",
        {{LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"PublishUserActivities", REG_DWORD, 0, nullptr},
         {LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System", L"UploadUserActivities", REG_DWORD, 0, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"consumer_feat", L"Automatic Store app suggestions off",
        L"Prevents Windows from silently installing promoted Microsoft Store apps.",
        {{LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent", L"DisableWindowsConsumerFeatures", REG_DWORD, 1, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"location_deny", L"System-wide location access off",
        L"Denies Windows and Store apps access to device location.",
        {{LM, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location", L"Value", REG_SZ, 0, L"Deny"}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"delivery_opt", L"Delivery Optimization P2P off",
        L"Stops Windows Update from uploading update files to other PCs on the Internet.",
        {{LM, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization", L"DODownloadMode", REG_DWORD, 0, nullptr}}, false, L"Performance", true));
    v.push_back(RegTweak(L"mouse_accel_off", L"Mouse acceleration off (1:1 pointer)",
        L"Disables Enhance Pointer Precision for consistent 1:1 mouse movement in games and desktop.",
        {{CU, L"Control Panel\\Mouse", L"MouseSpeed", REG_SZ, 0, L"0"},
         {CU, L"Control Panel\\Mouse", L"MouseThreshold1", REG_SZ, 0, L"0"},
         {CU, L"Control Panel\\Mouse", L"MouseThreshold2", REG_SZ, 0, L"0"}}, false, L"Gaming", true));
    v.push_back(RegTweak(L"show_file_ext", L"Show file extensions in Explorer",
        L"Displays known file extensions (.exe, .txt, .zip) in File Explorer.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"HideFileExt", REG_DWORD, 0, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"show_hidden", L"Show hidden files in Explorer",
        L"Displays hidden files and folders in File Explorer.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Hidden", REG_DWORD, 1, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"taskbar_endtask", L"End Task in taskbar right-click",
        L"Adds an 'End task' option to taskbar app context menus on Windows 11.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\TaskbarDeveloperSettings", L"TaskbarEndTask", REG_DWORD, 1, nullptr}}, false, L"Windows UI", true));
    // 2.8: more reversible tweaks (the common ones of the Sophia Script and WinUtil catalogues), all per-user registry values
    v.push_back(RegTweak(L"sticky_off", L"Sticky Keys, Toggle Keys and Filter Keys shortcuts off",
        L"Stops the pop-up that appears when Shift is pressed five times (and the similar Toggle / Filter Keys shortcuts), which interrupts games and typing.",
        {{CU, L"Control Panel\\Accessibility\\StickyKeys", L"Flags", REG_SZ, 0, L"506"},
         {CU, L"Control Panel\\Accessibility\\ToggleKeys", L"Flags", REG_SZ, 0, L"58"},
         {CU, L"Control Panel\\Accessibility\\Keyboard Response", L"Flags", REG_SZ, 0, L"122"}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"taskview_hide", L"Hide the Task View button",
        L"Removes the Task View button from the taskbar (Win+Tab still works).",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowTaskViewButton", REG_DWORD, 0, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"taskbar_left", L"Taskbar icons aligned to the left (Windows 11)",
        L"Moves the taskbar icons and the Start button to the left edge, as in Windows 10.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl", REG_DWORD, 0, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"search_box_hide", L"Hide the taskbar search box",
        L"Hides the search box on the taskbar (Start search still works).",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Search", L"SearchboxTaskbarMode", REG_DWORD, 0, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"recent_off", L"No recent and frequent items in Quick access",
        L"File Explorer stops listing recently opened files and frequent folders.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"ShowRecent", REG_DWORD, 0, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"ShowFrequent", REG_DWORD, 0, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"copilot_off", L"Windows Copilot off",
        L"Turns Windows Copilot off for this user and removes its taskbar button.",
        {{CU, L"Software\\Policies\\Microsoft\\Windows\\WindowsCopilot", L"TurnOffWindowsCopilot", REG_DWORD, 1, nullptr},
         {CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowCopilotButton", REG_DWORD, 0, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"start_more_pins", L"Start menu shows more pins (Windows 11)",
        L"Uses the Start layout with more pinned apps and fewer recommendations.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_Layout", REG_DWORD, 1, nullptr}}, false, L"Windows UI", true));
    v.push_back(RegTweak(L"onedrive_ads_off", L"No sync-provider notifications (OneDrive ads) in Explorer",
        L"Stops File Explorer from showing OneDrive and Microsoft 365 promotions.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"ShowSyncProviderNotifications", REG_DWORD, 0, nullptr}}, false, L"Privacy", true));
    v.push_back(RegTweak(L"explorer_thispc", L"File Explorer opens to This PC",
        L"Opens File Explorer directly to This PC instead of Home/Quick Access.",
        {{CU, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"LaunchTo", REG_DWORD, 1, nullptr}}, false, L"Windows UI", true));
    return v;
}
inline void RegisterSystemUndo() {
    UndoHandlers()[L"SVC"] = [](const JEntry& e) {
        return e.f.size() >= 3 && SetSvcStart(e.f[0].c_str(), static_cast<DWORD>(_wtoi(e.f[1].c_str())), e.f[2] == L"1");
    };
    UndoHandlers()[L"POWER"] = [](const JEntry& e) {
        const std::wstring g = e.f.empty() || e.f[0].size() != 36 ? L"381b4222-f694-41f0-9685-ff5bb260df2e" : e.f[0];
        return Run(SysDir() + L"\\powercfg.exe", L"/setactive " + g, true, 15000) == 0;
    };
    UndoHandlers()[L"HIBER"] = [](const JEntry&) { return Run(SysDir() + L"\\powercfg.exe", L"-h on", true, 30000) == 0; };
}

// ---------------------------------------------------------------- startup apps
struct StartupItem { std::wstring name, command, publisher, source, approvedKey; HKEY approvedRoot; bool enabled = true; };
inline std::wstring ExeFromCommand(const std::wstring& cmd) {
    std::wstring c = Expand(cmd);
    if (!c.empty() && c[0] == L'"') { const size_t e = c.find(L'"', 1); return c.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1); }
    const size_t e = Lower(c).find(L".exe");
    return e == std::wstring::npos ? c.substr(0, c.find(L' ')) : c.substr(0, e + 4);
}
inline std::wstring Publisher(const std::wstring& exe) {
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(exe.c_str(), &h);
    if (!n) return {};
    std::vector<uint8_t> b(n);
    if (!GetFileVersionInfoW(exe.c_str(), 0, n, b.data())) return {};
    struct LC { WORD l, c; }* tr = nullptr; UINT tl = 0;
    if (!VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &tl) || tl < sizeof(LC)) return {};
    wchar_t q[128]; swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->l, tr->c);
    wchar_t* s = nullptr; UINT sl = 0;
    return VerQueryValueW(b.data(), q, reinterpret_cast<void**>(&s), &sl) && s ? std::wstring(s) : std::wstring();
}
inline bool ApprovedEnabled(HKEY root, const std::wstring& key, const std::wstring& name) {
    const RegValue v = RegGet(root, key, name);
    return !v.exists || v.data.empty() || (v.data[0] & 1) == 0;
}
inline std::vector<StartupItem> StartupItems() {
    std::vector<StartupItem> out;
    const std::wstring ap = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
    struct Src { HKEY root; const wchar_t* key; REGSAM view; const wchar_t* label; const wchar_t* approved; };
    const Src srcs[] = {
        {HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, L"Registry (you)", L"Run"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, L"Registry (all users)", L"Run"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY, L"Registry (all users, 32-bit)", L"Run32"},
    };
    for (const auto& s : srcs)
        for (const auto& kv : RegStrings(s.root, s.key, s.view)) {
            StartupItem it; it.name = kv.first; it.command = kv.second; it.source = s.label;
            it.approvedRoot = s.root; it.approvedKey = ap + s.approved;
            it.enabled = ApprovedEnabled(it.approvedRoot, it.approvedKey, it.name);
            it.publisher = Publisher(ExeFromCommand(it.command));
            out.push_back(it);
        }
    const std::pair<KNOWNFOLDERID, HKEY> folders[] = {{FOLDERID_Startup, HKEY_CURRENT_USER}, {FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE}};
    for (const auto& f : folders) {
        const std::wstring dir = KnownFolder(f.first);
        for (const auto& n : ListDir(dir, false)) {
            if (_wcsicmp(n.c_str(), L"desktop.ini") == 0) continue;
            StartupItem it; it.name = n; it.command = dir + L"\\" + n; it.source = f.second == HKEY_CURRENT_USER ? L"Startup folder (you)" : L"Startup folder (all users)";
            it.approvedRoot = f.second; it.approvedKey = ap + L"StartupFolder";
            it.enabled = ApprovedEnabled(it.approvedRoot, it.approvedKey, it.name);
            out.push_back(it);
        }
    }
    return out;
}
inline bool SetStartupEnabled(const StartupItem& it, bool on) {
    uint8_t v[12] = {static_cast<uint8_t>(on ? 2 : 3)};
    if (!on) { FILETIME ft; GetSystemTimeAsFileTime(&ft); memcpy(v + 4, &ft, 8); }
    const bool wrote = JRegSet(L"startup:" + it.name, std::wstring(on ? L"Startup app on: " : L"Startup app off: ") + it.name, it.approvedRoot, it.approvedKey, it.name, REG_BINARY, v, 12);
    if (!wrote || ApprovedEnabled(it.approvedRoot, it.approvedKey, it.name) != on) {   // read back (SEC-3)
        LogE(it.name + L": the startup setting could not be changed.");
        return false;
    }
    LogOk(it.name + (on ? L" will start with Windows." : L" will not start with Windows."));
    return true;
}

// ---------------------------------------------------------------- cleanup
enum class Tag { Safe, Optional, Confirm };
struct CleanStats { uint64_t bytes = 0; uint64_t files = 0; uint64_t skipped = 0; uint64_t reparseSkipped = 0; uint64_t accessDenied = 0; };
// WT02-M2: one canonical form for every path the cleaner is asked to touch.
// Accepts only absolute drive paths ("C:\..."), also written with "/" or with the "\\?\" prefix. Refuses (returns false): empty or
// relative paths, UNC and device paths, ".." components, wildcard / illegal characters. 8.3 short names are expanded when the path exists.
// The result is lower case, uses "\" only and has no trailing separator (a drive root keeps "c:\").
inline bool CleanCanonicalPath(const std::wstring& raw, std::wstring& out) {
    out.clear();
    std::wstring p = raw;
    for (auto& ch : p) if (ch == L'/') ch = L'\\';
    if (p.compare(0, 4, L"\\\\?\\") == 0) {
        p.erase(0, 4);
        if (p.size() >= 4 && _wcsnicmp(p.c_str(), L"UNC\\", 4) == 0) return false;   // network paths are never cleaned
    } else if (p.compare(0, 2, L"\\\\") == 0) return false;                          // UNC or \\.\ device paths
    if (p.size() < 3 || !iswalpha(p[0]) || p[1] != L':' || p[2] != L'\\') return false;   // drive-absolute only ("c:" alone and "c:dir" are relative)
    for (wchar_t ch : p) if (ch < 0x20 || wcschr(L"<>\"|?*", ch)) return false;
    if (p.find(L':', 2) != std::wstring::npos) return false;                          // alternate data streams
    size_t at = 3;                                                                     // every component: no ".."
    while (at <= p.size()) {
        size_t e = p.find(L'\\', at); if (e == std::wstring::npos) e = p.size();
        if (p.substr(at, e - at) == L"..") return false;
        at = e + 1;
    }
    wchar_t full[32768]; const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);   // collapses "." and doubled separators
    if (n == 0 || n >= std::size(full)) return false;
    std::wstring f = full;
    for (std::wstring head = f; head.size() >= 3; ) {   // expand 8.3 names in the longest part that exists ("C:\\PROGRA~1\\x" is "C:\\Program Files\\x" even when x does not exist)
        wchar_t lng[32768]; const DWORD m = GetLongPathNameW(head.c_str(), lng, static_cast<DWORD>(std::size(lng)));
        if (m > 0 && m < std::size(lng)) { f = std::wstring(lng) + f.substr(head.size()); break; }
        const size_t cut = head.find_last_of(L'\\');
        if (cut == std::wstring::npos || cut < 3) break;
        head.resize(cut);
    }
    if (f.size() < 3 || f[1] != L':' || f[2] != L'\\') return false;
    f = Lower(f);
    while (f.size() > 3 && f.back() == L'\\') f.pop_back();
    out = f;
    return true;
}
// True when `p` is `root` itself (if orEqual) or lies below it. Separator-aware: "c:\windows\system32x" is not below "c:\windows\system32".
inline bool CleanIsUnder(const std::wstring& p, const std::wstring& root, bool orEqual = true) {
    if (root.empty()) return false;
    std::wstring r = root; while (r.size() > 3 && r.back() == L'\\') r.pop_back();
    if (p == r) return orEqual;
    const std::wstring pre = r.back() == L'\\' ? r : r + L'\\';
    return p.size() > pre.size() && p.compare(0, pre.size(), pre) == 0;
}
// A path the cleaner must never empty. Anything that is not a clean absolute path counts as protected.
inline bool IsProtectedCleanupPath(const std::wstring& raw) {
    std::wstring p;
    if (!CleanCanonicalPath(raw, p)) return true;
    if (p.size() == 3) return true;                                                   // a drive root
    auto canon = [](const std::wstring& x) { std::wstring c; return x.empty() || !CleanCanonicalPath(x, c) ? std::wstring() : c; };
    const std::wstring win = canon(WinDir());
    if (p == win) return true;                                                        // the Windows folder itself (its Temp, Logs... are cleaned item by item)
    for (const wchar_t* sub : {L"\\system32", L"\\syswow64", L"\\winsxs"}) if (!win.empty() && CleanIsUnder(p, win + sub)) return true;
    for (auto kf : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86}) if (CleanIsUnder(p, canon(KnownFolder(kf)))) return true;
    const std::wstring prof = canon(KnownFolder(FOLDERID_Profile));
    if (!prof.empty() && (p == prof || p == canon(DirOf(prof)))) return true;         // the user profile and C:\Users
    for (auto kf : {FOLDERID_LocalAppData, FOLDERID_RoamingAppData, FOLDERID_ProgramData}) { const std::wstring k = canon(KnownFolder(kf)); if (!k.empty() && p == k) return true; }
    for (auto kf : {FOLDERID_Documents, FOLDERID_Desktop, FOLDERID_Pictures, FOLDERID_Videos, FOLDERID_Music, FOLDERID_Downloads})
        if (CleanIsUnder(p, canon(KnownFolder(kf)))) return true;
    return false;
}
inline uint64_t SafeCleanDir(const std::wstring& dir, CleanStats& st, bool dryRun, int depth = 0) {
    if (depth > 20 || IsProtectedCleanupPath(dir) || !DirExists(dir)) return 0;
    if (depth > 0) {   // WT02-L2: the entry was a plain folder when it was listed; check again before going in (a junction may have replaced it)
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) { ++st.reparseSkipped; return 0; }
    }
    if (!dryRun && kTestBuild && !InTestArea(dir + L"\\x")) { TestRefused(L"clean folder " + dir); return 0; }
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) ++st.accessDenied;
        return 0;
    }
    uint64_t total = 0;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++st.reparseSkipped; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += SafeCleanDir(p, st, dryRun, depth + 1);   // the child counts its own files in st; they are not added again here (WT02-M1)
            if (!dryRun) RemoveDirectoryW(p.c_str());
            continue;
        }
        const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (dryRun) { total += sz; ++st.files; st.bytes += sz; continue; }
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str())) { total += sz; ++st.files; st.bytes += sz; }
        else { ++st.skipped; }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}
struct CleanItem {
    std::wstring id, title, desc, category = L"Temporary";
    Tag tag;
    bool defaultOn;
    std::function<uint64_t()> size;
    std::function<uint64_t()> clean;
    std::function<uint64_t(uint64_t& accessDenied)> sizeAccess;
};
inline std::vector<std::wstring> BrowserCacheDirs() {
    std::vector<std::wstring> v;
    const std::wstring la = KnownFolder(FOLDERID_LocalAppData);
    const wchar_t* chromium[] = {L"\\Google\\Chrome\\User Data", L"\\Microsoft\\Edge\\User Data", L"\\BraveSoftware\\Brave-Browser\\User Data"};
    for (const auto* c : chromium) {
        const std::wstring root = la + c;
        for (const auto& prof : ListDir(root, true)) {
            if (prof != L"Default" && prof.rfind(L"Profile ", 0) != 0) continue;
            for (const wchar_t* sub : {L"\\Cache", L"\\Code Cache", L"\\GPUCache"}) if (DirExists(root + L"\\" + prof + sub)) v.push_back(root + L"\\" + prof + sub);
        }
    }
    const std::wstring ff = la + L"\\Mozilla\\Firefox\\Profiles";
    for (const auto& prof : ListDir(ff, true)) if (DirExists(ff + L"\\" + prof + L"\\cache2")) v.push_back(ff + L"\\" + prof + L"\\cache2");
    return v;
}
inline bool BrowsersOpen() {
    for (const wchar_t* b : {L"chrome.exe", L"msedge.exe", L"brave.exe", L"firefox.exe"}) if (ProcessRunning(b)) return true;
    return false;
}
// A search pattern is a file-name pattern only: no folders, no "..", no drive or stream characters.
inline bool CleanPatternOk(const wchar_t* pat) {
    if (!pat || !*pat) return false;
    for (const wchar_t* c = pat; *c; ++c) if (*c == L'\\' || *c == L'/' || *c == L':' || *c < 0x20) return false;
    return wcsstr(pat, L"..") == nullptr;
}
inline uint64_t FilesSize(const std::wstring& dir, const wchar_t* pat, uint64_t* accessDenied = nullptr) {
    uint64_t t = 0;
    if (!CleanPatternOk(pat)) return 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) {
        if (accessDenied && GetLastError() == ERROR_ACCESS_DENIED) ++(*accessDenied);
        return 0;
    }
    do if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) t += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; while (FindNextFileW(f, &fd));
    FindClose(f);
    return t;
}
// WT02-L1: same protections as SafeCleanDir. A protected or malformed folder is refused; in the isolated test build only the test area
// may be touched; reparse points (links) and folders are skipped; a read-only attribute is cleared only after those checks passed.
inline uint64_t DeleteFiles(const std::wstring& dir, const wchar_t* pat, CleanStats* stats = nullptr) {
    CleanStats local; CleanStats& st = stats ? *stats : local;
    if (!CleanPatternOk(pat) || IsProtectedCleanupPath(dir) || !DirExists(dir)) return 0;
    if (kTestBuild && !InTestArea(dir + L"\\x")) { TestRefused(L"delete files in " + dir); return 0; }
    uint64_t t = 0;
    WIN32_FIND_DATAW fd; HANDLE f = FindFirstFileW((dir + L"\\" + pat).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++st.reparseSkipped; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring p = dir + L"\\" + fd.cFileName;
        const uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str())) { t += sz; ++st.files; st.bytes += sz; } else ++st.skipped;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return t;
}
// WT02-L3: the items "Clean selected" acts on: selected AND inside the category filter ("All" or empty = every category).
inline std::vector<size_t> CleanSelectedInScope(const std::vector<CleanItem>& items, const std::vector<bool>& selected, const std::wstring& category) {
    std::vector<size_t> out;
    for (size_t i = 0; i < items.size() && i < selected.size(); ++i)
        if (selected[i] && (category.empty() || category == L"All" || items[i].category == category)) out.push_back(i);
    return out;
}
inline const std::vector<CleanItem>& CleanItems() {
    static std::vector<CleanItem> v;
    if (!v.empty()) return v;
    auto folder = [](std::function<std::wstring()> path) {
        auto sz = [path](uint64_t& denied) { CleanStats st; const uint64_t b = SafeCleanDir(path(), st, true); denied += st.accessDenied; return b; };
        auto cl = [path] { CleanStats st; const uint64_t f = SafeCleanDir(path(), st, false); if (st.skipped) LogW(path() + L": " + std::to_wstring(st.skipped) + L" locked/in-use file(s) skipped."); return f; };
        return std::make_pair(sz, cl);
    };
    auto add = [&](const wchar_t* id, const wchar_t* title, const wchar_t* desc, Tag tag, bool on,
                   std::function<uint64_t(uint64_t&)> szA, std::function<uint64_t()> cl, const wchar_t* cat = L"Temporary") {
        v.push_back({id, title, desc, cat, tag, on, [szA] { uint64_t d = 0; return szA(d); }, std::move(cl), std::move(szA)});
    };
    auto addF = [&](const wchar_t* id, const wchar_t* title, const wchar_t* desc, Tag tag, bool on,
                    std::pair<std::function<uint64_t(uint64_t&)>, std::function<uint64_t()>> f, const wchar_t* cat = L"Temporary") {
        add(id, title, desc, tag, on, std::move(f.first), std::move(f.second), cat);
    };
    addF(L"usertemp", L"Your temporary files", L"%TEMP%: leftovers from installers and apps.", Tag::Safe, true, folder([] { return Expand(L"%TEMP%"); }), L"Temporary");
    addF(L"wintemp", L"Windows temporary files", L"C:\\Windows\\Temp.", Tag::Safe, true, folder([] { return WinDir() + L"\\Temp"; }), L"Temporary");
    add(L"crash", L"Crash dumps and error reports", L"Memory dumps and Windows Error Reporting files.", Tag::Safe, true,
        [](uint64_t& d) { return FolderSize(KnownFolder(FOLDERID_LocalAppData) + L"\\CrashDumps", L"*", 0, &d) +
                                  FolderSize(KnownFolder(FOLDERID_ProgramData) + L"\\Microsoft\\Windows\\WER\\ReportArchive", L"*", 0, &d) +
                                  FolderSize(KnownFolder(FOLDERID_ProgramData) + L"\\Microsoft\\Windows\\WER\\ReportQueue", L"*", 0, &d); },
        [] { return Evict(KnownFolder(FOLDERID_LocalAppData) + L"\\CrashDumps", false, false) + Evict(KnownFolder(FOLDERID_ProgramData) + L"\\Microsoft\\Windows\\WER\\ReportArchive", false, false) +
                    Evict(KnownFolder(FOLDERID_ProgramData) + L"\\Microsoft\\Windows\\WER\\ReportQueue", false, false); }, L"Logs");
    add(L"cbs", L"Windows setup logs", L"Old CBS log files.", Tag::Safe, true,
        [](uint64_t& d) { return FilesSize(WinDir() + L"\\Logs\\CBS", L"*.log", &d) + FilesSize(WinDir() + L"\\Logs\\CBS", L"*.cab", &d); },
        [] { return DeleteFiles(WinDir() + L"\\Logs\\CBS", L"*.log") + DeleteFiles(WinDir() + L"\\Logs\\CBS", L"*.cab"); }, L"Logs");
    add(L"wucache", L"Windows Update download cache", L"Already-installed update files. Windows downloads again if needed.", Tag::Optional, true,
        [](uint64_t& d) { return FolderSize(WinDir() + L"\\SoftwareDistribution\\Download", L"*", 0, &d); },
        [] { ServiceControlWait(L"wuauserv", false); ServiceControlWait(L"bits", false);
             const uint64_t r = Evict(WinDir() + L"\\SoftwareDistribution\\Download", false, false);
             ServiceControlWait(L"wuauserv", true); ServiceControlWait(L"bits", true); return r; }, L"System");
    add(L"delivery", L"Delivery Optimization cache", L"Update pieces shared with other PCs.", Tag::Optional, true,
        [](uint64_t& d) { return FolderSize(WinDir() + L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache", L"*", 0, &d); },
        [] { ServiceControlWait(L"DoSvc", false);
             const uint64_t r = Evict(WinDir() + L"\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache", false, false);
             ServiceControlWait(L"DoSvc", true); return r; }, L"System");
    add(L"browsers", L"Browser caches", L"Chrome, Edge, Brave and Firefox. Close the browsers first.", Tag::Optional, true,
        [](uint64_t& d) { uint64_t t = 0; for (const auto& dir : BrowserCacheDirs()) t += FolderSize(dir, L"*", 0, &d); return t; },
        [] { if (BrowsersOpen()) { LogW(L"Browser caches skipped: close your browsers first."); return uint64_t(0); }
             uint64_t t = 0; for (const auto& dir : BrowserCacheDirs()) t += Evict(dir, false, false); return t; }, L"Caches");
    add(L"recycle", L"Recycle Bin", L"Empties the Recycle Bin on all drives.", Tag::Confirm, false,
        [](uint64_t&) { SHQUERYRBINFO q{sizeof(q)}; return SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) ? static_cast<uint64_t>(q.i64Size) : 0ull; },
        [] { SHQUERYRBINFO q{sizeof(q)}; SHQueryRecycleBinW(nullptr, &q);
             SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND); return static_cast<uint64_t>(q.i64Size); }, L"System");
    add(L"prefetch", L"Prefetch", L"Windows rebuilds it; apps may start slower for a while.", Tag::Optional, false,
        [](uint64_t& d) { return FolderSize(WinDir() + L"\\Prefetch", L"*", 0, &d); },
        [] { ServiceControlWait(L"SysMain", false); const uint64_t r = Evict(WinDir() + L"\\Prefetch", false, false); ServiceControlWait(L"SysMain", true); return r; }, L"System");
    add(L"recent", L"Recent files list", L"Clears the list of recently opened files.", Tag::Optional, false,
        [](uint64_t& d) { return FolderSize(KnownFolder(FOLDERID_Recent), L"*", 0, &d); },
        [] { return Evict(KnownFolder(FOLDERID_Recent), false, false); }, L"Temporary");
    add(L"thumbs", L"Thumbnail cache", L"Picture previews are rebuilt. Finishes after a restart.", Tag::Optional, false,
        [](uint64_t& d) { return FilesSize(KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Explorer", L"thumbcache_*.db", &d); },
        [] { uint64_t t = FilesSize(KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Explorer", L"thumbcache_*.db");
             for (const auto& f : ListDir(KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Explorer", false, L"thumbcache_*.db"))
                 DeleteNowOrAtReboot(KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Explorer\\" + f);
             return t; }, L"Caches");
    // WT-02: Curated BleachBit-inspired safe cleanup targets (caches & logs; zero personal/session data)
    addF(L"dxshader", L"DirectX shader cache", L"Compiled GPU shaders in %LOCALAPPDATA%\\D3DSCache. Rebuilt automatically.", Tag::Safe, true,
         folder([] { return KnownFolder(FOLDERID_LocalAppData) + L"\\D3DSCache"; }), L"Caches");
    add(L"nvshader", L"NVIDIA shader cache", L"NVIDIA DXCache and GLCache files. Safe to clear; rebuilt by driver.", Tag::Safe, true,
        [](uint64_t& d) { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData); const uint64_t r = SafeCleanDir(la + L"\\NVIDIA\\DXCache", s, true) + SafeCleanDir(la + L"\\NVIDIA\\GLCache", s, true); d += s.accessDenied; return r; },
        [] { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData); const uint64_t r = SafeCleanDir(la + L"\\NVIDIA\\DXCache", s, false) + SafeCleanDir(la + L"\\NVIDIA\\GLCache", s, false);
             if (s.skipped) LogW(L"NVIDIA shader cache: " + std::to_wstring(s.skipped) + L" in-use file(s) skipped."); return r; }, L"Caches");
    add(L"amdshader", L"AMD shader cache", L"AMD DxCache and GLCache files. Safe to clear; rebuilt by driver.", Tag::Safe, false,
        [](uint64_t& d) { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData); const uint64_t r = SafeCleanDir(la + L"\\AMD\\DxCache", s, true) + SafeCleanDir(la + L"\\AMD\\GLCache", s, true); d += s.accessDenied; return r; },
        [] { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData); const uint64_t r = SafeCleanDir(la + L"\\AMD\\DxCache", s, false) + SafeCleanDir(la + L"\\AMD\\GLCache", s, false);
             if (s.skipped) LogW(L"AMD shader cache: " + std::to_wstring(s.skipped) + L" in-use file(s) skipped."); return r; }, L"Caches");
    add(L"winlogs", L"Windows DISM & setup logs", L"DISM and DPX diagnostic logs in C:\\Windows\\Logs.", Tag::Safe, true,
        [](uint64_t& d) { return FilesSize(WinDir() + L"\\Logs\\DISM", L"*.log", &d) + FilesSize(WinDir() + L"\\Logs\\DPX", L"*.log", &d); },
        [] { return DeleteFiles(WinDir() + L"\\Logs\\DISM", L"*.log") + DeleteFiles(WinDir() + L"\\Logs\\DPX", L"*.log"); }, L"Logs");
    add(L"pkgcaches", L"Developer package caches", L"pip, npm-cache, and NuGet v3-cache downloads. Re-downloaded when needed.", Tag::Optional, false,
        [](uint64_t& d) { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData);
             const uint64_t r = SafeCleanDir(la + L"\\pip\\Cache", s, true) + SafeCleanDir(la + L"\\npm-cache", s, true) + SafeCleanDir(la + L"\\NuGet\\v3-cache", s, true); d += s.accessDenied; return r; },
        [] { CleanStats s; const std::wstring la = KnownFolder(FOLDERID_LocalAppData);
             return SafeCleanDir(la + L"\\pip\\Cache", s, false) + SafeCleanDir(la + L"\\npm-cache", s, false) + SafeCleanDir(la + L"\\NuGet\\v3-cache", s, false); }, L"Caches");
    // 2.8: application caches (the common ones of the BleachBit definitions; AnkhCore's own list). Only cache folders, through SafeCleanDir's protections.
    auto multi = [](std::function<std::vector<std::wstring>()> paths) {
        auto sz = [paths](uint64_t& denied) { CleanStats st; uint64_t b = 0; for (const auto& d : paths()) b += SafeCleanDir(d, st, true); denied += st.accessDenied; return b; };
        auto cl2 = [paths] { CleanStats st; uint64_t f = 0; for (const auto& d : paths()) f += SafeCleanDir(d, st, false); if (st.skipped) LogW(std::to_wstring(st.skipped) + L" locked/in-use file(s) skipped in an application cache."); return f; };
        return std::make_pair(sz, cl2);
    };
    auto chromium = [](const std::wstring& root) { return std::vector<std::wstring>{root + L"\\Cache", root + L"\\Code Cache", root + L"\\GPUCache", root + L"\\DawnCache"}; };
    addF(L"discord", L"Discord cache", L"Cache, Code Cache and GPUCache of the Discord desktop app. Rebuilt automatically.", Tag::Safe, false,
         multi([chromium] { return chromium(KnownFolder(FOLDERID_RoamingAppData) + L"\\discord"); }), L"Caches");
    addF(L"slack", L"Slack cache", L"Cache, Code Cache and GPUCache of the Slack desktop app. Rebuilt automatically.", Tag::Safe, false,
         multi([chromium] { return chromium(KnownFolder(FOLDERID_RoamingAppData) + L"\\Slack"); }), L"Caches");
    addF(L"teams", L"Microsoft Teams cache (classic)", L"Cache, Code Cache, GPUCache and temporary files of classic Microsoft Teams. Rebuilt automatically.", Tag::Safe, false,
         multi([chromium] { auto v = chromium(KnownFolder(FOLDERID_RoamingAppData) + L"\\Microsoft\\Teams"); v.push_back(KnownFolder(FOLDERID_RoamingAppData) + L"\\Microsoft\\Teams\\tmp"); return v; }), L"Caches");
    addF(L"vscode", L"Visual Studio Code cache", L"Cache, cached data, Code Cache, GPUCache and logs of VS Code. Settings and extensions are not touched.", Tag::Safe, false,
         multi([chromium] { auto v = chromium(KnownFolder(FOLDERID_RoamingAppData) + L"\\Code"); const std::wstring r = KnownFolder(FOLDERID_RoamingAppData) + L"\\Code"; v.push_back(r + L"\\CachedData"); v.push_back(r + L"\\logs"); return v; }), L"Caches");
    addF(L"spotify", L"Spotify cache", L"The offline song cache (Storage) of Spotify. Songs are streamed again when needed.", Tag::Optional, false,
         multi([] { return std::vector<std::wstring>{KnownFolder(FOLDERID_LocalAppData) + L"\\Spotify\\Storage", KnownFolder(FOLDERID_LocalAppData) + L"\\Spotify\\Data"}; }), L"Caches");
    addF(L"steamweb", L"Steam web browser cache", L"The embedded web browser cache of the Steam client (not your games or saves).", Tag::Safe, false,
         multi([] { return std::vector<std::wstring>{KnownFolder(FOLDERID_LocalAppData) + L"\\Steam\\htmlcache"}; }), L"Caches");
    addF(L"officecache", L"Microsoft Office file cache", L"The Office document cache (OfficeFileCache). Documents are not touched; Office rebuilds it.", Tag::Optional, false,
         multi([] { return std::vector<std::wstring>{KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Office\\16.0\\OfficeFileCache"}; }), L"Caches");
    addF(L"storecache", L"Microsoft Store cache", L"The local cache of the Microsoft Store app. Rebuilt the next time the Store opens.", Tag::Optional, false,
         multi([] { return std::vector<std::wstring>{KnownFolder(FOLDERID_LocalAppData) + L"\\Packages\\Microsoft.WindowsStore_8wekyb3d8bbwe\\LocalCache"}; }), L"Caches");
    add(L"memorydumps", L"Kernel minidump files", L"Old crash minidumps in C:\\Windows\\Minidump.", Tag::Optional, false,
        [](uint64_t& d) { return FilesSize(WinDir() + L"\\Minidump", L"*.dmp", &d); },
        [] { return DeleteFiles(WinDir() + L"\\Minidump", L"*.dmp"); }, L"Logs");
    return v;
}

// ---------------------------------------------------------------- tools and repair
using LineFn = std::function<void(const std::wstring&)>;
inline bool PurgeStandbyRam(uint64_t& freed) {
    EnablePrivilege(L"SeProfileSingleProcessPrivilege");
    using NtSSI = LONG(NTAPI*)(ULONG, PVOID, ULONG);
    auto f = reinterpret_cast<NtSSI>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetSystemInformation"));
    MEMORYSTATUSEX a{sizeof(a)}; GlobalMemoryStatusEx(&a);
    int cmd = 4;   // MemoryPurgeStandbyList
    const bool ok = f && f(80 /* SystemMemoryListInformation */, &cmd, sizeof(cmd)) >= 0;
    Sleep(300);
    MEMORYSTATUSEX b{sizeof(b)}; GlobalMemoryStatusEx(&b);
    freed = b.ullAvailPhys > a.ullAvailPhys ? b.ullAvailPhys - a.ullAvailPhys : 0;
    return ok;
}
struct ToolDef { std::wstring id, title, desc; bool needsConfirm; bool restart; std::function<bool(const LineFn&)> run; };
inline const std::vector<ToolDef>& Tools() {
    static std::vector<ToolDef> v;
    if (!v.empty()) return v;
    const std::wstring sys = SysDir();
    v.push_back({L"ram", L"Free up RAM", L"Empties the standby memory list (like RAMMap).", false, false, [](const LineFn&) {
        uint64_t freed = 0; const bool ok = PurgeStandbyRam(freed);
        if (ok) LogOk(L"RAM: " + FormatBytes(freed) + L" freed."); else LogE(L"RAM could not be freed.");
        return ok; }});
    v.push_back({L"dns", L"Flush DNS", L"Clears the DNS cache (fixes some sites not opening).", false, false, [sys](const LineFn& l) {
        return RunCapture(sys + L"\\ipconfig.exe", L"/flushdns", l) == 0; }});
    v.push_back({L"net", L"Network tuning (safe)", L"TCP auto-tuning normal and RSS on.", false, false, [sys](const LineFn& l) {
        OpTally t;   // SEC-3: both commands count
        t.Add(RunCapture(sys + L"\\netsh.exe", L"int tcp set global autotuninglevel=normal", l) == 0);
        t.Add(RunCapture(sys + L"\\netsh.exe", L"int tcp set global rss=enabled", l) == 0);
        (t.Success() ? LogOk : LogE)(L"Network tuning: " + t.Text(L"command", L"commands", L"succeeded"));
        return t.Success(); }});
    v.push_back({L"bgkill", L"Close background apps", L"OneDrive, Phone Link and Widgets until the next restart.", false, false, [](const LineFn&) {
        int n = 0;
        for (const wchar_t* p : {L"OneDrive.exe", L"YourPhone.exe", L"PhoneExperienceHost.exe", L"Widgets.exe", L"WidgetService.exe"}) n += KillProcess(p, 0);
        LogOk(L"Closed " + std::to_wstring(n) + L" background app process(es).");
        return true; }});
    v.push_back({L"cleanmgr", L"Disk Cleanup", L"Opens Windows' Disk Cleanup.", false, false, [sys](const LineFn&) {
        const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (sys + L"\\cleanmgr.exe").c_str(), L"/d C:", nullptr, SW_SHOWNORMAL)) > 32;
        if (!ok) LogE(L"Disk Cleanup could not be opened.");
        return ok; }});
    v.push_back({L"chkscan", L"Check disk (scan only)", L"Looks for file system errors on C: without changing anything.", false, false, [sys](const LineFn& l) {
        return RunCapture(sys + L"\\chkdsk.exe", L"C: /scan", l) == 0; }});
    v.push_back({L"chkfix", L"Repair disk at restart", L"chkdsk C: /f /r runs at the next restart. It can take a long time.", true, true, [sys](const LineFn& l) {
        const DWORD rc = RunCapture(sys + L"\\cmd.exe", L"/c echo Y| \"" + sys + L"\\chkdsk.exe\" C: /f /r", l);
        if (rc == kRunLaunchFailed || rc == kRunTimedOut) { LogE(L"chkdsk could not be run, so no disk repair is scheduled."); return false; }
        LogI(L"chkdsk finished with exit code " + std::to_wstring(rc) + L". Its own output above says whether the repair is scheduled for the next restart.");
        return true; }});
    v.push_back({L"services", L"Open Services", L"Windows' Services manager.", false, false, [](const LineFn&) {
        const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        if (!ok) LogE(L"Services could not be opened.");
        return ok; }});
    return v;
}
inline const std::vector<ToolDef>& RepairTools() {
    static std::vector<ToolDef> v;
    if (!v.empty()) return v;
    const std::wstring sys = SysDir();
    v.push_back({L"sfc", L"System File Checker (SFC)", L"Repairs damaged Windows files. Note: it also puts back the original Windows icons.", true, false,
        [sys](const LineFn& l) { return RunCapture(sys + L"\\sfc.exe", L"/scannow", l) == 0; }});
    v.push_back({L"dismscan", L"DISM ScanHealth", L"Checks the Windows image for damage (read only).", false, false,
        [sys](const LineFn& l) { return RunCapture(sys + L"\\dism.exe", L"/Online /Cleanup-Image /ScanHealth", l) == 0; }});
    v.push_back({L"dismrestore", L"DISM RestoreHealth", L"Repairs the Windows image (uses Windows Update).", false, false,
        [sys](const LineFn& l) { return RunCapture(sys + L"\\dism.exe", L"/Online /Cleanup-Image /RestoreHealth", l) == 0; }});
    v.push_back({L"component", L"Component cleanup", L"Removes superseded update files (DISM StartComponentCleanup).", false, false,
        [sys](const LineFn& l) { return RunCapture(sys + L"\\dism.exe", L"/Online /Cleanup-Image /StartComponentCleanup", l) == 0; }});
    v.push_back({L"netreset", L"Network reset", L"Resets Winsock and TCP/IP and flushes DNS. Restart afterwards.", true, true, [sys](const LineFn& l) {
        OpTally t;   // SEC-3: all three commands count
        t.Add(RunCapture(sys + L"\\netsh.exe", L"winsock reset", l) == 0);
        t.Add(RunCapture(sys + L"\\netsh.exe", L"int ip reset", l) == 0);
        t.Add(RunCapture(sys + L"\\ipconfig.exe", L"/flushdns", l) == 0);
        (t.Success() ? LogOk : LogE)(L"Network reset: " + t.Text(L"command", L"commands", L"succeeded"));
        return t.Success(); }});
    return v;
}

// ---------------------------------------------------------------- monitoring
struct Usage { double cpu = 0, ram = 0, gpu = -1, disk = 0; uint64_t ramUsed = 0, ramTotal = 0, diskFree = 0, diskTotal = 0, uptimeMs = 0; };
struct Monitor {
    FILETIME pi{}, pk{}, pu{};
    Monitor() { GetSystemTimes(&pi, &pk, &pu); }
    static uint64_t U(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
    // CPU, memory and the system drive. GPU utilization is not sampled here: the engine fills Usage::gpu from
    // GpuProbe (gpu_probe.h), per adapter. -1 means "no reading".
    Usage Sample() {
        Usage u;
        FILETIME i, k, us; GetSystemTimes(&i, &k, &us);
        const uint64_t di = U(i) - U(pi), dk = U(k) - U(pk), du = U(us) - U(pu);
        if (dk + du) u.cpu = 100.0 * (dk + du - di) / (dk + du);
        pi = i; pk = k; pu = us;
        MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms);
        u.ram = ms.dwMemoryLoad; u.ramTotal = ms.ullTotalPhys; u.ramUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        ULARGE_INTEGER fa, tot, fr;
        if (GetDiskFreeSpaceExW((WinDir().substr(0, 3)).c_str(), &fa, &tot, &fr)) { u.diskFree = fr.QuadPart; u.diskTotal = tot.QuadPart; u.disk = 100.0 * (tot.QuadPart - fr.QuadPart) / std::max<uint64_t>(1, tot.QuadPart); }
        u.uptimeMs = GetTickCount64();
        return u;
    }
};
inline std::wstring UptimeText(uint64_t ms) {
    const uint64_t m = ms / 60000; wchar_t b[64];
    swprintf(b, 64, L"%llud %lluh %llum", static_cast<unsigned long long>(m / 1440), static_cast<unsigned long long>(m / 60 % 24), static_cast<unsigned long long>(m % 60));
    return b;
}
struct SecurityInfo { std::wstring av; bool avOn = false; bool firewall = false; bool uac = false; int secureBoot = -1; };
inline SecurityInfo GetSecurity() {
    SecurityInfo s;
    for (const auto& r : WmiQuery(L"ROOT\\SecurityCenter2", L"SELECT displayName, productState FROM AntiVirusProduct", {L"displayName", L"productState"})) {
        const auto it = r.find(L"productState"); if (it == r.end()) continue;
        const unsigned st = static_cast<unsigned>(_wtoi(it->second.c_str()));
        const bool on = ((st >> 12) & 0xF) == 1;
        if (on || s.av.empty()) { s.av = r.count(L"displayName") ? r.at(L"displayName") : L"Antivirus"; s.avOn = on; }
        if (on) break;
    }
    const wchar_t* fw = L"SYSTEM\\CurrentControlSet\\Services\\SharedAccess\\Parameters\\FirewallPolicy\\";
    s.firewall = RegDword(HKEY_LOCAL_MACHINE, std::wstring(fw) + L"StandardProfile", L"EnableFirewall", 0) &&
                 RegDword(HKEY_LOCAL_MACHINE, std::wstring(fw) + L"PublicProfile", L"EnableFirewall", 0);
    s.uac = RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", L"EnableLUA", 1) != 0;
    const RegValue sb = RegGet(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled");
    if (sb.exists && sb.data.size() >= 4) s.secureBoot = *reinterpret_cast<const DWORD*>(sb.data.data()) ? 1 : 0;
    return s;
}
struct DiskInfo { std::wstring kind = L"Unknown", model, health = L"Unknown"; bool healthOk = true; };
inline DiskInfo GetSystemDisk() {
    DiskInfo d;
    const std::wstring vol = L"\\\\.\\" + WinDir().substr(0, 2);
    HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD diskNo = 0;
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{StorageDeviceProperty, PropertyStandardQuery, {0}};
        std::vector<uint8_t> b(1024); DWORD r = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), b.data(), static_cast<DWORD>(b.size()), &r, nullptr)) {
            auto* dd = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(b.data());
            if (dd->ProductIdOffset) { std::string m(reinterpret_cast<char*>(b.data()) + dd->ProductIdOffset); while (!m.empty() && m.back() == ' ') m.pop_back(); d.model = Utf8ToW(m); }
            if (dd->BusType == BusTypeNvme) d.kind = L"NVMe SSD";
        }
        if (d.kind == L"Unknown") {
            STORAGE_PROPERTY_QUERY q2{StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {0}};
            DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q2, sizeof(q2), &sp, sizeof(sp), &r, nullptr)) d.kind = sp.IncursSeekPenalty ? L"HDD" : L"SSD";
        }
        STORAGE_DEVICE_NUMBER sdn{};
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof(sdn), &r, nullptr)) diskNo = sdn.DeviceNumber;
        CloseHandle(h);
    }
    for (const auto& r : WmiQuery(L"ROOT\\Microsoft\\Windows\\Storage", L"SELECT DeviceId, HealthStatus FROM MSFT_PhysicalDisk", {L"DeviceId", L"HealthStatus"})) {
        if (!r.count(L"DeviceId") || _wtoi(r.at(L"DeviceId").c_str()) != static_cast<int>(diskNo) || !r.count(L"HealthStatus")) continue;
        const int hs = _wtoi(r.at(L"HealthStatus").c_str());
        d.health = hs == 0 ? L"Healthy" : hs == 1 ? L"Warning" : hs == 2 ? L"Unhealthy" : L"Unknown";
        d.healthOk = hs == 0 || hs == 5;
    }
    return d;
}
struct RestorePoint { std::wstring description, date; int seq = 0; };
inline std::vector<RestorePoint> RestorePoints() {
    std::vector<RestorePoint> v;
    for (const auto& r : WmiQuery(L"ROOT\\default", L"SELECT Description, CreationTime, SequenceNumber FROM SystemRestore", {L"Description", L"CreationTime", L"SequenceNumber"})) {
        RestorePoint p;
        p.description = r.count(L"Description") ? r.at(L"Description") : L"";
        const std::wstring t = r.count(L"CreationTime") ? r.at(L"CreationTime") : L"";
        if (t.size() >= 12) p.date = t.substr(0, 4) + L"-" + t.substr(4, 2) + L"-" + t.substr(6, 2) + L" " + t.substr(8, 2) + L":" + t.substr(10, 2);
        p.seq = r.count(L"SequenceNumber") ? _wtoi(r.at(L"SequenceNumber").c_str()) : 0;
        v.push_back(p);
    }
    std::sort(v.begin(), v.end(), [](const RestorePoint& a, const RestorePoint& b) { return a.seq > b.seq; });
    return v;
}
inline bool PendingReboot() {
    return RegKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending") ||
           RegKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
           RegGet(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", L"PendingFileRenameOperations").exists;
}
// System Restore point (System Protection switched on if needed, 24-hour
// limit lifted for this call).
inline bool CreateRestorePoint(const std::wstring& description, std::wstring& err) {
    const wchar_t* srKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
    const RegValue oldFreq = RegGet(HKEY_LOCAL_MACHINE, srKey, L"SystemRestorePointCreationFrequency");
    RegSetDword(HKEY_LOCAL_MACHINE, srKey, L"SystemRestorePointCreationFrequency", 0);
    wchar_t drive[4] = {WinDir()[0], L':', L'\\', 0};
    Run(SysDir() + L"\\WindowsPowerShell\\v1.0\\powershell.exe",
        std::wstring(L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Enable-ComputerRestore -Drive '") + drive + L"' -ErrorAction SilentlyContinue\"", true, 120000);
    bool ok = false;
    if (HMODULE sr = LoadLibraryW(L"srclient.dll")) {
        using FnSet = BOOL(WINAPI*)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
        if (auto set = reinterpret_cast<FnSet>(GetProcAddress(sr, "SRSetRestorePointW"))) {
            RESTOREPOINTINFOW info{}; info.dwEventType = BEGIN_SYSTEM_CHANGE; info.dwRestorePtType = APPLICATION_INSTALL;
            lstrcpynW(info.szDescription, description.c_str(), MAX_DESC_W);
            STATEMGRSTATUS st{};
            if (set(&info, &st)) { ok = true; info.dwEventType = END_SYSTEM_CHANGE; info.llSequenceNumber = st.llSequenceNumber; set(&info, &st); }
            else err = st.nStatus == ERROR_SERVICE_DISABLED ? L"System Protection is turned off on this PC." : L"Windows error " + std::to_wstring(st.nStatus) + L".";
        }
        FreeLibrary(sr);
    } else err = L"System Restore is not available on this PC.";
    if (oldFreq.exists) RegSet(HKEY_LOCAL_MACHINE, srKey, L"SystemRestorePointCreationFrequency", oldFreq.type, oldFreq.data.data(), static_cast<DWORD>(oldFreq.data.size()));
    else RegDel(HKEY_LOCAL_MACHINE, srKey, L"SystemRestorePointCreationFrequency");
    if (ok) LogOk(L"Restore point created: " + description); else LogE(L"Restore point failed: " + err);
    return ok;
}

// ---------------------------------------------------------------- health checks
// 2.9.8: what a health finding offers the person: a direct fix, or the page where it is solved. The scan itself changes nothing.
enum class FindingAct { None, CreateRestorePoint, StartWindhawk, OpenSystemCare, OpenSecurity, OpenUac, OpenStorage, OpenProcesses };
inline bool FindingActIsFix(FindingAct a) { return a == FindingAct::CreateRestorePoint || a == FindingAct::StartWindhawk; }
inline const wchar_t* FindingActLabel(FindingAct a) {
    switch (a) {
    case FindingAct::CreateRestorePoint: return L"Create restore point";
    case FindingAct::StartWindhawk: return L"Start Windhawk";
    case FindingAct::OpenSystemCare: return L"Open System Care";
    case FindingAct::OpenSecurity: return L"Open Windows Security";
    case FindingAct::OpenUac: return L"Open UAC settings";
    case FindingAct::OpenStorage: return L"Open Storage";
    case FindingAct::OpenProcesses: return L"Open Process Manager";
    default: return L"";
    }
}
struct Finding { int severity; std::wstring text; FindingAct act = FindingAct::None; };   // 0 info, 1 minor, 2 major, 3 critical
// ---------------------------------------------------------------- 2.8.3: status tones for the dashboard
// The Windows / Uptime / System disk rows used the interface accent colour, which is not a status colour (with a neutral Windows accent it shows as grey).
// Each row now has a real tone from its own data: Good, Warning, Error, or Unknown when the value is not available (the only grey).
enum class Tone { Unknown, Good, Info, Warning, Error };
constexpr unsigned long long kUptimeWarnMs = 7ull * 86400000ull;   // the same limit the health scan uses for "not restarted for N days"
inline Tone ToneWindows(unsigned build) { return build == 0 ? Tone::Unknown : build >= 22000 ? Tone::Good : Tone::Warning; }   // Windows 10 is past its mainstream support; Windows 11 is current
inline Tone ToneUptime(unsigned long long uptimeMs) { return uptimeMs == 0 ? Tone::Unknown : uptimeMs > kUptimeWarnMs ? Tone::Warning : Tone::Good; }
// freePct: free space of the system drive in percent (< 0 = unknown). healthText: what the disk reported ("OK", "Unknown", ...). Same thresholds as the health scan (5 % and 12 %).
inline Tone ToneSystemDisk(bool dataReady, bool healthOk, const std::wstring& healthText, double freePct) {
    if (!dataReady) return Tone::Unknown;
    if (!healthOk) return Tone::Error;
    if (freePct >= 0 && freePct < 5) return Tone::Error;
    if (freePct >= 0 && freePct < 12) return Tone::Warning;
    if (healthText == L"Unknown" && freePct < 0) return Tone::Unknown;
    return Tone::Good;
}
// The scan button: its colour follows the latest valid health result (Health::score 0 good, 1 note, 2 warning, 3 problem); nothing before the first scan.
enum class ScanButtonState { Unknown, Scanning, Good, Info, Warning, Error };
inline ScanButtonState ScanStateOf(bool scanning, bool done, int score) {
    if (scanning) return ScanButtonState::Scanning;
    if (!done) return ScanButtonState::Unknown;
    return score <= 0 ? ScanButtonState::Good : score == 1 ? ScanButtonState::Info : score == 2 ? ScanButtonState::Warning : ScanButtonState::Error;
}

struct Health { int score = 0; std::wstring label; std::vector<Finding> findings; };
inline Health RunHealthScan(const Usage& u, const SecurityInfo& sec, const DiskInfo& disk, const std::vector<RestorePoint>& rps) {
    Health h;
    auto add = [&](int s, const std::wstring& t, FindingAct a = FindingAct::None) { h.findings.push_back({s, t, a}); };
    const double freePct = u.diskTotal ? 100.0 * u.diskFree / u.diskTotal : 100;
    if (freePct < 5) add(3, L"System drive almost full (" + FormatBytes(u.diskFree) + L" free). Use Optimize > Cleanup.", FindingAct::OpenSystemCare);
    else if (freePct < 12) add(2, L"System drive is getting full (" + FormatBytes(u.diskFree) + L" free).", FindingAct::OpenSystemCare);
    if (!sec.avOn) add(3, L"No active antivirus" + (sec.av.empty() ? std::wstring() : L" (" + sec.av + L" is off)") + L".", FindingAct::OpenSecurity);
    if (!sec.firewall) add(2, L"Windows Firewall is off.", FindingAct::OpenSecurity);
    if (!sec.uac) add(2, L"User Account Control (UAC) is off.", FindingAct::OpenUac);
    if (!disk.healthOk) add(3, L"The system disk reports: " + disk.health + L". Back up your files.", FindingAct::OpenStorage);
    if (rps.empty()) add(2, L"No System Restore points. Create one in Recovery.", FindingAct::CreateRestorePoint);
    if (PendingReboot()) add(1, L"Windows is waiting for a restart.");
    if (u.ram > 90) add(1, L"Memory is almost full right now.", FindingAct::OpenProcesses);
    if (u.uptimeMs > kUptimeWarnMs) add(1, L"Not restarted for " + UptimeText(u.uptimeMs) + L".");
    if (WindhawkInstalled() && !WindhawkRunning()) add(1, L"Windhawk is not running; mods are off.", FindingAct::StartWindhawk);
    int worst = 0, sum = 0; for (const auto& f : h.findings) { worst = std::max(worst, f.severity); sum += f.severity; }
    h.score = worst >= 3 ? 3 : (worst == 2 || sum >= 3) ? 2 : sum ? 1 : 0;
    static const wchar_t* labels[] = {L"Excellent", L"Good", L"Needs attention", L"Critical"};
    h.label = labels[h.score];
    if (h.findings.empty()) add(0, L"Everything looks fine.");
    return h;
}

// ---------------------------------------------------------------- Anubis System Health: Read-Only Cleanup Preview
enum class CleanupPreviewStatus { NotScanned, Scanning, Clean, CleanupAvailable, PartialScan, ScanFailed };
inline const wchar_t* CleanupPreviewLabel(CleanupPreviewStatus s) {
    switch (s) {
        case CleanupPreviewStatus::Scanning: return L"Scanning";
        case CleanupPreviewStatus::Clean: return L"Clean";
        case CleanupPreviewStatus::CleanupAvailable: return L"Cleanup Available";
        case CleanupPreviewStatus::PartialScan: return L"Partial Scan";
        case CleanupPreviewStatus::ScanFailed: return L"Scan Failed";
        default: return L"Not Scanned";
    }
}
struct CleanupCategoryEstimate {
    std::wstring id, title, category;
    uint64_t bytes = 0;
    uint64_t inaccessibleDirs = 0;
    bool ok = true;
};
struct CleanupScanReport {
    bool completed = false;
    bool cancelled = false;
    uint64_t totalBytes = 0;
    size_t categoriesWithData = 0;
    size_t scannedItems = 0;
    uint64_t inaccessibleDirs = 0;
    std::vector<CleanupCategoryEstimate> items;
    std::vector<std::wstring> warnings;
    std::wstring error;
    CleanupPreviewStatus status = CleanupPreviewStatus::NotScanned;
};
inline void ClassifyCleanupScan(CleanupScanReport& r) {
    if (r.cancelled) { r.completed = false; r.status = CleanupPreviewStatus::NotScanned; return; }
    if (!r.completed || r.scannedItems == 0) { r.completed = false; r.status = CleanupPreviewStatus::ScanFailed; return; }
    if (r.inaccessibleDirs > 0) { r.status = CleanupPreviewStatus::PartialScan; return; }
    r.status = r.totalBytes > 0 ? CleanupPreviewStatus::CleanupAvailable : CleanupPreviewStatus::Clean;
}
inline CleanupScanReport RunCleanupPreviewScan(const std::vector<CleanItem>& items, const std::atomic<bool>* cancel = nullptr) {
    CleanupScanReport r;
    if (items.empty()) { r.error = L"No cleanup categories configured."; ClassifyCleanupScan(r); return r; }
    r.items.reserve(items.size());
    bool browsersHaveData = false;
    for (const auto& it : items) {
        if (cancel && cancel->load()) { r.cancelled = true; ClassifyCleanupScan(r); return r; }
        CleanupCategoryEstimate est{it.id, it.title, it.category, 0, 0, true};
        try {
            if (it.sizeAccess) est.bytes = it.sizeAccess(est.inaccessibleDirs);
            else if (it.size) est.bytes = it.size();
            else est.ok = false;
        } catch (...) {
            est.ok = false;
            r.warnings.push_back(it.title + L": scan failed.");
        }
        if (est.ok) {
            ++r.scannedItems;
            r.totalBytes += est.bytes;
            if (est.bytes > 0) ++r.categoriesWithData;
            r.inaccessibleDirs += est.inaccessibleDirs;
            if (it.id == L"browsers" && est.bytes > 0) browsersHaveData = true;
        }
        r.items.push_back(std::move(est));
    }
    if (r.inaccessibleDirs > 0)
        r.warnings.push_back(std::to_wstring(r.inaccessibleDirs) + L" folder(s) could not be opened (access denied).");
    if (browsersHaveData && BrowsersOpen())
        r.warnings.push_back(L"Close open browsers before cleaning browser caches.");
    r.completed = r.scannedItems > 0;
    if (!r.completed && r.error.empty()) r.error = L"All cleanup categories failed to scan.";
    ClassifyCleanupScan(r);
    return r;
}
inline CleanupScanReport RunCleanupPreviewScan(const std::atomic<bool>* cancel = nullptr) {
    return RunCleanupPreviewScan(CleanItems(), cancel);
}

struct HealthScanBundle {
    SecurityInfo sec;
    DiskInfo disk;
    std::vector<RestorePoint> rps;
    Health health;
    CleanupScanReport cleanup;
};
class HealthScanController {
public:
    bool IsScanning() const { return scanning_.load(); }
    bool Start(HWND notifyWnd, UINT notifyMsg, const Usage& u, const std::vector<CleanItem>* customItems = nullptr) {
        if (scanning_.exchange(true)) return false;   // prevent duplicate concurrent scans
        if (worker_.joinable()) worker_.join();
        cancel_.store(false);
        worker_ = std::thread([this, notifyWnd, notifyMsg, u, customItems] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* out = new HealthScanBundle();
            if (!cancel_.load()) out->sec = GetSecurity();
            if (!cancel_.load()) out->disk = GetSystemDisk();
            if (!cancel_.load()) out->rps = RestorePoints();
            if (!cancel_.load()) out->health = RunHealthScan(u, out->sec, out->disk, out->rps);
            out->cleanup = customItems ? RunCleanupPreviewScan(*customItems, &cancel_) : RunCleanupPreviewScan(&cancel_);
            CoUninitialize();
            const bool delivered = !cancel_.load() && notifyWnd && IsWindow(notifyWnd) &&
                                   PostMessageW(notifyWnd, notifyMsg, 0, reinterpret_cast<LPARAM>(out));
            if (!delivered) delete out;
            scanning_.store(false);
        });
        return true;
    }
    void Shutdown() {
        cancel_.store(true);
        if (worker_.joinable()) worker_.join();
        scanning_.store(false);
    }
    ~HealthScanController() { Shutdown(); }
private:
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
};

// ---------------------------------------------------------------- install helpers
inline bool CreateLaunchTask(const std::wstring& sid) {
    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Author>As3aD</Author><Description>Starts AnkhCore with administrator rights.</Description></RegistrationInfo>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + sid + L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + AppExe() + L"\"</Command><Arguments>--elevated</Arguments><WorkingDirectory>" + AppDir() + L"</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n";
    const std::wstring tmp = DataDir() + L"\\task.xml";
    std::vector<uint8_t> b{0xFF, 0xFE};
    for (wchar_t c : xml) { b.push_back(static_cast<uint8_t>(c)); b.push_back(static_cast<uint8_t>(c >> 8)); }
    EnsureDir(DataDir());
    WriteAll(tmp, b.data(), b.size());
    const DWORD rc = Run(SysDir() + L"\\schtasks.exe", L"/Create /TN \"AnkhCore\\Launch\" /XML \"" + tmp + L"\" /F", true, 30000);
    DeleteFileW(tmp.c_str());
    return rc == 0;
}
inline bool RunLaunchTask() { return Run(SysDir() + L"\\schtasks.exe", L"/Run /TN \"AnkhCore\\Launch\"", true, 15000) == 0; }
inline bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& args, const std::wstring& desc) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* sl = nullptr; bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(target.c_str()); sl->SetArguments(args.c_str()); sl->SetDescription(desc.c_str());
        sl->SetIconLocation(target.c_str(), 0); sl->SetWorkingDirectory(DirOf(target).c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
        sl->Release();
    }
    return ok;
}
// The test build writes the entry under its own HKCU key (the setup sandbox), never to HKLM.
inline std::wstring UninstallKey() { return kTestBuild ? std::wstring(kPrefKey) + L"\\Sandbox\\Uninstall\\AnkhCore" : L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AnkhCore"; }
inline HKEY UninstallHive() { return kTestBuild ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
inline void WriteUninstallEntry(uint64_t sizeBytes) {
    const HKEY LM = UninstallHive(); const std::wstring k = UninstallKey();
    RegSetStr(LM, k, L"DisplayName", std::wstring(kProductFull) + L" " + kVersionLabel);
    RegSetStr(LM, k, L"DisplayVersion", kVersion);
    RegSetStr(LM, k, L"Publisher", L"As3aD");
    RegSetStr(LM, k, L"DisplayIcon", AppExe());
    RegSetStr(LM, k, L"InstallLocation", AppDir());
    RegSetStr(LM, k, L"URLInfoAbout", kLinks[0].url);
    RegSetStr(LM, k, L"UninstallString", L"\"" + AppExe() + L"\" --uninstall");
    RegSetDword(LM, k, L"NoModify", 1); RegSetDword(LM, k, L"NoRepair", 1);
    RegSetDword(LM, k, L"EstimatedSize", static_cast<DWORD>(sizeBytes / 1024));
}
// What an uninstall removes, in one place: the uninstaller (AnkhCore.exe --uninstall) runs it, and the setup's
// sandbox test runs the same code against a sandbox install. Not removed: the data folder (logs, backups of
// earlier versions, original icons, network totals) and the user's settings, so a rollback stays possible.
// The test build never touches tasks, shortcuts or HKLM (it never creates them), and deletes at once
// instead of scheduling anything for the next restart.
// SEC-3: every part is checked afterwards and the report names what is still there.
inline RemoveReport RemoveInstall() {
    RemoveReport rep;
    if (!kTestBuild) {
        for (const wchar_t* task : {L"AnkhCore\\Launch", L"AnkhCore\\Startup"}) {
            Run(SysDir() + L"\\schtasks.exe", L"/Delete /TN \"" + std::wstring(task) + L"\" /F", true, 15000);
            // /Delete also fails for a task that was never there; the query decides (0 = the task still exists)
            if (Run(SysDir() + L"\\schtasks.exe", L"/Query /TN \"" + std::wstring(task) + L"\"", true, 15000) == 0) rep.failed.push_back(std::wstring(L"the scheduled task \\") + task);
        }
        for (const std::wstring& lnk : {KnownFolder(FOLDERID_PublicDesktop) + L"\\AnkhCore.lnk", KnownFolder(FOLDERID_Desktop) + L"\\AnkhCore.lnk", KnownFolder(FOLDERID_CommonPrograms) + L"\\AnkhCore.lnk"}) {
            DeleteFileW(lnk.c_str());
            if (FileExists(lnk)) rep.failed.push_back(L"the shortcut " + lnk);
        }
        if (!RegDel(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", kRunOnceValue)) rep.failed.push_back(L"a theme start entry (RunOnce " + std::wstring(kRunOnceValue) + L")");
    }
    const std::wstring key = UninstallKey(); const size_t cut = key.find_last_of(L'\\');
    HKEY k;
    if (cut != std::wstring::npos && RegOpenKeyExW(UninstallHive(), key.substr(0, cut).c_str(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) { RegDeleteTreeW(k, key.substr(cut + 1).c_str()); RegCloseKey(k); }
    if (RegKeyExists(UninstallHive(), key)) rep.failed.push_back(L"the entry in Settings > Apps");
    if (!kTestBuild || InTestArea(AppDir())) Evict(AppDir(), true, !kTestBuild);   // production: this exe runs from the folder, so the rest goes at the next restart
    if (DirExists(AppDir())) {
        if (kTestBuild) rep.failed.push_back(L"the program folder " + AppDir());   // the test build deletes at once: anything left could not be deleted
        else rep.deferred = true;
    }
    for (const auto& f : rep.failed) LogE(L"Uninstall: not removed: " + f);
    return rep;
}

// ---------------------------------------------------------------- Driver Manager (SYS-4, read-only)
struct DriverEntry {
    std::wstring deviceName, provider, version, date, devClass, infName;
    bool isSigned = false;
};
inline bool DriverFilterMatches(const DriverEntry& d, int filter) {
    if (filter == 1) return _wcsicmp(d.provider.c_str(), L"Microsoft") != 0 && !d.provider.empty();
    if (filter == 2) return !d.isSigned;
    return true;
}
inline std::vector<DriverEntry> EnumSignedDrivers() {
    std::vector<DriverEntry> out;
    for (auto& r : WmiQuery(L"ROOT\\CIMV2", L"SELECT DeviceName, DriverProviderName, DriverVersion, DriverDate, DeviceClass, InfName, IsSigned FROM Win32_PnPSignedDriver",
                            {L"DeviceName", L"DriverProviderName", L"DriverVersion", L"DriverDate", L"DeviceClass", L"InfName", L"IsSigned"})) {
        if (r[L"DeviceName"].empty()) continue;
        DriverEntry d;
        d.deviceName = r[L"DeviceName"];
        d.provider = r[L"DriverProviderName"];
        d.version = r[L"DriverVersion"];
        const std::wstring rawDate = r[L"DriverDate"];
        if (rawDate.size() >= 8) d.date = rawDate.substr(0, 4) + L"-" + rawDate.substr(4, 2) + L"-" + rawDate.substr(6, 2);
        d.devClass = r[L"DeviceClass"];
        d.infName = r[L"InfName"];
        d.isSigned = _wcsicmp(r[L"IsSigned"].c_str(), L"true") == 0 || r[L"IsSigned"] == L"1";
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const DriverEntry& a, const DriverEntry& b) { return a.deviceName < b.deviceName; });
    return out;
}

// ---------------------------------------------------------------- Storage Analyzer (SYS-5, read-only)
struct StorageScanItem { std::wstring label, path; uint64_t bytes = 0; uint64_t files = 0; };
inline uint64_t FolderSizeBytes(const std::wstring& dir, uint64_t& filesOut, int maxDepth = 4, const std::atomic<bool>* cancel = nullptr) {
    if (maxDepth < 0 || !DirExists(dir)) return 0;
    if (cancel && cancel->load(std::memory_order_relaxed)) return 0;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += FolderSizeBytes(dir + L"\\" + fd.cFileName, filesOut, maxDepth - 1, cancel);
        } else {
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            ++filesOut;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}
inline std::vector<StorageScanItem> AnalyzeStorageRoots(const std::wstring& userTemp, const std::wstring& localAppData, const std::wstring& downloads, const std::wstring& ankhData, const std::atomic<bool>* cancel = nullptr) {
    std::vector<StorageScanItem> out;
    auto add = [&](const wchar_t* label, const std::wstring& path) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return;
        if (path.empty() || !DirExists(path)) return;
        StorageScanItem it; it.label = label; it.path = path;
        it.bytes = FolderSizeBytes(path, it.files, 3, cancel);
        out.push_back(std::move(it));
    };
    add(L"User Temp", userTemp);
    add(L"Downloads", downloads);
    add(L"Local AppData (top 3 levels)", localAppData);
    add(L"AnkhCore Data & Recovery", ankhData);
    std::sort(out.begin(), out.end(), [](const StorageScanItem& a, const StorageScanItem& b) { return a.bytes > b.bytes; });
    return out;
}

// Thread-safe async controller for Storage Analyzer (C2): prevents duplicate scans,
// supports cancellation/shutdown, and safely marshals results back to the UI thread.
struct StorageScanController {
    struct State {
        bool scanned = false;
        bool scanning = false;
        bool failed = false;
        std::wstring error;
        std::vector<StorageScanItem> items;
    };

    mutable std::mutex mx;
    std::vector<StorageScanItem> items;
    bool scanned = false;
    bool scanning = false;
    bool failed = false;
    std::wstring error;
    uint64_t generation = 0;
    std::atomic<bool> cancelFlag{false};
    HANDLE worker = nullptr;

    // Optional test seam: overrides AnalyzeStorageRoots when set.
    std::function<std::vector<StorageScanItem>(const std::atomic<bool>&)> scanFn;

    State Snapshot() const {
        std::lock_guard<std::mutex> lk(mx);
        return {scanned, scanning, failed, error, items};
    }

    bool StartScan(const std::wstring& userTemp, const std::wstring& localAppData,
                   const std::wstring& downloads, const std::wstring& ankhData,
                   std::function<void()> onDone) {
        uint64_t myGen = 0;
        {
            std::lock_guard<std::mutex> lk(mx);
            if (scanning) return false;   // prevent duplicate concurrent scans
            if (worker) {
                if (WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) { CloseHandle(worker); worker = nullptr; }
                else return false;
            }
            scanning = true;
            failed = false;
            error.clear();
            cancelFlag.store(false);
            myGen = ++generation;
        }
        struct Ctx {
            StorageScanController* self;
            std::wstring tmp, local, dl, data;
            uint64_t gen;
            std::function<void()> onDone;
        };
        auto* ctx = new Ctx{this, userTemp, localAppData, downloads, ankhData, myGen, std::move(onDone)};
        HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            std::unique_ptr<Ctx> c(static_cast<Ctx*>(p));
            std::vector<StorageScanItem> res;
            bool ok = true;
            std::wstring err;
            try {
                res = c->self->scanFn
                    ? c->self->scanFn(c->self->cancelFlag)
                    : AnalyzeStorageRoots(c->tmp, c->local, c->dl, c->data, &c->self->cancelFlag);
            } catch (const std::exception& e) {
                ok = false;
                err = Utf8ToW(e.what());
            } catch (...) {
                ok = false;
                err = L"Storage scan failed unexpectedly.";
            }
            const bool cancelled = c->self->cancelFlag.load();
            {
                std::lock_guard<std::mutex> lk(c->self->mx);
                c->self->scanning = false;
                if (!cancelled && c->self->generation == c->gen) {
                    if (ok) {
                        c->self->items = std::move(res);
                        c->self->scanned = true;
                        c->self->failed = false;
                        c->self->error.clear();
                    } else {
                        c->self->failed = true;
                        c->self->error = err.empty() ? L"Storage scan failed." : err;
                    }
                }
            }
            if (!cancelled && c->onDone) c->onDone();
            return 0;
        }, ctx, 0, nullptr);
        if (th) {
            std::lock_guard<std::mutex> lk(mx);
            worker = th;
            return true;
        }
        delete ctx;
        std::lock_guard<std::mutex> lk(mx);
        scanning = false;
        failed = true;
        error = L"Could not start storage scan worker.";
        return false;
    }

    void Cancel() {
        std::lock_guard<std::mutex> lk(mx);
        cancelFlag.store(true);
        ++generation;
    }

    void Shutdown() {
        Cancel();
        HANDLE th = nullptr;
        {
            std::lock_guard<std::mutex> lk(mx);
            th = worker;
            worker = nullptr;
        }
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    }
};

}  // namespace ak
