// eng_procmgr.h - 2.8 Process Manager: the change engine and the rules on top of the Process Intelligence inventory (eng_procintel.h).
//
// AnkhCore's own implementation of what process tools do (view, priority, CPU affinity, efficiency mode, end a process, rules that are applied
// automatically); designed after the capabilities of System Informer, Process Governor, ThreadPilot and Ananicy, with none of their code.
// Every change goes through ProcMayChange (critical / system / security / sensitive / unidentifiable / AnkhCore itself are NEVER changed; real-time
// priority is never used; ending a process only for ordinary applications and only after a confirmation). The state before the first change of a
// process is recorded, so "Restore" puts it back. A change counts only when the process reads back as asked. Side effects are injected.
#pragma once
#include "base.h"
#include "eng_procintel.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace ak {

struct ProcState { bool ok = false; DWORD prio = 0; uint64_t affinity = 0, systemAffinity = 0; int eco = -1; };   // eco: -1 unknown, 0 off, 1 on
struct ProcBackend {
    std::function<ProcState(DWORD pid)> read;
    std::function<bool(DWORD pid, DWORD prio, std::wstring& why)> setPriority;
    std::function<bool(DWORD pid, uint64_t mask, std::wstring& why)> setAffinity;
    std::function<bool(DWORD pid, bool eco, std::wstring& why)> setEfficiency;
    std::function<bool(DWORD pid, std::wstring& why)> terminate;
};
struct ProcHistoryItem { DWORD pid = 0; std::wstring name; ProcState before; };
using ProcHistory = std::map<DWORD, ProcHistoryItem>;
struct ProcChange { bool ok = false, refused = false; std::wstring detail; };

inline const wchar_t* ProcPrioName(DWORD p) {
    return p == kProcPrioIdle ? L"Idle" : p == kProcPrioBelowNormal ? L"Below normal" : p == kProcPrioNormal ? L"Normal" : p == kProcPrioAboveNormal ? L"Above normal" : p == kProcPrioHigh ? L"High" : p == kProcPrioRealtime ? L"Real-time" : L"unknown";
}

// The one place a change is made. `confirmed`: the person confirmed what the gate says needs confirming.
inline ProcChange ProcApplyChange(const ProcEntry& e, ProcAction a, uint64_t value, bool confirmed, DWORD selfPid, const ProcBackend& be, ProcHistory& hist) {
    ProcChange r;
    const ProcDecision d = ProcMayChange(e, a, value, selfPid);
    if (!d.allowed) { r.refused = true; r.detail = d.reason; return r; }
    if (d.needsConfirmation && !confirmed) { r.refused = true; r.detail = L"This change needs the person's confirmation first."; return r; }
    if (!be.read) { r.detail = L"There is no way to read the process."; return r; }
    const ProcState before = be.read(e.raw.pid);
    if (!before.ok) { r.detail = L"The process could not be opened (it may have ended, or it belongs to another user)."; return r; }
    if (a != ProcAction::Terminate && !hist.count(e.raw.pid)) { ProcHistoryItem h; h.pid = e.raw.pid; h.name = e.raw.name; h.before = before; hist[e.raw.pid] = h; }   // the first state is the one restored
    std::wstring why; bool did = false;
    switch (a) {
    case ProcAction::SetPriority: did = be.setPriority && be.setPriority(e.raw.pid, static_cast<DWORD>(value), why); break;
    case ProcAction::SetAffinity: {
        const uint64_t m = value & (before.systemAffinity ? before.systemAffinity : ~0ull);
        if (m == 0) { r.refused = true; r.detail = L"None of the chosen CPU cores exists on this PC."; return r; }
        did = be.setAffinity && be.setAffinity(e.raw.pid, m, why); value = m; break; }
    case ProcAction::SetEfficiency: did = be.setEfficiency && be.setEfficiency(e.raw.pid, value != 0, why); break;
    case ProcAction::Terminate: did = be.terminate && be.terminate(e.raw.pid, why); break;
    }
    if (!did) { r.detail = why.empty() ? std::wstring(L"Windows refused the change.") : why; return r; }
    const ProcState after = be.read(e.raw.pid);
    if (a == ProcAction::Terminate) { r.ok = !after.ok; r.detail = r.ok ? L"The process was ended." : L"The process is still running."; return r; }
    const bool verified = after.ok && (a == ProcAction::SetPriority ? after.prio == static_cast<DWORD>(value) : a == ProcAction::SetAffinity ? after.affinity == value : (after.eco == -1 || after.eco == (value ? 1 : 0)));
    r.ok = verified;
    r.detail = verified ? L"Done, and read back from Windows." : L"Windows accepted it but the process does not read back as asked: not counted.";
    return r;
}
// Puts back the state recorded before the first change. Each part goes through the same gate and read-back.
inline ProcChange ProcRestore(const ProcEntry& e, DWORD selfPid, const ProcBackend& be, ProcHistory& hist) {
    ProcChange r; auto it = hist.find(e.raw.pid);
    if (it == hist.end()) { r.detail = L"Nothing was changed for this process, so there is nothing to restore."; return r; }
    const ProcState b = it->second.before;
    ProcHistory scratch; bool allOk = true; std::wstring all;
    auto one = [&](ProcAction a, uint64_t v) { const ProcChange c = ProcApplyChange(e, a, v, true, selfPid, be, scratch); allOk = allOk && c.ok; if (!c.ok) all += c.detail + L" "; };
    if (b.prio) one(ProcAction::SetPriority, b.prio);
    if (b.affinity) one(ProcAction::SetAffinity, b.affinity);
    if (b.eco >= 0) one(ProcAction::SetEfficiency, b.eco ? 1 : 0);
    r.ok = allOk; r.detail = allOk ? L"The previous settings were restored." : all;
    if (allOk) hist.erase(it);
    return r;
}

// ---------------------------------------------------------------- rules (applied automatically while AnkhCore runs)
struct ProcRule { std::wstring exe; DWORD prio = 0; uint64_t affinity = 0; int eco = -1; bool enabled = true; };   // 0 / 0 / -1 = leave as it is
inline bool ProcRuleExeOk(const std::wstring& e) {
    if (e.empty() || e.size() > 64) return false;
    for (wchar_t c : e) if (!(iswalnum(c) || c == L'.' || c == L'_' || c == L'-' || c == L' ')) return false;
    return e.find(L"..") == std::wstring::npos;
}
inline std::wstring ProcSerializeRules(const std::vector<ProcRule>& v) {
    std::wstring o;
    for (const auto& r : v) { if (!o.empty()) o += L';'; o += Lower(r.exe) + L'|' + std::to_wstring(r.prio) + L'|' + std::to_wstring(r.affinity) + L'|' + std::to_wstring(r.eco) + L'|' + (r.enabled ? L"1" : L"0"); }
    return o;
}
inline std::vector<ProcRule> ProcParseRules(const std::wstring& s) {
    std::vector<ProcRule> out; std::set<std::wstring> seen; size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(L';', p); if (e == std::wstring::npos) e = s.size();
        const auto f = Split(s.substr(p, e - p), L'|'); p = e + 1;
        if (f.size() != 5 || !ProcRuleExeOk(f[0])) continue;
        ProcRule r; r.exe = Lower(f[0]);
        try { r.prio = static_cast<DWORD>(std::stoul(f[1])); r.affinity = std::stoull(f[2]); r.eco = std::stoi(f[3]); } catch (...) { continue; }
        if (r.prio != 0 && r.prio != kProcPrioIdle && r.prio != kProcPrioBelowNormal && r.prio != kProcPrioNormal && r.prio != kProcPrioAboveNormal && r.prio != kProcPrioHigh) continue;   // never real-time, never garbage
        if (r.eco < -1 || r.eco > 1) continue;
        r.enabled = f[4] == L"1";
        if (seen.insert(r.exe).second) out.push_back(r);
        if (e >= s.size()) break;
    }
    return out;
}
// Applies the enabled rules to the matching ADJUSTABLE processes (never Caution / Protected: a rule is not a confirmation for those). A process is touched once per rule
// (`done` remembers pid + exe); a value that already matches is left alone. Returns the number of changes made.
inline int ProcApplyRules(const std::vector<ProcRule>& rules, const std::vector<ProcEntry>& entries, DWORD selfPid, const ProcBackend& be, ProcHistory& hist, std::set<std::wstring>& done, std::vector<std::wstring>* log = nullptr) {
    int n = 0;
    for (const ProcEntry& e : entries) {
        if (e.prot != ProcProtection::Adjustable) continue;
        for (const ProcRule& r : rules) {
            const std::wstring nm = Lower(e.raw.name);
            if (!r.enabled || (nm != r.exe && nm != r.exe + L".exe")) continue;
            const std::wstring key = std::to_wstring(e.raw.pid) + L"|" + r.exe;
            if (done.count(key)) continue;
            const ProcState cur = be.read ? be.read(e.raw.pid) : ProcState{};
            if (!cur.ok) continue;
            done.insert(key);
            auto go = [&](ProcAction a, uint64_t v, bool differs) {
                if (!differs) return;
                const ProcChange c = ProcApplyChange(e, a, v, true, selfPid, be, hist);
                if (c.ok) ++n;
                if (log) log->push_back(e.raw.name + L": " + (a == ProcAction::SetPriority ? L"priority" : a == ProcAction::SetAffinity ? L"CPU cores" : L"efficiency mode") + L" - " + (c.ok ? L"set by your rule" : c.detail));
            };
            if (r.prio) go(ProcAction::SetPriority, r.prio, cur.prio != r.prio);
            if (r.affinity) go(ProcAction::SetAffinity, r.affinity, (r.affinity & (cur.systemAffinity ? cur.systemAffinity : ~0ull)) != cur.affinity);
            if (r.eco >= 0) go(ProcAction::SetEfficiency, r.eco ? 1 : 0, cur.eco != r.eco);
        }
    }
    return n;
}

// ---------------------------------------------------------------- live backend
inline ProcBackend ProcLiveBackend() {
    ProcBackend b;
    b.read = [](DWORD pid) {
        ProcState s; HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return s;
        DWORD_PTR pm = 0, sm = 0;
        if (GetProcessAffinityMask(h, &pm, &sm)) { s.affinity = pm; s.systemAffinity = sm; }
        s.prio = GetPriorityClass(h);
        PROCESS_POWER_THROTTLING_STATE t{};
        if (GetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t))) s.eco = (t.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ? 1 : 0;
        DWORD code = 0; s.ok = s.prio != 0 && (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE);
        CloseHandle(h); return s;
    };
    b.setPriority = [](DWORD pid, DWORD prio, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"set priority of " + std::to_wstring(pid)); why = L"the test build changes no process"; return false; }
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid); if (!h) { why = L"Windows refused access (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        const BOOL ok = SetPriorityClass(h, prio); if (!ok) why = L"Windows refused the change (error " + std::to_wstring(GetLastError()) + L")."; CloseHandle(h); return ok != 0; };
    b.setAffinity = [](DWORD pid, uint64_t mask, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"set affinity of " + std::to_wstring(pid)); why = L"the test build changes no process"; return false; }
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid); if (!h) { why = L"Windows refused access (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        const BOOL ok = SetProcessAffinityMask(h, static_cast<DWORD_PTR>(mask)); if (!ok) why = L"Windows refused the change (error " + std::to_wstring(GetLastError()) + L")."; CloseHandle(h); return ok != 0; };
    b.setEfficiency = [](DWORD pid, bool eco, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"set efficiency of " + std::to_wstring(pid)); why = L"the test build changes no process"; return false; }
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid); if (!h) { why = L"Windows refused access (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        PROCESS_POWER_THROTTLING_STATE t{}; t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION; t.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED; t.StateMask = eco ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
        const BOOL ok = SetProcessInformation(h, ProcessPowerThrottling, &t, sizeof(t)); if (!ok) why = L"Windows refused the change (error " + std::to_wstring(GetLastError()) + L"); efficiency mode needs Windows 10 version 1709 or newer."; CloseHandle(h); return ok != 0; };
    b.terminate = [](DWORD pid, std::wstring& why) {
        if (kTestBuild) { TestRefused(L"end process " + std::to_wstring(pid)); why = L"the test build ends no process"; return false; }
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid); if (!h) { why = L"Windows refused access (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        const BOOL ok = TerminateProcess(h, 1); if (!ok) why = L"Windows refused (error " + std::to_wstring(GetLastError()) + L")."; else WaitForSingleObject(h, 2000); CloseHandle(h); return ok != 0; };
    return b;
}

// ---------------------------------------------------------------- the model the page uses
struct ProcMgrRow { ProcEntry e; double cpu = -1; };
class ProcMgrModel {
public:
    std::vector<ProcMgrRow> Rows() const { std::lock_guard<std::mutex> lk(mx_); return rows_; }
    bool Busy() const { return busy_.load(); }
    ULONGLONG LastTick() const { return last_.load(); }
    ProcHistory History() const { std::lock_guard<std::mutex> lk(mx_); return hist_; }
    void SetHistory(const ProcHistory& h) { std::lock_guard<std::mutex> lk(mx_); hist_ = h; }
    void RefreshAsync(std::function<void()> notify) {
        if (busy_.exchange(true)) return;
        std::thread([this, notify = std::move(notify)] {
            const ProcContext ctx = ProcContextLive();
            const ProcInventory inv = ProcBuildInventory(ProcCollectLive(), ctx);
            std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> now;   // pid -> (cpu time in 100 ns, wall tick in ms)
            SYSTEM_INFO si; GetSystemInfo(&si); const double cores = std::max<DWORD>(1, si.dwNumberOfProcessors);
            std::vector<ProcMgrRow> rows;
            const ULONGLONG tick = GetTickCount64();
            for (const ProcEntry& e : inv.entries) {
                ProcMgrRow r; r.e = e;
                if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.raw.pid)) {
                    FILETIME c, x, k, u; if (GetProcessTimes(h, &c, &x, &k, &u)) {
                        const ULONGLONG t = ((static_cast<ULONGLONG>(k.dwHighDateTime) << 32) | k.dwLowDateTime) + ((static_cast<ULONGLONG>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
                        now[e.raw.pid] = {t, tick};
                        auto it = prev_.find(e.raw.pid);
                        if (it != prev_.end() && tick > it->second.second) r.cpu = std::min(100.0, 100.0 * (static_cast<double>(t - it->second.first) / 10000.0) / (static_cast<double>(tick - it->second.second) * cores));
                    }
                    CloseHandle(h);
                }
                rows.push_back(std::move(r));
            }
            prev_ = std::move(now);
            std::sort(rows.begin(), rows.end(), [](const ProcMgrRow& a, const ProcMgrRow& b) { return a.e.raw.workingSet > b.e.raw.workingSet; });
            { std::lock_guard<std::mutex> lk(mx_); rows_ = std::move(rows); }
            last_ = GetTickCount64(); busy_ = false;
            if (notify) notify();
        }).detach();
    }
private:
    mutable std::mutex mx_; std::vector<ProcMgrRow> rows_; ProcHistory hist_;
    std::map<DWORD, std::pair<ULONGLONG, ULONGLONG>> prev_;
    std::atomic<bool> busy_{false}; std::atomic<ULONGLONG> last_{0};
};

}  // namespace ak
