// AnkhCore 2.5 — installer logic shared by setup.cpp and the uninstaller (pure parts unit-tested in
// tests/test_base.cpp). The 2.5 setup is a *core* installer: it installs or upgrades AnkhCore and its
// hardware-monitoring helper and nothing else. It never touches Windhawk, mods, themes, cursors,
// wallpapers or icons (owner decision 2026-11-08; see docs/ASSETS.md).
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cwchar>
#include <algorithm>
#include <functional>
#include "op_result.h"
#pragma comment(lib, "bcrypt.lib")

namespace ak {

// ---------------------------------------------------------------- uninstall result (SEC-3)
// What the uninstaller did, collected step by step, and the message it shows. The message says "removed"
// only when every part of the removal was confirmed; anything that failed is named.
struct RemoveReport {
    std::vector<std::wstring> failed;   // parts of the install that are still there, in words ("the startup task")
    bool deferred = false;              // the program folder still exists: its files go at the next restart
};
struct UninstallReport {
    bool undoAsked = false;
    UndoCount undo;                     // journal entries selected / put back
    int icons = -1;                     // -1 nothing to do, 0 failed, 1 put back
    bool stillRunning = false;          // an AnkhCore window or the sensor helper did not stop
    RemoveReport removal;
    bool fullClean = false;             // 2.12: the person chose the full clean
    int cleanedThemes = 0, cleanedCursors = 0, cleanedWallpapers = 0, cleanedMods = 0;   // what the full clean took out of Windows and Windhawk
    std::vector<std::wstring> cleanFailed;   // what it could not take out
};
struct UninstallMessage { std::wstring text; bool ok = false; };
inline UninstallMessage UninstallSummary(const UninstallReport& r, const std::wstring& dataDir) {
    UninstallMessage m;
    const bool removedAll = r.removal.failed.empty();
    const bool undoOk = !r.undoAsked || (r.undo.Success() && r.icons != 0);
    m.ok = removedAll && undoOk && !r.stillRunning && r.cleanFailed.empty();
    m.text = removedAll ? std::wstring(L"AnkhCore was removed.") : std::wstring(L"AnkhCore was not completely removed.");
    if (removedAll && r.removal.deferred) m.text += L" The rest of the program folder is deleted at the next restart.";
    if (!removedAll) {
        m.text += L"\n\nStill there:";
        for (const auto& f : r.removal.failed) m.text += L"\n  \x2022 " + f;
        m.text += L"\nRun the uninstaller again after a restart.";
    }
    if (r.stillRunning) m.text += L"\n\nAn AnkhCore window or the hardware-monitoring helper did not stop. Restart Windows to end it.";
    if (r.undoAsked) {
        m.text += L"\n\nYour Windows changes: " + r.undo.Text();
        if (r.icons == 1) m.text += L"\nWindows icons: put back (some return after a restart).";
        else if (r.icons == 0) m.text += L"\nWindows icons: could NOT be fully put back. See the log.";
    } else m.text += L"\n\nYour Windows changes were kept as they are.";
    if (r.fullClean) {
        m.text += L"\n\nFull clean: " + std::to_wstring(r.cleanedThemes) + L" theme item(s), " + std::to_wstring(r.cleanedCursors) + L" cursor pack(s), " + std::to_wstring(r.cleanedWallpapers) + L" wallpaper folder(s) and " + std::to_wstring(r.cleanedMods) + L" Windhawk mod(s) were removed. Windhawk itself and your own mods stay.";
        for (const auto& f : r.cleanFailed) m.text += L"\n  \x2022 not removed: " + f;
    }
    m.text += L"\n\nThe PawnIO hardware driver stays installed (other monitoring tools can use it); remove it from Settings > Apps if you want.";
    if (r.fullClean) m.text += L"\nAnkhCore's settings and data were deleted; your Backup and Recovery folders stay in " + dataDir + L".";
    else m.text += L"\nYour AnkhCore data (logs, backups, original icons) stays in " + dataDir + L".";
    return m;
}

// ---------------------------------------------------------------- versions
struct Ver { int major = 0, minor = 0, patch = 0; bool valid = false; int build = 0; };
// "2.2", "2.5.0", "2.5.0.1" (the fourth part is the build: a preview or a fix of the same version); anything else is invalid.
inline Ver ParseVer(const std::wstring& s) {
    Ver v; int part[4] = {0, 0, 0, 0}, n = 0; size_t i = 0;
    while (i < s.size() && n < 4) {
        if (!iswdigit(s[i])) return Ver{};
        int x = 0, digits = 0;
        while (i < s.size() && iswdigit(s[i])) { x = x * 10 + (s[i] - L'0'); ++i; if (++digits > 6) return Ver{}; }
        part[n++] = x;
        if (i < s.size()) { if (s[i] != L'.') return Ver{}; ++i; if (i == s.size()) return Ver{}; }
    }
    if (n < 2 || i < s.size()) return Ver{};   // fewer than two parts, or something after the fourth
    v.major = part[0]; v.minor = part[1]; v.patch = part[2]; v.build = part[3]; v.valid = true;
    return v;
}
inline int CompareVer(const Ver& a, const Ver& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.build != b.build) return a.build < b.build ? -1 : 1;
    return 0;
}
inline std::wstring VerText(const Ver& v) { return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch) + (v.build ? L"." + std::to_wstring(v.build) : L""); }

// ---------------------------------------------------------------- what the setup will do
enum class InstallKind { Fresh, Upgrade, Repair, Downgrade };
enum class SetupMode { Auto, Repair, Update, CleanInstall };

struct ExistingInstallInfo {
    bool found = false;
    bool partial = false;
    Ver version;
    std::wstring installDir;
};

// installed: an AnkhCore.exe exists in the program folder. An unreadable installed version counts as an
// upgrade (back it up and replace), never as a reason to skip the backup.
inline InstallKind PlanKind(bool installed, const Ver& have, const Ver& next) {
    if (!installed) return InstallKind::Fresh;
    if (!have.valid) return InstallKind::Upgrade;
    const int c = CompareVer(have, next);
    return c < 0 ? InstallKind::Upgrade : c == 0 ? InstallKind::Repair : InstallKind::Downgrade;
}

inline InstallKind ResolveEffectiveInstallKind(SetupMode mode, bool installed, const Ver& have, const Ver& next) {
    if (!installed) return InstallKind::Fresh;
    if (mode == SetupMode::Repair) return InstallKind::Repair;
    if (mode == SetupMode::Update || mode == SetupMode::CleanInstall) return InstallKind::Upgrade;
    return PlanKind(installed, have, next);
}
// Program-folder items the setup replaces (backed up first on an upgrade). Everything else in the
// program folder (7tsp, IconPacks, SecureUxTheme, Assets from 2.2 appearance features) is left alone.
inline const std::vector<std::wstring>& ReplacedItems() { static const std::vector<std::wstring> v{L"AnkhCore.exe", L"Sensors", L"SecureUxTheme", L"Themetools", L"UniGetUI", L"THIRD-PARTY-NOTICES.txt"}; return v; }
inline const std::vector<std::wstring>& KeptItems() { static const std::vector<std::wstring> v{L"7tsp", L"IconPacks", L"IconThemes", L"Mods", L"Tools", L"Assets"}; return v; }
// Backup folder for the previous version: Backup\AnkhCore-2.2.0-20261108-142530
inline std::wstring BackupName(const Ver& v, int y, int mo, int d, int h, int mi, int s) {
    wchar_t b[64]; swprintf(b, 64, L"AnkhCore-%s-%04d%02d%02d-%02d%02d%02d", v.valid ? VerText(v).c_str() : L"unknown", y, mo, d, h, mi, s);
    return b;
}

// ---------------------------------------------------------------- payload layout
// A 2.5 core payload holds only APP/ (program folder) and SENS/ (program folder\Sensors) entries plus
// MANIFEST. Anything else (the 2.2 areas WH/, SRC/, M64/, REG/, WIN/, TSP/, ...) is refused, as is any
// path that could leave its folder.
inline bool SafeRelPath(const std::wstring& p) {
    if (p.empty() || p.size() > 240 || p[0] == L'/' || p.find(L'\\') != std::wstring::npos || p.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(L'/', start);
        const std::wstring seg = p.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (seg.empty() || seg == L"." || seg == L"..") return false;
        for (wchar_t c : seg) if (c < 32 || wcschr(L"<>\"|?*", c)) return false;
        if (seg.back() == L'.' || seg.back() == L' ') return false;   // Windows drops a trailing dot or space: "a." would name the file "a"
        {   // a reserved device name (with or without an extension) is not a file
            std::wstring stem = seg.substr(0, seg.find(L'.'));
            for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
            if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
                (stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) && stem[3] >= L'0' && stem[3] <= L'9')) return false;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}
inline bool PayloadDest(const std::wstring& path, const std::wstring& appDir, std::wstring& dest) {
    std::wstring rel, base;
    if (path.rfind(L"APP/", 0) == 0) { rel = path.substr(4); base = appDir; }
    else if (path.rfind(L"SENS/", 0) == 0) { rel = path.substr(5); base = appDir + L"\\Sensors"; }
    else if (path.rfind(L"SUX/", 0) == 0) { rel = path.substr(4); base = appDir + L"\\SecureUxTheme"; }
    else if (path.rfind(L"THEMETOOLS/", 0) == 0) { rel = path.substr(11); base = appDir + L"\\Themetools"; }
    else if (path.rfind(L"UNIGET/", 0) == 0) { rel = path.substr(7); base = appDir + L"\\UniGetUI"; }   // 2.7: the UniGetUI library AnkhCore updates programs with
    else return false;
    if (!SafeRelPath(rel)) return false;
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    dest = base + L"\\" + rel;
    return true;
}

// ---------------------------------------------------------------- integrity
// MANIFEST entry: one line per payload file, "<sha256 hex lower>  <payload path>" (UTF-8, LF).
struct ManifestEntry { std::string sha; std::wstring path; };
inline bool ParseManifest(const std::string& text, std::vector<ManifestEntry>& out) {
    out.clear(); size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos); if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos); pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') return false;
        const std::string sha = line.substr(0, 64);
        for (char c : sha) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        const std::string p = line.substr(66);
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p.data(), static_cast<int>(p.size()), nullptr, 0);
        if (n <= 0) return false;
        std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), w.data(), n);
        out.push_back({sha, w});
    }
    return !out.empty();
}
inline std::string Sha256Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; uint8_t d[32]{};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            size_t off = 0;
            while (off < size) { const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size - off, 1u << 30)); BCryptHashData(h, const_cast<uint8_t*>(data) + off, chunk, 0); off += chunk; }
            if (BCryptFinishHash(h, d, 32, 0) == 0) { static const char* x = "0123456789abcdef"; for (uint8_t b : d) { hex += x[b >> 4]; hex += x[b & 15]; } }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}
inline std::string Sha256File(const std::wstring& path) {
    // a scanner or indexer may hold the file open (read, or even write): sharing everything keeps a right file from failing as "empty"
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::vector<uint8_t> buf(static_cast<size_t>(sz.QuadPart)); size_t at = 0; bool ok = true;
    while (ok && at < buf.size()) { DWORD got = 0; const DWORD want = static_cast<DWORD>(std::min<size_t>(buf.size() - at, 1u << 26)); ok = ReadFile(f, buf.data() + at, want, &got, nullptr) && got > 0; at += got; }   // chunked: a file over 4 GiB no longer truncates the read
    CloseHandle(f);
    return ok ? Sha256Hex(buf.data(), buf.size()) : std::string();
}

// ---------------------------------------------------------------- INS-3 (D44): Windows 11 Taskbar Profile
struct TaskbarRegEntry {
    std::wstring subkeySuffix;   // L"" for Explorer\Advanced, L"\\People" for Explorer\Advanced\People
    std::wstring valueName;
    DWORD value = 0;
};

// Validates and parses `AnkhCore_Taskbar_Profile.reg`.
// Only accepts HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced (and \People)
// and known Taskbar DWORD values; rejects any HKLM, policy, or unrelated registry key.
inline bool ParseTaskbarProfileReg(const std::string& regText,
                                   std::vector<TaskbarRegEntry>& outEntries,
                                   std::wstring& outErr) {
    outEntries.clear();
    outErr.clear();
    std::string text = regText;
    if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF &&
        static_cast<unsigned char>(text[1]) == 0xFE) {
        const size_t wcCount = (text.size() - 2) / sizeof(wchar_t);
        std::wstring w(wcCount, L'\0');
        if (wcCount > 0) memcpy(w.data(), text.data() + 2, wcCount * sizeof(wchar_t));
        text = WToUtf8(w);
    } else if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
               static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    bool sawHeader = false;
    std::wstring curSuffix = L"<none>";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) ++first;
        line.erase(0, first);
        if (line.empty() || line[0] == ';') continue;
        if (!sawHeader) {
            if (line != "Windows Registry Editor Version 5.00") {
                outErr = L"Invalid .reg header (expected Windows Registry Editor Version 5.00).";
                return false;
            }
            sawHeader = true;
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            const std::string key = line.substr(1, line.size() - 2);
            if (key == "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced") {
                curSuffix = L"";
            } else if (key == "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\People") {
                curSuffix = L"\\People";
            } else {
                outErr = L"Disallowed registry key in Taskbar Profile: " +
                         std::wstring(key.begin(), key.end());
                return false;
            }
            continue;
        }
        if (curSuffix == L"<none>") {
            outErr = L"Registry value outside a section in Taskbar Profile.";
            return false;
        }
        // Expect "<Name>"=dword:<8 hex digits>
        if (line.front() != '"') {
            outErr = L"Unexpected line in Taskbar Profile.";
            return false;
        }
        const size_t q2 = line.find('"', 1);
        if (q2 == std::string::npos || line.compare(q2 + 1, 7, "=dword:") != 0) {
            outErr = L"Only DWORD values are allowed in Taskbar Profile.";
            return false;
        }
        const std::string nameA = line.substr(1, q2 - 1);
        const std::string hexA  = line.substr(q2 + 8);
        if (nameA.empty() || hexA.empty() || hexA.size() > 8) {
            outErr = L"Malformed DWORD value in Taskbar Profile.";
            return false;
        }
        char* endp = nullptr;
        const unsigned long val = std::strtoul(hexA.c_str(), &endp, 16);
        if (!endp || *endp != '\0') {
            outErr = L"Invalid hex DWORD in Taskbar Profile.";
            return false;
        }
        static const char* kAllowedAdv[] = {
            "TaskbarAnimations", "TaskbarGlomLevel", "MMTaskbarGlomLevel",
            "TaskbarAutoHideInTabletMode", "TaskbarMn", "TaskbarDa",
            "ShowTaskViewButton", "ShowCopilotButton", "TaskbarSn",
            "TaskbarAl", "TaskbarSi", "TaskbarBadges", "TaskbarFlashing"
        };
        bool allowed = false;
        if (curSuffix.empty()) {
            for (const char* k : kAllowedAdv) if (nameA == k) { allowed = true; break; }
        } else if (curSuffix == L"\\People") {
            allowed = (nameA == "PeopleBand");
        }
        if (!allowed) {
            outErr = L"Unrecognized or disallowed value in Taskbar Profile: " +
                     std::wstring(nameA.begin(), nameA.end());
            return false;
        }
        outEntries.push_back({curSuffix, std::wstring(nameA.begin(), nameA.end()), static_cast<DWORD>(val)});
    }
    if (!sawHeader || outEntries.size() != 14) {
        outErr = L"Taskbar Profile is incomplete (expected 14 validated Taskbar settings, found " +
                 std::to_wstring(outEntries.size()) + L").";
        return false;
    }
    return true;
}

inline std::wstring FormatTaskbarProfileSummary(bool selected, bool isWin11, bool applied, const std::wstring& detail) {
    std::wstring s = L"Taskbar Profile: ";
    s += selected ? L"Selected" : L"Not selected";
    s += isWin11 ? L" | OS: Windows 11 (supported)" : L" | OS: Windows 10 or older (not supported)";
    if (!isWin11) {
        s += L" | Result: Skipped (Windows 11 only)";
    } else if (!selected) {
        s += L" | Result: Skipped (not selected)";
    } else if (applied) {
        s += L" | Result: Applied";
    } else {
        s += L" | Result: Failed / skipped";
    }
    if (!detail.empty()) s += L" (" + detail + L")";
    return s;
}

// FC-16: Atomic, verified directory placement for icon themes (stages into <to>.ankh-stage,
// copies & SHA-256 verifies every file, runs validateDir, then swaps into <to> with rollback).
struct PlaceTreeFaults {
    std::wstring failCopyFileName;
    bool failSwap = false;
};

inline bool CopyDirChecked(const std::wstring& from, const std::wstring& to,
                           const PlaceTreeFaults* faults, std::wstring& err) {
    if (!DirExists(from)) { err = L"Source folder does not exist."; return false; }
    if (SHCreateDirectoryExW(nullptr, to.c_str(), nullptr) != ERROR_SUCCESS && !DirExists(to)) {
        err = L"Could not create staging directory.";
        return false;
    }
    for (const auto& d : ListDir(from, true)) {
        if (!CopyDirChecked(from + L"\\" + d, to + L"\\" + d, faults, err)) return false;
    }
    for (const auto& f : ListDir(from, false)) {
        if (faults && !faults->failCopyFileName.empty() && _wcsicmp(f.c_str(), faults->failCopyFileName.c_str()) == 0) {
            err = L"Simulated copy failure on '" + f + L"'.";
            return false;
        }
        const std::wstring src = from + L"\\" + f;
        const std::wstring dst = to + L"\\" + f;
        std::vector<uint8_t> data;
        if (!ReadAll(src, data) || !WriteAllFlushed(dst, data.data(), data.size())) {
            err = L"Failed to copy '" + f + L"'.";
            return false;
        }
        if (Sha256File(dst) != Sha256Hex(data.data(), data.size())) {
            err = L"Verification failed after copying '" + f + L"'.";
            return false;
        }
    }
    return true;
}

inline bool PlaceVerifiedTree(const std::wstring& from, const std::wstring& to,
                              const std::function<bool(const std::wstring&, std::wstring&)>& validateDir,
                              std::wstring& err,
                              const PlaceTreeFaults* faults = nullptr) {
    err.clear();
    if (!DirExists(from)) { err = L"Source folder is missing."; return false; }
    const std::wstring parent = DirOf(to);
    if (!parent.empty()) SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);
    const std::wstring staged = to + L".ankh-stage";
    const std::wstring backup = to + L".ankh-old";
    Evict(staged, true, true);
    Evict(backup, true, true);

    if (!CopyDirChecked(from, staged, faults, err)) {
        Evict(staged, true, true);
        return false;
    }
    if (validateDir && !validateDir(staged, err)) {
        Evict(staged, true, true);
        return false;
    }

    const bool hadExisting = DirExists(to);
    if (hadExisting) {
        if (!MoveFileExW(to.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            err = L"Could not move existing destination aside.";
            Evict(staged, true, true);
            return false;
        }
    }
    if ((faults && faults->failSwap) || !MoveFileExW(staged.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        err = L"Could not place verified folder into destination.";
        if (hadExisting) MoveFileExW(backup.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH);
        Evict(staged, true, true);
        return false;
    }
    if (hadExisting) Evict(backup, true, true);
    return true;
}

// 2.8.3: what Setup does with a tool it did not install itself (PowerToys, Windhawk) that it finds on the PC.
// The person is asked: keep it (leave it exactly as it is and carry on) or replace it (back its data up, remove the old one, clean the leftovers, then install).
enum class ToolPlan { InstallNew, KeepExisting, ReplaceClean };
inline ToolPlan PlanToolInstall(bool alreadyInstalled, bool replaceRequested) {
    if (!alreadyInstalled) return ToolPlan::InstallNew;
    return replaceRequested ? ToolPlan::ReplaceClean : ToolPlan::KeepExisting;   // the default answer is "keep": nothing is removed unless the person said so
}
// 2.8.7: Windhawk's own installer puts a shortcut on the desktop. AnkhCore manages Windhawk itself, so only a shortcut file whose NAME starts with "Windhawk" is removed
// from the desktops after Setup installed it (never another file, never the Start menu entry).
inline bool IsWindhawkDesktopShortcut(const std::wstring& fileName) {
    const std::wstring l = Lower(fileName);
    return l.size() >= 12 && l.compare(0, 8, L"windhawk") == 0 && l.compare(l.size() - 4, 4, L".lnk") == 0;
}

}  // namespace ak
