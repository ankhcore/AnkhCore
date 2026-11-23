// AnkhCore 2.5 — frame-time statistics and the animation governor (pure, unit-tested).
// Frame time = CPU time to draw a frame (not the wait for the display). Only frames drawn while something
// animates count, so the 1 s data tick never degrades effects. Degradation order (redesign §6.9):
// 1 = particles off, 2 = glass reveal and backdrop blur off, 3 = treated as reduced motion.
// It recovers one step after a sustained good period. User-chosen Reduced/Off are separate and always win.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace ak {

class FrameStats {
public:
    void Add(float ms) { if (v_.size() < kCap) v_.push_back(ms); else v_[next_] = ms; next_ = (next_ + 1) % kCap; ++total_; }
    size_t Count() const { return v_.size(); }
    uint64_t Total() const { return total_; }
    void Clear() { v_.clear(); next_ = 0; }
    float Avg() const { if (v_.empty()) return 0; float s = 0; for (float x : v_) s += x; return s / v_.size(); }
    float P95() const {
        if (v_.empty()) return 0;
        std::vector<float> t = v_; const size_t k = (t.size() * 95) / 100;
        std::nth_element(t.begin(), t.begin() + std::min(k, t.size() - 1), t.end());
        return t[std::min(k, t.size() - 1)];
    }
private:
    static constexpr size_t kCap = 240;
    std::vector<float> v_; size_t next_ = 0; uint64_t total_ = 0;
};

class AnimationGovernor {
public:
    static constexpr float kBudgetMs = 12.f, kGoodMs = 6.f;
    static constexpr size_t kWindow = 30;
    static constexpr uint64_t kRecoverMs = 8000;
    int level = 0;
    // Feed one animated frame; returns true when the level changed.
    bool Frame(float ms, uint64_t nowMs) {
        w_.Add(ms);
        if (w_.Count() < kWindow) return false;
        const float p95 = w_.P95();
        if (p95 > kBudgetMs && level < 3) { ++level; w_.Clear(); good_ = 0; return true; }
        if (p95 < kGoodMs && level > 0) {
            if (!good_) good_ = nowMs;
            else if (nowMs - good_ >= kRecoverMs) { --level; w_.Clear(); good_ = 0; return true; }
        } else good_ = 0;
        return false;
    }
    bool ParticlesOn() const { return level < 1; }
    bool GlassEffectsOn() const { return level < 2; }
    bool ForcesReduced() const { return level >= 3; }
private:
    FrameStats w_; uint64_t good_ = 0;
};

}  // namespace ak
