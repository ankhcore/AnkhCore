// AnkhCore 2.6 — M-2.6-UPD1: software updater with automatic pre-update backup, verification and honest rollback.
// Built only on the BAK1 engine (src/eng_backup.h): AcbCreateVerifiedRecoveryPoint, AcbVerifyRecoveryPoint,
// AcbDetectUniGetUI, AcbClassifyUniGetExit, AcbBuildUniGetInstallArgs, AcbPackage. There is no second backup format.
//
// What a "backup" is here, stated plainly: the recovery point holds AnkhCore's OWN settings (settings.json). It does not
// contain the updated application, its installer, or its data. Application data can be backed up only through an explicit
// per-application data map (owner question Q5, not defined yet); until then the updater says so and never claims that
// application data can be restored. Reinstalling an older version and restoring data are reported as two separate things.
//
// Every external effect goes through UpdProvider (command runner, installed-version query, optional data backup/restore).
// Automated tests inject mocks; the live provider (UpdMakeUniGetProvider) refuses to run in the isolated test build.
//
// Nothing here starts by itself: a caller (a user action with a confirmation naming package, version and source) invokes UpdRun.
#pragma once
#include "eng_backup.h"

namespace ak {

// ---------------------------------------------------------------- request, policy, versions
struct UpdPackage {
    std::string id;
    std::string name;
    std::string manager;           // UniGetUI manager id: winget, scoop, chocolatey, pip, npm, ...
    std::string source;
    std::string previousVersion;   // installed version before the update (detected when empty)
    std::string targetVersion;     // requested version; empty = "latest the manager offers"
    bool highImpact = false;       // the caller knows this update migrates data or drivers
    bool allowUnknownBefore = false;   // 2.8.6: the person chose "Update anyway" for a program whose installed version the listing shows as "Unknown"
    bool beforeUnknown = false;        // set by UpdRun when it went ahead without a confirmed version before (no rollback target)
};

enum class UpdBackupPolicy {
    Always,           // every update needs a verified recovery point first (default)
    HighImpactOnly    // only high-impact, major-version, unknown-version or non-comparable updates
};

// Identifiers reach a command line: only a conservative character set is accepted (no quotes, spaces, shell syntax).
inline bool UpdSafeToken(const std::string& s, bool allowEmpty) {
    if (s.empty()) return allowEmpty;
    if (s.size() > 200) return false;
    if (s[0] == '-') return false;   // an option lookalike (--skip-hash) must never be taken as a value
    for (const unsigned char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '.' || c == '_' || c == '-' || c == '+' || c == ':' || c == '/' || c == '@';
        if (!ok) return false;
    }
    if (s.find("..") != std::string::npos) return false;
    return true;
}

inline bool UpdValidatePackage(const UpdPackage& p, std::wstring& err) {
    if (!UpdSafeToken(p.id, false)) { err = L"The package id is empty or contains characters that are not allowed."; return false; }
    if (!UpdSafeToken(p.manager, false)) { err = L"The package manager id is empty or contains characters that are not allowed."; return false; }
    if (!UpdSafeToken(p.source, true) || !UpdSafeToken(p.previousVersion, true) || !UpdSafeToken(p.targetVersion, true)) {
        err = L"The source or a version contains characters that are not allowed.";
        return false;
    }
    return true;
}

// "1.2.3", "v1.2", "24.08.1-x64", "1.0-beta": a numeric core (up to 16 parts) plus an optional lower-case suffix.
// Text without a numeric core does not parse.
inline bool UpdParseVersionEx(const std::string& v, std::vector<unsigned long long>& parts, std::string& suffix) {
    parts.clear(); suffix.clear();
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    if (i >= v.size() || v[i] < '0' || v[i] > '9') return false;
    unsigned long long cur = 0; bool digits = false;
    for (;; ++i) {
        const bool end = i == v.size();
        const char c = end ? '\0' : v[i];
        if (!end && c >= '0' && c <= '9') {
            if (cur > 99999999999ull) return false;
            cur = cur * 10 + static_cast<unsigned>(c - '0'); digits = true;
        } else if (!end && c == '.') {
            if (!digits || parts.size() >= 16) return false;
            parts.push_back(cur); cur = 0; digits = false;
        } else {   // end of the numeric core
            if (!digits) return false;   // "1.2." or "1..2"
            if (parts.size() >= 16) return false;
            parts.push_back(cur);
            if (!end) { suffix = v.substr(i); for (auto& ch : suffix) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch))); }
            return true;
        }
    }
}
inline bool UpdParseVersion(const std::string& v, std::vector<unsigned long long>& parts) {
    std::string sfx;
    return UpdParseVersionEx(v, parts, sfx);
}

// A suffix that marks a pre-release (1.0-beta is older than 1.0). Anything else (-x64, a, -hotfix) has no known order.
inline bool UpdIsPreReleaseSuffix(std::string s) {
    while (!s.empty() && (s[0] == '-' || s[0] == '.' || s[0] == '_' || s[0] == '+')) s.erase(0, 1);
    static const char* words[] = {"alpha", "beta", "rc", "pre", "preview", "dev", "snapshot", "nightly", "canary"};
    for (const char* w : words) if (s.rfind(w, 0) == 0) return true;
    return false;
}

// -1 / 0 / 1, or -2 when the two cannot be ordered (identical text still compares equal).
// The numeric core decides first (24.08 < 24.09-hotfix). With equal cores only "pre-release < release" is known.
inline int UpdCompareVersions(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    std::vector<unsigned long long> pa, pb; std::string sa, sb;
    if (!UpdParseVersionEx(a, pa, sa) || !UpdParseVersionEx(b, pb, sb)) return -2;
    const size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned long long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa == sb) return 0;
    if (sa.empty() && UpdIsPreReleaseSuffix(sb)) return 1;
    if (sb.empty() && UpdIsPreReleaseSuffix(sa)) return -1;
    return -2;
}

inline bool UpdBackupRequired(UpdBackupPolicy policy, const UpdPackage& p, std::wstring& why) {
    if (policy == UpdBackupPolicy::Always) { why = L"policy: always back up before updating"; return true; }
    if (p.highImpact) { why = L"marked high impact"; return true; }
    if (p.previousVersion.empty() || p.targetVersion.empty()) { why = L"the version change is not known in advance"; return true; }
    std::vector<unsigned long long> a, b;
    if (!UpdParseVersion(p.previousVersion, a) || !UpdParseVersion(p.targetVersion, b)) { why = L"the versions cannot be compared"; return true; }
    if (a[0] != b[0]) { why = L"major version change"; return true; }
    why = L"minor update within the same major version; policy backs up high-impact updates only";
    return false;
}

// ---------------------------------------------------------------- rollback classification
enum class UpdRollbackCapability {
    Unsupported,   // neither the previous version can be reinstalled nor application data restored
    Partial,       // exactly one of the two is possible
    Full           // previous version reinstall AND verified application-data restore
};
inline const wchar_t* UpdRollbackCapabilityLabel(UpdRollbackCapability c) {
    return c == UpdRollbackCapability::Full ? L"Fully supported" : c == UpdRollbackCapability::Partial ? L"Partially supported" : L"Unsupported";
}

struct UpdRollbackAssessment {
    UpdRollbackCapability capability = UpdRollbackCapability::Unsupported;
    bool canReinstallPrevious = false;   // an older version can be requested from the package manager (verified afterwards)
    bool canRestoreAppData = false;      // the application's own data was backed up AND verified before the update
    std::wstring summary;
};

// Managers whose install command accepts an explicit version (UniGetUI `--version`). scoop, pwsh, winps, vcpkg and bun are
// left out: a pinned older version is not a reliable operation there, so no capability is claimed for them.
inline bool UpdManagerCanPinVersion(const std::string& manager) {
    static const char* ok[] = {"winget", "chocolatey", "pip", "npm", "dotnet-tool", "cargo"};
    for (const char* m : ok) if (manager == m) return true;
    return false;
}

// Capability from what can ACTUALLY be done: a mechanism that cannot run is never counted (R3).
//   reinstallMechanism = a working way to request the previous version AND to read the installed version back;
//   dataBackedUpAndVerified + restoreMechanism = application data was saved and verified AND a restore function exists.
// Full needs both halves; either half alone is Partial; neither is Unsupported.
inline UpdRollbackAssessment UpdAssessRollback(const UpdPackage& p, bool reinstallMechanism, bool dataBackedUpAndVerified, bool restoreMechanism) {
    UpdRollbackAssessment a;
    a.canReinstallPrevious = reinstallMechanism && !p.previousVersion.empty() && UpdManagerCanPinVersion(p.manager);
    a.canRestoreAppData = dataBackedUpAndVerified && restoreMechanism;
    if (a.canReinstallPrevious && a.canRestoreAppData) a.capability = UpdRollbackCapability::Full;
    else if (a.canReinstallPrevious || a.canRestoreAppData) a.capability = UpdRollbackCapability::Partial;
    else a.capability = UpdRollbackCapability::Unsupported;
    std::wstring s;
    s += a.canReinstallPrevious ? L"The previous version can be requested again from the package manager (checked afterwards; it may be refused). "
                                : L"The previous version cannot be reinstalled automatically. ";
    s += a.canRestoreAppData ? L"The application's data was backed up and verified, and a restore function exists. "
                             : L"The application's own data cannot be restored (no verified data backup and restore function; no data map is defined). ";
    s += L"The recovery point holds AnkhCore's own settings only.";
    a.summary = s;
    return a;
}

// ---------------------------------------------------------------- provider (all external effects)
struct UpdProvider {
    bool uniGetAvailable = false;
    // Optional gate checked at the start of every update and rollback (UPD2-A: "is the private UniGetUI session valid right now?").
    // Returning false blocks the operation before any command runs; `why` is shown to the user.
    std::function<bool(std::wstring& why)> preflight;
    // Runs `uniget <args>`; returns the process exit code (or (DWORD)-1 when it could not run) and its output lines.
    std::function<DWORD(const std::wstring& args, std::vector<std::wstring>& output)> run;
    // Reads the installed version; false = cannot be confirmed.
    std::function<bool(const UpdPackage&, std::string& version)> installedVersion;
    // Optional per-application data backup (needs a data map, Q5). Must verify what it wrote. Absent = not backed up.
    std::function<bool(const UpdPackage&, std::wstring& detail)> backupAppData;
    // Optional restore of that data. Absent = cannot restore.
    std::function<bool(const UpdPackage&, std::wstring& detail)> restoreAppData;
    // Optional: confirms NOW that a verified data backup exists (used by a stand-alone rollback; a caller's claim is not enough).
    std::function<bool(const UpdPackage&, std::wstring& detail)> verifyAppDataBackup;
    // Optional detailed form of installedVersion: also says WHY a version could not be confirmed (shown in the update record).
    std::function<bool(const UpdPackage&, std::string& version, std::wstring& why)> readInstalledVersion;
    // 2.8.7: asks the package manager to read its installed list again (UniGetUI keeps one in memory; right after an update it can still be the old one).
    std::function<void(const UpdPackage&)> refreshInstalled;
    // 2.8.7: asks UniGetUI to cancel the running operation of this package. The installer it started may already have changed things, so the caller reads the version afterwards.
    std::function<bool(const UpdPackage&, std::wstring& note)> cancelRunning;
};

inline bool UpdCanReinstall(const UpdProvider& pr) { return pr.uniGetAvailable && pr.run && pr.installedVersion; }
inline UpdRollbackAssessment UpdAssessRollbackFor(const UpdPackage& p, const UpdProvider& pr, bool dataBackedUpAndVerified) {
    return UpdAssessRollback(p, UpdCanReinstall(pr), dataBackedUpAndVerified, static_cast<bool>(pr.restoreAppData));
}

// The isolated test build may never run a live package command.
inline bool UpdLiveExecutionAllowed(bool testBuild) { return !testBuild; }

inline std::wstring UpdQuote(const std::string& s) { return L"\"" + Utf8ToW(s) + L"\""; }

inline std::wstring UpdBuildUniGetUpdateArgs(const UpdPackage& p) {
    std::wstring a = L"package update --id " + UpdQuote(p.id) + L" --manager " + UpdQuote(p.manager);
    if (!p.source.empty()) a += L" --source " + UpdQuote(p.source);
    if (!p.targetVersion.empty()) a += L" --version " + UpdQuote(p.targetVersion);
    a += L" --wait true";   // never --skip-hash, never --elevated or --interactive: those are the user's explicit choices only
    return a;
}

inline std::wstring UpdBuildUniGetInstalledArgs(const UpdPackage& p) {
    return L"package installed --manager " + UpdQuote(p.manager);
}

// ---------------------------------------------------------------- result classification (pure)
enum class UpdOutcome {
    Succeeded,
    PartiallySucceeded,
    Failed,
    AbortedBeforeUpdate,          // nothing was changed: a precondition or the backup failed
    Busy,                         // another update is running
    InterruptedPreviousUpdate,    // an earlier update never finished; acknowledge it first
    InvalidRequest,
    TimedOut                      // the command did not finish in time: the operation may still be running; the lock is kept
};
inline const wchar_t* UpdOutcomeLabel(UpdOutcome o) {
    switch (o) {
    case UpdOutcome::Succeeded: return L"Succeeded";
    case UpdOutcome::PartiallySucceeded: return L"Partially succeeded";
    case UpdOutcome::Failed: return L"Failed";
    case UpdOutcome::AbortedBeforeUpdate: return L"Aborted before updating";
    case UpdOutcome::Busy: return L"Another update is running";
    case UpdOutcome::InterruptedPreviousUpdate: return L"A previous update was interrupted";
    case UpdOutcome::TimedOut: return L"Timed out; final state unknown";
    default: return L"Invalid request";
    }
}

struct UpdVerdict {
    UpdOutcome outcome = UpdOutcome::Failed;
    bool versionChanged = false;
    bool reachedTarget = false;
    bool needsRollback = false;   // the installed version is neither the old one nor an acceptable new one
    std::wstring reason;
};

inline std::wstring UpdExitReason(DWORD code) {
    if (code == kRunTimedOut) return L"the command timed out (still running when the wait ended); the operation may still be running in the UniGetUI session";
    if (code == kRunLaunchFailed) return L"the command could not be run";
    switch (AcbClassifyUniGetExit(code)) {
    case AcbUniGetExit::Success: return L"the update command reported success";
    case AcbUniGetExit::CommandFailed: return L"the update command failed (exit 1)";
    case AcbUniGetExit::InvalidParam: return L"this UniGetUI build rejected the command (exit 2); the feature is unavailable";
    case AcbUniGetExit::IpcUnavailable: return L"no UniGetUI session is running (exit 3); the update was not started";
    case AcbUniGetExit::UnknownCommand: return L"this UniGetUI build does not know the command (exit 4); the feature is unavailable";
    default: return L"the update command ended with an unexpected exit code " + std::to_wstring(code);
    }
}

// An exit code of 0 alone is never success: the installed version must be confirmed and must have advanced.
// Version rules (R4): "latest" (no target) succeeds on any confirmed NEWER version; a rollback is requested only for a
// confirmed version that is LOWER than the one before. A changed version that cannot be ordered, or one newer than the
// requested target, is a partial success, never a reason to reinstall the old version.
inline UpdVerdict UpdClassifyResult(const UpdPackage& p, DWORD exitCode, bool versionConfirmed, const std::string& after) {
    UpdVerdict v;
    const bool exitOk = exitCode == 0;
    if (versionConfirmed && p.beforeUnknown && exitOk) {   // 2.8.6: the version before was "Unknown": a confirmed version after a successful command is the best evidence there is
        v.outcome = UpdOutcome::Succeeded; v.versionChanged = true; v.reachedTarget = true;
        v.reason = L"The installed version is now " + Utf8ToW(after) + L". It was listed as Unknown before, so it could not be compared with the old one.";
        return v;
    }
    if (!versionConfirmed) {
        if (exitOk) { v.outcome = UpdOutcome::PartiallySucceeded; v.reason = L"The command reported success but the installed version could not be confirmed, so success is not claimed."; }
        else { v.outcome = UpdOutcome::Failed; v.reason = UpdExitReason(exitCode) + L"; the installed version could not be confirmed."; }
        return v;
    }
    const int vsPrev = p.previousVersion.empty() ? -3 : UpdCompareVersions(after, p.previousVersion);
    const int vsTarget = p.targetVersion.empty() ? -3 : UpdCompareVersions(after, p.targetVersion);
    if (vsPrev == 0) {   // still the old version
        v.outcome = UpdOutcome::Failed;
        v.reason = exitOk ? L"The command reported success but the installed version did not change."
                          : UpdExitReason(exitCode) + L"; the installed version is unchanged.";
        return v;
    }
    v.versionChanged = true;
    const std::wstring wa = Utf8ToW(after);
    if (vsTarget == 0 || (p.targetVersion.empty() && vsPrev == 1)) v.reachedTarget = true;
    if (v.reachedTarget) {
        if (exitOk) { v.outcome = UpdOutcome::Succeeded; v.reason = L"The installed version is " + wa + L" as expected."; }
        else { v.outcome = UpdOutcome::PartiallySucceeded; v.reason = L"The installed version advanced to " + wa + L" but the command reported a problem (" + UpdExitReason(exitCode) + L")."; }
        return v;
    }
    if (vsPrev == -1) {   // confirmed lower than before: the only case that asks for a rollback
        v.outcome = UpdOutcome::Failed;
        v.needsRollback = true;
        v.reason = L"The installed version is " + wa + L", which is older than the version before the update.";
        return v;
    }
    v.outcome = UpdOutcome::PartiallySucceeded;
    if (vsPrev == 1 && vsTarget == -1) v.reason = L"The installed version advanced to " + wa + L" but not to the requested " + Utf8ToW(p.targetVersion) + L".";
    else if (vsTarget == 1) v.reason = L"The installed version is " + wa + L", newer than the requested " + Utf8ToW(p.targetVersion) + L".";
    else v.reason = L"The installed version changed to " + wa + L" but it cannot be compared with the previous or requested version, so the result is not confirmed.";
    return v;
}

// ---------------------------------------------------------------- B1: UniGetUI package listings (JSON)
// Verified against the supplied portable build (UniGetUI 2026.3.3, build 107, signed by Devolutions Inc.), read-only, in an
// isolated headless session (see ANKHCORE_2.6_UPD1_FINAL_AUDIT.md section 6b for exactly what was observed):
//   `package installed [--manager m]` -> exit 0, stdout one JSON document (indented, CRLF, no BOM, pure ASCII: non-ASCII is
//   written as \uXXXX), shape {"status":"success","packages":[{name,id,version,newVersion,source,manager,isUpgradable,
//   systemRestartPending}, ...]};   `package updates` -> the same entries under "updates" instead of "packages";
//   a failed command -> exit 2 and {"status":"error","command":"","message":"..."}.
// Also observed (so they are handled, never trusted): an unknown command exits 0 with NO output; with no running session
// `status` exits 0 with "running": false, other commands exit 2 ("The IPC API token is not available"); a version field can
// be "Unknown" or free text (e.g. "ad 9.8.3"), which is not a version and is never used.
enum class UpdListStatus {
    Ok,              // a success envelope with a packages/updates array (possibly empty)
    Empty,           // no output at all (what an unknown command prints, with exit 0)
    Malformed,       // not JSON, truncated JSON, or the array has the wrong type
    Unsupported,     // valid JSON of a shape this parser does not know
    ErrorEnvelope    // {"status":"error", ...}
};

struct UpdListedPackage {
    std::string id, name, version, newVersion, source, manager;
    bool isUpgradable = false;
};

struct UpdPackageListing {
    UpdListStatus status = UpdListStatus::Empty;
    std::string arrayKey;               // "packages" (installed) or "updates"
    std::wstring message;               // the error text, or why the output was not accepted
    std::vector<UpdListedPackage> packages;
    size_t skipped = 0;                 // array items that were not usable objects with an id
};

constexpr size_t kUpdMaxListingBytes = 8u << 20;

inline std::wstring UpdShort(const std::string& s, size_t maxLen = 200) {
    std::string t;
    for (const unsigned char c : s) { if (t.size() >= maxLen) break; t += (c < 0x20) ? ' ' : static_cast<char>(c); }
    return Utf8ToW(t);
}

inline UpdPackageListing UpdParsePackageListing(const std::string& textIn) {
    UpdPackageListing r;
    std::string text = textIn;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.erase(0, 3);
    bool blank = true;
    for (const unsigned char c : text) if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) { blank = false; break; }
    if (blank) { r.status = UpdListStatus::Empty; r.message = L"UniGetUI printed nothing."; return r; }
    if (text.size() > kUpdMaxListingBytes) { r.status = UpdListStatus::Unsupported; r.message = L"The listing is larger than the accepted limit."; return r; }
    JVal root;
    if (!JsonParse(text, root)) { r.status = UpdListStatus::Malformed; r.message = L"The output is not a complete, valid JSON document."; return r; }
    if (!root.Is(JVal::Obj)) { r.status = UpdListStatus::Unsupported; r.message = L"The JSON document is not an object."; return r; }
    const JVal* st = root.Get("status");
    if (!st || !st->Is(JVal::Str)) { r.status = UpdListStatus::Unsupported; r.message = L"The JSON document has no status field."; return r; }
    if (st->s == "error") {
        r.status = UpdListStatus::ErrorEnvelope;
        const JVal* m = root.Get("message");
        r.message = (m && m->Is(JVal::Str)) ? UpdShort(m->s, 300) : std::wstring(L"UniGetUI reported an error.");
        return r;
    }
    if (st->s != "success") { r.status = UpdListStatus::Unsupported; r.message = L"The status \"" + UpdShort(st->s, 40) + L"\" is not recognised."; return r; }
    const JVal* arr = root.Get("packages");
    r.arrayKey = "packages";
    if (!arr) { arr = root.Get("updates"); r.arrayKey = "updates"; }
    if (!arr) { r.arrayKey.clear(); r.status = UpdListStatus::Unsupported; r.message = L"The success document has no packages or updates list."; return r; }
    if (!arr->Is(JVal::Arr)) { r.status = UpdListStatus::Malformed; r.message = L"The packages list is not an array."; return r; }
    for (const JVal& it : arr->a) {
        if (!it.Is(JVal::Obj)) { ++r.skipped; continue; }
        auto str = [&](const char* k) { const JVal* v = it.Get(k); return (v && v->Is(JVal::Str)) ? v->s : std::string(); };
        UpdListedPackage p;
        p.id = str("id"); p.name = str("name"); p.version = str("version"); p.newVersion = str("newVersion");
        p.source = str("source"); p.manager = str("manager");
        if (const JVal* u = it.Get("isUpgradable"); u && u->Is(JVal::Bool)) p.isUpgradable = u->b;
        if (p.id.empty()) { ++r.skipped; continue; }
        r.packages.push_back(std::move(p));
    }
    r.status = UpdListStatus::Ok;
    return r;
}

// RunCapture hands the output over as trimmed lines; the JSON is the lines joined again.
inline UpdPackageListing UpdParsePackageListingLines(const std::vector<std::wstring>& lines) {
    std::string t;
    for (const std::wstring& l : lines) { t += WToUtf8(l); t += '\n'; }
    return UpdParsePackageListing(t);
}

enum class UpdLookup {
    Found,              // exactly one installed entry, with a usable (numeric) version
    NotListed,          // a good listing that does not contain the package
    Ambiguous,          // several entries with different versions and no way to tell which is meant
    VersionUnusable,    // listed, but the version is missing, "Unknown" or free text
    ListingUnusable     // the output could not be used at all (empty, malformed, unsupported, error, wrong list)
};
struct UpdVersionLookup {
    UpdLookup result = UpdLookup::ListingUnusable;
    std::string version;
    std::wstring why;
};

// The installed version of `p` from an `installed` listing. Never guesses: anything doubtful is not "Found".
inline UpdVersionLookup UpdFindInstalledVersion(const UpdPackageListing& l, const UpdPackage& p) {
    UpdVersionLookup r;
    if (l.status != UpdListStatus::Ok) {
        r.why = l.status == UpdListStatus::Empty ? L"UniGetUI printed nothing, so the installed version could not be read."
              : l.status == UpdListStatus::ErrorEnvelope ? L"UniGetUI reported an error: " + l.message
              : L"The installed-package listing was not understood: " + l.message;
        return r;
    }
    if (l.arrayKey != "packages") { r.why = L"The listing is not an installed-packages listing."; return r; }
    const std::wstring wid = Lower(Utf8ToW(p.id)), wmgr = Lower(Utf8ToW(p.manager)), wsrc = Lower(Utf8ToW(p.source));
    std::vector<const UpdListedPackage*> hit;
    for (const UpdListedPackage& e : l.packages)
        if (Lower(Utf8ToW(e.id)) == wid && Lower(Utf8ToW(e.manager)) == wmgr) hit.push_back(&e);
    if (hit.empty()) { r.result = UpdLookup::NotListed; r.why = L"The package is not in the installed list of this manager."; return r; }
    if (hit.size() > 1 && !wsrc.empty()) {   // several entries: narrow by an exactly matching source if that is possible
        std::vector<const UpdListedPackage*> bySrc;
        for (const auto* e : hit) if (Lower(Utf8ToW(e->source)) == wsrc) bySrc.push_back(e);
        if (!bySrc.empty()) hit = bySrc;
    }
    for (const auto* e : hit)
        if (e->version != hit[0]->version) { r.result = UpdLookup::Ambiguous; r.why = L"The package appears several times with different versions."; return r; }
    std::vector<unsigned long long> parts;
    if (!UpdParseVersion(hit[0]->version, parts)) {
        r.result = UpdLookup::VersionUnusable;
        r.why = hit[0]->version.empty() ? std::wstring(L"The listing has no version for this package.")
                                        : L"The listed version \"" + UpdShort(hit[0]->version, 60) + L"\" is not a comparable version.";
        return r;
    }
    r.result = UpdLookup::Found;
    r.version = hit[0]->version;
    return r;
}

// Builds a provider on top of a command runner (live or mock). The installed version is read ONLY from the JSON listing
// of `package installed --manager <m>` and only when the command succeeded; anything else is "not confirmed".
// 2.8.7: the operations of a `uniget operation list` answer that belong to one package. The shape of that answer is not documented, so this is tolerant and narrow:
// an object counts only when it has a whole-number "id" AND some text value that contains the package id.
inline void UpdCollectOpIds(const JVal& v, const std::string& lowId, std::vector<unsigned long long>& out) {
    if (v.t == JVal::Obj) {
        unsigned long long id = 0; bool haveId = false, mentions = false;
        if (const JVal* i = v.Get("id")) {
            uint64_t u = 0;
            if (i->AsUInt(u)) { id = u; haveId = true; }
            else if (i->Is(JVal::Str) && !i->s.empty() && i->s.size() < 10 && i->s.find_first_not_of("0123456789") == std::string::npos) { id = std::strtoull(i->s.c_str(), nullptr, 10); haveId = true; }
        }
        for (const JVal& c : v.a) if (c.Is(JVal::Str)) { std::string l = c.s; for (char& ch : l) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); if (l.find(lowId) != std::string::npos) mentions = true; }
        if (haveId && mentions) out.push_back(id);
    }
    if (v.t == JVal::Obj || v.t == JVal::Arr) for (const JVal& c : v.a) if (c.t == JVal::Obj || c.t == JVal::Arr) UpdCollectOpIds(c, lowId, out);
}
inline std::vector<unsigned long long> UpdFindOperationIds(const std::vector<std::wstring>& lines, const std::string& packageId) {
    std::vector<unsigned long long> ids;
    if (packageId.empty()) return ids;
    std::string t; for (const std::wstring& l : lines) { t += WToUtf8(l); t += '\n'; }
    JVal root; if (!JsonParse(t, root)) return ids;
    std::string low = packageId; for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    UpdCollectOpIds(root, low, ids);
    std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

inline UpdProvider UpdMakeUniGetProviderWith(bool uniGetAvailable,
                                             const std::function<DWORD(const std::wstring&, std::vector<std::wstring>&)>& run) {
    UpdProvider pr;
    pr.uniGetAvailable = uniGetAvailable;
    pr.run = run;
    auto read = [run](const UpdPackage& p, std::string& v, std::wstring& why) -> bool {
        v.clear();
        std::vector<std::wstring> out;
        const DWORD rc = run(UpdBuildUniGetInstalledArgs(p), out);
        const UpdPackageListing l = UpdParsePackageListingLines(out);
        if (rc != 0) {
            why = UpdExitReason(rc);
            if (l.status == UpdListStatus::ErrorEnvelope) why += L": " + l.message;
            return false;
        }
        const UpdVersionLookup lk = UpdFindInstalledVersion(l, p);
        why = lk.why;
        if (lk.result != UpdLookup::Found) return false;
        v = lk.version;
        return true;
    };
    pr.readInstalledVersion = read;
    pr.installedVersion = [read](const UpdPackage& p, std::string& v) { std::wstring w; return read(p, v, w); };
    pr.refreshInstalled = [run](const UpdPackage& p) { std::vector<std::wstring> o; run(L"manager reload --manager " + UpdQuote(p.manager), o); };   // an answer is not needed: a failure only means the list was not refreshed
    pr.cancelRunning = [run](const UpdPackage& p, std::wstring& note) -> bool {
        std::vector<std::wstring> o;
        if (run(L"operation list", o) != 0) { note = L"UniGetUI did not list its operations, so nothing was cancelled."; return false; }
        const std::vector<unsigned long long> ids = UpdFindOperationIds(o, p.id);
        if (ids.empty()) { note = L"UniGetUI shows no running operation for this program (it may have just finished)."; return false; }
        bool any = false;
        for (const unsigned long long id : ids) { std::vector<std::wstring> o2; if (run(L"operation cancel --id " + std::to_wstring(id), o2) == 0) any = true; }
        note = any ? L"The cancel request was sent to UniGetUI." : L"UniGetUI refused the cancel request.";
        return any;
    };
    // No backupAppData / restoreAppData / verifyAppDataBackup: there is no per-application data map yet (Q5).
    return pr;
}

inline UpdProvider UpdMakeUniGetProvider(const AcbUniGetInstall& ug, DWORD timeoutMs = 15 * 60 * 1000) {
    const std::wstring cli = ug.cliExe;
    return UpdMakeUniGetProviderWith(ug.found, [cli, timeoutMs](const std::wstring& args, std::vector<std::wstring>& out) -> DWORD {
        if (!UpdLiveExecutionAllowed(kTestBuild)) { TestRefused(L"uniget " + args); return kRunLaunchFailed; }
        return RunCapture(cli, args, [&out](const std::wstring& l) { out.push_back(l); }, timeoutMs);
    });
}

// ---------------------------------------------------------------- state, lock and history
struct UpdStepRecord {
    std::wstring step;
    bool ok = false;
    std::wstring detail;
    std::string utc;
};

struct UpdActiveInfo {
    std::string id, package, manager, previousVersion, targetVersion, phase, started, recoveryDir;
    DWORD pid = 0;
    std::string pidStart;       // the owner's process creation time (FILETIME, decimal): a recycled pid is not the same owner
    bool unresolved = false;    // a command timed out: the operation may still be running; only an explicit resolution releases this
    std::vector<UpdStepRecord> steps;
};

inline std::wstring UpdDir(const AcbRoots& r) { return r.data + L"\\Updater"; }
inline std::wstring UpdActivePath(const AcbRoots& r) { return UpdDir(r) + L"\\active-update.json"; }
inline std::wstring UpdHistoryDir(const AcbRoots& r) { return UpdDir(r) + L"\\history"; }

// Identifiers that come back from disk are untrusted: an id becomes part of a file name, so only the format AnkhCore
// writes (digits, letters, '-', '_'; starts with a letter or digit; at most 64) is accepted. No dots, separators or colons.
inline bool UpdValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!(alnum || (i > 0 && (c == '-' || c == '_')))) return false;
    }
    return true;
}
inline bool UpdPlainText(const std::string& s, size_t maxLen) {
    if (s.size() > maxLen) return false;
    for (const unsigned char c : s) if (c < 0x20) return false;
    return true;
}

inline std::string UpdActiveJson(const UpdActiveInfo& a) {
    std::string s = "{\r\n  \"formatVersion\": 1,\r\n"
        "  \"id\": \"" + JsonEsc(a.id) + "\",\r\n  \"package\": \"" + JsonEsc(a.package) + "\",\r\n"
        "  \"manager\": \"" + JsonEsc(a.manager) + "\",\r\n  \"previousVersion\": \"" + JsonEsc(a.previousVersion) + "\",\r\n"
        "  \"targetVersion\": \"" + JsonEsc(a.targetVersion) + "\",\r\n  \"phase\": \"" + JsonEsc(a.phase) + "\",\r\n"
        "  \"started\": \"" + JsonEsc(a.started) + "\",\r\n  \"pid\": " + std::to_string(a.pid) + ",\r\n" +
        (a.pidStart.empty() ? std::string() : "  \"pidStart\": \"" + JsonEsc(a.pidStart) + "\",\r\n") +
        "  \"unresolved\": " + (a.unresolved ? "true" : "false") + ",\r\n"
        "  \"recoveryDir\": \"" + JsonEsc(a.recoveryDir) + "\",\r\n  \"steps\": [\r\n";
    for (size_t i = 0; i < a.steps.size(); ++i) {
        const UpdStepRecord& t = a.steps[i];
        s += "    { \"step\": \"" + JsonEsc(WToUtf8(t.step)) + "\", \"ok\": " + (t.ok ? "true" : "false") +
             ", \"detail\": \"" + JsonEsc(WToUtf8(t.detail)) + "\", \"utc\": \"" + JsonEsc(t.utc) + "\" }" +
             (i + 1 < a.steps.size() ? ",\r\n" : "\r\n");
    }
    s += "  ]\r\n}\r\n";
    return s;
}

// Parses AND validates a marker. A marker whose identifiers do not match what AnkhCore writes is "unreadable": it still
// blocks new updates and is preserved, but none of its text is ever used as a path or a command.
inline bool UpdParseActive(const std::string& text, UpdActiveInfo& a) {
    a = UpdActiveInfo{};
    JVal r;
    if (text.empty() || !JsonParse(text, r) || !r.Is(JVal::Obj)) return false;
    auto str = [&](const char* k, std::string& out) { if (const JVal* v = r.Get(k); v && v->Is(JVal::Str)) out = v->s; };
    str("id", a.id); str("package", a.package); str("manager", a.manager); str("previousVersion", a.previousVersion);
    str("targetVersion", a.targetVersion); str("phase", a.phase); str("started", a.started); str("recoveryDir", a.recoveryDir);
    str("pidStart", a.pidStart);
    if (const JVal* p = r.Get("pid"); p && p->Is(JVal::Num) && p->n >= 0 && p->n < 4294967295.0) a.pid = static_cast<DWORD>(p->n);
    if (const JVal* u = r.Get("unresolved"); u && u->Is(JVal::Bool)) a.unresolved = u->b;
    if (const JVal* st = r.Get("steps"); st && st->Is(JVal::Arr)) {
        for (const JVal& it : st->a) {
            if (!it.Is(JVal::Obj)) continue;
            UpdStepRecord t;
            if (const JVal* x = it.Get("step"); x && x->Is(JVal::Str)) t.step = Utf8ToW(x->s);
            if (const JVal* x = it.Get("ok"); x && x->Is(JVal::Bool)) t.ok = x->b;
            if (const JVal* x = it.Get("detail"); x && x->Is(JVal::Str)) t.detail = Utf8ToW(x->s);
            if (const JVal* x = it.Get("utc"); x && x->Is(JVal::Str)) t.utc = x->s;
            a.steps.push_back(std::move(t));
        }
    }
    if (!UpdValidId(a.id)) return false;
    if (!UpdSafeToken(a.package, true) || !UpdSafeToken(a.manager, true) || !UpdSafeToken(a.previousVersion, true) || !UpdSafeToken(a.targetVersion, true)) return false;
    if (!UpdPlainText(a.phase, 32) || !UpdPlainText(a.started, 40) || !UpdPlainText(a.recoveryDir, 1000)) return false;
    for (const char c : a.pidStart) if (c < '0' || c > '9') return false;
    if (a.pidStart.size() > 20) return false;
    return true;
}

// ---- process identity: pid + creation time (a recycled pid is a different process)
inline bool UpdProcessStartString(DWORD pid, std::string& out) {
    out.clear();
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME c{}, e{}, k{}, u{};
    const bool ok = GetProcessTimes(h, &c, &e, &k, &u) != 0;
    CloseHandle(h);
    if (!ok) return false;
    out = std::to_string((static_cast<unsigned long long>(c.dwHighDateTime) << 32) | c.dwLowDateTime);
    return true;
}

inline bool UpdProcessAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // exists but is not ours to open: treat as alive (safe side)
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// Is the marker's owner still running? `override` (tests) answers by pid only. Without it: the pid must be running AND,
// when the marker recorded a start time, be the same process. An owner of 0 (an unresolved timeout) has no process.
inline bool UpdOwnerAlive(const UpdActiveInfo& o, const std::function<bool(DWORD)>& override) {
    if (o.pid == 0) return false;
    if (override) return override(o.pid);
    if (!UpdProcessAlive(o.pid)) return false;
    if (o.pidStart.empty()) return true;
    std::string now;
    if (!UpdProcessStartString(o.pid, now)) return true;   // cannot compare: stay on the safe side
    return now == o.pidStart;
}

// A directory AnkhCore creates for itself under the data root must be a plain directory, never a junction or symlink.
inline bool UpdPrepareDir(const std::wstring& dir) {
    EnsureDir(dir);
    const DWORD at = GetFileAttributesW(dir.c_str());
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return (at & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// The full path of "<historyDir>\<leaf>" if (and only if) it stays directly inside historyDir.
inline bool UpdContainedLeaf(const std::wstring& historyDir, const std::wstring& leaf, std::wstring& outPath) {
    outPath.clear();
    if (leaf.empty() || leaf.find_first_of(L"\\/:") != std::wstring::npos || leaf.find(L"..") != std::wstring::npos) return false;
    wchar_t hb[MAX_PATH * 2], fb[MAX_PATH * 2];
    const std::wstring cand = historyDir + L"\\" + leaf;
    if (!GetFullPathNameW(historyDir.c_str(), MAX_PATH * 2, hb, nullptr) || !GetFullPathNameW(cand.c_str(), MAX_PATH * 2, fb, nullptr)) return false;
    std::wstring h = hb, f = fb;
    while (!h.empty() && h.back() == L'\\') h.pop_back();
    if (f.size() <= h.size() + 1 || _wcsnicmp(f.c_str(), h.c_str(), h.size()) != 0 || f[h.size()] != L'\\') return false;
    if (f.find(L'\\', h.size() + 1) != std::wstring::npos) return false;   // exactly one level below
    outPath = f;
    return true;
}

// ---- the lock: <data>\Updater\active-update.json
enum class UpdLockResult { Created, Busy, Interrupted, Failed };
enum class UpdReadStatus { Ok, Missing, Locked, Empty };

// Reads the marker. A marker another process still has open (its creator is mid-write) is "Locked", never "unreadable".
inline UpdReadStatus UpdReadMarker(const std::wstring& path, std::string& text) {
    text.clear();
    bool sawLock = false, sawEmpty = false;
    for (int i = 0; i < 10; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return UpdReadStatus::Missing;
            sawLock = true; Sleep(30); continue;   // sharing violation / delete pending / access denied: held by someone
        }
        LARGE_INTEGER sz{};
        std::string buf;
        if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 22)) {
            buf.resize(static_cast<size_t>(sz.QuadPart));
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) got = 0;
            buf.resize(got);
        }
        CloseHandle(h);
        if (!buf.empty()) { text = buf; return UpdReadStatus::Ok; }
        sawEmpty = true; sawLock = false; Sleep(30);
    }
    return sawLock ? UpdReadStatus::Locked : (sawEmpty ? UpdReadStatus::Empty : UpdReadStatus::Locked);
}

// Race-safe: the full marker is written to a private temporary file and then moved into place WITHOUT replacing, so the
// marker either does not exist or is complete, and exactly one starter can win.
inline UpdLockResult UpdAcquire(const AcbRoots& roots, const UpdActiveInfo& info, UpdActiveInfo& existing,
                                const std::function<bool(DWORD)>& alive) {
    const std::wstring path = UpdActivePath(roots);
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"update marker " + path); return UpdLockResult::Failed; }
    if (!UpdPrepareDir(UpdDir(roots))) return UpdLockResult::Failed;
    const std::string body = UpdActiveJson(info);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::wstring tmp = UpdDir(roots) + L"\\active-update." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt) + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return UpdLockResult::Failed;
        DWORD wr = 0;
        const bool wrote = WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &wr, nullptr) && wr == body.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!wrote) { DeleteFileW(tmp.c_str()); return UpdLockResult::Failed; }
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return UpdLockResult::Created;   // no REPLACE_EXISTING: fails if a marker exists
        DeleteFileW(tmp.c_str());
        // somebody holds the lock: find out who
        existing = UpdActiveInfo{};
        std::string text;
        const UpdReadStatus st = UpdReadMarker(path, text);
        if (st == UpdReadStatus::Missing) continue;                  // it was released just now: try again
        if (st == UpdReadStatus::Locked) return UpdLockResult::Busy;  // open by its creator or in the middle of a swap: a live lock
        if (st == UpdReadStatus::Empty) return UpdLockResult::Interrupted;   // a zero-length marker is damaged, not live
        if (!UpdParseActive(text, existing)) return UpdLockResult::Interrupted;   // unreadable or hostile marker: never overwritten
        if (existing.unresolved) return UpdLockResult::Interrupted;
        return UpdOwnerAlive(existing, alive) ? UpdLockResult::Busy : UpdLockResult::Interrupted;
    }
    return UpdLockResult::Failed;
}

inline bool UpdSaveActive(const AcbRoots& roots, const UpdActiveInfo& info) {
    const std::string body = UpdActiveJson(info);
    return WriteAtomic(UpdActivePath(roots), reinterpret_cast<const uint8_t*>(body.data()), body.size());
}

// Finishes OUR update record: the final state goes to history, then the marker is released. It is released only if it
// is still ours (same id); a marker that is not ours is never touched.
inline bool UpdArchiveActive(const AcbRoots& roots, const UpdActiveInfo& info, const std::wstring& tag) {
    if (!UpdValidId(info.id) || !UpdPrepareDir(UpdHistoryDir(roots))) return false;
    std::wstring dst;
    if (!UpdContainedLeaf(UpdHistoryDir(roots), Utf8ToW(info.id) + L"-" + tag + L".json", dst)) return false;
    UpdActiveInfo cur;
    std::string text;
    if (UpdReadMarker(UpdActivePath(roots), text) == UpdReadStatus::Ok && UpdParseActive(text, cur) && cur.id != info.id) return false;
    const std::string body = UpdActiveJson(info);
    if (!WriteAtomic(dst, reinterpret_cast<const uint8_t*>(body.data()), body.size())) return false;
    return DeleteFileW(UpdActivePath(roots).c_str()) != 0;
}

enum class UpdAckResult {
    Acknowledged,        // the marker was moved into history; the lock is released
    NotFound,
    OwnerAlive,          // its owner is still running: refused
    Unresolved,          // a timed-out operation whose state is unknown: needs a verifier, or an explicit force
    StillMaybeRunning,   // the verifier found the package still listed as an operation: refused
    Failed
};

inline bool UpdListContainsId(const std::vector<std::wstring>& lines, const std::string& id) {
    const std::wstring wid = Lower(Utf8ToW(id));
    if (wid.empty()) return false;
    for (const std::wstring& l : lines) if (Lower(l).find(wid) != std::wstring::npos) return true;
    return false;
}

// The user's explicit acknowledgement of an unfinished update. It NEVER removes the lock of a live process, and it moves
// the marker (bytes unchanged) into <data>\Updater\history under a validated, contained name; the recovery point it
// names is left exactly as it is. A timed-out operation (unresolved) is released only after the verifier shows the
// package is not listed as a running operation (`uniget operation list`), or when `forceUnresolved` records an explicit
// user decision. A marker that cannot be parsed is preserved in history as "unreadable-<stamp>.json".
inline UpdAckResult UpdAcknowledge(const AcbRoots& roots, UpdActiveInfo* outInfo = nullptr,
                                   const std::function<bool(DWORD)>& alive = {},
                                   const UpdProvider* verifier = nullptr, bool forceUnresolved = false) {
    std::string text;
    const UpdReadStatus st = UpdReadMarker(UpdActivePath(roots), text);
    if (st == UpdReadStatus::Missing) return UpdAckResult::NotFound;
    if (st == UpdReadStatus::Locked) return UpdAckResult::OwnerAlive;   // open right now: somebody is writing it
    if (!UpdPrepareDir(UpdHistoryDir(roots))) return UpdAckResult::Failed;
    UpdActiveInfo a;
    std::wstring leaf, tag = L"interrupted-acknowledged";
    if (st == UpdReadStatus::Empty || !UpdParseActive(text, a)) {
        leaf = L"unreadable-" + AcbRecoveryFolderStamp() + L".json";
    } else {
        if (outInfo) *outInfo = a;
        if (a.unresolved) {
            if (forceUnresolved) tag = L"unresolved-forced";
            else if (verifier && verifier->run) {
                std::vector<std::wstring> out;
                const DWORD rc = verifier->run(L"operation list", out);
                if (rc != 0) return UpdAckResult::Unresolved;                       // cannot tell: stay blocked
                if (UpdListContainsId(out, a.package)) return UpdAckResult::StillMaybeRunning;
                tag = L"timeout-resolved";
            } else return UpdAckResult::Unresolved;
        } else if (UpdOwnerAlive(a, alive)) return UpdAckResult::OwnerAlive;
        leaf = Utf8ToW(a.id) + L"-" + tag + L".json";
    }
    std::wstring dst;
    if (!UpdContainedLeaf(UpdHistoryDir(roots), leaf, dst)) return UpdAckResult::Failed;
    if (FileExists(dst)) {   // never overwrite an earlier record
        if (!UpdContainedLeaf(UpdHistoryDir(roots), leaf.substr(0, leaf.size() - 5) + L"-" + AcbRecoveryFolderStamp() + L".json", dst)) return UpdAckResult::Failed;
    }
    return MoveFileExW(UpdActivePath(roots).c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH) ? UpdAckResult::Acknowledged : UpdAckResult::Failed;
}

// bool form: true only when the marker was released.
inline bool UpdAcknowledgeInterrupted(const AcbRoots& roots, UpdActiveInfo* outInfo = nullptr) {
    return UpdAcknowledge(roots, outInfo) == UpdAckResult::Acknowledged;
}

// ---------------------------------------------------------------- the update
enum class UpdPhase { Preparing, BackingUp, Updating, Verifying, RollingBack, Finished };
inline const char* UpdPhaseId(UpdPhase p) {
    switch (p) {
    case UpdPhase::Preparing: return "preparing"; case UpdPhase::BackingUp: return "backing-up"; case UpdPhase::Updating: return "updating";
    case UpdPhase::Verifying: return "verifying"; case UpdPhase::RollingBack: return "rolling-back"; default: return "finished";
    }
}

struct UpdContext {
    AcbRoots roots;
    UpdProvider provider;
    UpdBackupPolicy policy = UpdBackupPolicy::Always;
    bool autoRollback = false;    // when true, a "needs rollback" result runs UpdRollback immediately; default: only report
    bool journal = true;          // LOG-1 entries (tests pass false)
    DWORD pid = 0;                // 0 = this process
    int afterRetries = 0;         // 2.8.7: extra reads of the installed version after a successful command that could not be confirmed (the list can lag behind)
    DWORD afterRetryWaitMs = 0;
    std::function<bool(DWORD)> isProcessAlive;                 // default UpdProcessAlive
    // test seams
    std::function<void(const std::wstring&)> afterBackupSeal;  // runs after sealing, before the final backup verification
    std::function<bool(UpdPhase)> simulateCrashAt;             // true = stop here as if the process died (marker stays)
};

enum class UpdRollbackResult { NotNeeded, NotAttempted, Complete, Partial, Failed };
inline const wchar_t* UpdRollbackResultLabel(UpdRollbackResult r) {
    switch (r) {
    case UpdRollbackResult::NotNeeded: return L"Not needed";
    case UpdRollbackResult::NotAttempted: return L"Not attempted";
    case UpdRollbackResult::Complete: return L"Rolled back completely (previous version and data)";
    case UpdRollbackResult::Partial: return L"Rolled back partially";
    default: return L"Rollback failed";
    }
}

struct UpdReport {
    UpdOutcome outcome = UpdOutcome::InvalidRequest;
    std::wstring headline;
    std::vector<UpdStepRecord> steps;
    bool backupRequired = false, backupCreated = false, backupVerified = false, backupSealed = false;
    bool appDataBackedUp = false;
    std::wstring recoveryDir;
    bool commandRan = false;
    DWORD exitCode = 0;
    std::string versionBefore, versionAfter;
    bool versionAfterConfirmed = false;
    UpdVerdict verdict;
    UpdRollbackAssessment rollback;
    UpdRollbackResult rollbackResult = UpdRollbackResult::NotNeeded;
    bool binaryRestored = false, dataRestored = false;
    bool interrupted = false;         // simulateCrashAt fired
    bool timedOut = false;            // a command timed out: the lock is KEPT (unresolved) until the state is verified
    UpdActiveInfo previousActive;     // for InterruptedPreviousUpdate / Busy: what the old marker says
};

namespace upddetail {
struct RunState {
    UpdContext& ctx;
    UpdPackage pkg;
    UpdReport rep;
    UpdActiveInfo act;
    RunState(UpdContext& c, const UpdPackage& p) : ctx(c), pkg(p) {}

    void Step(const std::wstring& name, bool ok, const std::wstring& detail) {
        UpdStepRecord t; t.step = name; t.ok = ok; t.detail = detail; t.utc = AcbUtcStampNow();
        rep.steps.push_back(t); act.steps.push_back(t);
        UpdSaveActive(ctx.roots, act);   // the record survives a crash
    }
    void Phase(UpdPhase p) { act.phase = UpdPhaseId(p); UpdSaveActive(ctx.roots, act); }
    bool Crash(UpdPhase p) {
        if (ctx.simulateCrashAt && ctx.simulateCrashAt(p)) { rep.interrupted = true; return true; }
        return false;
    }
    void Journal(const std::wstring& kind, const std::wstring& title) {
        if (!ctx.journal) return;
        if (!kTestBuild || InTestArea(JournalPath())) JournalAdd(L"Updater", kind, title, {Utf8ToW(pkg.id), UpdOutcomeLabel(rep.outcome)});
    }
    // human-readable report next to the recovery point (if there is one): a separate file, the sealed manifest is untouched
    void WriteReport(UpdOutcome o, const std::wstring& headline) {
        if (rep.recoveryDir.empty() || !DirExists(rep.recoveryDir)) return;
        std::string t = "AnkhCore update report\r\nPackage: " + pkg.id + "\r\nOutcome: " + WToUtf8(UpdOutcomeLabel(o)) + "\r\n" + WToUtf8(headline) + "\r\n";
        for (const auto& s : rep.steps) t += std::string(s.ok ? "[ok]   " : "[FAIL] ") + WToUtf8(s.step) + ": " + WToUtf8(s.detail) + "\r\n";
        t += "Rollback: " + WToUtf8(UpdRollbackCapabilityLabel(rep.rollback.capability)) + " - " + WToUtf8(rep.rollback.summary) + "\r\n";
        WriteAtomic(rep.recoveryDir + L"\\update-report.txt", reinterpret_cast<const uint8_t*>(t.data()), t.size());
    }
    UpdReport& End(UpdOutcome o, const std::wstring& headline, const wchar_t* tag) {
        rep.outcome = o; rep.headline = headline;
        act.phase = UpdPhaseId(UpdPhase::Finished);
        WriteReport(o, headline);
        UpdArchiveActive(ctx.roots, act, tag);
        Journal(L"UPDATE", L"Update " + Utf8ToW(pkg.id) + L": " + UpdOutcomeLabel(o));
        return rep;
    }
    // A command timed out. RunCapture ends only the launcher; the operation itself runs in the UniGetUI session and may still
    // be going. So the lock is NOT released: the marker is left as "unresolved" with no owning process, which blocks every new
    // update until UpdAcknowledge (verifier or explicit force) resolves it. Nothing is rolled back or retried automatically.
    UpdReport& Unresolved(const std::wstring& headline) {
        act.unresolved = true; act.pid = 0; act.pidStart.clear(); act.phase = "operation-unknown";
        UpdSaveActive(ctx.roots, act);
        rep.outcome = UpdOutcome::TimedOut; rep.headline = headline; rep.timedOut = true;
        WriteReport(UpdOutcome::TimedOut, headline);
        Journal(L"UPDATE", L"Update " + Utf8ToW(pkg.id) + L": timed out, state unknown");
        return rep;
    }
};

// Reads the version; records it. confirmed=false means "unknown", never a guess. A value that is not a comparable
// version ("Unknown", free text) is not a confirmation either.
inline bool ReadVersion(RunState& r, const wchar_t* label, std::string& v, bool* listedUnusable = nullptr) {
    v.clear();
    if (listedUnusable) *listedUnusable = false;
    std::wstring why;
    bool ok = false;
    if (r.ctx.provider.readInstalledVersion) ok = r.ctx.provider.readInstalledVersion(r.pkg, v, why);
    else if (r.ctx.provider.installedVersion) ok = r.ctx.provider.installedVersion(r.pkg, v);
    if (!ok && listedUnusable && (why.find(L"is not a comparable version") != std::wstring::npos || why.find(L"has no version for this package") != std::wstring::npos)) *listedUnusable = true;   // listed, version unusable
    std::vector<unsigned long long> parts;
    if (ok && (v.empty() || !UpdParseVersion(v, parts))) { ok = false; why = L"the provider returned \"" + UpdShort(v, 60) + L"\", which is not a comparable version"; }
    if (!ok) v.clear();
    r.Step(label, ok, ok ? Utf8ToW(v) : (why.empty() ? std::wstring(L"the installed version could not be confirmed") : L"the installed version could not be confirmed: " + why));
    return ok;
}

// Runs the rollback plan that UpdAssessRollback declared; nothing else is ever touched.
inline UpdRollbackResult DoRollback(RunState& r) {
    UpdReport& rep = r.rep;
    r.Phase(UpdPhase::RollingBack);
    if (rep.rollback.capability == UpdRollbackCapability::Unsupported) {
        r.Step(L"rollback", false, L"not attempted: " + rep.rollback.summary);
        return UpdRollbackResult::NotAttempted;
    }
    bool binaryTried = false, dataTried = false;
    if (rep.rollback.canRestoreAppData && r.ctx.provider.restoreAppData) {
        dataTried = true;
        std::wstring d;
        rep.dataRestored = r.ctx.provider.restoreAppData(r.pkg, d);
        r.Step(L"rollback: restore application data", rep.dataRestored, d);
    }
    if (rep.rollback.canReinstallPrevious && r.ctx.provider.run) {
        binaryTried = true;
        AcbPackage ap; ap.id = r.pkg.id; ap.name = r.pkg.name; ap.version = r.pkg.previousVersion; ap.source = r.pkg.source; ap.managerName = r.pkg.manager;
        std::vector<std::wstring> out;
        const DWORD rc = r.ctx.provider.run(AcbBuildUniGetInstallArgs(ap, r.pkg.previousVersion, false, false), out);
        if (rc == kRunTimedOut) {   // the reinstall may still be running: do not read back, do not retry, keep the lock
            r.Step(L"rollback: reinstall previous version", false, UpdExitReason(rc));
            rep.timedOut = true;
            return UpdRollbackResult::Failed;
        }
        std::string v;
        const bool conf = r.ctx.provider.installedVersion && r.ctx.provider.installedVersion(r.pkg, v) && !v.empty();
        rep.binaryRestored = conf && UpdCompareVersions(v, r.pkg.previousVersion) == 0;   // only a confirmed read-back counts
        r.Step(L"rollback: reinstall previous version", rep.binaryRestored,
               rep.binaryRestored ? L"version " + Utf8ToW(v) + L" confirmed"
                                  : L"exit " + std::to_wstring(rc) + (conf ? L", installed version is " + Utf8ToW(v) : L", the installed version could not be confirmed"));
        if (conf) { rep.versionAfter = v; rep.versionAfterConfirmed = true; }
    }
    const bool bOk = rep.binaryRestored, dOk = rep.dataRestored;
    if (!binaryTried && !dataTried) return UpdRollbackResult::NotAttempted;
    if (bOk && dOk && rep.rollback.capability == UpdRollbackCapability::Full) return UpdRollbackResult::Complete;
    if (bOk || dOk) return UpdRollbackResult::Partial;   // a binary reinstall without data is always partial: it is said so
    return UpdRollbackResult::Failed;
}
}  // namespace upddetail

// Runs one update through every gate. Never executes anything except through ctx.provider.
inline UpdReport UpdRun(UpdContext& ctx, const UpdPackage& pkgIn) {
    using namespace upddetail;
    RunState r(ctx, pkgIn);
    UpdReport& rep = r.rep;
    std::wstring err;
    if (!UpdValidatePackage(r.pkg, err)) { rep.outcome = UpdOutcome::InvalidRequest; rep.headline = err; return rep; }

    // --- lock: one update at a time, and an unfinished earlier update must be acknowledged, never overwritten
    r.act.id = WToUtf8(AcbRecoveryFolderStamp());
    r.act.package = r.pkg.id; r.act.manager = r.pkg.manager; r.act.previousVersion = r.pkg.previousVersion;
    r.act.targetVersion = r.pkg.targetVersion; r.act.phase = UpdPhaseId(UpdPhase::Preparing); r.act.started = AcbUtcStampNow();
    r.act.pid = ctx.pid ? ctx.pid : GetCurrentProcessId();
    if (!ctx.pid) UpdProcessStartString(r.act.pid, r.act.pidStart);   // identifies THIS process, not just its pid
    const UpdLockResult lk = UpdAcquire(ctx.roots, r.act, rep.previousActive, ctx.isProcessAlive);
    if (lk == UpdLockResult::Busy) {
        rep.outcome = UpdOutcome::Busy;
        rep.headline = L"Another update" + (rep.previousActive.package.empty() ? std::wstring() : L" (" + Utf8ToW(rep.previousActive.package) + L")") + L" is in progress. Nothing was started.";
        return rep;
    }
    if (lk == UpdLockResult::Interrupted) {
        rep.outcome = UpdOutcome::InterruptedPreviousUpdate;
        if (rep.previousActive.unresolved)
            rep.headline = L"A previous update" + (rep.previousActive.package.empty() ? std::wstring() : L" of " + Utf8ToW(rep.previousActive.package)) +
                           L" timed out and its final state is unknown (its operation may still be running). Resolve it before starting another update. Nothing was started.";
        else
            rep.headline = L"A previous update" + (rep.previousActive.package.empty() ? std::wstring() : L" of " + Utf8ToW(rep.previousActive.package)) +
                           L" did not finish. Its record and recovery point are kept. Acknowledge it before starting another update. Nothing was started.";
        return rep;
    }
    if (lk == UpdLockResult::Failed) {
        rep.outcome = UpdOutcome::AbortedBeforeUpdate;
        rep.headline = L"The update state could not be recorded, so no update was started.";
        return rep;
    }
    r.Step(L"lock", true, L"update state recorded");
    if (r.Crash(UpdPhase::Preparing)) return rep;

    // --- pre-checks
    if (!ctx.provider.uniGetAvailable || !ctx.provider.run || !ctx.provider.installedVersion) {
        r.Step(L"pre-check", false, L"UniGetUI is not available, so no update can be run or verified");
        return r.End(UpdOutcome::AbortedBeforeUpdate, L"UniGetUI was not found. Nothing was changed.", L"aborted");
    }
    if (ctx.provider.preflight) {   // the UniGetUI session must be valid NOW; otherwise nothing runs
        std::wstring why;
        if (!ctx.provider.preflight(why)) {
            r.Step(L"pre-check", false, L"the UniGetUI session is not valid: " + why);
            return r.End(UpdOutcome::AbortedBeforeUpdate, L"The UniGetUI session is not valid, so no update was started. Nothing was changed. " + why, L"aborted");
        }
    }
    std::string before;
    bool beforeListedUnusable = false;
    const bool beforeOk = ReadVersion(r, L"installed version before", before, &beforeListedUnusable);
    if (beforeOk) {
        rep.versionBefore = before;
        if (!r.pkg.previousVersion.empty() && UpdCompareVersions(before, r.pkg.previousVersion) != 0)
            r.Step(L"pre-check", true, L"the installed version (" + Utf8ToW(before) + L") differs from the expected previous version (" + Utf8ToW(r.pkg.previousVersion) + L"); the installed one is used");
        r.pkg.previousVersion = before;
        r.act.previousVersion = before;
    }
    if (!beforeOk && beforeListedUnusable && r.pkg.allowUnknownBefore) {   // 2.8.6: only when the person asked for it, and only for a program that IS listed (with the version "Unknown")
        r.pkg.beforeUnknown = true;
        r.Step(L"pre-check", true, L"the installed version is listed as Unknown (typical for a program installed outside winget); updating anyway at the person's request. There is no earlier version to roll back to.");
    } else if (!beforeOk) {   // an update whose result cannot be verified (and whose previous version is unknown) is not started
        r.Step(L"pre-check", false, L"the installed version could not be confirmed from UniGetUI, so the result of an update could not be verified");
        return r.End(UpdOutcome::AbortedBeforeUpdate, L"The installed version could not be confirmed. Nothing was changed.", L"aborted");
    }
    if (beforeOk && !r.pkg.targetVersion.empty() && UpdCompareVersions(before, r.pkg.targetVersion) == 0) {
        r.Step(L"pre-check", true, L"already at the requested version; nothing to do");
        rep.versionAfter = before; rep.versionAfterConfirmed = true;
        return r.End(UpdOutcome::Succeeded, L"Already at version " + Utf8ToW(before) + L". No update was run.", L"noop");
    }

    // --- pre-update backup
    r.Phase(UpdPhase::BackingUp);
    std::wstring why;
    rep.backupRequired = UpdBackupRequired(ctx.policy, r.pkg, why);
    r.Step(L"backup policy", true, std::wstring(rep.backupRequired ? L"backup required: " : L"backup skipped: ") + why);
    if (rep.backupRequired) {
        const std::wstring label = L"Update " + Utf8ToW(r.pkg.id) + L" " + Utf8ToW(r.pkg.previousVersion) + L" to " + (r.pkg.targetVersion.empty() ? std::wstring(L"latest") : Utf8ToW(r.pkg.targetVersion));
        const AcbRecoveryPointResult rp = AcbCreateVerifiedRecoveryPoint(ctx.roots, label, ctx.journal, ctx.afterBackupSeal);
        rep.recoveryDir = rp.dir;
        rep.backupCreated = !rp.dir.empty();
        rep.backupVerified = rp.verified && rp.ok;
        rep.backupSealed = rp.sealed;
        r.act.recoveryDir = WToUtf8(rp.dir);
        r.Step(L"backup", rp.ok, rp.ok ? L"recovery point created, verified and sealed (" + std::to_wstring(rp.items) + L" file(s) of AnkhCore's own state): " + rp.dir : rp.error);
        if (!rp.ok) return r.End(UpdOutcome::AbortedBeforeUpdate, L"The backup was not created and verified, so the update was not started. Nothing was changed. " + rp.error, L"aborted");
        if (ctx.provider.backupAppData) {
            std::wstring d;
            const bool ok = ctx.provider.backupAppData(r.pkg, d);
            rep.appDataBackedUp = ok;
            r.Step(L"application data backup", ok, d);
            if (!ok) return r.End(UpdOutcome::AbortedBeforeUpdate, L"The application's data could not be backed up and verified, so the update was not started. Nothing was changed.", L"aborted");
        } else {
            r.Step(L"application data backup", true, L"not done: no per-application data map is defined, so the application's own data is not backed up");
        }
    }
    rep.rollback = UpdAssessRollbackFor(r.pkg, ctx.provider, rep.appDataBackedUp);
    r.Step(L"rollback capability", true, std::wstring(UpdRollbackCapabilityLabel(rep.rollback.capability)) + L": " + rep.rollback.summary);
    if (r.Crash(UpdPhase::BackingUp)) return rep;

    // --- execute
    r.Phase(UpdPhase::Updating);
    if (r.Crash(UpdPhase::Updating)) return rep;
    std::vector<std::wstring> out;
    rep.exitCode = ctx.provider.run(UpdBuildUniGetUpdateArgs(r.pkg), out);
    rep.commandRan = true;
    r.Step(L"update command", rep.exitCode == 0, UpdExitReason(rep.exitCode));
    if (rep.exitCode == kRunTimedOut)   // outcome unknown: keep the lock, verify nothing, roll nothing back
        return r.Unresolved(L"The update command timed out; the operation may still be running in the UniGetUI session, so the update state is unknown. "
                            L"No other update can start until this is resolved (the installed version and `uniget operation list` must be checked). Recovery point kept: " + rep.recoveryDir);

    // --- verify
    r.Phase(UpdPhase::Verifying);
    if (r.Crash(UpdPhase::Verifying)) return rep;
    std::string after;
    if (ctx.provider.refreshInstalled) ctx.provider.refreshInstalled(r.pkg);
    rep.versionAfterConfirmed = ReadVersion(r, L"installed version after", after);
    for (int attempt = 0; !rep.versionAfterConfirmed && attempt < ctx.afterRetries && rep.exitCode == 0; ++attempt) {
        if (ctx.afterRetryWaitMs) Sleep(ctx.afterRetryWaitMs);
        if (ctx.provider.refreshInstalled) ctx.provider.refreshInstalled(r.pkg);
        rep.versionAfterConfirmed = ReadVersion(r, L"installed version after (again)", after);
    }
    rep.versionAfter = after;
    rep.verdict = UpdClassifyResult(r.pkg, rep.exitCode, rep.versionAfterConfirmed, after);
    r.Step(L"verification", rep.verdict.outcome == UpdOutcome::Succeeded, rep.verdict.reason);

    // --- rollback (only the declared plan, and only when asked for)
    if (rep.verdict.needsRollback) {
        if (ctx.autoRollback) {
            rep.rollbackResult = DoRollback(r);
            if (rep.timedOut)
                return r.Unresolved(L"The rollback reinstall timed out; the operation may still be running in the UniGetUI session, so the state is unknown. "
                                    L"No other update can start until this is resolved. Recovery point kept: " + rep.recoveryDir);
        } else { rep.rollbackResult = UpdRollbackResult::NotAttempted; r.Step(L"rollback", false, L"not run automatically. Capability: " + std::wstring(UpdRollbackCapabilityLabel(rep.rollback.capability))); }
    } else rep.rollbackResult = UpdRollbackResult::NotNeeded;

    std::wstring head = rep.verdict.reason;
    if (rep.verdict.needsRollback) head += L" Rollback: " + std::wstring(UpdRollbackResultLabel(rep.rollbackResult)) + L" (" + UpdRollbackCapabilityLabel(rep.rollback.capability) + L").";
    if (rep.verdict.outcome != UpdOutcome::Succeeded && !rep.recoveryDir.empty()) head += L" Recovery point kept: " + rep.recoveryDir;
    return r.End(rep.verdict.outcome, head, rep.verdict.outcome == UpdOutcome::Succeeded ? L"done" : L"attention");
}

// The user's explicit request to roll an update back (after a failed or unwanted update). Takes the same lock.
inline UpdReport UpdRollback(UpdContext& ctx, const UpdPackage& pkgIn, bool appDataWasBackedUp) {
    using namespace upddetail;
    RunState r(ctx, pkgIn);
    UpdReport& rep = r.rep;
    std::wstring err;
    if (!UpdValidatePackage(r.pkg, err)) { rep.outcome = UpdOutcome::InvalidRequest; rep.headline = err; return rep; }
    r.act.id = WToUtf8(AcbRecoveryFolderStamp()); r.act.package = r.pkg.id; r.act.manager = r.pkg.manager;
    r.act.previousVersion = r.pkg.previousVersion; r.act.targetVersion = r.pkg.targetVersion;
    r.act.phase = UpdPhaseId(UpdPhase::RollingBack); r.act.started = AcbUtcStampNow();
    r.act.pid = ctx.pid ? ctx.pid : GetCurrentProcessId();
    if (!ctx.pid) UpdProcessStartString(r.act.pid, r.act.pidStart);
    const UpdLockResult lk = UpdAcquire(ctx.roots, r.act, rep.previousActive, ctx.isProcessAlive);
    if (lk != UpdLockResult::Created) {
        rep.outcome = lk == UpdLockResult::Busy ? UpdOutcome::Busy : lk == UpdLockResult::Interrupted ? UpdOutcome::InterruptedPreviousUpdate : UpdOutcome::AbortedBeforeUpdate;
        rep.headline = L"Rollback was not started: " + std::wstring(UpdOutcomeLabel(rep.outcome)) + L".";
        return rep;
    }
    if (ctx.provider.preflight) {
        std::wstring why;
        if (!ctx.provider.preflight(why)) {
            r.Step(L"pre-check", false, L"the UniGetUI session is not valid: " + why);
            return r.End(UpdOutcome::AbortedBeforeUpdate, L"The UniGetUI session is not valid, so the rollback was not started. " + why, L"aborted");
        }
    }
    // A caller's claim that the data was backed up is not evidence: it counts only when the provider confirms it now.
    bool dataOk = false;
    if (appDataWasBackedUp && ctx.provider.verifyAppDataBackup) {
        std::wstring d;
        dataOk = ctx.provider.verifyAppDataBackup(r.pkg, d);
        r.Step(L"application data backup check", dataOk, d);
    }
    rep.rollback = UpdAssessRollbackFor(r.pkg, ctx.provider, dataOk);
    rep.rollbackResult = DoRollback(r);
    if (rep.timedOut)
        return r.Unresolved(L"The rollback reinstall timed out; the operation may still be running in the UniGetUI session, so the state is unknown. "
                            L"No other update can start until this is resolved.");
    const bool good = rep.rollbackResult == UpdRollbackResult::Complete;
    const bool part = rep.rollbackResult == UpdRollbackResult::Partial;
    return r.End(good ? UpdOutcome::Succeeded : part ? UpdOutcome::PartiallySucceeded : UpdOutcome::Failed,
                 std::wstring(L"Rollback: ") + UpdRollbackResultLabel(rep.rollbackResult) + L". " + rep.rollback.summary, L"rollback");
}

}  // namespace ak
