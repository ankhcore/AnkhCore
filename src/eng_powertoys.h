// eng_powertoys.h - 2.8: Microsoft PowerToys (MIT) as an EXTERNAL optional component, like Windhawk.
//
// AnkhCore Setup installs the official PowerToys installer that ships beside it (hash-checked through COMPONENTS.sha256, installed silently and only
// when PowerToys is not already there). AnkhCore then shows PowerToys on its own page: whether it is installed and running, the useful utilities
// (FancyZones, Always On Top, Awake, Color Picker, Keyboard Manager, PowerToys Run, Text Extractor, ...) with a switch for each, and buttons to
// open them. The switches edit the `enabled` block of PowerToys' own settings.json (a backup is kept; only the known keys are touched; the file is
// written atomically) and PowerToys is restarted to apply them. Nothing of PowerToys is copied into AnkhCore: it stays a separate program.
// Pure text functions and detection only: nothing here starts or stops a process.
#pragma once
#include "base.h"

namespace ak {

struct PtInstall { bool installed = false; std::wstring dir, exe, version; };

inline std::wstring PtVersionOf(const std::wstring& path) {
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return L"";
    std::vector<uint8_t> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return L"";
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) || !fi) return L"";
    return std::to_wstring(HIWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionMS)) + L"." + std::to_wstring(HIWORD(fi->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(fi->dwFileVersionLS));
}
// Machine-wide (Program Files) or per-user installs.
inline PtInstall PtDetect() {
    PtInstall r;
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    for (const std::wstring& d : {KnownFolder(FOLDERID_ProgramFiles) + L"\\PowerToys", local + L"\\PowerToys", local + L"\\Programs\\PowerToys"}) {
        if (FileExists(d + L"\\PowerToys.exe")) { r.installed = true; r.dir = d; r.exe = d + L"\\PowerToys.exe"; r.version = PtVersionOf(r.exe); return r; }
    }
    return r;
}
inline std::wstring PtSettingsPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Microsoft\\PowerToys\\settings.json"; }

struct PtModule { const wchar_t* key; const wchar_t* title; const wchar_t* desc; const wchar_t* hotkey; const wchar_t* editorExe; };
// The utilities that add something AnkhCore does not have. The keys are the names PowerToys uses in its `enabled` block.
inline const std::vector<PtModule>& PtModules() {
    static const std::vector<PtModule> v = {
        {L"FancyZones", L"FancyZones", L"Snap windows into custom layouts (zones) on every monitor. Hold Shift while dragging a window.", L"Win+Shift+` (editor)", L"PowerToys.FancyZonesEditor.exe"},
        {L"AlwaysOnTop", L"Always On Top", L"Pin any window above all others.", L"Win+Ctrl+T", nullptr},
        {L"Awake", L"Awake", L"Keep the PC awake without changing its power settings.", L"tray icon", nullptr},
        {L"ColorPicker", L"Color Picker", L"Pick a color from anywhere on the screen and copy it in the format you choose.", L"Win+Shift+C", nullptr},
        {L"Keyboard Manager", L"Keyboard Manager", L"Remap keys and shortcuts.", L"settings", nullptr},
        {L"PowerToys Run", L"PowerToys Run", L"A quick launcher: apps, files, calculator, window switcher.", L"Alt+Space", nullptr},
        {L"TextExtractor", L"Text Extractor", L"Copy text from any image or video on the screen (OCR).", L"Win+Shift+T", nullptr},
        {L"Measure Tool", L"Screen Ruler", L"Measure pixels on the screen.", L"Win+Shift+M", nullptr},
        {L"Shortcut Guide", L"Shortcut Guide", L"Show the Windows shortcuts while the Windows key is held.", L"Win+Shift+/", nullptr},
        {L"File Locksmith", L"File Locksmith", L"See which process holds a file or folder open (right-click menu).", L"right-click", nullptr},
        {L"Peek", L"Peek", L"Preview a file without opening it.", L"Ctrl+Space", nullptr},
        {L"QuickAccent", L"Quick Accent", L"Type accented characters by holding a letter.", L"hold a key", nullptr},
        {L"ZoomIt", L"ZoomIt", L"Zoom and draw on the screen for presentations.", L"Ctrl+1 / Ctrl+2", nullptr},
        {L"Workspaces", L"Workspaces", L"Save and restore a set of apps and their window positions.", L"Win+Ctrl+`", nullptr},
        {L"Image Resizer", L"Image Resizer", L"Resize pictures from the right-click menu.", L"right-click", nullptr},
        {L"Hosts", L"Hosts File Editor", L"Edit the Windows hosts file safely.", L"settings", nullptr},
    };
    return v;
}

// 2.8.3: one recognisable symbol per utility (Segoe MDL2 Assets, the icon font the rest of AnkhCore uses). Approximate on purpose: these are not PowerToys' own artwork.
inline const wchar_t* PtIconOf(const std::wstring& key) {
    struct I { const wchar_t* key; const wchar_t* glyph; };
    static const I k[] = {
        {L"FancyZones", L"\xE7C4"},  // overlapping windows / layout
        {L"AlwaysOnTop", L"\xE718"},  // pin
        {L"Awake", L"\xE7E8"},  // power
        {L"ColorPicker", L"\xE790"},  // colour
        {L"Keyboard Manager", L"\xE765"},  // keyboard
        {L"PowerToys Run", L"\xE721"},  // search / launcher
        {L"TextExtractor", L"\xE7C3"},  // page of text
        {L"Measure Tool", L"\xE7F4"},  // screen
        {L"Shortcut Guide", L"\xE946"},  // info / guide
        {L"File Locksmith", L"\xE72E"},  // lock
        {L"Peek", L"\xE7B3"},  // eye
        {L"QuickAccent", L"\xE8D2"},  // font / letters
        {L"ZoomIt", L"\xE71E"},  // magnifier
        {L"Workspaces", L"\xE8A7"},  // windows layout
        {L"Image Resizer", L"\xE91B"},  // picture
        {L"Hosts", L"\xE774"},  // globe
    };
    for (const I& i : k) if (key == i.key) return i.glyph;
    return L"\xE8B0";
}

// ---------------------------------------------------------------- settings.json: the `enabled` block (text level: formatting and every other setting are untouched)
namespace ptdetail {
inline bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
inline size_t Skip(const std::string& s, size_t i) { while (i < s.size() && IsWs(s[i])) ++i; return i; }
// The span of the object that follows  "enabled": {  ... }  (the first such member). b = index after '{', e = index of the matching '}'.
inline bool EnabledBlock(const std::string& j, size_t& b, size_t& e) {
    size_t at = 0;
    while ((at = j.find("\"enabled\"", at)) != std::string::npos) {
        size_t i = Skip(j, at + 9);
        if (i < j.size() && j[i] == ':') { i = Skip(j, i + 1); if (i < j.size() && j[i] == '{') { b = i + 1; int depth = 1; bool str = false;
            for (size_t k = b; k < j.size(); ++k) { const char c = j[k]; if (str) { if (c == '\\') ++k; else if (c == '"') str = false; continue; } if (c == '"') str = true; else if (c == '{') ++depth; else if (c == '}' && --depth == 0) { e = k; return true; } }
            return false; } }
        at += 9;
    }
    return false;
}
// The position of the literal (true / false) after  "key":  inside [b, e). 0 when the key is not there.
inline size_t ValuePos(const std::string& j, size_t b, size_t e, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t at = b;
    while ((at = j.find(pat, at)) != std::string::npos && at < e) {
        size_t i = Skip(j, at + pat.size());
        if (i < e && j[i] == ':') { i = Skip(j, i + 1); if (j.compare(i, 4, "true") == 0 || j.compare(i, 5, "false") == 0) return i; }
        at += pat.size();
    }
    return 0;
}
}  // namespace ptdetail

// 1 = on, 0 = off, -1 = not listed (this PowerToys version does not have the module, or the file is not valid)
inline int PtReadEnabled(const std::string& json, const std::string& key) {
    size_t b, e; if (!ptdetail::EnabledBlock(json, b, e)) return -1;
    const size_t v = ptdetail::ValuePos(json, b, e, key);
    return v ? (json.compare(v, 4, "true") == 0 ? 1 : 0) : -1;
}
// Returns false (and leaves `out` empty) when the key is not listed. Everything else in the file stays byte for byte.
inline bool PtSetEnabled(const std::string& json, const std::string& key, bool on, std::string& out) {
    out.clear(); size_t b, e; if (!ptdetail::EnabledBlock(json, b, e)) return false;
    const size_t v = ptdetail::ValuePos(json, b, e, key); if (!v) return false;
    const size_t len = json.compare(v, 4, "true") == 0 ? 4 : 5;
    out = json.substr(0, v) + (on ? "true" : "false") + json.substr(v + len);
    return true;
}
inline bool PtKeyAllowed(const std::wstring& key) { for (const auto& m : PtModules()) if (key == m.key) return true; return false; }

// ---------------------------------------------------------------- live actions (the page calls them on a click)
// Writes the new value: a backup of the original is kept once, the new file is written beside it and moved over the old one.
inline bool PtWriteEnabled(const std::wstring& key, bool on, std::wstring& why) {
    if (kTestBuild) { TestRefused(L"edit the PowerToys settings"); why = L"the test build changes no PowerToys setting"; return false; }
    if (!PtKeyAllowed(key)) { why = L"Not a module AnkhCore manages."; return false; }
    const std::wstring path = PtSettingsPath();
    if (!FileExists(path)) { why = L"PowerToys has not created its settings yet. Open PowerToys once, then try again."; return false; }
    const std::string cur = ReadText(path); std::string next;
    if (!PtSetEnabled(cur, WToUtf8(key), on, next)) { why = L"This PowerToys version does not list " + key + L"."; return false; }
    if (!FileExists(path + L".ankhcore-bak")) WriteAll(path + L".ankhcore-bak", reinterpret_cast<const uint8_t*>(cur.data()), cur.size());
    const std::wstring tmp = path + L".ankhcore-new";
    if (!WriteAll(tmp, reinterpret_cast<const uint8_t*>(next.data()), next.size())) { why = L"The settings file could not be written."; return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(tmp.c_str()); why = L"The settings file could not be replaced."; return false; }
    return true;
}
inline bool PtRestart(const PtInstall& pt, std::wstring& why) {
    if (kTestBuild) { TestRefused(L"restart PowerToys"); why = L"the test build does not restart PowerToys"; return false; }
    if (!pt.installed) { why = L"PowerToys is not installed."; return false; }
    KillProcess(L"PowerToys.exe", 0);
    Sleep(800);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", pt.exe.c_str(), nullptr, pt.dir.c_str(), SW_SHOWNORMAL)) > 32;
}

}  // namespace ak
