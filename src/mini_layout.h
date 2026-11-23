// AnkhCore 2.5 — Mini Monitor: settings, layout and placement (pure logic; unit-tested in
// tests/test_base.cpp). Sizes are in DIPs; the window scales them by its monitor's DPI. The widget
// itself (app.cpp) only draws: values come from the shared 1 s snapshot, graphs from MonitorStore.
#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include "monitor_store.h"

namespace ak {

enum MiniMetric : uint32_t {
    MCpuLoad = 1u << 0, MCpuTemp = 1u << 1, MGpuLoad = 1u << 2, MGpuTemp = 1u << 3,
    MRam = 1u << 4, MFan = 1u << 5, MNet = 1u << 6, MClock = 1u << 7,
};
constexpr int kMiniMetricCount = 8;
constexpr uint32_t kMiniAllMetrics = (1u << kMiniMetricCount) - 1;
constexpr uint32_t kMiniDefaultMetrics = MCpuLoad | MCpuTemp | MGpuLoad | MGpuTemp | MRam | MFan;
inline MiniMetric MiniMetricAt(int i) { return static_cast<MiniMetric>(1u << i); }

// History series behind a metric's sparkline (-1: none, e.g. the clock).
inline int MiniSeries(MiniMetric m) {
    switch (m) {
    case MCpuLoad: return SCpuLoad; case MCpuTemp: return SCpuTemp; case MGpuLoad: return SGpuLoad;
    case MGpuTemp: return SGpuTemp; case MRam: return SRam; case MFan: return SFan; case MNet: return SNetDown;
    default: return -1;
    }
}

struct MiniCfg {
    int layout = 1;                          // 0 compact, 1 expanded
    uint32_t metrics = kMiniDefaultMetrics;  // MiniMetric bits, shown in bit order
    bool graphs = true;                      // sparklines (expanded layout only)
    bool clickThrough = false;               // mouse passes through to the desktop
    int layer = 0;                           // 0 always on top, 1 normal
    int opacity = 85;                        // background opacity %, text and graphs stay opaque
    // customization (2.5 M-WID1); 0 / -1 keep the layout's own size and the theme's colours
    int width = 0;                           // DIPs, 0 = the layout's own width; otherwise 150..400
    int rowHeight = 0;                       // DIPs per metric line, 0 = the layout's own; otherwise 16..36
    int scale = 100;                         // text and content size %, 80..200
    int background = -1;                     // -1 follows the shared "wallpaper glass" switch; 0 solid, 1 frosted glass, 2 acrylic blur
    int tint = 70;                           // how much tint lies over the glass %
    int accent = -1, textColor = -1;         // 0xRRGGBB, -1 = the theme's colour
    int glow = 0;                            // 0 off, 1 soft, 2 strong
    std::wstring styles;                     // per-metric display style (0 value, 1 bar, 2 sparkline)
};
enum MiniStyle { MiniStyleValue = 0, MiniStyleBar = 1, MiniStyleSpark = 2 };
inline std::array<int, kMiniMetricCount> MiniParseStyles(const std::wstring& s) {
    std::array<int, kMiniMetricCount> out{};
    size_t i = 0, p = 0;
    while (i < kMiniMetricCount && p < s.size()) {
        const size_t c = s.find(L',', p);
        const std::wstring tok = c == std::wstring::npos ? s.substr(p) : s.substr(p, c - p);
        if (!tok.empty()) out[i] = std::clamp(_wtoi(tok.c_str()), 0, 2);
        ++i;
        if (c == std::wstring::npos) break;
        p = c + 1;
    }
    return out;
}
inline std::wstring MiniFormatStyles(const std::array<int, kMiniMetricCount>& st) {
    std::wstring out;
    for (int i = 0; i < kMiniMetricCount; ++i) {
        if (i) out += L',';
        out += std::to_wstring(std::clamp(st[i], 0, 2));
    }
    return out;
}
inline std::wstring MiniSerializeProfile(const MiniCfg& c) {
    return std::to_wstring(c.layout) + L";" + std::to_wstring(c.metrics) + L";" +
           std::to_wstring(c.graphs ? 1 : 0) + L";" + std::to_wstring(c.opacity) + L";" +
           std::to_wstring(c.background) + L";" + std::to_wstring(c.tint) + L";" +
           std::to_wstring(c.glow) + L";" + MiniFormatStyles(MiniParseStyles(c.styles));
}
inline bool MiniParseProfile(const std::wstring& text, MiniCfg& out) {
    std::vector<std::wstring> parts;
    size_t p = 0;
    while (p <= text.size()) {
        const size_t s = text.find(L';', p);
        parts.push_back(s == std::wstring::npos ? text.substr(p) : text.substr(p, s - p));
        if (s == std::wstring::npos) break;
        p = s + 1;
    }
    if (parts.size() < 7) return false;
    out.layout = std::clamp(_wtoi(parts[0].c_str()), 0, 1);
    out.metrics = static_cast<uint32_t>(_wtoi(parts[1].c_str())) & kMiniAllMetrics;
    out.graphs = _wtoi(parts[2].c_str()) != 0;
    out.opacity = std::clamp(_wtoi(parts[3].c_str()), 10, 100);
    out.background = std::clamp(_wtoi(parts[4].c_str()), -1, 2);
    out.tint = std::clamp(_wtoi(parts[5].c_str()), 0, 100);
    out.glow = std::clamp(_wtoi(parts[6].c_str()), 0, 2);
    if (parts.size() >= 8) out.styles = parts[7];
    return true;
}
struct MiniBuiltin { const wchar_t* name; const wchar_t* text; };
inline const std::vector<MiniBuiltin>& MiniBuiltinProfiles() {
    static const std::vector<MiniBuiltin> p = {
        {L"Balanced",      L"1;63;1;85;-1;70;0;0,0,0,0,0,0,0,0"},
        {L"Compact Strip", L"0;83;0;88;0;70;0;0,0,0,0,0,0,0,0"},
        {L"Gaming HUD",    L"1;47;1;78;2;65;1;1,1,1,1,0,0,0,0"},
        {L"Minimal",       L"0;26;0;82;1;70;0;0,0,0,0,0,0,0,0"},
    };
    return p;
}
inline MiniCfg MiniSanitize(MiniCfg c) {
    c.layout = std::clamp(c.layout, 0, 1);
    c.metrics &= kMiniAllMetrics;
    c.layer = std::clamp(c.layer, 0, 1);
    c.opacity = std::clamp(c.opacity, 10, 100);
    if (c.width != 0) c.width = std::clamp(c.width, 150, 400);
    if (c.rowHeight != 0) c.rowHeight = std::clamp(c.rowHeight, 16, 36);
    c.scale = std::clamp(c.scale, 80, 200);
    c.background = std::clamp(c.background, -1, 2); c.tint = std::clamp(c.tint, 0, 100);
    c.accent = std::clamp(c.accent, -1, 0xFFFFFF); c.textColor = std::clamp(c.textColor, -1, 0xFFFFFF);
    c.glow = std::clamp(c.glow, 0, 2);
    return c;
}

struct MiniRow { MiniMetric metric; int top, height; bool graph; };
struct MiniLayout {
    int width = 0, height = 0;
    int headerH = 0, rowH = 0;   // rowH: one metric line (its graph, when shown, comes below it)
    std::vector<MiniRow> rows;   // empty: no metric selected (a hint row is drawn instead)
    int footerTop = 0, footerH = 0;
};
// Compact: a narrow list, one line per metric, no graphs, no footer.
// Expanded: wider, header with the logo, optional sparkline under each metric that has history,
// and a footer (Open button when interactive, the hotkey hint when click-through). The size never
// depends on the mouse mode, so switching modes does not make the widget jump.
inline MiniLayout MiniLayoutFor(const MiniCfg& cIn) {
    const MiniCfg c = MiniSanitize(cIn);
    const bool ex = c.layout == 1;
    MiniLayout L;
    L.width = c.width ? c.width : (ex ? 232 : 168);
    L.headerH = ex ? 38 : 24;
    const int rowH = c.rowHeight ? c.rowHeight : (ex ? 22 : 19), graphH = 20, pad = ex ? 6 : 5;
    L.rowH = rowH;
    int y = L.headerH + pad;
    for (int i = 0; i < kMiniMetricCount; ++i) {
        const MiniMetric m = MiniMetricAt(i);
        if (!(c.metrics & m)) continue;
        const bool g = ex && c.graphs && MiniSeries(m) >= 0;
        const int h = rowH + (g ? graphH : 0);
        L.rows.push_back({m, y, h, g});
        y += h + (g ? 4 : 0);
    }
    if (L.rows.empty()) y += rowH;   // "No metrics selected"
    y += pad;
    if (ex) { L.footerTop = y; L.footerH = 30; y += L.footerH + 8; }
    L.height = y;
    return L;
}

// ---- placement (physical pixels; rects are left, top, right, bottom)
struct MiniRect { int l, t, r, b; };
inline long long MiniOverlap(const MiniRect& a, const MiniRect& b) {
    const long long w = std::min(a.r, b.r) - std::max(a.l, b.l), h = std::min(a.b, b.b) - std::max(a.t, b.t);
    return w > 0 && h > 0 ? w * h : 0;
}
// Where a w x h widget goes. The saved spot is used when it overlaps a work area (the widget is then
// pulled fully inside that work area); otherwise (monitor unplugged, resolution changed, nothing saved)
// it goes to the default spot, bottom-right of the first work area (the primary monitor).
inline MiniRect MiniPosition(const std::vector<MiniRect>& works, bool haveSaved, int x, int y, int w, int h, int margin) {
    if (works.empty()) return {x, y, x + w, y + h};
    int best = -1; long long area = 0;
    if (haveSaved)
        for (size_t i = 0; i < works.size(); ++i) {
            const long long a = MiniOverlap({x, y, x + w, y + h}, works[i]);
            if (a > area) { area = a; best = static_cast<int>(i); }
        }
    if (best < 0) {
        const MiniRect& p = works[0];
        const int nx = std::max(p.l, p.r - w - margin), ny = std::max(p.t, p.b - h - margin);
        return {nx, ny, nx + w, ny + h};
    }
    const MiniRect& a = works[best];
    const int nx = std::clamp(x, a.l, std::max(a.l, a.r - w)), ny = std::clamp(y, a.t, std::max(a.t, a.b - h));
    return {nx, ny, nx + w, ny + h};
}

// A corner of a work area for a w x h widget (0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right).
inline MiniRect MiniCorner(const MiniRect& work, int corner, int w, int h, int margin) {
    const int x = (corner & 1) ? std::max(work.l, work.r - w - margin) : std::min(work.l + margin, std::max(work.l, work.r - w));
    const int y = (corner & 2) ? std::max(work.t, work.b - h - margin) : std::min(work.t + margin, std::max(work.t, work.b - h));
    return {x, y, x + w, y + h};
}

}  // namespace ak
