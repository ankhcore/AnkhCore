// AnkhCore 2.2 — fan control via NBFC (NoteBook FanControl / nbfc-revive).
// Laptops expose fan control through the embedded controller, not through the
// generic sensor libraries, so AnkhCore drives the bundled NBFC: it carries a
// tested EC config per laptop model and a safe "return to automatic" path.
// The model config is chosen by the user (manually if auto-detect misses), and
// every change is reversible — NBFC hands control back to the BIOS on "auto".
#pragma once
#include "base.h"
#include "install.h"

namespace ak {

// Mandatory 40% minimum fan duty across all AnkhCore fan logic (D23).
constexpr int kMinFanDutyPct = 40;
inline int ClampFanDuty40(int pct) {
    return std::clamp(pct, kMinFanDutyPct, 100);
}

inline std::wstring FanDir()        { return AppDir() + L"\\Fan"; }
// 2.8: the notebook fan configurations ship inside AnkhCore (Sensors\FanConfigs, installed by AnkhCore Setup); nothing is read from another PC folder.
inline std::wstring FanConfigsDir() { return AppDir() + L"\\Sensors\\FanConfigs"; }
inline std::wstring DevFanConfigsDir() { return FanConfigsDir(); }   // no developer-PC folder any more
inline std::wstring NbfcExe() {
    const std::wstring bundled = FanDir() + L"\\nbfc.exe";
    if (FileExists(bundled)) return bundled;
    const std::wstring pf86 = L"C:\\Program Files (x86)\\NoteBook FanControl\\nbfc.exe";
    if (FileExists(pf86)) return pf86;
    const std::wstring pf64 = L"C:\\Program Files\\NoteBook FanControl\\nbfc.exe";
    if (FileExists(pf64)) return pf64;
    return bundled;
}
inline std::wstring NbfcServiceExe() {
    const std::wstring bundled = FanDir() + L"\\NbfcService.exe";
    if (FileExists(bundled)) return bundled;
    const std::wstring d = DirOf(NbfcExe());
    if (!d.empty() && FileExists(d + L"\\NbfcService.exe")) return d + L"\\NbfcService.exe";
    return bundled;
}
inline std::wstring FanGuiExe() {
    const std::wstring bundled = FanDir() + L"\\NoteBookFanControl.exe";
    if (FileExists(bundled)) return bundled;
    const std::wstring d = DirOf(NbfcExe());
    if (!d.empty() && FileExists(d + L"\\NoteBookFanControl.exe")) return d + L"\\NoteBookFanControl.exe";
    return bundled;
}

inline bool FanInstalled() { return FileExists(FanGuiExe()) || FileExists(NbfcExe()); }

// ---------------------------------------------------------------- HW-4 / D42: Try Fan Compatibility
struct FanCompatCandidate {
    std::wstring name;           // config stem (e.g. "HP ZBook Fury 15 G7")
    std::wstring notebookModel;  // <NotebookModel> from XML
    std::wstring author;         // <Author> from XML
    std::wstring path;           // full path to .xml file
    std::string  sha256;         // SHA-256 of XML bytes
    int fanCount = 0;            // number of <FanConfiguration> entries
    int criticalTempC = 0;       // <CriticalTemperature> from XML
    bool exactModelMatch = false;
    bool familyMatch = false;
};

inline std::string ExtractXmlTagText(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t p1 = xml.find(open);
    if (p1 == std::string::npos) return {};
    const size_t start = p1 + open.size();
    const size_t p2 = xml.find(close, start);
    if (p2 == std::string::npos) return {};
    std::string val = xml.substr(start, p2 - start);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t' || val.front() == '\r' || val.front() == '\n')) val.erase(val.begin());
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
    return val;
}

// 2.8.7: the model text of a PC often repeats the maker ("HP HP ZBook 15 G5"), which matched no configuration at all. Repeated neighbouring words are collapsed.
inline std::wstring FanNormalizeModel(const std::wstring& model) {
    std::wstring out, prev;
    for (const std::wstring& w : Split(Lower(model), L' ')) {
        if (w.empty() || w == prev) continue;
        if (!out.empty()) out += L' ';
        out += w; prev = w;
    }
    return out;
}

inline bool ParseNbfcConfigXml(const std::string& xml,
                               const std::wstring& fileStem,
                               const std::wstring& fullPath,
                               const std::wstring& deviceModel,
                               FanCompatCandidate& out) {
    out = FanCompatCandidate{};
    if (xml.find("<FanControlConfig") == std::string::npos || xml.find("<FanConfigurations>") == std::string::npos) {
        return false;
    }
    out.name = fileStem;
    out.path = fullPath;
    out.sha256 = Sha256Hex(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
    const std::string model = ExtractXmlTagText(xml, "NotebookModel");
    out.notebookModel = model.empty() ? fileStem : Utf8ToW(model);
    out.author = Utf8ToW(ExtractXmlTagText(xml, "Author"));
    const std::string crit = ExtractXmlTagText(xml, "CriticalTemperature");
    if (!crit.empty()) out.criticalTempC = std::atoi(crit.c_str());

    size_t pos = 0;
    while ((pos = xml.find("<FanConfiguration", pos)) != std::string::npos) {
        const char nextCh = (pos + 17 < xml.size()) ? xml[pos + 17] : '\0';
        if (nextCh == '>' || nextCh == '/' || nextCh == ' ' || nextCh == '\t' || nextCh == '\r' || nextCh == '\n') {
            ++out.fanCount;
        }
        pos += 17;
    }
    if (out.fanCount <= 0) return false;

    const std::wstring devLow = FanNormalizeModel(deviceModel);
    const std::wstring cfgLow = FanNormalizeModel(out.notebookModel);
    const std::wstring stemLow = FanNormalizeModel(out.name);
    if (!devLow.empty()) {
        out.exactModelMatch = (devLow == cfgLow || devLow == stemLow);
        if (!out.exactModelMatch) {
            // Check vendor + product family prefix (first two words, e.g. "hp zbook")
            const auto devWords = Split(devLow, L' ');
            if (devWords.size() >= 2) {
                const std::wstring prefix = devWords[0] + L" " + devWords[1];
                out.familyMatch = (cfgLow.rfind(prefix, 0) == 0 || stemLow.rfind(prefix, 0) == 0);
            } else if (!devWords.empty() && devWords[0].size() >= 2) {
                out.familyMatch = (cfgLow.rfind(devWords[0], 0) == 0 || stemLow.rfind(devWords[0], 0) == 0);
            }
        }
    }
    return true;
}

// Discovers local NBFC XML configurations read-only from verified directories.
inline std::vector<FanCompatCandidate> DiscoverFanCompatCandidates(const std::wstring& deviceModel,
                                                                   const std::vector<std::wstring>& extraDirs = {}) {
    std::vector<std::wstring> dirs = extraDirs;
    if (dirs.empty()) {
        if (DirExists(FanConfigsDir())) dirs.push_back(FanConfigsDir());
        const std::wstring nbfcCfgs = DirOf(NbfcExe()) + L"\\Configs";
        if (!nbfcCfgs.empty() && DirExists(nbfcCfgs)) dirs.push_back(nbfcCfgs);
        if (DirExists(DevFanConfigsDir())) dirs.push_back(DevFanConfigsDir());
    }
    std::vector<FanCompatCandidate> out;
    std::set<std::wstring> seenNames;
    for (const auto& dir : dirs) {
        if (!DirExists(dir)) continue;
        for (const auto& f : ListDir(dir, false, L"*.xml")) {
            std::wstring stem = f;
            if (EndsWithI(stem, L".xml")) stem.resize(stem.size() - 4);
            const std::wstring key = Lower(stem);
            if (!seenNames.insert(key).second) continue;
            const std::wstring full = dir + L"\\" + f;
            const std::string xml = ReadText(full);
            if (xml.empty()) continue;
            FanCompatCandidate c;
            if (ParseNbfcConfigXml(xml, stem, full, deviceModel, c)) {
                out.push_back(std::move(c));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const FanCompatCandidate& a, const FanCompatCandidate& b) {
        if (a.exactModelMatch != b.exactModelMatch) return a.exactModelMatch > b.exactModelMatch;
        if (a.familyMatch != b.familyMatch) return a.familyMatch > b.familyMatch;
        return Lower(a.name) < Lower(b.name);
    });
    return out;
}

// Pure telemetry snapshot from SensorHub (NBFC temperature is NEVER used; D21, D22, D42).
struct FanCompatTelemetry {
    bool fresh = false;
    double cpuTempC = -1.0;
    double gpuTempC = -1.0;
    std::vector<double> rpms;   // measured RPMs from SensorHub
};

enum class FanCompatStage {
    Idle,
    Baseline,    // observing pre-trial SensorHub baseline RPM & temperatures
    Testing,     // holding candidate at clamped trial duty (>= 40%) and observing RPM response
    Restoring,   // restoring safe state after candidate
    Completed,   // finished one candidate
    Aborted      // aborted due to safety gate, stale telemetry, timeout, backend error, or user Stop
};

enum class FanCompatVerdict {
    Untested,
    Compatible,      // real, attributable SensorHub RPM response + passed all safety checks
    NoRpmResponse,   // commands succeeded and temperatures stayed safe, but SensorHub saw no attributable RPM response
    AbortedSafety,   // aborted by thermal gate, invalid/stale SensorHub reading, or backend failure
    AbortedUser      // stopped by user
};

struct FanCompatRecord {
    std::wstring candidateName;
    std::wstring sourcePath;
    std::string  sha256;
    int requestedDutyPct = kMinFanDutyPct;
    double baselineCpuTempC = -1.0;
    double peakCpuTempC = -1.0;
    double baselineRpm = -1.0;
    double peakObservedRpm = -1.0;
    double postRestoreRpm = -1.0;
    bool zeroRpmCoolIdleObserved = false;
    bool commandSucceeded = false;
    bool restoredSafeState = false;
    FanCompatVerdict verdict = FanCompatVerdict::Untested;
    std::wstring reason;
};

// Pure, deterministic state machine for "Try Fan Compatibility" (HW-4 / D42).
// Never executes external processes itself; all actuator steps and telemetry are passed in,
// and in production `kFanControlEnabled = false` prevents any live hardware execution.
struct FanCompatTrial {
    // Safety & attribution parameters
    double maxSafeTempC = 85.0;       // abort immediately if SensorHub CPU or GPU temp >= maxSafeTempC
    int    maxHoldSeconds = 12;       // bounded trial window per candidate
    double minActiveRpm = 1200.0;     // minimum RPM considered a spinning fan response
    double minDeltaRpm = 350.0;       // minimum RPM increase over baseline to attribute response

    FanCompatStage stage = FanCompatStage::Idle;
    FanCompatCandidate current;
    FanCompatRecord record;
    int elapsedHoldSec = 0;

    static double MaxRpmOf(const std::vector<double>& rpms) {
        double m = -1.0;
        for (double v : rpms) if (v > m) m = v;
        return m;
    }

    static bool TelemetrySafe(const FanCompatTelemetry& t, double maxTempC, std::wstring& why) {
        if (!t.fresh) { why = L"SensorHub telemetry is stale or unavailable."; return false; }
        if (t.cpuTempC <= 0.0 || t.cpuTempC > 115.0) { why = L"SensorHub CPU temperature is missing or implausible."; return false; }
        if (t.cpuTempC >= maxTempC) { why = L"CPU temperature reached safety limit (" + std::to_wstring(static_cast<int>(t.cpuTempC)) + L" \x00B0C)."; return false; }
        if (t.gpuTempC >= maxTempC) { why = L"GPU temperature reached safety limit (" + std::to_wstring(static_cast<int>(t.gpuTempC)) + L" \x00B0C)."; return false; }
        if (t.rpms.empty()) { why = L"No fan RPM sensor exposed in SensorHub."; return false; }
        return true;
    }

    // Step 1: Explicit user start for a single candidate. Refuses if another candidate is already active.
    bool StartCandidate(const FanCompatCandidate& cand, int requestedDutyPct, const FanCompatTelemetry& initialTel) {
        if (stage == FanCompatStage::Baseline || stage == FanCompatStage::Testing || stage == FanCompatStage::Restoring) {
            return false; // never run two configurations concurrently
        }
        current = cand;
        record = FanCompatRecord{};
        record.candidateName = cand.name;
        record.sourcePath = cand.path;
        record.sha256 = cand.sha256;
        record.requestedDutyPct = ClampFanDuty40(requestedDutyPct); // D23: mandatory 40% minimum
        elapsedHoldSec = 0;

        std::wstring why;
        if (!TelemetrySafe(initialTel, maxSafeTempC, why)) {
            stage = FanCompatStage::Aborted;
            record.verdict = FanCompatVerdict::AbortedSafety;
            record.reason = why;
            return false;
        }
        record.baselineCpuTempC = initialTel.cpuTempC;
        record.peakCpuTempC = initialTel.cpuTempC;
        record.baselineRpm = MaxRpmOf(initialTel.rpms);
        record.peakObservedRpm = record.baselineRpm;
        // A reading of 0 RPM while cool (< 55 C) is noted as natural cool-idle stop, not unsupported sensor
        if (record.baselineRpm == 0.0 && initialTel.cpuTempC < 55.0) {
            record.zeroRpmCoolIdleObserved = true;
        }
        stage = FanCompatStage::Baseline;
        return true;
    }

    // Step 2: Record the result of applying the candidate config + setting `record.requestedDutyPct`.
    bool OnCommandResult(bool ok, const std::wstring& errText = L"") {
        if (stage != FanCompatStage::Baseline) return false;
        record.commandSucceeded = ok;
        if (!ok) {
            stage = FanCompatStage::Restoring;
            record.verdict = FanCompatVerdict::AbortedSafety;
            record.reason = errText.empty() ? L"Backend command failed." : errText;
            return false;
        }
        stage = FanCompatStage::Testing;
        return true;
    }

    // Step 3: Continuous 1 Hz SensorHub telemetry check during the bounded hold window.
    FanCompatStage TickHold(const FanCompatTelemetry& tel) {
        if (stage != FanCompatStage::Testing) return stage;
        ++elapsedHoldSec;
        if (tel.cpuTempC > record.peakCpuTempC) record.peakCpuTempC = tel.cpuTempC;
        const double curRpm = MaxRpmOf(tel.rpms);
        if (curRpm > record.peakObservedRpm) record.peakObservedRpm = curRpm;

        std::wstring why;
        if (!TelemetrySafe(tel, maxSafeTempC, why)) {
            stage = FanCompatStage::Restoring;
            record.verdict = FanCompatVerdict::AbortedSafety;
            record.reason = why;
            return stage;
        }

        // Check whether SensorHub has observed a real, attributable RPM response
        const bool roseFromZero = (record.baselineRpm <= 0.0 && record.peakObservedRpm >= minActiveRpm);
        const bool roseFromSpin = (record.baselineRpm > 0.0 &&
                                   record.peakObservedRpm >= record.baselineRpm + minDeltaRpm &&
                                   record.peakObservedRpm >= minActiveRpm);
        if (roseFromZero || roseFromSpin) {
            stage = FanCompatStage::Restoring;
            record.verdict = FanCompatVerdict::Compatible;
            record.reason = L"SensorHub observed attributable RPM response (" +
                            std::to_wstring(static_cast<int>(record.baselineRpm)) + L" -> " +
                            std::to_wstring(static_cast<int>(record.peakObservedRpm)) + L" RPM).";
            return stage;
        }

        if (elapsedHoldSec >= maxHoldSeconds) {
            stage = FanCompatStage::Restoring;
            record.verdict = FanCompatVerdict::NoRpmResponse;
            record.reason = record.zeroRpmCoolIdleObserved
                ? L"Trial timed out with no attributable RPM rise (fan started at 0 RPM while cool; NBFC command alone does not prove compatibility)."
                : L"Trial timed out with no attributable RPM rise in SensorHub (" +
                  std::to_wstring(static_cast<int>(record.baselineRpm)) + L" -> " +
                  std::to_wstring(static_cast<int>(record.peakObservedRpm)) + L" RPM).";
        }
        return stage;
    }

    // Step 4: User Stop / Cancel at any time -> immediately transition to safe restore.
    void Cancel() {
        if (stage == FanCompatStage::Baseline || stage == FanCompatStage::Testing) {
            stage = FanCompatStage::Restoring;
            record.verdict = FanCompatVerdict::AbortedUser;
            record.reason = L"Stopped by user.";
        }
    }

    // Step 5: Complete safe restore/handover after candidate trial.
    void FinishRestore(bool restoreOk, const FanCompatTelemetry& postTel) {
        record.restoredSafeState = restoreOk;
        record.postRestoreRpm = MaxRpmOf(postTel.rpms);
        if (!restoreOk && record.verdict == FanCompatVerdict::Compatible) {
            record.verdict = FanCompatVerdict::AbortedSafety;
            record.reason = L"Safe state restore failed after trial.";
        }
        stage = (record.verdict == FanCompatVerdict::AbortedSafety || record.verdict == FanCompatVerdict::AbortedUser)
                    ? FanCompatStage::Aborted
                    : FanCompatStage::Completed;
    }
};

// Every bundled model config (file stem), for the manual picker.
inline std::vector<std::wstring> FanListConfigs() {
    std::vector<std::wstring> out;
    for (const auto& f : ListDir(FanConfigsDir(), false, L"*.xml")) {
        std::wstring n = f;
        if (n.size() > 4) n.resize(n.size() - 4);   // strip ".xml"
        out.push_back(n);
    }
    return out;
}

// Global mutex serializing all nbfc.exe CLI invocations so UI status polls and background
// keep-alive speed commands never collide on NBFC's single-client WCF named pipe.
inline std::mutex& NbfcCliMutex() {
    static std::mutex m;
    return m;
}

// Run nbfc.exe and collect its stdout lines + exit code (serialized + 1 retry on transient pipe busy).
inline bool NbfcRunRc(const std::wstring& args, std::wstring& out) {
    std::lock_guard<std::mutex> lk(NbfcCliMutex());
    out.clear();
    if (!FileExists(NbfcExe())) { out = L"nbfc.exe was not found."; return false; }
    for (int attempt = 0; attempt < 2; ++attempt) {
        out.clear();
        const DWORD rc = RunCapture(NbfcExe(), args, [&](const std::wstring& l) { out += l + L"\n"; }, 15000);
        if (rc == 0) return true;
        if (attempt == 0) Sleep(120);
    }
    return false;
}
inline std::wstring NbfcRun(const std::wstring& args) {
    std::wstring out;
    NbfcRunRc(args, out);
    return out;
}

// The NBFC service host must run (as admin) AND be in 'start --enabled' state for EC access.
// 2.8.5: NbfcService.exe is a Windows service host. Running the exe itself (the 2.5.2 way) only shows the "Windows Service Start Failure"
// dialog ("Cannot start service from the command line"), so the service is started through the service manager, and only when it is registered.
enum class NbfcSvcAction { None, StartService };
inline NbfcSvcAction PlanNbfcServiceStart(bool processRunning, bool serviceRegistered) {
    return (!processRunning && serviceRegistered) ? NbfcSvcAction::StartService : NbfcSvcAction::None;
}
inline bool NbfcServiceRegistered(bool start) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"NbfcService", start ? (SERVICE_START | SERVICE_QUERY_STATUS) : SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc && start) StartServiceW(svc, 0, nullptr);   // already running -> harmless error
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}
inline void EnsureFanService() {
    if (kTestBuild) { TestRefused(L"start NBFC service"); return; }
    if (PlanNbfcServiceStart(ProcessRunning(L"NbfcService.exe"), NbfcServiceRegistered(false)) == NbfcSvcAction::StartService) {
        NbfcServiceRegistered(true);
        Sleep(700);   // give the service time to bind its IPC pipe before nbfc.exe connects
    }
    const std::wstring st = NbfcRun(L"status --service");
    if (st.find(L"False") != std::wstring::npos || st.find(L"false") != std::wstring::npos) {
        std::wstring startOut;
        NbfcRunRc(L"start --enabled", startOut);
    }
}

// Models NBFC thinks match this machine (best-effort; may be empty).
inline std::vector<std::wstring> FanRecommend() {
    std::vector<std::wstring> out;
    const std::wstring s = NbfcRun(L"config --recommend");
    std::wstring line;
    for (wchar_t c : s) {
        if (c == L'\n') { if (!line.empty()) out.push_back(line); line.clear(); }
        else line.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- Capability classification & NBFC validation
enum class FanBackendKind { MonitoringOnly, DirectManual, ConfigurationBased };

struct FanControlCapability {
    FanBackendKind kind = FanBackendKind::MonitoringOnly;
    int totalFans = 0;
    int controllableFans = 0;
    bool nbfcAvailable = false;
    bool perFanSupported = false;
    std::wstring activeConfig;
    int configFanCount = 0;
    std::wstring statusLabel;   // "Monitoring Only" | "Manual Control" | "Configuration Based"
    std::wstring detail;
};

inline FanControlCapability AssessFanControlCapability(int totalFans,
                                                       int controllableFans,
                                                       bool nbfcAvailable,
                                                       const std::wstring& activeConfig,
                                                       int configFanCount) {
    FanControlCapability cap;
    cap.totalFans = totalFans;
    cap.controllableFans = controllableFans;
    cap.nbfcAvailable = nbfcAvailable;
    cap.activeConfig = activeConfig;
    cap.configFanCount = configFanCount;
    if (controllableFans > 0) {
        cap.kind = FanBackendKind::DirectManual;
        cap.statusLabel = L"Manual Control";
        cap.perFanSupported = (controllableFans > 1);
        cap.detail = L"Direct hardware fan control exposed via SensorHub (" +
                     std::to_wstring(controllableFans) + L" controllable channel(s)).";
    } else if (nbfcAvailable) {
        cap.kind = FanBackendKind::ConfigurationBased;
        cap.statusLabel = L"Configuration Based";
        cap.perFanSupported = (!activeConfig.empty() && configFanCount > 1);
        cap.detail = activeConfig.empty()
            ? L"No direct hardware fan control exposed. Configuration-based control (NBFC) is available; select and confirm a configuration below."
            : L"Configuration-based control active via NBFC (" + activeConfig + L").";
    } else {
        cap.kind = FanBackendKind::MonitoringOnly;
        cap.statusLabel = L"Monitoring Only";
        cap.perFanSupported = false;
        cap.detail = L"This device exposes no supported fan control backend. BIOS/firmware remains in control.";
    }
    return cap;
}

inline bool ValidateNbfcConfigName(const std::wstring& name,
                                   const std::vector<FanCompatCandidate>& cands,
                                   std::wstring& err) {
    if (name.empty()) { err = L"Configuration name is empty."; return false; }
    if (name.find(L"..") != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name.find(L'\\') != std::wstring::npos || name.find(L'"') != std::wstring::npos) {
        err = L"Configuration name contains invalid path characters.";
        return false;
    }
    for (const auto& c : cands) {
        if (_wcsicmp(c.name.c_str(), name.c_str()) == 0) return true;
    }
    err = L"Configuration '" + name + L"' was not found among validated local NBFC XML files.";
    return false;
}

struct NbfcStatusInfo {
    std::wstring selectedConfig;
    bool readOnly = false;
    bool autoControlEnabled = false;
    std::vector<double> targetSpeeds;
    std::vector<double> currentSpeeds;
    std::vector<std::wstring> fanNames;
};

inline NbfcStatusInfo ParseNbfcStatus(const std::wstring& s) {
    NbfcStatusInfo info;
    std::wstring curName;
    for (const auto& raw : Split(s, L'\n')) {
        std::wstring line = raw;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        const size_t colon = line.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring k = line.substr(0, colon);
        std::wstring v = line.substr(colon + 1);
        size_t a = v.find_first_not_of(L" \t");
        v = (a == std::wstring::npos) ? L"" : v.substr(a);
        if (k.find(L"Selected config") != std::wstring::npos || k.find(L"Selected Config") != std::wstring::npos) info.selectedConfig = v;
        else if (k.find(L"Read-only") != std::wstring::npos) info.readOnly = (_wcsicmp(v.c_str(), L"true") == 0);
        else if (k.find(L"Auto control enabled") != std::wstring::npos) info.autoControlEnabled = (_wcsicmp(v.c_str(), L"true") == 0);
        else if (k.find(L"Fan display name") != std::wstring::npos || k.find(L"Fan Display Name") != std::wstring::npos) { curName = v; info.fanNames.push_back(v); }
        else if (k.find(L"Current fan speed") != std::wstring::npos || k.find(L"Current Fan Speed") != std::wstring::npos) info.currentSpeeds.push_back(_wtof(v.c_str()));
        else if (k.find(L"Target fan speed") != std::wstring::npos || k.find(L"Target Fan Speed") != std::wstring::npos) info.targetSpeeds.push_back(_wtof(v.c_str()));
    }
    return info;
}

struct FanTelemetryView {
    std::wstring actualRpmLabel;     // "<N> rpm", "N/A", "N/A (unverified)", or "Not Supported"
    std::wstring targetLabel;        // "Target X%" or empty
    std::wstring appliedLabel;       // "Applied X%" (EC readback) or empty
    std::wstring controlStateLabel;  // "BIOS Auto" | "NBFC Auto" | "Manual"
    bool physicalVerified = false;
    bool biosAutoOverride = false;
};

inline FanTelemetryView FormatFanTelemetryView(bool readingsPresent,
                                               double rpm,
                                               bool rpmLive,
                                               bool controlOnly,
                                               double targetPct,
                                               double appliedPct,
                                               bool biosAuto) {
    FanTelemetryView v;
    const bool finiteRpm = std::isfinite(rpm);
    if (!readingsPresent || !finiteRpm || rpm < 0.0) {
        v.actualRpmLabel = controlOnly ? L"Not Supported" : L"N/A";
        v.physicalVerified = false;
    } else if (!rpmLive) {
        v.actualRpmLabel = L"N/A (unverified)";
        v.physicalVerified = false;
    } else {
        v.actualRpmLabel = std::to_wstring(static_cast<int>(rpm + 0.5)) + L" rpm";
        v.physicalVerified = true;
    }
    if (std::isfinite(targetPct) && targetPct >= 0.0)
        v.targetLabel = L"Target " + std::to_wstring(static_cast<int>(targetPct + 0.5)) + L"%";
    if (std::isfinite(appliedPct) && appliedPct >= 0.0)
        v.appliedLabel = L"Applied " + std::to_wstring(static_cast<int>(appliedPct + 0.5)) + L"%";
    v.biosAutoOverride = biosAuto;
    v.controlStateLabel = biosAuto ? L"BIOS/Auto" : L"Manual";
    return v;
}

struct FanOpResult {
    bool ok = false;
    std::wstring message;
    std::wstring output;
};

using NbfcCmdRunner = std::function<bool(const std::wstring& args, std::wstring& out)>;

enum class FanOperatingMode { Automatic = 0, Manual = 1, AutoFloor = 2 };

inline int ClampFanFloor(int pct, int hwMin = kMinFanDutyPct, int hwMax = 100) {
    const int lo = std::max(kMinFanDutyPct, hwMin);
    const int hi = std::max(lo, std::min(100, hwMax));
    return std::clamp(pct, lo, hi);
}

// Computes the target fan duty (%) for Automatic + Minimum Floor mode.
// Thermal override (>= 92 C or explicit override) always takes priority (100%).
inline int ComputeAutoFloorTargetPct(int floorPct, double maxTempC, double baseAutoPct, bool thermalOverrideActive) {
    const int safeFloor = ClampFanFloor(floorPct);
    if (thermalOverrideActive || maxTempC >= 92.0) return 100;
    int thermalDemand = 0;
    if (baseAutoPct > 0.0) thermalDemand = static_cast<int>(baseAutoPct + 0.5);
    else if (maxTempC >= 85.0) thermalDemand = 100;
    else if (maxTempC >= 55.0) thermalDemand = 40 + static_cast<int>(((maxTempC - 55.0) / 30.0) * 55.0 + 0.5);
    return std::clamp(std::max(safeFloor, thermalDemand), kMinFanDutyPct, 100);
}

inline FanOpResult FanSetAutoSafe(const NbfcCmdRunner& runner = nullptr) {
    FanOpResult res;
    if (!runner && kTestBuild) {
        TestRefused(L"NBFC set --auto & stop");
        res.message = L"Blocked in test build.";
        return res;
    }
    if (!runner) {
        EnsureFanService();
        std::wstring f0, f1;
        NbfcRunRc(L"set --auto --fan 0", f0);
        NbfcRunRc(L"set --auto --fan 1", f1);
    }
    std::wstring autoOut, stopOut;
    const bool okAuto = runner ? runner(L"set --auto", autoOut) : NbfcRunRc(L"set --auto", autoOut);
    const bool okStop = runner ? runner(L"stop", stopOut) : NbfcRunRc(L"stop", stopOut);
    res.output = autoOut + (stopOut.empty() ? L"" : L"\n" + stopOut);
    res.ok = okAuto && okStop;
    res.message = res.ok ? L"Fans returned to automatic (BIOS) and NBFC engine stopped."
                         : (res.output.empty() ? L"Failed to return fans to automatic / stop NBFC." : res.output);
    if (!runner) { if (res.ok) LogI(L"Fan: returned to automatic (BIOS) and stopped NBFC engine."); else LogE(res.message); }
    return res;
}

inline FanOpResult FanApplySafe(const std::wstring& name,
                                const std::vector<FanCompatCandidate>& cands,
                                const NbfcCmdRunner& runner = nullptr) {
    FanOpResult res;
    if (!ValidateNbfcConfigName(name, cands, res.message)) return res;
    if (!runner && kTestBuild) {
        TestRefused(L"NBFC config --apply " + name);
        res.message = L"Blocked in test build.";
        return res;
    }
    if (!runner) EnsureFanService();
    const std::wstring args = L"config --apply \"" + name + L"\"";
    const bool ok = runner ? runner(args, res.output) : NbfcRunRc(args, res.output);
    if (!ok) {
        std::wstring autoOut;
        if (runner) runner(L"set --auto", autoOut);
        else NbfcRunRc(L"set --auto", autoOut);
        res.ok = false;
        res.message = L"Failed to apply NBFC configuration '" + name + L"'; reverted to automatic.";
        if (!runner) LogE(res.message);
        return res;
    }
    res.ok = true;
    res.message = L"Applied NBFC configuration '" + name + L"'.";
    if (!runner) LogI(L"Fan: applied NBFC config '" + name + L"'.");
    return res;
}

inline FanOpResult FanSetSpeedSafe(int pct, int fanIndex = -1, const NbfcCmdRunner& runner = nullptr, bool quiet = false) {
    FanOpResult res;
    const int clamped = ClampFanDuty40(pct);   // D23: mandatory 40% minimum
    if (!runner && kTestBuild) {
        TestRefused(L"NBFC set --speed " + std::to_wstring(clamped));
        res.message = L"Blocked in test build.";
        return res;
    }
    if (!runner) EnsureFanService();
    std::wstring args = L"set --speed " + std::to_wstring(clamped);
    if (fanIndex >= 0) args += L" --fan " + std::to_wstring(fanIndex);
    const bool ok = runner ? runner(args, res.output) : NbfcRunRc(args, res.output);
    if (!ok) {
        std::wstring autoOut;
        if (runner) runner(L"set --auto", autoOut);
        else NbfcRunRc(L"set --auto", autoOut);
        res.ok = false;
        res.message = L"Fan speed command failed; reverted to automatic.";
        if (!runner) LogE(res.message);
        return res;
    }
    res.ok = true;
    res.message = L"Set fan speed to " + std::to_wstring(clamped) + L"%" +
                  (fanIndex >= 0 ? L" (Fan " + std::to_wstring(fanIndex + 1) + L")" : L"") + L".";
    if (!runner && !quiet) LogI(L"Fan: " + res.message);
    return res;
}

// Legacy wrappers preserved for compatibility (enforcing ClampFanDuty40 & kTestBuild guards).
inline bool FanApply(const std::wstring& name) {
    const auto cands = DiscoverFanCompatCandidates(L"");
    return FanApplySafe(name, cands).ok;
}
inline void FanSetSpeed(int pct) { FanSetSpeedSafe(pct); }
inline void FanSetAuto() { FanSetAutoSafe(); }

// Open the full NBFC window (manual model selection, live readings, its own
// safety controls). AnkhCore runs elevated, so the child has EC access.
inline void OpenFanGui() {
    if (kTestBuild) { TestRefused(L"open NoteBookFanControl.exe"); return; }
    if (FileExists(FanGuiExe())) Run(FanGuiExe(), L"", false, 0);
}

// Currently-selected config name from `nbfc status`, or empty.
inline std::wstring FanCurrentConfig() {
    return ParseNbfcStatus(NbfcRun(L"status")).selectedConfig;
}

// ---------------------------------------------------------------- Brand grouping & search for NBFC config picker
struct FanConfigBrandInfo {
    std::wstring brand;
    int count = 0;
    bool hasExact = false;
    bool hasFamily = false;
};

inline std::wstring FanTrim(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

inline std::wstring ExtractFanConfigBrand(const FanCompatCandidate& cd) {
    const std::wstring src = FanTrim(cd.name.empty() ? cd.notebookModel : cd.name);
    if (src.empty()) return L"Other";
    const size_t sp = src.find_first_of(L" _-");
    std::wstring tok = (sp == std::wstring::npos) ? src : src.substr(0, sp);
    const std::wstring low = Lower(tok);
    if (low == L"hp" || low == L"compaq") return L"HP";
    if (low == L"asus") return L"ASUS";
    if (low == L"msi") return L"MSI";
    if (low == L"lg") return L"LG";
    if (low == L"lenovo" || low == L"thinkpad" || low == L"ideapad") return L"Lenovo";
    if (low == L"dell" || low == L"alienware" || low == L"latitude" || low == L"xps" || low == L"inspiron") return L"Dell";
    if (low == L"acer" || low == L"predator" || low == L"aspire") return L"Acer";
    if (low == L"gigabyte" || low == L"aorus") return L"Gigabyte";
    if (low == L"samsung") return L"Samsung";
    if (low == L"toshiba") return L"Toshiba";
    if (low == L"razer") return L"Razer";
    if (low == L"fujitsu") return L"Fujitsu";
    if (low == L"huawei") return L"Huawei";
    if (low == L"xiaomi") return L"Xiaomi";
    if (low == L"clevo") return L"Clevo";
    if (low == L"medion") return L"Medion";
    if (low == L"sony" || low == L"vaio") return L"Sony";
    if (tok.size() >= 2 && tok[0] >= L'a' && tok[0] <= L'z') tok[0] = static_cast<wchar_t>(tok[0] - L'a' + L'A');
    return tok;
}

inline std::vector<FanConfigBrandInfo> DiscoverFanConfigBrands(const std::vector<FanCompatCandidate>& cands) {
    std::map<std::wstring, FanConfigBrandInfo> byName;
    for (const auto& cd : cands) {
        const std::wstring b = ExtractFanConfigBrand(cd);
        auto& info = byName[b];
        info.brand = b;
        ++info.count;
        info.hasExact |= cd.exactModelMatch;
        info.hasFamily |= cd.familyMatch;
    }
    std::vector<FanConfigBrandInfo> out;
    out.reserve(byName.size());
    for (const auto& kv : byName) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const FanConfigBrandInfo& a, const FanConfigBrandInfo& b) {
        const int pa = a.hasExact ? 2 : a.hasFamily ? 1 : 0;
        const int pb = b.hasExact ? 2 : b.hasFamily ? 1 : 0;
        if (pa != pb) return pa > pb;
        if (a.count != b.count) return a.count > b.count;
        return _wcsicmp(a.brand.c_str(), b.brand.c_str()) < 0;
    });
    return out;
}

inline std::vector<int> FilterFanConfigIndices(const std::vector<FanCompatCandidate>& cands,
                                               const std::wstring& brand,
                                               const std::wstring& searchQuery,
                                               bool matchOnly) {
    std::vector<int> out;
    const std::wstring q = Lower(FanTrim(searchQuery));
    for (int i = 0; i < static_cast<int>(cands.size()); ++i) {
        const auto& cd = cands[i];
        if (matchOnly && !cd.exactModelMatch && !cd.familyMatch) continue;
        if (!brand.empty() && _wcsicmp(ExtractFanConfigBrand(cd).c_str(), brand.c_str()) != 0) continue;
        if (!q.empty()) {
            const bool hit = Lower(cd.name).find(q) != std::wstring::npos ||
                             Lower(cd.notebookModel).find(q) != std::wstring::npos ||
                             Lower(cd.author).find(q) != std::wstring::npos;
            if (!hit) continue;
        }
        out.push_back(i);
    }
    return out;
}

inline bool ShouldReassertFanKeepAlive(ULONGLONG nowMs,
                                       ULONGLONG lastSendMs,
                                       int targetPct,
                                       int lastSentPct,
                                       bool overrideActive,
                                       ULONGLONG keepAliveIntervalMs) {
    if (overrideActive || targetPct != lastSentPct) return true;
    return (nowMs >= lastSendMs) && (nowMs - lastSendMs >= keepAliveIntervalMs);
}

// ---------------------------------------------------------------- Independent CPU/GPU Fan Channel Calibration
enum class FanChannelRole { Unknown = 0, Cpu = 1, Gpu = 2 };

struct FanChannelCalibration {
    int cpuOffsetPct = 0;   // independent CPU fan offset (-20 .. +20), default 0
    int gpuOffsetPct = 0;   // independent GPU fan offset (-20 .. +20), default 0 (e.g. +10 for ZBook dual-fan balance)
    bool IsActive() const { return cpuOffsetPct != 0 || gpuOffsetPct != 0; }
};

inline FanChannelRole ClassifyFanChannelRole(int fanIndex, int totalFans, const std::wstring& fanName) {
    const std::wstring low = Lower(fanName);
    if (low.find(L"gpu") != std::wstring::npos) return FanChannelRole::Gpu;
    if (low.find(L"cpu") != std::wstring::npos && totalFans == 1) return FanChannelRole::Cpu;
    // On dual-fan laptops (such as HP ZBook 15 G3/G5/G7 where Fan 0 is CPU/Left and Fan 1 is GPU/Right):
    if (totalFans >= 2) {
        if (fanIndex == 0) return FanChannelRole::Cpu;
        if (fanIndex == 1) return FanChannelRole::Gpu;
    }
    if (low.find(L"cpu") != std::wstring::npos) return FanChannelRole::Cpu;
    return FanChannelRole::Unknown;
}

inline int ComputeCalibratedFanDuty(int logicalRequestedPct,
                                    FanChannelRole role,
                                    const FanChannelCalibration& cal,
                                    bool thermalOverrideActive = false,
                                    int hwMin = kMinFanDutyPct,
                                    int hwMax = 100) {
    if (thermalOverrideActive) return 100;
    const int base = ClampFanFloor(logicalRequestedPct, hwMin, hwMax);
    int offset = 0;
    if (role == FanChannelRole::Cpu) offset = std::clamp(cal.cpuOffsetPct, -20, 20);
    else if (role == FanChannelRole::Gpu) offset = std::clamp(cal.gpuOffsetPct, -20, 20);
    return ClampFanFloor(base + offset, hwMin, hwMax);
}

inline FanOpResult FanSetSpeedCalibratedSafe(int logicalPct,
                                             int fanCount,
                                             const std::vector<std::wstring>& fanNames,
                                             const FanChannelCalibration& cal,
                                             const NbfcCmdRunner& runner = nullptr,
                                             bool quiet = false) {
    // Always command each fan index explicitly when fanCount >= 2 because nbfc.exe 'set --speed'
    // without '--fan <idx>' defaults to '--fan 0' only and leaves Fan 1 at its old speed.
    if (fanCount <= 1 && !cal.IsActive()) {
        return FanSetSpeedSafe(logicalPct, -1, runner, quiet);
    }
    const int effectiveCount = std::max(1, fanCount);
    FanOpResult combined;
    combined.ok = true;
    std::wstring summary;
    for (int i = 0; i < effectiveCount; ++i) {
        const std::wstring nm = (i < static_cast<int>(fanNames.size())) ? fanNames[i] : L"";
        const FanChannelRole role = ClassifyFanChannelRole(i, effectiveCount, nm);
        const int applied = ComputeCalibratedFanDuty(logicalPct, role, cal, false);
        const FanOpResult r = FanSetSpeedSafe(applied, i, runner, true);
        if (!combined.output.empty() && !r.output.empty()) combined.output += L"\n";
        combined.output += r.output;
        if (!r.ok) {
            combined.ok = false;
            combined.message = r.message;
            return combined;
        }
        if (i > 0) summary += L", ";
        summary += (role == FanChannelRole::Cpu ? L"CPU Fan " : role == FanChannelRole::Gpu ? L"GPU Fan " : L"Fan ") +
                   std::to_wstring(i + 1) + L": " + std::to_wstring(applied) + L"% (req " + std::to_wstring(ClampFanDuty40(logicalPct)) + L"%)";
    }
    combined.message = L"Set calibrated fan speed (" + summary + L").";
    if (!runner && !quiet) LogI(L"Fan: " + combined.message);
    return combined;
}

}  // namespace ak
