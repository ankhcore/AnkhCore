// FT-TOAST: notifications as a bounded queue with exact timing and a layout that cannot overlap. Pure: no Windows calls.
// Field test 2026-11-15 (Cursor Manager): toasts piled up at the top right, over the Apply / Hide buttons of the page, cut their text
// to one line, and every toast ever raised left an entry in the animator (a slow leak). Now: at most `maxVisible` toasts show at
// once, the rest wait their turn (bounded), every toast lives a time that depends on its text length and then goes, a toast that
// reports the same operation as one already queued replaces it instead of adding a second, a click dismisses one, and the stack
// has a fixed gap and stays inside the area it is given.
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ak {

struct ToastItem { std::wstring text; unsigned color = 0; uint64_t at = 0, id = 0, key = 0; };

inline int ToastLifetimeMs(const std::wstring& text) {   // short messages go quickly, long ones stay long enough to be read
    const int n = static_cast<int>(text.size());
    return std::min(9000, std::max(3500, 3000 + n * 45));
}

class ToastQueue {
public:
    size_t maxVisible = 3, maxWaiting = 12;
    // key != 0 identifies the operation a toast reports: a second toast for the same key replaces the first, so one operation never
    // shows twice. Returns true when what is on screen changed.
    bool Add(const std::wstring& text, unsigned color, uint64_t key, uint64_t now) {
        if (key) {
            for (auto* q : {&visible_, &waiting_}) for (auto& t : *q) if (t.key == key) {
                if (t.text == text && t.color == color) return false;   // the very same report again: nothing to show
                t.text = text; t.color = color; if (q == &visible_) t.at = now; return q == &visible_;
            }
        }
        ToastItem t{text, color, now, ++seq_, key};
        if (visible_.size() < maxVisible) { visible_.push_back(t); return true; }
        if (waiting_.size() >= maxWaiting) waiting_.pop_front();   // a flood: the oldest waiting one is dropped, never the newest
        waiting_.push_back(t);
        return false;
    }
    // Expires toasts that lived their time and lets waiting ones in. Returns true when what is on screen changed.
    bool Tick(uint64_t now) {
        bool changed = false;
        for (auto it = visible_.begin(); it != visible_.end();) {
            if (now - it->at >= static_cast<uint64_t>(ToastLifetimeMs(it->text))) { it = visible_.erase(it); changed = true; } else ++it;
        }
        while (visible_.size() < maxVisible && !waiting_.empty()) { ToastItem t = waiting_.front(); waiting_.pop_front(); t.at = now; visible_.push_back(t); changed = true; }
        return changed;
    }
    bool Dismiss(uint64_t id, uint64_t now) {
        for (auto it = visible_.begin(); it != visible_.end(); ++it) if (it->id == id) { visible_.erase(it); Tick(now); return true; }
        return false;
    }
    void Clear() { visible_.clear(); waiting_.clear(); }
    bool Empty() const { return visible_.empty() && waiting_.empty(); }
    const std::deque<ToastItem>& Visible() const { return visible_; }
    size_t WaitingCount() const { return waiting_.size(); }
    // Milliseconds until the next toast expires (0 when none is showing): the one timer the window needs.
    uint64_t NextDeadlineMs(uint64_t now) const {
        uint64_t best = 0;
        for (const auto& t : visible_) { const uint64_t end = t.at + static_cast<uint64_t>(ToastLifetimeMs(t.text)); const uint64_t left = end > now ? end - now : 1; if (!best || left < best) best = left; }
        return best;
    }
    bool Animating(uint64_t now, int ms = 260) const { for (const auto& t : visible_) if (now - t.at < static_cast<uint64_t>(ms)) return true; return false; }
private:
    std::deque<ToastItem> visible_, waiting_;
    uint64_t seq_ = 0;
};

// The stack, bottom up: the newest toast sits at the bottom of the area, each one above it with a fixed gap. `heights` are in the
// order of the queue (oldest first). Toasts that do not fit above areaTop are not placed (fitted counts the ones that are, the newest).
struct ToastSlot { int top = 0, height = 0; bool shown = false; };
inline std::vector<ToastSlot> ToastStack(const std::vector<int>& heights, int areaTop, int areaBottom, int gap, size_t& fitted) {
    std::vector<ToastSlot> out(heights.size()); fitted = 0;
    int y = areaBottom;
    for (size_t i = heights.size(); i-- > 0;) {
        const int top = y - heights[i];
        if (top < areaTop) break;
        out[i] = {top, heights[i], true}; ++fitted; y = top - gap;
    }
    return out;
}

// Entry animation without an animator entry per toast: 0..1 over `ms` from the moment the toast appeared.
inline float ToastEntry(uint64_t now, uint64_t at, int ms) {
    if (ms <= 0 || now >= at + static_cast<uint64_t>(ms)) return 1.f;
    const float t = static_cast<float>(now - at) / static_cast<float>(ms), u = 1.f - t;
    return 1.f - u * u * u;
}

}  // namespace ak
