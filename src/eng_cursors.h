// AnkhCore 2.5 — Cursor Manager engine (APP-4): discovery of cursor packs in folders and of the
// schemes registered in Windows, validation of the files, apply with a saved previous scheme and
// rollback, restore previous, restore Windows defaults. Pure logic is in cursor_scheme.h.
//
// Mechanism (the one Mouse Properties uses): the 17 values under HKCU\Control Panel\Cursors, the
// key's default value (scheme name), "Scheme Source", a list under ...\Schemes, then
// SystemParametersInfo(SPI_SETCURSORS). Cursor files are never copied, moved, renamed or deleted:
// a scheme is applied from where the user keeps it.
//
// Test build: every read and write goes to a sandbox copy of that key under the test build's own
// registry key (seeded once, read-only, from the real values), and SPI_SETCURSORS is not called.
// The real cursor settings of this PC cannot be changed by a test build.
#pragma once
#include <optional>
#include "base.h"
#include "settings.h"
#include "cursor_scheme.h"

namespace ak {

inline std::wstring CurRealKey() { return L"Control Panel\\Cursors"; }
inline std::wstring CurKey() { return kTestBuild ? std::wstring(kPrefKey) + L"\\CursorSandbox\\Control Panel\\Cursors" : CurRealKey(); }
inline std::wstring CurSchemesKey() { return CurKey() + L"\\Schemes"; }
inline std::wstring CurDataDir() { return DataDir() + L"\\Cursors"; }
inline std::wstring CurPrevPath() { return CurDataDir() + L"\\previous-scheme.txt"; }
inline const wchar_t* CurJournalGroup() { return L"cursors"; }   // the 2.2 group: Recovery's Undo covers these entries too

inline std::vector<std::wstring> CurRegValueNames(HKEY root, const std::wstring& key) {
    std::vector<std::wstring> out; HKEY k;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_QUERY_VALUE | View64(), &k) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512]; DWORD n = 512;
        if (RegEnumValueW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (n) out.push_back(std::wstring(name, n));
    }
    RegCloseKey(k);
    std::sort(out.begin(), out.end());
    return out;
}

inline CurState CurReadStateFrom(const std::wstring& key) {
    CurState s;
    s.name = RegStr(HKEY_CURRENT_USER, key, L"");
    const RegValue src = RegGet(HKEY_CURRENT_USER, key, L"Scheme Source");
    s.hasSource = src.exists && src.type == REG_DWORD && src.data.size() >= 4;
    if (s.hasSource) s.source = *reinterpret_cast<const DWORD*>(src.data.data());
    for (int i = 0; i < kCurRoles; ++i) {
        const RegValue v = RegGet(HKEY_CURRENT_USER, key, CurRoleTable()[i].key);
        s.exists[i] = v.exists && (v.type == REG_SZ || v.type == REG_EXPAND_SZ);
        if (s.exists[i]) s.value[i] = RegStr(HKEY_CURRENT_USER, key, CurRoleTable()[i].key);
    }
    return s;
}
// Test build only: the sandbox starts as a copy of the real settings, so the page shows this PC's scheme.
inline void CurSeedSandbox() {
    if (!kTestBuild) return;
    if (RegGet(HKEY_CURRENT_USER, CurKey(), L"AnkhSeeded").exists) return;
    const CurState real = CurReadStateFrom(CurRealKey());
    RegSetStr(HKEY_CURRENT_USER, CurKey(), L"", real.name);
    if (real.hasSource) RegSetDword(HKEY_CURRENT_USER, CurKey(), L"Scheme Source", real.source);
    for (int i = 0; i < kCurRoles; ++i) if (real.exists[i]) RegSetStr(HKEY_CURRENT_USER, CurKey(), CurRoleTable()[i].key, real.value[i], REG_EXPAND_SZ);
    for (const auto& n : CurRegValueNames(HKEY_CURRENT_USER, CurRealKey() + L"\\Schemes"))
        RegSetStr(HKEY_CURRENT_USER, CurSchemesKey(), n, RegStr(HKEY_CURRENT_USER, CurRealKey() + L"\\Schemes", n), REG_EXPAND_SZ);
    RegSetDword(HKEY_CURRENT_USER, CurKey(), L"AnkhSeeded", 1);
}
inline CurState CurReadState() { CurSeedSandbox(); return CurReadStateFrom(CurKey()); }

inline bool CurFileHeaderOk(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint8_t b[32] = {}; DWORD got = 0;
    const bool rd = ReadFile(f, b, sizeof(b), &got, nullptr) != 0;
    CloseHandle(f);
    const size_t dot = path.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : Lower(path.substr(dot));
    if (ext != L".ani" && ext != L".cur") return false;
    return rd && CurValidHeader(b, got, ext == L".ani");
}
// Full path of a role's file ("" when the scheme has none).
inline std::wstring CurRolePath(const CurScheme& sc, int role) {
    if (sc.file[role].empty()) return {};
    return sc.registered || sc.dir.empty() ? Expand(sc.file[role]) : sc.dir + L"\\" + sc.file[role];
}
// Fills sc.invalid: mapped files that are missing or are not cursor files.
inline void CurCheckFiles(CurScheme& sc) {
    sc.invalid.clear();
    for (int i = 0; i < kCurRoles; ++i) if (!sc.file[i].empty() && !CurFileHeaderOk(CurRolePath(sc, i))) sc.invalid.push_back(sc.file[i]);
}

inline void CurCollect(const std::wstring& dir, const std::wstring& rel, int depth, std::vector<std::wstring>& out) {
    for (const auto& f : ListDir(dir, false)) {
        const size_t dot = f.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L"" : Lower(f.substr(dot));
        if (ext == L".ani" || ext == L".cur") out.push_back(rel + f);
    }
    if (depth > 0) for (const auto& d : ListDir(dir, true)) CurCollect(dir + L"\\" + d, rel + d + L"\\", depth - 1, out);
}
// Cursor packs in a folder: each sub-folder that holds cursor files is a scheme. A folder whose own
// files already fill roles is itself one scheme (the user pointed at a single pack).
inline std::vector<CurScheme> CurDiscoverFolder(const std::wstring& root) {
    std::vector<CurScheme> out;
    if (!DirExists(root)) return out;
    {
        std::vector<std::wstring> top; CurCollect(root, L"", 0, top);
        if (CurMapScheme(NameOf(root), root, top).Mapped() > 0) {
            std::vector<std::wstring> all; CurCollect(root, L"", 2, all);
            CurScheme s = CurMapScheme(NameOf(root), root, all); CurCheckFiles(s); out.push_back(std::move(s));
            return out;
        }
    }
    for (const auto& d : ListDir(root, true)) {
        std::vector<std::wstring> files; CurCollect(root + L"\\" + d, L"", 2, files);
        if (files.empty()) continue;
        CurScheme s = CurMapScheme(d, root + L"\\" + d, files); CurCheckFiles(s);
        out.push_back(std::move(s));
    }
    return out;
}
// The user's schemes registered in Windows (Mouse Properties > Pointers > Scheme).
inline std::vector<CurScheme> CurRegisteredSchemes() {
    CurSeedSandbox();
    std::vector<CurScheme> out;
    for (const auto& n : CurRegValueNames(HKEY_CURRENT_USER, CurSchemesKey())) {
        CurScheme s; s.name = n; s.registered = true;
        s.file = CurParseSchemeList(RegStr(HKEY_CURRENT_USER, CurSchemesKey(), n));
        for (const auto& f : s.file) if (!f.empty()) s.premium |= CurParseName(NameOf(f)).premium;
        CurCheckFiles(s);
        if (s.Mapped() > 0) out.push_back(std::move(s));
    }
    return out;
}
// Folder packs from every root (each folder once), then the registered schemes.
inline std::vector<CurScheme> CurDiscover(const std::vector<std::wstring>& roots) {
    std::vector<CurScheme> out; std::vector<std::wstring> seen;
    for (const auto& r : roots) {
        if (r.empty()) continue;
        for (auto& s : CurDiscoverFolder(r)) {
            const std::wstring k = Lower(s.dir);
            if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
            seen.push_back(k); out.push_back(std::move(s));
        }
    }
    for (auto& s : CurRegisteredSchemes()) out.push_back(std::move(s));
    return CurFilterDuplicateNamedSchemes(out);
}

inline int& CurTestFailAfter() { static int n = -1; return n; }   // test build only: fail the Nth value write, to prove the rollback

// Writes a whole state. journal: each change gets an undo entry. Verified by reading back.
inline bool CurWriteState(const CurState& t, const std::wstring& title, bool journal) {
    const std::wstring key = CurKey();
    std::optional<JournalOpScope> jscope; if (journal) jscope.emplace(title);   // FT-RH: the 17 roles and the scheme name are ONE operation in the journal
    int written = 0;
    auto fail = [&] { return kTestBuild && CurTestFailAfter() >= 0 && written++ == CurTestFailAfter(); };
    for (int i = 0; i < kCurRoles; ++i) {
        const wchar_t* name = CurRoleTable()[i].key;
        if (fail()) return false;
        if (t.exists[i]) {
            const DWORD size = static_cast<DWORD>((t.value[i].size() + 1) * sizeof(wchar_t));
            const bool ok = journal ? JRegSet(CurJournalGroup(), title, HKEY_CURRENT_USER, key, name, REG_EXPAND_SZ, t.value[i].c_str(), size)
                                    : RegSet(HKEY_CURRENT_USER, key, name, REG_EXPAND_SZ, t.value[i].c_str(), size);
            if (!ok) return false;
        } else if (journal) JRegDel(CurJournalGroup(), title, HKEY_CURRENT_USER, key, name);
        else if (!RegDel(HKEY_CURRENT_USER, key, name)) return false;
    }
    if (!(journal ? JRegStr(CurJournalGroup(), title, HKEY_CURRENT_USER, key, L"", t.name) : RegSetStr(HKEY_CURRENT_USER, key, L"", t.name))) return false;
    if (t.hasSource) {
        if (!(journal ? JRegDword(CurJournalGroup(), title, HKEY_CURRENT_USER, key, L"Scheme Source", t.source) : RegSetDword(HKEY_CURRENT_USER, key, L"Scheme Source", t.source))) return false;
    } else if (journal) JRegDel(CurJournalGroup(), title, HKEY_CURRENT_USER, key, L"Scheme Source");
    else if (!RegDel(HKEY_CURRENT_USER, key, L"Scheme Source")) return false;
    return CurReadStateFrom(key) == t;
}
inline void CurBroadcast() {
    if (kTestBuild) return;   // the sandbox key is not the live one: nothing to reload
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}
inline std::wstring CurPendingPath() { return CurPrevPath() + L".pending"; }
// The scheme about to be replaced is written beside the saved one first and takes its place only once the
// change has succeeded, so a failed change never costs the user the previous scheme they could go back to.
inline bool CurSavePending(const CurState& s) {
    EnsureDir(CurDataDir());
    const std::string u = WToUtf8(CurEncodeState(s));
    return WriteAtomic(CurPendingPath(), reinterpret_cast<const uint8_t*>(u.data()), u.size());
}
inline bool CurPromotePending() { return MoveFileExW(CurPendingPath().c_str(), CurPrevPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0; }
inline bool CurLoadPrevious(CurState& s) { return FileExists(CurPrevPath()) && CurDecodeState(Utf8ToW(ReadText(CurPrevPath())), s); }

// Shared by apply, restore previous and restore defaults: save what is active, write the new state,
// put the old one back if any write or the read-back fails.
inline bool CurSwitchTo(const CurState& target, const std::wstring& title, std::wstring& err) {
    const CurState before = CurReadState();
    if (before == target) { LogI(title + L": already active, nothing to change."); return true; }
    if (!CurSavePending(before)) { err = L"The current cursor scheme could not be saved, so nothing was changed."; LogE(title + L": " + err); return false; }
    if (!CurWriteState(target, title, true)) {
        const int keep = CurTestFailAfter(); CurTestFailAfter() = -1;
        const bool back = CurWriteState(before, title + L" (rollback)", false);
        CurTestFailAfter() = keep;
        if (back) DeleteFileW(CurPendingPath().c_str());   // nothing changed: the saved previous scheme stays as it was
        else CurPromotePending();                           // the state is mixed: what was active becomes the scheme to restore
        CurBroadcast();
        err = back ? L"The cursor settings could not be written. The previous scheme was put back." : L"The cursor settings could not be written, and the previous scheme could not be fully put back. Use Restore previous scheme.";
        LogE(title + L": " + err);
        return false;
    }
    if (!CurPromotePending()) LogW(title + L": the previous scheme could not be saved for Restore previous scheme (the change itself succeeded and is in the undo journal).");
    CurBroadcast();
    return true;
}

// Applies a discovered scheme. Refused when it maps no role or when a mapped file is missing or not a cursor.
inline bool CurApply(CurScheme sc, std::wstring& err) {
    CurCheckFiles(sc);
    if (sc.Mapped() == 0) { err = L"\"" + sc.name + L"\" has no cursor file that matches a Windows cursor role."; LogE(err); return false; }
    if (!sc.invalid.empty()) { err = L"\"" + sc.name + L"\": " + std::to_wstring(sc.invalid.size()) + L" file(s) are missing or are not cursor files (first: " + sc.invalid[0] + L"). Nothing was changed."; LogE(err); return false; }
    const CurState target = CurTargetState(sc, WinDir());
    const std::wstring title = L"Mouse cursors: " + sc.name;
    if (!CurSwitchTo(target, title, err)) return false;
    if (!sc.registered) {   // listed in Mouse Properties under its name, like a scheme saved there
        const std::wstring list = CurBuildSchemeList(target.value);
        if (!JRegSet(CurJournalGroup(), title, HKEY_CURRENT_USER, CurSchemesKey(), sc.name, REG_EXPAND_SZ, list.c_str(), static_cast<DWORD>((list.size() + 1) * sizeof(wchar_t))))
            LogW(L"\"" + sc.name + L"\" is applied, but it could not be added to the scheme list in Mouse Properties.");   // SEC-3: said, not hidden
    }
    const int missing = kCurRoles - sc.Mapped();
    LogOk(L"Cursor scheme \"" + sc.name + L"\" applied: " + std::to_wstring(sc.Mapped()) + L" of 17 roles" + (missing ? L"; " + std::to_wstring(missing) + L" role(s) it does not provide use the Windows cursor." : L".") +
          (kTestBuild ? L" (test build: sandbox copy of the cursor settings)" : L""));
    return true;
}
inline bool CurRestorePrevious(std::wstring& err) {
    CurState prev;
    if (!CurLoadPrevious(prev)) { err = FileExists(CurPrevPath()) ? L"The saved previous cursor scheme is damaged and was not used." : L"No previous cursor scheme has been saved yet."; LogE(err); return false; }
    if (!CurSwitchTo(prev, L"Mouse cursors: restore previous scheme", err)) return false;
    LogOk(L"Previous cursor scheme restored" + (prev.name.empty() ? std::wstring(L".") : L": \"" + prev.name + L"\"."));
    return true;
}
inline bool CurRestoreDefaults(std::wstring& err) {
    if (!CurSwitchTo(CurWindowsDefault(), L"Mouse cursors: Windows default", err)) return false;
    LogOk(L"Windows default mouse cursors restored.");
    return true;
}

// Safe one-time cursor initialization (and automatic repair of a partial/broken "As3aD" 2/17 Custom mix).
// Never overwrites an existing valid user custom cursor scheme, and never reapplies on subsequent launches.
inline bool CurEnsureFirstRunDefault(const std::vector<std::wstring>& roots) {
    if (kTestBuild) return false;
    const CurState now = CurReadState();
    int validRoles = 0;
    for (int i = 0; i < kCurRoles; ++i) {
        if (now.exists[i] && !now.value[i].empty() && CurFileHeaderOk(Expand(now.value[i]))) ++validRoles;
    }
    const std::wstring curNameLow = CurLower(CurTrim(now.name));
    const bool brokenAs3aD = (curNameLow == L"as3ad" || curNameLow == L"as3ad2") && validRoles > 0 && validRoles < kCurRoles;
    const bool alreadyInit = Cfg().Bool(L"cursors.firstRunInitDone", false);
    if (alreadyInit && !brokenAs3aD) return false;

    if (!CurStateNeedsFirstRunDefault(now, validRoles)) {
        // User already has a valid custom cursor scheme; mark initialized and preserve it untouched.
        Cfg().SetBool(L"cursors.firstRunInitDone", true);
        Cfg().Save();
        return false;
    }

    {   // 2.10: the exact scheme first; the discovery below stays as the fallback
        std::wstring oerr;
        if (CurApply(CurOwnerDefaultScheme(WinDir() + L"\\Cursors"), oerr)) {
            Cfg().SetBool(L"cursors.firstRunInitDone", true); Cfg().Save();
            LogOk(L"Cursors: the As3aD pointer scheme was applied on first run.");
            return true;
        }
    }
    const std::vector<CurScheme> schemes = CurDiscover(roots);
    const int bestIdx = CurSelectBestNamedScheme(schemes, L"As3aD");
    if (bestIdx < 0 || CurSchemeValidRoleCount(schemes[bestIdx]) < 15) return false;

    CurScheme targetScheme = schemes[bestIdx];
    targetScheme.name = L"As3aD";
    std::wstring err;
    if (!CurApply(targetScheme, err)) return false;

    const CurState after = CurReadState();
    int validAfter = 0;
    for (int i = 0; i < kCurRoles; ++i) {
        if (after.exists[i] && !after.value[i].empty() && CurFileHeaderOk(Expand(after.value[i]))) ++validAfter;
    }
    if (validAfter >= targetScheme.Mapped()) {
        Cfg().SetBool(L"cursors.firstRunInitDone", true);
        Cfg().Save();
        LogOk(L"Cursors: verified As3aD scheme (" + std::to_wstring(validAfter) + L" of 17 roles) initialized.");
        return true;
    }
    return false;
}

}   // namespace ak
