// AnkhCore 2.5 — which window material applies right now (pure, unit-tested).
// The fallback ladder of the Blue Acrylic design. Windows' own settings always win over the user's choice.
#pragma once
#include <windows.h>

namespace ak {

enum Material { MatAcrylic = 0, MatFrosted = 1, MatSolid = 2, MatWallpaper = 3, MatClear = 4 };   // values of appearance.material; Clear (2.9.8): a see-through window, blur only inside the cards

struct MaterialInput {
    int choice = MatAcrylic;
    bool transparencyOn = true, highContrast = false, batterySaver = false, onBattery = false;
    bool frostedOnBattery = true;   // owner decision P01 (approved 2026-11-08): the default, changeable by the user
    DWORD build = 26300;
};

constexpr DWORD kSystemBackdropBuild = 22621;   // DWMWA_SYSTEMBACKDROP_TYPE: Windows 11 22H2

inline Material ResolveMaterial(const MaterialInput& in) {
    if (in.highContrast || !in.transparencyOn || in.batterySaver) return MatSolid;
    if (in.choice == MatSolid) return MatSolid;
    if (in.choice == MatClear) return MatClear;   // no system backdrop is needed: the window is plain transparency, the cards blur the wallpaper themselves
    if (in.choice == MatWallpaper || in.build < kSystemBackdropBuild) return MatWallpaper;   // no system backdrop before 22H2: simulated glass
    if (in.choice == MatFrosted) return MatFrosted;
    return in.onBattery && in.frostedOnBattery ? MatFrosted : MatAcrylic;
}
inline int BackdropType(Material m) { return m == MatAcrylic ? 3 /*DWMSBT_TRANSIENTWINDOW*/ : m == MatFrosted ? 4 /*DWMSBT_TABBEDWINDOW*/ : 1 /*DWMSBT_NONE: Solid and Wallpaper glass draw their own background*/; }

// 2.10 Clear glass controls (owner): how solid the window base is (the page behind the cards) and how strongly the cards blur the picture behind them.
constexpr int kClearBgDefault = 30, kClearBlurDefault = 14, kClearBlurMax = 40;
constexpr int kClearFrostDefault = 60;   // how much of the whole window is the blurred desktop picture (0 = see-through, 100 = frosted like an Explorer window with a blur mod)
inline int ClearBgPct(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
inline int ClearBlurPx(int v) { return v < 0 ? 0 : v > kClearBlurMax ? kClearBlurMax : v; }

}  // namespace ak
