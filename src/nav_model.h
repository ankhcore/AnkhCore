// nav_model.h - 2.7 J1: the page ids and the navigation structure (pure data and routing; unit-tested in tests/test_base.cpp).
//
// Moved out of app.cpp unchanged in meaning so that the routes can be tested. What is NEW in 2.7:
//   * the rail has the ten sections of the 2.7 architecture: Dashboard, Monitor, Sidebar, Windows Studio, Tweaks, Optimize & Repair,
//     Apps Manager, Recovery, Activity, About (the Settings control is a separate top-bar item, J2);
//   * Sidebar is its own section (it was a child of Monitor); Windows Studio holds Appearance & Themes (with its Theme, Wallpaper, Cursor
//     and Icon manager sub-pages) and Windhawk (it was a top-level item); Apps is called Apps Manager;
//   * NO page id changed and none was removed or reordered: `nav.page` values saved by earlier versions still open the same page. Only the
//     saved "open section" (an index into the rail) needs a migration: `nav.layout` (1 = before 2.7, absent = 1; 2 = now).
// Every page still has exactly one home; a sub-page has a parent (Back goes there) and has no rail entry of its own.
#pragma once
#include <vector>

namespace ak {

enum Page { PDashboard, PMonitor, PFan, PAppearance, PWindhawk, PTweaks, POptimize, PApps, PRecovery, PActivity, PAbout, PStartup, PDesktop,
            PSbLayout, PSbAppearance, PSbWidgets, PSbBehavior, PSbProfiles, PMiniSet, PMonPrefs,   // sub-pages of Sidebar & Mini Monitor (no rail entry of their own)
            PAppMaterial, PAppTheme, PAppWallpaper, PAppCursor, PAppIcons,                         // sub-pages of Appearance & Themes (M-NAV3)
            PAppsInstalled, PAppsBuiltin, PAppsUninstall,                                          // sub-pages of Apps (M-NAV3)
            PBackup, PRecHistory,                                                                  // sub-pages of Recovery (M-BAK5)
            PDrivers, PStorage,                                                                    // sub-pages of Optimize & Repair (M-SYS1)
            PHardware, PNetwork, PErrLog,                                                          // rail pages of Monitor (Hardware Information, Network Monitor) and Activity (Error Logs); appended so saved nav.page values stay valid
            PSettings, PAppsUpdates, PSystemCare, PProcMgr, PPowerToys,                                                                             // 2.7 J2: AnkhCore's own settings, opened from the top-bar control (no rail entry); appended so saved nav.page values stay valid
            PageCount };

struct NavSection { const wchar_t* name; const wchar_t* icon; std::vector<int> pages; };

constexpr int kNavLayoutVersion = 2;   // 1 = the rail of 2.5/2.6; 2 = the 2.7 rail

inline const std::vector<NavSection> kNav = {
    {L"Dashboard", L"\xE80F", {PDashboard}},
    {L"Monitor", L"\xE9D2", {PMonitor, PHardware, PNetwork, PFan, PStartup}},
    {L"Sidebar", L"\xE9D9", {PDesktop}},
    {L"Windows Studio", L"\xE771", {PAppearance, PWindhawk, PPowerToys}},
    {L"Tweaks", L"\xE713", {PTweaks}},
    {L"Optimize & Repair", L"\xE90F", {POptimize}},
    {L"Apps Manager", L"\xE71D", {PApps}},
    {L"Recovery", L"\xE81C", {PRecovery}},
    {L"Activity", L"\xE8FD", {PActivity, PErrLog}},
    {L"About", L"\xE946", {PAbout}},
};

inline int NavParent(int page) {
    if (page >= PSbLayout && page <= PMonPrefs) return PDesktop;
    if (page == PAppMaterial) return PSettings;   // 2.7 J2: "AnkhCore Appearance" is the application's own look: a Settings page, not Windows customization
    if (page >= PAppTheme && page <= PAppIcons) return PAppearance;
    if ((page >= PAppsInstalled && page <= PAppsUninstall) || page == PAppsUpdates) return PApps;   // 2.7: App Updates is an Apps Manager page (appended id)
    if (page >= PBackup && page <= PRecHistory) return PRecovery;
    if ((page >= PDrivers && page <= PStorage) || page == PSystemCare || page == PProcMgr) return POptimize;   // 2.8: System Care is an Optimize & Repair page (appended id)
    return -1;   // the page a sub-page goes Back to; -1 for a rail page
}
// A page that has no rail entry and no parent in the rail: it is opened from the top bar (2.7: Settings). Its sub-pages have it as parent.
inline bool NavIsTopBarPage(int page) { return page == PSettings; }
inline int NavSectionOf(int page) {
    if (NavParent(page) >= 0) page = NavParent(page);
    for (size_t s = 0; s < kNav.size(); ++s) for (int p : kNav[s].pages) if (p == page) return static_cast<int>(s);
    return 0;
}
inline std::vector<int> NavOrder() { std::vector<int> v; for (const auto& s : kNav) for (int p : s.pages) v.push_back(p); return v; }

// ---------------------------------------------------------------- Settings (2.7 J2)
// AnkhCore's own preferences, kept apart from Windows personalization. Each link opens an existing page; nothing is duplicated and no
// setting is invented: where AnkhCore has no setting (language, its own update checks) the page says so instead.
struct SettingsCategory { const wchar_t* id; const wchar_t* about; };
inline const std::vector<SettingsCategory>& SettingsCategories() {
    static const std::vector<SettingsCategory> v = {
        {L"General", L"Startup and general preferences."},
        {L"Appearance", L"How AnkhCore itself looks. Windows themes, wallpapers, cursors and icons are in Windows Studio."},
        {L"Performance", L"Animation and how often monitoring samples."},
        {L"Notifications", L"Alerts and their thresholds."},
        {L"Updates", L"Updating AnkhCore and other applications."},
        {L"Privacy", L"What AnkhCore stores and what leaves this PC."},
        {L"Advanced", L"Logging, diagnostics and where AnkhCore keeps its files."},
    };
    return v;
}
struct SettingsLink { const wchar_t* category; const wchar_t* title; const wchar_t* desc; int page; };
inline const std::vector<SettingsLink>& SettingsLinks() {
    static const std::vector<SettingsLink> v = {
        {L"General", L"Startup & Background", L"Start with Windows, start in the tray, and the background monitoring engine.", PStartup},
        {L"Appearance", L"AnkhCore Appearance", L"Window material, tint, battery behavior, blur, glow, motion and widget glass.", PAppMaterial},
        {L"Performance", L"Animation and motion", L"The motion level (full, reduced, off) is part of AnkhCore Appearance.", PAppMaterial},
        {L"Performance", L"Monitoring interval and history", L"How often the monitoring engine samples and how much history is kept.", PMonPrefs},
        {L"Notifications", L"Alerts and thresholds", L"Which alerts are on and at what values they fire (they show as toasts and tray notifications).", PMonPrefs},
        {L"Updates", L"Software updates for other applications", L"Check and update applications through AnkhCore's own UniGetUI session, with a recovery point first.", PBackup},
        {L"Privacy", L"Support bundle and log", L"Export a support bundle or the log. They stay on this PC and can contain folder paths with your user name.", PActivity},
        {L"Advanced", L"Error log", L"Warnings and errors AnkhCore recorded.", PErrLog},
        {L"Advanced", L"Backup and recovery", L"Backups, recovery points and the automatic backup before major changes.", PBackup},
    };
    return v;
}

// A saved page id is valid when it names a page; anything else falls back to the Dashboard (never a crash, never a stale index).
inline int NavPageOrDashboard(int saved) { return saved >= 0 && saved < PageCount ? saved : static_cast<int>(PDashboard); }

// The saved "open section" is an index into kNav. Layout 1 (2.5/2.6) had: 0 Dashboard, 1 Monitor, 2 Appearance, 3 Windhawk, 4 Tweaks, 5 Optimize,
// 6 Apps, 7 Recovery, 8 Activity, 9 About. Appearance (2) and Windhawk (3) are now both in Windows Studio (3); the others kept their index. The result
// is -1 (nothing open) for anything that is not a valid index of the layout it came from, or a section with one page.
inline int NavMigrateOpen(int savedOpen, int savedLayout) {
    if (savedOpen < 0) return -1;
    int idx = savedOpen;
    if (savedLayout < 2) {
        if (savedOpen > 9) return -1;
        idx = savedOpen == 2 || savedOpen == 3 ? 3 : savedOpen;
    }
    if (idx < 0 || idx >= static_cast<int>(kNav.size())) return -1;
    return kNav[static_cast<size_t>(idx)].pages.size() > 1 ? idx : -1;
}

}  // namespace ak
