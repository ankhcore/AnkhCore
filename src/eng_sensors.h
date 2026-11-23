// AnkhCore 2.2 — sensor hub. Launches and reads the AnkhSensors helper (C#,
// built on LibreHardwareMonitor) which publishes a flat key=value readings
// file every second; sends fan commands back through a command file. Every
// reading is optional and shows as N/A when the helper or a sensor is absent.
#pragma once
#include "base.h"

namespace ak {

inline std::wstring SensorsDir() { return AppDir() + L"\\Sensors"; }
inline std::wstring SensorsExe() { return SensorsDir() + L"\\AnkhSensors.exe"; }
inline std::wstring SensorsDataDir() { return DataDir() + L"\\Sensors"; }
// The test build reads the installed helper's readings file (read-only) so the UI shows live data.
inline std::wstring ReadingsFile() { return (kTestBuild ? ProdDataDir() + L"\\Sensors" : SensorsDataDir()) + L"\\readings.txt"; }
inline std::wstring CommandFile() { return SensorsDataDir() + L"\\command.txt"; }
inline std::wstring FanConfigDir() { return SensorsDir() + L"\\FanConfigs"; }

enum class FanCapability {
    Unknown,        // older helper or capability not reported
    MonitorOnly,    // RPM sensor only, no writable control
    VendorReadOnly, // vendor WMI read-only sensor (e.g. HP BIOS)
    Controllable,   // hardware/LHM control exposed and within valid range
    ControlOnly     // control exposed without RPM sensor
};

struct FanReading {
    std::wstring name; double rpm = -1; bool controllable = false; double percent = -1;
    std::wstring source;   // "LibreHardwareMonitor", "HP BIOS", or "" (older helper)
    bool live = true;      // vendor readings: false until the value has been seen to change
    FanCapability capability = FanCapability::Unknown;
    double minPercent = -1, maxPercent = -1;
    std::wstring controlMode;   // "software" | "default" | "undefined" | ""
};

inline FanCapability ParseFanCapability(const std::wstring& s) {
    if (s == L"controllable") return FanCapability::Controllable;
    if (s == L"monitor_only") return FanCapability::MonitorOnly;
    if (s == L"vendor_readonly") return FanCapability::VendorReadOnly;
    if (s == L"control_only") return FanCapability::ControlOnly;
    return FanCapability::Unknown;
}

inline std::wstring FanCapabilityLabel(const FanReading& f) {
    switch (f.capability) {
        case FanCapability::Controllable:   return L"Controllable";
        case FanCapability::VendorReadOnly: return L"Vendor read-only";
        case FanCapability::ControlOnly:    return L"Control only";
        case FanCapability::MonitorOnly:    return L"Monitoring only";
        default: return f.controllable ? L"Controllable" : (f.source == L"HP BIOS" ? L"Vendor read-only" : L"Monitoring only");
    }
}

struct Readings {
    bool present = false;          // the helper produced a readings file recently
    bool everSeen = false;         // a readings file exists at all
    double ageSec = -1;            // seconds since the helper last wrote the file (-1 = no file)
    SYSTEMTIME stamp{};            // local time of that write
    std::wstring time;
    double cpuTemp = -1, cpuLoad = -1, cpuClock = -1, cpuPower = -1;
    std::wstring gpuName;
    double gpuTemp = -1, gpuHotspot = -1, gpuLoad = -1, gpuClock = -1, gpuPower = -1, gpuVramUsed = -1, gpuVramLoad = -1;
    double ramLoad = -1;
    double diskTemp = -1, diskLoad = -1;
    double netDown = -1, netUp = -1;
    std::vector<FanReading> fans;
    int controls = 0;
    // fan control state as read back by the helper (2.5)
    std::wstring fanMode;                       // "auto" | "manual" | "" (older helper)
    double fanRequested = -1, fanReadback = -1;
    std::wstring fanReason, fanError;
    std::wstring model, nbfcMatch;
    std::wstring ecState;   // 2.8: the state of AnkhCore's own fan-control engine ("ready: ...", "active: <model>, N fan(s)", "not active: <reason>", "unavailable: <reason>")
    int ControllableFanCount() const {
        int n = 0; for (const auto& f : fans) if (f.controllable) ++n;
        return n;
    }
    int MonitorOnlyFanCount() const {
        int n = 0; for (const auto& f : fans) if (!f.controllable) ++n;
        return n;
    }
};

inline double ToNum(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r')) --b;
    if (a == b) return -1.0;
    const std::wstring trimmed = s.substr(a, b - a);
    wchar_t* end = nullptr;
    const double v = std::wcstod(trimmed.c_str(), &end);
    if (end == trimmed.c_str() || *end != L'\0' || !std::isfinite(v)) return -1.0;
    return v;
}

// C6: Consistent CPU usage across Dashboard, System Monitor, Sidebar, Mini Monitor, and Hardware Info.
// Uses the authoritative Windows CPU counter when available; falls back to fresh helper CPU.LOAD.
inline double EffectiveCpuLoad(double winCpu, const Readings& r) {
    if (winCpu >= 0.0 && std::isfinite(winCpu)) return winCpu;
    if (r.present && r.cpuLoad >= 0.0 && std::isfinite(r.cpuLoad)) return r.cpuLoad;
    return -1.0;
}

// C6: Distinguishes unavailable/stale helper telemetry (!r.present) from valid zero readings (0.0 when r.present).
inline double HelperReading(const Readings& r, double val) {
    return (r.present && val >= 0.0 && std::isfinite(val)) ? val : -1.0;
}

// Parses the flat key=value readings file into a Readings struct.
// Freshness of the readings file from its write time (both in FILETIME units, 100 ns).
// Fresh = written less than 5 s ago. A write time slightly *after* "now" is normal: the file
// system stamps with the precise clock, GetSystemTimeAsFileTime is coarser (~15.6 ms), so up to
// 2 s ahead counts as age 0. Further ahead means the clock moved back: not trusted as fresh.
struct Freshness { bool present; double ageSec; };
constexpr uint64_t kFreshFt = 50000000ull, kSkewFt = 20000000ull;   // 5 s, 2 s
inline Freshness ReadingsFreshness(uint64_t nowFt, uint64_t writeFt) {
    if (writeFt >= nowFt) return {writeFt - nowFt <= kSkewFt, 0.0};
    const uint64_t d = nowFt - writeFt;
    return {d < kFreshFt, d / 1e7};
}

inline Readings ParseReadings(const std::string& utf8) {
    Readings r;
    const std::wstring all = Utf8ToW(utf8);
    std::map<int, FanReading> fans;
    for (const auto& lineRaw : Split(all, L'\n')) {
        std::wstring line = lineRaw; if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == L"TIME") r.time = v;
        else if (k == L"CPU.TEMP") r.cpuTemp = ToNum(v);
        else if (k == L"CPU.LOAD") r.cpuLoad = ToNum(v);
        else if (k == L"CPU.CLOCK") r.cpuClock = ToNum(v);
        else if (k == L"CPU.POWER") r.cpuPower = ToNum(v);
        else if (k == L"GPU.NAME") { if (r.gpuName.empty()) r.gpuName = v; }
        else if (k == L"GPU.TEMP") r.gpuTemp = ToNum(v);
        else if (k == L"GPU.HOTSPOT") r.gpuHotspot = ToNum(v);
        else if (k == L"GPU.LOAD") r.gpuLoad = ToNum(v);
        else if (k == L"GPU.CLOCK") r.gpuClock = ToNum(v);
        else if (k == L"GPU.POWER") r.gpuPower = ToNum(v);
        else if (k == L"GPU.VRAMUSED") r.gpuVramUsed = ToNum(v);
        else if (k == L"GPU.VRAMLOAD") r.gpuVramLoad = ToNum(v);
        else if (k == L"RAM.LOAD") r.ramLoad = ToNum(v);
        else if (k == L"DISK.TEMP") r.diskTemp = ToNum(v);
        else if (k == L"DISK.LOAD") r.diskLoad = ToNum(v);
        else if (k == L"NET.DOWN") r.netDown = ToNum(v);
        else if (k == L"NET.UP") r.netUp = ToNum(v);
        else if (k == L"CONTROLS") r.controls = _wtoi(v.c_str());
        else if (k == L"FANCTL.MODE") r.fanMode = v;
        else if (k == L"FANCTL.REQUESTED") r.fanRequested = ToNum(v);
        else if (k == L"FANCTL.READBACK") r.fanReadback = ToNum(v);
        else if (k == L"FANCTL.REASON") r.fanReason = v;
        else if (k == L"FANCTL.ERROR") r.fanError = v;
        else if (k == L"MODEL") r.model = v;
        else if (k == L"NBFC_MATCH") r.nbfcMatch = v;
        else if (k == L"ECFAN.STATE") r.ecState = v;
        else if (StartsWith(k, L"FAN.")) {
            const auto parts = Split(k, L'.');
            if (parts.size() == 3) {
                const int idx = _wtoi(parts[1].c_str());
                FanReading& f = fans[idx];
                if (parts[2] == L"NAME") f.name = v;
                else if (parts[2] == L"RPM") f.rpm = ToNum(v);
                else if (parts[2] == L"CONTROL") f.controllable = v == L"1";
                else if (parts[2] == L"PERCENT") f.percent = ToNum(v);
                else if (parts[2] == L"SOURCE") f.source = v;
                else if (parts[2] == L"LIVE") f.live = v == L"1";
                else if (parts[2] == L"CAPABILITY") f.capability = ParseFanCapability(v);
                else if (parts[2] == L"MINPCT") f.minPercent = ToNum(v);
                else if (parts[2] == L"MAXPCT") f.maxPercent = ToNum(v);
                else if (parts[2] == L"CTRLMODE") f.controlMode = v;
            }
        }
    }
    for (auto& kv : fans) {
        FanReading& f = kv.second;
        if (f.capability == FanCapability::Unknown) {   // backward compatibility with older helpers
            if (f.controllable) f.capability = FanCapability::Controllable;
            else if (f.source == L"HP BIOS") f.capability = FanCapability::VendorReadOnly;
            else if (f.rpm >= 0) f.capability = FanCapability::MonitorOnly;
        }
        r.fans.push_back(f);
    }
    return r;
}

// Background reader + helper manager (one instance, polled by the UI).
struct SensorHub {
    CRITICAL_SECTION cs;
    Readings latest;
    HANDLE proc = nullptr;
    std::atomic<bool> run{false};
    HANDLE thread = nullptr;
    FILETIME lastStamp{};
    CRITICAL_SECTION cmdCs;   // the fan-command state is used by both the UI and the engine thread
    SensorHub() { InitializeCriticalSection(&cs); InitializeCriticalSection(&cmdCs); }

    bool HelperInstalled() { return FileExists(SensorsExe()); }
    bool HelperRunning() { return ProcessRunning(L"AnkhSensors.exe"); }

    void StartHelper() {
        if (kTestBuild) { TestRefused(L"start the sensor helper (the installed AnkhCore owns it)"); return; }
        if (!HelperInstalled() || HelperRunning()) return;
        EnsureDir(SensorsDataDir());
        STARTUPINFOW si{sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + SensorsExe() + L"\"";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, SensorsDir().c_str(), &si, &pi)) {
            proc = pi.hProcess; CloseHandle(pi.hThread);
            LogI(L"Hardware monitoring started.");
        } else LogW(L"Hardware monitoring helper could not be started (error " + std::to_wstring(GetLastError()) + L").");
    }
    // Graceful stop: the helper restores automatic fan control itself on "quit" (2.2 killed it,
    // so a manual speed stayed set after AnkhCore exited). Forced kill only as a last resort.
    void StopHelper() {
        if (kTestBuild) { TestRefused(L"stop the sensor helper"); return; }
        manual = false;
        WriteCommand("quit");
        for (int i = 0; i < 40 && HelperRunning(); ++i) Sleep(100);
        if (HelperRunning()) { LogW(L"The sensor helper did not stop by itself; ending it."); KillProcess(L"AnkhSensors.exe", 0); }
    }

    Readings Get() { EnterCriticalSection(&cs); Readings r = latest; LeaveCriticalSection(&cs); return r; }

    std::atomic<uint64_t> polls{0};   // proves a single poller: rises once per second, whatever the number of views
    // One poll per second (the only reader of readings.txt). The helper replaces the file every
    // second: the 2.5 helper with File.Replace, the installed 2.2 helper with Delete + Move, which
    // leaves a short window with no file (measured: about 1 in 3,000 reads). A bounded retry inside
    // this poll rides over that window; if the file is still missing, the last reading is kept and
    // aged from its own write time, so it turns stale after 5 s instead of flickering to "no data".
    void Poll() {
        ++polls;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        std::string text;
        bool ok = false;
        for (int attempt = 0; attempt < kPollAttempts && !ok; ++attempt) {
            if (attempt) Sleep(kPollRetryMs);
            ok = GetFileAttributesExW(ReadingsFile().c_str(), GetFileExInfoStandard, &fa) && !(text = ReadText(ReadingsFile())).empty();
        }
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const uint64_t nowFt = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        if (!ok) {
            EnterCriticalSection(&cs);
            if (latest.everSeen && lastFileTime) { const auto f = ReadingsFreshness(nowFt, lastFileTime); latest.present = f.present; latest.ageSec = f.ageSec; }
            else { latest.present = false; latest.ageSec = -1; }
            LeaveCriticalSection(&cs);
            return;
        }
        Readings r = ParseReadings(text);
        const uint64_t writeFt = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        const auto f = ReadingsFreshness(nowFt, writeFt);
        r.present = f.present;
        r.ageSec = f.ageSec;
        r.everSeen = true;
        FILETIME lf; SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lf) && FileTimeToSystemTime(&lf, &st)) r.stamp = st;
        EnterCriticalSection(&cs); latest = r; lastFileTime = writeFt; LeaveCriticalSection(&cs);
    }
    static constexpr int kPollAttempts = 3, kPollRetryMs = 20;
    uint64_t lastFileTime = 0;   // write time of the last readings file read (FILETIME units)

    bool manual = false; std::string lastCmd; DWORD lastWrite = 0;
    bool WriteCommand(const std::string& s) {
        EnterCriticalSection(&cmdCs);
        EnsureDir(SensorsDataDir());
        lastCmd = s; lastWrite = GetTickCount();
        const bool ok = WriteAll(CommandFile(), reinterpret_cast<const uint8_t*>(s.data()), s.size());
        LeaveCriticalSection(&cmdCs);
        return ok;
    }
    // Manual control is a lease: renewed every 3 s while AnkhCore runs; the helper returns
    // to automatic by itself if renewals stop for 10 s (crash, hang, logoff).
    void RenewLease() { EnterCriticalSection(&cmdCs); if (manual && GetTickCount() - lastWrite >= 3000) WriteCommand(lastCmd); LeaveCriticalSection(&cmdCs); }
    void SetFanAuto() { if (kTestBuild) { TestRefused(L"fan command 'auto' (fan control is not tested in this build)"); return; } manual = false; WriteCommand("auto"); LogI(L"Fans: automatic requested (the result is shown from the helper's read-back)."); }
    void SetFanAutoFor(int fanIndex) {
        if (fanIndex < 0) { SetFanAuto(); return; }
        if (kTestBuild) { TestRefused(L"fan command 'auto' for fan " + std::to_wstring(fanIndex)); return; }
        manual = false; WriteCommand("auto " + std::to_string(fanIndex));
        LogI(L"Fan " + std::to_wstring(fanIndex + 1) + L": automatic requested.");
    }
    static constexpr int kMinFanPercent = 40;   // D23: mandatory 40% minimum, aligned with FanPolicy and ClampFanDuty40
    void SetFanManual(int percent) {
        if (kTestBuild) { TestRefused(L"fan command 'manual' (fan control is not tested in this build)"); return; }
        const int p = std::clamp(percent, kMinFanPercent, 100);
        manual = WriteCommand("manual " + std::to_string(p));
        LogI(L"Fans: manual " + std::to_wstring(p) + L"% requested (not confirmed until the helper reads it back).");
    }
    void SetFanManualFor(int fanIndex, int percent) {
        if (fanIndex < 0) { SetFanManual(percent); return; }
        if (kTestBuild) { TestRefused(L"fan command 'manual' for fan " + std::to_wstring(fanIndex)); return; }
        const int p = std::clamp(percent, kMinFanPercent, 100);
        manual = WriteCommand("manual " + std::to_string(fanIndex) + " " + std::to_string(p));
        LogI(L"Fan " + std::to_wstring(fanIndex + 1) + L": manual " + std::to_wstring(p) + L"% requested.");
    }
    // Called every second with fresh readings: notice when the helper left manual mode on its own.
    void Reconcile(const Readings& r) {
        EnterCriticalSection(&cmdCs);
        if (manual && r.present && r.fanMode == L"auto" && GetTickCount() - lastWrite > 2500) {
            manual = false;
            LogW(L"Fans returned to automatic: " + (r.fanError.empty() ? r.fanReason : r.fanError));
        }
        LeaveCriticalSection(&cmdCs);
    }

    void Start() {
        if (run) return;
        run = true;
        StartHelper();
        thread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
            auto* h = static_cast<SensorHub*>(p);
            while (h->run) { h->Poll(); Sleep(1000); }
            return 0; }, this, 0, nullptr);
    }
    void Stop() { run = false; if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); thread = nullptr; } }
};
inline SensorHub& Hub() { static SensorHub h; return h; }

// Install-time build of the helper with the in-box C# compiler. Returns the
// compiler output so the setup can log it. The .cs source, LibreHardwareMonitor
// DLLs and a manifest are laid down next to it first.
inline std::wstring CscPath() {
    const std::wstring p = WinDir() + L"\\Microsoft.NET\\Framework64\\v4.0.30319\\csc.exe";
    return FileExists(p) ? p : WinDir() + L"\\Microsoft.NET\\Framework\\v4.0.30319\\csc.exe";
}
inline bool BuildSensorsHelper(std::wstring& log) {
    const std::wstring dir = SensorsDir(), csc = CscPath();
    if (!FileExists(csc)) { log = L"The in-box C# compiler was not found; hardware monitoring will be unavailable."; return false; }
    if (!FileExists(dir + L"\\AnkhSensors.cs")) { log = L"AnkhSensors.cs is missing."; return false; }
    if (!FileExists(dir + L"\\LibreHardwareMonitorLib.dll")) { log = L"LibreHardwareMonitorLib.dll is missing."; return false; }
    const std::wstring args =
        L"/nologo /target:winexe /platform:x64 /optimize+ "
        L"/out:\"" + dir + L"\\AnkhSensors.exe\" "
        L"/reference:\"" + dir + L"\\LibreHardwareMonitorLib.dll\" "
        L"/reference:System.Management.dll "
        L"/win32manifest:\"" + dir + L"\\AnkhSensors.manifest\" "
        L"\"" + dir + L"\\AnkhSensors.cs\"";
    std::wstring out;
    const DWORD rc = RunCapture(csc, args, [&](const std::wstring& l) { out += l + L"\n"; }, 120000);
    log = out;
    const bool ok = rc == 0 && FileExists(SensorsExe());
    LogI(ok ? L"Hardware monitoring helper built." : L"Hardware monitoring helper could not be built (csc " + std::to_wstring(rc) + L").");
    return ok;
}

}  // namespace ak
