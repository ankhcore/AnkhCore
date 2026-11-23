// eng_updui.h - UPD2-B: the model behind the updater section of Recovery > Backup & Migration.
// No drawing and no Windows messages here: the page (app.cpp) only reads Snapshot() and calls Check()/Run() from a worker thread
// (RunOp). Everything that decides what the user is told lives here so that it can be tested with a mock UniGetUI.
//   * the UniGetUI session is created and started only when Check()/Run() is called (never by opening the page);
//   * one operation at a time: a second call while one is running is refused, it never starts a second worker's worth of commands;
//   * after Shutdown() nothing is started again (no orphan session after the window is destroyed);
//   * every command goes through UgSession (private pipe, validated before each update).
#pragma once
#include "eng_updater.h"
#include "eng_ugsession.h"
#include <atomic>
#include <memory>
#include <set>

namespace ak {

// What the user is told about one finished update run. One line, honest about every outcome.
inline std::wstring UpdUiReportText(const UpdReport& rep) {
    std::wstring t = std::wstring(UpdOutcomeLabel(rep.outcome)) + L": " + rep.headline;
    if (!rep.versionBefore.empty() || !rep.versionAfter.empty())
        t += L"  (" + Utf8ToW(rep.versionBefore) + L" -> " + (rep.versionAfterConfirmed ? Utf8ToW(rep.versionAfter) : std::wstring(L"not confirmed")) + L")";
    if (rep.rollbackResult != UpdRollbackResult::NotNeeded) t += L"  Rollback: " + std::wstring(UpdRollbackResultLabel(rep.rollbackResult)) + L".";
    if (!rep.recoveryDir.empty()) t += L"  Recovery point: " + rep.recoveryDir;
    return t;
}
inline bool UpdUiIsSuccess(const UpdReport& rep) { return rep.outcome == UpdOutcome::Succeeded; }

// UniGetUI lists a source as "<Manager>: <name>" (for example "WinGet: winget"); `--source` wants only the name. A source that is not a
// plain safe token after that is dropped (the update then runs without --source) instead of being passed on or failing validation.
inline std::string UpdUiSourceName(const std::string& listed) {
    std::string n = listed;
    const size_t k = n.find(": ");
    if (k != std::string::npos) n = n.substr(k + 2);
    return UpdSafeToken(n, false) ? n : std::string();
}

// The confirmation question: the package, the installed version and the target version exactly as UniGetUI listed them.
inline std::wstring UpdUiConfirmText(const UpdListedPackage& lp) {
    const std::wstring name = Utf8ToW(lp.name.empty() ? lp.id : lp.name);
    return L"Update " + name + L" from " + Utf8ToW(lp.version) + L" to " + Utf8ToW(lp.newVersion) +
           L"?\n\nA verified recovery point is created first. The new version is checked afterwards; AnkhCore never reports success it could not confirm. "
           L"Application data is not backed up, so a rollback would not restore it.";
}

// 2.8.6: some programs are listed with the installed version "Unknown" (installed outside winget: AnyDesk, Filmora, DiskGenius...). The updater cannot compare or roll back
// for them, so "Update all" leaves them out and each one has its own "Update anyway" (with the limits said first). Without this they all aborted with
// "The installed version could not be confirmed".
inline bool UpdListedVersionUnknown(const UpdListedPackage& p) { std::vector<unsigned long long> parts; return !UpdParseVersion(p.version, parts); }
inline std::vector<UpdListedPackage> UpdOnlyKnownVersions(const std::vector<UpdListedPackage>& in, size_t* unknownCount = nullptr) {
    std::vector<UpdListedPackage> out; size_t u = 0;
    for (const auto& p : in) { if (UpdListedVersionUnknown(p)) ++u; else out.push_back(p); }
    if (unknownCount) *unknownCount = u;
    return out;
}
inline std::wstring UpdUiConfirmUnknownText(const UpdListedPackage& lp) {
    const std::wstring name = Utf8ToW(lp.name.empty() ? lp.id : lp.name);
    return L"Update " + name + L" to " + Utf8ToW(lp.newVersion) + L" anyway?\n\nThe installed version of this program is listed as \"" + Utf8ToW(lp.version.empty() ? std::string("Unknown") : lp.version) +
           L"\" (it was installed outside winget), so AnkhCore cannot compare it with the new one and cannot roll it back to the old one. A recovery point of AnkhCore's own state is still made first, "
           L"and the version after the update is read and shown. Application data is not backed up.";
}

// ---- UPD2-C: the fuller result view and the unfinished-update state
struct UpdUiLine { std::wstring title, detail; int state = 0; };   // state: 0 information, 1 ok, 2 problem

inline std::vector<UpdUiLine> UpdUiReportLines(const UpdReport& rep) {
    std::vector<UpdUiLine> v;
    v.push_back({L"Result", UpdUiReportText(rep), UpdUiIsSuccess(rep) ? 1 : 2});
    if (!rep.versionBefore.empty() || !rep.versionAfter.empty())
        v.push_back({L"Installed version", Utf8ToW(rep.versionBefore) + L" -> " + (rep.versionAfterConfirmed ? Utf8ToW(rep.versionAfter) : std::wstring(L"not confirmed")),
                     (rep.versionAfterConfirmed && rep.verdict.versionChanged) ? 1 : 2});   // an unchanged version is not ok, even when confirmed
    if (rep.backupRequired) {
        const bool all = rep.backupCreated && rep.backupVerified && rep.backupSealed;
        v.push_back({L"Recovery point", std::wstring(rep.backupCreated ? L"created" : L"not created") + L", " + (rep.backupVerified ? L"verified" : L"not verified") + L", " + (rep.backupSealed ? L"sealed" : L"not sealed") +
                                         (rep.recoveryDir.empty() ? std::wstring() : L"  \x00B7  " + rep.recoveryDir), all ? 1 : 2});
    } else v.push_back({L"Recovery point", L"not required by the backup policy for this update", 0});
    v.push_back({L"Application data", rep.appDataBackedUp ? L"backed up and verified" : L"not backed up (a rollback cannot restore it)", rep.appDataBackedUp ? 1 : 0});
    v.push_back({L"Rollback support", std::wstring(UpdRollbackCapabilityLabel(rep.rollback.capability)) + (rep.rollback.summary.empty() ? std::wstring() : L": " + rep.rollback.summary), 0});
    if (rep.rollbackResult != UpdRollbackResult::NotNeeded)
        v.push_back({L"Rollback", std::wstring(UpdRollbackResultLabel(rep.rollbackResult)) + L"  \x00B7  previous version " + (rep.binaryRestored ? L"restored" : L"not restored") + L", data " + (rep.dataRestored ? L"restored" : L"not restored"),
                     rep.rollbackResult == UpdRollbackResult::Complete ? 1 : 2});
    for (const UpdStepRecord& st : rep.steps) v.push_back({st.step, st.detail, st.ok ? 1 : 2});
    return v;
}

// What an update that did not finish leaves on disk (the marker <data>\Updater\active-update.json).
struct UpdUiPending {
    bool present = false, readable = true, unresolved = false;
    std::wstring text;
};
inline UpdUiPending UpdUiReadPending(const AcbRoots& roots) {
    UpdUiPending p;
    std::string t;
    const UpdReadStatus st = UpdReadMarker(UpdActivePath(roots), t);
    if (st == UpdReadStatus::Missing) return p;
    p.present = true;
    UpdActiveInfo a;
    if (st == UpdReadStatus::Locked) { p.text = L"An update record is open right now; wait a moment."; return p; }
    if (st == UpdReadStatus::Empty || !UpdParseActive(t, a)) { p.readable = false; p.text = L"An update record exists but cannot be read. Releasing it keeps a copy in the update history."; return p; }
    p.unresolved = a.unresolved;
    std::string phaseText = a.phase; for (char& ch : phaseText) if (ch == '-') ch = ' ';
    const std::wstring who = Utf8ToW(a.package) + (a.previousVersion.empty() ? std::wstring() : L" (installed " + Utf8ToW(a.previousVersion) + L")");
    p.text = a.unresolved ? L"The update of " + who + L" timed out and its final state is unknown. Starting another update is blocked until this is resolved."
                          : L"The update of " + who + L" did not finish (stopped at: " + Utf8ToW(phaseText) + L"). Starting another update is blocked until this is released.";
    if (!a.recoveryDir.empty()) p.text += L"  Recovery point: " + Utf8ToW(a.recoveryDir);
    return p;
}
inline const wchar_t* UpdUiAckText(UpdAckResult r, bool forced) {
    switch (r) {
    case UpdAckResult::Acknowledged: return forced ? L"Released without checking. The record was moved to the update history; the recovery point was left as it is."
                                                   : L"Released. The record was moved to the update history; the recovery point was left as it is.";
    case UpdAckResult::NotFound: return L"There is no unfinished update.";
    case UpdAckResult::OwnerAlive: return L"Not released: the process that started the update is still running.";
    case UpdAckResult::Unresolved: return L"Not released: AnkhCore could not check whether the operation is still running (UniGetUI did not answer). Use \"Release without checking\" only if you have verified it yourself.";
    case UpdAckResult::StillMaybeRunning: return L"Not released: UniGetUI still lists an operation for this package. Wait for it to finish, then try again.";
    default: return L"Not released: the record could not be moved.";
    }
}

// ---- 2.8.3: Skip (always / this version only) and Cancel (between packages)
// "Always skip": the package id is kept in a persistent list and never offered again until the person restores it.
// "Skip this version": only the exact (id, offered version) pair is kept; the next version of the same package appears again by itself.
// Ids are UpdSafeToken text (no ';' and no '='), so ';' separates entries and '=' separates id and version in the stored text.
struct UpdSkipSet {
    std::set<std::string> always;      // package ids
    std::set<std::string> versions;    // "id=offeredVersion"
};
inline std::set<std::string> UpdParseSkipText(const std::string& text) {
    std::set<std::string> out; std::string cur;
    auto flush = [&] { if (!cur.empty() && cur.size() <= 420) out.insert(cur); cur.clear(); };
    for (const char ch : text) { if (ch == ';') flush(); else if (ch != '\r' && ch != '\n') cur += ch; }
    flush();
    return out;
}
inline std::string UpdSerializeSkipText(const std::set<std::string>& v) { std::string t; for (const auto& e : v) { if (!t.empty()) t += ';'; t += e; } return t; }
inline std::string UpdSkipVersionKey(const std::string& id, const std::string& offered) { return id + "=" + offered; }
inline bool UpdIsSkipped(const UpdSkipSet& s, const UpdListedPackage& p) {
    return s.always.count(p.id) != 0 || s.versions.count(UpdSkipVersionKey(p.id, p.newVersion)) != 0;
}
// The list the person sees: nothing skipped in it. The counts say how many were hidden and why.
inline std::vector<UpdListedPackage> UpdFilterSkipped(const std::vector<UpdListedPackage>& in, const UpdSkipSet& s, size_t* hiddenAlways = nullptr, size_t* hiddenVersion = nullptr) {
    std::vector<UpdListedPackage> out; size_t ha = 0, hv = 0;
    for (const UpdListedPackage& p : in) {
        if (s.always.count(p.id)) ++ha;
        else if (s.versions.count(UpdSkipVersionKey(p.id, p.newVersion))) ++hv;
        else out.push_back(p);
    }
    if (hiddenAlways) *hiddenAlways = ha;
    if (hiddenVersion) *hiddenVersion = hv;
    return out;
}
// Only plain safe ids are stored (anything else could break the stored text); returns false when nothing was stored.
inline bool UpdSkipAlways(UpdSkipSet& s, const std::string& id) { if (!UpdSafeToken(id, false) || id.find(';') != std::string::npos || id.find('=') != std::string::npos) return false; s.always.insert(id); return true; }
inline bool UpdSkipVersion(UpdSkipSet& s, const std::string& id, const std::string& offered) {
    if (!UpdSafeToken(id, false) || offered.empty() || offered.find_first_of(";=\r\n") != std::string::npos || offered.size() > 100) return false;
    s.versions.insert(UpdSkipVersionKey(id, offered)); return true;
}

struct UpdBatchResult {
    size_t ok = 0, bad = 0, notStarted = 0;
    bool cancelled = false;         // the person asked to stop; the package that was already running was allowed to finish
    std::string stoppedId;          // an update whose final state is unknown stops the run (the page says where)
};
// Runs the packages one after another. The cancel request is honoured BETWEEN packages only: a running installer cannot be interrupted safely,
// so the package in progress always finishes and is verified. `unresolved` says an update is left in an unknown state (the run must stop).
inline UpdBatchResult UpdRunBatch(const std::vector<UpdListedPackage>& list, const std::function<bool(const UpdListedPackage&)>& runOne,
                                  const std::function<bool()>& cancelled, const std::function<bool()>& unresolved,
                                  const std::function<void(size_t, const UpdListedPackage&)>& progress = {}) {
    UpdBatchResult r;
    for (size_t i = 0; i < list.size(); ++i) {
        if (cancelled && cancelled()) { r.cancelled = true; r.notStarted = list.size() - i; break; }
        if (progress) progress(i, list[i]);
        if (runOne(list[i])) ++r.ok;
        else {
            ++r.bad;
            if (unresolved && unresolved()) { r.stoppedId = list[i].id.empty() ? std::string("?") : list[i].id; r.notStarted = list.size() - i - 1; break; }
        }
    }
    return r;
}
inline std::wstring UpdBatchText(const UpdBatchResult& r) {
    std::wstring t = L"Update all: " + std::to_wstring(r.ok) + L" updated, " + std::to_wstring(r.bad) + L" not updated";
    if (r.cancelled) t += L"; cancelled: " + std::to_wstring(r.notStarted) + L" not started (nothing was changed for them). The package that was running when you cancelled was allowed to finish and was checked, because a running installer cannot be interrupted safely";
    if (!r.stoppedId.empty()) t += L"; stopped at " + Utf8ToW(r.stoppedId) + L" (its final state is unknown: see App Updates)" + (r.notStarted ? L", " + std::to_wstring(r.notStarted) + L" not started" : std::wstring());
    return t + L".";
}

struct UpdUiSnapshot {
    bool checked = false;
    bool busy = false;
    bool sessionCreated = false;
    bool batchActive = false, cancelRequested = false;   // Update all is running / the person asked to stop after the current package
    bool updatingNow = false;                             // 2.8.7: one program is being updated right now (alone or inside Update all)
    std::wstring updatingName;
    std::wstring status, lastReport;
    std::vector<UpdListedPackage> list;
    std::vector<UpdUiLine> lines;       // the last update, in detail
    UpdUiPending pending;               // an unfinished update on disk (refreshed after every operation and by RefreshPending)
};

class UpdUiModel {
public:
    using SessionFactory = std::function<std::shared_ptr<UgSession>(std::wstring& why)>;
    using LogFn = std::function<void(bool ok, const std::wstring&)>;
    // factory: creates the (not yet started) session, or explains why it cannot. changed: called after every state change, from
    // whatever thread made it (the page passes a function that posts a message to the UI thread). tune: test seam for the context.
    UpdUiModel(SessionFactory factory, std::function<void()> changed, LogFn log, AcbRoots roots, std::function<void(UpdContext&)> tune = {})
        : factory_(std::move(factory)), changed_(std::move(changed)), log_(std::move(log)), roots_(std::move(roots)), tune_(std::move(tune)) {}

    UpdUiSnapshot Snapshot() const {
        std::lock_guard<std::mutex> g(mu_);
        UpdUiSnapshot s; s.checked = checked_; s.status = status_; s.lastReport = last_; s.list = list_;
        s.lines = lines_; s.pending = pending_;
        s.busy = busy_.load(); s.sessionCreated = session_ != nullptr; s.batchActive = batchActive_.load(); s.cancelRequested = cancel_.load();
        s.updatingNow = curSession_ != nullptr; s.updatingName = curName_;
        return s;
    }
    int RefusedBusy() const { return refusedBusy_.load(); }
    // Cancel: honoured between packages of an Update all (UpdRunBatch). Nothing here interrupts a running installer.
    void BeginBatch() { cancel_ = false; batchActive_ = true; Notify(); }
    void EndBatch() { batchActive_ = false; cancel_ = false; Notify(); }
    bool RequestCancel() { if (!batchActive_.load()) return false; cancel_ = true; Notify(); return true; }
    bool CancelRequested() const { return cancel_.load(); }
    // 2.8.7: cancel the program that is updating right now (also stops the rest of an Update all). UniGetUI is asked to cancel its operation; the installer it started may
    // already have changed things, which is why the version is read afterwards and reported as usual.
    bool CancelRunningUpdate(std::wstring& note) {
        std::shared_ptr<UgSession> s; UpdPackage p;
        { std::lock_guard<std::mutex> g(mu_); s = curSession_; p = curPkg_; }
        if (!s) { note = L"No update is running."; return false; }
        if (batchActive_.load()) cancel_ = true;   // the rest of an Update all does not start
        const UpdProvider pr = UgMakeUpdProvider(*s);
        const bool ok = pr.cancelRunning && pr.cancelRunning(p, note);
        Notify();
        return ok;
    }

    // Worker thread. Start (or reuse) the private session and list the available winget updates.
    bool Check() {
        if (!Begin()) return false;
        std::vector<UpdListedPackage> found; std::wstring status; bool ok = false;
        std::wstring why;
        const std::shared_ptr<UgSession> s = SessionLocked(why);
        if (!s) status = why;
        else {
            const UgResult r = s->Ensure();
            if (!r.ok) status = r.message;
            else {
                std::vector<std::wstring> lines;
                const DWORD rc = s->Run(L"package updates --manager winget", lines);
                const UpdPackageListing l = UpdParsePackageListingLines(lines);
                if (rc == 0 && l.status == UpdListStatus::Ok) {
                    for (const auto& p : l.packages) if (p.isUpgradable || !p.newVersion.empty()) found.push_back(p);
                    status = std::to_wstring(found.size()) + L" update(s) available (winget).";
                    ok = true;
                } else status = l.message.empty() ? L"UniGetUI did not return a usable update list." : l.message;
            }
        }
        { std::lock_guard<std::mutex> g(mu_); checked_ = ok; list_ = std::move(found); status_ = status; }
        if (log_) log_(ok, L"Update check: " + status);
        RefreshPending();
        End();
        return ok;
    }

    // Worker thread. Run one update through UpdRun; the report is kept in words.
    bool Run(const UpdListedPackage& lp, UpdReport* out = nullptr, bool allowUnknownBefore = false) {
        if (!Begin()) return false;
        std::wstring why, text; bool ok = false; std::vector<UpdUiLine> lines;
        const std::shared_ptr<UgSession> s = SessionLocked(why);
        if (!s) text = why;
        else {
            UpdContext ctx; ctx.roots = roots_; ctx.provider = UgMakeUpdProvider(*s);
            ctx.afterRetries = 2; ctx.afterRetryWaitMs = 4000;   // 2.8.7: UniGetUI's installed list can lag behind a finished update
            if (tune_) tune_(ctx);
            UpdPackage pkg; pkg.id = lp.id; pkg.name = lp.name; pkg.manager = lp.manager; pkg.source = UpdUiSourceName(lp.source);
            pkg.allowUnknownBefore = allowUnknownBefore && UpdListedVersionUnknown(lp);
            { std::lock_guard<std::mutex> g(mu_); curSession_ = s; curPkg_ = pkg; curName_ = Utf8ToW(lp.name.empty() ? lp.id : lp.name); }
            Notify();
            const UpdReport rep = UpdRun(ctx, pkg);
            { std::lock_guard<std::mutex> g(mu_); curSession_.reset(); curName_.clear(); }
            ok = UpdUiIsSuccess(rep);
            text = UpdUiReportText(rep);
            if (out) *out = rep;
            lines = UpdUiReportLines(rep);
        }
        { std::lock_guard<std::mutex> g(mu_); last_ = text; lines_ = std::move(lines); checked_ = false; }
        if (log_) log_(ok, L"Update " + Utf8ToW(lp.id) + L": " + text);
        RefreshPending();
        End();
        return ok;
    }

    // Worker thread. Release the record of an unfinished update (never of a live process). A timed-out update is released only after
    // UniGetUI (the AnkhCore session) shows no operation for the package; `force` is the owner's explicit decision to release it
    // without that check. Nothing else changes: the recovery point stays, the record moves into <data>\Updater\history.
    bool Acknowledge(bool force) {
        if (!Begin()) return false;
        const UpdUiPending p = UpdUiReadPending(roots_);
        UpdProvider verifier; bool haveVerifier = false; std::wstring note;
        if (!force && p.unresolved) {
            std::wstring why;
            const std::shared_ptr<UgSession> s = SessionLocked(why);
            if (!s) note = why;
            else {
                const UgResult r = s->Ensure();
                if (r.ok) { verifier = UgMakeUpdProvider(*s); haveVerifier = true; } else note = r.message;
            }
        }
        UpdActiveInfo info;
        const UpdAckResult r = UpdAcknowledge(roots_, &info, alive_, haveVerifier ? &verifier : nullptr, force);
        std::wstring text = UpdUiAckText(r, force);
        if (r == UpdAckResult::Unresolved && !note.empty()) text += L"  (" + note + L")";
        const bool ok = r == UpdAckResult::Acknowledged;
        { std::lock_guard<std::mutex> g(mu_); last_ = text; lines_.clear(); }
        if (log_) log_(ok, L"Unfinished update: " + text);
        RefreshPending();
        End();
        return ok;
    }

    // Re-reads the unfinished-update marker (one small file). Safe from any thread.
    void RefreshPending() {
        const UpdUiPending p = UpdUiReadPending(roots_);
        { std::lock_guard<std::mutex> g(mu_); pending_ = p; }
        Notify();
    }
    void SetOwnerAliveCheck(std::function<bool(DWORD)> f) { alive_ = std::move(f); }   // test seam

    // App exit. Ends only the session AnkhCore itself started (UgSession::Shutdown decides that); nothing starts afterwards.
    void Shutdown() {
        std::shared_ptr<UgSession> s;
        { std::lock_guard<std::mutex> g(mu_); closed_ = true; s = std::move(session_); }
        if (s) s->Shutdown();
    }

private:
    bool Begin() {
        { std::lock_guard<std::mutex> g(mu_); if (closed_) return false; }
        if (busy_.exchange(true)) { ++refusedBusy_; return false; }
        Notify();
        return true;
    }
    void End() { busy_ = false; Notify(); }
    void Notify() { if (changed_) changed_(); }
    std::shared_ptr<UgSession> SessionLocked(std::wstring& why) {
        std::lock_guard<std::mutex> g(mu_);
        if (closed_) { why = L"AnkhCore is closing."; return nullptr; }
        if (!session_) session_ = factory_ ? factory_(why) : nullptr;
        return session_;
    }

    SessionFactory factory_;
    std::function<void()> changed_;
    LogFn log_;
    AcbRoots roots_;
    std::function<void(UpdContext&)> tune_;
    mutable std::mutex mu_;
    std::shared_ptr<UgSession> session_;
    std::shared_ptr<UgSession> curSession_; UpdPackage curPkg_; std::wstring curName_;   // 2.8.7: the update that is running right now (for Cancel)
    bool checked_ = false, closed_ = false;
    std::wstring status_, last_;
    std::vector<UpdListedPackage> list_;
    std::vector<UpdUiLine> lines_;
    UpdUiPending pending_;
    std::function<bool(DWORD)> alive_;
    std::atomic<bool> busy_{false}, cancel_{false}, batchActive_{false};
    std::atomic<int> refusedBusy_{0};
};

}  // namespace ak
