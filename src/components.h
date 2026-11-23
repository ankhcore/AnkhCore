// AnkhCore 2.5 — Blue Acrylic components (redesign §4.2): gauges, graphs, tiles, sliders, state panels.
// They draw on either backend; Direct2D gets the full treatment, GDI (fallback only) a plain equivalent.
#pragma once
#include "ui.h"

namespace ak {

// 270 degree ring with a gradient arc, round caps and a tick every 10%. pct < 0: empty track (no data).
inline void GaugeRing(Dc dc, int cx, int cy, int radius, int thick, double pct, COLORREF color) {
    constexpr float kStart = 135.f * 3.14159265f / 180.f, kSweep = 270.f * 3.14159265f / 180.f;
    if (!dc.d2d()) {
        for (int i = 0; i < 54; ++i) {
            const double a = kStart + kSweep * i / 53.0;
            const bool on = pct >= 0 && i <= pct * 53 / 100.0;
            Line(dc, cx + static_cast<int>(cos(a) * radius), cy + static_cast<int>(sin(a) * radius), cx + static_cast<int>(cos(a) * (radius - thick)), cy + static_cast<int>(sin(a) * (radius - thick)), on ? color : col::track, 2);
        }
        return;
    }
    D2DSurface& s = *dc.d;
    const float r = static_cast<float>(radius) - thick / 2.f, fx = static_cast<float>(cx), fy = static_cast<float>(cy);
    auto arc = [&](float frac, ID2D1Brush* b, float w) {
        if (frac <= 0.001f) return;
        ComPtr<ID2D1PathGeometry> g; ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(s.f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return;
        const float end = kStart + kSweep * frac;
        sink->BeginFigure(D2D1::Point2F(fx + cosf(kStart) * r, fy + sinf(kStart) * r), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(fx + cosf(end) * r, fy + sinf(end) * r), D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, kSweep * frac > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->Close();
        s.dc->DrawGeometry(g.Get(), b, w, s.RoundCap());
    };
    arc(1.f, s.Brush(col::border, .9f), static_cast<float>(thick));
    if (pct > 0) {
        ID2D1Brush* g = s.Gradient(Mix(col::accentDeep, color, .55), 1.f, color, 1.f, D2D1::Point2F(fx - r, fy + r), D2D1::Point2F(fx + r, fy - r));
        arc(static_cast<float>(std::min(pct, 100.0) / 100.0), g ? g : s.Brush(color), static_cast<float>(thick));
    }
    for (int i = 0; i <= 10; ++i) {
        const float a = kStart + kSweep * i / 10.f, r0 = r + thick / 2.f + 2.f, r1 = r0 + 3.f;
        s.dc->DrawLine(D2D1::Point2F(fx + cosf(a) * r0, fy + sinf(a) * r0), D2D1::Point2F(fx + cosf(a) * r1, fy + sinf(a) * r1), s.Brush(col::faint, .55f), 1.f);
    }
}

// Area + line graph. `pts` are heights 0..1, oldest first; NaN = gap (a break, never interpolated).
inline void AreaGraph(Dc dc, const RECT& r, const std::vector<float>& pts, COLORREF color, int lineW, bool area = true) {
    const size_t n = pts.size();
    if (n < 2) return;
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top), x0 = static_cast<float>(r.left), bot = static_cast<float>(r.bottom);
    auto X = [&](size_t i) { return x0 + w * i / static_cast<float>(n - 1); };
    auto Y = [&](float v) { return bot - std::clamp(v, 0.f, 1.f) * (h - 2) - 1; };
    if (!dc.d2d()) {
        for (size_t i = 1; i < n; ++i) if (pts[i - 1] == pts[i - 1] && pts[i] == pts[i]) Line(dc, static_cast<int>(X(i - 1)), static_cast<int>(Y(pts[i - 1])), static_cast<int>(X(i)), static_cast<int>(Y(pts[i])), color, lineW);
        return;
    }
    D2DSurface& s = *dc.d;
    for (size_t i = 0; i < n;) {
        if (pts[i] != pts[i]) { ++i; continue; }
        size_t e = i; while (e + 1 < n && pts[e + 1] == pts[e + 1]) ++e;
        if (e > i) {
            auto build = [&](bool closed, ComPtr<ID2D1PathGeometry>& g) {
                ComPtr<ID2D1GeometrySink> sink;
                if (FAILED(s.f->CreatePathGeometry(&g)) || FAILED(g->Open(&sink))) return false;
                sink->BeginFigure(closed ? D2D1::Point2F(X(i), bot) : D2D1::Point2F(X(i), Y(pts[i])), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                if (closed) sink->AddLine(D2D1::Point2F(X(i), Y(pts[i])));
                for (size_t k = i + 1; k <= e; ++k) sink->AddLine(D2D1::Point2F(X(k), Y(pts[k])));
                if (closed) sink->AddLine(D2D1::Point2F(X(e), bot));
                sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN); sink->Close();
                return true;
            };
            ComPtr<ID2D1PathGeometry> fill, line;
            if (area && build(true, fill)) if (auto* g = s.Gradient(color, .30f, color, 0.f, D2D1::Point2F(x0, bot - h), D2D1::Point2F(x0, bot))) s.dc->FillGeometry(fill.Get(), g);
            if (build(false, line)) {
                s.dc->DrawGeometry(line.Get(), s.Brush(color, .22f), lineW + 3.f, s.RoundCap());
                s.dc->DrawGeometry(line.Get(), s.Brush(color), static_cast<float>(lineW), s.RoundCap());
            }
        }
        i = e + 1;
    }
}

// Empty, unavailable and error states share one look.
inline void StatePanel(Ctx& c, const wchar_t* glyph, const std::wstring& title, const std::wstring& detail, COLORREF color = col::sub) {
    const int h = c.S(116); RECT r{c.x, c.y, c.x + c.w, c.y + h};
    if (c.Visible(r)) {
        Round(c.dc, r, c.S(28), col::panel, col::border);
        Text(c.dc, glyph, RECT{r.left, r.top + c.S(14), r.right, r.top + c.S(48)}, c.u.f.iconBig, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Text(c.dc, title, RECT{r.left, r.top + c.S(50), r.right, r.top + c.S(74)}, c.u.f.btn, col::text, DT_CENTER | DT_SINGLELINE);
        Text(c.dc, detail, RECT{r.left + c.S(24), r.top + c.S(76), r.right - c.S(24), r.bottom - c.S(8)}, c.u.f.small, col::sub, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    c.y += h + c.S(12);
}
// Loading placeholder: a light sweeping over a glass bar.
inline void Shimmer(Dc dc, const RECT& r, int rad, uint64_t nowMs) {
    Round(dc, r, rad, col::track, col::track);
    if (!dc.d2d()) return;
    const float w = static_cast<float>(r.right - r.left), x = r.left + w * ((nowMs % 1400) / 1400.f) - w * .3f;
    const int clip = PushRoundClip(dc, r, rad);
    if (auto* g = dc.d->Gradient(col::accent, 0.f, col::accent, .28f, D2D1::Point2F(x, 0), D2D1::Point2F(x + w * .3f, 0)))
        dc.d->dc->FillRectangle(D2D1::RectF(x, static_cast<float>(r.top), x + w * .3f, static_cast<float>(r.bottom)), g);
    PopClip(dc, clip);
}

}  // namespace ak
