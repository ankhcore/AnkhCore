// FT-C01 / FT-C02: the Windhawk CLI side of the staged editor. Everything goes through `windhawk-cli --json` (the interface Windhawk
// 2.x documents), never through its registry keys or its files. Every change is verified by reading it back. `root` is the
// Windhawk app root override: empty for the installed Windhawk, a folder for the isolated roots the tests use.
// In the test build the installed Windhawk can only be READ (list, show, settings get): RunWindhawkCliJson refuses the rest.
#pragma once
#include <array>
#include "eng_icons.h"
#include "wh_stage.h"

namespace ak {

struct WhModInfo { std::string id; std::wstring name, version, author; bool enabled = false; };

inline bool WhCliList(std::vector<WhModInfo>& out, std::wstring& err, const std::wstring& root = L"") {
    out.clear();
    JVal doc; if (!RunWindhawkCliJson(L"mod list", doc, err, root)) return false;
    const JVal* data = doc.Get("data"); const JVal* mods = data ? data->Get("mods") : nullptr;
    if (!mods || !mods->Is(JVal::Arr)) { err = L"the CLI returned no mod list"; return false; }
    for (const JVal& m : mods->a) {
        WhModInfo i; const JVal* id = m.Get("id"); if (!id || !id->Is(JVal::Str)) continue;
        i.id = id->s;
        auto str = [&](const char* k) { const JVal* v = m.Get(k); return v && v->Is(JVal::Str) ? Utf8ToW(v->s) : std::wstring(); };
        i.name = str("name"); if (i.name.empty()) i.name = Utf8ToW(i.id);
        i.version = str("version"); i.author = str("author");
        const JVal* en = m.Get("enabled"); i.enabled = en && en->Is(JVal::Bool) && en->b;
        out.push_back(i);
    }
    return true;
}
inline bool WhCliShow(const std::string& id, JVal& data, std::wstring& err, const std::wstring& root = L"") {
    JVal doc; if (!RunWindhawkCliJson(L"mod show " + QuoteCliArg(Utf8ToW(id)), doc, err, root)) return false;
    const JVal* d = doc.Get("data"); if (!d) { err = L"the CLI returned no data"; return false; }
    data = *d; return true;
}
inline bool WhCliSettings(const std::string& id, WhFlat& flat, std::wstring& err, const std::wstring& root = L"") {
    JVal doc; if (!RunWindhawkCliJson(L"mod settings get " + QuoteCliArg(Utf8ToW(id)), doc, err, root)) return false;
    const JVal* d = doc.Get("data"); if (!d) { err = L"the CLI returned no data"; return false; }
    return WhParseFlat(*d, flat, err);
}
inline bool WhCliEnabled(const std::string& id, bool& enabled, std::wstring& err, const std::wstring& root = L"") {
    std::vector<WhModInfo> mods; if (!WhCliList(mods, err, root)) return false;
    for (const auto& m : mods) if (m.id == id) { enabled = m.enabled; return true; }
    err = L"the mod is not installed"; return false;
}

// Does the mod run inside explorer.exe? From its declared and configured process lists: included by name or by "*", not excluded.
inline bool WhReachesExplorer(const JVal& showData) {
    auto has = [](const JVal* arr, bool star) {
        if (!arr || !arr->Is(JVal::Arr)) return false;
        for (const JVal& v : arr->a) if (v.Is(JVal::Str)) { std::string s; for (char c : v.s) s += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c); if (s == "explorer.exe" || (star && s == "*")) return true; }
        return false;
    };
    const JVal* cfg = showData.Get("config"); const JVal* meta = showData.Get("metadata");
    const JVal* inc = cfg && cfg->Get("include") ? cfg->Get("include") : (meta ? meta->Get("include") : nullptr);
    const JVal* exc = cfg && cfg->Get("exclude") ? cfg->Get("exclude") : (meta ? meta->Get("exclude") : nullptr);
    return has(inc, true) && !has(exc, false);
}

inline std::wstring WhTempJson(const std::string& id) {
    std::wstring n = Utf8ToW(id); for (auto& c : n) if (!iswalnum(c)) c = L'_';
    return DataDir() + L"\\Cache\\wh-settings-" + n + L".json";
}

// The executor the page uses: change, then read back. A settings write is checked key by key: the changed keys hold the new value
// and every other key is unchanged; if not, the previous settings are written back and the error says so.
inline WhExec WhCliExec(const std::wstring& root = L"") {
    WhExec ex;
    ex.setEnabled = [root](const std::string& id, bool enable, std::wstring& err) {
        JVal doc;
        if (!RunWindhawkCliJson(std::wstring(enable ? L"mod enable " : L"mod disable ") + QuoteCliArg(Utf8ToW(id)), doc, err, root, 120000)) return false;
        bool now = false; std::wstring e2;
        if (!WhCliEnabled(id, now, e2, root)) { err = L"changed, but the result could not be read back: " + e2; return false; }
        if (now != enable) { err = L"Windhawk reports the mod is still " + std::wstring(now ? L"on" : L"off"); return false; }
        return true;
    };
    ex.writeSettings = [root](const std::string& id, const std::vector<std::pair<std::string, JVal>>& changes, std::wstring& err) {
        WhFlat before, merged, after;
        if (!WhCliSettings(id, before, err, root)) return false;
        if (!WhMerge(before, changes, merged, err)) return false;
        const std::wstring tmp = WhTempJson(id); EnsureDir(DirOf(tmp));
        auto put = [&](const WhFlat& f) { const std::string js = WhFlatToJson(f); return WriteAll(tmp, reinterpret_cast<const uint8_t*>(js.data()), js.size()); };
        auto run = [&](std::wstring& e) { JVal doc; return RunWindhawkCliJson(L"mod settings write " + QuoteCliArg(Utf8ToW(id)) + L" " + QuoteCliArg(tmp), doc, e, root, 120000); };
        if (!put(merged)) { err = L"could not write the temporary settings file"; return false; }
        std::wstring e1; const bool wrote = run(e1);
        bool good = wrote;
        if (wrote) {
            std::wstring e2;
            if (!WhCliSettings(id, after, e2, root)) { err = L"written, but the result could not be read back: " + e2; good = false; }
            else for (const auto& kv : merged) { const JVal* got = WhFlatFind(after, kv.first); if (!got || !WhValueEquals(*got, kv.second)) { err = L"after writing, " + Utf8ToW(kv.first) + L" does not hold the new value"; good = false; break; } }
            if (good && after.size() != merged.size()) { err = L"after writing, the number of settings changed"; good = false; }
        } else err = e1;
        if (!good && wrote) { std::wstring e3; if (put(before) && run(e3)) err += L" (the previous settings were put back)"; else err += L" (the previous settings could NOT be put back: " + e3 + L")"; }
        DeleteFileW(tmp.c_str());
        return good;
    };
    ex.reachesExplorer = [root](const std::string& id) { JVal d; std::wstring e; return WhCliShow(id, d, e, root) && WhReachesExplorer(d); };
    return ex;
}

// ---------------------------------------------------------------- Grouped Collapsible Windhawk Mod Categories
enum class WhModCategory {
    Effects = 0,
    TransparencyGlass = 1,
    ExplorerDesktop = 2,
    TaskbarStartMenu = 3,
    SystemInterface = 4,
    Count = 5
};

struct WhCategorySpec {
    WhModCategory id;
    const wchar_t* key;       // settings.json persistence suffix ("wh.cat.effects", etc.)
    const wchar_t* icon;      // Segoe MDL2 / Fluent glyph
    const wchar_t* title;     // Category heading
    const wchar_t* subtitle;  // Concise description
};

inline const std::array<WhCategorySpec, static_cast<size_t>(WhModCategory::Count)>& WhCategorySpecs() {
    static const std::array<WhCategorySpec, static_cast<size_t>(WhModCategory::Count)> kSpecs = {{
        {WhModCategory::Effects,           L"wh.cat.effects",      L"\xE771", L"Effects",              L"Windows animations, window transitions, mouse trail, and motion effects."},
        {WhModCategory::TransparencyGlass, L"wh.cat.transparency", L"\xE81E", L"Transparency & Glass", L"Window transparency, acrylic, blur, and glass appearance."},
        {WhModCategory::ExplorerDesktop,   L"wh.cat.explorer",     L"\xEC50", L"Explorer & Desktop",   L"File Explorer styling, tree view, file operations, and window arrangement."},
        {WhModCategory::TaskbarStartMenu,  L"wh.cat.taskbar",      L"\xE75B", L"Taskbar & Start Menu", L"Taskbar behavior, clock, system tray spacing, and Start Menu styling."},
        {WhModCategory::SystemInterface,   L"wh.cat.system",       L"\xE713", L"System & Interface",   L"System UI, Alt+Tab, Notification Center, corner radius, OSD, and icon redirection."},
    }};
    return kSpecs;
}

// Classifies any installed or managed Windhawk mod into its primary logical category.
// 2.8.3: two of the mods AnkhCore ships are MODIFIED versions of upstream mods. Wherever the author is shown, the modification is named too;
// the original author's name and the licence stay. The technical ids are never touched. Pure: matches the id only.
inline bool WhIsModifiedByAnkhCore(const std::wstring& modId) {
    const std::wstring s = Lower(modId);
    return s.find(L"icon-resource-redirect") != std::wstring::npos || s.find(L"windows-11-file-explorer-styler") != std::wstring::npos;
}
inline std::wstring WhModifiedNote(const std::wstring& modId) { return WhIsModifiedByAnkhCore(modId) ? std::wstring(L"Modified by As3aD") : std::wstring(); }

inline WhModCategory ClassifyWindhawkModCategory(const std::wstring& modId, const std::wstring& modName = L"") {
    const std::wstring s = Lower(modId + L" " + modName);
    // A. Effects: animations, transitions, mouse trail, cursor/motion visual effects
    if (s.find(L"animation") != std::wstring::npos ||
        s.find(L"mouse-trail") != std::wstring::npos ||
        s.find(L"mouse trail") != std::wstring::npos ||
        s.find(L"cursor-effect") != std::wstring::npos ||
        s.find(L"motion") != std::wstring::npos) {
        return WhModCategory::Effects;
    }
    // B. Transparency & Glass: translucent windows, acrylic, blur, mica, glass
    if (s.find(L"translucen") != std::wstring::npos ||
        s.find(L"transparen") != std::wstring::npos ||
        s.find(L"acrylic") != std::wstring::npos ||
        s.find(L"glass") != std::wstring::npos ||
        s.find(L"blur") != std::wstring::npos ||
        s.find(L"mica") != std::wstring::npos) {
        return WhModCategory::TransparencyGlass;
    }
    // D. Taskbar & Start Menu (checked before Explorer so taskbar-* and start-menu-* go to Taskbar & Start Menu)
    if (s.find(L"taskbar") != std::wstring::npos ||
        s.find(L"start-menu") != std::wstring::npos ||
        s.find(L"start menu") != std::wstring::npos ||
        s.find(L"start-button") != std::wstring::npos) {
        return WhModCategory::TaskbarStartMenu;
    }
    // C. Explorer & Desktop: File Explorer, tree items, file operations, window arrangement, desktop
    if (s.find(L"explorer") != std::wstring::npos ||
        s.find(L"file-operation") != std::wstring::npos ||
        s.find(L"window-arrangement") != std::wstring::npos ||
        s.find(L"desktop") != std::wstring::npos ||
        s.find(L"folder") != std::wstring::npos) {
        return WhModCategory::ExplorerDesktop;
    }
    // E. System & Interface: Alt+Tab, Notification Center, Dynamic Island, Corner Radius, Icon Redirect, OSD, UxTheme, Power Buttons
    return WhModCategory::SystemInterface;
}

}  // namespace ak
