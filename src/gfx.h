// AnkhCore 2.5 — drawing backend for the UI kit (redesign Phase 1).
// `Dc` is what every ui.h primitive draws on. It wraps either a GDI HDC (the setup, and the Desktop Sidebar /
// Mini Monitor until redesign Phase 7) or a Direct2D surface (the main window). The primitives keep their
// 2.2 signatures and geometry, so pages draw the same layout on either backend.
// D2DSurface: D3D11 device -> premultiplied flip-model swap chain -> DirectComposition target on the HWND;
// Direct2D 1.1 draws, DirectWrite renders text (grayscale antialiasing: required on a transparent target).
// Proven in the Phase 0 spike (docs: AnkhCore_2.5_Phase0_Acrylic_Spike.md): brushes and formats are cached.
#pragma once
#include "base.h"
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wincodec.h>
#include <d2d1effects.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <unordered_map>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxguid.lib")

namespace ak {
using Microsoft::WRL::ComPtr;

inline D2D1_COLOR_F D2DCol(COLORREF c, float a = 1.0f) { return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a); }
inline D2D1_RECT_F D2DRect(const RECT& r) { return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)); }

struct D2DSurface {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> d3d; ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIFactory2> fac;
    ComPtr<ID2D1Factory1> f; ComPtr<ID2D1Device> dev; ComPtr<ID2D1DeviceContext> dc;
    ComPtr<IDXGISwapChain1> sc; ComPtr<ID2D1Bitmap1> target, readback; UINT bmpW = 0, bmpH = 0;   // readback: offscreen mode (the widgets)
    ComPtr<ID2D1StrokeStyle> roundCap;
    ComPtr<ID2D1Bitmap1> layer; UINT layerW = 0, layerH = 0; ComPtr<ID2D1Effect> blur;   // offscreen scene for blur effects
    std::vector<D2D1_RECT_F> cardLog; bool logCards = false;   // cards drawn in this frame (glass reveal)
    ComPtr<IDCompositionVisual> pulseV; ComPtr<IDCompositionSurface> pulseS; ComPtr<IDCompositionEffectGroup> pulseE; bool pulseOn = false; int pulseSize = 0; COLORREF pulseColor = 0; float pulseX = -1, pulseY = -1;
    LARGE_INTEGER drawStart{}; float lastFrameMs = 0;
    ComPtr<ID2D1GradientStopCollection> haloStops; COLORREF haloColor = 0; float haloAlpha = -1;
    ComPtr<IDCompositionDevice> dcomp; ComPtr<IDCompositionTarget> ct; ComPtr<IDCompositionVisual> visual;
    ComPtr<IDWriteFactory> dw; ComPtr<ID2D1SolidColorBrush> brush;
    struct Fmt { LOGFONTW lf{}; ComPtr<IDWriteTextFormat> fmt; ComPtr<IDWriteInlineObject> ellipsis; };   // the trim sign is sized by its own font
    std::unordered_map<HFONT, Fmt> formats;
    struct Bmp { int w = 0, h = 0; ComPtr<ID2D1Bitmap> bmp; };
    std::unordered_map<const void*, Bmp> bitmaps;    // HBITMAP / HICON / pixel buffer -> device bitmap
    std::unordered_map<uint64_t, ComPtr<ID2D1LinearGradientBrush>> gradients;
    UINT w = 0, h = 0; bool warp = false, drawing = false;
    uint64_t frames = 0, deviceResets = 0;

    // Device-independent resources: created once.
    bool CreateFactories() {
        if (f) return true;
        D2D1_FACTORY_OPTIONS fo{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, reinterpret_cast<void**>(f.GetAddressOf())))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) return false;
        return SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
    }
    // Device-dependent resources (recreated after a GPU reset). Hardware first, WARP (software) as a fallback.
    bool CreateDevice() {
        const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        warp = false;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 4, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (FAILED(hr)) { warp = true; hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 4, D3D11_SDK_VERSION, &d3d, nullptr, nullptr); }
        if (FAILED(hr) || FAILED(d3d.As(&dxgi)) || FAILED(f->CreateDevice(dxgi.Get(), &dev)) ||
            FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp)))) return false;
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
        return true;
    }
    // Can a D2D surface be used on this PC at all? (decides WS_EX_NOREDIRECTIONBITMAP before the window exists)
    bool Probe() { return CreateFactories() && (d3d || CreateDevice()); }
    bool Attach(HWND h) {
        hwnd = h;
        if (!Probe()) return false;
        RECT r; GetClientRect(h, &r); w = std::max<LONG>(1, r.right); this->h = std::max<LONG>(1, r.bottom);
        DXGI_SWAP_CHAIN_DESC1 d{}; d.Width = w; d.Height = this->h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(fac->CreateSwapChainForComposition(d3d.Get(), &d, nullptr, &sc))) return false;
        if (FAILED(dcomp->CreateTargetForHwnd(h, TRUE, &ct)) || FAILED(dcomp->CreateVisual(&visual))) return false;
        visual->SetContent(sc.Get()); ct->SetRoot(visual.Get()); dcomp->Commit();
        return BindTarget();
    }
    bool BindTarget() {
        ComPtr<IDXGISurface> surf;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
        const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        return SUCCEEDED(dc->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target));
    }
    void Resize() {
        if (!sc) return;
        RECT r; GetClientRect(hwnd, &r);
        const UINT nw = std::max<LONG>(1, r.right), nh = std::max<LONG>(1, r.bottom);
        if (nw == w && nh == h && target) return;
        w = nw; h = nh; dc->SetTarget(nullptr); target.Reset();
        sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        BindTarget();
    }
    void Reset() {   // GPU reset / driver update / remote session change: drop every device resource and rebuild
        ++deviceResets;
        readback.Reset(); bmpW = bmpH = 0; haloStops.Reset(); haloAlpha = -1; layer.Reset(); layerW = layerH = 0; blur.Reset(); pulseV.Reset(); pulseS.Reset(); pulseE.Reset(); pulseOn = false; pulseSize = 0;
        bitmaps.clear(); gradients.clear(); brush.Reset(); target.Reset(); visual.Reset(); ct.Reset(); sc.Reset();
        dcomp.Reset(); dc.Reset(); dev.Reset(); dxgi.Reset(); d3d.Reset();
        if (!hwnd) { if (!CreateDevice()) LogE(L"Graphics device was lost and could not be rebuilt."); return; }   // offscreen surface
        if (CreateDevice() && Attach(hwnd)) LogW(L"Graphics device was reset; the window renderer was rebuilt.");
        else LogE(L"Graphics device was lost and could not be rebuilt.");
    }
    bool Begin() {
        if (!sc && hwnd) Reset();   // an earlier rebuild failed: try again at each paint
        if (!sc) return false;
        if (!target) Resize();
        if (!target) return false;
        dc->SetTarget(target.Get()); dc->SetDpi(96, 96);
        dc->BeginDraw(); dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        drawing = true;
        QueryPerformanceCounter(&drawStart);
        return true;
    }
    void End() {
        if (!drawing) return;
        drawing = false;
        const HRESULT hr = dc->EndDraw();
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        lastFrameMs = static_cast<float>((now.QuadPart - drawStart.QuadPart) * 1000.0 / fq.QuadPart);   // the draw itself, not the wait for the display
        const HRESULT pr = sc->Present(1, 0);
        ++frames;
        if (hr == D2DERR_RECREATE_TARGET || pr == DXGI_ERROR_DEVICE_REMOVED || pr == DXGI_ERROR_DEVICE_RESET) Reset();
    }
    // Offscreen frame for UpdateLayeredWindow: premultiplied BGRA, read back into the caller's top-down DIB.
    bool BeginBitmap(int pw, int ph) {
        if (!Probe()) return false;
        if (!target || !readback || bmpW != static_cast<UINT>(pw) || bmpH != static_cast<UINT>(ph)) {
            dc->SetTarget(nullptr); target.Reset(); readback.Reset();
            const D2D1_PIXEL_FORMAT pf = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            const D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pf, 96, 96);
            const D2D1_BITMAP_PROPERTIES1 rp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pf, 96, 96);
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &tp, &target)) || FAILED(dc->CreateBitmap(D2D1::SizeU(pw, ph), nullptr, 0, &rp, &readback))) { target.Reset(); readback.Reset(); return false; }
            bmpW = pw; bmpH = ph;
        }
        dc->SetTarget(target.Get()); dc->SetDpi(96, 96);
        dc->BeginDraw(); dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        drawing = true;
        return true;
    }
    bool EndBitmap(void* dst, int pw, int ph) {
        drawing = false;
        const HRESULT hr = dc->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) { Reset(); return false; }
        if (FAILED(hr)) return false;
        const D2D1_POINT_2U origin{0, 0}; const D2D1_RECT_U src{0, 0, static_cast<UINT32>(pw), static_cast<UINT32>(ph)};
        D2D1_MAPPED_RECT m{};
        if (FAILED(readback->CopyFromBitmap(&origin, target.Get(), &src)) || FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
        for (int y = 0; y < ph; ++y) memcpy(static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * pw * 4, m.bits + static_cast<size_t>(y) * m.pitch, static_cast<size_t>(pw) * 4);
        readback->Unmap();
        ++frames;
        return true;
    }
    // The scene goes to an offscreen bitmap, so a blur can be applied to it afterwards.
    bool BeginLayer() {
        if (!layer || layerW != w || layerH != h) {
            layer.Reset();
            const D2D1_BITMAP_PROPERTIES1 p = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &p, &layer))) return false;
            layerW = w; layerH = h;
        }
        dc->SetTarget(layer.Get()); dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        return true;
    }
    void EndLayer() { dc->SetTarget(target.Get()); }
    bool Blur(float stdDev) {
        if (!blur && FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return false;
        blur->SetInput(0, layer.Get()); blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, stdDev); blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        return true;
    }
    void DrawBlurredLayer(float stdDev) { if (Blur(stdDev)) dc->DrawImage(blur.Get()); }
    // Glass reveal: each logged card goes from blurred to sharp, one after the other; the rest of the page fades in.
    void ComposeReveal(float ms, float stdDev) {
        const D2D1_RECT_F all = D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h));
        std::vector<ComPtr<ID2D1RectangleGeometry>> holes(cardLog.size()); std::vector<ID2D1Geometry*> gs;
        ComPtr<ID2D1RectangleGeometry> big; f->CreateRectangleGeometry(all, &big); gs.push_back(big.Get());
        for (size_t i = 0; i < cardLog.size(); ++i) { f->CreateRectangleGeometry(cardLog[i], &holes[i]); gs.push_back(holes[i].Get()); }
        ComPtr<ID2D1GeometryGroup> rest; f->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, gs.data(), static_cast<UINT32>(gs.size()), &rest);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), rest.Get()), nullptr);
        dc->DrawBitmap(layer.Get(), nullptr, std::clamp(ms / 200.f, 0.f, 1.f));
        dc->PopLayer();
        const bool haveBlur = Blur(stdDev);
        for (size_t i = 0; i < cardLog.size(); ++i) {
            const float t = std::clamp((ms - std::min<size_t>(i, 12) * 30.f) / 200.f, 0.f, 1.f);
            if (t <= 0.f) continue;
            dc->PushAxisAlignedClip(cardLog[i], D2D1_ANTIALIAS_MODE_ALIASED);
            if (haveBlur && t < 1.f) { dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), std::min(1.f, t * 2.f) * (1.f - t)), nullptr); dc->DrawImage(blur.Get()); dc->PopLayer(); }
            dc->DrawBitmap(layer.Get(), nullptr, t);
            dc->PopAxisAlignedClip();
        }
    }
    // A live indicator's pulse: its own compositor visual with a repeating opacity animation, so it costs the app no frames.
    void SetPulse(bool on, float x, float y, int size, COLORREF color) {
        if (!visual || !dcomp || !sc) return;
        if (!on) { if (pulseOn) { visual->RemoveVisual(pulseV.Get()); dcomp->Commit(); pulseOn = false; } return; }
        bool changed = false;
        if (!pulseV || pulseSize != size || pulseColor != color) {
            if (pulseOn) { visual->RemoveVisual(pulseV.Get()); pulseOn = false; }
            pulseV.Reset(); pulseS.Reset(); pulseE.Reset();
            ComPtr<IDCompositionAnimation> a; ComPtr<IDXGISurface> ds; POINT off{0, 0}; ComPtr<ID2D1Bitmap1> bt;
            if (FAILED(dcomp->CreateVisual(&pulseV)) || FAILED(dcomp->CreateSurface(size, size, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &pulseS)) ||
                FAILED(dcomp->CreateEffectGroup(&pulseE)) || FAILED(dcomp->CreateAnimation(&a)) || FAILED(pulseS->BeginDraw(nullptr, IID_PPV_ARGS(&ds), &off))) { LogW(L"Pulse visual could not be created."); pulseV.Reset(); pulseS.Reset(); pulseE.Reset(); return; }
            const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            if (SUCCEEDED(dc->CreateBitmapFromDxgiSurface(ds.Get(), &bp, &bt))) {
                dc->SetTarget(bt.Get()); dc->BeginDraw(); dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(off.x), static_cast<float>(off.y)));
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));
                Halo(size / 2.f, size / 2.f, size / 2.f, color, .85f);
                dc->EndDraw(); dc->SetTransform(D2D1::Matrix3x2F::Identity()); dc->SetTarget(target.Get());
            }
            pulseS->EndDraw();
            a->AddSinusoidal(0, .55f, .45f, .5f, 0.f);   // a 2 s period, 0.1 to 1.0
            pulseE->SetOpacity(a.Get()); pulseV->SetEffect(pulseE.Get()); pulseV->SetContent(pulseS.Get());
            pulseSize = size; pulseColor = color; pulseX = pulseY = -1; changed = true;
        }
        if (pulseX != x || pulseY != y) { pulseV->SetOffsetX(x); pulseV->SetOffsetY(y); pulseX = x; pulseY = y; changed = true; }
        if (!pulseOn) { visual->AddVisual(pulseV.Get(), TRUE, nullptr); pulseOn = true; changed = true; }
        if (changed) dcomp->Commit();
    }
    ID2D1StrokeStyle* RoundCap() {
        if (!roundCap) f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &roundCap);
        return roundCap.Get();
    }
    // Window entrance / exit, run by the compositor (off the UI thread): opacity plus a slight scale about the centre.
    void AnimateWindow(float fromA, float toA, float fromS, float toS, int ms) {
        if (!visual || !dcomp || ms <= 0) return;
        const float sec = ms / 1000.f;
        ComPtr<IDCompositionEffectGroup> g; ComPtr<IDCompositionScaleTransform> st; ComPtr<IDCompositionAnimation> a, sa;
        if (FAILED(dcomp->CreateEffectGroup(&g)) || FAILED(dcomp->CreateScaleTransform(&st)) || FAILED(dcomp->CreateAnimation(&a)) || FAILED(dcomp->CreateAnimation(&sa))) return;
        a->AddCubic(0, fromA, (toA - fromA) / sec, 0, 0); a->End(sec, toA);
        sa->AddCubic(0, fromS, (toS - fromS) / sec, 0, 0); sa->End(sec, toS);
        g->SetOpacity(a.Get()); st->SetCenterX(w / 2.f); st->SetCenterY(h / 2.f); st->SetScaleX(sa.Get()); st->SetScaleY(sa.Get());
        visual->SetEffect(g.Get()); visual->SetTransform(st.Get()); dcomp->Commit();
    }
    void ClearWindowAnimation() { if (visual && dcomp) { visual->SetEffect(nullptr); visual->SetTransform(static_cast<IDCompositionTransform*>(nullptr)); dcomp->Commit(); } }
    bool GlyphsPresent(const wchar_t* family, const std::vector<UINT32>& cps, UINT32& missing) {
        ComPtr<IDWriteFontCollection> fc; UINT32 i = 0; BOOL ok = FALSE; ComPtr<IDWriteFontFamily> fam; ComPtr<IDWriteFont> font;
        if (!dw || FAILED(dw->GetSystemFontCollection(&fc)) || FAILED(fc->FindFamilyName(family, &i, &ok)) || !ok || FAILED(fc->GetFontFamily(i, &fam)) || FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) return false;
        for (UINT32 cp : cps) { BOOL has = FALSE; if (FAILED(font->HasCharacter(cp, &has)) || !has) { missing = cp; return false; } }
        return true;
    }
    // Soft round light that fades to nothing at its edge.
    void Halo(float cx, float cy, float radius, COLORREF c, float a) {
        if (!haloStops || haloColor != c || haloAlpha != a) {
            haloStops.Reset();
            const D2D1_GRADIENT_STOP st[2] = {{0.f, D2DCol(c, a)}, {1.f, D2DCol(c, 0.f)}};
            if (FAILED(dc->CreateGradientStopCollection(st, 2, &haloStops))) return;
            haloColor = c; haloAlpha = a;
        }
        ComPtr<ID2D1RadialGradientBrush> b;
        if (SUCCEEDED(dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), radius, radius), haloStops.Get(), &b)))
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), b.Get());
    }
    ID2D1SolidColorBrush* Brush(COLORREF c, float a = 1.0f) { brush->SetColor(D2DCol(c, a)); return brush.Get(); }
    // Linear gradient between two colours, cached per colour/alpha pair (only the end points change between calls).
    ID2D1LinearGradientBrush* Gradient(COLORREF c0, float a0, COLORREF c1, float a1, D2D1_POINT_2F p0, D2D1_POINT_2F p1) {
        const uint64_t key = (static_cast<uint64_t>(c0) << 32) ^ c1 ^ (static_cast<uint64_t>(a0 * 255) << 56) ^ (static_cast<uint64_t>(a1 * 255) << 24);
        ComPtr<ID2D1LinearGradientBrush>& b = gradients[key];
        if (!b) {
            const D2D1_GRADIENT_STOP st[2] = {{0.f, D2DCol(c0, a0)}, {1.f, D2DCol(c1, a1)}};
            ComPtr<ID2D1GradientStopCollection> sc2;
            if (FAILED(dc->CreateGradientStopCollection(st, 2, &sc2)) ||
                FAILED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), sc2.Get(), &b))) { gradients.erase(key); return nullptr; }
        }
        b->SetStartPoint(p0); b->SetEndPoint(p1);
        return b.Get();
    }
    void Forget(const void* key) { bitmaps.erase(key); }   // the owner freed the handle: Windows may reuse it for other pixels
    IDWriteTextFormat* Format(HFONT font) {
        LOGFONTW lf{};
        if (!font || !GetObjectW(font, sizeof(lf), &lf)) return nullptr;
        auto it = formats.find(font);
        // handles are reused after DeleteObject (DPI change): the cached format must describe the same font
        if (it != formats.end() && it->second.lf.lfHeight == lf.lfHeight && it->second.lf.lfWeight == lf.lfWeight && !wcscmp(it->second.lf.lfFaceName, lf.lfFaceName))
            return it->second.fmt.Get();
        ComPtr<IDWriteTextFormat> fmt;
        const float px = static_cast<float>(lf.lfHeight < 0 ? -lf.lfHeight : std::max<LONG>(1, lf.lfHeight * 4 / 5));   // GDI: negative = em height
        if (FAILED(dw->CreateTextFormat(lf.lfFaceName, nullptr, static_cast<DWRITE_FONT_WEIGHT>(lf.lfWeight ? lf.lfWeight : 400),
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"en-us", &fmt))) return nullptr;
        formats[font] = Fmt{lf, fmt};
        return fmt.Get();
    }
    // Lays text out the way DrawTextW does for the flags AnkhCore uses.
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& s, HFONT font, float width, float height, UINT fmt) {
        ComPtr<IDWriteTextLayout> l;
        IDWriteTextFormat* tf = Format(font);
        if (!tf || FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), tf, std::max(0.0f, width), std::max(0.0f, height), &l))) return nullptr;
        l->SetTextAlignment((fmt & DT_CENTER) ? DWRITE_TEXT_ALIGNMENT_CENTER : (fmt & DT_RIGHT) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        const bool single = (fmt & DT_SINGLELINE) != 0;
        l->SetParagraphAlignment(single && (fmt & DT_VCENTER) ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : single && (fmt & DT_BOTTOM) ? DWRITE_PARAGRAPH_ALIGNMENT_FAR : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        l->SetWordWrapping((fmt & DT_WORDBREAK) && !single ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (fmt & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS)) {
            Fmt& ff = formats[font];
            if (!ff.ellipsis) dw->CreateEllipsisTrimmingSign(tf, &ff.ellipsis);
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            l->SetTrimming(&t, ff.ellipsis.Get());
        }
        return l;
    }
    // Device bitmap for premultiplied BGRA pixels, keyed by their owner (`copy`: refresh the pixels every call).
    ID2D1Bitmap* PixelsBitmap(const void* key, const uint32_t* px, int pw, int ph, bool copy) {
        Bmp& b = bitmaps[key];
        if (!b.bmp || b.w != pw || b.h != ph) {
            b = Bmp{};
            const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(dc->CreateBitmap(D2D1::SizeU(pw, ph), px, pw * 4, &bp, &b.bmp))) { bitmaps.erase(key); return nullptr; }
            b.w = pw; b.h = ph;
        } else if (copy) b.bmp->CopyFromMemory(nullptr, px, pw * 4);
        return b.bmp.Get();
    }
    // 32-bit premultiplied DIB section whose rows are stored top first: every DIB the UI kit hands to DrawImage is created
    // top-down (ui.h DecodeImage and MakeCanvas, biHeight < 0). The row order cannot be read back from the handle: GetObject
    // reports a positive dsBmih.biHeight for every DIB section, top-down or not (FT-1 / B01, probe docs/preview2.1/evidence), so
    // the earlier "positive biHeight means bottom-up" test flipped every picture (the wallpaper previews; B16).
    ID2D1Bitmap* DibBitmap(HBITMAP hb) {
        auto it = bitmaps.find(hb);
        if (it != bitmaps.end() && it->second.bmp) return it->second.bmp.Get();
        DIBSECTION ds{};
        if (!GetObjectW(hb, sizeof(ds), &ds) || !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32) return nullptr;
        std::vector<uint32_t> px(static_cast<size_t>(ds.dsBm.bmWidth) * ds.dsBm.bmHeight);
        const auto* src = static_cast<const uint8_t*>(ds.dsBm.bmBits);
        for (int y = 0; y < ds.dsBm.bmHeight; ++y)
            memcpy(&px[static_cast<size_t>(y) * ds.dsBm.bmWidth], src + static_cast<size_t>(y) * ds.dsBm.bmWidthBytes, ds.dsBm.bmWidth * 4);
        return PixelsBitmap(hb, px.data(), ds.dsBm.bmWidth, ds.dsBm.bmHeight, false);
    }
    ID2D1Bitmap* IconBitmap(HICON icon) {
        auto it = bitmaps.find(icon);
        if (it != bitmaps.end() && it->second.bmp) return it->second.bmp.Get();
        ComPtr<IWICImagingFactory> wic; ComPtr<IWICBitmap> wb; ComPtr<IWICFormatConverter> cv;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(wic->CreateBitmapFromHICON(icon, &wb)) || FAILED(wic->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
        UINT iw = 0, ih = 0; cv->GetSize(&iw, &ih);
        std::vector<uint32_t> px(static_cast<size_t>(iw) * ih);
        if (FAILED(cv->CopyPixels(nullptr, iw * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) return nullptr;
        return PixelsBitmap(icon, px.data(), static_cast<int>(iw), static_cast<int>(ih), false);
    }
};

// The drawing handle. Implicit from an HDC (GDI) or a D2DSurface* (Direct2D).
struct Dc {
    HDC hdc = nullptr; D2DSurface* d = nullptr;
    Dc() = default;
    Dc(HDC h) : hdc(h) {}
    Dc(D2DSurface* s) : d(s) {}
    bool d2d() const { return d && d->drawing; }
};

// ---- clipping (stack): PushClip / PushRoundClip return a token for PopClip
inline int PushClip(Dc dc, const RECT& r) {
    if (dc.d2d()) { dc.d->dc->PushAxisAlignedClip(D2DRect(r), D2D1_ANTIALIAS_MODE_ALIASED); return 1; }
    const int s = SaveDC(dc.hdc); IntersectClipRect(dc.hdc, r.left, r.top, r.right, r.bottom); return s;
}
inline int PushRoundClip(Dc dc, const RECT& r, int rad) {
    if (dc.d2d()) {
        ComPtr<ID2D1RoundedRectangleGeometry> g;
        const float rr = std::min(rad / 2.0f, std::min(r.right - r.left, r.bottom - r.top) / 2.0f);
        dc.d->f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2DRect(r), rr, rr), &g);
        dc.d->dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g.Get()), nullptr);
        return 2;
    }
    const int s = SaveDC(dc.hdc);
    HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    ExtSelectClipRgn(dc.hdc, rg, RGN_AND); DeleteObject(rg);
    return s;
}
inline void PopClip(Dc dc, int token) {
    if (dc.d2d()) { if (token == 2) dc.d->dc->PopLayer(); else dc.d->dc->PopAxisAlignedClip(); return; }
    RestoreDC(dc.hdc, token);
}
// Part of a premultiplied image (screen-aligned wallpaper glass); stretched to the destination.
inline void DrawPixelsPart(Dc dc, const void* key, const uint32_t* px, int pw, int ph, const D2D1_RECT_F& dst, const D2D1_RECT_F& src, float opacity, bool changed = false) {
    if (!dc.d2d()) return;
    if (ID2D1Bitmap* b = dc.d->PixelsBitmap(key, px, pw, ph, changed)) dc.d->dc->DrawBitmap(b, dst, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
}
inline void DrawIconAt(Dc dc, HICON icon, int x, int y, int size, float opacity = 1.0f) {
    if (!icon) return;
    if (dc.d2d()) {
        if (ID2D1Bitmap* b = dc.d->IconBitmap(icon))
            dc.d->dc->DrawBitmap(b, D2D1::RectF(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + size), static_cast<float>(y + size)), opacity,
                                  D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    DrawIconEx(dc.hdc, x, y, icon, size, size, 0, nullptr, DI_NORMAL);
}
// Premultiplied BGRA pixels whose content changes between frames (the animated logo canvas).
inline void DrawPixels(Dc dc, const void* key, HBITMAP dib, const uint32_t* px, int pw, int ph, int x, int y, bool changed = true) {
    if (dc.d2d()) {
        if (ID2D1Bitmap* b = dc.d->PixelsBitmap(key, px, pw, ph, changed))
            dc.d->dc->DrawBitmap(b, D2D1::RectF(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + pw), static_cast<float>(y + ph)));
        return;
    }
    HDC mem = CreateCompatibleDC(dc.hdc); HGDIOBJ old = SelectObject(mem, dib);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    GdiAlphaBlend(dc.hdc, x, y, pw, ph, mem, 0, 0, pw, ph, bf);
    SelectObject(mem, old); DeleteDC(mem);
}

}  // namespace ak
