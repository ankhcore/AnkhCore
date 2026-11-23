// AnkhCore 2.9.5 (Task 3): the default appearance profile of a NEW installation.
// It is the portable part of the owner's current look (theme, glass, Sidebar look, mini widget look, navigation layout) and nothing else:
// no user names, paths, monitors, positions, device or hardware identifiers, and no behaviour switches (enabled, click-through, edge, layer ...).
// It is applied once, only when the settings file has no appearance or Sidebar values yet, and only to keys that are still missing.
// An existing user's settings are never touched; the marker "appearance.profile" is written either way so the decision is made once.
#pragma once
#include <cstdlib>
#include <string>
#include <vector>

namespace ak {

struct ProfileEntry { const wchar_t* key; char type; const wchar_t* value; };   // type: 'i' integer, 'b' boolean, 's' string

inline const std::vector<ProfileEntry>& AppearanceProfile() {
    static const std::vector<ProfileEntry> p = {
        // window look (2.11: the owner's live values, Clear glass)
        {L"appearance.theme", 's', L"neutral-glass"}, {L"appearance.material", 'i', L"4"}, {L"appearance.tint", 'i', L"47"},
        {L"appearance.blur", 'i', L"2"}, {L"appearance.glow", 'i', L"2"}, {L"appearance.motion", 'i', L"1"},
        {L"appearance.bgOpacity", 'i', L"10"}, {L"appearance.bgBlur", 'i', L"2"},
        {L"appearance.clearBg", 'i', L"67"}, {L"appearance.clearBlur", 'i', L"5"}, {L"appearance.clearFrost", 'i', L"88"}, {L"appearance.cardTint", 'i', L"80"},
        {L"appearance.frostedOnBattery", 'b', L"false"}, {L"appearance.widgetGlass", 'b', L"true"},
        {L"appearance.glassDefaults28", 'b', L"true"},   // the 2.8 default migration must not rewrite this theme back
        {L"candy.bgBlur", 'i', L"0"}, {L"candy.bgOpacity", 'i', L"100"},
        // navigation layout
        {L"nav.layout", 'i', L"2"}, {L"nav.compact", 'b', L"false"}, {L"nav.open", 'i', L"-1"},
        // Sidebar: look and behaviour (not the monitor, the import marker or the previous layer)
        {L"sidebar.layout", 'i', L"2"}, {L"sidebar.levels", 's', L"cpu:2,gpu:2,mem:1,storage:1,net:1,fans:1"},
        {L"sidebar.styles", 's', L"cpu:3,gpu:3,mem:3,storage:0,net:3,fans:1"}, {L"sidebar.sections", 's', L"cpu,gpu,mem,storage,net,fans"},
        {L"sidebar.spacing", 'i', L"1"}, {L"sidebar.fontScale", 'i', L"100"}, {L"sidebar.width", 'i', L"210"}, {L"sidebar.heightPct", 'i', L"100"},
        {L"sidebar.valign", 'i', L"0"}, {L"sidebar.opacity", 'i', L"100"}, {L"sidebar.background", 'i', L"2"}, {L"sidebar.tint", 'i', L"30"},
        {L"sidebar.accent", 'i', L"11583199"}, {L"sidebar.accent2", 'i', L"15267071"}, {L"sidebar.textColor", 'i', L"-1"}, {L"sidebar.tintColor", 'i', L"-1"},
        {L"sidebar.glow", 'i', L"1"}, {L"sidebar.animate", 'i', L"1"}, {L"sidebar.preset", 'i', L"0"},
        {L"sidebar.enabled", 'b', L"true"}, {L"sidebar.autoHide", 'i', L"1"}, {L"sidebar.clickThrough", 'b', L"true"}, {L"sidebar.locked", 'i', L"1"},
        {L"sidebar.edge", 'i', L"1"}, {L"sidebar.reserve", 'b', L"false"}, {L"sidebar.layer", 'i', L"3"},
        // mini widget look (not its position, not whether it is on)
        {L"mini.accent", 'i', L"2282478"}, {L"mini.background", 'i', L"2"}, {L"mini.glow", 'i', L"2"}, {L"mini.graphs", 'b', L"true"}, {L"mini.clickThrough", 'b', L"false"},
        {L"mini.layer", 'i', L"1"}, {L"mini.layout", 'i', L"0"}, {L"mini.metrics", 'i', L"255"}, {L"mini.opacity", 'i', L"75"}, {L"mini.scale", 'i', L"125"},
        // monitoring, alerts, wallpaper slideshow, Windhawk page layout
        {L"alert.cpu.crit.enabled", 'b', L"true"}, {L"alert.cpu.crit.threshold", 'i', L"99"}, {L"alert.cpu.warn.threshold", 'i', L"93"},
        {L"monitor.historyMin", 'i', L"15"}, {L"monitor.intervalSec", 'i', L"1"}, {L"monitoring.autoStart", 'b', L"true"}, {L"profile.active", 'i', L"0"},
        {L"wallpaper.intervalMin", 'i', L"360"}, {L"wallpaper.shuffle", 'b', L"true"}, {L"backup.kind", 'i', L"1"}, {L"wh.cat.effects", 'b', L"true"},
    };
    return p;
}

inline bool StartsWithKey(const std::wstring& k, const wchar_t* prefix) { return k.rfind(prefix, 0) == 0; }

// A settings file is "fresh" when nobody has chosen any look yet (a 2.2 migration alone only brings wallpaper and similar values).
template <class Cfg> bool AppearanceSettingsAreFresh(Cfg& cfg) {
    // only a look the person chose counts as an existing user; monitoring, alert and slideshow values (a 2.2 migration brings some) do not
    for (const auto& e : AppearanceProfile()) if ((StartsWithKey(e.key, L"appearance.") || StartsWithKey(e.key, L"sidebar.") || StartsWithKey(e.key, L"mini.") || StartsWithKey(e.key, L"nav.")) && cfg.Has(e.key)) return false;
    return true;
}

// Returns how many values were written. 0 when the profile was decided before, or when this is an existing user's file.
template <class Cfg> int ApplyAppearanceProfileOnce(Cfg& cfg) {
    if (cfg.Has(L"appearance.profile")) return 0;
    const bool fresh = AppearanceSettingsAreFresh(cfg);
    int n = 0;
    if (fresh) {
        for (const auto& e : AppearanceProfile()) {
            if (cfg.Has(e.key)) continue;   // defaults never overwrite a value that exists
            if (e.type == 'b') cfg.SetBool(e.key, std::wstring(e.value) == L"true");
            else if (e.type == 's') cfg.SetStr(e.key, e.value);
            else cfg.SetInt(e.key, _wtoi(e.value));
            ++n;
        }
    }
    cfg.SetStr(L"appearance.profile", fresh ? L"2.9.5-new-install" : L"2.9.5-existing-kept");
    return n;
}

}  // namespace ak
