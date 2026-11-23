// AnkhCore 2.2 — installed-apps inventory + uninstall.
// Desktop programs come from the three Uninstall registry hives (the same
// source "Programs and Features" uses); Microsoft Store / UWP apps come from
// PowerShell's Get-AppxPackage. Desktop apps are removed by running their own
// uninstall string; Store apps by Remove-AppxPackage — exactly as asked.
#pragma once
#include "base.h"
#include <algorithm>

namespace ak {

struct AppEntry {
    std::wstring name, publisher, version, uninstall, key;   // key: reg path | PackageFullName
    bool store = false;
    std::wstring installLocation, icon;   // 2.7: as the entry gave them (may be empty or wrong); used only as evidence, never as a path to delete without the K1 checks
    bool userHive = false;                // the uninstall entry lives under HKCU
};

inline void EnumWin32Apps(HKEY root, const std::wstring& base, std::vector<AppEntry>& out) {
    for (const auto& k : RegSubkeys(root, base)) {
        const std::wstring kp = base + L"\\" + k;
        const std::wstring name = RegStr(root, kp, L"DisplayName");
        if (name.empty()) continue;
        if (RegDword(root, kp, L"SystemComponent", 0) == 1) continue;   // hidden system entries
        if (!RegStr(root, kp, L"ParentKeyName").empty()) continue;      // updates/patches
        if (RegStr(root, kp, L"ReleaseType") == L"Security Update" ||
            RegStr(root, kp, L"ReleaseType") == L"Update") continue;
        std::wstring us = RegStr(root, kp, L"QuietUninstallString");
        if (us.empty()) us = RegStr(root, kp, L"UninstallString");
        if (us.empty()) continue;                                       // nothing to run
        AppEntry e;
        e.name = name; e.publisher = RegStr(root, kp, L"Publisher");
        e.version = RegStr(root, kp, L"DisplayVersion");
        e.uninstall = us; e.key = kp; e.store = false;
        e.installLocation = RegStr(root, kp, L"InstallLocation"); e.icon = RegStr(root, kp, L"DisplayIcon"); e.userHive = root == HKEY_CURRENT_USER;
        out.push_back(e);
    }
}

// Full inventory. includeStore runs PowerShell (slower) — call off the UI thread.
inline std::vector<AppEntry> EnumApps(bool includeStore) {
    std::vector<AppEntry> win;
    EnumWin32Apps(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", win);
    EnumWin32Apps(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", win);
    EnumWin32Apps(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", win);
    std::sort(win.begin(), win.end(), [](const AppEntry& a, const AppEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    win.erase(std::unique(win.begin(), win.end(), [](const AppEntry& a, const AppEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) == 0; }), win.end());

    std::vector<AppEntry> store;
    if (includeStore) {
        RunCapture(L"powershell.exe",
            L"-NoProfile -ExecutionPolicy Bypass -Command \""
            L"Get-AppxPackage | Where-Object { -not $_.IsFramework -and -not $_.NonRemovable } | "
            L"ForEach-Object { $_.Name + '|' + $_.PackageFullName + '|' + $_.Publisher }\"",
            [&](const std::wstring& line) {
                const auto p = Split(line, L'|');
                if (p.size() >= 2 && !p[0].empty()) {
                    AppEntry e; e.name = p[0]; e.key = p[1]; e.uninstall = p[1]; e.store = true;
                    if (p.size() >= 3) e.publisher = p[2];
                    store.push_back(e);
                }
            }, 90000);
        std::sort(store.begin(), store.end(), [](const AppEntry& a, const AppEntry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    }
    std::vector<AppEntry> out;
    out.reserve(win.size() + store.size());
    for (auto& e : win) out.push_back(std::move(e));
    for (auto& e : store) out.push_back(std::move(e));
    return out;
}

// Uninstall one app. Desktop: run its own uninstaller. Store: Remove-AppxPackage.
inline void UninstallApp(const AppEntry& e) {
    if (e.store) {
        LogI(L"Removing Store app: " + e.name);
        Run(L"powershell.exe",
            L"-NoProfile -ExecutionPolicy Bypass -Command \"Remove-AppxPackage -Package '" + e.key + L"'\"",
            true, 0);
    } else {
        LogI(L"Uninstalling: " + e.name);
        Run(L"cmd.exe", L"/c " + e.uninstall, false, 0);   // the app shows its own uninstaller
    }
}

}  // namespace ak
