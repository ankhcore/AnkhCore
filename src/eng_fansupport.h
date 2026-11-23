// eng_fansupport.h - 2.8: what AnkhCore can honestly do with the fans of THIS PC (desktop or any notebook), decided from facts, per model.
//
// Reading (telemetry) and control are separate questions and have separate answers:
//   * Reading sources, in the order AnkhCore tries them: the sensor helper (LibreHardwareMonitor: Super I/O chips, embedded controllers, graphics cards,
//     fan controllers), the HP BIOS WMI interface, the Dell Command | Monitor WMI provider. Where none of them reports a fan, the result says "not exposed"
//     with the reason and the way forward for that maker; it never invents a value.
//   * Control: direct (a channel the sensor helper reports as writable) or configuration based (an NBFC configuration for the model, exact or same family).
//     Control follows the MODEL: no exact or family configuration and no direct channel = monitoring only for that PC.
// Pure function over plain facts (testable without hardware); the page fills the facts from the live readings.
#pragma once
#include "base.h"

namespace ak {

enum class FanReadState { Live, Partial, NotExposed, HelperOff };
enum class FanControlRoute { Direct, AnkhEcActive, AnkhEcExact, AnkhEcFamily, ConfigExact, ConfigFamily, NbfcNoMatch, NbfcMissingMatchExists, NbfcMissing, NoRoute };

struct FanSupportFacts {
    std::wstring manufacturer, model;        // BIOS system manufacturer and the notebook model the helper reported
    bool helperRunning = false;              // the sensor helper is installed and has published readings
    int fans = 0, fansWithRpm = 0;           // fan channels listed / those with a live RPM value
    bool hpBiosFan = false, dellCmFan = false, sensorHelperFan = false;   // which source produced at least one fan value
    bool thinkpadEcFan = false, dellSmmFan = false;                       // 2.8: the PawnIO readers (ThinkPad embedded controller, Dell SMM)
    bool ecActive = false, ecReady = false;   // AnkhCore's own fan-control engine: running now / the embedded-controller module can be loaded on this PC
    bool pawnReadersOff = false;                                           // the person (or the safety guard) switched the PawnIO readers off
    int directControllable = 0;              // channels the sensor helper reports as writable
    bool nbfcInstalled = false;
    int configsExact = 0, configsFamily = 0; // NBFC configurations that match this model exactly / by family (from the model name)
    std::wstring activeConfig;               // the configuration NBFC currently uses, if any
    bool isNotebook = false;                 // from the chassis type when known
};

struct FanSupportReport {
    FanReadState reading = FanReadState::NotExposed;
    FanControlRoute control = FanControlRoute::NoRoute;
    std::wstring maker;                      // normalised maker name, "" when unknown
    std::wstring readingLine, controlLine;   // one honest sentence each
    std::vector<std::wstring> steps;         // what the person can do next (never a promise of something AnkhCore cannot do)
    std::vector<std::wstring> sourcesTried;  // "Sensor helper (LibreHardwareMonitor): fan values", "HP BIOS: not available (not an HP PC)", ...
    bool canControl = false;                 // a control route exists that AnkhCore can use now
};

inline std::wstring FanMakerOf(const std::wstring& manufacturer) {
    std::wstring m = Lower(manufacturer);
    while (!m.empty() && iswspace(m.back())) m.pop_back();
    if (m == L"hp") return L"HP";
    struct P { const wchar_t* key; const wchar_t* name; };
    static const P k[] = {{L"hewlett", L"HP"}, {L"hp inc", L"HP"}, {L"hp ", L"HP"}, {L"dell", L"Dell"}, {L"alienware", L"Dell"}, {L"lenovo", L"Lenovo"}, {L"asus", L"ASUS"}, {L"msi", L"MSI"}, {L"micro-star", L"MSI"},
                          {L"acer", L"Acer"}, {L"gigabyte", L"Gigabyte"}, {L"aorus", L"Gigabyte"}, {L"samsung", L"Samsung"}, {L"toshiba", L"Toshiba"}, {L"dynabook", L"Toshiba"}, {L"razer", L"Razer"},
                          {L"huawei", L"Huawei"}, {L"xiaomi", L"Xiaomi"}, {L"fujitsu", L"Fujitsu"}, {L"microsoft", L"Microsoft"}, {L"lg electronics", L"LG"}, {L"clevo", L"Clevo"}, {L"medion", L"Medion"}, {L"sony", L"Sony"}};
    for (const auto& p : k) if (m.find(p.key) != std::wstring::npos) return p.name;
    return L"";
}

inline FanSupportReport FanSupportAssess(const FanSupportFacts& f) {
    FanSupportReport r;
    r.maker = FanMakerOf(f.manufacturer);
    const bool hp = r.maker == L"HP", dell = r.maker == L"Dell";
    // ---- sources tried (what was actually looked at)
    r.sourcesTried.push_back(std::wstring(L"Sensor helper (LibreHardwareMonitor): ") + (!f.helperRunning ? L"not running" : f.sensorHelperFan ? L"reports fan values" : L"reports no fan"));
    r.sourcesTried.push_back(std::wstring(L"HP BIOS interface: ") + (hp ? (f.hpBiosFan ? L"reports fan values" : L"HP PC, no fan value returned") : L"not used (not an HP PC)"));
    r.sourcesTried.push_back(std::wstring(L"Dell Command | Monitor: ") + (dell ? (f.dellCmFan ? L"reports fan values" : L"Dell PC, not installed or no fan value") : L"not used (not a Dell PC)"));
    const bool thinkpad = r.maker == L"Lenovo" && Lower(f.model).find(L"thinkpad") != std::wstring::npos;
    r.sourcesTried.push_back(std::wstring(L"ThinkPad embedded controller (PawnIO module): ") + (f.pawnReadersOff && thinkpad ? L"switched off" : thinkpad ? (f.thinkpadEcFan ? L"reports fan 1" : L"ThinkPad, no value yet") : L"not used (not a ThinkPad)"));
    r.sourcesTried.push_back(std::wstring(L"Dell SMM (PawnIO module): ") + (f.pawnReadersOff && dell ? L"switched off" : dell ? (f.dellSmmFan ? L"reports fan values" : L"Dell PC, the firmware gave no fan value") : L"not used (not a Dell PC)"));
    // ---- reading
    if (!f.helperRunning) { r.reading = FanReadState::HelperOff; r.readingLine = L"The sensor helper is not running, so no fan can be read."; r.steps.push_back(L"Start monitoring on the Monitor page."); }
    else if (f.fans > 0 && f.fansWithRpm >= f.fans) { r.reading = FanReadState::Live; r.readingLine = std::to_wstring(f.fansWithRpm) + L" fan(s) report a live speed."; }
    else if (f.fansWithRpm > 0) { r.reading = FanReadState::Partial; r.readingLine = std::to_wstring(f.fansWithRpm) + L" of " + std::to_wstring(f.fans) + L" fan(s) report a live speed; the others are listed without a value."; }
    else {
        r.reading = FanReadState::NotExposed;
        r.readingLine = L"No fan speed is exposed on this " + std::wstring(f.isNotebook ? L"notebook" : L"PC") + L" through the interfaces AnkhCore can read without a driver of its own.";
        if (dell) r.steps.push_back(L"Dell: AnkhCore also tries Dell's system-management interface by itself (read-only). If no value appears, this model's BIOS does not answer it: install Dell Command | Monitor (free from Dell) and AnkhCore reads the fan speed through its WMI provider.");
        else if (thinkpad) r.steps.push_back(L"ThinkPad: AnkhCore reads the embedded controller through a signed PawnIO module (fan 1). If it shows nothing, the PawnIO driver may not be installed or this ThinkPad's controller uses another window.");
        else if (hp) r.steps.push_back(L"HP: this model's BIOS returned no fan value through the HP interface; the speed stays 'not exposed'.");
        else if (!r.maker.empty()) r.steps.push_back(r.maker + L": the maker publishes no driverless fan interface that AnkhCore can read. If a notebook-controller configuration matches this model (see Control), its speed in percent is shown; the RPM stays unverified.");
        else r.steps.push_back(L"The maker of this PC is unknown to AnkhCore, so only the generic sensor chips are tried.");
    }
    // ---- control follows the model
    const bool havePcts = f.configsExact + f.configsFamily > 0;
    if (f.directControllable > 0) { r.control = FanControlRoute::Direct; r.canControl = true; r.controlLine = std::to_wstring(f.directControllable) + L" fan channel(s) can be set directly by the sensor helper."; }
    else if (f.ecActive) { r.control = FanControlRoute::AnkhEcActive; r.canControl = true; r.controlLine = L"AnkhCore's own fan control is active for this model (" + (f.activeConfig.empty() ? std::wstring(L"configuration chosen") : f.activeConfig) + L"); the BIOS gets the fans back on automatic, on exit and on error."; }
    else if (f.ecReady && f.configsExact > 0) { r.control = FanControlRoute::AnkhEcExact; r.canControl = true; r.controlLine = L"A configuration made for this exact model is built into AnkhCore. Choose it below to turn AnkhCore's own fan control on."; }
    else if (f.ecReady && f.configsFamily > 0) { r.control = FanControlRoute::AnkhEcFamily; r.canControl = true; r.controlLine = L"Configurations for this model family are built into AnkhCore but none for this exact model. They are experimental: try one with the safeguards (40% minimum, thermal override, BIOS takes over on any error) and watch the temperatures."; }
    else if (f.nbfcInstalled && f.configsExact > 0) { r.control = FanControlRoute::ConfigExact; r.canControl = true; r.controlLine = L"An NBFC configuration made for this exact model is available" + (f.activeConfig.empty() ? std::wstring(L"; select it on this page.") : L" (active: " + f.activeConfig + L")."); }
    else if (f.nbfcInstalled && f.configsFamily > 0) { r.control = FanControlRoute::ConfigFamily; r.canControl = true; r.controlLine = L"NBFC configurations exist for this model family but none for this exact model. They are experimental: try one with the safeguards (40% minimum, thermal override) and watch the temperatures."; }
    else if (f.nbfcInstalled) { r.control = FanControlRoute::NbfcNoMatch; r.controlLine = L"NBFC is installed but has no configuration for this model. Fan speed cannot be set here; the BIOS keeps control."; }
    else if (havePcts) { r.control = FanControlRoute::NbfcMissingMatchExists; r.controlLine = L"A notebook fan-controller configuration exists for this model, but the NBFC program that applies it is not installed on this PC."; r.steps.push_back(L"Install NBFC (NoteBook FanControl) yourself, then reopen this page. AnkhCore does not bundle it (licence)."); }
    else if (f.isNotebook) { r.control = FanControlRoute::NbfcMissing; r.controlLine = L"No direct fan control and no known configuration for this notebook: monitoring only, the BIOS keeps control."; }
    else { r.control = FanControlRoute::NoRoute; r.controlLine = L"No direct fan control channel was reported: monitoring only. Desktop fans are usually set in the BIOS or by the motherboard's own software."; }
    return r;
}
// 2.8.3: the five separate levels of fan support for the SELECTED configuration, and the one honest reason the Apply button is off.
// Monitoring supported / control supported / configuration available / configuration verified / control operation available.
// Pure over plain facts: the page fills them; nothing here touches hardware and nothing here can switch a check off.
struct FanLevelFacts {
    FanReadState reading = FanReadState::NotExposed;
    bool controlSupported = false;          // FanSupportReport::canControl
    int configsAvailable = 0;               // exact + family configurations found for this model
    bool haveSelection = false, selExact = false, selFamily = false;   // the configuration the person selected, and how it matches this model
    bool engineReady = false;               // AnkhCore's own embedded-controller module can be loaded on this PC
    bool nbfcInstalled = false;             // the optional NBFC program is present (the old route)
    bool controlEnabled = true;             // kFanControlEnabled
    bool testBuild = false;                 // the test build never writes
    bool alreadyActive = false;             // AnkhCore's engine already runs this configuration
    std::wstring engineState;               // the helper's own words when the engine is not ready
};
struct FanLevels {
    bool monitoring = false, controlSupported = false, configAvailable = false, configVerified = false, operationAvailable = false;
    bool viaEngine = false, viaNbfc = false;  // which route the Apply button would take
    bool unverified = false;                  // 2.8.7: the chosen configuration is not for this model or its family: allowed on the person's explicit choice, with a stronger warning
    std::wstring reason;                      // why the Apply button is off ("" when it is on)
};
inline FanLevels FanAssessLevels(const FanLevelFacts& f) {
    FanLevels l;
    l.monitoring = f.reading == FanReadState::Live || f.reading == FanReadState::Partial;
    l.controlSupported = f.controlSupported;
    l.configAvailable = f.configsAvailable > 0;
    l.configVerified = f.haveSelection && f.selExact;   // verified = written for exactly this model; a family match stays experimental
    const bool usable = f.haveSelection && (f.selExact || f.selFamily);
    l.unverified = f.haveSelection && !usable;
    if (!f.controlEnabled) l.reason = L"Fan control is switched off in this build.";
    else if (f.testBuild) l.reason = L"The test build never writes to the fans.";
    else if (!f.haveSelection) l.reason = L"Select a configuration first.";
    else if (f.alreadyActive) l.reason = L"This configuration is already active. Use \"Turn AnkhCore fan control off\" to hand the fans back to the BIOS.";
    else if (f.nbfcInstalled) { l.viaNbfc = true; l.operationAvailable = true; }
    else if (!f.engineReady) l.reason = L"AnkhCore's fan engine cannot reach the embedded controller on this PC" + (f.engineState.empty() ? std::wstring(L".") : L" (" + f.engineState + L").") + L" Monitoring only; the BIOS keeps control.";
    else { l.viaEngine = true; l.operationAvailable = true; }
    return l;
}

// 2.8.6: which way the single "Minimum fan speed" setting reaches the fans on this PC (pure; the page only draws what this says).
enum class FanFloorRoute { None, Direct, Nbfc, OwnEngine };
inline FanFloorRoute FanFloorRouteOf(bool controlEnabled, bool directControllable, bool nbfcInstalled, bool haveConfig, bool engineReady, bool candidateUsable) {
    if (!controlEnabled) return FanFloorRoute::None;
    if (directControllable) return FanFloorRoute::Direct;
    if (nbfcInstalled) return haveConfig ? FanFloorRoute::Nbfc : FanFloorRoute::None;   // NBFC present: it is the route; without a configuration there is nothing to apply yet
    if (engineReady && candidateUsable) return FanFloorRoute::OwnEngine;
    return FanFloorRoute::None;
}

inline const wchar_t* FanReadingLabel(FanReadState x) { return x == FanReadState::Live ? L"Reading: live" : x == FanReadState::Partial ? L"Reading: partial" : x == FanReadState::HelperOff ? L"Reading: helper off" : L"Reading: not exposed"; }
inline const wchar_t* FanControlRouteLabel(FanControlRoute x) {
    switch (x) {
    case FanControlRoute::Direct: return L"Control: direct";
    case FanControlRoute::AnkhEcActive: return L"Control: AnkhCore engine (on)";
    case FanControlRoute::AnkhEcExact: return L"Control: exact-model config (built in)";
    case FanControlRoute::AnkhEcFamily: return L"Control: family config (built in, experimental)";
    case FanControlRoute::ConfigExact: return L"Control: exact-model config";
    case FanControlRoute::ConfigFamily: return L"Control: family config (experimental)";
    case FanControlRoute::NbfcNoMatch: return L"Control: no config for this model";
    case FanControlRoute::NbfcMissingMatchExists: return L"Control: NBFC not installed";
    default: return L"Control: monitoring only";
    }
}

}  // namespace ak
