// AnkhCore 2.6 — M-2.6-UPD2-A: an isolated, AnkhCore-owned UniGetUI session.
//
// Decision (owner, 2026-11-19): AnkhCore never talks to the user's own UniGetUI session. UniGetUI's CLI defaults to a shared
// pipe ("UniGetUI.IPC"), so "no --pipe-name" would silently reach the user's installed instance. This header makes that
// impossible by construction:
//   * the only way to run a `uniget` command is UgSession::Run, which ALWAYS prefixes `--transport named-pipe --pipe-name "<private>"`
//     and refuses any caller argument that tries to choose a transport, port or pipe itself;
//   * the private pipe name must start with "AnkhCore.UniGetUI." (UgValidPipeName), so it can never equal the default;
//   * the session is started headless by AnkhCore on that pipe, and is trusted ONLY after `status` answers
//     running == true, transport == "named-pipe" and namedPipeName == the expected private name (a started process or an exit
//     code 0 proves nothing: the real `status` exits 0 even when no session exists);
//   * AnkhCore ends only a process it started itself (recorded by pid + process start time) and never any other process.
// Every external effect goes through UgPlatform, so the tests use a mock; the live platform refuses to act in the test build.
#pragma once
#include "eng_updater.h"
#include <mutex>

namespace ak {

constexpr wchar_t kUgDefaultPipe[] = L"UniGetUI.IPC";        // the user's default session: never used
constexpr wchar_t kUgPipePrefix[] = L"AnkhCore.UniGetUI.";   // every private pipe name starts with this

// "AnkhCore.UniGetUI." + 8..32 lower-case hex digits.
inline bool UgValidPipeName(const std::wstring& p) {
    const size_t pre = sizeof(kUgPipePrefix) / sizeof(wchar_t) - 1;
    if (p.size() < pre + 8 || p.size() > pre + 32) return false;
    if (p.compare(0, pre, kUgPipePrefix) != 0) return false;
    for (size_t i = pre; i < p.size(); ++i) {
        const wchar_t c = p[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

// One stable private name per (data folder, Windows user): FNV-1a 64 over both, as 16 hex digits.
inline std::wstring UgPrivatePipeName(const std::wstring& dataRoot, const std::wstring& userName) {
    unsigned long long h = 1469598103934665603ull;
    const std::string s = WToUtf8(Lower(dataRoot) + L"|" + Lower(userName));
    for (const unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    wchar_t b[24];
    swprintf(b, 24, L"%016llx", h);
    return std::wstring(kUgPipePrefix) + b;
}
inline std::wstring UgPrivatePipeNameForThisUser(const AcbRoots& roots) {
    wchar_t u[256]; DWORD n = 256;
    if (!GetUserNameW(u, &n)) u[0] = 0;
    return UgPrivatePipeName(roots.data, u);
}

// ---------------------------------------------------------------- status validation
struct UgStatus {
    bool parsed = false, running = false;
    std::string transport, pipeName, version;
    int build = 0;
};

// The `status` document observed from UniGetUI 2026.3.3: {running, transport, tcpPort, namedPipeName, namedPipePath,
// baseAddress, version, buildNumber}.
inline UgStatus UgParseStatus(const std::string& text) {
    UgStatus s;
    JVal r;
    if (text.empty() || !JsonParse(text, r) || !r.Is(JVal::Obj)) return s;
    const JVal* run = r.Get("running");
    if (!run || !run->Is(JVal::Bool)) return s;
    s.running = run->b;
    if (const JVal* t = r.Get("transport"); t && t->Is(JVal::Str)) s.transport = t->s;
    if (const JVal* p = r.Get("namedPipeName"); p && p->Is(JVal::Str)) s.pipeName = p->s;
    if (const JVal* v = r.Get("version"); v && v->Is(JVal::Str)) s.version = v->s;
    if (const JVal* b = r.Get("buildNumber"); b && b->Is(JVal::Num) && b->n >= 0 && b->n < 1e9) s.build = static_cast<int>(b->n);
    s.parsed = true;
    return s;
}

inline bool UgStatusMatches(const UgStatus& s, const std::wstring& expectedPipe, std::wstring& why) {
    if (!s.parsed) { why = L"the status answer was not understood"; return false; }
    if (!s.running) { why = L"status reports that no session is running"; return false; }
    if (s.transport != "named-pipe") { why = L"the session does not use the named-pipe transport"; return false; }
    if (s.pipeName != WToUtf8(expectedPipe)) { why = L"the session answers on a different pipe (\"" + UpdShort(s.pipeName, 80) + L"\"), not AnkhCore's private one"; return false; }
    return true;
}

// ---------------------------------------------------------------- arguments
inline bool UgArgsAllowed(const std::wstring& args) {
    if (args.empty()) return false;
    const std::wstring l = Lower(args);
    for (const wchar_t* bad : {L"--pipe-name", L"--transport", L"--tcp-port", L"--ipc-api"})
        if (l.find(bad) != std::wstring::npos) return false;   // the caller never chooses the session
    return true;
}
inline std::wstring UgBuildCommand(const std::wstring& pipe, const std::wstring& args) {
    return L"--transport named-pipe --pipe-name \"" + pipe + L"\" " + args;
}
inline std::wstring UgBuildLaunchArgs(const std::wstring& pipe) {
    return L"--headless --ipc-api-transport named-pipe --ipc-api-pipe-name \"" + pipe + L"\"";
}

// A build that was verified to speak this protocol: UniGetUI 2026.3.3 (build 107). Newer is allowed (the format check at
// run time still guards), older or unknown is refused.
inline bool UgVersionCompatible(const std::string& fileVersion) {
    const int c = UpdCompareVersions(fileVersion, "2026.3.3");
    return c == 0 || c == 1;
}
inline std::string UgReadFileVersion(const std::wstring& path) {
    HMODULE m = LoadLibraryW(L"version.dll");
    if (!m) return {};
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto sz = reinterpret_cast<SizeFn>(GetProcAddress(m, "GetFileVersionInfoSizeW"));
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(m, "GetFileVersionInfoW"));
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(m, "VerQueryValueW"));
    std::string out;
    if (sz && info && query) {
        DWORD h = 0; const DWORD n = sz(path.c_str(), &h);
        std::vector<char> buf(n);
        VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
        if (n && info(path.c_str(), 0, n, buf.data()) && query(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) && fi) {
            char b[64];
            std::snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            out = b;
        }
    }
    FreeLibrary(m);
    return out;
}

// ---------------------------------------------------------------- the session record (<data>\Updater\uniget-session.json)
struct UgRecord {
    std::wstring pipe;
    DWORD pid = 0;                 // the UniGetUI process AnkhCore started
    std::string pidStart;          // its creation time: identity, not just a pid
    DWORD ownerPid = 0;            // the AnkhCore process that owns it
    std::string ownerStart;
    std::string phase;             // "starting" | "ready"
    std::wstring exe;
    std::string started;
};
inline std::wstring UgRecordPath(const AcbRoots& r) { return UpdDir(r) + L"\\uniget-session.json"; }

inline std::string UgRecordJson(const UgRecord& r) {
    return "{\r\n  \"formatVersion\": 1,\r\n  \"pipe\": \"" + JsonEsc(WToUtf8(r.pipe)) + "\",\r\n  \"pid\": " + std::to_string(r.pid) +
           ",\r\n  \"pidStart\": \"" + JsonEsc(r.pidStart) + "\",\r\n  \"ownerPid\": " + std::to_string(r.ownerPid) +
           ",\r\n  \"ownerStart\": \"" + JsonEsc(r.ownerStart) + "\",\r\n  \"phase\": \"" + JsonEsc(r.phase) +
           "\",\r\n  \"exe\": \"" + JsonEsc(WToUtf8(r.exe)) + "\",\r\n  \"started\": \"" + JsonEsc(r.started) + "\"\r\n}\r\n";
}
inline bool UgParseRecord(const std::string& text, UgRecord& r) {
    r = UgRecord{};
    JVal j;
    if (text.empty() || !JsonParse(text, j) || !j.Is(JVal::Obj)) return false;
    auto str = [&](const char* k, std::string& o) { if (const JVal* v = j.Get(k); v && v->Is(JVal::Str)) o = v->s; };
    auto num = [&](const char* k, DWORD& o) { if (const JVal* v = j.Get(k); v && v->Is(JVal::Num) && v->n >= 0 && v->n < 4294967295.0) o = static_cast<DWORD>(v->n); };
    std::string pipe, exe;
    str("pipe", pipe); str("pidStart", r.pidStart); str("ownerStart", r.ownerStart); str("phase", r.phase); str("exe", exe); str("started", r.started);
    num("pid", r.pid); num("ownerPid", r.ownerPid);
    r.pipe = Utf8ToW(pipe); r.exe = Utf8ToW(exe);
    if (!UgValidPipeName(r.pipe)) return false;                 // a record for any other pipe is not ours
    if (r.phase != "starting" && r.phase != "ready") return false;
    for (const std::string* d : {&r.pidStart, &r.ownerStart}) { if (d->size() > 20) return false; for (const char c : *d) if (c < '0' || c > '9') return false; }
    if (!UpdPlainText(exe, 520) || !UpdPlainText(r.started, 40)) return false;
    return true;
}
enum class UgRecStatus { Missing, Ok, Corrupt, Locked };
inline UgRecStatus UgReadRecord(const AcbRoots& roots, UgRecord& rec) {
    std::string text;
    switch (UpdReadMarker(UgRecordPath(roots), text)) {
    case UpdReadStatus::Missing: return UgRecStatus::Missing;
    case UpdReadStatus::Locked: return UgRecStatus::Locked;
    case UpdReadStatus::Empty: return UgRecStatus::Corrupt;
    default: return UgParseRecord(text, rec) ? UgRecStatus::Ok : UgRecStatus::Corrupt;
    }
}
// Exclusive create (temp file + move without replacing): exactly one starter wins, the file is never half-written.
inline bool UgCreateRecord(const AcbRoots& roots, const UgRecord& rec) {
    if (kTestBuild && !InTestArea(UgRecordPath(roots))) { TestRefused(L"session record"); return false; }
    if (!UpdPrepareDir(UpdDir(roots))) return false;
    const std::string body = UgRecordJson(rec);
    const std::wstring tmp = UpdDir(roots) + L"\\uniget-session." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &wr, nullptr) && wr == body.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (MoveFileExW(tmp.c_str(), UgRecordPath(roots).c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}
inline bool UgSaveRecord(const AcbRoots& roots, const UgRecord& rec) {
    const std::string body = UgRecordJson(rec);
    return WriteAtomic(UgRecordPath(roots), reinterpret_cast<const uint8_t*>(body.data()), body.size());
}
inline void UgRemoveRecord(const AcbRoots& roots) { DeleteFileW(UgRecordPath(roots).c_str()); }
inline void UgMoveAsideCorrupt(const AcbRoots& roots) {   // kept for inspection, never silently deleted
    const std::wstring dst = UpdDir(roots) + L"\\uniget-session.corrupt-" + AcbRecoveryFolderStamp() + L".json";
    MoveFileExW(UgRecordPath(roots).c_str(), dst.c_str(), 0);
}

// ---------------------------------------------------------------- platform (all external effects)
struct UgPlatform {
    // Starts `exe args` hidden and returns the new process's pid and creation time.
    std::function<bool(const std::wstring& exe, const std::wstring& args, DWORD& pid, std::string& start)> launch;
    // Runs `uniget.exe <args>` (args already carry the private pipe); returns the exit code, kRunLaunchFailed or kRunTimedOut.
    std::function<DWORD(const std::wstring& args, std::vector<std::wstring>& out)> cli;
    // Is THIS process (pid + creation time) still running?
    std::function<bool(DWORD pid, const std::string& start)> alive;
    // Ends one process whose identity AnkhCore recorded when it started it. Never called with anything else.
    std::function<bool(DWORD pid, const std::string& start)> terminate;
    std::function<void(DWORD ms)> sleep;
};

inline UgPlatform UgMakeLivePlatform(const AcbUniGetInstall& ug) {
    UgPlatform p;
    const std::wstring cli = ug.cliExe;
    p.launch = [](const std::wstring& exe, const std::wstring& args, DWORD& pid, std::string& start) -> bool {
        if (!UpdLiveExecutionAllowed(kTestBuild)) { TestRefused(L"launch " + exe); return false; }
        std::wstring cmd = L"\"" + exe + L"\" " + args;
        STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        const std::wstring dir = DirOf(exe);
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) return false;
        pid = pi.dwProcessId;
        const bool got = UpdProcessStartString(pid, start);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return got;
    };
    p.cli = [cli](const std::wstring& args, std::vector<std::wstring>& out) -> DWORD {
        if (!UpdLiveExecutionAllowed(kTestBuild)) { TestRefused(L"uniget " + args); return kRunLaunchFailed; }
        return RunCapture(cli, args, [&out](const std::wstring& l) { out.push_back(l); }, 25u * 60u * 1000u);   // 2.8: an installer (Chrome, ...) takes minutes; 60 s reported "timed out" for updates that did complete
    };
    p.alive = [](DWORD pid, const std::string& start) -> bool {
        if (pid == 0) return false;
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        bool ok = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
        CloseHandle(h);
        if (!ok) return false;
        std::string now;
        return UpdProcessStartString(pid, now) && (start.empty() ? false : now == start);
    };
    p.terminate = [](DWORD pid, const std::string& start) -> bool {
        if (!UpdLiveExecutionAllowed(kTestBuild)) { TestRefused(L"terminate process"); return false; }
        std::string now;
        if (pid == 0 || start.empty() || !UpdProcessStartString(pid, now) || now != start) return false;   // not the process AnkhCore started
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return false;
        const bool ok = TerminateProcess(h, 0) != 0;
        CloseHandle(h);
        return ok;
    };
    p.sleep = [](DWORD ms) { Sleep(ms); };
    return p;
}

// ---------------------------------------------------------------- the session
enum class UgState { NotStarted, Ready, Invalid, Terminated, Stopped };
inline const wchar_t* UgStateLabel(UgState s) {
    switch (s) {
    case UgState::Ready: return L"Ready";
    case UgState::Invalid: return L"Invalid";
    case UgState::Terminated: return L"Terminated";
    case UgState::Stopped: return L"Stopped";
    default: return L"Not started";
    }
}
enum class UgCode { Ok, Reused, Adopted, Started, Busy, Invalid, Failed };
struct UgResult {
    bool ok = false;
    UgCode code = UgCode::Failed;
    UgState state = UgState::NotStarted;
    std::wstring message;
};

struct UgConfig {
    AcbUniGetInstall ug;
    AcbRoots roots;
    std::wstring pipe;                 // must satisfy UgValidPipeName
    std::string exeVersion;            // the file version of UniGetUI.exe (UgReadFileVersion); empty = unknown = refused
    bool requirePortable = true;       // a portable copy keeps its own data; an installed copy would share the user's settings
    int startAttempts = 60;
    DWORD pollMs = 500;
    int quitAttempts = 20;
    DWORD ownerPid = 0;                // 0 = this process
    std::string ownerStart;
};

class UgSession {
public:
    UgSession(UgConfig cfg, UgPlatform plat) : cfg_(std::move(cfg)), p_(std::move(plat)) {
        if (!cfg_.ownerPid) {
            cfg_.ownerPid = GetCurrentProcessId();
            if (cfg_.ownerStart.empty()) UpdProcessStartString(cfg_.ownerPid, cfg_.ownerStart);
        }
    }

    // Start, reuse or adopt the private session. On any doubt the session is NOT usable.
    UgResult Ensure() { std::lock_guard<std::mutex> g(mu_); return EnsureLocked(); }

    // Is the session usable right now? Process alive (same identity) AND a matching `status` answer.
    UgResult Validate() { std::lock_guard<std::mutex> g(mu_); return ValidateLocked(); }

    // The only way to run a uniget command. Always on the private pipe; refuses when the session is not Ready.
    DWORD Run(const std::wstring& args, std::vector<std::wstring>& out) {
        std::wstring full;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (state_ != UgState::Ready) { msg_ = L"The UniGetUI session is not ready (" + std::wstring(UgStateLabel(state_)) + L")."; return kRunLaunchFailed; }
            if (!UgArgsAllowed(args)) { msg_ = L"The command was refused: it may not choose the transport, port or pipe."; return kRunLaunchFailed; }
            if (!p_.alive(pid_, start_)) { state_ = UgState::Terminated; msg_ = L"The AnkhCore UniGetUI session ended unexpectedly."; return kRunLaunchFailed; }
            full = UgBuildCommand(cfg_.pipe, args);
        }
        const DWORD rc = p_.cli(full, out);
        if (rc == 3 || (rc != 0 && UpdParsePackageListingLines(out).message.find(L"token is not available") != std::wstring::npos)) {
            std::lock_guard<std::mutex> g(mu_);
            state_ = UgState::Invalid;   // the session went away or lost its token: nothing more runs until it is validated again
            msg_ = L"The UniGetUI session stopped answering.";
        }
        return rc;
    }

    // Ends ONLY the session AnkhCore started or adopted from its own record. No identity = nothing is touched.
    UgResult Shutdown() { std::lock_guard<std::mutex> g(mu_); return ShutdownLocked(); }

    UgState State() const { std::lock_guard<std::mutex> g(mu_); return state_; }
    std::wstring Message() const { std::lock_guard<std::mutex> g(mu_); return msg_; }
    const std::wstring& Pipe() const { return cfg_.pipe; }

private:
    UgConfig cfg_;
    UgPlatform p_;
    mutable std::mutex mu_;
    UgState state_ = UgState::NotStarted;
    DWORD pid_ = 0;
    std::string start_;
    std::wstring msg_;

    UgResult Res(bool ok, UgCode c, const std::wstring& m) { msg_ = m; UgResult r; r.ok = ok; r.code = c; r.state = state_; r.message = m; return r; }

    static std::string Join(const std::vector<std::wstring>& lines) { std::string t; for (const auto& l : lines) { t += WToUtf8(l); t += '\n'; } return t; }

    // One status round trip on the private pipe.
    bool CheckStatus(std::wstring& why) {
        std::vector<std::wstring> out;
        const DWORD rc = p_.cli(UgBuildCommand(cfg_.pipe, L"status"), out);
        if (rc != 0) { why = L"status failed: " + UpdExitReason(rc); return false; }
        return UgStatusMatches(UgParseStatus(Join(out)), cfg_.pipe, why);
    }

    std::wstring ConfigProblem() const {
        if (!UgValidPipeName(cfg_.pipe)) return L"The private pipe name is not valid (it must start with \"AnkhCore.UniGetUI.\").";
        if (!cfg_.ug.found || cfg_.ug.guiExe.empty() || cfg_.ug.cliExe.empty()) return L"No UniGetUI was supplied or found.";
        if (cfg_.requirePortable && !cfg_.ug.portable) return L"The UniGetUI found is an installed copy; AnkhCore only starts a portable copy that keeps its own data.";
        if (cfg_.exeVersion.empty()) return L"The UniGetUI version could not be read.";
        if (!UgVersionCompatible(cfg_.exeVersion)) return L"UniGetUI " + Utf8ToW(cfg_.exeVersion) + L" is older than the verified 2026.3.3.";
        return {};
    }

    UgResult ValidateLocked() {
        if (pid_ == 0) return Res(false, UgCode::Invalid, L"There is no AnkhCore UniGetUI session.");
        if (!p_.alive(pid_, start_)) { state_ = UgState::Terminated; return Res(false, UgCode::Failed, L"The AnkhCore UniGetUI session ended unexpectedly."); }
        std::wstring why;
        if (!CheckStatus(why)) { state_ = UgState::Invalid; return Res(false, UgCode::Invalid, L"The UniGetUI session did not validate: " + why + L"."); }
        state_ = UgState::Ready;
        return Res(true, UgCode::Ok, L"The AnkhCore UniGetUI session is valid.");
    }

    UgResult EnsureLocked() {
        const std::wstring bad = ConfigProblem();
        if (!bad.empty()) { state_ = UgState::Invalid; return Res(false, UgCode::Invalid, bad); }

        if (state_ == UgState::Ready) {
            UgResult v = ValidateLocked();
            if (v.ok) { v.code = UgCode::Reused; return v; }
            if (state_ == UgState::Invalid) return v;   // alive but wrong: never start a second one over it
            // Terminated: fall through and start again (the record of the dead process is cleaned up below)
        }

        UgRecord rec;
        switch (UgReadRecord(cfg_.roots, rec)) {
        case UgRecStatus::Locked: return Res(false, UgCode::Busy, L"Another AnkhCore instance is starting the UniGetUI session.");
        case UgRecStatus::Corrupt: UgMoveAsideCorrupt(cfg_.roots); break;   // the pre-launch probe below still protects the pipe
        case UgRecStatus::Ok: {
            if (rec.phase == "starting") {
                if (p_.alive(rec.ownerPid, rec.ownerStart)) return Res(false, UgCode::Busy, L"Another AnkhCore instance is starting the UniGetUI session.");
                // the starter died: the process it launched (if any) is an unvalidated orphan that AnkhCore itself started
                if (rec.pid && p_.alive(rec.pid, rec.pidStart)) p_.terminate(rec.pid, rec.pidStart);
                UgRemoveRecord(cfg_.roots);
            } else if (p_.alive(rec.pid, rec.pidStart)) {   // "ready": a session an earlier AnkhCore started and left running
                pid_ = rec.pid; start_ = rec.pidStart;
                std::wstring why;
                if (!CheckStatus(why)) { state_ = UgState::Invalid; return Res(false, UgCode::Invalid, L"A session recorded by AnkhCore is running but did not validate: " + why + L". It is left alone; shut it down explicitly."); }
                rec.ownerPid = cfg_.ownerPid; rec.ownerStart = cfg_.ownerStart;
                UgSaveRecord(cfg_.roots, rec);
                state_ = UgState::Ready;
                return Res(true, UgCode::Adopted, L"The existing AnkhCore UniGetUI session was validated and reused.");
            } else UgRemoveRecord(cfg_.roots);   // stale: its process is gone
            break;
        }
        default: break;
        }
        pid_ = 0; start_.clear();

        // Pre-launch probe: if something already answers on OUR pipe, AnkhCore did not start it, so it is neither used nor replaced.
        {
            std::wstring why;
            std::vector<std::wstring> out;
            const DWORD rc = p_.cli(UgBuildCommand(cfg_.pipe, L"status"), out);
            const UgStatus st = UgParseStatus(Join(out));
            if (rc == 0 && st.parsed && st.running) {
                state_ = UgState::Invalid;
                return Res(false, UgCode::Invalid, L"The private pipe is already served by a session AnkhCore did not start. It was not used and not touched.");
            }
        }

        UgRecord mine;
        mine.pipe = cfg_.pipe; mine.phase = "starting"; mine.ownerPid = cfg_.ownerPid; mine.ownerStart = cfg_.ownerStart;
        mine.exe = cfg_.ug.guiExe; mine.started = AcbUtcStampNow();
        if (!UgCreateRecord(cfg_.roots, mine)) return Res(false, UgCode::Busy, L"Another AnkhCore instance is starting the UniGetUI session.");

        DWORD pid = 0; std::string start;
        if (!p_.launch(cfg_.ug.guiExe, UgBuildLaunchArgs(cfg_.pipe), pid, start)) {
            UgRemoveRecord(cfg_.roots);
            state_ = UgState::Terminated;
            return Res(false, UgCode::Failed, L"UniGetUI could not be started.");
        }
        mine.pid = pid; mine.pidStart = start;
        UgSaveRecord(cfg_.roots, mine);

        std::wstring why = L"no answer yet";
        for (int i = 0; i < cfg_.startAttempts; ++i) {
            if (!p_.alive(pid, start)) {
                UgRemoveRecord(cfg_.roots);
                state_ = UgState::Terminated;
                return Res(false, UgCode::Failed, L"UniGetUI exited while starting.");
            }
            if (CheckStatusFor(pid, start, why)) {
                mine.phase = "ready";
                UgSaveRecord(cfg_.roots, mine);
                pid_ = pid; start_ = start;
                state_ = UgState::Ready;
                return Res(true, UgCode::Started, L"The AnkhCore UniGetUI session was started and validated.");
            }
            p_.sleep(cfg_.pollMs);
        }
        // never validated: it is a process AnkhCore itself started (identity recorded), so it may be ended
        p_.terminate(pid, start);
        UgRemoveRecord(cfg_.roots);
        state_ = UgState::Invalid;
        return Res(false, UgCode::Invalid, L"The UniGetUI session did not validate in time (" + why + L"); the process AnkhCore started was ended.");
    }

    bool CheckStatusFor(DWORD, const std::string&, std::wstring& why) { return CheckStatus(why); }

    UgResult ShutdownLocked() {
        if (pid_ == 0) { state_ = UgState::Stopped; return Res(true, UgCode::Ok, L"There was no AnkhCore-owned session to end; nothing was touched."); }
        bool quitSent = false;
        if (state_ != UgState::Terminated && p_.alive(pid_, start_)) {
            std::vector<std::wstring> out;
            quitSent = p_.cli(UgBuildCommand(cfg_.pipe, L"app quit"), out) == 0;   // on the private pipe only
        }
        for (int i = 0; i < cfg_.quitAttempts && p_.alive(pid_, start_); ++i) p_.sleep(cfg_.pollMs);
        bool killed = false;
        if (p_.alive(pid_, start_)) killed = p_.terminate(pid_, start_);   // the identity-checked process AnkhCore started
        const bool gone = !p_.alive(pid_, start_);
        if (gone) UgRemoveRecord(cfg_.roots);
        const DWORD oldPid = pid_;
        (void)oldPid;
        pid_ = 0; start_.clear();
        state_ = UgState::Stopped;
        if (!gone) return Res(false, UgCode::Failed, L"The AnkhCore UniGetUI session could not be ended.");
        return Res(true, UgCode::Ok, killed ? L"The AnkhCore UniGetUI session did not quit on request and was ended."
                                            : quitSent ? L"The AnkhCore UniGetUI session was quit." : L"The AnkhCore UniGetUI session had already ended.");
    }
};

// An UpdProvider whose every command goes through the private session and which is gated by a fresh validation.
inline UpdProvider UgMakeUpdProvider(UgSession& s) {
    UpdProvider pr = UpdMakeUniGetProviderWith(true, [&s](const std::wstring& args, std::vector<std::wstring>& out) { return s.Run(args, out); });
    pr.preflight = [&s](std::wstring& why) { const UgResult r = s.Validate(); why = r.message; return r.ok; };
    return pr;
}

// Convenience for the live application: configuration from a detected UniGetUI.
inline UgConfig UgMakeConfig(const AcbUniGetInstall& ug, const AcbRoots& roots) {
    UgConfig c;
    c.ug = ug; c.roots = roots;
    c.pipe = UgPrivatePipeNameForThisUser(roots);
    c.exeVersion = ug.found ? UgReadFileVersion(ug.guiExe) : std::string();
    return c;
}

}  // namespace ak
