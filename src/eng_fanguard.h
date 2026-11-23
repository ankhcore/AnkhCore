// eng_fanguard.h - M-2.6-CFG1: the safe fan-control abstraction (MASTER_PROMPT 4.3.1, 2.6 stage).
//
// This is an INTERFACE and a state machine, not a fan controller. It is never given a real backend in 2.6: the application does not
// construct it and there is no NBFC or EC binding here. (The existing fan path in app.cpp / engine_process.h is separate and is not touched
// by this file; see ANKHCORE_2.6_CFG1_AUDIT.md for the state of `kFanControlEnabled`.) It exists so that the rules below are written
// once, tested with a mock backend, and are ready for the 2.7 integration:
//   * BIOS Auto is the default and the fallback. Entering it releases AnkhCore's ownership; AnkhCore never emulates firmware behaviour.
//   * AnkhCore Auto / Manual are refused unless ALL of these hold: explicit user consent; a verified device profile for exactly this device
//     (DpVerifiedFor); a verified safe release to firmware; verified limits on the fan; a working backend; fresh SensorHub telemetry.
//     AnkhCore Auto additionally needs a device-specific verified policy, and none exists: it is refused. No universal curve is applied.
//   * Every command is bounded by the profile's limits and by the 40 % minimum duty (D23); a request outside is refused, not clamped.
//   * A watchdog (Tick, fed only by SensorHub readings: no poller of its own) leaves control on: stale or invalid telemetry, over-temperature,
//     backend loss, an actuator error, an RPM response that does not come, or shutdown. On any of them AnkhCore stops issuing commands and
//     releases to firmware once. If the release itself fails, actuation is marked unsupported for the device until a new guard is built.
//   * Every mode change, command, verification, fault and fallback is recorded in a plain log (no secrets).
#pragma once
#include "base.h"
#include "eng_devprofile.h"

namespace ak {

enum class FgMode { BiosAuto, AnkhAuto, Manual };
inline const wchar_t* FgModeLabel(FgMode m) { return m == FgMode::Manual ? L"Manual" : m == FgMode::AnkhAuto ? L"AnkhCore Auto" : L"BIOS Auto"; }

enum class FgFault { None, StaleTelemetry, InvalidReading, Overtemperature, BackendLost, ActuatorError, RpmMismatch, ReleaseFailed };
inline const wchar_t* FgFaultLabel(FgFault f) {
    switch (f) {
    case FgFault::StaleTelemetry: return L"stale telemetry";
    case FgFault::InvalidReading: return L"invalid reading";
    case FgFault::Overtemperature: return L"over-temperature";
    case FgFault::BackendLost: return L"backend lost";
    case FgFault::ActuatorError: return L"actuator error";
    case FgFault::RpmMismatch: return L"no RPM response";
    case FgFault::ReleaseFailed: return L"release to firmware failed";
    default: return L"none";
    }
}

struct FgBackend {   // injected; tests pass a mock, production passes nothing in 2.6
    std::function<bool()> alive;
    std::function<bool(int channel, int percent, std::wstring& err)> setPercent;
    std::function<bool(std::wstring& err)> releaseToFirmware;   // returns control to the firmware's native automatic policy; safe to repeat
};

struct FgTelemetry {   // taken from SensorHub, never sampled here
    bool fresh = false;
    double cpuTempC = -1, gpuTempC = -1;
    std::vector<double> rpmByChannel;   // index = channel; -1 = no reading
};

struct FgConfig {
    double maxSafeTempC = 85.0;                          // the same limit the compatibility trial uses
    int responseTicks = 5;                               // telemetry samples allowed for an RPM response
    double minDeltaRpm = 350.0;                          // the rise that counts as a response (the trial's own threshold)
    int minRaisePct = 10;                                // a command that raises the target by less is not expected to move the fan visibly
    std::function<int(int channel, const FgTelemetry&)> policy;   // the device-specific verified AnkhCore Auto policy: none exists in 2.6
};

struct FgResult { bool ok = false; std::wstring reason; };
struct FgEvent { std::wstring what; bool ok = true; };

class FanControlGuard {
public:
    FanControlGuard(DeviceProfile profile, bool profileVerified, FgBackend backend, FgConfig cfg = {})
        : profile_(std::move(profile)), verified_(profileVerified), be_(std::move(backend)), cfg_(std::move(cfg)) {}

    FgMode Mode() const { return mode_; }
    FgFault Fault() const { return fault_; }
    bool ActuationDisabled() const { return disabled_; }
    const std::vector<FgEvent>& Log() const { return log_; }

    // Entering BIOS Auto needs no gate: it only gives control back. When AnkhCore never owned the fans nothing is sent at all.
    FgResult RequestMode(FgMode m, bool userConsent) {
        if (shutdown_) return Refuse(L"AnkhCore is shutting down.");
        if (m == FgMode::BiosAuto) {
            if (mode_ == FgMode::BiosAuto) return {true, L"Already under BIOS control."};
            const bool ok = Release(L"mode change to BIOS Auto");
            mode_ = FgMode::BiosAuto; pending_.clear();
            Note(L"mode: BIOS Auto", ok);
            return {ok, ok ? L"" : L"Control could not be returned to the firmware; actuation is disabled for this device."};
        }
        if (!userConsent) return Refuse(L"Explicit user consent is required.");
        if (disabled_) return Refuse(L"Control cannot be returned to the firmware safely on this device, so actuation is disabled.");
        if (!verified_) return Refuse(L"There is no verified device profile for this device.");
        if (!profile_.safeReleaseVerified) return Refuse(L"A safe release to the firmware is not verified for this device.");
        if (!HasVerifiedFanWithLimits()) return Refuse(L"No fan has a verified control with known limits.");
        if (!be_.alive || !be_.setPercent || !be_.releaseToFirmware) return Refuse(L"There is no control backend.");
        if (!be_.alive()) return Refuse(L"The control backend is not available.");
        if (!haveTelemetry_ || !Healthy(last_)) return Refuse(L"There is no fresh, valid SensorHub telemetry.");
        if (m == FgMode::AnkhAuto && !cfg_.policy) return Refuse(L"No verified device-specific AnkhCore Auto policy exists for this device; no universal curve is applied.");
        mode_ = m; fault_ = FgFault::None; pending_.clear();
        Note(std::wstring(L"mode: ") + FgModeLabel(m), true);
        return {true, L""};
    }

    // Manual: one fan, one percentage. Refused (never clamped) outside the verified limits or below the 40 % minimum.
    FgResult SetManual(int channel, int percent) {
        if (mode_ != FgMode::Manual) return Refuse(L"Manual mode is not active.");
        const DpFan* f = VerifiedFan(channel);
        if (!f) return Refuse(L"This fan has no verified control.");
        const int lo = std::max(f->minPercent, kMinFanDutyPct), hi = f->maxPercent;
        if (percent < lo || percent > hi) return Refuse(L"The requested speed is outside the verified limits (" + std::to_wstring(lo) + L"-" + std::to_wstring(hi) + L" %).");
        if (!be_.alive()) { Fall(FgFault::BackendLost, L"the backend is not available"); return {false, L"The control backend was lost; control returned to the firmware."}; }
        std::wstring err;
        if (!be_.setPercent(channel, percent, err)) { Fall(FgFault::ActuatorError, L"the actuator reported an error"); return {false, L"The actuator failed; control returned to the firmware."}; }
        Note(L"command: fan " + std::to_wstring(channel) + L" -> " + std::to_wstring(percent) + L" %", true);
        const int prev = lastPct_.count(channel) ? lastPct_[channel] : -1;
        lastPct_[channel] = percent;
        if (percent >= prev + cfg_.minRaisePct || prev < 0) {   // expect the fan to answer
            Pending p; p.channel = channel; p.startRpm = Rpm(last_, channel); p.ticksLeft = cfg_.responseTicks; pending_.push_back(p);
        }
        return {true, L""};
    }

    // One call per SensorHub telemetry sample. The watchdog; it never samples anything.
    void Tick(const FgTelemetry& t) {
        last_ = t; haveTelemetry_ = true;
        if (mode_ == FgMode::BiosAuto || shutdown_) return;
        if (!be_.alive || !be_.alive()) { Fall(FgFault::BackendLost, L"the backend is not available"); return; }
        if (!t.fresh) { Fall(FgFault::StaleTelemetry, L"SensorHub telemetry is stale"); return; }
        if (!ValidTemp(t.cpuTempC)) { Fall(FgFault::InvalidReading, L"the CPU temperature is missing or implausible"); return; }
        if (t.gpuTempC >= 0 && !ValidTemp(t.gpuTempC)) { Fall(FgFault::InvalidReading, L"the GPU temperature is implausible"); return; }
        if (t.rpmByChannel.empty()) { Fall(FgFault::InvalidReading, L"no fan RPM is reported"); return; }
        if (t.cpuTempC >= cfg_.maxSafeTempC || t.gpuTempC >= cfg_.maxSafeTempC) { Fall(FgFault::Overtemperature, L"a temperature reached the safety limit"); return; }
        for (size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            const double now = Rpm(t, p.channel);
            if (now >= 0 && p.startRpm >= 0 && now - p.startRpm >= cfg_.minDeltaRpm) { Note(L"verified: fan " + std::to_wstring(p.channel) + L" responded (" + std::to_wstring(static_cast<int>(now)) + L" rpm)", true); pending_.erase(pending_.begin() + static_cast<long>(i)); continue; }
            if (--p.ticksLeft <= 0) { Fall(FgFault::RpmMismatch, L"fan " + std::to_wstring(p.channel) + L" did not answer the command"); return; }
            ++i;
        }
    }

    // Application exit: hands control back once and refuses everything afterwards.
    void Shutdown() {
        if (shutdown_) return;
        shutdown_ = true;
        if (mode_ != FgMode::BiosAuto) { const bool ok = Release(L"shutdown"); mode_ = FgMode::BiosAuto; pending_.clear(); Note(L"shutdown: control returned to the firmware", ok); }
    }

private:
    struct Pending { int channel = 0; double startRpm = -1; int ticksLeft = 0; };
    static bool ValidTemp(double c) { return std::isfinite(c) && c > 0.0 && c <= 115.0; }
    static double Rpm(const FgTelemetry& t, int ch) { return ch >= 0 && static_cast<size_t>(ch) < t.rpmByChannel.size() ? t.rpmByChannel[static_cast<size_t>(ch)] : -1.0; }
    bool Healthy(const FgTelemetry& t) const { return t.fresh && ValidTemp(t.cpuTempC) && (t.gpuTempC < 0 || ValidTemp(t.gpuTempC)) && !t.rpmByChannel.empty() && t.cpuTempC < cfg_.maxSafeTempC && !(t.gpuTempC >= cfg_.maxSafeTempC); }
    const DpFan* VerifiedFan(int channel) const { for (const DpFan& f : profile_.fans) if (f.channel == channel && f.control == HwControl::Verified && f.minPercent >= 0 && f.maxPercent >= f.minPercent) return &f; return nullptr; }
    bool HasVerifiedFanWithLimits() const { for (const DpFan& f : profile_.fans) if (VerifiedFan(f.channel)) return true; return false; }
    FgResult Refuse(const std::wstring& why) { Note(L"refused: " + why, false); return {false, why}; }
    void Note(const std::wstring& what, bool ok) { log_.push_back({what, ok}); }
    // One release per fault; if it fails the device is marked unsupported for actuation.
    bool Release(const wchar_t* why) {
        std::wstring err;
        const bool ok = be_.releaseToFirmware && be_.releaseToFirmware(err);
        if (!ok) { disabled_ = true; fault_ = FgFault::ReleaseFailed; Note(std::wstring(L"release to firmware FAILED (") + why + L"): actuation disabled for this device", false); }
        else Note(std::wstring(L"released to firmware (") + why + L")", true);
        return ok;
    }
    void Fall(FgFault f, const std::wstring& what) {
        Note(std::wstring(L"fault: ") + FgFaultLabel(f) + L" - " + what, false);
        mode_ = FgMode::BiosAuto; pending_.clear();   // from here no command is issued
        const bool ok = Release(FgFaultLabel(f));
        if (ok) fault_ = f;
    }

    DeviceProfile profile_;
    bool verified_ = false;
    FgBackend be_;
    FgConfig cfg_;
    FgMode mode_ = FgMode::BiosAuto;
    FgFault fault_ = FgFault::None;
    bool disabled_ = false, shutdown_ = false, haveTelemetry_ = false;
    FgTelemetry last_;
    std::vector<Pending> pending_;
    std::map<int, int> lastPct_;
    std::vector<FgEvent> log_;
};

}  // namespace ak
