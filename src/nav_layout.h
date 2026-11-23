// AnkhCore 2.5 — navigation rail: scrolling and tooltip placement (pure logic; unit-tested in
// tests/test_base.cpp). All values are pixels in the main window's client area. The rail itself
// (app.cpp: NavLayout, PaintScene) owns the rows; nothing here reads a window or a global.
#pragma once
#include <algorithm>
#include <windows.h>

namespace ak {

// Furthest the rail can scroll: 0 when every row fits.
inline int NavScrollMax(int contentH, int viewH) { return std::max(0, contentH - std::max(0, viewH)); }

// A saved scroll offset brought back into range (a section collapsed, the window grew, the rail changed mode).
inline int NavScrollClamp(int scroll, int contentH, int viewH) { return std::clamp(scroll, 0, NavScrollMax(contentH, viewH)); }

// The smallest change of the scroll offset that puts a row fully inside the viewport. top/bottom are
// measured from the first row's top at scroll 0. A row taller than the viewport shows its top.
inline int NavScrollReveal(int scroll, int top, int bottom, int contentH, int viewH) {
    if (bottom - top >= viewH || top < scroll) scroll = top;
    else if (bottom > scroll + viewH) scroll = bottom - viewH;
    return NavScrollClamp(scroll, contentH, viewH);
}

// Tooltip of the compact rail: beside the rail, centred on its row, never outside the client area.
inline RECT NavTipRect(int railRight, int rowMidY, int textW, int padX, int halfH, int gap, int clientW, int clientH) {
    const int w = std::min(textW + 2 * padX, std::max(0, clientW - railRight - 2 * gap));
    const int top = std::clamp(rowMidY - halfH, gap, std::max(gap, clientH - gap - 2 * halfH));
    return RECT{railRight + gap, top, railRight + gap + w, top + 2 * halfH};
}

}   // namespace ak
