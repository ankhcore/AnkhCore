// AnkhCore 2.5 — motion core (pure, unit-tested). Immediate-mode tweens keyed by a UI element id.
// A tween is only alive for its own duration, so Active() goes false by itself when elements stop being drawn.
#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <algorithm>

namespace ak {

enum class Ease { OutCubic, InOutQuart, Spring, Linear };
inline float ApplyEase(Ease e, float t) {
    t = std::clamp(t, 0.f, 1.f);
    if (t >= 1.f) return 1.f;
    switch (e) {
        case Ease::Linear: return t;
        case Ease::InOutQuart: return t < .5f ? 8 * t * t * t * t : 1 - std::pow(-2 * t + 2, 4.f) / 2;
        case Ease::Spring: return 1 - std::exp(-6 * t) * std::cos(11 * t);
        default: return 1 - std::pow(1 - t, 3.f);
    }
}
enum MotionLevel { MotionFull = 0, MotionReduced = 1, MotionOff = 2 };

class Animator {
public:
    int level = MotionFull;
    void Tick(uint64_t nowMs) { now_ = nowMs; }
    // The current value of `id` on its way to `target`. A new id starts at the target (nothing to animate yet).
    float To(uint64_t id, float target, int ms, Ease e = Ease::OutCubic) {
        auto it = m_.find(id);
        if (it == m_.end()) { m_[id] = A{target, target, target, now_, 0, e}; return target; }
        A& a = it->second;
        if (a.to != target) { a.from = Value(a); a.to = target; a.start = now_; a.ms = Dur(ms); a.e = Eased(e); }
        a.cur = Value(a);
        return a.cur;
    }
    void Start(uint64_t id, float from, float to, int ms, Ease e = Ease::OutCubic) { m_[id] = A{from, to, from, now_, Dur(ms), Eased(e)}; }
    bool Active() const { for (const auto& kv : m_) if (now_ < kv.second.start + kv.second.ms) return true; return false; }
    int Duration(int ms) const { return Dur(ms); }
private:
    struct A { float from, to, cur; uint64_t start; int ms; Ease e; };
    int Dur(int ms) const { return level == MotionOff ? 0 : level == MotionReduced ? std::min(ms, 100) : ms; }
    Ease Eased(Ease e) const { return level != MotionFull && e == Ease::Spring ? Ease::OutCubic : e; }   // no overshoot under reduced motion
    float Value(const A& a) const { if (a.ms <= 0 || now_ >= a.start + a.ms) return a.to; return a.from + (a.to - a.from) * ApplyEase(a.e, static_cast<float>(now_ - a.start) / a.ms); }
    std::unordered_map<uint64_t, A> m_;
    uint64_t now_ = 0;
};
inline uint64_t AnimId(int a, int b, int kind = 0) { return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 48) ^ (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 24) ^ static_cast<uint32_t>(b); }

}  // namespace ak
