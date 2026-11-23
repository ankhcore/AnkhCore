// Candy Icons presentation background (2.5): the owner's brand_background picture behind the Candy preview, with two settings:
//   candy.bgOpacity 0..100 (default 100) and candy.bgBlur 0..30 px (default 0). Both act on the background layer ONLY.
// Layers, bottom to top:  1. the Candy picture's own base colour   2. the background (blurred copy, drawn with the opacity)
//                         3. the Candy artwork as a "matte": the original picture split into premultiplied colour + alpha
//                            against its base colour, so that over the base colour it reproduces the original pixel for pixel.
// Nothing here changes a file: the original candy-icons picture and the background picture are only read.
// Pure logic (no window, no Direct2D): everything expensive happens once per (size, blur) and is cached; painting only draws.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>

namespace ak {

constexpr int kCandyOpacityDefault = 100, kCandyBlurDefault = 0, kCandyBlurMax = 30;
inline int CandyClampOpacity(int v) { return std::clamp(v, 0, 100); }
inline int CandyClampBlur(int v) { return std::clamp(v, 0, kCandyBlurMax); }

// ---- the base colour of a Candy picture: per-channel median (the picture is mostly its dark ground), premultiplied BGRA input
inline uint32_t CandyBaseColor(const uint32_t* px, size_t n) {
    std::array<std::array<uint32_t, 256>, 3> h{}; size_t cnt = 0;
    const size_t step = n > 200000 ? n / 100000 : 1;
    for (size_t i = 0; i < n; i += step) { const uint32_t v = px[i]; ++h[0][v & 255]; ++h[1][(v >> 8) & 255]; ++h[2][(v >> 16) & 255]; ++cnt; }
    uint32_t c[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) { size_t acc = 0; for (int i = 0; i < 256; ++i) { acc += h[k][i]; if (acc * 2 >= cnt) { c[k] = i; break; } } }
    return 0xFF000000u | (c[2] << 16) | (c[1] << 8) | c[0];
}
// ---- the Candy artwork as a matte over `base`: alpha = how far the pixel is from the base, colour = what is left of it.
// Composited (source-over, premultiplied) onto the base colour this gives back the input pixel; onto any other ground it
// shows that ground through the soft edges and the dark parts of the picture.
inline std::vector<uint32_t> CandyMatte(const uint32_t* px, size_t n, uint32_t base) {
    std::vector<uint32_t> out(n);
    const int bb = base & 255, bg = (base >> 8) & 255, br = (base >> 16) & 255;
    for (size_t i = 0; i < n; ++i) {
        const int b = px[i] & 255, g = (px[i] >> 8) & 255, r = (px[i] >> 16) & 255;
        float a = 0;
        a = std::max(a, (b - bb) / std::max(1.f, 255.f - bb)); a = std::max(a, (g - bg) / std::max(1.f, 255.f - bg)); a = std::max(a, (r - br) / std::max(1.f, 255.f - br));
        a = std::clamp(a, 0.f, 1.f);
        const int A = static_cast<int>(a * 255.f + 0.5f);
        const float k = 1.f - A / 255.f;
        const int pb = std::clamp(static_cast<int>(b - bb * k + 0.5f), 0, A), pg = std::clamp(static_cast<int>(g - bg * k + 0.5f), 0, A), pr = std::clamp(static_cast<int>(r - br * k + 0.5f), 0, A);
        out[i] = (static_cast<uint32_t>(A) << 24) | (static_cast<uint32_t>(pr) << 16) | (static_cast<uint32_t>(pg) << 8) | static_cast<uint32_t>(pb);
    }
    return out;
}

// ---- blur: three box passes (a good Gaussian approximation), edge pixels repeated; `px` is premultiplied BGRA and stays opaque
// blurPx is the strength the user sees (0..30). Box radius per pass = ceil(blurPx / 2), so the visible spread is about blurPx.
inline int CandyBoxRadius(int blurPx) { return blurPx <= 0 ? 0 : (blurPx + 1) / 2; }
inline void CandyBlur(std::vector<uint32_t>& px, int w, int h, int blurPx) {
    const int r = CandyBoxRadius(blurPx);
    if (r <= 0 || w <= 0 || h <= 0) return;
    std::vector<uint32_t> tmp(px.size());
    auto pass = [&](const uint32_t* src, uint32_t* dst, int len, int stride, int lines, int lineStride) {
        const int win = 2 * r + 1;
        for (int l = 0; l < lines; ++l) {
            const uint32_t* s = src + static_cast<size_t>(l) * lineStride; uint32_t* d = dst + static_cast<size_t>(l) * lineStride;
            uint32_t sum[4] = {0, 0, 0, 0};
            auto at = [&](int i) { return s[static_cast<size_t>(std::clamp(i, 0, len - 1)) * stride]; };
            for (int i = -r; i <= r; ++i) { const uint32_t v = at(i); sum[0] += v & 255; sum[1] += (v >> 8) & 255; sum[2] += (v >> 16) & 255; sum[3] += v >> 24; }
            for (int i = 0; i < len; ++i) {
                d[static_cast<size_t>(i) * stride] = ((sum[3] / win) << 24) | ((sum[2] / win) << 16) | ((sum[1] / win) << 8) | (sum[0] / win);
                const uint32_t o = at(i - r), n2 = at(i + r + 1);
                sum[0] += (n2 & 255) - (o & 255); sum[1] += ((n2 >> 8) & 255) - ((o >> 8) & 255); sum[2] += ((n2 >> 16) & 255) - ((o >> 16) & 255); sum[3] += (n2 >> 24) - (o >> 24);
            }
        }
    };
    for (int k = 0; k < 3; ++k) { pass(px.data(), tmp.data(), w, 1, h, w); pass(tmp.data(), px.data(), h, w, w, 1); }
}

// ---- the cache: scaled background per (w, h) and its blurred variants per blur value; the Candy matte per picture id.
// Each stored picture has a stable address (heap node) that the painter uses as its Direct2D bitmap key; evicted keys are reported
// through DrainEvicted so the painter can drop the device bitmaps.
struct CandyLayer { std::vector<uint32_t> px; int w = 0, h = 0; };
class CandyBgCache {
public:
    // provider: the brand background scaled (aspect kept, cover, centre-cropped) to w x h as premultiplied opaque BGRA; empty on failure
    using Provider = std::function<std::vector<uint32_t>(int w, int h)>;
    explicit CandyBgCache(Provider p = nullptr, size_t maxVariants = 8) : provider_(std::move(p)), maxVar_(maxVariants) {}
    void SetProvider(Provider p) { provider_ = std::move(p); Clear(); }
    // the background for this size and blur; never null while the provider works. `fresh` is true when this call built it.
    // The sharp scaled picture is kept for the current size (blur 0 is that picture itself); blurred variants are an LRU list.
    const CandyLayer* Background(int w, int h, int blurPx, bool* fresh = nullptr) {
        blurPx = CandyClampBlur(blurPx); if (fresh) *fresh = false;
        if (w <= 0 || h <= 0 || !provider_) return nullptr;
        if (!sharp_ || sharp_->layer.w != w || sharp_->layer.h != h) {
            std::vector<uint32_t> px = provider_(w, h); ++scales_;
            if (px.size() != static_cast<size_t>(w) * h) return nullptr;
            Clear();
            if (sharp_) evicted_.push_back(&sharp_->layer);
            sharp_ = std::make_unique<Node>(); sharp_->layer.px = std::move(px); sharp_->layer.w = w; sharp_->layer.h = h;
            if (fresh) *fresh = true;
        }
        if (blurPx == 0) return &sharp_->layer;
        for (auto it = var_.begin(); it != var_.end(); ++it)
            if (it->blur == blurPx) { var_.splice(var_.begin(), var_, it); return &var_.front().layer; }
        var_.emplace_front(); Variant& v = var_.front(); v.blur = blurPx; v.layer = sharp_->layer; CandyBlur(v.layer.px, w, h, blurPx); ++blurs_;
        if (fresh) *fresh = true;
        while (var_.size() > maxVar_) { evicted_.push_back(&var_.back().layer); var_.pop_back(); }
        return &var_.front().layer;
    }
    // the matte of the Candy picture `id` (pixels premultiplied BGRA, w x h); built once per id
    const CandyLayer* Matte(uint64_t id, const uint32_t* px, int w, int h, uint32_t* baseOut = nullptr) {
        if (!px || w <= 0 || h <= 0) return nullptr;
        if (matte_ && matteId_ == id) { if (baseOut) *baseOut = base_; return &matte_->layer; }
        if (matte_) evicted_.push_back(&matte_->layer);
        const size_t n = static_cast<size_t>(w) * h;
        base_ = CandyBaseColor(px, n);
        matte_ = std::make_unique<Node>(); matte_->layer.px = CandyMatte(px, n, base_); matte_->layer.w = w; matte_->layer.h = h; matteId_ = id; ++mattes_;
        if (baseOut) *baseOut = base_;
        return &matte_->layer;
    }
    std::vector<const void*> DrainEvicted() { std::vector<const void*> o; o.swap(evicted_); return o; }
    void Clear() { for (auto& v : var_) evicted_.push_back(&v.layer); var_.clear(); }
    int Scales() const { return scales_; } int Blurs() const { return blurs_; } int Mattes() const { return mattes_; }
private:
    struct Variant { int blur = 0; CandyLayer layer; };
    struct Node { CandyLayer layer; };
    Provider provider_; size_t maxVar_;
    std::unique_ptr<Node> sharp_;
    std::list<Variant> var_;   // most recently used first; list nodes never move, so layer addresses are stable
    std::unique_ptr<Node> matte_; uint64_t matteId_ = 0; uint32_t base_ = 0xFF000000u;
    std::vector<const void*> evicted_; int scales_ = 0, blurs_ = 0, mattes_ = 0;
};

}  // namespace ak
