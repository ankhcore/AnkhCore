// AnkhCore — icon packs.
// 2.5 (D41 / APP-8): Runtime icon theming via Windhawk Resource Redirect 1.3
// (`icon-resource-redirect`, `themePaths`). Prebuilt resource-only PE modules
// + `theme.ini` live in `<AppDir>\IconThemes\<id>\` (with fallback to
// `<DataDir>\IconThemes\<id>\` and `<RepoRoot>\release-inputs\icon-themes\IconThemes\<id>\`
// in dev builds). Windows system files stay untouched.
// Legacy 2.2 7tsp/backup restore helpers remain available for users who patched
// system files on 2.2.
#pragma once
#include "base.h"
#include "inflate.h"
#include "pe_rsrc.h"
#include "json_min.h"
#include "settings.h"
#include "op_progress.h"
#include "icon_refresh.h"
#include <tlhelp32.h>
#include <atomic>
#include <mutex>

namespace ak {

struct IconPack {
    const wchar_t* id; const wchar_t* group; const wchar_t* name; const wchar_t* variant;
    const wchar_t* author; const wchar_t* desc; const wchar_t* preview;   // preview = resource name
    bool ipack;
    bool partial = false;
    const wchar_t* note = L"";
};
inline const std::vector<IconPack>& IconPacks() {
    static const std::vector<IconPack> p = {
        {L"bigsur-dark", L"bigsur", L"Big Sur 2", L"Dark", L"niivu", L"macOS Big Sur style folders and drives.", L"PV_BIGSUR_DARK", false, false, L""},
        {L"bigsur-light", L"bigsur", L"Big Sur 2", L"Light", L"niivu", L"macOS Big Sur style folders and drives.", L"PV_BIGSUR_LIGHT", false, false, L""},
        {L"bigsur", L"bigsur", L"Big Sur 2", L"Regular", L"niivu", L"macOS Big Sur style folders and drives.", L"PV_BIGSUR", false, false, L""},
        {L"sweet", L"sweet", L"Sweet Paranoia", L"", L"niivu", L"Neon candy colours on dark: purple, pink and orange.", L"PV_SWEET", false, false, L""},
        {L"candy", L"candy", L"Candy", L"", L"niivu", L"Vibrant candy-coloured folders and system icons.", L"", false, false, L"Identical icon set to Sweet Paranoia."},
        {L"tech", L"tech", L"Tech Icons", L"", L"Dark Knight", L"Dark tech hardware with blue accents. Made for Windows 10 22H2 and newer.", L"PV_TECH", false, false, L""},
        {L"pear-blue-dark", L"pear-blue-dark", L"Pear Blue Dark", L"", L"niivu", L"Sleek dark blue folders and system icons.", L"", false, false, L""},
        {L"alienware-invader", L"alienware-invader", L"Alienware Invader", L"", L"Alienware / 7TSP", L"Sci-fi dark silver and cyan folders and system icons.", L"", false, false, L""},
        {L"magnum", L"magnum", L"Magnum Dark", L"", L"Razorsedge & Moonnique", L"Glossy black with blue reflections; redirects icons & bitmaps across ~99 system modules.", L"PV_MAGNUM", false, true, L"Icons & bitmaps redirect via Resource Redirect; UIFILE/PNG extras require All resource redirect."},
        {L"hud", L"hud", L"HUD Machine Blue", L"", L"Mr GRiM", L"Black and electric-blue HUD icons across 36 system modules.", L"PV_HUD", false, true, L"Icons & bitmaps redirect via Resource Redirect; UIFILE/PNG extras require All resource redirect."},
        {L"alienware-eclipse", L"alienware-eclipse", L"Alienware Eclipse", L"", L"Alienware / 7TSP", L"Dark eclipse folders and system icons across 34 modules.", L"", false, true, L"Icons & bitmaps redirect via Resource Redirect; UIFILE/WAVE extras require All resource redirect."},
        // 2026-11-15: seven further 7TSP packs. Each theme folder (tools/prepare_icon_themes.py) carries the pack's own preview picture.
        {L"3d-purple", L"3d-purple", L"3D Purple", L"", L"WinX Customization", L"Purple 3D folders and system icons.", L"", false, false, L""},
        {L"amy-dark", L"amy-dark", L"Amy Dark", L"", L"Digifeye", L"Dark folders and system icons.", L"", false, false, L""},
        {L"coloration", L"coloration", L"Coloration", L"", L"WinX Customization", L"Colourful folders and system icons.", L"", false, false, L""},
        {L"hal-green", L"hal-green", L"Hal Green System", L"", L"7TSP", L"Green system folders and icons.", L"", false, false, L""},
        {L"macos-elegance", L"macos-elegance", L"macOS Elegance", L"", L"WinX Customization", L"macOS-style elegant folders and system icons.", L"", false, false, L""},
        {L"tela-black", L"tela-black", L"Tela Black", L"", L"Digifeye", L"Tela black folders and system icons.", L"", false, false, L""},
        {L"windows-23", L"windows-23", L"Windows 23", L"", L"Link Vegas", L"Windows 23 style folders and system icons.", L"", false, false, L""},
    };
    return p;
}
inline const IconPack* FindPack(const std::wstring& id) {
    if (id == L"hud-machine-blue") return FindPack(L"hud");
    for (const auto& p : IconPacks()) if (id == p.id) return &p;
    return nullptr;
}
inline std::wstring PackDir(const std::wstring& id) { return AppDir() + L"\\IconPacks\\" + id; }
inline std::wstring IconBackupDir() { return DataDir() + L"\\IconBackup"; }
inline const wchar_t* IconKey() { return L"SOFTWARE\\AnkhCore\\Icons"; }

// ---------------------------------------------------------------- Resource Redirect (D41 / APP-8)
inline std::wstring IconThemesRoot() {
    const std::wstring appRoot = AppDir() + L"\\IconThemes";
    if (DirExists(appRoot)) return appRoot;
    const std::wstring dataRoot = DataDir() + L"\\IconThemes";
    if (DirExists(dataRoot)) return dataRoot;
    return appRoot;   // installed themes only: never a development folder (master changeset §5.7)
}
inline std::wstring IconThemeFolderName(const std::wstring& id) {
    if (id == L"hud") return L"hud-machine-blue";
    return id;
}
inline std::wstring IconThemeDir(const std::wstring& id) {
    const std::wstring root = IconThemesRoot();
    const std::wstring direct = root + L"\\" + id;
    if (DirExists(direct)) return direct;
    const std::wstring mapped = root + L"\\" + IconThemeFolderName(id);
    if (DirExists(mapped)) return mapped;
    return direct;
}
// The copy the setup installed beside the program; never a development folder (master changeset §5.7).
inline std::wstring ResourceRedirectModSourcePath() { return AppDir() + L"\\Mods\\icon-resource-redirect.wh.cpp"; }

// Parses [redirections] from a Resource Redirect theme.ini file.
inline bool ParseThemeIniRedirections(const std::wstring& iniPath,
                                      std::vector<std::pair<std::wstring, std::wstring>>& outPairs,
                                      std::wstring& outErr) {
    outPairs.clear();
    outErr.clear();
    if (!FileExists(iniPath)) { outErr = L"Missing theme.ini: " + iniPath; return false; }
    std::vector<wchar_t> sec(32768, L'\0');
    const DWORD len = GetPrivateProfileSectionW(L"redirections", sec.data(), static_cast<DWORD>(sec.size()), iniPath.c_str());
    if (len == 0 || len >= sec.size() - 2) {
        outErr = L"Empty or unreadable [redirections] section in " + iniPath;
        return false;
    }
    const wchar_t* p = sec.data();
    while (*p) {
        const std::wstring line(p);
        p += line.size() + 1;
        if (line.empty() || line[0] == L';') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0 || eq + 1 >= line.size()) {
            outErr = L"Invalid redirection entry in theme.ini: " + line;
            return false;
        }
        const std::wstring targetRaw = line.substr(0, eq);
        const std::wstring redirRel  = line.substr(eq + 1);
        wchar_t expanded[MAX_PATH + 16]{};
        const DWORD expLen = ExpandEnvironmentStringsW(targetRaw.c_str(), expanded, MAX_PATH + 8);
        if (expLen == 0 || expLen > MAX_PATH) {
            outErr = L"Redirection target path exceeds MAX_PATH or failed expansion: " + targetRaw;
            return false;
        }
        if (redirRel.find(L"..") != std::wstring::npos || redirRel.find(L':') != std::wstring::npos) {
            outErr = L"Unsafe relative module path in theme.ini: " + redirRel;
            return false;
        }
        outPairs.push_back({targetRaw, redirRel});
    }
    if (outPairs.empty()) {
        outErr = L"No redirection entries found in " + iniPath;
        return false;
    }
    return true;
}

// Validates that `themeDir` contains a valid `theme.ini` and that every referenced
// target file exists and parses as a valid PE with a `.rsrc` directory (never a raw .res).
// Counts the full validations (each reads every module of the theme completely). Apply and setup need them; a page that
// paints must not (FT-2 / A04: the Icon Manager reads the cached quick check of icon_status.h instead). Tests read this.
inline std::atomic<int>& IconFullValidations() { static std::atomic<int> n{0}; return n; }
inline bool ValidateIconThemeDir(const std::wstring& themeDir,
                                 std::vector<std::pair<std::wstring, std::wstring>>* outPairs = nullptr,
                                 std::wstring* outErr = nullptr) {
    ++IconFullValidations();
    std::wstring err;
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    const std::wstring iniPath = themeDir + L"\\theme.ini";
    if (!ParseThemeIniRedirections(iniPath, pairs, err)) {
        if (outErr) *outErr = err;
        return false;
    }
    for (const auto& kv : pairs) {
        const std::wstring modPath = themeDir + L"\\" + kv.second;
        std::vector<uint8_t> bytes;
        if (!ReadAll(modPath, bytes)) {
            if (outErr) *outErr = L"Missing redirected module: " + modPath;
            return false;
        }
        pe::PE parsed;
        if (!parsed.Parse(bytes.data(), bytes.size()) || parsed.resRva == 0 || parsed.resSize == 0) {
            if (outErr) *outErr = L"File is not a valid PE resource module (.rsrc missing): " + modPath;
            return false;
        }
    }
    if (outPairs) *outPairs = std::move(pairs);
    return true;
}

inline bool IsIconThemeInstalled(const std::wstring& id) {
    return ValidateIconThemeDir(IconThemeDir(id));
}

// Pure helpers for Windhawk Resource Redirect `themePaths[i]` settings management.
// Resource Redirect 1.3 reads `themePaths[0]`, `themePaths[1]`, ... and stops at the first empty string.
inline std::vector<std::wstring> ExtractThemePathsFromFlatSettings(const JVal& settingsObj) {
    std::vector<std::wstring> out;
    if (!settingsObj.Is(JVal::Obj)) return out;
    for (int i = 0; i < 64; ++i) {
        const std::string key = "themePaths[" + std::to_string(i) + "]";
        const JVal* v = settingsObj.Get(key);
        if (!v || !v->Is(JVal::Str) || v->s.empty()) break;
        out.push_back(Utf8ToW(v->s));
    }
    return out;
}

inline std::wstring NormPathKey(std::wstring p) {
    for (auto& ch : p) if (ch == L'/') ch = L'\\';
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return Lower(p);
}

inline bool PathIsUnderRoot(const std::wstring& path, const std::wstring& root) {
    if (root.empty()) return false;
    const std::wstring np = NormPathKey(path);
    const std::wstring nr = NormPathKey(root);
    if (np == nr) return true;
    return np.size() > nr.size() && np.compare(0, nr.size(), nr) == 0 && np[nr.size()] == L'\\';
}

// Computes the updated `themePaths` array:
// - Preserves any non-empty user paths outside `managedRoot` / `prevManagedPath`.
// - Removes any previous AnkhCore-managed theme paths and duplicates.
// - Appends `newThemeDir` if non-empty (or leaves it removed when restoring default icons).
inline std::vector<std::wstring> PlanThemePathsUpdate(const std::vector<std::wstring>& currentPaths,
                                                      const std::wstring& managedRoot,
                                                      const std::wstring& prevManagedPath,
                                                      const std::wstring& newThemeDir) {
    std::vector<std::wstring> out;
    const std::wstring prevKey = NormPathKey(prevManagedPath);
    const std::wstring newKey  = NormPathKey(newThemeDir);
    std::set<std::wstring> seen;
    for (const auto& p : currentPaths) {
        if (p.empty()) continue;
        const std::wstring pk = NormPathKey(p);
        if (PathIsUnderRoot(p, managedRoot)) continue;
        if (!prevKey.empty() && pk == prevKey) continue;
        if (!newKey.empty() && pk == newKey) continue;
        if (seen.insert(pk).second) out.push_back(p);
    }
    if (!newThemeDir.empty() && seen.insert(newKey).second) {
        out.push_back(newThemeDir);
    }
    return out;
}

// Formats the key=value pairs for `windhawk-cli mod settings set <modId> ...`,
// clearing any trailing slots up to `max(oldSlotCount, newPaths.size()) + 1` so
// Resource Redirect's sequential reader stops cleanly at the new end.
inline std::vector<std::wstring> FormatThemePathsSetArgs(size_t oldSlotCount,
                                                         const std::vector<std::wstring>& newPaths) {
    std::vector<std::wstring> kv;
    for (size_t i = 0; i < newPaths.size(); ++i) {
        kv.push_back(L"themePaths[" + std::to_wstring(i) + L"]=" + newPaths[i]);
    }
    const size_t clearUntil = std::max(oldSlotCount, newPaths.size() + 1);
    for (size_t i = newPaths.size(); i < clearUntil; ++i) {
        kv.push_back(L"themePaths[" + std::to_wstring(i) + L"]=");
    }
    return kv;
}

// Boot identity (minutes since 1601 at boot) to tell "pending restart".
inline uint64_t BootStamp() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    const uint64_t now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 600000000ull;
    return now - GetTickCount64() / 60000ull;
}
struct IconState { std::wstring active; bool pending = false; std::vector<std::wstring> patched; };
inline IconState GetIconState() {
    IconState s;
    s.active = Cfg().Str(L"icons.activeTheme", L"");
    if (s.active == L"hud-machine-blue") s.active = L"hud";
    if (s.active.empty()) s.active = RegStr(HKEY_LOCAL_MACHINE, IconKey(), L"Active");
    const RegValue b = RegGet(HKEY_LOCAL_MACHINE, IconKey(), L"BootStamp");
    if (b.exists && b.data.size() >= 8) { uint64_t v; memcpy(&v, b.data.data(), 8); const uint64_t now = BootStamp(); s.pending = (now > v ? now - v : v - now) <= 2; }
    for (const auto& p : Split(RegStr(HKEY_LOCAL_MACHINE, IconKey(), L"Patched"), L'|')) if (!p.empty()) s.patched.push_back(p);
    return s;
}
// `needsRestart`: the 2.2 file swap changed protected files, so only a Windows restart completes it (BootStamp marks that).
// Resource Redirect (FT-2 / A01) does not: it leaves an earlier stamp as it is.
inline void SaveIconState(const std::wstring& active, const std::vector<std::wstring>& patched, bool needsRestart = true) {
    Cfg().SetStr(L"icons.activeTheme", active);
    Cfg().Save();
    RegSetStr(HKEY_LOCAL_MACHINE, IconKey(), L"Active", active);
    std::wstring all; for (const auto& p : patched) all += p + L"|";
    RegSetStr(HKEY_LOCAL_MACHINE, IconKey(), L"Patched", all);
    if (!needsRestart) return;
    const uint64_t b = BootStamp();
    RegSet(HKEY_LOCAL_MACHINE, IconKey(), L"BootStamp", REG_QWORD, &b, 8);
}

// ---------------------------------------------------------------- .res parsing
struct ResItem { std::wstring type, name; WORD typeId = 0, nameId = 0; WORD lang = 0; const uint8_t* data = nullptr; DWORD size = 0; };
inline bool ParseRes(const std::vector<uint8_t>& d, std::vector<ResItem>& out) {
    size_t o = 0;
    auto rd16 = [&](size_t q) { uint16_t v; memcpy(&v, d.data() + q, 2); return v; };
    while (o + 32 <= d.size()) {
        DWORD dsz, hsz; memcpy(&dsz, d.data() + o, 4); memcpy(&hsz, d.data() + o + 4, 4);
        if (hsz < 32 || o + hsz + dsz > d.size()) return !out.empty();
        size_t q = o + 8;
        ResItem it;
        auto readId = [&](std::wstring& s, WORD& id) {
            if (rd16(q) == 0xFFFF) { id = rd16(q + 2); q += 4; return; }
            while (q + 1 < d.size() && rd16(q)) { s.push_back(static_cast<wchar_t>(rd16(q))); q += 2; }
            q += 2;
        };
        readId(it.type, it.typeId); readId(it.name, it.nameId);
        q = (q + 3) & ~static_cast<size_t>(3);
        it.lang = rd16(q + 6);
        it.data = d.data() + o + hsz; it.size = dsz;
        if (it.typeId || !it.type.empty()) if (!(it.typeId == 0 && it.type.empty())) if (dsz) out.push_back(it);
        o = (o + hsz + dsz + 3) & ~static_cast<size_t>(3);
    }
    return true;
}
inline LPCWSTR ResTypePtr(const ResItem& r) { return r.type.empty() ? MAKEINTRESOURCEW(r.typeId) : r.type.c_str(); }
inline LPCWSTR ResNamePtr(const ResItem& r) { return r.name.empty() ? MAKEINTRESOURCEW(r.nameId) : r.name.c_str(); }

// "imageres.dll.mun.res" -> the system files it is meant for.
inline std::vector<std::wstring> ResTargets(const std::wstring& resFile) {
    std::wstring n = NameOf(resFile);
    if (EndsWithI(n, L".dz")) n.resize(n.size() - 3);
    if (EndsWithI(n, L".res")) n.resize(n.size() - 4);
    std::vector<std::wstring> t;
    const std::wstring win = WinDir(), sys = SysDir(), wow = win + L"\\SysWOW64";
    if (EndsWithI(n, L".mun")) {
        const std::wstring base = n.substr(0, n.size() - 4);
        if (FileExists(win + L"\\SystemResources\\" + n)) t.push_back(win + L"\\SystemResources\\" + n);
        else if (FileExists(sys + L"\\" + base)) t.push_back(sys + L"\\" + base);
    } else if (EndsWithI(n, L".mui")) {
        for (const auto& root : {win, sys})
            for (const auto& lang : ListDir(root, true))
                if (lang.size() >= 4 && lang.size() <= 10 && lang.find(L'-') != std::wstring::npos && FileExists(root + L"\\" + lang + L"\\" + n))
                    t.push_back(root + L"\\" + lang + L"\\" + n);
    } else if (FileExists(win + L"\\SystemResources\\" + n + L".mun")) {
        // Windows 10 1903+ loads this file's icons from its .mun: patch that,
        // never the code file itself.
        t.push_back(win + L"\\SystemResources\\" + n + L".mun");
    } else {
        for (const auto& root : {sys, wow, win})
            if (FileExists(root + L"\\" + n)) t.push_back(root + L"\\" + n);
    }
    return t;
}

// ---------------------------------------------------------------- backups
inline std::wstring BackupPathFor(const std::wstring& target) {
    std::wstring m = Lower(target);
    for (auto& c : m) if (c == L'\\' || c == L':' || c == L'/') c = L'_';
    return IconBackupDir() + L"\\" + m + L".orig";
}
inline void PrepareBackupDir() {
    SecureDataDir();
    EnsureDir(IconBackupDir());
    HANDLE h = CreateFileW(IconBackupDir().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {   // NTFS compression: backups take far less space
        USHORT fmt = COMPRESSION_FORMAT_DEFAULT; DWORD r = 0;
        DeviceIoControl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof(fmt), nullptr, 0, &r, nullptr);
        CloseHandle(h);
    }
}
inline bool EnsureBackup(const std::wstring& target) {
    const std::wstring b = BackupPathFor(target);
    if (FileExists(b)) return true;
    std::vector<uint8_t> data;
    if (!ReadAll(target, data)) { LogE(L"Cannot read " + target); return false; }
    if (!WriteAll(b, data.data(), data.size())) { LogE(L"Cannot back up " + target); return false; }
    RegSetStr(HKEY_LOCAL_MACHINE, std::wstring(IconKey()) + L"\\Backups", target, b);
    return true;
}
inline std::vector<std::wstring> BackedUpTargets() {
    std::vector<std::wstring> v;
    for (const auto& kv : RegStrings(HKEY_LOCAL_MACHINE, std::wstring(IconKey()) + L"\\Backups")) v.push_back(kv.first);
    return v;
}

// ---------------------------------------------------------------- swapping a protected system file
// System icon files (imageres.dll.mun ...) are owned by TrustedInstaller and
// held mapped in memory, so they can't be overwritten in place. Take
// ownership, grant Administrators full control, then replace — immediately if
// the file happens to be free, otherwise schedule the replace for the next
// boot (PendingFileRenameOperations), which is how 7tsp and iPack patch them
// reliably. A restart is needed afterwards either way.
inline bool TakeOwnAndGrant(const std::wstring& path) {
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME); EnablePrivilege(SE_RESTORE_NAME); EnablePrivilege(SE_BACKUP_NAME); EnablePrivilege(SE_SECURITY_NAME);
    BYTE sidBuf[SECURITY_MAX_SID_SIZE]; DWORD cb = sizeof(sidBuf);
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sidBuf, &cb)) return false;
    PSID admins = reinterpret_cast<PSID>(sidBuf);
    SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, admins, nullptr, nullptr, nullptr);
    PACL oldDacl = nullptr, newDacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr, &sd);
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_ALL; ea.grfAccessMode = GRANT_ACCESS; ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID; ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP; ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(admins);
    if (SetEntriesInAclW(1, &ea, oldDacl, &newDacl) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl, nullptr);
        LocalFree(newDacl);
    }
    if (sd) LocalFree(sd);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return true;
}
// Pending folder (writable, under ProgramData) holds the replacement files
// for the boot-time fallback. The main path overwrites the system file IN
// PLACE after taking ownership — the same thing 7tsp does, and it needs no
// write access on the protected folder (which Administrators don't have — the
// real cause of the earlier error 5), only on the file itself.
inline std::wstring PendingDir() { return DataDir() + L"\\Pending"; }
inline bool OverwriteInPlace(const std::wstring& target, const std::vector<uint8_t>& data) {
    // A few tries: an icon file is opened briefly by whatever reads its icons,
    // so a write handle is usually free a moment after the shell is stopped.
    for (int attempt = 0; attempt < 6; ++attempt) {
        HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Sleep(120); continue; }
        bool ok = true; size_t done = 0;
        while (done < data.size() && ok) {
            DWORD w = 0; const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
            ok = WriteFile(h, data.data() + done, chunk, &w, nullptr) && w == chunk; done += chunk;
        }
        if (ok) ok = SetEndOfFile(h) != 0;
        CloseHandle(h);
        if (ok) return true;
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    return false;
}
inline bool SwapSystemFile(const std::wstring& target, const std::wstring& src) {
    if (kTestBuild && !InTestArea(target)) { TestRefused(L"replace system file " + target); return false; }   // the isolated test build never touches a real system file or queues a boot-time rename
    std::vector<uint8_t> data;
    if (!ReadAll(src, data)) { LogE(L"Cannot read the prepared file for " + NameOf(target)); return false; }
    TakeOwnAndGrant(target);
    // 1) write directly over the file (works once the shell is stopped)
    if (OverwriteInPlace(target, data)) { LogI(L"Replaced now: " + NameOf(target)); return true; }
    // 2) still locked: let the SYSTEM boot-time rename do it (needs no folder
    //    permission now). Reliable last resort for a file held by many processes.
    EnsureDir(PendingDir());
    const std::wstring pend = PendingDir() + L"\\" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(rand()) + L"_" + NameOf(target);
    if (CopyFileW(src.c_str(), pend.c_str(), FALSE) &&
        MoveFileExW(pend.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) { LogI(L"Queued for restart: " + NameOf(target)); return true; }
    LogE(L"Cannot replace " + target + L" (error " + std::to_wstring(GetLastError()) + L").");
    DeleteFileW(pend.c_str());
    return false;
}
inline bool SameFile(const std::wstring& a, const std::wstring& b) {
    std::vector<uint8_t> x, y;
    return ReadAll(a, x) && ReadAll(b, y) && x == y;
}

// Writes the pack's icon resources into a copy of the original file, using
// Windows' own resource update (which always produces a valid PE). Each
// resource keeps the language the pack authored it for, plus the neutral and
// US-English languages as a fallback, so Explorer finds it whatever the
// system language. No LoadLibraryEx step: mapping a huge .mun (imageres is
// ~90 MB) just to list languages was slow/unreliable, and it is not needed.
// `written` returns how many resource writes succeeded.
inline bool BuildPatched(const std::wstring& original, const std::vector<ResItem>& items, const std::wstring& out, int& written) {
    written = 0;
    if (!CopyFileW(original.c_str(), out.c_str(), FALSE)) { LogE(L"  copy failed (" + std::to_wstring(GetLastError()) + L"): " + NameOf(original)); return false; }
    SetFileAttributesW(out.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE u = BeginUpdateResourceW(out.c_str(), FALSE);
    if (!u) { LogE(L"  BeginUpdateResource failed (" + std::to_wstring(GetLastError()) + L"): " + NameOf(out)); return false; }
    int failW = 0;
    for (const auto& r : items) {
        std::set<WORD> langs{r.lang, 0, 1033};
        for (WORD l : langs)
            if (UpdateResourceW(u, ResTypePtr(r), ResNamePtr(r), l, const_cast<uint8_t*>(r.data), r.size)) ++written;
            else ++failW;
    }
    if (written > 0 && EndUpdateResourceW(u, FALSE)) return true;
    EndUpdateResourceW(u, TRUE);   // discard; Windows refused this file (large system .mun, error 50)
    // Fall back to the built-in PE resource rebuilder, which Windows' own
    // UpdateResource limits do not affect. The result is verified before use.
    LogI(L"  Windows refused this file; rebuilding its resource section directly...");
    std::vector<uint8_t> orig;
    if (!ReadAll(original, orig)) { LogE(L"  could not re-read " + NameOf(original)); return false; }
    std::vector<pe::Res> icons;
    for (const auto& r : items) {
        if (r.typeId != 3 && r.typeId != 14) continue;
        pe::Res pr; pr.type = r.typeId; pr.nameIsId = r.name.empty(); pr.nameId = r.nameId;
        for (wchar_t ch : r.name) pr.name.push_back(static_cast<char16_t>(ch));
        pr.lang = r.lang; pr.data.assign(r.data, r.data + r.size);
        icons.push_back(pr);
    }
    int rbErr = 0;
    const std::vector<uint8_t> rebuilt = pe::ReplaceIconsEx(orig.data(), orig.size(), icons, rbErr);
    int g = 0, ic = 0;
    if (rebuilt.empty()) { LogE(L"  PE rebuild returned nothing for " + NameOf(out) + L" (code " + std::to_wstring(rbErr) + L": 1=parse 2=build 3=no-room 4=no-rsrc)"); return false; }
    if (!pe::VerifyIcons(rebuilt, g, ic)) { LogE(L"  PE rebuild did not verify for " + NameOf(out) + L" (built " + std::to_wstring(rebuilt.size()) + L" bytes)"); return false; }
    if (!WriteAll(out, rebuilt.data(), rebuilt.size())) { LogE(L"  could not write rebuilt file " + NameOf(out)); return false; }
    written = ic;
    LogI(L"  rebuilt " + NameOf(out) + L" (" + std::to_wstring(g) + L" groups, " + std::to_wstring(ic) + L" icons)");
    return true;
}
inline void ScheduleIconCacheReset() {
    const std::wstring la = UserFolder(FOLDERID_LocalAppData);
    DeleteNowOrAtReboot(la + L"\\IconCache.db");
    for (const wchar_t* sub : {L"\\Microsoft\\Windows\\Explorer", L"\\Microsoft\\Windows\\Caches"})
        for (const auto& f : ListDir(la + sub, false, L"iconcache_*.db")) DeleteNowOrAtReboot(la + sub + L"\\" + f);
    for (const auto& f : ListDir(la + L"\\Microsoft\\Windows\\Explorer", false, L"thumbcache_*.db")) DeleteNowOrAtReboot(la + L"\\Microsoft\\Windows\\Explorer\\" + f);
    // ask the shell to clear and rebuild its icon cache (runs as the signed-in user)
    const std::wstring ie4 = SysDir() + L"\\ie4uinit.exe";
    if (UserTokenRef()) { RunAsUser(ie4, L"-ClearIconCache", 15000); RunAsUser(ie4, L"-show", 15000); }
    else { Run(ie4, L"-ClearIconCache", true, 15000); Run(ie4, L"-show", true, 15000); }
    // tell the shell associations/icons changed so it repaints without a reboot
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
}

using IconProgress = std::function<void(const std::wstring&, int)>;

// Puts the original files back for the given targets (only where changed).
// failed (optional): files that differ from their original and could not be put back (SEC-3).
inline int RestoreTargets(const std::vector<std::wstring>& targets, const IconProgress& prog, int* failed = nullptr) {
    int n = 0, i = 0;
    for (const auto& t : targets) {
        ++i;
        const std::wstring b = BackupPathFor(t);
        if (!FileExists(b) || !FileExists(t)) continue;
        if (prog) prog(L"Restoring " + NameOf(t), ProgressOfCount(static_cast<size_t>(i - 1), targets.size()));   // files finished before this one
        if (SameFile(t, b)) continue;
        if (SwapSystemFile(t, b)) { ++n; LogI(L"Original restored: " + t); }
        else { if (failed) ++*failed; LogE(L"Original could not be restored: " + t); }
    }
    return n;
}
// Writes a short diagnostic report of the last icon operation to the user's
// Desktop, so it is easy to send back when something looks wrong.
inline void WriteIconReport(const std::wstring& action) {
    std::wstring r = L"AnkhCore icon report\r\n====================\r\n";
    r += action + L"\r\nTime: " + NowText(true) + L"\r\nWindows: " + WinVerText(GetWinVer()) + L"\r\n\r\n";
    const IconState st = GetIconState();
    r += L"Active pack: " + (st.active.empty() ? L"(Windows default)" : st.active) + (st.pending ? L"  [waiting for restart]" : L"") + L"\r\n";
    r += L"Patched files recorded: " + std::to_wstring(st.patched.size()) + L"\r\n";
    r += L"Backups kept: " + std::to_wstring(BackedUpTargets().size()) + L"\r\n\r\n";
    r += L"Pending file replacements at next boot:\r\n";
    int pend = 0;
    const RegValue pfr = RegGet(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", L"PendingFileRenameOperations");
    if (pfr.exists && pfr.data.size() > 2) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(pfr.data.data());
        const size_t n = pfr.data.size() / sizeof(wchar_t);
        for (size_t i = 0; i < n;) {
            std::wstring s(p + i); i += s.size() + 1;
            if (s.find(L"ankh") != std::wstring::npos || s.find(L".mun") != std::wstring::npos || s.find(L"imageres") != std::wstring::npos) { r += L"  " + s + L"\r\n"; ++pend; }
        }
    }
    if (!pend) r += L"  (none)\r\n";
    r += L"\r\nRecent log:\r\n";
    const auto lines = LogSnapshot();
    const size_t start = lines.size() > 220 ? lines.size() - 220 : 0;
    static const wchar_t* tag[] = {L"INFO ", L"OK   ", L"WARN ", L"ERROR"};
    for (size_t i = start; i < lines.size(); ++i) r += L"[" + lines[i].time + L"] " + tag[static_cast<int>(lines[i].level)] + L" " + lines[i].text + L"\r\n";
    const std::wstring path = UserFolder(FOLDERID_Desktop) + L"\\AnkhCore-icons-report.txt";
    const std::string u = WToUtf8(r);
    std::vector<uint8_t> b{0xEF, 0xBB, 0xBF}; b.insert(b.end(), u.begin(), u.end());
    WriteAll(path, b.data(), b.size());
    LogI(L"Icon report written to the Desktop: AnkhCore-icons-report.txt");
}
inline bool RestoreOriginalIcons(const IconProgress& prog) {
    const IconState st = GetIconState();
    // Every file AnkhCore ever backed up is checked; RestoreTargets only swaps the
    // ones that differ from their original. (2.2 used the "Patched" list, which a
    // later 7tsp apply cleared, so Restore silently restored nothing.)
    std::vector<std::wstring> targets = BackedUpTargets();
    for (const auto& p : st.patched) if (std::find(targets.begin(), targets.end(), p) == targets.end()) targets.push_back(p);
    if (prog) prog(L"Stopping Explorer...", kProgressUnknown);
    KillProcess(L"explorer.exe");
    int failed = 0;
    const int n = RestoreTargets(targets, prog, &failed);
    ScheduleIconCacheReset();
    StartExplorer();
    // SEC-3: success only when no changed file was left behind. After a failure the recorded state is kept, so Restore can be run again.
    if (failed == 0) {
        SaveIconState(L"", {});
        LogOk(n ? L"Original Windows icons restored (" + std::to_wstring(n) + L" file(s)). A restart makes every icon refresh."
                : std::wstring(L"Nothing to restore: every Windows icon file already matches its original."));
    } else LogE(L"Original Windows icons: " + std::to_wstring(n) + L" file(s) restored, " + std::to_wstring(failed) + L" could not be restored. Restart Windows and run Restore again.");
    WriteIconReport(L"Action: Restore original Windows icons");
    return failed == 0;
}

// Applies a 7tsp pack stored in Program Files\AnkhCore\IconPacks\<id>.
// Two phases: build every patched system file into staging first, touching
// nothing live; only if at least one built do we swap files in. A build
// failure therefore never leaves the PC on the bare original icons, and the
// stored state always matches what is really on disk.
inline bool ApplyResPack(const std::wstring& id, const IconProgress& prog) {
    const IconPack* pk = FindPack(id);
    const std::wstring nm = pk ? std::wstring(pk->name) + (*pk->variant ? L" " + std::wstring(pk->variant) : L"") : id;
    const std::wstring dir = PackDir(id);
    const auto files = ListDir(dir, false, L"*.res.dz");
    if (files.empty()) { LogE(L"Icon pack files are missing: " + dir); return false; }
    PrepareBackupDir();
    const std::wstring staging = DataDir() + L"\\Staging";
    Evict(staging, false, false);
    EnsureDir(staging);
    const IconState st = GetIconState();
    LogI(L"Applying icon pack " + nm + L" (" + std::to_wstring(files.size()) + L" files).");

    // ---- phase 1: build patched copies in staging (no live file touched)
    struct Pending { std::wstring target, staged; };
    std::vector<Pending> pending;
    int skipped = 0, buildFail = 0;
    for (size_t fi = 0; fi < files.size(); ++fi) {
        std::vector<uint8_t> z, raw;
        if (!ReadAll(dir + L"\\" + files[fi], z)) { LogE(L"Cannot read pack file " + std::to_wstring(fi + 1) + L"/" + std::to_wstring(files.size()) + L": " + files[fi]); ++buildFail; continue; }
        if (!LoadDz(z, raw)) { LogE(L"Damaged pack file " + std::to_wstring(fi + 1) + L"/" + std::to_wstring(files.size()) + L": " + files[fi] + L" (" + std::to_wstring(z.size()) + L" bytes compressed)"); ++buildFail; continue; }
        LogI(L"File " + std::to_wstring(fi + 1) + L"/" + std::to_wstring(files.size()) + L": " + files[fi].substr(0, files[fi].size() - 7) + L" (" + FormatBytes(raw.size()) + L" of icons)");
        std::vector<ResItem> items;
        if (!ParseRes(raw, items) || items.empty()) { LogW(L"  -> no resources could be read, skipped"); ++skipped; continue; }
        const auto targets = ResTargets(files[fi]);
        if (targets.empty()) { LogW(L"  -> not present on this Windows, skipped"); ++skipped; continue; }
        for (const auto& t : targets) {
            if (prog) prog(L"Preparing (1 of 2): " + NameOf(t), ProgressOfCount(fi, files.size()));   // pack files finished before this one
            if (!EnsureBackup(t)) { LogE(L"  -> could not back up " + t); ++buildFail; continue; }
            const std::wstring tmp = staging + L"\\" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(pending.size()) + L"_" + NameOf(t);
            int written = 0;
            const DWORD t0 = GetTickCount();
            if (!BuildPatched(BackupPathFor(t), items, tmp, written)) { LogE(L"  -> could not build " + t); DeleteFileW(tmp.c_str()); ++buildFail; continue; }
            LogI(L"  -> prepared " + t + L" (" + std::to_wstring(written) + L" icons, " + std::to_wstring(GetTickCount() - t0) + L" ms)");
            pending.push_back({t, tmp});
        }
    }
    if (pending.empty()) {
        Evict(staging, false, false);
        LogE(L"Icon pack " + nm + L": nothing could be prepared (" + std::to_wstring(buildFail) + L" errors, " +
             std::to_wstring(skipped) + L" not on this Windows). No system files were changed; your current icons are unchanged.");
        WriteIconReport(L"Action: Apply icon pack " + nm + L" (failed in preparation)");
        return false;
    }

    // ---- phase 2: swap the prepared files in.
    // Stop the shell first so it releases its handles on shell32/imageres .mun,
    // so they can be overwritten in place instead of waiting for a restart.
    if (prog) prog(L"Stopping Explorer...", kProgressUnknown);
    KillProcess(L"explorer.exe");
    if (st.active == L"hud") { LogI(L"Undoing the previous HUD Evolution pack first."); RestoreTargets(BackedUpTargets(), prog); }
    std::vector<std::wstring> patched;
    int done = 0, swapFail = 0;
    for (size_t i = 0; i < pending.size(); ++i) {
        if (prog) prog(L"Installing (2 of 2): " + NameOf(pending[i].target), ProgressOfCount(i, pending.size()));
        if (SwapSystemFile(pending[i].target, pending[i].staged)) { patched.push_back(pending[i].target); ++done; }
        else ++swapFail;
        DeleteFileW(pending[i].staged.c_str());
    }
    // files the previous res-pack changed that this one does not cover
    if (done > 0) {
        std::vector<std::wstring> leftover;
        for (const auto& p : st.patched) if (std::find(patched.begin(), patched.end(), p) == patched.end()) leftover.push_back(p);
        if (!leftover.empty()) RestoreTargets(leftover, prog);
    }
    Evict(staging, false, false);
    ScheduleIconCacheReset();
    if (prog) prog(L"Restarting Explorer...", kProgressUnknown);
    StartExplorer();
    if (done > 0) { SaveIconState(id, patched); LogOk(L"Icon pack " + nm + L": " + std::to_wstring(done) + L" file(s) updated" +
                                                       (swapFail ? L", " + std::to_wstring(swapFail) + L" failed" : L"") + L". A restart makes every icon refresh."); }
    else { SaveIconState(st.active, st.patched); LogE(L"Icon pack " + nm + L": prepared files could not be installed (all " + std::to_wstring(swapFail) + L" swaps failed). Icons unchanged."); }
    WriteIconReport(L"Action: Apply icon pack " + nm);
    return done > 0;
}
// Union of every file any stored pack touches (backed up before an iPack).
inline std::vector<std::wstring> AllPackTargets() {
    std::set<std::wstring> s;
    for (const auto& p : IconPacks()) {
        if (p.ipack) continue;
        for (const auto& f : ListDir(PackDir(p.id), false, L"*.res.dz"))
            for (const auto& t : ResTargets(f)) s.insert(t);
    }
    return {s.begin(), s.end()};
}
inline std::wstring HudExe() { return AppDir() + L"\\7tsp\\HUD_Evolution_iPack.exe"; }
// Prepares for the HUD iPack: originals back, backups of every known file.
inline bool PrepareIPack(const IconProgress& prog) {
    PrepareBackupDir();
    const IconState st = GetIconState();
    if (!st.patched.empty()) RestoreTargets(st.patched, prog);
    const auto all = AllPackTargets();
    int i = 0;
    for (const auto& t : all) { if (prog) prog(L"Backing up " + NameOf(t), ProgressOfCount(static_cast<size_t>(i), all.size())); ++i; EnsureBackup(t); }
    return FileExists(HudExe());
}
inline bool RunIPack() {
    if (!FileExists(HudExe())) { LogE(L"HUD Evolution installer is missing."); return false; }
    LogI(L"Starting the HUD Evolution installer...");
    Run(HudExe(), L"", false);
    ScheduleIconCacheReset();
    SaveIconState(L"hud", {});
    LogOk(L"HUD Evolution installer finished.");
    return true;
}

// ------------------------------------------------- 7tsp icon packs (primary)
// Icons are applied with the bundled 7tsp (7-Taskbar-Tweaker's icon patcher),
// the proven community tool, via its silent command line. AnkhCore runs
// elevated, so the child process has the admin rights 7tsp needs to replace
// protected system files (imageres.dll, etc.).
inline std::wstring TspDir() { return AppDir() + L"\\7tsp"; }
inline std::wstring TspExe() { return TspDir() + L"\\7tsp.exe"; }
inline std::wstring TspPack(const std::wstring& id) {
    if (id == L"tech")         return TspDir() + L"\\tech.zip";
    if (id == L"magnum")       return TspDir() + L"\\magnum.zip";
    if (id == L"sweet")        return TspDir() + L"\\sweet.zip";
    if (id == L"bigsur-dark")  return TspDir() + L"\\bigsur_dark.zip";
    if (id == L"bigsur-light") return TspDir() + L"\\bigsur_light.zip";
    if (id == L"bigsur")       return TspDir() + L"\\bigsur_reg.zip";
    return L"";
}
inline bool ApplyPack7tsp(const std::wstring& id, const IconProgress& prog) {
    if (id == L"hud") return RunIPack();   // HUD keeps its own iPack installer
    const std::wstring exe = TspExe(), pack = TspPack(id);
    if (!FileExists(exe))  { LogE(L"7tsp.exe is missing from the install."); return false; }
    if (!FileExists(pack)) { LogE(L"Icon pack file is missing: " + NameOf(pack)); return false; }
    LogI(L"Applying '" + id + L"' with 7tsp:  7tsp.exe -install \"" + NameOf(pack) + L"\" -silent");
    if (prog) prog(L"Running 7tsp (this can take a minute)...", kProgressUnknown);   // an external program: its progress is not known here
    // 7tsp.exe -install "<pack>" -silent
    const DWORD rc = Run(exe, L"-install \"" + pack + L"\" -silent", false, 20 * 60 * 1000);
    if (rc == kRunTimedOut) { LogE(L"7tsp did not finish within 20 minutes; the pack was not confirmed as applied. Check 7tsp's window, then restart."); return false; }
    if (rc == kRunLaunchFailed) { LogE(L"7tsp could not be started (error " + std::to_wstring(GetLastError()) + L")."); return false; }
    if (rc != 0) { LogE(L"7tsp reported an error (exit code " + std::to_wstring(rc) + L") for pack '" + id + L"'."); return false; }
    LogI(L"7tsp exit code: 0");
    ScheduleIconCacheReset();
    // keep the list of files AnkhCore itself patched earlier, so Restore still covers them
    SaveIconState(id, GetIconState().patched);
    if (prog) prog(L"7tsp finished. Restart Windows to see every icon.", 100);
    LogOk(L"7tsp finished for pack '" + id + L"' (exit code 0). The icons are confirmed only after a restart.");
    return true;
}
// Open 7tsp's own window so the user can Restore to the default icons
// (7tsp keeps its own backups, so its restore is the matching one).
inline bool Restore7tsp() {
    if (!FileExists(TspExe())) { LogE(L"7tsp.exe is missing."); return false; }
    LogI(L"Opening 7tsp \x2014 click \"Restore\" there to put the default icons back.");
    Run(TspExe(), L"", false, 0);   // detached GUI
    return true;
}
// SFC-based deep restore (original files from Windows' component store).
// SEC-3: every sfc run counts. The recorded icon state is cleared only when all of them succeeded.
inline bool DeepRestoreIcons(const std::function<void(const std::wstring&)>& line) {
    OpTally tally;
    for (const auto& t : BackedUpTargets()) {
        LogI(L"sfc /scanfile=" + t);
        const DWORD rc = RunCapture(SysDir() + L"\\sfc.exe", L"/scanfile=\"" + t + L"\"", line);
        if (rc != 0) LogE(L"sfc reported a problem for " + NameOf(t) + (rc == kRunLaunchFailed ? L" (it could not be started)." : rc == kRunTimedOut ? L" (it timed out)." : L" (exit code " + std::to_wstring(rc) + L")."));
        tally.Add(rc == 0);
    }
    ScheduleIconCacheReset();
    if (tally.Success() && tally.Attempted() > 0) SaveIconState(L"", {});
    (tally.Success() ? LogOk : LogE)(L"Deep restore with SFC: " + tally.Text(L"icon file", L"icon files", L"checked by SFC"));
    return tally.Success();
}

// ------------------------------------------------- Runtime Resource Redirect (D41 / APP-8)
inline std::wstring WindhawkCliExePath() {
    const std::wstring pf = KnownFolder(FOLDERID_ProgramFiles);
    if (!pf.empty()) {
        const std::wstring p = pf + L"\\Windhawk\\windhawk-cli.exe";
        if (FileExists(p)) return p;
    }
    const std::wstring p2 = L"C:\\Program Files\\Windhawk\\windhawk-cli.exe";
    return p2;
}

inline std::wstring QuoteCliArg(const std::wstring& s) {
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++bs;
        } else if (c == L'"') {
            o.append(bs * 2 + 1, L'\\');
            o.push_back(L'"');
            bs = 0;
        } else {
            o.append(bs, L'\\');
            o.push_back(c);
            bs = 0;
        }
    }
    o.append(bs * 2, L'\\');
    o.push_back(L'"');
    return o;
}

// Executes `windhawk-cli.exe --json ...` and parses its UTF-8 JSON output.
// In kTestBuild without `appRootOverride`, mutating commands are blocked by TestRefused.
inline bool RunWindhawkCliJson(const std::wstring& subArgs,
                               JVal& outJson,
                               std::wstring& outErr,
                               const std::wstring& appRootOverride = L"",
                               DWORD timeoutMs = 60000) {
    outJson = JVal{};
    outErr.clear();
    const std::wstring exe = WindhawkCliExePath();
    if (!FileExists(exe)) {
        outErr = L"Windhawk CLI not found at " + exe;
        return false;
    }
    const std::wstring low = Lower(subArgs);
    const bool isReadOnly = (low.find(L"mod list") != std::wstring::npos ||
                             low.find(L"mod show") != std::wstring::npos ||          // FT-C01: the settings schema is read-only too
                             low.find(L"mod settings get") != std::wstring::npos ||
                             low.find(L"source meta") != std::wstring::npos ||
                             low.find(L"status") != std::wstring::npos);
    if (kTestBuild && appRootOverride.empty() && !isReadOnly) {
        TestRefused(L"windhawk-cli " + subArgs);
        outErr = L"Blocked in TEST build (host Windhawk protection).";
        return false;
    }

    std::wstring fullArgs = L"--json ";
    if (!appRootOverride.empty()) {
        fullArgs += L"--app-root " + QuoteCliArg(appRootOverride) + L" ";
    }
    fullArgs += subArgs;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        outErr = L"CreatePipe failed.";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = QuoteCliArg(exe) + L" " + fullArgs;
    // Only stdout is read: with --json it carries the whole result, errors included. stderr carries progress text
    // ("Compiling for x86...") during `mod install`, which, mixed into stdout, made the JSON unreadable (found 2026-11-15).
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = nul != INVALID_HANDLE_VALUE ? nul : wr;
    si.hStdInput  = nullptr;
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(rd);
        CloseHandle(wr);
        outErr = L"Failed to launch windhawk-cli.exe.";
        return false;
    }
    CloseHandle(wr);

    std::string raw;
    const DWORD t0 = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(rd, buf, static_cast<DWORD>(std::min<size_t>(sizeof(buf), avail)), &got, nullptr) || !got) break;
            raw.append(buf, got);
        }
        if (WaitForSingleObject(pi.hProcess, 25) == WAIT_OBJECT_0) {
            char buf[4096];
            DWORD got = 0;
            while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) raw.append(buf, got);
            break;
        }
        if (timeoutMs != INFINITE && GetTickCount() - t0 >= timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(rd);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            outErr = L"windhawk-cli timed out.";
            return false;
        }
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Trim trailing whitespace/newlines before parsing JSON
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n' || raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
    if (!JsonParse(raw, outJson)) {
        outErr = L"windhawk-cli returned invalid JSON (exit " + std::to_wstring(exitCode) + L").";
        return false;
    }
    const JVal* succ = outJson.Get("success");
    if (!succ || !succ->Is(JVal::Bool) || !succ->b) {
        const JVal* errObj = outJson.Get("error");
        const JVal* msg = errObj ? errObj->Get("message") : nullptr;
        outErr = msg && msg->Is(JVal::Str) ? Utf8ToW(msg->s) : (L"windhawk-cli command failed (exit " + std::to_wstring(exitCode) + L").");
        return false;
    }
    return exitCode == 0;
}

struct ResourceRedirectModInfo {
    bool installed = false;
    bool enabled = false;
    std::wstring modId;   // "local@icon-resource-redirect" or "icon-resource-redirect"
    std::wstring version;
};

inline ResourceRedirectModInfo QueryResourceRedirectMod(const std::wstring& appRootOverride = L"") {
    ResourceRedirectModInfo info;
    JVal root;
    std::wstring err;
    if (!RunWindhawkCliJson(L"mod list", root, err, appRootOverride)) return info;
    const JVal* data = root.Get("data");
    const JVal* mods = data ? data->Get("mods") : nullptr;
    if (!mods || !mods->Is(JVal::Arr)) return info;
    for (const char* candidate : {"local@icon-resource-redirect", "icon-resource-redirect"}) {
        for (const auto& item : mods->a) {
            const JVal* idv = item.Get("id");
            if (idv && idv->Is(JVal::Str) && idv->s == candidate) {
                info.installed = true;
                info.modId = Utf8ToW(idv->s);
                const JVal* cfg = item.Get("config");
                const JVal* dis = cfg ? cfg->Get("disabled") : nullptr;
                info.enabled = !(dis && dis->Is(JVal::Bool) && dis->b);
                const JVal* meta = item.Get("metadata");
                const JVal* ver = meta ? meta->Get("version") : nullptr;
                if (ver && ver->Is(JVal::Str)) info.version = Utf8ToW(ver->s);
                return info;
            }
        }
    }
    return info;
}

// Updates Resource Redirect `themePaths` via windhawk-cli, verifies readback,
// and rolls back automatically if verification fails.
// When `newThemeDir` is empty, removes AnkhCore's managed theme path (restoring default icons).
inline bool UpdateResourceRedirectThemePaths(const std::wstring& modId,
                                             const std::wstring& managedRoot,
                                             const std::wstring& prevManagedPath,
                                             const std::wstring& newThemeDir,
                                             std::wstring& outErr,
                                             const std::wstring& appRootOverride = L"") {
    JVal beforeDoc;
    if (!RunWindhawkCliJson(L"mod settings get " + QuoteCliArg(modId), beforeDoc, outErr, appRootOverride)) {
        return false;
    }
    const JVal* beforeData = beforeDoc.Get("data");
    const JVal* beforeSettings = beforeData ? beforeData->Get("settings") : nullptr;
    if (!beforeSettings || !beforeSettings->Is(JVal::Obj)) {
        outErr = L"Resource Redirect settings object missing.";
        return false;
    }
    auto getStrProp = [](const JVal* obj, const char* k) -> std::string {
        const JVal* v = obj ? obj->Get(k) : nullptr;
        return (v && v->Is(JVal::Str)) ? v->s : std::string();
    };
    const std::string origIconTheme = getStrProp(beforeSettings, "iconTheme");
    const std::string origDisThumb  = getStrProp(beforeSettings, "disableThumbnails");
    const std::string origAllRes    = getStrProp(beforeSettings, "allResourceRedirect");

    const std::vector<std::wstring> beforePaths = ExtractThemePathsFromFlatSettings(*beforeSettings);
    const std::vector<std::wstring> targetPaths = PlanThemePathsUpdate(beforePaths, managedRoot, prevManagedPath, newThemeDir);
    const std::vector<std::wstring> setPairs    = FormatThemePathsSetArgs(beforePaths.size(), targetPaths);

    std::wstring setCmd = L"mod settings set " + QuoteCliArg(modId);
    for (const auto& kv : setPairs) setCmd += L" " + QuoteCliArg(kv);

    JVal setDoc;
    if (!RunWindhawkCliJson(setCmd, setDoc, outErr, appRootOverride)) {
        return false;
    }

    // Read back and verify
    JVal afterDoc;
    if (!RunWindhawkCliJson(L"mod settings get " + QuoteCliArg(modId), afterDoc, outErr, appRootOverride)) {
        return false;
    }
    const JVal* afterData = afterDoc.Get("data");
    const JVal* afterSettings = afterData ? afterData->Get("settings") : nullptr;
    if (!afterSettings || !afterSettings->Is(JVal::Obj)) {
        outErr = L"Resource Redirect readback settings missing.";
        return false;
    }
    const std::vector<std::wstring> afterPaths = ExtractThemePathsFromFlatSettings(*afterSettings);
    bool match = (afterPaths.size() == targetPaths.size());
    if (match) {
        for (size_t i = 0; i < afterPaths.size(); ++i) {
            if (NormPathKey(afterPaths[i]) != NormPathKey(targetPaths[i])) { match = false; break; }
        }
    }
    // Verify other Resource Redirect settings were preserved untouched
    const bool preservedOther =
        getStrProp(afterSettings, "iconTheme") == origIconTheme &&
        getStrProp(afterSettings, "disableThumbnails") == origDisThumb &&
        getStrProp(afterSettings, "allResourceRedirect") == origAllRes;

    if (!match || !preservedOther) {
        // Automatic rollback to beforePaths
        const std::vector<std::wstring> rbPairs = FormatThemePathsSetArgs(afterPaths.size(), beforePaths);
        std::wstring rbCmd = L"mod settings set " + QuoteCliArg(modId);
        for (const auto& kv : rbPairs) rbCmd += L" " + QuoteCliArg(kv);
        JVal rbDoc; std::wstring rbErr;
        RunWindhawkCliJson(rbCmd, rbDoc, rbErr, appRootOverride);
        outErr = !match ? L"Resource Redirect themePaths readback verification failed (rolled back)."
                        : L"Resource Redirect unrelated settings changed unexpectedly (rolled back).";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- FT-2 / A01: refresh after a Resource Redirect change
// The last result of an apply or restore (or of the user's Explorer restart), for the Icon Manager page.
inline std::mutex& IconRefreshMx() { static std::mutex m; return m; }
inline IconRefreshReport& IconRefreshStore() { static IconRefreshReport r; return r; }
inline IconRefreshReport LastIconRefresh() { std::lock_guard<std::mutex> lk(IconRefreshMx()); return IconRefreshStore(); }
inline void SetLastIconRefresh(const IconRefreshReport& r) { std::lock_guard<std::mutex> lk(IconRefreshMx()); IconRefreshStore() = r; }

// Read-only: is a module of the mod `modId` loaded into an Explorer process of this user session? Nothing is changed in
// Explorer. With waitMs > 0 it looks again every 300 ms (Windhawk may still be loading the mod just after a change).
// Unknown: there is no Explorer in this session to look at, or its modules cannot be read.
inline IconShellState ExplorerLoadsMod(const std::wstring& modId, DWORD waitMs = 0) {
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
        bool readable = false;
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
                DWORD s = 0; if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != session) continue;
                HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pe.th32ProcessID);
                if (ms == INVALID_HANDLE_VALUE) continue;
                readable = true;
                MODULEENTRY32W me{sizeof(me)};
                for (BOOL k = Module32FirstW(ms, &me); k; k = Module32NextW(ms, &me))
                    if (IconModuleIsMod(me.szModule, modId)) { CloseHandle(ms); CloseHandle(snap); return IconShellState::Loaded; }
                CloseHandle(ms);
            }
            CloseHandle(snap);
        }
        if (!readable) return IconShellState::Unknown;
        if (GetTickCount64() >= until) return IconShellState::NotLoaded;
        Sleep(300);
    }
}

// Restarts Explorer, only ever called after the user confirmed it (never automatic). Ends the Explorer processes of this
// user session only (a second signed-in user is left alone); Windows starts the shell again by itself, and if it has not
// within a few seconds it is started here. Open File Explorer windows close; nothing else is affected.
// 2.8.6: the app is told when IT restarts Explorer (starting = true before, false after the taskbar is back), so the desktop Sidebar waits for the new shell
// instead of re-creating itself in the middle of the restart (that produced the "no desktop layer" fallback window and a Sidebar that stayed gone).
inline std::function<void(bool)>& ExplorerRestartHook() { static std::function<void(bool)> h; return h; }
struct ExplorerRestartScope {
    ExplorerRestartScope() { if (auto& h = ExplorerRestartHook()) h(true); }
    ~ExplorerRestartScope() { if (auto& h = ExplorerRestartHook()) h(false); }
};
inline bool RestartExplorerShell() {
    if (kTestBuild) { TestRefused(L"restart Explorer"); return false; }
    ExplorerRestartScope restartScope;
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    int ended = 0, found = 0;
    if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
            DWORD s = 0; if (!ProcessIdToSessionId(pe.th32ProcessID, &s) || s != session) continue;
            ++found;
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) { if (TerminateProcess(h, 0)) ++ended; CloseHandle(h); }
        }
        CloseHandle(snap);
    }
    if (found == 0) { LogE(L"Explorer is not running in this session; nothing was restarted."); return false; }
    if (ended < found) LogW(L"Explorer: " + std::to_wstring(found - ended) + L" of " + std::to_wstring(found) + L" process(es) could not be ended.");
    Sleep(1500);
    StartExplorer();   // waits, starts the shell only if Windows has not, and waits for the taskbar
    const bool up = FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr;
    if (up && ended > 0) {
        IconRefreshReport r = LastIconRefresh(); r.explorerRestarted = true; SetLastIconRefresh(r);
        LogOk(L"Explorer restarted.");
    } else LogE(up ? L"Explorer could not be ended." : L"Explorer did not come back within the wait; sign out and in, or start explorer.exe from Task Manager.");
    return up && ended > 0;
}

// 2.8.3: Explorer restarts by itself after an icon theme was applied SUCCESSFULLY (so the taskbar, Start menu and File Explorer load the new icons).
// Never after a failed apply, never twice at once, never for another operation. The manual "Restart Explorer" button stays as the recovery path.
// Pure decision + one guarded call; the real restart is injected (RestartExplorerShell) so tests never touch Explorer.
enum class IconAutoRestart { Done, RestartFailed, SkippedApplyFailed, SkippedAlreadyRestarting };
inline const wchar_t* IconAutoRestartText(IconAutoRestart r) {
    switch (r) {
    case IconAutoRestart::Done: return L"Explorer was restarted automatically so the new icons load.";
    case IconAutoRestart::RestartFailed: return L"The icons were applied, but Explorer could not be restarted automatically. Use Restart Explorer below, or sign out and in.";
    case IconAutoRestart::SkippedApplyFailed: return L"The icons were not applied, so Explorer was left as it is.";
    default: return L"An Explorer restart is already in progress.";
    }
}
// 2.8.6: the restart waits `settleSeconds` first: Windhawk is still writing and loading the redirect while the apply returns, and ending Explorer in the middle of it
// leaves the change half-saved. The wait is counted down in the status line; `sleeper` is injected so tests do not wait.
constexpr int kIconRestartSettleSeconds = 8;
inline IconAutoRestart IconRestartAfterApply(bool applyOk, const std::function<bool()>& restart, std::atomic<bool>& inFlight, const std::function<void(const std::wstring&)>& status = {},
                                             int settleSeconds = 0, const std::function<void(int)>& sleeper = {}) {
    if (!applyOk) return IconAutoRestart::SkippedApplyFailed;
    if (inFlight.exchange(true)) return IconAutoRestart::SkippedAlreadyRestarting;   // a second request while one runs is dropped, not queued
    for (int left = settleSeconds; left > 0; --left) {
        if (status) status(L"Finishing the icon change, restarting Explorer in " + std::to_wstring(left) + L" s" + std::wstring(1, wchar_t(0x2026)));
        if (sleeper) sleeper(1000); else Sleep(1000);
    }
    if (status) status(L"Restarting Explorer so the new icons load" + std::wstring(1, wchar_t(0x2026)));
    const bool ok = restart && restart();
    inFlight = false;
    return ok ? IconAutoRestart::Done : IconAutoRestart::RestartFailed;
}

// Applies a prebuilt icon theme (`id`) at runtime using Windhawk Resource Redirect 1.3.
// FT-2 / A01: no Windows restart is needed or asked for. After the setting was written and read back, Windows is told that
// icons changed and the result is recorded (LastIconRefresh) with what could be verified about Explorer.
inline bool ApplyIconThemeRedirect(const std::wstring& id,
                                   const IconProgress& prog,
                                   const std::wstring& appRootOverride = L"",
                                   bool allowDisabledForTest = false) {
    const IconPack* pk = FindPack(id);
    const std::wstring nm = pk ? std::wstring(pk->name) + (*pk->variant ? L" " + std::wstring(pk->variant) : L"") : id;
    const std::wstring themeDir = IconThemeDir(id);
    // Progress (LOG-2): how many steps this run has is known only after the mod has been queried, so the
    // two checks before that are shown as work of unknown length; the steps after them are counted.
    if (prog) prog(L"Validating prebuilt icon theme " + nm + L"...", kProgressUnknown);
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    std::wstring valErr;
    if (!ValidateIconThemeDir(themeDir, &pairs, &valErr)) {
        LogE(L"Icon theme '" + nm + L"' is not installed or failed validation: " + valErr);
        return false;
    }

    if (prog) prog(L"Checking Windhawk Resource Redirect...", kProgressUnknown);
    ResourceRedirectModInfo mod = QueryResourceRedirectMod(appRootOverride);
    const bool willEnable = !allowDisabledForTest && (!mod.installed || !mod.enabled);   // a fresh install may come up enabled: then that step closes at once
    OpSteps steps((mod.installed ? 0 : 1) + (willEnable ? 1 : 0) + 1 + (appRootOverride.empty() ? 2 : 0));
    auto step = [&](const std::wstring& what) { steps.Next(); if (prog) prog(steps.Label(what), steps.Pct()); };
    if (!mod.installed) {
        const std::wstring srcFile = ResourceRedirectModSourcePath();
        if (!FileExists(srcFile)) {
            LogE(L"Windhawk Resource Redirect mod is not installed and source file was not found: " + srcFile);
            return false;
        }
        step(L"Installing Windhawk Resource Redirect 1.3...");
        JVal instDoc; std::wstring instErr;
        std::wstring instArgs = L"mod install --file " + QuoteCliArg(srcFile);
        if (allowDisabledForTest) instArgs += L" --disabled";
        if (!RunWindhawkCliJson(instArgs, instDoc, instErr, appRootOverride, 120000)) {
            LogE(L"Could not install Windhawk Resource Redirect: " + instErr);
            return false;
        }
        mod = QueryResourceRedirectMod(appRootOverride);
    }
    if (!mod.installed) {
        LogE(L"Windhawk Resource Redirect mod could not be located after installation.");
        return false;
    }
    if (willEnable) {
        step(mod.enabled ? L"Windhawk Resource Redirect is enabled." : L"Enabling Windhawk Resource Redirect...");
        if (!mod.enabled) {
            JVal enDoc; std::wstring enErr;
            if (!RunWindhawkCliJson(L"mod enable " + QuoteCliArg(mod.modId), enDoc, enErr, appRootOverride)) {
                LogE(L"Could not enable Windhawk Resource Redirect: " + enErr);
                return false;
            }
        }
    }

    step(L"Updating Resource Redirect themePaths...");
    const std::wstring prevThemeDir = Cfg().Str(L"icons.activeThemePath", L"");
    std::wstring setErr;
    if (!UpdateResourceRedirectThemePaths(mod.modId, IconThemesRoot(), prevThemeDir, themeDir, setErr, appRootOverride)) {
        LogE(L"Failed to apply icon theme '" + nm + L"': " + setErr);
        return false;
    }

    IconRefreshReport rep; rep.applied = true;
    if (appRootOverride.empty()) {
        Cfg().SetStr(L"icons.activeThemePath", themeDir);
        SaveIconState(id, GetIconState().patched, false);   // Resource Redirect: no Windows restart (FT-2 / A01)
        step(L"Telling Windows that icons changed...");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
        rep.shellNotified = true;
        step(L"Checking that Explorer has Resource Redirect loaded...");
        rep.inExplorer = ExplorerLoadsMod(mod.modId, 4000);
        SetLastIconRefresh(rep);
    }
    if (prog) prog(L"Applied " + nm + L" via Resource Redirect.", 100);
    LogOk(L"Icon theme '" + nm + L"' applied via Windhawk Resource Redirect (" + std::to_wstring(pairs.size()) + L" redirected targets; 0 system files modified).");
    if (appRootOverride.empty()) {
        const IconRefreshAdvice adv = IconAdvice(rep);
        (adv.warn ? LogW : LogI)(adv.text);
    }
    return true;
}

// Restores default Windows icons by removing AnkhCore's managed theme path from Resource Redirect.
inline bool RestoreDefaultIconsRedirect(const IconProgress& prog,
                                        const std::wstring& appRootOverride = L"") {
    if (prog) prog(L"Checking Windhawk Resource Redirect...", kProgressUnknown);
    const ResourceRedirectModInfo mod = QueryResourceRedirectMod(appRootOverride);
    OpSteps steps((mod.installed ? 1 : 0) + (appRootOverride.empty() ? 2 : 0));
    auto step = [&](const std::wstring& what) { steps.Next(); if (prog) prog(steps.Label(what), steps.Pct()); };
    if (mod.installed) {
        step(L"Clearing AnkhCore theme path from Resource Redirect...");
        const std::wstring prevThemeDir = Cfg().Str(L"icons.activeThemePath", L"");
        std::wstring err;
        if (!UpdateResourceRedirectThemePaths(mod.modId, IconThemesRoot(), prevThemeDir, L"", err, appRootOverride)) {
            LogE(L"Could not clear AnkhCore icon theme from Resource Redirect: " + err);
            return false;
        }
    }
    IconRefreshReport rep; rep.applied = true; rep.removed = true;
    if (appRootOverride.empty()) {
        Cfg().SetStr(L"icons.activeThemePath", L"");
        SaveIconState(L"", GetIconState().patched, false);   // Resource Redirect: no Windows restart (FT-2 / A01)
        step(L"Telling Windows that icons changed...");
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
        rep.shellNotified = true;
        step(L"Checking that Explorer has Resource Redirect loaded...");
        rep.inExplorer = mod.installed ? ExplorerLoadsMod(mod.modId, 0) : IconShellState::Unknown;
        SetLastIconRefresh(rep);
    }
    if (prog) prog(L"Default Windows icons restored.", 100);
    LogOk(L"Default Windows icons restored via Resource Redirect.");
    if (appRootOverride.empty()) {
        const IconRefreshAdvice adv = IconAdvice(rep);
        (adv.warn ? LogW : LogI)(adv.text);
    }
    return true;
}

}  // namespace ak
