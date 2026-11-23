// AnkhCore 2.5 — the background engine process (`AnkhCore.exe --engine`, D25) and the UI's client for it.
// Pipe security: per-user ACL, remote clients rejected, the server claims the first instance so nothing can
// squat the name, and each end verifies that the other is this same executable. No fan logic lives here:
// fan commands are passed to SensorHub unchanged, and NBFC control is not part of this code.
#pragma once
#include "ipc.h"
#include <sddl.h>
#include <mutex>
#pragma comment(lib, "advapi32.lib")

namespace ak {

inline std::wstring CurrentUserSid() {
    HANDLE t = nullptr; std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return out;
    DWORD n = 0; GetTokenInformation(t, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> b(n);
    if (n && GetTokenInformation(t, TokenUser, b.data(), n, &n)) { LPWSTR s = nullptr; if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, &s)) { out = s; LocalFree(s); } }
    CloseHandle(t);
    return out;
}
inline std::wstring EnginePipeName() { return L"\\\\.\\pipe\\AnkhCore.Engine" + std::wstring(kInstanceTag) + L"." + CurrentUserSid(); }
inline bool PeerIsSelf(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t b[MAX_PATH * 2]; DWORD n = MAX_PATH * 2;
    const BOOL ok = QueryFullProcessImageNameW(p, 0, b, &n);
    CloseHandle(p);
    return ok && _wcsicmp(b, SelfPath().c_str()) == 0;
}

// Overlapped framing over a pipe handle: reader and writer can run at the same time; a stop event ends a wait.
class PipeConn {
public:
    HANDLE h = INVALID_HANDLE_VALUE, stop = nullptr;
    bool Recv(uint32_t& type, std::vector<uint8_t>& payload) {
        uint8_t hd[12]; uint32_t len = 0;
        if (!Io(false, hd, 12, INFINITE) || !IpcHeaderOk(hd, type, len)) return false;
        payload.resize(len);
        return !len || Io(false, payload.data(), len, 5000);
    }
    bool Send(uint32_t type, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lk(wm_);
        if (h == INVALID_HANDLE_VALUE) return false;
        IpcWriter w; w.b = payload; auto m = IpcPack(type, w);
        return Io(true, m.data(), static_cast<DWORD>(m.size()), 1500);
    }
private:
    bool Io(bool write, void* p, DWORD n, DWORD timeout) {
        auto* c = static_cast<uint8_t*>(p); DWORD done = 0;
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr); bool ok = true;
        while (ok && done < n) {
            OVERLAPPED o{}; o.hEvent = ev; ResetEvent(ev); DWORD got = 0;
            const BOOL r = write ? WriteFile(h, c + done, n - done, nullptr, &o) : ReadFile(h, c + done, n - done, nullptr, &o);
            if (!r && GetLastError() != ERROR_IO_PENDING) { ok = false; break; }
            HANDLE w[2] = {ev, stop};
            const DWORD wr = WaitForMultipleObjects(stop ? 2 : 1, w, FALSE, timeout);
            if (wr != WAIT_OBJECT_0) { CancelIoEx(h, &o); GetOverlappedResult(h, &o, &got, TRUE); ok = false; break; }
            if (!GetOverlappedResult(h, &o, &got, FALSE) || !got) { ok = false; break; }
            done += got;
        }
        CloseHandle(ev);
        return ok;
    }
    std::mutex wm_;
};

// ---------------------------------------------------------------- engine process
struct EngineProcess {
    TelemetryEngine tel; PipeConn conn; HWND wnd = nullptr; HANDLE stop = nullptr; std::atomic<bool> connected{false};
    static EngineProcess& Get() { static EngineProcess e; return e; }
    void Serve() {
        SECURITY_ATTRIBUTES sa{sizeof(sa)}; PSECURITY_DESCRIPTOR sd = nullptr;
        const std::wstring sddl = L"D:P(A;;GA;;;" + CurrentUserSid() + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) { LogE(L"Engine: the pipe security could not be set."); return; }
        sa.lpSecurityDescriptor = sd;
        HANDLE p = CreateNamedPipeW(EnginePipeName().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1u << 20, 1u << 20, 0, &sa);
        LocalFree(sd);
        if (p == INVALID_HANDLE_VALUE) { LogE(L"Engine: the pipe could not be created."); return; }
        conn.h = p; conn.stop = stop;
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
            OVERLAPPED o{}; o.hEvent = ev; ResetEvent(ev);
            const BOOL c = ConnectNamedPipe(p, &o);
            const DWORD err = GetLastError();
            if (!c && err == ERROR_IO_PENDING) { HANDLE w[2] = {ev, stop}; if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) { CancelIoEx(p, &o); break; } }
            else if (!c && err != ERROR_PIPE_CONNECTED) { Sleep(100); continue; }
            ULONG pid = 0;
            if (GetNamedPipeClientProcessId(p, &pid) && PeerIsSelf(pid)) Session();
            else LogW(L"Engine: a connection from another program was refused.");
            connected = false;
            DisconnectNamedPipe(p);
        }
        CloseHandle(ev); conn.h = INVALID_HANDLE_VALUE; CloseHandle(p);
    }
    void Session() {
        IpcWriter hello; hello.U32(kIpcVersion); hello.U32(GetCurrentProcessId());
        MonitorStore snapshot; tel.CopyHistory(snapshot);
        if (!conn.Send(IpcHello, hello.b) || !conn.Send(IpcHistory, IpcEncodeHistory(snapshot))) return;
        connected = true;
        uint32_t type = 0; std::vector<uint8_t> m;
        while (conn.Recv(type, m)) {
            IpcReader r(m.data(), m.size());
            if (type == IpcCmdConfigure) { int iv = 0, hs = 0; std::vector<AlertRule> rules; if (!IpcDecodeConfigure(m.data(), m.size(), iv, hs, rules)) return; tel.Configure(iv, hs, rules); }
            else if (type == IpcCmdMonitor) { const bool on = r.B(); if (!r.Done()) return; if (on) { Hub().Start(); Hub().StartHelper(); } else Hub().StopHelper(); }
            else if (type == IpcCmdFan) {
                const int p = static_cast<int>(r.D()); if (!r.Ok()) return;
                int idx = -1;
                if (!r.Done()) { idx = static_cast<int>(r.D()); if (!r.Done()) return; }
                if (p < 0) { if (idx >= 0) Hub().SetFanAutoFor(idx); else Hub().SetFanAuto(); }
                else if (kFanControlEnabled) { if (idx >= 0) Hub().SetFanManualFor(idx, p); else Hub().SetFanManual(p); }
                else LogW(L"Engine: a manual fan speed was refused (fan control is switched off in this build).");
            }
            else if (type == IpcCmdNet) { const uint32_t mode = r.U32(); const std::wstring guid = r.S(); if (!r.Done() || mode > NetManual || guid.size() > 64) return; tel.SetNetSelection(static_cast<int>(mode), guid); }
            else if (type == IpcCmdShutdown) { PostMessageW(wnd, WM_CLOSE, 0, 0); return; }
            else return;   // unknown message: drop the connection
        }
    }
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        EngineProcess& e = Get();
        if (m == WM_APP + 77) {
            EngineFrame f; MonitorStore ignored; e.tel.Take(f, ignored);
            if (e.connected) { IpcStatus st; st.polls = Hub().polls.load(); st.hubRun = Hub().run.load(); st.pid = GetCurrentProcessId(); e.conn.Send(IpcFrame, IpcEncodeFrame(f, st)); }
            return 0;
        }
        if (m == WM_QUERYENDSESSION) return TRUE;
        if (m == WM_ENDSESSION) { if (w) e.tel.Shutdown(); return 0; }
        if (m == WM_CLOSE || (m == WM_COMMAND && LOWORD(w) == 103)) { DestroyWindow(h); return 0; }   // 103 = Exit: the setup and uninstaller send it to every AnkhCore window
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    }
    int Run(HINSTANCE inst) {
        HANDLE mx = CreateMutexW(nullptr, TRUE, Tagged(L"AnkhCoreEngine").c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) { if (mx) CloseHandle(mx); return 0; }
        WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = Proc; wc.hInstance = inst; static const std::wstring cls = Tagged(L"AnkhCoreEngineWindow"); wc.lpszClassName = cls.c_str(); RegisterClassExW(&wc);
        wnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls.c_str(), L"AnkhCore engine", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);   // never shown; it receives session-end messages
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        tel.Configure(Cfg().Int(L"monitor.intervalSec", 1, 1, 60), Cfg().Int(L"monitor.historyMin", 15, 1, 1440) * 60, AlertEngine().rules);
        tel.SetNetSelection(Cfg().Int(L"sidebar.netMode", NetAuto, NetAuto, NetManual), Cfg().Str(L"sidebar.netAdapter", L""));
        if (Cfg().Bool(L"monitoring.autoStart", true)) Hub().Start();
        tel.Start(wnd, WM_APP + 77);
        HANDLE srv = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { Get().Serve(); return 0; }, nullptr, 0, nullptr);
        LogI(L"Background engine started.");
        MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        SetEvent(stop); if (srv) { WaitForSingleObject(srv, 3000); CloseHandle(srv); }
        tel.Shutdown();
        LogI(L"Background engine stopped.");
        ReleaseMutex(mx); CloseHandle(mx);
        return 0;
    }
};

// ---------------------------------------------------------------- UI client (same calls the UI used in-process)
class EngineClient {
public:
    EngineClient() { InitializeCriticalSection(&cs_); }
    bool Running() const { return connected_.load(); }
    uint64_t Ticks() const { return ticks_.load(); }
    uint32_t EnginePid() const { return pid_.load(); }
    uint64_t Polls() const { return polls_.load(); }
    bool HubRun() const { return hubRun_.load(); }
    int Restarts() const { return restarts_.load(); }
    void Start(HWND notify, UINT msg) {
        if (thread_) return;
        notify_ = notify; msg_ = msg; stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { static_cast<EngineClient*>(p)->Loop(); return 0; }, this, 0, nullptr);
    }
    void Configure(int iv, int hs, const std::vector<AlertRule>& rules) {
        EnterCriticalSection(&cs_); store_.Configure(iv, hs); cfg_ = IpcEncodeConfigure(iv, hs, rules); LeaveCriticalSection(&cs_);
        if (connected_) conn_.Send(IpcCmdConfigure, cfg_);
    }
    void CopyHistory(MonitorStore& out) { EnterCriticalSection(&cs_); out = store_; LeaveCriticalSection(&cs_); }
    void Take(EngineFrame& out, MonitorStore& history) {
        EnterCriticalSection(&cs_); out = frame_; history = store_; frame_.alerts.clear(); LeaveCriticalSection(&cs_);
        posted_ = false;
    }
    void Monitor(bool on) { monitorChoice_ = on ? 1 : 0; IpcWriter w; w.B(on); if (connected_) conn_.Send(IpcCmdMonitor, w.b); }
    void Net(int mode, const std::wstring& guid) {
        IpcWriter w; w.U32(static_cast<uint32_t>(mode)); w.S(guid);
        EnterCriticalSection(&cs_); net_ = w.b; LeaveCriticalSection(&cs_);
        if (connected_) conn_.Send(IpcCmdNet, w.b);
    }
    void Fan(int percent, int fanIndex = -1) { IpcWriter w; w.D(percent); if (fanIndex >= 0) w.D(fanIndex); if (connected_) conn_.Send(IpcCmdFan, w.b); }
    // Tray Exit and session end (D26): the engine, poller and helper all stop.
    void Shutdown() {
        shuttingDown_ = true;
        if (connected_) conn_.Send(IpcCmdShutdown, {});
        HANDLE p = engineProc_ ? engineProc_ : nullptr;
        if (p && WaitForSingleObject(p, 7000) != WAIT_OBJECT_0) { LogW(L"The engine did not stop by itself; ending it."); TerminateProcess(p, 1); WaitForSingleObject(p, 1000); }
        StopThread();
    }
    void TestKillEngine() { if (engineProc_) TerminateProcess(engineProc_, 1); }
private:
    void StopThread() { if (!thread_) return; SetEvent(stop_); WaitForSingleObject(thread_, 3000); CloseHandle(thread_); thread_ = nullptr; CloseHandle(stop_); stop_ = nullptr; }
    void Spawn() {
        const uint64_t now = GetTickCount64();
        if (now - spawnWindow_ > 60000) { spawnWindow_ = now; spawns_ = 0; }
        if (spawns_ >= 3) return;   // bounded: no restart storm
        ++spawns_; if (everConnected_) ++restarts_;
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + SelfPath() + L"\" --engine";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { if (engineProc_) CloseHandle(engineProc_); engineProc_ = pi.hProcess; CloseHandle(pi.hThread); }
        else LogW(L"The background engine could not be started (error " + std::to_wstring(GetLastError()) + L").");
    }
    HANDLE Open() {
        HANDLE h = CreateFileW(EnginePipeName().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h == INVALID_HANDLE_VALUE) return h;
        ULONG pid = 0;
        if (!GetNamedPipeServerProcessId(h, &pid) || !PeerIsSelf(pid)) { LogW(L"The engine pipe belongs to another program; not connecting."); CloseHandle(h); return INVALID_HANDLE_VALUE; }
        return h;
    }
    void MarkLost() {   // no fabricated values: the last reading turns stale and the views say so
        EnterCriticalSection(&cs_); frame_.read.present = false; frame_.read.ageSec = -1; frame_.net.clear(); frame_.sys = SysExtra(); frame_.gpus.clear(); frame_.usage.gpu = -1; LeaveCriticalSection(&cs_);
        if (notify_ && !posted_.exchange(true)) PostMessageW(notify_, msg_, 0, 0);
    }
    void Loop() {
        int fails = 0; bool spawned = false;
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            HANDLE h = Open();
            if (h == INVALID_HANDLE_VALUE) {
                if (!spawned || ++fails >= 20) { if (!shuttingDown_) Spawn(); spawned = true; fails = 0; }
                WaitForSingleObject(stop_, 150);
                continue;
            }
            conn_.h = h; conn_.stop = stop_; fails = 0;
            Session();
            connected_ = false; conn_.h = INVALID_HANDLE_VALUE; CloseHandle(h); spawned = false;
            if (shuttingDown_) break;
            LogW(L"The background engine connection was lost.");
            MarkLost();
        }
    }
    void Session() {
        uint32_t type = 0; std::vector<uint8_t> m;
        while (conn_.Recv(type, m)) {
            if (type == IpcHello) {
                IpcReader r(m.data(), m.size()); const uint32_t ver = r.U32(), pid = r.U32();
                if (!r.Done() || ver != kIpcVersion) return;
                pid_ = pid; everConnected_ = true;
                if (!engineProc_) engineProc_ = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
            } else if (type == IpcHistory) {
                MonitorStore s; if (!IpcDecodeHistory(m.data(), m.size(), s)) return;
                EnterCriticalSection(&cs_); store_ = s; const auto cfg = cfg_, net = net_; LeaveCriticalSection(&cs_);
                connected_ = true;
                if (!cfg.empty()) conn_.Send(IpcCmdConfigure, cfg);
                if (!net.empty()) conn_.Send(IpcCmdNet, net);
                if (monitorChoice_ == 1 || (monitorChoice_ == -1 && lastHubRun_)) { IpcWriter w; w.B(true); conn_.Send(IpcCmdMonitor, w.b); }   // a restarted engine carries on monitoring
            } else if (type == IpcFrame) {
                EngineFrame f; IpcStatus st; if (!IpcDecodeFrame(m.data(), m.size(), f, st)) return;
                EnterCriticalSection(&cs_);
                store_.Tick(f.sampleSec, f.sample);
                for (const auto& a : frame_.alerts) f.alerts.insert(f.alerts.begin(), a);   // not yet taken: keep them
                frame_ = f;
                LeaveCriticalSection(&cs_);
                polls_ = st.polls; hubRun_ = st.hubRun; ++ticks_; if (st.hubRun) lastHubRun_ = true;
                if (notify_ && !posted_.exchange(true)) PostMessageW(notify_, msg_, 0, 0);
            } else return;
        }
    }
    CRITICAL_SECTION cs_; MonitorStore store_; EngineFrame frame_; std::vector<uint8_t> cfg_, net_; PipeConn conn_;
    HWND notify_ = nullptr; UINT msg_ = 0; HANDLE thread_ = nullptr, stop_ = nullptr, engineProc_ = nullptr;
    std::atomic<bool> connected_{false}, posted_{false}, hubRun_{false}, shuttingDown_{false}, everConnected_{false};
    std::atomic<int> monitorChoice_{-1}; std::atomic<bool> lastHubRun_{false};
    std::atomic<uint32_t> pid_{0}; std::atomic<uint64_t> ticks_{0}, polls_{0}; std::atomic<int> restarts_{0};
    uint64_t spawnWindow_ = 0; int spawns_ = 0;
};

}  // namespace ak
