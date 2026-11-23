// AnkhCore 2.7: the UniGetUI library that ships INSIDE AnkhCore (installed by AnkhCore Setup into <program folder>\UniGetUI).
//
// AnkhCore reads the installed programs, finds updates and updates them with this bundled copy: nobody installs or configures anything.
// The program folder is not writable for a normal user, and a portable UniGetUI keeps its data beside its own files, so each user gets a
// private working copy under the AnkhCore data folder (DataDir()\UniGetUI). It is made once, in the background, on first use and again
// only when the bundled copy changes (a stamp file records which bundled copy it was made from). The user's own installed UniGetUI is never
// touched or used. Pure file operations on injected paths so they can be tested in a folder.
#pragma once
#include "base.h"
#include <atomic>
#include <functional>
#include <thread>

namespace ak {

constexpr wchar_t kUgStampName[] = L".ankhcore-copy";

inline std::wstring UgBundledSource() { return AppDir() + L"\\UniGetUI"; }
inline std::wstring UgUserCopy() { return DataDir() + L"\\UniGetUI"; }

inline ULONGLONG UgFileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ? (static_cast<ULONGLONG>(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
}
inline ULONGLONG UgFileTime(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ? (static_cast<ULONGLONG>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime : 0;
}

// A bundled copy is complete only with both programs and the marker file that makes UniGetUI portable.
inline bool UgSourceComplete(const std::wstring& src) {
    return FileExists(src + L"\\UniGetUI.exe") && FileExists(src + L"\\uniget.exe") && FileExists(src + L"\\ForceUniGetUIPortable");
}
// What identifies the bundled copy a working copy was made from.
inline std::string UgStampFor(const std::wstring& src) {
    return "ankhcore-unigetui-copy v1 " + std::to_string(UgFileSize(src + L"\\UniGetUI.exe")) + " " + std::to_string(UgFileSize(src + L"\\uniget.exe")) +
           " " + std::to_string(UgFileTime(src + L"\\UniGetUI.exe")) + " " + std::to_string(UgFileTime(src + L"\\uniget.exe"));
}
// The working copy is ready when it was completed from this very bundled copy.
inline bool UgCopyReady(const std::wstring& src, const std::wstring& dst) {
    if (!UgSourceComplete(src) || !UgSourceComplete(dst)) return false;
    return ReadText(dst + L"\\" + kUgStampName) == UgStampFor(src);
}

namespace ugdetail {
inline bool CopyTree(const std::wstring& from, const std::wstring& to, const std::atomic<bool>* cancel, int& files) {
    EnsureDir(to);
    for (const auto& d : ListDir(from, true)) {
        if (cancel && cancel->load()) return false;
        if (!CopyTree(from + L"\\" + d, to + L"\\" + d, cancel, files)) return false;
    }
    for (const auto& f : ListDir(from, false)) {
        if (cancel && cancel->load()) return false;
        const std::wstring s = from + L"\\" + f, t = to + L"\\" + f;
        if (UgFileSize(s) == UgFileSize(t) && FileExists(t) && UgFileTime(s) <= UgFileTime(t)) continue;   // already the same file
        SetFileAttributesW(t.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(s.c_str(), t.c_str(), FALSE)) return false;
        SetFileAttributesW(t.c_str(), FILE_ATTRIBUTE_NORMAL);   // a copy from Program Files must stay writable for the user
        ++files;
    }
    return true;
}
}  // namespace ugdetail

// Makes (or refreshes) the working copy. Writes the stamp last, so a copy that was interrupted is never treated as ready.
// Returns true when the copy is ready on return. The user's UniGetUI data inside the working copy is kept.
inline bool UgPrepareCopy(const std::wstring& src, const std::wstring& dst, const std::atomic<bool>* cancel = nullptr, std::wstring* why = nullptr) {
    if (!UgSourceComplete(src)) { if (why) *why = L"The UniGetUI that comes with AnkhCore is missing or incomplete (reinstall AnkhCore with AnkhCore Setup)."; return false; }
    if (UgCopyReady(src, dst)) return true;
    DeleteFileW((dst + L"\\" + kUgStampName).c_str());
    int files = 0;
    if (!ugdetail::CopyTree(src, dst, cancel, files)) { if (why) *why = L"Preparing UniGetUI was interrupted or a file could not be copied."; return false; }
    const std::string st = UgStampFor(src);
    if (!WriteAll(dst + L"\\" + kUgStampName, reinterpret_cast<const uint8_t*>(st.data()), st.size())) { if (why) *why = L"Preparing UniGetUI could not write its stamp."; return false; }
    return UgCopyReady(src, dst);
}

// Background preparation for the application: one worker at a time, never blocks the UI.
struct UgPrep {
    std::atomic<int> state{0};   // 0 idle, 1 running, 2 ready, 3 failed
    std::wstring why;            // set before state becomes 3
    std::atomic<bool> cancel{false};
};
inline UgPrep& UgPrepState() { static UgPrep p; return p; }
inline bool UgBundledAvailable() { return !kTestBuild && UgSourceComplete(UgBundledSource()); }
// Starts the preparation if the bundled copy exists and the working copy is not ready; returns the state afterwards.
inline int UgEnsurePrepared(const std::function<void()>& notify) {
    UgPrep& p = UgPrepState();
    if (!UgBundledAvailable()) return p.state.load();
    if (p.state.load() == 1) return 1;
    if (UgCopyReady(UgBundledSource(), UgUserCopy())) { p.state = 2; return 2; }
    if (p.state.load() == 3) return 3;   // failed once: not retried until the application restarts (no loop of copies)
    int expected = 0;
    if (!p.state.compare_exchange_strong(expected, 1)) return p.state.load();
    std::thread([notify] {
        UgPrep& q = UgPrepState();
        std::wstring why;
        const bool ok = UgPrepareCopy(UgBundledSource(), UgUserCopy(), &q.cancel, &why);
        q.why = why;
        q.state = ok ? 2 : 3;
        if (notify) notify();
    }).detach();
    return 1;
}

}  // namespace ak
