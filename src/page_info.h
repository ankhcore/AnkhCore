// page_info.h: pure helpers for the Hardware Information, Network Monitor and Error Logs pages.
// No window, no global state: everything here takes its input as arguments, so test_base.cpp can check it.
#pragma once
#include "base.h"
#include "net_counters.h"

namespace ak {

// ---------------------------------------------------------------- Error Logs: the persisted log file
// Log() writes "[YYYY-MM-DD HH:MM:SS] TAG   text" (TAG = INFO / OK / WARN / ERROR, padded to 5), one line per entry.
struct ErrLogEntry { std::wstring date, time; Lv level = Lv::Info; std::wstring text; };

inline bool ParseLogLine(const std::wstring& raw, ErrLogEntry& e) {
    std::wstring l = raw;
    while (!l.empty() && (l.back() == L'\r' || l.back() == L'\n')) l.pop_back();
    if (l.size() < 28 || l[0] != L'[' || l[20] != L']' || l[21] != L' ' || l[27] != L' ') return false;
    for (int i : {1, 2, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19}) if (l[i] < L'0' || l[i] > L'9') return false;
    if (l[5] != L'-' || l[8] != L'-' || l[11] != L' ' || l[14] != L':' || l[17] != L':') return false;
    const std::wstring tag = l.substr(22, 5);
    if (tag == L"INFO ") e.level = Lv::Info;
    else if (tag == L"OK   ") e.level = Lv::Ok;
    else if (tag == L"WARN ") e.level = Lv::Warn;
    else if (tag == L"ERROR") e.level = Lv::Err;
    else return false;
    e.date = l.substr(1, 10); e.time = l.substr(12, 8); e.text = l.size() > 28 ? l.substr(28) : std::wstring();
    return true;
}
// All the entries in the text, in file order. A first line cut by a tail read, blank lines and foreign text are skipped.
inline std::vector<ErrLogEntry> ParseLogText(const std::wstring& text) {
    std::vector<ErrLogEntry> out;
    for (const auto& line : Split(text, L'\n')) { ErrLogEntry e; if (ParseLogLine(line, e)) out.push_back(std::move(e)); }
    return out;
}
// Warnings and errors (or errors only) of one day ("" = every day), newest first, at most `maxN`.
inline std::vector<ErrLogEntry> FilterLogEntries(const std::vector<ErrLogEntry>& in, bool errorsOnly, const std::wstring& date, size_t maxN) {
    std::vector<ErrLogEntry> out;
    for (auto it = in.rbegin(); it != in.rend() && out.size() < maxN; ++it) {
        const bool lvOk = errorsOnly ? it->level == Lv::Err : (it->level == Lv::Err || it->level == Lv::Warn);
        if (lvOk && (date.empty() || it->date == date)) out.push_back(*it);
    }
    return out;
}
struct LogCounts { int info = 0, ok = 0, warn = 0, err = 0; int Total() const { return info + ok + warn + err; } };
inline LogCounts CountLogEntries(const std::vector<ErrLogEntry>& in, const std::wstring& date) {
    LogCounts c;
    for (const auto& e : in) {
        if (!date.empty() && e.date != date) continue;
        switch (e.level) { case Lv::Info: ++c.info; break; case Lv::Ok: ++c.ok; break; case Lv::Warn: ++c.warn; break; case Lv::Err: ++c.err; break; }
    }
    return c;
}
// Reads at most the last `maxBytes` of a file as text. `cut` = the start of the file was not read (its first, partial line is dropped).
// False when the file cannot be opened (missing, or no access): the page says so instead of showing an empty list.
inline bool ReadLogTail(const std::wstring& path, size_t maxBytes, std::wstring& text, uint64_t& fileSize, bool& cut) {
    text.clear(); fileSize = 0; cut = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; if (!GetFileSizeEx(f, &sz)) { CloseHandle(f); return false; }
    fileSize = static_cast<uint64_t>(sz.QuadPart);
    const uint64_t want = std::min<uint64_t>(fileSize, maxBytes);
    LARGE_INTEGER pos{}; pos.QuadPart = static_cast<LONGLONG>(fileSize - want);
    if (want && !SetFilePointerEx(f, pos, nullptr, FILE_BEGIN)) { CloseHandle(f); return false; }
    std::string buf(static_cast<size_t>(want), '\0'); size_t at = 0; bool ok = true;
    while (ok && at < buf.size()) { DWORD got = 0; ok = ReadFile(f, buf.data() + at, static_cast<DWORD>(std::min<size_t>(buf.size() - at, 1u << 20)), &got, nullptr) && got > 0; at += got; }
    CloseHandle(f);
    buf.resize(at);
    if (!ok && at == 0 && want > 0) return false;
    cut = fileSize > want;
    text = Utf8ToW(buf);
    if (cut) { const size_t nl = text.find(L'\n'); text = nl == std::wstring::npos ? std::wstring() : text.substr(nl + 1); }
    return true;
}

// ---------------------------------------------------------------- Hardware Information
struct HwRow { std::wstring section, label, value; };
// The summary as plain text (what "Copy hardware summary" puts on the clipboard): one block per section, in the order the rows came.
inline std::wstring HwSummaryText(const std::vector<HwRow>& rows) {
    std::wstring out, cur; bool any = false;
    for (const auto& r : rows) {
        if (!any || r.section != cur) { if (any) out += L"\r\n"; out += L"== " + r.section + L" ==\r\n"; cur = r.section; any = true; }
        out += r.label + L": " + r.value + L"\r\n";
    }
    return out;
}
inline std::wstring DriveText(wchar_t letter, uint64_t total, uint64_t freeBytes) {
    const std::wstring name = std::wstring(1, letter ? letter : L'?') + L":";
    if (!total) return name + L" size not known";
    if (freeBytes > total) freeBytes = total;
    const int used = static_cast<int>(((total - freeBytes) * 100 + total / 2) / total);
    return name + L"  " + FormatBytes(total) + L", " + FormatBytes(freeBytes) + L" free (" + std::to_wstring(used) + L" % used)";
}
// "Intel(R) Core(TM) i7 CPU @ 2.60GHz" arrives padded with spaces and sometimes doubled ones.
inline std::wstring TidyHwName(const std::wstring& s) {
    std::wstring out; bool space = true;
    for (wchar_t c : s) { if (c == L' ' || c == L'\t' || c == 0) { if (!space) out.push_back(L' '); space = true; } else { out.push_back(c); space = false; } }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// ---------------------------------------------------------------- Network Monitor
inline std::wstring NetLinkText(uint64_t bps) {
    if (!bps) return L"speed not reported";
    wchar_t b[32];
    if (bps >= 1000000000ull) swprintf(b, 32, L"%.1f Gbps", bps / 1e9); else swprintf(b, 32, L"%.0f Mbps", bps / 1e6);
    return b;
}
inline const wchar_t* NetStateText(const NetAdapterView& a) { return !a.present ? L"Not present" : a.up ? L"Connected" : L"Disconnected"; }
// "Wi-Fi · 866 Mbps · signal 72 %" (speed and signal only while the link is up and the adapter reports them)
inline std::wstring NetAdapterLine(const NetAdapterView& a) {
    std::wstring s = NetTypeName(a.type);
    if (a.present && a.up && a.linkBps) s += L" · " + NetLinkText(a.linkBps);
    if (a.present && a.up && a.signal >= 0) s += L" · signal " + std::to_wstring(a.signal) + L" %";
    return s;
}

// ---------------------------------------------------------------- Acknowledgements & Credits (FC-13)
struct AcknowledgementItem {
    std::wstring name;
    std::wstring author;
    std::wstring license;
    std::wstring note;
    std::wstring url;
};

inline const std::vector<AcknowledgementItem>& AcknowledgementsList() {
    static const std::vector<AcknowledgementItem> kItems = {
        {L"LibreHardwareMonitorLib 0.9.6", L"LibreHardwareMonitor contributors", L"MPL-2.0",
         L"Powers AnkhSensors hardware temperature, clock, power, and fan telemetry. Thank you to all contributors.",
         L"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor"},
        {L"HidSharp 2.6.4", L"James F. Bellinger", L"Apache-2.0",
         L"HID device enumeration used unmodified by LibreHardwareMonitorLib.",
         L"http://software.seekye.com/hidsharp"},
        {L"BlackSharp.Core, DiskInfoToolkit, RAMSPDToolkit-NDD", L"Florian K. (Blacktempel)", L"See repository",
         L"Low-level storage and memory telemetry libraries used unmodified by LibreHardwareMonitorLib.",
         L"https://github.com/Blacktempel/BlackSharp"},
        {L".NET Runtime Packages", L".NET Foundation & Microsoft Corporation", L"MIT",
         L"System.Buffers, System.Memory, System.Numerics.Vectors, and System.Runtime.CompilerServices.Unsafe.",
         L"https://github.com/dotnet/runtime"},
        {L"PawnIO 2.2", L"namazso", L"See PawnIO project",
         L"Signed hardware sensor driver used by AnkhSensors in place of WinRing0.",
         L"https://github.com/namazso/PawnIO"},
        {L"PawnIO modules: LpcThinkpadEC, DellSMM", L"namazso & contributors (PawnIO.Modules 0.2.12)", L"LGPL-2.1",
         L"Signed modules loaded into the PawnIO driver to read the fan speed of ThinkPad and Dell notebooks (read-only). Shipped unmodified with their licence text; the source is at the project link.",
         L"https://github.com/namazso/PawnIO.Modules"},
        {L"SecureUxTheme", L"namazso", L"LGPL-2.1",
         L"Enables signed third-party Windows visual styles.",
         L"https://github.com/namazso/SecureUxTheme"},
        {L"Windhawk & Resource Redirect 1.3", L"Ramen Software & mod authors", L"See project / GPL-3.0 (mod)",
         L"Mod host and icon theme redirection engine; Windows Animations base by ReDrag (MIT). Two mods are modified versions: Resource Redirect and Windows 11 File Explorer Styler (modified by As3aD; the original authors and licences are kept).",
         L"https://windhawk.net"},
        {L"smartmontools (smartctl.exe, optional)", L"Bruce Allen, Christian Franke & contributors", L"GPL-2.0",
         L"SMART disk diagnostics: smartctl.exe 7.5 (the official Windows build, unmodified) is shipped with the package and run as a separate, read-only process, with its GPL-2.0 licence text.",
         L"https://www.smartmontools.org/"},
        {L"Notebook fan configurations (NoteBook FanControl project)", L"NBFC authors & contributors", L"GPL-3.0 (data)",
         L"The per-model embedded-controller register maps used by AnkhCore's fan control. Shipped unmodified, as data, with a licence notice. AnkhCore's own engine does not need the NBFC program; if you installed NBFC yourself, AnkhCore can drive it too.",
         L"https://github.com/hirschmann/nbfc"},
        {L"PawnIO module LpcACPIEC", L"namazso & contributors (PawnIO.Modules 0.2.12)", L"LGPL-2.1",
         L"Signed module that gives AnkhCore access to the notebook embedded controller for fan control (only the standard read and write commands, with confirmation first).",
         L"https://github.com/namazso/PawnIO.Modules"},
        {L"UniGetUI", L"Marti Climent & contributors", L"See project",
         L"Used by Software updates in AnkhCore's own private session (the portable copy that comes with AnkhCore; never your own UniGetUI window).",
         L"https://github.com/Devolutions/UniGetUI"},
        {L"Process, tweak and cleanup catalogues", L"System Informer, Process Governor, Sophia Script, WinUtil, BleachBit, Ananicy, ThreadPilot", L"MIT / GPL-3.0 / AGPL-3.0",
         L"The Process Manager, the extra Tweaks and the application-cache cleanup items are AnkhCore's own implementation, designed after the capabilities of these projects. No code of theirs is copied or run.",
         L"https://github.com/winsiderss/systeminformer"},
        {L"Microsoft PowerToys 0.102.3101", L"Microsoft Corporation and contributors", L"MIT",
         L"Installed with AnkhCore Setup as a separate program (like Windhawk). AnkhCore switches its utilities on or off from Windows Studio > PowerToys and does not copy any of its code.",
         L"https://github.com/microsoft/PowerToys"},
        {L"Community Themes, Cursors & Icon Packs", L"niivu, Mr GRiM, Dark Knight, Razorsedge & Moonnique", L"Original creator terms",
         L"Thank you to the creators of BIB, HUD Evolution, Tech Icons, Magnum Dark, Sweet Paranoia, and Big Sur 2.",
         L""}
    };
    return kItems;
}

}  // namespace ak
