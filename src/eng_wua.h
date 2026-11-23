// AnkhCore 2.9.5 (Tasks 4-7): Windows Update detection and selective download, as an engine with a provider seam.
//
//   * WuProvider is the only thing that talks to Windows (src/eng_wua_com.h: the Windows Update Agent API). Tests and the isolated TEST build use WuFakeProvider.
//   * The provider can search and download ONE update. It has no install, no restart, no policy and no service call: this feature cannot do those things.
//   * WuModel runs searches and downloads on its own worker thread. It does not use the application's single "busy" operation, so the window stays usable.
//     All state lives in one shared block that the worker holds by shared_ptr: a late callback can never touch a destroyed object.
//   * Pure helpers (error classification, KB extraction, sizes, policy text, state rules) are unit-tested without Windows.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ak {

// ---------------------------------------------------------------- states and errors
enum class WuState {
    Available,        // offered by Windows Update, not downloaded yet
    Downloaded,       // downloaded (by Windows or by AnkhCore); installing is Windows' job
    Installed,        // already installed
    Hidden,           // hidden: Windows is not offering it (hidden by someone, or no longer applicable)
    Queued,           // chosen, waiting for its turn in this download
    Downloading,
    Failed,           // the last download of this update failed
    Cancelled,        // the download was cancelled while it was running
};
inline const wchar_t* WuStateLabel(WuState s) {
    switch (s) {
    case WuState::Available: return L"Available";
    case WuState::Downloaded: return L"Downloaded";
    case WuState::Installed: return L"Installed";
    case WuState::Hidden: return L"Hidden / not offered";
    case WuState::Queued: return L"Queued";
    case WuState::Downloading: return L"Downloading";
    case WuState::Failed: return L"Download failed";
    case WuState::Cancelled: return L"Cancelled";
    }
    return L"";
}
// Only an update that still has to be downloaded can be chosen. A failed or cancelled one can be tried again.
inline bool WuSelectable(WuState s) { return s == WuState::Available || s == WuState::Failed || s == WuState::Cancelled; }
inline bool WuBusyState(WuState s) { return s == WuState::Queued || s == WuState::Downloading; }

enum class WuErr { None, Offline, Policy, AccessDenied, ServiceUnavailable, Cancelled, Api };
struct WuError { WuErr kind = WuErr::None; long hr = 0; std::wstring text; };

inline std::wstring WuHex(long hr) {
    wchar_t b[16]; swprintf_s(b, L"0x%08lX", static_cast<unsigned long>(hr)); return b;
}
// What a failed Windows Update Agent call means for the person. Codes from the Windows Update Agent result-code list; anything else is reported with its code, never guessed.
inline WuError WuClassifyHr(long hr) {
    WuError e; e.hr = hr;
    const unsigned long u = static_cast<unsigned long>(hr);
    switch (u) {
    case 0x80072EE2: case 0x80072EE7: case 0x80072EFD: case 0x80072EFE: case 0x80072F8F:
    case 0x8024402C: case 0x8024401C: case 0x80244010: case 0x80240438: case 0x80244022: case 0x80244016:
        e.kind = WuErr::Offline; e.text = L"Windows Update could not be reached (no internet, or the update server did not answer). " + WuHex(hr); break;
    case 0x80070005: case 0x80240044:
        e.kind = WuErr::AccessDenied; e.text = L"Windows refused access to Windows Update for this account. Run AnkhCore as administrator. " + WuHex(hr); break;
    case 0x80070422: case 0x80240001: case 0x800706BA: case 0x80240033: case 0x8024001E:
        e.kind = WuErr::ServiceUnavailable; e.text = L"The Windows Update service is not available (stopped, disabled or restarting). AnkhCore does not change that service. " + WuHex(hr); break;
    case 0x8024000B: case 0x80004004:
        e.kind = WuErr::Cancelled; e.text = L"The operation was cancelled. " + WuHex(hr); break;
    default:
        e.kind = WuErr::Api; e.text = L"Windows Update reported an error. " + WuHex(hr); break;
    }
    return e;
}

// ---------------------------------------------------------------- the update record and pure helpers
struct WuUpdate {
    std::wstring id;                 // the update's own identifier (a GUID), the key of everything below
    int revision = 0;
    std::wstring title, kb, description, category;
    uint64_t sizeBytes = 0; bool sizeKnown = false;
    WuState state = WuState::Available;
    int pct = 0;                     // download progress of this update
    std::wstring detail;             // why it is in this state (failure text, "not started: cancelled" ...)
    bool rebootAfterInstall = false;
};

// "KB5012345": from the KB list Windows gives, else from "(KB5012345)" in the title; empty when there is none.
inline std::wstring WuKbFrom(const std::vector<std::wstring>& kbIds, const std::wstring& title) {
    for (const auto& k : kbIds) {
        std::wstring digits; for (wchar_t ch : k) if (ch >= L'0' && ch <= L'9') digits += ch;
        if (digits.size() >= 5) return L"KB" + digits;
    }
    for (size_t i = 0; i + 4 < title.size(); ++i) {
        if ((title[i] == L'K' || title[i] == L'k') && (title[i + 1] == L'B' || title[i + 1] == L'b')) {
            size_t j = i + 2; std::wstring digits;
            while (j < title.size() && title[j] >= L'0' && title[j] <= L'9') digits += title[j++];
            if (digits.size() >= 5) return L"KB" + digits;
        }
    }
    return L"";
}
inline std::wstring WuSizeText(const WuUpdate& u) {
    if (!u.sizeKnown) return L"size unknown";
    const double mb = static_cast<double>(u.sizeBytes) / (1024.0 * 1024.0);
    wchar_t b[32];
    if (mb >= 1024.0) swprintf_s(b, L"%.2f GB", mb / 1024.0); else if (mb >= 1.0) swprintf_s(b, L"%.1f MB", mb); else swprintf_s(b, L"%.0f KB", static_cast<double>(u.sizeBytes) / 1024.0);
    return b;
}
// The state a search result is in, from the three facts Windows gives. Installed wins, then hidden, then downloaded.
inline WuState WuStateFromFlags(bool installed, bool hidden, bool downloaded) {
    if (installed) return WuState::Installed;
    if (hidden) return WuState::Hidden;
    return downloaded ? WuState::Downloaded : WuState::Available;
}
// Policy information only (read, never written): which policy values make Windows Update managed or restricted on this PC.
struct WuPolicy { bool managedServer = false, accessDisabled = false, autoUpdatesOff = false; std::wstring server; };
inline WuPolicy WuPolicyFrom(const std::function<long long(const wchar_t* sub, const wchar_t* value)>& num, const std::function<std::wstring(const wchar_t* value)>& str) {
    WuPolicy p;
    p.server = str(L"WUServer");
    p.managedServer = !p.server.empty() && num(L"AU", L"UseWUServer") == 1;
    p.accessDisabled = num(L"", L"DisableWindowsUpdateAccess") == 1;
    p.autoUpdatesOff = num(L"AU", L"NoAutoUpdate") == 1;
    return p;
}
inline std::wstring WuPolicyText(const WuPolicy& p) {
    std::wstring t;
    if (p.accessDisabled) t += L"A policy disables access to Windows Update. ";
    if (p.managedServer) t += L"Updates come from your organisation's server (" + p.server + L"). ";
    if (p.autoUpdatesOff) t += L"A policy turns automatic updates off. ";
    return t;
}

// ---------------------------------------------------------------- the provider seam
struct WuSearchResult { bool ok = false; WuError err; std::vector<WuUpdate> updates; std::wstring note; };
enum class WuDlKind { Done, Failed, Aborted };
struct WuDlOutcome { WuDlKind kind = WuDlKind::Failed; long hr = 0; bool verifiedDownloaded = false; std::wstring text; };
struct WuProvider {
    virtual ~WuProvider() = default;
    // Lists updates: available (with their downloaded flag), hidden, and a bounded number of installed ones. Read-only.
    virtual WuSearchResult Search() = 0;
    // Downloads exactly this one update. progress(0..100) is called while it runs. cancel() is polled: when it turns true the provider asks Windows to abort and returns
    // Aborted once Windows has stopped. verifiedDownloaded is read back from Windows after a "success", never assumed.
    virtual WuDlOutcome DownloadOne(const WuUpdate& u, const std::function<void(int)>& progress, const std::function<bool()>& cancel) = 0;
};

// ---------------------------------------------------------------- the model: state, worker, lifecycle
struct WuSnapshot {
    bool detecting = false, detected = false, downloading = false, cancelRequested = false, cancelCompleted = false;
    WuError error; std::wstring policyText, status, lastReport;
    std::vector<WuUpdate> updates;
    std::set<std::wstring> selected;
    int total = 0, done = 0, failed = 0, cancelledN = 0, notStarted = 0;   // of the running or last download
    int currentIndex = 0, overallPct = 0; std::wstring currentTitle;
    int installedShown = 0;
};
struct WuStartResult { bool started = false; std::vector<std::wstring> accepted, duplicates, notSelectable; std::wstring why; };

class WuModel {
public:
    explicit WuModel(std::shared_ptr<WuProvider> prov, std::function<void()> changed = {}, std::wstring policyText = {}) : d_(std::make_shared<Impl>()) {
        d_->prov = std::move(prov); d_->changed = std::move(changed); d_->s.policyText = std::move(policyText);
    }
    ~WuModel() { Shutdown(); }
    WuModel(const WuModel&) = delete; WuModel& operator=(const WuModel&) = delete;

    WuSnapshot Snapshot() const { std::lock_guard<std::mutex> g(d_->mu); return d_->s; }
    bool DownloadActive() const { std::lock_guard<std::mutex> g(d_->mu); return d_->s.downloading; }

    // Starts a read-only search on the worker. False when a search or a download is already running.
    bool Detect() {
        std::lock_guard<std::mutex> og(opMu_);
        { std::lock_guard<std::mutex> g(d_->mu); if (d_->running) return false; d_->running = true; d_->s.detecting = true; d_->s.status = L"Checking Windows Update\x2026"; }
        JoinFinished();
        auto d = d_;
        th_ = std::thread([d] { SearchWorker(d); });
        Notify(d_);
        return true;
    }
    void Toggle(const std::wstring& id) {
        { std::lock_guard<std::mutex> g(d_->mu);
          if (d_->s.downloading) return;                                     // the choice is fixed while a download runs
          const WuUpdate* u = Find(d_->s, id); if (!u || !WuSelectable(u->state)) return;
          if (!d_->s.selected.erase(id)) d_->s.selected.insert(id); }
        Notify(d_);
    }
    void SelectAll(bool on) {
        { std::lock_guard<std::mutex> g(d_->mu);
          if (d_->s.downloading) return;
          d_->s.selected.clear();
          if (on) for (const auto& u : d_->s.updates) if (WuSelectable(u.state)) d_->s.selected.insert(u.id); }
        Notify(d_);
    }
    // Downloads the chosen updates, one after another. Only ids in `ids` that are selectable and not already queued or running are accepted.
    WuStartResult StartDownload(const std::vector<std::wstring>& ids) {
        WuStartResult r;
        std::lock_guard<std::mutex> og(opMu_);
        std::vector<std::wstring> queue;
        {
            std::lock_guard<std::mutex> g(d_->mu);
            if (d_->running) { r.why = d_->s.downloading ? L"A download is already running." : L"A check is running; try again when it finishes."; return r; }
            std::set<std::wstring> seen;
            for (const auto& id : ids) {
                if (!seen.insert(id).second) { r.duplicates.push_back(id); continue; }
                const WuUpdate* u = Find(d_->s, id);
                if (!u || !WuSelectable(u->state)) { (u && WuBusyState(u->state) ? r.duplicates : r.notSelectable).push_back(id); continue; }
                queue.push_back(id);
            }
            if (queue.empty()) { r.why = L"Nothing to download: choose an update that is not downloaded yet."; return r; }
            d_->running = true; d_->cancel = false;
            WuSnapshot& s = d_->s;
            s.downloading = true; s.cancelRequested = false; s.cancelCompleted = false; s.total = static_cast<int>(queue.size()); s.done = s.failed = s.cancelledN = s.notStarted = 0;
            s.currentIndex = 0; s.overallPct = 0; s.currentTitle.clear(); s.lastReport.clear();
            for (const auto& id : queue) if (WuUpdate* u = FindMut(s, id)) { u->state = WuState::Queued; u->pct = 0; u->detail.clear(); }
            s.status = L"Downloading " + std::to_wstring(queue.size()) + L" update(s)\x2026";
            r.accepted = queue;
        }
        JoinFinished();
        auto d = d_;
        th_ = std::thread([d, queue] { DownloadWorker(d, queue); });
        r.started = true;
        Notify(d_);
        return r;
    }
    // The person asked to stop. cancelRequested turns true now; cancelCompleted only when Windows has stopped the running download and the worker has finished.
    bool RequestCancel() {
        { std::lock_guard<std::mutex> g(d_->mu);
          if (!d_->s.downloading || d_->s.cancelRequested) return false;
          d_->s.cancelRequested = true; d_->cancel = true;
          d_->s.status = L"Cancel requested\x2026 asking Windows to stop the running download."; }
        Notify(d_);
        return true;
    }
    // Application exit: stop scheduling, ask Windows to abort, wait a little for the worker, then let go. The worker only owns shared state, so letting go is safe.
    void Shutdown(unsigned waitMs = 4000) {
        { std::lock_guard<std::mutex> g(d_->mu); d_->cancel = true; d_->shutdown = true; d_->changed = nullptr; }
        std::unique_lock<std::mutex> lk(d_->mu);
        d_->cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] { return !d_->running; });
        const bool finished = !d_->running; lk.unlock();
        std::lock_guard<std::mutex> og(opMu_);
        if (th_.joinable()) { if (finished) th_.join(); else th_.detach(); }
    }

private:
    struct Impl {
        std::mutex mu; std::condition_variable cv;
        WuSnapshot s; std::shared_ptr<WuProvider> prov; std::function<void()> changed;
        bool running = false, shutdown = false;
        std::atomic<bool> cancel{false};
    };
    std::shared_ptr<Impl> d_;
    std::mutex opMu_; std::thread th_;

    void JoinFinished() { if (th_.joinable()) th_.join(); }   // only called when no worker is running, so it returns at once
    static void Notify(const std::shared_ptr<Impl>& d) {
        std::function<void()> f; { std::lock_guard<std::mutex> g(d->mu); f = d->changed; }
        if (f) f();
    }
    static const WuUpdate* Find(const WuSnapshot& s, const std::wstring& id) { for (const auto& u : s.updates) if (u.id == id) return &u; return nullptr; }
    static WuUpdate* FindMut(WuSnapshot& s, const std::wstring& id) { for (auto& u : s.updates) if (u.id == id) return &u; return nullptr; }

    static void SearchWorker(std::shared_ptr<Impl> d) {
        WuSearchResult r;
        try { r = d->prov->Search(); } catch (...) { r.ok = false; r.err = WuClassifyHr(static_cast<long>(0x80004005)); }
        {
            std::lock_guard<std::mutex> g(d->mu);
            WuSnapshot& s = d->s;
            s.detecting = false; s.detected = r.ok; s.error = r.ok ? WuError{} : r.err;
            if (r.ok) {
                std::set<std::wstring> keep = s.selected; s.selected.clear();
                s.updates = std::move(r.updates);
                int inst = 0; for (const auto& u : s.updates) { if (u.state == WuState::Installed) ++inst; if (keep.count(u.id) && WuSelectable(u.state)) s.selected.insert(u.id); }
                s.installedShown = inst;
                int avail = 0, down = 0; for (const auto& u : s.updates) { if (u.state == WuState::Available) ++avail; else if (u.state == WuState::Downloaded) ++down; }
                s.status = avail || down ? std::to_wstring(avail) + L" available, " + std::to_wstring(down) + L" already downloaded." : L"Windows reports no updates waiting.";
                if (!r.note.empty()) s.status += L" " + r.note;
            } else {
                s.status = s.error.text.empty() ? L"The check did not complete." : s.error.text;
            }
            d->running = false;
        }
        d->cv.notify_all(); Notify(d);
    }

    static void DownloadWorker(std::shared_ptr<Impl> d, std::vector<std::wstring> queue) {
        const int total = static_cast<int>(queue.size());
        for (int i = 0; i < total; ++i) {
            const std::wstring id = queue[i];
            WuUpdate cur;
            {   // before each update: a cancel means this one and every later one never start
                std::lock_guard<std::mutex> g(d->mu);
                if (d->cancel.load()) {
                    for (int j = i; j < total; ++j) if (WuUpdate* u = FindMut(d->s, queue[j])) { u->state = WuState::Available; u->detail = L"Not started: the download was cancelled."; ++d->s.notStarted; }
                    break;
                }
                WuUpdate* u = FindMut(d->s, id); if (!u) continue;
                u->state = WuState::Downloading; u->pct = 0; cur = *u;
                d->s.currentIndex = i + 1; d->s.currentTitle = u->title;
                d->s.overallPct = i * 100 / total;
                d->s.status = L"Downloading " + std::to_wstring(i + 1) + L" of " + std::to_wstring(total) + L": " + u->title;
            }
            Notify(d);
            WuDlOutcome o;
            try {
                o = d->prov->DownloadOne(cur,
                    [d, id, i, total](int pct) {
                        { std::lock_guard<std::mutex> g(d->mu); if (WuUpdate* u = FindMut(d->s, id)) u->pct = std::clamp(pct, 0, 100); d->s.overallPct = (i * 100 + std::clamp(pct, 0, 100)) / total; }
                        Notify(d);
                    },
                    [d] { return d->cancel.load(); });
            } catch (...) { o.kind = WuDlKind::Failed; o.hr = static_cast<long>(0x80004005); o.text = L"The download stopped unexpectedly."; }
            {
                std::lock_guard<std::mutex> g(d->mu);
                WuUpdate* u = FindMut(d->s, id);
                if (u) {
                    if (o.kind == WuDlKind::Done && o.verifiedDownloaded) { u->state = WuState::Downloaded; u->pct = 100; u->detail = L"Downloaded and verified. Windows installs it."; ++d->s.done; }
                    else if (o.kind == WuDlKind::Aborted) { u->state = WuState::Cancelled; u->detail = L"Cancelled while downloading."; ++d->s.cancelledN; }
                    else {
                        u->state = WuState::Failed; ++d->s.failed;
                        u->detail = o.kind == WuDlKind::Done ? L"Windows reported success but the update is not marked as downloaded." : (!o.text.empty() ? o.text : WuClassifyHr(o.hr).text);
                    }
                }
                d->s.overallPct = (i + 1) * 100 / total;
            }
            Notify(d);
        }
        {
            std::lock_guard<std::mutex> g(d->mu);
            WuSnapshot& s = d->s;
            std::wstring rep = std::to_wstring(s.done) + L" of " + std::to_wstring(s.total) + L" downloaded";
            if (s.failed) rep += L", " + std::to_wstring(s.failed) + L" failed";
            if (s.cancelledN) rep += L", " + std::to_wstring(s.cancelledN) + L" cancelled while running";
            if (s.notStarted) rep += L", " + std::to_wstring(s.notStarted) + L" not started";
            rep += s.cancelRequested ? L". Cancelled: what finished before is kept." : L".";
            s.lastReport = rep; s.status = rep;
            if (s.cancelRequested) s.cancelCompleted = true;     // Windows has stopped and nothing more will start
            s.downloading = false; s.currentTitle.clear(); s.overallPct = (s.done + s.failed + s.cancelledN + s.notStarted) * 100 / std::max(1, s.total);
            for (auto& u : s.updates) if (u.state == WuState::Queued) u.state = WuState::Available;   // never leaves a "queued" behind
            s.selected.clear();
            d->running = false;
        }
        d->cv.notify_all(); Notify(d);
    }
};

// ---------------------------------------------------------------- test and TEST-build provider
struct WuFakeProvider : WuProvider {
    WuSearchResult search;
    std::function<WuDlOutcome(const WuUpdate&, const std::function<void(int)>&, const std::function<bool()>&)> download;
    std::mutex mu; std::vector<std::wstring> downloaded;   // ids that DownloadOne was called for, in order
    int searches = 0;
    WuSearchResult Search() override { std::lock_guard<std::mutex> g(mu); ++searches; return search; }
    WuDlOutcome DownloadOne(const WuUpdate& u, const std::function<void(int)>& progress, const std::function<bool()>& cancel) override {
        { std::lock_guard<std::mutex> g(mu); downloaded.push_back(u.id); }
        if (download) return download(u, progress, cancel);
        progress(100); WuDlOutcome o; o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o;
    }
};

}  // namespace ak
