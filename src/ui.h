// AnkhCore — UI kit shared by the setup and the control center: neon-blue
// palette, fonts, GDI drawing helpers, WIC images, the animated logo
// (tint, glow, pulse, glitch) and immediate-mode widgets.
#pragma once
#include "base.h"
#include "gfx.h"
#include "anim.h"
#include "op_progress.h"
#include "brand_logo.h"
#include "theme.h"

namespace ak {

// ---------------------------------------------------------------- palette (Dual Theme System: Neutral Windows Glass [default] + Classic AnkhCore)
namespace col {
inline const ThemeSpec kT0 = WindowsGlassTheme();
inline COLORREF bg = kT0.bg, side = kT0.side, panel = kT0.panel, panel2 = kT0.panel2,
                border = kT0.border, borderHi = kT0.borderHi, accent = kT0.accent, accentDim = kT0.accentDim,
                accentDeep = kT0.accentDeep, text = kT0.text, sub = kT0.sub, faint = kT0.faint,
                ok = kT0.ok, warn = kT0.warn, err = kT0.err, track = kT0.track, overlay = kT0.overlay;
}
inline void ApplyTheme(const std::wstring& id) {
    const ThemeSpec t = ThemeById(id);
    MutableActiveTheme() = t;
    col::bg = t.bg; col::side = t.side; col::panel = t.panel; col::panel2 = t.panel2;
    col::border = t.border; col::borderHi = t.borderHi; col::accent = t.accent; col::accentDim = t.accentDim;
    col::accentDeep = t.accentDeep; col::text = t.text; col::sub = t.sub; col::faint = t.faint;
    col::ok = t.ok; col::warn = t.warn; col::err = t.err; col::track = t.track; col::overlay = t.overlay;
}

struct Fonts { HFONT title, h1, h2, text, small, tiny, btn, icon, iconBig; };
inline bool FontInstalled(const wchar_t* face) {
    HDC d = GetDC(nullptr); LOGFONTW lf{}; lf.lfCharSet = DEFAULT_CHARSET; wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    EnumFontFamiliesExW(d, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int { *reinterpret_cast<bool*>(p) = true; return 0; }, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, d);
    return found;
}
inline bool& LegacyFontsForced() { static bool v = false; return v; }   // test build: exercises the Windows 10 font path
inline const wchar_t* GlyphMemory() { static const bool fluent = FontInstalled(L"Segoe Fluent Icons"); return fluent && !LegacyFontsForced() ? L"\xEEA0" : L"\xE950"; }   // not in Segoe MDL2
inline Fonts MakeFonts(int dpi) {
    const bool variable = !LegacyFontsForced() && FontInstalled(L"Segoe UI Variable Text"), fluent = !LegacyFontsForced() && FontInstalled(L"Segoe Fluent Icons");
    const wchar_t* display = variable ? L"Segoe UI Variable Display" : L"Segoe UI", *body = variable ? L"Segoe UI Variable Text" : L"Segoe UI", *icons = fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto f = [&](int px, int w, const wchar_t* face) {
        return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    return {f(28, FW_SEMIBOLD, display), f(26, FW_SEMIBOLD, display), f(16, FW_SEMIBOLD, display), f(13, FW_NORMAL, body),
            f(12, FW_NORMAL, body), f(11, FW_NORMAL, body), f(13, FW_SEMIBOLD, body),
            f(16, FW_NORMAL, icons), f(22, FW_NORMAL, icons)};
}

inline COLORREF Mix(COLORREF a, COLORREF b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t, GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
}
inline void FillRect2(Dc dc, const RECT& r, COLORREF c) {
    if (dc.d2d()) { dc.d->dc->FillRectangle(D2DRect(r), dc.d->Brush(c)); return; }
    HBRUSH b = CreateSolidBrush(c); FillRect(dc.hdc, &r, b); DeleteObject(b);
}
// vertical gradient fill (top -> bottom)
inline void VGrad(Dc dc, const RECT& r, COLORREF top, COLORREF bot) {
    if (dc.d2d()) {
        const auto t = D2D1::Point2F(static_cast<float>(r.left), static_cast<float>(r.top)), b = D2D1::Point2F(static_cast<float>(r.left), static_cast<float>(r.bottom));
        if (auto* g = dc.d->Gradient(top, 1.f, bot, 1.f, t, b)) dc.d->dc->FillRectangle(D2DRect(r), g);
        return;
    }
    TRIVERTEX v[2] = {
        { r.left,  r.top,    (USHORT)(GetRValue(top) << 8), (USHORT)(GetGValue(top) << 8), (USHORT)(GetBValue(top) << 8), 0 },
        { r.right, r.bottom, (USHORT)(GetRValue(bot) << 8), (USHORT)(GetGValue(bot) << 8), (USHORT)(GetBValue(bot) << 8), 0 }
    };
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc.hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}
// How see-through a palette surface is on the glass renderer (GDI has no alpha, so it stays opaque there).
inline float GlassAlpha(COLORREF c) { return ThemeGlassAlpha(c); }
inline void FillGlass(Dc dc, const RECT& r, COLORREF c, float alpha) {
    if (dc.d2d()) dc.d->dc->FillRectangle(D2DRect(r), dc.d->Brush(c, alpha)); else FillRect2(dc, r, c);
}
inline void Halo(Dc dc, int cx, int cy, int radius, COLORREF c, float alpha) {
    if (dc.d2d()) dc.d->Halo(static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(radius), c, alpha);
}
// `rad` keeps the GDI meaning (the corner ellipse's diameter), so Direct2D uses rad / 2.
// 2.9.8: the Clear material draws a blurred piece of the wallpaper under every glass card (set by the application; null elsewhere)
inline void (*g_cardBackdropHook)(Dc, const RECT&, int) = nullptr;
inline void Round(Dc dc, const RECT& r, int rad, COLORREF fill, COLORREF border) {
    if (dc.d2d()) {
        const float rr = std::min(rad / 2.0f, std::min(r.right - r.left, r.bottom - r.top) / 2.0f);
        const D2D1_RECT_F q = D2DRect(r);
        if (dc.d->logCards && fill == col::panel) dc.d->cardLog.push_back(q);
        if ((fill == col::panel || fill == col::panel2 || fill == col::overlay) && border == col::border) {
            const GlassLayerSpec gl = GlassLayer(fill == col::overlay ? GlassLevel::Overlay : fill == col::panel2 ? GlassLevel::Elevated : GlassLevel::Surface);
            const COLORREF sh = ActiveTheme().isClassic ? RGB(2, 6, 18) : RGB(0, 0, 0);
            const float sa = ActiveTheme().isClassic ? .14f : gl.shadowAlpha;
            dc.d->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left - 1, q.top, q.right + 1, q.bottom + 2), rr, rr), dc.d->Brush(sh, sa), 9.f);
        }
        if (g_cardBackdropHook && (fill == col::panel || fill == col::panel2)) g_cardBackdropHook(dc, r, rad);
        dc.d->dc->FillRoundedRectangle(D2D1::RoundedRect(q, rr, rr), dc.d->Brush(fill, GlassAlpha(fill)));
        if (border != fill) {
            const GlassLayerSpec gl = GlassLayer(fill == col::overlay ? GlassLevel::Overlay : fill == col::panel2 ? GlassLevel::Elevated : GlassLevel::Surface);
            ID2D1Brush* edge = nullptr;
            if (border == col::border) {
                edge = ActiveTheme().isClassic
                     ? static_cast<ID2D1Brush*>(dc.d->Gradient(col::accent, .35f, col::bg, .10f, D2D1::Point2F(q.left, q.top), D2D1::Point2F(q.left, q.bottom)))
                     : static_cast<ID2D1Brush*>(dc.d->Gradient(RGB(255, 255, 255), gl.topEdgeAlpha, RGB(255, 255, 255), gl.bottomEdgeAlpha, D2D1::Point2F(q.left, q.top), D2D1::Point2F(q.left, q.bottom)));
            }
            if (!edge) edge = dc.d->Brush(border);
            dc.d->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left + .5f, q.top + .5f, q.right - .5f, q.bottom - .5f), std::max(0.f, rr - .5f), std::max(0.f, rr - .5f)), edge);
        }
        return;
    }
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc.hdc, b), op = SelectObject(dc.hdc, p);
    RoundRect(dc.hdc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc.hdc, ob); SelectObject(dc.hdc, op); DeleteObject(b); DeleteObject(p);
}
inline void Ellipse2(Dc dc, const RECT& r, COLORREF fill, COLORREF border) {
    if (dc.d2d()) {
        const float rx = (r.right - r.left) / 2.0f, ry = (r.bottom - r.top) / 2.0f;
        const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(r.left + rx, r.top + ry), rx, ry);
        dc.d->dc->FillEllipse(e, dc.d->Brush(fill));
        if (border != fill) dc.d->dc->DrawEllipse(D2D1::Ellipse(e.point, std::max(0.f, rx - .5f), std::max(0.f, ry - .5f)), dc.d->Brush(border));
        return;
    }
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc.hdc, b), op = SelectObject(dc.hdc, p);
    Ellipse(dc.hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc.hdc, ob); SelectObject(dc.hdc, op); DeleteObject(b); DeleteObject(p);
}
inline void Line(Dc dc, int x0, int y0, int x1, int y1, COLORREF c, int w = 1) {
    if (dc.d2d()) {
        const float o = (w & 1) ? 0.5f : 0.f;   // odd widths sit on the pixel centre, so straight lines stay crisp
        dc.d->dc->DrawLine(D2D1::Point2F(x0 + o, y0 + o), D2D1::Point2F(x1 + o, y1 + o), dc.d->Brush(c), static_cast<float>(w));
        return;
    }
    HPEN p = CreatePen(PS_SOLID, w, c); HGDIOBJ op = SelectObject(dc.hdc, p);
    MoveToEx(dc.hdc, x0, y0, nullptr); LineTo(dc.hdc, x1, y1); SelectObject(dc.hdc, op); DeleteObject(p);
}
// Horizontal gradient bar (GDI: the end colour, flat).
inline void BarFill(Dc dc, const RECT& r, int rad, COLORREF from, COLORREF to) {
    if (dc.d2d()) {
        const D2D1_RECT_F q = D2DRect(r); const float rr = std::min(rad / 2.0f, (q.bottom - q.top) / 2.0f);
        if (auto* g = dc.d->Gradient(from, 1.f, to, 1.f, D2D1::Point2F(q.left, q.top), D2D1::Point2F(q.right, q.top))) dc.d->dc->FillRoundedRectangle(D2D1::RoundedRect(q, rr, rr), g);
        return;
    }
    Round(dc, r, rad, to, to);
}
// Border only, for frames over a background that is already drawn.
inline void Outline(Dc dc, const RECT& r, int rad, COLORREF c) {
    if (dc.d2d()) {
        const D2D1_RECT_F q = D2DRect(r); const float rr = std::max(0.f, std::min(rad / 2.0f, (q.bottom - q.top) / 2.0f) - .5f);
        dc.d->dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left + .5f, q.top + .5f, q.right - .5f, q.bottom - .5f), rr, rr), dc.d->Brush(c));
        return;
    }
    Round(dc, r, rad, col::side, c);
}
inline void Text(Dc dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt = DT_LEFT | DT_TOP | DT_SINGLELINE) {
    if (dc.d2d()) {
        if (s.empty()) return;
        if (auto l = dc.d->Layout(s, f, static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top), fmt))
            dc.d->dc->DrawTextLayout(D2D1::Point2F(static_cast<float>(r.left), static_cast<float>(r.top)), l.Get(), dc.d->Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    SelectObject(dc.hdc, f); SetTextColor(dc.hdc, c); SetBkMode(dc.hdc, TRANSPARENT);
    DrawTextW(dc.hdc, s.c_str(), -1, &r, fmt | DT_NOPREFIX);
}
inline int TextHeight(Dc dc, const std::wstring& s, int width, HFONT f) {
    if (s.empty()) return 0;
    if (dc.d2d()) {
        DWRITE_TEXT_METRICS m{};
        auto l = dc.d->Layout(s, f, static_cast<float>(width), 100000.f, DT_WORDBREAK);
        return l && SUCCEEDED(l->GetMetrics(&m)) ? static_cast<int>(ceilf(m.height)) : 0;
    }
    SelectObject(dc.hdc, f); RECT r{0, 0, width, 0};
    DrawTextW(dc.hdc, s.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    return r.bottom;
}
inline int TextWidth(Dc dc, const std::wstring& s, HFONT f) {
    if (dc.d2d()) {
        DWRITE_TEXT_METRICS m{};
        auto l = dc.d->Layout(s, f, 100000.f, 1000.f, DT_SINGLELINE);
        return l && SUCCEEDED(l->GetMetrics(&m)) ? static_cast<int>(ceilf(m.widthIncludingTrailingWhitespace)) : 0;
    }
    SelectObject(dc.hdc, f); SIZE sz{}; GetTextExtentPoint32W(dc.hdc, s.c_str(), static_cast<int>(s.size()), &sz); return sz.cx;
}

// ---------------------------------------------------------------- images (WIC)
struct Image32 { HBITMAP bmp = nullptr; int w = 0, h = 0; };
// EXIF / TIFF orientation (1..8) of a decoded frame, 1 when the file has none (FT-1 / B01: previews follow the picture's own
// orientation tag, as Explorer and the wallpaper do; the file is never modified).
inline UINT PictureOrientation(IWICBitmapFrameDecode* fr) {
    IWICMetadataQueryReader* q = nullptr; UINT o = 1;
    if (SUCCEEDED(fr->GetMetadataQueryReader(&q)) && q) {
        for (const wchar_t* name : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(q->GetMetadataByName(name, &pv)) && (pv.vt == VT_UI2 || pv.vt == VT_UI1 || pv.vt == VT_UI4) && pv.uiVal >= 1 && pv.uiVal <= 8) { o = pv.uiVal; PropVariantClear(&pv); break; }
            PropVariantClear(&pv);
        }
        q->Release();
    }
    return o;
}
// The WIC transform that shows a picture stored with EXIF orientation `o` upright.
inline WICBitmapTransformOptions PictureOrientationTransform(UINT o) {
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    default: return WICBitmapTransformRotate0;
    }
}
inline Image32 DecodeImage(const uint8_t* data, size_t size, int maxW, int maxH, bool cover = false) {
    Image32 out;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    IWICStream* s = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapFlipRotator* fl = nullptr;
    IWICFormatConverter* cv = nullptr; IWICBitmapScaler* sc = nullptr; IWICBitmapClipper* cl = nullptr;
    UINT w0 = 0, h0 = 0;
    if (SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))) &&
        SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
        IWICBitmapSource* upright = fr;   // the stored picture, turned by its orientation tag when it has one
        const UINT orient = PictureOrientation(fr);
        if (orient > 1 && SUCCEEDED(f->CreateBitmapFlipRotator(&fl)) && SUCCEEDED(fl->Initialize(fr, PictureOrientationTransform(orient)))) upright = fl;
        if (SUCCEEDED(upright->GetSize(&w0, &h0)) && w0 && h0 && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(upright, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            const double k = cover ? std::max(static_cast<double>(maxW) / w0, static_cast<double>(maxH) / h0)
                                   : std::min(static_cast<double>(maxW) / w0, static_cast<double>(maxH) / h0);
            const UINT w = std::max<UINT>(1, static_cast<UINT>(w0 * k + 0.5)), h = std::max<UINT>(1, static_cast<UINT>(h0 * k + 0.5));
            IWICBitmapSource* src = cv;
            if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(cv, w, h, WICBitmapInterpolationModeFant))) src = sc;
            UINT ow = w, oh = h;
            if (cover && (w > static_cast<UINT>(maxW) || h > static_cast<UINT>(maxH))) {
                WICRect rc{static_cast<INT>((w - std::min<UINT>(w, maxW)) / 2), static_cast<INT>((h - std::min<UINT>(h, maxH)) / 2),
                           static_cast<INT>(std::min<UINT>(w, maxW)), static_cast<INT>(std::min<UINT>(h, maxH))};
                if (SUCCEEDED(f->CreateBitmapClipper(&cl)) && SUCCEEDED(cl->Initialize(src, &rc))) { src = cl; ow = rc.Width; oh = rc.Height; }
            }
            BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = static_cast<LONG>(ow);
            bi.bmiHeader.biHeight = -static_cast<LONG>(oh); bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && SUCCEEDED(src->CopyPixels(nullptr, ow * 4, ow * oh * 4, static_cast<BYTE*>(bits)))) { out.bmp = bmp; out.w = static_cast<int>(ow); out.h = static_cast<int>(oh); }
            else if (bmp) DeleteObject(bmp);
        }
    }
    if (cl) cl->Release(); if (sc) sc->Release(); if (cv) cv->Release(); if (fl) fl->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (s) s->Release();
    f->Release();
    return out;
}
inline bool ResourceBytes(const wchar_t* name, const uint8_t*& data, size_t& size) {
    HRSRC r = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10));
    if (!r) return false;
    data = static_cast<const uint8_t*>(LockResource(LoadResource(nullptr, r)));
    size = SizeofResource(nullptr, r);
    return data && size;
}
inline Image32 LoadImageRes(const wchar_t* name, int maxW, int maxH, bool cover = false) {
    const uint8_t* d; size_t n;
    return ResourceBytes(name, d, n) ? DecodeImage(d, n, maxW, maxH, cover) : Image32{};
}
inline void DrawImage(Dc dc, const Image32& im, int x, int y, BYTE alpha = 255) {
    if (!im.bmp) return;
    if (dc.d2d()) {
        if (ID2D1Bitmap* b = dc.d->DibBitmap(im.bmp))
            dc.d->dc->DrawBitmap(b, D2D1::RectF(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + im.w), static_cast<float>(y + im.h)), alpha / 255.f);
        return;
    }
    HDC mem = CreateCompatibleDC(dc.hdc); HGDIOBJ old = SelectObject(mem, im.bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    GdiAlphaBlend(dc.hdc, x, y, im.w, im.h, mem, 0, 0, im.w, im.h, bf);
    SelectObject(mem, old); DeleteDC(mem);
}

// ---------------------------------------------------------------- logo effects
struct Px { std::vector<uint32_t> p; int w = 0, h = 0; };
struct Canvas { HBITMAP bmp = nullptr; uint32_t* bits = nullptr; int w = 0, h = 0; };
inline Px ToPixels(const Image32& im) {
    Px out; DIBSECTION ds{};
    if (!im.bmp || !GetObjectW(im.bmp, sizeof(ds), &ds) || !ds.dsBm.bmBits) return out;
    out.w = im.w; out.h = im.h; out.p.resize(static_cast<size_t>(im.w) * im.h);
    const auto* src = static_cast<const uint8_t*>(ds.dsBm.bmBits);
    for (int y = 0; y < im.h; ++y) memcpy(&out.p[static_cast<size_t>(y) * im.w], src + static_cast<size_t>(y) * ds.dsBm.bmWidthBytes, im.w * 4);
    return out;
}
inline Canvas MakeCanvas(int w, int h) {
    Canvas c; BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    c.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (c.bmp) { c.bits = static_cast<uint32_t*>(bits); c.w = w; c.h = h; }
    return c;
}
inline uint32_t Hash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
inline float Rand01(uint32_t x) { return (Hash(x) & 0xFFFFFF) / 16777215.0f; }
inline uint32_t PackPx(int b, int g, int r, int a) {
    return static_cast<uint32_t>(std::min(b, 255)) | (static_cast<uint32_t>(std::min(g, 255)) << 8) |
           (static_cast<uint32_t>(std::min(r, 255)) << 16) | (static_cast<uint32_t>(std::min(a, 255)) << 24);
}
// Grey tone map (white = line core) -> neon blue with the gradient kept.
inline Px TintNeon(const Px& src) {
    Px out = src;
    for (auto& v : out.p) {
        const int a = v >> 24; if (!a) { v = 0; continue; }
        const float shade = std::min(1.0f, ((v >> 16) & 255) / static_cast<float>(a));
        const float k = 0.35f + 0.65f * shade, m = shade * shade * shade * 0.55f;
        const float r = (0 * k) * (1 - m) + 170 * m, g = (150 * k) * (1 - m) + 235 * m, b = (255 * k) * (1 - m) + 255 * m;
        v = PackPx(static_cast<int>(b * a / 255), static_cast<int>(g * a / 255), static_cast<int>(r * a / 255), a);
    }
    return out;
}
inline Px MakeGlow(const Px& src, int pad, int radius) {
    const int w = src.w + 2 * pad, h = src.h + 2 * pad;
    std::vector<float> a(static_cast<size_t>(w) * h, 0.0f), t(a.size());
    for (int y = 0; y < src.h; ++y) for (int x = 0; x < src.w; ++x) a[static_cast<size_t>(y + pad) * w + x + pad] = (src.p[static_cast<size_t>(y) * src.w + x] >> 24) / 255.0f;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            float s = 0; const float* row = &a[static_cast<size_t>(y) * w];
            for (int x = -radius; x <= radius; ++x) s += row[std::clamp(x, 0, w - 1)];
            for (int x = 0; x < w; ++x) { t[static_cast<size_t>(y) * w + x] = s / (2 * radius + 1); s += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)]; }
        }
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int y = -radius; y <= radius; ++y) s += t[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) { a[static_cast<size_t>(y) * w + x] = s / (2 * radius + 1); s += t[static_cast<size_t>(std::min(y + radius + 1, h - 1)) * w + x] - t[static_cast<size_t>(std::max(y - radius, 0)) * w + x]; }
        }
    }
    Px out; out.w = w; out.h = h; out.p.resize(a.size());
    for (size_t i = 0; i < a.size(); ++i) { const int v = static_cast<int>(std::min(1.0f, a[i] * 1.6f) * 255); out.p[i] = PackPx(255 * v / 255, 160 * v / 255, 0, v); }
    return out;
}
struct FxParams { float bright = 1.0f, glow = 0.0f, glitch = 0.0f; uint32_t seed = 0; };
inline void Compose(const Px& src, const Px* glow, Canvas& dst, int pad, const FxParams& fx) {
    const int W = dst.w, H = dst.h;
    const int bright = static_cast<int>(std::clamp(fx.bright, 0.0f, 1.0f) * 256), glowK = static_cast<int>(std::clamp(fx.glow, 0.0f, 1.0f) * 256);
    const float g = std::clamp(fx.glitch, 0.0f, 1.0f);
    const int split = g > 0 ? static_cast<int>(g * src.w * 0.035f * (Rand01(fx.seed * 7 + 1) < 0.5f ? -1 : 1)) : 0;
    const int maxShift = static_cast<int>(g * src.w * 0.12f);
    const int band = std::max(2, static_cast<int>(src.h / (8 + Rand01(fx.seed + 3) * 18)));
    const int bandOff = static_cast<int>(Rand01(fx.seed + 5) * band);
    auto fetch = [&](int x, int y) -> uint32_t { return (x < 0 || y < 0 || x >= src.w || y >= src.h) ? 0u : src.p[static_cast<size_t>(y) * src.w + x]; };
    for (int y = 0; y < H; ++y) {
        uint32_t* out = dst.bits + static_cast<size_t>(y) * W;
        const int sy = y - pad;
        int dx = 0, rowBright = bright;
        if (g > 0) {
            const uint32_t b = static_cast<uint32_t>((y + bandOff) / band) * 2654435761u ^ fx.seed;
            if (Rand01(b) < 0.10f + 0.45f * g) dx = static_cast<int>((Rand01(b + 11) * 2 - 1) * maxShift);
            if (Rand01(b + 23) < 0.08f * g) rowBright = 0;
            if (y & 1) rowBright = rowBright * (256 - static_cast<int>(70 * g)) / 256;
        }
        const uint32_t* grow = glow ? &glow->p[static_cast<size_t>(y) * W] : nullptr;
        for (int x = 0; x < W; ++x) {
            const int sx = x - pad - dx;
            int r, gg, b, a;
            if (!split) { const uint32_t v = fetch(sx, sy); b = v & 255; gg = (v >> 8) & 255; r = (v >> 16) & 255; a = v >> 24; }
            else {
                const uint32_t vr = fetch(sx + split, sy), vg = fetch(sx, sy), vb = fetch(sx - split, sy);
                r = (vr >> 16) & 255; gg = (vg >> 8) & 255; b = vb & 255;
                a = std::max({static_cast<int>(vr >> 24), static_cast<int>(vg >> 24), static_cast<int>(vb >> 24), r, gg, b});
            }
            r = r * rowBright >> 8; gg = gg * rowBright >> 8; b = b * rowBright >> 8; a = a * rowBright >> 8;
            if (grow && glowK) { const uint32_t q = grow[x]; r += ((q >> 16) & 255) * glowK >> 8; gg += ((q >> 8) & 255) * glowK >> 8; b += (q & 255) * glowK >> 8; a += (q >> 24) * glowK >> 8; }
            out[x] = PackPx(b, gg, r, a);
        }
    }
}
inline float GlitchBurst(double t, float base, float peak) {
    const uint32_t slot = static_cast<uint32_t>(t / 0.28);
    const float r = Rand01(slot * 977 + 13);
    if (r < 0.45f) return base + (peak - base) * (0.5f + 0.5f * Rand01(static_cast<uint32_t>(t / 0.05) + 101));
    if (r < 0.70f) return base * 0.5f;
    return 0.0f;
}
// Logo.jpg is artwork on a black field. Owner decision 2026-11-13: only the glowing symbol shows. Each pixel keeps
// its own colour and its opacity is its brightness (light added to a dark surface), so the black field vanishes
// and nothing of the drawing is recoloured, redrawn or cropped.
inline void LightOnDark(uint32_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) { const uint32_t r = (p[i] >> 16) & 255, g = (p[i] >> 8) & 255, b = p[i] & 255; p[i] = (std::max(r, std::max(g, b)) << 24) | (p[i] & 0x00FFFFFFu); }
}
// The animated background logo (A + Eye of Horus, neon blue).
struct Logo {
    Px px, glow; Canvas canvas; int pad = 0; DWORD start = GetTickCount();
    // tint=false keeps the image's own colours (the official ankh mark is never recoloured)
    void Load(const wchar_t* res, int w, int h, bool tint = true) {
        Image32 im = LoadImageRes(res, w, h);
        if (!im.bmp) return;
        px = tint ? TintNeon(ToPixels(im)) : ToPixels(im); DeleteObject(im.bmp);
        pad = px.w / 6; glow = MakeGlow(px, pad, std::max(3, px.w / 28));
        canvas = MakeCanvas(px.w + 2 * pad, px.h + 2 * pad);
        restCached = false;
    }
    // mode: 0 rest (breathing), 1 busy (glitch bursts), 2 success, 3 error flash
    // breathe=false: rest is a still frame (no 30 fps repaint needed while idle); it is composed once and reused.
    bool restCached = false;
    void Draw(Dc dc, int cx, int cy, int mode, float restMax = 1.0f, bool breathe = true) {
        if (!canvas.bmp) return;
        const double t = (GetTickCount() - start) / 1000.0;
        const bool still = mode == 0 && !breathe;
        const float p = still ? 0.5f : 0.5f + 0.5f * sinf(static_cast<float>(t * 2.0 * 3.14159265 / 3.2));
        if (still && restCached) { Blit(dc, cx, cy, false); return; }
        restCached = still;
        FxParams fx;
        if (mode == 1) { fx.bright = 0.78f + 0.12f * p; fx.glow = 0.30f + 0.25f * p; fx.glitch = GlitchBurst(t, 0.15f, 0.8f); fx.seed = Hash(static_cast<uint32_t>(t / 0.05) + 17); }
        else if (mode == 2) { fx.bright = 0.8f; fx.glow = 0.2f + 0.25f * p; }
        else if (mode == 3) { fx.bright = 0.7f; fx.glow = 0.2f; fx.glitch = 0.5f + 0.4f * Rand01(static_cast<uint32_t>(t / 0.05)); fx.seed = Hash(static_cast<uint32_t>(t / 0.05)); }
        else if (still) { fx.bright = restMax; fx.glow = 0.22f * restMax; }   // the brand mark at rest: full strength, never dimmed
        else { fx.bright = (0.18f + 0.36f * p) * restMax; fx.glow = (0.05f + 0.38f * p) * restMax; }
        Compose(px, &glow, canvas, pad, fx);
        Blit(dc, cx, cy);
    }
    void Blit(Dc dc, int cx, int cy, bool changed = true) { DrawPixels(dc, this, canvas.bmp, canvas.bits, canvas.w, canvas.h, cx - canvas.w / 2, cy - canvas.h / 2, changed); }
};

// ---------------------------------------------------------------- immediate-mode widgets
struct Hit { RECT r; std::function<void()> click; };
struct Ui {
    HWND wnd = nullptr; int dpi = 96; Fonts f{}; POINT mouse{-1, -1}; bool down = false;
    std::vector<Hit> hits; Animator* anim = nullptr;   // motion: null in the setup (values jump)
    int keyDy = 0;   // page scroll while a page draws: tween ids follow the content, not the screen position (else scrolling re-targets another control's animation)
    float Tween(int a, int b, int kind, bool on, int ms = 120, Ease e = Ease::OutCubic) { return anim ? anim->To(AnimId(a, b + keyDy, kind), on ? 1.f : 0.f, ms, e) : (on ? 1.f : 0.f); }
    int S(int v) const { return MulDiv(v, dpi, 96); }
    RECT R(int x, int y, int w, int h) const { return RECT{S(x), S(y), S(x + w), S(y + h)}; }
    bool Over(const RECT& r) const { return PtInRect(&r, mouse) != 0; }
    void Add(const RECT& r, std::function<void()> fn) { hits.push_back({r, std::move(fn)}); }
    bool Click(POINT p) {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (PtInRect(&it->r, p)) { auto fn = it->click; if (fn) fn(); return true; }
        return false;
    }
    bool OverAny() const { for (const auto& h : hits) if (PtInRect(&h.r, mouse)) return true; return false; }
};
// Pill toggle (Windows 11 style) in neon blue.
inline void DrawToggle(Ui& u, Dc dc, int x, int y, bool on, bool enabled, bool hover) {
    const int w = u.S(42), h = u.S(22);
    RECT tr{x, y, x + w, y + h};
    const float t = u.Tween(x, y, 1, on, 240, Ease::Spring);   // the knob slides on a spring
    const bool classic = ActiveTheme().isClassic;
    const COLORREF offF = hover ? col::panel2 : col::track, onF = hover ? (classic ? RGB(40, 196, 255) : RGB(236, 239, 245)) : col::accent;
    const COLORREF fill = !enabled ? col::track : t <= 0.f ? offF : t >= 1.f ? onF : Mix(col::track, onF, t);
    Round(dc, tr, h, fill, on && enabled ? col::accent : col::border);
    const int k = u.S(14), ky = y + (h - k) / 2, x0 = x + u.S(4), x1 = x + w - k - u.S(4), kx = x0 + static_cast<int>((x1 - x0) * t);
    RECT kr{kx, ky, kx + k, ky + k};
    if (on && enabled) Halo(dc, kx + k / 2, ky + k / 2, k, classic ? RGB(200, 240, 255) : RGB(255, 255, 255), classic ? .35f : .18f);
    Ellipse2(dc, kr, !enabled ? col::faint : on ? col::bg : col::sub, !enabled ? col::faint : on ? col::bg : col::sub);
}
inline void DrawCheck(Ui& u, Dc dc, int x, int y, bool on, bool enabled, bool hover) {
    const int s = u.S(20);
    RECT r{x, y, x + s, y + s};
    Round(dc, r, u.S(6), on && enabled ? col::accent : (hover ? col::panel2 : col::panel), on && enabled ? col::accent : (hover ? col::borderHi : col::border));
    if (on) Text(dc, L"\xE73E", r, u.f.icon, enabled ? col::bg : col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
enum class Btn { Primary, Secondary, Danger, Ghost };
inline void DrawButton(Ui& u, Dc dc, const RECT& r, const std::wstring& label, Btn kind, bool enabled, bool hover) {
    const bool classic = ActiveTheme().isClassic;
    COLORREF fill, border, text;
    switch (kind) {
        case Btn::Primary: fill = !enabled ? col::track : hover ? (classic ? RGB(40, 196, 255) : RGB(236, 239, 245)) : col::accent; border = fill; text = enabled ? col::bg : col::faint; break;
        case Btn::Danger: fill = hover && enabled ? RGB(68, 28, 32) : col::panel; border = enabled ? col::err : col::border; text = enabled ? col::err : col::faint; break;
        case Btn::Ghost: fill = hover && enabled ? col::panel2 : col::panel; border = col::border; text = enabled ? col::text : col::faint; break;
        default: fill = hover && enabled ? col::panel2 : col::panel; border = enabled ? (hover ? col::borderHi : col::border) : col::border; text = enabled ? (classic ? col::accent : col::text) : col::faint;
    }
    const float ht = u.Tween(r.left, r.top, 2, hover && enabled);
    if (kind == Btn::Primary && enabled && dc.d2d()) {
        const float t = static_cast<float>(r.top), b = static_cast<float>(r.bottom), rr = u.S(20) / 2.f;
        if (ht > .01f) Halo(dc, (r.left + r.right) / 2, (r.top + r.bottom) / 2, (r.right - r.left) * 3 / 5, classic ? col::accent : RGB(255, 255, 255), (classic ? .16f : .08f) * ht);
        const COLORREF topC = Mix(col::accent, classic ? RGB(40, 196, 255) : RGB(244, 246, 250), ht);
        const COLORREF botC = classic ? RGB(0, 134, 214) : RGB(178, 184, 196);
        if (auto* g = dc.d->Gradient(topC, 1.f, botC, 1.f, D2D1::Point2F(0, t), D2D1::Point2F(0, b)))
            dc.d->dc->FillRoundedRectangle(D2D1::RoundedRect(D2DRect(r), rr, rr), g);
        dc.d->dc->DrawLine(D2D1::Point2F(r.left + rr, t + .5f), D2D1::Point2F(r.right - rr, t + .5f), dc.d->Brush(classic ? RGB(200, 240, 255) : RGB(255, 255, 255), classic ? .45f : .35f));
    } else Round(dc, r, u.S(20), fill, border);
    Text(dc, label, r, u.f.btn, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
// 2.8.3: a button whose colour is a status (for example the Scan button turning green when the last scan was good). Same shape and text as the others.
inline void DrawStatusButton(Ui& u, Dc dc, const RECT& r, const std::wstring& label, COLORREF tone, bool enabled, bool hover) {
    const COLORREF fill = !enabled ? col::track : hover ? Mix(tone, RGB(255, 255, 255), .18) : tone;
    Round(dc, r, u.S(20), fill, fill);
    Text(dc, label, r, u.f.btn, enabled ? col::bg : col::faint, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
// Progress bar with a moving shine ("liquid") while active. pct < 0 (kProgressUnknown): the length of the work
// is not known, so a band travels along the track instead of a fill (LOG-2: never a full bar for unknown work).
inline void DrawProgress(Ui& u, Dc dc, const RECT& r, int pct, bool active, bool failed) {
    Round(dc, r, r.bottom - r.top, col::track, col::track);
    const ProgressFill pf = ProgressFillFor(pct, r.right - r.left, GetTickCount(), r.bottom - r.top);
    if (pf.band) {
        if (active && !failed && pf.right - pf.left >= r.bottom - r.top) { RECT b{r.left + pf.left, r.top, r.left + pf.right, r.bottom}; Round(dc, b, r.bottom - r.top, col::accent, col::accent); }
        return;
    }
    if (pf.right > 0) {
        RECT f = r; f.right = r.left + pf.right;
        Round(dc, f, r.bottom - r.top, failed ? col::err : col::accent, failed ? col::err : col::accent);
        if (active && !failed) {
            const int w = u.S(60), span = f.right - f.left + w;
            const int x = f.left - w + static_cast<int>((GetTickCount() % 1400) / 1400.0 * span);
            for (int i = 0; i < w; ++i) {
                const int xx = x + i; if (xx < f.left + 2 || xx >= f.right - 2) continue;
                const double a = sin(3.14159 * i / w);
                Line(dc, xx, f.top + 1, xx, f.bottom - 1, Mix(col::accent, RGB(255, 255, 255), a * 0.45));
            }
        }
    }
}
inline void DrawDot(Dc dc, int x, int y, int s, COLORREF c) { RECT r{x, y, x + s, y + s}; Ellipse2(dc, r, c, c); }

// Double-buffered paint helper.
struct Paint {
    HWND h; PAINTSTRUCT ps{}; HDC wdc = nullptr, dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ old = nullptr; RECT cr{};
    explicit Paint(HWND w) : h(w) {
        wdc = BeginPaint(h, &ps); GetClientRect(h, &cr);
        dc = CreateCompatibleDC(wdc); bmp = CreateCompatibleBitmap(wdc, cr.right, cr.bottom); old = SelectObject(dc, bmp);
        SetStretchBltMode(dc, HALFTONE);
    }
    ~Paint() {
        BitBlt(wdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc); EndPaint(h, &ps);
    }
};
inline void SetupWindowLook(HWND h) {
    const int round = 2; DwmSetWindowAttribute(h, 33, &round, sizeof(round));
    const BOOL dark = TRUE; DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));
    const COLORREF border = col::accentDeep; DwmSetWindowAttribute(h, 34, &border, sizeof(border));
}
inline int SystemDpi() { HDC dc = GetDC(nullptr); const int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96; if (dc) ReleaseDC(nullptr, dc); return d > 0 ? d : 96; }

// ---------------------------------------------------------------- floating intro (Anubis, glitches out)
struct Intro { HWND wnd = nullptr; Px img; Canvas canvas; int pad = 0; DWORD start = 0; };
inline Intro& IntroS() { static Intro i; return i; }
constexpr DWORD kIntroIn = 700, kIntroHold = 1300, kIntroOut = 950;
inline void IntroFrame() {
    Intro& in = IntroS();
    const DWORD t = GetTickCount() - in.start;
    FxParams fx;
    if (t < kIntroIn) { const float a = static_cast<float>(t) / kIntroIn; fx.bright = a * a * (3 - 2 * a); }
    else if (t < kIntroIn + kIntroHold) { fx.bright = 1.0f; const DWORD h = t - kIntroIn; if (h > kIntroHold - 260 && h < kIntroHold - 200) { fx.glitch = 0.35f; fx.seed = t / 40; } }
    else {
        const float uu = std::min(1.0f, static_cast<float>(t - kIntroIn - kIntroHold) / kIntroOut);
        fx.glitch = 0.35f + 0.65f * uu; fx.seed = Hash(t / 45 + 7); fx.bright = 1.0f - uu * uu;
        if (Rand01(t / 45 + 999) < 0.18f * uu) fx.bright *= 0.25f;
    }
    Compose(in.img, nullptr, in.canvas, in.pad, fx);
    HDC screen = GetDC(nullptr); HDC mem = CreateCompatibleDC(screen); HGDIOBJ old = SelectObject(mem, in.canvas.bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA}; POINT src{0, 0}; SIZE sz{in.canvas.w, in.canvas.h};
    UpdateLayeredWindow(in.wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
inline LRESULT CALLBACK IntroProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        IntroFrame();
        if (GetTickCount() - IntroS().start >= kIntroIn + kIntroHold + kIntroOut) { KillTimer(h, 1); DestroyWindow(h); }
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
inline void RunIntro(HINSTANCE inst, const wchar_t* res) {
    Intro& in = IntroS();
    const int dpi = SystemDpi();
    Image32 im = LoadImageRes(res, MulDiv(340, dpi, 96), MulDiv(340, dpi, 96));
    if (!im.bmp) return;
    in.img = ToPixels(im); DeleteObject(im.bmp);
    if (in.img.p.empty()) return;
    in.pad = in.img.w / 7;
    in.canvas = MakeCanvas(in.img.w + 2 * in.pad, in.img.h + 2 * in.pad);
    if (!in.canvas.bmp) return;
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = IntroProc; wc.hInstance = inst; wc.lpszClassName = L"AnkhCoreIntro";
    RegisterClassExW(&wc);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - in.canvas.w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - in.canvas.h) / 2;
    in.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, wc.lpszClassName, L"AnkhCore", WS_POPUP,
                             x, y, in.canvas.w, in.canvas.h, nullptr, nullptr, inst, nullptr);
    if (!in.wnd) { DeleteObject(in.canvas.bmp); return; }
    in.start = GetTickCount();
    IntroFrame();
    ShowWindow(in.wnd, SW_SHOWNOACTIVATE);
    SetTimer(in.wnd, 1, 15, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DeleteObject(in.canvas.bmp);
}

// ---------------------------------------------------------------- drawing context (immediate mode)
struct Ctx {
    Dc dc; Ui& u; int x, y, w; RECT clip; bool busy = false;
    int S(int v) const { return u.S(v); }
    bool Visible(const RECT& r) const { return r.bottom > clip.top && r.top < clip.bottom; }
    inline void Hit(const RECT& r, std::function<void()> fn) {
        RECT i; if (IntersectRect(&i, &r, &clip)) u.Add(i, std::move(fn));
    }
    bool Hover(const RECT& r) const { RECT i; return IntersectRect(&i, &r, &clip) && u.Over(i); }
};
inline void Section(Ctx& c, const std::wstring& title, const std::wstring& sub = L"") {
    c.y += c.S(10);
    Text(c.dc, title, RECT{c.x, c.y, c.x + c.w, c.y + c.S(26)}, c.u.f.h2, col::text);
    c.y += c.S(26);
    if (!sub.empty()) { const int h = TextHeight(c.dc, sub, c.w, c.u.f.small); Text(c.dc, sub, RECT{c.x, c.y, c.x + c.w, c.y + h}, c.u.f.small, col::sub, DT_WORDBREAK); c.y += h + c.S(4); }
    c.y += c.S(8);
}
inline void Chip(Ctx& c, int x, int y, const std::wstring& s, COLORREF color) {
    const int w = TextWidth(c.dc, s, c.u.f.tiny) + c.S(14);
    RECT r{x, y, x + w, y + c.S(18)};
    Round(c.dc, r, c.S(9), Mix(col::panel, color, 0.16), Mix(col::panel, color, 0.6));
    Text(c.dc, s, r, c.u.f.tiny, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
struct BtnSpec { std::wstring label; Btn kind; bool enabled; std::function<void()> fn; };
// Generic row: title, description, chips; right side: toggle, checkbox or buttons.
struct RowSpec {
    std::wstring title, desc; std::vector<std::pair<std::wstring, COLORREF>> chips;
    int toggle = -1; bool enabled = true; std::function<void()> onToggle;   // toggle: -1 none, 0/1 state
    std::vector<BtnSpec> buttons; std::wstring right; COLORREF rightColor = col::sub;
    int check = -1; std::function<void()> onCheck; const wchar_t* icon = nullptr; COLORREF iconColor = col::accent;
};
inline void Row(Ctx& c, const RowSpec& r) {
    int rightW = 0;
    if (r.toggle >= 0) rightW = c.S(60);
    for (const auto& b : r.buttons) rightW += std::max(c.S(96), TextWidth(c.dc, b.label, c.u.f.btn) + c.S(28)) + c.S(8);
    if (!r.right.empty()) rightW += TextWidth(c.dc, r.right, c.u.f.small) + c.S(16);
    const int lx = c.x + c.S(18) + (r.check >= 0 ? c.S(32) : 0) + (r.icon ? c.S(34) : 0);
    const int tw = c.w - (lx - c.x) - rightW - c.S(18);
    const int descH = TextHeight(c.dc, r.desc, tw, c.u.f.small);
    const int h = std::max(c.S(58), c.S(14) + c.S(22) + descH + c.S(14) + (r.chips.empty() ? 0 : c.S(22)));
    RECT box{c.x, c.y, c.x + c.w, c.y + h};
    if (c.Visible(box)) {
        const bool hov = c.Hover(box);
        Round(c.dc, box, c.S(28), hov ? col::panel2 : col::panel, hov ? Mix(col::border, col::accent, 0.35) : col::border);
        if (r.check >= 0) {
            const int cy = c.y + (h - c.S(20)) / 2;
            DrawCheck(c.u, c.dc, c.x + c.S(18), cy, r.check == 1, r.enabled, hov);
            c.Hit(RECT{c.x, c.y, lx, c.y + h}, r.onCheck);
        }
        if (r.icon) Text(c.dc, r.icon, RECT{lx - c.S(34), c.y, lx - c.S(4), c.y + h}, c.u.f.iconBig, r.iconColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        int ty = c.y + c.S(13);
        Text(c.dc, r.title, RECT{lx, ty, lx + tw, ty + c.S(22)}, c.u.f.btn, r.enabled ? col::text : col::faint, DT_SINGLELINE | DT_END_ELLIPSIS);
        ty += c.S(22);
        if (descH) { Text(c.dc, r.desc, RECT{lx, ty, lx + tw, ty + descH}, c.u.f.small, col::sub, DT_WORDBREAK); ty += descH + c.S(4); }
        int cx = lx;
        for (const auto& ch : r.chips) { Chip(c, cx, ty, ch.first, ch.second); cx += TextWidth(c.dc, ch.first, c.u.f.tiny) + c.S(22); }
        int rx = c.x + c.w - c.S(18);
        if (r.toggle >= 0) {
            rx -= c.S(42);
            RECT tr{rx, c.y + (h - c.S(22)) / 2, rx + c.S(42), c.y + (h + c.S(22)) / 2};
            DrawToggle(c.u, c.dc, tr.left, tr.top, r.toggle == 1, r.enabled && !c.busy, c.Hover(tr));
            if (r.enabled && r.onToggle) c.Hit(RECT{rx - c.S(10), c.y, rx + c.S(52), c.y + h}, r.onToggle);
            rx -= c.S(18);
        }
        for (auto it = r.buttons.rbegin(); it != r.buttons.rend(); ++it) {
            const int bw = std::max(c.S(96), TextWidth(c.dc, it->label, c.u.f.btn) + c.S(28));
            RECT br{rx - bw, c.y + (h - c.S(34)) / 2, rx, c.y + (h + c.S(34)) / 2};
            const bool en = it->enabled && !c.busy;
            DrawButton(c.u, c.dc, br, it->label, it->kind, en, c.Hover(br));
            if (en && it->fn) c.Hit(br, it->fn);
            rx -= bw + c.S(8);
        }
        if (!r.right.empty()) {
            const int w = TextWidth(c.dc, r.right, c.u.f.small);
            Text(c.dc, r.right, RECT{rx - w, c.y, rx, c.y + h}, c.u.f.small, r.rightColor, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    c.y += h + c.S(8);
}
inline void Buttons(Ctx& c, const std::vector<BtnSpec>& bs) {
    int x = c.x;
    for (const auto& b : bs) {
        const int bw = std::max(c.S(120), TextWidth(c.dc, b.label, c.u.f.btn) + c.S(32));
        RECT br{x, c.y, x + bw, c.y + c.S(38)};
        if (c.Visible(br)) { const bool en = b.enabled && !c.busy; DrawButton(c.u, c.dc, br, b.label, b.kind, en, c.Hover(br)); if (en && b.fn) c.Hit(br, b.fn); }
        x += bw + c.S(10);
    }
    c.y += c.S(38) + c.S(12);
}
inline void Para(Ctx& c, const std::wstring& s, HFONT f, COLORREF color) {
    const int h = TextHeight(c.dc, s, c.w, f);
    Text(c.dc, s, RECT{c.x, c.y, c.x + c.w, c.y + h}, f, color, DT_WORDBREAK);
    c.y += h + c.S(10);
}

}  // namespace ak
