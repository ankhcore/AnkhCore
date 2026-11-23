// settings_migrate.h - 2.7 F1: versioned, reversible settings migration on top of `settings.version`.
//
// The settings document is the flat key -> JSON-literal map that Settings keeps (settings.h). Before F1 every load overwrote
// `settings.version` with the current number, so a file written by a NEWER build was silently relabelled as ours and a damaged version
// literal was silently replaced. This runner makes the rules explicit:
//   * the version is READ, not assumed: missing or unreadable = version 0 (the reason is recorded, the original literal is kept in the notes);
//   * a document from a newer version is left exactly as it is (never relabelled, never downgraded): RefusedNewer;
//   * an older document is moved forward one declared step at a time, on a COPY: any failure leaves the original untouched;
//   * every step declares which keys it may touch; a change anywhere else fails the step (so a migration can never quietly drop or
//     rewrite a user's other settings; unknown keys always survive);
//   * a backup of the file is requested before the first step; a step marked highImpact is refused when no backup could be made;
//   * the report says what happened. Nothing is silently reset.
// Pure: no file, registry or log access here (the caller passes the backup hook and logs the report).
#pragma once
#include "base.h"

namespace ak {

using SettingsValues = std::map<std::wstring, std::wstring>;   // key -> JSON literal, exactly as Settings stores it

struct SettingsMigrationStep {
    int from = 0, to = 0;
    const char* id = "";
    bool highImpact = false;                 // refuse to run when the pre-migration backup could not be made
    std::vector<std::wstring> touches;       // exact keys, or prefixes ending in '.', that the step may add, change or remove
    std::function<bool(SettingsValues&, std::wstring& why)> apply;
};

enum class SettingsMigrationOutcome { NotNeeded, Applied, RefusedNewer, Failed };
inline const wchar_t* SettingsMigrationOutcomeLabel(SettingsMigrationOutcome o) {
    switch (o) {
    case SettingsMigrationOutcome::Applied: return L"migrated";
    case SettingsMigrationOutcome::RefusedNewer: return L"left unchanged (written by a newer version)";
    case SettingsMigrationOutcome::Failed: return L"migration failed, settings left unchanged";
    default: return L"no migration needed";
    }
}

struct SettingsMigrationReport {
    SettingsMigrationOutcome outcome = SettingsMigrationOutcome::NotNeeded;
    int fromVersion = -1, toVersion = -1;
    std::vector<std::wstring> stepsApplied, notes;
    bool backupMade = false;
    std::wstring backupPath, error;
};

struct SettingsMigrationHooks {
    // Makes a copy of the settings file before anything changes. Returns true when the copy exists or when there is nothing to copy
    // (`where` stays empty then). Absent hook = no backup possible.
    std::function<bool(int fromVersion, int toVersion, std::wstring& where)> backup;
};

// The migrations AnkhCore has today: the only change between "no version" and version 1 is the version stamp itself.
inline const std::vector<SettingsMigrationStep>& SettingsMigrationSteps() {
    static const std::vector<SettingsMigrationStep> v = {
        {0, 1, "stamp-version", false, {}, [](SettingsValues&, std::wstring&) { return true; }},
    };
    return v;
}

namespace migdetail {
inline bool Covered(const std::wstring& key, const std::vector<std::wstring>& touches) {
    for (const std::wstring& t : touches) {
        if (!t.empty() && t.back() == L'.') { if (key.compare(0, t.size(), t) == 0) return true; }
        else if (key == t) return true;
    }
    return false;
}
// "0" .. "999999", no sign, no fraction, no quotes
inline bool ParseVersion(const std::wstring& lit, int& out) {
    if (lit.empty() || lit.size() > 6) return false;
    int v = 0;
    for (wchar_t c : lit) { if (c < L'0' || c > L'9') return false; v = v * 10 + (c - L'0'); }
    out = v; return true;
}
}  // namespace migdetail

inline SettingsMigrationReport SettingsMigrate(SettingsValues& values, int target, const std::vector<SettingsMigrationStep>& steps, const SettingsMigrationHooks& hooks) {
    SettingsMigrationReport r;
    r.toVersion = target;
    const std::wstring kVer = L"settings.version";
    int cur = 0;
    auto it = values.find(kVer);
    if (it == values.end()) r.notes.push_back(L"The file has no settings.version; it is treated as version 0.");
    else if (!migdetail::ParseVersion(it->second, cur)) {
        cur = 0;
        r.notes.push_back(L"settings.version was not a plain whole number (" + it->second.substr(0, 40) + L"); it is treated as version 0 and replaced by the current version.");
    }
    r.fromVersion = cur;
    if (cur > target) {
        r.outcome = SettingsMigrationOutcome::RefusedNewer;
        r.notes.push_back(L"The settings were written by a newer version (" + std::to_wstring(cur) + L" > " + std::to_wstring(target) + L"). They are kept exactly as they are and are not relabelled or downgraded.");
        return r;
    }
    if (cur == target && it != values.end() && r.notes.empty()) return r;   // NotNeeded

    // the chain of steps from `cur` to `target`
    std::vector<const SettingsMigrationStep*> chain;
    for (int v = cur; v < target;) {
        const SettingsMigrationStep* next = nullptr;
        for (const auto& s : steps) if (s.from == v && s.to > v && s.to <= target && s.apply) { if (!next || s.to < next->to) next = &s; }
        if (!next) { r.outcome = SettingsMigrationOutcome::Failed; r.error = L"There is no migration from settings version " + std::to_wstring(v) + L"."; return r; }
        chain.push_back(next); v = next->to;
    }
    if (chain.empty()) {   // same version, but the version key itself was unreadable: only the stamp is repaired
        SettingsValues repaired = values; repaired[kVer] = std::to_wstring(target);
        values = std::move(repaired);
        r.outcome = SettingsMigrationOutcome::Applied;
        return r;
    }
    bool anyHigh = false; for (const auto* s : chain) anyHigh = anyHigh || s->highImpact;
    std::wstring where; bool ok = false;
    if (hooks.backup) ok = hooks.backup(cur, target, where);
    r.backupMade = ok && !where.empty(); r.backupPath = where;
    if (!ok) {
        if (anyHigh) { r.outcome = SettingsMigrationOutcome::Failed; r.error = L"A backup of the settings could not be made, and a step of this migration requires one. Nothing was changed."; return r; }
        r.notes.push_back(L"A backup of the settings could not be made before migrating.");
    }

    SettingsValues work = values;
    for (const auto* s : chain) {
        const SettingsValues before = work;
        std::wstring why;
        if (!s->apply(work, why)) { r.outcome = SettingsMigrationOutcome::Failed; r.error = std::wstring(L"Migration step \"") + Utf8ToW(s->id) + L"\" failed" + (why.empty() ? L"." : L": " + why); return r; }
        // nothing outside the declared keys may change (a migration must not quietly drop or rewrite other settings)
        auto undeclared = [&](const std::wstring& k) { return !migdetail::Covered(k, s->touches); };
        for (const auto& kv : before) { auto f = work.find(kv.first); if ((f == work.end() || f->second != kv.second) && undeclared(kv.first)) { r.outcome = SettingsMigrationOutcome::Failed; r.error = std::wstring(L"Migration step \"") + Utf8ToW(s->id) + L"\" changed the setting \"" + kv.first + L"\", which it did not declare."; return r; } }
        for (const auto& kv : work) if (!before.count(kv.first) && undeclared(kv.first)) { r.outcome = SettingsMigrationOutcome::Failed; r.error = std::wstring(L"Migration step \"") + Utf8ToW(s->id) + L"\" added the setting \"" + kv.first + L"\", which it did not declare."; return r; }
        work[kVer] = std::to_wstring(s->to);   // the runner owns the version key
        r.stepsApplied.push_back(Utf8ToW(s->id));
    }
    auto fin = work.find(kVer);
    int got = -1;
    if (fin == work.end() || !migdetail::ParseVersion(fin->second, got) || got != target) { r.outcome = SettingsMigrationOutcome::Failed; r.error = L"The migrated settings do not carry the target version."; return r; }
    values = std::move(work);
    r.outcome = SettingsMigrationOutcome::Applied;
    return r;
}

// One log line per migration that did something (the caller owns logging; the runner is pure).
inline void LogSettingsMigration(const SettingsMigrationReport& r) {
    if (r.outcome == SettingsMigrationOutcome::NotNeeded && r.notes.empty()) return;
    std::wstring t = L"Settings: " + std::wstring(SettingsMigrationOutcomeLabel(r.outcome)) + L" (version " + std::to_wstring(r.fromVersion) + L" -> " + std::to_wstring(r.toVersion) + L")";
    if (r.backupMade) t += L"; a copy of the file was kept at " + r.backupPath;
    if (!r.error.empty()) t += L". " + r.error;
    for (const auto& n : r.notes) t += L" " + n;
    if (r.outcome == SettingsMigrationOutcome::Failed || r.outcome == SettingsMigrationOutcome::RefusedNewer) LogW(t); else LogI(t);
}

}  // namespace ak
