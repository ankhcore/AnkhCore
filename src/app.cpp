// AnkhCore 2.2 — Windows Control Center
// Developer: As3aD
//
// Dashboard, Appearance, Windhawk, Tweaks, Optimize & Repair, Recovery,
// Activity, About. Every change goes through one operation at a time
// (Safety engine), is logged (Activity) and recorded for undo (Recovery).
#include "base.h"
#include "inflate.h"
#include "eng_theme.h"
#include "eng_cursors.h"
#include "eng_icons.h"
#include "eng_system.h"
#include "eng_smart.h"
#include "eng_sensors.h"
#include "eng_fan.h"
#include "eng_apps.h"
#include "eng_apps_manager.h"
#include "eng_syscare.h"
#include "eng_fansupport.h"
#include "eng_procmgr.h"
#include "eng_powertoys.h"
#include <psapi.h>
#include "eng_backup.h"
#include "eng_updater.h"
#include "eng_ugsession.h"
#include "eng_updui.h"
#include "eng_proglist.h"
#include "eng_hwintel.h"
#include "eng_devprofile.h"
#include "ui.h"
#include "candy_bg.h"
#include "icon_status.h"
#include "page_info.h"
#include "eng_windhawk.h"
#include "journal_view.h"
#include "toast_queue.h"
#include "thumb_cache.h"
#include "components.h"
#include "startup.h"
#include "monitor_store.h"
#include "mini_layout.h"
#include "sidebar_layout.h"
#include <wtsapi32.h>
#include "sidebar_lifecycle.h"
#include "nav_layout.h"
#include "nav_model.h"
#include "engine_process.h"
#include "material.h"
#include "appearance_profile.h"
#include "eng_wua.h"
#include "eng_wua_com.h"
#include "frames.h"
#include <limits>
#include <shellapi.h>
#include <mutex>
#ifdef ANKH_TEST_BUILD
#include <psapi.h>
#endif

using namespace ak;

namespace {

enum : UINT { WM_OPDONE = WM_APP + 10, WM_LOGLINE = WM_APP + 11, WM_REFRESHED = WM_APP + 12, WM_THUMBS = WM_APP + 13, WM_TRAY = WM_APP + 14, WM_ICONUI = WM_APP + 15, WM_HEALTHSCAN = WM_APP + 16 };
Readings g_read;            // latest hardware readings (refreshed each second)
int g_fanPercent = 40;      // manual fan slider position
FanOperatingMode g_fanMode = FanOperatingMode::Automatic;
int g_fanFloorPercent = 50; // minimum fan speed floor (%) for AutoFloor mode
std::wstring g_fanNbfcConfig; // last user-confirmed NBFC config name
int g_fanLastFloorSent = -1;
ULONGLONG g_fanLastFloorTick = 0;
FanChannelCalibration g_fanCal{};
int g_betaDaysRemaining = kBetaTrialDays;

void SaveFanPolicyCfg() {
    if (g_fanMode != FanOperatingMode::Automatic) {
        Cfg().SetInt(L"fan.lastActiveMode", static_cast<int>(g_fanMode));
    }
    Cfg().SetInt(L"fan.mode", static_cast<int>(g_fanMode));
    Cfg().SetInt(L"fan.manualPct", ClampFanDuty40(g_fanPercent));
    Cfg().SetInt(L"fan.minFloorPct", ClampFanFloor(g_fanFloorPercent));
    Cfg().SetStr(L"fan.nbfcConfig", g_fanNbfcConfig);
    Cfg().SetInt(L"fan.calCpuOffset", std::clamp(g_fanCal.cpuOffsetPct, -20, 20));
    Cfg().SetInt(L"fan.calGpuOffset", std::clamp(g_fanCal.gpuOffsetPct, -20, 20));
    Cfg().Save();
}
FanOperatingMode g_fanLastActiveMode = FanOperatingMode::AutoFloor;
void LoadFanPolicyCfg() {
    g_fanMode = static_cast<FanOperatingMode>(Cfg().Int(L"fan.mode", 0, 0, 2));
    const int lastAct = Cfg().Int(L"fan.lastActiveMode", static_cast<int>(FanOperatingMode::AutoFloor), 1, 2);
    g_fanLastActiveMode = (g_fanMode != FanOperatingMode::Automatic) ? g_fanMode : static_cast<FanOperatingMode>(lastAct);
    g_fanPercent = ClampFanDuty40(Cfg().Int(L"fan.manualPct", 70, kMinFanDutyPct, 100));
    g_fanFloorPercent = ClampFanFloor(Cfg().Int(L"fan.minFloorPct", 70, kMinFanDutyPct, 100));
    g_fanNbfcConfig = Cfg().Str(L"fan.nbfcConfig", L"");
    g_fanCal.cpuOffsetPct = std::clamp(Cfg().Int(L"fan.calCpuOffset", 0, -20, 20), -20, 20);
    g_fanCal.gpuOffsetPct = std::clamp(Cfg().Int(L"fan.calGpuOffset", 0, -20, 20), -20, 20);
    g_fanLastFloorSent = -1;
}
void RestoreSafeFanOnExit();
void FanAutoFloorTick(const Readings& r);
bool g_sidebarOn = false, g_miniOn = false, g_quitting = false;
HWND g_sidebar = nullptr, g_mini = nullptr;
HWND g_sbHost = nullptr;   // Desktop mode: the desktop window the Sidebar is a child of, or null (top-level)
// persist=false: closing at exit keeps "<widget>.enabled", so the widget comes back next start
void ToggleSidebar(bool on, bool persist = true);
void SbSetOption(const std::wstring& key, int value);            // sidebar.* setting -> persisted and applied live
void SbSetSections(const std::vector<ak::SbSecItem>& items);     // section order and visibility -> persisted and applied live
void SbSetStr(const std::wstring& key, const std::wstring& value);   // a text sidebar.* setting -> persisted and applied live
void SbApplyProfile(const std::wstring& text);                   // a saved look (sidebar_layout.h) -> settings, applied live
bool SbRecoveryFailed();                                           // the Sidebar is meant to be on, is gone and was not restored automatically (sidebar_lifecycle.h)
void MiniToCorner(int corner);                                   // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right of its screen
std::wstring SbCurrentProfile();                                 // the current look as one line of text
std::vector<std::pair<std::wstring, std::wstring>> SbMonitorChoices();   // (device, label)
void ToggleMini(bool on, bool persist = true);
void MiniSetOption(const std::wstring& key, int value);          // mini.* setting -> persisted and applied live
// Snapshot sequence: the 1 s timer bumps it after taking g_read/g_usage; each view records the
// sequence it last drew from, which proves every widget shows the same snapshot.
uint64_t g_snapSeq = 0, g_sbSeq = 0, g_miniSeq = 0;
const wchar_t* kPageName[PageCount] = {L"Dashboard", L"Monitor", L"Fan Control", L"Appearance & Themes", L"Windhawk", L"Tweaks", L"Optimize & Repair", L"Apps Manager", L"Recovery", L"Activity", L"About", L"Startup & Background", L"Sidebar & Mini Monitor",
    L"Sidebar: Layout", L"Sidebar: Appearance", L"Sidebar: Widgets", L"Sidebar: Behavior", L"Sidebar: Profiles", L"Mini Monitor", L"Monitoring Preferences",
    L"AnkhCore Appearance", L"Windows Theme Manager", L"Wallpaper Manager", L"Cursor Manager", L"Icon Manager",
    L"Installed Applications", L"Windows Built-in Apps", L"Uninstall Manager",
    L"Backup & Migration", L"Recovery History",
    L"Driver Manager", L"Storage Analyzer",
    L"Hardware Information", L"Network Monitor", L"Error Logs", L"Settings", L"App Updates", L"System Care", L"Process Manager", L"PowerToys"};
const wchar_t* kPageSub[PageCount] = {
    L"Live status of your PC, security and health.",
    L"Live hardware readings, hardware information, network and fan status.",
    L"Fan speed and temperature, with a return to automatic at any time.",
    L"Windows customization: theme, wallpapers, mouse cursors and icon packs.",
    L"Turn each Windhawk mod on or off.",
    L"Windows services, performance settings and startup apps.",
    L"Free up space and repair Windows.",
    L"Uninstall desktop programs and Microsoft Store apps.",
    L"Restore points, portable .acbackup archives and every change AnkhCore made.",
    L"Everything AnkhCore does, as it happens.",
    L"AnkhCore by As3aD.",
    L"Start with Windows, start in the tray, and the background monitoring engine.",
    L"The desktop sidebar and the mini monitor: layout, look, widgets, behavior and profiles, with the shared monitoring preferences.",
    L"Which sections show, in what order and in how much detail.",
    L"Background material, opacity, tint, colours and glow.",
    L"Indicators, gauges, numeric values or graphs; text size, spacing and animation.",
    L"Layer, screen, edge and size; overlay options under Advanced.",
    L"Built-in looks, your own saved looks, and reset.",
    L"Content, size, place and look of the small floating readout.",
    L"Sampling interval, history, alert thresholds, network adapter and reset.",
    L"Window material, tint, battery mode, blur, glow, motion and widget glass.",
    L"As3aD dark visual style, theme patch check and repair, and ThemeTool.",
    L"Anubis desktop wallpaper slideshow, timing, shuffle and fixed pictures.",
    L"Your cursor packs: preview, apply, restore the previous scheme or the Windows default.",
    L"Prebuilt icon themes via Windhawk Resource Redirect 1.3 and icon recovery.",
    L"Desktop programs installed on this PC and their native uninstallers.",
    L"Microsoft Store and built-in Windows packages.",
    L"Select several programs, uninstall them one after another, then clean what they left behind (undoable).",
    L"Portable .acbackup archives, settings merge, recovery points and UniGetUI bundles.",
    L"Recovery points created before restores or major changes, and the undo journal.",
    L"Signed device drivers installed on this PC (read-only inventory and export).",
    L"Fixed-drive capacity and user/temporary folder disk usage breakdown.",
    L"What this PC is made of: system, processor, graphics, memory, drives, network adapters and fans (read-only).",
    L"Live speed, history, totals and state of every network adapter.",
    L"Warnings and errors AnkhCore recorded, from the saved log file.",
    L"AnkhCore's own preferences. Windows customization is in Windows Studio.",
    L"Programs with a newer version available (through your portable UniGetUI), updated one at a time with a verified recovery point first.",
    L"Guided scan, review, approval, cleaning and verification: temporary files, caches, crash dumps, broken shortcuts. Read-only until you approve.",
    L"See every process, change its priority, CPU cores and efficiency mode, end an ordinary application, and keep rules that apply automatically. Critical and security processes are never touched.",
    L"Microsoft PowerToys, installed with AnkhCore: switch FancyZones, Always On Top, Awake, Color Picker, Keyboard Manager, PowerToys Run and more on or off from here."};
const wchar_t* kPageIcon[PageCount] = {L"\xE80F", L"\xE9D2", L"\xE9CA", L"\xE771", L"\xE74C", L"\xE713", L"\xE90F", L"\xE71D", L"\xE81C", L"\xE8FD", L"\xE946", L"\xE7E8", L"\xE9D9",
    L"\xE8FD", L"\xE771", L"\xE9D2", L"\xE713", L"\xE81C", L"\xE78B", L"\xE713",
    L"\xE771", L"\xE790", L"\xE7F4", L"\xEA86", L"\xE74C",
    L"\xE71D", L"\xE8FD", L"\xE74D",
    L"\xE81C", L"\xE8FD",
    L"\xE964", L"\xEDA2",
    L"\xE950", L"\xE839", L"\xE783",
    L"\xE713", L"\xE895", L"\xE73E", L"\xE950", L"\xE8B0"};

// 2.8: forward declarations (defined further down): the one scan page, the updater model and Update all
void NavOpenSystemCareScan();
UpdUiModel& UpdUi();
void UpdateAllPrograms();
std::vector<UpdListedPackage> UpdVisible(const UpdUiSnapshot& us);
void CareCleanSelected();   // 2.8.3: the System Care action bar and the button under the results share these
void CareUpdateAndFixAll();
int PinnedBarH();   // 2.8.3: the update list without the skipped programs
constexpr int kW = 1140, kH = 740, kSide = 236, kHeader = 92, kFooter = 52;

// ---------------------------------------------------------------- accordion navigation (2.5)
// Parent sections hold one or more pages. A one-page section is a plain item; a section with
// several pages expands (one at a time). Pages keep their Page*() functions; new child pages are
// added here without touching the rest of the shell.
constexpr int kNavTop = 136, kNavRowH = 42, kNavStep = 48, kNavChildH = 34, kNavChildStep = 38;
struct NavRow { RECT r; int section; int page; bool child; };
int g_navOpen = -1;                 // expanded section, -1 = none
std::vector<float> g_navAnim(16, 0.f);
bool g_navKeyboard = false;         // draw the keyboard focus ring
int g_navScroll = 0;
const wchar_t* NavChildName(int page) { return page == PMonitor ? L"System Monitor" : page == PActivity ? L"Operation History" : kPageName[page]; }

// ---------------------------------------------------------------- cached system state
struct Cache {
    WinVer ver; bool themeActive = false, patchLoaded = false, cursorsOn = false, wallOn = false;
    IconState icons; bool whInstalled = false, whRunning = false; std::vector<WhMod> mods;
    std::vector<int> svc;   // -1 missing, 0 off, 1 on
    std::vector<bool> tweak; std::vector<StartupItem> startup;
    SecurityInfo sec; DiskInfo disk; std::vector<RestorePoint> rps; bool deepDone = false;
    bool startupOn = false, startupChecked = false;   // read back from Task Scheduler
    Health health; bool healthDone = false;
    CleanupScanReport cleanupScan; bool cleanupScanFresh = false;
    std::shared_ptr<const std::vector<JEntry>> journal; std::shared_ptr<const JournalView> journalView;   // shared: a Snapshot() copy no longer copies the whole journal (FT-RH)
    ThemeCheck themeCheck; bool themeChecked = false;
};
CRITICAL_SECTION g_cs;
Cache g_cache;
Cache Snapshot() { EnterCriticalSection(&g_cs); Cache c = g_cache; LeaveCriticalSection(&g_cs); return c; }

HWND g_wnd;
Ui g_ui;
Logo g_logo;
Px g_brandBg, g_brandLogoRail, g_brandLogoAbout, g_brandLogoCompact;   // background.png (D44) and the unified logo.png (2026-11-14): scaled; the logo with BrandLogoSoften
HICON g_ankhBig = nullptr;
int g_page = PDashboard;
int g_scroll[PageCount] = {}, g_contentH[PageCount] = {};
bool PageSbGeom(RECT& thumb, int& viewH, int& th, int& range);
bool g_pageSbDrag = false; int g_pageSbGrabY = 0;   // dragging the page's scrollbar thumb (grab offset inside the thumb)
std::atomic<bool> g_busy{false};
std::atomic<bool> g_opBackground{false};   // 2.10: a long program update runs in the background; the rest of the window stays usable (a second operation still waits: RunOp)
std::atomic<uint64_t> g_opSeq{0};   // the running operation (one at a time); toasts for it carry this as their key
std::atomic<int> g_progress{-1};
std::wstring g_opName, g_opLine; CRITICAL_SECTION g_opCs;
int g_lastResult = 0;   // 0 none, 1 ok, 2 failed
DWORD g_resultAt = 0;
bool g_restartNeeded = false;
enum : UINT { WM_ENGINE_FRAME = WM_APP + 77 };
EngineClient g_engine; Usage g_usage;   // the engine samples; the UI keeps the latest published frame
SysExtra g_sys;                                                  // per-core usage, memory detail, drives: same frame
std::vector<GpuAdapterView> g_gpus;                              // every graphics adapter with its utilization: same frame
double g_sbGpuDrawn = -2;                                        // the GPU utilization the Sidebar last drew (diagnostics, self-test)
SbLayout g_sbLayout; int g_sbAvail = 0;                          // what the Sidebar last drew, and the height it had for it (DIPs)
int g_sbEditSec = 0, g_sbEditColor = 0;                          // settings page: the section and the colour being edited
// The Sidebar's own background follows the older shared "wallpaper glass" switch until it is chosen explicitly.
int SbBackgroundCfg() { return Cfg().Has(L"sidebar.background") ? Cfg().Int(L"sidebar.background", 2, 0, 2) : 2; }   // 2.8: Acrylic blur by default, like the Start menu
int SbTintCfg() { return Cfg().Int(L"sidebar.tint", 50, 0, 100); }
std::vector<NetAdapterView> g_net; NetSelection g_netSel;       // per-adapter network data from the same frame, and what the views show
int NetPeriodCfg() { return Cfg().Int(L"sidebar.netPeriod", 1, 0, 3); }
void NetApply() { g_netSel = NetSelect(g_net, Cfg().Int(L"sidebar.netMode", NetAuto, NetAuto, NetManual), Cfg().Str(L"sidebar.netAdapter", L"")); }
void NetSetMode(int mode, const std::wstring& guid);             // persisted, sent to the engine, applied live
void NetSetPeriod(int period);
const wchar_t* NetPeriodName(int p) { static const wchar_t* n[4] = {L"Session", L"Today", L"Since boot", L"Lifetime"}; return n[p & 3]; }
std::wstring NetRate(double v) { return v < 0 ? std::wstring(L"\u2014") : FormatBytes(static_cast<uint64_t>(v)) + L"/s"; }
// One line for tiles and widgets. These are Windows counters: live whenever the engine is, with or without the sensor helper.
std::wstring NetSpeedText(const wchar_t* gap = L"  ") {
    const NetSelection& n = g_netSel;
    if (!n.valid) return L"no data";
    if (!n.connected) return L"disconnected";
    return L"\u2193 " + NetRate(n.down) + gap + L"\u2191 " + NetRate(n.up);
}
std::vector<double> g_cpuHist, g_ramHist;
MonitorStore g_store;                                          // history for the Desktop Monitoring graphs
AlertEngine g_alerts;
std::vector<std::pair<std::wstring, std::wstring>> g_alertLog;   // (time, text), newest last, bounded
std::wstring g_selPack = L"bigsur-dark";
std::vector<bool> g_cleanSel; std::vector<uint64_t> g_cleanSize; bool g_cleanScanned = false;
HealthScanController g_healthWorker;
std::vector<Image32> g_thumbs; std::vector<std::wstring> g_thumbPaths;
std::map<std::wstring, Image32> g_previews;
// FT-2 / A04: the Icon Manager reads these two caches only; both are filled on worker threads (src/icon_status.h, src/thumb_cache.h)
IconStatusCache g_iconStatus; ThumbService g_iconThumbs;
// Candy presentation (2.5): the brand background behind the Candy preview. Settings candy.bgOpacity / candy.bgBlur act on the background only (src/candy_bg.h).
int CandyOpacityCfg() { return CandyClampOpacity(Cfg().Int(L"candy.bgOpacity", kCandyOpacityDefault, -100000, 100000)); }   // a hand-edited value is clamped, not dropped
int CandyBlurCfg() { return CandyClampBlur(Cfg().Int(L"candy.bgBlur", kCandyBlurDefault, -100000, 100000)); }
void CandyBgReset() { Cfg().SetInt(L"candy.bgOpacity", kCandyOpacityDefault); Cfg().SetInt(L"candy.bgBlur", kCandyBlurDefault); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }
CandyBgCache g_candyBg([](int w, int h) {   // the owner's picture scaled once per box size (Fant filter), aspect kept, centre-cropped: never stretched
    std::vector<uint32_t> out; Image32 im = LoadImageRes(L"BRANDBG", w, h, true);
    if (im.bmp) { Px px = ToPixels(im); DeleteObject(im.bmp); if (px.w == w && px.h == h) out = std::move(px.p); }
    return out; });
// Window background (owner 2026-11-16): brand_background.png is the main window's background; its opacity and blur are settings (Appearance), acting on the picture only.
// The default opacity is the strength it had before (27 % ~ 0.266), no blur; the owner tunes them and the final values become the defaults.
// 2.9.5: the tint default and its lowest value follow the 20 % more transparent glass (55 -> 44, 45 -> 36). Stored values of existing users stay as they are.
constexpr int kTintDefault = 44, kTintMin = 36;
constexpr int kWinBgOpacityDefault = 0, kWinBgBlurDefault = 0;   // 2.8: no picture behind the glass by default (the desktop shows through, as in Explorer)
int WinBgOpacityCfg() { return std::clamp(Cfg().Int(L"appearance.bgOpacity", kWinBgOpacityDefault, -100000, 100000), 0, 100); }
int WinBgBlurCfg() { return std::clamp(Cfg().Int(L"appearance.bgBlur", kWinBgBlurDefault, -100000, 100000), 0, kCandyBlurMax); }
Px g_brandBgBlur; int g_brandBgBlurPx = -1;   // the blurred copy for the current blur value: rebuilt only when that value changes, never per frame
void WinBgReset() { Cfg().SetInt(L"appearance.bgOpacity", kWinBgOpacityDefault); Cfg().SetInt(L"appearance.bgBlur", kWinBgBlurDefault); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }
std::wstring g_iconApplyId; std::atomic<int> g_iconApplyResult{0};   // FT-A02: the theme the last Apply from the Icon Manager was for, and how it ended (0 running or none, 1 applied, 2 failed)
std::atomic<bool> g_hasLegacyIconBackups{false};
void IconPageEnter() {   // opening the page (and an operation finishing): refresh the installed state, re-check the cached previews
    g_hasLegacyIconBackups.store(!BackedUpTargets().empty());
    g_iconStatus.RefreshAsync([] { if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
    g_iconThumbs.Revalidate();
}
// FT-C01 / FT-C02: the Windhawk page stages changes (switches and settings) and applies them together. The mutex protects the
// stage and the loaded schema: workers only assign results, the page reads while it draws.
std::mutex g_whMx; WhStage g_whStage;
std::string g_whSel, g_whGroup;                 // the mod whose settings are open, and the section shown
int g_whListIdx = 0; bool g_whShowOwnPool = false; // selected item in a structured list (e.g. per-program profiles)
std::vector<WhSettingDef> g_whDefs; WhFlat g_whFlat; std::wstring g_whLoadErr, g_whSummary;
std::atomic<bool> g_whLoading{false}; bool g_whExplorerOffer = false, g_whSummaryOk = true;
void WhLoadAsync(const std::string& id) {   // the CLI is a process: read the schema and the values off the UI thread
    { std::lock_guard<std::mutex> lk(g_whMx); g_whSel = id; g_whGroup.clear(); g_whListIdx = 0; g_whShowOwnPool = false; g_whDefs.clear(); g_whFlat.clear(); g_whLoadErr.clear(); }
    g_whLoading = true;
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::unique_ptr<std::string> idp(static_cast<std::string*>(p)); const std::string id = *idp;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        JVal data; std::wstring err, e2; WhFlat flat; std::vector<WhSettingDef> defs;
        bool ok = WhCliShow(id, data, err) && WhParseSchema(data, defs, e2) && WhCliSettings(id, flat, err);
        if (ok && !e2.empty() && defs.empty()) err = e2;   // "the mod declares no settings"
        { std::lock_guard<std::mutex> lk(g_whMx);
          if (g_whSel == id) { if (ok) { g_whDefs = std::move(defs); g_whFlat = std::move(flat); g_whLoadErr = g_whDefs.empty() ? (e2.empty() ? L"This mod declares no settings." : e2) : L""; } else g_whLoadErr = err; } }
        g_whLoading = false;
        if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0);
        return 0; }, new std::string(id), 0, nullptr));
}
bool g_logFollow = true;

void SetOpLine(const std::wstring& s) { EnterCriticalSection(&g_opCs); g_opLine = s; LeaveCriticalSection(&g_opCs); }
std::wstring OpLine() { EnterCriticalSection(&g_opCs); auto s = g_opLine; LeaveCriticalSection(&g_opCs); return s; }

// ---------------------------------------------------------------- refresh (background)
void RefreshState(bool deep) {
    Cache c = Snapshot();
    c.ver = GetWinVer();
    c.themeActive = ThemeIsActive(); c.patchLoaded = PatchLoaded();
    c.cursorsOn = CursorsActive(); c.wallOn = WallpaperActive();
    c.icons = GetIconState();
    c.whInstalled = WindhawkInstalled(); c.whRunning = WindhawkRunning(); c.mods = WindhawkMods();
    c.svc.clear(); for (const auto& d : Services()) { const SvcInfo i = GetSvc(d.name); c.svc.push_back(!i.exists ? -1 : SvcIsOn(d) ? 1 : 0); }
    c.tweak.clear(); for (const auto& t : Tweaks()) c.tweak.push_back(t.isOn());
    c.startup = StartupItems();
    { auto j = std::make_shared<std::vector<JEntry>>(JournalLoad()); c.journalView = std::make_shared<const JournalView>(JournalBuildView(*j)); c.journal = j; }
    c.startupOn = StartupTaskRegistered(); c.startupChecked = true;
    if (deep) { c.sec = GetSecurity(); c.disk = GetSystemDisk(); c.rps = RestorePoints(); c.deepDone = true; }
    EnterCriticalSection(&g_cs); g_cache = c; LeaveCriticalSection(&g_cs);
    PostMessageW(g_wnd, WM_REFRESHED, 0, 0);
}
void RefreshAsync(bool deep) {
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        RefreshState(p != nullptr); return 0; }, deep ? reinterpret_cast<LPVOID>(1) : nullptr, 0, nullptr));
}

// ------- installed-apps inventory (loaded off the UI thread; Store listing is slow)
std::vector<AppEntry> g_apps;
std::mutex g_appsMx;
std::atomic<bool> g_appsLoaded{false}, g_appsLoading{false};
void LoadAppsAsync() {
    if (g_appsLoading.exchange(true)) return;
    g_appsLoaded = false;
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto list = EnumApps(true);
        { std::lock_guard<std::mutex> lk(g_appsMx); g_apps = std::move(list); }
        g_appsLoaded = true; g_appsLoading = false;
        if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
        return 0; }, nullptr, 0, nullptr));
}

// ---------------------------------------------------------------- operations (one at a time)
struct Op { std::wstring name; std::function<bool()> fn; bool restart; bool deep; };
DWORD WINAPI OpThread(LPVOID p) {
    std::unique_ptr<Op> op(static_cast<Op*>(p));
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogI(L"\x25B6 " + op->name);
    bool ok = false;
    try { ok = op->fn(); } catch (...) { ok = false; }
    if (ok) LogOk(L"Done: " + op->name); else LogE(L"Failed: " + op->name);
    RefreshState(op->deep);
    PostMessageW(g_wnd, WM_OPDONE, ok ? 1 : 0, op->restart ? 1 : 0);
    return 0;
}
bool g_selftest = false;   // --selftest: scripted UI walk, no dialogs
// Test build: actions that change the system are refused here, before anything runs.
bool TestGuard(const std::wstring& what) {
    if (!kTestBuild) return false;
    TestRefused(what);
    if (!g_selftest)
        MessageBoxW(g_wnd, (L"Test build: \"" + what + L"\" is disabled.\n\nThis build only shows the interface. It does not change Windows, "
                            L"the installed AnkhCore, fans, icons, themes, cursors, services or tweaks.").c_str(),
                    L"AnkhCore (test build)", MB_OK | MB_ICONINFORMATION);
    return true;
}
void AddToast(const std::wstring& text, COLORREF color, uint64_t key);
bool RunOp(const std::wstring& name, std::function<bool()> fn, bool restart = false, bool deep = false, bool background = false) {
    if (TestGuard(name)) return false;
    // 2.8.7: a modal box here froze the whole window until it was closed; a toast says the same and the window stays usable.
    if (g_busy) { AddToast(L"Please wait: \"" + g_opName + L"\" is still running.", col::warn, 0); return false; }
    g_opBackground = background; g_busy = true; ++g_opSeq; g_opName = name; g_progress = -1; SetOpLine(L"");
    CloseHandle(CreateThread(nullptr, 0, OpThread, new Op{name, std::move(fn), restart, deep}, 0, nullptr));
    InvalidateRect(g_wnd, nullptr, FALSE);
    return true;
}
// Undo through the journal with an honest result (SEC-3): success only when every selected entry was put back.
// Selecting nothing is "nothing to undo" (success for the Undo all buttons, failure for one named entry).
bool UndoOp(const std::wstring& what, const std::function<bool(const JEntry&)>& pick, bool nothingIsFailure = false) {
    const UndoCount u = JournalUndoEx(pick);
    const bool ok = u.Success() && !(nothingIsFailure && u.Nothing());
    (ok ? LogOk : LogE)(what + L": " + u.Text());
    return ok;
}
bool g_surfOn = false;    // true: the window has no GDI surface (WS_EX_NOREDIRECTIONBITMAP) and draws only through g_surf
struct GlassDialog { bool active = false, result = false, danger = false; std::wstring text, verb = L"Continue"; } g_dlg;
DWORD g_uiThread = 0;
bool Confirm(const std::wstring& text, const wchar_t* verb = L"Continue", bool danger = false) {
    if (kTestBuild && g_selftest) { LogI(L"[selftest] auto-confirmed: " + text.substr(0, text.find(L'\n'))); return true; }   // so the guard behind it is exercised
    if (g_surfOn && GetCurrentThreadId() == g_uiThread && IsWindowVisible(g_wnd)) {   // in-app glass dialog, modal through a nested message loop
        g_dlg.active = true; g_dlg.result = false; g_dlg.text = text; g_dlg.verb = verb; g_dlg.danger = danger;
        InvalidateRect(g_wnd, nullptr, FALSE);
        MSG m;
        while (g_dlg.active) {
            const BOOL got = GetMessageW(&m, nullptr, 0, 0);
            if (got <= 0) { PostQuitMessage(static_cast<int>(m.wParam)); g_dlg.active = false; return false; }
            TranslateMessage(&m); DispatchMessageW(&m);
        }
        InvalidateRect(g_wnd, nullptr, FALSE);
        return g_dlg.result;
    }
    return MessageBoxW(g_wnd, text.c_str(), kProductFull, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES; }
// Output lines from console tools: show the latest, and take the percentage the tool itself prints
// (ParseToolPercent: one unambiguous value, chkdsk's total; nothing is made up for lines without one).
LineFn OpLines() {
    return [](const std::wstring& l) {
        SetOpLine(l);
        const int pc = ParseToolPercent(l);
        if (pc != kProgressUnknown) g_progress = pc;
        static std::wstring last;
        if (l != last && l.find(L'%') == std::wstring::npos) { LogI(l); last = l; }
        PostMessageW(g_wnd, WM_LOGLINE, 0, 0);
    };
}

COLORREF Good(bool ok) { return ok ? col::ok : col::err; }
const Image32& Preview(const wchar_t* res, int w, int h) {
    const std::wstring key = std::wstring(res) + L"@" + std::to_wstring(w) + L"x" + std::to_wstring(h);
    auto it = g_previews.find(key);
    if (it == g_previews.end()) it = g_previews.emplace(key, LoadImageRes(res, w, h, true)).first;
    return it->second;
}

// ---------------------------------------------------------------- pages
// Gauges and numbers roll to a new value, but small jitter on the 1 s tick jumps (no animation frames while idle).
Animator g_anim;
uint64_t g_animFrames = 0;
FrameStats g_frameStats; AnimationGovernor g_gov;
struct WidgetFrame { std::vector<uint32_t> px; int w = 0, h = 0; uint64_t seq = 0, drawn = 0; } g_widgetFrame[2];   // 0 Mini, 1 Sidebar: the last frame, for the live previews
const uint64_t kPageFadeId = 0x7001;
void NavGo(int page);
void FindingDo(FindingAct a);   // 2.9.8: the fix, or the page, a health finding offers
std::unordered_map<uint64_t, double> g_rollShown;
double RollValue(uint64_t id, double v) {
    double& shown = g_rollShown.emplace(id, v).first->second;
    if (std::abs(v - shown) >= 4.0) shown = v;
    return g_anim.To(id, static_cast<float>(shown), 300);
}
void DrawRing(Ctx& c, RECT r, double pct, const std::wstring& label, const std::wstring& value, const std::wstring& sub) {
    Round(c.dc, r, c.S(28), col::panel, col::border);
    const int d = c.S(74), cx = r.left + c.S(18), cy = r.top + (r.bottom - r.top - d) / 2;
    GaugeRing(c.dc, cx + d / 2, cy + d / 2, d / 2 - c.S(4), c.S(7), pct < 0 ? pct : RollValue(AnimId(r.left, r.top, 4), pct), pct < 0 ? col::faint : col::accent);
    Text(c.dc, pct < 0 ? L"--" : std::to_wstring(static_cast<int>(pct + 0.5)) + L"%", RECT{cx, cy, cx + d, cy + d}, c.u.f.btn, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const int tx = cx + d + c.S(14);
    Text(c.dc, label, RECT{tx, r.top + c.S(22), r.right - c.S(10), r.top + c.S(42)}, c.u.f.small, col::sub);
    Text(c.dc, value, RECT{tx, r.top + c.S(42), r.right - c.S(10), r.top + c.S(66)}, c.u.f.h2, col::text, DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(c.dc, sub, RECT{tx, r.top + c.S(68), r.right - c.S(10), r.top + c.S(88)}, c.u.f.tiny, col::faint, DT_SINGLELINE | DT_END_ELLIPSIS);
}
COLORREF ToneColor(Tone t) { return t == Tone::Good ? col::ok : t == Tone::Warning ? col::warn : t == Tone::Error ? col::err : t == Tone::Info ? col::accent : col::faint; }
void StatusGrid(Ctx& c, const std::wstring& title, const std::vector<std::tuple<std::wstring, std::wstring, COLORREF>>& rows, int x, int w, int& y) {
    const int h = c.S(46) + static_cast<int>(rows.size()) * c.S(30) + c.S(10);
    RECT box{x, y, x + w, y + h};
    if (c.Visible(box)) {
        Round(c.dc, box, c.S(28), col::panel, col::border);
        Text(c.dc, title, RECT{x + c.S(18), y + c.S(14), x + w, y + c.S(38)}, c.u.f.btn, col::text);
        int ry = y + c.S(46);
        for (const auto& [k, v, color] : rows) {
            DrawDot(c.dc, x + c.S(18), ry + c.S(9), c.S(8), color);
            Text(c.dc, k, RECT{x + c.S(34), ry, x + w / 2, ry + c.S(26)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, v, RECT{x + w / 2 - c.S(30), ry, x + w - c.S(16), ry + c.S(26)}, c.u.f.small, col::text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ry += c.S(30);
        }
    }
    y += h + c.S(14);
}
// ---- Dashboard (redesign §3)
inline bool TempUnitF() { return Cfg().Int(L"monitor.tempUnit", 0, 0, 1) == 1; }
inline const wchar_t* TempSuffix() { return TempUnitF() ? L" \u00B0F" : L" \u00B0C"; }
inline double DispTemp(double c) { return c < 0 ? c : ConvertTempUnit(c, TempUnitF()); }
std::wstring MiniValue(MiniMetric m, bool& dim);
std::wstring NA(double v, const wchar_t* suf, int dec);
std::vector<float> SeriesPoints(const Ring& s, size_t count, double lo, double hi) {   // newest on the right, gaps stay NaN
    std::vector<float> p(count, std::numeric_limits<float>::quiet_NaN());
    const size_t n = s.Size(), have = std::min(n, count);
    for (size_t i = 0; i < have; ++i) { const double v = s.At(n - have + i); if (HasValue(v) && hi > lo) p[count - have + i] = static_cast<float>((v - lo) / (hi - lo)); }
    return p;
}
std::unordered_map<std::string, uint64_t> g_alertFired;   // when each alert last fired (the sustained-heat engine is the only source of Warm / Hot)
std::pair<std::wstring, COLORREF> TempState(bool cpu, double t) {
    const Readings& r = g_read;
    if (!r.present) return {r.everSeen ? L"stale" : L"no data", col::faint};
    if (t < 0) return {L"not exposed", col::faint};
    const uint64_t now = GetTickCount64() / 1000;
    auto on = [&](const char* id) { auto it = g_alertFired.find(id); const AlertRule* ru = g_alerts.Find(id); return it != g_alertFired.end() && ru && now - it->second < 900 && t >= ru->threshold - ru->hysteresis; };
    if (on(cpu ? "cpu.crit" : "gpu.crit")) return {L"Hot", col::err};
    if (on(cpu ? "cpu.warn" : "gpu.warn")) return {L"Warm", col::warn};
    return {L"Normal", col::ok};
}
// Vertical line, a dot per series and a glass readout at the sample under the pointer.
void GraphCrosshair(Ctx& c, const RECT& g, int n, const std::function<std::wstring(int)>& label, std::initializer_list<std::pair<const std::vector<float>*, COLORREF>> series) {
    if (!c.Hover(g) || n < 2) return;
    const int w = g.right - g.left, i = std::clamp(static_cast<int>(((c.u.mouse.x - g.left) * (n - 1) + w / 2) / std::max(1, w)), 0, n - 1), x = g.left + w * i / (n - 1);
    Line(c.dc, x, g.top, x, g.bottom, Mix(col::panel, col::accent, .75));
    for (const auto& [p, cc] : series) { const float v = (*p)[i]; if (v == v) { const int y = g.bottom - 1 - static_cast<int>(std::clamp(v, 0.f, 1.f) * (g.bottom - g.top - 2)), d = c.S(4); Ellipse2(c.dc, RECT{x - d, y - d, x + d, y + d}, cc, RGB(232, 244, 255)); } }
    const std::wstring t = label(i); const int tw = TextWidth(c.dc, t, c.u.f.tiny) + c.S(20), th = c.S(24);
    int bx = x + c.S(10); if (bx + tw > g.right) bx = x - c.S(10) - tw;
    RECT b{bx, g.top + c.S(4), bx + tw, g.top + c.S(4) + th};
    Round(c.dc, b, c.S(16), col::overlay, Mix(col::side, col::accent, .4));
    Text(c.dc, t, b, c.u.f.tiny, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
void MetricTile(Ctx& c, RECT r, const wchar_t* glyph, const wchar_t* label, MiniMetric m, COLORREF color, const std::wstring& chip, COLORREF chipC, double lo, double hi) {
    if (!c.Visible(r)) return;
    bool dim = false; const std::wstring val = MiniValue(m, dim);
    Round(c.dc, r, c.S(28), col::panel, col::border);
    Text(c.dc, glyph, RECT{r.left + c.S(14), r.top + c.S(10), r.left + c.S(38), r.top + c.S(34)}, c.u.f.icon, dim ? col::faint : color, DT_VCENTER | DT_SINGLELINE);
    Text(c.dc, label, RECT{r.left + c.S(38), r.top + c.S(10), r.right - c.S(8), r.top + c.S(34)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(c.dc, val, RECT{r.left + c.S(14), r.top + c.S(36), r.right - c.S(8), r.top + c.S(66)}, c.u.f.h1, dim ? col::faint : col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int sid = MiniSeries(m);
    if (sid >= 0) AreaGraph(c.dc, RECT{r.left + c.S(14), r.top + c.S(70), r.right - c.S(14), r.bottom - c.S(32)}, SeriesPoints(g_store.Get(static_cast<Series>(sid)), 60, lo, hi), color, c.S(2) - 1);
    if (!chip.empty()) Chip(c, r.left + c.S(14), r.bottom - c.S(26), chip, chipC);
}
bool g_monPaused = false;
void PageDashboard(Ctx& c, const Cache& s) {
    const Usage& u = g_usage; const Readings& rd = g_read;
    const int gap = c.S(16);
    const bool wide = c.w >= c.S(800);            // 12-column grid; below that it reflows to two columns
    const int hero = wide ? (c.w - gap) * 8 / 12 : c.w, hh = c.S(246);
    // row 1: System pulse + Health
    {
        RECT r{c.x, c.y, c.x + hero, c.y + hh};
        if (c.Visible(r)) {
            Round(c.dc, r, c.S(28), col::panel, col::border);
            Text(c.dc, L"System pulse", RECT{r.left + c.S(20), r.top + c.S(14), r.right, r.top + c.S(38)}, c.u.f.btn, col::text);
            Text(c.dc, L"Last 60 seconds", RECT{r.left, r.top + c.S(14), r.right - c.S(20), r.top + c.S(38)}, c.u.f.tiny, col::faint, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            const RECT g{r.left + c.S(20), r.top + c.S(44), r.right - c.S(20), r.top + c.S(168)};
            for (int i = 0; i <= 4; ++i) { const int y = g.top + (g.bottom - g.top) * i / 4; Line(c.dc, g.left, y, g.right, y, Mix(col::panel, col::accent, .10)); }
            AreaGraph(c.dc, g, SeriesPoints(g_store.Get(SRam), 60, 0, 100), RGB(120, 110, 255), c.S(2) - 1, false);
            AreaGraph(c.dc, g, SeriesPoints(g_store.Get(SGpuLoad), 60, 0, 100), col::ok, c.S(2) - 1, false);
            AreaGraph(c.dc, g, SeriesPoints(g_store.Get(SCpuLoad), 60, 0, 100), col::accent, c.S(2));
            {   // legend, then the crosshair readout under the pointer
                int lx = r.left + c.S(130);
                for (const auto& [nm, cc] : std::initializer_list<std::pair<const wchar_t*, COLORREF>>{{L"CPU", col::accent}, {L"GPU", col::ok}, {L"RAM", RGB(120, 110, 255)}}) { DrawDot(c.dc, lx, r.top + c.S(22), c.S(7), cc); Text(c.dc, nm, RECT{lx + c.S(11), r.top + c.S(14), lx + c.S(48), r.top + c.S(38)}, c.u.f.tiny, col::sub, DT_VCENTER | DT_SINGLELINE); lx += c.S(52); }
                const auto cpuP = SeriesPoints(g_store.Get(SCpuLoad), 60, 0, 100), gpuP = SeriesPoints(g_store.Get(SGpuLoad), 60, 0, 100), ramP = SeriesPoints(g_store.Get(SRam), 60, 0, 100);
                auto pctTxt = [](float v) { return v != v ? std::wstring(L"gap") : std::to_wstring(static_cast<int>(v * 100 + .5f)) + L"%"; };
                GraphCrosshair(c, g, 60, [&](int i) { return std::to_wstring(59 - i) + L" s ago   CPU " + pctTxt(cpuP[i]) + L"   GPU " + pctTxt(gpuP[i]) + L"   RAM " + pctTxt(ramP[i]); }, {{&cpuP, col::accent}, {&gpuP, col::ok}, {&ramP, RGB(120, 110, 255)}});
            }
            wchar_t clockB[32] = L"", powerB[32] = L"";
            if (rd.present && rd.cpuClock >= 0) swprintf(clockB, 32, L"%.2f GHz", rd.cpuClock / 1000);
            if (rd.present && rd.cpuPower >= 0) swprintf(powerB, 32, L"%.0f W", rd.cpuPower);
            const std::wstring missing = rd.present ? L"not exposed" : rd.everSeen ? L"stale" : L"no data";
            struct Out { const wchar_t* k; std::wstring v; bool dim; COLORREF cc; } outs[3] = {
                {L"CPU load", EffectiveCpuLoad(u.cpu, rd) < 0 ? L"N/A" : std::to_wstring(static_cast<int>(EffectiveCpuLoad(u.cpu, rd) + .5)) + L"%", EffectiveCpuLoad(u.cpu, rd) < 0, col::accent},
                {L"CPU clock", clockB[0] ? clockB : missing, !clockB[0], col::accent},
                {L"CPU power", powerB[0] ? powerB : missing, !powerB[0], col::warn}};
            const int ow = (r.right - r.left - c.S(40)) / 3;
            for (int i = 0; i < 3; ++i) {
                const int ox = r.left + c.S(20) + i * ow;
                DrawDot(c.dc, ox, r.top + c.S(186), c.S(8), outs[i].cc);
                Text(c.dc, outs[i].k, RECT{ox + c.S(14), r.top + c.S(178), ox + ow, r.top + c.S(198)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
                Text(c.dc, outs[i].v, RECT{ox + c.S(14), r.top + c.S(198), ox + ow, r.top + c.S(234)}, c.u.f.h1, outs[i].dim ? col::faint : col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        const Health& h = s.health;
        const bool scanning = g_healthWorker.IsScanning();
        const CleanupScanReport& cs = s.cleanupScan;
        const CleanupPreviewStatus cstat = scanning ? CleanupPreviewStatus::Scanning
                                           : s.cleanupScanFresh ? cs.status : CleanupPreviewStatus::NotScanned;
        const ScanButtonState scanState = ScanStateOf(scanning, s.healthDone, h.score);   // 2.8.3: one status model for the gauge, the text and the Scan button
        const COLORREF hc = scanState == ScanButtonState::Scanning ? col::accent : scanState == ScanButtonState::Unknown ? col::sub : scanState == ScanButtonState::Good ? col::ok
                          : scanState == ScanButtonState::Info ? col::accent : scanState == ScanButtonState::Warning ? col::warn : col::err;
        const COLORREF cc = cstat == CleanupPreviewStatus::Scanning ? col::accent
                          : cstat == CleanupPreviewStatus::Clean ? col::ok
                          : cstat == CleanupPreviewStatus::CleanupAvailable ? col::accent
                          : cstat == CleanupPreviewStatus::PartialScan ? col::warn
                          : cstat == CleanupPreviewStatus::ScanFailed ? col::err : col::sub;
        RECT hr = wide ? RECT{c.x + hero + gap, c.y, c.x + c.w, c.y + hh} : RECT{c.x, c.y + hh + gap, c.x + c.w, c.y + 2 * hh + gap};
        if (c.Visible(hr)) {
            Round(c.dc, hr, c.S(28), col::panel, s.healthDone ? Mix(col::border, hc, .5) : col::border);
            Text(c.dc, L"Anubis System Health", RECT{hr.left + c.S(20), hr.top + c.S(14), hr.right, hr.top + c.S(38)}, c.u.f.btn, col::text);
            const int cx = (hr.left + hr.right) / 2, cy = hr.top + c.S(86);
            Halo(c.dc, cx, cy, c.S(58), hc, .14f);
            GaugeRing(c.dc, cx, cy, c.S(42), c.S(7), scanning ? -1 : s.healthDone ? 100 : -1, hc);
            Text(c.dc, scanning ? L"Scanning\x2026" : s.healthDone ? h.label : L"Not scanned", RECT{cx - c.S(38), cy - c.S(14), cx + c.S(38), cy + c.S(14)}, c.u.f.small, hc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            // Separate Health finding line vs Cleanup Preview status
            std::wstring hNote = L"System checks: not scanned yet";
            if (scanning) hNote = L"Checking system security & cleanup areas\x2026";
            else if (s.healthDone) {
                hNote = (h.score >= 2 && !h.findings.empty()) ? h.findings.front().text : L"System health: " + h.label;
            }
            Text(c.dc, hNote, RECT{hr.left + c.S(16), hr.top + c.S(136), hr.right - c.S(16), hr.top + c.S(154)}, c.u.f.tiny, s.healthDone && h.score >= 2 ? hc : col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            std::wstring cLine = std::wstring(CleanupPreviewLabel(cstat));
            if (cstat == CleanupPreviewStatus::Clean) cLine += L"  \x00B7  0 B reclaimable";
            else if (cstat == CleanupPreviewStatus::CleanupAvailable)
                cLine += L"  \x00B7  " + FormatBytes(cs.totalBytes) + L" in " + std::to_wstring(cs.categoriesWithData) + L" item(s)";
            else if (cstat == CleanupPreviewStatus::PartialScan)
                cLine += L"  \x00B7  " + FormatBytes(cs.totalBytes) + L" (" + std::to_wstring(cs.inaccessibleDirs) + L" denied)";
            else if (cstat == CleanupPreviewStatus::ScanFailed && !cs.error.empty())
                cLine += L"  \x00B7  " + cs.error;
            Text(c.dc, cLine, RECT{hr.left + c.S(16), hr.top + c.S(156), hr.right - c.S(16), hr.top + c.S(176)}, c.u.f.small, cc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (!scanning && s.cleanupScanFresh && !cs.warnings.empty()) {
                Text(c.dc, cs.warnings.front(), RECT{hr.left + c.S(16), hr.top + c.S(176), hr.right - c.S(16), hr.top + c.S(192)}, c.u.f.tiny, col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            const bool showOpenClean = !scanning && s.cleanupScanFresh && cs.completed && (cs.totalBytes > 0 || cs.inaccessibleDirs > 0);
            if (showOpenClean) {
                const int bw = c.S(116), bg = c.S(8), bx = cx - (2 * bw + bg) / 2;
                RECT br{bx, hr.bottom - c.S(48), bx + bw, hr.bottom - c.S(14)};
                RECT cr{bx + bw + bg, hr.bottom - c.S(48), bx + 2 * bw + bg, hr.bottom - c.S(14)};
                if (scanState == ScanButtonState::Unknown || scanState == ScanButtonState::Scanning || scanState == ScanButtonState::Info) DrawButton(c.u, c.dc, br, L"Scan again", Btn::Secondary, true, c.Hover(br));
                else DrawStatusButton(c.u, c.dc, br, L"Scan again", hc, true, c.Hover(br));
                DrawButton(c.u, c.dc, cr, L"Open System Care", Btn::Primary, true, c.Hover(cr));
                c.Hit(br, [] { g_healthWorker.Start(g_wnd, WM_HEALTHSCAN, g_usage); NavOpenSystemCareScan(); });
                c.Hit(cr, [] { NavGo(PSystemCare); });
            } else {
                RECT br{cx - c.S(74), hr.bottom - c.S(48), cx + c.S(74), hr.bottom - c.S(14)};
                const std::wstring scanLabel = scanning ? L"Scanning\x2026" : s.healthDone ? L"Scan again" : L"Scan now";
                if (scanState == ScanButtonState::Good || scanState == ScanButtonState::Warning || scanState == ScanButtonState::Error) DrawStatusButton(c.u, c.dc, br, scanLabel, hc, !scanning, c.Hover(br));   // green / amber / red from the real result
                else DrawButton(c.u, c.dc, br, scanLabel, Btn::Primary, !scanning, c.Hover(br));
                if (!scanning) c.Hit(br, [] { g_healthWorker.Start(g_wnd, WM_HEALTHSCAN, g_usage); NavOpenSystemCareScan(); });
            }
        }
        c.y = (wide ? r.bottom : hr.bottom) + gap;
    }
    // row 2: metric tiles
    {
        const int cols = wide ? 4 : 2, tw = (c.w - gap * (cols - 1)) / cols, th = c.S(150);
        const auto ct = TempState(true, rd.cpuTemp), gt = TempState(false, rd.gpuTemp);
        double rpm = -1; bool live = false; for (const auto& f : rd.fans) if (f.rpm > rpm) { rpm = f.rpm; live = f.live; }
        const bool fanOk = rd.present && rpm >= 0;
        double fmax = 1000, a = 0, b = 0; if (g_store.Get(SFan).MinMax(a, b)) fmax = std::max(1000.0, b * 1.1);
        double nmax = 1024; if (g_store.Get(SNetDown).MinMax(a, b)) nmax = std::max(1024.0, b * 1.1);
        struct T { const wchar_t* g, *k; MiniMetric m; COLORREF cc; std::wstring chip; COLORREF chipC; double lo, hi; } tiles[4] = {
            {L"\xE950", L"CPU temp", MCpuTemp, col::warn, ct.first, ct.second, 30, 100},
            {L"\xF211", L"GPU temp", MGpuTemp, col::warn, gt.first, gt.second, 30, 100},
            {L"\xE9CA", L"Fan", MFan, col::ok, !fanOk ? (rd.present ? L"not exposed" : L"stale") : live ? L"Live" : L"unverified", fanOk && live ? col::ok : col::faint, 0, fmax},
            {L"\xE839", L"Network", MNet, col::accent, !g_netSel.valid ? L"no data" : g_netSel.connected ? L"Live" : L"disconnected", g_netSel.connected ? col::ok : col::faint, 0, nmax}};
        for (int i = 0; i < 4; ++i) {
            const int cl = i % cols, rw = i / cols;
            RECT tr{c.x + cl * (tw + gap), c.y + rw * (th + gap), c.x + cl * (tw + gap) + tw, c.y + rw * (th + gap) + th};
            MetricTile(c, tr, tiles[i].g, tiles[i].k, tiles[i].m, tiles[i].cc, tiles[i].chip, tiles[i].chipC, tiles[i].lo, tiles[i].hi);
        }
        c.y += (th + gap) * (wide ? 1 : 2);
        if (!rd.present) { Text(c.dc, L"\x24D8 Hardware monitoring is off. Open the Monitor tab to turn it on.", RECT{c.x, c.y, c.x + c.w, c.y + c.S(20)}, c.u.f.small, col::faint, DT_SINGLELINE); c.y += c.S(26); }
    }
    // row 3: security, system, quick actions
    {
        const IconPack* ip = FindPack(s.icons.active);
        std::wstring iconTxt = ip ? std::wstring(ip->name) + (*ip->variant ? L" " + std::wstring(ip->variant) : L"") : L"Windows default";
        if (s.icons.pending) iconTxt += L" (restart)";
        int enabledMods = 0; for (const auto& m : s.mods) enabledMods += m.enabled;
        const SecurityInfo& sec = s.sec;
        const int cols = wide ? 3 : 2, w3 = (c.w - gap * (cols - 1)) / cols;
        int y1 = c.y, y2 = c.y;
        StatusGrid(c, L"Security & disk", {
            {L"Antivirus", !s.deepDone ? L"..." : sec.av.empty() ? L"None found" : sec.av + (sec.avOn ? L" (on)" : L" (off)"), !s.deepDone ? col::faint : Good(sec.avOn)},
            {L"Firewall", sec.firewall ? L"On" : L"Off", !s.deepDone ? col::faint : Good(sec.firewall)},
            {L"UAC", sec.uac ? L"On" : L"Off", !s.deepDone ? col::faint : Good(sec.uac)},
            {L"Secure Boot", sec.secureBoot < 0 ? L"Unknown" : sec.secureBoot ? L"On" : L"Off", sec.secureBoot == 1 ? col::ok : col::faint},
            {L"System disk", !s.deepDone ? L"..." : s.disk.kind + (s.disk.model.empty() ? L"" : L" \x00B7 " + s.disk.model), ToneColor(ToneSystemDisk(s.deepDone, s.disk.healthOk, s.disk.health, u.diskTotal ? 100.0 * static_cast<double>(u.diskFree) / static_cast<double>(u.diskTotal) : -1.0))},
            {L"Disk health / restore", !s.deepDone ? L"..." : (s.disk.health + L"  \x00B7  " + std::to_wstring(s.rps.size()) + L" restore pt(s)"), !s.deepDone ? col::faint : (s.disk.health == L"Unknown" ? col::sub : s.disk.healthOk ? col::ok : col::err)}}, c.x, w3, y1);
        StatusGrid(c, L"System", {
            {L"Windows", WinVerText(s.ver), ToneColor(ToneWindows(s.ver.build))},
            {L"Uptime", UptimeText(u.uptimeMs), ToneColor(ToneUptime(u.uptimeMs))},
            {L"Theme", s.themeActive ? L"As3aD" : L"Other theme", s.themeActive ? col::ok : col::faint},
            {L"Theme patch", s.patchLoaded ? L"Active" : L"Not active", s.patchLoaded ? col::ok : col::faint},
            {L"Windhawk", !s.whInstalled ? L"Not installed" : s.whRunning ? L"Running, " + std::to_wstring(enabledMods) + L" mods on" : L"Stopped", s.whRunning ? col::ok : col::warn},
            {L"Icons / cursors", iconTxt + L" / " + (s.cursorsOn ? L"Prismatic" : L"Windows"), ip ? col::ok : col::faint}}, c.x + w3 + gap, w3, y2);
        // the actions open their pages, where each has its own confirmation
        const int ax = wide ? c.x + 2 * (w3 + gap) : c.x, ay = wide ? c.y : std::max(y1, y2), aw = wide ? w3 : c.w, ah = c.S(46) + 6 * c.S(30) + c.S(10) - c.S(14) + c.S(14);
        RECT ar{ax, ay, ax + aw, ay + ah};
        if (c.Visible(ar)) {
            Round(c.dc, ar, c.S(28), col::panel, col::border);
            Text(c.dc, L"Quick actions", RECT{ax + c.S(18), ay + c.S(14), ax + aw, ay + c.S(38)}, c.u.f.btn, col::text);
            struct Qa { const wchar_t* g, *t; int page; } qa[4] = {{L"\xE74D", L"Scan & clean", PSystemCare}, {L"\xE90F", L"Repair", POptimize}, {L"\xE790", L"Apply theme", PAppearance}, {L"\xEA86", L"Open Windhawk", PWindhawk}};
            const int bw = (aw - c.S(44)) / 2, bh = c.S(66);
            for (int i = 0; i < 4; ++i) {
                RECT b{ax + c.S(18) + (i % 2) * (bw + c.S(8)), ay + c.S(50) + (i / 2) * (bh + c.S(8)), ax + c.S(18) + (i % 2) * (bw + c.S(8)) + bw, ay + c.S(50) + (i / 2) * (bh + c.S(8)) + bh};
                const bool ho = c.Hover(b);
                Round(c.dc, b, c.S(20), ho ? col::panel2 : col::panel, ho ? col::accent : col::border);
                Text(c.dc, qa[i].g, RECT{b.left, b.top + c.S(8), b.right, b.top + c.S(36)}, c.u.f.icon, col::accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                Text(c.dc, qa[i].t, RECT{b.left, b.top + c.S(38), b.right, b.bottom - c.S(6)}, c.u.f.small, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                const int page = qa[i].page; c.Hit(b, [page] { NavGo(page); });
            }
        }
        c.y = std::max({y1, y2, ay + ah + c.S(14)}) ;
    }
    // health findings (full list, once scanned)
    if (s.healthDone && !s.health.findings.empty()) {
        const int rowH = c.S(32);   // 2.9.8: every finding with an action carries its button
        const int fh = c.S(46) + static_cast<int>(s.health.findings.size()) * rowH + c.S(8);
        RECT r{c.x, c.y, c.x + c.w, c.y + fh};
        if (c.Visible(r)) {
            Round(c.dc, r, c.S(28), col::panel, col::border);
            Text(c.dc, L"Health findings", RECT{r.left + c.S(18), r.top + c.S(14), r.right, r.top + c.S(38)}, c.u.f.btn, col::text);
            int fy = r.top + c.S(44);
            for (const auto& f : s.health.findings) {
                const COLORREF fc = f.severity == 0 ? col::ok : f.severity == 1 ? col::accent : f.severity == 2 ? col::warn : col::err;
                DrawDot(c.dc, r.left + c.S(18), fy + c.S(12), c.S(7), fc);
                int textRight = r.right - c.S(16);
                if (f.act != FindingAct::None) {
                    const std::wstring lab = FindingActLabel(f.act);
                    const int bw = std::max(c.S(120), TextWidth(c.dc, lab, c.u.f.small) + c.S(28));
                    const RECT br{r.right - c.S(16) - bw, fy, r.right - c.S(16), fy + c.S(28)};
                    DrawButton(c.u, c.dc, br, lab, FindingActIsFix(f.act) ? Btn::Primary : Btn::Secondary, !g_busy.load(), c.Hover(br));
                    const FindingAct act = f.act; if (!g_busy.load()) c.Hit(br, [act] { FindingDo(act); });
                    textRight = br.left - c.S(12);
                }
                Text(c.dc, f.text, RECT{r.left + c.S(34), fy, textRight, fy + c.S(28)}, c.u.f.small, col::text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                fy += rowH;
            }
        }
        c.y += fh + gap;
    }
    // row 4: Desktop Monitoring quick actions + live previews of Sidebar and Mini Monitor (MON-13)
    {
        const int wh = c.S(186), paneW = c.S(190), paneGap = c.S(14); RECT r{c.x, c.y, c.x + c.w, c.y + wh};
        if (c.Visible(r)) {
            Round(c.dc, r, c.S(28), col::panel, col::border);
            Text(c.dc, L"\xE7F4", RECT{r.left + c.S(18), r.top + c.S(14), r.left + c.S(50), r.top + c.S(46)}, c.u.f.iconBig, col::accent, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, L"Desktop Monitoring", RECT{r.left + c.S(58), r.top + c.S(12), r.left + c.S(300), r.top + c.S(34)}, c.u.f.btn, col::text);
            const size_t al = g_alertLog.size();
            Text(c.dc, al ? std::to_wstring(al) + L" alert(s) this session" : L"No alerts this session", RECT{r.left + c.S(58), r.top + c.S(34), r.left + c.S(320), r.top + c.S(54)}, c.u.f.small, al ? col::warn : col::sub);
            {
                struct Dqa { const wchar_t* text; std::function<void()> fn; } acts[3] = {
                    {L"Open", [] { NavGo(PDesktop); }},
                    {g_monPaused ? L"Resume" : L"Pause", [] {
                        if (TestGuard(g_monPaused ? L"Resume monitoring" : L"Pause monitoring")) return;
                        g_monPaused = !g_monPaused;
                        g_engine.Monitor(!g_monPaused);
                        LogI(g_monPaused ? L"Hardware monitoring paused." : L"Hardware monitoring resumed.");
                        InvalidateRect(g_wnd, nullptr, FALSE);
                    }},
                    {L"Settings", [] { NavGo(PMonPrefs); }}};
                int bx = r.left + c.S(18), by = r.top + c.S(70);
                for (const auto& a : acts) {
                    const int bw = c.S(78), bh = c.S(32);
                    RECT br{bx, by, bx + bw, by + bh};
                    DrawButton(c.u, c.dc, br, a.text, Btn::Secondary, true, c.Hover(br));
                    c.Hit(br, a.fn);
                    bx += bw + c.S(8);
                }
                const std::wstring monSt = g_monPaused ? L"Monitoring paused" : (rd.present ? L"Monitoring live (1 s)" : L"Monitoring idle");
                Text(c.dc, monSt, RECT{r.left + c.S(18), r.top + c.S(114), r.left + c.S(320), r.top + c.S(136)}, c.u.f.small, g_monPaused ? col::warn : (rd.present ? col::ok : col::faint));
            }
            for (int i = 0; i < 2; ++i) {
                const bool on = i ? g_sidebarOn : g_miniOn;
                const int px = r.right - c.S(18) - (2 - i) * paneW - (1 - i) * paneGap;
                const RECT box{px, r.top + c.S(44), px + paneW, r.bottom - c.S(14)};
                Text(c.dc, i ? L"Sidebar" : L"Mini Monitor", RECT{px, r.top + c.S(12), px + c.S(120), r.top + c.S(40)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
                RECT tr{px + paneW - c.S(42), r.top + c.S(15), px + paneW, r.top + c.S(37)};
                DrawToggle(c.u, c.dc, tr.left, tr.top, on, true, c.Hover(tr));
                if (i) c.Hit(tr, [] { ToggleSidebar(!g_sidebarOn); InvalidateRect(g_wnd, nullptr, FALSE); }); else c.Hit(tr, [] { ToggleMini(!g_miniOn); InvalidateRect(g_wnd, nullptr, FALSE); });
                Round(c.dc, box, c.S(20), col::track, col::border);
                WidgetFrame& wf = g_widgetFrame[i];
                if (on && c.dc.d2d() && wf.w > 0 && wf.h > 0) {
                    const float bw = static_cast<float>(box.right - box.left), bh = static_cast<float>(box.bottom - box.top);
                    const float k = i ? bw / 2.2f / wf.w : std::min((bh - 8) / wf.h, (bw - 8) / wf.w);   // the Sidebar is tall: show its top part
                    const float dw = wf.w * k, srcH = std::min<float>(static_cast<float>(wf.h), (bh - 8) / k), dh = srcH * k;
                    const float dx = box.left + (bw - dw) / 2, dy = box.top + (bh - dh) / 2;
                    const int clip = PushRoundClip(c.dc, box, c.S(20));
                    DrawPixelsPart(c.dc, &wf, wf.px.data(), wf.w, wf.h, D2D1::RectF(dx, dy, dx + dw, dy + dh), D2D1::RectF(0, 0, static_cast<float>(wf.w), srcH), 1.f, wf.drawn != wf.seq);
                    PopClip(c.dc, clip);
                    wf.drawn = wf.seq;
                } else Text(c.dc, on ? L"Starting\x2026" : L"Off", box, c.u.f.small, col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        c.y += wh + gap;
    }
}
std::wstring NA(double v, const wchar_t* suf = L"", int dec = 0) {
    if (v < 0) return L"N/A";
    wchar_t b[48]; swprintf(b, 48, dec ? L"%.1f%s" : L"%.0f%s", v, suf);
    return b;
}
// A small two-column readout box: rows of label / value, N/A when missing.
void Readout(Ctx& c, const std::wstring& title, const wchar_t* icon, const std::vector<std::pair<std::wstring, std::wstring>>& rows, int x, int w, int& y) {
    const int h = c.S(48) + static_cast<int>(rows.size()) * c.S(26) + c.S(8);
    RECT box{x, y, x + w, y + h};
    if (c.Visible(box)) {
        Round(c.dc, box, c.S(28), col::panel, col::border);
        Text(c.dc, icon, RECT{x + c.S(16), y + c.S(13), x + c.S(44), y + c.S(41)}, c.u.f.iconBig, col::accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(c.dc, title, RECT{x + c.S(50), y + c.S(14), x + w - c.S(12), y + c.S(40)}, c.u.f.btn, col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        int ry = y + c.S(48);
        for (const auto& [k, v] : rows) {
            Text(c.dc, k, RECT{x + c.S(16), ry, x + w / 2 + c.S(20), ry + c.S(24)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, v, RECT{x + w / 2, ry, x + w - c.S(16), ry + c.S(24)}, c.u.f.small, v == L"N/A" ? col::faint : col::text, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            ry += c.S(26);
        }
    }
    y += h + c.S(14);
}
// A labelled row of option chips (selected = highlighted); wraps to more lines when needed.
using ChipOpt = std::tuple<std::wstring, bool, std::function<void()>>;
void Chips(Ctx& c, const wchar_t* label, const std::vector<ChipOpt>& opts) {
    Text(c.dc, label, RECT{c.x + c.S(16), c.y, c.x + c.S(150), c.y + c.S(30)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
    int x = c.x + c.S(150);
    for (const auto& [text, sel, fn] : opts) {
        const int bw = std::max(c.S(64), TextWidth(c.dc, text, c.u.f.small) + c.S(24));
        if (x + bw > c.x + c.w) { x = c.x + c.S(150); c.y += c.S(36); }
        RECT br{x, c.y, x + bw, c.y + c.S(30)};
        Round(c.dc, br, c.S(15), sel ? col::accentDeep : (c.Hover(br) ? col::panel2 : col::panel), sel ? col::accent : col::border);
        Text(c.dc, text, br, c.u.f.small, sel ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const auto f = fn; c.Hit(br, [f] { f(); InvalidateRect(g_wnd, nullptr, FALSE); });
        x += bw + c.S(8);
    }
    c.y += c.S(38);
}
void PageMonitor(Ctx& c, const Cache&) {
    const Readings& r = g_read;
    const bool on = Hub().HelperInstalled();
    {
        RowSpec row; row.icon = L"\xE9D2"; row.title = L"Hardware monitoring";
        row.desc = !on ? L"The monitoring helper is not installed. Reinstall AnkhCore to add it."
                       : r.present ? L"Running. Readings update every second." + (r.model.empty() ? std::wstring() : L"  Device: " + r.model)
                                   : L"Installed but not reporting yet. Press Start, then wait a moment.";
        row.chips.push_back({r.present ? L"Live" : on ? L"Idle" : L"Missing", r.present ? col::ok : col::warn});
        row.buttons = {
            {r.present ? L"Restart" : L"Start", Btn::Primary, on, [] { if (TestGuard(L"Start hardware monitoring")) return; g_engine.Monitor(true); }},
            {L"Stop", Btn::Secondary, on && Hub().HelperRunning(), [] { if (TestGuard(L"Stop hardware monitoring")) return; g_engine.Monitor(false); }}};
        Row(c, row);
    }
    if (!on) { Para(c, L"Live temperatures, clocks, power and fan speeds come from the AnkhSensors helper (built on LibreHardwareMonitor). Readings show N/A where a sensor is not exposed by your hardware.", c.u.f.small, col::sub); return; }
    Section(c, L"Live readings");
    const int gap = c.S(14), colw = (c.w - gap) / 2;
    int y1 = c.y, y2 = c.y;
    const double cpuPct = EffectiveCpuLoad(g_usage.cpu, r);
    Readout(c, L"Processor", L"\xE950", {
        {L"Temperature", NA(DispTemp(HelperReading(r, r.cpuTemp)), TempSuffix())}, {L"Usage", NA(cpuPct, L" %")},
        {L"Clock", NA(HelperReading(r, r.cpuClock), L" MHz")}, {L"Power", NA(HelperReading(r, r.cpuPower), L" W")}}, c.x, colw, y1);
    {   // utilization and memory come from Windows for each adapter; the helper's sensor values are not tied to one model (B02)
        std::vector<std::pair<std::wstring, std::wstring>> rows = {{L"Usage", NA(g_usage.gpu, L" %")}};
        for (const GpuAdapterView& g : g_gpus) if (!g.software)
            rows.push_back({g.name, NA(g.util, L" %") + (g.dedicatedTotal >= (512ull << 20) ? L"  \u00B7  " + FormatBytes(g.dedicatedUsed) + L" VRAM" : std::wstring())});
        rows.push_back({L"Temperature", NA(DispTemp(HelperReading(r, r.gpuTemp)), TempSuffix())}); rows.push_back({L"Hotspot", NA(DispTemp(HelperReading(r, r.gpuHotspot)), TempSuffix())});
        rows.push_back({L"Clock", NA(HelperReading(r, r.gpuClock), L" MHz")}); rows.push_back({L"Power", NA(HelperReading(r, r.gpuPower), L" W")});
        Readout(c, L"Graphics", L"\xF211", rows, c.x, colw, y1);
    }
    std::vector<std::pair<std::wstring, std::wstring>> fanRows;
    if (!r.present || r.fans.empty()) fanRows.push_back({L"Fans", L"N/A"});
    else for (const auto& f : r.fans) fanRows.push_back({f.name.empty() ? L"Fan" : f.name, f.rpm < 0 ? L"N/A" : std::to_wstring(static_cast<int>(f.rpm)) + L" rpm"});
    Readout(c, L"Fans", L"\xE9CA", fanRows, c.x + colw + gap, colw, y2);
    Readout(c, L"Memory & storage", GlyphMemory(), {
        {L"RAM usage", NA(g_usage.ram >= 0 ? g_usage.ram : HelperReading(r, r.ramLoad), L" %")},
        {L"Disk temp", NA(DispTemp(HelperReading(r, r.diskTemp)), TempSuffix())}, {L"Disk usage", NA(HelperReading(r, r.diskLoad), L" %")}}, c.x + colw + gap, colw, y2);
    Readout(c, L"Network", L"\xE839", {
        {L"Adapter", g_netSel.valid ? g_netSel.name : std::wstring(L"no data")},
        {L"Download", g_netSel.connected ? NetRate(g_netSel.down) : std::wstring(L"N/A")},
        {L"Upload", g_netSel.connected ? NetRate(g_netSel.up) : std::wstring(L"N/A")}}, c.x + colw + gap, colw, y2);
    c.y = std::max(y1, y2);
    Section(c, L"Desktop widgets", L"The Sidebar and the Mini Monitor show these readings on the desktop and stay there with the AnkhCore window closed.");
    {
        RowSpec row; row.icon = L"\xE9D9"; row.title = L"Desktop Monitoring"; row.desc = L"Sidebar: Layout, Appearance, Widgets, Behavior, Profiles. Mini Monitor settings.";
        row.buttons.push_back({L"Open", Btn::Secondary, true, [] { NavGo(PDesktop); }});
        Row(c, row);
    }
}
// ---------------------------------------------------------------- Desktop Monitoring sub-pages (D36, D40)
// The Sidebar's and the Mini Monitor's settings, one topic per page. Every control is the one that used to sit
// on System Monitor: the same settings keys and values, saved and applied at once. Nothing is stored differently.
bool g_advOpen[PageCount] = {};   // a page's "Advanced" group is open (a view state, not a setting)
bool AdvancedGroup(Ctx& c, int page, const wchar_t* desc) {
    RowSpec row; row.icon = L"\xE713"; row.title = L"Advanced"; row.desc = desc; row.toggle = g_advOpen[page] ? 1 : 0;
    row.onToggle = [page] { g_advOpen[page] = !g_advOpen[page]; InvalidateRect(g_wnd, nullptr, FALSE); };
    Row(c, row);
    return g_advOpen[page];
}
void SbPreview(Ctx& c, int heightDip) {
        {   // live preview: the Sidebar's own last frame, whole on the left and its top at full size on the right
            const int ph = c.S(heightDip); const RECT box{c.x + c.S(16), c.y, c.x + c.w, c.y + ph};
            if (c.Visible(box)) {
                Round(c.dc, box, c.S(20), col::track, col::border);
                WidgetFrame& wf = g_widgetFrame[1];
                if (g_sidebarOn && c.dc.d2d() && wf.w > 0 && wf.h > 0) {
                    const float in = static_cast<float>(ph - c.S(20)), k = std::min(1.f, in / wf.h), dw = wf.w * k, dh = wf.h * k;
                    const float dx = static_cast<float>(box.left + c.S(18)), dy = box.top + (ph - dh) / 2;
                    DrawPixelsPart(c.dc, &wf, wf.px.data(), wf.w, wf.h, D2D1::RectF(dx, dy, dx + dw, dy + dh), D2D1::RectF(0, 0, static_cast<float>(wf.w), static_cast<float>(wf.h)), 1.f, wf.drawn != wf.seq);
                    wf.drawn = wf.seq;
                    const float fx = dx + dw + c.S(24), room = static_cast<float>(box.right - c.S(18)) - fx, fw = std::min<float>(static_cast<float>(wf.w), room), fh = std::min<float>(static_cast<float>(wf.h), in);
                    if (fw > c.S(60)) DrawPixelsPart(c.dc, &wf, wf.px.data(), wf.w, wf.h, D2D1::RectF(fx, static_cast<float>(box.top + c.S(10)), fx + fw, box.top + c.S(10) + fh), D2D1::RectF(0, 0, fw, fh), 1.f, false);
                    const int tx = static_cast<int>(fx + fw) + c.S(20);
                    if (box.right - tx > c.S(120)) {
                        Text(c.dc, L"Live preview", RECT{tx, box.top + c.S(14), box.right - c.S(12), box.top + c.S(36)}, c.u.f.btn, col::text, DT_SINGLELINE);
                        Text(c.dc, L"The Sidebar's real frame: whole (left) and its top at actual size. " + std::to_wstring(g_sbLayout.boxes.size()) + L" section(s), " + std::to_wstring(g_sbLayout.used) + L" of " + std::to_wstring(g_sbAvail) + L" units used.",
                             RECT{tx, box.top + c.S(40), box.right - c.S(12), box.top + c.S(150)}, c.u.f.small, col::sub, DT_WORDBREAK);
                    }
                } else Text(c.dc, L"Turn the Desktop sidebar on to see the live preview.", box, c.u.f.small, col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            c.y += ph + c.S(12);
        }
}
void PageSbLayout(Ctx& c, const Cache&) {
        auto chips = [&](const wchar_t* label, const std::vector<ChipOpt>& opts) { Chips(c, label, opts); };
        auto opt = [](const std::wstring& key, int v) { return [key, v] { SbSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"sidebar.clickThrough", false);
        const int layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop), edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
        const int width = Cfg().Int(L"sidebar.width", 210, 140, 420), op = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
        SbPreview(c, 300);
        Section(c, L"Detail", L"How much every section shows. Custom sets a level for each section.");
        const int density = Cfg().Int(L"sidebar.layout", SbStandard, SbMinimal, SbCustom);
        chips(L"Detail", {{L"Minimal", density == SbMinimal, opt(L"sidebar.layout", SbMinimal)}, {L"Standard", density == SbStandard, opt(L"sidebar.layout", SbStandard)},
                          {L"Detailed", density == SbDetailed, opt(L"sidebar.layout", SbDetailed)}, {L"Custom", density == SbCustom, opt(L"sidebar.layout", SbCustom)}});
        Section(c, L"Sections", L"Pick a section, then show, hide or move it.");
        const std::vector<SbSecItem> secs = SbParseSections(Cfg().Str(L"sidebar.sections", L""));
        g_sbEditSec = std::clamp(g_sbEditSec, 0, SbSecCount - 1); const SbSec es = static_cast<SbSec>(g_sbEditSec);
        bool esOn = true;
        {
            std::vector<ChipOpt> o;
            for (size_t i = 0; i < secs.size(); ++i) {
                const SbSec sc = secs[i].sec; if (sc == es) esOn = secs[i].on;
                o.emplace_back(std::to_wstring(i + 1) + L". " + SbSecName(sc) + (secs[i].on ? L"" : L" (hidden)"), sc == es, [sc] { g_sbEditSec = sc; });
            }
            chips(L"Section", o);
        }
        auto show = [secs, es](bool v) { return [secs, es, v] { auto m = secs; SbShowSection(m, es, v); SbSetSections(m); }; };
        auto move = [secs, es](int dir) { return [secs, es, dir] { auto m = secs; SbMoveSection(m, es, dir); SbSetSections(m); }; };
        chips(L"   Show", {{L"Shown", esOn, show(true)}, {L"Hidden", !esOn, show(false)}, {L"Move up", false, move(-1)}, {L"Move down", false, move(1)}});
        if (density == SbCustom) {
            const SbPerSec lv = SbParsePerSec(Cfg().Str(L"sidebar.levels", L""), SbStandard, 0, 2);
            auto level = [lv, es](int v) { return [lv, es, v] { SbPerSec m = lv; m.v[es] = v; SbSetStr(L"sidebar.levels", SbFormatPerSec(m)); }; };
            chips(L"   Detail", {{L"Minimal", lv.v[es] == SbMinimal, level(SbMinimal)}, {L"Standard", lv.v[es] == SbStandard, level(SbStandard)}, {L"Detailed", lv.v[es] == SbDetailed, level(SbDetailed)}});
        }
        if (g_sidebar && (g_sbLayout.reduced || g_sbLayout.dropped))
            Para(c, L"This screen is not tall enough for everything at this detail: " + std::to_wstring(g_sbLayout.reduced) + L" section(s) show less, " + std::to_wstring(g_sbLayout.dropped) + L" are left out (from the bottom up).", c.u.f.small, col::warn);
}
void PageSbAppearance(Ctx& c, const Cache&) {
        auto chips = [&](const wchar_t* label, const std::vector<ChipOpt>& opts) { Chips(c, label, opts); };
        auto opt = [](const std::wstring& key, int v) { return [key, v] { SbSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"sidebar.clickThrough", false);
        const int layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop), edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
        const int width = Cfg().Int(L"sidebar.width", 210, 140, 420), op = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
        SbPreview(c, 300);
        Section(c, L"Background", L"The material behind the readings and how much of the desktop shows through.");
        const int bk = SbBackgroundCfg(), tn = SbTintCfg();
        chips(L"Background", {{L"Solid", bk == 0, opt(L"sidebar.background", 0)}, {L"Frosted glass", bk == 1, opt(L"sidebar.background", 1)}, {L"Acrylic blur", bk == 2, opt(L"sidebar.background", 2)}});
        chips(L"Opacity", {{L"25%", op == 25, opt(L"sidebar.opacity", 25)}, {L"50%", op == 50, opt(L"sidebar.opacity", 50)}, {L"75%", op == 75, opt(L"sidebar.opacity", 75)}, {L"92%", op == 92, opt(L"sidebar.opacity", 92)}, {L"100%", op == 100, opt(L"sidebar.opacity", 100)}});
        if (bk > 0) chips(L"   Tint", {{L"30%", tn == 30, opt(L"sidebar.tint", 30)}, {L"50%", tn == 50, opt(L"sidebar.tint", 50)}, {L"70%", tn == 70, opt(L"sidebar.tint", 70)}, {L"90%", tn == 90, opt(L"sidebar.tint", 90)}});
        Section(c, L"Colours");
        {
            static const wchar_t* const ckeys[4] = {L"sidebar.accent", L"sidebar.accent2", L"sidebar.textColor", L"sidebar.tintColor"};
            static const COLORREF cdef[4] = {col::accent, col::warn, col::text, col::side};
            g_sbEditColor = std::clamp(g_sbEditColor, 0, 3);
            const std::wstring ck = ckeys[g_sbEditColor]; const int cv = Cfg().Int(ck, -1, -1, 0xFFFFFF);
            const COLORREF d = cdef[g_sbEditColor]; const int base = cv < 0 ? (GetRValue(d) << 16) | (GetGValue(d) << 8) | GetBValue(d) : cv;
            chips(L"Colour of", {{L"Accent", g_sbEditColor == 0, [] { g_sbEditColor = 0; }}, {L"Temperature", g_sbEditColor == 1, [] { g_sbEditColor = 1; }}, {L"Text", g_sbEditColor == 2, [] { g_sbEditColor = 2; }}, {L"Background tint", g_sbEditColor == 3, [] { g_sbEditColor = 3; }}});
            struct Sw { const wchar_t* n; int rgb; };
            static const Sw sw[] = {{L"Blue", 0x00AEFF}, {L"Cyan", 0x22D3EE}, {L"Teal", 0x2DD4BF}, {L"Green", 0x2ECC71}, {L"Amber", 0xF5B041}, {L"Orange", 0xFF8A3D}, {L"Red", 0xFF5C5C}, {L"Pink", 0xFF6FB5}, {L"Violet", 0xA78BFA}, {L"White", 0xE8F4FF}, {L"Navy", 0x081633}, {L"Black", 0x05080F}, {L"Theme colour", -1}};
            std::vector<ChipOpt> o; for (const Sw& w2 : sw) o.emplace_back(w2.n, cv == w2.rgb, opt(ck, w2.rgb));
            chips(L"   Colour", o);
            chips((L"   " + SbColorHex(cv)).c_str(), {{L"R \u2212", false, opt(ck, SbColorStep(base, 0, -16))}, {L"R +", false, opt(ck, SbColorStep(base, 0, 16))}, {L"G \u2212", false, opt(ck, SbColorStep(base, 1, -16))},
                                                    {L"G +", false, opt(ck, SbColorStep(base, 1, 16))}, {L"B \u2212", false, opt(ck, SbColorStep(base, 2, -16))}, {L"B +", false, opt(ck, SbColorStep(base, 2, 16))}});
        }
        Section(c, L"Glow");
        const int gl = Cfg().Int(L"sidebar.glow", 1, 0, 2); const bool an = Cfg().Int(L"sidebar.animate", 0, 0, 1) != 0;
        chips(L"Glow", {{L"Off", gl == 0, opt(L"sidebar.glow", 0)}, {L"Strong", gl == 2, opt(L"sidebar.glow", 2)}, {L"Soft", gl == 1, opt(L"sidebar.glow", 1)}});
        Para(c, L"Frosted glass and Acrylic blur are drawn from a blurred copy of your wallpaper: a click-through overlay cannot blur the live windows behind it. "
                L"Animation follows the Motion setting (Appearance) and Windows' own animation switch; it redraws the Sidebar a few extra times a second, so it uses more processor time.", c.u.f.small, col::faint);
}
void PageSbWidgets(Ctx& c, const Cache&) {
        auto chips = [&](const wchar_t* label, const std::vector<ChipOpt>& opts) { Chips(c, label, opts); };
        auto opt = [](const std::wstring& key, int v) { return [key, v] { SbSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"sidebar.clickThrough", false);
        const int layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop), edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
        const int width = Cfg().Int(L"sidebar.width", 210, 140, 420), op = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
        SbPreview(c, 300);
        Section(c, L"Style", L"How a section draws its readings: indicators, gauges, numeric values or graphs.");
        const std::vector<SbSecItem> secs = SbParseSections(Cfg().Str(L"sidebar.sections", L""));
        g_sbEditSec = std::clamp(g_sbEditSec, 0, SbSecCount - 1); const SbSec es = static_cast<SbSec>(g_sbEditSec);
        bool esOn = true;
        {
            std::vector<ChipOpt> o;
            for (size_t i = 0; i < secs.size(); ++i) {
                const SbSec sc = secs[i].sec; if (sc == es) esOn = secs[i].on;
                o.emplace_back(std::to_wstring(i + 1) + L". " + SbSecName(sc) + (secs[i].on ? L"" : L" (hidden)"), sc == es, [sc] { g_sbEditSec = sc; });
            }
            chips(L"Section", o);
        }
        const SbPerSec st = SbParsePerSec(Cfg().Str(L"sidebar.styles", L""), SbStyleBar, 0, 3);
        auto style = [st, es](int v, bool all) { return [st, es, v, all] { SbPerSec m = st; if (all) { for (int& q : m.v) q = v; } else m.v[es] = v; SbSetStr(L"sidebar.styles", SbFormatPerSec(m)); }; };
        chips(L"   Style", {{L"Indicators", st.v[es] == SbStyleBar, style(SbStyleBar, false)}, {L"Gauges", st.v[es] == SbStyleGauge, style(SbStyleGauge, false)}, {L"Numeric values", st.v[es] == SbStyleValue, style(SbStyleValue, false)},
                            {L"Graphs", st.v[es] == SbStyleGraph, style(SbStyleGraph, false)}, {L"Use for every section", false, style(st.v[es], true)}});
        Section(c, L"Text and spacing");
        const int sp = Cfg().Int(L"sidebar.spacing", 1, 0, 2), fs = Cfg().Int(L"sidebar.fontScale", 100, 80, 150);
        chips(L"Spacing", {{L"Compact", sp == 0, opt(L"sidebar.spacing", 0)}, {L"Normal", sp == 1, opt(L"sidebar.spacing", 1)}, {L"Relaxed", sp == 2, opt(L"sidebar.spacing", 2)}});
        chips(L"Text size", {{L"90%", fs == 90, opt(L"sidebar.fontScale", 90)}, {L"115%", fs == 115, opt(L"sidebar.fontScale", 115)}, {L"130%", fs == 130, opt(L"sidebar.fontScale", 130)}, {L"100%", fs == 100, opt(L"sidebar.fontScale", 100)}});
        Section(c, L"Animation");
        const int gl = Cfg().Int(L"sidebar.glow", 1, 0, 2); const bool an = Cfg().Int(L"sidebar.animate", 0, 0, 1) != 0;
        chips(L"Animation", {{L"Smooth bars and gauges", an, opt(L"sidebar.animate", 1)}, {L"Off", !an, opt(L"sidebar.animate", 0)}});
}
void PageSbBehavior(Ctx& c, const Cache&) {
        auto chips = [&](const wchar_t* label, const std::vector<ChipOpt>& opts) { Chips(c, label, opts); };
        auto opt = [](const std::wstring& key, int v) { return [key, v] { SbSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"sidebar.clickThrough", false);
        const int layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop), edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
        const int width = Cfg().Int(L"sidebar.width", 210, 140, 420), op = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
        Section(c, L"Placement", L"Where the Sidebar sits and how it relates to your windows.");
        chips(L"Layer", {{L"Desktop", layer == 3, opt(L"sidebar.layer", 3)}, {L"Below windows", layer == 2, opt(L"sidebar.layer", 2)}, {L"Normal", layer == 1, opt(L"sidebar.layer", 1)}, {L"Always on top", layer == 0, opt(L"sidebar.layer", 0)}});
        if (layer == 3) Para(c, g_sbHost || !g_sidebar ? L"Desktop: the Sidebar is part of the desktop. Every window covers it, desktop icons and menus work as usual, and it takes no screen space and no mouse input. Mouse, screen-edge, auto-hide and drag settings apply to the other layers."
                                                    : L"Desktop: Windows offered no desktop layer to join, so the Sidebar stays below all windows instead, without reserving space.", c.u.f.small, col::faint);
        if (layer == 3 && Cfg().Has(L"sidebar.prevLayer")) {
            const int prev = Cfg().Int(L"sidebar.prevLayer", SbLayerTop, SbLayerTop, SbLayerBottom);
            chips(L"Earlier setup", {{L"Restore previous placement", false, [prev] { SbSetOption(L"sidebar.layer", prev); LogI(L"Desktop sidebar: previous placement restored."); }}});
        }
        chips(L"Edge", {{L"Left", edge == 0, opt(L"sidebar.edge", 0)}, {L"Right", edge == 1, opt(L"sidebar.edge", 1)}});
        {
            std::vector<std::tuple<std::wstring, bool, std::function<void()>>> mons;
            const std::wstring cur = Cfg().Str(L"sidebar.monitor", L"");
            const auto choices = SbMonitorChoices();
            for (size_t i = 0; i < choices.size(); ++i) {
                const std::wstring dev = choices[i].first;
                const bool sel = cur.empty() ? i == 0 : _wcsicmp(cur.c_str(), dev.c_str()) == 0;
                mons.emplace_back(choices[i].second, sel, [dev] { Cfg().SetStr(L"sidebar.monitor", dev); SbSetOption(L"", 0); });
            }
            chips(L"Screen", mons);
        }
        Section(c, L"Size");
        chips((L"Width  " + std::to_wstring(width)).c_str(), {{L"\u2212 10", false, opt(L"sidebar.width", std::max(140, width - 10))}, {L"170", width == 170, opt(L"sidebar.width", 170)}, {L"210", width == 210, opt(L"sidebar.width", 210)},
                          {L"260", width == 260, opt(L"sidebar.width", 260)}, {L"320", width == 320, opt(L"sidebar.width", 320)}, {L"+ 10", false, opt(L"sidebar.width", std::min(420, width + 10))}});
        const int hp = Cfg().Int(L"sidebar.heightPct", 100, 40, 100), va = Cfg().Int(L"sidebar.valign", 0, 0, 2);
        chips(L"Height", {{L"50%", hp == 50, opt(L"sidebar.heightPct", 50)}, {L"65%", hp == 65, opt(L"sidebar.heightPct", 65)}, {L"80%", hp == 80, opt(L"sidebar.heightPct", 80)}, {L"Full", hp == 100, opt(L"sidebar.heightPct", 100)}});
        const int pp = Cfg().Int(L"sidebar.posPct", -1, -1, 100);
        auto place = [](int v) { return [v] { Cfg().SetInt(L"sidebar.posPct", -1); SbSetOption(L"sidebar.valign", v); }; };
        if (hp < 100) chips(pp >= 0 ? (L"   Position  " + std::to_wstring(pp) + L"%").c_str() : L"   Position", {{L"Top", pp < 0 && va == 0, place(0)}, {L"Middle", pp < 0 && va == 1, place(1)}, {L"Bottom", pp < 0 && va == 2, place(2)}});
        if (AdvancedGroup(c, PSbBehavior, L"Mouse mode, reserved screen edge, auto-hide and dragging. These apply to the overlay layers, not to Desktop.")) {
        chips(L"Mouse", {{L"Interactive", !ct, opt(L"sidebar.clickThrough", 0)}, {L"Click-through", ct, opt(L"sidebar.clickThrough", 1)}});
        const bool rs = Cfg().Bool(L"sidebar.reserve", false);
        chips(L"Screen edge", {{L"Overlay windows", !rs, opt(L"sidebar.reserve", 0)}, {L"Reserve the edge", rs, opt(L"sidebar.reserve", 1)}});
        Para(c, L"Click-through: the mouse passes to whatever is under the sidebar. Switch back with Ctrl+Alt+Shift+S or the tray menu. Right-click the sidebar to make it click-through.", c.u.f.small, col::faint);
        const bool ah = Cfg().Int(L"sidebar.autoHide", 0, 0, 1) != 0, lk = Cfg().Int(L"sidebar.locked", 1, 0, 1) != 0;
        chips(L"Auto-hide", {{L"On", ah, opt(L"sidebar.autoHide", 1)}, {L"Off", !ah, opt(L"sidebar.autoHide", 0)}});
        chips(L"Position", {{L"Unlocked (drag to move)", !lk, opt(L"sidebar.locked", 0)}, {L"Locked", lk, opt(L"sidebar.locked", 1)}});
        Para(c, L"Auto-hide: the sidebar slides off its edge when the pointer has been away for a moment and returns when the pointer touches that edge (not while the edge is reserved). "
                L"Unlocked: drag the sidebar to another screen, to the other side, or up and down when it is shorter than the screen.", c.u.f.small, col::faint);
        }
}
void PageSbProfiles(Ctx& c, const Cache&) {
        auto chips = [&](const wchar_t* label, const std::vector<ChipOpt>& opts) { Chips(c, label, opts); };
        auto opt = [](const std::wstring& key, int v) { return [key, v] { SbSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"sidebar.clickThrough", false);
        const int layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop), edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
        const int width = Cfg().Int(L"sidebar.width", 210, 140, 420), op = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
        SbPreview(c, 300);
        Section(c, L"Looks", L"A profile holds the whole look: layout, widgets, colours and background. Applying one changes only those settings.");
        {   // profiles: built-in looks and three of the user's own
            std::vector<ChipOpt> o; const std::wstring cur = SbCurrentProfile();
            for (const SbBuiltin& b : SbBuiltinProfiles()) { const std::wstring t = b.text; o.emplace_back(b.name, false, [t] { SbApplyProfile(t); }); }
            for (int i = 1; i <= 3; ++i) {
                const std::wstring t = Cfg().Str(L"sidebar.profile." + std::to_wstring(i), L"");
                if (!t.empty()) o.emplace_back(L"My profile " + std::to_wstring(i), t == cur, [t] { SbApplyProfile(t); });
            }
            chips(L"Profile", o);
            std::vector<ChipOpt> sv;
            for (int i = 1; i <= 3; ++i) sv.emplace_back(L"My profile " + std::to_wstring(i), false, [i] { Cfg().SetStr(L"sidebar.profile." + std::to_wstring(i), SbCurrentProfile()); Cfg().Save(); });
            chips(L"Save look as", sv);
        }
        Section(c, L"Reset");
        chips(L"Defaults", {{L"Restore the default look", false, [] { SbApplyProfile(SbBuiltinProfiles()[0].text); }}});
}
MiniCfg MiniLoad();
void MiniApplyProfile(const std::wstring& text);
void PageMiniSet(Ctx& c, const Cache&) {
        auto opt = [](const std::wstring& key, int v) { return [key, v] { MiniSetOption(key, v); }; };
        const bool ct = Cfg().Bool(L"mini.clickThrough", false), graphs = Cfg().Bool(L"mini.graphs", true);
        const int layer = Cfg().Has(L"mini.layer") ? Cfg().Int(L"mini.layer", 0, 0, 1) : (Cfg().Bool(L"mini.topmost", true) ? 0 : 1);
        const int layout = Cfg().Int(L"mini.layout", 1, 0, 1), op = Cfg().Int(L"mini.opacity", 85, 10, 100);
        const uint32_t mets = static_cast<uint32_t>(Cfg().Int(L"mini.metrics", static_cast<int>(kMiniDefaultMetrics), 0, static_cast<int>(kMiniAllMetrics)));
        Section(c, L"Content", L"What the Mini Monitor shows.");
        Chips(c, L"Layout", {{L"Compact", layout == 0, opt(L"mini.layout", 0)}, {L"Expanded", layout == 1, opt(L"mini.layout", 1)}});
            static const wchar_t* names[kMiniMetricCount] = {L"CPU load", L"CPU temp", L"GPU load", L"GPU temp", L"RAM", L"Fan", L"Network", L"Clock"};
            std::vector<ChipOpt> ms;
            for (int i = 0; i < kMiniMetricCount; ++i) {
                const uint32_t bit = MiniMetricAt(i);
                ms.emplace_back(names[i], (mets & bit) != 0, [mets, bit] { MiniSetOption(L"mini.metrics", static_cast<int>(mets ^ bit)); });
            }
            Chips(c, L"Metrics", ms);
        Chips(c, L"Graphs", {{L"On (expanded)", graphs, opt(L"mini.graphs", 1)}, {L"Off", !graphs, opt(L"mini.graphs", 0)}});
        Section(c, L"Size and place");
            const int mw = Cfg().Int(L"mini.width", 0, 0, 400), mr = Cfg().Int(L"mini.rowHeight", 0, 0, 36), sc = Cfg().Int(L"mini.scale", 100, 80, 200);
            const int shownW = mw ? std::max(150, mw) : (layout == 1 ? 232 : 168);
            Chips(c, (L"Width  " + std::to_wstring(shownW)).c_str(), {{L"\u2212 10", false, opt(L"mini.width", std::max(150, shownW - 10))}, {L"200", mw == 200, opt(L"mini.width", 200)}, {L"260", mw == 260, opt(L"mini.width", 260)},
                              {L"320", mw == 320, opt(L"mini.width", 320)}, {L"+ 10", false, opt(L"mini.width", std::min(400, shownW + 10))}, {L"Automatic", mw == 0, opt(L"mini.width", 0)}});
            Chips(c, L"Line height", {{L"18", mr == 18, opt(L"mini.rowHeight", 18)}, {L"26", mr == 26, opt(L"mini.rowHeight", 26)}, {L"32", mr == 32, opt(L"mini.rowHeight", 32)}, {L"Automatic", mr == 0, opt(L"mini.rowHeight", 0)}});
            Chips(c, L"Size", {{L"80%", sc == 80, opt(L"mini.scale", 80)}, {L"125%", sc == 125, opt(L"mini.scale", 125)}, {L"150%", sc == 150, opt(L"mini.scale", 150)}, {L"100%", sc == 100, opt(L"mini.scale", 100)}});
            Chips(c, L"Move to", {{L"Top left", false, [] { MiniToCorner(0); }}, {L"Top right", false, [] { MiniToCorner(1); }}, {L"Bottom left", false, [] { MiniToCorner(2); }}, {L"Bottom right", false, [] { MiniToCorner(3); }}});
        Section(c, L"Look", L"Independent of the Sidebar's look.");
        Chips(c, L"Background", {{L"25%", op == 25, opt(L"mini.opacity", 25)}, {L"50%", op == 50, opt(L"mini.opacity", 50)}, {L"75%", op == 75, opt(L"mini.opacity", 75)}, {L"85%", op == 85, opt(L"mini.opacity", 85)}, {L"100%", op == 100, opt(L"mini.opacity", 100)}});
            const int mb = Cfg().Has(L"mini.background") ? Cfg().Int(L"mini.background", 0, 0, 2) : (Cfg().Bool(L"appearance.widgetGlass", false) ? 2 : 0), mt = Cfg().Int(L"mini.tint", 70, 0, 100);
            Chips(c, L"Glass", {{L"Solid", mb == 0, opt(L"mini.background", 0)}, {L"Frosted glass", mb == 1, opt(L"mini.background", 1)}, {L"Acrylic blur", mb == 2, opt(L"mini.background", 2)}});
            if (mb > 0) Chips(c, L"   Tint", {{L"30%", mt == 30, opt(L"mini.tint", 30)}, {L"50%", mt == 50, opt(L"mini.tint", 50)}, {L"70%", mt == 70, opt(L"mini.tint", 70)}, {L"90%", mt == 90, opt(L"mini.tint", 90)}});
            struct Sw { const wchar_t* n; int rgb; };
            static const Sw sw[] = {{L"Blue", 0x00AEFF}, {L"Cyan", 0x22D3EE}, {L"Teal", 0x2DD4BF}, {L"Green", 0x2ECC71}, {L"Amber", 0xF5B041}, {L"Red", 0xFF5C5C}, {L"Pink", 0xFF6FB5}, {L"Violet", 0xA78BFA}, {L"White", 0xE8F4FF}, {L"Theme colour", -1}};
            const int ma = Cfg().Int(L"mini.accent", -1, -1, 0xFFFFFF), mx = Cfg().Int(L"mini.textColor", -1, -1, 0xFFFFFF), mg = Cfg().Int(L"mini.glow", 0, 0, 2);
            std::vector<ChipOpt> oa, ox; for (const Sw& w2 : sw) { oa.emplace_back(w2.n, ma == w2.rgb, opt(L"mini.accent", w2.rgb)); ox.emplace_back(w2.n, mx == w2.rgb, opt(L"mini.textColor", w2.rgb)); }
            Chips(c, L"Accent", oa);
            Chips(c, L"Text colour", ox);
            Chips(c, L"Glow", {{L"Soft", mg == 1, opt(L"mini.glow", 1)}, {L"Strong", mg == 2, opt(L"mini.glow", 2)}, {L"Off", mg == 0, opt(L"mini.glow", 0)}});
        Section(c, L"Profiles", L"A profile holds the Mini Monitor's layout, metrics, graphs, opacity, glass look and per-metric styles.");
        {
            std::vector<ChipOpt> profs;
            const std::wstring cur = MiniSerializeProfile(MiniLoad());
            for (const MiniBuiltin& b : MiniBuiltinProfiles()) {
                const std::wstring t = b.text;
                profs.emplace_back(b.name, t == cur, [t] { MiniApplyProfile(t); });
            }
            for (int i = 1; i <= 3; ++i) {
                const std::wstring t = Cfg().Str(L"mini.profile." + std::to_wstring(i), L"");
                if (!t.empty()) profs.emplace_back(L"My profile " + std::to_wstring(i), t == cur, [t] { MiniApplyProfile(t); });
            }
            Chips(c, L"Profile", profs);
            std::vector<ChipOpt> sv;
            for (int i = 1; i <= 3; ++i) sv.emplace_back(L"My profile " + std::to_wstring(i), false, [i] {
                Cfg().SetStr(L"mini.profile." + std::to_wstring(i), MiniSerializeProfile(MiniLoad()));
                Cfg().Save();
            });
            Chips(c, L"Save look as", sv);
        }
        Section(c, L"Per-metric display style", L"Choose Value, Bar, or inline Sparkline for each active metric.");
        {
            auto st = MiniParseStyles(Cfg().Str(L"mini.styles", L""));
            for (int i = 0; i < kMiniMetricCount; ++i) {
                if (!(mets & MiniMetricAt(i))) continue;
                std::vector<ChipOpt> opts;
                const bool canSpark = MiniSeries(MiniMetricAt(i)) >= 0;
                const int curSt = st[i];
                auto setSt = [st, i](int v) {
                    auto next = st;
                    next[i] = v;
                    Cfg().SetStr(L"mini.styles", MiniFormatStyles(next));
                    MiniSetOption(L"", 0);
                };
                opts.emplace_back(L"Value", curSt == MiniStyleValue, [setSt] { setSt(MiniStyleValue); });
                if (i != 7 && i != 6) opts.emplace_back(L"Bar", curSt == MiniStyleBar, [setSt] { setSt(MiniStyleBar); });
                if (canSpark) opts.emplace_back(L"Sparkline", curSt == MiniStyleSpark, [setSt] { setSt(MiniStyleSpark); });
                Chips(c, names[i], opts);
            }
        }
        Section(c, L"Reset");
        Chips(c, L"Defaults", {{L"Restore Mini Monitor defaults", false, [] {
            for (const wchar_t* k : {L"mini.width", L"mini.rowHeight", L"mini.glow", L"mini.background"}) Cfg().SetInt(k, 0);
            Cfg().SetInt(L"mini.scale", 100); Cfg().SetInt(L"mini.accent", -1); Cfg().SetInt(L"mini.textColor", -1); Cfg().SetInt(L"mini.tint", 70); Cfg().SetInt(L"mini.opacity", 85);
            Cfg().SetInt(L"mini.layout", 1); Cfg().SetBool(L"mini.graphs", true); Cfg().SetInt(L"mini.metrics", static_cast<int>(kMiniDefaultMetrics));
            Cfg().SetStr(L"mini.styles", L"0,0,0,0,0,0,0,0"); MiniSetOption(L"", 0); }}});
        if (AdvancedGroup(c, PMiniSet, L"Mouse mode and layer.")) {
        Chips(c, L"Mouse", {{L"Interactive", !ct, opt(L"mini.clickThrough", 0)}, {L"Click-through", ct, opt(L"mini.clickThrough", 1)}});
        Chips(c, L"Layer", {{L"Always on top", layer == 0, opt(L"mini.layer", 0)}, {L"Normal", layer == 1, opt(L"mini.layer", 1)}});
        Para(c, L"Click-through: the mouse passes to whatever is under the Mini Monitor. Switch back with Ctrl+Alt+Shift+M or the tray menu. Right-click it to make it click-through; drag it to move it (interactive mode).", c.u.f.small, col::faint);
        }
}
void RestoreSafeFanOnExit() {
    if (kTestBuild || g_fanMode == FanOperatingMode::Automatic) return;
    if (g_read.ControllableFanCount() > 0) g_engine.Fan(-1);
    if (FileExists(NbfcExe()) && !FanCurrentConfig().empty()) FanSetAutoSafe();
}
std::atomic<bool> g_fanNbfcTickBusy{false};
std::atomic<bool> g_fanNbfcTickFailed{false};
ULONGLONG g_fanLastNbfcTick = 0;
std::atomic<ULONGLONG> g_fanLastConfigReapplyTick{0};

void DispatchNbfcFanKeepAlive(int target, bool overrideActive, bool quiet, ULONGLONG now) {
    if (g_fanNbfcTickBusy.exchange(true)) return;
    g_fanLastNbfcTick = now;
    const std::wstring cfgName = g_fanNbfcConfig;
    const FanChannelCalibration cal = g_fanCal;
    const int fanCnt = std::max(2, static_cast<int>(g_read.fans.size()));
    std::vector<std::wstring> fanNames;
    for (const auto& f : g_read.fans) fanNames.push_back(f.name);
    std::thread([target, overrideActive, quiet, now, cfgName, cal, fanCnt, fanNames]() {
        const bool wasSvcUp = ProcessRunning(L"NbfcService.exe");
        const ULONGLONG lastCfgMs = g_fanLastConfigReapplyTick.load();
        // Re-apply the NBFC configuration every 45 seconds (~1 minute) as well as when starting up,
        // so laptop EC 'OnInitialization' registers (e.g. HP ZBook thermal zone select/fake temp) stay locked in.
        const bool needConfigRefresh = !cfgName.empty() && (!wasSvcUp || lastCfgMs == 0 || (now >= lastCfgMs && now - lastCfgMs >= 45000));
        if (needConfigRefresh) {
            const auto cands = DiscoverFanCompatCandidates(L"");
            if (FanApplySafe(cfgName, cands).ok) {
                g_fanLastConfigReapplyTick.store(now);
            }
        }
        const int effectivePct = overrideActive ? 100 : target;
        const FanOpResult res = FanSetSpeedCalibratedSafe(effectivePct, fanCnt, fanNames, cal, nullptr, quiet);
        if (!res.ok) {
            g_fanNbfcTickFailed.store(true);
            if (!quiet) LogW(L"Fan: NBFC speed update failed transiently; will retry (user setting preserved).");
        } else {
            g_fanNbfcTickFailed.store(false);
        }
        g_fanNbfcTickBusy.store(false);
    }).detach();
}

void FanAutoFloorTick(const Readings& r) {
    if (kTestBuild || !kFanControlEnabled) return;
    if (g_fanNbfcTickFailed.exchange(false)) g_fanLastFloorSent = -1;

    const ULONGLONG now = GetTickCount64();
    if (g_fanMode == FanOperatingMode::Manual) {
        const int target = ClampFanDuty40(g_fanPercent);
        if (r.ControllableFanCount() > 0) {
            if (ShouldReassertFanKeepAlive(now, g_fanLastFloorTick, target, g_fanLastFloorSent, false, 5000)) {
                g_engine.Fan(target);
                g_fanLastFloorSent = target;
                g_fanLastFloorTick = now;
            }
        } else if (FileExists(NbfcExe()) && !g_fanNbfcConfig.empty()) {
            // Re-assert NBFC speed every 10s (and full config + speed every 45s) so BIOS/EC cannot drop fan speed.
            if (ShouldReassertFanKeepAlive(now, g_fanLastNbfcTick, target, g_fanLastFloorSent, false, 10000)) {
                const bool quiet = std::abs(target - g_fanLastFloorSent) < 8;   // 2.8.7: the log line is written only for a real change (it was one line every 3 s)
                g_fanLastFloorSent = target;
                DispatchNbfcFanKeepAlive(target, false, quiet, now);
            }
        }
        return;
    }
    if (g_fanMode != FanOperatingMode::AutoFloor) return;
    if (now - g_fanLastFloorTick < 2500) return;
    double maxT = 0.0;
    if (r.present) {
        if (r.cpuTemp > maxT) maxT = r.cpuTemp;
        if (r.gpuTemp > maxT) maxT = r.gpuTemp;
    }
    const bool overrideActive = (maxT >= 92.0);
    // 2.8.7: the demand comes from the TEMPERATURE only. It used to take the fans' own read-back as demand, which after our own command is just that command: one spike to 100% kept the target at 100% even at 60 °C.
    int target = ComputeAutoFloorTargetPct(g_fanFloorPercent, maxT, 0.0, overrideActive);
    // Apply 3% hysteresis between keep-alive intervals to prevent 1-2 C jitter from spamming NBFC every 2.5s
    if (!overrideActive && g_fanLastFloorSent >= kMinFanDutyPct &&
        target != g_fanFloorPercent && std::abs(target - g_fanLastFloorSent) < 4) {
        target = g_fanLastFloorSent;
    }
    target = std::max(ClampFanFloor(g_fanFloorPercent), target);
    if (r.ControllableFanCount() > 0) {
        if (ShouldReassertFanKeepAlive(now, g_fanLastFloorTick, target, g_fanLastFloorSent, overrideActive, 5000)) {
            g_engine.Fan(overrideActive ? 100 : target);
            g_fanLastFloorSent = target;
            g_fanLastFloorTick = now;
        }
    } else if (FileExists(NbfcExe()) && !g_fanNbfcConfig.empty()) {
        if (ShouldReassertFanKeepAlive(now, g_fanLastNbfcTick, target, g_fanLastFloorSent, overrideActive, 10000)) {
            const bool quiet = std::abs(target - g_fanLastFloorSent) < 8 && !overrideActive;
            g_fanLastFloorSent = target;
            g_fanLastFloorTick = now;
            DispatchNbfcFanKeepAlive(target, overrideActive, quiet, now);
        }
    }
}
bool PromptText(const std::wstring& title, const std::wstring& label, std::wstring& value);
// 2.8.3: the one place where the person's confirmation for AnkhCore's own fan engine is asked and recorded (used by the configuration list and by "Apply configuration").
static bool FanEngineConfirmAndEnable(const FanCompatCandidate& cd) {
    const std::wstring ecFile = ProdDataDir() + L"\\Sensors\\ec-fan.txt";
    if (TestGuard(L"Turn fan control on")) return false;
    if (!Confirm(L"Turn on AnkhCore's own fan control with the configuration \"" + cd.name + L"\"?\n\n"
                 L"AnkhCore will write the fan speed to your notebook's embedded controller using the register map of this configuration" + std::wstring(cd.exactModelMatch ? L" (made for your model)." : cd.familyMatch ? L" (made for a model of the same family: experimental)." : L" (NOT made for your model or its family: you chose it yourself; a wrong register map can make the fans misbehave or hang the notebook).") +
                 L"\n\nSafeguards: a 40% minimum, a thermal override, the BIOS gets the fans back on automatic, on exit and on any error, and the engine switches itself off for good if the embedded controller ever stops answering. "
                 L"Watch the temperatures the first time.", L"Fan control", true)) return false;
    const std::wstring leaf = cd.path.substr(cd.path.find_last_of(L'\\') + 1);
    const std::string txt = "AnkhCore fan control: confirmed by the person.\nconfig=" + WToUtf8(leaf) + "\nsha256=" + Sha256File(cd.path) + "\nconfirmed=1\n";
    EnsureDir(DirOf(ecFile));
    WriteAll(ecFile, reinterpret_cast<const uint8_t*>(txt.data()), txt.size());
    LogI(L"Fan: AnkhCore fan control confirmed with the configuration '" + cd.name + L"' (the sensor helper applies it within a second).");
    InvalidateRect(g_wnd, nullptr, FALSE);
    return true;
}
FanSupportReport AssessFanSupportCached(const FanSupportFacts& f) { return FanSupportAssess(f); }   // pure and cheap: assessed on every paint from live facts
void PageFan(Ctx& c, const Cache&) {
    const Readings& r = g_read;
    const bool on = Hub().HelperInstalled() && r.present;
    const bool nbfcAvail = FileExists(NbfcExe());
    const bool ecActiveNow = g_read.ecState.rfind(L"active:", 0) == 0;
    const bool ecReadyNow = ecActiveNow || (!g_read.ecState.empty() && g_read.ecState.rfind(L"unavailable", 0) != 0);

    static std::wstring s_cachedModel;
    static bool s_candsLoaded = false;
    static std::vector<FanCompatCandidate> s_cands;
    if (!s_candsLoaded || s_cachedModel != r.model) {
        s_cachedModel = r.model;
        s_cands = DiscoverFanCompatCandidates(r.model);
        s_candsLoaded = true;
    }

    static bool s_nbfcChecked = false;
    static ULONGLONG s_nbfcLastTick = 0;
    static NbfcStatusInfo s_nbfcStatus;
    const ULONGLONG nowTick = GetTickCount64();
    // 2.8.7: nbfc.exe was called here on the UI thread (up to 15 s when the service or the CPU is busy, which froze the window during scans and updates); it now runs on a worker and the page takes the newest answer.
    static std::mutex s_nbfcMx; static NbfcStatusInfo s_nbfcFresh; static std::atomic<bool> s_nbfcFreshReady{false}, s_nbfcQuerying{false};
    if (s_nbfcFreshReady.exchange(false)) { std::lock_guard<std::mutex> g(s_nbfcMx); s_nbfcStatus = s_nbfcFresh; s_nbfcChecked = true; }
    if ((!s_nbfcChecked || nowTick - s_nbfcLastTick >= 3000) && nbfcAvail && !kTestBuild && !s_nbfcQuerying.exchange(true)) {
        s_nbfcLastTick = nowTick;
        std::thread([] {
            const NbfcStatusInfo st = ParseNbfcStatus(NbfcRun(L"status"));
            { std::lock_guard<std::mutex> g(s_nbfcMx); s_nbfcFresh = st; }
            s_nbfcFreshReady = true; s_nbfcQuerying = false;
            if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0);
        }).detach();
    }

    int activeCfgFans = 0;
    for (const auto& cd : s_cands) {
        if (!s_nbfcStatus.selectedConfig.empty() && _wcsicmp(cd.name.c_str(), s_nbfcStatus.selectedConfig.c_str()) == 0) {
            activeCfgFans = cd.fanCount;
            break;
        }
    }
    if (activeCfgFans == 0 && !s_nbfcStatus.fanNames.empty()) activeCfgFans = static_cast<int>(s_nbfcStatus.fanNames.size());

    const int ctlCnt = r.ControllableFanCount();
    const FanControlCapability cap = AssessFanControlCapability(
        static_cast<int>(r.fans.size()), ctlCnt, nbfcAvail, s_nbfcStatus.selectedConfig, activeCfgFans);

    if (cap.kind == FanBackendKind::ConfigurationBased) {
        Para(c, L"WARNING: Experimental fan control. Applying unsupported configurations may cause overheating or hardware instability. Use at your own risk.", c.u.f.small, col::err);
    } else if (cap.kind == FanBackendKind::DirectManual) {
        Para(c, L"\x26A0 Manual fan control enforces a 40% minimum duty and thermal override (92 \u00B0C / 8 s). Press \"Return to automatic (BIOS)\" at any time.", c.u.f.small, col::warn);
    }

    {
        RowSpec row; row.icon = L"\xE9CA"; row.title = L"Fan Control Capability";
        row.desc = !Hub().HelperInstalled() ? L"The monitoring helper is not installed." :
                   !r.present ? L"Start monitoring on the Monitor tab for live speeds.  " + cap.detail :
                   cap.detail;
        if (!r.model.empty()) row.chips.push_back({r.model, col::accent});
        if (on && !r.fans.empty()) row.chips.push_back({std::to_wstring(r.fans.size()) + (r.fans.size() == 1 ? L" fan" : L" fans"), col::sub});
        row.chips.push_back({cap.statusLabel, cap.kind == FanBackendKind::DirectManual ? col::ok : cap.kind == FanBackendKind::ConfigurationBased ? col::warn : col::sub});
        if (cap.perFanSupported) row.chips.push_back({L"Per-fan supported", col::accent});
        Row(c, row);
    }

    {   // 2.8: what AnkhCore can honestly do with the fans of THIS PC (reading and control are separate; control follows the model)
        FanSupportFacts ff;
        ff.manufacturer = RegStr(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemManufacturer"); ff.model = r.model;
        ff.helperRunning = on; ff.fans = static_cast<int>(r.fans.size());
        for (const auto& f : r.fans) {
            if (f.rpm >= 0) ++ff.fansWithRpm;
            if (f.source == L"HP BIOS") ff.hpBiosFan = true; else if (f.source == L"ThinkPad EC") ff.thinkpadEcFan = true; else if (f.source == L"Dell SMM") ff.dellSmmFan = true; else if (f.source.find(L"Dell") != std::wstring::npos) ff.dellCmFan = true; else ff.sensorHelperFan = true;
        }
        ff.directControllable = ctlCnt; ff.nbfcInstalled = nbfcAvail;
        ff.ecActive = r.ecState.rfind(L"active:", 0) == 0;
        ff.ecReady = ff.ecActive || (!r.ecState.empty() && r.ecState.rfind(L"unavailable", 0) != 0);
        ff.activeConfig = ff.ecActive ? FanTrim(r.ecState.substr(7)) : s_nbfcStatus.selectedConfig;
        for (const auto& cd : s_cands) { if (cd.exactModelMatch) ++ff.configsExact; else if (cd.familyMatch) ++ff.configsFamily; }
        SYSTEM_POWER_STATUS ps{}; ff.isNotebook = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
        const std::wstring pawnOff = ProdDataDir() + L"\\Sensors\\disable-pawnio-fans.txt";
        ff.pawnReadersOff = FileExists(pawnOff);
        const FanSupportReport fr = AssessFanSupportCached(ff);
        Section(c, L"Fan support for this PC", L"Reading (what can be measured) and control (what can be set) are decided separately, and control follows your model.");
        RowSpec row; row.icon = L"\xE9CA"; row.title = (fr.maker.empty() ? std::wstring(L"This PC") : fr.maker) + (ff.model.empty() ? std::wstring() : L"  \x00B7  " + ff.model);
        row.desc = fr.readingLine + L"  " + fr.controlLine;
        row.chips.push_back({FanReadingLabel(fr.reading), fr.reading == FanReadState::Live ? col::ok : fr.reading == FanReadState::Partial ? col::warn : col::faint});
        row.chips.push_back({FanControlRouteLabel(fr.control), fr.canControl ? col::ok : col::faint});
        Row(c, row);
        for (const auto& t : fr.sourcesTried) Para(c, L"\x2022 " + t, c.u.f.small, col::sub);
        for (const auto& t : fr.steps) Para(c, L"\x2192 " + t, c.u.f.small, col::warn);
        {   // 2.8: AnkhCore's own fan control (no NBFC, no other program): the notebook configurations are built in; the person chooses and confirms one
            if (!r.ecState.empty()) Para(c, L"AnkhCore fan engine: " + r.ecState, c.u.f.small, ff.ecActive ? col::ok : col::sub);
            const std::wstring ecFile = ProdDataDir() + L"\\Sensors\\ec-fan.txt";
            if (ff.ecActive) {
                Buttons(c, {{L"Turn AnkhCore fan control off (BIOS control)", Btn::Danger, true, [ecFile] {
                    if (TestGuard(L"Turn fan control off")) return;
                    DeleteFileW(ecFile.c_str()); LogI(L"Fan: AnkhCore fan control turned off; the BIOS controls the fans again (the sensor helper applies it within a second).");
                    InvalidateRect(g_wnd, nullptr, FALSE); }}});
            } else if (ff.ecReady && ff.directControllable == 0 && ff.fans <= 2 + 2) {
                std::vector<const FanCompatCandidate*> pick;
                for (const auto& cd : s_cands) if (cd.exactModelMatch) pick.push_back(&cd);
                for (const auto& cd : s_cands) if (!cd.exactModelMatch && cd.familyMatch) pick.push_back(&cd);
                for (size_t i = 0; i < pick.size() && i < 6; ++i) {
                    const FanCompatCandidate cd = *pick[i];
                    RowSpec cr; cr.icon = L"\xE9CA"; cr.title = cd.name;
                    cr.desc = std::wstring(cd.exactModelMatch ? L"Made for this exact model" : L"Same model family (experimental)") + L"  \x00B7  " + std::to_wstring(cd.fanCount) + (cd.fanCount == 1 ? L" fan" : L" fans") + (cd.criticalTempC > 0 ? L"  \x00B7  critical " + std::to_wstring(cd.criticalTempC) + L" \x00B0" L"C" : std::wstring());
                    cr.chips.push_back({cd.exactModelMatch ? L"Exact" : L"Family", cd.exactModelMatch ? col::ok : col::warn});
                    cr.buttons = {{L"Use this configuration", cd.exactModelMatch ? Btn::Primary : Btn::Secondary, true, [cd] { FanEngineConfirmAndEnable(cd); }}};
                    Row(c, cr);
                }
            }
        }
        if (fr.maker == L"Dell" || fr.maker == L"Lenovo") {   // the PawnIO readers: read-only, and switchable if one ever misbehaves on a PC
            Buttons(c, {{ff.pawnReadersOff ? L"Turn ThinkPad / Dell fan reading on" : L"Turn ThinkPad / Dell fan reading off", Btn::Secondary, true, [pawnOff, off = ff.pawnReadersOff] {
                if (TestGuard(L"Fan reading switch")) return;
                if (off) { DeleteFileW(pawnOff.c_str()); DeleteFileW((ProdDataDir() + L"\\Sensors\\fan-guard.dellsmm").c_str()); DeleteFileW((ProdDataDir() + L"\\Sensors\\fan-guard.tpec").c_str()); }
                else { EnsureDir(ProdDataDir() + L"\\Sensors"); const std::string t = "AnkhCore: the ThinkPad / Dell fan readers are off while this file exists.\n"; WriteAll(pawnOff, reinterpret_cast<const uint8_t*>(t.data()), t.size()); }
                LogI(off ? L"ThinkPad / Dell fan reading turned on (takes effect when the sensor helper restarts)." : L"ThinkPad / Dell fan reading turned off (takes effect when the sensor helper restarts).");
                InvalidateRect(g_wnd, nullptr, FALSE); }}});
        }
    }

    static std::vector<int> s_perFanPct;
    if (on && !r.fans.empty()) {
        if (s_perFanPct.size() < r.fans.size()) s_perFanPct.resize(r.fans.size(), SensorHub::kMinFanPercent);
        Section(c, L"Current speed & per-fan targets");
        for (size_t idx = 0; idx < r.fans.size(); ++idx) {
            const auto& f = r.fans[idx];
            RowSpec row; row.title = f.name.empty() ? (L"Fan " + std::to_wstring(idx + 1)) : f.name;
            const double tgtPct = (f.controllable && f.percent >= 0) ? f.percent
                                : (cap.kind == FanBackendKind::ConfigurationBased && idx < s_nbfcStatus.targetSpeeds.size()) ? s_nbfcStatus.targetSpeeds[idx] : -1.0;
            const double appPct = (f.controllable && r.fanReadback >= 0) ? r.fanReadback
                                : (cap.kind == FanBackendKind::ConfigurationBased && idx < s_nbfcStatus.currentSpeeds.size()) ? s_nbfcStatus.currentSpeeds[idx] : -1.0;
            const bool isAuto = (cap.kind == FanBackendKind::DirectManual) ? (r.fanMode != L"manual")
                              : (s_nbfcStatus.selectedConfig.empty() || s_nbfcStatus.autoControlEnabled);
            const FanChannelRole chRole = ClassifyFanChannelRole(static_cast<int>(idx), std::max(activeCfgFans, static_cast<int>(r.fans.size())), f.name);
            const FanTelemetryView tv = FormatFanTelemetryView(
                r.present, f.rpm, f.live, f.capability == FanCapability::ControlOnly, tgtPct, appPct, isAuto);
            row.right = tv.actualRpmLabel;
            row.rightColor = tv.physicalVerified ? col::accent : col::sub;
            if (chRole != FanChannelRole::Unknown) {
                const int chOff = (chRole == FanChannelRole::Cpu) ? g_fanCal.cpuOffsetPct : g_fanCal.gpuOffsetPct;
                row.chips.push_back({std::wstring(chRole == FanChannelRole::Cpu ? L"CPU Channel" : L"GPU Channel") +
                                     (chOff != 0 ? (L" (" + std::wstring(chOff > 0 ? L"+" : L"") + std::to_wstring(chOff) + L"% cal)") : L""),
                                     chOff != 0 ? col::accent : col::sub});
            }
            if (!tv.targetLabel.empty()) row.chips.push_back({tv.targetLabel, col::warn});
            if (!tv.appliedLabel.empty()) row.chips.push_back({tv.appliedLabel, col::ok});
            row.chips.push_back({tv.controlStateLabel, tv.biosAutoOverride ? col::sub : col::warn});
            row.chips.push_back({f.source.empty() ? L"Sensor" : f.source, col::accent});
            row.chips.push_back({FanCapabilityLabel(f), f.controllable ? col::warn : col::sub});
            if (f.controllable && f.maxPercent > 0) {
                const int effMin = std::max(SensorHub::kMinFanPercent, static_cast<int>(f.minPercent >= 0 ? f.minPercent : 0));
                row.chips.push_back({std::to_wstring(effMin) + L"-" + std::to_wstring(static_cast<int>(f.maxPercent)) + L"%", col::sub});
            }
            if (!tv.physicalVerified) {
                row.chips.push_back({L"Physical speed unverified", col::warn});
                row.desc = L"Physical fan speed cannot be verified (no live hardware RPM reading). Target/Applied values reflect controller state only.";
            } else if (f.capability == FanCapability::VendorReadOnly && cap.kind == FanBackendKind::ConfigurationBased) {
                row.desc = L"Vendor WMI RPM sensor (live). Controlled via NBFC configuration when active.";
            } else if (f.capability == FanCapability::VendorReadOnly) {
                row.desc = L"Read-only vendor sensor (live). Hardware control is not exposed through this interface.";
            } else if (!f.controllable) {
                row.desc = L"RPM sensor only; no writable hardware control channel exposed.";
            }
            Row(c, row);
        }
    }

    static int s_selCand = -1;
    static bool s_pickNbfcConfig = false;
    static bool s_nbfcFilterMatchOnly = false;
    if (nbfcAvail || !s_cands.empty()) {
        if (s_selCand < 0 && !s_cands.empty()) {
            s_selCand = 0;
            const std::wstring pref = !s_nbfcStatus.selectedConfig.empty() ? s_nbfcStatus.selectedConfig : g_fanNbfcConfig;
            if (!pref.empty()) {
                for (int i = 0; i < static_cast<int>(s_cands.size()); ++i)
                    if (_wcsicmp(s_cands[i].name.c_str(), pref.c_str()) == 0) { s_selCand = i; break; }
            }
        }
        if (s_selCand >= static_cast<int>(s_cands.size())) s_selCand = 0;
        Section(c, L"Configuration-based control (NBFC)", L"Select a validated NBFC XML profile when direct hardware control is unavailable. Nothing is applied without explicit confirmation.");
        Para(c, L"WARNING: Experimental fan control. Applying unsupported configurations may cause overheating or hardware instability. Use at your own risk.", c.u.f.small, col::err);

        if (s_pickNbfcConfig && !s_cands.empty()) {
            static std::wstring s_nbfcBrand;
            static std::wstring s_nbfcSearch;
            const bool showingConfigs = !s_nbfcBrand.empty() || !s_nbfcSearch.empty() || s_nbfcFilterMatchOnly;

            if (!showingConfigs) {
                Buttons(c, {
                    {L"\x2190 Back to Fan Control", Btn::Secondary, true, [] { s_pickNbfcConfig = false; s_nbfcBrand.clear(); s_nbfcSearch.clear(); g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE); }},
                    {L"Search configs\x2026", Btn::Primary, true, [] {
                        std::wstring q = s_nbfcSearch;
                        if (PromptText(L"Search Fan Configurations", L"Enter model, brand, or config name (e.g. ZBook, ThinkPad, ROG):", q)) {
                            s_nbfcSearch = FanTrim(q); g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE);
                        }
                    }},
                    {L"Recommended matches", Btn::Ghost, true, [] { s_nbfcFilterMatchOnly = true; g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}
                });
                const auto brands = DiscoverFanConfigBrands(s_cands);
                Section(c, L"Select laptop brand", std::to_wstring(brands.size()) + L" brands  \x00B7  " + std::to_wstring(s_cands.size()) + L" total configurations");
                for (const auto& b : brands) {
                    RowSpec br; br.icon = L"\xE9CA"; br.title = b.brand;
                    br.desc = std::to_wstring(b.count) + (b.count == 1 ? L" configuration" : L" configurations");
                    if (b.hasExact) br.chips.push_back({L"Exact match", col::ok});
                    else if (b.hasFamily) br.chips.push_back({L"Family match", col::accent});
                    const std::wstring bName = b.brand;
                    br.buttons = {{L"Open", b.hasExact || b.hasFamily ? Btn::Primary : Btn::Secondary, true, [bName] {
                        s_nbfcBrand = bName; s_nbfcSearch.clear(); g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE);
                    }}};
                    Row(c, br);
                }
                return;
            }

            std::vector<BtnSpec> bar = {
                {L"\x2190 All Brands", Btn::Secondary, true, [] { s_nbfcBrand.clear(); s_nbfcSearch.clear(); s_nbfcFilterMatchOnly = false; g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE); }},
                {s_nbfcSearch.empty() ? L"Search\x2026" : L"Search: " + s_nbfcSearch, Btn::Primary, true, [] {
                    std::wstring q = s_nbfcSearch;
                    if (PromptText(L"Search Fan Configurations", L"Filter by model, brand, or config name:", q)) {
                        s_nbfcSearch = FanTrim(q); g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE);
                    }
                }}
            };
            if (!s_nbfcSearch.empty()) bar.push_back({L"Clear search", Btn::Ghost, true, [] { s_nbfcSearch.clear(); g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE); }});
            bar.push_back({s_nbfcFilterMatchOnly ? L"Show All" : L"Recommended only", Btn::Ghost, true, [] { s_nbfcFilterMatchOnly = !s_nbfcFilterMatchOnly; g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE); }});
            Buttons(c, bar);

            const std::vector<int> indices = FilterFanConfigIndices(s_cands, s_nbfcBrand, s_nbfcSearch, s_nbfcFilterMatchOnly);
            const std::wstring heading = (!s_nbfcBrand.empty() ? s_nbfcBrand : L"All brands") +
                                         (!s_nbfcSearch.empty() ? L"  \x00B7  Search \"" + s_nbfcSearch + L"\"" : L"");
            Section(c, heading, std::to_wstring(indices.size()) + L" matching configuration(s)");
            for (int idx : indices) {
                const auto& cd = s_cands[idx];
                RowSpec cr; cr.icon = L"\xE9CA"; cr.title = cd.name;
                cr.desc = std::to_wstring(cd.fanCount) + L" fan(s)" + (cd.criticalTempC > 0 ? L"  \x00B7  Crit " + std::to_wstring(cd.criticalTempC) + L"\u00B0C" : L"") +
                          (!cd.author.empty() ? L"  \x00B7  by " + cd.author : L"");
                if (cd.exactModelMatch) cr.chips.push_back({L"Exact match", col::ok});
                else if (cd.familyMatch) cr.chips.push_back({L"Family match", col::accent});
                if (idx == s_selCand) cr.chips.push_back({L"Selected", col::accent});
                cr.buttons = {{L"Select", idx == s_selCand ? Btn::Primary : Btn::Secondary, true, [idx] {
                    s_selCand = idx; s_pickNbfcConfig = false; g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE);
                }}};
                Row(c, cr);
            }
            if (indices.empty()) Para(c, L"No matching configurations found. Clear the search or go back to All Brands.", c.u.f.small, col::sub);
            return;
        }

        size_t exactCnt = 0, famCnt = 0;
        for (const auto& cd : s_cands) { if (cd.exactModelMatch) ++exactCnt; else if (cd.familyMatch) ++famCnt; }
        RowSpec row; row.icon = L"\xE9CA";
        row.title = s_nbfcStatus.selectedConfig.empty() ? L"Active configuration: None" : (L"Active configuration: " + s_nbfcStatus.selectedConfig);
        if (s_cands.empty()) {
            row.desc = L"No local NBFC XML configurations found.";
        } else {
            const FanCompatCandidate& sel = s_cands[s_selCand];
            row.desc = L"Selected profile (" + std::to_wstring(s_selCand + 1) + L"/" + std::to_wstring(s_cands.size()) + L"): " +
                       sel.name + L" (" + std::to_wstring(sel.fanCount) + L" fan(s), " +
                       (sel.exactModelMatch ? L"exact model match" : sel.familyMatch ? L"family match" : L"generic") + L").";
        }
        row.chips.push_back({std::to_wstring(exactCnt) + L" exact, " + std::to_wstring(famCnt) + L" family", col::accent});
        if (g_fanNbfcConfig.empty() && !s_nbfcStatus.selectedConfig.empty()) {
            g_fanNbfcConfig = s_nbfcStatus.selectedConfig;
            SaveFanPolicyCfg();
        }
        if (!s_nbfcStatus.selectedConfig.empty()) row.chips.push_back({L"Profile active", col::warn});
        if (!s_cands.empty()) {
            const std::wstring selName = s_cands[s_selCand].name;
            const FanCompatCandidate selCandCopy = s_cands[s_selCand];
            FanLevelFacts lf;
            lf.reading = (!Hub().HelperInstalled() || !r.present) ? FanReadState::HelperOff : (r.fans.empty() ? FanReadState::NotExposed : FanReadState::Live);
            lf.controlSupported = nbfcAvail || ecReadyNow || r.ControllableFanCount() > 0;
            lf.configsAvailable = static_cast<int>(exactCnt + famCnt);
            lf.haveSelection = true; lf.selExact = selCandCopy.exactModelMatch; lf.selFamily = selCandCopy.familyMatch;
            lf.engineReady = ecReadyNow; lf.nbfcInstalled = nbfcAvail; lf.controlEnabled = kFanControlEnabled; lf.testBuild = kTestBuild;
            lf.alreadyActive = ecActiveNow && FanTrim(r.ecState.substr(7)) == selCandCopy.name;
            lf.engineState = r.ecState;
            const FanLevels applyLv = FanAssessLevels(lf);
            row.chips.push_back({applyLv.monitoring ? L"Monitoring: yes" : L"Monitoring: no", applyLv.monitoring ? col::ok : col::faint});
            row.chips.push_back({applyLv.configVerified ? L"Config: verified for this model" : selCandCopy.familyMatch ? L"Config: family only (unverified)" : L"Config: not verified", applyLv.configVerified ? col::ok : col::warn});
            row.chips.push_back({applyLv.operationAvailable ? L"Control: available" : L"Control: not available", applyLv.operationAvailable ? col::ok : col::faint});
            if (applyLv.unverified) row.chips.push_back({L"Not for this model: your choice", col::err});
            const int selFanCnt = std::max(2, s_cands[s_selCand].fanCount);
            row.buttons = {
                {L"Browse all (" + std::to_wstring(s_cands.size()) + L")", Btn::Secondary, true, [] { s_pickNbfcConfig = true; InvalidateRect(g_wnd, nullptr, FALSE); }},
                {L"Apply configuration", Btn::Danger, applyLv.operationAvailable, [selName, selFanCnt, &r, selCandCopy, viaEngine = applyLv.viaEngine] {
                    if (viaEngine) { FanEngineConfirmAndEnable(selCandCopy); return; }   // 2.8.3: no NBFC needed: AnkhCore's own engine, same confirmation and safeguards
                    if (TestGuard(L"Apply NBFC configuration")) return;
                    if (!Confirm(L"WARNING: Experimental fan control. Applying unsupported configurations may cause overheating or hardware instability. Use at your own risk.\n\nApply NBFC configuration \"" + selName + L"\"?", L"Confirm Experimental Fan Configuration", true)) return;
                    g_fanNbfcConfig = selName;
                    // If user previously returned to BIOS Automatic, automatically re-engage their last active speed mode
                    // (Manual or AutoFloor) and apply their selected speed immediately upon applying the engine!
                    if (g_fanMode == FanOperatingMode::Automatic) {
                        g_fanMode = (g_fanLastActiveMode != FanOperatingMode::Automatic) ? g_fanLastActiveMode : FanOperatingMode::AutoFloor;
                    }
                    g_fanLastFloorSent = -1;
                    SaveFanPolicyCfg();
                    const FanOperatingMode activeMode = g_fanMode;
                    const double maxT = std::max(r.cpuTemp, r.gpuTemp);
                    const int initialPct = (activeMode == FanOperatingMode::Manual)
                        ? ClampFanDuty40(g_fanPercent)
                        : std::max(ClampFanFloor(g_fanFloorPercent), ComputeAutoFloorTargetPct(g_fanFloorPercent, maxT, 0.0, maxT >= 92.0));
                    const FanChannelCalibration cal = g_fanCal;
                    std::vector<std::wstring> fNames = s_nbfcStatus.fanNames;
                    if (fNames.empty()) { for (const auto& fn : r.fans) fNames.push_back(fn.name); }
                    RunOp(L"Apply NBFC config " + selName, [selName, selFanCnt, initialPct, cal, fNames] {
                        const FanOpResult res = FanApplySafe(selName, s_cands);
                        if (!res.ok) { s_nbfcChecked = false; LogE(res.message); return false; }
                        g_fanLastConfigReapplyTick.store(GetTickCount64());
                        const FanOpResult spdRes = FanSetSpeedCalibratedSafe(initialPct, selFanCnt, fNames, cal);
                        s_nbfcChecked = false;
                        if (!spdRes.ok) { LogE(spdRes.message); return false; }
                        g_fanLastFloorSent = initialPct;
                        g_fanLastNbfcTick = GetTickCount64();
                        LogOk(res.message + L" " + spdRes.message);
                        return true;
                    });
                }},
                {L"Restore automatic (BIOS)", Btn::Secondary, kFanControlEnabled && nbfcAvail, [] {
                    if (TestGuard(L"Restore NBFC automatic")) return;
                    if (g_fanMode != FanOperatingMode::Automatic) g_fanLastActiveMode = g_fanMode;
                    g_fanMode = FanOperatingMode::Automatic; g_fanLastFloorSent = -1; SaveFanPolicyCfg();
                    RunOp(L"Restore automatic fans (BIOS)", [] {
                        const FanOpResult res = FanSetAutoSafe();
                        s_nbfcStatus = NbfcStatusInfo{};
                        s_nbfcChecked = false;
                        if (!res.ok) { LogE(res.message); return false; }
                        LogOk(res.message); return true;
                    });
                }}
            };
        }
        Row(c, row);
        if (!s_cands.empty()) {   // 2.8.3: say why Apply is off instead of leaving a grey button
            const FanCompatCandidate& sc = s_cands[s_selCand];
            FanLevelFacts lf2; lf2.haveSelection = true; lf2.selExact = sc.exactModelMatch; lf2.selFamily = sc.familyMatch; lf2.engineReady = ecReadyNow; lf2.nbfcInstalled = nbfcAvail;
            lf2.controlEnabled = kFanControlEnabled; lf2.testBuild = kTestBuild; lf2.engineState = r.ecState;
            lf2.alreadyActive = ecActiveNow && FanTrim(r.ecState.substr(7)) == sc.name;
            const FanLevels lv2 = FanAssessLevels(lf2);
            if (!lv2.reason.empty()) Para(c, L"Apply is off: " + lv2.reason, c.u.f.small, col::sub);
            else Para(c, lv2.viaEngine ? L"Apply uses AnkhCore's own fan engine (no other program needed) after you confirm." : L"Apply uses the installed NBFC program after you confirm.", c.u.f.small, col::sub);
        }
    }

    // ---- 2.8.6: the only fan option is a minimum speed that rises with the temperature (no fixed speed, no per-fan targets, no calibration to set)
    FanCompatCandidate floorCand;   // the configuration chosen in the list above (an empty name means none)
    if (!s_cands.empty() && s_selCand >= 0 && s_selCand < static_cast<int>(s_cands.size())) floorCand = s_cands[s_selCand];
    std::wstring floorCfg = floorCand.name;
    const FanFloorRoute floorRoute = FanFloorRouteOf(kFanControlEnabled, cap.kind == FanBackendKind::DirectManual, nbfcAvail, !floorCfg.empty(), ecReadyNow || ecActiveNow, !floorCand.name.empty());   // any chosen configuration (2.8.7)
    const bool viaDirect = floorRoute == FanFloorRoute::Direct, useNbfc = floorRoute == FanFloorRoute::Nbfc, viaNewEngine = floorRoute == FanFloorRoute::OwnEngine;

    Section(c, L"Minimum fan speed", L"The only fan setting: the fans never run slower than this, and speed up by themselves as the PC gets hotter.");
    if (!kFanControlEnabled || !(viaDirect || useNbfc || viaNewEngine)) {
        Para(c, nbfcAvail && floorCfg.empty() ? L"NBFC is installed but no configuration is chosen yet. Choose the one for your model above (Browse all), then this setting becomes available."
                : cap.kind == FanBackendKind::MonitoringOnly && !s_cands.empty() ? L"Choose a configuration for your model above to enable the minimum fan speed."
                : L"Monitoring only: this device exposes no supported fan control backend. Your BIOS controls the fans.", c.u.f.small, col::sub);
        return;
    }
    Para(c, viaDirect ? L"Control: direct (the sensor helper)." : useNbfc ? L"Control: NBFC, configuration \"" + floorCfg + L"\"." : L"Control: AnkhCore's own fan engine, configuration \"" + floorCand.name + L"\" (asks you to confirm once).", c.u.f.small, col::faint);

    auto sendAuto = [useNbfc] {
        if (g_fanMode != FanOperatingMode::Automatic) g_fanLastActiveMode = g_fanMode;
        g_fanMode = FanOperatingMode::Automatic; g_fanLastFloorSent = -1; SaveFanPolicyCfg();
        if (useNbfc) {
            RunOp(L"Restore automatic fans (BIOS)", [] {
                const FanOpResult res = FanSetAutoSafe();
                s_nbfcStatus = NbfcStatusInfo{};
                s_nbfcChecked = false;
                if (!res.ok) { LogE(res.message); return false; }
                LogOk(res.message); return true;
            });
        } else {
            g_engine.Fan(-1);
        }
    };
    const int calFanCnt = std::max(2, std::max(activeCfgFans, static_cast<int>(r.fans.size())));
    std::vector<std::wstring> calFanNames = s_nbfcStatus.fanNames;
    if (calFanNames.empty()) { for (const auto& f : r.fans) calFanNames.push_back(f.name); }

    auto applyFloorNow = [useNbfc, viaNewEngine, floorCfg, floorCand, calFanCnt, calFanNames, &r](int floorPct) {
        if (TestGuard(L"Apply minimum fan speed")) return;
        if (viaNewEngine && !FanEngineConfirmAndEnable(floorCand)) return;   // the person confirms once; nothing is written before that
        if (useNbfc && _wcsicmp(g_fanNbfcConfig.c_str(), floorCfg.c_str()) != 0) {
            if (!Confirm(L"WARNING: Experimental fan control. A configuration that does not match the notebook may cause overheating or hardware instability. Use at your own risk.\n\nUse the NBFC configuration \"" + floorCfg + L"\" for the minimum fan speed?", L"Confirm fan configuration", true)) return;
            g_fanNbfcConfig = floorCfg;
        }
        g_fanFloorPercent = ClampFanFloor(floorPct);
        g_fanMode = FanOperatingMode::AutoFloor;
        g_fanLastActiveMode = FanOperatingMode::AutoFloor;
        g_fanLastFloorSent = -1;
        SaveFanPolicyCfg();
        const double maxT = std::max(r.cpuTemp, r.gpuTemp);
        const int targetNow = std::max(g_fanFloorPercent, ComputeAutoFloorTargetPct(g_fanFloorPercent, maxT, 0.0, maxT >= 92.0));
        if (useNbfc) {
            const FanChannelCalibration cal = g_fanCal;
            const std::wstring cfgName = g_fanNbfcConfig;
            RunOp(L"Apply minimum fan speed " + std::to_wstring(g_fanFloorPercent) + L"%", [targetNow, calFanCnt, calFanNames, cal, cfgName] {
                if ((!ProcessRunning(L"NbfcService.exe") || s_nbfcStatus.selectedConfig.empty()) && !cfgName.empty()) {
                    FanApplySafe(cfgName, s_cands);
                    g_fanLastConfigReapplyTick.store(GetTickCount64());
                }
                const FanOpResult res = FanSetSpeedCalibratedSafe(targetNow, calFanCnt, calFanNames, cal);
                s_nbfcChecked = false;
                if (!res.ok) { LogE(res.message); return false; }
                g_fanLastFloorSent = targetNow;
                g_fanLastNbfcTick = GetTickCount64();
                LogOk(res.message); return true;
            });
        } else if (!viaNewEngine) {
            g_engine.Fan(targetNow); g_fanLastFloorSent = targetNow;
        }   // the new engine: the keep-alive sends the speed as soon as the helper reports the fans as controllable
    };

    if (g_fanMode == FanOperatingMode::AutoFloor) {
        const double maxT = std::max(r.cpuTemp, r.gpuTemp);
        const int liveTarget = ComputeAutoFloorTargetPct(g_fanFloorPercent, maxT, 0.0, maxT >= 92.0);
        Para(c, L"Active: fans stay at or above " + std::to_wstring(g_fanFloorPercent) + L"% (" + std::to_wstring(liveTarget) + L"% target now) and rise with the temperature. At 92 °C they go to 100%.", c.u.f.small, col::ok);
    } else {
        Para(c, L"Not active: the BIOS controls the fans. Choose a minimum and press Apply.", c.u.f.small, col::sub);
    }
    Text(c.dc, L"Minimum fan speed (40-100%)", RECT{c.x, c.y, c.x + c.w, c.y + c.S(22)}, c.u.f.btn, col::text); c.y += c.S(30);
    const int sw = c.w - c.S(120), sx = c.x, sy = c.y + c.S(8);
    RECT track{sx, sy, sx + sw, sy + c.S(6)}; Round(c.dc, track, c.S(3), col::track, col::track);
    RECT fill{sx, sy, sx + sw * g_fanFloorPercent / 100, sy + c.S(6)}; Round(c.dc, fill, c.S(3), col::accent, col::accent);
    const int kx = sx + sw * g_fanFloorPercent / 100;
    Ellipse2(c.dc, RECT{kx - c.S(8), sy - c.S(7), kx + c.S(8), sy + c.S(13)}, col::accent, col::accent);
    Text(c.dc, std::to_wstring(g_fanFloorPercent) + L"%", RECT{sx + sw + c.S(16), c.y, c.x + c.w, c.y + c.S(26)}, c.u.f.h2, col::text, DT_VCENTER | DT_SINGLELINE);
    c.Hit(RECT{sx - c.S(8), sy - c.S(14), sx + sw + c.S(8), sy + c.S(20)}, [sx, sw] {
        POINT p = g_ui.mouse; g_fanFloorPercent = ClampFanFloor(static_cast<int>(100.0 * (p.x - sx) / std::max(1, sw))); g_fanLastFloorSent = -1; SaveFanPolicyCfg(); InvalidateRect(g_wnd, nullptr, FALSE);
    });
    c.y += c.S(34);
    Buttons(c, {
        {L"Apply minimum " + std::to_wstring(g_fanFloorPercent) + L"%", Btn::Primary, true, [applyFloorNow] { applyFloorNow(g_fanFloorPercent); }},
        {L"Quiet (40%)", Btn::Secondary, true, [applyFloorNow] { applyFloorNow(40); }},
        {L"Balanced (55%)", Btn::Secondary, true, [applyFloorNow] { applyFloorNow(55); }},
        {L"Cooling (70%)", Btn::Secondary, true, [applyFloorNow] { applyFloorNow(70); }},
        {L"Return to automatic (BIOS)", Btn::Danger, g_fanMode != FanOperatingMode::Automatic, [sendAuto] { if (TestGuard(L"Return fans to automatic")) return; sendAuto(); }}
    });
}
// ---------------------------------------------------------------- window material (Blue Acrylic)
D2DSurface g_surf;        // the main window's renderer
const Px& WinBgPicture(int blurPx) {   // the window background: the sharp picture, or its blurred copy for this blur value (rebuilt only when the value changes)
    if (blurPx <= 0 || g_brandBg.p.empty()) return g_brandBg;
    if (g_brandBgBlurPx != blurPx) { g_brandBgBlur = g_brandBg; CandyBlur(g_brandBgBlur.p, g_brandBgBlur.w, g_brandBgBlur.h, blurPx); g_brandBgBlurPx = blurPx; g_surf.Forget(&g_brandBgBlur); }
    return g_brandBgBlur;
}
Material g_material = MatSolid;
struct StMatOverride { bool on = false; MaterialInput v; } g_stMat;   // self-test only: simulated system state, nothing on the PC changes
MaterialInput ReadMaterialInput() {
    MaterialInput in;
    in.choice = Cfg().Int(L"appearance.material", MatAcrylic, MatAcrylic, MatClear);
    in.frostedOnBattery = Cfg().Bool(L"appearance.frostedOnBattery", true);
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) in.transparencyOn = v != 0;
    HIGHCONTRASTW hc{sizeof(hc)}; if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) in.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    SYSTEM_POWER_STATUS ps{}; if (GetSystemPowerStatus(&ps)) { in.onBattery = ps.ACLineStatus == 0; in.batterySaver = (ps.SystemStatusFlag & 1) != 0; }
    in.build = GetWinVer().build;
    if (kTestBuild && g_stMat.on) { const int c = in.choice; const bool b = in.frostedOnBattery; in = g_stMat.v; in.choice = c; in.frostedOnBattery = b; }
    return in;
}
// ---- wallpaper glass: the wallpaper, blurred once, drawn screen-aligned under the tint (simulated: other windows do not show)
struct WallGlass { Px px; bool tried = false; std::wstring sig; ULONGLONG checked = 0; } g_wall;   // 2.9.7: sig = which desktop picture the blurred copy was made from
void BlurPx(Px& im, int radius) {   // three box passes
    if (im.p.empty() || radius < 1) return;
    std::vector<uint32_t> t(im.p.size());
    auto pass = [radius](const uint32_t* src, uint32_t* dst, int n, int sstep, int dstep) {
        int r = 0, g = 0, b = 0; const int win = 2 * radius + 1;
        auto at = [&](int i) { return src[std::clamp(i, 0, n - 1) * sstep]; };
        for (int i = -radius; i <= radius; ++i) { const uint32_t v = at(i); b += v & 255; g += (v >> 8) & 255; r += (v >> 16) & 255; }
        for (int i = 0; i < n; ++i) {
            dst[i * dstep] = 0xFF000000u | ((r / win) << 16) | ((g / win) << 8) | (b / win);
            const uint32_t a = at(i + radius + 1), d = at(i - radius);
            b += (a & 255) - (d & 255); g += ((a >> 8) & 255) - ((d >> 8) & 255); r += ((a >> 16) & 255) - ((d >> 16) & 255);
        }
    };
    for (int k = 0; k < 3; ++k) {
        for (int y = 0; y < im.h; ++y) pass(&im.p[static_cast<size_t>(y) * im.w], &t[static_cast<size_t>(y) * im.w], im.w, 1, 1);
        for (int x = 0; x < im.w; ++x) pass(&t[x], &im.p[x], im.h, im.w, im.w);
    }
}
extern D2DSurface g_widgetSurf;
// ---- the desktop behind a hosted Sidebar. Windows does not compose per-pixel transparency for a child of the
// desktop, so in Desktop mode the Sidebar draws the wallpaper under itself: this is the sharp, screen-aligned
// piece of wallpaper for the Sidebar's docked rectangle (the same pixels the desktop shows there).
struct SbBackdrop { Px px; RECT rc{}; bool tried = false; int gen = 0; std::wstring sig; ULONGLONG checked = 0; } g_sbBack;
// 2.9.5: what the desktop shows is identified by the picture file (path, size, time it was written) and how it is placed. The copy the Sidebar keeps is checked against this
// every two seconds, so a wallpaper change is picked up even when the "wallpaper changed" broadcast came before Windows finished writing the new file, or never came.
std::wstring SbBackdropSignature() {
    wchar_t path[MAX_PATH] = {};
    std::wstring sig = RegStr(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background") + L"|" + RegStr(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle") + L"|" +
                       RegStr(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper") + L"|";
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) && path[0]) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        sig += path;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa))
            sig += L"|" + std::to_wstring(fa.ftLastWriteTime.dwHighDateTime) + L"." + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"|" + std::to_wstring(fa.nFileSizeLow);
    }
    return sig;
}
void SbBackdropForget() { g_widgetSurf.Forget(&g_sbBack); const int gen = g_sbBack.gen + 1; g_sbBack = SbBackdrop{}; g_sbBack.gen = gen; }
void SbBackdropEnsure(const RECT& rc) {
    if (g_sbBack.tried && EqualRect(&g_sbBack.rc, &rc)) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_sbBack.checked < 2000) return;
        g_sbBack.checked = now;
        if (SbBackdropSignature() == g_sbBack.sig) return;   // the desktop is still the same picture
    }
    const std::wstring sig = SbBackdropSignature();            // taken before the picture is read: a change during the read is caught on the next check
    SbBackdropForget(); g_sbBack.tried = true; g_sbBack.rc = rc; g_sbBack.sig = sig; g_sbBack.checked = GetTickCount64();
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    MONITORINFO mi{sizeof(mi)};
    if (W <= 0 || H <= 0 || !GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const int sw = mi.rcMonitor.right - mi.rcMonitor.left, sh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int br = 0, bg = 0, bb = 0;   // the desktop colour shows where the picture does not reach (Fit, Center) and when there is no picture
    swscanf_s(RegStr(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background").c_str(), L"%d %d %d", &br, &bg, &bb);
    const uint32_t solid = 0xFF000000u | (static_cast<uint32_t>(br & 255) << 16) | (static_cast<uint32_t>(bg & 255) << 8) | static_cast<uint32_t>(bb & 255);
    g_sbBack.px.w = W; g_sbBack.px.h = H; g_sbBack.px.p.assign(static_cast<size_t>(W) * H, solid);
    wchar_t path[MAX_PATH] = {}; std::vector<uint8_t> d;
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0] || !ReadAll(path, d)) return;
    Image32 probe = DecodeImage(d.data(), d.size(), 64, 64);   // only for the picture's proportions
    if (!probe.bmp) return;
    const int pw = probe.w, ph = probe.h; DeleteObject(probe.bmp);
    const WallFit fit = WallFitFromStyle(_wtoi(RegStr(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle").c_str()), RegStr(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper") == L"1");
    WallBox box = WallPlacement(fit, pw, ph, sw, sh);
    if (fit == WallCenter) {   // Center keeps the picture's own size, which the small probe does not carry: decode it whole
        Image32 full = DecodeImage(d.data(), d.size(), 16384, 16384);
        if (!full.bmp) return;
        box = WallPlacement(fit, full.w, full.h, sw, sh); DeleteObject(full.bmp);
    }
    const int bw = std::max(1, static_cast<int>(box.r - box.l + 0.5)), bh = std::max(1, static_cast<int>(box.b - box.t + 0.5));
    Image32 im = DecodeImage(d.data(), d.size(), bw, bh, fit == WallStretch);   // scaled once, to the size it has on screen
    if (!im.bmp) return;
    const Px src = ToPixels(im); DeleteObject(im.bmp);
    if (src.p.empty()) return;
    const double kx = src.w / (box.r - box.l), ky = src.h / (box.b - box.t);
    for (int y = 0; y < H; ++y) {
        const int v = static_cast<int>((rc.top - mi.rcMonitor.top + y + 0.5 - box.t) * ky);
        if (v < 0 || v >= src.h) continue;
        for (int x = 0; x < W; ++x) {
            const int u = static_cast<int>((rc.left - mi.rcMonitor.left + x + 0.5 - box.l) * kx);
            if (u < 0 || u >= src.w) continue;
            const uint32_t s = src.p[static_cast<size_t>(v) * src.w + u], a = s >> 24;   // premultiplied: a picture with transparency lies over the desktop colour
            if (a == 255) { g_sbBack.px.p[static_cast<size_t>(y) * W + x] = s; continue; }
            const uint32_t k = 255 - a;
            g_sbBack.px.p[static_cast<size_t>(y) * W + x] = 0xFF000000u | ((((s >> 16) & 255) + (br & 255) * k / 255) << 16) | ((((s >> 8) & 255) + (bg & 255) * k / 255) << 8) | ((s & 255) + (bb & 255) * k / 255);
        }
    }
}
void WallForget() { g_surf.Forget(&g_wall); g_widgetSurf.Forget(&g_wall); g_wall = WallGlass{}; SbBackdropForget(); }
void WallEnsure() {
    if (g_wall.tried) {
        // 2.9.7: the glass behind the windows and the Sidebar was read once and only dropped on a theme message, so the old desktop picture stayed behind the glass
        // after a wallpaper change (the "old background left as traces"). It is now compared with the desktop's picture every two seconds.
        const ULONGLONG now = GetTickCount64();
        if (now - g_wall.checked < 2000) return;
        g_wall.checked = now;
        if (SbBackdropSignature() == g_wall.sig) return;
        g_surf.Forget(&g_wall); g_widgetSurf.Forget(&g_wall); g_wall = WallGlass{};
    }
    g_wall.tried = true; g_wall.sig = SbBackdropSignature(); g_wall.checked = GetTickCount64();
    wchar_t path[MAX_PATH] = {}; std::vector<uint8_t> d;
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0] || !ReadAll(path, d)) return;
    const bool clear = g_material == MatClear;   // 2.9.8: Clear glass keeps more of the picture (a larger copy, a light blur)
    Image32 im = clear ? DecodeImage(d.data(), d.size(), 1280, 720, true) : DecodeImage(d.data(), d.size(), 480, 270, true);
    if (!im.bmp) return;
    g_wall.px = ToPixels(im); DeleteObject(im.bmp);
    const int lvl = Cfg().Int(L"appearance.blur", 1, 0, 2);
    if (clear) { const int b = ClearBlurPx(Cfg().Int(L"appearance.clearBlur", kClearBlurDefault, 0, kClearBlurMax)); if (b > 0) BlurPx(g_wall.px, b); }   // 2.10: the card blur is the person's own setting
    else BlurPx(g_wall.px, lvl == 0 ? 3 : lvl == 1 ? 7 : 12);
}
void WallDraw(Dc dc, HWND w, const RECT& client, float opacity) {
    WallEnsure();
    if (g_wall.px.p.empty() || !dc.d2d()) return;
    RECT wr; GetWindowRect(w, &wr);
    MONITORINFO mi{sizeof(mi)}; if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const float mw = static_cast<float>(mi.rcMonitor.right - mi.rcMonitor.left), mh = static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top), kx = g_wall.px.w / mw, ky = g_wall.px.h / mh;
    DrawPixelsPart(dc, &g_wall, g_wall.px.p.data(), g_wall.px.w, g_wall.px.h, D2DRect(client),
                   D2D1::RectF((wr.left - mi.rcMonitor.left) * kx, (wr.top - mi.rcMonitor.top) * ky, (wr.right - mi.rcMonitor.left) * kx, (wr.bottom - mi.rcMonitor.top) * ky), opacity);
}
// 2.9.8 Clear glass: the window itself is plain transparency; every glass card shows a lightly blurred piece of the wallpaper behind it (what that spot of the desktop shows).
void WallDrawRect(Dc dc, HWND w, const RECT& card, float opacity) {
    WallEnsure();
    if (g_wall.px.p.empty() || !dc.d2d()) return;
    RECT wr; GetWindowRect(w, &wr);
    MONITORINFO mi{sizeof(mi)}; if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const float mw = static_cast<float>(mi.rcMonitor.right - mi.rcMonitor.left), mh = static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top), kx = g_wall.px.w / mw, ky = g_wall.px.h / mh;
    DrawPixelsPart(dc, &g_wall, g_wall.px.p.data(), g_wall.px.w, g_wall.px.h, D2DRect(card),
                   D2D1::RectF((wr.left + card.left - mi.rcMonitor.left) * kx, (wr.top + card.top - mi.rcMonitor.top) * ky, (wr.left + card.right - mi.rcMonitor.left) * kx, (wr.top + card.bottom - mi.rcMonitor.top) * ky), opacity);
}
void ClearCardBackdrop(Dc dc, const RECT& r, int rad) {
    if (g_material != MatClear || !dc.d2d() || dc.d != &g_surf) return;   // the main window only (the Sidebar and the Mini Monitor have their own glass)
    const int clip = PushRoundClip(dc, r, rad);
    WallDrawRect(dc, g_wnd, r, 1.f);
    {   // 2.10: the person's own colour over the blurred picture (0 % = the plain glass)
        const int amt = Cfg().Int(L"appearance.cardTint", 0, 0, 100);
        if (amt > 0) {
            const int cv = Cfg().Int(L"appearance.cardColor", -1, -1, 0xFFFFFF);
            const COLORREF cc = cv < 0 ? col::panel : RGB((cv >> 16) & 0xFF, (cv >> 8) & 0xFF, cv & 0xFF);
            dc.d->dc->FillRectangle(D2DRect(r), dc.d->Brush(cc, amt / 100.f));
        }
    }
    PopClip(dc, clip);
}
const bool g_cardHookSet = [] { g_cardBackdropHook = ClearCardBackdrop; return true; }();
bool g_probeBlank = false, g_stGallery = false;   // self-test only
void ApplyMaterial(HWND h) {
    const Material m = g_surfOn ? ResolveMaterial(ReadMaterialInput()) : MatSolid;
    static bool applied = false;   // setting the backdrop makes Windows send theme messages: re-apply only on a real change
    if (applied && m == g_material) return;
    applied = true;
    if (g_surfOn) {
        MARGINS all{-1, -1, -1, -1}; DwmExtendFrameIntoClientArea(h, &all);
        const COLORREF noCaption = 0xFFFFFFFE; DwmSetWindowAttribute(h, 35 /*DWMWA_CAPTION_COLOR = DWMWA_COLOR_NONE*/, &noCaption, sizeof(noCaption));
        const int type = BackdropType(m); DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type));
        if (m == MatClear || g_material == MatClear) {   // 2.10: with no backdrop the extended frame is drawn opaque black. An empty blur region turns the window into a plain see-through sheet (no blur), so the alpha of what we draw counts.
            DWM_BLURBEHIND bb{}; bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION; bb.fEnable = (m == MatClear) ? TRUE : FALSE; bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
            DwmEnableBlurBehindWindow(h, &bb); if (bb.hRgnBlur) DeleteObject(bb.hRgnBlur);
        }
    }
    if (m != g_material) LogI(std::wstring(L"Window material: ") + (m == MatAcrylic ? L"acrylic" : m == MatFrosted ? L"frosted" : m == MatWallpaper ? L"wallpaper glass" : m == MatClear ? L"clear glass" : L"solid"));
    const bool matChanged = m != g_material;
    g_material = m;
    if (matChanged) WallForget();   // the wallpaper copy is blurred differently for Clear glass
    InvalidateRect(h, nullptr, FALSE);
}
float TintAlpha() { if (g_material == MatClear) return ClearBgPct(Cfg().Int(L"appearance.clearBg", kClearBgDefault, 0, 100)) / 100.f; return g_material == MatSolid || ((g_material == MatWallpaper) && g_wall.px.p.empty()) ? 1.f : Cfg().Int(L"appearance.tint", kTintDefault, kTintMin, 92) / 100.f; }
float GlowK() { const int g = Cfg().Int(L"appearance.glow", 2, 0, 2); return g == 0 ? 0.f : g == 1 ? .55f : 1.f; }
void UpdateMotion() { int m = Cfg().Int(L"appearance.motion", MotionFull, MotionFull, MotionOff); BOOL anim = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0); if ((!anim || g_gov.ForcesReduced()) && m == MotionFull) m = MotionReduced; g_anim.level = m; }
bool AnubisActive() { static const bool on = [] { for (const auto& m : WindhawkMods()) if (m.id.find(L"anubis") != std::wstring::npos && m.enabled) return true; return false; }(); return on; }
std::function<void()> g_dragFn, g_dragEnd;   // the slider being dragged (immediate mode: the hit starts it)
bool g_dragging = false;
void Slider(Ctx& c, const wchar_t* label, int value, int lo, int hi, const wchar_t* unit, std::function<void(int)> set, std::function<void()> commit) {
    Text(c.dc, label, RECT{c.x + c.S(16), c.y, c.x + c.S(150), c.y + c.S(30)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
    const int x0 = c.x + c.S(158), x1 = c.x + c.w - c.S(90), my = c.y + c.S(15);
    const float frac = std::clamp((value - lo) / static_cast<float>(hi - lo), 0.f, 1.f);
    const int kx = x0 + static_cast<int>((x1 - x0) * frac), k = c.S(16);
    Round(c.dc, RECT{x0, my - c.S(2), x1, my + c.S(2)}, c.S(4), col::track, col::track);
    BarFill(c.dc, RECT{x0, my - c.S(2), kx, my + c.S(2)}, c.S(4), col::accentDim, col::accent);
    RECT hit{x0 - c.S(8), c.y, x1 + c.S(8), c.y + c.S(30)};
    const bool hot = c.Hover(hit);
    if (hot) Halo(c.dc, kx, my, k * 3 / 2, col::accent, .30f);
    Ellipse2(c.dc, RECT{kx - k / 2, my - k / 2, kx + k / 2, my + k / 2}, col::text, col::accent);
    const std::wstring vs = std::to_wstring(value) + unit;
    if (g_dragging && hot) { RECT bb{kx - c.S(24), c.y - c.S(24), kx + c.S(24), c.y - c.S(2)}; Round(c.dc, bb, c.S(12), col::overlay, col::accent); Text(c.dc, vs, bb, c.u.f.tiny, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    Text(c.dc, vs, RECT{x1 + c.S(14), c.y, c.x + c.w, c.y + c.S(30)}, c.u.f.small, col::text, DT_VCENTER | DT_SINGLELINE);
    c.Hit(hit, [x0, x1, lo, hi, set, commit] {
        auto apply = [=] { const int v = lo + static_cast<int>((std::clamp<int>(g_ui.mouse.x, x0, x1) - x0) * (hi - lo) / static_cast<float>(std::max(1, x1 - x0)) + .5f); set(v); InvalidateRect(g_wnd, nullptr, FALSE); };
        apply();
        if (GetKeyState(VK_LBUTTON) < 0) { g_dragFn = apply; g_dragEnd = commit; g_dragging = true; SetCapture(g_wnd); } else commit();
    });
    c.y += c.S(38);
}
void SbRender(); void MiniRender();
void AppearanceMaterial(Ctx& c) {
    Section(c, L"Theme", L"Windows Glass (default, the Windows 11 Explorer / Start look with your accent colour), Neutral Windows Glass (graphite and silver) or Classic AnkhCore (the original neon-blue identity). Saved and restored on startup.");
    {
        const std::wstring curTheme = ActiveTheme().id;
        const std::pair<ThemeSpec, std::wstring> themes[] = {
            {WindowsGlassTheme(), L"The Windows 11 look: Mica and Acrylic dark layers like Explorer and the Start menu, with your own Windows accent colour."},
            {NeutralGlassTheme(),  L"Dark graphite, translucent charcoal cards and subtle silver rims."},
            {ClassicAnkhTheme(),  L"Original AnkhCore deep-navy acrylic look with neon cyan-blue accents and glow."}
        };
        const int gap = c.S(12), cw = (c.w - 2 * gap) / 3, ch = c.S(124);
        for (int idx = 0; idx < 3; ++idx) {
            const ThemeSpec& ts = themes[idx].first;
            const bool sel = (curTheme == ts.id);
            const int cx = c.x + idx * (cw + gap);
            RECT card{cx, c.y, cx + cw, c.y + ch};
            const bool hov = c.Hover(card);
            Round(c.dc, card, c.S(20), sel ? col::accentDeep : hov ? col::panel2 : col::panel, sel ? col::accent : col::border);
            // Mini visual preview inside each theme card
            RECT pv{cx + c.S(12), c.y + c.S(12), cx + c.S(96), c.y + c.S(64)};
            Round(c.dc, pv, c.S(10), ts.bg, ts.border);
            RECT pvSide{pv.left + c.S(4), pv.top + c.S(4), pv.left + c.S(24), pv.bottom - c.S(4)};
            Round(c.dc, pvSide, c.S(6), ts.side, ts.side);
            RECT pvCard{pvSide.right + c.S(4), pv.top + c.S(6), pv.right - c.S(6), pv.bottom - c.S(14)};
            Round(c.dc, pvCard, c.S(6), ts.panel, ts.border);
            RECT pvBtn{pvSide.right + c.S(6), pv.bottom - c.S(11), pv.right - c.S(20), pv.bottom - c.S(5)};
            Round(c.dc, pvBtn, c.S(4), ts.accent, ts.accent);

            const int tx = pv.right + c.S(12);
            Text(c.dc, ts.name, RECT{tx, c.y + c.S(12), cx + cw - c.S(12), c.y + c.S(34)}, c.u.f.btn, col::text, DT_SINGLELINE | DT_END_ELLIPSIS);
            if (sel) Chip(c, tx, c.y + c.S(36), L"Active", col::ok);
            else if (idx == 0) Chip(c, tx, c.y + c.S(36), L"Default", col::accent);
            Text(c.dc, themes[idx].second, RECT{cx + c.S(12), pv.bottom + c.S(8), cx + cw - c.S(12), c.y + ch - c.S(8)}, c.u.f.tiny, col::sub, DT_WORDBREAK | DT_END_ELLIPSIS);
            const std::wstring tid = ts.id;
            c.Hit(card, [tid] {
                ApplyTheme(tid);
                Cfg().SetStr(L"appearance.theme", tid);
                Cfg().Save();
                if (g_sidebar) SbRender();
                if (g_mini) MiniRender();
                InvalidateRect(g_wnd, nullptr, FALSE);
            });
        }
        c.y += ch + c.S(14);
    }
    Section(c, L"Window material", L"Defaults: Acrylic on mains, Frosted on battery, 55% tint. Windows turns blur off when Transparency effects, High Contrast or Battery saver say so.");
    const int mat = Cfg().Int(L"appearance.material", MatAcrylic, MatAcrylic, MatClear);
    const bool bat = Cfg().Bool(L"appearance.frostedOnBattery", true);
    auto setI = [](const wchar_t* k, int v) { Cfg().SetInt(k, v); Cfg().Save(); ApplyMaterial(g_wnd); InvalidateRect(g_wnd, nullptr, FALSE); };
    auto opts = [&](const wchar_t* key, std::initializer_list<const wchar_t*> names, int cur) { std::vector<ChipOpt> o; int i = 0; for (const wchar_t* n : names) { o.emplace_back(n, cur == i, [=] { setI(key, i); }); ++i; } return o; };
    Chips(c, L"Material", {{L"Acrylic", mat == MatAcrylic, [=] { setI(L"appearance.material", MatAcrylic); }}, {L"Frosted", mat == MatFrosted, [=] { setI(L"appearance.material", MatFrosted); }},
                           {L"Wallpaper glass", mat == MatWallpaper, [=] { setI(L"appearance.material", MatWallpaper); }}, {L"Clear glass", mat == MatClear, [=] { setI(L"appearance.material", MatClear); }}, {L"Solid", mat == MatSolid, [=] { setI(L"appearance.material", MatSolid); }}});
    if (g_material == MatClear) {   // 2.10: the two Clear glass controls
        Slider(c, L"Window opacity", ClearBgPct(Cfg().Int(L"appearance.clearBg", kClearBgDefault, 0, 100)), 0, 100, L"%", [](int v) { Cfg().SetInt(L"appearance.clearBg", ClearBgPct(v)); }, [] { Cfg().Save(); });
        {   // 2.10: card colour (strength + colour), the same swatches the Sidebar uses
            const int cv = Cfg().Int(L"appearance.cardColor", -1, -1, 0xFFFFFF);
            const int base = cv < 0 ? (GetRValue(col::panel) << 16) | (GetGValue(col::panel) << 8) | GetBValue(col::panel) : cv;
            Slider(c, L"Card colour", Cfg().Int(L"appearance.cardTint", 0, 0, 100), 0, 100, L"%", [](int v) { Cfg().SetInt(L"appearance.cardTint", std::clamp(v, 0, 100)); }, [] { Cfg().Save(); });
            struct Sw { const wchar_t* n; int rgb; };
            static const Sw sw[] = {{L"Blue", 0x00AEFF}, {L"Cyan", 0x22D3EE}, {L"Teal", 0x2DD4BF}, {L"Green", 0x2ECC71}, {L"Amber", 0xF5B041}, {L"Orange", 0xFF8A3D}, {L"Red", 0xFF5C5C}, {L"Pink", 0xFF6FB5}, {L"Violet", 0xA78BFA}, {L"White", 0xE8F4FF}, {L"Navy", 0x081633}, {L"Black", 0x05080F}, {L"Theme colour", -1}};
            auto setC = [](int v) { return [v] { Cfg().SetInt(L"appearance.cardColor", v); if (Cfg().Int(L"appearance.cardTint", 0, 0, 100) == 0) Cfg().SetInt(L"appearance.cardTint", 35); Cfg().Save(); }; };
            std::vector<ChipOpt> o; for (const Sw& w2 : sw) o.emplace_back(w2.n, cv == w2.rgb, setC(w2.rgb));
            Chips(c, L"   Colour", o);
            Chips(c, (L"   " + SbColorHex(cv)).c_str(), {{L"R −", false, setC(SbColorStep(base, 0, -16))}, {L"R +", false, setC(SbColorStep(base, 0, 16))}, {L"G −", false, setC(SbColorStep(base, 1, -16))},
                                                           {L"G +", false, setC(SbColorStep(base, 1, 16))}, {L"B −", false, setC(SbColorStep(base, 2, -16))}, {L"B +", false, setC(SbColorStep(base, 2, 16))}});
        }
        Slider(c, L"Window frost", ClearBgPct(Cfg().Int(L"appearance.clearFrost", kClearFrostDefault, 0, 100)), 0, 100, L"%", [](int v) { Cfg().SetInt(L"appearance.clearFrost", ClearBgPct(v)); }, [] { Cfg().Save(); });
        Slider(c, L"Blur strength", ClearBlurPx(Cfg().Int(L"appearance.clearBlur", kClearBlurDefault, 0, kClearBlurMax)), 0, kClearBlurMax, L" px", [](int v) { Cfg().SetInt(L"appearance.clearBlur", ClearBlurPx(v)); WallForget(); }, [] { Cfg().Save(); });
    }
    Slider(c, L"Tint strength", Cfg().Int(L"appearance.tint", kTintDefault, kTintMin, 92), kTintMin, 92, L"%", [](int v) { Cfg().SetInt(L"appearance.tint", v); }, [] { Cfg().Save(); });
    // the window's brand picture: its own opacity and blur (they change the picture only, never the text or the glass)
    Slider(c, L"Background opacity", WinBgOpacityCfg(), 0, 100, L"%", [](int v) { Cfg().SetInt(L"appearance.bgOpacity", std::clamp(v, 0, 100)); }, [] { Cfg().Save(); });
    Slider(c, L"Background blur", WinBgBlurCfg(), 0, kCandyBlurMax, L" px", [](int v) { Cfg().SetInt(L"appearance.bgBlur", std::clamp(v, 0, kCandyBlurMax)); }, [] { Cfg().Save(); });
    Chips(c, L"Background", {{L"Reset background", WinBgOpacityCfg() != kWinBgOpacityDefault || WinBgBlurCfg() != kWinBgBlurDefault, [] { WinBgReset(); }}});
    Chips(c, L"On battery", {{L"Frosted (saves power)", bat, [] { Cfg().SetBool(L"appearance.frostedOnBattery", true); Cfg().Save(); ApplyMaterial(g_wnd); }},
                             {L"Keep my choice", !bat, [] { Cfg().SetBool(L"appearance.frostedOnBattery", false); Cfg().Save(); ApplyMaterial(g_wnd); }}});
    {   // honest label: Windows fixes the acrylic blur radius
        std::vector<ChipOpt> o; int i = 0; const int cur = Cfg().Int(L"appearance.blur", 1, 0, 2);
        for (const wchar_t* n : {L"Low", L"Medium", L"High"}) { o.emplace_back(n, cur == i, [i] { Cfg().SetInt(L"appearance.blur", i); Cfg().Save(); WallForget(); }); ++i; }
        Chips(c, L"Wallpaper blur", o);
    }
    {   // 2.11: where the strength of Windows' own Acrylic blur is set (the Windhawk mod "AnkhCore Global Acrylic Tuner": it changes Start, taskbar, Explorer and every acrylic window)
        bool present = false; for (const auto& m : Snapshot().mods) if (m.id.find(L"ankhcore-acrylic-tuner") != std::wstring::npos) present = true;
        Chips(c, L"Windows blur", {{present ? L"Set the Windows blur (Windhawk mod)" : L"Windows blur mod not installed", false, [present] { if (!present) return; NavGo(PWindhawk); WhLoadAsync("local@ankhcore-acrylic-tuner"); }}});
    }
    Chips(c, L"Glow", opts(L"appearance.glow", {L"Off", L"Subtle", L"Normal"}, Cfg().Int(L"appearance.glow", 2, 0, 2)));
    Chips(c, L"Motion", opts(L"appearance.motion", {L"Full", L"Reduced", L"Off"}, Cfg().Int(L"appearance.motion", MotionFull, MotionFull, MotionOff)));
    const bool wg = Cfg().Bool(L"appearance.widgetGlass", false);
    Chips(c, L"Widgets", {{L"Plain glass", !wg, [] { Cfg().SetBool(L"appearance.widgetGlass", false); Cfg().Save(); }}, {L"Wallpaper glass", wg, [] { Cfg().SetBool(L"appearance.widgetGlass", true); Cfg().Save(); }}});
    Para(c, std::wstring(L"Now showing: ") + (g_material == MatAcrylic ? L"Acrylic" : g_material == MatFrosted ? L"Frosted" : g_material == MatWallpaper ? L"Wallpaper glass" : g_material == MatClear ? L"Clear glass" : L"Solid navy") + L". Windows sets the acrylic blur strength; the tint controls how much shows through.", c.u.f.small, col::sub);
}
void PageAppMaterial(Ctx& c, const Cache&) {
    AppearanceMaterial(c);
    Section(c, L"Reset");
    Chips(c, L"Defaults", {{L"Restore default appearance", false, [] {
        Cfg().SetInt(L"appearance.material", MatAcrylic);
        Cfg().SetInt(L"appearance.tint", kTintDefault);
        Cfg().SetBool(L"appearance.frostedOnBattery", true);
        Cfg().SetInt(L"appearance.blur", 1);
        Cfg().SetInt(L"appearance.glow", 2);
        Cfg().SetInt(L"appearance.motion", MotionFull);
        Cfg().SetBool(L"appearance.widgetGlass", false);
        Cfg().SetInt(L"appearance.bgOpacity", kWinBgOpacityDefault);
        Cfg().SetInt(L"appearance.bgBlur", kWinBgBlurDefault);
        Cfg().Save();
        WallForget();
        UpdateMotion();
        ApplyMaterial(g_wnd);
        InvalidateRect(g_wnd, nullptr, FALSE);
    }}});
}
void SbRecoverAfterThemeChange();
void LoadThumbsAsync();
std::atomic<bool> g_thumbsLoading{false};

void PageAppTheme(Ctx& c, const Cache& s) {
    const bool w11 = s.ver.win11();
    Section(c, L"Primary AnkhCore Theme — As3aD",
            w11 ? L"Authoritative application-local As3aD.theme (Themetools\\As3aD.theme), applied silently via SecureUxTheme."
                : L"The Windows 10 variant of the As3aD theme (Themetools\\As3aD10.theme, After Dark Glass Cyan), applied silently via SecureUxTheme.");
    {
        std::wstring localPath, localDet;
        const bool localVerified = VerifyAppLocalAs3aDTheme(localPath, localDet);
        const SuxFullStatus suxSt = s.themeChecked ? s.themeCheck.suxStatus : QuerySuxFullStatus(SuxDir());
        RowSpec r; r.icon = L"\xE790"; r.title = s.themeActive ? L"As3aD is your current theme" : L"As3aD (Primary AnkhCore Theme)";
        r.desc = L"Source: " + localPath + L"\n" + suxSt.summary;
        r.enabled = true;
        r.chips.push_back({s.themeActive ? L"Active" : L"Not applied", s.themeActive ? col::ok : col::faint});
        r.chips.push_back({localVerified ? L"Themetools\\ SHA-256 OK" : L"Themetools\\ unverified", localVerified ? col::accent : col::warn});
        r.chips.push_back({suxSt.loadedInSession ? L"Patch Loaded" : (suxSt.installed ? L"Pending Restart" : L"Patch Missing"),
                           suxSt.loadedInSession ? col::ok : col::warn});
        r.buttons = {
            {L"Apply As3aD", Btn::Primary, true, [] {
                if (!kTestBuild && VerifySuxDir(SuxDir()).ok && !PatchLoaded()) {
                    ScheduleThemeReapply(HKEY_CURRENT_USER, 1);
                    LogW(L"SecureUxTheme hook not active in this session yet; pending silent apply scheduled for next sign-in.");
                    if (Confirm(L"SecureUxTheme is installed and verified, but the hook is not active in this Windows session yet.\n\nA silent Apply has been scheduled for your next sign-in. Restart Windows now to activate?", L"Restart Required", false)) {
                        Run(SysDir() + L"\\shutdown.exe", L"/r /t 3 /c \"Restarting Windows to activate SecureUxTheme.\"", true, 5000);
                    }
                    return;
                }
                RunOp(L"Apply the As3aD theme", [] {
                    std::wstring m; const bool ok = ApplyTheme(true, m); (ok ? LogOk : LogE)(m);
                    if (g_wnd) PostMessageW(g_wnd, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
                    return ok;
                });
            }},
            {L"Windows theme", Btn::Secondary, true, [] {
                RunOp(L"Restore the Windows theme", [] {
                    std::wstring m; const bool ok = RestoreWindowsTheme(m); LogI(m);
                    if (g_wnd) PostMessageW(g_wnd, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
                    return ok;
                });
            }},
            {L"Check & repair", Btn::Secondary, true, [] { RunOp(L"Check & repair the theme", [] {
                std::wstring m; const bool ok = RepairTheme(m); const ThemeCheck ch = CheckTheme();
                for (const auto& [n, good] : ch.items) Log(good ? Lv::Ok : Lv::Warn, n + (good ? L": OK" : L": missing / not active"));
                if (!ok && !m.empty()) LogE(m);
                EnterCriticalSection(&g_cs); g_cache.themeCheck = ch; g_cache.themeChecked = true; LeaveCriticalSection(&g_cs);
                return ok; }); }},
            {L"ThemeTool", Btn::Ghost, w11, [] { if (TestGuard(L"Open ThemeTool")) return; ShellExecuteW(nullptr, L"open", (SuxDir() + L"\\ThemeTool.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}};
        Row(c, r);
        if (s.themeChecked) {
            std::wstring t; for (const auto& [n, good] : s.themeCheck.items) t += (good ? L"\x2714 " : L"\x2716 ") + n + L"     ";
            Para(c, t, c.u.f.small, s.themeCheck.allOk ? col::ok : col::warn);
            if (!s.themeCheck.patchDetail.empty()) Para(c, s.themeCheck.patchDetail, c.u.f.small, s.themeCheck.allOk ? col::sub : col::warn);
        }
    }

    // Separate presentation for the authoritative Windows Theme Collection (`D:\Ankhcore\windows\Resources\Themes`)
    static int s_winThemeFilter = 0;   // 0 All, 1 BIB Dark, 2 BIB Light, 3 BIB3 Dark, 4 BIB3 Light
    static bool s_winThemeCompatOnly = false;   // 2.8.3: only the themes that fit this PC's Windows
    std::vector<DiscoveredWinTheme> collection = DiscoverAuthoritativeThemes(ThemeCollectionRoot());   // 2.8.4: Windows 10 reads Windows' themes folder (the After Dark pack Setup installed)
    if (!w11) collection.erase(std::remove_if(collection.begin(), collection.end(), [](const DiscoveredWinTheme& t) { return t.os != ThemeOs::Windows10; }), collection.end());
    Section(c, L"Windows Theme Collection (" + std::to_wstring(collection.size()) + L" themes)",
            L"Discovered dynamically from " + ThemeCollectionRoot() + (w11 ? L". Separated from the primary As3aD theme." : L". On Windows 10 these are the After Dark themes Setup installed for your release."));
    if (collection.empty()) {
        Para(c, w11 ? L"No additional Windows themes were found in " + ThemeCollectionRoot() + L"." : L"No After Dark theme is installed for this Windows 10. Run AnkhCore Setup again and keep \"After Dark theme\" ticked (it picks the pack for your Windows release).", c.u.f.small, col::sub);
    } else {
        int cntBibDark = 0, cntBibLight = 0, cntBib3Dark = 0, cntBib3Light = 0;
        for (const auto& th : collection) {
            if (_wcsicmp(th.family.c_str(), L"BIB") == 0) { if (th.darkMode) ++cntBibDark; else ++cntBibLight; }
            else if (_wcsicmp(th.family.c_str(), L"BIB3") == 0) { if (th.darkMode) ++cntBib3Dark; else ++cntBib3Light; }
        }
        if (w11) Chips(c, L"Collection", {
            {L"All (" + std::to_wstring(collection.size()) + L")", s_winThemeFilter == 0, [] { s_winThemeFilter = 0; InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"BIB Night (" + std::to_wstring(cntBibDark) + L")", s_winThemeFilter == 1, [] { s_winThemeFilter = 1; InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"BIB Light (" + std::to_wstring(cntBibLight) + L")", s_winThemeFilter == 2, [] { s_winThemeFilter = 2; InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"BIB3 Night (" + std::to_wstring(cntBib3Dark) + L")", s_winThemeFilter == 3, [] { s_winThemeFilter = 3; InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"BIB3 Light (" + std::to_wstring(cntBib3Light) + L")", s_winThemeFilter == 4, [] { s_winThemeFilter = 4; InvalidateRect(g_wnd, nullptr, FALSE); }}
        });
        Chips(c, L"Windows", {
            {L"All themes", !s_winThemeCompatOnly, [] { s_winThemeCompatOnly = false; InvalidateRect(g_wnd, nullptr, FALSE); }},
            {s.ver.win11() ? L"For Windows 11 (this PC)" : L"For Windows 10 (this PC)", s_winThemeCompatOnly, [] { s_winThemeCompatOnly = true; InvalidateRect(g_wnd, nullptr, FALSE); }}});
        const std::wstring activeVs = Lower(NameOf(GetActiveVisualStylePath()));
        const std::wstring curThemeReg = Lower(NameOf(RegStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes", L"CurrentTheme")));
        for (const auto& th : collection) {
            if (s_winThemeFilter == 1 && !(_wcsicmp(th.family.c_str(), L"BIB") == 0 && th.darkMode)) continue;
            if (s_winThemeFilter == 2 && !(_wcsicmp(th.family.c_str(), L"BIB") == 0 && !th.darkMode)) continue;
            if (s_winThemeFilter == 3 && !(_wcsicmp(th.family.c_str(), L"BIB3") == 0 && th.darkMode)) continue;
            if (s_winThemeFilter == 4 && !(_wcsicmp(th.family.c_str(), L"BIB3") == 0 && !th.darkMode)) continue;

            const ThemeApplyCheck osChk = ThemeApplyCheckFor(th.os, s.ver.build, kThemeApplierWin10);
            if (s_winThemeCompatOnly && !ThemeFitsHostOs(th.os, s.ver.build)) continue;
            const bool isCurrent = !s.themeActive && (!activeVs.empty() && activeVs == Lower(NameOf(th.vsRelPath))) &&
                                   (curThemeReg == Lower(NameOf(th.themeFilePath)));
            RowSpec tr; tr.icon = L"\xE790"; tr.title = th.displayName;
            tr.desc = L"Visual style: " + th.vsRelPath + (th.wallpaperValid ? (L"  \x00B7  Wallpaper: " + NameOf(th.wallpaperPath)) : L"") + L"  \x00B7  For: " + ThemeOsLabel(th.os) + L" (" + th.osSource + L")" + (osChk.reason.empty() ? std::wstring() : L"\n" + osChk.reason);
            tr.chips.push_back({th.family, col::accent});
            tr.chips.push_back({th.darkMode ? L"Dark" : L"Light", th.darkMode ? col::sub : col::faint});
            tr.chips.push_back({ThemeOsLabel(th.os), th.os == ThemeOs::Unknown ? col::warn : osChk.allowed || ThemeFitsHostOs(th.os, s.ver.build) ? col::accent : col::err});   // 2.8.3: which Windows it is for
            if (!osChk.allowed) tr.chips.push_back({ThemeFitsHostOs(th.os, s.ver.build) ? L"Not available here" : L"Not for this Windows", col::err});
            else if (osChk.uncertain) tr.chips.push_back({L"Compatibility unknown", col::warn});
            if (isCurrent) tr.chips.push_back({L"Active", col::ok});
            else if (th.valid) tr.chips.push_back({L"Verified", col::ok});
            const DiscoveredWinTheme captured = th;
            tr.buttons = {{L"Apply", isCurrent ? Btn::Secondary : Btn::Primary, osChk.allowed && th.valid && !g_busy, [captured] {
                if (TestGuard(L"Apply discovered Windows theme " + captured.displayName)) return;
                RunOp(L"Apply theme " + captured.displayName, [captured] {
                    std::wstring m;
                    const bool ok = ApplyDiscoveredWindowsTheme(captured, m);
                    (ok ? LogOk : LogE)(m);
                    if (g_wnd) PostMessageW(g_wnd, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
                    return ok;
                });
            }}};
            Row(c, tr);
        }
    }
}
void PageAppWallpaper(Ctx& c, const Cache& s) {
    if (g_thumbPaths.empty() && !g_thumbsLoading && !WallImages().empty()) LoadThumbsAsync();
    Section(c, L"Anubis wallpapers", L"Changes the desktop picture automatically. Pick a picture below to keep it fixed.");
    {
        RowSpec r; r.icon = L"\xE91B"; r.title = L"Anubis wallpapers"; r.desc = L"Slideshow from C:\\Windows\\Web\\Wallpaper\\Anubis.";
        r.toggle = s.wallOn;
        r.onToggle = [on = s.wallOn] {
            if (g_busy) return;
            RunOp(on ? L"Anubis wallpapers off" : L"Anubis wallpapers on", [on] { return on ? WallpaperOff() : SetWallSlideshow(); });
        };
        Row(c, r);

        const DWORD cur = WallInterval();
        const std::pair<int, const wchar_t*> iv[] = {{10, L"10 min"}, {30, L"30 min"}, {60, L"1 hour"}, {360, L"6 hours"}, {1440, L"1 day"}};
        std::vector<ChipOpt> ivChips;
        for (const auto& [m, label] : iv) {
            ivChips.emplace_back(label, cur == static_cast<DWORD>(m), [m] {
                Cfg().SetInt(L"wallpaper.intervalMin", m); Cfg().Save();
                if (!g_busy && Snapshot().wallOn) RunOp(L"Update wallpaper timing", [] { return SetWallSlideshow(); });
                InvalidateRect(g_wnd, nullptr, FALSE);
            });
        }
        Chips(c, L"Change every", ivChips);

        const bool shuf = WallShuffle();
        auto setShuf = [](bool v) {
            return [v] {
                Cfg().SetBool(L"wallpaper.shuffle", v); Cfg().Save();
                if (!g_busy && Snapshot().wallOn) RunOp(L"Update wallpaper order", [] { return SetWallSlideshow(); });
                InvalidateRect(g_wnd, nullptr, FALSE);
            };
        };
        Chips(c, L"Order", {{L"Shuffle", shuf, setShuf(true)}, {L"In order", !shuf, setShuf(false)}});
        c.y += c.S(8);

        // thumbnails
        const int cols = 4, gap = c.S(12), tw = (c.w - gap * (cols - 1)) / cols, th = tw * 9 / 16;
        const int cardR = c.S(16);
        for (size_t i = 0; i < g_thumbPaths.size(); ++i) {
            const int cx = c.x + static_cast<int>(i % cols) * (tw + gap), cy = c.y + static_cast<int>(i / cols) * (th + gap);
            RECT r{cx, cy, cx + tw, cy + th};
            if (!c.Visible(r)) continue;
            const bool hov = c.Hover(r);
            Round(c.dc, r, cardR, col::panel, hov ? col::accent : col::border);
            const int thumbClip = PushRoundClip(c.dc, RECT{r.left + 1, r.top + 1, r.right - 1, r.bottom - 1}, cardR);
            if (i < g_thumbs.size() && g_thumbs[i].bmp) {
                DrawImage(c.dc, g_thumbs[i], cx + (tw - g_thumbs[i].w) / 2, cy + (th - g_thumbs[i].h) / 2);
            } else {
                Text(c.dc, L"\xE91B", r, c.u.f.iconBig, col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            if (hov) {
                RECT hb{r.left, r.bottom - c.S(26), r.right, r.bottom};
                FillRect2(c.dc, hb, col::accentDeep);
                Text(c.dc, L"Use this picture", hb, c.u.f.tiny, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            PopClip(c.dc, thumbClip);
            const std::wstring p = g_thumbPaths[i];
            c.Hit(r, [p] { if (!g_busy) RunOp(L"Set wallpaper " + NameOf(p), [p] { return SetWallStatic(p); }); });
        }
        if (g_thumbPaths.empty()) Para(c, L"The Anubis wallpapers are not on this PC. Use Check & repair on Windows Theme Manager.", c.u.f.small, col::warn);
        c.y += static_cast<int>((g_thumbPaths.size() + cols - 1) / cols) * (th + gap) + c.S(6);
    }
    Section(c, L"Reset");
    Chips(c, L"Defaults", {{L"Restore default wallpaper timing", false, [] {
        Cfg().SetInt(L"wallpaper.intervalMin", 30);
        Cfg().SetBool(L"wallpaper.shuffle", true);
        Cfg().Save();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }}});
}
// ---- Cursor Manager (APP-4): state for the page. The engine is eng_cursors.h; nothing here writes the registry.
void AddToast(const std::wstring& text, COLORREF color, uint64_t key = 0);
struct CurUi {
    bool scanned = false; std::vector<CurScheme> schemes; std::vector<CurState> targets;   // targets: what each scheme would write, paths expanded
    CurState now; int active = -1; int sel = -1; DWORD readAt = 0; bool hasPrev = false; std::wstring prevName;
    std::array<bool, kCurRoles> nowOk{};   // the active role's file exists and is a cursor (checked with the state, not at every paint)
} g_cur;
std::map<std::wstring, HCURSOR> g_curPreview;   // loaded cursor files (first frame is drawn); freed on rescan
// Where packs are looked for: Windows' own Cursors folder, the folder the user chose, AnkhCore's content folder.
std::vector<std::wstring> CurRoots() {
    // 2.11: the packs the setup installed (cursor-packs.txt) are the app's own pointer list, each from its folder in %SystemRoot%\Cursors; without the file the old roots apply
    std::vector<std::wstring> r; std::vector<uint8_t> raw;
    if (!kTestBuild && ReadAll(DataDir() + L"\\cursor-packs.txt", raw) && !raw.empty()) {
        std::string line; auto flush = [&] { if (!line.empty()) { const std::wstring n = Utf8ToW(line); if (DirExists(WinDir() + L"\\Cursors\\" + n)) r.push_back(WinDir() + L"\\Cursors\\" + n); line.clear(); } };
        for (const uint8_t ch : raw) { if (ch == '\n' || ch == '\r') flush(); else line.push_back(static_cast<char>(ch)); }
        flush();
    }
    if (r.empty()) r.push_back(WinDir() + L"\\Cursors");
    r.push_back(Cfg().Str(L"cursors.folder", L"")); r.push_back(AcbDefaultRoots().content + L"\\Cursors");
    return r;
}
CurState CurExpanded(CurState s) { for (auto& v : s.value) v = Expand(v); return s; }
void CurRefresh(bool rescan) {
    if (rescan || !g_cur.scanned) {
        for (auto& p : g_curPreview) if (p.second) DestroyCursor(p.second);
        g_curPreview.clear();
        g_cur.schemes.clear(); g_cur.targets.clear();
        for (auto& sc : CurDiscover(CurRoots())) {
            CurState t = CurExpanded(CurTargetState(sc, WinDir()));
            // a scheme saved in Windows that is the same 17 files as a pack already listed (e.g. one applied from here) is shown once
            if (sc.registered && CurFindActive(g_cur.targets, t) >= 0) continue;
            g_cur.schemes.push_back(std::move(sc)); g_cur.targets.push_back(std::move(t));
        }
        g_cur.scanned = true; g_cur.readAt = 0;
        if (g_cur.sel >= static_cast<int>(g_cur.schemes.size())) g_cur.sel = -1;
    }
    if (!g_cur.readAt || GetTickCount() - g_cur.readAt > 1000) {   // the registry state: at most once a second while the page is shown
        g_cur.now = CurReadState();
        const CurState ex = CurExpanded(g_cur.now);
        g_cur.active = CurFindActive(g_cur.targets, ex);
        for (int i = 0; i < kCurRoles; ++i) g_cur.nowOk[i] = ex.exists[i] && !ex.value[i].empty() && CurFileHeaderOk(ex.value[i]);
        CurState prev; g_cur.hasPrev = CurLoadPrevious(prev); g_cur.prevName = prev.name;
        g_cur.readAt = GetTickCount() | 1;
    }
}
HCURSOR CurPreview(const std::wstring& path) {
    if (path.empty()) return nullptr;
    auto it = g_curPreview.find(path);
    if (it != g_curPreview.end()) return it->second;
    HCURSOR h = CurFileHeaderOk(path) ? static_cast<HCURSOR>(LoadImageW(nullptr, path.c_str(), IMAGE_CURSOR, 0, 0, LR_LOADFROMFILE)) : nullptr;
    g_curPreview[path] = h;
    return h;
}
// One cell per role: the cursor's picture and the role's name. note[i] replaces the picture ("Windows", "missing").
void CurRoleGrid(Ctx& c, const std::array<std::wstring, kCurRoles>& path, const std::array<std::wstring, kCurRoles>& note) {
    const int cols = 6, gap = c.S(8), cw = (c.w - gap * (cols - 1)) / cols, ch = c.S(74);
    for (int i = 0; i < kCurRoles; ++i) {
        const int cx = c.x + (i % cols) * (cw + gap), cy = c.y + (i / cols) * (ch + gap);
        const RECT r{cx, cy, cx + cw, cy + ch};
        if (!c.Visible(r)) continue;
        Round(c.dc, r, c.S(20), col::panel, col::border);
        HCURSOR h = note[i].empty() ? CurPreview(path[i]) : nullptr;
        if (h) DrawIconAt(c.dc, reinterpret_cast<HICON>(h), cx + (cw - c.S(32)) / 2, cy + c.S(8), c.S(32));
        else Text(c.dc, note[i].empty() ? L"not readable" : note[i], RECT{cx + c.S(4), cy + c.S(8), cx + cw - c.S(4), cy + c.S(40)}, c.u.f.tiny, note[i].empty() ? col::err : col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(c.dc, CurRoleTable()[i].label, RECT{cx + c.S(6), cy + c.S(46), cx + cw - c.S(6), cy + ch - c.S(6)}, c.u.f.tiny, col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    c.y += ((kCurRoles + cols - 1) / cols) * (ch + gap) + c.S(4);
}
// Runs a cursor operation. Production: through RunOp (worker thread, logged, one at a time). Test build: at once,
// because the engine then works only on its sandbox copy of the cursor settings (RunOp refuses everything there).
void CurRun(const std::wstring& name, std::function<bool(std::wstring&)> fn) {
    const int mode = Cfg().Int(L"backup.autoBeforeChanges", 0, 0, 2);   // Recovery > Backup & Migration: always / ask / never
    const bool point = mode == 0 || (mode == 1 && Confirm(L"Create a recovery point before changing the mouse cursors?", L"Create recovery point", false));
    auto body = [name, fn, point](std::wstring& err) {
        std::wstring recDir;
        if (point) {
            if (AcbCreatePreChangeRecoveryPoint(AcbDefaultRoots(), name, recDir)) {   // the cursor settings as they are now go into the recovery point
                EnsureDir(recDir + L"\\files\\cursors");
                const std::string u = WToUtf8(CurEncodeState(CurReadState()));
                WriteAtomic(recDir + L"\\files\\cursors\\scheme-before.txt", reinterpret_cast<const uint8_t*>(u.data()), u.size());
            } else LogW(L"The recovery point could not be created; the previous cursor scheme is still saved separately.");
        }
        const bool ok = fn(err);
        if (ok && !recDir.empty()) {
            if (!AcbSealRecoveryPoint(AcbDefaultRoots(), recDir))
                LogW(L"The pre-change recovery point could not be sealed; it will remain treated as an unsealed point.");
        }
        return ok;
    };
    if (kTestBuild) {
        std::wstring err; const bool ok = body(err);
        g_cur.readAt = 0;
        static uint64_t sandboxKey = 0x8000000000000000ull;   // test build only: each sandbox run is its own operation
        AddToast(ok ? name + L": done in the test sandbox" : err.empty() ? name + L" failed" : err, ok ? col::ok : col::err, ++sandboxKey);
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    RunOp(name, [body] { std::wstring err; const bool ok = body(err); g_cur.readAt = 0; return ok; });
}
void CurChooseFolder() {
    if (g_selftest) return;   // a system dialog: never opened by the scripted walk
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opt = 0; dlg->GetOptions(&opt); dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Folder that holds your cursor packs");
    if (SUCCEEDED(dlg->Show(g_wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { Cfg().SetStr(L"cursors.folder", p); Cfg().Save(); CoTaskMemFree(p); CurRefresh(true); }
            it->Release();
        }
    }
    dlg->Release();
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void PageAppCursor(Ctx& c, const Cache& s) {
    CurRefresh(false);
    const auto& rt = CurRoleTable();
    Section(c, L"Active cursor scheme", kTestBuild ? L"Test build: this page works on a sandbox copy of the cursor settings. The cursors of this PC are not changed."
                                                   : L"What Windows uses now, read from the cursor settings.");
    {
        const CurState now = CurExpanded(g_cur.now);
        int set = 0, broken = 0;
        std::array<std::wstring, kCurRoles> path, note;
        for (int i = 0; i < kCurRoles; ++i) {
            path[i] = now.exists[i] ? now.value[i] : std::wstring();
            if (path[i].empty()) note[i] = L"Windows";
            else { ++set; if (!g_cur.nowOk[i]) { ++broken; note[i] = L"file missing"; } }
        }
        RowSpec r; r.icon = L"\xE962";
        r.title = g_cur.active >= 0 ? g_cur.schemes[g_cur.active].name : !g_cur.now.name.empty() ? g_cur.now.name : L"Windows default";
        r.desc = std::to_wstring(set) + L" of 17 roles have their own cursor file" + (broken ? L"; " + std::to_wstring(broken) + L" point to a file that is missing or is not a cursor." : L".");
        if (g_cur.active >= 0) r.chips.push_back({L"Matches a scheme below", col::ok});
        else if (set) r.chips.push_back({L"Custom mix", col::accent});
        if (broken) r.chips.push_back({std::to_wstring(broken) + L" broken", col::err});
        Row(c, r);
        CurRoleGrid(c, path, note);
    }
    Section(c, L"Cursor schemes", L"Packs found in the Windows Cursors folder, in your own folder and among the schemes saved in Windows. Files are used where they are: "
                                  L"AnkhCore never copies, renames or deletes a cursor file.");
    if (g_cur.schemes.empty()) StatePanel(c, L"\xE962", L"No cursor pack found", L"Put each pack in its own folder, then choose the folder that holds them.", col::faint);
    for (size_t i = 0; i < g_cur.schemes.size(); ++i) {
        const CurScheme& sc = g_cur.schemes[i];
        const int idx = static_cast<int>(i), missing = kCurRoles - sc.Mapped();
        const bool active = g_cur.active == idx, sel = g_cur.sel == idx, usable = sc.Mapped() > 0 && sc.invalid.empty();
        RowSpec r; r.icon = L"\xE962"; r.iconColor = active ? col::ok : col::accent;
        r.title = sc.name;
        r.desc = std::to_wstring(sc.Mapped()) + L" of 17 roles" + (sc.alternates.empty() ? L"" : L"  \x00B7  " + std::to_wstring(sc.alternates.size()) + L" alternates") +
                 L"  \x00B7  " + (sc.registered ? std::wstring(L"saved in Windows (Mouse Properties)") : sc.dir);
        if (active) r.chips.push_back({L"Active", col::ok});
        for (const auto& t : sc.tags) r.chips.push_back({L"[" + t + L"]", col::accent});
        if (sc.premium) r.chips.push_back({L"Premium: your own copy", col::warn});
        if (sc.Mapped() == 0) r.chips.push_back({sc.alternates.empty() ? L"No cursor file for any role" : L"Variants only: nothing to apply", col::warn});   // a folder with only alternate files has no role file: Apply stays off, and the row says why
        else if (missing) r.chips.push_back({std::to_wstring(missing) + L" not provided", col::faint});
        if (!sc.unassigned.empty()) r.chips.push_back({std::to_wstring(sc.unassigned.size()) + L" not recognised", col::faint});
        if (!sc.invalid.empty()) r.chips.push_back({std::to_wstring(sc.invalid.size()) + L" invalid", col::err});
        r.buttons.push_back({sel ? L"Hide" : L"Preview", Btn::Secondary, true, [idx, sel] { g_cur.sel = sel ? -1 : idx; InvalidateRect(g_wnd, nullptr, FALSE); }});
        r.buttons.push_back({L"Apply", Btn::Primary, usable && !active, [idx] {
            if (idx >= static_cast<int>(g_cur.schemes.size())) return;
            const CurScheme pick = g_cur.schemes[idx];
            const int miss = kCurRoles - pick.Mapped();
            if (!Confirm(L"Apply the cursor scheme \"" + pick.name + L"\"?\n\nThe scheme that is active now is saved first, so Restore previous scheme brings it back." +
                         (miss ? L"\n" + std::to_wstring(miss) + L" role(s) this pack does not provide will use the Windows cursor." : L""), L"Apply scheme", false)) return;
            CurRun(L"Apply cursor scheme " + pick.name, [pick](std::wstring& err) { return CurApply(pick, err); }); }});
        Row(c, r);
        if (!sel) continue;
        std::array<std::wstring, kCurRoles> path, note;
        for (int k = 0; k < kCurRoles; ++k) { path[k] = CurRolePath(sc, k); if (path[k].empty()) note[k] = L"Windows"; }
        CurRoleGrid(c, path, note);
        std::wstring info;
        if (missing) { info += L"Not provided, the Windows cursor is used: "; bool first = true; for (int k : sc.Missing()) { info += (first ? L"" : L", ") + std::wstring(rt[k].label); first = false; } info += L".  "; }
        if (!sc.aliased.empty()) { info += L"Assigned by AnkhCore because the pack names no file for it: "; for (size_t k = 0; k < sc.aliased.size(); ++k) info += (k ? L", " : L"") + std::wstring(rt[sc.aliased[k]].label) + L" = " + NameOf(sc.file[sc.aliased[k]]); info += L" (confirm it looks right).  "; }
        if (!sc.unassigned.empty()) { info += L"Not recognised, not used: "; for (size_t k = 0; k < sc.unassigned.size(); ++k) info += (k ? L", " : L"") + sc.unassigned[k]; info += L".  "; }
        if (!sc.invalid.empty()) { info += L"Missing or not a cursor file: "; for (size_t k = 0; k < sc.invalid.size(); ++k) info += (k ? L", " : L"") + NameOf(sc.invalid[k]); info += L".  "; }
        if (!sc.alternates.empty()) info += std::to_wstring(sc.alternates.size()) + L" alternate file(s) (variants and sub-folders) are listed but not used.";
        if (sc.Mapped() == 0 && sc.invalid.empty()) info += L"  This folder has no file for any of the 17 cursor roles (its files are variants such as \"- Glow\"), so there is nothing to apply. Put the variant you want in a pack folder under the role's name, or use a complete pack.";
        if (!info.empty()) Para(c, info, c.u.f.small, sc.invalid.empty() ? col::sub : col::warn);
    }
    Buttons(c, {
        {g_cur.hasPrev && !g_cur.prevName.empty() ? L"Restore previous scheme (" + g_cur.prevName + L")" : std::wstring(L"Restore previous scheme"), Btn::Secondary, g_cur.hasPrev, [] {
            if (!Confirm(L"Put back the cursor scheme that was active before the last change?", L"Restore previous", false)) return;
            CurRun(L"Restore previous cursor scheme", [](std::wstring& err) { return CurRestorePrevious(err); }); }},
        {L"Restore Windows default", Btn::Secondary, true, [] {
            if (!Confirm(L"Use the Windows default mouse cursors?\n\nThe scheme that is active now is saved first.", L"Restore default", false)) return;
            CurRun(L"Restore Windows default cursors", [](std::wstring& err) { return CurRestoreDefaults(err); }); }},
        {L"Rescan", Btn::Ghost, true, [] { CurRefresh(true); InvalidateRect(g_wnd, nullptr, FALSE); }},
        {L"Choose folder\x2026", Btn::Ghost, true, [] { CurChooseFolder(); }}});
    {
        const std::wstring own = Cfg().Str(L"cursors.folder", L"");
        Para(c, L"Your folder: " + (own.empty() ? std::wstring(L"none chosen") : own + (DirExists(own) ? L"" : L" (not found)")) + L".  Also searched: " + CurRoots()[0] + L", " + CurRoots()[2] + L".",
             c.u.f.small, col::faint);
    }
    Section(c, L"AnkhCore 2.2 preset", L"The Prismatic switch from 2.2. Separate from the Windhawk Mouse Trail effect.");
    {
        RowSpec r; r.icon = L"\xE962"; r.title = L"Prismatic cursors (DIM v3.2)"; r.desc = L"Animated neon cursors. Works with any theme. Off restores your previous cursors.";
        r.chips.push_back({s.cursorsOn ? L"Active" : L"Windows default", s.cursorsOn ? col::ok : col::faint});
        r.toggle = s.cursorsOn; r.onToggle = [on = s.cursorsOn] { RunOp(on ? L"Prismatic cursors off" : L"Prismatic cursors on", [on] { return SetCursors(!on); }); };
        Row(c, r);
    }
}
// FT-A02: one Apply for the card buttons and the button under the cards: the same confirmation, the same operation.
void IconApplyTheme(const std::wstring& id) {
    const IconPack* p = FindPack(id); if (!p) return;
    const std::wstring nm = std::wstring(p->name) + (*p->variant ? L" " + std::wstring(p->variant) : L"");
    if (!Confirm(L"Apply the " + nm + L" icon theme via Windhawk Resource Redirect?\n\nResource Redirect redirects icon loads in memory using prebuilt resource-only PE modules. Protected Windows system files are not modified.\n\nWhen the theme is applied successfully, Explorer restarts by itself so the new icons load (open File Explorer windows close; Windows does not restart).", L"Apply icon theme", false)) return;
    g_iconApplyId = id; g_iconApplyResult = 0;
    if (!RunOp(L"Apply icon theme " + nm, [id] {
            auto prog = [](const std::wstring& t, int pc) { SetOpLine(t); g_progress = pc; };
            const bool ok = ApplyIconThemeRedirect(id, prog);
            g_iconApplyResult = ok ? 1 : 2;
            static std::atomic<bool> s_explorerRestarting{false};
            const IconAutoRestart ar = IconRestartAfterApply(ok, [] { return RestartExplorerShell(); }, s_explorerRestarting, [](const std::wstring& t) { SetOpLine(t); g_progress = kProgressUnknown; }, kIconRestartSettleSeconds);   // 2.8.3: only after a successful apply; 2.8.6: after a settle wait
            if (ok) { if (ar == IconAutoRestart::Done) LogOk(IconAutoRestartText(ar)); else LogW(IconAutoRestartText(ar)); }
            return ok; }))
        g_iconApplyId.clear();   // refused (busy, or the test build): nothing is running for any card
}
void PageAppIcons(Ctx& c, const Cache& s) {
    Section(c, L"Icon packs", L"Redirects Windows' own icons in memory via Windhawk Resource Redirect 1.3 (themePaths). Protected Windows system files stay untouched.");
    {
        // one card per group
        std::vector<const IconPack*> groups;
        for (const auto& p : IconPacks()) { bool seen = false; for (auto* g : groups) seen |= wcscmp(g->group, p.group) == 0; if (!seen) groups.push_back(&p); }
        const int cols = 3, gap = c.S(14), cw = (c.w - gap * (cols - 1)) / cols, ph = cw * 9 / 16, chh = ph + c.S(164);
        const IconPack* selPk = FindPack(g_selPack);
        for (const void* k : g_iconThumbs.DrainEvicted()) g_surf.Forget(k);   // device bitmaps of previews that left the memory cache
        for (size_t i = 0; i < groups.size(); ++i) {
            const IconPack* g = groups[i];
            const bool selGroup = selPk && wcscmp(selPk->group, g->group) == 0;
            const IconPack* shown = selGroup ? selPk : g;
            const int cx = c.x + static_cast<int>(i % cols) * (cw + gap), cy = c.y + static_cast<int>(i / cols) * (chh + gap);
            RECT r{cx, cy, cx + cw, cy + chh};
            if (!c.Visible(r)) continue;
            const bool hov = c.Hover(r);
            Round(c.dc, r, c.S(28), selGroup ? col::accentDeep : hov ? col::panel2 : col::panel, selGroup ? col::accent : hov ? col::accentDim : col::border);
            // FT-2 / A04: installed state and preview picture come from caches filled off the UI thread; nothing is read or decoded here
            const IconThemeStatus st = g_iconStatus.Get(shown->id);
            const int prW = cw - c.S(16), prH = ph - c.S(8);
            std::shared_ptr<const ThumbPixels> thumb = st.preview.empty() ? nullptr : g_iconThumbs.Get(st.preview, prW, prH, false);   // FT-A03: fitted, never cropped
            if (thumb && thumb->w > 0 && thumb->h > 0) {   // the picture keeps its own proportions, centred in the preview box
                const float k = std::min(1.0f, std::min(static_cast<float>(prW) / thumb->w, static_cast<float>(prH) / thumb->h));
                const float dw = thumb->w * k, dh = thumb->h * k, dx = cx + c.S(8) + (prW - dw) / 2, dy = cy + c.S(8) + (prH - dh) / 2;
                bool layered = false;
                if (shown->id == std::wstring(L"candy") && c.dc.d2d()) {   // Candy: base colour, then the background (opacity, blur), then the artwork on top
                    uint32_t base = 0; bool fresh = false;
                    const CandyLayer* mt = g_candyBg.Matte(thumb->id, thumb->px.data(), thumb->w, thumb->h, &base);
                    const CandyLayer* bgl = g_candyBg.Background(thumb->w, thumb->h, g_ui.S(CandyBlurCfg()), &fresh);
                    for (const void* k : g_candyBg.DrainEvicted()) g_surf.Forget(k);
                    if (mt && bgl) {
                        const D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + dw, dy + dh), src = D2D1::RectF(0, 0, static_cast<float>(thumb->w), static_cast<float>(thumb->h));
                        FillRect2(c.dc, RECT{static_cast<int>(dx), static_cast<int>(dy), static_cast<int>(std::ceil(dx + dw)), static_cast<int>(std::ceil(dy + dh))}, RGB((base >> 16) & 255, (base >> 8) & 255, base & 255));
                        DrawPixelsPart(c.dc, bgl, bgl->px.data(), bgl->w, bgl->h, dst, src, CandyOpacityCfg() / 100.0f);
                        DrawPixelsPart(c.dc, mt, mt->px.data(), mt->w, mt->h, dst, src, 1.0f);
                        layered = true;
                    }
                }
                if (!layered) DrawPixelsPart(c.dc, thumb->Key(), thumb->px.data(), thumb->w, thumb->h, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                               D2D1::RectF(0, 0, static_cast<float>(thumb->w), static_cast<float>(thumb->h)), 1.0f);
            } else Text(c.dc, !st.checked ? L"Checking\x2026" : !st.installed ? L"Theme not installed" : st.preview.empty() ? L"This pack has no preview picture" :
                              g_iconThumbs.Failed(st.preview, prW, prH, false) ? L"The preview picture could not be read" : L"Loading preview\x2026", RECT{cx + c.S(14), cy + c.S(8), cx + cw - c.S(14), cy + ph}, c.u.f.small, col::faint,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);   // third-party previews are not shipped (docs/ASSETS.md)
            int ty = cy + ph + c.S(8);
            Text(c.dc, g->name, RECT{cx + c.S(14), ty, cx + cw - c.S(104), ty + c.S(22)}, c.u.f.btn, col::text);
            const bool active = s.icons.active == shown->id;
            const bool installed = st.installed;
            if (active) Chip(c, cx + cw - c.S(96), ty + c.S(2), s.icons.pending ? L"Restart" : L"Active", s.icons.pending ? col::warn : col::ok);
            else if (st.checked && !installed) Chip(c, cx + cw - c.S(104), ty + c.S(2), L"Not installed", col::faint);
            else if (shown->partial) Chip(c, cx + cw - c.S(96), ty + c.S(2), L"Partial", col::accent);
            else if (st.checked && installed) Chip(c, cx + cw - c.S(96), ty + c.S(2), L"Installed", col::faint);
            ty += c.S(22);
            Text(c.dc, std::wstring(L"by ") + g->author, RECT{cx + c.S(14), ty, cx + cw - c.S(10), ty + c.S(18)}, c.u.f.tiny, col::faint);
            ty += c.S(18);
            Text(c.dc, shown->desc, RECT{cx + c.S(14), ty, cx + cw - c.S(12), ty + c.S(36)}, c.u.f.tiny, col::sub, DT_WORDBREAK | DT_END_ELLIPSIS);
            ty += c.S(38);
            const std::wstring id = selGroup ? std::wstring(selPk->id) : (wcscmp(g->group, L"bigsur") == 0 ? std::wstring(L"bigsur-dark") : std::wstring(g->id));
            // Ui::Click iterates hits in reverse (rbegin->rend): register the card body FIRST so variant chips and Apply win on overlap.
            c.Hit(r, [id] { if (FindPack(g_selPack) && wcscmp(FindPack(g_selPack)->group, FindPack(id)->group) == 0) return; g_selPack = id; InvalidateRect(g_wnd, nullptr, FALSE); });
            if (wcscmp(g->group, L"bigsur") == 0) {   // variant chips
                int vx = cx + c.S(14);
                for (const auto& p : IconPacks()) {
                    if (wcscmp(p.group, L"bigsur") != 0) continue;
                    const int bw = TextWidth(c.dc, p.variant, c.u.f.tiny) + c.S(20);
                    RECT vr{vx, ty, vx + bw, ty + c.S(22)};
                    const bool vs = g_selPack == p.id;
                    Round(c.dc, vr, c.S(11), vs ? col::accentDeep : col::panel2, vs ? col::accent : col::border);
                    Text(c.dc, p.variant, vr, c.u.f.tiny, vs ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    const std::wstring vid = p.id;
                    c.Hit(vr, [vid] { g_selPack = vid; InvalidateRect(g_wnd, nullptr, FALSE); });
                    vx += bw + c.S(6);
                }
            }
            {   // FT-A02: this card's own Apply, its progress and its result. Clicking the card itself only selects it.
                const IconThemeStatus ast = g_iconStatus.Get(id);
                IconCardInput ci; ci.checked = ast.checked; ci.installed = ast.installed; ci.windhawk = s.whInstalled;
                ci.active = s.icons.active == id; ci.restartPending = s.icons.pending; ci.busy = g_busy; ci.applyingHere = g_busy && g_iconApplyId == id;
                ci.lastResult = g_iconApplyId == id ? g_iconApplyResult.load() : 0;
                const IconCardView cv = IconCardState(ci);
                const int by = cy + chh - c.S(40), bw = c.S(92);
                RECT br{cx + cw - c.S(14) - bw, by, cx + cw - c.S(14), by + c.S(30)};
                DrawButton(c.u, c.dc, br, cv.label, cv.primary ? Btn::Primary : Btn::Secondary, cv.enabled, c.Hover(br) && cv.enabled);
                const RECT note{cx + c.S(14), by, br.left - c.S(8), by + c.S(30)};
                if (ci.applyingHere) {
                    const std::wstring line = OpLine();
                    DrawProgress(c.u, c.dc, RECT{note.left, by + c.S(2), note.right, by + c.S(10)}, g_progress.load(), true, false);
                    Text(c.dc, line.empty() ? std::wstring(L"Working\u2026") : line, RECT{note.left, by + c.S(12), note.right, by + c.S(30)}, c.u.f.tiny, col::sub, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
                } else if (!cv.note.empty()) Text(c.dc, cv.note, note, c.u.f.tiny, cv.noteKind == 2 ? col::err : cv.noteKind == 1 ? col::ok : col::faint, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
                const bool canApply = cv.enabled;
                if (canApply) c.Hit(br, [id] { IconApplyTheme(id); });
            }
        }
        c.y += static_cast<int>((groups.size() + cols - 1) / cols) * (chh + gap) + c.S(4);
        // 2.8.7: the "Candy preview background" opacity / blur controls were removed (they only changed a preview picture and had no use in the program)
        const IconPack* sp = FindPack(g_selPack);
        const std::wstring spName = sp ? std::wstring(sp->name) + (*sp->variant ? L" " + std::wstring(sp->variant) : L"") : L"";
        const IconThemeStatus spSt = sp ? g_iconStatus.Get(sp->id) : IconThemeStatus{};
        const bool spInstalled = sp != nullptr && spSt.checked && spSt.installed;   // the full validation happens in Apply, never while painting
        if (sp && *sp->note) {
            Para(c, std::wstring(L"\x24D8 ") + sp->note, c.u.f.small, col::sub);
        }
        Buttons(c, {
            {L"Apply " + spName, Btn::Primary, spInstalled, [] { const IconPack* p = FindPack(g_selPack); if (p) IconApplyTheme(p->id); }},
            {L"Restore default icons", Btn::Secondary, true, [] {
                if (!Confirm(L"Restore the default Windows icons by clearing AnkhCore's active icon theme from Windhawk Resource Redirect?", L"Restore default icons", false)) return;
                RunOp(L"Restore default Windows icons", [] {
                    auto prog = [](const std::wstring& t, int pc) { SetOpLine(t); g_progress = pc; };
                    return RestoreDefaultIconsRedirect(prog); }); }},
            {L"Restore legacy 7tsp", Btn::Ghost, FileExists(TspExe()), [] {
                if (TestGuard(L"Restore default icons (7tsp)")) return;
                if (!Confirm(L"Open 7tsp so you can restore system files patched by an older 2.2 installation?", L"Open 7tsp", false)) return;
                Restore7tsp(); }},
            {L"Restore originals (2.2 backup)", Btn::Ghost, g_hasLegacyIconBackups.load(), [] {
                if (!Confirm(L"Put back the original Windows icon files AnkhCore backed up before changing them?", L"Restore icons", false)) return;
                RunOp(L"Restore original Windows icons", [] {
                    auto prog = [](const std::wstring& t, int pc) { SetOpLine(t); g_progress = pc; };
                    const bool ok = RestoreOriginalIcons(prog); g_hasLegacyIconBackups.store(!BackedUpTargets().empty()); return ok; }, true); }},
            {L"Deep restore (SFC)", Btn::Ghost, g_hasLegacyIconBackups.load(), [] {
                if (!Confirm(L"Ask Windows (SFC) to restore every icon file AnkhCore knows from its own store? This can take several minutes.", L"Run SFC", false)) return;
                RunOp(L"Deep restore icons with SFC", [] { const bool ok = DeepRestoreIcons(OpLines()); g_hasLegacyIconBackups.store(!BackedUpTargets().empty()); return ok; }, true); }}});
    }
    {   // FT-2 / A01: what the last apply or restore verified, and the optional Explorer restart (a separate, confirmed action)
        IconRefreshReport rep = LastIconRefresh(); rep.oldMethodPending = s.icons.pending;
        const IconRefreshAdvice adv = IconAdvice(rep);
        if (!adv.text.empty()) Para(c, adv.text, c.u.f.small, adv.warn ? col::warn : col::sub);
        if (adv.offerExplorerRestart)
            Buttons(c, {{L"Restart Explorer", Btn::Secondary, !g_busy, [] {
                if (!Confirm(L"Restart Windows Explorer?\n\nOpen File Explorer windows close and the taskbar disappears for a few seconds. Nothing is lost, and Windows itself does not restart.", L"Restart Explorer", false)) return;
                RunOp(L"Restart Explorer", [] { return RestartExplorerShell(); }); }}});
    }
}
void PageAppearance(Ctx& c, const Cache& s) {
    Section(c, L"Windows customization", L"Windows theme, wallpaper, cursor and icon managers. AnkhCore's own look is in Settings (the gear in the top bar).");
    {
        auto link = [&](int page, const std::wstring& stateChip, COLORREF chipCol) {
            RowSpec r; r.icon = kPageIcon[page]; r.title = kPageName[page]; r.desc = kPageSub[page];
            if (!stateChip.empty()) r.chips.push_back({stateChip, chipCol});
            r.buttons.push_back({L"Open", Btn::Secondary, true, [page] { NavGo(page); }});
            Row(c, r);
        };
        link(PAppTheme, s.themeActive ? L"As3aD active" : L"Windows theme", s.themeActive ? col::ok : col::faint);
        link(PAppWallpaper, s.wallOn ? L"Slideshow on" : L"Off", s.wallOn ? col::ok : col::faint);
        CurRefresh(false);
        const bool curDefault = g_cur.now.name.empty() || Lower(g_cur.now.name) == L"windows default";
        link(PAppCursor, curDefault ? L"Windows default" : g_cur.now.name, curDefault ? col::faint : col::ok);
        const IconPack* actPk = s.icons.active.empty() ? nullptr : FindPack(s.icons.active);
        const std::wstring icChip = actPk ? std::wstring(actPk->name) : (s.icons.active.empty() ? L"Default icons" : s.icons.active);
        link(PAppIcons, icChip, actPk ? col::ok : col::faint);
    }
}
// 2.7 J2: AnkhCore's own settings. Every row opens an existing page or states a fact; no setting is duplicated or invented.
int g_settingsFrom = PDashboard;   // where the top-bar Settings control was opened from (what Back returns to)
void NavOpenSettings();
void DrawWindhawkTrayRow(Ctx& c, bool whInstalled);
void PageSettings(Ctx& c, const Cache& cache) {
    Para(c, L"These settings belong to AnkhCore itself. Windows personalization (theme, wallpapers, cursors, icons, Windhawk) is in Windows Studio.", c.u.f.small, col::sub);
    for (const SettingsCategory& cat : SettingsCategories()) {
        Section(c, cat.id, cat.about);
        for (const SettingsLink& l : SettingsLinks()) {
            if (std::wstring(l.category) != cat.id) continue;
            RowSpec r; r.icon = kPageIcon[l.page]; r.title = l.title; r.desc = l.desc;
            if (l.page == PAppMaterial) { const wchar_t* matName = g_material == MatAcrylic ? L"Acrylic" : g_material == MatFrosted ? L"Frosted" : g_material == MatWallpaper ? L"Wallpaper glass" : g_material == MatClear ? L"Clear glass" : L"Solid"; r.chips.push_back({matName, col::accent}); }
            const int pg = l.page;
            r.buttons = {{L"Open", Btn::Secondary, true, [pg] { NavGo(pg); }}};
            Row(c, r);
        }
        const std::wstring id = cat.id;
        if (id == L"General") DrawWindhawkTrayRow(c, cache.whInstalled);
        if (id == L"General") Para(c, L"Language: the interface is English. AnkhCore is not translated yet, so there is no language setting.", c.u.f.small, col::sub);
        if (id == L"Performance") Para(c, L"Cache management: AnkhCore does not offer a cache cleaner yet; its caches live in the data folder shown under Advanced.", c.u.f.small, col::sub);
        if (id == L"Updates") Para(c, std::wstring(kProductFull) + L" " + kVersionLabel + L". AnkhCore does not check for or download updates of itself: a newer version is installed with AnkhCore Setup, which keeps your settings.", c.u.f.small, col::sub);
        if (id == L"Privacy") Para(c, L"AnkhCore does not send usage data. Its own code contains no network client (source-inspected). Web links open in your browser only when you click them. Settings, logs and backups stay on this PC.", c.u.f.small, col::sub);
        if (id == L"Advanced") {
            Para(c, L"Data folder: " + DataDir(), c.u.f.small, col::sub);
            Para(c, L"Settings file: " + Cfg().Path(), c.u.f.small, col::sub);
            Buttons(c, {{L"Open data folder", Btn::Secondary, true, [] { if (TestGuard(L"Open data folder")) return; EnsureDir(DataDir()); ShellExecuteW(nullptr, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }},
                        {L"Open log folder", Btn::Secondary, true, [] { if (TestGuard(L"Open log folder")) return; ShellExecuteW(nullptr, L"open", (DataDir() + L"\\Logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}});
        }
    }
}

int g_appsFilter = 0;   // 0 = Desktop programs, 1 = Windows & Store apps, 2 = All
bool EnsureAppsList(Ctx& c, std::vector<AppEntry>& apps, int& win, int& st) {
    if (!g_appsLoaded && !g_appsLoading) LoadAppsAsync();
    if (!g_appsLoaded) {   // loading state: shimmer rows
        Para(c, L"Reading installed apps\x2026  (listing Microsoft Store apps can take a few seconds)", c.u.f.small, col::sub);
        for (int i = 0; i < 6; ++i) { Shimmer(c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(52)}, c.S(28), GetTickCount64()); c.y += c.S(60); }
        return false;
    }
    { std::lock_guard<std::mutex> lk(g_appsMx); apps = g_apps; }
    win = 0; st = 0; for (const auto& e : apps) (e.store ? st : win)++;
    return true;
}
void DrawAppRow(Ctx& c, const AppEntry& e) {
    RowSpec r; r.icon = e.store ? L"\xE8A9" : L"\xE977"; r.title = e.name;
    std::wstring d = e.publisher;
    if (!e.store && !e.version.empty()) d += (d.empty() ? L"v" : L"   \x00B7   v") + e.version;
    r.desc = d;
    const AppEntry cap = e;
    if (!e.store) {
        r.buttons = {{L"Uninstall", Btn::Danger, true, [cap] {
            if (TestGuard(L"Uninstall " + cap.name)) return;
            if (Confirm(L"Uninstall \"" + cap.name + L"\"?\n\nThis runs the program's own uninstaller. It cannot be undone.", L"Uninstall", true)) UninstallApp(cap); }}};
    } else {
        r.buttons = {{L"Remove", Btn::Danger, true, [cap] {
            if (TestGuard(L"Remove Store app " + cap.name)) return;
            if (Confirm(L"Remove the Store app \"" + cap.name + L"\"?\n\nUses PowerShell Remove-AppxPackage. It cannot be undone.", L"Remove app", true)) UninstallApp(cap); }}};
    }
    Row(c, r);
}
void PageAppsInstalled(Ctx& c, const Cache&) {
    std::vector<AppEntry> apps; int win = 0, st = 0;
    if (!EnsureAppsList(c, apps, win, st)) return;
    Section(c, L"Desktop programs (" + std::to_wstring(win) + L")", L"Desktop programs are removed by their own uninstaller. Uninstalling cannot be undone.");
    Buttons(c, {{L"Refresh list", Btn::Secondary, true, [] { if (kTestBuild && g_selftest && g_appsLoaded) return; g_appsLoaded = false; LoadAppsAsync(); }}});
    for (const auto& e : apps) if (!e.store) DrawAppRow(c, e);
}
void PageAppsBuiltin(Ctx& c, const Cache&) {
    std::vector<AppEntry> apps; int win = 0, st = 0;
    if (!EnsureAppsList(c, apps, win, st)) return;
    Section(c, L"Microsoft Store & built-in apps (" + std::to_wstring(st) + L")", L"Removed with PowerShell (Remove-AppxPackage). Some apps are needed by Windows, so remove only what you recognise.");
    Buttons(c, {{L"Refresh list", Btn::Secondary, true, [] { if (kTestBuild && g_selftest && g_appsLoaded) return; g_appsLoaded = false; LoadAppsAsync(); }}});
    for (const auto& e : apps) if (e.store) DrawAppRow(c, e);
}
// ---- 2.7 K4: the Apps Manager's uninstaller: select several programs, uninstall them one after another through their own uninstallers,
// then clean what they left behind (moved to a recovery folder, undoable). The engine is src/eng_apps_manager.h; this is only the page.
AppsMgr& Mgr() { static AppsMgr m; return m; }
void MgrNotify() { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); }
COLORREF UnStateColor(UnState st) {
    switch (st) {
    case UnState::Removed: return col::ok;
    case UnState::RemovedRestartRequired: case UnState::Unverified: case UnState::NotRemoved: case UnState::TimedOut: return col::warn;
    case UnState::Failed: return col::err;
    default: return col::faint;
    }
}
void PageAppsUninstall(Ctx& c, const Cache&) {
    std::vector<AppEntry> apps; int win = 0, st = 0;
    if (!EnsureAppsList(c, apps, win, st)) return;
    AppsMgr& m = Mgr();
    const AppsMgrView v = m.Snapshot();
    const bool busy = v.phase == 1 || v.phase == 3;

    if (v.phase == 1) {
        Section(c, L"Uninstalling\x2026", L"One program at a time, each through its own uninstaller (its window may appear: answer it there). A program counts as removed only when it is gone from the list afterwards.");
        Para(c, std::to_wstring(v.done) + L" of " + std::to_wstring(v.total) + L" finished" + (v.current.empty() ? std::wstring() : L"  \x00B7  now: " + v.current), c.u.f.small, col::sub);
        Buttons(c, {{L"Stop after this program", Btn::Secondary, true, [] { Mgr().CancelBatch(); }}});
    }
    if (v.phase >= 2) {
        Section(c, L"Uninstall result", v.report.summary);
        for (const UninstallItemResult& r : v.report.results) {
            RowSpec row; row.icon = r.item.store ? L"\xE8A9" : L"\xE977"; row.title = r.item.name; row.desc = r.detail;
            row.chips.push_back({UnStateLabel(r.state), UnStateColor(r.state)});
            Row(c, row);
        }
        if (v.report.restartRequired) Para(c, L"Windows must be restarted to finish removing some programs. AnkhCore never restarts Windows by itself.", c.u.f.small, col::warn);
    }
    if (!v.remnants.empty() && (v.phase == 2 || v.phase == 3 || v.phase == 4)) {
        size_t chosen = 0; for (const auto& r : v.remnants) if (r.selected) ++chosen;
        Section(c, L"Left behind (" + std::to_wstring(v.remnants.size()) + L")", L"Found after the programs were removed, from exact evidence only (their own install folder, shortcuts to deleted programs, their own registry entry). Nothing is deleted: chosen items are MOVED to a recovery folder and can be put back. Protected items are never touched.");
        for (size_t i = 0; i < v.remnants.size(); ++i) {
            const AppsMgrRemRow& rr = v.remnants[i];
            RowSpec row; row.icon = rr.c.kind == RemKind::RegistryKey ? L"\xE713" : rr.c.kind == RemKind::Shortcut ? L"\xE71B" : L"\xE8B7";
            row.title = rr.c.path; row.desc = rr.app + L"  \x00B7  " + rr.c.origin + L".  " + rr.c.reason;
            row.chips.push_back({RemClassLabel(rr.c.cls), rr.c.cls == RemClass::Safe ? col::ok : rr.c.cls == RemClass::Confirm ? col::warn : col::faint});
            if (rr.c.bytes) row.chips.push_back({FormatBytes(rr.c.bytes), col::sub});
            if (rr.c.cls != RemClass::Protected && !busy) { row.check = rr.selected ? 1 : 0; row.onCheck = [i] { Mgr().ToggleRemnant(i); InvalidateRect(g_wnd, nullptr, FALSE); }; }
            Row(c, row);
        }
        Buttons(c, {{L"Clean selected (" + std::to_wstring(chosen) + L")", Btn::Danger, chosen > 0 && !busy, [v] {
            if (TestGuard(L"Clean leftovers")) return;
            size_t nConfirm = 0; std::wstring list;
            for (const auto& r : v.remnants) if (r.selected) { if (r.c.cls == RemClass::Confirm) ++nConfirm; if (list.size() < 700) list += L"\n  \x2022 " + r.c.path; }
            if (!Confirm(L"Move the selected leftovers to the recovery folder?" + list + (nConfirm ? L"\n\n" + std::to_wstring(nConfirm) + L" of them need your confirmation (their own settings or uninstall entry): choosing Clean confirms them." : std::wstring()) +
                         L"\n\nNothing is deleted. You can undo this from this page.", L"Clean leftovers", true)) return;
            Mgr().StartClean(RemLiveBackend(), true, MgrNotify); }}});
    }
    if (v.phase == 4) {
        Section(c, L"Cleanup result", v.clean.summary);
        for (const CleanItemResult& r : v.clean.items) {
            RowSpec row; row.title = r.c.path; row.desc = r.detail; row.chips.push_back({CleanStateLabel(r.state), r.state == CleanState::Removed ? col::ok : r.state == CleanState::Failed ? col::err : col::faint});
            Row(c, row);
        }
    }
    if (!v.lastRecoveryDir.empty() && !busy) {
        Para(c, L"Recovery folder of the last cleanup: " + v.lastRecoveryDir, c.u.f.small, col::sub);
        Buttons(c, {{L"Undo last cleanup", Btn::Secondary, true, [] {
            if (TestGuard(L"Undo last cleanup")) return;
            if (!Confirm(L"Put everything from the last cleanup back where it was?", L"Undo cleanup", false)) return;
            const std::wstring t = Mgr().UndoLast(); LogI(L"Apps Manager undo: " + t); InvalidateRect(g_wnd, nullptr, FALSE); }}});
    }
    if (!v.undoText.empty()) Para(c, v.undoText, c.u.f.small, col::sub);
    if (v.phase == 2 || v.phase == 4) Buttons(c, {{L"Done", Btn::Secondary, true, [] { Mgr().Dismiss(); g_appsLoaded = false; LoadAppsAsync(); }}});

    Section(c, L"Select programs to uninstall", L"Tick the programs, then press Uninstall. Desktop programs run their own uninstaller; Microsoft Store apps are removed with PowerShell. AnkhCore itself cannot be removed from here.");
    Chips(c, L"Show", {{L"Desktop programs (" + std::to_wstring(win) + L")", g_appsFilter == 0, [] { g_appsFilter = 0; InvalidateRect(g_wnd, nullptr, FALSE); }},
                       {L"Windows & Store (" + std::to_wstring(st) + L")", g_appsFilter == 1, [] { g_appsFilter = 1; InvalidateRect(g_wnd, nullptr, FALSE); }},
                       {L"All (" + std::to_wstring(apps.size()) + L")", g_appsFilter == 2, [] { g_appsFilter = 2; InvalidateRect(g_wnd, nullptr, FALSE); }}});
    const size_t nSel = m.SelectedCount();
    Buttons(c, {{L"Uninstall selected (" + std::to_wstring(nSel) + L")", Btn::Danger, nSel > 0 && !busy, [apps] {
                    if (TestGuard(L"Uninstall selected programs")) return;
                    const std::set<std::wstring> sel = Mgr().Selection();
                    std::vector<UninstallItem> items; std::wstring names;
                    for (const AppEntry& e : apps) { UninstallItem it = AppToUninstallItem(e); if (sel.count(it.id)) { if (items.size() < 14) names += L"\n  \x2022 " + e.name; items.push_back(std::move(it)); } }
                    if (items.empty()) return;
                    if (!Confirm(L"Uninstall " + std::to_wstring(items.size()) + L" program(s)?" + names + (items.size() > 14 ? L"\n  \x2026" : std::wstring()) +
                                 L"\n\nEach one runs its own uninstaller, one after another. This cannot be undone (leftovers are offered afterwards and can be undone).", L"Uninstall", true)) return;
                    UninstallBackend be = UninstallLiveBackend(); be.scanRemnants = AppsScanHook(apps);
                    LogI(L"Apps Manager: uninstalling " + std::to_wstring(items.size()) + L" program(s)");
                    Mgr().StartUninstall(std::move(items), std::move(be), MgrNotify); }},
                {L"Clear selection", Btn::Secondary, nSel > 0 && !busy, [] { Mgr().ClearSelection(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                {L"Refresh list", Btn::Secondary, !busy, [] { if (kTestBuild && g_selftest && g_appsLoaded) return; g_appsLoaded = false; LoadAppsAsync(); }}});
    for (const auto& e : apps) {
        if (g_appsFilter == 0 && e.store) continue;
        if (g_appsFilter == 1 && !e.store) continue;
        const UninstallItem it = AppToUninstallItem(e);
        RowSpec r; r.icon = e.store ? L"\xE8A9" : L"\xE977"; r.title = e.name;
        std::wstring d = e.publisher;
        if (!e.store && !e.version.empty()) d += (d.empty() ? L"v" : L"   \x00B7   v") + e.version;
        r.desc = d;
        const std::wstring id = it.id;
        if (!busy) { r.check = m.Selected(id) ? 1 : 0; r.onCheck = [id] { Mgr().Toggle(id); InvalidateRect(g_wnd, nullptr, FALSE); }; }
        Row(c, r);
    }
}
void PageApps(Ctx& c, const Cache&) {
    std::vector<AppEntry> apps; int win = 0, st = 0;
    if (!EnsureAppsList(c, apps, win, st)) return;
    Section(c, L"Application pages", L"Desktop programs are removed by their own uninstaller. Microsoft Store apps are removed with PowerShell (Remove-AppxPackage).");
    {
        auto link = [&](int page, const std::wstring& countChip) {
            RowSpec r; r.icon = kPageIcon[page]; r.title = kPageName[page]; r.desc = kPageSub[page];
            r.chips.push_back({countChip, col::accent});
            r.buttons.push_back({L"Open", Btn::Secondary, true, [page] { NavGo(page); }});
            Row(c, r);
        };
        link(PAppsInstalled, std::to_wstring(win) + L" desktop");
        link(PAppsBuiltin, std::to_wstring(st) + L" Store / built-in");
        link(PAppsUninstall, std::to_wstring(apps.size()) + L" total");
        link(PAppsUpdates, L"UniGetUI");
    }
    Buttons(c, {{L"Refresh list", Btn::Secondary, true, [] { if (kTestBuild && g_selftest && g_appsLoaded) return; g_appsLoaded = false; LoadAppsAsync(); }}});
}
// ---------------------------------------------------------------- Windhawk page (FT-C01 settings, FT-C02 staged Apply)
// A small modal text prompt for the values a setting needs typed (the glass UI has no edit control). Not used by the self-test.
struct PromptState { std::wstring title, label, value; bool ok = false, done = false; HWND edit = nullptr; };
LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<PromptState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    static HBRUSH bg = nullptr; static COLORREF bgCol = 0;
    if (!bg || bgCol != col::overlay) { if (bg) DeleteObject(bg); bgCol = col::overlay; bg = CreateSolidBrush(bgCol); }
    switch (m) {
    case WM_CREATE: {
        st = static_cast<PromptState*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        const HINSTANCE hi = GetModuleHandleW(nullptr); const HFONT f = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND lab = CreateWindowExW(0, L"STATIC", st->label.c_str(), WS_CHILD | WS_VISIBLE, 16, 14, 468, 44, h, nullptr, hi, nullptr);
        st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 62, 468, 26, h, nullptr, hi, nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 300, 104, 88, 28, h, reinterpret_cast<HMENU>(IDOK), hi, nullptr);
        HWND no = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 396, 104, 88, 28, h, reinterpret_cast<HMENU>(IDCANCEL), hi, nullptr);
        for (HWND x : {lab, st->edit, ok, no}) SendMessageW(x, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        SendMessageW(st->edit, EM_SETSEL, 0, -1); SetFocus(st->edit);
        return 0; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: SetTextColor(reinterpret_cast<HDC>(w), col::text); SetBkColor(reinterpret_cast<HDC>(w), col::overlay); return reinterpret_cast<LRESULT>(bg);
    case WM_COMMAND:
        if (LOWORD(w) == IDOK && st) { wchar_t b[2048] = L""; GetWindowTextW(st->edit, b, 2048); st->value = b; st->ok = true; st->done = true; DestroyWindow(h); return 0; }
        if (LOWORD(w) == IDCANCEL && st) { st->done = true; DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: if (st) st->done = true; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
bool PromptText(const std::wstring& title, const std::wstring& label, std::wstring& value) {
    if (g_selftest) return false;
    static bool reg = false; static const std::wstring cls = Tagged(L"AnkhCorePrompt");
    if (!reg) { WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = PromptProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls.c_str(); wc.hbrBackground = CreateSolidBrush(RGB(12, 22, 44)); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassExW(&wc); reg = true; }
    PromptState st; st.title = title; st.label = label; st.value = value;
    RECT wr{}; GetWindowRect(g_wnd, &wr);
    const int W = 516, H = 180;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls.c_str(), title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, (wr.left + wr.right - W) / 2, (wr.top + wr.bottom - H) / 2, W, H, g_wnd, nullptr, GetModuleHandleW(nullptr), &st);
    if (!h) return false;
    const BOOL dark = TRUE; DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
    EnableWindow(g_wnd, FALSE);
    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) { if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    EnableWindow(g_wnd, TRUE); SetForegroundWindow(g_wnd);
    if (st.ok) value = st.value;
    return st.ok;
}
int PromptChoice(const std::vector<std::wstring>& items, int selected) {   // index chosen, or -1
    if (g_selftest || items.empty()) return -1;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < items.size() && i < 400; ++i) AppendMenuW(m, MF_STRING | (static_cast<int>(i) == selected ? MF_CHECKED : 0), static_cast<UINT_PTR>(i + 1), items[i].c_str());
    POINT pt; GetCursorPos(&pt); SetForegroundWindow(g_wnd);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    return cmd - 1;
}
std::wstring WhReferencePath(const std::string& id) { std::wstring n = Utf8ToW(id); for (auto& ch : n) if (!iswalnum(ch)) ch = L'_'; return DataDir() + L"\\WindhawkReference\\" + n + L".json"; }
std::wstring WhModName(const std::string& id) { for (const auto& m : Snapshot().mods) if (Utf8ToW(id) == m.id) return m.name; return Utf8ToW(id); }
// Enabling and disabling goes through windhawk-cli when Windhawk 2.x has it (verified by reading the state back); Windhawk 1.x keeps
// the registry switch it always had. Settings and the Explorer check need the CLI.
WhExec WhAppExec() {
    WhExec ex = WhCliExec();
    const bool cli = FileExists(WindhawkCliExePath());
    const auto cliEnable = ex.setEnabled;
    ex.setEnabled = [cli, cliEnable](const std::string& id, bool on, std::wstring& err) {
        if (cli) return cliEnable(id, on, err);
        const std::wstring wid = Utf8ToW(id);
        if (!SetModEnabled(wid, on, WhModName(id))) { err = L"Windhawk did not accept the change (see Activity)"; return false; }
        return true;
    };
    if (!cli) {
        ex.writeSettings = [](const std::string&, const std::vector<std::pair<std::string, JVal>>&, std::wstring& err) { err = L"settings need Windhawk 2.x (windhawk-cli.exe is not installed)"; return false; };
        ex.reachesExplorer = nullptr;
    }
    return ex;
}
void WhStageSet(const std::string& mod, const WhSettingDef& d, const JVal& want) {
    std::lock_guard<std::mutex> lk(g_whMx);
    const JVal* applied = WhFlatFind(g_whFlat, d.path);
    g_whStage.StageSetting(mod, d.path, want, applied ? *applied : d.def);
}
void WhReview() {   // REVIEW -> APPLY -> EXECUTE -> VERIFY
    std::wstring review; size_t n = 0;
    {   std::lock_guard<std::mutex> lk(g_whMx);
        n = g_whStage.Count();
        for (const WhOp& op : g_whStage.Plan()) {
            if (op.kind == WhOp::SetEnabled) review += L"\n  \x2022 " + WhModName(op.modId) + (op.enable ? L": turn on" : L": turn off");
            else review += L"\n  \x2022 " + WhModName(op.modId) + L": " + std::to_wstring(op.changes.size()) + L" setting(s)";
        } }
    if (!n) return;
    if (!Confirm(L"Apply " + std::to_wstring(n) + L" pending Windhawk change(s)?" + review + L"\n\nEach change is verified by reading it back. A change that fails stays pending so you can retry it.", L"Apply", false)) return;
    RunOp(L"Apply Windhawk changes", [] {
        WhStage work; { std::lock_guard<std::mutex> lk(g_whMx); work = g_whStage; }
        const WhApplyResult r = WhApply(work, WhAppExec());
        const std::wstring sum = WhSummary(r, [](const std::string& id) { return WhModName(id); });
        { std::lock_guard<std::mutex> lk(g_whMx); g_whStage = work; g_whSummary = sum; g_whSummaryOk = r.AllOk(); g_whExplorerOffer = r.offerExplorerRestart; }
        (r.AllOk() ? LogOk : LogE)(L"Windhawk: " + sum);
        std::string sel; { std::lock_guard<std::mutex> lk(g_whMx); sel = g_whSel; }
        if (!sel.empty()) WhLoadAsync(sel);   // show what Windhawk now holds
        return r.AllOk(); });
}
// Official Windhawk tray visibility setting: <RegistryKey>\Settings\HideTrayIcon (REG_DWORD 0/1).
// Never stops Windhawk Service or Engine; in kTestBuild, isolated under HKCU\Software\AnkhCore\Sandbox\WindhawkSettings.
inline HKEY WhTrayRegRoot() { return kTestBuild ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
inline std::wstring WhTrayRegSubkey() {
    return kTestBuild ? L"Software\\AnkhCore\\Sandbox\\WindhawkSettings" : L"SOFTWARE\\Windhawk\\Settings";
}
inline bool WhReadHideTrayIcon(bool& hiddenOut) {
    DWORD val = 0, sz = sizeof(val), type = 0;
    if (RegGetValueW(WhTrayRegRoot(), WhTrayRegSubkey().c_str(), L"HideTrayIcon", RRF_RT_REG_DWORD, &type, &val, &sz) == ERROR_SUCCESS) {
        hiddenOut = (val != 0);
        return true;
    }
    hiddenOut = false;
    return false;
}
inline bool WhWriteHideTrayIcon(bool hide, std::wstring& errOut) {
    if (!Cfg().Bool(L"wh.trayOrigSaved", false)) {
        bool orig = false;
        WhReadHideTrayIcon(orig);
        Cfg().SetBool(L"wh.trayOrigSaved", true);
        Cfg().SetInt(L"wh.trayOrigVal", orig ? 1 : 0);
        Cfg().Save();
    }
    HKEY k = nullptr;
    LONG rc = RegCreateKeyExW(WhTrayRegRoot(), WhTrayRegSubkey().c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) { errOut = L"Registry error " + std::to_wstring(rc); return false; }
    const DWORD v = hide ? 1 : 0;
    rc = RegSetValueExW(k, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) { errOut = L"Registry write failed (" + std::to_wstring(rc) + L")"; return false; }
    return true;
}
inline bool WhRestoreTrayIcon(std::wstring& errOut) {
    const bool orig = Cfg().Int(L"wh.trayOrigVal", 0, 0, 1) != 0;
    HKEY k = nullptr;
    LONG rc = RegCreateKeyExW(WhTrayRegRoot(), WhTrayRegSubkey().c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) { errOut = L"Registry error " + std::to_wstring(rc); return false; }
    const DWORD v = orig ? 1 : 0;
    rc = RegSetValueExW(k, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) { errOut = L"Registry restore failed (" + std::to_wstring(rc) + L")"; return false; }
    return true;
}

// 2.8.7: Windhawk's System Tray icon switch lives in AnkhCore Settings (it is an AnkhCore preference, not a mod).
void DrawWindhawkTrayRow(Ctx& c, bool whInstalled) {
    bool trayHidden = false;
    const bool hasTrayKey = WhReadHideTrayIcon(trayHidden);
    if (!(whInstalled || hasTrayKey)) { Para(c, L"Windhawk is not installed, so there is no Windhawk tray icon to manage.", c.u.f.small, col::sub); return; }
    {
            RowSpec tr; tr.icon = L"\xE8B7"; tr.title = L"Windhawk System Tray Icon";
            tr.desc = L"Uses Windhawk's official HideTrayIcon setting (HKLM\\SOFTWARE\\Windhawk\\Settings). Windhawk Engine and mods keep running in the background; other tray icons are unaffected.";
            tr.chips.push_back({trayHidden ? L"Hidden" : L"Visible", trayHidden ? col::ok : col::sub});
            tr.buttons = {
                {trayHidden ? L"Show Tray Icon" : L"Hide Tray Icon", Btn::Secondary, !g_busy, [trayHidden] {
                    const bool targetHide = !trayHidden;
                    if (!Confirm(targetHide
                        ? L"Hide Windhawk's own icon from the Windows System Tray (sets HideTrayIcon=1)?\n\nWindhawk Service, Engine, updates, and all mods continue running normally."
                        : L"Show Windhawk's icon in the Windows System Tray again (sets HideTrayIcon=0)?",
                        L"Windhawk Tray Icon", false)) return;
                    std::wstring err;
                    if (WhWriteHideTrayIcon(targetHide, err)) {
                        LogOk(targetHide ? L"Windhawk tray icon hidden (HideTrayIcon=1)." : L"Windhawk tray icon shown (HideTrayIcon=0).");
                    } else {
                        LogE(L"Windhawk tray icon update failed: " + err);
                    }
                    InvalidateRect(g_wnd, nullptr, FALSE);
                }},
                {L"Restore Original", Btn::Ghost, !g_busy && Cfg().Bool(L"wh.trayOrigSaved", false), [] {
                    std::wstring err;
                    if (WhRestoreTrayIcon(err)) LogOk(L"Restored Windhawk's original HideTrayIcon setting.");
                    else LogE(L"Failed to restore Windhawk tray setting: " + err);
                    InvalidateRect(g_wnd, nullptr, FALSE);
                }}
            };
            Row(c, tr);
    }
}
void PageWindhawk(Ctx& c, const Cache& s) {
    const bool cli = FileExists(WindhawkCliExePath());
    {
        std::lock_guard<std::mutex> lk(g_whMx);
        if (!g_whSel.empty()) {
            const std::string sel = g_whSel;
            const size_t pendingCnt = g_whStage.SettingCount(sel);
            std::wstring modVer, modAuthor;
            for (const auto& m : s.mods) if (WToUtf8(m.id) == sel) { modVer = m.version; modAuthor = m.author; break; }

            auto closeModSettings = [sel](bool confirmDiscard) {
                size_t pCnt = 0;
                { std::lock_guard<std::mutex> l(g_whMx); pCnt = g_whStage.SettingCount(sel); }
                if (confirmDiscard && pCnt > 0) {
                    if (!Confirm(L"You have " + std::to_wstring(pCnt) + L" unsaved setting change(s) for " + WhModName(sel) + L". Discard them and return?", L"Unsaved Windhawk Changes", false))
                        return;
                }
                {
                    std::lock_guard<std::mutex> l(g_whMx);
                    g_whStage.CancelMod(sel);
                    g_whSel.clear(); g_whGroup.clear(); g_whDefs.clear(); g_whFlat.clear();
                }
                g_scroll[g_page] = 0;
                InvalidateRect(g_wnd, nullptr, FALSE);
            };
            auto saveModSettings = [sel] {
                WhStage oneMod;
                {
                    std::lock_guard<std::mutex> l(g_whMx);
                    for (const WhOp& op : g_whStage.Plan()) {
                        if (op.modId != sel) continue;
                        if (op.kind == WhOp::SetEnabled) oneMod.StageEnabled(op.modId, op.enable, !op.enable);
                        else for (const auto& ch : op.changes) {
                            const JVal* cur = WhFlatFind(g_whFlat, ch.first);
                            oneMod.StageSetting(op.modId, ch.first, ch.second, cur ? *cur : ch.second);
                        }
                    }
                }
                if (oneMod.Empty()) {
                    std::lock_guard<std::mutex> l(g_whMx);
                    g_whSel.clear(); g_whGroup.clear(); g_whDefs.clear(); g_whFlat.clear();
                    g_scroll[g_page] = 0; InvalidateRect(g_wnd, nullptr, FALSE);
                    return;
                }
                RunOp(L"Save " + WhModName(sel) + L" settings", [sel, oneMod] {
                    WhStage work = oneMod;
                    const WhApplyResult r = WhApply(work, WhAppExec());
                    const std::wstring sum = WhSummary(r, [](const std::string& id) { return WhModName(id); });
                    {
                        std::lock_guard<std::mutex> l(g_whMx);
                        g_whSummary = sum; g_whSummaryOk = r.AllOk(); g_whExplorerOffer = r.offerExplorerRestart;
                        if (r.AllOk()) {
                            g_whStage.CancelMod(sel);
                            g_whSel.clear(); g_whGroup.clear(); g_whDefs.clear(); g_whFlat.clear();
                            g_scroll[g_page] = 0;
                        }
                    }
                    (r.AllOk() ? LogOk : LogE)(L"Windhawk: " + sum);
                    return r.AllOk();
                });
            };

            Buttons(c, {
                {L"\x2190 Back to Windhawk", Btn::Secondary, !g_busy, [closeModSettings] { closeModSettings(true); }},
                {L"Save" + (pendingCnt ? L" (" + std::to_wstring(pendingCnt) + L")" : L""), Btn::Primary, !g_busy && pendingCnt > 0, [saveModSettings] { saveModSettings(); }},
                {L"Cancel", Btn::Ghost, !g_busy, [closeModSettings] { closeModSettings(true); }}
            });

            RowSpec hdr; hdr.icon = L"\xE74C"; hdr.title = WhModName(sel);
            hdr.desc = L"Mod ID: " + Utf8ToW(sel) + (modAuthor.empty() ? L"" : L"  \x00B7  by " + modAuthor) + (WhIsModifiedByAnkhCore(Utf8ToW(sel)) ? L"  \x00B7  " + WhModifiedNote(Utf8ToW(sel)) + L" (original author and licence kept)" : L"") +
                       L"\nChanges are staged until you press Save. List settings are preserved untouched.";
            if (!modVer.empty()) hdr.chips.push_back({L"v" + modVer, col::accent});
            if (pendingCnt > 0) hdr.chips.push_back({std::to_wstring(pendingCnt) + L" unsaved", col::warn});
            Row(c, hdr);

            if (g_whLoading) { Para(c, L"Reading the settings from Windhawk\x2026", c.u.f.small, col::sub); return; }
            if (g_whDefs.empty()) { Para(c, g_whLoadErr.empty() ? L"This mod declares no editable settings." : g_whLoadErr, c.u.f.small, g_whLoadErr.empty() ? col::sub : col::warn); return; }
            if (sel.find("mouse-trail") != std::string::npos) {
                Para(c, L"Mouse Trail draws one effect that follows the pointer, whatever cursor is showing, so every cursor role already gets it.", c.u.f.small, col::sub);
            }
            {
                std::vector<ChipOpt> secs; bool general = false; std::string firstGroup;
                for (const auto& d : g_whDefs) {
                    if (d.depth == 0 && (d.kind == WhKind::Group || (d.kind == WhKind::List && !d.itemDefs.empty()))) {
                        if (firstGroup.empty()) firstGroup = d.path;
                        secs.push_back({Utf8ToW(d.name), g_whGroup == d.path, [p = d.path] { g_whGroup = p; g_scroll[g_page] = 0; }});
                    } else if (d.depth == 0) general = true;
                }
                if (general) secs.insert(secs.begin(), ChipOpt{L"General", g_whGroup.empty(), [] { g_whGroup.clear(); g_scroll[g_page] = 0; }});
                else if (g_whGroup.empty() && !firstGroup.empty()) g_whGroup = firstGroup;
                if (secs.size() > 1) Chips(c, L"Section", secs);
            }
            auto renderSettingRow = [&](const WhSettingDef& d) {
                const JVal* applied = WhFlatFind(g_whFlat, d.path); const JVal cur = applied ? *applied : d.def;
                const JVal* pend = g_whStage.SettingPending(sel, d.path); const JVal& val = pend ? *pend : cur;
                RowSpec r; r.title = Utf8ToW(d.name);
                std::wstring desc = Utf8ToW(d.description); if (desc.size() > 150) desc = desc.substr(0, 147) + L"\x2026"; r.desc = desc;
                if (pend) r.chips.push_back({L"Unsaved (was " + Utf8ToW(WhValueText(cur)) + L")", col::warn});
                else if (d.kind != WhKind::List && !WhValueEquals(cur, d.def)) r.chips.push_back({L"Modified", col::faint});
                const WhSettingDef dd = d;
                switch (d.kind) {
                case WhKind::Bool: r.toggle = WhAsBool(val) ? 1 : 0; r.enabled = !g_busy; r.onToggle = [sel, dd, val] { WhStageSet(sel, dd, WhMakeNum(WhAsBool(val) ? 0 : 1)); }; break;
                case WhKind::Enum: {
                    std::wstring label = Utf8ToW(WhValueText(val)); for (const auto& o : d.options) if (o.value == WhValueText(val)) label = Utf8ToW(o.label);
                    r.right = label;
                    r.buttons = {{L"Choose\x2026", Btn::Secondary, !g_busy, [sel, dd, val] {
                        std::vector<std::wstring> items; int cur = -1; for (size_t i = 0; i < dd.options.size(); ++i) { items.push_back(Utf8ToW(dd.options[i].label)); if (dd.options[i].value == WhValueText(val)) cur = static_cast<int>(i); }
                        const int k = PromptChoice(items, cur); if (k >= 0) WhStageSet(sel, dd, WhMakeStr(dd.options[k].value)); }}};
                    break; }
                case WhKind::Int: case WhKind::Real: case WhKind::Text: case WhKind::Color:
                    r.right = d.kind == WhKind::Color ? L"#" + Utf8ToW(WhValueText(val)) : Utf8ToW(WhValueText(val)).substr(0, 40);
                    r.buttons = {{L"Edit\x2026", Btn::Secondary, !g_busy, [sel, dd, val] {
                        std::wstring text = Utf8ToW(WhValueText(val));
                        for (;;) {
                            if (!PromptText(Utf8ToW(dd.name), Utf8ToW(dd.description).substr(0, 180), text)) return;
                            JVal nv; std::wstring err;
                            if (WhParseInput(dd, WToUtf8(text), nv, err)) { WhStageSet(sel, dd, nv); return; }
                            MessageBoxW(g_wnd, (L"That value does not fit this setting: " + err + L".").c_str(), kProductFull, MB_OK | MB_ICONINFORMATION);
                        } }}};
                    break;
                case WhKind::List: r.right = L"list (" + std::to_wstring(d.listItems) + L")"; r.desc += (r.desc.empty() ? L"" : L"  ") + std::wstring(L"Edit this list in Windhawk; it is kept as it is."); r.enabled = false; break;
                default: break;
                }
                if (d.kind != WhKind::List && !WhValueEquals(val, d.def))
                    r.buttons.push_back({L"Default", Btn::Ghost, !g_busy, [sel, dd] { WhStageSet(sel, dd, dd.def); }});
                Row(c, r);
            };
            const WhSettingDef* activeListDef = nullptr;
            for (const auto& d : g_whDefs) if (d.depth == 0 && d.kind == WhKind::List && !d.itemDefs.empty() && g_whGroup == d.path) { activeListDef = &d; break; }
            int shown = 0;
            if (activeListDef) {
                const std::string listPath = activeListDef->path;
                const std::string listBracket = listPath + "[";
                int maxIdx = activeListDef->listItems - 1;
                auto checkKeyIdx = [&](const std::string& k) {
                    if (k.compare(0, listBracket.size(), listBracket) != 0) return;
                    const size_t rb = k.find(']', listBracket.size());
                    if (rb == std::string::npos) return;
                    const int idx = std::atoi(k.substr(listBracket.size(), rb - listBracket.size()).c_str());
                    if (idx > maxIdx) maxIdx = idx;
                };
                for (const auto& kv : g_whFlat) checkKeyIdx(kv.first);
                for (const WhOp& op : g_whStage.Plan()) if (op.modId == sel) for (const auto& ch : op.changes) checkKeyIdx(ch.first);
                const int itemCount = std::max(1, maxIdx + 1);
                if (g_whListIdx < 0 || g_whListIdx >= itemCount) g_whListIdx = 0;
                auto readItemVal = [&](int idx, const std::string& subKey, const JVal& fallback) -> JVal {
                    const std::string fk = listPath + "[" + std::to_string(idx) + "]." + subKey;
                    if (const JVal* p = g_whStage.SettingPending(sel, fk)) return *p;
                    if (const JVal* a = WhFlatFind(g_whFlat, fk)) return *a;
                    return fallback;
                };
                std::vector<ChipOpt> itemChips;
                for (int idx = 0; idx < itemCount; ++idx) {
                    const std::string exe = WhValueText(readItemVal(idx, "executable", WhMakeStr("")));
                    const bool en = WhAsBool(readItemVal(idx, "enabled", WhMakeNum(1)));
                    std::wstring lbl = exe.empty() ? (L"#" + std::to_wstring(idx + 1) + L" (template)") : Utf8ToW(exe);
                    if (!en) lbl += L" (off)";
                    itemChips.push_back({lbl, g_whListIdx == idx, [idx] { g_whListIdx = idx; InvalidateRect(g_wnd, nullptr, FALSE); }});
                }
                Chips(c, L"Program", itemChips);
                const WhSettingDef listCopy = *activeListDef;
                Buttons(c, {
                    {L"+ Add Program\x2026", Btn::Primary, !g_busy, [sel, listCopy, itemCount] {
                        std::wstring exeName;
                        if (!PromptText(L"Add Program Animation Profile", L"Type the program file name (e.g. chrome.exe, discord.exe, code.exe, notepad.exe):", exeName)) return;
                        while (!exeName.empty() && iswspace(exeName.front())) exeName.erase(exeName.begin());
                        while (!exeName.empty() && iswspace(exeName.back())) exeName.pop_back();
                        if (exeName.empty()) return;
                        int targetIdx = itemCount;
                        {
                            std::lock_guard<std::mutex> l(g_whMx);
                            const std::string k0 = listCopy.path + "[0].executable";
                            const JVal* p0 = g_whStage.SettingPending(sel, k0);
                            const JVal* a0 = WhFlatFind(g_whFlat, k0);
                            const std::string v0 = p0 ? WhValueText(*p0) : (a0 ? WhValueText(*a0) : "");
                            if (v0.empty()) targetIdx = 0;
                        }
                        for (const WhSettingDef& id : listCopy.itemDefs) {
                            if (id.kind == WhKind::Group || id.kind == WhKind::List) continue;
                            WhSettingDef dd = id;
                            dd.path = listCopy.path + "[" + std::to_string(targetIdx) + "]." + id.path;
                            JVal v = id.def;
                            if (id.path == "enabled") v = WhMakeNum(1);
                            else if (id.path == "executable") v = WhMakeStr(WToUtf8(exeName));
                            else if (id.path == "minimize_restore.mode" || id.path == "launch_close.mode") v = WhMakeStr("custom");
                            WhStageSet(sel, dd, v);
                        }
                        g_whListIdx = targetIdx;
                        InvalidateRect(g_wnd, nullptr, FALSE);
                    }},
                    {g_whShowOwnPool ? L"Hide Own Pool Checkboxes" : L"Show Own Pool Checkboxes", Btn::Secondary, !g_busy, [] {
                        g_whShowOwnPool = !g_whShowOwnPool;
                        InvalidateRect(g_wnd, nullptr, FALSE);
                    }}
                });
                static std::set<std::string> s_openPacks;
                auto renderPackHeader = [&](const WhSettingDef& gd, const std::string& packKey) -> bool {
                    const bool open = s_openPacks.count(packKey) > 0;
                    RowSpec pr;
                    pr.icon = open ? L"\x25BC" : L"\x25B6";
                    pr.title = Utf8ToW(gd.name);
                    pr.desc = Utf8ToW(gd.description.empty() ? "Click to open or close this animation pack." : gd.description);
                    pr.chips.push_back({open ? L"Opened" : L"Pack (click to expand)", open ? col::ok : col::accent});
                    pr.buttons = {{open ? L"\x25B2 Close Pack" : L"\x25BC Open Pack", Btn::Secondary, !g_busy, [packKey] {
                        if (s_openPacks.count(packKey)) s_openPacks.erase(packKey);
                        else s_openPacks.insert(packKey);
                        InvalidateRect(g_wnd, nullptr, FALSE);
                    }}};
                    Row(c, pr);
                    return open;
                };
                const std::string itemPrefix = listPath + "[" + std::to_string(g_whListIdx) + "].";
                std::string skipPackPrefix;
                for (const WhSettingDef& id : activeListDef->itemDefs) {
                    const bool isOwnPool = id.path.find("own_pool") != std::string::npos;
                    if (isOwnPool && !g_whShowOwnPool) continue;
                    if (!skipPackPrefix.empty()) {
                        if (id.path.compare(0, skipPackPrefix.size(), skipPackPrefix) == 0) continue;
                        skipPackPrefix.clear();
                    }
                    if (id.kind == WhKind::Group) {
                        if (id.path.find(".pack_") != std::string::npos) {
                            ++shown;
                            if (!renderPackHeader(id, itemPrefix + id.path)) skipPackPrefix = id.path + ".";
                        } else {
                            Section(c, Utf8ToW(id.name), Utf8ToW(id.description));
                        }
                        continue;
                    }
                    ++shown;
                    WhSettingDef dd = id;
                    dd.path = itemPrefix + id.path;
                    renderSettingRow(dd);
                }
            } else {
                const std::string prefix = g_whGroup.empty() ? "" : g_whGroup + ".";
                std::string skipPackPrefix;
                for (const WhSettingDef& d : g_whDefs) {
                    const bool inSection = g_whGroup.empty() ? (d.depth == 0 && d.kind != WhKind::Group && (d.kind != WhKind::List || d.itemDefs.empty())) : (d.path.compare(0, prefix.size(), prefix) == 0 && d.path.size() > prefix.size());
                    if (!inSection) continue;
                    if (!skipPackPrefix.empty()) {
                        if (d.path.compare(0, skipPackPrefix.size(), skipPackPrefix) == 0) continue;
                        skipPackPrefix.clear();
                    }
                    if (d.kind == WhKind::Group) {
                        static std::set<std::string> s_openGlobalPacks;
                        if (d.path.find(".pack_") != std::string::npos) {
                            ++shown;
                            const std::string packKey = d.path;
                            const bool open = s_openGlobalPacks.count(packKey) > 0;
                            RowSpec pr;
                            pr.icon = open ? L"\x25BC" : L"\x25B6";
                            pr.title = Utf8ToW(d.name);
                            pr.desc = Utf8ToW(d.description.empty() ? "Click to open or close this animation pack." : d.description);
                            pr.chips.push_back({open ? L"Opened" : L"Pack (click to expand)", open ? col::ok : col::accent});
                            pr.buttons = {{open ? L"\x25B2 Close Pack" : L"\x25BC Open Pack", Btn::Secondary, !g_busy, [packKey] {
                                if (s_openGlobalPacks.count(packKey)) s_openGlobalPacks.erase(packKey);
                                else s_openGlobalPacks.insert(packKey);
                                InvalidateRect(g_wnd, nullptr, FALSE);
                            }}};
                            Row(c, pr);
                            if (!open) skipPackPrefix = d.path + ".";
                        } else {
                            Section(c, Utf8ToW(d.name), Utf8ToW(d.description));
                        }
                        continue;
                    }
                    ++shown;
                    renderSettingRow(d);
                }
            }
            if (!shown) Para(c, L"Nothing to show in this section.", c.u.f.small, col::sub);
            Buttons(c, {
                {L"Save" + (pendingCnt ? L" (" + std::to_wstring(pendingCnt) + L")" : L""), Btn::Primary, !g_busy && pendingCnt > 0, [saveModSettings] { saveModSettings(); }},
                {L"Reset to Default", Btn::Secondary, !g_busy, [sel] {
                    if (!Confirm(L"Stage the declared default for every setting of this mod? Press Save to apply.", L"Reset to Default", false)) return;
                    std::lock_guard<std::mutex> l(g_whMx);
                    for (const WhSettingDef& d : g_whDefs) { if (d.kind == WhKind::Group || d.kind == WhKind::List) continue; const JVal* a = WhFlatFind(g_whFlat, d.path); g_whStage.StageSetting(sel, d.path, d.def, a ? *a : d.def); }
                }},
                {L"Cancel", Btn::Ghost, !g_busy, [closeModSettings] { closeModSettings(true); }}
            });
            return;
        }
    }

    {
        bool trayHidden = false;
        const bool hasTrayKey = WhReadHideTrayIcon(trayHidden);
        RowSpec r; r.icon = L"\xE74C"; r.title = s.whInstalled ? L"Windhawk" : L"Windhawk is not installed";
        r.desc = !s.whInstalled ? L"Windhawk is not installed. The AnkhCore setup does not install Windhawk or mods; install Windhawk yourself to use them here." : s.whRunning ? L"Running. Changes are staged here and applied together; turning a mod off keeps its settings." : L"Stopped: no mod is running.";
        r.chips.push_back({s.whRunning ? L"Running" : L"Stopped", s.whRunning ? col::ok : col::warn});
        if (s.whInstalled || hasTrayKey) r.chips.push_back({trayHidden ? L"Tray icon hidden" : L"Tray icon visible", trayHidden ? col::accent : col::faint});
        // 2.9: Windhawk is a tool of AnkhCore: no "Open Windhawk" button (it has no shortcuts either) and it never updates itself; a newer Windhawk comes with an AnkhCore update.
        const bool updOff = RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Settings", L"DisableUpdateCheck", 0) != 0;
        if (s.whInstalled) r.chips.push_back({updOff ? L"Updates: off" : L"Updates: on", updOff ? col::ok : col::warn});
        r.buttons = {{L"Restart Windhawk", Btn::Secondary, s.whInstalled, [] { RunOp(L"Restart Windhawk", [] { return RestartWindhawk(); }); }}};
        if (s.whInstalled && !updOff)
            r.buttons.push_back({L"Turn Windhawk updates off", Btn::Secondary, !g_busy, [] {
                if (TestGuard(L"Turn Windhawk updates off")) return;
                RunOp(L"Turn Windhawk updates off", [] {
                    JVal doc; std::wstring err; const bool ran = RunWindhawkCliJson(L"app settings set disableUpdateCheck true", doc, err, L"", 15000);
                    if (!ran) { LogE(L"Windhawk updates could not be switched off: " + err); return false; }
                    LogOk(L"Windhawk will not check for or install its own updates (a newer Windhawk comes with an AnkhCore update)."); return true; }); }});
        Row(c, r);

        // 2.8.7: the System Tray option moved to AnkhCore Settings (General).
    }
    std::lock_guard<std::mutex> lk(g_whMx);
    if (!g_whStage.Empty()) {   // PENDING: what will change, beside what is applied now
        Section(c, L"Pending changes", L"Nothing has been changed in Windhawk yet. Review the list and press Apply, or discard it.");
        for (const WhOp& op : g_whStage.Plan())
            Para(c, L"\x2022 " + WhModName(op.modId) + (op.kind == WhOp::SetEnabled ? (op.enable ? L": turn on" : L": turn off") : L": " + std::to_wstring(op.changes.size()) + L" setting(s) changed"), c.u.f.small, col::warn);
        Buttons(c, {{L"Apply " + std::to_wstring(g_whStage.Count()) + L" change(s)", Btn::Primary, !g_busy && s.whInstalled, [] { WhReview(); }},
                    {L"Discard all", Btn::Secondary, !g_busy, [] { std::lock_guard<std::mutex> l(g_whMx); g_whStage.DiscardAll(); }}});
    }
    if (!g_whSummary.empty()) {
        Para(c, g_whSummary, c.u.f.small, g_whSummaryOk ? col::sub : col::warn);
        if (g_whExplorerOffer) Buttons(c, {{L"Restart Explorer", Btn::Secondary, !g_busy, [] {
            if (!Confirm(L"Restart Windows Explorer once so every change is picked up?\n\nOpen File Explorer windows close and the taskbar disappears for a few seconds. Windows itself does not restart.", L"Restart Explorer", false)) return;
            { std::lock_guard<std::mutex> l(g_whMx); g_whExplorerOffer = false; }
            RunOp(L"Restart Explorer", [] { return RestartExplorerShell(); }); }}});
    }
    int on = 0; for (const auto& m : s.mods) on += m.enabled;
    Section(c, L"Mods (" + std::to_wstring(on) + L" of " + std::to_wstring(s.mods.size()) + L" active)",
            L"Organized by category. Click any category header to expand or collapse its mods." + std::wstring(cli ? L"" : L" Settings need Windhawk 2.x (windhawk-cli.exe)."));
    if (s.mods.empty()) {
        StatePanel(c, L"\xE74C", L"No Windhawk mods found", L"Mods you install in Windhawk appear here.");
        return;
    }

    const auto& catSpecs = WhCategorySpecs();
    bool anyExpanded = false, allExpanded = true;
    for (size_t ci = 0; ci < catSpecs.size(); ++ci) {
        const bool exp = Cfg().Bool(catSpecs[ci].key, ci == 0);
        anyExpanded |= exp;
        allExpanded &= exp;
    }
    Chips(c, L"Categories", {
        {L"Expand all", allExpanded, [&catSpecs] {
            for (const auto& sp : catSpecs) Cfg().SetBool(sp.key, true);
            Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE);
        }},
        {L"Collapse all", !anyExpanded, [&catSpecs] {
            for (const auto& sp : catSpecs) Cfg().SetBool(sp.key, false);
            Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE);
        }}
    });

    for (size_t ci = 0; ci < catSpecs.size(); ++ci) {
        const WhCategorySpec& sp = catSpecs[ci];
        std::vector<const WhMod*> catMods;
        int catOn = 0, catPending = 0;
        for (const auto& m : s.mods) {
            if (ClassifyWindhawkModCategory(m.id, m.name) == sp.id) {
                catMods.push_back(&m);
                if (m.enabled) ++catOn;
                const std::string sid = WToUtf8(m.id);
                bool want = m.enabled;
                if (g_whStage.EnabledPending(sid, want) || g_whStage.SettingCount(sid) > 0) ++catPending;
            }
        }
        if (catMods.empty()) continue;   // Never render empty categories

        const bool expanded = Cfg().Bool(sp.key, ci == 0);
        const int hdrH = c.S(58);
        RECT hdr{c.x, c.y, c.x + c.w, c.y + hdrH};
        if (c.Visible(hdr)) {
            const bool hov = c.Hover(hdr);
            Round(c.dc, hdr, c.S(16), expanded ? col::panel2 : (hov ? col::panel2 : col::panel),
                  expanded ? col::accent : (hov ? col::accent : col::border));
            // Category icon badge
            RECT icBox{hdr.left + c.S(14), hdr.top + c.S(11), hdr.left + c.S(50), hdr.bottom - c.S(11)};
            Round(c.dc, icBox, c.S(10), expanded ? col::accentDeep : col::bg, expanded ? col::accent : col::border);
            Text(c.dc, sp.icon, icBox, c.u.f.icon, expanded ? col::accent : col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Title & subtitle
            const int tx = icBox.right + c.S(12);
            const int titleW = TextWidth(c.dc, sp.title, c.u.f.h2);
            Text(c.dc, sp.title, RECT{tx, hdr.top + c.S(8), tx + titleW + c.S(8), hdr.top + c.S(30)}, c.u.f.h2, col::text, DT_VCENTER | DT_SINGLELINE);
            int cx = tx + titleW + c.S(12);
            const std::wstring onBadge = std::to_wstring(catOn) + L" / " + std::to_wstring(catMods.size()) + L" on";
            Chip(c, cx, hdr.top + c.S(10), onBadge, catOn > 0 ? col::ok : col::faint);
            cx += TextWidth(c.dc, onBadge, c.u.f.tiny) + c.S(24);
            if (catPending > 0) {
                Chip(c, cx, hdr.top + c.S(10), std::to_wstring(catPending) + L" pending", col::warn);
            }
            Text(c.dc, sp.subtitle, RECT{tx, hdr.top + c.S(32), hdr.right - c.S(56), hdr.bottom - c.S(6)}, c.u.f.tiny, col::sub, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Dropdown / expand arrow indicator (▼ expanded, ▶ collapsed)
            RECT arrowRc{hdr.right - c.S(46), hdr.top + c.S(12), hdr.right - c.S(12), hdr.bottom - c.S(12)};
            Round(c.dc, arrowRc, c.S(8), hov ? col::accentDeep : col::bg, hov ? col::accent : col::border);
            Text(c.dc, expanded ? L"\x25BC" : L"\x25B6", arrowRc, c.u.f.small, expanded ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const std::wstring keyStr = sp.key;
        c.Hit(hdr, [keyStr, expanded] {
            Cfg().SetBool(keyStr, !expanded);
            Cfg().Save();
            InvalidateRect(g_wnd, nullptr, FALSE);
        });
        c.y += hdrH + c.S(8);

        if (expanded) {
            const int indent = c.S(16);
            const int groupTop = c.y;
            c.x += indent;
            c.w -= indent;
            for (const WhMod* pm : catMods) {
                const WhMod& m = *pm;
                const std::string sid = WToUtf8(m.id); bool want = m.enabled; const bool pend = g_whStage.EnabledPending(sid, want);
                RowSpec r; r.title = m.name; r.desc = m.id;
                r.chips.push_back({m.enabled ? L"On now" : L"Off now", m.enabled ? col::ok : col::faint});
                if (pend) r.chips.push_back({want ? L"Pending: turn on" : L"Pending: turn off", col::warn});
                if (g_whStage.SettingCount(sid)) r.chips.push_back({std::to_wstring(g_whStage.SettingCount(sid)) + L" setting(s) pending", col::warn});
                if (!m.version.empty()) r.chips.push_back({L"v" + m.version, col::accent});
                if (!m.author.empty()) r.chips.push_back({m.author, col::sub});
                if (WhIsModifiedByAnkhCore(m.id)) r.chips.push_back({WhModifiedNote(m.id), col::accent});   // 2.8.3: a modified upstream mod says so
                if (m.id.find(L"mouse-trail") != std::wstring::npos) r.chips.push_back({L"Mouse effect", col::warn});
                r.toggle = want; r.enabled = s.whInstalled && !g_busy;
                const bool en = m.enabled;
                r.onToggle = [sid, en] { std::lock_guard<std::mutex> l(g_whMx); bool w = en; const bool p = g_whStage.EnabledPending(sid, w); g_whStage.StageEnabled(sid, !(p ? w : en), en); };
                r.buttons = {{L"Settings", Btn::Ghost, cli && s.whInstalled && !g_busy, [sid] {
                    g_scroll[g_page] = 0;
                    WhLoadAsync(sid); }}};
                Row(c, r);
            }
            c.x -= indent;
            c.w += indent;
            // Subtle vertical accent guide bar along the expanded category's mod list
            RECT guide{c.x + c.S(6), groupTop, c.x + c.S(9), std::max(groupTop, c.y - c.S(8))};
            if (c.Visible(guide) && guide.bottom > guide.top) {
                Round(c.dc, guide, c.S(2), col::accentDim, col::accentDim);
            }
            c.y += c.S(6);
        }
    }
}
void PageTweaks(Ctx& c, const Cache& s) {
    Section(c, L"Windows services", L"Off = AnkhCore's lighter setting. On = back to how Windows had it. Important services (updates, security, network, audio) are not listed.");
    Buttons(c, {{L"Apply recommended", Btn::Primary, true, [] {
                     if (!Confirm(L"Turn off the services marked Recommended?", L"Turn off services", false)) return;
                     RunOp(L"Apply recommended services", [] {   // SEC-3: every service that was tried counts
                         OpTally t; for (const auto& d : Services()) if (d.recommended && SvcIsOn(d)) t.Add(SetSvcOn(d, false));
                         (t.Success() ? LogOk : LogE)(L"Recommended services: " + t.Text(L"service", L"services", L"turned off"));
                         return t.Success(); }); }},
                 {L"Undo all service changes", Btn::Secondary, true, [] {
                     RunOp(L"Undo all service changes", [] { return UndoOp(L"Service changes", [](const JEntry& e) { return e.group.rfind(L"svc:", 0) == 0; }); }); }}});
    for (size_t i = 0; i < Services().size(); ++i) {
        const SvcDef& d = Services()[i];
        const int st = i < s.svc.size() ? s.svc[i] : -1;
        RowSpec r; r.title = d.title; r.desc = std::wstring(d.when) + L"  (" + d.name + L")";
        if (d.recommended) r.chips.push_back({L"Recommended", col::ok}); else r.chips.push_back({L"Your choice", col::warn});
        if (st < 0) { r.chips.push_back({L"Not on this PC", col::faint}); r.enabled = false; r.toggle = 0; }
        else { r.toggle = st; const bool on = st == 1;
               r.onToggle = [i, on] { const SvcDef& dd = Services()[i]; RunOp(std::wstring(on ? L"Service off: " : L"Service on: ") + dd.title, [i, on] { return SetSvcOn(Services()[i], !on); }); }; }
        Row(c, r);
    }
    Section(c, L"Windows performance & system tweaks", L"Categorized tweaks with state verification and full rollback via the recovery journal.");
    {
        static int g_tweakCat = 0;
        static const wchar_t* cats[6] = {L"All", L"Performance", L"Gaming", L"Privacy", L"Power & Storage", L"Windows UI"};
        std::vector<ChipOpt> catOpts;
        for (int ci = 0; ci < 6; ++ci) catOpts.emplace_back(cats[ci], g_tweakCat == ci, [ci] { g_tweakCat = ci; InvalidateRect(g_wnd, nullptr, FALSE); });
        Chips(c, L"Category", catOpts);
        const bool laptop = IsLaptop();
        for (size_t i = 0; i < Tweaks().size(); ++i) {
            const Tweak& t = Tweaks()[i];
            if (g_tweakCat > 0 && t.category != cats[g_tweakCat]) continue;
            RowSpec r; r.title = t.title; r.desc = t.desc + (t.laptopWarn && laptop ? L" This is a laptop: the battery runs down faster." : L"");
            r.chips.push_back({t.category, col::sub});
            const bool on = i < s.tweak.size() && s.tweak[i];
            r.chips.push_back({on ? L"Applied" : L"Default", on ? col::ok : col::faint});
            if (t.restart) r.chips.push_back({L"Restart needed", col::warn});
            r.toggle = on;
            r.onToggle = [i, on] {
                const Tweak& tt = Tweaks()[i];
                if (!on && tt.laptopWarn && IsLaptop() && !Confirm(tt.title + L"\n\n" + tt.desc + L"\n\nContinue on battery/laptop?", L"Apply tweak", false)) return;
                else if (!on && tt.confirm && !Confirm(tt.title + L"\n\n" + tt.desc + L"\n\nApply this Windows setting? You can revert it anytime via Undo.", L"Apply Windows tweak", false)) return;
                RunOp(tt.title + (on ? L" (undo)" : L""), [i, on] {
                    const bool ok = on ? Tweaks()[i].off() : Tweaks()[i].on();
                    // SEC-3: the tweak's own state check decides. A change that did not take is reported as failed, not as done.
                    if (ok && Tweaks()[i].isOn() != !on) { LogE(Tweaks()[i].title + L": Windows still reports the tweak as " + (on ? L"applied" : L"not applied") + L" after the change."); return false; }
                    return ok;
                }, Tweaks()[i].restart);
            };
            Row(c, r);
        }
    }
    Buttons(c, {{L"Undo all performance tweaks", Btn::Secondary, true, [] {
        RunOp(L"Undo all performance tweaks", [] { return UndoOp(L"Tweaks", [](const JEntry& e) { return e.group.rfind(L"tweak:", 0) == 0; }); }); }}});
    Section(c, L"Startup apps", L"Programs that start with Windows. Turning one off doesn't uninstall it.");
    for (size_t i = 0; i < s.startup.size(); ++i) {
        const StartupItem& it = s.startup[i];
        RowSpec r; r.title = it.name; r.desc = it.command;
        if (!it.publisher.empty()) r.chips.push_back({it.publisher, col::accent});
        r.chips.push_back({it.source, col::sub});
        r.toggle = it.enabled;
        const StartupItem copy = it;
        r.onToggle = [copy] { RunOp((copy.enabled ? L"Startup off: " : L"Startup on: ") + copy.name, [copy] { return SetStartupEnabled(copy, !copy.enabled); }); };
        Row(c, r);
    }
    if (s.startup.empty()) StatePanel(c, L"\xE7E8", L"No startup apps found", L"Nothing starts with Windows from the usual places.");
}
void PageDrivers(Ctx& c, const Cache&) {
    static std::mutex mx;
    static std::vector<DriverEntry> drivers;
    static bool loaded = false, loading = false;
    static int filter = 0;
    if (!loaded && !loading) {
        loading = true;
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            auto d = EnumSignedDrivers();
            { std::lock_guard<std::mutex> lk(mx); drivers = std::move(d); loaded = true; loading = false; }
            if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
            return 0;
        }, nullptr, 0, nullptr));
    }
    if (!loaded) {
        Para(c, L"Querying installed PnP signed drivers\x2026", c.u.f.small, col::sub);
        for (int i = 0; i < 6; ++i) { Shimmer(c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(52)}, c.S(28), GetTickCount64()); c.y += c.S(60); }
        return;
    }
    std::vector<DriverEntry> list;
    { std::lock_guard<std::mutex> lk(mx); list = drivers; }
    int thirdParty = 0, unsignedCnt = 0;
    for (const auto& d : list) {
        if (DriverFilterMatches(d, 1)) ++thirdParty;
        if (DriverFilterMatches(d, 2)) ++unsignedCnt;
    }
    Section(c, L"Driver Manager (" + std::to_wstring(list.size()) + L" signed drivers)", L"Read-only inventory of installed PnP drivers from Windows (Win32_PnPSignedDriver).");
    Chips(c, L"Filter", {{L"All (" + std::to_wstring(list.size()) + L")", filter == 0, [] { filter = 0; InvalidateRect(g_wnd, nullptr, FALSE); }},
                         {L"Third-party (" + std::to_wstring(thirdParty) + L")", filter == 1, [] { filter = 1; InvalidateRect(g_wnd, nullptr, FALSE); }},
                         {L"Unsigned (" + std::to_wstring(unsignedCnt) + L")", filter == 2, [] { filter = 2; InvalidateRect(g_wnd, nullptr, FALSE); }}});
    Buttons(c, {{L"Export driver report", Btn::Secondary, true, [list] {
                     if (TestGuard(L"Export driver report")) return;
                     const std::wstring out = DataDir() + L"\\Logs\\drivers-" + AcbRecoveryFolderStamp() + L".txt";
                     EnsureDir(DirOf(out));
                     std::string txt = "AnkhCore Driver Inventory\r\n\r\n";
                     for (const auto& d : list)
                         txt += WToUtf8(d.deviceName + L" | " + d.provider + L" | v" + d.version + L" | " + d.date + L" | " + d.infName + L"\r\n");
                     if (WriteAtomic(out, reinterpret_cast<const uint8_t*>(txt.data()), txt.size())) LogOk(L"Driver list exported to " + out);
                 }},
                {L"Refresh", Btn::Ghost, true, [] { loaded = false; InvalidateRect(g_wnd, nullptr, FALSE); }}});
    size_t shown = 0;
    for (const auto& d : list) {
        if (!DriverFilterMatches(d, filter)) continue;
        if (++shown > 80) break;
        RowSpec r; r.icon = L"\xE964"; r.title = d.deviceName;
        r.desc = (d.provider.empty() ? L"Unknown provider" : d.provider) + (d.version.empty() ? L"" : L"  \x00B7  v" + d.version) + (d.date.empty() ? L"" : L"  \x00B7  " + d.date);
        if (!d.devClass.empty()) r.chips.push_back({d.devClass, col::sub});
        if (!d.infName.empty()) r.chips.push_back({d.infName, col::accent});
        r.chips.push_back({d.isSigned ? L"Signed" : L"Unsigned", d.isSigned ? col::ok : col::warn});
        Row(c, r);
    }
    if (shown == 0) StatePanel(c, L"\xE964", L"No drivers match this filter", L"Select \"All\" to view every installed PnP driver.");
}
void DrawCleanupSection(Ctx& c) {
    Section(c, L"Cleanup", L"Scan (dry run) first to see how much each item frees. Deleted files cannot be restored unless you have a backup.");
    const auto& items = CleanItems();
    if (g_cleanSel.size() != items.size()) { g_cleanSel.clear(); for (const auto& it : items) g_cleanSel.push_back(it.defaultOn); g_cleanSize.assign(items.size(), 0); }
    static int g_cleanCat = 0;
    static const wchar_t* cats[5] = {L"All", L"Temporary", L"Caches", L"Logs", L"System"};
    // WT02-L3: "Clean selected" acts on the checked items that are shown by the category filter; checked items hidden by it are left alone and said so.
    const std::vector<size_t> scope = CleanSelectedInScope(items, g_cleanSel, cats[g_cleanCat]);
    size_t hiddenSel = 0; for (size_t i = 0; i < items.size(); ++i) hiddenSel += g_cleanSel[i] ? 1 : 0;
    hiddenSel -= scope.size();
    uint64_t total = 0; for (size_t i : scope) total += g_cleanSize[i];
    std::vector<ChipOpt> catOpts;
    for (int ci = 0; ci < 5; ++ci) catOpts.emplace_back(cats[ci], g_cleanCat == ci, [ci] { g_cleanCat = ci; InvalidateRect(g_wnd, nullptr, FALSE); });
    Chips(c, L"Category", catOpts);
    Buttons(c, {{g_cleanScanned ? L"Scan again" : L"Scan (dry run)", Btn::Secondary, true, [] {
                     RunOp(L"Cleanup scan", [] { const auto& it = CleanItems(); std::vector<uint64_t> sz;
                         for (size_t i = 0; i < it.size(); ++i) { SetOpLine(it[i].title); g_progress = ProgressOfCount(i, it.size()); sz.push_back(it[i].size()); }
                         g_cleanSize = sz; g_cleanScanned = true;
                         uint64_t t = 0; for (auto x : sz) t += x; LogI(L"Cleanup scan (dry run): " + FormatBytes(t) + L" can be freed."); return true; }); }},
                {L"Clean selected (" + std::to_wstring(scope.size()) + (scope.size() == 1 ? L" item" : L" items") + (g_cleanScanned ? L", " + FormatBytes(total) : std::wstring()) + L")", Btn::Primary, true, [] {
                     const auto& it = CleanItems();
                     const std::vector<size_t> sc = CleanSelectedInScope(it, g_cleanSel, cats[g_cleanCat]);   // recomputed at click time: what the filter shows now
                     if (sc.empty()) { LogW(L"No cleanup items selected" + std::wstring(g_cleanCat ? L" in " + std::wstring(cats[g_cleanCat]) : L"") + L"."); return; }
                     bool needRecycleConfirm = false; uint64_t tot = 0;
                     for (size_t i : sc) { needRecycleConfirm |= it[i].tag == Tag::Confirm; tot += g_cleanSize[i]; }
                     if (needRecycleConfirm && !Confirm(L"The Recycle Bin is selected and will be permanently emptied. Continue?", L"Empty Recycle Bin", true)) return;
                     std::wstring list;   // the confirmation names exactly what will be deleted
                     for (size_t k = 0; k < sc.size() && k < 12; ++k) list += L"\n\x2022 " + it[sc[k]].title;
                     if (sc.size() > 12) list += L"\n\x2026 and " + std::to_wstring(sc.size() - 12) + L" more";
                     if (!Confirm(L"Permanently delete the files of these " + std::to_wstring(sc.size()) + L" item(s)" + (g_cleanScanned ? L" (" + FormatBytes(tot) + L")" : L"") + L"?" + list + L"\n\nDeleted files cannot be restored unless you have a backup.", L"Confirm cleanup", false)) return;
                     std::vector<bool> sel(it.size(), false); for (size_t i : sc) sel[i] = true;
                     RunOp(L"Cleanup", [sel] { const auto& it2 = CleanItems(); uint64_t freed = 0;
                         size_t chosen = 0, cleaned = 0; for (size_t i = 0; i < it2.size() && i < sel.size(); ++i) chosen += sel[i];
                         for (size_t i = 0; i < it2.size(); ++i) { if (!sel[i]) continue; SetOpLine(it2[i].title); g_progress = ProgressOfCount(cleaned++, chosen);
                             const uint64_t f = it2[i].clean(); freed += f; LogI(it2[i].title + L": " + FormatBytes(f)); g_cleanSize[i] = it2[i].size(); }
                         EnterCriticalSection(&g_cs); g_cache.cleanupScanFresh = false; LeaveCriticalSection(&g_cs);
                         LogOk(L"Cleanup freed " + FormatBytes(freed) + L"."); return true; }); }}});
    Para(c, hiddenSel ? std::to_wstring(hiddenSel) + (hiddenSel == 1 ? L" checked item is" : L" checked items are") + L" in other categories and will not be cleaned while \"" + cats[g_cleanCat] + L"\" is shown. Choose All to include " + (hiddenSel == 1 ? L"it." : L"them.")
                     : std::wstring(L"Only the checked items listed below are cleaned."), c.u.f.small, hiddenSel ? col::warn : col::faint);
    for (size_t i = 0; i < items.size(); ++i) {
        if (g_cleanCat > 0 && items[i].category != cats[g_cleanCat]) continue;
        RowSpec r; r.title = items[i].title; r.desc = items[i].desc;
        r.check = g_cleanSel[i]; r.onCheck = [i] { g_cleanSel[i] = !g_cleanSel[i]; InvalidateRect(g_wnd, nullptr, FALSE); };
        r.chips.push_back({items[i].category, col::sub});
        const Tag t = items[i].tag;
        r.chips.push_back({t == Tag::Safe ? L"Safe" : t == Tag::Optional ? L"Optional" : L"Asks first", t == Tag::Safe ? col::ok : t == Tag::Optional ? col::accent : col::warn});
        if (g_cleanScanned) { r.right = FormatBytes(g_cleanSize[i]); r.rightColor = g_cleanSize[i] > (100ull << 20) ? col::accent : col::sub; }
        Row(c, r);
    }
}
// WT-03: SMART disk diagnostics. Read-only; reads the last scan (src/eng_smart.h), never a drive, while painting.
COLORREF SmartColor(SmartState s) { return s == SmartState::Good ? col::ok : s == SmartState::Warning ? col::warn : s == SmartState::Critical ? col::err : col::faint; }
std::wstring SmartTimeText(time_t t) {
    if (!t) return L"never";
    tm lt{}; localtime_s(&lt, &t); wchar_t b[48]; wcsftime(b, 48, L"%Y-%m-%d %H:%M", &lt); return b;
}
// One finding line. A finding the drive itself reported is shown as such; one that rests on a limit AnkhCore chose is marked "Advisory" (WT-03).
std::wstring SmartIssueLine(const SmartIssue& is) {
    return std::wstring(is.advisory ? L"Advisory" : is.level == SmartState::Critical ? L"Critical (reported by the drive)" : L"Warning (reported by the drive)") + L": " + is.text;
}
void DrawSmartSection(Ctx& c) {
    const SmartReport rep = Smart().Get();
    Section(c, L"Disk health (SMART)", L"Reads each drive's own SMART report with smartctl (read-only; no self-test is started and nothing on a drive is changed).");
    Buttons(c, {{rep.scanning ? L"Scanning\x2026" : rep.done ? L"Rescan disks" : L"Scan disks", Btn::Primary, !rep.scanning, [] {
                     Smart().RefreshAsync([] { if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
                     InvalidateRect(g_wnd, nullptr, FALSE);
                 }}});
    if (!rep.done) {
        Para(c, rep.scanning ? L"Reading the drives\x2026" : L"Press \"Scan disks\" to read the health of each drive. Reading SMART data needs administrator rights.", c.u.f.small, col::sub);
        return;
    }
    Para(c, L"Last scan: " + SmartTimeText(rep.scanned) + (rep.smartctlFound ? L"  \x00B7  smartctl " + (rep.smartctlVersion.empty() ? std::wstring(L"found") : rep.smartctlVersion) : L"  \x00B7  smartctl not available"), c.u.f.small, col::faint);
    if (!rep.problem.empty()) Para(c, rep.problem, c.u.f.small, col::warn);
    if (rep.disks.empty()) { StatePanel(c, L"\xEDA2", L"No drives found", L"Neither smartctl nor Windows listed a physical drive."); return; }
    for (const auto& d : rep.disks) {
        RowSpec r; r.icon = L"\xEDA2";
        r.title = d.model.empty() ? d.dev : d.model;
        std::wstring desc = (d.capacity ? FormatBytes(d.capacity) + L"  \x00B7  " : L"") + d.kind + L"  \x00B7  " + d.iface + L"  \x00B7  " + d.dev;
        if (d.hasPoh) desc += L"  \x00B7  " + std::to_wstring(d.powerOnHours) + L" h powered on";
        if (d.hasCycles) desc += L"  \x00B7  " + std::to_wstring(d.powerCycles) + L" power cycles";
        r.desc = desc;
        r.chips.push_back({SmartStateName(d.state), SmartColor(d.state)});
        if (d.hasStatus) r.chips.push_back({d.statusPassed ? L"SMART: passed" : L"SMART: FAILED", d.statusPassed ? col::ok : col::err});
        else if (!d.limited) r.chips.push_back({L"SMART: no verdict", col::faint});
        if (d.hasTemp) r.chips.push_back({std::to_wstring(d.tempC) + L" \x00B0" L"C", d.tempC >= SmartAdvisory().tempWarnC ? col::warn : col::accent});
        if (d.hasLife) r.chips.push_back({std::to_wstring(d.lifeLeftPct) + L"% life left", d.lifeLeftPct <= SmartAdvisory().lifeWarnPct ? col::warn : col::accent});
        if (d.hasSpare) r.chips.push_back({std::to_wstring(d.sparePct) + L"% spare", d.sparePct < SmartAdvisory().spareLowPct ? col::warn : col::accent});
        if (d.hasWritten) r.chips.push_back({FormatBytes(d.bytesWritten) + L" written", col::accent});
        Row(c, r);
        for (const auto& is : d.issues) Para(c, SmartIssueLine(is), c.u.f.small, SmartColor(is.level));
        if (d.issues.empty() && !d.hasData && !d.problem.empty()) Para(c, d.problem, c.u.f.small, col::faint);
        else if (d.hasData || d.limited) {
            std::wstring extra;
            auto add = [&](const std::wstring& s) { if (!extra.empty()) extra += L"  \x00B7  "; extra += s; };
            if (d.hasRealloc) add(L"Reallocated " + std::to_wstring(d.reallocated));
            if (d.hasPending) add(L"Pending " + std::to_wstring(d.pending));
            if (d.hasUncorrectable) add(L"Uncorrectable " + std::to_wstring(d.uncorrectable));
            if (d.hasCrc) add(L"Interface CRC errors " + std::to_wstring(d.crcErrors));
            if (d.hasMediaErrors) add(L"Media errors " + std::to_wstring(d.mediaErrors));
            if (d.hasUnsafe) add(L"Unsafe shutdowns " + std::to_wstring(d.unsafeShutdowns));
            if (d.hasSelfTest) add(L"Last self-test: " + (d.selfTestText.empty() ? std::wstring(d.selfTestPassed ? L"passed" : L"failed") : d.selfTestText));
            if (!d.hasTemp && d.hasData) add(L"Temperature not reported");
            if (!d.hasPoh && d.hasData) add(L"Power-on hours not reported");
            if (!extra.empty()) Para(c, extra, c.u.f.small, col::faint);
        }
    }
    Para(c, L"\"Advisory\" lines compare reported numbers with limits chosen by AnkhCore (" + std::to_wstring(SmartAdvisory().tempWarnC) + L" \x00B0" L"C, " + std::to_wstring(SmartAdvisory().lifeWarnPct) + L"% life left, " +
                std::to_wstring(SmartAdvisory().spareLowPct) + L"% spare, any sector errors). They are rules of thumb, not hardware standards. \"Good\" is not a guarantee that a drive will not fail.", c.u.f.small, col::faint);
}
StorageScanController g_storageScan;
AcbBackupListCache g_backupList;
void PageStorage(Ctx& c, const Cache&) {
    Section(c, L"Fixed drives", L"Live capacity and free space for each fixed volume on this PC.");
    if (g_sys.drives.empty()) {
        StatePanel(c, L"\xEDA2", L"Waiting for drive telemetry", L"Drive capacity updates once per second from Windows.");
    } else {
        for (const auto& d : g_sys.drives) {
            const uint64_t used = d.total > d.free ? d.total - d.free : 0;
            const double pct = d.total ? 100.0 * static_cast<double>(used) / static_cast<double>(d.total) : 0.0;
            RowSpec r; r.icon = L"\xEDA2"; r.title = std::wstring(1, d.letter) + L":\\";
            r.desc = FormatBytes(used) + L" used of " + FormatBytes(d.total) + L"  \x00B7  " + FormatBytes(d.free) + L" free";
            r.chips.push_back({std::to_wstring(static_cast<int>(pct + 0.5)) + L"% used", pct >= 90.0 ? col::warn : col::accent});
            Row(c, r);
        }
    }
    DrawSmartSection(c);
    const auto st = g_storageScan.Snapshot();
    Section(c, L"Folder breakdown", L"Scans your user temporary folder, Downloads, Local AppData (top 3 levels), and AnkhCore Data.");
    std::vector<BtnSpec> scanBtns;
    scanBtns.push_back({st.scanning ? L"Scanning..." : (st.scanned ? L"Rescan folders" : L"Scan folders"), Btn::Primary, !st.scanning, [] {
        wchar_t tmpBuf[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tmpBuf);
        const std::wstring tmp = tmpBuf;
        const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
        const std::wstring dl = UserFolder(FOLDERID_Downloads);
        const std::wstring data = DataDir();
        if (g_storageScan.StartScan(tmp, local, dl, data, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); })) {
            InvalidateRect(g_wnd, nullptr, FALSE);
        }
    }});
    if (st.scanning) {
        scanBtns.push_back({L"Cancel scan", Btn::Secondary, true, [] {
            g_storageScan.Cancel();
            InvalidateRect(g_wnd, nullptr, FALSE);
        }});
    }
    scanBtns.push_back({L"Open System Care", Btn::Secondary, true, [] { NavGo(PSystemCare); }});
    Buttons(c, scanBtns);
    if (st.scanning && !st.scanned) {
        Para(c, L"Scanning user folders in the background...", c.u.f.small, col::sub);
    } else if (st.failed) {
        Para(c, L"Scan error: " + st.error, c.u.f.small, col::err);
    } else if (!st.scanned) {
        Para(c, L"Press \"Scan folders\" to measure disk usage in your user folders (read-only).", c.u.f.small, col::sub);
    } else {
        for (const auto& it : st.items) {
            RowSpec r; r.icon = L"\xE8B7"; r.title = it.label;
            r.desc = it.path + L"  \x00B7  " + std::to_wstring(it.files) + L" file(s)";
            r.right = FormatBytes(it.bytes);
            r.rightColor = it.bytes > (512ull << 20) ? col::warn : col::accent;
            Row(c, r);
        }
    }
    Section(c, L"Cleanup", L"Scanning and cleaning now live in one place: System Care, which also checks your programs for updates.");
    Buttons(c, {{L"Open System Care", Btn::Primary, true, [] { NavGo(PSystemCare); }}});
}
// ---------------------------------------------------------------- 2.8 System Care (the page; the engine is src/eng_syscare.h, the cleaner under it is CleanItems)
CareMgr& Care() { static CareMgr m; return m; }
// 2.9.5: when the Beta trial ends, the person is not left with a bare message. One page: what happened, and the owner's links to get the full version (they open on click,
// only these links). The dialog is native (TaskDialog), so it works before the application's own window exists.
void BetaExpiredNotice(HWND owner, bool clockTampered) {
    std::wstring content = clockTampered ? L"AnkhCore Beta could not verify the system clock and has stopped.\n\n" : L"The 30-day trial of this AnkhCore Beta has ended on this device.\n\n";
    content += L"To keep using AnkhCore, download the full version from one of these links:\n";
    for (const auto& l : kLinks) content += L"\n\u2022 <a href=\"" + std::wstring(l.url) + L"\">" + l.text + L"</a>";
    TASKDIALOGCONFIG cfg{sizeof(cfg)};
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    cfg.pszWindowTitle = L"AnkhCore Beta ended";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Download the full version of AnkhCore";
    cfg.pszContent = content.c_str();
    cfg.pfCallback = [](HWND, UINT msg, WPARAM, LPARAM lp, LONG_PTR) -> HRESULT {
        if (msg == TDN_HYPERLINK_CLICKED) {
            const wchar_t* href = reinterpret_cast<const wchar_t*>(lp);
            for (const auto& l : kLinks) if (href && wcscmp(href, l.url) == 0) ShellExecuteW(nullptr, L"open", l.url, nullptr, nullptr, SW_SHOWNORMAL);   // only the owner's known links
        }
        return S_OK;
    };
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}
WuModel& Wu();   // defined with the System Care page below
void CareNotify() { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); }
std::set<std::wstring> CareIgnoreNow() { return CareParseIgnore(Cfg().Str(L"care.ignore", L"")); }
void CareSaveIgnore(const std::set<std::wstring>& s) { Cfg().SetStr(L"care.ignore", CareSerializeIgnore(s)); }
const CleanItem* CareFindClean(const std::wstring& id) { for (const auto& it : CleanItems()) if (it.id == id) return &it; return nullptr; }
std::vector<CareFinding> CareLiveScan(const std::set<std::wstring>& ignore, const std::function<void(size_t, size_t, const std::wstring&)>& prog, const std::atomic<bool>& cancel) {
    std::vector<CareFinding> out;
    const auto& items = CleanItems();
    const size_t n = items.size() + 1;
    for (size_t i = 0; i < items.size(); ++i) {   // read-only: only the size functions are called here
        if (cancel.load()) return out;
        if (CareIsIgnored(ignore, items[i].id)) continue;
        prog(i, n, items[i].title);
        uint64_t denied = 0;
        const uint64_t b = items[i].sizeAccess ? items[i].sizeAccess(denied) : items[i].size();
        out.push_back(CareClassifyCleanerItem(items[i].id, items[i].title, items[i].category, items[i].desc, items[i].tag == Tag::Safe ? 0 : items[i].tag == Tag::Optional ? 1 : 2, b, denied));
    }
    if (cancel.load()) return out;
    prog(items.size(), n, L"Broken shortcuts");
    CareShortcutEnv env;
    env.roots = {KnownFolder(FOLDERID_CommonPrograms), KnownFolder(FOLDERID_Programs), KnownFolder(FOLDERID_PublicDesktop), KnownFolder(FOLDERID_Desktop)};
    env.targetDriveIsFixedAndPresent = [](const std::wstring& target) { const std::wstring root = target.substr(0, 3); return target.size() >= 3 && target[1] == L':' && GetDriveTypeW(root.c_str()) == DRIVE_FIXED && GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES; };
    for (CareFinding& f : CareScanBrokenShortcuts(env, RemLiveFs())) if (!CareIsIgnored(ignore, f.id)) out.push_back(std::move(f));
    if (!cancel.load() && !CareIsIgnored(ignore, L"windows-updates")) {   // Windows updates (Windows Update Agent): read-only listing, kept apart from program updates
        prog(items.size(), n, L"Windows updates");
        CareFinding w; w.kind = CareKind::WindowsUpdates; w.id = L"windows-updates"; w.title = L"Windows updates"; w.category = L"Windows Update"; w.cls = CareClass::Unknown; w.irreversible = false;
        const bool started = Wu().Detect();
        for (int i = 0; i < 480 && !cancel.load(); ++i) { if (!Wu().Snapshot().detecting) break; Sleep(250); }   // up to two minutes: a search can be slow
        const WuSnapshot ws = Wu().Snapshot();
        if (ws.detecting) w.evidence = L"Windows Update is still answering; see the Windows updates section.";
        else if (!ws.detected) w.evidence = ws.status.empty() ? L"Windows updates were not checked." : ws.status;
        else { int a = 0, d = 0; for (const auto& u : ws.updates) { if (u.state == WuState::Available || u.state == WuState::Failed || u.state == WuState::Cancelled) ++a; else if (u.state == WuState::Downloaded) ++d; } w.bytes = static_cast<uint64_t>(a); w.evidence = ws.status; }
        (void)started;
        out.push_back(std::move(w));
    }
    if (cancel.load() || kTestBuild || CareIsIgnored(ignore, L"program-updates")) return out;
    {   // program updates (winget, through the UniGetUI that comes with AnkhCore): read-only listing
        prog(items.size(), n, L"Program updates (winget)");
        CareFinding u; u.kind = CareKind::ProgramUpdates; u.id = L"program-updates"; u.title = L"Program updates"; u.category = L"Updates"; u.cls = CareClass::Unknown; u.irreversible = false;
        for (int i = 0; i < 240 && !cancel.load(); ++i) { const int st = UgEnsurePrepared([] {}); if (st == 2 || st == 3 || !UgBundledAvailable()) break; Sleep(750); }   // the first use copies UniGetUI (about 130 MB)
        const AcbUniGetInstall ug = AcbDetectUniGetUI();
        if (!ug.found || !ug.portable) u.evidence = L"The UniGetUI that comes with AnkhCore is not ready, so program updates were not checked.";
        else if (!UpdUi().Check()) u.evidence = L"The update check did not complete: " + UpdUi().Snapshot().status;
        else {
            const std::vector<UpdListedPackage> vl = UpdVisible(UpdUi().Snapshot());   // 2.8.3: skipped programs are not counted
            u.bytes = vl.size();   // the count, shown as "N programs"
            if (vl.empty()) u.evidence = L"Every program that winget manages is up to date.";
            else { u.evidence = std::to_wstring(vl.size()) + L" program(s) have a newer version: "; for (size_t i = 0; i < vl.size() && i < 6; ++i) u.evidence += (i ? L", " : L"") + Utf8ToW(vl[i].name.empty() ? vl[i].id : vl[i].name); if (vl.size() > 6) u.evidence += L" 26"; }
        }
        out.push_back(std::move(u));
    }
    return out;
}
// The one entry for every scan button in the application: open System Care and start the Smart scan.
void NavOpenSystemCareScan() {
    NavGo(PSystemCare);
    if (TestGuard(L"System Care scan")) return;
    const std::set<std::wstring> ig = CareIgnoreNow();
    Care().StartScan([ig](const std::function<void(size_t, size_t, const std::wstring&)>& p, const std::atomic<bool>& cancel) { return CareLiveScan(ig, p, cancel); }, CareNotify);
}
CareBackend CareLiveBackend() {
    CareBackend be;
    be.exists = [](const CareFinding& f) { return f.kind == CareKind::BrokenShortcut ? GetFileAttributesW(f.id.c_str()) != INVALID_FILE_ATTRIBUTES : CareFindClean(f.id) != nullptr; };
    be.measure = [](const CareFinding& f) -> uint64_t {
        if (f.kind == CareKind::BrokenShortcut) { WIN32_FILE_ATTRIBUTE_DATA a{}; return GetFileAttributesExW(f.id.c_str(), GetFileExInfoStandard, &a) ? ((static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) : 0; }
        const CleanItem* it = CareFindClean(f.id); return it ? it->size() : 0; };
    be.clean = [](const CareFinding& f) -> uint64_t { const CleanItem* it = CareFindClean(f.id); return it ? it->clean() : 0; };   // the existing cleaner, with its own protections
    be.moveToRecovery = [](const CareFinding& f, const std::wstring& dir, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"move shortcut " + f.id); why = L"the test build moves nothing"; return false; }
        return RemMoveToRecovery(f.id, dir, why); };
    return be;
}
std::vector<std::wstring> CareRunValueNames(HKEY root, const wchar_t* key) {
    std::vector<std::wstring> v; HKEY h;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) return v;
    for (DWORD i = 0;; ++i) { wchar_t nm[512]; DWORD len = 512; if (RegEnumValueW(h, i, nm, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; v.emplace_back(nm, len); }
    RegCloseKey(h); return v;
}
CareDiag CareLiveDiag() {   // read-only
    CareDiag d;
    for (const auto& n : CareRunValueNames(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run")) d.startupNames.push_back(n);
    for (const auto& n : CareRunValueNames(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run")) d.startupNames.push_back(n);
    for (const auto& n : CareRunValueNames(HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Run")) d.startupNames.push_back(n);
    d.startupEntries = static_cast<int>(d.startupNames.size());
    std::map<std::wstring, uint64_t> byName;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID); if (!h) continue;
            PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
            if (K32GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) byName[pe.szExeFile] += pmc.WorkingSetSize;
            CloseHandle(h);
        }
        CloseHandle(snap);
    }
    for (const auto& kv : byName) d.topMemory.push_back({kv.first, kv.second});
    std::sort(d.topMemory.begin(), d.topMemory.end(), [](const CareDiag::Proc& a, const CareDiag::Proc& b) { return a.bytes > b.bytes; });
    if (d.topMemory.size() > 6) d.topMemory.resize(6);
    return d;
}
// ---- 2.9.5: Windows updates inside System Care (detection and selective download only; Windows installs what is downloaded). The engine is src/eng_wua.h.
WuModel& Wu() {
    static WuModel* m = [] {   // never destroyed on purpose (no static-destruction order issues); Shutdown() runs when the window is destroyed
        std::shared_ptr<WuProvider> p;
        if (kTestBuild) {      // the isolated TEST build never talks to Windows Update: a scripted provider shows the screens
            auto f = std::make_shared<WuFakeProvider>();
            f->search.ok = true;
            auto mk = [](const wchar_t* id, const wchar_t* title, const wchar_t* cat, uint64_t mb, WuState st) { WuUpdate u; u.id = id; u.title = title; u.kb = WuKbFrom({}, title); u.category = cat; u.sizeBytes = mb << 20; u.sizeKnown = true; u.state = st; u.description = L"Test update (not real)."; return u; };
            f->search.updates = {mk(L"00000000-0000-0000-0000-000000000001", L"Test cumulative update (KB5000001)", L"Security Updates", 420, WuState::Available),
                                 mk(L"00000000-0000-0000-0000-000000000002", L"Test .NET update (KB5000002)", L"Updates", 85, WuState::Available),
                                 mk(L"00000000-0000-0000-0000-000000000003", L"Test driver update", L"Drivers", 12, WuState::Downloaded),
                                 mk(L"00000000-0000-0000-0000-000000000004", L"Test older update (KB5000004)", L"Updates", 30, WuState::Installed)};
            f->download = [](const WuUpdate&, const std::function<void(int)>& prog, const std::function<bool()>& cancel) {
                WuDlOutcome o;
                for (int i = 0; i <= 20; ++i) { if (cancel()) { o.kind = WuDlKind::Aborted; return o; } prog(i * 5); Sleep(150); }
                o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o; };
            p = f;
        } else p = std::make_shared<WuaProvider>();
        return new WuModel(p, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
    }();
    return *m;
}
COLORREF WuStateColor(WuState s) {
    switch (s) { case WuState::Available: return col::warn; case WuState::Downloaded: return col::ok; case WuState::Installed: return col::faint; case WuState::Hidden: return col::faint;
                 case WuState::Queued: case WuState::Downloading: return col::accent; case WuState::Failed: return col::err; case WuState::Cancelled: return col::warn; }
    return col::sub;
}
void WuSection(Ctx& c) {
    static WuPolicy s_pol; static ULONGLONG s_polAt = 0;
    if (!s_polAt || GetTickCount64() - s_polAt > 15000) { s_pol = kTestBuild ? WuPolicy{} : WuLivePolicy(); s_polAt = GetTickCount64(); }
    const WuSnapshot s = Wu().Snapshot();
    Section(c, L"Windows updates", L"Detection and download only. AnkhCore never installs Windows updates, never restarts Windows and never changes Windows Update settings, policies or services. What you download here, Windows installs itself.");
    const std::wstring pol = WuPolicyText(s_pol);
    if (!pol.empty()) Para(c, pol + L"AnkhCore follows these settings and does not change them.", c.u.f.small, col::warn);
    uint64_t chosenBytes = 0; size_t chosen = 0;
    for (const auto& u : s.updates) if (s.selected.count(u.id)) { ++chosen; chosenBytes += u.sizeBytes; }
    const bool idle = !s.detecting && !s.downloading;
    Buttons(c, {{s.detecting ? L"Checking\x2026" : L"Check Windows updates", Btn::Primary, idle, [] { if (TestGuard(L"Check Windows updates")) return; Wu().Detect(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                {L"Select all not downloaded", Btn::Secondary, idle && s.detected, [] { Wu().SelectAll(true); }},
                {L"Download selected (" + std::to_wstring(chosen) + (chosen ? L", " + FormatBytes(chosenBytes) : std::wstring()) + L")", Btn::Primary, idle && chosen > 0, [s] {
                     if (TestGuard(L"Download Windows updates")) return;
                     std::vector<std::wstring> ids(s.selected.begin(), s.selected.end());
                     const WuStartResult r = Wu().StartDownload(ids);
                     if (!r.started) AddToast(r.why.empty() ? L"Windows updates: nothing was started." : r.why, col::warn, 0);
                     else if (!r.duplicates.empty()) AddToast(std::to_wstring(r.duplicates.size()) + L" update(s) were already queued and were not added twice.", col::warn, 0);
                     InvalidateRect(g_wnd, nullptr, FALSE); }},
                {s.cancelRequested && !s.cancelCompleted ? L"Cancelling\x2026" : L"Cancel download", Btn::Danger, s.downloading && !s.cancelRequested, [] { Wu().RequestCancel(); InvalidateRect(g_wnd, nullptr, FALSE); }}});
    if (s.downloading) {
        Para(c, s.status, c.u.f.small, col::text);
        const RECT pb{c.x + c.S(16), c.y, c.x + c.w - c.S(16), c.y + c.S(8)}; DrawProgress(c.u, c.dc, pb, s.overallPct, false, false); c.y += c.S(16);
        if (s.cancelRequested) Para(c, L"Cancel requested. Windows is stopping the running download; no further update will start. What finished before is kept.", c.u.f.small, col::warn);
    } else if (!s.status.empty()) {
        Para(c, s.status, c.u.f.small, !s.detected && s.error.kind != WuErr::None ? col::err : col::sub);
        if (s.cancelCompleted) Para(c, L"Cancelled. Windows has stopped; nothing more will start.", c.u.f.small, col::sub);
    } else Para(c, L"Press Check Windows updates (or run the Smart scan) to see what Windows offers. Nothing is downloaded until you choose it.", c.u.f.small, col::faint);
    size_t shown = 0;
    for (const WuUpdate& u : s.updates) {
        if (++shown > 60) break;
        RowSpec r; r.icon = L"\xE895"; r.title = u.title.empty() ? u.id : u.title;
        r.desc = (u.kb.empty() ? std::wstring() : u.kb + L"  \x00B7  ") + (u.category.empty() ? std::wstring() : u.category + L"  \x00B7  ") + WuSizeText(u) + (u.detail.empty() ? std::wstring() : L"  \x00B7  " + u.detail);
        r.chips.push_back({WuStateLabel(u.state), WuStateColor(u.state)});
        r.chips.push_back({L"Windows update", col::sub});
        if (u.state == WuState::Downloading || u.state == WuState::Queued) r.right = u.state == WuState::Downloading ? std::to_wstring(u.pct) + L"%" : L"waiting";
        if (WuSelectable(u.state) && !s.downloading) { const std::wstring id = u.id; r.check = s.selected.count(id) ? 1 : 0; r.onCheck = [id] { Wu().Toggle(id); }; }
        Row(c, r);
    }
    if (s.updates.size() > 60) Para(c, L"Showing the first 60 of " + std::to_wstring(s.updates.size()) + L".", c.u.f.small, col::faint);
}
COLORREF CareClassColor(CareClass c) { return c == CareClass::Cleanable ? col::ok : c == CareClass::Review ? col::warn : col::faint; }
void PageSystemCare(Ctx& c, const Cache&) {
    CareMgr& m = Care();
    const CareView v = m.Snapshot();
    const bool busy = v.phase == 1 || v.phase == 3;
    const std::set<std::wstring> ignore = CareIgnoreNow();
    Para(c, L"One workflow for every cleanup: scan (read-only)  \x2192  classify  \x2192  you review  \x2192  you approve  \x2192  clean one item at a time  \x2192  measure again. "
            L"Nothing is deleted while scanning. Space is counted only when a second measurement confirms it. No registry cleaning, and your documents, downloads, photos, passwords and browser sessions are never touched.", c.u.f.small, col::sub);

    Section(c, L"Scan");
    Buttons(c, {{v.phase == 1 ? L"Scanning\x2026" : (v.phase >= 2 ? L"Scan again" : L"Smart scan (all categories)"), Btn::Primary, !busy, [ignore] {
                     (void)ignore; NavOpenSystemCareScan(); }},
                {L"Stop", Btn::Secondary, v.phase == 1, [] { Care().Cancel(); }}});
    if (v.phase == 1) Para(c, std::to_wstring(v.done) + L" of " + std::to_wstring(v.total) + L"  \x00B7  " + v.current, c.u.f.small, col::sub);
    if (v.phase == 0) Para(c, L"Press Smart scan. The scan only measures; you choose what to clean afterwards.", c.u.f.small, col::faint);

    if (v.phase >= 2 || v.phase == 3) {
        std::vector<CareFinding> fs = v.findings;
        std::stable_sort(fs.begin(), fs.end(), [](const CareFinding& a, const CareFinding& b) { const bool ua = a.kind == CareKind::ProgramUpdates || a.kind == CareKind::WindowsUpdates, ub = b.kind == CareKind::ProgramUpdates || b.kind == CareKind::WindowsUpdates; if (ua != ub) return ua; if (a.cls != b.cls) return static_cast<int>(a.cls) < static_cast<int>(b.cls); return a.bytes > b.bytes; });
        uint64_t cleanable = 0, chosenBytes = 0; size_t nChosen = 0, nReview = 0;
        for (const auto& f : fs) { if (f.cls == CareClass::Cleanable && f.kind != CareKind::ProgramUpdates && f.kind != CareKind::WindowsUpdates) cleanable += f.bytes; if (v.chosen.count(f.id)) { chosenBytes += f.bytes; ++nChosen; if (f.cls == CareClass::Review) ++nReview; } }
        Section(c, L"Results (" + std::to_wstring(fs.size()) + L")", L"Cleanable: " + FormatBytes(cleanable) + L" found. Cleanable items start ticked; Review items never do. Items that could not be read are left alone.");
        for (const CareFinding& f : fs) {
            if (f.kind == CareKind::WindowsUpdates) {
                RowSpec r; r.icon = L"\xE895"; r.title = f.title; r.desc = f.evidence; r.chips.push_back({L"Windows Update", col::sub});
                r.chips.push_back({f.bytes ? std::to_wstring(f.bytes) + L" to download" : std::wstring(L"Nothing to download"), f.bytes ? col::warn : col::ok});
                Row(c, r); continue;
            }
            if (f.kind == CareKind::ProgramUpdates) {
                RowSpec r; r.icon = L"\xE895"; r.title = f.title; r.desc = f.evidence; r.chips.push_back({f.category, col::sub});
                const UpdUiSnapshot us = UpdUi().Snapshot(); const std::vector<UpdListedPackage> vl = UpdVisible(us);
                r.chips.push_back({vl.empty() ? L"Up to date" : std::to_wstring(vl.size()) + L" available", vl.empty() ? col::ok : col::warn});
                r.buttons = {{L"Update all", Btn::Primary, !vl.empty() && !busy && !us.busy && !us.pending.present, [] { UpdateAllPrograms(); }}, {L"Open", Btn::Secondary, true, [] { NavGo(PAppsUpdates); }}};
                Row(c, r); continue;
            }
            RowSpec r; r.icon = f.kind == CareKind::BrokenShortcut ? L"\xE71B" : L"\xE74D"; r.title = f.title; r.desc = f.evidence;
            r.chips.push_back({CareClassLabel(f.cls), CareClassColor(f.cls)});
            r.chips.push_back({f.category, col::sub});
            r.chips.push_back({f.irreversible ? L"Deleted files cannot be restored" : L"Undoable", f.irreversible ? col::faint : col::accent});
            r.right = FormatBytes(f.bytes); r.rightColor = f.bytes > (100ull << 20) ? col::accent : col::sub;
            const bool selectable = f.cls == CareClass::Cleanable || f.cls == CareClass::Review;
            const std::wstring id = f.id; const bool review = f.cls == CareClass::Review;
            if (selectable && !busy) { r.check = v.chosen.count(id) ? 1 : 0; r.onCheck = [id, review] { Care().Toggle(id, review); InvalidateRect(g_wnd, nullptr, FALSE); }; }
            if (!busy) r.buttons = {{L"Ignore", Btn::Ghost, true, [id] { std::set<std::wstring> ig = CareIgnoreNow(); ig.insert(Lower(id)); CareSaveIgnore(ig); Care().Forget(id); InvalidateRect(g_wnd, nullptr, FALSE); }}};
            Row(c, r);
        }
        Buttons(c, {{L"Clean selected (" + std::to_wstring(nChosen) + L", " + FormatBytes(chosenBytes) + L")", Btn::Danger, nChosen > 0 && !busy, [] { CareCleanSelected(); }}});
        if (v.phase == 3) Para(c, std::to_wstring(v.done) + L" of " + std::to_wstring(v.total) + L"  \x00B7  " + v.current, c.u.f.small, col::sub);
        (void)nReview;
    }
    WuSection(c);
    if (v.phase == 4 || (v.phase == 2 && !v.report.items.empty())) {
        Section(c, L"Cleanup result", v.report.summary);
        for (const CareResult& r : v.report.items) {
            RowSpec row; row.title = r.f.title; row.desc = r.detail;
            row.chips.push_back({CareStateLabel(r.state), r.state == CareState::Cleaned ? col::ok : r.state == CareState::Failed ? col::err : col::faint});
            if (r.state == CareState::Cleaned) row.right = FormatBytes(r.verified);
            Row(c, row);
        }
        Buttons(c, {{L"Done", Btn::Secondary, !busy, [] { Care().Dismiss(); }}});
    }
    if (!v.lastRecoveryDir.empty() && !busy) {
        Para(c, L"Broken shortcuts moved to: " + v.lastRecoveryDir, c.u.f.small, col::sub);
        Buttons(c, {{L"Undo: put shortcuts back", Btn::Secondary, true, [] {
            if (TestGuard(L"Undo shortcut cleanup")) return;
            std::vector<std::wstring> problems; const int n = RemUndo(Care().LastRecoveryDir(), problems);
            std::wstring t = std::to_wstring(n) + L" shortcut(s) put back."; for (const auto& p : problems) t += L"  " + p;
            Care().SetUndoText(t, problems.empty()); LogI(L"System Care undo: " + t); InvalidateRect(g_wnd, nullptr, FALSE); }}});
    }
    if (!v.undoText.empty()) Para(c, v.undoText, c.u.f.small, col::sub);

    Section(c, L"Ignored items (" + std::to_wstring(ignore.size()) + L")", L"Items you chose to ignore are skipped by every scan and can never enter a cleanup. Remove one to scan it again.");
    if (ignore.empty()) Para(c, L"Nothing is ignored.", c.u.f.small, col::faint);
    for (const std::wstring& id : ignore) {
        RowSpec r; r.icon = L"\xE711"; const CleanItem* ci = nullptr; for (const auto& it : CleanItems()) if (Lower(it.id) == id) ci = &it;
        r.title = ci ? ci->title : id; r.desc = ci ? ci->desc : id;
        r.buttons = {{L"Stop ignoring", Btn::Secondary, true, [id] { std::set<std::wstring> ig = CareIgnoreNow(); ig.erase(id); CareSaveIgnore(ig); InvalidateRect(g_wnd, nullptr, FALSE); }}};
        Row(c, r);
    }

    Section(c, L"Application leftovers", L"Leftovers of an uninstalled program are never guessed here. Apps Manager removes a program through its own uninstaller, verifies it is gone, and only then offers what that program left behind (moved to a recovery folder, undoable).");
    Buttons(c, {{L"Open Uninstall Manager", Btn::Secondary, true, [] { NavGo(PAppsUninstall); }}});

    Section(c, L"Startup & performance (read-only)", L"Information only: nothing here changes your system.");
    {
        static CareDiag s_diag; static ULONGLONG s_diagAt = 0;
        if (!s_diagAt || GetTickCount64() - s_diagAt > 15000) { if (!kTestBuild) s_diag = CareLiveDiag(); s_diagAt = GetTickCount64(); }
        MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms);
        { RowSpec r; r.icon = L"\xE950"; r.title = L"Memory"; r.desc = FormatBytes(ms.ullTotalPhys - ms.ullAvailPhys) + L" used of " + FormatBytes(ms.ullTotalPhys); r.chips.push_back({std::to_wstring(ms.dwMemoryLoad) + L"% in use", ms.dwMemoryLoad >= 85 ? col::warn : col::accent}); Row(c, r); }
        for (const auto& d : g_sys.drives) {
            RowSpec r; r.icon = L"\xEDA2"; r.title = std::wstring(1, d.letter) + L":\\"; r.desc = FormatBytes(d.free) + L" free of " + FormatBytes(d.total);
            const double freePct = d.total ? 100.0 * static_cast<double>(d.free) / static_cast<double>(d.total) : 100.0;
            r.chips.push_back({std::to_wstring(static_cast<int>(freePct + 0.5)) + L"% free", freePct < 10 ? col::warn : col::ok}); Row(c, r);
        }
        { RowSpec r; r.icon = L"\xE7E8"; r.title = L"Programs that start with Windows"; r.desc = std::to_wstring(s_diag.startupEntries) + L" entr" + (s_diag.startupEntries == 1 ? L"y" : L"ies") + L" in the Run keys (registry). Open Startup & Background to manage AnkhCore's own startup.";
          r.chips.push_back({std::to_wstring(s_diag.startupEntries), s_diag.startupEntries > 12 ? col::warn : col::sub}); r.buttons = {{L"Open", Btn::Secondary, true, [] { NavGo(PStartup); }}}; Row(c, r); }
        for (const auto& p : s_diag.topMemory) { RowSpec r; r.icon = L"\xE9D9"; r.title = p.name; r.right = FormatBytes(p.bytes); r.desc = L"Working set (memory in use right now)."; Row(c, r); }
    }
}
// ---------------------------------------------------------------- 2.8 Process Manager (the page; the engine is src/eng_procmgr.h on the PROC1 inventory)
ProcMgrModel g_pm;
DWORD g_pmSel = 0; DWORD g_pmPrio = 0; int g_pmAff = 0, g_pmEco = -1, g_pmSort = 0; std::wstring g_pmMsg;
std::vector<ProcRule> PmRules() { return ProcParseRules(Cfg().Str(L"proc.rules", L"")); }
void PmSaveRules(const std::vector<ProcRule>& r) { Cfg().SetStr(L"proc.rules", ProcSerializeRules(r)); Cfg().Save(); }
void PmNotify() { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); }
// 0 keep, 1 all cores, 2 half, 3 a quarter, 4 one core: the lowest cores of the mask the process may use
uint64_t PmAffMask(int kind, uint64_t sysMask) {
    if (kind <= 0 || sysMask == 0) return 0;
    if (kind == 1) return sysMask;
    int total = 0; for (int i = 0; i < 64; ++i) if (sysMask & (1ull << i)) ++total;
    const int want = kind == 2 ? std::max(1, total / 2) : kind == 3 ? std::max(1, total / 4) : 1;
    uint64_t m = 0; int got = 0; for (int i = 0; i < 64 && got < want; ++i) if (sysMask & (1ull << i)) { m |= (1ull << i); ++got; }
    return m;
}
// The rules are applied by a small watcher while AnkhCore runs: every few seconds, only to ordinary applications, each process once per rule.
void StartProcRuleWatcher() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        ProcHistory hist; std::set<std::wstring> done; const ProcBackend be = ProcLiveBackend();
        while (!g_quitting) {
            for (int i = 0; i < 10 && !g_quitting; ++i) Sleep(500);
            const std::vector<ProcRule> rules = PmRules();
            if (rules.empty() || g_quitting) { done.clear(); continue; }
            const ProcContext ctx = ProcContextLive();
            const std::vector<ProcEntry> ents = ProcBuildInventory(ProcCollectLive(), ctx).entries;
            std::set<std::wstring> alive; for (const auto& e : ents) alive.insert(std::to_wstring(e.raw.pid));
            for (auto it = done.begin(); it != done.end();) { const std::wstring pid = it->substr(0, it->find(L'|')); it = alive.count(pid) ? std::next(it) : done.erase(it); }
            std::vector<std::wstring> log;
            ProcApplyRules(rules, ents, ctx.selfPid, be, hist, done, &log);
            for (const auto& l : log) LogI(L"Process rule: " + l);
        }
    }).detach();
}
std::wstring PmRuleText(const ProcRule& r) {
    std::wstring t;
    if (r.prio) t += std::wstring(L"priority ") + ProcPrioName(r.prio);
    if (r.affinity) { int n = 0; for (int i = 0; i < 64; ++i) if (r.affinity & (1ull << i)) ++n; t += (t.empty() ? L"" : L"  \x00B7  ") + std::to_wstring(n) + L" CPU core(s)"; }
    if (r.eco >= 0) t += (t.empty() ? L"" : L"  \x00B7  ") + std::wstring(r.eco ? L"efficiency mode on" : L"efficiency mode off");
    return t.empty() ? L"no change" : t;
}
void PageProcMgr(Ctx& c, const Cache&) {
    Para(c, L"Every process on this PC. Critical, system, security and unidentifiable processes (and AnkhCore itself) are shown but never changed; the real-time priority is never used. "
            L"A change is checked by reading the process back, and the state from before your first change is remembered so you can restore it.", c.u.f.small, col::sub);
    const ULONGLONG age = GetTickCount64() - g_pm.LastTick();
    if (!g_pm.Busy() && (g_pm.LastTick() == 0 || age > 5000) && !kTestBuild) g_pm.RefreshAsync(PmNotify);
    const std::vector<ProcMgrRow> rows = g_pm.Rows();
    const ProcBackend be = ProcLiveBackend();

    const ProcMgrRow* sel = nullptr; for (const auto& r : rows) if (r.e.raw.pid == g_pmSel) sel = &r;
    if (sel) {
        const ProcEntry e = sel->e;
        const ProcState st = be.read(e.raw.pid);
        Section(c, L"Manage " + e.raw.name + L"  (PID " + std::to_wstring(e.raw.pid) + L")", std::wstring(ProcProtectionLabel(e.prot)) + L": " + e.reason);
        { int cores = 0; for (int i = 0; i < 64; ++i) if (st.affinity & (1ull << i)) ++cores;
          Para(c, st.ok ? L"Now: priority " + std::wstring(ProcPrioName(st.prio)) + L"  \x00B7  " + std::to_wstring(cores) + L" CPU core(s)  \x00B7  efficiency mode " + (st.eco < 0 ? L"unknown" : st.eco ? L"on" : L"off") : std::wstring(L"The process could not be read (it ended, or Windows denies access)."), c.u.f.small, col::sub); }
        const bool can = e.prot != ProcProtection::Protected && st.ok;
        Chips(c, L"Priority", {{L"Keep", g_pmPrio == 0, [] { g_pmPrio = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"Idle", g_pmPrio == kProcPrioIdle, [] { g_pmPrio = kProcPrioIdle; InvalidateRect(g_wnd, nullptr, FALSE); }},
                               {L"Below normal", g_pmPrio == kProcPrioBelowNormal, [] { g_pmPrio = kProcPrioBelowNormal; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"Normal", g_pmPrio == kProcPrioNormal, [] { g_pmPrio = kProcPrioNormal; InvalidateRect(g_wnd, nullptr, FALSE); }},
                               {L"Above normal", g_pmPrio == kProcPrioAboveNormal, [] { g_pmPrio = kProcPrioAboveNormal; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"High", g_pmPrio == kProcPrioHigh, [] { g_pmPrio = kProcPrioHigh; InvalidateRect(g_wnd, nullptr, FALSE); }}});
        Chips(c, L"CPU cores", {{L"Keep", g_pmAff == 0, [] { g_pmAff = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"All", g_pmAff == 1, [] { g_pmAff = 1; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"Half", g_pmAff == 2, [] { g_pmAff = 2; InvalidateRect(g_wnd, nullptr, FALSE); }},
                                {L"A quarter", g_pmAff == 3, [] { g_pmAff = 3; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"One core", g_pmAff == 4, [] { g_pmAff = 4; InvalidateRect(g_wnd, nullptr, FALSE); }}});
        Chips(c, L"Efficiency mode", {{L"Keep", g_pmEco < 0, [] { g_pmEco = -1; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"On (less power, lower speed)", g_pmEco == 1, [] { g_pmEco = 1; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"Off", g_pmEco == 0, [] { g_pmEco = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}});
        Buttons(c, {{L"Apply", Btn::Primary, can && (g_pmPrio || g_pmAff || g_pmEco >= 0), [e, st] {
                        if (TestGuard(L"Change a process")) return;
                        std::vector<std::pair<ProcAction, uint64_t>> todo; bool needConfirm = false;
                        if (g_pmPrio) todo.push_back({ProcAction::SetPriority, g_pmPrio});
                        if (g_pmAff) todo.push_back({ProcAction::SetAffinity, PmAffMask(g_pmAff, st.systemAffinity)});
                        if (g_pmEco >= 0) todo.push_back({ProcAction::SetEfficiency, static_cast<uint64_t>(g_pmEco)});
                        for (const auto& t : todo) { const ProcDecision d = ProcMayChange(e, t.first, t.second, GetCurrentProcessId()); if (!d.allowed) { g_pmMsg = e.raw.name + L": " + d.reason; InvalidateRect(g_wnd, nullptr, FALSE); return; } needConfirm |= d.needsConfirmation; }
                        if (needConfirm && !Confirm(L"Change " + e.raw.name + L" (PID " + std::to_wstring(e.raw.pid) + L")?\n\nThe priority or the CPU cores change how the application performs. Your previous settings are remembered and can be restored.", L"Process Manager", false)) return;
                        ProcHistory h = g_pm.History(); std::wstring msg;
                        for (const auto& t : todo) { const ProcChange r = ProcApplyChange(e, t.first, t.second, true, GetCurrentProcessId(), ProcLiveBackend(), h); msg += (t.first == ProcAction::SetPriority ? L"Priority: " : t.first == ProcAction::SetAffinity ? L"CPU cores: " : L"Efficiency mode: ") + r.detail + L"  "; LogI(L"Process Manager: " + e.raw.name + L" " + msg); }
                        g_pm.SetHistory(h); g_pmMsg = msg; g_pm.RefreshAsync(PmNotify); InvalidateRect(g_wnd, nullptr, FALSE); }},
                    {L"Restore previous", Btn::Secondary, can && g_pm.History().count(e.raw.pid) > 0, [e] {
                        if (TestGuard(L"Restore a process")) return;
                        ProcHistory h = g_pm.History(); const ProcChange r = ProcRestore(e, GetCurrentProcessId(), ProcLiveBackend(), h); g_pm.SetHistory(h); g_pmMsg = r.detail; LogI(L"Process Manager: " + e.raw.name + L": " + r.detail); g_pm.RefreshAsync(PmNotify); InvalidateRect(g_wnd, nullptr, FALSE); }},
                    {L"Always apply to " + e.raw.name, Btn::Secondary, e.prot == ProcProtection::Adjustable && (g_pmPrio || g_pmAff || g_pmEco >= 0), [e, st] {
                        std::vector<ProcRule> rules = PmRules(); const std::wstring exe = Lower(e.raw.name);
                        ProcRule nr; nr.exe = exe.size() > 4 && exe.compare(exe.size() - 4, 4, L".exe") == 0 ? exe.substr(0, exe.size() - 4) : exe; nr.prio = g_pmPrio; nr.affinity = PmAffMask(g_pmAff, st.systemAffinity); nr.eco = g_pmEco;
                        if (!ProcRuleExeOk(nr.exe)) { g_pmMsg = L"This executable name cannot be used in a rule."; InvalidateRect(g_wnd, nullptr, FALSE); return; }
                        rules.erase(std::remove_if(rules.begin(), rules.end(), [&](const ProcRule& x) { return x.exe == nr.exe; }), rules.end()); rules.push_back(nr); PmSaveRules(rules);
                        g_pmMsg = L"Rule saved: " + e.raw.name + L" always gets " + PmRuleText(nr) + L" while AnkhCore runs."; LogI(g_pmMsg); InvalidateRect(g_wnd, nullptr, FALSE); }},
                    {L"End process", Btn::Danger, e.prot == ProcProtection::Adjustable && st.ok, [e] {
                        if (TestGuard(L"End a process")) return;
                        if (!Confirm(L"End " + e.raw.name + L" (PID " + std::to_wstring(e.raw.pid) + L")?\n\nAnything it has not saved is lost. This cannot be undone.", L"End process", true)) return;
                        ProcHistory h = g_pm.History(); const ProcChange r = ProcApplyChange(e, ProcAction::Terminate, 0, true, GetCurrentProcessId(), ProcLiveBackend(), h);
                        g_pmMsg = e.raw.name + L": " + r.detail; LogI(L"Process Manager: " + g_pmMsg); g_pmSel = 0; g_pm.RefreshAsync(PmNotify); InvalidateRect(g_wnd, nullptr, FALSE); }},
                    {L"Close", Btn::Ghost, true, [] { g_pmSel = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}});
    }
    if (!g_pmMsg.empty()) Para(c, g_pmMsg, c.u.f.small, col::accent);

    const std::vector<ProcRule> rules = PmRules();
    Section(c, L"Rules (" + std::to_wstring(rules.size()) + L")", L"Applied automatically while AnkhCore runs (checked every few seconds), only to ordinary applications, each process once. Choose a process, set what you want, then \"Always apply to ...\".");
    if (rules.empty()) Para(c, L"No rules yet.", c.u.f.small, col::faint);
    for (size_t i = 0; i < rules.size(); ++i) {
        const ProcRule ru = rules[i];
        RowSpec r; r.icon = L"\xE9D9"; r.title = ru.exe; r.desc = PmRuleText(ru); r.chips.push_back({ru.enabled ? L"On" : L"Off", ru.enabled ? col::ok : col::faint});
        r.buttons = {{ru.enabled ? L"Turn off" : L"Turn on", Btn::Secondary, true, [i] { auto v = PmRules(); if (i < v.size()) { v[i].enabled = !v[i].enabled; PmSaveRules(v); } InvalidateRect(g_wnd, nullptr, FALSE); }},
                     {L"Remove", Btn::Ghost, true, [i] { auto v = PmRules(); if (i < v.size()) { v.erase(v.begin() + i); PmSaveRules(v); } InvalidateRect(g_wnd, nullptr, FALSE); }}};
        Row(c, r);
    }

    int prot = 0, caut = 0, adj = 0; for (const auto& r : rows) (r.e.prot == ProcProtection::Protected ? prot : r.e.prot == ProcProtection::Caution ? caut : adj)++;
    Section(c, L"Processes (" + std::to_wstring(rows.size()) + L")", std::to_wstring(adj) + L" ordinary, " + std::to_wstring(caut) + L" need care, " + std::to_wstring(prot) + L" protected. The 60 biggest are listed.");
    Chips(c, L"Sort", {{L"Memory", g_pmSort == 0, [] { g_pmSort = 0; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"CPU", g_pmSort == 1, [] { g_pmSort = 1; InvalidateRect(g_wnd, nullptr, FALSE); }}, {L"Name", g_pmSort == 2, [] { g_pmSort = 2; InvalidateRect(g_wnd, nullptr, FALSE); }}});
    Buttons(c, {{g_pm.Busy() ? L"Refreshing" + std::wstring(L"\x2026") : std::wstring(L"Refresh"), Btn::Secondary, !g_pm.Busy(), [] { g_pm.RefreshAsync(PmNotify); }}});
    std::vector<ProcMgrRow> sorted = rows;
    if (g_pmSort == 1) std::stable_sort(sorted.begin(), sorted.end(), [](const ProcMgrRow& a, const ProcMgrRow& b) { return a.cpu > b.cpu; });
    else if (g_pmSort == 2) std::stable_sort(sorted.begin(), sorted.end(), [](const ProcMgrRow& a, const ProcMgrRow& b) { return Lower(a.e.raw.name) < Lower(b.e.raw.name); });
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        const ProcMgrRow& pr = sorted[i]; const DWORD pid = pr.e.raw.pid;
        RowSpec r; r.icon = L"\xE950"; r.title = pr.e.raw.name; r.desc = std::wstring(ProcClassLabel(pr.e.cls)) + L"  \x00B7  PID " + std::to_wstring(pid) + L"  \x00B7  " + std::to_wstring(pr.e.raw.threads) + L" threads";
        r.chips.push_back({ProcProtectionLabel(pr.e.prot), pr.e.prot == ProcProtection::Adjustable ? col::ok : pr.e.prot == ProcProtection::Caution ? col::warn : col::faint});
        if (pr.e.raw.priorityClass) r.chips.push_back({ProcPrioName(pr.e.raw.priorityClass), col::sub});
        if (g_pm.History().count(pid)) r.chips.push_back({L"Changed by you", col::accent});
        r.right = FormatBytes(pr.e.raw.workingSet) + (pr.cpu >= 0 ? L"  \x00B7  " + std::to_wstring(static_cast<int>(pr.cpu + 0.5)) + L"% CPU" : std::wstring());
        r.buttons = {{pr.e.prot == ProcProtection::Protected ? L"Why protected" : L"Manage", Btn::Secondary, true, [pid, e = pr.e] { g_pmSel = pid; g_pmPrio = 0; g_pmAff = 0; g_pmEco = -1; g_pmMsg = e.prot == ProcProtection::Protected ? e.reason : std::wstring(); InvalidateRect(g_wnd, nullptr, FALSE); }}};
        Row(c, r);
    }
}
// ---------------------------------------------------------------- 2.8 PowerToys (external, installed by AnkhCore Setup; the engine is src/eng_powertoys.h)
bool g_ptDirty = false; std::wstring g_ptMsg;
// 2.8.7: the FancyZones editor is opened by the FancyZones module of the RUNNING PowerToys (the module prepares the editor's data). Starting
// PowerToys.FancyZonesEditor.exe on its own showed "Editor data parsing error. The input does not contain any JSON tokens". PowerToys is started when it is not running.
void PtOpenFancyZonesEditorAsync(const std::wstring& dir) {
    std::thread([dir] {
        const wchar_t* kEvt = L"Local\\FancyZones-ToggleEditorEvent-1e174338-06a3-472b-874d-073b21c62f14";
        auto signal = [&] { HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, kEvt); if (!h) return false; const BOOL ok = SetEvent(h); CloseHandle(h); return ok != FALSE; };
        if (signal()) { LogI(L"PowerToys: the FancyZones editor was asked to open."); return; }
        if (!ProcessRunning(L"PowerToys.exe")) {
            LogI(L"PowerToys is not running; starting it so FancyZones can open its editor.");
            ShellExecuteW(nullptr, L"open", (dir + L"\\PowerToys.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        }
        for (int i = 0; i < 48; ++i) { Sleep(250); if (signal()) { LogI(L"PowerToys: the FancyZones editor was asked to open."); return; } }
        LogW(L"PowerToys: the FancyZones editor could not be opened. FancyZones must be switched on (the toggle on its row) and PowerToys must be running.");
    }).detach();
}
void PagePowerToys(Ctx& c, const Cache&) {
    static PtInstall pt; static ULONGLONG ptAt = 0; static std::string json; static ULONGLONG jsonAt = 0;
    if (!ptAt || GetTickCount64() - ptAt > 3000) { pt = PtDetect(); ptAt = GetTickCount64(); }
    if (!jsonAt || GetTickCount64() - jsonAt > 2000) { json = FileExists(PtSettingsPath()) ? ReadText(PtSettingsPath()) : std::string(); jsonAt = GetTickCount64(); }
    const bool running = pt.installed && ProcessRunning(L"PowerToys.exe");
    Para(c, L"PowerToys is Microsoft's collection of Windows utilities (MIT licence). It is a separate program that AnkhCore Setup installs for you; this page switches its utilities on or off and opens them. AnkhCore changes only the on/off switches in PowerToys' own settings file (a backup is kept) and restarts PowerToys to apply them.", c.u.f.small, col::sub);
    Section(c, L"PowerToys");
    {
        RowSpec r; r.icon = L"\xE8B0"; r.title = L"Microsoft PowerToys";
        r.desc = pt.installed ? L"Version " + (pt.version.empty() ? std::wstring(L"unknown") : pt.version) + L"  \x00B7  " + pt.dir
                              : L"Not installed on this PC. Re-run AnkhCore Setup to install it from the package, or install it now with winget.";
        r.chips.push_back({pt.installed ? L"Installed" : L"Not installed", pt.installed ? col::ok : col::warn});
        if (pt.installed) r.chips.push_back({running ? L"Running" : L"Not running", running ? col::ok : col::faint});
        Row(c, r);
    }
    if (pt.installed) {
        Buttons(c, {{running ? L"Open PowerToys settings" : L"Start PowerToys", Btn::Primary, true, [] { if (TestGuard(L"Start PowerToys")) return; const PtInstall p = PtDetect(); if (p.installed) ShellExecuteW(nullptr, L"open", p.exe.c_str(), nullptr, p.dir.c_str(), SW_SHOWNORMAL); }},
                    {L"Restart PowerToys", Btn::Secondary, true, [] { std::wstring why; const PtInstall p = PtDetect(); const bool ok = PtRestart(p, why); g_ptDirty = !ok ? g_ptDirty : false; g_ptMsg = ok ? L"PowerToys was restarted; your switches are applied." : why; LogI(L"PowerToys: " + g_ptMsg); InvalidateRect(g_wnd, nullptr, FALSE); }}});
    } else {
        Buttons(c, {{L"Install PowerToys with winget", Btn::Primary, true, [] {
            if (TestGuard(L"Install PowerToys")) return;
            if (!Confirm(L"Install Microsoft PowerToys with winget?\n\nWindows may ask for permission. It downloads about 300 MB.", L"PowerToys", false)) return;
            RunOp(L"Install PowerToys", [] { return Run(L"winget.exe", L"install --id Microsoft.PowerToys -e --silent --accept-package-agreements --accept-source-agreements", true, 20u * 60u * 1000u) == 0; }); }}});
    }
    if (g_ptDirty) Para(c, L"Restart PowerToys to apply the switches you changed.", c.u.f.small, col::warn);
    if (!g_ptMsg.empty()) Para(c, g_ptMsg, c.u.f.small, col::accent);
    if (pt.installed) {
        if (json.empty()) Para(c, L"PowerToys has not created its settings yet. Start PowerToys once, then come back: the switches appear here.", c.u.f.small, col::warn);
        Section(c, L"Utilities", L"Switch a utility on or off. The shortcut shown is PowerToys' default; change it in PowerToys settings.");
        for (const PtModule& m : PtModules()) {
            const int on = json.empty() ? -1 : PtReadEnabled(json, WToUtf8(m.key));
            RowSpec r; r.icon = PtIconOf(m.key); r.title = m.title; r.desc = m.desc;
            r.chips.push_back({m.hotkey, col::sub});
            if (on < 0 && !json.empty()) r.chips.push_back({L"Not in this version", col::faint});
            if (on >= 0) { r.toggle = on; const std::wstring key = m.key; const bool now = on == 1;
                r.onToggle = [key, now] { if (TestGuard(L"Change a PowerToys setting")) return; std::wstring why; const bool ok = PtWriteEnabled(key, !now, why);
                                          if (ok) { g_ptDirty = true; g_ptMsg = key + (now ? L" switched off." : L" switched on."); jsonAt = 0; } else g_ptMsg = why; LogI(L"PowerToys: " + g_ptMsg); InvalidateRect(g_wnd, nullptr, FALSE); }; }
            if (m.editorExe && on == 1 && FileExists(pt.dir + L"\\" + m.editorExe)) { const std::wstring ex = pt.dir + L"\\" + m.editorExe, dir = pt.dir; r.buttons = {{L"Open editor", Btn::Secondary, true, [dir] { if (TestGuard(L"Open the FancyZones editor")) return; PtOpenFancyZonesEditorAsync(dir); }}}; }
            Row(c, r);
        }
    }
}
void PageOptimize(Ctx& c, const Cache&) {
    Section(c, L"System tools", L"Driver inventory and storage analysis sub-pages.");
    {
        auto link = [&](int page) {
            RowSpec r; r.icon = kPageIcon[page]; r.title = kPageName[page]; r.desc = kPageSub[page];
            r.buttons.push_back({L"Open", Btn::Secondary, true, [page] { NavGo(page); }});
            Row(c, r);
        };
        link(PSystemCare);
        link(PProcMgr);
        link(PDrivers);
        link(PStorage);
    }
    Section(c, L"Scan & clean", L"One scan for cleanup, broken shortcuts and program updates, with a review before anything is changed.");
    Buttons(c, {{L"Open System Care", Btn::Primary, true, [] { NavGo(PSystemCare); }}});
    Section(c, L"Tools");
    for (const auto& t : Tools()) {
        RowSpec r; r.title = t.title; r.desc = t.desc;
        if (t.restart) r.chips.push_back({L"Restart needed", col::warn});
        const ToolDef* tp = &t;
        r.buttons = {{L"Run", Btn::Secondary, true, [tp] { if (tp->needsConfirm && !Confirm(tp->title + L"\n\n" + tp->desc + L"\n\nContinue?", L"Run tool", false)) return;
                                                        RunOp(tp->title, [tp] { return tp->run(OpLines()); }, tp->restart); }}};
        Row(c, r);
    }
    Section(c, L"Repair Windows", L"Progress shows at the bottom of the window; the full output is in Activity.");
    for (const auto& t : RepairTools()) {
        RowSpec r; r.title = t.title; r.desc = t.desc; r.icon = L"\xE90F";
        if (t.restart) r.chips.push_back({L"Restart needed", col::warn});
        const ToolDef* tp = &t;
        r.buttons = {{L"Run", Btn::Secondary, true, [tp] { if (tp->needsConfirm && !Confirm(tp->title + L"\n\n" + tp->desc + L"\n\nContinue?", L"Run tool", false)) return;
                                                        RunOp(tp->title, [tp] { return tp->run(OpLines()); }, tp->restart); }}};
        Row(c, r);
    }
}
void ApplyMonitorConfig();
void NavOpenSystemCareScan();   // defined with System Care; every scan button in the application leads there
// 2.8: the Windows Glass defaults reach an EXISTING settings file once. Only values that are still the OLD defaults are changed (the old theme, the old
// tint 68, the old background picture 27 %, the old solid sidebar); the Classic theme and anything else the person chose stay. Marked so it never repeats.
void ApplyGlassDefaultsOnce() {
    if (Cfg().Bool(L"appearance.glassDefaults28", false)) return;
    if (Cfg().Str(L"appearance.theme", L"") == L"neutral-glass") Cfg().SetStr(L"appearance.theme", L"windows-glass");
    if (Cfg().Has(L"appearance.tint") && Cfg().Int(L"appearance.tint", 0, 0, 100) == 68) Cfg().SetInt(L"appearance.tint", 55);
    if (Cfg().Has(L"appearance.bgOpacity") && Cfg().Int(L"appearance.bgOpacity", -1, -100000, 100000) == 27) Cfg().SetInt(L"appearance.bgOpacity", 0);
    if (Cfg().Has(L"sidebar.background") && Cfg().Int(L"sidebar.background", 1, 0, 2) == 0) Cfg().SetInt(L"sidebar.background", 2);
    if (Cfg().Has(L"sidebar.tint") && Cfg().Int(L"sidebar.tint", 0, 0, 100) == 70) Cfg().SetInt(L"sidebar.tint", 50);
    Cfg().SetBool(L"appearance.glassDefaults28", true);
    Cfg().Save();
}
// ---------------------------------------------------------------- UPD2-B: software updates through the AnkhCore-owned UniGetUI session
// Every command goes through UgSession (private pipe, validated before each update, never the user's own UniGetUI session).
// Only winget packages are offered: it is the one manager whose listings were verified (see the UPD1 audit, section 6b).
UpdUiModel& UpdUi() {   // created on first use; creating it starts nothing
    static UpdUiModel m(
        [](std::wstring& why) -> std::shared_ptr<UgSession> {
            const AcbUniGetInstall ug = AcbDetectUniGetUI();
            if (!ug.found) { why = L"UniGetUI was not found."; return nullptr; }
            if (!ug.portable) { why = L"Only a portable UniGetUI is used for updates, so that AnkhCore's session never shares the settings of an installed copy."; return nullptr; }
            return std::make_shared<UgSession>(UgMakeConfig(ug, AcbDefaultRoots()), UgMakeLivePlatform(ug));
        },
        [] { if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0); },   // the UI thread repaints; workers never touch the UI directly
        [](bool ok, const std::wstring& t) { (ok ? LogOk : LogE)(t); },
        AcbDefaultRoots());
    return m;
}
// 2.8.3: the skip lists (persistent) and the list the person actually sees. "Always skip" keeps the id; "Skip this version" keeps id + offered version.
UpdSkipSet UpdSkipNow() {
    UpdSkipSet s;
    s.always = UpdParseSkipText(WToUtf8(Cfg().Str(L"upd.skipAlways", L"")));
    s.versions = UpdParseSkipText(WToUtf8(Cfg().Str(L"upd.skipVersions", L"")));
    return s;
}
void UpdSkipSave(const UpdSkipSet& s) {
    Cfg().SetStr(L"upd.skipAlways", Utf8ToW(UpdSerializeSkipText(s.always)));
    Cfg().SetStr(L"upd.skipVersions", Utf8ToW(UpdSerializeSkipText(s.versions)));
    Cfg().Save();
}
std::vector<UpdListedPackage> UpdVisible(const UpdUiSnapshot& us) { return UpdFilterSkipped(us.list, UpdSkipNow()); }
// 2.8: Update all. One update after another through the same audited path as a single update (recovery point first, verified afterwards).
// A package that cannot be confirmed is reported and the next one still runs; an update that timed out (state unknown) stops the run.
UpdBatchResult UpdateBatchNow(const std::vector<UpdListedPackage>& list) {   // worker thread: Update all, honouring Cancel between programs
    UpdUi().BeginBatch();
    const UpdBatchResult br = UpdRunBatch(list, [](const UpdListedPackage& p) { return UpdUi().Run(p); },
        [] { return UpdUi().CancelRequested(); }, [] { return UpdUi().Snapshot().pending.present; },
        [&list](size_t i, const UpdListedPackage& p) { SetOpLine(Utf8ToW(p.name.empty() ? p.id : p.name) + L"  (" + std::to_wstring(i + 1) + L" of " + std::to_wstring(list.size()) + L")"); g_progress = ProgressOfCount(i, list.size()); });
    UpdUi().EndBatch();
    LogI(UpdBatchText(br));
    UpdUi().Check();   // the list is read again so it shows what is still outdated
    return br;
}
std::wstring UpdNamesText(const std::vector<UpdListedPackage>& list) {
    std::wstring names;
    for (size_t i = 0; i < list.size() && i < 12; ++i) names += L"\n  \x2022 " + Utf8ToW(list[i].name.empty() ? list[i].id : list[i].name) + L"   " + Utf8ToW(list[i].version) + L" \x2192 " + Utf8ToW(list[i].newVersion);
    if (list.size() > 12) names += L"\n  \x2026 and " + std::to_wstring(list.size() - 12) + L" more";
    return names;
}
// 2.8.6: the list of installed programs (winget export). Worker thread. True only when the saved file really holds entries.
bool ProgListSaveNow(const std::wstring& plDir) {
    EnsureDir(plDir);
    const std::wstring path = plDir + L"\\" + ProgListFileName(AcbRecoveryFolderStamp());
    const DWORD rc = Run(L"winget.exe", ProgExportArgs(path), true, 5u * 60u * 1000u);
    const int n = ProgListCount(ReadText(path));
    if (n <= 0) { DeleteFileW(path.c_str()); LogW(L"Programs list: winget could not export the list (exit " + std::to_wstring(rc) + L"); nothing was saved."); return false; }
    LogOk(L"Programs list saved: " + std::to_wstring(n) + L" program(s) in " + path);
    return true;
}
void UpdateAllPrograms() {
    const std::vector<UpdListedPackage> list = UpdOnlyKnownVersions(UpdVisible(UpdUi().Snapshot()));   // 2.8.3: skipped programs are not updated; 2.8.6: nor those whose installed version is Unknown (they have "Update anyway")
    if (list.empty() || TestGuard(L"Update all programs")) return;
    if (!Confirm(L"Update " + std::to_wstring(list.size()) + L" program(s), one after another?" + UpdNamesText(list) +
                 L"\n\nEach update first makes a verified recovery point and is checked afterwards. A program that cannot be confirmed is reported and the next one still runs. "
                 L"Some programs close or restart while they update. You can cancel: the program that is updating finishes first, the rest do not start.", L"Update all", false)) return;
    RunOp(L"Update all", [list] { SetOpLine(L"Saving the list of installed programs first\x2026"); ProgListSaveNow(ProgListDir(AcbDefaultBackupDir())); const UpdBatchResult br = UpdateBatchNow(list); return br.bad == 0 && !br.cancelled; }, false, false, true);
}
// ---- 2.8.3: System Care action bar
CareBackend CareLiveBackend();
void CareCleanSelected() {
    if (TestGuard(L"System Care cleanup")) return;
    const CareView v = Care().Snapshot();
    if (v.phase != 2 && v.phase != 4) return;   // never during a scan or another cleanup
    const std::set<std::wstring> ignore = CareIgnoreNow();
    std::wstring list; size_t k = 0, review = 0, shortcuts = 0;
    for (const auto& f : v.findings) if (v.chosen.count(f.id) && (f.cls == CareClass::Cleanable || f.cls == CareClass::Review)) { if (k++ < 12) list += L"\n  \x2022 " + f.title; if (f.cls == CareClass::Review) ++review; if (f.kind == CareKind::BrokenShortcut) ++shortcuts; }
    if (!k) return;
    if (k > 12) list += L"\n  \x2026 and " + std::to_wstring(k - 12) + L" more";
    std::wstring text = L"Clean " + std::to_wstring(k) + L" selected item(s)?" + list;
    if (review) text += L"\n\n" + std::to_wstring(review) + L" of them are 'Review required': ticking them confirmed them. Some are permanent (for example the Recycle Bin).";
    text += L"\n\nDeleted files cannot be restored." + std::wstring(shortcuts ? L" Broken shortcuts are moved to a recovery folder and can be put back from this page." : L"") + L" Each item is checked again right before it is cleaned, and the result is measured afterwards.";
    if (!Confirm(text, L"System Care", true)) return;
    Care().StartClean(ignore, CareLiveBackend(), [](const CareFinding& f) { return CleanRecoveryDirFor(f.kind == CareKind::BrokenShortcut ? f.id : DataDir()); }, CareNotify);
}
// Update all / Fix all: program updates (not skipped) and broken shortcuts only. The person's own ticks, and every cache/junk item, are left alone.
void CareUpdateAndFixAll() {
    if (TestGuard(L"Update all / Fix all")) return;
    const CareView v = Care().Snapshot();
    const UpdUiSnapshot us = UpdUi().Snapshot();
    const std::vector<UpdListedPackage> ups = UpdVisible(us);
    const CareBarState bs = CareBarAssess(v, ups.size(), us.busy, us.pending.present, g_busy.load(), CareIgnoreNow());
    if (!bs.canFixAll) return;
    const bool doUpdates = !ups.empty() && !us.pending.present;
    std::wstring text = L"Do these now?";
    if (doUpdates) text += L"\n\n" + std::to_wstring(ups.size()) + L" program update(s), one after another (a verified recovery point first, checked afterwards):" + UpdNamesText(ups);
    if (!bs.fixIds.empty()) text += L"\n\n" + std::to_wstring(bs.fixIds.size()) + L" broken shortcut(s) moved to a recovery folder (you can put them back from this page).";
    text += L"\n\nNothing else is cleaned. Use Clean selected for cache and junk items.";
    if (!Confirm(text, L"Update all / Fix all", false)) return;
    const std::set<std::wstring> fix(bs.fixIds.begin(), bs.fixIds.end());
    const std::set<std::wstring> ignore = CareIgnoreNow();
    auto startFix = [fix, ignore] {
        if (!fix.empty()) Care().StartClean(ignore, CareLiveBackend(), [](const CareFinding& f) { return CleanRecoveryDirFor(f.kind == CareKind::BrokenShortcut ? f.id : DataDir()); }, CareNotify, &fix);
    };
    if (doUpdates) RunOp(L"Update all / Fix all", [ups, startFix] {
        const UpdBatchResult br = UpdateBatchNow(ups);
        if (!br.cancelled) startFix();   // a cancelled run does not go on to repair
        return br.bad == 0 && !br.cancelled; }, false, false, true);
    else startFix();
}
// The pinned bar: always visible above the scrolling results (the page area starts below it, see ContentRect).
int PinnedBarH() { return g_page == PSystemCare ? g_ui.S(66) : 0; }
void DrawCareBar(Dc dc, const RECT& bar) {
    const CareView v = Care().Snapshot();
    const UpdUiSnapshot us = UpdUi().Snapshot();
    const CareBarState bs = CareBarAssess(v, UpdVisible(us).size(), us.busy, us.pending.present, g_busy.load(), CareIgnoreNow());
    if (g_material == MatClear) FillGlass(dc, bar, col::bg, TintAlpha());   // 2.11: the System Care bar has the page's own colour on Clear glass (it was a black strip)
    else FillGlass(dc, bar, col::overlay, std::max(.86f, TintAlpha()));
    Line(dc, bar.left, bar.bottom - 1, bar.right, bar.bottom - 1, Mix(col::side, col::border, .65));
    Ctx c{dc, g_ui, bar.left + g_ui.S(32), bar.top + g_ui.S(10), bar.right - bar.left - g_ui.S(64), bar, false};
    int x = c.x;
    auto btn = [&](const std::wstring& label, Btn kind, bool en, std::function<void()> fn) {
        const int bw = std::max(g_ui.S(120), TextWidth(dc, label, g_ui.f.btn) + g_ui.S(32));
        const RECT br{x, bar.top + g_ui.S(10), x + bw, bar.top + g_ui.S(10) + g_ui.S(36)};
        DrawButton(g_ui, dc, br, label, kind, en, c.Hover(br));
        if (en && fn) c.Hit(br, std::move(fn));
        x += bw + g_ui.S(10);
    };
    btn(L"Update All / Fix All", Btn::Primary, bs.canFixAll, [] { CareUpdateAndFixAll(); });
    btn(L"Clean selected (" + std::to_wstring(bs.chosen) + L")", Btn::Danger, bs.canClean, [] { CareCleanSelected(); });
    if (us.busy && us.batchActive) btn(us.cancelRequested ? L"Cancelling\x2026" : L"Cancel updates", Btn::Secondary, !us.cancelRequested, [] { UpdUi().RequestCancel(); InvalidateRect(g_wnd, nullptr, FALSE); });
    // status: what is selected, what is running (the reason a button is off is in its tooltip-free line below)
    std::wstring st; int pct = -1;
    if (v.phase == 1) { st = L"Scanning  " + std::to_wstring(v.done) + L" of " + std::to_wstring(v.total) + (v.current.empty() ? std::wstring() : L"  \x00B7  " + v.current); if (v.total) pct = static_cast<int>(v.done * 100 / v.total); }
    else if (v.phase == 3) { st = L"Cleaning  " + std::to_wstring(v.done) + L" of " + std::to_wstring(v.total) + (v.current.empty() ? std::wstring() : L"  \x00B7  " + v.current); if (v.total) pct = static_cast<int>(v.done * 100 / v.total); }
    else if (g_busy) { st = g_opName; const std::wstring l = OpLine(); if (!l.empty()) st += L"  \x00B7  " + l; pct = g_progress; }
    else if (bs.scanned) st = std::to_wstring(bs.chosen) + L" selected  \x00B7  " + FormatBytes(bs.chosenBytes) + (bs.updates ? L"  \x00B7  " + std::to_wstring(bs.updates) + L" program update(s)" : std::wstring()) + (bs.fixIds.empty() ? std::wstring() : L"  \x00B7  " + std::to_wstring(bs.fixIds.size()) + L" shortcut(s) to fix");
    else st = L"Run Smart scan to enable the actions.";
    const bool busyNow = v.phase == 1 || v.phase == 3 || g_busy;
    RECT tr{x + g_ui.S(6), bar.top + g_ui.S(8), bar.right - g_ui.S(32), bar.top + g_ui.S(36)};
    if (tr.right > tr.left + g_ui.S(60)) Text(dc, st, tr, g_ui.f.small, busyNow ? col::text : col::sub, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_RIGHT);
    if (tr.right > tr.left + g_ui.S(60) && !busyNow) {
        const std::wstring why = !bs.canFixAll ? bs.fixWhy : std::wstring(); const std::wstring why2 = !bs.canClean ? bs.cleanWhy : std::wstring();
        const std::wstring both = (!why2.empty() ? L"Clean selected: " + why2 : std::wstring()) + (!why.empty() && !why2.empty() ? L"   " : L"") + (!why.empty() ? L"Fix all: " + why : std::wstring());
        if (!both.empty() && bs.scanned) Text(dc, both, RECT{tr.left, bar.top + g_ui.S(34), tr.right, bar.top + g_ui.S(54)}, g_ui.f.tiny, col::faint, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_RIGHT);
    }
    if (busyNow) { RECT pb{bar.left + g_ui.S(32), bar.bottom - g_ui.S(10), bar.right - g_ui.S(32), bar.bottom - g_ui.S(5)}; DrawProgress(g_ui, dc, pb, pct, true, false); }
}
void PageBackup(Ctx& c, const Cache&) {
    const AcbRoots roots = AcbDefaultRoots();
    const std::wstring bakDir = AcbDefaultBackupDir();
    Section(c, L"Create backup (.acbackup)", L"Writes a single portable ZIP archive with a SHA-256 manifest. Contains no machine names, credentials, Windows system files or executables.");
    const int bKind = Cfg().Int(L"backup.kind", 0, 0, 1);
    Chips(c, L"Backup mode", {{L"Settings & monitoring", bKind == 0, [] { Cfg().SetInt(L"backup.kind", 0); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                              {L"Full backup", bKind == 1, [] { Cfg().SetInt(L"backup.kind", 1); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }}});
    Para(c, L"Backup folder: " + bakDir, c.u.f.small, col::sub);
    Buttons(c, {{L"Create .acbackup now", Btn::Primary, true, [roots, bakDir, bKind] {
                     if (TestGuard(L"Create .acbackup")) return;
                     RunOp(L"Create .acbackup", [roots, bakDir, bKind] {
                         EnsureDir(bakDir);
                         const std::wstring path = bakDir + L"\\AnkhCore-" + AcbRecoveryFolderStamp() + L".acbackup";
                         AcbCheck chk{}; std::wstring err;
                         const bool ok = AcbCreateBackup(roots, path, bKind == 1 ? "full" : "settings", {}, chk, err);
                         g_backupList.Invalidate();
                         g_backupList.RefreshAsync(bakDir, 12, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
                         if (!ok) {
                             LogE(L"Backup failed: " + err);
                             return false;
                         }
                         LogOk(L"Created verified backup: " + path + L" (" + std::to_wstring(chk.manifest.files.size()) + L" files)");
                         return true;
                     }); }},
                {L"Refresh list", Btn::Secondary, true, [bakDir] {
                     g_backupList.Invalidate();
                     g_backupList.RefreshAsync(bakDir, 12, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
                     InvalidateRect(g_wnd, nullptr, FALSE);
                 }},
                {L"Open backup folder", Btn::Secondary, true, [bakDir] {
                     if (TestGuard(L"Open backup folder")) return;
                     EnsureDir(bakDir);
                     ShellExecuteW(nullptr, L"open", bakDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}});

    {   // 2.8.6: the programs list
        const std::wstring plDir = ProgListDir(bakDir);
        static std::vector<ProgListInfo> s_lists; static ULONGLONG s_listsTick = 0; static bool s_listsDirty = true;
        if (s_listsDirty || GetTickCount64() - s_listsTick > 8000) { s_lists = ProgListFiles(plDir); s_listsTick = GetTickCount64(); s_listsDirty = false; }
        Section(c, L"Installed programs list", L"Saves the names of the programs installed through winget, so a fresh Windows can get them back with one click. It is the list only: the programs' own data is not in it. A list is also saved before every Update all.");
        Buttons(c, {{L"Save the programs list now", Btn::Primary, true, [plDir] {
                         if (TestGuard(L"Save the programs list")) return;
                         RunOp(L"Save the programs list", [plDir] { const bool ok = ProgListSaveNow(plDir); s_listsDirty = true; return ok; }); }},
                    {L"Open folder", Btn::Secondary, true, [plDir] {
                         if (TestGuard(L"Open the programs list folder")) return;
                         EnsureDir(plDir); ShellExecuteW(nullptr, L"open", plDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}});
        if (s_lists.empty()) Para(c, L"No programs list saved yet.", c.u.f.small, col::sub);
        for (const ProgListInfo& pl : s_lists) {
            RowSpec r; r.icon = L"\xE71D"; r.title = pl.name; r.desc = std::to_wstring(pl.count) + L" program(s)  \x00B7  " + FormatBytes(pl.bytes);
            const std::wstring path = pl.path; const int cnt = pl.count;
            r.buttons = {{L"Reinstall these programs", Btn::Secondary, true, [path, cnt] {
                              if (TestGuard(L"Reinstall the programs")) return;
                              if (!Confirm(L"Install the programs of this list that are missing (" + std::to_wstring(cnt) + L" in the list)?\n\nwinget installs each one silently; programs that are already installed are skipped and programs winget cannot find are ignored. Their own settings and data are not restored. This can take a long time.", L"Reinstall programs", false)) return;
                              RunOp(L"Reinstall programs from the list", [path] { const DWORD rc = Run(L"winget.exe", ProgImportArgs(path), true, 60u * 60u * 1000u); if (rc != 0) LogW(L"winget import ended with exit code " + std::to_wstring(rc) + L" (some programs may not have been installed)."); return rc == 0; }); }}};
            Row(c, r);
        }
    }

    Section(c, L"Automatic backup before major changes", L"Writes a recovery point under Recovery\\<timestamp> before applying an icon theme, cursor scheme, Windows theme or large tweak group.");
    const int autoMode = Cfg().Int(L"backup.autoBeforeChanges", 0, 0, 2);
    Chips(c, L"Before major changes", {{L"Always back up", autoMode == 0, [] { Cfg().SetInt(L"backup.autoBeforeChanges", 0); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                                       {L"Ask first", autoMode == 1, [] { Cfg().SetInt(L"backup.autoBeforeChanges", 1); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                                       {L"Never", autoMode == 2, [] { Cfg().SetInt(L"backup.autoBeforeChanges", 2); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }}});

    Section(c, L"What a backup contains", L"A \"Settings & monitoring\" backup holds the first two rows. A \"Full backup\" holds every row marked Included or Partly. Anything marked Not included is not in the file; the reason is shown, and the backup file repeats it in its notes.");
    for (const AcbCoverageRow& cv : AcbCoverage()) {
        RowSpec r; r.icon = L"\xE81C"; r.title = Utf8ToW(cv.title); r.desc = Utf8ToW(cv.limitation);
        r.chips.push_back({cv.support == AcbSupport::Included ? L"Included" : cv.support == AcbSupport::Partial ? L"Partly" : L"Not included", cv.support == AcbSupport::Included ? col::ok : cv.support == AcbSupport::Partial ? col::warn : col::faint});
        Row(c, r);
    }

    Section(c, L"Available backups", L"Lists cached archive metadata (newest 12). Full SHA-256 verification runs before any file is restored; restoring creates a recovery point first and rolls back automatically on failure.");
    const auto bState = g_backupList.Snapshot();
    if (!bState.valid && !bState.loading) {
        g_backupList.RefreshAsync(bakDir, 12, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
    }
    const auto& archives = *bState.items;
    if (bState.loading && archives.empty()) {
        StatePanel(c, L"\xE81C", L"Loading backup archives...", L"Reading backup metadata in the background.");
    } else if (archives.empty()) {
        StatePanel(c, L"\xE81C", L"No .acbackup archives in the backup folder yet", L"Create a backup above or place an .acbackup file into the backup folder.");
    } else {
        for (size_t i = 0; i < archives.size() && i < 12; ++i) {
            const auto& a = archives[i];
            RowSpec r; r.icon = L"\xE81C"; r.title = a.fileName;
            if (a.check.ok) {
                r.desc = L"Created " + acbdetail::Show(a.check.manifest.created) + L"  \x00B7  AnkhCore " +
                         acbdetail::Show(a.check.manifest.appVersion) + L"  \x00B7  " +
                         std::to_wstring(a.check.manifest.files.size()) + L" file(s), " + FormatBytes(a.fileBytes);
                r.chips.push_back({L"Manifest valid", col::ok});
                const std::wstring path = a.fullPath;
                r.buttons = {{L"Restore settings", Btn::Secondary, true, [roots, bakDir, path] {
                                  if (TestGuard(L"Restore backup settings")) return;
                                  if (!Confirm(L"Restore settings and monitoring totals from:\n" + NameOf(path) + L"\n\nA recovery point will be created first.", L"Restore settings", false)) return;
                                  RunOp(L"Restore backup", [roots, bakDir, path] {
                                      AcbRestoreOptions opts; opts.settingsOnly = true;
                                      const AcbRestoreReport rep = AcbRestoreBackup(roots, path, opts);
                                      g_backupList.Invalidate();
                                      g_backupList.RefreshAsync(bakDir, 12, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
                                      if (!rep.ok) { LogE(L"Restore failed: " + rep.error); return false; }
                                      Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass")); ApplyMonitorConfig(); SbSetOption(L"", 0); MiniSetOption(L"", 0); UpdateMotion(); ApplyMaterial(g_wnd);
                                      LogOk(L"Restored " + std::to_wstring(rep.filesRestored) + L" file(s); recovery point at " + rep.recoveryDir);
                                      return true;
                                  }); }}};
            } else {
                r.desc = a.check.error;
                r.chips.push_back({L"Invalid", col::err});
            }
            Row(c, r);
        }
    }

    Section(c, L"Application migration (UniGetUI)", L"A package bundle lists package references to reinstall through UniGetUI; it is never an offline copy of program installers.");
    const AcbUniGetInstall ug = AcbDetectUniGetUI();
    {
        RowSpec r; r.icon = L"\xE71D"; r.title = L"UniGetUI package backend";
        r.desc = ug.found ? L"Found at " + ug.dir + L" (uniget.exe beside UniGetUI.exe)."
                          : L"Not detected in default locations. Settings & monitoring backups work without UniGetUI.";
        r.chips.push_back({ug.found ? (ug.portable ? L"Portable found" : L"Installed") : L"Not detected", ug.found ? col::ok : col::faint});
        Row(c, r);
    }

    {   // 2.7: the updater has its own page in Apps Manager
        RowSpec r; r.icon = kPageIcon[PAppsUpdates]; r.title = kPageName[PAppsUpdates]; r.desc = kPageSub[PAppsUpdates];
        r.buttons.push_back({L"Open", Btn::Secondary, true, [] { NavGo(PAppsUpdates); }});
        Row(c, r);
    }
}
void PageAppsUpdates(Ctx& c, const Cache&) {
    // 2.7: AnkhCore brings its own UniGetUI (installed by AnkhCore Setup); the first visit prepares a private working copy for this user in the background.
    const int prep = UgEnsurePrepared([] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });
    const AcbUniGetInstall ug = AcbDetectUniGetUI();
    {   // 2.8.7: Windows Update (Windows' own updater; AnkhCore starts it and shows its state, it never downloads or installs Windows updates itself)
        const std::wstring wuKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update";
        const std::wstring last = RegStr(HKEY_LOCAL_MACHINE, wuKey + L"\\Results\\Detect", L"LastSuccessTime");
        const std::wstring lastInstall = RegStr(HKEY_LOCAL_MACHINE, wuKey + L"\\Results\\Install", L"LastSuccessTime");
        const bool reboot = RegKeyExists(HKEY_LOCAL_MACHINE, wuKey + L"\\RebootRequired") || RegKeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending");
        Section(c, L"Windows Update", L"Windows' own updater. AnkhCore starts the check and shows what Windows reports; Windows downloads and installs its updates itself.");
        RowSpec r; r.icon = L"\xE895"; r.title = L"Windows Update";
        r.desc = L"Last successful check: " + (last.empty() ? std::wstring(L"unknown") : last + L" (UTC)") + L"   \x00B7   last install: " + (lastInstall.empty() ? std::wstring(L"unknown") : lastInstall + L" (UTC)");
        if (reboot) r.chips.push_back({L"Restart required", col::warn});
        r.buttons = {{L"Check for updates", Btn::Primary, true, [] {
                          if (TestGuard(L"Check for Windows updates")) return;
                          Run(L"UsoClient.exe", L"StartInteractiveScan", false, 0);
                          ShellExecuteW(nullptr, L"open", L"ms-settings:windowsupdate", nullptr, nullptr, SW_SHOWNORMAL);
                          LogI(L"Windows Update: a check was started; the result is shown in Windows Update."); }},
                     {L"Open Windows Update", Btn::Secondary, true, [] { if (TestGuard(L"Open Windows Update")) return; ShellExecuteW(nullptr, L"open", L"ms-settings:windowsupdate", nullptr, nullptr, SW_SHOWNORMAL); }},
                     {L"Choose and download (System Care)", Btn::Secondary, true, [] { NavGo(PSystemCare); }}};
        Row(c, r);
    }
    Section(c, L"Software updates (UniGetUI)", L"Runs in AnkhCore's own private UniGetUI session, never in your own UniGetUI window. Before each update a verified recovery point is made; the result is checked against the installed version. Application data is not backed up yet, so a rollback can restore the program version at most, not its data.");
    {
        const UpdUiSnapshot us = UpdUi().Snapshot();
        const std::wstring& status = us.status; const std::wstring& last = us.lastReport; const std::vector<UpdListedPackage> list = UpdVisible(us); const bool checked = us.checked;
        size_t unknownVer = 0; const size_t batchCount = UpdOnlyKnownVersions(list, &unknownVer).size();
        Buttons(c, {{L"Check for updates (winget)", Btn::Secondary, ug.found && ug.portable, [] { RunOp(L"Check for updates", [] { return UpdUi().Check(); }); }},
                    {L"Update all (" + std::to_wstring(batchCount) + L")", Btn::Primary, ug.found && ug.portable && batchCount > 0 && !us.busy && !us.pending.present, [] { UpdateAllPrograms(); }}});
        if (unknownVer) Para(c, std::to_wstring(unknownVer) + L" program(s) below show the installed version \"Unknown\" (installed outside winget). They are not in Update all; each has its own \"Update anyway\".", c.u.f.small, col::warn);
        if (!(us.busy && us.batchActive)) Buttons(c, {{L"Cancel (only while Update all runs)", Btn::Danger, false, [] {}}});   // 2.8.6: always visible, so it can be found; it works only during a batch
        if (us.busy && us.updatingNow) Buttons(c, {{L"Cancel this update" + (us.updatingName.empty() ? std::wstring() : L" (" + us.updatingName + L")"), Btn::Danger, true, [] {   // 2.8.7: also a single update can be cancelled
            if (!Confirm(L"Cancel the update that is running?\n\nUniGetUI is asked to stop it. If the program's installer is already running it may leave the program half-updated; the installed version is checked afterwards and the result is shown. The rest of an Update all will not start.", L"Cancel update", true)) return;
            std::thread([] { std::wstring note; const bool ok = UpdUi().CancelRunningUpdate(note); if (ok) LogOk(L"Updates: " + note); else LogW(L"Updates: " + note); }).detach(); }}});
        if (us.busy && us.batchActive) {   // 2.8.3: Cancel stops BETWEEN packages; the one that is running always finishes (an installer cannot be interrupted safely)
            Buttons(c, {{us.cancelRequested ? L"Cancelling after the current program\x2026" : L"Cancel (after the current program)", Btn::Danger, !us.cancelRequested, [] { UpdUi().RequestCancel(); InvalidateRect(g_wnd, nullptr, FALSE); }}});
            Para(c, us.cancelRequested ? L"Cancel requested: the program that is updating now will finish and be checked; the remaining ones will not start."
                                       : L"Cancel stops the remaining programs from starting. A program that is already updating cannot be interrupted safely, so it finishes first.", c.u.f.small, col::sub);
        } else if (us.busy) Para(c, L"An update is running. Cancel asks UniGetUI to stop it; otherwise it finishes and is checked.", c.u.f.small, col::sub);
        if (prep == 1) Para(c, L"Preparing the UniGetUI that comes with AnkhCore (first use only, about 130 MB), please wait.", c.u.f.small, col::warn);
        else if (prep == 3) Para(c, UgPrepState().why, c.u.f.small, col::err);
        else if (!ug.found || !ug.portable) Para(c, UgBundledAvailable() ? L"The UniGetUI that comes with AnkhCore is not ready yet." : L"The UniGetUI that comes with AnkhCore was not found in the program folder. Reinstall AnkhCore with AnkhCore Setup (it installs it).", c.u.f.small, col::sub);
        else if (!status.empty()) Para(c, status, c.u.f.small, checked ? col::sub : col::warn);
        if (us.pending.present && !us.busy) {   // an update that never finished blocks the next one: say so and offer the way out
            RowSpec pr; pr.icon = L"\xE7BA"; pr.title = us.pending.unresolved ? L"An update timed out" : L"An update did not finish";
            pr.desc = us.pending.text;
            pr.chips.push_back({us.pending.unresolved ? L"Final state unknown" : L"Interrupted", col::warn});
            pr.buttons = {{us.pending.unresolved ? L"Check and release" : L"Release", Btn::Secondary, true, [] {
                               RunOp(L"Release the unfinished update", [] { return UpdUi().Acknowledge(false); }); }}};
            if (us.pending.unresolved)
                pr.buttons.push_back({L"Release without checking", Btn::Ghost, true, [] {
                    if (TestGuard(L"Release the unfinished update")) return;
                    if (!Confirm(L"Release the unfinished update WITHOUT checking whether it is still running?\n\nOnly do this if you have verified in UniGetUI that the operation has ended. "
                                 L"If it is still running, a second update of the same package could start. The record is kept in the update history and the recovery point is not touched.", L"Release", true)) return;
                    RunOp(L"Release the unfinished update (not checked)", [] { return UpdUi().Acknowledge(true); }); }});
            Row(c, pr);
        }
        if (!last.empty()) Para(c, L"Last update: " + last, c.u.f.small, col::sub);
        for (const UpdUiLine& ln : us.lines) {
            RowSpec dr; dr.title = ln.title; dr.desc = ln.detail;
            if (ln.state) dr.chips.push_back({ln.state == 1 ? L"OK" : L"Problem", ln.state == 1 ? col::ok : col::err});
            Row(c, dr);
        }
        for (size_t i = 0; i < list.size() && i < 25; ++i) {
            const UpdListedPackage lp = list[i];
            RowSpec r; r.icon = L"\xE71D"; r.title = Utf8ToW(lp.name.empty() ? lp.id : lp.name);
            r.desc = Utf8ToW(lp.id) + L"  \x00B7  " + Utf8ToW(lp.version) + L" -> " + Utf8ToW(lp.newVersion);
            const bool verUnknown = UpdListedVersionUnknown(lp);
            if (verUnknown) r.chips.push_back({L"Installed version unknown", col::warn});
            r.buttons = {{verUnknown ? L"Update anyway" : L"Update", Btn::Secondary, true, [lp, verUnknown] {
                              if (TestGuard(L"Update " + Utf8ToW(lp.id))) return;
                              if (!Confirm(verUnknown ? UpdUiConfirmUnknownText(lp) : UpdUiConfirmText(lp), verUnknown ? L"Update anyway" : L"Update", verUnknown)) return;
                              RunOp(L"Update " + Utf8ToW(lp.id), [lp, verUnknown] { return UpdUi().Run(lp, nullptr, verUnknown); }, false, false, true); }},
                         {L"Skip this version", Btn::Ghost, !us.busy, [lp] {
                              UpdSkipSet ss = UpdSkipNow();
                              if (UpdSkipVersion(ss, lp.id, lp.newVersion)) { UpdSkipSave(ss); LogI(L"Updates: " + Utf8ToW(lp.id) + L" " + Utf8ToW(lp.newVersion) + L" will not be offered again; a newer version will appear."); }
                              InvalidateRect(g_wnd, nullptr, FALSE); }},
                         {L"Always skip", Btn::Ghost, !us.busy, [lp] {
                              if (!Confirm(L"Never offer updates for " + Utf8ToW(lp.name.empty() ? lp.id : lp.name) + L" again?\n\nThis program stays out of the update list and out of Update all until you restore it in the Skipped section below.", L"Always skip", false)) return;
                              UpdSkipSet ss = UpdSkipNow();
                              if (UpdSkipAlways(ss, lp.id)) { UpdSkipSave(ss); LogI(L"Updates: " + Utf8ToW(lp.id) + L" is always skipped."); }
                              InvalidateRect(g_wnd, nullptr, FALSE); }}};
            Row(c, r);
        }
        {   // 2.8.3: what is skipped, and how to bring it back
            const UpdSkipSet ss = UpdSkipNow();
            if (!ss.always.empty() || !ss.versions.empty()) {
                Section(c, L"Skipped (" + std::to_wstring(ss.always.size() + ss.versions.size()) + L")", L"\"Always skip\" keeps a program out until you restore it. \"Skip this version\" hides only that version; a newer one is offered again by itself.");
                for (const std::string& id : ss.always) {
                    RowSpec sr; sr.icon = L"\xE71D"; sr.title = Utf8ToW(id); sr.desc = L"Always skipped"; sr.chips.push_back({L"Always", col::warn});
                    sr.buttons = {{L"Restore", Btn::Secondary, true, [id] { UpdSkipSet t = UpdSkipNow(); t.always.erase(id); UpdSkipSave(t); InvalidateRect(g_wnd, nullptr, FALSE); }}};
                    Row(c, sr);
                }
                for (const std::string& key : ss.versions) {
                    const size_t eq = key.find('=');
                    RowSpec sr; sr.icon = L"\xE71D"; sr.title = Utf8ToW(eq == std::string::npos ? key : key.substr(0, eq)); sr.desc = L"Version " + Utf8ToW(eq == std::string::npos ? std::string() : key.substr(eq + 1)) + L" is skipped; a newer version will be offered.";
                    sr.chips.push_back({L"This version", col::sub});
                    sr.buttons = {{L"Restore", Btn::Secondary, true, [key] { UpdSkipSet t = UpdSkipNow(); t.versions.erase(key); UpdSkipSave(t); InvalidateRect(g_wnd, nullptr, FALSE); }}};
                    Row(c, sr);
                }
            }
        }
    }
}
// ---------------------------------------------------------------- Recovery History (FT-RH)
// The page used to list one row per journal entry (a cursor scheme = 18-20 rows with one title), scan the Recovery folder on every
// paint (manifests, sizes) and copy the whole journal with every Snapshot(). Now: operations are the rows (src/journal_view.h),
// shown 25 at a time with "Show more" and expandable to their entries, and the Recovery folder is listed once per page visit on a
// worker thread. Undo, per entry and per operation, works on the same journal data as before; nothing is removed.
std::mutex g_recMx; std::shared_ptr<const std::vector<AcbRecoveryPointInfo>> g_recPoints;
size_t g_recShown = 25; std::set<int> g_recOpen;
void RecRefreshAsync() {
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto v = std::make_shared<const std::vector<AcbRecoveryPointInfo>>(AcbListRecoveryPoints(AcbDefaultRoots()));
        { std::lock_guard<std::mutex> lk(g_recMx); g_recPoints = std::move(v); }
        if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0);
        return 0; }, nullptr, 0, nullptr));
}
std::shared_ptr<const std::vector<AcbRecoveryPointInfo>> RecPoints() { std::lock_guard<std::mutex> lk(g_recMx); return g_recPoints; }
std::wstring JournalEntryDetail(const JEntry& e) {   // what one entry changed, in words
    if (e.kind == L"REG" && e.f.size() >= 3) return e.f[0] + L"\\" + e.f[1] + L"  \x00B7  " + (e.f[2].empty() ? std::wstring(L"(default)") : e.f[2]);
    return e.kind + (e.f.empty() ? std::wstring() : L"  \x00B7  " + e.f[0]);
}
std::wstring g_snapPreviewDir;
std::set<std::string> g_snapForceInclude;
bool g_snapAllowPartial = false;

void DrawJournalOps(Ctx& c, const Cache& s, size_t limit, bool allowMore) {
    const JournalView* jv = s.journalView.get();
    if (!jv || jv->ops.empty()) { Para(c, L"No changes yet.", c.u.f.small, col::sub); return; }
    const std::vector<JEntry>& all = *s.journal;
    const size_t shown = std::min(limit, jv->ops.size());
    for (size_t k = 0; k < shown; ++k) {
        const JournalOpRow& op = jv->ops[k];
        const bool open = g_recOpen.count(op.key) != 0, many = op.ids.size() > 1;
        RowSpec r; r.title = op.title;
        r.desc = op.time + L"  \x00B7  " + (op.informational ? L"Informational" : (many ? std::to_wstring(op.ids.size()) + L" changes" : std::wstring(L"1 change")));
        if (op.informational) {
            r.chips.push_back({op.kind == L"RECOVERY_POINT" ? L"Recovery point" : L"Snapshot restore", col::accent});
            if (op.kind == L"RECOVERY_POINT" && !op.idx.empty() && !all[op.idx.front()].f.empty()) {
                const std::wstring dir = all[op.idx.front()].f[0];
                r.buttons.push_back({L"Preview restore", Btn::Secondary, true, [dir] {
                    g_snapPreviewDir = dir; g_snapForceInclude.clear(); g_snapAllowPartial = false;
                    NavGo(PRecHistory);
                }});
            }
        } else {
            r.chips.push_back(op.active && op.undone ? std::pair<std::wstring, COLORREF>{L"Partly undone", col::warn} : op.active ? std::pair<std::wstring, COLORREF>{L"Active", col::ok} : std::pair<std::wstring, COLORREF>{L"Undone", col::faint});
            if (many) r.buttons.push_back({open ? L"Hide details" : L"Details", Btn::Ghost, true, [key = op.key] { if (!g_recOpen.erase(key)) g_recOpen.insert(key); }});
            if (op.active) {
                const auto pick = JournalPickOp(op); const std::wstring t = op.title; const int n = op.active;
                r.buttons.push_back({many ? L"Undo (" + std::to_wstring(n) + L")" : std::wstring(L"Undo"), Btn::Secondary, true, [pick, t] {
                    RunOp(L"Undo: " + t, [pick, t] { return UndoOp(t, pick, /*nothingIsFailure*/true); }); }});
            }
        }
        Row(c, r);
        if (open && !op.informational) {
            const size_t cap = 40; size_t n = 0;
            for (auto it = op.idx.rbegin(); it != op.idx.rend() && n < cap; ++it, ++n) {
                const JEntry& e = all[*it]; const bool act = e.state == L"A";
                RowSpec d; d.title = JournalEntryDetail(e); d.desc = e.time; d.chips.push_back({act ? L"Active" : L"Undone", act ? col::ok : col::faint});
                if (act && !JournalIsInformationalKind(e.kind)) { const int id = e.id; const std::wstring t = e.title; d.buttons = {{L"Undo", Btn::Ghost, true, [id, t] { RunOp(L"Undo: " + t, [id, t] { return UndoOp(t, [id](const JEntry& x) { return x.id == id; }, true); }); }}}; }
                Row(c, d);
            }
            if (op.idx.size() > cap) Para(c, L"\x2026 and " + std::to_wstring(op.idx.size() - cap) + L" older entries of this operation (they are undone with the operation).", c.u.f.small, col::faint);
        }
    }
    if (allowMore && shown < jv->ops.size())
        Buttons(c, {{L"Show more (" + std::to_wstring(jv->ops.size() - shown) + L" more operations)", Btn::Secondary, true, [] { g_recShown += 25; }}});
}
void PageRecHistory(Ctx& c, const Cache& s) {
    const AcbRoots roots = AcbDefaultRoots();
    const auto points = RecPoints();
    uint64_t totalBytes = 0; if (points) for (const auto& pt : *points) totalBytes += pt.bytesOnDisk;
    Section(c, L"Recovery points (Backup & Migration)", L"Stored in " + roots.data + L"\\Recovery. Used for automatic rollback and snapshot restoration" + (points ? L" (" + FormatBytes(totalBytes) + L" used)." : std::wstring(L".")));
    if (!points) Para(c, L"Reading the recovery points\x2026", c.u.f.small, col::sub);
    else if (points->empty()) {
        StatePanel(c, L"\xE81C", L"No backup recovery points yet", L"Recovery points are written automatically before a backup restore or major change.");
    } else {
        for (size_t i = 0; i < points->size() && i < 20; ++i) {
            const auto& pt = (*points)[i];
            RowSpec r; r.icon = L"\xE81C"; r.title = pt.id + (pt.sourceArchive.empty() ? L"" : L"  \x00B7  " + pt.sourceArchive);
            r.desc = L"Created " + pt.created + L"  \x00B7  " + std::to_wstring(pt.fileCount) + L" file(s)  \x00B7  " + FormatBytes(pt.bytesOnDisk) +
                     (pt.restoredSummary.empty() ? std::wstring() : L"\n" + pt.restoredSummary);
            r.chips.push_back({pt.status, pt.interrupted || pt.status == L"Rollback incomplete" ? col::err : pt.status == L"Rolled back" ? col::warn : col::ok});
            r.chips.push_back({pt.sealed ? L"Sealed" : L"Older point", pt.sealed ? col::accent : col::sub});
            const std::wstring dir = pt.dir;
            if (pt.interrupted) {
                r.buttons = {{L"Finish rolling back", Btn::Danger, true, [roots, dir] {
                    if (TestGuard(L"Finish interrupted rollback")) return;
                    RunOp(L"Finish rolling back", [roots, dir] {
                        const AcbSnapRestoreReport rep = AcbFinishInterruptedRollback(roots, dir);
                        RecRefreshAsync();
                        if (!rep.ok) { LogE(rep.error); return false; }
                        LogOk(L"Interrupted restore rolled back (" + std::to_wstring(rep.rollbackPutBack) + L" file(s)).");
                        return true;
                    });
                }}};
            } else {
                r.buttons = {{L"Preview restore", Btn::Secondary, true, [dir] {
                    g_snapPreviewDir = dir; g_snapForceInclude.clear(); g_snapAllowPartial = false;
                    InvalidateRect(g_wnd, nullptr, FALSE);
                }}};
            }
            Row(c, r);
        }
        if (points->size() > 20) Para(c, L"The newest 20 of " + std::to_wstring(points->size()) + L" recovery points are listed; the others are still in the Recovery folder.", c.u.f.small, col::faint);
    }
    if (!g_snapPreviewDir.empty()) {
        const AcbSnapPlan plan = AcbPlanSnapshotRestore(roots, g_snapPreviewDir);
        Section(c, L"Restore Snapshot preview", L"Point: " + NameOf(g_snapPreviewDir) + L". Creates a new recovery point first; files modified after the operation are skipped by default.");
        if (!plan.ok) {
            Para(c, L"Preview unavailable: " + plan.error, c.u.f.small, col::err);
            Buttons(c, {{L"Close preview", Btn::Secondary, true, [] { g_snapPreviewDir.clear(); InvalidateRect(g_wnd, nullptr, FALSE); }}});
        } else {
            for (const auto& pi : plan.items) {
                RowSpec pr; pr.icon = L"\xE81C"; pr.title = acbdetail::Show(pi.item.dest);
                const bool forced = g_snapForceInclude.count(pi.item.dest) != 0;
                std::wstring actTxt; COLORREF actCol = col::sub;
                switch (pi.action) {
                    case AcbSnapAction::Replace: actTxt = L"Replace"; actCol = col::warn; break;
                    case AcbSnapAction::Delete: actTxt = L"Delete"; actCol = col::warn; break;
                    case AcbSnapAction::MergeSettings: actTxt = L"3-way merge (" + std::to_wstring(pi.mergeReverted) + L" revert, " + std::to_wstring(pi.mergeKept) + L" keep)"; actCol = col::ok; break;
                    case AcbSnapAction::AlreadyBefore: actTxt = L"Already as before"; actCol = col::ok; break;
                    case AcbSnapAction::BlockedDamaged: actTxt = L"Blocked"; actCol = col::err; break;
                    default: actTxt = forced ? L"Include anyway" : L"Skip"; actCol = forced ? col::warn : col::sub; break;
                }
                pr.desc = pi.reason.empty() ? pi.destPath : (pi.reason + L"  \x00B7  " + pi.destPath);
                pr.chips.push_back({actTxt, actCol});
                if (pi.action == AcbSnapAction::SkipModified || pi.action == AcbSnapAction::SkipDeleted ||
                    pi.action == AcbSnapAction::SkipCreatedOrChanged || pi.action == AcbSnapAction::SkipUnsealed) {
                    const std::string destKey = pi.item.dest;
                    pr.buttons = {{forced ? L"Skip (default)" : L"Include anyway", Btn::Ghost, true, [destKey] {
                        if (!g_snapForceInclude.erase(destKey)) g_snapForceInclude.insert(destKey);
                        InvalidateRect(g_wnd, nullptr, FALSE);
                    }}};
                }
                Row(c, pr);
            }
            const bool anyAction = (plan.replaceCount + plan.deleteCount + plan.mergeCount) > 0 || !g_snapForceInclude.empty();
            const std::wstring snapDir = g_snapPreviewDir;
            Buttons(c, {
                {L"Restore snapshot", Btn::Primary, anyAction && plan.blockedCount == 0, [roots, snapDir] {
                    if (TestGuard(L"Restore snapshot")) return;
                    if (!Confirm(L"Restore snapshot from:\n" + NameOf(snapDir) + L"\n\nA new recovery point of your current state will be created first.", L"Restore snapshot", false)) return;
                    AcbSnapRestoreOptions opts; opts.forceIncludeDests = g_snapForceInclude; opts.allowPartialVerified = g_snapAllowPartial;
                    RunOp(L"Restore snapshot", [roots, snapDir, opts] {
                        const AcbSnapRestoreReport rep = AcbRestoreSnapshot(roots, snapDir, opts);
                        RecRefreshAsync();
                        if (!rep.ok) { LogE(L"Snapshot restore failed: " + rep.error); return false; }
                        Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass")); ApplyMonitorConfig(); SbSetOption(L"", 0); MiniSetOption(L"", 0); UpdateMotion(); ApplyMaterial(g_wnd);
                        LogOk(L"Snapshot restored: " + std::to_wstring(rep.filesRestored + rep.filesDeleted) + L" item(s), " + std::to_wstring(rep.filesSkipped) + L" skipped.");
                        return true;
                    });
                }},
                {L"Close preview", Btn::Secondary, true, [] { g_snapPreviewDir.clear(); InvalidateRect(g_wnd, nullptr, FALSE); }}
            });
        }
    }
    const JournalView* jv = s.journalView.get();
    const int active = jv ? jv->activeEntries : 0;
    Section(c, L"Changes made by AnkhCore", jv ? std::to_wstring(jv->ops.size()) + L" operation(s), " + std::to_wstring(jv->entries) + L" recorded change(s). One row per operation; Details shows its entries. Newest first." : std::wstring(L"Reading the journal\x2026"));
    Buttons(c, {{L"Undo all (" + std::to_wstring(active) + L")", Btn::Danger, active > 0, [] {
        if (!Confirm(L"Undo every change AnkhCore recorded (services, tweaks, startup apps, mods, cursors, wallpaper)?\n\nRecovery points and icon packs are not touched by Undo all.", L"Undo all changes", true)) return;
        RunOp(L"Undo all changes", [] { return UndoOp(L"All changes", JournalPickUndoAll()); }, true); }}});
    DrawJournalOps(c, s, g_recShown, true);
}
void PageRecovery(Ctx& c, const Cache& s) {
    const auto points = RecPoints();
    const int active = s.journalView ? s.journalView->activeEntries : 0;
    Section(c, L"Backup & recovery pages", L"Portable .acbackup archives, settings merge, recovery points and the AnkhCore undo journal.");
    {
        RowSpec r; r.icon = L"\xE81C"; r.title = L"Backup & Migration";
        r.desc = L"Create and restore portable .acbackup archives, select settings groups, and configure automatic pre-change backups.";
        r.chips.push_back({L".acbackup v1", col::accent});
        r.buttons = {{L"Open", Btn::Secondary, true, [] { NavGo(PBackup); }}};
        Row(c, r);
    }
    {
        RowSpec r; r.icon = L"\xE8FD"; r.title = L"Recovery History";
        r.desc = L"Inspect recovery points under Recovery\\<timestamp> and undo recorded AnkhCore changes.";
        r.chips.push_back({(points ? std::to_wstring(points->size()) : std::wstring(L"\u2026")) + L" recovery pt(s), " + std::to_wstring(active) + L" active journal", col::sub});
        r.buttons = {{L"Open", Btn::Secondary, true, [] { NavGo(PRecHistory); }}};
        Row(c, r);
    }
    Section(c, L"System Restore points", L"A restore point takes Windows back to an earlier state: theme patch, icons, Windhawk and settings. Your personal files are not touched.");
    Buttons(c, {{L"Create restore point", Btn::Primary, true, [] { RunOp(L"Create a restore point", [] { std::wstring e; return CreateRestorePoint(L"AnkhCore " + NowText(true), e); }, false, true); }},
                {L"Open System Restore", Btn::Secondary, true, [] { if (TestGuard(L"Open System Restore")) return; ShellExecuteW(nullptr, L"open", (SysDir() + L"\\rstrui.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }},
                {L"Refresh", Btn::Ghost, true, [] { RefreshAsync(true); }}});
    if (!s.deepDone) { Shimmer(c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(52)}, c.S(28), GetTickCount64()); c.y += c.S(62); }
    else if (s.rps.empty()) StatePanel(c, L"\xE7BA", L"No restore points found", L"Create one before making system changes.", col::warn);
    for (size_t i = 0; i < s.rps.size() && i < 12; ++i) { RowSpec r; r.icon = L"\xE81C"; r.title = s.rps[i].description; r.desc = s.rps[i].date; Row(c, r); }
    Section(c, L"Changes made by AnkhCore", L"Each change can be undone on its own, newest first.");
    Buttons(c, {{L"Undo all (" + std::to_wstring(active) + L")", Btn::Danger, active > 0, [] {
        if (!Confirm(L"Undo every change AnkhCore recorded (services, tweaks, startup apps, mods, cursors, wallpaper)?\n\nRecovery points and icon packs are not touched by Undo all.", L"Undo all changes", true)) return;
        RunOp(L"Undo all changes", [] { return UndoOp(L"All changes", JournalPickUndoAll()); }, true); }}});
    DrawJournalOps(c, s, 8, false);   // the newest operations; Recovery History has them all
    if (s.journalView && s.journalView->ops.size() > 8) Para(c, L"The newest 8 operations are shown here; open Recovery History for all " + std::to_wstring(s.journalView->ops.size()) + L".", c.u.f.small, col::faint);
}
HwInputs HwCollectInputs(const Cache& s);   // defined with the Hardware Information page
void PageActivity(Ctx& c, const Cache& s) {
    Buttons(c, {{L"Export log", Btn::Primary, true, [] {
                     if (TestGuard(L"Export log")) return;
                     const std::wstring dst = UserFolder(FOLDERID_Desktop) + L"\\AnkhCore log " + NowText(true).substr(0, 10) + L".txt";
                     if (CopyFileW(LogPath().c_str(), dst.c_str(), FALSE)) { LogOk(L"Log exported to " + dst); MessageBoxW(g_wnd, (L"Saved to your desktop:\n" + dst).c_str(), kProductFull, MB_OK | MB_ICONINFORMATION); }
                     else LogE(L"Could not export the log."); }},
                {L"Export support bundle (.zip)", Btn::Secondary, true, [s] {
                     if (TestGuard(L"Export support bundle")) return;
                     const std::wstring dst = UserFolder(FOLDERID_Desktop) + L"\\AnkhCore-Support-" + NowText(true).substr(0, 10) + L".zip";
                     AcbRoots roots = AcbDefaultRoots();
                     std::string txt = WToUtf8(kProductFull) + " Support Bundle\r\nVersion: " + WToUtf8(kVersionLabel) + " (" + WToUtf8(kVersion) + ")\r\n";
                     txt += "OS: " + WToUtf8(WinVerText(s.ver)) + "\r\n";
                     txt += "Mode: " + std::string(g_read.present ? "Live" : "Idle") + "\r\n";
                     std::wstring err;
                     // C1: the hardware inventory (identity hash, sources and availability, telemetry vs control, limitations; no serial numbers, no machine or user name)
                     const HwInventory hwInv = BuildHwInventory(HwCollectInputs(s));
                     const std::vector<std::pair<std::string, std::string>> extra = {{"hardware-inventory.txt", WToUtf8(HwInventoryText(hwInv))}, {"hardware-inventory.json", HwInventoryJson(hwInv)}};
                     if (AcbExportSupportBundle(roots, dst, txt, err, extra)) {
                         LogOk(L"Support bundle exported to " + dst);
                         MessageBoxW(g_wnd, (L"Support bundle saved to your desktop:\n" + dst).c_str(), kProductFull, MB_OK | MB_ICONINFORMATION);
                     } else {
                         LogE(L"Support bundle export failed: " + err);
                     } }},
                {L"Open log folder", Btn::Secondary, true, [] { if (TestGuard(L"Open log folder")) return; ShellExecuteW(nullptr, L"open", (DataDir() + L"\\Logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}});
    const auto lines = LogSnapshot();
    const int lh = c.S(24);
    RECT box{c.x, c.y, c.x + c.w, c.y + std::max<int>(1, static_cast<int>(lines.size())) * lh + c.S(20)};
    Round(c.dc, box, c.S(28), col::panel, col::border);
    int y = c.y + c.S(10);
    for (const auto& l : lines) {
        RECT r{c.x + c.S(14), y, c.x + c.w - c.S(14), y + lh};
        if (c.Visible(r)) {
            const COLORREF lc = l.level == Lv::Ok ? col::ok : l.level == Lv::Warn ? col::warn : l.level == Lv::Err ? col::err : col::text;
            const wchar_t* tag = l.level == Lv::Ok ? L"OK" : l.level == Lv::Warn ? L"WARN" : l.level == Lv::Err ? L"ERROR" : L"INFO";
            Text(c.dc, L"[" + l.time + L"]", RECT{r.left, r.top, r.left + c.S(80), r.bottom}, c.u.f.small, col::faint, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, tag, RECT{r.left + c.S(80), r.top, r.left + c.S(130), r.bottom}, c.u.f.small, lc, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, l.text, RECT{r.left + c.S(130), r.top, r.right, r.bottom}, c.u.f.small, l.level == Lv::Info ? col::sub : col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += lh;
    }
    c.y = box.bottom + c.S(10);
}
void PageStartup(Ctx& c, const Cache& s) {
    Section(c, L"Start with Windows", L"Uses one scheduled task (\\AnkhCore\\Startup) with the same rights as AnkhCore's own launcher: no service and no extra background program.");
    {
        RowSpec r; r.icon = L"\xE7E8"; r.title = L"Start AnkhCore when I sign in";
        r.desc = !s.startupChecked ? L"Checking Task Scheduler\x2026" : s.startupOn ? L"On. Task Scheduler starts AnkhCore about 20 seconds after you sign in."
                                                                                   : L"Off. AnkhCore starts only when you open it.";
        r.chips.push_back({!s.startupChecked ? L"Checking" : s.startupOn ? L"Registered (verified)" : L"Not registered", s.startupOn ? col::ok : col::faint});
        r.toggle = s.startupOn ? 1 : 0; r.enabled = s.startupChecked;
        r.onToggle = [on = s.startupOn] { RunOp(on ? L"Stop starting with Windows" : L"Start with Windows", [on] { return SetStartWithWindows(!on); }); };
        Row(c, r);
    }
    {
        RowSpec r; r.icon = L"\xE8A0"; r.title = L"Start minimized to the system tray";
        r.desc = L"At sign-in AnkhCore opens in the tray only. The monitoring engine, Desktop Sidebar and Mini Monitor start as configured.";
        const bool on = Cfg().Bool(L"startup.minimized", true);
        r.toggle = on ? 1 : 0;
        r.onToggle = [on] { Cfg().SetBool(L"startup.minimized", !on); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    Section(c, L"Monitoring engine", L"One sensor helper feeds the Dashboard, System Monitor, Desktop Sidebar and Mini Monitor. It keeps running while the window is closed to the tray.");
    {
        RowSpec r; r.icon = L"\xE9D2"; r.title = L"Start hardware monitoring automatically";
        r.desc = L"When off, monitoring starts when you press Start on System Monitor or open the Desktop Sidebar or Mini Monitor.";
        const bool on = Cfg().Bool(L"monitoring.autoStart", true);
        r.toggle = on ? 1 : 0;
        r.onToggle = [on] { Cfg().SetBool(L"monitoring.autoStart", !on); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {
        const bool running = Hub().HelperRunning(), live = g_read.present;
        RowSpec r; r.icon = L"\xE9F9"; r.title = L"Engine status";
        r.desc = live ? L"Running. Readings are live." : running ? L"Running, waiting for the first readings." : L"Stopped.";
        r.chips.push_back({live ? L"Live" : running ? L"Starting" : L"Stopped", live ? col::ok : running ? col::warn : col::faint});
        r.buttons = {{L"Start", Btn::Primary, !running, [] { if (TestGuard(L"Start hardware monitoring")) return; g_engine.Monitor(true); }},
                     {L"Stop", Btn::Secondary, running, [] { if (TestGuard(L"Stop hardware monitoring")) return; g_engine.Monitor(false); }}};
        Row(c, r);
    }
    Section(c, L"Preferences", L"Sampling rate, history retention, alert thresholds and network adapter selection.");
    {
        RowSpec r; r.icon = kPageIcon[PMonPrefs]; r.title = kPageName[PMonPrefs]; r.desc = kPageSub[PMonPrefs];
        r.buttons.push_back({L"Open", Btn::Secondary, true, [] { NavGo(PMonPrefs); }});
        Row(c, r);
    }
    Para(c, L"Closing the window keeps AnkhCore in the tray, so monitoring, the Desktop Sidebar and the Mini Monitor stay on. Exit (tray menu), "
            L"signing out and shutting down stop the engine safely: fan control returns to automatic first.", c.u.f.small, col::sub);
}
// ---------------------------------------------------------------- Desktop Monitoring (2.5)
// Overview, alerts, graphs/history and desktop integration. Everything here reads the single snapshot
// the 1 s timer takes (g_read from SensorHub, g_usage from PDH): no page, widget or alert polls on its own.
std::wstring Ago(double sec) {
    if (sec < 0) return L"never";
    if (sec < 90) return std::to_wstring(static_cast<int>(sec + 0.5)) + L" s ago";
    if (sec < 5400) return std::to_wstring(static_cast<int>(sec / 60 + 0.5)) + L" min ago";
    return std::to_wstring(static_cast<int>(sec / 3600 + 0.5)) + L" h ago";
}
std::wstring StampText(const SYSTEMTIME& st) { wchar_t b[16]; swprintf(b, 16, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond); return b; }
// Live value, "stale" (helper data too old) or "not exposed" (the hardware has no such sensor).
std::wstring Live(double v, bool fromHelper, const wchar_t* unit) {
    if (fromHelper && !g_read.present) return g_read.everSeen ? L"stale" : L"no data";
    if (v < 0) return L"not exposed";
    return NA(v, unit);
}
void DrawGraph(Ctx& c, RECT r, const Ring& s, double lo, double hi, COLORREF color, const std::wstring& title, const wchar_t* unit) {
    Round(c.dc, r, c.S(28), col::panel, col::border);
    Text(c.dc, title, RECT{r.left + c.S(14), r.top + c.S(8), r.right - c.S(10), r.top + c.S(26)}, c.u.f.small, col::sub, DT_SINGLELINE);
    double mn = 0, mx = 0;
    const bool any = s.MinMax(mn, mx);
    const std::wstring stats = !any ? L"no data" : NA(s.Last(), unit) + L"   min " + NA(mn, unit) + L"   max " + NA(mx, unit);
    Text(c.dc, HasValue(s.Last()) || !any ? stats : L"(gap)   min " + NA(mn, unit) + L"   max " + NA(mx, unit),
         RECT{r.left + c.S(14), r.top + c.S(8), r.right - c.S(12), r.top + c.S(26)}, c.u.f.small, any ? col::text : col::faint, DT_RIGHT | DT_SINGLELINE);
    const int gx = r.left + c.S(12), gy = r.top + c.S(32), gw = r.right - r.left - c.S(24), gh = r.bottom - r.top - c.S(42);
    Line(c.dc, gx, gy + gh, gx + gw, gy + gh, col::border);
    if (hi <= lo) hi = lo + 1;
    const size_t n = s.Size(), cap = s.Capacity();
    if (n < 2) return;
    const auto graphP = SeriesPoints(s, std::min<size_t>(cap, 600), lo, hi);
    AreaGraph(c.dc, RECT{gx, gy, gx + gw, gy + gh}, graphP, color, c.S(2));
    const int m = static_cast<int>(graphP.size());
    GraphCrosshair(c, RECT{gx, gy, gx + gw, gy + gh}, m, [&](int i) { const float v = graphP[i]; return v != v ? std::wstring(L"gap") : NA(lo + v * (hi - lo), unit); }, {{&graphP, color}});
}
const std::vector<std::pair<int, const wchar_t*>> kIntervals = {{1, L"1 s"}, {2, L"2 s"}, {5, L"5 s"}, {10, L"10 s"}};
const std::vector<std::pair<int, const wchar_t*>> kHistories = {{5, L"5 min"}, {15, L"15 min"}, {60, L"1 hour"}, {240, L"4 hours"}};
void ApplyMonitorConfig() {
    for (auto& r : g_alerts.rules) {
        r.enabled = Cfg().Bool(L"alert." + Utf8ToW(r.id) + L".enabled", r.enabled);
        const int lo = r.id == "disk.low" ? 1 : (r.id == "net.high" ? 50 : (r.id == "ram.high" || r.id == "cpu.load" ? 50 : 40));
        const int hi = r.id == "net.high" ? 5000 : (r.id == "disk.low" ? 50 : (r.id == "ram.high" || r.id == "cpu.load" ? 100 : 110));
        r.threshold = Cfg().Int(L"alert." + Utf8ToW(r.id) + L".threshold", static_cast<int>(r.threshold), lo, hi);
    }
    g_engine.Configure(Cfg().Int(L"monitor.intervalSec", 1, 1, 60), Cfg().Int(L"monitor.historyMin", 15, 1, 1440) * 60, g_alerts.rules);
    g_engine.CopyHistory(g_store);   // the UI's copy follows at once, so settings show immediately
}
std::wstring AlertName(const std::string& id) {
    if (id == "cpu.warn") return L"CPU temperature warning";
    if (id == "cpu.crit") return L"CPU temperature critical";
    if (id == "gpu.warn") return L"GPU temperature warning";
    if (id == "gpu.crit") return L"GPU temperature critical";
    if (id == "fan.stop") return L"Fan stopped under load";
    if (id == "ram.high") return L"Memory usage high";
    if (id == "disk.low") return L"Disk free space low";
    if (id == "cpu.load") return L"Sustained CPU load";
    if (id == "net.high") return L"Sustained network throughput";
    return Utf8ToW(id);
}
std::wstring AlertRuleDesc(const AlertRule& r) {
    const std::wstring th = std::to_wstring(static_cast<int>(r.threshold));
    const std::wstring hold = std::to_wstring(r.holdSec);
    if (r.id == "fan.stop")
        return L"A live fan reports 0 rpm while the CPU stays at " + th + L" \u00B0C or more for " + hold + L" s. Unverified fan sources never trigger it.";
    if (r.id == "disk.low")
        return L"At " + th + L"% free space or less for " + hold + L" s; clears above " + std::to_wstring(static_cast<int>(r.threshold + r.hysteresis)) + L"%.";
    const std::wstring unit = r.id == "net.high" ? L" Mbps" : (r.id == "ram.high" || r.id == "cpu.load" ? L"%" : L" \u00B0C");
    return L"At " + th + unit + L" or more for " + hold + L" s; clears below " + std::to_wstring(static_cast<int>(r.threshold - r.hysteresis)) + unit + L".";
}
std::pair<std::vector<int>, std::wstring> AlertRuleChoices(const std::string& id) {
    if (id == "cpu.warn") return {{85, 88, 90, 93}, L" \u00B0C"};
    if (id == "cpu.crit") return {{95, 97, 99}, L" \u00B0C"};
    if (id == "gpu.warn") return {{80, 85, 88}, L" \u00B0C"};
    if (id == "gpu.crit") return {{90, 93, 95}, L" \u00B0C"};
    if (id == "ram.high") return {{80, 85, 90, 95}, L"%"};
    if (id == "disk.low") return {{5, 10, 15, 20}, L"%"};
    if (id == "cpu.load") return {{85, 90, 95}, L"%"};
    if (id == "net.high") return {{100, 250, 500, 1000}, L" Mbps"};
    return {{}, L""};
}
void ApplySystemProfile(int id) {
    Cfg().SetInt(L"profile.active", id);
    if (id == 1) {          // Gaming: 1s sampling, Mini Monitor on, Sidebar minimal
        Cfg().SetInt(L"monitor.intervalSec", 1);
        Cfg().SetInt(L"monitor.historyMin", 15);
        Cfg().SetBool(L"mini.enabled", true);
        Cfg().SetInt(L"sidebar.preset", 0);
    } else if (id == 2) {   // Work: 2s sampling, balanced Sidebar
        Cfg().SetInt(L"monitor.intervalSec", 2);
        Cfg().SetInt(L"monitor.historyMin", 30);
        Cfg().SetBool(L"sidebar.enabled", true);
        Cfg().SetInt(L"sidebar.preset", 1);
    } else if (id == 3) {   // Minimal: 5s sampling, widgets off
        Cfg().SetInt(L"monitor.intervalSec", 5);
        Cfg().SetInt(L"monitor.historyMin", 10);
        Cfg().SetBool(L"mini.enabled", false);
        Cfg().SetBool(L"sidebar.enabled", false);
    } else {                // Default
        Cfg().SetInt(L"monitor.intervalSec", 1);
        Cfg().SetInt(L"monitor.historyMin", 15);
    }
    Cfg().Save();
    ApplyMonitorConfig();
    LogOk(L"Applied system profile #" + std::to_wstring(id));
}
void PageMonPrefs(Ctx& c, const Cache&) {
    Section(c, L"Profiles", L"Quickly switch monitoring interval, history retention, and widget presets for your current workload.");
    {
        const int prof = Cfg().Int(L"profile.active", 0, 0, 3);
        Chips(c, L"Active profile", {
            {L"Default", prof == 0, [] { ApplySystemProfile(0); }},
            {L"Gaming", prof == 1, [] { ApplySystemProfile(1); }},
            {L"Work", prof == 2, [] { ApplySystemProfile(2); }},
            {L"Minimal", prof == 3, [] { ApplySystemProfile(3); }}});
    }
    Section(c, L"Sampling & history", L"Kept in memory only (bounded; nothing is written to disk). Gaps show where readings were missing or stale.");
    {
        const int iv = g_store.Interval(), hm = g_store.History() / 60, tu = TempUnitF() ? 1 : 0;
        std::vector<ChipOpt> a, b;
        for (const auto& [v, t] : kIntervals) a.emplace_back(t, iv == v, [v = v] { Cfg().SetInt(L"monitor.intervalSec", v); Cfg().Save(); ApplyMonitorConfig(); });
        for (const auto& [v, t] : kHistories) b.emplace_back(t, hm == v, [v = v] { Cfg().SetInt(L"monitor.historyMin", v); Cfg().Save(); ApplyMonitorConfig(); });
        Chips(c, L"Sampling interval", a);
        Chips(c, L"History", b);
        Chips(c, L"Temperature unit", {{L"\u00B0C (Celsius)", tu == 0, [] { Cfg().SetInt(L"monitor.tempUnit", 0); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }},
                                       {L"\u00B0F (Fahrenheit)", tu == 1, [] { Cfg().SetInt(L"monitor.tempUnit", 1); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); }}});
        const size_t cap = g_store.Get(SCpuTemp).Capacity();
        Para(c, std::to_wstring(cap) + L" points per series (" + std::to_wstring(static_cast<int>(cap * iv / 60)) + L" min at " + std::to_wstring(iv) + L" s), about " +
                std::to_wstring(static_cast<int>(cap * SeriesCount * sizeof(double) / 1024)) + L" KB.", c.u.f.small, col::faint);
    }
    Section(c, L"Network adapter", L"Which adapter feeds the Network readout on the Dashboard, System Monitor, Desktop Sidebar and Mini Monitor.");
    {
        const int mode = Cfg().Int(L"sidebar.netMode", NetAuto, NetAuto, NetManual);
        const std::wstring manual = Cfg().Str(L"sidebar.netAdapter", L"");
        std::wstring first; for (const auto& a : g_net) if (a.present && (first.empty() || a.isDefault)) first = a.guid;
        Chips(c, L"Adapter", {{L"Automatic", mode == NetAuto, [] { NetSetMode(NetAuto, L""); }},
                              {L"All adapters", mode == NetAll, [] { NetSetMode(NetAll, L""); }},
                              {L"Manual", mode == NetManual, [manual, first] { NetSetMode(NetManual, manual.empty() ? first : manual); }}});
        if (mode == NetManual) {
            std::vector<ChipOpt> opts;
            for (const auto& a : g_net) if (a.present || a.guid == manual) {
                const std::wstring g = a.guid;
                opts.emplace_back(a.name.empty() ? g : a.name, g == manual, [g] { NetSetMode(NetManual, g); });
            }
            if (!opts.empty()) Chips(c, L"Manual adapter", opts);
        }
    }
    Section(c, L"Alerts", L"An alert needs the condition to last (brief Turbo Boost spikes are ignored), repeats at most once per cooldown, and never fires for a sensor that is missing or stale.");
    for (auto& rule : g_alerts.rules) {
        const std::wstring id = Utf8ToW(rule.id);
        RowSpec row; row.title = AlertName(rule.id);
        row.desc = AlertRuleDesc(rule);
        if (g_alerts.Active(rule.id)) row.chips.push_back({L"Active", col::err});
        row.toggle = rule.enabled ? 1 : 0;
        row.onToggle = [id, on = rule.enabled] { Cfg().SetBool(L"alert." + id + L".enabled", !on); Cfg().Save(); ApplyMonitorConfig(); };
        Row(c, row);
        const auto [choices, suf] = AlertRuleChoices(rule.id);
        if (!choices.empty()) {
            std::vector<ChipOpt> opts;
            for (int t : choices) opts.emplace_back(std::to_wstring(t) + suf, static_cast<int>(rule.threshold) == t,
                                                    [id, t] { Cfg().SetInt(L"alert." + id + L".threshold", t); Cfg().Save(); ApplyMonitorConfig(); });
            Chips(c, L"Threshold", opts);
        }
    }
    Section(c, L"Reset");
    Chips(c, L"Defaults", {{L"Restore monitoring defaults", false, [] {
        Cfg().SetInt(L"monitor.intervalSec", 1);
        Cfg().SetInt(L"monitor.historyMin", 15);
        Cfg().SetBool(L"alert.cpu.warn.enabled", true); Cfg().SetInt(L"alert.cpu.warn.threshold", 88);
        Cfg().SetBool(L"alert.cpu.crit.enabled", true); Cfg().SetInt(L"alert.cpu.crit.threshold", 95);
        Cfg().SetBool(L"alert.gpu.warn.enabled", true); Cfg().SetInt(L"alert.gpu.warn.threshold", 85);
        Cfg().SetBool(L"alert.gpu.crit.enabled", true); Cfg().SetInt(L"alert.gpu.crit.threshold", 92);
        Cfg().SetBool(L"alert.fan.stop.enabled", true);
        Cfg().SetBool(L"alert.ram.high.enabled", true); Cfg().SetInt(L"alert.ram.high.threshold", 90);
        Cfg().SetBool(L"alert.disk.low.enabled", true); Cfg().SetInt(L"alert.disk.low.threshold", 10);
        Cfg().SetBool(L"alert.cpu.load.enabled", false); Cfg().SetInt(L"alert.cpu.load.threshold", 95);
        Cfg().SetBool(L"alert.net.high.enabled", false); Cfg().SetInt(L"alert.net.high.threshold", 500);
        Cfg().Save();
        NetSetMode(NetAuto, L"");
        ApplyMonitorConfig();
        LoadFanPolicyCfg();
        InvalidateRect(g_wnd, nullptr, FALSE);
    }}});
}
void PageDesktop(Ctx& c, const Cache&) {
    // ---- the desktop widgets and the way to their settings pages (first, so they are one click away)
    Section(c, L"Desktop widgets", L"The Desktop Sidebar, Mini Monitor, these pages and the alerts all use the same snapshot: one sensor helper, read once per second.");
    {
        auto link = [&](int page) { RowSpec r; r.icon = kPageIcon[page]; r.title = kPageName[page]; r.desc = kPageSub[page]; r.buttons.push_back({L"Open", Btn::Secondary, true, [page] { NavGo(page); }}); Row(c, r); };
        RowSpec row; row.icon = L"\xE8A0"; row.title = L"Desktop Sidebar"; row.desc = SbRecoveryFailed() ? L"The Sidebar is not showing and could not be reopened automatically. Switch it off and on to try again." : L"Live readings as part of your desktop.";
        row.toggle = g_sidebarOn ? 1 : 0; row.onToggle = [] { ToggleSidebar(!g_sidebarOn); };
        Row(c, row);
        for (int pg : {PSbLayout, PSbAppearance, PSbWidgets, PSbBehavior, PSbProfiles}) link(pg);
        RowSpec m; m.icon = L"\xE78B"; m.title = L"Mini Monitor"; m.desc = L"A small floating readout you can drag anywhere. Same live data as the Sidebar."; m.toggle = g_miniOn ? 1 : 0; m.onToggle = [] { ToggleMini(!g_miniOn); };
        Row(c, m);
        link(PMiniSet);
        link(PMonPrefs);
        Para(c, L"Data source: " + std::to_wstring(g_engine.Polls()) + L" polls since start (one per second, however many views are open). "
                L"Closing the window keeps monitoring running in the tray (see Startup & Background).", c.u.f.small, col::faint);
    }
    const Readings& r = g_read;
    auto chipRow = [&](const wchar_t* label, const std::vector<std::tuple<std::wstring, bool, std::function<void()>>>& opts) {
        Text(c.dc, label, RECT{c.x + c.S(16), c.y, c.x + c.S(170), c.y + c.S(30)}, c.u.f.small, col::sub, DT_VCENTER | DT_SINGLELINE);
        int x = c.x + c.S(170);
        for (const auto& [text, sel, fn] : opts) {
            const int bw = std::max(c.S(58), TextWidth(c.dc, text, c.u.f.small) + c.S(24));
            RECT br{x, c.y, x + bw, c.y + c.S(30)};
            Round(c.dc, br, c.S(15), sel ? col::accentDeep : (c.Hover(br) ? col::panel2 : col::panel), sel ? col::accent : col::border);
            Text(c.dc, text, br, c.u.f.small, sel ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const auto f = fn; c.Hit(br, [f] { f(); InvalidateRect(g_wnd, nullptr, FALSE); });
            x += bw + c.S(8);
        }
        c.y += c.S(38);
    };
    // ---- overview
    Section(c, L"Overview", L"Live values with their age. \"not exposed\" = this hardware has no such sensor; \"stale\" = the sensor helper stopped updating.");
    {
        const bool running = Hub().HelperRunning();
        RowSpec row; row.icon = L"\xE9D2"; row.title = L"Monitoring engine";
        row.desc = r.present ? L"Live. Last update " + StampText(r.stamp) + L" (" + Ago(r.ageSec) + L")."
                 : r.everSeen ? L"Stale: the last update was " + StampText(r.stamp) + L" (" + Ago(r.ageSec) + L"). " + (running ? L"The helper is running but not writing." : L"The helper is not running.")
                              : L"No data yet. Start monitoring below or on System Monitor.";
        row.chips.push_back({r.present ? L"Live" : r.everSeen ? L"Stale" : L"Stopped", r.present ? col::ok : col::warn});
        row.buttons = {{L"Start", Btn::Primary, !running, [] { if (TestGuard(L"Start hardware monitoring")) return; g_engine.Monitor(true); }}};
        Row(c, row);
    }
    {
        const int gap = c.S(12), cols = 3, tw = (c.w - gap * (cols - 1)) / cols, th = c.S(66);
        struct Tile { std::wstring k, v; };
        std::vector<Tile> tiles = {
            {L"CPU temperature", Live(DispTemp(r.cpuTemp), true, TempSuffix())}, {L"CPU usage", NA(g_usage.cpu, L" %")},
            {L"GPU temperature", Live(DispTemp(r.gpuTemp), true, TempSuffix())}, {L"GPU usage", NA(g_usage.gpu, L" %")},
            {L"Memory", NA(g_usage.ram, L" %") + L"  (" + FormatBytes(g_usage.ramUsed) + L")"},
            {L"Network", NetSpeedText()}};
        for (size_t i = 0; i < tiles.size(); ++i) {
            const int x = c.x + static_cast<int>(i % cols) * (tw + gap), y = c.y + static_cast<int>(i / cols) * (th + gap);
            RECT tr{x, y, x + tw, y + th};
            Round(c.dc, tr, c.S(28), col::panel, col::border);
            Text(c.dc, tiles[i].k, RECT{x + c.S(14), y + c.S(10), x + tw, y + c.S(28)}, c.u.f.small, col::sub);
            const bool dim = tiles[i].v == L"stale" || tiles[i].v == L"not exposed" || tiles[i].v == L"no data" || tiles[i].v == L"N/A" || tiles[i].v == L"disconnected";
            Text(c.dc, tiles[i].v, RECT{x + c.S(14), y + c.S(30), x + tw - c.S(8), y + th - c.S(6)}, c.u.f.h2, dim ? col::faint : col::text, DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        c.y += static_cast<int>((tiles.size() + cols - 1) / cols) * (th + gap) + c.S(4);
    }
    if (!r.present) Para(c, L"Fans: no live data.", c.u.f.small, col::faint);
    else if (r.fans.empty()) Para(c, L"Fans: this hardware exposes no fan sensor to AnkhCore.", c.u.f.small, col::faint);
    else for (const auto& f : r.fans) {
        RowSpec row; row.title = f.name.empty() ? L"Fan" : f.name;
        row.right = f.rpm < 0 ? L"not exposed" : std::to_wstring(static_cast<int>(f.rpm)) + L" rpm"; row.rightColor = f.live ? col::accent : col::faint;
        row.chips.push_back({f.source.empty() ? L"Sensor" : f.source, col::sub});
        row.chips.push_back({f.live ? L"Live" : L"Not yet verified live", f.live ? col::ok : col::warn});
        Row(c, row);
    }
    // ---- network
    Section(c, L"Network", L"Speeds and totals for each physical adapter, from Windows counters. They are live with or without the sensor helper.");
    {
        const int mode = g_netSel.mode, period = NetPeriodCfg();
        const std::wstring manual = Cfg().Str(L"sidebar.netAdapter", L"");
        std::wstring first; for (const auto& a : g_net) if (a.present && (first.empty() || a.isDefault)) first = a.guid;
        chipRow(L"Adapter", {{L"Automatic", mode == NetAuto, [] { NetSetMode(NetAuto, L""); }}, {L"All adapters", mode == NetAll, [] { NetSetMode(NetAll, L""); }},
                             {L"Manual", mode == NetManual, [manual, first] { NetSetMode(NetManual, manual.empty() ? first : manual); }}});
        if (mode == NetManual) {
            std::vector<std::tuple<std::wstring, bool, std::function<void()>>> opts;
            for (const auto& a : g_net) if (a.present || a.guid == manual) { const std::wstring g = a.guid; opts.emplace_back(a.name.empty() ? g : a.name, g == manual, [g] { NetSetMode(NetManual, g); }); }
            if (!opts.empty()) chipRow(L"Manual adapter", opts);
        }
        std::vector<std::tuple<std::wstring, bool, std::function<void()>>> per;
        for (int p = 0; p < 4; ++p) per.emplace_back(NetPeriodName(p), period == p, [p] { NetSetPeriod(p); });
        chipRow(L"Totals shown", per);
        auto totals = [&](const NetTotals& t) { return std::wstring(NetPeriodName(period)) + L": \u2193 " + FormatBytes(t.rx) + L"   \u2191 " + FormatBytes(t.tx); };
        auto link = [](uint64_t bps) { return bps >= 1000000000ull ? NA(bps / 1e9, L" Gbps", 1) : NA(bps / 1e6, L" Mbps"); };
        {
            const NetSelection& n = g_netSel;
            RowSpec row; row.icon = L"\xE839"; row.title = n.valid ? n.name : std::wstring(L"No adapter data yet");
            row.desc = totals(n.totals[period]);
            if (n.mode == NetAll) row.desc += L"   (" + std::to_wstring(n.count) + L" connected)";
            else if (n.mode == NetManual && !n.connected) row.desc += L"   The chosen adapter is unavailable; no other adapter is shown in its place.";
            row.right = NetSpeedText(); row.rightColor = n.connected ? col::accent : col::faint;
            row.chips.push_back({n.mode == NetAuto ? L"Automatic" : n.mode == NetAll ? L"All adapters" : L"Manual", col::sub});
            if (n.valid) row.chips.push_back({n.connected ? L"Connected" : L"Disconnected", n.connected ? col::ok : col::warn});
            Row(c, row);
        }
        for (const auto& a : g_net) {
            const bool on = a.present && a.up;
            RowSpec row; row.title = a.name.empty() ? a.guid : a.name;
            row.desc = NetTypeName(a.type);
            if (on && a.linkBps) row.desc += L" \u00B7 " + link(a.linkBps);
            if (on && a.signal >= 0) row.desc += L" \u00B7 signal " + std::to_wstring(a.signal) + L" %";
            row.desc += L" \u00B7 " + totals(a.totals[period]);
            row.right = on ? L"\u2193 " + NetRate(a.down) + L"  \u2191 " + NetRate(a.upRate) : std::wstring(a.present ? L"disconnected" : L"not present");
            row.rightColor = on ? col::accent : col::faint;
            if (a.isDefault) row.chips.push_back({L"Default route", col::ok});
            Row(c, row);
        }
        Para(c, L"Each adapter keeps its own totals, saved once a minute and at Exit. \"All adapters\" adds them when shown, so nothing is counted twice. "
                L"A counter reset or an impossible jump adds nothing; traffic while AnkhCore was not running is only added within the same Windows session.", c.u.f.small, col::faint);
    }
    // ---- alerts
    Section(c, L"Alerts", L"An alert needs the condition to last (brief Turbo Boost spikes are ignored), repeats at most once per cooldown, and never fires for a sensor that is missing or stale.");
    for (auto& rule : g_alerts.rules) {
        const std::wstring id = Utf8ToW(rule.id);
        RowSpec row; row.title = AlertName(rule.id);
        row.desc = AlertRuleDesc(rule);
        if (g_alerts.Active(rule.id)) row.chips.push_back({L"Active", col::err});
        row.toggle = rule.enabled ? 1 : 0;
        row.onToggle = [id, on = rule.enabled] { Cfg().SetBool(L"alert." + id + L".enabled", !on); Cfg().Save(); ApplyMonitorConfig(); };
        Row(c, row);
        const auto [choices, suf] = AlertRuleChoices(rule.id);
        if (!choices.empty()) {
            std::vector<std::tuple<std::wstring, bool, std::function<void()>>> opts;
            for (int t : choices) opts.emplace_back(std::to_wstring(t) + suf, static_cast<int>(rule.threshold) == t,
                                                    [id, t] { Cfg().SetInt(L"alert." + id + L".threshold", t); Cfg().Save(); ApplyMonitorConfig(); });
            chipRow(L"Threshold", opts);
        }
    }
    if (g_alertLog.empty()) Para(c, L"No alerts since AnkhCore started.", c.u.f.small, col::sub);
    for (auto it = g_alertLog.rbegin(); it != g_alertLog.rend(); ++it) { RowSpec row; row.icon = L"\xE7BA"; row.title = it->second; row.desc = it->first; Row(c, row); }
    // ---- graphs & history
    Section(c, L"Graphs & history", L"Kept in memory only (bounded; nothing is written to disk). Gaps show where readings were missing or stale.");
    {
        const int iv = g_store.Interval(), hm = g_store.History() / 60;
        std::vector<std::tuple<std::wstring, bool, std::function<void()>>> a, b;
        for (const auto& [v, t] : kIntervals) a.emplace_back(t, iv == v, [v = v] { Cfg().SetInt(L"monitor.intervalSec", v); Cfg().Save(); ApplyMonitorConfig(); });
        for (const auto& [v, t] : kHistories) b.emplace_back(t, hm == v, [v = v] { Cfg().SetInt(L"monitor.historyMin", v); Cfg().Save(); ApplyMonitorConfig(); });
        chipRow(L"Sampling interval", a);
        chipRow(L"History", b);
        const size_t cap = g_store.Get(SCpuTemp).Capacity();
        Para(c, std::to_wstring(cap) + L" points per series (" + std::to_wstring(static_cast<int>(cap * iv / 60)) + L" min at " + std::to_wstring(iv) + L" s), about " +
                std::to_wstring(static_cast<int>(cap * SeriesCount * sizeof(double) / 1024)) + L" KB. " + std::to_wstring(g_store.Points()) + L" points recorded since start.", c.u.f.small, col::faint);
    }
    {
        const int gap = c.S(12), gw = (c.w - gap) / 2, gh = c.S(130);
        double lo, hi;
        auto pct = [&](Series s, COLORREF cc, const wchar_t* t, int x, int y) { DrawGraph(c, RECT{x, y, x + gw, y + gh}, g_store.Get(s), 0, 100, cc, t, L" %"); };
        auto temp = [&](Series s, COLORREF cc, const wchar_t* t, int x, int y) {
            if (!g_store.Get(s).MinMax(lo, hi)) { lo = 30; hi = 100; }
            DrawGraph(c, RECT{x, y, x + gw, y + gh}, g_store.Get(s), std::min(30.0, lo - 5), std::max(100.0, hi + 2), cc, t, L" \u00B0C");
        };
        temp(SCpuTemp, col::warn, L"CPU temperature", c.x, c.y); pct(SCpuLoad, col::accent, L"CPU usage", c.x + gw + gap, c.y); c.y += gh + gap;
        temp(SGpuTemp, col::warn, L"GPU temperature", c.x, c.y); pct(SGpuLoad, col::accent, L"GPU usage", c.x + gw + gap, c.y); c.y += gh + gap;
        pct(SRam, col::accentDim, L"Memory", c.x, c.y);
        const Ring& nd = g_store.Get(SNetDown);
        double nlo = 0, nhi = 1; nd.MinMax(nlo, nhi);
        DrawGraph(c, RECT{c.x + gw + gap, c.y, c.x + c.w, c.y + gh}, nd, 0, std::max(1.0, nhi), col::ok, L"Download (bytes/s)", L"");
        c.y += gh + gap;
        const Ring& dk = g_store.Get(SDiskIo);
        double dlo = 0, dhi = 1; dk.MinMax(dlo, dhi);
        DrawGraph(c, RECT{c.x, c.y, c.x + c.w, c.y + gh}, dk, 0, std::max(1024.0, dhi), col::accent, L"Disk Read/Write (bytes/s)", L"");
        c.y += gh + gap;
        Ring fan = g_store.Get(SFan); double flo = 0, fhi = 1;
        if (fan.MinMax(flo, fhi)) { DrawGraph(c, RECT{c.x, c.y, c.x + c.w, c.y + gh}, fan, 0, std::max(1000.0, fhi + 200), col::ok, L"Fan (highest, rpm)", L""); c.y += gh + gap; }
    }
}

// ---------------------------------------------------------------- Hardware Information, Network Monitor, Error Logs (2.5 rail pages)
// Read-only views of data AnkhCore already has (the engine frame, Windows, the log file). They add no setting, no sampler and no second
// adapter detection: Network Monitor shows `g_net` / `g_netSel` and changes the same three settings as Monitoring Preferences.
bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_wnd)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
            if (void* p = GlobalLock(g)) { memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr; }
            if (!ok) GlobalFree(g);   // the clipboard owns the memory only after a successful SetClipboardData
        }
    }
    CloseClipboard();
    return ok;
}
std::wstring HwReg(const wchar_t* key, const wchar_t* name) { return TidyHwName(RegStr(HKEY_LOCAL_MACHINE, key, name)); }
// The strings Windows gives once and never changes while it runs (read once, shared by the rows and the HW1 inventory).
struct HwStatics { std::wstring cpuName, sysMan, sysProd, boardMan, boardProd, biosVer, biosDate; };
const HwStatics& HwStaticStrings() {
    static const HwStatics st = [] {
        static const wchar_t* kCpu0 = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"; static const wchar_t* kBios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
        HwStatics h; h.cpuName = HwReg(kCpu0, L"ProcessorNameString"); h.sysMan = HwReg(kBios, L"SystemManufacturer"); h.sysProd = HwReg(kBios, L"SystemProductName");
        h.boardMan = HwReg(kBios, L"BaseBoardManufacturer"); h.boardProd = HwReg(kBios, L"BaseBoardProduct"); h.biosVer = HwReg(kBios, L"BIOSVersion"); h.biosDate = HwReg(kBios, L"BIOSReleaseDate");
        return h; }();
    return st;
}
// M-2.6-HW1: the inputs of the normalized inventory, taken from data AnkhCore already holds (the SensorHub frame, the engine frame, the
// strings above). Nothing here samples anything: calling it twice adds no poller and no second read of a sensor.
HwInputs HwCollectInputs(const Cache& s) {
    const HwStatics& h = HwStaticStrings();
    HwInputs in;
    in.os = WinVerText(s.ver); in.sysManufacturer = h.sysMan; in.sysProduct = h.sysProd; in.boardManufacturer = h.boardMan; in.boardProduct = h.boardProd;
    in.biosVersion = h.biosVer; in.biosDate = h.biosDate; in.cpuName = h.cpuName; in.helperModel = g_read.model;
    in.physCores = g_sys.physCores; in.logicalCores = g_sys.logical;
    in.cpuLoadWindows = g_usage.cpu; in.ramLoadWindows = g_usage.ram; in.ramTotal = g_usage.ramTotal;
    for (const GpuAdapterView& g : g_gpus) { HwInputs::Gpu o; o.name = g.name; o.vram = g.dedicatedTotal; o.util = g.util; o.software = g.software; in.gpus.push_back(o); }
    for (const DriveInfo& d : g_sys.drives) in.drives.push_back({d.letter, d.total, d.free});
    for (const NetAdapterView& a : g_net) if (a.present) ++in.networkAdapters;
    in.readings = g_read; in.helperInstalled = Hub().HelperInstalled();
    return in;
}
std::vector<HwRow> HwRows(const Cache& s) {
    const HwStatics& hs = HwStaticStrings();
    const std::wstring &cpuName = hs.cpuName, &sysMan = hs.sysMan, &sysProd = hs.sysProd, &boardMan = hs.boardMan, &boardProd = hs.boardProd, &biosVer = hs.biosVer, &biosDate = hs.biosDate;
    const Readings& r = g_read; std::vector<HwRow> v;
    auto add = [&](const wchar_t* sec, const std::wstring& k, const std::wstring& val) { v.push_back({sec, k, val.empty() ? std::wstring(L"not reported") : val}); };
    auto join = [](const std::wstring& a, const std::wstring& b) { return a.empty() ? b : b.empty() ? a : a + L" " + b; };
    add(L"System", L"Operating system", WinVerText(s.ver));
    add(L"System", L"Device", join(sysMan, sysProd).empty() ? r.model : join(sysMan, sysProd));
    add(L"System", L"Motherboard", join(boardMan, boardProd));
    add(L"System", L"BIOS", biosVer.empty() ? std::wstring() : biosVer + (biosDate.empty() ? std::wstring() : L" (" + biosDate + L")"));
    add(L"System", L"Sensor source", !Hub().HelperInstalled() ? L"monitoring helper not installed" : r.present ? L"AnkhSensors (LibreHardwareMonitor), live" : L"AnkhSensors installed, not reporting");
    add(L"Processor", L"Name", cpuName);
    add(L"Processor", L"Cores / threads", g_sys.logical ? std::to_wstring(g_sys.physCores) + L" cores · " + std::to_wstring(g_sys.logical) + L" threads" : std::wstring(L"no data yet"));
    add(L"Processor", L"Usage", NA(EffectiveCpuLoad(g_usage.cpu, r), L" %"));
    add(L"Processor", L"Temperature", NA(DispTemp(HelperReading(r, r.cpuTemp)), TempSuffix()));
    add(L"Processor", L"Clock", NA(HelperReading(r, r.cpuClock), L" MHz"));
    add(L"Processor", L"Power", NA(HelperReading(r, r.cpuPower), L" W"));
    int gpuN = 0;
    for (const GpuAdapterView& g : g_gpus) if (!g.software) {
        ++gpuN;
        add(L"Graphics", L"Adapter " + std::to_wstring(gpuN), g.name + (g.dedicatedTotal >= (512ull << 20) ? L" · " + FormatBytes(g.dedicatedTotal) + L" VRAM" : std::wstring()) + L" · " + NA(g.util, L" %"));
    }
    if (!gpuN) add(L"Graphics", L"Adapters", L"no data yet");
    add(L"Memory", L"Installed", g_usage.ramTotal ? FormatBytes(g_usage.ramTotal) : std::wstring(L"no data yet"));
    add(L"Memory", L"In use", g_usage.ramTotal ? FormatBytes(g_usage.ramUsed) + L" (" + NA(g_usage.ram, L" %") + L")" : std::wstring(L"no data yet"));
    if (g_sys.logical) {
        add(L"Memory", L"Available / cached", FormatBytes(g_sys.memAvail) + L" / " + FormatBytes(g_sys.memCached));
        add(L"Memory", L"Committed", FormatBytes(g_sys.commitUsed) + L" of " + FormatBytes(g_sys.commitLimit));
    }
    for (const DriveInfo& d : g_sys.drives) add(L"Storage", L"Drive", DriveText(d.letter, d.total, d.free));
    if (g_sys.drives.empty()) add(L"Storage", L"Drives", L"no data yet");
    if (HelperReading(r, r.diskTemp) >= 0) add(L"Storage", L"Disk temperature", NA(DispTemp(HelperReading(r, r.diskTemp)), TempSuffix()));
    int netN = 0;
    for (const NetAdapterView& a : g_net) if (a.present) { ++netN; add(L"Network adapters", a.name.empty() ? a.guid : a.name, std::wstring(NetStateText(a)) + L" · " + NetAdapterLine(a)); }
    if (!netN) add(L"Network adapters", L"Adapters", L"no data yet");
    if (r.fans.empty()) add(L"Fans", L"Fans", L"this hardware exposes no fan sensor");
    else for (const auto& f : r.fans) add(L"Fans", f.name.empty() ? L"Fan" : f.name, f.rpm < 0 ? std::wstring(L"not exposed") : std::to_wstring(static_cast<int>(f.rpm)) + L" rpm");
    return v;
}
const wchar_t* HwSectionIcon(const std::wstring& sec) {
    return sec == L"System" ? L"\xE770" : sec == L"Processor" ? L"\xE950" : sec == L"Graphics" ? L"\xF211" : sec == L"Memory" ? GlyphMemory() : sec == L"Storage" ? L"\xEDA2" : sec == L"Network adapters" ? L"\xE839" : L"\xE9CA";
}
void PageHardware(Ctx& c, const Cache& s) {
    const std::vector<HwRow> rows = HwRows(s);
    {
        RowSpec row; row.icon = L"\xE950"; row.title = L"This PC";
        row.desc = L"Read-only. The values come from Windows and from the sensor helper; nothing on this page changes the hardware.";
        row.buttons = {{L"Copy hardware summary", Btn::Secondary, true, [rows] {
            if (TestGuard(L"Copy hardware summary")) return;
            if (CopyTextToClipboard(HwSummaryText(rows))) LogOk(L"Hardware summary copied to the clipboard."); else LogE(L"The hardware summary could not be copied to the clipboard."); }}};
        Row(c, row);
    }
    if (!g_read.present && !g_sys.logical && g_gpus.empty() && g_net.empty())
        Para(c, L"Waiting for the first data from the monitoring engine. The system rows below come straight from Windows; the others fill in within a few seconds.", c.u.f.small, col::sub);
    else if (!Hub().HelperInstalled())
        Para(c, L"The monitoring helper is not installed: temperatures, clocks, power and fan speeds show as not reported. Everything else comes from Windows.", c.u.f.small, col::sub);
    for (size_t i = 0; i < rows.size();) {
        std::vector<std::pair<std::wstring, std::wstring>> rr; size_t j = i;
        while (j < rows.size() && rows[j].section == rows[i].section) { rr.push_back({rows[j].label, rows[j].value}); ++j; }
        int y = c.y; Readout(c, rows[i].section, HwSectionIcon(rows[i].section), rr, c.x, c.w, y); c.y = y;
        i = j;
    }
    Para(c, L"\"not reported\" and \"N/A\" mean this hardware or driver does not expose that value to Windows or to the sensor helper. Drive health (SMART) is on Storage Analyzer.", c.u.f.small, col::faint);
    {   // M-2.6-HW1: a fan that can be READ is not a fan that can be CONTROLLED; the two are classified separately here
        const HwInventory inv = BuildHwInventory(HwCollectInputs(s));
        Section(c, L"Capabilities", L"Telemetry (what can be read) and control (what could be set) are separate. Fan control is offered only on the Fan Control page, where a usable control exists; a control the sensor helper merely reports is shown as unverified here.");
        for (const HwFan& f : inv.fans) {
            RowSpec r; r.icon = L"\xE9CA"; r.title = f.name; r.desc = f.note;
            r.right = f.rpm >= 0 ? std::to_wstring(static_cast<int>(f.rpm)) + L" rpm" : std::wstring(L"no RPM");
            r.chips.push_back({HwTelemetryLabel(f.telemetry), f.telemetry == HwTelemetry::Live ? col::ok : col::faint});
            r.chips.push_back({HwControlLabel(f.control), f.control == HwControl::Verified ? col::ok : f.control == HwControl::ReportedUnverified ? col::warn : col::faint});
            Row(c, r);
        }
        for (const std::wstring& l : inv.caps.limitations) Para(c, l, c.u.f.small, col::sub);
        Buttons(c, {{L"Copy hardware report (JSON)", Btn::Secondary, true, [] {
            if (TestGuard(L"Copy hardware report")) return;
            const std::wstring text = Utf8ToW(HwInventoryJson(BuildHwInventory(HwCollectInputs(Snapshot()))));
            if (CopyTextToClipboard(text)) LogOk(L"Hardware report (JSON) copied to the clipboard."); else LogE(L"The hardware report could not be copied to the clipboard."); }},
                    {L"Save device profile", Btn::Secondary, true, [] {   // M-2.6-CFG1: a DESCRIPTIVE profile (identity, provenance, capabilities); it holds no control data and verifies nothing
            if (TestGuard(L"Save device profile")) return;
            const HwInventory inv = BuildHwInventory(HwCollectInputs(Snapshot()));
            if (!inv.device.complete) { LogE(L"Device profile not saved: the device model could not be read, so it has no identity."); return; }
            const DeviceProfile prof = DpGenerate(inv, AcbUtcStampNow(), "AnkhCore " + WToUtf8(kVersion));
            const std::wstring dir = DataDir() + L"\\Profiles"; std::wstring why;
            if (DpSave(dir, prof, why)) LogOk(L"Device profile saved: " + DpPathFor(dir, prof.identity) + L" (" + Utf8ToW(prof.compatibility) + L"; nothing is verified)."); else LogE(L"Device profile not saved: " + why); }}});
    }
}
void PageNetwork(Ctx& c, const Cache&) {
    const NetSelection& n = g_netSel; const int mode = n.mode, period = NetPeriodCfg();
    if (!n.valid) StatePanel(c, L"\xE839", L"No network data yet", L"The monitoring engine has not reported any adapter. It starts with AnkhCore; if this stays, see Startup & Background.");
    {
        RowSpec row; row.icon = L"\xE839"; row.title = n.valid ? n.name : std::wstring(L"No adapter data yet");
        row.desc = n.valid ? std::wstring(NetPeriodName(period)) + L": ↓ " + FormatBytes(n.totals[period].rx) + L"   ↑ " + FormatBytes(n.totals[period].tx) : std::wstring(L"Nothing to show yet.");
        if (n.mode == NetAll) row.desc += L"   (" + std::to_wstring(n.count) + L" connected)";
        else if (n.mode == NetManual && !n.connected) row.desc += L"   The chosen adapter is unavailable; no other adapter is shown in its place.";
        else if (n.mode == NetAuto && n.valid && !n.connected) row.desc += L"   No adapter has a default route right now.";
        if (n.valid && n.mode != NetAll && n.connected) { row.desc += L"   " + std::wstring(NetTypeName(n.type)); if (n.linkBps) row.desc += L" · " + NetLinkText(n.linkBps); if (n.signal >= 0) row.desc += L" · signal " + std::to_wstring(n.signal) + L" %"; }
        row.right = NetSpeedText(); row.rightColor = n.connected ? col::accent : col::faint;
        row.chips.push_back({mode == NetAuto ? L"Automatic" : mode == NetAll ? L"All adapters" : L"Manual", col::sub});
        if (n.valid) row.chips.push_back({n.connected ? L"Connected" : L"Disconnected", n.connected ? col::ok : col::warn});
        Row(c, row);
    }
    Section(c, L"Adapter", L"The same setting as in Monitoring Preferences; it also drives the Sidebar and the Mini Monitor.");
    {
        const std::wstring manual = Cfg().Str(L"sidebar.netAdapter", L"");
        std::wstring first; for (const auto& a : g_net) if (a.present && (first.empty() || a.isDefault)) first = a.guid;
        Chips(c, L"Adapter", {{L"Automatic", mode == NetAuto, [] { NetSetMode(NetAuto, L""); }}, {L"All adapters", mode == NetAll, [] { NetSetMode(NetAll, L""); }},
                              {L"Manual", mode == NetManual, [manual, first] { NetSetMode(NetManual, manual.empty() ? first : manual); }}});
        if (mode == NetManual) {
            std::vector<ChipOpt> opts;
            for (const auto& a : g_net) if (a.present || a.guid == manual) { const std::wstring g = a.guid; opts.emplace_back(a.name.empty() ? g : a.name, g == manual, [g] { NetSetMode(NetManual, g); }); }
            if (!opts.empty()) Chips(c, L"Manual adapter", opts);
        }
        std::vector<ChipOpt> per; for (int p = 0; p < 4; ++p) per.emplace_back(NetPeriodName(p), period == p, [p] { NetSetPeriod(p); });
        Chips(c, L"Totals shown", per);
    }
    Section(c, L"History", L"Kept in memory only (the length is set in Monitoring Preferences). Gaps show where no reading arrived.");
    {
        const int gap = c.S(12), gw = (c.w - gap) / 2, gh = c.S(130);
        const Ring& dn = g_store.Get(SNetDown); const Ring& up = g_store.Get(SNetUp);
        double lo = 0, hi = 1, hi2 = 1; dn.MinMax(lo, hi); up.MinMax(lo, hi2);
        DrawGraph(c, RECT{c.x, c.y, c.x + gw, c.y + gh}, dn, 0, std::max(1024.0, hi), col::ok, L"Download (bytes/s)", L"");
        DrawGraph(c, RECT{c.x + gw + gap, c.y, c.x + c.w, c.y + gh}, up, 0, std::max(1024.0, hi2), col::accent, L"Upload (bytes/s)", L"");
        c.y += gh + gap;
    }
    Section(c, L"Totals", L"For what the Adapter setting selects. A counter reset or an impossible jump adds nothing.");
    {
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        for (int p = 0; p < 4; ++p) rows.push_back({NetPeriodName(p), n.valid ? L"↓ " + FormatBytes(n.totals[p].rx) + L"    ↑ " + FormatBytes(n.totals[p].tx) : std::wstring(L"no data")});
        int y = c.y; Readout(c, n.valid ? n.name : std::wstring(L"Totals"), L"\xE839", rows, c.x, c.w, y); c.y = y;
    }
    Section(c, L"Adapters", L"Every adapter Windows lists, with its own counters. Wi-Fi signal and link speed show while the link is up.");
    if (g_net.empty()) StatePanel(c, L"\xE839", L"No adapters reported", L"Windows lists no network adapter, or the engine has not sent one yet.");
    for (const auto& a : g_net) {
        const bool on = a.present && a.up;
        RowSpec row; row.title = a.name.empty() ? a.guid : a.name;
        row.desc = NetAdapterLine(a) + L" · " + NetPeriodName(period) + L": ↓ " + FormatBytes(a.totals[period].rx) + L"  ↑ " + FormatBytes(a.totals[period].tx);
        row.right = on ? L"↓ " + NetRate(a.down) + L"  ↑ " + NetRate(a.upRate) : std::wstring(NetStateText(a)); row.rightColor = on ? col::accent : col::faint;
        row.chips.push_back({NetStateText(a), on ? col::ok : col::faint});
        if (a.isDefault) row.chips.push_back({L"Default route", col::ok});
        Row(c, row);
    }
}
// The persisted log, read on a worker thread (up to 2 MB from the end of the file) and shown filtered. View choices are not saved.
struct ErrLogView {
    std::mutex mx; std::vector<ErrLogEntry> all; bool loading = false, loaded = false, ok = true, cut = false; uint64_t fileSize = 0;
    std::atomic<bool> busy{false}; std::thread th;
    bool errorsOnly = false, todayOnly = true;   // view state
    void Refresh() {
        if (busy.exchange(true)) return;
        if (th.joinable()) th.join();
        { std::lock_guard<std::mutex> lk(mx); loading = true; }
        th = std::thread([this] {
            std::wstring text; uint64_t size = 0; bool cutR = false;
            const bool rd = ReadLogTail(LogPath(), 2u << 20, text, size, cutR);
            std::vector<ErrLogEntry> parsed; if (rd) parsed = ParseLogText(text);
            { std::lock_guard<std::mutex> lk(mx); all = std::move(parsed); ok = rd; cut = cutR; fileSize = size; loaded = true; loading = false; }
            busy = false;
            if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
        });
    }
    ~ErrLogView() { if (th.joinable()) th.join(); }
};
ErrLogView g_errLog;
void PageErrLog(Ctx& c, const Cache&) {
    ErrLogView& v = g_errLog;
    Buttons(c, {{L"Refresh", Btn::Primary, !v.busy.load(), [] { g_errLog.Refresh(); }},
                {L"Open log folder", Btn::Secondary, true, [] { if (TestGuard(L"Open log folder")) return; ShellExecuteW(nullptr, L"open", (DataDir() + L"\\Logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}});
    Chips(c, L"Show", {{L"Warnings and errors", !v.errorsOnly, [] { g_errLog.errorsOnly = false; }}, {L"Errors only", v.errorsOnly, [] { g_errLog.errorsOnly = true; }}});
    Chips(c, L"Period", {{L"Today", v.todayOnly, [] { g_errLog.todayOnly = true; }}, {L"All recorded", !v.todayOnly, [] { g_errLog.todayOnly = false; }}});
    std::vector<ErrLogEntry> shown; LogCounts cnt; bool loaded, loading, ok, cut; uint64_t size;
    const std::wstring today = NowText(true).substr(0, 10), day = v.todayOnly ? today : std::wstring();
    { std::lock_guard<std::mutex> lk(v.mx); loaded = v.loaded; loading = v.loading; ok = v.ok; cut = v.cut; size = v.fileSize;
      if (loaded && ok) { shown = FilterLogEntries(v.all, v.errorsOnly, day, 300); cnt = CountLogEntries(v.all, day); } }
    if (!loaded && !loading) { v.Refresh(); loading = true; }   // first visit of the session (NavGo starts it too)
    if (loading && !loaded) { Para(c, L"Reading the log file\x2026", c.u.f.small, col::sub); return; }
    if (!ok) { StatePanel(c, L"\xE783", L"The log file could not be read", LogPath() + L" is missing or not readable. Operation History still shows this session.", col::warn); return; }
    Para(c, std::to_wstring(cnt.err) + L" error(s), " + std::to_wstring(cnt.warn) + L" warning(s) " + (v.todayOnly ? L"today" : L"in the part of the log that was read") +
            L" · " + std::to_wstring(cnt.Total()) + L" entries · log file " + FormatBytes(size) +
            (cut ? L" · only the last 2 MB were read" : L"") + (loading ? L" · refreshing\x2026" : L""), c.u.f.small, col::sub);
    if (shown.empty()) {
        const std::wstring what = v.errorsOnly ? L"an error" : L"a warning or an error";
        StatePanel(c, L"\xE73E", v.errorsOnly ? L"No errors" : L"No warnings or errors",
                   v.todayOnly ? L"Nothing was recorded as " + what + L" today. Choose All recorded to look further back." : L"Nothing was recorded as " + what + L" in the part of the log that was read.", col::ok);
        return;
    }
    const int lh = c.S(26);
    RECT box{c.x, c.y, c.x + c.w, c.y + static_cast<int>(shown.size()) * lh + c.S(20)};
    Round(c.dc, box, c.S(28), col::panel, col::border);
    int y = c.y + c.S(10);
    for (const auto& e : shown) {
        RECT r{c.x + c.S(14), y, c.x + c.w - c.S(14), y + lh};
        if (c.Visible(r)) {
            const COLORREF lc = e.level == Lv::Err ? col::err : col::warn;
            Text(c.dc, e.date + L" " + e.time, RECT{r.left, r.top, r.left + c.S(150), r.bottom}, c.u.f.small, col::faint, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, e.level == Lv::Err ? L"ERROR" : L"WARN", RECT{r.left + c.S(150), r.top, r.left + c.S(210), r.bottom}, c.u.f.small, lc, DT_VCENTER | DT_SINGLELINE);
            Text(c.dc, e.text, RECT{r.left + c.S(210), r.top, r.right, r.bottom}, c.u.f.small, col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        y += lh;
    }
    c.y = box.bottom + c.S(10);
    if (cnt.err + cnt.warn > static_cast<int>(shown.size())) Para(c, L"Showing the newest " + std::to_wstring(shown.size()) + L". Export the full log from Operation History.", c.u.f.small, col::faint);
}
void PageAbout(Ctx& c, const Cache& s) {
    RECT top{c.x, c.y, c.x + c.w, c.y + c.S(150)};
    Round(c.dc, top, c.S(28), col::panel, col::border);
    if (!g_brandLogoAbout.p.empty() && c.dc.d2d())   // the unified logo, on the card with nothing drawn behind it
        DrawPixelsPart(c.dc, &g_brandLogoAbout, g_brandLogoAbout.p.data(), g_brandLogoAbout.w, g_brandLogoAbout.h, D2D1::RectF(static_cast<float>(c.x + c.S(24)), static_cast<float>(c.y + c.S(19)), static_cast<float>(c.x + c.S(136)), static_cast<float>(c.y + c.S(131))),
                       D2D1::RectF(0, 0, static_cast<float>(g_brandLogoAbout.w), static_cast<float>(g_brandLogoAbout.h)), 1.f);
    else DrawIconAt(c.dc, g_ankhBig, c.x + c.S(24), c.y + c.S(19), c.S(112));
    Text(c.dc, kProductFull, RECT{c.x + c.S(156), c.y + c.S(26), c.x + c.w, c.y + c.S(66)}, c.u.f.title, col::text);
    const std::wstring verText = kIsBetaBuild ? FormatBetaVersionLabel(g_betaDaysRemaining, false) : std::wstring(kVersionLabel);
    Text(c.dc, L"Version " + verText + L" (" + kVersion + L")  \x00B7  Windows Control Center", RECT{c.x + c.S(156), c.y + c.S(70), c.x + c.w, c.y + c.S(92)}, c.u.f.text, col::accent);
    Text(c.dc, L"Developer: As3aD", RECT{c.x + c.S(156), c.y + c.S(98), c.x + c.w, c.y + c.S(120)}, c.u.f.text, col::sub);
    c.y += c.S(166);
    std::vector<BtnSpec> links;
    for (const auto& l : kLinks) { const std::wstring url = l.url; links.push_back({l.text, Btn::Secondary, true, [url] { if (kTestBuild && g_selftest) { LogI(L"[selftest] skipped link: " + url); return; } ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}); }
    Buttons(c, links);
    Section(c, L"This PC");
    Para(c, WinVerText(s.ver), c.u.f.text, col::text);
    Section(c, L"Credits");
    Para(c, L"AnkhCore, Anubis \x2014 Windows Animations, the As3aD theme, settings and redesigns: As3aD.\n"
            L"Windhawk: Ramen Software (windhawk.net).  Windows Animations (base of Anubis): ReDrag (MIT License).\n"
            L"Windhawk mods by their authors.  SecureUxTheme: namazso (LGPL-2.1).  BIB theme: niivu DESIGNS.\n"
            L"DIM Prismatic cursors: original author.  Icon packs: HUD Evolution (Mr GRiM), Tech Icons (Dark Knight), "
            L"Magnum Dark (Razorsedge & Moonnique), Sweet Paranoia and Big Sur 2 (niivu).", c.u.f.small, col::sub);
    {
        static bool s_ackOpen = false;
        RowSpec ackRow; ackRow.icon = L"\xE946"; ackRow.title = L"Acknowledgements & Credits";
        ackRow.desc = L"Third-party tools, open-source libraries, licenses, and project links used by AnkhCore.";
        ackRow.chips.push_back({std::to_wstring(AcknowledgementsList().size()) + L" items", col::accent});
        ackRow.buttons = {{s_ackOpen ? L"Close" : L"Open", Btn::Secondary, true, [] {
            s_ackOpen = !s_ackOpen;
            InvalidateRect(g_wnd, nullptr, FALSE);
        }}};
        Row(c, ackRow);
        if (s_ackOpen) {
            for (const auto& item : AcknowledgementsList()) {
                RowSpec ir; ir.icon = L"\xE8F1"; ir.title = item.name;
                ir.desc = item.author + L"\n" + item.note;
                ir.chips.push_back({item.license, col::accent});
                if (!item.url.empty()) {
                    const std::wstring url = item.url;
                    ir.buttons = {{L"Project link", Btn::Ghost, true, [url] {
                        if (kTestBuild && g_selftest) { LogI(L"[selftest] skipped link: " + url); return; }
                        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }}};
                }
                Row(c, ir);
            }
        }
    }
    {
        wchar_t fb[160]; swprintf(fb, 160, L"Renderer: %s  \x00B7  animated frames avg %.1f ms, p95 %.1f ms (budget 12 ms)  \x00B7  effects level %d", g_surfOn ? L"Direct2D" : L"GDI", g_frameStats.Avg(), g_frameStats.P95(), g_gov.level);
        Section(c, L"Graphics"); Para(c, fb, c.u.f.small, col::sub);
    }
    Section(c, L"Uninstall");
    Buttons(c, {{L"Uninstall AnkhCore", Btn::Danger, true, [] {
        if (TestGuard(L"Uninstall AnkhCore")) return;
        ShellExecuteW(nullptr, nullptr, AppExe().c_str(), L"--uninstall", nullptr, SW_SHOWNORMAL); }}});
}

// ---------------------------------------------------------------- window
// ---- navigation layout, activation, animation, persistence
bool g_navCompact = false;   // the 64 DIP icon rail
std::wstring g_navTipShown;   // what the last paint drew: the compact rail's tooltip text, its place, the rail-mode switch and the status block
RECT g_navTipRect{}, g_navToggleRect{}, g_navStatusRect{};
int SideW() { return g_ui.S(g_navCompact ? 64 : kSide); }
int NavTop() { return g_ui.S(g_navCompact ? 72 : kNavTop); }
int ClientH();
// The rail's viewport: from NavTop() down to the status block (the compact rail keeps its expand button above that block).
int NavViewH() { return ClientH() - g_ui.S(g_navCompact ? 92 : 64) - NavTop(); }
// settled: the layout once the expand animation has finished (what scrolling aims for)
std::vector<NavRow> NavLayout(bool settled = false) {
    std::vector<NavRow> rows;
    int y = NavTop() - g_navScroll;
    const int l = g_ui.S(12), r = SideW() - g_ui.S(12);
    for (size_t s = 0; s < kNav.size(); ++s) {
        rows.push_back({RECT{l, y, r, y + g_ui.S(kNavRowH)}, static_cast<int>(s), kNav[s].pages[0], false});
        y += g_ui.S(kNavStep);
        if (kNav[s].pages.size() > 1) {
            const int full = static_cast<int>(kNav[s].pages.size()) * g_ui.S(kNavChildStep);
            const int vis = static_cast<int>(full * (settled ? (static_cast<int>(s) == g_navOpen ? 1.f : 0.f) : g_navAnim[s]) + 0.5f);
            for (size_t c = 0; c < kNav[s].pages.size(); ++c) {
                const int off = static_cast<int>(c) * g_ui.S(kNavChildStep);
                if (off + g_ui.S(kNavChildH) > vis) break;   // still hidden by the expand animation
                rows.push_back({RECT{l + (g_navCompact ? 0 : g_ui.S(10)), y - g_ui.S(4) + off, r, y - g_ui.S(4) + off + g_ui.S(kNavChildH)}, static_cast<int>(s), kNav[s].pages[c], true});
            }
            y += vis;
        }
    }
    return rows;
}
int NavContentHeight(bool settled = false) { const auto rows = NavLayout(settled); return rows.empty() ? 0 : rows.back().r.bottom + g_navScroll - NavTop(); }
// Keeps the scroll offset in range: a collapsed section, a taller window or the other rail mode can leave it past the
// end. While a section is still opening, the height it will reach counts, so a reveal is not undone mid-animation.
void NavClampScroll() { g_navScroll = NavScrollClamp(g_navScroll, std::max(NavContentHeight(false), NavContentHeight(true)), NavViewH()); }
// Scrolls the rail so that the page's row (its section's row while the section is closed) is fully visible.
void NavReveal(int page) {
    if (NavParent(page) >= 0) page = NavParent(page);
    const NavRow* hit = nullptr;
    const auto rows = NavLayout(true);
    for (const NavRow& r : rows) if (r.page == page && r.child) hit = &r;
    if (!hit) for (const NavRow& r : rows) if (!r.child && r.section == NavSectionOf(page)) hit = &r;
    if (!hit) return;
    const int base = NavTop() - g_navScroll;
    g_navScroll = NavScrollReveal(g_navScroll, hit->r.top - base, hit->r.bottom - base, NavContentHeight(true), NavViewH());
}
bool ReducedMotion() { BOOL anim = TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0); return !anim; }
void NavSave() { Cfg().SetInt(L"nav.page", g_page); Cfg().SetInt(L"nav.open", g_navOpen); Cfg().SetInt(L"nav.layout", kNavLayoutVersion); Cfg().Save(); }
void FindingDo(FindingAct a) {
    switch (a) {
    case FindingAct::CreateRestorePoint:
        RunOp(L"Create a restore point", [] { std::wstring e; return CreateRestorePoint(L"AnkhCore " + NowText(true), e); }, false, true);
        AddToast(L"Creating a restore point. Press Scan again afterwards to refresh the result.", col::accent, 0);
        break;
    case FindingAct::StartWindhawk:
        RunOp(L"Start Windhawk", [] { return ServiceControlWait(L"Windhawk", true); }, false, true);
        break;
    case FindingAct::OpenSystemCare: NavGo(PSystemCare); break;
    case FindingAct::OpenStorage: NavGo(PStorage); break;
    case FindingAct::OpenProcesses: NavGo(PProcMgr); break;
    case FindingAct::OpenSecurity: if (!TestGuard(L"Open Windows Security")) ShellExecuteW(nullptr, L"open", L"windowsdefender:", nullptr, nullptr, SW_SHOWNORMAL); break;
    case FindingAct::OpenUac: if (!TestGuard(L"Open UAC settings")) ShellExecuteW(nullptr, L"open", (SysDir() + L"\UserAccountControlSettings.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    default: break;
    }
}
void NavGo(int page) {
    if (g_page != page) g_anim.Start(kPageFadeId, 0.f, 1.f, g_anim.level == MotionFull && g_gov.GlassEffectsOn() ? 560 : 240, Ease::Linear);
    const int s = NavSectionOf(page);
    g_navOpen = kNav[s].pages.size() > 1 ? s : -1;   // opening one section closes the others
    if (g_anim.level != MotionFull) for (size_t i = 0; i < kNav.size(); ++i) g_navAnim[i] = static_cast<int>(i) == g_navOpen ? 1.f : 0.f;
    if (g_page != page) {
        g_page = page;
        if (page == PRecovery || page == PDashboard) RefreshAsync(page == PRecovery);
        if (page == PAppIcons) IconPageEnter();
        if (page == PErrLog) g_errLog.Refresh();   // the log file is read once per visit, off the UI thread
        if (page == PRecHistory || page == PRecovery) { if (page == PRecHistory) { g_recShown = 25; g_recOpen.clear(); } RecRefreshAsync(); }   // FT-RH: the Recovery folder is listed once per visit, off the UI thread
        if (page == PBackup) { g_backupList.RefreshAsync(AcbDefaultBackupDir(), 12, [] { if (g_wnd && !g_quitting) PostMessageW(g_wnd, WM_ICONUI, 0, 0); }); }
        if (page == PAppsUpdates) UpdUi().RefreshPending();
    }
    NavReveal(page);
    NavSave();
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void NavOpenSettings() {
    if (g_page != PSettings && NavParent(g_page) != PSettings) g_settingsFrom = g_page;   // Back returns to where the person was
    NavGo(PSettings);
}
void NavActivate(const NavRow& row) {
    const NavSection& sec = kNav[row.section];
    if (row.child || sec.pages.size() == 1) { NavGo(row.page); return; }
    if (g_navOpen == row.section) {   // collapse; the current page stays selected
        g_navOpen = -1;
        if (g_anim.level != MotionFull) g_navAnim[row.section] = 0.f;
        NavSave();
    } else if (NavSectionOf(g_page) == row.section) {
        g_navOpen = row.section;
        if (g_anim.level != MotionFull) g_navAnim[row.section] = 1.f;
        NavSave();
    } else NavGo(sec.pages[0]);
    InvalidateRect(g_wnd, nullptr, FALSE);
}
// called from the 33 ms timer; returns true while something is still moving
bool NavAnimate() {
    bool moving = false;
    for (size_t i = 0; i < kNav.size(); ++i) {
        const float target = static_cast<int>(i) == g_navOpen ? 1.f : 0.f;
        if (g_navAnim[i] == target) continue;
        const float step = 0.22f;
        g_navAnim[i] = g_navAnim[i] < target ? std::min(target, g_navAnim[i] + step) : std::max(target, g_navAnim[i] - step);
        moving = true;
    }
    return moving;
}
// keyboard: Up/Down/Home/End move through every page in navigation order, Left/Right collapse/expand
bool NavKey(WPARAM key) {
    const auto order = NavOrder();
    const int cur = NavParent(g_page) >= 0 ? NavParent(g_page) : g_page;   // on a sub-page the arrows move from its parent
    int at = static_cast<int>(std::find(order.begin(), order.end(), cur) - order.begin());
    const int n = static_cast<int>(order.size());
    if (at >= n) at = 0;   // a top-bar page (Settings) is not in the rail order: the arrows start from the top
    switch (key) {
        case VK_UP:   NavGo(order[(at + n - 1) % n]); break;
        case VK_DOWN: NavGo(order[(at + 1) % n]); break;
        case VK_HOME: NavGo(order.front()); break;
        case VK_END:  NavGo(order.back()); break;
        case VK_BACK:  if (g_page == PSettings) { NavGo(g_settingsFrom); break; } if (NavParent(g_page) < 0) return false; NavGo(NavParent(g_page)); break;   // Back from a sub-page; from Settings to where it was opened
        case VK_LEFT:  if (g_navOpen >= 0) { g_navOpen = -1; NavReveal(g_page); NavSave(); } break;
        case VK_RIGHT: { const int s = NavSectionOf(g_page); if (kNav[s].pages.size() > 1) { g_navOpen = s; NavReveal(g_page); NavSave(); } break; }
        default: return false;
    }
    g_navKeyboard = true;
    InvalidateRect(g_wnd, nullptr, FALSE);
    return true;
}
void NavRestore() {
    g_page = NavPageOrDashboard(Cfg().Int(L"nav.page", PDashboard, 0, PageCount - 1));
    g_navCompact = Cfg().Bool(L"nav.compact", false);
    // 2.7 J1: the saved "open section" is an index into the rail; a value saved by the 2.5/2.6 rail (no nav.layout) is migrated
    g_navOpen = NavMigrateOpen(Cfg().Int(L"nav.open", -1, -1, 99), Cfg().Int(L"nav.layout", 1, 1, 99));
    if (g_navOpen >= 0) g_navAnim[g_navOpen] = 1.f;
}

int g_cw = 0, g_ch = 0;   // client size in pixels (the window is resizable)
int ClientW() { return g_cw ? g_cw : g_ui.S(kW); }
int ClientH() { return g_ch ? g_ch : g_ui.S(kH); }
RECT ContentRect() { return RECT{SideW(), g_ui.S(kHeader) + PinnedBarH(), ClientW(), ClientH() - g_ui.S(kFooter)}; }   // 2.8.3: a pinned bar (System Care) sits between the header and the scrolling content
uint64_t g_mouseMoves = 0;   // real mouse movement over the window (each one repaints; the self-test tells them from idle repaints)
uint64_t g_paints = 0;   // main-window paints (the self-test checks that an idle window doesn't repaint at 30 fps)
struct PaintTarget { Dc dc; RECT cr; };
void PaintScene(const PaintTarget& p);
// FT-TOAST: see src/toast_queue.h. `key` is the identity of the operation a toast reports (0: none); the same key never shows twice.
ToastQueue g_toastQ; std::vector<RECT> g_toastRects;   // the rectangles drawn in the last paint (the self-test checks them)
void ToastSchedule(HWND h) {   // one timer, set for the next expiry; none while nothing shows
    const uint64_t left = g_toastQ.NextDeadlineMs(GetTickCount64());
    if (!left) KillTimer(h, 5); else SetTimer(h, 5, static_cast<UINT>(std::max<uint64_t>(30, left)), nullptr);
}
void AddToast(const std::wstring& text, COLORREF color, uint64_t key) {
    g_toastQ.Add(text, color, key, GetTickCount64());
    ToastSchedule(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
}
void ProbePage(Ctx& c) {   // self-test only: one glass card, the rest bare tint, so the backdrop can be measured
    RECT card{c.x, c.clip.top + c.S(20), c.x + c.S(360), c.clip.top + c.S(300)};
    Round(c.dc, card, c.S(28), col::panel, col::border);
}
void PageGallery(Ctx& c) {   // self-test only: every component on one page
    Section(c, L"Buttons");
    Buttons(c, {{L"Primary", Btn::Primary, true, nullptr}, {L"Secondary", Btn::Secondary, true, nullptr}, {L"Ghost", Btn::Ghost, true, nullptr}, {L"Danger", Btn::Danger, true, nullptr}, {L"Disabled", Btn::Primary, false, nullptr}});
    Section(c, L"Toggles, slider, chips");
    DrawToggle(c.u, c.dc, c.x, c.y, true, true, false); DrawToggle(c.u, c.dc, c.x + c.S(60), c.y, false, true, false); DrawToggle(c.u, c.dc, c.x + c.S(120), c.y, true, false, false);
    DrawCheck(c.u, c.dc, c.x + c.S(180), c.y, true, true, false); Chip(c, c.x + c.S(220), c.y, L"Live", col::ok); Chip(c, c.x + c.S(280), c.y, L"stale", col::faint); Chip(c, c.x + c.S(340), c.y, L"Hot", col::err);
    c.y += c.S(40);
    Slider(c, L"Slider", 68, 45, 92, L"%", [](int) {}, [] {});
    Section(c, L"Gauges and graphs");
    GaugeRing(c.dc, c.x + c.S(60), c.y + c.S(60), c.S(50), c.S(8), 72, col::accent); GaugeRing(c.dc, c.x + c.S(190), c.y + c.S(60), c.S(50), c.S(8), -1, col::faint);
    std::vector<float> pts; for (int i = 0; i < 60; ++i) pts.push_back(i > 26 && i < 34 ? std::numeric_limits<float>::quiet_NaN() : .5f + .35f * sinf(i * .3f));
    RECT g{c.x + c.S(280), c.y + c.S(10), c.x + c.w, c.y + c.S(110)}; Round(c.dc, g, c.S(28), col::panel, col::border); AreaGraph(c.dc, RECT{g.left + c.S(12), g.top + c.S(10), g.right - c.S(12), g.bottom - c.S(10)}, pts, col::accent, c.S(2));
    c.y += c.S(130);
    Section(c, L"Progress and states");
    DrawProgress(c.u, c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(8)}, 62, true, false); c.y += c.S(20);
    Shimmer(c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(18)}, c.S(12), GetTickCount64()); c.y += c.S(30);
    StatePanel(c, L"\xE7BA", L"Unavailable", L"This sensor is not exposed by the hardware.", col::warn);
}
// The animation clock runs only while something can move; otherwise the app gets no 16 ms wake-ups at all.
bool NeedTick() {
    if (g_busy || g_toastQ.Animating(GetTickCount64()) || g_anim.Active() || g_surf.logCards) return true;
    for (size_t i = 0; i < kNav.size(); ++i) if (g_navAnim[i] != (static_cast<int>(i) == g_navOpen ? 1.f : 0.f)) return true;
    const DWORD age = GetTickCount() - g_resultAt;
    if ((g_lastResult == 2 && age < 900) || (g_lastResult == 1 && age < 700)) return true;
    return ((g_page == PApps || NavParent(g_page) == PApps) && !g_appsLoaded) || (g_page == PRecovery && !Snapshot().deepDone);
}
bool g_tickOn = false;
void UpdateTick(HWND h) {
    const bool need = NeedTick();
    if (need && !g_tickOn) SetTimer(h, 1, 16, nullptr); else if (!need && g_tickOn) KillTimer(h, 1);
    g_tickOn = need;
}
std::pair<std::wstring, COLORREF> TempState(bool cpu, double t);
void UpdatePulse(HWND h) {   // the status dot breathes in the compositor while the window is active
    const bool fg = GetForegroundWindow() == h && g_anim.level == MotionFull && !g_gov.ForcesReduced() && !(kTestBuild && GetEnvironmentVariableW(L"ANKH_NOPULSE", nullptr, 0));
    COLORREF c = col::ok; bool on = g_read.present && fg;
    for (const bool cpu : {false, true}) { const auto st = TempState(cpu, cpu ? g_read.cpuTemp : g_read.gpuTemp); if (st.second == col::err || st.second == col::warn) c = st.second; }
    const int size = g_ui.S(30), cy = ClientH() - g_ui.S(33);
    g_surf.SetPulse(on, static_cast<float>(g_ui.S(28) - size / 2), static_cast<float>(cy - size / 2), size, c);
}
void PaintAll(HWND h) {
    ++g_paints;
    if (g_surfOn) {
        PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);   // validate only: every frame is drawn whole
        if (!g_surf.Begin()) return;
        RECT cr; GetClientRect(h, &cr);
        const bool animating = g_anim.Active();
        PaintScene(PaintTarget{Dc(&g_surf), cr});
        g_surf.End();
        if (animating) { g_frameStats.Add(g_surf.lastFrameMs); g_gov.Frame(g_surf.lastFrameMs, GetTickCount64()); }
        UpdatePulse(h);
        UpdateTick(h);
        return;
    }
    { Paint p(h); PaintScene(PaintTarget{Dc(p.dc), p.cr}); }
    UpdateTick(h);
}
void PaintScene(const PaintTarget& p) {
    Dc dc = p.dc;
    g_ui.hits.clear();
    // L1: navy tint over the DWM backdrop, a faint light from the top-left, the ankh watermark (5%) on two pages
    g_anim.Tick(GetTickCount64()); UpdateMotion();
    const bool dialogBlur = dc.d2d() && g_dlg.active && g_gov.GlassEffectsOn() && g_anim.level == MotionFull && g_surf.BeginLayer();
    if (g_material == MatWallpaper) WallDraw(dc, g_wnd, p.cr, 1.f);
    if (g_material == MatClear) { const int fr = ClearBgPct(Cfg().Int(L"appearance.clearFrost", kClearFrostDefault, 0, 100)); if (fr > 0) WallDraw(dc, g_wnd, p.cr, fr / 100.f); }   // 2.10: the whole window frosted with the blurred desktop picture
    FillGlass(dc, p.cr, col::bg, TintAlpha());
    {   // legibility floor: bare text in the content area keeps 4.5:1 over a white desktop even at the lowest tint (self-test measures it)
        constexpr float kFloor = .60f; const float t = TintAlpha();
        if (t < kFloor && g_material != MatClear) FillGlass(dc, RECT{SideW(), 0, p.cr.right, p.cr.bottom}, col::bg, 1.f - (1.f - kFloor) / (1.f - t));
    }
    if (ActiveTheme().isClassic) Halo(dc, 0, 0, g_ui.S(640), col::accent, .08f * GlowK());
    if (dc.d2d() && !g_brandBg.p.empty() && !g_probeBlank) {
        // The supplied background.png exactly as delivered: the whole picture, upright, fitted to the window height
        // with its proportions kept (never cropped, stretched, mirrored or rotated), centred in the content area.
        // It lies over the tint at a share that leaves the glass and the text contrast as they were; its two side
        // edges fade in so there is no seam against the glass.
        const float H = static_cast<float>(p.cr.bottom - p.cr.top), W = H * g_brandBg.w / g_brandBg.h;
        if (g_brandBg.w > g_brandBg.h) {   // a landscape picture is made for the whole window: it covers it edge to edge, proportions kept, centred
            const float cw = static_cast<float>(p.cr.right - p.cr.left), s = std::max(cw / g_brandBg.w, H / g_brandBg.h), dw = g_brandBg.w * s, dh = g_brandBg.h * s;
            const Px& bgPic = WinBgPicture(WinBgBlurCfg());   // the sharp picture, or its cached blurred copy; opacity is the draw alpha only
            DrawPixelsPart(dc, &bgPic, bgPic.p.data(), bgPic.w, bgPic.h, D2D1::RectF((cw - dw) / 2, (H - dh) / 2, (cw + dw) / 2, (H + dh) / 2), D2D1::RectF(0, 0, static_cast<float>(bgPic.w), static_cast<float>(bgPic.h)), WinBgOpacityCfg() / 100.0f);
        } else {
        const float x0 = SideW() + ((p.cr.right - SideW()) - W) / 2, k = g_brandBg.w / W, ih = static_cast<float>(g_brandBg.h);
        constexpr float kShare = kBrandBgOpacity; constexpr int kFade = 16;
        const float fadeW = W * .12f, step = fadeW / kFade;
        auto part = [&](float l, float r, float share) {
            DrawPixelsPart(dc, &g_brandBg, g_brandBg.p.data(), g_brandBg.w, g_brandBg.h, D2D1::RectF(x0 + l, 0, x0 + r, H), D2D1::RectF(l * k, 0, r * k, ih), share);
        };
        for (int i = 0; i < kFade; ++i) { const float sh = kShare * (i + 1) / (kFade + 1); part(i * step, (i + 1) * step, sh); part(W - (i + 1) * step, W - i * step, sh); }
        part(fadeW, W - fadeW, kShare);
        }
    }
    const Cache s = Snapshot();

    // L2: navigation rail
    RECT side{0, 0, SideW(), p.cr.bottom};
    FillGlass(dc, side, col::side, std::min(1.f, TintAlpha() + (g_material == MatClear ? .20f : .10f)));   // 2.11: on Clear glass the rail stays different from the page but is not black
    Line(dc, side.right - 1, 0, side.right - 1, p.cr.bottom, Mix(col::side, ActiveTheme().isClassic ? col::accent : col::border, .65));
    // brand plinth: reserved for the official mark (never recoloured; the glitch modes carry the operation state)
    const int logoMode = g_busy ? 1 : (g_lastResult == 2 && GetTickCount() - g_resultAt < 900) ? 3 : 0;
    if (g_navCompact) {
        if (!g_brandLogoCompact.p.empty() && dc.d2d()) {   // the unified logo at 40 DIP; its own glow replaces the drawn halo
            const float sz = static_cast<float>(g_ui.S(40)), lx = side.right / 2 - sz / 2, ly = static_cast<float>(g_ui.S(16));
            DrawPixelsPart(dc, &g_brandLogoCompact, g_brandLogoCompact.p.data(), g_brandLogoCompact.w, g_brandLogoCompact.h, D2D1::RectF(lx, ly, lx + sz, ly + sz),
                           D2D1::RectF(0, 0, static_cast<float>(g_brandLogoCompact.w), static_cast<float>(g_brandLogoCompact.h)), 1.f);
        } else {
            Halo(dc, side.right / 2, g_ui.S(36), g_ui.S(38), col::accent, .24f * GlowK());
            DrawIconAt(dc, g_ankhBig, side.right / 2 - g_ui.S(20), g_ui.S(16), g_ui.S(40));
        }
    } else {
        if (!g_brandLogoRail.p.empty() && dc.d2d()) {   // the owner's logo.png with its own transparency, with nothing drawn behind or around it
            const float sz = static_cast<float>(g_ui.S(84)), lx = side.right / 2 - sz / 2, ly = static_cast<float>(g_ui.S(2));
            DrawPixelsPart(dc, &g_brandLogoRail, g_brandLogoRail.p.data(), g_brandLogoRail.w, g_brandLogoRail.h, D2D1::RectF(lx, ly, lx + sz, ly + sz),
                           D2D1::RectF(0, 0, static_cast<float>(g_brandLogoRail.w), static_cast<float>(g_brandLogoRail.h)), 1.f);
        } else {
            Halo(dc, side.right / 2, g_ui.S(50), g_ui.S(64), col::accent, .26f * GlowK());
            g_logo.Draw(dc, side.right / 2, g_ui.S(50), logoMode, 1.0f, /*breathe*/false);   // still at rest: no idle repaint
        }
        if (g_busy && g_anim.level == MotionFull && g_gov.ParticlesOn() && dc.d2d()) {   // particles drift up from the mark while an operation runs
            const double t = GetTickCount() / 1000.0;
            for (int i = 0; i < 48; ++i) {
                const double ph = fmod(t / 2.2 + Rand01(i * 31 + 7), 1.0);
                const float px = static_cast<float>(side.right / 2 + (Rand01(i * 17 + 3) - .5) * g_ui.S(96)), py = static_cast<float>(g_ui.S(92) - ph * g_ui.S(84));
                dc.d->dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), 1.4f, 1.4f), dc.d->Brush(RGB(120, 220, 255), static_cast<float>((1 - ph) * .75)));
            }
        }
        Text(dc, L"AnkhCore", RECT{0, g_ui.S(86), side.right, g_ui.S(110)}, g_ui.f.h2, col::text, DT_CENTER | DT_SINGLELINE);
        const std::wstring sideVer = kIsBetaBuild ? FormatBetaVersionLabel(g_betaDaysRemaining, true) : std::wstring(kVersionLabel);
        Text(dc, sideVer + L"  \x00B7  by As3aD", RECT{0, g_ui.S(108), side.right, g_ui.S(126)}, g_ui.f.small, col::accent, DT_CENTER | DT_SINGLELINE);
    }
    if (kTestBuild) {   // impossible to mistake for the installed AnkhCore
        RECT tb{g_ui.S(4), g_ui.S(4), g_ui.S(4) + g_ui.S(g_navCompact ? 40 : 66), g_ui.S(4) + g_ui.S(16)};
        Round(dc, tb, g_ui.S(6), RGB(60, 40, 8), col::warn);
        Text(dc, g_navCompact ? L"TEST" : L"TEST BUILD", tb, g_ui.f.tiny, col::warn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // accordion navigation (2.5): glass pill, cyan light bar, and a circuit trace joining a section's children
    std::wstring navTip, navFocusTip; RECT navTipRow{}, navFocusRow{};   // compact rail: the hovered item's name, else the keyboard-focused one
    {
        NavClampScroll();
        const int clipTop = NavTop(), clipBottom = clipTop + NavViewH();
        const int save = PushClip(dc, RECT{0, clipTop, side.right, clipBottom});
        const auto rows = NavLayout();
        if (!g_navCompact) for (size_t sec = 0; sec < kNav.size(); ++sec) {   // the trace goes under the rows
            int top = INT_MAX, bot = INT_MIN, tx = 0;
            for (const NavRow& row : rows) if (row.child && row.section == static_cast<int>(sec)) { tx = row.r.left + g_ui.S(32); top = std::min<int>(top, (row.r.top + row.r.bottom) / 2); bot = std::max<int>(bot, (row.r.top + row.r.bottom) / 2); }
            if (top < bot) Line(dc, tx, top, tx, bot, Mix(col::side, col::accent, .32));
        }
        for (const NavRow& row : rows) {
            const RECT& r = row.r;
            if (r.bottom < clipTop || r.top > clipBottom) continue;
            const NavSection& sec = kNav[row.section];
            const bool multi = sec.pages.size() > 1;
            const bool sel = row.child ? (g_page == row.page || NavParent(g_page) == row.page) : (!multi && (g_page == sec.pages[0] || NavParent(g_page) == sec.pages[0]));
            const bool parentActive = !row.child && multi && NavSectionOf(g_page) == row.section;
            const bool hov = g_ui.Over(r);
            // keyboard focus: the current page's row; while its section is closed, the section's row carries it
            const bool closedParent = !row.child && multi && parentActive && NavParent(g_page) < 0 && g_navOpen != row.section && g_navAnim[row.section] <= 0.f;
            const bool focus = g_navKeyboard && ((row.child && g_page == row.page) || (!row.child && !multi && g_page == sec.pages[0]) || closedParent);
            if (focus && g_navCompact) { navFocusTip = closedParent ? std::wstring(sec.name) + L"  \x00B7  " + NavChildName(g_page) : row.child ? NavChildName(row.page) : sec.name; navFocusRow = r; }
            if (sel || hov) Round(dc, r, g_ui.S(row.child ? 20 : 24), col::panel, sel ? col::accentDim : col::panel);
            if (focus) Round(dc, r, g_ui.S(row.child ? 20 : 24), col::panel, col::accent);
            if (sel) {   // the active item's cyan light bar
                RECT bar{r.left, r.top + g_ui.S(row.child ? 8 : 10), r.left + g_ui.S(3), r.bottom - g_ui.S(row.child ? 8 : 10)};
                Halo(dc, bar.left, (bar.top + bar.bottom) / 2, g_ui.S(20), col::accent, .32f * GlowK());
                FillRect2(dc, bar, col::accent);
            }
            const float ht = g_ui.Tween(row.page, row.child, 5, sel || parentActive || hov);
            COLORREF fg = Mix(col::sub, col::text, ht);
            const COLORREF ic = Mix(col::sub, sel || parentActive ? col::accent : col::text, ht);
            const int cy = (r.top + r.bottom) / 2;
            if (!row.child) {
                if (g_navCompact) Text(dc, sec.icon, r, g_ui.f.icon, ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                else {
                    Text(dc, sec.icon, RECT{r.left + g_ui.S(14), r.top, r.left + g_ui.S(44), r.bottom}, g_ui.f.icon, ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    Text(dc, sec.name, RECT{r.left + g_ui.S(52), r.top, r.right - g_ui.S(28), r.bottom}, g_ui.f.text, fg, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (multi) {   // the chevron turns with the expand tween
                        const RECT cr2{r.right - g_ui.S(30), r.top, r.right - g_ui.S(8), r.bottom};
                        if (dc.d2d()) dc.d->dc->SetTransform(D2D1::Matrix3x2F::Rotation(g_navAnim[row.section] * 180.f, D2D1::Point2F((cr2.left + cr2.right) / 2.f, (cr2.top + cr2.bottom) / 2.f)));
                        Text(dc, dc.d2d() || g_navOpen != row.section ? L"\xE70D" : L"\xE70E", cr2, g_ui.f.icon, col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        if (dc.d2d()) dc.d->dc->SetTransform(D2D1::Matrix3x2F::Identity());
                    }
                }
                if (hov && g_navCompact) { navTip = sec.name; navTipRow = r; }
            } else {
                int idx = 0; for (size_t q = 0; q < sec.pages.size(); ++q) if (sec.pages[q] == row.page) idx = static_cast<int>(q);
                const float vis = std::clamp((g_navAnim[row.section] - idx * .12f) / .6f, 0.f, 1.f);   // children fade in staggered
                fg = Mix(col::side, fg, vis);
                const int dotX = g_navCompact ? (r.left + r.right) / 2 - g_ui.S(3) : r.left + g_ui.S(30);
                if (sel) Halo(dc, dotX + g_ui.S(3), cy, g_ui.S(11), col::accent, .55f * GlowK());
                DrawDot(dc, dotX, cy - g_ui.S(3), g_ui.S(6), sel ? col::accent : Mix(col::side, col::accent, .4));
                if (!g_navCompact) Text(dc, NavChildName(row.page), RECT{r.left + g_ui.S(46), r.top, r.right - g_ui.S(8), r.bottom}, g_ui.f.small, fg, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                else if (hov) { navTip = NavChildName(row.page); navTipRow = r; }
            }
            if (r.top >= clipTop && r.bottom <= clipBottom) {
                const NavRow cp = row;
                g_ui.Add(r, [cp] { NavActivate(cp); });
            }
        }
        PopClip(dc, save);
    }
    if (navTip.empty()) { navTip = navFocusTip; navTipRow = navFocusRow; }
    {   // status block: live-data dot and CPU temperature; opens Desktop Monitoring. Beside it: the compact-rail switch.
        const COLORREF st = g_read.present ? col::ok : g_read.everSeen ? col::warn : col::faint;
        const RECT sb{g_ui.S(12), p.cr.bottom - g_ui.S(52), side.right - g_ui.S(g_navCompact ? 12 : 48), p.cr.bottom - g_ui.S(14)};
        g_navStatusRect = sb;
        if (g_ui.Over(sb)) Round(dc, sb, g_ui.S(20), col::panel2, col::panel2);
        Halo(dc, sb.left + g_ui.S(16), (sb.top + sb.bottom) / 2, g_ui.S(11), st, .5f * GlowK());
        DrawDot(dc, sb.left + g_ui.S(12), (sb.top + sb.bottom) / 2 - g_ui.S(4), g_ui.S(8), st);
        bool dim = false; const std::wstring live = std::wstring(g_read.present ? L"Live   " : L"") + MiniValue(MCpuTemp, dim);   // the same words in both rail modes
        if (!g_navCompact) Text(dc, live, RECT{sb.left + g_ui.S(30), sb.top, sb.right, sb.bottom}, g_ui.f.small, dim ? col::faint : col::text, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        else if (g_ui.Over(sb)) { navTip = std::wstring(kPageName[PDesktop]) + L"  \x00B7  " + live; navTipRow = sb; }
        g_ui.Add(sb, [] { NavGo(PDesktop); });
        if (!g_navCompact) {
            const RECT cb{side.right - g_ui.S(44), sb.top + g_ui.S(3), side.right - g_ui.S(12), sb.bottom - g_ui.S(3)};
            g_navToggleRect = cb;
            if (g_ui.Over(cb)) Round(dc, cb, g_ui.S(16), col::panel2, col::panel2);
            Text(dc, L"\xE76B", cb, g_ui.f.icon, col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_ui.Add(cb, [] { g_navCompact = true; Cfg().SetBool(L"nav.compact", true); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); });
        } else {
            const RECT cb{g_ui.S(16), sb.top - g_ui.S(34), side.right - g_ui.S(16), sb.top - g_ui.S(4)};
            g_navToggleRect = cb;
            if (g_ui.Over(cb)) { Round(dc, cb, g_ui.S(16), col::panel2, col::panel2); navTip = L"Expand navigation"; navTipRow = cb; }
            Text(dc, L"\xE76C", cb, g_ui.f.icon, col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_ui.Add(cb, [] { g_navCompact = false; Cfg().SetBool(L"nav.compact", false); Cfg().Save(); InvalidateRect(g_wnd, nullptr, FALSE); });
        }
    }

    // header
    {   // a sub-page shows a Back control and where it belongs, so the way back is always in the same place
        const int parent = NavParent(g_page); int tx = SideW() + g_ui.S(32);
        const int backTo = g_page == PSettings ? g_settingsFrom : parent;   // Settings goes back to where it was opened from
        if (backTo >= 0) {
            const RECT bk{tx - g_ui.S(6), g_ui.S(22), tx + g_ui.S(30), g_ui.S(56)};
            if (g_ui.Over(bk)) Round(dc, bk, g_ui.S(8), col::panel2, col::panel2);
            Text(dc, L"\xE72B", bk, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            g_ui.Add(bk, [backTo] { NavGo(backTo); });
            tx += g_ui.S(42);
        }
        Text(dc, kPageName[g_page], RECT{tx, g_ui.S(20), ClientW() - g_ui.S(200), g_ui.S(58)}, g_ui.f.h1, col::text);
        Text(dc, parent >= 0 ? std::wstring(kPageName[parent]) + L"  \u203A  " + kPageSub[g_page] : std::wstring(kPageSub[g_page]), RECT{tx + g_ui.S(1), g_ui.S(58), ClientW() - g_ui.S(200), g_ui.S(78)}, g_ui.f.small, col::sub, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    const int capR = ClientW() - g_ui.S(14), capT = g_ui.S(12), capB = g_ui.S(42), capW = g_ui.S(36), capStep = g_ui.S(40);
    RECT rc{capR - capW, capT, capR, capB}, rx{capR - capStep - capW, capT, capR - capStep, capB}, rm{capR - 2 * capStep - capW, capT, capR - 2 * capStep, capB};
    const RECT rs{capR - 3 * capStep - capW, capT, capR - 3 * capStep, capB};   // 2.7 J2: Settings, apart from the navigation
    if (g_ui.Over(rc)) Round(dc, rc, g_ui.S(8), RGB(196, 43, 28), RGB(196, 43, 28));
    if (g_ui.Over(rx)) Round(dc, rx, g_ui.S(8), col::panel2, col::panel2);
    Text(dc, IsZoomed(g_wnd) ? L"\xE923" : L"\xE922", rx, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    g_ui.Add(rx, [] { ShowWindow(g_wnd, IsZoomed(g_wnd) ? SW_RESTORE : SW_MAXIMIZE); });
    if (g_ui.Over(rm)) Round(dc, rm, g_ui.S(8), col::panel2, col::panel2);
    Text(dc, L"\xE8BB", rc, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"\xE921", rm, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    {   // the Settings control: highlighted while Settings or one of its pages is open
        const bool inSettings = g_page == PSettings || NavParent(g_page) == PSettings;
        if (g_ui.Over(rs)) Round(dc, rs, g_ui.S(8), col::panel2, col::panel2);
        Text(dc, L"\xE713", rs, g_ui.f.icon, inSettings ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        g_ui.Add(rs, [] { NavOpenSettings(); });
    }
    if (g_page == PDashboard) {
        const std::wstring stamp = g_read.everSeen ? L"Last update " + StampText(g_read.stamp) + (g_read.present ? L"  \x00B7  Live" : L"  \x00B7  stale") : L"No sensor data";
        const int w = TextWidth(dc, stamp, g_ui.f.tiny) + g_ui.S(14);
        const RECT probe{rs.left - w - g_ui.S(12), g_ui.S(18)};
        RECT cr3{probe.left, g_ui.S(18), probe.left + w, g_ui.S(36)};
        const COLORREF cc = g_read.present ? col::ok : col::faint;
        Round(dc, cr3, g_ui.S(18), Mix(col::panel, cc, .16), Mix(col::panel, cc, .6));
        Text(dc, stamp, cr3, g_ui.f.tiny, cc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    g_ui.Add(rc, [] { PostMessageW(g_wnd, WM_CLOSE, 0, 0); });
    g_ui.Add(rm, [] { ShowWindow(g_wnd, SW_MINIMIZE); });

    // content (clipped, scrolled)
    RECT cr = ContentRect();
    const float pageMs = g_anim.To(kPageFadeId, 1.f, 240) * 560.f;
    const bool reveal = dc.d2d() && pageMs < 556.f && g_gov.GlassEffectsOn() && g_anim.level == MotionFull && !g_probeBlank && !dialogBlur && g_surf.BeginLayer();
    if (reveal) { g_surf.cardLog.clear(); g_surf.logCards = true; }
    const int save = PushClip(dc, cr);
    g_ui.keyDy = g_scroll[g_page];
    Ctx c{dc, g_ui, cr.left + g_ui.S(32), cr.top + g_ui.S(6) - g_scroll[g_page], cr.right - cr.left - g_ui.S(64), cr, g_busy.load() && !g_opBackground.load()};
    const int y0 = c.y;
    const float pageT = g_anim.To(kPageFadeId, 1.f, 240);
    const bool pageLayer = dc.d2d() && pageT < .995f && !reveal && !dialogBlur;
    if (pageLayer) {   // page open: fades in and settles 12 DIP
        dc.d->dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), pageT), nullptr);
        dc.d->dc->SetTransform(D2D1::Matrix3x2F::Translation(0.f, g_ui.S(12) * (1.f - pageT)));
    }
    if (g_probeBlank) ProbePage(c); else if (g_stGallery) PageGallery(c); else
    switch (g_page) {
        case PDashboard: PageDashboard(c, s); break;
        case PMonitor: PageMonitor(c, s); break;
        case PFan: PageFan(c, s); break;
        case PAppearance: PageAppearance(c, s); break;
        case PSettings: PageSettings(c, s); break;
        case PWindhawk: PageWindhawk(c, s); break;
        case PTweaks: PageTweaks(c, s); break;
        case POptimize: PageOptimize(c, s); break;
        case PApps: PageApps(c, s); break;
        case PRecovery: PageRecovery(c, s); break;
        case PActivity: PageActivity(c, s); break;
        case PHardware: PageHardware(c, s); break;
        case PNetwork: PageNetwork(c, s); break;
        case PErrLog: PageErrLog(c, s); break;
        case PStartup: PageStartup(c, s); break;
        case PDesktop: PageDesktop(c, s); break;
        case PSbLayout: PageSbLayout(c, s); break;
        case PSbAppearance: PageSbAppearance(c, s); break;
        case PSbWidgets: PageSbWidgets(c, s); break;
        case PSbBehavior: PageSbBehavior(c, s); break;
        case PSbProfiles: PageSbProfiles(c, s); break;
        case PMiniSet: PageMiniSet(c, s); break;
        case PMonPrefs: PageMonPrefs(c, s); break;
        case PAppMaterial: PageAppMaterial(c, s); break;
        case PAppTheme: PageAppTheme(c, s); break;
        case PAppWallpaper: PageAppWallpaper(c, s); break;
        case PAppCursor: PageAppCursor(c, s); break;
        case PAppIcons: PageAppIcons(c, s); break;
        case PAppsInstalled: PageAppsInstalled(c, s); break;
        case PAppsBuiltin: PageAppsBuiltin(c, s); break;
        case PAppsUninstall: PageAppsUninstall(c, s); break;
        case PAppsUpdates: PageAppsUpdates(c, s); break;
        case PSystemCare: PageSystemCare(c, s); break;
        case PProcMgr: PageProcMgr(c, s); break;
        case PPowerToys: PagePowerToys(c, s); break;
        case PBackup: PageBackup(c, s); break;
        case PRecHistory: PageRecHistory(c, s); break;
        case PDrivers: PageDrivers(c, s); break;
        case PStorage: PageStorage(c, s); break;
        default: PageAbout(c, s);
    }
    g_contentH[g_page] = c.y - y0 + g_ui.S(20);
    if (pageLayer) { dc.d->dc->SetTransform(D2D1::Matrix3x2F::Identity()); dc.d->dc->PopLayer(); }
    PopClip(dc, save);
    if (reveal) { g_surf.logCards = false; g_surf.EndLayer(); g_surf.ComposeReveal(pageMs, static_cast<float>(g_ui.S(8))); }
    g_ui.keyDy = 0;
    g_scroll[g_page] = std::clamp(g_scroll[g_page], 0, std::max(0, g_contentH[g_page] - static_cast<int>(cr.bottom - cr.top)));   // after a resize
    // scrollbar
    const int viewH = cr.bottom - cr.top;
    if (g_contentH[g_page] > viewH) {
        const int th = std::max(g_ui.S(40), viewH * viewH / g_contentH[g_page]);
        const int ty = cr.top + (viewH - th) * g_scroll[g_page] / std::max(1, g_contentH[g_page] - viewH);
        RECT sb{cr.right - g_ui.S(10), ty, cr.right - g_ui.S(5), ty + th};
        const bool grab = g_pageSbDrag || (g_ui.mouse.x >= cr.right - g_ui.S(16) && g_ui.mouse.x < cr.right && g_ui.mouse.y >= ty && g_ui.mouse.y < ty + th);
        Round(dc, sb, g_ui.S(5), grab ? col::accent : col::border, grab ? col::accent : col::border);
    }

    if (PinnedBarH() > 0) DrawCareBar(dc, RECT{cr.left, cr.top - PinnedBarH(), cr.right, cr.top});
    // footer: current operation (Bottom Status Bar — uses active theme overlay & border tokens)
    RECT ft{cr.left, p.cr.bottom - g_ui.S(kFooter), p.cr.right, p.cr.bottom};
    FillGlass(dc, ft, col::overlay, g_material == MatClear ? std::min(1.f, TintAlpha() + .25f) : std::max(.86f, TintAlpha()));   // L4 (2.11: lighter on Clear glass, still a different shade from the page)
    Line(dc, ft.left, ft.top, ft.right, ft.top, Mix(col::side, ActiveTheme().isClassic ? col::accent : col::border, .65));
    if (dc.d2d() && g_anim.level != MotionOff) {   // the operation band
        const DWORD age = GetTickCount() - g_resultAt;
        const float fw = static_cast<float>(ft.right - ft.left), fy = static_cast<float>(ft.top);
        if (g_busy) {
            const float x = ft.left + fw * ((GetTickCount() % 1600) / 1600.f) * 1.3f - fw * .3f;
            const COLORREF bandHi = ActiveTheme().isClassic ? RGB(120, 220, 255) : col::accent;
            if (auto* g = dc.d->Gradient(col::accent, 0.f, bandHi, .9f, D2D1::Point2F(x, fy), D2D1::Point2F(x + fw * .3f, fy))) dc.d->dc->FillRectangle(D2D1::RectF(std::max<float>(x, static_cast<float>(ft.left)), fy, x + fw * .3f, fy + 2.f), g);
        } else if (g_lastResult == 1 && age < 600) dc.d->dc->FillRectangle(D2D1::RectF(static_cast<float>(ft.left), fy, ft.left + fw * age / 600.f, fy + 2.f), dc.d->Brush(col::ok, 1.f - age / 600.f));
        else if (g_lastResult == 2 && age < 250) { const float j = (age % 40 < 20) ? 3.f : -3.f; dc.d->dc->FillRectangle(D2D1::RectF(ft.left + j, fy, ft.right + j, fy + 2.f), dc.d->Brush(col::err)); }
    }
    std::wstring st; COLORREF sc = col::sub;
    const bool wuDl = Wu().DownloadActive(); WuSnapshot wus; if (wuDl) wus = Wu().Snapshot();   // 2.9.5: a Windows update download shows here without being the single busy operation
    if (g_busy) { st = g_opName; const std::wstring l = OpLine(); if (!l.empty()) st += L"  \x00B7  " + l; sc = col::text; }
    else if (wuDl) { st = L"Windows Update: " + wus.status; sc = col::text; }
    else if (g_lastResult == 1) { st = L"Done: " + g_opName; sc = col::ok; }
    else if (g_lastResult == 2) { st = L"Failed: " + g_opName + L"  (details in Activity)"; sc = col::err; }
    else st = L"Ready.";
    const int barW = g_ui.S(220);
    Text(dc, st, RECT{ft.left + g_ui.S(32), ft.top, ft.right - barW - g_ui.S(220), ft.bottom}, g_ui.f.small, sc, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (g_busy || wuDl) {
        RECT pb{ft.right - barW - g_ui.S(200), ft.top + g_ui.S(22), ft.right - g_ui.S(200), ft.top + g_ui.S(30)};
        DrawProgress(g_ui, dc, pb, g_busy ? static_cast<int>(g_progress) : wus.overallPct, true, false);   // unknown length (-1) is a travelling band, never a full bar (LOG-2)
    }
    if (g_restartNeeded) {
        RECT rr{ft.right - g_ui.S(186), ft.top + g_ui.S(10), ft.right - g_ui.S(20), ft.bottom - g_ui.S(10)};
        DrawButton(g_ui, dc, rr, L"Restart now", Btn::Primary, !g_busy, g_ui.Over(rr));
        if (!g_busy) g_ui.Add(rr, [] {
            if (TestGuard(L"Restart the PC")) return;
            if (!Confirm(L"Restart the PC now? Save your work first.", L"Restart now", true)) return;
            EnablePrivilege(SE_SHUTDOWN_NAME);
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED); });
    }
    g_navTipShown = navTip; g_navTipRect = RECT{};
    if (!navTip.empty()) {   // compact rail tooltip, on glass
        const RECT tr = NavTipRect(side.right, (navTipRow.top + navTipRow.bottom) / 2, TextWidth(dc, navTip, g_ui.f.small), g_ui.S(12), g_ui.S(15), g_ui.S(6), p.cr.right, p.cr.bottom);
        g_navTipRect = tr;
        Round(dc, tr, g_ui.S(16), col::overlay, Mix(col::side, ActiveTheme().isClassic ? col::accent : col::borderHi, .4));
        Text(dc, navTip, tr, g_ui.f.small, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    g_toastRects.clear();
    if (!g_toastQ.Empty()) {   // FT-TOAST: bottom left of the content, above the footer: away from the action buttons at the right edge of the rows
        const uint64_t now = GetTickCount64();
        const int areaL = SideW() + g_ui.S(20), areaTop = g_ui.S(kHeader) + g_ui.S(10), areaBottom = p.cr.bottom - g_ui.S(kFooter) - g_ui.S(14), gap = g_ui.S(8);
        const int w = std::min<int>(g_ui.S(380), static_cast<int>(p.cr.right - SideW()) * 45 / 100), tw = w - g_ui.S(36);
        const int lineH = std::max(1, TextHeight(dc, L"Ag", tw, g_ui.f.small)), maxText = lineH * 4;
        std::vector<int> heights; for (const ToastItem& t : g_toastQ.Visible()) heights.push_back(std::max(g_ui.S(48), std::min(TextHeight(dc, t.text, tw, g_ui.f.small), maxText) + g_ui.S(22)));
        size_t fitted = 0; const auto slots = ToastStack(heights, areaTop, areaBottom, gap, fitted);
        size_t i = 0;
        for (const ToastItem& t : g_toastQ.Visible()) {
            const ToastSlot& sl = slots[i++];
            if (!sl.shown) continue;
            const float in = g_anim.level == MotionOff ? 1.f : ToastEntry(now, t.at, 220);
            const int dx = static_cast<int>((1.f - in) * -g_ui.S(24));
            const RECT tr{areaL + dx, sl.top, areaL + w + dx, sl.top + sl.height};
            g_toastRects.push_back(tr);
            Round(dc, tr, g_ui.S(28), col::overlay, Mix(col::side, t.color, .55));
            FillRect2(dc, RECT{tr.left + g_ui.S(8), tr.top + g_ui.S(12), tr.left + g_ui.S(11), tr.bottom - g_ui.S(12)}, t.color);
            Text(dc, t.text, RECT{tr.left + g_ui.S(22), tr.top + g_ui.S(8), tr.right - g_ui.S(14), tr.bottom - g_ui.S(8)}, g_ui.f.small, col::text, DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
            const uint64_t id = t.id; g_ui.Add(tr, [id] { g_toastQ.Dismiss(id, GetTickCount64()); ToastSchedule(g_wnd); });   // a click clears it
        }
        if (g_toastQ.WaitingCount() && fitted) {
            const std::wstring more = L"+" + std::to_wstring(g_toastQ.WaitingCount()) + L" more";
            Text(dc, more, RECT{areaL, areaTop - g_ui.S(2), areaL + w, areaTop + g_ui.S(16)}, g_ui.f.tiny, col::faint, DT_SINGLELINE | DT_VCENTER);
        }
    }
    if (g_dlg.active) {   // the dialog owns the window: everything behind it is dimmed and takes no clicks
        g_ui.hits.clear();
        if (dialogBlur) { g_surf.EndLayer(); g_surf.DrawBlurredLayer(static_cast<float>(g_ui.S(6))); }
        FillGlass(dc, p.cr, col::bg, .55f);
        const int w = g_ui.S(460), tw = w - g_ui.S(112), th = std::max(TextHeight(dc, g_dlg.text, tw, g_ui.f.text), g_ui.S(40)), h = th + g_ui.S(112);
        RECT b{(p.cr.right - w) / 2, (p.cr.bottom - h) / 2, (p.cr.right + w) / 2, (p.cr.bottom + h) / 2};
        Round(dc, b, g_ui.S(36), col::overlay, Mix(col::side, col::accent, .5));
        Round(dc, RECT{b.left + g_ui.S(24), b.top + g_ui.S(26), b.left + g_ui.S(64), b.top + g_ui.S(66)}, g_ui.S(40), col::accentDeep, col::accent);
        Text(dc, L"\xE897", RECT{b.left + g_ui.S(24), b.top + g_ui.S(26), b.left + g_ui.S(64), b.top + g_ui.S(66)}, g_ui.f.icon, col::accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, g_dlg.text, RECT{b.left + g_ui.S(84), b.top + g_ui.S(26), b.right - g_ui.S(28), b.top + g_ui.S(26) + th}, g_ui.f.text, col::text, DT_WORDBREAK);
        RECT yes{b.right - g_ui.S(144), b.bottom - g_ui.S(58), b.right - g_ui.S(24), b.bottom - g_ui.S(22)}, no{yes.left - g_ui.S(130), yes.top, yes.left - g_ui.S(10), yes.bottom};
        const int vw = std::max(g_ui.S(120), TextWidth(dc, g_dlg.verb, g_ui.f.btn) + g_ui.S(32)); yes.left = yes.right - vw; no.right = yes.left - g_ui.S(10); no.left = no.right - g_ui.S(120);
        DrawButton(g_ui, dc, yes, g_dlg.verb, g_dlg.danger ? Btn::Danger : Btn::Primary, true, g_ui.Over(yes));
        DrawButton(g_ui, dc, no, L"Cancel", Btn::Secondary, true, g_ui.Over(no));
        g_ui.Add(yes, [] { g_dlg.result = true; g_dlg.active = false; });
        g_ui.Add(no, [] { g_dlg.result = false; g_dlg.active = false; });
    }
}
void LoadThumbsAsync() {
    if (g_thumbsLoading.exchange(true)) return;
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto paths = WallImages();
        std::vector<Image32> th;
        const int w = g_ui.S(220), h = g_ui.S(124);
        for (const auto& p : paths) { std::vector<uint8_t> d; th.push_back(ReadAll(p, d) ? DecodeImage(d.data(), d.size(), w, h, true) : Image32{}); }
        auto* pk = new std::pair<std::vector<std::wstring>, std::vector<Image32>>(paths, th);
        g_thumbsLoading = false;
        PostMessageW(g_wnd, WM_THUMBS, 0, reinterpret_cast<LPARAM>(pk));
        return 0; }, nullptr, 0, nullptr));
}
// ---------------------------------------------------------------- tray icon
NOTIFYICONDATAW g_nid{};
void TrayAdd(HWND h) {
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = h; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    lstrcpynW(g_nid.szTip, MainWindowTitle(kTestBuild).c_str(), 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
void TrayRemove(HWND) { Shell_NotifyIconW(NIM_DELETE, &g_nid); }
void TrayBalloon(HWND, const std::wstring& text) {
    g_nid.uFlags = NIF_INFO; g_nid.dwInfoFlags = NIIF_NONE;
    lstrcpynW(g_nid.szInfoTitle, kProductFull, 64);
    lstrcpynW(g_nid.szInfo, text.c_str(), 256);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}
void TrayMenu(HWND h) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 100, L"Open AnkhCore");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_sidebarOn ? MF_CHECKED : 0), 101, L"Desktop sidebar");
    AppendMenuW(menu, MF_STRING | (g_miniOn ? MF_CHECKED : 0), 102, L"Mini monitor");
    AppendMenuW(menu, MF_STRING | (g_sidebarOn ? 0 : MF_GRAYED) | (g_sidebarOn && Cfg().Bool(L"sidebar.clickThrough", false) ? MF_CHECKED : 0), 104, L"Sidebar click-through	Ctrl+Alt+Shift+S");
    AppendMenuW(menu, MF_STRING | (g_miniOn ? 0 : MF_GRAYED) | (g_miniOn && Cfg().Bool(L"mini.clickThrough", false) ? MF_CHECKED : 0), 105, L"Mini click-through	Ctrl+Alt+Shift+M");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 106, g_monPaused ? L"Resume monitoring" : L"Pause monitoring");
    AppendMenuW(menu, MF_STRING, 107, L"Monitoring settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 103, L"Exit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(h);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, h, nullptr);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- live stat bar
void StatBar(Dc dc, Ui& u, RECT r, const std::wstring& label, double value, double pct, const std::wstring& unit, COLORREF color) {
    Text(dc, label, RECT{r.left, r.top, r.right, r.top + u.S(16)}, u.f.tiny, col::sub, DT_SINGLELINE);
    const std::wstring vs = value < 0 ? L"N/A" : (std::to_wstring(static_cast<int>(value + 0.5)) + unit);
    Text(dc, vs, RECT{r.left, r.top, r.right, r.top + u.S(16)}, u.f.tiny, value < 0 ? col::faint : col::text, DT_RIGHT | DT_SINGLELINE);
    RECT track{r.left, r.top + u.S(18), r.right, r.top + u.S(23)};
    Round(dc, track, u.S(2), col::track, col::track);
    if (pct > 0) { RECT f = track; f.right = track.left + static_cast<int>((track.right - track.left) * std::clamp(pct, 0.0, 100.0) / 100); BarFill(dc, f, u.S(5), Mix(col::bg, color, .45), color); }
}

// ---------------------------------------------------------------- desktop sidebar (2.5)
// A desktop-native overlay built on the techniques SidebarDiagnostics uses, re-implemented for
// AnkhCore's own Win32 window:
// - dock to the left or right edge of a chosen monitor's work area, at that monitor's DPI
//   (the window is created per-monitor-DPI aware, independent of the main window);
// - optionally reserve the screen edge as an AppBar so maximised windows don't cover it;
// - three layers: always on top, normal, or desktop (kept at the bottom);
// - click-through vs interactive (WS_EX_TRANSPARENT), switchable from the tray and Ctrl+Alt+Shift+S;
// - never takes focus (WS_EX_NOACTIVATE + MA_NOACTIVATE);
// - adjustable background opacity with readable, opaque text (per-pixel alpha);
// - every setting persisted in settings.json; follows display, work-area and Explorer restarts.
// All live values come from the same g_read / g_usage the main window shows (no second poller).
struct SidebarCfg {
    int edge = 1;            // 0 left, 1 right
    std::wstring monitor;    // display device name ("" = primary)
    int width = 210;         // DIPs
    int opacity = 92;        // background opacity %, text stays opaque
    int layer = SbLayerDesktop;   // 0 always on top, 1 normal, 2 below windows (top-level), 3 desktop (hosted in the desktop; default)
    bool clickThrough = false;
    int layout = SbStandard;  // information density: 0 minimal, 1 standard, 2 detailed, 3 custom (a level per section)
    std::wstring sections;    // order and visibility ("cpu,gpu,-mem,..."); see sidebar_layout.h
    std::wstring levels, styles;   // per section: custom detail level, and indicators / gauges / values / graphs
    int heightPct = 100, valign = 0;   // share of the work area's height; 0 top, 1 middle, 2 bottom
    int spacing = 1, fontScale = 100;  // 0 compact, 1 normal, 2 relaxed; text and content size %
    int background = 0, tint = 70;     // 0 solid, 1 frosted glass, 2 acrylic blur; how much tint lies over the glass %
    int accent = -1, accent2 = -1, textColor = -1, tintColor = -1;   // 0xRRGGBB, -1 = the theme's colour
    int glow = 1; bool animate = false;
    bool autoHide = false;    // slides off its edge when the pointer is away; returns when the pointer touches the edge
    bool locked = true;       // unlocked: drag it to another screen, side or height
    int posPct = -1;          // where it sits along the edge when shorter than the screen (0..100); -1 = from valign
    bool reserve = false;    // AppBar: reserve the edge
};
SidebarCfg SbLoad() {
    SidebarCfg c;
    c.edge = Cfg().Int(L"sidebar.edge", 1, 0, 1);
    c.monitor = Cfg().Str(L"sidebar.monitor", L"");
    c.width = Cfg().Int(L"sidebar.width", 210, 140, 420);
    c.opacity = Cfg().Int(L"sidebar.opacity", 92, 10, 100);
    c.layer = Cfg().Int(L"sidebar.layer", SbLayerDesktop, SbLayerTop, SbLayerDesktop);
    c.clickThrough = Cfg().Bool(L"sidebar.clickThrough", false);
    c.reserve = Cfg().Bool(L"sidebar.reserve", false);
    c.layout = Cfg().Int(L"sidebar.layout", SbStandard, SbMinimal, SbCustom);
    c.sections = Cfg().Str(L"sidebar.sections", L"");
    c.levels = Cfg().Str(L"sidebar.levels", L""); c.styles = Cfg().Str(L"sidebar.styles", L"");
    c.heightPct = Cfg().Int(L"sidebar.heightPct", 100, 40, 100); c.valign = Cfg().Int(L"sidebar.valign", 0, 0, 2);
    c.spacing = Cfg().Int(L"sidebar.spacing", 1, 0, 2); c.fontScale = Cfg().Int(L"sidebar.fontScale", 100, 80, 150);
    c.background = SbBackgroundCfg(); c.tint = SbTintCfg();
    c.accent = Cfg().Int(L"sidebar.accent", -1, -1, 0xFFFFFF); c.accent2 = Cfg().Int(L"sidebar.accent2", -1, -1, 0xFFFFFF);
    c.textColor = Cfg().Int(L"sidebar.textColor", -1, -1, 0xFFFFFF); c.tintColor = Cfg().Int(L"sidebar.tintColor", -1, -1, 0xFFFFFF);
    c.glow = Cfg().Int(L"sidebar.glow", 1, 0, 2); c.animate = Cfg().Int(L"sidebar.animate", 0, 0, 1) != 0;
    c.autoHide = Cfg().Int(L"sidebar.autoHide", 0, 0, 1) != 0; c.locked = Cfg().Int(L"sidebar.locked", 1, 0, 1) != 0;
    c.posPct = Cfg().Int(L"sidebar.posPct", -1, -1, 100);
    // Desktop mode takes no screen space and no input, whatever the stored overlay options say (they are kept for the other layers)
    const SbPlacement p = SbEffectivePlacement(c.layer, SbPlacement{c.reserve, c.clickThrough, c.autoHide, c.locked});
    c.reserve = p.reserve; c.clickThrough = p.clickThrough; c.autoHide = p.autoHide; c.locked = p.locked;
    return c;
}
// One-time import of the owner's SidebarDiagnostics preferences (read-only), so the AnkhCore Sidebar
// starts the way the replaced tool looked.
void SbImportSidebarDiagnostics() {
    if (Cfg().Has(L"sidebar.importedFrom")) return;
    Cfg().SetStr(L"sidebar.importedFrom", L"none");
    const std::string j = ReadText(KnownFolder(FOLDERID_LocalAppData) + L"\\SidebarDiagnostics\\settings.json");
    if (j.empty()) { Cfg().Save(); return; }
    auto num = [&](const char* key, double& out) {
        const size_t k = j.find(std::string("\"") + key + "\"");
        if (k == std::string::npos) return false;
        const size_t c = j.find(':', k); if (c == std::string::npos) return false;
        const std::string v = j.substr(c + 1, 24);
        if (v.find("true") != std::string::npos && v.find("true") < 3) { out = 1; return true; }
        if (v.find("false") != std::string::npos && v.find("false") < 3) { out = 0; return true; }
        out = atof(v.c_str()); return true;
    };
    double v = 0;
    if (num("SidebarWidth", v) && v >= 140 && v <= 420) Cfg().SetInt(L"sidebar.width", static_cast<int>(v));
    if (num("DockEdge", v)) Cfg().SetInt(L"sidebar.edge", v == 0 ? 0 : 1);            // SD: 0 left, 2 right
    if (num("BGOpacity", v) && v > 0 && v <= 1) Cfg().SetInt(L"sidebar.opacity", std::max(10, static_cast<int>(v * 100 + 0.5)));
    if (num("ClickThrough", v)) Cfg().SetBool(L"sidebar.clickThrough", v != 0);
    if (num("AlwaysTop", v)) Cfg().SetInt(L"sidebar.layer", v != 0 ? 0 : 1);
    if (num("UseAppBar", v)) Cfg().SetBool(L"sidebar.reserve", v != 0);
    Cfg().SetStr(L"sidebar.importedFrom", L"SidebarDiagnostics");
    Cfg().Save();
    LogI(L"Desktop sidebar: initial look imported from SidebarDiagnostics settings (read-only).");
}

// One-time move of an existing setup to Desktop mode (owner decision 2026-11-12, D35). Only sidebar.layer changes;
// sidebar.prevLayer keeps the old layer so "Restore previous placement" can bring the old behaviour back.
void SbMigrateDesktopMode() {
    bool has = false; for (const wchar_t* k : {L"sidebar.layer", L"sidebar.reserve", L"sidebar.clickThrough", L"sidebar.enabled", L"sidebar.width"}) has = has || Cfg().Has(k);
    const SbLayerMove m = SbDesktopMigration(Cfg().Has(L"sidebar.desktopMigrated"), has, Cfg().Int(L"sidebar.layer", SbLayerTop, SbLayerTop, SbLayerDesktop));
    if (m.write) {
        Cfg().SetInt(L"sidebar.prevLayer", m.prevLayer); Cfg().SetInt(L"sidebar.layer", m.layer);
        LogI(L"Desktop sidebar: moved to Desktop mode (part of the desktop: no reserved space, no mouse input). The previous placement can be restored in the Sidebar settings.");
    }
    if (!Cfg().Has(L"sidebar.desktopMigrated")) { Cfg().SetInt(L"sidebar.desktopMigrated", 1); Cfg().Save(); }
}

// Creates the calling thread's next windows per-monitor DPI aware (v2, Windows 10 1607+) for the
// scope's lifetime, independent of the main window's system-DPI awareness.
struct PerMonitorDpiScope {
    using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    SetCtx set = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT prev = set ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    ~PerMonitorDpiScope() { if (set && prev) set(prev); }
};
struct MonitorInfo { HMONITOR h; std::wstring device; RECT rc, work; bool primary; };
std::vector<MonitorInfo> Monitors() {
    std::vector<MonitorInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hm, &mi)) reinterpret_cast<std::vector<MonitorInfo>*>(p)->push_back({hm, mi.szDevice, mi.rcMonitor, mi.rcWork, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
        return TRUE; }, reinterpret_cast<LPARAM>(&v));
    std::sort(v.begin(), v.end(), [](const MonitorInfo& a, const MonitorInfo& b) { return a.primary != b.primary ? a.primary : a.rc.left < b.rc.left; });
    return v;
}
MonitorInfo SbMonitor(const SidebarCfg& c) {
    const auto all = Monitors();
    for (const auto& m : all) if (!c.monitor.empty() && _wcsicmp(m.device.c_str(), c.monitor.c_str()) == 0) return m;
    if (!all.empty()) return all.front();   // the chosen monitor is gone: fall back to the primary
    MonitorInfo m{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &m.work, 0); m.rc = m.work; m.primary = true; return m;
}
UINT MonitorDpi(HMONITOR hm) {
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static Fn f = reinterpret_cast<Fn>(GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
    UINT x = 96, y = 96;
    if (!f || FAILED(f(hm, 0 /*MDT_EFFECTIVE_DPI*/, &x, &y))) return static_cast<UINT>(SystemDpi());
    return x;
}

Ui g_sbUi;
RECT g_sbOpenBtn{};
void MiniSpark(Dc dc, const RECT& rc, const Ring& s, double lo, double hi, COLORREF color, int pen);
SidebarCfg g_sb;
bool g_sbAppBar = false;
UINT WM_SB_APPBAR = WM_APP + 40;
const int kSbHotkey = 0xA5C1;
#define kSbBg col::side
D2DSurface g_widgetSurf;   // offscreen renderer shared by the Sidebar and the Mini (no window of its own)

// Desktop mode: the Sidebar is a child of the shell's wallpaper layer, the WorkerW that Progman creates behind the
// icon view (SHELLDLL_DefView). There it is drawn over the wallpaper and under the desktop icons; every window
// covers it; the icon view in front of it takes all mouse input, so desktop clicks and menus are untouched.
bool g_sbHostDeny = false;          // self-test only: act as if the shell offered no host (the fallback path)
int g_sbHostState = -1;             // last state logged: 1 hosted, 0 fallback
bool SbHostUsable(HWND w) {
    if (!w || !IsWindow(w) || !IsWindowVisible(w)) return false;
    RECT r; GetWindowRect(w, &r);
    return r.right - r.left >= GetSystemMetrics(SM_CXSCREEN) / 2 && r.bottom - r.top >= GetSystemMetrics(SM_CYSCREEN) / 2;   // the wallpaper layer spans the screens; helper WorkerWs are tiny
}
HWND g_sbHostAfter = HWND_TOP;      // the sibling the Sidebar sits directly beneath inside its host
HWND SbLookForHost(HWND progman) {
    g_sbHostAfter = HWND_TOP;
    // Windows 11 24H2 and later: Progman holds the icon view (SHELLDLL_DefView) and, beneath it, the wallpaper
    // layer (WorkerW). Windows composes that layer's wallpaper over the layer's own children, so the Sidebar
    // becomes a child of Progman, placed directly beneath the icon view and above the wallpaper layer.
    if (HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); SbHostUsable(w)) {
        if (HWND view = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr)) { g_sbHostAfter = view; return progman; }
        return w;
    }
    // earlier Windows: the icon view sits in a top-level WorkerW and the wallpaper layer is the next top-level WorkerW
    HWND host = nullptr;
    EnumWindows([](HWND top, LPARAM p) -> BOOL {
        wchar_t cls[32] = L""; GetClassNameW(top, cls, 32);
        if (wcscmp(cls, L"WorkerW") == 0 && FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) { *reinterpret_cast<HWND*>(p) = FindWindowExW(nullptr, top, L"WorkerW", nullptr); return FALSE; }
        return TRUE; }, reinterpret_cast<LPARAM>(&host));
    return SbHostUsable(host) ? host : nullptr;
}
HWND SbFindDesktopHost() {
    if (g_sbHostDeny) return nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    if (HWND w = SbLookForHost(progman)) return w;
    // No wallpaper layer yet: ask Progman to create it. This is the request the shell itself uses (for the
    // desktop slideshow and Task View); it changes nothing the user can see.
    DWORD_PTR res = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &res);
    // 2.9.6: the shell answers before the new layer is visible and full-size. Looking only once made the Sidebar fall back to a window below all
    // others, which on Windows 11 sits beneath the desktop itself: it "disappeared" when Desktop was chosen in Settings. Wait briefly (at most ~1.2 s).
    for (int i = 0; i < 12; ++i) {
        if (HWND w = SbLookForHost(progman)) return w;
        Sleep(100);
    }
    return SbLookForHost(progman);
}
// Screen position to the position SetWindowPos wants: the host's client area when the Sidebar is a child.
POINT SbWinPos(int x, int y) { POINT p{x, y}; if (g_sbHost) MapWindowPoints(nullptr, g_sbHost, &p, 1); return p; }
void SbAppBarRemove() {
    if (!g_sbAppBar || !g_sidebar) return;
    APPBARDATA ab{sizeof(ab)}; ab.hWnd = g_sidebar;
    SHAppBarMessage(ABM_REMOVE, &ab);
    g_sbAppBar = false;
}
// Positions the window; with "reserve" it negotiates the edge with the shell (ABM_QUERYPOS/SETPOS).
// Sliding (auto-hide, switching on and off): the window moves off its edge and is clipped to its docked
// rectangle, so nothing ever shows on a neighbouring screen.
RECT g_sbBase{}; int g_sbPeek = 3;                 // the docked rectangle (not slid) and the strip left visible when hidden
double g_sbSlide = 0, g_sbSlideTo = 0;             // 0 shown .. 1 hidden
bool g_sbClosing = false, g_sbDragging = false; POINT g_sbGrab{}; SbHide g_sbHide;
bool g_sbPtrTest = false; POINT g_sbPtr{};         // self-test only: stands in for the pointer, so the real mouse is never moved
bool SbMotion() { return Cfg().Int(L"appearance.motion", MotionFull, MotionFull, MotionOff) == MotionFull && !ReducedMotion(); }
void SbClip(int off) {
    if (!g_sidebar) return;
    const int w = g_sbBase.right - g_sbBase.left, h = g_sbBase.bottom - g_sbBase.top;
    if (off <= 0) { SetWindowRgn(g_sidebar, nullptr, TRUE); return; }
    SetWindowRgn(g_sidebar, g_sb.edge ? CreateRectRgn(0, 0, w - off, h) : CreateRectRgn(off, 0, w, h), TRUE);   // the system owns the region
}
void SbRender();
void SbSlideApply() {
    if (!g_sidebar) return;
    PerMonitorDpiScope pm;
    const int off = SbSlideOffset(g_sbBase.right - g_sbBase.left, g_sbPeek, g_sbSlide);
    const POINT at = SbWinPos(g_sbBase.left + (g_sb.edge ? off : -off), g_sbBase.top);
    SetWindowPos(g_sidebar, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SbClip(off);
    if (g_sbHost || off == 0) SbRender();
    if (g_sbHost && IsWindow(g_sbHost)) InvalidateRect(g_sbHost, nullptr, FALSE);
}
bool g_sbDestroyedByUs = false;   // SbDestroy is running: the WM_NCDESTROY that follows is ours, not a loss
constexpr UINT_PTR kSbLifeTimer = 11;   // one-shot timer on the main window: the next lifecycle look, only while something is pending
SbRetry g_sbHealthRetry;          // the same bounds for in-place repairs of a living window
SbRetry g_sbLife;                 // recovery of a lost window (bounded, with back-off); g_sbHealthRetry (below) bounds the in-place repairs
void SbOnUnexpectedLoss();        // below: the window vanished while it was meant to be on
// Everything that belonged to one window and must not outlive it: a half-finished slide-out or drag of a window that is
// gone used to leave "busy" set, which made the recovery and the health check skip the next window for good.
void SbResetWindowState() { g_sbClosing = false; g_sbDragging = false; g_sbSlide = g_sbSlideTo = 0; g_sbHide = SbHide(); }
void SbDestroy() {
    if (!g_sidebar) return;
    KillTimer(g_sidebar, 7); KillTimer(g_sidebar, 8); KillTimer(g_sidebar, 9);
    g_sbDestroyedByUs = true; DestroyWindow(g_sidebar); g_sbDestroyedByUs = false; g_sidebar = nullptr;
    SbResetWindowState();
}
void SbSlideTo(double target) {
    if (!g_sidebar) return;
    g_sbSlideTo = target;
    if (!SbMotion()) { g_sbSlide = target; SbSlideApply(); if (g_sbClosing && target >= 1) SbDestroy(); return; }
    SetTimer(g_sidebar, 9, 15, nullptr);
}
void SbPlace() {
    if (!g_sidebar) return;
    PerMonitorDpiScope pm;   // work areas and position in physical pixels
    const MonitorInfo m = SbMonitor(g_sb);
    const int dpi = static_cast<int>(MonitorDpi(m.h));
    const int sdpi = MulDiv(dpi, g_sb.fontScale, 100);   // the content (text, bars, gauges) scales with the text size
    if (!g_sbUi.f.title || sdpi != g_sbUi.dpi) {
        for (HFONT f : {g_sbUi.f.title, g_sbUi.f.h1, g_sbUi.f.h2, g_sbUi.f.text, g_sbUi.f.small, g_sbUi.f.tiny, g_sbUi.f.btn, g_sbUi.f.icon, g_sbUi.f.iconBig}) if (f) DeleteObject(f);
        g_sbUi.dpi = sdpi; g_sbUi.f = MakeFonts(sdpi);
    }
    const int w = MulDiv(g_sb.width, dpi, 96);            // the width is the user's own number: monitor DPI only
    RECT r{};
    if (g_sb.reserve) {
        APPBARDATA ab{sizeof(ab)}; ab.hWnd = g_sidebar; ab.uCallbackMessage = WM_SB_APPBAR;
        if (!g_sbAppBar) g_sbAppBar = SHAppBarMessage(ABM_NEW, &ab) != 0;
        ab.uEdge = g_sb.edge ? ABE_RIGHT : ABE_LEFT;
        ab.rc = m.rc;
        if (g_sb.edge) ab.rc.left = ab.rc.right - w; else ab.rc.right = ab.rc.left + w;
        SHAppBarMessage(ABM_QUERYPOS, &ab);
        if (g_sb.edge) ab.rc.left = ab.rc.right - w; else ab.rc.right = ab.rc.left + w;
        SHAppBarMessage(ABM_SETPOS, &ab);
        r = ab.rc;
        r.top = std::max(r.top, m.work.top); r.bottom = std::min(r.bottom, m.work.bottom);   // stay clear of the taskbar
    } else {
        SbAppBarRemove();
        r = m.work;
        if (g_sb.edge) r.left = r.right - w; else r.right = r.left + w;
        if (g_sb.heightPct < 100) {   // a shorter Sidebar: top, middle, bottom, or where the user dropped it
            const int hh = (r.bottom - r.top) * g_sb.heightPct / 100;
            r.top = SbTopFor(r.top, r.bottom, hh, g_sb.posPct >= 0 ? g_sb.posPct : g_sb.valign * 50); r.bottom = r.top + hh;
        }
    }
    g_sbBase = r; g_sbPeek = std::max(2, MulDiv(3, dpi, 96));
    const HWND after = g_sbHost ? g_sbHostAfter : g_sb.layer == 0 ? HWND_TOPMOST : g_sb.layer >= 2 ? HWND_BOTTOM : HWND_NOTOPMOST;
    const int off = SbSlideOffset(r.right - r.left, g_sbPeek, g_sbSlide);
    const POINT at = SbWinPos(r.left + (g_sb.edge ? off : -off), r.top);
    SetWindowPos(g_sidebar, after, at.x, at.y, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    // Ensure no usable WorkerW wallpaper child of Progman sits in front of g_sidebar
    if (g_sbHost && IsWindow(g_sbHost)) {
        for (HWND ch = GetWindow(g_sbHost, GW_CHILD); ch && ch != g_sidebar; ch = GetWindow(ch, GW_HWNDNEXT)) {
            wchar_t cls[32]{}; GetClassNameW(ch, cls, 32);
            if (wcscmp(cls, L"WorkerW") == 0 && SbHostUsable(ch)) {
                SetWindowPos(ch, g_sidebar, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
                break;
            }
        }
    }
    SbClip(off);
}
void SbApplyStyle() {
    if (!g_sidebar) return;
    const LONG_PTR oldEx = GetWindowLongPtrW(g_sidebar, GWL_EXSTYLE);
    LONG_PTR ex = oldEx;
    ex |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    ex = g_sb.clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    ex = g_sb.layer == 0 ? (ex | WS_EX_TOPMOST) : (ex & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
    if (ex != oldEx) SetWindowLongPtrW(g_sidebar, GWL_EXSTYLE, ex);
    // Desktop mode: become a child of the wallpaper layer; any other layer: a top-level window again
    PerMonitorDpiScope pm;
    const LONG_PTR st = GetWindowLongPtrW(g_sidebar, GWL_STYLE);
    HWND cur = (st & WS_CHILD) ? GetParent(g_sidebar) : nullptr;
    HWND host = g_sb.layer == SbLayerDesktop ? SbFindDesktopHost() : nullptr;
    // 2.9.7: while the shell rebuilds its wallpaper layer (a wallpaper or theme change) no host may be found for a moment. A Sidebar that is already inside a live
    // desktop window stays there; leaving it would drop it into a window below all others, which sits under the desktop itself: the Sidebar "disappeared".
    if (!host && cur && g_sb.layer == SbLayerDesktop && !g_sbHostDeny && IsWindow(cur)) host = cur;
    if (host != cur) {
        if (host) {
            SbAppBarRemove();
            SetWindowLongPtrW(g_sidebar, GWL_STYLE, (st & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD);
            {   // The desktop window has no drawing surface of its own: a child shows only when it is layered, and only
                // with ordinary painting (verified on Windows 11 26H2: a per-pixel layered update is accepted but never
                // shown there). So the layered state is started afresh in the attribute form, before joining.
                const LONG_PTR lx = GetWindowLongPtrW(g_sidebar, GWL_EXSTYLE);
                SetWindowLongPtrW(g_sidebar, GWL_EXSTYLE, lx & ~static_cast<LONG_PTR>(WS_EX_LAYERED)); SetWindowLongPtrW(g_sidebar, GWL_EXSTYLE, lx | WS_EX_LAYERED);
                SetLayeredWindowAttributes(g_sidebar, 0, 255, LWA_ALPHA);
            }
            if (!SetParent(g_sidebar, host)) { SetWindowLongPtrW(g_sidebar, GWL_STYLE, st); host = nullptr; }
        } else {
            SetParent(g_sidebar, nullptr);
            SetWindowLongPtrW(g_sidebar, GWL_STYLE, (st & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP);
            const LONG_PTR lx = GetWindowLongPtrW(g_sidebar, GWL_EXSTYLE);   // back to per-pixel transparency: the layered state starts afresh
            SetWindowLongPtrW(g_sidebar, GWL_EXSTYLE, lx & ~static_cast<LONG_PTR>(WS_EX_LAYERED)); SetWindowLongPtrW(g_sidebar, GWL_EXSTYLE, lx | WS_EX_LAYERED);
        }
    }
    g_sbHost = host;
    if (g_sbHost && (!g_sb.autoHide || g_sb.reserve) && g_sbSlide == 0) {
        SetWindowRgn(g_sidebar, nullptr, TRUE);
    }
    const int state = g_sb.layer != SbLayerDesktop ? -1 : host ? 1 : 0;
    if (state != g_sbHostState) {
        g_sbHostState = state;
        if (state == 1) LogI(L"Desktop sidebar: part of the desktop (under every window and the desktop icons).");
        else if (state == 0) LogW(L"Desktop sidebar: Windows offered no desktop layer to join; it stays below all windows instead, without reserving space.");
    }
}

// Paints into a 32-bit DIB and pushes it with UpdateLayeredWindow: the background (bg) gets the
// chosen opacity while anything drawn on it (text, bars, icons, graphs) stays fully opaque.
// cornerR > 0 cuts rounded corners (alpha 0: the desktop shows and the mouse passes there).
// Shared by the Desktop Sidebar and the Mini Monitor.
struct LayerLook { int kind = 0; COLORREF tint = col::side; int tintPct = 70; };   // kind: 0 solid, 1 frosted glass, 2 acrylic blur
struct LayerBuf {   // one widget's frame buffer: [0] Mini Monitor, [1] Sidebar
    HWND w = nullptr; int W = 0, H = 0; HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; void* bits = nullptr;
    void Free() { if (dc) { SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); } *this = LayerBuf{}; }
};
LayerBuf g_layerBuf[2]; int g_layerAllocs = 0;   // g_layerAllocs: buffers created so far (the self-test checks that drawing does not create any)
void PushLayered(HWND w, COLORREF bg, int opacityPct, int cornerR, const std::function<void(Dc, int, int)>& paint, const LayerLook* look = nullptr) {
    PerMonitorDpiScope pm;   // physical coordinates (also when called from the system-DPI main window)
    RECT wr; GetWindowRect(w, &wr);
    const int W = wr.right - wr.left, H = wr.bottom - wr.top;
    if (W <= 0 || H <= 0) return;
    // The frame buffer is kept between frames. Creating a new one every second (about 700 KB for a full-height
    // Sidebar) made Windows hand out and zero fresh memory each time: some 190 page faults a second while idle.
    LayerBuf& lb = g_layerBuf[w == g_sidebar ? 1 : 0];
    HDC sdc = GetDC(nullptr);
    if (lb.w != w || lb.W != W || lb.H != H) {
        lb.Free();
        BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB};
        lb.dc = CreateCompatibleDC(sdc);
        lb.bmp = lb.dc ? CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &lb.bits, nullptr, 0) : nullptr;
        if (!lb.bmp) { if (lb.dc) DeleteDC(lb.dc); lb = LayerBuf{}; ReleaseDC(nullptr, sdc); return; }
        lb.old = SelectObject(lb.dc, lb.bmp); lb.w = w; lb.W = W; lb.H = H; ++g_layerAllocs;
    }
    HDC dc = lb.dc; void* bits = lb.bits;
    bool ready = true;
    const bool child = (GetWindowLongPtrW(w, GWL_STYLE) & WS_CHILD) != 0;   // the Sidebar in Desktop mode
    if (g_widgetSurf.BeginBitmap(W, H)) {   // real alpha: the navy gradient takes the opacity, everything on it keeps its own
        if (child) {   // part of the desktop: the wallpaper goes underneath, so the finished frame is opaque and looks as it did over the desktop
            SbBackdropEnsure(g_sbBase);
            if (!g_sbBack.px.p.empty()) {
                const float dx = static_cast<float>(wr.left - g_sbBase.left), dy = static_cast<float>(wr.top - g_sbBase.top);   // sliding moves the window, not the desktop
                DrawPixelsPart(Dc(&g_widgetSurf), &g_sbBack, g_sbBack.px.p.data(), g_sbBack.px.w, g_sbBack.px.h, D2D1::RectF(0, 0, static_cast<float>(W), static_cast<float>(H)),
                               D2D1::RectF(dx, dy, dx + W, dy + H), 1.f);
            }
        }
        const float a = std::clamp(opacityPct, 10, 100) / 100.f, rr = static_cast<float>(cornerR);
        // Glass is simulated from the screen-aligned blurred wallpaper: a click-through layered window cannot
        // blur the live windows behind it (Phase 0). Without a look, the shared "wallpaper glass" switch decides.
        const bool glass = look ? look->kind > 0 : Cfg().Bool(L"appearance.widgetGlass", false);
        const D2D1_ROUNDED_RECT whole = D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(W), static_cast<float>(H)), rr, rr);
        if (glass) {
            const int clip = PushRoundClip(Dc(&g_widgetSurf), RECT{0, 0, W, H}, cornerR * 2);
            WallDraw(Dc(&g_widgetSurf), w, RECT{0, 0, W, H}, a);
            PopClip(Dc(&g_widgetSurf), clip);
        }
        const COLORREF top = look ? look->tint : col::side, bot = look && look->tint != col::side ? Mix(look->tint, RGB(0, 0, 0), .35) : col::bg;
        const float ta = look && glass ? a * std::clamp(look->tintPct, 0, 100) / 100.f : a;   // over glass, the tint is the user's share
        if (auto* g = g_widgetSurf.Gradient(top, ta, bot, ta, D2D1::Point2F(0, 0), D2D1::Point2F(0, static_cast<float>(H))))
            g_widgetSurf.dc->FillRoundedRectangle(whole, g);
        if (look && look->kind == 1) g_widgetSurf.dc->FillRoundedRectangle(whole, g_widgetSurf.Brush(RGB(255, 255, 255), .07f * a));   // the frost: a faint milky veil
        paint(Dc(&g_widgetSurf), W, H);
        ready = g_widgetSurf.EndBitmap(bits, W, H);
        if (ready) { WidgetFrame& wf = g_widgetFrame[w == g_sidebar ? 1 : 0]; wf.px.assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + static_cast<size_t>(W) * H); wf.w = W; wf.h = H; ++wf.seq; }
    } else {
    FillRect2(dc, RECT{0, 0, W, H}, bg);
    paint(Dc(dc), W, H);
    GdiFlush();
    // per-pixel alpha: background pixels take the opacity, drawn pixels stay opaque
    const BYTE bgA = static_cast<BYTE>(std::clamp(opacityPct, 10, 100) * 255 / 100);
    auto* px = static_cast<uint32_t*>(bits);
    const int bgR = GetRValue(bg), bgG = GetGValue(bg), bgB = GetBValue(bg);
    const long long r2 = static_cast<long long>(cornerR) * cornerR;
    for (int i = 0; i < W * H; ++i) {
        if (cornerR > 0) {   // outside a rounded corner: fully transparent
            const int x = i % W, y = i / W;
            const int cx = x < cornerR ? cornerR : x >= W - cornerR ? W - cornerR - 1 : x;
            const int cy = y < cornerR ? cornerR : y >= H - cornerR ? H - cornerR - 1 : y;
            if (cx != x && cy != y && static_cast<long long>(x - cx) * (x - cx) + static_cast<long long>(y - cy) * (y - cy) > r2) { px[i] = 0; continue; }
        }
        const uint32_t c = px[i];
        const int b = c & 0xFF, g = (c >> 8) & 0xFF, rr = (c >> 16) & 0xFF;
        const int d = std::abs(rr - bgR) + std::abs(g - bgG) + std::abs(b - bgB);
        const int a = std::min(255, bgA + (255 - bgA) * std::min(d, 60) / 60);
        px[i] = (static_cast<uint32_t>(a) << 24) | ((rr * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
    }
    }
    POINT src{0, 0}, dst{wr.left, wr.top}; SIZE sz{W, H};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (ready && child) { HDC wdc = GetDC(w); BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY); ReleaseDC(w, wdc); }   // ordinary painting; WM_PAINT repeats it from the kept buffer
    else if (ready && !UpdateLayeredWindow(w, sdc, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA)) {
        static DWORD lastErr = 0; const DWORD e = GetLastError();
        if (e != lastErr) { lastErr = e; LogW(L"A desktop widget could not be drawn (layered update failed, error " + std::to_wstring(e) + L")."); }
    }
    ReleaseDC(nullptr, sdc);
}
std::map<int, double> g_sbShown;   // animation: what each bar and gauge currently shows on its way to the real value
bool g_sbAnimBusy = false, g_sbAnimTick = false; int g_sbAnimFrames = 0;
SbLook SbLookOf(const SidebarCfg& c) {
    SbLook l; l.density = c.layout; l.levels = SbParsePerSec(c.levels, SbStandard, 0, 2); l.styles = SbParsePerSec(c.styles, SbStyleBar, 0, 3); l.spacing = c.spacing;
    return l;
}
COLORREF SbRgb(int v, COLORREF def) { return v < 0 ? def : RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }
void SbRender() {
    if (!g_sidebar) return;
    g_sbSeq = g_snapSeq;
    if (!g_sbAnimTick) g_sbAnimFrames = 0;   // a data frame (or a setting) starts a new, short run of in-between frames
    g_sbAnimTick = false; g_sbAnimBusy = false;
    const bool animate = g_sb.animate && Cfg().Int(L"appearance.motion", MotionFull, MotionFull, MotionOff) == MotionFull && !ReducedMotion();
    LayerLook look; look.kind = g_sb.background; look.tint = SbRgb(g_sb.tintColor, col::side); look.tintPct = g_sb.tint;
    PushLayered(g_sidebar, kSbBg, g_sb.opacity, g_sb.heightPct < 100 ? g_sbUi.S(12) : 0, [animate](Dc dc, int W, int H) {
    Ui& u = g_sbUi;
    const Readings& r = g_read;
    const COLORREF acc = SbRgb(g_sb.accent, col::accent), hot = SbRgb(g_sb.accent2, col::warn), txt = SbRgb(g_sb.textColor, col::text);
    const COLORREF accDeep = g_sb.accent < 0 ? col::accentDeep : Mix(col::bg, acc, .40), accDim = g_sb.accent < 0 ? col::accentDim : Mix(col::bg, acc, .62);
    const float glow = static_cast<float>(g_sb.glow);   // 0 off, 1 soft, 2 strong
    const int x = u.S(14), rw = W - u.S(28);
    // accent edge on the screen-facing side
    const int ex = g_sb.edge ? 0 : W - u.S(2);
    VGrad(dc, RECT{ex, 0, ex + u.S(2), H}, acc, accDeep);
    if (glow > 1) Halo(dc, g_sb.edge ? 0 : W, H / 4, u.S(70), acc, .10f);
    // header: ankh mark, name, clock and date
    if (glow > 0) Halo(dc, x + u.S(15), u.S(31), u.S(30), acc, .22f * glow);
    DrawIconAt(dc, g_ankhBig, x, u.S(16), u.S(30));
    Text(dc, L"AnkhCore", RECT{x + u.S(38), u.S(14), W, u.S(34)}, u.f.btn, txt);
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t clk[32]; swprintf(clk, 32, L"%02u:%02u", st.wHour, st.wMinute);
    wchar_t dat[64]; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", dat, 64, nullptr);
    Text(dc, std::wstring(clk) + L"   " + dat, RECT{x + u.S(38), u.S(34), W, u.S(52)}, u.f.tiny, acc, DT_SINGLELINE);
    Line(dc, x, u.S(62), x + rw, u.S(62), accDeep, 1);
    // Sections: what is drawn, where and in which style comes from the pure layout (sidebar_layout.h). This code only draws it.
    const int top = u.S(72), footer = g_sb.clickThrough ? u.S(38) : u.S(58);
    SbCounts cnt; cnt.cores = static_cast<int>(g_sys.cores.size()); cnt.drives = static_cast<int>(g_sys.drives.size()); cnt.fans = r.present ? static_cast<int>(r.fans.size()) : 0;
    std::vector<const GpuAdapterView*> hw; for (const GpuAdapterView& g : g_gpus) if (!g.software) hw.push_back(&g);
    cnt.gpus = static_cast<int>(hw.size()); g_sbGpuDrawn = -2;
    g_sbAvail = MulDiv(H - top - footer, 96, u.dpi);
    g_sbLayout = SbLayoutFor(SbLookOf(g_sb), SbParseSections(g_sb.sections), cnt, g_sbAvail);
    const NetSelection& n = g_netSel;
    auto isDim = [](const std::wstring& v) { return v == L"stale" || v == L"no data" || v == L"not exposed" || v == L"N/A" || v == L"unverified" || v == L"disconnected" || v == L"\u2014"; };
    // Animation: a bar or gauge moves towards its value over a few in-between frames; the text always shows the real value.
    auto shown = [&](const SbRow& row, double target) -> double {
        if (!animate || target < 0) return target;
        const int key = row.id * 16 + row.index; const auto it = g_sbShown.find(key);
        if (it == g_sbShown.end()) { g_sbShown[key] = target; return target; }
        double& cur = it->second; cur += (target - cur) * 0.55;
        if (std::abs(target - cur) < 1.5) cur = target; else g_sbAnimBusy = true;   // small changes jump: every extra frame redraws the whole Sidebar
        return cur;
    };
    // One metric, drawn in its section's style: indicator bar, gauge, large value or sparkline.
    auto metric = [&](const SbRow& row, const RECT& rr, const std::wstring& lab, const std::wstring& labShort, const std::wstring& val, const std::wstring& brief, double pctIn, COLORREF cc, int series, double lo, double hi) {
        const bool dim = isDim(val); const double pct = shown(row, dim ? 0 : std::clamp(pctIn, 0.0, 100.0));
        if (row.kind == SbGauge) {
            const int cw = rw / std::max(1, row.cols), cx = x + row.col * cw + cw / 2, cy = rr.top + u.S(33), rad = u.S(26);
            if (glow > 0 && !dim) Halo(dc, cx, cy, rad + u.S(6), cc, .09f * glow);
            GaugeRing(dc, cx, cy, rad, u.S(5), dim ? -1 : pct, cc);
            Text(dc, brief, RECT{cx - rad, cy - u.S(9), cx + rad, cy + u.S(9)}, u.f.tiny, dim ? col::faint : txt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Text(dc, labShort, RECT{cx - cw / 2 + u.S(2), rr.top + u.S(56), cx + cw / 2 - u.S(2), rr.top + u.S(72)}, u.f.tiny, col::sub, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return;
        }
        if (row.kind == SbValue) {
            Text(dc, lab, rr, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            Text(dc, val, rr, u.f.btn, dim ? col::faint : (cc == hot && pctIn >= 85 ? hot : txt), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            return;
        }
        Text(dc, lab, RECT{rr.left, rr.top, rr.right, rr.top + u.S(16)}, u.f.tiny, col::sub, DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, val, RECT{rr.left, rr.top, rr.right, rr.top + u.S(16)}, u.f.tiny, dim ? col::faint : txt, DT_RIGHT | DT_SINGLELINE);
        if (row.kind == SbSpark && series >= 0) {
            RECT g{rr.left, rr.top + u.S(18), rr.right, rr.bottom - u.S(3)};
            Round(dc, g, u.S(6), col::panel2, col::border);
            InflateRect(&g, -u.S(2), -u.S(2));
            MiniSpark(dc, g, g_store.Get(static_cast<Series>(series)), lo, hi, cc, u.S(1));
            return;
        }
        RECT track{rr.left, rr.top + u.S(18), rr.right, rr.top + u.S(23)};
        Round(dc, track, u.S(2), col::track, col::track);
        if (pct > 0) {
            RECT f = track; f.right = track.left + static_cast<int>((track.right - track.left) * pct / 100);
            BarFill(dc, f, u.S(5), Mix(col::bg, cc, .45), cc);
            if (glow > 0 && f.right > f.left + u.S(2)) Halo(dc, f.right, (f.top + f.bottom) / 2, u.S(9), cc, .16f * glow);
        }
    };
    auto text = [&](const RECT& rr, const std::wstring& lab, const std::wstring& val) {
        Text(dc, lab, rr, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER);
        Text(dc, val, rr, u.f.tiny, isDim(val) ? col::faint : txt, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    };
    auto graph = [&](RECT rr, Series a, double lo, double hi, COLORREF cc, int second) {
        rr.top += u.S(3); rr.bottom -= u.S(3);
        Round(dc, rr, u.S(8), col::panel2, col::border);
        InflateRect(&rr, -u.S(3), -u.S(2));
        if (second >= 0) MiniSpark(dc, rr, g_store.Get(static_cast<Series>(second)), lo, hi, accDim, u.S(1));
        MiniSpark(dc, rr, g_store.Get(a), lo, hi, cc, u.S(1));
    };
    auto clockPower = [&](double mhz, double watts) -> std::wstring {
        if (!r.present) return Live(-1, true, L"");
        if (mhz < 0 && watts < 0) return L"not exposed";
        std::wstring o; wchar_t b[32];
        if (mhz >= 0) { swprintf(b, 32, L"%.2f GHz", mhz / 1000); o = b; }
        if (watts >= 0) { swprintf(b, 32, L"%.0f W", watts); o += (o.empty() ? L"" : L" \u00B7 ") + std::wstring(b); }
        return o;
    };
    auto temp = [&](const SbRow& row, const RECT& rr, double t, Series sr) {
        const double dt = DispTemp(t);
        const std::wstring val = Live(dt, true, TempSuffix());
        metric(row, rr, L"Temperature", L"Temp", val, r.present && t >= 0 ? NA(dt, L"\u00B0") : val, r.present ? t : 0, hot, sr, 30, 100);
    };
    auto pair = [](uint64_t a2, uint64_t b2) { return FormatBytes(a2) + L" / " + FormatBytes(b2); };
    double fanHi = 1000; { double a2 = 0, b2 = 0; if (g_store.Get(SFan).MinMax(a2, b2)) fanHi = std::max(1000.0, b2 * 1.1); }
    static const wchar_t* const glyph[SbSecCount] = {L"\xE950", L"\xF211", nullptr, L"\xEDA2", L"\xE839", L"\xE9CA"};
    for (const SbBox& box : g_sbLayout.boxes) {
        const int by = top + u.S(box.top);
        Round(dc, RECT{x - u.S(6), by, x + rw + u.S(6), by + u.S(box.h)}, u.S(14), col::panel, col::border);
        Text(dc, box.sec == SecMem ? GlyphMemory() : glyph[box.sec], RECT{x, by + u.S(4), x + u.S(18), by + u.S(4 + kSbTitleH - 2)}, u.f.icon, acc, DT_SINGLELINE | DT_VCENTER);
        Text(dc, SbSecTitle(box.sec), RECT{x + u.S(22), by + u.S(4), x + rw, by + u.S(4 + kSbTitleH - 2)}, u.f.tiny, acc, DT_SINGLELINE | DT_VCENTER);
        for (const SbRow& row : box.rows) {
            const RECT rr{x, top + u.S(row.top + 4), x + rw, top + u.S(row.top + 4 + row.h)};
            switch (row.id) {
            case RCpuLoad: metric(row, rr, L"Usage", L"Usage", NA(g_usage.cpu, L"%"), NA(g_usage.cpu, L"%"), g_usage.cpu, acc, SCpuLoad, 0, 100); break;
            case RCpuTemp: temp(row, rr, r.cpuTemp, SCpuTemp); break;
            case RCpuInfo: text(rr, L"Clock \u00B7 power", clockPower(r.cpuClock, r.cpuPower)); break;
            case RCpuGraph: graph(rr, SCpuLoad, 0, 100, acc, -1); break;
            case RCpuCores: {   // one small bar per logical processor
                const int per = SbCoresPerRow(cnt.cores), cw = rw / per, ch = u.S(kSbCoreH);
                for (int i = 0; i < cnt.cores; ++i) {
                    const int cx = x + (i % per) * cw, cy = rr.top + u.S(2) + (i / per) * ch;
                    RECT cell{cx, cy, cx + cw - u.S(3), cy + ch - u.S(4)};
                    Round(dc, cell, u.S(2), col::track, col::track);
                    const double v = std::clamp(g_sys.cores[static_cast<size_t>(i)], 0.0, 100.0);
                    RECT f = cell; f.right = cell.left + static_cast<int>((cell.right - cell.left) * v / 100);
                    if (f.right > f.left) BarFill(dc, f, u.S(2), Mix(col::bg, acc, .45), v >= 90 ? hot : acc);
                }
                break;
            }
            case RCpuTopo: text(rr, L"Processor", g_sys.logical ? std::to_wstring(g_sys.physCores) + L" cores \u00B7 " + std::to_wstring(g_sys.logical) + L" threads" : std::wstring(L"no data")); break;
            case RGpuLoad: { const double v = g_usage.gpu; g_sbGpuDrawn = v; metric(row, rr, L"Usage", L"Usage", NA(v, L"%"), NA(v, L"%"), v, acc, SGpuLoad, 0, 100); break; }
            case RGpuEach: {   // each adapter's own utilization (Windows counters, matched by LUID)
                if (static_cast<size_t>(row.index) >= hw.size()) break;
                const GpuAdapterView& g = *hw[static_cast<size_t>(row.index)]; const std::wstring val = NA(g.util, L"%");
                Text(dc, g.name, RECT{rr.left, rr.top, rr.right - u.S(40), rr.bottom}, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                Text(dc, val, rr, u.f.tiny, isDim(val) ? col::faint : txt, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                break;
            }
            case RGpuTemp: temp(row, rr, r.gpuTemp, SGpuTemp); break;
            case RGpuInfo: text(rr, L"Clock \u00B7 power", clockPower(r.gpuClock, r.gpuPower)); break;
            case RGpuVram: {   // the adapter with the most dedicated memory (Windows counters)
                const GpuAdapterView* big = nullptr; for (const GpuAdapterView* g : hw) if (!big || g->dedicatedTotal > big->dedicatedTotal) big = g;
                text(rr, L"VRAM used", big && big->dedicatedTotal ? FormatBytes(big->dedicatedUsed) + L" / " + FormatBytes(big->dedicatedTotal) : std::wstring(L"no data"));
                break;
            }
            case RGpuGraph: graph(rr, SGpuLoad, 0, 100, acc, -1); break;
            case RMemLoad: metric(row, rr, L"In use", L"In use", NA(g_usage.ram, L"%"), NA(g_usage.ram, L"%"), g_usage.ram, acc, SRam, 0, 100); break;
            case RMemUsed: text(rr, L"Used / total", pair(g_usage.ramUsed, g_usage.ramTotal)); break;
            case RMemAvail: text(rr, L"Free / cached", g_sys.logical ? pair(g_sys.memAvail, g_sys.memCached) : std::wstring(L"no data")); break;
            case RMemCommit: text(rr, L"Committed", g_sys.logical ? pair(g_sys.commitUsed, g_sys.commitLimit) : std::wstring(L"no data")); break;
            case RMemGraph: graph(rr, SRam, 0, 100, accDim, -1); break;
            case RDrive: {
                if (static_cast<size_t>(row.index) < g_sys.drives.size()) {
                    const DriveInfo& d = g_sys.drives[static_cast<size_t>(row.index)];
                    const double pct = 100.0 * static_cast<double>(d.total - d.free) / static_cast<double>(std::max<uint64_t>(1, d.total));
                    const std::wstring name = std::wstring(1, d.letter) + L":";
                    metric(row, rr, name + L"  " + FormatBytes(d.free) + L" free", name, NA(pct, L"%"), NA(pct, L"%"), pct, pct >= 90 ? hot : acc, -1, 0, 100);
                } else metric(row, rr, L"System drive", L"Drive", NA(g_usage.disk, L"%"), NA(g_usage.disk, L"%"), g_usage.disk, acc, -1, 0, 100);
                break;
            }
            case RDiskIo: text(rr, L"Read \u00B7 write", g_sys.diskRead < 0 ? std::wstring(L"\u2014") : FormatBytes(static_cast<uint64_t>(g_sys.diskRead)) + L"/s \u00B7 " + FormatBytes(static_cast<uint64_t>(std::max(0.0, g_sys.diskWrite))) + L"/s"); break;
            case RDiskTemp: text(rr, L"Temperature", Live(r.diskTemp, true, L" \u00B0C")); break;
            case RNetHead: {
                std::wstring head = n.valid ? n.name : std::wstring(L"Network");
                if (n.valid && n.mode != NetAll && n.type && head.rfind(NetTypeName(n.type), 0) != 0) head += std::wstring(L" \u00B7 ") + NetTypeName(n.type);   // "Wi-Fi 5" already says it
                if (n.signal >= 0) head += L" " + std::to_wstring(n.signal) + L"%";
                Text(dc, head, rr, u.f.tiny, txt, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                break;
            }
            case RNetSpeed: Text(dc, NetSpeedText(L"   "), rr, u.f.tiny, n.connected ? acc : col::faint, DT_SINGLELINE | DT_VCENTER); break;
            case RNetDown: case RNetUp: {   // 2.9.6: one direction in the section's style; the ring and bar are measured against the link speed, or the recent peak when it is unknown
                const bool dn = row.id == RNetDown; const double v = dn ? n.down : n.up;
                double lo2 = 0, hi2 = 0, a2 = 0, b2 = 0; g_store.Get(SNetDown).MinMax(lo2, hi2); if (g_store.Get(SNetUp).MinMax(a2, b2)) hi2 = std::max(hi2, b2);
                const double peak = std::max(1024.0, hi2 * 1.1), scale = n.linkBps && n.mode != NetAll ? std::max(1.0, n.linkBps / 8.0) : peak;
                const std::wstring val = !n.valid ? std::wstring(L"no data") : !n.connected ? std::wstring(L"disconnected") : NetRate(v);
                const std::wstring brief = !n.valid || !n.connected || v < 0 ? val : FormatBytes(static_cast<uint64_t>(v));
                metric(row, rr, dn ? L"Download" : L"Upload", dn ? L"Down" : L"Up", val, brief, v < 0 ? 0 : 100.0 * v / scale, dn ? acc : col::ok, dn ? SNetDown : SNetUp, 0, peak);
                break; }
            case RNetTotals: { const int p = NetPeriodCfg();
                Text(dc, std::wstring(NetPeriodName(p)) + L" \u2193" + FormatBytes(n.totals[p].rx) + L" \u2191" + FormatBytes(n.totals[p].tx), rr, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                break; }
            case RNetLink: text(rr, L"Link", !n.valid ? std::wstring(L"no data") : !n.connected ? std::wstring(L"disconnected") : n.mode == NetAll ? std::to_wstring(n.count) + L" connected"
                                             : n.linkBps >= 1000000000ull ? NA(n.linkBps / 1e9, L" Gbps", 1) : n.linkBps ? NA(n.linkBps / 1e6, L" Mbps") : std::wstring(L"\u2014")); break;
            case RNetGraph: { double lo = 0, hi = 0, a2 = 0, b2 = 0; g_store.Get(SNetDown).MinMax(lo, hi); if (g_store.Get(SNetUp).MinMax(a2, b2)) hi = std::max(hi, b2);
                graph(rr, SNetDown, 0, std::max(1024.0, hi * 1.1), col::ok, SNetUp); break; }
            case RFanTop: {
                double rpm = -1; bool live = false; for (const auto& f : r.fans) if (f.rpm > rpm) { rpm = f.rpm; live = f.live; }
                const std::wstring val = !r.present ? Live(-1, true, L"") : rpm < 0 ? std::wstring(L"not exposed") : NA(rpm, L" rpm");
                metric(row, rr, L"Fastest fan", L"Fan rpm", val, rpm < 0 || !r.present ? val : NA(rpm, L""), r.present && rpm > 0 ? std::min(100.0, rpm / 60.0) : 0, live ? col::ok : col::faint, SFan, 0, fanHi);
                break;
            }
            case RFan: {
                if (!r.present || static_cast<size_t>(row.index) >= r.fans.size()) { const std::wstring v = Live(-1, true, L""); metric(row, rr, L"Fan", L"Fan", v, v, 0, col::faint, -1, 0, 100); break; }
                const FanReading& f = r.fans[static_cast<size_t>(row.index)];
                const std::wstring nm = f.name.empty() ? std::wstring(L"Fan") : f.name, val = f.rpm < 0 ? std::wstring(L"not exposed") : NA(f.rpm, L" rpm");
                metric(row, rr, nm + (f.live ? L"" : L" (unverified)"), nm, val, f.rpm < 0 ? val : NA(f.rpm, L""), f.rpm > 0 ? std::min(100.0, f.rpm / 60.0) : 0, f.live ? col::ok : col::faint, -1, 0, 100);
                break;
            }
            case RFanMode: text(rr, L"Mode", !r.present ? Live(-1, true, L"") : r.fanMode.empty() ? std::wstring(L"not exposed") : r.fanMode); break;
            }
        }
    }
    // interactive mode only: open button and a hint how to switch modes
    if (!g_sb.clickThrough) {
        g_sbOpenBtn = RECT{x, H - u.S(46), x + rw, H - u.S(14)};
        Round(dc, g_sbOpenBtn, u.S(8), acc, acc);
        Text(dc, L"Open AnkhCore", g_sbOpenBtn, u.f.btn, col::bg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        g_sbOpenBtn = RECT{};
        Text(dc, L"Click-through  \x00B7  Ctrl+Alt+Shift+S", RECT{x, H - u.S(30), x + rw, H - u.S(12)}, u.f.tiny, col::faint, DT_SINGLELINE);
    }
    }, &look);
    if (animate && g_sbAnimBusy && g_sbAnimFrames < 3) { ++g_sbAnimFrames; SetTimer(g_sidebar, 7, 60, nullptr); }   // at most three in-between frames per data frame
}
void SbApplyAll() {
    if (!g_sidebar || g_sbClosing) return;
    SbRetryReset(g_sbHealthRetry);
    SbApplyStyle(); SbPlace(); SbRender();
    if (g_sidebarOn && !g_sbHide.hidden && !IsWindowVisible(g_sidebar)) {
        ShowWindow(g_sidebar, SW_SHOWNOACTIVATE);
    }
    const bool hide = g_sb.autoHide && !g_sb.reserve;   // a reserved edge would stay empty: no auto-hide there
    if (hide) SetTimer(g_sidebar, 8, 120, nullptr);
    else {
        KillTimer(g_sidebar, 8);
        KillTimer(g_sidebar, 9);
        if (g_sbHide.hidden || g_sbSlide != 0) {
            g_sbHide = SbHide();
            g_sbSlide = g_sbSlideTo = 0;
            SbClip(0);
            SbPlace();
            SbRender();
        }
    }
}
// A drag ended at `p` (screen pixels): the Sidebar docks to that screen's nearer side, at that height.
void SbDropAt(POINT p, int grabDy) {
    if (!g_sidebar) return;
    std::vector<SbRect> works; std::vector<MonitorInfo> mons; int h = 0;
    { PerMonitorDpiScope pm; mons = Monitors(); for (const auto& m : mons) works.push_back({m.work.left, m.work.top, m.work.right, m.work.bottom}); RECT wr; GetWindowRect(g_sidebar, &wr); h = wr.bottom - wr.top; }
    if (mons.empty()) return;
    const SbDrop d = SbDropTarget(works, p.x, p.y, h, grabDy);
    Cfg().SetStr(L"sidebar.monitor", mons[static_cast<size_t>(d.monitor)].device); Cfg().SetInt(L"sidebar.edge", d.edge); Cfg().SetInt(L"sidebar.posPct", d.posPct);
    SbSetOption(L"", 0);
}
void SbSetClickThrough(bool on) {
    Cfg().SetBool(L"sidebar.clickThrough", on); Cfg().Save(); g_sb = SbLoad();   // Desktop mode never takes the mouse; the choice is kept for the overlay layers
    SbApplyAll();
    LogI(std::wstring(L"Desktop sidebar: ") + (on ? L"click-through (mouse passes to the desktop)" : L"interactive"));
}

void SbLifeArm(ULONGLONG ms);
SbHealthInput SbHealthFacts();
bool g_sbReplacePending = false, g_sbGaveUpLogged = false;
UINT g_sbLastMsg = 0;
SbSyncState SbCaptureSyncState() {
    SbSyncState st;
    st.userRequestedVisible = g_sidebarOn;
    st.actualWindowVisible = (g_sidebar && IsWindow(g_sidebar) && IsWindowVisible(g_sidebar) != FALSE);
    st.desktopIntegrationActive = (g_sbHost != nullptr && g_sidebar && (GetWindowLongPtrW(g_sidebar, GWL_STYLE) & WS_CHILD) != 0);
    st.autoHideActive = (g_sb.autoHide && !g_sb.reserve);
    st.positionLocked = g_sb.locked;
    st.currentMonitor = g_sb.monitor;
    st.currentPosition = g_sbBase;
    st.lastMessage = g_sbLastMsg;
    const SbHealthInput hi = SbHealthFacts();
    st.lifecycleState = SbComputeState(g_sidebarOn, hi.haveWindow, hi.visible || st.autoHideActive, g_sb.layer == SbLayerDesktop, hi.hosted && hi.parentIsHost && hi.beneathIcons, hi.hostAvailable && hi.hostVisible, g_sbLife.gaveUp || g_sbHealthRetry.gaveUp);
    if (!hi.haveWindow) st.lastDisappearReason = SbDisappearReason::InvalidHandle;
    else if (!hi.visible && !st.autoHideActive) st.lastDisappearReason = SbDisappearReason::HiddenBySystem;
    else if (!hi.parentIsHost) st.lastDisappearReason = SbDisappearReason::DetachedFromHost;
    else if (!hi.beneathIcons) st.lastDisappearReason = SbDisappearReason::CoveredByWorkerW;
    else if (!hi.onScreen) st.lastDisappearReason = SbDisappearReason::OffScreen;
    return st;
}
void SbLogDiagnostics(const wchar_t* reason) {
    const SbSyncState st = SbCaptureSyncState();
    HWND par = (g_sidebar && IsWindow(g_sidebar)) ? GetParent(g_sidebar) : nullptr;
    HWND prev = (g_sidebar && IsWindow(g_sidebar)) ? GetWindow(g_sidebar, GW_HWNDPREV) : nullptr;
    HWND next = (g_sidebar && IsWindow(g_sidebar)) ? GetWindow(g_sidebar, GW_HWNDNEXT) : nullptr;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g_sidebar && IsWindow(g_sidebar)) GetWindowPlacement(g_sidebar, &wp);
    wchar_t buf[512];
    swprintf(buf, 512,
        L"Desktop sidebar diag [%ls]: state=%ls disappear=%ls hwnd=%p parent=%p host=%p after=%p prev=%p next=%p vis=%d req=%d hosted=%d autoHide=%d locked=%d showCmd=%u rect=(%ld,%ld,%ld,%ld) lastMsg=0x%X",
        reason ? reason : L"",
        SbStateName(st.lifecycleState), SbDisappearReasonName(st.lastDisappearReason),
        g_sidebar, par, g_sbHost, g_sbHostAfter, prev, next,
        st.actualWindowVisible ? 1 : 0, st.userRequestedVisible ? 1 : 0,
        st.desktopIntegrationActive ? 1 : 0, st.autoHideActive ? 1 : 0, st.positionLocked ? 1 : 0,
        wp.showCmd, wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
        st.lastMessage);
    LogW(buf);
}
LRESULT CALLBACK SidebarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SHOWWINDOW || m == WM_WINDOWPOSCHANGING || m == WM_WINDOWPOSCHANGED ||
        m == WM_ACTIVATE || m == WM_DISPLAYCHANGE || m == WM_SETTINGCHANGE || m == WM_NCDESTROY) {
        g_sbLastMsg = m;
    }
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never steal focus
    case WM_NCHITTEST: return g_sb.clickThrough ? HTTRANSPARENT : HTCLIENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {   // only a desktop-hosted Sidebar is painted this way; the other layers are pushed whole
        PAINTSTRUCT ps; HDC pdc = BeginPaint(h, &ps);
        const LayerBuf& lb = g_layerBuf[1];
        if (lb.w == h && lb.dc && (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)) BitBlt(pdc, 0, 0, lb.W, lb.H, lb.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&g_sbOpenBtn, pt)) { ShowWindow(g_wnd, SW_SHOW); ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); }
        else if (!g_sb.locked && !g_sb.reserve) {   // unlocked: drag to another screen, side or height
            PerMonitorDpiScope pm; POINT c2; GetCursorPos(&c2); RECT wr; GetWindowRect(h, &wr);
            g_sbGrab = POINT{c2.x - wr.left, c2.y - wr.top}; g_sbDragging = true; SetCapture(h); SetWindowRgn(h, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_sbDragging) {
            PerMonitorDpiScope pm; POINT c2; GetCursorPos(&c2);
            const POINT at = SbWinPos(c2.x - g_sbGrab.x, c2.y - g_sbGrab.y);
            SetWindowPos(h, nullptr, at.x, at.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (g_sbDragging) {
            g_sbDragging = false; if (m == WM_LBUTTONUP) ReleaseCapture();
            POINT c2; { PerMonitorDpiScope pm; GetCursorPos(&c2); }
            SbDropAt(c2, g_sbGrab.y);
        }
        return 0;
    case WM_LBUTTONDBLCLK: ShowWindow(g_wnd, SW_SHOW); ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); return 0;
    case WM_RBUTTONUP: SbSetClickThrough(true); return 0;   // right-click: hand the mouse back to the desktop
    case WM_WINDOWPOSCHANGING:
        if (g_sbHost && g_sbHostAfter != HWND_TOP && IsWindow(g_sbHostAfter)) reinterpret_cast<WINDOWPOS*>(l)->hwndInsertAfter = g_sbHostAfter;   // hosted: always directly beneath the icon view
        else if (!g_sbHost && g_sb.layer >= 2) reinterpret_cast<WINDOWPOS*>(l)->hwndInsertAfter = HWND_BOTTOM;   // below-windows layer, and Desktop mode without a host: stay under windows
        break;
    case WM_DPICHANGED: case WM_DISPLAYCHANGE: SbApplyAll(); return 0;
    case WM_SETTINGCHANGE: if (w == SPI_SETWORKAREA && !g_sb.reserve) SbApplyAll(); return 0;
    case WM_HOTKEY: if (w == kSbHotkey) SbSetClickThrough(!g_sb.clickThrough); return 0;
    case WM_TIMER:
        if (w == 7) { KillTimer(h, 7); g_sbAnimTick = true; SbRender(); }   // an in-between animation frame
        else if (w == 8) {   // auto-hide: where is the pointer?
            POINT c2; { PerMonitorDpiScope pm; GetCursorPos(&c2); }
            if (g_sbPtrTest) c2 = g_sbPtr;
            RECT strip = g_sbBase;
            const int actW = std::max(g_sbPeek + 4, g_sbUi.S(8));
            if (g_sb.edge) { strip.left = strip.right - actW; strip.right += 2; }
            else { strip.right = strip.left + actW; strip.left -= 2; }
            const bool inside = !g_sbHide.hidden && PtInRect(&g_sbBase, c2), atEdge = PtInRect(&strip, c2) != 0;
            if (SbHideNext(g_sbHide, inside, atEdge, g_sbDragging || g_sbClosing, GetTickCount64(), 700)) SbSlideTo(g_sbHide.hidden ? 1 : 0);
        } else if (w == 9) {   // one step of a slide (about 180 ms end to end)
            const double step = 15.0 / 180.0;
            g_sbSlide = g_sbSlide < g_sbSlideTo ? std::min(g_sbSlideTo, g_sbSlide + step) : std::max(g_sbSlideTo, g_sbSlide - step);
            SbSlideApply();
            if (g_sbSlide == g_sbSlideTo) { KillTimer(h, 9); if (g_sbClosing && g_sbSlide >= 1) SbDestroy(); }
        }
        return 0;
    case WM_DESTROY: UnregisterHotKey(h, kSbHotkey); return 0;
    case WM_SHOWWINDOW:   // FT-D01 diagnostics: something other than AnkhCore hid the window
        if (!w && h == g_sidebar && g_sidebarOn && !g_sbClosing && !g_sbHide.hidden) {
            SbLogDiagnostics(L"WM_SHOWWINDOW(hide)");
            g_sbReplacePending = true;
            SbLifeArm(150);
        }
        break;
    case WM_WINDOWPOSCHANGED:
        if (h == g_sidebar && g_sidebarOn && !g_sbClosing && !g_sbDragging && !g_sbHide.hidden) {
            const auto* wp = reinterpret_cast<const WINDOWPOS*>(l);
            const bool hid = !IsWindowVisible(h) || (wp && (wp->flags & SWP_HIDEWINDOW));
            const bool parentLost = g_sbHost && GetParent(h) != g_sbHost;
            const bool zLost = g_sbHost && g_sbHostAfter != HWND_TOP && IsWindow(g_sbHostAfter) && GetWindow(h, GW_HWNDPREV) != g_sbHostAfter;
            if (hid || parentLost || zLost) {
                SbLogDiagnostics(hid ? L"WM_WINDOWPOSCHANGED(invisible)" : parentLost ? L"WM_WINDOWPOSCHANGED(parent-lost)" : L"WM_WINDOWPOSCHANGED(z-order)");
                g_sbReplacePending = true;
                SbLifeArm(150);
            }
        }
        break;
    case WM_NCDESTROY:   // also reached when the desktop host goes away (Explorer restarting takes its child windows with it)
        if (h == g_sidebar) {
            const SbEnd why = SbClassifyEnd(g_sidebarOn, g_quitting, g_sbClosing, g_sbDestroyedByUs);
            g_sidebar = nullptr; g_sbHost = nullptr; g_sbAppBar = false; g_sbHostState = -1;
            if (!g_sbDestroyedByUs) SbResetWindowState();   // SbDestroy does its own reset after DestroyWindow returns
            if (why == SbEnd::Unexpected) SbOnUnexpectedLoss();
        }
        break;
    }
    if (m == WM_SB_APPBAR) {   // shell notifications for the reserved edge
        if (w == ABN_POSCHANGED) SbApplyAll();
        else if (w == ABN_FULLSCREENAPP) {
            if (l) SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            else SbApplyAll();
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
// Creates the window and brings it up (class, per-monitor DPI, hot key, placement). False when Windows refuses; the
// caller decides what that means (the user's switch-on: it stays off; an automatic recovery: it tries again later).
bool SbCreateWindow(bool slideIn) {
    if (g_sidebar && IsWindow(g_sidebar)) return true;   // never create a duplicate Sidebar instance
    g_engine.Monitor(true);   // the sidebar needs live readings (no-op when monitoring already runs)
    g_sb = SbLoad();
    SbResetWindowState();     // a new window starts with a clean slate
    static bool reg = false;
    static const std::wstring cls = Tagged(L"AnkhCoreSidebar");
    if (!reg) { WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = SidebarProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls.c_str(); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS; RegisterClassExW(&wc); reg = true; }
    // the widget is per-monitor DPI aware on its own (Windows 10 1607+), independent of the main window
    using SetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static SetCtx setCtx = reinterpret_cast<SetCtx>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT prev = setCtx ? setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    g_sidebar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.c_str(), L"AnkhCore Sidebar",
                                WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (setCtx && prev) setCtx(prev);
    if (!g_sidebar) return false;
    RegisterHotKey(g_sidebar, kSbHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S');
    g_sbHide = SbHide(); g_sbSlide = slideIn && SbMotion() ? 1 : 0;   // switched on by the user: it slides in
    SbApplyAll();
    SbSlideTo(0);
    return true;
}
void ToggleSidebar(bool on, bool persist) {
    if (on == g_sidebarOn) return;
    g_sidebarOn = on;
    SbRetryReset(g_sbLife);   // the user's own choice (or the start/exit path) ends any recovery in progress
    if (g_wnd && IsWindow(g_wnd)) KillTimer(g_wnd, kSbLifeTimer);
    if (persist) { Cfg().SetBool(L"sidebar.enabled", on); Cfg().Save(); }
    if (!on) {   // switched off by the user: it slides away first; at Exit (persist = false) it goes at once
        if (g_sidebar) { SbAppBarRemove(); KillTimer(g_sidebar, 8); if (persist && SbMotion()) { g_sbClosing = true; SbSlideTo(1); } else SbDestroy(); }
        return;
    }
    if (g_sidebar) { g_sb = SbLoad(); g_engine.Monitor(true); g_sbClosing = false; g_sbHide = SbHide(); SbApplyAll(); SbSlideTo(0); return; }   // switched back on while sliding away
    if (!SbCreateWindow(persist)) { g_sidebarOn = false; LogE(L"The desktop sidebar could not be created."); }
}

// ---------------------------------------------------------------- lifecycle: a Sidebar that is meant to be on but whose window is gone
// Why the window can be gone (source evidence): in Desktop mode it is a child of Progman / the wallpaper WorkerW, so
// Explorer exiting destroys it (WM_NCDESTROY, SidebarProc); another program can also close it. Recovery used to run
// only from the engine's once-a-second frame (a second process), which stops while the engine is down, and it
// recreated the window without any limit or back-off. Now the loss itself starts a one-shot timer (no polling);
// attempts are bounded (src/sidebar_lifecycle.h) and the user's own off/on, the slide-out and Exit never trigger one.
void SbLifeArm(ULONGLONG ms) { if (g_wnd && IsWindow(g_wnd) && !g_quitting) SetTimer(g_wnd, kSbLifeTimer, static_cast<UINT>(std::max<ULONGLONG>(50, std::min<ULONGLONG>(ms, 60000))), nullptr); }
std::atomic<bool> g_sbExplorerRestarting{false};   // 2.8.6: AnkhCore itself is restarting Explorer: the Sidebar is not re-created until the new shell is up
void SbOnUnexpectedLoss() {
    const ULONGLONG now = GetTickCount64();
    SbRetryLoss(g_sbLife, now);
    g_sbGaveUpLogged = false;
    if (g_sbExplorerRestarting) LogI(L"Desktop sidebar: Explorer is restarting; the Sidebar comes back as soon as the new desktop is ready.");
    else LogW(L"Desktop sidebar: its window was destroyed (Explorer restarting, or another program closed it). It is reopened automatically.");
    SbLifeArm(std::max<ULONGLONG>(500, g_sbLife.nextAt > now ? g_sbLife.nextAt - now : 0));   // a moment for the shell to settle
}
// One look at the Sidebar's lifecycle; safe to call from anywhere on the UI thread, any number of times.
void SbEnsure() {
    if (g_wnd && IsWindow(g_wnd)) KillTimer(g_wnd, kSbLifeTimer);
    if (g_sbExplorerRestarting) { SbLifeArm(700); return; }   // 2.8.6: wait for the new shell; no attempt is spent while Explorer is down
    if (g_sidebar && !IsWindow(g_sidebar)) { g_sidebar = nullptr; g_sbHost = nullptr; g_sbAppBar = false; SbResetWindowState(); }   // a stale handle is never trusted
    const ULONGLONG now = GetTickCount64();
    const SbLifeFacts f{g_sidebarOn, g_quitting, g_sbClosing, g_sidebar != nullptr};
    const SbLifeVerdict v = SbLifeDecide(f, g_sbLife, now);
    switch (v.act) {
    case SbLifeAct::Wait: SbLifeArm(v.waitMs); break;
    case SbLifeAct::GiveUp:
        if (!g_sbGaveUpLogged) { g_sbGaveUpLogged = true; LogW(L"Desktop sidebar: not reopened automatically after repeated failures. It is tried again when Windows reports a change, or switch it off and on."); }
        break;
    case SbLifeAct::Recreate: {
        SbRetryAttempt(g_sbLife, now);
        const int n = g_sbLife.tries;
        const bool ok = SbCreateWindow(false);
        if (ok) LogOk(L"Desktop sidebar: reopened (attempt " + std::to_wstring(n) + L" of " + std::to_wstring(kSbRetryMax) + L").");
        else LogW(L"Desktop sidebar: could not be created (attempt " + std::to_wstring(n) + L" of " + std::to_wstring(kSbRetryMax) + L").");
        if (!ok) {
            if (g_sbLife.gaveUp) { g_sbGaveUpLogged = true; LogW(L"Desktop sidebar: not reopened automatically after repeated failures. It is tried again when Windows reports a change, or switch it off and on."); }
            else SbLifeArm(g_sbLife.nextAt > now ? g_sbLife.nextAt - now : 1000);
        }
        break;
    }
    default: break;
    }
}
bool SbRecoveryFailed() { return g_sidebarOn && !g_sidebar && g_sbLife.gaveUp; }
const bool g_sbRestartHookSet = [] {   // 2.8.6: runs on the worker that restarts Explorer; the one-shot lifecycle timer is then fired on the UI thread
    ExplorerRestartHook() = [](bool starting) {
        g_sbExplorerRestarting = starting;
        if (!starting && g_wnd && !g_quitting) { Sleep(1500); PostMessageW(g_wnd, WM_TIMER, kSbLifeTimer, 0); }   // the new shell's desktop layer needs a moment, then the Sidebar is put back at once
    };
    return true;
}();
// FT-D01: the Desktop-layer Sidebar is checked once a second (the engine frame) and put back when it is not where it should be.
// The facts are collected here (Windows calls only, nothing is created or sent to the shell); src/sidebar_layout.h decides.
bool g_sbHealthGaveUpLogged = false;
int g_sbRecoveries = 0; std::wstring g_sbHealthLast; ULONGLONG g_sbHealthTried = 0; int g_sbWaiting = 0;
int SbWindowCount() {   // Sidebar windows of this program anywhere on the desktop (a duplicate would show here)
    const std::wstring cls = Tagged(L"AnkhCoreSidebar"); int n = 0;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, cls.c_str(), nullptr)) != nullptr;) ++n;
    if (HWND pm = FindWindowW(L"Progman", nullptr)) {
        for (HWND w = nullptr; (w = FindWindowExW(pm, w, cls.c_str(), nullptr)) != nullptr;) ++n;
        for (HWND wk = nullptr; (wk = FindWindowExW(pm, wk, L"WorkerW", nullptr)) != nullptr;)
            for (HWND w = nullptr; (w = FindWindowExW(wk, w, cls.c_str(), nullptr)) != nullptr;) ++n;
    }
    return n;
}
SbHealthInput SbHealthFacts() {
    SbHealthInput i;
    i.wantDesktop = g_sidebarOn && g_sb.layer == SbLayerDesktop;
    i.haveWindow = g_sidebar && IsWindow(g_sidebar);
    i.busy = g_sbClosing || g_sbDragging;
    if (!i.wantDesktop || !i.haveWindow) return i;
    HWND progman = g_sbHostDeny ? nullptr : FindWindowW(L"Progman", nullptr);
    HWND host = progman ? SbLookForHost(progman) : nullptr;   // a quiet look: it never asks the shell to create a layer
    i.hostAvailable = host != nullptr;
    i.hostVisible = !host || IsWindowVisible(host) != FALSE;
    i.hosted = g_sbHost != nullptr && (GetWindowLongPtrW(g_sidebar, GWL_STYLE) & WS_CHILD) != 0;
    i.parentIsHost = i.hosted && host && g_sbHost == host && GetParent(g_sidebar) == host;
    i.visible = IsWindowVisible(g_sidebar) != FALSE;
    {
        PerMonitorDpiScope pm;
        RECT wr{}; GetWindowRect(g_sidebar, &wr);
        const MonitorInfo m = SbMonitor(g_sb);
        const RECT& checkRect = (g_sb.autoHide && (g_sbHide.hidden || g_sbSlide > 0.01)) ? g_sbBase : wr;
        i.onScreen = SbRectIntersectsWorkArea(checkRect.left, checkRect.top, checkRect.right, checkRect.bottom, m.work.left, m.work.top, m.work.right, m.work.bottom, 16) &&
                     (g_sb.autoHide || g_sbSlide < 0.99);
    }
    // Hosted Z-order check: valid if below g_sbHostAfter (SHELLDLL_DefView) and above any WorkerW wallpaper sibling.
    bool beneath = !i.hosted || g_sbHostAfter == HWND_TOP;
    if (i.hosted && g_sbHostAfter != HWND_TOP && host) {
        bool seenIcons = false, seenSb = false, behindWorker = false;
        for (HWND ch = GetWindow(host, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
            if (ch == g_sbHostAfter) seenIcons = true;
            else if (ch == g_sidebar) { seenSb = true; break; }
            else {
                wchar_t cls[32]{}; GetClassNameW(ch, cls, 32);
                if (wcscmp(cls, L"WorkerW") == 0 && SbHostUsable(ch)) { behindWorker = true; break; }
            }
        }
        beneath = (seenIcons && seenSb && !behindWorker);
    }
    i.beneathIcons = beneath;
    return i;
}
// Returns what it decided (the self-test calls this directly).
SbHealth SbHealthTick(bool force = false) {
    if (g_sidebarOn && !g_sbClosing && !g_sbDragging && !g_sbHide.hidden && g_sidebar && IsWindow(g_sidebar) && !IsWindowVisible(g_sidebar)) {
        SbLogDiagnostics(L"SbHealthTick(invisible)");
        ShowWindow(g_sidebar, SW_SHOWNOACTIVATE);
        SbApplyAll();
        LogOk(L"Desktop sidebar: restored visibility automatically.");
    }
    const SbHealthInput in = SbHealthFacts();
    const SbVerdict v = SbHealthDecide(in);
    if (v.what == SbHealth::Healthy) {
        if (!g_sbHealthLast.empty()) { LogOk(L"Desktop sidebar: back in order (" + g_sbHealthLast + L")."); g_sbHealthLast.clear(); }
        SbRetryHeal(g_sbHealthRetry, GetTickCount64());
        g_sbWaiting = 0; return v.what;
    }
    if (v.what == SbHealth::Waiting) { if (++g_sbWaiting == 5) LogW(std::wstring(L"Desktop sidebar: still waiting, ") + v.why + L"."); return v.what; }
    g_sbWaiting = 0;
    const std::wstring key = std::wstring(v.why);
    if (key != g_sbHealthLast) {   // one line per distinct problem, with the facts that tell hidden, detached and covered apart
        g_sbHealthLast = key;
        SbLogDiagnostics(v.why);
    }
    const ULONGLONG now = GetTickCount64();
    if (!force) {   // bounded: growing waits between repairs, with a 10 s cooldown re-arm so a live window never stays abandoned
        if (g_sbHealthRetry.gaveUp) SbRetryAllowCooldown(g_sbHealthRetry, now, 10000);
        if (!SbRetryMay(g_sbHealthRetry, now)) {
            if (g_sbHealthRetry.gaveUp && !g_sbHealthGaveUpLogged) { g_sbHealthGaveUpLogged = true; LogW(L"Desktop sidebar: put back several times; cooling down 10 s before retrying."); }
            else if (!g_sbHealthRetry.gaveUp) SbLifeArm(g_sbHealthRetry.nextAt - now);
            return v.what;
        }
        SbRetryAttempt(g_sbHealthRetry, now); g_sbHealthGaveUpLogged = false;
    }
    g_sbHealthTried = now; ++g_sbRecoveries;
    if (HWND pm = g_sbHostDeny ? nullptr : FindWindowW(L"Progman", nullptr)) SbLookForHost(pm);   // refresh the sibling it sits beneath
    SbApplyAll();
    return v.what;
}
SbSessionState g_sbSession;
// Windows reported a change that can move, detach or hide a hosted window (it never gets these broadcasts itself: it is a child).
void SbShellEventNote(SbShellEvent e, const wchar_t* what) {
    if (!g_sidebarOn || g_quitting) return;
    const ULONGLONG now = GetTickCount64();
    if (SbRetryKick(g_sbLife, now)) g_sbGaveUpLogged = false;
    SbRetryKick(g_sbHealthRetry, now);
    g_sbReplacePending = true;
    LogI(std::wstring(L"Desktop sidebar: ") + what + L"; checking its placement.");
    SbLifeArm(SbEventSettleMs(e));
}
// The timer / event path: re-place a live window once, or run the lifecycle for a missing one.
void SbLifeTimerFired() {
    if (g_sidebar && IsWindow(g_sidebar) && g_sbReplacePending && !g_sbClosing) { g_sbReplacePending = false; SbApplyAll(); }
    g_sbReplacePending = false;
    SbEnsure();
    SbHealthTick();
}

void SbRecoverAfterThemeChange() {
    if (!g_sidebarOn || g_quitting || g_sbClosing) return;
    WallForget();
    if (g_sidebar && IsWindow(g_sidebar)) {
        const SbHealthInput in = SbHealthFacts();
        const SbVerdict v = SbHealthDecide(in);
        if (v.what == SbHealth::Healthy) {
            SbRender();
            return;
        }
        SbLogDiagnostics(L"PostThemeCheck(unhealthy)");
    } else {
        SbLogDiagnostics(L"PostThemeCheck(missing)");
    }
    SbShellEventNote(SbShellEvent::ThemeChanged, L"theme/wallpaper changed");
    SbEnsure();
    SbHealthTick(true);
}

void SbSetOption(const std::wstring& key, int value) {
    if (!key.empty()) {
        if (key == L"sidebar.clickThrough" || key == L"sidebar.reserve") Cfg().SetBool(key, value != 0); else Cfg().SetInt(key, value);
    }
    Cfg().Save();
    g_sb = SbLoad();
    if (g_sidebar) SbApplyAll();
}
void SbSetSections(const std::vector<SbSecItem>& items) { Cfg().SetStr(L"sidebar.sections", SbFormatSections(items)); SbSetOption(L"", 0); }
void SbSetStr(const std::wstring& key, const std::wstring& value) { Cfg().SetStr(key, value); SbSetOption(L"", 0); }
// The current look: every profile key, taken from the settings, or from the Default look where a setting was never written.
std::wstring SbCurrentProfile() {
    auto kv = SbProfileDecode(SbBuiltinProfiles()[0].text);
    for (const SbProfileKey& k : SbProfileKeys()) {
        if (!Cfg().Has(k.key)) continue;
        const std::wstring v = k.text ? Cfg().Str(k.key, L"") : std::to_wstring(Cfg().Int(k.key, 0, k.lo, k.hi));
        bool had = false; for (auto& p : kv) if (p.first == k.key) { p.second = v; had = true; }
        if (!had) kv.push_back({k.key, v});
    }
    return SbProfileEncode(kv);
}
void SbApplyProfile(const std::wstring& text) {   // only known keys with valid values are ever written
    for (const auto& [key, val] : SbProfileDecode(text)) {
        bool isText = false; for (const SbProfileKey& k : SbProfileKeys()) if (key == k.key) isText = k.text;
        if (isText) Cfg().SetStr(key, val); else Cfg().SetInt(key, _wtoi(val.c_str()));
    }
    g_sbShown.clear();
    SbSetOption(L"", 0);
}
std::vector<std::pair<std::wstring, std::wstring>> SbMonitorChoices() {
    std::vector<std::pair<std::wstring, std::wstring>> v;
    const auto all = Monitors();
    for (size_t i = 0; i < all.size(); ++i) {
        const int w = all[i].rc.right - all[i].rc.left, h = all[i].rc.bottom - all[i].rc.top;
        v.emplace_back(all[i].device, L"Screen " + std::to_wstring(i + 1) + (all[i].primary ? L" (main)" : L"") + L"  " + std::to_wstring(w) + L"\u00D7" + std::to_wstring(h));
    }
    return v;
}

// ---------------------------------------------------------------- mini monitor (2.5)
// A small floating readout built on the Sidebar's engine: layered per-pixel alpha (PushLayered),
// per-monitor DPI, click-through or interactive, never activating. Values come from the shared 1 s
// snapshot (g_read / g_usage), graphs from g_store: the Mini never polls anything itself.
Ui g_miniUi;
RECT g_miniOpenBtn{};   // "Open" button hit-rect (interactive, expanded layout)
MiniCfg g_miniCfg;
bool g_miniHotkeyOk = false;
const int kMiniHotkey = 0xA5C2;   // Ctrl+Alt+Shift+M (the sidebar has 0xA5C1, Ctrl+Alt+Shift+S)

MiniCfg MiniLoad() {
    // earlier 2.5 builds stored only "mini.topmost": migrate it once to the layer setting
    if (!Cfg().Has(L"mini.layer") && Cfg().Has(L"mini.topmost")) { Cfg().SetInt(L"mini.layer", Cfg().Bool(L"mini.topmost", true) ? 0 : 1); Cfg().Save(); }
    MiniCfg c;
    c.layout = Cfg().Int(L"mini.layout", 1, 0, 1);
    c.metrics = static_cast<uint32_t>(Cfg().Int(L"mini.metrics", static_cast<int>(kMiniDefaultMetrics), 0, static_cast<int>(kMiniAllMetrics)));
    c.graphs = Cfg().Bool(L"mini.graphs", true);
    c.clickThrough = Cfg().Bool(L"mini.clickThrough", false);
    c.layer = Cfg().Int(L"mini.layer", 0, 0, 1);
    c.opacity = Cfg().Int(L"mini.opacity", 85, 10, 100);
    c.width = Cfg().Int(L"mini.width", 0, 0, 400); c.rowHeight = Cfg().Int(L"mini.rowHeight", 0, 0, 36); c.scale = Cfg().Int(L"mini.scale", 100, 80, 200);
    c.background = Cfg().Has(L"mini.background") ? Cfg().Int(L"mini.background", 0, 0, 2) : -1; c.tint = Cfg().Int(L"mini.tint", 70, 0, 100);
    c.accent = Cfg().Int(L"mini.accent", -1, -1, 0xFFFFFF); c.textColor = Cfg().Int(L"mini.textColor", -1, -1, 0xFFFFFF); c.glow = Cfg().Int(L"mini.glow", 0, 0, 2);
    c.styles = Cfg().Str(L"mini.styles", L"");
    return MiniSanitize(c);
}
const wchar_t* MiniGlyph(MiniMetric m) {
    switch (m) {
    case MCpuLoad: return L"\xE950"; case MCpuTemp: return L"\xE9CA"; case MGpuLoad: return L"\xF211"; case MGpuTemp: return L"\xE9CA";
    case MRam: return GlyphMemory(); case MFan: return L"\xE9CA"; case MNet: return L"\xE839"; default: return L"\xE121";
    }
}
const wchar_t* MiniLabel(MiniMetric m) {
    switch (m) {
    case MCpuLoad: return L"CPU"; case MCpuTemp: return L"CPU TEMP"; case MGpuLoad: return L"GPU"; case MGpuTemp: return L"GPU TEMP";
    case MRam: return L"RAM"; case MFan: return L"FAN"; case MNet: return L"NET"; default: return L"TIME";
    }
}
COLORREF MiniColor(MiniMetric m);
COLORREF MiniTone(MiniMetric m) { const COLORREF c = MiniColor(m); return c == col::accent ? SbRgb(g_miniCfg.accent, c) : c; }   // the user's accent, where the theme's would be
COLORREF MiniColor(MiniMetric m) {
    switch (m) {
    case MCpuTemp: case MGpuTemp: return col::warn; case MRam: return col::accentDim; case MFan: case MNet: return col::ok;
    case MClock: return col::sub; default: return col::accent;
    }
}
// The same wording as Desktop Monitoring: helper values are "stale" / "no data" when the helper's
// file is old or absent, "not exposed" when the hardware has no such sensor; Windows counters
// (CPU load, RAM, GPU fallback) are always live. A vendor fan value that has not been seen to change
// is "unverified" (it may be frozen), never shown as a live speed.
std::wstring MiniValue(MiniMetric m, bool& dim) {
    const Readings& r = g_read;
    std::wstring v;
    switch (m) {
    case MCpuLoad: v = NA(g_usage.cpu, L"%"); break;
    case MCpuTemp: v = Live(DispTemp(r.cpuTemp), true, TempSuffix()); break;
    case MGpuLoad: v = NA(g_usage.gpu, L"%"); break;
    case MGpuTemp: v = Live(DispTemp(r.gpuTemp), true, TempSuffix()); break;
    case MRam: v = NA(g_usage.ram, L"%"); break;
    case MFan: {
        if (!r.present) { v = Live(-1, true, L""); break; }
        double rpm = -1; bool live = false;
        for (const auto& f : r.fans) if (f.rpm >= 0) { rpm = std::max(rpm, f.rpm); live = live || f.live; }
        v = rpm < 0 ? L"not exposed" : !live ? L"unverified" : NA(rpm, L" rpm");
        break;
    }
    case MNet: {
        const NetSelection& n = g_netSel;
        if (!n.valid) v = L"no data"; else if (!n.connected) v = L"disconnected"; else if (n.down < 0) v = L"\u2014";
        else v = L"\u2193" + FormatBytes(static_cast<uint64_t>(n.down)) + L"  \u2191" + FormatBytes(static_cast<uint64_t>(n.up));
        break;
    }
    default: { SYSTEMTIME st; GetLocalTime(&st); wchar_t b[16]; swprintf(b, 16, L"%02u:%02u", st.wHour, st.wMinute); v = b; }
    }
    dim = v == L"stale" || v == L"no data" || v == L"not exposed" || v == L"N/A" || v == L"unverified" || v == L"disconnected";
    return v;
}
// Sparkline over the most recent 60 points of a history series; gaps (NaN) break the line.
void MiniSpark(Dc dc, const RECT& rc, const Ring& s, double lo, double hi, COLORREF color, int pen) {
    const size_t n = s.Size();
    if (n < 2 || hi <= lo) return;
    const size_t show = std::min<size_t>(n, 60), start = n - show;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    auto X = [&](size_t i) { return rc.right - 1 - static_cast<int>((show - 1 - i) * (w - 1) / 59.0); };   // newest at the right edge
    auto Y = [&](double v) { return rc.bottom - 1 - static_cast<int>((std::clamp(v, lo, hi) - lo) * (h - 2) / (hi - lo)); };
    (void)X; (void)Y; (void)start;
    AreaGraph(dc, RECT{rc.right - static_cast<int>((show - 1) * (w - 1) / 59.0) - 1, rc.top, rc.right, rc.bottom}, SeriesPoints(s, show, lo, hi), color, pen);
}
void MiniGraph(Dc dc, const Ui& u, RECT rc, MiniMetric m) {
    Round(dc, rc, u.S(8), col::panel, col::border);
    InflateRect(&rc, -u.S(3), -u.S(2));
    const int sid = MiniSeries(m);
    if (sid < 0) return;
    const Ring& s = g_store.Get(static_cast<Series>(sid));
    double lo = 0, hi = 100, mn = 0, mx = 0;
    const bool any = s.MinMax(mn, mx);
    if (m == MCpuTemp || m == MGpuTemp) { lo = 30; hi = 100; }
    else if (m == MFan) hi = std::max(1000.0, any ? mx * 1.1 : 0);
    else if (m == MNet) { double a = 0, b = 0; hi = std::max({1024.0, any ? mx : 0, g_store.Get(SNetUp).MinMax(a, b) ? b : 0}) * 1.1; }
    if (m == MNet) MiniSpark(dc, rc, g_store.Get(SNetUp), lo, hi, col::accentDim, 1);
    MiniSpark(dc, rc, s, lo, hi, MiniTone(m), 1);
}
void MiniRender() {
    if (!g_mini) return;
    g_miniSeq = g_snapSeq;
    LayerLook look; look.kind = std::max(0, g_miniCfg.background); look.tintPct = g_miniCfg.tint;
    PushLayered(g_mini, kSbBg, g_miniCfg.opacity, g_miniUi.S(8), [](Dc dc, int W, int H) {
        const Ui& u = g_miniUi;
        const Readings& r = g_read;
        const MiniLayout L = MiniLayoutFor(g_miniCfg);
        const bool ex = g_miniCfg.layout == 1;
        const COLORREF acc = SbRgb(g_miniCfg.accent, col::accent), accDeep = g_miniCfg.accent < 0 ? col::accentDeep : Mix(col::bg, acc, .40), txt = SbRgb(g_miniCfg.textColor, col::text);
        // frame: thin border, accent along the top
        Outline(dc, RECT{0, 0, W, H}, u.S(16), accDeep);
        Line(dc, u.S(8), 0, W - u.S(8), 0, acc, u.S(2));
        if (g_miniCfg.glow > 0) Halo(dc, W / 2, 0, W / 2, acc, .08f * g_miniCfg.glow);
        const int x = u.S(ex ? 12 : 9), right = W - x;
        // header: mark + name, data state dot (green live, amber stale, grey none)
        const COLORREF state = r.present ? col::ok : r.everSeen ? col::warn : col::faint;
        if (ex) {
            if (g_miniCfg.glow > 0) Halo(dc, x + u.S(10), u.S(19), u.S(18), acc, .20f * g_miniCfg.glow);
            DrawIconAt(dc, g_ankhBig, x, u.S(9), u.S(20));
            Text(dc, L"AnkhCore", RECT{x + u.S(26), u.S(8), right, u.S(30)}, u.f.btn, txt, DT_SINGLELINE | DT_VCENTER);
        } else {
            Text(dc, L"AnkhCore", RECT{x, u.S(4), right, u.S(22)}, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER);
        }
        const int dot = u.S(7), dy = (u.S(L.headerH) - dot) / 2 + (ex ? 0 : -u.S(1));
        Round(dc, RECT{right - dot, dy, right, dy + dot}, dot, state, state);
        Line(dc, x, u.S(L.headerH), right, u.S(L.headerH), accDeep, 1);
        // metric rows
        const int rowH = L.rowH;
        if (L.rows.empty())
            Text(dc, L"No metrics selected", RECT{x, u.S(L.headerH + 6), right, u.S(L.headerH + 6 + rowH)}, u.f.tiny, col::faint, DT_SINGLELINE | DT_VCENTER);
        const auto stMap = MiniParseStyles(g_miniCfg.styles);
        for (const MiniRow& row : L.rows) {
            const int top = u.S(row.top), bottom = u.S(row.top + rowH);
            bool dim = false;
            const std::wstring val = MiniValue(row.metric, dim);
            int mIdx = 0; while (mIdx < kMiniMetricCount && MiniMetricAt(mIdx) != row.metric) ++mIdx;
            const int style = mIdx < kMiniMetricCount ? stMap[mIdx] : MiniStyleValue;
            const COLORREF tone = dim ? col::faint : MiniTone(row.metric);
            Text(dc, MiniGlyph(row.metric), RECT{x, top, x + u.S(18), bottom}, u.f.icon, tone, DT_SINGLELINE | DT_VCENTER);
            Text(dc, MiniLabel(row.metric), RECT{x + u.S(22), top, x + u.S(ex ? 92 : 80), bottom}, u.f.tiny, col::sub, DT_SINGLELINE | DT_VCENTER);
            if (style == MiniStyleSpark && !row.graph && MiniSeries(row.metric) >= 0) {
                const RECT sp{x + u.S(ex ? 84 : 70), top + u.S(3), right - u.S(54), bottom - u.S(3)};
                if (sp.right - sp.left > u.S(20)) MiniGraph(dc, u, sp, row.metric);
            }
            Text(dc, val, RECT{x + u.S(ex ? 80 : 66), top, right, bottom}, ex ? u.f.small : u.f.tiny, dim ? col::faint : txt,
                 DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (style == MiniStyleBar && !dim) {
                double pct = -1;
                switch (row.metric) {
                case MCpuLoad: pct = g_usage.cpu; break;
                case MGpuLoad: pct = g_usage.gpu; break;
                case MRam: pct = g_usage.ram; break;
                case MCpuTemp: pct = r.present && r.cpuTemp >= 0 ? (r.cpuTemp - 30.0) * 100.0 / 70.0 : -1; break;
                case MGpuTemp: pct = r.present && r.gpuTemp >= 0 ? (r.gpuTemp - 30.0) * 100.0 / 70.0 : -1; break;
                case MFan: {
                    double rpm = -1; for (const auto& f : r.fans) if (f.rpm >= 0 && f.live && std::isfinite(f.rpm)) rpm = std::max(rpm, f.rpm);
                    if (r.present && rpm >= 0) pct = rpm / 50.0;
                    break;
                }
                default: break;
                }
                if (pct >= 0) {
                    const RECT tr{x + u.S(22), bottom - u.S(3), right, bottom};
                    Round(dc, tr, u.S(1), col::track, col::track);
                    RECT fill = tr; fill.right = tr.left + static_cast<int>((tr.right - tr.left) * std::clamp(pct, 0.0, 100.0) / 100.0);
                    if (fill.right > fill.left) BarFill(dc, fill, u.S(2), Mix(col::bg, tone, .45), tone);
                }
            }
            if (row.graph) MiniGraph(dc, u, RECT{x + u.S(22), bottom, right, u.S(row.top + row.height)}, row.metric);
        }
        // footer (expanded): Open button when interactive, the way back when click-through
        g_miniOpenBtn = RECT{};
        if (ex) {
            const RECT fr{x, u.S(L.footerTop), right, u.S(L.footerTop + L.footerH)};
            if (!g_miniCfg.clickThrough) {
                g_miniOpenBtn = fr;
                Round(dc, fr, u.S(7), acc, acc);
                Text(dc, L"\xE8A7", RECT{fr.left + u.S(10), fr.top, fr.left + u.S(30), fr.bottom}, u.f.icon, col::bg, DT_SINGLELINE | DT_VCENTER);
                Text(dc, L"Open AnkhCore", fr, u.f.btn, col::bg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                Text(dc, L"Click-through  \x00B7  Ctrl+Alt+Shift+M", fr, u.f.tiny, col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }, g_miniCfg.background < 0 ? nullptr : &look);   // no choice made yet: the shared "wallpaper glass" switch decides, as before
}
void MiniApplyStyle() {
    if (!g_mini) return;
    LONG_PTR ex = GetWindowLongPtrW(g_mini, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    ex = g_miniCfg.clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(g_mini, GWL_EXSTYLE, ex);
}
// Size and position for the monitor the widget is on. The saved spot (mini.x/y, physical pixels) is
// only read here, never rewritten: if its monitor is gone the widget shows on the primary screen,
// and returns to the saved spot when that monitor is back. dpiOverride/at: from WM_DPICHANGED.
void MiniPlace(UINT dpiOverride = 0, const POINT* at = nullptr) {
    if (!g_mini) return;
    PerMonitorDpiScope pm;   // work areas and position in physical pixels
    const auto mons = Monitors();
    std::vector<MiniRect> works;
    for (const auto& m : mons) works.push_back({m.work.left, m.work.top, m.work.right, m.work.bottom});
    const int sx = Cfg().Int(L"mini.x", INT_MIN, -100000, 100000), sy = Cfg().Int(L"mini.y", INT_MIN, -100000, 100000);
    const bool have = at || (sx != INT_MIN && sy != INT_MIN);
    const POINT want = at ? *at : POINT{sx, sy};
    const MiniLayout L = MiniLayoutFor(g_miniCfg);
    auto useDpi = [](int monitorDpi) {
        const int dpi = MulDiv(monitorDpi, g_miniCfg.scale, 100);   // the whole widget scales with its size setting
        if (g_miniUi.f.title && dpi == g_miniUi.dpi) return;
        for (HFONT f : {g_miniUi.f.title, g_miniUi.f.h1, g_miniUi.f.h2, g_miniUi.f.text, g_miniUi.f.small, g_miniUi.f.tiny, g_miniUi.f.btn, g_miniUi.f.icon, g_miniUi.f.iconBig}) if (f) DeleteObject(f);
        g_miniUi.dpi = dpi; g_miniUi.f = MakeFonts(dpi);
    };
    // pass 1 with the DPI of the monitor under the wanted spot; pass 2 with the DPI where it landed
    HMONITOR hm = have ? MonitorFromPoint(want, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    useDpi(dpiOverride ? static_cast<int>(dpiOverride) : static_cast<int>(MonitorDpi(hm)));
    MiniRect p = MiniPosition(works, have, want.x, want.y, g_miniUi.S(L.width), g_miniUi.S(L.height), g_miniUi.S(20));
    if (!dpiOverride) {
        const RECT pr{p.l, p.t, p.r, p.b};
        useDpi(static_cast<int>(MonitorDpi(MonitorFromRect(&pr, MONITOR_DEFAULTTOPRIMARY))));
        p = MiniPosition(works, true, p.l, p.t, g_miniUi.S(L.width), g_miniUi.S(L.height), g_miniUi.S(20));
    }
    SetWindowPos(g_mini, g_miniCfg.layer == 0 ? HWND_TOPMOST : HWND_NOTOPMOST, p.l, p.t, p.r - p.l, p.b - p.t, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void MiniApplyAll(UINT dpiOverride = 0, const POINT* at = nullptr) { if (!g_mini) return; MiniApplyStyle(); MiniPlace(dpiOverride, at); MiniRender(); }
void NetSetMode(int mode, const std::wstring& guid) {
    Cfg().SetInt(L"sidebar.netMode", mode);
    if (mode == NetManual && !guid.empty()) Cfg().SetStr(L"sidebar.netAdapter", guid);   // the manual choice is kept while another mode is active
    Cfg().Save();
    g_engine.Net(mode, Cfg().Str(L"sidebar.netAdapter", L""));   // the history series follows the same selection
    NetApply();
    if (g_sidebar) SbRender();
    if (g_mini) MiniRender();
}
void NetSetPeriod(int period) { Cfg().SetInt(L"sidebar.netPeriod", period); Cfg().Save(); if (g_sidebar) SbRender(); }
void MiniSetOption(const std::wstring& key, int value) {
    if (!key.empty()) {
        if (key == L"mini.clickThrough" || key == L"mini.graphs") Cfg().SetBool(key, value != 0); else Cfg().SetInt(key, value);
    }
    Cfg().Save();
    g_miniCfg = MiniLoad();
    if (g_mini) MiniApplyAll();
}
void MiniApplyProfile(const std::wstring& text) {
    MiniCfg p;
    if (!MiniParseProfile(text, p)) return;
    Cfg().SetInt(L"mini.layout", p.layout);
    Cfg().SetInt(L"mini.metrics", static_cast<int>(p.metrics));
    Cfg().SetBool(L"mini.graphs", p.graphs);
    Cfg().SetInt(L"mini.opacity", p.opacity);
    Cfg().SetInt(L"mini.background", p.background);
    Cfg().SetInt(L"mini.tint", p.tint);
    Cfg().SetInt(L"mini.glow", p.glow);
    Cfg().SetStr(L"mini.styles", MiniFormatStyles(MiniParseStyles(p.styles)));
    MiniSetOption(L"", 0);
}
// Moves the Mini to a corner of the screen it is on, and remembers that spot like a drop.
void MiniToCorner(int corner) {
    if (!g_mini) return;
    {
        PerMonitorDpiScope pm;
        RECT wr; GetWindowRect(g_mini, &wr);
        MONITORINFO mi{sizeof(mi)}; if (!GetMonitorInfoW(MonitorFromWindow(g_mini, MONITOR_DEFAULTTONEAREST), &mi)) return;
        const UINT dpi = MonitorDpi(MonitorFromWindow(g_mini, MONITOR_DEFAULTTONEAREST));
        const MiniRect to = MiniCorner({mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom}, corner, wr.right - wr.left, wr.bottom - wr.top, MulDiv(20, static_cast<int>(dpi), 96));
        Cfg().SetInt(L"mini.x", to.l); Cfg().SetInt(L"mini.y", to.t);
    }
    Cfg().Save();
    MiniApplyAll();
}
void MiniSetClickThrough(bool on) {
    MiniSetOption(L"mini.clickThrough", on ? 1 : 0);
    LogI(std::wstring(L"Mini Monitor: ") + (on ? L"click-through (mouse passes to the desktop)" : L"interactive"));
}
void MiniSavePos() {
    PerMonitorDpiScope pm;
    RECT r; GetWindowRect(g_mini, &r);
    Cfg().SetInt(L"mini.x", r.left); Cfg().SetInt(L"mini.y", r.top); Cfg().Save();
}
void ShowMainWindow() { ShowWindow(g_wnd, SW_SHOW); ShowWindow(g_wnd, SW_RESTORE); SetForegroundWindow(g_wnd); }
LRESULT CALLBACK MiniProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;   // never steal focus
    case WM_NCHITTEST: return g_miniCfg.clickThrough ? HTTRANSPARENT : HTCLIENT;
    case WM_LBUTTONDOWN: {   // interactive only (click-through gets no mouse input)
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (PtInRect(&g_miniOpenBtn, pt)) { ShowMainWindow(); return 0; }
        ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);   // drag (modal move loop, no activation)
        return 0;
    }
    case WM_LBUTTONDBLCLK: ShowMainWindow(); return 0;
    case WM_RBUTTONUP: MiniSetClickThrough(true); return 0;   // right-click: hand the mouse back to the desktop
    case WM_EXITSIZEMOVE: MiniSavePos(); return 0;             // remember where the user dropped it
    case WM_DPICHANGED: {   // moved to a monitor with another scale: re-layout at the new DPI where Windows suggests
        const RECT* s = reinterpret_cast<const RECT*>(l);
        const POINT at{s->left, s->top};
        MiniApplyAll(LOWORD(w), &at);
        return 0;
    }
    case WM_DISPLAYCHANGE: MiniApplyAll(); return 0;
    case WM_SETTINGCHANGE: if (w == SPI_SETWORKAREA) MiniApplyAll(); return 0;
    case WM_HOTKEY: if (w == kMiniHotkey) MiniSetClickThrough(!g_miniCfg.clickThrough); return 0;
    case WM_DESTROY: UnregisterHotKey(h, kMiniHotkey); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void ToggleMini(bool on, bool persist) {
    if (on == g_miniOn) return;
    g_miniOn = on;
    if (persist) { Cfg().SetBool(L"mini.enabled", on); Cfg().Save(); }
    if (!on) { if (g_mini) { DestroyWindow(g_mini); g_mini = nullptr; } return; }
    g_engine.Monitor(true);   // the mini monitor needs live readings (no-op when monitoring already runs)
    g_miniCfg = MiniLoad();
    static bool reg = false;
    static const std::wstring cls = Tagged(L"AnkhCoreMini");
    if (!reg) { WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = MiniProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = cls.c_str(); wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.style = CS_DBLCLKS; RegisterClassExW(&wc); reg = true; }
    {
        PerMonitorDpiScope pm;   // per-monitor DPI aware on its own, like the sidebar
        g_mini = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.c_str(), L"AnkhCore Mini",
                                 WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (!g_mini) { g_miniOn = false; LogE(L"The Mini Monitor could not be created."); return; }
    g_miniHotkeyOk = RegisterHotKey(g_mini, kMiniHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'M') != 0;
    if (!g_miniHotkeyOk) LogW(L"Mini Monitor: Ctrl+Alt+Shift+M is taken by another program; switch click-through from the tray menu.");
    MiniApplyAll();
}

// ---------------------------------------------------------------- test build: scripted UI self-test
// --selftest (test build only): walks every page through real sidebar clicks, captures each page,
// scrolls each page and clicks every content/footer control with confirmations auto-accepted (so
// every guard is exercised through the real UI path), opens the desktop widgets, probes the
// low-level guards, writes DataDir()\selftest\report.txt and exits. Never touches the system.
#ifdef ANKH_TEST_BUILD
std::vector<std::wstring> g_stReport;
int g_stFail = 0;
// The walk clicks every control, and toggles persist to settings.json. Without isolation one run
// leaks into the next (e.g. "monitoring.autoStart" flipped off -> the next run starts no monitoring
// and correctly sees no live readings). The file is snapshotted when the self-test starts and put
// back at exit, so every run starts from the same settings.
std::string g_stSettings;
bool g_stHaveSettings = false;
void StRestoreSettings() {
    if (!g_stHaveSettings) return;
    WriteAtomic(Cfg().Path(), reinterpret_cast<const uint8_t*>(g_stSettings.data()), g_stSettings.size());
}
void StCheck(bool ok, const std::wstring& what) { g_stReport.push_back((ok ? L"PASS  " : L"FAIL  ") + what); if (!ok) ++g_stFail; }
// B15: an on-screen probe is only meaningful while AnkhCore is the foreground window. When another program has it, the probe is reported as
// SKIP (never as PASS, never as FAIL) and the result line says how many were skipped, so a busy desktop no longer looks like a product failure.
int g_stSkip = 0;
void StSkip(const std::wstring& what) { g_stReport.push_back(L"SKIP  " + what); ++g_stSkip; }
void StProbe(bool foregroundOk, bool ok, const std::wstring& what) { if (foregroundOk) StCheck(ok, what); else StSkip(what + L" [AnkhCore was not the foreground window: this probe needs a quiet desktop]"); }
void StPump(DWORD ms) {
    const DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < ms) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(15);
    }
}
bool StSavePng(HWND w, const std::wstring& path) {
    RECT r; GetWindowRect(w, &r);
    const int cw = r.right - r.left, ch = r.bottom - r.top;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, cw, ch);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool printed = PrintWindow(w, mdc, 2 /*PW_RENDERFULLCONTENT*/) != 0;
    SelectObject(mdc, old); DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    bool ok = false;
    IWICImagingFactory* f = nullptr; IWICBitmap* wb = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    if (printed && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, &wb)) && SUCCEEDED(f->CreateStream(&st)) &&
        SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(wb, nullptr)) &&
        SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) ok = true;
    if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wb) wb->Release(); if (f) f->Release();
    DeleteObject(bmp);
    return ok;
}
int g_stBack = 0;   // what the probe's backing window shows: 0 white, 1 black, 2 stripes
LRESULT CALLBACK StBackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC d = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        FillRect2(d, r, g_stBack == 1 ? RGB(0, 0, 0) : RGB(255, 255, 255));
        if (g_stBack == 3) FillRect2(d, RECT{r.right * 3 / 5, 0, r.right, r.bottom}, RGB(0, 0, 0));   // one hard edge, under the bare glass
        if (g_stBack == 2) for (int x = 0; x < r.right; x += 48) FillRect2(d, RECT{x, 0, x + 24, r.bottom}, RGB(0, 0, 0));
        EndPaint(h, &ps); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
struct ScreenStat { double lum = 0, detail = 0; };
double StLin(double c) { c /= 255; return c <= .04045 ? c / 12.92 : pow((c + .055) / 1.055, 2.4); }
double StLum(COLORREF c) { return .2126 * StLin(GetRValue(c)) + .7152 * StLin(GetGValue(c)) + .0722 * StLin(GetBValue(c)); }
double StContrast(double a, double b) { return (std::max(a, b) + .05) / (std::min(a, b) + .05); }
ScreenStat StMeasure(const RECT& sr, const std::wstring& png) {   // real composed pixels from the screen, not the window's own surface
    ScreenStat out; const int w = sr.right - sr.left, h = sr.bottom - sr.top;
    HDC s = GetDC(nullptr), m = CreateCompatibleDC(s);
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}; void* bits = nullptr;
    HBITMAP b = CreateDIBSection(s, &bi, DIB_RGB_COLORS, &bits, nullptr, 0); HGDIOBJ o = SelectObject(m, b);
    BitBlt(m, 0, 0, w, h, s, sr.left, sr.top, SRCCOPY | CAPTUREBLT); GdiFlush();
    const auto* p = static_cast<const uint32_t*>(bits); double sumL = 0, sumD = 0, prev = 0; size_t nD = 0;
    for (int y = 0; y < h; y += 2) for (int x = 0; x < w; ++x) {
        const uint32_t v = p[static_cast<size_t>(y) * w + x];
        const double g = .2126 * ((v >> 16) & 255) + .7152 * ((v >> 8) & 255) + .0722 * (v & 255);
        sumL += .2126 * StLin((v >> 16) & 255) + .7152 * StLin((v >> 8) & 255) + .0722 * StLin(v & 255);
        if (x) { sumD += std::abs(g - prev); ++nD; } prev = g;
    }
    out.lum = sumL / std::max<size_t>(1, static_cast<size_t>(w) * ((h + 1) / 2)); out.detail = sumD / std::max<size_t>(1, nD);
    SelectObject(m, o);
    if (!png.empty()) {
        IWICImagingFactory* f = nullptr; IWICBitmap* wb = nullptr; IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateBitmapFromHBITMAP(b, nullptr, WICBitmapIgnoreAlpha, &wb)) && SUCCEEDED(f->CreateStream(&st)) &&
            SUCCEEDED(st->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(wb, nullptr))) { fr->Commit(); enc->Commit(); }
        if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); if (wb) wb->Release(); if (f) f->Release();
    }
    DeleteObject(b); DeleteDC(m); ReleaseDC(nullptr, s);
    return out;
}
void StRepaint(HWND h);
bool g_stHoverOverride = false;
void StGlassProbe(HWND h, const std::wstring& dir) {
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = StBackProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"AnkhCoreProbeBack.Test"; RegisterClassExW(&wc);
    RECT wr; GetWindowRect(h, &wr);
    HWND back = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, wr.left - 60, wr.top - 60, wr.right - wr.left + 120, wr.bottom - wr.top + 120, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(back, SW_SHOWNOACTIVATE);
    auto activate = [&] {
        if (IsIconic(h) || !IsWindowVisible(h)) { ShowWindow(h, SW_RESTORE); ShowWindow(h, SW_SHOW); StPump(300); }   // someone minimized it (e.g. Show desktop)
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // nothing may cover the measured area
        for (int i = 0; i < 12 && GetForegroundWindow() != h; ++i) {
            // No injected key presses here (B13): they reach whatever the person at the PC is typing into.
            const DWORD fgT = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
            if (fgT && fgT != me) AttachThreadInput(me, fgT, TRUE);
            BringWindowToTop(h); SetForegroundWindow(h); SetActiveWindow(h);
            if (fgT && fgT != me) AttachThreadInput(me, fgT, FALSE);
            StPump(200);
        }
        SetWindowPos(back, h, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return GetForegroundWindow() == h;
    };
    g_gov.level = 0;   // the page walk can legitimately degrade effects under load; the probe measures them at full strength
    Cfg().SetInt(L"appearance.motion", MotionFull); Cfg().SetInt(L"appearance.glow", 2); UpdateMotion();   // the control walk above leaves these on its last click
    g_probeBlank = true;
    const RECT cr = ContentRect();
    POINT o{0, 0}; ClientToScreen(h, &o);
    const RECT cardR{o.x + cr.left + g_ui.S(52), o.y + cr.top + g_ui.S(80), o.x + cr.left + g_ui.S(372), o.y + cr.top + g_ui.S(290)};
    const RECT bareR{o.x + cr.left + g_ui.S(440), o.y + cr.top + g_ui.S(80), o.x + cr.right - g_ui.S(40), o.y + cr.top + g_ui.S(290)};
    struct Shot { ScreenStat card, bare; bool fg; };
    auto shot = [&](int mat, int tint, int bg, const std::wstring& png) {
        Cfg().SetInt(L"appearance.material", mat); Cfg().SetInt(L"appearance.tint", tint); ApplyMaterial(h);
        g_stBack = bg; RedrawWindow(back, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        const bool fg = activate(); StRepaint(h); StPump(1100);
        Shot s; s.fg = fg && GetForegroundWindow() == h; s.card = StMeasure(cardR, L""); s.bare = StMeasure(bareR, png);
        s.fg = s.fg && GetForegroundWindow() == h;   // still active after the capture
        return s;
    };
    (void)shot(MatAcrylic, 68, 0, L""); StPump(1500);   // DWM's backdrop settles after the window behind it first appears
    const Shot acW = shot(MatAcrylic, 68, 0, L""), acK = shot(MatAcrylic, 68, 1, L""), acS = shot(MatAcrylic, 68, 2, dir + L"\\probe_acrylic_stripes_active.png");
    { RECT all = wr; StMeasure(all, dir + L"\\probe_acrylic_window_active.png"); }
    {   // a hard black/white edge behind the glass must come through as a soft gradient
        const Shot e = shot(MatAcrylic, 68, 3, L""); (void)e; StMeasure(wr, dir + L"\\probe_acrylic_edge_active.png");
        const int mid = (wr.left + wr.right) + 0;
        (void)mid;
        wchar_t eb[160]; const RECT backR{wr.left - 60, wr.top - 60, wr.right + 60, wr.bottom + 60};
        const int edge = backR.left + (backR.right - backR.left) * 3 / 5, o2 = g_ui.S(90);
        const RECT lr{edge - o2 * 2, bareR.top, edge - o2, bareR.bottom}, rr{edge + o2, bareR.top, edge + o2 * 2, bareR.bottom};
        const ScreenStat ls = StMeasure(lr, L""), rs = StMeasure(rr, L"");
        swprintf(eb, 160, L"blur probe (active): a hard edge behind the glass shows through (white side %.3f, black side %.3f)", ls.lum, rs.lum);
        StProbe(GetForegroundWindow() == h, ls.lum > rs.lum + .008, eb);
    }
    const Shot soW = shot(MatSolid, 68, 0, L""), soK = shot(MatSolid, 68, 1, L""), soS = shot(MatSolid, 68, 2, dir + L"\\probe_solid_stripes_active.png");
    { g_stBack = 2; RedrawWindow(back, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); }
    const bool allFg = acW.fg && acK.fg && acS.fg;
    if (!allFg) { wchar_t ft[120] = L""; GetWindowTextW(GetForegroundWindow(), ft, 120); g_stReport.push_back(std::wstring(L"INFO  foreground window during the probe: ") + ft + L" (the probe needs a quiet desktop)"); }
    StProbe(allFg, true, L"blur probe: AnkhCore really was the foreground window for every acrylic capture");
    const double raw = 255.0 * 2 / 48;   // mean step of the bare stripes
    wchar_t b[220];
    swprintf(b, 220, L"blur probe (active): desktop shows through acrylic (white %.3f vs black %.3f linear luminance; solid %.3f vs %.3f)", acW.bare.lum, acK.bare.lum, soW.bare.lum, soK.bare.lum);
    StProbe(allFg, acW.bare.lum - acK.bare.lum > .012 && std::abs(soW.bare.lum - soK.bare.lum) < .005, b);
    swprintf(b, 220, L"blur probe (active): stripes behind acrylic are blurred, detail %.2f (raw %.2f; unblurred through 68%% tint would be about %.2f; solid %.2f)", acS.bare.detail, raw, raw * .32, soS.bare.detail);
    StProbe(allFg, acS.bare.detail < raw * .32 * .35, b);
    // contrast of text on real composed pixels, worst cases over white and black desktops
    struct Case { const wchar_t* n; int mat, tint; } cases[] = {{L"acrylic 68", MatAcrylic, 68}, {L"acrylic 45", MatAcrylic, 45}, {L"acrylic 92", MatAcrylic, 92}, {L"frosted 68", MatFrosted, 68}, {L"wallpaper 68", MatWallpaper, 68}, {L"solid", MatSolid, 68}};
    for (const auto& k : cases) for (int bg : {0, 1}) {
        const Shot t = shot(k.mat, k.tint, bg, L"");
        const double cText = StContrast(StLum(col::text), t.card.lum), cSub = StContrast(StLum(col::sub), t.card.lum), cBare = StContrast(StLum(col::text), t.bare.lum), cBareSub = StContrast(StLum(col::sub), t.bare.lum);
        swprintf(b, 220, L"contrast %s over %s: card text %.1f:1, card secondary %.1f:1, bare text %.1f:1, bare secondary %.1f:1 (minimum 4.5:1)", k.n, bg ? L"black" : L"white", cText, cSub, cBare, cBareSub);
        StProbe(t.fg, cText >= 4.5 && cSub >= 4.5 && cBare >= 4.5 && cBareSub >= 4.5, b);
    }
    // isolated system-state simulation: only this process's inputs change, never the PC's settings
    struct Sim { const wchar_t* n; DWORD build; bool trans, hc, saver, bat; Material want; } sims[] = {
        {L"Windows 10 22H2 (build 19045)", 19045, true, false, false, false, MatWallpaper}, {L"Windows 11 21H2 (build 22000)", 22000, true, false, false, false, MatWallpaper},
        {L"Transparency effects off", 26300, false, false, false, false, MatSolid}, {L"High Contrast", 26300, true, true, false, false, MatSolid},
        {L"Battery saver", 26300, true, false, true, false, MatSolid}, {L"On battery", 26300, true, false, false, true, MatFrosted}};
    for (const auto& k : sims) {
        g_stMat.on = true; g_stMat.v = MaterialInput{}; g_stMat.v.build = k.build; g_stMat.v.transparencyOn = k.trans; g_stMat.v.highContrast = k.hc; g_stMat.v.batterySaver = k.saver; g_stMat.v.onBattery = k.bat;
        const Shot t = shot(MatAcrylic, 68, 0, L"");
        int bd = -1; DwmGetWindowAttribute(h, 38, &bd, sizeof(bd));
        const double c1 = StContrast(StLum(col::text), t.card.lum), c2 = StContrast(StLum(col::sub), t.bare.lum);
        wchar_t sb[220]; swprintf(sb, 220, L"simulated %s: material %d, backdrop %d, contrast card %.1f:1, bare secondary %.1f:1", k.n, static_cast<int>(g_material), bd, c1, c2);
        StCheck(g_material == k.want && bd == BackdropType(k.want) && c1 >= 4.5 && c2 >= 4.5, sb);
        if (k.build == 19045) {   // the Windows 10 font path, drawn for real
            LegacyFontsForced() = true; g_ui.f = MakeFonts(g_ui.dpi); g_probeBlank = false; StRepaint(h); StPump(300);
            StCheck(StSavePng(h, dir + L"\\win10_simulated.png"), L"simulated Windows 10 page captured with Segoe UI and Segoe MDL2 Assets");
            UINT32 miss = 0; std::vector<UINT32> cps; for (const wchar_t* g : kPageIcon) cps.push_back(g[0]); for (UINT32 x : {0xE921u, 0xE922u, 0xE923u, 0xE8BBu, 0xE76Bu, 0xE76Cu, 0xE70Du, 0xE7BAu, 0xE897u, 0xE950u, 0xF211u, 0xE9CAu, 0xEDA2u, 0xE839u, 0xE74Du, 0xE90Fu, 0xE790u, 0xEA86u, 0xE7F4u, 0xE74Cu, 0xE7E8u}) cps.push_back(x);
            std::wstring missing; for (UINT32 cp : cps) { UINT32 one = 0; if (!g_surf.GlyphsPresent(L"Segoe MDL2 Assets", {cp}, one)) { wchar_t hx[12]; swprintf(hx, 12, L" U+%04X", cp); missing += hx; } } (void)miss;
            StCheck(missing.empty(), L"Windows 10 icon font has every UI icon" + (missing.empty() ? std::wstring() : L" (missing:" + missing + L")"));
            LegacyFontsForced() = false; g_ui.f = MakeFonts(g_ui.dpi); g_probeBlank = true;
        }
    }
    g_stMat.on = false;
    {   // the live dot's pulse runs in the compositor: sample its pixels over time, with no app frames
        Cfg().SetInt(L"appearance.material", MatSolid); ApplyMaterial(h); activate(); StRepaint(h); StPump(400);
        POINT dc0{g_ui.S(28), ClientH() - g_ui.S(33)}; ClientToScreen(h, &dc0);
        const RECT dr{dc0.x + g_ui.S(5), dc0.y - g_ui.S(5), dc0.x + g_ui.S(13), dc0.y + g_ui.S(5)};
        double lo = 9, hi = -1; const uint64_t p0 = g_paints;
        for (int i = 0; i < 16; ++i) { const ScreenStat q = StMeasure(dr, L""); lo = std::min(lo, q.lum); hi = std::max(hi, q.lum); StPump(130); }
        wchar_t pb[120]; swprintf(pb, 120, L"neon pulse animates on screen (luminance %.4f to %.4f) with %d app paints", lo, hi, static_cast<int>(g_paints - p0));
        {   // when this fails, say whether it was the pulse or the foreground that failed, and who had the foreground
            const HWND fg = GetForegroundWindow(); wchar_t fc[64] = L"", ft[64] = L""; if (fg && fg != h) { GetClassNameW(fg, fc, 64); GetWindowTextW(fg, ft, 64); }
            StProbe(fg == h, hi - lo > .0015, std::wstring(pb) + (fg == h ? std::wstring() : std::wstring(L" - foreground window: ") + fc + L" '" + ft + L"'"));
        }
    }
    Cfg().SetInt(L"appearance.material", MatAcrylic); Cfg().SetInt(L"appearance.tint", 68); ApplyMaterial(h);
    g_probeBlank = false;
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DestroyWindow(back); StPump(200);
}
void StRepaint(HWND h) { RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW); }
void RunSelftest(HWND h) {
    const std::wstring dir = DataDir() + L"\\selftest";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);   // inside the test data folder
    g_stReport.push_back(L"AnkhCore " + std::wstring(kVersion) + L" test build self-test  " + NowText(true));
    g_stReport.push_back(L"Data folder: " + DataDir());
    g_stSettings = ReadText(Cfg().Path()); g_stHaveSettings = !g_stSettings.empty();
    StCheck(g_stHaveSettings, L"settings snapshot taken (restored at exit, so runs do not leak into each other)");
    StCheck(!IsElevated(), L"runs without elevation");
    StCheck(Hub().HelperRunning(), L"installed AnkhSensors helper is running (read-only source of live readings)");
    for (int i = 0; i < 40 && !g_engine.HubRun(); ++i) StPump(250);   // the engine is a separate process: allow for its start
    StCheck(g_engine.HubRun(), L"monitoring started at launch (monitoring.autoStart is on in the test settings)");
    StPump(2500);   // first refresh + sensor poll
    StCheck(g_read.present, L"live readings visible (fresh readings.txt from the installed helper)");
    if (GetEnvironmentVariableW(L"ANKH_PROBEONLY", nullptr, 0)) {   // diagnostics: only the glass probe
        StPump(1500); StGlassProbe(h, dir);
        std::wstring all; for (const auto& l : g_stReport) all += l + L"\r\n";
        const std::string u = WToUtf8(all); WriteAll(dir + L"\\probe_report.txt", reinterpret_cast<const uint8_t*>(u.data()), u.size());
        StRestoreSettings(); g_quitting = true; DestroyWindow(h); return;
    }
    const RECT cr = ContentRect();
    const int viewH = cr.bottom - cr.top;
    for (int i = 0; i < PageCount; ++i) {
        // navigate with a real click on the sidebar item (hits come from the last paint)
        StRepaint(h);
        auto rowFor = [&](int page, bool child, POINT& pt) {
            for (const NavRow& r : NavLayout())
                if (r.page == page && r.child == child) { pt = POINT{(r.r.left + r.r.right) / 2, (r.r.top + r.r.bottom) / 2}; return true; }
            return false;
        };
        std::wstring navDbg;   // FC-32: what the rail click did, shown only when the navigation check fails
        auto railFacts = [&] { return L"rail scroll " + std::to_wstring(g_navScroll) + L", open section " + std::to_wstring(g_navOpen) + L", rail view " + std::to_wstring(NavTop()) + L".." + std::to_wstring(NavTop() + NavViewH()) + L", " + std::to_wstring(NavLayout().size()) + L" rows"; };
        if (NavParent(i) >= 0 || NavIsTopBarPage(i)) NavGo(i);   // a sub-page and the top-bar Settings page have no rail entry; reaching it by a click from its parent is checked further down
        else {
        const int sec = NavSectionOf(i);
        POINT p{-1, -1};
        if (kNav[sec].pages.size() > 1 && !rowFor(i, true, p)) {   // open the section with a real click first
            NavReveal(kNav[sec].pages[0]); StRepaint(h);
            POINT hp{-1, -1}; const bool hf = rowFor(kNav[sec].pages[0], false, hp);
            const int hitsBefore = static_cast<int>(g_ui.hits.size());
            SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(hp.x, hp.y));
            StPump(500); StRepaint(h);
            navDbg += L"header row " + std::wstring(hf ? L"found" : L"NOT found") + L" at (" + std::to_wstring(hp.x) + L"," + std::to_wstring(hp.y) + L"), " + std::to_wstring(hitsBefore) + L" hits; after the click: page '" + kPageName[g_page] + L"', " + railFacts() + L"; ";
        }
        StRepaint(h); NavReveal(i); StRepaint(h);   // a long rail (several open children) is scrolled to the row first, as a user would with the wheel
        const bool childFound = rowFor(i, kNav[sec].pages.size() > 1, p);
        bool headerFound = false; if (!childFound) headerFound = rowFor(i, false, p);
        navDbg += std::wstring(L"row for the page: ") + (childFound ? L"child row" : headerFound ? L"header row" : L"NONE (clicking (" + std::to_wstring(p.x) + L"," + std::to_wstring(p.y) + L"))") + L" at (" + std::to_wstring(p.x) + L"," + std::to_wstring(p.y) + L"), " + railFacts();
        SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        }
        StPump((i == PApps || NavParent(i) == PApps) ? 300 : 600);
        if (i == PApps || NavParent(i) == PApps) for (int t = 0; t < 120 && !g_appsLoaded; ++t) StPump(250);
        StRepaint(h); StPump(200);
        StCheck(g_page == i, std::wstring(L"navigate to page '") + kPageName[i] + L"'" + (g_page == i || navDbg.empty() ? std::wstring() : L" [stayed on '" + std::wstring(kPageName[g_page]) + L"'; " + navDbg + L"]"));
        StCheck(StSavePng(h, dir + L"\\page_" + (i < 10 ? L"0" : L"") + std::to_wstring(i) + L".png"), std::wstring(L"capture page '") + kPageName[i] + L"'");
        // scroll the page screen by screen; click every content and footer control on each screen
        const int refusedBefore = TestRefusedCount();
        int clicked = 0, screens = 0;
        g_scroll[i] = 0;
        for (;;) {
            StRepaint(h);
            // bottom-up, so list rows (e.g. Uninstall) are clicked before a "Refresh" that rebuilds the list
            std::vector<RECT> rects; for (auto it = g_ui.hits.rbegin(); it != g_ui.hits.rend(); ++it) rects.push_back(it->r);
            for (const RECT& r : rects) {
                if (r.right <= SideW() || r.bottom <= g_ui.S(kHeader)) continue;   // skip navigation, close, minimise
                const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                if (c.y < cr.top || c.y >= ClientH() || c.x >= ClientW()) continue;     // off-screen rows: reached by scrolling
                SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(c.x, c.y));
                ++clicked;
                StPump(30);
                if (g_page != i) g_page = i;   // stay on the page under test
            }
            ++screens;
            const int maxS = std::max(0, g_contentH[i] - viewH);
            if (g_scroll[i] >= maxS || screens > 60) break;
            g_scroll[i] = std::min(maxS, g_scroll[i] + viewH * 3 / 4);
        }
        StCheck(!g_busy, std::wstring(L"page '") + kPageName[i] + L"': " + std::to_wstring(clicked) + L" control clicks on " + std::to_wstring(screens) +
                         L" screen(s), " + std::to_wstring(TestRefusedCount() - refusedBefore) + L" system change(s) refused, no operation started");
        g_scroll[i] = 0;
    }
    // accordion keyboard navigation and persistence
    NavGo(PDashboard); StPump(300);
    SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0); StPump(400);
    StCheck(g_page == PMonitor && g_navOpen == NavSectionOf(PMonitor), L"keyboard Down: Dashboard -> System Monitor, section expands");
    for (int want : {PHardware, PNetwork, PFan}) {   // the children of Monitor in rail order (Hardware Information and Network Monitor sit after System Monitor; the Sidebar has its own section since 2.7)
        SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0); StPump(200);
        StCheck(g_page == want, std::wstring(L"keyboard Down: -> ") + kPageName[want] + L" (child)");
    }
    SendMessageW(h, WM_KEYDOWN, VK_LEFT, 0); StPump(400);
    StCheck(g_navOpen == -1 && g_page == PFan, L"keyboard Left collapses, page stays");
    SendMessageW(h, WM_KEYDOWN, VK_RIGHT, 0); StPump(400);
    StCheck(g_navOpen == NavSectionOf(PFan), L"keyboard Right expands");
    SendMessageW(h, WM_KEYDOWN, VK_END, 0); StPump(200);
    StCheck(g_page == PAbout && g_navOpen == -1, L"keyboard End -> About, other sections collapse");
    StCheck(StSavePng(h, dir + L"\\nav_keyboard.png"), L"capture keyboard focus ring");
    NavGo(PFan); StPump(400);
    {
        const std::string saved = ReadText(Cfg().Path());
        StCheck(saved.find("\"nav.page\": " + std::to_string(PFan)) != std::string::npos && saved.find("\"settings.version\": 1") != std::string::npos,
                L"navigation and settings version persisted to settings.json");
    }
    if (GetEnvironmentVariableW(L"ANKH_NAVONLY", nullptr, 0)) {   // diagnostics: only the page walk and the keyboard checks (a few minutes instead of ten)
        std::wstring all; for (const auto& l : g_stReport) all += l + L"\r\n";
        const std::string u = WToUtf8(all); WriteAll(dir + L"\\nav_report.txt", reinterpret_cast<const uint8_t*>(u.data()), u.size());
        StRestoreSettings(); g_quitting = true; DestroyWindow(h); return;
    }
    // widgets (the page walk may have toggled them from the Monitor page: start from off)
    // The control walk above clicked every widget setting; rows that appear and disappear shift its targets, so
    // what it leaves behind is not predictable. The widget checks below start from the default look and size.
    SbApplyProfile(SbBuiltinProfiles()[0].text);
    Cfg().SetInt(L"sidebar.posPct", -1); Cfg().SetInt(L"sidebar.autoHide", 0); Cfg().SetInt(L"sidebar.locked", 1);
    for (const wchar_t* k : {L"mini.width", L"mini.rowHeight", L"mini.glow"}) Cfg().SetInt(k, 0);
    Cfg().SetInt(L"mini.scale", 100); Cfg().SetInt(L"mini.background", 0); Cfg().SetInt(L"mini.accent", -1); Cfg().SetInt(L"mini.textColor", -1);
    Cfg().SetInt(L"monitor.intervalSec", 1); Cfg().SetInt(L"monitor.historyMin", 15); ApplyMonitorConfig(); NetSetMode(NetAuto, L"");
    Cfg().SetInt(L"appearance.material", MatAcrylic); Cfg().SetInt(L"appearance.tint", 68); Cfg().SetInt(L"appearance.glow", 2); Cfg().SetInt(L"appearance.motion", MotionFull); UpdateMotion(); ApplyMaterial(h);
    Cfg().Save();
    SbSetOption(L"", 0); MiniSetOption(L"", 0);
    ToggleSidebar(false); ToggleMini(false); StPump(500);   // the Sidebar slides away before its window goes
    SendMessageW(h, WM_COMMAND, 101, 0); SendMessageW(h, WM_COMMAND, 102, 0); StPump(1500);
    StCheck(g_sidebar && IsWindowVisible(g_sidebar), L"desktop sidebar opens");
    StCheck(g_mini && IsWindowVisible(g_mini), L"mini monitor opens");
    if (g_sidebar) StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar.png"), L"capture sidebar");
    if (g_mini) StCheck(StSavePng(g_mini, dir + L"\\widget_mini.png"), L"capture mini monitor");
    if (g_sidebar) {   // sidebar engine behaviour (2.5)
        auto ex = [] { return GetWindowLongPtrW(g_sidebar, GWL_EXSTYLE); };
        StCheck(Cfg().Str(L"sidebar.importedFrom", L"") != L"", L"sidebar: SidebarDiagnostics look imported once (" + Cfg().Str(L"sidebar.importedFrom", L"") + L")");
        {   // ---- Desktop mode (M-DSK1): what the desktop actually does, not which styles are set
            PerMonitorDpiScope pm;
            auto cls = [](HWND w) { wchar_t b[64] = L""; if (w) GetClassNameW(w, b, 64); return std::wstring(b); };
            auto hosted = [] { return g_sidebar && g_sbHost && (GetWindowLongPtrW(g_sidebar, GWL_STYLE) & WS_CHILD) && GetParent(g_sidebar) == g_sbHost; };
            auto mid = [] { RECT r; GetWindowRect(g_sidebar, &r); return POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; };
            StCheck(Cfg().Has(L"sidebar.desktopMigrated"), L"desktop: the one-time move to Desktop mode has run (previous layer kept: " + std::to_wstring(Cfg().Int(L"sidebar.prevLayer", -1, -1, 3)) + L")");
            SbSetOption(L"sidebar.layer", 1); SbSetOption(L"sidebar.reserve", 0); StPump(500);
            RECT wa0{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa0, 0);
            Cfg().SetBool(L"sidebar.reserve", true); Cfg().SetBool(L"sidebar.clickThrough", false); Cfg().SetInt(L"sidebar.autoHide", 1);   // overlay options that Desktop mode must ignore
            SbSetOption(L"sidebar.layer", 3); StPump(900);
            RECT wa1{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa1, 0);
            HWND rootW = g_sidebar ? GetAncestor(g_sidebar, GA_ROOT) : nullptr; DWORD rootPid = 0; GetWindowThreadProcessId(rootW, &rootPid);
            StCheck(hosted() && rootW != g_sidebar && rootPid != GetCurrentProcessId() && IsWindowVisible(g_sidebar),
                    L"desktop: the Sidebar is a child of the desktop (host " + cls(g_sbHost) + L", top window " + cls(rootW) + L")");
            StCheck(EqualRect(&wa0, &wa1) && !g_sbAppBar && !g_sb.reserve, L"desktop: no screen space is reserved, even with a saved \"reserve the edge\" (work area " + std::to_wstring(wa1.right - wa1.left) + L" x " + std::to_wstring(wa1.bottom - wa1.top) + L" unchanged)");
            RECT sr{}; GetWindowRect(g_sidebar, &sr); const MonitorInfo dm = SbMonitor(g_sb);
            StCheck((g_sb.edge ? sr.right == dm.work.right : sr.left == dm.work.left) && sr.top == dm.work.top && sr.bottom == dm.work.bottom && sr.right - sr.left == MulDiv(g_sb.width, static_cast<int>(MonitorDpi(dm.h)), 96),
                    L"desktop: it sits at the screen edge at its own width and the full work-area height (window " + std::to_wstring(sr.left) + L"," + std::to_wstring(sr.top) + L"," + std::to_wstring(sr.right) + L"," + std::to_wstring(sr.bottom) +
                    L"; work area " + std::to_wstring(dm.work.left) + L"," + std::to_wstring(dm.work.top) + L"," + std::to_wstring(dm.work.right) + L"," + std::to_wstring(dm.work.bottom) + L"; width setting " + std::to_wstring(g_sb.width) + L")");
            {   const WidgetFrame& wf = g_widgetFrame[1]; size_t clear = 0; for (const uint32_t p : wf.px) clear += (p >> 24) != 255;
                StCheck(!wf.px.empty() && clear == 0 && !g_sbBack.px.p.empty() && EqualRect(&g_sbBack.rc, &g_sbBase),
                        L"desktop: the frame is drawn over the wallpaper behind it (" + std::to_wstring(wf.w) + L" x " + std::to_wstring(wf.h) + L" px, " + std::to_wstring(clear) + L" pixels left transparent)"); }
            HWND under = WindowFromPoint(mid());
            StCheck(under != g_sidebar && SendMessageW(g_sidebar, WM_NCHITTEST, 0, MAKELPARAM(mid().x, mid().y)) == HTTRANSPARENT && g_sb.clickThrough && !g_sb.autoHide,
                    L"desktop: the mouse never reaches the Sidebar (the window at its centre is " + cls(under) + L"), even with \"interactive\" saved");
            StCheck(GetForegroundWindow() != g_sidebar && GetFocus() != g_sidebar && SendMessageW(g_sidebar, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, L"desktop: it never has the focus or the foreground");
            {   // an ordinary window placed over it must cover it: on screen, and for the mouse
                static const wchar_t* kCover = L"AnkhCoreCoverProbe";
                WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = kCover; wc.hbrBackground = CreateSolidBrush(RGB(18, 52, 86)); RegisterClassExW(&wc);
                HWND cover = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kCover, L"", WS_POPUP, sr.left, sr.top, sr.right - sr.left, sr.bottom - sr.top, nullptr, nullptr, wc.hInstance, nullptr);
                SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); UpdateWindow(cover); StPump(350);
                HDC sdc = GetDC(nullptr); const COLORREF px = GetPixel(sdc, mid().x, mid().y); ReleaseDC(nullptr, sdc);
                const HWND top = WindowFromPoint(mid());
                {   // when this fails, say which window was on top and what colour was there: the probe depends on nothing else being above it
                    wchar_t tc[64] = L"", tt[64] = L""; if (top) { GetClassNameW(top, tc, 64); GetWindowTextW(top, tt, 64); }
                    const bool covered = top == cover && px == RGB(18, 52, 86);
                    StCheck(covered, L"desktop: an ordinary window covers the Sidebar completely (on screen and for the mouse)" + (covered ? std::wstring() : std::wstring(L" - window at the point: ") + tc + L" '" + tt + L"', pixel " + std::to_wstring(GetRValue(px)) + L"," + std::to_wstring(GetGValue(px)) + L"," + std::to_wstring(GetBValue(px))));
                }
                DestroyWindow(cover);
            }
            SendMessageW(h, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0); StPump(700);
            StCheck(hosted() && IsWindowVisible(g_sidebar), L"desktop: it is still part of the desktop after an Explorer-restart notification");
            DestroyWindow(g_sidebar); StPump(2600);   // what happens to a child window when its desktop host is destroyed
            StCheck(g_sidebarOn && hosted() && IsWindowVisible(g_sidebar), L"desktop: a Sidebar lost with its desktop host comes back by itself and joins the desktop again");
            g_sbHostDeny = true; SbSetOption(L"", 0); StPump(500);
            RECT wa2{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa2, 0);
            const LONG_PTR fx = GetWindowLongPtrW(g_sidebar, GWL_EXSTYLE);
            StCheck(!g_sbHost && !(GetWindowLongPtrW(g_sidebar, GWL_STYLE) & WS_CHILD) && !(fx & WS_EX_TOPMOST) && (fx & WS_EX_TRANSPARENT) && EqualRect(&wa0, &wa2) && WindowFromPoint(mid()) != g_sidebar,
                    L"desktop: without a desktop host it falls back to a window below all others, still without reserved space or mouse input");
            g_sbHostDeny = false; SbSetOption(L"", 0); StPump(500);
            StCheck(hosted(), L"desktop: it joins the desktop again once a host is available");
            {   // FT-D01: each way the Sidebar can leave the desktop is noticed by the health check and put right, with one window and no focus change
                const HWND fg0 = GetForegroundWindow(); const int rec0 = g_sbRecoveries;
                StCheck(SbHealthTick(true) == SbHealth::Healthy && g_sbRecoveries == rec0, L"sidebar health: a Sidebar that is in order is left alone");
                ShowWindow(g_sidebar, SW_HIDE);
                StCheck(SbHealthTick(true) == SbHealth::Reshow && IsWindowVisible(g_sidebar) && hosted() && SbWindowCount() == 1, L"sidebar health: a hidden Sidebar is shown again, still one window on the desktop");
                SetParent(g_sidebar, GetDesktopWindow());   // attached to something that is not the current desktop layer
                StCheck(SbHealthTick(true) == SbHealth::Rejoin && hosted() && g_sbHost && GetParent(g_sidebar) == g_sbHost && IsWindowVisible(g_sidebar) && SbWindowCount() == 1,
                        L"sidebar health: a Sidebar attached to a stale or wrong parent joins the current desktop layer again");
                g_sbHostDeny = true; SbSetOption(L"", 0); StPump(300);
                StCheck(!g_sbHost && SbHealthTick(true) == SbHealth::Waiting, L"sidebar health: while the shell offers no desktop layer it waits and does not loop");
                g_sbHostDeny = false;   // the shell offers a layer again, but nothing re-applied the Sidebar: the health check does it
                StCheck(!g_sbHost && SbHealthTick(true) == SbHealth::Rejoin && hosted() && SbWindowCount() == 1, L"sidebar health: a Sidebar that fell back to a window below all others rejoins the desktop by itself when the layer is back");
                StCheck(SbHealthTick(true) == SbHealth::Healthy && g_sbHealthLast.empty(), L"sidebar health: and is then reported healthy");
                for (int i = 0; i < 8; ++i) { ToggleSidebar(false, false); ToggleSidebar(true, false); StPump(80); }   // repeated off/on
                StPump(400);
                StCheck(g_sidebar && hosted() && IsWindowVisible(g_sidebar) && SbWindowCount() == 1 && SbHealthTick(true) == SbHealth::Healthy, L"sidebar health: eight quick off/on cycles leave exactly one healthy Sidebar on the desktop");
                StCheck(GetForegroundWindow() == fg0 || GetForegroundWindow() != g_sidebar, L"sidebar health: no recovery gave the Sidebar the focus or foreground");
                {   // Sidebar lifecycle: loss kinds and bounded recovery (the real window and the real timers; Explorer itself is never restarted)
                    ToggleSidebar(false); StPump(700); StPump(2500);
                    StCheck(!g_sidebar && !g_sidebarOn && !SbRecoveryFailed(), L"lifecycle: a Sidebar the user closed stays closed (no automatic reopen)");
                    ToggleSidebar(true); StPump(800);
                    ToggleSidebar(false); DestroyWindow(g_sidebar); StPump(100);   // the host goes away while the slide-out is running
                    StCheck(!g_sidebar && !g_sbClosing && !g_sbDragging && !g_sidebarOn, L"lifecycle: a window lost during the user's slide-out leaves no stale busy state");
                    ToggleSidebar(true); StPump(800);
                    StCheck(g_sidebar && hosted() && IsWindowVisible(g_sidebar) && SbWindowCount() == 1 && SbHealthTick(true) == SbHealth::Healthy, L"lifecycle: switched on again after that, the Sidebar is placed, visible and healthy (it used to stay unplaced)");
                    g_sbDragging = true; DestroyWindow(g_sidebar); StPump(2600);
                    StCheck(!g_sbDragging && g_sidebar && hosted() && IsWindowVisible(g_sidebar) && SbWindowCount() == 1, L"lifecycle: a window lost mid-drag is reopened and the drag state is cleared");
                    StCheck(g_sbLife.tries == 1 && !g_sbLife.gaveUp, L"lifecycle: one unexpected loss is one recovery attempt");
                    // a desktop that destroys the Sidebar again every time it is recreated
                    for (int i = 0; i < 80 && !g_sbLife.gaveUp; ++i) { if (g_sidebar) DestroyWindow(g_sidebar); StPump(300); }
                    if (g_sidebar) { DestroyWindow(g_sidebar); StPump(300); }   // the last attempt's window is lost as well
                    StCheck(g_sbLife.gaveUp && !g_sidebar && SbRecoveryFailed() && g_sbLife.tries <= kSbRetryMax && SbWindowCount() == 0, L"lifecycle: repeated destruction ends in a bounded number of attempts (" + std::to_wstring(g_sbLife.tries) + L"), not an endless loop; the Sidebar is reported as not restored");
                    const int tries0 = g_sbLife.tries; StPump(3000);
                    StCheck(!g_sidebar && g_sbLife.tries == tries0, L"lifecycle: after giving up nothing is attempted on its own");
                    g_sbLife.gaveUpAt = GetTickCount64() - kSbKickGapMs - 100; SbShellEventNote(SbShellEvent::ExplorerRestarted, L"self-test: Explorer restart event"); StPump(2600);
                    StCheck(g_sidebar && hosted() && IsWindowVisible(g_sidebar) && SbWindowCount() == 1 && !SbRecoveryFailed(), L"lifecycle: a shell event after giving up brings it back, one window");
                    StCheck(!g_sbLife.gaveUp || g_sidebar, L"lifecycle: the recovery counters survive only as long as they are needed");
                    ToggleSidebar(false, false); StPump(2600);
                    StCheck(!g_sidebar && !g_sidebarOn, L"lifecycle: closing at Exit never triggers an automatic reopen");
                    ToggleSidebar(true, false); StPump(700);
                }
            }
            SbSetOption(L"sidebar.layer", Cfg().Int(L"sidebar.prevLayer", 1, 0, 2)); StPump(400);
            StCheck(!g_sbHost && !(GetWindowLongPtrW(g_sidebar, GWL_STYLE) & WS_CHILD) && GetParent(g_sidebar) == nullptr, L"desktop: restoring the previous placement makes it an ordinary window again");
            Cfg().SetBool(L"sidebar.reserve", false); Cfg().SetInt(L"sidebar.autoHide", 0); SbSetOption(L"sidebar.layer", 1); StPump(300);   // the checks below exercise the overlay layers
        }
        SbSetOption(L"sidebar.clickThrough", 1); StPump(150);
        StCheck((ex() & WS_EX_TRANSPARENT) && (ex() & WS_EX_LAYERED), L"sidebar: click-through sets WS_EX_TRANSPARENT");
        SbSetOption(L"sidebar.clickThrough", 0); StPump(150);
        StCheck(!(ex() & WS_EX_TRANSPARENT), L"sidebar: interactive clears WS_EX_TRANSPARENT");
        StCheck(SendMessageW(g_sidebar, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE && (ex() & WS_EX_NOACTIVATE), L"sidebar: never takes focus");
        SbSetOption(L"sidebar.layer", 0); StPump(150);
        StCheck((ex() & WS_EX_TOPMOST) != 0, L"sidebar: always-on-top layer");
        SbSetOption(L"sidebar.layer", 1); StPump(150);
        StCheck((ex() & WS_EX_TOPMOST) == 0, L"sidebar: normal layer");
        SbSetOption(L"sidebar.edge", 1); SbSetOption(L"sidebar.width", 210); SbSetOption(L"sidebar.reserve", 0); StPump(200);
        const MonitorInfo mon = SbMonitor(g_sb);
        RECT wr; GetWindowRect(g_sidebar, &wr);
        const int expW = MulDiv(210, static_cast<int>(MonitorDpi(mon.h)), 96);
        StCheck(wr.right == mon.work.right && wr.right - wr.left == expW && wr.top == mon.work.top && wr.bottom == mon.work.bottom,
                L"sidebar: docked to the right edge of the work area at monitor DPI (" + std::to_wstring(wr.right - wr.left) + L" px)");
        SbSetOption(L"sidebar.edge", 0); StPump(200); GetWindowRect(g_sidebar, &wr);
        StCheck(wr.left == mon.work.left, L"sidebar: docked to the left edge");
        SbSetOption(L"sidebar.edge", 1); StPump(100);
        MONITORINFO before{sizeof(before)}; GetMonitorInfoW(mon.h, &before);
        SbSetOption(L"sidebar.reserve", 1); StPump(800);
        MONITORINFO during{sizeof(during)}; GetMonitorInfoW(mon.h, &during);
        StCheck(during.rcWork.right <= before.rcWork.right - expW + 2, L"sidebar: reserving the edge shrinks the work area (AppBar)");
        SbSetOption(L"sidebar.reserve", 0); StPump(800);
        MONITORINFO after{sizeof(after)}; GetMonitorInfoW(mon.h, &after);
        StCheck(EqualRect(&after.rcWork, &before.rcWork), L"sidebar: releasing the edge restores the work area");
        SbSetOption(L"sidebar.opacity", 25); StPump(200);
        StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_25.png"), L"capture sidebar at 25% background");
        {   // sections: density, visibility and order come from the pure layout and always fit the height
            for (int i = 0; i < 20 && g_sys.cores.empty(); ++i) StPump(250);
            SYSTEM_INFO si; GetSystemInfo(&si);
            StCheck(g_sys.cores.size() == si.dwNumberOfProcessors && g_sys.logical == static_cast<int>(si.dwNumberOfProcessors) && g_sys.physCores > 0 && g_sys.physCores <= g_sys.logical,
                    L"sidebar data: per-core usage for " + std::to_wstring(g_sys.cores.size()) + L" logical processors (" + std::to_wstring(g_sys.physCores) + L" cores) arrives in the engine frame");
            StCheck(!g_sys.drives.empty() && g_sys.drives[0].total > 0 && g_sys.drives[0].free <= g_sys.drives[0].total && g_sys.memAvail > 0 && g_sys.commitLimit >= g_sys.commitUsed,
                    L"sidebar data: " + std::to_wstring(g_sys.drives.size()) + L" fixed drive(s) and memory detail arrive in the engine frame");
            SbSetSections(SbParseSections(L""));
            static const wchar_t* const names[3] = {L"minimal", L"standard", L"detailed"};
            int usedAt[3] = {};
            for (int d = 0; d < 3; ++d) {
                SbSetOption(L"sidebar.layout", d); StPump(250);
                usedAt[d] = g_sbLayout.used;
                StCheck(g_sbLayout.boxes.size() + static_cast<size_t>(g_sbLayout.dropped) == SbSecCount && g_sbLayout.used > 0 && g_sbLayout.used <= g_sbAvail,
                        std::wstring(L"sidebar ") + names[d] + L": " + std::to_wstring(g_sbLayout.boxes.size()) + L" sections use " + std::to_wstring(g_sbLayout.used) + L" of " + std::to_wstring(g_sbAvail) + L" DIPs (" + std::to_wstring(g_sbLayout.reduced) + L" reduced)");
                StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_" + names[d] + L".png"), std::wstring(L"capture sidebar, ") + names[d]);
            }
            StCheck(usedAt[0] < usedAt[1] && usedAt[1] <= usedAt[2], L"sidebar: more detail uses more of the height");
            SbSetOption(L"sidebar.layout", SbStandard);
            auto items = SbParseSections(L""); SbShowSection(items, SecGpu, false); SbMoveSection(items, SecNet, -1); SbMoveSection(items, SecNet, -1); SbMoveSection(items, SecNet, -1); SbMoveSection(items, SecNet, -1);
            SbSetSections(items); StPump(250);
            bool gpuDrawn = false; for (const auto& b : g_sbLayout.boxes) gpuDrawn |= b.sec == SecGpu;
            StCheck(!gpuDrawn && !g_sbLayout.boxes.empty() && g_sbLayout.boxes[0].sec == SecNet && Cfg().Str(L"sidebar.sections", L"") == L"net,cpu,-gpu,mem,storage,fans",
                    L"sidebar: a hidden section is not drawn, a moved one leads, and both are saved (" + Cfg().Str(L"sidebar.sections", L"") + L")");
            SbSetSections(SbParseSections(L"")); StPump(150);
        }
        const std::string saved = ReadText(Cfg().Path());
        StCheck(saved.find("\"sidebar.opacity\": 25") != std::string::npos && saved.find("\"sidebar.enabled\": true") != std::string::npos, L"sidebar settings persisted");
        {   // customization (M-SB2): styles, custom detail, size, appearance, profiles
            auto kinds = [](SbRowKind k, bool& paired) { int n = 0; for (const auto& b : g_sbLayout.boxes) for (const auto& rw2 : b.rows) if (rw2.kind == k) { ++n; paired |= rw2.cols == 2; } return n; };
            auto winRect = [] { PerMonitorDpiScope pm; RECT rc{}; GetWindowRect(g_sidebar, &rc); return rc; };
            SbApplyProfile(SbBuiltinProfiles()[0].text); StPump(250);
            const int usedBars = g_sbLayout.used; const RECT full = winRect(); bool pr = false;
            StCheck(kinds(SbGauge, pr) == 0 && kinds(SbBar, pr) > 6 && g_sb.background == 0 && g_sb.accent == -1, L"sidebar look: the Default profile draws indicators on a solid background in the theme colours");
            SbSetStr(L"sidebar.styles", L"cpu:1,gpu:1,mem:1,storage:1,net:0,fans:1"); StPump(250); pr = false;
            const int gauges = kinds(SbGauge, pr);
            StCheck(gauges >= 6 && pr && g_sbLayout.used <= g_sbAvail, L"sidebar style: " + std::to_wstring(gauges) + L" gauges, two to a line, fit the height");
            StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_gauges.png"), L"capture sidebar, gauges");
            SbSetStr(L"sidebar.styles", L"cpu:3,gpu:3,mem:3,storage:3,net:3,fans:3"); StPump(250); pr = false;
            bool sectionGraph = false; for (const auto& b : g_sbLayout.boxes) for (const auto& rw2 : b.rows) sectionGraph |= rw2.id == RCpuGraph;
            StCheck(kinds(SbSpark, pr) >= 5 && !sectionGraph && kinds(SbBar, pr) >= 1, L"sidebar style: graphs give each metric with history a sparkline; drives stay indicators");
            StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_graphs.png"), L"capture sidebar, graphs");
            SbSetStr(L"sidebar.styles", L"cpu:2,gpu:2,mem:2,storage:2,net:2,fans:2"); StPump(250); pr = false;
            StCheck(kinds(SbValue, pr) > 6 && kinds(SbBar, pr) == 0 && g_sbLayout.used < usedBars, L"sidebar style: numeric values need less height than indicators");
            StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_values.png"), L"capture sidebar, values");
            SbSetStr(L"sidebar.styles", L""); SbSetStr(L"sidebar.levels", L"cpu:2,gpu:0,mem:0,storage:0,net:0,fans:0"); SbSetOption(L"sidebar.layout", SbCustom); StPump(250);
            StCheck(g_sbLayout.boxes.size() == SbSecCount && g_sbLayout.boxes[0].level == SbDetailed && g_sbLayout.boxes[1].level == SbMinimal && g_sbLayout.boxes[5].level == SbMinimal, L"sidebar custom layout: each section keeps its own detail level");
            SbSetOption(L"sidebar.layout", SbStandard);
            // size
            SbSetOption(L"sidebar.heightPct", 50); SbSetOption(L"sidebar.valign", 2); StPump(250);
            const RECT half = winRect(); const int fh = full.bottom - full.top, hh = half.bottom - half.top;
            StCheck(std::abs(hh - fh / 2) <= 2 && half.bottom == full.bottom && half.right - half.left == full.right - full.left && g_sbLayout.used <= g_sbAvail,
                    L"sidebar size: half height sits at the bottom of the edge (" + std::to_wstring(hh) + L" of " + std::to_wstring(fh) + L" px) and the sections still fit (" + std::to_wstring(g_sbLayout.reduced) + L" reduced, " + std::to_wstring(g_sbLayout.dropped) + L" dropped)");
            SbSetOption(L"sidebar.heightPct", 100); SbSetOption(L"sidebar.valign", 0);
            const int dpi0 = g_sbUi.dpi, avail0 = g_sbAvail;
            SbSetOption(L"sidebar.fontScale", 130); StPump(250);
            const RECT big = winRect();
            StCheck(g_sbUi.dpi == MulDiv(dpi0, 130, 100) && big.right - big.left == full.right - full.left && g_sbAvail < avail0 && g_sbLayout.used <= g_sbAvail,
                    L"sidebar size: 130% text scales the content, not the width, and the layout refits (" + std::to_wstring(g_sbLayout.reduced) + L" reduced)");
            SbSetOption(L"sidebar.fontScale", 100);
            SbSetOption(L"sidebar.spacing", 0); StPump(200); const int tightUsed = g_sbLayout.used;
            SbSetOption(L"sidebar.spacing", 2); StPump(200);
            StCheck(tightUsed <= g_sbLayout.used && g_sbLayout.used <= g_sbAvail, L"sidebar size: compact spacing never needs more height than relaxed");
            SbSetOption(L"sidebar.spacing", 1);
            // appearance
            SbSetOption(L"sidebar.opacity", 100); SbSetOption(L"sidebar.accent", 0xFF5C5C); StPump(250);
            const WidgetFrame& wf = g_widgetFrame[1];
            auto edgePx = [&] { const uint32_t p = wf.w > 0 && wf.h > 8 ? wf.px[static_cast<size_t>(wf.h / 8) * wf.w + (g_sb.edge ? 0 : wf.w - 1)] : 0; return p; };
            const uint32_t red = edgePx();
            StCheck(((red >> 16) & 255) > 180 && (red & 255) < 140, L"sidebar colour: a custom accent reaches the drawn frame (edge pixel is red)");
            SbSetOption(L"sidebar.accent", -1); StPump(200); const uint32_t blue = edgePx();
            StCheck((blue & 255) > 180 && ((blue >> 16) & 255) < 80, L"sidebar colour: the theme accent returns");
            auto midPx = [&] { return wf.w > 0 ? wf.px[static_cast<size_t>(wf.h - 2) * wf.w + wf.w / 2] : 0u; };   // bare background, below the footer
            SbSetOption(L"sidebar.opacity", 50); SbSetOption(L"sidebar.background", 0); StPump(250); const uint32_t solid = midPx();
            SbSetOption(L"sidebar.background", 2); SbSetOption(L"sidebar.tint", 30); StPump(400); const uint32_t acrylic = midPx();
            SbSetOption(L"sidebar.background", 1); StPump(400); const uint32_t frosted = midPx();
            StCheck((solid >> 24) > 100 && (solid >> 24) < 150, L"sidebar background: solid at 50% opacity is half transparent (alpha " + std::to_wstring(solid >> 24) + L")");
            StCheck(acrylic != solid && frosted != acrylic, L"sidebar background: solid, acrylic blur and frosted glass draw three different backgrounds");
            StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_frosted.png"), L"capture sidebar, frosted glass");
            SbSetOption(L"sidebar.glow", 2); SbSetOption(L"sidebar.animate", 1); StPump(1300);
            StCheck(g_sb.glow == 2 && g_sb.animate && g_sbAnimFrames <= 3, L"sidebar animation: at most three in-between frames per data frame (" + std::to_wstring(g_sbAnimFrames) + L")");
            // profiles
            SbSetOption(L"sidebar.accent", 0x2DD4BF); SbSetOption(L"sidebar.width", 240);
            const std::wstring mine = SbCurrentProfile();
            Cfg().SetStr(L"sidebar.profile.1", mine); Cfg().Save();
            SbApplyProfile(SbBuiltinProfiles()[0].text); StPump(250);
            StCheck(g_sb.accent == -1 && g_sb.background == 0 && g_sb.width == 210 && !g_sb.animate && SbCurrentProfile() != mine, L"sidebar profile: Default resets the look");
            SbApplyProfile(Cfg().Str(L"sidebar.profile.1", L"")); StPump(250);
            StCheck(g_sb.accent == 0x2DD4BF && g_sb.background == 1 && g_sb.width == 240 && g_sb.glow == 2 && g_sb.animate && SbCurrentProfile() == mine, L"sidebar profile: a saved look comes back exactly");
            const int edge0 = g_sb.edge; const bool ct0 = g_sb.clickThrough;
            SbApplyProfile(L"sidebar.width=9999|sidebar.edge=" + std::to_wstring(1 - edge0) + L"|sidebar.clickThrough=1|evil.key=1|sidebar.sections=<script>"); StPump(200);
            StCheck(g_sb.width == 240 && g_sb.edge == edge0 && g_sb.clickThrough == ct0 && !Cfg().Has(L"evil.key") && Cfg().Str(L"sidebar.sections", L"").find(L'<') == std::wstring::npos,
                    L"sidebar profile: out-of-range values, behaviour keys and unknown keys in a profile are ignored");
            for (const SbBuiltin& b : SbBuiltinProfiles()) { SbApplyProfile(b.text); StPump(300); StCheck(!g_sbLayout.boxes.empty() && g_sbLayout.used <= g_sbAvail, std::wstring(L"sidebar profile \"") + b.name + L"\" applies and fits"); }
            StCheck(StSavePng(g_sidebar, dir + L"\\widget_sidebar_graphwall.png"), L"capture sidebar, Graph wall profile");
            {   // the settings with their live preview (System Monitor, scrolled to the Sidebar's look)
                {   // ---- settings pages (M-NAV): reached by real clicks from their hub pages, Back works, and looking changes nothing
        auto noNav = [](std::string s) { for (size_t at; (at = s.find("\"nav.")) != std::string::npos;) { const size_t e = s.find_first_of(",\n}", at); s.erase(at, (e == std::string::npos ? s.size() : e) - at); } return s; };
        const std::string before = noNav(ReadText(Cfg().Path()));
        for (int pg = PSbLayout; pg <= PRecHistory; ++pg) { NavGo(pg); StPump(80); StRepaint(h); }
        Cfg().Save();
        StCheck(noNav(ReadText(Cfg().Path())) == before, L"settings pages: opening every Sidebar, Mini Monitor, Monitoring Preferences, Appearance, Apps and Recovery sub-page changes no setting");
        const std::string saved0 = ReadText(Cfg().Path());
        const RECT cr2 = ContentRect(); const int viewH2 = cr2.bottom - cr2.top;
        auto walkHub = [&](int hub, int firstChild, int lastChild) {
            std::set<int> reached;
            const size_t need = static_cast<size_t>(lastChild - firstChild + 1);
            NavGo(hub); g_scroll[hub] = 0; StPump(200);
            for (int screens = 0; screens < 40 && reached.size() < need; ++screens) {
                StRepaint(h);
                std::vector<RECT> rects; for (const auto& hit : g_ui.hits) rects.push_back(hit.r);
                for (const RECT& r : rects) {
                    const POINT pt{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
                    if (r.right <= SideW() || pt.y < cr2.top || pt.y >= ClientH() || pt.x >= ClientW()) continue;
                    const bool sbWas = g_sidebarOn, miniWas = g_miniOn;
                    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y)); StPump(40);
                    if (g_page != hub) { if (g_page >= firstChild && g_page <= lastChild) reached.insert(g_page); g_page = hub; StRepaint(h); }
                    if (g_sidebarOn != sbWas) ToggleSidebar(sbWas); if (g_miniOn != miniWas) ToggleMini(miniWas);
                    if (reached.size() >= need) break;
                }
                const int maxS = std::max(0, g_contentH[hub] - viewH2);
                if (g_scroll[hub] >= maxS) break;
                g_scroll[hub] = std::min(maxS, g_scroll[hub] + viewH2 * 3 / 4);
            }
            g_scroll[hub] = 0;
            return reached;
        };
        const std::set<int> reachedDsk = walkHub(PDesktop, PSbLayout, PMonPrefs);
        const std::set<int> reachedApp = walkHub(PAppearance, PAppTheme, PAppIcons);
        const std::set<int> reachedSettings = walkHub(PSettings, PAppMaterial, PAppMaterial);
        const std::set<int> reachedApps = walkHub(PApps, PAppsInstalled, PAppsUninstall);
        const std::set<int> reachedRec = walkHub(PRecovery, PBackup, PRecHistory);
        const std::set<int> reachedOpt = walkHub(POptimize, PDrivers, PStorage);
        // the walk clicked controls on the hub pages: put the settings back as they were
        if (Cfg().LoadFromText(Utf8ToW(saved0))) { Cfg().Save(); ApplyMonitorConfig(); SbSetOption(L"", 0); MiniSetOption(L"", 0); UpdateMotion(); ApplyMaterial(h); }
        bool allDsk = true; for (int pg = PSbLayout; pg <= PMonPrefs; ++pg) allDsk = allDsk && reachedDsk.count(pg);
        StCheck(allDsk, L"settings pages: Layout, Appearance, Widgets, Behavior, Profiles, Mini Monitor and Monitoring Preferences each open with a click from Desktop Monitoring (" + std::to_wstring(reachedDsk.size()) + L" pages reached)");
        bool allApp = true; for (int pg = PAppTheme; pg <= PAppIcons; ++pg) allApp = allApp && reachedApp.count(pg);
        StCheck(allApp, L"settings pages: Windows Theme, Wallpaper, Cursor and Icon Manager each open with a click from Appearance & Themes (" + std::to_wstring(reachedApp.size()) + L" pages reached)");
        StCheck(reachedSettings.count(PAppMaterial) == 1, L"settings pages: AnkhCore Appearance opens with a click from Settings (the top-bar page)");
        {   // the top-bar Settings control: opens Settings from any page and Back returns to where it was opened
            NavGo(PHardware); StPump(150); StRepaint(h);
            POINT gp{-1, -1}; for (const auto& hit : g_ui.hits) if (hit.r.top < g_ui.S(60) && hit.r.right > ClientW() - g_ui.S(170) && hit.r.right < ClientW() - g_ui.S(100)) gp = POINT{(hit.r.left + hit.r.right) / 2, (hit.r.top + hit.r.bottom) / 2};
            if (gp.x >= 0) SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(gp.x, gp.y)); StPump(200);
            StCheck(gp.x >= 0 && g_page == PSettings, L"settings: the top-bar Settings control opens Settings from Hardware Information");
            SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(200);
            StCheck(g_page == PHardware, L"settings: Backspace returns to the page Settings was opened from");
        }
        bool allApps = true; for (int pg = PAppsInstalled; pg <= PAppsUninstall; ++pg) allApps = allApps && reachedApps.count(pg);
        StCheck(allApps, L"settings pages: Installed Applications, Windows Built-in Apps and Uninstall Manager each open with a click from Apps (" + std::to_wstring(reachedApps.size()) + L" pages reached)");
        bool allRec = true; for (int pg = PBackup; pg <= PRecHistory; ++pg) allRec = allRec && reachedRec.count(pg);
        StCheck(allRec, L"settings pages: Backup & Migration and Recovery History each open with a click from Recovery (" + std::to_wstring(reachedRec.size()) + L" pages reached)");
        bool allOpt = true; for (int pg = PDrivers; pg <= PStorage; ++pg) allOpt = allOpt && reachedOpt.count(pg);
        StCheck(allOpt, L"settings pages: Driver Manager and Storage Analyzer each open with a click from Optimize & Repair (" + std::to_wstring(reachedOpt.size()) + L" pages reached)");
        NavGo(PSbBehavior); StPump(150); StRepaint(h);
        StCheck(NavSectionOf(PSbBehavior) == NavSectionOf(PDesktop) && kNav[NavSectionOf(PDesktop)].pages.size() == 1 && kNav[NavSectionOf(PDesktop)].name == std::wstring(L"Sidebar"), L"settings pages: a sub-page keeps the Sidebar section highlighted in the rail");
        POINT bp{-1, -1}; for (const auto& hit : g_ui.hits) if (hit.r.left >= SideW() && hit.r.bottom <= g_ui.S(kHeader) && hit.r.right < SideW() + g_ui.S(80)) bp = POINT{(hit.r.left + hit.r.right) / 2, (hit.r.top + hit.r.bottom) / 2};
        if (bp.x >= 0) SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bp.x, bp.y)); StPump(150);
        StCheck(bp.x >= 0 && g_page == PDesktop, L"settings pages: the Back control in the header returns to Desktop Monitoring");
        NavGo(PMonPrefs); StPump(100); SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(150);
        StCheck(g_page == PDesktop, L"settings pages: Backspace goes back from Monitoring Preferences to Desktop Monitoring");
        NavGo(PAppIcons); StPump(100); SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(150);
        StCheck(g_page == PAppearance, L"settings pages: Backspace goes back from Icon Manager to Appearance");
        {   // FT-2 / A04: painting the Icon Manager reads cached state only (no module of any theme is read to paint it)
            NavGo(PAppIcons);
            for (int i = 0; i < 100 && !g_iconStatus.Loaded(); ++i) StPump(50);
            bool all = g_iconStatus.Loaded(); for (const auto& p : IconPacks()) all = all && g_iconStatus.Get(p.id).checked;
            StCheck(all, L"icon manager: the installed state of every theme is filled by the worker thread when the page opens");
            const int full0 = IconFullValidations();
            for (int i = 0; i < 6; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); }
            StCheck(g_page == PAppIcons && IconFullValidations() == full0, L"icon manager: six repaints of the page ran no full theme validation (" + std::to_wstring(IconFullValidations() - full0) + L")");
            StCheck(!LastIconRefresh().applied && !g_restartNeeded, L"icon manager: opening the page raises no restart prompt and no stale apply result");
            SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(100);
        }
        {   // Candy presentation: brand background behind the Candy card; opacity and blur act on the background only
            const std::wstring fixRoot = DataDir() + L"\\CandyFixture";   // test area only: IconThemes\candy with the owner's preview picture (docs: Candy background test)
            const std::wstring fixPng = fixRoot + L"\\IconThemes\\candy\\preview.png";
            if (!FileExists(fixPng)) {   // make the fixture from the project's own picture (found above the test exe): the test no longer needs a hand-made folder
                std::wstring up = DirOf(SelfPath());
                for (int i = 0; i < 6 && !up.empty(); ++i, up = DirOf(up)) {
                    const std::wstring src = up + L"\\recovered_res\\candy-icons.png";
                    if (FileExists(src)) { const std::string bytes = ReadText(src); EnsureDir(DirOf(fixPng)); if (!bytes.empty()) WriteAll(fixPng, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()); break; }
                    if (DirOf(up) == up) break;
                }
            }
            const bool haveFix = FileExists(fixPng);
            const std::wstring oldApp = AppDirOverride(); if (haveFix) AppDirOverride() = fixRoot;
            NavGo(PAppIcons); IconPageEnter(); StPump(300); for (int i = 0; i < 100 && !g_iconStatus.Loaded(); ++i) StPump(50);
            StCheck(haveFix, L"candy background: the Candy fixture folder is present in the test data area");
            const IconThemeStatus cst = g_iconStatus.Get(L"candy");
            StCheck(cst.installed && !cst.preview.empty(), L"candy background: the Candy theme fixture and its preview picture are present");
            { const bool wasDef = CandyOpacityCfg() == 100 && CandyBlurCfg() == 0; if (!wasDef) CandyBgReset();   // the generic control walk before this may have moved the sliders
              StCheck(CandyOpacityCfg() == 100 && CandyBlurCfg() == 0, L"candy background: defaults are 100% opacity and 0 px blur (after Reset " + std::wstring(wasDef ? L"not needed)" : L"needed: the walk had moved them)")); }
            { const uint8_t* d; size_t n; StCheck(ResourceBytes(L"BRANDBG", d, n) && n > 100000, L"candy background: the supplied brand background is embedded and readable"); }
            auto settle = [&] { for (int i = 0; i < 8; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); } };
            auto shot = [&](const wchar_t* name) {
                settle();
                StCheck(StSavePng(h, dir + L"\\candy_" + name + L".png"), std::wstring(L"capture Candy presentation, ") + name);
                const int sv = g_scroll[PAppIcons]; g_scroll[PAppIcons] = 100000; StRepaint(h); settle();   // the controls under the cards (the page clamps the scroll)
                StCheck(StSavePng(h, dir + L"\\candy_" + name + L"_controls.png"), std::wstring(L"capture Candy controls, ") + name);
                g_scroll[PAppIcons] = sv; StRepaint(h);
            };
            settle(); shot(L"default");
            const int sc0 = g_candyBg.Scales(), bl0 = g_candyBg.Blurs(), mt0 = g_candyBg.Mattes();
            StCheck(sc0 >= 1 && mt0 == 1, L"candy background: the background was scaled once and the Candy matte built once (" + std::to_wstring(sc0) + L" scale, " + std::to_wstring(mt0) + L" matte)");
            settle(); settle();
            StCheck(g_candyBg.Scales() == sc0 && g_candyBg.Blurs() == bl0 && g_candyBg.Mattes() == mt0, L"candy background: sixteen more repaints did no image processing");
            Cfg().SetInt(L"candy.bgOpacity", 40); settle();
            StCheck(g_candyBg.Scales() == sc0 && g_candyBg.Blurs() == bl0 && g_candyBg.Mattes() == mt0, L"candy background: changing opacity re-scaled and re-blurred nothing, and the matte is untouched");
            shot(L"opacity40");
            Cfg().SetInt(L"candy.bgOpacity", 0); shot(L"opacity0");
            Cfg().SetInt(L"candy.bgOpacity", 100); Cfg().SetInt(L"candy.bgBlur", 12); settle();
            StCheck(g_candyBg.Blurs() == bl0 + 1 && g_candyBg.Scales() == sc0 && g_candyBg.Mattes() == mt0, L"candy background: changing blur built one blurred variant, did not re-scale and left the matte alone");
            shot(L"blur12");
            Cfg().SetInt(L"candy.bgOpacity", 60); settle();
            StCheck(g_candyBg.Blurs() == bl0 + 1, L"candy background: opacity and blur are independent (an opacity change did not re-blur)");
            shot(L"opacity60_blur12");
            Cfg().Save();
            Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass"));   // what a restart does: read settings.json again
            StCheck(CandyOpacityCfg() == 60 && CandyBlurCfg() == 12, L"candy background: after reloading settings.json (a restart) 60% and 12 px are restored");
            CandyBgReset(); settle();
            StCheck(CandyOpacityCfg() == 100 && CandyBlurCfg() == 0, L"candy background: Reset restores 100% and 0 px");
            Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass")); StCheck(CandyOpacityCfg() == 100 && CandyBlurCfg() == 0, L"candy background: the reset values are the ones saved");
            shot(L"reset");
            Cfg().SetInt(L"candy.bgBlur", 999); Cfg().SetInt(L"candy.bgOpacity", -5);
            StCheck(CandyBlurCfg() == 30 && CandyOpacityCfg() == 0, L"candy background: out-of-range stored values are clamped to 0..30 px and 0..100 %");
            CandyBgReset();
            AppDirOverride() = oldApp; IconPageEnter(); StPump(100);
            SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(100);
        }
        {   // FT-C02: a switch on the Windhawk page only stages the request; the mod's real state is not touched until Apply
            NavGo(PWindhawk); StPump(250);
            { std::lock_guard<std::mutex> l(g_whMx); g_whStage.DiscardAll(); }   // the scripted page walk before this clicks every switch: its requests are only staged, and are cleared here
            const Cache sn = Snapshot();
            if (sn.mods.empty()) StCheck(true, L"windhawk page: no mods on this PC, so there is nothing to stage (page opens without error)");
            else {
                const WhMod m0 = sn.mods[0]; const std::string sid = WToUtf8(m0.id);
                const std::wstring key = std::wstring(WhModsKey()) + L"\\" + m0.id;
                const DWORD disabledBefore = RegDword(HKEY_LOCAL_MACHINE, key, L"Disabled", 0);
                { std::lock_guard<std::mutex> l(g_whMx); g_whStage.StageEnabled(sid, !m0.enabled, m0.enabled); }
                for (int i = 0; i < 4; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); }
                bool want = m0.enabled; size_t n; bool pending; { std::lock_guard<std::mutex> l(g_whMx); n = g_whStage.Count(); pending = g_whStage.EnabledPending(sid, want); }
                StCheck(n == 1 && pending && want == !m0.enabled && RegDword(HKEY_LOCAL_MACHINE, key, L"Disabled", 0) == disabledBefore && !g_busy,
                        L"windhawk page: switching a mod only stages it as pending (" + m0.name + L"); its real state in Windhawk is unchanged and no operation ran");
                { std::lock_guard<std::mutex> l(g_whMx); g_whStage.StageEnabled(sid, m0.enabled, m0.enabled); n = g_whStage.Count(); }
                StCheck(n == 0, L"windhawk page: switching it back cancels the request");
            }
            { std::lock_guard<std::mutex> l(g_whMx); g_whStage.DiscardAll(); g_whSummary.clear(); }
            SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(100);
        }
        {   // FT-RH: one operation is one row and is undone as a whole; a second operation with the same name and data is a separate row and is left alone
            static int rhUndone = 0; rhUndone = 0;
            UndoHandlers()[L"RHTEST"] = [](const JEntry&) { ++rhUndone; return true; };
            { JournalOpScope a(L"RH probe"); for (int i = 0; i < 4; ++i) JournalAdd(L"rh:test", L"RHTEST", L"RH probe", {L"v" + std::to_wstring(i)}); JournalAdd(L"rh:test", L"RHTEST", L"RH probe", {L"v0"}); }   // the repeat is not recorded
            { JournalOpScope b(L"RH probe"); for (int i = 0; i < 2; ++i) JournalAdd(L"rh:test", L"RHTEST", L"RH probe", {L"v" + std::to_wstring(i)}); }
            const auto all = JournalLoad(); const JournalView jv = JournalBuildView(all);
            StCheck(jv.ops.size() >= 2 && jv.ops[0].ids.size() == 2 && jv.ops[1].ids.size() == 4 && jv.ops[0].title == L"RH probe" && jv.ops[1].title == L"RH probe" && jv.ops[0].key != jv.ops[1].key,
                    L"recovery history: one operation is one row (4 entries; a repeated event was not recorded) and a second operation with the same name and data is its own row (" + std::to_wstring(jv.ops.size()) + L" rows in the test journal)");
            const UndoCount u = JournalUndoEx(JournalPickOp(jv.ops[1]));
            int stillActive = 0; for (const auto& e : JournalLoad()) if (e.group == L"rh:test" && e.state == L"A") ++stillActive;
            StCheck(u.picked == 4 && u.undone == 4 && rhUndone == 4 && stillActive == 2, L"recovery history: undoing an operation undoes exactly its 4 entries; the other operation's 2 entries stay active");
            const UndoCount rest = JournalUndoEx([](const JEntry& e) { return e.group == L"rh:test"; });
            StCheck(rest.picked == 2 && rest.undone == 2 && rhUndone == 6, L"recovery history: the remaining operation is undone on its own (test entries cleaned up through the same undo)");
            RefreshState(false); StPump(100);   // the page's data comes from the shared view
            NavGo(PRecHistory);
            for (int i = 0; i < 100 && !RecPoints(); ++i) StPump(50);
            g_recOpen.insert(Snapshot().journalView ? Snapshot().journalView->ops[0].key : 0);
            for (int i = 0; i < 4; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); }
            StCheck(g_page == PRecHistory && RecPoints() && Snapshot().journalView && !Snapshot().journalView->ops.empty(), L"recovery history: the page opens, the recovery points load on a worker thread, and a row's details open without error");
            g_recOpen.clear(); SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(100);
        }
        {   // FT-TOAST: acceptance on the real window: five quick operations, order, bounds, wrapping, one toast per operation, expiry
            g_toastQ.Clear(); NavGo(PAppCursor); StPump(200);
            const std::vector<std::wstring> names = {L"Done: Apply cursor scheme AEON", L"Done: Apply cursor scheme Azenis", L"Done: Restore previous cursor scheme", L"Done: Restore Windows default cursors", L"Done: Apply cursor scheme As3aD"};
            const uint64_t tAdd = GetTickCount64();
            for (size_t i = 0; i < names.size(); ++i) AddToast(names[i], col::ok, 9000 + i);
            for (int i = 0; i < 4; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); }
            StCheck(g_toastQ.Visible().size() == 3 && g_toastQ.WaitingCount() == 2 && g_toastQ.Visible()[0].text == names[0] && g_toastQ.Visible()[2].text == names[2],
                    L"toast: five operations in quick succession show 3 at once in arrival order and 2 wait their turn (" + std::to_wstring(g_toastQ.Visible().size()) + L" shown, " + std::to_wstring(g_toastQ.WaitingCount()) + L" waiting)");
            RECT cr{}; GetClientRect(h, &cr); const RECT content = ContentRect(); bool inside = g_toastRects.size() == 3, apart = true, clear = true;
            for (size_t i = 0; i < g_toastRects.size(); ++i) {
                const RECT& a = g_toastRects[i];
                inside = inside && a.left >= content.left && a.top >= content.top && a.bottom <= content.bottom && a.right <= cr.right && a.right - content.left <= (content.right - content.left) * 45 / 100 + g_ui.S(24);
                for (size_t j = i + 1; j < g_toastRects.size(); ++j) { RECT x; apart = apart && !IntersectRect(&x, &a, &g_toastRects[j]); }
                clear = clear && a.right < content.right - (content.right - content.left) * 40 / 100;   // the right edge of the rows, where Apply / Hide sit, stays free
            }
            StCheck(inside && apart && clear, L"toast: the stack is inside the content area, does not overlap, keeps a gap, and leaves the right edge of the page (Apply, Hide) uncovered");
            const uint64_t life = std::max({static_cast<uint64_t>(ToastLifetimeMs(names[0])), static_cast<uint64_t>(ToastLifetimeMs(names[1])), static_cast<uint64_t>(ToastLifetimeMs(names[2]))});   // by text length: 4.35-4.62 s here
            StCheck(g_toastQ.Visible().size() == 3 && GetTickCount64() - tAdd < life, L"toast: until their time is up the first three stay (they are not dismissed early)");
            while (GetTickCount64() - tAdd < life + 300) StPump(100);
            StCheck(g_toastQ.Visible().size() == 2 && g_toastQ.WaitingCount() == 0 && g_toastQ.Visible()[0].text == names[3] && g_toastQ.Visible()[1].text == names[4],
                    L"toast: the first three went after their time (" + std::to_wstring(life) + L" ms for the longest) and the two that waited took their place, in order");
            g_toastQ.Clear();
            const std::wstring longText(420, L'x'); AddToast(longText + L" long message that must wrap and stay readable", col::err, 9100); AddToast(L"OK", col::ok, 9101);
            for (int i = 0; i < 3; ++i) { InvalidateRect(h, nullptr, FALSE); UpdateWindow(h); StPump(60); }
            StCheck(g_toastRects.size() == 2 && g_toastRects[0].bottom - g_toastRects[0].top <= 4 * g_ui.S(22) + g_ui.S(22) && g_toastRects[0].bottom - g_toastRects[0].top > g_toastRects[1].bottom - g_toastRects[1].top && g_toastRects[1].bottom - g_toastRects[1].top >= g_ui.S(48) && ToastLifetimeMs(longText) == 9000 && ToastLifetimeMs(L"OK") == 3500,
                    L"toast: a very long message wraps within 4 lines (the box does not grow without limit) and stays 9 s; a short one keeps the minimum box and goes in 3.5 s");
            g_toastQ.Clear();
            for (int i = 0; i < 200; ++i) AddToast(L"flood " + std::to_wstring(i), col::warn, 0);
            StCheck(g_toastQ.Visible().size() <= 3 && g_toastQ.WaitingCount() <= 12, L"toast: 200 toasts in a burst keep the queue bounded (3 shown, at most 12 waiting)");
            g_toastQ.Clear();
            g_cur.scanned = false;
            for (const wchar_t* which : {L"previous", L"defaults"}) {
                g_toastQ.Clear();
                CurRun(which[0] == L'p' ? L"Restore previous cursor scheme" : L"Restore Windows default cursors", [which](std::wstring& err) { return which[0] == L'p' ? CurRestorePrevious(err) : CurRestoreDefaults(err); });
                StCheck(g_toastQ.Visible().size() + g_toastQ.WaitingCount() == 1, std::wstring(L"toast: ") + (which[0] == L'p' ? L"Restore Previous Scheme" : L"Restore Windows Default") + L" raises exactly one notification for the operation");
            }
            g_toastQ.Clear(); ToastSchedule(h); StPump(100);
            StCheck(g_toastQ.Empty() && !g_toastQ.Animating(GetTickCount64()), L"toast: when the queue is empty no timer work is left (nothing animating)");
            NavGo(PDashboard); StPump(100);
        }
        NavGo(PAppsUninstall); StPump(100); SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(150);
        StCheck(g_page == PApps, L"settings pages: Backspace goes back from Uninstall Manager to Apps");
        NavGo(PBackup); StPump(100);
        {   // UPD2-B: opening the updater section starts nothing; in the test build the check and the update are refused before any session exists
            StCheck(!UpdUi().Snapshot().sessionCreated, L"updater: opening Backup & Migration creates no UniGetUI session");
            StCheck(!RunOp(L"Check for updates", [] { return UpdUi().Check(); }), L"updater: the test build refuses the update check");
            UpdListedPackage lp; lp.id = "x.y"; lp.manager = "winget"; lp.version = "1"; lp.newVersion = "2";
            StCheck(!RunOp(L"Update x.y", [lp] { return UpdUi().Run(lp); }), L"updater: the test build refuses an update");
            StCheck(!UpdUi().Snapshot().sessionCreated && !UpdUi().Snapshot().busy, L"updater: ... and still no session, no worker");
        }
        SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(150);
        StCheck(g_page == PRecovery, L"settings pages: Backspace goes back from Backup & Migration to Recovery");
        NavGo(PStorage); StPump(100); SendMessageW(h, WM_KEYDOWN, VK_BACK, 0); StPump(150);
        StCheck(g_page == POptimize, L"settings pages: Backspace goes back from Storage Analyzer to Optimize & Repair");
        {   // WT-03 / WT-02 captures. The SMART rows come from FIXTURES (hand-written smartctl JSON), not from a drive: they show how each state is drawn.
            static const char* kFx[] = {
                R"J({"smartctl":{"exit_status":0},"device":{"name":"/dev/nvme0","type":"nvme","protocol":"NVMe"},"model_name":"Fixture NVMe 1TB","user_capacity":{"bytes":1000204886016},"smart_status":{"passed":true},"nvme_smart_health_information_log":{"critical_warning":0,"temperature":38,"available_spare":100,"available_spare_threshold":10,"percentage_used":3,"data_units_written":8000000,"power_cycles":412,"power_on_hours":2210,"unsafe_shutdowns":12,"media_errors":0}})J",
                R"J({"smartctl":{"exit_status":64},"device":{"name":"/dev/sdb","type":"sat","protocol":"ATA"},"model_name":"Fixture HDD 4TB","user_capacity":{"bytes":4000787030016},"rotation_rate":5400,"smart_status":{"passed":true},"ata_smart_attributes":{"table":[{"id":5,"name":"Reallocated_Sector_Ct","value":196,"worst":196,"thresh":140,"flags":{"prefailure":true},"raw":{"value":24,"string":"24"}},{"id":197,"name":"Current_Pending_Sector","value":199,"worst":199,"thresh":0,"flags":{"prefailure":false},"raw":{"value":8,"string":"8"}}]},"temperature":{"current":74},"power_on_time":{"hours":29000}})J",
                R"J({"smartctl":{"exit_status":8},"device":{"name":"/dev/nvme1","type":"nvme","protocol":"NVMe"},"model_name":"Fixture NVMe (failing)","user_capacity":{"bytes":512110190592},"smart_status":{"passed":false},"nvme_smart_health_information_log":{"critical_warning":4,"temperature":45,"available_spare":5,"available_spare_threshold":10,"percentage_used":105,"media_errors":3,"power_on_hours":40000}})J",
                R"J({"smartctl":{"exit_status":2,"messages":[{"string":"/dev/sdd: Unknown USB bridge [0x1234:0x5678 (0x100)]","severity":"error"}]},"device":{"name":"/dev/sdd","type":"scsi","protocol":"SCSI"}})J"};
            SmartReport fx; fx.done = true; fx.scanned = time(nullptr); fx.smartctlFound = true; fx.smartctlVersion = L"fixture, no real drive";
            for (const char* j : kFx) { SmartDisk d; SmartParseDevice(j, d); fx.disks.push_back(d); }
            StCheck(fx.disks.size() == 4 && fx.disks[0].state == SmartState::Good && fx.disks[1].state == SmartState::Warning && fx.disks[2].state == SmartState::Critical && fx.disks[3].state == SmartState::Unsupported, L"Storage Analyzer SMART fixtures: Good, Warning, Critical and Not supported are all reached");
            StCheck(SmartIssueLine(fx.disks[1].issues.empty() ? SmartIssue{} : fx.disks[1].issues[0]).rfind(L"Advisory", 0) == 0 && SmartIssueLine(fx.disks[2].issues[0]).find(L"reported by the drive") != std::wstring::npos, L"Storage Analyzer SMART: advisory findings are labelled Advisory, drive-reported findings say so");
            Smart().SetReportForTest(fx);
            NavGo(PStorage); g_scroll[PStorage] = g_ui.S(330); StRepaint(h); StPump(300);
            StCheck(StSavePng(h, dir + L"\\storage_smart_fixture_1.png"), L"capture Storage Analyzer (SMART section, fixture states, part 1)");
            g_scroll[PStorage] = g_ui.S(700); StRepaint(h); StPump(300);
            StCheck(StSavePng(h, dir + L"\\storage_smart_fixture_2.png"), L"capture Storage Analyzer (SMART section, fixture states, part 2)");
            g_scroll[PStorage] = 0;
            NavGo(POptimize); g_scroll[POptimize] = g_ui.S(260); StRepaint(h); StPump(300);
            StCheck(StSavePng(h, dir + L"\\optimize_cleanup_section.png"), L"capture Optimize & Repair (Cleanup section, category filter and selected-items count)");
            g_scroll[POptimize] = 0;
            Smart().SetReportForTest(SmartReport{});
            NavGo(PDashboard); StPump(100);
        }
        NavGo(PSbProfiles); StPump(100); SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0); StPump(150);
        StCheck(NavParent(g_page) < 0 && g_page != PDesktop, L"settings pages: the arrow keys leave a sub-page for the next rail page");
        StCheck(StSavePng(h, dir + L"\\page_monitor_readings.png") || true, L"settings pages: walk finished");
    }
    const int page0 = g_page; NavGo(PSbLayout); StPump(300);
                const uint64_t seq0 = g_widgetFrame[1].seq;
                g_scroll[PSbLayout] = 0; StRepaint(h); StPump(1200); StRepaint(h);
                StCheck(g_widgetFrame[1].w > 0 && g_widgetFrame[1].drawn == g_widgetFrame[1].seq && g_widgetFrame[1].seq > seq0, L"sidebar live preview: the settings page draws the Sidebar's latest frame");
                StCheck(StSavePng(h, dir + L"\\sidebar_settings_1.png"), L"capture sidebar settings (preview, profiles, content)");
                NavGo(PSbAppearance); StPump(300); StRepaint(h);
                StCheck(StSavePng(h, dir + L"\\sidebar_settings_2.png"), L"capture sidebar settings (size, appearance)");
                NavGo(page0); StPump(200);
            }
            SbApplyProfile(SbBuiltinProfiles()[0].text); StPump(200);
        }
        {   // behaviour (M-WID1): auto-hide, sliding on and off, dragging and the lock
            auto rc = [] { PerMonitorDpiScope pm; RECT r2{}; GetWindowRect(g_sidebar, &r2); return r2; };
            auto visibleW = [] { RECT b{}; const int k = GetWindowRgnBox(g_sidebar, &b); return k == ERROR || k == NULLREGION ? -1 : static_cast<int>(b.right - b.left); };
            const std::wstring mon0 = Cfg().Str(L"sidebar.monitor", L"");
            SbSetOption(L"sidebar.edge", 1); SbSetOption(L"sidebar.reserve", 0); StPump(300);
            const RECT docked = rc(); const int w = docked.right - docked.left;
            StCheck(g_sbSlide == 0 && EqualRect(&docked, &g_sbBase) && visibleW() == -1, L"sidebar: docked, fully shown and not clipped");
            g_sbPtrTest = true; g_sbPtr = POINT{docked.left - 400, (docked.top + docked.bottom) / 2};   // the pointer is elsewhere
            SbSetOption(L"sidebar.autoHide", 1); StPump(400);
            StCheck(!g_sbHide.hidden && g_sbSlide == 0, L"sidebar auto-hide: stays through the delay after the pointer leaves");
            StPump(1300); RECT hid = rc();
            StCheck(g_sbHide.hidden && g_sbSlide == 1 && hid.left == docked.left + w - g_sbPeek && visibleW() == g_sbPeek,
                    L"sidebar auto-hide: slides off the edge and leaves a " + std::to_wstring(g_sbPeek) + L" px strip, clipped to its own screen");
            g_sbPtr = POINT{docked.right - 1, docked.top + 40}; StPump(700); const RECT back = rc();   // the pointer touches the edge strip
            StCheck(!g_sbHide.hidden && g_sbSlide == 0 && EqualRect(&back, &docked) && visibleW() == -1, L"sidebar auto-hide: returns when the pointer touches the edge");
            g_sbPtr = POINT{docked.left + 20, docked.top + 200}; StPump(1500);
            StCheck(!g_sbHide.hidden, L"sidebar auto-hide: stays while the pointer is on it");
            g_sbPtr = POINT{docked.left - 400, docked.top + 200}; StPump(1500);
            SbSetOption(L"sidebar.autoHide", 0); StPump(500); const RECT off = rc();
            StCheck(!g_sbHide.hidden && g_sbSlide == 0 && EqualRect(&off, &docked), L"sidebar auto-hide: switching it off brings a hidden Sidebar back");
            SbSetOption(L"sidebar.autoHide", 1); SbSetOption(L"sidebar.reserve", 1); StPump(1800);
            StCheck(!g_sbHide.hidden && g_sbSlide == 0, L"sidebar auto-hide: never hides while the edge is reserved");
            SbSetOption(L"sidebar.reserve", 0); SbSetOption(L"sidebar.autoHide", 0); StPump(800);
            g_sbPtrTest = false;
            // switching on and off slides; Exit does not wait
            const HWND sw = g_sidebar;
            ToggleSidebar(false); const bool closing = g_sidebar == sw && g_sbClosing && !g_sidebarOn;
            StPump(600);
            StCheck(closing && !g_sidebar, L"sidebar: switching it off slides it away, then the window is gone");
            ToggleSidebar(true); const bool startsOut = g_sidebar && g_sbSlide == 1;
            StPump(600); const RECT in = rc();
            StCheck(startsOut && g_sbSlide == 0 && EqualRect(&in, &docked), L"sidebar: switching it on slides it in to the docked position");
            const HWND sw2 = g_sidebar; ToggleSidebar(false); StPump(45); ToggleSidebar(true); StPump(600); const RECT again = rc();
            StCheck(g_sidebar == sw2 && !g_sbClosing && g_sidebarOn && g_sbSlide == 0 && EqualRect(&again, &docked), L"sidebar: switched back on while sliding away, it returns (same window)");
            ToggleSidebar(false, false); const bool gone = !g_sidebar; ToggleSidebar(true, false); StPump(200);
            StCheck(gone && g_sidebar && g_sbSlide == 0, L"sidebar: closing at Exit is immediate, with no slide");
            // dragging and the lock
            const LPARAM pt = MAKELPARAM(20, 300);   // not on the Open button
            SbSetOption(L"sidebar.locked", 1); SendMessageW(g_sidebar, WM_LBUTTONDOWN, 0, pt); const bool lockedDrag = g_sbDragging;
            SbSetOption(L"sidebar.locked", 0); SendMessageW(g_sidebar, WM_LBUTTONDOWN, 0, pt); const bool freeDrag = g_sbDragging;
            g_sbDragging = false; ReleaseCapture(); SbApplyAll(); StPump(100);   // cancelled: nothing is dropped
            StCheck(!lockedDrag && freeDrag && GetForegroundWindow() != g_sidebar, L"sidebar: a locked Sidebar ignores a drag; an unlocked one starts it (still without taking focus)");
            SbSetOption(L"sidebar.heightPct", 50); StPump(200);
            const MonitorInfo mi = SbMonitor(g_sb); const int hh = rc().bottom - rc().top;
            SbDropAt(POINT{mi.work.left + 60, mi.work.bottom - 10}, 30); StPump(300); const RECT dl = rc();
            StCheck(g_sb.edge == 0 && dl.left == mi.work.left && dl.bottom == mi.work.bottom && g_sb.posPct == 100 && Cfg().Int(L"sidebar.posPct", -1, -1, 100) == 100,
                    L"sidebar drag: dropped at the bottom left, it docks to the left edge at the bottom, and that is saved");
            SbDropAt(POINT{mi.work.right - 60, mi.work.top + (mi.work.bottom - mi.work.top) / 4 + 30}, 30); StPump(300); const RECT dr = rc();
            StCheck(g_sb.edge == 1 && dr.right == mi.work.right && std::abs(dr.top - (mi.work.top + (mi.work.bottom - mi.work.top) / 4)) <= (mi.work.bottom - mi.work.top - hh) / 100 + 2 && g_sb.posPct > 40 && g_sb.posPct < 60,
                    L"sidebar drag: dropped on the right a quarter down, it docks there (position " + std::to_wstring(g_sb.posPct) + L"%)");
            Cfg().SetInt(L"sidebar.posPct", -1); Cfg().SetStr(L"sidebar.monitor", mon0); SbSetOption(L"sidebar.heightPct", 100); SbSetOption(L"sidebar.locked", 1); StPump(200);
            const RECT end = rc();
            StCheck(EqualRect(&end, &docked) && g_sb.locked && !g_sb.autoHide, L"sidebar: back to the full-height docked position, locked");
        }
    }
    if (g_mini) {   // Mini Monitor engine (2.5)
        auto ex = [] { return GetWindowLongPtrW(g_mini, GWL_EXSTYLE); };
        auto wrect = [] { PerMonitorDpiScope pm; RECT r{}; GetWindowRect(g_mini, &r); return r; };
        auto hit = [] { RECT r = RECT{}; { PerMonitorDpiScope pm; GetWindowRect(g_mini, &r); } return SendMessageW(g_mini, WM_NCHITTEST, 0, MAKELPARAM(r.left + 5, r.top + 5)); };
        const HWND fg = GetForegroundWindow();
        StCheck((ex() & WS_EX_LAYERED) && (ex() & WS_EX_NOACTIVATE) && (ex() & WS_EX_TOOLWINDOW) && SendMessageW(g_mini, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE && fg != g_mini,
                L"mini: layered tool window, never takes focus");
        {   // per-monitor DPI awareness of the window itself
            using GetCtx = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND); using Eq = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            auto gc = reinterpret_cast<GetCtx>(GetProcAddress(u32, "GetWindowDpiAwarenessContext"));
            auto eq = reinterpret_cast<Eq>(GetProcAddress(u32, "AreDpiAwarenessContextsEqual"));
            StCheck(gc && eq && eq(gc(g_mini), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), L"mini: per-monitor DPI aware (v2), independent of the main window");
        }
        const UINT mdpi = [] { PerMonitorDpiScope pm; return MonitorDpi(MonitorFromWindow(g_mini, MONITOR_DEFAULTTONEAREST)); }();
        auto sizeOk = [&](UINT dpi) { const MiniLayout L = MiniLayoutFor(g_miniCfg); const RECT r = wrect();
            return r.right - r.left == MulDiv(L.width, static_cast<int>(dpi), 96) && r.bottom - r.top == MulDiv(L.height, static_cast<int>(dpi), 96); };
        auto sz = [&] { const RECT r = wrect(); return std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top) + L" px"; };
        MiniSetOption(L"mini.metrics", static_cast<int>(kMiniDefaultMetrics)); MiniSetOption(L"mini.graphs", 1); MiniSetOption(L"mini.opacity", 85);
        MiniSetOption(L"mini.layout", 1); StPump(1200);
        StCheck(sizeOk(mdpi), L"mini: expanded layout at monitor DPI " + std::to_wstring(mdpi) + L" (" + sz() + L")");
        StCheck(StSavePng(g_mini, dir + L"\\widget_mini_expanded.png"), L"capture mini (expanded)");
        MiniSetOption(L"mini.layout", 0); StPump(200);
        StCheck(sizeOk(mdpi), L"mini: compact layout (" + sz() + L")");
        StCheck(StSavePng(g_mini, dir + L"\\widget_mini_compact.png"), L"capture mini (compact)");
        MiniSetOption(L"mini.layout", 1);
        MiniSetOption(L"mini.metrics", static_cast<int>(MCpuLoad | MNet | MClock)); StPump(200);
        StCheck(sizeOk(mdpi) && MiniLayoutFor(g_miniCfg).rows.size() == 3, L"mini: metric selection changes the rows (3 metrics, " + sz() + L")");
        MiniSetOption(L"mini.metrics", static_cast<int>(kMiniAllMetrics)); StPump(200);
        StCheck(sizeOk(mdpi) && MiniLayoutFor(g_miniCfg).rows.size() == kMiniMetricCount, L"mini: all eight metrics (" + sz() + L")");
        MiniSetOption(L"mini.opacity", 40); StPump(200);
        StCheck(StSavePng(g_mini, dir + L"\\widget_mini_all_40.png"), L"capture mini (all metrics, 40% background)");
        MiniSetOption(L"mini.metrics", static_cast<int>(kMiniDefaultMetrics)); MiniSetOption(L"mini.opacity", 85); StPump(200);
        {   // WM_DPICHANGED (as sent when it is dragged to a monitor with another scale)
            const UINT hi = mdpi * 3 / 2; RECT cur = wrect();
            SendMessageW(g_mini, WM_DPICHANGED, MAKEWPARAM(hi, hi), reinterpret_cast<LPARAM>(&cur)); StPump(150);
            const bool up = sizeOk(hi) && g_miniUi.dpi == static_cast<int>(hi);
            const std::wstring upSz = sz();
            cur = wrect(); SendMessageW(g_mini, WM_DPICHANGED, MAKEWPARAM(mdpi, mdpi), reinterpret_cast<LPARAM>(&cur)); StPump(150);
            StCheck(up && sizeOk(mdpi), L"mini: WM_DPICHANGED re-lays out at the new DPI (" + upSz + L" at " + std::to_wstring(hi) + L", back to " + sz() + L")");
        }
        // mouse modes: tray, hotkey, settings; the sidebar's mode is independent
        const bool sbCt = g_sb.clickThrough;
        MiniSetOption(L"mini.clickThrough", 0); StPump(100);
        StCheck(!(ex() & WS_EX_TRANSPARENT) && hit() == HTCLIENT, L"mini: interactive takes the mouse (drag, Open button)");
        SendMessageW(h, WM_COMMAND, 105, 0); StPump(100);
        StCheck((ex() & WS_EX_TRANSPARENT) && hit() == HTTRANSPARENT && Cfg().Bool(L"mini.clickThrough", false), L"mini: tray item switches to click-through (WS_EX_TRANSPARENT, HTTRANSPARENT)");
        SendMessageW(g_mini, WM_HOTKEY, kMiniHotkey, 0); StPump(100);
        StCheck(!(ex() & WS_EX_TRANSPARENT) && !g_miniCfg.clickThrough, L"mini: hotkey switches back to interactive");
        {   // the system-wide hotkey has one owner: a running installed AnkhCore with its Mini open legitimately holds it
            const bool heldByInstalled = !g_miniHotkeyOk && FindWindowW(L"AnkhCoreWindow", nullptr);
            StCheck((g_miniHotkeyOk || heldByInstalled) && kMiniHotkey != kSbHotkey, heldByInstalled ? L"mini: Ctrl+Alt+Shift+M held by the running installed AnkhCore (registration failure handled)" : L"mini: Ctrl+Alt+Shift+M registered, separate from the sidebar's Ctrl+Alt+Shift+S");
        }
        StCheck(g_sb.clickThrough == sbCt, L"mini: switching its mode leaves the sidebar's mode alone");
        SendMessageW(g_mini, WM_RBUTTONUP, 0, 0); StPump(100);
        StCheck((ex() & WS_EX_TRANSPARENT) != 0, L"mini: right-click hands the mouse back to the desktop");
        MiniSetOption(L"mini.clickThrough", 0); StPump(100);
        StCheck(GetForegroundWindow() != g_mini, L"mini: still never the foreground window after mode changes");
        // layers
        MiniSetOption(L"mini.layer", 1); StPump(100);
        StCheck((ex() & WS_EX_TOPMOST) == 0, L"mini: normal layer");
        MiniSetOption(L"mini.layer", 0); StPump(100);
        StCheck((ex() & WS_EX_TOPMOST) != 0, L"mini: always-on-top layer");
        // position: a drop is saved, survives a restart, an invalid spot falls back on screen
        const RECT before = wrect();
        const MonitorInfo prim = Monitors().front();
        const int tx = prim.work.left + 40, ty = prim.work.top + 60;
        { PerMonitorDpiScope pm; SetWindowPos(g_mini, nullptr, tx, ty, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
        SendMessageW(g_mini, WM_EXITSIZEMOVE, 0, 0); StPump(100);
        StCheck(Cfg().Int(L"mini.x", INT_MIN, -100000, 100000) == tx && Cfg().Int(L"mini.y", INT_MIN, -100000, 100000) == ty, L"mini: dropped position saved");
        ToggleMini(false, false); StPump(100);   // the exit path
        StCheck(ReadText(Cfg().Path()).find("\"mini.enabled\": true") != std::string::npos, L"mini: closing at exit keeps mini.enabled (reopens next start)");
        Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass")); ToggleMini(true); StPump(300);   // "next start": settings re-read from the file
        RECT re = wrect();
        StCheck(g_mini && re.left == tx && re.top == ty && g_miniCfg.layer == 0 && !g_miniCfg.clickThrough && g_miniCfg.layout == 1,
                L"mini: position, layer, mode and layout restored after a restart");
        Cfg().SetInt(L"mini.x", -60000); Cfg().SetInt(L"mini.y", -60000); Cfg().Save();
        ToggleMini(false, false); ToggleMini(true); StPump(300); re = wrect();
        bool onScreen = false; for (const auto& m : Monitors()) onScreen |= re.left >= m.work.left && re.top >= m.work.top && re.right <= m.work.right && re.bottom <= m.work.bottom;
        StCheck(onScreen && Cfg().Int(L"mini.x", 0, -100000, 100000) == -60000, L"mini: saved spot off every screen -> shown inside a work area, saved spot kept");
        Cfg().SetInt(L"mini.x", before.left); Cfg().SetInt(L"mini.y", before.top); Cfg().Save(); MiniApplyAll();
        const std::string ms = ReadText(Cfg().Path());
        StCheck(ms.find("\"mini.layout\": 1") != std::string::npos && ms.find("\"mini.metrics\": ") != std::string::npos && ms.find("\"mini.layer\": 0") != std::string::npos,
                L"mini settings persisted to settings.json");
        {   // customization (M-WID1): size, scale, corners, glass, colours; independent of the Sidebar
            auto mrc = [] { PerMonitorDpiScope pm; RECT r2{}; GetWindowRect(g_mini, &r2); return r2; };
            const int x0 = Cfg().Int(L"mini.x", 0, -100000, 100000), y0 = Cfg().Int(L"mini.y", 0, -100000, 100000);
            MiniSetOption(L"mini.layout", 1); MiniSetOption(L"mini.scale", 100); MiniSetOption(L"mini.width", 0); MiniSetOption(L"mini.rowHeight", 0); StPump(200);
            const RECT a = mrc(); const int dpi = g_miniUi.dpi, w0 = a.right - a.left, h0 = a.bottom - a.top;
            StCheck(w0 == MulDiv(232, dpi, 96), L"mini: automatic size is the 2.5 layout (" + std::to_wstring(w0) + L" px wide)");
            MiniSetOption(L"mini.width", 300); MiniSetOption(L"mini.rowHeight", 30); StPump(200); const RECT b = mrc();
            StCheck(b.right - b.left == MulDiv(300, dpi, 96) && b.bottom - b.top > h0, L"mini: custom width and line height resize the widget");
            MiniSetOption(L"mini.width", 0); MiniSetOption(L"mini.rowHeight", 0); MiniSetOption(L"mini.scale", 150); StPump(200); const RECT c2 = mrc();
            StCheck(g_miniUi.dpi == MulDiv(dpi, 150, 100) && c2.right - c2.left == MulDiv(232, g_miniUi.dpi, 96) && c2.bottom - c2.top > h0, L"mini: 150% size scales the whole widget");
            MiniSetOption(L"mini.scale", 100); StPump(150);
            MONITORINFO mi{sizeof(mi)}; { PerMonitorDpiScope pm; GetMonitorInfoW(MonitorFromWindow(g_mini, MONITOR_DEFAULTTONEAREST), &mi); }
            const int mg = MulDiv(20, dpi, 96);
            MiniToCorner(0); StPump(150); const RECT tl = mrc();
            MiniToCorner(3); StPump(150); const RECT br = mrc();
            StCheck(tl.left == mi.rcWork.left + mg && tl.top == mi.rcWork.top + mg && br.right == mi.rcWork.right - mg && br.bottom == mi.rcWork.bottom - mg && Cfg().Int(L"mini.x", 0, -100000, 100000) == br.left,
                    L"mini: Move to puts it in a corner of its screen with a margin, and remembers the spot");
            const WidgetFrame& wf = g_widgetFrame[0];
            auto topPx = [&] { return wf.w > 0 && wf.h > 2 ? wf.px[static_cast<size_t>(wf.w / 2)] : 0u; };   // the accent line along the top (row 0)
            auto bgPx = [&] { return wf.w > 0 && wf.h > 8 ? wf.px[static_cast<size_t>(wf.h - 4) * wf.w + wf.w / 2] : 0u; };
            MiniSetOption(L"mini.opacity", 100); MiniSetOption(L"mini.accent", 0xFF5C5C); StPump(200); const uint32_t red = topPx();
            MiniSetOption(L"mini.accent", -1); StPump(200); const uint32_t blue = topPx();
            StCheck(((red >> 16) & 255) > 160 && (red & 255) < 150 && (blue & 255) > 160 && ((blue >> 16) & 255) < 90, L"mini colour: a custom accent reaches the drawn frame, and the theme accent returns");
            MiniSetOption(L"mini.opacity", 50); MiniSetOption(L"mini.background", 0); StPump(250); const uint32_t solid = bgPx();
            MiniSetOption(L"mini.background", 2); MiniSetOption(L"mini.tint", 30); StPump(400); const uint32_t glass = bgPx();
            StCheck(glass != solid && g_sb.background == SbBackgroundCfg() && Cfg().Int(L"sidebar.background", 0, 0, 2) == 0, L"mini background: glass differs from solid, and the Sidebar's own background is untouched");
            MiniSetOption(L"mini.glow", 2); MiniSetOption(L"mini.textColor", 0xF5B041); StPump(200);
            StCheck(StSavePng(g_mini, dir + L"\\widget_mini_custom.png"), L"capture mini, customized");
            const std::string cs = ReadText(Cfg().Path());
            StCheck(cs.find("\"mini.background\": 2") != std::string::npos && cs.find("\"mini.glow\": 2") != std::string::npos && cs.find("\"mini.tint\": 30") != std::string::npos, L"mini customization persisted to settings.json");
            MiniSetOption(L"mini.glow", 0); MiniSetOption(L"mini.textColor", -1); MiniSetOption(L"mini.background", 0); MiniSetOption(L"mini.opacity", 85);
            Cfg().SetInt(L"mini.x", x0); Cfg().SetInt(L"mini.y", y0); Cfg().Save(); MiniApplyAll(); StPump(150);
        }
    }
    {   // Desktop Monitoring: one data source, timestamps, bounded history
        {   // engine lifecycle (D25): a separate process, independent of the UI thread and the main window
            StCheck(g_engine.Running() && g_engine.EnginePid() && g_engine.EnginePid() != GetCurrentProcessId(), L"engine runs as its own process (pid " + std::to_wstring(g_engine.EnginePid()) + L") and the pipe is connected");
            const uint64_t t0 = g_engine.Ticks(); Sleep(3200);   // the UI thread is deliberately blocked here
            const uint64_t during = g_engine.Ticks() - t0;
            StCheck(during >= 2, L"engine keeps delivering while the UI thread is blocked (" + std::to_wstring(during) + L" frames in 3.2 s)");
            StPump(300);
            ShowWindow(h, SW_HIDE);
            const uint64_t s0 = g_snapSeq, p1 = g_engine.Polls(); StPump(3200);
            StCheck(!IsWindowVisible(h) && g_snapSeq > s0 && g_sbSeq == g_snapSeq && g_miniSeq == g_snapSeq, L"main window hidden: telemetry continues and the Sidebar and Mini stay current (frames +" + std::to_wstring(g_snapSeq - s0) + L")");
            const uint64_t dpoll = g_engine.Polls() - p1;
            StCheck(dpoll >= 2 && dpoll <= 5, L"still a single poller with the window hidden (" + std::to_wstring(dpoll) + L" polls in 3.2 s)");
            ShowWindow(h, SW_SHOW); StPump(200);
            StCheck(!Hub().run.load(), L"the UI process runs no sensor poller of its own");
            const uint32_t pidA = g_engine.EnginePid();
            g_engine.TestKillEngine(); StPump(500);
            StCheck(!g_read.present || !g_engine.Running(), L"engine killed: the views stop showing live data (no fabricated values)");
            for (int i = 0; i < 40 && !(g_engine.Running() && g_engine.EnginePid() != pidA); ++i) StPump(250);
            StCheck(g_engine.Running() && g_engine.EnginePid() != pidA, L"engine restarted and reconnected (new pid " + std::to_wstring(g_engine.EnginePid()) + L")");
            const uint64_t t2 = g_engine.Ticks(); StPump(2500);
            StCheck(g_engine.Ticks() > t2, L"frames flow again after the restart");
        }
        {   // GPU utilization: Windows counters per adapter, matched by LUID; never the helper's first-GPU value; one figure everywhere
            for (int i = 0; i < 20 && g_gpus.empty(); ++i) StPump(250);
            size_t hwN = 0; bool named = true, distinct = true, ranged = true;
            for (size_t i = 0; i < g_gpus.size(); ++i) {
                const GpuAdapterView& g = g_gpus[i]; named &= !g.name.empty();
                if (!g.software) { ++hwN; ranged &= g.util >= 0 && g.util <= 100; }
                for (size_t k = 0; k < i; ++k) distinct &= !(g_gpus[k].luid == g.luid);
            }
            StCheck(hwN >= 1 && named && distinct && ranged, L"gpu: " + std::to_wstring(hwN) + L" hardware adapter(s) with a utilization reading arrive in the engine frame, each with its own LUID");
            StCheck(g_usage.gpu >= 0 && g_usage.gpu == GpuHeadline(g_gpus), L"gpu: the single figure is the busiest hardware adapter's (" + NA(g_usage.gpu, L" %") + L")");
            // not stuck: with this window repainting, the reading must take more than one value
            std::set<int> seen; uint64_t lastSeq = 0; bool history = true, mini = true, sidebar = true;
            for (int i = 0; i < 14; ++i) {
                for (int k = 0; k < 6; ++k) { StRepaint(h); StPump(170); }
                if (g_snapSeq == lastSeq) continue;
                lastSeq = g_snapSeq; seen.insert(static_cast<int>(g_usage.gpu * 10 + 0.5));
                bool dim = false;
                history &= g_store.Get(SGpuLoad).Size() && g_store.Get(SGpuLoad).Last() == g_usage.gpu;
                if (g_mini) mini &= MiniValue(MGpuLoad, dim) == NA(g_usage.gpu, L"%");
                if (g_sidebar && g_sbGpuDrawn > -2) sidebar &= g_sbGpuDrawn == g_usage.gpu;
            }
            StCheck(seen.size() >= 2, L"gpu: the reading is live, not stuck (" + std::to_wstring(seen.size()) + L" different values in " + std::to_wstring(static_cast<int>(14)) + L" s of repainting)");
            StCheck(history && mini && sidebar, std::wstring(L"gpu: the history, the Sidebar and the Mini show the same value as the pages, frame by frame") + (history && mini && sidebar ? L"" : std::wstring(L" (differs: ") + (history ? L"" : L"history ") + (mini ? L"" : L"Mini ") + (sidebar ? L"" : L"Sidebar") + L")"));
        }
        {   // network adapters: Windows counters through the engine frame, the three modes, separate per-adapter totals
            for (int i = 0; i < 20 && g_net.empty(); ++i) StPump(250);
            size_t present = 0, defs = 0; for (const auto& a : g_net) { present += a.present ? 1 : 0; defs += a.isDefault ? 1 : 0; }
            StCheck(!g_net.empty() && present > 0, L"network: " + std::to_wstring(present) + L" physical adapter(s) arrive in the engine frame");
            StCheck(defs <= 1, L"network: at most one adapter carries the default route");
        {   // privacy: Wi-Fi signal comes from the RSSI query, which is not location access. With a connected Wi-Fi
            // adapter a reading must arrive whether Windows Location is on or off (the old query was refused when off).
            int wifiUp = 0, wifiSig = 0; std::wstring sig;
            for (const auto& a : g_net) if (a.type == 71 && a.present && a.up) { ++wifiUp; if (a.signal >= 0 && a.signal <= 100) { ++wifiSig; sig = std::to_wstring(a.signal) + L" %"; } }
            StCheck(wifiSig == wifiUp, wifiUp ? L"privacy: Wi-Fi signal (" + sig + L") is read without location access" : std::wstring(L"privacy: no connected Wi-Fi adapter, no signal query to check"));
        }
            NetSetMode(NetAuto, L""); StPump(1200);
            StCheck(g_netSel.mode == NetAuto && (defs ? g_netSel.connected && !g_netSel.guid.empty() : !g_netSel.connected),
                    defs ? L"network: Automatic follows the default-route adapter (" + g_netSel.name + L")" : std::wstring(L"network: Automatic shows no connection when no adapter has a default route"));
            const NetSelection autoSel = g_netSel;
            NetSetMode(NetAll, L""); StPump(1200);
            uint64_t sum = 0; for (const auto& a : g_net) sum += a.totals[3].rx;
            StCheck(g_netSel.mode == NetAll && g_netSel.totals[3].rx == sum && g_netSel.guid.empty(), L"network: All adapters is the sum of the per-adapter totals, added at display time");
            NetSetMode(NetManual, L"{00000000-0000-0000-0000-000000000000}"); StPump(1200);
            StCheck(g_netSel.mode == NetManual && !g_netSel.connected && g_netSel.down < 0 && NetSpeedText() == L"disconnected", L"network: Manual with an absent adapter shows disconnected and never substitutes another");
            if (defs) {
                NetSetMode(NetManual, autoSel.guid); StPump(2300);
                StCheck(g_netSel.connected && g_netSel.guid == autoSel.guid && g_netSel.totals[0].rx >= autoSel.totals[0].rx, L"network: Manual selects one adapter by GUID; its session total only grows");
            }
            const std::string ns = ReadText(Cfg().Path());
            StCheck(ns.find("\"sidebar.netMode\": 2") != std::string::npos && ns.find("\"sidebar.netAdapter\": ") != std::string::npos, L"network: mode and adapter persisted to settings.json");
            NetSetMode(NetAuto, L""); StPump(300);
        }
        const uint64_t p0 = g_engine.Polls(); StPump(5000); const uint64_t dp = g_engine.Polls() - p0;
        StCheck(dp >= 3 && dp <= 7, L"single data source: " + std::to_wstring(dp) + L" polls in 5 s with Sidebar + Mini + pages open (one poller)");
        StCheck(g_sidebar && g_mini && g_snapSeq > 0 && g_sbSeq == g_snapSeq && g_miniSeq == g_snapSeq,
                L"same snapshot: Sidebar and Mini last drew from snapshot #" + std::to_wstring(g_snapSeq) + L" (no view reads sensors itself)");
        {   // background work: drawing the widgets every second must reuse their frame buffers, not create new ones
            const int la0 = g_layerAllocs; const uint64_t sq0 = g_snapSeq; StPump(3200);
            StCheck(g_snapSeq - sq0 >= 2 && g_layerAllocs == la0, L"widget frame buffers are reused: " + std::to_wstring(g_snapSeq - sq0) + L" data frames drawn, " + std::to_wstring(g_layerAllocs - la0) + L" buffers created");
        }
        // idle repaint: nothing animates, so the main window must not repaint at the 33 ms animation rate
        NavGo(PAbout); StPump(800);
        const uint64_t a0 = g_mouseMoves, af0 = g_animFrames;
        uint64_t q0 = g_paints; StPump(3000); const uint64_t idleAbout = g_paints - q0, aboutMoves = g_mouseMoves - a0 + (g_animFrames - af0);
        StCheck(!g_busy && idleAbout <= 2 + aboutMoves, L"idle window does not repaint: " + std::to_wstring(idleAbout) + L" paints in 3 s on About (" + std::to_wstring(g_mouseMoves - a0) + L" mouse moves, " + std::to_wstring(g_animFrames - af0) + L" animation frames; 2.2: ~90)");
        NavGo(PDashboard); StPump(800);
        StGlassProbe(h, dir);
        g_stGallery = true; StRepaint(h); StPump(300); StCheck(StSavePng(h, dir + L"\\gallery.png"), L"component gallery captured"); g_stGallery = false;
        AddToast(L"Toast preview: an operation finished", col::ok); g_dlg.active = true; g_dlg.text = L"Dialog preview. Apply this change?"; StRepaint(h); StPump(350);
        StCheck(StSavePng(h, dir + L"\\dialog_toast.png"), L"dialog and toast captured"); g_dlg.active = false; g_toastQ.Clear(); StRepaint(h);
        {   // ---- unified logo (owner brief 2026-11-14): logo.png with its own transparency at every size it is shown
            auto good = [](const Px& lg, int size) {
                if (lg.p.empty() || lg.w != size || lg.h != size) return false;
                const bool clearCorner = (lg.p[0] >> 24) == 0 && (lg.p.back() >> 24) <= 1;
                const bool solidMiddle = (lg.p[static_cast<size_t>(lg.h / 2) * lg.w + lg.w / 2] >> 24) > 200;   // the ankh keeps full strength
                return clearCorner && solidMiddle;
            };
            NavGo(PDashboard); StPump(300); StRepaint(h);
            StCheck(good(g_brandLogoRail, g_ui.S(84)) && good(g_brandLogoAbout, g_ui.S(112)) && good(g_brandLogoCompact, g_ui.S(40)) && StSavePng(h, dir + L"\\rail_logo.png"),
                    L"unified logo: rail (84), About (112) and compact rail (40) load logo.png, corners transparent, the ankh opaque (no square behind it)");
            NavGo(PAbout); StPump(300); StRepaint(h); StCheck(StSavePng(h, dir + L"\\about_logo.png"), L"capture About with the unified logo");
            NavGo(PDashboard); StPump(200); StRepaint(h);
        }
        {   // ---- SEC-3: no false success. Real engine functions on values under the test build's own registry key; a value outside it is
            //      refused by the test guard, which is the failure these checks need. Nothing outside the test key is written.
            static const std::wstring secKey = std::wstring(kPrefKey) + L"\\Sec3Test";
            static const std::wstring outKey = L"Software\\AnkhCore-Sec3-Outside-Test-Key";   // never created: writes to it are refused
            RegDeleteTreeW(HKEY_CURRENT_USER, secKey.c_str());
            auto retire = [](const std::wstring& groupPrefix) {   // entries that can never be undone here leave the journal's active list
                EnterCriticalSection(&JournalLock()); auto all = JournalLoad();
                for (auto& e : all) if (e.state == L"A" && e.group.rfind(groupPrefix, 0) == 0) e.state = L"R";
                JournalSave(all); LeaveCriticalSection(&JournalLock());
            };
            retire(L"sec3:"); retire(L"tweak:sec3"); retire(L"startup:AnkhSec3");
            const int refused0 = TestRefusedCount();
            // undo: two real entries and one that cannot be undone
            JRegDword(L"sec3:mixed", L"SEC-3 probe A", HKEY_CURRENT_USER, secKey, L"A", 1);
            JRegDword(L"sec3:mixed", L"SEC-3 probe B", HKEY_CURRENT_USER, secKey, L"B", 2);
            JournalAdd(L"sec3:mixed", L"REG", L"SEC-3 probe outside the test key", {L"HKCU", outKey, L"X", L"1", std::to_wstring(REG_DWORD), L"01000000"});
            const UndoCount mixed = JournalUndoEx([](const JEntry& e) { return e.group == L"sec3:mixed"; });
            int stillActive = 0; for (const auto& e : JournalLoad()) stillActive += e.group == L"sec3:mixed" && e.state == L"A";
            StCheck(mixed.picked == 3 && mixed.undone == 2 && !mixed.Success() && stillActive == 1 && !RegGet(HKEY_CURRENT_USER, secKey, L"A").exists && !RegGet(HKEY_CURRENT_USER, secKey, L"B").exists,
                    L"no false success: undo of 3 changes where 1 cannot be undone reports 2 of 3, and the failed one stays recorded");
            StCheck(!UndoOp(L"Self-test", [](const JEntry& e) { return e.group == L"sec3:mixed"; }), L"no false success: Undo reports failure while a selected change still cannot be undone");
            retire(L"sec3:");
            JRegDword(L"sec3:good", L"SEC-3 probe C", HKEY_CURRENT_USER, secKey, L"C", 3);
            StCheck(UndoOp(L"Self-test", [](const JEntry& e) { return e.group == L"sec3:good"; }) && !RegGet(HKEY_CURRENT_USER, secKey, L"C").exists, L"no false success: Undo reports success when every selected change was put back");
            StCheck(UndoOp(L"Self-test", [](const JEntry& e) { return e.group == L"sec3:none"; }) && !UndoOp(L"Self-test", [](const JEntry& e) { return e.group == L"sec3:none"; }, true),
                    L"no false success: nothing to undo is success for an Undo all button and failure for one named entry");
            // registry tweak: written, read back, undone; refused; not applied by AnkhCore
            const Tweak tw = RegTweak(L"sec3", L"SEC-3 probe tweak", L"", {{HKEY_CURRENT_USER, secKey.c_str(), L"T1", REG_DWORD, 7, nullptr}, {HKEY_CURRENT_USER, secKey.c_str(), L"T2", REG_SZ, 0, L"on"}});
            const bool twOn = tw.on(), twIsOn = tw.isOn(), twOff = tw.off(), twIsOff = !tw.isOn(), twOffAgain = tw.off();
            StCheck(twOn && twIsOn && twOff && twIsOff && !twOffAgain, L"no false success: a tweak is on only after its values read back, off only after they are put back, and undoing a tweak AnkhCore did not apply fails");
            const Tweak twBad = RegTweak(L"sec3bad", L"SEC-3 probe tweak (refused)", L"", {{HKEY_CURRENT_USER, secKey.c_str(), L"T3", REG_DWORD, 1, nullptr}, {HKEY_CURRENT_USER, outKey.c_str(), L"T4", REG_DWORD, 1, nullptr}});
            const bool badOn = twBad.on(), badIsOn = twBad.isOn();
            StCheck(!badOn && !badIsOn, L"no false success: a tweak with one value that cannot be written reports failure");
            retire(L"tweak:sec3bad"); RegDel(HKEY_CURRENT_USER, secKey, L"T3");
            // startup app switch: read back; refused
            StartupItem si; si.name = L"AnkhSec3Probe"; si.approvedRoot = HKEY_CURRENT_USER; si.approvedKey = secKey;
            const bool sOff = SetStartupEnabled(si, false), sIsOff = !ApprovedEnabled(HKEY_CURRENT_USER, secKey, si.name), sOn = SetStartupEnabled(si, true), sIsOn = ApprovedEnabled(HKEY_CURRENT_USER, secKey, si.name);
            StartupItem sx = si; sx.approvedKey = outKey;
            const bool sBad = SetStartupEnabled(sx, false);
            StCheck(sOff && sIsOff && sOn && sIsOn && !sBad, L"no false success: a startup app switch succeeds only when the setting reads back; a refused write reports failure");
            retire(L"startup:AnkhSec3");
            StCheck(TestRefusedCount() > refused0 && !RegKeyExists(HKEY_CURRENT_USER, outKey), L"no false success: the writes outside the test key were refused (" + std::to_wstring(TestRefusedCount() - refused0) + L"), and that key does not exist");
            RegDeleteTreeW(HKEY_CURRENT_USER, secKey.c_str());
        }
        {   // ---- LOG-2: the footer's progress bar. No operation runs here: the busy state is set by hand, drawn, captured and cleared.
            const std::wstring keepName = g_opName; const int keepProg = g_progress; const int keepResult = g_lastResult;
            g_busy = true; g_opName = L"Self-test: operation of unknown length"; g_progress = kProgressUnknown; SetOpLine(L"an external program is running"); StRepaint(h); StPump(150);
            const bool capUnknown = StSavePng(h, dir + L"\\progress_unknown.png");
            g_opName = L"Self-test: counted steps"; g_progress = OpSteps(5).Pct(); { OpSteps s5(5); s5.Next(); s5.Next(); s5.Next(); g_progress = s5.Pct(); SetOpLine(s5.Label(L"third step")); } StRepaint(h); StPump(150);
            const bool capSteps = StSavePng(h, dir + L"\\progress_steps.png");
            StCheck(capUnknown && capSteps && g_progress == 40, L"progress (LOG-2): footer captured for unknown length (travelling band) and for step 3 of 5 (40 %)");
            LineFn lf = OpLines(); g_progress = kProgressUnknown;
            lf(L"Beginning verification phase of system scan."); const int a0 = g_progress;
            lf(L"Verification 45% complete."); const int a1 = g_progress;
            lf(L"Windows Resource Protection did not find any integrity violations."); const int a2 = g_progress;
            lf(L"Stage: 13%; Step: 4%"); const int a3 = g_progress;
            lf(L"Progress: 12 of 90 done; Stage: 13%; Total:  4%; ETA:   0:00:12"); const int a4 = g_progress;
            StCheck(a0 == kProgressUnknown && a1 == 45 && a2 == 45 && a3 == 45 && a4 == 4, L"progress (LOG-2): tool output sets the bar only from the tool's own percentage; text without one, or an ambiguous line, changes nothing");
            g_busy = false; g_opName = keepName; g_progress = keepProg; g_lastResult = keepResult; SetOpLine(L""); StRepaint(h);
        }
        {   // ---- APP-4: Cursor Manager. The engine works on the test build's sandbox copy of the cursor settings; this PC's real key is read before and after.
            const std::wstring realSchemes = CurRealKey() + L"\\Schemes", sandboxRoot = std::wstring(kPrefKey) + L"\\CursorSandbox";
            const CurState realBefore = CurReadStateFrom(CurRealKey());
            const auto realNamesBefore = CurRegValueNames(HKEY_CURRENT_USER, realSchemes);
            RegDeleteTreeW(HKEY_CURRENT_USER, sandboxRoot.c_str()); DeleteFileW(CurPrevPath().c_str()); DeleteFileW(CurPendingPath().c_str());
            StCheck(CurKey() != CurRealKey() && TestRegAllowed(HKEY_CURRENT_USER, CurKey()) && !TestRegAllowed(HKEY_CURRENT_USER, CurRealKey()), L"cursor manager: the test build's cursor key is its own sandbox; the real key is refused");
            const CurState seeded = CurReadState();
            StCheck(seeded == realBefore && CurRegValueNames(HKEY_CURRENT_USER, CurSchemesKey()) == realNamesBefore, L"cursor manager: the sandbox starts as a copy of this PC's cursor settings (scheme \"" + seeded.name + L"\", " + std::to_wstring(realNamesBefore.size()) + L" saved schemes)");
            // fixture packs in the test data folder: plain 32x32 test cursors made here (no pack file of the owner is copied or changed)
            const std::wstring fx = AcbDefaultRoots().content + L"\\Cursors";
            std::vector<uint8_t> curFile(22 + 40 + 32 * 32 * 4 + 32 * 4, 0);
            {
                uint8_t* p = curFile.data(); p[2] = 2; p[4] = 1; p[6] = 32; p[7] = 32;
                const uint32_t bytes = static_cast<uint32_t>(curFile.size() - 22), off = 22; memcpy(p + 14, &bytes, 4); memcpy(p + 18, &off, 4);
                const uint32_t bi[10] = {40, 32, 64, 1u | (32u << 16), 0, 32 * 32 * 4, 0, 0, 0, 0}; memcpy(p + 22, bi, 40);
                for (int y = 0; y < 32; ++y) for (int x = 0; x <= (31 - y) / 2; ++x) memset(p + 62 + ((31 - y) * 32 + x) * 4, 0xFF, 4);   // a white wedge, so previews show something
            }
            auto put = [&](const std::wstring& rel, bool valid) { EnsureDir(DirOf(fx + L"\\" + rel)); return valid ? WriteAll(fx + L"\\" + rel, curFile.data(), curFile.size()) : WriteAll(fx + L"\\" + rel, reinterpret_cast<const uint8_t*>("not a cursor file, on purpose"), 29); };
            bool made = true;
            for (const auto& ri : CurRoleTable()) made &= put(std::wstring(L"ST Full\\[ST] ") + ri.label + L" v1.0.cur", true);
            made &= put(L"ST Full\\Extra\\[ST] Busy v1.0 - Alt.cur", true);
            made &= put(L"ST Partial\\Arrow.cur", true) && put(L"ST Partial\\Wait.cur", true) && put(L"ST Partial\\button.cur", true);
            made &= put(L"ST Broken\\Arrow.cur", false) && put(L"ST Broken\\Wait.cur", true);
            const auto fxFound = CurDiscoverFolder(fx);
            auto byName = [&](const wchar_t* n) -> const CurScheme* { for (const auto& sc : fxFound) if (sc.name == n) return &sc; return nullptr; };
            const CurScheme* full = byName(L"ST Full"); const CurScheme* part = byName(L"ST Partial"); const CurScheme* broken = byName(L"ST Broken");
            StCheck(made && full && part && broken && full->Mapped() == 17 && full->alternates.size() == 1 && full->invalid.empty() && full->tags.size() == 1 && full->tags[0] == L"ST" && !full->premium &&
                    part->Mapped() == 2 && part->unassigned.size() == 1 && part->unassigned[0] == L"button.cur" && broken->Mapped() == 2 && broken->invalid.size() == 1,
                    L"cursor manager: discovery maps the three test packs (17 of 17 with one alternate; 2 of 17 with one unrecognised file; one invalid file found)");
            if (full && part && broken) {
                {
                    const auto all = CurDiscover(CurRoots()); int folders = 0, reg = 0, prem = 0, complete = 0, bad = 0;
                    for (const auto& sc : all) { (sc.registered ? reg : folders)++; prem += sc.premium; complete += sc.Mapped() == 17; bad += !sc.invalid.empty(); }
                    g_stReport.push_back(L"INFO  cursor packs on this PC incl. the 3 test packs: " + std::to_wstring(folders) + L" folders, " + std::to_wstring(reg) + L" saved in Windows; " + std::to_wstring(complete) + L" fill all 17 roles, " +
                                         std::to_wstring(prem) + L" premium, " + std::to_wstring(bad) + L" with invalid files");
                }
                const CurState fullT = CurTargetState(*full, WinDir()), partT = CurTargetState(*part, WinDir());
                std::wstring err; CurState prev;
                const bool a1 = CurApply(*full, err);
                bool journaled = false; for (const auto& e : JournalLoad()) journaled |= e.group == L"cursors" && e.f.size() >= 3 && e.f[1] == CurKey() && e.f[2] == L"Arrow";
                StCheck(a1 && CurReadState() == fullT && CurLoadPrevious(prev) && prev == seeded && !FileExists(CurPendingPath()) && CurParseSchemeList(RegStr(HKEY_CURRENT_USER, CurSchemesKey(), L"ST Full")) == fullT.value && journaled,
                        L"cursor manager: apply writes all 17 roles, the scheme name and its list, saves the scheme that was active, and records undo entries" + (err.empty() ? L"" : L" (" + err + L")"));
                const bool a2 = CurApply(*part, err);
                const CurState afterPart = CurReadState();
                StCheck(a2 && afterPart == partT && afterPart.value[0] == part->dir + L"\\Arrow.cur" && afterPart.value[1] == L"%SystemRoot%\\cursors\\aero_helpsel.cur" && afterPart.exists[4] && afterPart.value[4].empty() &&
                        CurLoadPrevious(prev) && prev == fullT, L"cursor manager: a pack with 2 of 17 roles applies; the 15 it lacks get the Windows cursors, none keeps the old pack's file");
                const bool a3 = CurApply(*broken, err);
                StCheck(!a3 && !err.empty() && CurReadState() == partT && CurLoadPrevious(prev) && prev == fullT, L"cursor manager: a pack with an invalid file is refused and nothing changes (" + err + L")");
                CurTestFailAfter() = 6; const bool a4 = CurApply(*full, err); CurTestFailAfter() = -1;
                StCheck(!a4 && CurReadState() == partT && CurLoadPrevious(prev) && prev == fullT && !FileExists(CurPendingPath()), L"cursor manager: a write that fails after 6 roles is rolled back to the exact previous state, and the saved previous scheme is kept (" + err + L")");
                const bool r1 = CurRestorePrevious(err);
                StCheck(r1 && CurReadState() == fullT && CurLoadPrevious(prev) && prev == partT, L"cursor manager: Restore previous scheme brings the earlier scheme back and keeps the replaced one as the new previous");
                const bool r2 = CurRestoreDefaults(err);
                StCheck(r2 && CurReadState() == CurWindowsDefault() && CurLoadPrevious(prev) && prev == fullT, L"cursor manager: Restore Windows default writes the default scheme and saves the replaced one");
                WriteAll(CurPrevPath(), reinterpret_cast<const uint8_t*>("ankhcore-cursor-scheme 1\nname=x\n"), 32);
                const bool r3 = CurRestorePrevious(err);
                StCheck(!r3 && CurReadState() == CurWindowsDefault(), L"cursor manager: a damaged saved scheme is refused and nothing changes (" + err + L")");
                DeleteFileW(CurPrevPath().c_str());
                // the page
                g_cur.sel = -1; CurRefresh(true); NavGo(PAppCursor); StPump(500); g_scroll[PAppCursor] = 0; StRepaint(h);
                int fullIdx = -1; for (size_t i = 0; i < g_cur.schemes.size(); ++i) if (g_cur.schemes[i].name == L"ST Full" && !g_cur.schemes[i].registered) fullIdx = static_cast<int>(i);
                StCheck(g_page == PAppCursor && fullIdx >= 0 && g_cur.active == -1 && Lower(g_cur.now.name) == L"windows default", L"cursor manager page: lists the packs (" + std::to_wstring(g_cur.schemes.size()) + L") and shows Windows default as active");
                StCheck(CurPreview(CurRolePath(*full, 0)) != nullptr && CurPreview(fx + L"\\ST Broken\\Arrow.cur") == nullptr, L"cursor manager page: a valid cursor file loads for the preview; an invalid one is not loaded");
                auto recWithCursors = [] { int n = 0; for (const auto& rp : AcbListRecoveryPoints(AcbDefaultRoots())) n += FileExists(rp.dir + L"\\files\\cursors\\scheme-before.txt"); return n; };
                const int recBefore = recWithCursors();
                StPump(1100);   // recovery folders are named by the second: this one must be new
                Cfg().SetInt(L"backup.autoBeforeChanges", 0);
                const CurScheme pick = g_cur.schemes[fullIdx];
                CurRun(L"Apply cursor scheme ST Full", [pick](std::wstring& e) { return CurApply(pick, e); });
                CurRefresh(false);
                StCheck(g_cur.active == fullIdx && g_cur.hasPrev && recWithCursors() == recBefore + 1, L"cursor manager page: Apply marks the pack Active, enables Restore previous scheme, and the recovery point holds the cursor settings from before");
                g_cur.sel = fullIdx; StRepaint(h); StPump(200);
                StCheck(StSavePng(h, dir + L"\\cursor_manager.png"), L"capture Cursor Manager (active scheme and roles)");
                int dupes = 0; for (size_t i = 0; i < g_cur.schemes.size(); ++i) dupes += g_cur.schemes[i].registered && g_cur.schemes[i].name == L"ST Full";
                StCheck(dupes == 0, L"cursor manager page: the applied pack is not listed a second time as a scheme saved in Windows");
                g_cur.sel = 0; g_scroll[PAppCursor] = g_ui.S(430); StRepaint(h); StPump(200);   // the first pack on this PC, whatever it is
                StCheck(StSavePng(h, dir + L"\\cursor_manager_preview.png"), L"capture Cursor Manager (pack preview)");
                g_scroll[PAppCursor] = 0; g_cur.sel = -1;
            }
            StCheck(CurReadStateFrom(CurRealKey()) == realBefore && CurRegValueNames(HKEY_CURRENT_USER, realSchemes) == realNamesBefore,
                    L"cursor manager: this PC's real cursor settings and saved schemes are exactly as before (scheme \"" + realBefore.name + L"\")");
            RegDeleteTreeW(HKEY_CURRENT_USER, sandboxRoot.c_str()); DeleteFileW(CurPrevPath().c_str());
            g_cur.scanned = false; g_toastQ.Clear(); NavGo(PDashboard); StPump(300); StRepaint(h);
        }
        {   // ---- NAV-2: the compact rail by real clicks, its tooltips, keyboard focus, and rail scrolling
            auto clickAt = [&](const RECT& r) { SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2)); StPump(150); StRepaint(h); };
            auto hover = [&](const RECT& r) { g_ui.mouse = POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2}; StRepaint(h); return g_navTipShown; };
            auto rowOf = [&](int page, bool child, RECT& out) { for (const NavRow& r : NavLayout()) if (r.page == page && r.child == child) { out = r.r; return true; } return false; };
            auto savedCompact = [&](bool on) { return ReadText(Cfg().Path()).find(on ? "\"nav.compact\": true" : "\"nav.compact\": false") != std::string::npos; };
            auto tipInside = [&] { return g_navTipRect.left >= SideW() && g_navTipRect.right <= ClientW() && g_navTipRect.top >= 0 && g_navTipRect.bottom <= ClientH() && g_navTipRect.right > g_navTipRect.left; };
            const std::wstring dotSep = L"  \x00B7  ";
            NavGo(PDashboard); StPump(400); g_ui.mouse = POINT{-1, -1}; g_navKeyboard = false; StRepaint(h);
            RECT rr{};
            StCheck(!g_navCompact && rowOf(PAbout, false, rr) && hover(rr).empty(), L"full rail: hovering an item shows no tooltip (its name is already there)");
            clickAt(g_navToggleRect);
            StCheck(g_navCompact && savedCompact(true), L"compact rail: a click on the rail switch turns it on and saves nav.compact");
            g_ui.mouse = POINT{-1, -1}; StRepaint(h); StPump(200);
            StCheck(StSavePng(h, dir + L"\\nav_compact.png") && ContentRect().left == g_ui.S(64), L"compact rail: content reflows to the 64 DIP rail");
            StCheck(g_navTipShown.empty(), L"compact rail: no tooltip while nothing is hovered or keyboard-focused");
            size_t named = 0; bool inside = true;
            for (const NavSection& sc : kNav) if (rowOf(sc.pages[0], false, rr) && hover(rr) == sc.name) { ++named; inside &= tipInside(); }
            StCheck(named == kNav.size() && inside, L"compact rail: hovering each of the " + std::to_wstring(kNav.size()) + L" section icons shows its name beside the rail (" + std::to_wstring(named) + L" named)");
            // no message pump between the hover and the capture: a real mouse move in between would replace the simulated position
            rowOf(PMonitor, false, rr); hover(rr); StCheck(g_navTipShown == kNav[NavSectionOf(PMonitor)].name && StSavePng(h, dir + L"\\nav_compact_tip.png"), L"capture compact rail tooltip");
            NavGo(PFan); StPump(500); StRepaint(h);
            StCheck(rowOf(PFan, true, rr) && hover(rr) == NavChildName(PFan) && tipInside(), L"compact rail: hovering a child dot shows the page name");
            const std::wstring statusTip = hover(g_navStatusRect);
            StCheck(statusTip.rfind(kPageName[PDesktop], 0) == 0 && statusTip.size() > wcslen(kPageName[PDesktop]) + dotSep.size() && tipInside(), L"compact rail: the status dot's tooltip names Desktop Monitoring and the live state (" + statusTip + L")");
            StCheck(hover(g_navToggleRect) == L"Expand navigation" && tipInside(), L"compact rail: the expand button has a tooltip");
            {
                int onSwitch = 0, onStatus = 0; RECT x{};
                for (const auto& ht : g_ui.hits) { onSwitch += IntersectRect(&x, &ht.r, &g_navToggleRect) != 0; onStatus += IntersectRect(&x, &ht.r, &g_navStatusRect) != 0; }
                StCheck(onSwitch == 1 && onStatus == 1 && NavTop() + NavViewH() <= g_navToggleRect.top, L"compact rail: with a section open, no item is drawn or clickable over the expand button or the status dot");
            }
            g_ui.mouse = POINT{-1, -1}; NavGo(PDashboard); StPump(500);
            SendMessageW(h, WM_KEYDOWN, VK_DOWN, 0); StPump(500); StRepaint(h);
            StCheck(g_page == PMonitor && g_navTipShown == NavChildName(PMonitor) && tipInside(), L"compact rail: the keyboard-focused page shows its name (" + g_navTipShown + L")");
            SendMessageW(h, WM_KEYDOWN, VK_LEFT, 0); StPump(500); StRepaint(h);
            StCheck(g_navOpen == -1 && g_navTipShown == kNav[NavSectionOf(PMonitor)].name + dotSep + NavChildName(PMonitor), L"compact rail: with its section closed, the focus moves to the section icon and names section and page (" + g_navTipShown + L")");
            StCheck(StSavePng(h, dir + L"\\nav_compact_focus.png"), L"capture compact rail keyboard focus");
            // rail scrolling at the smallest window: before every key press the rail is wheeled to one end, so the row must be brought back
            RECT wr{}; GetWindowRect(h, &wr);
            SetWindowPos(h, nullptr, 0, 0, g_ui.S(1040), g_ui.S(680), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); StPump(200); StRepaint(h);
            auto wheel = [&](bool down) { POINT sp{g_ui.S(20), NavTop() + g_ui.S(20)}; ClientToScreen(h, &sp); SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(down ? -WHEEL_DELTA * 20 : WHEEL_DELTA * 20))), MAKELPARAM(sp.x, sp.y)); };
            auto walk = [&](int& maxScroll) {   // every page down and back up; returns the steps that left the selected row outside the rail
                int bad = 0; const auto order = NavOrder(); const int n = static_cast<int>(order.size());
                NavGo(order.front()); StPump(300);
                for (int k = 0; k < 2 * n; ++k) {
                    wheel(k % 2 == 0); maxScroll = std::max(maxScroll, g_navScroll);
                    SendMessageW(h, WM_KEYDOWN, k < n ? VK_DOWN : VK_UP, 0); StPump(350); StRepaint(h);
                    RECT r2{};
                    if (!rowOf(g_page, kNav[NavSectionOf(g_page)].pages.size() > 1, r2) || r2.top < NavTop() || r2.bottom > NavTop() + NavViewH()) ++bad;
                }
                return bad;
            };
            int scC = 0; const int badC = walk(scC);
            StCheck(badC == 0 && scC > 0, L"compact rail, window " + std::to_wstring(ClientW()) + L"x" + std::to_wstring(ClientH()) + L": the keyboard-selected item is always fully inside the rail (" + std::to_wstring(badC) + L" outside; rail scrolled up to " + std::to_wstring(scC) + L" px)");
            g_navKeyboard = false; StRepaint(h); clickAt(g_navToggleRect);
            StCheck(!g_navCompact && savedCompact(false) && ContentRect().left == g_ui.S(kSide), L"compact rail: a click on the expand button restores the full rail and saves nav.compact");
            int scF = 0; const int badF = walk(scF);
            StCheck(badF == 0 && scF > 0, L"full rail, window " + std::to_wstring(ClientW()) + L"x" + std::to_wstring(ClientH()) + L": the keyboard-selected item is always fully inside the rail (" + std::to_wstring(badF) + L" outside; rail scrolled up to " + std::to_wstring(scF) + L" px)");
            NavGo(PFan); StPump(500); wheel(true); StRepaint(h);
            const int atSmall = g_navScroll;
            SetWindowPos(h, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); StPump(200); StRepaint(h);
            const int roomBig = NavScrollMax(NavContentHeight(true), NavViewH());
            StCheck(atSmall > roomBig && g_navScroll == roomBig, L"rail scroll: a taller window pulls the offset back into range (" + std::to_wstring(atSmall) + L" -> " + std::to_wstring(g_navScroll) + L" px)");
            wheel(true); StRepaint(h); const int beforeClose = g_navScroll;
            NavGo(PAbout); StPump(500); StRepaint(h);
            StCheck(beforeClose > 0 && g_navScroll == 0 && rowOf(PDashboard, false, rr) && rr.top == NavTop(), L"rail scroll: closing the open section leaves no stale offset (" + std::to_wstring(beforeClose) + L" -> " + std::to_wstring(g_navScroll) + L" px)");
            NavGo(PDashboard); StPump(400); g_navKeyboard = false; g_ui.mouse = POINT{-1, -1}; StRepaint(h);
        }
        {   // 2.5 rail pages: Hardware Information and Network Monitor (under Monitor), Error Logs (under Activity)
            const int secMon = NavSectionOf(PMonitor), secAct = NavSectionOf(PActivity); const auto order = NavOrder();
            StCheck(NavSectionOf(PHardware) == secMon && NavSectionOf(PNetwork) == secMon && NavSectionOf(PErrLog) == secAct && NavParent(PHardware) < 0 && NavParent(PNetwork) < 0 && NavParent(PErrLog) < 0 &&
                    kNav[secMon].pages.size() == 5 && kNav[secAct].pages.size() == 2,
                    L"rail pages: Hardware Information and Network Monitor belong to Monitor, Error Logs to Activity; none is a sub-page, so each is reached from the rail (no Back button) and the rail still has one open section at a time");
            StCheck(std::find(order.begin(), order.end(), PHardware) != order.end() && std::find(order.begin(), order.end(), PNetwork) != order.end() && std::find(order.begin(), order.end(), PErrLog) != order.end(),
                    L"rail pages: the Up/Down keys reach all three");
            for (int pg : {PHardware, PNetwork, PErrLog}) {
                NavGo(pg); g_scroll[pg] = 0; g_ui.mouse = POINT{-1, -1}; StRepaint(h); StPump(600); StRepaint(h);
                StCheck(g_page == pg && g_navOpen == NavSectionOf(pg) && g_contentH[pg] > g_ui.S(150), std::wstring(L"page ") + kPageName[pg] + L": opens from the rail, the section opens, the content draws (" + std::to_wstring(g_contentH[pg]) + L" px)");
            }
            const std::vector<HwRow> hw = HwRows(Snapshot()); std::set<std::wstring> secs; for (const auto& r : hw) secs.insert(r.section);
            const std::wstring hwText = HwSummaryText(hw);
            StCheck(secs.count(L"System") && secs.count(L"Processor") && secs.count(L"Graphics") && secs.count(L"Memory") && secs.count(L"Storage") && secs.count(L"Network adapters") && secs.count(L"Fans") &&
                    hwText.find(L"== Processor ==") != std::wstring::npos && hwText.find(L"Operating system: ") != std::wstring::npos,
                    L"Hardware Information: every section is present with rows, also while a value is missing (" + std::to_wstring(hw.size()) + L" rows)");
            g_errLog.todayOnly = true; g_errLog.errorsOnly = false;
            LogW(L"selftest rail-page warning probe"); LogE(L"selftest rail-page error probe");
            for (int i = 0; i < 60 && g_errLog.busy.load(); ++i) StPump(100);
            g_errLog.Refresh();
            for (int i = 0; i < 80; ++i) { { std::lock_guard<std::mutex> lk(g_errLog.mx); if (g_errLog.loaded && !g_errLog.loading) break; } StPump(100); }
            {
                std::lock_guard<std::mutex> lk(g_errLog.mx); const std::wstring today = NowText(true).substr(0, 10);
                const auto both = FilterLogEntries(g_errLog.all, false, today, 300), errs = FilterLogEntries(g_errLog.all, true, today, 300);
                auto has = [](const std::vector<ErrLogEntry>& v, const wchar_t* t) { for (const auto& e : v) if (e.text.find(t) != std::wstring::npos) return true; return false; };
                StCheck(g_errLog.ok && g_errLog.loaded && has(both, L"rail-page warning probe") && has(both, L"rail-page error probe") && has(errs, L"rail-page error probe") && !has(errs, L"rail-page warning probe"),
                        L"Error Logs: a warning and an error written now are read back from the log file; Errors only keeps the error and drops the warning (" + std::to_wstring(both.size()) + L" of today's warnings/errors)");
            }
            NavGo(PErrLog); StRepaint(h); StPump(300); StRepaint(h);
            StCheck(StSavePng(h, dir + L"\\page_error_logs.png"), L"Error Logs page captured");
            NavGo(PHardware); StRepaint(h); StPump(300); StRepaint(h); StCheck(StSavePng(h, dir + L"\\page_hardware.png"), L"Hardware Information page captured");
            NavGo(PNetwork); StRepaint(h); StPump(300); StRepaint(h); StCheck(StSavePng(h, dir + L"\\page_network.png"), L"Network Monitor page captured");
            NavGo(PDashboard); StPump(300); StRepaint(h);
        }
        {   // page scrollbar + animation identity (regression: the bar only drew; scrolling re-targeted other controls' toggle animations)
            for (int pg : {PTweaks, PAppearance, PActivity}) {
                NavGo(pg); g_scroll[pg] = 0; g_ui.mouse = POINT{-1, -1}; StRepaint(h); StPump(700); StRepaint(h);
                RECT th0; int vh, tl, rg; const bool has = PageSbGeom(th0, vh, tl, rg);
                if (!has) { StCheck(true, L"page scrollbar: page " + std::to_wstring(pg) + L" fits the window (no bar to drag)"); continue; }
                POINT sp{ContentRect().left + g_ui.S(40), ContentRect().top + g_ui.S(40)}; ClientToScreen(h, &sp);
                for (int i = 0; i < 4; ++i) { SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(-WHEEL_DELTA))), MAKELPARAM(sp.x, sp.y)); StRepaint(h); }
                const bool calm = !g_anim.Active();
                StCheck(g_scroll[pg] > 0 && calm, L"wheel scroll on page " + std::to_wstring(pg) + L" starts no toggle animation (scroll " + std::to_wstring(g_scroll[pg]) + L" px, animation " + (calm ? L"idle)" : L"running)"));
                g_scroll[pg] = 0; StRepaint(h); StRepaint(h);
                PageSbGeom(th0, vh, tl, rg);
                const int gx = th0.left + g_ui.S(6), gy = th0.top + tl / 2;
                SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(gx, gy));
                const bool grabbed = g_pageSbDrag;
                SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(gx, gy + rg / 2)); StRepaint(h);
                const int mid = g_scroll[pg];
                SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(gx, gy + rg * 3)); StRepaint(h);   // far past the end: clamps
                const int maxS = g_contentH[pg] - vh;
                const int atEnd = g_scroll[pg];
                SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(gx, gy));
                StCheck(grabbed && mid > 0 && mid < maxS && atEnd == maxS && !g_pageSbDrag, L"scrollbar drag on page " + std::to_wstring(pg) + L": grab " + (grabbed ? L"ok" : L"FAILED") + L", half-way " + std::to_wstring(mid) + L" of " + std::to_wstring(maxS) + L", clamped end " + std::to_wstring(atEnd) + L", released");
                g_scroll[pg] = 0;
            }
            NavGo(PDashboard); StPump(400); StRepaint(h);
        }
        {
            UINT32 miss = 0; std::vector<UINT32> cps; for (const wchar_t* g : kPageIcon) cps.push_back(g[0]); for (UINT32 x : {0xE921u, 0xE922u, 0xE923u, 0xE8BBu, 0xE76Bu, 0xE76Cu, 0xE70Du, 0xE7BAu, 0xE897u}) cps.push_back(x);
            const bool ok = g_surf.GlyphsPresent(FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", cps, miss);
            wchar_t gb[80]; swprintf(gb, 80, L"every UI icon exists in the icon font (first missing U+%04X)", miss); StCheck(ok, ok ? L"every UI icon exists in the icon font" : gb);
        }
        {   // scripted page transitions: frame-time report, then the modes that must switch effects off
            g_frameStats.Clear(); g_gov.level = 0;
            for (int i = 0; i < 12; ++i) { NavGo(i & 1 ? PAbout : PDashboard); StPump(620); }
            wchar_t ft[140]; swprintf(ft, 140, L"frame-time report: %d animated frames, avg %.2f ms, p95 %.2f ms (budget 12 ms), effects level %d", static_cast<int>(g_frameStats.Count()), g_frameStats.Avg(), g_frameStats.P95(), g_gov.level);
            StCheck(g_frameStats.Count() >= 20 && g_frameStats.P95() > 0, ft);
            NavGo(PDashboard); StPump(700); g_dlg.active = true; g_dlg.text = L"Dialog preview: empty the Recycle Bin?"; g_dlg.verb = L"Empty Recycle Bin"; g_dlg.danger = true; StRepaint(h); StPump(200);
            StCheck(StSavePng(h, dir + L"\\dialog_blur.png"), L"dialog with blurred backdrop and a context-specific action captured"); g_dlg.active = false; g_dlg.verb = L"Continue"; g_dlg.danger = false; StRepaint(h);
            g_stHoverOverride = true; const RECT cr0 = ContentRect(); g_ui.mouse = POINT{cr0.left + (cr0.right - cr0.left) * 2 / 5, cr0.top + g_ui.S(110)}; StRepaint(h); StPump(120);
            StCheck(StSavePng(h, dir + L"\\crosshair.png"), L"graph crosshair captured"); g_ui.mouse = POINT{-1, -1}; g_stHoverOverride = false; StRepaint(h);
            const int m0 = Cfg().Int(L"appearance.motion", 0, 0, 2);
            for (int lvl : {MotionReduced, MotionOff}) {
                Cfg().SetInt(L"appearance.motion", lvl); UpdateMotion(); StRepaint(h);
                const uint64_t fr0 = g_animFrames; NavGo(PAbout); StPump(500); NavGo(PDashboard); StPump(500);
                const uint64_t got = g_animFrames - fr0;
                wchar_t mb[120]; swprintf(mb, 120, L"motion %s: page changes drew %d animation frames", lvl == MotionOff ? L"off" : L"reduced", static_cast<int>(got));
                StCheck(lvl == MotionOff ? got == 0 : got <= 16, mb);
            }
            Cfg().SetInt(L"appearance.motion", m0); UpdateMotion();
            g_gov.level = 3; UpdateMotion(); StCheck(g_anim.level == MotionReduced, L"heavy load: the governor falls back to reduced motion"); g_gov.level = 0; UpdateMotion();
        }
        NavGo(PDashboard); StPump(1000);
        const uint64_t f0 = g_surf.frames;
        const uint64_t m0 = g_mouseMoves, af1 = g_animFrames;
        q0 = g_paints; StPump(3000); const uint64_t idleDash = g_paints - q0, dashMoves = g_mouseMoves - m0 + (g_animFrames - af1);
        StCheck(g_surfOn && g_surf.frames > f0, L"renderer is Direct2D through DirectComposition (" + std::to_wstring(g_surf.frames) + L" frames" + (g_surf.warp ? L", software WARP)" : L", hardware)"));
        StCheck(g_surf.deviceResets == 0, L"no graphics device resets during the run");
        {
            int bd = -1; DwmGetWindowAttribute(h, 38, &bd, sizeof(bd));
            StCheck(bd == BackdropType(ResolveMaterial(ReadMaterialInput())), L"material applied to the window: backdrop type " + std::to_wstring(bd) + (g_material == MatAcrylic ? L" (acrylic)" : g_material == MatFrosted ? L" (frosted)" : L" (solid)"));
            for (int want : {MatFrosted, MatSolid, MatAcrylic}) { Cfg().SetInt(L"appearance.material", want); ApplyMaterial(h); StPump(120); int got = -1; DwmGetWindowAttribute(h, 38, &got, sizeof(got)); StCheck(got == BackdropType(ResolveMaterial(ReadMaterialInput())), L"material setting applies live (choice " + std::to_wstring(want) + L", backdrop " + std::to_wstring(got) + L")"); }
            RECT wr0; GetWindowRect(h, &wr0);
            SetWindowPos(h, nullptr, 0, 0, g_ui.S(1040), g_ui.S(680), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); StRepaint(h); StPump(200);
            RECT cr1; GetClientRect(h, &cr1);
            StCheck(cr1.right == g_ui.S(1040) && cr1.bottom == g_ui.S(680) && g_surf.w == static_cast<UINT>(cr1.right), L"window resizes to its minimum; the surface follows (" + std::to_wstring(cr1.right) + L"x" + std::to_wstring(cr1.bottom) + L")");
            StCheck(StSavePng(h, dir + L"\\resized_min.png"), L"capture at the minimum size");
            SetWindowPos(h, nullptr, 0, 0, wr0.right - wr0.left, wr0.bottom - wr0.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); StRepaint(h); StPump(200);
            StCheck(g_widgetSurf.frames > 0 && g_widgetSurf.deviceResets == 0, L"Sidebar and Mini draw through the shared renderer (" + std::to_wstring(g_widgetSurf.frames) + L" frames)");
        }
        StCheck(idleDash >= 1 && idleDash <= 6 + dashMoves, L"idle Dashboard repaints only on the 1 s data tick: " + std::to_wstring(idleDash) + L" paints in 3 s (" + std::to_wstring(g_mouseMoves - m0) + L" mouse moves, " + std::to_wstring(g_animFrames - af1) + L" animation frames)");
        StCheck(!g_read.present || (g_read.ageSec >= 0 && g_read.ageSec < 5), L"readings are timestamped and fresh (" + Ago(g_read.ageSec) + L", " + StampText(g_read.stamp) + L")");
        StCheck(g_store.Points() > 0 && (!g_read.present || g_store.Get(SCpuTemp).Valid() > 0), L"history recorded from the live snapshot (" + std::to_wstring(g_store.Points()) + L" points)");
        Cfg().SetInt(L"monitor.intervalSec", 5); Cfg().SetInt(L"monitor.historyMin", 5); ApplyMonitorConfig();
        StCheck(g_store.Get(SCpuTemp).Capacity() == 60, L"history settings applied: 5 min at 5 s = 60 points");
        Cfg().SetInt(L"monitor.intervalSec", 1); Cfg().SetInt(L"monitor.historyMin", 240); ApplyMonitorConfig();
        StCheck(g_store.Get(SCpuTemp).Capacity() == MonitorStore::kMaxPoints, L"history bounded: 4 h at 1 s is capped at " + std::to_wstring(MonitorStore::kMaxPoints) + L" points");
        NavGo(PDesktop); StPump(1500); StRepaint(h);
        StCheck(StSavePng(h, dir + L"\\desktop_monitoring.png"), L"capture Desktop Monitoring overview");
    }
    SendMessageW(h, WM_COMMAND, 101, 0); SendMessageW(h, WM_COMMAND, 102, 0); StPump(700);   // the Sidebar slides away first
    StCheck(!g_sidebar && !g_mini, L"desktop widgets close");
    {   // Backup & Migration (M-BAK2): create .acbackup in sandbox, change settings, restore and compare
        Cfg().SetInt(L"sidebar.width", 222); Cfg().SetInt(L"appearance.tint", 72); Cfg().Save();
        const AcbRoots roots = AcbDefaultRoots();
        const std::wstring acbPath = dir + L"\\selftest.acbackup";
        AcbCheck vChk{}; std::wstring bErr;
        const bool created = AcbCreateBackup(roots, acbPath, "settings", {}, vChk, bErr);
        StCheck(created && vChk.ok && !vChk.manifest.files.empty(),
                L"backup (M-BAK2): created and verified selftest.acbackup (" + std::to_wstring(vChk.manifest.files.size()) + L" file(s))");
        Cfg().SetInt(L"sidebar.width", 310); Cfg().SetInt(L"appearance.tint", 45); Cfg().Save();
        const AcbRestoreReport rRep = AcbRestoreBackup(roots, acbPath);
        Cfg().Load();
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass"));
        StCheck(rRep.ok && !rRep.rolledBack && Cfg().Int(L"sidebar.width", 210, 140, 360) == 222 &&
                Cfg().Int(L"appearance.tint", 68, 35, 95) == 72 && FileExists(rRep.recoveryDir + L"\\recovery-manifest.json"),
                L"backup (M-BAK2): restore recovered settings (width=222, tint=72) and wrote recovery point");
        std::wstring preDir;
        const bool preOk = AcbCreatePreChangeRecoveryPoint(roots, L"Selftest icon theme switch", preDir);
        const auto recList = AcbListRecoveryPoints(roots);
        StCheck(preOk && !recList.empty() && FileExists(preDir + L"\\recovery-manifest.json"),
                L"backup (M-BAK5): pre-change recovery point created and listed in Recovery History (" + std::to_wstring(recList.size()) + L" point(s))");
        bool sawEngineLog = false;
        for (const auto& l : LogSnapshot()) if (l.text.find(L"Background engine started.") != std::wstring::npos) { sawEngineLog = true; break; }
        StCheck(sawEngineLog, L"activity log (B04): engine process log lines appear in LogSnapshot() on the Activity page");
    }
    // low-level guards (second line of defence)
    StCheck(!RunOp(L"Self-test probe operation", [] { return true; }), L"RunOp refuses operations");
    StCheck(!RegSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore", L"SelftestProbe", 1) &&
            !RegGet(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore", L"SelftestProbe").exists, L"HKLM write refused and absent");
    StCheck(!RegSetDword(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"SelftestProbe", 1) &&
            !RegGet(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"SelftestProbe").exists, L"HKCU write outside the test key refused");
    StCheck(RegSetDword(HKEY_CURRENT_USER, kPrefKey, L"SelftestProbe", 1) && RegDel(HKEY_CURRENT_USER, kPrefKey, L"SelftestProbe"), L"own HKCU test key writable");
    const std::wstring probe = WinDir() + L"\\Temp\\ankhcore_selftest_probe.txt";
    StCheck(!WriteAll(probe, reinterpret_cast<const uint8_t*>("x"), 1) && !FileExists(probe), L"file write outside the test folder refused");
    StCheck(!WriteAll(ProdDataDir() + L"\\Sensors\\command.txt", reinterpret_cast<const uint8_t*>("auto"), 4), L"write to the installed helper's command file refused");
    StCheck(Run(SysDir() + L"\\cmd.exe", L"/c exit 0", true, 5000) == static_cast<DWORD>(-1), L"child process refused");
    StCheck(KillProcess(L"AnkhSensors.exe") == 0 && Hub().HelperRunning(), L"process kill refused; installed helper still running");
    PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof(pm);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm)))
        g_stReport.push_back(L"INFO  private memory " + FormatBytes(pm.PrivateUsage) + L", total refused system changes " + std::to_wstring(TestRefusedCount()));
    g_stReport.push_back((g_stFail ? L"RESULT: " + std::to_wstring(g_stFail) + L" failure(s)" : std::wstring(L"RESULT: all checks passed")) +
                         (g_stSkip ? L" (" + std::to_wstring(g_stSkip) + L" on-screen probe check(s) SKIPPED: AnkhCore was not the foreground window)" : std::wstring()));
    std::wstring all; for (const auto& l : g_stReport) all += l + L"\r\n";
    const std::string u = WToUtf8(all);
    WriteAll(dir + L"\\report.txt", reinterpret_cast<const uint8_t*>(u.data()), u.size());
    LogI(L"[selftest] finished: " + std::to_wstring(g_stFail) + L" failure(s)");
    g_quitting = true;
    PostMessageW(h, WM_COMMAND, 103, 0);   // explicit Exit path (tray menu "Exit")
}
#endif

// The page scrollbar: thumb rectangle (wide grab strip) and the scroll for a thumb top; false when the page fits.
bool PageSbGeom(RECT& thumb, int& viewH, int& th, int& range) {
    const RECT cr = ContentRect(); viewH = cr.bottom - cr.top;
    if (g_contentH[g_page] <= viewH) return false;
    th = std::max(g_ui.S(40), viewH * viewH / g_contentH[g_page]); range = std::max(1, viewH - th);
    const int ty = cr.top + range * g_scroll[g_page] / std::max(1, g_contentH[g_page] - viewH);
    thumb = RECT{cr.right - g_ui.S(16), ty, cr.right, ty + th};
    return true;
}
void PageSbSetFromMouse(int mouseY) {
    RECT t; int viewH, th, range; if (!PageSbGeom(t, viewH, th, range)) return;
    const RECT cr = ContentRect();
    const int top = std::clamp(mouseY - g_pageSbGrabY - static_cast<int>(cr.top), 0, range);
    g_scroll[g_page] = std::clamp(static_cast<int>(static_cast<long long>(top) * (g_contentH[g_page] - viewH) / range), 0, g_contentH[g_page] - viewH);
    if (g_page == PActivity) g_logFollow = g_scroll[g_page] >= g_contentH[g_page] - viewH - g_ui.S(30);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static const UINT kTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == kTaskbarCreated) {   // Explorer restarted: the tray icon and the reserved edge are gone
        TrayAdd(h);
        if (g_sidebar) { g_sbAppBar = false; SbApplyAll(); }   // also re-joins the new desktop in Desktop mode
        SbEnsure();
        SbShellEventNote(SbShellEvent::ExplorerRestarted, L"Explorer restarted");   // and once more when the new desktop has settled
        return 0;
    }
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        g_ui.wnd = h; g_ui.anim = &g_anim; g_uiThread = GetCurrentThreadId(); UpdateMotion();
        SetupWindowLook(h);
        if (g_surfOn && !g_surf.Attach(h)) LogE(L"The window renderer could not start; it retries at the next paint.");
        ApplyMaterial(h);
        g_logo.Load(L"ANKHMARK", g_ui.S(64), g_ui.S(68), /*tint*/false);   // only used if the brand pictures cannot be read
    {   // the owner's originals (D44), decoded as they are; the logo is scaled once, with a high-quality filter, to each size it is shown at
        auto load = [](const wchar_t* res, int w, int h) { Px px; Image32 im = LoadImageRes(res, w, h); if (im.bmp) { px = ToPixels(im); DeleteObject(im.bmp); } return px; };
        g_brandBg = load(L"BRANDBG", 1920, 1080); /* scaled once to fit a full-HD window; shown at the window's size */ // the unified logo (owner brief 2026-11-14): logo.png with its own transparency, the outer circuitry turned down (brand_logo.h)
        auto brand = [&](int size) { Px px = load(L"BRANDLOGO", size, size); BrandLogoSoften(px.p.data(), px.w, px.h); return px; };
        g_brandLogoRail = brand(g_ui.S(84)); g_brandLogoAbout = brand(g_ui.S(112)); g_brandLogoCompact = brand(g_ui.S(40));
    }
        g_ankhBig = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
        LogS().notify = h; LogS().msg = WM_LOGLINE;
        TrayAdd(h);
        SbImportSidebarDiagnostics();
        SbMigrateDesktopMode();
        ApplyMonitorConfig();
        LoadFanPolicyCfg();
        if (Cfg().Bool(L"sidebar.enabled", false)) ToggleSidebar(true);
        if (Cfg().Bool(L"mini.enabled", false)) ToggleMini(true);
        UpdateTick(h);    // the animation clock runs only while something moves
        g_engine.Net(Cfg().Int(L"sidebar.netMode", NetAuto, NetAuto, NetManual), Cfg().Str(L"sidebar.netAdapter", L""));
        g_engine.Start(h, WM_ENGINE_FRAME);   // the once-a-second pipeline runs on its own thread from here on
        g_sbSession.OnRegistered(WTSRegisterSessionNotification(h, NOTIFY_FOR_THIS_SESSION) != FALSE);
        g_iconThumbs.Start(DataDir() + L"\\Cache\\Thumbs", [] { if (g_wnd) PostMessageW(g_wnd, WM_ICONUI, 0, 0); });   // FT-2 / A04: preview thumbnails (worker thread, disk cache)
#ifdef ANKH_TEST_BUILD
        if (g_selftest) SetTimer(h, 9, 1500, nullptr);
#endif
        RefreshAsync(true);
        LoadThumbsAsync();
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(l) == WM_LBUTTONDBLCLK) { ShowWindow(h, SW_SHOW); ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        else if (LOWORD(l) == WM_RBUTTONUP) TrayMenu(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
            case 100: ShowWindow(h, SW_SHOW); ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); break;
            case 101: ToggleSidebar(!g_sidebarOn); break;
            case 102: ToggleMini(!g_miniOn); break;
            case 103: if (Wu().DownloadActive() && !Confirm(L"A Windows update download is running. Exit anyway? The running download is cancelled; what finished is kept.", L"Exit anyway", true)) break; g_quitting = true; DestroyWindow(h); break;
            case 104: if (g_sidebar) SbSetClickThrough(!g_sb.clickThrough); break;
            case 105: if (g_mini) MiniSetClickThrough(!g_miniCfg.clickThrough); break;
            case 106: if (!TestGuard(g_monPaused ? L"Resume monitoring" : L"Pause monitoring")) { g_monPaused = !g_monPaused; g_engine.Monitor(!g_monPaused); LogI(g_monPaused ? L"Hardware monitoring paused from tray." : L"Hardware monitoring resumed from tray."); InvalidateRect(h, nullptr, FALSE); } break;
            case 107: ShowMainWindow(); NavGo(PMonPrefs); break;
        }
        return 0;
    case WM_TIMER:
#ifdef ANKH_TEST_BUILD
        if (w == 9) { KillTimer(h, 9); RunSelftest(h); return 0; }
#endif
        if (w == kSbLifeTimer) { KillTimer(h, kSbLifeTimer); SbLifeTimerFired(); return 0; }
        if (w == 5) { if (g_toastQ.Tick(GetTickCount64())) InvalidateRect(h, nullptr, FALSE); ToastSchedule(h); return 0; }   // FT-TOAST: a toast lived its time
        if (w == 3) { KillTimer(h, 3); ShowWindow(h, SW_HIDE); g_surf.ClearWindowAnimation(); return 0; }   // after the exit animation
        if (w == 2) return 0;   // no longer used: the data tick comes from the engine (WM_ENGINE_FRAME)
        {
            // 33 ms animation tick: repaint the logo area only while it animates (operation running, failure flash),
            // plus one last frame to settle back to the still rest frame. At rest nothing is invalidated here
            // (2.2 repainted this area 30 times a second for the logo's breathing: ~29% of a CPU core while idle).
            g_anim.Tick(GetTickCount64());
            if (g_anim.Active()) { ++g_animFrames; InvalidateRect(h, nullptr, FALSE); }
            if (g_toastQ.Animating(GetTickCount64())) InvalidateRect(h, nullptr, FALSE);   // the entry slide; expiry has its own timer
            UpdateTick(h);
            static unsigned half = 0; if (++half & 1) return 0;   // the rest of the tick keeps its 33 ms pace
            static bool logoWasAnimating = false;
            const bool logoAnimating = g_busy || (g_lastResult == 2 && GetTickCount() - g_resultAt < 900) || (g_lastResult == 1 && GetTickCount() - g_resultAt < 700);
            if (logoAnimating || logoWasAnimating) { InvalidateRect(h, nullptr, FALSE); }
            logoWasAnimating = logoAnimating;
            if (NavAnimate()) { RECT sr{0, 0, SideW(), ClientH()}; InvalidateRect(h, &sr, FALSE); }
            if (((g_page == PApps || NavParent(g_page) == PApps) && !g_appsLoaded) || (g_page == PRecovery && !Snapshot().deepDone)) InvalidateRect(h, nullptr, FALSE);   // loading shimmer
            if (g_busy) { RECT f{0, ClientH() - g_ui.S(kFooter), ClientW(), ClientH()}; InvalidateRect(h, &f, FALSE); }
        }
        return 0;
    case WM_ENGINE_FRAME: {   // the engine published a frame: everything below draws from this one snapshot
            if (kTestBuild && GetEnvironmentVariableW(L"ANKH_PERF", nullptr, 0)) {   // diagnostics: one line per second
                FILETIME a, b, k, u; GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
                ULARGE_INTEGER ku; ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime; ULARGE_INTEGER uu; uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
                char line[200]; const int n = snprintf(line, sizeof(line), "paints=%llu animFrames=%llu surfFrames=%llu tick=%d animActive=%d lastMs=%.2f uiThreadMs=%llu fg=%d\r\n", static_cast<unsigned long long>(g_paints), static_cast<unsigned long long>(g_animFrames), static_cast<unsigned long long>(g_surf.frames), g_tickOn ? 1 : 0, g_anim.Active() ? 1 : 0, g_surf.lastFrameMs, static_cast<unsigned long long>((ku.QuadPart + uu.QuadPart) / 10000), GetForegroundWindow() == h ? 1 : 0);
                FILE* fp = nullptr; if (!_wfopen_s(&fp, (DataDir() + L"\\perf.txt").c_str(), L"ab") && fp) { fwrite(line, 1, n, fp); fclose(fp); }
            }
            EngineFrame frame; g_engine.Take(frame, g_store);
            g_usage = frame.usage; g_read = frame.read; g_net = frame.net; g_sys = frame.sys; g_gpus = frame.gpus; NetApply();
            g_cpuHist.push_back(g_usage.cpu); g_ramHist.push_back(g_usage.ram);
            if (g_cpuHist.size() > 60) { g_cpuHist.erase(g_cpuHist.begin()); g_ramHist.erase(g_ramHist.begin()); }
            ++g_snapSeq;
            for (const auto& ev : frame.alerts) {
                const std::wstring text = AlertName(ev.id) + L": " + (ev.id == "fan.stop" ? std::wstring(L"0 rpm while hot") : NA(ev.value, L" \u00B0C"));
                g_alertLog.push_back({NowText(true), text});
                g_alertFired[ev.id] = ev.atSec; AddToast(text, ev.id.find("crit") != std::string::npos || ev.id == "fan.stop" ? col::err : col::warn);
                if (g_alertLog.size() > 30) g_alertLog.erase(g_alertLog.begin());
                LogW(L"Alert: " + text);
                TrayBalloon(h, text);
            }
            SbEnsure(); SbHealthTick();
            FanAutoFloorTick(g_read);
            if constexpr (kIsBetaBuild) {
                static ULONGLONG s_lastBetaCheck = 0;
                const ULONGLONG nowBeta = GetTickCount64();
                if (nowBeta - s_lastBetaCheck >= 3600000ULL) {
                    s_lastBetaCheck = nowBeta;
                    const BetaTrialStatus bt = CheckAndTouchBetaTrial(true);
                    g_betaDaysRemaining = bt.daysRemaining;
                    if (bt.expired) {
                        RestoreSafeFanOnExit();
                        BetaExpiredNotice(h, bt.clockTampered);
                        DestroyWindow(h);
                        return 0;
                    }
                }
            }
            if (g_sidebar) SbRender();
            if (g_mini) MiniRender();
            if (kTestBuild && GetEnvironmentVariableW(L"ANKH_GPULOG", nullptr, 0)) {   // diagnostics: GPU utilization on every surface, one line per frame
                bool dim = false; SYSTEMTIME lt; GetLocalTime(&lt); wchar_t head[160];
                swprintf(head, 160, L"%02u:%02u:%02u seq=%llu head=%.1f history=%.1f sidebar=%.1f mini=", lt.wHour, lt.wMinute, lt.wSecond, static_cast<unsigned long long>(frame.seq), g_usage.gpu,
                         g_store.Get(SGpuLoad).Size() ? g_store.Get(SGpuLoad).Last() : -1.0, g_sidebar ? g_sbGpuDrawn : -2.0);
                std::wstring line = head + (g_mini ? MiniValue(MGpuLoad, dim) : std::wstring(L"-")) + L" helperGpuLoad=" + NA(g_read.gpuLoad, L"");
                for (const GpuAdapterView& g : g_gpus) { wchar_t b[64]; swprintf(b, 64, L"=%.1f", g.util); line += L" | " + g.name + b; }
                const std::string out = WToUtf8(line) + "\r\n";
                FILE* fp = nullptr; if (!_wfopen_s(&fp, (DataDir() + L"\\gpu-log.txt").c_str(), L"ab") && fp) { fwrite(out.data(), 1, out.size(), fp); fclose(fp); }
            }
            {   // live pages redraw on the data tick; in the background every 3rd second is enough
                static unsigned bgTick = 0;
                if ((g_page == PDashboard || g_page == PMonitor || g_page == PFan || g_page == PDesktop || NavParent(g_page) == PDesktop) && (GetForegroundWindow() == h || ++bgTick % 3 == 0)) InvalidateRect(h, nullptr, FALSE);
            }
        return 0;
    }
    case WM_OPDONE:
        g_busy = false; g_opBackground = false; g_lastResult = w ? 1 : 2; g_resultAt = GetTickCount();
        AddToast((w ? L"Done: " : L"Failed: ") + g_opName, w ? col::ok : col::err, g_opSeq.load());   // keyed by the operation: one toast per operation
        if (l) g_restartNeeded = true;
        if (g_page == PAppIcons) IconPageEnter();   // an apply or restore may have changed what is installed or active
        if (g_page == PRecHistory || g_page == PRecovery) RecRefreshAsync();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_REFRESHED: case WM_LOGLINE:
        if (m == WM_LOGLINE && g_page == PActivity && g_logFollow) {
            const RECT cr = ContentRect();
            g_scroll[PActivity] = std::max(0, g_contentH[PActivity] + g_ui.S(30) - static_cast<int>(cr.bottom - cr.top));
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ICONUI:   // a worker finished the icon installed-state check or a preview (FT-2 / A04)
    {   // 2.9.5: the tray tooltip says what a Windows update download is doing (and goes back to the title when it ends)
        static std::wstring s_tip;
        const bool wuDl = Wu().DownloadActive();
        const std::wstring tip = wuDl ? L"AnkhCore: Windows update download " + std::to_wstring(Wu().Snapshot().overallPct) + L"%" : MainWindowTitle(kTestBuild);
        if (tip != s_tip) { s_tip = tip; lstrcpynW(g_nid.szTip, tip.c_str(), 128); g_nid.uFlags = NIF_TIP | NIF_ICON | NIF_MESSAGE; Shell_NotifyIconW(NIM_MODIFY, &g_nid); }
    }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_HEALTHSCAN: {
        auto* b = reinterpret_cast<HealthScanBundle*>(l);
        if (b) {
            EnterCriticalSection(&g_cs);
            g_cache.sec = b->sec; g_cache.disk = b->disk; g_cache.rps = b->rps;
            g_cache.health = b->health; g_cache.healthDone = true; g_cache.deepDone = true;
            g_cache.cleanupScan = b->cleanup;
            g_cache.cleanupScanFresh = b->cleanup.completed && !b->cleanup.cancelled;
            LeaveCriticalSection(&g_cs);
            const auto& items = CleanItems();
            if (b->cleanup.completed && !b->cleanup.cancelled && b->cleanup.items.size() == items.size()) {
                if (g_cleanSel.size() != items.size()) { g_cleanSel.clear(); for (const auto& it : items) g_cleanSel.push_back(it.defaultOn); }
                if (g_cleanSize.size() != items.size()) g_cleanSize.assign(items.size(), 0);
                for (size_t i = 0; i < items.size(); ++i) {
                    if (b->cleanup.items[i].ok && b->cleanup.items[i].inaccessibleDirs == 0) g_cleanSize[i] = b->cleanup.items[i].bytes;
                }
                g_cleanScanned = (b->cleanup.inaccessibleDirs == 0);
            }
            for (const auto& f : b->health.findings) Log(f.severity >= 2 ? Lv::Warn : Lv::Info, L"Health: " + f.text);
            LogI(L"Cleanup scan: " + std::wstring(CleanupPreviewLabel(b->cleanup.status)) + L" (" + FormatBytes(b->cleanup.totalBytes) + L" in " + std::to_wstring(b->cleanup.categoriesWithData) + L" item(s)).");
            for (const auto& wmsg : b->cleanup.warnings) LogW(L"Cleanup scan: " + wmsg);
            delete b;
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_THUMBS: {
        auto* pk =reinterpret_cast<std::pair<std::vector<std::wstring>, std::vector<Image32>>*>(l);
        for (auto& t : g_thumbs) if (t.bmp) { g_surf.Forget(t.bmp); DeleteObject(t.bmp); }
        g_thumbPaths = pk->first; g_thumbs = pk->second; delete pk;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_NCCALCSIZE: if (w) return 0; break;   // custom chrome: the client area is the whole window; DWM keeps the shadow and rounded corners
    case WM_NCHITTEST: {   // the resize border (the rest is client: the drag areas are handled on mouse down)
        if (IsZoomed(h)) return HTCLIENT;
        const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; RECT wr; GetWindowRect(h, &wr); const int b = g_ui.S(6);
        const bool L = pt.x < wr.left + b, R = pt.x >= wr.right - b, T = pt.y < wr.top + b, B = pt.y >= wr.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mi = reinterpret_cast<MINMAXINFO*>(l);
        mi->ptMinTrackSize = POINT{g_ui.S(1040), g_ui.S(680)};
        MONITORINFO mon{sizeof(mon)};   // maximized = the work area of this monitor (the taskbar stays visible)
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mon)) {
            mi->ptMaxPosition = POINT{mon.rcWork.left - mon.rcMonitor.left, mon.rcWork.top - mon.rcMonitor.top};
            mi->ptMaxSize = POINT{mon.rcWork.right - mon.rcWork.left, mon.rcWork.bottom - mon.rcWork.top};
        }
        return 0;
    }
    case WM_ACTIVATE: UpdatePulse(h); break;
    case WM_MOVE: if (g_material == MatWallpaper || g_material == MatClear) InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DISPLAYCHANGE: WallForget(); InvalidateRect(h, nullptr, FALSE); SbShellEventNote(SbShellEvent::DisplayChanged, L"the display setup changed"); break;   // a hosted Sidebar is a child window: only this one gets the broadcast
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER) { WallForget(); InvalidateRect(h, nullptr, FALSE); SbRecoverAfterThemeChange(); }
        else if (w == SPI_SETWORKAREA) SbShellEventNote(SbShellEvent::WorkAreaChanged, L"the work area changed");
        else SbRecoverAfterThemeChange();
        ApplyMaterial(h);
        return 0;
    case WM_THEMECHANGED:
        ApplyMaterial(h);
        SbRecoverAfterThemeChange();
        return 0;   // Transparency effects, High Contrast
    case WM_POWERBROADCAST: if (w == PBT_APMPOWERSTATUSCHANGE) ApplyMaterial(h); else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) SbShellEventNote(SbShellEvent::Resumed, L"the PC resumed"); return TRUE;   // mains / battery, Battery saver
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK) {
            g_sbSession.OnSessionEvent(true, false, GetTickCount64());
        } else if (w == WTS_SESSION_UNLOCK) {
            if (g_sbSession.OnSessionEvent(false, true, GetTickCount64())) {
                SbShellEventNote(SbShellEvent::SessionUnlocked, L"the Windows session was unlocked");
                if (!g_quitting) RefreshAsync(false);
            }
        }
        return 0;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g_cw = LOWORD(l); g_ch = HIWORD(l); if (g_surfOn) g_surf.Resize(); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_PAINT: PaintAll(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_ui.mouse = p; ++g_mouseMoves;
        if (g_dragging && g_dragFn) g_dragFn();
        if (g_pageSbDrag) PageSbSetFromMouse(p.y);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0}; TrackMouseEvent(&tme);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE: g_ui.mouse = {-1, -1}; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        if (g_pageSbDrag) { g_pageSbDrag = false; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (g_dragging) { g_dragging = false; ReleaseCapture(); if (g_dragEnd) g_dragEnd(); g_dragFn = nullptr; g_dragEnd = nullptr; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (g_dlg.active) { if (w == VK_ESCAPE || w == VK_RETURN) { g_dlg.result = false; g_dlg.active = false; } return 0; }   // Enter = the safe answer, as the old dialog's default
        if (NavKey(w)) return 0;
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, g_ui.OverAny() ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        g_ui.mouse = p;
        g_navKeyboard = false;
        if (!g_dlg.active && p.x >= SideW()) {   // the page scrollbar: grab the thumb, or jump the thumb to a click on the track
            RECT t; int viewH, th, range; const RECT cr = ContentRect();
            if (PageSbGeom(t, viewH, th, range) && p.x >= t.left && p.y >= cr.top && p.y < cr.bottom) {
                g_pageSbGrabY = (p.y >= t.top && p.y < t.bottom) ? p.y - t.top : th / 2;
                g_pageSbDrag = true; SetCapture(h); PageSbSetFromMouse(p.y);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
        }
        if (!g_ui.Click(p) && p.y < g_ui.S(kHeader) && p.x > SideW()) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        else if (p.x < SideW() && p.y < NavTop() - g_ui.S(8)) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT wp{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ScreenToClient(h, &wp);
        if (wp.x < SideW()) {
            g_navScroll = NavScrollClamp(g_navScroll - GET_WHEEL_DELTA_WPARAM(w) * g_ui.S(48) / WHEEL_DELTA, NavContentHeight(true), NavViewH());
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        const RECT cr = ContentRect();
        const int maxS = std::max(0, g_contentH[g_page] - static_cast<int>(cr.bottom - cr.top));
        g_scroll[g_page] = std::clamp(g_scroll[g_page] - GET_WHEEL_DELTA_WPARAM(w) * g_ui.S(60) / WHEEL_DELTA, 0, maxS);
        if (g_page == PActivity) g_logFollow = g_scroll[g_page] >= maxS - g_ui.S(30);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (g_dlg.active) { g_dlg.result = false; g_dlg.active = false; return 0; }
        if (g_busy && !Confirm(L"\"" + g_opName + L"\" is still running. Close anyway?", L"Close anyway", true)) return 0;
        // keep running in the tray so the sidebar, mini monitor and sensors stay
        if (!g_quitting) {
            if (g_surfOn && g_anim.level == MotionFull && !AnubisActive() && !IsIconic(h)) { g_surf.AnimateWindow(1.f, 0.f, 1.f, .97f, 180); SetTimer(h, 3, 190, nullptr); }
            else ShowWindow(h, SW_HIDE);
            static bool told = false;
            if (!told) { told = true; TrayBalloon(h, L"AnkhCore is still running here. Right-click for options or Exit."); }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        // signing out / shutting down: the process may be ended without WM_DESTROY, so stop the
        // sensor helper now (it returns fan control to automatic) and release the reserved screen edge
        if (w) { RestoreSafeFanOnExit(); g_engine.Shutdown(); if (g_sidebar) SbAppBarRemove(); TrayRemove(h); LogI(L"Session ending: monitoring stopped safely."); }
        return 0;
    case WM_DESTROY:
        if (g_sbSession.registered) { WTSUnRegisterSessionNotification(h); g_sbSession.OnUnregistered(); }
        TrayRemove(h);
        ToggleSidebar(false, false); ToggleMini(false, false);   // keep "enabled": they reopen next start
        g_healthWorker.Shutdown();
        g_iconThumbs.Stop(); g_iconStatus.Join();
        g_backupList.Join(); g_storageScan.Shutdown();
        UpdUi().Shutdown();
        Wu().Shutdown();   // 2.9.5: cancels a running Windows update download and lets the worker finish; it never touches the window afterwards
        RestoreSafeFanOnExit();
        g_engine.Shutdown();
#ifdef ANKH_TEST_BUILD
        if (g_selftest) StRestoreSettings();   // last write: the run leaves the settings as it found them
#endif
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 2.12 (owner): a new installation puts the AnkhCore look on by itself. The pointers are applied by CurEnsureFirstRunDefault; this does the Anubis wallpapers (the slideshow of the
// package's pictures, shuffled, with the interval of the settings). Only for a new installation: an existing user keeps the wallpaper they have.
void FirstRunWallpapers() {
    if (Cfg().Bool(L"wallpaper.autoApplied", false)) return;
    if (Cfg().Str(L"appearance.profile", L"").find(L"new-install") == std::wstring::npos) return;
    if (WallImages().empty()) return;   // the pictures are not in place yet: tried again at the next start
    if (SetWallSlideshow()) { Cfg().SetBool(L"wallpaper.autoApplied", true); Cfg().Save(); LogOk(L"First run: the Anubis wallpapers were put on."); }
}

// ---------------------------------------------------------------- uninstall
int Uninstall() {
    if (!IsElevated()) { ShellExecuteW(nullptr, L"runas", SelfPath().c_str(), L"--uninstall", nullptr, SW_SHOWNORMAL); return 0; }
    if (MessageBoxW(nullptr, L"Uninstall AnkhCore?\n\nWindhawk and its mods stay installed (remove Windhawk from Settings > Apps if you want).",
                    kProductFull, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
    const int undo = MessageBoxW(nullptr, L"Do a full clean: also undo AnkhCore's changes and remove what it added?\n\n"
                                          L"Yes (full clean): services, tweaks, startup apps, pointers, wallpaper and the theme go back; the original icons return after a restart. "
                                          L"AnkhCore's theme, visual styles, cursor packs and Anubis wallpapers are removed from Windows, and so are the Windhawk mods that came with AnkhCore "
                                          L"(Windhawk itself and your own mods stay). AnkhCore's settings and data are deleted; your Backup and Recovery folders stay.\n"
                                          L"No: remove only the program and keep everything else as it is now.", kProductFull, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (undo == IDCANCEL) return 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RegisterThemeUndo(); RegisterSystemUndo();
    UninstallReport rep; rep.undoAsked = undo == IDYES;   // SEC-3: every step's result goes into the final message
    LogI(L"Uninstall started" + std::wstring(rep.undoAsked ? L" (undo AnkhCore's changes: yes)." : L" (undo AnkhCore's changes: no)."));
    if (rep.undoAsked) {
        rep.undo = JournalUndoEx([](const JEntry&) { return true; });
        (rep.undo.Success() ? LogOk : LogE)(L"Uninstall: " + rep.undo.Text());
        bool iconsOk = true, iconsTried = false;
        // the 2.5 icon theme lives in Windhawk Resource Redirect's settings and points into the program folder that is about to go
        if (!Cfg().Str(L"icons.activeThemePath", L"").empty()) { iconsTried = true; iconsOk &= RestoreDefaultIconsRedirect(nullptr); }
        // 2.2 file swaps: only when AnkhCore changed or backed up icon files (the restore stops Explorer for a moment)
        if (!BackedUpTargets().empty() || !GetIconState().patched.empty()) { iconsTried = true; iconsOk &= RestoreOriginalIcons(nullptr); }
        rep.icons = iconsTried ? (iconsOk ? 1 : 0) : -1;
    }
    // a running AnkhCore and its engine exit first (D26); the engine stops the helper, which hands fan control back
    auto windowsOpen = [] { return FindWindowW(Tagged(L"AnkhCoreWindow").c_str(), nullptr) || FindWindowW(Tagged(L"AnkhCoreEngineWindow").c_str(), nullptr); };
    for (const wchar_t* cls : {L"AnkhCoreWindow", L"AnkhCoreEngineWindow"}) if (HWND w = FindWindowW(Tagged(cls).c_str(), nullptr)) PostMessageW(w, WM_COMMAND, 103, 0);
    for (int i = 0; i < 75 && windowsOpen(); ++i) Sleep(200);
    // stop the sensor helper if nothing else did: on "quit" it hands fan control back to automatic and frees its files
    Hub().Stop(); Hub().StopHelper();
    rep.stillRunning = windowsOpen() || Hub().HelperRunning();
    if (rep.stillRunning) LogE(L"Uninstall: an AnkhCore window or the sensor helper is still running.");
    rep.removal = RemoveInstall();   // tasks, shortcuts, the uninstall entry and the program folder (the rest of it at the next restart); the data folder and settings stay
    if (rep.undoAsked) {   // 2.12: the full clean (the data itself goes last, after the final message)
        const FullCleanResult fc = FullCleanWindowsAndMods();
        rep.fullClean = true; rep.cleanedThemes = fc.themes; rep.cleanedCursors = fc.cursors; rep.cleanedWallpapers = fc.wallpapers; rep.cleanedMods = fc.mods; rep.cleanFailed = fc.failed;
        for (const auto& f : fc.failed) LogE(L"Uninstall: full clean could not remove " + f);
        LogOk(L"Uninstall: full clean removed " + std::to_wstring(fc.themes) + L" theme item(s), " + std::to_wstring(fc.cursors) + L" cursor pack(s), " + std::to_wstring(fc.wallpapers) + L" wallpaper folder(s), " + std::to_wstring(fc.mods) + L" Windhawk mod(s).");
    }
    const UninstallMessage msg = UninstallSummary(rep, DataDir());
    (msg.ok ? LogOk : LogE)(L"Uninstall finished: " + std::wstring(msg.ok ? L"everything confirmed." : L"not everything succeeded (see the lines above)."));
    MessageBoxW(nullptr, msg.text.c_str(), kProductFull, MB_OK | (msg.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
    if (rep.fullClean) { FullCleanResult d; FullCleanData(d); }
    return msg.ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring cmd = cmdLine ? cmdLine : L"";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (kTestBuild) {
        // isolated test build: only the UI (and --selftest). No helper modes, no elevation, no task.
        g_selftest = cmd.find(L"--selftest") != std::wstring::npos;
        if (!g_selftest && cmd.find(L"--") != std::wstring::npos && cmd.find(L"--startup") == std::wstring::npos && cmd.find(L"--engine") == std::wstring::npos) {
            MessageBoxW(nullptr, L"This is the AnkhCore test build. Helper modes (install, uninstall, theme, cursors, wallpaper) are disabled.", L"AnkhCore (test build)", MB_OK | MB_ICONINFORMATION);
            return 2;
        }
    }
    Cfg().Load();
    if (cmd.find(L"--engine") == std::wstring::npos) { LogSettingsMigration(Cfg().LastMigration()); if (!kTestBuild) { if (ApplyAppearanceProfileOnce(Cfg()) > 0) Cfg().Save(); ApplyGlassDefaultsOnce(); StartProcRuleWatcher(); } }   // the engine process only reads; the UI process logs and applies the 2.8 defaults once
    ApplyTheme(Cfg().Str(L"appearance.theme", L"windows-glass"));
    if (cmd.find(L"--engine") != std::wstring::npos) {
        if constexpr (kIsBetaBuild) {
            if (CheckAndTouchBetaTrial(false).expired) return 0;
        }
        return EngineProcess::Get().Run(inst);   // the background engine: no UI, no tray
    }
    RegisterThemeUndo(); RegisterSystemUndo();
    (void)Tweaks(); (void)CleanItems(); (void)Tools(); (void)RepairTools();   // build once, before any thread
    // helper modes (run as the signed-in user by the setup / at sign-in)
    if (cmd.find(L"--apply-theme") != std::wstring::npos) { const size_t p = cmd.find(L"--apply-theme") + 13; return ThemeLogonHelper(_wtoi(cmd.c_str() + p)); }
    if (cmd.find(L"--apply-now") != std::wstring::npos) { std::wstring m; return ApplyTheme(false, m) ? 0 : 1; }
    if (cmd.find(L"--wallpaper-on") != std::wstring::npos) return SetWallSlideshow() ? 0 : 1;
    if (cmd.find(L"--cursors-on") != std::wstring::npos) return SetCursors(true) ? 0 : 1;
    if (cmd.find(L"--cursors-off") != std::wstring::npos) return SetCursors(false) ? 0 : 1;
    if (cmd.find(L"--uninstall") != std::wstring::npos) return Uninstall();
    // the control center always runs as administrator
    if (!kTestBuild && !IsElevated()) {
        if (cmd.find(L"--elevated") == std::wstring::npos && RunLaunchTask()) return 0;
        ShellExecuteW(nullptr, L"runas", SelfPath().c_str(), L"--elevated", nullptr, SW_SHOWNORMAL);
        return 0;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, Tagged(L"AnkhCoreControlCenter").c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cmd.find(L"--startup") != std::wstring::npos) return 0;   // already running: sign-in start stays silent
        if (HWND other = FindWindowW(Tagged(L"AnkhCoreWindow").c_str(), nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_opCs);
    SecureDataDir();
    if constexpr (kIsBetaBuild) {
        const BetaTrialStatus bt = CheckAndTouchBetaTrial(true);
        g_betaDaysRemaining = bt.daysRemaining;
        if (bt.expired) {
            LogW(bt.clockTampered ? L"AnkhCore Beta stopped: system clock tampering detected." : L"AnkhCore Beta 30-day trial expired.");
            BetaExpiredNotice(nullptr, bt.clockTampered);
            ReleaseMutex(mutex);
            return 0;
        }
        LogI(L"Beta 30-day trial active: " + std::to_wstring(bt.daysRemaining) + L" day(s) remaining on this device.");
    }
    if (kTestBuild) {
        // read-only snapshot of the installed journal so Recovery shows real history; the original is never written
        CopyFileW((ProdDataDir() + L"\\journal.txt").c_str(), JournalPath().c_str(), TRUE);
        LogI(L"AnkhCore " + std::wstring(kVersion) + L" TEST BUILD (isolated: " + DataDir() + L"; system changes disabled" + (g_selftest ? L"; self-test" : L"") + L").");
    }
    LogI(L"AnkhCore " + std::wstring(kVersion) + L" started on " + WinVerText(GetWinVer()) + L".");
    g_ui.dpi = SystemDpi();
    g_ui.f = MakeFonts(g_ui.dpi);
    NavRestore();
    if (!kTestBuild) { CurEnsureFirstRunDefault(CurRoots()); FirstRunWallpapers(); }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; static const std::wstring mainCls = Tagged(L"AnkhCoreWindow"); wc.lpszClassName = mainCls.c_str();
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    const int w = g_ui.S(kW), h = g_ui.S(kH);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    g_surfOn = g_surf.Probe();   // a window without a redirection surface cannot fall back to GDI later, so decide first
    if (!g_surfOn) LogW(L"Direct2D is not available on this PC; using the GDI renderer.");
    HWND wnd = CreateWindowExW(WS_EX_APPWINDOW | (g_surfOn ? WS_EX_NOREDIRECTIONBITMAP : 0), wc.lpszClassName, MainWindowTitle(kTestBuild).c_str(), WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                               wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    const bool startHidden = cmd.find(L"--startup") != std::wstring::npos && Cfg().Bool(L"startup.minimized", true);
    if (startHidden) LogI(L"Started at sign-in, minimized to the system tray.");
    else {
        if (g_surfOn && g_anim.level == MotionFull && !AnubisActive()) g_surf.AnimateWindow(0.f, 1.f, .97f, 1.f, 220);   // run by the compositor; Anubis already animates windows
        ShowWindow(wnd, SW_SHOW); UpdateWindow(wnd);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    ReleaseMutex(mutex);
    return 0;
}
