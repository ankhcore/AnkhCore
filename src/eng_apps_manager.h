// eng_apps_manager.h - 2.7 K2/K4: the Apps Manager's "select, uninstall, clean what is left" workflow.
//
//   select programs  ->  UninstallBatchRunner (K3: one at a time, each through its OWN uninstaller, verified gone)
//   verified removals ->  RemScan (K1: exact evidence only; Safe / Needs confirmation / Protected)
//   the person picks what to clean ->  RemExecute (K2, this file)
//
// K2 rules: Protected is never removed, "needs confirmation" only when the person confirmed that exact item; every item is checked again
// right before it is touched; nothing is deleted: a folder or shortcut is MOVED into a recovery folder (same volume, instant) and a
// registry key is exported to a .reg file first. The recovery folder carries a manifest, so "Undo last cleanup" puts everything back.
// Everything with side effects is injected (CleanBackend); the file/registry operations are plain functions tested in a temp folder.
#pragma once
#include "base.h"
#include "eng_apps.h"
#include "eng_remnants.h"
#include "eng_uninstall_batch.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <tlhelp32.h>

namespace ak {

// ---------------------------------------------------------------- inventory -> uninstall items
// The base name of the executable an entry names in DisplayIcon ("C:\...\app.exe,0" -> "app"); generic uninstaller names are not evidence.
inline std::wstring AppIconExeName(const std::wstring& iconIn) {
    std::wstring t = iconIn;
    while (!t.empty() && (t.front() == L'"' || iswspace(t.front()))) t.erase(t.begin());
    const size_t q = t.find(L'"'); if (q != std::wstring::npos) t = t.substr(0, q);
    const size_t comma = t.rfind(L','); if (comma != std::wstring::npos && comma + 1 < t.size() && (iswdigit(t[comma + 1]) || t[comma + 1] == L'-')) t = t.substr(0, comma);
    const size_t slash = t.find_last_of(L"\\/"); if (slash != std::wstring::npos) t = t.substr(slash + 1);
    const std::wstring low = Lower(t);
    if (low.size() < 5 || low.compare(low.size() - 4, 4, L".exe") != 0) return L"";
    t = t.substr(0, t.size() - 4);
    const std::wstring b = Lower(t);
    if (b.rfind(L"unins", 0) == 0 || b.find(L"uninstall") != std::wstring::npos || b == L"setup" || b == L"install" || b == L"msiexec" || b == L"update" || b == L"launcher") return L"";
    return t;
}
inline UninstallItem AppToUninstallItem(const AppEntry& e) {
    UninstallItem it;
    it.id = (e.store ? L"store:" : L"win:") + e.key;
    it.name = e.name; it.publisher = e.publisher; it.store = e.store;
    it.command = e.store ? e.key : e.uninstall;
    it.installLocation = e.installLocation;
    it.uninstallKey = e.store ? L"" : e.key;
    it.userHive = e.userHive;
    const std::wstring ex = AppIconExeName(e.icon);
    if (!ex.empty()) it.exeNames.push_back(ex);
    return it;
}

// ---------------------------------------------------------------- the live environment of a scan (read-only)
inline RemEnv RemLiveEnv(const std::vector<AppEntry>& inventory) {
    RemEnv e;
    e.windows = KnownFolder(FOLDERID_Windows); e.programFiles = KnownFolder(FOLDERID_ProgramFiles); e.programFilesX86 = KnownFolder(FOLDERID_ProgramFilesX86);
    e.programData = KnownFolder(FOLDERID_ProgramData); e.appData = KnownFolder(FOLDERID_RoamingAppData); e.localAppData = KnownFolder(FOLDERID_LocalAppData);
    e.userProfile = KnownFolder(FOLDERID_Profile);
    wchar_t tmp[MAX_PATH]; const DWORD n = GetTempPathW(MAX_PATH, tmp); e.temp = n ? std::wstring(tmp, n) : L"";
    while (e.temp.size() > 3 && e.temp.back() == L'\\') e.temp.pop_back();
    e.startMenuCommon = KnownFolder(FOLDERID_CommonPrograms); e.startMenuUser = KnownFolder(FOLDERID_Programs);
    for (const KNOWNFOLDERID* id : {&FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos}) { const std::wstring p = KnownFolder(*id); if (!p.empty()) e.personalFolders.push_back(p); }
    for (const wchar_t* ev : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) { wchar_t b[MAX_PATH]; if (GetEnvironmentVariableW(ev, b, MAX_PATH)) e.personalFolders.push_back(b); }
    for (const AppEntry& a : inventory) if (!a.installLocation.empty()) e.otherInstallLocations.push_back(a.installLocation);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t path[MAX_PATH * 2]; DWORD len = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(h, 0, path, &len)) e.runningImages.push_back(std::wstring(path, len));
            CloseHandle(h);
        }
        CloseHandle(snap);
    }
    return e;
}
// The scan hook for the batch runner: after a VERIFIED removal only (the runner guarantees it), with the entry's own data.
inline std::function<std::vector<RemCandidate>(const UninstallItem&)> AppsScanHook(const std::vector<AppEntry>& inventory) {
    const std::shared_ptr<const RemEnv> env = std::make_shared<const RemEnv>(RemLiveEnv(inventory));
    return [env](const UninstallItem& it) {
        RemApp a = UninstallToRemApp(it, true);
        if (!it.uninstallKey.empty()) a.uninstallKey = std::wstring(it.userHive ? L"HKCU\\" : L"HKLM\\") + it.uninstallKey;
        return RemScan(a, *env, RemLiveFs(), RemLiveReg());
    };
}

// ---------------------------------------------------------------- K2: executing a cleanup
enum class CleanState { Removed, Refused, Failed, Missing };
inline const wchar_t* CleanStateLabel(CleanState s) { return s == CleanState::Removed ? L"Moved to recovery" : s == CleanState::Refused ? L"Not touched" : s == CleanState::Failed ? L"Failed" : L"Already gone"; }
struct CleanItemResult { RemCandidate c; CleanState state = CleanState::Refused; std::wstring detail; };
struct CleanReport {
    std::vector<CleanItemResult> items; int removed = 0, refused = 0, failed = 0, missing = 0; std::wstring summary, recoveryDir;
};
struct CleanBackend {
    std::function<bool(const RemCandidate&)> exists;                                            // checked again right before the removal
    std::function<bool(const RemCandidate&, const std::wstring& recoveryDir, std::wstring& detail)> remove;   // moves/exports, never deletes outright
};
// `confirmed`: the exact paths the person confirmed (needed for "Needs your confirmation" items). Items run one at a time; one failure does not stop the others.
inline CleanReport RemExecute(const std::vector<RemCandidate>& cands, const std::set<std::wstring>& confirmed, const CleanBackend& be, const std::wstring& recoveryDir) {
    CleanReport rep; rep.recoveryDir = recoveryDir;
    std::set<std::wstring> seen;
    for (const RemCandidate& c : cands) {
        CleanItemResult r; r.c = c;
        const std::wstring key = (c.kind == RemKind::RegistryKey ? L"r:" : L"f:") + remdetail::Norm(c.path);
        const RemDecision d = RemMayRemove(c, confirmed.count(c.path) > 0);
        if (!seen.insert(key).second) { r.state = CleanState::Refused; r.detail = L"Listed twice."; }
        else if (!d.allowed) { r.state = CleanState::Refused; r.detail = d.reason; }
        else if (c.kind != RemKind::RegistryKey && !remdetail::ValidAbs(remdetail::Norm(c.path))) { r.state = CleanState::Refused; r.detail = L"Not a full, plain path."; }
        else if (!be.exists || !be.remove) { r.state = CleanState::Failed; r.detail = L"There is no way to remove it."; }
        else if (!be.exists(c)) { r.state = CleanState::Missing; r.detail = L"It is already gone."; }
        else { std::wstring why; if (be.remove(c, recoveryDir, why)) { r.state = CleanState::Removed; r.detail = why; } else { r.state = CleanState::Failed; r.detail = why; } }
        rep.items.push_back(std::move(r));
    }
    for (const auto& r : rep.items) { switch (r.state) { case CleanState::Removed: ++rep.removed; break; case CleanState::Refused: ++rep.refused; break; case CleanState::Failed: ++rep.failed; break; default: ++rep.missing; } }
    rep.summary = std::to_wstring(rep.removed) + L" moved to the recovery folder, " + std::to_wstring(rep.failed) + L" failed, " + std::to_wstring(rep.refused) + L" not touched, " + std::to_wstring(rep.missing) + L" already gone.";
    return rep;
}

// ---------------------------------------------------------------- the plain file / registry operations (testable in a temp folder)
namespace cleandetail {
inline std::wstring Drive(const std::wstring& p) { return p.size() >= 2 && p[1] == L':' ? Lower(p.substr(0, 2)) : std::wstring(); }
inline std::wstring ManifestPath(const std::wstring& recoveryDir) { return recoveryDir + L"\\cleanup-manifest.txt"; }
inline void AppendManifest(const std::wstring& recoveryDir, const std::wstring& line) {
    std::string old = ReadText(ManifestPath(recoveryDir));
    const std::string add = WToUtf8(line) + "\n"; old += add;
    WriteAll(ManifestPath(recoveryDir), reinterpret_cast<const uint8_t*>(old.data()), old.size());
}
inline std::wstring Unique(const std::wstring& dir, const std::wstring& leaf) {
    std::wstring t = dir + L"\\" + leaf;
    for (int i = 2; GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i) t = dir + L"\\" + std::to_wstring(i) + L"_" + leaf;
    return t;
}
}  // namespace cleandetail

// Moves a file or folder into `recoveryDir` (same volume: a rename, nothing is copied or deleted). Refuses a reparse point and anything on another volume.
inline bool RemMoveToRecovery(const std::wstring& path, const std::wstring& recoveryDir, std::wstring& detail) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { detail = L"It no longer exists."; return false; }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) { detail = L"It is a link (junction/symlink), which is never followed or removed."; return false; }
    if (cleandetail::Drive(path) != cleandetail::Drive(recoveryDir)) { detail = L"The recovery folder is on another drive."; return false; }
    EnsureDir(recoveryDir);
    const size_t k = path.find_last_of(L'\\');
    const std::wstring to = cleandetail::Unique(recoveryDir, k == std::wstring::npos ? path : path.substr(k + 1));
    if (!MoveFileExW(path.c_str(), to.c_str(), 0)) { detail = L"Windows could not move it (error " + std::to_wstring(GetLastError()) + L"); it may be in use. Nothing was changed."; return false; }
    cleandetail::AppendManifest(recoveryDir, L"F\t" + path + L"\t" + to);
    detail = L"Moved to " + to;
    return true;
}
// Exports the key to a .reg file in the recovery folder, then deletes it. The export must succeed first.
inline bool RemExportAndDeleteKey(const std::wstring& key, const std::wstring& recoveryDir, std::wstring& detail) {
    HKEY root; std::wstring sub;
    if (!remdetail::SplitKey(key, root, sub) || sub.empty()) { detail = L"Not a registry key that can be handled."; return false; }
    EnsureDir(recoveryDir);
    const std::wstring file = cleandetail::Unique(recoveryDir, L"registry.reg");
    if (Run(SysDir() + L"\\reg.exe", L"export \"" + key + L"\" \"" + file + L"\" /y", true, 30000) != 0 || GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) { detail = L"The key could not be saved first, so it was not touched."; return false; }
    const LSTATUS rc = RegDeleteTreeW(root, sub.c_str());
    if (rc != ERROR_SUCCESS) { detail = L"Windows refused to delete the key (error " + std::to_wstring(rc) + L"). It was saved to " + file + L" and left in place."; return false; }
    cleandetail::AppendManifest(recoveryDir, L"R\t" + key + L"\t" + file);
    detail = L"Saved to " + file + L" and removed.";
    return true;
}
inline CleanBackend RemLiveBackend() {
    CleanBackend be;
    be.exists = [](const RemCandidate& c) { return c.kind == RemKind::RegistryKey ? RemLiveReg().keyExists(c.path) : GetFileAttributesW(c.path.c_str()) != INVALID_FILE_ATTRIBUTES; };
    be.remove = [](const RemCandidate& c, const std::wstring& dir, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"cleanup of " + c.path); why = L"the test build removes nothing"; return false; }
        return c.kind == RemKind::RegistryKey ? RemExportAndDeleteKey(c.path, dir, why) : RemMoveToRecovery(c.path, dir, why);
    };
    return be;
}
// A recovery folder next to the data folder when it is on the same drive as the item, else on the item's own drive (a move cannot cross drives).
inline std::wstring CleanRecoveryDirFor(const std::wstring& anyPath) {
    SYSTEMTIME t; GetLocalTime(&t); wchar_t ts[40]; swprintf(ts, 40, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (cleandetail::Drive(anyPath).empty() || cleandetail::Drive(anyPath) == cleandetail::Drive(DataDir())) return DataDir() + L"\\Removed\\" + ts;
    return anyPath.substr(0, 2) + L"\\AnkhCore-Removed\\" + ts;
}
// Undo: moves everything back (only where the original place is still free) and re-imports exported keys. Returns the number restored; `problems` lists what could not be.
inline int RemUndo(const std::wstring& recoveryDir, std::vector<std::wstring>& problems) {
    int restored = 0;
    const std::string text = ReadText(cleandetail::ManifestPath(recoveryDir));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
        const std::wstring line = Utf8ToW(text.substr(pos, e - pos)); pos = e + 1;
        const auto f = Split(line, L'\t'); if (f.size() != 3) continue;
        if (f[0] == L"F") {
            if (GetFileAttributesW(f[1].c_str()) != INVALID_FILE_ATTRIBUTES) { problems.push_back(f[1] + L": something is already there."); continue; }
            const size_t k = f[1].find_last_of(L'\\'); if (k != std::wstring::npos) EnsureDir(f[1].substr(0, k));
            if (MoveFileExW(f[2].c_str(), f[1].c_str(), 0)) ++restored; else problems.push_back(f[1] + L": could not be moved back (error " + std::to_wstring(GetLastError()) + L").");
        } else if (f[0] == L"R") {
            if (Run(SysDir() + L"\\reg.exe", L"import \"" + f[2] + L"\"", true, 30000) == 0) ++restored; else problems.push_back(f[1] + L": the saved registry file could not be imported.");
        }
    }
    return restored;
}

// ---------------------------------------------------------------- the model the page uses
struct AppsMgrRemRow { RemCandidate c; std::wstring app; bool selected = false; };
struct AppsMgrView {
    int phase = 0;                        // 0 idle, 1 uninstalling, 2 uninstall finished, 3 cleaning, 4 cleanup finished
    size_t total = 0, done = 0;           // uninstall progress
    std::wstring current;                 // the application being uninstalled
    UninstallBatchReport report;          // valid when phase >= 2
    std::vector<AppsMgrRemRow> remnants;  // what the scan found after verified removals
    CleanReport clean;                    // valid when phase == 4
    std::wstring lastRecoveryDir;         // the last cleanup that can be undone
    std::wstring undoText;
};
class AppsMgr {
public:
    bool Selected(const std::wstring& id) const { std::lock_guard<std::mutex> lk(mx_); return sel_.count(id) > 0; }
    void Toggle(const std::wstring& id) { std::lock_guard<std::mutex> lk(mx_); if (!sel_.erase(id)) sel_.insert(id); }
    void ClearSelection() { std::lock_guard<std::mutex> lk(mx_); sel_.clear(); }
    size_t SelectedCount() const { std::lock_guard<std::mutex> lk(mx_); return sel_.size(); }
    std::set<std::wstring> Selection() const { std::lock_guard<std::mutex> lk(mx_); return sel_; }
    AppsMgrView Snapshot() const { std::lock_guard<std::mutex> lk(mx_); return v_; }
    bool Busy() const { std::lock_guard<std::mutex> lk(mx_); return v_.phase == 1 || v_.phase == 3; }
    void ToggleRemnant(size_t i) { std::lock_guard<std::mutex> lk(mx_); if (i < v_.remnants.size() && v_.remnants[i].c.cls != RemClass::Protected) v_.remnants[i].selected = !v_.remnants[i].selected; }
    void CancelBatch() { runner_.Cancel(); }
    void Dismiss() { std::lock_guard<std::mutex> lk(mx_); if (v_.phase == 2 || v_.phase == 4) { const std::wstring keep = v_.lastRecoveryDir; v_ = AppsMgrView{}; v_.lastRecoveryDir = keep; } }

    // Starts the batch on a worker thread. Returns false when one is already running or nothing is selected.
    bool StartUninstall(std::vector<UninstallItem> items, UninstallBackend be, std::function<void()> notify) {
        { std::lock_guard<std::mutex> lk(mx_); if (v_.phase == 1 || v_.phase == 3 || items.empty()) return false; const std::wstring keep = v_.lastRecoveryDir; v_ = AppsMgrView{}; v_.lastRecoveryDir = keep; v_.phase = 1; v_.total = items.size(); }
        std::thread([this, items = std::move(items), be = std::move(be), notify = std::move(notify)] {
            const UninstallBatchReport rep = runner_.Run(items, be, [this, &notify](const UninstallEvent& ev) {
                { std::lock_guard<std::mutex> lk(mx_); v_.current = ev.state == UnState::Running ? ev.name : std::wstring(); if (ev.state != UnState::Running && ev.state != UnState::Pending) v_.done = ev.index + 1; }
                if (notify) notify(); });
            LogI(L"Apps Manager uninstall finished: " + rep.summary);
            for (const auto& r : rep.results) LogI(L"  " + r.item.name + L": " + UnStateLabel(r.state) + L" - " + r.detail);
            {
                std::lock_guard<std::mutex> lk(mx_);
                v_.report = rep; v_.phase = 2; v_.current.clear();
                for (const auto& r : rep.results) for (const RemCandidate& c : r.remnants) { AppsMgrRemRow row; row.c = c; row.app = r.item.name; row.selected = c.cls == RemClass::Safe; v_.remnants.push_back(std::move(row)); }
                for (const auto& r : rep.results) if (r.state == UnState::Removed || r.state == UnState::RemovedRestartRequired) sel_.erase(r.item.id);
            }
            if (notify) notify();
        }).detach();
        return true;
    }
    // Cleans the selected remnants (Needs-confirmation items only if `confirmNeeded` says the person confirmed them).
    bool StartClean(CleanBackend be, bool confirmNeeded, std::function<void()> notify) {
        std::vector<RemCandidate> chosen; std::set<std::wstring> confirmed;
        { std::lock_guard<std::mutex> lk(mx_); if (v_.phase != 2 && v_.phase != 4) return false;
          for (const auto& r : v_.remnants) if (r.selected) { chosen.push_back(r.c); if (confirmNeeded && r.c.cls == RemClass::Confirm) confirmed.insert(r.c.path); }
          if (chosen.empty()) return false; v_.phase = 3; }
        std::thread([this, chosen = std::move(chosen), confirmed = std::move(confirmed), be = std::move(be), notify = std::move(notify)] {
            const std::wstring dir = CleanRecoveryDirFor(chosen.front().kind == RemKind::RegistryKey ? DataDir() : chosen.front().path);
            // items on another drive get their own recovery folder: one RemExecute per drive group
            CleanReport all; all.recoveryDir = dir;
            std::map<std::wstring, std::vector<RemCandidate>> byDir;
            for (const auto& c : chosen) byDir[c.kind == RemKind::RegistryKey ? dir : CleanRecoveryDirFor(c.path)].push_back(c);
            for (auto& kv : byDir) { CleanReport r = RemExecute(kv.second, confirmed, be, kv.first); for (auto& it : r.items) all.items.push_back(std::move(it)); }
            for (const auto& r : all.items) { switch (r.state) { case CleanState::Removed: ++all.removed; break; case CleanState::Refused: ++all.refused; break; case CleanState::Failed: ++all.failed; break; default: ++all.missing; } }
            all.summary = std::to_wstring(all.removed) + L" moved to the recovery folder, " + std::to_wstring(all.failed) + L" failed, " + std::to_wstring(all.refused) + L" not touched, " + std::to_wstring(all.missing) + L" already gone.";
            LogI(L"Apps Manager cleanup finished: " + all.summary + L"  recovery folder: " + dir);
            for (const auto& r : all.items) LogI(L"  " + r.c.path + L": " + CleanStateLabel(r.state) + L" - " + r.detail);
            { std::lock_guard<std::mutex> lk(mx_);
              v_.clean = all; v_.phase = 4;
              if (all.removed > 0) v_.lastRecoveryDir = byDir.size() == 1 ? dir : byDir.begin()->first;
              std::vector<AppsMgrRemRow> left;
              for (const auto& row : v_.remnants) { bool gone = false; for (const auto& r : all.items) if (r.c.path == row.c.path && (r.state == CleanState::Removed || r.state == CleanState::Missing)) gone = true; if (!gone) left.push_back(row); }
              v_.remnants = std::move(left); }
            if (notify) notify();
        }).detach();
        return true;
    }
    std::wstring UndoLast() {
        std::wstring dir; { std::lock_guard<std::mutex> lk(mx_); dir = v_.lastRecoveryDir; }
        if (dir.empty()) return L"There is no cleanup to undo.";
        std::vector<std::wstring> problems; const int n = RemUndo(dir, problems);
        std::wstring t = std::to_wstring(n) + L" item(s) put back.";
        for (const auto& p : problems) t += L"  " + p;
        { std::lock_guard<std::mutex> lk(mx_); v_.undoText = t; if (problems.empty()) v_.lastRecoveryDir.clear(); }
        return t;
    }
private:
    mutable std::mutex mx_;
    std::set<std::wstring> sel_;
    AppsMgrView v_;
    UninstallBatchRunner runner_;
};

}  // namespace ak
