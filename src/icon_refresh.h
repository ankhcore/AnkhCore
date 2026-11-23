// FT-2 / A01: what AnkhCore can honestly say after an icon theme was applied or removed through Windhawk Resource Redirect.
// No Windows restart is needed for that method: the theme is a setting of a mod that is already loaded into Explorer and
// other programs. What AnkhCore can verify is stated as verified; what it cannot (whether every open window already
// shows the new icons) is not claimed. Pure logic: no Windows calls here (tests: tests\test_base.cpp).
#pragma once
#include <string>

namespace ak {

enum class IconShellState { Unknown, Loaded, NotLoaded };   // is the Resource Redirect module loaded in Explorer?

struct IconRefreshReport {
    bool applied = false;                       // the theme path was written to Resource Redirect and read back (or removed and read back)
    bool removed = false;                       // "restore default icons" instead of an apply
    IconShellState inExplorer = IconShellState::Unknown;
    bool shellNotified = false;                 // SHChangeNotify(SHCNE_ASSOCCHANGED) was sent
    bool oldMethodPending = false;              // icons changed by the 2.2 file swap this boot: only a Windows restart completes those
    bool explorerRestarted = false;             // the user restarted Explorer after the change (nothing is left to offer)
};

struct IconRefreshAdvice {
    std::wstring text;                          // shown under the buttons; empty: nothing to say
    bool offerExplorerRestart = false;          // show "Restart Explorer" (a separate, confirmed action: never automatic)
    bool warn = false;                          // something could not be confirmed
    bool needsWindowsRestart = false;           // only for the 2.2 file-swap method
};

// Windhawk names a loaded mod module "<mod id>_<version>_<build>.dll" (for example "local@icon-resource-redirect_1.3_647871.dll").
inline bool IconModuleIsMod(const std::wstring& moduleName, const std::wstring& modId) {
    if (modId.empty() || moduleName.size() <= modId.size() + 5) return false;
    auto low = [](wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c; };
    for (size_t i = 0; i < modId.size(); ++i) if (low(moduleName[i]) != low(modId[i])) return false;
    if (moduleName[modId.size()] != L'_') return false;
    const std::wstring tail = moduleName.substr(moduleName.size() - 4);
    return low(tail[0]) == L'.' && low(tail[1]) == L'd' && low(tail[2]) == L'l' && low(tail[3]) == L'l';
}

inline IconRefreshAdvice IconAdvice(const IconRefreshReport& r) {
    IconRefreshAdvice a;
    a.needsWindowsRestart = r.oldMethodPending;
    if (!r.applied) {   // nothing was changed: the operation reports its own failure
        if (r.oldMethodPending) a.text = L"Icons changed by the older file-swap method need a restart of Windows.";
        return a;
    }
    if (r.explorerRestarted) {
        a.text = L"Explorer was restarted after the change, so the taskbar, the Start menu and File Explorer load the current icons. Windows itself does not need a restart.";
        if (r.oldMethodPending) { a.warn = true; a.needsWindowsRestart = true; a.text += L" Icons changed by the older file-swap method still need a restart of Windows."; }
        return a;
    }
    const wchar_t* what = r.removed ? L"The default icons were restored in Windhawk" : L"The theme was applied in Windhawk";
    a.text = what;
    a.text += L" (the setting was written and read back). ";
    if (!r.removed) {   // restoring the default icons needs no mod in Explorer
        if (r.inExplorer == IconShellState::Loaded) a.text += L"Resource Redirect is loaded in Explorer. ";
        else if (r.inExplorer == IconShellState::NotLoaded) {
            a.warn = true;
            a.text += L"Resource Redirect is NOT loaded in Explorer yet, so Explorer cannot show the change: check in Windhawk that the mod is on and covers explorer.exe. ";
        } else { a.warn = true; a.text += L"AnkhCore could not check whether Resource Redirect is loaded in Explorer. "; }
    }
    a.text += r.shellNotified ? L"Windows was told that icons changed. " : L"Windows could not be told that icons changed. ";
    if (!r.shellNotified) a.warn = true;
    a.text += L"Windows that are already open, the taskbar and the Start menu can keep the old icons until they reload; AnkhCore cannot see that. "
              L"Restarting Explorer refreshes them. Windows itself does not need a restart.";
    a.offerExplorerRestart = true;
    if (r.oldMethodPending) { a.text += L" Icons changed by the older file-swap method still need a restart of Windows."; }
    return a;
}

}  // namespace ak
