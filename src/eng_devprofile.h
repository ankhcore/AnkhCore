// eng_devprofile.h - M-2.6-CFG1: the device profile (schema, generator, strict validation, storage).
//
// A device profile DESCRIBES what a device is and what was observed about it. It is generated from the normalized hardware
// inventory (eng_hwintel.h) and never from a similar model name. By construction it cannot hold a low-level fan/EC control
// configuration: the schema has no field for registers, commands, curves or NBFC config content, and the validator rejects any key
// it does not know. Actuator configuration needs separate, device-specific verification (MASTER_PROMPT 4.4) and is not part of this.
//
//   * identity: the SHA-256 identity of the inventory (manufacturer|model|board manufacturer|board). A profile applies to exactly one
//     identity; "similar name" never matches (DpMatchDevice compares the hash only).
//   * provenance: generator, UTC time and the SHA-256 of the inventory the profile was generated from.
//   * integrity: `hash` = SHA-256 of the canonical body. It detects accidental damage and edits; it does NOT authenticate who wrote
//     the file (there is no key). So a file alone can never make a control "verified": see DpVerifiedFor.
//   * verification: a control may be marked "verified" only with evidence of a successful, attributable Try Fan Compatibility trial
//     (RPM response, safe state restored) AND a verified safe release to firmware. The generator never produces either; nothing in
//     2.6 runs a trial, so every profile AnkhCore generates says `control-reported-unverified` or `monitor-only` at best.
#pragma once
#include "base.h"
#include "json_min.h"
#include "eng_hwintel.h"
#include "eng_fan.h"

namespace ak {

constexpr size_t kDpMaxBytes = 256u << 10;
constexpr size_t kDpMaxFans = 16;
constexpr int kDpVersion = 1;

struct DpFan {
    std::wstring name;
    int channel = 0;
    std::string role = "unknown";      // "cpu" | "gpu" | "unknown": only when the fan's own name says so, never from its index
    HwTelemetry telemetry = HwTelemetry::Unknown;
    HwControl control = HwControl::Unknown;
    int minPercent = -1, maxPercent = -1;   // limits REPORTED by the sensor helper; -1 = not reported
};

struct DpEvidence {
    std::string kind = "fan-compat-trial";
    std::wstring candidate;
    std::string configSha256;
    std::string verdict;               // must be "compatible"
    double baselineRpm = -1, peakRpm = -1;
    bool restoredSafeState = false;
    std::string recordedUtc;
};

struct DeviceProfile {
    int version = kDpVersion;
    std::string generatedUtc, generator, inventorySha256;
    std::string identity;
    std::wstring manufacturer, model, board;
    std::vector<DpFan> fans;
    std::vector<DpEvidence> evidence;
    bool safeReleaseVerified = false;
    std::string compatibility;         // derived from the fans, never stored independently of them
    std::string hash;                  // SHA-256 of the canonical body
};

inline const char* DpTelemetryName(HwTelemetry t) { return t == HwTelemetry::Live ? "live" : t == HwTelemetry::Frozen ? "frozen" : t == HwTelemetry::None ? "none" : "unknown"; }
inline const char* DpControlName(HwControl c) { return c == HwControl::Verified ? "verified" : c == HwControl::ReportedUnverified ? "reported-unverified" : c == HwControl::None ? "none" : "unknown"; }
inline bool DpParseTelemetry(const std::string& s, HwTelemetry& o) { for (HwTelemetry t : {HwTelemetry::Live, HwTelemetry::Frozen, HwTelemetry::None, HwTelemetry::Unknown}) if (s == DpTelemetryName(t)) { o = t; return true; } return false; }
inline bool DpParseControl(const std::string& s, HwControl& o) { for (HwControl c : {HwControl::Verified, HwControl::ReportedUnverified, HwControl::None, HwControl::Unknown}) if (s == DpControlName(c)) { o = c; return true; } return false; }

inline std::string DpCompatibility(const std::vector<DpFan>& fans) {
    if (fans.empty()) return "no-fans";
    bool anyVerified = false, anyReported = false, anyReadOnly = false;
    for (const DpFan& f : fans) { anyVerified |= f.control == HwControl::Verified; anyReported |= f.control == HwControl::ReportedUnverified; anyReadOnly |= f.control == HwControl::None; }
    if (anyVerified) return "control-verified";
    if (anyReported) return "control-reported-unverified";
    if (anyReadOnly) return "monitor-only";
    return "unknown";
}

inline bool DpValidHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const unsigned char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// Names that reach a file and a screen: printable, bounded, no control characters.
inline bool DpValidText(const std::wstring& w, size_t maxLen, bool allowEmpty) {
    if (w.empty()) return allowEmpty;
    if (w.size() > maxLen) return false;
    for (wchar_t c : w) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
inline bool DpValidUtc(const std::string& s) {   // 2026-11-19T10:20:30Z or the compact stamp AnkhCore writes; conservative charset only
    if (s.empty() || s.size() > 32) return false;
    for (const unsigned char c : s) if (!((c >= '0' && c <= '9') || c == '-' || c == ':' || c == 'T' || c == 'Z' || c == '_' || c == '.')) return false;
    return true;
}

// ---------------------------------------------------------------- canonical writer
inline std::string DpBodyJson(const DeviceProfile& p) {
    std::string j = "{\n  \"schema\": \"ankhcore.device-profile\",\n  \"version\": " + std::to_string(p.version) + ",\n";
    j += "  \"generatedUtc\": \"" + p.generatedUtc + "\",\n  \"generator\": \"" + p.generator + "\",\n  \"inventorySha256\": \"" + p.inventorySha256 + "\",\n";
    j += "  \"device\": {\"identity\": \"" + p.identity + "\", \"manufacturer\": \"" + HwJsonEsc(p.manufacturer) + "\", \"model\": \"" + HwJsonEsc(p.model) + "\", \"board\": \"" + HwJsonEsc(p.board) + "\"},\n";
    j += "  \"fans\": [";
    for (size_t i = 0; i < p.fans.size(); ++i) {
        const DpFan& f = p.fans[i];
        j += std::string(i ? ", " : "") + "{\"name\": \"" + HwJsonEsc(f.name) + "\", \"channel\": " + std::to_string(f.channel) + ", \"role\": \"" + f.role + "\", \"telemetry\": \"" + DpTelemetryName(f.telemetry) +
             "\", \"control\": \"" + DpControlName(f.control) + "\", \"minPercent\": " + std::to_string(f.minPercent) + ", \"maxPercent\": " + std::to_string(f.maxPercent) + "}";
    }
    j += "],\n  \"compatibility\": \"" + DpCompatibility(p.fans) + "\",\n";
    char b1[40], b2[40];
    j += "  \"verification\": {\"safeReleaseVerified\": " + std::string(p.safeReleaseVerified ? "true" : "false") + ", \"evidence\": [";
    for (size_t i = 0; i < p.evidence.size(); ++i) {
        const DpEvidence& e = p.evidence[i];
        snprintf(b1, sizeof(b1), "%.0f", e.baselineRpm); snprintf(b2, sizeof(b2), "%.0f", e.peakRpm);
        j += std::string(i ? ", " : "") + "{\"kind\": \"" + e.kind + "\", \"candidate\": \"" + HwJsonEsc(e.candidate) + "\", \"configSha256\": \"" + e.configSha256 + "\", \"verdict\": \"" + e.verdict +
             "\", \"baselineRpm\": " + b1 + ", \"peakRpm\": " + b2 + ", \"restoredSafeState\": " + (e.restoredSafeState ? "true" : "false") + ", \"recordedUtc\": \"" + e.recordedUtc + "\"}";
    }
    j += "]},\n  \"safeDefaults\": {\"mode\": \"bios-auto\"}";
    return j;
}
inline std::string DpComputeHash(const DeviceProfile& p) { return HwSha256Hex(DpBodyJson(p)); }
inline std::string DpToJson(const DeviceProfile& p) { return DpBodyJson(p) + ",\n  \"hash\": \"" + DpComputeHash(p) + "\"\n}\n"; }

// ---------------------------------------------------------------- generator (from the inventory only)
// Never produces a verified control or any evidence: whatever the inventory says is capped at "reported, unverified".
inline DeviceProfile DpGenerate(const HwInventory& inv, const std::string& utcStamp, const std::string& generator) {
    DeviceProfile p;
    p.generatedUtc = utcStamp; p.generator = generator;
    p.inventorySha256 = HwSha256Hex(HwInventoryJson(inv));
    p.identity = inv.device.identityHash;
    p.manufacturer = inv.device.manufacturer; p.model = inv.device.model; p.board = inv.device.board;
    int ch = 0;
    for (const HwFan& f : inv.fans) {
        DpFan o; o.name = f.name; o.channel = ch++;
        const std::wstring low = Lower(f.name);
        const bool cpu = low.find(L"cpu") != std::wstring::npos, gpu = low.find(L"gpu") != std::wstring::npos;
        o.role = cpu && !gpu ? "cpu" : gpu && !cpu ? "gpu" : "unknown";
        o.telemetry = f.telemetry;
        o.control = f.control == HwControl::Verified ? HwControl::ReportedUnverified : f.control;
        if (f.minPercent >= 0 && f.maxPercent >= f.minPercent && f.maxPercent <= 100) { o.minPercent = static_cast<int>(f.minPercent); o.maxPercent = static_cast<int>(f.maxPercent); }
        if (o.name.size() > 64) o.name.resize(64);
        p.fans.push_back(o);
        if (p.fans.size() >= kDpMaxFans) break;
    }
    p.compatibility = DpCompatibility(p.fans);
    p.hash = DpComputeHash(p);
    return p;
}

// ---------------------------------------------------------------- strict validation
namespace dpdetail {
inline bool OnlyKeys(const JVal& o, std::initializer_list<const char*> keys) {
    for (const std::string& k : o.keys) { bool ok = false; for (const char* a : keys) if (k == a) ok = true; if (!ok) return false; }
    return true;
}
inline bool Str(const JVal& o, const char* k, std::string& out) { const JVal* v = o.Get(k); if (!v || !v->Is(JVal::Str)) return false; out = v->s; return true; }
inline bool Whole(const JVal& o, const char* k, int lo, int hi, int& out) {
    const JVal* v = o.Get(k); if (!v || !v->Is(JVal::Num)) return false;
    const double d = v->n; if (!(d >= lo && d <= hi) || d != static_cast<double>(static_cast<long long>(d))) return false;
    out = static_cast<int>(d); return true;
}
}  // namespace dpdetail

inline bool DpEvidenceValid(const DpEvidence& e, std::wstring& why) {
    if (e.kind != "fan-compat-trial") { why = L"unknown evidence kind"; return false; }
    if (e.verdict != "compatible") { why = L"the trial verdict is not 'compatible'"; return false; }
    if (!e.restoredSafeState) { why = L"the trial did not restore a safe state"; return false; }
    if (!DpValidText(e.candidate, 128, false)) { why = L"the evidence has no valid candidate name"; return false; }
    if (!DpValidHex64(e.configSha256)) { why = L"the evidence has no valid configuration hash"; return false; }
    if (!DpValidUtc(e.recordedUtc)) { why = L"the evidence has no valid time"; return false; }
    const FanCompatTrial limits;   // the thresholds the trial itself uses
    if (!(e.baselineRpm >= 0) || !(e.peakRpm >= limits.minActiveRpm) || !(e.peakRpm - e.baselineRpm >= limits.minDeltaRpm)) { why = L"the RPM response is below the trial's own thresholds"; return false; }
    return true;
}

// Parses and validates a profile. Refuses (and says why): oversized or non-strict JSON, a wrong schema or version, any unknown key at any level
// (so no EC/register/curve data can ride along), invalid identity/names/ranges/enums, a wrong hash, and any claim of a verified control
// without valid evidence and a verified safe release.
inline bool DpValidate(const std::string& text, DeviceProfile& out, std::wstring& why) {
    using namespace dpdetail;
    out = DeviceProfile{};
    if (text.empty()) { why = L"The profile is empty."; return false; }
    if (text.size() > kDpMaxBytes) { why = L"The profile is larger than 256 KiB."; return false; }
    JVal root;
    if (!JsonParse(text, root) || !root.Is(JVal::Obj)) { why = L"The profile is not strictly valid JSON."; return false; }
    if (!OnlyKeys(root, {"schema", "version", "generatedUtc", "generator", "inventorySha256", "device", "fans", "compatibility", "verification", "safeDefaults", "hash"})) { why = L"The profile has a field this version does not define."; return false; }
    std::string schema; int ver = 0;
    if (!Str(root, "schema", schema) || schema != "ankhcore.device-profile") { why = L"This is not an AnkhCore device profile."; return false; }
    if (!Whole(root, "version", 0, 1000000, ver) || ver != kDpVersion) { why = L"The profile version is not supported."; return false; }
    DeviceProfile p; p.version = ver;
    if (!Str(root, "generatedUtc", p.generatedUtc) || !DpValidUtc(p.generatedUtc)) { why = L"The generation time is missing or invalid."; return false; }
    if (!Str(root, "generator", p.generator) || p.generator.empty() || p.generator.size() > 64) { why = L"The generator name is missing or invalid."; return false; }
    for (const unsigned char c : p.generator) if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') { why = L"The generator name has invalid characters."; return false; }
    if (!Str(root, "inventorySha256", p.inventorySha256) || !DpValidHex64(p.inventorySha256)) { why = L"The inventory hash is missing or invalid."; return false; }
    const JVal* dev = root.Get("device");
    if (!dev || !dev->Is(JVal::Obj) || !OnlyKeys(*dev, {"identity", "manufacturer", "model", "board"})) { why = L"The device section is missing or has an unknown field."; return false; }
    std::string m, md, bd;
    if (!Str(*dev, "identity", p.identity) || !DpValidHex64(p.identity)) { why = L"The device identity is missing or is not a SHA-256."; return false; }
    if (!Str(*dev, "manufacturer", m) || !Str(*dev, "model", md) || !Str(*dev, "board", bd)) { why = L"The device names are missing."; return false; }
    p.manufacturer = Utf8ToW(m); p.model = Utf8ToW(md); p.board = Utf8ToW(bd);
    if (!DpValidText(p.manufacturer, 128, true) || !DpValidText(p.model, 128, true) || !DpValidText(p.board, 128, true)) { why = L"A device name is too long or has control characters."; return false; }
    const JVal* fans = root.Get("fans");
    if (!fans || !fans->Is(JVal::Arr)) { why = L"The fan list is missing."; return false; }
    if (fans->a.size() > kDpMaxFans) { why = L"The profile lists too many fans."; return false; }
    std::set<int> channels;
    for (const JVal& f : fans->a) {
        if (!f.Is(JVal::Obj) || !OnlyKeys(f, {"name", "channel", "role", "telemetry", "control", "minPercent", "maxPercent"})) { why = L"A fan entry is malformed or has an unknown field."; return false; }
        DpFan o; std::string n, tel, ctl;
        if (!Str(f, "name", n) || !Str(f, "role", o.role) || !Str(f, "telemetry", tel) || !Str(f, "control", ctl)) { why = L"A fan entry is missing a field."; return false; }
        o.name = Utf8ToW(n);
        if (!DpValidText(o.name, 64, false)) { why = L"A fan name is empty, too long or has control characters."; return false; }
        if (o.role != "cpu" && o.role != "gpu" && o.role != "unknown") { why = L"A fan role is not cpu, gpu or unknown."; return false; }
        if (!DpParseTelemetry(tel, o.telemetry) || !DpParseControl(ctl, o.control)) { why = L"A fan telemetry or control class is not one this version knows."; return false; }
        if (!Whole(f, "channel", 0, 255, o.channel) || !channels.insert(o.channel).second) { why = L"A fan channel is invalid or repeated."; return false; }
        if (!Whole(f, "minPercent", -1, 100, o.minPercent) || !Whole(f, "maxPercent", -1, 100, o.maxPercent)) { why = L"A fan limit is out of range."; return false; }
        if ((o.minPercent < 0) != (o.maxPercent < 0) || (o.minPercent >= 0 && o.minPercent > o.maxPercent)) { why = L"A fan's limits are inconsistent."; return false; }
        p.fans.push_back(o);
    }
    std::string comp;
    if (!Str(root, "compatibility", comp) || comp != DpCompatibility(p.fans)) { why = L"The stored compatibility class does not match the fans."; return false; }
    p.compatibility = comp;
    const JVal* ver2 = root.Get("verification");
    if (!ver2 || !ver2->Is(JVal::Obj) || !OnlyKeys(*ver2, {"safeReleaseVerified", "evidence"})) { why = L"The verification section is missing or has an unknown field."; return false; }
    const JVal* sr = ver2->Get("safeReleaseVerified"); const JVal* ev = ver2->Get("evidence");
    if (!sr || !sr->Is(JVal::Bool) || !ev || !ev->Is(JVal::Arr) || ev->a.size() > 8) { why = L"The verification section is malformed."; return false; }
    p.safeReleaseVerified = sr->b;
    for (const JVal& e : ev->a) {
        if (!e.Is(JVal::Obj) || !OnlyKeys(e, {"kind", "candidate", "configSha256", "verdict", "baselineRpm", "peakRpm", "restoredSafeState", "recordedUtc"})) { why = L"An evidence entry is malformed or has an unknown field."; return false; }
        DpEvidence d; std::string cand; const JVal* b = e.Get("baselineRpm"); const JVal* pk = e.Get("peakRpm"); const JVal* rs = e.Get("restoredSafeState");
        if (!Str(e, "kind", d.kind) || !Str(e, "candidate", cand) || !Str(e, "configSha256", d.configSha256) || !Str(e, "verdict", d.verdict) || !Str(e, "recordedUtc", d.recordedUtc) ||
            !b || !b->Is(JVal::Num) || !pk || !pk->Is(JVal::Num) || !rs || !rs->Is(JVal::Bool)) { why = L"An evidence entry is missing a field."; return false; }
        d.candidate = Utf8ToW(cand); d.baselineRpm = b->n; d.peakRpm = pk->n; d.restoredSafeState = rs->b;
        p.evidence.push_back(d);
    }
    const JVal* sd = root.Get("safeDefaults");
    if (!sd || !sd->Is(JVal::Obj) || !OnlyKeys(*sd, {"mode"}) || !sd->Get("mode") || sd->Get("mode")->s != "bios-auto") { why = L"The safe default must be BIOS auto."; return false; }
    bool anyVerified = false; for (const DpFan& f : p.fans) anyVerified |= f.control == HwControl::Verified;
    if (anyVerified) {   // a "verified" claim needs real evidence AND a verified safe release
        std::wstring ew;
        if (p.evidence.empty()) { why = L"A control is marked verified but the profile has no trial evidence."; return false; }
        for (const DpEvidence& e : p.evidence) if (!DpEvidenceValid(e, ew)) { why = L"A control is marked verified but its evidence is not valid: " + ew; return false; }
        if (!p.safeReleaseVerified) { why = L"A control is marked verified but a safe release to firmware is not verified."; return false; }
    }
    if (!p.evidence.empty() && !anyVerified) { why = L"The profile carries trial evidence but marks no control as verified."; return false; }
    if (!Str(root, "hash", p.hash) || !DpValidHex64(p.hash)) { why = L"The profile has no valid hash."; return false; }
    if (p.hash != DpComputeHash(p)) { why = L"The profile hash does not match its contents (damaged or edited)."; return false; }
    out = p;
    return true;
}

// ---------------------------------------------------------------- matching and verification
enum class DpMatch { Exact, IdentityMismatch, NoIdentity };
// Identity hash only. A similar model name, the same vendor or the same family never matches.
inline DpMatch DpMatchDevice(const DeviceProfile& p, const HwInventory& inv) {
    if (!inv.device.complete || inv.device.identityHash.empty() || p.identity.empty()) return DpMatch::NoIdentity;
    return p.identity == inv.device.identityHash ? DpMatch::Exact : DpMatch::IdentityMismatch;
}
// The ONLY route to `HwInputs::deviceProfileVerified`. A file is not authenticated, so on top of a valid, matching profile with a verified
// control and evidence, the caller must hold a LOCAL record that this machine really ran that trial (`localTrialConfirmed`). AnkhCore keeps
// no such record in 2.6 (no trial has been approved), so the application passes false and this is false for every device.
inline bool DpVerifiedFor(const DeviceProfile& p, const HwInventory& inv, bool localTrialConfirmed) {
    if (!localTrialConfirmed || DpMatchDevice(p, inv) != DpMatch::Exact || !p.safeReleaseVerified || p.evidence.empty()) return false;
    for (const DpFan& f : p.fans) if (f.control == HwControl::Verified) return true;
    return false;
}

// ---------------------------------------------------------------- storage: <dir>\<identity>.json
inline std::wstring DpPathFor(const std::wstring& dir, const std::string& identity) {
    if (!DpValidHex64(identity)) return L"";
    return dir + L"\\" + Utf8ToW(identity) + L".json";
}
inline bool DpSave(const std::wstring& dir, const DeviceProfile& p, std::wstring& why) {
    const std::wstring path = DpPathFor(dir, p.identity);
    if (path.empty()) { why = L"The profile has no valid device identity, so it is not saved."; return false; }
    if (kTestBuild && !InTestArea(path)) { TestRefused(L"device profile " + path); why = L"Test build: refused."; return false; }
    DeviceProfile chk; std::wstring w2; const std::string json = DpToJson(p);
    if (!DpValidate(json, chk, w2)) { why = L"The profile did not validate: " + w2; return false; }   // never write what would not load back
    EnsureDir(dir);
    if (!WriteAtomic(path, reinterpret_cast<const uint8_t*>(json.data()), json.size())) { why = L"The profile could not be written."; return false; }
    return true;
}
inline bool DpLoad(const std::wstring& dir, const std::string& identity, DeviceProfile& out, std::wstring& why) {
    const std::wstring path = DpPathFor(dir, identity);
    if (path.empty()) { why = L"Not a valid device identity."; return false; }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { why = L"There is no profile for this device."; return false; }
    LARGE_INTEGER sz{}; const bool gotSize = GetFileSizeEx(h, &sz) != 0;
    std::string buf;
    if (gotSize && sz.QuadPart > 0 && static_cast<uint64_t>(sz.QuadPart) <= kDpMaxBytes) {
        buf.resize(static_cast<size_t>(sz.QuadPart)); DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) got = 0;
        buf.resize(got);
    }
    const bool tooBig = gotSize && static_cast<uint64_t>(sz.QuadPart) > kDpMaxBytes;
    CloseHandle(h);
    if (tooBig) { why = L"The profile file is larger than 256 KiB."; return false; }
    if (!DpValidate(buf, out, why)) return false;   // the damaged file is left exactly as it is
    if (out.identity != identity) { why = L"The profile inside belongs to another device identity than its file name."; out = DeviceProfile{}; return false; }
    return true;
}

}  // namespace ak
