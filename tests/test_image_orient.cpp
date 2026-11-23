// FT-1 / B01 (and B16): pictures drawn through DrawImage must keep the orientation of their source file.
// Real code under test: ui.h DecodeImage (WIC), DrawImage, gfx.h D2DSurface::DibBitmap (Direct2D path) and the GDI path.
// Test pictures are generated in memory with the WIC encoders; nothing on the desktop, in the registry or in settings is touched.
// Pattern of every picture: green block in the top-left quarter-corner, red upper half, blue lower half.
#include "base.h"
#include "ui.h"
#include "test_pictures.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace ak;
using namespace testpic;

namespace {
int passes = 0, fails = 0;
void Check(bool ok, const std::string& what) {
    if (ok) ++passes; else { ++fails; std::printf("FAIL  %s\n", what.c_str()); }
}

// The picture as it ends up: red must be above blue (always), the green block in the top-left corner (when the picture shows its corner).
void CheckPixels(const std::vector<uint32_t>& p, int w, int h, bool corner, const std::string& tag) {
    // sampled well away from the red/blue border, so the smoothing of a strongly enlarged tiny picture does not decide the result
    const uint32_t up = p[static_cast<size_t>(h / 8) * w + w / 2], dn = p[static_cast<size_t>(h * 7 / 8) * w + w / 2];
    Check(Red(up) && Blue(dn), tag + ": red upper half above blue lower half");
    if (corner) {
        const uint32_t tl = p[static_cast<size_t>(h / 8) * w + w / 8], bl = p[static_cast<size_t>(h * 7 / 8) * w + w / 8];
        Check(Green(tl) && Blue(bl), tag + ": green block top-left, blue bottom-left");
    }
}

// Through the Direct2D path: the real DrawImage -> D2DSurface::DibBitmap -> offscreen target -> read back.
bool ViaD2D(D2DSurface& s, const Image32& im, std::vector<uint32_t>& out) {
    if (!s.BeginBitmap(im.w, im.h)) return false;
    DrawImage(Dc(&s), im, 0, 0);
    out.assign(static_cast<size_t>(im.w) * im.h, 0);
    return s.EndBitmap(out.data(), im.w, im.h);
}
// Through the GDI path (setup, widgets): DrawImage -> GdiAlphaBlend into a top-down DIB.
bool ViaGdi(const Image32& im, std::vector<uint32_t>& out) {
    Canvas c = MakeCanvas(im.w, im.h);
    if (!c.bmp) return false;
    HDC dc = CreateCompatibleDC(nullptr); HGDIOBJ old = SelectObject(dc, c.bmp);
    DrawImage(Dc(dc), im, 0, 0);
    out.assign(c.bits, c.bits + static_cast<size_t>(im.w) * im.h);
    SelectObject(dc, old); DeleteDC(dc); DeleteObject(c.bmp);
    return true;
}
}  // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Fmt { const char* name; GUID id; };
    const Fmt fmts[] = {{"PNG", GUID_ContainerFormatPng}, {"BMP", GUID_ContainerFormatBmp}, {"JPEG", GUID_ContainerFormatJpeg}, {"TIFF", GUID_ContainerFormatTiff}};
    const int sizes[][2] = {{64, 32}, {32, 64}, {200, 113}, {480, 270}, {7, 5}};   // landscape, portrait, 16:9, thumbnail source, tiny
    struct Mode { const char* name; int w, h; bool cover; };
    const Mode modes[] = {{"fit 160x90", 160, 90, false}, {"cover 160x90 (wallpaper thumbnail)", 160, 90, true}, {"fit 48x48", 48, 48, false}, {"cover 100x100", 100, 100, true}};

    D2DSurface s;
    bool d2dOk = s.Probe();
    Check(d2dOk, "a Direct2D device (hardware or WARP) is available for the test");

    int combos = 0, expected = 0;
    for (const Fmt& f : fmts) {
        for (const auto& sz : sizes) {
            if (f.id == GUID_ContainerFormatJpeg && sz[0] < 16) continue;   // a JPEG smaller than its 8x8 blocks cannot hold the pattern (a limit of the format, not of the code)
            ++expected;
            const bool block = sz[0] >= 32 && sz[1] >= 32;   // a block of a few pixels would not survive JPEG
            const std::vector<uint8_t> file = Encode(f.id, sz[0], sz[1], block);
            Check(!file.empty(), std::string(f.name) + " " + std::to_string(sz[0]) + "x" + std::to_string(sz[1]) + ": test picture encoded");
            if (file.empty()) continue;
            for (const Mode& m : modes) {
                const std::string tag = std::string(f.name) + " " + std::to_string(sz[0]) + "x" + std::to_string(sz[1]) + " " + m.name;
                Image32 im = DecodeImage(file.data(), file.size(), m.w, m.h, m.cover);
                Check(im.bmp && im.w > 0 && im.h > 0, tag + ": decoded");
                if (!im.bmp) continue;
                ++combos;
                const bool corner = block && !m.cover;   // cover crops the edges: the corner is then not shown
                CheckPixels(ToPixels(im).p, im.w, im.h, corner, tag + " [decoded pixels]");
                std::vector<uint32_t> g;
                Check(ViaGdi(im, g), tag + ": GDI draw works");
                if (!g.empty()) CheckPixels(g, im.w, im.h, corner, tag + " [GDI path]");
                if (d2dOk) {
                    std::vector<uint32_t> d, d2;
                    const bool ok = ViaD2D(s, im, d);
                    Check(ok, tag + ": Direct2D draw works");
                    if (ok) {
                        CheckPixels(d, im.w, im.h, corner, tag + " [Direct2D path]");
                        Check(ViaD2D(s, im, d2) && d2 == d, tag + ": a second draw (cached device bitmap) is identical");
                    }
                    s.Forget(im.bmp);
                }
                DeleteObject(im.bmp);
            }
        }
    }
    Check(expected == 19 && combos == expected * 4, "every format x size x mode combination was decoded (" + std::to_string(combos) + " of " + std::to_string(expected * 4) + ")");

    {   // the preview pipeline reads the wallpaper file and never writes it: the bytes on disk are identical afterwards
        wchar_t tmp[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = tmp; while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
        dir += L"\\ankh-ft1-orient";
        EnsureDir(dir);
        const std::wstring path = dir + L"\\wallpaper.png";
        const std::vector<uint8_t> png = Encode(GUID_ContainerFormatPng, 200, 113, true);
        Check(WriteAll(path, png.data(), png.size()), "temp wallpaper written");
        std::vector<uint8_t> before, after, work;
        Check(ReadAll(path, before) && before == png, "temp wallpaper reads back unchanged");
        if (ReadAll(path, work)) { Image32 im = DecodeImage(work.data(), work.size(), 480, 270, true); if (im.bmp) { if (d2dOk) { std::vector<uint32_t> d; ViaD2D(s, im, d); s.Forget(im.bmp); } DeleteObject(im.bmp); } }
        Check(ReadAll(path, after) && after == before, "after decode and draw the wallpaper file is byte-identical (originals are never modified)");
        DeleteFileW(path.c_str()); RemoveDirectoryW(dir.c_str());
    }

    {   // FT-B01: a picture stored with an EXIF / TIFF orientation tag is shown the way the tag says (the file is not modified)
        // stored pixel (x, y) of a W x H picture is shown at: 1 (x,y); 2 (W-1-x,y); 3 (W-1-x,H-1-y); 4 (x,H-1-y); 5 (y,x); 6 (H-1-y,x); 7 (H-1-y,W-1-x); 8 (y,W-1-x)
        const int W = 40, H = 24;
        std::vector<uint32_t> stored(static_cast<size_t>(W) * H);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) stored[static_cast<size_t>(y) * W + x] = 0xFF000000u | (static_cast<uint32_t>(x * 6) << 16) | (static_cast<uint32_t>(y * 10) << 8) | 100u;
        struct C { const char* name; GUID id; };
        for (const C& c : {C{"TIFF", GUID_ContainerFormatTiff}, C{"JPEG", GUID_ContainerFormatJpeg}}) {
            for (UINT o = 1; o <= 8; ++o) {
                const std::vector<uint8_t> file = EncodePixels(c.id, W, H, stored, o);
                const int dw = o >= 5 ? H : W, dh = o >= 5 ? W : H;
                const std::string tag = std::string(c.name) + " orientation " + std::to_string(o);
                Check(!file.empty(), tag + ": encoded");
                Image32 im = DecodeImage(file.data(), file.size(), dw, dh, false);
                Check(im.bmp && im.w == dw && im.h == dh, tag + ": shown at " + std::to_string(dw) + "x" + std::to_string(dh) + " (width and height swap for 5-8)");
                if (!im.bmp || im.w != dw || im.h != dh) { if (im.bmp) DeleteObject(im.bmp); continue; }
                const Px got = ToPixels(im); DeleteObject(im.bmp);
                int bad = 0;
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    int X = x, Y = y;
                    switch (o) { case 2: X = W - 1 - x; break; case 3: X = W - 1 - x; Y = H - 1 - y; break; case 4: Y = H - 1 - y; break; case 5: X = y; Y = x; break;
                                 case 6: X = H - 1 - y; Y = x; break; case 7: X = H - 1 - y; Y = W - 1 - x; break; case 8: X = y; Y = W - 1 - x; break; default: break; }
                    const uint32_t e = stored[static_cast<size_t>(y) * W + x], g = got.p[static_cast<size_t>(Y) * dw + X];
                    const int tol = c.id == GUID_ContainerFormatJpeg ? 40 : 2;
                    auto d = [](uint32_t a, uint32_t b, int s) { const int u = static_cast<int>((a >> s) & 255), v = static_cast<int>((b >> s) & 255); return u > v ? u - v : v - u; };
                    if (d(e, g, 16) > tol || d(e, g, 8) > tol) ++bad;
                }
                Check(bad == 0, tag + ": every pixel is where the orientation tag puts it (" + std::to_string(bad) + " wrong)");
            }
        }
        // a picture without a tag, and a tag outside 1..8, are shown as stored
        const std::vector<uint8_t> plain = EncodePixels(GUID_ContainerFormatTiff, W, H, stored, 0), odd = EncodePixels(GUID_ContainerFormatTiff, W, H, stored, 9);
        Image32 a = DecodeImage(plain.data(), plain.size(), W, H, false), b = DecodeImage(odd.data(), odd.size(), W, H, false);
        Check(a.bmp && b.bmp && a.w == W && b.w == W && ToPixels(a).p == stored && ToPixels(b).p == stored, "orientation: no tag, or an invalid tag, shows the picture as stored");
        if (a.bmp) DeleteObject(a.bmp); if (b.bmp) DeleteObject(b.bmp);
    }
    std::printf("%s (%d passed, %d failed)\n", fails ? "FAILED" : "ALL PASSED", passes, fails);
    return fails ? 1 : 0;
}
