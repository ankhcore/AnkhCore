// AnkhCore 2.5 — Backup & Migration engine (M-BAK2: BAK-1, BAK-2, BAK-3, D27, D30).
// Built on src/acbackup.h and src/json_min.h:
//   - Pure root resolution (appdata, data, content) so test builds resolve everything inside %LOCALAPPDATA%\AnkhCore-Test.
//   - Streamed SHA-256 file hashing (1 MiB chunks via Windows CNG) for large content.
//   - Create and verify .acbackup archives for AnkhCore's own 'settings' and 'monitoring' categories.
//   - Restore in the design's exact order (docs/design/BACKUP_MIGRATION.md §5):
//       1. Validate whole archive before writing anything
//       2. Select categories / settings groups
//       3. Create recovery point under <data>\Recovery\<timestamp> + journal entry (LOG-1)
//       4. Apply ('settings-merge' with range checks & D30 fan-control safety; 'file-copy' with atomic replace & hash check)
//       5. Automatic rollback in reverse order if any step fails, plus restore report.
#pragma once
#include "acbackup.h"
#include "settings.h"
#include "eng_ugbundle.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace ak {

struct AcbRoots {
    std::wstring appdata;
    std::wstring data;
    std::wstring content;
};

inline AcbRoots AcbDefaultRoots() {
    if (kTestBuild) {
        const std::wstring d = DataDir();
        return {d, d, d + L"\\Content"};
    }
    return {
        KnownFolder(FOLDERID_RoamingAppData) + L"\\AnkhCore",
        DataDir(),
        DataDir() + L"\\Content"
    };
}

// Pure destination resolution ("appdata:settings.json" -> "<roots.appdata>\settings.json").
// Returns true and sets outPath = L"" when dest is "none:".
inline bool AcbResolveDest(const AcbRoots& roots, const std::string& dest, std::wstring& outPath) {
    outPath.clear();
    std::string root, rel;
    if (!AcbSplitDest(dest, root, rel)) return false;
    if (root == "none") return true;
    std::wstring base;
    if (root == "appdata") base = roots.appdata;
    else if (root == "data") base = roots.data;
    else if (root == "content") base = roots.content;
    if (base.empty()) return false;
    while (base.size() > 3 && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    std::wstring wrel;
    if (!acbdetail::Wide(rel, wrel) || !SafeRelPath(wrel)) return false;
    for (auto& ch : wrel) if (ch == L'/') ch = L'\\';
    outPath = base + L"\\" + wrel;
    return true;
}

// Streamed SHA-256 and byte size for a disk file (1 MiB buffer; supports files larger than memory).
inline bool AcbHashFile(const std::wstring& path, uint64_t& outSize, std::string& outSha256) {
    outSize = 0;
    outSha256.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0) { CloseHandle(f); return false; }
    outSize = static_cast<uint64_t>(sz.QuadPart);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    uint8_t digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<uint8_t> buf(1u << 20);
    uint64_t done = 0;
    while (ok && done < outSize) {
        const DWORD want = static_cast<DWORD>(std::min<uint64_t>(outSize - done, buf.size()));
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr) || got == 0) { ok = false; break; }
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
        done += got;
    }
    if (ok) ok = BCryptFinishHash(h, digest, sizeof(digest), 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    static const char* hex = "0123456789abcdef";
    outSha256.resize(64);
    for (int i = 0; i < 32; ++i) {
        outSha256[i * 2] = hex[digest[i] >> 4];
        outSha256[i * 2 + 1] = hex[digest[i] & 15];
    }
    return true;
}

// Maps a flat setting key to its logical group id for custom selective backup/restore.
// Groups: "sidebar", "mini", "monitor", "appearance", "fan", "general".
inline std::string AcbSettingGroup(const std::string& key) {
    if (key.compare(0, 8, "sidebar.") == 0) return "sidebar";
    if (key.compare(0, 5, "mini.") == 0) return "mini";
    if (key.compare(0, 8, "monitor.") == 0 || key.compare(0, 6, "alert.") == 0 || key.compare(0, 11, "monitoring.") == 0) return "monitor";
    if (key.compare(0, 11, "appearance.") == 0 || key.compare(0, 10, "wallpaper.") == 0 || key.compare(0, 8, "widgets.") == 0) return "appearance";
    if (key.compare(0, 4, "fan.") == 0) return "fan";
    return "general";
}

// Validates a single setting key/value against AnkhCore's range checks and safety rules.
// Unknown keys pass if they are flat scalar values without control characters.
// Fan keys obey D30 (master §8.5): fan control is never enabled by a restore, and manual speeds are clamped to >= 40%.
inline bool AcbSanitizeSetting(const std::string& key, JVal& val) {
    if (key.empty() || key.size() > 96) return false;
    for (const char c : key) if (static_cast<unsigned char>(c) < 0x20 || c == '"' || c == '\\') return false;
    if (key == "settings.version") { val.t = JVal::Num; val.n = kSettingsVersion; return true; }

    // D30: fan control is never switched on by a restore, and a saved manual mode/speed is never live-enabled.
    if (key == "fan.enabled" || key == "fan.active" || key == "fan.controlEnabled" || key == "fan.manualActive") {
        val.t = JVal::Bool; val.b = false; return true;
    }
    if (key == "fan.mode") {
        if (val.Is(JVal::Str)) { if (val.s == "manual") val.s = "auto"; return true; }
        if (val.Is(JVal::Num)) { if (val.n != 0) val.n = 0; return true; }
        return false;
    }
    if (key == "fan.dutyPct" || key == "fan.manualSpeed" || key == "fan.targetPct") {
        if (!val.Is(JVal::Num) || std::floor(val.n) != val.n) return false;
        val.n = std::clamp(static_cast<int>(val.n), 40, 100);
        return true;
    }

    struct IntRange { const char* k; int lo, hi; };
    static const IntRange kRanges[] = {
        {"sidebar.width", 140, 360}, {"sidebar.heightPct", 25, 100}, {"sidebar.posPct", -1, 100},
        {"sidebar.valign", 0, 2}, {"sidebar.edge", 0, 1}, {"sidebar.layer", 0, 3},
        {"sidebar.layout", 0, 3}, {"sidebar.spacing", 0, 2}, {"sidebar.fontScale", 80, 150},
        {"sidebar.opacity", 15, 100}, {"sidebar.background", 0, 2}, {"sidebar.tint", 0, 100},
        {"sidebar.glow", 0, 2}, {"sidebar.autoHide", 0, 1}, {"sidebar.autoHideSec", 1, 15},
        {"sidebar.netMode", 0, 2}, {"sidebar.locked", 0, 1}, {"sidebar.animate", 0, 1},
        {"sidebar.accent", -1, 0xFFFFFF}, {"sidebar.tempColor", -1, 0xFFFFFF},
        {"sidebar.textColor", -1, 0xFFFFFF}, {"sidebar.tintColor", -1, 0xFFFFFF},
        {"mini.layout", 0, 1}, {"mini.layer", 0, 2}, {"mini.opacity", 15, 100},
        {"mini.width", 0, 520}, {"mini.rowHeight", 0, 48}, {"mini.scale", 75, 175},
        {"mini.background", 0, 2}, {"mini.tint", 0, 100}, {"mini.glow", 0, 2},
        {"mini.accent", -1, 0xFFFFFF}, {"mini.textColor", -1, 0xFFFFFF},
        {"mini.x", -100000, 100000}, {"mini.y", -100000, 100000},
        {"appearance.material", 0, 3}, {"appearance.onBattery", 0, 2},
        {"appearance.tint", 35, 95}, {"appearance.blur", 0, 2},
        {"appearance.glow", 0, 2}, {"appearance.motion", 0, 2},
        {"appearance.bgOpacity", 0, 100}, {"appearance.bgBlur", 0, 30},
        {"candy.bgOpacity", 0, 100}, {"candy.bgBlur", 0, 30}, {"sidebar.netPeriod", 0, 3},
        {"wallpaper.intervalMin", 1, 1440},
        {"alert.cpu.warn.threshold", 60, 105}, {"alert.cpu.crit.threshold", 70, 110},
        {"alert.gpu.warn.threshold", 60, 105}, {"alert.gpu.crit.threshold", 70, 110},
        {"nav.page", 0, 64},
    };
    for (const auto& r : kRanges) {
        if (key == r.k) {
            if (!val.Is(JVal::Num) || std::floor(val.n) != val.n) return false;
            const double v = val.n;
            return v >= r.lo && v <= r.hi;
        }
    }
    if (key == "monitor.intervalSec") {
        if (!val.Is(JVal::Num) || std::floor(val.n) != val.n) return false;
        const int v = static_cast<int>(val.n);
        return v == 1 || v == 2 || v == 5 || v == 10;
    }
    if (key == "monitor.historyMin") {
        if (!val.Is(JVal::Num) || std::floor(val.n) != val.n) return false;
        const int v = static_cast<int>(val.n);
        return v == 5 || v == 15 || v == 60 || v == 240;
    }
    static const char* const kBools[] = {
        "sidebar.enabled", "sidebar.reserve", "sidebar.clickThrough",
        "mini.enabled", "mini.clickThrough", "widgets.glass",
        "wallpaper.shuffle", "monitoring.autoStart", "nav.compact",
        "alert.cpu.warn.enabled", "alert.cpu.crit.enabled",
        "alert.gpu.warn.enabled", "alert.gpu.crit.enabled",
        "alert.fan.stop.enabled",
    };
    for (const char* b : kBools) {
        if (key == b) return val.Is(JVal::Bool);
    }
    if (val.Is(JVal::Str)) {
        if (val.s.size() > 4096) return false;
        for (const char c : val.s) {
            if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>') return false;
        }
        return true;
    }
    return val.Is(JVal::Bool) || (val.Is(JVal::Num) && std::isfinite(val.n));
}

inline std::string AcbFormatJValScalar(const JVal& v) {
    if (v.Is(JVal::Bool)) return v.b ? "true" : "false";
    if (v.Is(JVal::Str)) return "\"" + JsonEsc(v.s) + "\"";
    if (v.Is(JVal::Num)) {
        if (std::floor(v.n) == v.n && std::fabs(v.n) <= 9007199254740992.0)
            return std::to_string(static_cast<long long>(v.n));
        char b[64]; std::snprintf(b, sizeof(b), "%.6g", v.n);
        return b;
    }
    return "null";
}

// Pure settings-merge implementation (design §5 step 5):
// - Keeps existing keys (including unknown keys and unselected groups).
// - Applies incoming keys from selected groups (empty groups = all groups) that pass AcbSanitizeSetting.
// - Enforces D30 on fan.* keys.
inline bool AcbMergeSettings(const std::string& currentJson, const std::string& incomingJson,
                             const std::vector<std::string>& groups, std::string& outJson,
                             int& appliedCount, int& skippedCount, std::wstring& err) {
    appliedCount = 0;
    skippedCount = 0;
    outJson.clear();
    std::map<std::string, std::string> merged;
    if (!currentJson.empty()) {
        JVal cur;
        if (JsonParse(currentJson, cur) && cur.Is(JVal::Obj)) {
            for (size_t i = 0; i < cur.keys.size(); ++i) {
                const JVal& v = cur.a[i];
                if (v.Is(JVal::Bool) || v.Is(JVal::Num) || v.Is(JVal::Str))
                    merged[cur.keys[i]] = AcbFormatJValScalar(v);
            }
        }
    }
    // E1 (2.7): the live file's own version is read BEFORE the merge. A restore is a second writer of settings.json; it must not relabel a
    // file written by a newer version as ours (settings_migrate.h: such a file is left exactly as it is).
    int liveVersion = 0;
    { auto vit = merged.find("settings.version"); if (vit != merged.end()) { const std::string& lit = vit->second; bool plain = !lit.empty() && lit.size() <= 6; int n = 0; for (char c : lit) { if (c < '0' || c > '9') { plain = false; break; } n = n * 10 + (c - '0'); } if (plain) liveVersion = n; } }
    JVal inc;
    if (!JsonParse(incomingJson, inc) || !inc.Is(JVal::Obj)) {
        err = L"The backup's settings.json is not a valid JSON object.";
        return false;
    }
    for (size_t i = 0; i < inc.keys.size(); ++i) {
        const std::string& k = inc.keys[i];
        JVal v = inc.a[i];
        if (!v.Is(JVal::Bool) && !v.Is(JVal::Num) && !v.Is(JVal::Str)) {
            ++skippedCount;
            continue;
        }
        if (!groups.empty()) {
            const std::string g = AcbSettingGroup(k);
            if (std::find(groups.begin(), groups.end(), g) == groups.end()) {
                ++skippedCount;
                continue;
            }
        }
        if (!AcbSanitizeSetting(k, v)) {
            ++skippedCount;
            continue;
        }
        merged[k] = AcbFormatJValScalar(v);
        ++appliedCount;
    }
    merged["settings.version"] = std::to_string(liveVersion > kSettingsVersion ? liveVersion : kSettingsVersion);
    outJson = "{\r\n";
    size_t idx = 0;
    for (const auto& [k, lit] : merged) {
        outJson += "  \"" + JsonEsc(k) + "\": " + lit + (++idx < merged.size() ? ",\r\n" : "\r\n");
    }
    outJson += "}\r\n";
    return true;
}

inline std::string AcbUtcStampNow() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

inline std::wstring AcbRecoveryFolderStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t b[32];
    swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u-%03u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

// ---------------------------------------------------------------- 2.7 A1: category providers (the CREATION side of a backup)
// The format, the verifier, the preview and the restore already know every category in AcbCategories(). What was missing is a single,
// honest place that says, per category, what a backup made by AcbCreateBackup actually contains. Each provider either collects its data
// from AnkhCore-owned locations (never from Windows folders, never following a link) or says plainly that it does not. There is still one
// backup implementation: a provider only fills AcbItems that AcbBuild writes.
enum class AcbSupport { Included, Partial, NotIncluded };
inline const char* AcbSupportLabel(AcbSupport s) { return s == AcbSupport::Included ? "included" : s == AcbSupport::Partial ? "partly included" : "not included"; }

struct AcbCollectResult { std::vector<AcbItem> items; std::vector<std::string> notes; };

struct AcbFolderLimits { size_t maxFiles = 512; uint64_t maxFileBytes = 4ull << 20; uint64_t maxTotalBytes = 64ull << 20; int maxDepth = 3; };

namespace acbdetail {
// Collects the files of `dir` whose extension the category allows, into items stored under rel = <prefix>/<path inside dir>. Links
// (reparse points: junctions, symbolic links) are neither followed nor read; a file that is too big, a name that is not a safe relative
// path and anything beyond the limits is SKIPPED AND REPORTED in `notes`, never silently dropped.
inline void CollectFolder(const std::wstring& dir, const std::wstring& relBase, const AcbCatSpec& cat, const std::string& destPrefix, const AcbFolderLimits& lim,
                          int depth, uint64_t& total, AcbCollectResult& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<WIN32_FIND_DATAW> entries;
    do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) entries.push_back(fd); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(entries.begin(), entries.end(), [](const WIN32_FIND_DATAW& a, const WIN32_FIND_DATAW& b) { return _wcsicmp(a.cFileName, b.cFileName) < 0; });   // a stable order
    for (const WIN32_FIND_DATAW& e : entries) {
        const std::wstring name = e.cFileName, rel = relBase.empty() ? name : relBase + L"/" + name;
        if (e.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { out.notes.push_back("skipped a link: " + WToUtf8(rel)); continue; }
        if (e.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth + 1 > lim.maxDepth) { out.notes.push_back("skipped a folder that is nested too deeply: " + WToUtf8(rel)); continue; }
            CollectFolder(dir + L"\\" + name, rel, cat, destPrefix, lim, depth + 1, total, out);
            continue;
        }
        std::wstring ext; const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) ext = Lower(name.substr(dot));
        if (!AcbExtAllowed(cat, WToUtf8(ext))) continue;   // another kind of file: not part of this category (silently, it was never meant to be)
        const uint64_t size = (static_cast<uint64_t>(e.nFileSizeHigh) << 32) | e.nFileSizeLow;
        if (!SafeRelPath(rel)) { out.notes.push_back("skipped a file whose name is not a safe path: " + WToUtf8(rel)); continue; }
        if (size == 0) { out.notes.push_back("skipped an empty file: " + WToUtf8(rel)); continue; }
        if (size > lim.maxFileBytes) { out.notes.push_back("skipped a file that is too large: " + WToUtf8(rel)); continue; }
        if (out.items.size() >= lim.maxFiles) { out.notes.push_back("stopped at the file limit; further files were not included"); return; }
        if (total + size > lim.maxTotalBytes) { out.notes.push_back("stopped at the size limit; further files were not included"); return; }
        std::vector<uint8_t> data;
        if (!ReadAll(dir + L"\\" + name, data) || data.size() != size) { out.notes.push_back("could not read: " + WToUtf8(rel)); continue; }
        total += size;
        AcbItem it; it.category = cat.id; it.rel = WToUtf8(rel); it.dest = destPrefix + it.rel; it.data = std::move(data);
        out.items.push_back(std::move(it));
    }
}
}  // namespace acbdetail

// Test seam: the limits can be lowered. Production uses the defaults.
inline void AcbCollectCursors(const AcbRoots& roots, AcbCollectResult& out, const AcbFolderLimits& lim = {}) {
    const AcbCatSpec* cat = AcbFindCat("cursors");
    if (!cat) return;
    uint64_t total = 0;
    acbdetail::CollectFolder(roots.content + L"\\Cursors", L"", *cat, "content:Cursors/", lim, 0, total, out);
}

struct AcbCategoryProvider {
    const char* id;                 // an AcbCategories() id
    AcbSupport support;
    const char* limitation;         // plain words, shown to the person and written into the backup's notes
    std::function<void(const AcbRoots&, AcbCollectResult&)> collect;   // empty: nothing is collected for this category
};

inline const std::vector<AcbCategoryProvider>& AcbProviders() {
    static const std::vector<AcbCategoryProvider> v = {
        {"settings", AcbSupport::Included,
         "AnkhCore preferences: Sidebar, Mini Monitor, appearance and material choices, monitoring and alert settings, profiles stored as settings. A restore never switches fan control on (D30).",
         [](const AcbRoots& r, AcbCollectResult& o) {
             std::vector<uint8_t> data;
             if (ReadAll(r.appdata + L"\\settings.json", data) && !data.empty()) o.items.push_back({"settings", "settings.json", "appdata:settings.json", std::move(data)});
         }},
        {"monitoring", AcbSupport::Included, "Network usage totals.",
         [](const AcbRoots& r, AcbCollectResult& o) {
             std::vector<uint8_t> data;
             if (ReadAll(r.data + L"\\net-counters.txt", data) && !data.empty()) o.items.push_back({"monitoring", "net-counters.txt", "data:net-counters.txt", std::move(data)});
         }},
        {"cursors", AcbSupport::Partial,
         "Only cursor files (.cur, .ani) that AnkhCore imported into its own Content\\Cursors folder, with size and count limits. The cursor scheme Windows is using now, and cursor folders you own, are not copied (the choice is in the settings; recovery points keep the scheme from before each change).",
         [](const AcbRoots& r, AcbCollectResult& o) { AcbCollectCursors(r, o); }},
        {"wallpapers", AcbSupport::NotIncluded, "Wallpapers live in the Windows folder or in folders you own, not in AnkhCore's own storage, so they are not copied. The wallpaper choices are in the settings.", {}},
        {"history", AcbSupport::NotIncluded, "The operation journal and logs can contain file paths with your user name; they are not written into a backup until that has been reviewed.", {}},
        {"apps", AcbSupport::NotIncluded, "The application list is written by the UniGetUI bundle export (Application migration), not by this backup. A bundle names packages to reinstall; it is not an offline copy of installers.", {}},
        {"iconpacks", AcbSupport::NotIncluded, "Icon packs are large third-party files and are not backed up. Only the selected icon theme is in the settings.", {}},
        {"themes", AcbSupport::NotIncluded, "Windows themes are system files; applying one is a system change that a generic restore does not do. The theme choice is in the settings.", {}},
        {"windhawk", AcbSupport::NotIncluded, "Windhawk configuration and mod records are not backed up yet (reserved until WH-9).", {}},
        {"tweaks", AcbSupport::NotIncluded, "Tweak records need a mapped, verified registry snapshot, which is not collected yet (2.7 A2). AnkhCore's own tweaks can still be undone from the journal.", {}},
        {"appdata", AcbSupport::NotIncluded, "Other applications' data needs a per-application data map, which does not exist yet (Q5). Nothing of other applications is backed up.", {}},
    };
    return v;
}

struct AcbCoverageRow { std::string id, title; AcbSupport support = AcbSupport::NotIncluded; std::string limitation; };
// One row per category the format knows: what a backup contains and what it does not. A category with no provider is "not included".
inline std::vector<AcbCoverageRow> AcbCoverage() {
    std::vector<AcbCoverageRow> rows;
    for (const AcbCatSpec& c : AcbCategories()) {
        AcbCoverageRow r; r.id = c.id; r.title = c.title; r.limitation = "No provider: this category is not backed up.";
        for (const AcbCategoryProvider& p : AcbProviders()) if (r.id == p.id) { r.support = p.support; r.limitation = p.limitation; }
        rows.push_back(std::move(r));
    }
    return rows;
}

// Collects AnkhCore's own 'settings' and 'monitoring' categories from roots, builds a verified .acbackup,
// writes it atomically to archivePath, and re-verifies the written file on disk.
inline bool AcbCreateBackup(const AcbRoots& roots, const std::wstring& archivePath,
                            const std::string& kind, const std::vector<std::string>& categories,
                            AcbCheck& outVerified, std::wstring& err) {
    outVerified = AcbCheck{};
    if (kTestBuild && !InTestArea(archivePath)) {
        TestRefused(L"backup write " + archivePath);
        err = L"Test build refuses to write backup outside the test sandbox.";
        return false;
    }
    auto wantCat = [&](const std::string& id) {
        if (kind == "settings") return id == "settings" || id == "monitoring";
        if (!categories.empty()) return std::find(categories.begin(), categories.end(), id) != categories.end();
        return true;
    };

    AcbBuildInput in;
    in.kind = (kind == "full" || kind == "settings" || kind == "custom") ? kind : "settings";
    in.created = AcbUtcStampNow();
    in.appVersion = WToUtf8(kVersion);
    in.appLabel = WToUtf8(kVersionLabel);
    const int bNum = std::max(1, _wtoi(RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber").c_str()));
    in.osName = bNum >= 22000 ? "Windows 11" : "Windows 10";
    in.osBuild = bNum;
    in.arch = "x64";

    AcbCollectResult col;
    for (const AcbCategoryProvider& pr : AcbProviders()) {
        if (!wantCat(pr.id)) continue;
        if (pr.collect) pr.collect(roots, col);
        // the backup says what it does NOT hold for a category that was asked for (a "settings" backup asks for two only)
        if (pr.support != AcbSupport::Included) col.notes.push_back(std::string(pr.id) + " (" + AcbSupportLabel(pr.support) + "): " + pr.limitation);
    }
    in.items = std::move(col.items);
    in.notes = std::move(col.notes);
    if (in.items.empty()) {
        err = L"Nothing to back up was found for the chosen categories.";
        return false;
    }

    std::vector<uint8_t> zipBytes;
    if (!AcbBuild(in, zipBytes, err)) return false;
    EnsureDir(DirOf(archivePath));
    if (!WriteAtomic(archivePath, zipBytes.data(), zipBytes.size())) {
        err = L"The backup file could not be written.";
        return false;
    }
    std::vector<uint8_t> diskBytes;
    if (!ReadAll(archivePath, diskBytes)) {
        err = L"The written backup file could not be read back for verification.";
        return false;
    }
    outVerified = AcbVerify(diskBytes.data(), diskBytes.size());
    if (!outVerified.ok) {
        err = outVerified.error;
        return false;
    }
    return true;
}

struct AcbRestoreOptions {
    bool settingsOnly = false;
    std::vector<std::string> categories;
    std::vector<std::string> settingGroups;
    int failAfterFiles = -1;   // for unit tests: >= 0 simulates a failure after N applied files to verify rollback
    std::function<void(const std::wstring&)> beforeRollback;   // for unit tests: runs with the recovery folder just before a rollback starts (to damage what it needs)
};

// ---------------------------------------------------------------- B2: Recovery Point Manifest v2 & Sealing
struct AcbRecItem {
    std::string dest;
    std::string backupPath;   // relative path under recovery dir (e.g. "files\\settings\\settings.json")
    bool existed = false;
    uint64_t size = 0;
    std::string sha256;
    std::string afterSha256;
    uint64_t afterSize = 0;
    bool afterExists = false;
    std::string strategy;     // "settings-merge3" | "file-copy"
};

struct AcbRecManifest {
    int formatVersion = 1;
    std::string kind;         // "restore" | "pre-change"
    std::string created;
    std::string sourceArchive;
    bool sealed = false;
    std::vector<AcbRecItem> items;
};

inline std::wstring AcbInferBackupRelPath(const std::string& dest, const std::string& explicitRel = {}) {
    if (!explicitRel.empty()) {
        std::wstring w = acbdetail::Show(explicitRel);
        for (auto& ch : w) if (ch == L'/') ch = L'\\';
        if (w.find(L"..") == std::wstring::npos && w.find(L':') == std::wstring::npos && !w.empty() && w[0] != L'\\')
            return w;
    }
    if (dest == "appdata:settings.json") return L"files\\settings\\settings.json";
    if (dest == "data:net-counters.txt") return L"files\\monitoring\\net-counters.txt";
    const size_t col = dest.find(':');
    const std::string cat = (col == std::string::npos) ? "files" : dest.substr(0, col);
    std::string tail = (col == std::string::npos) ? dest : dest.substr(col + 1);
    for (auto& ch : tail) if (ch == '/') ch = '\\';
    if (tail.find("..") != std::string::npos) return {};
    return L"files\\" + acbdetail::Show(cat) + L"\\" + acbdetail::Show(tail);
}

inline bool AcbParseRecoveryManifest(const std::string& json, AcbRecManifest& out, std::wstring& err) {
    out = AcbRecManifest{};
    JVal root;
    if (json.empty() || !JsonParse(json, root) || !root.Is(JVal::Obj)) {
        err = L"The recovery manifest is missing or unreadable.";
        return false;
    }
    if (const JVal* fv = root.Get("formatVersion"); fv) {
        if (!fv->Is(JVal::Num) || fv->n < 1 || std::floor(fv->n) != fv->n) {
            err = L"The recovery manifest has an invalid formatVersion.";
            return false;
        }
        out.formatVersion = static_cast<int>(fv->n);
        if (out.formatVersion > 2) {
            err = L"This recovery point was created by a newer version of AnkhCore.";
            return false;
        }
    }
    if (const JVal* k = root.Get("kind"); k && k->Is(JVal::Str)) out.kind = k->s;
    if (const JVal* c = root.Get("created"); c && c->Is(JVal::Str)) out.created = c->s;
    if (const JVal* s = root.Get("sourceArchive"); s && s->Is(JVal::Str)) out.sourceArchive = s->s;
    if (const JVal* sl = root.Get("sealed"); sl && sl->Is(JVal::Bool)) out.sealed = sl->b;
    const JVal* items = root.Get("items");
    if (!items || !items->Is(JVal::Arr)) {
        err = L"The recovery manifest has no items list.";
        return false;
    }
    for (const JVal& it : items->a) {
        if (!it.Is(JVal::Obj)) continue;
        AcbRecItem ri;
        if (const JVal* d = it.Get("dest"); d && d->Is(JVal::Str)) ri.dest = d->s;
        if (ri.dest.empty()) continue;
        if (const JVal* bp = it.Get("backupPath"); bp && bp->Is(JVal::Str)) ri.backupPath = bp->s;
        if (const JVal* ex = it.Get("existed"); ex && ex->Is(JVal::Bool)) ri.existed = ex->b;
        if (const JVal* sz = it.Get("size"); sz && sz->Is(JVal::Num) && sz->n >= 0) ri.size = static_cast<uint64_t>(sz->n);
        if (const JVal* sh = it.Get("sha256"); sh && sh->Is(JVal::Str)) ri.sha256 = sh->s;
        if (const JVal* as = it.Get("afterSha256"); as && as->Is(JVal::Str)) ri.afterSha256 = as->s;
        if (const JVal* az = it.Get("afterSize"); az && az->Is(JVal::Num) && az->n >= 0) ri.afterSize = static_cast<uint64_t>(az->n);
        if (const JVal* ae = it.Get("afterExists"); ae && ae->Is(JVal::Bool)) ri.afterExists = ae->b;
        if (const JVal* st = it.Get("strategy"); st && st->Is(JVal::Str)) ri.strategy = st->s;
        if (ri.strategy.empty()) ri.strategy = (ri.dest == "appdata:settings.json") ? "settings-merge3" : "file-copy";
        out.items.push_back(std::move(ri));
    }
    return true;
}

inline bool AcbWriteRecoveryManifest(const std::wstring& recDir, const AcbRecManifest& m) {
    std::string s = "{\r\n"
                    "  \"formatVersion\": " + std::to_string(m.formatVersion) + ",\r\n"
                    "  \"kind\": \"" + JsonEsc(m.kind) + "\",\r\n"
                    "  \"created\": \"" + JsonEsc(m.created) + "\",\r\n"
                    "  \"sourceArchive\": \"" + JsonEsc(m.sourceArchive) + "\",\r\n"
                    "  \"sealed\": " + (m.sealed ? "true" : "false") + ",\r\n"
                    "  \"items\": [\r\n";
    for (size_t i = 0; i < m.items.size(); ++i) {
        const AcbRecItem& it = m.items[i];
        s += "    { \"dest\": \"" + JsonEsc(it.dest) + "\""
             ", \"backupPath\": \"" + JsonEsc(it.backupPath) + "\""
             ", \"existed\": " + (it.existed ? "true" : "false") +
             ", \"size\": " + std::to_string(it.size) +
             ", \"sha256\": \"" + JsonEsc(it.sha256) + "\""
             ", \"afterExists\": " + (it.afterExists ? "true" : "false") +
             ", \"afterSize\": " + std::to_string(it.afterSize) +
             ", \"afterSha256\": \"" + JsonEsc(it.afterSha256) + "\""
             ", \"strategy\": \"" + JsonEsc(it.strategy) + "\" }" +
             (i + 1 < m.items.size() ? ",\r\n" : "\r\n");
    }
    s += "  ]\r\n}\r\n";
    return WriteAtomic(recDir + L"\\recovery-manifest.json", reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Seals a recovery point after its operation completes by recording afterSha256/afterSize/afterExists
// and saving the "after" settings.json base for 3-way merge. Never fails the parent operation.
inline bool AcbSealRecoveryPoint(const AcbRoots& roots, const std::wstring& recDir) {
    AcbRecManifest m;
    std::wstring err;
    if (!AcbParseRecoveryManifest(ReadText(recDir + L"\\recovery-manifest.json"), m, err)) return false;
    for (AcbRecItem& it : m.items) {
        std::wstring destPath;
        if (!AcbResolveDest(roots, it.dest, destPath) || destPath.empty()) continue;
        if (FileExists(destPath)) {
            uint64_t sz = 0; std::string sha;
            if (!AcbHashFile(destPath, sz, sha)) return false;
            it.afterExists = true;
            it.afterSize = sz;
            it.afterSha256 = sha;
            if (it.strategy == "settings-merge3") {
                std::vector<uint8_t> bytes;
                if (!ReadAll(destPath, bytes)) return false;
                EnsureDir(recDir + L"\\after");
                if (!WriteAtomic(recDir + L"\\after\\settings.json", bytes.data(), bytes.size())) return false;
            }
        } else {
            it.afterExists = false;
            it.afterSize = 0;
            it.afterSha256.clear();
        }
    }
    m.formatVersion = 2;
    m.sealed = true;
    return AcbWriteRecoveryManifest(recDir, m);
}

struct AcbRestoreReport {
    bool ok = false;
    bool rolledBack = false;           // every touched file was put back and checked against the recovery copy's SHA-256
    bool rollbackIncomplete = false;   // a rollback was needed and at least one file could NOT be put back (rollbackFailures); the originals stay in recoveryDir
    std::vector<std::wstring> rollbackFailures;
    int rollbackPutBack = 0;           // touched files that were put back and checked (with rollbackFailures: a partial rollback is visible as N of M)
    std::wstring error;
    std::wstring recoveryDir;
    std::wstring reportPath;
    int filesRestored = 0;
    int settingsApplied = 0;
    int settingsSkipped = 0;
    std::vector<std::wstring> restoredItems;
    std::vector<std::wstring> skippedItems;
};

// Restores a .acbackup archive into roots in the design's exact order (validate -> select -> recovery point -> apply -> report / rollback).
inline AcbRestoreReport AcbRestoreBackup(const AcbRoots& roots, const std::wstring& archivePath, const AcbRestoreOptions& opts = {}) {
    AcbRestoreReport rep;
    std::vector<uint8_t> raw;
    if (!ReadAll(archivePath, raw)) {
        rep.error = L"The backup archive could not be read.";
        return rep;
    }
    // 1. Validate the whole archive before anything is written.
    const AcbCheck chk = AcbVerify(raw.data(), raw.size());
    if (!chk.ok) {
        rep.error = chk.error;
        return rep;
    }
    for (const auto& u : chk.unsupported) {
        rep.skippedItems.push_back(L"Unsupported category skipped: " + acbdetail::Show(u));
    }

    // 2. Select restorable files.
    const std::vector<AcbFile> selected = AcbSelect(chk.manifest, opts.categories, opts.settingsOnly);
    if (selected.empty()) {
        rep.error = L"No restorable files matched the selection.";
        return rep;
    }

    // Resolve all destination paths upfront and verify sandbox safety before creating recovery point.
    struct PlanItem {
        AcbFile file;
        std::wstring destPath;
        std::wstring backupCopyPath;
        bool existedBefore = false;
        std::string shaBefore;   // SHA-256 of the recovery copy (what a rollback must put back)
    };
    std::vector<PlanItem> plan;
    for (const auto& f : selected) {
        std::wstring destPath;
        if (!AcbResolveDest(roots, f.dest, destPath)) {
            rep.error = L"Unsafe destination path in backup: " + acbdetail::Show(f.dest);
            return rep;
        }
        if (destPath.empty()) {
            rep.skippedItems.push_back(L"Reference/info entry not written to disk: " + acbdetail::Show(f.path));
            continue;
        }
        if (kTestBuild && !InTestArea(destPath)) {
            TestRefused(L"restore write " + destPath);
            rep.error = L"Test build refuses to restore outside the test sandbox.";
            return rep;
        }
        PlanItem pi;
        pi.file = f;
        pi.destPath = destPath;
        pi.existedBefore = FileExists(destPath);
        plan.push_back(std::move(pi));
    }

    if (plan.empty()) {
        rep.ok = true;
        return rep;
    }

    // 3. Create recovery point under <roots.data>\Recovery\<timestamp> and record a journal entry (LOG-1).
    rep.recoveryDir = roots.data + L"\\Recovery\\" + AcbRecoveryFolderStamp();
    if (kTestBuild && !InTestArea(rep.recoveryDir + L"\\x")) {
        TestRefused(L"recovery dir " + rep.recoveryDir);
        rep.error = L"Test build refuses to create recovery point outside the test sandbox.";
        return rep;
    }
    EnsureDir(rep.recoveryDir + L"\\files");

    AcbRecManifest recMan;
    recMan.formatVersion = 2;
    recMan.kind = "restore";
    recMan.created = AcbUtcStampNow();
    recMan.sourceArchive = WToUtf8(NameOf(archivePath));
    recMan.sealed = false;
    for (size_t i = 0; i < plan.size(); ++i) {
        PlanItem& pi = plan[i];
        std::wstring relName = acbdetail::Show(pi.file.path);
        for (auto& ch : relName) if (ch == L'/') ch = L'\\';
        pi.backupCopyPath = rep.recoveryDir + L"\\" + relName;
        EnsureDir(DirOf(pi.backupCopyPath));
        std::string shaBefore;
        uint64_t sizeBefore = 0;
        if (pi.existedBefore) {
            std::vector<uint8_t> curBytes;
            if (!ReadAll(pi.destPath, curBytes) || !WriteAtomic(pi.backupCopyPath, curBytes.data(), curBytes.size()) ||
                !AcbHashFile(pi.backupCopyPath, sizeBefore, shaBefore)) {
                rep.error = L"Could not save recovery copy of \"" + NameOf(pi.destPath) + L"\".";
                return rep;
            }
            pi.shaBefore = shaBefore;
        }
        const AcbCatSpec* spec = AcbFindCat(pi.file.category);
        AcbRecItem ri;
        ri.dest = pi.file.dest;
        ri.backupPath = WToUtf8(relName);
        ri.existed = pi.existedBefore;
        ri.size = sizeBefore;
        ri.sha256 = shaBefore;
        ri.strategy = (spec && spec->restore == "settings-merge") ? "settings-merge3" : "file-copy";
        recMan.items.push_back(std::move(ri));
    }
    if (!AcbWriteRecoveryManifest(rep.recoveryDir, recMan)) {
        rep.error = L"Could not write recovery point manifest.";
        return rep;
    }
    if (!kTestBuild || InTestArea(JournalPath())) {
        JournalAdd(L"Backup", L"RECOVERY_POINT", L"Recovery point before backup restore",
                   {rep.recoveryDir, NameOf(archivePath)});
    }

    // Helper to roll back already-touched files in reverse order.
    std::vector<size_t> touchedIndices;
    // A rollback is only reported as done when every touched file really is back: a recovery copy that no longer matches its SHA-256 is not
    // used, a write is read back and hashed, a file the restore created is checked to be gone. Anything else is listed (rollbackFailures)
    // and the report says ROLLBACK INCOMPLETE, so nobody believes the files are as they were when they are not.
    auto rollback = [&]() {
        if (opts.beforeRollback) opts.beforeRollback(rep.recoveryDir);
        for (auto it = touchedIndices.rbegin(); it != touchedIndices.rend(); ++it) {
            const PlanItem& pi = plan[*it];
            bool back = false;
            if (pi.existedBefore) {
                uint64_t cSz = 0; std::string cSha; std::vector<uint8_t> orig;
                if (AcbHashFile(pi.backupCopyPath, cSz, cSha) && cSha == pi.shaBefore && ReadAll(pi.backupCopyPath, orig) && WriteAtomic(pi.destPath, orig.data(), orig.size())) {
                    uint64_t dSz = 0; std::string dSha;
                    back = AcbHashFile(pi.destPath, dSz, dSha) && dSha == pi.shaBefore;
                }
            } else {
                if (FileExists(pi.destPath)) DeleteFileW(pi.destPath.c_str());
                back = !FileExists(pi.destPath);
            }
            if (back) ++rep.rollbackPutBack; else rep.rollbackFailures.push_back(NameOf(pi.destPath));
        }
        if (rep.rollbackFailures.empty()) rep.rolledBack = true;
        else {
            rep.rollbackIncomplete = true;
            std::wstring names; for (const auto& n : rep.rollbackFailures) names += (names.empty() ? L"" : L", ") + n;
            rep.error += (rep.error.empty() ? L"" : L" ") + std::wstring(L"ROLLBACK INCOMPLETE (") + std::to_wstring(rep.rollbackPutBack) + L" of " + std::to_wstring(rep.rollbackPutBack + static_cast<int>(rep.rollbackFailures.size())) +
                         L" file(s) put back): could not put back " + names + L". The original files are kept in " + rep.recoveryDir + L".";
        }
    };

    // 4. Apply each item according to its category's restore action.
    for (size_t i = 0; i < plan.size(); ++i) {
        const PlanItem& pi = plan[i];
        const AcbCatSpec* spec = AcbFindCat(pi.file.category);
        if (!spec) continue;
        std::vector<uint8_t> content;
        if (!AcbExtract(raw.data(), raw.size(), chk, pi.file, content, rep.error)) {
            rollback();
            break;
        }
        EnsureDir(DirOf(pi.destPath));
        const std::string action = spec->restore;
        if (action == "settings-merge") {
            const std::string curText = pi.existedBefore ? ReadText(pi.destPath) : std::string();
            const std::string incText(content.begin(), content.end());
            std::string mergedText;
            int appN = 0, skipN = 0;
            if (!AcbMergeSettings(curText, incText, opts.settingGroups, mergedText, appN, skipN, rep.error)) {
                rollback();
                break;
            }
            if (!WriteAtomic(pi.destPath, reinterpret_cast<const uint8_t*>(mergedText.data()), mergedText.size())) {
                rep.error = L"Failed to write merged settings.json.";   // WriteAtomic leaves the old file in place on failure: this one was not touched
                rollback();
                break;
            }
            touchedIndices.push_back(i);   // touched = really modified, only after the write succeeded
            rep.settingsApplied += appN;
            rep.settingsSkipped += skipN;
            ++rep.filesRestored;
            rep.restoredItems.push_back(L"settings-merge: " + acbdetail::Show(pi.file.path) +
                                        L" (" + std::to_wstring(appN) + L" applied, " + std::to_wstring(skipN) + L" skipped)");
        } else if (action == "file-copy") {
            const std::wstring tmpPath = pi.destPath + L".acb-tmp";
            if (!WriteAllFlushed(tmpPath, content.data(), content.size())) {   // flushed: the data is on disk before the rename below replaces the live file
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Failed to stage restored file \"" + NameOf(pi.destPath) + L"\".";
                rollback();
                break;
            }
            uint64_t diskSz = 0;
            std::string diskSha;
            if (!AcbHashFile(tmpPath, diskSz, diskSha) || diskSz != pi.file.size || diskSha != pi.file.sha256) {
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Staged file \"" + NameOf(pi.destPath) + L"\" failed SHA-256 verification.";
                rollback();
                break;
            }
            if (!MoveFileExW(tmpPath.c_str(), pi.destPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Failed to replace \"" + NameOf(pi.destPath) + L"\".";   // the live file was not replaced: it is not touched
                rollback();
                break;
            }
            touchedIndices.push_back(i);   // touched = really replaced, only after the swap succeeded
            ++rep.filesRestored;
            rep.restoredItems.push_back(L"file-copy: " + acbdetail::Show(pi.file.path));
        }
        if (opts.failAfterFiles >= 0 && rep.filesRestored >= opts.failAfterFiles) {
            rep.error = L"Simulated failure after " + std::to_wstring(rep.filesRestored) + L" file(s) for rollback verification.";
            rollback();
            break;
        }
    }

    rep.ok = !rep.rolledBack && rep.error.empty();
    if (rep.ok) {
        if (!AcbSealRecoveryPoint(roots, rep.recoveryDir)) {
            LogW(L"Recovery point could not be sealed; it will remain treated as an unsealed older point.");
        }
    }

    // 5. Write restore report beside the recovery point.
    rep.reportPath = rep.recoveryDir + L"\\restore-report.txt";
    std::wstring rpt = L"AnkhCore 2.5 Backup Restore Report\r\n";
    rpt += L"Status: " + std::wstring(rep.ok ? L"SUCCESS" : (rep.rolledBack ? L"ROLLED BACK" : (rep.rollbackIncomplete ? L"ROLLBACK INCOMPLETE" : L"FAILED"))) + L"\r\n";
    if (!rep.error.empty()) rpt += L"Error: " + rep.error + L"\r\n";
    rpt += L"Recovery point: " + rep.recoveryDir + L"\r\n";
    for (const auto& s : rep.rollbackFailures) rpt += L"NOT PUT BACK: " + s + L"\r\n";
    for (const auto& s : rep.restoredItems) rpt += L"RESTORED: " + s + L"\r\n";
    for (const auto& s : rep.skippedItems) rpt += L"SKIPPED: " + s + L"\r\n";
    const std::string rptU = WToUtf8(rpt);
    WriteAtomic(rep.reportPath, reinterpret_cast<const uint8_t*>(rptU.data()), rptU.size());
    return rep;
}

// ---------------------------------------------------------------- M-BAK3: UniGetUI detection, bundle & inventory
enum class AcbUniGetExit {
    Success = 0,          // 0: command succeeded
    CommandFailed = 1,    // 1: command failed
    InvalidParam = 2,     // 2: invalid parameter (feature unsupported by this build)
    IpcUnavailable = 3,   // 3: no running UniGetUI session reachable
    UnknownCommand = 4,   // 4: unknown automation command (feature unsupported by this build)
    OtherError = 5
};

inline AcbUniGetExit AcbClassifyUniGetExit(DWORD code) {
    switch (code) {
    case 0: return AcbUniGetExit::Success;
    case 1: return AcbUniGetExit::CommandFailed;
    case 2: return AcbUniGetExit::InvalidParam;
    case 3: return AcbUniGetExit::IpcUnavailable;
    case 4: return AcbUniGetExit::UnknownCommand;
    default: return AcbUniGetExit::OtherError;
    }
}

struct AcbUniGetInstall {
    bool found = false;
    bool portable = false;
    std::wstring dir;
    std::wstring guiExe;   // UniGetUI.exe
    std::wstring cliExe;   // uniget.exe (must sit beside UniGetUI.exe, design §2)
};

// Detects an installed or user-configured UniGetUI folder where uniget.exe sits beside UniGetUI.exe.
inline AcbUniGetInstall AcbDetectUniGetUI(const std::vector<std::wstring>& extraDirs = {}) {
    std::vector<std::wstring> dirs = extraDirs;
    if (extraDirs.empty()) {
        const std::wstring cfgDir = Cfg().Str(L"backup.unigetDir", L"");
        if (!cfgDir.empty()) dirs.push_back(cfgDir);
        if (!kTestBuild) {
            if (UgCopyReady(UgBundledSource(), UgUserCopy())) dirs.push_back(UgUserCopy());   // 2.7: the UniGetUI that ships with AnkhCore (a private working copy), preferred over any other
            dirs.push_back(KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\UniGetUI");
            dirs.push_back(KnownFolder(FOLDERID_ProgramFiles) + L"\\UniGetUI");
        }
    }
    for (std::wstring d : dirs) {
        while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        if (d.empty()) continue;
        const std::wstring gui = d + L"\\UniGetUI.exe";
        const std::wstring cli = d + L"\\uniget.exe";
        if (FileExists(gui) && FileExists(cli)) {
            AcbUniGetInstall res;
            res.found = true;
            res.portable = FileExists(d + L"\\ForceUniGetUIPortable");
            res.dir = d;
            res.guiExe = gui;
            res.cliExe = cli;
            return res;
        }
    }
    return {};
}

// SerializableBundle (export_version 3, docs/design/BACKUP_MIGRATION.md §2)
struct AcbPackage {
    std::string id;
    std::string name;
    std::string version;
    std::string source;
    std::string managerName;
};

struct AcbIncompatPackage {
    std::string id;
    std::string name;
    std::string version;
    std::string source;
};

struct AcbUbundle {
    int exportVersion = 3;
    std::vector<AcbPackage> packages;
    std::string incompatInfo = "Cannot be installed automatically; install manually.";
    std::vector<AcbIncompatPackage> incompatible;
};

inline bool AcbParseUbundle(const std::string& jsonText, AcbUbundle& out, std::wstring& err) {
    out = AcbUbundle{};
    JVal r;
    if (!JsonParse(jsonText, r) || !r.Is(JVal::Obj)) {
        err = L"The package bundle is not a valid JSON object.";
        return false;
    }
    uint64_t ev = 0;
    if (!r.Get("export_version") || !r.Get("export_version")->AsUInt(ev) || ev < 1 || ev > 3) {
        err = L"Unsupported UniGetUI bundle export_version.";
        return false;
    }
    out.exportVersion = static_cast<int>(ev);
    const auto str = [](const JVal* o, const char* k, std::string& s) {
        const JVal* v = o ? o->Get(k) : nullptr;
        if (!v || !v->Is(JVal::Str)) return false;
        s = v->s;
        return true;
    };
    if (const JVal* pk = r.Get("packages")) {
        if (!pk->Is(JVal::Arr)) { err = L"Invalid 'packages' array in bundle."; return false; }
        for (const auto& item : pk->a) {
            if (!item.Is(JVal::Obj)) { err = L"Invalid package entry in bundle."; return false; }
            AcbPackage p;
            if (!str(&item, "Id", p.id) || !str(&item, "Name", p.name) ||
                !str(&item, "Version", p.version) || !str(&item, "Source", p.source) ||
                !str(&item, "ManagerName", p.managerName) || p.id.empty() || p.name.empty()) {
                err = L"Package entry in bundle is missing required fields.";
                return false;
            }
            out.packages.push_back(std::move(p));
        }
    }
    if (const JVal* info = r.Get("incompatible_packages_info"); info && info->Is(JVal::Str)) {
        out.incompatInfo = info->s;
    }
    if (const JVal* ic = r.Get("incompatible_packages")) {
        if (!ic->Is(JVal::Arr)) { err = L"Invalid 'incompatible_packages' array in bundle."; return false; }
        for (const auto& item : ic->a) {
            if (!item.Is(JVal::Obj)) { err = L"Invalid incompatible_package entry in bundle."; return false; }
            AcbIncompatPackage p;
            if (!str(&item, "Id", p.id) || !str(&item, "Name", p.name) ||
                !str(&item, "Version", p.version) || !str(&item, "Source", p.source) || p.name.empty()) {
                err = L"Incompatible package entry in bundle is missing required fields.";
                return false;
            }
            out.incompatible.push_back(std::move(p));
        }
    }
    return true;
}

inline std::string AcbBuildUbundle(const AcbUbundle& b) {
    const auto q = [](const std::string& s) { return "\"" + JsonEsc(s) + "\""; };
    std::string j = "{\r\n  \"export_version\": " + std::to_string(b.exportVersion) + ",\r\n  \"packages\": [";
    for (size_t i = 0; i < b.packages.size(); ++i) {
        const auto& p = b.packages[i];
        j += std::string(i ? "," : "") + "\r\n    { \"Id\": " + q(p.id) + ", \"Name\": " + q(p.name) +
             ", \"Version\": " + q(p.version) + ", \"Source\": " + q(p.source) +
             ", \"ManagerName\": " + q(p.managerName) + ", \"InstallationOptions\": {}, \"Updates\": {} }";
    }
    j += "\r\n  ],\r\n  \"incompatible_packages_info\": " + q(b.incompatInfo) + ",\r\n  \"incompatible_packages\": [";
    for (size_t i = 0; i < b.incompatible.size(); ++i) {
        const auto& p = b.incompatible[i];
        j += std::string(i ? "," : "") + "\r\n    { \"Id\": " + q(p.id) + ", \"Name\": " + q(p.name) +
             ", \"Version\": " + q(p.version) + ", \"Source\": " + q(p.source) + " }";
    }
    j += "\r\n  ]\r\n}\r\n";
    return j;
}

// Builds files/apps/inventory.json combining UniGetUI bundle entries with registry-enumerated desktop programs
// (marking programs no package manager knows as "manual", design §6).
inline std::string AcbBuildAppInventoryJson(const AcbUbundle& bundle,
                                            const std::vector<std::pair<std::string, std::string>>& desktopApps) {
    const auto q = [](const std::string& s) { return "\"" + JsonEsc(s) + "\""; };
    std::set<std::string> seenNames;
    std::string j = "{\r\n  \"format\": \"ankhcore-app-inventory\",\r\n  \"version\": 1,\r\n  \"applications\": [";
    bool first = true;
    for (const auto& p : bundle.packages) {
        seenNames.insert(p.name);
        j += std::string(first ? "" : ",") + "\r\n    { \"id\": " + q(p.id) + ", \"name\": " + q(p.name) +
             ", \"version\": " + q(p.version) + ", \"source\": " + q(p.source) +
             ", \"manager\": " + q(p.managerName) + ", \"installable\": true }";
        first = false;
    }
    for (const auto& p : bundle.incompatible) {
        seenNames.insert(p.name);
        j += std::string(first ? "" : ",") + "\r\n    { \"id\": " + q(p.id) + ", \"name\": " + q(p.name) +
             ", \"version\": " + q(p.version) + ", \"source\": " + q(p.source) +
             ", \"manager\": \"manual\", \"installable\": false }";
        first = false;
    }
    for (const auto& [name, ver] : desktopApps) {
        if (name.empty() || seenNames.count(name)) continue;
        seenNames.insert(name);
        j += std::string(first ? "" : ",") + "\r\n    { \"id\": \"\", \"name\": " + q(name) +
             ", \"version\": " + q(ver) + ", \"source\": \"Local PC\", \"manager\": \"manual\", \"installable\": false }";
        first = false;
    }
    j += "\r\n  ]\r\n}\r\n";
    return j;
}

// ---------------------------------------------------------------- M-BAK4: Application restore plan & safe CLI builder
enum class AcbAppRestoreAction {
    AlreadyInstalled,       // Installed on this PC -> offer to restore supported application data
    OfferUniGetInstall,     // Not installed, has package reference & manager available -> offer install with confirmation
    ManualInstallRequired   // Not installed, no usable reference -> show the master's fixed message (§6)
};

constexpr const wchar_t* kAcbManualInstallMsg =
    L"This application requires installation before its data and settings can be restored. "
    L"Please install it first, then run Restore Application Data again.";

struct AcbAppRestoreItem {
    std::string id;
    std::string name;
    std::string backupVersion;
    std::string candidateVersion;
    std::string source;
    std::string managerName;
    AcbAppRestoreAction action = AcbAppRestoreAction::ManualInstallRequired;
    bool versionMismatch = false;   // true when candidateVersion != backupVersion (requires explicit user confirmation; no silent downgrade)
    std::wstring message;
};

inline std::vector<AcbAppRestoreItem> AcbPlanAppRestore(const AcbUbundle& bundle,
                                                        const std::vector<AcbPackage>& installedOnPc,
                                                        const std::vector<AcbPackage>& catalogCandidates,
                                                        const std::vector<std::string>& availableManagers) {
    std::vector<AcbAppRestoreItem> out;
    auto isInstalled = [&](const std::string& id, const std::string& name) {
        for (const auto& inst : installedOnPc) {
            if ((!id.empty() && inst.id == id) || (!name.empty() && inst.name == name)) return true;
        }
        return false;
    };
    auto hasMgr = [&](const std::string& m) {
        return !m.empty() && std::find(availableManagers.begin(), availableManagers.end(), m) != availableManagers.end();
    };
    auto findCandidateVer = [&](const std::string& id, const std::string& fallbackVer) {
        for (const auto& c : catalogCandidates) {
            if (c.id == id && !c.version.empty()) return c.version;
        }
        return fallbackVer;
    };

    for (const auto& p : bundle.packages) {
        AcbAppRestoreItem it;
        it.id = p.id;
        it.name = p.name;
        it.backupVersion = p.version;
        it.source = p.source;
        it.managerName = p.managerName;
        if (isInstalled(p.id, p.name)) {
            it.action = AcbAppRestoreAction::AlreadyInstalled;
            it.candidateVersion = p.version;
            it.message = L"Installed on this PC — ready to restore supported application data.";
        } else if (!p.id.empty() && hasMgr(p.managerName)) {
            it.action = AcbAppRestoreAction::OfferUniGetInstall;
            it.candidateVersion = findCandidateVer(p.id, p.version);
            it.versionMismatch = (!it.candidateVersion.empty() && !p.version.empty() && it.candidateVersion != p.version);
            it.message = it.versionMismatch
                ? L"Available through " + acbdetail::Show(p.managerName) + L" (version " + acbdetail::Show(it.candidateVersion) +
                  L" vs backed-up " + acbdetail::Show(p.version) + L" — confirm before installing)."
                : L"Available to install through " + acbdetail::Show(p.managerName) + L" (" + acbdetail::Show(p.source) + L").";
        } else {
            it.action = AcbAppRestoreAction::ManualInstallRequired;
            it.message = kAcbManualInstallMsg;
        }
        out.push_back(std::move(it));
    }
    for (const auto& p : bundle.incompatible) {
        AcbAppRestoreItem it;
        it.id = p.id;
        it.name = p.name;
        it.backupVersion = p.version;
        it.source = p.source;
        it.managerName = "manual";
        if (isInstalled(p.id, p.name)) {
            it.action = AcbAppRestoreAction::AlreadyInstalled;
            it.candidateVersion = p.version;
            it.message = L"Installed on this PC — ready to restore supported application data.";
        } else {
            it.action = AcbAppRestoreAction::ManualInstallRequired;
            it.message = kAcbManualInstallMsg;
        }
        out.push_back(std::move(it));
    }
    return out;
}

// Builds safe command-line arguments for `uniget.exe package install ...` (design §2):
// - Never passes --skip-hash.
// - Passes --elevated and --interactive only on explicit user choice.
// - Always passes --wait true so AnkhCore follows per-package completion and can stop on cancel.
inline std::wstring AcbBuildUniGetInstallArgs(const AcbPackage& pkg, const std::string& versionToUse,
                                              bool userChoseElevated, bool userChoseInteractive) {
    auto clean = [](const std::string& s) {
        std::wstring w = acbdetail::Show(s);
        w.erase(std::remove(w.begin(), w.end(), L'"'), w.end());
        return w;
    };
    std::wstring args = L"package install --id \"" + clean(pkg.id) + L"\"";
    if (!pkg.managerName.empty()) args += L" --manager \"" + clean(pkg.managerName) + L"\"";
    if (!pkg.source.empty()) args += L" --source \"" + clean(pkg.source) + L"\"";
    const std::string ver = !versionToUse.empty() ? versionToUse : pkg.version;
    if (!ver.empty()) args += L" --version \"" + clean(ver) + L"\"";
    args += std::wstring(L" --elevated ") + (userChoseElevated ? L"true" : L"false");
    args += std::wstring(L" --interactive ") + (userChoseInteractive ? L"true" : L"false");
    args += L" --wait true";
    return args;
}

// ---------------------------------------------------------------- M-BAK5: Recovery points & backup folder enumeration
struct AcbRecoveryPointInfo {
    std::wstring id;
    std::wstring dir;
    std::wstring created;
    std::wstring sourceArchive;
    std::wstring status;
    size_t fileCount = 0;
    uint64_t bytesOnDisk = 0;
    bool sealed = false;
    bool interrupted = false;
    std::wstring restoredSummary;
};

inline std::vector<AcbRecoveryPointInfo> AcbListRecoveryPoints(const AcbRoots& roots) {
    std::vector<AcbRecoveryPointInfo> out;
    const std::wstring recRoot = roots.data + L"\\Recovery";
    if (!DirExists(recRoot)) return out;
    const auto dirs = ListDir(recRoot, true);
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        const std::wstring d = recRoot + L"\\" + *it;
        const std::string manText = ReadText(d + L"\\recovery-manifest.json");
        JVal r;
        if (manText.empty() || !JsonParse(manText, r) || !r.Is(JVal::Obj)) continue;
        AcbRecoveryPointInfo info;
        info.id = *it;
        info.dir = d;
        if (const JVal* c = r.Get("created"); c && c->Is(JVal::Str)) info.created = acbdetail::Show(c->s);
        if (const JVal* s = r.Get("sourceArchive"); s && s->Is(JVal::Str)) info.sourceArchive = acbdetail::Show(s->s);
        if (const JVal* sl = r.Get("sealed"); sl && sl->Is(JVal::Bool)) info.sealed = sl->b;
        if (const JVal* items = r.Get("items"); items && items->Is(JVal::Arr)) info.fileCount = items->a.size();
        info.bytesOnDisk = FolderSize(d);
        info.interrupted = FileExists(d + L"\\restore-in-progress.marker");
        const std::string rptText = ReadText(d + L"\\restore-report.txt");
        if (info.interrupted) info.status = L"Interrupted restore";
        else if (rptText.find("Status: SUCCESS") != std::string::npos) info.status = L"Applied";
        else if (rptText.find("Status: PARTIAL") != std::string::npos) info.status = L"Partial";
        else if (rptText.find("Status: ROLLED BACK") != std::string::npos) info.status = L"Rolled back";
        else if (rptText.find("Status: ROLLBACK INCOMPLETE") != std::string::npos) info.status = L"Rollback incomplete";
        else if (rptText.find("Status: PRE-CHANGE") != std::string::npos) info.status = L"Pre-change snapshot";
        else info.status = L"Saved";
        const std::string logText = ReadText(d + L"\\snapshot-restores.log");
        if (!logText.empty()) {
            const auto lines = Split(Utf8ToW(logText), L'\n');
            for (auto li = lines.rbegin(); li != lines.rend(); ++li) {
                std::wstring t = *li;
                while (!t.empty() && (t.back() == L'\r' || t.back() == L' ')) t.pop_back();
                if (!t.empty()) { info.restoredSummary = t; break; }
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

// Creates a pre-change recovery point before a major operation (icon pack, cursor scheme, theme, large tweak group; BAK-3).
// M-2.6-UPD1: options and result of the strict form. `requireContent` makes "nothing was captured" a failure
// (the plain form below still allows it, for callers that only add their own files to the point afterwards).
struct AcbRecoveryPointOptions {
    bool requireContent = false;   // fail unless at least one file of AnkhCore's own state was captured
    bool journal = true;           // write the LOG-1 journal entry (tests pass false)
};
struct AcbRecoveryPointResult {
    bool ok = false;
    bool hasContent = false;       // at least one file was copied into the point
    bool verified = false;         // (verified form) every copy re-hashed against its manifest entry
    bool sealed = false;           // (verified form) sealed and the sealed manifest re-read
    size_t items = 0;
    std::wstring dir;
    std::wstring error;
};

inline AcbRecoveryPointResult AcbCreateRecoveryPointEx(const AcbRoots& roots, const std::wstring& operationLabel,
                                                       const AcbRecoveryPointOptions& opt) {
    AcbRecoveryPointResult res;
    const std::wstring recDir = roots.data + L"\\Recovery\\" + AcbRecoveryFolderStamp();
    if (kTestBuild && !InTestArea(recDir + L"\\x")) {
        TestRefused(L"pre-change recovery dir " + recDir);
        res.error = L"The test build refuses to create a recovery point outside its sandbox.";
        return res;
    }
    EnsureDir(recDir + L"\\files\\settings");
    // Nothing usable was written: remove only the empty folders created just above (never anything with content).
    auto dropEmpty = [&]() {
        RemoveDirectoryW((recDir + L"\\files\\settings").c_str());
        RemoveDirectoryW((recDir + L"\\files").c_str());
        RemoveDirectoryW(recDir.c_str());
    };
    const std::wstring sPath = roots.appdata + L"\\settings.json";
    uint64_t sz = 0;
    std::string sha;
    bool hasSettings = false;
    if (FileExists(sPath)) {
        // The file exists, so a failed copy or hash is a failure, never a silent "nothing to save".
        std::vector<uint8_t> bytes;
        if (!ReadAll(sPath, bytes) || !WriteAtomic(recDir + L"\\files\\settings\\settings.json", bytes.data(), bytes.size()) ||
            !AcbHashFile(recDir + L"\\files\\settings\\settings.json", sz, sha) || sz != bytes.size()) {
            res.error = L"AnkhCore's settings file exists but could not be copied into the recovery point.";
            return res;
        }
        hasSettings = true;
    }
    res.hasContent = hasSettings;
    if (opt.requireContent && !hasSettings) {
        dropEmpty();
        res.error = L"There is no AnkhCore settings file to put into a recovery point, so no meaningful recovery point can be made.";
        return res;
    }
    AcbRecManifest recMan;
    recMan.formatVersion = 2;
    recMan.kind = "pre-change";
    recMan.created = AcbUtcStampNow();
    recMan.sourceArchive = WToUtf8(operationLabel);
    recMan.sealed = false;
    if (hasSettings) {
        AcbRecItem ri;
        ri.dest = "appdata:settings.json";
        ri.backupPath = "files\\settings\\settings.json";
        ri.existed = true;
        ri.size = sz;
        ri.sha256 = sha;
        ri.strategy = "settings-merge3";
        recMan.items.push_back(std::move(ri));
    }
    if (!AcbWriteRecoveryManifest(recDir, recMan)) {
        res.error = L"The recovery manifest could not be written.";
        return res;
    }
    const std::string rpt = "AnkhCore 2.5 Pre-Change Recovery Point\r\nStatus: PRE-CHANGE\r\nOperation: " + WToUtf8(operationLabel) + "\r\n";
    WriteAtomic(recDir + L"\\restore-report.txt", reinterpret_cast<const uint8_t*>(rpt.data()), rpt.size());
    if (opt.journal && (!kTestBuild || InTestArea(JournalPath()))) {
        JournalAdd(L"Backup", L"RECOVERY_POINT", L"Automatic recovery point before " + operationLabel, {recDir, operationLabel});
    }
    res.ok = true;
    res.dir = recDir;
    res.items = recMan.items.size();
    return res;
}

inline bool AcbCreatePreChangeRecoveryPoint(const AcbRoots& roots, const std::wstring& operationLabel, std::wstring& outDir) {
    const AcbRecoveryPointResult r = AcbCreateRecoveryPointEx(roots, operationLabel, AcbRecoveryPointOptions{});
    outDir = r.ok ? r.dir : std::wstring();
    return r.ok;
}

// Re-reads a recovery point from disk: the manifest parses, lists at least one item, every saved copy exists and
// matches its recorded size and SHA-256, and (requireSealed) the point is sealed with after-state recorded.
inline bool AcbVerifyRecoveryPoint(const std::wstring& recDir, bool requireSealed, size_t& itemsOut, std::wstring& err) {
    itemsOut = 0;
    AcbRecManifest m;
    if (!AcbParseRecoveryManifest(ReadText(recDir + L"\\recovery-manifest.json"), m, err)) return false;
    if (m.items.empty()) { err = L"The recovery point holds no items."; return false; }
    if (requireSealed && !m.sealed) { err = L"The recovery point is not sealed."; return false; }
    size_t saved = 0;
    for (const AcbRecItem& it : m.items) if (it.existed) ++saved;
    if (saved == 0) { err = L"The recovery point saved no file, so there is nothing to restore from."; return false; }   // a point that holds no copy is not "verified"
    for (const AcbRecItem& it : m.items) {
        if (it.existed) {
            const std::wstring rel = AcbInferBackupRelPath(it.dest, it.backupPath);
            uint64_t sz = 0; std::string sh;
            if (rel.empty() || !FileExists(recDir + L"\\" + rel) || !AcbHashFile(recDir + L"\\" + rel, sz, sh) ||
                sz != it.size || sh != it.sha256) {
                err = L"A saved copy in the recovery point is missing or damaged.";
                return false;
            }
        }
        if (requireSealed && it.afterExists && it.afterSha256.empty()) { err = L"The sealed record is incomplete."; return false; }
    }
    itemsOut = m.items.size();
    return true;
}

// Strict recovery point for an operation that must not start without one (M-2.6-UPD1): content is required, then
// create -> verify -> seal -> verify the sealed point again. `ok` is true only if every stage passed. A point that fails
// a stage is kept on disk as it is (recovery information is never deleted) and `error` names the stage.
// `afterSeal` is a test seam that runs just before the final verification.
inline AcbRecoveryPointResult AcbCreateVerifiedRecoveryPoint(const AcbRoots& roots, const std::wstring& operationLabel,
                                                             bool journal = true,
                                                             const std::function<void(const std::wstring&)>& afterSeal = {}) {
    AcbRecoveryPointOptions o; o.requireContent = true; o.journal = journal;
    AcbRecoveryPointResult r = AcbCreateRecoveryPointEx(roots, operationLabel, o);
    if (!r.ok) return r;
    r.ok = false;
    size_t n = 0; std::wstring err;
    if (!AcbVerifyRecoveryPoint(r.dir, false, n, err)) { r.error = L"Recovery point verification failed: " + err; return r; }
    r.verified = true;
    if (!AcbSealRecoveryPoint(roots, r.dir)) { r.error = L"The recovery point could not be sealed."; return r; }
    if (afterSeal) afterSeal(r.dir);
    if (!AcbVerifyRecoveryPoint(r.dir, true, n, err)) { r.verified = false; r.error = L"Sealed recovery point verification failed: " + err; return r; }
    r.sealed = true;
    r.items = n;
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- B2: 3-Way Settings Merge, Planner & Snapshot Executor
inline bool AcbJValEqual(const JVal* a, const JVal* b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->t != b->t) return false;
    return AcbFormatJValScalar(*a) == AcbFormatJValScalar(*b);
}

// Pure 3-way merge for settings.json:
// - Keys where cur[k] == after[k] and before[k] differs are reverted to before[k] (or removed if added by op).
// - Keys modified by the user after the operation (cur[k] != after[k]) or newly added in cur are preserved.
// - All resulting keys pass AcbSanitizeSetting (D30 fan safety + range checks).
inline bool AcbMergeSettings3(const std::string& beforeJson,
                              const std::string& afterJson,
                              const std::string& curJson,
                              std::string& outJson,
                              int& revertedKeys,
                              int& keptKeys,
                              std::wstring& err) {
    outJson.clear();
    revertedKeys = 0;
    keptKeys = 0;
    JVal jb, ja, jc;
    if (!JsonParse(beforeJson, jb) || !jb.Is(JVal::Obj)) { err = L"Saved before-settings JSON is invalid."; return false; }
    if (!JsonParse(afterJson, ja) || !ja.Is(JVal::Obj)) { err = L"Saved after-settings JSON is invalid."; return false; }
    if (curJson.empty()) { jc.t = JVal::Obj; }
    else if (!JsonParse(curJson, jc) || !jc.Is(JVal::Obj)) { err = L"Current settings.json could not be parsed."; return false; }

    std::map<std::string, JVal> merged;
    for (size_t i = 0; i < jc.keys.size() && i < jc.a.size(); ++i) merged[jc.keys[i]] = jc.a[i];
    std::set<std::string> opKeys;
    for (const auto& k : jb.keys) opKeys.insert(k);
    for (const auto& k : ja.keys) opKeys.insert(k);

    for (const std::string& k : opKeys) {
        const JVal* pB = jb.Get(k);
        const JVal* pA = ja.Get(k);
        const JVal* pC = jc.Get(k);
        if (AcbJValEqual(pC, pA)) {
            if (!AcbJValEqual(pB, pA)) {
                if (pB) {
                    JVal val = *pB;
                    if (AcbSanitizeSetting(k, val)) {
                        merged[k] = std::move(val);
                        ++revertedKeys;
                    }
                } else {
                    if (merged.erase(k) > 0) ++revertedKeys;
                }
            }
        } else {
            ++keptKeys;
        }
    }

    // Re-sanitize final map so D30/D23 invariants always hold.
    for (auto it = merged.begin(); it != merged.end();) {
        if (!AcbSanitizeSetting(it->first, it->second)) it = merged.erase(it);
        else ++it;
    }

    outJson = "{\r\n";
    size_t i = 0;
    for (const auto& kv : merged) {
        outJson += "  \"" + JsonEsc(kv.first) + "\": " + AcbFormatJValScalar(kv.second);
        outJson += (++i < merged.size()) ? ",\r\n" : "\r\n";
    }
    outJson += "}\r\n";
    return true;
}

enum class AcbSnapAction {
    Replace,
    Delete,
    MergeSettings,
    AlreadyBefore,
    SkipModified,
    SkipDeleted,
    SkipCreatedOrChanged,
    SkipUnsealed,
    BlockedDamaged
};

struct AcbSnapPlanItem {
    AcbRecItem item;
    std::wstring destPath;
    std::wstring backupCopyPath;
    std::wstring afterCopyPath;
    AcbSnapAction action = AcbSnapAction::SkipUnsealed;
    std::wstring reason;
    bool defaultIncluded = false;
    int mergeReverted = 0;
    int mergeKept = 0;
};

struct AcbSnapPlan {
    bool ok = false;
    std::wstring error;
    AcbRecManifest manifest;
    std::vector<AcbSnapPlanItem> items;
    int replaceCount = 0, deleteCount = 0, mergeCount = 0, skipCount = 0, alreadyCount = 0, blockedCount = 0;
};

inline AcbSnapPlan AcbPlanSnapshotRestore(const AcbRoots& roots, const std::wstring& recDir) {
    AcbSnapPlan plan;
    if (!AcbParseRecoveryManifest(ReadText(recDir + L"\\recovery-manifest.json"), plan.manifest, plan.error)) {
        return plan;
    }
    if (plan.manifest.items.empty()) {
        plan.error = L"This recovery point contains no restorable items.";
        return plan;
    }
    for (const AcbRecItem& it : plan.manifest.items) {
        AcbSnapPlanItem pi;
        pi.item = it;
        if (!AcbResolveDest(roots, it.dest, pi.destPath) || pi.destPath.empty()) {
            pi.action = AcbSnapAction::BlockedDamaged;
            pi.reason = L"unsafe or unsupported destination path";
            ++plan.blockedCount;
            plan.items.push_back(std::move(pi));
            continue;
        }
        if (it.existed) {
            const std::wstring rel = AcbInferBackupRelPath(it.dest, it.backupPath);
            if (rel.empty()) {
                pi.action = AcbSnapAction::BlockedDamaged;
                pi.reason = L"recovery copy path invalid";
                ++plan.blockedCount;
                plan.items.push_back(std::move(pi));
                continue;
            }
            pi.backupCopyPath = recDir + L"\\" + rel;
            uint64_t bSz = 0; std::string bSha;
            if (!FileExists(pi.backupCopyPath) || !AcbHashFile(pi.backupCopyPath, bSz, bSha) ||
                bSz != it.size || bSha != it.sha256) {
                pi.action = AcbSnapAction::BlockedDamaged;
                pi.reason = L"recovery copy damaged or missing";
                ++plan.blockedCount;
                plan.items.push_back(std::move(pi));
                continue;
            }
        }
        const bool curExists = FileExists(pi.destPath);
        uint64_t curSz = 0; std::string curSha;
        if (curExists) AcbHashFile(pi.destPath, curSz, curSha);

        if (!plan.manifest.sealed) {
            if ((it.existed && curExists && curSha == it.sha256) || (!it.existed && !curExists)) {
                pi.action = AcbSnapAction::AlreadyBefore;
                ++plan.alreadyCount;
            } else {
                pi.action = AcbSnapAction::SkipUnsealed;
                pi.reason = L"what the operation left is not recorded (older point)";
                ++plan.skipCount;
            }
            plan.items.push_back(std::move(pi));
            continue;
        }

        if (it.strategy == "settings-merge3" && it.existed) {
            if (curExists && curSha == it.sha256) {
                pi.action = AcbSnapAction::AlreadyBefore;
                ++plan.alreadyCount;
            } else {
                pi.afterCopyPath = recDir + L"\\after\\settings.json";
                if (!FileExists(pi.afterCopyPath)) {
                    pi.action = AcbSnapAction::SkipUnsealed;
                    pi.reason = L"after-state settings copy missing";
                    ++plan.skipCount;
                } else {
                    std::string merged, mErr;
                    std::wstring wErr;
                    if (!AcbMergeSettings3(ReadText(pi.backupCopyPath), ReadText(pi.afterCopyPath),
                                           curExists ? ReadText(pi.destPath) : std::string(),
                                           merged, pi.mergeReverted, pi.mergeKept, wErr)) {
                        pi.action = AcbSnapAction::SkipModified;
                        pi.reason = wErr.empty() ? L"settings merge failed" : wErr;
                        ++plan.skipCount;
                    } else if (pi.mergeReverted == 0) {
                        pi.action = AcbSnapAction::AlreadyBefore;
                        ++plan.alreadyCount;
                    } else {
                        pi.action = AcbSnapAction::MergeSettings;
                        pi.defaultIncluded = true;
                        ++plan.mergeCount;
                    }
                }
            }
        } else if (it.existed) {
            if (curExists && curSha == it.sha256) {
                pi.action = AcbSnapAction::AlreadyBefore;
                ++plan.alreadyCount;
            } else if (!it.afterExists) {
                if (!curExists) {
                    pi.action = AcbSnapAction::Replace;
                    pi.defaultIncluded = true;
                    ++plan.replaceCount;
                } else {
                    pi.action = AcbSnapAction::SkipModified;
                    pi.reason = L"created after the operation deleted it";
                    ++plan.skipCount;
                }
            } else if (!curExists) {
                pi.action = AcbSnapAction::SkipDeleted;
                pi.reason = L"deleted after the operation";
                ++plan.skipCount;
            } else if (curSha == it.afterSha256) {
                pi.action = AcbSnapAction::Replace;
                pi.defaultIncluded = true;
                ++plan.replaceCount;
            } else {
                pi.action = AcbSnapAction::SkipModified;
                pi.reason = L"changed after the operation";
                ++plan.skipCount;
            }
        } else {
            if (!curExists) {
                pi.action = AcbSnapAction::AlreadyBefore;
                ++plan.alreadyCount;
            } else if (it.afterExists && curSha == it.afterSha256) {
                pi.action = AcbSnapAction::Delete;
                pi.defaultIncluded = true;
                ++plan.deleteCount;
            } else {
                pi.action = AcbSnapAction::SkipCreatedOrChanged;
                pi.reason = L"created or changed after the operation";
                ++plan.skipCount;
            }
        }
        plan.items.push_back(std::move(pi));
    }
    plan.ok = true;
    return plan;
}

struct AcbSnapRestoreOptions {
    std::set<std::string> forceIncludeDests;
    bool allowPartialVerified = false;
    bool failPrePointForTest = false;
    int failAfterFilesForTest = -1;
    bool interruptAfterFirstForTest = false;
    std::function<void(const std::wstring&)> beforeRollback;
};

struct AcbSnapRestoreReport {
    bool ok = false;
    bool partial = false;
    bool interrupted = false;
    bool rolledBack = false;
    bool rollbackIncomplete = false;
    int filesRestored = 0;
    int filesDeleted = 0;
    int filesSkipped = 0;
    int rollbackPutBack = 0;
    std::wstring newRecoveryDir;
    std::wstring error;
    std::vector<std::wstring> restoredItems;
    std::vector<std::wstring> skippedItems;
    std::vector<std::wstring> rollbackFailures;
};

inline AcbSnapRestoreReport AcbRestoreSnapshot(const AcbRoots& roots,
                                                 const std::wstring& recDir,
                                                 const AcbSnapRestoreOptions& opts = {}) {
    AcbSnapRestoreReport rep;
    const AcbSnapPlan plan = AcbPlanSnapshotRestore(roots, recDir);
    if (!plan.ok) {
        rep.error = plan.error;
        return rep;
    }

    std::vector<AcbSnapPlanItem> selected;
    for (const auto& pi : plan.items) {
        const bool forced = opts.forceIncludeDests.count(pi.item.dest) != 0;
        if (pi.action == AcbSnapAction::BlockedDamaged) {
            if (!opts.allowPartialVerified) {
                rep.error = L"Snapshot verification failed for \"" + acbdetail::Show(pi.item.dest) + L"\": " + pi.reason + L".";
                return rep;
            }
            ++rep.filesSkipped;
            rep.skippedItems.push_back(acbdetail::Show(pi.item.dest) + L" (" + pi.reason + L")");
            continue;
        }
        if (pi.action == AcbSnapAction::AlreadyBefore) {
            continue;
        }
        if (pi.defaultIncluded || forced) {
            if (forced && !pi.item.existed && FileExists(pi.destPath)) {
                // Forced delete of a file created/changed after operation
                AcbSnapPlanItem copy = pi;
                copy.action = AcbSnapAction::Delete;
                selected.push_back(std::move(copy));
            } else if (forced && pi.item.existed) {
                AcbSnapPlanItem copy = pi;
                copy.action = AcbSnapAction::Replace;
                selected.push_back(std::move(copy));
            } else {
                selected.push_back(pi);
            }
        } else {
            ++rep.filesSkipped;
            rep.skippedItems.push_back(acbdetail::Show(pi.item.dest) + L" (" + pi.reason + L")");
        }
    }

    if (selected.empty()) {
        rep.ok = true;
        rep.partial = rep.filesSkipped > 0;
        return rep;
    }

    // 4. Create a new pre-restore recovery point of the current state before touching anything.
    rep.newRecoveryDir = roots.data + L"\\Recovery\\" + AcbRecoveryFolderStamp();
    if (kTestBuild && !InTestArea(rep.newRecoveryDir + L"\\x")) {
        TestRefused(L"snapshot recovery dir " + rep.newRecoveryDir);
        rep.error = L"Test build refuses to create recovery point outside the test sandbox.";
        return rep;
    }
    if (opts.failPrePointForTest) {
        rep.error = L"Could not create pre-restore recovery point before applying snapshot.";
        return rep;
    }

    struct PreItem {
        std::wstring destPath;
        std::wstring copyPath;
        bool existed = false;
        std::string shaBefore;
        uint64_t sizeBefore = 0;
    };
    std::vector<PreItem> preItems(selected.size());
    AcbRecManifest newMan;
    newMan.formatVersion = 2;
    newMan.kind = "pre-change";
    newMan.created = AcbUtcStampNow();
    newMan.sourceArchive = "Before restoring snapshot " + WToUtf8(NameOf(recDir));
    newMan.sealed = false;

    EnsureDir(rep.newRecoveryDir + L"\\files");
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& si = selected[i];
        PreItem& pre = preItems[i];
        pre.destPath = si.destPath;
        pre.existed = FileExists(si.destPath);
        const std::wstring rel = AcbInferBackupRelPath(si.item.dest, si.item.backupPath);
        pre.copyPath = rep.newRecoveryDir + L"\\" + rel;
        EnsureDir(DirOf(pre.copyPath));
        if (pre.existed) {
            std::vector<uint8_t> curBytes;
            if (!ReadAll(pre.destPath, curBytes) || !WriteAtomic(pre.copyPath, curBytes.data(), curBytes.size()) ||
                !AcbHashFile(pre.copyPath, pre.sizeBefore, pre.shaBefore)) {
                rep.error = L"Could not save pre-restore recovery copy of \"" + NameOf(pre.destPath) + L"\".";
                return rep;
            }
        }
        AcbRecItem ri;
        ri.dest = si.item.dest;
        ri.backupPath = WToUtf8(rel);
        ri.existed = pre.existed;
        ri.size = pre.sizeBefore;
        ri.sha256 = pre.shaBefore;
        ri.strategy = si.item.strategy;
        newMan.items.push_back(std::move(ri));
    }
    if (!AcbWriteRecoveryManifest(rep.newRecoveryDir, newMan)) {
        rep.error = L"Could not write pre-restore recovery manifest.";
        return rep;
    }
    if (!kTestBuild || InTestArea(JournalPath())) {
        JournalAdd(L"Backup", L"RECOVERY_POINT", L"Recovery point before snapshot restore", {rep.newRecoveryDir, NameOf(recDir)});
    }

    // 5. Write in-progress marker.
    const std::wstring markerPath = rep.newRecoveryDir + L"\\restore-in-progress.marker";
    const std::string markerTxt = WToUtf8(recDir);
    if (!WriteAtomic(markerPath, reinterpret_cast<const uint8_t*>(markerTxt.data()), markerTxt.size())) {
        rep.error = L"Could not write restore-in-progress marker.";
        return rep;
    }

    std::vector<size_t> touched;
    auto rollback = [&]() {
        if (opts.beforeRollback) opts.beforeRollback(rep.newRecoveryDir);
        for (auto it = touched.rbegin(); it != touched.rend(); ++it) {
            const PreItem& pre = preItems[*it];
            bool back = false;
            if (pre.existed) {
                uint64_t cSz = 0; std::string cSha; std::vector<uint8_t> orig;
                if (AcbHashFile(pre.copyPath, cSz, cSha) && cSha == pre.shaBefore &&
                    ReadAll(pre.copyPath, orig) && WriteAtomic(pre.destPath, orig.data(), orig.size())) {
                    uint64_t dSz = 0; std::string dSha;
                    back = AcbHashFile(pre.destPath, dSz, dSha) && dSha == pre.shaBefore;
                }
            } else {
                if (FileExists(pre.destPath)) DeleteFileW(pre.destPath.c_str());
                back = !FileExists(pre.destPath);
            }
            if (back) ++rep.rollbackPutBack; else rep.rollbackFailures.push_back(NameOf(pre.destPath));
        }
        DeleteFileW(markerPath.c_str());
        if (rep.rollbackFailures.empty()) rep.rolledBack = true;
        else {
            rep.rollbackIncomplete = true;
            std::wstring names; for (const auto& n : rep.rollbackFailures) names += (names.empty() ? L"" : L", ") + n;
            rep.error += (rep.error.empty() ? L"" : L" ") + std::wstring(L"ROLLBACK INCOMPLETE (") +
                         std::to_wstring(rep.rollbackPutBack) + L" of " +
                         std::to_wstring(rep.rollbackPutBack + static_cast<int>(rep.rollbackFailures.size())) +
                         L" file(s) put back): could not put back " + names + L".";
        }
    };

    // 6. Apply item by item.
    for (size_t i = 0; i < selected.size(); ++i) {
        const AcbSnapPlanItem& si = selected[i];
        if (si.action == AcbSnapAction::Delete) {
            if (FileExists(si.destPath) && !DeleteFileW(si.destPath.c_str())) {
                rep.error = L"Failed to delete \"" + NameOf(si.destPath) + L"\".";
                rollback();
                break;
            }
            touched.push_back(i);
            ++rep.filesDeleted;
            rep.restoredItems.push_back(L"deleted: " + acbdetail::Show(si.item.dest));
        } else if (si.action == AcbSnapAction::MergeSettings) {
            std::string merged;
            int rev = 0, kept = 0;
            if (!AcbMergeSettings3(ReadText(si.backupCopyPath), ReadText(si.afterCopyPath),
                                   FileExists(si.destPath) ? ReadText(si.destPath) : std::string(),
                                   merged, rev, kept, rep.error) ||
                !WriteAtomic(si.destPath, reinterpret_cast<const uint8_t*>(merged.data()), merged.size())) {
                if (rep.error.empty()) rep.error = L"Failed to write merged settings.json.";
                rollback();
                break;
            }
            touched.push_back(i);
            ++rep.filesRestored;
            rep.restoredItems.push_back(L"settings-merge3: " + acbdetail::Show(si.item.dest) +
                                        L" (" + std::to_wstring(rev) + L" reverted, " + std::to_wstring(kept) + L" kept)");
        } else {
            std::vector<uint8_t> content;
            if (!ReadAll(si.backupCopyPath, content)) {
                rep.error = L"Failed to read recovery copy for \"" + NameOf(si.destPath) + L"\".";
                rollback();
                break;
            }
            EnsureDir(DirOf(si.destPath));
            const std::wstring tmpPath = si.destPath + L".acb-tmp";
            if (!WriteAllFlushed(tmpPath, content.data(), content.size())) {
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Failed to stage \"" + NameOf(si.destPath) + L"\".";
                rollback();
                break;
            }
            uint64_t dSz = 0; std::string dSha;
            if (!AcbHashFile(tmpPath, dSz, dSha) || dSz != si.item.size || dSha != si.item.sha256) {
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Staged file \"" + NameOf(si.destPath) + L"\" failed SHA-256 verification.";
                rollback();
                break;
            }
            if (!MoveFileExW(tmpPath.c_str(), si.destPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(tmpPath.c_str());
                rep.error = L"Failed to replace \"" + NameOf(si.destPath) + L"\".";
                rollback();
                break;
            }
            touched.push_back(i);
            ++rep.filesRestored;
            rep.restoredItems.push_back(L"restored: " + acbdetail::Show(si.item.dest));
        }

        if (opts.interruptAfterFirstForTest) {
            rep.interrupted = true;
            rep.error = L"Snapshot restore was interrupted before completion.";
            return rep;   // marker stays in newRecoveryDir for AcbFinishInterruptedRollback
        }
        if (opts.failAfterFilesForTest >= 0 && (rep.filesRestored + rep.filesDeleted) >= opts.failAfterFilesForTest) {
            rep.error = L"Simulated snapshot apply failure after " + std::to_wstring(rep.filesRestored + rep.filesDeleted) + L" file(s).";
            rollback();
            break;
        }
    }

    rep.ok = !rep.rolledBack && !rep.rollbackIncomplete && !rep.interrupted && rep.error.empty();
    rep.partial = rep.ok && rep.filesSkipped > 0;
    if (rep.ok) {
        DeleteFileW(markerPath.c_str());
        AcbSealRecoveryPoint(roots, rep.newRecoveryDir);
    }

    const std::wstring statusWord = rep.ok ? (rep.partial ? L"PARTIAL" : L"SUCCESS")
                                           : (rep.rolledBack ? L"ROLLED BACK" : (rep.rollbackIncomplete ? L"ROLLBACK INCOMPLETE" : L"FAILED"));
    std::wstring rpt = L"AnkhCore 2.5 Snapshot Restore Report\r\nStatus: " + statusWord + L"\r\n";
    if (!rep.error.empty()) rpt += L"Error: " + rep.error + L"\r\n";
    for (const auto& s : rep.rollbackFailures) rpt += L"NOT PUT BACK: " + s + L"\r\n";
    for (const auto& s : rep.restoredItems) rpt += L"RESTORED: " + s + L"\r\n";
    for (const auto& s : rep.skippedItems) rpt += L"SKIPPED: " + s + L"\r\n";
    const std::string rptU = WToUtf8(rpt);
    WriteAtomic(rep.newRecoveryDir + L"\\restore-report.txt", reinterpret_cast<const uint8_t*>(rptU.data()), rptU.size());

    if (rep.ok) {
        const std::wstring logLine = L"Restored on " + NowText(true) + L" (" +
                                     std::to_wstring(rep.filesRestored + rep.filesDeleted) + L" item(s), " +
                                     std::to_wstring(rep.filesSkipped) + L" skipped)\r\n";
        std::string prev = ReadText(recDir + L"\\snapshot-restores.log");
        prev += WToUtf8(logLine);
        WriteAtomic(recDir + L"\\snapshot-restores.log", reinterpret_cast<const uint8_t*>(prev.data()), prev.size());
    }
    if (!kTestBuild || InTestArea(JournalPath())) {
        JournalAdd(L"Backup", L"SNAPSHOT_RESTORE", L"Snapshot restore (" + statusWord + L")",
                   {rep.newRecoveryDir, recDir, std::to_wstring(rep.filesRestored + rep.filesDeleted),
                    std::to_wstring(rep.filesSkipped), statusWord});
    }
    return rep;
}

// Completes rollback for an interrupted snapshot restore identified by restore-in-progress.marker.
inline AcbSnapRestoreReport AcbFinishInterruptedRollback(const AcbRoots& roots, const std::wstring& recDir) {
    AcbSnapRestoreReport rep;
    rep.newRecoveryDir = recDir;
    const std::wstring markerPath = recDir + L"\\restore-in-progress.marker";
    if (!FileExists(markerPath)) {
        rep.ok = true;
        return rep;
    }
    AcbRecManifest man;
    if (!AcbParseRecoveryManifest(ReadText(recDir + L"\\recovery-manifest.json"), man, rep.error)) {
        return rep;
    }
    for (const AcbRecItem& it : man.items) {
        std::wstring destPath;
        if (!AcbResolveDest(roots, it.dest, destPath) || destPath.empty()) {
            rep.rollbackFailures.push_back(acbdetail::Show(it.dest));
            continue;
        }
        bool back = false;
        if (it.existed) {
            const std::wstring copyPath = recDir + L"\\" + AcbInferBackupRelPath(it.dest, it.backupPath);
            uint64_t curSz = 0; std::string curSha;
            if (FileExists(destPath) && AcbHashFile(destPath, curSz, curSha) && curSha == it.sha256) {
                back = true;
            } else {
                uint64_t cSz = 0; std::string cSha; std::vector<uint8_t> orig;
                if (AcbHashFile(copyPath, cSz, cSha) && cSha == it.sha256 &&
                    ReadAll(copyPath, orig) && WriteAtomic(destPath, orig.data(), orig.size())) {
                    uint64_t dSz = 0; std::string dSha;
                    back = AcbHashFile(destPath, dSz, dSha) && dSha == it.sha256;
                }
            }
        } else {
            if (FileExists(destPath)) DeleteFileW(destPath.c_str());
            back = !FileExists(destPath);
        }
        if (back) ++rep.rollbackPutBack; else rep.rollbackFailures.push_back(NameOf(destPath));
    }
    DeleteFileW(markerPath.c_str());
    if (rep.rollbackFailures.empty()) {
        rep.rolledBack = true;
        rep.ok = true;
    } else {
        rep.rollbackIncomplete = true;
        rep.error = L"ROLLBACK INCOMPLETE during interrupted restore recovery.";
    }
    std::wstring rpt = L"AnkhCore 2.5 Interrupted Restore Recovery\r\nStatus: " +
                       std::wstring(rep.rolledBack ? L"ROLLED BACK" : L"ROLLBACK INCOMPLETE") + L"\r\n";
    for (const auto& s : rep.rollbackFailures) rpt += L"NOT PUT BACK: " + s + L"\r\n";
    const std::string rptU = WToUtf8(rpt);
    WriteAtomic(recDir + L"\\restore-report.txt", reinterpret_cast<const uint8_t*>(rptU.data()), rptU.size());
    return rep;
}

inline std::wstring AcbDefaultBackupDir() {
    if (kTestBuild) return DataDir() + L"\\Backups";
    const std::wstring docs = UserFolder(FOLDERID_Documents);
    return (!docs.empty() ? docs : DataDir()) + L"\\AnkhCore Backups";
}

struct AcbArchiveSummary {
    std::wstring fileName;
    std::wstring fullPath;
    uint64_t fileBytes = 0;
    AcbCheck check;
};

inline std::vector<AcbArchiveSummary> AcbListBackups(const std::wstring& dir) {
    std::vector<AcbArchiveSummary> out;
    if (!DirExists(dir)) return out;
    const auto files = ListDir(dir, false, L"*.acbackup");
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        const std::wstring p = dir + L"\\" + *it;
        std::vector<uint8_t> raw;
        if (!ReadAll(p, raw)) continue;
        AcbArchiveSummary s;
        s.fileName = *it;
        s.fullPath = p;
        s.fileBytes = raw.size();
        s.check = AcbVerify(raw.data(), raw.size());
        out.push_back(std::move(s));
    }
    return out;
}

// Fast listing for UI display: inspects ZIP structure and manifest.json only (no payload SHA-256
// decompression) and stops after maxEntries newest archives when maxEntries > 0.
inline std::vector<AcbArchiveSummary> AcbListBackupsMetadata(const std::wstring& dir, size_t maxEntries = 0) {
    std::vector<AcbArchiveSummary> out;
    if (!DirExists(dir)) return out;
    const auto files = ListDir(dir, false, L"*.acbackup");
    for (auto it = files.rbegin(); it != files.rend(); ++it) {
        if (maxEntries > 0 && out.size() >= maxEntries) break;
        const std::wstring p = dir + L"\\" + *it;
        std::vector<uint8_t> raw;
        if (!ReadAll(p, raw)) continue;
        AcbArchiveSummary s;
        s.fileName = *it;
        s.fullPath = p;
        s.fileBytes = raw.size();
        s.check = AcbInspectMetadata(raw.data(), raw.size());
        out.push_back(std::move(s));
    }
    return out;
}

// Thread-safe backup listing cache (C2): avoids re-reading and hashing .acbackup archives on every paint.
struct AcbBackupListCache {
    struct State {
        bool valid = false;
        bool loading = false;
        std::shared_ptr<const std::vector<AcbArchiveSummary>> items;
    };

    mutable std::mutex mx;
    std::wstring cachedDir;
    size_t cachedMax = 0;
    uint64_t dirStamp = 0;
    bool valid = false;
    bool loading = false;
    std::wstring loadingDir;
    uint64_t loadingGen = 0;
    uint64_t generation = 0;
    std::shared_ptr<const std::vector<AcbArchiveSummary>> items = std::make_shared<const std::vector<AcbArchiveSummary>>();
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<bool> stopRequested{false};
    HANDLE worker = nullptr;

    // Optional test seam: overrides AcbListBackupsMetadata when set.
    std::function<std::vector<AcbArchiveSummary>(const std::wstring&, size_t)> listFn;

    static uint64_t ComputeDirStamp(const std::wstring& dir) {
        uint64_t h = 14695981039346656037ull;
        auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
        if (dir.empty() || !DirExists(dir)) { mix(0); return h; }
        WIN32_FIND_DATAW fd{};
        HANDLE fh = FindFirstFileW((dir + L"\\*.acbackup").c_str(), &fd);
        if (fh == INVALID_HANDLE_VALUE) { mix(1); return h; }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            for (const wchar_t* p = fd.cFileName; *p; ++p) mix(static_cast<uint64_t>(*p));
            mix(static_cast<uint64_t>(fd.nFileSizeHigh));
            mix(static_cast<uint64_t>(fd.nFileSizeLow));
            mix(static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime));
            mix(static_cast<uint64_t>(fd.ftLastWriteTime.dwLowDateTime));
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        return h;
    }

    State Snapshot() const {
        std::lock_guard<std::mutex> lk(mx);
        return {valid, loading, items};
    }

    void Invalidate() {
        std::lock_guard<std::mutex> lk(mx);
        valid = false;
        ++generation;
    }

    std::shared_ptr<const std::vector<AcbArchiveSummary>> GetOrRefresh(const std::wstring& dir, size_t maxEntries = 12, bool* wasCacheHit = nullptr) {
        const uint64_t stamp = ComputeDirStamp(dir);
        uint64_t myGen = 0;
        {
            std::lock_guard<std::mutex> lk(mx);
            if (valid && cachedDir == dir && cachedMax == maxEntries && dirStamp == stamp) {
                ++hits;
                if (wasCacheHit) *wasCacheHit = true;
                return items;
            }
            myGen = generation;
        }
        ++misses;
        if (wasCacheHit) *wasCacheHit = false;
        auto fresh = std::make_shared<const std::vector<AcbArchiveSummary>>(
            listFn ? listFn(dir, maxEntries) : AcbListBackupsMetadata(dir, maxEntries));
        std::lock_guard<std::mutex> lk(mx);
        if (generation == myGen) {
            cachedDir = dir;
            cachedMax = maxEntries;
            dirStamp = ComputeDirStamp(dir);
            items = fresh;
            valid = true;
        }
        return items;
    }

    void RefreshAsync(const std::wstring& dir, size_t maxEntries, std::function<void()> onDone) {
        const uint64_t stamp = ComputeDirStamp(dir);
        uint64_t myGen = 0;
        {
            std::lock_guard<std::mutex> lk(mx);
            if (valid && cachedDir == dir && cachedMax == maxEntries && dirStamp == stamp) return;
            if (loading && loadingDir == dir && loadingGen == generation) return;
            if (worker) {
                if (WaitForSingleObject(worker, 0) == WAIT_OBJECT_0) { CloseHandle(worker); worker = nullptr; }
                else return;
            }
            loading = true;
            loadingDir = dir;
            loadingGen = generation;
            myGen = generation;
        }
        struct Ctx {
            AcbBackupListCache* self;
            std::wstring dir;
            size_t maxEntries;
            uint64_t gen;
            std::function<void()> onDone;
        };
        auto* ctx = new Ctx{this, dir, maxEntries, myGen, std::move(onDone)};
        HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            std::unique_ptr<Ctx> c(static_cast<Ctx*>(p));
            if (c->self->stopRequested.load()) return 0;
            ++c->self->misses;
            auto fresh = std::make_shared<const std::vector<AcbArchiveSummary>>(
                c->self->listFn ? c->self->listFn(c->dir, c->maxEntries) : AcbListBackupsMetadata(c->dir, c->maxEntries));
            const uint64_t stampNow = ComputeDirStamp(c->dir);
            {
                std::lock_guard<std::mutex> lk(c->self->mx);
                c->self->loading = false;
                if (!c->self->stopRequested.load() && c->self->generation == c->gen) {
                    c->self->cachedDir = c->dir;
                    c->self->cachedMax = c->maxEntries;
                    c->self->dirStamp = stampNow;
                    c->self->items = std::move(fresh);
                    c->self->valid = true;
                }
            }
            if (!c->self->stopRequested.load() && c->onDone) c->onDone();
            return 0;
        }, ctx, 0, nullptr);
        if (th) {
            std::lock_guard<std::mutex> lk(mx);
            worker = th;
        } else {
            delete ctx;
            std::lock_guard<std::mutex> lk(mx);
            loading = false;
        }
    }

    void Join() {
        stopRequested.store(true);
        HANDLE th = nullptr;
        {
            std::lock_guard<std::mutex> lk(mx);
            th = worker;
            worker = nullptr;
        }
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    }
};

/// Export a verified diagnostic support bundle (.zip) containing system summary, logs, settings, and network counters.
// extraTextFiles (2.7 C1): additional named text files built by the caller (the hardware inventory). A name must be a plain file name
// ([A-Za-z0-9._-], at most 64 characters, no path); an unsafe, empty, oversized (over 512 KiB) or duplicate entry is skipped, never written.
inline bool AcbExportSupportBundle(const AcbRoots& roots, const std::wstring& outZipPath,
                                   const std::string& summaryTxt, std::wstring& err,
                                   const std::vector<std::pair<std::string, std::string>>& extraTextFiles = {}) {
    std::vector<ZipItem> zip;
    std::set<std::string> have;   // archive names already in the bundle: ZipList refuses two entries with one name, which used to fail the whole export
    auto addBytes = [&](const std::string& arcName, std::vector<uint8_t> bytes) {
        if (!have.insert(arcName).second) return;
        ZipItem z;
        z.name = arcName;
        z.data = std::move(bytes);
        zip.push_back(std::move(z));
    };

    if (!summaryTxt.empty()) {
        std::vector<uint8_t> b(summaryTxt.begin(), summaryTxt.end());
        addBytes("system-summary.txt", std::move(b));
    }

    for (const auto& ex : extraTextFiles) {
        const std::string& n = ex.first;
        bool safe = !n.empty() && n.size() <= 64 && n[0] != '.';
        for (const unsigned char ch : n) if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) safe = false;
        if (!safe || ex.second.empty() || ex.second.size() > (512u << 10)) continue;
        addBytes(n, std::vector<uint8_t>(ex.second.begin(), ex.second.end()));   // a name already in the bundle (the summary) is not replaced
    }

    auto tryInclude = [&](const std::wstring& diskPath, const std::string& arcName) {
        if (diskPath.empty() || have.count(arcName)) return;   // the first source listed wins (Logs\ankhcore.log before the old data\ankhcore.log; data before appdata)
        std::vector<uint8_t> raw;
        if (ReadAll(diskPath, raw) && !raw.empty()) addBytes(arcName, std::move(raw));
    };

    if (!roots.data.empty()) {
        tryInclude(roots.data + L"\\Logs\\ankhcore.log", "ankhcore.log");
        tryInclude(roots.data + L"\\ankhcore.log", "ankhcore.log");
        tryInclude(roots.data + L"\\net-counters.txt", "net-counters.txt");
    }
    if (!roots.appdata.empty()) {
        tryInclude(roots.appdata + L"\\settings.json", "settings.json");
        tryInclude(roots.appdata + L"\\net-counters.txt", "net-counters.txt");
    }

    if (zip.empty()) {
        addBytes("system-summary.txt", {'A','n','k','h','C','o','r','e','\n'});
    }

    SYSTEMTIME st; GetSystemTime(&st);
    const uint16_t dosDate = static_cast<uint16_t>((std::max(0, static_cast<int>(st.wYear) - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    const uint16_t dosTime = static_cast<uint16_t>((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    std::vector<uint8_t> zipBytes;
    if (!ZipWrite(zip, dosTime, dosDate, zipBytes)) {
        err = L"Support bundle exceeds ZIP size limit";
        return false;
    }
    std::vector<ZipEntry> check;
    if (!ZipList(zipBytes.data(), zipBytes.size(), check, err)) return false;
    EnsureDir(DirOf(outZipPath));
    if (!WriteAtomic(outZipPath, zipBytes.data(), zipBytes.size())) {
        err = L"Failed to write support bundle";
        return false;
    }
    return true;
}

// ================================================================ 2.6 Unified Backup & Recovery Foundation (M-2.6-BAK1)
// 1. Explicit 5-Class Application Restoration Classification (§4.1):
//    - PortableAppAndData:        portable app files and known data
//    - InstalledAppReadyForData:  installation-based app already installed on destination PC
//    - PackageManagerReinstall:   package-manager reinstall available (UniGetUI / winget / scoop / choco)
//    - ManualInstallRequired:     manual installation required before supported data can be restored
//    - UnsupportedOrUnknown:      unsupported or unknown restoration method
enum class AcbAppRestoreClass {
    PortableAppAndData = 0,
    InstalledAppReadyForData = 1,
    PackageManagerReinstall = 2,
    ManualInstallRequired = 3,
    UnsupportedOrUnknown = 4
};

inline const char* AcbAppRestoreClassId(AcbAppRestoreClass c) {
    switch (c) {
    case AcbAppRestoreClass::PortableAppAndData:       return "portable";
    case AcbAppRestoreClass::InstalledAppReadyForData: return "installed";
    case AcbAppRestoreClass::PackageManagerReinstall:  return "package-manager";
    case AcbAppRestoreClass::ManualInstallRequired:    return "manual";
    default:                                           return "unsupported";
    }
}

inline const wchar_t* AcbAppRestoreClassLabel(AcbAppRestoreClass c) {
    switch (c) {
    case AcbAppRestoreClass::PortableAppAndData:       return L"Portable app files and known data";
    case AcbAppRestoreClass::InstalledAppReadyForData: return L"Installation-based app (already installed)";
    case AcbAppRestoreClass::PackageManagerReinstall:  return L"Package-manager reinstall available";
    case AcbAppRestoreClass::ManualInstallRequired:    return L"Manual installation required";
    default:                                           return L"Unsupported / unknown";
    }
}

inline AcbAppRestoreClass AcbParseAppRestoreClassId(const std::string& s) {
    if (s == "portable") return AcbAppRestoreClass::PortableAppAndData;
    if (s == "installed") return AcbAppRestoreClass::InstalledAppReadyForData;
    if (s == "package-manager") return AcbAppRestoreClass::PackageManagerReinstall;
    if (s == "manual") return AcbAppRestoreClass::ManualInstallRequired;
    return AcbAppRestoreClass::UnsupportedOrUnknown;
}

struct AcbAppInventoryEntry26 {
    std::string id;
    std::string name;
    std::string version;
    std::string source;
    std::string managerName;
    bool installable = false;
    bool isPortable = false;
    bool hasKnownDataMap = false;
    AcbAppRestoreClass restoreClass = AcbAppRestoreClass::UnsupportedOrUnknown;
};

struct AcbAppRestoreItem26 {
    std::string id;
    std::string name;
    std::string backupVersion;
    std::string candidateVersion;
    std::string source;
    std::string managerName;
    bool isPortable = false;
    bool hasKnownDataMap = false;
    bool versionMismatch = false;
    AcbAppRestoreClass restoreClass = AcbAppRestoreClass::UnsupportedOrUnknown;
    std::wstring message;
};

struct AcbAppRestorePlan26 {
    std::vector<AcbAppRestoreItem26> items;
    int portableCount = 0;
    int installedCount = 0;
    int packageManagerCount = 0;
    int manualCount = 0;
    int unsupportedCount = 0;
};

inline AcbAppRestoreItem26 AcbClassifyAppRestoration(const AcbAppInventoryEntry26& entry,
                                                     const std::vector<AcbPackage>& installedOnPc,
                                                     const std::vector<AcbPackage>& catalogCandidates,
                                                     const std::vector<std::string>& availableManagers) {
    AcbAppRestoreItem26 out;
    out.id = entry.id;
    out.name = entry.name;
    out.backupVersion = entry.version;
    out.source = entry.source;
    out.managerName = entry.managerName;
    out.isPortable = entry.isPortable;
    out.hasKnownDataMap = entry.hasKnownDataMap;

    auto isInstalled = [&]() {
        for (const auto& inst : installedOnPc) {
            if ((!entry.id.empty() && inst.id == entry.id) || (!entry.name.empty() && inst.name == entry.name))
                return true;
        }
        return false;
    };
    auto hasMgr = [&](const std::string& m) {
        return !m.empty() && m != "manual" && m != "unknown" &&
               std::find(availableManagers.begin(), availableManagers.end(), m) != availableManagers.end();
    };
    auto findCandidateVer = [&]() {
        for (const auto& c : catalogCandidates) {
            if (!entry.id.empty() && c.id == entry.id && !c.version.empty()) return c.version;
        }
        return entry.version;
    };

    if (entry.name.empty()) {
        out.restoreClass = AcbAppRestoreClass::UnsupportedOrUnknown;
        out.message = L"Unsupported or unidentifiable application entry.";
        return out;
    }
    if (entry.isPortable && entry.hasKnownDataMap) {
        out.restoreClass = AcbAppRestoreClass::PortableAppAndData;
        out.candidateVersion = entry.version;
        out.message = L"Portable application with known data mapping — ready to restore portable configuration and data.";
        return out;
    }
    if (isInstalled()) {
        out.restoreClass = AcbAppRestoreClass::InstalledAppReadyForData;
        out.candidateVersion = entry.version;
        out.message = entry.hasKnownDataMap
            ? L"Installed on this PC — ready to restore mapped application data and settings."
            : L"Installed on this PC — no additional package installation required.";
        return out;
    }
    if (!entry.id.empty() && entry.installable && hasMgr(entry.managerName)) {
        out.restoreClass = AcbAppRestoreClass::PackageManagerReinstall;
        out.candidateVersion = findCandidateVer();
        out.versionMismatch = (!out.candidateVersion.empty() && !entry.version.empty() && out.candidateVersion != entry.version);
        out.message = out.versionMismatch
            ? L"Available through " + acbdetail::Show(entry.managerName) + L" (version " + acbdetail::Show(out.candidateVersion) +
              L" vs backed-up " + acbdetail::Show(entry.version) + L" — confirm before installing)."
            : L"Available to reinstall through " + acbdetail::Show(entry.managerName) + L" (" + acbdetail::Show(entry.source) + L").";
        return out;
    }
    if (entry.hasKnownDataMap || !entry.id.empty() || entry.managerName == "manual" || entry.source == "Local PC") {
        out.restoreClass = AcbAppRestoreClass::ManualInstallRequired;
        out.message = kAcbManualInstallMsg;
        return out;
    }
    out.restoreClass = AcbAppRestoreClass::UnsupportedOrUnknown;
    out.message = L"Unsupported or unknown restoration method (no verified package reference, portable descriptor, or data map).";
    return out;
}

inline AcbAppRestorePlan26 AcbPlanAppRestore26(const std::vector<AcbAppInventoryEntry26>& inventory,
                                               const std::vector<AcbPackage>& installedOnPc,
                                               const std::vector<AcbPackage>& catalogCandidates,
                                               const std::vector<std::string>& availableManagers) {
    AcbAppRestorePlan26 plan;
    for (const auto& e : inventory) {
        AcbAppRestoreItem26 it = AcbClassifyAppRestoration(e, installedOnPc, catalogCandidates, availableManagers);
        switch (it.restoreClass) {
        case AcbAppRestoreClass::PortableAppAndData:       ++plan.portableCount; break;
        case AcbAppRestoreClass::InstalledAppReadyForData: ++plan.installedCount; break;
        case AcbAppRestoreClass::PackageManagerReinstall:  ++plan.packageManagerCount; break;
        case AcbAppRestoreClass::ManualInstallRequired:    ++plan.manualCount; break;
        case AcbAppRestoreClass::UnsupportedOrUnknown:     ++plan.unsupportedCount; break;
        }
        plan.items.push_back(std::move(it));
    }
    return plan;
}

inline std::string AcbBuildAppInventoryJson26(const std::vector<AcbAppInventoryEntry26>& apps) {
    const auto q = [](const std::string& s) { return "\"" + JsonEsc(s) + "\""; };
    std::string j = "{\r\n  \"format\": \"ankhcore-app-inventory\",\r\n  \"version\": 2,\r\n  \"applications\": [";
    for (size_t i = 0; i < apps.size(); ++i) {
        const auto& a = apps[i];
        j += std::string(i ? "," : "") +
             "\r\n    { \"id\": " + q(a.id) +
             ", \"name\": " + q(a.name) +
             ", \"version\": " + q(a.version) +
             ", \"source\": " + q(a.source) +
             ", \"manager\": " + q(a.managerName) +
             ", \"installable\": " + (a.installable ? "true" : "false") +
             ", \"isPortable\": " + (a.isPortable ? "true" : "false") +
             ", \"hasKnownDataMap\": " + (a.hasKnownDataMap ? "true" : "false") +
             ", \"restoreClass\": " + q(AcbAppRestoreClassId(a.restoreClass)) + " }";
    }
    j += "\r\n  ]\r\n}\r\n";
    return j;
}

inline bool AcbParseAppInventoryJson26(const std::string& jsonText,
                                       std::vector<AcbAppInventoryEntry26>& out,
                                       std::wstring& err) {
    out.clear();
    JVal r;
    if (!JsonParse(jsonText, r) || !r.Is(JVal::Obj)) {
        err = L"Application inventory is not a valid JSON object.";
        return false;
    }
    const JVal* fmt = r.Get("format");
    if (!fmt || !fmt->Is(JVal::Str) || fmt->s != "ankhcore-app-inventory") {
        err = L"File is not an AnkhCore application inventory.";
        return false;
    }
    uint64_t ver = 0;
    if (!r.Get("version") || !r.Get("version")->AsUInt(ver) || ver < 1 || ver > 2) {
        err = L"Unsupported application inventory version.";
        return false;
    }
    const JVal* arr = r.Get("applications");
    if (!arr || !arr->Is(JVal::Arr)) {
        err = L"Application inventory is missing the 'applications' array.";
        return false;
    }
    for (const auto& item : arr->a) {
        if (!item.Is(JVal::Obj)) { err = L"Invalid application object in inventory."; return false; }
        AcbAppInventoryEntry26 e;
        if (const JVal* v = item.Get("id"); v && v->Is(JVal::Str)) e.id = v->s;
        if (const JVal* v = item.Get("name"); v && v->Is(JVal::Str)) e.name = v->s;
        if (const JVal* v = item.Get("version"); v && v->Is(JVal::Str)) e.version = v->s;
        if (const JVal* v = item.Get("source"); v && v->Is(JVal::Str)) e.source = v->s;
        if (const JVal* v = item.Get("manager"); v && v->Is(JVal::Str)) e.managerName = v->s;
        if (const JVal* v = item.Get("installable"); v && v->Is(JVal::Bool)) e.installable = v->b;
        if (const JVal* v = item.Get("isPortable"); v && v->Is(JVal::Bool)) e.isPortable = v->b;
        if (const JVal* v = item.Get("hasKnownDataMap"); v && v->Is(JVal::Bool)) e.hasKnownDataMap = v->b;
        if (const JVal* v = item.Get("restoreClass"); v && v->Is(JVal::Str)) {
            e.restoreClass = AcbParseAppRestoreClassId(v->s);
        } else {
            e.restoreClass = e.isPortable && e.hasKnownDataMap
                ? AcbAppRestoreClass::PortableAppAndData
                : (e.installable ? AcbAppRestoreClass::PackageManagerReinstall
                                 : (!e.name.empty() ? AcbAppRestoreClass::ManualInstallRequired
                                                    : AcbAppRestoreClass::UnsupportedOrUnknown));
        }
        out.push_back(std::move(e));
    }
    return true;
}

// ---------------------------------------------------------------- 2. Mapped Registry / Tweak Settings & OS/Hardware Compatibility
struct AcbMappedRegValue {
    std::string tweakId;
    std::string title;
    std::string category;
    std::string root = "HKCU";        // "HKCU" | "HKLM"
    std::string subkey;               // exact mapped registry subkey
    std::string valueName;            // exact value name
    uint32_t regType = REG_DWORD;     // original value type: REG_DWORD, REG_SZ, REG_EXPAND_SZ, REG_QWORD
    bool existedBefore = false;
    uint64_t numValue = 0;            // for REG_DWORD / REG_QWORD
    std::string strValue;             // for REG_SZ / REG_EXPAND_SZ
    uint64_t origNumValue = 0;
    std::string origStrValue;
    int minOsBuild = 0;               // e.g. 22000 for Windows 11-only tweaks
    bool requiresWindows11 = false;
    std::string requiredDeviceModel;  // empty = any supported PC; non-empty = specific device model
};

// Ensures a mapped registry setting never touches security hives, services/drivers, or shell hijack keys.
inline bool AcbIsSafeMappedRegEntry(const AcbMappedRegValue& e, std::wstring& err) {
    err.clear();
    if (e.tweakId.empty() || e.subkey.empty() || e.valueName.empty()) {
        err = L"Mapped registry entry is missing tweakId, subkey, or valueName.";
        return false;
    }
    if (e.root != "HKCU" && e.root != "HKLM") {
        err = L"Mapped registry root must be HKCU or HKLM.";
        return false;
    }
    if (e.regType != REG_DWORD && e.regType != REG_SZ && e.regType != REG_EXPAND_SZ && e.regType != REG_QWORD) {
        err = L"Unsupported registry value type in backup (only REG_DWORD, REG_SZ, REG_EXPAND_SZ, REG_QWORD are permitted).";
        return false;
    }
    if (e.regType == REG_DWORD && (e.numValue > 0xFFFFFFFFull || e.origNumValue > 0xFFFFFFFFull)) {
        err = L"REG_DWORD value exceeds 32-bit range.";
        return false;
    }
    if (e.subkey.front() == '\\' || e.subkey.front() == '/' || e.subkey.back() == '\\' || e.subkey.back() == '/' ||
        e.subkey.find("..") != std::string::npos || e.subkey.find("\\\\") != std::string::npos || e.subkey.find("//") != std::string::npos) {
        err = L"Unsafe registry subkey path.";
        return false;
    }
    for (char c : e.subkey) {
        if (static_cast<unsigned char>(c) < 0x20) { err = L"Control character in registry subkey."; return false; }
    }
    std::string low = e.subkey;
    for (char& c : low) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c == '/') c = '\\';
    }
    static const char* const kForbiddenKeyPrefixes[] = {
        "sam", "security", "system\\currentcontrolset\\services",
        "software\\microsoft\\windows nt\\currentversion\\winlogon",
        "software\\microsoft\\windows nt\\currentversion\\image file execution options",
        "software\\policies\\microsoft\\windows defender",
        "software\\microsoft\\windows defender"
    };
    for (const char* pre : kForbiddenKeyPrefixes) {
        if (low == pre || low.rfind(std::string(pre) + "\\", 0) == 0) {
            err = L"Protected security, service, driver, or boot registry keys are prohibited in tweak backups.";
            return false;
        }
    }
    if (low.rfind("system\\controlset", 0) == 0 && low.find("\\services") != std::string::npos) {
        err = L"Protected service or driver registry keys are prohibited in tweak backups.";
        return false;
    }
    return true;
}

inline std::string AcbBuildTweakBackupJson(const std::vector<AcbMappedRegValue>& entries) {
    const auto q = [](const std::string& s) { return "\"" + JsonEsc(s) + "\""; };
    std::string j = "{\r\n  \"format\": \"ankhcore-tweak-backup\",\r\n  \"version\": 1,\r\n  \"entries\": [";
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        j += std::string(i ? "," : "") +
             "\r\n    { \"tweakId\": " + q(e.tweakId) +
             ", \"title\": " + q(e.title) +
             ", \"category\": " + q(e.category) +
             ", \"root\": " + q(e.root) +
             ", \"subkey\": " + q(e.subkey) +
             ", \"valueName\": " + q(e.valueName) +
             ", \"regType\": " + std::to_string(e.regType) +
             ", \"existedBefore\": " + (e.existedBefore ? "true" : "false") +
             ", \"numValue\": " + std::to_string(e.numValue) +
             ", \"strValue\": " + q(e.strValue) +
             ", \"origNumValue\": " + std::to_string(e.origNumValue) +
             ", \"origStrValue\": " + q(e.origStrValue) +
             ", \"minOsBuild\": " + std::to_string(e.minOsBuild) +
             ", \"requiresWindows11\": " + (e.requiresWindows11 ? "true" : "false") +
             ", \"requiredDeviceModel\": " + q(e.requiredDeviceModel) + " }";
    }
    j += "\r\n  ]\r\n}\r\n";
    return j;
}

inline bool AcbParseTweakBackupJson(const std::string& jsonText,
                                    std::vector<AcbMappedRegValue>& out,
                                    std::wstring& err) {
    out.clear();
    JVal r;
    if (!JsonParse(jsonText, r) || !r.Is(JVal::Obj)) {
        err = L"Tweak backup is not a valid JSON object.";
        return false;
    }
    const JVal* fmt = r.Get("format");
    if (!fmt || !fmt->Is(JVal::Str) || fmt->s != "ankhcore-tweak-backup") {
        err = L"File is not an AnkhCore tweak backup manifest.";
        return false;
    }
    uint64_t ver = 0;
    if (!r.Get("version") || !r.Get("version")->AsUInt(ver) || ver != 1) {
        err = L"Unsupported tweak backup format version.";
        return false;
    }
    const JVal* arr = r.Get("entries");
    if (!arr || !arr->Is(JVal::Arr)) {
        err = L"Tweak backup is missing the 'entries' array.";
        return false;
    }
    for (const auto& item : arr->a) {
        if (!item.Is(JVal::Obj)) { err = L"Invalid tweak entry object."; return false; }
        AcbMappedRegValue e;
        if (const JVal* v = item.Get("tweakId"); v && v->Is(JVal::Str)) e.tweakId = v->s;
        if (const JVal* v = item.Get("title"); v && v->Is(JVal::Str)) e.title = v->s;
        if (const JVal* v = item.Get("category"); v && v->Is(JVal::Str)) e.category = v->s;
        if (const JVal* v = item.Get("root"); v && v->Is(JVal::Str)) e.root = v->s;
        if (const JVal* v = item.Get("subkey"); v && v->Is(JVal::Str)) e.subkey = v->s;
        if (const JVal* v = item.Get("valueName"); v && v->Is(JVal::Str)) e.valueName = v->s;
        uint64_t rt = 0;
        if (!item.Get("regType") || !item.Get("regType")->AsUInt(rt)) {
            err = L"Tweak entry is missing numeric regType.";
            return false;
        }
        e.regType = static_cast<uint32_t>(rt);
        if (const JVal* v = item.Get("existedBefore"); v && v->Is(JVal::Bool)) e.existedBefore = v->b;
        if (const JVal* v = item.Get("numValue"); v) v->AsUInt(e.numValue);
        if (const JVal* v = item.Get("strValue"); v && v->Is(JVal::Str)) e.strValue = v->s;
        if (const JVal* v = item.Get("origNumValue"); v) v->AsUInt(e.origNumValue);
        if (const JVal* v = item.Get("origStrValue"); v && v->Is(JVal::Str)) e.origStrValue = v->s;
        uint64_t minB = 0;
        if (const JVal* v = item.Get("minOsBuild"); v && v->AsUInt(minB)) e.minOsBuild = static_cast<int>(minB);
        if (const JVal* v = item.Get("requiresWindows11"); v && v->Is(JVal::Bool)) e.requiresWindows11 = v->b;
        if (const JVal* v = item.Get("requiredDeviceModel"); v && v->Is(JVal::Str)) e.requiredDeviceModel = v->s;
        if (!AcbIsSafeMappedRegEntry(e, err)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(e));
    }
    return true;
}

struct AcbHostCompatibilityContext {
    int osBuild = 26300;
    std::string arch = "x64";
    std::string deviceModel;
};

struct AcbSkippedTweakInfo {
    AcbMappedRegValue entry;
    std::wstring reason;
};

struct AcbCompatibilityReport {
    bool archCompatible = true;
    bool osGenerationChanged = false;   // e.g. Windows 11 backup being restored on Windows 10
    std::vector<AcbMappedRegValue> compatibleTweaks;
    std::vector<AcbSkippedTweakInfo> skippedTweaks;
    std::vector<std::wstring> warnings;
};

// Hardware & OS compatibility gate before reapplying backed-up settings and mapped registry tweaks (2.6 §4.1).
inline AcbCompatibilityReport AcbCheckRestoreCompatibility(const AcbManifest& manifest,
                                                           const std::vector<AcbMappedRegValue>& tweaks,
                                                           const AcbHostCompatibilityContext& host) {
    AcbCompatibilityReport rep;
    if (!manifest.arch.empty() && !host.arch.empty() && manifest.arch != host.arch) {
        rep.archCompatible = false;
        rep.warnings.push_back(L"Architecture mismatch: backup was created on " + acbdetail::Show(manifest.arch) +
                               L", current host is " + acbdetail::Show(host.arch) + L".");
    }
    const bool srcWin11 = manifest.osBuild >= 22000;
    const bool hostWin11 = host.osBuild >= 22000;
    if (srcWin11 != hostWin11) {
        rep.osGenerationChanged = true;
        rep.warnings.push_back(L"OS version change detected: backup was created on build " +
                               std::to_wstring(manifest.osBuild) + L" (" + acbdetail::Show(manifest.osName) +
                               L"), current host is build " + std::to_wstring(host.osBuild) +
                               L". OS-specific settings will be re-evaluated.");
    }
    for (const auto& t : tweaks) {
        if (!rep.archCompatible) {
            rep.skippedTweaks.push_back({t, L"Skipped due to CPU architecture mismatch."});
            continue;
        }
        if (t.requiresWindows11 && host.osBuild < 22000) {
            rep.skippedTweaks.push_back({t, L"Requires Windows 11 (build 22000+); current host is build " + std::to_wstring(host.osBuild) + L"."});
            continue;
        }
        if (t.minOsBuild > 0 && host.osBuild < t.minOsBuild) {
            rep.skippedTweaks.push_back({t, L"Requires Windows build " + std::to_wstring(t.minOsBuild) + L" or newer; current host is build " + std::to_wstring(host.osBuild) + L"."});
            continue;
        }
        if (!t.requiredDeviceModel.empty() && _stricmp(t.requiredDeviceModel.c_str(), host.deviceModel.c_str()) != 0) {
            rep.skippedTweaks.push_back({t, L"Device-specific setting for '" + acbdetail::Show(t.requiredDeviceModel) +
                                            L"' skipped on '" + (host.deviceModel.empty() ? std::wstring(L"Unknown device") : acbdetail::Show(host.deviceModel)) + L"'."});
            continue;
        }
        rep.compatibleTweaks.push_back(t);
    }
    return rep;
}

// ---------------------------------------------------------------- 3. Non-destructive Pre-Restore Preview
struct AcbPreviewFileItem {
    AcbFile file;
    std::wstring resolvedDestPath;
    std::string restoreAction;
    bool writesToDisk = false;
    bool destinationExists = false;
};

struct AcbRestorePreview {
    bool ok = false;
    std::wstring error;
    AcbManifest manifest;
    std::vector<std::string> unsupportedCategories;
    std::vector<AcbPreviewFileItem> selectedFiles;
    uint64_t totalSelectedBytes = 0;
    int diskWriteCount = 0;
    int referenceOnlyCount = 0;
    AcbCompatibilityReport compatibility;
    std::vector<AcbAppInventoryEntry26> appInventory;
};

inline AcbRestorePreview AcbPreviewRestore(const uint8_t* raw, size_t size,
                                           const AcbRoots& roots,
                                           const AcbHostCompatibilityContext& host,
                                           const AcbRestoreOptions& opts = {}) {
    AcbRestorePreview pv;
    const AcbCheck chk = AcbVerify(raw, size);
    if (!chk.ok) {
        pv.error = chk.error;
        return pv;
    }
    pv.manifest = chk.manifest;
    pv.unsupportedCategories = chk.unsupported;

    const std::vector<AcbFile> sel = AcbSelect(chk.manifest, opts.categories, opts.settingsOnly);
    std::vector<AcbMappedRegValue> tweaks;
    for (const auto& f : sel) {
        AcbPreviewFileItem item;
        item.file = f;
        if (!AcbResolveDest(roots, f.dest, item.resolvedDestPath)) {
            pv.error = L"Unsafe destination in backup: " + acbdetail::Show(f.dest);
            return pv;
        }
        const AcbCatSpec* spec = AcbFindCat(f.category);
        item.restoreAction = spec ? spec->restore : "";
        item.writesToDisk = !item.resolvedDestPath.empty() && (item.restoreAction == "settings-merge" || item.restoreAction == "file-copy");
        item.destinationExists = item.writesToDisk && FileExists(item.resolvedDestPath);
        if (item.writesToDisk) ++pv.diskWriteCount;
        else ++pv.referenceOnlyCount;
        pv.totalSelectedBytes += f.size;
        pv.selectedFiles.push_back(std::move(item));

        if (f.category == "tweaks" && f.path == "files/tweaks/tweaks.json") {
            std::vector<uint8_t> tBytes;
            std::wstring tErr;
            if (!AcbExtract(raw, size, chk, f, tBytes, tErr) ||
                !AcbParseTweakBackupJson(std::string(tBytes.begin(), tBytes.end()), tweaks, tErr)) {
                pv.error = tErr.empty() ? L"Invalid tweak backup manifest in archive." : tErr;
                return pv;
            }
        }
        if (f.category == "apps" && f.path == "files/apps/inventory.json") {
            std::vector<uint8_t> aBytes;
            std::wstring aErr;
            if (!AcbExtract(raw, size, chk, f, aBytes, aErr) ||
                !AcbParseAppInventoryJson26(std::string(aBytes.begin(), aBytes.end()), pv.appInventory, aErr)) {
                pv.error = aErr.empty() ? L"Invalid application inventory in archive." : aErr;
                return pv;
            }
        }
    }
    pv.compatibility = AcbCheckRestoreCompatibility(chk.manifest, tweaks, host);
    pv.ok = true;
    return pv;
}

}  // namespace ak
