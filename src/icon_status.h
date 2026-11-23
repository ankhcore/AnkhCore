// FT-2 / A04: the installed state of the icon themes, cached.
// Before: the Icon Manager called IsIconThemeInstalled (ValidateIconThemeDir) for every card on every paint, and that reads
// every resource module of the theme completely into memory and parses it: up to 646 MB of file reads on the UI thread.
// Now: the page only reads this cache. It is filled on a worker thread with a cheap check (theme.ini parses, every module
// it names exists, is not empty and starts with "MZ"). The full PE validation stays where a theme is applied (Apply) and
// where setup copies it, because a wrong theme must never be applied.
#pragma once
#include "eng_icons.h"
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

namespace ak {

struct IconThemeStatus {
    bool checked = false;        // false: not looked at yet (the card shows "checking")
    bool installed = false;
    std::wstring dir;            // the theme folder
    std::wstring problem;        // why it is not installed, in words
    int modules = 0;             // modules named by theme.ini
    std::wstring preview;        // path of the theme's own preview picture (FT-3 uses it); empty when it has none
};

inline std::wstring FindIconThemePreviewFile(const std::wstring& themeDir) {
    if (!DirExists(themeDir)) return L"";
    for (const wchar_t* n : {L"preview.png", L"preview.jpg", L"preview.jpeg", L"preview.webp", L"preview.bmp"})
        if (FileExists(themeDir + L"\\" + n)) return themeDir + L"\\" + n;
    return L"";
}

// Cheap check: no module is read beyond its first two bytes.
inline IconThemeStatus IconThemeQuickCheck(const std::wstring& themeDir) {
    IconThemeStatus s; s.checked = true; s.dir = themeDir;
    if (!DirExists(themeDir)) { s.problem = L"not installed"; return s; }
    s.preview = FindIconThemePreviewFile(themeDir);
    std::vector<std::pair<std::wstring, std::wstring>> pairs; std::wstring err;
    if (!ParseThemeIniRedirections(themeDir + L"\\theme.ini", pairs, err)) { s.problem = err; return s; }
    s.modules = static_cast<int>(pairs.size());
    for (const auto& kv : pairs) {
        const std::wstring p = themeDir + L"\\" + kv.second;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (a.nFileSizeHigh == 0 && a.nFileSizeLow < 64)) { s.problem = L"missing or empty module " + kv.second; return s; }
        HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        char mz[2] = {}; DWORD got = 0;
        const bool ok = h != INVALID_HANDLE_VALUE && ReadFile(h, mz, 2, &got, nullptr) && got == 2 && mz[0] == 'M' && mz[1] == 'Z';
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!ok) { s.problem = L"module " + kv.second + L" is not a program file"; return s; }
    }
    s.installed = true;
    return s;
}

// FT-A02: what a theme card's own Apply button and note say. Pure: the page only draws it.
struct IconCardInput {
    bool checked = false, installed = false, windhawk = false;   // the cached state of this theme, and whether Windhawk is installed
    bool active = false, restartPending = false;                  // this theme is the active one; a 2.2 file swap still waits for a restart
    bool busy = false, applyingHere = false;                      // an operation runs; it is this card's Apply
    int lastResult = 0;                                           // the last Apply from this card: 0 none, 1 applied, 2 failed
};
struct IconCardView {
    std::wstring label, note;
    bool enabled = false, primary = false;
    int noteKind = 0;                                              // 0 none, 1 good, 2 error, 3 faint
};
inline IconCardView IconCardState(const IconCardInput& i) {
    IconCardView v;
    if (i.applyingHere) { v.label = L"Applying\u2026"; return v; }
    if (i.active) { v.label = i.restartPending ? L"Restart needed" : L"Applied"; return v; }
    v.label = L"Apply"; v.primary = true;
    v.enabled = !i.busy && i.checked && i.installed && i.windhawk;
    if (!i.busy && i.lastResult == 2) { v.note = L"Failed: details are in Activity."; v.noteKind = 2; }
    else if (!v.enabled) {
        v.note = !i.windhawk ? L"Windhawk is not installed" : !i.checked ? L"Checking\u2026" : !i.installed ? L"Theme not installed" : L"Busy\u2026"; v.noteKind = 3;
    }
    return v;
}

// id -> status for every theme the program knows. Read by the UI thread, written by a worker.
class IconStatusCache {
public:
    // Marks the cache stale and refreshes it on a worker thread (once at a time); `notify` is called from that thread when done.
    void RefreshAsync(std::function<void()> notify = nullptr) {
        again_ = true;                      // the request is recorded BEFORE the busy test: a worker that is finishing re-checks it after clearing busy_
        if (busy_.exchange(true)) return;   // a worker is running (or finishing): it will see the request and refresh once more
        if (worker_.joinable()) worker_.join();
        ++workersStarted;
        worker_ = std::thread([this, notify] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;) {
                // a refresh that throws (out of memory, a folder that vanished mid-scan) must not kill the thread or leave the cache busy for ever:
                // the old map stays, and the next request starts again
                while (again_.exchange(false)) { try { if (work) work(); else RefreshNow(); } catch (...) {} if (notify) { try { notify(); } catch (...) {} } }
                if (beforeIdle) beforeIdle();   // test seam: the instant between "no more requests" and "not busy"
                busy_ = false;
                if (!again_.load() || busy_.exchange(true)) break;   // nothing arrived while finishing, or a new worker has taken over
            }
            CoUninitialize();
        });
    }
    // Test seams (empty in the program): `work` replaces RefreshNow, `beforeIdle` runs when a worker is about to go idle; `workersStarted` counts threads.
    std::function<void()> work, beforeIdle;
    std::atomic<int> workersStarted{0};
    void RefreshNow() {
        std::map<std::wstring, IconThemeStatus> m;
        for (const auto& p : IconPacks()) {
            IconThemeStatus st = IconThemeQuickCheck(IconThemeDir(p.id));
            if (st.preview.empty()) {
                wchar_t self[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0)
                    st.preview = FindIconThemePreviewFile(DirOf(self) + L"\\IconThemes\\" + IconThemeFolderName(p.id));
            }
            m[p.id] = std::move(st);
        }
        std::lock_guard<std::mutex> lk(mx_); map_ = std::move(m); ++generation_;
    }
    IconThemeStatus Get(const std::wstring& id) const {
        std::lock_guard<std::mutex> lk(mx_);
        const auto it = map_.find(id);
        return it == map_.end() ? IconThemeStatus{} : it->second;
    }
    bool Loaded() const { std::lock_guard<std::mutex> lk(mx_); return generation_ > 0; }
    uint64_t Generation() const { std::lock_guard<std::mutex> lk(mx_); return generation_; }
    void Join() { if (worker_.joinable()) worker_.join(); }
    ~IconStatusCache() { Join(); }
private:
    mutable std::mutex mx_;
    std::map<std::wstring, IconThemeStatus> map_;
    uint64_t generation_ = 0;
    std::atomic<bool> busy_{false}, again_{false};
    std::thread worker_;
};

}  // namespace ak
