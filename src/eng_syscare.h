// eng_syscare.h - 2.8 System Care: a guided scan -> review -> approval -> execution -> verification workflow ON TOP of the cleaner AnkhCore already has.
//
// It adds no second cleaner. The categories, their size measuring and their deletion code are the existing CleanItems (eng_system.h, with the
// WT02 protections); this file adds what was missing around them:
//   * Scanner:   read-only. Measures every non-ignored cleaner item and finds broken shortcuts. Nothing is deleted while scanning or classifying.
//   * Classifier: Cleanable / Review required / In use or not readable / Protected / Unknown, with the evidence that produced it.
//   * Planner:   turns the person's choices into a plan; only Cleanable (and confirmed Review) items can enter it; ignored items never do.
//   * Executor:  fresh validation right before each item, one item at a time; the result reports what the cleaner SAID it freed and what a
//                new measurement VERIFIED. Space is never claimed that was not measured.
//   * Recovery:  a broken shortcut is moved to a recovery folder (undoable). Cleaner items are deleted files: they say so, no rollback is promised.
//   * Exclusions: a visible, persistent ignore list.
// No registry cleaning, no heuristic deletion. Everything with side effects is injected (CareBackend), so it is tested without touching a system.
#pragma once
#include "base.h"
#include "eng_remnants.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace ak {

enum class CareClass { Cleanable, Review, NotReadable, Protected, Unknown };
inline const wchar_t* CareClassLabel(CareClass c) {
    switch (c) {
    case CareClass::Cleanable: return L"Cleanable";
    case CareClass::Review: return L"Review required";
    case CareClass::NotReadable: return L"In use or not readable";
    case CareClass::Protected: return L"Protected";
    default: return L"Unknown";
    }
}
enum class CareKind { CleanerItem, BrokenShortcut, ProgramUpdates, WindowsUpdates };   // ProgramUpdates and WindowsUpdates are information only: never cleaned. The page offers Update all / its Windows updates section

struct CareFinding {
    CareKind kind = CareKind::CleanerItem;
    std::wstring id;                  // CleanItem id, or the shortcut's full path
    std::wstring title, category, evidence;   // evidence: what it is and why it has this class
    CareClass cls = CareClass::Unknown;
    uint64_t bytes = 0, notReadable = 0;       // bytes measured; folders that could not be read
    bool irreversible = true;         // deleted files cannot be restored; false only for what is moved to a recovery folder
};

// ---------------------------------------------------------------- exclusions (pure)
inline std::set<std::wstring> CareParseIgnore(const std::wstring& stored) {
    std::set<std::wstring> s; size_t p = 0;
    while (p <= stored.size()) {
        size_t e = stored.find(L'|', p); if (e == std::wstring::npos) e = stored.size();
        std::wstring t = stored.substr(p, e - p);
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        if (!t.empty() && t.size() <= 520) s.insert(Lower(t));
        p = e + 1;
    }
    return s;
}
inline std::wstring CareSerializeIgnore(const std::set<std::wstring>& s) { std::wstring o; for (const auto& x : s) { if (!o.empty()) o += L'|'; o += x; } return o; }
inline bool CareIsIgnored(const std::set<std::wstring>& ignore, const std::wstring& id) { return ignore.count(Lower(id)) > 0; }

// ---------------------------------------------------------------- the broken-shortcut scanner (read-only)
struct CareShortcutEnv {
    std::vector<std::wstring> roots;                                       // Start Menu (all users, this user), Desktop (all users, this user)
    std::function<bool(const std::wstring&)> targetDriveIsFixedAndPresent; // exact-target validation: a removable or network drive that is offline is not "broken"
};
inline std::vector<CareFinding> CareScanBrokenShortcuts(const CareShortcutEnv& env, const RemFs& fs) {
    using namespace remdetail;
    std::vector<CareFinding> out;
    if (!fs.list || !fs.shortcutTarget || !fs.exists) return out;
    std::set<std::wstring> seen;
    for (const std::wstring& base : env.roots) {
        if (base.empty()) continue;
        std::vector<std::wstring> dirs{base};
        for (const RemDirEntry& e : fs.list(base)) if (e.dir && !e.reparse) dirs.push_back(base + L"\\" + e.name);   // one level of folders, never a link
        for (const std::wstring& d : dirs)
            for (const RemDirEntry& e : fs.list(d)) {
                if (e.dir || e.reparse || !HasExt(Norm(e.name), {L".lnk"})) continue;
                const std::wstring lnk = d + L"\\" + e.name;
                if (!seen.insert(Norm(lnk)).second) continue;
                std::wstring target;
                if (!fs.shortcutTarget(lnk, target) || Trim(target).empty()) continue;      // cannot read it / a shell item (Store apps): not judged
                if (target.find(L'%') != std::wstring::npos) continue;                      // unresolved variable: not judged
                const std::wstring tn = Norm(target);
                if (!ValidAbs(tn)) continue;                                                // not a plain drive path (UNC, device, relative): not judged
                if (fs.exists(target)) continue;                                            // the target is there: not broken
                if (!env.targetDriveIsFixedAndPresent || !env.targetDriveIsFixedAndPresent(target)) continue;   // an offline removable / network drive is not evidence
                CareFinding f; f.kind = CareKind::BrokenShortcut; f.id = lnk; f.title = e.name; f.category = L"Broken shortcuts"; f.bytes = e.size;
                f.cls = CareClass::Cleanable; f.irreversible = false;
                f.evidence = L"Its target " + target + L" is on a local drive that is present and the file or folder does not exist. Moved to a recovery folder, not deleted.";
                out.push_back(std::move(f));
            }
    }
    return out;
}

// ---------------------------------------------------------------- classification of a cleaner item (pure)
// `tagRank`: 0 Safe, 1 Optional (review), 2 Confirm (asks first, irreversible).
inline CareFinding CareClassifyCleanerItem(const std::wstring& id, const std::wstring& title, const std::wstring& category, const std::wstring& desc, int tagRank, uint64_t bytes, uint64_t notReadable) {
    CareFinding f; f.kind = CareKind::CleanerItem; f.id = id; f.title = title; f.category = category; f.bytes = bytes; f.notReadable = notReadable;
    if (tagRank == 0) { f.cls = CareClass::Cleanable; f.evidence = desc; }
    else { f.cls = CareClass::Review; f.evidence = desc + (tagRank == 2 ? L"  Asks first: the files are permanently removed." : L"  Optional: review before cleaning."); }
    if (notReadable > 0) f.evidence += L"  " + std::to_wstring(notReadable) + L" folder(s) could not be read (in use or no access) and are left alone.";
    return f;
}

// ---------------------------------------------------------------- planner (pure)
struct CarePlan { std::vector<CareFinding> items; std::vector<std::wstring> refused; };
// `chosen`: ids the person ticked. `confirmedReview`: ids of Review items the person explicitly confirmed. Ignored items never enter.
inline CarePlan CareBuildPlan(const std::vector<CareFinding>& findings, const std::set<std::wstring>& chosen, const std::set<std::wstring>& confirmedReview, const std::set<std::wstring>& ignore) {
    CarePlan p; std::set<std::wstring> seen;
    for (const CareFinding& f : findings) {
        if (!chosen.count(f.id)) continue;
        if (!seen.insert(Lower(f.id)).second) { p.refused.push_back(f.title + L": listed twice."); continue; }
        if (CareIsIgnored(ignore, f.id)) { p.refused.push_back(f.title + L": it is on the ignore list."); continue; }
        if (f.cls == CareClass::Cleanable || (f.cls == CareClass::Review && confirmedReview.count(f.id))) p.items.push_back(f);
        else p.refused.push_back(f.title + L": " + CareClassLabel(f.cls) + (f.cls == CareClass::Review ? L" and not confirmed." : L" - never cleaned automatically."));
    }
    return p;
}

// ---------------------------------------------------------------- executor
enum class CareState { Cleaned, NothingFreed, Failed, Refused };
inline const wchar_t* CareStateLabel(CareState s) { return s == CareState::Cleaned ? L"Cleaned" : s == CareState::NothingFreed ? L"Nothing freed" : s == CareState::Failed ? L"Failed" : L"Not run"; }
struct CareResult { CareFinding f; CareState state = CareState::Refused; uint64_t before = 0, after = 0, reported = 0, verified = 0; std::wstring detail; };
struct CareReport { std::vector<CareResult> items; uint64_t verified = 0, reported = 0; int cleaned = 0, nothing = 0, failed = 0, refused = 0; std::wstring summary, recoveryDir; };
struct CareBackend {
    std::function<bool(const CareFinding&)> exists;                                             // fresh validation right before the item
    std::function<uint64_t(const CareFinding&)> measure;                                        // bytes there now
    std::function<uint64_t(const CareFinding&)> clean;                                          // the existing cleaner; returns what it says it freed
    std::function<bool(const CareFinding&, const std::wstring& recoveryDir, std::wstring& why)> moveToRecovery;   // broken shortcuts
};
inline CareReport CareExecute(const CarePlan& plan, const CareBackend& be, const std::wstring& recoveryDir, const std::function<void(size_t, size_t, const std::wstring&)>& progress = {}) {
    CareReport rep; rep.recoveryDir = recoveryDir;
    for (size_t i = 0; i < plan.items.size(); ++i) {
        CareResult r; r.f = plan.items[i];
        if (progress) progress(i, plan.items.size(), r.f.title);
        if (!(r.f.cls == CareClass::Cleanable || r.f.cls == CareClass::Review)) { r.state = CareState::Refused; r.detail = L"Not allowed."; }
        else if (!be.measure) { r.state = CareState::Failed; r.detail = L"There is no way to measure."; }
        else if (be.exists && !be.exists(r.f)) { r.state = CareState::NothingFreed; r.detail = L"It is already gone."; }
        else {
            r.before = be.measure(r.f);
            if (r.f.kind == CareKind::BrokenShortcut) {
                std::wstring why;
                if (be.moveToRecovery && be.moveToRecovery(r.f, recoveryDir, why)) { r.state = CareState::Cleaned; r.detail = why; r.verified = r.before; r.after = 0; }
                else { r.state = CareState::Failed; r.detail = why.empty() ? L"It could not be moved." : why; }
            } else if (!be.clean) { r.state = CareState::Failed; r.detail = L"There is no cleaner."; }
            else {
                r.reported = be.clean(r.f);
                r.after = be.measure(r.f);
                r.verified = r.before > r.after ? r.before - r.after : 0;     // only what a second measurement confirms is claimed
                if (r.verified > 0) { r.state = CareState::Cleaned; r.detail = L"Measured " + FormatBytes(r.before) + L" before and " + FormatBytes(r.after) + L" after."; }
                else { r.state = CareState::NothingFreed; r.detail = r.reported > 0 ? L"The cleaner reported " + FormatBytes(r.reported) + L", but a new measurement found nothing less: not counted." : L"Nothing could be removed (files in use or already empty)."; }
                if (r.after > 0 && r.verified > 0) r.detail += L"  " + FormatBytes(r.after) + L" remain (in use or protected).";
            }
        }
        rep.items.push_back(std::move(r));
    }
    for (const auto& r : rep.items) {
        rep.reported += r.reported;
        if (r.state == CareState::Cleaned) { ++rep.cleaned; rep.verified += r.verified; } else if (r.state == CareState::NothingFreed) ++rep.nothing; else if (r.state == CareState::Failed) ++rep.failed; else ++rep.refused;
    }
    rep.summary = L"Verified free space gained: " + FormatBytes(rep.verified) + L".  " + std::to_wstring(rep.cleaned) + L" cleaned, " + std::to_wstring(rep.nothing) + L" nothing freed, " + std::to_wstring(rep.failed) + L" failed, " + std::to_wstring(rep.refused) + L" not run.";
    return rep;
}

// ---------------------------------------------------------------- read-only diagnostics
struct CareDiag {
    struct Proc { std::wstring name; uint64_t bytes = 0; };
    std::vector<Proc> topMemory;       // the biggest working sets
    int startupEntries = 0;            // values under the Run keys (HKCU, HKLM, HKLM Wow6432Node)
    std::vector<std::wstring> startupNames;
};

// ---------------------------------------------------------------- the model the page uses (one scan or one clean at a time)
struct CareView {
    int phase = 0;                       // 0 idle, 1 scanning, 2 scanned, 3 cleaning, 4 cleaned
    size_t done = 0, total = 0; std::wstring current;
    std::vector<CareFinding> findings;
    std::set<std::wstring> chosen, confirmed;
    CareReport report;
    std::wstring undoText, lastRecoveryDir;
    ULONGLONG scannedAt = 0;
};
class CareMgr {
public:
    CareView Snapshot() const { std::lock_guard<std::mutex> lk(mx_); return v_; }
    bool Busy() const { std::lock_guard<std::mutex> lk(mx_); return v_.phase == 1 || v_.phase == 3; }
    void Toggle(const std::wstring& id, bool review) {
        std::lock_guard<std::mutex> lk(mx_);
        if (!v_.chosen.erase(id)) { v_.chosen.insert(id); if (review) v_.confirmed.insert(id); } else v_.confirmed.erase(id);
    }
    void Cancel() { cancel_ = true; }
    void Forget(const std::wstring& id) { std::lock_guard<std::mutex> lk(mx_); v_.chosen.erase(id); v_.confirmed.erase(id); std::vector<CareFinding> keep; for (auto& f : v_.findings) if (Lower(f.id) != Lower(id)) keep.push_back(f); v_.findings = std::move(keep); }
    // `scan` produces the findings (read-only); it receives a progress callback and the cancel flag.
    bool StartScan(std::function<std::vector<CareFinding>(const std::function<void(size_t, size_t, const std::wstring&)>&, const std::atomic<bool>&)> scan, std::function<void()> notify) {
        { std::lock_guard<std::mutex> lk(mx_); if (v_.phase == 1 || v_.phase == 3) return false; const std::wstring keep = v_.lastRecoveryDir; v_ = CareView{}; v_.lastRecoveryDir = keep; v_.phase = 1; }
        cancel_ = false;
        std::thread([this, scan = std::move(scan), notify = std::move(notify)] {
            auto prog = [this, &notify](size_t i, size_t n, const std::wstring& cur) { { std::lock_guard<std::mutex> lk(mx_); v_.done = i; v_.total = n; v_.current = cur; } if (notify) notify(); };
            std::vector<CareFinding> f = scan(prog, cancel_);
            { std::lock_guard<std::mutex> lk(mx_);
              v_.findings = std::move(f); v_.phase = 2; v_.current.clear(); v_.scannedAt = GetTickCount64();
              for (const auto& x : v_.findings) if (x.cls == CareClass::Cleanable && x.bytes > 0) v_.chosen.insert(x.id); }   // cleanable items start ticked; Review items never do
            if (notify) notify();
        }).detach();
        return true;
    }
    bool StartClean(const std::set<std::wstring>& ignore, CareBackend be, std::function<std::wstring(const CareFinding&)> recoveryFor, std::function<void()> notify, const std::set<std::wstring>* only = nullptr) {
        CarePlan plan;
        { std::lock_guard<std::mutex> lk(mx_); if (v_.phase != 2 && v_.phase != 4) return false; plan = CareBuildPlan(v_.findings, only ? *only : v_.chosen, only ? std::set<std::wstring>() : v_.confirmed, ignore); if (plan.items.empty()) return false; v_.phase = 3; v_.total = plan.items.size(); v_.done = 0; }
        std::thread([this, plan = std::move(plan), be = std::move(be), recoveryFor = std::move(recoveryFor), notify = std::move(notify), onlyCopy = only ? *only : std::set<std::wstring>(), hasOnly = only != nullptr] {
            const std::wstring dir = recoveryFor ? recoveryFor(plan.items.front()) : std::wstring();
            auto prog = [this, &notify](size_t i, size_t n, const std::wstring& cur) { { std::lock_guard<std::mutex> lk(mx_); v_.done = i; v_.total = n; v_.current = cur; } if (notify) notify(); };
            const CareReport rep = CareExecute(plan, be, dir, prog);
            LogI(L"System Care: " + rep.summary);
            for (const auto& r : rep.items) LogI(L"  " + r.f.title + L": " + CareStateLabel(r.state) + L" - " + r.detail);
            { std::lock_guard<std::mutex> lk(mx_);
              v_.report = rep; v_.phase = 4; v_.current.clear();
              bool moved = false; for (const auto& r : rep.items) if (r.f.kind == CareKind::BrokenShortcut && r.state == CareState::Cleaned) moved = true;
              if (moved) v_.lastRecoveryDir = dir;
              std::vector<CareFinding> left;
              for (const auto& f : v_.findings) { bool gone = false; for (const auto& r : rep.items) if (r.f.id == f.id && r.state == CareState::Cleaned && f.kind == CareKind::BrokenShortcut) gone = true; if (!gone) left.push_back(f); }
              v_.findings = std::move(left);
              if (hasOnly) { for (const auto& id : onlyCopy) { v_.chosen.erase(id); v_.confirmed.erase(id); } }   // Fix All: the ticks the person made themselves stay
              else { v_.chosen.clear(); v_.confirmed.clear(); } }
            if (notify) notify();
        }).detach();
        return true;
    }
    void Dismiss() { std::lock_guard<std::mutex> lk(mx_); if (v_.phase == 4) { v_.phase = 2; v_.report = CareReport{}; } }
    void SetUndoText(const std::wstring& t, bool allBack) { std::lock_guard<std::mutex> lk(mx_); v_.undoText = t; if (allBack) v_.lastRecoveryDir.clear(); }
    std::wstring LastRecoveryDir() const { std::lock_guard<std::mutex> lk(mx_); return v_.lastRecoveryDir; }
private:
    mutable std::mutex mx_;
    CareView v_;
    std::atomic<bool> cancel_{false};
};

// ---------------------------------------------------------------- 2.8.3: the pinned action bar (pure state; the page draws it)
// "Clean selected" cleans exactly what the person ticked. "Update all / Fix all" does the safe, applicable actions only: the program updates that
// are not skipped, and the broken shortcuts (moved to a recovery folder, undoable). It never touches cache/junk items and never anything the person did not see.
struct CareBarState {
    size_t chosen = 0; uint64_t chosenBytes = 0;     // what "Clean selected" would clean
    size_t updates = 0;                               // program updates available (not skipped)
    std::vector<std::wstring> fixIds;                 // broken shortcuts that "Fix all" would move to recovery
    bool scanned = false;                             // a finished scan exists (phase 2 or 4)
    bool canClean = false, canFixAll = false;
    std::wstring cleanWhy, fixWhy;                    // why a button is off ("" when it is on)
};
inline CareBarState CareBarAssess(const CareView& v, size_t visibleUpdates, bool updatesBusy, bool updatePending, bool otherOperationBusy, const std::set<std::wstring>& ignore) {
    CareBarState b;
    b.scanned = v.phase == 2 || v.phase == 4;
    for (const CareFinding& f : v.findings) {
        if (v.chosen.count(f.id) && (f.cls == CareClass::Cleanable || f.cls == CareClass::Review)) { ++b.chosen; b.chosenBytes += f.bytes; }
        if (f.kind == CareKind::BrokenShortcut && f.cls == CareClass::Cleanable && !CareIsIgnored(ignore, f.id)) b.fixIds.push_back(f.id);
    }
    b.updates = visibleUpdates;
    const bool scanning = v.phase == 1, cleaning = v.phase == 3;
    if (scanning) b.cleanWhy = b.fixWhy = L"A scan is running. Nothing is cleaned while scanning.";
    else if (cleaning) b.cleanWhy = b.fixWhy = L"A cleanup is running.";
    else if (otherOperationBusy || updatesBusy) b.cleanWhy = b.fixWhy = L"Another operation is running.";
    else if (!b.scanned) b.cleanWhy = b.fixWhy = L"Run Smart scan first.";
    else {
        if (b.chosen == 0) b.cleanWhy = L"Nothing is selected.";
        if (b.updates == 0 && b.fixIds.empty()) b.fixWhy = L"No program update and no broken shortcut to fix.";
        else if (b.updates > 0 && updatePending && b.fixIds.empty()) b.fixWhy = L"An unfinished update must be released first (App Updates).";
    }
    b.canClean = b.cleanWhy.empty();
    b.canFixAll = b.fixWhy.empty();
    return b;
}

}  // namespace ak
