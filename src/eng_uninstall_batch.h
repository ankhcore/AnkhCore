// eng_uninstall_batch.h - 2.7 K3: the sequential Multi-Uninstall runner (Apps Manager, workstream K, part D).
//
// Rules of the 2.7 prompt, as code:
//   * applications are removed ONE AT A TIME, through their own uninstaller (or Remove-AppxPackage for Store apps): no guessed paths, no forced
//     removal, no assumption that an uninstaller is silent or that its exit code means anything;
//   * a removal counts only when the application is verified GONE from the inventory afterwards. Exit code 0 with the entry still listed is
//     "not removed" (the person may have cancelled the uninstaller's own window); a failing code with the entry gone is still "removed";
//     when the inventory cannot tell, the result says "finished, not verified";
//   * success, failure, cancellation, refusal and restart-required are reported separately, per application;
//   * the batch is single-flight: a second batch while one runs is refused; cancellation takes effect between applications (an uninstaller
//     that is already running is never killed);
//   * a timed-out uninstaller may still be running, so the batch stops there by default;
//   * AnkhCore never restarts Windows: msiexec commands get /norestart, and a restart request is only REPORTED;
//   * after a verified removal an injected scan hook (K1) may list remnants; nothing is deleted here.
// Everything with side effects is injected (`UninstallBackend`), so it is tested without installing or removing anything.
#pragma once
#include "base.h"
#include "eng_remnants.h"
#include <atomic>

namespace ak {

struct UninstallItem {
    std::wstring id;                    // unique within a batch (the inventory key)
    std::wstring name, publisher;
    std::wstring command;               // the uninstall command line; for a Store app: its PackageFullName
    std::wstring installLocation;       // captured before the uninstall, for the remnants scan
    std::wstring uninstallKey;          // registry sub-path of its uninstall entry, for the verification
    std::vector<std::wstring> exeNames;
    bool store = false;
    bool userHive = false;              // the uninstall entry is under HKCU (else HKLM)
};

enum class UnState {
    Pending, Running,
    Removed,                // verified gone
    RemovedRestartRequired, // gone (or restart pending) and the uninstaller asked for a restart: never done by AnkhCore
    NotRemoved,             // the uninstaller finished but the application is still listed
    Unverified,             // finished with success, but the inventory cannot say whether it is gone
    Failed,                 // could not start, or failed and the application is still listed
    TimedOut,               // still running when the wait ended: the final state is unknown
    Cancelled,              // not started because the batch was cancelled
    Refused,                // not run: unusable command, AnkhCore itself, duplicate
    NotStarted              // not started because an earlier timeout (or failure, when so configured) stopped the batch
};
inline const wchar_t* UnStateLabel(UnState s) {
    switch (s) {
    case UnState::Pending: return L"Waiting";
    case UnState::Running: return L"Uninstalling";
    case UnState::Removed: return L"Removed";
    case UnState::RemovedRestartRequired: return L"Removed (restart required)";
    case UnState::NotRemoved: return L"Not removed: still listed";
    case UnState::Unverified: return L"Finished (could not verify)";
    case UnState::Failed: return L"Failed";
    case UnState::TimedOut: return L"Timed out (final state unknown)";
    case UnState::Cancelled: return L"Cancelled";
    case UnState::Refused: return L"Not run";
    default: return L"Not started";
    }
}

struct UninstallRunResult { bool launched = false; DWORD exitCode = 0; bool timedOut = false, cancelled = false; std::wstring detail; };
enum class UnInstalled { Gone, StillInstalled, Unknown };
struct UninstallBackend {
    // Runs the (prepared) command and waits. `cancelled` lets a backend that can still stop before it starts say so.
    std::function<UninstallRunResult(const UninstallItem&, const std::wstring& preparedCommand, const std::function<bool()>& cancelled)> run;
    std::function<UnInstalled(const UninstallItem&)> verify;
    std::function<std::vector<RemCandidate>(const UninstallItem&)> scanRemnants;   // optional; called only after a verified removal
};

struct UninstallOptions { bool stopOnFailure = false, stopOnTimeout = true; };

struct UninstallItemResult {
    UninstallItem item;
    UnState state = UnState::Pending;
    DWORD exitCode = 0;
    bool restartRequested = false;
    std::wstring detail;
    std::vector<RemCandidate> remnants;
};
struct UninstallEvent { size_t index = 0, total = 0; std::wstring name; UnState state = UnState::Pending; std::wstring detail; };

struct UninstallBatchReport {
    std::vector<UninstallItemResult> results;
    bool refusedBusy = false, cancelled = false, stoppedEarly = false, restartRequired = false;
    int removed = 0, restart = 0, notRemoved = 0, unverified = 0, failed = 0, timedOut = 0, cancelledCount = 0, refused = 0, notStarted = 0;
    std::wstring summary;
};

// ---------------------------------------------------------------- command checks (pure)
// "" = usable; otherwise why the command is not run. Desktop uninstall strings are command lines: control characters and unquoted
// cmd operators (& | < > ^) are refused, because an uninstall entry is data that anyone who can write the registry can change.
inline std::wstring UninstallCommandIssue(const UninstallItem& it) {
    if (it.name.empty() || it.id.empty()) return L"The application has no name or identity.";
    if (it.name.compare(0, 8, kBrand) == 0 || Lower(it.id).find(L"ankhcore") != std::wstring::npos) return L"AnkhCore cannot remove itself from here: use its own uninstaller.";
    const std::wstring c = it.command;
    if (c.empty()) return L"There is no uninstall command.";
    if (c.size() > 2048) return L"The uninstall command is too long.";
    for (wchar_t ch : c) if (ch < 0x20) return L"The uninstall command contains control characters.";
    if (it.store) {
        for (wchar_t ch : c) if (!(iswalnum(ch) || ch == L'.' || ch == L'_' || ch == L'~' || ch == L'-')) return L"The Store package name is not a plain package name.";
        return L"";
    }
    bool quoted = false;
    for (wchar_t ch : c) { if (ch == L'"') quoted = !quoted; else if (!quoted && (ch == L'&' || ch == L'|' || ch == L'<' || ch == L'>' || ch == L'^')) return L"The uninstall command contains a shell operator, so it is not run."; }
    if (quoted) return L"The uninstall command has an unbalanced quote.";
    return L"";
}
// The command that is actually run: msiexec gets /norestart (AnkhCore never restarts Windows), nothing else is changed.
inline std::wstring UninstallPrepare(const UninstallItem& it) {
    if (it.store) return it.command;
    std::wstring t = it.command; while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    const std::wstring low = Lower(t);
    const bool msi = low.rfind(L"msiexec", 0) == 0 || low.rfind(L"\"msiexec", 0) == 0 || low.find(L"\\msiexec.exe") != std::wstring::npos;
    if (msi && low.find(L"/norestart") == std::wstring::npos && low.find(L"/forcerestart") == std::wstring::npos && low.find(L"/promptrestart") == std::wstring::npos) return it.command + L" /norestart";
    return it.command;
}
inline bool UninstallExitWantsRestart(DWORD code) { return code == 3010 || code == 1641; }   // ERROR_SUCCESS_REBOOT_REQUIRED, ERROR_SUCCESS_REBOOT_INITIATED

inline RemApp UninstallToRemApp(const UninstallItem& it, bool uninstalledOk) {
    RemApp a; a.name = it.name; a.publisher = it.publisher; a.installLocation = it.installLocation; a.exeNames = it.exeNames; a.uninstalledOk = uninstalledOk;
    return a;
}

// ---------------------------------------------------------------- the runner
class UninstallBatchRunner {
public:
    // Blocking: call it from a worker thread. Events are delivered on that thread.
    UninstallBatchReport Run(const std::vector<UninstallItem>& items, const UninstallBackend& be, const std::function<void(const UninstallEvent&)>& onEvent = {}, const UninstallOptions& opt = {}) {
        UninstallBatchReport rep;
        if (busy_.exchange(true)) { rep.refusedBusy = true; rep.summary = L"Another uninstall batch is already running."; return rep; }
        cancel_ = false;
        struct Release { std::atomic<bool>& b; ~Release() { b = false; } } release{busy_};
        const size_t total = items.size();
        rep.results.resize(total);
        std::set<std::wstring> seen;
        bool stopped = false; std::wstring stopWhy;
        const std::function<bool()> cancelled = [this] { return cancel_.load(); };
        auto emit = [&](size_t i, UnState s, const std::wstring& detail) { rep.results[i].state = s; rep.results[i].detail = detail; if (onEvent) onEvent({i, total, rep.results[i].item.name, s, detail}); };
        for (size_t i = 0; i < total; ++i) { rep.results[i].item = items[i]; }
        for (size_t i = 0; i < total; ++i) {
            UninstallItemResult& r = rep.results[i];
            if (cancel_) { emit(i, UnState::Cancelled, L"The batch was cancelled before this application was started."); continue; }
            if (stopped) { emit(i, UnState::NotStarted, stopWhy); continue; }
            const std::wstring key = Lower(r.item.id);
            if (!seen.insert(key).second) { emit(i, UnState::Refused, L"This application is listed twice in the batch."); continue; }
            const std::wstring issue = UninstallCommandIssue(r.item);
            if (!issue.empty()) { emit(i, UnState::Refused, issue); continue; }
            if (!be.run) { emit(i, UnState::Failed, L"There is no way to run an uninstaller."); continue; }
            emit(i, UnState::Running, L"Running its own uninstaller.");
            const UninstallRunResult run = be.run(r.item, UninstallPrepare(r.item), cancelled);
            r.exitCode = run.exitCode;
            if (!run.launched) { emit(i, UnState::Failed, L"The uninstaller could not be started" + (run.detail.empty() ? std::wstring(L".") : L": " + run.detail)); if (opt.stopOnFailure) { stopped = true; stopWhy = L"Stopped after a failure."; } continue; }
            if (run.cancelled) { emit(i, UnState::Cancelled, L"Cancelled before the uninstaller ran."); continue; }
            if (run.timedOut) { emit(i, UnState::TimedOut, L"The uninstaller did not finish in time. It may still be running, so the final state is unknown."); if (opt.stopOnTimeout) { stopped = true; stopWhy = L"Stopped because an earlier uninstaller did not finish."; } continue; }
            r.restartRequested = UninstallExitWantsRestart(run.exitCode);
            const UnInstalled v = be.verify ? be.verify(r.item) : UnInstalled::Unknown;
            const std::wstring code = L"exit code " + std::to_wstring(run.exitCode);
            if (v == UnInstalled::Gone) {
                if (r.restartRequested) emit(i, UnState::RemovedRestartRequired, L"Removed. Windows must be restarted to finish; AnkhCore does not restart it.");
                else emit(i, UnState::Removed, run.exitCode == 0 ? std::wstring(L"Removed.") : L"Removed (" + code + L", but the application is no longer listed).");
                if (be.scanRemnants) r.remnants = be.scanRemnants(r.item);   // only after a verified removal; nothing is deleted
            } else if (v == UnInstalled::StillInstalled) {
                if (run.exitCode == 0 || r.restartRequested) emit(i, UnState::NotRemoved, r.restartRequested ? L"The uninstaller asked for a restart and the application is still listed." : L"The uninstaller finished but the application is still listed (it may have been cancelled in its own window).");
                else { emit(i, UnState::Failed, L"The uninstaller failed (" + code + L") and the application is still listed."); if (opt.stopOnFailure) { stopped = true; stopWhy = L"Stopped after a failure."; } }
            } else {
                if (run.exitCode == 0) emit(i, UnState::Unverified, L"The uninstaller finished with exit code 0, but AnkhCore cannot tell whether the application is gone.");
                else if (r.restartRequested) emit(i, UnState::RemovedRestartRequired, L"The uninstaller asked for a restart; AnkhCore cannot verify the removal and does not restart Windows.");
                else { emit(i, UnState::Failed, L"The uninstaller failed (" + code + L")."); if (opt.stopOnFailure) { stopped = true; stopWhy = L"Stopped after a failure."; } }
            }
        }
        for (const UninstallItemResult& r : rep.results) {
            switch (r.state) {
            case UnState::Removed: ++rep.removed; break;
            case UnState::RemovedRestartRequired: ++rep.restart; break;
            case UnState::NotRemoved: ++rep.notRemoved; break;
            case UnState::Unverified: ++rep.unverified; break;
            case UnState::Failed: ++rep.failed; break;
            case UnState::TimedOut: ++rep.timedOut; break;
            case UnState::Cancelled: ++rep.cancelledCount; break;
            case UnState::Refused: ++rep.refused; break;
            case UnState::NotStarted: ++rep.notStarted; break;
            default: break;
            }
            if (r.restartRequested) rep.restartRequired = true;
        }
        rep.cancelled = cancel_.load() && rep.cancelledCount > 0;
        rep.stoppedEarly = rep.notStarted > 0;
        rep.summary = std::to_wstring(rep.removed + rep.restart) + L" removed" + (rep.restart ? L" (" + std::to_wstring(rep.restart) + L" need a restart)" : std::wstring()) +
                      L", " + std::to_wstring(rep.notRemoved) + L" still listed, " + std::to_wstring(rep.unverified) + L" not verified, " + std::to_wstring(rep.failed) + L" failed, " +
                      std::to_wstring(rep.timedOut) + L" timed out, " + std::to_wstring(rep.cancelledCount) + L" cancelled, " + std::to_wstring(rep.refused) + L" not run, " + std::to_wstring(rep.notStarted) + L" not started.";
        return rep;
    }
    // Takes effect before the NEXT application starts. An uninstaller that is already running is never killed.
    void Cancel() { cancel_ = true; }
    bool Busy() const { return busy_.load(); }
private:
    std::atomic<bool> busy_{false}, cancel_{false};
};

// ---------------------------------------------------------------- the live backend (used by the application later)
inline UninstallBackend UninstallLiveBackend(DWORD timeoutMs = 30u * 60u * 1000u) {
    UninstallBackend be;
    be.run = [timeoutMs](const UninstallItem& it, const std::wstring& cmd, const std::function<bool()>& cancelled) {
        UninstallRunResult r;
        if (cancelled && cancelled()) { r.launched = true; r.cancelled = true; return r; }
        if (kTestBuild) { TestRefused(L"uninstall " + it.name); r.detail = L"the test build runs no uninstaller"; return r; }
        if (it.store) {
            const DWORD rc = Run(L"powershell.exe", L"-NoProfile -ExecutionPolicy Bypass -Command \"Remove-AppxPackage -Package '" + cmd + L"'\"", true, timeoutMs);
            r.launched = rc != kRunLaunchFailed; r.timedOut = rc == kRunTimedOut; r.exitCode = (r.launched && !r.timedOut) ? rc : 0; return r;
        }
        wchar_t expanded[4096]; const DWORD n = ExpandEnvironmentStringsW(cmd.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
        std::wstring line = (n > 0 && n <= std::size(expanded)) ? std::wstring(expanded) : cmd;
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { r.detail = L"Windows error " + std::to_wstring(GetLastError()); return r; }   // the uninstaller shows its own window
        r.launched = true;
        if (WaitForSingleObject(pi.hProcess, timeoutMs) != WAIT_OBJECT_0) r.timedOut = true; else { DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); r.exitCode = code; }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return r;
    };
    be.verify = [](const UninstallItem& it) {
        if (it.store || it.uninstallKey.empty()) return UnInstalled::Unknown;   // a Store package would need PowerShell: not verified here
        return (RegKeyExists(HKEY_LOCAL_MACHINE, it.uninstallKey) || RegKeyExists(HKEY_CURRENT_USER, it.uninstallKey)) ? UnInstalled::StillInstalled : UnInstalled::Gone;
    };
    return be;
}

}  // namespace ak
