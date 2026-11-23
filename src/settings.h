// AnkhCore 2.5 — settings store.
// One versioned file per user: %APPDATA%\AnkhCore\settings.json (test build: its own data folder).
// A flat JSON object with dotted keys ("sidebar.width": 170). Values are strings, numbers or booleans.
// - Unknown keys are kept and written back unchanged (newer versions, other tools).
// - Values are range-checked when read: a damaged or edited file can never produce an out-of-range setting.
// - Saved with WriteAtomic: a crash leaves the old or the new file, never a half-written one.
// - First run migrates the 2.2 preferences from HKCU\Software\AnkhCore.
#pragma once
#include "base.h"
#include "settings_migrate.h"
#include <mutex>

namespace ak {

constexpr int kSettingsVersion = 1;

class Settings {
public:
    static Settings& Get() { static Settings s; return s; }

    std::wstring Path() const {
        if (!pathOverride_.empty()) return pathOverride_;   // tests only
        return kTestBuild ? DataDir() + L"\\settings.json" : KnownFolder(FOLDERID_RoamingAppData) + L"\\AnkhCore\\settings.json";
    }
    void Load() {
        std::lock_guard<std::mutex> lk(mx_);
        values_.clear();
        const std::string text = ReadText(Path());
        if (!text.empty() && !Parse(Utf8ToW(text))) {
            // keep the damaged file for inspection and start from defaults
            const std::wstring bad = Path() + L".damaged";
            if (!kTestBuild || InTestArea(bad)) MoveFileExW(Path().c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
            values_.clear();
        }
        if (text.empty()) Migrate22();
        // 2.7 F1: the version is READ and migrated by explicit steps (settings_migrate.h). A newer file is left as it is, nothing is
        // reset silently, and a copy of the file is kept before anything is rewritten.
        SettingsMigrationHooks hooks;
        const std::wstring path = Path();
        hooks.backup = [&](int from, int, std::wstring& where) {
            where.clear();
            if (text.empty() || !FileExists(path)) return true;   // nothing to copy (a damaged file was already kept as .damaged)
            const std::wstring dst = path + L".pre-migration-v" + std::to_wstring(from);
            if (kTestBuild && !InTestArea(dst)) return false;
            if (!CopyFileW(path.c_str(), dst.c_str(), FALSE)) return false;
            where = dst; return true;
        };
        lastMigration_ = SettingsMigrate(values_, kSettingsVersion, SettingsMigrationSteps(), hooks);
        loaded_ = true;
    }
    SettingsMigrationReport LastMigration() { std::lock_guard<std::mutex> lk(mx_); return lastMigration_; }
    void SetPathForTest(const std::wstring& p) { pathOverride_ = p; }
    bool Save() {
        std::lock_guard<std::mutex> lk(mx_);
        std::wstring out = L"{\r\n";
        size_t i = 0;
        for (const auto& [k, v] : values_) out += L"  \"" + Esc(k) + L"\": " + v + (++i < values_.size() ? L",\r\n" : L"\r\n");
        out += L"}\r\n";
        const std::wstring dir = DirOf(Path());
        if (kTestBuild ? InTestArea(Path()) : true) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        const std::string u = WToUtf8(out);
        const bool ok = WriteAtomic(Path(), reinterpret_cast<const uint8_t*>(u.data()), u.size());
        if (!ok) LogE(L"Settings could not be saved to " + Path());
        return ok;
    }

    int Int(const std::wstring& key, int def, int lo, int hi) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = values_.find(key);
        if (it == values_.end()) return def;
        const std::wstring& v = it->second;
        if (v.empty() || !(iswdigit(v[0]) || v[0] == L'-')) return def;
        const int n = _wtoi(v.c_str());
        return n < lo || n > hi ? def : n;
    }
    bool Bool(const std::wstring& key, bool def) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = values_.find(key);
        if (it == values_.end()) return def;
        return it->second == L"true" ? true : it->second == L"false" ? false : def;
    }
    std::wstring Str(const std::wstring& key, const std::wstring& def) {
        std::lock_guard<std::mutex> lk(mx_);
        auto it = values_.find(key);
        if (it == values_.end() || it->second.size() < 2 || it->second.front() != L'"') return def;
        return Unesc(it->second.substr(1, it->second.size() - 2));
    }
    void SetInt(const std::wstring& key, int v) { Put(key, std::to_wstring(v)); }
    void SetBool(const std::wstring& key, bool v) { Put(key, v ? L"true" : L"false"); }
    void SetStr(const std::wstring& key, const std::wstring& v) { Put(key, L"\"" + Esc(v) + L"\""); }
    bool Has(const std::wstring& key) { std::lock_guard<std::mutex> lk(mx_); return values_.count(key) != 0; }
    size_t Count() { std::lock_guard<std::mutex> lk(mx_); return values_.size(); }

    // for tests: parse a document without touching the file
    bool LoadFromText(const std::wstring& text) { std::lock_guard<std::mutex> lk(mx_); values_.clear(); return Parse(text); }
    std::wstring RawForTest(const std::wstring& key) { std::lock_guard<std::mutex> lk(mx_); auto it = values_.find(key); return it == values_.end() ? L"" : it->second; }

private:
    std::map<std::wstring, std::wstring> values_;   // value kept as its JSON literal
    std::mutex mx_;
    bool loaded_ = false;
    SettingsMigrationReport lastMigration_;
    std::wstring pathOverride_;

    void Put(const std::wstring& k, const std::wstring& lit) { std::lock_guard<std::mutex> lk(mx_); values_[k] = lit; }

    static std::wstring Esc(const std::wstring& s) {
        std::wstring o;
        for (wchar_t c : s) {
            if (c == L'"' || c == L'\\') { o += L'\\'; o += c; }
            else if (c == L'\n') o += L"\\n";
            else if (c == L'\r') o += L"\\r";
            else if (c == L'\t') o += L"\\t";
            else if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", c); o += b; }
            else o += c;
        }
        return o;
    }
    static std::wstring Unesc(const std::wstring& s) {
        std::wstring o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != L'\\' || i + 1 >= s.size()) { o += s[i]; continue; }
            const wchar_t c = s[++i];
            if (c == L'n') o += L'\n'; else if (c == L'r') o += L'\r'; else if (c == L't') o += L'\t';
            else if (c == L'u' && i + 4 < s.size()) { o += static_cast<wchar_t>(wcstol(s.substr(i + 1, 4).c_str(), nullptr, 16)); i += 4; }
            else o += c;
        }
        return o;
    }
    // Flat object only: { "key": "str" | number | true | false | null , ... }. Nested values are rejected.
    bool Parse(const std::wstring& t) {
        size_t i = 0;
        auto ws = [&] { while (i < t.size() && iswspace(t[i])) ++i; };
        auto str = [&](std::wstring& raw) -> bool {   // raw literal including quotes
            if (i >= t.size() || t[i] != L'"') return false;
            const size_t a = i++;
            while (i < t.size() && t[i] != L'"') { if (t[i] == L'\\') ++i; ++i; }
            if (i >= t.size()) return false;
            raw = t.substr(a, ++i - a);
            return true;
        };
        ws(); if (i < t.size() && t[i] == 0xFEFF) { ++i; ws(); }
        if (i >= t.size() || t[i++] != L'{') return false;
        ws(); if (i < t.size() && t[i] == L'}') return true;
        for (;;) {
            ws(); std::wstring k;
            if (!str(k)) return false;
            ws(); if (i >= t.size() || t[i++] != L':') return false;
            ws(); std::wstring v;
            if (i < t.size() && t[i] == L'"') { if (!str(v)) return false; }
            else {
                const size_t a = i;
                while (i < t.size() && t[i] != L',' && t[i] != L'}' && !iswspace(t[i])) ++i;
                v = t.substr(a, i - a);
                const bool num = !v.empty() && (iswdigit(v[0]) || v[0] == L'-');
                if (!(num || v == L"true" || v == L"false" || v == L"null")) return false;
            }
            values_[Unesc(k.substr(1, k.size() - 2))] = v;
            ws(); if (i >= t.size()) return false;
            if (t[i] == L',') { ++i; continue; }
            if (t[i] == L'}') return true;
            return false;
        }
    }
    // 2.2 kept two preferences in HKCU\Software\AnkhCore. They are copied once, and the registry is left as it is.
    void Migrate22() {
        const wchar_t* k22 = L"Software\\AnkhCore";
        const RegValue iv = RegGet(HKEY_CURRENT_USER, k22, L"WallpaperIntervalMin");
        if (iv.exists && iv.type == REG_DWORD && iv.data.size() >= 4) values_[L"wallpaper.intervalMin"] = std::to_wstring(*reinterpret_cast<const DWORD*>(iv.data.data()));
        const RegValue sh = RegGet(HKEY_CURRENT_USER, k22, L"WallpaperShuffle");
        if (sh.exists && sh.type == REG_DWORD && sh.data.size() >= 4) values_[L"wallpaper.shuffle"] = *reinterpret_cast<const DWORD*>(sh.data.data()) ? L"true" : L"false";
        values_[L"settings.migratedFrom"] = L"\"2.2\"";
    }
};
inline Settings& Cfg() { return Settings::Get(); }

}  // namespace ak
