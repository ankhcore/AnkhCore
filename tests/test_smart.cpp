// WT-03 unit tests: smartctl JSON parsing and the health assessment, with fixtures written to the documented
// `smartctl -a -j` / `--scan -j` schema (smartmontools JSON format 1.0). The fixtures are hand-written, not captured from a
// real drive: no smartctl.exe is available in this tree. No drive is touched and nothing is run.
#define ANKH_TEST_BUILD 1
#include "eng_smart.h"
#include <cstdio>

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what) { if (ok) ++g_pass; else { ++g_fail; std::printf("FAIL  %s\n", what); } }

static const char* kNvmeGood = R"J({"json_format_version":[1,0],"smartctl":{"version":[7,4],"exit_status":0},
 "device":{"name":"/dev/nvme0","info_name":"/dev/nvme0","type":"nvme","protocol":"NVMe"},
 "model_name":"Samsung SSD 980 PRO 1TB","serial_number":"S5GXNX0T123456A","firmware_version":"5B2QGXA7",
 "nvme_total_capacity":1000204886016,"user_capacity":{"blocks":1953525168,"bytes":1000204886016},
 "smart_support":{"available":true,"enabled":true},"smart_status":{"passed":true,"nvme":{"value":0}},
 "nvme_smart_health_information_log":{"critical_warning":0,"temperature":38,"available_spare":100,"available_spare_threshold":10,
  "percentage_used":3,"data_units_read":10000000,"data_units_written":8000000,"power_cycles":412,"power_on_hours":2210,
  "unsafe_shutdowns":12,"media_errors":0,"num_err_log_entries":5},
 "temperature":{"current":38},"power_cycle_count":412,"power_on_time":{"hours":2210}})J";

static const char* kNvmeBad = R"J({"smartctl":{"exit_status":8},"device":{"name":"/dev/nvme1","type":"nvme","protocol":"NVMe"},
 "model_name":"Worn NVMe","smart_status":{"passed":false,"nvme":{"value":4}},
 "nvme_smart_health_information_log":{"critical_warning":4,"temperature":72,"available_spare":5,"available_spare_threshold":10,
  "percentage_used":105,"media_errors":3,"power_on_hours":40000,"power_cycles":900,"data_units_written":1}})J";

static const char* kSataSsd = R"J({"smartctl":{"exit_status":0},"device":{"name":"/dev/sda","type":"sat","protocol":"ATA"},
 "model_name":"CT500MX500SSD1","model_family":"Crucial/Micron MX500 SSDs","serial_number":"2012E1234567","firmware_version":"M3CR033",
 "user_capacity":{"bytes":500107862016},"rotation_rate":0,"smart_support":{"available":true,"enabled":true},"smart_status":{"passed":true},
 "ata_smart_attributes":{"table":[
  {"id":5,"name":"Reallocated_Sector_Ct","value":100,"worst":100,"thresh":10,"flags":{"prefailure":true},"raw":{"value":0,"string":"0"}},
  {"id":9,"name":"Power_On_Hours","value":99,"worst":99,"thresh":0,"flags":{"prefailure":false},"raw":{"value":6400,"string":"6400"}},
  {"id":12,"name":"Power_Cycle_Count","value":99,"worst":99,"thresh":0,"flags":{"prefailure":false},"raw":{"value":120,"string":"120"}},
  {"id":177,"name":"Wear_Leveling_Count","value":97,"worst":97,"thresh":0,"flags":{"prefailure":false},"raw":{"value":55,"string":"55"}},
  {"id":194,"name":"Temperature_Celsius","value":66,"worst":50,"thresh":0,"flags":{"prefailure":false},"raw":{"value":34,"string":"34 (Min/Max 0/50)"}},
  {"id":197,"name":"Current_Pending_Sector","value":100,"worst":100,"thresh":0,"flags":{"prefailure":false},"raw":{"value":0,"string":"0"}},
  {"id":199,"name":"UDMA_CRC_Error_Count","value":100,"worst":100,"thresh":0,"flags":{"prefailure":false},"raw":{"value":0,"string":"0"}}]},
 "ata_smart_data":{"self_test":{"status":{"value":0,"string":"completed without error","passed":true}}},
 "ata_smart_error_log":{"summary":{"revision":1,"count":0}},"temperature":{"current":34},"power_on_time":{"hours":6400},"power_cycle_count":120})J";

static const char* kHddWarn = R"J({"smartctl":{"exit_status":64},"device":{"name":"/dev/sdb","type":"sat","protocol":"ATA"},
 "model_name":"WDC WD40EFRX-68N32N0","user_capacity":{"bytes":4000787030016},"rotation_rate":5400,"smart_status":{"passed":true},
 "ata_smart_attributes":{"table":[
  {"id":5,"name":"Reallocated_Sector_Ct","value":196,"worst":196,"thresh":140,"flags":{"prefailure":true},"raw":{"value":24,"string":"24"}},
  {"id":197,"name":"Current_Pending_Sector","value":199,"worst":199,"thresh":0,"flags":{"prefailure":false},"raw":{"value":8,"string":"8"}},
  {"id":9,"name":"Power_On_Hours","value":60,"worst":60,"thresh":0,"flags":{"prefailure":false},"raw":{"value":29000,"string":"29000"}}]},
 "ata_smart_error_log":{"summary":{"count":2}},"temperature":{"current":41},"power_on_time":{"hours":29000}})J";

static const char* kPrefailThreshold = R"J({"smartctl":{"exit_status":16},"device":{"name":"/dev/sdc","type":"sat","protocol":"ATA"},
 "model_name":"Old HDD","rotation_rate":7200,"smart_status":{"passed":true},
 "ata_smart_attributes":{"table":[{"id":3,"name":"Spin_Up_Time","value":20,"worst":20,"thresh":21,"flags":{"prefailure":true},"raw":{"value":9000,"string":"9000"}}]}})J";

static const char* kUsbUnsupported = R"J({"smartctl":{"exit_status":2,"messages":[{"string":"/dev/sdd: Unknown USB bridge [0x1234:0x5678 (0x100)]","severity":"error"}]},
 "device":{"name":"/dev/sdd","info_name":"/dev/sdd","type":"scsi","protocol":"SCSI"}})J";

static const char* kScan = R"J({"smartctl":{"version":[7,4],"exit_status":0},"devices":[
 {"name":"/dev/sda","info_name":"/dev/sda","type":"sat","protocol":"ATA"},
 {"name":"/dev/nvme0","info_name":"/dev/nvme0","type":"nvme","protocol":"NVMe"},
 {"name":"/dev/sdz;calc","type":"sat","protocol":"ATA"}]})J";

// The test program doubles as a stand-in smartctl.exe for the end-to-end run below: copied to <DataDir>\Tools\smartctl.exe, it
// answers `--scan -j` and `-a -j ...` with the fixtures. It is a test harness for the runner only, never shipped.
static int FakeSmartctl(const std::wstring& args) {
    if (args.rfind(L"--scan", 0) == 0) { std::fputs(kScan, stdout); return 0; }
    if (args.find(L"/dev/nvme0") != std::wstring::npos) { std::fputs(kNvmeGood, stdout); return 0; }
    if (args.find(L"/dev/sda") != std::wstring::npos) { std::fputs(kHddWarn, stdout); return 64; }   // smartctl's exit status is a bit mask, not an error
    std::fputs("{}", stdout); return 2;
}

int main() {
    using namespace ak;
    {   // run as the fake smartctl?
        wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH);
        if (Lower(NameOf(me)) == L"smartctl.exe") { std::wstring cl = GetCommandLineW(); const size_t sp = cl.find(L".exe"); std::wstring a = cl.substr(sp == std::wstring::npos ? 0 : sp + 4); while (!a.empty() && (a[0] == L'"' || a[0] == L' ')) a.erase(0, 1); return FakeSmartctl(a); }
    }
    SmartDisk n; Check(SmartParseDevice(kNvmeGood, n), "nvme good: parses");
    Check(n.iface == L"NVMe" && n.kind == L"NVMe SSD" && n.capacity == 1000204886016ull && n.model == L"Samsung SSD 980 PRO 1TB" && n.serialTail == L"456A", "nvme good: identity, interface, capacity; serial shortened to its last 4");
    Check(n.hasTemp && n.tempC == 38 && n.hasPoh && n.powerOnHours == 2210 && n.hasCycles && n.powerCycles == 412 && n.hasLife && n.lifeLeftPct == 97 && n.hasSpare && n.sparePct == 100 && n.hasWritten && n.bytesWritten == 8000000ull * 512000ull, "nvme good: temperature, hours, cycles, life, spare, bytes written");
    Check(n.state == SmartState::Good && n.issues.empty() && n.hasStatus && n.statusPassed, "nvme good: state Good, no findings (num_err_log_entries alone is not a warning)");

    SmartDisk b; Check(SmartParseDevice(kNvmeBad, b), "nvme bad: parses");
    Check(b.state == SmartState::Critical && b.issues.size() >= 3, "nvme bad: Critical (failed status, critical warning, spare below threshold)");
    Check(b.hasLife && b.lifeLeftPct == 0 && b.hasMediaErrors && b.mediaErrors == 3 && b.tempC == 72, "nvme bad: life 0 % (percentage_used capped), media errors, temperature");

    SmartDisk s; Check(SmartParseDevice(kSataSsd, s), "sata ssd: parses");
    Check(s.iface == L"SATA" && s.kind == L"SSD" && s.hasLife && s.lifeLeftPct == 97 && s.hasTemp && s.tempC == 34 && s.hasPoh && s.powerOnHours == 6400 && s.attrs.size() == 7, "sata ssd: kind SSD, wear attribute 177 read as 97 % left, temperature, hours, 7 attributes");
    Check(s.state == SmartState::Good && s.hasSelfTest && s.selfTestPassed && s.hasCrc && s.crcErrors == 0, "sata ssd: Good; last self-test passed");

    SmartDisk h; Check(SmartParseDevice(kHddWarn, h), "hdd warn: parses");
    Check(h.kind == L"HDD" && h.state == SmartState::Warning && h.hasRealloc && h.reallocated == 24 && h.hasPending && h.pending == 8 && h.hasErrorLog && h.errorLogCount == 2, "hdd warn: Warning for reallocated, pending sectors and error-log entries (status itself passed)");
    Check(h.hasSelfTest == false && h.capacity == 4000787030016ull, "hdd warn: no self-test info is invented when the drive reports none");

    SmartDisk p; Check(SmartParseDevice(kPrefailThreshold, p), "prefail: parses");
    Check(p.state == SmartState::Critical && !p.issues.empty(), "prefail: an attribute at its threshold is Critical even when the overall status passed");

    SmartDisk u; Check(SmartParseDevice(kUsbUnsupported, u), "usb: parses");
    Check(!u.hasData && u.state == SmartState::Unsupported && u.problem.find(L"Unknown USB bridge") != std::wstring::npos && u.model.empty(), "usb bridge: Not supported, with smartctl's own message, no values invented");

    SmartDisk bad; Check(!SmartParseDevice("smartctl: unrecognized option", bad) && !SmartParseDevice("[1,2]", bad) && !SmartParseDevice("", bad), "non-JSON or non-object output is rejected, never half-parsed");

    SmartDisk partial; Check(SmartParseDevice(R"J({"device":{"name":"/dev/sde","type":"sat","protocol":"ATA"},"model_name":"X","smart_status":{"passed":true}})J", partial) &&
                             partial.state == SmartState::Good && !partial.hasTemp && !partial.hasPoh && !partial.hasLife, "a drive that reports only the overall status: Good, and the missing values stay absent (not zero)");
    SmartDisk nodata; Check(SmartParseDevice(R"J({"device":{"name":"/dev/sdf","type":"sat"},"model_name":"Y","ata_smart_attributes":{"table":[]}})J", nodata) &&
                            nodata.state == SmartState::Unknown, "data without any verdict from the drive: Unknown");
    SmartDisk off; Check(SmartParseDevice(R"J({"device":{"name":"/dev/sdg","type":"sat"},"model_name":"Z","smart_support":{"available":false,"enabled":false}})J", off) &&
                         off.state != SmartState::Good && !off.hasData, "SMART not available on the drive: never Good");

    std::vector<SmartScanDev> devs;
    Check(SmartParseScan(kScan, devs) && devs.size() == 3 && devs[0].name == L"/dev/sda" && devs[1].type == L"nvme", "scan: three devices read");
    Check(SmartParseScan(R"J({"smartctl":{"exit_status":0}})J", devs) && devs.empty() && !SmartParseScan("not json", devs), "scan: an empty list is valid, text is not");
    Check(SmartDevNameOk(L"/dev/sda") && SmartDevNameOk(L"/dev/nvme0") && !SmartDevNameOk(devs.empty() ? L"/dev/sdz;calc" : L"") && !SmartDevNameOk(L"/dev/sda -t long") && !SmartDevNameOk(L"C:\\x") && !SmartDevNameOk(L"/dev/"), "device names: only /dev/<letters+digits> reach a command line");
    Check(SmartTypeOk(L"sat") && SmartTypeOk(L"nvme") && SmartTypeOk(L"sat,12") && SmartTypeOk(L"megaraid,0") && !SmartTypeOk(L"sat -t long") && !SmartTypeOk(L"") && !SmartTypeOk(L"a&b"), "device types: no spaces or shell characters");
    Check(SmartArgsAreReadOnly(L"--scan -j") && SmartArgsAreReadOnly(L"-a -j -d nvme /dev/nvme0") && SmartArgsAreReadOnly(L"-a -j /dev/sda") &&
          !SmartArgsAreReadOnly(L"-t long /dev/sda") && !SmartArgsAreReadOnly(L"-a -j -t short /dev/sda") && !SmartArgsAreReadOnly(L"-a -j -s on /dev/sda") &&
          !SmartArgsAreReadOnly(L"-a -j --set=standby,now /dev/sda") && !SmartArgsAreReadOnly(L"-s off /dev/sda") && !SmartArgsAreReadOnly(L"-a -j --offlineauto=on /dev/sda"),
          "read-only guard: scan and -a -j pass; self-tests, enabling SMART, setting options and automatic offline testing are refused");

    // WT-03 hardening: production trusts only <AppDir>\Tools; the user-writable data folder is for the isolated test build alone
    {
        const auto prod = SmartctlCandidateDirs(false), test = SmartctlCandidateDirs(true);
        Check(prod.size() == 1 && prod[0] == AppDir() + L"\\Tools", "smartctl path: production looks only in <AppDir>\\Tools");
        bool dataDir = false; for (const auto& d : prod) dataDir = dataDir || Lower(d).rfind(Lower(DataDir()), 0) == 0;
        Check(!dataDir && test.size() == 2 && test[1] == DataDir() + L"\\Tools", "smartctl path: the data folder is never a production location; only the test build adds it");
        Check(!SmartExePathOk(L"smartctl.exe") && !SmartExePathOk(L"C:\\Tools\\..\\smartctl.exe") && !SmartExePathOk(L"\\\\server\\share\\smartctl.exe") && !SmartExePathOk(WinDir() + L"\\System32\\cmd.exe") && !SmartExePathOk(WinDir() + L"\\System32"),
              "executable path: relative, '..', network, another program name and a folder are refused");
    }
    // advisory vs device-reported findings
    {
        bool critDevice = !b.issues.empty(); for (const auto& i : b.issues) if (i.level == SmartState::Critical) critDevice = critDevice && !i.advisory;
        Check(critDevice, "findings: every Critical finding comes from the drive's own verdict or threshold, never from an AnkhCore limit");
        bool allAdv = !h.issues.empty(); for (const auto& i : h.issues) allAdv = allAdv && i.advisory;
        Check(allAdv && h.state == SmartState::Warning, "findings: sector counts / error log with a passing drive verdict are Advisory warnings");
        SmartDisk hot; SmartParseDevice(R"J({"device":{"name":"/dev/sdh","type":"sat","protocol":"ATA"},"model_name":"H","smart_status":{"passed":true},"temperature":{"current":75}})J", hot);
        Check(hot.state == SmartState::Warning && hot.issues.size() == 1 && hot.issues[0].advisory && hot.issues[0].text.find(L"advisory limit 70") != std::wstring::npos, "findings: temperature alone is an Advisory warning that names the limit, never Critical");
        SmartDisk cool; SmartParseDevice(R"J({"device":{"name":"/dev/sdi","type":"sat","protocol":"ATA"},"model_name":"C","smart_status":{"passed":true},"temperature":{"current":69}})J", cool);
        Check(cool.state == SmartState::Good && cool.issues.empty(), "findings: just below the advisory temperature limit stays Good");
        Check(SmartAdvisory().tempWarnC == 70 && SmartAdvisory().lifeWarnPct == 20 && SmartAdvisory().spareLowPct == 20, "advisory limits: the documented defaults");
    }
    // the shared read-only rule (single definition in base.h) refuses anything but scan / -a -j [-d type] /dev/x
    Check(SmartctlArgsAreReadOnly(L"--scan -j") && SmartctlArgsAreReadOnly(L"-a -j -d sat,12 /dev/sda") && !SmartctlArgsAreReadOnly(L"-a -j -d nvme /dev/nvme0 -t short") && !SmartctlArgsAreReadOnly(L"-a -j /dev/sda /dev/sdb") &&
          !SmartctlArgsAreReadOnly(L"-a -j") && !SmartctlArgsAreReadOnly(L"-a -j C:\\x.txt") && !SmartctlArgsAreReadOnly(L"-x -j /dev/sda") && !SmartctlArgsAreReadOnly(L"-a -j --drivedb=x /dev/sda") && !SmartctlArgsAreReadOnly(L"-a -j /dev/sda;calc") &&
          !SmartctlArgsAreReadOnly(L"-a -j -d nvme;x /dev/sda") && !SmartctlArgsAreReadOnly(L"-d sat /dev/sda"), "read-only rule: a closed list of tokens");
    {   // the runner refuses a non-read-only command and a program that is not smartctl.exe
        DWORD rc = 0;
        Check(SmartRunJson(WinDir() + L"\\System32\\cmd.exe", L"--scan -j", 5000, &rc).empty() && rc == kRunLaunchFailed, "runner: another program is never run");
        Check(SmartRunJson(L"C:\\x\\smartctl.exe", L"-a -j -t long /dev/sda", 5000, &rc).empty() && rc == kRunLaunchFailed, "runner: a self-test command line is never run");
    }
    // missing smartctl: an accurate "not installed" state, with Windows' own list as the fallback, never fake SMART values
    if (SmartctlPath().empty()) {
        const SmartReport r = SmartScanNow();
        Check(r.done && !r.smartctlFound && !r.problem.empty() && r.problem.find(L"not installed") != std::wstring::npos, "no smartctl.exe: the report says it is not installed");
        bool honest = true; for (const auto& d : r.disks) honest = honest && d.limited && !d.hasTemp && !d.hasPoh && d.attrs.empty() && !d.problem.empty();
        Check(honest, "no smartctl.exe: Windows' own drive list carries no invented temperature, hours or attributes");
        std::printf("      (fallback listed %d drive(s) from Windows)\n", static_cast<int>(r.disks.size()));
    } else std::printf("      (smartctl.exe present at %ls: missing-file case skipped)\n", SmartctlPath().c_str());
    {   // end to end through the real runner: copy this program as Tools\smartctl.exe in the isolated test data folder
        const std::wstring tools = DataDir() + L"\\Tools", dst = tools + L"\\smartctl.exe";
        wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH);
        const bool had = FileExists(dst);
        EnsureDir(tools);
        if (!had && CopyFileW(me, dst.c_str(), TRUE) && SmartctlPath() == dst) {
            const SmartReport r = SmartScanNow();
            Check(r.smartctlFound && r.problem.empty() && r.disks.size() == 3, "runner: scan + one read per drive; three devices listed");
            const SmartDisk* nv = nullptr; const SmartDisk* sa = nullptr; const SmartDisk* bad = nullptr;
            for (const auto& d : r.disks) { if (d.dev == L"/dev/nvme0") nv = &d; else if (d.model.find(L"WDC") != std::wstring::npos) sa = &d; else bad = &d; }
            Check(nv && nv->state == SmartState::Good && nv->hasTemp && nv->tempC == 38, "runner: the NVMe drive is read and Good");
            Check(sa && sa->state == SmartState::Warning && sa->exitStatus == 64 && sa->reallocated == 24, "runner: a non-zero smartctl exit status with JSON is a result, not a failure");
            Check(bad && !bad->problem.empty() && bad->state == SmartState::Unknown && !bad->hasData, "runner: a device name of an unexpected shape is not put on a command line");
            DeleteFileW(dst.c_str()); RemoveDirectoryW(tools.c_str());
        } else Check(had, "runner: could not stage the stand-in smartctl.exe in the test data folder");
    }
    Check(!SmartLooksLikeX64Exe(L"C:\\definitely\\missing\\smartctl.exe") && !SmartLooksLikeX64Exe(ak::WinDir() + L"\\win.ini") && SmartLooksLikeX64Exe(ak::WinDir() + L"\\System32\\cmd.exe"),
          "program check: a missing file and a text file are refused; a real 64-bit executable passes");

    std::printf(g_fail ? "FAILED (%d passed, %d failed)\n" : "ALL PASSED (%d passed)\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
