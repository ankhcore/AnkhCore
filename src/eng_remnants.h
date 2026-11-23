// eng_remnants.h - 2.7 K1: post-uninstall remnant candidates and their protection classes (Apps Manager, workstream K, part E).
//
// This slice SCANS AND CLASSIFIES. It deletes nothing and it contains no delete call. `RemMayRemove` is the gate that a later cleanup step
// (K2: confirmation, recovery point, execution, log) must pass for every single item.
//
// The rules that the 2.7 prompt gives for the cleaner, as code:
//   * a scan happens only after a SUCCESSFUL uninstall (the app is no longer installed): otherwise there is nothing to scan;
//   * a candidate comes from EXACT evidence only: the application's own InstallLocation, a folder whose name equals the application name
//     (or one of its executables' names) exactly, a shortcut whose TARGET lies inside the removed install folder and is gone, the
//     application's own uninstall key, `HKCU\\Software\\<publisher>\\<name>`. A similar or partial name never creates a candidate;
//   * three classes: Safe (empty, or only logs/cache/temp files), Confirm (the person must review it), Protected (never removed, not even
//     with a confirmation: system and profile folders, personal files and folders that may hold them, links, anything shared with
//     another installed application or the publisher, anything in use, anything that is not a verified application-specific key);
//   * "cannot tell" is Protected; every candidate carries its reason and the evidence it came from.
// Everything outside is injected (`RemFs`, `RemReg`, `RemEnv`), so the tests run on a fake machine; a read-only live implementation of each
// is provided and used by the application later.
#pragma once
#include "base.h"

namespace ak {

enum class RemClass { Safe, Confirm, Protected };
inline const wchar_t* RemClassLabel(RemClass c) { return c == RemClass::Safe ? L"Safe to remove" : c == RemClass::Confirm ? L"Needs your confirmation" : L"Protected: never removed automatically"; }
enum class RemKind { Folder, Shortcut, RegistryKey };

struct RemApp {
    std::wstring name, publisher;
    std::wstring installLocation;            // the application's own InstallLocation as the uninstall entry gave it (may be empty or wrong)
    std::wstring uninstallKey;               // "HKLM\\SOFTWARE\\...\\Uninstall\\<id>" of its uninstall entry
    std::vector<std::wstring> exeNames;      // executable base names without extension, e.g. L"chrome"
    bool uninstalledOk = false;              // set only when the uninstall was verified to have succeeded
};

struct RemDirEntry { std::wstring name; bool dir = false; bool reparse = false; uint64_t size = 0; };
struct RemFs {
    std::function<bool(const std::wstring&)> exists, isDir, isReparse;
    std::function<std::vector<RemDirEntry>(const std::wstring&)> list;             // direct children
    std::function<bool(const std::wstring& lnk, std::wstring& target)> shortcutTarget;
};
struct RemReg {
    std::function<bool(const std::wstring& key)> keyExists;                        // "HKCU\\Software\\X"
    std::function<std::wstring(const std::wstring& key, const std::wstring& value)> str;
};
struct RemEnv {
    std::wstring windows, programFiles, programFilesX86, programData, appData, localAppData, userProfile, temp, startMenuCommon, startMenuUser;
    std::vector<std::wstring> personalFolders;        // Documents, Desktop, Downloads, Pictures, Music, Videos, OneDrive
    std::vector<std::wstring> otherInstallLocations;  // install folders of OTHER installed applications
    std::vector<std::wstring> runningImages;          // full paths of the images of running processes
};

struct RemCandidate {
    RemKind kind = RemKind::Folder;
    std::wstring path;                  // file-system path, or "HKCU\\..." for a registry key
    std::wstring origin;                // what exact evidence produced it
    RemClass cls = RemClass::Protected;
    std::wstring reason;                // why it has this class
    int confidence = 0;                 // 0..100: how sure it belongs to the removed application
    uint64_t bytes = 0;                 // size of what was inspected (folders)
    bool fullyInspected = true;
};

struct RemLimits { size_t maxEntries = 400; int maxDepth = 4; };

namespace remdetail {
inline std::wstring Norm(std::wstring p) {   // backslashes, no duplicate separators, no trailing separator (except a drive root), lower case
    for (auto& c : p) if (c == L'/') c = L'\\';
    std::wstring o; for (wchar_t c : p) { if (c == L'\\' && !o.empty() && o.back() == L'\\' && o.size() > 1) continue; o += static_cast<wchar_t>(towlower(c)); }
    while (o.size() > 3 && o.back() == L'\\') o.pop_back();
    return o;
}
// An absolute drive path without "." or ".." components or device prefixes.
inline bool ValidAbs(const std::wstring& n) {
    if (n.size() < 3 || !iswalpha(n[0]) || n[1] != L':' || n[2] != L'\\') return false;
    size_t i = 3;
    while (i <= n.size()) {
        const size_t j = n.find(L'\\', i); const std::wstring comp = n.substr(i, (j == std::wstring::npos ? n.size() : j) - i);
        if (comp == L"." || comp == L"..") return false;
        for (wchar_t c : comp) if (c < 0x20 || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') return false;
        if (j == std::wstring::npos) break; i = j + 1;
    }
    return true;
}
inline bool Under(const std::wstring& n, const std::wstring& dirN) { return !dirN.empty() && n.size() > dirN.size() && n.compare(0, dirN.size(), dirN) == 0 && (dirN.back() == L'\\' || n[dirN.size()] == L'\\'); }
inline bool SameOrUnder(const std::wstring& n, const std::wstring& dirN) { return n == dirN || Under(n, dirN); }
inline std::wstring Leaf(const std::wstring& n) { const size_t k = n.rfind(L'\\'); return k == std::wstring::npos ? n : n.substr(k + 1); }
inline std::wstring Join(const std::wstring& a, const std::wstring& b) { return a.empty() ? L"" : a + L"\\" + b; }
inline std::wstring Trim(std::wstring s) { while (!s.empty() && iswspace(s.back())) s.pop_back(); size_t a = 0; while (a < s.size() && iswspace(s[a])) ++a; return s.substr(a); }
inline bool HasExt(const std::wstring& nameLower, std::initializer_list<const wchar_t*> exts) { for (const wchar_t* e : exts) { const size_t l = wcslen(e); if (nameLower.size() > l && nameLower.compare(nameLower.size() - l, l, e) == 0) return true; } return false; }
// folder names that belong to the system or to many applications
inline bool SharedFolderName(const std::wstring& leaf) {
    static const wchar_t* const k[] = {L"microsoft", L"windows", L"windowsapps", L"packages", L"programs", L"common files", L"temp", L"microsoft shared", L"microsoft visual studio", L"dotnet", L".net", L"windows nt",
                                       L"internet explorer", L"nvidia corporation", L"nvidia", L"intel", L"amd", L"realtek", L"google", L"mozilla", L"adobe", L"oracle", L"java", L"python", L"node", L"nodejs"};
    for (const wchar_t* s : k) if (leaf == s) return true;
    return false;
}
inline bool SharedPublisherName(const std::wstring& pub) { static const wchar_t* const k[] = {L"microsoft", L"microsoft corporation", L"windows", L"intel", L"intel corporation", L"nvidia", L"nvidia corporation", L"amd", L"advanced micro devices", L"realtek"}; for (const wchar_t* s : k) if (pub == s) return true; return false; }
}  // namespace remdetail

// ---------------------------------------------------------------- folder classification
// What is inside a folder, inspected without following links and within limits.
struct RemContent { uint64_t bytes = 0; size_t files = 0; bool onlyRegenerable = true, personal = false, reparseInside = false, truncated = false; };
inline void RemInspect(const RemFs& fs, const std::wstring& dir, const RemLimits& lim, int depth, RemContent& c, size_t& seen) {
    if (!fs.list) { c.truncated = true; return; }
    for (const RemDirEntry& e : fs.list(dir)) {
        if (++seen > lim.maxEntries) { c.truncated = true; return; }
        const std::wstring child = dir + L"\\" + e.name;
        if (e.reparse) { c.reparseInside = true; continue; }
        if (e.dir) { if (depth + 1 > lim.maxDepth) { c.truncated = true; continue; } RemInspect(fs, child, lim, depth + 1, c, seen); continue; }
        ++c.files; c.bytes += e.size;
        const std::wstring n = remdetail::Norm(e.name);
        if (remdetail::HasExt(n, {L".log", L".tmp", L".cache", L".etl", L".dmp"}) == false) c.onlyRegenerable = false;
        if (remdetail::HasExt(n, {L".doc", L".docx", L".xls", L".xlsx", L".ppt", L".pptx", L".pdf", L".odt", L".rtf", L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".heic", L".mp3", L".wav", L".flac", L".mp4", L".mkv", L".avi", L".mov",
                                     L".zip", L".7z", L".rar", L".kdbx", L".key", L".pem", L".pfx", L".wallet", L".sav", L".psd", L".ai", L".dwg"}) && !remdetail::HasExt(n, {L".log"})) c.personal = true;
        if (c.truncated) return;
    }
}

inline RemCandidate RemClassifyFolder(const std::wstring& pathIn, const std::wstring& origin, int baseConfidence, const RemApp& app, const RemEnv& env, const RemFs& fs, const RemLimits& lim = {}) {
    using namespace remdetail;
    RemCandidate c; c.kind = RemKind::Folder; c.origin = origin; c.confidence = baseConfidence;
    const std::wstring n = Norm(pathIn); c.path = pathIn;
    auto prot = [&](const wchar_t* why) { c.cls = RemClass::Protected; c.reason = why; return c; };
    if (!ValidAbs(n)) return prot(L"The path is not a plain absolute path, so nothing is removed.");
    if (n.size() <= 3) return prot(L"A drive root is never removed.");
    const std::wstring win = Norm(env.windows);
    // containers: the folder itself, or a parent of one, is a system or profile folder
    for (const std::wstring* k : {&env.windows, &env.programFiles, &env.programFilesX86, &env.programData, &env.appData, &env.localAppData, &env.userProfile, &env.temp, &env.startMenuCommon, &env.startMenuUser}) {
        const std::wstring kn = Norm(*k); if (kn.empty()) continue;
        if (n == kn || Under(kn, n)) return prot(L"A system or profile folder (or a folder that contains one).");
    }
    if (!win.empty() && Under(n, win)) return prot(L"Inside the Windows folder.");
    for (const std::wstring& pf : env.personalFolders) { const std::wstring p = Norm(pf); if (!p.empty() && (n == p || Under(n, p) || Under(p, n))) return prot(L"Your personal files and folders are never removed."); }
    if (!env.userProfile.empty()) {   // the profile's direct children other than AppData-like roots are the person's own folders
        const std::wstring up = Norm(env.userProfile);
        if (Under(n, up) && Leaf(n) != L"" && n.find(L'\\', up.size() + 1) == std::wstring::npos) return prot(L"A folder directly in your user profile.");
    }
    if (fs.isReparse && fs.isReparse(pathIn)) return prot(L"A link (junction or symbolic link): it is not followed and not removed.");
    // an ancestor that is a link
    { size_t k = n.find(L'\\', 3); while (k != std::wstring::npos) { if (fs.isReparse && fs.isReparse(n.substr(0, k))) return prot(L"A parent folder is a link: it is not followed."); k = n.find(L'\\', k + 1); } }
    for (const std::wstring& o : env.otherInstallLocations) { const std::wstring on = Norm(o); if (on.empty()) continue; if (n == on || Under(on, n) || Under(n, on)) return prot(L"Shared with another installed application."); }
    for (const std::wstring& img : env.runningImages) { const std::wstring in = Norm(img); if (!in.empty() && Under(in, n)) return prot(L"A program in this folder is running."); }
    const std::wstring leaf = Leaf(n);
    if (SharedFolderName(leaf)) return prot(L"A folder shared by the system or by many applications.");
    const std::wstring pub = Norm(app.publisher);
    if (!pub.empty() && leaf == pub) return prot(L"Named after the publisher: it can hold other applications of the same publisher.");
    if (!fs.exists || !fs.exists(pathIn)) return prot(L"It no longer exists.");
    RemContent ct; size_t seen = 0;
    RemInspect(fs, pathIn, lim, 0, ct, seen);
    c.bytes = ct.bytes; c.fullyInspected = !ct.truncated;
    if (ct.reparseInside) return prot(L"It contains a link, which is not followed.");
    if (ct.personal) return prot(L"It contains documents, pictures, archives or databases that may be yours.");
    if (ct.truncated) { c.cls = RemClass::Confirm; c.reason = L"Large: it was not inspected completely, so it needs your confirmation."; return c; }
    if (ct.files == 0) { c.cls = RemClass::Safe; c.reason = L"An empty leftover folder."; return c; }
    if (ct.onlyRegenerable) { c.cls = RemClass::Safe; c.reason = L"Only log, temporary or cache files remain."; return c; }
    c.cls = RemClass::Confirm;
    c.reason = origin == L"InstallLocation" ? L"Files remain in the application's own install folder." : L"A folder named exactly like the application still holds files (settings or data).";
    return c;
}

// ---------------------------------------------------------------- registry classification
inline RemCandidate RemClassifyRegKey(const std::wstring& key, const std::wstring& origin, int baseConfidence, const RemApp& app) {
    using namespace remdetail;
    RemCandidate c; c.kind = RemKind::RegistryKey; c.path = key; c.origin = origin; c.confidence = baseConfidence;
    const std::wstring k = Norm(key);
    auto prot = [&](const wchar_t* why) { c.cls = RemClass::Protected; c.reason = why; return c; };
    const std::wstring uk = Norm(app.uninstallKey), pub = Trim(Norm(app.publisher)), nm = Trim(Norm(app.name));
    const bool ownUninstall = !uk.empty() && k == uk && Leaf(k) != L"uninstall" && k.find(L"\\currentversion\\uninstall\\") != std::wstring::npos;
    const bool ownSoftware = !pub.empty() && !nm.empty() && k == L"hkcu\\software\\" + pub + L"\\" + nm;
    if (!ownUninstall && !ownSoftware) return prot(L"Not a registry key that is verified to belong to this application alone.");
    if (SharedPublisherName(pub) && ownSoftware) return prot(L"The publisher's key is shared with its other products.");
    // the application's own uninstall entry lives under ...\\Microsoft\\Windows\\CurrentVersion\\Uninstall by design; only its software key is checked against protected areas
    static const wchar_t* const kBad[] = {L"\\classes", L"\\policies", L"\\services", L"\\microsoft\\windows", L"\\run", L"\\drivers", L"\\winlogon"};
    if (!ownUninstall) for (const wchar_t* b : kBad) if (k.find(b) != std::wstring::npos) return prot(L"A protected part of the registry.");
    c.cls = RemClass::Confirm;
    c.reason = ownUninstall ? L"The application's own uninstall entry is still listed." : L"The application's own settings key under its publisher still exists.";
    return c;
}

// ---------------------------------------------------------------- the scan
inline std::vector<RemCandidate> RemScan(const RemApp& app, const RemEnv& env, const RemFs& fs, const RemReg& reg, const RemLimits& lim = {}) {
    using namespace remdetail;
    std::vector<RemCandidate> out;
    if (!app.uninstalledOk) return out;                       // only after a verified successful uninstall
    const std::wstring name = Trim(app.name);
    if (name.empty()) return out;
    std::set<std::wstring> seen;                              // one candidate per path
    auto add = [&](RemCandidate c) { const std::wstring key = (c.kind == RemKind::RegistryKey ? L"r:" : L"f:") + Norm(c.path); if (seen.insert(key).second) out.push_back(std::move(c)); };

    if (!Trim(app.installLocation).empty()) {
        if (fs.exists && fs.exists(app.installLocation)) add(RemClassifyFolder(app.installLocation, L"InstallLocation", 90, app, env, fs, lim));
    }
    // exact-name folders under the usual roots. No directory is listed to look for "similar" names: only these exact paths are asked for.
    std::vector<std::wstring> names{name};
    for (const std::wstring& e : app.exeNames) { const std::wstring t = Trim(e); if (!t.empty() && Norm(t) != Norm(name)) names.push_back(t); }
    std::vector<std::pair<std::wstring, std::wstring>> roots = {
        {env.appData, L"AppData"}, {env.localAppData, L"LocalAppData"}, {Join(env.localAppData, L"Programs"), L"LocalAppData\\Programs"},
        {env.programData, L"ProgramData"}, {env.programFiles, L"Program Files"}, {env.programFilesX86, L"Program Files (x86)"}};
    for (const auto& r : roots) {
        if (r.first.empty()) continue;
        for (const std::wstring& n : names) {
            if (n.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || n == L"." || n == L"..") continue;   // not a plain folder name
            const std::wstring p = r.first + L"\\" + n;
            if (fs.exists && fs.exists(p)) add(RemClassifyFolder(p, L"a folder named exactly like the application under " + r.second, 70, app, env, fs, lim));
        }
        if (!Trim(app.publisher).empty() && Trim(app.publisher).find_first_of(L"\\/:*?\"<>|") == std::wstring::npos) {   // shown so the person sees what is kept, and why
            const std::wstring p = r.first + L"\\" + Trim(app.publisher);
            if (fs.exists && fs.exists(p)) add(RemClassifyFolder(p, L"the publisher's folder under " + r.second, 40, app, env, fs, lim));
        }
    }
    // shortcuts whose TARGET lay inside the removed install folder and is gone (the name of the shortcut is not evidence)
    const std::wstring inst = Norm(app.installLocation);
    if (!inst.empty() && ValidAbs(inst) && fs.list && fs.shortcutTarget) {
        for (const std::wstring& base : {env.startMenuCommon, env.startMenuUser}) {
            if (base.empty()) continue;
            std::vector<std::wstring> dirs{base};
            for (const RemDirEntry& e : fs.list(base)) if (e.dir && !e.reparse) dirs.push_back(base + L"\\" + e.name);
            for (const std::wstring& d : dirs)
                for (const RemDirEntry& e : fs.list(d)) {
                    if (e.dir || e.reparse || !HasExt(Norm(e.name), {L".lnk"})) continue;
                    const std::wstring lnk = d + L"\\" + e.name; std::wstring target;
                    if (!fs.shortcutTarget(lnk, target) || target.empty()) continue;
                    const std::wstring tn = Norm(target);
                    if (!ValidAbs(tn) || !Under(tn, inst)) continue;
                    if (fs.exists && fs.exists(target)) continue;   // the target is still there: not broken
                    RemCandidate c; c.kind = RemKind::Shortcut; c.path = lnk; c.origin = L"a shortcut to a program in the removed install folder"; c.confidence = 85;
                    c.cls = RemClass::Safe; c.reason = L"A shortcut whose program was in the removed install folder and no longer exists.";
                    add(std::move(c));
                }
        }
    }
    if (reg.keyExists) {
        if (!Trim(app.uninstallKey).empty() && reg.keyExists(app.uninstallKey)) add(RemClassifyRegKey(app.uninstallKey, L"the application's own uninstall entry", 80, app));
        const std::wstring pub = Trim(app.publisher);
        if (!pub.empty() && pub.find_first_of(L"\\/") == std::wstring::npos && name.find_first_of(L"\\/") == std::wstring::npos) {
            const std::wstring k = L"HKCU\\Software\\" + pub + L"\\" + name;
            if (reg.keyExists(k)) add(RemClassifyRegKey(k, L"HKCU\\Software\\<publisher>\\<application>", 60, app));
        }
    }
    std::sort(out.begin(), out.end(), [](const RemCandidate& a, const RemCandidate& b) { if (a.cls != b.cls) return static_cast<int>(a.cls) < static_cast<int>(b.cls); return Lower(a.path) < Lower(b.path); });
    return out;
}

// ---------------------------------------------------------------- the removal gate (used by K2)
struct RemDecision { bool allowed = false; bool needsConfirmation = false; std::wstring reason; };
inline RemDecision RemMayRemove(const RemCandidate& c, bool userConfirmed) {
    RemDecision d;
    if (c.cls == RemClass::Protected) { d.reason = c.reason.empty() ? std::wstring(L"Protected.") : c.reason; return d; }   // never, not even with a confirmation
    if (c.cls == RemClass::Confirm) { d.needsConfirmation = true; d.allowed = userConfirmed; d.reason = userConfirmed ? L"Confirmed by the person." : L"Needs the person's confirmation first."; return d; }
    d.allowed = true; d.reason = L"Safe to remove."; return d;
}

// ---------------------------------------------------------------- read-only live implementations
inline RemFs RemLiveFs() {
    RemFs f;
    f.exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    f.isDir = [](const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); };
    f.isReparse = [](const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT); };
    f.list = [](const std::wstring& dir) {
        std::vector<RemDirEntry> v; WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return v;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            RemDirEntry e; e.name = fd.cFileName; e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0; e.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow; v.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h); return v;
    };
    f.shortcutTarget = [](const std::wstring& lnk, std::wstring& target) {
        // The shortcut's target is read through the shell link COM interface (read-only).
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&sl)))) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))) {
                if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) { wchar_t buf[MAX_PATH * 2] = L""; WIN32_FIND_DATAW fd{}; if (SUCCEEDED(sl->GetPath(buf, static_cast<int>(std::size(buf)), &fd, SLGP_RAWPATH)) && buf[0]) { target = buf; ok = true; } }
                pf->Release();
            }
            sl->Release();
        }
        if (SUCCEEDED(co)) CoUninitialize();
        return ok;
    };
    return f;
}
namespace remdetail {
inline bool SplitKey(const std::wstring& key, HKEY& root, std::wstring& sub) {
    const size_t k = key.find(L'\\');
    const std::wstring r = key.substr(0, k); sub = k == std::wstring::npos ? L"" : key.substr(k + 1);
    if (r == L"HKCU") root = HKEY_CURRENT_USER; else if (r == L"HKLM") root = HKEY_LOCAL_MACHINE; else return false;
    return true;
}
}  // namespace remdetail
inline RemReg RemLiveReg() {
    RemReg r;
    r.keyExists = [](const std::wstring& key) { HKEY root; std::wstring sub; return remdetail::SplitKey(key, root, sub) && !sub.empty() && RegKeyExists(root, sub); };
    r.str = [](const std::wstring& key, const std::wstring& value) { HKEY root; std::wstring sub; return remdetail::SplitKey(key, root, sub) ? RegStr(root, sub, value) : std::wstring(); };
    return r;
}

}  // namespace ak
