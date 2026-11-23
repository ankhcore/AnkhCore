// AnkhCore 2.2 — shared base: strings, paths, files, registry, processes,
// privileges, the signed-in user, activity log and the rollback journal.
// Developer: As3aD
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>   // before windows.h: iphlpapi.h only declares the Vista+ interface tables with Winsock 2
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <srrestoreptapi.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <aclapi.h>
#include <sddl.h>
#include <winternl.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>
#include <algorithm>
#include <memory>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cwchar>
#include "op_result.h"

namespace ak {

constexpr wchar_t kProduct[] = L"AnkhCore";              // technical product id: install folder, data folders, tasks. NEVER renamed for display reasons.
// 2.7 product identity (display text only; the technical identifiers above and below stay as they are):
//   parent brand "AnkhCore"; product "AnkhCore Control Center"; installer "AnkhCore Setup"; helper "AnkhCore Sensors"; mod "AnkhCore Anubis".
constexpr wchar_t kBrand[] = L"AnkhCore";
constexpr wchar_t kProductFull[] = L"AnkhCore Control Center";
constexpr wchar_t kInstallerProduct[] = L"AnkhCore Setup";
constexpr wchar_t kSensorsProduct[] = L"AnkhCore Sensors";   // shown to people; the file stays AnkhSensors.exe
constexpr wchar_t kAnubisProduct[] = L"AnkhCore Anubis";     // shown to people; the Windhawk mod id stays anubis-windows-animations
// The main window title. It MUST start with the brand: the setup finds and closes a running copy by that prefix (setup.cpp, wcsncmp(title, L"AnkhCore", 8)).
inline std::wstring MainWindowTitle(bool testBuild) { return testBuild ? std::wstring(kProductFull) + L" (test build)" : std::wstring(kProductFull); }
constexpr wchar_t kVersion[] = L"2.11.0.0";              // numeric: what the setup compares (the fourth part is the build)
#ifdef ANKH_BETA_BUILD
constexpr bool kIsBetaBuild = true;
constexpr int kBetaTrialDays = 30;
constexpr wchar_t kVersionLabel[] = L"2.11 Beta (30-Day Trial)";
#else
constexpr bool kIsBetaBuild = false;
constexpr int kBetaTrialDays = 0;
constexpr wchar_t kVersionLabel[] = L"2.11 Beta - Feature Freeze";   // what people see
#endif
// Fan control stays off until the exit handover (P11) and the thermal-override thresholds (P12) are decided
// and tested on hardware with the owner's approval (D30). Off = monitoring only: no page offers a control and the
// engine refuses a manual speed. Returning fans to automatic is always allowed.
// 2026-11-19 (later): the OWNER re-enabled fan control for the 2.7 release (the 2.5.2 build had it on and worked on his hardware; 2.7 had switched it off by mistake of a recalled rule).
// The safety rules stay: thermal override, BIOS-auto restore, guard (eng_fanguard.h).
constexpr bool kFanControlEnabled = true;
constexpr wchar_t kDeveloper[] = L"As3aD";
constexpr wchar_t kThemeName[] = L"As3aD";
constexpr wchar_t kRunOnceValue[] = L"AnkhCoreTheme";
constexpr wchar_t kTaskName[] = L"\\AnkhCore\\Launch";
struct LinkDef { const wchar_t* text; const wchar_t* url; };
constexpr LinkDef kLinks[3] = {{L"Facebook Page", L"https://www.facebook.com/AnkhCore.Pts"},
                               {L"Developer", L"https://www.facebook.com/As3aD.Elba7ary"},
                               {L"WhatsApp", L"https://wa.me/as3ad"}};

// ---------------------------------------------------------------- strings
inline bool StartsWith(const std::wstring& s, const wchar_t* pre) { return s.rfind(pre, 0) == 0; }
inline bool EndsWithI(const std::wstring& s, const std::wstring& suf) {
    return s.size() >= suf.size() && _wcsicmp(s.c_str() + s.size() - suf.size(), suf.c_str()) == 0;
}
inline std::wstring Lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
inline std::wstring Slashes(std::wstring s) { for (auto& c : s) if (c == L'/') c = L'\\'; return s; }
inline std::wstring DirOf(const std::wstring& p) { const size_t i = p.find_last_of(L'\\'); return i == std::wstring::npos ? p : p.substr(0, i); }
inline std::wstring NameOf(const std::wstring& p) { const size_t i = p.find_last_of(L'\\'); return i == std::wstring::npos ? p : p.substr(i + 1); }
inline std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
inline std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring Hex(const uint8_t* p, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
inline std::vector<uint8_t> Unhex(const std::wstring& s) {
    std::vector<uint8_t> v;
    auto h = [](wchar_t c) { return c <= L'9' ? c - L'0' : (c | 32) - L'a' + 10; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(static_cast<uint8_t>(h(s[i]) * 16 + h(s[i + 1])));
    return v;
}
inline std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out; size_t a = 0;
    for (;;) { const size_t b = s.find(sep, a); out.push_back(s.substr(a, b == std::wstring::npos ? b : b - a)); if (b == std::wstring::npos) break; a = b + 1; }
    return out;
}
inline std::wstring FormatBytes(uint64_t b) {
    wchar_t buf[64];
    if (b >= (1ull << 30)) swprintf(buf, 64, L"%.2f GB", b / 1073741824.0);
    else if (b >= (1ull << 20)) swprintf(buf, 64, L"%.1f MB", b / 1048576.0);
    else if (b >= 1024) swprintf(buf, 64, L"%.0f KB", b / 1024.0);
    else swprintf(buf, 64, L"%llu B", static_cast<unsigned long long>(b));
    return buf;
}
inline std::wstring NowText(bool date = false) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[64];
    if (date) swprintf(b, 64, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else swprintf(b, 64, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return b;
}

// ---------------------------------------------------------------- paths and files
inline std::wstring KnownFolder(REFKNOWNFOLDERID id, HANDLE token = nullptr) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, token, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}
inline std::wstring WinDir() { wchar_t b[MAX_PATH]; GetWindowsDirectoryW(b, MAX_PATH); return b; }
inline std::wstring SysDir() { wchar_t b[MAX_PATH]; GetSystemDirectoryW(b, MAX_PATH); return b; }
inline std::wstring Expand(const std::wstring& s) {
    wchar_t b[2048]; const DWORD n = ExpandEnvironmentStringsW(s.c_str(), b, 2048);
    return n && n < 2048 ? std::wstring(b) : s;
}
inline std::wstring SelfPath() { wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH); return b; }
// The test build's setup sandbox redirects the program folder into the test area (empty = the real one).
inline std::wstring& AppDirOverride() { static std::wstring s; return s; }
inline std::wstring AppDir() { return !AppDirOverride().empty() ? AppDirOverride() : KnownFolder(FOLDERID_ProgramFiles) + L"\\AnkhCore"; }
inline std::wstring AppExe() { return AppDir() + L"\\AnkhCore.exe"; }
// ---- isolated test build (2.5 development) ---------------------------------
// Built with /DANKH_TEST_BUILD: runs unelevated beside the installed AnkhCore,
// keeps its own data in %LOCALAPPDATA%\AnkhCore-Test and refuses every system
// change (registry outside its own key, files outside its data folder, child
// processes, process kills, service and driver work). Production builds are
// unaffected: kTestBuild is false and every guard below compiles away.
#ifdef ANKH_TEST_BUILD
constexpr bool kTestBuild = true;
constexpr wchar_t kInstanceTag[] = L".Test";
constexpr wchar_t kPrefKey[] = L"Software\\AnkhCore-Test";
#else
constexpr bool kTestBuild = false;
constexpr wchar_t kInstanceTag[] = L"";
constexpr wchar_t kPrefKey[] = L"Software\\AnkhCore";
#endif
inline std::wstring ProdDataDir() { return KnownFolder(FOLDERID_ProgramData) + L"\\AnkhCore"; }
inline std::wstring DataDir() { return kTestBuild ? KnownFolder(FOLDERID_LocalAppData) + L"\\AnkhCore-Test" : ProdDataDir(); }
inline std::wstring Tagged(const wchar_t* name) { return std::wstring(name) + kInstanceTag; }
inline void TestRefused(const std::wstring& what);   // defined after the log
inline bool InTestArea(const std::wstring& path) {
    const std::wstring root = DataDir() + L"\\";
    return path.size() > root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 && path.find(L"..") == std::wstring::npos;
}
inline std::wstring ExecutableDir() {
    if (!AppDirOverride().empty()) return AppDirOverride();
    const std::wstring selfDir = DirOf(SelfPath());
    if (!selfDir.empty() && GetFileAttributesW((selfDir + L"\\Themetools\\As3aD.theme").c_str()) != INVALID_FILE_ATTRIBUTES)
        return selfDir;
    return AppDir();
}
inline std::wstring ThemetoolsDir() { return ExecutableDir() + L"\\Themetools"; }
inline std::wstring AppLocalAs3aDThemePath() { return ThemetoolsDir() + L"\\As3aD.theme"; }
inline std::wstring SuxDir() {
    const std::wstring tt = ThemetoolsDir();
    if (GetFileAttributesW((tt + L"\\ThemeDll.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return tt;
    return AppDir() + L"\\SecureUxTheme";
}
inline void EnsureDir(const std::wstring& dir) {
    if (kTestBuild && !InTestArea(dir + L"\\x") ) { TestRefused(L"create folder " + dir); return; }
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
}
inline bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool WriteAll(const std::wstring& path, const uint8_t* data, uint64_t size) {
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"write " + path); return false; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint64_t done = 0; bool ok = true;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(f, data + done, chunk, &wrote, nullptr) || wrote != chunk) { ok = false; break; }
        done += chunk;
    }
    CloseHandle(f);
    return ok;
}
inline bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t done = 0; bool ok = true;
    while (done < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 20));
        if (!ReadFile(f, out.data() + done, want, &got, nullptr) || !got) { ok = false; break; }
        done += got;
    }
    CloseHandle(f);
    return ok;
}
// Crash-safe replace: write a sibling temp file, flush it, then atomically swap it in.
// A power cut leaves either the old or the new content, never a truncated file.
inline bool WriteAtomic(const std::wstring& path, const uint8_t* data, uint64_t size) {
    const std::wstring tmp = path + L".tmp";
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"write " + path); return false; }
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint64_t done = 0; bool ok = true;
    while (done < size && ok) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size - done, 1u << 20)); DWORD w = 0;
        ok = WriteFile(f, data + done, chunk, &w, nullptr) && w == chunk; done += chunk;
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}
// WriteAll plus FlushFileBuffers, for a file that is about to be renamed over a live one: without the flush the rename can reach the disk before the
// file's data does, and a power cut then leaves the live file empty or cut. A failed write removes its own partial file.
inline bool WriteAllFlushed(const std::wstring& path, const uint8_t* data, uint64_t size) {
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"write " + path); return false; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint64_t done = 0; bool ok = true;
    while (done < size && ok) {
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size - done, 1u << 20)); DWORD w = 0;
        ok = WriteFile(f, data + done, chunk, &w, nullptr) && w == chunk; done += chunk;
    }
    ok = ok && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
inline std::string ReadText(const std::wstring& path) {
    std::vector<uint8_t> v; if (!ReadAll(path, v)) return {};
    return std::string(v.begin(), v.end());
}
// Faults the tests inject into PlaceFile (all off by default; only test code ever sets them, no production path does).
struct PlaceFileFaults {
    std::wstring nameContains;   // empty = every file, else only paths containing this text
    int64_t cutAfter = -1;       // >= 0: the write stops after this many bytes (a power cut or a full disk in the middle of the write)
    bool corrupt = false;        // flip one byte of what is written (a bad disk sector, a wrong buffer)
    bool refuseReplace = false;  // the final swap-in is refused (a file that stays locked)
    bool Hits(const std::wstring& p) const { return nameContains.empty() ? Armed() : (Armed() && p.find(nameContains) != std::wstring::npos); }
    bool Armed() const { return cutAfter >= 0 || corrupt || refuseReplace; }
};
inline PlaceFileFaults& PlaceFileFaultSet() { static PlaceFileFaults f; return f; }
inline int& PlaceFileFaultsHit() { static int n = 0; return n; }
// Installing a file (WT-04 / hardening 2026-11-17). The live file is NEVER written in place: the new content goes to a sibling
// "<name>.ankh-new", is flushed, optionally verified (`verify` gets the staged path and says whether it is the right content), and
// only then swapped in with one atomic replace. A cut in the middle of the write, a full disk or a wrong hash leaves the old file
// exactly as it was; the staged file is removed on every failure. A swap-in that keeps failing (a locked running program) is queued
// for the next restart (the staged file stays for that, `deferred` = true) unless `allowDeferred` is false: then it fails ("replace") and nothing is queued. In the isolated test build that queue is refused, because
// it writes the host's registry. `why` (optional) says what failed: "write", "verify" or "replace".
inline int& PlaceFileRetries() { static int n = 0; return n; }   // how often PlaceFile had to wait for a locked file (setup logs it)
inline bool PlaceFile(const std::wstring& path, const uint8_t* data, uint64_t size, bool& deferred,
                      const std::function<bool(const std::wstring&)>& verify = nullptr, std::wstring* why = nullptr, bool allowDeferred = true) {
    auto fail = [&](const wchar_t* w) { if (why) *why = w; return false; };
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"place file " + path); return fail(L"write"); }
    EnsureDir(DirOf(path));
    const std::wstring staged = path + L".ankh-new";
    const PlaceFileFaults flt = PlaceFileFaultSet(); const bool fault = flt.Hits(path);
    {   // 1. write the new content next to the target and flush it to disk
        HANDLE f = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return fail(L"write");
        uint64_t done = 0; bool ok = true;
        while (done < size && ok) {
            DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size - done, 1u << 20)); DWORD w = 0;
            std::vector<uint8_t> bad;
            const uint8_t* src = data + done;
            if (fault && flt.cutAfter >= 0) { if (done >= static_cast<uint64_t>(flt.cutAfter)) { ok = false; ++PlaceFileFaultsHit(); break; } chunk = static_cast<DWORD>(std::min<uint64_t>(chunk, flt.cutAfter - done)); }
            if (fault && flt.corrupt && done == 0 && chunk > 0) { bad.assign(src, src + chunk); bad[0] ^= 0x5A; src = bad.data(); ++PlaceFileFaultsHit(); }
            ok = WriteFile(f, src, chunk, &w, nullptr) && w == chunk; done += chunk;
        }
        ok = ok && FlushFileBuffers(f);
        CloseHandle(f);
        if (!ok) { DeleteFileW(staged.c_str()); return fail(L"write"); }
    }
    // 2. the staged file must be the right content before it replaces anything
    if (verify && !verify(staged)) { DeleteFileW(staged.c_str()); return fail(L"verify"); }
    // 3. swap it in. FT-X5: replacing an existing file can fail for a moment while a scanner or indexer holds it (seen once in 20
    // sandbox runs): a few short retries come first; each retry is counted (PlaceFileRetries) so the setup can log it.
    const bool existed = FileExists(path);
    for (int i = 0; i < 8; ++i) {
        if (!(fault && flt.refuseReplace)) {
            if (MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        } else ++PlaceFileFaultsHit();
        if (!existed && !(fault && flt.refuseReplace)) break;   // a new file that cannot be placed is not a lock: no waiting
        ++PlaceFileRetries(); Sleep(200);
    }
    // never queue a restart-time rename from the tests (it writes HKLM); and not for a caller that cannot accept "done at the next restart"
    // (`allowDeferred` false: an optional file that must be either in place now or not at all, so nothing is left to appear after a reboot)
    if (kTestBuild || (fault && flt.refuseReplace) || !allowDeferred) { DeleteFileW(staged.c_str()); return fail(L"replace"); }
    if (MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT)) { deferred = true; return true; }
    DeleteFileW(staged.c_str());
    return fail(L"replace");
}
inline void DeleteNowOrAtReboot(const std::wstring& path) {
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"delete " + path); return; }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}
// Deletes a folder's content (and the folder when removeSelf). Locked files
// are moved aside and deleted at the restart. Returns bytes freed now.
inline uint64_t Evict(const std::wstring& dir, bool removeSelf = true, bool atReboot = true) {
    if (kTestBuild && !InTestArea(dir + L"\\x")) { TestRefused(L"delete folder " + dir); return 0; }
    uint64_t freed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { RemoveDirectoryW(p.c_str()); continue; }
                freed += Evict(p, true, atReboot);
                continue;
            }
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str())) { freed += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; continue; }
            if (!atReboot) continue;
            const std::wstring old = p + L".ankh-old-" + std::to_wstring(GetTickCount());
            if (MoveFileExW(p.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            else MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (removeSelf) RemoveDirectoryW(dir.c_str());
    return freed;
}
inline uint64_t FolderSize(const std::wstring& dir, const wchar_t* pattern = L"*", int depth = 0, uint64_t* accessDenied = nullptr) {
    uint64_t total = 0;
    if (depth > 24) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        if (accessDenied && GetLastError() == ERROR_ACCESS_DENIED) ++(*accessDenied);
        return 0;
    }
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) total += FolderSize(dir + L"\\" + n, L"*", depth + 1, accessDenied);
        } else {
            total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}
inline std::vector<std::wstring> ListDir(const std::wstring& dir, bool dirs, const wchar_t* pattern = L"*") {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (!!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == dirs) out.push_back(n);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------- registry
struct RegValue { bool exists = false; DWORD type = REG_NONE; std::vector<uint8_t> data; };
inline REGSAM View64() { return KEY_WOW64_64KEY; }
inline RegValue RegGet(HKEY root, const std::wstring& key, const std::wstring& name) {
    RegValue v; HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | View64(), &k) != ERROR_SUCCESS) return v;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(k, name.empty() ? nullptr : name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
        v.data.resize(size);
        if (RegQueryValueExW(k, name.empty() ? nullptr : name.c_str(), nullptr, &type, v.data.data(), &size) == ERROR_SUCCESS) {
            v.exists = true; v.type = type; v.data.resize(size);
        }
    }
    RegCloseKey(k);
    return v;
}
inline bool TestRegAllowed(HKEY root, const std::wstring& key) {
    return root == HKEY_CURRENT_USER && _wcsnicmp(key.c_str(), kPrefKey, wcslen(kPrefKey)) == 0;
}
inline bool RegSet(HKEY root, const std::wstring& key, const std::wstring& name, DWORD type, const void* data, DWORD size) {
    if (kTestBuild && !TestRegAllowed(root, key)) { TestRefused(L"registry write " + key + L"\\" + name); return false; }
    HKEY k;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE | View64(), nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const bool ok = RegSetValueExW(k, name.empty() ? nullptr : name.c_str(), 0, type, static_cast<const BYTE*>(data), size) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
inline bool RegSetDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD v) { return RegSet(root, key, name, REG_DWORD, &v, 4); }
inline bool RegSetStr(HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& v, DWORD type = REG_SZ) {
    return RegSet(root, key, name, type, v.c_str(), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}
inline bool RegDel(HKEY root, const std::wstring& key, const std::wstring& name) {
    if (kTestBuild && !TestRegAllowed(root, key)) { TestRefused(L"registry delete " + key + L"\\" + name); return false; }
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE | View64(), &k) != ERROR_SUCCESS) return true;
    const LONG r = RegDeleteValueW(k, name.c_str());
    RegCloseKey(k);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}
inline DWORD RegDword(HKEY root, const std::wstring& key, const std::wstring& name, DWORD def) {
    const RegValue v = RegGet(root, key, name);
    return v.exists && v.type == REG_DWORD && v.data.size() >= 4 ? *reinterpret_cast<const DWORD*>(v.data.data()) : def;
}
inline std::wstring RegStr(HKEY root, const std::wstring& key, const std::wstring& name) {
    const RegValue v = RegGet(root, key, name);
    if (!v.exists || (v.type != REG_SZ && v.type != REG_EXPAND_SZ)) return {};
    std::wstring s(reinterpret_cast<const wchar_t*>(v.data.data()), v.data.size() / sizeof(wchar_t));
    while (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}
inline bool RegKeyExists(HKEY root, const std::wstring& key) {
    HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | View64(), &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}
inline std::vector<std::wstring> RegSubkeys(HKEY root, const std::wstring& key) {
    std::vector<std::wstring> out; HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | View64(), &k) != ERROR_SUCCESS) return out;
    wchar_t n[256];
    for (DWORD i = 0;; ++i) { DWORD l = 256; if (RegEnumKeyExW(k, i, n, &l, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; out.push_back(n); }
    RegCloseKey(k);
    return out;
}
inline std::vector<std::pair<std::wstring, std::wstring>> RegStrings(HKEY root, const std::wstring& key, REGSAM view = KEY_WOW64_64KEY) {
    std::vector<std::pair<std::wstring, std::wstring>> out; HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t n[512]; DWORD nl = 512, type = 0, dl = 0;
        if (RegEnumValueW(k, i, n, &nl, nullptr, &type, nullptr, &dl) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        std::vector<wchar_t> d(dl / 2 + 2, 0); nl = 512; DWORD dl2 = dl;
        if (RegEnumValueW(k, i, n, &nl, nullptr, &type, reinterpret_cast<BYTE*>(d.data()), &dl2) == ERROR_SUCCESS)
            out.emplace_back(n, d.data());
    }
    RegCloseKey(k);
    return out;
}

// ---------------------------------------------------------------- privileges, Windows version
inline bool EnablePrivilege(const wchar_t* name) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{1, {{{0, 0}, SE_PRIVILEGE_ENABLED}}};
    bool ok = LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}
inline bool IsElevated() {
    HANDLE tok; TOKEN_ELEVATION e{}; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(tok);
    return e.TokenIsElevated != 0;
}
struct WinVer { DWORD major = 10, minor = 0, build = 0, ubr = 0; std::wstring display, edition; bool win11() const { return build >= 22000; } };
inline WinVer GetWinVer() {
    WinVer v;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{sizeof(vi)};
    if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&vi);
    v.major = vi.dwMajorVersion; v.minor = vi.dwMinorVersion; v.build = vi.dwBuildNumber;
    const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    v.ubr = RegDword(HKEY_LOCAL_MACHINE, cv, L"UBR", 0);
    v.display = RegStr(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion");
    v.edition = RegStr(HKEY_LOCAL_MACHINE, cv, L"EditionID");
    return v;
}
inline std::wstring WinVerText(const WinVer& v) {
    std::wstring s = v.win11() ? L"Windows 11" : L"Windows 10";
    if (!v.edition.empty()) s += L" " + v.edition;
    if (!v.display.empty()) s += L" " + v.display;
    s += L" (build " + std::to_wstring(v.build) + L"." + std::to_wstring(v.ubr) + L")";
    return s;
}

// ---------------------------------------------------------------- processes
// Run()/RunCapture() results that are not a process exit code.
constexpr DWORD kRunLaunchFailed = static_cast<DWORD>(-1);
constexpr DWORD kRunTimedOut = static_cast<DWORD>(-2);   // still running when the wait ended (never "259 = success")
// Starts a program. timeoutMs == 0: detached launch, returns 0 once started.
// Otherwise waits; returns the exit code, kRunTimedOut, or kRunLaunchFailed.
inline DWORD Run(const std::wstring& exe, const std::wstring& args, bool hidden, DWORD timeoutMs = INFINITE) {
    if (kTestBuild) { TestRefused(L"run " + NameOf(exe) + L" " + args); return kRunLaunchFailed; }
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    if (hidden) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr, nullptr, &si, &pi))
        return kRunLaunchFailed;
    if (timeoutMs == 0) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 0; }
    if (WaitForSingleObject(pi.hProcess, timeoutMs) != WAIT_OBJECT_0) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return kRunTimedOut; }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}
// The only smartctl command lines AnkhCore runs: `--scan ...` or `-a -j [-d type] /dev/x`. Anything that could start a self-test, enable or
// change a SMART feature, set a drive option or read another file is refused. One definition: RunCapture's test-build guard and eng_smart.h use it.
inline bool SmartctlArgsAreReadOnly(const std::wstring& args) {
    std::wstring a = Lower(args);
    if (a.rfind(L"--scan", 0) != 0 && a.rfind(L"-a -j ", 0) != 0) return false;
    a += L" ";
    // tokens: only "-a", "-j", "-d <type>", "--scan", "-j" and one /dev/<name>; nothing else may appear
    size_t at = 0; int devs = 0; bool afterD = false;
    while (at < a.size()) {
        const size_t sp = a.find(L' ', at); if (sp == std::wstring::npos) break;
        const std::wstring tok = a.substr(at, sp - at); at = sp + 1;
        if (tok.empty()) continue;
        if (afterD) { afterD = false; for (wchar_t c : tok) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L',' || c == L'+' || c == L'#')) return false; continue; }
        if (tok == L"--scan" || tok == L"-a" || tok == L"-j" || tok == L"--json") continue;
        if (tok == L"-d") { afterD = true; continue; }
        if (tok.rfind(L"/dev/", 0) == 0 && tok.size() > 5 && tok.size() <= 24) {
            for (size_t i = 5; i < tok.size(); ++i) { const wchar_t c = tok[i]; if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) return false; }
            ++devs; continue;
        }
        return false;
    }
    return !afterD && devs <= 1 && (a.rfind(L"--scan", 0) == 0 ? devs == 0 : devs == 1);
}
// Runs a console tool hidden and streams its output line by line (UTF-16
// output such as sfc's and OEM code page output such as DISM's).
inline DWORD RunCapture(const std::wstring& exe, const std::wstring& args, const std::function<void(const std::wstring&)>& onLine,
                        DWORD timeoutMs = INFINITE) {
    if (kTestBuild) {
        // read-only queries the UI needs to show real state; everything else is refused
        const std::wstring n = Lower(NameOf(exe)), a = Lower(args);
        const bool readOnly = (n == L"powercfg.exe" && a == L"/getactivescheme") ||
                              (n == L"smartctl.exe" && SmartctlArgsAreReadOnly(args)) ||
                              (n == L"schtasks.exe" && a.rfind(L"/query ", 0) == 0 && a.find(L"/create") == std::wstring::npos &&
                               a.find(L"/delete") == std::wstring::npos && a.find(L"/change") == std::wstring::npos && a.find(L"/run") == std::wstring::npos) ||
                              (n == L"powershell.exe" && a.find(L"get-appxpackage") != std::wstring::npos &&
                               a.find(L"remove-") == std::wstring::npos && a.find(L"set-") == std::wstring::npos &&
                               a.find(L"add-") == std::wstring::npos && a.find(L";") == std::wstring::npos);
        if (!readOnly) { TestRefused(L"run " + NameOf(exe) + L" " + args); return static_cast<DWORD>(-1); }
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return static_cast<DWORD>(-1);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); return static_cast<DWORD>(-1);
    }
    CloseHandle(wr);
    std::string raw; std::wstring line;
    int utf16 = -1;
    auto flush = [&](bool all) {
        if (utf16 < 0 && raw.size() >= 4) utf16 = (raw[1] == 0 && raw[3] == 0) ? 1 : 0;
        if (utf16 < 0 && !all) return;
        std::wstring w;
        if (utf16 == 1) { const size_t n = raw.size() / 2; w.assign(reinterpret_cast<const wchar_t*>(raw.data()), n); raw.erase(0, n * 2); }
        else {
            const int n = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            w.resize(n); MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), w.data(), n); raw.clear();
        }
        for (wchar_t c : w) {
            if (c == L'\r' || c == L'\n') {
                size_t a = line.find_first_not_of(L" \t\0", 0, 3);
                if (a != std::wstring::npos) onLine(line.substr(a));
                line.clear();
            } else if (c) line.push_back(c);
        }
        if (all && !line.empty()) { onLine(line); line.clear(); }
    };
    char buf[4096]; DWORD got = 0;
    const DWORD start = GetTickCount();
    bool timedOut = false;
    // Poll the pipe instead of blocking in ReadFile: a child that prints nothing used to block the read for ever and the timeout was never looked at.
    auto drain = [&](bool& open) {   // reads what is in the pipe now; `open` = false once the pipe is closed and empty
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) { open = false; return; }
            if (!avail) return;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) { open = false; return; }
            raw.append(buf, got); flush(false);
        }
    };
    for (bool open = true; open; ) {
        drain(open);
        if (!open) break;
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) { drain(open); break; }   // the child ended: take what it left in the pipe
        if (timeoutMs != INFINITE && GetTickCount() - start > timeoutMs) { TerminateProcess(pi.hProcess, 1); timedOut = true; break; }
    }
    flush(true);
    if (WaitForSingleObject(pi.hProcess, timeoutMs == INFINITE ? INFINITE : 5000) != WAIT_OBJECT_0) timedOut = true;
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    if (timedOut) code = kRunTimedOut;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return code;
}
inline bool ProcessRunning(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)}; bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) found = _wcsicmp(pe.szExeFile, name) == 0;
    CloseHandle(snap);
    return found;
}
inline int KillProcess(const wchar_t* name, DWORD waitMs = 1200) {
    if (kTestBuild) { TestRefused(std::wstring(L"end process ") + name); return 0; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)}; int n = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0)
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) { TerminateProcess(h, 0); CloseHandle(h); ++n; }
    CloseHandle(snap);
    if (n && waitMs) Sleep(waitMs);
    return n;
}

// ---------------------------------------------------------------- the signed-in user
// Setup runs elevated (maybe as another admin); per-user work is done as the
// user who owns explorer.exe in the console session.
inline HANDLE& UserTokenRef() { static HANDLE t = nullptr; return t; }
// Takes ownership of the token handle ShellUserToken() returns: a token set before is closed (it used to be dropped, one handle per call).
inline void SetUserToken(HANDLE t) { HANDLE& r = UserTokenRef(); if (r && r != t) CloseHandle(r); r = t; }
struct UserTokenGuard { ~UserTokenGuard() { SetUserToken(nullptr); } };   // closes the token when the program ends (setup)
inline HANDLE ShellUserToken() {
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    PROCESSENTRY32W pe{sizeof(pe)}; HANDLE result = nullptr;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !result; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"explorer.exe") != 0) continue;
        DWORD s = 0;
        if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s != session) continue;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!proc) continue;
        HANDLE tok = nullptr;
        if (OpenProcessToken(proc, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_ASSIGN_PRIMARY, &tok)) {
            DuplicateTokenEx(tok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &result);
            CloseHandle(tok);
        }
        CloseHandle(proc);
    }
    CloseHandle(snap);
    return result;
}
inline std::wstring UserFolder(REFKNOWNFOLDERID id) {
    const std::wstring r = KnownFolder(id, UserTokenRef());
    return r.empty() ? KnownFolder(id) : r;
}
inline std::wstring UserSid() {
    HANDLE tok = UserTokenRef(); bool own = false;
    if (!tok) { if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return {}; own = true; }
    std::vector<uint8_t> buf(512); DWORD n = 0; std::wstring s;
    if (GetTokenInformation(tok, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
        LPWSTR str = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) { s = str; LocalFree(str); }
    }
    if (own) CloseHandle(tok);
    return s;
}
inline HKEY OpenUserHive() {
    HKEY hive = nullptr;
    if (UserTokenRef() && ImpersonateLoggedOnUser(UserTokenRef())) { RegOpenCurrentUser(KEY_ALL_ACCESS, &hive); RevertToSelf(); }
    if (!hive) RegOpenCurrentUser(KEY_ALL_ACCESS, &hive);
    return hive;
}
// Runs a program as the signed-in user (not elevated) and waits.
inline DWORD RunAsUser(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    if (kTestBuild) { TestRefused(L"run as user " + NameOf(exe) + L" " + args); return static_cast<DWORD>(-1); }
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (UserTokenRef() && CreateProcessWithTokenW(UserTokenRef(), 0, exe.c_str(), cmd.data(), CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return code;
    }
    return Run(exe, args, true, timeoutMs);
}
inline void StartExplorer() {
    if (kTestBuild) { TestRefused(L"start Explorer"); return; }
    Sleep(2500);
    if (!ProcessRunning(L"explorer.exe")) {
        const std::wstring exe = WinDir() + L"\\explorer.exe";
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\"";
        if (UserTokenRef() && CreateProcessWithTokenW(UserTokenRef(), 0, exe.c_str(), cmd.data(), 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        } else {
            ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    for (int i = 0; i < 40 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i) Sleep(500);
}

// ---------------------------------------------------------------- data folder (protected)
// ProgramData lets every user create files; AnkhCore's folder holds backups
// that get copied into System32, so only SYSTEM and Administrators may write.
inline void SecureDataDir() {
    if (kTestBuild) { SHCreateDirectoryExW(nullptr, (DataDir() + L"\\Logs").c_str(), nullptr); return; }   // own per-user folder, no ACL change
    EnsureDir(DataDir());
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)",
                                                             SDDL_REVISION_1, &sd, nullptr)) {
        BOOL present = FALSE, def = FALSE; PACL dacl = nullptr;
        GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
        SetNamedSecurityInfoW(const_cast<wchar_t*>(DataDir().c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        LocalFree(sd);
    }
}

// ---------------------------------------------------------------- activity log
enum class Lv { Info, Ok, Warn, Err };
struct LogLine { std::wstring time; Lv level; std::wstring text; };
struct LogState {
    CRITICAL_SECTION cs; std::vector<LogLine> lines; HWND notify = nullptr; UINT msg = 0; bool fileOk = true;
    uint64_t readOff = 0; bool readInit = false;
    LogState() { InitializeCriticalSection(&cs); }
};
inline LogState& LogS() { static LogState s; return s; }
inline std::wstring LogPath() { return DataDir() + L"\\Logs\\ankhcore.log"; }
// Imports log lines written to ankhcore.log by the companion process (e.g. AnkhCore.exe --engine, B04).
// Caller must hold s.cs. Returns true if at least one new line was added.
inline bool LogSyncSharedLocked(LogState& s) {
    HANDLE f = CreateFileW(LogPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { s.readInit = true; return false; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    const uint64_t endOff = sz.QuadPart > 0 ? static_cast<uint64_t>(sz.QuadPart) : 0;
    if (!s.readInit) {
        s.readInit = true;
        // Look back up to 16 KB on startup so engine lines written just before the UI opened are included.
        s.readOff = endOff > 16384 ? endOff - 16384 : 0;
    }
    if (endOff <= s.readOff) { CloseHandle(f); return false; }
    const uint64_t toRead = std::min<uint64_t>(endOff - s.readOff, 256u << 10);
    LARGE_INTEGER seekPos{}; seekPos.QuadPart = static_cast<LONGLONG>(endOff - toRead);
    SetFilePointerEx(f, seekPos, nullptr, FILE_BEGIN);
    std::string buf(static_cast<size_t>(toRead), '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf.data(), static_cast<DWORD>(toRead), &got, nullptr);
    CloseHandle(f);
    s.readOff = endOff;
    if (!ok || !got) return false;
    buf.resize(got);
    const std::wstring wbuf = Utf8ToW(buf);
    const std::wstring today = NowText(true).substr(0, 10);
    bool added = false;
    for (const auto& raw : Split(wbuf, L'\n')) {
        std::wstring l = raw;
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        // Format: "[YYYY-MM-DD HH:MM:SS] TAG   text"
        if (l.size() < 29 || l[0] != L'[' || l[20] != L']' || l[21] != L' ') continue;
        if (l.compare(1, 10, today) != 0) continue;
        const std::wstring tm = l.substr(12, 8);
        const std::wstring tg = l.substr(22, 5);
        const std::wstring tx = l.substr(28);
        Lv lv = Lv::Info;
        if (tg == L"OK   ") lv = Lv::Ok;
        else if (tg == L"WARN ") lv = Lv::Warn;
        else if (tg == L"ERROR") lv = Lv::Err;
        bool dup = false;
        for (auto it = s.lines.rbegin(); it != s.lines.rend() && std::distance(s.lines.rbegin(), it) < 16; ++it) {
            if (it->time == tm && it->text == tx) { dup = true; break; }
        }
        if (!dup) {
            s.lines.push_back({tm, lv, tx});
            added = true;
        }
    }
    if (s.lines.size() > 4000) s.lines.erase(s.lines.begin(), s.lines.begin() + (s.lines.size() - 3500));
    return added;
}
inline void Log(Lv lv, const std::wstring& text) {
    LogState& s = LogS();
    LogLine l{NowText(), lv, text};
    EnterCriticalSection(&s.cs);
    LogSyncSharedLocked(s);
    s.lines.push_back(l);
    if (s.lines.size() > 4000) s.lines.erase(s.lines.begin(), s.lines.begin() + 500);
    if (s.fileOk) {
        static const wchar_t* tag[] = {L"INFO ", L"OK   ", L"WARN ", L"ERROR"};
        const std::string row = WToUtf8(L"[" + NowText(true) + L"] " + tag[static_cast<int>(lv)] + L" " + text + L"\r\n");
        HANDLE f = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) { EnsureDir(DataDir() + L"\\Logs"); f = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr); }
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0; WriteFile(f, row.data(), static_cast<DWORD>(row.size()), &w, nullptr);
            LARGE_INTEGER sz{}; if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0) s.readOff = static_cast<uint64_t>(sz.QuadPart);
            CloseHandle(f);
        } else s.fileOk = false;
    }
    const HWND n = s.notify; const UINT m = s.msg;
    LeaveCriticalSection(&s.cs);
    if (n) PostMessageW(n, m, 0, 0);
}
// Test build: every refused system change is logged and counted (never silent).
inline std::atomic<int>& TestRefusedCount() { static std::atomic<int> n{0}; return n; }
inline void TestRefused(const std::wstring& what) { ++TestRefusedCount(); Log(Lv::Warn, L"[test build] refused: " + what); }
inline void LogI(const std::wstring& t) { Log(Lv::Info, t); }
inline void LogOk(const std::wstring& t) { Log(Lv::Ok, t); }
inline void LogW(const std::wstring& t) { Log(Lv::Warn, t); }
inline void LogE(const std::wstring& t) { Log(Lv::Err, t); }
inline std::vector<LogLine> LogSnapshot() {
    LogState& s = LogS(); EnterCriticalSection(&s.cs); LogSyncSharedLocked(s); auto v = s.lines; LeaveCriticalSection(&s.cs); return v;
}

// ---------------------------------------------------------------- rollback journal
// Every change AnkhCore makes is recorded with the value it replaced, so it
// can be undone on its own. One UTF-8 line per entry, tab separated:
//   id  time  group  kind  state  title  fields...
// kinds: REG root key name existed type hexdata | SVC name start delayed |
//        POWER guid | HIBER on
// FT-RH: `op` is the identity of the operation an entry belongs to: the id of the operation's first entry, written to the file as a
// trailing "~op=N" field (older entries have none and keep op = 0). The other fields are exactly as before, so every consumer that
// reads them by position (registry undo, services, wallpaper) is unchanged.
struct JEntry { int id = 0; std::wstring time, group, kind, state, title; std::vector<std::wstring> f; int op = 0; };
inline std::wstring& JournalPathOverride() { static std::wstring p; return p; }   // tests only: a journal file of their own
inline std::wstring JournalPath() { return JournalPathOverride().empty() ? DataDir() + L"\\journal.txt" : JournalPathOverride(); }
inline std::vector<JEntry> JournalLoad() {
    std::vector<JEntry> out;
    const std::wstring all = Utf8ToW(ReadText(JournalPath()));
    for (const auto& line : Split(all, L'\n')) {
        std::wstring l = line; if (!l.empty() && l.back() == L'\r') l.pop_back();
        if (l.empty()) continue;
        auto c = Split(l, L'\t');
        if (c.size() < 6) continue;
        JEntry e; e.id = _wtoi(c[0].c_str()); e.time = c[1]; e.group = c[2]; e.kind = c[3]; e.state = c[4]; e.title = c[5];
        e.f.assign(c.begin() + 6, c.end());
        if (!e.f.empty() && e.f.back().compare(0, 4, L"~op=") == 0) { e.op = _wtoi(e.f.back().c_str() + 4); e.f.pop_back(); }
        out.push_back(e);
    }
    return out;
}
inline std::wstring JournalLine(const JEntry& e) {
    std::wstring s = std::to_wstring(e.id) + L"\t" + e.time + L"\t" + e.group + L"\t" + e.kind + L"\t" + e.state + L"\t" + e.title;
    for (const auto& x : e.f) s += L"\t" + x;
    if (e.op) s += L"\t~op=" + std::to_wstring(e.op);
    return s + L"\r\n";
}
// What the journal file looked like after the last write of this process: lets JournalAdd append one line instead of loading and
// rewriting the whole file for every entry. A file that changed behind its back (another process, an undo) is read again.
struct JournalCacheState { bool ok = false; ULONGLONG size = 0; int lastId = 0; };
inline JournalCacheState& JournalCache() { static JournalCacheState s; return s; }
inline ULONGLONG JournalFileSize() { WIN32_FILE_ATTRIBUTE_DATA a{}; return GetFileAttributesExW(JournalPath().c_str(), GetFileExInfoStandard, &a) ? (static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0; }
inline void JournalSave(const std::vector<JEntry>& all) {
    std::wstring s;
    for (const auto& e : all) s += JournalLine(e);
    EnsureDir(DataDir());
    const std::string u = WToUtf8(s);
    if (!WriteAtomic(JournalPath(), reinterpret_cast<const uint8_t*>(u.data()), u.size())) LogE(L"The change journal could not be saved.");
    JournalCache().ok = false;   // the next add reads the file's real state
}
inline CRITICAL_SECTION& JournalLock() { static CRITICAL_SECTION cs; static bool init = (InitializeCriticalSection(&cs), true); (void)init; return cs; }

// One operation = one journal group of entries. While a scope is open on this thread, every entry added belongs to it, and an entry
// that repeats one already recorded in the same operation (same kind, same data: a repeated event, callback or retry) is not recorded
// again; its existing id is returned. Different operations are never merged, whatever their names: identity is the op id and the data.
struct JournalOpState { std::wstring title; int opId = 0; struct Seen { std::wstring kind; std::vector<std::wstring> f; int id; }; std::vector<Seen> seen; };
inline JournalOpState*& JournalOpCurrent() { static thread_local JournalOpState* p = nullptr; return p; }
struct JournalOpScope {
    JournalOpState st; JournalOpState* prev;
    explicit JournalOpScope(const std::wstring& title) : prev(JournalOpCurrent()) { st.title = title; JournalOpCurrent() = &st; }
    ~JournalOpScope() { JournalOpCurrent() = prev; }
    JournalOpScope(const JournalOpScope&) = delete; JournalOpScope& operator=(const JournalOpScope&) = delete;
};
inline int JournalAdd(const std::wstring& group, const std::wstring& kind, const std::wstring& title, const std::vector<std::wstring>& f) {
    EnterCriticalSection(&JournalLock());
    JournalCacheState& js = JournalCache();
    const ULONGLONG size = JournalFileSize();
    if (!js.ok || js.size != size) {   // first use, or the file was changed by something else: read the last id from it
        int last = 0; for (const auto& e : JournalLoad()) last = std::max(last, e.id);
        js.lastId = last; js.size = size; js.ok = true;
    }
    JEntry e; e.id = js.lastId + 1; e.time = NowText(true); e.group = group; e.kind = kind; e.state = L"A"; e.title = title; e.f = f;
    for (auto& x : e.f) for (auto& c : x) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
    JournalOpState* op = JournalOpCurrent();
    if (op) {
        for (const auto& s : op->seen) if (s.kind == kind && s.f == e.f) { LeaveCriticalSection(&JournalLock()); return s.id; }   // already recorded in this operation
        if (!op->opId) op->opId = e.id;
        e.op = op->opId;
    } else e.op = e.id;   // a single entry is its own operation
    if (kTestBuild && !InTestArea(JournalPath())) { TestRefused(L"journal write " + JournalPath()); LeaveCriticalSection(&JournalLock()); return e.id; }
    EnsureDir(DataDir());
    const std::string u = WToUtf8(JournalLine(e));
    bool wrote = false;
    HANDLE h = CreateFileW(JournalPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD n = 0; wrote = WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr) && n == u.size(); CloseHandle(h); }
    if (wrote) { js.lastId = e.id; js.size += u.size(); if (op) op->seen.push_back({kind, e.f, e.id}); }
    else { js.ok = false; LogE(L"The change journal could not be saved."); }
    LeaveCriticalSection(&JournalLock());
    return e.id;
}
inline HKEY RootFromName(const std::wstring& n) { return n == L"HKCU" ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
inline std::wstring RootName(HKEY r) { return r == HKEY_CURRENT_USER ? L"HKCU" : L"HKLM"; }
// Registry write with journal entry (old value kept for undo).
inline bool JRegSet(const std::wstring& group, const std::wstring& title, HKEY root, const std::wstring& key, const std::wstring& name,
                    DWORD type, const void* data, DWORD size) {
    const RegValue old = RegGet(root, key, name);
    JournalAdd(group, L"REG", title, {RootName(root), key, name, old.exists ? L"1" : L"0", std::to_wstring(old.type),
                                      Hex(old.data.data(), old.data.size())});
    return RegSet(root, key, name, type, data, size);
}
inline bool JRegDword(const std::wstring& g, const std::wstring& t, HKEY root, const std::wstring& key, const std::wstring& name, DWORD v) {
    return JRegSet(g, t, root, key, name, REG_DWORD, &v, 4);
}
inline bool JRegStr(const std::wstring& g, const std::wstring& t, HKEY root, const std::wstring& key, const std::wstring& name, const std::wstring& v) {
    return JRegSet(g, t, root, key, name, REG_SZ, v.c_str(), static_cast<DWORD>((v.size() + 1) * 2));
}
inline void JRegDel(const std::wstring& group, const std::wstring& title, HKEY root, const std::wstring& key, const std::wstring& name) {
    const RegValue old = RegGet(root, key, name);
    if (!old.exists) return;
    JournalAdd(group, L"REG", title, {RootName(root), key, name, L"1", std::to_wstring(old.type), Hex(old.data.data(), old.data.size())});
    RegDel(root, key, name);
}
// Undo hooks for kinds handled by engines (services, power...).
inline std::map<std::wstring, std::function<bool(const JEntry&)>>& UndoHandlers() {
    static std::map<std::wstring, std::function<bool(const JEntry&)>> m; return m;
}
inline bool UndoEntry(const JEntry& e) {
    if (e.kind == L"REG" && e.f.size() >= 6) {
        const HKEY root = RootFromName(e.f[0]);
        if (e.f[3] == L"1") { const auto d = Unhex(e.f[5]); return RegSet(root, e.f[1], e.f[2], static_cast<DWORD>(_wtoi(e.f[4].c_str())), d.data(), static_cast<DWORD>(d.size())); }
        return RegDel(root, e.f[1], e.f[2]);
    }
    auto it = UndoHandlers().find(e.kind);
    return it != UndoHandlers().end() && it->second(e);
}
// Undoes entries (newest first); filter picks which. Reports how many were selected and how many were
// really put back (SEC-3): an entry that could not be undone stays active in the journal.
inline UndoCount JournalUndoEx(const std::function<bool(const JEntry&)>& pick) {
    EnterCriticalSection(&JournalLock());
    auto all = JournalLoad();
    UndoCount c;
    std::set<int> undoneIds;   // the entries really put back: the file is re-read before it is rewritten (below)
    std::vector<std::pair<std::wstring, std::pair<int, int>>> perTitle;   // FT-RH: one log line per operation title, not one per journal entry
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        if (it->state != L"A" || !pick(*it)) continue;
        ++c.picked;
        const bool ok = UndoEntry(*it);
        if (ok) { it->state = L"R"; ++c.undone; undoneIds.insert(it->id); }
        auto pt = std::find_if(perTitle.begin(), perTitle.end(), [&](const auto& x) { return x.first == it->title; });
        if (pt == perTitle.end()) { perTitle.push_back({it->title, {0, 0}}); pt = std::prev(perTitle.end()); }
        (ok ? pt->second.first : pt->second.second)++;
    }
    for (const auto& pt : perTitle) {
        if (pt.second.first) LogOk(L"Undone: " + pt.first + (pt.second.first > 1 ? L" (" + std::to_wstring(pt.second.first) + L" changes)" : std::wstring()));
        if (pt.second.second) LogE(L"Could not undo: " + pt.first + (pt.second.second > 1 ? L" (" + std::to_wstring(pt.second.second) + L" changes)" : std::wstring()));
    }
    // Entries added while the undo ran (by an undo handler, or by another writer: the lock is only this process's) are in the file but not in
    // `all`: rewriting `all` would drop them. Re-read the file, mark the undone entries there, and save that. A re-read that came back SHORTER
    // than the first one (the file could not be read for a moment) is not trusted: the first read is saved instead, as before.
    auto fresh = JournalLoad();
    if (fresh.size() >= all.size()) { for (auto& e : fresh) if (e.state == L"A" && undoneIds.count(e.id)) e.state = L"R"; JournalSave(fresh); }
    else JournalSave(all);
    LeaveCriticalSection(&JournalLock());
    return c;
}
// Returns how many entries were undone. Callers that report a result use JournalUndoEx.
inline int JournalUndo(const std::function<bool(const JEntry&)>& pick) { return JournalUndoEx(pick).undone; }

// ---------------------------------------------------------------- Beta 30-day device trial
struct BetaTrialStatus {
    bool active = false;
    bool expired = false;
    bool clockTampered = false;
    int daysRemaining = 0;
    int64_t installEpoch = 0;
    int64_t lastSeenEpoch = 0;
};

inline BetaTrialStatus EvaluateBetaTrial(int64_t nowEpoch,
                                         int64_t installEpoch,
                                         int64_t lastSeenEpoch,
                                         int trialDays = 30) {
    BetaTrialStatus st;
    if (trialDays <= 0) return st;
    st.active = true;
    const int64_t effectiveInstall = (installEpoch > 0) ? installEpoch : nowEpoch;
    st.installEpoch = effectiveInstall;
    if (lastSeenEpoch > 0 && nowEpoch + 86400LL < lastSeenEpoch) {
        st.clockTampered = true;
        st.expired = true;
        st.daysRemaining = 0;
        st.lastSeenEpoch = lastSeenEpoch;
        return st;
    }
    st.lastSeenEpoch = (lastSeenEpoch > nowEpoch) ? lastSeenEpoch : nowEpoch;
    const int64_t elapsed = (nowEpoch > effectiveInstall) ? (nowEpoch - effectiveInstall) : 0;
    const int64_t limit = static_cast<int64_t>(trialDays) * 86400LL;
    st.expired = (elapsed >= limit);
    st.daysRemaining = st.expired ? 0 : static_cast<int>((limit - elapsed + 86399LL) / 86400LL);
    return st;
}

inline int64_t CurrentUnixEpochSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui{};
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(ui.QuadPart / 10000000ULL) - 11644473600LL;
}

inline BetaTrialStatus CheckAndTouchBetaTrial(bool recordInstallIfMissing = true) {
    if (!kIsBetaBuild) return BetaTrialStatus{};
    const int64_t now = CurrentUnixEpochSeconds();
    auto readQword = [](HKEY root, const wchar_t* sub, const wchar_t* name, int64_t& out) -> bool {
        uint64_t v = 0; DWORD sz = sizeof(v), tp = 0;
        if (RegGetValueW(root, sub, name, RRF_RT_REG_QWORD, &tp, &v, &sz) == ERROR_SUCCESS && v > 0) {
            out = static_cast<int64_t>(v);
            return true;
        }
        return false;
    };
    auto writeQword = [](HKEY root, const wchar_t* sub, const wchar_t* name, int64_t val) {
        if (kTestBuild && !TestRegAllowed(root, sub)) return;
        HKEY k = nullptr;
        if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
            const uint64_t v = static_cast<uint64_t>(val);
            RegSetValueExW(k, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
            RegCloseKey(k);
        }
    };

    const std::wstring sbSub = std::wstring(kPrefKey) + L"\\Sandbox\\BetaTrial";
    int64_t installEpoch = 0, lastSeenEpoch = 0;
    auto mergeFrom = [&](HKEY root, const wchar_t* sub) {
        int64_t inst = 0, seen = 0;
        if (readQword(root, sub, L"BetaInstallEpoch", inst)) {
            if (installEpoch == 0 || inst < installEpoch) installEpoch = inst;
        }
        if (readQword(root, sub, L"BetaLastSeenEpoch", seen)) {
            if (seen > lastSeenEpoch) lastSeenEpoch = seen;
        }
    };
    if (kTestBuild) {
        mergeFrom(HKEY_CURRENT_USER, sbSub.c_str());
    } else {
        mergeFrom(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore\\BetaTrial");
        mergeFrom(HKEY_CURRENT_USER, L"Software\\AnkhCore\\BetaTrial");
    }

    const BetaTrialStatus st = EvaluateBetaTrial(now, installEpoch, lastSeenEpoch, kBetaTrialDays);
    if (recordInstallIfMissing && !st.clockTampered) {
        if (kTestBuild) {
            if (installEpoch == 0) writeQword(HKEY_CURRENT_USER, sbSub.c_str(), L"BetaInstallEpoch", st.installEpoch);
            writeQword(HKEY_CURRENT_USER, sbSub.c_str(), L"BetaLastSeenEpoch", st.lastSeenEpoch);
        } else {
            if (installEpoch == 0) {
                writeQword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore\\BetaTrial", L"BetaInstallEpoch", st.installEpoch);
                writeQword(HKEY_CURRENT_USER, L"Software\\AnkhCore\\BetaTrial", L"BetaInstallEpoch", st.installEpoch);
            }
            writeQword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AnkhCore\\BetaTrial", L"BetaLastSeenEpoch", st.lastSeenEpoch);
            writeQword(HKEY_CURRENT_USER, L"Software\\AnkhCore\\BetaTrial", L"BetaLastSeenEpoch", st.lastSeenEpoch);
        }
    }
    return st;
}

inline std::wstring FormatBetaVersionLabel(int daysRemaining, bool shortForm = false) {
    const int d = (daysRemaining < 0) ? 0 : daysRemaining;
    if (shortForm) return L"2.11 Beta \x00B7 " + std::to_wstring(d) + L"d left";
    return L"2.11 Beta (" + std::to_wstring(d) + L" / 30 days left)";
}

inline std::wstring CurrentBetaVersionLabel(bool shortForm = false) {
    if (!kIsBetaBuild) return kVersionLabel;
    const BetaTrialStatus st = CheckAndTouchBetaTrial(false);
    return FormatBetaVersionLabel(st.daysRemaining, shortForm);
}

}  // namespace ak
