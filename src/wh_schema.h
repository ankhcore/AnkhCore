// FT-C01: the settings a Windhawk mod declares, read from what the Windhawk CLI itself reports (`mod show --json`:
// data.initialSettings, `mod settings get --json`: data.settings). Nothing is invented: a control exists only for a setting the
// mod declares, its kind comes from the declared default and options, and anything the editor cannot edit (lists, structured
// lists) is reported as such and preserved untouched when the other settings are written. Pure: no Windows calls.
#pragma once
#include "json_min.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ak {

enum class WhKind { Bool, Int, Real, Text, Color, Enum, List, Group };
struct WhOption { std::string value, label; };
struct WhSettingDef {
    std::string path;                 // the flat key as `settings get` prints it: "Group.key"
    std::string name, description;    // as declared (name falls back to the key)
    WhKind kind = WhKind::Text;
    int depth = 0;                    // nesting of the group it is in
    std::vector<WhOption> options;    // Enum
    JVal def;                         // the declared default (not for Group / List)
    int listItems = 0;                // List: how many items the declared default holds
    std::vector<WhSettingDef> itemDefs; // List of structured items (e.g. per-program profiles): schema of one item
};

inline bool WhLooksLikeColor(const std::string& s) {
    if (s.size() != 6 && s.size() != 8) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}
inline bool WhNameHas(const std::string& hay, const char* needle) {
    std::string h; for (char c : hay) h += static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
    return h.find(needle) != std::string::npos;
}

namespace whdetail {
inline void Walk(const JVal& list, const std::string& prefix, int depth, std::vector<WhSettingDef>& out) {
    for (const JVal& e : list.a) {
        const JVal* key = e.Get("key"); const JVal* val = e.Get("value");
        if (!key || !key->Is(JVal::Str) || key->s.empty() || !val) continue;
        WhSettingDef d; d.path = prefix + key->s; d.depth = depth;
        const JVal* nm = e.Get("name"); const JVal* ds = e.Get("description");
        d.name = nm && nm->Is(JVal::Str) && !nm->s.empty() ? nm->s : key->s;
        d.description = ds && ds->Is(JVal::Str) ? ds->s : std::string();
        if (val->Is(JVal::Arr)) {
            const bool group = !val->a.empty() && val->a[0].Is(JVal::Obj) && val->a[0].Get("key") && val->a[0].Get("value");
            if (group) { d.kind = WhKind::Group; out.push_back(d); Walk(*val, d.path + ".", depth + 1, out); }
            else {
                d.kind = WhKind::List;
                d.listItems = static_cast<int>(val->a.size());
                if (!val->a.empty() && val->a[0].Is(JVal::Arr)) Walk(val->a[0], "", 0, d.itemDefs);
                out.push_back(d);
            }
            continue;
        }
        d.def = *val;
        if (const JVal* op = e.Get("options"); op && op->Is(JVal::Arr)) {
            for (const JVal& o : op->a) if (o.Is(JVal::Obj) && !o.keys.empty() && o.a[0].Is(JVal::Str)) d.options.push_back({o.keys[0], o.a[0].s});
        }
        if (val->Is(JVal::Bool)) d.kind = WhKind::Bool;
        else if (val->Is(JVal::Num)) d.kind = (val->n == static_cast<double>(static_cast<long long>(val->n))) ? WhKind::Int : WhKind::Real;
        else if (val->Is(JVal::Str)) {
            if (!d.options.empty()) d.kind = WhKind::Enum;
            else if (WhLooksLikeColor(val->s) && (WhNameHas(d.path, "color") || WhNameHas(d.path, "colour"))) d.kind = WhKind::Color;
            else d.kind = WhKind::Text;
        } else continue;   // null: nothing to edit
        out.push_back(d);
    }
}
}  // namespace whdetail

// `showData` is the "data" object of `mod show --json`.
inline bool WhParseSchema(const JVal& showData, std::vector<WhSettingDef>& out, std::wstring& err) {
    out.clear(); err.clear();
    const JVal* ins = showData.Get("initialSettings");
    if (!ins) { err = L"the mod declares no settings"; return true; }   // a mod without settings is normal
    if (!ins->Is(JVal::Arr)) { err = L"initialSettings is not a list"; return false; }
    whdetail::Walk(*ins, "", 0, out);
    return true;
}

using WhFlat = std::vector<std::pair<std::string, JVal>>;   // `settings get`: flat keys in the mod's own order

inline bool WhParseFlat(const JVal& getData, WhFlat& out, std::wstring& err) {
    out.clear(); err.clear();
    const JVal* st = getData.Get("settings");
    if (!st || !st->Is(JVal::Obj)) { err = L"the CLI returned no settings object"; return false; }
    for (size_t i = 0; i < st->keys.size(); ++i) out.push_back({st->keys[i], st->a[i]});
    return true;
}
inline const JVal* WhFlatFind(const WhFlat& f, const std::string& key) {
    for (const auto& kv : f) if (kv.first == key) return &kv.second;
    return nullptr;
}

// Values: a Windhawk boolean is stored as 1 or 0; true / false in a file are the same thing.
inline bool WhValueEquals(const JVal& a, const JVal& b) {
    auto num = [](const JVal& v, double& d) { if (v.Is(JVal::Num)) { d = v.n; return true; } if (v.Is(JVal::Bool)) { d = v.b ? 1 : 0; return true; } return false; };
    double x, y;
    if (num(a, x) && num(b, y)) return x == y;
    return a.t == b.t && a.t == JVal::Str && a.s == b.s;
}
inline JVal WhMakeNum(double n) { JVal v; v.t = JVal::Num; v.n = n; return v; }
inline JVal WhMakeStr(const std::string& s) { JVal v; v.t = JVal::Str; v.s = s; return v; }
inline bool WhAsBool(const JVal& v) { return v.Is(JVal::Bool) ? v.b : (v.Is(JVal::Num) && v.n != 0); }

// What a value looks like to the user.
inline std::string WhValueText(const JVal& v) {
    if (v.Is(JVal::Str)) return v.s;
    if (v.Is(JVal::Bool)) return v.b ? "on" : "off";
    if (v.Is(JVal::Num)) { char b[40]; if (v.n == static_cast<double>(static_cast<long long>(v.n))) std::snprintf(b, sizeof b, "%lld", static_cast<long long>(v.n)); else std::snprintf(b, sizeof b, "%.10g", v.n); return b; }
    return "";
}
// Text typed by the user -> a value of the declared kind; false when it does not fit (nothing is written then).
inline bool WhParseInput(const WhSettingDef& d, const std::string& text, JVal& out, std::wstring& err) {
    err.clear();
    switch (d.kind) {
    case WhKind::Bool: { const bool on = text == "on" || text == "1" || text == "true"; if (!on && text != "off" && text != "0" && text != "false") { err = L"use on or off"; return false; } out = WhMakeNum(on ? 1 : 0); return true; }
    case WhKind::Int: case WhKind::Real: {
        char* end = nullptr; const double n = std::strtod(text.c_str(), &end);
        if (text.empty() || end == text.c_str() || *end) { err = L"not a number"; return false; }
        if (d.kind == WhKind::Int && n != static_cast<double>(static_cast<long long>(n))) { err = L"a whole number is needed"; return false; }
        out = WhMakeNum(n); return true; }
    case WhKind::Enum: { for (const auto& o : d.options) if (o.value == text) { out = WhMakeStr(text); return true; } err = L"not one of the options"; return false; }
    case WhKind::Color: {
        std::string t = text; if (!t.empty() && t[0] == '#') t.erase(0, 1);
        if (!WhLooksLikeColor(t)) { err = L"a colour is 6 or 8 hex digits (RRGGBB)"; return false; }
        out = WhMakeStr(t); return true; }
    case WhKind::Text: out = WhMakeStr(text); return true;
    default: err = L"this setting cannot be edited here"; return false;
    }
}

// The JSON `mod settings write` takes: every flat key with its (possibly changed) value, in the mod's own order. Keys the
// editor does not touch are written back exactly as read, so unknown and unsupported settings are preserved.
inline std::string WhJsonString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += static_cast<char>(c);
    }
    return o + "\"";
}
inline std::string WhFlatToJson(const WhFlat& f) {
    std::string o = "{";
    for (size_t i = 0; i < f.size(); ++i) {
        if (i) o += ",";
        o += WhJsonString(f[i].first) + ":";
        const JVal& v = f[i].second;
        if (v.Is(JVal::Str)) o += WhJsonString(v.s); else if (v.Is(JVal::Bool)) o += v.b ? "true" : "false"; else if (v.Is(JVal::Num)) o += WhValueText(v); else o += "null";
    }
    return o + "}";
}
// True when `key` is an indexed element of an existing structured list in `current` (e.g. "profiles[2].executable" when "profiles[0].executable" exists).
inline bool WhIsListElementAddition(const WhFlat& current, const std::string& key) {
    const size_t lb = key.find('['), rb = (lb != std::string::npos) ? key.find(']', lb + 1) : std::string::npos;
    if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return false;
    for (size_t i = lb + 1; i < rb; ++i) if (key[i] < '0' || key[i] > '9') return false;
    const std::string item0Key = key.substr(0, lb) + "[0]" + key.substr(rb + 1);
    return WhFlatFind(current, item0Key) != nullptr;
}
// `changes` replace matching keys; a key the mod does not have is an error (nothing invented).
inline bool WhMerge(const WhFlat& current, const std::vector<std::pair<std::string, JVal>>& changes, WhFlat& out, std::wstring& err) {
    out = current; err.clear();
    for (const auto& ch : changes) {
        bool hit = false;
        for (auto& kv : out) if (kv.first == ch.first) { kv.second = ch.second; hit = true; break; }
        if (!hit && WhIsListElementAddition(current, ch.first)) { out.push_back(ch); hit = true; }
        if (!hit) { err = L"the mod has no setting named " + std::wstring(ch.first.begin(), ch.first.end()); return false; }
    }
    return true;
}

}  // namespace ak
