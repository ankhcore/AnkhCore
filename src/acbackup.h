// AnkhCore 2.5 — the .acbackup container (Backup & Migration, BAK-1): a strict ZIP subset, the
// manifest, SHA-256 per file and the validation that runs before anything is restored.
// Pure logic on memory buffers; unit-tested in tests/test_base.cpp. Design: docs/design/BACKUP_MIGRATION.md.
// Nothing here touches the file system, the registry or another process.
#pragma once
#include "install.h"
#include "inflate.h"
#include "json_min.h"
#include <set>
#include <cwctype>

namespace ak {

constexpr int kAcbFormatVersion = 1;
constexpr size_t kAcbMaxEntries = 65000;              // no ZIP64 in format version 1
constexpr size_t kAcbMaxManifest = 16u << 20;
constexpr uint64_t kAcbMaxBytes = 0xFFFFFFFEull;      // per file and per archive

namespace acbdetail {
inline uint16_t R16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t R32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline void W16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
inline void W32(std::vector<uint8_t>& o, uint32_t v) { W16(o, v & 0xFFFF); W16(o, v >> 16); }
// Strict UTF-8 to UTF-16: invalid sequences fail instead of becoming U+FFFD.
inline bool Wide(const std::string& s, std::wstring& w) {
    w.clear();
    if (s.empty()) return true;
    if (s.size() > 0x7FFFFFFF) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return true;
}
inline std::wstring Fold(std::wstring w) { if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size())); return w; }
inline std::wstring Show(const std::string& s) { std::wstring w; return Wide(s, w) ? w : std::wstring(L"(unreadable name)"); }
inline void FillCrc(uint32_t* t) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } }
}  // namespace acbdetail

inline uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t t[256]; static const bool init = (acbdetail::FillCrc(t), true); (void)init;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- paths
// A relative archive or destination path that cannot leave its folder on Windows: SafeRelPath
// (no absolute paths, no '\', ':', '.', '..', control or wildcard characters) plus no reserved device
// names and no segment ending in a dot or a space (Windows strips those, which would alias names).
inline bool AcbSafePath(const std::string& utf8) {
    std::wstring w;
    if (!acbdetail::Wide(utf8, w) || !SafeRelPath(w)) return false;
    size_t start = 0;
    for (;;) {
        const size_t end = w.find(L'/', start);
        std::wstring seg = w.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (seg.back() == L'.' || seg.back() == L' ') return false;
        std::wstring stem = seg.substr(0, seg.find(L'.'));
        while (!stem.empty() && stem.back() == L' ') stem.pop_back();
        for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return false;
        if (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'1' && stem[3] <= L'9') return false;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}
// Lower-case extension of the last segment including the dot (".json"), or "" when there is none.
inline std::string AcbExt(const std::string& path) {
    const size_t slash = path.rfind('/'); const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    std::string e = path.substr(dot);
    for (auto& c : e) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return e;
}

// ---------------------------------------------------------------- 2.6 data path safety (secrets, credentials, binaries)
enum class AcbDataPathSafety {
    Safe = 0,
    UnsafePath,
    ForbiddenBinaryOrExecutable,
    ForbiddenSecretOrCredential
};

// Classifies a relative archive or destination path for application-data / user-configuration backup & restore (2.6 §4.1):
// - Rejects any path that fails AcbSafePath.
// - Rejects executable, driver, service, installer, script, and raw registry-hive files (.exe, .dll, .sys, .drv, .msi, .ps1, .bat, .cmd, .reg, .hiv, ...).
// - Rejects secrets, credentials, tokens, private keys, keystores, browser password/cookie stores, and DPAPI/vault state.
inline AcbDataPathSafety AcbCheckDataPathSafety(const std::string& relPath, std::wstring& reason) {
    reason.clear();
    if (!AcbSafePath(relPath)) {
        reason = L"Path is not a safe relative backup path.";
        return AcbDataPathSafety::UnsafePath;
    }
    const std::string ext = AcbExt(relPath);
    static const char* const kForbiddenBinaryExts[] = {
        ".exe", ".dll", ".sys", ".drv", ".ocx", ".cpl", ".scr",
        ".msi", ".msp", ".msix", ".appx", ".bat", ".cmd", ".ps1",
        ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".com",
        ".hiv", ".reg", ".cat", ".inf"
    };
    for (const char* bExt : kForbiddenBinaryExts) {
        if (ext == bExt) {
            reason = L"Executables, drivers, installers, scripts, and raw registry files are never backed up or restored as application data.";
            return AcbDataPathSafety::ForbiddenBinaryOrExecutable;
        }
    }
    static const char* const kForbiddenSecretExts[] = {
        ".pfx", ".p12", ".pem", ".key", ".der", ".cer", ".crt",
        ".kdbx", ".kdb", ".jks", ".keystore", ".p7b", ".p7c", ".ovpn"
    };
    for (const char* sExt : kForbiddenSecretExts) {
        if (ext == sExt) {
            reason = L"Private keys, certificates, and password vaults are excluded to protect secrets and credentials.";
            return AcbDataPathSafety::ForbiddenSecretOrCredential;
        }
    }

    std::string low = relPath;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);

    size_t start = 0;
    while (start < low.size()) {
        const size_t slash = low.find('/', start);
        const std::string seg = low.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const size_t dot = seg.find('.');
        const std::string stem = (dot == std::string::npos) ? seg : seg.substr(0, dot);

        // Exact segment or stem matches for credential/secret/session/hive files and folders
        static const char* const kSecretExact[] = {
            "id_rsa", "id_ed25519", "id_ecdsa", "id_dsa", "known_hosts", "authorized_keys",
            "credentials", "credential", "secrets", "secret", "token", "tokens",
            "oauth", "oauth2", "apikey", "api_key", "api-key", "auth_token", "access_token", "refresh_token",
            "password", "passwords", "passwd", "shadow", "masterkey",
            "login data", "web data", "cookies", "local state", "network persistent state",
            "key3", "key4", "logins", "cert9", "signons",
            "dpapi", "vault", "protect", "ntuser", "usrclass", "sam", "security"
        };
        for (const char* s : kSecretExact) {
            if (seg == s || stem == s) {
                reason = L"Credentials, tokens, cookies, and machine-specific security files are excluded from backup and restore.";
                return AcbDataPathSafety::ForbiddenSecretOrCredential;
            }
        }
        // Substring tokens inside segment names indicating secret/credential/token material
        static const char* const kSecretSubstrings[] = {
            "credential", "secret", "password", "passwd", "apikey", "api_key", "api-key",
            "access_token", "refresh_token", "auth_token", "bearer_token", "oauth_token",
            "private_key", "privatekey", "client_secret", "master_key", "cookies"
        };
        for (const char* sub : kSecretSubstrings) {
            if (seg.find(sub) != std::string::npos) {
                reason = L"Path contains secret, credential, or authentication token material.";
                return AcbDataPathSafety::ForbiddenSecretOrCredential;
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return AcbDataPathSafety::Safe;
}

inline bool AcbIsSafeBackupDataPath(const std::string& relPath, std::wstring* reasonOut = nullptr) {
    std::wstring r;
    const AcbDataPathSafety s = AcbCheckDataPathSafety(relPath, r);
    if (reasonOut) *reasonOut = r;
    return s == AcbDataPathSafety::Safe;
}

// ---------------------------------------------------------------- ZIP subset
// What AnkhCore writes and therefore all it accepts: no encryption, no ZIP64, one disk, no data
// descriptors, no directory entries, no archive comment; methods 0 (stored) and 8 (deflate).
struct ZipItem {
    std::string name; std::vector<uint8_t> data;
    bool deflated = false; uint32_t usize = 0, crc = 0;   // deflated: data is a raw deflate stream of usize bytes with this CRC
};
inline bool ZipWrite(const std::vector<ZipItem>& items, uint16_t dosTime, uint16_t dosDate, std::vector<uint8_t>& out) {
    using namespace acbdetail;
    out.clear();
    if (items.empty() || items.size() > kAcbMaxEntries) return false;
    struct Rec { uint32_t off, crc, csize, usize; };
    std::vector<Rec> recs;
    uint64_t total = 22;
    for (const auto& it : items) total += 30 + 46 + 2 * static_cast<uint64_t>(it.name.size()) + it.data.size();
    if (total > kAcbMaxBytes) return false;
    out.reserve(static_cast<size_t>(total));
    for (const auto& it : items) {
        if (it.name.empty() || it.name.size() > 0xFFFF) { out.clear(); return false; }
        const Rec r{static_cast<uint32_t>(out.size()), it.deflated ? it.crc : Crc32(it.data.data(), it.data.size()),
                    static_cast<uint32_t>(it.data.size()), it.deflated ? it.usize : static_cast<uint32_t>(it.data.size())};
        W32(out, 0x04034b50); W16(out, 20); W16(out, 0x0800); W16(out, it.deflated ? 8 : 0); W16(out, dosTime); W16(out, dosDate);
        W32(out, r.crc); W32(out, r.csize); W32(out, r.usize); W16(out, static_cast<uint32_t>(it.name.size())); W16(out, 0);
        out.insert(out.end(), it.name.begin(), it.name.end());
        out.insert(out.end(), it.data.begin(), it.data.end());
        recs.push_back(r);
    }
    const size_t cd = out.size();
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i]; const Rec& r = recs[i];
        W32(out, 0x02014b50); W16(out, 20); W16(out, 20); W16(out, 0x0800); W16(out, it.deflated ? 8 : 0); W16(out, dosTime); W16(out, dosDate);
        W32(out, r.crc); W32(out, r.csize); W32(out, r.usize); W16(out, static_cast<uint32_t>(it.name.size())); W16(out, 0); W16(out, 0);
        W16(out, 0); W16(out, 0); W32(out, 0); W32(out, r.off);
        out.insert(out.end(), it.name.begin(), it.name.end());
    }
    const size_t cdSize = out.size() - cd;
    W32(out, 0x06054b50); W16(out, 0); W16(out, 0); W16(out, static_cast<uint32_t>(items.size())); W16(out, static_cast<uint32_t>(items.size()));
    W32(out, static_cast<uint32_t>(cdSize)); W32(out, static_cast<uint32_t>(cd)); W16(out, 0);
    return true;
}

struct ZipEntry { std::string name; uint16_t method = 0; uint32_t crc = 0, csize = 0, usize = 0; size_t dataOff = 0; };
// Reads and checks the whole directory. Any rule broken refuses the archive; nothing is decompressed here.
inline bool ZipList(const uint8_t* p, size_t n, std::vector<ZipEntry>& out, std::wstring& err) {
    using namespace acbdetail;
    out.clear();
    const auto bad = [&](const wchar_t* m) { err = m; out.clear(); return false; };
    if (!p || n < 22 || n > kAcbMaxBytes) return bad(L"The file is not a backup archive (it is too small or too large).");
    const uint8_t* e = p + n - 22;
    if (R32(e) != 0x06054b50 || R16(e + 20) != 0) return bad(L"The file is not a backup archive, or it is cut off or has data added after its end.");
    if (R16(e + 4) != 0 || R16(e + 6) != 0) return bad(L"The archive is split over several disks, which a backup never is.");
    const uint32_t count = R16(e + 10), cdSize = R32(e + 12), cdOff = R32(e + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOff == 0xFFFFFFFFu) return bad(L"The archive uses ZIP64, which this backup format does not.");
    if (R16(e + 8) != count || static_cast<uint64_t>(cdOff) + cdSize != n - 22) return bad(L"The archive directory is damaged.");
    if (count == 0 || count > kAcbMaxEntries) return bad(L"The archive is empty or holds too many entries.");
    size_t pos = cdOff; const size_t cdEnd = n - 22;
    std::set<std::wstring> seen; std::vector<std::pair<size_t, size_t>> ranges;
    for (uint32_t i = 0; i < count; ++i) {
        if (cdEnd - pos < 46 || R32(p + pos) != 0x02014b50) return bad(L"The archive directory is damaged.");
        const uint8_t* c = p + pos;
        const uint16_t flags = R16(c + 8), method = R16(c + 10), nlen = R16(c + 28), xlen = R16(c + 30), clen = R16(c + 32);
        const uint32_t crc = R32(c + 16), csize = R32(c + 20), usize = R32(c + 24), lho = R32(c + 42);
        if (cdEnd - pos - 46 < static_cast<size_t>(nlen) + xlen + clen) return bad(L"The archive directory is damaged.");
        if (flags & 0x0001) return bad(L"The archive holds an encrypted entry, which a backup never does.");
        if (flags & 0x0008) return bad(L"The archive uses a layout AnkhCore does not write (data descriptors).");
        if (method != 0 && method != 8) return bad(L"The archive uses a compression method a backup never does.");
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || lho == 0xFFFFFFFFu) return bad(L"The archive uses ZIP64, which this backup format does not.");
        if (R16(c + 34) != 0) return bad(L"The archive is split over several disks, which a backup never is.");
        if (method == 0 ? csize != usize : static_cast<uint64_t>(usize) > static_cast<uint64_t>(csize) * 1032 + 64) return bad(L"An entry declares an impossible size.");
        std::string name(reinterpret_cast<const char*>(c + 46), nlen); std::wstring wname;
        if (name.empty() || name.back() == '/' || !Wide(name, wname)) return bad(L"The archive holds an entry with an invalid name.");
        if (!seen.insert(Fold(wname)).second) return bad(L"The archive holds two entries with the same name.");
        if (static_cast<uint64_t>(lho) + 30 > cdOff) return bad(L"An entry points outside the archive.");
        const uint8_t* l = p + lho;
        if (R32(l) != 0x04034b50) return bad(L"An entry points outside the archive.");
        const uint16_t lnlen = R16(l + 26), lxlen = R16(l + 28);
        const uint64_t dataOff = static_cast<uint64_t>(lho) + 30 + lnlen + lxlen;
        if (dataOff + csize > cdOff) return bad(L"An entry points outside the archive.");
        if (R16(l + 6) != flags || R16(l + 8) != method || R32(l + 14) != crc || R32(l + 18) != csize || R32(l + 22) != usize || lnlen != nlen || memcmp(l + 30, c + 46, nlen) != 0)
            return bad(L"An entry is described differently in two places of the archive.");
        ranges.push_back({lho, static_cast<size_t>(dataOff + csize)});
        ZipEntry ze; ze.name = std::move(name); ze.method = method; ze.crc = crc; ze.csize = csize; ze.usize = usize; ze.dataOff = static_cast<size_t>(dataOff);
        out.push_back(std::move(ze));
        pos += 46 + static_cast<size_t>(nlen) + xlen + clen;
    }
    if (pos != cdEnd) return bad(L"The archive directory is damaged.");
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) if (ranges[i].first < ranges[i - 1].second) return bad(L"Two entries of the archive overlap.");
    return true;
}
// One entry's content, CRC-checked. e must come from ZipList of the same buffer.
inline bool ZipRead(const uint8_t* p, size_t n, const ZipEntry& e, std::vector<uint8_t>& out, std::wstring& err) {
    out.clear();
    if (static_cast<uint64_t>(e.dataOff) + e.csize > n) { err = L"An entry points outside the archive."; return false; }
    out.resize(e.usize);
    bool ok = true;
    if (e.method == 0) { if (e.usize) memcpy(out.data(), p + e.dataOff, e.usize); }
    else if (e.usize) ok = Inflater::Run(p + e.dataOff, e.csize, out.data(), e.usize);
    if (!ok || Crc32(out.data(), out.size()) != e.crc) { out.clear(); err = L"\"" + acbdetail::Show(e.name) + L"\" is damaged (its checksum does not match)."; return false; }
    return true;
}

// ---------------------------------------------------------------- categories
// exts: the file types a category may hold, space-separated; "" = a reserved id this version neither
// writes nor restores. root: the only destination root its files may name. restore: the action (design §5).
struct AcbCatSpec { const char* id; const char* title; const char* restore; const char* exts; const char* root; bool settingsOnly; };
inline const std::vector<AcbCatSpec>& AcbCategories() {
    static const std::vector<AcbCatSpec> v{
        {"settings",   "AnkhCore settings and preferences",     "settings-merge", ".json",                                     "appdata", true},
        {"monitoring", "Network totals and monitoring data",    "file-copy",      ".txt .json",                                "data",    true},
        {"history",    "Operation history and logs",            "info",           ".txt .log .json",                           "none",    false},
        {"cursors",    "Cursor packs",                          "file-copy",      ".cur .ani",                                 "content", false},
        {"wallpapers", "Wallpapers",                            "file-copy",      ".jpg .jpeg .png .bmp",                      "content", false},
        {"apps",       "Application list (UniGetUI bundle)",    "reference",      ".ubundle .json",                            "none",    false},
        {"iconpacks",  "Icon packs",                            "", "", "", false},
        {"themes",     "Themes",                                "", "", "", false},
        {"windhawk",   "Windhawk configuration",                "", "", "", false},                                   // reserved until WH-9
        {"tweaks",     "Tweak configuration (mapped values)",   "reference",      ".json",                                     "none",    false},   // 2.6: read for preview/compatibility only, never written back by restore
        {"appdata",    "Application data",                      "", "", "", false},                                   // reserved until a per-app data map exists (Q5); AcbCheckDataPathSafety gates it when enabled
    };
    return v;
}
inline const AcbCatSpec* AcbFindCat(const std::string& id) { for (const auto& c : AcbCategories()) if (id == c.id) return &c; return nullptr; }
// A category this version can restore.
inline bool AcbSupported(const std::string& id) { const AcbCatSpec* c = AcbFindCat(id); return c && *c->exts; }
inline bool AcbExtAllowed(const AcbCatSpec& c, const std::string& ext) { return !ext.empty() && (" " + std::string(c.exts) + " ").find(" " + ext + " ") != std::string::npos; }

// ---------------------------------------------------------------- manifest
struct AcbCategory { std::string id, title, restore; };
struct AcbFile { std::string path, category, sha256, dest; uint64_t size = 0; };
struct AcbManifest {
    int formatVersion = kAcbFormatVersion;
    std::string kind = "full";                  // full | settings | custom
    std::string created;                        // 2026-11-11T12:00:00Z
    std::string appVersion, appLabel, osName, arch; int osBuild = 0;
    std::vector<AcbCategory> categories; std::vector<AcbFile> files;
    std::string bundle;                         // archive path of the UniGetUI bundle, or ""
    std::vector<std::string> notes;
};
inline bool AcbStampOk(const std::string& s) {
    if (s.size() != 20) return false;
    static const char* shape = "dddd-dd-ddTdd:dd:ddZ";
    for (int i = 0; i < 20; ++i) if (shape[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != shape[i]) return false;
    const auto num = [&](int at, int len) { int v = 0; for (int i = 0; i < len; ++i) v = v * 10 + (s[at + i] - '0'); return v; };
    return num(0, 4) >= 2000 && num(0, 4) <= 2107 && num(5, 2) >= 1 && num(5, 2) <= 12 && num(8, 2) >= 1 && num(8, 2) <= 31 && num(11, 2) <= 23 && num(14, 2) <= 59 && num(17, 2) <= 59;
}
// "root:relative/path". Roots: appdata, data, content; "none:" for files that are never written back.
inline bool AcbSplitDest(const std::string& dest, std::string& root, std::string& rel) {
    const size_t c = dest.find(':');
    if (c == std::string::npos) return false;
    root = dest.substr(0, c); rel = dest.substr(c + 1);
    if (root == "none") return rel.empty();
    return (root == "appdata" || root == "data" || root == "content") && AcbSafePath(rel);
}
inline std::string AcbManifestJson(const AcbManifest& m) {
    const auto q = [](const std::string& s) { return "\"" + JsonEsc(s) + "\""; };
    std::string j = "{\n  \"format\": \"acbackup\",\n  \"formatVersion\": " + std::to_string(m.formatVersion) + ",\n  \"kind\": " + q(m.kind) + ",\n  \"created\": " + q(m.created) +
        ",\n  \"app\": { \"version\": " + q(m.appVersion) + ", \"label\": " + q(m.appLabel) + " },\n  \"source\": { \"os\": " + q(m.osName) + ", \"build\": " + std::to_string(m.osBuild) + ", \"arch\": " + q(m.arch) + " },\n  \"categories\": [";
    for (size_t i = 0; i < m.categories.size(); ++i) { const auto& c = m.categories[i]; j += std::string(i ? "," : "") + "\n    { \"id\": " + q(c.id) + ", \"title\": " + q(c.title) + ", \"restore\": " + q(c.restore) + " }"; }
    j += "\n  ],\n  \"files\": [";
    for (size_t i = 0; i < m.files.size(); ++i) { const auto& f = m.files[i]; j += std::string(i ? "," : "") + "\n    { \"path\": " + q(f.path) + ", \"category\": " + q(f.category) + ", \"size\": " + std::to_string(f.size) + ", \"sha256\": " + q(f.sha256) + ", \"dest\": " + q(f.dest) + " }"; }
    j += "\n  ]";
    if (!m.bundle.empty()) j += ",\n  \"bundle\": " + q(m.bundle);
    j += ",\n  \"notes\": [";
    for (size_t i = 0; i < m.notes.size(); ++i) j += std::string(i ? ", " : "") + q(m.notes[i]);
    j += "]\n}\n";
    return j;
}
// Reads the manifest's structure. A newer format version is refused here, before fields it may have changed are read.
inline bool AcbParseManifest(const std::string& json, AcbManifest& m, std::wstring& err) {
    m = AcbManifest{};
    const auto bad = [&](const wchar_t* t) { err = t; return false; };
    static const wchar_t* kDamaged = L"The backup's manifest is damaged or incomplete.";
    std::wstring w; JVal r;
    if (json.size() > kAcbMaxManifest || !acbdetail::Wide(json, w) || !JsonParse(json, r) || !r.Is(JVal::Obj)) return bad(kDamaged);
    const auto str = [](const JVal* o, const char* k, std::string& out) { const JVal* v = o ? o->Get(k) : nullptr; if (!v || !v->Is(JVal::Str)) return false; out = v->s; return true; };
    std::string fmt; uint64_t fv = 0;
    if (!str(&r, "format", fmt) || fmt != "acbackup") return bad(L"This file is not an AnkhCore backup.");
    if (!r.Get("formatVersion") || !r.Get("formatVersion")->AsUInt(fv) || fv < 1 || fv > 1000000) return bad(kDamaged);
    m.formatVersion = static_cast<int>(fv);
    if (m.formatVersion > kAcbFormatVersion) return bad(L"This backup was made by a newer AnkhCore. Update AnkhCore to open it.");
    const JVal* app = r.Get("app"); const JVal* src = r.Get("source"); const JVal* cats = r.Get("categories"); const JVal* files = r.Get("files");
    uint64_t build = 0;
    if (!str(&r, "kind", m.kind) || !str(&r, "created", m.created) || !str(app, "version", m.appVersion) || !str(app, "label", m.appLabel) ||
        !str(src, "os", m.osName) || !str(src, "arch", m.arch) || !src->Get("build") || !src->Get("build")->AsUInt(build) || build > 10000000 ||
        !cats || !cats->Is(JVal::Arr) || !files || !files->Is(JVal::Arr) || files->a.size() > kAcbMaxEntries || cats->a.size() > 256) return bad(kDamaged);
    m.osBuild = static_cast<int>(build);
    for (const auto& c : cats->a) { AcbCategory x; if (!str(&c, "id", x.id) || !str(&c, "title", x.title) || !str(&c, "restore", x.restore)) return bad(kDamaged); m.categories.push_back(std::move(x)); }
    for (const auto& f : files->a) {
        AcbFile x;
        if (!str(&f, "path", x.path) || !str(&f, "category", x.category) || !str(&f, "sha256", x.sha256) || !str(&f, "dest", x.dest) || !f.Get("size") || !f.Get("size")->AsUInt(x.size)) return bad(kDamaged);
        m.files.push_back(std::move(x));
    }
    if (const JVal* b = r.Get("bundle")) { if (!b->Is(JVal::Str)) return bad(kDamaged); m.bundle = b->s; }
    if (const JVal* nt = r.Get("notes")) { if (!nt->Is(JVal::Arr)) return bad(kDamaged); for (const auto& x : nt->a) { if (!x.Is(JVal::Str)) return bad(kDamaged); m.notes.push_back(x.s); } }
    return true;
}
// Everything a manifest must satisfy before its files are even looked at (design §3).
inline bool AcbValidateManifest(const AcbManifest& m, std::wstring& err) {
    using acbdetail::Show;
    const auto bad = [&](const std::wstring& t) { err = t; return false; };
    if (m.formatVersion < 1) return bad(L"The backup's manifest is damaged or incomplete.");
    if (m.formatVersion > kAcbFormatVersion) return bad(L"This backup was made by a newer AnkhCore. Update AnkhCore to open it.");
    if (m.kind != "full" && m.kind != "settings" && m.kind != "custom") return bad(L"The backup names an unknown backup type.");
    if (!AcbStampOk(m.created)) return bad(L"The backup's creation time is not valid.");
    std::wstring wv; if (!acbdetail::Wide(m.appVersion, wv) || !ParseVer(wv).valid) return bad(L"The backup does not say which AnkhCore version made it.");
    if (m.files.empty()) return bad(L"The backup contains no files.");
    if (m.files.size() > kAcbMaxEntries) return bad(L"The backup lists too many files.");
    std::set<std::string> declared;
    for (const auto& c : m.categories) {
        if (c.id.empty() || c.id.size() > 32) return bad(L"The backup names an invalid category.");
        for (const char ch : c.id) if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return bad(L"The backup names an invalid category.");
        if (!declared.insert(c.id).second) return bad(L"The backup lists the category \"" + Show(c.id) + L"\" twice.");
        if (AcbSupported(c.id) && c.restore != AcbFindCat(c.id)->restore) return bad(L"The backup asks for a restore action the category \"" + Show(c.id) + L"\" does not allow.");
    }
    std::set<std::wstring> paths, dests; uint64_t total = 0; bool bundleListed = m.bundle.empty();
    for (const auto& f : m.files) {
        std::wstring wp;
        if (!declared.count(f.category)) return bad(L"\"" + Show(f.path) + L"\" belongs to a category the backup does not declare.");
        const std::string prefix = "files/" + f.category + "/";
        if (f.path.size() <= prefix.size() || f.path.compare(0, prefix.size(), prefix) != 0 || !AcbSafePath(f.path) || !acbdetail::Wide(f.path, wp))
            return bad(L"The backup lists a file with an unsafe path: \"" + Show(f.path) + L"\".");
        if (!paths.insert(acbdetail::Fold(wp)).second) return bad(L"The backup lists \"" + Show(f.path) + L"\" twice.");
        if (f.sha256.size() != 64) return bad(L"\"" + Show(f.path) + L"\" has no valid SHA-256.");
        for (const char ch : f.sha256) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return bad(L"\"" + Show(f.path) + L"\" has no valid SHA-256.");
        if (f.size > kAcbMaxBytes || (total += f.size) > kAcbMaxBytes) return bad(L"The backup is larger than this format allows.");
        std::string root, rel;
        if (!AcbSplitDest(f.dest, root, rel)) return bad(L"\"" + Show(f.path) + L"\" has an unsafe destination.");
        if (root != "none") { std::wstring wd; acbdetail::Wide(f.dest, wd); if (!dests.insert(acbdetail::Fold(wd)).second) return bad(L"Two files of the backup share the destination \"" + Show(f.dest) + L"\"."); }
        if (const AcbCatSpec* spec = AcbFindCat(f.category); spec && *spec->exts) {
            if (!AcbExtAllowed(*spec, AcbExt(f.path)) || (root != "none" && !AcbExtAllowed(*spec, AcbExt(rel))))
                return bad(L"\"" + Show(f.path) + L"\" is not a file type the category \"" + Show(f.category) + L"\" may hold.");
            if (root != "none" && root != spec->root) return bad(L"\"" + Show(f.path) + L"\" names a destination its category may not write to.");
            if (f.category == "appdata") {
                std::wstring safetyWhy;
                if (!AcbIsSafeBackupDataPath(f.path, &safetyWhy) || (root != "none" && !AcbIsSafeBackupDataPath(rel, &safetyWhy)))
                    return bad(L"\"" + Show(f.path) + L"\" is excluded from application data backup/restore: " + safetyWhy);
            }
        }
        if (f.path == m.bundle) { if (f.category != "apps") return bad(L"The backup's package bundle is not in the applications category."); bundleListed = true; }
    }
    if (!bundleListed) return bad(L"The backup names a package bundle it does not contain.");
    return true;
}
// Categories present in the backup that this version cannot restore (shown in the preview, never written).
inline std::vector<std::string> AcbUnsupported(const AcbManifest& m) {
    std::vector<std::string> v;
    for (const auto& c : m.categories) if (!AcbSupported(c.id)) v.push_back(c.id);
    return v;
}
// The files a restore would handle: supported categories only, narrowed by Settings Only and by a
// category selection (empty = all).
inline std::vector<AcbFile> AcbSelect(const AcbManifest& m, const std::vector<std::string>& categories, bool settingsOnly) {
    std::vector<AcbFile> v;
    for (const auto& f : m.files) {
        const AcbCatSpec* spec = AcbFindCat(f.category);
        if (!spec || !*spec->exts || (settingsOnly && !spec->settingsOnly)) continue;
        if (!categories.empty() && std::find(categories.begin(), categories.end(), f.category) == categories.end()) continue;
        v.push_back(f);
    }
    return v;
}

// ---------------------------------------------------------------- whole-archive verification
struct AcbCheck { bool ok = false; std::wstring error; AcbManifest manifest; std::vector<std::string> unsupported; std::vector<ZipEntry> entries; };
// Lightweight metadata & manifest validation for listing .acbackup archives without decompressing
// or SHA-256 hashing every payload entry. Full AcbVerify() is still required before any restore.
inline AcbCheck AcbInspectMetadata(const uint8_t* p, size_t n) {
    AcbCheck r;
    if (!ZipList(p, n, r.entries, r.error)) return r;
    const ZipEntry* man = nullptr;
    for (const auto& e : r.entries) if (e.name == "manifest.json") man = &e;
    if (!man) { r.error = L"This file is not an AnkhCore backup (it has no manifest)."; return r; }
    if (man->usize > kAcbMaxManifest) { r.error = L"The backup's manifest is damaged or incomplete."; return r; }
    std::vector<uint8_t> buf;
    if (!ZipRead(p, n, *man, buf, r.error)) return r;
    if (!AcbParseManifest(std::string(buf.begin(), buf.end()), r.manifest, r.error) || !AcbValidateManifest(r.manifest, r.error)) return r;
    std::set<std::string> listed;
    for (const auto& f : r.manifest.files) listed.insert(f.path);
    for (const auto& e : r.entries)
        if (e.name != "manifest.json" && e.name != "README.txt" && !listed.count(e.name)) { r.error = L"The backup holds a file its manifest does not list: \"" + acbdetail::Show(e.name) + L"\"."; return r; }
    for (const auto& f : r.manifest.files) {
        const ZipEntry* e = nullptr;
        for (const auto& x : r.entries) if (x.name == f.path) { e = &x; break; }
        if (!e) { r.error = L"The backup is missing \"" + acbdetail::Show(f.path) + L"\"."; return r; }
        if (e->usize != f.size) { r.error = L"\"" + acbdetail::Show(f.path) + L"\" does not have the size its manifest records."; return r; }
    }
    r.unsupported = AcbUnsupported(r.manifest);
    r.ok = true;
    return r;
}

// Validates a complete .acbackup: structure, manifest, the exact set of entries, and the size and
// SHA-256 of every file. A restore may start only when ok is true.
inline AcbCheck AcbVerify(const uint8_t* p, size_t n) {
    AcbCheck r;
    if (!ZipList(p, n, r.entries, r.error)) return r;
    const ZipEntry* man = nullptr;
    for (const auto& e : r.entries) if (e.name == "manifest.json") man = &e;
    if (!man) { r.error = L"This file is not an AnkhCore backup (it has no manifest)."; return r; }
    if (man->usize > kAcbMaxManifest) { r.error = L"The backup's manifest is damaged or incomplete."; return r; }
    std::vector<uint8_t> buf;
    if (!ZipRead(p, n, *man, buf, r.error)) return r;
    if (!AcbParseManifest(std::string(buf.begin(), buf.end()), r.manifest, r.error) || !AcbValidateManifest(r.manifest, r.error)) return r;
    std::set<std::string> listed;
    for (const auto& f : r.manifest.files) listed.insert(f.path);
    for (const auto& e : r.entries)
        if (e.name != "manifest.json" && e.name != "README.txt" && !listed.count(e.name)) { r.error = L"The backup holds a file its manifest does not list: \"" + acbdetail::Show(e.name) + L"\"."; return r; }
    for (const auto& f : r.manifest.files) {
        const ZipEntry* e = nullptr;
        for (const auto& x : r.entries) if (x.name == f.path) { e = &x; break; }
        if (!e) { r.error = L"The backup is missing \"" + acbdetail::Show(f.path) + L"\"."; return r; }
        if (e->usize != f.size) { r.error = L"\"" + acbdetail::Show(f.path) + L"\" does not have the size its manifest records."; return r; }
        if (!ZipRead(p, n, *e, buf, r.error)) return r;
        if (Sha256Hex(buf.data(), buf.size()) != f.sha256) { r.error = L"\"" + acbdetail::Show(f.path) + L"\" does not match its SHA-256 in the manifest."; return r; }
    }
    r.unsupported = AcbUnsupported(r.manifest);
    r.ok = true;
    return r;
}
// One file's content out of a verified backup, hash-checked again at the point of use.
inline bool AcbExtract(const uint8_t* p, size_t n, const AcbCheck& c, const AcbFile& f, std::vector<uint8_t>& out, std::wstring& err) {
    out.clear();
    if (!c.ok) { err = L"The backup has not passed validation."; return false; }
    for (const auto& e : c.entries)
        if (e.name == f.path) {
            if (!ZipRead(p, n, e, out, err)) return false;
            if (out.size() != f.size || Sha256Hex(out.data(), out.size()) != f.sha256) { out.clear(); err = L"\"" + acbdetail::Show(f.path) + L"\" does not match its SHA-256 in the manifest."; return false; }
            return true;
        }
    err = L"The backup is missing \"" + acbdetail::Show(f.path) + L"\".";
    return false;
}

// ---------------------------------------------------------------- building
struct AcbItem { std::string category, rel, dest; std::vector<uint8_t> data; };      // stored as files/<category>/<rel>
struct AcbBuildInput {
    std::string kind = "full", created, appVersion, appLabel, osName, arch; int osBuild = 0;
    std::vector<AcbItem> items; std::vector<std::string> notes;
};
inline const char* AcbReadme() {
    return "AnkhCore backup (.acbackup)\r\n\r\n"
           "This file is a backup made by AnkhCore. To restore it, open AnkhCore, go to Recovery > Backup & Migration,\r\n"
           "choose Restore and select this file. AnkhCore checks every file against the manifest before it changes anything,\r\n"
           "shows what the backup contains, and lets you choose what to restore.\r\n\r\n"
           "The file is a ZIP archive, so its contents can be inspected with any ZIP tool. Do not edit it: a changed\r\n"
           "backup no longer passes validation. It holds settings and user content only. It does not hold Windows system\r\n"
           "files, programs, drivers or passwords. A list of applications in it names packages to reinstall; it is not\r\n"
           "an offline copy of their installers.\r\n";
}
// Builds a complete backup from in-memory items. Only supported categories and their allowed file
// types can be written; the result is validated with AcbVerify before it is returned.
inline bool AcbBuild(const AcbBuildInput& in, std::vector<uint8_t>& out, std::wstring& err) {
    out.clear();
    AcbManifest m; m.kind = in.kind; m.created = in.created; m.appVersion = in.appVersion; m.appLabel = in.appLabel; m.osName = in.osName; m.arch = in.arch; m.osBuild = in.osBuild; m.notes = in.notes;
    if (!AcbStampOk(in.created)) { err = L"The backup's creation time is not valid."; return false; }
    std::vector<ZipItem> zip(2);
    zip[0].name = "manifest.json"; zip[1].name = "README.txt";
    const std::string readme = AcbReadme(); zip[1].data.assign(readme.begin(), readme.end());
    for (const auto& it : in.items) {
        if (!AcbSupported(it.category)) { err = L"The category \"" + acbdetail::Show(it.category) + L"\" cannot be backed up by this version."; return false; }
        AcbFile f; f.path = "files/" + it.category + "/" + it.rel; f.category = it.category; f.dest = it.dest; f.size = it.data.size();
        f.sha256 = Sha256Hex(it.data.data(), it.data.size());
        if (f.sha256.size() != 64) { err = L"SHA-256 is not available on this system."; return false; }
        if (it.category == "apps" && AcbExt(it.rel) == ".ubundle" && m.bundle.empty()) m.bundle = f.path;
        m.files.push_back(f);
        ZipItem z; z.name = f.path; z.data = it.data; zip.push_back(std::move(z));
    }
    for (const auto& spec : AcbCategories())
        for (const auto& f : m.files) if (f.category == spec.id) { m.categories.push_back({spec.id, spec.title, spec.restore}); break; }
    if (!AcbValidateManifest(m, err)) return false;
    const std::string json = AcbManifestJson(m); zip[0].data.assign(json.begin(), json.end());
    const auto num = [&](int at, int len) { int v = 0; for (int i = 0; i < len; ++i) v = v * 10 + (in.created[at + i] - '0'); return v; };
    const uint16_t dosDate = static_cast<uint16_t>(((num(0, 4) - 1980) << 9) | (num(5, 2) << 5) | num(8, 2));
    const uint16_t dosTime = static_cast<uint16_t>((num(11, 2) << 11) | (num(14, 2) << 5) | (num(17, 2) / 2));
    if (!ZipWrite(zip, dosTime, dosDate, out)) { err = L"The backup is larger than this format allows."; return false; }
    const AcbCheck chk = AcbVerify(out.data(), out.size());
    if (!chk.ok) { err = chk.error; out.clear(); return false; }
    return true;
}

}  // namespace ak
