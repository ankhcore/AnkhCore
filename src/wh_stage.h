// FT-C02: Windhawk changes are staged, then applied together: SELECT -> CONFIGURE -> REVIEW -> APPLY -> EXECUTE -> VERIFY.
// A switch or a setting edit only records what the user wants (pending); the applied state stays visible beside it. Apply runs
// every pending operation through an executor that changes AND reads back, keeps whatever failed pending for a retry, names
// each failure, and offers at most one Explorer restart for the whole transaction. Pure: the executor does the real work.
#pragma once
#include "wh_schema.h"
#include <functional>
#include <map>

namespace ak {

struct WhOp {
    enum Kind { SetEnabled, SetSettings } kind = SetEnabled;
    std::string modId;
    bool enable = false;                                         // SetEnabled
    std::vector<std::pair<std::string, JVal>> changes;           // SetSettings: flat key -> new value
};

class WhStage {
public:
    // The user asks for a state; asking for the one that is already applied clears the request.
    void StageEnabled(const std::string& mod, bool want, bool applied) { if (want == applied) enabled_.erase(mod); else enabled_[mod] = want; }
    void StageSetting(const std::string& mod, const std::string& path, const JVal& want, const JVal& applied) {
        auto& m = settings_[mod];
        if (WhValueEquals(want, applied)) m.erase(path); else m[path] = want;
        if (m.empty()) settings_.erase(mod);
    }
    bool EnabledPending(const std::string& mod, bool& want) const { const auto it = enabled_.find(mod); if (it == enabled_.end()) return false; want = it->second; return true; }
    const JVal* SettingPending(const std::string& mod, const std::string& path) const {
        const auto it = settings_.find(mod); if (it == settings_.end()) return nullptr;
        const auto jt = it->second.find(path); return jt == it->second.end() ? nullptr : &jt->second;
    }
    size_t SettingCount(const std::string& mod) const { const auto it = settings_.find(mod); return it == settings_.end() ? 0 : it->second.size(); }
    size_t Count() const { size_t n = enabled_.size(); for (const auto& m : settings_) n += m.second.size(); return n; }
    bool Empty() const { return Count() == 0; }
    void CancelMod(const std::string& mod) { enabled_.erase(mod); settings_.erase(mod); }
    void DiscardAll() { enabled_.clear(); settings_.clear(); }
    // Order: turning mods on or off first, then settings, each mod's settings as one write.
    std::vector<WhOp> Plan() const {
        std::vector<WhOp> ops;
        for (const auto& e : enabled_) { WhOp o; o.kind = WhOp::SetEnabled; o.modId = e.first; o.enable = e.second; ops.push_back(o); }
        for (const auto& m : settings_) { WhOp o; o.kind = WhOp::SetSettings; o.modId = m.first; for (const auto& kv : m.second) o.changes.push_back({kv.first, kv.second}); ops.push_back(o); }
        return ops;
    }
    void Done(const WhOp& op) {   // a verified operation leaves the stage; a failed one stays for a retry
        if (op.kind == WhOp::SetEnabled) enabled_.erase(op.modId);
        else { auto it = settings_.find(op.modId); if (it != settings_.end()) { for (const auto& ch : op.changes) it->second.erase(ch.first); if (it->second.empty()) settings_.erase(it); } }
    }
private:
    std::map<std::string, bool> enabled_;
    std::map<std::string, std::map<std::string, JVal>> settings_;
};

// The real work. Each call changes AND verifies by reading the state back; true only when the read-back matches.
struct WhExec {
    std::function<bool(const std::string& mod, bool enable, std::wstring& err)> setEnabled;
    std::function<bool(const std::string& mod, const std::vector<std::pair<std::string, JVal>>& changes, std::wstring& err)> writeSettings;
    std::function<bool(const std::string& mod)> reachesExplorer;   // does the mod run inside explorer.exe? (decides the one Explorer offer)
};

struct WhOpResult { WhOp op; bool ok = false; std::wstring error; };
struct WhApplyResult {
    std::vector<WhOpResult> results;
    bool offerExplorerRestart = false;                             // at most once per Apply, never done automatically
    int Ok() const { int n = 0; for (const auto& r : results) n += r.ok; return n; }
    int Failed() const { return static_cast<int>(results.size()) - Ok(); }
    bool AllOk() const { return !results.empty() && Failed() == 0; }
};

inline WhApplyResult WhApply(WhStage& stage, const WhExec& ex) {
    WhApplyResult res;
    const std::vector<WhOp> plan = stage.Plan();
    for (const WhOp& op : plan) {
        WhOpResult r; r.op = op;
        if (op.kind == WhOp::SetEnabled) r.ok = ex.setEnabled && ex.setEnabled(op.modId, op.enable, r.error);
        else r.ok = ex.writeSettings && ex.writeSettings(op.modId, op.changes, r.error);
        if (!r.ok && r.error.empty()) r.error = L"the operation is not available";
        if (r.ok) { stage.Done(op); if (ex.reachesExplorer && ex.reachesExplorer(op.modId)) res.offerExplorerRestart = true; }
        res.results.push_back(r);
    }
    return res;
}

inline std::wstring WhSummary(const WhApplyResult& r, const std::function<std::wstring(const std::string&)>& nameOf) {
    if (r.results.empty()) return L"Nothing was pending.";
    std::wstring s = std::to_wstring(r.Ok()) + L" of " + std::to_wstring(r.results.size()) + L" change(s) applied and verified";
    if (r.Failed()) {
        s += L"; " + std::to_wstring(r.Failed()) + L" failed and stay pending:";
        for (const auto& x : r.results) if (!x.ok) s += L"\n  \x2022 " + nameOf(x.op.modId) + (x.op.kind == WhOp::SetEnabled ? (x.op.enable ? L" (turn on): " : L" (turn off): ") : L" (settings): ") + x.error;
    } else s += L".";
    if (r.offerExplorerRestart) s += L"\nSome of these mods run inside Explorer; restarting Explorer once makes it pick everything up (optional).";
    return s;
}

}  // namespace ak
