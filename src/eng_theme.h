// AnkhCore — appearance engines: SecureUxTheme patch, As3aD theme (Windows
// 11 only), mouse cursors, Anubis wallpapers.
#pragma once
#include "base.h"
#include "settings.h"
#include "inflate.h"
#include "install.h"

namespace ak {

// ---------------------------------------------------------------- SecureUxTheme (ThemeDll.dll)
// Install flags verified in ThemeDll's install routine:
//   1 hook explorer, 2 hook SystemSettings, 4 hook LogonUI, 8 remove DefaultColors
constexpr ULONG kSuxInstallHookExplorer        = 1u << 0; // 0x01: NOT enabled by AnkhCore
constexpr ULONG kSuxInstallHookSystemSettings  = 1u << 1; // 0x02: NOT enabled by AnkhCore
constexpr ULONG kSuxHookLogonUI                = 1u << 2; // 0x04: Hook LogonUI (Enabled)
constexpr ULONG kSuxRemoveDefaultColors        = 1u << 3; // 0x08: Remove DefaultColors (Enabled)
constexpr ULONG kSuxDesiredInstallFlags        = kSuxHookLogonUI | kSuxRemoveDefaultColors;

// State flags returned by secureuxtheme_get_state_flags():
constexpr ULONG kSuxStateInstalled             = 1u << 0; // 0x01
constexpr ULONG kSuxStateLoadedSession         = 1u << 1; // 0x02
constexpr ULONG kSuxStateLoaded                = 1u << 2; // 0x04 (global loaded)
constexpr ULONG kSuxStateExplorerHooked        = 1u << 3; // 0x08
constexpr ULONG kSuxStateSystemSettingsHooked  = 1u << 4; // 0x10
constexpr ULONG kSuxStateLogonUIHooked         = 1u << 5; // 0x20

struct Sux {
    HMODULE m = nullptr;
    HRESULT (*install)(ULONG) = nullptr;
    HRESULT (*uninstall)() = nullptr;
    ULONG (*state)() = nullptr;
    BOOLEAN (*hookTest)(LPCWSTR) = nullptr;
    HRESULT (*hookAdd)(LPCWSTR) = nullptr;
    HRESULT (*hookRemove)(LPCWSTR) = nullptr;
    HRESULT (*init)() = nullptr;
    HRESULT (*count)(PULONG) = nullptr;
    HRESULT (*get)(ULONG, void**) = nullptr;
    HRESULT (*name)(void*, LPWSTR, SIZE_T) = nullptr;
    HRESULT (*vspath)(void*, LPWSTR, SIZE_T) = nullptr;
    void (*release)(void*) = nullptr;
    HRESULT (*setActive)(HWND, ULONG, BOOLEAN, ULONG, ULONG) = nullptr;
    HRESULT (*sigCheck)(LPCWSTR) = nullptr;
    HRESULT (*sigFix)(LPCWSTR) = nullptr;
    bool Load(const std::wstring& dir) {
        m = LoadLibraryW((dir + L"\\ThemeDll.dll").c_str());
        if (!m) return false;
        auto P = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(m, n)); };
        install = reinterpret_cast<decltype(install)>(P("secureuxtheme_install"));
        uninstall = reinterpret_cast<decltype(uninstall)>(P("secureuxtheme_uninstall"));
        state = reinterpret_cast<decltype(state)>(P("secureuxtheme_get_state_flags"));
        hookTest = reinterpret_cast<decltype(hookTest)>(P("secureuxtheme_hook_test"));
        hookAdd = reinterpret_cast<decltype(hookAdd)>(P("secureuxtheme_hook_add"));
        hookRemove = reinterpret_cast<decltype(hookRemove)>(P("secureuxtheme_hook_remove"));
        init = reinterpret_cast<decltype(init)>(P("themetool_init"));
        count = reinterpret_cast<decltype(count)>(P("themetool_get_theme_count"));
        get = reinterpret_cast<decltype(get)>(P("themetool_get_theme"));
        name = reinterpret_cast<decltype(name)>(P("themetool_theme_get_display_name"));
        vspath = reinterpret_cast<decltype(vspath)>(P("themetool_theme_get_vs_path"));
        release = reinterpret_cast<decltype(release)>(P("themetool_theme_release"));
        setActive = reinterpret_cast<decltype(setActive)>(P("themetool_set_active"));
        sigCheck = reinterpret_cast<decltype(sigCheck)>(P("themetool_signature_check"));
        sigFix = reinterpret_cast<decltype(sigFix)>(P("themetool_signature_fix"));
        return install && state && init && count && get && name && release && setActive;
    }
    ~Sux() { if (m) FreeLibrary(m); }
    bool Loaded() { return state && ((state() & (kSuxStateLoaded | kSuxStateLoadedSession)) != 0); }
    HRESULT InitManager() {
        if (!init) return E_FAIL;
        const HRESULT hr = init();
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED)) return S_OK;
        return hr;
    }
    // Applies the theme whose display name (or visual style) matches.
    bool Apply(const std::function<bool(const std::wstring& name, const std::wstring& vs)>& match) {
        if (FAILED(InitManager())) return false;
        for (int tries = 0; tries < 4; ++tries) {
            ULONG n = 0; count(&n);
            for (ULONG i = 0; i < n; ++i) {
                void* th = nullptr;
                if (FAILED(get(i, &th)) || !th) continue;
                wchar_t dn[260] = {}, vs[MAX_PATH] = {};
                name(th, dn, 260);
                if (vspath) vspath(th, vs, MAX_PATH);
                release(th);
                if (match(dn, vs)) return SUCCEEDED(setActive(nullptr, i, TRUE, 1u << 8 /* no hourglass */, 1u << 2 /* silent */));
            }
            Sleep(800);
        }
        return false;
    }
};

// ---------------------------------------------------------------- As3aD primary theme & authoritative theme paths
constexpr char kAuthoritativeAs3aDThemeSha256[] = "64d336ee6f427f705ef2ab9371cd58e0ae33dd43c4cda9bffa8ef63d4d7c2cc8";
inline std::wstring ThemeUserPath() { return UserFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\Windows\\Themes\\As3aD.theme"; }
inline std::wstring ThemeWinPath() { return WinDir() + L"\\Resources\\Themes\\As3aD.theme"; }
inline std::wstring BibStylePath() { return WinDir() + L"\\Resources\\Themes\\BIB\\BIB Night - Arc.msstyles"; }
// 2.12 (owner): one As3aD theme per Windows. Windows 11 uses As3aD.theme (BIB Night - Arc); Windows 10 uses As3aD10.theme (After Dark Glass Cyan). Both carry the display name "As3aD",
// so everything that finds the theme by its name keeps working; only the file, its pinned hash and its visual style differ.
constexpr char kAuthoritativeAs3aD10ThemeSha256[] = "f8c1d830f7c60945bc933bcbf2179dfda4080fe19343666bd928422346a0e46d";
inline const char* AuthoritativeThemeSha(bool win11) { return win11 ? kAuthoritativeAs3aDThemeSha256 : kAuthoritativeAs3aD10ThemeSha256; }
inline std::wstring AuthoritativeThemeFile(bool win11) { return win11 ? L"As3aD.theme" : L"As3aD10.theme"; }
inline std::wstring GlassCyanStyleDir() { return WinDir() + L"\\Resources\\Themes\\After Dark Glass Cyan"; }
inline std::wstring GlassCyanStylePath() { return GlassCyanStyleDir() + L"\\After Dark Glass Cyan1.msstyles"; }
inline std::wstring ThemeStylePathFor(bool win11) { return win11 ? BibStylePath() : GlassCyanStylePath(); }
inline std::wstring Bib3StyleDir() { return WinDir() + L"\\Resources\\Themes\\BIB3"; }
inline std::wstring CursorDir() { return WinDir() + L"\\Cursors\\Ankhcore Prismatic"; }
inline std::wstring CursorLegacyDir() { return WinDir() + L"\\Cursors\\DIM v3.2 - Prismatic [PREMIUM]"; }
inline std::wstring WallDir() { return WinDir() + L"\\Web\\Wallpaper\\Anubis"; }
// 2.8: the Windows assets (cursor schemes, Anubis wallpapers, Windows themes) ship WITH the setup package (WindowsAssets\\) and are verified against COMPONENTS.sha256;
// the setup sets this to the verified folder. Before 2.8 they were read from D:\\Ankhcore\\windows, which exists on the developer PC only.
inline std::wstring& PackageAssetsRoot() { static std::wstring r; return r; }
inline std::wstring AuthoritativeThemesSourceDir() {
    if (!PackageAssetsRoot().empty() && DirExists(PackageAssetsRoot() + L"\\Themes")) return PackageAssetsRoot() + L"\\Themes";
    if (DirExists(AppDir() + L"\\WindowsThemes")) return AppDir() + L"\\WindowsThemes";
    return WinDir() + L"\\Resources\\Themes";
}
inline std::wstring AuthoritativeWallpapersSourceDir() {
    if (!PackageAssetsRoot().empty() && DirExists(PackageAssetsRoot() + L"\\Wallpapers\\Anubis")) return PackageAssetsRoot() + L"\\Wallpapers\\Anubis";
    if (DirExists(AppDir() + L"\\Wallpapers\\Anubis")) return AppDir() + L"\\Wallpapers\\Anubis";
    return WallDir();
}
inline std::wstring AuthoritativeCursorsSourceDir() {
    if (!PackageAssetsRoot().empty() && DirExists(PackageAssetsRoot() + L"\\Cursors")) return PackageAssetsRoot() + L"\\Cursors";
    if (DirExists(AppDir() + L"\\Cursors")) return AppDir() + L"\\Cursors";
    return WinDir() + L"\\Cursors";
}

inline std::wstring GetActiveVisualStylePath() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return L"";
    using GetCurThemeNameFn = HRESULT(WINAPI*)(LPWSTR, int, LPWSTR, int, LPWSTR, int);
    auto fn = reinterpret_cast<GetCurThemeNameFn>(GetProcAddress(ux, "GetCurrentThemeName"));
    if (!fn) return L"";
    wchar_t vs[MAX_PATH] = {};
    if (FAILED(fn(vs, MAX_PATH, nullptr, 0, nullptr, 0))) return L"";
    return vs;
}

inline bool IsAs3aDThemeActiveState(const std::wstring& currentThemeReg, const std::wstring& activeVsPath) {
    const std::wstring cur = Lower(currentThemeReg);
    const std::wstring vs = Lower(activeVsPath);
    if (cur.find(L"as3ad.theme") != std::wstring::npos) return true;
    if (vs.find(L"bib night - arc.msstyles") != std::wstring::npos) return true;
    if (vs.find(L"after dark glass cyan1.msstyles") != std::wstring::npos) return true;   // 2.12: the Windows 10 variant (As3aD10.theme)
    return false;
}

inline bool ThemeIsActive() {
    const std::wstring cur = RegStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes", L"CurrentTheme");
    return IsAs3aDThemeActiveState(cur, GetActiveVisualStylePath());
}
inline bool PatchLoaded() { Sux s; return s.Load(SuxDir()) && s.Loaded(); }

// ---------------------------------------------------------------- SecureUxTheme verification & recovery
struct SuxFileSpec { const wchar_t* name; const char* sha256; };
inline const std::vector<SuxFileSpec>& SuxRequiredFiles() {
    static const std::vector<SuxFileSpec> v = {
        {L"ThemeDll.dll",             "765560b253278a2cbfa88081b7ac43ebe386046106b3b1c061559c52ced53332"},
        {L"SecureUxTheme.signed.dll", "1f34d15eef9fffd1122904310c42de2131b8bcd1b830e3f2c26c71535d6f1722"},
        {L"ThemeTool.exe",            "84f7ee902d5b10c4a6b639de29ef84e9162e53fc4de3947e96fd77a7a8bb5257"},
    };
    return v;
}

inline bool IsPe64Binary(const std::wstring& path) {
    std::vector<uint8_t> buf;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    buf.resize(4096);
    DWORD nr = 0;
    const BOOL ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &nr, nullptr);
    CloseHandle(h);
    if (!ok || nr < 0x40) return false;
    const uint16_t mz = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
    if (mz != IMAGE_DOS_SIGNATURE) return false;
    uint32_t e_lfanew = 0;
    memcpy(&e_lfanew, buf.data() + 0x3C, sizeof(e_lfanew));
    if (e_lfanew > nr - 24) return false;
    uint32_t sig = 0;
    memcpy(&sig, buf.data() + e_lfanew, sizeof(sig));
    if (sig != IMAGE_NT_SIGNATURE) return false;
    uint16_t machine = 0;
    memcpy(&machine, buf.data() + e_lfanew + 4, sizeof(machine));
    return machine == IMAGE_FILE_MACHINE_AMD64;
}

struct SuxVerifyResult {
    bool ok = false;
    std::vector<std::wstring> missing;
    std::vector<std::wstring> corrupted;
    std::vector<std::wstring> wrongArch;
    std::wstring detail;
};

inline SuxVerifyResult VerifySuxDir(const std::wstring& dir) {
    SuxVerifyResult r;
    for (const auto& f : SuxRequiredFiles()) {
        const std::wstring p = dir + L"\\" + f.name;
        if (!FileExists(p)) { r.missing.push_back(f.name); continue; }
        if (!IsPe64Binary(p)) { r.wrongArch.push_back(f.name); continue; }
        if (Sha256File(p) != f.sha256) { r.corrupted.push_back(f.name); continue; }
    }
    r.ok = r.missing.empty() && r.corrupted.empty() && r.wrongArch.empty();
    auto join = [](const std::vector<std::wstring>& xs) {
        std::wstring s; for (size_t i = 0; i < xs.size(); ++i) { if (i) s += L", "; s += xs[i]; } return s;
    };
    if (r.ok) {
        r.detail = L"SecureUxTheme verified (3/3 x64 SHA-256 OK).";
    } else {
        std::wstring d;
        if (!r.missing.empty()) d += L"Missing: " + join(r.missing) + L". ";
        if (!r.wrongArch.empty()) d += L"Invalid x64 binary: " + join(r.wrongArch) + L". ";
        if (!r.corrupted.empty()) d += L"SHA-256 mismatch (corrupted): " + join(r.corrupted) + L". ";
        r.detail = d;
    }
    return r;
}

inline int RestoreSuxFromCacheDir(const std::wstring& cacheDir, const std::wstring& dstDir) {
    if (!DirExists(cacheDir)) return 0;
    int restored = 0;
    EnsureDir(dstDir);
    for (const auto& f : SuxRequiredFiles()) {
        const std::wstring src = cacheDir + L"\\" + f.name;
        const std::wstring dst = dstDir + L"\\" + f.name;
        const bool dstValid = FileExists(dst) && IsPe64Binary(dst) && Sha256File(dst) == f.sha256;
        if (dstValid) continue;
        if (FileExists(src) && IsPe64Binary(src) && Sha256File(src) == f.sha256) {
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE) && Sha256File(dst) == f.sha256) ++restored;
        }
    }
    return restored;
}

inline int RestoreSuxFromPackageFile(const std::wstring& pkgPath, const std::wstring& dstDir) {
    if (!FileExists(pkgPath)) return 0;
    std::vector<uint8_t> blob;
    if (!ReadAll(pkgPath, blob) || blob.size() < 12) return 0;
    static constexpr char kMag[] = "ANKPK2\0\0";
    std::vector<PkEntry> entries;
    bool parsed = false;
    for (size_t pos = 0; pos + 12 <= blob.size(); ++pos) {
        if (memcmp(blob.data() + pos, kMag, 8) == 0 && ParsePackage(blob.data() + pos, blob.size() - pos, entries)) {
            parsed = true;
            break;
        }
    }
    if (!parsed) return 0;
    int restored = 0;
    EnsureDir(dstDir);
    for (const auto& f : SuxRequiredFiles()) {
        const std::wstring dst = dstDir + L"\\" + f.name;
        if (FileExists(dst) && IsPe64Binary(dst) && Sha256File(dst) == f.sha256) continue;
        const std::wstring wantPath = std::wstring(L"SUX/") + f.name;
        for (const auto& e : entries) {
            if (e.path != wantPath) continue;
            std::vector<uint8_t> raw;
            if (!Unpack(e, raw)) break;
            if (Sha256Hex(raw.data(), raw.size()) != f.sha256) break;
            if (WriteAll(dst, raw.data(), raw.size()) && IsPe64Binary(dst) && Sha256File(dst) == f.sha256) ++restored;
            break;
        }
    }
    return restored;
}

inline int RepairSuxFiles(const std::wstring& dstDir,
                          const std::wstring& cacheDir,
                          const std::vector<std::wstring>& pkgCandidates,
                          std::wstring& outDetail) {
    SuxVerifyResult pre = VerifySuxDir(dstDir);
    if (pre.ok) { outDetail = pre.detail; return 0; }
    int restored = RestoreSuxFromCacheDir(cacheDir, dstDir);
    SuxVerifyResult mid = VerifySuxDir(dstDir);
    if (!mid.ok) {
        for (const auto& pkg : pkgCandidates) {
            restored += RestoreSuxFromPackageFile(pkg, dstDir);
            if (VerifySuxDir(dstDir).ok) break;
        }
    }
    const SuxVerifyResult post = VerifySuxDir(dstDir);
    outDetail = post.detail;
    return restored;
}

inline std::wstring AnkhStyleDir() { return WinDir() + L"\\Resources\\Themes\\Ankh"; }

// Installs the patch the way the pack needs it: DefaultColors removed and
enum class SuxActivationState {
    NotInstalled,
    ConfiguredPendingRestart,
    InstalledPendingRestart,
    LoadedAndActive,
    VerifyFailed
};

struct SuxFullStatus {
    bool filesVerified = false;          // ThemeDll.dll, SecureUxTheme.signed.dll, ThemeTool.exe x64 + SHA-256
    bool systemDllPresent = false;       // C:\Windows\System32\SecureUxTheme.dll exists & x64
    bool installed = false;              // kSuxStateInstalled & systemDllPresent
    bool loadedInSession = false;        // (stateFlags & (kSuxStateLoaded | kSuxStateLoadedSession)) != 0
    bool explorerHooked = false;         // Expected: false
    bool systemSettingsHooked = false;   // Expected: false
    bool logonUiHooked = false;          // Expected: true
    bool defaultColorsRemoved = false;   // Expected: true (HKLM\...\DefaultColors\Standard has 0 values)
    bool configurationMatches = false;   // installed && !explorerHooked && !systemSettingsHooked && logonUiHooked && defaultColorsRemoved
    bool restartRequired = false;        // installed && configurationMatches && !loadedInSession
    ULONG rawStateFlags = 0;
    SuxActivationState activationState = SuxActivationState::NotInstalled;
    std::wstring summary;
};

inline bool IsDefaultColorsStandardCleared() {
    HKEY k = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\DefaultColors\\Standard", 0, KEY_READ, &k);
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND) return true;
    if (rc != ERROR_SUCCESS || !k) return false;
    DWORD valueCount = 0;
    const LONG q = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    return (q == ERROR_SUCCESS && valueCount == 0);
}

inline SuxFullStatus EvaluateSuxStatusFromFacts(bool filesVerified,
                                                bool systemDllPresent,
                                                ULONG stateFlags,
                                                bool hookExplorer,
                                                bool hookSystemSettings,
                                                bool hookLogonUi,
                                                bool defaultColorsCleared) {
    SuxFullStatus st;
    st.filesVerified = filesVerified;
    st.systemDllPresent = systemDllPresent;
    st.rawStateFlags = stateFlags;
    st.installed = ((stateFlags & kSuxStateInstalled) != 0) && systemDllPresent;
    st.loadedInSession = (stateFlags & (kSuxStateLoaded | kSuxStateLoadedSession)) != 0;
    st.explorerHooked = hookExplorer || ((stateFlags & kSuxStateExplorerHooked) != 0);
    st.systemSettingsHooked = hookSystemSettings || ((stateFlags & kSuxStateSystemSettingsHooked) != 0);
    st.logonUiHooked = hookLogonUi || ((stateFlags & kSuxStateLogonUIHooked) != 0);
    st.defaultColorsRemoved = defaultColorsCleared;
    st.configurationMatches = st.installed && !st.explorerHooked && !st.systemSettingsHooked && st.logonUiHooked && st.defaultColorsRemoved;
    st.restartRequired = st.installed && st.configurationMatches && !st.loadedInSession;
    if (!st.filesVerified) st.activationState = SuxActivationState::VerifyFailed;
    else if (!st.installed) st.activationState = SuxActivationState::NotInstalled;
    else if (!st.configurationMatches) st.activationState = SuxActivationState::ConfiguredPendingRestart;
    else if (!st.loadedInSession) st.activationState = SuxActivationState::InstalledPendingRestart;
    else st.activationState = SuxActivationState::LoadedAndActive;

    st.summary = std::wstring(L"Installed: ") + (st.installed ? L"Yes" : L"No") +
                 L" | Loaded: " + (st.loadedInSession ? L"Yes (Active)" : (st.installed ? L"Pending Restart" : L"No")) +
                 L" | Explorer hooked: " + (st.explorerHooked ? L"Yes" : L"No") +
                 L" | SystemSettings hooked: " + (st.systemSettingsHooked ? L"Yes" : L"No") +
                 L" | LogonUI hooked: " + (st.logonUiHooked ? L"Yes" : L"No") +
                 L" | Remove DefaultColors: " + (st.defaultColorsRemoved ? L"Enabled" : L"Disabled");
    return st;
}

inline SuxFullStatus QuerySuxFullStatus(const std::wstring& suxDir = SuxDir()) {
    const SuxVerifyResult ver = VerifySuxDir(suxDir);
    const std::wstring sysDll = SysDir() + L"\\SecureUxTheme.dll";
    const bool sysOk = FileExists(sysDll) && IsPe64Binary(sysDll);
    Sux s;
    if (!ver.ok || !s.Load(suxDir)) {
        return EvaluateSuxStatusFromFacts(ver.ok, sysOk, 0, false, false, false, IsDefaultColorsStandardCleared());
    }
    const ULONG flags = s.state ? s.state() : 0;
    const bool hExp = s.hookTest ? (s.hookTest(L"explorer.exe") != 0) : ((flags & kSuxStateExplorerHooked) != 0);
    const bool hSys = s.hookTest ? (s.hookTest(L"SystemSettings.exe") != 0) : ((flags & kSuxStateSystemSettingsHooked) != 0);
    const bool hLog = s.hookTest ? (s.hookTest(L"LogonUI.exe") != 0) : ((flags & kSuxStateLogonUIHooked) != 0);
    return EvaluateSuxStatusFromFacts(ver.ok, sysOk, flags, hExp, hSys, hLog, IsDefaultColorsStandardCleared());
}

// Fixes .msstyles signatures in a directory recursively (BIB\, BIB3\, etc.).
inline int FixVisualStyleSignaturesInTree(Sux& s, const std::wstring& rootDir) {
    if (!DirExists(rootDir) || !s.sigCheck || !s.sigFix) return 0;
    int fixed = 0;
    for (const auto& f : ListDir(rootDir, false, L"*.msstyles")) {
        const std::wstring p = rootDir + L"\\" + f;
        if (s.sigCheck(p.c_str()) == E_FAIL && SUCCEEDED(s.sigFix(p.c_str()))) ++fixed;
    }
    for (const auto& d : ListDir(rootDir, true)) {
        fixed += FixVisualStyleSignaturesInTree(s, rootDir + L"\\" + d);
    }
    return fixed;
}

// Full SecureUxTheme installer & repair configuration:
// - Configures: LogonUI hooked = Yes, Remove DefaultColors = Enabled, Explorer hooked = No, SystemSettings hooked = No
// - Verifies System32\SecureUxTheme.dll and hook readback
// - Reports whether a Windows restart is required
inline bool ConfigureAndVerifySux(SuxFullStatus& outStatus, bool& outRestartRequired, std::wstring& outErr, const std::wstring& suxDir = SuxDir()) {
    const SuxFullStatus pre = QuerySuxFullStatus(suxDir);
    const SuxVerifyResult ver = VerifySuxDir(suxDir);
    if (!ver.ok) {
        outStatus = pre;
        outErr = ver.detail;
        return false;
    }
    Sux s;
    if (!s.Load(suxDir)) {
        outStatus = pre;
        outErr = L"ThemeDll.dll could not be loaded from " + suxDir + L".";
        return false;
    }
    const HRESULT hr = s.install(kSuxDesiredInstallFlags);
    if (FAILED(hr)) {
        wchar_t b[32]; swprintf(b, 32, L"%08X", static_cast<unsigned>(hr));
        outStatus = QuerySuxFullStatus(suxDir);
        outErr = std::wstring(L"secureuxtheme_install failed with HRESULT 0x") + b;
        return false;
    }
    // Explicitly ensure Explorer and SystemSettings are NOT hooked (matching ThemeTool reference configuration)
    if (s.hookRemove && s.hookTest) {
        if (s.hookTest(L"explorer.exe")) s.hookRemove(L"explorer.exe");
        if (s.hookTest(L"SystemSettings.exe")) s.hookRemove(L"SystemSettings.exe");
    }
    FixVisualStyleSignaturesInTree(s, DirOf(BibStylePath()));
    FixVisualStyleSignaturesInTree(s, Bib3StyleDir());

    outStatus = QuerySuxFullStatus(suxDir);
    outRestartRequired = !outStatus.loadedInSession || !pre.installed || !pre.configurationMatches;
    outStatus.restartRequired = outRestartRequired;
    if (!outStatus.installed || !outStatus.configurationMatches) {
        outErr = L"SecureUxTheme post-install verification did not match expected configuration (" + outStatus.summary + L").";
        return false;
    }
    outErr.clear();
    return true;
}

// Installs the patch the way the pack needs it: DefaultColors removed and
// LogonUI hooked, so classic windows (Run...) keep the dark theme colours.
inline bool InstallPatch(std::wstring& err) {
    SuxFullStatus st;
    bool restartReq = false;
    return ConfigureAndVerifySux(st, restartReq, err);
}

// RunOnce entry (current user) that re-applies the theme after a restart.
inline void ScheduleThemeReapply(HKEY hive, int attempt) {
    const std::wstring cmd = L"\"" + AppExe() + L"\" --apply-theme " + std::to_wstring(attempt);
    HKEY ro;
    if (RegCreateKeyExW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &ro, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(ro, kRunOnceValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.size() + 1) * 2));
        RegCloseKey(ro);
    }
}

enum class ThemeApplyOutcome {
    NotRun,
    ApplyCallFailed,
    ActivationPending,
    AppliedAndVerified,
    AppliedWithSecondaryWarning
};

struct SilentApplyDiag {
    ThemeApplyOutcome outcome = ThemeApplyOutcome::NotRun;
    bool dllLoaded = false;
    bool exportsResolved = false;
    ULONG stateFlags = 0;
    bool hookActive = false;
    int sigFixedCount = 0;
    ULONG themesEnumerated = 0;
    LONG matchedIndex = -1;
    HRESULT initHr = S_OK;
    HRESULT applyHr = E_FAIL;
    bool currentThemeVerified = false;
    bool visualStyleVerified = false;
    bool wallpaperVerified = false;
    std::wstring currentThemeValue;
    std::wstring activeVisualStyle;
    std::wstring secondaryWarning;
    std::wstring summary;
};

using SilentSetActiveFn = std::function<HRESULT(HWND, ULONG, BOOLEAN, ULONG, ULONG)>;
using SilentThemeActiveFn = std::function<bool(std::wstring& outCurrentTheme, std::wstring& outVisualStyle)>;
using SilentSleepFn = std::function<void(DWORD ms)>;

// Ensures [Control Panel\Desktop]\Wallpaper= in a .theme file points to an existing Anubis wallpaper
// while preserving every other section ([Theme], [Control Panel\Cursors], [VisualStyles], etc.).
inline bool EnsureAs3aDWallpaperInThemeFile(const std::wstring& themePath,
                                            const std::wstring& wallpaperPathForTheme,
                                            const std::wstring& existingImageOnDisk,
                                            std::wstring& outDetail) {
    if (!FileExists(themePath)) { outDetail = L"theme file not found"; return false; }
    if (existingImageOnDisk.empty() || !FileExists(existingImageOnDisk)) {
        outDetail = L"Anubis wallpaper image missing";
        return false;
    }
    std::vector<uint8_t> raw;
    if (!ReadAll(themePath, raw) || raw.empty()) { outDetail = L"could not read theme file"; return false; }

    std::string text;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        const std::wstring w(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        text = WToUtf8(w);
    } else {
        size_t off = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        text.assign(reinterpret_cast<const char*>(raw.data() + off), raw.size() - off);
    }

    const std::string wantWall = WToUtf8(wallpaperPathForTheme);
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    bool inDesktop = false, sawDesktop = false, replacedWall = false, replacedPos = false;
    std::vector<std::string> outLines;
    outLines.reserve(lines.size() + 4);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string l = lines[i];
        std::string low = l;
        for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        if (!low.empty() && low.front() == '[') {
            if (inDesktop) {
                if (!replacedWall) { outLines.push_back("Wallpaper=" + wantWall); replacedWall = true; }
                if (!replacedPos)  { outLines.push_back("PicturePosition=4"); replacedPos = true; }
            }
            inDesktop = (low.find("[control panel\\desktop]") == 0);
            if (inDesktop) sawDesktop = true;
        } else if (inDesktop) {
            if (low.rfind("wallpaper=", 0) == 0) {
                if (low.find("\\anubis\\") == std::string::npos) l = "Wallpaper=" + wantWall;
                replacedWall = true;
            } else if (low.rfind("pictureposition=", 0) == 0) {
                replacedPos = true;
            }
        }
        outLines.push_back(l);
    }
    if (inDesktop) {
        if (!replacedWall) outLines.push_back("Wallpaper=" + wantWall);
        if (!replacedPos)  outLines.push_back("PicturePosition=4");
    }
    if (!sawDesktop) {
        std::vector<std::string> withDesktop;
        bool inserted = false;
        for (const auto& l : outLines) {
            std::string low = l;
            for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (!inserted && low.rfind("[visualstyles]", 0) == 0) {
                withDesktop.push_back("[Control Panel\\Desktop]");
                withDesktop.push_back("Wallpaper=" + wantWall);
                withDesktop.push_back("Pattern=");
                withDesktop.push_back("MultimonBackgrounds=0");
                withDesktop.push_back("PicturePosition=4");
                withDesktop.push_back("");
                inserted = true;
            }
            withDesktop.push_back(l);
        }
        if (!inserted) {
            withDesktop.push_back("");
            withDesktop.push_back("[Control Panel\\Desktop]");
            withDesktop.push_back("Wallpaper=" + wantWall);
            withDesktop.push_back("Pattern=");
            withDesktop.push_back("MultimonBackgrounds=0");
            withDesktop.push_back("PicturePosition=4");
        }
        outLines.swap(withDesktop);
    }

    std::string rebuilt;
    for (size_t i = 0; i < outLines.size(); ++i) {
        rebuilt += outLines[i];
        if (i + 1 < outLines.size()) rebuilt += "\r\n";
    }
    if (rebuilt == text) { outDetail = L"wallpaper already configured"; return true; }
    if (!WriteAtomic(themePath, reinterpret_cast<const uint8_t*>(rebuilt.data()), rebuilt.size())) {
        outDetail = L"could not update theme file";
        return false;
    }
    outDetail = L"updated [Control Panel\\Desktop] Wallpaper=" + wallpaperPathForTheme;
    return true;
}

inline bool VerifySilentApplyWithHooks(ULONG themeCount,
                                       const std::function<bool(ULONG, std::wstring&, std::wstring&)>& getTheme,
                                       const SilentSetActiveFn& setActiveFn,
                                       const SilentThemeActiveFn& checkActiveFn,
                                       SilentApplyDiag& diag,
                                       std::wstring& msg,
                                       int pollAttempts = 15,
                                       DWORD pollIntervalMs = 200,
                                       const SilentSleepFn& sleepFn = nullptr) {
    constexpr ULONG kNoHourglass = 1u << 8;
    constexpr ULONG kSilentMode  = 1u << 2;
    diag.themesEnumerated = themeCount;
    // Prefer an As3aD entry that has a non-empty visual style path (skipping any stale broken entry with vs == "").
    LONG bestIdx = -1;
    for (ULONG i = 0; i < themeCount; ++i) {
        std::wstring dn, vs;
        if (!getTheme(i, dn, vs)) continue;
        if (_wcsicmp(dn.c_str(), kThemeName) != 0) continue;
        if (bestIdx < 0) bestIdx = static_cast<LONG>(i);
        if (!vs.empty()) { bestIdx = static_cast<LONG>(i); break; }
    }
    if (bestIdx >= 0) {
        const ULONG i = static_cast<ULONG>(bestIdx);
        diag.matchedIndex = bestIdx;
        diag.applyHr = setActiveFn(nullptr, i, TRUE, kNoHourglass, kSilentMode);
        if (FAILED(diag.applyHr)) {
            diag.outcome = ThemeApplyOutcome::ApplyCallFailed;
            wchar_t hb[16]; swprintf(hb, 16, L"0x%08X", static_cast<unsigned>(diag.applyHr));
            msg = std::wstring(L"Silent theme apply failed (themetool_set_active HRESULT ") + hb + L").";
            return false;
        }
        // Windows updates CurrentTheme and uxtheme's active visual style asynchronously after SetCurrentTheme.
        for (int p = 0; p < std::max(1, pollAttempts); ++p) {
            if (checkActiveFn(diag.currentThemeValue, diag.activeVisualStyle)) {
                diag.currentThemeVerified = Lower(diag.currentThemeValue).find(L"as3ad.theme") != std::wstring::npos;
                diag.visualStyleVerified = IsAs3aDThemeActiveState(diag.currentThemeValue, diag.activeVisualStyle);
                diag.outcome = ThemeApplyOutcome::AppliedAndVerified;
                msg = L"As3aD theme applied silently and verified.";
                return true;
            }
            if (p + 1 < pollAttempts) {
                if (sleepFn) sleepFn(pollIntervalMs);
                else Sleep(pollIntervalMs);
            }
        }
        diag.outcome = ThemeApplyOutcome::ActivationPending;
        msg = L"themetool_set_active succeeded, but Windows activation is still pending (CurrentTheme=\"" +
              diag.currentThemeValue + L"\", vs=\"" + diag.activeVisualStyle + L"\").";
        return false;
    }
    diag.outcome = ThemeApplyOutcome::ApplyCallFailed;
    msg = L"The As3aD theme was not found in Windows' theme list (" + std::to_wstring(themeCount) + L" themes checked).";
    return false;
}

// ---------------------------------------------------------------- 2.8.3: which Windows a theme pack is for
// BIB / BIB3 packs are Windows 11 themes; After Dark packs are Windows 10 themes. The answer comes from evidence, in this order, and says which:
//   1. the package declares it (a line "; AnkhCore-OS: Windows 11 | Windows 10 | Both" or "OS=..." under [AnkhCore] in the .theme file);
//   2. the .theme file's own header names exactly one of "Windows 11" / "Windows 10";
//   3. the known pack family (the visual-style folder or the theme name: BIB, BIB3 = Windows 11; After Dark = Windows 10);
//   otherwise Unknown: reported as unknown, never guessed.
// A filename alone is not evidence unless it is a known pack family (3). Pure: takes the text, touches no file.
enum class ThemeOs { Unknown, Windows11, Windows10, Both };
struct ThemeOsInfo { ThemeOs os = ThemeOs::Unknown; std::wstring source; };
inline const wchar_t* ThemeOsLabel(ThemeOs o) { return o == ThemeOs::Windows11 ? L"Windows 11" : o == ThemeOs::Windows10 ? L"Windows 10" : o == ThemeOs::Both ? L"Windows 10 and 11" : L"OS unknown"; }
inline bool ThemeOsFromText(const std::string& valueIn, ThemeOs& out) {
    std::string v; for (char ch : valueIn) v += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    const bool w11 = v.find("11") != std::string::npos, w10 = v.find("10") != std::string::npos;
    if (v.find("both") != std::string::npos || (w10 && w11)) { out = ThemeOs::Both; return true; }
    if (w11) { out = ThemeOs::Windows11; return true; }
    if (w10) { out = ThemeOs::Windows10; return true; }
    return false;
}
inline ThemeOsInfo ClassifyThemeOs(const std::wstring& family, const std::wstring& id, const std::wstring& displayName, const std::string& themeText) {
    ThemeOsInfo r;
    // 1. a declaration
    {
        size_t pos = 0; std::string sec;
        while (pos <= themeText.size()) {
            size_t nl = themeText.find('\n', pos);
            std::string line = nl == std::string::npos ? themeText.substr(pos) : themeText.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = line; while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
            std::string low = t; for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            ThemeOs o;
            if (low.rfind("; ankhcore-os:", 0) == 0 && ThemeOsFromText(t.substr(14), o)) { r.os = o; r.source = L"declared by the theme package"; return r; }
            if (!t.empty() && t.front() == '[' && t.back() == ']') sec = low;
            else if (sec == "[ankhcore]" && low.rfind("os=", 0) == 0 && ThemeOsFromText(t.substr(3), o)) { r.os = o; r.source = L"declared by the theme package"; return r; }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    // 2. the header comments (everything before the first section) name one Windows
    {
        std::string head; size_t pos = 0;
        while (pos < themeText.size()) {
            size_t nl = themeText.find('\n', pos);
            std::string line = nl == std::string::npos ? themeText.substr(pos) : themeText.substr(pos, nl - pos);
            std::string t = line; while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
            if (!t.empty() && t.front() == '[') break;
            head += t + "\n";
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        std::string low; for (char ch : head) low += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        const bool w11 = low.find("windows 11") != std::string::npos || low.find("win11") != std::string::npos;
        const bool w10 = low.find("windows 10") != std::string::npos || low.find("win10") != std::string::npos;
        if (w11 != w10) { r.os = w11 ? ThemeOs::Windows11 : ThemeOs::Windows10; r.source = L"named in the theme file header"; return r; }
    }
    // 3. the known pack family
    const std::wstring f = Lower(family), n = Lower(id) + L" " + Lower(displayName);
    auto has = [&](const wchar_t* k) { return f.find(k) != std::wstring::npos; };
    const bool afterDark = has(L"after dark") || has(L"afterdark") || n.rfind(L"after dark", 0) != std::wstring::npos || n.rfind(L"afterdark", 0) != std::wstring::npos;
    if (afterDark) { r.os = ThemeOs::Windows10; r.source = L"known pack family (After Dark = Windows 10)"; return r; }
    if (f == L"bib" || f == L"bib3" || n.rfind(L"bib ", 0) == 0 || n.rfind(L"bib3", 0) == 0) { r.os = ThemeOs::Windows11; r.source = L"known pack family (BIB = Windows 11)"; return r; }
    r.source = L"no OS requirement is declared in this theme";
    return r;
}
inline bool ThemeFitsHostOs(ThemeOs os, unsigned hostBuild) { return os == ThemeOs::Both || os == ThemeOs::Unknown || (os == ThemeOs::Windows11) == (hostBuild >= 22000); }
struct ThemeApplyCheck { bool allowed = false, uncertain = false; std::wstring reason; };
// hostBuild: this PC's Windows build; mechanismWin10: whether AnkhCore's theme applier works on Windows 10 (the SecureUxTheme setup ships for Windows 11 only, so this is false today).
inline ThemeApplyCheck ThemeApplyCheckFor(ThemeOs os, unsigned hostBuild, bool mechanismWin10) {
    ThemeApplyCheck r; const bool host11 = hostBuild >= 22000;
    if (os == ThemeOs::Windows11 && !host11) { r.reason = L"This theme is for Windows 11; this PC runs Windows 10."; return r; }
    if (os == ThemeOs::Windows10 && host11) { r.reason = L"This theme is for Windows 10; this PC runs Windows 11."; return r; }
    if (!host11 && !mechanismWin10) { r.reason = L"AnkhCore applies themes through SecureUxTheme, which this version sets up for Windows 11 only."; return r; }
    r.allowed = true; r.uncertain = os == ThemeOs::Unknown;
    if (r.uncertain) r.reason = L"Compatibility unknown: the theme does not say which Windows it is for.";
    return r;
}

// ---------------------------------------------------------------- Authoritative Theme Discovery & Asset Synchronization
struct DiscoveredWinTheme {
    std::wstring id;             // filename stem, e.g. "BIB Night - Arc"
    std::wstring displayName;    // from [Theme]\DisplayName=
    std::wstring family;         // subfolder name, e.g. "BIB" or "BIB3"
    std::wstring themeFilePath;  // full path to .theme file
    std::wstring vsRelPath;      // relative to Themes root, e.g. "BIB\\BIB Night - Arc.msstyles"
    std::wstring vsFullPath;     // resolved full path to .msstyles
    std::wstring wallpaperPath;  // resolved full path to wallpaper image
    ThemeOs os = ThemeOs::Unknown;   // 2.8.3: the Windows this theme is for, and the evidence
    std::wstring osSource;
    bool darkMode = false;
    bool vsValid = false;
    bool wallpaperValid = false;
    bool valid = false;
};

inline std::wstring ResolveThemeRelativePath(const std::wstring& rawVal, const std::wstring& themesRoot) {
    std::wstring s = rawVal;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'"')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n' || s.back() == L'"')) s.pop_back();
    const std::wstring low = Lower(s);
    const wchar_t* prefixes[] = {
        L"%resourcedir%\\themes\\",
        L"%systemroot%\\resources\\themes\\",
    };
    for (const wchar_t* pre : prefixes) {
        const size_t n = wcslen(pre);
        if (low.rfind(pre, 0) == 0) {
            return s.substr(n);
        }
    }
    const std::wstring exp = Expand(s);
    const std::wstring winTh = Lower(WinDir() + L"\\resources\\themes\\");
    if (Lower(exp).rfind(winTh, 0) == 0) {
        return exp.substr(winTh.size());
    }
    return {};
}

inline bool ParseDiscoveredThemeFile(const std::wstring& themePath, const std::wstring& themesRoot, DiscoveredWinTheme& out) {
    out = DiscoveredWinTheme{};
    if (!FileExists(themePath)) return false;
    std::vector<uint8_t> raw;
    if (!ReadAll(themePath, raw) || raw.empty()) return false;
    std::string text;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        const std::wstring w(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        text = WToUtf8(w);
    } else {
        size_t off = (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
        text.assign(reinterpret_cast<const char*>(raw.data() + off), raw.size() - off);
    }
    std::wstring stem = NameOf(themePath);
    if (EndsWithI(stem, L".theme")) stem.resize(stem.size() - 6);
    out.id = stem;
    out.displayName = stem;
    out.themeFilePath = themePath;

    std::string curSec;
    std::wstring rawVs, rawWall, sysMode, appMode;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = 0; while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
        line.erase(0, a);
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            curSec = line.substr(1, line.size() - 2);
            for (char& ch : curSec) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
        } else if (!line.empty() && line.front() != ';') {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                for (char& ch : k) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                if (curSec == "theme" && k == "displayname" && !v.empty() && v[0] != '@') out.displayName = Utf8ToW(v);
                else if (curSec == "visualstyles" && k == "path") rawVs = Utf8ToW(v);
                else if (curSec == "visualstyles" && k == "systemmode") sysMode = Utf8ToW(v);
                else if (curSec == "visualstyles" && k == "appmode") appMode = Utf8ToW(v);
                else if (curSec == "control panel\\desktop" && k == "wallpaper") rawWall = Utf8ToW(v);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    out.vsRelPath = ResolveThemeRelativePath(rawVs, themesRoot);
    if (!out.vsRelPath.empty()) {
        const size_t sl = out.vsRelPath.find(L'\\');
        if (sl != std::wstring::npos) out.family = out.vsRelPath.substr(0, sl);
        out.vsFullPath = themesRoot + L"\\" + out.vsRelPath;
        if (!FileExists(out.vsFullPath)) {
            const std::wstring winCandidate = WinDir() + L"\\Resources\\Themes\\" + out.vsRelPath;
            if (FileExists(winCandidate)) out.vsFullPath = winCandidate;
        }
    }
    out.vsValid = !out.vsFullPath.empty() && FileExists(out.vsFullPath);

    const std::wstring wallRel = ResolveThemeRelativePath(rawWall, themesRoot);
    if (!wallRel.empty()) {
        std::wstring cand = themesRoot + L"\\" + wallRel;
        if (!FileExists(cand) && EndsWithI(cand, L".jpg")) {
            const std::wstring pngCand = cand.substr(0, cand.size() - 4) + L".png";
            if (FileExists(pngCand)) cand = pngCand;
        }
        if (!FileExists(cand)) {
            std::wstring winCand = WinDir() + L"\\Resources\\Themes\\" + wallRel;
            if (!FileExists(winCand) && EndsWithI(winCand, L".jpg")) {
                const std::wstring pngWin = winCand.substr(0, winCand.size() - 4) + L".png";
                if (FileExists(pngWin)) winCand = pngWin;
            }
            if (FileExists(winCand)) cand = winCand;
        }
        out.wallpaperPath = cand;
    } else if (!rawWall.empty()) {
        out.wallpaperPath = Expand(rawWall);
    }
    out.wallpaperValid = !out.wallpaperPath.empty() && FileExists(out.wallpaperPath);
    out.darkMode = (_wcsicmp(sysMode.c_str(), L"Dark") == 0 || _wcsicmp(appMode.c_str(), L"Dark") == 0 ||
                    Lower(out.displayName).find(L"night") != std::wstring::npos || Lower(out.id).find(L"dm ") != std::wstring::npos);
    out.valid = FileExists(out.themeFilePath) && out.vsValid;
    { const ThemeOsInfo oi = ClassifyThemeOs(out.family, out.id, out.displayName, text); out.os = oi.os; out.osSource = oi.source; }
    return out.valid;
}

// Discovers all valid Windows themes from the authoritative theme source (`D:\Ankhcore\windows\Resources\Themes`),
// excluding the primary `As3aD` theme and built-in Aero defaults, with deduplication.
inline std::vector<DiscoveredWinTheme> DiscoverAuthoritativeThemes(const std::wstring& sourceRoot = AuthoritativeThemesSourceDir()) {
    std::vector<DiscoveredWinTheme> out;
    if (!DirExists(sourceRoot)) return out;
    std::set<std::wstring> seenKeys;
    for (const auto& f : ListDir(sourceRoot, false, L"*.theme")) {
        const std::wstring lowF = Lower(f);
        if (lowF.rfind(L"as3ad", 0) == 0) continue;   // Primary theme is presented separately
        if (lowF == L"aero.theme" || lowF == L"dark.theme" || lowF == L"spotlight.theme" ||
            lowF == L"themea.theme" || lowF == L"themeb.theme" || lowF == L"themec.theme" || lowF == L"themed.theme") continue;
        DiscoveredWinTheme th;
        if (!ParseDiscoveredThemeFile(sourceRoot + L"\\" + f, sourceRoot, th)) continue;
        if (Lower(th.family) == L"aero" || Lower(th.family) == L"ankh") continue;   // retire obsolete Ankh duplicate
        const std::wstring dedupKey = Lower(th.displayName) + L"|" + Lower(th.vsRelPath);
        if (!seenKeys.insert(dedupKey).second) continue;
        out.push_back(std::move(th));
    }
    std::sort(out.begin(), out.end(), [](const DiscoveredWinTheme& a, const DiscoveredWinTheme& b) {
        if (a.family != b.family) return Lower(a.family) < Lower(b.family);
        if (a.darkMode != b.darkMode) return a.darkMode > b.darkMode;   // Night/Dark variants first within family
        return Lower(a.displayName) < Lower(b.displayName);
    });
    return out;
}

inline int CopyDirectoryTreeOverwrite(const std::wstring& from, const std::wstring& to) {
    if (!DirExists(from)) return 0;
    int copied = 0;
    EnsureDir(to);
    for (const auto& d : ListDir(from, true)) {
        copied += CopyDirectoryTreeOverwrite(from + L"\\" + d, to + L"\\" + d);
    }
    for (const auto& f : ListDir(from, false)) {
        const std::wstring src = from + L"\\" + f;
        const std::wstring dst = to + L"\\" + f;
        if (!FileExists(dst) || Sha256File(src) != Sha256File(dst)) {
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++copied;
        }
    }
    return copied;
}

// Synchronizes Anubis wallpapers from `D:\Ankhcore\windows\Web\Wallpaper\Anubis` into `C:\Windows\Web\Wallpaper\Anubis`,
// removing any stale/old wallpaper files so the exact wallpapers from the authoritative source are applied.
inline int SyncAnubisWallpapersClean() {
    if (kTestBuild) return 0;
    const std::wstring srcDir = AuthoritativeWallpapersSourceDir();
    const std::wstring dstDir = WallDir();
    if (!DirExists(srcDir) || Lower(srcDir) == Lower(dstDir)) return 0;
    EnsureDir(dstDir);
    const std::vector<std::wstring> srcFiles = ListDir(srcDir, false);
    if (srcFiles.empty()) return 0;
    std::set<std::wstring> validNames;
    for (const auto& f : srcFiles) validNames.insert(Lower(f));

    // Remove any old/stale files in C:\Windows\Web\Wallpaper\Anubis that are not in the authoritative source
    for (const auto& existing : ListDir(dstDir, false)) {
        if (validNames.find(Lower(existing)) == validNames.end()) {
            const std::wstring stale = dstDir + L"\\" + existing;
            SetFileAttributesW(stale.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(stale.c_str());
        }
    }
    // Also remove legacy C:\Windows\Resources\Themes\As3aD\DesktopBackground so old neon wallpapers cannot override Anubis
    const std::wstring legacyAs3aDWall = WinDir() + L"\\Resources\\Themes\\As3aD";
    if (DirExists(legacyAs3aDWall)) Evict(legacyAs3aDWall, true, false);

    int synced = 0;
    for (const auto& f : srcFiles) {
        const std::wstring s = srcDir + L"\\" + f;
        const std::wstring d = dstDir + L"\\" + f;
        if (!FileExists(d) || Sha256File(s) != Sha256File(d)) {
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (CopyFileW(s.c_str(), d.c_str(), FALSE)) ++synced;
        }
    }
    return synced;
}

// Synchronizes cursor packs from `D:\Ankhcore\windows\Cursors` into `C:\Windows\Cursors`
// (ensuring both `Ankhcore Prismatic` and `DIM v3.2 - Prismatic [PREMIUM]` exist and have all 17 roles).
inline int SyncAuthoritativeCursors() {
    if (kTestBuild) return 0;
    const std::wstring srcRoot = AuthoritativeCursorsSourceDir();
    const std::wstring dstRoot = WinDir() + L"\\Cursors";
    int synced = 0;
    if (DirExists(srcRoot) && Lower(srcRoot) != Lower(dstRoot)) {
        for (const auto& d : ListDir(srcRoot, true)) {
            synced += CopyDirectoryTreeOverwrite(srcRoot + L"\\" + d, dstRoot + L"\\" + d);
        }
        // 2.11 (owner): the cursor page lists exactly the packs the package carries; their names are recorded here
        std::string list; for (const auto& d : ListDir(srcRoot, true)) list += WToUtf8(d) + "\n";
        EnsureDir(DataDir()); WriteAll(DataDir() + L"\\cursor-packs.txt", reinterpret_cast<const uint8_t*>(list.data()), list.size());
    }
    if (DirExists(CursorDir()) && !DirExists(CursorLegacyDir())) {
        synced += CopyDirectoryTreeOverwrite(CursorDir(), CursorLegacyDir());
    } else if (DirExists(CursorLegacyDir()) && !DirExists(CursorDir())) {
        synced += CopyDirectoryTreeOverwrite(CursorLegacyDir(), CursorDir());
    }
    return synced;
}

// Synchronizes the authoritative Windows themes (`D:\Ankhcore\windows\Resources\Themes`) into `C:\Windows\Resources\Themes`
// and retires obsolete duplicate `Ankh` theme entries.
// 2.12: Windows 10 only needs the one visual style of As3aD10.theme (After Dark Glass Cyan), not the Windows 11 BIB collection. Returns the files copied.
inline int SyncGlassCyanStyle() {
    if (kTestBuild) return 0;
    const std::wstring src = AuthoritativeThemesSourceDir() + L"\\After Dark Glass Cyan";
    if (!DirExists(src) || Lower(src) == Lower(GlassCyanStyleDir())) return 0;
    return CopyDirectoryTreeOverwrite(src, GlassCyanStyleDir());
}
inline int SyncAuthoritativeThemesCollection() {
    if (kTestBuild) return 0;
    const std::wstring srcRoot = AuthoritativeThemesSourceDir();
    const std::wstring winThemes = WinDir() + L"\\Resources\\Themes";
    int synced = 0;
    if (DirExists(srcRoot) && Lower(srcRoot) != Lower(winThemes)) {
        for (const auto& d : ListDir(srcRoot, true)) {
            synced += CopyDirectoryTreeOverwrite(srcRoot + L"\\" + d, winThemes + L"\\" + d);
        }
        for (const auto& f : ListDir(srcRoot, false, L"*.theme")) {
            const std::wstring s = srcRoot + L"\\" + f;
            const std::wstring d = winThemes + L"\\" + f;
            if (!FileExists(d) || Sha256File(s) != Sha256File(d)) {
                if (CopyFileW(s.c_str(), d.c_str(), FALSE)) ++synced;
            }
        }
    }
    if (DirExists(srcRoot) && Lower(srcRoot) != Lower(winThemes)) {   // 2.12: what was put into Windows' themes folder, so a full-clean uninstall can take it out again
        std::string list; for (const auto& d : ListDir(srcRoot, true)) list += "D|" + WToUtf8(d) + "\n";
        for (const auto& f : ListDir(srcRoot, false, L"*.theme")) list += "F|" + WToUtf8(f) + "\n";
        EnsureDir(DataDir()); WriteAll(DataDir() + L"\\windows-assets.txt", reinterpret_cast<const uint8_t*>(list.data()), list.size());
    }
    // Retire obsolete duplicate C:\Windows\Resources\Themes\Ankh folder and stale user As3aD - *.theme files referencing it
    const std::wstring oldAnkhDir = winThemes + L"\\Ankh";
    if (DirExists(oldAnkhDir)) Evict(oldAnkhDir, true, false);
    const std::wstring userThemesDir = DirOf(ThemeUserPath());
    if (DirExists(userThemesDir)) {
        for (const auto& f : ListDir(userThemesDir, false, L"*.theme")) {
            const std::wstring p = userThemesDir + L"\\" + f;
            const std::string txt = ReadText(p);
            std::string low = txt;
            for (char& ch : low) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (low.find("\\themes\\ankh\\") != std::string::npos) {
                DeleteFileW(p.c_str());
            }
        }
    }
    return synced;
}

// Resolves and verifies the application-local `Themetools\As3aD.theme` file (never searching Windows Themes folders).
inline bool VerifyAppLocalAs3aDTheme(std::wstring& outLocalPath, std::wstring& outDetail) {
    const bool w11 = GetWinVer().win11();   // 2.12: As3aD.theme on Windows 11, As3aD10.theme on Windows 10
    const std::wstring file = AuthoritativeThemeFile(w11); const std::string wantSha = AuthoritativeThemeSha(w11);
    outLocalPath = ThemetoolsDir() + L"\\" + file;
    const std::wstring devPath = L"D:\\Ankhcore\\" + file;
    if (!FileExists(outLocalPath)) {
        if (!kTestBuild && FileExists(devPath) && Sha256File(devPath) == wantSha) {
            EnsureDir(ThemetoolsDir());
            CopyFileW(devPath.c_str(), outLocalPath.c_str(), FALSE);
        }
    }
    if (!FileExists(outLocalPath)) {
        outDetail = L"Application-local " + file + L" not found at " + outLocalPath + L".";
        return false;
    }
    if (Sha256File(outLocalPath) != wantSha) {
        if (!kTestBuild && FileExists(devPath) && Sha256File(devPath) == wantSha) {
            CopyFileW(devPath.c_str(), outLocalPath.c_str(), FALSE);
        }
    }
    if (Sha256File(outLocalPath) != wantSha) {
        outDetail = L"Application-local " + file + L" SHA-256 mismatch at " + outLocalPath + L".";
        return false;
    }
    outDetail = L"Verified application-local " + file + L" (" + outLocalPath + L", SHA-256 OK).";
    return true;
}

// Applies As3aD silently using the original 2.2 ThemeDll.dll in-process mechanism
// (`themetool_set_active(nullptr, i, TRUE, 1u << 8, 1u << 2)`), always sourcing from the verified
// application-local `Themetools\As3aD.theme`.
inline bool ApplyThemeSilent(bool requirePatch, std::wstring& msg, SilentApplyDiag* outDiag = nullptr) {
    SilentApplyDiag localDiag;
    SilentApplyDiag& d = outDiag ? *outDiag : localDiag;
    const bool w11 = GetWinVer().win11();   // 2.12: Windows 10 has its own As3aD10.theme (After Dark Glass Cyan)

    const SuxVerifyResult ver = VerifySuxDir(SuxDir());
    if (!ver.ok) { msg = ver.detail + L" Use Check & repair."; return false; }

    Sux s;
    d.exportsResolved = s.Load(SuxDir());
    d.dllLoaded = (s.m != nullptr);
    if (!d.dllLoaded) { msg = L"ThemeDll.dll could not be loaded from " + SuxDir() + L". Use Check & repair."; return false; }
    if (!d.exportsResolved) { msg = L"ThemeDll.dll is missing required exports. Use Check & repair."; return false; }

    d.stateFlags = s.state ? s.state() : 0;
    d.hookActive = s.Loaded();
    if (requirePatch && !d.hookActive) {
        ScheduleThemeReapply(HKEY_CURRENT_USER, 1);
        wchar_t hexBuf[16]; swprintf(hexBuf, 16, L"0x%lX", d.stateFlags);
        msg = std::wstring(L"SecureUxTheme is installed (state ") + hexBuf + L") but not active in this Windows session yet; pending apply scheduled for next sign-in. Restart Windows to activate.";
        return false;
    }

    // Step 1: Synchronize authoritative Anubis wallpapers, cursors, and BIB/BIB3 visual styles.
    if (!kTestBuild) {
        SyncAnubisWallpapersClean();
        SyncAuthoritativeCursors();
        if (w11) SyncAuthoritativeThemesCollection(); else SyncGlassCyanStyle();
    }

    // Step 2: Verify the application-local `Themetools\As3aD.theme` (never searching Windows Themes for another file).
    std::wstring localThemePath, localThemeDetail;
    if (!VerifyAppLocalAs3aDTheme(localThemePath, localThemeDetail)) {
        msg = localThemeDetail + L" Use Check & repair.";
        return false;
    }
    if (!FileExists(ThemeStylePathFor(w11))) {
        msg = L"Referenced visual style missing: " + ThemeStylePathFor(w11) + L". Use Check & repair.";
        return false;
    }
    if (!DirExists(CursorDir())) {
        msg = L"Referenced cursor folder missing: " + CursorDir() + L". Use Check & repair.";
        return false;
    }
    if (!FileExists(WallDir() + L"\\Anubis  (1).jpg")) {
        msg = L"Referenced Anubis wallpaper missing: " + WallDir() + L"\\Anubis  (1).jpg. Use Check & repair.";
        return false;
    }

    // Step 3: Stage exact byte-for-byte copy of verified `Themetools\As3aD.theme` for IThemeManager2 enumeration
    // (overwriting any older system-level or user-level copy without modifying the theme content).
    if (!kTestBuild) {
        for (const auto& stagePath : {ThemeWinPath(), ThemeUserPath()}) {
            EnsureDir(DirOf(stagePath));
            SetFileAttributesW(stagePath.c_str(), FILE_ATTRIBUTE_NORMAL);
            CopyFileW(localThemePath.c_str(), stagePath.c_str(), FALSE);
        }
    }

    // Step 4: Fix .msstyles signatures in BIB and BIB3.
    if (w11) {
        d.sigFixedCount += FixVisualStyleSignaturesInTree(s, DirOf(BibStylePath()));
        d.sigFixedCount += FixVisualStyleSignaturesInTree(s, Bib3StyleDir());
    } else d.sigFixedCount += FixVisualStyleSignaturesInTree(s, GlassCyanStyleDir());

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    d.initHr = s.InitManager();
    if (FAILED(d.initHr)) {
        d.outcome = ThemeApplyOutcome::ApplyCallFailed;
        wchar_t hb[16]; swprintf(hb, 16, L"0x%08X", static_cast<unsigned>(d.initHr));
        msg = std::wstring(L"themetool_init failed (HRESULT ") + hb + L").";
        return false;
    }

    bool applied = false;
    for (int tries = 0; tries < 4 && !applied; ++tries) {
        ULONG n = 0; s.count(&n);
        auto getFn = [&](ULONG idx, std::wstring& dn, std::wstring& vs) -> bool {
            void* th = nullptr;
            if (FAILED(s.get(idx, &th)) || !th) return false;
            wchar_t dnBuf[260] = {}, vsBuf[MAX_PATH] = {};
            s.name(th, dnBuf, 260);
            if (s.vspath) s.vspath(th, vsBuf, MAX_PATH);
            s.release(th);
            dn = dnBuf; vs = vsBuf;
            return true;
        };
        auto setFn = [&](HWND hwnd, ULONG idx, BOOLEAN b, ULONG f1, ULONG f2) -> HRESULT {
            return s.setActive(hwnd, idx, b, f1, f2);
        };
        auto chkFn = [](std::wstring& curOut, std::wstring& vsOut) -> bool {
            curOut = RegStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes", L"CurrentTheme");
            vsOut = GetActiveVisualStylePath();
            return IsAs3aDThemeActiveState(curOut, vsOut);
        };
        applied = VerifySilentApplyWithHooks(n, getFn, setFn, chkFn, d, msg);
        if (!applied && d.matchedIndex >= 0) break;
        if (!applied && tries < 3) Sleep(800);
    }

    // Step 5: Ensure Anubis wallpaper & Prismatic cursors from As3aD.theme are active in the current session.
    if (applied && !kTestBuild) {
        const std::wstring first = WallDir() + L"\\Anubis  (1).jpg";
        if (FileExists(first)) {
            if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(first.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
                d.outcome = ThemeApplyOutcome::AppliedWithSecondaryWarning;
                d.secondaryWarning = L"Wallpaper update reported error " + std::to_wstring(GetLastError());
                msg = L"As3aD theme applied and verified, with wallpaper warning: " + d.secondaryWarning;
                LogW(msg);
            } else {
                d.wallpaperVerified = true;
            }
        }
    }

    wchar_t stHex[16], hrHex[16];
    swprintf(stHex, 16, L"0x%lX", d.stateFlags);
    swprintf(hrHex, 16, L"0x%08X", static_cast<unsigned>(d.applyHr));
    d.summary = L"ThemeDll=" + std::wstring(d.dllLoaded ? L"ok" : L"fail") +
                L", exports=" + (d.exportsResolved ? L"ok" : L"fail") +
                L", hook=" + (d.hookActive ? L"active" : L"inactive") + L"(" + stHex + L")" +
                L", localTheme=" + localThemePath +
                L", sigFixed=" + std::to_wstring(d.sigFixedCount) +
                L", enum=" + std::to_wstring(d.themesEnumerated) +
                L", idx=" + std::to_wstring(d.matchedIndex) +
                L", applyHr=" + hrHex +
                L", currentThemeOk=" + (d.currentThemeVerified ? L"true" : L"false") +
                L", vsOk=" + (d.visualStyleVerified ? L"true" : L"false") +
                L", wallOk=" + (d.wallpaperVerified ? L"true" : L"false");
    LogI(L"Theme silent apply: " + d.summary);
    return applied;
}

inline bool ApplyTheme(bool requirePatch, std::wstring& msg) {
    return ApplyThemeSilent(requirePatch, msg, nullptr);
}

// Applies any discovered theme from the authoritative Windows Themes collection silently via ThemeDll.dll.
// 2.8.4: the theme collection of THIS Windows: Windows 11 reads the package's BIB / BIB3 folder; Windows 10 reads Windows' own themes folder, where Setup put the
// After Dark pack made for this release (only themes classified Windows 10 are listed there).
inline std::wstring ThemeCollectionRoot() { return GetWinVer().win11() ? AuthoritativeThemesSourceDir() : WinDir() + L"\\Resources\\Themes"; }
constexpr bool kThemeApplierWin10 = true;   // the SecureUxTheme patch is configured by Setup for Windows 10 from 1809 on; the apply below checks it is loaded
inline bool ApplyDiscoveredWindowsTheme(const DiscoveredWinTheme& th, std::wstring& msg) {
    const bool w11 = GetWinVer().win11();
    if (!w11 && th.os != ThemeOs::Windows10) { msg = L"This theme is for Windows 11 or unknown; this PC runs Windows 10."; return false; }
    { const ThemeApplyCheck chk = ThemeApplyCheckFor(th.os, GetWinVer().build, kThemeApplierWin10); if (!chk.allowed) { msg = L"Theme '" + th.displayName + L"' was not applied: " + chk.reason; return false; } }   // 2.8.3
    if (!th.valid) { msg = L"Theme '" + th.displayName + L"' is missing its .theme or .msstyles resource."; return false; }
    const SuxVerifyResult ver = VerifySuxDir(SuxDir());
    if (!ver.ok) { msg = ver.detail + L" Use Check & repair."; return false; }
    Sux s;
    if (!s.Load(SuxDir())) { msg = L"ThemeDll.dll could not be loaded from " + SuxDir() + L"."; return false; }
    if (!s.Loaded()) {
        msg = L"SecureUxTheme is installed but not active in this session yet. Restart Windows to activate.";
        return false;
    }
    if (w11) {
        if (!kTestBuild) SyncAuthoritativeThemesCollection();
        FixVisualStyleSignaturesInTree(s, DirOf(BibStylePath()));
        FixVisualStyleSignaturesInTree(s, Bib3StyleDir());
    } else FixVisualStyleSignaturesInTree(s, DirOf(th.vsFullPath));   // Windows 10: the pack was copied into Windows' themes folder by Setup

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring wantName = Lower(th.displayName);
    const std::wstring wantId = Lower(th.id);
    const std::wstring wantVs = Lower(NameOf(th.vsRelPath));
    const bool ok = s.Apply([&](const std::wstring& dn, const std::wstring& vs) {
        const std::wstring ldn = Lower(dn);
        const std::wstring lvs = Lower(NameOf(vs));
        if (ldn == wantName || ldn == wantId) {
            return wantVs.empty() || lvs.empty() || lvs == wantVs;
        }
        return false;
    });
    if (!ok) {
        msg = L"Could not apply theme '" + th.displayName + L"' via SecureUxTheme.";
        return false;
    }
    if (!kTestBuild && th.wallpaperValid && FileExists(th.wallpaperPath)) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(th.wallpaperPath.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    msg = L"Applied theme '" + th.displayName + L"' (" + th.family + L").";
    return true;
}

// Back to Windows' own theme (dark on Windows 11).
inline bool RestoreWindowsTheme(std::wstring& msg) {
    const bool w11 = GetWinVer().win11();
    Sux s;
    if (s.Load(SuxDir())) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::wstring want = Lower(WinDir() + L"\\resources\\themes\\aero\\aero.msstyles");
        bool ok = w11 && s.Apply([&](const std::wstring& n, const std::wstring& vs) {
            return Lower(Expand(vs)) == want && Lower(n).find(L"dark") != std::wstring::npos;
        });
        if (!ok) ok = s.Apply([&](const std::wstring&, const std::wstring& vs) { return Lower(Expand(vs)) == want; });
        if (ok) { msg = L"Windows theme restored."; return true; }
    }
    const std::wstring f = WinDir() + (w11 ? L"\\Resources\\Themes\\dark.theme" : L"\\Resources\\Themes\\aero.theme");
    ShellExecuteW(nullptr, L"open", f.c_str(), nullptr, nullptr, SW_SHOWMINNOACTIVE);
    msg = L"Windows theme restored.";
    return true;
}

struct ThemeCheck {
    std::vector<std::pair<std::wstring, bool>> items;
    bool allOk = true;
    std::wstring patchDetail;
    SuxFullStatus suxStatus;
    int discoveredThemesCount = 0;
};

inline ThemeCheck CheckTheme() {
    ThemeCheck c;
    auto add = [&](const std::wstring& n, bool ok) { c.items.emplace_back(n, ok); c.allOk &= ok; };
    add(L"Windows 11", GetWinVer().win11());
    std::wstring localPath, localDet;
    const bool localOk = VerifyAppLocalAs3aDTheme(localPath, localDet);
    add(L"As3aD.theme (Themetools\\ application-local)", localOk);
    add(L"BIB Night visual style", FileExists(BibStylePath()));
    const auto disc = DiscoverAuthoritativeThemes();
    c.discoveredThemesCount = static_cast<int>(disc.size());
    add(L"Windows Themes collection (" + std::to_wstring(c.discoveredThemesCount) + L" themes)", !disc.empty());
    add(L"Ankhcore Prismatic cursors", DirExists(CursorDir()));
    add(L"Anubis wallpapers", DirExists(WallDir()) && FileExists(WallDir() + L"\\Anubis  (1).jpg"));
    c.suxStatus = QuerySuxFullStatus(SuxDir());
    add(L"Theme patch files", c.suxStatus.filesVerified);
    add(L"Theme patch configured (LogonUI + DefaultColors)", c.suxStatus.configurationMatches);
    add(L"Theme patch active in session", c.suxStatus.loadedInSession);
    c.patchDetail = c.suxStatus.summary;
    return c;
}

inline bool RepairTheme(std::wstring& msg) {
    int restored = 0;
    if (!kTestBuild) {
        restored += SyncAnubisWallpapersClean();
        restored += SyncAuthoritativeCursors();
        restored += SyncAuthoritativeThemesCollection();
    }
    std::wstring localPath, localDet;
    if (VerifyAppLocalAs3aDTheme(localPath, localDet) && !kTestBuild) {
        for (const auto& t : {ThemeWinPath(), ThemeUserPath()}) {
            EnsureDir(DirOf(t));
            SetFileAttributesW(t.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (CopyFileW(localPath.c_str(), t.c_str(), FALSE)) ++restored;
        }
    }
    std::wstring suxDetail;
    const std::vector<std::wstring> pkgs = {
        AppDir() + L"\\AnkhCore_Installer.exe",
        ProdDataDir() + L"\\AnkhCore_Installer.exe",
        AppDir() + L"\\AnkhCore_2.5_Setup.exe",
        ProdDataDir() + L"\\AnkhCore_2.5_Setup.exe",
        AppDir() + L"\\AnkhCore_2.2_Setup.exe",
    };
    restored += RepairSuxFiles(ThemetoolsDir(), ProdDataDir() + L"\\Cache\\SecureUxTheme", pkgs, suxDetail);
    restored += RepairSuxFiles(AppDir() + L"\\SecureUxTheme", ProdDataDir() + L"\\Cache\\SecureUxTheme", pkgs, suxDetail);
    std::wstring err;
    SuxFullStatus suxSt;
    bool restartReq = false;
    const bool patched = ConfigureAndVerifySux(suxSt, restartReq, err);
    msg = L"Synchronized/restored " + std::to_wstring(restored) + L" theme resource(s). " +
          (patched ? suxSt.summary : err);
    LogI(L"Theme repair: " + msg + (suxDetail.empty() ? L"" : L" [" + suxDetail + L"]"));
    return patched;
}

// Logon helper: waits for the desktop, applies when the patch is loaded,
// otherwise tries again at the next sign-in (3 times).
inline int ThemeLogonHelper(int attempt) {
    for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i) Sleep(500);
    Sleep(2500);
    std::wstring msg;
    if (ApplyThemeSilent(true, msg)) { LogOk(L"Theme applied at sign-in."); return 0; }
    if (attempt < 3) { ScheduleThemeReapply(HKEY_CURRENT_USER, attempt + 1); LogW(L"Theme not applied yet (" + msg + L"); will retry at next sign-in."); }
    return 1;
}

// ---------------------------------------------------------------- cursors
struct CursorRole { const wchar_t* key; const wchar_t* prismatic; const wchar_t* aero; };
inline const std::vector<CursorRole>& CursorRoles() {
    static const std::vector<CursorRole> r = {
        {L"AppStarting", L"[PREMIUM] [PR] Working In Background v3.2.ani", L"aero_working.ani"},
        {L"Arrow", L"Bonus\\[PREMIUM] [PR] Link Select v3.2 - Glow.ani", L"aero_arrow.cur"},
        {L"Crosshair", L"[PREMIUM] [PR] Precision Select v3.2.ani", L""},
        {L"Hand", L"[PREMIUM] [PR] Link Select v3.2.ani", L"aero_link.cur"},
        {L"Help", L"[PREMIUM] [PR] Help Select v3.2.ani", L"aero_helpsel.cur"},
        {L"IBeam", L"[PREMIUM] [PR] Text Select v3.2.ani", L""},
        {L"No", L"[PREMIUM] [PR] Unavailable v3.2.ani", L"aero_unavail.cur"},
        {L"NWPen", L"[PREMIUM] [PR] Handwriting v3.2.ani", L"aero_pen.cur"},
        {L"SizeAll", L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Move v3.2.cur", L"aero_move.cur"},
        {L"SizeNESW", L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Diagonal Resize 2 v3.2.cur", L"aero_nesw.cur"},
        {L"SizeNS", L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Vertical Resize v3.2.cur", L"aero_ns.cur"},
        {L"SizeNWSE", L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Diagonal Resize 1 v3.2.cur", L"aero_nwse.cur"},
        {L"SizeWE", L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Horizontal Resize v3.2.cur", L"aero_ew.cur"},
        {L"UpArrow", L"[PREMIUM] [PR] Alternate Select v3.2.ani", L"aero_up.cur"},
        {L"Wait", L"[PREMIUM] [PR] Busy v3.2.ani", L"aero_busy.ani"},
        {L"Pin", L"[PREMIUM] [PR] Location Select v3.2.ani", L"aero_pin.cur"},
        {L"Person", L"[PREMIUM] [PR] Person Select v3.2.ani", L"aero_person.cur"},
    };
    return r;
}
inline bool CursorsActive() {
    return Lower(RegStr(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"Arrow")).find(L"prismatic") != std::wstring::npos;
}
inline bool SetCursors(bool on) {
    const wchar_t* key = L"Control Panel\\Cursors";
    OpTally tally;   // SEC-3: every value written counts, and the result is read back
    if (on) {
        if (!kTestBuild) SyncAuthoritativeCursors();
        if (!DirExists(CursorDir())) { LogE(L"Prismatic cursor files are missing (" + CursorDir() + L")."); return false; }
        JournalOpScope jscope(L"Mouse cursors: Prismatic");   // FT-RH: one operation, many registry values
        for (const auto& r : CursorRoles())
            tally.Add(JRegSet(L"cursors", L"Mouse cursors: Prismatic", HKEY_CURRENT_USER, key, r.key, REG_EXPAND_SZ,
                    (std::wstring(L"%SystemRoot%\\Cursors\\Ankhcore Prismatic\\") + r.prismatic).c_str(),
                    static_cast<DWORD>((wcslen(L"%SystemRoot%\\Cursors\\Ankhcore Prismatic\\") + wcslen(r.prismatic) + 1) * 2)));
        tally.Add(JRegStr(L"cursors", L"Mouse cursors: Prismatic", HKEY_CURRENT_USER, key, L"", L"As3aD"));
    } else {
        // Undo what AnkhCore set; if nothing is recorded, use Windows' cursors.
        const UndoCount u = JournalUndoEx([](const JEntry& e) { return e.group == L"cursors"; });
        for (int i = 0; i < u.undone; ++i) tally.Add(true);
        for (int i = 0; i < u.Failed(); ++i) tally.Add(false);
        if (u.undone == 0 || CursorsActive()) {
            for (const auto& r : CursorRoles()) {
                const std::wstring v = *r.aero ? std::wstring(L"%SystemRoot%\\cursors\\") + r.aero : L"";
                tally.Add(RegSet(HKEY_CURRENT_USER, key, r.key, REG_EXPAND_SZ, v.c_str(), static_cast<DWORD>((v.size() + 1) * 2)));
            }
            tally.Add(RegSetStr(HKEY_CURRENT_USER, key, L"", L"Windows Default"));
        }
    }
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    if (!tally.Success() || CursorsActive() != on) {
        LogE(std::wstring(on ? L"Prismatic mouse cursors were not fully applied: " : L"Windows mouse cursors were not fully restored: ") + tally.Text(L"cursor setting", L"cursor settings", L"written"));
        return false;
    }
    LogOk(on ? L"Prismatic mouse cursors applied." : L"Windows mouse cursors restored.");
    return true;
}

// ---------------------------------------------------------------- wallpapers
inline const wchar_t* PrefKey() { return kPrefKey; }   // Software\AnkhCore (test build: Software\AnkhCore-Test)
// 2.5: kept in settings.json (migrated once from the 2.2 registry values)
inline DWORD WallInterval() { return static_cast<DWORD>(Cfg().Int(L"wallpaper.intervalMin", 360, 1, 10080)); }
inline bool WallShuffle() { return Cfg().Bool(L"wallpaper.shuffle", false); }
inline std::vector<std::wstring> WallImages() {
    std::vector<std::wstring> v;
    for (const auto& f : ListDir(WallDir(), false))
        if (EndsWithI(f, L".jpg") || EndsWithI(f, L".jpeg") || EndsWithI(f, L".png")) v.push_back(WallDir() + L"\\" + f);
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) {
        auto num = [](const std::wstring& s) { const size_t i = s.find_last_of(L'('); return i == std::wstring::npos ? 0 : _wtoi(s.c_str() + i + 1); };
        return num(a) < num(b);
    });
    return v;
}
struct Wallpaper {
    IDesktopWallpaper* w = nullptr;
    Wallpaper() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&w)); }
    ~Wallpaper() { if (w) w->Release(); }
    std::wstring Current() {
        if (!w) return {};
        LPWSTR p = nullptr; std::wstring r;
        if (SUCCEEDED(w->GetWallpaper(nullptr, &p)) && p) { r = p; CoTaskMemFree(p); }
        return r;
    }
};
inline bool WallpaperActive() {
    Wallpaper wp;
    return Lower(wp.Current()).find(Lower(WallDir())) != std::wstring::npos ||
           Lower(RegStr(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallPaper")).find(L"\\anubis\\") != std::wstring::npos;
}
inline void RememberWallpaper() {
    // Keep the wallpaper that was there before Anubis, once.
    for (const auto& e : JournalLoad()) if (e.group == L"wallpaper" && e.kind == L"WALL" && e.state == L"A") return;
    Wallpaper wp;
    const std::wstring cur = wp.Current();
    if (Lower(cur).find(Lower(WallDir())) != std::wstring::npos) return;
    JournalAdd(L"wallpaper", L"WALL", L"Wallpaper: Anubis", {cur});
}
inline bool SetWallSlideshow() {
    Wallpaper wp;
    if (!wp.w) { LogE(L"Wallpaper service is not available."); return false; }
    if (WallImages().empty()) { LogE(L"Anubis wallpapers are missing (" + WallDir() + L")."); return false; }
    RememberWallpaper();
    IShellItem* folder = nullptr; IShellItemArray* arr = nullptr;
    bool ok = SUCCEEDED(SHCreateItemFromParsingName(WallDir().c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
              SUCCEEDED(SHCreateShellItemArrayFromShellItem(folder, IID_PPV_ARGS(&arr))) &&
              SUCCEEDED(wp.w->SetSlideshow(arr));
    if (ok) {
        wp.w->SetSlideshowOptions(WallShuffle() ? DSO_SHUFFLEIMAGES : static_cast<DESKTOP_SLIDESHOW_OPTIONS>(0), WallInterval() * 60000);
        wp.w->SetPosition(DWPOS_FILL);
    }
    if (arr) arr->Release();
    if (folder) folder->Release();
    if (!ok) {   // fall back to the first picture
        const auto imgs = WallImages();
        ok = SUCCEEDED(wp.w->SetWallpaper(nullptr, imgs.front().c_str()));
    }
    if (ok) LogOk(L"Anubis wallpapers on (every " + std::to_wstring(WallInterval()) + L" min" + (WallShuffle() ? L", shuffled)." : L")."));
    else LogE(L"Could not set the Anubis wallpapers.");
    return ok;
}
inline bool SetWallStatic(const std::wstring& image) {
    Wallpaper wp;
    if (!wp.w) { LogE(L"Wallpaper service is not available."); return false; }
    RememberWallpaper();
    const bool ok = SUCCEEDED(wp.w->SetWallpaper(nullptr, image.c_str()));
    if (ok) { wp.w->SetPosition(DWPOS_FILL); LogOk(L"Wallpaper set: " + NameOf(image)); }
    else LogE(L"The wallpaper could not be set: " + NameOf(image));
    return ok;
}
inline bool WallpaperOff() {
    std::wstring prev;
    EnterCriticalSection(&JournalLock());
    auto all = JournalLoad();
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (it->group == L"wallpaper" && it->kind == L"WALL" && it->state == L"A") { prev = it->f.empty() ? L"" : it->f[0]; it->state = L"R"; break; }
    JournalSave(all);
    LeaveCriticalSection(&JournalLock());
    if (prev.empty() || !FileExists(prev) || Lower(prev).find(Lower(WallDir())) != std::wstring::npos)
        prev = WinDir() + L"\\Web\\Wallpaper\\Windows\\img0.jpg";
    Wallpaper wp;
    const bool ok = wp.w && SUCCEEDED(wp.w->SetWallpaper(nullptr, prev.c_str()));
    if (ok) LogOk(L"Anubis wallpapers off."); else LogE(L"The previous wallpaper could not be put back (" + NameOf(prev) + L").");   // SEC-3: no success line for a failed change
    return ok;
}
inline void RegisterThemeUndo() {
    UndoHandlers()[L"WALL"] = [](const JEntry& e) {
        Wallpaper wp;
        const std::wstring prev = e.f.empty() || !FileExists(e.f[0]) ? WinDir() + L"\\Web\\Wallpaper\\Windows\\img0.jpg" : e.f[0];
        return wp.w && SUCCEEDED(wp.w->SetWallpaper(nullptr, prev.c_str()));
    };
}

// ---------------------------------------------------------------- 2.12 full-clean uninstall (owner: "the uninstaller does a full clean and removes what AnkhCore put there")
// Takes out what AnkhCore itself added, nothing else: its theme and visual styles, its cursor packs and Anubis wallpapers in Windows' folders, the Windhawk mods of its package
// (Windhawk itself stays), and its own data and settings. The person's Backup and Recovery folders stay. Every name comes from a record the setup wrote when it installed.
struct FullCleanResult { int themes = 0, cursors = 0, wallpapers = 0, mods = 0, dataFolders = 0; std::vector<std::wstring> failed; };
inline std::vector<std::wstring> FullCleanLines(const std::string& text) {   // one name per line; blanks and CR dropped
    std::vector<std::wstring> out; std::string cur;
    for (const char ch : text + "\n") { if (ch == '\n' || ch == '\r') { if (!cur.empty()) out.push_back(Utf8ToW(cur)); cur.clear(); } else cur.push_back(ch); }
    return out;
}
// A name from a record is used only if it is a single folder or file name (no path parts), so a damaged record can never reach outside the Windows folder it names.
inline bool FullCleanSafeName(const std::wstring& n) { return !n.empty() && n.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos && n != L"." && n != L".."; }
inline FullCleanResult FullCleanWindowsAndMods() {
    FullCleanResult r;
    if (kTestBuild) return r;   // the test build never touches Windows' folders, Windhawk or HKLM
    const std::wstring themes = WinDir() + L"\\Resources\\Themes", cursors = WinDir() + L"\\Cursors";
    auto lines = [](const std::wstring& file) { return FullCleanLines(ReadText(file)); };
    for (const std::wstring& l : lines(DataDir() + L"\\windows-assets.txt")) {
        if (l.size() < 3 || l[1] != L'|' || !FullCleanSafeName(l.substr(2))) continue;
        const std::wstring p = themes + L"\\" + l.substr(2);
        if (l[0] == L'D' && DirExists(p)) { Evict(p, true, false); if (DirExists(p)) r.failed.push_back(p); else ++r.themes; }
        else if (l[0] == L'F' && FileExists(p)) { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); if (DeleteFileW(p.c_str())) ++r.themes; else r.failed.push_back(p); }
    }
    for (const std::wstring& p : {ThemeWinPath(), ThemeUserPath()}) if (FileExists(p)) { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); if (DeleteFileW(p.c_str())) ++r.themes; else r.failed.push_back(p); }
    if (DirExists(GlassCyanStyleDir())) { Evict(GlassCyanStyleDir(), true, false); if (!DirExists(GlassCyanStyleDir())) ++r.themes; }   // Windows 10 variant (also recorded on Windows 10)
    for (const std::wstring& n : lines(DataDir() + L"\\cursor-packs.txt")) {
        if (!FullCleanSafeName(n)) continue;
        const std::wstring p = cursors + L"\\" + n;
        if (DirExists(p)) { Evict(p, true, false); if (DirExists(p)) r.failed.push_back(p); else ++r.cursors; }
    }
    if (DirExists(WallDir())) { Evict(WallDir(), true, false); if (DirExists(WallDir())) r.failed.push_back(WallDir()); else ++r.wallpapers; }
    // Windhawk: only the mods of the package. Removing a mod's key makes the engine unload it; its source and libraries go after (a loaded library may stay until a restart).
    for (const std::wstring& id : lines(DataDir() + L"\\windhawk-bundle-mods.txt")) {
        if (!FullCleanSafeName(id)) continue;
        bool any = false;
        const std::wstring key = std::wstring(WhModsKey()) + L"\\" + id;
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, WhModsKey(), 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) { if (RegDeleteTreeW(k, id.c_str()) == ERROR_SUCCESS) any = true; RegCloseKey(k); }
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Engine\\ModsWritable", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) { RegDeleteTreeW(k, id.c_str()); RegCloseKey(k); }
        const std::wstring src = WindhawkData() + L"\\ModsSource\\" + id + L".wh.cpp";
        if (FileExists(src) && DeleteFileW(src.c_str())) any = true;
        for (const wchar_t* arch : {L"64", L"32"}) for (const std::wstring& f : ListDir(WindhawkData() + L"\\Engine\\Mods\\" + arch, false, (id + L"_*.dll").c_str())) {
            const std::wstring p = WindhawkData() + L"\\Engine\\Mods\\" + arch + L"\\" + f; SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        if (any) ++r.mods;
    }
    return r;
}
// AnkhCore's own data: everything but Backup and Recovery (those are the person's safety copies). The per-user settings go too. Runs last, after the final message and log line.
inline void FullCleanData(FullCleanResult& r) {
    if (kTestBuild) return;
    const std::wstring data = ProdDataDir();
    if (DirExists(data)) for (const std::wstring& d : ListDir(data, true)) {
        if (_wcsicmp(d.c_str(), L"Backup") == 0 || _wcsicmp(d.c_str(), L"Recovery") == 0) continue;
        Evict(data + L"\\" + d, true, false); if (!DirExists(data + L"\\" + d)) ++r.dataFolders; else r.failed.push_back(data + L"\\" + d);
    }
    for (const std::wstring& f : ListDir(data, false)) DeleteFileW((data + L"\\" + f).c_str());
    const std::wstring roam = KnownFolder(FOLDERID_RoamingAppData) + L"\\AnkhCore";
    if (DirExists(roam)) { Evict(roam, true, false); if (DirExists(roam)) r.failed.push_back(roam); else ++r.dataFolders; }
}

}  // namespace ak
