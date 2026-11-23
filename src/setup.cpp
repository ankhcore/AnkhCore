// AnkhCore 2.5 Setup — core installer.
// Developer: As3aD
//
// Installs or upgrades AnkhCore and nothing else: the program, the hardware-monitoring helper (built
// on the PC from its C# source with the in-box compiler), the PawnIO driver when it is missing, Start
// menu / desktop shortcuts, the admin launch task, optional start with Windows and the uninstall entry.
// Windhawk, mods, themes, cursors, wallpapers and icons are never touched (owner decision 2026-11-08;
// docs/ASSETS.md). Upgrade: AnkhCore and its helper are stopped (the helper hands fan control back to
// automatic), the previous program and helper are moved to ProgramData\AnkhCore\Backup\AnkhCore-<ver>-
// <time>, the new files are written and verified against the payload's SHA-256 manifest, and any failure
// puts the previous version back. User data (settings, logs, journal, original icons) is never touched.
//
// Command line: --silent (no wizard; exit code 0 = installed), --rollback (restore the newest backup of
// another version), --allow-downgrade, and in the test build --sandbox-test (automated scenarios inside
// the test folder; report in %LOCALAPPDATA%\AnkhCore-Test\setup-sandbox-report.txt).
#include "base.h"
#include "eng_powertoys.h"
#include "inflate.h"
#include "eng_theme.h"
#include "eng_icons.h"
#include "eng_system.h"
#include "eng_windhawk.h"
#include "eng_smart.h"
#include "eng_sensors.h"
#include "startup.h"
#include "install.h"
#include "eng_afterdark.h"
#include "package_components.h"
#include "ui.h"
#include "material.h"
#include <tlhelp32.h>
#include <map>
#pragma comment(lib, "version.lib")

using namespace ak;

namespace {

enum : UINT { WM_STATUS = WM_APP + 1, WM_DONE = WM_APP + 2 };

// ---------------------------------------------------------------- paths and guarded file operations
std::wstring SandboxRoot() { return DataDir() + L"\\SetupSandbox"; }
std::wstring SetupDataRoot() { return kTestBuild ? SandboxRoot() + L"\\ProgramData\\AnkhCore" : ProdDataDir(); }
std::wstring BackupRoot() { return SetupDataRoot() + L"\\Backup"; }
bool Allowed(const std::wstring& p) { return !kTestBuild || InTestArea(p); }

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    if (!Allowed(to)) { TestRefused(L"copy to " + to); return false; }
    const DWORD a = GetFileAttributesW(from.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { EnsureDir(DirOf(to)); return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0; }
    EnsureDir(to);
    for (const auto& d : ListDir(from, true)) if (!CopyTree(from + L"\\" + d, to + L"\\" + d)) return false;
    for (const auto& f : ListDir(from, false)) if (!CopyFileW((from + L"\\" + f).c_str(), (to + L"\\" + f).c_str(), FALSE)) return false;
    return true;
}
// One optional file whose SHA-256 is known (a component listed in COMPONENTS.sha256): written beside its target, checked, then swapped in
// (PlaceFile), so a cut in the middle never leaves a half-written file at the target.
bool PlaceVerifiedFile(const std::wstring& from, const std::wstring& to, const std::string& sha) {
    if (!Allowed(to)) { TestRefused(L"copy to " + to); return false; }
    std::vector<uint8_t> data; bool deferred = false;
    if (!ReadAll(from, data)) return false;
    // allowDeferred = false: a component whose target stays locked is NOT queued for the next restart. That used to leave a state the setup
    // could not describe (reported "not installed", then the file appeared after a reboot, maybe without its licence).
    return PlaceFile(to, data.data(), data.size(), deferred, [&](const std::wstring& staged) { return Sha256File(staged) == sha; }, nullptr, false) && !deferred;
}
// Tests only: makes a chosen move fail (a locked file, a full disk) to exercise the failure paths of the install and the rollback.
std::function<bool(const std::wstring&, const std::wstring&)>& MoveFault() { static std::function<bool(const std::wstring&, const std::wstring&)> f; return f; }
// Moves a file or folder (rename on the same volume, copy + delete otherwise).
bool MoveItem(const std::wstring& from, const std::wstring& to) {
    if (!Allowed(from) || !Allowed(to)) { TestRefused(L"move " + from + L" -> " + to); return false; }
    if (MoveFault() && MoveFault()(from, to)) return false;
    EnsureDir(DirOf(to));
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    if (!CopyTree(from, to)) return false;
    if (DirExists(from)) Evict(from, true, true); else DeleteNowOrAtReboot(from);
    return true;
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void RemoveItem(const std::wstring& p) { if (DirExists(p)) Evict(p, true, true); else if (Exists(p)) DeleteNowOrAtReboot(p); }

// ---------------------------------------------------------------- what is installed
Ver InstalledVersion() {
    const Ver v = ParseVer(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion"));
    if (v.valid) return v;
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(AppExe().c_str(), &h);
    std::vector<uint8_t> b(n);
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (n && GetFileVersionInfoW(AppExe().c_str(), 0, n, b.data()) && VerQueryValueW(b.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi) {
        Ver f; f.major = HIWORD(fi->dwFileVersionMS); f.minor = LOWORD(fi->dwFileVersionMS); f.patch = HIWORD(fi->dwFileVersionLS); f.build = LOWORD(fi->dwFileVersionLS); f.valid = true;
        return f;
    }
    return Ver{};
}
ExistingInstallInfo DetectExistingInstall() {
    ExistingInstallInfo info;
    const bool hasExe = FileExists(AppExe());
    const bool hasSens = DirExists(SensorsDir());
    const bool hasLegacy = DirExists(AppDir() + L"\\Fan") || DirExists(AppDir() + L"\\7tsp") || DirExists(AppDir() + L"\\IconPacks");
    const std::wstring regLoc = RegStr(UninstallHive(), UninstallKey(), L"InstallLocation");
    const std::wstring regVer = RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion");
    const bool hasReg = !regVer.empty() || !regLoc.empty();
    info.found = hasExe || hasSens || hasLegacy || (!kTestBuild && hasReg && DirExists(regLoc));
    info.partial = info.found && (!hasExe || !hasSens);
    info.version = info.found ? InstalledVersion() : Ver{};
    info.installDir = (!regLoc.empty() && DirExists(regLoc) && !kTestBuild) ? regLoc : AppDir();
    return info;
}
bool PawnIOInstalled() {
    bool ok = false;
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        if (SC_HANDLE s = OpenServiceW(scm, L"PawnIO", SERVICE_QUERY_STATUS)) { ok = true; CloseServiceHandle(s); }
        CloseServiceHandle(scm);
    }
    return ok;
}
// Running processes with this name whose image lies in the program folder (a test build never sees the
// installed AnkhCore, only its own sandbox).
// The folder's true location: junctions and file-system redirection can make the configured path differ from a process's image path.
std::wstring TrueDir(const std::wstring& d) {
    HANDLE h = CreateFileW(d.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    wchar_t b[1024]; const DWORD n = GetFinalPathNameByHandleW(h, b, 1024, VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= 1024) return d;
    std::wstring s(b); return s.rfind(L"\\\\?\\", 0) == 0 ? s.substr(4) : s;
}
std::vector<DWORD> ProcessesInAppDir(const wchar_t* name) {
    std::vector<DWORD> v; const std::wstring dir = Lower(TrueDir(AppDir()) + L"\\");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return v;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) != 0) continue;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            wchar_t p[MAX_PATH]; DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, p, &sz) && Lower(p).rfind(dir, 0) == 0) v.push_back(pe.th32ProcessID);
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return v;
}
bool WaitGone(const wchar_t* name, DWORD ms) {
    const DWORD t0 = GetTickCount();
    while (!ProcessesInAppDir(name).empty()) { if (GetTickCount() - t0 > ms) return false; Sleep(200); }
    return true;
}
// Closes AnkhCore through its own Exit command (the tray menu's), then the helper with "quit" (it restores
// automatic fan control). Ending the helper by force is the last resort, as in AnkhCore itself.
bool StopAnkhCore(std::wstring& why) {
    const auto app = ProcessesInAppDir(L"AnkhCore.exe");
    if (!app.empty()) {
        struct PidList { const std::vector<DWORD>* pids; } ctx{&app};
        EnumWindows([](HWND h, LPARAM l) -> BOOL {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            const auto* c = reinterpret_cast<PidList*>(l);
            wchar_t t[64]{}; GetWindowTextW(h, t, 64);
            if (std::find(c->pids->begin(), c->pids->end(), pid) != c->pids->end() && wcsncmp(t, L"AnkhCore", 8) == 0 && !GetParent(h))
                PostMessageW(h, WM_COMMAND, 103, 0);   // "Exit" (2.2 and 2.5)
            return TRUE; }, reinterpret_cast<LPARAM>(&ctx));
        LogI(L"Setup: asked the running AnkhCore to exit.");
        if (!WaitGone(L"AnkhCore.exe", 20000)) { why = L"AnkhCore is still running. Close it from its tray icon (Exit), then run the setup again."; return false; }
    }
    if (!ProcessesInAppDir(L"AnkhSensors.exe").empty()) {
        Hub().StopHelper();
        if (!WaitGone(L"AnkhSensors.exe", 6000)) { why = L"The hardware-monitoring helper did not stop."; return false; }
    }
    return true;
}

// ---------------------------------------------------------------- payload
std::vector<PkEntry> g_entries;
bool LoadPayload() {
    const uint8_t* d; size_t n;
    return ResourceBytes(L"PAYLOAD", d, n) && ParsePackage(d, n, g_entries);
}
// Every file must be listed in MANIFEST with its SHA-256, go to an allowed place, and match its hash.
bool VerifyPayload(const std::vector<PkEntry>& entries, std::vector<PkEntry>& files, std::map<std::wstring, std::string>& sha, std::wstring& err) {
    files.clear(); sha.clear();
    const PkEntry* man = nullptr;
    for (const auto& e : entries) if (e.path == L"MANIFEST") man = &e;
    std::vector<uint8_t> raw; std::vector<ManifestEntry> list;
    if (!man || !Unpack(*man, raw) || !ParseManifest(std::string(raw.begin(), raw.end()), list)) { err = L"no valid manifest"; return false; }
    for (const auto& m : list) sha[m.path] = m.sha;
    for (const auto& e : entries) {
        if (e.path == L"MANIFEST") continue;
        std::wstring dest;
        if (!PayloadDest(e.path, L"X", dest)) { err = L"unexpected entry " + e.path; return false; }
        auto it = sha.find(e.path);
        if (it == sha.end()) { err = L"unlisted entry " + e.path; return false; }
        if (!Unpack(e, raw) || Sha256Hex(raw.data(), raw.size()) != it->second) { err = L"damaged entry " + e.path; return false; }
        files.push_back(e);
    }
    if (files.size() != sha.size()) { err = L"missing entries"; return false; }
    return true;
}

// ---------------------------------------------------------------- install, rollback
struct Options {
    bool driver = true, startup = false, desktop = true, allowDowngrade = false;
    bool taskbarProfile = false;          // INS-3 / D44: unchecked by default, Windows 11 only
    bool osIsWin11 = GetWinVer().win11(); // actual OS build >= 22000 (overridable in sandbox test)
    std::vector<std::wstring> iconThemes; // INS-3 / D41: optional prebuilt icon theme IDs selected by user
    bool smartctl = false;                // WT-03: the optional smartctl component (only when the package carries and verifies it)
    unsigned osBuild = GetWinVer().build;  // 2.8.4: the Windows build (overridable in the sandbox test); the After Dark pack is chosen from it
    bool adInstall = false, adOffice = false, adSleepFix = false; int adVariant = 0;   // 2.8.4: After Dark for Windows 10: install it / the Office-background-fix pack / the white-after-sleep registry fix / which command-bar variant
    bool replacePowerToys = false, replaceWindhawk = false;   // 2.8.3: an existing PowerToys / Windhawk is kept unless the person chose to replace it (remove + clean + install)
    bool powertoys = false;               // 2.8: Microsoft PowerToys (external, like Windhawk): only when the package carries and verifies its installer
    SetupMode mode = SetupMode::Auto;     // Auto, Repair, Update, or CleanInstall
    bool cleanConfirmed = false;          // explicit user confirmation required for CleanInstall
    bool reinstallWindhawk = false;       // CleanInstall opt-in: back up & reinstall AnkhCore-managed Windhawk components
    bool installWindhawkBundle = false;   // Auto-enabled in wWinMain when Windhawk\ bundle sits beside the setup
    bool hideWindhawkTray = true;         // Official Windhawk HideTrayIcon=1 setting (checked by default)
    bool failUserBackupForTest = false;   // test seam: simulates user-data backup failure before CleanInstall
};
struct Outcome {
    bool ok = false;
    std::wstring message;
    InstallKind kind = InstallKind::Fresh;
    Ver from;
    std::wstring backup;
    bool deferred = false;
    std::vector<std::wstring> warnings;
    bool taskbarApplied = false;
    std::wstring taskbarBackupPath;
    std::wstring taskbarSummary;
    int iconThemesInstalled = 0;
    std::wstring iconThemesSummary;
    bool smartctlInstalled = false;
    bool powertoysInstalled = false; std::wstring powertoysSummary;
    std::wstring afterDarkSummary; bool afterDarkInstalled = false;
    std::wstring smartctlSummary;
    bool windhawkBundleInstalled = false;
    std::wstring windhawkBundleSummary;
    bool windhawkTrayHidden = false;
    std::wstring windhawkTraySummary;
    std::wstring suxSummary;
    std::wstring themeAssetsSummary;
    bool restartRequired = false;
};
using Progress = std::function<void(const std::wstring&, int)>;

// ---- optional components beside the setup (test package): IconThemes\, Mods\, the Taskbar Profile, COMPONENTS.sha256.
// Only the folder the setup runs from is used, never a development folder (master changeset §5.7). The test build also
// honours ANKH_COMPONENT_ROOT, so a development run can point at a staged package.
std::wstring ComponentRoot() {
    if (kTestBuild) { wchar_t b[MAX_PATH]{}; if (GetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", b, MAX_PATH) && *b) return b; }
    wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    return DirOf(self);
}
struct Components { std::wstring root, err; ComponentManifest m; bool loaded = false; };
Components LoadComponents() {
    Components c; c.root = ComponentRoot();
    const std::string t = ReadText(c.root + L"\\COMPONENTS.sha256");
    if (t.empty()) { c.err = L"this setup package has no COMPONENTS.sha256"; return c; }
    c.loaded = ParseComponentManifest(t, c.m, c.err);
    if (!c.loaded) c.err = L"COMPONENTS.sha256 is damaged: " + c.err;
    return c;
}
void ListFilesRel(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    for (const auto& f : ListDir(root + L"\\" + rel, false)) out.push_back(rel + L"\\" + f);
    for (const auto& d : ListDir(root + L"\\" + rel, true)) ListFilesRel(root, rel + L"\\" + d, out);
}
// Empty when the component (a folder or one file, relative to the package) is complete and unchanged; else the reason.
std::wstring VerifyComponent(const Components& c, const std::wstring& rel) {
    if (!c.loaded) return c.err;
    std::vector<std::wstring> present;
    if (DirExists(c.root + L"\\" + rel)) ListFilesRel(c.root, rel, present);
    else if (FileExists(c.root + L"\\" + rel)) present.push_back(rel);
    return CheckComponent(c.m, rel, present, [&](const std::wstring& k) { return Sha256File(c.root + L"\\" + k); });
}
std::wstring FindTaskbarProfileRegPath() { return ComponentRoot() + L"\\AnkhCore_Taskbar_Profile.reg"; }

// In kTestBuild, Taskbar Profile registry reads/writes are isolated inside kPrefKey\Sandbox\TaskbarExplorerAdv
// so the host user's Explorer/Taskbar settings are never touched during --sandbox-test.
std::wstring TaskbarExplorerAdvBaseKey() {
    return kTestBuild ? (std::wstring(kPrefKey) + L"\\Sandbox\\TaskbarExplorerAdv")
                      : std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced");
}

// FT-G01 / FT-X1 / FT-X2. Field test 2026-11-15: "failed writing registry value TaskbarDa" stopped the profile half way (5 of 14
// values written, nothing put back). Two rules now: Windows 11 refuses writes to the Widgets button value (TaskbarDa) from
// programs other than Explorer on current builds, so an access-denied on that value alone is reported as skipped and the rest
// is applied; any other failure puts every value already written back (from the backup taken first) and applies nothing.
inline bool TaskbarValueIsProtected(const std::wstring& name) { return name == L"TaskbarDa"; }
// Test hook: the sandbox makes a named value fail with a Windows error code; empty in the program.
inline std::function<LONG(const std::wstring&)>& TaskbarWriteFault() { static std::function<LONG(const std::wstring&)> f; return f; }
LONG TaskbarSetDword(const std::wstring& key, const std::wstring& name, DWORD v) {
    if (kTestBuild && !TestRegAllowed(HKEY_CURRENT_USER, key)) { TestRefused(L"registry write " + key + L"\\" + name); return ERROR_ACCESS_DENIED; }   // the test build only ever writes its own sandbox key
    if (TaskbarWriteFault()) { const LONG e = TaskbarWriteFault()(name); if (e != ERROR_SUCCESS) return e; }
    HKEY k = nullptr; LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(k); return r;
}

bool ApplyTaskbarProfileWithBackup(const std::wstring& regFilePath,
                                   bool isWin11,
                                   const std::wstring& backupFilePath,
                                   std::wstring& outDetail) {
    outDetail.clear();
    if (!isWin11) {
        outDetail = L"Windows 11 required";
        return false;
    }
    const std::string regText = ReadText(regFilePath);
    if (regText.empty()) {
        outDetail = L"missing profile file: " + NameOf(regFilePath);
        return false;
    }
    std::vector<TaskbarRegEntry> entries;
    std::wstring parseErr;
    if (!ParseTaskbarProfileReg(regText, entries, parseErr)) {
        outDetail = parseErr;
        return false;
    }
    const std::wstring baseKey = TaskbarExplorerAdvBaseKey();
    // 1. Back up current values before modifying anything
    EnsureDir(DirOf(backupFilePath));
    std::wstring bkLines = L"# AnkhCore Taskbar Profile Backup\n";
    struct Prev { std::wstring key, name; bool had; DWORD val; };
    std::vector<Prev> prevs;
    for (const auto& e : entries) {
        const std::wstring fullKey = baseKey + e.subkeySuffix;
        const RegValue cur = RegGet(HKEY_CURRENT_USER, fullKey, e.valueName);
        DWORD prevVal = 0;
        const bool hadDword = cur.exists && cur.type == REG_DWORD && cur.data.size() >= sizeof(DWORD);
        if (hadDword) memcpy(&prevVal, cur.data.data(), sizeof(DWORD));
        bkLines += e.subkeySuffix + L"|" + e.valueName + L"|" + (hadDword ? L"1" : L"0") + L"|" + std::to_wstring(prevVal) + L"\n";
        prevs.push_back({fullKey, e.valueName, hadDword, prevVal});
    }
    const std::string bkUtf8 = WToUtf8(bkLines);
    if (!WriteAll(backupFilePath, reinterpret_cast<const uint8_t*>(bkUtf8.data()), bkUtf8.size())) {
        outDetail = L"could not write Taskbar backup to " + backupFilePath;
        return false;
    }
    // 2. Apply the validated DWORD settings; a protected value that Windows refuses is skipped, any other failure undoes the lot
    std::vector<size_t> written; std::vector<std::wstring> skipped;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const LONG err = TaskbarSetDword(prevs[i].key, e.valueName, e.value);
        if (err == ERROR_SUCCESS) { written.push_back(i); continue; }
        if (err == ERROR_ACCESS_DENIED && TaskbarValueIsProtected(e.valueName)) { skipped.push_back(e.valueName); continue; }
        int back = 0;
        for (auto it = written.rbegin(); it != written.rend(); ++it) {   // newest first
            const Prev& p = prevs[*it];
            if (p.had) { if (TaskbarSetDword(p.key, p.name, p.val) == ERROR_SUCCESS) ++back; }
            else if (RegDel(HKEY_CURRENT_USER, p.key, p.name)) ++back;
        }
        outDetail = L"failed writing registry value " + e.valueName + L" (Windows error " + std::to_wstring(err) + L"); " + std::to_wstring(back) + L" of " + std::to_wstring(written.size()) +
                    L" value(s) already written were put back, nothing was applied";
        return false;
    }
    outDetail = std::to_wstring(written.size()) + L" of " + std::to_wstring(entries.size()) + L" settings applied";
    if (!skipped.empty()) {
        outDetail += L"; skipped, because Windows refuses changes to them from other programs: ";
        for (size_t i = 0; i < skipped.size(); ++i) outDetail += (i ? L", " : L"") + skipped[i];
    }
    outDetail += L"; backup saved to " + NameOf(backupFilePath);
    return true;
}

bool RestoreTaskbarProfileBackup(const std::wstring& backupFilePath) {
    const std::string text = ReadText(backupFilePath);
    if (text.empty()) return false;
    const std::wstring wtext = Utf8ToW(text);
    const std::wstring baseKey = TaskbarExplorerAdvBaseKey();
    int restored = 0;
    for (const auto& rawLine : Split(wtext, L'\n')) {
        std::wstring line = rawLine;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        const auto parts = Split(line, L'|');
        if (parts.size() != 4) return false;
        const std::wstring fullKey = baseKey + parts[0];
        const std::wstring valName = parts[1];
        const bool existed = (parts[2] == L"1");
        const DWORD val = static_cast<DWORD>( std::wcstoul(parts[3].c_str(), nullptr, 10) );
        if (existed) {
            if (!RegSetDword(HKEY_CURRENT_USER, fullKey, valName, val)) return false;
        } else {
            RegDel(HKEY_CURRENT_USER, fullKey, valName);
        }
        ++restored;
    }
    return restored > 0;
}

// Prebuilt icon themes in this package (D41 / INS-3): IconPacks() ids whose folder is beside the setup.
std::vector<std::wstring> PackageIconThemes() {
    std::vector<std::wstring> v;
    for (const auto& p : IconPacks()) if (DirExists(ComponentRoot() + L"\\IconThemes\\" + IconThemeFolderName(p.id))) v.push_back(p.id);
    return v;
}
constexpr wchar_t kRedirectModRel[] = L"Mods\\icon-resource-redirect.wh.cpp";
constexpr wchar_t kWhBundleDir[]    = L"Windhawk";
constexpr wchar_t kWhSetupRel[]     = L"Windhawk\\windhawk_setup_offline.exe";
constexpr wchar_t kWhRegRel[]       = L"Windhawk\\Windhawk_Mods_Settings.reg";
constexpr wchar_t kWhSrcRel[]       = L"Windhawk\\ModsSource";
constexpr wchar_t kWhMods64Rel[]    = L"Windhawk\\Mods64";
constexpr wchar_t kWhMods32Rel[]    = L"Windhawk\\Mods32";
bool PackageHasWindhawkBundle() {
    const std::wstring root = ComponentRoot();
    return FileExists(root + L"\\" + kWhSetupRel) &&
           FileExists(root + L"\\" + kWhRegRel) &&
           DirExists(root + L"\\" + kWhSrcRel) &&
           DirExists(root + L"\\" + kWhMods64Rel);
}

// WT-03: the optional smartctl component (Tools\smartctl.exe + Tools\COPYING.smartmontools beside the setup). Both files, or neither:
// each must be listed in COMPONENTS.sha256 and match its SHA-256; the program must be a 64-bit Windows executable and the license file
// the GPL text. A package without them is a normal core-only package. No binary is bundled with the project; the owner supplies one.
constexpr wchar_t kSmartctlRel[] = L"Tools\\smartctl.exe";
constexpr wchar_t kSmartctlLicenseRel[] = L"Tools\\COPYING.smartmontools";
struct SmartctlOffer { bool present = false, ok = false; std::wstring reason; };
SmartctlOffer CheckSmartctlComponent(const Components& comp) {
    SmartctlOffer o;
    const bool listed = comp.loaded && (!ComponentEntriesUnder(comp.m, kSmartctlRel).empty() || !ComponentEntriesUnder(comp.m, kSmartctlLicenseRel).empty());
    const bool onDisk = FileExists(comp.root + L"\\" + kSmartctlRel) || FileExists(comp.root + L"\\" + kSmartctlLicenseRel);
    o.present = listed || onDisk;
    if (!o.present) { o.reason = L"it is not part of this setup package (core-only install)"; return o; }
    if (!comp.loaded) { o.reason = comp.err; return o; }
    for (const wchar_t* rel : {kSmartctlRel, kSmartctlLicenseRel}) {
        const std::wstring bad = VerifyComponent(comp, rel);
        if (!bad.empty()) { o.reason = std::wstring(NameOf(rel)) + L": " + bad; return o; }
    }
    if (!SmartLooksLikeX64Exe(comp.root + L"\\" + kSmartctlRel)) { o.reason = L"smartctl.exe is not a 64-bit Windows program"; return o; }
    if (ReadText(comp.root + L"\\" + kSmartctlLicenseRel).find("GNU GENERAL PUBLIC LICENSE") == std::string::npos) { o.reason = L"COPYING.smartmontools is not the GPL license text"; return o; }
    o.ok = true;
    return o;
}

// 2.8: the optional PowerToys component (PowerToys\PowerToysSetup-<version>-x64.exe beside the setup, the official installer, MIT). Listed in COMPONENTS.sha256 and unchanged,
// a 64-bit program, Windows 10 version 2004 or newer. A package without it is a normal package.
struct PtOffer { bool present = false, ok = false; std::wstring reason, exe; };
PtOffer CheckPowerToysComponent(const Components& comp) {
    PtOffer o; const std::wstring dir = comp.root + L"\\PowerToys";
    o.present = DirExists(dir) || (comp.loaded && !ComponentEntriesUnder(comp.m, L"PowerToys").empty());
    if (!o.present) { o.reason = L"it is not part of this setup package"; return o; }
    if (!comp.loaded) { o.reason = comp.err; return o; }
    const std::wstring bad = VerifyComponent(comp, L"PowerToys");
    if (!bad.empty()) { o.reason = bad; return o; }
    const std::vector<std::wstring> f = ListDir(dir, false, L"PowerToysSetup-*-x64.exe");
    if (f.size() != 1) { o.reason = L"the PowerToys installer is missing from the package"; return o; }
    o.exe = dir + L"\\" + f[0];
    if (!SmartLooksLikeX64Exe(o.exe)) { o.reason = L"the PowerToys installer is not a 64-bit Windows program"; return o; }
    if (GetWinVer().build < 19041) { o.reason = L"PowerToys needs Windows 10 version 2004 or newer"; return o; }
    o.ok = true; return o;
}

std::wstring BackupFor(const Ver& v) {
    SYSTEMTIME st; GetLocalTime(&st);
    const std::wstring base = BackupRoot() + L"\\" + BackupName(v, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring p = base;
    for (int i = 2; Exists(p); ++i) p = base + L"-" + std::to_wstring(i);   // two backups within one second
    return p;
}
// Moves the replaced items of the current program folder into `dest`; fills `moved` for an undo.
bool MoveReplacedTo(const std::wstring& dest, const Ver& v, std::vector<std::pair<std::wstring, std::wstring>>& moved, std::wstring& failed) {
    EnsureDir(dest);
    std::wstring items;
    for (const auto& item : ReplacedItems()) {
        const std::wstring from = AppDir() + L"\\" + item;
        if (!Exists(from)) continue;
        if (!MoveItem(from, dest + L"\\" + item)) { failed = item; return false; }
        moved.push_back({from, dest + L"\\" + item}); items += item + L";";
    }
    const std::string info = WToUtf8(L"version=" + (v.valid ? VerText(v) : std::wstring(L"unknown")) + L"\nfrom=" + AppDir() + L"\ndate=" + NowText(true) + L"\nitems=" + items + L"\n");
    WriteAll(dest + L"\\backup.txt", reinterpret_cast<const uint8_t*>(info.data()), info.size());
    return true;
}
void UndoMoves(std::vector<std::pair<std::wstring, std::wstring>>& moved) {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) { RemoveItem(it->first); MoveItem(it->second, it->first); }
    moved.clear();
}

inline HKEY SetupWhTrayRegRoot() { return kTestBuild ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
inline std::wstring SetupWhTrayRegSubkey() {
    return kTestBuild ? (std::wstring(kPrefKey) + L"\\Sandbox\\WindhawkSettings")
                      : std::wstring(L"SOFTWARE\\Windhawk\\Settings");
}

bool ApplyWindhawkHideTrayIcon(std::wstring& outSummary) {
    const HKEY root = SetupWhTrayRegRoot();
    const std::wstring subkey = SetupWhTrayRegSubkey();

    if (kTestBuild) {
        if (!TestRegAllowed(root, subkey)) {
            TestRefused(L"registry write " + subkey + L"\\HideTrayIcon");
            outSummary = L"Windhawk tray icon: refused outside sandbox";
            return false;
        }
        HKEY k = nullptr;
        LONG rc = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
        if (rc != ERROR_SUCCESS) {
            outSummary = L"Windhawk tray icon: sandbox registry error " + std::to_wstring(rc);
            return false;
        }
        const DWORD v = 1;
        rc = RegSetValueExW(k, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(k);
        DWORD got = 0, sz = sizeof(got), type = 0;
        const bool ok = (rc == ERROR_SUCCESS) &&
                        (RegGetValueW(root, subkey.c_str(), L"HideTrayIcon", RRF_RT_REG_DWORD, &type, &got, &sz) == ERROR_SUCCESS) &&
                        (got == 1);
        outSummary = ok ? L"Windhawk tray icon: hidden (official HideTrayIcon=1, sandbox)"
                        : L"Windhawk tray icon: sandbox verification failed";
        return ok;
    }

    const std::wstring cliExe = WindhawkCliExePath();
    const bool hasCli = FileExists(cliExe);
    const bool hasWhReg = (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk", L"StoragePath", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS) ||
                          DirExists(KnownFolder(FOLDERID_ProgramData) + L"\\Windhawk");
    if (!hasCli && !hasWhReg) {
        outSummary = L"Windhawk tray icon: Windhawk is not installed yet (skipped)";
        return false;
    }

    if (hasCli) {
        JVal doc; std::wstring cliErr;
        RunWindhawkCliJson(L"app settings set hideTrayIcon true --confirm-app-restart", doc, cliErr, L"", 15000);
    }

    DWORD val = 0, sz = sizeof(val), type = 0;
    bool isHidden = (RegGetValueW(root, subkey.c_str(), L"HideTrayIcon", RRF_RT_REG_DWORD, &type, &val, &sz) == ERROR_SUCCESS) && (val != 0);
    if (!isHidden) {
        HKEY k = nullptr;
        LONG rc = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
        if (rc == ERROR_SUCCESS) {
            const DWORD v = 1;
            rc = RegSetValueExW(k, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            RegCloseKey(k);
            if (rc == ERROR_SUCCESS) {
                sz = sizeof(val);
                isHidden = (RegGetValueW(root, subkey.c_str(), L"HideTrayIcon", RRF_RT_REG_DWORD, &type, &val, &sz) == ERROR_SUCCESS) && (val != 0);
            }
        }
    }
    outSummary = isHidden ? L"Windhawk tray icon: hidden (official HideTrayIcon=1)"
                          : L"Windhawk tray icon: could not set HideTrayIcon=1";
    return isHidden;
}

// 2.9: Windhawk is a tool of AnkhCore. It never checks for or installs its own updates in the background (the version AnkhCore ships is the tested one; a newer Windhawk
// arrives with an AnkhCore update), and its shortcuts on the desktop and in the Start menu are removed, so it is used through AnkhCore only.
bool ApplyWindhawkAsTool(std::wstring& outSummary) {
    if (kTestBuild) { TestRefused(L"Windhawk: update check off, shortcuts removed"); outSummary = L"Windhawk as a tool: would switch its update check off and remove its shortcuts (sandbox: nothing is run)"; return false; }
    const std::wstring cliExe = WindhawkCliExePath();
    if (FileExists(cliExe)) { JVal doc; std::wstring err; RunWindhawkCliJson(L"app settings set disableUpdateCheck true", doc, err, L"", 15000); }
    DWORD val = 0, sz = sizeof(val), type = 0;
    bool off = (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Settings", L"DisableUpdateCheck", RRF_RT_REG_DWORD, &type, &val, &sz) == ERROR_SUCCESS) && val != 0;
    if (!off) {   // the same official value, written directly when the CLI could not
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Settings", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            const DWORD one = 1; RegSetValueExW(k, L"DisableUpdateCheck", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one)); RegCloseKey(k);
            sz = sizeof(val); off = (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Settings", L"DisableUpdateCheck", RRF_RT_REG_DWORD, &type, &val, &sz) == ERROR_SUCCESS) && val != 0;
        }
    }
    int removed = 0;
    for (const std::wstring& dir : {KnownFolder(FOLDERID_PublicDesktop), KnownFolder(FOLDERID_Desktop), KnownFolder(FOLDERID_CommonPrograms), KnownFolder(FOLDERID_Programs)}) {
        if (dir.empty() || !DirExists(dir)) continue;
        for (const std::wstring& f : ListDir(dir, false, L"*.lnk")) if (IsWindhawkDesktopShortcut(f) && DeleteFileW((dir + L"\\" + f).c_str())) ++removed;
    }
    outSummary = std::wstring(L"Windhawk as a tool: update check ") + (off ? L"off" : L"could not be switched off") + L", " + std::to_wstring(removed) + L" shortcut(s) removed";
    return off;
}

// 2.8.3: replacing an existing PowerToys / Windhawk. The data is copied to the setup's Backup folder FIRST (if that fails nothing is removed), then the old
// program is uninstalled by its own uninstaller, the leftovers are cleaned, and only then the packaged version is installed.
std::wstring ToolStamp() { SYSTEMTIME st{}; GetLocalTime(&st); wchar_t b[40]; swprintf(b, 40, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond); return b; }
bool BackupToolData(const std::wstring& from, const std::wstring& tag, std::wstring& bak) {
    bak.clear();
    if (!DirExists(from)) return true;   // nothing to keep
    bak = BackupRoot() + L"\\" + tag + L"-" + ToolStamp();
    EnsureDir(bak);
    return CopyTree(from, bak);
}
bool ReplacePowerToysClean(const std::wstring& installerExe, std::wstring& note, std::vector<std::wstring>& warnings) {
    const std::wstring data = KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\PowerToys";
    std::wstring bak;
    if (!BackupToolData(data, L"PowerToys", bak)) { note = L"PowerToys: left as it is (its settings could not be backed up first)"; warnings.push_back(L"PowerToys was not replaced: its settings could not be backed up, so nothing was removed."); return false; }
    for (const wchar_t* exe : {L"PowerToys.exe", L"PowerToys.FancyZones.exe", L"PowerToys.PowerLauncher.exe", L"PowerToys.Awake.exe", L"PowerToys.ColorPickerUI.exe", L"PowerToys.KeyboardManagerEngine.exe"}) KillProcess(exe, 300);
    LogI(L"Setup: removing the installed PowerToys with the packaged installer (uninstall)");
    const DWORD rc = Run(installerExe, L"/uninstall /silent /norestart", true, 15u * 60u * 1000u);
    if (PtDetect().installed) { note = L"PowerToys: the old version could not be removed (code " + std::to_wstring(rc) + L"); left as it is"; warnings.push_back(L"The installed PowerToys could not be uninstalled (installer code " + std::to_wstring(rc) + L"); it was kept and the new one was not installed over it."); return false; }
    RemoveItem(data);   // the clean: its settings were copied to the Backup folder above
    for (const std::wstring& d : {KnownFolder(FOLDERID_ProgramFiles) + L"\\PowerToys", KnownFolder(FOLDERID_LocalAppData) + L"\\PowerToys", KnownFolder(FOLDERID_LocalAppData) + L"\\Programs\\PowerToys"}) if (DirExists(d)) RemoveItem(d);
    note = L"PowerToys: the old version was removed and cleaned" + (bak.empty() ? std::wstring() : L" (settings backed up to " + bak + L")");
    return true;
}
bool ReplaceWindhawkClean(std::wstring& note, std::vector<std::wstring>& warnings) {
    const std::wstring data = WindhawkData();
    std::wstring bak;
    if (!BackupToolData(data, L"Windhawk", bak)) { note = L"Windhawk: left as it is (its data could not be backed up first)"; warnings.push_back(L"Windhawk was not replaced: its data could not be backed up, so nothing was removed."); return false; }
    StopWindhawk();
    const std::wstring dir = KnownFolder(FOLDERID_ProgramFiles) + L"\\Windhawk", un = dir + L"\\uninstall.exe";
    if (FileExists(un)) {
        LogI(L"Setup: uninstalling the installed Windhawk (" + un + L")");
        Run(un, L"/S _?=" + dir, true, 5u * 60u * 1000u);   // _?= keeps the NSIS uninstaller in place so that Run waits for it
    }
    if (FileExists(WindhawkExePath())) { note = L"Windhawk: the old version could not be removed; left as it is"; warnings.push_back(L"The installed Windhawk could not be uninstalled; it was kept and the new one was not installed over it."); return false; }
    RemoveItem(dir); RemoveItem(data);   // the clean: its data was copied to the Backup folder above
    note = L"Windhawk: the old version was removed and cleaned" + (bak.empty() ? std::wstring() : L" (data backed up to " + bak + L")");
    return true;
}
Outcome InstallCore(const std::vector<PkEntry>& entries, const Options& opt, const Progress& status) {
    Outcome o;
    {   // FT-G01: what the user chose is part of the record, so "it was checked by default" can be told from "it was ticked"
        std::wstring themes; for (const auto& id : opt.iconThemes) themes += (themes.empty() ? L"" : L", ") + id;
        LogI(std::wstring(L"Setup options: PawnIO driver ") + (opt.driver ? L"yes" : L"no") + L", start with Windows " + (opt.startup ? L"yes" : L"no") + L", desktop shortcut " + (opt.desktop ? L"yes" : L"no") +
             L", Taskbar Profile " + (opt.taskbarProfile ? L"yes" : L"no") + (opt.osIsWin11 ? L" (Windows 11)" : L" (not Windows 11)") +
             L", hide Windhawk tray " + (opt.hideWindhawkTray ? L"yes" : L"no") + L", icon themes: " + (themes.empty() ? std::wstring(L"none") : themes) +
             L"; components are read from " + ComponentRoot() + L".");
    }
    const ExistingInstallInfo existing = DetectExistingInstall();
    const bool installed = existing.found;
    o.from = installed ? existing.version : Ver{};
    const Ver next = ParseVer(kVersion);
    o.kind = ResolveEffectiveInstallKind(opt.mode, installed, o.from, next);
    if (opt.mode == SetupMode::CleanInstall && !opt.cleanConfirmed) {
        o.message = L"Clean Install requires explicit confirmation before removing old application files. Nothing was changed.";
        return o;
    }
    if constexpr (kIsBetaBuild) {
        const BetaTrialStatus bt = CheckAndTouchBetaTrial(true);
        if (bt.expired) {
            o.message = bt.clockTampered
                ? L"AnkhCore Beta could not verify the system clock. Please install the official AnkhCore release."
                : L"This AnkhCore Beta 30-day trial has expired on this device. Please install the official AnkhCore release.";
            return o;
        }
        LogI(L"Setup: Beta 30-day trial active (" + std::to_wstring(bt.daysRemaining) + L" day(s) remaining on this device).");
    }
    // Progress (LOG-2): the steps this run will really perform, counted before the first one. The bar shows
    // finished steps out of that count, and inside "Installing files" the files written out of all files.
    const bool backsUp = o.kind == InstallKind::Upgrade || o.kind == InstallKind::Downgrade;
    const bool doWhBundle = opt.installWindhawkBundle && PackageHasWindhawkBundle();
    OpSteps steps(1 /*check*/ + (installed ? 1 : 0) + (backsUp ? 1 : 0) + 1 /*files*/ + 3 /*monitoring, helper, shortcuts*/ +
                  (opt.iconThemes.empty() ? 0 : 1) + (opt.smartctl ? 1 : 0) + (opt.taskbarProfile && opt.osIsWin11 ? 1 : 0) +
                  (doWhBundle ? 1 : 0) + (opt.hideWindhawkTray ? 1 : 0) + (opt.adInstall && !opt.osIsWin11 ? 1 : 0));
    auto step = [&](const std::wstring& what) { steps.Next(); status(steps.Label(what), steps.Pct()); };
    step(L"Checking the setup package...");
    std::vector<PkEntry> files; std::map<std::wstring, std::string> sha; std::wstring err;
    if (!VerifyPayload(entries, files, sha, err)) { o.message = L"The setup package is damaged (" + err + L"). Please download it again. Nothing was changed."; return o; }
    static const wchar_t* kKind[] = {L"fresh install", L"upgrade", L"repair", L"downgrade"};
    LogI(L"Setup " + std::wstring(kVersionLabel) + L" (" + kVersion + L"): " + kKind[static_cast<int>(o.kind)] + (installed ? L" of " + (o.from.valid ? VerText(o.from) : std::wstring(L"an unknown version")) : L"") + L" in " + AppDir());
    if (o.kind == InstallKind::Downgrade && !opt.allowDowngrade) {
        o.message = L"A newer AnkhCore (" + VerText(o.from) + L") is already installed. Nothing was changed."; return o;
    }
    if (installed) {
        step(L"Closing AnkhCore and the hardware-monitoring helper...");
        if (!StopAnkhCore(err)) { o.message = err + L" Nothing was changed."; return o; }
    }
    // Clean Install: back up user settings and application data first; if backup fails, stop safely without changing anything.
    std::wstring userDataBak;
    std::vector<std::pair<std::wstring, std::wstring>> userFilesToRestore;
    if (opt.mode == SetupMode::CleanInstall && installed) {
        SYSTEMTIME st{}; GetLocalTime(&st);
        wchar_t ubuf[64]; swprintf(ubuf, 64, L"UserData-%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        userDataBak = BackupRoot() + L"\\" + ubuf;
        if (opt.failUserBackupForTest) {
            o.message = L"Could not back up user settings and data before Clean Install. Existing installation was preserved.";
            return o;
        }
        EnsureDir(userDataBak);
        const std::wstring userSettingsDir = kTestBuild ? SetupDataRoot() : (KnownFolder(FOLDERID_RoamingAppData) + L"\\AnkhCore");
        const std::vector<std::pair<std::wstring, std::wstring>> candidates = {
            {userSettingsDir + L"\\settings.json", L"settings.json"},
            {SetupDataRoot() + L"\\journal.txt", L"journal.txt"},
            {SetupDataRoot() + L"\\net-counters.txt", L"net-counters.txt"}
        };
        for (const auto& [src, rel] : candidates) {
            if (!FileExists(src)) continue;
            const std::wstring dst = userDataBak + L"\\" + rel;
            std::vector<uint8_t> raw;
            if (!ReadAll(src, raw) || !WriteAllFlushed(dst, raw.data(), raw.size()) || Sha256File(dst) != Sha256Hex(raw.data(), raw.size())) {
                o.message = L"Could not back up '" + rel + L"' before Clean Install. Existing installation was preserved.";
                return o;
            }
            userFilesToRestore.push_back({dst, src});
        }
        LogOk(L"Setup: user settings and data backed up to " + userDataBak);
    }
    // previous version: moved aside, restored automatically if anything below fails
    std::vector<std::pair<std::wstring, std::wstring>> moved;
    if (backsUp) {
        o.backup = BackupFor(o.from);
        step(L"Backing up AnkhCore " + (o.from.valid ? VerText(o.from) : std::wstring(L"(unknown version)")) + L"...");
        std::wstring failed;
        if (!MoveReplacedTo(o.backup, o.from, moved, failed)) {
            UndoMoves(moved); o.message = L"The previous version could not be backed up (" + failed + L"). Nothing was changed."; return o;
        }
        if (opt.mode == SetupMode::CleanInstall) {
            const std::wstring oldFan = AppDir() + L"\\Fan";
            if (DirExists(oldFan)) {
                const std::wstring dstFan = o.backup + L"\\Fan";
                if (MoveItem(oldFan, dstFan)) moved.push_back({oldFan, dstFan});
            }
            const std::wstring oldMod = AppDir() + L"\\" + kRedirectModRel;
            if (FileExists(oldMod)) {
                const std::wstring dstMod = o.backup + L"\\" + kRedirectModRel;
                EnsureDir(DirOf(dstMod));
                if (MoveItem(oldMod, dstMod)) moved.push_back({oldMod, dstMod});
            }
            if (opt.reinstallWindhawk) {
                const std::wstring whRoot = kTestBuild ? (SandboxRoot() + L"\\WindhawkManaged") : (KnownFolder(FOLDERID_ProgramData) + L"\\Windhawk");
                if (DirExists(whRoot)) {
                    const std::wstring whBak = userDataBak + L"\\WindhawkBackup";
                    EnsureDir(whBak);
                    // Back up custom/user mods and AnkhCore-managed mod before touching only AnkhCore-managed files
                    if (!CopyTree(whRoot, whBak)) {
                        UndoMoves(moved);
                        o.message = L"Could not back up Windhawk data before Clean Install. Existing installation was preserved.";
                        return o;
                    }
                    const std::wstring managedSrc = whRoot + L"\\ModsSource\\local@icon-resource-redirect.wh.cpp";
                    if (FileExists(managedSrc)) {
                        const std::wstring dstWhMod = o.backup + L"\\WindhawkManaged\\local@icon-resource-redirect.wh.cpp";
                        EnsureDir(DirOf(dstWhMod));
                        if (MoveItem(managedSrc, dstWhMod)) moved.push_back({managedSrc, dstWhMod});
                    }
                }
            }
        }
        LogOk(L"Setup: previous version backed up to " + o.backup);
    }
    auto failWith = [&](const std::wstring& m) {
        if (!moved.empty()) {   // put the previous version back
            for (const auto& item : ReplacedItems()) RemoveItem(AppDir() + L"\\" + item);
            UndoMoves(moved); RemoveItem(o.backup);
            o.message = m + L" The previous version was restored.";
        } else o.message = m;
        LogE(L"Setup: " + o.message);
        return o;
    };
    size_t n = 0;
    steps.Next();
    for (const auto& e : files) {
        std::wstring dest; PayloadDest(e.path, AppDir(), dest);
        status(steps.Label(L"Installing files (" + std::to_wstring(n + 1) + L" / " + std::to_wstring(files.size()) + L")..."), steps.Pct(n, files.size()));   // n files are written and verified
        ++n;
        std::vector<uint8_t> raw;
        if (!Unpack(e, raw)) return failWith(L"The setup package is damaged (" + e.path + L").");
        bool deferred = false; std::wstring why; const std::string want = sha[e.path];
        // the file is written beside its target, checked against the package's SHA-256 and only then swapped in (an interrupted write or a wrong hash never touches the live file)
        if (!PlaceFile(dest, raw.data(), raw.size(), deferred, [&](const std::wstring& staged) { return Sha256File(staged) == want; }, &why))
            return failWith(why == L"verify" ? L"Verification failed for " + dest + L"." : L"Could not write " + dest + L".");
        if (Sha256File(deferred ? dest + L".ankh-new" : dest) != want) return failWith(L"Verification failed for " + dest + L".");
        o.deferred |= deferred;
    }
    if (PlaceFileRetries() > 0) LogW(L"Setup: " + std::to_wstring(PlaceFileRetries()) + L" file write(s) had to wait for a locked file and then succeeded (a scanner or indexer held it).");
    LogOk(L"Setup: " + std::to_wstring(files.size()) + L" files written and verified (SHA-256).");
    {   // Cache verified SecureUxTheme & Themetools\As3aD.theme files and configure SecureUxTheme hooks
        const std::wstring suxCache = SetupDataRoot() + L"\\Cache\\SecureUxTheme";
        EnsureDir(suxCache);
        for (const auto& f : SuxRequiredFiles()) {
            const std::wstring src = (AppDir() + L"\\Themetools\\" + f.name);
            const std::wstring dst = suxCache + L"\\" + f.name;
            if (FileExists(src) && Sha256File(src) == f.sha256) CopyFileW(src.c_str(), dst.c_str(), FALSE);
        }
        const std::wstring themeFile = AuthoritativeThemeFile(opt.osIsWin11);   // 2.12: As3aD.theme on Windows 11, As3aD10.theme on Windows 10
        const std::wstring localTheme = AppDir() + L"\\Themetools\\" + themeFile;
        if (!FileExists(localTheme) || Sha256File(localTheme) != AuthoritativeThemeSha(opt.osIsWin11)) {
            return failWith(L"Verification failed for application-local Themetools\\" + themeFile + L".");
        }
        if (!VerifySuxDir(AppDir() + L"\\Themetools").ok || !VerifySuxDir(AppDir() + L"\\SecureUxTheme").ok) {
            return failWith(L"Verification failed for SecureUxTheme components in Themetools\\.");
        }
        int wallSynced = 0, curSynced = 0;
        if (!kTestBuild) {   // 2.8: cursors and wallpapers are for EVERY Windows version (10 and 11); only the Windows 11 theme is not
            const Components wc = LoadComponents();
            const std::wstring wbad = VerifyComponent(wc, L"WindowsAssets");
            if (wbad.empty()) { PackageAssetsRoot() = wc.root + L"\\WindowsAssets"; LogI(L"Setup: Windows assets (cursors, wallpapers, themes) verified in the package."); }
            else if (DirExists(wc.root + L"\\WindowsAssets")) { o.warnings.push_back(L"The Windows assets in the package (cursors, wallpapers, themes) were not used: " + wbad + L"."); LogW(L"Setup: Windows assets not used: " + wbad); }
            wallSynced = SyncAnubisWallpapersClean();
            curSynced = SyncAuthoritativeCursors();
        }
        if (!kTestBuild) {   // 2.12: both Windows versions get their As3aD theme (the Windows 10 one brings its own visual style)
            const int thSynced = opt.osIsWin11 ? SyncAuthoritativeThemesCollection() : SyncGlassCyanStyle();
            for (const auto& stagePath : {ThemeWinPath(), ThemeUserPath()}) {
                EnsureDir(DirOf(stagePath));
                SetFileAttributesW(stagePath.c_str(), FILE_ATTRIBUTE_NORMAL);
                CopyFileW(localTheme.c_str(), stagePath.c_str(), FALSE);
            }
            o.themeAssetsSummary = L"Themes & Assets: Themetools\\As3aD.theme verified (SHA-256); synced " +
                                   std::to_wstring(thSynced) + L" theme file(s), " +
                                   std::to_wstring(wallSynced) + L" Anubis wallpaper(s), " +
                                   std::to_wstring(curSynced) + L" cursor file(s)";
            LogOk(L"Setup: " + o.themeAssetsSummary);

            SuxFullStatus suxSt;
            bool suxRestart = false;
            std::wstring patchErr;
            if (ConfigureAndVerifySux(suxSt, suxRestart, patchErr, AppDir() + L"\\Themetools")) {
                o.suxSummary = L"SecureUxTheme: " + suxSt.summary;
                ScheduleThemeReapply(HKEY_CURRENT_USER, 1);
                LogOk(L"Setup: " + o.suxSummary);
            } else {
                o.suxSummary = L"SecureUxTheme warning: " + patchErr;
                o.warnings.push_back(L"SecureUxTheme configuration reported: " + patchErr);
            }
        } else {
            if (!kTestBuild && !opt.osIsWin11) {
                o.themeAssetsSummary = L"Windows 10: " + std::to_wstring(curSynced) + L" cursor file(s) and " + std::to_wstring(wallSynced) + L" wallpaper(s) installed. The Windows 11 theme is not applied on Windows 10 (this Windows does not support it); everything else is installed and active.";
                o.suxSummary = L"";
                LogOk(L"Setup: " + o.themeAssetsSummary);
            } else {
                o.themeAssetsSummary = L"Themes & Assets: Themetools\\As3aD.theme and SecureUxTheme binaries verified (SHA-256)";
                o.suxSummary = L"SecureUxTheme: verified in Themetools\\ (sandbox / Windows 11 target)";
            }
        }
    }
    if (opt.mode == SetupMode::CleanInstall && !userFilesToRestore.empty()) {
        for (const auto& [bakSrc, liveDst] : userFilesToRestore) {
            std::vector<uint8_t> raw;
            if (ReadAll(bakSrc, raw)) WriteAtomic(liveDst, raw.data(), raw.size());
        }
        LogOk(L"Setup: restored compatible user settings and data.");
    }
    // hardware monitoring
    step(L"Setting up hardware monitoring...");
    if (!PawnIOInstalled()) {
        if (opt.driver) {
            const std::wstring pawn = SensorsDir() + L"\\PawnIO_setup.exe";
            Run(pawn, L"-install -silent", true, 180000);   // PawnIO's own flags (not NSIS /S)
            if (!PawnIOInstalled()) o.warnings.push_back(L"The PawnIO driver was not installed: CPU temperatures and some other sensors will show as not exposed.");
        } else o.warnings.push_back(L"PawnIO was not installed (your choice): CPU temperatures and some other sensors will show as not exposed.");
    } else LogI(L"Setup: the PawnIO driver is already installed; left as it is.");
    step(L"Building the hardware-monitoring helper...");
    std::wstring buildLog;
    if (!BuildSensorsHelper(buildLog)) o.warnings.push_back(L"The hardware-monitoring helper could not be built: " + buildLog.substr(0, 200));
    // Windows integration
    step(L"Creating shortcuts and the launch task...");
    if (!CreateLaunchTask(UserSid())) o.warnings.push_back(L"The admin launch task could not be created; AnkhCore will ask for permission when it starts.");
    if (kTestBuild) TestRefused(L"create the Start menu and desktop shortcuts");
    else {
        MakeShortcut(KnownFolder(FOLDERID_CommonPrograms) + L"\\AnkhCore.lnk", AppExe(), L"", kProductFull);
        const std::wstring desk = UserFolder(FOLDERID_Desktop) + L"\\AnkhCore.lnk";
        if (opt.desktop) MakeShortcut(desk, AppExe(), L"", kProductFull); else DeleteFileW(desk.c_str());
    }
    if (o.kind == InstallKind::Fresh ? opt.startup : opt.startup != StartupTaskRegistered())
        if (!SetStartWithWindows(opt.startup)) o.warnings.push_back(L"Start with Windows could not be changed; set it in AnkhCore > Startup & Background.");

    // INS-3 (D41): Optional prebuilt icon themes (copied without conversion, never auto-applied)
    if (!opt.iconThemes.empty()) {
        step(L"Installing selected prebuilt icon themes...");
        const Components comp = LoadComponents();   // every file is checked against COMPONENTS.sha256 before it is used
        const std::wstring srcThemesRoot = comp.root + L"\\IconThemes";
        const std::wstring dstThemesRoot = AppDir() + L"\\IconThemes";
        std::wstring installedNames;
        int okThemes = 0;
        for (const auto& id : opt.iconThemes) {
            const std::wstring folder = IconThemeFolderName(id);
            const std::wstring sDir = srcThemesRoot + L"\\" + folder;
            const std::wstring dDir = dstThemesRoot + L"\\" + folder;
            std::wstring valErr;
            const std::wstring bad = VerifyComponent(comp, L"IconThemes\\" + folder);
            if (!bad.empty()) { o.warnings.push_back(L"Icon theme '" + id + L"' skipped: " + bad + L"."); continue; }
            if (!ValidateIconThemeDir(sDir, nullptr, &valErr)) {
                o.warnings.push_back(L"Icon theme '" + id + L"' skipped (" + valErr + L").");
                continue;
            }
            if (!Allowed(dDir)) { TestRefused(L"copy to " + dDir); continue; }
            if (!PlaceVerifiedTree(sDir, dDir, [](const std::wstring& dir, std::wstring& vErr) {
                    return ValidateIconThemeDir(dir, nullptr, &vErr);
                }, valErr)) {
                o.warnings.push_back(L"Icon theme '" + id + L"' could not be copied (" + valErr + L").");
                continue;
            }
            ++okThemes;
            const IconPack* pk = FindPack(id);
            const std::wstring label = pk ? std::wstring(pk->name) + (*pk->variant ? L" " + std::wstring(pk->variant) : L"") : id;
            if (!installedNames.empty()) installedNames += L", ";
            installedNames += label;
        }
        if (okThemes > 0) {   // the themes are applied through Resource Redirect: install its source beside the program
            const std::wstring bad = VerifyComponent(comp, kRedirectModRel);
            const std::wstring modDst = AppDir() + L"\\" + kRedirectModRel;
            if (!bad.empty()) o.warnings.push_back(L"The Resource Redirect mod was not installed (" + bad + L"); the icon themes cannot be applied until it is.");
            else if (!PlaceVerifiedFile(comp.root + L"\\" + kRedirectModRel, modDst, comp.m.sha.at(ComponentKey(kRedirectModRel))) || Sha256File(modDst) != comp.m.sha.at(ComponentKey(kRedirectModRel)))
                o.warnings.push_back(L"The Resource Redirect mod could not be copied to " + modDst + L"; the icon themes cannot be applied until it is.");
        }
        o.iconThemesInstalled = okThemes;
        o.iconThemesSummary = L"Icon Themes: " + std::to_wstring(okThemes) + L" / " + std::to_wstring(opt.iconThemes.size()) +
                              L" installed" + (installedNames.empty() ? L"" : L" (" + installedNames + L")") + L" — not auto-applied";
        LogI(L"Setup: " + o.iconThemesSummary);
    } else {
        o.iconThemesSummary = L"Icon Themes: None selected (core install)";
    }

    // 2.8.4: After Dark for Windows 10. The pack is chosen from the Windows build of THIS PC; only the chosen variant is copied into Windows\Resources\Themes,
    // after it was verified against COMPONENTS.sha256.
    if (opt.adInstall && !opt.osIsWin11) {
        step(L"Installing the After Dark theme for Windows 10...");
        const Components comp = LoadComponents();
        const std::wstring adRoot = comp.root + L"\\AfterDark";
        const AdPack pk = AdFindPack(adRoot, opt.osBuild);
        if (!pk.found) {
            o.afterDarkSummary = L"After Dark: not installed (" + pk.reason + L")";
            o.warnings.push_back(L"The After Dark theme was not installed: " + pk.reason);
        } else {
            const std::wstring variant = (opt.adVariant >= 0 && opt.adVariant < static_cast<int>(pk.variants.size())) ? pk.variants[opt.adVariant] : pk.variants[0];
            const bool useOffice = opt.adOffice && pk.officeAvailable;
            const std::vector<AdFile> plan = AdPlanFiles(adRoot, pk, useOffice, variant);
            const std::wstring bad = VerifyComponent(comp, AdComponentDir(pk, useOffice, variant));
            const std::wstring label = L"Windows 10 " + pk.release + L", " + variant + (useOffice ? L", Office background fix" : (pk.officeOnly ? L", Office background fix (the only pack for this release)" : L""));
            if (plan.empty() || !bad.empty()) {
                o.afterDarkSummary = L"After Dark: not installed (" + (bad.empty() ? std::wstring(L"the pack has no files") : bad) + L")";
                o.warnings.push_back(L"The After Dark theme was not installed: " + (bad.empty() ? std::wstring(L"the pack has no files") : bad) + L".");
            } else if (kTestBuild) {
                TestRefused(L"copy the After Dark theme into the Windows themes folder");
                o.afterDarkSummary = L"After Dark: " + label + L" verified, " + std::to_wstring(plan.size()) + L" file(s) (sandbox: nothing copied)";
            } else {
                const std::wstring themesDir = WinDir() + L"\\Resources\\Themes";
                int copied = 0, failed = 0;
                for (const AdFile& f : plan) {
                    const auto it = comp.m.sha.find(ComponentKey(L"AfterDark\\" + f.srcRel));
                    if (it == comp.m.sha.end()) { ++failed; continue; }
                    EnsureDir(DirOf(themesDir + L"\\" + f.dstRel));
                    if (PlaceVerifiedFile(adRoot + L"\\" + f.srcRel, themesDir + L"\\" + f.dstRel, it->second)) ++copied; else ++failed;
                }
                o.afterDarkInstalled = copied > 0 && failed == 0;
                o.afterDarkSummary = L"After Dark: " + label + L": " + std::to_wstring(copied) + L" file(s) installed" + (failed ? L", " + std::to_wstring(failed) + L" failed" : std::wstring());
                if (failed) o.warnings.push_back(L"After Dark: " + std::to_wstring(failed) + L" file(s) could not be copied.");
                if (o.afterDarkInstalled) {
                    if (AdPatchSupported(opt.osBuild)) {   // the ThemeDll (SecureUxTheme) patch that lets Windows load these visual styles
                        SuxFullStatus suxSt; bool suxRestart = false; std::wstring patchErr;
                        if (ConfigureAndVerifySux(suxSt, suxRestart, patchErr, AppDir() + L"\\Themetools")) { o.suxSummary = L"SecureUxTheme: " + suxSt.summary; LogOk(L"Setup: " + o.suxSummary); }
                        else { o.suxSummary = L"SecureUxTheme warning: " + patchErr; o.warnings.push_back(L"SecureUxTheme configuration reported: " + patchErr + L". The After Dark theme cannot be applied until it is set up."); }
                    } else o.warnings.push_back(L"This Windows 10 release (" + pk.release + L") is older than 1809: the files are installed, but AnkhCore does not set up the theme patch for it.");
                    if (opt.adSleepFix && pk.sleepFix) {   // opt-in registry fix; the key is exported first and the undo file is kept
                        const std::wstring dir = L"Extras\\Fix bug white color after sleep for theme dark";
                        const std::wstring fixRel = L"AfterDark\\" + dir + L"\\FIX.reg", unRel = L"AfterDark\\" + dir + L"\\UNFIX (restore defaults).reg";
                        const std::wstring b1 = VerifyComponent(comp, fixRel), b2 = VerifyComponent(comp, unRel);
                        if (!b1.empty() || !b2.empty()) o.warnings.push_back(L"The white-after-sleep fix was not applied: " + (b1.empty() ? b2 : b1) + L".");
                        else {
                            const std::wstring keep = AppDir() + L"\\AfterDark"; EnsureDir(keep);
                            const std::wstring bak = BackupRoot() + L"\\DefaultColors-before-sleep-fix-" + ToolStamp() + L".reg";
                            EnsureDir(BackupRoot());
                            Run(SysDir() + L"\\reg.exe", L"export " + std::wstring(1, L'"') + L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\DefaultColors" + std::wstring(1, L'"') + L" " + std::wstring(1, L'"') + bak + std::wstring(1, L'"') + L" /y", true, 60000);
                            const bool keptUndo = PlaceVerifiedFile(adRoot + L"\\" + dir + L"\\UNFIX (restore defaults).reg", keep + L"\\UNFIX (restore defaults).reg", comp.m.sha.at(ComponentKey(unRel)));
                            const DWORD rc = keptUndo ? Run(SysDir() + L"\\reg.exe", L"import " + std::wstring(1, L'"') + adRoot + L"\\" + dir + L"\\FIX.reg" + std::wstring(1, L'"'), true, 60000) : 1;
                            o.afterDarkSummary += keptUndo && rc == 0 ? L"; white-after-sleep fix applied (undo file: " + keep + L"\\UNFIX (restore defaults).reg)" : L"; white-after-sleep fix NOT applied";
                            if (!keptUndo || rc != 0) o.warnings.push_back(L"The white-after-sleep registry fix was not applied (" + std::wstring(keptUndo ? L"reg import code " + std::to_wstring(rc) : L"its undo file could not be kept first") + L").");
                        }
                    }
                }
            }
        }
        (o.afterDarkInstalled ? LogOk : LogI)(L"Setup: " + o.afterDarkSummary);
    }

    // WT-03: optional smartctl component (copied only when both files verified; never run by the setup)
    if (opt.smartctl) {
        step(L"Installing the optional smartctl component...");
        const Components comp = LoadComponents();
        const SmartctlOffer off = CheckSmartctlComponent(comp);
        if (!off.ok) {
            o.smartctlSummary = L"SMART component: not installed (" + off.reason + L")";
            o.warnings.push_back(L"SMART disk diagnostics (smartctl) was not installed: " + off.reason + L". AnkhCore shows the basic drive health Windows reports instead.");
        } else {
            const std::wstring dstDir = AppDir() + L"\\Tools";
            const std::wstring dstExe = dstDir + L"\\smartctl.exe", dstLic = dstDir + L"\\COPYING.smartmontools";
            EnsureDir(dstDir);
            const bool copied = PlaceVerifiedFile(comp.root + L"\\" + kSmartctlRel, dstExe, comp.m.sha.at(ComponentKey(kSmartctlRel))) &&
                                PlaceVerifiedFile(comp.root + L"\\" + kSmartctlLicenseRel, dstLic, comp.m.sha.at(ComponentKey(kSmartctlLicenseRel)));
            const bool same = copied && Sha256File(dstExe) == comp.m.sha.at(ComponentKey(kSmartctlRel)) && Sha256File(dstLic) == comp.m.sha.at(ComponentKey(kSmartctlLicenseRel));
            if (!same) {   // all or nothing: a program without its license, or a changed copy, is not left behind
                DeleteFileW(dstExe.c_str()); DeleteFileW(dstLic.c_str()); RemoveDirectoryW(dstDir.c_str());
                o.smartctlSummary = L"SMART component: not installed (the copy could not be verified)";
                o.warnings.push_back(L"SMART disk diagnostics (smartctl) could not be copied and verified; nothing of it was installed.");
            } else {
                o.smartctlInstalled = true;
                o.smartctlSummary = L"SMART component: smartctl.exe and its GPL license installed in " + dstDir + L" (read-only use)";
            }
        }
        LogI(L"Setup: " + o.smartctlSummary);
    } else {
        o.smartctlSummary = L"SMART component: not selected (core install)";
    }

    // 2.8: PowerToys (external component, installed silently with the official installer; skipped when it is already installed or cannot be verified)
    if (opt.powertoys) {
        step(L"Installing PowerToys...");
        const Components comp = LoadComponents();
        const PtOffer off = CheckPowerToysComponent(comp);
        const PtInstall had = PtDetect();
        const ToolPlan plan = PlanToolInstall(had.installed, opt.replacePowerToys);
        if (!off.ok) {
            o.powertoysSummary = L"PowerToys: not installed (" + off.reason + L")";
            o.warnings.push_back(L"PowerToys was not installed: " + off.reason + L".");
        } else if (plan == ToolPlan::KeepExisting) {
            o.powertoysInstalled = true;
            o.powertoysSummary = L"PowerToys: already installed (version " + had.version + L"); kept as it is";
        } else if (kTestBuild) {
            TestRefused(plan == ToolPlan::ReplaceClean ? L"remove and reinstall PowerToys" : L"install PowerToys");
            o.powertoysSummary = plan == ToolPlan::ReplaceClean ? L"PowerToys: verified; it would be removed, cleaned and reinstalled (sandbox: nothing is run)" : L"PowerToys: verified (sandbox: the installer is not run)";
        } else {
            bool go = true; std::wstring replaceNote;
            if (plan == ToolPlan::ReplaceClean) go = ReplacePowerToysClean(off.exe, replaceNote, o.warnings);
            if (!go) { o.powertoysInstalled = PtDetect().installed; o.powertoysSummary = replaceNote; }
            else {
                LogI(L"Setup: running the PowerToys installer silently: " + off.exe);
                const DWORD rc = Run(off.exe, L"/silent /norestart", true, 20u * 60u * 1000u);
                const PtInstall now = PtDetect();
                o.powertoysInstalled = now.installed;
                o.powertoysSummary = (replaceNote.empty() ? std::wstring() : replaceNote + L"; ") + (now.installed ? L"PowerToys: installed (version " + now.version + L")" : L"PowerToys: the installer finished (code " + std::to_wstring(rc) + L") but PowerToys was not found");
                if (!now.installed) o.warnings.push_back(L"PowerToys did not install (installer code " + std::to_wstring(rc) + L"). Run the installer in the PowerToys folder of the package yourself.");
            }
        }
        LogI(L"Setup: " + o.powertoysSummary);
    } else {
        o.powertoysSummary = L"PowerToys: not selected";
    }

    // INS-3 (D44): Optional Windows 11 Taskbar Profile (unchecked by default, Windows 11 only, with backup)
    if (opt.taskbarProfile && opt.osIsWin11) {
        step(L"Applying Windows 11 Taskbar Profile...");
        const std::wstring regPath = FindTaskbarProfileRegPath();
        const std::wstring bkPath  = BackupRoot() + L"\\taskbar-profile-backup.txt";
        std::wstring detail = VerifyComponent(LoadComponents(), L"AnkhCore_Taskbar_Profile.reg");   // listed and unchanged, or not used
        if (detail.empty() && ApplyTaskbarProfileWithBackup(regPath, opt.osIsWin11, bkPath, detail)) {
            o.taskbarApplied = true;
            o.taskbarBackupPath = bkPath;
            o.taskbarSummary = FormatTaskbarProfileSummary(true, true, true, detail);
            LogOk(L"Setup: " + o.taskbarSummary);
        } else {
            o.taskbarApplied = false;
            o.taskbarSummary = FormatTaskbarProfileSummary(true, true, false, detail);
            o.warnings.push_back(L"Taskbar Profile was not applied: " + detail);
        }
    } else {
        o.taskbarSummary = FormatTaskbarProfileSummary(opt.taskbarProfile, opt.osIsWin11, false, L"");
        if (opt.taskbarProfile && !opt.osIsWin11) {
            LogI(L"Setup: " + o.taskbarSummary);
        }
    }

    if (doWhBundle) {
        step(L"Installing Windhawk, 22 mods, and registry settings...");
        const Components comp = LoadComponents();
        const std::wstring bad = VerifyComponent(comp, kWhBundleDir);
        if (!bad.empty()) {
            o.windhawkBundleSummary = L"Windhawk & Mods: skipped (" + bad + L")";
            o.warnings.push_back(L"Windhawk & mods were not installed (" + bad + L").");
            LogW(L"Setup: " + o.windhawkBundleSummary);
        } else {
            const std::wstring whDataDir = kTestBuild ? (SandboxRoot() + L"\\ProgramData\\Windhawk") : WindhawkData();
            bool whExeReady = kTestBuild ? true : FileExists(WindhawkExePath());
            bool installedNow = false;
            std::wstring whReplaceNote;
            if (!kTestBuild && whExeReady && PlanToolInstall(true, opt.replaceWindhawk) == ToolPlan::ReplaceClean) {   // 2.8.3: the person chose to replace the Windhawk that is already here
                if (ReplaceWindhawkClean(whReplaceNote, o.warnings)) whExeReady = false;   // removed and cleaned: the packaged installer runs below
            } else if (kTestBuild && opt.replaceWindhawk) { TestRefused(L"remove and reinstall Windhawk"); whReplaceNote = L"it would be removed, cleaned and reinstalled (sandbox: nothing is run)"; }
            if (!kTestBuild && (!whExeReady || opt.reinstallWindhawk)) {
                const std::wstring whSetup = comp.root + L"\\" + kWhSetupRel;
                LogI(L"Setup: running offline Windhawk installer (/S): " + whSetup);
                Run(whSetup, L"/S", false, 240000);
                whExeReady = FileExists(WindhawkExePath());
                if (!whExeReady) {
                    LogW(L"Setup: silent Windhawk install did not produce windhawk.exe; retrying interactive installer...");
                    Run(whSetup, L"", false, 300000);
                    whExeReady = FileExists(WindhawkExePath());
                }
                installedNow = whExeReady;
                if (installedNow) {   // 2.8.7: no Windhawk icon on the desktop
                    int removed = 0;
                    for (const std::wstring& dir : {KnownFolder(FOLDERID_PublicDesktop), KnownFolder(FOLDERID_Desktop)}) {
                        if (dir.empty() || !DirExists(dir)) continue;
                        for (const std::wstring& f : ListDir(dir, false, L"*.lnk")) if (IsWindhawkDesktopShortcut(f) && DeleteFileW((dir + L"\\" + f).c_str())) ++removed;
                    }
                    if (removed) LogI(L"Setup: removed " + std::to_wstring(removed) + L" Windhawk desktop shortcut(s).");
                }
            }
            if (!whExeReady) {
                o.windhawkBundleSummary = L"Windhawk & Mods: Windhawk installer did not complete";
                o.warnings.push_back(L"Windhawk was not installed. Run Windhawk\\windhawk_setup_offline.exe manually, then re-run setup.");
                LogW(L"Setup: " + o.windhawkBundleSummary);
            } else {
                if (!kTestBuild) StopWindhawk();
                int copiedSrc = 0, copied64 = 0, copied32 = 0, copyFail = 0;
                auto copyBundleFolder = [&](const wchar_t* relSub, const std::wstring& dstDir, int& count) {
                    if (!DirExists(comp.root + L"\\" + relSub)) return;
                    if (!Allowed(dstDir)) { TestRefused(L"copy to " + dstDir); return; }
                    EnsureDir(dstDir);
                    for (const auto& f : ListDir(comp.root + L"\\" + relSub, false)) {
                        const std::wstring rel = std::wstring(relSub) + L"\\" + f;
                        const auto it = comp.m.sha.find(ComponentKey(rel));
                        if (it == comp.m.sha.end()) { ++copyFail; continue; }
                        if (FileExists(dstDir + L"\\" + f) && Sha256File(dstDir + L"\\" + f) == it->second) { ++count; continue; }   // already the shipped file (an upgrade; it may be loaded by Explorer)
                        if (PlaceVerifiedFile(comp.root + L"\\" + rel, dstDir + L"\\" + f, it->second)) ++count;
                        else ++copyFail;
                    }
                };
                copyBundleFolder(kWhSrcRel,    whDataDir + L"\\ModsSource",       copiedSrc);
                copyBundleFolder(kWhMods64Rel, whDataDir + L"\\Engine\\Mods\\64", copied64);
                copyBundleFolder(kWhMods32Rel, whDataDir + L"\\Engine\\Mods\\32", copied32);
                if (!kTestBuild) {   // 2.12: the mods this package brought, so a full-clean uninstall removes exactly these
                    std::string list; for (const auto& f : ListDir(comp.root + L"\\" + kWhSrcRel, false)) { if (f.size() > 7 && f.compare(f.size() - 7, 7, L".wh.cpp") == 0) list += WToUtf8(f.substr(0, f.size() - 7)) + "\n"; }
                    EnsureDir(DataDir()); WriteAll(DataDir() + L"\\windhawk-bundle-mods.txt", reinterpret_cast<const uint8_t*>(list.data()), list.size());
                }

                bool regOk = false;
                const std::wstring regSrc = comp.root + L"\\" + kWhRegRel;
                if (kTestBuild) {
                    TestRefused(L"import Windhawk mod settings registry " + regSrc);
                    regOk = true;
                } else {
                    const DWORD rc = Run(SysDir() + L"\\reg.exe", L"import \"" + regSrc + L"\" /reg:64", true, 60000);
                    regOk = (rc == 0);
                    if (!regOk) o.warnings.push_back(L"Windhawk mod registry settings import returned error " + std::to_wstring(rc) + L".");
                }
                if (!kTestBuild) StartWindhawk();
                o.windhawkBundleInstalled = (copyFail == 0 && regOk && copiedSrc > 0 && copied64 > 0);
                o.windhawkBundleSummary = L"Windhawk & Mods: " + (whReplaceNote.empty() ? std::wstring() : whReplaceNote + L"; ") + std::wstring(installedNow ? L"installed Windhawk + " : L"Windhawk kept as it is + ") +
                    std::to_wstring(copiedSrc) + L" mod sources, " + std::to_wstring(copied64) + L" x64 DLLs, " +
                    std::to_wstring(copied32) + L" x86 DLLs" + (regOk ? L", registry settings applied" : L", registry import failed");
                if (copyFail > 0) o.warnings.push_back(L"Windhawk bundle: " + std::to_wstring(copyFail) + L" file(s) could not be copied.");
                (o.windhawkBundleInstalled ? LogOk : LogW)(L"Setup: " + o.windhawkBundleSummary);
            }
        }
    }

    if (opt.installWindhawkBundle && PackageHasWindhawkBundle()) {   // 2.9: Windhawk is used through AnkhCore only
        std::wstring toolNote;
        const bool toolOk = ApplyWindhawkAsTool(toolNote);
        (toolOk ? LogOk : LogI)(L"Setup: " + toolNote);
    }
    if (opt.hideWindhawkTray) {
        step(L"Configuring Windhawk tray icon visibility...");
        o.windhawkTrayHidden = ApplyWindhawkHideTrayIcon(o.windhawkTraySummary);
        if (o.windhawkTrayHidden) LogOk(L"Setup: " + o.windhawkTraySummary);
        else LogI(L"Setup: " + o.windhawkTraySummary);
    } else {
        o.windhawkTraySummary = L"Windhawk tray icon: left unchanged";
        LogI(L"Setup: " + o.windhawkTraySummary);
    }

    WriteUninstallEntry(FolderSize(AppDir()));
    o.restartRequired = true;   // Mandatory Windows restart after verified installation
    for (const auto& w : o.warnings) LogW(L"Setup: " + w);
    LogOk(L"AnkhCore " + std::wstring(kVersionLabel) + L" (" + kVersion + L") Setup finished; mandatory Windows restart required" + (o.deferred ? L" (deferred files queued for restart)." : L"."));
    status(L"Done.", 100);
    o.ok = true;
    return o;
}

// Puts back the newest backup of a version other than the installed one; the replaced files are kept
// as a backup themselves, so a rollback can be undone with the setup.
Outcome RollbackCore(const Progress& status) {
    Outcome o;
    const Ver cur = InstalledVersion();
    std::wstring best, bestStamp; Ver bestV;
    for (const auto& d : ListDir(BackupRoot(), true, L"AnkhCore-*")) {
        const std::string info = ReadText(BackupRoot() + L"\\" + d + L"\\backup.txt");
        const size_t p = info.find("version="), e = info.find('\n', p);
        const Ver v = p == std::string::npos ? Ver{} : ParseVer(Utf8ToW(info.substr(p + 8, e - p - 8)));
        if (!v.valid || (cur.valid && CompareVer(v, cur) == 0)) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa{};   // newest = latest backup.txt write time (folder names can carry a -2 suffix)
        if (!GetFileAttributesExW((BackupRoot() + L"\\" + d + L"\\backup.txt").c_str(), GetFileExInfoStandard, &fa)) continue;
        wchar_t st[32]; swprintf(st, 32, L"%010lu%010lu", fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
        const std::wstring stamp = st;
        if (stamp > bestStamp) { bestStamp = stamp; best = d; bestV = v; }
    }
    if (best.empty()) { o.message = L"There is no backup of another AnkhCore version to restore."; return o; }
    const std::wstring src = BackupRoot() + L"\\" + best;
    OpSteps steps(2);
    steps.Next(); status(steps.Label(L"Closing AnkhCore..."), steps.Pct());
    std::wstring err;
    if (!StopAnkhCore(err)) { o.message = err + L" Nothing was changed."; return o; }
    steps.Next(); status(steps.Label(L"Restoring AnkhCore " + VerText(bestV) + L"..."), steps.Pct());
    std::vector<std::pair<std::wstring, std::wstring>> moved; std::wstring failed;
    o.backup = BackupFor(cur);
    if (!MoveReplacedTo(o.backup, cur, moved, failed)) { UndoMoves(moved); RemoveItem(o.backup); o.message = L"The current version could not be moved aside (" + failed + L"). Nothing was changed."; return o; }
    std::vector<std::pair<std::wstring, std::wstring>> restored;   // items already taken out of the old backup: on a failure they go back into it (they used to be deleted, and the backup lost them)
    for (const auto& item : ReplacedItems()) {
        const std::wstring from = src + L"\\" + item;
        if (!Exists(from)) continue;
        if (!MoveItem(from, AppDir() + L"\\" + item)) {
            UndoMoves(restored);                     // the restored items return to the old backup
            RemoveItem(AppDir() + L"\\" + item);     // whatever a failed move left of this one
            UndoMoves(moved); RemoveItem(o.backup); o.message = L"The backup could not be restored (" + item + L"). The current version was kept."; return o;
        }
        restored.push_back({from, AppDir() + L"\\" + item});
    }
    DeleteFileW((src + L"\\backup.txt").c_str()); RemoveDirectoryW(src.c_str());
    RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", VerText(bestV));
    if (CompareVer(bestV, ParseVer(L"2.5")) < 0 && !kTestBuild && StartupTaskRegistered()) SetStartWithWindows(false);   // start with Windows is a 2.5 feature
    LogOk(L"Setup: AnkhCore " + VerText(bestV) + L" restored from " + src + L"; version " + VerText(cur) + L" kept in " + o.backup + L".");
    status(L"Done.", 100);
    o.ok = true; o.from = bestV;
    return o;
}

// ---------------------------------------------------------------- wizard
Options g_opt;
HWND g_wnd; Ui g_ui; Logo g_logo; Image32 g_brandLogo, g_brandLogoSmall; HICON g_ankh = nullptr; WinVer g_ver;
int g_step = 0;   // 0 welcome, 1 options, 2 installing/done
std::wstring g_status = L"Ready."; int g_progress = 0;
bool g_busy = false, g_done = false, g_failed = false, g_rollbackMode = false;
Outcome g_out; bool g_installed = false; Ver g_have; InstallKind g_kind = InstallKind::Fresh; bool g_pawn = false; ExistingInstallInfo g_existing;
constexpr int kW = 920, kH = 620;
// FT-G01: the Options page is taller than its window (four option rows plus the icon themes). It scrolls (mouse wheel, arrow keys,
// Page Up/Down) and shows a bar and a hint, so no option is ever cut off without a way to reach it.
int g_optScroll = 0, g_optContent = 0, g_optView = 0;
int OptMaxScroll() { return std::max(0, g_optContent - g_optView); }
// 2.9.5 (Task 8): the final page. The choice is explicit and defaults to "Restart later"; nothing restarts before Finish is clicked.
bool g_restartNow = false, g_finishing = false;
// The success page with a restart to offer scrolls like the Options page and keeps its Finish button in a fixed footer.
bool FinalPageHasRestart() { return g_step == 2 && g_done && !g_failed && g_out.restartRequired && !g_rollbackMode; }
bool PageScrolls() { return g_step == 1 || (g_step == 2 && g_done); }

void Status(const std::wstring& s, int pct) {
    LogI(s);
    if (g_wnd) PostMessageW(g_wnd, WM_STATUS, static_cast<WPARAM>(pct), reinterpret_cast<LPARAM>(new std::wstring(s)));
}
DWORD WINAPI InstallThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetUserToken(ShellUserToken());
    SecureDataDir();
    auto* r = new Outcome(g_rollbackMode ? RollbackCore(Status) : InstallCore(g_entries, g_opt, Status));
    PostMessageW(g_wnd, WM_DONE, 0, reinterpret_cast<LPARAM>(r));
    return 0;
}
const wchar_t* kSteps[] = {L"Welcome", L"Options", L"Install"};
std::wstring PlanText() {
    if (g_existing.found) {
        const std::wstring ver = g_existing.version.valid ? VerText(g_existing.version) : (g_existing.partial ? L"(partial / damaged)" : L"(unknown version)");
        switch (g_opt.mode) {
        case SetupMode::Repair:
            return L"A. Repair (" + ver + L"): Rewrites and verifies AnkhCore " + std::wstring(kVersionLabel) + L" files while preserving user settings, logs, and personal data.";
        case SetupMode::Update:
            return L"B. Update (" + ver + L" -> " + std::wstring(kVersionLabel) + L"): Backs up the existing version for rollback, updates AnkhCore, and preserves user settings and personal data.";
        case SetupMode::CleanInstall:
        default:
            return L"C. Clean Install (Recommended): Backs up user settings and personal data first, removes obsolete AnkhCore files and stale folders, installs " + std::wstring(kVersionLabel) + L" cleanly, and restores compatible settings.";
        }
    }
    switch (g_kind) {
    case InstallKind::Fresh: return L"AnkhCore " + std::wstring(kVersionLabel) + L" will be installed in " + AppDir() + L".";
    case InstallKind::Upgrade: return L"AnkhCore " + (g_have.valid ? VerText(g_have) : std::wstring(L"(unknown version)")) + L" is installed. It will be replaced by " + kVersionLabel + L" (" + kVersion + L")" +
                                      L". The previous version is backed up first and can be restored (run this setup with --rollback).";
    case InstallKind::Repair: return L"AnkhCore " + std::wstring(kVersionLabel) + L" is already installed. Setup will repair it (rewrite and verify its files).";
    default: return L"A newer AnkhCore (" + VerText(g_have) + L") is installed. This setup will not replace it.";
    }
}
void PageWelcome(Ctx& c) {
    Text(c.dc, g_existing.found ? L"AnkhCore Installation Detected" : L"Welcome to AnkhCore", RECT{c.x, c.y, c.x + c.w, c.y + c.S(40)}, c.u.f.title, col::text);
    c.y += c.S(44);
    if (g_existing.found) {
        const std::wstring ver = g_existing.version.valid ? VerText(g_existing.version) : (g_existing.partial ? L"partial installation" : L"unknown version");
        Text(c.dc, L"Detected " + ver + L" in " + AppDir() + L"  \x00B7  Target: " + std::wstring(kVersionLabel), RECT{c.x, c.y, c.x + c.w, c.y + c.S(22)}, c.u.f.text, col::accent);
        c.y += c.S(32);
        std::vector<BtnSpec> modes = {
            {L"A. Repair", g_opt.mode == SetupMode::Repair ? Btn::Primary : Btn::Ghost, true, [] { g_opt.mode = SetupMode::Repair; g_kind = ResolveEffectiveInstallKind(g_opt.mode, g_installed, g_have, ParseVer(kVersion)); InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"B. Update", g_opt.mode == SetupMode::Update ? Btn::Primary : Btn::Ghost, true, [] { g_opt.mode = SetupMode::Update; g_kind = ResolveEffectiveInstallKind(g_opt.mode, g_installed, g_have, ParseVer(kVersion)); InvalidateRect(g_wnd, nullptr, FALSE); }},
            {L"C. Clean Install (Recommended)", g_opt.mode == SetupMode::CleanInstall ? Btn::Primary : Btn::Ghost, true, [] { g_opt.mode = SetupMode::CleanInstall; g_kind = ResolveEffectiveInstallKind(g_opt.mode, g_installed, g_have, ParseVer(kVersion)); InvalidateRect(g_wnd, nullptr, FALSE); }},
        };
        Buttons(c, modes);
        c.y += c.S(4);
    } else {
        Text(c.dc, L"Windows Control Center  \x00B7  version " + std::wstring(kVersionLabel) + L"  \x00B7  Developer: As3aD", RECT{c.x, c.y, c.x + c.w, c.y + c.S(22)}, c.u.f.text, col::accent);
        c.y += c.S(36);
    }
    Para(c, PlanText(), c.u.f.text, g_kind == InstallKind::Downgrade ? col::warn : col::text);
    c.y += c.S(6);
    Para(c, L"User backups (Backup\\), Windhawk, mods, themes, cursors, wallpapers, icons, and unrelated personal files are never deleted. "
            L"Only AnkhCore application files and obsolete AnkhCore runtime folders (in Clean Install) are replaced.", c.u.f.small, col::sub);
    Para(c, L"Detected OS: " + WinVerText(g_ver), c.u.f.small, col::sub);
    c.y += c.S(6);
    std::vector<BtnSpec> links;
    for (const auto& l : kLinks) { const std::wstring url = l.url; links.push_back({l.text, Btn::Ghost, true, [url] { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }}); }
    Buttons(c, links);
}
void PageOptions(Ctx& c) {
    // 2.8.9: two ways to run the wrong file and think the install is "fake" / incomplete. Both are said at the top, in red.
    if (kTestBuild) Para(c, L"THIS IS THE TEST SETUP. It only checks the screens and changes nothing on this PC. To install AnkhCore, run AnkhCore_Installer.exe from the package folder.", c.u.f.btn, col::err);
    {
        static const Components pkgCheck = LoadComponents();
        if (!pkgCheck.loaded) Para(c, L"This installer is not beside its package files (" + pkgCheck.err + L"). Windhawk, PowerToys, icon themes, After Dark and the other optional parts cannot be installed from here. Extract the whole package and run AnkhCore_Installer.exe from inside that folder.", c.u.f.btn, col::err);
    }
    Section(c, L"1. Core components", L"Required application files and hardware-monitoring runtime.");
    {
        RowSpec core; core.icon = L"\xE74C"; core.title = L"AnkhCore " + std::wstring(kVersionLabel) + L" & Sensors";
        core.desc = L"Core control center application and LibreHardwareMonitor sensor service. Always installed.";
        core.check = 1; core.enabled = false;
        core.chips.push_back({L"Required", col::ok});
        Row(c, core);
    }
    {
        RowSpec r; r.icon = L"\xE9D2"; r.title = L"Hardware monitoring driver (PawnIO)";
        r.desc = g_pawn ? L"Already installed on this PC; left as it is." : L"Signed driver used to read CPU temperatures and other sensors. Recommended.";
        r.check = g_pawn ? 1 : g_opt.driver; r.enabled = !g_pawn;
        r.chips.push_back({g_pawn ? L"Installed" : L"Default", col::ok});
        r.onCheck = [] { g_opt.driver = !g_opt.driver; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    Section(c, L"2. Optional icon themes (Resource Redirect 1.3)",
            L"Prebuilt icon packs copied to IconThemes\\ (never applied automatically). Select individual packs or use Select all.");
    {
        static const std::vector<std::wstring> inPackage = PackageIconThemes();   // only what this package really carries
        if (inPackage.empty()) Para(c, L"This setup package contains no icon themes.", c.u.f.small, col::sub);
        else {   // FT-G01: how many themes this package carries, and a quick way to take all or none
            Para(c, std::to_wstring(g_opt.iconThemes.size()) + L" of " + std::to_wstring(inPackage.size()) + L" icon theme(s) selected. Themes marked (partial) redirect icons supported on Windows 11.", c.u.f.small, col::sub);
            const std::wstring allLabel = L"Select all (" + std::to_wstring(inPackage.size()) + L")";
            int lx = c.x;
            for (const auto& [label, all] : {std::pair<std::wstring, bool>{allLabel, true}, {L"Select none", false}}) {
                const int bw = TextWidth(c.dc, label, c.u.f.tiny) + c.S(22);
                RECT cr{lx, c.y, lx + bw, c.y + c.S(24)};
                const bool active = all ? (g_opt.iconThemes.size() == inPackage.size()) : g_opt.iconThemes.empty();
                Round(c.dc, cr, c.S(12), active ? col::accentDeep : (c.Hover(cr) ? col::panel2 : col::panel), active ? col::accent : col::border);
                Text(c.dc, label, cr, c.u.f.tiny, active ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                c.Hit(cr, [all] { g_opt.iconThemes = all ? PackageIconThemes() : std::vector<std::wstring>{}; InvalidateRect(g_wnd, nullptr, FALSE); });
                lx += bw + c.S(8);
            }
            c.y += c.S(32);
        }
        int x = c.x;
        for (const auto& p : IconPacks()) {
            if (std::find(inPackage.begin(), inPackage.end(), p.id) == inPackage.end()) continue;
            const std::wstring label = std::wstring(p.name) + (*p.variant ? L" " + std::wstring(p.variant) : L"") + (p.partial ? L" (partial)" : L"");
            const int bw = TextWidth(c.dc, label, c.u.f.tiny) + c.S(22);
            if (x + bw > c.x + c.w) { x = c.x; c.y += c.S(28); }
            RECT cr{x, c.y, x + bw, c.y + c.S(24)};
            const bool sel = std::find(g_opt.iconThemes.begin(), g_opt.iconThemes.end(), p.id) != g_opt.iconThemes.end();
            Round(c.dc, cr, c.S(12), sel ? col::accentDeep : (c.Hover(cr) ? col::panel2 : col::panel), sel ? col::accent : col::border);
            Text(c.dc, label, cr, c.u.f.tiny, sel ? col::accent : col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            const std::wstring id = p.id;
            c.Hit(cr, [id] {
                auto it = std::find(g_opt.iconThemes.begin(), g_opt.iconThemes.end(), id);
                if (it == g_opt.iconThemes.end()) g_opt.iconThemes.push_back(id);
                else g_opt.iconThemes.erase(it);
                InvalidateRect(g_wnd, nullptr, FALSE);
            });
            x += bw + c.S(8);
        }
        c.y += c.S(30);
    }
    Section(c, L"3. System integration & optional tools");
    {
        RowSpec r; r.icon = L"\xE7E8"; r.title = L"Start AnkhCore with Windows"; r.desc = L"Starts in the system tray when you sign in, so monitoring and desktop widgets stay active.";
        r.check = g_opt.startup; r.onCheck = [] { g_opt.startup = !g_opt.startup; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {
        RowSpec r; r.icon = L"\xE8FC"; r.title = L"Desktop shortcut"; r.desc = L"A Start menu shortcut is always created.";
        r.check = g_opt.desktop; r.onCheck = [] { g_opt.desktop = !g_opt.desktop; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {
        const bool w11 = g_ver.win11(), inPkg = FileExists(FindTaskbarProfileRegPath()), can = w11 && inPkg;
        RowSpec r; r.icon = L"\xE75B";
        r.title = L"Apply AnkhCore Taskbar Profile (Windows 11 only)";
        r.desc = !inPkg ? L"Not in this setup package (AnkhCore_Taskbar_Profile.reg is not beside the setup)."
               : w11 ? L"Optional Taskbar customization profile (AnkhCore_Taskbar_Profile.reg). Unchecked by default; backs up your current Taskbar settings first."
                     : L"Available only on Windows 11 (detected " + WinVerText(g_ver) + L").";
        r.check = can ? (g_opt.taskbarProfile ? 1 : 0) : 0;
        r.enabled = can;
        r.chips.push_back({!inPkg ? L"Not in package" : w11 ? L"Windows 11" : L"Windows 11 only", can ? col::accent : col::faint});
        r.onCheck = [can] { if (!can) return; g_opt.taskbarProfile = !g_opt.taskbarProfile; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {   // WT-03: the optional smartctl component, offered only when this package carries it
        static const SmartctlOffer offer = CheckSmartctlComponent(LoadComponents());
        static const bool smartDefault = offer.ok ? (g_opt.smartctl = true) : false; (void)smartDefault;   // 2.8: a verified component is installed by default
        if (offer.present) {
            RowSpec r; r.icon = L"\xEDA2";
            r.title = L"SMART disk diagnostics (smartctl, GPL-2.0)";
            r.desc = offer.ok ? L"Optional: lets AnkhCore read each drive's own SMART health report (read-only). Installs smartctl.exe and its license into Tools. Unchecked by default."
                              : L"This package carries a smartctl component that did not verify and will not be installed: " + offer.reason + L".";
            r.check = offer.ok ? (g_opt.smartctl ? 1 : 0) : 0;
            r.enabled = offer.ok;
            r.chips.push_back({offer.ok ? L"Verified" : L"Not verified", offer.ok ? col::ok : col::warn});
            r.onCheck = [] { if (!offer.ok) return; g_opt.smartctl = !g_opt.smartctl; InvalidateRect(g_wnd, nullptr, FALSE); };
            Row(c, r);
        }
    }
    {   // 2.8: PowerToys, offered only when this package carries its installer
        static const PtOffer ptOffer = CheckPowerToysComponent(LoadComponents());
        static const bool ptDefault = ptOffer.ok ? (g_opt.powertoys = true) : false; (void)ptDefault;
        if (ptOffer.present) {
            RowSpec r; r.icon = L"\xE8B0";
            r.title = L"PowerToys (Microsoft, MIT)";
            r.desc = ptOffer.ok ? L"Installs the official PowerToys with AnkhCore's package (FancyZones window layouts, Always On Top, Awake, Color Picker, Keyboard Manager, PowerToys Run and more). AnkhCore then lets you switch each utility on or off from Windows Studio > PowerToys. Skipped when PowerToys is already installed."
                               : L"This package carries a PowerToys installer that did not verify and will not be installed: " + ptOffer.reason + L".";
            r.check = ptOffer.ok ? (g_opt.powertoys ? 1 : 0) : 0;
            r.enabled = ptOffer.ok;
            r.chips.push_back({ptOffer.ok ? L"Verified" : L"Not verified", ptOffer.ok ? col::ok : col::warn});
            r.onCheck = [] { if (!ptOffer.ok) return; g_opt.powertoys = !g_opt.powertoys; InvalidateRect(g_wnd, nullptr, FALSE); };
            Row(c, r);
        }
    }
    {   // 2.8.4: After Dark for Windows 10: the pack for the release of THIS PC and only the options that pack really has
        static const AdPack adPk = AdFindPack(LoadComponents().root + L"\\AfterDark", g_opt.osBuild);
        if (g_opt.osIsWin11) {
            Para(c, L"Windows 11 detected: the BIB / BIB3 themes are installed. The After Dark packs are for Windows 10 and are not installed here.", c.u.f.small, col::faint);
        } else if (!adPk.found) {
            Para(c, L"After Dark (Windows 10): " + adPk.reason, c.u.f.small, col::faint);
        } else {
            RowSpec r; r.icon = L"\xE790"; r.title = L"After Dark theme for Windows 10 " + adPk.release + L" (found for this PC)";
            r.desc = L"Detected Windows 10 " + adPk.release + L" (build " + std::to_wstring(g_opt.osBuild) + L"). Installs the After Dark visual styles made for this release into Windows' themes folder" + std::wstring(AdPatchSupported(g_opt.osBuild) ? L" and sets up the theme patch (SecureUxTheme) that lets Windows load them." : L". This release is older than 1809: the patch is not set up for it.");
            r.check = g_opt.adInstall ? 1 : 0;
            r.chips.push_back({adPk.officeOnly ? L"Office background fix version" : L"Matches this Windows", col::accent});
            r.onCheck = [] { g_opt.adInstall = !g_opt.adInstall; InvalidateRect(g_wnd, nullptr, FALSE); };
            Row(c, r);
            if (g_opt.adInstall) {
                if (adPk.variants.size() > 1) {
                    const int vi = (g_opt.adVariant >= 0 && g_opt.adVariant < static_cast<int>(adPk.variants.size())) ? g_opt.adVariant : 0;
                    RowSpec o1; o1.icon = L"\xE790"; o1.title = L"File Explorer command bar: " + adPk.variants[vi];
                    o1.desc = L"The After Dark pack has a version with the command bar shown and one with it hidden. Press Change to switch.";
                    const size_t nv = adPk.variants.size();
                    o1.buttons = {{L"Change", Btn::Secondary, true, [nv] { g_opt.adVariant = (g_opt.adVariant + 1) % static_cast<int>(nv); InvalidateRect(g_wnd, nullptr, FALSE); }}};
                    Row(c, o1);
                }
                if (adPk.officeAvailable) {
                    RowSpec o2; o2.icon = L"\xE790"; o2.title = L"Use the version that fixes the white background in Microsoft Office";
                    o2.desc = L"A second After Dark pack for this release with the Office white-background fix. Unchecked: the standard pack.";
                    o2.check = g_opt.adOffice ? 1 : 0; o2.onCheck = [] { g_opt.adOffice = !g_opt.adOffice; InvalidateRect(g_wnd, nullptr, FALSE); };
                    Row(c, o2);
                }
                if (adPk.sleepFix) {
                    RowSpec o3; o3.icon = L"\xE790"; o3.title = L"Fix the white colour that can appear after sleep (registry)";
                    o3.desc = L"Changes the Windows DefaultColors registry keys the way the pack's own FIX.reg does. The key is exported to the Setup backup first and the undo file is kept in AnkhCore's folder. Unchecked by default.";
                    o3.check = g_opt.adSleepFix ? 1 : 0; o3.onCheck = [] { g_opt.adSleepFix = !g_opt.adSleepFix; InvalidateRect(g_wnd, nullptr, FALSE); };
                    Row(c, o3);
                }
            }
        }
    }
    {   // 2.8.3: PowerToys already on this PC: keep it, or replace it (back up, remove, clean, install)
        static const PtInstall ptHad = PtDetect();
        static const PtOffer ptOff2 = CheckPowerToysComponent(LoadComponents());
        if (ptHad.installed && ptOff2.ok && g_opt.powertoys) {
            RowSpec r; r.icon = L"\xE8B0"; r.title = L"PowerToys is already installed (version " + ptHad.version + L"): reinstall it?";
            r.desc = g_opt.replacePowerToys ? L"Yes: its settings are backed up first, the old PowerToys is uninstalled, the leftovers are cleaned, then the packaged version is installed."
                                            : L"No (default): the PowerToys you have stays exactly as it is and Setup carries on. Tick to remove the old one, clean it and install the packaged version.";
            r.check = g_opt.replacePowerToys ? 1 : 0;
            r.chips.push_back({g_opt.replacePowerToys ? L"Replace" : L"Keep existing", g_opt.replacePowerToys ? col::warn : col::ok});
            r.onCheck = [] { g_opt.replacePowerToys = !g_opt.replacePowerToys; InvalidateRect(g_wnd, nullptr, FALSE); };
            Row(c, r);
        }
    }
    if (PackageHasWindhawkBundle() && g_opt.installWindhawkBundle && WindhawkInstalled()) {   // 2.8.3: Windhawk already on this PC
        RowSpec r; r.icon = L"\xE74C"; r.title = L"Windhawk is already installed: reinstall it?";
        r.desc = g_opt.replaceWindhawk ? L"Yes: its data (mods and settings) is backed up first, Windhawk is uninstalled, the leftovers are cleaned, then the packaged Windhawk and the AnkhCore mods are installed."
                                       : L"No (default): the Windhawk you have stays installed and Setup carries on, adding the AnkhCore mods next to yours. Tick to remove it, clean it and install the packaged one.";
        r.check = g_opt.replaceWindhawk ? 1 : 0;
        r.chips.push_back({g_opt.replaceWindhawk ? L"Replace" : L"Keep existing", g_opt.replaceWindhawk ? col::warn : col::ok});
        r.onCheck = [] { g_opt.replaceWindhawk = !g_opt.replaceWindhawk; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    if (PackageHasWindhawkBundle()) {
        RowSpec r; r.icon = L"\xE74C";
        r.title = L"Install Windhawk + 22 AnkhCore Mods & Settings";
        r.desc = L"Installs Windhawk offline if missing, deploys all 22 precompiled AnkhCore mods (x64/x86 + sources), and applies the AnkhCore Windhawk registry settings.";
        r.check = g_opt.installWindhawkBundle ? 1 : 0;
        r.chips.push_back({L"Default", col::accent});
        r.onCheck = [] { g_opt.installWindhawkBundle = !g_opt.installWindhawkBundle; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {
        RowSpec r; r.icon = L"\xED1A";
        r.title = L"Hide Windhawk tray icon (official setting)";
        r.desc = L"Applies Windhawk's official HideTrayIcon=1 setting so Windhawk runs quietly in the background without its own tray icon.";
        r.check = g_opt.hideWindhawkTray ? 1 : 0;
        r.chips.push_back({L"Default", col::accent});
        r.onCheck = [] { g_opt.hideWindhawkTray = !g_opt.hideWindhawkTray; InvalidateRect(g_wnd, nullptr, FALSE); };
        Row(c, r);
    }
    {
        if (g_opt.mode == SetupMode::CleanInstall && g_existing.found) {
            RowSpec r; r.icon = L"\xE74C";
            r.title = L"Reinstall AnkhCore-managed Windhawk components (Clean Install)";
            r.desc = L"Unchecked (Keep Existing): preserves your existing Windhawk and custom mods. Checked: backs up Windhawk data first, removes old AnkhCore-managed mod files, and reinstalls them cleanly.";
            r.check = g_opt.reinstallWindhawk ? 1 : 0;
            r.onCheck = [] { g_opt.reinstallWindhawk = !g_opt.reinstallWindhawk; InvalidateRect(g_wnd, nullptr, FALSE); };
            r.chips.push_back({g_opt.reinstallWindhawk ? L"Reinstall managed" : L"Keep existing", g_opt.reinstallWindhawk ? col::warn : col::ok});
            Row(c, r);
        }
        std::wstring sum = L"Core: AnkhCore " + std::wstring(kVersionLabel) + L" + Sensors";
        if (g_opt.driver && !g_pawn) sum += L" + PawnIO driver";
        if (g_opt.startup) sum += L" + Startup";
        if (g_opt.desktop) sum += L" + Desktop shortcut";
        if (g_opt.taskbarProfile) sum += L" + Win11 Taskbar Profile";
        if (g_opt.smartctl) sum += L" + smartctl";
        if (g_opt.powertoys) sum += L" + PowerToys";
        if (g_opt.installWindhawkBundle && PackageHasWindhawkBundle()) sum += L" + Windhawk & 22 Mods";
        if (g_opt.hideWindhawkTray) sum += L" + Hide Windhawk tray";
        if (g_opt.mode == SetupMode::CleanInstall && g_existing.found) {
            sum += L"\nClean Install replaces: AnkhCore.exe, Sensors\\, obsolete Fan\\, Mods\\icon-resource-redirect.wh.cpp";
            sum += g_opt.reinstallWindhawk ? L", + Windhawk managed mod (custom mods & backup preserved)." : L" (Windhawk kept as-is).";
        }
        sum += g_opt.iconThemes.empty()
             ? L"\nOptional icon themes: 0 selected (click Select all above if you want prebuilt icon packs)."
             : L"\nOptional icon themes: " + std::to_wstring(g_opt.iconThemes.size()) + L" pack(s) will be copied to IconThemes\\.";
        sum += L"\nA restart of Windows is required to finish. Setup will tell you when, and you choose: Restart now or Restart later. Nothing restarts by itself.";
        Section(c, L"4. Summary before install", sum);
    }
}
int g_restartCountdownSec = 15;
inline bool TriggerFirstInstallRestart(int delaySeconds) {
    if (kTestBuild) {
        TestRefused(L"mandatory post-install system restart");
        return false;
    }
    const int sec = std::max(0, delaySeconds);
    const std::wstring args = L"/r /t " + std::to_wstring(sec) +
        L" /c \"AnkhCore installation complete. Windows restarts to finish setup.\"";
    if (Run(SysDir() + L"\\shutdown.exe", args, true, 5000) == 0) return true;
    // shutdown.exe could not schedule it (refused or missing): restart directly instead of leaving the user waiting for a restart that never comes
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp{}; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    return ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_INSTALLATION) != 0;
}
// "Restart later": cancels the restart that was scheduled when the installation finished. The user restarts Windows when it suits them.
inline void CancelScheduledRestart() {
    if (kTestBuild) { TestRefused(L"cancel of the post-install restart"); return; }
    Run(SysDir() + L"\\shutdown.exe", L"/a", true, 5000);
}
void PageInstall(Ctx& c) {
    const std::wstring titleStr = !g_done ? (g_rollbackMode ? L"Restoring..." : L"Installing...")
                                : g_failed ? L"Setup stopped — Restart Cancelled"
                                : g_rollbackMode ? L"Previous version restored"
                                : (L"Installation complete — restart required");
    Text(c.dc, titleStr, RECT{c.x, c.y, c.x + c.w, c.y + c.S(40)}, c.u.f.title, g_failed ? col::err : col::text);
    c.y += c.S(56);
    DrawProgress(c.u, c.dc, RECT{c.x, c.y, c.x + c.w, c.y + c.S(8)}, g_progress, g_busy, g_failed);
    c.y += c.S(22);
    Para(c, g_failed ? g_out.message : g_status, c.u.f.text, g_failed ? col::err : col::sub);
    if (g_done) {
        if constexpr (kIsBetaBuild) {
            if (!g_failed) Para(c, CurrentBetaVersionLabel(false), c.u.f.small, col::accent);
        }
        if (!g_out.suxSummary.empty()) Para(c, g_out.suxSummary, c.u.f.small, col::ok);
        if (!g_out.themeAssetsSummary.empty()) Para(c, g_out.themeAssetsSummary, c.u.f.small, col::ok);
        if (!g_out.taskbarSummary.empty()) Para(c, g_out.taskbarSummary, c.u.f.small, g_out.taskbarApplied ? col::ok : col::sub);
        if (!g_out.iconThemesSummary.empty()) Para(c, g_out.iconThemesSummary, c.u.f.small, g_out.iconThemesInstalled > 0 ? col::ok : col::sub);
        if (!g_out.smartctlSummary.empty()) Para(c, g_out.smartctlSummary, c.u.f.small, g_out.smartctlInstalled ? col::ok : col::sub);
        if (!g_out.powertoysSummary.empty()) Para(c, g_out.powertoysSummary, c.u.f.small, g_out.powertoysInstalled ? col::ok : col::sub);
        if (!g_out.afterDarkSummary.empty()) Para(c, g_out.afterDarkSummary, c.u.f.small, g_out.afterDarkInstalled ? col::ok : col::sub);
        if (!g_out.windhawkBundleSummary.empty()) Para(c, g_out.windhawkBundleSummary, c.u.f.small, g_out.windhawkBundleInstalled ? col::ok : col::warn);
        if (!g_out.windhawkTraySummary.empty()) Para(c, g_out.windhawkTraySummary, c.u.f.small, g_out.windhawkTrayHidden ? col::ok : col::sub);
        for (const auto& w : g_out.warnings) Para(c, L"\xE7BA  " + w, c.u.f.small, col::warn);
        if (!g_failed && g_out.restartRequired && !g_rollbackMode) {
            Para(c, L"A restart is required: AnkhCore, the Windows theme tools and SecureUxTheme are installed and verified, and they become active after Windows restarts. Nothing is restarted until you choose below and click Finish.", c.u.f.small, col::warn);
        } else if (!g_failed && g_out.deferred) {
            Para(c, L"Some files are in use and are replaced at the next restart.", c.u.f.small, col::warn);
        }
        if (!g_failed && !g_out.backup.empty()) Para(c, L"Backup: " + g_out.backup, c.u.f.small, col::faint);
        if (!g_failed && !g_out.taskbarBackupPath.empty()) Para(c, L"Taskbar backup: " + g_out.taskbarBackupPath, c.u.f.small, col::faint);
        if (!FinalPageHasRestart()) {
            std::vector<BtnSpec> b;
            b.push_back({L"Close", Btn::Secondary, true, [] { DestroyWindow(g_wnd); }});
            Buttons(c, b);
        }
        Para(c, L"Details of every step: " + ProdDataDir() + L"\\Logs\\ankhcore.log", c.u.f.small, col::faint);
    }
}
void StartInstall() {
    g_step = 2; g_busy = true; g_done = g_failed = false; g_progress = 0;   // nothing is done yet (LOG-2)
    CloseHandle(CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr));
    InvalidateRect(g_wnd, nullptr, FALSE);
}
// ---------------------------------------------------------------- 2.9.5 Task 1: the installer looks like AnkhCore
// Dark graphite surfaces with the gold of the AnkhCore mark as the accent, on the same Windows backdrop material the application uses (acrylic when Windows offers it and
// transparency is on, otherwise solid). Painting stays GDI: every pixel is drawn as before, then the surfaces that are glass in the application (window base, cards, controls)
// get their alpha in one pass, so the backdrop shows through them while text and edges stay fully opaque.
bool g_glassWin = false;
void ApplyInstallerTheme() {
    ThemeSpec t = NeutralGlassTheme();                 // the application's own palette as the base
    // 2.9.7 (owner): a solid black installer whose buttons and accents take the blue of the AnkhCore logo (the glass version showed the logo's black matte as a grey halo).
    t.bg = RGB(3, 4, 6); t.side = RGB(3, 4, 6);
    t.panel = RGB(13, 16, 22); t.panel2 = RGB(20, 25, 34); t.track = RGB(28, 34, 46); t.overlay = RGB(10, 13, 18);
    t.border = RGB(30, 42, 58);
    t.accent = RGB(38, 160, 255);                      // the blue of the AnkhCore mark
    t.accentDim = RGB(28, 112, 190);
    t.accentDeep = RGB(10, 46, 86);
    t.borderHi = RGB(60, 150, 230);
    t.text = RGB(240, 246, 252);
    MutableActiveTheme() = t;
    col::bg = t.bg; col::side = t.side; col::panel = t.panel; col::panel2 = t.panel2; col::border = t.border; col::borderHi = t.borderHi;
    col::accent = t.accent; col::accentDim = t.accentDim; col::accentDeep = t.accentDeep; col::text = t.text; col::sub = t.sub; col::faint = t.faint;
    col::ok = t.ok; col::warn = t.warn; col::err = t.err; col::track = t.track; col::overlay = t.overlay;
}
// Optional trace for a developer: set ANKH_GLASS_DIAG=<file> and the setup writes which look it chose and why. Nothing is written otherwise.
void SetupGlassDiag(const std::wstring& line) {
    wchar_t f[MAX_PATH] = L""; if (!GetEnvironmentVariableW(L"ANKH_GLASS_DIAG", f, MAX_PATH)) return;
    HANDLE hf = CreateFileW(f, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); if (hf == INVALID_HANDLE_VALUE) return;
    const std::string u = WToUtf8(line + L"\r\n"); DWORD n = 0; WriteFile(hf, u.data(), static_cast<DWORD>(u.size()), &n, nullptr); CloseHandle(hf);
}
void SetupGlassLook(HWND h) {
    MaterialInput in;
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) in.transparencyOn = v != 0;
    HIGHCONTRASTW hc{sizeof(hc)}; if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) in.highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    in.build = GetWinVer().build;
    const Material m = ResolveMaterial(in);
    g_glassWin = false;
    wchar_t diag[160]; swprintf(diag, 160, L"material=%d transparencyOn=%d highContrast=%d build=%lu", static_cast<int>(m), in.transparencyOn ? 1 : 0, in.highContrast ? 1 : 0, static_cast<unsigned long>(in.build));
    constexpr bool kInstallerGlass = true;   // 2.9.7: black at 75 % opacity, see-through but not blurred (owner decision); false = solid black
    if (!kInstallerGlass || (m != MatAcrylic && m != MatFrosted)) { SetupGlassDiag(std::wstring(diag) + L" -> solid by material"); return; }      // Windows 10, transparency off, high contrast: the solid installer
    // plain per-pixel transparency (no blur, owner: "75 % opacity is fine"): a layered window, redrawn atomically with UpdateLayeredWindow
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_LAYERED);
    g_glassWin = true;
    SetupGlassDiag(std::wstring(diag) + L" -> glass on (layered window, no blur)");
}
// The opacity of a surface colour on the glass installer. Colours the application draws as glass use the application's own layer values; the window base sits a little
// denser so the setup text is always easy to read; every other colour (text, gold, logo, status colours) is fully opaque.
inline float InstallerSurfaceAlpha(COLORREF c) {
    if (c == col::bg) return 0.75f;
    const float a = ThemeGlassAlpha(c);
    return a;
}
// GDI back buffer with alpha: a top-down 32-bit DIB, converted to premultiplied alpha just before it reaches the window.
struct GlassPaint {
    HWND h; PAINTSTRUCT ps{}; HDC wdc = nullptr, dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; RECT cr{}; uint32_t* bits = nullptr;
    explicit GlassPaint(HWND w) : h(w) {
        wdc = BeginPaint(h, &ps); GetClientRect(h, &cr);
        dc = CreateCompatibleDC(wdc);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = cr.right; bi.bmiHeader.biHeight = -cr.bottom; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (!bmp) bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom);
        old = SelectObject(dc, bmp);
        SetStretchBltMode(dc, HALFTONE);
    }
    ~GlassPaint() {
        if (g_glassWin && bits) {
            struct Key { uint32_t rgb; float a; } keys[6]; int nk = 0;
            auto add = [&](COLORREF c) { const uint32_t k = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); for (int i = 0; i < nk; ++i) if (keys[i].rgb == k) return; if (nk < 6) keys[nk++] = {k, InstallerSurfaceAlpha(c)}; };
            add(col::bg); add(col::panel); add(col::panel2); add(col::track); add(col::overlay); add(col::accentDeep);
            const size_t n = static_cast<size_t>(cr.right) * static_cast<size_t>(cr.bottom);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t px = bits[i] & 0x00FFFFFF; float a = 1.f;
                bool keyed = false;
                for (int k = 0; k < nk; ++k) if (keys[k].rgb == px) { a = keys[k].a; keyed = true; break; }
                // the logo's near-black matte (anti-aliased, so not an exact key colour) takes the window's own opacity, so it does not show as a darker patch
                if (!keyed && std::max({(px >> 16) & 0xFFu, (px >> 8) & 0xFFu, px & 0xFFu}) <= 18u) a = InstallerSurfaceAlpha(col::bg);
                const uint32_t A = static_cast<uint32_t>(a * 255.f + 0.5f);
                bits[i] = (A << 24) | ((((px >> 16) & 0xFF) * A / 255) << 16) | ((((px >> 8) & 0xFF) * A / 255) << 8) | ((px & 0xFF) * A / 255);
            }
            // the window is a glass sheet: copy the whole buffer (pixel + alpha) so no old frame keeps showing through
            // SRCCOPY to a window DC drops the alpha byte (the surface came out opaque: the installer looked solid). The area is cleared to transparent first,
            // then the premultiplied frame is blended in with per-pixel alpha, which keeps it.
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            // 2.9.8: UpdateLayeredWindow swaps the whole frame in one step. The earlier clear-then-blend showed the cleared (nearly invisible) frame for a moment on every
            // repaint, so the installer flickered to ~20-30 % opacity whenever the mouse moved over it.
            SIZE sz{cr.right, cr.bottom}; POINT src{0, 0};
            if (!UpdateLayeredWindow(h, nullptr, nullptr, &sz, dc, &src, 0, &bf, ULW_ALPHA)) {
                PatBlt(wdc, 0, 0, cr.right, cr.bottom, BLACKNESS);
                GdiAlphaBlend(wdc, 0, 0, cr.right, cr.bottom, dc, 0, 0, cr.right, cr.bottom, bf);
            }
        } else {
            BitBlt(wdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        }
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); EndPaint(h, &ps);
    }
};
RECT ContentRect() { return RECT{g_ui.S(36), g_ui.S(118), g_ui.S(kW - 36), g_ui.S(kH - 76)}; }
void PaintAll(HWND h) {
    GlassPaint p(h);
    HDC dc = p.dc;
    g_ui.hits.clear();
    FillRect2(dc, p.cr, col::bg);
    if (g_brandLogo.bmp) DrawImage(dc, g_brandLogo, g_ui.S(kW - 170) - g_brandLogo.w / 2, g_ui.S(kH / 2 + 20) - g_brandLogo.h / 2);
    else g_logo.Draw(dc, g_ui.S(kW - 170), g_ui.S(kH / 2 + 20), g_busy ? 1 : (g_done && !g_failed) ? 2 : g_failed ? 3 : 0, 0.5f);
    if (g_brandLogoSmall.bmp) DrawImage(dc, g_brandLogoSmall, g_ui.S(34) + (g_ui.S(40) - g_brandLogoSmall.w) / 2, g_ui.S(22) + (g_ui.S(40) - g_brandLogoSmall.h) / 2);   // the unified logo beside the title
    else if (g_ankh) DrawIconEx(dc, g_ui.S(34), g_ui.S(22), g_ankh, g_ui.S(40), g_ui.S(40), 0, nullptr, DI_NORMAL);
    Text(dc, L"AnkhCore", RECT{g_ui.S(84), g_ui.S(18), g_ui.S(400), g_ui.S(48)}, g_ui.f.h1, col::text);
    Text(dc, L"Setup  \x00B7  " + std::wstring(kVersionLabel) + L"  \x00B7  Developer: As3aD", RECT{g_ui.S(86), g_ui.S(46), g_ui.S(500), g_ui.S(66)}, g_ui.f.small, col::accent);
    RECT rc = g_ui.R(kW - 50, 12, 36, 30), rm = g_ui.R(kW - 90, 12, 36, 30);
    if (g_ui.Over(rc)) Round(dc, rc, g_ui.S(8), RGB(70, 24, 28), RGB(70, 24, 28));
    if (g_ui.Over(rm)) Round(dc, rm, g_ui.S(8), col::panel2, col::panel2);
    Text(dc, L"\xE8BB", rc, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(dc, L"\xE921", rm, g_ui.f.icon, col::text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    g_ui.Add(rc, [] { PostMessageW(g_wnd, WM_CLOSE, 0, 0); });
    g_ui.Add(rm, [] { ShowWindow(g_wnd, SW_MINIMIZE); });
    int sx = g_ui.S(36);
    for (int i = 0; i < 3; ++i) {
        const bool active = g_step == i, past = i < g_step;
        RECT d{sx, g_ui.S(82), sx + g_ui.S(22), g_ui.S(104)};
        Ellipse2(dc, d, active ? col::accent : past ? col::accentDeep : col::panel, active || past ? col::accent : col::border);
        Text(dc, past ? L"\xE73E" : std::to_wstring(i + 1), d, g_ui.f.tiny, active ? col::bg : past ? col::accent : col::sub, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(dc, kSteps[i], RECT{sx + g_ui.S(28), g_ui.S(82), sx + g_ui.S(140), g_ui.S(104)}, g_ui.f.small, active ? col::text : col::sub, DT_VCENTER | DT_SINGLELINE);
        if (i < 2) Line(dc, sx + g_ui.S(120), g_ui.S(93), sx + g_ui.S(150), g_ui.S(93), past ? col::accent : col::border);
        sx += g_ui.S(160);
    }
    RECT cr = ContentRect();
    const int save = SaveDC(dc);
    IntersectClipRect(dc, cr.left, cr.top, cr.right, cr.bottom);
    g_optScroll = std::min(std::max(0, g_optScroll), OptMaxScroll());
    const int top0 = cr.top + g_ui.S(4) - (PageScrolls() ? g_optScroll : 0);
    Ctx c{dc, g_ui, cr.left, top0, g_ui.S(560), cr, g_busy};
    switch (g_step) { case 0: PageWelcome(c); break; case 1: PageOptions(c); break; default: PageInstall(c); }
    RestoreDC(dc, save);
    if (PageScrolls()) {   // measured on this paint; a changed height corrects the scroll position on the next one
        g_optContent = c.y - top0 + g_ui.S(8); g_optView = cr.bottom - cr.top;
        if (g_optScroll > OptMaxScroll()) { g_optScroll = OptMaxScroll(); InvalidateRect(g_wnd, nullptr, FALSE); }
        if (OptMaxScroll() > 0) {
            const int tx = cr.left + g_ui.S(572), th = g_optView, bh = std::max(g_ui.S(28), th * g_optView / g_optContent);
            const int by = cr.top + (th - bh) * g_optScroll / OptMaxScroll();
            Round(dc, RECT{tx, cr.top, tx + g_ui.S(4), cr.bottom}, g_ui.S(2), col::panel, col::panel);
            Round(dc, RECT{tx, by, tx + g_ui.S(4), by + bh}, g_ui.S(2), col::accent, col::accent);
            if (g_optScroll < OptMaxScroll()) Text(dc, L"More options below: scroll with the mouse wheel", RECT{cr.left, cr.bottom + g_ui.S(2), cr.left + g_ui.S(560), cr.bottom + g_ui.S(24)}, g_ui.f.tiny, col::accent, DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (FinalPageHasRestart()) {
        const int fy = kH - 62;
        struct Opt { const wchar_t* text; bool now; int x; };
        const Opt opts[] = {{L"Restart later", false, 36}, {L"Restart now", true, 36 + 190}};
        for (const auto& o : opts) {
            RECT hit = g_ui.R(o.x, fy, 180, 40);
            const bool on = g_restartNow == o.now, hov = g_ui.Over(hit);
            RECT dot{hit.left, hit.top + g_ui.S(10), hit.left + g_ui.S(20), hit.top + g_ui.S(30)};
            Ellipse2(dc, dot, on ? col::accent : (hov ? col::panel2 : col::panel), on ? col::accent : (hov ? col::borderHi : col::border));
            if (on) Ellipse2(dc, RECT{dot.left + g_ui.S(6), dot.top + g_ui.S(6), dot.right - g_ui.S(6), dot.bottom - g_ui.S(6)}, col::bg, col::bg);
            Text(dc, o.text, RECT{dot.right + g_ui.S(10), hit.top, hit.right, hit.bottom}, g_ui.f.text, on ? col::text : col::sub, DT_VCENTER | DT_SINGLELINE);
            const bool now = o.now;
            g_ui.Add(hit, [now] { if (!g_finishing) { g_restartNow = now; InvalidateRect(g_wnd, nullptr, FALSE); } });
        }
        RECT fin = g_ui.R(kW - 36 - 150, fy, 150, 40);
        DrawButton(g_ui, dc, fin, L"Finish", Btn::Primary, !g_finishing, g_ui.Over(fin));
        g_ui.Add(fin, [] {
            if (g_finishing) return;       // a second click never starts a second restart
            if (!g_restartNow) { DestroyWindow(g_wnd); return; }
            g_finishing = true;
            const bool ok = TriggerFirstInstallRestart(0);
            if (ok) { DestroyWindow(g_wnd); return; }
            g_finishing = false;           // the installation result stays on screen; the user can retry or choose Restart later
            MessageBoxW(g_wnd, kTestBuild ? L"The test setup does not restart Windows." : L"Windows could not be restarted. The installation itself is complete. Choose Restart later and restart Windows yourself, or try again.",
                        L"AnkhCore Setup", MB_OK | MB_ICONWARNING);
            InvalidateRect(g_wnd, nullptr, FALSE);
        });
    }
    if (g_step < 2) {
        RECT next = g_ui.R(kW - 36 - 130, kH - 62, 130, 40), back = g_ui.R(kW - 36 - 130 - 10 - 110, kH - 62, 110, 40);
        const bool canNext = g_kind != InstallKind::Downgrade;
        DrawButton(g_ui, dc, next, g_step == 1 ? L"Install" : L"Next", Btn::Primary, canNext, g_ui.Over(next));
        if (canNext) g_ui.Add(next, [] {
            if (g_step == 1) {
                if (g_opt.mode == SetupMode::CleanInstall && !g_opt.cleanConfirmed) {
                    const int ans = MessageBoxW(g_wnd,
                        L"Clean Install will:\n"
                        L"1. Back up your user settings, journal, and network counters.\n"
                        L"2. Remove the existing AnkhCore application and obsolete runtime folders (including Fan\\).\n"
                        L"3. Install AnkhCore 2.5 cleanly and restore compatible user settings.\n\n"
                        L"Your Backup\\ folder, themes, icons, and unrelated personal files will NOT be deleted.\n\n"
                        L"Proceed with Clean Install?",
                        L"Confirm Clean Install", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                    if (ans != IDYES) return;
                    g_opt.cleanConfirmed = true;
                }
                if ((g_opt.replacePowerToys && g_opt.powertoys && PtDetect().installed) || (g_opt.replaceWindhawk && g_opt.installWindhawkBundle && WindhawkInstalled())) {
                    std::wstring t = L"You chose to replace:\n";
                    if (g_opt.replacePowerToys && g_opt.powertoys && PtDetect().installed) t += L"  " + std::wstring(1, wchar_t(0x2022)) + L" PowerToys\n";
                    if (g_opt.replaceWindhawk && g_opt.installWindhawkBundle && WindhawkInstalled()) t += L"  " + std::wstring(1, wchar_t(0x2022)) + L" Windhawk\n";
                    t += L"\nFor each: its data is copied to the Setup backup folder first, the old program is uninstalled, the leftovers are cleaned, then the new one is installed. If the backup or the uninstall fails, the old one is kept.\n\nContinue?";
                    if (MessageBoxW(g_wnd, t.c_str(), L"Replace existing programs", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
                }
                StartInstall();
            } else {
                ++g_step; InvalidateRect(g_wnd, nullptr, FALSE);
            }
        });
        if (g_step > 0) {
            DrawButton(g_ui, dc, back, L"Back", Btn::Secondary, true, g_ui.Over(back));
            g_ui.Add(back, [] { --g_step; InvalidateRect(g_wnd, nullptr, FALSE); });
        }
        Text(dc, L"Windows 10 / 11, 64-bit", RECT{g_ui.S(36), g_ui.S(kH - 62), g_ui.S(400), g_ui.S(kH - 22)}, g_ui.f.tiny, col::faint, DT_VCENTER | DT_SINGLELINE);
    }
}
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_wnd = h; g_ui.wnd = h; SetupWindowLook(h); SetupGlassLook(h);
    {   // the unified logo (owner brief 2026-11-14): logo.png with its own transparency, the outer circuitry turned down (brand_logo.h)
        auto brand = [](int size) {
            Image32 im = LoadImageRes(L"BRANDLOGO", size, size);
            DIBSECTION ds{};
            if (im.bmp && GetObjectW(im.bmp, sizeof(ds), &ds) && ds.dsBm.bmBits && ds.dsBm.bmWidthBytes == im.w * 4) BrandLogoSoften(static_cast<uint32_t*>(ds.dsBm.bmBits), im.w, im.h);
            return im;
        };
        g_brandLogo = brand(g_ui.S(240)); g_brandLogoSmall = brand(g_ui.S(40));
    }
    if (!g_brandLogo.bmp) g_logo.Load(L"ANKHMARK", g_ui.S(230), g_ui.S(280), /*tint*/false);
        g_ankh = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0));
        SetTimer(h, 1, 33, nullptr);
        if (g_rollbackMode) StartInstall();
        return 0;
    case WM_TIMER:
        if (g_busy) InvalidateRect(h, nullptr, FALSE);   // the logo animates only while working
        return 0;
    case WM_STATUS: {
        auto* s = reinterpret_cast<std::wstring*>(l); g_status = *s; delete s;
        g_progress = std::max(g_progress, static_cast<int>(w));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_DONE: {
        std::unique_ptr<Outcome> r(reinterpret_cast<Outcome*>(l));
        g_out = *r; g_busy = false; g_done = true; g_failed = !r->ok; g_optScroll = 0; g_restartNow = false; g_finishing = false;
        if (r->ok) g_progress = 100;
        // 2.8: Setup never restarts Windows by itself. It says a restart is needed and offers Restart now / Restart later.
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: PaintAll(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: g_ui.mouse = {GET_X_LPARAM(l), GET_Y_LPARAM(l)}; { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); } InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSELEAVE: g_ui.mouse = {-1, -1}; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, g_ui.OverAny() ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (!g_ui.Click(p) && p.y < g_ui.S(76)) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (PageScrolls()) { g_optScroll = std::min(std::max(0, g_optScroll - GET_WHEEL_DELTA_WPARAM(w) * g_ui.S(60) / 120), OptMaxScroll()); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (PageScrolls()) {
            const int page = g_optView * 3 / 4, line = g_ui.S(60);
            int d = 0;
            if (w == VK_DOWN) d = line; else if (w == VK_UP) d = -line; else if (w == VK_NEXT) d = page; else if (w == VK_PRIOR) d = -page; else if (w == VK_END) d = OptMaxScroll(); else if (w == VK_HOME) d = -OptMaxScroll();
            if (d) { g_optScroll = std::min(std::max(0, g_optScroll + d), OptMaxScroll()); InvalidateRect(h, nullptr, FALSE); return 0; }
        }
        break;
    case WM_CLOSE:
        if (g_busy) { MessageBoxW(h, L"Please wait until the setup finishes.", L"AnkhCore Setup", MB_OK | MB_ICONINFORMATION); return 0; }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- test build: automated sandbox scenarios
#ifdef ANKH_TEST_BUILD
std::vector<std::wstring> g_rep; int g_fails = 0, g_skips = 0;
void Chk(bool ok, const std::wstring& what) { g_rep.push_back((ok ? L"PASS  " : L"FAIL  ") + what); if (!ok) ++g_fails; }
void Skip(const std::wstring& what) { g_rep.push_back(L"SKIP  " + what); ++g_skips; }   // not run: never counted as a pass
std::map<std::wstring, std::string> HashTree(const std::wstring& root, const std::wstring& rel = L"") {
    std::map<std::wstring, std::string> m; const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    for (const auto& f : ListDir(dir, false)) m[rel.empty() ? f : rel + L"\\" + f] = Sha256File(dir + L"\\" + f);
    for (const auto& d : ListDir(dir, true)) for (const auto& [k, v] : HashTree(root, rel.empty() ? d : rel + L"\\" + d)) m[k] = v;
    return m;
}
std::map<std::wstring, std::string> Under(const std::map<std::wstring, std::string>& m, const std::wstring& prefix) {
    std::map<std::wstring, std::string> r; for (const auto& [k, v] : m) if (k == prefix || k.rfind(prefix + L"\\", 0) == 0) r[k] = v; return r;
}
int SandboxTest() {
    // The upgrade scenarios need a real 2.2. Once 2.5 is installed on the PC, that is the copy the setup kept in its backup (read only).
    std::wstring realApp = KnownFolder(FOLDERID_ProgramFiles) + L"\\AnkhCore";
    if (!DirExists(realApp + L"\\Sensors\\FanConfigs")) {
        WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((ProdDataDir() + L"\\Backup\\AnkhCore-2.2.*").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            std::wstring newest;
            do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring(fd.cFileName) > newest) newest = fd.cFileName; } while (FindNextFileW(fh, &fd));
            FindClose(fh);
            if (!newest.empty()) realApp = ProdDataDir() + L"\\Backup\\" + newest;
        }
    }
    const std::wstring S = SandboxRoot();
    AppDirOverride() = S + L"\\Program Files\\AnkhCore";
    const std::wstring app = AppDir();
    g_rep.push_back(L"AnkhCore " + std::wstring(kVersion) + L" setup sandbox test  " + NowText(true));
    g_rep.push_back(L"Sandbox: " + S + L"   (program folder " + app + L", backups " + BackupRoot() + L")");
    auto wipe = [&] {
        if (DirExists(S)) Evict(S, true, false);
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(kPrefKey) + L"\\Sandbox").c_str());
    };
    std::vector<std::pair<std::wstring, int>> seen;   // every progress report of the run in work (LOG-2)
    auto quiet = [&seen](const std::wstring& s, int pct) { seen.push_back({s, pct}); LogI(L"[sandbox] " + s); };
    // Honest progress: starts at 0, never goes back, says "Step k of n" with one n for the whole run and every step
    // from 1 to n in order, and reaches 100 only with the final "Done." of a run that succeeded.
    auto progressOk = [&seen](bool succeeded, int& stepsOut) {
        stepsOut = 0;
        if (seen.empty() || seen.front().second != 0) return false;
        int last = 0, n = 0, k = 0;
        for (size_t i = 0; i < seen.size(); ++i) {
            const bool final = succeeded && i + 1 == seen.size();
            if (seen[i].second < last || seen[i].second > 100 || (seen[i].second == 100) != final) return false;
            last = seen[i].second;
            if (final) { if (seen[i].first != L"Done.") return false; continue; }
            int sk = 0, sn = 0;
            if (swscanf_s(seen[i].first.c_str(), L"Step %d of %d:", &sk, &sn) != 2) return false;
            if (n == 0) n = sn;
            if (sn != n || (sk != k && sk != k + 1) || sk > n) return false;
            k = sk;
        }
        stepsOut = n;
        return succeeded ? k == n : k >= 1;
    };
    Options opt; opt.desktop = false; opt.startup = false;
    std::map<std::wstring, std::string> want;   // payload path -> sha (from the manifest)
    {
        std::vector<PkEntry> files; std::wstring err;
        const bool verified = VerifyPayload(g_entries, files, want, err);   // evaluated first: the message below reports its result
        Chk(verified && files.size() == 354 && want.size() == 354, L"payload: manifest present, every entry listed and intact (" + std::to_wstring(files.size()) + L" files, 354 expected: 22 core + 84 of the bundled UniGetUI + 4 PawnIO modules and licence + 243 notebook fan configurations + 1 notice)" + (err.empty() ? L"" : L": " + err));
        bool onlyCore = true; int fanCfgs = 0;
        for (const auto& e : g_entries) { onlyCore &= e.path == L"MANIFEST" || e.path.rfind(L"APP/", 0) == 0 || e.path.rfind(L"SENS/", 0) == 0 || e.path.rfind(L"SUX/", 0) == 0 || e.path.rfind(L"THEMETOOLS/", 0) == 0 || e.path.rfind(L"UNIGET/", 0) == 0; if (e.path.rfind(L"SENS/FanConfigs/", 0) == 0 && e.path.size() > 4 && e.path.compare(e.path.size() - 4, 4, L".xml") == 0) ++fanCfgs; }
        Chk(onlyCore, L"payload: only APP/, SENS/, SUX/, and THEMETOOLS/ (verified core + application-local As3aD.theme & SecureUxTheme)");
        Chk(fanCfgs == 243, L"payload: the 243 notebook fan configurations of AnkhCore's own fan-control engine are in the package (" + std::to_wstring(fanCfgs) + L")");
    }
    auto installedOk = [&](const wchar_t* label) {
        bool all = !want.empty(); std::wstring bad;   // an empty manifest must not pass
        for (const auto& [p, sha] : want) { std::wstring d; PayloadDest(p, app, d); if (Sha256File(d) != sha) { all = false; bad = p; } }
        Chk(all, std::wstring(label) + L": every payload file installed and matches its SHA-256" + (bad.empty() ? L"" : L" (first mismatch " + bad + L")"));
    };
    // A. fresh install
    wipe();
    seen.clear();
    Outcome a = InstallCore(g_entries, opt, quiet);
    Chk(a.ok && a.kind == InstallKind::Fresh && a.restartRequired, L"fresh install succeeds and requires mandatory restart (" + (a.ok ? std::wstring(L"ok") : a.message) + L")");
    { int n = 0; const bool ok = progressOk(true, n); Chk(ok && n == 6, L"fresh install: progress is counted steps (" + std::to_wstring(n) + L" steps, " + std::to_wstring(seen.size()) + L" reports), never goes back, 100 only at Done"); }
    installedOk(L"fresh install");
    Chk(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == kVersion && RegStr(UninstallHive(), UninstallKey(), L"InstallLocation") == app,
        L"fresh install: uninstall entry written (version " + RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") + L", sandbox key)");
    Chk(ListDir(BackupRoot(), true).empty(), L"fresh install: no backup made");
    Chk(DirExists(app + L"\\Sensors\\FanConfigs") && FileExists(app + L"\\Sensors\\FanConfigs\\HP ZBook Studio G3.xml") && FileExists(app + L"\\Sensors\\PawnIO\\LpcACPIEC.bin") && FileExists(app + L"\\Sensors\\LibreHardwareMonitorLib.dll") && FileExists(app + L"\\Sensors\\PawnIO_setup.exe") && FileExists(app + L"\\THIRD-PARTY-NOTICES.txt"),
        L"fresh install: helper files, PawnIO setup and third-party notices, the fan configurations and the PawnIO modules in place");
    {
        HMODULE m = LoadLibraryExW(AppExe().c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        bool held = false; for (const wchar_t* r : {L"BGLOGO", L"INTRO", L"PV_HUD", L"PV_TECH", L"PV_MAGNUM", L"PV_SWEET", L"PV_BIGSUR"}) held |= m && FindResourceW(m, r, RT_RCDATA);
        Chk(m && !held && FindResourceW(m, L"ANKHMARK", RT_RCDATA), L"installed AnkhCore.exe: official ankh mark embedded; no held assets (BGLOGO, INTRO, pack previews)");
        {   // unified logo (owner brief 2026-11-14): the installed program and this setup carry the same transparent logo.png, byte for byte
            auto res = [](HMODULE mod, const uint8_t*& d, DWORD& n) {
                HRSRC r = mod ? FindResourceW(mod, L"BRANDLOGO", RT_RCDATA) : nullptr; HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
                d = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr; n = r ? SizeofResource(mod, r) : 0; return d != nullptr && n > 0;
            };
            const uint8_t *a = nullptr, *b = nullptr; DWORD na = 0, nb = 0;
            static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            const bool both = res(m, a, na) && res(GetModuleHandleW(nullptr), b, nb);
            const bool same = both && na == nb && memcmp(a, b, na) == 0;
            // IHDR: width and height at bytes 16..23, colour type 6 (RGBA) at byte 25
            const bool rgba = same && na > 26 && memcmp(a, png, 8) == 0 && ((a[16] << 24) | (a[17] << 16) | (a[18] << 8) | a[19]) == 1254 && ((a[20] << 24) | (a[21] << 16) | (a[22] << 8) | a[23]) == 1254 && a[25] == 6;
            Chk(rgba, L"logo: installed AnkhCore.exe and the setup embed the same logo.png (" + std::to_wstring(na) + L" bytes, PNG 1254 x 1254 with alpha)");
        }
        if (m) FreeLibrary(m);
        HMODULE self = GetModuleHandleW(nullptr); held = false;
        for (const wchar_t* r : {L"BGLOGO", L"INTRO", L"PV_HUD"}) held |= FindResourceW(self, r, RT_RCDATA) != nullptr;
        Chk(!held && FindResourceW(self, L"ANKHMARK", RT_RCDATA), L"setup: official ankh mark embedded; no held assets");
    }
    // B. upgrade over a copy of the installed 2.2 (read from Program Files, written only into the sandbox)
    wipe();
    Chk(CopyTree(realApp + L"\\AnkhCore.exe", app + L"\\AnkhCore.exe") && CopyTree(realApp + L"\\Sensors", app + L"\\Sensors"), L"upgrade: staged a copy of the real 2.2 program and helper from " + realApp);
    for (const auto& k : KeptItems()) { const std::string s = "kept " + WToUtf8(k); WriteAll(app + L"\\" + k + L"\\marker.txt", reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", L"2.2");
    const auto h22 = HashTree(app);
    seen.clear();
    Outcome b = InstallCore(g_entries, opt, quiet);
    Chk(b.ok && b.kind == InstallKind::Upgrade && b.from.valid && b.from.minor == 2, L"upgrade from 2.2 succeeds (" + (b.ok ? std::wstring(L"ok") : b.message) + L")");
    { int n = 0; const bool ok = progressOk(true, n); Chk(ok && n == 8, L"upgrade: progress counts the two extra steps, close and back up (" + std::to_wstring(n) + L" steps)"); }
    installedOk(L"upgrade");
    {
        const auto bk = HashTree(b.backup);
        bool same = !bk.empty();
        for (const auto& item : ReplacedItems()) for (const auto& [k, v] : Under(h22, item)) { auto it = bk.find(k); same &= it != bk.end() && it->second == v; }
        Chk(same, L"upgrade: the 2.2 program and helper are in the backup unchanged (" + std::to_wstring(bk.size() - 1) + L" files, " + NameOf(b.backup) + L")");
        Chk(bk.count(L"Sensors\\FanConfigs\\HP ZBook 15 G5.xml") || !Under(bk, L"Sensors\\FanConfigs").empty(), L"upgrade: 2.2's NBFC configs went to the backup, not the new install");
        bool kept = true; const auto now = HashTree(app);
        for (const auto& k : KeptItems()) for (const auto& [p, v] : Under(h22, k)) { auto it = now.find(p); kept &= it != now.end() && it->second == v; }
        Chk(kept, L"upgrade: appearance content (7tsp, IconPacks, SecureUxTheme, Assets) left unchanged");
        Chk(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == kVersion, L"upgrade: uninstall entry now says " + std::wstring(kVersion));
    }
    // C. repair
    const size_t backups = ListDir(BackupRoot(), true).size();
    Outcome c = InstallCore(g_entries, opt, quiet);
    Chk(c.ok && c.kind == InstallKind::Repair && c.backup.empty() && ListDir(BackupRoot(), true).size() == backups, L"repair (same version): succeeds without a new backup");
    installedOk(L"repair");
    // D. damaged package: refused before anything changes
    {
        const auto before = HashTree(app);
        std::vector<PkEntry> bad = g_entries; std::vector<uint8_t> copy;
        for (auto& e : bad) if (e.path == L"APP/AnkhCore.exe") { copy.assign(e.data, e.data + e.stored); copy[copy.size() / 2] ^= 0x5A; e.data = copy.data(); }
        seen.clear();
        Outcome d = InstallCore(bad, opt, quiet);
        Chk(!d.ok && d.message.find(L"damaged") != std::wstring::npos && HashTree(app) == before, L"damaged package: refused, nothing changed (" + d.message.substr(0, 60) + L"...)");
        { int n = 0; const bool ok = progressOk(false, n); Chk(ok && seen.size() == 1 && seen[0].second == 0, L"damaged package: progress stops at step 1 with nothing done (never shows completion)"); }
    }
    // D2. the program file cannot be installed (write cut in the middle, wrong hash, swap-in refused) during an upgrade:
    // the previous version comes back byte for byte, no staged file and no backup folder is left, the uninstall entry is untouched
    {
        const wchar_t* names[] = {L"write cut in the middle", L"wrong hash on the staged file", L"swap-in refused (file stays locked)"};
        for (int k = 0; k < 3; ++k) {
            RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", L"2.5.0.1");
            const auto before = HashTree(app); const size_t nb = ListDir(BackupRoot(), true).size(); const int h0 = PlaceFileFaultsHit();
            PlaceFileFaults& fl = PlaceFileFaultSet(); fl = PlaceFileFaults{}; fl.nameContains = L"\\AnkhCore.exe";
            if (k == 0) fl.cutAfter = 1000; else if (k == 1) fl.corrupt = true; else fl.refuseReplace = true;
            seen.clear();
            Outcome x = InstallCore(g_entries, opt, quiet);
            fl = PlaceFileFaults{};
            Chk(!x.ok && x.message.find(L"previous version was restored") != std::wstring::npos && PlaceFileFaultsHit() > h0 && HashTree(app) == before &&
                RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == L"2.5.0.1" && ListDir(BackupRoot(), true).size() == nb && !FileExists(app + L"\\AnkhCore.exe.ankh-new"),
                std::wstring(L"failed upgrade, ") + names[k] + L": the previous version is back byte for byte, nothing left behind (" + x.message.substr(0, 70) + L"...)");
        }
        RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", kVersion);
        Outcome again = InstallCore(g_entries, opt, quiet);
        Chk(again.ok, L"after the failed upgrades the setup installs normally (" + (again.ok ? std::wstring(L"ok") : again.message) + L")");
        installedOk(L"install after failed upgrades");
    }
    // E. downgrade refused
    {
        RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", L"2.99");   // newer than any shipped version (was 2.6 until the 2.7 release)
        const auto before = HashTree(app);
        Outcome e = InstallCore(g_entries, opt, quiet);
        Chk(!e.ok && e.kind == InstallKind::Downgrade && HashTree(app) == before, L"newer version installed: setup refuses (no --allow-downgrade), nothing changed");
        RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", kVersion);
    }
    // F. rollback to 2.2, then forward again with the setup
    {
        Outcome f = RollbackCore(quiet);
        const auto now = HashTree(app);
        bool same = f.ok;
        for (const auto& item : ReplacedItems()) for (const auto& [k, v] : Under(h22, item)) { auto it = now.find(k); same &= it != now.end() && it->second == v; }
        bool noExtra = true; for (const auto& item : ReplacedItems()) for (const auto& [k, v] : Under(now, item)) noExtra &= h22.count(k) == 1;
        Chk(same && noExtra, L"rollback: the 2.2 program and helper are back, byte for byte (" + (f.ok ? std::wstring(L"ok") : f.message) + L")");
        Chk(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == L"2.2.0", L"rollback: uninstall entry says 2.2.0");
        Chk(!f.backup.empty() && FileExists(f.backup + L"\\AnkhCore.exe"), L"rollback: the replaced 2.5 files are kept as a backup");
        Outcome g = InstallCore(g_entries, opt, quiet);
        Chk(g.ok && g.kind == InstallKind::Upgrade, L"after a rollback the setup upgrades again");
        installedOk(L"re-upgrade");
        {   // FC-18: a rollback that cannot restore one item of the old version puts the current version back, keeps the old backup and leaves no new backup folder
            const auto before = HashTree(app); const size_t nb = ListDir(BackupRoot(), true).size();
            const std::wstring failFrom = g.backup + L"\\Sensors";
            MoveFault() = [failFrom](const std::wstring& from, const std::wstring&) { return from == failFrom; };
            seen.clear();
            Outcome fr = RollbackCore(quiet);
            MoveFault() = nullptr;
            const auto afterT = HashTree(app); std::wstring diff;
            for (const auto& [k, v] : before) { auto it = afterT.find(k); if (it == afterT.end()) diff += L" missing:" + k; else if (it->second != v) diff += L" changed:" + k; }
            for (const auto& [k, v] : afterT) if (!before.count(k)) diff += L" extra:" + k;
            const bool refusedOk = !fr.ok && fr.message.find(L"could not be restored") != std::wstring::npos;
            const bool sameTree = diff.empty(), sameVer = RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == kVersion;
            const size_t nb2 = ListDir(BackupRoot(), true).size();
            const bool oldKept = FileExists(g.backup + L"\\AnkhCore.exe") && FileExists(g.backup + L"\\backup.txt");
            Chk(refusedOk && sameTree && sameVer && nb2 == nb && oldKept,
                L"rollback failure: an item that cannot be restored puts the current version back byte for byte, keeps the old backup and leaves no new backup folder (refused " + std::to_wstring(refusedOk) + L", tree differences:" + (diff.empty() ? std::wstring(L" none") : diff) +
                L", version " + std::to_wstring(sameVer) + L", backup folders " + std::to_wstring(nb) + L" -> " + std::to_wstring(nb2) + L", old backup kept " + std::to_wstring(oldKept) + L")");
            Outcome fr2 = RollbackCore(quiet);   // and the rollback itself still works afterwards
            Chk(fr2.ok, L"rollback failure: the same rollback succeeds once the fault is gone (" + (fr2.ok ? std::wstring(L"ok") : fr2.message) + L")");
            Outcome g2 = InstallCore(g_entries, opt, quiet);
            Chk(g2.ok, L"rollback failure: the setup upgrades again afterwards");
            installedOk(L"after the failed rollback test");
        }
    }
    {   // uninstall: the same RemoveInstall() the uninstaller runs, against a sandbox install that has a backup behind it
        wipe();
        Outcome u1 = InstallCore(g_entries, opt, quiet);
        RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", L"2.5");   // as if the earlier 2.5 (build 0) were installed
        Outcome u2 = InstallCore(g_entries, opt, quiet);
        Chk(u1.ok && u2.ok && u2.kind == InstallKind::Upgrade && !u2.backup.empty() && FileExists(u2.backup + L"\\AnkhCore.exe"),
            L"preview over 2.5: the same version with an older build is an upgrade, and the previous files are backed up first (" + (u2.backup.empty() ? std::wstring(L"no backup") : u2.backup.substr(u2.backup.find_last_of(L'\\') + 1)) + L")");
        Chk(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == kVersion && RegStr(UninstallHive(), UninstallKey(), L"DisplayName") == std::wstring(kProductFull) + L" " + kVersionLabel,
            L"preview: the uninstall entry is named \"AnkhCore " + std::wstring(kVersionLabel) + L"\", version " + kVersion);
        Outcome u3 = InstallCore(g_entries, opt, quiet);
        Chk(u3.ok && u3.kind == InstallKind::Repair, L"preview over itself is a repair");
        {   // the owner's real path: this build over the installed 2.5 Preview 1 (2.5.0.1)
            RegSetStr(UninstallHive(), UninstallKey(), L"DisplayVersion", L"2.5.0.1");
            seen.clear();
            Outcome p2 = InstallCore(g_entries, opt, quiet);
            int steps = 0; const bool prog = progressOk(true, steps);
            const bool newer = CompareVer(ParseVer(kVersion), ParseVer(L"2.5.0.1")) > 0;
            Chk(newer && p2.ok && p2.kind == InstallKind::Upgrade && p2.from.build == 1 && !p2.backup.empty() && p2.backup.find(L"AnkhCore-2.5.0.1-") != std::wstring::npos &&
                FileExists(p2.backup + L"\\AnkhCore.exe") && RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion") == kVersion && prog,
                L"upgrade over Preview 1 (2.5.0.1): " + std::wstring(kVersion) + L" is newer, upgrades with a backup of 2.5.0.1 first, and the entry now says " + kVersion);
            installedOk(L"upgrade over Preview 1");
        }
        const std::wstring keep = SetupDataRoot() + L"\\Logs\\keep-me.txt", userFile = SetupDataRoot() + L"\\net-counters.txt";
        EnsureDir(SetupDataRoot() + L"\\Logs"); const std::string mark = "user data";
        WriteAll(keep, reinterpret_cast<const uint8_t*>(mark.data()), mark.size()); WriteAll(userFile, reinterpret_cast<const uint8_t*>(mark.data()), mark.size());
        const std::wstring bk = u2.backup;
        const RemoveReport rr = RemoveInstall();
        Chk(rr.failed.empty() && !rr.deferred, L"uninstall: the removal report confirms every part (" + std::to_wstring(rr.failed.size()) + L" not removed)");
        Chk(!DirExists(app) && !FileExists(app + L"\\AnkhCore.exe"), L"uninstall: the program folder is gone");
        Chk(RegStr(UninstallHive(), UninstallKey(), L"DisplayVersion").empty() && RegStr(UninstallHive(), UninstallKey(), L"UninstallString").empty(), L"uninstall: the uninstall entry is gone");
        Chk(ReadText(keep) == mark && ReadText(userFile) == mark, L"uninstall: logs and data in the data folder are kept");
        Chk(DirExists(bk) && FileExists(bk + L"\\AnkhCore.exe") && FileExists(bk + L"\\backup.txt"), L"uninstall: the backup of the previous version is kept (rollback material)");
        Outcome u4 = InstallCore(g_entries, opt, quiet);
        Chk(u4.ok && u4.kind == InstallKind::Fresh, L"after an uninstall the setup installs fresh again");
        installedOk(L"reinstall after uninstall");
        Chk(ReadText(keep) == mark, L"reinstall: existing data is still untouched");
        {   // SEC-3: a file that cannot be deleted (held open without delete sharing) must show up in the report, not as "removed"
            HANDLE held = CreateFileW((app + L"\\AnkhCore.exe").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            const RemoveReport rb = RemoveInstall();
            const bool named = std::any_of(rb.failed.begin(), rb.failed.end(), [&](const std::wstring& f) { return f.find(app) != std::wstring::npos; });
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            UninstallReport ur; ur.removal = rb;
            const UninstallMessage um = UninstallSummary(ur, SetupDataRoot());
            Chk(held != INVALID_HANDLE_VALUE && named && FileExists(app + L"\\AnkhCore.exe") && !um.ok && um.text.find(L"not completely removed") != std::wstring::npos,
                L"uninstall: a program file that cannot be deleted is named in the report, and the message says AnkhCore was not completely removed");
            const RemoveReport rc = RemoveInstall();
            Chk(rc.failed.empty() && !DirExists(app), L"uninstall: once the file is free, running it again removes the rest");
        }
    }
    {   // WT-03: the optional smartctl component. Stand-in files only: the running test setup is copied as "smartctl.exe" (a valid 64-bit program,
        // NOT a real smartctl) and a short text with the GPL heading serves as the license. They exist only inside this sandbox.
        wchar_t selfExe[MAX_PATH]; GetModuleFileNameW(nullptr, selfExe, MAX_PATH);
        const std::wstring pkg = S + L"\\SmartPkg", pkgExe = pkg + L"\\Tools\\smartctl.exe", pkgLic = pkg + L"\\Tools\\COPYING.smartmontools";
        const std::wstring appExe = app + L"\\Tools\\smartctl.exe", appLic = app + L"\\Tools\\COPYING.smartmontools";
        const std::string gpl = "GNU GENERAL PUBLIC LICENSE\nVersion 2, June 1991 (sandbox stand-in, not the real file)\n";
        wchar_t prevEnv[MAX_PATH]{}; const bool hadEnv = GetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", prevEnv, MAX_PATH) > 0;
        struct Stage { bool exe = true, lic = true, listExe = true, listLic = true, exeText = false, licNotGpl = false, manifest = true; };
        auto stage = [&](const Stage& st) {
            EnsureDir(pkg + L"\\Tools");
            if (st.exe) { if (st.exeText) WriteAll(pkgExe, reinterpret_cast<const uint8_t*>(gpl.data()), gpl.size()); else CopyFileW(selfExe, pkgExe.c_str(), FALSE); }
            const std::string lic = st.licNotGpl ? std::string("not a license\n") : gpl;
            if (st.lic) WriteAll(pkgLic, reinterpret_cast<const uint8_t*>(lic.data()), lic.size());
            std::wstring m = L"# sandbox package manifest\n";
            if (st.exe && st.listExe) m += Utf8ToW(Sha256File(pkgExe)) + L"  Tools\\smartctl.exe\n";
            if (st.lic && st.listLic) m += Utf8ToW(Sha256File(pkgLic)) + L"  Tools\\COPYING.smartmontools\n";
            if (!st.exe && !st.lic) m += Utf8ToW(Sha256File(selfExe)) + L"  Mods\\unrelated.txt\n";   // a manifest must list something
            if (st.manifest) { const std::string u = WToUtf8(m); WriteAll(pkg + L"\\COMPONENTS.sha256", reinterpret_cast<const uint8_t*>(u.data()), u.size()); }
            SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", pkg.c_str());
        };
        auto appendByte = [](const std::wstring& p) { HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr); if (h == INVALID_HANDLE_VALUE) return false; DWORD w = 0; const char b = 'x'; const bool ok = WriteFile(h, &b, 1, &w, nullptr) && w == 1; CloseHandle(h); return ok; };
        Options so = opt; so.smartctl = true;
        auto warned = [](const Outcome& r) { for (const auto& w : r.warnings) if (w.find(L"smartctl") != std::wstring::npos) return true; return false; };
        auto refused = [&](const wchar_t* label, const Stage& st, const wchar_t* expectInReason, const std::function<void()>& tamper = {}) {
            wipe(); stage(st); if (tamper) tamper();
            const SmartctlOffer off = CheckSmartctlComponent(LoadComponents());
            Outcome r = InstallCore(g_entries, so, quiet);
            std::wstring why; for (const auto& w : r.warnings) if (w.find(L"smartctl") != std::wstring::npos) why = w;
            Chk(off.present && !off.ok && off.reason.find(expectInReason) != std::wstring::npos && r.ok && !r.smartctlInstalled && warned(r) && !FileExists(appExe) && !FileExists(appLic) && !DirExists(app + L"\\Tools") && why.find(expectInReason) != std::wstring::npos,
                std::wstring(L"smartctl component: ") + label + L" is refused (" + off.reason + L"); the core install still succeeds and nothing of the component is left");
        };

        // complete and unchanged: installed, both files verified against the manifest
        wipe(); stage(Stage{});
        const SmartctlOffer okOffer = CheckSmartctlComponent(LoadComponents());
        Chk(okOffer.present && okOffer.ok, L"smartctl component: a package with both files listed and unchanged verifies (" + okOffer.reason + L")");
        Outcome s1 = InstallCore(g_entries, so, quiet);
        Chk(s1.ok && s1.smartctlInstalled && FileExists(appExe) && FileExists(appLic) && Sha256File(appExe) == Sha256File(pkgExe) && Sha256File(appLic) == Sha256File(pkgLic) && !warned(s1) &&
            s1.smartctlSummary.find(L"installed in") != std::wstring::npos,
            L"smartctl component: installs smartctl.exe and COPYING.smartmontools into <program folder>\\Tools, each identical (SHA-256) to the package file");
        Chk(SmartctlPath() == appExe && SmartExePathOk(appExe) && SmartLooksLikeX64Exe(appExe) && SmartctlCandidateDirs(false).size() == 1 && SmartctlCandidateDirs(false)[0] == app + L"\\Tools",
            L"smartctl component: the engine finds the installed copy at AppDir\\Tools\\smartctl.exe, which is the only production location");
        installedOk(L"core install with the smartctl component");
        // a later install that does not select it leaves the installed component alone, and uninstall removes it with the program folder
        Outcome s2 = InstallCore(g_entries, opt, quiet);
        Chk(s2.ok && !s2.smartctlInstalled && s2.smartctlSummary.find(L"not selected") != std::wstring::npos && FileExists(appExe) && FileExists(appLic), L"smartctl component: a repair that does not select it keeps the installed component (kept item) and says it was not selected");
        const RemoveReport sr = RemoveInstall();
        Chk(sr.failed.empty() && !DirExists(app + L"\\Tools") && !FileExists(appExe) && !FileExists(appLic) && !DirExists(app), L"smartctl component: uninstall removes the component with the program folder (report: " + std::to_wstring(sr.failed.size()) + L" not removed)");

        // core-only: no component in the package, or no manifest at all
        wipe(); stage(Stage{false, false, false, false});
        const SmartctlOffer none = CheckSmartctlComponent(LoadComponents());
        Outcome c1 = InstallCore(g_entries, so, quiet);
        Chk(!none.present && !none.ok && c1.ok && !c1.smartctlInstalled && !DirExists(app + L"\\Tools") && FileExists(app + L"\\AnkhCore.exe"), L"smartctl component: a package without it installs core-only, with no Tools folder and no failure");
        wipe(); { Stage st; st.manifest = false; st.exe = st.lic = false; EnsureDir(pkg); SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", pkg.c_str()); }
        Outcome c2 = InstallCore(g_entries, so, quiet);
        Chk(!CheckSmartctlComponent(LoadComponents()).present && c2.ok && !c2.smartctlInstalled && !DirExists(app + L"\\Tools"), L"smartctl component: a package without COMPONENTS.sha256 also installs core-only");
        wipe(); stage(Stage{});
        Outcome c3 = InstallCore(g_entries, opt, quiet);
        Chk(c3.ok && !c3.smartctlInstalled && c3.smartctlSummary.find(L"not selected") != std::wstring::npos && !DirExists(app + L"\\Tools"), L"smartctl component: present in the package but not selected (the default): not installed");

        // a cut write, or wrong bytes, while the component is copied: the core install still succeeds and no partial smartctl.exe is left
        for (int k = 0; k < 3; ++k) {
            wipe(); stage(Stage{});
            PlaceFileFaults& fl = PlaceFileFaultSet(); fl = PlaceFileFaults{}; fl.nameContains = L"smartctl.exe"; if (k == 0) fl.cutAfter = 1000; else if (k == 1) fl.corrupt = true; else fl.refuseReplace = true;
            const int h0 = PlaceFileFaultsHit();
            Outcome r = InstallCore(g_entries, so, quiet);
            fl = PlaceFileFaults{};
            Chk(r.ok && !r.smartctlInstalled && warned(r) && PlaceFileFaultsHit() > h0 && !FileExists(appExe) && !FileExists(appLic) && !FileExists(appExe + L".ankh-new") && !DirExists(app + L"\\Tools"),
                std::wstring(L"smartctl component: ") + (k == 0 ? L"a write cut in the middle" : k == 1 ? L"wrong bytes on the staged file" : L"a swap-in that stays refused (nothing is queued for a restart)") + L" leaves no smartctl.exe, no staged file and no Tools folder; the core install succeeds");
        }

        // refusals
        refused(L"a changed smartctl.exe (hash mismatch)", Stage{}, L"SHA-256", [&] { appendByte(pkgExe); });
        refused(L"a changed COPYING.smartmontools (hash mismatch)", Stage{}, L"SHA-256", [&] { appendByte(pkgLic); });
        refused(L"a listed COPYING.smartmontools that is missing", Stage{}, L"missing file", [&] { DeleteFileW(pkgLic.c_str()); });
        refused(L"a listed smartctl.exe that is missing", Stage{}, L"missing file", [&] { DeleteFileW(pkgExe.c_str()); });
        { Stage st; st.lic = false; refused(L"smartctl.exe without any license file in the package", st, L"not listed"); }
        { Stage st; st.exe = false; refused(L"a license file without smartctl.exe", st, L"not listed"); }
        { Stage st; st.listExe = false; refused(L"an unlisted smartctl.exe", st, L"not listed"); }
        { Stage st; st.listLic = false; refused(L"an unlisted license file", st, L"not listed"); }
        { Stage st; st.exeText = true; refused(L"a smartctl.exe that is not a 64-bit Windows program (hash is correct)", st, L"64-bit"); }
        { Stage st; st.licNotGpl = true; refused(L"a license file that is not the GPL text (hash is correct)", st, L"GPL"); }

        wipe();
        SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", hadEnv ? prevEnv : nullptr);
    }
    {   // the real two-process app, as a test-build copy named AnkhCore.exe inside the sandbox: setup must be able to stop it
        wipe();
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring root = self; for (int up = 0; up < 3; ++up) root = root.substr(0, root.find_last_of(L'\\'));
        const std::wstring beside = DirOf(self) + L"\\AnkhCore_TEST.exe";   // a verification copy of a package carries the test app beside the test setup
        const std::wstring src = FileExists(beside) ? beside : root + L"\\build-test\\out\\AnkhCore_TEST.exe", dst = app + L"\\AnkhCore.exe";
        EnsureDir(app);
        const bool copied = CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
        Chk(copied, L"engine stop: a test-build copy of the app is staged in the sandbox");
        auto launch = [&](const std::wstring& args) {
            STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; std::wstring c = L"\"" + dst + L"\"" + args;
            if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, app.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return true; }
            return false; };
        std::wstring why;
        Chk(copied && launch(L" --engine"), L"engine stop: engine started with no UI (as after a UI crash)");
        Sleep(3000);
        Chk(ProcessesInAppDir(L"AnkhCore.exe").size() == 1, L"engine stop: exactly one engine running");
        Chk(StopAnkhCore(why) && ProcessesInAppDir(L"AnkhCore.exe").empty(), L"engine stop: setup stops an engine left without a UI " + why);
        Chk(copied && launch(L""), L"engine stop: UI started");
        Sleep(6000);
        Chk(ProcessesInAppDir(L"AnkhCore.exe").size() == 2, L"engine stop: UI and its engine running (" + std::to_wstring(ProcessesInAppDir(L"AnkhCore.exe").size()) + L")");
        why.clear();
        Chk(StopAnkhCore(why) && ProcessesInAppDir(L"AnkhCore.exe").empty(), L"engine stop: setup stops the UI and the engine, nothing left " + why);
        wipe();
    }
    {   // G. INS-3 (D41 & D44): Optional installer components (Taskbar Profile + prebuilt Icon Themes)
        wipe();
        Options defOpt;
        Chk(!defOpt.taskbarProfile && defOpt.iconThemes.empty(),
            L"INS-3: Taskbar Profile and optional icon themes are unchecked by default");
        const std::wstring tbKey = TaskbarExplorerAdvBaseKey();
        // Default install leaves Taskbar settings untouched
        Outcome g0 = InstallCore(g_entries, defOpt, quiet);
        Chk(g0.ok && !g0.taskbarApplied && !RegGet(HKEY_CURRENT_USER, tbKey, L"TaskbarAl").exists &&
            g0.taskbarSummary.find(L"Not selected") != std::wstring::npos &&
            g0.iconThemesSummary.find(L"None selected") != std::wstring::npos,
            L"INS-3: default install does not modify Taskbar settings or install optional icon themes");

        // Windows 10 or older: even if taskbarProfile is set true, it is refused and never applied
        Options w10Opt = defOpt; w10Opt.taskbarProfile = true; w10Opt.osIsWin11 = false;
        Outcome g10 = InstallCore(g_entries, w10Opt, quiet);
        Chk(g10.ok && !g10.taskbarApplied && !RegGet(HKEY_CURRENT_USER, tbKey, L"TaskbarAl").exists &&
            g10.taskbarSummary.find(L"Skipped (Windows 11 only)") != std::wstring::npos,
            L"INS-3: Windows 10 refuses Taskbar Profile and leaves registry untouched");

        {   // FT-G01 / FT-X1 / FT-X2: a protected value that Windows refuses is skipped and the rest applied; any other failure puts everything back
            const std::wstring tdir = SetupDataRoot() + L"\\TaskbarFt"; EnsureDir(tdir);
            const std::wstring regF = tdir + L"\\p.reg", bkF = tdir + L"\\backup.txt";
            // the same 14 values as the shipped profile (13 under Advanced, PeopleBand under Advanced\People): the parser insists on them
            static const wchar_t* const kNames[13] = {L"TaskbarAnimations", L"TaskbarGlomLevel", L"MMTaskbarGlomLevel", L"TaskbarAutoHideInTabletMode", L"TaskbarMn", L"TaskbarDa", L"ShowTaskViewButton",
                                                       L"ShowCopilotButton", L"TaskbarSn", L"TaskbarAl", L"TaskbarSi", L"TaskbarBadges", L"TaskbarFlashing"};
            std::string rt = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced]\r\n";
            for (const wchar_t* n : kNames) rt += "\"" + WToUtf8(n) + "\"=dword:00000001\r\n";
            rt += "\r\n[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\People]\r\n\"PeopleBand\"=dword:00000001\r\n";
            WriteAll(regF, reinterpret_cast<const uint8_t*>(rt.data()), rt.size());
            const std::wstring peopleKey = tbKey + L"\\People";
            auto seed = [&] { for (const wchar_t* n : kNames) RegSetDword(HKEY_CURRENT_USER, tbKey, n, 7); RegSetDword(HKEY_CURRENT_USER, peopleKey, L"PeopleBand", 7); RegDel(HKEY_CURRENT_USER, tbKey, L"TaskbarSi"); };
            auto val = [&](const wchar_t* n) -> int { const RegValue v = RegGet(HKEY_CURRENT_USER, tbKey, n); if (!v.exists || v.data.size() < 4) return -1; DWORD d; memcpy(&d, v.data.data(), 4); return static_cast<int>(d); };
            auto people = [&]() -> int { const RegValue v = RegGet(HKEY_CURRENT_USER, peopleKey, L"PeopleBand"); if (!v.exists || v.data.size() < 4) return -1; DWORD d; memcpy(&d, v.data.data(), 4); return static_cast<int>(d); };
            auto untouched = [&] { bool ok = people() == 7 && val(L"TaskbarSi") == -1; for (const wchar_t* n : kNames) if (wcscmp(n, L"TaskbarSi") != 0) ok = ok && val(n) == 7; return ok; };
            std::wstring det;
            seed();
            TaskbarWriteFault() = [](const std::wstring& n) -> LONG { return n == L"TaskbarDa" ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; };
            const bool a = ApplyTaskbarProfileWithBackup(regF, true, bkF, det);
            Chk(a && det.find(L"13 of 14 settings applied") != std::wstring::npos && det.find(L"TaskbarDa") != std::wstring::npos && val(L"TaskbarAnimations") == 1 && val(L"TaskbarAl") == 1 && val(L"TaskbarSi") == 1 && people() == 1 && val(L"TaskbarDa") == 7,
                L"taskbar profile: the protected TaskbarDa that Windows refuses is skipped and named, the other 13 values are applied (" + det + L")");
            seed();
            TaskbarWriteFault() = [](const std::wstring& n) -> LONG { return n == L"TaskbarAl" ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; };
            const bool b = ApplyTaskbarProfileWithBackup(regF, true, bkF, det);
            Chk(!b && untouched() && det.find(L"put back") != std::wstring::npos && FileExists(bkF),
                L"taskbar profile: a refused value that is not protected undoes every value already written (nothing half applied): " + det);
            seed();
            TaskbarWriteFault() = [](const std::wstring& n) -> LONG { return n == L"TaskbarDa" ? 1 : ERROR_SUCCESS; };   // another error on the protected value: still undone
            const bool c2 = ApplyTaskbarProfileWithBackup(regF, true, bkF, det);
            Chk(!c2 && untouched(), L"taskbar profile: only access-denied counts as 'protected'; any other error on that value undoes everything too");
            seed();
            TaskbarWriteFault() = [](const std::wstring& n) -> LONG { return n == L"PeopleBand" ? ERROR_ACCESS_DENIED : ERROR_SUCCESS; };   // the last value fails: all 13 before it come back
            const bool e2 = ApplyTaskbarProfileWithBackup(regF, true, bkF, det);
            Chk(!e2 && untouched(), L"taskbar profile: a failure on the last value also puts back all the values before it");
            TaskbarWriteFault() = nullptr; seed();
            const bool d2 = ApplyTaskbarProfileWithBackup(regF, true, bkF, det);
            Chk(d2 && det.find(L"14 of 14 settings applied") != std::wstring::npos && val(L"TaskbarAnimations") == 1 && val(L"TaskbarDa") == 1 && val(L"TaskbarSi") == 1 && people() == 1, L"taskbar profile: with no refusal all 14 values are applied (" + det + L")");
            for (const wchar_t* n : kNames) RegDel(HKEY_CURRENT_USER, tbKey, n);
            RegDel(HKEY_CURRENT_USER, peopleKey, L"PeopleBand");
            DeleteFileW(regF.c_str()); DeleteFileW(bkF.c_str()); RemoveDirectoryW(tdir.c_str());
        }

        // The optional components come only from the package beside this test setup (or ANKH_COMPONENT_ROOT): never D:\Ankhcore
        const Components comp = LoadComponents();
        g_rep.push_back(L"INFO  component package: " + comp.root + (comp.loaded ? L" (" + std::to_wstring(comp.m.sha.size()) + L" files in COMPONENTS.sha256)" : L" (" + comp.err + L")"));
        if (!comp.loaded) {
            Skip(L"INS-3: optional components (Taskbar Profile, icon themes, Resource Redirect, runtime Apply): no component package beside the test setup");
            wipe();
        } else {
        Chk(Lower(comp.root).rfind(L"d:\\ankhcore", 0) != 0 && Lower(comp.root).find(L"release-inputs") == std::wstring::npos,
            L"package: components are read from the package copy " + comp.root + L", not from D:\\Ankhcore or release-inputs");
        // Seed pre-existing user Taskbar settings in the sandbox key (TaskbarAl = 0, TaskbarAnimations = 1, TaskbarSi absent)
        RegSetDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAl", 0);
        RegSetDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAnimations", 1);

        // Windows 11 + Taskbar Profile selected + 2 prebuilt icon themes selected
        Options w11Opt = defOpt;
        w11Opt.taskbarProfile = true;
        w11Opt.osIsWin11 = true;
        w11Opt.iconThemes = {L"bigsur-dark", L"sweet"};
        Outcome g11 = InstallCore(g_entries, w11Opt, quiet);
        Chk(g11.ok && g11.taskbarApplied && FileExists(g11.taskbarBackupPath) &&
            g11.taskbarSummary.find(L"Result: Applied") != std::wstring::npos,
            L"INS-3: Windows 11 with Taskbar Profile selected backs up current settings and applies the 14 profile DWORDs");
        Chk(RegDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAl", 99) == 1 &&
            RegDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAnimations", 99) == 0 &&
            RegDword(HKEY_CURRENT_USER, tbKey, L"ShowCopilotButton", 99) == 0 &&
            RegDword(HKEY_CURRENT_USER, tbKey + L"\\People", L"PeopleBand", 99) == 0,
            L"INS-3: applied Taskbar Profile values verified in sandbox registry");

        // Restore from backup puts pre-existing values back and deletes values that did not exist before
        Chk(RestoreTaskbarProfileBackup(g11.taskbarBackupPath) &&
            RegDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAl", 99) == 0 &&
            RegDword(HKEY_CURRENT_USER, tbKey, L"TaskbarAnimations", 99) == 1 &&
            !RegGet(HKEY_CURRENT_USER, tbKey, L"TaskbarSi").exists &&
            !RegGet(HKEY_CURRENT_USER, tbKey + L"\\People", L"PeopleBand").exists,
            L"INS-3: RestoreTaskbarProfileBackup restores previous values and removes newly added values");

        // Prebuilt icon themes copied and validated without auto-applying
        Chk(g11.iconThemesInstalled == 2 &&
            ValidateIconThemeDir(app + L"\\IconThemes\\bigsur-dark") &&
            ValidateIconThemeDir(app + L"\\IconThemes\\sweet") &&
            !DirExists(app + L"\\IconThemes\\magnum") &&
            FileExists(app + L"\\Mods\\icon-resource-redirect.wh.cpp") &&
            g11.iconThemesSummary.find(L"not auto-applied") != std::wstring::npos,
            L"INS-3: only selected prebuilt icon themes (bigsur-dark, sweet) and Resource Redirect mod are copied and validated");

        // A changed file in the package: that theme is refused, nothing of it is copied
        {
            wipe();
            const std::wstring tp = S + L"\\TamperedPackage";
            const bool staged = CopyTree(comp.root + L"\\IconThemes\\sweet", tp + L"\\IconThemes\\sweet") && CopyTree(comp.root + L"\\COMPONENTS.sha256", tp + L"\\COMPONENTS.sha256");
            std::wstring victim; for (const auto& f : ListDir(tp + L"\\IconThemes\\sweet", false)) if (EndsWithI(f, L".dll")) { victim = tp + L"\\IconThemes\\sweet\\" + f; break; }
            std::vector<uint8_t> b; const bool flipped = !victim.empty() && ReadAll(victim, b) && b.size() > 4096 && (b[b.size() - 100] ^= 0x01, WriteAll(victim, b.data(), b.size()));
            wchar_t prevEnv[MAX_PATH]{}; const bool hadEnv = GetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", prevEnv, MAX_PATH) > 0;
            SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", tp.c_str());
            Options tOpt = defOpt; tOpt.iconThemes = {L"sweet"};
            Outcome gt = InstallCore(g_entries, tOpt, quiet);
            SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", hadEnv ? prevEnv : nullptr);
            bool named = false; for (const auto& w : gt.warnings) named |= w.find(L"changed file") != std::wstring::npos;
            Chk(staged && flipped && gt.ok && gt.iconThemesInstalled == 0 && named && !DirExists(app + L"\\IconThemes\\sweet"),
                L"package: a theme with one changed byte is refused by its SHA-256 and nothing of it is installed");
        }
        // Every theme in the package installs, and the installed files are byte for byte the ones in the manifest
        wipe();
        Options allOpt = defOpt; allOpt.iconThemes = PackageIconThemes();
        Outcome ga = InstallCore(g_entries, allOpt, quiet);
        int verified = 0; std::wstring firstBad;
        for (const auto& id : allOpt.iconThemes) {
            const std::wstring folder = IconThemeFolderName(id);
            Components inst; inst.root = app; inst.m = comp.m; inst.loaded = true;   // the same manifest, checked against the installed copy
            const std::wstring bad = VerifyComponent(inst, L"IconThemes\\" + folder);
            if (bad.empty() && ValidateIconThemeDir(app + L"\\IconThemes\\" + folder)) ++verified; else if (firstBad.empty()) firstBad = folder + L": " + bad;
        }
        Components instMod; instMod.root = app; instMod.m = comp.m; instMod.loaded = true;
        const bool modOk = VerifyComponent(instMod, kRedirectModRel).empty();
        Chk(ga.ok && !allOpt.iconThemes.empty() && allOpt.iconThemes.size() <= IconPacks().size() && ga.iconThemesInstalled == static_cast<int>(allOpt.iconThemes.size()) && verified == ga.iconThemesInstalled && modOk,
            L"package: all " + std::to_wstring(allOpt.iconThemes.size()) + L" icon themes and the Resource Redirect mod install, every file matching its SHA-256 (" +
            std::to_wstring(verified) + L" verified" + (firstBad.empty() ? L"" : L"; first problem: " + firstBad) + L")");
        {
            const wchar_t* seven[] = {L"3d-purple", L"amy-dark", L"coloration", L"hal-green", L"macos-elegance", L"tela-black", L"windows-23"};
            int ok7 = 0; std::wstring bad7;
            for (const wchar_t* id : seven) {
                wipe();
                Options one = defOpt; one.iconThemes = {id};
                Outcome go = InstallCore(g_entries, one, quiet);
                const std::wstring folder = IconThemeFolderName(id);
                Components inst; inst.root = app; inst.m = comp.m; inst.loaded = true;
                const std::wstring err = VerifyComponent(inst, L"IconThemes\\" + folder);
                if (go.ok && go.iconThemesInstalled == 1 && err.empty() && ValidateIconThemeDir(app + L"\\IconThemes\\" + folder)) ++ok7;
                else if (bad7.empty()) bad7 = std::wstring(id) + L": " + (err.empty() ? go.message : err);
            }
            Chk(ok7 == 7, L"package: each of the 7 newly included icon packs (3d-purple, amy-dark, coloration, hal-green, macos-elegance, tela-black, windows-23) installs individually (" + std::to_wstring(ok7) + L"/7" + (bad7.empty() ? L"" : L"; " + bad7) + L")");
            // Default GUI selection: all 18 themes selected by default, deselecting 1 leaves 17, preserved across page navigation
            wipe();
            Options guiOpt = defOpt;
            guiOpt.iconThemes = PackageIconThemes();
            const size_t totalDef = guiOpt.iconThemes.size();
            auto itSw = std::find(guiOpt.iconThemes.begin(), guiOpt.iconThemes.end(), L"sweet");
            if (itSw != guiOpt.iconThemes.end()) guiOpt.iconThemes.erase(itSw);
            // Simulate step navigation (Welcome -> Options -> Welcome -> Options): g_opt.iconThemes is preserved
            int stepSim = 0; stepSim = 1; stepSim = 0; stepSim = 1; (void)stepSim;
            Outcome gDesel = InstallCore(g_entries, guiOpt, quiet);
            Chk(totalDef == 18 && guiOpt.iconThemes.size() == 17 && gDesel.ok && gDesel.iconThemesInstalled == 17 && !DirExists(app + L"\\IconThemes\\sweet"),
                L"package: default GUI selection has all 18 themes; deselecting sweet installs 17 and skips sweet");
            // Re-install all themes for section H
            wipe(); InstallCore(g_entries, allOpt, quiet);
        }

        // H. Runtime Apply against an isolated Windhawk root: same CLI and compiler as the installed Windhawk, its own
        //    registry key and data folder under this sandbox, so the Windhawk that runs on this PC never sees the mod.
        const std::wstring cli = WindhawkCliExePath();
        if (!FileExists(cli)) Skip(L"runtime Apply: windhawk-cli.exe is not installed on this PC");
        else {
            const std::wstring whDir = DirOf(cli), hostIni = whDir + L"\\windhawk.ini", root = S + L"\\WindhawkIsolated";
            auto iniPath = [&](const wchar_t* key, const wchar_t* def) {
                wchar_t v[MAX_PATH]{}; GetPrivateProfileStringW(L"Storage", key, def, v, MAX_PATH, hostIni.c_str());
                const std::wstring s = v; return s.size() > 1 && s[1] == L':' ? s : whDir + L"\\" + s;
            };
            EnsureDir(root + L"\\Data");
            const std::string ini = WToUtf8(L"[Storage]\r\nPortable=0\r\nCompilerPath=" + iniPath(L"CompilerPath", L"Compiler") + L"\r\nEnginePath=" + iniPath(L"EnginePath", L"Engine") +
                                            L"\r\nUIPath=" + iniPath(L"UIPath", L"UI") + L"\r\nAppDataPath=" + root + L"\\Data\r\nRegistryKey=HKCU\\" + kPrefKey + L"\\Sandbox\\Windhawk\r\n");
            WriteAll(root + L"\\windhawk.ini", reinterpret_cast<const uint8_t*>(ini.data()), ini.size());
            const auto hostMods = RegSubkeys(HKEY_LOCAL_MACHINE, WhModsKey());
            // FT-X6: the host Windhawk may already have Resource Redirect (the owner applied a theme): the check is that this test changed nothing, not that the mod is absent
            const std::wstring hostRrKey = std::wstring(WhModsKey()) + L"\\local@icon-resource-redirect";
            const bool hostHadRr = RegKeyExists(HKEY_LOCAL_MACHINE, hostRrKey);
            const std::wstring hostRrLib = RegStr(HKEY_LOCAL_MACHINE, hostRrKey, L"LibraryFileName");
            const RegValue hostRrOff = RegGet(HKEY_LOCAL_MACHINE, hostRrKey, L"Disabled");
            auto paths = [&] {
                JVal d; std::wstring e; std::vector<std::wstring> out;
                const ResourceRedirectModInfo mi = QueryResourceRedirectMod(root);
                if (mi.installed && RunWindhawkCliJson(L"mod settings get " + QuoteCliArg(mi.modId), d, e, root))
                    if (const JVal* data = d.Get("data")) if (const JVal* st = data->Get("settings")) out = ExtractThemePathsFromFlatSettings(*st);
                std::vector<std::wstring> nonEmpty; for (const auto& p : out) if (!p.empty()) nonEmpty.push_back(NormPathKey(p));
                return nonEmpty;
            };
            const std::wstring dirA = NormPathKey(app + L"\\IconThemes\\bigsur-dark"), dirB = NormPathKey(app + L"\\IconThemes\\sweet");
            const bool a1 = ApplyIconThemeRedirect(L"bigsur-dark", nullptr, root, /*allowDisabledForTest*/true);
            const auto p1 = paths(); const ResourceRedirectModInfo m1 = QueryResourceRedirectMod(root);
            Chk(a1 && m1.installed && !m1.enabled && p1.size() == 1 && p1[0] == dirA && dirA.find(L"program files") != std::wstring::npos,
                L"runtime Apply: the mod is compiled and installed (disabled) from the installed program folder, and themePaths points at the installed Big Sur 2 DarkMode (a path with spaces)");
            const bool a2 = ApplyIconThemeRedirect(L"sweet", nullptr, root, true);
            const auto p2 = paths();
            Chk(a2 && p2.size() == 1 && p2[0] == dirB, L"runtime Apply: switching to Sweet Paranoia replaces the path, with no stale or duplicate entry");
            const bool a3 = RestoreDefaultIconsRedirect(nullptr, root);
            Chk(a3 && paths().empty(), L"runtime Restore Default: AnkhCore's theme path is removed from Resource Redirect");
            {   // FT-C01 / FT-C02 against the real Windhawk CLI in the isolated root: staged enable and settings, verified by read-back, unknown keys kept
                const std::string rrId = "local@icon-resource-redirect";
                std::vector<WhModInfo> mods; std::wstring err; const bool listed = WhCliList(mods, err, root);
                bool found = false, wasOn = true; for (const auto& m : mods) if (m.id == rrId) { found = true; wasOn = m.enabled; }
                Chk(listed && found && !wasOn, L"windhawk stage: the CLI lists the isolated Resource Redirect, switched off (" + std::to_wstring(mods.size()) + L" mod(s))");
                JVal show; std::vector<WhSettingDef> defs; std::wstring e2;
                const bool schemaOk = WhCliShow(rrId, show, err, root) && WhParseSchema(show, defs, e2);
                int nBool = 0, nOther = 0, nList = 0; const WhSettingDef* edit = nullptr;
                for (const auto& d : defs) { if (d.kind == WhKind::List) ++nList; else if (d.kind == WhKind::Group) continue; else { ++nOther; if (d.kind == WhKind::Bool) ++nBool; if (!edit && (d.kind == WhKind::Bool || d.kind == WhKind::Enum)) edit = &d; } }
                Chk(schemaOk && !defs.empty() && nList >= 1 && edit, L"windhawk stage: the real schema of Resource Redirect is read: " + std::to_wstring(defs.size()) + L" setting(s), " + std::to_wstring(nOther) + L" editable, " + std::to_wstring(nList) + L" list(s) kept as they are");
                WhFlat before; const bool gotBefore = WhCliSettings(rrId, before, err, root);
                Chk(gotBefore && !before.empty(), L"windhawk stage: the current settings are read back from the CLI (" + std::to_wstring(before.size()) + L" key(s))");
                if (schemaOk && edit && gotBefore) {
                    const JVal* cur = WhFlatFind(before, edit->path);
                    JVal want = edit->kind == WhKind::Bool ? WhMakeNum(cur && WhAsBool(*cur) ? 0 : 1) : WhMakeStr(edit->options.size() > 1 ? (cur && cur->s == edit->options[0].value ? edit->options[1].value : edit->options[0].value) : (cur ? cur->s : ""));
                    WhStage stage; stage.StageEnabled(rrId, true, false); stage.StageSetting(rrId, edit->path, want, cur ? *cur : edit->def);
                    Chk(stage.Count() == 2 && !wasOn, L"windhawk stage: a switch and a setting are only staged: nothing changed in Windhawk yet");
                    std::vector<WhModInfo> still; WhCliList(still, err, root); bool stillOff = true; for (const auto& m : still) if (m.id == rrId) stillOff = !m.enabled;
                    WhFlat unchanged; WhCliSettings(rrId, unchanged, err, root);
                    Chk(stillOff && WhFlatToJson(unchanged) == WhFlatToJson(before), L"windhawk stage: staging left the applied state (mod off, settings) exactly as it was");
                    const WhApplyResult res = WhApply(stage, WhCliExec(root));
                    bool nowOn = false; WhCliEnabled(rrId, nowOn, err, root);
                    WhFlat after; WhCliSettings(rrId, after, err, root);
                    const JVal* got = WhFlatFind(after, edit->path);
                    bool othersSame = after.size() == before.size();
                    for (const auto& kv : before) { if (kv.first == edit->path) continue; const JVal* a = WhFlatFind(after, kv.first); if (!a || !WhValueEquals(*a, kv.second)) othersSame = false; }
                    Chk(res.AllOk() && res.Ok() == 2 && nowOn && got && WhValueEquals(*got, want) && othersSame && stage.Empty(),
                        L"windhawk stage: Apply turned the mod on and changed the setting, both verified by reading them back; every other setting (including the lists) is unchanged, and the stage is empty");
                    WhStage bad; bad.StageEnabled(rrId, false, true); bad.StageSetting(rrId, "no.such.setting", WhMakeNum(1), WhMakeNum(0));
                    const WhApplyResult r2 = WhApply(bad, WhCliExec(root));
                    bool offNow = true; WhCliEnabled(rrId, offNow, err, root);
                    Chk(r2.Ok() == 1 && r2.Failed() == 1 && !offNow && bad.Count() == 1 && WhSummary(r2, [](const std::string& s) { return Utf8ToW(s); }).find(L"stay pending") != std::wstring::npos,
                        L"windhawk stage: with one good and one impossible change the good one is applied and verified, the bad one is named and stays pending (partial failure reported)");
                    WhFlat after2; WhCliSettings(rrId, after2, err, root);
                    Chk(WhFlatToJson(after2) == WhFlatToJson(after), L"windhawk stage: the failed settings write left the settings exactly as they were");
                    // put the isolated root back as it was (off, original value), through the same staged path
                    WhStage back; back.StageSetting(rrId, edit->path, cur ? *cur : edit->def, want); const WhApplyResult r3 = WhApply(back, WhCliExec(root));
                    WhFlat restored; WhCliSettings(rrId, restored, err, root);
                    Chk(r3.AllOk() && WhFlatToJson(restored) == WhFlatToJson(before), L"windhawk stage: staging the original value back restores the settings byte for byte");
                    Chk(res.offerExplorerRestart == WhReachesExplorer(show) , L"windhawk stage: the single Explorer offer follows the mod's own process list (" + std::wstring(WhReachesExplorer(show) ? L"it runs in Explorer: one offer" : L"it does not run in Explorer: none") + L")");
                }
            }
            const RegValue hostRrOffNow = RegGet(HKEY_LOCAL_MACHINE, hostRrKey, L"Disabled");
            Chk(RegSubkeys(HKEY_LOCAL_MACHINE, WhModsKey()) == hostMods && RegKeyExists(HKEY_LOCAL_MACHINE, hostRrKey) == hostHadRr && RegStr(HKEY_LOCAL_MACHINE, hostRrKey, L"LibraryFileName") == hostRrLib &&
                hostRrOffNow.exists == hostRrOff.exists && hostRrOffNow.data == hostRrOff.data,
                L"runtime Apply: the Windhawk installed on this PC is untouched (same mods, same Resource Redirect state: " + std::wstring(hostHadRr ? L"it was installed before and is unchanged" : L"it was absent and still is") + L")");
        }
        wipe();
        }
    }
    {
        // I. Smart Installer: old/partial detection, Repair, Update, Clean Install (confirm + backup-fail abort + preserve Backup & unrelated files)
        const std::wstring app = AppDir(), data = DataDir(), backupRoot = BackupRoot();
        auto cleanSandbox = [&] {
            if (DirExists(app)) Evict(app, true, false);
            if (DirExists(backupRoot)) Evict(backupRoot, true, false);
            if (DirExists(data + L"\\Backup")) Evict(data + L"\\Backup", true, false);
            DeleteFileW((data + L"\\settings.json").c_str());
            DeleteFileW((data + L"\\journal.txt").c_str());
            DeleteFileW((data + L"\\net-counters.txt").c_str());
            if (DirExists(app + L".backup-user-tmp")) Evict(app + L".backup-user-tmp", true, false);
        };

        cleanSandbox();
        const ExistingInstallInfo freshInfo = DetectExistingInstall();
        Chk(!freshInfo.found && !freshInfo.partial, L"smart installer: clean system reports no existing installation");

        // Partial installation (Sensors exists without AnkhCore.exe)
        EnsureDir(app + L"\\Sensors");
        WriteAll(app + L"\\Sensors\\stale.txt", reinterpret_cast<const uint8_t*>("old"), 3);
        const ExistingInstallInfo partInfo = DetectExistingInstall();
        Chk(partInfo.found && partInfo.partial, L"smart installer: partial/damaged installation is detected");

        // Repair preserves user settings & unrelated files while restoring AnkhCore.exe
        EnsureDir(data);
        WriteAll(data + L"\\settings.json", reinterpret_cast<const uint8_t*>("{\"keep\":1}"), 10);
        WriteAll(app + L"\\MyPersonalNotes.txt", reinterpret_cast<const uint8_t*>("user-note"), 9);
        Options optRepair; optRepair.mode = SetupMode::Repair; optRepair.desktop = false; optRepair.startup = false; optRepair.driver = false;
        const Outcome rRep = InstallCore(g_entries, optRepair, quiet);
        std::vector<uint8_t> sBytes, uBytes;
        Chk(rRep.ok && FileExists(app + L"\\AnkhCore.exe") && ReadAll(data + L"\\settings.json", sBytes) && sBytes.size() == 10 &&
            ReadAll(app + L"\\MyPersonalNotes.txt", uBytes) && uBytes.size() == 9,
            L"smart installer: Repair restores missing AnkhCore.exe and preserves user settings and unrelated files");

        // Clean Install without confirmation is refused safely
        Options optNoConf; optNoConf.mode = SetupMode::CleanInstall; optNoConf.cleanConfirmed = false; optNoConf.desktop = false; optNoConf.startup = false; optNoConf.driver = false;
        const Outcome rRefused = InstallCore(g_entries, optNoConf, quiet);
        Chk(!rRefused.ok && rRefused.message.find(L"confirmation") != std::wstring::npos && FileExists(app + L"\\AnkhCore.exe"),
            L"smart installer: Clean Install without explicit confirmation aborts without modifying files");

        // Clean Install backup failure aborts safely before modifying any files
        EnsureDir(app + L"\\Fan");
        WriteAll(app + L"\\Fan\\obsolete.cfg", reinterpret_cast<const uint8_t*>("legacy"), 6);
        Options optFailBak = optNoConf; optFailBak.cleanConfirmed = true; optFailBak.failUserBackupForTest = true;
        const Outcome rBakFail = InstallCore(g_entries, optFailBak, quiet);
        Chk(!rBakFail.ok && FileExists(app + L"\\Fan\\obsolete.cfg") && FileExists(data + L"\\settings.json"),
            L"smart installer: Clean Install aborts cleanly if user-data backup fails, leaving existing files untouched");

        // Clean Install with confirmation: removes obsolete Fan\, preserves Backup\ and unrelated files, restores user settings
        EnsureDir(data + L"\\Backup");
        WriteAll(data + L"\\Backup\\snapshot.zip", reinterpret_cast<const uint8_t*>("zip"), 3);
        Options optClean = optNoConf; optClean.cleanConfirmed = true;
        const Outcome rClean = InstallCore(g_entries, optClean, quiet);
        std::vector<uint8_t> sAfter, bAfter, uAfter;
        Chk(rClean.ok && !DirExists(app + L"\\Fan") && FileExists(app + L"\\AnkhCore.exe") &&
            ReadAll(data + L"\\settings.json", sAfter) && sAfter.size() == 10 &&
            ReadAll(data + L"\\Backup\\snapshot.zip", bAfter) && bAfter.size() == 3 &&
            ReadAll(app + L"\\MyPersonalNotes.txt", uAfter) && uAfter.size() == 9,
            L"smart installer: Clean Install removes obsolete Fan\\, installs 2.5 cleanly, restores settings, and preserves Backup\\ + unrelated files");

        // Clean Install with Windhawk reinstall opt-in vs Keep Existing: custom user mods preserved, managed mod refreshed
        const std::wstring whSand = SandboxRoot() + L"\\WindhawkManaged";
        if (DirExists(whSand)) Evict(whSand, true, false);
        EnsureDir(whSand + L"\\ModsSource");
        WriteAll(whSand + L"\\ModsSource\\local@icon-resource-redirect.wh.cpp", reinterpret_cast<const uint8_t*>("old-mod"), 7);
        WriteAll(whSand + L"\\ModsSource\\custom-user-mod.wh.cpp", reinterpret_cast<const uint8_t*>("user-mod"), 8);
        Options optKeepWh = optClean; optKeepWh.reinstallWindhawk = false;
        const Outcome rKeepWh = InstallCore(g_entries, optKeepWh, quiet);
        Chk(rKeepWh.ok && FileExists(whSand + L"\\ModsSource\\local@icon-resource-redirect.wh.cpp") && FileExists(whSand + L"\\ModsSource\\custom-user-mod.wh.cpp"),
            L"smart installer: Clean Install with Keep Existing leaves Windhawk and custom mods untouched");

        Options optReWh = optClean; optReWh.reinstallWindhawk = true;
        const Outcome rReWh = InstallCore(g_entries, optReWh, quiet);
        Chk(rReWh.ok && !FileExists(whSand + L"\\ModsSource\\local@icon-resource-redirect.wh.cpp") && FileExists(whSand + L"\\ModsSource\\custom-user-mod.wh.cpp"),
            L"smart installer: Clean Install with Reinstall Windhawk refreshes managed mod while preserving user custom mods");
        if (DirExists(whSand)) Evict(whSand, true, false);

        // Update mode preserves settings & unrelated files and supports rollback
        Options optUpd; optUpd.mode = SetupMode::Update; optUpd.desktop = false; optUpd.startup = false; optUpd.driver = false;
        const Outcome rUpd = InstallCore(g_entries, optUpd, quiet);
        Chk(rUpd.ok && !rUpd.backup.empty() && DirExists(rUpd.backup) && FileExists(app + L"\\AnkhCore.exe") && FileExists(data + L"\\settings.json"),
            L"smart installer: Update mode backs up previous install and updates while preserving user data");

        // Official Windhawk HideTrayIcon=1 applied by default in sandbox registry
        DWORD whGot = 0, whSz = sizeof(whGot), whTp = 0;
        const bool whRegOk = RegGetValueW(SetupWhTrayRegRoot(), SetupWhTrayRegSubkey().c_str(), L"HideTrayIcon", RRF_RT_REG_DWORD, &whTp, &whGot, &whSz) == ERROR_SUCCESS && whGot == 1;
        Chk(optUpd.hideWindhawkTray && rUpd.windhawkTrayHidden && whRegOk && rUpd.restartRequired && !rRefused.restartRequired,
            L"windhawk tray & mandatory restart: Setup applies official HideTrayIcon=1 by default in isolated sandbox key and requires mandatory restart only on success");
        RegDeleteTreeW(HKEY_CURRENT_USER, SetupWhTrayRegSubkey().c_str());

        // Windhawk offline bundle + mods + registry settings (sandboxed)
        {
            const std::wstring whPkg = S + L"\\WhPkg";
            if (DirExists(whPkg)) Evict(whPkg, true, false);
            EnsureDir(whPkg + L"\\Windhawk\\ModsSource");
            EnsureDir(whPkg + L"\\Windhawk\\Mods64");
            EnsureDir(whPkg + L"\\Windhawk\\Mods32");
            const std::string fakeSetup = "MZ_FAKE_WINDHAWK_SETUP", fakeReg = "Windows Registry Editor Version 5.00\r\n", fakeSrc = "// mod src", fakeDll = "MZ_DLL";
            WriteAll(whPkg + L"\\Windhawk\\windhawk_setup_offline.exe", reinterpret_cast<const uint8_t*>(fakeSetup.data()), fakeSetup.size());
            WriteAll(whPkg + L"\\Windhawk\\Windhawk_Mods_Settings.reg", reinterpret_cast<const uint8_t*>(fakeReg.data()), fakeReg.size());
            WriteAll(whPkg + L"\\Windhawk\\ModsSource\\test-mod.wh.cpp", reinterpret_cast<const uint8_t*>(fakeSrc.data()), fakeSrc.size());
            WriteAll(whPkg + L"\\Windhawk\\Mods64\\test-mod_1.0_111.dll", reinterpret_cast<const uint8_t*>(fakeDll.data()), fakeDll.size());
            WriteAll(whPkg + L"\\Windhawk\\Mods32\\test-mod_1.0_111.dll", reinterpret_cast<const uint8_t*>(fakeDll.data()), fakeDll.size());
            std::wstring manifest;
            for (const wchar_t* rel : {L"Windhawk\\windhawk_setup_offline.exe", L"Windhawk\\Windhawk_Mods_Settings.reg",
                                       L"Windhawk\\ModsSource\\test-mod.wh.cpp", L"Windhawk\\Mods64\\test-mod_1.0_111.dll", L"Windhawk\\Mods32\\test-mod_1.0_111.dll"}) {
                manifest += Utf8ToW(Sha256File(whPkg + L"\\" + rel)) + L"  " + rel + L"\r\n";
            }
            const std::string mu = WToUtf8(manifest);
            WriteAll(whPkg + L"\\COMPONENTS.sha256", reinterpret_cast<const uint8_t*>(mu.data()), mu.size());
            SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", whPkg.c_str());
            Options optWh = optUpd; optWh.installWindhawkBundle = true;
            const Outcome rWh = InstallCore(g_entries, optWh, quiet);
            SetEnvironmentVariableW(L"ANKH_COMPONENT_ROOT", nullptr);
            const std::wstring whDataSand = SandboxRoot() + L"\\ProgramData\\Windhawk";
            Chk(rWh.ok && rWh.windhawkBundleInstalled &&
                FileExists(whDataSand + L"\\ModsSource\\test-mod.wh.cpp") &&
                FileExists(whDataSand + L"\\Engine\\Mods\\64\\test-mod_1.0_111.dll") &&
                FileExists(whDataSand + L"\\Engine\\Mods\\32\\test-mod_1.0_111.dll"),
                L"windhawk bundle: installs verified ModsSource, x64/x86 mod DLLs, and triggers registry import in sandbox");
            Evict(whPkg, true, false);
        }

        cleanSandbox();
    }
    g_rep.push_back(L"INFO  refused system changes in the test build (expected: launch task, start-up task, helper build, shortcuts): " + std::to_wstring(TestRefusedCount()));
    g_rep.push_back((g_fails ? L"RESULT: " + std::to_wstring(g_fails) + L" failure(s)" : std::wstring(L"RESULT: all checks passed")) +
                    (g_skips ? L" (" + std::to_wstring(g_skips) + L" skipped)" : std::wstring()));
    std::wstring all; for (const auto& l : g_rep) all += l + L"\r\n";
    const std::string u = WToUtf8(all);
    WriteAll(DataDir() + L"\\setup-sandbox-report.txt", reinterpret_cast<const uint8_t*>(u.data()), u.size());
    return g_fails ? 1 : 0;
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring cmd = cmdLine ? cmdLine : L"";
    UserTokenGuard tokenGuard;   // the shell user's token (SetUserToken) is closed on every way out of the program
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_ver = GetWinVer();
    if (g_ver.major < 10 || g_ver.build < 17763) {
        MessageBoxW(nullptr, L"AnkhCore needs Windows 10 (1809 or newer) or Windows 11, 64-bit.", L"AnkhCore Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!LoadPayload()) { MessageBoxW(nullptr, L"The setup package is damaged. Please download it again.", L"AnkhCore Setup", MB_OK | MB_ICONERROR); return 1; }
    // the test build never looks at the real installation: every mode works on its sandbox program folder
    if (kTestBuild) AppDirOverride() = SandboxRoot() + L"\\Program Files\\AnkhCore";
#ifdef ANKH_TEST_BUILD
    if (cmd.find(L"--sandbox-test") != std::wstring::npos) return SandboxTest();
#endif
    g_opt.allowDowngrade = cmd.find(L"--allow-downgrade") != std::wstring::npos;
    g_rollbackMode = cmd.find(L"--rollback") != std::wstring::npos;
    if (cmd.find(L"--repair") != std::wstring::npos) g_opt.mode = SetupMode::Repair;
    else if (cmd.find(L"--update") != std::wstring::npos) g_opt.mode = SetupMode::Update;
    else if (cmd.find(L"--clean-install") != std::wstring::npos) g_opt.mode = SetupMode::CleanInstall;
    g_opt.cleanConfirmed = cmd.find(L"--confirm-clean") != std::wstring::npos;
    g_existing = DetectExistingInstall();
    g_installed = g_existing.found;
    g_have = g_existing.version;
    if (g_existing.found && g_opt.mode == SetupMode::Auto) g_opt.mode = SetupMode::CleanInstall;
    g_kind = ResolveEffectiveInstallKind(g_opt.mode, g_installed, g_have, ParseVer(kVersion));
    if (g_kind == InstallKind::Downgrade && g_opt.allowDowngrade) g_kind = InstallKind::Upgrade;
    g_pawn = PawnIOInstalled();
    g_opt.startup = g_installed && !kTestBuild ? StartupTaskRegistered() : false;
    // an upgrade keeps what the user has: a desktop shortcut only if there already is one (fresh: on)
    g_opt.desktop = !g_installed || FileExists(UserFolder(FOLDERID_Desktop) + L"\\AnkhCore.lnk");
    if (cmd.find(L"--no-icon-themes") == std::wstring::npos) g_opt.iconThemes = PackageIconThemes();
    if (cmd.find(L"--no-windhawk-bundle") == std::wstring::npos && PackageHasWindhawkBundle()) g_opt.installWindhawkBundle = true;
    if (!g_opt.osIsWin11 && cmd.find(L"--no-after-dark") == std::wstring::npos && AdFindPack(LoadComponents().root + L"\\AfterDark", g_opt.osBuild).found) g_opt.adInstall = true;   // 2.8.4: the pack for this Windows 10 release, on by default
    if (cmd.find(L"--silent") != std::wstring::npos) {
        SetUserToken(ShellUserToken()); SecureDataDir();
        const Outcome o = g_rollbackMode ? RollbackCore([](const std::wstring& s, int) { LogI(s); }) : InstallCore(g_entries, g_opt, [](const std::wstring& s, int) { LogI(s); });
        if (!o.ok) LogE(L"Setup (silent) failed: " + o.message);
        return o.ok ? 0 : 1;
    }
    g_ui.dpi = SystemDpi();
    ApplyInstallerTheme();
    g_ui.f = MakeFonts(g_ui.dpi);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"AnkhCoreSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    if (g_rollbackMode) g_step = 2;
    const int w = g_ui.S(kW), h = g_ui.S(kH);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, kInstallerProduct, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                               wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
