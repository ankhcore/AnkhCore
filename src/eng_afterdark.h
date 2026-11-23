// eng_afterdark.h - 2.8.4: the After Dark theme packs for Windows 10, chosen by the Windows build of THIS PC.
//
// The package carries one folder per Windows 10 release (see AdReleaseOf). Setup finds the folder that matches the build it runs on, offers only the options
// that folder really has, and copies only the chosen variant into %SystemRoot%\Resources\Themes. Nothing here guesses: a build that is not a known
// Windows 10 release (or a release without a pack) gets "no pack" and the reason.
//
// Folder layout of the source (D:\Ankhcore\windows\Resources\after-dark, shipped beside the setup as AfterDark\):
//   Theme For Win10 <release>\<Show|Hide> Commandbar\*.theme + "After Dark CC\" (msstyles, shell\NormalColor\shellstyle.dll, Wallpaper\)
//   Theme Fix Background White For Office\For Win10 <release>\...   (the same, with the Office white-background fix; older releases only have this)
//   Theme Fix Background White For Office\Theme For Win10 20H2 | 21H1\...
//   Extras\Fix bug white color after sleep for theme dark\{FIX.reg, UNFIX (restore defaults).reg}
// Pure over the file system it is given (a directory path), so tests run on a temp tree.
#pragma once
#include "base.h"

namespace ak {

struct AdRelease { unsigned build; const wchar_t* name; };
inline const std::vector<AdRelease>& AdReleases() {
    static const std::vector<AdRelease> v = {{14393, L"1607"}, {15063, L"1703"}, {16299, L"1709"}, {17134, L"1803"}, {17763, L"1809"}, {18362, L"1903"},
                                             {18363, L"1909"}, {19041, L"2004"}, {19042, L"20H2"}, {19043, L"21H1"}, {19044, L"21H2"}, {19045, L"22H2"}};
    return v;
}
// The Windows 10 release name of a build number, or "" (Windows 11, an unknown build, an Insider build).
inline std::wstring AdReleaseOf(unsigned build) { for (const auto& r : AdReleases()) if (r.build == build) return r.name; return L""; }
// The ThemeDll (SecureUxTheme) patch that lets Windows load these unsigned visual styles is only set up from 1809 on; older releases get the files but are told so.
inline bool AdPatchSupported(unsigned build) { return build >= 17763 && build < 22000; }

struct AdPack {
    bool found = false;
    bool officeOnly = false;              // this release only has the Office-background-fix variant: it IS the theme
    bool officeAvailable = false;         // a second pack with the Office white-background fix exists next to the standard one
    bool sleepFix = false;                // the "white after sleep" registry fix is in the package
    std::wstring release;                 // "22H2"
    std::wstring packRel;                 // folder of the standard (or only) pack, relative to the AfterDark root
    std::wstring officeRel;               // folder of the Office-fix pack, relative to the AfterDark root ("" when none)
    std::vector<std::wstring> variants;   // "Show Commandbar", "Hide Commandbar": only those that exist (and contain a .theme file)
    std::wstring reason;                  // why nothing was found
};

inline bool AdVariantHasTheme(const std::wstring& dir) { return DirExists(dir) && !ListDir(dir, false, L"*.theme").empty(); }

inline AdPack AdFindPack(const std::wstring& root, unsigned build) {
    AdPack p;
    p.release = AdReleaseOf(build);
    if (p.release.empty()) { p.reason = build >= 22000 ? L"This is Windows 11 (the After Dark packs are for Windows 10)." : L"This Windows 10 build (" + std::to_wstring(build) + L") is not one of the releases the After Dark packs were made for."; return p; }
    if (!DirExists(root)) { p.reason = L"The After Dark packs are not in this package."; return p; }
    const std::wstring std1 = L"Theme For Win10 " + p.release;
    const std::wstring off0 = L"Theme Fix Background White For Office";
    const std::wstring offA = off0 + L"\\For Win10 " + p.release, offB = off0 + L"\\Theme For Win10 " + p.release;
    auto hasVariant = [&](const std::wstring& rel) { return AdVariantHasTheme(root + L"\\" + rel + L"\\Show Commandbar") || AdVariantHasTheme(root + L"\\" + rel + L"\\Hide Commandbar"); };
    std::wstring off;
    if (hasVariant(offA)) off = offA; else if (hasVariant(offB)) off = offB;
    if (hasVariant(std1)) { p.packRel = std1; p.officeRel = off; p.officeAvailable = !off.empty(); }
    else if (!off.empty()) { p.packRel = off; p.officeOnly = true; }
    else { p.reason = L"There is no After Dark pack for Windows 10 " + p.release + L" in this package."; return p; }
    for (const wchar_t* v : {L"Show Commandbar", L"Hide Commandbar"}) if (AdVariantHasTheme(root + L"\\" + p.packRel + L"\\" + v)) p.variants.push_back(v);
    p.sleepFix = FileExists(root + L"\\Extras\\Fix bug white color after sleep for theme dark\\FIX.reg") && FileExists(root + L"\\Extras\\Fix bug white color after sleep for theme dark\\UNFIX (restore defaults).reg");
    p.found = !p.variants.empty();
    if (!p.found) p.reason = L"The pack folder has no theme file.";
    return p;
}

struct AdFile { std::wstring srcRel, dstRel; };   // srcRel: relative to the AfterDark root; dstRel: relative to %SystemRoot%\Resources\Themes
inline void AdWalk(const std::wstring& root, const std::wstring& rel, const std::wstring& dstRel, std::vector<AdFile>& out) {
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    for (const std::wstring& f : ListDir(dir, false)) {
        if (_wcsicmp(f.c_str(), L"Thumbs.db") == 0 || _wcsicmp(f.c_str(), L"desktop.ini") == 0) continue;
        out.push_back({rel + L"\\" + f, dstRel.empty() ? f : dstRel + L"\\" + f});
    }
    for (const std::wstring& d : ListDir(dir, true)) AdWalk(root, rel + L"\\" + d, dstRel.empty() ? d : dstRel + L"\\" + d, out);
}
// Every file of the chosen variant: the .theme files go to Themes\, the "After Dark CC" folder to Themes\After Dark CC\. `useOffice` picks the Office-fix pack when there is one.
inline std::vector<AdFile> AdPlanFiles(const std::wstring& root, const AdPack& p, bool useOffice, const std::wstring& variant) {
    std::vector<AdFile> out;
    if (!p.found) return out;
    const std::wstring base = (useOffice && p.officeAvailable) ? p.officeRel : p.packRel;
    if (std::find(p.variants.begin(), p.variants.end(), variant) == p.variants.end()) return out;   // only a variant that exists
    const std::wstring rel = base + L"\\" + variant;
    if (!AdVariantHasTheme(root + L"\\" + rel)) return out;
    for (const std::wstring& f : ListDir(root + L"\\" + rel, false)) {
        if (_wcsicmp(f.c_str(), L"Thumbs.db") == 0) continue;
        out.push_back({rel + L"\\" + f, f});
    }
    for (const std::wstring& d : ListDir(root + L"\\" + rel, true)) AdWalk(root, rel + L"\\" + d, d, out);
    return out;
}
// The folder (relative to the AfterDark root) that Setup verifies against COMPONENTS.sha256 before copying anything from it.
inline std::wstring AdComponentDir(const AdPack& p, bool useOffice, const std::wstring& variant) {
    return L"AfterDark\\" + std::wstring((useOffice && p.officeAvailable) ? p.officeRel : p.packRel) + L"\\" + variant;
}

}  // namespace ak
