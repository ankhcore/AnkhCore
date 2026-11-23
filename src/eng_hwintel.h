// eng_hwintel.h - M-2.6-HW1: the normalized hardware inventory and capability classification.
//
// One rule shapes this file: it POLLS NOTHING. It never opens a sensor, a counter, the registry or a file. Everything arrives in
// `HwInputs`, which the application fills from data it already has (the SensorHub readings, the engine frame, Windows strings read
// once). That is how "no duplicate sensor polling" is kept: a second consumer of the same data adds no sampler.
//
// What it adds on top of the raw readings:
//   * a stable device identity (SHA-256 of the normalized manufacturer / model / board; serial numbers and UUIDs are never used);
//   * for every value: where it came from (Windows or the sensor helper or a vendor BIOS interface) and whether it is available,
//     not reported, stale (helper installed but silent) or missing (helper not installed);
//   * for every fan: TELEMETRY and CONTROL classified separately. A readable RPM does not prove the fan can be controlled, and a
//     control the helper reports is only "reported, unverified" until a verified device profile exists (none exists in 2.6), so
//     `fanControlExposable` is false for every device today. This file offers no control; it only classifies.
//   * a plain list of limitations, and a deterministic text / JSON report (no secrets, no serial numbers).
#pragma once
#include "base.h"
#include "eng_sensors.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace ak {

enum class HwSource { None, Windows, SensorHelper, VendorBios };
inline const wchar_t* HwSourceLabel(HwSource s) {
    switch (s) {
    case HwSource::Windows: return L"Windows";
    case HwSource::SensorHelper: return L"sensor helper (LibreHardwareMonitor)";
    case HwSource::VendorBios: return L"vendor BIOS interface";
    default: return L"none";
    }
}

enum class HwAvail {
    Available,       // a value was reported and is current
    NotReported,     // the source is working but this hardware or driver does not expose the value
    Stale,           // the helper is installed but is not reporting right now: nothing is known
    HelperMissing,   // the helper is not installed
    NoData           // a Windows-sourced value that has not arrived yet
};
inline const wchar_t* HwAvailLabel(HwAvail a) {
    switch (a) {
    case HwAvail::Available: return L"available";
    case HwAvail::NotReported: return L"not reported by this hardware";
    case HwAvail::Stale: return L"helper not reporting";
    case HwAvail::HelperMissing: return L"helper not installed";
    default: return L"no data yet";
    }
}

struct HwMetric {
    std::wstring name, unit;
    HwSource source = HwSource::None;
    HwAvail avail = HwAvail::NoData;
    double value = -1;
};

enum class HwTelemetry { None, Live, Frozen, Unknown };   // Frozen: a vendor value that has never been seen to change
enum class HwControl {
    Unknown,              // the helper did not classify it
    None,                 // read-only: monitoring only, or a vendor read-only interface
    ReportedUnverified,   // the helper says a control exists; no verified device profile backs it, so it is NOT offered
    Verified              // a verified device profile exists (not reachable in 2.6: there is no profile source yet)
};
inline const wchar_t* HwTelemetryLabel(HwTelemetry t) {
    switch (t) { case HwTelemetry::Live: return L"RPM readable"; case HwTelemetry::Frozen: return L"RPM readable but never seen to change"; case HwTelemetry::None: return L"no RPM"; default: return L"unknown"; }
}
inline const wchar_t* HwControlLabel(HwControl c) {
    switch (c) {
    case HwControl::None: return L"read-only (no control)";
    case HwControl::ReportedUnverified: return L"control reported, not verified";
    case HwControl::Verified: return L"control verified";
    default: return L"control unknown";
    }
}

struct HwFan {
    std::wstring name;
    HwSource source = HwSource::SensorHelper;
    HwTelemetry telemetry = HwTelemetry::Unknown;
    HwControl control = HwControl::Unknown;
    double rpm = -1, minPercent = -1, maxPercent = -1;
    std::wstring note;
};

struct HwDeviceId {
    std::wstring manufacturer, model, board, biosVersion, biosDate;
    std::string identityHash;     // SHA-256 hex of the normalized manufacturer|model|board; empty when neither model nor board is known
    bool complete = false;        // a model or a board name is known, so the hash identifies something
};

struct HwGpu { std::wstring name; uint64_t vramBytes = 0; HwMetric load; };
struct HwDrive { wchar_t letter = 0; uint64_t total = 0, free = 0; };

struct HwInputs {
    std::wstring os, sysManufacturer, sysProduct, boardManufacturer, boardProduct, biosVersion, biosDate, cpuName, helperModel;
    int physCores = 0, logicalCores = 0;
    double cpuLoadWindows = -1, ramLoadWindows = -1;
    uint64_t ramTotal = 0;
    struct Gpu { std::wstring name; uint64_t vram = 0; double util = -1; bool software = false; };
    std::vector<Gpu> gpus;
    std::vector<HwDrive> drives;
    int networkAdapters = 0;
    Readings readings;            // the SensorHub frame as it is; never re-read here
    bool helperInstalled = false;
    bool deviceProfileVerified = false;   // true only when a verified profile for THIS device exists (2.6: nothing sets it)
};

struct HwCapabilities {
    int fansWithTelemetry = 0, fansControlReported = 0, fansControlVerified = 0, fansReadOnly = 0, fansUnknown = 0;
    bool fanControlExposable = false;      // AnkhCore Auto / Manual may be offered: needs a Verified control on a fan
    std::vector<std::wstring> limitations;
};

struct HwInventory {
    HwDeviceId device;
    std::wstring os;
    std::wstring cpuName; int physCores = 0, logicalCores = 0;
    HwMetric cpuLoad, cpuTemp, cpuClock, cpuPower;
    std::vector<HwGpu> gpus;
    uint64_t ramTotal = 0; HwMetric ramLoad;
    std::vector<HwDrive> drives; HwMetric diskTemp;
    int networkAdapters = 0;
    HwMetric battery;                      // no battery source exists in AnkhCore yet: always honestly "no data"
    std::vector<HwFan> fans;
    HwAvail sensorHelper = HwAvail::HelperMissing;   // Available / Stale / HelperMissing: the state of the one source of temperatures, clocks, power and fans
    HwCapabilities caps;
};

// ---------------------------------------------------------------- building blocks (pure)
inline std::string HwSha256Hex(const std::string& s) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr; uint8_t d[32]{};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(h, reinterpret_cast<PUCHAR>(const_cast<char*>(s.data())), static_cast<ULONG>(s.size()), 0) == 0 && BCryptFinishHash(h, d, 32, 0) == 0) {
                static const char* x = "0123456789abcdef";
                for (uint8_t b : d) { hex += x[b >> 4]; hex += x[b & 15]; }
            }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

// lower case, single spaces, trimmed: "Dell Inc. " and "dell  inc." are the same device
inline std::wstring HwNormalize(const std::wstring& s) {
    std::wstring out; bool space = true;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0) { if (!space) out.push_back(L' '); space = true; }
        else { out.push_back(static_cast<wchar_t>(towlower(c))); space = false; }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

inline HwDeviceId HwMakeDeviceId(const HwInputs& in) {
    HwDeviceId d;
    d.manufacturer = in.sysManufacturer; d.model = in.sysProduct.empty() ? in.helperModel : in.sysProduct;
    d.board = in.boardProduct; d.biosVersion = in.biosVersion; d.biosDate = in.biosDate;
    d.complete = !HwNormalize(d.model).empty() || !HwNormalize(d.board).empty();
    if (d.complete) d.identityHash = HwSha256Hex(WToUtf8(HwNormalize(d.manufacturer) + L"|" + HwNormalize(d.model) + L"|" + HwNormalize(in.boardManufacturer) + L"|" + HwNormalize(d.board)));
    return d;
}

// A value only the helper can supply (temperature, clock, power, disk temperature, fans).
inline HwMetric HwHelperMetric(const wchar_t* name, const wchar_t* unit, double v, const HwInputs& in) {
    HwMetric m; m.name = name; m.unit = unit; m.source = HwSource::SensorHelper;
    if (!in.helperInstalled) m.avail = HwAvail::HelperMissing;
    else if (!in.readings.present) m.avail = HwAvail::Stale;
    else if (!(v >= 0.0) || !std::isfinite(v)) m.avail = HwAvail::NotReported;
    else { m.avail = HwAvail::Available; m.value = v; }
    return m;
}
// A value Windows supplies (counters, strings). A missing value is "no data yet", never a zero.
inline HwMetric HwWindowsMetric(const wchar_t* name, const wchar_t* unit, double v) {
    HwMetric m; m.name = name; m.unit = unit; m.source = HwSource::Windows;
    if (v >= 0.0 && std::isfinite(v)) { m.avail = HwAvail::Available; m.value = v; } else m.avail = HwAvail::NoData;
    return m;
}

// Telemetry and control are classified SEPARATELY. A readable RPM says nothing about control; a reported control is not a verified one.
inline HwFan HwClassifyFan(const FanReading& f, const HwInputs& in) {
    HwFan o; o.name = f.name.empty() ? std::wstring(L"Fan") : f.name;
    o.source = f.source == L"HP BIOS" ? HwSource::VendorBios : HwSource::SensorHelper;
    o.rpm = f.rpm; o.minPercent = f.minPercent; o.maxPercent = f.maxPercent;
    if (!(f.rpm >= 0.0) || !std::isfinite(f.rpm)) o.telemetry = HwTelemetry::None;
    else if (!f.live) o.telemetry = HwTelemetry::Frozen;
    else o.telemetry = HwTelemetry::Live;
    switch (f.capability) {
    case FanCapability::Controllable:
    case FanCapability::ControlOnly:
        o.control = in.deviceProfileVerified ? HwControl::Verified : HwControl::ReportedUnverified; break;
    case FanCapability::MonitorOnly:
    case FanCapability::VendorReadOnly:
        o.control = HwControl::None; break;
    default:
        o.control = f.controllable ? (in.deviceProfileVerified ? HwControl::Verified : HwControl::ReportedUnverified) : HwControl::Unknown; break;
    }
    switch (o.control) {
    case HwControl::ReportedUnverified: o.note = L"The helper reports a writable control, but no verified device profile exists for this device, so AnkhCore does not offer fan control."; break;
    case HwControl::None: o.note = L"Read-only: the speed can be shown but not set."; break;
    case HwControl::Unknown: o.note = L"Whether this fan can be controlled is not known; treated as not controllable."; break;
    default: o.note = L"A verified device profile covers this control."; break;
    }
    if (o.telemetry == HwTelemetry::None && o.control != HwControl::None) o.note += L" It reports no RPM, so a change could not be confirmed.";
    return o;
}

inline HwInventory BuildHwInventory(const HwInputs& in) {
    HwInventory v;
    v.device = HwMakeDeviceId(in);
    v.os = in.os;
    v.cpuName = in.cpuName; v.physCores = in.physCores; v.logicalCores = in.logicalCores;
    v.cpuLoad = HwWindowsMetric(L"CPU load", L"%", in.cpuLoadWindows);
    if (!(v.cpuLoad.avail == HwAvail::Available) && in.readings.present && in.readings.cpuLoad >= 0.0) {   // fall back to the helper's load, labelled as such
        v.cpuLoad = HwHelperMetric(L"CPU load", L"%", in.readings.cpuLoad, in);
    }
    v.cpuTemp = HwHelperMetric(L"CPU temperature", L"C", in.readings.cpuTemp, in);
    v.cpuClock = HwHelperMetric(L"CPU clock", L"MHz", in.readings.cpuClock, in);
    v.cpuPower = HwHelperMetric(L"CPU power", L"W", in.readings.cpuPower, in);
    for (const auto& g : in.gpus) {
        if (g.software) continue;   // the software/basic adapter is not hardware
        HwGpu o; o.name = g.name; o.vramBytes = g.vram; o.load = HwWindowsMetric(L"GPU load", L"%", g.util);
        v.gpus.push_back(o);
    }
    v.ramTotal = in.ramTotal; v.ramLoad = HwWindowsMetric(L"Memory load", L"%", in.ramLoadWindows);
    v.drives = in.drives; v.diskTemp = HwHelperMetric(L"Disk temperature", L"C", in.readings.diskTemp, in);
    v.networkAdapters = in.networkAdapters;
    v.battery.name = L"Battery"; v.battery.source = HwSource::None; v.battery.avail = HwAvail::NoData;
    v.sensorHelper = !in.helperInstalled ? HwAvail::HelperMissing : in.readings.present ? HwAvail::Available : HwAvail::Stale;
    for (const FanReading& f : in.readings.fans) v.fans.push_back(HwClassifyFan(f, in));
    HwCapabilities& c = v.caps;
    for (const HwFan& f : v.fans) {
        if (f.telemetry == HwTelemetry::Live) ++c.fansWithTelemetry;
        switch (f.control) {
        case HwControl::Verified: ++c.fansControlVerified; break;
        case HwControl::ReportedUnverified: ++c.fansControlReported; break;
        case HwControl::None: ++c.fansReadOnly; break;
        default: ++c.fansUnknown; break;
        }
    }
    // Only a verified control on a fan whose RPM can be read back (so a command is provable) makes control offerable.
    for (const HwFan& f : v.fans) if (f.control == HwControl::Verified && f.telemetry == HwTelemetry::Live) c.fanControlExposable = true;
    if (!c.fanControlExposable) c.limitations.push_back(L"Fan control is not offered: no fan has a verified device profile with readable RPM feedback. Fans stay under BIOS / firmware control.");
    if (c.fansControlReported > 0) c.limitations.push_back(L"The sensor helper reports " + std::to_wstring(c.fansControlReported) + L" control(s); a reported control is not proof it is safe or that it works on this device.");
    if (v.fans.empty()) c.limitations.push_back(L"No fan sensor is exposed by this hardware or reported by the helper.");
    if (v.sensorHelper == HwAvail::HelperMissing) c.limitations.push_back(L"The sensor helper is not installed: temperatures, clocks, power and fan data are unavailable.");
    else if (v.sensorHelper == HwAvail::Stale) c.limitations.push_back(L"The sensor helper is installed but not reporting: helper-sourced values are unknown, not zero.");
    if (!v.device.complete) c.limitations.push_back(L"The device model could not be read, so the device has no stable identity.");
    c.limitations.push_back(L"No battery information source exists yet.");
    return v;
}

// ---------------------------------------------------------------- reports
inline std::wstring HwMetricText(const HwMetric& m) {
    if (m.avail != HwAvail::Available) return std::wstring(HwAvailLabel(m.avail));
    wchar_t b[48]; swprintf(b, 48, L"%.0f %ls", m.value, m.unit.c_str());
    return b;
}

// Plain text, one block per area, in a fixed order. No serial numbers, no machine name, no user name.
inline std::wstring HwInventoryText(const HwInventory& v) {
    std::wstring t;
    auto line = [&](const std::wstring& k, const std::wstring& val) { t += k + L": " + (val.empty() ? std::wstring(L"not reported") : val) + L"\r\n"; };
    t += L"== Device ==\r\n";
    line(L"Operating system", v.os); line(L"Manufacturer", v.device.manufacturer); line(L"Model", v.device.model); line(L"Board", v.device.board);
    line(L"BIOS", v.device.biosVersion + (v.device.biosDate.empty() ? std::wstring() : L" (" + v.device.biosDate + L")"));
    line(L"Identity (SHA-256)", Utf8ToW(v.device.identityHash));
    t += L"\r\n== Processor ==\r\n";
    line(L"Name", v.cpuName); line(L"Cores / threads", std::to_wstring(v.physCores) + L" / " + std::to_wstring(v.logicalCores));
    for (const HwMetric* m : {&v.cpuLoad, &v.cpuTemp, &v.cpuClock, &v.cpuPower}) line(m->name, HwMetricText(*m) + L"  [" + HwSourceLabel(m->source) + L"]");
    t += L"\r\n== Graphics ==\r\n";
    if (v.gpus.empty()) t += L"No hardware adapter reported.\r\n";
    for (const HwGpu& g : v.gpus) line(g.name, g.vramBytes ? std::to_wstring(g.vramBytes >> 20) + L" MiB VRAM, " + HwMetricText(g.load) : HwMetricText(g.load));
    t += L"\r\n== Memory and storage ==\r\n";
    line(L"Installed memory", v.ramTotal ? std::to_wstring(v.ramTotal >> 20) + L" MiB" : std::wstring());
    line(v.ramLoad.name, HwMetricText(v.ramLoad));
    line(L"Drives", std::to_wstring(v.drives.size())); line(v.diskTemp.name, HwMetricText(v.diskTemp) + L"  [" + HwSourceLabel(v.diskTemp.source) + L"]");
    line(L"Network adapters", std::to_wstring(v.networkAdapters)); line(L"Battery", HwMetricText(v.battery));
    t += L"\r\n== Fans (telemetry and control are separate) ==\r\n";
    line(L"Sensor helper", HwAvailLabel(v.sensorHelper));
    if (v.fans.empty()) t += L"No fan reported.\r\n";
    for (const HwFan& f : v.fans) {
        line(f.name, std::wstring(HwTelemetryLabel(f.telemetry)) + (f.rpm >= 0 ? L" (" + std::to_wstring(static_cast<int>(f.rpm)) + L" rpm)" : std::wstring()) + L"; " + HwControlLabel(f.control) + L"; source: " + HwSourceLabel(f.source));
    }
    line(L"Fan control offered", v.caps.fanControlExposable ? L"yes" : L"no");
    t += L"\r\n== Limitations ==\r\n";
    for (const auto& l : v.caps.limitations) t += L"- " + l + L"\r\n";
    return t;
}

inline std::string HwJsonEsc(const std::wstring& w) {
    const std::string s = WToUtf8(w); std::string o;
    for (const unsigned char c : s) {
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += static_cast<char>(c);
    }
    return o;
}
inline std::string HwMetricJson(const HwMetric& m) {
    std::string j = "{\"name\": \"" + HwJsonEsc(m.name) + "\", \"source\": \"" + HwJsonEsc(HwSourceLabel(m.source)) + "\", \"availability\": \"" + HwJsonEsc(HwAvailLabel(m.avail)) + "\"";
    if (m.avail == HwAvail::Available) { char b[40]; snprintf(b, sizeof(b), "%.2f", m.value); j += std::string(", \"value\": ") + b + ", \"unit\": \"" + HwJsonEsc(m.unit) + "\""; }
    return j + "}";
}
// Versioned, deterministic, no personal data. `identity` is what a device profile (M-2.6-CFG1) will be keyed on.
inline std::string HwInventoryJson(const HwInventory& v) {
    std::string j = "{\n  \"schema\": \"ankhcore.hardware-inventory\",\n  \"version\": 1,\n";
    j += "  \"device\": {\"manufacturer\": \"" + HwJsonEsc(v.device.manufacturer) + "\", \"model\": \"" + HwJsonEsc(v.device.model) + "\", \"board\": \"" + HwJsonEsc(v.device.board) + "\", \"biosVersion\": \"" + HwJsonEsc(v.device.biosVersion) +
         "\", \"identity\": \"" + v.device.identityHash + "\", \"identityComplete\": " + (v.device.complete ? "true" : "false") + "},\n";
    j += "  \"sensorHelper\": \"" + HwJsonEsc(HwAvailLabel(v.sensorHelper)) + "\",\n";
    j += "  \"cpu\": {\"name\": \"" + HwJsonEsc(v.cpuName) + "\", \"cores\": " + std::to_string(v.physCores) + ", \"threads\": " + std::to_string(v.logicalCores) + ", \"metrics\": [" +
         HwMetricJson(v.cpuLoad) + ", " + HwMetricJson(v.cpuTemp) + ", " + HwMetricJson(v.cpuClock) + ", " + HwMetricJson(v.cpuPower) + "]},\n";
    j += "  \"gpus\": [";
    for (size_t i = 0; i < v.gpus.size(); ++i) j += std::string(i ? ", " : "") + "{\"name\": \"" + HwJsonEsc(v.gpus[i].name) + "\", \"vramBytes\": " + std::to_string(v.gpus[i].vramBytes) + ", \"load\": " + HwMetricJson(v.gpus[i].load) + "}";
    j += "],\n  \"memory\": {\"totalBytes\": " + std::to_string(v.ramTotal) + ", \"load\": " + HwMetricJson(v.ramLoad) + "},\n";
    j += "  \"storage\": {\"drives\": " + std::to_string(v.drives.size()) + ", \"temperature\": " + HwMetricJson(v.diskTemp) + "},\n";
    j += "  \"networkAdapters\": " + std::to_string(v.networkAdapters) + ",\n";
    j += "  \"fans\": [";
    for (size_t i = 0; i < v.fans.size(); ++i) {
        const HwFan& f = v.fans[i];
        j += std::string(i ? ", " : "") + "{\"name\": \"" + HwJsonEsc(f.name) + "\", \"source\": \"" + HwJsonEsc(HwSourceLabel(f.source)) + "\", \"telemetry\": \"" + HwJsonEsc(HwTelemetryLabel(f.telemetry)) +
             "\", \"control\": \"" + HwJsonEsc(HwControlLabel(f.control)) + "\"";
        if (f.rpm >= 0) j += ", \"rpm\": " + std::to_string(static_cast<int>(f.rpm));
        j += "}";
    }
    j += "],\n  \"capabilities\": {\"fanControlOffered\": " + std::string(v.caps.fanControlExposable ? "true" : "false") + ", \"fansWithTelemetry\": " + std::to_string(v.caps.fansWithTelemetry) +
         ", \"fansControlReported\": " + std::to_string(v.caps.fansControlReported) + ", \"fansControlVerified\": " + std::to_string(v.caps.fansControlVerified) + ", \"fansReadOnly\": " + std::to_string(v.caps.fansReadOnly) + ", \"fansUnknown\": " + std::to_string(v.caps.fansUnknown) + "},\n";
    j += "  \"limitations\": [";
    for (size_t i = 0; i < v.caps.limitations.size(); ++i) j += std::string(i ? ", " : "") + "\"" + HwJsonEsc(v.caps.limitations[i]) + "\"";
    j += "]\n}\n";
    return j;
}

}  // namespace ak
