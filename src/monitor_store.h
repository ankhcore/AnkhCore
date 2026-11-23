// AnkhCore 2.5 — monitoring history and alerts (pure logic; unit-tested in tests/test_base.cpp).
// Fed once per sampling interval from the single sensor snapshot (SensorHub + 1 s timer); it never
// polls hardware itself. Missing or stale readings are stored as gaps, never as invented values.
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace ak {

inline bool HasValue(double v) { return !std::isnan(v); }
constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();
inline double Present(double v) { return v < 0 ? kNoValue : v; }   // the sensor layer uses -1 for "not exposed"

// Fixed-capacity ring buffer: memory never grows past its capacity.
class Ring {
public:
    explicit Ring(size_t cap = 60) { Reset(cap); }
    void Reset(size_t cap) { cap_ = std::max<size_t>(1, cap); buf_.assign(cap_, kNoValue); head_ = 0; size_ = 0; }
    void Push(double v) { buf_[head_] = v; head_ = (head_ + 1) % cap_; if (size_ < cap_) ++size_; }
    size_t Size() const { return size_; }
    size_t Capacity() const { return cap_; }
    double At(size_t i) const { return buf_[(head_ + cap_ - size_ + i) % cap_]; }   // 0 = oldest
    double Last() const { return size_ ? At(size_ - 1) : kNoValue; }
    size_t Valid() const { size_t n = 0; for (size_t i = 0; i < size_; ++i) n += HasValue(At(i)); return n; }
    bool MinMax(double& lo, double& hi) const {
        bool any = false;
        for (size_t i = 0; i < size_; ++i) { const double v = At(i); if (!HasValue(v)) continue; if (!any) lo = hi = v; else { lo = std::min(lo, v); hi = std::max(hi, v); } any = true; }
        return any;
    }
private:
    std::vector<double> buf_; size_t cap_ = 1, head_ = 0, size_ = 0;
};

enum Series { SCpuLoad, SCpuTemp, SGpuLoad, SGpuTemp, SRam, SNetDown, SNetUp, SFan, SDiskIo, SeriesCount };

inline double ConvertTempUnit(double celsius, bool fahrenheit) {
    return fahrenheit ? (celsius * 9.0 / 5.0 + 32.0) : celsius;
}

struct MonitorSample {
    bool fresh = false;      // false: the sensor helper data is stale or absent (stored as gaps)
    double v[SeriesCount];
    MonitorSample() { for (auto& x : v) x = kNoValue; }
};

class MonitorStore {
public:
    static constexpr int kMaxPoints = 3600;   // per series: bounds memory (8 series x 3600 x 8 B ~ 230 KB)
    void Configure(int intervalSec, int historySec) {
        interval_ = std::clamp(intervalSec, 1, 60);
        history_ = std::clamp(historySec, 60, 24 * 3600);
        const size_t cap = static_cast<size_t>(std::clamp(history_ / interval_, 2, kMaxPoints));
        if (cap != series_[0].Capacity() || !configured_) for (auto& s : series_) s.Reset(cap);
        configured_ = true;
    }
    int Interval() const { return interval_; }
    int History() const { return history_; }
    // Called every second with the current snapshot; records one point per interval.
    bool Tick(uint64_t nowSec, const MonitorSample& s) {
        if (!configured_) Configure(1, 300);
        if (lastSec_ != UINT64_MAX && nowSec < lastSec_ + static_cast<uint64_t>(interval_)) return false;
        lastSec_ = nowSec;
        for (int i = 0; i < SeriesCount; ++i) series_[i].Push(s.fresh ? s.v[i] : kNoValue);
        ++points_;
        return true;
    }
    const Ring& Get(Series s) const { return series_[s]; }
    uint64_t Points() const { return points_; }
private:
    Ring series_[SeriesCount];
    int interval_ = 1, history_ = 300;
    bool configured_ = false;
    uint64_t lastSec_ = UINT64_MAX, points_ = 0;
};

// ---------------------------------------------------------------- alerts
// A condition must hold for holdSec before it fires (brief Turbo Boost spikes never alert), clears only
// below threshold - hysteresis, and repeats no sooner than cooldownSec. Unavailable or stale sensors
// produce no alert (an unsupported sensor is not a hardware failure).
struct AlertRule {
    std::string id;           // "cpu.warn", "cpu.crit", "gpu.warn", "gpu.crit", "fan.stop", "ram.high", "disk.low", "cpu.load", "net.high"
    bool enabled = true;
    double threshold = 0;
    int holdSec = 30;
    double hysteresis = 5;
    int cooldownSec = 600;
};
struct AlertEvent { std::string id; double value; uint64_t atSec; };

struct AlertInput {
    bool fresh = false;
    double cpuTemp = kNoValue, gpuTemp = kNoValue;
    bool fanLive = false;     // a fan source that is proven live (value has changed)
    double fanRpm = kNoValue; // highest live fan reading
    double ramPct = kNoValue;
    double diskFreePct = kNoValue;
    double cpuLoad = kNoValue;
    double netMbps = kNoValue;
};

class AlertEngine {
public:
    std::vector<AlertRule> rules;
    AlertEngine() { Defaults(); }
    void Defaults() {
        rules = {{"cpu.warn", true, 90, 30, 5, 600}, {"cpu.crit", true, 97, 15, 5, 300},
                 {"gpu.warn", true, 85, 30, 5, 600}, {"gpu.crit", true, 93, 15, 5, 300},
                 {"fan.stop", true, 85, 60, 5, 900},   // fan.stop: live fan at 0 rpm while CPU >= threshold
                 {"ram.high", true, 90, 30, 5, 600},
                 {"disk.low", true, 10, 60, 2, 1800},
                 {"cpu.load", false, 95, 120, 10, 600},
                 {"net.high", false, 500, 60, 50, 600}};
        state_.assign(rules.size(), State{});
    }
    AlertRule* Find(const std::string& id) { for (auto& r : rules) if (r.id == id) return &r; return nullptr; }
    // Returns the alerts that fire at this second.
    std::vector<AlertEvent> Tick(uint64_t nowSec, const AlertInput& in) {
        std::vector<AlertEvent> out;
        if (state_.size() != rules.size()) state_.assign(rules.size(), State{});
        for (size_t i = 0; i < rules.size(); ++i) {
            const AlertRule& r = rules[i]; State& st = state_[i];
            double value = kNoValue; bool cond = false, clear = true;
            if (!r.enabled) { st.since = UINT64_MAX; continue; }
            if (r.id == "ram.high") {
                value = in.ramPct;
                if (!HasValue(value)) { st.since = UINT64_MAX; continue; }
                cond = value >= r.threshold;
                clear = value < r.threshold - r.hysteresis;
            } else if (r.id == "disk.low") {
                value = in.diskFreePct;
                if (!HasValue(value)) { st.since = UINT64_MAX; continue; }
                cond = value <= r.threshold;
                clear = value > r.threshold + r.hysteresis;
            } else if (r.id == "cpu.load") {
                value = in.cpuLoad;
                if (!HasValue(value)) { st.since = UINT64_MAX; continue; }
                cond = value >= r.threshold;
                clear = value < r.threshold - r.hysteresis;
            } else if (r.id == "net.high") {
                value = in.netMbps;
                if (!HasValue(value)) { st.since = UINT64_MAX; continue; }
                cond = value >= r.threshold;
                clear = value < r.threshold - r.hysteresis;
            } else if (!in.fresh) {
                st.since = UINT64_MAX; continue;   // sensor-helper rules require fresh readings
            } else if (r.id.rfind("cpu.", 0) == 0 || r.id.rfind("gpu.", 0) == 0) {
                value = r.id[0] == 'c' ? in.cpuTemp : in.gpuTemp;
                if (!HasValue(value)) { st.since = UINT64_MAX; continue; }    // sensor not exposed: not a failure
                cond = value >= r.threshold;
                clear = value < r.threshold - r.hysteresis;
            } else if (r.id == "fan.stop") {
                if (!in.fanLive || !HasValue(in.fanRpm) || !HasValue(in.cpuTemp)) { st.since = UINT64_MAX; continue; }
                value = in.fanRpm;
                cond = in.fanRpm < 1 && in.cpuTemp >= r.threshold;
                clear = in.fanRpm >= 1 || in.cpuTemp < r.threshold - r.hysteresis;
            }
            if (cond) {
                if (st.since == UINT64_MAX) st.since = nowSec;
                const bool held = nowSec - st.since >= static_cast<uint64_t>(r.holdSec);
                const bool cooled = st.lastFired == UINT64_MAX || nowSec - st.lastFired >= static_cast<uint64_t>(r.cooldownSec);
                if (held && !st.active && cooled) { st.active = true; st.lastFired = nowSec; out.push_back({r.id, value, nowSec}); }
            } else {
                st.since = UINT64_MAX;
                if (clear) st.active = false;
            }
        }
        return out;
    }
    bool Active(const std::string& id) const { for (size_t i = 0; i < rules.size() && i < state_.size(); ++i) if (rules[i].id == id) return state_[i].active; return false; }
private:
    struct State { uint64_t since = UINT64_MAX; uint64_t lastFired = UINT64_MAX; bool active = false; };
    std::vector<State> state_;
};

}  // namespace ak
