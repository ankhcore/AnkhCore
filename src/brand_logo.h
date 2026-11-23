// AnkhCore 2.5 — display treatment of the brand logo (owner brief 2026-11-14, Phase 0). Pure logic on
// premultiplied BGRA pixels; unit-tested in tests/test_base.cpp.
//
// The supplied logo.png is an ankh in a ring, surrounded by blue circuitry, on a transparent background.
// In the dark interface the outer circuitry should stay present but quiet, while the central ankh stays
// sharp and fully luminous. The treatment only lowers the opacity of pixels by their distance from the
// centre: inside kBrandInner nothing changes; from there it eases down to kBrandFloor at kBrandOuter and
// beyond. Colours, shapes and proportions are untouched (premultiplied pixels are scaled as a whole), so
// it is the same artwork with its outer glow turned down, not a redrawn or cropped logo.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace ak {

// Measured on logo.png (1254 x 1254): the ankh spans x 0.33..0.67 and y 0.23..0.80 of the image, so every
// part of it lies within 0.31 of the centre. The ring around it ends near 0.36.
constexpr float kBrandInner = 0.33f;   // up to here: full opacity (the ankh and its ring)
constexpr float kBrandOuter = 0.55f;   // from here: kBrandFloor
// FT-E01 (owner, Preview 2.1 field test): the Main Window background was too prominent. It is drawn at 38 % over the tint; the
// owner asked for "approximately 70 %". 38 % was never 100 %, so the request is applied as 70 % of the strength it had
// (0.38 x 0.70 = 0.266), which makes the picture softer, as the owner wants. One constant, used by both placements
// (landscape full-window and portrait fitted), drawn only on the background layer: tint, glass cards and text are unchanged.
constexpr float kBrandBgWas = 0.38f;                    // Preview 2.1
constexpr float kBrandBgOpacity = kBrandBgWas * 0.70f;  // now
constexpr float kBrandFloor = 0.30f;  // opacity kept by the outer circuitry (preview 2026-11-14: 0.40 was barely visible as a change)

// Opacity factor at a point given in image fractions (0..1, centre 0.5).
inline float BrandLogoFactor(float fx, float fy) {
    const float r = std::sqrt((fx - .5f) * (fx - .5f) + (fy - .5f) * (fy - .5f));
    if (r <= kBrandInner) return 1.f;
    if (r >= kBrandOuter) return kBrandFloor;
    const float t = (r - kBrandInner) / (kBrandOuter - kBrandInner);
    const float s = t * t * (3.f - 2.f * t);   // smoothstep: no visible ring where the fade starts or ends
    return 1.f + (kBrandFloor - 1.f) * s;
}

// Applies the treatment to w x h premultiplied BGRA pixels in place.
inline void BrandLogoSoften(uint32_t* p, int w, int h) {
    if (!p || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y) {
        const float fy = (y + .5f) / h;
        for (int x = 0; x < w; ++x) {
            const float k = BrandLogoFactor((x + .5f) / w, fy);
            if (k >= 1.f) continue;
            uint32_t& v = p[static_cast<size_t>(y) * w + x];
            auto ch = [&](int shift) { return static_cast<uint32_t>(std::lround(((v >> shift) & 255u) * k)) << shift; };
            v = ch(24) | ch(16) | ch(8) | ch(0);
        }
    }
}

}   // namespace ak
