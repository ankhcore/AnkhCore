// AnkhCore 2.5 — Desktop Sidebar: sections, information density, display styles, profiles and the
// full-height layout (pure logic; unit-tested in tests/test_base.cpp). Sizes are in DIPs; the window
// scales them by its monitor's DPI and the chosen text size. The widget (app.cpp) only draws what
// this layout lists: values come from the shared engine frame.
#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ak {

enum SbSec { SecCpu, SecGpu, SecMem, SecStorage, SecNet, SecFans, SbSecCount };
enum SbDensity { SbMinimal = 0, SbStandard = 1, SbDetailed = 2, SbCustom = 3 };   // Custom: a level per section
enum SbStyle { SbStyleBar = 0, SbStyleGauge = 1, SbStyleValue = 2, SbStyleGraph = 3 };   // indicators, gauges, numeric values, graphs
enum SbRowId {
    RCpuLoad, RCpuTemp, RCpuInfo, RCpuGraph, RCpuCores, RCpuTopo,
    RGpuLoad, RGpuTemp, RGpuInfo, RGpuVram, RGpuGraph,
    RMemLoad, RMemUsed, RMemAvail, RMemCommit, RMemGraph,
    RDrive, RDiskIo, RDiskTemp,
    RNetHead, RNetSpeed, RNetTotals, RNetLink, RNetGraph,
    RFanTop, RFan, RFanMode,
    RGpuEach,   // one line per graphics adapter (hybrid laptops): its own utilization
    RNetDown, RNetUp,   // 2.9.6: download and upload as separate metrics, so the Network section follows the section style (gauges, values, graphs)
};
// How a row is drawn. A metric (SbBar by default) becomes a gauge, a large value or a sparkline with the section's style.
enum SbRowKind { SbBar, SbText, SbGraph, SbCores, SbGauge, SbValue, SbSpark };

inline const wchar_t* SbSecKey(SbSec s) { static const wchar_t* k[SbSecCount] = {L"cpu", L"gpu", L"mem", L"storage", L"net", L"fans"}; return k[s]; }
inline const wchar_t* SbSecTitle(SbSec s) { static const wchar_t* k[SbSecCount] = {L"CPU", L"GRAPHICS", L"MEMORY", L"STORAGE", L"NETWORK", L"FANS"}; return k[s]; }
inline const wchar_t* SbSecName(SbSec s) { static const wchar_t* k[SbSecCount] = {L"CPU", L"Graphics", L"Memory", L"Storage", L"Network", L"Fans"}; return k[s]; }
inline SbRowKind SbKindOf(SbRowId id) {
    switch (id) {
    case RCpuLoad: case RCpuTemp: case RGpuLoad: case RGpuTemp: case RMemLoad: case RDrive: case RFanTop: case RFan: case RNetDown: case RNetUp: return SbBar;
    case RCpuGraph: case RGpuGraph: case RMemGraph: case RNetGraph: return SbGraph;
    case RCpuCores: return SbCores;
    default: return SbText;
    }
}
// Metrics with a history series behind them (the others cannot be drawn as a sparkline).
inline bool SbHasHistory(SbRowId id) { return id == RCpuLoad || id == RCpuTemp || id == RGpuLoad || id == RGpuTemp || id == RMemLoad || id == RFanTop || id == RNetDown || id == RNetUp; }

// ---- which sections are shown, and in what order ("cpu,gpu,-mem,..."; '-' = hidden)
struct SbSecItem { SbSec sec; bool on; };
inline std::vector<std::wstring> SbSplit(const std::wstring& text, wchar_t sep) {
    std::vector<std::wstring> out; size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find(sep, i); if (j == std::wstring::npos) j = text.size();
        std::wstring t = text.substr(i, j - i); i = j + 1;
        t.erase(std::remove(t.begin(), t.end(), L' '), t.end());
        if (!t.empty()) out.push_back(t);
    }
    return out;
}
inline int SbSecFromKey(const std::wstring& k) { for (int s = 0; s < SbSecCount; ++s) if (k == SbSecKey(static_cast<SbSec>(s))) return s; return -1; }
inline std::vector<SbSecItem> SbParseSections(const std::wstring& text) {
    std::vector<SbSecItem> out; bool seen[SbSecCount] = {};
    for (std::wstring t : SbSplit(text, L',')) {
        const bool off = t[0] == L'-'; if (off) t.erase(0, 1);
        const int s = SbSecFromKey(t);
        if (s >= 0 && !seen[s]) { seen[s] = true; out.push_back({static_cast<SbSec>(s), !off}); }
    }
    for (int s = 0; s < SbSecCount; ++s) if (!seen[s]) out.push_back({static_cast<SbSec>(s), true});   // a section this text does not name is shown, last
    return out;
}
inline std::wstring SbFormatSections(const std::vector<SbSecItem>& v) {
    std::wstring o;
    for (const auto& it : v) { if (!o.empty()) o += L','; if (!it.on) o += L'-'; o += SbSecKey(it.sec); }
    return o;
}
inline void SbShowSection(std::vector<SbSecItem>& v, SbSec s, bool on) { for (auto& it : v) if (it.sec == s) it.on = on; }
inline void SbMoveSection(std::vector<SbSecItem>& v, SbSec s, int dir) {   // dir -1 up, +1 down; the ends stay put
    for (size_t i = 0; i < v.size(); ++i) if (v[i].sec == s) {
        const size_t j = dir < 0 ? i - 1 : i + 1;
        if ((dir < 0 && i == 0) || j >= v.size()) return;
        std::swap(v[i], v[j]); return;
    }
}

// ---- one small number per section ("cpu:2,gpu:1"): the custom detail levels and the display styles
struct SbPerSec { int v[SbSecCount]; };
inline SbPerSec SbParsePerSec(const std::wstring& text, int def, int lo, int hi) {
    SbPerSec p; for (int& x : p.v) x = def;
    for (const std::wstring& t : SbSplit(text, L',')) {
        const size_t c = t.find(L':'); if (c == std::wstring::npos || c + 2 != t.size()) continue;   // one digit only
        const int s = SbSecFromKey(t.substr(0, c)), n = t[c + 1] - L'0';
        if (s >= 0 && n >= lo && n <= hi) p.v[s] = n;
    }
    return p;
}
inline std::wstring SbFormatPerSec(const SbPerSec& p) {
    std::wstring o;
    for (int s = 0; s < SbSecCount; ++s) { if (s) o += L','; o += SbSecKey(static_cast<SbSec>(s)); o += L':'; o += static_cast<wchar_t>(L'0' + std::clamp(p.v[s], 0, 9)); }
    return o;
}

// ---- layout
struct SbLook {
    int density = SbStandard;                                  // SbMinimal..SbDetailed, or SbCustom
    SbPerSec levels{{SbStandard, SbStandard, SbStandard, SbStandard, SbStandard, SbStandard}};   // used with SbCustom
    SbPerSec styles{{SbStyleBar, SbStyleBar, SbStyleBar, SbStyleBar, SbStyleBar, SbStyleBar}};
    int spacing = 1;                                           // 0 compact, 1 normal, 2 relaxed
};
struct SbCounts { int cores = 0, drives = 1, fans = 0, gpus = 0; };   // gpus: hardware graphics adapters
struct SbRow { SbRowId id; int index; int top, h; SbRowKind kind = SbText; int col = 0, cols = 1; };   // gauges share a line: col of cols
struct SbBox { SbSec sec; int level; int top, h; std::vector<SbRow> rows; };
struct SbLayout {
    std::vector<SbBox> boxes;
    int used = 0;        // DIPs from the first section's top to the last one's bottom
    int reduced = 0;     // sections shown with less detail than asked, to fit the height
    int dropped = 0;     // sections that did not fit at all
};
constexpr int kSbTitleH = 20, kSbBarH = 30, kSbTextH = 18, kSbGraphH = 38, kSbCoreH = 16, kSbGaugeH = 74, kSbValueH = 24, kSbSparkH = 40, kSbMaxList = 6;
constexpr int kSbPad = 12, kSbGap = 8;   // normal spacing
inline int SbPadFor(int spacing) { return spacing <= 0 ? 8 : spacing == 1 ? kSbPad : 16; }
inline int SbGapFor(int spacing) { return spacing <= 0 ? 4 : spacing == 1 ? kSbGap : 14; }

inline int SbCoreRows(int cores) { if (cores <= 0) return 0; const int per = cores > 24 ? (cores + 3) / 4 : 6; return (cores + per - 1) / per; }
inline int SbCoresPerRow(int cores) { return cores > 24 ? (cores + 3) / 4 : 6; }

inline std::vector<SbRow> SbRowsFor(SbSec s, int level, const SbCounts& c, int style = SbStyleBar) {
    std::vector<SbRow> r;
    auto add = [&](SbRowId id, int index = 0) {
        SbRow row{id, index, 0, 0}; row.kind = SbKindOf(id);
        if (row.kind == SbBar) row.kind = style == SbStyleGauge ? SbGauge : style == SbStyleValue ? SbValue : style == SbStyleGraph && SbHasHistory(id) ? SbSpark : SbBar;
        if (row.kind == SbGraph && style == SbStyleGraph && id != RNetGraph) return;   // every metric already has its own graph
        r.push_back(row);
    };
    const int drives = std::clamp(c.drives, 1, kSbMaxList), fans = std::clamp(c.fans, 0, kSbMaxList);
    switch (s) {
    case SecCpu:
        add(RCpuLoad); add(RCpuTemp);
        if (level >= SbStandard) { add(RCpuInfo); add(RCpuGraph); }
        if (level >= SbDetailed) { if (c.cores > 0) add(RCpuCores); add(RCpuTopo); }
        break;
    case SecGpu:
        add(RGpuLoad); add(RGpuTemp);
        if (level >= SbStandard && c.gpus > 1) for (int i = 0; i < std::min(c.gpus, 4); ++i) add(RGpuEach, i);
        if (level >= SbStandard) add(RGpuInfo);
        if (level >= SbDetailed) { add(RGpuVram); add(RGpuGraph); }
        break;
    case SecMem:
        add(RMemLoad);
        if (level >= SbStandard) add(RMemUsed);
        if (level >= SbDetailed) { add(RMemAvail); add(RMemCommit); add(RMemGraph); }
        break;
    case SecStorage:
        if (level == SbMinimal) add(RDrive, 0);
        else { for (int i = 0; i < drives; ++i) add(RDrive, i); add(RDiskIo); }
        if (level >= SbDetailed) add(RDiskTemp);
        break;
    case SecNet: {
        // Indicators keep the original one-line speed. Gauges, numeric values and graphs show download and upload as two metrics (2.9.6).
        const bool split = style == SbStyleGauge || style == SbStyleValue || style == SbStyleGraph;
        if (level != SbMinimal) add(RNetHead);
        if (split) { add(RNetDown); add(RNetUp); } else add(RNetSpeed);
        if (level != SbMinimal) add(RNetTotals);
        if (level >= SbDetailed) { add(RNetLink); if (style != SbStyleGraph) add(RNetGraph); }   // the graph style already draws both directions
        break;
    }
    case SecFans:
        if (level == SbMinimal || fans == 0) add(RFanTop);
        else for (int i = 0; i < fans; ++i) add(RFan, i);
        if (level >= SbDetailed) add(RFanMode);
        break;
    default: break;
    }
    return r;
}
inline int SbKindH(SbRowKind k, const SbCounts& c) {
    switch (k) {
    case SbBar: return kSbBarH; case SbGraph: return kSbGraphH; case SbGauge: return kSbGaugeH; case SbValue: return kSbValueH; case SbSpark: return kSbSparkH;
    case SbCores: return SbCoreRows(c.cores) * kSbCoreH + 4;
    default: return kSbTextH;
    }
}
// Stacks the rows from `y`; gauges sit two to a line. `grow` is added to every section graph. Returns the bottom.
inline int SbStack(std::vector<SbRow>& rows, const SbCounts& c, int y, int grow = 0) {
    for (size_t i = 0; i < rows.size(); ++i) {
        SbRow& r = rows[i];
        r.top = y; r.h = SbKindH(r.kind, c) + (r.kind == SbGraph ? grow : 0); r.col = 0; r.cols = 1;
        if (r.kind == SbGauge && i + 1 < rows.size() && rows[i + 1].kind == SbGauge) {
            SbRow& n = rows[i + 1];
            r.cols = n.cols = 2; n.col = 1; n.top = y; n.h = r.h; ++i;
        }
        y += r.h;
    }
    return y;
}
inline int SbBoxH(SbSec s, int level, const SbCounts& c, int style = SbStyleBar, int spacing = 1) {
    std::vector<SbRow> rows = SbRowsFor(s, level, c, style);
    return SbStack(rows, c, kSbTitleH) + SbPadFor(spacing);
}

// Fills `avail` DIPs from the top. Too tall: detail is taken away from the bottom section upwards, one
// level at a time, and only then are whole sections dropped (last first). Too short: the spare height
// goes to the graphs first, then to the gaps, so the Sidebar uses the screen instead of leaving a void.
inline SbLayout SbLayoutFor(const SbLook& lookIn, const std::vector<SbSecItem>& items, const SbCounts& c, int avail) {
    SbLook look = lookIn; look.density = std::clamp(look.density, 0, 3); look.spacing = std::clamp(look.spacing, 0, 2);
    std::vector<SbSec> secs; for (const auto& it : items) if (it.on) secs.push_back(it.sec);
    auto style = [&](SbSec s) { return std::clamp(look.styles.v[s], 0, 3); };
    std::vector<int> asked, level;
    for (SbSec s : secs) asked.push_back(look.density == SbCustom ? std::clamp(look.levels.v[s], 0, 2) : look.density);
    level = asked;
    const int gap0 = SbGapFor(look.spacing);
    auto total = [&] { int t = 0; for (size_t i = 0; i < secs.size(); ++i) t += SbBoxH(secs[i], level[i], c, style(secs[i]), look.spacing) + (i ? gap0 : 0); return t; };
    SbLayout L;
    for (int target = SbStandard; target >= 0 && total() > avail; --target)
        for (size_t i = secs.size(); i-- > 0 && total() > avail;) if (level[i] > target) level[i] = target;
    while (!secs.empty() && total() > avail) { secs.pop_back(); level.pop_back(); asked.pop_back(); ++L.dropped; }
    for (size_t i = 0; i < level.size(); ++i) if (level[i] < asked[i]) ++L.reduced;
    int graphs = 0;
    for (size_t i = 0; i < secs.size(); ++i) for (const SbRow& r : SbRowsFor(secs[i], level[i], c, style(secs[i]))) if (r.kind == SbGraph) ++graphs;
    int spare = std::max(0, avail - total());
    const int grow = graphs ? std::min(40, spare / graphs) : 0; spare -= grow * graphs;
    const int gap = gap0 + (secs.size() > 1 ? std::min(16, spare / static_cast<int>(secs.size() - 1)) : 0);
    int y = 0;
    for (size_t i = 0; i < secs.size(); ++i) {
        SbBox b; b.sec = secs[i]; b.level = level[i]; b.top = y; b.rows = SbRowsFor(secs[i], level[i], c, style(secs[i]));
        b.h = SbStack(b.rows, c, y + kSbTitleH, grow) + SbPadFor(look.spacing) - y; y += b.h;
        L.used = y;
        if (i + 1 < secs.size()) y += gap;
        L.boxes.push_back(std::move(b));
    }
    return L;
}
inline SbLayout SbLayoutFor(int density, const std::vector<SbSecItem>& items, const SbCounts& c, int avail) {
    SbLook look; look.density = std::clamp(density, 0, 2);
    return SbLayoutFor(look, items, c, avail);
}

// ---- placement along the edge, auto-hide and dragging (physical pixels; rects are left, top, right, bottom)
struct SbRect { int l, t, r, b; };
// The top of an h-tall Sidebar inside a work area: posPct 0 = top, 50 = middle, 100 = bottom.
inline int SbTopFor(int workTop, int workBottom, int h, int posPct) {
    const int travel = std::max(0, (workBottom - workTop) - h);
    return workTop + travel * std::clamp(posPct, 0, 100) / 100;
}
// Layers (sidebar.layer). Desktop mode (D35) is the default: the Sidebar is part of the desktop, under every
// window and under the desktop icons, takes no screen space and no input.
enum SbLayer { SbLayerTop = 0, SbLayerNormal = 1, SbLayerBottom = 2, SbLayerDesktop = 3 };
// One-time move of an existing setup to Desktop mode. Only the layer changes; the previous layer is kept so the
// old placement can be restored, and every other saved setting (reserve, mouse mode, auto-hide, lock) stays as
// it was: Desktop mode simply does not use them.
struct SbLayerMove { bool write = false; int layer = SbLayerDesktop; int prevLayer = -1; };
inline SbLayerMove SbDesktopMigration(bool alreadyDone, bool hasSidebarSettings, int storedLayer) {
    SbLayerMove m;
    if (alreadyDone || !hasSidebarSettings) return m;          // done before, or a new setup that starts on the default
    m.write = true; m.prevLayer = storedLayer >= SbLayerTop && storedLayer <= SbLayerBottom ? storedLayer : SbLayerTop;
    return m;
}
// What Desktop mode overrides while it is on (the stored values are untouched).
struct SbPlacement { bool reserve, clickThrough, autoHide, locked; };
inline SbPlacement SbEffectivePlacement(int layer, SbPlacement stored) {
    if (layer == SbLayerDesktop) return SbPlacement{false, true, false, true};
    return stored;
}
// Where Windows puts a wallpaper of iw x ih on a screen of sw x sh (pixels, relative to the screen's corner).
// Used to blend a desktop-hosted Sidebar over exactly what is behind it. Tile and Span are treated as Fill.
enum WallFit { WallFill = 0, WallFitIn = 1, WallStretch = 2, WallCenter = 3 };
inline WallFit WallFitFromStyle(int wallpaperStyle, bool tile) {   // HKCU\Control Panel\Desktop: WallpaperStyle, TileWallpaper
    if (tile) return WallFill;
    return wallpaperStyle == 6 ? WallFitIn : wallpaperStyle == 2 ? WallStretch : wallpaperStyle == 0 ? WallCenter : WallFill;   // 10 Fill, 22 Span
}
struct WallBox { double l = 0, t = 0, r = 0, b = 0; };
inline WallBox WallPlacement(WallFit fit, int iw, int ih, int sw, int sh) {
    if (iw <= 0 || ih <= 0 || sw <= 0 || sh <= 0) return WallBox{};
    if (fit == WallStretch) return WallBox{0, 0, static_cast<double>(sw), static_cast<double>(sh)};
    const double kx = static_cast<double>(sw) / iw, ky = static_cast<double>(sh) / ih;
    const double k = fit == WallCenter ? 1.0 : fit == WallFitIn ? (kx < ky ? kx : ky) : (kx > ky ? kx : ky);
    const double w = iw * k, h = ih * k;
    return WallBox{(sw - w) / 2, (sh - h) / 2, (sw + w) / 2, (sh + h) / 2};
}
// How far the Sidebar is pushed off its edge: frac 0 = fully shown, 1 = hidden except a `peek` strip.
inline int SbSlideOffset(int width, int peek, double frac) {
    const double f = std::clamp(frac, 0.0, 1.0), e = 1 - (1 - f) * (1 - f) * (1 - f);   // ease out
    return static_cast<int>(std::max(0, width - peek) * e + 0.5);
}
// Auto-hide: the Sidebar leaves after the pointer has been away for `delayMs`, and returns at once when
// the pointer touches its strip at the screen edge. `hold` (a drag, a menu) keeps it where it is.
struct SbHide { bool hidden = false; unsigned long long awaySince = 0; };
inline bool SbHideNext(SbHide& s, bool inside, bool atEdge, bool hold, unsigned long long nowMs, int delayMs) {
    const bool was = s.hidden;
    if (hold) { s.awaySince = 0; return false; }
    if (s.hidden) { if (atEdge || inside) { s.hidden = false; s.awaySince = 0; } }
    else if (inside) s.awaySince = 0;
    else if (!s.awaySince) s.awaySince = nowMs ? nowMs : 1;
    else if (nowMs - s.awaySince >= static_cast<unsigned long long>(std::max(0, delayMs))) { s.hidden = true; s.awaySince = 0; }
    return s.hidden != was;
}
// Where a dragged Sidebar lands: the screen under the pointer, the nearer side of it, and how far down.
struct SbDrop { int monitor = 0, edge = 1, posPct = 0; };
inline SbDrop SbDropTarget(const std::vector<SbRect>& works, int px, int py, int h, int grabDy) {
    SbDrop d; if (works.empty()) return d;
    long long best = -1;
    for (size_t i = 0; i < works.size(); ++i) {   // the work area containing the pointer, else the nearest one
        const SbRect& w = works[i];
        const long long dx = px < w.l ? w.l - px : px >= w.r ? px - w.r + 1 : 0, dy = py < w.t ? w.t - py : py >= w.b ? py - w.b + 1 : 0, dist = dx * dx + dy * dy;
        if (best < 0 || dist < best) { best = dist; d.monitor = static_cast<int>(i); }
    }
    const SbRect& w = works[static_cast<size_t>(d.monitor)];
    d.edge = px >= (w.l + w.r) / 2 ? 1 : 0;
    const int travel = (w.b - w.t) - h, top = std::clamp(py - grabDy, w.t, w.t + std::max(0, travel));
    d.posPct = travel > 0 ? std::clamp(static_cast<int>(((top - w.t) * 100LL + travel / 2) / travel), 0, 100) : 0;
    return d;
}

// ---- colours (0xRRGGBB; -1 = the theme's own colour)
inline int SbColorStep(int rgb, int channel, int delta) {   // channel 0 red, 1 green, 2 blue
    if (rgb < 0 || channel < 0 || channel > 2) return rgb;
    const int shift = (2 - channel) * 8, v = std::clamp(((rgb >> shift) & 0xFF) + delta, 0, 255);
    return (rgb & ~(0xFF << shift)) | (v << shift);
}
inline std::wstring SbColorHex(int rgb) {
    if (rgb < 0) return L"theme";
    static const wchar_t* d = L"0123456789ABCDEF"; std::wstring o = L"#";
    for (int s = 20; s >= 0; s -= 4) o += d[(rgb >> s) & 15];
    return o;
}

// ---- profiles: the look of the Sidebar as one line of text ("key=value|key=value"). Where the Sidebar
// sits and how the mouse treats it (edge, screen, layer, click-through, reserved edge) is not part of a look.
struct SbProfileKey { const wchar_t* key; bool text; int lo, hi; };   // text: a string setting; otherwise an integer in lo..hi
inline const std::vector<SbProfileKey>& SbProfileKeys() {
    static const std::vector<SbProfileKey> k = {
        {L"sidebar.layout", false, 0, 3}, {L"sidebar.levels", true, 0, 0}, {L"sidebar.styles", true, 0, 0}, {L"sidebar.sections", true, 0, 0},
        {L"sidebar.spacing", false, 0, 2}, {L"sidebar.fontScale", false, 80, 150}, {L"sidebar.width", false, 140, 420}, {L"sidebar.heightPct", false, 40, 100},
        {L"sidebar.valign", false, 0, 2}, {L"sidebar.opacity", false, 10, 100}, {L"sidebar.background", false, 0, 2}, {L"sidebar.tint", false, 0, 100},
        {L"sidebar.accent", false, -1, 0xFFFFFF}, {L"sidebar.accent2", false, -1, 0xFFFFFF}, {L"sidebar.textColor", false, -1, 0xFFFFFF}, {L"sidebar.tintColor", false, -1, 0xFFFFFF},
        {L"sidebar.glow", false, 0, 2}, {L"sidebar.animate", false, 0, 1}, {L"sidebar.netPeriod", false, 0, 3},
    };
    return k;
}
inline std::wstring SbProfileEncode(const std::vector<std::pair<std::wstring, std::wstring>>& kv) {
    std::wstring o;
    for (const auto& p : kv) { if (!o.empty()) o += L'|'; o += p.first + L'=' + p.second; }
    return o;
}
// Only known keys with valid values come back; anything else in the text is ignored (never applied).
inline std::vector<std::pair<std::wstring, std::wstring>> SbProfileDecode(const std::wstring& text) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find(L'|', i); if (j == std::wstring::npos) j = text.size();
        const std::wstring t = text.substr(i, j - i); i = j + 1;
        const size_t e = t.find(L'='); if (e == std::wstring::npos) continue;
        const std::wstring key = t.substr(0, e), val = t.substr(e + 1);
        for (const SbProfileKey& k : SbProfileKeys()) {
            if (key != k.key) continue;
            bool ok = val.size() <= 120;
            if (ok && k.text) { for (wchar_t ch : val) ok = ok && (ch == L',' || ch == L':' || ch == L'-' || (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')); }
            else if (ok) {
                ok = !val.empty() && val.size() <= 9; bool neg = false; long n = 0;
                for (size_t q = 0; q < val.size() && ok; ++q) { if (q == 0 && val[q] == L'-') { neg = true; ok = val.size() > 1; } else if (val[q] >= L'0' && val[q] <= L'9') n = n * 10 + (val[q] - L'0'); else ok = false; }
                if (neg) n = -n;
                ok = ok && n >= k.lo && n <= k.hi;
            }
            if (ok) { bool had = false; for (auto& p : out) if (p.first == key) { p.second = val; had = true; } if (!had) out.push_back({key, val}); }
        }
    }
    return out;
}
// ---------------------------------------------------------------- FT-D01: is the Desktop-layer Sidebar where it should be?
// Field test 2026-11-15: after icon-theme applies the log showed "Windows offered no desktop layer to join" and the Sidebar
// stayed gone until it was closed and opened again. Nothing watched the window afterwards. The facts below are collected
// once a second by the app (Windows calls only) and decided here, so every case can be tested without a desktop.
struct SbHealthInput {
    bool wantDesktop = false;      // layer = Desktop and the Sidebar is meant to be on
    bool haveWindow = false;       // the window exists (a destroyed one is recreated elsewhere: SbEnsure)
    bool busy = false;             // closing, dragging or sliding: leave it alone
    bool hosted = false;           // currently a child of the desktop host
    bool hostAvailable = false;    // the shell offers a desktop layer right now
    bool parentIsHost = false;     // hosted: its parent is that layer (not a stale one)
    bool visible = false;          // IsWindowVisible
    bool beneathIcons = false;     // hosted: directly beneath the icon view in the z-order
    bool hostVisible = true;       // host desktop window (Progman / WorkerW) is visible
    bool onScreen = true;          // Sidebar rect is non-empty and overlaps its monitor work area
};
inline bool SbRectIntersectsWorkArea(int l, int t, int r, int b, int wl, int wt, int wr, int wb, int minOverlapPx = 16) {
    if (r - l < minOverlapPx || b - t < minOverlapPx) return false;
    const int il = (l > wl) ? l : wl;
    const int it = (t > wt) ? t : wt;
    const int ir = (r < wr) ? r : wr;
    const int ib = (b < wb) ? b : wb;
    return (ir - il >= minOverlapPx) && (ib - it >= minOverlapPx);
}
enum class SbHealth { Healthy, Waiting, Rejoin, Reshow, Restack };
struct SbVerdict { SbHealth what = SbHealth::Healthy; const wchar_t* why = L""; };
inline SbVerdict SbHealthDecide(const SbHealthInput& i) {
    if (!i.wantDesktop || !i.haveWindow || i.busy) return {};
    if (!i.hosted) {   // fallback window (below all others): join the desktop as soon as the shell offers it again
        if (i.hostAvailable) return {SbHealth::Rejoin, L"it is outside the desktop layer and the shell offers one again"};
        return {SbHealth::Waiting, L"the shell offers no desktop layer yet"};
    }
    if (!i.hostAvailable) return {SbHealth::Waiting, L"the desktop layer is being rebuilt"};
    if (!i.parentIsHost) return {SbHealth::Rejoin, L"it is attached to a desktop layer that is no longer the current one"};
    if (!i.hostVisible) return {SbHealth::Rejoin, L"its desktop host window is hidden"};
    if (!i.visible) return {SbHealth::Reshow, L"it was hidden"};
    if (!i.onScreen) return {SbHealth::Restack, L"it was clipped or moved outside the visible desktop"};
    if (!i.beneathIcons) return {SbHealth::Restack, L"it is no longer directly beneath the desktop icons"};
    return {};
}
inline const wchar_t* SbHealthName(SbHealth h) {
    switch (h) { case SbHealth::Waiting: return L"waiting"; case SbHealth::Rejoin: return L"rejoin the desktop"; case SbHealth::Reshow: return L"show again"; case SbHealth::Restack: return L"restack"; default: return L"healthy"; }
}

struct SbBuiltin { const wchar_t* name; const wchar_t* text; };
inline const std::vector<SbBuiltin>& SbBuiltinProfiles() {
    static const std::vector<SbBuiltin> p = {
        {L"Default", L"sidebar.layout=1|sidebar.levels=cpu:1,gpu:1,mem:1,storage:1,net:1,fans:1|sidebar.styles=cpu:0,gpu:0,mem:0,storage:0,net:0,fans:0|sidebar.sections=cpu,gpu,mem,storage,net,fans|sidebar.spacing=1|sidebar.fontScale=100|sidebar.width=210|sidebar.heightPct=100|sidebar.valign=0|sidebar.opacity=92|sidebar.background=0|sidebar.tint=70|sidebar.accent=-1|sidebar.accent2=-1|sidebar.textColor=-1|sidebar.tintColor=-1|sidebar.glow=1|sidebar.animate=0"},
        {L"Compact", L"sidebar.layout=0|sidebar.styles=cpu:2,gpu:2,mem:2,storage:0,net:0,fans:2|sidebar.sections=cpu,gpu,mem,storage,net,fans|sidebar.spacing=0|sidebar.fontScale=100|sidebar.width=170|sidebar.heightPct=100|sidebar.valign=0|sidebar.opacity=92|sidebar.background=0|sidebar.glow=0|sidebar.animate=0"},
        {L"Glass gauges", L"sidebar.layout=1|sidebar.styles=cpu:1,gpu:1,mem:1,storage:0,net:0,fans:1|sidebar.sections=cpu,gpu,mem,storage,net,fans|sidebar.spacing=1|sidebar.fontScale=100|sidebar.width=230|sidebar.heightPct=100|sidebar.opacity=80|sidebar.background=2|sidebar.tint=60|sidebar.glow=2|sidebar.animate=1"},
        {L"Graph wall", L"sidebar.layout=3|sidebar.levels=cpu:2,gpu:1,mem:1,storage:1,net:2,fans:0|sidebar.styles=cpu:3,gpu:3,mem:3,storage:0,net:0,fans:3|sidebar.sections=cpu,gpu,mem,net,storage,fans|sidebar.spacing=1|sidebar.fontScale=100|sidebar.width=260|sidebar.heightPct=100|sidebar.opacity=88|sidebar.background=1|sidebar.tint=70|sidebar.glow=1|sidebar.animate=1"},
    };
    return p;
}

}  // namespace ak
