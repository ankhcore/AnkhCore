// AnkhCore — Neutral Windows Glass Design System (Default Theme).
// Centralizes the Windows 11 Mica / Acrylic inspired neutral graphite, charcoal,
// and silver palette, 5-tier glass transparency levels, border highlights, and
// theme registry so future themes can be added without rewriting UI code.
#pragma once
#include <windows.h>
#include <string>

namespace ak {

enum class GlassLevel {
    Background = 0,  // Main window & navigation base (calm dark graphite)
    Surface    = 1,  // Primary content cards & panels (translucent charcoal)
    Elevated   = 2,  // Hover cards, modals, dialogs, elevated overlays
    Interactive= 3,  // Buttons, chips, sliders, active nav pills
    Overlay    = 4,  // Floating tooltips, menus, toasts, Sidebar/Mini overlays
};

struct GlassLayerSpec {
    COLORREF fill;
    float alpha;
    float topEdgeAlpha;
    float bottomEdgeAlpha;
    float shadowAlpha;
};

struct ThemeSpec {
    const wchar_t* id;
    const wchar_t* name;
    bool isClassic;
    COLORREF bg;
    COLORREF side;
    COLORREF panel;
    COLORREF panel2;
    COLORREF border;
    COLORREF borderHi;
    COLORREF accent;
    COLORREF accentDim;
    COLORREF accentDeep;
    COLORREF text;
    COLORREF sub;
    COLORREF faint;
    COLORREF ok;
    COLORREF warn;
    COLORREF err;
    COLORREF track;
    COLORREF overlay;
    int cardRadius;
    int controlRadius;
};

constexpr ThemeSpec NeutralGlassTheme() {
    return {
        L"neutral-glass",
        L"Neutral Windows Glass",
        false,
        RGB(18, 19, 22),     // bg: Dark Graphite
        RGB(22, 23, 27),     // side: Calm Navigation Graphite
        RGB(31, 33, 38),     // panel: Translucent Charcoal Glass Surface
        RGB(45, 48, 55),     // panel2: Elevated Hover Glass
        RGB(64, 68, 77),     // border: Subtle Neutral Border
        RGB(168, 174, 186),  // borderHi: Subtle Silver Highlight
        RGB(216, 220, 228),  // accent: Neutral Silver / Platinum (interactive focus)
        RGB(132, 138, 150),  // accentDim: Muted Silver
        RGB(52, 56, 64),     // accentDeep: Selected Dark Glass
        RGB(242, 244, 248),  // text: Soft White
        RGB(172, 177, 188),  // sub: Neutral Gray
        RGB(112, 117, 128),  // faint: Muted Gray (disabled)
        RGB(74, 200, 128),   // ok: Semantic Green (states/alerts only)
        RGB(230, 174, 72),   // warn: Semantic Amber (states/alerts only)
        RGB(232, 92, 92),    // err: Semantic Red (states/alerts only)
        RGB(44, 47, 54),     // track: Slider / Progress Track
        RGB(24, 26, 30),     // overlay: Floating Dialog / Toast Glass
        28,                  // cardRadius (diameter -> 14px D2D radius)
        18,                  // controlRadius
    };
}

constexpr ThemeSpec ClassicAnkhTheme() {
    return {
        L"classic",
        L"Classic AnkhCore",
        true,
        RGB(7, 17, 42),      // bg: Deep Navy
        RGB(8, 22, 51),      // side: Navy Rail
        RGB(14, 33, 72),     // panel: Blue Acrylic Card
        RGB(22, 46, 94),     // panel2: Hover Blue Card
        RGB(30, 58, 102),    // border: Navy Border
        RGB(0, 120, 190),    // borderHi: Cyan-Blue Highlight
        RGB(0, 174, 255),    // accent: Neon Cyan-Blue
        RGB(0, 98, 160),     // accentDim: Dim Blue
        RGB(0, 60, 110),     // accentDeep: Deep Blue
        RGB(232, 244, 255),  // text: Cool White
        RGB(159, 180, 204),  // sub: Steel Gray-Blue
        RGB(94, 117, 145),   // faint: Muted Blue-Gray
        RGB(46, 204, 113),   // ok
        RGB(245, 176, 65),   // warn
        RGB(255, 92, 92),    // err
        RGB(22, 41, 77),     // track
        RGB(10, 24, 52),     // overlay
        28,
        18,
    };
}

// ---- 2.8: Windows Glass (the default). The Windows 11 dark Fluent tokens of Explorer (Mica #202020) and the Start menu / flyouts (Acrylic #1C1C1C - #2C2C2C):
// layer fill #202020, card #2B2B2B, hover #323232, stroke white ~7-10% (#3B3B3B / #5A5A5A), text white / 78% / 54%, flyout #2C2C2C, the system semantic colours
// (success #6CCB5F, caution #FCE100, critical #FF99A4) and the person's own Windows accent colour (Settings > Personalization > Colors), lightened as Windows does in dark mode.
inline COLORREF ThemeMix(COLORREF a, COLORREF b, int pctB) {
    const int p = std::clamp(pctB, 0, 100);
    return RGB((GetRValue(a) * (100 - p) + GetRValue(b) * p) / 100, (GetGValue(a) * (100 - p) + GetGValue(b) * p) / 100, (GetBValue(a) * (100 - p) + GetBValue(b) * p) / 100);
}
inline COLORREF WindowsAccentRgb() {   // HKCU\Software\Microsoft\Windows\DWM AccentColor = 0xAABBGGRR; Windows' default blue when it is not set
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return RGB(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF);
    return RGB(0, 120, 212);
}
inline ThemeSpec WindowsGlassTheme(COLORREF systemAccent = WindowsAccentRgb()) {
    const COLORREF acc = ThemeMix(systemAccent, RGB(255, 255, 255), 35);   // dark-mode accent: the light shade of the system colour
    return {
        L"windows-glass",
        L"Windows Glass",
        false,
        RGB(32, 32, 32),     // bg: Mica base of Explorer
        RGB(28, 28, 28),     // side: Start-menu acrylic base
        RGB(43, 43, 43),     // panel: card fill
        RGB(50, 50, 50),     // panel2: card / control hover
        RGB(59, 59, 59),     // border: control stroke (white ~7%)
        RGB(90, 90, 90),     // borderHi: stroke on hover
        acc,                 // accent: the person's Windows accent colour (light shade)
        ThemeMix(acc, RGB(43, 43, 43), 45),    // accentDim
        ThemeMix(acc, RGB(32, 32, 32), 72),    // accentDeep: selected fill
        RGB(255, 255, 255),  // text: primary
        RGB(200, 200, 200),  // sub: secondary (78%)
        RGB(139, 139, 139),  // faint: tertiary (54%)
        RGB(108, 203, 95),   // ok
        RGB(252, 225, 0),    // warn
        RGB(255, 153, 164),  // err
        RGB(80, 80, 80),     // track
        RGB(44, 44, 44),     // overlay: flyout
        20,                  // cardRadius (diameter): Windows 11 rounded corners
        12,                  // controlRadius
    };
}

inline ThemeSpec& MutableActiveTheme() {
    static ThemeSpec t = WindowsGlassTheme();
    return t;
}

inline const ThemeSpec& ActiveTheme() {
    return MutableActiveTheme();
}

inline ThemeSpec ThemeById(const std::wstring& id) {
    if (id == L"classic") return ClassicAnkhTheme();
    if (id == L"neutral-glass") return NeutralGlassTheme();
    return WindowsGlassTheme();   // "windows-glass" and anything unknown: the default
}

// 2.9.5 (Task 2): the glass is 20 % more transparent than 2.8 (every layer keeps 80 % of its old opacity). Pop-up overlays only give up 6 %, so menus stay readable.
constexpr float kGlassTransparencyScale = 0.80f, kOverlayTransparencyScale = 0.94f;
inline GlassLayerSpec GlassLayer(GlassLevel level) {
    const ThemeSpec& t = ActiveTheme();
    switch (level) {
        case GlassLevel::Background:  return {t.bg,      0.78f * kGlassTransparencyScale, 0.06f, 0.02f, 0.00f};
        case GlassLevel::Surface:     return {t.panel,   0.60f * kGlassTransparencyScale, 0.15f, 0.04f, 0.26f};
        case GlassLevel::Elevated:    return {t.panel2,  0.74f * kGlassTransparencyScale, 0.18f, 0.06f, 0.30f};
        case GlassLevel::Interactive: return {t.track,   0.72f * kGlassTransparencyScale, 0.14f, 0.05f, 0.12f};
        case GlassLevel::Overlay:     return {t.overlay, 0.90f * kOverlayTransparencyScale, 0.18f, 0.07f, 0.36f};
    }
    return {t.panel, 0.60f * kGlassTransparencyScale, 0.15f, 0.04f, 0.26f};
}

inline float ThemeGlassAlpha(COLORREF c) {
    const ThemeSpec& t = ActiveTheme();
    if (c == t.panel)      return GlassLayer(GlassLevel::Surface).alpha;
    if (c == t.panel2)     return GlassLayer(GlassLevel::Elevated).alpha;
    if (c == t.track)      return GlassLayer(GlassLevel::Interactive).alpha;
    if (c == t.accentDeep) return 0.68f;
    if (c == t.overlay)    return GlassLayer(GlassLevel::Overlay).alpha;
    return 1.0f;
}

}  // namespace ak
