// eng_procintel.h - M-2.6-PROC1: the first safe slice of Process Intelligence (MASTER_PROMPT 4.5).
//
// This slice READS and DECIDES; it CHANGES NOTHING. It contains no call that sets a priority, an affinity, a CPU set, an efficiency
// mode, a power plan or ends a process. What it provides:
//   * a process inventory (`ProcCollectLive`: one snapshot on demand, never a timer; names, ids, image path, session, memory, priority
//     class. Command lines are deliberately NOT read: they can contain secrets);
//   * a protection classification (`ProcClassify`) that sorts every process into Protected / Caution / Adjustable with a stated reason,
//     failing SAFE: anything that cannot be identified is Protected;
//   * a policy gate (`ProcMayChange`) that a future change engine must pass before touching a process: critical, system, security,
//     service, other-session, AnkhCore's own and unidentifiable processes are never allowed; ending a process is never allowed; the
//     real-time priority class is never allowed; every allowed change must record the previous state first (so it can be undone).
// The pure functions take a plain `ProcRaw` list and a `ProcContext`, so they are tested with fabricated data.
// Later slices (priority/affinity/EcoQoS with history and undo, application rules) must go through ProcMayChange; they are not part of PROC1.
#pragma once
#include "base.h"
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace ak {

struct ProcRaw {
    DWORD pid = 0, ppid = 0;
    DWORD session = 0xFFFFFFFF;          // 0xFFFFFFFF = unknown
    std::wstring name, path;             // file name; full image path (empty = could not be read)
    DWORD threads = 0;
    uint64_t workingSet = 0;
    DWORD priorityClass = 0;             // Win32 class constant; 0 = could not be read
};

struct ProcContext {
    DWORD selfPid = 0, selfSession = 0xFFFFFFFF;
    std::wstring systemRoot;                       // e.g. C:\Windows
    std::vector<std::wstring> securityDirs;        // image directories of the built-in security products
    std::vector<std::wstring> selfNamePrefixes = {L"ankhcore", L"ankhsensors"};   // AnkhCore's own executables
};

enum class ProcClass { Critical, Security, Self, Service, Unknown, System, Shell, Sensitive, Application };
inline const wchar_t* ProcClassLabel(ProcClass c) {
    switch (c) {
    case ProcClass::Critical: return L"Critical system process";
    case ProcClass::Security: return L"Security";
    case ProcClass::Self: return L"AnkhCore";
    case ProcClass::Service: return L"Other session / service";
    case ProcClass::Unknown: return L"Not identifiable";
    case ProcClass::System: return L"Windows component";
    case ProcClass::Shell: return L"Desktop shell";
    case ProcClass::Sensitive: return L"Sensitive third-party";
    default: return L"Application";
    }
}
enum class ProcProtection { Protected, Caution, Adjustable };
inline const wchar_t* ProcProtectionLabel(ProcProtection p) { return p == ProcProtection::Protected ? L"Protected" : p == ProcProtection::Caution ? L"Caution" : L"Adjustable"; }

struct ProcEntry {
    ProcRaw raw;
    ProcClass cls = ProcClass::Unknown;
    ProcProtection prot = ProcProtection::Protected;
    std::wstring reason;
};

namespace procdetail {
inline bool In(const std::wstring& n, std::initializer_list<const wchar_t*> list) { for (const wchar_t* s : list) if (n == s) return true; return false; }
inline std::wstring Dir(std::wstring p) { std::replace(p.begin(), p.end(), L'/', L'\\'); return Lower(p); }
inline bool Under(const std::wstring& path, std::wstring dir) {
    if (dir.empty() || path.empty()) return false;
    const std::wstring p = Dir(path); dir = Dir(dir);
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == L'\\';
}
}  // namespace procdetail

// One process, one decision. The order matters: the most protective rule that applies wins, and "cannot tell" is Protected.
inline ProcEntry ProcClassify(const ProcRaw& r, const ProcContext& ctx, const std::set<DWORD>& selfTree) {
    using namespace procdetail;
    ProcEntry e; e.raw = r;
    const std::wstring n = Lower(r.name);
    auto set = [&](ProcClass c, ProcProtection p, const wchar_t* why) { e.cls = c; e.prot = p; e.reason = why; return e; };
    if (r.pid == 0 || r.pid == 4 || In(n, {L"system", L"registry", L"memory compression", L"secure system", L"system idle process", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
                                          L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe", L"sihost.exe", L"ctfmon.exe", L"audiodg.exe", L"spoolsv.exe", L"wudfhost.exe"}))
        return set(ProcClass::Critical, ProcProtection::Protected, L"A core Windows process: changing it can destabilize or lock the system.");
    if (r.pid == ctx.selfPid || selfTree.count(r.pid)) return set(ProcClass::Self, ProcProtection::Protected, L"AnkhCore's own process or one it started.");
    for (const std::wstring& pre : ctx.selfNamePrefixes) if (!pre.empty() && n.compare(0, pre.size(), pre) == 0) return set(ProcClass::Self, ProcProtection::Protected, L"An AnkhCore executable.");
    if (In(n, {L"msmpeng.exe", L"nissrv.exe", L"securityhealthservice.exe", L"securityhealthsystray.exe", L"smartscreen.exe", L"mpdefendercoreservice.exe", L"sgrmbroker.exe", L"mssense.exe", L"sense.exe",
                 L"avp.exe", L"mbamservice.exe", L"mbam.exe", L"avastsvc.exe", L"avgsvc.exe", L"bdagent.exe", L"ekrn.exe", L"mcshield.exe", L"csfalconservice.exe", L"csfalconcontainer.exe", L"sentinelagent.exe", L"sentinelservicehost.exe"}))
        return set(ProcClass::Security, ProcProtection::Protected, L"A security component (Windows or a known security product): it must never be slowed down or limited.");
    for (const std::wstring& d : ctx.securityDirs) if (Under(r.path, d)) return set(ProcClass::Security, ProcProtection::Protected, L"Runs from a security product's folder: it must never be slowed down or limited.");
    if (r.session == 0xFFFFFFFF || ctx.selfSession == 0xFFFFFFFF || r.session != ctx.selfSession)
        return set(ProcClass::Service, ProcProtection::Protected, L"Not in your desktop session (a service, or another user's process): AnkhCore does not manage it.");
    if (ctx.systemRoot.empty()) return set(ProcClass::Unknown, ProcProtection::Protected, L"The Windows folder is not known, so nothing can be told apart from a Windows component.");
    if (In(n, {L"explorer.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"searchhost.exe", L"searchapp.exe", L"textinputhost.exe", L"runtimebroker.exe", L"taskhostw.exe", L"applicationframehost.exe", L"lockapp.exe", L"widgets.exe"}))
        return set(ProcClass::Shell, ProcProtection::Protected, L"Part of the desktop you are using: changing it makes Windows feel unresponsive.");
    if (r.path.empty()) return set(ProcClass::Unknown, ProcProtection::Protected, L"Its image path could not be read (usually a protected or elevated process), so it cannot be identified safely.");
    if (Under(r.path, ctx.systemRoot)) return set(ProcClass::System, ProcProtection::Protected, L"A component of Windows (runs from the Windows folder).");
    if (In(n, {L"vgc.exe", L"easyanticheat.exe", L"easyanticheat_eos.exe", L"beservice.exe", L"faceitclient.exe", L"vmware-vmx.exe", L"vmwp.exe", L"vboxheadless.exe", L"vboxsvc.exe", L"vboxsup.exe"}))
        return set(ProcClass::Sensitive, ProcProtection::Caution, L"An anti-cheat or virtual-machine process: changing it can break a game or a running machine. Needs an explicit confirmation.");
    return set(ProcClass::Application, ProcProtection::Adjustable, L"An ordinary application in your session.");
}

struct ProcAppGroup {
    std::wstring name;
    int count = 0;
    uint64_t workingSet = 0;
    ProcProtection prot = ProcProtection::Adjustable;   // the most restrictive of its instances
    std::vector<DWORD> pids;
};

struct ProcInventory {
    std::vector<ProcEntry> entries;
    std::vector<ProcAppGroup> groups;     // per executable name, largest memory first
    int protectedCount = 0, cautionCount = 0, adjustableCount = 0;
};

inline ProcInventory ProcBuildInventory(const std::vector<ProcRaw>& raw, const ProcContext& ctx) {
    ProcInventory inv;
    // AnkhCore's own tree: every descendant of this process is AnkhCore's (its helpers, a UniGetUI it started, a console tool).
    std::set<DWORD> tree;
    for (bool grew = true; grew;) {
        grew = false;
        for (const ProcRaw& r : raw) if (r.pid != 0 && !tree.count(r.pid) && r.ppid != r.pid && (r.ppid == ctx.selfPid || tree.count(r.ppid))) { tree.insert(r.pid); grew = true; }
    }
    std::map<std::wstring, size_t> at;
    for (const ProcRaw& r : raw) {
        ProcEntry e = ProcClassify(r, ctx, tree);
        (e.prot == ProcProtection::Protected ? inv.protectedCount : e.prot == ProcProtection::Caution ? inv.cautionCount : inv.adjustableCount)++;
        const std::wstring key = Lower(r.name);
        auto it = at.find(key);
        if (it == at.end()) { it = at.emplace(key, inv.groups.size()).first; ProcAppGroup g; g.name = r.name; inv.groups.push_back(g); }
        ProcAppGroup& g = inv.groups[it->second];
        ++g.count; g.workingSet += r.workingSet; g.pids.push_back(r.pid);
        if (static_cast<int>(e.prot) < static_cast<int>(g.prot)) g.prot = e.prot;   // Protected(0) < Caution(1) < Adjustable(2)
        inv.entries.push_back(std::move(e));
    }
    std::sort(inv.groups.begin(), inv.groups.end(), [](const ProcAppGroup& a, const ProcAppGroup& b) { return a.workingSet != b.workingSet ? a.workingSet > b.workingSet : Lower(a.name) < Lower(b.name); });
    return inv;
}

// Counts and the biggest groups, names only: no paths (they contain the user name), no command lines.
inline std::wstring ProcSummaryText(const ProcInventory& inv, size_t top = 10) {
    std::wstring t = L"Processes: " + std::to_wstring(inv.entries.size()) + L"  (protected " + std::to_wstring(inv.protectedCount) + L", caution " + std::to_wstring(inv.cautionCount) + L", adjustable " + std::to_wstring(inv.adjustableCount) + L")\r\n";
    for (size_t i = 0; i < inv.groups.size() && i < top; ++i) {
        const ProcAppGroup& g = inv.groups[i];
        t += g.name + L" x" + std::to_wstring(g.count) + L", " + std::to_wstring(g.workingSet >> 20) + L" MiB, " + ProcProtectionLabel(g.prot) + L"\r\n";
    }
    return t;
}

// ---------------------------------------------------------------- the policy gate for any future change
enum class ProcAction { SetPriority, SetAffinity, SetEfficiency, Terminate };
struct ProcDecision {
    bool allowed = false;
    bool needsConfirmation = false;
    bool mustRecordPrevious = false;   // an allowed change must first capture the current state, so it can be undone and defaults are preserved
    std::wstring reason;
};
constexpr DWORD kProcPrioIdle = 0x40, kProcPrioBelowNormal = 0x4000, kProcPrioNormal = 0x20, kProcPrioAboveNormal = 0x8000, kProcPrioHigh = 0x80, kProcPrioRealtime = 0x100;

// `value` = the target priority class for SetPriority, or the affinity mask for SetAffinity (ignored otherwise).
inline ProcDecision ProcMayChange(const ProcEntry& e, ProcAction a, uint64_t value, DWORD selfPid) {
    ProcDecision d;
    auto deny = [&](const std::wstring& why) { d.allowed = false; d.reason = why; return d; };
    if (e.raw.pid == selfPid || e.cls == ProcClass::Self) return deny(L"AnkhCore never changes its own processes.");
    if (e.prot == ProcProtection::Protected) return deny(e.reason);
    if (a == ProcAction::Terminate) {   // 2.8: only an ordinary application, and only after an explicit confirmation (its unsaved work is lost)
        if (e.prot != ProcProtection::Adjustable) return deny(L"Only ordinary applications can be ended from here.");
        d.allowed = true; d.needsConfirmation = true; d.mustRecordPrevious = false; d.reason = L"Allowed after an explicit confirmation: ending a process loses its unsaved work."; return d;
    }
    if (a == ProcAction::SetPriority) {
        const DWORD c = static_cast<DWORD>(value);
        if (value > 0xFFFFFFFFull) return deny(L"Not a priority class.");
        if (c == kProcPrioRealtime) return deny(L"The real-time priority class is never used: it can freeze the system.");
        if (c != kProcPrioIdle && c != kProcPrioBelowNormal && c != kProcPrioNormal && c != kProcPrioAboveNormal && c != kProcPrioHigh) return deny(L"Not a priority class.");
        d.needsConfirmation = (c == kProcPrioAboveNormal || c == kProcPrioHigh) || e.prot == ProcProtection::Caution;
    } else if (a == ProcAction::SetAffinity) {
        if (value == 0) return deny(L"An empty CPU mask would leave the process unable to run.");
        d.needsConfirmation = true;   // affinity changes how an application performs: always explained and confirmed
    } else {
        d.needsConfirmation = e.prot == ProcProtection::Caution;
    }
    d.allowed = true; d.mustRecordPrevious = true;
    d.reason = d.needsConfirmation ? L"Allowed after an explicit confirmation; the current state is recorded first so it can be restored." : L"Allowed; the current state is recorded first so it can be restored.";
    return d;
}

// ---------------------------------------------------------------- live, read-only collection
inline ProcContext ProcContextLive() {
    ProcContext c;
    c.selfPid = GetCurrentProcessId();
    DWORD s = 0xFFFFFFFF; if (ProcessIdToSessionId(c.selfPid, &s)) c.selfSession = s;
    wchar_t w[MAX_PATH]; if (GetWindowsDirectoryW(w, MAX_PATH)) c.systemRoot = w;
    for (const wchar_t* env : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t b[MAX_PATH]; if (GetEnvironmentVariableW(env, b, MAX_PATH)) { c.securityDirs.push_back(std::wstring(b) + L"\\Windows Defender"); c.securityDirs.push_back(std::wstring(b) + L"\\Windows Security"); }
    }
    wchar_t pd[MAX_PATH]; if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) c.securityDirs.push_back(std::wstring(pd) + L"\\Microsoft\\Windows Defender");
    return c;
}

// One snapshot, on demand (the caller runs it off the UI thread). It only opens handles with query rights; it changes nothing.
inline std::vector<ProcRaw> ProcCollectLive() {
    std::vector<ProcRaw> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        ProcRaw r; r.pid = pe.th32ProcessID; r.ppid = pe.th32ParentProcessID; r.name = pe.szExeFile; r.threads = pe.cntThreads;
        DWORD s = 0; if (ProcessIdToSessionId(r.pid, &s)) r.session = s;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid)) {
            wchar_t p[MAX_PATH * 2]; DWORD n = static_cast<DWORD>(std::size(p));
            if (QueryFullProcessImageNameW(h, 0, p, &n)) r.path.assign(p, n);
            PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) r.workingSet = pmc.WorkingSetSize;
            r.priorityClass = GetPriorityClass(h);
            CloseHandle(h);
        }
        out.push_back(std::move(r));
    }
    CloseHandle(snap);
    return out;
}

}  // namespace ak
