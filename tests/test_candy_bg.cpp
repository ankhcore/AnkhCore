// Candy presentation background (src/candy_bg.h): matte reproduces the original, blur/opacity independence, caching. Pure logic, no window.
#include "candy_bg.h"
#include <cstdio>
using namespace ak;
static int g_fail = 0;
#define CHECK(c, msg) do { const bool ok_ = (c); std::printf("%s  %s\n", ok_ ? "PASS" : "FAIL", msg); if (!ok_) ++g_fail; } while (0)
static uint32_t PX(int r, int g, int b) { return 0xFF000000u | (r << 16) | (g << 8) | b; }
int main() {
    const int W = 64, H = 36;
    std::vector<uint32_t> candy(W * H, PX(21, 20, 30));
    for (int y = 10; y < 26; ++y) for (int x = 10; x < 54; ++x) candy[y * W + x] = PX(250, 80 + x, 200 - y);   // "icon" area
    candy[0] = PX(60, 40, 90);                                                                                  // a glow pixel
    const uint32_t base = CandyBaseColor(candy.data(), candy.size());
    CHECK(base == PX(21, 20, 30), "base colour is the dominant ground");
    auto m = CandyMatte(candy.data(), candy.size(), base);
    // composite matte over base == original
    int maxErr = 0;
    for (size_t i = 0; i < candy.size(); ++i) {
        const int a = m[i] >> 24, k = 255 - a;
        const int ch[3] = {int(m[i] & 255) + (base & 255) * k / 255, int((m[i] >> 8) & 255) + ((base >> 8) & 255) * k / 255, int((m[i] >> 16) & 255) + ((base >> 16) & 255) * k / 255};
        const int o[3] = {int(candy[i] & 255), int((candy[i] >> 8) & 255), int((candy[i] >> 16) & 255)};
        for (int c = 0; c < 3; ++c) maxErr = std::max(maxErr, std::abs(ch[c] - o[c]));
    }
    CHECK(maxErr <= 2, "matte over its base colour reproduces the original pixels (max error <= 2)");
    CHECK((m[5 * W + 5] >> 24) == 0, "pure ground is fully transparent in the matte (background shows through)");
    CHECK((m[15 * W + 30] >> 24) > 200, "icon pixels are opaque in the matte (opacity of the background cannot touch them)");
    // blur
    std::vector<uint32_t> img(W * H, PX(0, 0, 0)); for (int y = 14; y < 23; ++y) for (int x = 28; x < 37; ++x) img[y * W + x] = PX(255, 255, 255);
    auto b0 = img; CandyBlur(b0, W, H, 0); CHECK(b0 == img, "blur 0 leaves the picture unchanged");
    auto b8 = img; CandyBlur(b8, W, H, 8);
    CHECK(((b8[14 * W + 28] & 255) < 255) && ((b8[18 * W + 40] & 255) > 0), "blur spreads a point");
    CHECK((b8[0] >> 24) == 255 && (b8[17 * W + 31] >> 24) == 255, "blur keeps the picture opaque");
    auto b30 = img; CandyBlur(b30, W, H, 30); CHECK((b30[18 * W + 32] & 255) < (b8[18 * W + 32] & 255), "stronger blur flattens more");
    std::vector<uint32_t> flat(W * H, PX(40, 50, 60)); CandyBlur(flat, W, H, 30);
    CHECK(flat[0] == PX(40, 50, 60) && flat[W * H - 1] == PX(40, 50, 60), "blur of a flat picture is unchanged at the edges (no dark border)");
    // cache
    int calls = 0;
    CandyBgCache cache([&](int w, int h) { ++calls; return std::vector<uint32_t>(static_cast<size_t>(w) * h, PX(1, 2, 3)); }, 3);
    bool fresh = false;
    const CandyLayer* a = cache.Background(W, H, 0, &fresh); CHECK(a && fresh && calls == 1, "first request scales once");
    const CandyLayer* a2 = cache.Background(W, H, 0, &fresh); CHECK(a2 == a && !fresh && calls == 1, "same request is a cache hit (same address)");
    const CandyLayer* bb = cache.Background(W, H, 10, &fresh); CHECK(bb && bb != a && fresh && calls == 1 && cache.Blurs() == 1, "a blur variant reuses the scaled picture (no second scale)");
    cache.Background(W, H, 10, &fresh); CHECK(!fresh && cache.Blurs() == 1, "blur variant is cached");
    cache.Background(W, H, 99, &fresh); CHECK(cache.Background(W, H, 30, &fresh) != nullptr && !fresh, "blur is clamped to 30");
    cache.Background(W, H, 5); cache.Background(W, H, 7);
    CHECK(!cache.DrainEvicted().empty(), "old variants are evicted past the cap and reported");
    cache.Background(W * 2, H * 2, 0); CHECK(calls == 2, "a new size scales again");
    int m0 = cache.Mattes(); cache.Matte(7, candy.data(), W, H); cache.Matte(7, candy.data(), W, H); CHECK(cache.Mattes() == m0 + 1, "matte built once per picture");
    CHECK(CandyClampOpacity(-4) == 0 && CandyClampOpacity(180) == 100 && CandyClampBlur(-1) == 0 && CandyClampBlur(99) == 30, "settings clamp to 0..100 and 0..30");
    std::printf(g_fail ? "FAILED %d\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
