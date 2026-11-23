// AnkhCore 2.5 — WT-03: SMART disk diagnostics (read-only).
//
// Source of the data: the external `smartctl.exe` of smartmontools (GPL-2.0, run as a separate program, never linked),
// always with structured JSON output (`--scan -j`, `-a -j`); the human-readable text is never parsed. When smartctl is not
// installed the page falls back to what Windows itself reports (MSFT_PhysicalDisk: model, size, bus, media type, health)
// and says so; it never invents SMART values. Only read-only smartctl options are ever used: no self-test is started
// (`-t`), nothing is enabled or changed (`-s`, `--set`, `--offlineauto`).
//
// Pure parts (ParseScan, ParseDevice, Assess, ArgsAreReadOnly) are unit-tested with JSON fixtures in tests/test_smart.cpp.
// Not every drive exposes the same attributes: each value carries a "has" flag and is shown only when the drive reported it.
#pragma once
#include "base.h"
#include "json_min.h"
#include "eng_system.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <ctime>

namespace ak {

enum class SmartState { Good, Warning, Critical, Unknown, Unsupported };
inline const wchar_t* SmartStateName(SmartState s) {
    switch (s) {
        case SmartState::Good: return L"Good";
        case SmartState::Warning: return L"Warning";
        case SmartState::Critical: return L"Critical";
        case SmartState::Unsupported: return L"Not supported";
        default: return L"Unknown";
    }
}

struct SmartAttr {
    int id = 0; std::wstring name; int value = 0, worst = 0, thresh = 0; uint64_t raw = 0; std::wstring rawText; bool prefail = false;
};
// One finding, in words. `advisory`: false = the drive itself (or smartctl) reported it (a failed overall status, NVMe critical-warning bits, a
// spare or attribute at the drive's own threshold); true = AnkhCore's own reading of a reported number against a limit AnkhCore chose.
struct SmartIssue { SmartState level = SmartState::Warning; std::wstring text; bool advisory = false; };

// AnkhCore's advisory limits. They are rules of thumb, NOT hardware standards and NOT thresholds published by any drive vendor:
//  - tempWarnC: many HDDs are rated to about 55-60 C and many NVMe drives only throttle between 70 and 85 C, so one number fits none of them well.
//  - lifeWarnPct / spareLowPct: wear and spare indicators are vendor estimates with vendor-specific scaling; drives often report 100 % until late.
// A result that rests only on these is shown as "Advisory", never as the drive's own verdict, and never as Critical. Not user-configurable yet.
struct SmartAdvisoryLimits { int tempWarnC = 70; int lifeWarnPct = 20; int spareLowPct = 20; };
inline const SmartAdvisoryLimits& SmartAdvisory() { static const SmartAdvisoryLimits l; return l; }

struct SmartDisk {
    // identity
    std::wstring dev, type, protocol;            // "/dev/sda", "sat", "ATA"
    std::wstring model, family, firmware, serialTail;   // serialTail: the last 4 characters only
    uint64_t capacity = 0;
    std::wstring iface = L"Unknown", kind = L"Unknown";   // "SATA" / "NVMe" / "USB bridge" / "SCSI / SAS"; "HDD" / "SSD" / "NVMe SSD"
    int rpm = -1;                                // 0 = solid state, >0 = rotating, -1 = not reported
    // SMART values: each is shown only when its has-flag is set (drives differ)
    bool hasStatus = false, statusPassed = false;
    bool hasTemp = false; int tempC = 0;
    bool hasPoh = false; uint64_t powerOnHours = 0;
    bool hasCycles = false; uint64_t powerCycles = 0;
    bool hasLife = false; int lifeLeftPct = 0;   // remaining life in %, from NVMe percentage_used or an SSD wear attribute
    bool hasSpare = false; int sparePct = 0, spareThresh = 0;
    bool hasWritten = false; uint64_t bytesWritten = 0;
    bool hasMediaErrors = false; uint64_t mediaErrors = 0;
    bool hasUnsafe = false; uint64_t unsafeShutdowns = 0;
    bool hasNvmeWarn = false; unsigned nvmeCriticalWarning = 0;
    bool hasRealloc = false; uint64_t reallocated = 0;
    bool hasPending = false; uint64_t pending = 0;
    bool hasUncorrectable = false; uint64_t uncorrectable = 0;
    bool hasCrc = false; uint64_t crcErrors = 0;
    bool hasErrorLog = false; uint64_t errorLogCount = 0;
    bool hasSelfTest = false; bool selfTestPassed = true; std::wstring selfTestText;   // the last self-test result the drive recorded (none is started here)
    bool selfTestCapable = false;
    std::vector<SmartAttr> attrs;                // the ATA attribute table as reported
    unsigned exitStatus = 0; bool hasExit = false;
    // assessment
    SmartState state = SmartState::Unknown;
    std::vector<SmartIssue> issues;
    std::wstring problem;                        // why nothing could be read (open failed, bridge unsupported, ...)
    bool hasData = false;                        // the drive reported SMART data
    bool limited = false;                        // from Windows' own storage data only (smartctl missing): no SMART attributes
};

// ---------------------------------------------------------------- pure parsing
inline const JVal* SmartPath(const JVal& v, std::initializer_list<const char*> keys) {
    const JVal* p = &v;
    for (const char* k : keys) { p = p->Get(k); if (!p) return nullptr; }
    return p;
}
inline bool SmartU64(const JVal* v, uint64_t& out) { return v && v->AsUInt(out); }
inline bool SmartStr(const JVal* v, std::wstring& out) { if (!v || !v->Is(JVal::Str)) return false; out = Utf8ToW(v->s); return true; }

struct SmartScanDev { std::wstring name, type, protocol; };
// `smartctl --scan -j`: {"devices":[{"name":"/dev/sda","info_name":"/dev/sda","type":"sat","protocol":"ATA"}, ...]}
inline bool SmartParseScan(const std::string& json, std::vector<SmartScanDev>& out) {
    out.clear(); JVal root;
    if (!JsonParse(json, root) || !root.Is(JVal::Obj)) return false;
    const JVal* d = root.Get("devices");
    if (!d) return root.Get("smartctl") != nullptr;   // a valid answer without any device
    if (!d->Is(JVal::Arr)) return false;
    for (const JVal& e : d->a) {
        SmartScanDev s;
        if (!SmartStr(e.Get("name"), s.name) || s.name.empty()) continue;
        SmartStr(e.Get("type"), s.type); SmartStr(e.Get("protocol"), s.protocol);
        out.push_back(std::move(s));
    }
    return true;
}
// Device name and type go onto a command line: only the shapes smartctl itself prints are accepted.
inline bool SmartDevNameOk(const std::wstring& n) {
    if (n.size() < 6 || n.size() > 24 || n.compare(0, 5, L"/dev/") != 0) return false;
    for (size_t i = 5; i < n.size(); ++i) { const wchar_t c = n[i]; if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) return false; }
    return true;
}
inline bool SmartTypeOk(const std::wstring& t) {
    if (t.empty() || t.size() > 24) return false;
    for (wchar_t c : t) if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L',' || c == L'+' || c == L'#')) return false;
    return true;
}
// Read-only guard: the single definition lives in base.h (SmartctlArgsAreReadOnly), shared with RunCapture's test-build guard.
inline bool SmartArgsAreReadOnly(const std::wstring& args) { return SmartctlArgsAreReadOnly(args); }

inline void SmartAddIssue(SmartDisk& d, SmartState lv, const std::wstring& t) { d.issues.push_back({lv, t, false}); }
inline void SmartAddAdvisory(SmartDisk& d, SmartState lv, const std::wstring& t) { d.issues.push_back({lv, t, true}); }

// The assessment is deterministic and written out here so a result can be explained:
//   Critical  - the drive's own overall SMART status is FAILED; NVMe critical-warning bits set; NVMe spare at or below its
//               threshold; an ATA prefail attribute at or below its threshold.
//   Warning   - reallocated / pending / uncorrectable sectors; NVMe media errors; NVMe or SSD life used 80 % or more;
//               low spare; temperature 70 °C or more; the last recorded self-test failed; entries in the drive's error log.
//   Good      - the drive reports SMART data, its status passed and none of the above applies.
//   Unknown   - the drive answered but gave no verdict; Not supported - SMART is not available through this connection.
inline void SmartAssess(SmartDisk& d) {
    d.issues.clear();
    if (!d.hasData) {
        const std::wstring p = Lower(d.problem);
        const bool unsup = p.find(L"unsupported") != std::wstring::npos || p.find(L"unknown usb bridge") != std::wstring::npos ||
                           p.find(L"unable to detect device type") != std::wstring::npos || p.find(L"not support") != std::wstring::npos ||
                           p.find(L"smart support is: unavailable") != std::wstring::npos;
        d.state = unsup ? SmartState::Unsupported : SmartState::Unknown;
        return;
    }
    if (d.hasStatus && !d.statusPassed) SmartAddIssue(d, SmartState::Critical, L"The drive reports that SMART status has FAILED. Back up your data now.");
    if (d.hasNvmeWarn && d.nvmeCriticalWarning) {
        std::wstring w;
        const struct { unsigned bit; const wchar_t* t; } f[] = {{1, L"available spare below threshold"}, {2, L"temperature outside limits"}, {4, L"reliability degraded"}, {8, L"media is read-only"}, {16, L"volatile memory backup failed"}};
        for (const auto& x : f) if (d.nvmeCriticalWarning & x.bit) { if (!w.empty()) w += L", "; w += x.t; }
        SmartAddIssue(d, SmartState::Critical, L"NVMe critical warning: " + (w.empty() ? std::wstring(L"unspecified flag") : w) + L".");
    }
    if (d.hasSpare && d.spareThresh > 0 && d.sparePct <= d.spareThresh) SmartAddIssue(d, SmartState::Critical, L"Available spare (" + std::to_wstring(d.sparePct) + L" %) is at or below the drive's threshold.");
    else if (d.hasSpare && d.sparePct < SmartAdvisory().spareLowPct) SmartAddAdvisory(d, SmartState::Warning, L"Available spare is low (" + std::to_wstring(d.sparePct) + L" %, below the advisory " + std::to_wstring(SmartAdvisory().spareLowPct) + L" %).");
    for (const auto& a : d.attrs)
        if (a.prefail && a.thresh > 0 && a.value > 0 && a.value <= a.thresh) SmartAddIssue(d, SmartState::Critical, L"Attribute " + std::to_wstring(a.id) + L" " + a.name + L" has reached its failure threshold.");
    if (d.hasRealloc && d.reallocated) SmartAddAdvisory(d, SmartState::Warning, std::to_wstring(d.reallocated) + L" reallocated sector(s).");
    if (d.hasPending && d.pending) SmartAddAdvisory(d, SmartState::Warning, std::to_wstring(d.pending) + L" sector(s) waiting to be reallocated.");
    if (d.hasUncorrectable && d.uncorrectable) SmartAddAdvisory(d, SmartState::Warning, std::to_wstring(d.uncorrectable) + L" uncorrectable sector error(s).");
    if (d.hasMediaErrors && d.mediaErrors) SmartAddAdvisory(d, SmartState::Warning, std::to_wstring(d.mediaErrors) + L" media / data integrity error(s).");
    if (d.hasLife && d.lifeLeftPct <= SmartAdvisory().lifeWarnPct) SmartAddAdvisory(d, SmartState::Warning, L"About " + std::to_wstring(d.lifeLeftPct) + L" % of the rated life is left.");
    if (d.hasTemp && d.tempC >= SmartAdvisory().tempWarnC) SmartAddAdvisory(d, SmartState::Warning, L"The drive is hot (" + std::to_wstring(d.tempC) + L" \x00B0" L"C, advisory limit " + std::to_wstring(SmartAdvisory().tempWarnC) + L" \x00B0" L"C).");
    if (d.hasSelfTest && !d.selfTestPassed) SmartAddIssue(d, SmartState::Warning, L"The last self-test recorded by the drive failed.");
    if (d.hasErrorLog && d.errorLogCount) SmartAddAdvisory(d, SmartState::Warning, std::to_wstring(d.errorLogCount) + L" entr" + (d.errorLogCount == 1 ? L"y" : L"ies") + L" in the drive's error log.");
    if (d.hasExit) {   // smartctl's own summary bits, for what the fields above may not show (bit 3: failing, 4: prefail at threshold)
        if ((d.exitStatus & 8) && d.hasStatus == false) SmartAddIssue(d, SmartState::Critical, L"smartctl reports the disk as failing.");
    }
    SmartState s = SmartState::Good;
    for (const auto& i : d.issues) if (i.level == SmartState::Critical) s = SmartState::Critical; else if (i.level == SmartState::Warning && s == SmartState::Good) s = SmartState::Warning;
    if (s == SmartState::Good && !d.hasStatus && !d.hasNvmeWarn) s = SmartState::Unknown;   // data, but no verdict from the drive
    d.state = s;
}

// One `smartctl -a -j` document. Returns false when the text is not a JSON object at all.
inline bool SmartParseDevice(const std::string& json, SmartDisk& d) {
    JVal r;
    if (!JsonParse(json, r) || !r.Is(JVal::Obj)) return false;
    uint64_t u = 0;
    if (const JVal* dv = r.Get("device")) { SmartStr(dv->Get("name"), d.dev); SmartStr(dv->Get("type"), d.type); SmartStr(dv->Get("protocol"), d.protocol); }
    if (const JVal* sc = r.Get("smartctl")) {
        if (SmartU64(sc->Get("exit_status"), u)) { d.exitStatus = static_cast<unsigned>(u); d.hasExit = true; }
        if (const JVal* m = sc->Get("messages")) if (m->Is(JVal::Arr)) for (const JVal& e : m->a) {
            std::wstring s, sev; SmartStr(e.Get("string"), s); SmartStr(e.Get("severity"), sev);
            if (!s.empty() && (sev == L"error" || sev == L"warning") && d.problem.empty()) d.problem = s;
        }
    }
    SmartStr(r.Get("model_name"), d.model); if (d.model.empty()) SmartStr(r.Get("model_family"), d.model);
    SmartStr(r.Get("model_family"), d.family); SmartStr(r.Get("firmware_version"), d.firmware);
    { std::wstring sn; if (SmartStr(r.Get("serial_number"), sn) && !sn.empty()) d.serialTail = sn.size() > 4 ? sn.substr(sn.size() - 4) : sn; }
    if (SmartU64(SmartPath(r, {"user_capacity", "bytes"}), u)) d.capacity = u;
    else if (SmartU64(r.Get("nvme_total_capacity"), u)) d.capacity = u;
    if (const JVal* rr = r.Get("rotation_rate")) if (rr->Is(JVal::Num)) d.rpm = static_cast<int>(rr->n);
    const std::wstring ty = Lower(d.type), pr = Lower(d.protocol);
    const bool nvme = pr == L"nvme" || ty.rfind(L"nvme", 0) == 0;
    d.iface = nvme ? L"NVMe" : ty.rfind(L"usb", 0) == 0 ? L"USB bridge" : pr == L"ata" || ty == L"sat" || ty == L"ata" ? L"SATA" : pr == L"scsi" ? L"SCSI / SAS" : d.protocol.empty() ? L"Unknown" : d.protocol;
    d.kind = nvme ? L"NVMe SSD" : d.rpm > 0 ? L"HDD" : d.rpm == 0 ? L"SSD" : L"Unknown";

    // what the drive says about SMART itself
    bool smartAvail = true;
    if (const JVal* ss = r.Get("smart_support")) if (const JVal* av = ss->Get("available")) if (av->Is(JVal::Bool)) smartAvail = av->b;
    if (const JVal* st = r.Get("smart_status")) if (const JVal* p = st->Get("passed")) if (p->Is(JVal::Bool)) { d.hasStatus = true; d.statusPassed = p->b; }
    if (const JVal* t = SmartPath(r, {"temperature", "current"})) if (t->Is(JVal::Num) && t->n > -50 && t->n < 200) { d.hasTemp = true; d.tempC = static_cast<int>(t->n); }
    if (SmartU64(SmartPath(r, {"power_on_time", "hours"}), u)) { d.hasPoh = true; d.powerOnHours = u; }
    if (SmartU64(r.Get("power_cycle_count"), u)) { d.hasCycles = true; d.powerCycles = u; }

    if (const JVal* nv = r.Get("nvme_smart_health_information_log")) {
        d.hasData = true;
        if (SmartU64(nv->Get("critical_warning"), u)) { d.hasNvmeWarn = true; d.nvmeCriticalWarning = static_cast<unsigned>(u); }
        if (!d.hasTemp) if (const JVal* t = nv->Get("temperature")) if (t->Is(JVal::Num) && t->n > -50 && t->n < 200) { d.hasTemp = true; d.tempC = static_cast<int>(t->n); }
        if (SmartU64(nv->Get("percentage_used"), u)) { d.hasLife = true; d.lifeLeftPct = u >= 100 ? 0 : 100 - static_cast<int>(u); }
        if (SmartU64(nv->Get("available_spare"), u)) { d.hasSpare = true; d.sparePct = static_cast<int>(u); if (SmartU64(nv->Get("available_spare_threshold"), u)) d.spareThresh = static_cast<int>(u); }
        if (SmartU64(nv->Get("data_units_written"), u)) { d.hasWritten = true; d.bytesWritten = u * 512000ull; }   // a data unit is 1000 x 512 bytes
        if (SmartU64(nv->Get("media_errors"), u)) { d.hasMediaErrors = true; d.mediaErrors = u; }
        if (SmartU64(nv->Get("unsafe_shutdowns"), u)) { d.hasUnsafe = true; d.unsafeShutdowns = u; }
        if (!d.hasPoh && SmartU64(nv->Get("power_on_hours"), u)) { d.hasPoh = true; d.powerOnHours = u; }
        if (!d.hasCycles && SmartU64(nv->Get("power_cycles"), u)) { d.hasCycles = true; d.powerCycles = u; }
        // num_err_log_entries counts every error ever logged (often harmless on NVMe), so it is not scored.
    }
    if (const JVal* at = SmartPath(r, {"ata_smart_attributes", "table"})) if (at->Is(JVal::Arr)) {
        d.hasData = true;
        for (const JVal& e : at->a) {
            SmartAttr a; uint64_t x = 0;
            if (!SmartU64(e.Get("id"), x)) continue;
            a.id = static_cast<int>(x); SmartStr(e.Get("name"), a.name);
            if (SmartU64(e.Get("value"), x)) a.value = static_cast<int>(x);
            if (SmartU64(e.Get("worst"), x)) a.worst = static_cast<int>(x);
            if (SmartU64(e.Get("thresh"), x)) a.thresh = static_cast<int>(x);
            if (const JVal* rw = e.Get("raw")) { SmartU64(rw->Get("value"), a.raw); SmartStr(rw->Get("string"), a.rawText); }
            if (const JVal* fl = e.Get("flags")) if (const JVal* pf = fl->Get("prefailure")) a.prefail = pf->Is(JVal::Bool) && pf->b;
            d.attrs.push_back(a);
            const uint64_t lo = a.raw & 0xFFFFFFFFull;   // several vendors pack extra counters into the upper raw bytes
            switch (a.id) {
                case 5: d.hasRealloc = true; d.reallocated = lo; break;
                case 187: d.hasUncorrectable = true; d.uncorrectable = std::max(d.uncorrectable, lo); break;
                case 197: d.hasPending = true; d.pending = lo; break;
                case 198: d.hasUncorrectable = true; d.uncorrectable = std::max(d.uncorrectable, lo); break;
                case 199: d.hasCrc = true; d.crcErrors = lo; break;
                case 9: if (!d.hasPoh) { d.hasPoh = true; d.powerOnHours = lo; } break;
                case 12: if (!d.hasCycles) { d.hasCycles = true; d.powerCycles = lo; } break;
                case 194: case 190: if (!d.hasTemp) { const int t = static_cast<int>(a.raw & 0xFF); if (t > 0 && t < 120) { d.hasTemp = true; d.tempC = t; } } break;
                case 177: case 231: case 233: case 202:   // normalised value = remaining life on SSDs
                    if (d.rpm == 0 && !d.hasLife && a.value > 0 && a.value <= 100) { d.hasLife = true; d.lifeLeftPct = a.value; }
                    break;
                default: break;
            }
        }
    }
    if (const JVal* sl = SmartPath(r, {"ata_smart_error_log", "summary", "count"})) { uint64_t c = 0; if (SmartU64(sl, c)) { d.hasErrorLog = true; d.errorLogCount = c; } }
    if (const JVal* stt = SmartPath(r, {"ata_smart_data", "self_test", "status"})) {
        d.selfTestCapable = true;
        std::wstring s; if (SmartStr(stt->Get("string"), s)) d.selfTestText = s;
        if (const JVal* p = stt->Get("passed")) if (p->Is(JVal::Bool)) { d.hasSelfTest = true; d.selfTestPassed = p->b; }
    }
    if (!smartAvail) d.hasData = false;
    if (d.hasStatus && smartAvail) d.hasData = true;
    if (d.model.empty() && !d.hasData && d.problem.empty()) d.problem = L"The drive did not identify itself.";
    SmartAssess(d);
    return true;
}

// ---------------------------------------------------------------- finding and running smartctl
// Where smartctl.exe may come from. Production builds trust ONLY <AppDir>\Tools\smartctl.exe (Program Files: not writable by standard users).
// <DataDir>\Tools is user-writable, so it is consulted only by the isolated test build (unit tests stage a stand-in there).
inline std::vector<std::wstring> SmartctlCandidateDirs(bool testBuild) {
    std::vector<std::wstring> dirs = {AppDir() + L"\\Tools"};
    if (testBuild) dirs.push_back(DataDir() + L"\\Tools");
    return dirs;
}
inline std::wstring SmartctlPath() {
    for (const std::wstring& dir : SmartctlCandidateDirs(kTestBuild)) {
        const std::wstring p = dir + L"\\smartctl.exe";
        if (FileExists(p)) return p;
    }
    return {};
}
// A path may be executed only when it is a canonical absolute path to a file named smartctl.exe that is a regular file (not a link).
inline bool SmartExePathOk(const std::wstring& path) {
    std::wstring c;
    if (!CleanCanonicalPath(path, c) || NameOf(c) != L"smartctl.exe") return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
// A program file we are about to run must at least be a 64-bit Windows executable (not a text file, not a renamed script).
inline bool SmartLooksLikeX64Exe(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t b[0x200] = {}; DWORD got = 0; const bool rd = ReadFile(h, b, sizeof(b), &got, nullptr) != 0; CloseHandle(h);
    if (!rd || got < 0x40 || b[0] != 'M' || b[1] != 'Z') return false;
    const uint32_t pe = *reinterpret_cast<const uint32_t*>(b + 0x3C);
    if (pe + 6 > got || b[pe] != 'P' || b[pe + 1] != 'E') return false;
    return *reinterpret_cast<const uint16_t*>(b + pe + 4) == 0x8664;
}

struct SmartReport {
    bool done = false, scanning = false;
    bool smartctlFound = false; std::wstring smartctlPath, smartctlVersion;
    std::wstring problem;                       // a whole-scan problem in words ("smartctl.exe is not installed", ...)
    std::vector<SmartDisk> disks;
    time_t scanned = 0;
};

inline std::string SmartRunJson(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs, DWORD* exitOut = nullptr) {
    std::string out;
    if (!SmartExePathOk(exe) || !SmartArgsAreReadOnly(args)) { if (exitOut) *exitOut = kRunLaunchFailed; return out; }   // never run another program or a non-read-only command line
    const DWORD rc = RunCapture(exe, args, [&](const std::wstring& line) { out += WToUtf8(line); out += '\n'; }, timeoutMs);
    if (exitOut) *exitOut = rc;
    return out;
}

// The drives Windows itself lists (used when smartctl is missing, and to name drives smartctl could not read).
inline std::vector<SmartDisk> SmartWindowsDisks() {
    std::vector<SmartDisk> out;
    for (const auto& r : WmiQuery(L"ROOT\\Microsoft\\Windows\\Storage", L"SELECT DeviceId, FriendlyName, Size, MediaType, BusType, HealthStatus FROM MSFT_PhysicalDisk",
                                  {L"DeviceId", L"FriendlyName", L"Size", L"MediaType", L"BusType", L"HealthStatus"})) {
        SmartDisk d; d.limited = true;
        auto g = [&](const wchar_t* k) { auto it = r.find(k); return it == r.end() ? std::wstring() : it->second; };
        d.dev = L"Disk " + g(L"DeviceId"); d.model = g(L"FriendlyName"); d.capacity = _wcstoui64(g(L"Size").c_str(), nullptr, 10);
        const int bus = _wtoi(g(L"BusType").c_str()), media = _wtoi(g(L"MediaType").c_str());
        d.iface = bus == 17 ? L"NVMe" : bus == 11 || bus == 3 ? L"SATA" : bus == 7 ? L"USB" : bus == 10 ? L"SAS" : bus == 8 ? L"RAID" : bus == 16 ? L"Storage Spaces" : L"Unknown";
        d.kind = bus == 17 ? L"NVMe SSD" : media == 3 ? L"HDD" : media == 4 ? L"SSD" : L"Unknown";
        const std::wstring hs = g(L"HealthStatus");
        if (hs == L"0") { d.hasStatus = true; d.statusPassed = true; d.state = SmartState::Good; }
        else if (hs == L"1") { d.state = SmartState::Warning; d.issues.push_back({SmartState::Warning, L"Windows reports the drive's health as Warning."}); }
        else if (hs == L"2") { d.state = SmartState::Critical; d.issues.push_back({SmartState::Critical, L"Windows reports the drive's health as Unhealthy."}); }
        else d.state = SmartState::Unknown;
        d.problem = L"smartctl.exe is not installed: only the basic health Windows itself reports is shown.";
        out.push_back(std::move(d));
    }
    return out;
}

// Scans and reads every drive. Blocking: call it on a worker thread.
inline SmartReport SmartScanNow() {
    SmartReport rep; rep.done = true; rep.scanned = time(nullptr);
    const std::wstring exe = SmartctlPath();
    if (exe.empty() || !SmartExePathOk(exe) || !SmartLooksLikeX64Exe(exe)) {
        rep.problem = exe.empty() ? L"smartctl.exe is not installed (Tools\\smartctl.exe). Showing only what Windows itself reports." : L"Tools\\smartctl.exe is not a 64-bit Windows program and was not run.";
        rep.disks = SmartWindowsDisks();
        return rep;
    }
    rep.smartctlFound = true; rep.smartctlPath = exe;
    DWORD rc = 0;
    const std::string scan = SmartRunJson(exe, L"--scan -j", 60000, &rc);
    std::vector<SmartScanDev> devs;
    if (rc == kRunTimedOut || rc == static_cast<DWORD>(-1) || !SmartParseScan(scan, devs)) {
        rep.problem = L"smartctl did not return a readable device list (" + std::wstring(rc == kRunTimedOut ? L"timed out" : L"no JSON output") + L"). Showing only what Windows itself reports.";
        rep.disks = SmartWindowsDisks();
        return rep;
    }
    { JVal j; if (JsonParse(scan, j)) if (const JVal* v = SmartPath(j, {"smartctl", "version"})) if (v->Is(JVal::Arr)) {
        for (size_t i = 0; i < v->a.size(); ++i) { uint64_t n = 0; if (v->a[i].AsUInt(n)) { if (i) rep.smartctlVersion += L"."; rep.smartctlVersion += std::to_wstring(n); } } } }
    for (const auto& dv : devs) {
        SmartDisk d;
        if (!SmartDevNameOk(dv.name) || (!dv.type.empty() && !SmartTypeOk(dv.type))) {
            d.dev = dv.name; d.problem = L"The device name reported by smartctl has an unexpected form and was skipped."; d.state = SmartState::Unknown; rep.disks.push_back(d); continue;
        }
        std::wstring args = L"-a -j";
        if (!dv.type.empty()) args += L" -d " + dv.type;
        args += L" " + dv.name;
        const std::string js = SmartRunJson(exe, args, 45000, &rc);
        if (rc == kRunTimedOut) { d.dev = dv.name; d.type = dv.type; d.protocol = dv.protocol; d.problem = L"The drive did not answer in time."; SmartAssess(d); rep.disks.push_back(d); continue; }
        if (!SmartParseDevice(js, d)) { d = SmartDisk(); d.dev = dv.name; d.type = dv.type; d.protocol = dv.protocol; d.problem = L"smartctl returned no readable JSON for this drive."; SmartAssess(d); }
        if (d.dev.empty()) d.dev = dv.name;
        if (d.type.empty()) d.type = dv.type;
        if (d.protocol.empty()) d.protocol = dv.protocol;
        rep.disks.push_back(std::move(d));
    }
    if (rep.disks.empty()) { rep.problem = L"smartctl found no drives. Run AnkhCore as administrator to read SMART data."; rep.disks = SmartWindowsDisks(); }
    return rep;
}

// Shared result; the scan runs on a worker thread and the page only reads the last copy.
class SmartCache {
public:
    SmartReport Get() const { std::lock_guard<std::mutex> lk(mx_); return rep_; }
    void RefreshAsync(std::function<void()> notify = nullptr) {
        { std::lock_guard<std::mutex> lk(mx_); if (rep_.scanning) return; rep_.scanning = true; }
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, notify] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SmartReport r = SmartScanNow();
            { std::lock_guard<std::mutex> lk(mx_); rep_ = std::move(r); rep_.scanning = false; }
            CoUninitialize();
            if (notify) notify();
        });
    }
    // Test build only (the UI self-test shows fixture states); refused in the program.
    void SetReportForTest(SmartReport r) { if (!kTestBuild) return; std::lock_guard<std::mutex> lk(mx_); rep_ = std::move(r); }
    ~SmartCache() { if (worker_.joinable()) worker_.join(); }
private:
    mutable std::mutex mx_; SmartReport rep_; std::thread worker_;
};
inline SmartCache& Smart() { static SmartCache c; return c; }

}   // namespace ak
