// Focused tests for base.h process and file primitives (normal build, harmless commands, %TEMP% only).
// Build + run: tools\test_base.cmd
#include "base.h"
#include "settings.h"
#include "startup.h"
#include "monitor_store.h"
#include "eng_sensors.h"
#include "mini_layout.h"
#include "sidebar_layout.h"
#include "sidebar_lifecycle.h"
#include "nav_layout.h"
#include "cursor_scheme.h"
#include "brand_logo.h"
#include "package_components.h"
#include "eng_cursors.h"
#include "material.h"
#include "anim.h"
#include "frames.h"
#include "net_counters.h"
#include "ipc.h"
#include "eng_theme.h"
#include "install.h"
#include "acbackup.h"
#include "eng_backup.h"
#include "eng_updater.h"
#include "eng_ugsession.h"
#include "eng_updui.h"
#include "eng_proglist.h"
#include "eng_hwintel.h"
#include "eng_devprofile.h"
#include "eng_fanguard.h"
#include "eng_procintel.h"
#include "nav_model.h"
#include "eng_apps_manager.h"
#include "eng_syscare.h"
#include "eng_fansupport.h"
#include "eng_procmgr.h"
#include "eng_powertoys.h"
#include "eng_ugbundle.h"
#include "eng_afterdark.h"
#include <filesystem>
#include <fstream>
#include "eng_remnants.h"
#include "eng_uninstall_batch.h"
#include "eng_icons.h"
#include "icon_status.h"
#include "page_info.h"
#include "eng_windhawk.h"
#include "wh_stage.h"
#include "theme.h"
#include "appearance_profile.h"
#include "eng_wua.h"
#include "journal_view.h"
#include "toast_queue.h"
#include "eng_fan.h"
#include "eng_system.h"
#include <cstdio>
#include <set>
#include <limits>
#include <algorithm>
#include <thread>
#include <map>
#include <atomic>
using namespace ak;
static int fails = 0, passes = 0;
static void Check(bool ok, const char* name) { if (ok) ++passes; else { ++fails; std::printf("FAIL  %s\n", name); } }
static void Check(bool ok, const std::string& name) { Check(ok, name.c_str()); }   // tests that put a measured number in their name
int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wcscmp(argv[1], L"--sleep-test") == 0) {
        Sleep(5000);
        return 0;
    }
    const std::wstring cmd = SysDir() + L"\\cmd.exe";
    const std::wstring selfExe = SelfPath();
    Check(Run(cmd, L"/c exit 7", true, 10000) == 7, "Run returns the exit code");
    Check(Run(selfExe, L"--sleep-test", true, 300) == kRunTimedOut, "Run reports a timeout (not 259)");
    Check(Run(cmd, L"/c exit 3", true, 0) == 0, "Run with timeout 0 = detached launch");
    Check(Run(SysDir() + L"\\does-not-exist.exe", L"", true, 1000) == kRunLaunchFailed, "Run reports a launch failure");
    int lines = 0;
    Check(RunCapture(cmd, L"/c echo one& echo two", [&](const std::wstring&) { ++lines; }, 10000) == 0 && lines == 2, "RunCapture streams lines");
    Check(RunCapture(selfExe, L"--sleep-test", [](const std::wstring&) {}, 500) == kRunTimedOut, "RunCapture reports a timeout");
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const std::wstring f = std::wstring(tmp) + L"ankhcore_test_atomic.txt";
    const std::string a = "first", b = "second-version";
    Check(WriteAtomic(f, reinterpret_cast<const uint8_t*>(a.data()), a.size()) && ReadText(f) == a, "WriteAtomic creates");
    Check(WriteAtomic(f, reinterpret_cast<const uint8_t*>(b.data()), b.size()) && ReadText(f) == b, "WriteAtomic replaces");
    Check(!FileExists(f + L".tmp"), "WriteAtomic leaves no temp file");
    DeleteFileW(f.c_str());
    // settings store (parser, validation, unknown keys)
    Settings s;
    Check(s.LoadFromText(L"{ \"sidebar.width\": 170, \"sidebar.clickThrough\": true, \"name\": \"A \\\"q\\\" \\u00e9\", \"future.key\": [1] }") == false,
          "settings: nested/array value rejected");
    Check(s.LoadFromText(L"\xFEFF{ \"sidebar.width\": 170, \"sidebar.clickThrough\": true, \"name\": \"A \\\"q\\\" \\u00e9\", \"future.key\": 42 }"), "settings: flat object with BOM parses");
    Check(s.Int(L"sidebar.width", 200, 120, 600) == 170, "settings: int in range");
    Check(s.Int(L"sidebar.width", 200, 180, 600) == 200, "settings: out-of-range int falls back to default");
    Check(s.Bool(L"sidebar.clickThrough", false), "settings: bool");
    Check(s.Str(L"name", L"") == L"A \"q\" \u00e9", "settings: string escapes");
    Check(s.RawForTest(L"future.key") == L"42", "settings: unknown key preserved verbatim");
    Check(!s.LoadFromText(L"{ \"a\": 1, }"), "settings: trailing comma rejected (file treated as damaged)");
    {   // 2.9.5 Task 3: default appearance profile of a new installation
        Settings fresh;
        fresh.LoadFromText(L"{ \"settings.version\": 1, \"wallpaper.intervalMin\": 360 }");   // a migration alone is still a fresh look
        const int n = ApplyAppearanceProfileOnce(fresh);
        Check(n > 20 && fresh.Int(L"appearance.tint", 0, 0, 100) == 47 && fresh.Str(L"appearance.theme", L"") == L"neutral-glass" &&
              fresh.Int(L"sidebar.width", 0, 0, 1000) == 210 && fresh.Bool(L"appearance.glassDefaults28", false) && fresh.Has(L"appearance.profile"),
              "appearance profile: a new installation gets the owner's portable look (theme, glass tint, Sidebar look)");
        Check(ApplyAppearanceProfileOnce(fresh) == 0, "appearance profile: it is decided once; a second start writes nothing");
        fresh.SetInt(L"appearance.tint", 60);
        ApplyAppearanceProfileOnce(fresh);
        Check(fresh.Int(L"appearance.tint", 0, 0, 100) == 60, "appearance profile: a value the person changed later is never reset");

        Settings old1; old1.LoadFromText(L"{ \"appearance.tint\": 70, \"sidebar.width\": 222, \"nav.layout\": 1 }");
        const size_t before = old1.Count();
        Check(ApplyAppearanceProfileOnce(old1) == 0 && old1.Int(L"appearance.tint", 0, 0, 100) == 70 && old1.Int(L"sidebar.width", 0, 0, 1000) == 222 &&
              old1.Int(L"nav.layout", 0, 0, 9) == 1 && old1.Count() == before + 1 && old1.Str(L"appearance.profile", L"") == L"2.9.5-existing-kept",
              "appearance profile: an existing user's settings survive an upgrade untouched (only the decision marker is added)");
        Settings old2; old2.LoadFromText(L"{ \"sidebar.opacity\": 90 }");
        Check(ApplyAppearanceProfileOnce(old2) == 0 && old2.Int(L"sidebar.opacity", 0, 0, 100) == 90 && !old2.Has(L"appearance.theme"),
              "appearance profile: a Sidebar-only customisation also counts as an existing user");

        bool portable = true; std::vector<std::pair<std::wstring, std::wstring>> sb;
        const wchar_t* denied[] = {L"sidebar.monitor", L"sidebar.importedFrom", L"sidebar.prevLayer", L"cursors.firstRunInitDone", L"nav.page", L"mini.x", L"mini.y", L"mini.enabled", L"icons.activeThemePath", L"fan.nbfcConfig", L"upd.skipAlways"};
        for (const auto& e : AppearanceProfile()) {
            const std::wstring k = e.key, v = e.value;
            for (const wchar_t* d : denied) if (k == d) portable = false;
            if (v.find(L'\\') != std::wstring::npos || v.find(L'/') != std::wstring::npos || v.find(L"DISPLAY") != std::wstring::npos || v.find(L'@') != std::wstring::npos) portable = false;
            if (!(k.rfind(L"appearance.", 0) == 0 || k.rfind(L"sidebar.", 0) == 0 || k.rfind(L"mini.", 0) == 0 || k.rfind(L"nav.", 0) == 0 || k.rfind(L"candy.", 0) == 0 || k.rfind(L"alert.", 0) == 0 || k.rfind(L"monitor", 0) == 0 || k.rfind(L"wallpaper.", 0) == 0 || k.rfind(L"wh.cat.", 0) == 0 || k == L"profile.active" || k == L"backup.kind")) portable = false;
            const bool behaviour = k == L"sidebar.enabled" || k == L"sidebar.autoHide" || k == L"sidebar.clickThrough" || k == L"sidebar.locked" || k == L"sidebar.edge" || k == L"sidebar.reserve" || k == L"sidebar.layer" || k == L"sidebar.preset";   // 2.10: the owner's behaviour too
            if (k.rfind(L"sidebar.", 0) == 0 && !behaviour) sb.push_back({k, v});
        }
        Check(portable, "appearance profile: appearance values only; no path, monitor, position, account or device value and no behaviour switch");
        bool known = true; for (const auto& kv : sb) { bool f = false; for (const auto& pk : SbProfileKeys()) if (kv.first == pk.key) f = true; known = known && f; }
        const auto decoded = SbProfileDecode(SbProfileEncode(sb));
        Check(known && decoded.size() == sb.size(), "appearance profile: every Sidebar value is a Sidebar-look key and passes the Sidebar range validation");
    }

    {   // 2.9.5 Tasks 4-7: Windows Update detection, selective download, cancel and lifecycle (engine + scripted provider; nothing touches Windows)
        auto waitFor = [](const std::function<bool()>& f, int ms = 8000) { for (int i = 0; i < ms / 5 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return f(); };
        auto mkU = [](const wchar_t* id, const wchar_t* title, WuState st) { WuUpdate u; u.id = id; u.title = title; u.state = st; u.sizeBytes = 50u << 20; u.sizeKnown = true; return u; };
        auto mkProv = [&](std::initializer_list<WuUpdate> ups) { auto f = std::make_shared<WuFakeProvider>(); f->search.ok = true; f->search.updates = ups; return f; };

        // pure helpers
        Check(WuClassifyHr(static_cast<long>(0x80072EE7)).kind == WuErr::Offline && WuClassifyHr(static_cast<long>(0x8024402C)).kind == WuErr::Offline &&
              WuClassifyHr(static_cast<long>(0x80070005)).kind == WuErr::AccessDenied && WuClassifyHr(static_cast<long>(0x80070422)).kind == WuErr::ServiceUnavailable &&
              WuClassifyHr(static_cast<long>(0x8024000B)).kind == WuErr::Cancelled && WuClassifyHr(static_cast<long>(0x80246007)).kind == WuErr::Api &&
              WuClassifyHr(static_cast<long>(0x80246007)).text.find(L"0x80246007") != std::wstring::npos,
              "windows update: error codes are classified (offline, access denied, service, cancelled) and an unknown code is reported with its number");
        Check(WuKbFrom({L"5034123"}, L"x") == L"KB5034123" && WuKbFrom({}, L"2024-01 Cumulative Update (KB5034765) for Windows") == L"KB5034765" && WuKbFrom({}, L"Some driver").empty() && WuKbFrom({L"12"}, L"no kb").empty(),
              "windows update: the KB number comes from Windows' list, else from the title, else it is empty");
        { WuUpdate a; a.sizeBytes = 3ull << 30; a.sizeKnown = true; WuUpdate b; b.sizeBytes = 5u << 20; b.sizeKnown = true; WuUpdate c0;
          Check(WuSizeText(a) == L"3.00 GB" && WuSizeText(b) == L"5.0 MB" && WuSizeText(c0) == L"size unknown", "windows update: sizes read as GB / MB and say 'size unknown' when Windows gives none"); }
        Check(WuStateFromFlags(true, true, true) == WuState::Installed && WuStateFromFlags(false, true, true) == WuState::Hidden && WuStateFromFlags(false, false, true) == WuState::Downloaded && WuStateFromFlags(false, false, false) == WuState::Available &&
              WuSelectable(WuState::Available) && WuSelectable(WuState::Failed) && WuSelectable(WuState::Cancelled) && !WuSelectable(WuState::Downloaded) && !WuSelectable(WuState::Installed) && !WuSelectable(WuState::Hidden) && !WuSelectable(WuState::Downloading),
              "windows update: installed / hidden / downloaded / available are told apart, and only what still needs a download can be chosen");
        { const WuPolicy p = WuPolicyFrom([](const wchar_t* sub, const wchar_t* v) -> long long { return std::wstring(sub) == L"AU" && std::wstring(v) == L"UseWUServer" ? 1 : std::wstring(v) == L"DisableWindowsUpdateAccess" ? 1 : -1; }, [](const wchar_t*) { return std::wstring(L"http://wsus.example"); });
          Check(p.managedServer && p.accessDisabled && !p.autoUpdatesOff && WuPolicyText(p).find(L"wsus.example") != std::wstring::npos, "windows update: policy values are read and described (managed server, access disabled), never written"); }

        // detection
        {
            auto f = mkProv({mkU(L"A", L"Update A (KB5000001)", WuState::Available), mkU(L"B", L"Update B", WuState::Downloaded), mkU(L"C", L"Update C", WuState::Installed), mkU(L"D", L"Update D", WuState::Hidden)});
            WuModel m(f);
            Check(m.Detect() && waitFor([&] { return !m.Snapshot().detecting; }), "windows update: a check runs in the background and finishes");
            const WuSnapshot s = m.Snapshot();
            Check(s.detected && s.updates.size() == 4 && s.updates[1].state == WuState::Downloaded && s.updates[2].state == WuState::Installed && s.updates[3].state == WuState::Hidden && s.status.find(L"1 available") != std::wstring::npos,
                  "windows update: detection reports available, downloaded, installed and hidden updates with a status line");
            auto off = std::make_shared<WuFakeProvider>(); off->search.ok = false; off->search.err = WuClassifyHr(static_cast<long>(0x80072EE7));
            WuModel m2(off); m2.Detect(); waitFor([&] { return !m2.Snapshot().detecting; });
            const WuSnapshot s2 = m2.Snapshot();
            Check(!s2.detected && s2.error.kind == WuErr::Offline && !s2.status.empty() && s2.updates.empty(), "windows update: offline is reported as offline, with no invented list");
        }
        // only the chosen updates are downloaded; the provider has no install call at all
        {
            auto f = mkProv({mkU(L"A", L"A", WuState::Available), mkU(L"B", L"B", WuState::Available), mkU(L"C", L"C", WuState::Available), mkU(L"D", L"D", WuState::Downloaded)});
            WuModel m(f); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; });
            m.Toggle(L"A"); m.Toggle(L"C"); m.Toggle(L"D");                       // D is already downloaded: it cannot be chosen
            Check(m.Snapshot().selected == std::set<std::wstring>({L"A", L"C"}), "windows update: only updates that still need a download can be chosen");
            const WuStartResult r = m.StartDownload({L"A", L"C"});
            waitFor([&] { return !m.Snapshot().downloading; });
            const WuSnapshot s = m.Snapshot();
            Check(r.started && f->downloaded == std::vector<std::wstring>({L"A", L"C"}) && s.updates[0].state == WuState::Downloaded && s.updates[2].state == WuState::Downloaded && s.updates[1].state == WuState::Available &&
                  s.done == 2 && s.failed == 0 && s.overallPct == 100 && s.lastReport.find(L"2 of 2 downloaded") != std::wstring::npos && !s.cancelRequested,
                  "windows update: exactly the chosen updates are downloaded, in order, and the others are left alone");
            const WuStartResult none = m.StartDownload({L"B", L"B"});               // a duplicate id inside one request is counted once
            waitFor([&] { return !m.Snapshot().downloading; });
            Check(none.started && none.accepted.size() == 1 && none.duplicates.size() == 1 && f->downloaded.size() == 3, "windows update: the same update twice in one request is downloaded once");
            Check(!m.StartDownload({L"A"}).started && !m.StartDownload({}).started, "windows update: an already-downloaded update or an empty choice starts nothing");
        }
        // duplicate / conflicting operations while a download runs
        {
            std::atomic<bool> release{false};
            auto f = mkProv({mkU(L"A", L"A", WuState::Available), mkU(L"B", L"B", WuState::Available)});
            f->download = [&](const WuUpdate&, const std::function<void(int)>& prog, const std::function<bool()>&) { prog(40); WuDlOutcome o; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2)); o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o; };
            WuModel m(f); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; });
            m.Toggle(L"A"); m.Toggle(L"B");
            m.StartDownload({L"A", L"B"});
            waitFor([&] { return m.Snapshot().updates[0].pct == 40; });
            const WuStartResult again = m.StartDownload({L"A"});
            Check(!again.started && !m.Detect() && m.DownloadActive(), "windows update: while a download runs, a second download or a new check is refused");
            m.Toggle(L"B"); m.SelectAll(false);
            Check(m.Snapshot().updates[0].state == WuState::Downloading && m.Snapshot().updates[1].state == WuState::Queued, "windows update: the running update and the queued one show their own state; the choice cannot be changed meanwhile");
            release = true; waitFor([&] { return !m.Snapshot().downloading; });
            Check(m.Snapshot().done == 2 && f->downloaded.size() == 2, "windows update: after the block, both complete exactly once");
        }
        // cancellation: requested vs completed, nothing more scheduled, finished downloads kept
        {
            auto f = mkProv({mkU(L"A", L"A", WuState::Available), mkU(L"B", L"B", WuState::Available), mkU(L"C", L"C", WuState::Available)});
            std::atomic<int> calls{0}; std::atomic<bool> slowStopped{false};
            f->download = [&](const WuUpdate&, const std::function<void(int)>& prog, const std::function<bool()>& cancel) {
                WuDlOutcome o; const int n = ++calls;
                if (n == 1) { prog(100); o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o; }
                while (!cancel()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::this_thread::sleep_for(std::chrono::milliseconds(120));        // Windows needs a moment to stop
                slowStopped = true; o.kind = WuDlKind::Aborted; return o; };
            WuModel m(f); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; });
            m.SelectAll(true); m.StartDownload({L"A", L"B", L"C"});
            waitFor([&] { return calls.load() == 2; });
            Check(m.RequestCancel() && m.Snapshot().cancelRequested && !m.Snapshot().cancelCompleted && m.Snapshot().downloading, "windows update: cancel is first 'requested' (still stopping), not 'completed'");
            Check(!m.RequestCancel(), "windows update: a second cancel request changes nothing");
            waitFor([&] { return !m.Snapshot().downloading; });
            const WuSnapshot s = m.Snapshot();
            Check(slowStopped && s.cancelCompleted && s.updates[0].state == WuState::Downloaded && s.updates[1].state == WuState::Cancelled && s.updates[2].state == WuState::Available &&
                  f->downloaded.size() == 2 && s.done == 1 && s.cancelledN == 1 && s.notStarted == 1 && s.lastReport.find(L"1 of 3 downloaded") != std::wstring::npos && s.lastReport.find(L"1 not started") != std::wstring::npos,
                  "windows update: cancel keeps the finished download, marks the stopped one cancelled, never starts the rest, and reports the partial result");
            Check(!m.RequestCancel() && WuSelectable(s.updates[1].state) && WuSelectable(s.updates[2].state), "windows update: after a cancel nothing is running and both can be chosen again");
        }
        // one failure does not corrupt the others; a success Windows does not confirm is a failure
        {
            auto f = mkProv({mkU(L"A", L"A", WuState::Available), mkU(L"B", L"B", WuState::Available), mkU(L"C", L"C", WuState::Available), mkU(L"D", L"D", WuState::Available)});
            f->download = [&](const WuUpdate& u, const std::function<void(int)>& prog, const std::function<bool()>&) {
                WuDlOutcome o; prog(100);
                if (u.id == L"B") { o.kind = WuDlKind::Failed; o.hr = static_cast<long>(0x80072EE7); return o; }
                if (u.id == L"C") { o.kind = WuDlKind::Done; o.verifiedDownloaded = false; return o; }
                o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o; };
            WuModel m(f); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; });
            m.SelectAll(true); m.StartDownload({L"A", L"B", L"C", L"D"});
            waitFor([&] { return !m.Snapshot().downloading; });
            const WuSnapshot s = m.Snapshot();
            Check(s.updates[0].state == WuState::Downloaded && s.updates[1].state == WuState::Failed && s.updates[2].state == WuState::Failed && s.updates[3].state == WuState::Downloaded &&
                  s.done == 2 && s.failed == 2 && f->downloaded.size() == 4 && s.updates[1].detail.find(L"could not be reached") != std::wstring::npos && s.updates[2].detail.find(L"not marked as downloaded") != std::wstring::npos &&
                  s.lastReport.find(L"2 of 4 downloaded, 2 failed") != std::wstring::npos && !s.cancelCompleted,
                  "windows update: a failed or unverified download is reported as failed; the others still complete and the overall state stays consistent");
        }
        // lifecycle: closing the application during a download; a provider that throws; callbacks after shutdown
        {
            auto f = mkProv({mkU(L"A", L"A", WuState::Available)});
            std::atomic<bool> sawCancel{false}; std::atomic<int> notes{0};
            f->download = [&](const WuUpdate&, const std::function<void(int)>& prog, const std::function<bool()>& cancel) {
                WuDlOutcome o; prog(10); while (!cancel()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                sawCancel = true; std::this_thread::sleep_for(std::chrono::milliseconds(30)); prog(10); o.kind = WuDlKind::Aborted; return o; };
            const auto t0 = std::chrono::steady_clock::now();
            {
                WuModel m(f, [&] { ++notes; }); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; });
                m.SelectAll(true); m.StartDownload({L"A"}); waitFor([&] { return m.Snapshot().updates[0].pct == 10; });
            }                                                                          // destroyed while the download runs: it must stop and return, not hang or crash
            const int after = notes.load(); std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            Check(sawCancel && ms < 3000 && notes.load() == after, "windows update: closing the application cancels the running download, returns promptly, and no callback reaches the destroyed model");
            auto g = mkProv({mkU(L"X", L"X", WuState::Available)});
            g->download = [](const WuUpdate&, const std::function<void(int)>&, const std::function<bool()>&) -> WuDlOutcome { throw 7; };
            WuModel m(g); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; }); m.SelectAll(true); m.StartDownload({L"X"}); waitFor([&] { return !m.Snapshot().downloading; });
            Check(m.Snapshot().updates[0].state == WuState::Failed && !m.DownloadActive() && m.Detect(), "windows update: a provider that throws becomes a failed update, and the model is usable again");
        }
        // state survives leaving and re-entering the page (the model is not tied to a page)
        {
            std::atomic<bool> release{false};
            auto f = mkProv({mkU(L"A", L"A", WuState::Available)});
            f->download = [&](const WuUpdate&, const std::function<void(int)>& prog, const std::function<bool()>&) { prog(55); WuDlOutcome o; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(2)); o.kind = WuDlKind::Done; o.verifiedDownloaded = true; return o; };
            WuModel m(f); m.Detect(); waitFor([&] { return !m.Snapshot().detecting; }); m.SelectAll(true); m.StartDownload({L"A"}); waitFor([&] { return m.Snapshot().overallPct == 55; });
            const WuSnapshot first = m.Snapshot(); const WuSnapshot second = m.Snapshot();      // two page visits read the same live state
            Check(first.downloading && second.downloading && first.overallPct == 55 && second.updates[0].pct == 55 && first.currentTitle == L"A", "windows update: the download state is the same on every read, whichever page is showing");
            release = true; waitFor([&] { return !m.Snapshot().downloading; });
        }
    }
    // start-with-Windows task definition and read-back validation
    const std::wstring exe = L"C:\\Program Files\\AnkhCore\\AnkhCore.exe";
    const std::wstring tx = StartupTaskXml(L"S-1-5-21-1-2-3-1001", exe, L"C:\\Program Files\\AnkhCore");
    Check(tx.find(L"<LogonTrigger><Enabled>true</Enabled><UserId>S-1-5-21-1-2-3-1001</UserId><Delay>PT20S</Delay>") != std::wstring::npos, "startup task: logon trigger for this user, 20 s delay");
    Check(tx.find(L"<RunLevel>HighestAvailable</RunLevel>") != std::wstring::npos && tx.find(L"--elevated --startup") != std::wstring::npos, "startup task: same rights as Launch, --startup flag");
    Check(tx.find(L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>") != std::wstring::npos, "startup task: never a second instance");
    Check(StartupXmlValid(tx, exe), "startup read-back: own definition is valid");
    std::wstring disabled = tx; disabled.replace(disabled.find(L"<Enabled>true</Enabled>"), 23, L"<Enabled>false</Enabled>");
    Check(!StartupXmlValid(disabled, exe), "startup read-back: a disabled trigger is not reported as on");
    Check(!StartupXmlValid(tx, L"C:\\Other\\AnkhCore.exe"), "startup read-back: a task for another exe is not reported as on");
    Check(!StartupXmlValid(L"", exe), "startup read-back: missing task is off");
    // ---- monitoring history (bounded, gaps for missing/stale data)
    {
        Ring rg(3); rg.Push(1); rg.Push(2); rg.Push(3); rg.Push(4);
        Check(rg.Size() == 3 && rg.At(0) == 2 && rg.Last() == 4, "ring: bounded, keeps the newest values");
        MonitorStore ms; ms.Configure(5, 600);
        Check(ms.Get(SCpuTemp).Capacity() == 120, "store: capacity = history / interval (600 s / 5 s)");
        ms.Configure(1, 24 * 3600);
        Check(ms.Get(SCpuTemp).Capacity() == MonitorStore::kMaxPoints, "store: capacity capped (memory bounded)");
        ms.Configure(2, 60);
        MonitorSample s; s.fresh = true; s.v[SCpuTemp] = 60; s.v[SGpuTemp] = Present(-1);
        int recorded = 0;
        for (uint64_t t = 0; t < 10; ++t) recorded += ms.Tick(t, s);
        Check(recorded == 5, "store: one point per sampling interval (2 s)");
        Check(!HasValue(ms.Get(SGpuTemp).Last()) && ms.Get(SGpuTemp).Valid() == 0, "store: a sensor that is not exposed stays a gap (never 0)");
        MonitorSample stale = s; stale.fresh = false;
        ms.Tick(20, stale);
        Check(!HasValue(ms.Get(SCpuTemp).Last()), "store: stale helper data is recorded as a gap");
        double lo = 0, hi = 0; Check(ms.Get(SCpuTemp).MinMax(lo, hi) && lo == 60 && hi == 60, "store: min/max ignore gaps");
    }
    // ---- alerts (sustained only; no verdict without data)
    {
        AlertEngine ae; AlertInput in; in.fresh = true; in.cpuTemp = 100;
        int fired = 0;
        for (uint64_t t = 0; t < 5; ++t) fired += static_cast<int>(ae.Tick(t, in).size());   // 5 s Turbo spike
        in.cpuTemp = 62; for (uint64_t t = 5; t < 10; ++t) fired += static_cast<int>(ae.Tick(t, in).size());
        Check(fired == 0, "alerts: a 5 s Turbo spike to 100 C does not alert");
        AlertEngine a2; AlertInput h; h.fresh = true; h.cpuTemp = 98;
        std::vector<std::string> ids;
        for (uint64_t t = 0; t <= 31; ++t) for (auto& e : a2.Tick(t, h)) ids.push_back(e.id);
        Check(ids.size() == 2 && ids[0] == "cpu.crit" && ids[1] == "cpu.warn", "alerts: sustained 98 C -> critical after 15 s, warning after 30 s");
        int again = 0; for (uint64_t t = 32; t < 200; ++t) again += static_cast<int>(a2.Tick(t, h).size());
        Check(again == 0, "alerts: an active alert does not repeat (no spam)");
        h.cpuTemp = 93; for (uint64_t t = 200; t < 230; ++t) a2.Tick(t, h);
        Check(a2.Active("cpu.crit"), "alerts: hysteresis keeps the critical alert until 5 C below the threshold");
        h.cpuTemp = 80; a2.Tick(230, h);
        Check(!a2.Active("cpu.crit") && !a2.Active("cpu.warn"), "alerts: clears once cool");
        h.cpuTemp = 98; int re = 0; for (uint64_t t = 231; t < 280; ++t) re += static_cast<int>(a2.Tick(t, h).size());
        Check(re == 0, "alerts: cooldown prevents a repeat within 5/10 minutes");
        AlertEngine a3; AlertInput na; na.fresh = true;   // GPU/CPU temperature not exposed
        int n3 = 0; for (uint64_t t = 0; t < 100; ++t) n3 += static_cast<int>(a3.Tick(t, na).size());
        Check(n3 == 0, "alerts: an unavailable sensor never alerts (not a hardware failure)");
        AlertEngine a4; AlertInput st; st.fresh = false; st.cpuTemp = 100;
        int n4 = 0; for (uint64_t t = 0; t < 100; ++t) n4 += static_cast<int>(a4.Tick(t, st).size());
        Check(n4 == 0, "alerts: stale data never alerts");
        AlertEngine a5; AlertInput f; f.fresh = true; f.cpuTemp = 88; f.fanRpm = 0; f.fanLive = false;
        int n5 = 0; for (uint64_t t = 0; t < 120; ++t) for (auto& e : a5.Tick(t, f)) n5 += e.id == "fan.stop";
        Check(n5 == 0, "alerts: 0 rpm from an unverified (possibly frozen) fan source never alerts");
        f.fanLive = true; std::vector<std::string> fid;
        for (uint64_t t = 120; t < 200; ++t) for (auto& e : a5.Tick(t, f)) fid.push_back(e.id);
        Check(std::count(fid.begin(), fid.end(), std::string("fan.stop")) == 1, "alerts: live fan at 0 rpm under sustained heat -> one fan alert");
        AlertEngine a6; AlertInput idle; idle.fresh = true; idle.cpuTemp = 45; idle.fanLive = true; idle.fanRpm = 0;
        int n6 = 0; for (uint64_t t = 0; t < 300; ++t) n6 += static_cast<int>(a6.Tick(t, idle).size());
        Check(n6 == 0, "alerts: fan stopped while cool (normal fan-stop mode) is not a failure");

        // M-MON2 (MON-12): RAM, disk free, sustained CPU load, and network throughput rules (live without SensorHub)
        AlertEngine a7;
        a7.Find("cpu.load")->enabled = true;
        a7.Find("net.high")->enabled = true;
        AlertInput win; win.fresh = false; win.ramPct = 92; win.diskFreePct = 8; win.cpuLoad = 96; win.netMbps = 650;
        std::vector<std::string> ev7;
        for (uint64_t t = 0; t < 130; ++t) for (auto& e : a7.Tick(t, win)) ev7.push_back(e.id);
        Check(std::count(ev7.begin(), ev7.end(), std::string("ram.high")) == 1 &&
              std::count(ev7.begin(), ev7.end(), std::string("disk.low")) == 1 &&
              std::count(ev7.begin(), ev7.end(), std::string("cpu.load")) == 1 &&
              std::count(ev7.begin(), ev7.end(), std::string("net.high")) == 1,
              "alerts (MON-12): RAM high, low disk free, sustained CPU load, and network alerts fire on Windows counters even without SensorHub");
    }
    // ---- readings freshness (fresh / stale / timestamp equality / clock granularity)
    {
        const uint64_t now = 133000000000000000ull, s1 = 10000000ull;
        const auto f0 = ReadingsFreshness(now, now);
        Check(f0.present && f0.ageSec == 0, "freshness: written exactly now is fresh (equal timestamps)");
        const auto fa = ReadingsFreshness(now, now + 150000);   // 15 ms ahead: precise file time vs coarse clock
        Check(fa.present && fa.ageSec == 0, "freshness: write time a few ms ahead of now is fresh, age 0");
        const auto f3 = ReadingsFreshness(now, now - 3 * s1);
        Check(f3.present && f3.ageSec > 2.99 && f3.ageSec < 3.01, "freshness: 3 s old is fresh with its age");
        const auto f5 = ReadingsFreshness(now, now - 5 * s1);
        Check(!f5.present && f5.ageSec > 4.99, "freshness: 5 s old is stale");
        const auto fb = ReadingsFreshness(now, now + 10 * s1);
        Check(!fb.present, "freshness: far-future write time (clock moved back) is not trusted as fresh");
    }
    // ---- Mini Monitor: settings, layouts, metric selection, placement
    {
        MiniCfg bad; bad.layout = 7; bad.metrics = 0xFFFF; bad.layer = -3; bad.opacity = 400;
        const MiniCfg s2 = MiniSanitize(bad);
        Check(s2.layout == 1 && s2.metrics == kMiniAllMetrics && s2.layer == 0 && s2.opacity == 100, "mini: settings out of range are clamped");
        Check(MiniSanitize(MiniCfg{0, 0, true, false, 1, 3}).opacity == 10, "mini: opacity never below 10% (stays visible and hittable)");
        MiniCfg ex; ex.layout = 1; ex.graphs = true; ex.metrics = kMiniDefaultMetrics;
        MiniCfg cp = ex; cp.layout = 0;
        const MiniLayout le = MiniLayoutFor(ex), lc = MiniLayoutFor(cp);
        Check(le.rows.size() == 6 && lc.rows.size() == 6, "mini: one row per selected metric (default six)");
        Check(lc.width < le.width && lc.height < le.height, "mini: compact layout is smaller than expanded");
        Check(std::all_of(lc.rows.begin(), lc.rows.end(), [](const MiniRow& r) { return !r.graph; }) && lc.footerH == 0, "mini: compact has no graphs and no footer");
        Check(std::all_of(le.rows.begin(), le.rows.end(), [](const MiniRow& r) { return r.graph; }) && le.footerH > 0, "mini: expanded draws a sparkline per metric and a footer");
        MiniCfg ng = ex; ng.graphs = false;
        Check(MiniLayoutFor(ng).height < le.height, "mini: graphs off shrinks the expanded layout");
        MiniCfg sel = ex; sel.metrics = MNet | MClock | MCpuLoad;
        const MiniLayout ls = MiniLayoutFor(sel);
        Check(ls.rows.size() == 3 && ls.rows[0].metric == MCpuLoad && ls.rows[1].metric == MNet && ls.rows[2].metric == MClock, "mini: selected metrics in a fixed order");
        Check(ls.rows[1].graph && !ls.rows[2].graph, "mini: network has history, the clock has no graph");
        bool ordered = true; for (size_t i = 1; i < le.rows.size(); ++i) ordered &= le.rows[i].top >= le.rows[i - 1].top + le.rows[i - 1].height;
        Check(ordered && le.rows.back().top + le.rows.back().height <= le.footerTop && le.footerTop + le.footerH <= le.height, "mini: rows do not overlap each other or the footer");
        MiniCfg none = ex; none.metrics = 0;
        Check(MiniLayoutFor(none).rows.empty() && MiniLayoutFor(none).height > MiniLayoutFor(none).headerH, "mini: no metric selected still lays out (hint row)");
        Check(MiniSeries(MCpuTemp) == SCpuTemp && MiniSeries(MFan) == SFan && MiniSeries(MClock) == -1, "mini: metrics map to the shared history series");
        const std::vector<MiniRect> works = {{0, 0, 1920, 1040}, {1920, 0, 3840, 1080}};
        const MiniRect p1 = MiniPosition(works, true, 100, 200, 200, 150, 20);
        Check(p1.l == 100 && p1.t == 200, "mini: a valid saved position is kept");
        const MiniRect p2 = MiniPosition({works[0]}, true, 1850, 1000, 200, 150, 20);
        Check(p2.l == 1720 && p2.t == 890, "mini: partly off-screen is pulled fully inside the work area");
        const MiniRect p2b = MiniPosition(works, true, 1850, 1000, 200, 150, 20);   // overlaps screen 2 more
        Check(p2b.l == 1920 && p2b.b <= 1080, "mini: straddling two screens -> the one it overlaps most");
        const MiniRect p3 = MiniPosition(works, true, 3000, 500, 200, 150, 20);
        Check(p3.l == 3000 && p3.t == 500, "mini: a position on the second monitor is kept");
        const MiniRect p4 = MiniPosition({works[0]}, true, 3000, 500, 200, 150, 20);
        Check(p4.l == 1920 - 200 - 20 && p4.t == 1040 - 150 - 20, "mini: monitor gone -> default spot on the primary work area");
        const MiniRect p5 = MiniPosition(works, false, 0, 0, 200, 150, 20);
        Check(p5.l == 1700 && p5.t == 870, "mini: nothing saved -> bottom-right of the primary work area");
    }
    // ---- installer logic (install.h)
    {
        const Ver v22 = ParseVer(L"2.2"), v25 = ParseVer(L"2.5.0"), v250 = ParseVer(L"2.5.0.0"), v26 = ParseVer(L"2.6");
        Check(v22.valid && v22.major == 2 && v22.minor == 2 && v22.patch == 0 && v25.valid && v250.valid, "install: versions parse (2.2, 2.5.0, 2.5.0.0)");
        Check(!ParseVer(L"").valid && !ParseVer(L"2").valid && !ParseVer(L"2.x").valid && !ParseVer(L"2.5.").valid && !ParseVer(L" 2.5").valid, "install: malformed versions are invalid");
        Check(CompareVer(v22, v25) < 0 && CompareVer(v25, v250) == 0 && CompareVer(v26, v25) > 0, "install: version order");
        const Ver p1 = ParseVer(L"2.5.0.1"), p2 = ParseVer(L"2.5.0.2");
        Check(p1.valid && p1.build == 1 && CompareVer(v25, p1) < 0 && CompareVer(ParseVer(L"2.5"), p1) < 0 && CompareVer(p1, p2) < 0 && CompareVer(p2, v26) < 0 && CompareVer(ParseVer(L"2.11.0.0"), ParseVer(kVersion)) == 0 && CompareVer(p2, ParseVer(kVersion)) < 0 && CompareVer(p1, ParseVer(kVersion)) < 0,
              "install: the fourth part is the build; a preview is newer than the plain version and older than the next one; this build (Preview 2.1) is newer than Preview 1 and Preview 2");
        Check(VerText(p1) == L"2.5.0.1" && VerText(v25) == L"2.5.0" && !ParseVer(L"2.5.0.1.9").valid && !ParseVer(L"2.5.0.1 ").valid && !ParseVer(L"2.5.0.x").valid, "install: version text keeps the build; a fifth part or trailing text is invalid");
        Check(PlanKind(true, v25, p1) == InstallKind::Upgrade && PlanKind(true, p1, p1) == InstallKind::Repair && PlanKind(true, p2, p1) == InstallKind::Downgrade && PlanKind(true, v22, p1) == InstallKind::Upgrade && PlanKind(false, Ver{}, p1) == InstallKind::Fresh,
              "install: the preview upgrades 2.2 and the earlier 2.5, repairs itself and refuses to replace a newer build");
        Check(kFanControlEnabled, "fan control is on (owner decision 2026-11-19, as in 2.5.2); the guard and thermal override still apply");
        Check(PlanKind(false, Ver{}, v25) == InstallKind::Fresh, "install: nothing installed -> fresh install");
        Check(PlanKind(true, v22, v25) == InstallKind::Upgrade, "install: 2.2 installed -> upgrade (with backup)");
        Check(PlanKind(true, v25, v250) == InstallKind::Repair, "install: same version -> repair");
        Check(PlanKind(true, v26, v25) == InstallKind::Downgrade, "install: newer installed -> downgrade (refused by default)");
        Check(PlanKind(true, Ver{}, v25) == InstallKind::Upgrade, "install: unreadable installed version still gets a backup (upgrade)");
        Check(ResolveEffectiveInstallKind(SetupMode::Repair, true, v22, v25) == InstallKind::Repair &&
              ResolveEffectiveInstallKind(SetupMode::Update, true, v25, v250) == InstallKind::Upgrade &&
              ResolveEffectiveInstallKind(SetupMode::CleanInstall, true, v26, v25) == InstallKind::Upgrade &&
              ResolveEffectiveInstallKind(SetupMode::CleanInstall, false, Ver{}, v25) == InstallKind::Fresh,
              "install: SetupMode Repair, Update, and CleanInstall resolve effective InstallKind properly");
        std::wstring d;
        Check(PayloadDest(L"APP/AnkhCore.exe", L"C:\\P\\AnkhCore", d) && d == L"C:\\P\\AnkhCore\\AnkhCore.exe", "install: APP/ goes to the program folder");
        Check(PayloadDest(L"SENS/LibreHardwareMonitorLib.dll", L"C:\\P\\AnkhCore", d) && d == L"C:\\P\\AnkhCore\\Sensors\\LibreHardwareMonitorLib.dll", "install: SENS/ goes to program folder\\Sensors");
        Check(PayloadDest(L"SUX/ThemeDll.dll", L"C:\\P\\AnkhCore", d) && d == L"C:\\P\\AnkhCore\\SecureUxTheme\\ThemeDll.dll", "install: SUX/ goes to program folder\\SecureUxTheme");
        Check(!PayloadDest(L"WH/windhawk_setup.exe", L"C:\\P", d) && !PayloadDest(L"SRC/x.wh.cpp", L"C:\\P", d) && !PayloadDest(L"TSP/Magnum.7z", L"C:\\P", d) && !PayloadDest(L"REG/settings.reg", L"C:\\P", d),
              "install: 2.2 areas (Windhawk, mods, settings, icon packs) are refused");
        Check(!PayloadDest(L"APP/../evil.exe", L"C:\\P", d) && !PayloadDest(L"APP/a\\b.exe", L"C:\\P", d) && !PayloadDest(L"APP/C:x", L"C:\\P", d) && !PayloadDest(L"APP/", L"C:\\P", d) && !PayloadDest(L"SENS//x", L"C:\\P", d),
              "install: unsafe payload paths are refused");
        Check(!PayloadDest(L"APP/a.", L"C:\\P", d) && !PayloadDest(L"APP/a ", L"C:\\P", d) && !PayloadDest(L"APP/NUL", L"C:\\P", d) && !PayloadDest(L"APP/con.txt", L"C:\\P", d) && !PayloadDest(L"SENS/Com1.dll", L"C:\\P", d) && !PayloadDest(L"APP/x/LPT9", L"C:\\P", d)
              && PayloadDest(L"APP/AnkhCore.exe", L"C:\\P", d) && PayloadDest(L"SENS/HidSharp.dll", L"C:\\P", d) && PayloadDest(L"APP/COM10.dll", L"C:\\P", d) && PayloadDest(L"APP/console.dll", L"C:\\P", d),
              "install: trailing dots or spaces and reserved device names are refused; real names (COM10, console) are not");
        {   // a file a scanner holds open for reading or writing is still hashed (it used to read as empty and fail a good install)
            wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
            std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-sha-share"; EnsureDir(dir);
            const std::wstring f = dir + L"\\a.bin"; const std::string body = "payload body";
            WriteAll(f, reinterpret_cast<const uint8_t*>(body.data()), body.size());
            HANDLE w = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const std::string want = Sha256Hex(reinterpret_cast<const uint8_t*>(body.data()), body.size());
            Check(w != INVALID_HANDLE_VALUE && !want.empty() && Sha256File(f) == want, "install: Sha256File hashes a file that another process holds open for writing");
            if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
            DeleteFileW(f.c_str()); RemoveDirectoryW(dir.c_str());
        }
        std::vector<ManifestEntry> m;
        const std::string hexA = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        Check(Sha256Hex(reinterpret_cast<const uint8_t*>("abc"), 3) == hexA, "install: SHA-256 known answer (\"abc\")");
        Check(ParseManifest(hexA + "  APP/AnkhCore.exe\n" + hexA + "  SENS/x.dll\r\n", m) && m.size() == 2 && m[1].path == L"SENS/x.dll" && m[0].sha == hexA, "install: manifest parses (LF and CRLF)");
        Check(!ParseManifest("abc  APP/x\n", m) && !ParseManifest(std::string(64, 'G') + "  APP/x\n", m) && !ParseManifest("", m), "install: malformed manifest rejected");
        Check(BackupName(v22, 2026, 11, 8, 14, 5, 9) == L"AnkhCore-2.2.0-20261108-140509", "install: backup folder name");
        Check(std::find(KeptItems().begin(), KeptItems().end(), L"IconPacks") != KeptItems().end() && std::find(ReplacedItems().begin(), ReplacedItems().end(), L"Sensors") != ReplacedItems().end(),
              "install: upgrade replaces the program and helper, keeps appearance content");
    }
    {   // engine protocol: round trip, and malformed input fails cleanly
        EngineFrame f; f.seq = 7; f.usage.cpu = 12.5; f.usage.ramTotal = 1234567890123ull; f.read.present = true; f.read.cpuTemp = 88; f.read.gpuName = L"Quadro"; f.read.fanMode = L"auto"; f.read.stamp.wHour = 21;
        FanReading fr; fr.name = L"CPU Fan"; fr.rpm = 4819; fr.source = L"HP BIOS"; fr.live = false; f.read.fans.push_back(fr);
        AlertEvent ae; ae.id = "cpu.warn"; ae.value = 91; ae.atSec = 500; f.alerts.push_back(ae);
        f.sample.fresh = true; for (double& v : f.sample.v) v = kNoValue; f.sample.v[SCpuTemp] = 88; f.sampleSec = 900;
        IpcStatus st; st.polls = 42; st.hubRun = true; st.pid = 99;
        NetAdapterView na; na.guid = L"{A}"; na.name = L"Wi-Fi 5"; na.type = 71; na.present = na.up = na.isDefault = true; na.signal = 77; na.linkBps = 702000000ull; na.down = 2500000.5; na.upRate = 81000; na.totals[3].rx = 123456789012345ull; na.totals[1].tx = 42; f.net.push_back(na);
        f.sys.cores = {12.5, 100, 0}; f.sys.physCores = 6; f.sys.logical = 12; f.sys.memAvail = 17ull << 30; f.sys.memCached = 9ull << 30; f.sys.commitUsed = 20ull << 30; f.sys.commitLimit = 40ull << 30; f.sys.diskRead = 1234.5;
        DriveInfo dv; dv.letter = L'C'; dv.total = 476ull << 30; dv.free = 120ull << 30; dv.writeBps = 77; f.sys.drives.push_back(dv);
        GpuAdapterView ga; ga.name = L"Intel(R) UHD Graphics 630"; ga.luid.lo = 0xE3C5; ga.util = 57.5; ga.sharedUsed = 275ull << 20; ga.dedicatedTotal = 128ull << 20; f.gpus.push_back(ga);
        GpuAdapterView gb; gb.name = L"Microsoft Basic Render Driver"; gb.luid.hi = 7; gb.luid.lo = 0xE877; gb.software = true; gb.util = -1; f.gpus.push_back(gb);
        NetAdapterView nb; nb.guid = L"{B}"; nb.name = L"Ethernet 11"; nb.type = 6; nb.totals[3].tx = 9; f.net.push_back(nb);
        const auto enc = IpcEncodeFrame(f, st);
        EngineFrame g; IpcStatus st2; Check(IpcDecodeFrame(enc.data(), enc.size(), g, st2), "ipc: frame decodes");
        Check(g.seq == 7 && g.usage.ramTotal == 1234567890123ull && g.read.cpuTemp == 88 && g.read.gpuName == L"Quadro" && g.read.stamp.wHour == 21, "ipc: frame values survive");
        Check(g.read.fans.size() == 1 && g.read.fans[0].rpm == 4819 && g.read.fans[0].source == L"HP BIOS" && !g.read.fans[0].live, "ipc: fans keep rpm, source and live state");
        Check(g.alerts.size() == 1 && g.alerts[0].id == "cpu.warn" && !HasValue(g.sample.v[SRam]) && g.sample.v[SCpuTemp] == 88 && st2.polls == 42 && st2.pid == 99, "ipc: alerts, gaps and status survive");
        Check(g.net.size() == 2 && g.net[0].guid == L"{A}" && g.net[0].name == L"Wi-Fi 5" && g.net[0].type == 71 && g.net[0].present && g.net[0].up && g.net[0].isDefault && g.net[0].signal == 77 && g.net[0].linkBps == 702000000ull
              && g.net[0].down == 2500000.5 && g.net[0].upRate == 81000 && g.net[0].totals[3].rx == 123456789012345ull && g.net[0].totals[1].tx == 42, "ipc: an adapter keeps identity, state, rates and totals");
        Check(g.sys.cores.size() == 3 && g.sys.cores[1] == 100 && g.sys.physCores == 6 && g.sys.logical == 12 && g.sys.memCached == (9ull << 30) && g.sys.commitLimit == (40ull << 30) && g.sys.diskRead == 1234.5 && g.sys.diskWrite == -1
              && g.sys.drives.size() == 1 && g.sys.drives[0].letter == L'C' && g.sys.drives[0].free == (120ull << 30) && g.sys.drives[0].readBps == -1 && g.sys.drives[0].writeBps == 77, "ipc: per-core usage, memory detail and drives survive");
        Check(g.gpus.size() == 2 && g.gpus[0].name == L"Intel(R) UHD Graphics 630" && g.gpus[0].luid.lo == 0xE3C5 && g.gpus[0].util == 57.5 && g.gpus[0].sharedUsed == (275ull << 20) && g.gpus[0].dedicatedTotal == (128ull << 20) && !g.gpus[0].software
              && g.gpus[1].software && g.gpus[1].luid.hi == 7 && g.gpus[1].util == -1, "ipc: graphics adapters keep name, LUID, utilization and memory");
        Check(g.net[1].guid == L"{B}" && !g.net[1].present && g.net[1].signal == -1 && g.net[1].down == -1 && g.net[1].totals[3].tx == 9, "ipc: an absent adapter keeps its history and unknown rates");
        { IpcWriter many; EngineFrame e2; const auto base = IpcEncodeFrame(e2, st); many.b = base; many.b.resize(many.b.size() - 4); many.U32(65); Check(!IpcDecodeFrame(many.b.data(), many.b.size(), g, st2), "ipc: an impossible adapter count is refused"); }
        bool allFail = true; for (size_t cut = 0; cut + 1 < enc.size(); cut += 17) allFail &= !IpcDecodeFrame(enc.data(), cut, g, st2);
        Check(allFail, "ipc: every truncated frame is refused");
        auto extra = enc; extra.push_back(0); Check(!IpcDecodeFrame(extra.data(), extra.size(), g, st2), "ipc: trailing bytes are refused");
        IpcWriter bad; bad.U32(0x7FFFFFFF); IpcReader br(bad.b.data(), bad.b.size()); (void)br.S(); Check(!br.Ok(), "ipc: an impossible string length is refused without allocating");
        uint8_t hd[12]; IpcWriter hw; hw.U32(kIpcMagic); hw.U32(IpcFrame); hw.U32(kIpcMaxPayload + 1); memcpy(hd, hw.b.data(), 12); uint32_t ty = 0, ln = 0; Check(!IpcHeaderOk(hd, ty, ln), "ipc: an oversized message is refused");
        IpcWriter hx; hx.U32(1); hx.U32(IpcFrame); hx.U32(1); memcpy(hd, hx.b.data(), 12); Check(!IpcHeaderOk(hd, ty, ln), "ipc: a wrong magic is refused");
        MonitorStore ms; ms.Configure(5, 300); MonitorSample s1; s1.fresh = true; for (double& v : s1.v) v = kNoValue; s1.v[SRam] = 40; ms.Tick(5, s1); s1.v[SRam] = 50; ms.Tick(10, s1);
        MonitorStore back; const auto he = IpcEncodeHistory(ms);
        Check(IpcDecodeHistory(he.data(), he.size(), back) && back.Interval() == 5 && back.Get(SRam).Size() == ms.Get(SRam).Size() && back.Get(SRam).Last() == 50 && !HasValue(back.Get(SCpuTemp).Last()), "ipc: history replays with gaps intact");
        std::vector<AlertRule> rules; AlertRule ru; ru.id = "cpu.warn"; ru.threshold = 90; ru.holdSec = 30; rules.push_back(ru); int iv = 0, hs = 0; std::vector<AlertRule> o;
        const auto ce = IpcEncodeConfigure(5, 300, rules); Check(IpcDecodeConfigure(ce.data(), ce.size(), iv, hs, o) && iv == 5 && hs == 300 && o.size() == 1 && o[0].threshold == 90 && o[0].holdSec == 30, "ipc: configuration round trip");
    }
    {   // network accounting: nothing doubtful may ever be added
        const uint64_t G = 1000000000ull;
        auto S = [&](const wchar_t* g, uint64_t rx, uint64_t tx) { IfSample i; i.guid = g; i.name = g; i.type = 71; i.rx = rx; i.tx = tx; i.linkBps = G; return i; };
        NetCounters n;
        n.Tick({S(L"w", 5000, 100)}, 1, 20261109, 100);
        Check(n.Get(L"w", NetPeriod::Lifetime).rx == 0, "net: the first sighting is only a baseline");
        n.Tick({S(L"w", 6000, 150)}, 1, 20261109, 101);
        Check(n.Get(L"w", NetPeriod::Session).rx == 1000 && n.Get(L"w", NetPeriod::Lifetime).tx == 50, "net: deltas are counted");
        n.Tick({S(L"w", 200, 10)}, 1, 20261109, 102);
        Check(n.Get(L"w", NetPeriod::Lifetime).rx == 1000, "net: a counter reset adds nothing");
        n.Tick({S(L"w", 300, 20)}, 1, 20261109, 103);
        Check(n.Get(L"w", NetPeriod::Lifetime).rx == 1100, "net: counting resumes from the new baseline");
        n.Tick({S(L"w", 300 + 60 * G, 20)}, 1, 20261109, 104);
        Check(n.Get(L"w", NetPeriod::Lifetime).rx == 1100, "net: an impossible jump for the link speed is rejected");
        n.Tick({}, 1, 20261109, 105);
        n.Tick({S(L"w", 500 + 60 * G, 20)}, 1, 20261109, 106);
        Check(n.Get(L"w", NetPeriod::Lifetime).rx == 1100, "net: an adapter coming back is baseline only");
        n.Tick({S(L"w", 700 + 60 * G, 20), S(L"e", 9000, 1)}, 1, 20261109, 107);
        n.Tick({S(L"w", 800 + 60 * G, 20), S(L"e", 9400, 1)}, 1, 20261109, 108);
        Check(n.Get(L"e", NetPeriod::Lifetime).rx == 400 && n.Get(L"w", NetPeriod::Lifetime).rx == 1400, "net: each adapter keeps its own history");
        Check(n.All(NetPeriod::Lifetime).rx == 1800, "net: All adapters is the sum, counted once");
        n.Tick({S(L"w", 900 + 60 * G, 20), S(L"e", 9500, 1)}, 1, 20261110, 109);
        Check(n.Get(L"w", NetPeriod::Today).rx == 100 && n.Get(L"w", NetPeriod::Lifetime).rx == 1500, "net: the day rolls over without touching lifetime");
        const std::string saved = n.Save(109);
        NetCounters r; Check(r.Load(saved), "net: saved totals load");
        Check(r.Get(L"w", NetPeriod::Lifetime).rx == 1500 && r.Get(L"w", NetPeriod::Session).rx == 0, "net: lifetime persists, session does not");
        r.Tick({S(L"w", 1900 + 60 * G, 20)}, 1, 20261110, 119);
        Check(r.Get(L"w", NetPeriod::Lifetime).rx == 2500 && r.Get(L"w", NetPeriod::Session).rx == 0, "net: a restart in the same boot adds the gap once, outside the session");
        NetCounters b; b.Load(saved); b.Tick({S(L"w", 7000, 20)}, 2, 20261110, 500);
        Check(b.Get(L"w", NetPeriod::Boot).rx == 0 && b.Get(L"w", NetPeriod::Lifetime).rx == 1500, "net: a new boot clears since-boot and guesses nothing");
        NetCounters d; Check(!d.Load("ankh-net 1\nboot 1\na x") && !d.Load("garbage") && d.adapters.empty(), "net: a damaged file is refused");
        Check(!d.Load(saved.substr(0, saved.size() - 4)), "net: a truncated file is refused");
        // names, the old file format, rates and the three selection modes
        NetCounters nm; IfSample wi = S(L"{W}", 1000, 10); wi.name = L"Wi-Fi 5 \u00E9"; wi.isDefault = true; wi.signal = 77; IfSample et = S(L"{E}", 50, 5); et.name = L"Ethernet 11"; et.type = 6; et.up = false;
        nm.Tick({wi, et}, 1, 20261110, 100, 5000);
        Check(nm.Views().size() == 2 && nm.Views()[1].down == -1, "net: no rate is shown before a second sample");
        wi.rx = 3000; wi.tx = 510; nm.Tick({wi, et}, 1, 20261110, 101, 5500);
        const auto vw = nm.Views();
        Check(vw[1].guid == L"{W}" && vw[1].down == 4000 && vw[1].upRate == 1000 && vw[1].totals[0].rx == 2000, "net: rates use the precise elapsed time (2000 bytes in 0.5 s)");
        NetCounters nr; Check(nr.Load(nm.Save(101)) && nr.adapters[L"{W}"].name == L"Wi-Fi 5 \u00E9" && nr.adapters[L"{E}"].name == L"Ethernet 11", "net: adapter names survive the file (spaces, non-ASCII)");
        Check(nr.Load("ankh-net 1\nboot 1\nday 20261110\nat 101\na {W} 71 5 6 7 8 9 10 1 3000 510\nend\n") && nr.Get(L"{W}", NetPeriod::Lifetime).rx == 9, "net: a format 1 file still loads");
        Check(!nr.Load("ankh-net 2\nboot 1\nday 20261110\nat 101\na {W} 71 5 6 7 8 9 10 1 3000 510 zz\nend\n") && !nr.Load("ankh-net 3\nend\n"), "net: a bad name field or unknown format is refused");
        const NetSelection sa = NetSelect(vw, NetAuto, L"");
        Check(sa.valid && sa.connected && sa.guid == L"{W}" && sa.down == 4000 && sa.signal == 77 && sa.totals[3].rx == 2000, "net select: Automatic follows the default-route adapter");
        const NetSelection sl = NetSelect(vw, NetAll, L"");
        Check(sl.connected && sl.count == 1 && sl.guid.empty() && sl.down == 4000 && sl.totals[3].rx == 2000 && sl.totals[3].tx == 500, "net select: All adds connected adapters' rates and every record's totals");
        const NetSelection sm = NetSelect(vw, NetManual, L"{E}");
        Check(sm.valid && !sm.connected && sm.name == L"Ethernet 11" && sm.down == -1 && sm.guid == L"{E}", "net select: Manual on a disconnected adapter shows it as disconnected, never another one");
        const NetSelection sx = NetSelect(vw, NetManual, L"{gone}");
        Check(!sx.connected && sx.count == 0 && sx.down == -1, "net select: Manual on an unknown adapter substitutes nothing");
        auto nodef = vw; for (auto& a : nodef) a.isDefault = false;
        Check(!NetSelect(nodef, NetAuto, L"").connected && !NetSelect({}, NetAuto, L"").valid && NetSelect(vw, 99, L"").mode == NetAuto, "net select: no default route, no data and a bad mode are handled");
        nm.Tick({et}, 1, 20261110, 102, 6500);
        const NetSelection gone = NetSelect(nm.Views(), NetAll, L"");
        Check(!gone.connected && gone.down == -1 && gone.totals[3].rx == 2000, "net select: an adapter that left keeps its history in All adapters");
    }
    {   // sidebar sections: order, visibility, density and the fit to the screen height
        auto all = SbParseSections(L"");
        Check(all.size() == SbSecCount && all[0].sec == SecCpu && all[5].sec == SecFans && SbFormatSections(all) == L"cpu,gpu,mem,storage,net,fans", "sidebar: default order shows every section");
        auto cu = SbParseSections(L"net, -gpu ,bogus,net,cpu");
        Check(cu.size() == SbSecCount && cu[0].sec == SecNet && cu[1].sec == SecGpu && !cu[1].on && cu[2].sec == SecCpu && cu[3].sec == SecMem && cu[3].on, "sidebar: custom order parses; unknown and repeated names are ignored; unnamed sections follow");
        Check(SbFormatSections(SbParseSections(SbFormatSections(cu))) == SbFormatSections(cu), "sidebar: section list round trip");
        auto mv = all; SbMoveSection(mv, SecCpu, -1); SbMoveSection(mv, SecFans, 1); Check(SbFormatSections(mv) == SbFormatSections(all), "sidebar: the first section cannot move up, the last cannot move down");
        SbMoveSection(mv, SecMem, -1); SbShowSection(mv, SecStorage, false); Check(SbFormatSections(mv) == L"cpu,mem,gpu,-storage,net,fans", "sidebar: move and hide");
        SbCounts cn; cn.cores = 12; cn.drives = 2; cn.fans = 2;
        const SbLayout mn = SbLayoutFor(SbMinimal, all, cn, 2000), sd = SbLayoutFor(SbStandard, all, cn, 2000), dt = SbLayoutFor(SbDetailed, all, cn, 2000);
        auto rows = [](const SbLayout& l) { size_t n = 0; for (const auto& b : l.boxes) n += b.rows.size(); return n; };
        Check(mn.boxes.size() == 6 && sd.boxes.size() == 6 && dt.boxes.size() == 6 && rows(mn) < rows(sd) && rows(sd) < rows(dt) && !dt.reduced && !dt.dropped, "sidebar: each density adds rows");
        bool ordered = true, inside = true; int prev = 0;
        for (const auto& b : dt.boxes) { ordered &= b.top >= prev; int ry = b.top; for (const auto& r : b.rows) { inside &= r.top >= ry && r.top + r.h <= b.top + b.h; ry = r.top + r.h; } prev = b.top + b.h; }
        Check(ordered && inside && dt.used == prev && dt.used <= 2000, "sidebar: sections never overlap and rows stay inside their section");
        bool cores = false; for (const auto& r : dt.boxes[0].rows) if (r.id == RCpuCores) cores = r.h == 2 * kSbCoreH + 4;
        Check(cores && SbCoreRows(64) == 4 && SbCoresPerRow(64) == 16 && SbCoreRows(0) == 0, "sidebar: 12 logical processors take two rows; many cores stay within four rows");
        int natural = 0; for (int i = 0; i < SbSecCount; ++i) natural += SbBoxH(static_cast<SbSec>(i), SbStandard, cn) + (i ? kSbGap : 0);   // Standard with nothing stretched
        const SbLayout nat = SbLayoutFor(SbStandard, all, cn, natural);
        const int tight = natural + 60;   // room for the first section's extra detail, not for everyone's
        const SbLayout rd = SbLayoutFor(SbDetailed, all, cn, tight);
        Check(rd.boxes.size() == 6 && rd.reduced > 0 && !rd.dropped && rd.used <= tight && rd.boxes[0].level == SbDetailed && rd.boxes[5].level < SbDetailed, "sidebar: too tall -> detail is removed from the bottom section first");
        const SbLayout tiny = SbLayoutFor(SbDetailed, all, cn, 150);
        Check(tiny.boxes.size() < 6 && tiny.dropped == 6 - static_cast<int>(tiny.boxes.size()) && tiny.used <= 150 && (tiny.boxes.empty() || tiny.boxes[0].sec == SecCpu), "sidebar: a very short screen keeps the first sections and drops the rest");
        Check(SbLayoutFor(SbStandard, all, cn, 10).boxes.empty() && SbLayoutFor(99, all, cn, 2000).boxes.size() == 6, "sidebar: no room and a bad density are handled");
        const SbLayout roomy = SbLayoutFor(SbStandard, all, cn, natural + 400);
        int g0 = 0, g1 = 0; for (const auto& r : nat.boxes[0].rows) if (r.id == RCpuGraph) g0 = r.h; for (const auto& r : roomy.boxes[0].rows) if (r.id == RCpuGraph) g1 = r.h;
        Check(nat.used == natural && !nat.reduced && g0 == kSbGraphH && g1 > g0 && roomy.used > natural && roomy.used <= natural + 400, "sidebar: spare height goes to the graphs and the gaps, never past the screen");
        auto hid = all; SbShowSection(hid, SecGpu, false); SbShowSection(hid, SecFans, false);
        const SbLayout hl = SbLayoutFor(SbStandard, hid, cn, 2000); bool none = true; for (const auto& b : hl.boxes) none &= b.sec != SecGpu && b.sec != SecFans;
        Check(hl.boxes.size() == 4 && none, "sidebar: hidden sections take no space");
        SbCounts nf = cn; nf.fans = 0; nf.drives = 9; const SbLayout lf = SbLayoutFor(SbStandard, all, nf, 2000);
        Check(lf.boxes[5].rows.size() == 1 && lf.boxes[5].rows[0].id == RFanTop && lf.boxes[3].rows.size() == kSbMaxList + 1, "sidebar: no fans still shows the fan state; long drive lists are capped");
    }
    {   // sidebar customization: styles, custom levels, spacing, colours, profiles
        const auto all = SbParseSections(L""); SbCounts cn; cn.cores = 12; cn.drives = 3; cn.fans = 2;
        const SbPerSec ps = SbParsePerSec(L"cpu:2, gpu:0,bogus:1,mem:9,net:x,fans:12,storage", 1, 0, 2);
        Check(ps.v[SecCpu] == 2 && ps.v[SecGpu] == 0 && ps.v[SecMem] == 1 && ps.v[SecNet] == 1 && ps.v[SecFans] == 1 && ps.v[SecStorage] == 1, "sidebar: per-section numbers parse; bad entries keep the default");
        Check(SbFormatPerSec(ps) == L"cpu:2,gpu:0,mem:1,storage:1,net:1,fans:1" && SbFormatPerSec(SbParsePerSec(SbFormatPerSec(ps), 0, 0, 2)) == SbFormatPerSec(ps), "sidebar: per-section numbers round trip");
        SbLook lk; const SbLayout bars = SbLayoutFor(lk, all, cn, 3000);
        auto count = [](const SbLayout& l, SbRowKind k) { int n = 0; for (const auto& b : l.boxes) for (const auto& r : b.rows) if (r.kind == k) ++n; return n; };
        Check(count(bars, SbGauge) == 0 && count(bars, SbValue) == 0 && count(bars, SbSpark) == 0 && SbLayoutFor(SbStandard, all, cn, 3000).used == bars.used, "sidebar: the default look is indicators, same as the plain density call");
        SbLook g = lk; for (int& v : g.styles.v) v = SbStyleGauge; const SbLayout gl = SbLayoutFor(g, all, cn, 3000);
        bool pairsOk = true, lone = false; for (const auto& b : gl.boxes) for (size_t i = 0; i < b.rows.size(); ++i) { const SbRow& r = b.rows[i]; if (r.kind != SbGauge) continue;
            if (r.cols == 2 && r.col == 0) pairsOk &= i + 1 < b.rows.size() && b.rows[i + 1].top == r.top && b.rows[i + 1].col == 1 && b.rows[i + 1].h == r.h; if (r.cols == 1) lone = true; }
        Check(count(gl, SbBar) == 0 && count(gl, SbGauge) == 2 + 2 + 1 + 3 + 2 + 2 && pairsOk && lone, "sidebar: gauges replace every indicator and sit two to a line; an odd one stands alone");
        Check(gl.boxes[0].rows[0].h == kSbGaugeH && gl.boxes[0].rows[2].top == gl.boxes[0].rows[0].top + kSbGaugeH, "sidebar: a pair of gauges takes one gauge row of height");
        SbLook vv = lk; for (int& v : vv.styles.v) v = SbStyleValue; const SbLayout vl = SbLayoutFor(vv, all, cn, 3000);
        int natBars = 0, natVals = 0; for (int i = 0; i < SbSecCount; ++i) { natBars += SbBoxH(static_cast<SbSec>(i), SbStandard, cn, SbStyleBar); natVals += SbBoxH(static_cast<SbSec>(i), SbStandard, cn, SbStyleValue); }
        Check(count(vl, SbValue) == 12 && count(vl, SbBar) == 0 && natVals < natBars, "sidebar: numeric values need less height than indicators");
        SbLook gr = lk; for (int& v : gr.styles.v) v = SbStyleGraph; const SbLayout grl = SbLayoutFor(gr, all, cn, 3000);
        bool cpuGraph = false, netGraph = false; { SbLook d2 = gr; d2.density = SbDetailed; for (const auto& b : SbLayoutFor(d2, all, cn, 5000).boxes) for (const auto& r : b.rows) { cpuGraph |= r.id == RCpuGraph; netGraph |= r.id == RNetGraph; } }
        Check(count(grl, SbSpark) == 5 + 2 && count(grl, SbBar) == 3 + 2 && !cpuGraph && !netGraph, "sidebar: graph style gives metrics with history (and, since 2.9.6, download and upload) a sparkline, keeps drives and single fans as indicators, and drops the duplicate section graphs");
        SbLook cu = lk; cu.density = SbCustom; cu.levels = SbParsePerSec(L"cpu:2,gpu:0,net:2", 1, 0, 2); const SbLayout cl = SbLayoutFor(cu, all, cn, 3000);
        Check(cl.boxes[0].level == 2 && cl.boxes[1].level == 0 && cl.boxes[2].level == 1 && cl.boxes[4].level == 2 && !cl.reduced, "sidebar: Custom gives each section its own detail level");
        int need = 0; for (int i = 0; i < SbSecCount; ++i) need += SbBoxH(static_cast<SbSec>(i), cu.levels.v[i], cn) + (i ? kSbGap : 0);
        const SbLayout cs = SbLayoutFor(cu, all, cn, need - 20);
        Check(cs.reduced == 1 && cs.boxes[4].level == SbStandard && cs.boxes[0].level == SbDetailed && cs.used <= need - 20, "sidebar: Custom too tall -> only the lowest detailed section is reduced, and it is counted");
        SbLook s0 = lk, s2 = lk; s0.spacing = 0; s2.spacing = 2; int n0 = 0, n1 = 0, n2 = 0;
        for (int i = 0; i < SbSecCount; ++i) { n0 += SbBoxH(static_cast<SbSec>(i), 1, cn, 0, 0) + (i ? SbGapFor(0) : 0); n1 += SbBoxH(static_cast<SbSec>(i), 1, cn, 0, 1) + (i ? SbGapFor(1) : 0); n2 += SbBoxH(static_cast<SbSec>(i), 1, cn, 0, 2) + (i ? SbGapFor(2) : 0); }
        Check(n0 < n1 && n1 < n2 && SbLayoutFor(s0, all, cn, n0).used == n0 && SbLayoutFor(s2, all, cn, n2).used == n2 && !SbLayoutFor(s2, all, cn, n2).reduced, "sidebar: compact, normal and relaxed spacing");
        SbLook bad; bad.density = 77; bad.spacing = -4; bad.styles.v[0] = 99; bad.levels.v[0] = -3;
        Check(SbLayoutFor(bad, all, cn, 3000).boxes.size() == 6, "sidebar: out-of-range look values are clamped");
        Check(SbColorStep(0x10FF00, 0, -32) == 0x00FF00 && SbColorStep(0x10FF00, 1, 16) == 0x10FF00 && SbColorStep(0x10FF00, 2, 16) == 0x10FF10 && SbColorStep(-1, 0, 16) == -1 && SbColorHex(0x00AEFF) == L"#00AEFF" && SbColorHex(-1) == L"theme", "sidebar: colour steps clamp per channel; hex text");
        const auto dp = SbProfileDecode(L"sidebar.width=260|sidebar.styles=cpu:1,gpu:3|sidebar.accent=-1|sidebar.accent2=16777215|bogus=1|sidebar.edge=0|sidebar.width=999|sidebar.opacity=abc|sidebar.tint=|sidebar.sections=cpu,<b>|sidebar.glow=2|noequals|sidebar.glow=1");
        auto val = [&](const wchar_t* k) { for (const auto& p : dp) if (p.first == k) return p.second; return std::wstring(L"?"); };
        Check(dp.size() == 5 && val(L"sidebar.width") == L"260" && val(L"sidebar.styles") == L"cpu:1,gpu:3" && val(L"sidebar.accent") == L"-1" && val(L"sidebar.accent2") == L"16777215" && val(L"sidebar.glow") == L"1",
              "sidebar profile: only known keys with valid values survive; the last valid value wins");
        Check(val(L"sidebar.edge") == L"?" && val(L"sidebar.opacity") == L"?" && val(L"sidebar.sections") == L"?" && val(L"bogus") == L"?", "sidebar profile: placement keys, unknown keys and malformed values are dropped");
        Check(SbProfileDecode(SbProfileEncode(dp)) == dp && SbProfileDecode(L"").empty() && SbProfileDecode(L"|||=|").empty(), "sidebar profile: round trip; empty and junk text give nothing");
        bool builtins = SbBuiltinProfiles().size() == 4;
        for (const SbBuiltin& b : SbBuiltinProfiles()) { const auto kv = SbProfileDecode(b.text); size_t parts = 1; for (const wchar_t* q = b.text; *q; ++q) parts += *q == L'|'; builtins &= kv.size() == parts && kv.size() >= 10; }
        Check(builtins && SbProfileDecode(SbBuiltinProfiles()[0].text).size() == SbProfileKeys().size() - 1, "sidebar profile: every built-in profile is fully valid; Default sets every look key");
    }
    {   // widgets (M-WID1): Mini size and corners; Sidebar position, auto-hide and drag target
        MiniCfg d; const MiniLayout l0 = MiniLayoutFor(d);
        Check(l0.width == 232 && l0.rowH == 22 && d.scale == 100 && d.background == -1 && d.accent == -1 && d.glow == 0, "mini: the defaults keep the 2.5 layout and look");
        MiniCfg w = d; w.width = 300; w.rowHeight = 30; const MiniLayout lw = MiniLayoutFor(w);
        Check(lw.width == 300 && lw.rowH == 30 && lw.height > l0.height && lw.rows.size() == l0.rows.size() && lw.rows[1].top - lw.rows[0].top == l0.rows[1].top - l0.rows[0].top + 8, "mini: custom width and row height resize the widget");
        MiniCfg cw = d; cw.layout = 0; cw.width = 200; Check(MiniLayoutFor(cw).width == 200 && MiniLayoutFor(cw).rowH == 19, "mini: the compact layout takes a custom width too");
        MiniCfg bad; bad.width = 9000; bad.rowHeight = 3; bad.scale = 1000; bad.background = 9; bad.tint = -5; bad.accent = 0x7FFFFFFF; bad.textColor = -9; bad.glow = 7;
        const MiniCfg sb = MiniSanitize(bad);
        Check(sb.width == 400 && sb.rowHeight == 16 && sb.scale == 200 && sb.background == 2 && sb.tint == 0 && sb.accent == 0xFFFFFF && sb.textColor == -1 && sb.glow == 2, "mini: out-of-range customization is clamped");
        // M-MON2 (MON-8): Mini Monitor profiles and per-metric display styles
        const auto& mProfs = MiniBuiltinProfiles();
        MiniCfg parsedM;
        const auto stArr = MiniParseStyles(L"1,2,0,1,0,0,2,0");
        Check(mProfs.size() >= 4 && MiniParseProfile(mProfs[2].text, parsedM) &&
              MiniSerializeProfile(parsedM) == mProfs[2].text &&
              stArr[0] == MiniStyleBar && stArr[1] == MiniStyleSpark &&
              MiniFormatStyles(stArr) == L"1,2,0,1,0,0,2,0",
              "mini (MON-8): built-in profiles and per-metric display styles round-trip");
        const MiniRect wa{0, 0, 1920, 1032};
        const MiniRect c0 = MiniCorner(wa, 0, 232, 300, 20), c1 = MiniCorner(wa, 1, 232, 300, 20), c2 = MiniCorner(wa, 2, 232, 300, 20), c3 = MiniCorner(wa, 3, 232, 300, 20);
        Check(c0.l == 20 && c0.t == 20 && c1.r == 1900 && c1.t == 20 && c2.l == 20 && c2.b == 1012 && c3.r == 1900 && c3.b == 1012, "mini: the four corners keep a margin");
        const MiniRect tiny = MiniCorner(MiniRect{100, 100, 300, 250}, 3, 232, 300, 20);
        Check(tiny.l == 100 && tiny.t == 100, "mini: a corner in a work area smaller than the widget starts at its origin");
        Check(SbTopFor(0, 1032, 516, 0) == 0 && SbTopFor(0, 1032, 516, 50) == 258 && SbTopFor(0, 1032, 516, 100) == 516 && SbTopFor(40, 1072, 2000, 70) == 40 && SbTopFor(0, 1032, 516, 400) == 516, "sidebar: position along the edge, clamped, never past the work area");
        Check(SbSlideOffset(210, 3, 0) == 0 && SbSlideOffset(210, 3, 1) == 207 && SbSlideOffset(210, 3, 0.5) > 103 && SbSlideOffset(210, 3, 0.5) < 207 && SbSlideOffset(210, 3, 7) == 207 && SbSlideOffset(2, 3, 1) == 0, "sidebar: slide offset eases out and leaves the peek strip");
        SbHide h;
        Check(!SbHideNext(h, true, false, false, 1000, 700) && !h.hidden, "sidebar auto-hide: stays while the pointer is on it");
        Check(!SbHideNext(h, false, false, false, 1100, 700) && !SbHideNext(h, false, false, false, 1700, 700) && !h.hidden, "sidebar auto-hide: waits out the delay after the pointer leaves");
        Check(!SbHideNext(h, true, false, false, 1750, 700) && !SbHideNext(h, false, false, false, 1800, 700) && !SbHideNext(h, false, false, false, 2400, 700), "sidebar auto-hide: coming back restarts the delay");
        Check(SbHideNext(h, false, false, false, 2500, 700) && h.hidden, "sidebar auto-hide: hides once the delay has passed");
        Check(!SbHideNext(h, false, false, false, 9000, 700) && h.hidden && SbHideNext(h, false, true, false, 9100, 700) && !h.hidden, "sidebar auto-hide: returns as soon as the pointer touches the edge strip");
        SbHide hd; SbHideNext(hd, false, false, false, 100, 700);
        Check(!SbHideNext(hd, false, false, true, 5000, 700) && !hd.hidden && !SbHideNext(hd, false, false, false, 5100, 700) && SbHideNext(hd, false, false, false, 5800, 700), "sidebar auto-hide: a drag holds it, and the delay starts again afterwards");
        const std::vector<SbRect> two = {{0, 0, 1920, 1032}, {1920, 0, 3840, 1080}};
        const SbDrop a = SbDropTarget(two, 300, 700, 516, 100), b = SbDropTarget(two, 3700, 2000, 540, 0), c2d = SbDropTarget(two, 1000, 5, 1032, 400), e = SbDropTarget(two, -500, 400, 516, 16);
        Check(a.monitor == 0 && a.edge == 0 && a.posPct == 100 && b.monitor == 1 && b.edge == 1 && b.posPct == 100, "sidebar drag: lands on the screen under the pointer, at its nearer side");
        Check(c2d.monitor == 0 && c2d.edge == 1 && c2d.posPct == 0 && e.monitor == 0 && e.edge == 0 && e.posPct == 74 && SbDropTarget({}, 5, 5, 10, 0).monitor == 0, "sidebar drag: a full-height Sidebar has no travel; a pointer off every screen uses the nearest; no screens is handled");
    }
    {   // GPU utilization (regression: the reading was stuck at the idle GPU's 0 %)
        GpuLuid l; int ph = -1, en = -1; std::wstring ty;
        Check(GpuParseEngine(L"pid_10468_luid_0x00000000_0x0000E3C5_phys_0_eng_0_engtype_3D", l, ph, en, ty) && l.hi == 0 && l.lo == 0xE3C5 && ph == 0 && en == 0 && ty == L"3d", "gpu: a counter instance name parses (as PDH gives it)");
        Check(GpuParseEngine(L"pid_4_luid_0x0000000a_0x0000e8a9_phys_1_eng_11_engtype_gdi render", l, ph, en, ty) && l.hi == 0xA && l.lo == 0xE8A9 && ph == 1 && en == 11 && ty == L"gdi render", "gpu: lower case and an engine type with a space parse");
        Check(!GpuParseEngine(L"", l, ph, en, ty) && !GpuParseEngine(L"_Total", l, ph, en, ty) && !GpuParseEngine(L"pid_1_luid_0x1_phys_0_eng_0_engtype_3D", l, ph, en, ty) && !GpuParseEngine(L"pid_1_luid_0x123456789_0x1_phys_0_eng_0_engtype_3D", l, ph, en, ty)
              && !GpuParseEngine(L"pid_x_luid_0x0_0x1_phys_0_eng_0_engtype_3D", l, ph, en, ty) && !GpuParseEngine(L"pid_1_luid_0x0_0x1_phys_0_eng_0_engtype_", l, ph, en, ty), "gpu: malformed instance names are refused");
        Check(GpuParseAdapter(L"luid_0x00000000_0x0000E8A9_phys_0", l) && l.lo == 0xE8A9 && !GpuParseAdapter(L"luid_0x00000000_0x0000E8A9_phys_0_eng_1", l) && !GpuParseAdapter(L"pid_1_luid_0x0_0x1_phys_0", l), "gpu: adapter memory instance names parse");
        const std::wstring I = L"_luid_0x00000000_0x0000E3C5_phys_0_", N = L"_luid_0x00000000_0x0000E8A9_phys_0_";
        std::vector<GpuCounterSample> cs = {
            {L"pid_100" + I + L"eng_0_engtype_3D", 30.0}, {L"pid_200" + I + L"eng_0_engtype_3D", 12.5}, {L"pid_300" + I + L"eng_0_engtype_3D", 0.0},   // one engine, three processes
            {L"pid_100" + I + L"eng_1_engtype_VideoDecode", 61.0}, {L"pid_100" + I + L"eng_2_engtype_Copy", 3.0},
            {L"pid_100" + N + L"eng_0_engtype_3D", 0.0}, {L"pid_100" + N + L"eng_4_engtype_Copy", 0.3},
            {L"garbage", 99.0}, {L"pid_100" + I + L"eng_3_engtype_Compute_0", -5.0}};
        auto ag = GpuAggregate(cs);
        Check(ag.size() == 2 && ag[0].luid.lo == 0xE3C5 && ag[0].util == 61.0 && ag[0].util3d == 42.5 && ag[0].engines == 4 && ag[1].luid.lo == 0xE8A9 && ag[1].util == 0.3 && ag[1].util3d == 0.0,
              "gpu: an engine is the sum over processes; an adapter is its busiest engine; adapters are kept apart by LUID");
        std::vector<GpuCounterSample> over = {{L"pid_1" + I + L"eng_0_engtype_3D", 80.0}, {L"pid_2" + I + L"eng_0_engtype_3D", 70.0}, {L"pid_3" + I + L"eng_0_engtype_3D", std::nan("")}};
        Check(GpuAggregate(over).size() == 1 && GpuAggregate(over)[0].util == 100.0 && GpuAggregate({}).empty(), "gpu: an engine never exceeds 100 %; a NaN sample counts as 0; no samples, no adapters");
        std::vector<GpuAdapterView> ad(3); ad[0].name = L"NVIDIA Quadro P1000"; ad[0].luid.lo = 0xE8A9; ad[1].name = L"Intel(R) UHD Graphics 630"; ad[1].luid.lo = 0xE3C5; ad[2].name = L"Microsoft Basic Render Driver"; ad[2].luid.lo = 0xE877; ad[2].software = true;
        GpuJoin(ad, ag, true);
        Check(ad[0].util == 0.3 && ad[1].util == 61.0 && ad[2].util == 0.0, "gpu: counters join adapters by LUID whatever the listing order; an adapter with no engine instance is idle");
        Check(GpuBusiest(ad) == 1 && GpuHeadline(ad) == 61.0, "gpu: the single figure follows the busiest adapter, not the first one listed (the idle discrete GPU of a hybrid laptop)");
        auto idle = ad; GpuJoin(idle, {}, true); Check(GpuHeadline(idle) == 0.0 && GpuBusiest(idle) == 0, "gpu: every adapter idle reads 0 %, a real reading");
        auto dead = ad; GpuJoin(dead, ag, false); Check(dead[0].util == -1 && dead[1].util == -1 && GpuHeadline(dead) == -1 && GpuBusiest(dead) == -1, "gpu: when the counters cannot be read every adapter is 'no reading'; no earlier value is kept");
        std::vector<GpuAdapterView> sw(1); sw[0].software = true; sw[0].util = 90; Check(GpuHeadline(sw) == -1 && GpuHeadline({}) == -1, "gpu: a software adapter never supplies the figure; no adapters, no reading");
        auto moved = ad; moved[1].luid.lo = 0xFFFF; GpuJoin(moved, ag, true); Check(moved[1].util == 0.0 && moved[0].util == 0.3, "gpu: a changed LUID (driver restart) never borrows another adapter's reading");
        // the engine's history point: Windows utilization, with or without the helper, and never the helper's GPU.LOAD
        Usage us; us.cpu = 20; us.ram = 40; us.gpu = 61; Readings rd; rd.present = true; rd.gpuLoad = 0; rd.gpuTemp = 44; rd.cpuTemp = 55;
        Check(EngineSample(us, rd, 10, 5).v[SGpuLoad] == 61 && EngineSample(us, rd, 10, 5).v[SGpuTemp] == 44, "gpu: the helper reporting 0 % for its first GPU does not mask the real utilization (the stuck-reading defect)");
        rd.present = false; Check(EngineSample(us, rd, 10, 5).v[SGpuLoad] == 61 && !HasValue(EngineSample(us, rd, 10, 5).v[SGpuTemp]) && !HasValue(EngineSample(us, rd, 10, 5).v[SCpuTemp]), "gpu: utilization is recorded without the sensor helper; helper values become gaps");
        us.gpu = 0; Check(EngineSample(us, rd, -1, -1).v[SGpuLoad] == 0 && !HasValue(EngineSample(us, rd, -1, -1).v[SNetDown]), "gpu: a real 0 % is recorded as 0, not as a gap");
        us.gpu = -1; rd.present = true; rd.gpuLoad = 35; Check(!HasValue(EngineSample(us, rd, 1, 1).v[SGpuLoad]), "gpu: no Windows reading is a gap; the helper's value is not substituted");
        SbCounts gc; gc.cores = 12; gc.drives = 2; gc.fans = 2; gc.gpus = 2; int each = 0; for (const auto& r2 : SbRowsFor(SecGpu, SbStandard, gc)) each += r2.id == RGpuEach;
        int eachMin = 0; for (const auto& r2 : SbRowsFor(SecGpu, SbMinimal, gc)) eachMin += r2.id == RGpuEach; gc.gpus = 1; int eachOne = 0; for (const auto& r2 : SbRowsFor(SecGpu, SbDetailed, gc)) eachOne += r2.id == RGpuEach;
        Check(each == 2 && eachMin == 0 && eachOne == 0, "sidebar: two graphics adapters each get a line from Standard detail; one adapter needs none");
    }
    {   // 2.9.8: Clear glass (transparent window, blur only in the cards)
        MaterialInput cl; cl.choice = MatClear; cl.build = 19045;
        Check(ResolveMaterial(cl) == MatClear && BackdropType(MatClear) == 1, "material: Clear glass is a choice on any Windows and uses no system backdrop");
        cl.transparencyOn = false; Check(ResolveMaterial(cl) == MatSolid, "material: Clear glass follows Windows when transparency is off");
        cl.transparencyOn = true; cl.highContrast = true; Check(ResolveMaterial(cl) == MatSolid, "material: Clear glass is solid in high contrast");
    }
    {   // 2.10: Clear glass controls, the exact first-run pointer scheme, stock Windows scheme names
        const CurScheme os = CurOwnerDefaultScheme(L"C:\Windows\Cursors");
        CurState st; st.name = L"Windows Aero";
        Check(ClearBgPct(-5) == 0 && ClearBgPct(300) == 100 && ClearBgPct(30) == 30 && ClearBlurPx(-1) == 0 && ClearBlurPx(999) == kClearBlurMax && kClearBgDefault == 30 && kClearFrostDefault == 60 && kClearBlurDefault > 0,
              "clear glass: the window opacity and the card blur are clamped, with defaults of 30 % and a real blur");
        Check(os.name == L"As3aD" && os.Mapped() == 17 && os.file[0].find(L"Glow") != std::wstring::npos && os.file[15].find(L"DIM v3.2") != std::wstring::npos && CurStateNeedsFirstRunDefault(st, 0),
              "cursors: the first-run scheme is the owner's As3aD (17 roles) and a stock Windows scheme still counts as no custom pointers");
    }
    {   // 2.12: the full clean, the Windows 10 theme variant, the sensor-free pure parts
        const std::vector<std::wstring> ln = FullCleanLines("D|BIB\r\nF|BIB - WIN.theme\n\n  \nD|After Dark Glass Cyan\n");
        Check(ln.size() == 4 && ln[0] == L"D|BIB" && ln[2] == L"  " && ln[3] == L"D|After Dark Glass Cyan" && FullCleanSafeName(L"BIB") && !FullCleanSafeName(L"..\\Windows") && !FullCleanSafeName(L"a/b") && !FullCleanSafeName(L"") && !FullCleanSafeName(L".."),
              "full clean: the records are read line by line and a name with a path part is never used");
        UninstallReport ur; ur.undoAsked = true; ur.fullClean = true; ur.cleanedThemes = 3; ur.cleanedCursors = 28; ur.cleanedWallpapers = 1; ur.cleanedMods = 23;
        const UninstallMessage fm = UninstallSummary(ur, L"C:\\ProgramData\\AnkhCore");
        ur.cleanFailed.push_back(L"C:\\Windows\\Cursors\\x");
        const UninstallMessage fb = UninstallSummary(ur, L"C:\\ProgramData\\AnkhCore");
        Check(fm.text.find(L"28 cursor pack(s)") != std::wstring::npos && fm.text.find(L"23 Windhawk mod(s)") != std::wstring::npos && fm.text.find(L"settings and data were deleted") != std::wstring::npos && !fb.ok && fb.text.find(L"not removed: C:\\Windows\\Cursors\\x") != std::wstring::npos,
              "full clean: the final message says what was removed and names what was not");
        Check(std::string(AuthoritativeThemeSha(true)) == kAuthoritativeAs3aDThemeSha256 && std::string(AuthoritativeThemeSha(false)) == kAuthoritativeAs3aD10ThemeSha256 && AuthoritativeThemeFile(false) == L"As3aD10.theme" && AuthoritativeThemeFile(true) == L"As3aD.theme" &&
              ThemeStylePathFor(false).find(L"After Dark Glass Cyan1.msstyles") != std::wstring::npos && IsAs3aDThemeActiveState(L"", L"C:\\Windows\\Resources\\Themes\\After Dark Glass Cyan\\After Dark Glass Cyan1.msstyles"),
              "theme: Windows 10 uses As3aD10.theme (After Dark Glass Cyan), Windows 11 As3aD.theme, and both are recognised as the active As3aD");
        if (FileExists(L"D:\\Ankhcore\\As3aD10.theme")) Check(Sha256File(L"D:\\Ankhcore\\As3aD10.theme") == kAuthoritativeAs3aD10ThemeSha256, "theme: the pinned hash is the one of As3aD10.theme");
    }
    {   // 2.9.8: a health finding offers a fix or the page that solves it
        Check(FindingActIsFix(FindingAct::CreateRestorePoint) && FindingActIsFix(FindingAct::StartWindhawk) && !FindingActIsFix(FindingAct::OpenSecurity) && !FindingActIsFix(FindingAct::None),
              "health findings: only the restore point and the Windhawk start are direct fixes; the others open the page");
        bool labels = std::wstring(FindingActLabel(FindingAct::None)).empty();
        for (FindingAct a : {FindingAct::CreateRestorePoint, FindingAct::StartWindhawk, FindingAct::OpenSystemCare, FindingAct::OpenSecurity, FindingAct::OpenUac, FindingAct::OpenStorage, FindingAct::OpenProcesses}) labels = labels && *FindingActLabel(a) != 0;
        Check(labels, "health findings: every action has a button label");
        Finding plain{1, L"x"}; Check(plain.act == FindingAct::None, "health findings: a finding without an action shows no button");
    }
    {   // 2.9.6: the Network section follows the section style (it had no effect before)
        SbCounts nc; nc.drives = 1;
        auto ids = [&](int level, int style) { std::vector<int> v; for (const auto& r2 : SbRowsFor(SecNet, level, nc, style)) v.push_back(r2.id); return v; };
        auto kindOf = [&](int level, int style, SbRowId id) { for (const auto& r2 : SbRowsFor(SecNet, level, nc, style)) if (r2.id == id) return static_cast<int>(r2.kind); return -1; };
        Check(ids(SbMinimal, SbStyleBar) == std::vector<int>{RNetSpeed} && ids(SbStandard, SbStyleBar) == std::vector<int>{RNetHead, RNetSpeed, RNetTotals} &&
              ids(SbDetailed, SbStyleBar) == std::vector<int>{RNetHead, RNetSpeed, RNetTotals, RNetLink, RNetGraph}, "sidebar network: the indicator style keeps the earlier rows exactly");
        Check(kindOf(SbStandard, SbStyleGauge, RNetDown) == SbGauge && kindOf(SbStandard, SbStyleGauge, RNetUp) == SbGauge && kindOf(SbStandard, SbStyleGauge, RNetSpeed) == -1, "sidebar network: gauges show download and upload as two gauges");
        Check(kindOf(SbStandard, SbStyleValue, RNetDown) == SbValue && kindOf(SbMinimal, SbStyleValue, RNetUp) == SbValue, "sidebar network: numeric values show download and upload as values, also at minimal detail");
        Check(kindOf(SbStandard, SbStyleGraph, RNetDown) == SbSpark && kindOf(SbStandard, SbStyleGraph, RNetUp) == SbSpark && kindOf(SbDetailed, SbStyleGraph, RNetGraph) == -1, "sidebar network: graphs draw one sparkline per direction, without a second combined graph");
        Check(SbBoxH(SecNet, SbStandard, nc, SbStyleGauge) != SbBoxH(SecNet, SbStandard, nc, SbStyleBar), "sidebar network: a different style changes the section's height");
    }
    {   // window material: the fallback ladder
        MaterialInput in;
        Check(ResolveMaterial(in) == MatAcrylic, "material: acrylic by default on mains");
        in.onBattery = true; Check(ResolveMaterial(in) == MatFrosted, "material: frosted on battery");
        in.frostedOnBattery = false; Check(ResolveMaterial(in) == MatAcrylic, "material: the battery rule is a setting");
        MaterialInput f; f.choice = MatFrosted; Check(ResolveMaterial(f) == MatFrosted, "material: frosted choice");
        f.choice = MatSolid; Check(ResolveMaterial(f) == MatSolid, "material: solid choice");
        MaterialInput t; t.transparencyOn = false; Check(ResolveMaterial(t) == MatSolid, "material: Transparency effects off wins");
        MaterialInput h; h.highContrast = true; Check(ResolveMaterial(h) == MatSolid, "material: High Contrast wins");
        MaterialInput b; b.batterySaver = true; Check(ResolveMaterial(b) == MatSolid, "material: Battery saver wins");
        MaterialInput o; o.build = 22000; Check(ResolveMaterial(o) == MatWallpaper, "material: older Windows gets wallpaper glass");
        MaterialInput ow; ow.build = 22000; ow.transparencyOn = false; Check(ResolveMaterial(ow) == MatSolid, "material: Transparency off beats wallpaper glass");
        MaterialInput wc; wc.choice = MatWallpaper; Check(ResolveMaterial(wc) == MatWallpaper && BackdropType(MatWallpaper) == 1, "material: wallpaper glass is a choice and uses no DWM backdrop");
        Animator an; an.Tick(1000);
        Check(an.To(1, 0.f, 200) == 0.f && !an.Active(), "anim: a new element starts at its target, nothing to animate");
        an.To(1, 1.f, 200); an.Tick(1100); const float mid = an.To(1, 1.f, 200);
        Check(mid > 0.f && mid < 1.f && an.Active(), "anim: moves toward the target during its duration");
        an.Tick(1200); Check(an.To(1, 1.f, 200) == 1.f && !an.Active(), "anim: settles and stops needing frames");
        an.Tick(5000); an.To(2, 0.f, 200); an.To(2, 1.f, 200); an.Tick(5300); Check(!an.Active(), "anim: a tween not drawn any more cannot keep frames running");
        Animator rd; rd.level = MotionReduced; Check(rd.Duration(400) == 100, "anim: reduced motion shortens to 100 ms");
        Animator off; off.level = MotionOff; off.Tick(1); off.To(3, 0.f, 200); Check(off.To(3, 1.f, 200) == 1.f && !off.Active(), "anim: motion off is instant");
        { FrameStats st; for (int i = 1; i <= 100; ++i) st.Add(static_cast<float>(i)); Check(st.Avg() == 50.5f && st.P95() >= 95.f && st.P95() <= 96.f, "frames: average and p95"); for (int i = 0; i < 500; ++i) st.Add(1.f); Check(st.Count() == 240 && st.Avg() == 1.f, "frames: the window is bounded"); }
        { AnimationGovernor g; uint64_t t = 0; for (int i = 0; i < 29; ++i) Check(!g.Frame(30.f, t += 16), "gov: needs a full window"), (void)0; Check(g.Frame(30.f, t += 16) && g.level == 1 && !g.ParticlesOn() && g.GlassEffectsOn(), "gov: slow frames turn particles off first");
          for (int i = 0; i < 30; ++i) g.Frame(30.f, t += 16); Check(g.level == 2 && !g.GlassEffectsOn(), "gov: then glass effects"); for (int i = 0; i < 30; ++i) g.Frame(30.f, t += 16); Check(g.level == 3 && g.ForcesReduced(), "gov: then reduced motion"); for (int i = 0; i < 30; ++i) g.Frame(30.f, t += 16); Check(g.level == 3, "gov: stops at the last level");
          for (int i = 0; i < 1000; ++i) g.Frame(2.f, t += 16); Check(g.level < 3, "gov: recovers after sustained good frames");
          AnimationGovernor ok; for (int i = 0; i < 300; ++i) ok.Frame(3.f, i * 16ull); Check(ok.level == 0, "gov: good frames never degrade"); }
        Check(ApplyEase(Ease::Linear, .25f) == .25f, "anim: linear ease");
        Check(ApplyEase(Ease::OutCubic, 0.f) == 0.f && ApplyEase(Ease::OutCubic, 1.f) == 1.f && ApplyEase(Ease::Spring, 1.f) == 1.f, "anim: easing end points");
        Check(BackdropType(MatAcrylic) == 3 && BackdropType(MatFrosted) == 4 && BackdropType(MatSolid) == 1, "material: DWM backdrop types");
    }
    // ---- Desktop mode (M-DSK1): migration and what the mode overrides
    { const SbLayerMove a = SbDesktopMigration(false, true, SbLayerBottom), b = SbDesktopMigration(true, true, SbLayerTop), c2 = SbDesktopMigration(false, false, SbLayerTop), d = SbDesktopMigration(false, true, 9);
      Check(a.write && a.layer == SbLayerDesktop && a.prevLayer == SbLayerBottom, "desktop: an existing setup moves to Desktop mode and its previous layer is kept");
      Check(!b.write && !c2.write, "desktop: the move happens once, and a new setup needs none");
      Check(d.write && d.prevLayer == SbLayerTop, "desktop: an unreadable previous layer falls back to the old default");
      const SbPlacement on = SbEffectivePlacement(SbLayerDesktop, SbPlacement{true, false, true, false}), off = SbEffectivePlacement(SbLayerBottom, SbPlacement{true, false, true, false});
      Check(!on.reserve && on.clickThrough && !on.autoHide && on.locked, "desktop: Desktop mode reserves no space, takes no mouse input, never auto-hides or drags");
      Check(off.reserve && !off.clickThrough && off.autoHide && !off.locked, "desktop: the other layers keep the saved overlay options"); }
    { const WallBox f = WallPlacement(WallFill, 4000, 3000, 1920, 1080), i = WallPlacement(WallFitIn, 4000, 3000, 1920, 1080), s = WallPlacement(WallStretch, 4000, 3000, 1920, 1080), c2 = WallPlacement(WallCenter, 800, 600, 1920, 1080);
      Check(f.l == 0 && f.r == 1920 && f.t == -180 && f.b == 1260, "wallpaper: Fill covers the screen and crops the overflow evenly");
      Check(i.t == 0 && i.b == 1080 && i.l == 240 && i.r == 1680, "wallpaper: Fit shows the whole picture, centred");
      Check(s.l == 0 && s.t == 0 && s.r == 1920 && s.b == 1080 && c2.l == 560 && c2.t == 240 && c2.r == 1360 && c2.b == 840, "wallpaper: Stretch fills exactly; Center keeps the picture's own size");
      Check(WallFitFromStyle(10, false) == WallFill && WallFitFromStyle(6, false) == WallFitIn && WallFitFromStyle(2, false) == WallStretch && WallFitFromStyle(0, false) == WallCenter && WallFitFromStyle(0, true) == WallFill && WallFitFromStyle(22, false) == WallFill,
            "wallpaper: the Windows fit settings map to Fill, Fit, Stretch and Center (Tile and Span use Fill)"); }
    // ---- privacy (M-PRIV1): Wi-Fi signal from the RSSI query, never from the location-gated connection query
    Check(kWlanOpcodeRssi == 0x10000102 && kWlanOpcodeRssi != 7, "privacy: the Wi-Fi query is wlan_intf_opcode_rssi, not the location-gated current-connection query");
    Check(WlanQualityFromRssi(-46) == 100 && WlanQualityFromRssi(-50) == 100 && WlanQualityFromRssi(-75) == 50 && WlanQualityFromRssi(-100) == 0 && WlanQualityFromRssi(-120) == 0, "wifi: dBm maps to the 0-100 % Windows shows");
    Check(WlanQualityFromRssi(0) == -1 && WlanQualityFromRssi(12) == -1, "wifi: a value that is not a received power is no reading");
    {   // ---- Backup & Migration (M-BAK1): JSON reader, ZIP subset, .acbackup manifest and validation
        JVal j;
        Check(JsonParse("\xEF\xBB\xBF{ \"a\": [1, 2.5, -3e2, true, null], \"b\": { \"c\": \"x\\ny \\u00e9 \\ud83d\\ude00\" } }", j) && j.Get("a")->a.size() == 5 && j.Get("a")->a[2].n == -300 &&
              j.Get("b")->Get("c")->s == "x\ny \xC3\xA9 \xF0\x9F\x98\x80", "json: nested document, numbers, escapes and a surrogate pair");
        Check(!JsonParse("{ \"a\": 1, \"a\": 2 }", j), "json: a duplicate key is refused");
        Check(!JsonParse("{ \"a\": 1 } x", j) && !JsonParse("{ \"a\": 1, }", j) && !JsonParse("[1 2]", j) && !JsonParse("", j), "json: trailing text, a trailing comma and a missing comma are refused");
        Check(!JsonParse("[01]", j) && !JsonParse("[1.]", j) && !JsonParse("[\"\\ud83d\"]", j) && !JsonParse("[\"a\tb\"]", j), "json: bad numbers, a lone surrogate and a raw control character are refused");
        std::string deep(40, '['); deep += std::string(40, ']');
        Check(!JsonParse(deep, j) && JsonParse("[[[[1]]]]", j), "json: nesting depth is bounded");
        uint64_t u = 0;
        Check(JsonParse("[4294967296, 1.5, -1]", j) && j.a[0].AsUInt(u) && u == 4294967296ull && !j.a[1].AsUInt(u) && !j.a[2].AsUInt(u), "json: sizes are whole non-negative numbers");
        const std::string odd = "q\" b\\ n\n c\x01 \xC3\xA9";
        JVal rt; Check(JsonParse("\"" + JsonEsc(odd) + "\"", rt) && rt.s == odd, "json: escaping round-trips");

        Check(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "zip: CRC-32 check value");
        const auto B = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
        std::vector<ZipItem> zi(2); zi[0].name = "a.bin"; zi[0].data = B("hello"); zi[1].name = "dir/b \xC3\xA9.txt";
        std::vector<uint8_t> z, got; std::vector<ZipEntry> ze; std::wstring er;
        Check(ZipWrite(zi, 0, 0x5B6B, z) && ZipList(z.data(), z.size(), ze, er) && ze.size() == 2 && ze[1].name == zi[1].name && ZipRead(z.data(), z.size(), ze[0], got, er) && got == zi[0].data &&
              ZipRead(z.data(), z.size(), ze[1], got, er) && got.empty(), "zip: a written archive lists and reads back (UTF-8 name, empty file)");
        { auto t = z; t.push_back(0); Check(!ZipList(t.data(), t.size(), ze, er), "zip: data after the end record is refused"); }
        { auto t = z; t.resize(t.size() - 5); Check(!ZipList(t.data(), t.size(), ze, er), "zip: a cut-off archive is refused"); }
        const size_t cd = z[z.size() - 6] | (z[z.size() - 5] << 8) | (z[z.size() - 4] << 16);     // central directory offset
        { auto t = z; t[cd + 8] |= 1; Check(!ZipList(t.data(), t.size(), ze, er) && er.find(L"encrypted") != std::wstring::npos, "zip: an encrypted entry is refused"); }
        { auto t = z; t[cd + 10] = 12; Check(!ZipList(t.data(), t.size(), ze, er), "zip: an unknown compression method is refused"); }
        { auto t = z; t[18] ^= 1; Check(!ZipList(t.data(), t.size(), ze, er) && er.find(L"two places") != std::wstring::npos, "zip: a local header that disagrees with the directory is refused"); }
        { auto t = z; t[35] ^= 0x20; ZipList(t.data(), t.size(), ze, er); Check(ze.size() == 2 && !ZipRead(t.data(), t.size(), ze[0], got, er), "zip: changed content fails its CRC"); }
        { std::vector<ZipItem> d(2); d[0].name = "A.txt"; d[1].name = "a.TXT"; std::vector<uint8_t> t; ZipWrite(d, 0, 0x5B6B, t); Check(!ZipList(t.data(), t.size(), ze, er), "zip: names differing only by letter case are refused"); }
        { std::vector<ZipItem> d(1); d[0].name = "folder/"; std::vector<uint8_t> t; ZipWrite(d, 0, 0x5B6B, t); Check(!ZipList(t.data(), t.size(), ze, er), "zip: a directory entry is refused"); }
        {   std::vector<ZipItem> d(1); d[0].name = "c.txt"; d[0].data = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'}; d[0].deflated = true; d[0].usize = 5; d[0].crc = Crc32(zi[0].data.data(), 5);
            std::vector<uint8_t> t;
            Check(ZipWrite(d, 0, 0x5B6B, t) && ZipList(t.data(), t.size(), ze, er) && ZipRead(t.data(), t.size(), ze[0], got, er) && got == zi[0].data, "zip: a deflate entry is read");
            d[0].usize = 4000000; ZipWrite(d, 0, 0x5B6B, t);
            Check(!ZipList(t.data(), t.size(), ze, er) && er.find(L"impossible") != std::wstring::npos, "zip: a size no compression could reach is refused before memory is taken"); }
        {   // an entry hidden inside another entry's data
            std::vector<ZipItem> inner(1); inner[0].name = "b.bin"; inner[0].data = B("payload"); std::vector<uint8_t> zin; ZipWrite(inner, 0, 0x5B6B, zin);
            std::vector<ZipItem> d(2); d[0].name = "a.bin"; d[0].data.assign(zin.begin(), zin.begin() + 30 + 5 + 7); d[1] = inner[0];
            std::vector<uint8_t> t; ZipWrite(d, 0, 0x5B6B, t);
            const size_t c2 = (t[t.size() - 6] | (t[t.size() - 5] << 8) | (t[t.size() - 4] << 16)) + 46 + 5;     // the second directory entry
            t[c2 + 42] = 35; t[c2 + 43] = 0; t[c2 + 44] = 0; t[c2 + 45] = 0;                                       // point it into the first entry's data
            Check(!ZipList(t.data(), t.size(), ze, er) && er.find(L"overlap") != std::wstring::npos, "zip: overlapping entries are refused"); }

        Check(AcbSafePath("a/b c/d.txt") && !AcbSafePath("") && !AcbSafePath("a//b") && !AcbSafePath("a/./b") && !AcbSafePath("a/../b") && !AcbSafePath("/a") && !AcbSafePath("a\\b") && !AcbSafePath("a:b"),
              "acbackup: relative paths only (no empty, dot, parent, absolute, backslash or drive parts)");
        Check(!AcbSafePath("x/com1.txt") && !AcbSafePath("LPT9") && !AcbSafePath("aux") && !AcbSafePath("nul.tar.gz") && !AcbSafePath("a/b.") && !AcbSafePath("a /b") && AcbSafePath("console.txt") && !AcbSafePath("a\xFF"),
              "acbackup: reserved device names, trailing dots or spaces and invalid UTF-8 are refused");
        Check(AcbExt("files/a.b/NAME.JSON") == ".json" && AcbExt("files/a.b/name").empty(), "acbackup: extension of the last path segment, lower-cased");

        AcbBuildInput bi; bi.kind = "full"; bi.created = "2026-11-11T12:30:40Z"; bi.appVersion = "2.5.0.1"; bi.appLabel = "2.5 Preview 1"; bi.osName = "Windows 11 Pro"; bi.osBuild = 26300; bi.arch = "x64";
        bi.items.push_back({"settings", "settings.json", "appdata:settings.json", B("{ \"sidebar.width\": 170 }")});
        bi.items.push_back({"monitoring", "net-counters.txt", "data:net-counters.txt", B("v1\n")});
        bi.items.push_back({"history", "journal.txt", "none:", B("entry\n")});
        bi.items.push_back({"apps", "bundle.ubundle", "none:", B("{ \"export_version\": 3, \"packages\": [] }")});
        std::vector<uint8_t> bk, bk2;
        Check(AcbBuild(bi, bk, er), "acbackup: a backup is built and passes its own verification");
        const AcbCheck ck = AcbVerify(bk.data(), bk.size());
        Check(ck.ok && ck.manifest.files.size() == 4 && ck.manifest.categories.size() == 4 && ck.manifest.kind == "full" && ck.manifest.appVersion == "2.5.0.1" && ck.manifest.osBuild == 26300 &&
              ck.manifest.created == bi.created && ck.manifest.bundle == "files/apps/bundle.ubundle" && ck.unsupported.empty(), "acbackup: the manifest round-trips (kind, versions, source system, categories, files, bundle)");
        Check(ck.ok && ck.entries.size() == 6 && ck.entries[0].name == "manifest.json" && ck.entries[1].name == "README.txt", "acbackup: the manifest comes first, then the read-me, then the files");
        Check(ck.ok && ck.manifest.files[0].sha256 == Sha256Hex(bi.items[0].data.data(), bi.items[0].data.size()) && ck.manifest.files[0].size == bi.items[0].data.size() &&
              AcbExtract(bk.data(), bk.size(), ck, ck.manifest.files[0], got, er) && got == bi.items[0].data, "acbackup: size and SHA-256 per file; a file is extracted and checked again");
        AcbBuild(bi, bk2, er); Check(!bk.empty() && bk == bk2, "acbackup: the same input gives the same bytes");
        Check(AcbSelect(ck.manifest, {}, false).size() == 4 && AcbSelect(ck.manifest, {}, true).size() == 2 && AcbSelect(ck.manifest, {"monitoring"}, false).size() == 1,
              "acbackup: Restore All, Settings Only and a category selection pick the right files");
        { AcbBuildInput x = bi; x.items.push_back({"iconpacks", "p.7z", "content:p.7z", B("x")}); Check(!AcbBuild(x, bk2, er) && bk2.empty(), "acbackup: a reserved category cannot be written yet");
          x = bi; x.items.push_back({"settings", "run.exe", "appdata:run.exe", B("MZ")}); Check(!AcbBuild(x, bk2, er), "acbackup: a program file cannot be written into a backup");
          x = bi; x.created = "yesterday"; Check(!AcbBuild(x, bk2, er), "acbackup: a backup needs a valid creation time"); }

        std::vector<ZipItem> base; for (const auto& it : bi.items) { ZipItem x; x.name = "files/" + it.category + "/" + it.rel; x.data = it.data; base.push_back(x); }
        const auto Pack = [&](const AcbManifest& m, std::vector<ZipItem> files) { ZipItem man; man.name = "manifest.json"; man.data = B(AcbManifestJson(m)); files.insert(files.begin(), man); std::vector<uint8_t> o; ZipWrite(files, 0, 0x5B6B, o); return o; };
        const auto Bad = [&](const std::function<void(AcbManifest&, std::vector<ZipItem>&)>& change, const wchar_t* word) {
            AcbManifest m = ck.manifest; auto files = base; change(m, files);
            const auto o = Pack(m, files); const AcbCheck c = AcbVerify(o.data(), o.size());
            return !c.ok && c.error.find(word) != std::wstring::npos; };
        { const auto o = Pack(ck.manifest, base); Check(AcbVerify(o.data(), o.size()).ok, "acbackup: a hand-packed copy of the same content verifies (the tamper tests start from a valid backup)"); }
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].sha256 = std::string(64, '0'); }, L"SHA-256"), "acbackup: a wrong SHA-256 in the manifest refuses the backup");
        Check(Bad([](AcbManifest&, std::vector<ZipItem>& f) { f[0].data[2] ^= 1; }, L"SHA-256"), "acbackup: changed file content refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[1].size += 1; }, L"size"), "acbackup: a size that differs from the manifest refuses the backup");
        Check(Bad([&](AcbManifest&, std::vector<ZipItem>& f) { ZipItem x; x.name = "files/settings/extra.json"; x.data = B("{}"); f.push_back(x); }, L"does not list"), "acbackup: a file the manifest does not list refuses the backup");
        Check(Bad([](AcbManifest&, std::vector<ZipItem>& f) { f.pop_back(); }, L"missing"), "acbackup: a listed file that is absent refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "files/settings/../settings.json"; }, L"unsafe path") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "files/settings/C:/x.json"; }, L"unsafe path") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "/files/settings/x.json"; }, L"unsafe path") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "files/settings/NUL.json"; }, L"unsafe path"), "acbackup: path traversal, drive, absolute and device-name paths refuse the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "files/monitoring/settings.json"; }, L"unsafe path"), "acbackup: a file outside its own category folder refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { AcbFile x = m.files[0]; x.path = "files/settings/SETTINGS.json"; x.dest = "appdata:other.json"; m.files.push_back(x); }, L"twice"), "acbackup: paths differing only by letter case refuse the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { AcbFile x = m.files[0]; x.path = "files/settings/b.json"; x.dest = "appdata:Settings.JSON"; m.files.push_back(x); }, L"share the destination"), "acbackup: two files for one destination refuse the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].path = "files/settings/run.exe"; }, L"file type") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "appdata:run.exe"; }, L"file type"), "acbackup: an unexpected file type, in the archive or as a destination, refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "data:settings.json"; }, L"may not write") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[2].dest = "data:journal.txt"; }, L"may not write"), "acbackup: a category cannot write outside its own root; history and bundles are never written back");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "appdata:../x.json"; }, L"unsafe destination") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "appdata:C:/x.json"; }, L"unsafe destination") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "C:\\Windows\\x.json"; }, L"unsafe destination") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].dest = "nowhere:x.json"; }, L"unsafe destination") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[2].dest = "none:x.txt"; }, L"unsafe destination"), "acbackup: absolute, parent and unknown-root destinations refuse the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.categories[0].restore = "file-copy"; }, L"restore action"), "acbackup: a known category cannot be given another restore action");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.files[0].category = "ghost"; m.files[0].path = "files/ghost/settings.json"; }, L"does not declare"), "acbackup: a file in an undeclared category refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.formatVersion = 2; }, L"newer AnkhCore"), "acbackup: a newer format version is refused with an update message");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.kind = "everything"; }, L"backup type") && Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.created = "2026-13-01T00:00:00Z"; }, L"creation time") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.appVersion = "latest"; }, L"version"), "acbackup: an unknown type, a bad time or a bad version refuses the backup");
        Check(Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.bundle = "files/apps/other.ubundle"; }, L"package bundle") &&
              Bad([](AcbManifest& m, std::vector<ZipItem>&) { m.bundle = "files/settings/settings.json"; }, L"package bundle"), "acbackup: the package bundle must be a listed file of the applications category");
        {   // a category from a later version: hash-checked, reported, never selected for restore
            AcbManifest m = ck.manifest; auto files = base;
            const auto blob = B("future"); ZipItem zf; zf.name = "files/future-thing/blob.bin"; zf.data = blob; files.push_back(zf);
            m.categories.push_back({"future-thing", "Something newer", "magic"});
            AcbFile nf; nf.path = zf.name; nf.category = "future-thing"; nf.size = blob.size(); nf.sha256 = Sha256Hex(blob.data(), blob.size()); nf.dest = "none:"; m.files.push_back(nf);
            auto o = Pack(m, files); AcbCheck c = AcbVerify(o.data(), o.size());
            Check(c.ok && c.unsupported.size() == 1 && c.unsupported[0] == "future-thing" && AcbSelect(c.manifest, {}, false).size() == 4, "acbackup: an unknown category is verified, reported as not restorable and never restored");
            files.back().data[0] ^= 1; o = Pack(m, files); c = AcbVerify(o.data(), o.size());
            Check(!c.ok, "acbackup: an unknown category's files are still hash-checked"); }
        {   std::vector<ZipItem> only(1); only[0].name = "README.txt"; only[0].data = B("x"); std::vector<uint8_t> o; ZipWrite(only, 0, 0x5B6B, o);
            const AcbCheck c = AcbVerify(o.data(), o.size()); Check(!c.ok && c.error.find(L"no manifest") != std::wstring::npos, "acbackup: an archive without a manifest is not a backup");
            AcbManifest pm; std::wstring pe;
            Check(!AcbParseManifest("{ \"format\": \"zip\" }", pm, pe) && pe.find(L"not an AnkhCore backup") != std::wstring::npos && !AcbParseManifest("not json", pm, pe) && !AcbParseManifest("{ \"format\": \"acbackup\", \"formatVersion\": 1 }", pm, pe),
                  "acbackup: a foreign, unreadable or incomplete manifest is refused");
            Check(!AcbParseManifest("{ \"format\": \"acbackup\", \"formatVersion\": 7, \"anything\": [] }", pm, pe) && pe.find(L"newer AnkhCore") != std::wstring::npos, "acbackup: a newer manifest is recognised before its other fields are read"); }
    }
    {   // ---- Icon Engine Resource Redirect (M-ICON1 / APP-8 / D41)
        {   // the 2026-11-15 packs: all registered once, none duplicated (ids and display names), HUD Machine Blue stays the single existing "hud" entry
            bool added = true, unique = true; std::set<std::wstring> ids, names;
            for (const wchar_t* id : {L"3d-purple", L"amy-dark", L"coloration", L"hal-green", L"macos-elegance", L"tela-black", L"windows-23"}) added = added && FindPack(id) != nullptr;
            for (const auto& pk : IconPacks()) { unique = unique && ids.insert(pk.id).second && names.insert(std::wstring(pk.name) + L"|" + pk.variant).second; }
            Check(added && unique && FindPack(L"hud-machine-blue") == FindPack(L"hud"), "icons: the seven new 7TSP packs are registered once; HUD Machine Blue is not duplicated; no id or name repeats");
        }
        Check(IconPacks().size() == 18 && FindPack(L"bigsur-dark") != nullptr && FindPack(L"candy") != nullptr &&
              FindPack(L"pear-blue-dark") != nullptr && FindPack(L"alienware-invader") != nullptr &&
              FindPack(L"alienware-eclipse") != nullptr && FindPack(L"hud-machine-blue") != nullptr &&
              !FindPack(L"bigsur-dark")->partial && FindPack(L"magnum")->partial && FindPack(L"hud")->partial && FindPack(L"alienware-eclipse")->partial,
              "icons: all 18 prebuilt themes are registered with supported vs partial metadata");

        // Pure themePaths settings extraction and planning
        JVal sObj;
        Check(JsonParse("{\"iconTheme\":\"\",\"disableThumbnails\":\"0\",\"allResourceRedirect\":\"0\","
                        "\"themePaths[0]\":\"D:\\\\Custom\\\\UserTheme\","
                        "\"themePaths[1]\":\"C:\\\\Program Files\\\\AnkhCore\\\\IconThemes\\\\bigsur\","
                        "\"themePaths[2]\":\"\","
                        "\"themePaths[3]\":\"D:\\\\IgnoredAfterEmpty\"}", sObj),
              "icons: flat Resource Redirect settings JSON parses");
        const auto ext = ExtractThemePathsFromFlatSettings(sObj);
        Check(ext.size() == 2 && ext[0] == L"D:\\Custom\\UserTheme" &&
              ext[1] == L"C:\\Program Files\\AnkhCore\\IconThemes\\bigsur",
              "icons: ExtractThemePathsFromFlatSettings stops at the first empty slot");

        const std::wstring mRoot = L"C:\\Program Files\\AnkhCore\\IconThemes";
        // Switching from bigsur to sweet preserves D:\Custom\UserTheme and replaces bigsur with sweet
        const auto sw = PlanThemePathsUpdate(ext, mRoot, L"C:\\Program Files\\AnkhCore\\IconThemes\\bigsur",
                                             L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet");
        Check(sw.size() == 2 && sw[0] == L"D:\\Custom\\UserTheme" &&
              sw[1] == L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet",
              "icons: PlanThemePathsUpdate preserves unrelated user paths and switches the AnkhCore theme");

        // Re-applying the same theme does not create duplicates
        const auto dup = PlanThemePathsUpdate(sw, mRoot, L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet",
                                              L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet");
        Check(dup.size() == 2 && dup[0] == L"D:\\Custom\\UserTheme" &&
              dup[1] == L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet",
              "icons: PlanThemePathsUpdate prevents duplicate entries");

        // Restoring default icons removes only AnkhCore's managed theme path
        const auto rest = PlanThemePathsUpdate(sw, mRoot, L"C:\\Program Files\\AnkhCore\\IconThemes\\sweet", L"");
        Check(rest.size() == 1 && rest[0] == L"D:\\Custom\\UserTheme",
              "icons: PlanThemePathsUpdate with empty newThemeDir removes only the AnkhCore theme path");

        // Formatting CLI arguments clears trailing slots so Resource Redirect's sequential reader stops
        const auto args = FormatThemePathsSetArgs(3, rest);
        Check(args.size() == 3 && args[0] == L"themePaths[0]=D:\\Custom\\UserTheme" &&
              args[1] == L"themePaths[1]=" && args[2] == L"themePaths[2]=",
              "icons: FormatThemePathsSetArgs clears trailing themePaths slots");

        // pe::Builder deduplicates same-(type,name,lang) entries (keeping last)
        {
            pe::Builder pb(0x1000);
            pe::Res r1; r1.type = 3; r1.nameIsId = true; r1.nameId = 100; r1.lang = 1033; r1.data = {1, 2, 3, 4};
            pe::Res r2 = r1; r2.data = {9, 8, 7, 6, 5};
            std::vector<pe::Res> rlist{r1, r2};
            std::vector<pe::Builder::Type> types;
            pe::Builder::SortBuild(rlist, types);
            const auto sec = pb.Build(rlist);
            Check(!sec.empty() && types.size() == 1 && types[0].names.size() == 1 &&
                  types[0].names[0].langs.size() == 1 && *types[0].names[0].langs[0].data == r2.data,
                  "icons: pe::Builder deduplicates duplicate (type, name, lang) entries keeping the last");
        }

        // ValidateIconThemeDir rejects missing theme.ini, raw .res files masquerading as .dll, and accepts valid PE resource modules
        const std::wstring tDir = std::wstring(tmp) + L"ankhcore_test_icontheme_" + std::to_wstring(GetCurrentProcessId());
        Evict(tDir, false, false);
        EnsureDir(tDir);
        std::wstring vErr;
        Check(!ValidateIconThemeDir(tDir, nullptr, &vErr) && vErr.find(L"Missing theme.ini") != std::wstring::npos,
              "icons: ValidateIconThemeDir rejects folder without theme.ini");

        const std::string iniTxt = "[redirections]\r\n%SystemRoot%\\SystemResources\\imageres.dll.mun=imageres.dll.mun\r\n";
        WriteAll(tDir + L"\\theme.ini", reinterpret_cast<const uint8_t*>(iniTxt.data()), iniTxt.size());
        Check(!ValidateIconThemeDir(tDir, nullptr, &vErr) && vErr.find(L"Missing redirected module") != std::wstring::npos,
              "icons: ValidateIconThemeDir rejects missing target module");

        // Write a raw .res header (not a PE) to imageres.dll.mun -> must be rejected
        const uint8_t fakeRes[32] = {0,0,0,0, 0x20,0,0,0, 0xFF,0xFF,0,0, 0xFF,0xFF,0,0};
        WriteAll(tDir + L"\\imageres.dll.mun", fakeRes, sizeof(fakeRes));
        Check(!ValidateIconThemeDir(tDir, nullptr, &vErr) && vErr.find(L"not a valid PE") != std::wstring::npos,
              "icons: ValidateIconThemeDir rejects raw .res / non-PE file");

        // Copy a real resource-Bearing PE (or prebuilt module if present) and verify it passes
        const std::wstring prebuiltBigSur = L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\bigsur";
        if (DirExists(prebuiltBigSur)) {
            std::vector<std::pair<std::wstring, std::wstring>> pairs;
            Check(ValidateIconThemeDir(prebuiltBigSur, &pairs, &vErr) && pairs.size() == 8,
                  "icons: prebuilt bigsur icon theme validates all PE modules and theme.ini (8 redirections)");
            const std::wstring hudDir = L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\hud-machine-blue";
            std::vector<std::pair<std::wstring, std::wstring>> hudPairs;
            Check(ValidateIconThemeDir(hudDir, &hudPairs, &vErr) && hudPairs.size() == 110,
                  "icons: prebuilt hud-machine-blue theme validates all 110 redirection entries (including dual main.cpl targets)");
            int allValid = 0;
            for (const auto& pk : IconPacks()) {
                const std::wstring pDir = L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\" + IconThemeFolderName(pk.id);
                if (ValidateIconThemeDir(pDir)) ++allValid;
            }
            Check(allValid == 18, "icons: all 18 prebuilt icon themes pass ValidateIconThemeDir");
            Check(FileExists(L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\candy\\preview.png") && IconThemeQuickCheck(L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes\\candy").preview.find(L"preview.png") != std::wstring::npos,
                  "icons: the Candy theme carries its preview picture and the status check finds it");
        }
        Evict(tDir, false, false);
    }
    {   // ---- Try Fan Compatibility (HW-4 / D42) & Protected Fan Control
        Check(kFanControlEnabled, "fan-control: kFanControlEnabled is true (owner decision 2026-11-19): commands go through the guard");
        Check(ClampFanDuty40(-10) == 40 && ClampFanDuty40(0) == 40 && ClampFanDuty40(39) == 40 &&
              ClampFanDuty40(40) == 40 && ClampFanDuty40(75) == 75 && ClampFanDuty40(140) == 100,
              "fan-compat: ClampFanDuty40 enforces the mandatory 40% minimum duty (D23)");
        Check(SensorHub::kMinFanPercent == 40 && kMinFanDutyPct == 40,
              "fcd-04: SensorHub::kMinFanPercent and kMinFanDutyPct are unified at 40% (D23)");

        {   // FCD-01 / FCD-02: new per-fan capability telemetry + backward compatibility with older helpers
            const std::string modern =
                "TIME=12:00:00\n"
                "FAN.0.NAME=CPU Fan\nFAN.0.RPM=2450\nFAN.0.CONTROL=1\nFAN.0.CAPABILITY=controllable\n"
                "FAN.0.SOURCE=LibreHardwareMonitor\nFAN.0.LIVE=1\nFAN.0.PERCENT=55\n"
                "FAN.0.MINPCT=0\nFAN.0.MAXPCT=100\nFAN.0.CTRLMODE=software\n"
                "FAN.1.NAME=Chassis Fan\nFAN.1.RPM=1200\nFAN.1.CONTROL=0\nFAN.1.CAPABILITY=monitor_only\n"
                "FAN.1.SOURCE=LibreHardwareMonitor\nFAN.1.LIVE=1\n"
                "FAN.2.NAME=GPU Fan\nFAN.2.RPM=0\nFAN.2.CONTROL=0\nFAN.2.CAPABILITY=vendor_readonly\n"
                "FAN.2.SOURCE=HP BIOS\nFAN.2.LIVE=0\n"
                "FANCOUNT=3\nCONTROLS=1\n";
            const Readings rm = ParseReadings(modern);
            Check(rm.fans.size() == 3 && rm.ControllableFanCount() == 1 && rm.MonitorOnlyFanCount() == 2,
                  "fcd-01/02: ParseReadings parses multi-fan capability counts");
            Check(rm.fans[0].capability == FanCapability::Controllable && rm.fans[0].controllable &&
                  rm.fans[0].minPercent == 0.0 && rm.fans[0].maxPercent == 100.0 && rm.fans[0].controlMode == L"software" &&
                  FanCapabilityLabel(rm.fans[0]) == L"Controllable",
                  "fcd-01/02: controllable fan capability, range and controlMode parsed");
            Check(rm.fans[1].capability == FanCapability::MonitorOnly && !rm.fans[1].controllable &&
                  rm.fans[1].live && FanCapabilityLabel(rm.fans[1]) == L"Monitoring only",
                  "fcd-01/02: monitor_only fan capability parsed");
            Check(rm.fans[2].capability == FanCapability::VendorReadOnly && !rm.fans[2].live &&
                  FanCapabilityLabel(rm.fans[2]) == L"Vendor read-only",
                  "fcd-01/02: vendor_readonly fan capability and LIVE=0 parsed");

            const std::string legacy =
                "TIME=12:00:01\n"
                "FAN.0.NAME=Legacy LHM\nFAN.0.RPM=1800\nFAN.0.CONTROL=1\nFAN.0.PERCENT=60\n"
                "FAN.1.NAME=Legacy LHM RO\nFAN.1.RPM=1500\nFAN.1.CONTROL=0\n"
                "FAN.2.NAME=Legacy HP\nFAN.2.RPM=3200\nFAN.2.CONTROL=0\nFAN.2.SOURCE=HP BIOS\nFAN.2.LIVE=1\n"
                "FANCOUNT=3\nCONTROLS=1\n";
            const Readings rl = ParseReadings(legacy);
            Check(rl.fans.size() == 3 &&
                  rl.fans[0].capability == FanCapability::Controllable && rl.fans[0].live &&
                  rl.fans[1].capability == FanCapability::MonitorOnly && rl.fans[1].live &&
                  rl.fans[2].capability == FanCapability::VendorReadOnly && rl.fans[2].live,
                  "fcd-02: ParseReadings infers capabilities and live=true for legacy helper telemetry");
        }

        FanCompatCandidate c1, c2;
        const std::string validXml =
            "<?xml version=\"1.0\"?><FanControlConfigV2>"
            "<NotebookModel>HP ZBook Fury 15 G7</NotebookModel>"
            "<Author>jwhendy</Author><CriticalTemperature>85</CriticalTemperature>"
            "<FanConfigurations><FanConfiguration/><FanConfiguration/></FanConfigurations>"
            "</FanControlConfigV2>";
        Check(ParseNbfcConfigXml(validXml, L"HP ZBook Fury 15 G7", L"C:\\Configs\\HP ZBook Fury 15 G7.xml", L"HP ZBook 15 G5", c1) &&
              c1.notebookModel == L"HP ZBook Fury 15 G7" && c1.author == L"jwhendy" && c1.criticalTempC == 85 &&
              c1.fanCount == 2 && !c1.exactModelMatch && c1.familyMatch && c1.sha256.size() == 64,
              "fan-compat: ParseNbfcConfigXml parses model, author, critical temp, fan count, SHA-256 and family match");
        Check(ParseNbfcConfigXml(validXml, L"HP ZBook Fury 15 G7", L"C:\\Configs\\HP ZBook Fury 15 G7.xml", L"HP ZBook Fury 15 G7", c2) &&
              c2.exactModelMatch && !c2.familyMatch,
              "fan-compat: ParseNbfcConfigXml identifies an exact model match");
        Check(!ParseNbfcConfigXml("<NotNbfc/>", L"bad", L"bad.xml", L"HP ZBook 15 G5", c2),
              "fan-compat: ParseNbfcConfigXml rejects non-NBFC XML");

        const std::wstring localCfgDir = L"D:\\Ankhcore\\Fan control\\Configs";
        if (DirExists(localCfgDir)) {
            const auto list = DiscoverFanCompatCandidates(L"HP ZBook 15 G5", {localCfgDir});
            size_t exact = 0, fam = 0;
            bool foundFury = false;
            for (const auto& item : list) {
                if (item.exactModelMatch) ++exact;
                if (item.familyMatch) ++fam;
                if (item.name == L"HP ZBook Fury 15 G7" && item.fanCount == 2 && item.criticalTempC == 85) foundFury = true;
            }
            Check(list.size() == 243 && exact == 0 && fam >= 4 && list[0].familyMatch && foundFury,
                  "fan-compat: DiscoverFanCompatCandidates discovers all 243 local configs read-only and ranks HP ZBook family first");
        }

        // Pure state machine tests: concurrency, 40% clamp, thermal & freshness gates, user Stop, and SensorHub RPM attribution
        {
            FanCompatTrial tr;
            const FanCompatTelemetry goodBase{true, 52.0, 48.0, {2200.0, 2150.0}};
            Check(tr.StartCandidate(c1, 20, goodBase) && tr.stage == FanCompatStage::Baseline && tr.record.requestedDutyPct == 40,
                  "fan-compat: StartCandidate enters Baseline and clamps requested duty (20% -> 40%)");
            Check(!tr.StartCandidate(c1, 60, goodBase),
                  "fan-compat: StartCandidate refuses concurrent trial while a candidate is active");
            Check(tr.OnCommandResult(true) && tr.stage == FanCompatStage::Testing,
                  "fan-compat: OnCommandResult(true) enters Testing");
            // 2 ticks with no significant RPM change, then a real RPM rise (+650 RPM)
            Check(tr.TickHold({true, 53.0, 49.0, {2250.0, 2200.0}}) == FanCompatStage::Testing,
                  "fan-compat: small RPM jitter (+50 RPM) does not prematurely claim compatibility");
            Check(tr.TickHold({true, 53.0, 49.0, {2850.0, 2700.0}}) == FanCompatStage::Restoring &&
                  tr.record.verdict == FanCompatVerdict::Compatible,
                  "fan-compat: attributable SensorHub RPM rise (2200 -> 2850 RPM) marks candidate Compatible and enters Restoring");
            tr.FinishRestore(true, goodBase);
            Check(tr.stage == FanCompatStage::Completed && tr.record.restoredSafeState && tr.record.verdict == FanCompatVerdict::Compatible,
                  "fan-compat: FinishRestore(true) completes Compatible record");
        }
        {   // 0 RPM cool idle: stays 0 RPM -> NoRpmResponse (never compatible from command exit code alone); rises from 0 -> 2400 -> Compatible
            FanCompatTrial tr;
            tr.maxHoldSeconds = 3;
            const FanCompatTelemetry coolZero{true, 43.0, 40.0, {0.0, 0.0}};
            Check(tr.StartCandidate(c1, 60, coolZero) && tr.record.zeroRpmCoolIdleObserved,
                  "fan-compat: 0 RPM at 43 C is recorded as cool-idle stop, not sensor error");
            tr.OnCommandResult(true);
            tr.TickHold(coolZero);
            tr.TickHold(coolZero);
            Check(tr.TickHold(coolZero) == FanCompatStage::Restoring && tr.record.verdict == FanCompatVerdict::NoRpmResponse,
                  "fan-compat: command success without SensorHub RPM response ends as NoRpmResponse after timeout");
            tr.FinishRestore(true, coolZero);

            // Now 0 RPM cool idle that spins up to 2400 RPM
            Check(tr.StartCandidate(c1, 60, coolZero) && tr.OnCommandResult(true), "fan-compat: restart from cool-idle 0 RPM");
            Check(tr.TickHold({true, 44.0, 41.0, {2400.0, 0.0}}) == FanCompatStage::Restoring &&
                  tr.record.verdict == FanCompatVerdict::Compatible,
                  "fan-compat: rising from 0 RPM to 2400 RPM in SensorHub proves compatibility");
            tr.FinishRestore(true, coolZero);
        }
        {   // Safety gates: thermal limit, stale telemetry, backend failure, user Stop, and restore failure
            FanCompatTrial tr;
            const FanCompatTelemetry base{true, 60.0, 55.0, {2500.0}};
            Check(!tr.StartCandidate(c1, 60, {false, 60.0, 55.0, {2500.0}}) && tr.stage == FanCompatStage::Aborted &&
                  tr.record.verdict == FanCompatVerdict::AbortedSafety,
                  "fan-compat: stale SensorHub telemetry at start is refused immediately");

            Check(tr.StartCandidate(c1, 60, base) && tr.OnCommandResult(true), "fan-compat: start for thermal gate check");
            Check(tr.TickHold({true, 86.0, 58.0, {2600.0}}) == FanCompatStage::Restoring &&
                  tr.record.verdict == FanCompatVerdict::AbortedSafety,
                  "fan-compat: reaching maxSafeTempC (86 C >= 85 C) aborts immediately to Restoring");
            tr.FinishRestore(true, base);

            Check(tr.StartCandidate(c1, 60, base) && tr.OnCommandResult(true), "fan-compat: start for user Stop check");
            tr.Cancel();
            Check(tr.stage == FanCompatStage::Restoring && tr.record.verdict == FanCompatVerdict::AbortedUser,
                  "fan-compat: user Cancel() immediately transitions to Restoring with AbortedUser");
            tr.FinishRestore(true, base);

            Check(tr.StartCandidate(c1, 60, base) && tr.OnCommandResult(true), "fan-compat: start for failed restore check");
            tr.TickHold({true, 61.0, 55.0, {3200.0}});
            tr.FinishRestore(false, base);
            Check(tr.stage == FanCompatStage::Aborted && tr.record.verdict == FanCompatVerdict::AbortedSafety,
                  "fan-compat: failed safe-state restore downgrades Compatible verdict to AbortedSafety");
        }
        {   // Capability assessment, config validation, limits, per-fan commands, and failure recovery (zero hardware writes)
            const auto capManual = AssessFanControlCapability(2, 2, false, L"", 0);
            Check(capManual.kind == FanBackendKind::DirectManual && capManual.statusLabel == L"Manual Control" && capManual.perFanSupported,
                  "fan-ctrl: AssessFanControlCapability detects DirectManual with per-fan support");
            const auto capCfg = AssessFanControlCapability(2, 0, true, L"HP ZBook Fury 15 G7", 2);
            Check(capCfg.kind == FanBackendKind::ConfigurationBased && capCfg.statusLabel == L"Configuration Based" && capCfg.perFanSupported,
                  "fan-ctrl: AssessFanControlCapability detects ConfigurationBased with per-fan support");
            const auto capMon = AssessFanControlCapability(2, 0, false, L"", 0);
            Check(capMon.kind == FanBackendKind::MonitoringOnly && capMon.statusLabel == L"Monitoring Only" && !capMon.perFanSupported,
                  "fan-ctrl: AssessFanControlCapability detects MonitoringOnly on unsupported hardware");

            std::wstring vErr;
            Check(ValidateNbfcConfigName(c1.name, {c1}, vErr), "fan-ctrl: ValidateNbfcConfigName accepts valid config");
            Check(!ValidateNbfcConfigName(L"../bad", {c1}, vErr) && !ValidateNbfcConfigName(L"Unknown Model", {c1}, vErr),
                  "fan-ctrl: ValidateNbfcConfigName rejects traversal and unknown configs");

            std::vector<std::wstring> cmds;
            auto okRunner = [&](const std::wstring& args, std::wstring&) { cmds.push_back(args); return true; };
            Check(FanSetSpeedSafe(15, -1, okRunner).ok && cmds.back() == L"set --speed 40",
                  "fan-ctrl: FanSetSpeedSafe clamps <40% to 40%");
            Check(FanSetSpeedSafe(150, -1, okRunner).ok && cmds.back() == L"set --speed 100",
                  "fan-ctrl: FanSetSpeedSafe clamps >100% to 100%");
            Check(FanSetSpeedSafe(75, 1, okRunner).ok && cmds.back() == L"set --speed 75 --fan 1",
                  "fan-ctrl: FanSetSpeedSafe formats per-fan --fan argument");

            // Failure recovery: failed apply or speed command triggers set --auto fallback
            std::vector<std::wstring> failCmds;
            auto failSpeedRunner = [&](const std::wstring& args, std::wstring&) {
                failCmds.push_back(args);
                return args == L"set --auto";
            };
            const FanOpResult rFailSpd = FanSetSpeedSafe(70, -1, failSpeedRunner);
            Check(!rFailSpd.ok && failCmds.size() == 2 && failCmds[1] == L"set --auto",
                  "fan-ctrl: failed speed command triggers automatic fallback recovery");
            failCmds.clear();
            Check(!FanApplySafe(L"Unknown", {c1}, failSpeedRunner).ok && failCmds.empty(),
                  "fan-ctrl: invalid config rejected with zero commands");
            const FanOpResult rFailApp = FanApplySafe(c1.name, {c1}, failSpeedRunner);
            Check(!rFailApp.ok && failCmds.size() == 2 && failCmds[1] == L"set --auto",
                  "fan-ctrl: failed config apply triggers automatic fallback recovery");

            const NbfcStatusInfo st = ParseNbfcStatus(
                L"Read-only               : False\n"
                L"Selected Config Name    : HP ZBook Fury 15 G7\n"
                L"Fan Display Name        : CPU Fan\n"
                L"Current Fan Speed       : 48.5\n"
                L"Target Fan Speed        : 60.0\n");
            Check(!st.readOnly && st.selectedConfig == L"HP ZBook Fury 15 G7" &&
                  st.fanNames.size() == 1 && st.targetSpeeds.size() == 1 && st.targetSpeeds[0] == 60.0,
                  "fan-ctrl: ParseNbfcStatus extracts config name, read-only flag, and fan target");
        }
    }
    {   // ---- Optional Installer Components (INS-3 / D41 / D44)
        const std::wstring regFile = L"D:\\Ankhcore\\AnkhCore_Taskbar_Profile.reg";
        if (FileExists(regFile)) {
            std::vector<TaskbarRegEntry> entries;
            std::wstring err;
            Check(ParseTaskbarProfileReg(ReadText(regFile), entries, err) && entries.size() == 14,
                  "ins-3: real AnkhCore_Taskbar_Profile.reg parses and validates all 14 HKCU Taskbar entries");
        }
        std::vector<TaskbarRegEntry> badEntries;
        std::wstring badErr;
        Check(!ParseTaskbarProfileReg("Windows Registry Editor Version 5.00\r\n[HKEY_LOCAL_MACHINE\\Software\\Policies]\r\n\"TaskbarAl\"=dword:00000001\r\n", badEntries, badErr) &&
              badErr.find(L"Disallowed registry key") != std::wstring::npos,
              "ins-3: ParseTaskbarProfileReg refuses HKLM or policy registry keys");
        Check(!ParseTaskbarProfileReg("Windows Registry Editor Version 5.00\r\n[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced]\r\n\"UnrelatedSetting\"=dword:00000001\r\n", badEntries, badErr) &&
              badErr.find(L"Unrecognized or disallowed value") != std::wstring::npos,
              "ins-3: ParseTaskbarProfileReg refuses unrelated Explorer registry values");
        Check(FormatTaskbarProfileSummary(false, true, false, L"").find(L"Not selected | OS: Windows 11 (supported) | Result: Skipped (not selected)") != std::wstring::npos &&
              FormatTaskbarProfileSummary(true, false, false, L"").find(L"Selected | OS: Windows 10 or older (not supported) | Result: Skipped (Windows 11 only)") != std::wstring::npos &&
              FormatTaskbarProfileSummary(true, true, true, L"14 settings").find(L"Selected | OS: Windows 11 (supported) | Result: Applied (14 settings)") != std::wstring::npos,
              "ins-3: FormatTaskbarProfileSummary reports selection, OS support, and outcome clearly");
    }
    {   // ---- Backup & Migration Engine (M-BAK2: BAK-1, BAK-2, BAK-3, D30)
        const std::wstring bakSandbox = std::wstring(tmp) + L"ankhcore_test_bak2_" + std::to_wstring(GetCurrentProcessId());
        Evict(bakSandbox, true, false);
        const AcbRoots roots{bakSandbox + L"\\AppData", bakSandbox + L"\\Data", bakSandbox + L"\\Content"};
        EnsureDir(roots.appdata);
        EnsureDir(roots.data);
        EnsureDir(roots.content);

        // 1. Pure root resolution
        std::wstring resPath;
        Check(AcbResolveDest(roots, "appdata:settings.json", resPath) && resPath == roots.appdata + L"\\settings.json" &&
              AcbResolveDest(roots, "data:net-counters.txt", resPath) && resPath == roots.data + L"\\net-counters.txt" &&
              AcbResolveDest(roots, "content:packs/cursor.cur", resPath) && resPath == roots.content + L"\\packs\\cursor.cur" &&
              AcbResolveDest(roots, "none:", resPath) && resPath.empty() &&
              !AcbResolveDest(roots, "appdata:../escape.json", resPath) &&
              !AcbResolveDest(roots, "unknown:file.txt", resPath),
              "bak2: AcbResolveDest maps appdata, data, content, and none safely and rejects traversal/unknown roots");

        // 2. Streamed file SHA-256 hashing
        const std::wstring streamFile = bakSandbox + L"\\stream_test.bin";
        std::vector<uint8_t> bigBuf((1u << 20) + 12345, 0x5Au);
        for (size_t i = 0; i < bigBuf.size(); i += 4096) bigBuf[i] = static_cast<uint8_t>(i & 0xFF);
        WriteAll(streamFile, bigBuf.data(), bigBuf.size());
        uint64_t fSz = 0;
        std::string fSha;
        Check(AcbHashFile(streamFile, fSz, fSha) && fSz == bigBuf.size() && fSha == Sha256Hex(bigBuf.data(), bigBuf.size()),
              "bak2: AcbHashFile streams multi-chunk file and matches in-memory SHA-256");

        // 3. Settings merge: range checks, group filtering, unknown keys, and D30 fan safety
        const std::string curSettings =
            "{\r\n"
            "  \"appearance.tint\": 68,\r\n"
            "  \"future.customKey\": \"keep-me\",\r\n"
            "  \"mini.opacity\": 85,\r\n"
            "  \"sidebar.width\": 210\r\n"
            "}\r\n";
        const std::string incSettings =
            "{\r\n"
            "  \"appearance.tint\": 80,\r\n"
            "  \"fan.dutyPct\": 15,\r\n"
            "  \"fan.enabled\": true,\r\n"
            "  \"fan.mode\": \"manual\",\r\n"
            "  \"future.fromBackup\": 99,\r\n"
            "  \"mini.opacity\": 50,\r\n"
            "  \"sidebar.sections\": \"<script>\",\r\n"
            "  \"sidebar.width\": 9999\r\n"
            "}\r\n";
        std::string mergedAll;
        int appN = 0, skipN = 0;
        std::wstring mErr;
        Check(AcbMergeSettings(curSettings, incSettings, {}, mergedAll, appN, skipN, mErr) &&
              mergedAll.find("\"appearance.tint\": 80") != std::string::npos &&
              mergedAll.find("\"mini.opacity\": 50") != std::string::npos &&
              mergedAll.find("\"sidebar.width\": 210") != std::string::npos &&          // 9999 rejected by range check
              mergedAll.find("<script>") == std::string::npos &&                        // <script> rejected
              mergedAll.find("\"future.customKey\": \"keep-me\"") != std::string::npos && // existing unknown key kept
              mergedAll.find("\"future.fromBackup\": 99") != std::string::npos &&       // incoming unknown key kept
              mergedAll.find("\"fan.enabled\": false") != std::string::npos &&          // D30: fan control never switched on
              mergedAll.find("\"fan.mode\": \"auto\"") != std::string::npos &&          // D30: manual mode reset to auto
              mergedAll.find("\"fan.dutyPct\": 40") != std::string::npos &&             // D23/D30: 15% clamped to 40%
              skipN == 2,
              "bak2: AcbMergeSettings enforces range checks, keeps unknown keys, and enforces D30 fan safety");

        // Window background / Candy / net-period keys: in-range values restore, out-of-range ones are refused (they were unchecked before)
        {
            const std::string curBg = "{\r\n  \"appearance.bgBlur\": 4,\r\n  \"appearance.bgOpacity\": 27,\r\n  \"candy.bgBlur\": 6,\r\n  \"candy.bgOpacity\": 40,\r\n  \"sidebar.netPeriod\": 1\r\n}\r\n";
            const std::string incBgGood = "{\r\n  \"appearance.bgBlur\": 12,\r\n  \"appearance.bgOpacity\": 55,\r\n  \"candy.bgBlur\": 30,\r\n  \"candy.bgOpacity\": 100,\r\n  \"sidebar.netPeriod\": 3\r\n}\r\n";
            const std::string incBgBad = "{\r\n  \"appearance.bgBlur\": 31,\r\n  \"appearance.bgOpacity\": 101,\r\n  \"candy.bgBlur\": -1,\r\n  \"candy.bgOpacity\": 250,\r\n  \"sidebar.netPeriod\": 4\r\n}\r\n";
            std::string mg, mb; int a1 = 0, s1 = 0, a2 = 0, s2 = 0; std::wstring e1, e2;
            const bool okG = AcbMergeSettings(curBg, incBgGood, {}, mg, a1, s1, e1);
            const bool okB = AcbMergeSettings(curBg, incBgBad, {}, mb, a2, s2, e2);
            Check(okG && s1 == 0 && mg.find("\"appearance.bgBlur\": 12") != std::string::npos && mg.find("\"candy.bgOpacity\": 100") != std::string::npos && mg.find("\"sidebar.netPeriod\": 3") != std::string::npos,
                  "bak2: window-background, Candy and net-period settings in range are restored");
            Check(okB && s2 == 5 && mb.find("\"appearance.bgBlur\": 4") != std::string::npos && mb.find("\"appearance.bgOpacity\": 27") != std::string::npos && mb.find("\"candy.bgOpacity\": 40") != std::string::npos && mb.find("\"sidebar.netPeriod\": 1") != std::string::npos,
                  "bak2: out-of-range background, Candy and net-period settings are refused, the current values stay");
        }

        // Selective group merge (only "mini" group selected -> appearance.tint stays 68)
        std::string mergedMiniOnly;
        Check(AcbMergeSettings(curSettings, incSettings, {"mini"}, mergedMiniOnly, appN, skipN, mErr) &&
              mergedMiniOnly.find("\"mini.opacity\": 50") != std::string::npos &&
              mergedMiniOnly.find("\"appearance.tint\": 68") != std::string::npos &&
              appN == 1,
              "bak2: AcbMergeSettings with group filter updates only selected group ('mini')");

        // 4. Create backup + restore + recovery point + rollback on failure
        const std::string origSettings =
            "{\r\n"
            "  \"appearance.material\": 0,\r\n"
            "  \"appearance.tint\": 74,\r\n"
            "  \"monitor.intervalSec\": 2,\r\n"
            "  \"settings.version\": 1,\r\n"
            "  \"sidebar.width\": 240,\r\n"
            "  \"unknown.toolKey\": \"v1\"\r\n"
            "}\r\n";
        const std::string origNet = "v1\r\nadapter={11111111-2222-3333-4444-555555555555} rx=102400 tx=51200\r\n";
        WriteAtomic(roots.appdata + L"\\settings.json", reinterpret_cast<const uint8_t*>(origSettings.data()), origSettings.size());
        WriteAtomic(roots.data + L"\\net-counters.txt", reinterpret_cast<const uint8_t*>(origNet.data()), origNet.size());

        const std::wstring archivePath = bakSandbox + L"\\ankh_test.acbackup";
        AcbCheck verified{};
        std::wstring cErr;
        Check(AcbCreateBackup(roots, archivePath, "settings", {}, verified, cErr) &&
              verified.ok && verified.manifest.files.size() == 2,
              "bak2: AcbCreateBackup packages settings.json and net-counters.txt and passes AcbVerify");

        // Mutate live files in sandbox before restore
        const std::string mutatedSettings =
            "{\r\n"
            "  \"appearance.material\": 2,\r\n"
            "  \"appearance.tint\": 40,\r\n"
            "  \"local.preservedKey\": \"stay\",\r\n"
            "  \"monitor.intervalSec\": 10,\r\n"
            "  \"settings.version\": 1,\r\n"
            "  \"sidebar.width\": 160\r\n"
            "}\r\n";
        const std::string mutatedNet = "v1\r\nadapter={11111111-2222-3333-4444-555555555555} rx=999 tx=111\r\n";
        WriteAtomic(roots.appdata + L"\\settings.json", reinterpret_cast<const uint8_t*>(mutatedSettings.data()), mutatedSettings.size());
        WriteAtomic(roots.data + L"\\net-counters.txt", reinterpret_cast<const uint8_t*>(mutatedNet.data()), mutatedNet.size());

        // Test simulated failure after 1st file -> must roll back settings.json to mutatedSettings!
        AcbRestoreOptions failOpts;
        failOpts.failAfterFiles = 1;
        const AcbRestoreReport failRep = AcbRestoreBackup(roots, archivePath, failOpts);
        Check(!failRep.ok && failRep.rolledBack &&
              ReadText(roots.appdata + L"\\settings.json") == mutatedSettings &&
              ReadText(roots.data + L"\\net-counters.txt") == mutatedNet &&
              FileExists(failRep.recoveryDir + L"\\recovery-manifest.json") &&
              FileExists(failRep.reportPath) &&
              ReadText(failRep.reportPath).find("ROLLED BACK") != std::string::npos,
              "bak2: mid-restore failure rolls back already-written files from the recovery point");
        {   // B3: a rollback that cannot put a file back must say so (it used to report ROLLED BACK with the file still changed)
            const std::wstring sj = roots.appdata + L"\\settings.json";
            for (int k = 0; k < 2; ++k) {
                WriteAtomic(sj, reinterpret_cast<const uint8_t*>(mutatedSettings.data()), mutatedSettings.size());
                AcbRestoreOptions bad; bad.failAfterFiles = 1;
                bad.beforeRollback = [k](const std::wstring& recDir) {
                    const std::wstring copy = recDir + L"\\files\\settings\\settings.json";
                    if (k == 0) DeleteFileW(copy.c_str());                                                       // the recovery copy is gone
                    else { const std::string junk = "damaged"; WriteAll(copy, reinterpret_cast<const uint8_t*>(junk.data()), junk.size()); }   // or no longer matches its SHA-256
                };
                const AcbRestoreReport br = AcbRestoreBackup(roots, archivePath, bad);
                const std::string rtext = ReadText(br.reportPath);
                bool listed = false; for (const auto& p : AcbListRecoveryPoints(roots)) if (p.dir == br.recoveryDir && p.status == L"Rollback incomplete") listed = true;
                Check(!br.ok && !br.rolledBack && br.rollbackIncomplete && br.rollbackFailures.size() == 1 && br.rollbackFailures[0] == L"settings.json" &&
                      br.error.find(L"ROLLBACK INCOMPLETE") != std::wstring::npos && br.error.find(br.recoveryDir) != std::wstring::npos &&
                      rtext.find("Status: ROLLBACK INCOMPLETE") != std::string::npos && rtext.find("NOT PUT BACK: settings.json") != std::string::npos && listed &&
                      ReadText(sj) != mutatedSettings,
                      std::string("bak2: a rollback whose recovery copy is ") + (k == 0 ? "missing" : "damaged") + " reports ROLLBACK INCOMPLETE, names the file, keeps the recovery folder and is listed as such (it is never reported as rolled back)");
            }
            const std::wstring nc = roots.data + L"\\net-counters.txt";
            auto noLeftovers = [&] { return !FileExists(nc + L".acb-tmp") && !FileExists(nc + L".tmp") && !FileExists(sj + L".tmp") && !FileExists(sj + L".acb-tmp"); };
            {   // a file whose replacement failed was never modified: it is not "touched", so a damaged copy of it cannot fail the rollback
                WriteAtomic(sj, reinterpret_cast<const uint8_t*>(mutatedSettings.data()), mutatedSettings.size());
                WriteAtomic(nc, reinterpret_cast<const uint8_t*>(mutatedNet.data()), mutatedNet.size());
                HANDLE lock = CreateFileW(nc.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // the swap-in of net-counters.txt will be refused
                AcbRestoreOptions o3; o3.beforeRollback = [](const std::wstring& recDir) {   // damage the recovery copy of the file that was never changed
                    const std::string junk = "damaged"; WriteAll(recDir + L"\\files\\monitoring\\net-counters.txt", reinterpret_cast<const uint8_t*>(junk.data()), junk.size()); };
                const AcbRestoreReport r3 = AcbRestoreBackup(roots, archivePath, o3);
                if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
                Check(lock != INVALID_HANDLE_VALUE && !r3.ok && r3.rolledBack && !r3.rollbackIncomplete && r3.rollbackFailures.empty() && r3.error.find(L"Failed to replace") != std::wstring::npos &&
                      ReadText(sj) == mutatedSettings && ReadText(nc) == mutatedNet && noLeftovers(),
                      "bak2: a file whose replacement failed is not treated as touched; the files restored before it are put back, user data is as it was, no temporary file is left");
            }
            {   // a partial rollback: one file is put back, the other cannot be, and the report says exactly that
                WriteAtomic(sj, reinterpret_cast<const uint8_t*>(mutatedSettings.data()), mutatedSettings.size());
                WriteAtomic(nc, reinterpret_cast<const uint8_t*>(mutatedNet.data()), mutatedNet.size());
                AcbRestoreOptions o4; o4.failAfterFiles = 2;   // both files are restored, then the failure
                o4.beforeRollback = [](const std::wstring& recDir) { DeleteFileW((recDir + L"\\files\\monitoring\\net-counters.txt").c_str()); };
                const AcbRestoreReport r4 = AcbRestoreBackup(roots, archivePath, o4);
                const std::string t4 = ReadText(r4.reportPath);
                Check(!r4.ok && !r4.rolledBack && r4.rollbackIncomplete && r4.rollbackFailures.size() == 1 && r4.rollbackFailures[0] == L"net-counters.txt" && r4.rollbackPutBack == 1 &&
                      r4.error.find(L"(1 of 2 file(s) put back)") != std::wstring::npos && ReadText(sj) == mutatedSettings && ReadText(nc) == origNet &&
                      t4.find("ROLLBACK INCOMPLETE") != std::string::npos && t4.find("NOT PUT BACK: net-counters.txt") != std::string::npos && noLeftovers(),
                      "bak2: a partial rollback reports N of M put back and names the file it could not, the file it could put back is as it was, no temporary file is left");
            }
            WriteAtomic(sj, reinterpret_cast<const uint8_t*>(mutatedSettings.data()), mutatedSettings.size());   // the next steps start from the known state again
            WriteAtomic(nc, reinterpret_cast<const uint8_t*>(mutatedNet.data()), mutatedNet.size());
            {   // WriteAllFlushed (the staging write of a restored file)
                const std::wstring wf = roots.data + L"\\flushed.bin"; const std::string big(2500000, 'q');
                Check(WriteAllFlushed(wf, reinterpret_cast<const uint8_t*>(big.data()), big.size()) && ReadText(wf) == big && !WriteAllFlushed(roots.data + L"\\no-such-folder\\x.bin", reinterpret_cast<const uint8_t*>(big.data()), 4) &&
                      WriteAllFlushed(wf, reinterpret_cast<const uint8_t*>("z"), 1) && ReadText(wf) == "z",
                      "write flushed: a multi-chunk file is written and read back whole, a missing folder fails cleanly, a rewrite replaces the content");
                DeleteFileW(wf.c_str());
            }
        }

        // Now run full restore -> must merge settings (restoring width=240, tint=74 while keeping local.preservedKey) and restore net-counters.txt
        const AcbRestoreReport okRep = AcbRestoreBackup(roots, archivePath);
        const std::string restoredSettings = ReadText(roots.appdata + L"\\settings.json");
        Check(okRep.ok && !okRep.rolledBack && okRep.filesRestored == 2 &&
              restoredSettings.find("\"sidebar.width\": 240") != std::string::npos &&
              restoredSettings.find("\"appearance.tint\": 74") != std::string::npos &&
              restoredSettings.find("\"monitor.intervalSec\": 2") != std::string::npos &&
              restoredSettings.find("\"local.preservedKey\": \"stay\"") != std::string::npos &&
              restoredSettings.find("\"unknown.toolKey\": \"v1\"") != std::string::npos &&
              ReadText(roots.data + L"\\net-counters.txt") == origNet &&
              FileExists(okRep.recoveryDir + L"\\files\\settings\\settings.json") &&
              FileExists(okRep.recoveryDir + L"\\files\\monitoring\\net-counters.txt") &&
              ReadText(okRep.reportPath).find("SUCCESS") != std::string::npos,
              "bak2: AcbRestoreBackup creates recovery point, merges settings, restores monitoring counters, and writes report");

        // 5. M-BAK3 / M-BAK4 / M-BAK5: UniGetUI detection, SerializableBundle v3, inventory.json, restore plan, safe CLI args, Recovery History
        const std::wstring fakeUg = bakSandbox + L"\\UniGetUI";
        EnsureDir(fakeUg);
        WriteAll(fakeUg + L"\\UniGetUI.exe", reinterpret_cast<const uint8_t*>("MZ"), 2);
        Check(!AcbDetectUniGetUI({fakeUg}).found, "bak3: AcbDetectUniGetUI requires both UniGetUI.exe and uniget.exe beside it");
        WriteAll(fakeUg + L"\\uniget.exe", reinterpret_cast<const uint8_t*>("MZ"), 2);
        WriteAll(fakeUg + L"\\ForceUniGetUIPortable", reinterpret_cast<const uint8_t*>("1"), 1);
        const AcbUniGetInstall ugDet = AcbDetectUniGetUI({fakeUg});
        Check(ugDet.found && ugDet.portable && ugDet.cliExe == fakeUg + L"\\uniget.exe",
              "bak3: AcbDetectUniGetUI detects portable UniGetUI with uniget.exe beside UniGetUI.exe");
        Check(AcbClassifyUniGetExit(0) == AcbUniGetExit::Success &&
              AcbClassifyUniGetExit(3) == AcbUniGetExit::IpcUnavailable &&
              AcbClassifyUniGetExit(4) == AcbUniGetExit::UnknownCommand,
              "bak3: AcbClassifyUniGetExit maps documented exit codes (0, 3 IPC unavailable, 4 unknown command)");

        AcbUbundle bun;
        bun.packages.push_back({"7zip.7zip", "7-Zip", "24.08", "winget", "Winget"});
        bun.packages.push_back({"VideoLAN.VLC", "VLC media player", "3.0.21", "winget", "Winget"});
        bun.packages.push_back({"ScoopExtras.Fzf", "fzf", "0.55.0", "extras", "Scoop"});
        bun.incompatible.push_back({"", "CustomVendorTool", "1.4.2", "Local PC"});
        const std::string bunJson = AcbBuildUbundle(bun);
        AcbUbundle parsedBun;
        std::wstring bErr;
        Check(AcbParseUbundle(bunJson, parsedBun, bErr) &&
              parsedBun.exportVersion == 3 && parsedBun.packages.size() == 3 && parsedBun.incompatible.size() == 1 &&
              parsedBun.packages[0].id == "7zip.7zip" && parsedBun.incompatible[0].name == "CustomVendorTool",
              "bak3: AcbBuildUbundle and AcbParseUbundle round-trip SerializableBundle v3");

        const std::string invJson = AcbBuildAppInventoryJson(parsedBun, {{"7-Zip", "24.08"}, {"UnlistedLegacyApp", "2.0"}});
        JVal invVal;
        Check(JsonParse(invJson, invVal) && invVal.Get("applications") && invVal.Get("applications")->a.size() == 5 &&
              invJson.find("\"name\": \"UnlistedLegacyApp\"") != std::string::npos &&
              invJson.find("\"manager\": \"manual\"") != std::string::npos,
              "bak3: AcbBuildAppInventoryJson combines bundle packages with registry-only manual apps without duplicates");

        // M-BAK4: Application restore plan (Installed, OfferUniGetInstall with version check, ManualInstallRequired)
        const std::vector<AcbPackage> installedNow = {{"7zip.7zip", "7-Zip", "24.08", "winget", "Winget"}};
        const std::vector<AcbPackage> catalogNow = {{"VideoLAN.VLC", "VLC media player", "3.0.20", "winget", "Winget"}}; // older version in catalog -> versionMismatch!
        const auto rPlan = AcbPlanAppRestore(parsedBun, installedNow, catalogNow, {"Winget"}); // Scoop not available -> fzf becomes ManualInstallRequired
        Check(rPlan.size() == 4 &&
              rPlan[0].action == AcbAppRestoreAction::AlreadyInstalled &&
              rPlan[1].action == AcbAppRestoreAction::OfferUniGetInstall && rPlan[1].versionMismatch && rPlan[1].candidateVersion == "3.0.20" &&
              rPlan[2].action == AcbAppRestoreAction::ManualInstallRequired && rPlan[2].message == kAcbManualInstallMsg &&
              rPlan[3].action == AcbAppRestoreAction::ManualInstallRequired && rPlan[3].message == kAcbManualInstallMsg,
              "bak4: AcbPlanAppRestore classifies Installed, OfferUniGetInstall (with version mismatch guard), and ManualInstallRequired");

        const std::wstring installArgs = AcbBuildUniGetInstallArgs(parsedBun.packages[1], "3.0.21", false, true);
        Check(installArgs.find(L"--skip-hash") == std::wstring::npos &&
              installArgs.find(L"--elevated false") != std::wstring::npos &&
              installArgs.find(L"--interactive true") != std::wstring::npos &&
              installArgs.find(L"--wait true") != std::wstring::npos,
              "bak4: AcbBuildUniGetInstallArgs never passes --skip-hash and honors user elevation/interactivity + --wait true");

        // M-BAK5: Pre-change recovery point and AcbListRecoveryPoints
        std::wstring preDir;
        Check(AcbCreatePreChangeRecoveryPoint(roots, L"Unit test theme change", preDir) &&
              FileExists(preDir + L"\\recovery-manifest.json"),
              "bak5: AcbCreatePreChangeRecoveryPoint creates pre-change recovery snapshot");
        const auto recPts = AcbListRecoveryPoints(roots);
        const auto listedBaks = AcbListBackups(bakSandbox);
        Check(recPts.size() >= 3 && recPts[0].status == L"Pre-change snapshot" &&
              listedBaks.size() == 1 && listedBaks[0].check.ok,
              "bak5: AcbListRecoveryPoints and AcbListBackups enumerate recovery points and verified .acbackup files");

        Evict(bakSandbox, true, false);
    }
    {   // M-SYS1 (SYS-1, SYS-4, SYS-5): Tweaks categories, Driver filter, and Storage Analyzer
        int perf = 0, gam = 0, priv = 0, pwr = 0;
        for (const auto& t : Tweaks()) {
            if (t.category == L"Performance") ++perf;
            else if (t.category == L"Gaming") ++gam;
            else if (t.category == L"Privacy") ++priv;
            else if (t.category == L"Power & Storage") ++pwr;
        }
        Check(perf >= 4 && gam >= 3 && priv >= 3 && pwr >= 1,
              "sys1: Tweaks() are categorized into Performance, Gaming, Privacy, and Power & Storage");

        DriverEntry ms{L"ACPI Fan", L"Microsoft", L"10.0", L"2024-01-01", L"System", L"machine.inf", true};
        DriverEntry nv{L"NVIDIA Quadro", L"NVIDIA", L"32.0", L"2025-02-01", L"Display", L"nv.inf", true};
        DriverEntry un{L"Legacy Tool", L"Vendor", L"1.0", L"2020-01-01", L"Other", L"leg.inf", false};
        Check(DriverFilterMatches(ms, 0) && !DriverFilterMatches(ms, 1) &&
              DriverFilterMatches(nv, 1) && !DriverFilterMatches(nv, 2) && DriverFilterMatches(un, 2),
              "sys4: DriverFilterMatches filters All, Third-party, and Unsigned drivers");

        wchar_t tmpBuf[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tmpBuf);
        const std::wstring storDir = std::wstring(tmpBuf) + L"\\ankh_sys5_test_" + std::to_wstring(GetCurrentProcessId());
        Evict(storDir, true, false);
        EnsureDir(storDir + L"\\sub");
        WriteAll(storDir + L"\\a.bin", reinterpret_cast<const uint8_t*>("123456"), 6);
        WriteAll(storDir + L"\\sub\\b.bin", reinterpret_cast<const uint8_t*>("1234"), 4);
        uint64_t fcnt = 0;
        const uint64_t bytes = FolderSizeBytes(storDir, fcnt, 3);
        const auto roots = AnalyzeStorageRoots(storDir, L"", L"", L"");
        Check(bytes == 10 && fcnt == 2 && roots.size() == 1 && roots[0].bytes == 10 && roots[0].files == 2,
              "sys5: FolderSizeBytes and AnalyzeStorageRoots measure folder byte & file counts");
        Evict(storDir, true, false);
    }
    {   // M-CFG1: Temperature unit conversion (MON-11), SDiskIo series (MON-10), Support bundle export (LOG-3)
        Check(std::abs(ConvertTempUnit(25.0, false) - 25.0) < 1e-6 && std::abs(ConvertTempUnit(25.0, true) - 77.0) < 1e-6,
              "mon11: ConvertTempUnit converts Celsius and Fahrenheit accurately");

        MonitorStore st; st.Configure(1, 60);
        MonitorSample smp; smp.fresh = true; smp.v[SDiskIo] = 1048576.0;
        st.Tick(1000, smp);
        Check(st.Get(SDiskIo).Size() == 1 && std::abs(st.Get(SDiskIo).Last() - 1048576.0) < 1e-6,
              "mon10: MonitorStore records SDiskIo combined disk throughput series");

        wchar_t tmpBuf[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tmpBuf);
        const std::wstring bundleDir = std::wstring(tmpBuf) + L"\\ankh_log3_test_" + std::to_wstring(GetCurrentProcessId());
        Evict(bundleDir, true, false);
        EnsureDir(bundleDir);
        WriteAll(bundleDir + L"\\ankhcore.log", reinterpret_cast<const uint8_t*>("log-line\n"), 9);
        AcbRoots roots; roots.data = bundleDir;
        const std::wstring zipPath = bundleDir + L"\\support.zip";
        std::wstring err;
        const bool ok = AcbExportSupportBundle(roots, zipPath, "AnkhCore Test Summary\r\n", err);
        std::vector<uint8_t> zipRaw;
        const bool readOk = ok && ReadAll(zipPath, zipRaw);
        std::vector<ZipEntry> entries;
        const bool zipOk = readOk && ZipList(zipRaw.data(), zipRaw.size(), entries, err);
        Check(ok && zipOk && entries.size() >= 2,
              "log3: AcbExportSupportBundle produces a verified .zip bundle with summary and log");
        {   // B1: a bundle with the same name from two sources must not fail the whole export
            std::vector<ZipItem> dup(2); dup[0].name = dup[1].name = "ankhcore.log"; dup[0].data = {'a'}; dup[1].data = {'b'};
            std::vector<uint8_t> dz; std::vector<ZipEntry> de; std::wstring dErr;
            Check(ZipWrite(dup, 0, 0, dz) && !ZipList(dz.data(), dz.size(), de, dErr) && dErr.find(L"same name") != std::wstring::npos,
                  "bundle: a ZIP with two entries of one name is refused by the verifier (the mechanism that failed the export)");
            const std::wstring bdir = std::wstring(tmpBuf) + L"\\ankh_b1_test_" + std::to_wstring(GetCurrentProcessId());
            Evict(bdir, true, false); EnsureDir(bdir + L"\\Data\\Logs"); EnsureDir(bdir + L"\\Appdata");
            const char* body = "x";
            for (const wchar_t* f : {L"\\Data\\Logs\\ankhcore.log", L"\\Data\\ankhcore.log", L"\\Data\\net-counters.txt", L"\\Appdata\\net-counters.txt", L"\\Appdata\\settings.json"})
                WriteAll(bdir + f, reinterpret_cast<const uint8_t*>(body), 1);
            AcbRoots r2; r2.data = bdir + L"\\Data"; r2.appdata = bdir + L"\\Appdata";
            std::wstring e2; const std::wstring z2 = bdir + L"\\bundle.zip";
            const bool ok2 = AcbExportSupportBundle(r2, z2, "summary\r\n", e2);
            std::vector<uint8_t> raw2; std::vector<ZipEntry> en2; std::wstring e3;
            const bool listed = ok2 && ReadAll(z2, raw2) && ZipList(raw2.data(), raw2.size(), en2, e3);
            std::map<std::string, int> cnt; for (const auto& e : en2) ++cnt[e.name];
            bool sourcesKept = true; for (const wchar_t* f : {L"\\Data\\Logs\\ankhcore.log", L"\\Data\\ankhcore.log", L"\\Data\\net-counters.txt", L"\\Appdata\\net-counters.txt", L"\\Appdata\\settings.json"}) sourcesKept &= ReadText(bdir + f) == "x";
            Check(ok2 && listed && en2.size() == 4 && cnt["ankhcore.log"] == 1 && cnt["net-counters.txt"] == 1 && cnt["settings.json"] == 1 && cnt["system-summary.txt"] == 1 && sourcesKept && !FileExists(z2 + L".tmp"),
                  "bundle: the same file name found in two places is included once (the first source wins), the export verifies, the source files are untouched and no temporary file is left (" + std::to_string(en2.size()) + " entries)");
            r2.appdata = r2.data;   // the test build's roots: both are one folder, so every name is found twice
            const bool ok3 = AcbExportSupportBundle(r2, bdir + L"\\bundle2.zip", "", e2);
            Check(ok3, "bundle: roots that name the same folder twice also export");
            Evict(bdir, true, false);
        }
        Evict(bundleDir, true, false);
    }
    {   // navigation rail (NAV-2): scroll range, reveal, tooltip placement
        Check(NavScrollMax(480, 540) == 0 && NavScrollMax(632, 540) == 92 && NavScrollMax(632, -5) == 632, "nav: no scroll range when the rows fit; the overflow otherwise");
        Check(NavScrollClamp(92, 480, 540) == 0 && NavScrollClamp(200, 632, 540) == 92 && NavScrollClamp(-7, 632, 540) == 0 && NavScrollClamp(40, 632, 540) == 40,
              "nav: a stale offset is pulled back after a collapse or a taller window; a valid one is kept");
        Check(NavScrollReveal(92, 0, 42, 632, 540) == 0, "nav reveal: a row above the viewport scrolls up to its top");
        Check(NavScrollReveal(0, 590, 632, 632, 540) == 92, "nav reveal: a row below the viewport scrolls down just enough");
        Check(NavScrollReveal(40, 100, 142, 632, 540) == 40 && NavScrollReveal(40, 40, 82, 632, 540) == 40 && NavScrollReveal(40, 538, 580, 632, 540) == 40,
              "nav reveal: a row already fully visible (edges included) does not move the rail");
        Check(NavScrollReveal(0, 10, 52, 300, 540) == 0 && NavScrollReveal(500, 10, 52, 300, 540) == 0, "nav reveal: nothing scrolls when the rows fit");
        Check(NavScrollReveal(0, 300, 400, 900, 60) == 300, "nav reveal: a row taller than the viewport shows its top");
        bool always = true;
        for (int view : {120, 480, 540}) for (int content : {300, 632, 900}) for (int top = 0; top + 42 <= content; top += 19) for (int sc : {0, 55, 400}) {
            const int s = NavScrollReveal(sc, top, top + 42, content, view);
            always &= s >= 0 && s <= NavScrollMax(content, view) && top >= s && top + 42 <= s + std::max(view, content);
            if (content > view) always &= top + 42 <= s + view;
        }
        Check(always, "nav reveal: for every row, viewport and start offset the row ends fully visible and the offset in range");
        const RECT t1 = NavTipRect(64, 300, 80, 12, 15, 6, 1140, 740);
        Check(t1.left == 70 && t1.right == 174 && t1.top == 285 && t1.bottom == 315, "nav tooltip: beside the rail, centred on its row");
        const RECT t2 = NavTipRect(64, 4, 80, 12, 15, 6, 1140, 740), t3 = NavTipRect(64, 738, 80, 12, 15, 6, 1140, 740);
        Check(t2.top == 6 && t2.bottom == 36 && t3.bottom == 734 && t3.top == 704, "nav tooltip: kept inside the window at the top and bottom edges");
        const RECT t4 = NavTipRect(64, 300, 5000, 12, 15, 6, 400, 740);
        Check(t4.left == 70 && t4.right == 394, "nav tooltip: a long name never leaves the window (the text is ellipsized)");
    }
    {   // Cursor Manager (APP-4): names as they are in the owner's packs (file names only; no cursor file is read here)
        const auto& rt = CurRoleTable();
        const wchar_t* order[] = {L"Arrow", L"Help", L"AppStarting", L"Wait", L"Crosshair", L"IBeam", L"NWPen", L"No", L"SizeNS", L"SizeWE", L"SizeNWSE", L"SizeNESW", L"SizeAll", L"UpArrow", L"Hand", L"Pin", L"Person"};
        bool ord = true; for (int i = 0; i < kCurRoles; ++i) ord &= std::wstring(rt[i].key) == order[i];
        Check(ord, "cursor roles: 17 roles in the order of a Windows scheme list");
        const CurName n1 = CurParseName(L"[SB] Normal Select v3.0.ani");
        Check(n1.role == 0 && n1.base == L"Normal Select" && n1.version == L"3.0" && n1.tags.size() == 1 && n1.tags[0] == L"SB" && n1.ani && !n1.premium && n1.variant.empty(),
              "cursor name: [SB] tag and version are not part of the role name");
        const CurName n2 = CurParseName(L"[PREMIUM] [TB] Diagonal Resize 2 v3.2.ani");
        Check(n2.role == 11 && n2.premium && n2.tags.size() == 2 && n2.tags[1] == L"TB", "cursor name: [PREMIUM] [TB] is recognised as premium; Diagonal Resize 2 = SizeNESW");
        const CurName n3 = CurParseName(L"[PREMIUM] [PR] Link Select v3.2 - Glow.ani");
        Check(n3.role == 14 && n3.variant == L"Glow", "cursor name: a ' - Glow' suffix is a variant of Link Select");
        const CurName n4 = CurParseName(L"[PREMIUM] [Stat] [PR] Precision Select v3.2 - Style 1 (w Mid-Point).cur");
        Check(n4.role == 4 && !n4.ani && n4.cursorFile && n4.variant == L"Style 1 (w Mid-Point)", "cursor name: static variant with a hyphen inside the variant text");
        Check(CurParseName(L"cross.ani").role == 4 && CurParseName(L"Cross.ani").role == 4 && CurParseName(L"NO.ani").role == 7 && CurParseName(L"Handwriting.ani").role == 6 &&
              CurParseName(L"SizeNWSE.ani").role == 10 && CurParseName(L"AppStarting.ani").role == 2 && CurParseName(L"IBeam.ani").role == 5,
              "cursor name: registry-style names (cross, NO, Handwriting, SizeNWSE) map without regard to case");
        Check(CurParseName(L"button.ani").role == -1 && CurParseName(L"Arrow.txt").role == -1 && !CurParseName(L"readme.txt").cursorFile && CurParseName(L"Normal Select Extra.ani").role == -1,
              "cursor name: an unknown name or a non-cursor file maps to no role (nothing is guessed)");
        auto pack = [](const wchar_t* tag, const wchar_t* ver) {
            std::vector<std::wstring> v;
            for (const wchar_t* l : {L"Alternate Select", L"Busy", L"Diagonal Resize 1", L"Diagonal Resize 2", L"Handwriting", L"Help Select", L"Horizontal Resize", L"Link Select", L"Location Select",
                                     L"Move", L"Normal Select", L"Person Select", L"Precision Select", L"Text Select", L"Unavailable", L"Vertical Resize", L"Working In Background"})
                v.push_back(std::wstring(tag) + L" " + l + L" " + ver + L".ani");
            return v;
        };
        const CurScheme sb = CurMapScheme(L"Night Diamond - Sapphire Blue v3.0", L"C:\\Windows\\Cursors\\Night Diamond - Sapphire Blue v3.0", pack(L"[SB]", L"v3.0"));
        Check(sb.Mapped() == 17 && sb.Missing().empty() && !sb.premium && sb.tags.size() == 1 && sb.tags[0] == L"SB" && sb.unassigned.empty() && sb.alternates.empty() &&
              sb.file[0] == L"[SB] Normal Select v3.0.ani" && sb.file[15] == L"[SB] Location Select v3.0.ani", "cursor map: the [SB] pack fills all 17 roles and is not premium");
        const CurScheme tbp = CurMapScheme(L"TechnoBlue Pro", L"D:\\c\\TB", pack(L"[PREMIUM] [TB]", L"v3.2"));
        Check(tbp.Mapped() == 17 && tbp.premium && tbp.tags.size() == 1 && tbp.tags[0] == L"TB", "cursor map: the [PREMIUM] [TB] pack fills all 17 roles and is marked premium");
        std::vector<std::wstring> pr = pack(L"[PREMIUM] [PR]", L"v3.2");
        pr.push_back(L"Bonus\\[PREMIUM] [PR] Link Select v3.2 - Glow.ani"); pr.push_back(L"Bonus\\Statics\\[PREMIUM] [Stat] [PR] Move v3.2.cur"); pr.push_back(L"Bonus\\[PREMIUM] [PR] Busy v3.2 - Classic.ani");
        const CurScheme prs = CurMapScheme(L"Prismatic", L"D:\\c\\PR", pr);
        Check(prs.Mapped() == 17 && prs.alternates.size() == 3 && prs.file[14] == L"[PREMIUM] [PR] Link Select v3.2.ani" && prs.file[12] == L"[PREMIUM] [PR] Move v3.2.ani",
              "cursor map: Bonus and Statics files are alternates; the top-level file is the role's cursor");
        const std::vector<std::wstring> azenis = {L"AppStarting.ani", L"button.ani", L"cross.ani", L"Hand.ani", L"Handwriting.ani", L"Help.ani", L"IBeam.ani", L"NO.ani", L"SizeAll.ani",
                                                  L"SizeNESW.ani", L"SizeNS.ani", L"SizeNWSE.ani", L"SizeWE.ani", L"UpArrow.ani", L"Wait.ani", L"notes.txt"};
        const CurScheme az = CurMapScheme(L"Azenis", L"D:\\c\\Azenis", azenis);
        const std::vector<int> azMiss = az.Missing();
        Check(az.Mapped() == 15 && azMiss.size() == 2 && azMiss[0] == 15 && azMiss[1] == 16 && az.unassigned.empty() && az.file[0] == L"button.ani" && az.aliased.size() == 1 && az.aliased[0] == 0,
              "cursor map (FT-F01): Azenis has no Arrow file; its only other cursor, button.ani, is Normal Select (listed as assigned by AnkhCore); Pin and Person stay missing");
        const CurScheme other = CurMapScheme(L"Other", L"D:\\c\\Other", {L"Wait.ani", L"button.ani"});
        Check(other.Mapped() == 1 && other.unassigned.size() == 1 && other.aliased.empty() && other.file[0].empty(), "cursor map (FT-F01): the alias is only for the pack it was checked in; any other button.ani stays unassigned");
        const CurScheme azOwn = CurMapScheme(L"Azenis", L"D:\\c\\Azenis", {L"Arrow.ani", L"button.ani"});
        Check(azOwn.file[0] == L"Arrow.ani" && azOwn.aliased.empty() && azOwn.unassigned.size() == 1, "cursor map (FT-F01): a real Arrow file is never replaced by the alias");
        const CurScheme dup = CurMapScheme(L"Dup", L"D:\\c\\Dup", {L"Arrow.cur", L"Arrow.ani", L"Wait.cur"});
        Check(dup.file[0] == L"Arrow.ani" && dup.alternates.size() == 1 && dup.alternates[0] == L"Arrow.cur" && dup.file[3] == L"Wait.cur", "cursor map: two files for one role: the animated one is used, the other is an alternate");
        const CurState azT = CurTargetState(az, L"C:\\Windows");
        Check(azT.name == L"Azenis" && azT.source == 1 && azT.value[0] == L"D:\\c\\Azenis\\button.ani" && azT.value[15] == L"%SystemRoot%\\cursors\\aero_pin.cur" &&
              azT.value[3] == L"D:\\c\\Azenis\\Wait.ani" && azT.exists[0], "cursor target: the Azenis Normal Select is its own button.ani (FT-F01); a still-missing role (Pin) gets the Windows default cursor, never another scheme's file");
        const CurState sbT = CurTargetState(sb, L"C:\\WINDOWS");
        Check(sbT.value[0] == L"%SystemRoot%\\Cursors\\Night Diamond - Sapphire Blue v3.0\\[SB] Normal Select v3.0.ani" && CurPortablePath(L"C:\\WindowsOld\\x.cur", L"C:\\Windows") == L"C:\\WindowsOld\\x.cur",
              "cursor target: files under the Windows folder are stored as %SystemRoot% paths; a look-alike folder is not");
        const CurState wd = CurWindowsDefault();
        Check(wd.name == L"Windows Default" && wd.source == 2 && wd.value[4].empty() && wd.value[5].empty() && wd.value[3] == L"%SystemRoot%\\cursors\\aero_busy.ani", "cursor defaults: Windows Default has empty Crosshair and IBeam (built in) and aero files for the rest");
        const std::wstring csv = L"C:\\a\\1.ani,C:\\a\\2.ani, C:\\a\\3.ani ,,C:\\a\\5.cur";
        const auto lst = CurParseSchemeList(csv);
        Check(lst[0] == L"C:\\a\\1.ani" && lst[2] == L"C:\\a\\3.ani" && lst[3].empty() && lst[4] == L"C:\\a\\5.cur" && lst[16].empty(), "cursor scheme list: a short registry list fills the first roles; the rest stay empty");
        Check(CurParseSchemeList(CurBuildSchemeList(sbT.value)) == sbT.value, "cursor scheme list: build and parse round-trip all 17 entries");
        CurState back; CurState odd = azT; odd.exists[16] = false; odd.value[16].clear(); odd.hasSource = false; odd.source = 0; odd.name = L"As3aD2";
        Check(CurDecodeState(CurEncodeState(odd), back) && back == odd && CurDecodeState(CurEncodeState(wd), back) && back == wd, "cursor backup: the saved scheme round-trips, including a role with no value and no Scheme Source");
        std::wstring enc = CurEncodeState(azT);
        CurState junk;
        std::wstring noHead = enc; noHead[0] = L'x';
        std::wstring swapped = enc; { const size_t a = swapped.find(L"Help="), b = swapped.find(L"AppStarting="); std::swap_ranges(swapped.begin() + a, swapped.begin() + a + 4, swapped.begin() + b); }
        std::wstring cut = enc.substr(0, enc.rfind(L"Person="));
        std::wstring ctl = enc; ctl[ctl.find(L"name=") + 5] = L'\x01';
        std::wstring badSrc = enc; badSrc.replace(badSrc.find(L"source=1"), 8, L"source=7");
        Check(!CurDecodeState(noHead, junk) && !CurDecodeState(swapped, junk) && !CurDecodeState(cut, junk) && !CurDecodeState(ctl, junk) && !CurDecodeState(badSrc, junk) && !CurDecodeState(L"", junk),
              "cursor backup: a damaged file (header, role order, missing role, control character, bad source) is rejected");
        const uint8_t ani[] = {'R', 'I', 'F', 'F', 0x7E, 0x4C, 2, 0, 'A', 'C', 'O', 'N', 'a', 'n', 'i', 'h'};
        const uint8_t cur[] = {0, 0, 2, 0, 1, 0, 0x20, 0x20, 0, 0, 5, 0, 3, 0, 0xA8, 0x10, 0, 0, 0x16, 0, 0, 0};
        const uint8_t ico[] = {0, 0, 1, 0, 1, 0, 0x20, 0x20, 0, 0, 1, 0, 32, 0, 0xA8, 0x10, 0, 0, 0x16, 0, 0, 0};
        Check(CurValidHeader(ani, sizeof(ani), true) && CurValidHeader(cur, sizeof(cur), false) && !CurValidHeader(cur, sizeof(cur), true) && !CurValidHeader(ani, sizeof(ani), false) &&
              !CurValidHeader(ico, sizeof(ico), false) && !CurValidHeader(ani, 8, true) && !CurValidHeader(nullptr, 0, false),
              "cursor validation: RIFF/ACON for .ani, cursor directory for .cur; an icon, a short file or the wrong kind is rejected");
        std::vector<CurState> tg = {sbT, azT};
        CurState now = azT; for (auto& v : now.value) v = CurLower(v);
        CurState mix = sbT; mix.value[3] = azT.value[3];
        Check(CurFindActive(tg, now) == 1 && CurFindActive(tg, sbT) == 0 && CurFindActive(tg, mix) == -1 && CurFindActive(tg, wd) == -1, "cursor active: the active scheme is found by all 17 roles, ignoring case; a mix of two schemes matches none");
        // discovery on real folders (a temp folder; read-only functions: no registry access in this suite)
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        const std::wstring croot = std::wstring(tb) + L"ankh_cur_test_" + std::to_wstring(GetCurrentProcessId());
        Evict(croot, true, false);
        auto putc = [&](const std::wstring& rel, const uint8_t* d, size_t n) { EnsureDir(DirOf(croot + L"\\" + rel)); return WriteAll(croot + L"\\" + rel, d, n); };
        bool wrote = putc(L"Pack A\\[SB] Normal Select v3.0.ani", ani, sizeof(ani)) && putc(L"Pack A\\[SB] Busy v3.0.ani", ani, sizeof(ani)) && putc(L"Pack A\\Bonus\\[SB] Busy v3.0 - Classic.ani", ani, sizeof(ani)) &&
                     putc(L"Pack A\\Bonus\\Statics\\[Stat] [SB] Move v3.0.cur", cur, sizeof(cur)) && putc(L"Pack A\\readme.txt", ani, 4) &&
                     putc(L"Pack B\\Arrow.cur", ani, sizeof(ani)) && putc(L"Pack B\\Wait.cur", cur, sizeof(cur)) && putc(L"Empty\\notes.txt", ani, 4) && putc(L"aero_arrow.cur", cur, sizeof(cur));
        const auto found = CurDiscoverFolder(croot);
        Check(wrote && found.size() == 2 && found[0].name == L"Pack A" && found[0].Mapped() == 2 && found[0].alternates.size() == 2 && found[0].invalid.empty() && found[0].dir == croot + L"\\Pack A" &&
              found[1].name == L"Pack B" && found[1].Mapped() == 2 && found[1].invalid.size() == 1 && found[1].invalid[0] == L"Arrow.cur",
              "cursor discovery: each sub-folder with cursor files is a pack; loose aero files and folders without cursors are not; a .cur with the wrong content is flagged");
        const auto single = CurDiscoverFolder(croot + L"\\Pack A");
        Check(single.size() == 1 && single[0].name == L"Pack A" && single[0].Mapped() == 2 && single[0].alternates.size() == 2 && CurDiscoverFolder(croot + L"\\nope").empty(),
              "cursor discovery: a folder that is itself one pack is found as one scheme; a missing folder gives none");
        Check(CurFileHeaderOk(croot + L"\\Pack B\\Wait.cur") && !CurFileHeaderOk(croot + L"\\Pack B\\Arrow.cur") && !CurFileHeaderOk(croot + L"\\Pack A\\readme.txt") && !CurFileHeaderOk(croot + L"\\gone.ani"),
              "cursor validation: files are checked by content and extension; a missing file is invalid");
        Evict(croot, true, false);
        const std::wstring realPacks = L"D:\\Ankhcore\\windows\\Cursors";   // the supplied packs, read only (nothing is applied)
        if (DirExists(realPacks)) {
            const auto real = CurDiscoverFolder(realPacks);
            std::set<std::wstring> seen; bool ok = !real.empty(), az = false;
            for (const auto& s : real) { bool good = seen.insert(Lower(s.dir)).second && (s.Mapped() > 0 || s.name.find(L"bonus-cursors") != std::wstring::npos) && s.invalid.empty(); if (s.name == L"Ankhcore scursor_green") { good = good && s.Mapped() == 15 && s.file[0] == L"SCursor_point.cur" && s.file[13] == L"SCursor_alt.cur" && s.file[15].empty() && s.unassigned.empty(); } if (!good) std::wprintf(L"  pack not ok: %ls (mapped %d, invalid %d)\n", s.name.c_str(), s.Mapped(), static_cast<int>(s.invalid.size())); ok = ok && good; if (s.name == L"Ankhcore Azenis") az = s.file[0] == L"Arrow.ani" && s.aliased.empty(); }
            Check(ok && real.size() >= 20, "cursor packs (supplied folder): every pack folder with cursor files is found once, maps to roles and has no invalid file");
            Check(az, "cursor packs: Ankhcore Azenis now maps its own Arrow.ani to Normal Select (no alias needed)");
        }
    }
    {   // honest progress (LOG-2)
        Check(ProgressOfCount(0, 10) == 0 && ProgressOfCount(5, 10) == 50 && ProgressOfCount(10, 10) == 100 && ProgressOfCount(12, 10) == 100, "progress count: items finished out of items counted");
        Check(ProgressOfCount(99, 100) == 99 && ProgressOfCount(199, 200) == 99 && ProgressOfCount(999, 1000) == 99, "progress count: never 100 before the last item is finished");
        Check(ProgressOfCount(0, 0) == kProgressUnknown && ProgressOfCount(3, 0) == kProgressUnknown, "progress count: nothing counted means unknown, not 0 and not 100");
        OpSteps st(4);
        const int p0 = st.Pct(); st.Next(); const int p1 = st.Pct(); const std::wstring l1 = st.Label(L"Checking");
        st.Next(); const int p2 = st.Pct(); const int p2half = st.Pct(5, 10); const int p2full = st.Pct(10, 10); const std::wstring l2 = st.Label(L"Files");
        st.Next(); st.Next(); const int p4 = st.Pct(); const std::wstring l4 = st.Label(L"Last");
        st.Next(); const int pEnd = st.Pct(); st.Next(); const int pOver = st.Pct();
        Check(p0 == 0 && p1 == 0 && l1 == L"Step 1 of 4: Checking" && p2 == 25 && l2 == L"Step 2 of 4: Files" && p4 == 75 && l4 == L"Step 4 of 4: Last",
              "progress steps: the bar is the finished steps out of the counted steps; the step in work is named 'Step k of n'");
        Check(p2half == 37 && p2full == 50 && st.Pct(99, 10) == 99, "progress steps: a measured share of the step in work moves the bar inside that step only");
        Check(pEnd == 99 && pOver == 99 && st.Current() == 4, "progress steps: closing every step is still not 100 (the caller reports the end); extra calls change nothing");
        bool mono = true; { OpSteps s2(7); int last = 0; for (int i = 0; i < 7; ++i) { s2.Next(); for (size_t f = 0; f <= 15; ++f) { const int p = s2.Pct(f, 15); mono &= p >= last && p < 100; last = p; } } }
        Check(mono && OpSteps(0).total == 1 && OpSteps(-3).total == 1, "progress steps: never goes back and never reaches 100 across 7 steps with 15 measured items each");
        Check(ParseToolPercent(L"Verification 45% complete.") == 45 && ParseToolPercent(L"[===========         20.0%                  ]") == 20 &&
              ParseToolPercent(L"[==========================100.0%==========================]") == 100 && ParseToolPercent(L"0% complete") == 0,
              "tool percent: SFC and DISM lines give the tool's own value");
        Check(ParseToolPercent(L"Progress: 12 of 90 done; Stage: 13%; Total:  4%; ETA:   0:00:12") == 4 && ParseToolPercent(L"Stage: 99%; Total: 100%") == 100,
              "tool percent: chkdsk gives the total, not the stage");
        Check(ParseToolPercent(L"Stage: 13%; Step: 4%") == kProgressUnknown && ParseToolPercent(L"The operation completed successfully.") == kProgressUnknown && ParseToolPercent(L"") == kProgressUnknown &&
              ParseToolPercent(L"%") == kProgressUnknown && ParseToolPercent(L"Total: none") == kProgressUnknown,
              "tool percent: no percentage, or several without a total, gives no value");
        Check(ParseToolPercent(L"Usage 150% of quota") == kProgressUnknown && ParseToolPercent(L"x86% build") == kProgressUnknown && ParseToolPercent(L"change -5% today") == kProgressUnknown &&
              ParseToolPercent(L"100000000000000000000%") == kProgressUnknown && ParseToolPercent(L".%") == kProgressUnknown,
              "tool percent: a value above 100, a number glued to a word and a negative are not progress (nothing is cut down to 100)");
        bool bandOk = true; int widest = 0, leftmost = 1000, rightmost = 0;
        for (unsigned t = 0; t < 3200; t += 7) {
            const ProgressFill f = ProgressFillFor(kProgressUnknown, 220, t, 8);
            bandOk &= f.band && f.left >= 0 && f.right <= 220 && f.right >= f.left && f.right - f.left <= 66;
            widest = std::max(widest, f.right - f.left); leftmost = std::min(leftmost, f.left); rightmost = std::max(rightmost, f.right);
        }
        Check(bandOk && widest == 66 && leftmost == 0 && rightmost == 220, "progress bar: unknown length is a band of at most 30% that travels the whole track and never fills it");
        const ProgressFill z = ProgressFillFor(0, 220, 5, 8), h = ProgressFillFor(50, 220, 5, 8), full = ProgressFillFor(100, 220, 5, 8), tiny = ProgressFillFor(1, 220, 5, 8), big = ProgressFillFor(250, 220, 5, 8);
        Check(!z.band && z.right == 0 && h.right == 110 && full.right == 220 && tiny.right == 8 && big.right == 220 && !h.band && ProgressFillFor(50, 0, 5, 8).right == 0,
              "progress bar: known progress fills in proportion; 0 is empty; only 100 fills the track");
    }
    {   // honest results (SEC-3)
        OpTally none, good, part, bad;
        good.Add(true); good.Add(true); good.Add(true);
        part.Add(true); part.Add(false); part.Add(true);
        bad.Add(false);
        Check(none.Success() && none.Attempted() == 0 && none.Text(L"service", L"services", L"turned off") == L"Nothing to do: no service needed a change.",
              "result tally: attempting nothing is not a failure and is said in words");
        Check(good.Success() && good.Text(L"service", L"services", L"turned off") == L"3 services turned off.", "result tally: all parts succeeded");
        Check(!part.Success() && part.ok == 2 && part.failed == 1 && part.Text(L"service", L"services", L"turned off") == L"2 of 3 services turned off; 1 failed.",
              "result tally: one failed part makes the operation fail, with counts");
        Check(!bad.Success() && bad.Text(L"command", L"commands", L"succeeded") == L"0 of 1 commands succeeded; 1 failed.", "result tally: a single failed part is a failure");
        OpTally one; one.Add(true);
        Check(one.Text(L"service", L"services", L"turned off") == L"1 service turned off.", "result tally: singular wording for one part");
        UndoCount u0, u1{1, 1}, u3{3, 3}, up{3, 2};
        Check(u0.Success() && u0.Nothing() && u0.Text() == L"Nothing to undo: no recorded change matches.", "undo count: nothing selected is reported as nothing to undo");
        Check(u1.Success() && u1.Text() == L"1 change undone." && u3.Success() && u3.Text() == L"3 changes undone.", "undo count: every selected change undone");
        Check(!up.Success() && up.Failed() == 1 && up.Text() == L"2 of 3 changes undone; 1 could not be undone and stay recorded.", "undo count: a change that could not be undone makes the undo fail");
    }
    {   // component package manifest (Preview 2.1): COMPONENTS.sha256 beside the setup
        const std::string h1(64, 'a'), h2(64, '0');
        ComponentManifest m; std::wstring err;
        const std::string good = "\xEF\xBB\xBF# AnkhCore components\r\n" + h1 + "  IconThemes\\sweet\\theme.ini\r\n" + h2 + "  IconThemes/sweet/imageres.dll\n" + h1 + "  Mods\\icon-resource-redirect.wh.cpp\n";
        Check(ParseComponentManifest(good, m, err) && m.sha.size() == 3 && m.sha.count(L"iconthemes\\sweet\\imageres.dll") && m.sha.at(L"mods\\icon-resource-redirect.wh.cpp") == h1,
              "components: manifest parses (BOM, comments, CRLF, forward slashes, case-insensitive keys)");
        ComponentManifest x; std::wstring e2;
        Check(!ParseComponentManifest(std::string(64, 'A') + "  a.txt\n", x, e2) && !ParseComponentManifest(h1 + " a.txt\n", x, e2) && !ParseComponentManifest(h1 + "  ..\\a.txt\n", x, e2) &&
              !ParseComponentManifest(h1 + "  C:\\a.txt\n", x, e2) && !ParseComponentManifest(h1 + "  \\a.txt\n", x, e2) && !ParseComponentManifest(h1 + "  a.txt\n" + h2 + "  A.TXT\n", x, e2) &&
              !ParseComponentManifest("# only a comment\n", x, e2) && !ParseComponentManifest(h1 + "  a\x01.txt\n", x, e2) && !ParseComponentManifest(h1 + "  IconThemes\\.\\x\n", x, e2),
              "components: upper-case hash, one space, '..', drive, rooted path, duplicate, empty manifest, control character and '.' are refused");
        const auto under = ComponentEntriesUnder(m, L"IconThemes\\sweet");
        Check(under.size() == 2 && ComponentEntriesUnder(m, L"IconThemes\\swe").empty() && ComponentEntriesUnder(m, L"Mods\\icon-resource-redirect.wh.cpp").size() == 1 && ComponentEntriesUnder(m, L"..\\x").empty(),
              "components: entries under a folder or one file; a name prefix of a folder does not match");
        std::map<std::wstring, std::string> disk = {{L"iconthemes\\sweet\\theme.ini", h1}, {L"iconthemes\\sweet\\imageres.dll", h2}};
        auto hash = [&](const std::wstring& k) { return disk.count(k) ? disk[k] : std::string(); };
        const std::vector<std::wstring> both = {L"IconThemes\\sweet\\theme.ini", L"IconThemes\\sweet\\imageres.dll"};
        Check(CheckComponent(m, L"IconThemes\\sweet", both, hash).empty(), "components: a complete, unchanged theme passes");
        Check(CheckComponent(m, L"IconThemes\\sweet", {both[0]}, hash).find(L"missing file") == 0 &&
              CheckComponent(m, L"IconThemes\\sweet", {both[0], both[1], L"IconThemes\\sweet\\extra.dll"}, hash).find(L"unlisted file") == 0 &&
              CheckComponent(m, L"IconThemes\\magnum", {}, hash).find(L"not listed") == 0,
              "components: a missing file, an extra unlisted file and an unlisted theme are refused");
        disk[L"iconthemes\\sweet\\imageres.dll"] = h1;
        Check(CheckComponent(m, L"IconThemes\\sweet", both, hash).find(L"changed file") == 0, "components: a changed file (different SHA-256) is refused");
    }
    {   // brand logo display treatment (owner brief 2026-11-14)
        Check(BrandLogoFactor(.5f, .5f) == 1.f && BrandLogoFactor(.5f, .23f) == 1.f && BrandLogoFactor(.5f, .80f) == 1.f && BrandLogoFactor(.33f, .5f) == 1.f && BrandLogoFactor(.67f, .5f) == 1.f,
              "brand logo: the ankh (x 0.33..0.67, y 0.23..0.80) keeps full opacity");
        const float edgeMid = BrandLogoFactor(1.f, .5f);   // 0.50 from the centre: inside the fade
        Check(kBrandFloor > 0.f && std::abs(BrandLogoFactor(0.f, 0.f) - kBrandFloor) < 1e-6f && std::abs(BrandLogoFactor(1.f, 1.f) - kBrandFloor) < 1e-6f && std::abs(BrandLogoFactor(.5f + kBrandOuter + .01f, .5f) - kBrandFloor) < 1e-6f &&
              edgeMid > kBrandFloor && edgeMid < 1.f, "brand logo: the outer circuitry keeps the floor opacity beyond the outer radius (never removed); in between it fades");
        bool mono = true; float last = 2.f;
        for (int i = 0; i <= 100; ++i) { const float f = BrandLogoFactor(.5f + .5f * i / 100.f, .5f); mono &= f <= last + 1e-6f && f >= kBrandFloor - 1e-6f && f <= 1.f; last = f; }
        Check(mono, "brand logo: opacity falls smoothly and only outward from the centre");
        std::vector<uint32_t> img(64 * 64, 0xC8406080u);   // premultiplied: alpha 200, colour 64/96/128
        BrandLogoSoften(img.data(), 64, 64);
        const uint32_t mid = img[32 * 64 + 32], corner = img[0];
        const auto ch = [](uint32_t v, int s) { return static_cast<int>((v >> s) & 255u); };
        Check(mid == 0xC8406080u && ch(corner, 24) == 60 && ch(corner, 16) == 19 && ch(corner, 8) == 29 && ch(corner, 0) == 38,
              "brand logo: the centre is untouched; a corner pixel is scaled as a whole (premultiplied colour stays the same hue)");
        BrandLogoSoften(nullptr, 64, 64); BrandLogoSoften(img.data(), 0, 64);
        Check(true, "brand logo: an empty image is left alone");
    }
    {   // uninstall message (SEC-3)
        const std::wstring dd = L"C:\\ProgramData\\AnkhCore";
        UninstallReport ok; ok.removal.deferred = true;
        const UninstallMessage m1 = UninstallSummary(ok, dd);
        Check(m1.ok && m1.text.rfind(L"AnkhCore was removed. The rest of the program folder is deleted at the next restart.", 0) == 0 && m1.text.find(L"were kept as they are") != std::wstring::npos &&
              m1.text.find(dd) != std::wstring::npos, "uninstall message: everything confirmed says removed, the restart, and that changes were kept");
        UninstallReport u; u.undoAsked = true; u.undo = UndoCount{4, 4}; u.icons = 1;
        const UninstallMessage m2 = UninstallSummary(u, dd);
        Check(m2.ok && m2.text.find(L"4 changes undone.") != std::wstring::npos && m2.text.find(L"Windows icons: put back") != std::wstring::npos && m2.text.find(L"next restart") == std::wstring::npos,
              "uninstall message: undo and icons reported with counts; no restart line when nothing waits for one");
        UninstallReport p = u; p.undo = UndoCount{4, 3};
        const UninstallMessage m3 = UninstallSummary(p, dd);
        Check(!m3.ok && m3.text.find(L"3 of 4 changes undone; 1 could not be undone") != std::wstring::npos && m3.text.rfind(L"AnkhCore was removed.", 0) == 0,
              "uninstall message: a change that could not be undone makes the result a warning, with counts");
        UninstallReport ic = u; ic.icons = 0;
        Check(!UninstallSummary(ic, dd).ok && UninstallSummary(ic, dd).text.find(L"could NOT be fully put back") != std::wstring::npos, "uninstall message: icons that could not be put back are a warning");
        UninstallReport f; f.removal.failed = {L"the scheduled task \\AnkhCore\\Launch", L"the entry in Settings > Apps"};
        const UninstallMessage m5 = UninstallSummary(f, dd);
        Check(!m5.ok && m5.text.rfind(L"AnkhCore was not completely removed.", 0) == 0 && m5.text.find(L"\\AnkhCore\\Launch") != std::wstring::npos &&
              m5.text.find(L"Settings > Apps") != std::wstring::npos && m5.text.find(L"was removed.") == std::wstring::npos,
              "uninstall message: parts that are still there are named, and it never says removed");
        UninstallReport s; s.stillRunning = true;
        Check(!UninstallSummary(s, dd).ok && UninstallSummary(s, dd).text.find(L"did not stop") != std::wstring::npos, "uninstall message: a process that did not stop is a warning");
    }
    {   // FT-2 / A01: what is said after an icon change (no Windows restart for Resource Redirect; only verified facts are stated)
        const std::wstring mod = L"local@icon-resource-redirect";
        Check(IconModuleIsMod(L"local@icon-resource-redirect_1.3_647871.dll", mod) && IconModuleIsMod(L"LOCAL@ICON-RESOURCE-REDIRECT_1.3_1.DLL", mod), "icon refresh: a Windhawk module file name is matched to its mod id");
        Check(!IconModuleIsMod(L"icon-resource-redirect_1.3_1.dll", mod) && !IconModuleIsMod(L"local@icon-resource-redirect-extra_1.0_1.dll", mod) &&
              !IconModuleIsMod(L"local@icon-resource-redirect_1.3_1.whl", mod) && !IconModuleIsMod(L"x.dll", L"") && !IconModuleIsMod(L"", mod), "icon refresh: other mods, other file types and empty names do not match");
        IconRefreshReport r; r.applied = true; r.shellNotified = true; r.inExplorer = IconShellState::Loaded;
        IconRefreshAdvice a = IconAdvice(r);
        Check(a.offerExplorerRestart && !a.warn && !a.needsWindowsRestart && a.text.find(L"loaded in Explorer") != std::wstring::npos && a.text.find(L"does not need a restart") != std::wstring::npos &&
              a.text.find(L"read back") != std::wstring::npos, "icon refresh: a verified apply says what was verified, offers an Explorer restart, and does not ask for a Windows restart");
        Check(a.text.find(L"cannot see that") != std::wstring::npos, "icon refresh: it does not claim that every open window already shows the new icons");
        r.inExplorer = IconShellState::NotLoaded; a = IconAdvice(r);
        Check(a.warn && a.offerExplorerRestart && a.text.find(L"NOT loaded in Explorer") != std::wstring::npos, "icon refresh: a mod that Explorer has not loaded is a warning");
        r.inExplorer = IconShellState::Unknown; a = IconAdvice(r);
        Check(a.warn && a.text.find(L"could not check") != std::wstring::npos, "icon refresh: an Explorer that could not be checked is a warning, not a success");
        r.inExplorer = IconShellState::Loaded; r.shellNotified = false; a = IconAdvice(r);
        Check(a.warn && a.text.find(L"could not be told") != std::wstring::npos, "icon refresh: a failed shell notification is a warning");
        IconRefreshReport none; a = IconAdvice(none);
        Check(a.text.empty() && !a.offerExplorerRestart && !a.needsWindowsRestart, "icon refresh: nothing applied, nothing said");
        IconRefreshReport rm; rm.applied = true; rm.removed = true; rm.shellNotified = true; rm.inExplorer = IconShellState::NotLoaded; a = IconAdvice(rm);
        Check(!a.warn && a.text.find(L"default icons were restored") != std::wstring::npos && a.text.find(L"NOT loaded") == std::wstring::npos, "icon refresh: restoring the default icons needs no mod in Explorer");
        IconRefreshReport old = none; old.oldMethodPending = true; a = IconAdvice(old);
        Check(a.needsWindowsRestart && a.text.find(L"file-swap") != std::wstring::npos && !a.offerExplorerRestart, "icon refresh: only the older file-swap method asks for a Windows restart");
        r.shellNotified = true; r.oldMethodPending = true; a = IconAdvice(r);
        Check(a.needsWindowsRestart && a.offerExplorerRestart && a.text.find(L"file-swap") != std::wstring::npos, "icon refresh: both are said when a 2.2 swap is pending beside a Resource Redirect apply");
        IconRefreshReport done = r; done.oldMethodPending = false; done.explorerRestarted = true; a = IconAdvice(done);
        Check(!a.offerExplorerRestart && !a.warn && a.text.find(L"Explorer was restarted") != std::wstring::npos, "icon refresh: after the user restarted Explorer nothing more is offered");
        Check(ExplorerLoadsMod(L"no-such-mod-for-this-test", 0) != IconShellState::Loaded, "icon refresh: a mod that is not loaded is not reported as loaded (read-only look at Explorer)");
    }
    {   // FT-2 / A04: the installed state is a cheap, cached check; the full validation is not run to paint a page
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring root = tb; while (!root.empty() && root.back() == L'\\') root.pop_back();
        root += L"\\ankh-ft2-status"; EnsureDir(root);
        auto put = [](const std::wstring& p, const std::string& s) { WriteAll(p, reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
        const std::string mz = std::string("MZ") + std::string(200, 'x');
        const std::wstring good = root + L"\\good"; EnsureDir(good);
        put(good + L"\\theme.ini", "[redirections]\r\nC:\\Windows\\System32\\one.dll=one.dll\r\nC:\\Windows\\System32\\two.dll=two.dll\r\n");
        put(good + L"\\one.dll", mz); put(good + L"\\two.dll", mz);
        const int full0 = IconFullValidations();
        IconThemeStatus s = IconThemeQuickCheck(good);
        Check(s.checked && s.installed && s.modules == 2 && s.problem.empty() && s.preview.empty(), "icon status: a complete theme is installed (2 modules, no preview file)");
        Check(IconFullValidations() == full0, "icon status: the quick check never runs the full validation");
        ValidateIconThemeDir(good);
        Check(IconFullValidations() == full0 + 1, "icon status: the full validation (used by Apply) is counted");
        put(good + L"\\preview.png", "png"); s = IconThemeQuickCheck(good);
        Check(s.installed && s.preview == good + L"\\preview.png", "icon status: the pack's own preview.png is found");
        DeleteFileW((good + L"\\preview.png").c_str()); put(good + L"\\preview.bmp", "bmp"); s = IconThemeQuickCheck(good);
        Check(s.preview == good + L"\\preview.bmp", "icon status: preview.bmp is found too");
        DeleteFileW((good + L"\\two.dll").c_str()); s = IconThemeQuickCheck(good);
        Check(s.checked && !s.installed && s.problem.find(L"two.dll") != std::wstring::npos, "icon status: a missing module is named and the theme is not installed");
        put(good + L"\\two.dll", "MZ"); s = IconThemeQuickCheck(good);
        Check(!s.installed && s.problem.find(L"two.dll") != std::wstring::npos, "icon status: an empty module is refused");
        put(good + L"\\two.dll", std::string("PK") + std::string(200, 'x')); s = IconThemeQuickCheck(good);
        Check(!s.installed && s.problem.find(L"not a program file") != std::wstring::npos, "icon status: a module that is not a program file is refused");
        DeleteFileW((good + L"\\theme.ini").c_str()); s = IconThemeQuickCheck(good);
        Check(s.checked && !s.installed && !s.problem.empty(), "icon status: no theme.ini, not installed");
        s = IconThemeQuickCheck(root + L"\\absent");
        Check(s.checked && !s.installed && s.problem == L"not installed", "icon status: no folder, not installed");
        {   // the cache: unknown until looked at, then one status per known theme; refreshing twice at once is safe
            IconStatusCache cache;
            Check(!cache.Get(L"bigsur").checked && !cache.Loaded(), "icon status: before the first check a theme is 'not checked yet'");
            cache.RefreshNow();
            bool all = cache.Loaded();
            for (const auto& p : IconPacks()) all = all && cache.Get(p.id).checked;
            Check(all && cache.Generation() == 1, "icon status: the first check covers every known theme");
            std::atomic<int> notified{0};
            cache.RefreshAsync([&] { ++notified; }); cache.RefreshAsync([&] { ++notified; }); cache.Join();
            Check(cache.Generation() >= 2 && notified >= 1, "icon status: a refresh on a worker thread completes and notifies, also when asked twice at once");
        }
        for (const wchar_t* f : {L"\\good\\theme.ini", L"\\good\\one.dll", L"\\good\\two.dll", L"\\good\\preview.bmp", L"\\good\\preview.png"}) DeleteFileW((root + f).c_str());
        RemoveDirectoryW(good.c_str()); RemoveDirectoryW(root.c_str());
    }
    {   // IconStatusCache.RefreshAsync: deterministic interleavings (closure audit 2026-11-17)
        auto waitFor = [](const std::function<bool()>& c) { for (int i = 0; i < 5000 && !c(); ++i) Sleep(1); return c(); };
        {   // A. requests that arrive while a refresh is running are served by exactly one more refresh, on the same worker
            IconStatusCache c; std::atomic<int> runs{0}, active{0}, maxActive{0}; std::atomic<bool> gate{false};
            c.work = [&] { const int a = ++active; int m = maxActive.load(); while (a > m && !maxActive.compare_exchange_weak(m, a)) {} const int n = ++runs; if (n == 1) while (!gate.load()) Sleep(1); --active; };
            c.RefreshAsync(); const bool started = waitFor([&] { return runs.load() == 1; });
            c.RefreshAsync(); c.RefreshAsync();   // two requests while the first run is blocked
            gate = true; c.Join();
            Check(started && runs == 2 && maxActive == 1 && c.workersStarted == 1, "icon status cache: two requests during a running refresh are served by one more refresh on the same worker (" + std::to_string(runs.load()) + " runs, " + std::to_string(c.workersStarted.load()) + " worker, never more than " + std::to_string(maxActive.load()) + " at once)");
        }
        {   // B. a request in the instant the worker is finishing is not lost (it used to be: the flag was cleared after the last check)
            IconStatusCache c; std::atomic<int> runs{0}; bool fired = false;
            c.work = [&] { ++runs; };
            c.beforeIdle = [&] { if (!fired) { fired = true; c.RefreshAsync(); } };
            c.RefreshAsync(); c.Join();
            Check(runs == 2 && c.workersStarted == 1, "icon status cache: a request arriving while the worker finishes is served (" + std::to_string(runs.load()) + " runs on " + std::to_string(c.workersStarted.load()) + " worker)");
            c.RefreshAsync(); c.Join();
            Check(runs == 3 && c.workersStarted == 2, "icon status cache: after that the cache is idle and the next request starts a fresh worker");
        }
        {   // C. the final cache reflects the latest filesystem state: a theme file removed during the completion window shows in the result
            wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
            std::wstring root = tb; while (!root.empty() && root.back() == L'\\') root.pop_back(); root += L"\\ankh-status-race"; EnsureDir(root);
            const std::wstring savedApp = AppDirOverride(); AppDirOverride() = root;
            const std::string id = WToUtf8(IconPacks().front().id); const std::wstring wid = IconPacks().front().id;
            const std::wstring td = root + L"\\IconThemes\\" + wid; EnsureDir(td);
            auto put = [](const std::wstring& p, const std::string& s) { WriteAll(p, reinterpret_cast<const uint8_t*>(s.data()), s.size()); };
            const std::string mz = std::string("MZ") + std::string(200, 'x');
            put(td + L"\\theme.ini", "[redirections]\r\nC:\\Windows\\System32\\one.dll=one.dll\r\nC:\\Windows\\System32\\two.dll=two.dll\r\n"); put(td + L"\\one.dll", mz); put(td + L"\\two.dll", mz);
            IconStatusCache c; bool fired = false;
            c.RefreshAsync(); c.Join();
            const IconThemeStatus before = c.Get(wid); const uint64_t g1 = c.Generation();
            c.beforeIdle = [&] { if (!fired) { fired = true; DeleteFileW((td + L"\\two.dll").c_str()); c.RefreshAsync(); } };
            c.RefreshAsync(); c.Join();
            const IconThemeStatus after = c.Get(wid);
            Check(before.installed && !after.installed && after.problem.find(L"two.dll") != std::wstring::npos && c.Generation() >= g1 + 2,
                  "icon status cache: the file removed while the refresh was finishing is in the final result (" + std::string(before.installed ? "installed" : "not installed") + " -> " + std::string(after.installed ? "installed" : "not installed") + ", generation " + std::to_string(g1) + " -> " + std::to_string(c.Generation()) + ")");
            DeleteFileW((td + L"\\theme.ini").c_str()); DeleteFileW((td + L"\\one.dll").c_str()); DeleteFileW((td + L"\\two.dll").c_str());
            RemoveDirectoryW(td.c_str()); RemoveDirectoryW((root + L"\\IconThemes").c_str()); RemoveDirectoryW(root.c_str());
            AppDirOverride() = savedApp; (void)id;
        }
        {   // D. a refresh that throws neither kills the program nor leaves the cache busy for ever
            IconStatusCache c; std::atomic<int> runs{0}, notified{0};
            c.work = [&] { if (++runs == 1) throw std::runtime_error("scan failed"); };
            c.RefreshAsync([&] { ++notified; }); c.Join();
            const bool afterFail = runs == 1 && notified == 1 && !c.Loaded();
            c.RefreshAsync([&] { ++notified; }); c.Join();
            Check(afterFail && runs == 2 && notified == 2 && c.workersStarted == 2, "icon status cache: a refresh that throws leaves the cache usable: the next request runs on a new worker (" + std::to_string(runs.load()) + " runs, " + std::to_string(c.workersStarted.load()) + " workers)");
        }
    }
    {   // RunCapture timeout (closure audit 2026-11-17): the timeout used to be checked only between reads, so a child that prints nothing was never cut off
        const std::wstring cmdExe = SysDir() + L"\\cmd.exe";
        std::vector<std::wstring> lines;
        const ULONGLONG t0 = GetTickCount64();
        const DWORD code = RunCapture(SelfPath(), L"--sleep-test", [&](const std::wstring& l) { lines.push_back(l); }, 1000);   // silent for 5 s
        const ULONGLONG took = GetTickCount64() - t0;
        Check(code == kRunTimedOut && took < 4500, "RunCapture: a silent child that runs longer than the timeout is cut off (" + std::to_string(took) + " ms for a 1000 ms timeout, result " + (code == kRunTimedOut ? std::string("timed out") : std::to_string(code)) + ")");
        lines.clear();
        const DWORD ok = RunCapture(cmdExe, L"/c echo first& echo second", [&](const std::wstring& l) { lines.push_back(l); }, 10000);
        Check(ok == 0 && lines.size() == 2 && lines[0] == L"first" && lines[1] == L"second", "RunCapture: output of a quick child is delivered line by line and the exit code is returned");
        lines.clear();
        const DWORD bad = RunCapture(cmdExe, L"/c echo one& exit /b 3", [&](const std::wstring& l) { lines.push_back(l); }, 10000);
        Check(bad == 3 && lines.size() == 1 && lines[0] == L"one", "RunCapture: a non-zero exit code is returned with the output");
    }
    {   // page_info.h: Error Logs, Hardware Information and Network Monitor helpers (2.5 rail pages)
        ErrLogEntry e;
        Check(ParseLogLine(L"[2026-11-17 09:05:01] ERROR Could not write C:\\x\\y.dll\r", e) && e.level == Lv::Err && e.date == L"2026-11-17" && e.time == L"09:05:01" && e.text == L"Could not write C:\\x\\y.dll", "log line: an ERROR entry parses (date, time, level, text; CR dropped)");
        Check(ParseLogLine(L"[2026-11-17 09:05:01] WARN  slow disk", e) && e.level == Lv::Warn && e.text == L"slow disk" && ParseLogLine(L"[2026-11-17 09:05:01] OK    done", e) && e.level == Lv::Ok && ParseLogLine(L"[2026-11-17 09:05:01] INFO  hi", e) && e.level == Lv::Info && ParseLogLine(L"[2026-11-17 09:05:01] INFO  ", e) && e.text.empty(),
              "log line: WARN, OK and INFO parse; an empty text is allowed");
        Check(!ParseLogLine(L"", e) && !ParseLogLine(L"not a log line at all, long enough to pass the size test", e) && !ParseLogLine(L"[2026-11-17 09:05:01] DEBUG x", e) && !ParseLogLine(L"[2026-1X-17 09:05:01] INFO  x", e) && !ParseLogLine(L"[2026-11-17 09:05:01]INFO  x", e) && !ParseLogLine(L"[2026-11-17 09:05:01] ERR", e),
              "log line: empty, foreign, unknown-tag, non-numeric or too short lines are refused");
        const std::wstring text = L"tial line cut by a tail read\r\n[2026-11-16 23:59:59] INFO  old info\r\n\r\n[2026-11-16 23:59:59] ERROR old error\r\n[2026-11-17 00:00:01] WARN  today warning\r\n[2026-11-17 00:00:02] OK    fine\r\n[2026-11-17 00:00:03] ERROR today error\r\n";
        const auto all = ParseLogText(text);
        Check(all.size() == 5 && all[0].text == L"old info" && all[4].text == L"today error", "log text: a cut first line and blank lines are skipped, order is the file's (5 entries)");
        const auto bothToday = FilterLogEntries(all, false, L"2026-11-17", 300), errAll = FilterLogEntries(all, true, L"", 300), cap = FilterLogEntries(all, false, L"", 2);
        Check(bothToday.size() == 2 && bothToday[0].text == L"today error" && bothToday[1].text == L"today warning" && errAll.size() == 2 && errAll[0].text == L"today error" && errAll[1].text == L"old error" && cap.size() == 2 && cap[0].text == L"today error" && FilterLogEntries(all, true, L"2026-11-18", 300).empty() && FilterLogEntries({}, false, L"", 300).empty(),
              "log filter: warnings and errors or errors only, one day or all, newest first, capped; nothing matching and an empty log give an empty list");
        const LogCounts lc = CountLogEntries(all, L"2026-11-17"), la = CountLogEntries(all, L"");
        Check(lc.err == 1 && lc.warn == 1 && lc.ok == 1 && lc.info == 0 && lc.Total() == 3 && la.Total() == 5 && la.info == 1 && la.err == 2, "log counts: per level, for one day and for all");
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-page-info"; EnsureDir(dir);
        const std::wstring lf = dir + L"\\ankhcore.log"; std::string big;
        for (int i = 0; i < 400; ++i) { char b[96]; snprintf(b, sizeof(b), "[2026-11-17 10:%02d:%02d] WARN  line number %03d\r\n", i / 60 % 60, i % 60, i); big += b; }
        WriteAll(lf, reinterpret_cast<const uint8_t*>(big.data()), big.size());
        std::wstring tail; uint64_t fsz = 0; bool cut = false;
        Check(ReadLogTail(lf, 100000, tail, fsz, cut) && !cut && fsz == big.size() && ParseLogText(tail).size() == 400, "log tail: a file smaller than the limit is read whole");
        Check(ReadLogTail(lf, 1000, tail, fsz, cut) && cut && fsz == big.size() && !ParseLogText(tail).empty() && ParseLogText(tail).size() < 400 && ParseLogText(tail).back().text == L"line number 399" && tail.rfind(L"[2026", 0) == 0,
              "log tail: a larger file is read from its end, the cut first line is dropped, the newest entry is intact");
        const std::wstring empty = dir + L"\\empty.log"; WriteAll(empty, reinterpret_cast<const uint8_t*>(""), 0);
        Check(ReadLogTail(empty, 1000, tail, fsz, cut) && tail.empty() && fsz == 0 && !cut && !ReadLogTail(dir + L"\\missing.log", 1000, tail, fsz, cut), "log tail: an empty file reads as empty; a missing file is an error, not an empty list");
        DeleteFileW(lf.c_str()); DeleteFileW(empty.c_str()); RemoveDirectoryW(dir.c_str());
        // Hardware Information
        const std::vector<HwRow> rows = {{L"System", L"Operating system", L"Windows 11"}, {L"System", L"Device", L"X"}, {L"Memory", L"Installed", L"16 GB"}};
        Check(HwSummaryText(rows) == L"== System ==\r\nOperating system: Windows 11\r\nDevice: X\r\n\r\n== Memory ==\r\nInstalled: 16 GB\r\n" && HwSummaryText({}).empty(), "hardware summary: one block per section in row order; nothing for no rows");
        Check(DriveText(L'C', 500ull << 30, 125ull << 30).find(L"C:") == 0 && DriveText(L'C', 500ull << 30, 125ull << 30).find(L"(75 % used)") != std::wstring::npos && DriveText(L'D', 0, 0) == L"D: size not known" && DriveText(L'E', 100, 500).find(L"(0 % used)") != std::wstring::npos && DriveText(0, 100, 50).find(L"?:") == 0,
              "hardware drive text: size, free and percent used; unknown size, free above total and no letter are handled");
        Check(TidyHwName(L"  Intel(R)  Core(TM) i7 \t CPU   @ 2.60GHz  ") == L"Intel(R) Core(TM) i7 CPU @ 2.60GHz" && TidyHwName(L"").empty() && TidyHwName(L"   ").empty(), "hardware name: padding and doubled spaces are removed");
        // Network Monitor
        NetAdapterView wifi; wifi.type = 71; wifi.present = true; wifi.up = true; wifi.linkBps = 866000000ull; wifi.signal = 72;
        NetAdapterView eth; eth.type = 6; eth.present = true; eth.up = false; eth.linkBps = 1000000000ull; eth.signal = -1;
        NetAdapterView gone; gone.type = 6; gone.present = false;
        Check(NetLinkText(0) == L"speed not reported" && NetLinkText(866000000ull) == L"866 Mbps" && NetLinkText(2500000000ull) == L"2.5 Gbps" && NetLinkText(100000000ull) == L"100 Mbps", "network link text: Mbps, Gbps and not reported");
        Check(std::wstring(NetStateText(wifi)) == L"Connected" && std::wstring(NetStateText(eth)) == L"Disconnected" && std::wstring(NetStateText(gone)) == L"Not present", "network state text: connected, disconnected, not present");
        Check(NetAdapterLine(wifi) == L"Wi-Fi \u00B7 866 Mbps \u00B7 signal 72 %" && NetAdapterLine(eth) == L"Ethernet" && NetAdapterLine(gone) == L"Ethernet", "network adapter line: speed and signal only while the link is up");
    }
    {   // FT-E01: the Main Window background is 70 % of the strength it had (0.38), one constant for both placements
        Check(std::fabs(kBrandBgOpacity - 0.266f) < 0.0005f && std::fabs(kBrandBgOpacity / kBrandBgWas - 0.70f) < 0.0005f && kBrandBgOpacity < kBrandBgWas, "background: 70 % of the previous strength (0.38 -> 0.266), softer than before");
    }
    {   // FT-X5: a file that is locked for a moment (scanner, indexer) is written after a short wait, not failed or deferred to a restart
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-ft-x5"; EnsureDir(dir);
        const std::wstring f = dir + L"\\a.bin";
        const std::string oldData = "old", newData = "new data";
        WriteAll(f, reinterpret_cast<const uint8_t*>(oldData.data()), oldData.size());
        HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // writers are refused while this is open
        Check(held != INVALID_HANDLE_VALUE && !WriteAll(f, reinterpret_cast<const uint8_t*>(newData.data()), newData.size()), "place file: the locked file really cannot be written (test setup)");
        std::thread release([&] { Sleep(450); CloseHandle(held); });
        const int r0 = PlaceFileRetries(); bool deferred = false;
        const bool ok = PlaceFile(f, reinterpret_cast<const uint8_t*>(newData.data()), newData.size(), deferred);
        release.join();
        Check(ok && !deferred && ReadText(f) == newData && PlaceFileRetries() > r0, "place file: a file locked for under a second is written after retries, not deferred to a restart, and the retries are counted");
        const std::wstring g = dir + L"\\fresh.bin"; bool d2 = false; const int r1 = PlaceFileRetries();
        Check(PlaceFile(g, reinterpret_cast<const uint8_t*>(newData.data()), newData.size(), d2) && !d2 && PlaceFileRetries() == r1 && ReadText(g) == newData, "place file: a new file is written at once, with no waiting");
        DeleteFileW(f.c_str()); DeleteFileW(g.c_str()); RemoveDirectoryW(dir.c_str());
    }
    {   // PlaceFile hardening: the live file is never written in place; every failure leaves the old content and no staged file behind
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-place-atomic"; EnsureDir(dir);
        const std::wstring f = dir + L"\\prog.bin", staged = f + L".ankh-new";
        const std::string oldData(3 * 1024 * 1024 + 17, 'o'), newData(3 * 1024 * 1024 + 29, 'n');   // spans several 1 MB write chunks
        auto reset = [&] { WriteAll(f, reinterpret_cast<const uint8_t*>(oldData.data()), oldData.size()); DeleteFileW(staged.c_str()); PlaceFileFaultSet() = PlaceFileFaults{}; };
        auto place = [&](bool& def, std::wstring& why, const std::function<bool(const std::wstring&)>& v = nullptr) { return PlaceFile(f, reinterpret_cast<const uint8_t*>(newData.data()), newData.size(), def, v, &why); };
        const auto sameAsNew = [&](const std::wstring& p) { return ReadText(p) == newData; };
        bool def = false; std::wstring why;
        reset();
        Check(place(def, why, sameAsNew) && !def && ReadText(f) == newData && !FileExists(staged), "place file: a good write is verified before it replaces the file and leaves no staged file");
        reset(); PlaceFileFaultSet().cutAfter = 1500000; def = false; why.clear(); const int hit0 = PlaceFileFaultsHit();
        Check(!place(def, why, sameAsNew) && why == L"write" && PlaceFileFaultsHit() > hit0 && ReadText(f) == oldData && !FileExists(staged), "place file: a write cut in the middle leaves the old file byte for byte and no staged file");
        reset(); PlaceFileFaultSet().cutAfter = 0; def = false; why.clear();
        Check(!place(def, why) && ReadText(f) == oldData && !FileExists(staged), "place file: a write that fails at once (full disk) leaves the old file intact");
        reset(); PlaceFileFaultSet().corrupt = true; def = false; why.clear();
        Check(!place(def, why, sameAsNew) && why == L"verify" && ReadText(f) == oldData && !FileExists(staged), "place file: a wrong hash is caught on the staged file, the old file stays, nothing is left behind");
        reset(); PlaceFileFaultSet().refuseReplace = true; def = false; why.clear(); const int r0 = PlaceFileRetries();
        Check(!place(def, why, sameAsNew) && why == L"replace" && !def && PlaceFileRetries() > r0 && ReadText(f) == oldData && !FileExists(staged), "place file: a swap-in that is refused keeps the old file, retries, does not queue a restart rename in the test build, leaves no staged file");
        reset(); PlaceFileFaultSet().cutAfter = 10; PlaceFileFaultSet().nameContains = L"some-other-file"; def = false; why.clear();
        Check(place(def, why, sameAsNew) && ReadText(f) == newData, "place file: a fault aimed at another file name does not touch this one");
        const std::wstring fresh = dir + L"\\fresh.bin"; DeleteFileW(fresh.c_str());
        reset(); PlaceFileFaultSet().cutAfter = 100; def = false; why.clear();
        Check(!PlaceFile(fresh, reinterpret_cast<const uint8_t*>(newData.data()), newData.size(), def, nullptr, &why) && !FileExists(fresh) && !FileExists(fresh + L".ankh-new"), "place file: a cut write of a new file creates neither the file nor a staged one");
        PlaceFileFaultSet() = PlaceFileFaults{};
        {   // B4: a caller that cannot accept "replaced at the next restart" (an optional component) gets a clean failure instead of a queued rename
            reset();
            HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // stays open for the whole call: the swap-in cannot succeed
            bool d4 = false; std::wstring why4; const int r4 = PlaceFileRetries();
            const bool placed = PlaceFile(f, reinterpret_cast<const uint8_t*>(newData.data()), newData.size(), d4, nullptr, &why4, /*allowDeferred*/ false);
            if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
            Check(held != INVALID_HANDLE_VALUE && !placed && !d4 && why4 == L"replace" && PlaceFileRetries() > r4 && ReadText(f) == oldData && !FileExists(staged),
                  "place file: with deferral not allowed, a target that stays locked fails with \"replace\", queues nothing, keeps the old file and leaves no staged file");
        }
        {   // B5: SetUserToken owns the handle: the one it replaces, and the one still held at the end, are closed
            HANDLE h1 = CreateEventW(nullptr, TRUE, FALSE, nullptr), h2 = CreateEventW(nullptr, TRUE, FALSE, nullptr); DWORD fl = 0;
            DWORD hc0 = 0, hc1 = 0, hc2 = 0, hc3 = 0;
            GetProcessHandleCount(GetCurrentProcess(), &hc0);
            const HANDLE before = UserTokenRef();
            SetUserToken(h1); SetUserToken(h1);                          // the same handle again closes nothing
            const bool h1Open = GetHandleInformation(h1, &fl) != 0;
            GetProcessHandleCount(GetCurrentProcess(), &hc1);
            SetUserToken(h2);                                            // replaces h1: h1 is closed (handle count drops by 1)
            GetProcessHandleCount(GetCurrentProcess(), &hc2);
            const bool h1Closed = (hc2 < hc1), h2Open = GetHandleInformation(h2, &fl) != 0 && UserTokenRef() == h2;
            { UserTokenGuard g; }                                        // the guard closes the one in use
            GetProcessHandleCount(GetCurrentProcess(), &hc3);
            const bool h2Closed = (hc3 < hc2) && UserTokenRef() == nullptr;
            Check(h1Open && h1Closed && h2Open && h2Closed && before == nullptr, "user token: SetUserToken closes the handle it replaces and the guard closes the last one (no leaked token handle)");
        }
        DeleteFileW(f.c_str()); DeleteFileW(staged.c_str()); DeleteFileW(fresh.c_str()); RemoveDirectoryW(dir.c_str());
    }
    {   // FT-D01: the Sidebar health decision (every way it can leave the desktop; nothing else is touched)
        SbHealthInput ok; ok.wantDesktop = true; ok.haveWindow = true; ok.hosted = true; ok.hostAvailable = true; ok.parentIsHost = true; ok.visible = true; ok.beneathIcons = true;
        Check(SbHealthDecide(ok).what == SbHealth::Healthy, "sidebar health: in order is healthy");
        SbHealthInput x = ok; x.wantDesktop = false; Check(SbHealthDecide(x).what == SbHealth::Healthy, "sidebar health: another layer is not checked");
        x = ok; x.haveWindow = false; Check(SbHealthDecide(x).what == SbHealth::Healthy, "sidebar health: no window is SbEnsure's job, not this check's");
        x = ok; x.busy = true; x.visible = false; Check(SbHealthDecide(x).what == SbHealth::Healthy, "sidebar health: closing, dragging or sliding is left alone");
        x = ok; x.visible = false; Check(SbHealthDecide(x).what == SbHealth::Reshow, "sidebar health: hidden -> show again");
        x = ok; x.beneathIcons = false; Check(SbHealthDecide(x).what == SbHealth::Restack, "sidebar health: above the desktop icons -> restack");
        x = ok; x.parentIsHost = false; Check(SbHealthDecide(x).what == SbHealth::Rejoin, "sidebar health: attached to another or stale layer -> rejoin");
        x = ok; x.hostAvailable = false; Check(SbHealthDecide(x).what == SbHealth::Waiting, "sidebar health: the layer is being rebuilt -> wait, do not fight it");
        x = ok; x.hosted = false; x.parentIsHost = false; Check(SbHealthDecide(x).what == SbHealth::Rejoin, "sidebar health: fell back to a window below all others and a layer is offered -> rejoin");
        x.hostAvailable = false; Check(SbHealthDecide(x).what == SbHealth::Waiting, "sidebar health: fallback and no layer offered -> wait");
        x = ok; x.visible = false; x.parentIsHost = false; Check(SbHealthDecide(x).what == SbHealth::Rejoin, "sidebar health: a wrong parent is fixed before the visibility (re-joining shows it)");
        Check(std::wstring(SbHealthDecide(ok).why).empty() && std::wstring(SbHealthName(SbHealth::Rejoin)).find(L"rejoin") != std::wstring::npos && !std::wstring(SbHealthDecide([&] { auto y = ok; y.visible = false; return y; }()).why).empty(), "sidebar health: every problem comes with a reason in words");
    }
    {   // FT-C01: the schema, read from what the Windhawk CLI reports; nothing invented
        const std::string show = R"({"initialSettings":[{"key":"radius","value":12,"name":"Corner radius","description":"Pixels."},{"key":"flag","value":true,"name":"Flag"},{"key":"scale","value":1.5,"name":"Scale"},
            {"key":"mode","value":"a","name":"Mode","options":[{"a":"Alpha"},{"b":"Beta"}]},{"key":"tintColor","value":"FF6B35","name":"Tint colour"},{"key":"label","value":"hi","name":"Label"},{"key":"label2","value":"ABCDEF","name":"Not a colour by name"},
            {"key":"excluded","value":[""],"name":"Excluded"},{"key":"grp","name":"Group","value":[{"key":"inner","value":3,"name":"Inner"},{"key":"deep","value":[{"key":"x","value":false,"name":"X"}],"name":"Deep"}]},
            {"key":"styles","value":[{"target":"a","styles":["b"]}],"name":"Styles"},{"key":"nothing","value":null,"name":"Null"}]})";
        JVal doc; std::vector<WhSettingDef> d; std::wstring err;
        Check(JsonParse(show, doc) && WhParseSchema(doc, d, err), "wh schema: a declared settings list is parsed");
        auto kind = [&](const char* path) { for (const auto& x : d) if (x.path == path) return x.kind; return WhKind::Group; };
        auto find = [&](const char* path) -> const WhSettingDef* { for (const auto& x : d) if (x.path == path) return &x; return nullptr; };
        Check(d.size() == 13 && kind("radius") == WhKind::Int && kind("flag") == WhKind::Bool && kind("scale") == WhKind::Real && kind("mode") == WhKind::Enum && kind("tintColor") == WhKind::Color &&
              kind("label") == WhKind::Text && kind("label2") == WhKind::Text && kind("excluded") == WhKind::List && kind("styles") == WhKind::List && !find("nothing"),
              "wh schema: kinds come from the declared default and options (int, bool, real, enum, colour only when the name says colour, text, list); a null default is not editable");
        Check(find("mode") && find("mode")->options.size() == 2 && find("mode")->options[1].value == "b" && find("mode")->options[1].label == "Beta" && find("radius")->description == "Pixels." && find("flag")->name == "Flag",
              "wh schema: options (value and label), descriptions and names are kept as declared");
        Check(find("grp") && find("grp")->kind == WhKind::Group && find("grp.inner") && find("grp.inner")->depth == 1 && find("grp.deep.x") && find("grp.deep.x")->depth == 2 && find("grp.deep.x")->kind == WhKind::Bool,
              "wh schema: groups nest and give the flat keys `settings get` prints (grp.inner, grp.deep.x)");
        Check(find("excluded")->listItems == 1 && find("styles")->kind == WhKind::List, "wh schema: lists, scalar or structured, are reported as lists (shown, kept, not edited)");
        JVal none; JsonParse(R"({"id":"m"})", none); std::vector<WhSettingDef> d2; std::wstring e2;
        Check(WhParseSchema(none, d2, e2) && d2.empty() && !e2.empty(), "wh schema: a mod without settings is normal, and says so");
        JVal bad; JsonParse(R"({"initialSettings":5})", bad);
        Check(!WhParseSchema(bad, d2, e2), "wh schema: a damaged settings list is an error, not an empty editor");
        JVal flatDoc; WhFlat flat;
        Check(JsonParse(R"({"settings":{"radius":12,"flag":1,"grp.inner":3,"excluded[0]":"x"}})", flatDoc) && WhParseFlat(flatDoc, flat, err) && flat.size() == 4 && flat[3].first == "excluded[0]" && WhFlatFind(flat, "flag") && !WhFlatFind(flat, "none"),
              "wh settings: the flat settings keep the mod's own order, including list items");
        Check(WhValueEquals(WhMakeNum(1), [] { JVal v; v.t = JVal::Bool; v.b = true; return v; }()) && !WhValueEquals(WhMakeNum(1), WhMakeNum(2)) && WhValueEquals(WhMakeStr("a"), WhMakeStr("a")) && !WhValueEquals(WhMakeStr("1"), WhMakeNum(1)),
              "wh values: a boolean is the same as 1 or 0; a string is not a number");
        JVal v;
        Check(WhParseInput(*find("radius"), "20", v, err) && v.n == 20 && !WhParseInput(*find("radius"), "2.5", v, err) && !WhParseInput(*find("radius"), "abc", v, err) && !WhParseInput(*find("radius"), "", v, err) && !WhParseInput(*find("radius"), "12px", v, err),
              "wh input: a whole number is accepted; a fraction, text, nothing, or trailing text is refused");
        Check(WhParseInput(*find("scale"), "2.25", v, err) && v.n == 2.25 && WhParseInput(*find("flag"), "off", v, err) && v.n == 0 && WhParseInput(*find("flag"), "1", v, err) && v.n == 1 && !WhParseInput(*find("flag"), "maybe", v, err),
              "wh input: real numbers and on/off are parsed; anything else is refused");
        Check(WhParseInput(*find("mode"), "b", v, err) && v.s == "b" && !WhParseInput(*find("mode"), "c", v, err), "wh input: only a declared option is accepted");
        Check(WhParseInput(*find("tintColor"), "#00bfff", v, err) && v.s == "00bfff" && WhParseInput(*find("tintColor"), "00BFFFAA", v, err) && !WhParseInput(*find("tintColor"), "12345", v, err) && !WhParseInput(*find("tintColor"), "GGGGGG", v, err),
              "wh input: a colour is 6 or 8 hex digits, with or without #");
        Check(!WhParseInput(*find("excluded"), "x", v, err), "wh input: a list cannot be edited here");
        const WhFlat cur = {{"radius", WhMakeNum(12)}, {"label", WhMakeStr("he said \"hi\"\\ \n\xC3\xA9")}, {"flag", WhMakeNum(1)}, {"excluded[0]", WhMakeStr("")}};
        WhFlat merged; Check(WhMerge(cur, {{"radius", WhMakeNum(20)}}, merged, err) && merged.size() == 4 && merged[0].first == "radius" && merged[0].second.n == 20 && merged[3].first == "excluded[0]" && merged[1].second.s == cur[1].second.s,
                             "wh merge: only the named key changes; the order, the list item and every other value stay");
        Check(!WhMerge(cur, {{"nope", WhMakeNum(1)}}, merged, err) && err.find(L"nope") != std::wstring::npos, "wh merge: a key the mod does not have is refused, never invented");
        JVal round; const std::string js = WhFlatToJson(cur); WhFlat back; JVal rd;
        Check(JsonParse(js, round) && round.Is(JVal::Obj) && round.keys.size() == 4 && round.Get("label")->s == cur[1].second.s && round.Get("radius")->n == 12, "wh json: the settings are written as JSON that reads back to the same values (quotes, backslash, newline, accents)");
    }
    {   // FT-C02: staged changes and the Apply transaction
        WhStage st; Check(st.Empty() && st.Count() == 0, "wh stage: starts empty");
        st.StageEnabled("a", true, false); bool w = false;
        Check(st.EnabledPending("a", w) && w && st.Count() == 1, "wh stage: a switch is only recorded as pending");
        st.StageEnabled("a", false, false);
        Check(!st.EnabledPending("a", w) && st.Empty(), "wh stage: switching back to the applied state cancels the request");
        st.StageSetting("a", "k", WhMakeNum(5), WhMakeNum(1));
        Check(st.SettingPending("a", "k") && st.SettingPending("a", "k")->n == 5 && st.SettingCount("a") == 1 && !st.SettingPending("a", "other") && !st.SettingPending("b", "k"), "wh stage: a setting is recorded per mod and key");
        st.StageSetting("a", "k", WhMakeNum(1), WhMakeNum(1));
        Check(st.Empty(), "wh stage: setting the applied value again cancels it");
        st.StageEnabled("z", true, false); st.StageEnabled("b", false, true); st.StageSetting("m", "k1", WhMakeNum(1), WhMakeNum(0)); st.StageSetting("m", "k2", WhMakeStr("x"), WhMakeStr("y"));
        const auto plan = st.Plan();
        Check(plan.size() == 3 && plan[0].kind == WhOp::SetEnabled && plan[1].kind == WhOp::SetEnabled && plan[2].kind == WhOp::SetSettings && plan[2].changes.size() == 2 && st.Count() == 4,
              "wh stage: the plan turns mods on or off first, then writes each mod's settings once");
        std::vector<std::string> log; bool explorerMod = false;
        WhExec ex;
        ex.setEnabled = [&](const std::string& id, bool on, std::wstring& e) { log.push_back((on ? "on:" : "off:") + id); if (id == "b") { e = L"Windhawk reports the mod is still on"; return false; } return true; };
        ex.writeSettings = [&](const std::string& id, const std::vector<std::pair<std::string, JVal>>& ch, std::wstring&) { log.push_back("set:" + id + ":" + std::to_string(ch.size())); return true; };
        ex.reachesExplorer = [&](const std::string& id) { return id == "z" || id == "m" ? (explorerMod = true) : false; };
        const WhApplyResult r = WhApply(st, ex);
        Check(r.results.size() == 3 && r.Ok() == 2 && r.Failed() == 1 && !r.AllOk() && log.size() == 3 && log[2] == "set:m:2", "wh apply: every pending operation runs, in plan order, and the result is counted per operation");
        Check(st.Count() == 1 && [&] { bool wb = true; return st.EnabledPending("b", wb) && !wb; }(), "wh apply: only the failed operation stays pending, for a retry; the verified ones left the stage");
        const std::wstring sum = WhSummary(r, [](const std::string& s) { return Utf8ToW(s); });
        Check(sum.find(L"2 of 3") != std::wstring::npos && sum.find(L"b (turn off): Windhawk reports the mod is still on") != std::wstring::npos && sum.find(L"stay pending") != std::wstring::npos && sum.find(L"Explorer") != std::wstring::npos,
              "wh apply: the summary names the failed operation and why, says it stays pending, and offers the Explorer restart once");
        Check(r.offerExplorerRestart && explorerMod, "wh apply: one Explorer offer for the whole transaction, however many mods run in Explorer");
        WhStage st2; st2.StageEnabled("q", true, false);
        WhExec none;   // no executor functions: nothing can be applied, and nothing is reported as done
        const WhApplyResult r2 = WhApply(st2, none);
        Check(r2.Failed() == 1 && !r2.offerExplorerRestart && st2.Count() == 1 && !r2.results[0].error.empty(), "wh apply: an operation that cannot run is a named failure and stays pending, never a silent success");
        WhStage st3; WhExec ok3; ok3.setEnabled = [](const std::string&, bool, std::wstring&) { return true; }; ok3.reachesExplorer = [](const std::string&) { return false; };
        st3.StageEnabled("q", true, false);
        const WhApplyResult r3 = WhApply(st3, ok3);
        Check(r3.AllOk() && !r3.offerExplorerRestart && st3.Empty() && WhSummary(r3, [](const std::string& s) { return Utf8ToW(s); }).find(L"Explorer") == std::wstring::npos, "wh apply: mods that do not run in Explorer cause no Explorer offer");
        st.CancelMod("b"); Check(st.Empty(), "wh stage: one mod's pending changes can be cancelled");
        st.StageEnabled("c", true, false); st.StageSetting("d", "k", WhMakeNum(1), WhMakeNum(0)); st.DiscardAll(); Check(st.Empty(), "wh stage: everything pending can be discarded");
        WhApplyResult empty; Check(WhSummary(empty, [](const std::string& s) { return Utf8ToW(s); }) == L"Nothing was pending.", "wh apply: nothing pending says so");
        JVal sd;
        JsonParse(R"({"config":{"include":["*"],"exclude":["dwm.exe"]}})", sd); Check(WhReachesExplorer(sd), "wh explorer: included by * and not excluded runs in Explorer");
        JsonParse(R"({"config":{"include":["*"],"exclude":["explorer.exe"]}})", sd); Check(!WhReachesExplorer(sd), "wh explorer: excluded explicitly does not");
        JsonParse(R"({"config":{"include":["Explorer.EXE"]}})", sd); Check(WhReachesExplorer(sd), "wh explorer: named, any case");
        JsonParse(R"({"config":{"include":["chrome.exe"]}})", sd); Check(!WhReachesExplorer(sd), "wh explorer: another program only does not");
        JsonParse(R"({"metadata":{"include":["windhawk.exe"]}})", sd); Check(!WhReachesExplorer(sd), "wh explorer: the mouse-trail style mod that runs only in windhawk.exe does not (metadata used when there is no config)");
    }
    {   // FT-RH: Recovery History: one operation is one row; repeated events are not recorded twice; different operations stay different
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-ft-rh"; EnsureDir(dir);
        const std::wstring jf = dir + L"\\journal.txt"; DeleteFileW(jf.c_str());
        JournalPathOverride() = jf; JournalCache().ok = false;
        auto rd = [&] { return ReadText(jf); };
        auto reg = [](int i) { return std::vector<std::wstring>{L"HKCU", L"Control Panel\\Cursors", L"Role" + std::to_wstring(i), L"1", L"2", L"2500"}; };
        {   // 1. one operation writing 18 values is one row
            JournalOpScope sc(L"Mouse cursors: X");
            for (int i = 0; i < 18; ++i) JournalAdd(L"cursors", L"REG", L"Mouse cursors: X", reg(i));
        }
        auto all = JournalLoad(); JournalView v = JournalBuildView(all);
        Check(all.size() == 18 && v.ops.size() == 1 && v.ops[0].ids.size() == 18 && v.ops[0].exact && v.ops[0].key == all[0].id && all[17].op == all[0].id && v.entries == 18 && v.activeEntries == 18 && all[5].f.size() == 6 && all[5].f[2] == L"Role5",
              "journal view: one operation with 18 entries is ONE row, its entries keep their 6 fields");
        Check(Utf8ToW(rd()).find(L"~op=1") != std::wstring::npos && Split(Utf8ToW(rd()), L'\n').size() >= 18, "journal file: the operation id is a trailing field of each line");
        {   // 2. the same event reported twice in one operation (repeated callback, retry): recorded once
            JournalOpScope sc(L"Tweak: T");
            const int a = JournalAdd(L"tweak:t", L"REG", L"Tweak: T", reg(1)), b = JournalAdd(L"tweak:t", L"REG", L"Tweak: T", reg(1)), c2 = JournalAdd(L"tweak:t", L"REG", L"Tweak: T", reg(2));
            Check(a == b && c2 == a + 1, "journal: a repeat of the same data inside one operation returns the first entry's id and records nothing; other data is recorded");
        }
        all = JournalLoad();
        Check(all.size() == 20 && all[18].id == 19 && all[19].id == 20 && all[18].op == 19 && all[19].op == 19, "journal: the repeat left 2 entries for that operation (20 in total), ids continue without a gap");
        {   // 3. two different operations with the same name AND the same data: two rows, nothing merged
            { JournalOpScope s1(L"Same name"); JournalAdd(L"g", L"REG", L"Same name", reg(7)); }
            { JournalOpScope s2(L"Same name"); JournalAdd(L"g", L"REG", L"Same name", reg(7)); }
        }
        all = JournalLoad(); v = JournalBuildView(all);
        Check(all.size() == 22 && v.ops.size() == 4 && v.ops[0].title == L"Same name" && v.ops[1].title == L"Same name" && v.ops[0].key != v.ops[1].key && v.ops[0].ids.size() == 1 && v.ops[1].ids.size() == 1,
              "journal view: two different operations with the same name and data stay two rows (identity is the operation, not the text)");
        const int x1 = JournalAdd(L"g", L"REG", L"Outside", reg(9)), x2 = JournalAdd(L"g", L"REG", L"Outside", reg(9));
        all = JournalLoad(); v = JournalBuildView(all);
        Check(x2 == x1 + 1 && all.size() == 24 && v.ops.size() == 6, "journal: entries added outside an operation scope are single operations and are never merged or dropped");
        Check(v.ops[0].key == x2 && v.ops[1].key == x1 && v.ops.back().title == L"Mouse cursors: X", "journal view: newest operation first, the oldest last");
        {   // 4. a nested scope on the same thread belongs to its own operation and the outer one continues afterwards
            JournalOpScope outer(L"Outer"); const int o1 = JournalAdd(L"g", L"REG", L"Outer", reg(31));
            { JournalOpScope inner(L"Inner"); JournalAdd(L"g", L"REG", L"Inner", reg(32)); }
            const int o2 = JournalAdd(L"g", L"REG", L"Outer", reg(33));
            all = JournalLoad(); int oOuter1 = 0, oOuter2 = 0, oInner = 0; for (const auto& e : all) { if (e.id == o1) oOuter1 = e.op; if (e.id == o2) oOuter2 = e.op; if (e.id == o1 + 1) oInner = e.op; }
            Check(oOuter1 == o1 && oOuter2 == o1 && oInner == o1 + 1, "journal: a nested scope is its own operation; the outer operation continues after it");
        }
        // 5. entries written before FT-RH (no op id): the file format is kept, and they are grouped by the documented rule only
        const std::wstring legacy = dir + L"\\legacy.txt";
        std::string lg;
        auto line = [&](int id, const char* time, const char* title) { lg += std::to_string(id) + "\t" + time + "\tcursors\tREG\tA\t" + title + "\tHKCU\tControl Panel\\Cursors\tRole" + std::to_string(id) + "\t1\t2\t25\r\n"; };
        line(1, "2026-11-05 00:06:50", "Mouse cursors: P"); line(2, "2026-11-05 00:06:50", "Mouse cursors: P"); line(3, "2026-11-05 00:06:51", "Mouse cursors: P");   // one operation across a second boundary
        line(4, "2026-11-05 00:07:30", "Mouse cursors: P"); line(5, "2026-11-05 00:07:30", "Mouse cursors: P");                                                    // the same name 40 s later: another operation
        line(6, "2026-11-05 00:07:30", "Mouse cursors: Q");                                                                                                       // same second, other title: another operation
        lg += "8\t2026-11-05 00:07:31\tcursors\tREG\tA\tMouse cursors: Q\tHKCU\tK\tN\t1\t2\t25\r\n";                                                           // id 7 missing: not consecutive: another operation
        WriteAll(legacy, reinterpret_cast<const uint8_t*>(lg.data()), lg.size());
        JournalPathOverride() = legacy; JournalCache().ok = false;
        auto la = JournalLoad(); JournalView lv = JournalBuildView(la);
        Check(la.size() == 7 && la[0].op == 0 && la[0].f.size() == 6 && lv.ops.size() == 4 && !lv.ops[3].exact && lv.ops[3].ids.size() == 3 && lv.ops[2].ids.size() == 2 && lv.ops[1].ids.size() == 1 && lv.ops[0].ids.size() == 1,
              "journal view: older entries without an operation id are grouped by consecutive ids, same group and title, at most 2 s apart (3 + 2 + 1 + 1 entries here); nothing else is merged");
        JournalSave(la);
        Check(ReadText(legacy) == lg, "journal file: loading and saving older entries gives back exactly the same text (no migration, no added fields)");
        const int nid = JournalAdd(L"cursors", L"REG", L"Mouse cursors: P", reg(40));
        const std::string after = ReadText(legacy);
        Check(nid == 9 && after.compare(0, lg.size(), lg) == 0 && after.size() > lg.size() && after.find("~op=9") != std::string::npos, "journal file: a new entry is appended after the older ones, which stay byte for byte as they were");
        // 6. the file changed behind the cache (another process or an undo): the next add reads the real last id
        { const std::string extra = "20\t2026-11-06 00:00:00\tg\tREG\tA\tExternal\tHKCU\tK\tN\t1\t2\t25\r\n"; HANDLE h = CreateFileW(legacy.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); DWORD n = 0; if (h != INVALID_HANDLE_VALUE) { WriteFile(h, extra.data(), static_cast<DWORD>(extra.size()), &n, nullptr); CloseHandle(h); } }
        Check(JournalAdd(L"g", L"REG", L"After external", reg(41)) == 21, "journal: a journal changed by another process is re-read, ids stay unique");
        // 7. a large journal: the view is built once and recording stays linear
        {
            std::vector<JEntry> big;
            for (int i = 1; i <= 20000; ++i) { JEntry e; e.id = i; e.op = (i - 1) / 20 + 1 > 0 ? ((i - 1) / 20) * 20 + 1 : 1; e.time = L"2026-11-05 00:06:50"; e.group = L"cursors"; e.kind = L"REG"; e.state = L"A"; e.title = L"Mouse cursors: S" + std::to_wstring((i - 1) / 20 % 7); e.f = {L"HKCU", L"K", L"R" + std::to_wstring(i), L"1", L"2", L"25"}; big.push_back(e); }
            const auto t0 = GetTickCount64(); const JournalView bv = JournalBuildView(big); const auto ms = GetTickCount64() - t0;
            Check(bv.ops.size() == 1000 && bv.entries == 20000 && bv.activeEntries == 20000 && bv.ops[0].ids.size() == 20 && ms < 1000, "journal view: 20,000 entries become 1,000 rows in " + std::to_string(ms) + " ms (once per refresh, not per paint)");
            const std::wstring bf = dir + L"\\big.txt"; DeleteFileW(bf.c_str()); JournalPathOverride() = bf; JournalCache().ok = false;
            const auto a0 = GetTickCount64(); for (int i = 0; i < 500; ++i) JournalAdd(L"g", L"REG", L"B", reg(1000 + i)); const auto first = GetTickCount64() - a0;
            for (int i = 0; i < 2500; ++i) JournalAdd(L"g", L"REG", L"B", reg(2000 + i));
            const auto b0 = GetTickCount64(); for (int i = 0; i < 500; ++i) JournalAdd(L"g", L"REG", L"B", reg(5000 + i)); const auto last = GetTickCount64() - b0;
            Check(JournalLoad().size() == 3500 && last <= first * 4 + 400, "journal: appending stays linear: entries 3001-3500 took " + std::to_string(last) + " ms against " + std::to_string(first) + " ms for entries 1-500 (it was a full rewrite per entry)");
            DeleteFileW(bf.c_str());
        }
        JournalPathOverride().clear(); JournalCache().ok = false;
        DeleteFileW(jf.c_str()); DeleteFileW(legacy.c_str()); RemoveDirectoryW(dir.c_str());
    }
    {   // Journal concurrency (closure audit 2026-11-17): threads of this process, and a second writer, against JournalUndoEx
        wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
        std::wstring dir = tb; while (!dir.empty() && dir.back() == L'\\') dir.pop_back(); dir += L"\\ankh-journal-conc"; EnsureDir(dir);
        const std::wstring jf = dir + L"\\journal.txt"; DeleteFileW(jf.c_str());
        JournalPathOverride() = jf; JournalCache().ok = false;
        std::atomic<int> handled{0};
        UndoHandlers()[L"TESTK"] = [&](const JEntry&) { ++handled; return true; };
        for (int i = 0; i < 100; ++i) JournalAdd(L"gU", L"TESTK", L"U", {std::to_wstring(i)});
        std::atomic<bool> stop{false}; std::atomic<int> rounds{0};
        auto adder = [&](int k) { for (int i = 0; i < 200; ++i) JournalAdd(L"g" + std::to_wstring(k), L"TESTK", L"A" + std::to_wstring(k), {std::to_wstring(i)}); };
        std::thread a1(adder, 1), a2(adder, 2), a3(adder, 3);
        std::thread und([&] { while (!stop.load() || rounds.load() < 3) { JournalUndoEx([](const JEntry& e) { return e.group == L"gU"; }); ++rounds; Sleep(1); } });
        a1.join(); a2.join(); a3.join(); stop = true; und.join();
        JournalUndoEx([](const JEntry& e) { return e.group == L"gU"; });
        const auto all = JournalLoad();
        bool ids = true, states = true, unique = true; int gu = 0, others = 0; std::set<int> seen;
        for (size_t i = 0; i < all.size(); ++i) {
            if (i && all[i].id <= all[i - 1].id) ids = false;
            if (!seen.insert(all[i].id).second) unique = false;
            if (all[i].group == L"gU") { ++gu; if (all[i].state != L"R") states = false; } else { ++others; if (all[i].state != L"A") states = false; }
        }
        Check(all.size() == 700 && gu == 100 && others == 600 && unique && ids && states && handled == 100,
              "journal concurrency: 3 writer threads and a repeated undo in one process lose no entry (700), keep ids unique and ascending in file order, undo exactly the 100 picked entries once each (" + std::to_string(all.size()) + " entries, " + std::to_string(handled.load()) + " undone, " + std::to_string(rounds.load()) + " undo rounds)");
        {   // a second writer (another program or instance) appends while an undo is in progress: its entry must survive the undo's rewrite
            DeleteFileW(jf.c_str()); JournalCache().ok = false;
            const int x = JournalAdd(L"gX", L"TESTK2", L"X", {L"1"}); JournalAdd(L"gY", L"TESTK2", L"Y", {L"2"});
            UndoHandlers()[L"TESTK2"] = [&](const JEntry& e) {
                if (e.group == L"gX") {   // the foreign append happens in the middle of the undo
                    HANDLE h = CreateFileW(jf.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    const std::string line = "9001\t2026-11-17 00:00:00\tforeign\tTESTK2\tA\tForeign\t1\r\n"; DWORD n = 0;
                    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &n, nullptr); CloseHandle(h); }
                }
                return true;
            };
            const UndoCount uc = JournalUndoEx([](const JEntry& e) { return e.group == L"gX"; });
            const auto after = JournalLoad(); bool foreign = false, xr = false, yA = false; for (const auto& e : after) { foreign |= e.id == 9001 && e.group == L"foreign" && e.state == L"A"; xr |= e.id == x && e.state == L"R"; yA |= e.group == L"gY" && e.state == L"A"; }
            Check(uc.Success() && xr && yA && foreign && after.size() == 3, "journal concurrency: an entry appended by another writer in the middle of an undo is not lost by the undo's rewrite (" + std::to_string(after.size()) + " of 3 entries)");
            const int nid = JournalAdd(L"gZ", L"TESTK2", L"Z", {L"3"});
            Check(nid > 9001, "journal concurrency: the next id continues after the foreign entry (" + std::to_string(nid) + ")");
        }
        UndoHandlers().erase(L"TESTK"); UndoHandlers().erase(L"TESTK2");
        JournalPathOverride().clear(); JournalCache().ok = false;
        DeleteFileW(jf.c_str()); RemoveDirectoryW(dir.c_str());
    }
    {   // FT-TOAST: the queue: order, timing, one toast per operation, bounds, geometry
        ToastQueue q;
        Check(q.Add(L"first", 1, 0, 1000) && q.Visible().size() == 1 && q.Visible()[0].text == L"first", "toast queue: a toast shows at once");
        for (int i = 2; i <= 5; ++i) q.Add(L"n" + std::to_wstring(i), 1, 100 + i, 1000);
        Check(q.Visible().size() == 3 && q.WaitingCount() == 2 && q.Visible()[0].text == L"first" && q.Visible()[1].text == L"n2" && q.Visible()[2].text == L"n3", "toast queue: five in quick succession: 3 shown in arrival order, 2 waiting");
        Check(!q.Tick(1000 + 3499) && q.Visible().size() == 3, "toast queue: nothing expires before its time (short text: 3.5 s)");
        Check(q.Tick(1000 + 3500) && q.Visible().size() == 2 && q.Visible()[0].text == L"n4" && q.Visible()[0].at == 4500 && q.WaitingCount() == 0, "toast queue: at 3.5 s the three go and the two that waited come in, with their own full time");
        Check(q.Tick(4500 + 3500) && q.Empty(), "toast queue: and they go after theirs");
        ToastQueue k;
        Check(k.Add(L"Done: Restore previous", 2, 7, 0) && !k.Add(L"Done: Restore previous", 2, 7, 10) && k.Visible().size() == 1, "toast queue: the same report for the same operation again adds nothing");
        Check(k.Add(L"Failed: Restore previous", 3, 7, 20) && k.Visible().size() == 1 && k.Visible()[0].text == L"Failed: Restore previous" && k.Visible()[0].at == 20, "toast queue: a changed report for the same operation replaces it (still one toast)");
        k.Add(L"Done: Apply", 2, 8, 30); k.Add(L"Done: Apply", 2, 9, 30);
        Check(k.Visible().size() == 3 && k.Visible()[1].text == L"Done: Apply" && k.Visible()[2].text == L"Done: Apply", "toast queue: two different operations with the same text are two toasts (identity is the operation, not the words)");
        k.Add(L"waiting one", 2, 10, 30); Check(k.WaitingCount() == 1 && !k.Add(L"waiting one", 2, 10, 31) && k.Add(L"waiting two", 2, 10, 32) == false && k.WaitingCount() == 1, "toast queue: the same operation reported while it waits replaces its waiting toast");
        ToastQueue f; for (int i = 0; i < 100; ++i) f.Add(L"flood " + std::to_wstring(i), 1, 0, 5);
        Check(f.Visible().size() == 3 && f.WaitingCount() == 12 && f.Visible()[0].text == L"flood 0", "toast queue: a burst of 100 keeps 3 shown and at most 12 waiting");
        f.Tick(5 + 9000 * 20); Check(f.Visible().size() == 3 && f.Visible()[0].text == L"flood 88" && f.WaitingCount() == 9, "toast queue: of a flood the oldest waiting ones were dropped; the newest 12 are the ones that get their turn");
        ToastQueue d; d.Add(L"a", 1, 0, 0); d.Add(L"b", 1, 0, 0); d.Add(L"c", 1, 0, 0); d.Add(L"d", 1, 0, 0);
        const uint64_t idA = d.Visible()[0].id;
        Check(d.Dismiss(idA, 100) && d.Visible().size() == 3 && d.Visible().back().text == L"d" && d.WaitingCount() == 0 && !d.Dismiss(idA, 100), "toast queue: a click dismisses one toast and lets the next waiting one in");
        Check(ToastLifetimeMs(L"") == 3500 && ToastLifetimeMs(std::wstring(100, L'x')) == 7500 && ToastLifetimeMs(std::wstring(1000, L'x')) == 9000 && ToastLifetimeMs(L"Done: Apply cursor scheme AEON") < ToastLifetimeMs(std::wstring(150, L'x')),
              "toast queue: lifetime grows with the text, 3.5 s minimum, 9 s maximum");
        ToastQueue n; Check(n.NextDeadlineMs(0) == 0, "toast queue: no deadline when nothing shows");
        n.Add(L"x", 1, 0, 1000); n.Add(std::wstring(300, L'y'), 1, 0, 1200);
        Check(n.NextDeadlineMs(1000) == 3500 && n.NextDeadlineMs(4400) == 100 && n.NextDeadlineMs(9999) == 1, "toast queue: the one timer is set for the nearest expiry");
        Check(n.Animating(1100) && !n.Animating(1500), "toast queue: only the first 260 ms of a toast animate");
        size_t fit = 0;
        const auto st = ToastStack({50, 80, 60}, 100, 400, 8, fit);
        Check(fit == 3 && st[2].top + st[2].height == 400 && st[1].top + st[1].height + 8 == st[2].top && st[0].top + st[0].height + 8 == st[1].top && st[0].top >= 100, "toast stack: bottom up with a fixed 8 px gap, newest at the bottom, nothing overlaps");
        const auto st2 = ToastStack({50, 80, 60}, 300, 400, 8, fit);
        Check(fit == 1 && !st2[0].shown && !st2[1].shown && st2[2].shown && st2[2].top >= 300, "toast stack: in a small area only the newest that fit are placed, none outside the area");
        const auto st3 = ToastStack({50}, 100, 120, 8, fit);
        Check(fit == 0 && !st3[0].shown, "toast stack: a toast taller than the area is not placed outside it");
        Check(ToastEntry(100, 100, 220) == 0.f && ToastEntry(320, 100, 220) == 1.f && ToastEntry(210, 100, 220) > 0.5f && ToastEntry(150, 100, 220) < ToastEntry(200, 100, 220), "toast entry: 0 at the start, 1 after 220 ms, rising in between (no animator entry per toast)");
    }
    {   // FT-A02: the per-card Apply button and note
        IconCardInput in; in.checked = true; in.installed = true; in.windhawk = true;
        IconCardView v = IconCardState(in);
        Check(v.label == L"Apply" && v.enabled && v.primary && v.note.empty(), "icon card: an installed, inactive theme can be applied");
        IconCardInput x = in; x.checked = false; v = IconCardState(x);
        Check(!v.enabled && v.note == L"Checking\x2026", "icon card: before the installed state is known the button is off and says so");
        x = in; x.installed = false; v = IconCardState(x);
        Check(!v.enabled && v.note == L"Theme not installed" && v.noteKind == 3, "icon card: a theme that is not installed cannot be applied");
        x = in; x.windhawk = false; v = IconCardState(x);
        Check(!v.enabled && v.note == L"Windhawk is not installed", "icon card: without Windhawk Apply is off, and the reason is given");
        x = in; x.busy = true; v = IconCardState(x);
        Check(!v.enabled && v.note == L"Busy\x2026", "icon card: while another operation runs every Apply is off");
        x = in; x.busy = true; x.applyingHere = true; v = IconCardState(x);
        Check(!v.enabled && v.label == L"Applying\x2026" && !v.primary, "icon card: the card being applied shows Applying (its progress is drawn in the card)");
        x = in; x.active = true; v = IconCardState(x);
        Check(!v.enabled && v.label == L"Applied" && !v.primary, "icon card: the active theme shows Applied and cannot be applied again");
        x.restartPending = true; v = IconCardState(x);
        Check(v.label == L"Restart needed" && !v.enabled, "icon card: a 2.2 file swap that still needs a restart is said on the active card");
        x = in; x.lastResult = 2; v = IconCardState(x);
        Check(v.enabled && v.noteKind == 2 && v.note.find(L"Failed") == 0, "icon card: a failed apply is shown on its card and Apply stays available for a retry");
        x = in; x.lastResult = 2; x.busy = true; v = IconCardState(x);
        Check(v.noteKind != 2, "icon card: the failure note is not shown while something else is running");
    }
    {   // Sidebar lifecycle: what ended the window, bounded recovery, and the scenarios (decision logic only; fakes for the clock and the shell)
        Check(SbClassifyEnd(true, false, false, false) == SbEnd::Unexpected, "sidebar lifecycle: a window that vanishes while meant to be on is an unexpected loss");
        Check(SbClassifyEnd(true, false, true, false) == SbEnd::UserClose && SbClassifyEnd(true, false, false, true) == SbEnd::UserClose && SbClassifyEnd(false, false, false, false) == SbEnd::UserClose, "sidebar lifecycle: slide-out, our own destroy and 'off' are user closes");
        Check(SbClassifyEnd(true, true, false, false) == SbEnd::Shutdown && SbClassifyEnd(false, true, true, true) == SbEnd::Shutdown, "sidebar lifecycle: while the app quits every end is a shutdown, never a loss");
        Check(SbBackoffMs(0) == 0 && SbBackoffMs(1) == 1000 && SbBackoffMs(2) == 2000 && SbBackoffMs(4) == 8000 && SbBackoffMs(5) == 16000 && SbBackoffMs(40) <= 30000, "sidebar lifecycle: waits grow 1, 2, 4, 8, 16 s and are capped");
        SbLifeFacts f; f.wantOn = true;
        SbRetry r;
        Check(SbLifeDecide(f, r, 1000).act == SbLifeAct::Recreate, "sidebar lifecycle: window missing, on, no history -> recreate");
        { SbLifeFacts x = f; x.wantOn = false; Check(SbLifeDecide(x, r, 1000).act == SbLifeAct::Nothing, "sidebar lifecycle: user closed it -> nothing is recreated"); }
        { SbLifeFacts x = f; x.quitting = true; Check(SbLifeDecide(x, r, 1000).act == SbLifeAct::Nothing, "sidebar lifecycle: app shutdown -> no automatic reopen"); }
        { SbLifeFacts x = f; x.closing = true; Check(SbLifeDecide(x, r, 1000).act == SbLifeAct::Nothing, "sidebar lifecycle: during the user's slide-out -> nothing"); }
        { SbLifeFacts x = f; x.haveWindow = true; Check(SbLifeDecide(x, r, 1000).act == SbLifeAct::Nothing, "sidebar lifecycle: a window exists -> never a second one (no duplicate)"); }
        // scenario: Explorer restart. The window is lost once and recreated; a loss long after is a new incident.
        { SbRetry e; uint64_t t = 100000; SbRetryLoss(e, t);
          Check(SbLifeDecide(f, e, t).act == SbLifeAct::Recreate, "scenario Explorer restart: first loss -> recreate");
          SbRetryAttempt(e, t); t += 400;
          Check(SbLifeDecide(f, e, t).act == SbLifeAct::Wait && SbLifeDecide(f, e, t).waitMs == 600, "scenario Explorer restart: a second loss moments later waits out the back-off (600 ms left)");
          t += 40000; SbRetryLoss(e, t); Check(e.tries == 0 && !e.gaveUp && SbLifeDecide(f, e, t).act == SbLifeAct::Recreate, "scenario Explorer restart: a loss long after the last attempt is a new incident (counter reset)"); }
        // scenario: the window is destroyed again right after every recreation (a broken desktop). Bounded: no endless loop.
        { SbRetry e; uint64_t t = 0; int created = 0;
          for (int step = 0; step < 2400; ++step, t += 250) {   // ten simulated minutes in 250 ms steps
              if (SbLifeDecide(f, e, t).act == SbLifeAct::Recreate) { SbRetryAttempt(e, t); ++created; SbRetryLoss(e, t); }   // ... and it is destroyed again at once
          }
          Check(created == kSbRetryMax && e.gaveUp, "scenario repeated failure: " + std::to_string(created) + " attempts in 10 minutes, then it stops (limit " + std::to_string(kSbRetryMax) + ")");
          Check(SbLifeDecide(f, e, t).act == SbLifeAct::GiveUp, "scenario repeated failure: after the limit the verdict is give up, not wait or retry");
          Check(!SbRetryKick(e, e.gaveUpAt + 1000), "scenario repeated failure: an event right after giving up does not re-arm");
          const uint64_t later = e.gaveUpAt + kSbKickGapMs;
          Check(SbRetryKick(e, later) && !e.gaveUp && SbLifeDecide(f, e, later).act == SbLifeAct::Recreate, "scenario repeated failure: a later shell event allows a fresh series");
          SbRetry u = e; SbRetryReset(u); Check(!u.gaveUp && u.tries == 0 && SbLifeDecide(f, u, 1).act == SbLifeAct::Recreate, "scenario repeated failure: the user's own switch-on starts clean"); }
        // scenario: it comes back and holds -> a give-up is forgiven
        { SbRetry e; uint64_t t = 0; for (int i = 0; i < kSbRetryMax; ++i) { SbRetryAttempt(e, t); t += 2000; }
          Check(e.gaveUp, "scenario: five quick failures give up");
          SbRetryHeal(e, t + kSbRetryStableMs + 1); Check(!e.gaveUp && e.tries == 0, "scenario: healthy for 30 s forgives the earlier failures"); }
        // a refused creation (CreateWindow fails) is counted like any other attempt and backs off
        { SbRetry e; SbRetryAttempt(e, 1000); Check(!SbRetryMay(e, 1500) && SbRetryMay(e, 2000), "scenario creation refused: the next try waits 1 s"); }
        // in-place repairs of a living window are bounded the same way
        { SbRetry h; uint64_t t = 0; int repairs = 0; for (int step = 0; step < 2400; ++step, t += 250) if (SbRetryMay(h, t)) { SbRetryAttempt(h, t); ++repairs; }
          Check(repairs == kSbRetryMax, "health repairs: a problem that never goes away is repaired " + std::to_string(repairs) + " times in 10 minutes, then left until a shell event"); }
        Check(SbEventSettleMs(SbShellEvent::ExplorerRestarted) > SbEventSettleMs(SbShellEvent::DisplayChanged) && SbEventSettleMs(SbShellEvent::Resumed) >= SbEventSettleMs(SbShellEvent::ExplorerRestarted) && SbEventNeedsReplace(SbShellEvent::DisplayChanged), "sidebar lifecycle: shell events re-place the window after a settle time");
        { SbRetry e; SbRetryAttempt(e, 0); SbRetryAttempt(e, 1000); SbRetryAttempt(e, 3000); Check(e.nextAt == 3000 + 4000, "scenario: the third attempt is followed by a 4 s wait"); Check(SbRetryKick(e, 4000) && e.nextAt == 5000 && !e.gaveUp, "scenario: Explorer restarts during a long back-off -> look again in 1 s"); }
    }
    {   // ---- C2: Backup listing metadata cache & async storage scan controller
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring c2Dir = std::wstring(tmp) + L"ankhcore_c2_test_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring bakDir = c2Dir + L"\\Backups";
        const std::wstring rootApp = c2Dir + L"\\AppData";
        const std::wstring rootData = c2Dir + L"\\Data";
        const std::wstring rootContent = c2Dir + L"\\Content";
        Evict(c2Dir, false, false);
        SHCreateDirectoryExW(nullptr, bakDir.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, rootApp.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, rootData.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, rootContent.c_str(), nullptr);
        const std::string cfgJson = "{\r\n  \"settings.version\": 1,\r\n  \"nav.page\": 0\r\n}\r\n";
        WriteAll(rootApp + L"\\settings.json", reinterpret_cast<const uint8_t*>(cfgJson.data()), cfgJson.size());
        const AcbRoots roots{rootApp, rootData, rootContent};

        const std::wstring b1 = bakDir + L"\\AnkhCore-20261117-010000.acbackup";
        const std::wstring b2 = bakDir + L"\\AnkhCore-20261117-020000.acbackup";
        AcbCheck chk1{}, chk2{}; std::wstring err;
        Check(AcbCreateBackup(roots, b1, "settings", {}, chk1, err) && AcbCreateBackup(roots, b2, "settings", {}, chk2, err),
              "c2: created two test .acbackup archives");

        // 1. Metadata inspection + 12-entry cap without full payload SHA-256 verification
        std::vector<uint8_t> raw1;
        Check(ReadAll(b1, raw1) && AcbInspectMetadata(raw1.data(), raw1.size()).ok,
              "c2: AcbInspectMetadata validates archive structure and manifest without hashing payload");
        const auto capped = AcbListBackupsMetadata(bakDir, 1);
        Check(capped.size() == 1 && capped[0].fileName == L"AnkhCore-20261117-020000.acbackup",
              "c2: AcbListBackupsMetadata respects maxEntries cap and orders newest first");

        // 2. Tamper detection preserved before actual restore (AcbRestoreBackup still runs full AcbVerify)
        {
            std::vector<uint8_t> tampered = raw1;
            // Corrupt a byte inside the compressed/stored settings.json payload (keep ZIP central directory intact if possible, or any corruption)
            if (tampered.size() > 64) tampered[40] ^= 0xFF;
            const std::wstring badArc = bakDir + L"\\AnkhCore-20261117-000000-bad.acbackup";
            WriteAll(badArc, tampered.data(), tampered.size());
            const AcbRestoreReport rBad = AcbRestoreBackup(roots, badArc);
            Check(!rBad.ok && !rBad.error.empty(),
                  "c2: AcbRestoreBackup still rejects corrupted archive with full verification before writing");
            DeleteFileW(badArc.c_str());
        }

        // 3. Cache validity, hit reuse, explicit invalidation, and directory change detection (create/import/delete)
        {
            AcbBackupListCache cache;
            bool hit = false;
            const auto first = cache.GetOrRefresh(bakDir, 12, &hit);
            Check(!hit && cache.misses.load() == 1 && cache.hits.load() == 0 && first->size() == 2,
                  "c2: first backup cache lookup is a miss and populates 2 entries");
            const auto second = cache.GetOrRefresh(bakDir, 12, &hit);
            Check(hit && cache.misses.load() == 1 && cache.hits.load() == 1 && second->size() == 2,
                  "c2: repeated backup cache lookup is a hit with zero re-reads");

            cache.Invalidate();
            const auto afterInv = cache.GetOrRefresh(bakDir, 12, &hit);
            Check(!hit && cache.misses.load() == 2 && afterInv->size() == 2,
                  "c2: explicit Invalidate() forces refresh on next lookup");

            // Simulate importing/creating a new backup in the folder without explicit Invalidate()
            const std::wstring b3 = bakDir + L"\\AnkhCore-20261117-030000.acbackup";
            AcbCheck chk3{};
            Check(AcbCreateBackup(roots, b3, "settings", {}, chk3, err), "c2: create third archive");
            const auto afterAdd = cache.GetOrRefresh(bakDir, 12, &hit);
            Check(!hit && cache.misses.load() == 3 && afterAdd->size() == 3,
                  "c2: directory stamp detects newly created/imported backup");

            // Simulate deleting a backup
            DeleteFileW(b3.c_str());
            const auto afterDel = cache.GetOrRefresh(bakDir, 12, &hit);
            Check(!hit && cache.misses.load() == 4 && afterDel->size() == 2,
                  "c2: directory stamp detects deleted backup");

            // Async refresh + Join
            AcbBackupListCache asyncCache;
            std::atomic<int> callbacks{0};
            asyncCache.RefreshAsync(bakDir, 12, [&] { ++callbacks; });
            for (int i = 0; i < 100 && !asyncCache.Snapshot().valid; ++i) Sleep(20);
            asyncCache.Join();
            const auto snap = asyncCache.Snapshot();
            Check(snap.valid && !snap.loading && snap.items->size() == 2 && callbacks.load() == 1,
                  "c2: AcbBackupListCache::RefreshAsync populates snapshot and invokes completion callback");
        }

        // 4. StorageScanController: duplicate prevention, marshaling callback, cancellation, and failure handling
        {
            StorageScanController sc;
            HANDLE startedEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE releaseEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<int> doneCount{0};
            sc.scanFn = [&](const std::atomic<bool>&) -> std::vector<StorageScanItem> {
                SetEvent(startedEv);
                WaitForSingleObject(releaseEv, 5000);
                return {{L"User Temp", L"C:\\Temp", 4096, 3}};
            };
            const bool firstStarted = sc.StartScan(L"", L"", L"", L"", [&] { ++doneCount; });
            WaitForSingleObject(startedEv, 2000);
            const bool dupStarted = sc.StartScan(L"", L"", L"", L"", [&] { ++doneCount; });
            Check(firstStarted && !dupStarted && sc.Snapshot().scanning,
                  "c2: StorageScanController prevents duplicate concurrent scans");
            SetEvent(releaseEv);
            for (int i = 0; i < 100 && sc.Snapshot().scanning; ++i) Sleep(20);
            sc.Shutdown();
            const auto stDone = sc.Snapshot();
            Check(stDone.scanned && !stDone.scanning && !stDone.failed && stDone.items.size() == 1 && doneCount.load() == 1,
                  "c2: StorageScanController completes, stores items, and invokes callback");
            CloseHandle(startedEv);
            CloseHandle(releaseEv);
        }
        {   // Cancellation discards in-flight scan result and does not mark scanned
            StorageScanController sc;
            HANDLE startedEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE releaseEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::atomic<int> doneCount{0};
            sc.scanFn = [&](const std::atomic<bool>&) -> std::vector<StorageScanItem> {
                SetEvent(startedEv);
                WaitForSingleObject(releaseEv, 5000);
                return {{L"Downloads", L"C:\\DL", 1024, 1}};
            };
            Check(sc.StartScan(L"", L"", L"", L"", [&] { ++doneCount; }), "c2: start cancellable scan");
            WaitForSingleObject(startedEv, 2000);
            sc.Cancel();
            SetEvent(releaseEv);
            sc.Shutdown();
            const auto stCancel = sc.Snapshot();
            Check(!stCancel.scanned && !stCancel.scanning && stCancel.items.empty() && doneCount.load() == 0,
                  "c2: cancelled StorageScanController discards results and skips completion callback");
            CloseHandle(startedEv);
            CloseHandle(releaseEv);
        }
        {   // Failure handling: worker exception surfaces error and clears scanning state
            StorageScanController sc;
            std::atomic<int> doneCount{0};
            sc.scanFn = [&](const std::atomic<bool>&) -> std::vector<StorageScanItem> {
                throw std::runtime_error("simulated disk I/O error");
            };
            Check(sc.StartScan(L"", L"", L"", L"", [&] { ++doneCount; }), "c2: start failing scan");
            for (int i = 0; i < 100 && sc.Snapshot().scanning; ++i) Sleep(20);
            sc.Shutdown();
            const auto stErr = sc.Snapshot();
            Check(!stErr.scanning && stErr.failed && stErr.error.find(L"simulated disk I/O error") != std::wstring::npos && doneCount.load() == 1,
                  "c2: StorageScanController captures worker failure safely without leaving scanning stuck");
        }
        Evict(c2Dir, false, false);
    }
    {   // ---- B2: Recovery Point & Snapshot Restore
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring b2Dir = std::wstring(tmp) + L"ankhcore_b2_test_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring rootApp = b2Dir + L"\\AppData";
        const std::wstring rootData = b2Dir + L"\\Data";
        const std::wstring rootContent = b2Dir + L"\\Content";
        Evict(b2Dir, false, false);
        SHCreateDirectoryExW(nullptr, rootApp.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, rootData.c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, rootContent.c_str(), nullptr);
        const AcbRoots roots{rootApp, rootData, rootContent};

        // 1. Journal separation: RECOVERY_POINT & SNAPSHOT_RESTORE excluded from active count & Undo All
        {
            const std::wstring jPath = b2Dir + L"\\journal_b2.txt";
            JournalPathOverride() = jPath;
            DeleteFileW(jPath.c_str());
            JournalAdd(L"Backup", L"RECOVERY_POINT", L"Recovery point before backup restore", {b2Dir + L"\\Recovery\\pt1", L"test.acbackup"});
            JournalAdd(L"Backup", L"SNAPSHOT_RESTORE", L"Snapshot restore (SUCCESS)", {b2Dir + L"\\Recovery\\pt2", b2Dir + L"\\Recovery\\pt1", L"1", L"0", L"SUCCESS"});
            const auto all = JournalLoad();
            const JournalView jv = JournalBuildView(all);
            Check(jv.ops.size() == 2 && jv.activeEntries == 0 && jv.ops[0].informational && jv.ops[1].informational,
                  "b2: JournalBuildView marks RECOVERY_POINT and SNAPSHOT_RESTORE informational with 0 active entries");
            const UndoCount uc = JournalUndoEx(JournalPickUndoAll());
            Check(uc.picked == 0 && uc.undone == 0,
                  "b2: JournalPickUndoAll excludes RECOVERY_POINT and SNAPSHOT_RESTORE (FC-28)");
            JournalPathOverride().clear();
        }

        // 2. Manifest v1/v2/v3 parsing
        {
            AcbRecManifest m1, m3; std::wstring mErr;
            const std::string v1Json = "{ \"created\": \"2026-11-17T00:00:00Z\", \"sourceArchive\": \"old.acbackup\", \"items\": [ { \"dest\": \"data:net-counters.txt\", \"existed\": true, \"size\": 5, \"sha256\": \"abc\" } ] }";
            Check(AcbParseRecoveryManifest(v1Json, m1, mErr) && m1.formatVersion == 1 && !m1.sealed && m1.items.size() == 1,
                  "b2: v1 recovery manifest parses cleanly as unsealed");
            const std::string v3Json = "{ \"formatVersion\": 3, \"items\": [] }";
            Check(!AcbParseRecoveryManifest(v3Json, m3, mErr) && mErr.find(L"newer") != std::wstring::npos,
                  "b2: v3 recovery manifest is refused");
        }

        // 3. 3-way settings.json merge
        {
            const std::string beforeS = "{\r\n  \"nav.page\": 1,\r\n  \"sidebar.width\": 180,\r\n  \"fan.enabled\": false\r\n}\r\n";
            const std::string afterS  = "{\r\n  \"nav.page\": 1,\r\n  \"sidebar.width\": 240,\r\n  \"fan.enabled\": true\r\n}\r\n";
            const std::string curS    = "{\r\n  \"nav.page\": 5,\r\n  \"sidebar.width\": 240,\r\n  \"fan.enabled\": false,\r\n  \"user.added\": 42\r\n}\r\n";
            std::string mergedS; int rev = 0, kept = 0; std::wstring sErr;
            Check(AcbMergeSettings3(beforeS, afterS, curS, mergedS, rev, kept, sErr) &&
                  rev >= 1 && kept >= 1 &&
                  mergedS.find("\"sidebar.width\": 180") != std::string::npos &&
                  mergedS.find("\"nav.page\": 5") != std::string::npos &&
                  mergedS.find("\"user.added\": 42") != std::string::npos &&
                  mergedS.find("\"fan.enabled\": false") != std::string::npos,
                  "b2: AcbMergeSettings3 reverts op-changed key, preserves user-modified/added keys, and enforces D30");
        }

        // 4. Successful snapshot restoration + modified file skip + newly created file delete + deleted file skip
        const std::wstring sPath = rootApp + L"\\settings.json";
        const std::wstring nPath = rootData + L"\\net-counters.txt";
        const std::string sBefore = "{\r\n  \"settings.version\": 1,\r\n  \"sidebar.width\": 180,\r\n  \"nav.page\": 1\r\n}\r\n";
        const std::string sAfter  = "{\r\n  \"settings.version\": 1,\r\n  \"sidebar.width\": 260,\r\n  \"nav.page\": 1\r\n}\r\n";
        const std::string nBefore = "net-before\n";
        const std::string nAfter  = "net-after\n";
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sBefore.data()), sBefore.size());
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nBefore.data()), nBefore.size());

        // Create a sealed recovery point representing an operation that changed settings.json & net-counters.txt,
        // plus created content:newfile.txt (existed: false -> afterExists: true)
        const std::wstring cNewPath = rootContent + L"\\newfile.txt";
        const std::wstring recDir1 = rootData + L"\\Recovery\\20261117-010000-001";
        EnsureDir(recDir1 + L"\\files\\settings");
        EnsureDir(recDir1 + L"\\files\\monitoring");
        WriteAll(recDir1 + L"\\files\\settings\\settings.json", reinterpret_cast<const uint8_t*>(sBefore.data()), sBefore.size());
        WriteAll(recDir1 + L"\\files\\monitoring\\net-counters.txt", reinterpret_cast<const uint8_t*>(nBefore.data()), nBefore.size());
        AcbRecManifest man1;
        man1.formatVersion = 2; man1.kind = "restore"; man1.created = "2026-11-17T01:00:00Z"; man1.sourceArchive = "test.acbackup";
        {
            AcbRecItem i1; i1.dest = "appdata:settings.json"; i1.backupPath = "files\\settings\\settings.json"; i1.existed = true; i1.strategy = "settings-merge3";
            AcbHashFile(recDir1 + L"\\files\\settings\\settings.json", i1.size, i1.sha256);
            AcbRecItem i2; i2.dest = "data:net-counters.txt"; i2.backupPath = "files\\monitoring\\net-counters.txt"; i2.existed = true; i2.strategy = "file-copy";
            AcbHashFile(recDir1 + L"\\files\\monitoring\\net-counters.txt", i2.size, i2.sha256);
            AcbRecItem i3; i3.dest = "content:newfile.txt"; i3.backupPath = "files\\content\\newfile.txt"; i3.existed = false; i3.strategy = "file-copy";
            man1.items = {i1, i2, i3};
        }
        AcbWriteRecoveryManifest(recDir1, man1);
        // Apply operation's "after" state and seal
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sAfter.data()), sAfter.size());
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nAfter.data()), nAfter.size());
        const std::string newTxt = "created-by-op\n";
        WriteAll(cNewPath, reinterpret_cast<const uint8_t*>(newTxt.data()), newTxt.size());
        Check(AcbSealRecoveryPoint(roots, recDir1), "b2: AcbSealRecoveryPoint seals after-state and after\\settings.json");

        // Case A: Successful snapshot restore (user also changed nav.page=4 in settings.json; net-counters.txt & newfile.txt untouched)
        const std::string sUser = "{\r\n  \"settings.version\": 1,\r\n  \"sidebar.width\": 260,\r\n  \"nav.page\": 4\r\n}\r\n";
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sUser.data()), sUser.size());
        {
            const AcbSnapRestoreReport rOk = AcbRestoreSnapshot(roots, recDir1);
            const std::string sNow = ReadText(sPath);
            Check(rOk.ok && !rOk.partial && rOk.filesRestored == 2 && rOk.filesDeleted == 1 &&
                  sNow.find("\"sidebar.width\": 180") != std::string::npos &&
                  sNow.find("\"nav.page\": 4") != std::string::npos &&
                  ReadText(nPath) == nBefore && !FileExists(cNewPath) && !rOk.newRecoveryDir.empty(),
                  "b2: successful snapshot restore reverts settings (preserving nav.page), restores net-counters, deletes op-created file, and creates new recovery point");
        }

        // Case B: Modified file after operation -> skipped; Deleted file after operation -> skipped
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sAfter.data()), sAfter.size());
        const std::string nUserMod = "user-edited-net\n";
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nUserMod.data()), nUserMod.size());
        DeleteFileW(cNewPath.c_str());
        {
            const AcbSnapPlan pMod = AcbPlanSnapshotRestore(roots, recDir1);
            Check(pMod.ok && pMod.items[1].action == AcbSnapAction::SkipModified,
                  "b2: file modified after operation is planned as SkipModified");
            const AcbSnapRestoreReport rMod = AcbRestoreSnapshot(roots, recDir1);
            Check(rMod.ok && rMod.partial && rMod.filesSkipped == 1 && ReadText(nPath) == nUserMod,
                  "b2: modified file is skipped and not overwritten during snapshot restore");
        }
        // Deleted file after operation
        DeleteFileW(nPath.c_str());
        {
            const AcbSnapPlan pDel = AcbPlanSnapshotRestore(roots, recDir1);
            Check(pDel.ok && pDel.items[1].action == AcbSnapAction::SkipDeleted,
                  "b2: file deleted after operation is planned as SkipDeleted");
        }

        // Case C: Pre-restore recovery point failure -> zero writes
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nAfter.data()), nAfter.size());
        {
            AcbSnapRestoreOptions opts; opts.failPrePointForTest = true;
            const AcbSnapRestoreReport rPre = AcbRestoreSnapshot(roots, recDir1, opts);
            Check(!rPre.ok && ReadText(nPath) == nAfter,
                  "b2: failure to create pre-restore recovery point aborts with zero writes");
        }

        // Case D: Partial restoration failure -> rollback from newRecoveryDir
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sAfter.data()), sAfter.size());
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nAfter.data()), nAfter.size());
        {
            AcbSnapRestoreOptions opts; opts.failAfterFilesForTest = 1;
            const AcbSnapRestoreReport rFail = AcbRestoreSnapshot(roots, recDir1, opts);
            Check(!rFail.ok && rFail.rolledBack && ReadText(sPath) == sAfter && ReadText(nPath) == nAfter,
                  "b2: partial restoration failure rolls back touched files and never reports success");
        }

        // Case E: Interrupted restoration + AcbFinishInterruptedRollback
        WriteAll(sPath, reinterpret_cast<const uint8_t*>(sAfter.data()), sAfter.size());
        WriteAll(nPath, reinterpret_cast<const uint8_t*>(nAfter.data()), nAfter.size());
        {
            AcbSnapRestoreOptions opts; opts.interruptAfterFirstForTest = true;
            const AcbSnapRestoreReport rInt = AcbRestoreSnapshot(roots, recDir1, opts);
            Check(!rInt.ok && rInt.interrupted && FileExists(rInt.newRecoveryDir + L"\\restore-in-progress.marker"),
                  "b2: interrupted snapshot restore leaves marker and reports interrupted");
            const AcbSnapRestoreReport rFin = AcbFinishInterruptedRollback(roots, rInt.newRecoveryDir);
            Check(rFin.ok && rFin.rolledBack && !FileExists(rInt.newRecoveryDir + L"\\restore-in-progress.marker") &&
                  ReadText(sPath) == sAfter,
                  "b2: AcbFinishInterruptedRollback restores pre-restore state and clears marker");
            const AcbSnapRestoreReport rFin2 = AcbFinishInterruptedRollback(roots, rInt.newRecoveryDir);
            Check(rFin2.ok && rFin2.rollbackPutBack == 0,
                  "b2: second AcbFinishInterruptedRollback call is a safe no-op");
        }

        // Case F: Missing / corrupted recovery copy -> zero writes
        {
            const std::wstring badCopy = recDir1 + L"\\files\\monitoring\\net-counters.txt";
            const std::string corrupt = "corrupted-bytes";
            WriteAll(badCopy, reinterpret_cast<const uint8_t*>(corrupt.data()), corrupt.size());
            const AcbSnapPlan pBad = AcbPlanSnapshotRestore(roots, recDir1);
            const AcbSnapRestoreReport rBad = AcbRestoreSnapshot(roots, recDir1);
            Check(pBad.ok && pBad.blockedCount == 1 && !rBad.ok && ReadText(nPath) == nAfter,
                  "b2: corrupted recovery copy blocks restore with zero writes");
            DeleteFileW(badCopy.c_str());
            const AcbSnapPlan pMiss = AcbPlanSnapshotRestore(roots, recDir1);
            Check(pMiss.ok && pMiss.blockedCount == 1 && !AcbRestoreSnapshot(roots, recDir1).ok,
                  "b2: missing recovery copy blocks restore with zero writes");
        }

        Evict(b2Dir, false, false);
    }
    {   // ---- C5 & C6: Session unlock lifecycle & System Monitor data consistency
        // C5: Session unlock lifecycle, duplicate handling, shutdown cleanup
        SbSessionState ss;
        ss.OnRegistered(true);
        Check(ss.registered && !ss.locked, "c5: OnRegistered marks session notification active");
        Check(!ss.OnSessionEvent(true, false, 1000) && ss.locked, "c5: WTS_SESSION_LOCK sets locked state");
        Check(ss.OnSessionEvent(false, true, 1200) && !ss.locked && ss.unlockEvents == 1,
              "c5: WTS_SESSION_UNLOCK triggers handling and clears locked");
        Check(!ss.OnSessionEvent(false, true, 1500) && ss.duplicatesIgnored == 1 && ss.unlockEvents == 1,
              "c5: duplicate unlock within debounce window is ignored");
        Check(ss.OnSessionEvent(false, true, 2500) && ss.unlockEvents == 2,
              "c5: unlock after debounce window triggers again");
        ss.OnUnregistered();
        Check(!ss.registered && !ss.locked, "c5: OnUnregistered cleans up session state on shutdown");
        Check(SbEventSettleMs(SbShellEvent::SessionUnlocked) == 1500 && SbEventNeedsReplace(SbShellEvent::SessionUnlocked),
              "c5: SessionUnlocked settle time and replace flag preserved");

        // C6: ToNum rejects NaN/Infinity/invalid strings instead of returning 0 or NaN
        Check(ToNum(L"0") == 0.0 && ToNum(L"42.5") == 42.5, "c6: ToNum parses valid zero and positive numbers");
        Check(ToNum(L"") == -1.0 && ToNum(L"NaN") == -1.0 && ToNum(L"Infinity") == -1.0 &&
              ToNum(L"-inf") == -1.0 && ToNum(L"abc") == -1.0 && ToNum(L"12abc") == -1.0,
              "c6: ToNum returns -1 for empty, NaN, Infinity, and malformed numbers");

        // C6: Unavailable helper telemetry vs valid zero, CPU source consistency, and GPU association
        Readings rLive = ParseReadings("CPU.LOAD=18.5\nGPU.NAME=NVIDIA Quadro P2000\nGPU.TEMP=52\nGPU.NAME=Intel UHD 630\nDISK.TEMP=0\nDISK.LOAD=0\n");
        rLive.present = true;
        Check(HelperReading(rLive, rLive.diskLoad) == 0.0 && HelperReading(rLive, rLive.diskTemp) == 0.0,
              "c6: valid 0 reading preserved when helper telemetry is live");
        Readings rStale = rLive;
        rStale.present = false;
        Check(HelperReading(rStale, rStale.diskLoad) == -1.0 && HelperReading(rStale, rStale.cpuTemp) == -1.0,
              "c6: stale/unavailable helper telemetry returns -1 instead of stale values");
        Check(EffectiveCpuLoad(37.5, rLive) == 37.5 && EffectiveCpuLoad(37.5, rStale) == 37.5 &&
              EffectiveCpuLoad(-1.0, rLive) == 18.5 && EffectiveCpuLoad(-1.0, rStale) == -1.0,
              "c6: EffectiveCpuLoad keeps Dashboard/System Monitor/Sidebar/Hardware Info consistent");
        Check(rLive.gpuName == L"NVIDIA Quadro P2000",
              "c6: ParseReadings preserves primary GPU.NAME association");
    }
    {   // ---- FC-16: Atomic verified directory placement (PlaceVerifiedTree)
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring f16 = std::wstring(tmp) + L"ankhcore_fc16_test_" + std::to_wstring(GetCurrentProcessId());
        const std::wstring src = f16 + L"\\src";
        const std::wstring dst = f16 + L"\\dst\\theme";
        const std::wstring unrelated = f16 + L"\\dst\\unrelated.txt";
        Evict(f16, false, false);
        SHCreateDirectoryExW(nullptr, (src + L"\\sub").c_str(), nullptr);
        SHCreateDirectoryExW(nullptr, (f16 + L"\\dst").c_str(), nullptr);
        const std::string a = "pack-ini", b = "icon-data", u = "keep-me";
        WriteAll(src + L"\\Pack.ini", reinterpret_cast<const uint8_t*>(a.data()), a.size());
        WriteAll(src + L"\\sub\\imageres.dll", reinterpret_cast<const uint8_t*>(b.data()), b.size());
        WriteAll(unrelated, reinterpret_cast<const uint8_t*>(u.data()), u.size());

        std::wstring err;
        // 1. Missing source fails safely
        Check(!PlaceVerifiedTree(f16 + L"\\missing", dst, nullptr, err) && !DirExists(dst),
              "fc-16: missing source fails without creating destination");

        // 2. Partial copy failure leaves no partial files at dst and no staging dir
        {
            PlaceTreeFaults flt; flt.failCopyFileName = L"imageres.dll";
            Check(!PlaceVerifiedTree(src, dst, nullptr, err, &flt) &&
                  !DirExists(dst) && !DirExists(dst + L".ankh-stage") && ReadText(unrelated) == u,
                  "fc-16: mid-tree copy failure leaves destination absent, no staging dir, and unrelated file intact");
        }

        // 3. Validation failure leaves no partial files at dst
        Check(!PlaceVerifiedTree(src, dst, [](const std::wstring&, std::wstring& ve) { ve = L"invalid theme"; return false; }, err) &&
              !DirExists(dst) && !DirExists(dst + L".ankh-stage"),
              "fc-16: failed validation leaves destination absent and cleans up staging dir");

        // 4. Successful placement preserves directory structure and all files
        Check(PlaceVerifiedTree(src, dst, nullptr, err) &&
              ReadText(dst + L"\\Pack.ini") == a &&
              ReadText(dst + L"\\sub\\imageres.dll") == b &&
              !DirExists(dst + L".ankh-stage") && ReadText(unrelated) == u,
              "fc-16: PlaceVerifiedTree copies full tree atomically and preserves unrelated files");

        // 5. Existing destination + swap failure rolls back to previous destination intact
        {
            const std::string aNew = "pack-ini-v2";
            WriteAll(src + L"\\Pack.ini", reinterpret_cast<const uint8_t*>(aNew.data()), aNew.size());
            PlaceTreeFaults flt; flt.failSwap = true;
            Check(!PlaceVerifiedTree(src, dst, nullptr, err, &flt) &&
                  ReadText(dst + L"\\Pack.ini") == a &&
                  !DirExists(dst + L".ankh-stage") && !DirExists(dst + L".ankh-old"),
                  "fc-16: swap failure restores existing destination intact with no leftover temp dirs");
        }

        // 6. Existing destination + successful upgrade replaces with new files
        Check(PlaceVerifiedTree(src, dst, nullptr, err) &&
              ReadText(dst + L"\\Pack.ini") == "pack-ini-v2" &&
              !DirExists(dst + L".ankh-old") && ReadText(unrelated) == u,
              "fc-16: upgrade over existing destination succeeds cleanly");

        Evict(f16, false, false);
    }
    {   // ---- FC-13: Acknowledgements & Credits list
        const auto& acks = AcknowledgementsList();
        bool hasLhm = false, hasPawn = false, hasWh = false, allValid = !acks.empty();
        for (const auto& a : acks) {
            if (a.name.empty() || a.author.empty() || a.license.empty() || a.note.empty()) allValid = false;
            if (a.name.find(L"LibreHardwareMonitorLib") != std::wstring::npos) hasLhm = true;
            if (a.name.find(L"PawnIO") != std::wstring::npos) hasPawn = true;
            if (a.name.find(L"Windhawk") != std::wstring::npos) hasWh = true;
        }
        Check(allValid && hasLhm && hasPawn && hasWh,
              "fc-13: AcknowledgementsList contains valid third-party attributions, licenses, and links");
        // FC-9: QuoteCliArg escapes trailing backslashes and backslashes before quotes per CommandLineToArgvW
        Check(QuoteCliArg(L"C:\\My Dir\\") == L"\"C:\\My Dir\\\\\"" &&
              QuoteCliArg(L"a\\\"b") == L"\"a\\\\\\\"b\"" &&
              QuoteCliArg(L"simple") == L"\"simple\"",
              "fc-9: QuoteCliArg escapes trailing and pre-quote backslashes");
    }
    {   // ---- 2.5.1 Phase C: Fan Auto+Floor, ClampFanFloor, and FanSetAutoSafe (set --auto + stop)
        std::vector<std::wstring> cmds;
        const FanOpResult autoRes = FanSetAutoSafe([&](const std::wstring& args, std::wstring& out) {
            cmds.push_back(args);
            out = L"ok";
            return true;
        });
        Check(autoRes.ok && cmds.size() == 2 && cmds[0] == L"set --auto" && cmds[1] == L"stop",
              "2.5.1 fan: FanSetAutoSafe executes both 'set --auto' and 'stop' to disable NBFC and restore BIOS control");
        Check(ClampFanFloor(25) == 40 && ClampFanFloor(60) == 60 && ClampFanFloor(120) == 100,
              "2.5.1 fan: ClampFanFloor enforces safe [40, 100] bounds");
        Check(ComputeAutoFloorTargetPct(40, 60.0, 0.0, false) == 49 && ComputeAutoFloorTargetPct(40, 70.0, 0.0, false) == 68 && ComputeAutoFloorTargetPct(40, 85.0, 0.0, false) == 100 && ComputeAutoFloorTargetPct(70, 60.0, 0.0, false) == 70,
              "fan 2.8.7: the target follows the temperature only (60 C with a 40% floor = 49%, not the fans' own last speed); the floor still holds");
        Check(ComputeAutoFloorTargetPct(60, 45.0, 0.0, false) == 60 &&
              ComputeAutoFloorTargetPct(60, 75.0, 72.0, false) == 72 &&
              ComputeAutoFloorTargetPct(60, 45.0, 0.0, false) == 60 &&
              ComputeAutoFloorTargetPct(60, 92.0, 60.0, false) == 100 &&
              ComputeAutoFloorTargetPct(60, 50.0, 50.0, true) == 100,
              "2.5.1 fan: Auto+Minimum Floor keeps >=60%, scales up with thermal demand, returns to 60%, and never blocks thermal override");
        // Manual Fixed Speed sequence: 60% -> 75% -> Automatic + hardware-rejection fallback + bounds
        std::vector<std::wstring> manCmds;
        auto mockOk = [&](const std::wstring& args, std::wstring& out) { manCmds.push_back(args); out = L"ok"; return true; };
        const FanOpResult r60 = FanSetSpeedSafe(60, -1, mockOk);
        const FanOpResult r75 = FanSetSpeedSafe(75, -1, mockOk);
        const FanOpResult rAuto = FanSetAutoSafe(mockOk);
        std::vector<std::wstring> rejCmds;
        const FanOpResult rRej = FanSetSpeedSafe(60, -1, [&](const std::wstring& args, std::wstring& out) {
            rejCmds.push_back(args);
            if (args.find(L"--speed") != std::wstring::npos) { out = L"rejected"; return false; }
            out = L"ok"; return true;
        });
        Check(r60.ok && r75.ok && rAuto.ok &&
              manCmds.size() == 4 && manCmds[0] == L"set --speed 60" && manCmds[1] == L"set --speed 75" &&
              manCmds[2] == L"set --auto" && manCmds[3] == L"stop" &&
              !rRej.ok && rejCmds.size() == 2 && rejCmds[0] == L"set --speed 60" && rejCmds[1] == L"set --auto" &&
              ClampFanDuty40(20) == 40 && ClampFanDuty40(120) == 100,
              "2.5.1 fan: Manual Fixed Speed sends 60% -> 75% -> Auto+Stop, reverts on rejection, and clamps [40, 100]");
        // Fan telemetry accuracy: Target vs Applied vs Actual RPM, N/A/Not Supported on invalid/unverified RPM, NBFC status parsing
        const NbfcStatusInfo nbfcSt = ParseNbfcStatus(
            L"Selected config name : HP ZBook Fury 15 G7\n"
            L"Read-only : False\n"
            L"Fan display name : Right fan\n"
            L"Auto control enabled : True\n"
            L"Current fan speed : 77.33\n"
            L"Target fan speed : 0.00\n");
        const FanTelemetryView tvUnavail = FormatFanTelemetryView(true, -1.0, false, false, 0.0, -1.0, nbfcSt.autoControlEnabled);
        const FanTelemetryView tvCtrlOnly = FormatFanTelemetryView(true, -1.0, false, true, 60.0, 60.0, false);
        const FanTelemetryView tvStaleZero = FormatFanTelemetryView(true, 0.0, false, false, 60.0, 58.0, false);
        const FanTelemetryView tvNan = FormatFanTelemetryView(true, std::numeric_limits<double>::quiet_NaN(), true, false, 60.0, 60.0, false);
        const FanTelemetryView tvLive = FormatFanTelemetryView(true, 4819.0, true, false, 0.0, 77.33, nbfcSt.autoControlEnabled);
        Check(nbfcSt.selectedConfig == L"HP ZBook Fury 15 G7" && nbfcSt.autoControlEnabled &&
              nbfcSt.currentSpeeds.size() == 1 && static_cast<int>(nbfcSt.currentSpeeds[0]) == 77 &&
              nbfcSt.targetSpeeds.size() == 1 && static_cast<int>(nbfcSt.targetSpeeds[0]) == 0 &&
              tvUnavail.actualRpmLabel == L"N/A" && !tvUnavail.physicalVerified &&
              tvCtrlOnly.actualRpmLabel == L"Not Supported" && !tvCtrlOnly.physicalVerified &&
              tvStaleZero.actualRpmLabel == L"N/A (unverified)" && !tvStaleZero.physicalVerified &&
              tvNan.actualRpmLabel == L"N/A" && !tvNan.physicalVerified &&
              tvLive.actualRpmLabel == L"4819 rpm" && tvLive.physicalVerified &&
              tvLive.targetLabel == L"Target 0%" && tvLive.appliedLabel == L"Applied 77%" && tvLive.biosAutoOverride,
              "fan telemetry: Target vs Applied vs Actual RPM separated; invalid/stale/NaN RPM never shown as 0 rpm");
    }
    {   // ---- 2.5.1 Phase B: Icon Pack Previews for all 8 reported packs
        const std::wstring themesRoot = L"D:\\Ankhcore\\release-inputs\\icon-themes\\IconThemes";
        if (DirExists(themesRoot)) {
            const wchar_t* packs8[] = {L"candy", L"3d-purple", L"amy-dark", L"coloration", L"hal-green", L"macos-elegance", L"tela-black", L"windows-23"};
            int pvOk = 0;
            for (const wchar_t* id : packs8) {
                const IconThemeStatus st = IconThemeQuickCheck(themesRoot + L"\\" + id);
                if (st.installed && !st.preview.empty() && FileExists(st.preview)) ++pvOk;
            }
            Check(pvOk == 8, "2.5.1 icons: all 8 reported packs (Candy + 7 themes) validate and expose readable preview images");
        }
    }
    {   // ---- 2.5.1 Phase E: Azenis Normal Select mapping (with Arrow.ani and button.ani fallback)
        const CurScheme withArrow = CurMapScheme(L"Ankhcore Azenis", L"D:\\Cursors\\Ankhcore Azenis", {L"Arrow.ani", L"button.ani", L"Busy.ani"});
        const CurScheme withAlias = CurMapScheme(L"Ankhcore Azenis", L"D:\\Cursors\\Ankhcore Azenis", {L"button.ani", L"Busy.ani"});
        Check(withArrow.file[0] == L"Arrow.ani" && withAlias.file[0] == L"button.ani",
              "2.5.1 cursor: Ankhcore Azenis maps Arrow.ani to Normal Select and falls back to button.ani when absent");
        const std::wstring azenisDir = L"D:\\Ankhcore\\windows\\Cursors\\Ankhcore Azenis";
        if (DirExists(azenisDir)) {
            const auto found = CurDiscoverFolder(azenisDir);
            const bool hasReal = found.size() == 1 && found[0].file[0] == L"Arrow.ani" && CurFileHeaderOk(CurRolePath(found[0], 0));
            const CurState tgt = hasReal ? CurTargetState(found[0], WinDir()) : CurState{};
            const CurState def = CurWindowsDefault();
            Check(hasReal && tgt.value[0] == azenisDir + L"\\Arrow.ani" && def.value[0].find(L"aero_arrow.cur") != std::wstring::npos,
                  "2.5.1 cursor: real Azenis pack on disk maps Normal Select to valid Arrow.ani and Windows default restores aero_arrow.cur");
        }
    }
    {   // ---- 2.5.1 Phase D: Windhawk Settings Save/Cancel staging + Simulated HideTrayIcon Registry
        WhStage st;
        st.StageSetting("test-mod", "opacity", WhMakeNum(80), WhMakeNum(100));
        Check(st.SettingCount("test-mod") == 1 && !st.Empty(),
              "2.5.1 windhawk: mod setting edits are staged until Save");
        st.CancelMod("test-mod");
        Check(st.SettingCount("test-mod") == 0 && st.Empty(),
              "2.5.1 windhawk: Cancel discards staged mod settings without applying");

        const wchar_t* sbKey = L"Software\\AnkhCore\\Sandbox\\WindhawkSettingsTest";
        RegDeleteTreeW(HKEY_CURRENT_USER, sbKey);
        HKEY hk = nullptr;
        DWORD v0 = 0, v1 = 1, got = 99, sz = sizeof(got), tp = 0;
        const LONG openRc = RegCreateKeyExW(HKEY_CURRENT_USER, sbKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &hk, nullptr);
        bool regOk = (openRc == ERROR_SUCCESS);
        if (regOk) {
            regOk &= RegSetValueExW(hk, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v0), sizeof(v0)) == ERROR_SUCCESS;
            regOk &= RegSetValueExW(hk, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v1), sizeof(v1)) == ERROR_SUCCESS;
            regOk &= RegQueryValueExW(hk, L"HideTrayIcon", nullptr, &tp, reinterpret_cast<BYTE*>(&got), &sz) == ERROR_SUCCESS && got == 1;
            regOk &= RegSetValueExW(hk, L"HideTrayIcon", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v0), sizeof(v0)) == ERROR_SUCCESS;
            sz = sizeof(got);
            regOk &= RegQueryValueExW(hk, L"HideTrayIcon", nullptr, &tp, reinterpret_cast<BYTE*>(&got), &sz) == ERROR_SUCCESS && got == 0;
            RegCloseKey(hk);
            RegDeleteTreeW(HKEY_CURRENT_USER, sbKey);
        }
        if (openRc == ERROR_ACCESS_DENIED)   // the process token may not write HKCU (restricted sandbox): not run, and said so; never counted as a pass
            std::printf("SKIP  2.5.1 windhawk: HideTrayIcon registry round-trip (HKCU write refused by this environment, error 5)\n");
        else
            Check(regOk, "2.5.1 windhawk: HideTrayIcon write and restore verified in isolated HKCU sandbox key");
    }
    {   // ---- Neutral Windows Glass Design System (theme.h)
        const ThemeSpec& t = ActiveTheme();
        auto isNeutral = [](COLORREF c) {
            const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
            const int spread = std::max({r, g, b}) - std::min({r, g, b});
            return spread <= 16;
        };
        const GlassLayerSpec bgL = GlassLayer(GlassLevel::Background);
        const GlassLayerSpec surfL = GlassLayer(GlassLevel::Surface);
        const GlassLayerSpec elevL = GlassLayer(GlassLevel::Elevated);
        const GlassLayerSpec interL = GlassLayer(GlassLevel::Interactive);
        const GlassLayerSpec overL = GlassLayer(GlassLevel::Overlay);
        Check(wcscmp(t.id, L"windows-glass") == 0 &&
              isNeutral(t.bg) && isNeutral(t.side) && isNeutral(t.panel) && isNeutral(t.panel2) &&
              isNeutral(t.border) && isNeutral(t.text) && isNeutral(t.sub) &&
              surfL.alpha < elevL.alpha && elevL.alpha < overL.alpha &&
              ThemeGlassAlpha(t.panel) == surfL.alpha && ThemeGlassAlpha(t.panel2) == elevL.alpha &&
              ThemeGlassAlpha(t.overlay) == overL.alpha && bgL.shadowAlpha == 0.0f && surfL.shadowAlpha > 0.0f &&
              interL.alpha > 0.0f,
              "theme: Windows Glass is the default, neutral-toned surfaces (no blue/purple tint; only the accent follows Windows), with 5-tier glass depth hierarchy");
        {   // 2.9.5 Task 2: the glass is 20 % more transparent, readable, still ordered
            auto closeTo = [](float a, float b) { return std::fabs(a - b) < 0.002f; };
            Check(closeTo(surfL.alpha, 0.60f * 0.80f) && closeTo(elevL.alpha, 0.74f * 0.80f) && closeTo(interL.alpha, 0.72f * 0.80f) && closeTo(bgL.alpha, 0.78f * 0.80f),
                  "glass 2.9.5: panel, elevated, interactive and background layers keep 80 % of their old opacity (20 % more transparent)");
            Check(overL.alpha >= 0.80f && overL.alpha < 0.90f && surfL.alpha >= 0.40f && surfL.alpha < elevL.alpha,
                  "glass 2.9.5: pop-up overlays stay at least 80 % opaque and panels at least 40 % (readability floor), layer order kept");
        }
        // Dual Theme switching: Neutral Glass <-> Classic AnkhCore + Footer (col::overlay) unification
        const ThemeSpec cl = ThemeById(L"classic");
        MutableActiveTheme() = cl;
        const bool clOk = ActiveTheme().isClassic && wcscmp(ActiveTheme().id, L"classic") == 0 &&
                          cl.overlay == RGB(10, 24, 52) && cl.accent == RGB(0, 174, 255);
        MutableActiveTheme() = ThemeById(L"neutral-glass");
        const bool ngOk = !ActiveTheme().isClassic && wcscmp(ActiveTheme().id, L"neutral-glass") == 0 &&
                          ActiveTheme().overlay == RGB(24, 26, 30) && isNeutral(ActiveTheme().overlay);
        const ThemeSpec fallback = ThemeById(L"nonexistent");
        Check(clOk && ngOk && wcscmp(fallback.id, L"windows-glass") == 0,
              "theme: the themes switch cleanly (including overlay/footer) and an unknown id falls back to Windows Glass");
    }
    {   // ---- Fan settings persistence across unrelated setting changes
        Settings s;
        s.SetInt(L"fan.mode", static_cast<int>(FanOperatingMode::AutoFloor));
        s.SetInt(L"fan.manualPct", 75);
        s.SetInt(L"fan.minFloorPct", 60);
        s.SetStr(L"fan.nbfcConfig", L"HP ZBook Fury 15 G7");
        s.SetStr(L"appearance.theme", L"classic");
        s.SetInt(L"monitor.intervalSec", 2);
        Check(s.Int(L"fan.mode", 0, 0, 2) == 2 &&
              s.Int(L"fan.manualPct", 40, 40, 100) == 75 &&
              s.Int(L"fan.minFloorPct", 50, 40, 100) == 60 &&
              s.Str(L"fan.nbfcConfig", L"") == L"HP ZBook Fury 15 G7" &&
              s.Str(L"appearance.theme", L"") == L"classic",
              "fan: changing unrelated settings preserves fan.mode, fan.manualPct, fan.minFloorPct, and fan.nbfcConfig");
    }
    {   // ---- Fan NBFC Brand Grouping & Search
        std::vector<FanCompatCandidate> cs(4);
        cs[0].name = L"HP ZBook 15 G5"; cs[0].exactModelMatch = true;
        cs[1].name = L"HP EliteBook 840";
        cs[2].name = L"Lenovo ThinkPad X1";
        cs[3].name = L"Asus ROG Strix";
        const auto brands = DiscoverFanConfigBrands(cs);
        const auto hpSearch = FilterFanConfigIndices(cs, L"HP", L"zbook", false);
        const auto globalSearch = FilterFanConfigIndices(cs, L"", L"thinkpad", false);
        Check(brands.size() == 3 && brands[0].brand == L"HP" && brands[0].count == 2 && brands[0].hasExact &&
              hpSearch.size() == 1 && hpSearch[0] == 0 &&
              globalSearch.size() == 1 && globalSearch[0] == 2,
              "fan: NBFC config picker groups by brand (HP/Lenovo/ASUS) and filters by search query");

        const bool keepImmediate = ShouldReassertFanKeepAlive(1000, 1000, 70, 60, false, 10000);
        const bool keepOverride  = ShouldReassertFanKeepAlive(1000, 1000, 70, 70, true,  10000);
        const bool keepSuppress  = !ShouldReassertFanKeepAlive(5000, 1000, 70, 70, false, 10000);
        const bool keepPeriodic  = ShouldReassertFanKeepAlive(11000, 1000, 70, 70, false, 10000);
        Check(keepImmediate && keepOverride && keepSuppress && keepPeriodic,
              "fan: keep-alive re-asserts periodically after 10s even when target matches lastSent");
    }
    {   // ---- Sidebar Desktop Integration stability (SbRetryAllowCooldown + SbHealthDecide + SbSyncState)
        SbRetry hr;
        uint64_t t = 1000;
        for (int i = 0; i < kSbRetryMax; ++i) { SbRetryAttempt(hr, t); t += 1000; }
        const bool gaveUpBeforeCooldown = hr.gaveUp && !SbRetryAllowCooldown(hr, t + 5000, 10000);
        const bool rearmedAfterCooldown = SbRetryAllowCooldown(hr, t + 11000, 10000) && !hr.gaveUp && SbRetryMay(hr, t + 11000);

        SbHealthInput in; in.wantDesktop = true; in.haveWindow = true; in.hosted = true; in.hostAvailable = true; in.parentIsHost = true; in.visible = true; in.beneathIcons = true;
        const SbVerdict vOk = SbHealthDecide(in);
        in.beneathIcons = false;
        const SbVerdict vRestack = SbHealthDecide(in);
        in.beneathIcons = true; in.visible = false;
        const SbVerdict vReshow = SbHealthDecide(in);
        in.visible = true; in.parentIsHost = false;
        const SbVerdict vRejoin = SbHealthDecide(in);

        SbSyncState sync;
        sync.userRequestedVisible = true; sync.actualWindowVisible = true; sync.desktopIntegrationActive = true;
        sync.autoHideActive = false; sync.positionLocked = true; sync.currentMonitor = L"\\\\.\\DISPLAY1";
        Check(gaveUpBeforeCooldown && rearmedAfterCooldown &&
              vOk.what == SbHealth::Healthy && vRestack.what == SbHealth::Restack &&
              vReshow.what == SbHealth::Reshow && vRejoin.what == SbHealth::Rejoin &&
              sync.userRequestedVisible && sync.desktopIntegrationActive && sync.positionLocked,
              "sidebar: cooldown re-arms after Z-order/visibility retry exhaustion; SbHealthDecide & SbSyncState cover all states");
    }
    {   // ---- Beta 30-day device trial evaluation
        const int64_t t0 = 1700000000LL;
        const BetaTrialStatus b0 = EvaluateBetaTrial(t0, 0, 0, 30);
        const BetaTrialStatus b15 = EvaluateBetaTrial(t0 + 15LL * 86400LL, t0, t0 + 14LL * 86400LL, 30);
        const BetaTrialStatus b30 = EvaluateBetaTrial(t0 + 30LL * 86400LL, t0, t0 + 29LL * 86400LL, 30);
        const BetaTrialStatus bTamper = EvaluateBetaTrial(t0 + 5LL * 86400LL, t0, t0 + 10LL * 86400LL, 30);
        const BetaTrialStatus bOff = EvaluateBetaTrial(t0 + 99LL * 86400LL, t0, t0, 0);
        Check(b0.active && !b0.expired && b0.daysRemaining == 30 &&
              b15.active && !b15.expired && b15.daysRemaining == 15 &&
              b30.active && b30.expired && b30.daysRemaining == 0 &&
              bTamper.active && bTamper.expired && bTamper.clockTampered &&
              !bOff.active && !bOff.expired &&
              FormatBetaVersionLabel(30, true) == L"2.11 Beta \x00B7 30d left" &&
              FormatBetaVersionLabel(1, false) == L"2.11 Beta (1 / 30 days left)",
              "beta: 30-day trial tracks remaining days, expires at day 30, and detects clock rollback");
    }
    {   // ---- SecureUxTheme missing/corrupted detection & Check & repair restoration
        const std::wstring suxTmp = std::wstring(tmp) + L"\\SuxTest";
        Evict(suxTmp, true, false);
        EnsureDir(suxTmp);
        const SuxVerifyResult vMissing = VerifySuxDir(suxTmp);
        // Write corrupted non-PE bytes for ThemeDll.dll
        WriteAll(suxTmp + L"\\ThemeDll.dll", reinterpret_cast<const uint8_t*>("corrupted"), 9);
        const SuxVerifyResult vBad = VerifySuxDir(suxTmp);
        // Restore from local release-inputs SUX cache if present
        std::wstring repDetail;
        const std::wstring suxInputs = L"D:\\Ankhcore\\release-inputs\\from-2.2\\SUX";
        int nRestored = 0;
        bool postOk = false;
        if (DirExists(suxInputs)) {
            nRestored = RepairSuxFiles(suxTmp, suxInputs, {}, repDetail);
            postOk = VerifySuxDir(suxTmp).ok;
        }
        Evict(suxTmp, true, false);
        Check(!vMissing.ok && vMissing.missing.size() == 3 &&
              !vBad.ok && vBad.wrongArch.size() == 1 &&
              (!DirExists(suxInputs) || (nRestored == 3 && postOk)),
              "theme: VerifySuxDir detects missing/corrupted patch files and RepairSuxFiles restores verified x64 SHA-256 files");

        // Silent ThemeDll.dll apply (2.2 direct API with 1<<8 no-hourglass and 1<<2 silent flags), delayed async activation, and CurrentTheme/VS verification
        ULONG gotF1 = 0, gotF2 = 0;
        ULONG gotIdx = 999;
        auto mockGet = [](ULONG idx, std::wstring& dn, std::wstring& vs) -> bool {
            if (idx == 0) { dn = L"Windows Dark"; vs = L"aero.msstyles"; return true; }
            if (idx == 1) { dn = L"As3aD"; vs = L"ankh Night - Arc.msstyles"; return true; }
            return false;
        };
        auto mockSetOk = [&](HWND hwnd, ULONG idx, BOOLEAN b, ULONG f1, ULONG f2) -> HRESULT {
            if (hwnd != nullptr || b != TRUE) return E_INVALIDARG;
            gotIdx = idx; gotF1 = f1; gotF2 = f2;
            return S_OK;
        };
        auto noSleep = [](DWORD) {};
        SilentApplyDiag dNoCur; std::wstring mNoCur;
        const bool okNoCur = VerifySilentApplyWithHooks(2, mockGet, mockSetOk,
            [](std::wstring& c, std::wstring& vs) { c = L"dark.theme"; vs = L"aero.msstyles"; return false; },
            dNoCur, mNoCur, 3, 1, noSleep);
        int polls = 0;
        SilentApplyDiag dDelayed; std::wstring mDelayed;
        const bool okDelayed = VerifySilentApplyWithHooks(2, mockGet, mockSetOk,
            [&](std::wstring& c, std::wstring& vs) {
                if (++polls < 3) { c = L"dark.theme"; vs = L"aero.msstyles"; return false; }
                c = L"C:\\Users\\As3aD\\AppData\\Local\\Microsoft\\Windows\\Themes\\As3aD.theme";
                vs = L"C:\\Windows\\Resources\\Themes\\Ankh\\ankh Night - Arc.msstyles";
                return IsAs3aDThemeActiveState(c, vs);
            },
            dDelayed, mDelayed, 5, 1, noSleep);
        Check(!okNoCur && dNoCur.outcome == ThemeApplyOutcome::ActivationPending &&
              okDelayed && polls == 3 && gotIdx == 1 && gotF1 == (1u << 8) && gotF2 == (1u << 2) &&
              dDelayed.outcome == ThemeApplyOutcome::AppliedAndVerified && dDelayed.currentThemeVerified && dDelayed.visualStyleVerified,
              "theme: VerifySilentApplyWithHooks handles delayed async activation, 2.2 silent flags, and CurrentTheme+VS verification");

        // Wallpaper .theme validation & synchronization + Sidebar offscreen/hostVisible checks
        const std::wstring wTmp = std::wstring(tmp) + L"\\WallThemeTest";
        Evict(wTmp, true, false);
        EnsureDir(wTmp);
        const std::string thNoWall = "[Theme]\r\nDisplayName=As3aD\r\n\r\n[VisualStyles]\r\nPath=%SystemRoot%\\resources\\Themes\\Ankh\\ankh Night - Arc.msstyles\r\n";
        const std::wstring thFile = wTmp + L"\\As3aD.theme";
        const std::wstring imgFile = wTmp + L"\\Anubis  (1).jpg";
        WriteAll(thFile, reinterpret_cast<const uint8_t*>(thNoWall.data()), thNoWall.size());
        std::wstring wd1, wd2;
        const bool wRefusedMissingImg = !EnsureAs3aDWallpaperInThemeFile(thFile, L"%SystemRoot%\\Web\\Wallpaper\\Anubis\\Anubis  (1).jpg", imgFile, wd1);
        WriteAll(imgFile, reinterpret_cast<const uint8_t*>("JPG"), 3);
        const bool wInjectedOk = EnsureAs3aDWallpaperInThemeFile(thFile, L"%SystemRoot%\\Web\\Wallpaper\\Anubis\\Anubis  (1).jpg", imgFile, wd2);
        const std::string thAfter = ReadText(thFile);
        Evict(wTmp, true, false);

        SbHealthInput sbi; sbi.wantDesktop = true; sbi.haveWindow = true; sbi.hosted = true; sbi.hostAvailable = true; sbi.parentIsHost = true; sbi.visible = true; sbi.beneathIcons = true;
        sbi.onScreen = false;
        const SbVerdict vOff = SbHealthDecide(sbi);
        sbi.onScreen = true; sbi.hostVisible = false;
        const SbVerdict vHostHid = SbHealthDecide(sbi);
        const bool rectIn = SbRectIntersectsWorkArea(1600, 0, 1920, 1040, 0, 0, 1920, 1040, 16);
        const bool rectOut = !SbRectIntersectsWorkArea(-320, 0, -5, 1040, 0, 0, 1920, 1040, 16);

        Check(wRefusedMissingImg && wInjectedOk &&
              thAfter.find("[Control Panel\\Desktop]") != std::string::npos &&
              thAfter.find("Wallpaper=%SystemRoot%\\Web\\Wallpaper\\Anubis\\Anubis  (1).jpg") != std::string::npos &&
              vOff.what == SbHealth::Restack && vHostHid.what == SbHealth::Rejoin && rectIn && rectOut,
              "stabilization: As3aD wallpaper sync validates image & updates .theme; Sidebar detects offscreen/clipped & hidden host");

        // Comprehensive Stabilization Pass tests:
        // 1. Sidebar lifecycle state machine & disappear reason
        const SbState stRun = SbComputeState(true, true, true, true, true, true, false);
        const SbState stDet = SbComputeState(true, true, true, true, false, false, false);
        const SbState stRec = SbComputeState(true, true, true, true, false, true, false);
        const SbState stFail = SbComputeState(true, false, false, true, false, false, true);
        Check(stRun == SbState::Running && stDet == SbState::TemporarilyDetached &&
              stRec == SbState::Recovering && stFail == SbState::Failed &&
              std::wstring(SbStateName(stRun)) == L"Running" &&
              std::wstring(SbDisappearReasonName(SbDisappearReason::CoveredByWorkerW)) == L"CoveredByWorkerW",
              "sidebar: SbComputeState & SbDisappearReasonName classify Running, TemporarilyDetached, Recovering, and Failed states");

        // 2. Duplicate As3aD cursor scheme deduplication & first-run default cursor check
        CurScheme sBroken; sBroken.name = L"As3aD"; sBroken.registered = true; sBroken.file[15] = L"pin.cur"; sBroken.file[16] = L"person.cur";
        CurScheme sComplete; sComplete.name = L"As3aD"; sComplete.dir = L"C:\\Windows\\Cursors\\DIM v3.2 - Prismatic [PREMIUM]";
        for (int i = 0; i < kCurRoles; ++i) sComplete.file[i] = L"role" + std::to_wstring(i) + L".cur";
        CurScheme sAlias; sAlias.name = L"As3aD2"; sAlias.registered = true;
        for (int i = 0; i < kCurRoles; ++i) sAlias.file[i] = L"role" + std::to_wstring(i) + L".cur";
        CurScheme sOther; sOther.name = L"Windows Default"; sOther.registered = true;
        for (int i = 0; i < 15; ++i) sOther.file[i] = L"aero" + std::to_wstring(i) + L".cur";
        std::vector<CurScheme> rawSchemes = { sBroken, sComplete, sAlias, sOther };
        const std::vector<CurScheme> deduped = CurFilterDuplicateNamedSchemes(rawSchemes);
        CurState stEmpty;
        CurState stPartial2; stPartial2.name = L"As3aD"; stPartial2.value[15] = L"pin.cur"; stPartial2.value[16] = L"person.cur";
        CurState stValidCustom; stValidCustom.name = L"UserCustom";
        const bool dedupOk = (deduped.size() == 2 && deduped[0].name == L"As3aD" && CurSchemeValidRoleCount(deduped[0]) == 17 && deduped[1].name == L"Windows Default");
        const bool firstRunOk = CurStateNeedsFirstRunDefault(stEmpty, 0) && CurStateNeedsFirstRunDefault(stPartial2, 2) && !CurStateNeedsFirstRunDefault(stValidCustom, 15);
        Check(dedupOk && firstRunOk,
              "cursors: CurFilterDuplicateNamedSchemes selects complete 17-role As3aD over 2-role broken/alias entries; CurStateNeedsFirstRunDefault preserves valid custom cursors");

        // 3. CPU/GPU independent fan channel calibration & thermal override preservation
        const FanChannelRole rCpu = ClassifyFanChannelRole(0, 2, L"CPU Fan");
        const FanChannelRole rGpu = ClassifyFanChannelRole(1, 2, L"Video / GPU Fan");
        const FanChannelRole rIdx0 = ClassifyFanChannelRole(0, 2, L"System Fan");
        const FanChannelRole rIdx1 = ClassifyFanChannelRole(1, 2, L"System Fan");
        FanChannelCalibration cal{ 0, 10 };
        const int dCpu60 = ComputeCalibratedFanDuty(60, FanChannelRole::Cpu, cal, false);
        const int dGpu60 = ComputeCalibratedFanDuty(60, FanChannelRole::Gpu, cal, false);
        const int dHotCpu = ComputeCalibratedFanDuty(60, FanChannelRole::Cpu, cal, true);
        const int dHotGpu = ComputeCalibratedFanDuty(60, FanChannelRole::Gpu, cal, true);
        std::vector<std::wstring> cmdsSent;
        auto mockRunner = [&](const std::wstring& args, std::wstring& out) -> bool {
            cmdsSent.push_back(args);
            out = L"OK";
            return true;
        };
        const std::vector<std::wstring> fanNames = { L"CPU Fan", L"GPU Fan" };
        const FanOpResult resCal = FanSetSpeedCalibratedSafe(60, 2, fanNames, cal, mockRunner, true);
        Check(rCpu == FanChannelRole::Cpu && rGpu == FanChannelRole::Gpu &&
              rIdx0 == FanChannelRole::Cpu && rIdx1 == FanChannelRole::Gpu &&
              dCpu60 == 60 && dGpu60 == 70 && dHotCpu == 100 && dHotGpu == 100 &&
              resCal.ok && cmdsSent.size() == 2 &&
              cmdsSent[0].find(L"set --speed 60 --fan 0") != std::wstring::npos &&
              cmdsSent[1].find(L"set --speed 70 --fan 1") != std::wstring::npos,
              "fan: independent CPU/GPU channel calibration applies per-channel duty offsets and preserves 92C thermal override");

        // 4. Dual-fan FanSetSpeedCalibratedSafe with 0/0 offsets still dispatches to BOTH --fan 0 and --fan 1
        std::vector<std::wstring> zeroCalCmds;
        auto mockZeroRunner = [&](const std::wstring& args, std::wstring& out) -> bool {
            zeroCalCmds.push_back(args);
            out = L"OK";
            return true;
        };
        FanChannelCalibration zeroCal{ 0, 0 };
        const FanOpResult resZeroCal = FanSetSpeedCalibratedSafe(70, 2, fanNames, zeroCal, mockZeroRunner, true);
        Check(resZeroCal.ok && zeroCalCmds.size() == 2 &&
              zeroCalCmds[0] == L"set --speed 70 --fan 0" &&
              zeroCalCmds[1] == L"set --speed 70 --fan 1",
              "fan: FanSetSpeedCalibratedSafe commands both --fan 0 and --fan 1 even at 0% offset so GPU never lags CPU");

        // 5. SecureUxTheme installer hook evaluation & restart detection
        const SuxFullStatus suxActive = EvaluateSuxStatusFromFacts(true, true, 0x27, false, false, true, true);
        const SuxFullStatus suxPending = EvaluateSuxStatusFromFacts(true, true, 0x21, false, false, true, true);
        const SuxFullStatus suxMisconf = EvaluateSuxStatusFromFacts(true, true, 0x2F, true, false, true, true);
        Check(suxActive.installed && suxActive.loadedInSession && !suxActive.explorerHooked && !suxActive.systemSettingsHooked &&
              suxActive.logonUiHooked && suxActive.defaultColorsRemoved && suxActive.configurationMatches && !suxActive.restartRequired &&
              suxActive.activationState == SuxActivationState::LoadedAndActive &&
              suxPending.installed && !suxPending.loadedInSession && suxPending.configurationMatches && suxPending.restartRequired &&
              suxPending.activationState == SuxActivationState::InstalledPendingRestart &&
              !suxMisconf.configurationMatches && suxMisconf.activationState == SuxActivationState::ConfiguredPendingRestart,
              "sux: EvaluateSuxStatusFromFacts verifies LogonUI+DefaultColors configuration, unhooked Explorer/SystemSettings, and restart detection");

        // 6. Authoritative Windows Themes discovery & THEMETOOLS/As3aD.theme payload routing
        std::wstring ttDest;
        const bool ttDestOk = PayloadDest(L"THEMETOOLS/As3aD.theme", L"C:\\Program Files\\AnkhCore", ttDest) &&
                              ttDest == L"C:\\Program Files\\AnkhCore\\Themetools\\As3aD.theme";
        if (DirExists(L"D:\\Ankhcore\\windows\\Resources\\Themes")) PackageAssetsRoot() = L"D:\\Ankhcore\\windows\\Resources";   // the themes come from the package folder (2.8); here the developer copy stands in for it
        const std::vector<DiscoveredWinTheme> authThemes = DiscoverAuthoritativeThemes();
        bool allValid = !authThemes.empty(), noAs3aDMixed = true;
        for (const auto& th : authThemes) {
            allValid &= (th.valid && th.vsValid && th.wallpaperValid);
            noAs3aDMixed &= (Lower(th.id).rfind(L"as3ad", 0) != 0);
        }
        Check(ttDestOk && (!DirExists(L"D:\\Ankhcore\\windows\\Resources\\Themes") || (authThemes.size() == 22 && allValid && noAs3aDMixed)),
              "theme-source: THEMETOOLS/As3aD.theme routes to Themetools\\ and DiscoverAuthoritativeThemes finds all 22 BIB/BIB3 themes without mixing As3aD");

        // 7. Windhawk grouped collapsible mod categories
        Check(ClassifyWindhawkModCategory(L"local@anubis-windows-animations", L"Anubis Windows Animations") == WhModCategory::Effects &&
              ClassifyWindhawkModCategory(L"mouse-trail", L"Mouse Trail") == WhModCategory::Effects &&
              ClassifyWindhawkModCategory(L"local@translucent-windows", L"Translucent Windows") == WhModCategory::TransparencyGlass &&
              ClassifyWindhawkModCategory(L"windows-11-file-explorer-styler", L"Windows 11 File Explorer Styler") == WhModCategory::ExplorerDesktop &&
              ClassifyWindhawkModCategory(L"windows-11-start-menu-styler", L"Windows 11 Start Menu Styler") == WhModCategory::TaskbarStartMenu &&
              ClassifyWindhawkModCategory(L"taskbar-clock-customization", L"Taskbar Clock Customization") == WhModCategory::TaskbarStartMenu &&
              ClassifyWindhawkModCategory(L"local@icon-resource-redirect", L"Icon Resource Redirect") == WhModCategory::SystemInterface &&
              WhCategorySpecs().size() == 5,
              "windhawk: ClassifyWindhawkModCategory assigns mods to Effects, Transparency & Glass, Explorer & Desktop, Taskbar & Start Menu, and System & Interface");
    }
    {   // ---- 2.6 Unified Backup & Recovery Foundation (M-2.6-BAK1)
        // 1. Data path safety: excludes binaries, scripts, raw hives, secrets, credentials, tokens, cookies, DPAPI
        std::wstring why;
        Check(AcbCheckDataPathSafety("AppData/AppA/config.json", why) == AcbDataPathSafety::Safe &&
              AcbCheckDataPathSafety("AppData/AppA/prefs.ini", why) == AcbDataPathSafety::Safe &&
              AcbCheckDataPathSafety("../escape.json", why) == AcbDataPathSafety::UnsafePath &&
              AcbCheckDataPathSafety("AppData/AppA/app.exe", why) == AcbDataPathSafety::ForbiddenBinaryOrExecutable &&
              AcbCheckDataPathSafety("AppData/AppA/driver.sys", why) == AcbDataPathSafety::ForbiddenBinaryOrExecutable &&
              AcbCheckDataPathSafety("AppData/AppA/export.reg", why) == AcbDataPathSafety::ForbiddenBinaryOrExecutable &&
              AcbCheckDataPathSafety("AppData/AppA/setup.ps1", why) == AcbDataPathSafety::ForbiddenBinaryOrExecutable &&
              AcbCheckDataPathSafety("AppData/AppA/cert.pfx", why) == AcbDataPathSafety::ForbiddenSecretOrCredential &&
              AcbCheckDataPathSafety("AppData/AppA/id_rsa", why) == AcbDataPathSafety::ForbiddenSecretOrCredential &&
              AcbCheckDataPathSafety("AppData/Browser/Cookies", why) == AcbDataPathSafety::ForbiddenSecretOrCredential &&
              AcbCheckDataPathSafety("AppData/AppA/oauth_token.json", why) == AcbDataPathSafety::ForbiddenSecretOrCredential &&
              AcbCheckDataPathSafety("AppData/Windows/DPAPI/masterkey", why) == AcbDataPathSafety::ForbiddenSecretOrCredential,
              "2.6 bak: AcbCheckDataPathSafety allows safe configs and rejects traversal, binaries/drivers/reg, and secrets/tokens/cookies");

        // 2. 5-class application restoration classification & v2 inventory JSON round-trip
        std::vector<AcbAppInventoryEntry26> inv = {
            {"", "PortableTool", "1.2.0", "Local PC", "manual", false, true, true, AcbAppRestoreClass::PortableAppAndData},
            {"7zip.7zip", "7-Zip", "24.08", "winget", "Winget", true, false, true, AcbAppRestoreClass::InstalledAppReadyForData},
            {"VideoLAN.VLC", "VLC media player", "3.0.21", "winget", "Winget", true, false, false, AcbAppRestoreClass::PackageManagerReinstall},
            {"", "CustomVendorApp", "2.0", "Local PC", "manual", false, false, true, AcbAppRestoreClass::ManualInstallRequired},
            {"", "", "", "", "", false, false, false, AcbAppRestoreClass::UnsupportedOrUnknown}
        };
        const std::string inv26Json = AcbBuildAppInventoryJson26(inv);
        std::vector<AcbAppInventoryEntry26> parsedInv;
        std::wstring invErr;
        Check(AcbParseAppInventoryJson26(inv26Json, parsedInv, invErr) && parsedInv.size() == 5 &&
              parsedInv[0].restoreClass == AcbAppRestoreClass::PortableAppAndData &&
              parsedInv[4].restoreClass == AcbAppRestoreClass::UnsupportedOrUnknown,
              "2.6 bak: AcbBuildAppInventoryJson26 and AcbParseAppInventoryJson26 round-trip all 5 restoration classes");

        const std::vector<AcbPackage> installedPc = {{"7zip.7zip", "7-Zip", "24.08", "winget", "Winget"}};
        const std::vector<AcbPackage> catalogPc = {{"VideoLAN.VLC", "VLC media player", "3.0.20", "winget", "Winget"}};
        const AcbAppRestorePlan26 plan26 = AcbPlanAppRestore26(parsedInv, installedPc, catalogPc, {"Winget"});
        Check(plan26.portableCount == 1 && plan26.installedCount == 1 && plan26.packageManagerCount == 1 &&
              plan26.manualCount == 1 && plan26.unsupportedCount == 1 &&
              plan26.items[0].restoreClass == AcbAppRestoreClass::PortableAppAndData &&
              plan26.items[1].restoreClass == AcbAppRestoreClass::InstalledAppReadyForData &&
              plan26.items[2].restoreClass == AcbAppRestoreClass::PackageManagerReinstall && plan26.items[2].versionMismatch &&
              plan26.items[3].restoreClass == AcbAppRestoreClass::ManualInstallRequired &&
              plan26.items[4].restoreClass == AcbAppRestoreClass::UnsupportedOrUnknown,
              "2.6 bak: AcbPlanAppRestore26 classifies Portable, Installed, PackageManager (with version mismatch), Manual, and Unsupported");

        // 3. Mapped registry/tweak settings safety, JSON round-trip & OS/hardware compatibility checks
        AcbMappedRegValue twWin11;
        twWin11.tweakId = "win11-taskbar-align"; twWin11.title = "Taskbar alignment"; twWin11.category = "Visual Effects";
        twWin11.root = "HKCU"; twWin11.subkey = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
        twWin11.valueName = "TaskbarAl"; twWin11.regType = REG_DWORD; twWin11.existedBefore = true;
        twWin11.numValue = 0; twWin11.origNumValue = 1; twWin11.minOsBuild = 22000; twWin11.requiresWindows11 = true;

        AcbMappedRegValue twGeneral;
        twGeneral.tweakId = "verbose-status"; twGeneral.title = "Verbose status"; twGeneral.category = "System";
        twGeneral.root = "HKLM"; twGeneral.subkey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
        twGeneral.valueName = "VerboseStatus"; twGeneral.regType = REG_DWORD; twGeneral.existedBefore = false;
        twGeneral.numValue = 1;

        AcbMappedRegValue twDevice;
        twDevice.tweakId = "zbook-profile"; twDevice.title = "Device-specific"; twDevice.category = "Power";
        twDevice.root = "HKCU"; twDevice.subkey = "Software\\AnkhCore\\Profile";
        twDevice.valueName = "Mode"; twDevice.regType = REG_SZ; twDevice.strValue = "Work";
        twDevice.requiredDeviceModel = "HP ZBook 15 G5";

        AcbMappedRegValue twBadSvc = twGeneral;
        twBadSvc.subkey = "SYSTEM\\CurrentControlSet\\Services\\wuauserv";
        AcbMappedRegValue twBadCs2 = twGeneral;
        twBadCs2.subkey = "SYSTEM\\ControlSet002\\Services\\wuauserv";
        AcbMappedRegValue twBadDblSlash = twGeneral;
        twBadDblSlash.subkey = "SYSTEM\\\\CurrentControlSet\\\\Services\\\\wuauserv";
        AcbMappedRegValue twBadDword = twGeneral;
        twBadDword.numValue = 0x100000000ull;
        AcbMappedRegValue twBadRegType = twGeneral;
        twBadRegType.regType = REG_BINARY;
        std::wstring regErr;
        Check(AcbIsSafeMappedRegEntry(twWin11, regErr) &&
              !AcbIsSafeMappedRegEntry(twBadSvc, regErr) &&
              !AcbIsSafeMappedRegEntry(twBadCs2, regErr) &&
              !AcbIsSafeMappedRegEntry(twBadDblSlash, regErr) &&
              !AcbIsSafeMappedRegEntry(twBadDword, regErr) &&
              !AcbIsSafeMappedRegEntry(twBadRegType, regErr),
              "2.6 bak: AcbIsSafeMappedRegEntry permits safe mapped values and rejects protected Services/ControlSet keys, double slashes, 32-bit overflow, and raw REG_BINARY");

        const std::string twJson = AcbBuildTweakBackupJson({twWin11, twGeneral, twDevice});
        std::vector<AcbMappedRegValue> parsedTw;
        Check(AcbParseTweakBackupJson(twJson, parsedTw, regErr) && parsedTw.size() == 3 &&
              parsedTw[0].regType == REG_DWORD && parsedTw[0].requiresWindows11 && parsedTw[2].regType == REG_SZ,
              "2.6 bak: AcbBuildTweakBackupJson and AcbParseTweakBackupJson preserve exact types and OS/device constraints");

        // 4. Non-destructive AcbPreviewRestore + OS/hardware compatibility report
        AcbBuildInput pvIn;
        pvIn.kind = "full"; pvIn.created = "2026-11-18T12:00:00Z";
        pvIn.appVersion = "2.6.0.0"; pvIn.appLabel = "2.6 Dev";
        pvIn.osName = "Windows 11"; pvIn.osBuild = 26300; pvIn.arch = "x64";
        pvIn.items.push_back({"settings", "settings.json", "appdata:settings.json",
                              std::vector<uint8_t>{'{', '"', 'n', 'a', 'v', '.', 'p', 'a', 'g', 'e', '"', ':', '1', '}'}});
        pvIn.items.push_back({"tweaks", "tweaks.json", "none:", std::vector<uint8_t>(twJson.begin(), twJson.end())});
        pvIn.items.push_back({"apps", "inventory.json", "none:", std::vector<uint8_t>(inv26Json.begin(), inv26Json.end())});

        std::vector<uint8_t> pvArchive;
        std::wstring pvErr;
        Check(AcbBuild(pvIn, pvArchive, pvErr), "2.6 bak: AcbBuild packages settings, tweaks, and v2 apps inventory");

        wchar_t tmp26[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tmp26);
        const std::wstring pvRootDir = std::wstring(tmp26) + L"ankh_26_preview_" + std::to_wstring(GetCurrentProcessId());
        Evict(pvRootDir, false, false);
        SHCreateDirectoryExW(nullptr, (pvRootDir + L"\\AppData").c_str(), nullptr);
        const AcbRoots pvRoots{pvRootDir + L"\\AppData", pvRootDir + L"\\Data", pvRootDir + L"\\Content"};

        AcbHostCompatibilityContext win10Host;
        win10Host.osBuild = 19045; win10Host.arch = "x64"; win10Host.deviceModel = "Other Laptop";
        const AcbRestorePreview pv = AcbPreviewRestore(pvArchive.data(), pvArchive.size(), pvRoots, win10Host);
        Check(pv.ok && pv.diskWriteCount == 1 && pv.referenceOnlyCount == 2 &&
              !FileExists(pvRootDir + L"\\AppData\\settings.json") &&
              pv.compatibility.archCompatible && pv.compatibility.osGenerationChanged &&
              pv.compatibility.compatibleTweaks.size() == 1 && pv.compatibility.skippedTweaks.size() == 2 &&
              pv.appInventory.size() == 5,
              "2.6 bak: AcbPreviewRestore is non-destructive, parses inventory, and filters Win11/device-specific tweaks on Win10 host");

        // Tampered archive must be refused by AcbPreviewRestore
        std::vector<uint8_t> badPv = pvArchive;
        if (badPv.size() > 80) badPv[50] ^= 0xFF;
        const AcbRestorePreview pvBad = AcbPreviewRestore(badPv.data(), badPv.size(), pvRoots, win10Host);
        Check(!pvBad.ok && !pvBad.error.empty(), "2.6 bak: AcbPreviewRestore rejects tampered archive");
        Evict(pvRootDir, false, false);
    }
    {   // ---- M-2.6-UPD1: updater, pre-update backup, verification, rollback (mock provider only; no live command, no registry)
        // Every Check below is ONE condition with its own name, so a failure identifies the exact scenario.
        // Tags [R1]..[R5] = the five defects confirmed by ANKHCORE_2.6_UPD1_INDEPENDENT_REVIEW.md, [S] = its smaller findings.
        const std::wstring updRoot = std::wstring(tmp) + L"ankhcore_test_upd1_" + std::to_wstring(GetCurrentProcessId());
        const std::string settingsBody = "{\"sidebar.width\":300,\"appearance.theme\":\"As3aD\"}\r\n";
        auto freshRoots = [&](const wchar_t* name, bool withSettings) {
            const std::wstring d = updRoot + L"\\" + name;
            Evict(d, true, false);
            AcbRoots rt{d + L"\\AppData", d + L"\\Data", d + L"\\Content"};
            EnsureDir(rt.appdata); EnsureDir(rt.data); EnsureDir(rt.content);
            if (withSettings) WriteAtomic(rt.appdata + L"\\settings.json", reinterpret_cast<const uint8_t*>(settingsBody.data()), settingsBody.size());
            return rt;
        };
        auto recoveryDirs = [&](const AcbRoots& rt) { return DirExists(rt.data + L"\\Recovery") ? ListDir(rt.data + L"\\Recovery", true).size() : size_t(0); };
        const DWORD kDeadPid = 0x7FFFFF00;   // no such process
        auto writeRaw = [&](const std::wstring& path, const std::string& text) { EnsureDir(DirOf(path)); WriteAtomic(path, reinterpret_cast<const uint8_t*>(text.data()), text.size()); };

        // ---- strict recovery point (BAK1 weakness: a missing settings.json used to give a "successful" empty point)
        {
            const AcbRoots rt = freshRoots(L"rp_none", false);
            const AcbRecoveryPointResult plain = AcbCreateRecoveryPointEx(rt, L"legacy form", AcbRecoveryPointOptions{false, false});
            Check(plain.ok, "upd1 rp: the plain (legacy) recovery point still succeeds without settings.json");
            Check(!plain.hasContent, "upd1 rp: the plain recovery point reports hasContent=false instead of hiding that it is empty");
            const AcbRoots rt2 = freshRoots(L"rp_none2", false);
            const AcbRecoveryPointResult strict = AcbCreateVerifiedRecoveryPoint(rt2, L"no settings", false);
            Check(!strict.ok && !strict.verified && !strict.sealed, "upd1 rp: a missing settings.json never yields a verified, sealed recovery point");
            Check(strict.error.find(L"no meaningful recovery point") != std::wstring::npos, "upd1 rp: the refusal names the reason");
            Check(recoveryDirs(rt2) == 0, "upd1 rp: a refused recovery point leaves no empty folder behind");
        }
        {
            const AcbRoots rt = freshRoots(L"rp_ok", true);
            const AcbRecoveryPointResult rp = AcbCreateVerifiedRecoveryPoint(rt, L"update test", false);
            size_t n = 0; std::wstring e;
            Check(rp.ok && rp.verified && rp.sealed, "upd1 rp: create + verify + seal + re-verify all pass for a real settings.json");
            Check(rp.items == 1 && AcbVerifyRecoveryPoint(rp.dir, true, n, e) && n == 1, "upd1 rp: the sealed point re-verifies from disk with exactly one saved file");
            uint64_t sz = 0; std::string sha, want;
            AcbHashFile(rp.dir + L"\\files\\settings\\settings.json", sz, sha);
            AcbHashFile(rt.appdata + L"\\settings.json", sz, want);
            Check(!sha.empty() && sha == want, "upd1 rp: the recovery copy is byte-identical to the live settings.json");
        }
        {
            const AcbRoots rt = freshRoots(L"rp_bad", true);
            const AcbRecoveryPointResult rp = AcbCreateVerifiedRecoveryPoint(rt, L"corrupt", false, [](const std::wstring& dir) {
                std::vector<uint8_t> b;
                if (ReadAll(dir + L"\\files\\settings\\settings.json", b) && !b.empty()) { b[2] ^= 0x55; WriteAtomic(dir + L"\\files\\settings\\settings.json", b.data(), b.size()); }
            });
            Check(!rp.ok && !rp.sealed && rp.error.find(L"damaged") != std::wstring::npos, "upd1 rp: a copy corrupted after sealing is detected");
            Check(DirExists(rp.dir) && FileExists(rp.dir + L"\\recovery-manifest.json"), "upd1 rp: a point that failed verification is kept on disk, not deleted");
        }
        {
            const AcbRoots rt = freshRoots(L"rp_lock", true);
            HANDLE hold = CreateFileW((rt.appdata + L"\\settings.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            const AcbRecoveryPointResult rp = AcbCreateVerifiedRecoveryPoint(rt, L"locked", false);
            if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
            Check(hold != INVALID_HANDLE_VALUE, "upd1 rp: test set-up held settings.json open");
            Check(!rp.ok && rp.error.find(L"could not be copied") != std::wstring::npos, "upd1 rp: an existing settings.json that cannot be copied is a failure, never a silent empty point");
        }
        {   // [S3] a verifier must not call a point with nothing saved "verified"
            const AcbRoots rt = freshRoots(L"rp_empty", true);
            const std::wstring dir = rt.data + L"\\Recovery\\empty1";
            EnsureDir(dir);
            AcbRecManifest m; m.formatVersion = 2; m.kind = "pre-change"; m.created = AcbUtcStampNow();
            AcbRecItem it; it.dest = "appdata:settings.json"; it.existed = false; it.strategy = "settings-merge3"; m.items.push_back(it);
            AcbWriteRecoveryManifest(dir, m);
            size_t n = 0; std::wstring e;
            Check(!AcbVerifyRecoveryPoint(dir, false, n, e), "upd1 [S3]: AcbVerifyRecoveryPoint rejects a point whose items saved no file");
            const std::wstring dir2 = rt.data + L"\\Recovery\\empty2";
            EnsureDir(dir2);
            AcbRecManifest none; none.formatVersion = 2; none.kind = "pre-change"; none.created = AcbUtcStampNow();
            AcbWriteRecoveryManifest(dir2, none);
            Check(!AcbVerifyRecoveryPoint(dir2, false, n, e), "upd1 [S3]: AcbVerifyRecoveryPoint rejects a point with no items");
        }

        // ---- pure logic: versions
        Check(UpdCompareVersions("1.2.3", "1.2.10") == -1, "upd1 ver: 1.2.3 < 1.2.10 (numeric, not text)");
        Check(UpdCompareVersions("2.0", "2.0.0") == 0, "upd1 ver: 2.0 == 2.0.0");
        Check(UpdCompareVersions("v3.1", "3.0.9") == 1, "upd1 ver: a leading v is ignored");
        Check(UpdCompareVersions("", "1.0") == -2, "upd1 ver: an empty version cannot be ordered");
        Check(UpdCompareVersions("abc", "abc") == 0, "upd1 ver: identical non-numeric text is equal");
        Check(UpdCompareVersions("abc", "abd") == -2, "upd1 ver: different non-numeric text cannot be ordered");
        Check(UpdCompareVersions("1.0-beta", "1.0") == -1, "upd1 [R4] ver: a pre-release (1.0-beta) is older than its release (1.0)");
        Check(UpdCompareVersions("24.08", "24.09-hotfix") == -1, "upd1 [R4] ver: a numeric core decides before any suffix (24.08 < 24.09-hotfix)");
        Check(UpdCompareVersions("1.0-beta", "1.1.0") == -1, "upd1 [R4] ver: 1.0-beta < 1.1.0");
        Check(UpdCompareVersions("24.08.1-x64", "24.08.1") == -2, "upd1 [R4] ver: an unknown suffix with an equal core is not ordered");
        {
            std::vector<unsigned long long> parts;
            Check(UpdParseVersion("24.08.1-x64", parts), "upd1 [R4] ver: a numeric core with a suffix parses");
            Check(UpdParseVersion("2024.1a", parts), "upd1 [R4] ver: 2024.1a parses (core 2024.1)");
            Check(UpdParseVersion("1.2.3.4.5.6.7.8.9", parts), "upd1 [R4] ver: more than 8 numeric parts parse");
            Check(!UpdParseVersion("beta", parts) && !UpdParseVersion("", parts) && !UpdParseVersion("v", parts), "upd1 ver: text without a numeric core does not parse");
        }

        // ---- pure logic: tokens, args, policy
        Check(UpdSafeToken("Microsoft.PowerToys", false), "upd1 tok: a normal package id is accepted");
        Check(UpdSafeToken("", true) && !UpdSafeToken("", false), "upd1 tok: empty is accepted only where optional");
        Check(!UpdSafeToken("a b", false) && !UpdSafeToken("x\"y", false) && !UpdSafeToken("x&calc", false), "upd1 tok: spaces, quotes and shell characters are rejected");
        Check(!UpdSafeToken("..\\x", false), "upd1 tok: path traversal is rejected");
        Check(!UpdSafeToken("--skip-hash", false) && !UpdSafeToken("-x", false), "upd1 [S1] tok: an identifier beginning with '-' (an option lookalike) is rejected");
        {
            UpdPackage dash; dash.id = "--skip-hash"; dash.manager = "winget";
            std::wstring e;
            Check(!UpdValidatePackage(dash, e), "upd1 [S1] tok: UpdValidatePackage refuses an id beginning with '-'");
            UpdPackage dashMgr; dashMgr.id = "a"; dashMgr.manager = "-winget";
            Check(!UpdValidatePackage(dashMgr, e), "upd1 [S1] tok: UpdValidatePackage refuses a manager beginning with '-'");
        }
        UpdPackage pk; pk.id = "7zip.7zip"; pk.name = "7-Zip"; pk.manager = "winget"; pk.source = "winget"; pk.targetVersion = "1.1.0";
        {
            UpdPackage p = pk; p.previousVersion = "24.07"; p.targetVersion = "24.08";
            const std::wstring a = UpdBuildUniGetUpdateArgs(p);
            Check(a.rfind(L"package update --id \"7zip.7zip\" --manager \"winget\"", 0) == 0, "upd1 args: the update command line has the documented shape");
            Check(a.find(L"--wait true") != std::wstring::npos, "upd1 args: the update waits for completion");
            Check(a.find(L"--skip-hash") == std::wstring::npos && a.find(L"--elevated") == std::wstring::npos && a.find(L"--interactive") == std::wstring::npos,
                  "upd1 args: never --skip-hash, --elevated or --interactive");
            std::wstring why;
            UpdPackage minor = p, major = p, unk = p, hi = p;
            major.targetVersion = "25.01"; unk.targetVersion.clear(); hi.highImpact = true;
            Check(UpdBackupRequired(UpdBackupPolicy::Always, minor, why), "upd1 policy: Always requires a backup");
            Check(!UpdBackupRequired(UpdBackupPolicy::HighImpactOnly, minor, why), "upd1 policy: HighImpactOnly skips a minor update");
            Check(UpdBackupRequired(UpdBackupPolicy::HighImpactOnly, major, why), "upd1 policy: HighImpactOnly backs up a major update");
            Check(UpdBackupRequired(UpdBackupPolicy::HighImpactOnly, unk, why), "upd1 policy: HighImpactOnly backs up when the target is unknown");
            Check(UpdBackupRequired(UpdBackupPolicy::HighImpactOnly, hi, why), "upd1 policy: HighImpactOnly backs up a high-impact package");
        }
        // (the whitespace-table parser that stood here was a guess about an unseen format; it was replaced by the JSON parser tested in the [B1] block below)
        Check(!UpdLiveExecutionAllowed(true), "upd1 live: the live provider refuses to run in the isolated test build");
        Check(UpdLiveExecutionAllowed(false), "upd1 live: and is allowed in a normal build");
        {   // [R5] exit reasons for the two non-exit-code results of RunCapture/Run
            Check(UpdExitReason(kRunTimedOut).find(L"timed out") != std::wstring::npos, "upd1 [R5] exit: kRunTimedOut is explained as a timeout, not as an exit code");
            Check(UpdExitReason(kRunLaunchFailed).find(L"could not be run") != std::wstring::npos, "upd1 [R5] exit: kRunLaunchFailed is explained as a launch failure");
        }

        // ---- pure logic: result classification (exit code alone never means success)
        {
            UpdPackage p; p.id = "a"; p.manager = "winget"; p.previousVersion = "1.0.0"; p.targetVersion = "1.1.0";
            Check(UpdClassifyResult(p, 0, true, "1.1.0").outcome == UpdOutcome::Succeeded, "upd1 class: exit 0 and the target version = Succeeded");
            const UpdVerdict same0 = UpdClassifyResult(p, 0, true, "1.0.0");
            Check(same0.outcome == UpdOutcome::Failed && !same0.versionChanged, "upd1 class: exit 0 but the version did not change = Failed");
            Check(UpdClassifyResult(p, 0, false, "").outcome == UpdOutcome::PartiallySucceeded, "upd1 class: exit 0 and an unconfirmable version = Partially succeeded");
            Check(UpdClassifyResult(p, 1, true, "1.1.0").outcome == UpdOutcome::PartiallySucceeded, "upd1 class: exit 1 but the version advanced = Partially succeeded");
            Check(UpdClassifyResult(p, 1, true, "1.0.0").outcome == UpdOutcome::Failed, "upd1 class: exit 1 and no change = Failed");
            const UpdVerdict mid = UpdClassifyResult(p, 0, true, "1.0.5");
            Check(mid.outcome == UpdOutcome::PartiallySucceeded && !mid.needsRollback, "upd1 class: newer than before but short of the target = Partially succeeded, no rollback");
            const UpdVerdict low = UpdClassifyResult(p, 0, true, "0.9.0");
            Check(low.outcome == UpdOutcome::Failed && low.needsRollback, "upd1 class: a confirmed LOWER version than before = Failed, rollback needed");
            const UpdVerdict ipc = UpdClassifyResult(p, 3, true, "1.0.0");
            Check(ipc.outcome == UpdOutcome::Failed && ipc.reason.find(L"no UniGetUI session") != std::wstring::npos, "upd1 class: UniGetUI exit 3 = Failed, no session");
            UpdPackage latest = p; latest.targetVersion.clear();
            Check(UpdClassifyResult(latest, 0, true, "2.0.0").outcome == UpdOutcome::Succeeded, "upd1 class: 'latest' with a newer version = Succeeded");
            // [R4]
            UpdPackage beta = latest; beta.previousVersion = "1.0-beta";
            const UpdVerdict r4a = UpdClassifyResult(beta, 0, true, "1.1.0");
            Check(r4a.outcome == UpdOutcome::Succeeded, "upd1 [R4] class: prev 1.0-beta, latest, after 1.1.0 = Succeeded (was Failed)");
            Check(!r4a.needsRollback, "upd1 [R4] class: ... and does NOT ask for a rollback");
            UpdPackage hot = latest; hot.previousVersion = "24.08";
            const UpdVerdict r4b = UpdClassifyResult(hot, 0, true, "24.09-hotfix");
            Check(r4b.outcome == UpdOutcome::Succeeded && !r4b.needsRollback, "upd1 [R4] class: 24.08 -> 24.09-hotfix with 'latest' = Succeeded, no rollback");
            const UpdVerdict newer = UpdClassifyResult(p, 0, true, "1.2.0");
            Check(newer.outcome == UpdOutcome::PartiallySucceeded && !newer.needsRollback, "upd1 [R4] class: newer than the requested target = Partially succeeded, no rollback");
            const UpdVerdict odd = UpdClassifyResult(p, 0, true, "zeta");
            Check(odd.outcome == UpdOutcome::PartiallySucceeded && !odd.needsRollback && odd.versionChanged, "upd1 [R4] class: a changed but unorderable version = Partially succeeded, no rollback");
        }

        // ---- full runs through the mock provider
        struct Mock {
            std::string installed = "1.0.0", afterUpdate = "1.1.0", afterReinstall = "1.0.0";
            DWORD updateExit = 0, installExit = 0;
            bool readable = true, unreadableAfterUpdate = false;
            std::vector<std::wstring> cmds;
            int updates = 0, installs = 0;
            bool recoveryBeforeUpdate = false;
        };
        auto mkCtx = [&](Mock& m, const AcbRoots& rt) {
            UpdContext c; c.roots = rt; c.journal = false;
            c.provider.uniGetAvailable = true;
            c.provider.run = [&m, rt](const std::wstring& a, std::vector<std::wstring>&) -> DWORD {
                m.cmds.push_back(a);
                if (a.rfind(L"package update", 0) == 0) {
                    ++m.updates;
                    m.recoveryBeforeUpdate = DirExists(rt.data + L"\\Recovery") && !ListDir(rt.data + L"\\Recovery", true).empty();
                    m.installed = m.afterUpdate;
                    if (m.unreadableAfterUpdate) m.readable = false;
                    return m.updateExit;
                }
                if (a.rfind(L"package install", 0) == 0) { ++m.installs; m.installed = m.afterReinstall; return m.installExit; }
                return 0;
            };
            c.provider.installedVersion = [&m](const UpdPackage&, std::string& v) { if (!m.readable) return false; v = m.installed; return true; };
            return c;
        };
        {   // success
            const AcbRoots rt = freshRoots(L"run_ok", true);
            Mock m; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            uint64_t sz = 0; std::string sha, live;
            AcbHashFile(r.recoveryDir + L"\\files\\settings\\settings.json", sz, sha);
            AcbHashFile(rt.appdata + L"\\settings.json", sz, live);
            Check(r.outcome == UpdOutcome::Succeeded, "upd1 run: a verified version advance = Succeeded");
            Check(r.backupRequired && r.backupCreated && r.backupVerified && r.backupSealed, "upd1 run: the recovery point was created, verified and sealed");
            Check(m.updates == 1 && m.recoveryBeforeUpdate, "upd1 run: the recovery point existed on disk before the update command ran");
            Check(r.versionBefore == "1.0.0" && r.versionAfter == "1.1.0" && r.versionAfterConfirmed, "upd1 run: before/after versions were read back");
            Check(!sha.empty() && sha == live, "upd1 run: settings.json is unchanged and equals its recovery copy");
            Check(!FileExists(UpdActivePath(rt)), "upd1 run: the active-update lock is released after a finished update");
            Check(DirExists(UpdHistoryDir(rt)) && !ListDir(UpdHistoryDir(rt), false).empty(), "upd1 run: the finished update is recorded in history");
            Check(FileExists(r.recoveryDir + L"\\update-report.txt"), "upd1 run: a readable report sits beside the recovery point");
            Check(r.rollback.capability == UpdRollbackCapability::Partial && r.rollback.canReinstallPrevious, "upd1 run: rollback is reported Partial (reinstall possible)");
            Check(!r.rollback.canRestoreAppData && !r.appDataBackedUp, "upd1 run: application data is reported as NOT restorable (no data map)");
        }
        {
            const AcbRoots rt = freshRoots(L"run_same", true);
            Mock m; m.afterUpdate = "1.0.0"; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::Failed && r.commandRan, "upd1 run: exit 0 with an unchanged version = Failed");
            Check(r.rollbackResult == UpdRollbackResult::NotNeeded && m.installs == 0, "upd1 run: ... and nothing is rolled back");
        }
        {
            const AcbRoots rt = freshRoots(L"run_unconf", true);
            Mock m; m.unreadableAfterUpdate = true; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::PartiallySucceeded && !r.versionAfterConfirmed, "upd1 run: exit 0 with an unconfirmable version = Partially succeeded");
        }
        {
            const AcbRoots rt = freshRoots(L"run_adv1", true);
            Mock m; m.updateExit = 1; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::PartiallySucceeded && r.verdict.reachedTarget, "upd1 run: a failing exit code with an advanced version = Partially succeeded");
        }
        {
            const AcbRoots rt = freshRoots(L"run_ipc", true);
            Mock m; m.updateExit = 3; m.afterUpdate = "1.0.0"; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::Failed && r.verdict.reason.find(L"no UniGetUI session") != std::wstring::npos, "upd1 run: UniGetUI exit 3 = Failed, unchanged");
        }
        {
            const AcbRoots rt = freshRoots(L"run_nobak", false);
            Mock m; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::AbortedBeforeUpdate, "upd1 run: no meaningful backup = aborted before updating");
            Check(m.updates == 0 && !r.commandRan, "upd1 run: ... and no update command was run");
            Check(!FileExists(UpdActivePath(rt)), "upd1 run: ... and the lock is released");
        }
        {
            const AcbRoots rt = freshRoots(L"run_corrupt", true);
            Mock m; UpdContext c = mkCtx(m, rt);
            c.afterBackupSeal = [](const std::wstring& dir) {
                std::vector<uint8_t> b;
                if (ReadAll(dir + L"\\files\\settings\\settings.json", b) && !b.empty()) { b[1] ^= 0x7F; WriteAtomic(dir + L"\\files\\settings\\settings.json", b.data(), b.size()); }
            };
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && m.updates == 0 && !r.backupSealed, "upd1 run: a corrupted backup aborts the update before any command");
            Check(DirExists(r.recoveryDir), "upd1 run: the damaged recovery point is kept for inspection");
        }
        {
            const AcbRoots rt = freshRoots(L"run_pre", true);
            Mock m; UpdContext c = mkCtx(m, rt); c.provider.uniGetAvailable = false;
            const UpdReport r1 = UpdRun(c, pk);
            Check(r1.outcome == UpdOutcome::AbortedBeforeUpdate && m.cmds.empty(), "upd1 pre: no UniGetUI = aborted, nothing run");
            Mock m2; m2.readable = false; UpdContext c2 = mkCtx(m2, rt); UpdPackage noVer = pk; noVer.targetVersion.clear();
            const UpdReport r2 = UpdRun(c2, noVer);
            Check(r2.outcome == UpdOutcome::AbortedBeforeUpdate && m2.updates == 0, "upd1 pre: unknown installed AND target version = aborted");
            Mock m3; m3.installed = "1.1.0"; UpdContext c3 = mkCtx(m3, rt);
            const UpdReport r3 = UpdRun(c3, pk);
            Check(r3.outcome == UpdOutcome::Succeeded && !r3.commandRan && m3.updates == 0, "upd1 pre: already at the requested version = no-op success, no command");
            Mock m4; UpdContext c4 = mkCtx(m4, rt); UpdPackage bad = pk; bad.id = "x\" --skip-hash";
            const UpdReport r4 = UpdRun(c4, bad);
            Check(r4.outcome == UpdOutcome::InvalidRequest && m4.cmds.empty(), "upd1 pre: an unsafe identifier is rejected without running anything");
            Check(!FileExists(UpdActivePath(rt)), "upd1 pre: no pre-check path leaves a lock behind");
        }
        {
            const AcbRoots rt = freshRoots(L"run_policy", false);
            Mock m; UpdContext c = mkCtx(m, rt); c.policy = UpdBackupPolicy::HighImpactOnly;
            const UpdReport minor = UpdRun(c, pk);
            Check(minor.outcome == UpdOutcome::Succeeded && !minor.backupRequired && !minor.backupCreated && m.updates == 1, "upd1 policy run: HighImpactOnly skips the backup for a minor update");
            Mock m2; m2.afterUpdate = "2.0.0"; UpdContext c2 = mkCtx(m2, rt); c2.policy = UpdBackupPolicy::HighImpactOnly;
            UpdPackage majorPk = pk; majorPk.targetVersion = "2.0.0";
            const UpdReport major = UpdRun(c2, majorPk);
            Check(major.outcome == UpdOutcome::AbortedBeforeUpdate && major.backupRequired && m2.updates == 0, "upd1 policy run: ... but a major update without a verified backup is refused");
        }

        // ---- [R4] the full run: "latest" must not trigger a rollback of a good update
        {
            const AcbRoots rt = freshRoots(L"r4_run", true);
            Mock m; m.installed = "1.0-beta"; m.afterUpdate = "1.1.0"; m.afterReinstall = "1.0-beta";
            UpdContext c = mkCtx(m, rt); c.autoRollback = true;
            UpdPackage latest = pk; latest.targetVersion.clear();
            const UpdReport r = UpdRun(c, latest);
            Check(r.outcome == UpdOutcome::Succeeded, "upd1 [R4] run: 1.0-beta -> 1.1.0 with target 'latest' = Succeeded");
            Check(m.installs == 0, "upd1 [R4] run: ... and autoRollback did NOT reinstall the old version over the good update");
            Check(r.rollbackResult == UpdRollbackResult::NotNeeded, "upd1 [R4] run: ... rollback result is NotNeeded");
        }

        // ---- rollback behaviour
        {
            const AcbRoots rt = freshRoots(L"run_rb", true);
            Mock m; m.afterUpdate = "0.9.0"; UpdContext c = mkCtx(m, rt);
            const UpdReport noAuto = UpdRun(c, pk);
            Check(noAuto.outcome == UpdOutcome::Failed && noAuto.verdict.needsRollback, "upd1 rb: a lower installed version fails the update and flags rollback");
            Check(noAuto.rollbackResult == UpdRollbackResult::NotAttempted && m.installs == 0, "upd1 rb: ... but rollback is not run unless asked");
            Check(noAuto.headline.find(L"Recovery point kept") != std::wstring::npos, "upd1 rb: ... and the recovery point is named in the result");
            const AcbRoots rt2 = freshRoots(L"run_rb2", true);
            Mock m2; m2.afterUpdate = "0.9.0"; m2.afterReinstall = "1.0.0"; UpdContext c2 = mkCtx(m2, rt2); c2.autoRollback = true;
            const UpdReport au = UpdRun(c2, pk);
            Check(au.rollbackResult == UpdRollbackResult::Partial && au.binaryRestored && !au.dataRestored && m2.installs == 1, "upd1 rb: reinstalling the previous version without data is a PARTIAL rollback");
            Check(m2.cmds.back().find(L"--version \"1.0.0\"") != std::wstring::npos && m2.cmds.back().find(L"--skip-hash") == std::wstring::npos, "upd1 rb: the reinstall pins the previous version and never skips the hash");
            const AcbRoots rt3 = freshRoots(L"run_rb3", true);
            Mock m3; m3.afterUpdate = "0.9.0"; m3.afterReinstall = "0.9.0"; UpdContext c3 = mkCtx(m3, rt3); c3.autoRollback = true;
            const UpdReport fl = UpdRun(c3, pk);
            Check(fl.rollbackResult == UpdRollbackResult::Failed && !fl.binaryRestored, "upd1 rb: a reinstall that does not restore the version is a FAILED rollback (read-back, not exit code)");
            const AcbRoots rt4 = freshRoots(L"run_rb4", true);
            Mock m4; m4.afterUpdate = "0.9.0"; UpdContext c4 = mkCtx(m4, rt4); c4.autoRollback = true;
            UpdPackage sc = pk; sc.manager = "scoop";
            const UpdReport us = UpdRun(c4, sc);
            Check(us.rollback.capability == UpdRollbackCapability::Unsupported, "upd1 rb: a manager that cannot pin versions is Unsupported");
            Check(us.rollbackResult == UpdRollbackResult::NotAttempted && m4.installs == 0, "upd1 rb: ... and no rollback command is run");
        }
        {   // Full only with a verified data backup AND a data restore mechanism
            const AcbRoots rt = freshRoots(L"run_full", true);
            Mock m; m.afterUpdate = "0.9.0"; UpdContext c = mkCtx(m, rt); c.autoRollback = true;
            bool restored = false;
            c.provider.backupAppData = [](const UpdPackage&, std::wstring& d) { d = L"mock data map backed up and verified"; return true; };
            c.provider.restoreAppData = [&restored](const UpdPackage&, std::wstring& d) { restored = true; d = L"mock restore"; return true; };
            const UpdReport r = UpdRun(c, pk);
            Check(r.rollback.capability == UpdRollbackCapability::Full && r.appDataBackedUp, "upd1 rb: Full = previous-version reinstall AND data backup + restore mechanisms");
            Check(restored && r.binaryRestored && r.dataRestored && r.rollbackResult == UpdRollbackResult::Complete, "upd1 rb: Complete only when both were performed and verified");
            const AcbRoots rt2 = freshRoots(L"run_dfail", true);
            Mock m2; UpdContext c2 = mkCtx(m2, rt2);
            c2.provider.backupAppData = [](const UpdPackage&, std::wstring& d) { d = L"mock data backup failed"; return false; };
            const UpdReport r2 = UpdRun(c2, pk);
            Check(r2.outcome == UpdOutcome::AbortedBeforeUpdate && m2.updates == 0, "upd1 rb: a failed application-data backup aborts the update before any command");
        }
        {   // [R3] capability must not be claimed without a working mechanism
            const AcbRoots rt = freshRoots(L"r3_a", true);
            Mock m; UpdContext c = mkCtx(m, rt);
            c.provider.backupAppData = [](const UpdPackage&, std::wstring& d) { d = L"backed up"; return true; };   // NO restoreAppData
            const UpdReport r = UpdRun(c, pk);
            Check(r.rollback.capability != UpdRollbackCapability::Full, "upd1 [R3] a data backup without a restore mechanism is NOT 'Fully supported'");
            Check(!r.rollback.canRestoreAppData, "upd1 [R3] ... canRestoreAppData is false when no restore function exists");
            const AcbRoots rt2 = freshRoots(L"r3_b", true);
            Mock m2; m2.afterUpdate = "0.9.0"; UpdContext c2 = mkCtx(m2, rt2); c2.autoRollback = true;
            c2.provider.backupAppData = [](const UpdPackage&, std::wstring& d) { d = L"backed up"; return true; };
            const UpdReport r2 = UpdRun(c2, pk);
            Check(r2.rollbackResult != UpdRollbackResult::Complete, "upd1 [R3] ... and a rollback without a data restore is never 'Complete'");
            const AcbRoots rt3 = freshRoots(L"r3_c", true);
            Mock m3; UpdContext c3 = mkCtx(m3, rt3); c3.provider.run = nullptr;   // no way to run any command
            UpdPackage p3 = pk; p3.previousVersion = "1.0.0";
            const UpdReport r3 = UpdRollback(c3, p3, false);
            Check(r3.rollback.capability == UpdRollbackCapability::Unsupported && !r3.rollback.canReinstallPrevious, "upd1 [R3] a reinstall is not claimed when the provider cannot run commands");
            const AcbRoots rt4 = freshRoots(L"r3_d", true);
            Mock m4; UpdContext c4 = mkCtx(m4, rt4);
            c4.provider.restoreAppData = [](const UpdPackage&, std::wstring& d) { d = L"mock"; return true; };
            UpdPackage p4 = pk; p4.previousVersion = "1.0.0";
            const UpdReport r4 = UpdRollback(c4, p4, true);   // the caller merely CLAIMS a verified data backup
            Check(r4.rollback.capability != UpdRollbackCapability::Full, "upd1 [R3] a caller's unverified claim of a data backup cannot make rollback 'Fully supported'");
        }
        {   // standalone rollback on request
            const AcbRoots rt = freshRoots(L"rb_only", true);
            Mock m; m.installed = "1.1.0"; m.afterReinstall = "1.0.0"; UpdContext c = mkCtx(m, rt);
            UpdPackage p2 = pk; p2.previousVersion = "1.0.0";
            const UpdReport r = UpdRollback(c, p2, false);
            Check(r.rollbackResult == UpdRollbackResult::Partial && r.outcome == UpdOutcome::PartiallySucceeded && m.installs == 1, "upd1 rb: a requested rollback runs only the declared plan and reports Partial");
            UpdPackage sc = p2; sc.manager = "scoop"; Mock m2; UpdContext c2 = mkCtx(m2, rt);
            const UpdReport r2 = UpdRollback(c2, sc, false);
            Check(r2.rollbackResult == UpdRollbackResult::NotAttempted && r2.outcome == UpdOutcome::Failed && m2.installs == 0, "upd1 rb: an unsupported manager is not attempted");
            Check(!FileExists(UpdActivePath(rt)), "upd1 rb: the lock is released after a requested rollback");
        }

        // ---- [R1] lock ownership, races, liveness
        {   // acknowledging must never remove a LIVE process's lock
            const AcbRoots rt = freshRoots(L"r1_live", true);
            UpdActiveInfo held; held.id = "20261119-000000-000"; held.package = "live.pkg"; held.phase = "updating"; held.pid = GetCurrentProcessId(); held.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, held);
            UpdActiveInfo got;
            const bool acked = UpdAcknowledgeInterrupted(rt, &got);
            Check(!acked, "upd1 [R1] acknowledge REFUSES a marker owned by a live process");
            Check(FileExists(UpdActivePath(rt)), "upd1 [R1] ... and the live process's lock is still there");
            Mock m; UpdContext c = mkCtx(m, rt);
            Check(UpdRun(c, pk).outcome == UpdOutcome::Busy && m.cmds.empty(), "upd1 [R1] ... so a new update is still refused as Busy");
        }
        {
            const AcbRoots rt = freshRoots(L"r1_dead", true);
            UpdActiveInfo dead; dead.id = "20261119-000001-000"; dead.package = "dead.pkg"; dead.phase = "updating"; dead.pid = kDeadPid; dead.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, dead);
            UpdActiveInfo got;
            Check(UpdAcknowledgeInterrupted(rt, &got) && got.package == "dead.pkg", "upd1 [R1] acknowledge works for a marker whose owner is gone");
            Check(!FileExists(UpdActivePath(rt)), "upd1 [R1] ... and releases the lock");
            Check(!ListDir(UpdHistoryDir(rt), false).empty(), "upd1 [R1] ... keeping the record in history");
        }
        {   // a starter reading the marker while its creator still holds it open must see Busy, not "interrupted"
            const AcbRoots rt = freshRoots(L"r1_race", true);
            UpdActiveInfo info; info.id = "20261119-000002-000"; info.package = "racing.pkg"; info.phase = "preparing"; info.pid = GetCurrentProcessId(); info.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt));
            const std::string body = UpdActiveJson(info);
            HANDLE hold = CreateFileW(UpdActivePath(rt).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wr = 0;
            if (hold != INVALID_HANDLE_VALUE) WriteFile(hold, body.data(), static_cast<DWORD>(body.size()), &wr, nullptr);
            Mock m; UpdContext c = mkCtx(m, rt); c.isProcessAlive = [](DWORD) { return false; };
            const UpdReport r = UpdRun(c, pk);
            if (hold != INVALID_HANDLE_VALUE) CloseHandle(hold);
            Check(hold != INVALID_HANDLE_VALUE, "upd1 [R1] race: test set-up held the marker open");
            Check(r.outcome == UpdOutcome::Busy, "upd1 [R1] race: a marker that is open for writing = Busy (was 'interrupted', inviting a wrong acknowledge)");
            Check(m.cmds.empty(), "upd1 [R1] race: ... and no command ran");
        }
        {   // PID reuse: the same pid with a different process start time is not the same owner
            const AcbRoots rt = freshRoots(L"r1_reuse", true);
            UpdActiveInfo stale; stale.id = "20261119-000003-000"; stale.package = "stale.pkg"; stale.phase = "updating"; stale.pid = GetCurrentProcessId(); stale.started = AcbUtcStampNow();
            std::string j = UpdActiveJson(stale);
            const std::string key = "\"formatVersion\": 1,\r\n";
            j.insert(j.find(key) + key.size(), "  \"pidStart\": \"1\",\r\n");
            writeRaw(UpdActivePath(rt), j);
            Mock m; UpdContext c = mkCtx(m, rt);   // real liveness check
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::InterruptedPreviousUpdate, "upd1 [R1] a recycled pid (start time differs) is not mistaken for the live owner");
            Check(m.cmds.empty() && FileExists(UpdActivePath(rt)), "upd1 [R1] ... the stale marker is kept and no command ran");
        }
        {   // two real concurrent starters: exactly one may run
            const AcbRoots rt = freshRoots(L"r1_threads", true);
            std::atomic<int> updates{0}; std::atomic<bool> updated{false};
            auto prep = [&](UpdContext& c) {
                c.roots = rt; c.journal = false; c.provider.uniGetAvailable = true;
                c.provider.run = [&](const std::wstring& a, std::vector<std::wstring>&) -> DWORD {
                    if (a.rfind(L"package update", 0) == 0) { ++updates; Sleep(500); updated = true; }
                    return 0; };
                c.provider.installedVersion = [&](const UpdPackage&, std::string& v) { v = updated ? "1.1.0" : "1.0.0"; return true; };
            };
            UpdContext c1, c2; prep(c1); prep(c2);
            UpdReport r1, r2;
            std::thread t1([&] { r1 = UpdRun(c1, pk); });
            Sleep(150);
            std::thread t2([&] { r2 = UpdRun(c2, pk); });
            t1.join(); t2.join();
            Check(updates == 1, "upd1 [R1] threads: exactly one update command ran");
            Check(r1.outcome == UpdOutcome::Succeeded, "upd1 [R1] threads: the first starter succeeded");
            Check(r2.outcome == UpdOutcome::Busy, "upd1 [R1] threads: the second starter was refused as Busy");
        }
        {   // an interrupted update blocks new ones until acknowledged
            const AcbRoots rt = freshRoots(L"run_conc", true);
            UpdActiveInfo held; held.id = "20261119-000004-000"; held.package = "other.pkg"; held.phase = "updating"; held.pid = kDeadPid; held.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, held);
            Mock m; UpdContext c = mkCtx(m, rt);
            const UpdReport intr = UpdRun(c, pk);
            Check(intr.outcome == UpdOutcome::InterruptedPreviousUpdate && m.cmds.empty(), "upd1 int: a dead owner's marker blocks a new update");
            Check(FileExists(UpdActivePath(rt)), "upd1 int: ... and is kept, not overwritten");
            UpdActiveInfo ack;
            Check(UpdAcknowledgeInterrupted(rt, &ack) && ack.package == "other.pkg", "upd1 int: acknowledging releases it");
            Mock m3; UpdContext c3 = mkCtx(m3, rt);
            Check(UpdRun(c3, pk).outcome == UpdOutcome::Succeeded, "upd1 int: then a new update runs");
        }
        {   // simulated crash during the update
            const AcbRoots rt = freshRoots(L"run_crash", true);
            Mock m; UpdContext c = mkCtx(m, rt);
            c.simulateCrashAt = [](UpdPhase p) { return p == UpdPhase::Updating; };
            const UpdReport r = UpdRun(c, pk);
            UpdActiveInfo left;
            const bool parsed = UpdParseActive(ReadText(UpdActivePath(rt)), left);
            Check(r.interrupted && m.updates == 0, "upd1 int: a simulated crash stops before the update command");
            Check(parsed && left.phase == "updating" && !left.steps.empty(), "upd1 int: the crash leaves phase and steps recorded");
            Check(!left.recoveryDir.empty() && DirExists(Utf8ToW(left.recoveryDir)), "upd1 int: ... and the recovery point it made");
            Mock m2; UpdContext c2 = mkCtx(m2, rt); c2.isProcessAlive = [](DWORD) { return false; };
            const UpdReport next = UpdRun(c2, pk);
            Check(next.outcome == UpdOutcome::InterruptedPreviousUpdate && m2.updates == 0, "upd1 int: the next update refuses to start over it");
        }

        // ---- [R2] marker identifiers and containment
        {
            const AcbRoots rt = freshRoots(L"r2_trav", true);
            UpdActiveInfo bad; bad.id = "..\\..\\escaped"; bad.package = "x"; bad.phase = "updating"; bad.pid = kDeadPid; bad.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, bad);
            UpdAcknowledgeInterrupted(rt);
            Check(!FileExists(rt.data + L"\\escaped-interrupted-acknowledged.json"), "upd1 [R2] a hostile marker id cannot write a file outside Updater\\history");
            Check(!FileExists(UpdActivePath(rt)), "upd1 [R2] ... the hostile marker is still released safely");
            Check(DirExists(UpdHistoryDir(rt)) && !ListDir(UpdHistoryDir(rt), false).empty(), "upd1 [R2] ... and preserved inside history");
        }
        {
            UpdActiveInfo a;
            const char* ids[] = {"..\\x", "../x", "C:\\x", "a b", "", "x\\y", "a:b", ".", "-x", "a..b"};
            bool allRejected = true;
            for (const char* id : ids) {
                UpdActiveInfo t; t.id = id; t.package = "p"; t.pid = 1;
                if (UpdParseActive(UpdActiveJson(t), a)) allRejected = false;
            }
            Check(allRejected, "upd1 [R2] UpdParseActive rejects traversal / drive / separator / dash / empty marker ids");
            UpdActiveInfo good; good.id = "20261119-123456-789"; good.package = "7zip.7zip"; good.pid = 1;
            Check(UpdParseActive(UpdActiveJson(good), a) && a.id == good.id, "upd1 [R2] ... and still accepts the id format AnkhCore writes");
            UpdActiveInfo badPkg = good; badPkg.package = "x\" & calc";
            Check(!UpdParseActive(UpdActiveJson(badPkg), a), "upd1 [R2] a marker with an unsafe package field is rejected too");
        }
        {   // an unreadable/hostile marker blocks, is never overwritten, and can be acknowledged into history
            const AcbRoots rt = freshRoots(L"r2_block", true);
            UpdActiveInfo bad; bad.id = "..\\..\\x"; bad.package = "x"; bad.phase = "updating"; bad.pid = kDeadPid; bad.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, bad);
            const std::string before = ReadText(UpdActivePath(rt));
            Mock m; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.outcome == UpdOutcome::InterruptedPreviousUpdate && m.cmds.empty(), "upd1 [R2] a hostile marker blocks updates");
            Check(ReadText(UpdActivePath(rt)) == before, "upd1 [R2] ... and is not modified");
        }
        {   // containment helper (canonical path stays inside the history directory)
            const AcbRoots rt = freshRoots(L"r2_contain", true);
            UpdActiveInfo ok; ok.id = "20261119-000005-000"; ok.package = "p"; ok.phase = "updating"; ok.pid = kDeadPid; ok.started = AcbUtcStampNow();
            EnsureDir(UpdDir(rt)); UpdSaveActive(rt, ok);
            UpdAcknowledgeInterrupted(rt);
            const auto files = ListDir(UpdHistoryDir(rt), false);
            Check(files.size() == 1 && files[0].find(L"20261119-000005-000") == 0, "upd1 [R2] a normal acknowledge writes exactly one file named from the validated id, inside history");
            Check(!FileExists(rt.data + L"\\20261119-000005-000-interrupted-acknowledged.json"), "upd1 [R2] ... and nothing next to the history folder");
        }
        {   // [S2] a reparse point where the Updater directory should be is refused (the lock must not be created through a junction)
            const AcbRoots rt = freshRoots(L"r2_junction", true);
            const std::wstring elsewhere = updRoot + L"\\r2_junction_target";
            Evict(elsewhere, true, false);
            EnsureDir(elsewhere);
            Run(L"cmd.exe", L"/c mklink /J \"" + rt.data + L"\\Updater\" \"" + elsewhere + L"\"", true, 20000);
            Mock m; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            const bool leaked = !ListDir(elsewhere, false).empty() || !ListDir(elsewhere, true).empty();   // anything written through the junction (the lock is released by the end of a run)
            Run(L"cmd.exe", L"/c rmdir \"" + rt.data + L"\\Updater\"", true, 20000);   // removes only the junction itself
            Check(!leaked, "upd1 [S2] the lock is not created through a junction at <data>\\Updater");
            Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && m.cmds.empty(), "upd1 [S2] ... the update is aborted before any command");
        }

        // ---- [R5] timeouts
        {
            const AcbRoots rt = freshRoots(L"r5_a", true);
            Mock m; m.updateExit = kRunTimedOut; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            Check(r.headline.find(L"timed out") != std::wstring::npos, "upd1 [R5] a timed-out update command is reported as a timeout");
            Check(FileExists(UpdActivePath(rt)), "upd1 [R5] ... the lock is NOT released: the operation may still be running in the UniGetUI session");
            Check(m.installs == 0, "upd1 [R5] ... and nothing is rolled back automatically");
            Mock m2; UpdContext c2 = mkCtx(m2, rt); c2.isProcessAlive = [](DWORD) { return false; };
            const UpdReport again = UpdRun(c2, pk);
            Check(again.outcome != UpdOutcome::Succeeded && m2.cmds.empty(), "upd1 [R5] ... so another update cannot begin (no command ran)");
            Mock m3; UpdContext c3 = mkCtx(m3, rt);
            const UpdReport again2 = UpdRun(c3, pk);
            Check(again2.outcome != UpdOutcome::Succeeded && m3.cmds.empty(), "upd1 [R5] ... even from the same still-running process");
            Check(!UpdAcknowledgeInterrupted(rt), "upd1 [R5] ... and a plain acknowledge refuses an unresolved timeout");
            Check(FileExists(UpdActivePath(rt)), "upd1 [R5] ... leaving the marker in place");
        }
        {   // a timeout during the rollback reinstall is tracked the same way
            const AcbRoots rt = freshRoots(L"r5_b", true);
            Mock m; m.afterUpdate = "0.9.0"; m.installExit = kRunTimedOut; UpdContext c = mkCtx(m, rt); c.autoRollback = true;
            UpdRun(c, pk);
            Check(FileExists(UpdActivePath(rt)), "upd1 [R5] a timeout in the rollback reinstall also keeps the lock");
            Mock m2; UpdContext c2 = mkCtx(m2, rt); c2.isProcessAlive = [](DWORD) { return false; };
            Check(UpdRun(c2, pk).outcome != UpdOutcome::Succeeded && m2.cmds.empty(), "upd1 [R5] ... and blocks the next update");
        }

        // ---- [R3] rollback assessment matrix (what can ACTUALLY be done)
        {
            UpdPackage p = pk; p.previousVersion = "1.0.0";
            Check(UpdAssessRollback(p, false, false, false).capability == UpdRollbackCapability::Unsupported, "upd1 [R3] assess: no mechanism = Unsupported");
            const UpdRollbackAssessment re = UpdAssessRollback(p, true, false, false);
            Check(re.capability == UpdRollbackCapability::Partial && re.canReinstallPrevious && !re.canRestoreAppData, "upd1 [R3] assess: reinstall only = Partial");
            const UpdRollbackAssessment da = UpdAssessRollback(p, false, true, true);
            Check(da.capability == UpdRollbackCapability::Partial && !da.canReinstallPrevious && da.canRestoreAppData, "upd1 [R3] assess: data restore only = Partial");
            Check(UpdAssessRollback(p, true, true, true).capability == UpdRollbackCapability::Full, "upd1 [R3] assess: reinstall AND verified data backup AND restore = Full");
            const UpdRollbackAssessment nr = UpdAssessRollback(p, true, true, false);
            Check(nr.capability != UpdRollbackCapability::Full && !nr.canRestoreAppData, "upd1 [R3] assess: a data backup without a restore mechanism is not a data capability");
            Check(UpdAssessRollback(p, true, false, true).capability != UpdRollbackCapability::Full, "upd1 [R3] assess: a restore mechanism without a verified backup is not Full");
            UpdPackage sc = p; sc.manager = "scoop";
            Check(UpdAssessRollback(sc, true, false, false).capability == UpdRollbackCapability::Unsupported, "upd1 [R3] assess: scoop cannot pin a version = no reinstall capability");
            UpdPackage np = p; np.previousVersion.clear();
            Check(UpdAssessRollback(np, true, false, false).capability == UpdRollbackCapability::Unsupported, "upd1 [R3] assess: an unknown previous version = no reinstall capability");
        }
        {   // stand-alone rollback: Full only when the provider confirms the data backup now
            const AcbRoots rt = freshRoots(L"r3_full", true);
            Mock m; m.installed = "1.1.0"; m.afterReinstall = "1.0.0"; UpdContext c = mkCtx(m, rt);
            bool restored = false;
            c.provider.restoreAppData = [&restored](const UpdPackage&, std::wstring& d) { restored = true; d = L"mock restore"; return true; };
            c.provider.verifyAppDataBackup = [](const UpdPackage&, std::wstring& d) { d = L"mock backup verified"; return true; };
            UpdPackage p2 = pk; p2.previousVersion = "1.0.0";
            const UpdReport r = UpdRollback(c, p2, true);
            Check(r.rollback.capability == UpdRollbackCapability::Full, "upd1 [R3] stand-alone rollback: Full when the provider confirms the data backup and can restore it");
            Check(restored && r.binaryRestored && r.dataRestored && r.rollbackResult == UpdRollbackResult::Complete, "upd1 [R3] stand-alone rollback: ... and Complete only after both were done");
            const AcbRoots rt2 = freshRoots(L"r3_full2", true);
            Mock m2; m2.installed = "1.1.0"; UpdContext c2 = mkCtx(m2, rt2);
            c2.provider.restoreAppData = [](const UpdPackage&, std::wstring& d) { d = L"mock"; return true; };
            c2.provider.verifyAppDataBackup = [](const UpdPackage&, std::wstring& d) { d = L"no backup found"; return false; };
            const UpdReport r2 = UpdRollback(c2, p2, true);
            Check(r2.rollback.capability != UpdRollbackCapability::Full && !r2.dataRestored, "upd1 [R3] stand-alone rollback: a provider that cannot confirm the backup = not Full, no data restored");
        }

        // ---- [R1] lock primitives
        {
            const AcbRoots rt = freshRoots(L"r1_acq", true);
            UpdActiveInfo a; a.id = "20261119-100000-000"; a.package = "p"; a.phase = "preparing"; a.pid = GetCurrentProcessId(); a.started = AcbUtcStampNow();
            UpdProcessStartString(a.pid, a.pidStart);
            UpdActiveInfo ex;
            Check(UpdAcquire(rt, a, ex, {}) == UpdLockResult::Created, "upd1 [R1] acquire: the first starter gets the lock");
            Check(UpdAcquire(rt, a, ex, {}) == UpdLockResult::Busy && ex.package == "p", "upd1 [R1] acquire: a second starter sees the live owner = Busy (real pid + start-time check)");
            Check(UpdAcquire(rt, a, ex, [](DWORD) { return false; }) == UpdLockResult::Interrupted, "upd1 [R1] acquire: an owner reported dead = Interrupted");
            Check(ListDir(UpdDir(rt), false).size() == 1, "upd1 [R1] acquire: no temporary files are left behind by refused starters");
        }
        {
            UpdActiveInfo a; a.id = "20261119-100001-000"; a.package = "p"; a.pid = 4321; a.pidStart = "132456789012345678"; a.unresolved = true; a.phase = "operation-unknown";
            UpdActiveInfo b;
            Check(UpdParseActive(UpdActiveJson(a), b) && b.unresolved && b.pidStart == a.pidStart && b.pid == 4321, "upd1 [R1] marker: pid, start time and the unresolved flag survive a round trip");
            UpdActiveInfo c = a; c.pidStart = "12ab";
            Check(!UpdParseActive(UpdActiveJson(c), b), "upd1 [R1] marker: a non-numeric start time is rejected");
        }
        {   // a running update records its own process identity
            const AcbRoots rt = freshRoots(L"r1_ident", true);
            Mock m; UpdContext c = mkCtx(m, rt);
            c.simulateCrashAt = [](UpdPhase p) { return p == UpdPhase::Updating; };
            UpdRun(c, pk);
            UpdActiveInfo left; UpdParseActive(ReadText(UpdActivePath(rt)), left);
            std::string self; UpdProcessStartString(GetCurrentProcessId(), self);
            Check(left.pid == GetCurrentProcessId() && !left.pidStart.empty() && left.pidStart == self, "upd1 [R1] a running update records its pid AND process start time");
            Check(UpdOwnerAlive(left, {}), "upd1 [R1] ... and is recognised as alive");
            left.pidStart = "1";
            Check(!UpdOwnerAlive(left, {}), "upd1 [R1] ... but a different start time for the same pid is not the owner");
        }

        // ---- [R5] resolving a timed-out operation (explicit, verified, never automatic)
        {
            auto timedOutState = [&](const wchar_t* name) {
                const AcbRoots rt = freshRoots(name, true);
                Mock m; m.updateExit = kRunTimedOut; UpdContext c = mkCtx(m, rt);
                UpdRun(c, pk);
                return rt;
            };
            auto verifierWith = [&](DWORD rc, const std::vector<std::wstring>& lines) {
                UpdProvider pr; pr.uniGetAvailable = true;
                pr.run = [rc, lines](const std::wstring& a, std::vector<std::wstring>& out) -> DWORD { if (a.rfind(L"operation list", 0) == 0) out = lines; return rc; };
                return pr;
            };
            const AcbRoots rt1 = timedOutState(L"r5_res1");
            UpdActiveInfo info;
            Check(UpdAcknowledge(rt1, &info) == UpdAckResult::Unresolved && info.unresolved, "upd1 [R5] resolve: an unresolved timeout cannot be acknowledged without verification");
            const UpdProvider still = verifierWith(0, {L"123  7zip.7zip  winget  Running"});
            Check(UpdAcknowledge(rt1, nullptr, {}, &still) == UpdAckResult::StillMaybeRunning && FileExists(UpdActivePath(rt1)), "upd1 [R5] resolve: the package still listed as an operation = refused, lock kept");
            const UpdProvider cant = verifierWith(3, {});
            Check(UpdAcknowledge(rt1, nullptr, {}, &cant) == UpdAckResult::Unresolved && FileExists(UpdActivePath(rt1)), "upd1 [R5] resolve: a verifier that cannot answer (exit 3) leaves it blocked");
            const UpdProvider none = verifierWith(0, {L"(no operations)"});
            Check(UpdAcknowledge(rt1, nullptr, {}, &none) == UpdAckResult::Acknowledged && !FileExists(UpdActivePath(rt1)), "upd1 [R5] resolve: not listed as a running operation = released");
            Mock m5; UpdContext c5 = mkCtx(m5, rt1);
            Check(UpdRun(c5, pk).outcome == UpdOutcome::Succeeded, "upd1 [R5] resolve: ... and a new update can then run");
            const AcbRoots rt2 = timedOutState(L"r5_res2");
            Check(UpdAcknowledge(rt2, nullptr, {}, nullptr, true) == UpdAckResult::Acknowledged && !FileExists(UpdActivePath(rt2)), "upd1 [R5] resolve: an explicit force releases it");
            bool forcedRecorded = false;
            for (const auto& f : ListDir(UpdHistoryDir(rt2), false)) if (f.find(L"unresolved-forced") != std::wstring::npos) forcedRecorded = true;
            Check(forcedRecorded, "upd1 [R5] resolve: ... and the forced decision is recorded in history");
        }
        {   // recovery information survives a timeout
            const AcbRoots rt = freshRoots(L"r5_keep", true);
            Mock m; m.updateExit = kRunTimedOut; UpdContext c = mkCtx(m, rt);
            const UpdReport r = UpdRun(c, pk);
            UpdActiveInfo left; const bool parsed = UpdParseActive(ReadText(UpdActivePath(rt)), left);
            Check(r.outcome == UpdOutcome::TimedOut && r.timedOut, "upd1 [R5] a timed-out update reports TimedOut");
            Check(parsed && left.unresolved && left.pid == 0 && left.phase == "operation-unknown", "upd1 [R5] ... the marker is unresolved and owned by no process");
            Check(!left.recoveryDir.empty() && DirExists(Utf8ToW(left.recoveryDir)) && FileExists(r.recoveryDir + L"\\update-report.txt"), "upd1 [R5] ... and the recovery point and report are kept");
        }

        // ---- [B1] UniGetUI JSON listings. Fixtures have the exact structure observed from the real UniGetUI 2026.3.3 (indented,
        // CRLF, ASCII with \uXXXX escapes); their VALUES are synthetic (no real package list is stored in the repository).
        {
            auto pkgJson = [](const std::string& name, const std::string& id, const std::string& ver, const std::string& nver, const std::string& src,
                              const std::string& mgr, bool upg, bool last) {
                return std::string("    {\r\n      \"name\": \"") + name + "\",\r\n      \"id\": \"" + id + "\",\r\n      \"version\": \"" + ver +
                       "\",\r\n      \"newVersion\": \"" + nver + "\",\r\n      \"source\": \"" + src + "\",\r\n      \"manager\": \"" + mgr +
                       "\",\r\n      \"isUpgradable\": " + (upg ? "true" : "false") + ",\r\n      \"systemRestartPending\": false\r\n    }" + (last ? "\r\n" : ",\r\n");
            };
            auto doc = [](const std::string& key, const std::string& body) {
                return std::string("{\r\n  \"status\": \"success\",\r\n  \"") + key + "\": [\r\n" + body + "  ]\r\n}\r\n";
            };
            auto toLines = [](const std::string& t) {   // what RunCapture delivers: no CR/LF, leading spaces trimmed, blank lines dropped
                std::vector<std::wstring> out; std::string cur;
                auto flush = [&]() { size_t a = cur.find_first_not_of(" \t"); if (a != std::string::npos) out.push_back(Utf8ToW(cur.substr(a))); cur.clear(); };
                for (char c : t) { if (c == '\n' || c == '\r') flush(); else cur += c; }
                flush();
                return out;
            };
            const std::string installed = doc("packages",
                pkgJson("Example Tool \\u00AE", "Example.Tool", "2.5.0.3", "", "WinGet: winget", "winget", false, false) +
                pkgJson("Quote \\u0022App\\u0022", "Quote.App", "26.06", "", "Local PC", "winget", false, false) +
                pkgJson("Third", "Third.Pkg", "9.13.013", "", "Microsoft Store", "winget", false, true));

            const UpdPackageListing a = UpdParsePackageListing(installed);
            Check(a.status == UpdListStatus::Ok, "upd1 [B1] parse: a real-shape installed listing is accepted");
            Check(a.arrayKey == "packages" && a.packages.size() == 3 && a.skipped == 0, "upd1 [B1] parse: three packages, none skipped, list key 'packages'");
            Check(a.packages[0].id == "Example.Tool" && a.packages[0].version == "2.5.0.3" && a.packages[0].manager == "winget" && a.packages[0].source == "WinGet: winget" && !a.packages[0].isUpgradable,
                  "upd1 [B1] parse: id, version, manager, source and isUpgradable are read");
            Check(a.packages[0].name == "Example Tool \xC2\xAE", "upd1 [B1] parse: a \\u00AE escape decodes to UTF-8");
            Check(a.packages[1].name == "Quote \"App\"", "upd1 [B1] parse: a \\u0022 escape decodes to a quote");
            Check(a.packages[2].version == "9.13.013", "upd1 [B1] parse: a version with a leading-zero part is kept as text");
            Check(UpdParsePackageListing(std::string("\xEF\xBB\xBF") + installed).status == UpdListStatus::Ok, "upd1 [B1] parse: a UTF-8 BOM is tolerated");
            const UpdPackageListing lines = UpdParsePackageListingLines(toLines(installed));
            Check(lines.status == UpdListStatus::Ok && lines.packages.size() == 3, "upd1 [B1] parse: the same document as RunCapture lines (trimmed, no CR/LF) parses identically");
            Check(UpdParsePackageListing("{\"status\":\"success\",\"packages\":[]}").status == UpdListStatus::Ok &&
                  UpdParsePackageListing("{\"status\":\"success\",\"packages\":[]}").packages.empty(), "upd1 [B1] parse: an empty packages array is a valid, empty listing");

            const std::string updates = doc("updates",
                pkgJson("Up One", "Up.One", "2.0.30", "2.0.31", "WinGet: winget", "winget", true, false) +
                pkgJson("Up Two", "Up.Two", "Unknown", "1.2", "WinGet: winget", "winget", true, true));
            const UpdPackageListing u = UpdParsePackageListing(updates);
            Check(u.status == UpdListStatus::Ok && u.arrayKey == "updates" && u.packages.size() == 2, "upd1 [B1] parse: a `package updates` listing (key 'updates') is accepted");
            Check(u.packages[0].newVersion == "2.0.31" && u.packages[0].isUpgradable && u.packages[1].version == "Unknown", "upd1 [B1] parse: newVersion, isUpgradable and an 'Unknown' version are read as they are");

            const UpdPackageListing err = UpdParsePackageListing("{\r\n  \"status\": \"error\",\r\n  \"command\": \"\",\r\n  \"message\": \"No ready package manager matching \\u0022x\\u0022 is available.\"\r\n}\r\n");
            Check(err.status == UpdListStatus::ErrorEnvelope && err.message.find(L"No ready package manager") != std::wstring::npos && err.packages.empty(), "upd1 [B1] parse: an error envelope (exit 2 in the real build) is recognised with its message");
            Check(UpdParsePackageListing("").status == UpdListStatus::Empty, "upd1 [B1] parse: empty output = Empty (what an unknown command prints, with exit 0)");
            Check(UpdParsePackageListing(" \r\n\t\r\n").status == UpdListStatus::Empty, "upd1 [B1] parse: whitespace-only output = Empty");
            Check(UpdParsePackageListing("\xEF\xBB\xBF").status == UpdListStatus::Empty, "upd1 [B1] parse: a BOM alone = Empty");
            Check(UpdParsePackageListingLines({}).status == UpdListStatus::Empty, "upd1 [B1] parse: no lines at all = Empty");

            Check(UpdParsePackageListing(installed.substr(0, installed.size() / 2)).status == UpdListStatus::Malformed, "upd1 [B1] parse: truncated JSON = Malformed");
            Check(UpdParsePackageListing("Usage:\r\n  unigetui [global-options] <command>\r\n").status == UpdListStatus::Malformed, "upd1 [B1] parse: help text instead of JSON = Malformed");
            Check(UpdParsePackageListing(installed + "trailing text").status == UpdListStatus::Malformed, "upd1 [B1] parse: text after the JSON document = Malformed");
            Check(UpdParsePackageListing("{\"status\":\"success\",\"packages\":[],\"packages\":[]}").status == UpdListStatus::Malformed, "upd1 [B1] parse: a duplicate key = Malformed (strict parser)");
            Check(UpdParsePackageListing("{\"status\":\"success\",\"packages\":{}}").status == UpdListStatus::Malformed, "upd1 [B1] parse: packages that is not an array = Malformed");
            Check(UpdParsePackageListing(std::string(100, '[') + std::string(100, ']')).status == UpdListStatus::Malformed, "upd1 [B1] parse: a deeply nested document = Malformed");
            Check(UpdParsePackageListing("[]").status == UpdListStatus::Unsupported, "upd1 [B1] parse: a JSON array at the top = Unsupported");
            Check(UpdParsePackageListing("{\"status\":\"success\"}").status == UpdListStatus::Unsupported, "upd1 [B1] parse: success without a list = Unsupported");
            Check(UpdParsePackageListing("{\"packages\":[]}").status == UpdListStatus::Unsupported, "upd1 [B1] parse: no status field = Unsupported");
            Check(UpdParsePackageListing("{\"status\":\"weird\",\"packages\":[]}").status == UpdListStatus::Unsupported, "upd1 [B1] parse: an unknown status = Unsupported");
            Check(UpdParsePackageListing("{\"status\":7,\"packages\":[]}").status == UpdListStatus::Unsupported, "upd1 [B1] parse: a non-text status = Unsupported");
            Check(UpdParsePackageListing("{\"running\":true,\"version\":\"2026.3.3\"}").status == UpdListStatus::Unsupported, "upd1 [B1] parse: the `status` command's document is not a package listing");
            Check(UpdParsePackageListing(std::string(9u << 20, ' ') + installed).status == UpdListStatus::Unsupported, "upd1 [B1] parse: output beyond the size limit is refused");

            const UpdPackageListing odd = UpdParsePackageListing(
                "{\"status\":\"success\",\"packages\":[{\"id\":\"A.B\",\"manager\":\"winget\"},{\"name\":\"no id\",\"version\":\"1.0\"},7,null,"
                "{\"id\":\"C.D\",\"version\":null,\"manager\":\"winget\"}]}");
            Check(odd.status == UpdListStatus::Ok && odd.packages.size() == 2 && odd.skipped == 3, "upd1 [B1] parse: items without an id or that are not objects are skipped and counted");
            Check(odd.packages[0].version.empty() && odd.packages[1].version.empty(), "upd1 [B1] parse: a missing or null version field reads as empty");

            // a big listing (the real winget list had 371 entries, 119 KB)
            std::string body;
            for (int i = 0; i < 400; ++i) body += pkgJson("Name " + std::to_string(i), "Gen.Pkg" + std::to_string(i), "1." + std::to_string(i) + ".0", "", "WinGet: winget", "winget", false, i == 399);
            const UpdPackageListing big = UpdParsePackageListing(doc("packages", body));
            Check(big.status == UpdListStatus::Ok && big.packages.size() == 400, "upd1 [B1] parse: a 400-entry listing parses completely");

            // ---- lookup
            UpdPackage want; want.id = "Example.Tool"; want.manager = "winget";
            const UpdVersionLookup f = UpdFindInstalledVersion(a, want);
            Check(f.result == UpdLookup::Found && f.version == "2.5.0.3", "upd1 [B1] lookup: the installed version is found by id and manager");
            UpdPackage ci = want; ci.id = "example.TOOL"; ci.manager = "WinGet";
            Check(UpdFindInstalledVersion(a, ci).result == UpdLookup::Found, "upd1 [B1] lookup: id and manager match case-insensitively");
            UpdPackage nope = want; nope.id = "No.Such";
            Check(UpdFindInstalledVersion(a, nope).result == UpdLookup::NotListed, "upd1 [B1] lookup: a package that is not listed = NotListed");
            UpdPackage other = want; other.manager = "scoop";
            Check(UpdFindInstalledVersion(a, other).result == UpdLookup::NotListed, "upd1 [B1] lookup: the same id under another manager is not a match");
            Check(UpdFindInstalledVersion(u, want).result == UpdLookup::ListingUnusable, "upd1 [B1] lookup: an `updates` listing is never read as installed state");
            Check(UpdFindInstalledVersion(UpdParsePackageListing(""), want).result == UpdLookup::ListingUnusable, "upd1 [B1] lookup: an empty listing is unusable");
            Check(UpdFindInstalledVersion(err, want).result == UpdLookup::ListingUnusable && UpdFindInstalledVersion(err, want).why.find(L"No ready package manager") != std::wstring::npos, "upd1 [B1] lookup: an error envelope is unusable and its message is kept");
            Check(UpdFindInstalledVersion(UpdParsePackageListing("{\"status\":\"success\"}"), want).result == UpdLookup::ListingUnusable, "upd1 [B1] lookup: an unsupported document is unusable");

            const std::string dups = doc("packages",
                pkgJson("A", "Dup.Pkg", "1.0", "", "WinGet: winget", "winget", false, false) +
                pkgJson("B", "Dup.Pkg", "2.0", "", "Local PC", "winget", false, false) +
                pkgJson("C", "Same.Pkg", "3.0", "", "WinGet: winget", "winget", false, false) +
                pkgJson("D", "Same.Pkg", "3.0", "", "Local PC", "winget", false, true));
            const UpdPackageListing dl = UpdParsePackageListing(dups);
            UpdPackage d1 = want; d1.id = "Dup.Pkg";
            Check(UpdFindInstalledVersion(dl, d1).result == UpdLookup::Ambiguous, "upd1 [B1] lookup: one id with two different versions = Ambiguous");
            UpdPackage d2 = d1; d2.source = "Local PC";
            const UpdVersionLookup narrowed = UpdFindInstalledVersion(dl, d2);
            Check(narrowed.result == UpdLookup::Found && narrowed.version == "2.0", "upd1 [B1] lookup: an exactly matching source resolves the ambiguity");
            UpdPackage d3 = want; d3.id = "Same.Pkg";
            Check(UpdFindInstalledVersion(dl, d3).result == UpdLookup::Found, "upd1 [B1] lookup: duplicates with the same version are not ambiguous");

            const std::string bad = doc("packages",
                pkgJson("U", "Unk.Pkg", "Unknown", "", "WinGet: winget", "winget", false, false) +
                pkgJson("T", "Text.Pkg", "ad 9.8.3", "", "WinGet: winget", "winget", false, false) +
                pkgJson("E", "Empty.Pkg", "", "", "WinGet: winget", "winget", false, true));
            const UpdPackageListing bl = UpdParsePackageListing(bad);
            UpdPackage b1 = want, b2 = want, b3 = want; b1.id = "Unk.Pkg"; b2.id = "Text.Pkg"; b3.id = "Empty.Pkg";
            Check(UpdFindInstalledVersion(bl, b1).result == UpdLookup::VersionUnusable, "upd1 [B1] lookup: the version 'Unknown' is not a version");
            Check(UpdFindInstalledVersion(bl, b2).result == UpdLookup::VersionUnusable, "upd1 [B1] lookup: free text ('ad 9.8.3') is not a version");
            Check(UpdFindInstalledVersion(bl, b3).result == UpdLookup::VersionUnusable, "upd1 [B1] lookup: a missing version is unusable");

            // ---- provider on a mock runner (the real parsing path, no process)
            struct W { std::string cur = "1.0.0", afterUpdate = "1.1.0", afterReinstall = "1.0.0"; DWORD updateExit = 0, listExit = 0;
                       int mode = 0;   // how `package installed` answers: 0 JSON, 1 empty, 2 malformed, 3 unsupported json, 4 error envelope, 5 'Unknown' version, 6 updates-shaped
                       int updates = 0, installs = 0, lists = 0; std::vector<std::wstring> cmds; bool listAfterUpdateOnly = false; bool updated = false; int modeAfterUpdate = -1; };
            auto answer = [&](W& w, std::vector<std::wstring>& out) {
                ++w.lists;
                int m = (w.updated && w.modeAfterUpdate >= 0) ? w.modeAfterUpdate : w.mode;
                std::string t;
                if (m == 0) t = doc("packages", pkgJson("N", "Other.App", "3.1", "", "WinGet: winget", "winget", false, false) + pkgJson("T", "7zip.7zip", w.cur, "", "WinGet: winget", "winget", false, true));
                else if (m == 1) t = "";
                else if (m == 2) t = "{\r\n  \"status\": \"succ";
                else if (m == 3) t = "{\"status\":\"success\",\"items\":[]}";
                else if (m == 4) t = "{\r\n  \"status\": \"error\",\r\n  \"command\": \"\",\r\n  \"message\": \"The IPC API token is not available. Start UniGetUI and try again.\"\r\n}\r\n";
                else if (m == 5) t = doc("packages", pkgJson("T", "7zip.7zip", "Unknown", "", "WinGet: winget", "winget", false, true));
                else t = doc("updates", pkgJson("T", "7zip.7zip", w.cur, "9.9", "WinGet: winget", "winget", true, true));
                out = toLines(t);
            };
            auto runnerFor = [&](W& w) {
                return [&w, answer](const std::wstring& args, std::vector<std::wstring>& out) -> DWORD {
                    w.cmds.push_back(args);
                    if (args.rfind(L"package installed", 0) == 0) { answer(w, out); return w.listExit != 0 ? w.listExit : (w.mode == 4 && !(w.updated && w.modeAfterUpdate >= 0) ? 2 : 0); }
                    if (args.rfind(L"package update", 0) == 0) { ++w.updates; w.cur = w.afterUpdate; w.updated = true; return w.updateExit; }
                    if (args.rfind(L"package install", 0) == 0) { ++w.installs; w.cur = w.afterReinstall; return 0; }
                    return 0;
                };
            };
            UpdPackage sp = pk;   // 7zip.7zip / winget / target 1.1.0
            {
                W w; const UpdProvider pr = UpdMakeUniGetProviderWith(true, runnerFor(w));
                std::string v; std::wstring why;
                Check(pr.readInstalledVersion && pr.readInstalledVersion(sp, v, why) && v == "1.0.0", "upd1 [B1] provider: the installed version is read from the JSON listing");
                Check(!w.cmds.empty() && w.cmds[0] == L"package installed --manager \"winget\"", "upd1 [B1] provider: the command is `package installed --manager \"winget\"` (read-only)");
                W w2; w2.mode = 1; const UpdProvider p2 = UpdMakeUniGetProviderWith(true, runnerFor(w2));
                Check(!p2.readInstalledVersion(sp, v, why) && v.empty() && why.find(L"printed nothing") != std::wstring::npos, "upd1 [B1] provider: exit 0 with NO output (an unknown command) is not a confirmation");
                W w3; w3.mode = 2; const UpdProvider p3 = UpdMakeUniGetProviderWith(true, runnerFor(w3));
                Check(!p3.readInstalledVersion(sp, v, why) && why.find(L"not understood") != std::wstring::npos, "upd1 [B1] provider: malformed JSON is not a confirmation");
                W w4; w4.mode = 3; const UpdProvider p4 = UpdMakeUniGetProviderWith(true, runnerFor(w4));
                Check(!p4.readInstalledVersion(sp, v, why), "upd1 [B1] provider: an unsupported JSON shape is not a confirmation");
                W w5; w5.mode = 4; const UpdProvider p5 = UpdMakeUniGetProviderWith(true, runnerFor(w5));
                Check(!p5.readInstalledVersion(sp, v, why) && why.find(L"token is not available") != std::wstring::npos, "upd1 [B1] provider: exit 2 with an error envelope is not a confirmation and keeps UniGetUI's message");
                W w6; w6.mode = 5; const UpdProvider p6 = UpdMakeUniGetProviderWith(true, runnerFor(w6));
                Check(!p6.readInstalledVersion(sp, v, why), "upd1 [B1] provider: an 'Unknown' version is not a confirmation");
                W w7; w7.mode = 6; const UpdProvider p7 = UpdMakeUniGetProviderWith(true, runnerFor(w7));
                Check(!p7.readInstalledVersion(sp, v, why), "upd1 [B1] provider: an `updates`-shaped answer is not installed state");
                W w8; w8.listExit = kRunTimedOut; const UpdProvider p8 = UpdMakeUniGetProviderWith(true, runnerFor(w8));
                Check(!p8.readInstalledVersion(sp, v, why) && why.find(L"timed out") != std::wstring::npos, "upd1 [B1] provider: a timed-out listing is not a confirmation");
                W w9; w9.listExit = 1; const UpdProvider p9 = UpdMakeUniGetProviderWith(true, runnerFor(w9));
                Check(!p9.readInstalledVersion(sp, v, why), "upd1 [B1] provider: a failing exit code is not a confirmation even if the output parses");
                Check(UpdMakeUniGetProviderWith(false, runnerFor(w)).uniGetAvailable == false, "upd1 [B1] provider: availability is passed through");
            }
            // ---- full updater runs on top of the real parsing path
            auto runWith = [&](W& w, bool autoRb, UpdPackage pkg, const wchar_t* dirName) {
                const AcbRoots rt = freshRoots(dirName, true);
                UpdContext c; c.roots = rt; c.journal = false; c.autoRollback = autoRb;
                c.provider = UpdMakeUniGetProviderWith(true, runnerFor(w));
                return UpdRun(c, pkg);
            };
            {
                W w; const UpdReport r = runWith(w, true, sp, L"b1_ok");
                Check(r.outcome == UpdOutcome::Succeeded && r.versionBefore == "1.0.0" && r.versionAfter == "1.1.0" && r.versionAfterConfirmed, "upd1 [B1] run: success only because the JSON listing independently shows 1.0.0 -> 1.1.0");
                Check(w.updates == 1 && w.installs == 0, "upd1 [B1] run: one update command, no rollback");
            }
            {   // the update command says success but the listing afterwards is unusable: never success, never a rollback
                const struct { int mode; const char* name; } afterModes[] = {{1, "empty output"}, {2, "malformed JSON"}, {3, "an unsupported document"}, {4, "an error envelope"}, {5, "an 'Unknown' version"}, {6, "an updates-shaped document"}};
                for (const auto& am : afterModes) {
                    W w; w.modeAfterUpdate = am.mode;
                    const UpdReport r = runWith(w, true, sp, L"b1_after");
                    Check(r.outcome == UpdOutcome::PartiallySucceeded && !r.versionAfterConfirmed, std::string("upd1 [B1] run: exit 0 then ") + am.name + " = Partially succeeded, version not confirmed");
                    Check(w.installs == 0 && r.rollbackResult == UpdRollbackResult::NotNeeded && !r.verdict.needsRollback, std::string("upd1 [B1] run: ... and autoRollback did NOT fire after ") + am.name);
                }
            }
            {   // before the update the format is unusable: the update is not started at all
                const int beforeModes[] = {1, 2, 3, 4, 5, 6};
                for (int bm : beforeModes) {
                    W w; w.mode = bm;
                    const UpdReport r = runWith(w, true, sp, L"b1_before");
                    Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && w.updates == 0 && !r.commandRan, "upd1 [B1] run: an unusable installed listing before the update (mode " + std::to_string(bm) + ") aborts without running the update");
                }
                W wn; wn.mode = 0;
                UpdPackage missing = sp; missing.id = "Not.Installed";
                const UpdReport rn = runWith(wn, true, missing, L"b1_notlisted");
                Check(rn.outcome == UpdOutcome::AbortedBeforeUpdate && wn.updates == 0, "upd1 [B1] run: a package that is not in the installed list is not updated");
            }
            {   // 2.8.6: a program listed with the version "Unknown" (installed outside winget): refused by default, updated only on "Update anyway", never for other reasons
                W w; w.mode = 5; w.modeAfterUpdate = 0; w.afterUpdate = "5.0.1";
                const UpdReport r0 = runWith(w, true, sp, L"b1_unk_default");
                Check(r0.outcome == UpdOutcome::AbortedBeforeUpdate && w.updates == 0, "upd 2.8.6: by default an 'Unknown' installed version still aborts (nothing changes)");
                W w1; w1.mode = 5; w1.modeAfterUpdate = 0; w1.afterUpdate = "5.0.1";
                UpdPackage any = sp; any.allowUnknownBefore = true;
                const UpdReport r1 = runWith(w1, true, any, L"b1_unk_any");
                Check(r1.outcome == UpdOutcome::Succeeded && w1.updates == 1 && r1.versionAfterConfirmed && r1.versionAfter == "5.0.1" && w1.installs == 0 && r1.versionBefore.empty(),
                      "upd 2.8.6: 'Update anyway' runs the update, reads the version after, reports success, and never fires a rollback (no earlier version exists)");
                W w2; w2.mode = 5; w2.modeAfterUpdate = 1;   // after the update the listing is unusable: not a success
                const UpdReport r2 = runWith(w2, true, any, L"b1_unk_after_bad");
                Check(r2.outcome == UpdOutcome::PartiallySucceeded && !r2.versionAfterConfirmed && w2.installs == 0, "upd 2.8.6: 'Update anyway' with an unusable listing afterwards is not claimed as success");
                W w3; w3.mode = 5; w3.modeAfterUpdate = 0; w3.afterUpdate = "5.0.1"; w3.updateExit = 1;
                const UpdReport r3 = runWith(w3, true, any, L"b1_unk_exit");
                Check(r3.outcome != UpdOutcome::Succeeded, "upd 2.8.6: 'Update anyway' with a failing command is never a success");
                W w4; w4.mode = 1; w4.modeAfterUpdate = 0; UpdPackage any2 = sp; any2.allowUnknownBefore = true;
                const UpdReport r4 = runWith(w4, true, any2, L"b1_unk_empty");
                Check(r4.outcome == UpdOutcome::AbortedBeforeUpdate && w4.updates == 0, "upd 2.8.6: 'Update anyway' does not apply when the listing itself is unusable (only a program that IS listed as Unknown)");
                UpdListedPackage a1; a1.id = "A"; a1.version = "Unknown"; UpdListedPackage a2; a2.id = "B"; a2.version = "1.2.3"; UpdListedPackage a3; a3.id = "C"; a3.version = "";
                size_t uc = 0; const auto kept = UpdOnlyKnownVersions({a1, a2, a3}, &uc);
                Check(UpdListedVersionUnknown(a1) && !UpdListedVersionUnknown(a2) && UpdListedVersionUnknown(a3) && kept.size() == 1 && kept[0].id == "B" && uc == 2, "upd 2.8.6: Update all leaves out the programs whose installed version is Unknown or empty");
                Check(UpdUiConfirmUnknownText(a1).find(L"anyway") != std::wstring::npos && UpdUiConfirmUnknownText(a1).find(L"roll") != std::wstring::npos, "upd 2.8.6: the 'Update anyway' question says there is no rollback");
            }
            {   // exit 0 and the listing still shows the old version
                W w; w.afterUpdate = "1.0.0";
                const UpdReport r = runWith(w, true, sp, L"b1_same");
                Check(r.outcome == UpdOutcome::Failed && !r.verdict.versionChanged && w.installs == 0, "upd1 [B1] run: exit 0 but the listing shows the old version = Failed, no rollback");
            }
            {   // a real downgrade confirmed by the listing is the only case that offers rollback
                W w; w.afterUpdate = "0.9.0"; w.afterReinstall = "1.0.0";
                const UpdReport r = runWith(w, true, sp, L"b1_down");
                Check(r.verdict.needsRollback && w.installs == 1 && r.binaryRestored, "upd1 [B1] run: a version confirmed LOWER by the listing triggers the declared rollback, verified by a second listing");
            }
        }

        // ---- [UG] UPD2-A: the isolated, AnkhCore-owned UniGetUI session (mock platform; no process, no pipe, no registry)
        {
            // pure helpers
            const std::wstring pipeA = UgPrivatePipeName(L"C:\\ProgramData\\AnkhCore", L"alice");
            Check(UgValidPipeName(pipeA), "upd1 [UG] pipe: a generated private pipe name is valid");
            Check(pipeA == UgPrivatePipeName(L"C:\\ProgramData\\AnkhCore", L"alice") && pipeA == UgPrivatePipeName(L"c:\\programdata\\ankhcore", L"ALICE"), "upd1 [UG] pipe: the name is stable and case-insensitive for the same folder and user");
            Check(pipeA != UgPrivatePipeName(L"C:\\ProgramData\\AnkhCore", L"bob") && pipeA != UgPrivatePipeName(L"D:\\Other", L"alice"), "upd1 [UG] pipe: another user or data folder gets another name");
            Check(pipeA.rfind(L"AnkhCore.UniGetUI.", 0) == 0 && pipeA != kUgDefaultPipe, "upd1 [UG] pipe: it can never equal UniGetUI's default pipe");
            Check(!UgValidPipeName(kUgDefaultPipe), "upd1 [UG] pipe: the default pipe name 'UniGetUI.IPC' is not accepted");
            Check(!UgValidPipeName(L"") && !UgValidPipeName(L"AnkhCore.UniGetUI.") && !UgValidPipeName(L"AnkhCore.UniGetUI.ABCDEF0123456789"), "upd1 [UG] pipe: empty, prefix-only and upper-case names are refused");
            Check(!UgValidPipeName(L"AnkhCore.UniGetUI.0123456789abcdef\" --x") && !UgValidPipeName(L"AnkhCore.UniGetUI.0123 4567"), "upd1 [UG] pipe: quotes and spaces are refused");
            Check(!UgValidPipeName(L"Other.Prefix.0123456789abcdef") && !UgValidPipeName(L"AnkhCore.UniGetUI.0123456789abcdef0123456789abcdef0"), "upd1 [UG] pipe: another prefix or an overlong name is refused");

            const UgStatus okSt = UgParseStatus("{\r\n  \"running\": true,\r\n  \"transport\": \"named-pipe\",\r\n  \"tcpPort\": 7058,\r\n  \"namedPipeName\": \"X\",\r\n  \"namedPipePath\": \"\",\r\n  \"baseAddress\": \"http://localhost\",\r\n  \"version\": \"2026.3.3\",\r\n  \"buildNumber\": 107\r\n}\r\n");
            Check(okSt.parsed && okSt.running && okSt.transport == "named-pipe" && okSt.pipeName == "X" && okSt.version == "2026.3.3" && okSt.build == 107, "upd1 [UG] status: the real-shape document is parsed");
            Check(!UgParseStatus("").parsed && !UgParseStatus("not json").parsed && !UgParseStatus("{\"transport\":\"named-pipe\"}").parsed && !UgParseStatus("[]").parsed, "upd1 [UG] status: empty, malformed, array and running-less documents are not parsed");
            std::wstring why;
            auto st = [](bool running, const char* transport, const char* pipe) {
                UgStatus s; s.parsed = true; s.running = running; s.transport = transport; s.pipeName = pipe; return s; };
            Check(UgStatusMatches(st(true, "named-pipe", "P"), L"P", why), "upd1 [UG] status: running + named-pipe + the expected pipe = valid");
            Check(!UgStatusMatches(st(false, "named-pipe", "P"), L"P", why), "upd1 [UG] status: running=false is not a session (the real status exits 0 here)");
            Check(!UgStatusMatches(st(true, "tcp", "P"), L"P", why), "upd1 [UG] status: another transport is refused");
            Check(!UgStatusMatches(st(true, "named-pipe", "UniGetUI.IPC"), L"P", why) && why.find(L"different pipe") != std::wstring::npos, "upd1 [UG] status: the user's default pipe answering is refused");
            Check(!UgStatusMatches(st(true, "named-pipe", "p"), L"P", why), "upd1 [UG] status: the pipe name must match exactly");
            Check(!UgStatusMatches(UgStatus{}, L"P", why), "upd1 [UG] status: an unparsed answer is refused");

            Check(UgArgsAllowed(L"package installed --manager \"winget\"") && UgArgsAllowed(L"app quit") && UgArgsAllowed(L"status"), "upd1 [UG] args: ordinary commands are allowed");
            Check(!UgArgsAllowed(L"status --pipe-name UniGetUI.IPC") && !UgArgsAllowed(L"--PIPE-NAME x status"), "upd1 [UG] args: a caller-supplied --pipe-name is refused (any case)");
            Check(!UgArgsAllowed(L"status --transport tcp") && !UgArgsAllowed(L"status --tcp-port 7058") && !UgArgsAllowed(L"status --ipc-api-pipe-name x"), "upd1 [UG] args: transport, port and server-side options are refused");
            Check(!UgArgsAllowed(L""), "upd1 [UG] args: an empty command is refused");
            Check(UgBuildCommand(L"P", L"status") == L"--transport named-pipe --pipe-name \"P\" status", "upd1 [UG] args: every command is prefixed with the explicit transport and private pipe");
            Check(UgBuildLaunchArgs(L"P") == L"--headless --ipc-api-transport named-pipe --ipc-api-pipe-name \"P\"", "upd1 [UG] args: the session is launched headless on the private pipe");
            Check(UgVersionCompatible("2026.3.3.0") && UgVersionCompatible("2026.4.0.1") && !UgVersionCompatible("2026.3.2.0") && !UgVersionCompatible("") && !UgVersionCompatible("abc"), "upd1 [UG] version: 2026.3.3 and newer only; unknown is refused");
            {
                UgRecord r; r.pipe = pipeA; r.pid = 42; r.pidStart = "123"; r.ownerPid = 7; r.ownerStart = "456"; r.phase = "ready"; r.exe = L"C:\\x\\UniGetUI.exe"; r.started = "2026-11-19T00:00:00Z";
                UgRecord o;
                Check(UgParseRecord(UgRecordJson(r), o) && o.pipe == pipeA && o.pid == 42 && o.phase == "ready" && o.exe == r.exe, "upd1 [UG] record: round trip");
                UgRecord foreign = r; foreign.pipe = L"UniGetUI.IPC";
                Check(!UgParseRecord(UgRecordJson(foreign), o), "upd1 [UG] record: a record for the default pipe is rejected");
                UgRecord badPhase = r; badPhase.phase = "weird";
                Check(!UgParseRecord(UgRecordJson(badPhase), o), "upd1 [UG] record: an unknown phase is rejected");
                UgRecord badStart = r; badStart.pidStart = "12x";
                Check(!UgParseRecord(UgRecordJson(badStart), o), "upd1 [UG] record: a non-numeric start time is rejected");
            }
        }
        {
            // ---- the fake machine: a user session on the DEFAULT pipe that must never be reached, and whatever AnkhCore starts
            struct Srv { DWORD pid = 0; long pollsLeft = 0; std::wstring answerPipe; };
            struct World {
                std::mutex m;
                std::map<DWORD, std::pair<std::string, bool>> procs;     // pid -> (start time, alive)
                std::map<std::wstring, Srv> servers;                      // pipe -> server
                DWORD nextPid = 5000;
                const DWORD userPid = 3272;
                int defaultTouched = 0, userKilled = 0, launches = 0, polls = 0;
                std::vector<std::wstring> cmds, launchArgs, quitPipes;
                std::vector<DWORD> terminated;
                int mode = 0;               // 0 ok, 1 launch fails, 2 dies at once, 3 never answers, 4 answers on the default pipe, 6 empty status, 7 malformed status
                long readyAfter = 1;
                std::string cur = "1.0.0", afterUpdate = "1.1.0", afterReinstall = "1.0.0";
                int updates = 0, installs = 0;
                int launchDelayMs = 0;
                bool quitIgnored = false, loseToken = false;
                std::string updatesJson = "{\"status\": \"success\", \"updates\": []}";   // answer to `package updates`
                std::string opList;                       // stdout of `operation list`
                DWORD opListRc = 0;
                DWORD updateRc = 0;                       // exit code of `package update` (kRunTimedOut simulates a hung update)
                std::atomic<bool> hold{false};            // while true, `package update` blocks (a long update)
                std::atomic<int> holding{0};              // how many update commands are blocked right now
                std::set<std::thread::id> cmdThreads;     // which threads sent commands
                World() {
                    procs[userPid] = {"999", true};
                    servers[L"UniGetUI.IPC"] = Srv{userPid, 0, L"UniGetUI.IPC"};
                    procs[7001] = {"111", true}; procs[7002] = {"222", true};   // two fake AnkhCore processes (owners)
                }
            };
            auto pipeOf = [](const std::wstring& a, const wchar_t* key) -> std::wstring {
                const size_t k = a.find(key); if (k == std::wstring::npos) return L"";
                const size_t q1 = a.find(L'"', k); if (q1 == std::wstring::npos) return L"";
                const size_t q2 = a.find(L'"', q1 + 1); if (q2 == std::wstring::npos) return L"";
                return a.substr(q1 + 1, q2 - q1 - 1);
            };
            auto statusJson = [](bool running, const std::wstring& pipe) {
                return Utf8ToW(std::string("{\"running\": ") + (running ? "true" : "false") + ", \"transport\": \"named-pipe\", \"tcpPort\": 7058, \"namedPipeName\": \"" + WToUtf8(pipe) + "\", \"namedPipePath\": \"\", \"baseAddress\": \"http://localhost\", \"version\": \"2026.3.3\", \"buildNumber\": 107}");
            };
            auto platformFor = [&](World& w) {
                UgPlatform p;
                p.launch = [&w, pipeOf](const std::wstring&, const std::wstring& args, DWORD& pid, std::string& start) -> bool {
                    int delay;
                    { std::lock_guard<std::mutex> g(w.m); ++w.launches; w.launchArgs.push_back(args); if (w.mode == 1) return false; delay = w.launchDelayMs; }
                    if (delay) Sleep(delay);
                    std::lock_guard<std::mutex> g(w.m);
                    pid = w.nextPid++; start = std::to_string(1000 + pid);
                    w.procs[pid] = {start, w.mode != 2};
                    const std::wstring pipe = pipeOf(args, L"--ipc-api-pipe-name");
                    Srv s; s.pid = pid; s.pollsLeft = (w.mode == 3) ? 1000000 : w.readyAfter; s.answerPipe = (w.mode == 4) ? L"UniGetUI.IPC" : pipe;
                    w.servers[pipe] = s;
                    return true;
                };
                p.cli = [&w, pipeOf, statusJson](const std::wstring& args, std::vector<std::wstring>& out) -> DWORD {
                    if (args.find(L"package update") != std::wstring::npos && args.find(L"package updates") == std::wstring::npos)
                        if (w.hold.load()) { ++w.holding; while (w.hold.load()) Sleep(1); --w.holding; }
                    std::lock_guard<std::mutex> g(w.m);
                    w.cmds.push_back(args); w.cmdThreads.insert(std::this_thread::get_id());
                    std::wstring pipe = pipeOf(args, L"--pipe-name");
                    if (args.find(L"--pipe-name") == std::wstring::npos) { pipe = L"UniGetUI.IPC"; ++w.defaultTouched; }   // would reach the user's session
                    if (pipe == L"UniGetUI.IPC" && args.find(L"--pipe-name") != std::wstring::npos) ++w.defaultTouched;     // an explicit default is just as wrong
                    auto it = w.servers.find(pipe);
                    const bool up = it != w.servers.end() && w.procs[it->second.pid].second;
                    if (w.loseToken && args.find(L"package") != std::wstring::npos) { out = {L"{\"status\": \"error\", \"command\": \"\", \"message\": \"The IPC API token is not available. Start UniGetUI and try again.\"}"}; return 2; }
                    const bool isStatus = args.find(L" status") != std::wstring::npos && args.find(L"package") == std::wstring::npos;
                    if (isStatus) {
                        ++w.polls;
                        if (!up) { out = {statusJson(false, pipe)}; return 0; }
                        Srv& s = it->second;
                        if (s.pollsLeft > 0) { --s.pollsLeft; out = {statusJson(false, pipe)}; return 0; }
                        if (w.mode == 6) return 0;                                   // empty output, exit 0
                        if (w.mode == 7) { out = {L"{\"running\": tr"}; return 0; }   // truncated
                        out = {statusJson(true, s.answerPipe)};
                        return 0;
                    }
                    if (!up) { out = {L"{", L"\"status\": \"error\",", L"\"command\": \"\",", L"\"message\": \"The IPC API token is not available. Start UniGetUI and try again.\"", L"}"}; return 2; }
                    if (args.find(L"app quit") != std::wstring::npos) {
                        w.quitPipes.push_back(pipe);
                        if (!w.quitIgnored) w.procs[it->second.pid].second = false;
                        out = {L"{\"status\": \"success\", \"command\": \"quit-app\", \"message\": null}"};
                        return 0;
                    }
                    if (args.find(L"package installed") != std::wstring::npos) {
                        out = {Utf8ToW("{\"status\": \"success\", \"packages\": [{\"name\": \"T\", \"id\": \"7zip.7zip\", \"version\": \"" + w.cur + "\", \"newVersion\": \"\", \"source\": \"WinGet: winget\", \"manager\": \"winget\", \"isUpgradable\": false, \"systemRestartPending\": false}]}")};
                        return 0;
                    }
                    if (args.find(L"operation list") != std::wstring::npos) { if (!w.opList.empty()) out = {Utf8ToW(w.opList)}; return w.opListRc; }
                    if (args.find(L"package updates") != std::wstring::npos) { out = {Utf8ToW(w.updatesJson)}; return 0; }
                    if (args.find(L"package update") != std::wstring::npos) { ++w.updates; w.cur = w.afterUpdate; return w.updateRc; }
                    if (args.find(L"package install") != std::wstring::npos) { ++w.installs; w.cur = w.afterReinstall; return 0; }
                    return 0;
                };
                p.alive = [&w](DWORD pid, const std::string& start) -> bool {
                    std::lock_guard<std::mutex> g(w.m);
                    auto it = w.procs.find(pid);
                    return it != w.procs.end() && it->second.second && it->second.first == start;
                };
                p.terminate = [&w](DWORD pid, const std::string& start) -> bool {
                    std::lock_guard<std::mutex> g(w.m);
                    auto it = w.procs.find(pid);
                    if (it == w.procs.end() || it->second.first != start) return false;
                    if (pid == w.userPid) ++w.userKilled;
                    w.terminated.push_back(pid);
                    it->second.second = false;
                    return true;
                };
                p.sleep = [&w](DWORD) { std::lock_guard<std::mutex> g(w.m); };
                return p;
            };
            auto cfgFor = [&](const AcbRoots& rt, DWORD owner = 7001, const char* ownerStart = "111") {
                UgConfig c;
                c.ug.found = true; c.ug.portable = true; c.ug.dir = L"X:\\ug"; c.ug.guiExe = L"X:\\ug\\UniGetUI.exe"; c.ug.cliExe = L"X:\\ug\\uniget.exe";
                c.roots = rt; c.pipe = UgPrivatePipeName(rt.data, L"tester"); c.exeVersion = "2026.3.3.0";
                c.startAttempts = 6; c.pollMs = 1; c.quitAttempts = 3; c.ownerPid = owner; c.ownerStart = ownerStart;
                return c;
            };
            auto privateOnly = [&](const World& w, const std::wstring& pipe) {   // every command AnkhCore sent carries the private pipe
                for (const auto& c : w.cmds) if (c.rfind(L"--transport named-pipe --pipe-name \"" + pipe + L"\" ", 0) != 0) return false;
                return true;
            };

            // ---- start, validate, reuse, run, shut down
            {
                const AcbRoots rt = freshRoots(L"ug_basic", true);
                World w; UgSession s(cfgFor(rt), platformFor(w));
                const UgResult r = s.Ensure();
                Check(r.ok && r.code == UgCode::Started && r.state == UgState::Ready, "upd1 [UG] start: the session is started and validated");
                Check(w.launches == 1 && w.launchArgs[0] == UgBuildLaunchArgs(s.Pipe()), "upd1 [UG] start: exactly one launch, headless, on the private pipe");
                Check(w.launchArgs[0].find(L"UniGetUI.IPC") == std::wstring::npos, "upd1 [UG] start: the launch never mentions the default pipe");
                Check(w.polls >= 2 && !w.cmds.empty(), "upd1 [UG] start: it waited until status answered (the first status said running=false)");
                UgRecord rec;
                Check(UgReadRecord(rt, rec) == UgRecStatus::Ok && rec.phase == "ready" && rec.pid == 5000 && rec.pipe == s.Pipe() && rec.ownerPid == 7001, "upd1 [UG] start: the session record is ready and names the process AnkhCore started");
                const UgResult again = s.Ensure();
                Check(again.ok && again.code == UgCode::Reused && w.launches == 1, "upd1 [UG] reuse: a second Ensure validates and reuses it, no second launch");
                std::vector<std::wstring> out;
                const DWORD rc = s.Run(L"package installed --manager \"winget\"", out);
                Check(rc == 0 && w.cmds.back() == UgBuildCommand(s.Pipe(), L"package installed --manager \"winget\""), "upd1 [UG] run: the command carries --transport named-pipe --pipe-name <private>");
                const size_t before = w.cmds.size();
                for (const wchar_t* bad : {L"status --pipe-name UniGetUI.IPC", L"status --transport tcp", L"status --tcp-port 7058", L"status --ipc-api-pipe-name x"}) {
                    std::vector<std::wstring> o2;
                    Check(s.Run(bad, o2) == kRunLaunchFailed, std::string("upd1 [UG] run: a caller-chosen session option is refused: ") + WToUtf8(bad));
                }
                Check(w.cmds.size() == before, "upd1 [UG] run: ... and nothing was sent for them");
                Check(privateOnly(w, s.Pipe()) && w.defaultTouched == 0, "upd1 [UG] run: EVERY command sent so far targeted the private pipe; none reached the default pipe");
                const UgResult down = s.Shutdown();
                Check(down.ok && s.State() == UgState::Stopped && !w.procs[5000].second, "upd1 [UG] shutdown: the AnkhCore session is quit and gone");
                Check(w.quitPipes.size() == 1 && w.quitPipes[0] == s.Pipe(), "upd1 [UG] shutdown: `app quit` was sent once, on the private pipe only");
                Check(w.terminated.empty(), "upd1 [UG] shutdown: nothing had to be force-ended");
                Check(UgReadRecord(rt, rec) == UgRecStatus::Missing, "upd1 [UG] shutdown: the session record is removed");
                Check(w.procs[w.userPid].second && w.userKilled == 0 && w.defaultTouched == 0 && w.servers[L"UniGetUI.IPC"].pid == w.userPid, "upd1 [UG] shutdown: the user's default session is alive and was never touched");
                std::vector<std::wstring> o3;
                Check(s.Run(L"status", o3) == kRunLaunchFailed, "upd1 [UG] shutdown: after it, no command can run");
                Check(privateOnly(w, s.Pipe()), "upd1 [UG] shutdown: ... and every command including `app quit` stayed on the private pipe");
            }
            {   // shutting down something that was never started touches nothing
                const AcbRoots rt = freshRoots(L"ug_nostart", true);
                World w; UgSession s(cfgFor(rt), platformFor(w));
                const UgResult down = s.Shutdown();
                Check(down.ok && w.cmds.empty() && w.terminated.empty() && w.launches == 0, "upd1 [UG] shutdown: with no AnkhCore session it sends nothing and ends nothing");
                std::vector<std::wstring> o;
                Check(s.Run(L"status", o) == kRunLaunchFailed && w.cmds.empty(), "upd1 [UG] run: before Ensure no command is possible");
            }
            // ---- configuration that is refused before anything runs
            {
                struct Bad { const char* name; std::function<void(UgConfig&)> mut; };
                const Bad bads[] = {
                    {"the default pipe name", [](UgConfig& c) { c.pipe = L"UniGetUI.IPC"; }},
                    {"a pipe without the AnkhCore prefix", [](UgConfig& c) { c.pipe = L"Some.Other.0123456789abcdef"; }},
                    {"no UniGetUI found", [](UgConfig& c) { c.ug.found = false; }},
                    {"an installed (non-portable) copy", [](UgConfig& c) { c.ug.portable = false; }},
                    {"an unknown version", [](UgConfig& c) { c.exeVersion.clear(); }},
                    {"an old version", [](UgConfig& c) { c.exeVersion = "2025.9.1.0"; }},
                };
                for (const Bad& b : bads) {
                    const AcbRoots rt = freshRoots(L"ug_cfg", true);
                    World w; UgConfig c = cfgFor(rt); b.mut(c);
                    UgSession s(c, platformFor(w));
                    const UgResult r = s.Ensure();
                    Check(!r.ok && r.code == UgCode::Invalid && s.State() == UgState::Invalid, std::string("upd1 [UG] config: refused: ") + b.name);
                    Check(w.launches == 0 && w.cmds.empty(), std::string("upd1 [UG] config: ... and nothing ran for: ") + b.name);
                }
            }
            // ---- start failures: no usable session, nothing else harmed
            {
                const AcbRoots rt = freshRoots(L"ug_f1", true);
                World w; w.mode = 1; UgSession s(cfgFor(rt), platformFor(w));
                const UgResult r = s.Ensure(); UgRecord rec;
                Check(!r.ok && s.State() == UgState::Terminated && UgReadRecord(rt, rec) == UgRecStatus::Missing, "upd1 [UG] fail: a launch that fails leaves no session and no record");
                std::vector<std::wstring> o; Check(s.Run(L"package installed", o) == kRunLaunchFailed, "upd1 [UG] fail: ... and blocks every command");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_f2", true);
                World w; w.mode = 2; UgSession s(cfgFor(rt), platformFor(w));
                const UgResult r = s.Ensure(); UgRecord rec;
                Check(!r.ok && s.State() == UgState::Terminated && r.message.find(L"exited while starting") != std::wstring::npos, "upd1 [UG] fail: a process that dies during startup is detected");
                Check(w.terminated.empty() && UgReadRecord(rt, rec) == UgRecStatus::Missing, "upd1 [UG] fail: ... nothing is ended (it was already gone) and the record is released");
            }
            {
                struct M { int mode; const char* name; };
                const M ms[] = {{3, "never answers (timeout)"}, {4, "answers on the user's default pipe name"}, {6, "status exit 0 with empty output"}, {7, "status with truncated JSON"}};
                for (const M& m : ms) {
                    const AcbRoots rt = freshRoots(L"ug_f3", true);
                    World w; w.mode = m.mode; UgSession s(cfgFor(rt), platformFor(w)); UgRecord rec;
                    const UgResult r = s.Ensure();
                    Check(!r.ok && s.State() == UgState::Invalid, std::string("upd1 [UG] fail: a session that ") + m.name + " is Invalid, not usable");
                    Check(w.terminated.size() == 1 && w.terminated[0] == 5000 && w.userKilled == 0, std::string("upd1 [UG] fail: ... only the process AnkhCore itself started was ended (") + m.name + ")");
                    Check(w.procs[w.userPid].second && w.defaultTouched == 0 && UgReadRecord(rt, rec) == UgRecStatus::Missing, std::string("upd1 [UG] fail: ... the user's session untouched, record released (") + m.name + ")");
                    std::vector<std::wstring> o; Check(s.Run(L"package installed", o) == kRunLaunchFailed, std::string("upd1 [UG] fail: ... commands blocked (") + m.name + ")");
                    Check(privateOnly(w, s.Pipe()), std::string("upd1 [UG] fail: ... all commands stayed private (") + m.name + ")");
                }
                const AcbRoots rt = freshRoots(L"ug_f3b", true);
                World w; w.mode = 3; UgConfig c = cfgFor(rt); c.startAttempts = 5; UgSession s(c, platformFor(w));
                s.Ensure();
                Check(w.polls == 5 + 1, "upd1 [UG] fail: the wait is bounded by startAttempts (one pre-launch probe + 5 polls)");
            }
            {   // something already answers on OUR pipe: not ours, not used, not touched
                const AcbRoots rt = freshRoots(L"ug_foreign", true);
                World w; UgConfig c = cfgFor(rt);
                w.procs[6000] = {"x", true}; w.servers[c.pipe] = Srv{6000, 0, c.pipe};
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure();
                Check(!r.ok && s.State() == UgState::Invalid && r.message.find(L"did not start") != std::wstring::npos, "upd1 [UG] foreign: a session already on the private pipe that AnkhCore did not start is refused");
                Check(w.launches == 0 && w.terminated.empty() && w.quitPipes.empty() && w.procs[6000].second, "upd1 [UG] foreign: ... it is neither replaced, quit nor ended");
                std::vector<std::wstring> o; Check(s.Run(L"package installed", o) == kRunLaunchFailed, "upd1 [UG] foreign: ... and nothing runs on it");
                Check(s.Shutdown().ok && w.quitPipes.empty() && w.terminated.empty(), "upd1 [UG] foreign: Shutdown does not touch a session AnkhCore never owned");
            }
            // ---- unexpected termination, mismatch, lost token
            {
                const AcbRoots rt = freshRoots(L"ug_term", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                w.procs[5000].second = false;   // the session crashes
                std::vector<std::wstring> o;
                const size_t n = w.cmds.size();
                Check(s.Run(L"package installed", o) == kRunLaunchFailed && s.State() == UgState::Terminated && w.cmds.size() == n, "upd1 [UG] crash: an unexpected termination is detected before a command is sent");
                Check(!s.Validate().ok && s.State() == UgState::Terminated, "upd1 [UG] crash: Validate reports Terminated");
                const UgResult re = s.Ensure();
                Check(re.ok && re.code == UgCode::Started && w.launches == 2 && w.servers[s.Pipe()].pid == 5001, "upd1 [UG] crash: Ensure starts a fresh session (a second launch)");
                Check(w.terminated.empty() && w.userKilled == 0 && w.defaultTouched == 0, "upd1 [UG] crash: nothing was force-ended and the default session was not touched");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_mismatch", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                w.servers[s.Pipe()].answerPipe = L"UniGetUI.IPC";   // the session now identifies as the default one
                const UgResult v = s.Validate();
                Check(!v.ok && s.State() == UgState::Invalid && v.message.find(L"different pipe") != std::wstring::npos, "upd1 [UG] mismatch: a session answering with another pipe name is Invalid");
                std::vector<std::wstring> o; Check(s.Run(L"package installed", o) == kRunLaunchFailed, "upd1 [UG] mismatch: ... and blocks commands");
                const UgResult re = s.Ensure();
                Check(!re.ok && w.launches == 1, "upd1 [UG] mismatch: Ensure refuses to start a second session over it");
                Check(w.terminated.empty() && w.defaultTouched == 0, "upd1 [UG] mismatch: nothing was ended or reached on the default pipe");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_token", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                w.loseToken = true;   // the session loses its token: commands now fail with the real error envelope
                std::vector<std::wstring> o;
                const DWORD rc = s.Run(L"package installed --manager \"winget\"", o);
                Check(rc == 2 && s.State() == UgState::Invalid, "upd1 [UG] token: a 'token is not available' answer marks the session Invalid");
                std::vector<std::wstring> o2; Check(s.Run(L"package installed", o2) == kRunLaunchFailed, "upd1 [UG] token: ... and later commands are blocked until it is validated again");
            }
            // ---- updates are gated by the session
            UpdPackage ugPk; ugPk.id = "7zip.7zip"; ugPk.name = "7-Zip"; ugPk.manager = "winget"; ugPk.source = "winget"; ugPk.targetVersion = "1.1.0";
            {
                const AcbRoots rt = freshRoots(L"ug_upd_ok", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                UpdContext c; c.roots = rt; c.journal = false; c.provider = UgMakeUpdProvider(s);
                const UpdReport r = UpdRun(c, ugPk);
                Check(r.outcome == UpdOutcome::Succeeded && w.updates == 1, "upd1 [UG] update: a healthy private session lets the update run");
                Check(privateOnly(w, s.Pipe()) && w.defaultTouched == 0, "upd1 [UG] update: every command of the whole update (status, listings, update) went to the private pipe");
                Check(w.procs[w.userPid].second && w.userKilled == 0, "upd1 [UG] update: the user's session was not involved");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_upd_mis", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                w.servers[s.Pipe()].answerPipe = L"UniGetUI.IPC";
                UpdContext c; c.roots = rt; c.journal = false; c.provider = UgMakeUpdProvider(s);
                const UpdReport r = UpdRun(c, ugPk);
                Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && w.updates == 0 && !r.commandRan, "upd1 [UG] update: a session that identifies as another pipe cannot run an update");
                Check(r.headline.find(L"session is not valid") != std::wstring::npos && !FileExists(UpdActivePath(rt)), "upd1 [UG] update: ... the reason is shown and the lock is released");
                bool anyUpdateCmd = false; for (const auto& cmd : w.cmds) if (cmd.find(L"package update") != std::wstring::npos) anyUpdateCmd = true;
                Check(!anyUpdateCmd && w.defaultTouched == 0, "upd1 [UG] update: ... no update command was sent to any pipe");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_upd_dead", true);
                World w; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                w.procs[5000].second = false;
                UpdContext c; c.roots = rt; c.journal = false; c.provider = UgMakeUpdProvider(s);
                const UpdReport r = UpdRun(c, ugPk);
                Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && w.updates == 0, "upd1 [UG] update: a terminated session cannot run an update");
                UpdPackage rb = ugPk; rb.previousVersion = "1.0.0";
                const UpdReport rr = UpdRollback(c, rb, false);
                Check(rr.outcome == UpdOutcome::AbortedBeforeUpdate && w.installs == 0, "upd1 [UG] update: ... nor a rollback");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_upd_none", true);
                World w; UgSession s(cfgFor(rt), platformFor(w));   // never started
                UpdContext c; c.roots = rt; c.journal = false; c.provider = UgMakeUpdProvider(s);
                const UpdReport r = UpdRun(c, ugPk);
                Check(r.outcome == UpdOutcome::AbortedBeforeUpdate && w.cmds.empty() && w.updates == 0, "upd1 [UG] update: without a started session nothing is sent at all");
            }
            // ---- stale, adopted, orphaned, corrupt and concurrent sessions
            {
                const AcbRoots rt = freshRoots(L"ug_stale", true);
                World w; UgConfig c = cfgFor(rt);
                UgRecord dead; dead.pipe = c.pipe; dead.pid = 4321; dead.pidStart = "1"; dead.ownerPid = 7001; dead.ownerStart = "111"; dead.phase = "ready"; dead.exe = c.ug.guiExe; dead.started = AcbUtcStampNow();
                EnsureDir(UpdDir(rt)); UgSaveRecord(rt, dead);
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure(); UgRecord rec;
                Check(r.ok && r.code == UgCode::Started && w.launches == 1, "upd1 [UG] stale: a record whose process is gone is cleaned up and a new session started");
                Check(UgReadRecord(rt, rec) == UgRecStatus::Ok && rec.pid == 5000 && w.terminated.empty(), "upd1 [UG] stale: ... the new record names the new process; nothing was ended");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_adopt", true);
                World w1; UgSession a(cfgFor(rt, 7001, "111"), platformFor(w1)); a.Ensure();   // an earlier AnkhCore starts it and "exits" without shutdown
                UgSession b(cfgFor(rt, 7002, "222"), platformFor(w1));                         // the next AnkhCore
                const UgResult r = b.Ensure(); UgRecord rec;
                Check(r.ok && r.code == UgCode::Adopted && w1.launches == 1, "upd1 [UG] adopt: a later AnkhCore validates and reuses the recorded session, no new launch");
                Check(UgReadRecord(rt, rec) == UgRecStatus::Ok && rec.ownerPid == 7002, "upd1 [UG] adopt: ... the record now names the new owner");
                std::vector<std::wstring> o; Check(b.Run(L"package installed", o) == 0 && privateOnly(w1, b.Pipe()), "upd1 [UG] adopt: ... commands go to the private pipe");
                Check(b.Shutdown().ok && !w1.procs[5000].second && w1.defaultTouched == 0 && w1.procs[w1.userPid].second, "upd1 [UG] adopt: ... and the adopter can shut it down; the default session is untouched");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_adopt_bad", true);
                World w; UgConfig c = cfgFor(rt); UgSession a(c, platformFor(w)); a.Ensure();
                w.servers[c.pipe].answerPipe = L"UniGetUI.IPC";
                UgSession b(cfgFor(rt, 7002, "222"), platformFor(w));
                const UgResult r = b.Ensure();
                Check(!r.ok && r.code == UgCode::Invalid && w.launches == 1 && w.terminated.empty(), "upd1 [UG] adopt: a recorded session that does not validate is refused, not replaced and not ended");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_busy", true);
                World w; UgConfig c = cfgFor(rt);
                UgRecord starting; starting.pipe = c.pipe; starting.pid = 0; starting.ownerPid = 7002; starting.ownerStart = "222"; starting.phase = "starting"; starting.exe = c.ug.guiExe; starting.started = AcbUtcStampNow();
                EnsureDir(UpdDir(rt)); UgSaveRecord(rt, starting);
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure();
                Check(!r.ok && r.code == UgCode::Busy && w.launches == 0 && w.cmds.empty(), "upd1 [UG] concurrent: another live AnkhCore is starting the session = Busy, nothing launched or sent");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_orphan", true);
                World w; UgConfig c = cfgFor(rt);
                w.procs[6100] = {"610001", true};   // the process the dead starter had launched, never validated
                UgRecord starting; starting.pipe = c.pipe; starting.pid = 6100; starting.pidStart = "610001"; starting.ownerPid = 7002; starting.ownerStart = "222"; starting.phase = "starting"; starting.exe = c.ug.guiExe; starting.started = AcbUtcStampNow();
                EnsureDir(UpdDir(rt)); UgSaveRecord(rt, starting);
                w.procs[7002].second = false;      // the starter died
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure();
                Check(r.ok && r.code == UgCode::Started, "upd1 [UG] orphan: after the starter died, a new session is started");
                Check(w.terminated.size() == 1 && w.terminated[0] == 6100 && w.userKilled == 0, "upd1 [UG] orphan: only the unvalidated process AnkhCore itself had launched was ended (identity checked)");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_recycle", true);
                World w; UgConfig c = cfgFor(rt);
                w.procs[6200] = {"another-process", true};   // the pid now belongs to an unrelated process
                UgRecord stale; stale.pipe = c.pipe; stale.pid = 6200; stale.pidStart = "original-start"; stale.ownerPid = 7002; stale.ownerStart = "222"; stale.phase = "ready"; stale.exe = c.ug.guiExe; stale.started = AcbUtcStampNow();
                // pidStart must be digits in a record: use numeric identities
                stale.pidStart = "111222"; w.procs[6200].first = "999888";
                EnsureDir(UpdDir(rt)); UgSaveRecord(rt, stale);
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure();
                Check(r.ok && r.code == UgCode::Started && w.launches == 1, "upd1 [UG] recycled pid: a record whose pid now belongs to another process is stale; a new session is started");
                Check(w.terminated.empty() && w.procs[6200].second, "upd1 [UG] recycled pid: ... the unrelated process is not touched");
            }
            {
                const AcbRoots rt = freshRoots(L"ug_corrupt", true);
                World w; UgConfig c = cfgFor(rt);
                EnsureDir(UpdDir(rt));
                const std::string junk = "{ this is not a session record";
                WriteAtomic(UgRecordPath(rt), reinterpret_cast<const uint8_t*>(junk.data()), junk.size());
                UgSession s(c, platformFor(w));
                const UgResult r = s.Ensure();
                bool kept = false; for (const auto& f : ListDir(UpdDir(rt), false)) if (f.find(L"uniget-session.corrupt-") != std::wstring::npos) kept = true;
                Check(r.ok && r.code == UgCode::Started, "upd1 [UG] corrupt: an unreadable record does not block a safe start");
                Check(kept, "upd1 [UG] corrupt: ... and it is kept aside for inspection, not deleted");
            }
            {   // two AnkhCore instances starting at the same moment: one launch
                const AcbRoots rt = freshRoots(L"ug_threads", true);
                World w; w.launchDelayMs = 400;
                UgSession a(cfgFor(rt, 7001, "111"), platformFor(w)), b(cfgFor(rt, 7002, "222"), platformFor(w));
                UgResult ra, rb;
                std::thread t1([&] { ra = a.Ensure(); });
                Sleep(100);
                std::thread t2([&] { rb = b.Ensure(); });
                t1.join(); t2.join();
                Check(w.launches == 1, "upd1 [UG] concurrent: two simultaneous startups launched exactly one session");
                Check(ra.ok && ra.code == UgCode::Started, "upd1 [UG] concurrent: the first starter owns it");
                Check(!rb.ok && rb.code == UgCode::Busy, "upd1 [UG] concurrent: the second is told Busy and sends no command");
                Check(w.defaultTouched == 0 && w.terminated.empty(), "upd1 [UG] concurrent: the default session was not touched and nothing was ended");
            }
            // ---- a session that ignores `app quit`: only the process AnkhCore started is force-ended, by identity
            {
                const AcbRoots rt = freshRoots(L"ug_stuck", true);
                World w; w.quitIgnored = true; UgSession s(cfgFor(rt), platformFor(w)); s.Ensure();
                const UgResult down = s.Shutdown(); UgRecord rec;
                Check(down.ok && w.terminated.size() == 1 && w.terminated[0] == 5000 && w.userKilled == 0, "upd1 [UG] shutdown: a session that ignores `quit` is ended by identity; the user's process is not");
                Check(w.quitPipes.size() == 1 && w.quitPipes[0] == s.Pipe() && UgReadRecord(rt, rec) == UgRecStatus::Missing, "upd1 [UG] shutdown: the quit went to the private pipe and the record is gone");
            }
            // ---- [UI] UPD2-B: the model behind the Backup & Migration updater section (same fake machine; no real update, no UI, no registry)
            {
                const std::string twoUpdates =
                    "{\"status\": \"success\", \"updates\": ["
                    "{\"name\": \"7-Zip\", \"id\": \"7zip.7zip\", \"version\": \"1.0.0\", \"newVersion\": \"1.1.0\", \"source\": \"WinGet: winget\", \"manager\": \"winget\", \"isUpgradable\": true, \"systemRestartPending\": false},"
                    "{\"name\": \"Other\", \"id\": \"o.other\", \"version\": \"2.0\", \"newVersion\": \"\", \"source\": \"WinGet: winget\", \"manager\": \"winget\", \"isUpgradable\": false, \"systemRestartPending\": false}]}";
                struct UiRig {
                    World w; AcbRoots rt; std::atomic<int> changed{0}; int factories = 0;
                    std::mutex lm; std::vector<std::pair<bool, std::wstring>> logs;
                    std::set<std::thread::id> changedThreads;
                    std::unique_ptr<UpdUiModel> m;
                };
                auto mkRig = [&](const wchar_t* tag, bool found = true) {
                    auto r = std::make_unique<UiRig>();
                    r->rt = freshRoots(tag, true);
                    UiRig* rp = r.get();
                    r->m = std::make_unique<UpdUiModel>(
                        [rp, found, &platformFor, &cfgFor](std::wstring& why) -> std::shared_ptr<UgSession> {
                            ++rp->factories;
                            if (!found) { why = L"UniGetUI was not found."; return nullptr; }
                            return std::make_shared<UgSession>(cfgFor(rp->rt), platformFor(rp->w));
                        },
                        [rp] { ++rp->changed; std::lock_guard<std::mutex> g(rp->lm); rp->changedThreads.insert(std::this_thread::get_id()); },
                        [rp](bool ok, const std::wstring& t) { std::lock_guard<std::mutex> g(rp->lm); rp->logs.push_back({ok, t}); },
                        r->rt, [](UpdContext& c) { c.journal = false; });
                    return r;
                };
                const std::thread::id mainId = std::this_thread::get_id();

                {   // the session starts only when asked for
                    auto r = mkRig(L"ui_lazy"); r->w.updatesJson = twoUpdates;
                    for (int i = 0; i < 5; ++i) (void)r->m->Snapshot();
                    Check(r->factories == 0 && r->w.launches == 0 && r->w.cmds.empty() && r->changed == 0, "upd1 [UI] lazy: creating the model and reading its state (opening the page) starts no session and sends no command");
                    Check(!r->m->Snapshot().sessionCreated && !r->m->Snapshot().busy && !r->m->Snapshot().checked, "upd1 [UI] lazy: ... and the state says so");
                    std::thread t([&] { r->m->Check(); }); t.join();
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(r->factories == 1 && r->w.launches == 1 && s.sessionCreated, "upd1 [UI] lazy: the first check creates and starts exactly one session");
                    Check(s.checked && s.list.size() == 1 && s.list[0].id == "7zip.7zip" && s.status.find(L"1 update(s)") != std::wstring::npos, "upd1 [UI] check: only the package with a newer version is offered, and the status says how many");
                    Check(privateOnly(r->w, UgPrivatePipeName(r->rt.data, L"tester")) && r->w.defaultTouched == 0, "upd1 [UI] check: every command went to the private pipe, none to the default pipe");
                    bool listedWinget = false; for (const auto& c : r->w.cmds) if (c.find(L"package updates --manager \"winget\"") != std::wstring::npos || c.find(L"package updates --manager winget") != std::wstring::npos) listedWinget = true;
                    Check(listedWinget, "upd1 [UI] check: the listing asks for winget only");
                    Check(r->w.updates == 0 && r->w.installs == 0, "upd1 [UI] check: checking never updates or installs anything");
                    Check(r->w.cmdThreads.count(mainId) == 0 && r->w.cmdThreads.size() == 1, "upd1 [UI] threads: every UniGetUI command ran on the worker thread, none on the caller (UI) thread");
                    Check(r->changed >= 2 && r->changedThreads.count(mainId) == 0, "upd1 [UI] threads: the page is told through the change callback from the worker (the page only posts a message), never by touching the UI");
                    Check(r->logs.size() == 1 && r->logs[0].first, "upd1 [UI] check: one log line, marked as success");
                    Check(r->m->Snapshot().busy == false, "upd1 [UI] check: the busy flag is cleared at the end");
                    // repeated checks reuse the one session
                    for (int i = 0; i < 4; ++i) { std::thread t2([&] { r->m->Check(); }); t2.join(); }
                    Check(r->factories == 1 && r->w.launches == 1, "upd1 [UI] repeat: five checks in a row used one session and one launch (no orphan session)");
                    r->m->Shutdown();
                    Check(!r->w.procs[5000].second && r->w.quitPipes.size() == 1 && r->w.quitPipes[0] == UgPrivatePipeName(r->rt.data, L"tester"), "upd1 [UI] close: Shutdown ended the AnkhCore session through `app quit` on the private pipe");
                    Check(r->w.procs[r->w.userPid].second && r->w.userKilled == 0 && r->w.defaultTouched == 0 && r->w.terminated.empty(), "upd1 [UI] close: the user's session was not touched and nothing was force-ended");
                    const size_t cmdsAtClose = r->w.cmds.size();
                    bool res_4496 = true; std::thread t3([&] { res_4496 = r->m->Check(); }); t3.join(); Check(!res_4496, "upd1 [UI] close: a check after Shutdown is refused");
                    Check(r->factories == 1 && r->w.launches == 1 && r->w.cmds.size() == cmdsAtClose, "upd1 [UI] close: after Shutdown nothing starts again (no session created by a late check)");
                    r->m->Shutdown();
                    Check(r->w.quitPipes.size() == 1, "upd1 [UI] close: a second Shutdown sends nothing more");
                }
                {   // opening and closing without ever using the updater touches nothing
                    auto r = mkRig(L"ui_never");
                    (void)r->m->Snapshot(); r->m->Shutdown();
                    Check(r->factories == 0 && r->w.cmds.empty() && r->w.launches == 0 && r->w.terminated.empty() && r->w.quitPipes.empty(), "upd1 [UI] close: closing the app without using the updater creates, quits and ends nothing");
                }
                {   // UniGetUI missing / session cannot start
                    auto r = mkRig(L"ui_missing", false);
                    bool res_4508 = true; std::thread t([&] { res_4508 = r->m->Check(); }); t.join(); Check(!res_4508, "upd1 [UI] missing: the check fails");
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(!s.checked && s.status == L"UniGetUI was not found." && r->w.cmds.empty() && r->w.launches == 0, "upd1 [UI] missing: the reason is shown and nothing is run");
                    Check(r->logs.size() == 1 && !r->logs[0].first, "upd1 [UI] missing: logged as a failure");
                    UpdListedPackage lp; lp.id = "7zip.7zip"; lp.name = "7-Zip"; lp.manager = "winget"; lp.version = "1.0.0"; lp.newVersion = "1.1.0";
                    bool res_4513 = true; std::thread t2([&] { res_4513 = r->m->Run(lp); }); t2.join(); Check(!res_4513, "upd1 [UI] missing: the update attempt fails");
                    Check(r->m->Snapshot().lastReport == L"UniGetUI was not found." && r->w.updates == 0, "upd1 [UI] missing: an update attempt reports the same reason and runs nothing");
                }
                {
                    auto r = mkRig(L"ui_nolaunch"); r->w.mode = 1;   // the process cannot be launched
                    bool res_4518 = true; std::thread t([&] { res_4518 = r->m->Check(); }); t.join(); Check(!res_4518, "upd1 [UI] no-start: the check fails");
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(!s.checked && !s.status.empty() && s.list.empty() && r->w.updates == 0, "upd1 [UI] no-start: a session that cannot start shows its message and offers no updates");
                }
                {   // a listing UniGetUI answers badly is shown as such, never as 'no updates'
                    auto r = mkRig(L"ui_badlist"); r->w.updatesJson = "{\"status\": \"success\", \"updates\": [";
                    bool res_4524 = true; std::thread t([&] { res_4524 = r->m->Check(); }); t.join(); Check(!res_4524, "upd1 [UI] list: the check fails");
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(!s.checked && s.list.empty() && s.status.find(L"update(s) available") == std::wstring::npos, "upd1 [UI] list: a truncated listing is an error, not '0 updates available'");
                }
                Check(UpdUiSourceName("WinGet: winget") == "winget" && UpdUiSourceName("winget") == "winget" && UpdUiSourceName("") == "", "upd1 [UI] source: UniGetUI's \"<Manager>: <name>\" is reduced to the name that --source takes");
                Check(UpdUiSourceName("Weird: a b") == "" && UpdUiSourceName("x: -rf") == "" && UpdUiSourceName("a\"b") == "", "upd1 [UI] source: a source that is not a plain safe token is dropped, never passed on");
                {   // the confirmation names the package and both versions
                    UpdListedPackage lp; lp.id = "7zip.7zip"; lp.name = "7-Zip"; lp.version = "1.0.0"; lp.newVersion = "1.1.0";
                    const std::wstring t = UpdUiConfirmText(lp);
                    Check(t.find(L"7-Zip") != std::wstring::npos && t.find(L"from 1.0.0 to 1.1.0") != std::wstring::npos, "upd1 [UI] confirm: the question names the package, the installed version and the target version");
                    Check(t.find(L"recovery point") != std::wstring::npos && t.find(L"not backed up") != std::wstring::npos, "upd1 [UI] confirm: it says a recovery point is made and that application data is not backed up");
                    UpdListedPackage noName = lp; noName.name.clear();
                    Check(UpdUiConfirmText(noName).find(L"Update 7zip.7zip from") == 0, "upd1 [UI] confirm: without a name the id is shown");
                }
                // ---- one update per outcome, displayed in words
                struct Case { const char* name; std::function<void(World&)> setup; UpdOutcome want; bool ok; const wchar_t* text; };
                const Case cases[] = {
                    {"succeeded", [](World&) {}, UpdOutcome::Succeeded, true, L"Succeeded"},
                    {"failed (version unchanged)", [](World& w) { w.afterUpdate = "1.0.0"; }, UpdOutcome::Failed, false, L"Failed"},
                    {"partial (version not confirmable)", [](World& w) { w.afterUpdate = "Unknown"; }, UpdOutcome::PartiallySucceeded, false, L"Partially succeeded"},
                    {"timed out", [](World& w) { w.updateRc = kRunTimedOut; }, UpdOutcome::TimedOut, false, L"Timed out"},
                };
                for (const Case& cs : cases) {
                    auto r = mkRig(L"ui_case"); r->w.updatesJson = twoUpdates; cs.setup(r->w);
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    const UpdUiSnapshot before = r->m->Snapshot();
                    UpdReport rep; bool ok = false;
                    UpdListedPackage lp = before.list.empty() ? UpdListedPackage{} : before.list[0];
                    std::thread t([&] { ok = r->m->Run(lp, &rep); }); t.join();
                    const UpdUiSnapshot s = r->m->Snapshot();
                    const std::string nm = cs.name;
                    Check(rep.outcome == cs.want, "upd1 [UI] outcome " + nm + ": the engine's outcome is the expected one");
                    Check(ok == cs.ok, "upd1 [UI] outcome " + nm + ": only a confirmed success returns true");
                    Check(s.lastReport.find(cs.text) == 0, "upd1 [UI] outcome " + nm + ": the page text starts with the outcome in words");
                    Check(!s.busy && !s.checked, "upd1 [UI] outcome " + nm + ": not busy afterwards and the list must be checked again");
                    Check(r->logs.size() == 2 && r->logs[1].first == cs.ok, "upd1 [UI] outcome " + nm + ": logged as success or failure to match");
                    Check(r->w.updates == 1 && r->w.defaultTouched == 0 && privateOnly(r->w, UgPrivatePipeName(r->rt.data, L"tester")), "upd1 [UI] outcome " + nm + ": one update command, all on the private pipe");
                    if (cs.want == UpdOutcome::TimedOut) Check(s.lastReport.find(L"unknown") != std::wstring::npos || s.lastReport.find(L"Timed out") == 0, "upd1 [UI] outcome timed out: the text says the final state is unknown");
                    r->m->Shutdown();
                }
                {   // a version that went DOWN is reported, never silently 'rolled back'
                    auto r = mkRig(L"ui_lower"); r->w.updatesJson = twoUpdates; r->w.afterUpdate = "0.9.0";
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdReport rep; UpdListedPackage lp = r->m->Snapshot().list[0];
                    bool res_4566 = true; std::thread t([&] { res_4566 = r->m->Run(lp, &rep); }); t.join(); Check(!res_4566, "upd1 [UI] outcome lower: the update is not reported as success");
                    Check(rep.outcome != UpdOutcome::Succeeded && r->w.installs == 0, "upd1 [UI] outcome lower: a lower version is not success and no rollback is started without being asked");
                    r->m->Shutdown();
                }
                // ---- the page text for outcomes that only the engine's lock layer produces
                for (const UpdOutcome o : {UpdOutcome::Busy, UpdOutcome::InterruptedPreviousUpdate, UpdOutcome::AbortedBeforeUpdate, UpdOutcome::InvalidRequest, UpdOutcome::TimedOut}) {
                    UpdReport rep; rep.outcome = o; rep.headline = L"why";
                    const std::wstring t = UpdUiReportText(rep);
                    Check(t.find(UpdOutcomeLabel(o)) == 0 && t.find(L"why") != std::wstring::npos && !UpdUiIsSuccess(rep), std::string("upd1 [UI] text: outcome ") + WToUtf8(UpdOutcomeLabel(o)) + " is shown in words and is never a success");
                }
                {
                    UpdReport rep; rep.outcome = UpdOutcome::Succeeded; rep.headline = L"ok"; rep.versionBefore = "1.0.0"; rep.versionAfter = "1.1.0"; rep.versionAfterConfirmed = true; rep.recoveryDir = L"C:\\R\\1";
                    const std::wstring t = UpdUiReportText(rep);
                    Check(UpdUiIsSuccess(rep) && t.find(L"(1.0.0 -> 1.1.0)") != std::wstring::npos && t.find(L"Recovery point: C:\\R\\1") != std::wstring::npos, "upd1 [UI] text: success shows both versions and the recovery point");
                    rep.versionAfterConfirmed = false;
                    Check(UpdUiReportText(rep).find(L"not confirmed") != std::wstring::npos, "upd1 [UI] text: an unconfirmed version is shown as 'not confirmed', not as the target");
                    rep.rollbackResult = UpdRollbackResult::Partial;
                    Check(UpdUiReportText(rep).find(L"Rollback: Rolled back partially") != std::wstring::npos, "upd1 [UI] text: a partial rollback is stated as partial");
                }
                // ---- one operation at a time: a second call while one runs is refused, not queued
                {
                    auto r = mkRig(L"ui_busy"); r->w.updatesJson = twoUpdates;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    r->w.hold = true;
                    bool ok1 = false; std::thread tu([&] { ok1 = r->m->Run(lp); });
                    for (int i = 0; i < 4000 && r->w.holding.load() == 0; ++i) Sleep(1);   // the update command is now blocked inside the (mock) CLI
                    Check(r->m->Snapshot().busy, "upd1 [UI] busy: the model reports busy while the update runs");
                    const size_t cmdsBefore = [&] { std::lock_guard<std::mutex> g(r->w.m); return r->w.cmds.size(); }();
                    const bool c2 = r->m->Check(); const bool r2 = r->m->Run(lp);
                    Check(!c2 && !r2 && r->m->RefusedBusy() == 2, "upd1 [UI] busy: a check and a second update are refused while one runs");
                    { std::lock_guard<std::mutex> g(r->w.m); Check(r->w.cmds.size() == cmdsBefore, "upd1 [UI] busy: ... and the refused calls sent no command (no second worker's commands)"); }
                    r->w.hold = false; tu.join();
                    Check(ok1 && !r->m->Snapshot().busy && r->w.updates == 1, "upd1 [UI] busy: the running update finished normally, exactly one update command in total");
                    r->m->Shutdown();
                    Check(!r->w.procs[5000].second && r->w.userKilled == 0 && r->w.defaultTouched == 0, "upd1 [UI] busy: Shutdown afterwards ended only the AnkhCore session");
                }
                {   // closing while an update is still running: the session is not pulled out from under it
                    auto r = mkRig(L"ui_closing"); r->w.updatesJson = twoUpdates;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    r->w.hold = true;
                    std::thread tu([&] { r->m->Run(lp); });
                    for (int i = 0; i < 4000 && r->w.holding.load() == 0; ++i) Sleep(1);
                    r->m->Shutdown();            // while the update command is still in flight
                    r->w.hold = false; tu.join();
                    Check(r->w.userKilled == 0 && r->w.defaultTouched == 0 && !r->m->Snapshot().busy, "upd1 [UI] closing: Shutdown during a running update leaves the user's session alone and the worker finishes");
                    bool res_4613 = true; std::thread t4([&] { res_4613 = r->m->Run(lp); }); t4.join(); Check(!res_4613, "upd1 [UI] closing: an update after Shutdown is refused");
                    Check(r->w.updates <= 1 && r->factories == 1, "upd1 [UI] closing: no further update or session after Shutdown");
                }
                // ---- [UI-C] UPD2-C: the fuller result view and releasing an unfinished update
                {   // the detail lines of a finished update
                    auto r = mkRig(L"uic_lines"); r->w.updatesJson = twoUpdates;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    Check(r->m->Snapshot().lines.empty() && !r->m->Snapshot().pending.present, "upd1 [UI-C] lines: before any update there is no detail and no unfinished update");
                    UpdListedPackage lp = r->m->Snapshot().list[0]; UpdReport rep;
                    std::thread t([&] { r->m->Run(lp, &rep); }); t.join();
                    const UpdUiSnapshot s = r->m->Snapshot();
                    auto has = [&](const wchar_t* title) { for (const auto& l : s.lines) if (l.title == title) return true; return false; };
                    Check(!s.lines.empty() && s.lines[0].title == L"Result" && s.lines[0].state == 1, "upd1 [UI-C] lines: the first line is the result, marked ok for a success");
                    Check(has(L"Installed version") && has(L"Recovery point") && has(L"Application data") && has(L"Rollback support"), "upd1 [UI-C] lines: versions, recovery point, application data and rollback support are all shown");
                    bool dataLineHonest = false; for (const auto& l : s.lines) if (l.title == L"Application data" && l.detail.find(L"not backed up") != std::wstring::npos && l.state == 0) dataLineHonest = true;
                    Check(dataLineHonest, "upd1 [UI-C] lines: application data is reported as not backed up (no data map exists yet)");
                    bool rbLine = false; for (const auto& l : s.lines) if (l.title == L"Rollback support" && l.detail.find(UpdRollbackCapabilityLabel(rep.rollback.capability)) == 0) rbLine = true;
                    Check(rbLine && rep.rollback.capability != UpdRollbackCapability::Full, "upd1 [UI-C] lines: the rollback class is shown and is never 'Fully supported' without a data backup");
                    size_t stepLines = 0; for (const auto& l : s.lines) for (const auto& st : rep.steps) if (l.title == st.step && l.detail == st.detail && l.state == (st.ok ? 1 : 2)) ++stepLines;
                    Check(!rep.steps.empty() && stepLines >= rep.steps.size(), "upd1 [UI-C] lines: every step of the run is listed with its detail and its ok/problem state");
                    Check(s.pending.present == false, "upd1 [UI-C] lines: a finished update leaves nothing pending");
                    r->m->Shutdown();
                }
                {   // a failed update: the problem lines are marked
                    auto r = mkRig(L"uic_failed"); r->w.updatesJson = twoUpdates; r->w.afterUpdate = "1.0.0";
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    std::thread t([&] { r->m->Run(lp); }); t.join();
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(!s.lines.empty() && s.lines[0].state == 2, "upd1 [UI-C] lines: a failed update's result line is marked as a problem");
                    Check(s.lines.size() > 1 && std::any_of(s.lines.begin(), s.lines.end(), [](const UpdUiLine& l) { return l.title == L"Installed version" && l.state != 1; }), "upd1 [UI-C] lines: the unchanged version is not shown as ok");
                    r->m->Shutdown();
                }
                {   // a timed-out update leaves an unresolved record that blocks the next update, and the page can see it
                    auto r = mkRig(L"uic_timeout"); r->w.updatesJson = twoUpdates; r->w.updateRc = kRunTimedOut;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    std::thread t([&] { r->m->Run(lp); }); t.join();
                    UpdUiSnapshot s = r->m->Snapshot();
                    Check(s.pending.present && s.pending.unresolved && s.pending.readable, "upd1 [UI-C] pending: a timed-out update is reported as unresolved");
                    Check(s.pending.text.find(L"7zip.7zip") != std::wstring::npos && s.pending.text.find(L"final state is unknown") != std::wstring::npos && s.pending.text.find(L"blocked") != std::wstring::npos, "upd1 [UI-C] pending: the text names the package, says the state is unknown and that further updates are blocked");
                    r->w.updateRc = 0;
                    UpdReport rep2; std::thread t2([&] { r->m->Run(lp, &rep2); }); t2.join();
                    Check(rep2.outcome == UpdOutcome::InterruptedPreviousUpdate && r->w.updates == 1, "upd1 [UI-C] pending: a second update is refused while it is unresolved (no second update command)");
                    // the verifier sees the package still listed as a running operation: not released
                    r->w.opList = "{\"status\": \"success\", \"operations\": [{\"package\": \"7zip.7zip\", \"status\": \"running\"}]}";
                    bool a1 = true; std::thread t3([&] { a1 = r->m->Acknowledge(false); }); t3.join();
                    s = r->m->Snapshot();
                    Check(!a1 && s.pending.present && s.lastReport.find(L"still lists an operation") != std::wstring::npos, "upd1 [UI-C] release: while UniGetUI still lists the operation the record stays and the reason is shown");
                    Check(FileExists(UpdActivePath(r->rt)), "upd1 [UI-C] release: ... the lock file is still there");
                    // the verifier cannot answer: stays blocked
                    r->w.opList.clear(); r->w.opListRc = 2;
                    bool a2 = true; std::thread t4([&] { a2 = r->m->Acknowledge(false); }); t4.join();
                    s = r->m->Snapshot();
                    Check(!a2 && s.pending.present && s.lastReport.find(L"could not check") != std::wstring::npos, "upd1 [UI-C] release: when UniGetUI cannot answer the record stays and says it could not check");
                    Check(s.lastReport.find(L"Release without checking") != std::wstring::npos, "upd1 [UI-C] release: ... and points at the explicit way out");
                    // the verifier shows the package is not running: released into history, recovery point untouched
                    r->w.opListRc = 0;
                    const std::wstring recDirBefore = s.pending.text;
                    bool a3 = false; std::thread t5([&] { a3 = r->m->Acknowledge(false); }); t5.join();
                    s = r->m->Snapshot();
                    Check(a3 && !s.pending.present && !FileExists(UpdActivePath(r->rt)), "upd1 [UI-C] release: once UniGetUI shows no operation for the package the record is released");
                    Check(s.lastReport.find(L"Released.") == 0, "upd1 [UI-C] release: the message says it was released");
                    bool hist = false; WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((UpdHistoryDir(r->rt) + L"\\*timeout-resolved*").c_str(), &fd);
                    if (fh != INVALID_HANDLE_VALUE) { hist = true; FindClose(fh); }
                    Check(hist, "upd1 [UI-C] release: the record was moved into the update history, not deleted");
                    Check(r->w.userKilled == 0 && r->w.defaultTouched == 0 && privateOnly(r->w, UgPrivatePipeName(r->rt.data, L"tester")), "upd1 [UI-C] release: the check used the private pipe only; the user's session was not touched");
                    // and a new update may start again
                    r->w.cur = "1.0.0";   // the machine state of the earlier (timed-out) update is reset for the new one
                    UpdReport rep3; std::thread t6([&] { r->m->Run(lp, &rep3); }); t6.join();
                    Check(rep3.outcome == UpdOutcome::Succeeded, "upd1 [UI-C] release: afterwards a new update runs normally");
                    r->m->Shutdown();
                }
                {   // an explicit force release works without UniGetUI, is named as such and starts no session
                    auto r = mkRig(L"uic_force"); r->w.updatesJson = twoUpdates; r->w.updateRc = kRunTimedOut;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    std::thread t([&] { r->m->Run(lp); }); t.join();
                    r->m->Shutdown();   // the session is gone; a forced release must not need it
                    const size_t before = r->w.cmds.size();
                    r->m->RefreshPending();
                    Check(r->m->Snapshot().pending.unresolved, "upd1 [UI-C] force: the unresolved record is visible after the session ended");
                    bool af = false;
                    // after Shutdown the model refuses new work, so use a fresh model over the same data
                    auto r2 = mkRig(L"uic_force2");
                    r2->rt = r->rt; r2->m = std::make_unique<UpdUiModel>([rp = r2.get()](std::wstring& why) -> std::shared_ptr<UgSession> { ++rp->factories; why = L"no"; return nullptr; }, [] {}, [](bool, const std::wstring&) {}, r->rt, [](UpdContext& c) { c.journal = false; });
                    std::thread t2([&] { af = r2->m->Acknowledge(true); }); t2.join();
                    const UpdUiSnapshot s = r2->m->Snapshot();
                    Check(af && !s.pending.present && s.lastReport.find(L"Released without checking") == 0, "upd1 [UI-C] force: an explicit release works and is worded as 'without checking'");
                    Check(r2->factories == 0 && r->w.cmds.size() == before, "upd1 [UI-C] force: it neither created a session nor sent any command");
                    bool hist = false; WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((UpdHistoryDir(r->rt) + L"\\*unresolved-forced*").c_str(), &fd);
                    if (fh != INVALID_HANDLE_VALUE) { hist = true; FindClose(fh); }
                    Check(hist, "upd1 [UI-C] force: the record is kept in the history as 'unresolved-forced'");
                }
                {   // a crashed update (the owner process is gone) is released directly; a live owner is never released
                    auto r = mkRig(L"uic_crash"); r->w.updatesJson = twoUpdates;
                    r->m = std::make_unique<UpdUiModel>(
                        [rp = r.get(), &platformFor, &cfgFor](std::wstring&) -> std::shared_ptr<UgSession> { ++rp->factories; return std::make_shared<UgSession>(cfgFor(rp->rt), platformFor(rp->w)); },
                        [] {}, [](bool, const std::wstring&) {}, r->rt,
                        [](UpdContext& c) { c.journal = false; c.pid = 7002; c.isProcessAlive = [](DWORD) { return false; }; c.simulateCrashAt = [](UpdPhase p) { return p == UpdPhase::Updating; }; });
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0]; UpdReport rep;
                    std::thread t([&] { r->m->Run(lp, &rep); }); t.join();
                    Check(rep.interrupted && r->w.updates == 0, "upd1 [UI-C] crash: the simulated crash stopped the update before the update command");
                    UpdUiSnapshot s = r->m->Snapshot();
                    Check(s.pending.present && !s.pending.unresolved && s.pending.text.find(L"did not finish") != std::wstring::npos && s.pending.text.find(L"stopped at: updating") != std::wstring::npos, "upd1 [UI-C] crash: the page sees an interrupted update and the phase it stopped in");
                    r->m->SetOwnerAliveCheck([](DWORD) { return true; });
                    bool a1 = true; std::thread t1([&] { a1 = r->m->Acknowledge(false); }); t1.join();
                    s = r->m->Snapshot();
                    Check(!a1 && s.pending.present && s.lastReport.find(L"still running") != std::wstring::npos, "upd1 [UI-C] crash: while the owner process is alive nothing is released");
                    r->m->SetOwnerAliveCheck([](DWORD) { return false; });
                    bool a2 = false; std::thread t2([&] { a2 = r->m->Acknowledge(false); }); t2.join();
                    s = r->m->Snapshot();
                    Check(a2 && !s.pending.present && s.lastReport.find(L"Released.") == 0, "upd1 [UI-C] crash: once the owner is gone the record is released");
                    Check(r->w.updates == 0 && r->w.installs == 0, "upd1 [UI-C] crash: releasing runs no update and no reinstall");
                    r->m->Shutdown();
                }
                {   // nothing to release, and a damaged record is kept in the history
                    auto r = mkRig(L"uic_none");
                    bool a = true; std::thread t([&] { a = r->m->Acknowledge(false); }); t.join();
                    Check(!a && r->m->Snapshot().lastReport == L"There is no unfinished update." && r->factories == 0 && r->w.cmds.empty(), "upd1 [UI-C] release: with nothing pending it says so and starts nothing");
                    EnsureDir(UpdDir(r->rt));
                    { const std::string junk = "{not json"; WriteAtomic(UpdActivePath(r->rt), reinterpret_cast<const uint8_t*>(junk.data()), junk.size()); }
                    r->m->RefreshPending();
                    const UpdUiSnapshot s = r->m->Snapshot();
                    Check(s.pending.present && !s.pending.readable && s.pending.text.find(L"cannot be read") != std::wstring::npos, "upd1 [UI-C] damaged: an unreadable record is shown as such");
                    bool a2 = false; std::thread t2([&] { a2 = r->m->Acknowledge(false); }); t2.join();
                    bool hist = false; WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((UpdHistoryDir(r->rt) + L"\\unreadable-*").c_str(), &fd);
                    if (fh != INVALID_HANDLE_VALUE) { hist = true; FindClose(fh); }
                    Check(a2 && hist && !FileExists(UpdActivePath(r->rt)), "upd1 [UI-C] damaged: releasing it keeps a copy in the history as 'unreadable-...'");
                }
                {   // a release cannot run at the same time as another operation
                    auto r = mkRig(L"uic_busy"); r->w.updatesJson = twoUpdates;
                    std::thread t0([&] { r->m->Check(); }); t0.join();
                    UpdListedPackage lp = r->m->Snapshot().list[0];
                    r->w.hold = true;
                    std::thread tu([&] { r->m->Run(lp); });
                    for (int i = 0; i < 4000 && r->w.holding.load() == 0; ++i) Sleep(1);
                    Check(!r->m->Acknowledge(false) && !r->m->Acknowledge(true) && r->m->RefusedBusy() == 2, "upd1 [UI-C] busy: a release is refused while an update runs");
                    Check(FileExists(UpdActivePath(r->rt)) || r->w.holding.load() == 1, "upd1 [UI-C] busy: the running update's own lock was not released by it");
                    r->w.hold = false; tu.join();
                    Check(!r->m->Snapshot().pending.present, "upd1 [UI-C] busy: the update finished and left nothing pending");
                    r->m->Shutdown();
                }
                Check(UpdLiveExecutionAllowed(false) && !UpdLiveExecutionAllowed(true), "upd1 [UI] test build: the live platform (which the page's factory uses) refuses every launch, command and process end in the isolated test build");
            }
            // ---- overall guarantee across every scenario above is asserted per scenario (defaultTouched == 0 / userKilled == 0).
            Check(UpdLiveExecutionAllowed(false) && !UpdLiveExecutionAllowed(true), "upd1 [UG] live: the live platform acts only outside the isolated test build");
        }

        {   // user data and settings were never modified
            const AcbRoots rt = freshRoots(L"run_keep", true);
            Mock m; m.afterUpdate = "0.9.0"; UpdContext c = mkCtx(m, rt); c.autoRollback = true;
            UpdRun(c, pk);
            std::vector<uint8_t> now;
            ReadAll(rt.appdata + L"\\settings.json", now);
            Check(std::string(now.begin(), now.end()) == settingsBody, "upd1 data: settings.json is byte-identical after a failed update with rollback");
        }
        Evict(updRoot, true, false);
    }
    {   // ---- M-2.6-HW1: normalized hardware inventory, sensor source/availability, telemetry vs control classification (pure; polls nothing)
        auto mkIn = [] {
            HwInputs in;
            in.os = L"Windows 11 Pro 10.0.26300"; in.sysManufacturer = L"Acme Inc."; in.sysProduct = L"Laptop X1"; in.boardManufacturer = L"Acme Inc."; in.boardProduct = L"X1-BOARD";
            in.biosVersion = L"1.2.3"; in.biosDate = L"01/02/2025"; in.cpuName = L"Test CPU @ 3.0GHz"; in.physCores = 6; in.logicalCores = 12;
            in.cpuLoadWindows = 12.0; in.ramLoadWindows = 40.0; in.ramTotal = 16ull << 30; in.networkAdapters = 2;
            in.helperInstalled = true; in.readings.present = true;
            in.readings.cpuTemp = 55; in.readings.cpuClock = 3000; in.readings.cpuPower = 18; in.readings.diskTemp = 33;
            return in;
        };
        auto mkFan = [](const wchar_t* name, double rpm, FanCapability cap, bool controllable, const wchar_t* source = L"LibreHardwareMonitor", bool live = true) {
            FanReading f; f.name = name; f.rpm = rpm; f.capability = cap; f.controllable = controllable; f.source = source; f.live = live; return f;
        };

        // identity
        {
            HwInputs a = mkIn();
            const HwInventory ia = BuildHwInventory(a);
            Check(ia.device.complete && ia.device.identityHash.size() == 64, "hw1 identity: a known model gives a 64-hex SHA-256 identity");
            Check(ia.device.identityHash.find_first_not_of("0123456789abcdef") == std::string::npos, "hw1 identity: it is lower-case hex");
            HwInputs b = mkIn(); b.sysManufacturer = L"  ACME   inc. "; b.sysProduct = L"laptop  x1";
            Check(BuildHwInventory(b).device.identityHash == ia.device.identityHash, "hw1 identity: case and spacing do not change it");
            HwInputs c = mkIn(); c.sysProduct = L"Laptop X2";
            Check(BuildHwInventory(c).device.identityHash != ia.device.identityHash, "hw1 identity: another model is another identity");
            HwInputs d = mkIn(); d.biosVersion = L"9.9"; d.cpuName = L"Other CPU"; d.os = L"Windows 10";
            Check(BuildHwInventory(d).device.identityHash == ia.device.identityHash, "hw1 identity: BIOS version, CPU and OS (they change) are not part of it");
            HwInputs e = mkIn(); e.sysProduct.clear(); e.boardProduct.clear(); e.helperModel.clear();
            const HwInventory ie = BuildHwInventory(e);
            Check(!ie.device.complete && ie.device.identityHash.empty(), "hw1 identity: without a model or board there is no identity (not a hash of nothing)");
            Check(std::any_of(ie.caps.limitations.begin(), ie.caps.limitations.end(), [](const std::wstring& l) { return l.find(L"no stable identity") != std::wstring::npos; }), "hw1 identity: the missing identity is listed as a limitation");
            HwInputs f = mkIn(); f.sysProduct.clear(); f.helperModel = L"Laptop X1";
            Check(BuildHwInventory(f).device.model == L"Laptop X1", "hw1 identity: the helper's model name is the fallback when Windows gives none");
        }
        // availability and source of every metric
        {
            HwInputs in = mkIn();
            HwInventory v = BuildHwInventory(in);
            Check(v.cpuTemp.avail == HwAvail::Available && v.cpuTemp.value == 55 && v.cpuTemp.source == HwSource::SensorHelper, "hw1 metric: a fresh helper value is Available and attributed to the sensor helper");
            Check(v.cpuLoad.avail == HwAvail::Available && v.cpuLoad.source == HwSource::Windows && v.cpuLoad.value == 12.0, "hw1 metric: CPU load comes from Windows when Windows has it");
            Check(v.ramLoad.source == HwSource::Windows && v.sensorHelper == HwAvail::Available, "hw1 metric: memory load is Windows-sourced; the helper is Available");
            in.readings.cpuTemp = 0;
            Check(BuildHwInventory(in).cpuTemp.avail == HwAvail::Available, "hw1 metric: a real zero reading is Available, not missing");
            in.readings.cpuTemp = -1;
            Check(BuildHwInventory(in).cpuTemp.avail == HwAvail::NotReported, "hw1 metric: a working helper with no value says 'not reported by this hardware'");
            in.readings.cpuTemp = std::numeric_limits<double>::quiet_NaN();
            Check(BuildHwInventory(in).cpuTemp.avail == HwAvail::NotReported, "hw1 metric: NaN is not a value");
            in = mkIn(); in.readings.present = false;
            v = BuildHwInventory(in);
            Check(v.cpuTemp.avail == HwAvail::Stale && v.cpuClock.avail == HwAvail::Stale && v.diskTemp.avail == HwAvail::Stale && v.sensorHelper == HwAvail::Stale, "hw1 metric: an installed but silent helper makes every helper value Stale (unknown, not zero)");
            Check(v.cpuLoad.avail == HwAvail::Available && v.cpuLoad.source == HwSource::Windows, "hw1 metric: Windows values stay Available when the helper is silent");
            in.helperInstalled = false;
            v = BuildHwInventory(in);
            Check(v.cpuTemp.avail == HwAvail::HelperMissing && v.sensorHelper == HwAvail::HelperMissing, "hw1 metric: no helper installed is HelperMissing");
            in = mkIn(); in.cpuLoadWindows = -1;
            v = BuildHwInventory(in); in.readings.cpuLoad = 7;
            Check(v.cpuLoad.avail != HwAvail::Available, "hw1 metric: no Windows load and no helper load is not a number");
            v = BuildHwInventory(in);
            Check(v.cpuLoad.avail == HwAvail::Available && v.cpuLoad.source == HwSource::SensorHelper && v.cpuLoad.value == 7, "hw1 metric: the helper's CPU load is the labelled fallback");
            in = mkIn(); in.cpuLoadWindows = -1; in.ramLoadWindows = -1;
            v = BuildHwInventory(in);
            Check(v.ramLoad.avail == HwAvail::NoData && v.ramLoad.value < 0, "hw1 metric: a Windows value that has not arrived is 'no data yet', never 0");
            Check(v.battery.avail == HwAvail::NoData && v.battery.source == HwSource::None, "hw1 metric: battery has no source and says so");
        }
        // GPUs
        {
            HwInputs in = mkIn();
            HwInputs::Gpu hw; hw.name = L"Real GPU"; hw.vram = 4ull << 30; hw.util = 3.5;
            HwInputs::Gpu sw; sw.name = L"Microsoft Basic Render Driver"; sw.software = true;
            in.gpus = {hw, sw};
            const HwInventory v = BuildHwInventory(in);
            Check(v.gpus.size() == 1 && v.gpus[0].name == L"Real GPU" && v.gpus[0].vramBytes == (4ull << 30), "hw1 gpu: the software adapter is not hardware and is left out");
            Check(v.gpus[0].load.avail == HwAvail::Available && v.gpus[0].load.source == HwSource::Windows, "hw1 gpu: load is Windows-sourced");
        }
        // fan telemetry vs control, one case each
        {
            const HwInputs in = mkIn();
            HwFan f = HwClassifyFan(mkFan(L"CPU Fan", 2400, FanCapability::Controllable, true), in);
            Check(f.telemetry == HwTelemetry::Live && f.control == HwControl::ReportedUnverified, "hw1 fan: a controllable fan with RPM is telemetry Live and control only REPORTED (unverified)");
            Check(f.note.find(L"does not offer fan control") != std::wstring::npos, "hw1 fan: ... and the note says AnkhCore does not offer control");
            f = HwClassifyFan(mkFan(L"Fan", 1800, FanCapability::MonitorOnly, false), in);
            Check(f.telemetry == HwTelemetry::Live && f.control == HwControl::None, "hw1 fan: a readable RPM alone is read-only (monitoring does not imply control)");
            f = HwClassifyFan(mkFan(L"Fan", 1800, FanCapability::VendorReadOnly, false, L"HP BIOS"), in);
            Check(f.control == HwControl::None && f.source == HwSource::VendorBios, "hw1 fan: a vendor read-only interface is read-only and attributed to the vendor BIOS");
            f = HwClassifyFan(mkFan(L"Fan", 1800, FanCapability::VendorReadOnly, false, L"HP BIOS", false), in);
            Check(f.telemetry == HwTelemetry::Frozen, "hw1 fan: a vendor value never seen to change is Frozen, not Live");
            f = HwClassifyFan(mkFan(L"Fan", -1, FanCapability::ControlOnly, true), in);
            Check(f.telemetry == HwTelemetry::None && f.control == HwControl::ReportedUnverified && f.note.find(L"no RPM") != std::wstring::npos, "hw1 fan: a control without RPM has no telemetry and the note says a change could not be confirmed");
            f = HwClassifyFan(mkFan(L"Fan", 1000, FanCapability::Unknown, true), in);
            Check(f.control == HwControl::ReportedUnverified, "hw1 fan: an older helper's controllable flag is also only 'reported'");
            f = HwClassifyFan(mkFan(L"Fan", 1000, FanCapability::Unknown, false), in);
            Check(f.control == HwControl::Unknown && f.note.find(L"treated as not controllable") != std::wstring::npos, "hw1 fan: an unclassified fan is Unknown and treated as not controllable");
            f = HwClassifyFan(mkFan(L"Fan", std::numeric_limits<double>::quiet_NaN(), FanCapability::MonitorOnly, false), in);
            Check(f.telemetry == HwTelemetry::None, "hw1 fan: a NaN RPM is no telemetry");
            f = HwClassifyFan(mkFan(L"", 100, FanCapability::MonitorOnly, false), in);
            Check(f.name == L"Fan", "hw1 fan: an empty name gets a placeholder");
            HwInputs vin = mkIn(); vin.deviceProfileVerified = true;
            f = HwClassifyFan(mkFan(L"CPU Fan", 2400, FanCapability::Controllable, true), vin);
            Check(f.control == HwControl::Verified, "hw1 fan: only a verified device profile turns a reported control into Verified");
            f = HwClassifyFan(mkFan(L"Fan", 2400, FanCapability::MonitorOnly, false), vin);
            Check(f.control == HwControl::None, "hw1 fan: a verified profile never makes a monitor-only fan controllable");
        }
        // capability summary: control is never offered without verification and readable RPM
        {
            HwInputs in = mkIn();
            in.readings.fans = {mkFan(L"A", 2000, FanCapability::Controllable, true), mkFan(L"B", 1500, FanCapability::MonitorOnly, false), mkFan(L"C", -1, FanCapability::Unknown, false)};
            HwInventory v = BuildHwInventory(in);
            Check(v.fans.size() == 3 && v.caps.fansWithTelemetry == 2 && v.caps.fansControlReported == 1 && v.caps.fansReadOnly == 1 && v.caps.fansUnknown == 1 && v.caps.fansControlVerified == 0, "hw1 caps: the counts split telemetry, reported control, read-only and unknown");
            Check(!v.caps.fanControlExposable, "hw1 caps: a reported (unverified) control does NOT make fan control offerable");
            Check(std::any_of(v.caps.limitations.begin(), v.caps.limitations.end(), [](const std::wstring& l) { return l.find(L"BIOS / firmware control") != std::wstring::npos; }), "hw1 caps: the limitation says fans stay under BIOS / firmware control");
            Check(std::any_of(v.caps.limitations.begin(), v.caps.limitations.end(), [](const std::wstring& l) { return l.find(L"not proof") != std::wstring::npos; }), "hw1 caps: a reported control is explicitly 'not proof'");
            in.deviceProfileVerified = true;
            v = BuildHwInventory(in);
            Check(v.caps.fansControlVerified == 1 && v.caps.fanControlExposable, "hw1 caps: a verified control on a fan with readable RPM is the only way control becomes offerable");
            in.readings.fans = {mkFan(L"A", -1, FanCapability::ControlOnly, true)};
            v = BuildHwInventory(in);
            Check(v.caps.fansControlVerified == 1 && !v.caps.fanControlExposable, "hw1 caps: a verified control WITHOUT readable RPM is still not offerable (no way to confirm a command)");
            in = mkIn();
            v = BuildHwInventory(in);
            Check(v.fans.empty() && !v.caps.fanControlExposable && std::any_of(v.caps.limitations.begin(), v.caps.limitations.end(), [](const std::wstring& l) { return l.find(L"No fan sensor") != std::wstring::npos; }), "hw1 caps: no fan at all is reported as such");
            in.helperInstalled = false;
            v = BuildHwInventory(in);
            Check(std::any_of(v.caps.limitations.begin(), v.caps.limitations.end(), [](const std::wstring& l) { return l.find(L"not installed") != std::wstring::npos; }), "hw1 caps: a missing helper is a limitation");
        }
        // the real readings parser feeds the classifier (the file format the helper writes)
        {
            const std::string text =
                "TIME=2026-01-01 00:00:00\nCPU.TEMP=61\nMODEL=Laptop X1\n"
                "FAN.0.NAME=CPU Fan\nFAN.0.RPM=2500\nFAN.0.CONTROL=1\nFAN.0.CAPABILITY=controllable\nFAN.0.SOURCE=LibreHardwareMonitor\n"
                "FAN.1.NAME=GPU Fan\nFAN.1.RPM=1900\nFAN.1.CAPABILITY=monitor_only\n"
                "FAN.2.NAME=Vendor Fan\nFAN.2.RPM=2100\nFAN.2.CAPABILITY=vendor_readonly\nFAN.2.SOURCE=HP BIOS\nFAN.2.LIVE=0\n";
            HwInputs in; in.helperInstalled = true; in.readings = ParseReadings(text); in.readings.present = true; in.sysProduct = L"Laptop X1";
            const HwInventory v = BuildHwInventory(in);
            Check(v.fans.size() == 3, "hw1 parse: three fans from the readings file");
            Check(v.fans[0].telemetry == HwTelemetry::Live && v.fans[0].control == HwControl::ReportedUnverified, "hw1 parse: fan 0 is readable and reported-controllable");
            Check(v.fans[1].control == HwControl::None && v.fans[1].telemetry == HwTelemetry::Live, "hw1 parse: fan 1 is monitor-only");
            Check(v.fans[2].source == HwSource::VendorBios && v.fans[2].telemetry == HwTelemetry::Frozen && v.fans[2].control == HwControl::None, "hw1 parse: fan 2 is a frozen vendor read-only value");
            Check(!v.caps.fanControlExposable && v.cpuTemp.value == 61, "hw1 parse: control is still not offered; the CPU temperature came through");
        }
        // reports: deterministic, valid JSON, no personal data, escaped
        {
            HwInputs in = mkIn();
            HwInputs::Gpu g; g.name = L"GPU \"X\" \\ 1"; g.vram = 2ull << 30; g.util = 1;
            in.gpus = {g};
            in.readings.fans = {mkFan(L"CPU Fan", 2400, FanCapability::Controllable, true)};
            const HwInventory v = BuildHwInventory(in);
            const std::string j1 = HwInventoryJson(v), j2 = HwInventoryJson(BuildHwInventory(in));
            Check(j1 == j2 && HwInventoryText(v) == HwInventoryText(BuildHwInventory(in)), "hw1 report: the same inputs give byte-identical text and JSON");
            JVal root;
            Check(JsonParse(j1, root) && root.Is(JVal::Obj), "hw1 report: the JSON is valid and strictly parseable (escaped names included)");
            const JVal* schema = root.Get("schema"); const JVal* ver = root.Get("version");
            Check(schema && schema->s == "ankhcore.hardware-inventory" && ver && ver->n == 1, "hw1 report: the JSON carries a schema name and a version");
            const JVal* dev = root.Get("device");
            Check(dev && dev->Get("identity") && dev->Get("identity")->s == v.device.identityHash && dev->Get("identityComplete") && dev->Get("identityComplete")->b, "hw1 report: the device identity is in the JSON");
            const JVal* caps = root.Get("capabilities");
            Check(caps && caps->Get("fanControlOffered") && !caps->Get("fanControlOffered")->b && caps->Get("fansControlReported") && caps->Get("fansControlReported")->n == 1, "hw1 report: the JSON says control is not offered and counts the reported control");
            const JVal* gpus = root.Get("gpus");
            Check(gpus && gpus->a.size() == 1 && gpus->a[0].Get("name") && gpus->a[0].Get("name")->s == "GPU \"X\" \\ 1", "hw1 report: a name with quotes and backslashes round-trips");
            const std::wstring t = HwInventoryText(v);
            Check(t.find(L"Fan control offered: no") != std::wstring::npos && t.find(L"control reported, not verified") != std::wstring::npos, "hw1 report: the text states control is not offered and why");
            Check(t.find(L"== Limitations ==") != std::wstring::npos && t.find(L"[Windows]") != std::wstring::npos && t.find(L"sensor helper") != std::wstring::npos, "hw1 report: sources and limitations are listed");
            Check(j1.find("serial") == std::string::npos && j1.find("uuid") == std::string::npos && j1.find("Serial") == std::string::npos, "hw1 report: no serial number or UUID field exists");
            HwInputs empty;
            const HwInventory ev = BuildHwInventory(empty);
            JVal er;
            Check(JsonParse(HwInventoryJson(ev), er), "hw1 report: an empty inventory still gives valid JSON");
            Check(!ev.caps.fanControlExposable && ev.sensorHelper == HwAvail::HelperMissing, "hw1 report: an empty inventory offers nothing and reports the missing helper");
        }
        // ---- M-2.6-CFG1: device profile (schema, generator, strict validation, matching, storage). Mock data only; no hardware, no network.
        {
            HwInputs in = mkIn();
            FanReading cpuFan = mkFan(L"CPU Fan", 2400, FanCapability::Controllable, true); cpuFan.minPercent = 20; cpuFan.maxPercent = 100;
            FanReading gpuFan = mkFan(L"GPU Fan", 1900, FanCapability::MonitorOnly, false);
            in.readings.fans = {cpuFan, gpuFan};
            const HwInventory inv = BuildHwInventory(in);
            const std::string stamp = "2026-11-19T10:00:00Z", gen = "AnkhCore 2.6 test";
            const DeviceProfile p = DpGenerate(inv, stamp, gen);

            // ---- generator
            Check(p.identity == inv.device.identityHash && DpValidHex64(p.identity), "cfg1 gen: the profile is keyed on the inventory's identity hash");
            Check(p.inventorySha256 == HwSha256Hex(HwInventoryJson(inv)) && DpValidHex64(p.inventorySha256), "cfg1 gen: provenance carries the SHA-256 of the inventory it was generated from");
            Check(p.generatedUtc == stamp && p.generator == gen, "cfg1 gen: generator and time are recorded as given (deterministic, no clock inside)");
            Check(p.fans.size() == 2 && p.fans[0].role == "cpu" && p.fans[1].role == "gpu", "cfg1 gen: a role is set only when the fan's own name says so");
            Check(p.fans[0].control == HwControl::ReportedUnverified && p.fans[1].control == HwControl::None, "cfg1 gen: a reported control stays 'reported-unverified'; a read-only fan stays read-only");
            Check(p.fans[0].telemetry == HwTelemetry::Live && p.fans[0].minPercent == 20 && p.fans[0].maxPercent == 100, "cfg1 gen: telemetry and the helper-reported limits are copied");
            Check(p.compatibility == "control-reported-unverified" && DpCompatibility(p.fans) == p.compatibility, "cfg1 gen: the compatibility class is derived from the fans");
            Check(p.evidence.empty() && !p.safeReleaseVerified, "cfg1 gen: a generated profile has no evidence and no verified safe release");
            Check(DpToJson(p) == DpToJson(DpGenerate(inv, stamp, gen)), "cfg1 gen: the same inventory gives a byte-identical profile");
            {
                HwInputs vin = in; vin.deviceProfileVerified = true;
                const HwInventory vinv = BuildHwInventory(vin);
                Check(vinv.fans[0].control == HwControl::Verified, "cfg1 gen: (precondition) the inventory can carry a Verified control");
                const DeviceProfile vp = DpGenerate(vinv, stamp, gen);
                Check(vp.fans[0].control == HwControl::ReportedUnverified && vp.evidence.empty(), "cfg1 gen: even then the generator never emits a verified control or evidence");
            }
            {
                HwInputs nin = mkIn();
                nin.readings.fans = {mkFan(L"Fan", 1000, FanCapability::MonitorOnly, false), mkFan(L"CPU/GPU Fan", 1000, FanCapability::MonitorOnly, false)};
                const DeviceProfile np = DpGenerate(BuildHwInventory(nin), stamp, gen);
                Check(np.fans[0].role == "unknown" && np.fans[1].role == "unknown", "cfg1 gen: fan 0 is not guessed to be the CPU fan by index, and an ambiguous name stays unknown");
                Check(np.compatibility == "monitor-only", "cfg1 gen: read-only fans give 'monitor-only'");
                FanReading bad = mkFan(L"CPU Fan", 1000, FanCapability::Controllable, true); bad.minPercent = 80; bad.maxPercent = 20;
                nin.readings.fans = {bad};
                Check(DpGenerate(BuildHwInventory(nin), stamp, gen).fans[0].minPercent == -1, "cfg1 gen: inconsistent reported limits are not copied");
                nin.readings.fans.clear();
                Check(DpGenerate(BuildHwInventory(nin), stamp, gen).compatibility == "no-fans", "cfg1 gen: no fan gives 'no-fans'");
            }
            const std::string json = DpToJson(p);
            for (const char* banned : {"register", "curve", "nbfc", "command", "\"ec\"", "offset"})
                Check(json.find(banned) == std::string::npos, std::string("cfg1 schema: the profile has no field for low-level control data (") + banned + ")");
            Check(json.find("\"mode\": \"bios-auto\"") != std::string::npos, "cfg1 schema: the safe default is BIOS auto");

            // ---- validation: round trip
            DeviceProfile q; std::wstring why;
            Check(DpValidate(json, q, why) && why.empty(), "cfg1 val: a generated profile validates");
            Check(q.identity == p.identity && q.fans.size() == 2 && q.fans[0].name == L"CPU Fan" && q.fans[0].control == HwControl::ReportedUnverified && q.hash == DpComputeHash(q) && q.manufacturer == p.manufacturer, "cfg1 val: the parsed profile equals the generated one");
            Check(DpToJson(q) == json, "cfg1 val: parse then write is byte-identical (canonical)");

            // ---- validation: refusals, one condition each
            auto rejects = [&](const std::string& name, const std::string& text, const wchar_t* part = nullptr) {
                DeviceProfile o; std::wstring w;
                const bool ok = DpValidate(text, o, w);
                Check(!ok && !w.empty() && (!part || w.find(part) != std::wstring::npos), "cfg1 val refuses: " + name);
            };
            auto edit = [&](const std::string& from, const std::string& to) { std::string s = json; const size_t k = s.find(from); if (k == std::string::npos) { Check(false, "cfg1 test setup: text to edit not found: " + from); return s; } s.replace(k, from.size(), to); return s; };
            rejects("an empty file", "", L"empty");
            rejects("a file over 256 KiB", std::string(kDpMaxBytes + 1, ' '), L"256 KiB");
            rejects("text that is not JSON", "not json at all", L"JSON");
            rejects("truncated JSON", json.substr(0, json.size() / 2), L"JSON");
            rejects("a duplicate key", edit("\"version\": 1,", "\"version\": 1, \"version\": 1,"), L"JSON");
            rejects("a JSON array instead of an object", "[1,2,3]", L"JSON");
            rejects("another schema name", edit("ankhcore.device-profile", "ankhcore.other"), L"not an AnkhCore device profile");
            rejects("an unsupported version", edit("\"version\": 1,", "\"version\": 2,"), L"version");
            rejects("an unknown top-level field (low-level data cannot ride along)", edit("\"version\": 1,", "\"version\": 1,\n  \"ecRegisters\": [1, 2],"), L"field this version does not define");
            rejects("an unknown field in a fan", edit("\"channel\": 0,", "\"channel\": 0, \"curve\": [1],"), L"unknown field");
            rejects("an unknown field in the device", edit("\"board\": \"", "\"serial\": \"x\", \"board\": \""), L"unknown field");
            rejects("an unknown field in verification", edit("\"evidence\": []", "\"evidence\": [], \"note\": \"x\""), L"unknown field");
            rejects("a non-hex identity", edit(p.identity, std::string(64, 'z')), L"identity");
            rejects("an upper-case identity", edit(p.identity, std::string(64, 'A')), L"identity");
            rejects("a short identity", edit(p.identity, p.identity.substr(0, 63)), L"identity");
            rejects("an invalid fan role", edit("\"role\": \"cpu\"", "\"role\": \"fan0\""), L"role");
            rejects("an unknown telemetry class", edit("\"telemetry\": \"live\"", "\"telemetry\": \"great\""), L"class");
            rejects("an unknown control class", edit("\"control\": \"reported-unverified\"", "\"control\": \"full-control\""), L"class");
            rejects("a repeated channel", edit("\"channel\": 1,", "\"channel\": 0,"), L"channel");
            rejects("a channel out of range", edit("\"channel\": 1,", "\"channel\": 999,"), L"channel");
            rejects("a limit above 100", edit("\"maxPercent\": 100", "\"maxPercent\": 101"), L"limit");
            rejects("a fractional limit", edit("\"minPercent\": 20", "\"minPercent\": 20.5"), L"limit");
            rejects("limits with min above max", edit("\"maxPercent\": 100", "\"maxPercent\": 10"), L"limits");
            rejects("an empty fan name", edit("\"name\": \"CPU Fan\"", "\"name\": \"\""), L"name");
            rejects("a fan name with a control character", edit("\"name\": \"CPU Fan\"", "\"name\": \"CPU\\u0001Fan\""), L"name");
            rejects("a stored compatibility class that does not match the fans", edit("\"compatibility\": \"control-reported-unverified\"", "\"compatibility\": \"control-verified\""), L"compatibility");
            rejects("an edited model name (the hash no longer matches)", edit("Laptop X1", "Laptop X9"), L"hash");
            rejects("a wrong hash", edit(q.hash, std::string(64, '0')), L"hash");
            rejects("a missing hash", edit(",\n  \"hash\": \"" + q.hash + "\"", ""), L"hash");
            rejects("a safe default other than BIOS auto", edit("\"mode\": \"bios-auto\"", "\"mode\": \"manual\""), L"BIOS auto");
            rejects("a bad generation time", edit("2026-11-19T10:00:00Z", "yesterday evening"), L"time");
            {
                std::string many = json; std::string fanList;
                for (int i = 0; i < 17; ++i) fanList += std::string(i ? ", " : "") + "{\"name\": \"F" + std::to_string(i) + "\", \"channel\": " + std::to_string(i) + ", \"role\": \"unknown\", \"telemetry\": \"none\", \"control\": \"none\", \"minPercent\": -1, \"maxPercent\": -1}";
                const size_t a = many.find("\"fans\": ["), b = many.find("],\n  \"compatibility\"");
                many.replace(a, b - a + 1, "\"fans\": [" + fanList + "]");
                rejects("more than 16 fans", many, L"too many");
            }

            // ---- verification claims
            DeviceProfile vp = p; vp.fans[0].control = HwControl::Verified;
            rejects("a verified control with no evidence", DpToJson(vp), L"no trial evidence");
            DpEvidence ev; ev.candidate = L"HP ZBook Fury 15 G7"; ev.configSha256 = std::string(64, 'a'); ev.verdict = "compatible"; ev.baselineRpm = 1500; ev.peakRpm = 3000; ev.restoredSafeState = true; ev.recordedUtc = "2026-11-19T11:00:00Z";
            vp.evidence = {ev};
            rejects("a verified control whose safe release to firmware is not verified", DpToJson(vp), L"safe release");
            vp.safeReleaseVerified = true;
            { DeviceProfile ok; std::wstring w; Check(DpValidate(DpToJson(vp), ok, w), "cfg1 val: a verified control WITH valid evidence and a verified safe release is a valid file"); }
            for (int k = 0; k < 6; ++k) {
                DeviceProfile bad = vp; DpEvidence& e = bad.evidence[0];
                const char* name = "";
                if (k == 0) { e.verdict = "no-rpm-response"; name = "evidence whose verdict is not compatible"; }
                if (k == 1) { e.restoredSafeState = false; name = "evidence of a trial that did not restore a safe state"; }
                if (k == 2) { e.peakRpm = 1500; name = "evidence with no RPM rise"; }
                if (k == 3) { e.peakRpm = 1100; e.baselineRpm = 0; name = "evidence below the minimum active RPM"; }
                if (k == 4) { e.configSha256 = "abc"; name = "evidence with an invalid configuration hash"; }
                if (k == 5) { e.kind = "hand-written"; name = "evidence of an unknown kind"; }
                rejects(std::string("a verified control with ") + name, DpToJson(bad), L"evidence");
            }
            { DeviceProfile only = p; only.evidence = {ev}; rejects("trial evidence on a profile that marks no control verified", DpToJson(only), L"no control as verified"); }

            // ---- matching: identity hash only
            Check(DpMatchDevice(p, inv) == DpMatch::Exact, "cfg1 match: the profile matches the inventory it was generated from");
            {
                HwInputs other = in; other.sysProduct = L"Laptop X1 G2";
                Check(DpMatchDevice(p, BuildHwInventory(other)) == DpMatch::IdentityMismatch, "cfg1 match: a SIMILAR model name (Laptop X1 vs Laptop X1 G2) does not match");
                HwInputs other2 = in; other2.boardProduct = L"X1-BOARD-2";
                Check(DpMatchDevice(p, BuildHwInventory(other2)) == DpMatch::IdentityMismatch, "cfg1 match: the same model on another board does not match");
                HwInputs same = in; same.sysProduct = L"  laptop   x1 "; same.biosVersion = L"9.9.9";
                Check(DpMatchDevice(p, BuildHwInventory(same)) == DpMatch::Exact, "cfg1 match: spacing, case and BIOS version do not matter");
                HwInputs none = in; none.sysProduct.clear(); none.boardProduct.clear(); none.helperModel.clear();
                Check(DpMatchDevice(p, BuildHwInventory(none)) == DpMatch::NoIdentity, "cfg1 match: a device without an identity never matches");
            }
            // ---- DpVerifiedFor: the only route to deviceProfileVerified
            Check(!DpVerifiedFor(p, inv, true), "cfg1 verified: a generated profile (reported control only) is never verified, even with a local record");
            Check(!DpVerifiedFor(vp, inv, false), "cfg1 verified: a valid verified-looking FILE is not enough without a local record of the trial (a file is not authenticated)");
            Check(DpVerifiedFor(vp, inv, true), "cfg1 verified: valid profile + exact identity + a local record of the trial is verified");
            {
                HwInputs other = in; other.sysProduct = L"Laptop X1 G2";
                Check(!DpVerifiedFor(vp, BuildHwInventory(other), true), "cfg1 verified: a profile never verifies a different device");
                DeviceProfile nr = vp; nr.safeReleaseVerified = false;
                Check(!DpVerifiedFor(nr, inv, true), "cfg1 verified: without a verified safe release nothing is verified");
            }
            Check(!BuildHwInventory(in).caps.fanControlExposable, "cfg1 verified: nothing in the application sets deviceProfileVerified, so control is not offered");

            // ---- storage
            const std::wstring dir = std::wstring(tmp) + L"ankhcore_test_cfg1_" + std::to_wstring(GetCurrentProcessId());
            Evict(dir, true, false);
            const std::wstring path = DpPathFor(dir, p.identity);
            Check(path == dir + L"\\" + Utf8ToW(p.identity) + L".json", "cfg1 store: the file is named after the identity hash");
            Check(DpPathFor(dir, "..\\..\\x").empty() && DpPathFor(dir, "").empty() && DpPathFor(dir, std::string(64, 'g')).empty(), "cfg1 store: a path can only be built from a valid identity (no traversal)");
            { std::wstring w; Check(!DpLoad(dir, p.identity, q, w) && w.find(L"no profile") != std::wstring::npos, "cfg1 store: loading a missing profile says so"); }
            { std::wstring w; Check(DpSave(dir, p, w) && FileExists(path), "cfg1 store: a profile is saved"); }
            { std::wstring w; DeviceProfile l; Check(DpLoad(dir, p.identity, l, w) && DpToJson(l) == json, "cfg1 store: it loads back byte-identical"); }
            { std::wstring w; Check(DpSave(dir, p, w), "cfg1 store: saving again over it works"); Check(ListDir(dir, false).size() == 1, "cfg1 store: no temporary file is left behind"); }
            { DeviceProfile noId = p; noId.identity.clear(); std::wstring w; Check(!DpSave(dir, noId, w) && w.find(L"identity") != std::wstring::npos, "cfg1 store: a profile without an identity is not saved"); }
            { DeviceProfile broken = p; broken.fans[0].name.clear(); std::wstring w; Check(!DpSave(dir, broken, w), "cfg1 store: a profile that would not load back is not written"); }
            {   // a damaged file is refused and left exactly as it is
                std::vector<uint8_t> before; ReadAll(path, before);
                std::string t(before.begin(), before.end()); t[t.find("Laptop")] = 'X';
                WriteAtomic(path, reinterpret_cast<const uint8_t*>(t.data()), t.size());
                std::vector<uint8_t> damaged; ReadAll(path, damaged);
                std::wstring w; DeviceProfile l;
                Check(!DpLoad(dir, p.identity, l, w) && w.find(L"hash") != std::wstring::npos, "cfg1 store: a damaged file is refused");
                std::vector<uint8_t> after; ReadAll(path, after);
                Check(after == damaged, "cfg1 store: ... and is left exactly as it was (nothing deleted or rewritten)");
                WriteAtomic(path, before.data(), before.size());
            }
            {   // a valid profile under another identity's file name is refused
                DeviceProfile other = p; other.identity = std::string(64, 'b');
                const std::string oj = DpToJson(other);
                const std::wstring wrongName = DpPathFor(dir, p.identity);
                WriteAtomic(wrongName, reinterpret_cast<const uint8_t*>(oj.data()), oj.size());
                std::wstring w; DeviceProfile l;
                Check(!DpLoad(dir, p.identity, l, w) && w.find(L"another device identity") != std::wstring::npos, "cfg1 store: a profile of another device under this file name is refused");
                std::string pj = json; WriteAtomic(wrongName, reinterpret_cast<const uint8_t*>(pj.data()), pj.size());
            }
            {   // an oversized file is refused before it is parsed
                const std::string big(kDpMaxBytes + 10, 'x');
                WriteAtomic(path, reinterpret_cast<const uint8_t*>(big.data()), big.size());
                std::wstring w; DeviceProfile l;
                Check(!DpLoad(dir, p.identity, l, w) && w.find(L"256 KiB") != std::wstring::npos, "cfg1 store: an oversized file is refused");
            }
            Evict(dir, true, false);
        }
        // ---- M-2.6-CFG1: the safe fan-control abstraction (mock backend only; nothing here can reach a fan)
        {
            struct Mock {
                bool alive = true, setOk = true, releaseOk = true;
                int sets = 0, releases = 0, aliveCalls = 0;
                std::vector<std::pair<int, int>> sentCmds;
            };
            auto mkProfile = [] {
                DeviceProfile p;
                p.identity = std::string(64, 'c'); p.safeReleaseVerified = true;
                DpFan a; a.name = L"CPU Fan"; a.channel = 0; a.telemetry = HwTelemetry::Live; a.control = HwControl::Verified; a.minPercent = 30; a.maxPercent = 90;
                DpFan b; b.name = L"GPU Fan"; b.channel = 1; b.telemetry = HwTelemetry::Live; b.control = HwControl::ReportedUnverified;
                p.fans = {a, b};
                return p;
            };
            auto backendFor = [](Mock& m) {
                FgBackend be;
                be.alive = [&m] { ++m.aliveCalls; return m.alive; };
                be.setPercent = [&m](int ch, int pct, std::wstring& err) { ++m.sets; m.sentCmds.push_back({ch, pct}); if (!m.setOk) { err = L"mock failure"; return false; } return true; };
                be.releaseToFirmware = [&m](std::wstring& err) { ++m.releases; if (!m.releaseOk) { err = L"mock failure"; return false; } return true; };
                return be;
            };
            auto good = [](double rpm0 = 1500, double cpu = 60) { FgTelemetry t; t.fresh = true; t.cpuTempC = cpu; t.gpuTempC = 55; t.rpmByChannel = {rpm0, 1400}; return t; };
            auto armed = [&](Mock& m, FgConfig cfg = {}) {   // a guard that has everything it needs, in Manual mode
                auto g = std::make_unique<FanControlGuard>(mkProfile(), true, backendFor(m), cfg);
                g->Tick(good());
                return g;
            };

            // ---- defaults and gates: a refused request never touches the backend
            {
                Mock m; FanControlGuard g(mkProfile(), false, backendFor(m)); g.Tick(good());
                Check(g.Mode() == FgMode::BiosAuto, "fg gate: the default mode is BIOS Auto");
                Check(!g.RequestMode(FgMode::Manual, true).ok && g.Mode() == FgMode::BiosAuto, "fg gate: Manual is refused without a verified device profile");
                Check(!g.RequestMode(FgMode::AnkhAuto, true).ok, "fg gate: AnkhCore Auto is refused without a verified device profile");
                Check(m.sets == 0 && m.releases == 0 && m.aliveCalls == 0, "fg gate: refused requests never touched the backend at all");
                Check(g.RequestMode(FgMode::BiosAuto, false).ok && m.releases == 0 && m.sets == 0, "fg gate: asking for BIOS Auto when AnkhCore never owned the fans sends nothing");
            }
            {
                Mock m; FanControlGuard g(mkProfile(), true, backendFor(m)); g.Tick(good());
                const FgResult r = g.RequestMode(FgMode::Manual, false);
                Check(!r.ok && r.reason.find(L"consent") != std::wstring::npos && g.Mode() == FgMode::BiosAuto, "fg gate: explicit user consent is required");
                Check(g.RequestMode(FgMode::Manual, true).ok && g.Mode() == FgMode::Manual, "fg gate: with consent, a verified profile, limits, backend and fresh telemetry Manual is accepted");
                Check(m.sets == 0, "fg gate: entering Manual sends no command by itself");
            }
            {
                Mock m; FanControlGuard g(mkProfile(), true, backendFor(m)); g.Tick(good());
                const FgResult r = g.RequestMode(FgMode::AnkhAuto, true);
                Check(!r.ok && r.reason.find(L"device-specific") != std::wstring::npos && r.reason.find(L"no universal curve") != std::wstring::npos, "fg gate: AnkhCore Auto is refused: no verified device-specific policy exists, and no universal curve is applied");
            }
            {
                Mock m; FgConfig cfg; cfg.policy = [](int, const FgTelemetry&) { return 50; };
                FanControlGuard g(mkProfile(), true, backendFor(m), cfg); g.Tick(good());
                Check(g.RequestMode(FgMode::AnkhAuto, true).ok, "fg gate: AnkhCore Auto is accepted only when a device-specific policy is supplied");
            }
            {
                Mock m; DeviceProfile p = mkProfile(); p.safeReleaseVerified = false;
                FanControlGuard g(p, true, backendFor(m)); g.Tick(good());
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when a safe release to firmware is not verified");
            }
            {
                Mock m; DeviceProfile p = mkProfile(); p.fans[0].minPercent = -1; p.fans[0].maxPercent = -1;
                FanControlGuard g(p, true, backendFor(m)); g.Tick(good());
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when the verified fan has no known limits");
            }
            {
                Mock m; DeviceProfile p = mkProfile(); p.fans[0].control = HwControl::ReportedUnverified;
                FanControlGuard g(p, true, backendFor(m)); g.Tick(good());
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when no fan has a VERIFIED control (a reported one is not enough)");
            }
            {
                Mock m; FanControlGuard g(mkProfile(), true, FgBackend{});
                g.Tick(good());
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when there is no backend");
            }
            {
                Mock m; m.alive = false; FanControlGuard g(mkProfile(), true, backendFor(m)); g.Tick(good());
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when the backend is not available");
            }
            {
                Mock m; FanControlGuard g(mkProfile(), true, backendFor(m));
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused before any telemetry has arrived");
                FgTelemetry stale = good(); stale.fresh = false; g.Tick(stale);
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused on stale telemetry");
                FgTelemetry hot = good(1500, 90); g.Tick(hot);
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused when a temperature is already at the safety limit");
                FgTelemetry nan = good(); nan.cpuTempC = std::numeric_limits<double>::quiet_NaN(); g.Tick(nan);
                Check(!g.RequestMode(FgMode::Manual, true).ok, "fg gate: refused on an invalid temperature");
                Check(m.sets == 0 && m.releases == 0, "fg gate: none of those touched the actuator or the release");
            }
            // ---- commands are bounded
            {
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true);
                Check(g->SetManual(0, 60).ok && m.sentCmds.size() == 1 && m.sentCmds[0] == std::make_pair(0, 60), "fg cmd: an in-range request reaches the backend with exactly that value");
                Check(!g->SetManual(0, 39).ok, "fg cmd: below the 40 % minimum duty (D23) is refused, not clamped");
                Check(!g->SetManual(0, 91).ok, "fg cmd: above the verified maximum is refused, not clamped");
                Check(!g->SetManual(0, -5).ok && !g->SetManual(0, 1000).ok, "fg cmd: absurd values are refused");
                Check(!g->SetManual(1, 60).ok, "fg cmd: a fan without a verified control is refused");
                Check(!g->SetManual(7, 60).ok, "fg cmd: an unknown channel is refused");
                Check(m.sets == 1, "fg cmd: only the one valid command ever reached the backend");
                Check(g->Mode() == FgMode::Manual && g->Fault() == FgFault::None, "fg cmd: refusals are not faults; Manual stays active");
            }
            {
                Mock m; auto g = armed(m);
                Check(!g->SetManual(0, 60).ok && m.sets == 0, "fg cmd: no command in BIOS Auto mode");
            }
            // ---- watchdog: every fault stops commands, releases once, and falls back to BIOS Auto
            struct Case { const char* name; std::function<void(Mock&, FanControlGuard&)> trigger; FgFault want; };
            auto badTick = [&](std::function<void(FgTelemetry&)> mut) { return [&good, mut](Mock&, FanControlGuard& g) { FgTelemetry t = good(); mut(t); g.Tick(t); }; };
            const Case cases[] = {
                {"stale telemetry", badTick([](FgTelemetry& t) { t.fresh = false; }), FgFault::StaleTelemetry},
                {"a NaN CPU temperature", badTick([](FgTelemetry& t) { t.cpuTempC = std::numeric_limits<double>::quiet_NaN(); }), FgFault::InvalidReading},
                {"a zero CPU temperature", badTick([](FgTelemetry& t) { t.cpuTempC = 0; }), FgFault::InvalidReading},
                {"an impossible CPU temperature", badTick([](FgTelemetry& t) { t.cpuTempC = 300; }), FgFault::InvalidReading},
                {"an impossible GPU temperature", badTick([](FgTelemetry& t) { t.gpuTempC = 500; }), FgFault::InvalidReading},
                {"no RPM reported", badTick([](FgTelemetry& t) { t.rpmByChannel.clear(); }), FgFault::InvalidReading},
                {"a CPU temperature at the limit", badTick([](FgTelemetry& t) { t.cpuTempC = 85; }), FgFault::Overtemperature},
                {"a GPU temperature at the limit", badTick([](FgTelemetry& t) { t.gpuTempC = 86; }), FgFault::Overtemperature},
                {"backend loss", [](Mock& m, FanControlGuard& g) { m.alive = false; g.Tick(FgTelemetry{true, 60, 55, {1500, 1400}}); }, FgFault::BackendLost},
            };
            for (const Case& cs : cases) {
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true); g->SetManual(0, 60);
                const int setsBefore = m.sets;
                cs.trigger(m, *g);
                const std::string nm = cs.name;
                Check(g->Fault() == cs.want, "fg watchdog " + nm + ": the fault is recorded as the right kind");
                Check(g->Mode() == FgMode::BiosAuto, "fg watchdog " + nm + ": control falls back to BIOS Auto");
                Check(m.releases == 1, "fg watchdog " + nm + ": control was released to the firmware exactly once");
                m.alive = true;
                Check(!g->SetManual(0, 60).ok && m.sets == setsBefore, "fg watchdog " + nm + ": no command is issued after the fault");
                g->Tick(good()); g->Tick(good());
                Check(m.releases == 1 && m.sets == setsBefore, "fg watchdog " + nm + ": further telemetry causes no more commands or releases");
                const auto& log = g->Log();
                Check(std::any_of(log.begin(), log.end(), [](const FgEvent& e) { return e.what.rfind(L"fault:", 0) == 0 && !e.ok; }) && std::any_of(log.begin(), log.end(), [](const FgEvent& e) { return e.what.rfind(L"released to firmware", 0) == 0 && e.ok; }), "fg watchdog " + nm + ": the fault and the fallback are logged");
                Check(!g->ActuationDisabled(), "fg watchdog " + nm + ": a clean release does not disable actuation");
            }
            {   // actuator error
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true);
                m.setOk = false;
                const FgResult r = g->SetManual(0, 60);
                Check(!r.ok && g->Fault() == FgFault::ActuatorError && g->Mode() == FgMode::BiosAuto && m.releases == 1, "fg watchdog actuator error: the failed command falls back and releases once");
                m.setOk = true;
                Check(!g->SetManual(0, 60).ok && m.sets == 1, "fg watchdog actuator error: no retry is issued");
            }
            {   // backend lost at command time
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true);
                m.alive = false;
                Check(!g->SetManual(0, 60).ok && g->Fault() == FgFault::BackendLost && m.sets == 0, "fg watchdog backend loss at command time: nothing is sent and control is released");
            }
            // ---- RPM response is verified, and a missing response is a fault
            {
                Mock m; FgConfig cfg; cfg.responseTicks = 3; auto g = armed(m, cfg); g->RequestMode(FgMode::Manual, true);
                g->SetManual(0, 60);
                g->Tick(good(1500)); g->Tick(good(1600));
                Check(g->Fault() == FgFault::None && g->Mode() == FgMode::Manual, "fg rpm: within the allowed ticks the fan has not answered yet; still waiting");
                g->Tick(good(2300));
                Check(g->Fault() == FgFault::None && g->Mode() == FgMode::Manual, "fg rpm: an RPM rise of the trial's threshold confirms the command");
                g->Tick(good(2300)); g->Tick(good(2300)); g->Tick(good(2300));
                Check(g->Fault() == FgFault::None, "fg rpm: once confirmed there is nothing left to wait for");
                const auto& log = g->Log();
                Check(std::any_of(log.begin(), log.end(), [](const FgEvent& e) { return e.what.rfind(L"verified: fan 0 responded", 0) == 0; }), "fg rpm: the confirmation is logged");
            }
            {
                Mock m; FgConfig cfg; cfg.responseTicks = 3; auto g = armed(m, cfg); g->RequestMode(FgMode::Manual, true);
                g->SetManual(0, 80);
                g->Tick(good(1500)); g->Tick(good(1520)); g->Tick(good(1510));
                Check(g->Fault() == FgFault::RpmMismatch && g->Mode() == FgMode::BiosAuto && m.releases == 1, "fg rpm: a fan that never answers is a fault: control returns to the firmware");
            }
            {   // a small change is not expected to move the fan visibly
                Mock m; FgConfig cfg; cfg.responseTicks = 2; auto g = armed(m, cfg); g->RequestMode(FgMode::Manual, true);
                g->SetManual(0, 60); g->Tick(good(2300));
                g->SetManual(0, 63);
                g->Tick(good(2300)); g->Tick(good(2300)); g->Tick(good(2300));
                Check(g->Fault() == FgFault::None && g->Mode() == FgMode::Manual, "fg rpm: a 3-point change expects no visible RPM response");
            }
            // ---- release failure: actuation is marked unsupported for the device
            {
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true); g->SetManual(0, 60);
                m.releaseOk = false;
                g->Tick(FgTelemetry{false, 60, 55, {1500, 1400}});
                Check(g->Fault() == FgFault::ReleaseFailed && g->ActuationDisabled() && g->Mode() == FgMode::BiosAuto, "fg release failure: the device is marked unsupported for actuation");
                m.releaseOk = true;
                const int sets = m.sets;
                g->Tick(good());
                const FgResult r = g->RequestMode(FgMode::Manual, true);
                Check(!r.ok && r.reason.find(L"cannot be returned to the firmware safely") != std::wstring::npos && m.sets == sets, "fg release failure: Manual is refused from then on, even with consent and a working backend");
                const auto& log = g->Log();
                Check(std::any_of(log.begin(), log.end(), [](const FgEvent& e) { return e.what.find(L"FAILED") != std::wstring::npos && !e.ok; }), "fg release failure: it is logged");
            }
            // ---- mode switching and shutdown
            {
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true); g->SetManual(0, 60);
                Check(g->RequestMode(FgMode::BiosAuto, false).ok && g->Mode() == FgMode::BiosAuto && m.releases == 1, "fg mode: switching back to BIOS Auto releases ownership");
                Check(!g->SetManual(0, 60).ok && m.sets == 1, "fg mode: no command after switching to BIOS Auto");
                g->Tick(good());
                Check(g->RequestMode(FgMode::Manual, true).ok && g->Mode() == FgMode::Manual, "fg mode: Manual can be entered again after a clean release");
                Check(g->RequestMode(FgMode::BiosAuto, false).ok && m.releases == 2, "fg mode: and released again");
            }
            {
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true); g->SetManual(0, 60);
                g->Shutdown();
                Check(g->Mode() == FgMode::BiosAuto && m.releases == 1, "fg shutdown: control is returned to the firmware once");
                g->Shutdown();
                Check(m.releases == 1, "fg shutdown: a second call does nothing");
                Check(!g->RequestMode(FgMode::Manual, true).ok && !g->SetManual(0, 60).ok && m.sets == 1, "fg shutdown: nothing can be started afterwards");
                const auto& log = g->Log();
                Check(std::any_of(log.begin(), log.end(), [](const FgEvent& e) { return e.what.rfind(L"shutdown:", 0) == 0; }), "fg shutdown: logged");
            }
            {
                Mock m; auto g = armed(m);
                g->Shutdown();
                Check(m.releases == 0 && m.sets == 0, "fg shutdown: when AnkhCore never owned the fans nothing is sent");
            }
            {   // shutdown with a failing release disables actuation instead of pretending
                Mock m; auto g = armed(m); g->RequestMode(FgMode::Manual, true);
                m.releaseOk = false; g->Shutdown();
                Check(g->ActuationDisabled() && g->Fault() == FgFault::ReleaseFailed, "fg shutdown: a failed release at exit is recorded, not hidden");
            }
        }
        // the module is pure: building an inventory changes nothing in its inputs and needs no live source
        {
            HwInputs in = mkIn(); in.readings.fans = {mkFan(L"A", 1000, FanCapability::Controllable, true)};
            const HwInputs copy = in;
            (void)BuildHwInventory(in);
            Check(in.readings.fans.size() == copy.readings.fans.size() && in.readings.cpuTemp == copy.readings.cpuTemp && in.os == copy.os && in.helperInstalled == copy.helperInstalled, "hw1 pure: building an inventory does not change its inputs");
        }
    }
    {   // ---- M-2.6-PROC1: process inventory, protection classification, policy gate (fabricated data; plus ONE live read-only snapshot)
        ProcContext ctx; ctx.selfPid = 5000; ctx.selfSession = 1; ctx.systemRoot = L"C:\\Windows"; ctx.securityDirs = {L"C:\\Program Files\\Windows Defender"};
        auto mk = [](DWORD pid, DWORD ppid, const wchar_t* name, const wchar_t* path, DWORD session = 1, uint64_t ws = 100ull << 20) {
            ProcRaw r; r.pid = pid; r.ppid = ppid; r.name = name; r.path = path; r.session = session; r.workingSet = ws; r.priorityClass = kProcPrioNormal; return r;
        };
        auto cls = [&](const ProcRaw& r) { return ProcClassify(r, ctx, {}); };
        auto isProt = [&](const ProcRaw& r, ProcClass c) { const ProcEntry e = cls(r); return e.cls == c && e.prot == ProcProtection::Protected && !e.reason.empty(); };

        // critical, by id and by name (case-insensitive), whatever the path says
        Check(isProt(mk(0, 0, L"System Idle Process", L""), ProcClass::Critical), "proc critical: pid 0 is protected");
        Check(isProt(mk(4, 0, L"System", L""), ProcClass::Critical), "proc critical: System (pid 4) is protected");
        for (const wchar_t* n : {L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe", L"Registry", L"Memory Compression", L"Secure System"})
            Check(isProt(mk(900, 4, n, L"C:\\Windows\\System32\\x.exe"), ProcClass::Critical), std::string("proc critical: ") + WToUtf8(n) + " is protected");
        Check(isProt(mk(901, 4, L"LSASS.EXE", L"C:\\Windows\\System32\\lsass.exe"), ProcClass::Critical), "proc critical: the name match ignores case");
        Check(isProt(mk(902, 1, L"svchost.exe", L"C:\\Users\\x\\AppData\\svchost.exe"), ProcClass::Critical), "proc critical: a look-alike svchost.exe outside the Windows folder is protected too (conservative)");

        // AnkhCore itself and everything it started
        Check(isProt(mk(5000, 1, L"AnkhCore.exe", L"C:\\Program Files\\AnkhCore\\AnkhCore.exe"), ProcClass::Self), "proc self: this process is protected");
        Check(isProt(mk(6000, 1, L"AnkhSensors.exe", L"C:\\Program Files\\AnkhCore\\Sensors\\AnkhSensors.exe"), ProcClass::Self), "proc self: AnkhSensors.exe is protected by name");
        Check(isProt(mk(6001, 1, L"AnkhCore_TEST.exe", L"D:\\build\\AnkhCore_TEST.exe"), ProcClass::Self), "proc self: the test build's executable is protected by name");
        {
            const std::vector<ProcRaw> raw = {mk(5000, 1, L"AnkhCore.exe", L"C:\\x\\AnkhCore.exe"), mk(5100, 5000, L"chrome.exe", L"C:\\Program Files\\C\\chrome.exe"), mk(5200, 5100, L"helper.exe", L"C:\\Program Files\\C\\helper.exe"),
                                              mk(7000, 1, L"chrome.exe", L"C:\\Program Files\\C\\chrome.exe"), mk(1, 0, L"explorer.exe", L"C:\\Windows\\explorer.exe")};
            const ProcInventory inv = ProcBuildInventory(raw, ctx);
            auto find = [&](DWORD pid) -> const ProcEntry* { for (const auto& e : inv.entries) if (e.raw.pid == pid) return &e; return nullptr; };
            Check(find(5100) && find(5100)->cls == ProcClass::Self, "proc self: a process AnkhCore started is AnkhCore's (protected), even with an ordinary name");
            Check(find(5200) && find(5200)->cls == ProcClass::Self, "proc self: so is its child (the whole tree)");
            Check(find(7000) && find(7000)->cls == ProcClass::Application && find(7000)->prot == ProcProtection::Adjustable, "proc self: an unrelated process with the same name is an ordinary application");
        }
        // security
        for (const wchar_t* n : {L"MsMpEng.exe", L"NisSrv.exe", L"SecurityHealthService.exe", L"smartscreen.exe", L"avp.exe", L"csfalconservice.exe", L"SentinelAgent.exe", L"ekrn.exe"})
            Check(isProt(mk(1100, 1, n, L"C:\\Program Files\\Sec\\x.exe"), ProcClass::Security), std::string("proc security: ") + WToUtf8(n) + " is protected");
        Check(isProt(mk(1101, 1, L"whatever.exe", L"C:\\Program Files\\Windows Defender\\whatever.exe"), ProcClass::Security), "proc security: anything running from the Defender folder is protected");
        Check(isProt(mk(1102, 1, L"x.exe", L"c:/program files/windows defender/sub/x.exe"), ProcClass::Security), "proc security: path case and slashes do not matter");
        Check(cls(mk(1103, 1, L"x.exe", L"C:\\Program Files\\Windows Defender Tools\\x.exe")).cls == ProcClass::Application, "proc security: a folder that only starts with the same text is not the Defender folder");
        // session
        Check(isProt(mk(1200, 4, L"worker.exe", L"C:\\Apps\\worker.exe", 0), ProcClass::Service), "proc session: a session 0 process (service) is protected");
        Check(isProt(mk(1201, 1, L"app.exe", L"C:\\Apps\\app.exe", 2), ProcClass::Service), "proc session: another user's session is protected");
        Check(isProt(mk(1202, 1, L"app.exe", L"C:\\Apps\\app.exe", 0xFFFFFFFF), ProcClass::Service), "proc session: an unknown session is protected");
        { ProcContext bad = ctx; bad.selfSession = 0xFFFFFFFF; Check(ProcClassify(mk(1203, 1, L"app.exe", L"C:\\Apps\\app.exe"), bad, {}).prot == ProcProtection::Protected, "proc session: when AnkhCore's own session is unknown nothing is adjustable"); }
        // identification
        Check(isProt(mk(1300, 1, L"mystery.exe", L""), ProcClass::Unknown), "proc unknown: a process whose path cannot be read is protected (fail safe)");
        { ProcContext bad = ctx; bad.systemRoot.clear(); Check(ProcClassify(mk(1301, 1, L"app.exe", L"C:\\Apps\\app.exe"), bad, {}).prot == ProcProtection::Protected, "proc unknown: without a known Windows folder nothing is adjustable"); }
        Check(isProt(mk(1302, 1, L"notepad.exe", L"C:\\Windows\\System32\\notepad.exe"), ProcClass::System), "proc system: a Windows component is protected");
        Check(isProt(mk(1303, 1, L"x.exe", L"c:/WINDOWS/system32/x.exe"), ProcClass::System), "proc system: case and slashes do not matter");
        Check(cls(mk(1304, 1, L"app.exe", L"C:\\WindowsApps\\Vendor\\app.exe")).cls == ProcClass::Application, "proc system: C:\\WindowsApps is not C:\\Windows (the prefix needs a separator)");
        Check(isProt(mk(1305, 1, L"explorer.exe", L"C:\\Windows\\explorer.exe"), ProcClass::Shell), "proc shell: the desktop shell is protected");
        Check(isProt(mk(1306, 1, L"StartMenuExperienceHost.exe", L"C:\\Windows\\SystemApps\\x\\StartMenuExperienceHost.exe"), ProcClass::Shell), "proc shell: the Start menu host is protected");
        { const ProcEntry e = cls(mk(1400, 1, L"EasyAntiCheat.exe", L"D:\\Games\\EasyAntiCheat.exe")); Check(e.cls == ProcClass::Sensitive && e.prot == ProcProtection::Caution, "proc sensitive: an anti-cheat process needs caution"); }
        { const ProcEntry e = cls(mk(1401, 1, L"vmware-vmx.exe", L"C:\\Program Files\\VMware\\vmware-vmx.exe")); Check(e.cls == ProcClass::Sensitive && e.prot == ProcProtection::Caution, "proc sensitive: a virtual machine process needs caution"); }
        { const ProcEntry e = cls(mk(1500, 1, L"game.exe", L"D:\\Games\\game.exe")); Check(e.cls == ProcClass::Application && e.prot == ProcProtection::Adjustable, "proc app: an ordinary application in the user's session is the only adjustable kind"); }

        // inventory: counts and groups
        {
            const std::vector<ProcRaw> raw = {mk(4, 0, L"System", L""), mk(1, 4, L"chrome.exe", L"C:\\Apps\\chrome.exe", 1, 300ull << 20), mk(2, 4, L"Chrome.exe", L"C:\\Apps\\chrome.exe", 1, 200ull << 20),
                                              mk(3, 4, L"game.exe", L"D:\\g\\game.exe", 1, 900ull << 20), mk(6, 4, L"EasyAntiCheat.exe", L"D:\\g\\e.exe", 1, 50ull << 20), mk(7, 4, L"app.exe", L"", 1, 10ull << 20)};
            const ProcInventory inv = ProcBuildInventory(raw, ctx);
            Check(inv.entries.size() == 6 && inv.protectedCount == 2 && inv.cautionCount == 1 && inv.adjustableCount == 3, "proc inventory: counts per protection level add up");
            Check(inv.groups.size() == 5 && inv.groups[0].name == L"game.exe" && inv.groups[1].count == 2 && inv.groups[1].workingSet == (500ull << 20), "proc inventory: instances group by name (case-insensitive), largest memory first");
            Check(inv.groups[1].pids.size() == 2, "proc inventory: a group keeps its process ids");
            {
                std::vector<ProcRaw> r2 = raw; r2[2].path = L"";   // one chrome instance cannot be identified
                const ProcInventory i2 = ProcBuildInventory(r2, ctx);
                bool ok = false; for (const auto& g : i2.groups) if (Lower(g.name) == L"chrome.exe") ok = g.prot == ProcProtection::Protected;
                Check(ok, "proc inventory: a group takes the most restrictive protection of its instances");
            }
            const std::wstring t = ProcSummaryText(inv, 2);
            Check(t.find(L"Processes: 6") != std::wstring::npos && t.find(L"game.exe x1, 900 MiB, Adjustable") != std::wstring::npos, "proc inventory: the summary shows counts and the biggest groups");
            Check(t.find(L"C:\\") == std::wstring::npos && t.find(L"D:\\") == std::wstring::npos, "proc inventory: the summary has no paths (they hold the user name)");
            Check(ProcBuildInventory({}, ctx).entries.empty(), "proc inventory: an empty snapshot gives an empty inventory");
            // a parent cycle in the data must not hang the tree walk
            const std::vector<ProcRaw> cyc = {mk(10, 11, L"a.exe", L"C:\\a.exe"), mk(11, 10, L"b.exe", L"C:\\b.exe")};
            Check(ProcBuildInventory(cyc, ctx).entries.size() == 2, "proc inventory: a parent cycle in the data is harmless");
        }

        // policy gate
        {
            const ProcEntry app = cls(mk(1500, 1, L"game.exe", L"D:\\Games\\game.exe"));
            const ProcEntry sens = cls(mk(1400, 1, L"EasyAntiCheat.exe", L"D:\\Games\\e.exe"));
            const ProcEntry crit = cls(mk(900, 4, L"lsass.exe", L"C:\\Windows\\System32\\lsass.exe"));
            const ProcEntry self = cls(mk(5000, 1, L"AnkhCore.exe", L"C:\\x\\AnkhCore.exe"));
            const ProcEntry unk = cls(mk(1300, 1, L"mystery.exe", L""));
            for (ProcAction a : {ProcAction::SetPriority, ProcAction::SetAffinity, ProcAction::SetEfficiency, ProcAction::Terminate}) {
                const std::string an = a == ProcAction::SetPriority ? "priority" : a == ProcAction::SetAffinity ? "affinity" : a == ProcAction::SetEfficiency ? "efficiency" : "terminate";
                Check(!ProcMayChange(crit, a, kProcPrioIdle, 5000).allowed, "proc gate: a critical process is never changed (" + an + ")");
                Check(!ProcMayChange(self, a, kProcPrioIdle, 5000).allowed, "proc gate: AnkhCore never changes itself (" + an + ")");
                Check(!ProcMayChange(unk, a, kProcPrioIdle, 5000).allowed, "proc gate: an unidentifiable process is never changed (" + an + ")");
            }
            Check(ProcMayChange(app, ProcAction::Terminate, 0, 5000).allowed && ProcMayChange(app, ProcAction::Terminate, 0, 5000).needsConfirmation && !ProcMayChange(sens, ProcAction::Terminate, 0, 5000).allowed, "proc gate: an ordinary application can be ended only after a confirmation; a sensitive one never");
            Check(ProcMayChange(sens, ProcAction::Terminate, 0, 5000).reason.size() > 5 && ProcMayChange(app, ProcAction::Terminate, 0, 5000).reason.find(L"unsaved work") != std::wstring::npos, "proc gate: the answer always says why");
            Check(!ProcMayChange(crit, ProcAction::SetPriority, kProcPrioIdle, 5000).reason.empty(), "proc gate: a refusal always carries a reason");
            ProcDecision d = ProcMayChange(app, ProcAction::SetPriority, kProcPrioBelowNormal, 5000);
            Check(d.allowed && !d.needsConfirmation && d.mustRecordPrevious, "proc gate: lowering an ordinary application's priority is allowed and must record the previous state");
            d = ProcMayChange(app, ProcAction::SetPriority, kProcPrioIdle, 5000);
            Check(d.allowed && !d.needsConfirmation, "proc gate: Idle priority for an ordinary application is allowed");
            d = ProcMayChange(app, ProcAction::SetPriority, kProcPrioHigh, 5000);
            Check(d.allowed && d.needsConfirmation && d.mustRecordPrevious, "proc gate: raising to High needs a confirmation");
            d = ProcMayChange(app, ProcAction::SetPriority, kProcPrioAboveNormal, 5000);
            Check(d.allowed && d.needsConfirmation, "proc gate: raising to Above Normal needs a confirmation");
            Check(!ProcMayChange(app, ProcAction::SetPriority, kProcPrioRealtime, 5000).allowed, "proc gate: the real-time class is never allowed");
            Check(!ProcMayChange(app, ProcAction::SetPriority, 0x1234, 5000).allowed && !ProcMayChange(app, ProcAction::SetPriority, 0, 5000).allowed && !ProcMayChange(app, ProcAction::SetPriority, 0x100000000ull, 5000).allowed, "proc gate: a value that is not a priority class is refused");
            d = ProcMayChange(app, ProcAction::SetAffinity, 0x3, 5000);
            Check(d.allowed && d.needsConfirmation && d.mustRecordPrevious, "proc gate: an affinity change is always confirmed and records the previous mask");
            Check(!ProcMayChange(app, ProcAction::SetAffinity, 0, 5000).allowed, "proc gate: an empty CPU mask is refused");
            d = ProcMayChange(app, ProcAction::SetEfficiency, 0, 5000);
            Check(d.allowed && !d.needsConfirmation && d.mustRecordPrevious, "proc gate: an efficiency change on an ordinary application is allowed and recorded");
            d = ProcMayChange(sens, ProcAction::SetPriority, kProcPrioBelowNormal, 5000);
            Check(d.allowed && d.needsConfirmation, "proc gate: a sensitive third-party process needs a confirmation even for a lower priority");
            d = ProcMayChange(sens, ProcAction::SetEfficiency, 0, 5000);
            Check(d.allowed && d.needsConfirmation, "proc gate: ... and for an efficiency change");
            ProcEntry forged = app; forged.raw.pid = 5000;
            Check(!ProcMayChange(forged, ProcAction::SetPriority, kProcPrioIdle, 5000).allowed, "proc gate: the process id of AnkhCore itself is refused even if it was classified as an application");
        }

        // ONE live, read-only snapshot of this machine (integration level: it reads the real process table, changes nothing)
        {
            const ProcContext live = ProcContextLive();
            const std::vector<ProcRaw> raw = ProcCollectLive();
            Check(!raw.empty() && live.selfPid == GetCurrentProcessId() && !live.systemRoot.empty(), "proc live: a real snapshot has processes and a usable context");
            const ProcInventory inv = ProcBuildInventory(raw, live);
            const ProcEntry* me = nullptr; const ProcEntry* sys = nullptr;
            for (const auto& e : inv.entries) { if (e.raw.pid == live.selfPid) me = &e; if (e.raw.pid == 4) sys = &e; }
            Check(me && me->cls == ProcClass::Self && me->prot == ProcProtection::Protected, "proc live: this test process is found and is classified as AnkhCore's own (protected)");
            Check(me && !me->raw.path.empty() && me->raw.workingSet > 0 && me->raw.priorityClass != 0, "proc live: its path, memory and priority class were read");
            Check(sys && sys->cls == ProcClass::Critical && sys->prot == ProcProtection::Protected, "proc live: the System process is critical and protected");
            bool anyAdjustableWithoutPath = false, anyAdjustableInOtherSession = false, anyAdjustableInWindows = false;
            for (const auto& e : inv.entries) if (e.prot == ProcProtection::Adjustable) {
                if (e.raw.path.empty()) anyAdjustableWithoutPath = true;
                if (e.raw.session != live.selfSession) anyAdjustableInOtherSession = true;
                if (!live.systemRoot.empty() && Lower(e.raw.path).rfind(Lower(live.systemRoot) + L"\\", 0) == 0) anyAdjustableInWindows = true;
            }
            Check(!anyAdjustableWithoutPath && !anyAdjustableInOtherSession && !anyAdjustableInWindows, "proc live: on this real machine nothing adjustable lacks a path, lives in another session or in the Windows folder");
            Check(inv.protectedCount > 0 && inv.protectedCount + inv.cautionCount + inv.adjustableCount == static_cast<int>(inv.entries.size()), "proc live: the counts add up on the real table");
        }
    }
    {   // ---- M-2.6-GPU1: per-process GPU utilization (the rule of the M-GPU2 baseline, now real and tested)
        auto inst = [](int pid, const wchar_t* luidLo, int phys, int eng, const wchar_t* type) {
            return L"pid_" + std::to_wstring(pid) + L"_luid_0x00000000_" + std::wstring(luidLo) + L"_phys_" + std::to_wstring(phys) + L"_eng_" + std::to_wstring(eng) + L"_engtype_" + type;
        };
        Check(GpuInstancePid(inst(1234, L"0x0000E3C5", 0, 0, L"3D")) == 1234 && GpuInstancePid(inst(4, L"0x0000E3C5", 0, 0, L"3D")) == 4, "gpu pid: the process id is read from an engine instance");
        Check(GpuInstancePid(L"PID_77_LUID_0X00000000_0X0000E3C5_PHYS_0_ENG_1_ENGTYPE_COPY") == 77, "gpu pid: letter case does not matter");
        Check(GpuInstancePid(L"luid_0x00000000_0x0000E3C5_phys_0") == -1 && GpuInstancePid(L"pid_x_luid_0x0_0x1_phys_0_eng_0_engtype_3D") == -1 && GpuInstancePid(L"") == -1, "gpu pid: anything that is not an engine instance is -1");
        const std::vector<GpuCounterSample> s = {
            {inst(100, L"0x0000E3C5", 0, 0, L"3D"), 2.0}, {inst(100, L"0x0000E3C5", 0, 0, L"3D"), 1.5},   // two instances of the same engine: summed
            {inst(100, L"0x0000E3C5", 0, 1, L"Copy"), 0.5},
            {inst(10, L"0x0000E3C5", 0, 0, L"3D"), 9.0},                                                   // pid 10 must not be mistaken for pid 100 (or the reverse)
            {inst(200, L"0x0000E3C5", 0, 0, L"3D"), 60.0}, {inst(200, L"0x0000FC53", 0, 0, L"3D"), 80.0},   // another process on two adapters
            {inst(300, L"0x0000E3C5", 0, 2, L"VideoDecode"), 7.0},
        };
        GpuAdapterUtil a = GpuProcessUtil(s, 100);
        Check(a.util == 3.5 && a.util3d == 3.5 && a.engines == 2, "gpu proc: instances of one engine are summed; the figure is the busiest of the process's own engines");
        Check(GpuProcessUtil(s, 10).util == 9.0 && GpuProcessUtil(s, 10).engines == 1, "gpu proc: pid 10 is not pid 100 (exact match)");
        Check(GpuProcessUtil(s, 200).util == 80.0 && GpuProcessUtil(s, 200).engines == 2, "gpu proc: the busiest engine over ALL adapters, other processes excluded");
        a = GpuProcessUtil(s, 300);
        Check(a.util == 7.0 && a.util3d == 0.0, "gpu proc: a non-3D engine counts for the process figure but not for util3d");
        a = GpuProcessUtil(s, 999);
        Check(a.util == 0.0 && a.engines == 0, "gpu proc: a process with no instance is 0 with no engines");
        Check(GpuProcessUtil({}, 100).engines == 0 && GpuProcessUtil({}, 100).util == 0.0, "gpu proc: no samples at all is 0 with no engines");
        {
            const std::vector<GpuCounterSample> hot = {{inst(5, L"0x0000E3C5", 0, 0, L"3D"), 70.0}, {inst(5, L"0x0000E3C5", 0, 0, L"3D"), 70.0}};
            Check(GpuProcessUtil(hot, 5).util == 100.0, "gpu proc: an engine never exceeds 100 %");
            const std::vector<GpuCounterSample> bad = {{inst(5, L"0x0000E3C5", 0, 0, L"3D"), std::numeric_limits<double>::quiet_NaN()}, {inst(5, L"0x0000E3C5", 0, 1, L"3D"), -4.0}, {inst(5, L"0x0000E3C5", 0, 2, L"3D"), 1e9}, {inst(5, L"0x0000E3C5", 0, 3, L"3D"), 2.5}};
            Check(GpuProcessUtil(bad, 5).util == 2.5, "gpu proc: NaN, negative and absurd values count as 0");
            const std::vector<GpuCounterSample> junk = {{L"not an instance", 50.0}, {inst(5, L"0x0000E3C5", 0, 0, L"3D"), 1.0}};
            Check(GpuProcessUtil(junk, 5).util == 1.0, "gpu proc: names that are not engine instances are ignored");
        }
        {   // the same rule as the system-wide one: summing every process of an adapter gives the adapter figure
            const std::vector<GpuAdapterUtil> all = GpuAggregate(s);
            double mx = 0; for (const auto& x : all) mx = std::max(mx, x.util);
            Check(mx == 80.0 && GpuProcessUtil(s, 200).util == mx, "gpu proc: it agrees with the adapter-wide rule when one process is the busiest");
        }
    }
    {   // ---- 2.7 product identity: the display names changed, every technical identifier did not
        Check(std::wstring(kProductFull) == L"AnkhCore Control Center", "naming: the product is AnkhCore Control Center");
        Check(std::wstring(kBrand) == L"AnkhCore" && std::wstring(kProductFull).rfind(kBrand, 0) == 0, "naming: the product name starts with the parent brand");
        Check(std::wstring(kInstallerProduct) == L"AnkhCore Setup" && std::wstring(kSensorsProduct) == L"AnkhCore Sensors" && std::wstring(kAnubisProduct) == L"AnkhCore Anubis", "naming: installer, sensor helper and Anubis names follow 'AnkhCore <function>'");
        Check(MainWindowTitle(false) == L"AnkhCore Control Center" && MainWindowTitle(true) == L"AnkhCore Control Center (test build)", "naming: the window title is the product name, with the test-build suffix only in the test build");
        Check(MainWindowTitle(false).compare(0, 8, L"AnkhCore") == 0 && MainWindowTitle(true).compare(0, 8, L"AnkhCore") == 0, "naming: the window title still starts with 'AnkhCore' (the setup closes a running copy by this prefix)");
        // technical identifiers: renaming any of them breaks upgrades from existing installs (2.7 prompt, section 5)
        Check(std::wstring(kProduct) == L"AnkhCore", "naming: the technical product id is unchanged");
        { const std::wstring exe = AppExe(); Check(exe.size() > 13 && exe.compare(exe.size() - 13, 13, L"\\AnkhCore.exe") == 0, "naming: the executable file name is still AnkhCore.exe"); }
        Check(std::wstring(kTaskName) == L"\\AnkhCore\\Launch" && std::wstring(kStartupTaskName) == L"AnkhCore\\Startup", "naming: the scheduled task identifiers are unchanged");
        Check(std::wstring(kRunOnceValue) == L"AnkhCoreTheme", "naming: the RunOnce value is unchanged");
        Check(UgPrivatePipeName(L"C:\\x", L"u").rfind(L"AnkhCore.UniGetUI.", 0) == 0, "naming: the updater's private pipe prefix is unchanged");
        Check(std::wstring(kVersion) == L"2.11.0.0", "naming: the numeric version the setup compares is 2.10.0 (the 2.10 beta, owner decision 2026-11-19)");
    }
    {   // ---- 2.7 A1: backup category providers (fixtures in a private temp folder; nothing outside it is read or written)
        const std::wstring a1 = std::wstring(tmp) + L"ankhcore_test_a1_" + std::to_wstring(GetCurrentProcessId());
        auto mkRoots = [&](const wchar_t* n) { const std::wstring d = a1 + L"\\" + n; Evict(d, true, false); AcbRoots rt{d + L"\\AppData", d + L"\\Data", d + L"\\Content"}; EnsureDir(rt.appdata); EnsureDir(rt.data); EnsureDir(rt.content); return rt; };
        auto put = [&](const std::wstring& path, const std::string& body) { EnsureDir(DirOf(path)); WriteAtomic(path, reinterpret_cast<const uint8_t*>(body.data()), body.size()); };
        auto text = [&](const std::wstring& path) { std::vector<uint8_t> d; ReadAll(path, d); return std::string(d.begin(), d.end()); };
        auto has = [](const std::vector<std::string>& v, const char* needle) { for (const auto& x : v) if (x.find(needle) != std::string::npos) return true; return false; };
        Evict(a1, true, false);

        // ---- the coverage report: every category the format knows is accounted for, honestly
        {
            const auto cov = AcbCoverage();
            Check(cov.size() == AcbCategories().size(), "a1 coverage: one row per category the format knows");
            auto row = [&](const char* id) -> const AcbCoverageRow* { for (const auto& r : cov) if (r.id == id) return &r; return nullptr; };
            Check(row("settings") && row("settings")->support == AcbSupport::Included && row("monitoring") && row("monitoring")->support == AcbSupport::Included, "a1 coverage: settings and monitoring are included");
            Check(row("cursors") && row("cursors")->support == AcbSupport::Partial, "a1 coverage: cursors are only partly included");
            for (const char* id : {"wallpapers", "history", "apps", "iconpacks", "themes", "windhawk", "tweaks", "appdata"})
                Check(row(id) && row(id)->support == AcbSupport::NotIncluded && row(id)->limitation.size() > 20, std::string("a1 coverage: ") + id + " is 'not included' and says why");
            bool onlyKnown = true; for (const auto& pr : AcbProviders()) if (!AcbFindCat(pr.id)) onlyKnown = false;
            Check(onlyKnown, "a1 coverage: every provider belongs to a category of the format");
            bool reservedNeverIncluded = true; for (const auto& r : cov) if (!AcbSupported(r.id) && r.support != AcbSupport::NotIncluded) reservedNeverIncluded = false;
            Check(reservedNeverIncluded, "a1 coverage: a reserved category the format cannot restore is never reported as included");
            bool providerWithoutCollectorIsHonest = true; for (const auto& pr : AcbProviders()) if (!pr.collect && pr.support != AcbSupport::NotIncluded) providerWithoutCollectorIsHonest = false;
            Check(providerWithoutCollectorIsHonest, "a1 coverage: a provider that collects nothing never claims to include anything");
            Check(row("wallpapers")->limitation.find("Windows folder") != std::string::npos && row("windhawk")->limitation.find("reserved") != std::string::npos && row("appdata")->limitation.find("Q5") != std::string::npos, "a1 coverage: the reasons are specific");
            Check(row("cursors")->limitation.find("recovery points") != std::string::npos, "a1 coverage: the cursor limitation points at the recovery points");
            Check(row("history")->limitation.find("user name") != std::string::npos, "a1 coverage: history says why it is left out (paths with the user name)");
        }

        // ---- collecting cursor files
        {
            const AcbRoots rt = mkRoots(L"cur");
            const std::wstring cd = rt.content + L"\\Cursors";
            put(cd + L"\\a.cur", "AAAA"); put(cd + L"\\Pack One\\b.ANI", "BBBBBB"); put(cd + L"\\Pack One\\readme.txt", "text"); put(cd + L"\\run.exe", "MZ"); put(cd + L"\\empty.cur", "");
            AcbCollectResult r; AcbCollectCursors(rt, r);
            Check(r.items.size() == 2, "a1 cursors: only .cur and .ani files are collected (not .txt or .exe), empty files are skipped");
            Check(r.items.size() == 2 && r.items[0].rel == "a.cur" && r.items[1].rel == "Pack One/b.ANI", "a1 cursors: stable order, subfolders kept, the extension check ignores case");
            Check(r.items.size() == 2 && r.items[0].category == "cursors" && r.items[0].dest == "content:Cursors/a.cur" && r.items[1].dest == "content:Cursors/Pack One/b.ANI", "a1 cursors: the destination is the content root only");
            Check(r.items.size() == 2 && std::string(r.items[1].data.begin(), r.items[1].data.end()) == "BBBBBB", "a1 cursors: the bytes are the file's bytes");
            Check(has(r.notes, "empty file: empty.cur"), "a1 cursors: a skipped empty file is reported");
            AcbCollectResult none; AcbCollectCursors(mkRoots(L"cur_none"), none);
            Check(none.items.empty() && none.notes.empty(), "a1 cursors: no folder gives nothing and no complaint");
            AcbFolderLimits one; one.maxFiles = 1;
            AcbCollectResult lim1; AcbCollectCursors(rt, lim1, one);
            Check(lim1.items.size() == 1 && has(lim1.notes, "file limit"), "a1 cursors: the file-count limit stops collection and says so");
            AcbFolderLimits small; small.maxFileBytes = 4;
            AcbCollectResult lim2; AcbCollectCursors(rt, lim2, small);
            Check(lim2.items.size() == 1 && has(lim2.notes, "too large: Pack One/b.ANI"), "a1 cursors: a file over the size limit is skipped and reported");
            AcbFolderLimits tot; tot.maxTotalBytes = 5;
            AcbCollectResult lim3; AcbCollectCursors(rt, lim3, tot);
            Check(lim3.items.size() == 1 && has(lim3.notes, "size limit"), "a1 cursors: the total-size limit stops collection and says so");
            AcbFolderLimits flat; flat.maxDepth = 0;
            AcbCollectResult lim4; AcbCollectCursors(rt, lim4, flat);
            Check(lim4.items.size() == 1 && has(lim4.notes, "nested too deeply"), "a1 cursors: folders deeper than the limit are skipped and reported");
            // a link is never followed
            const AcbRoots rl = mkRoots(L"cur_link");
            const std::wstring outside = a1 + L"\\cur_link_outside";
            Evict(outside, true, false); put(outside + L"\\secret.cur", "SECRET");
            put(rl.content + L"\\Cursors\\own.cur", "OWN");
            Run(L"cmd.exe", L"/c mklink /J \"" + rl.content + L"\\Cursors\\Link\" \"" + outside + L"\"", true, 20000);
            AcbCollectResult lk; AcbCollectCursors(rl, lk);
            bool leaked = false; for (const auto& it : lk.items) if (it.data.size() == 6 || it.rel.find("secret") != std::string::npos) leaked = true;
            Check(!leaked && lk.items.size() == 1 && lk.items[0].rel == "own.cur", "a1 cursors: a junction inside the folder is not followed and its target's files are not collected");
            Check(has(lk.notes, "skipped a link"), "a1 cursors: the skipped link is reported");
        }

        // ---- the creator: kinds, categories, notes, verification
        {
            const AcbRoots rt = mkRoots(L"full");
            put(rt.appdata + L"\\settings.json", "{\"sidebar.width\":300,\"appearance.theme\":\"As3aD\"}");
            put(rt.data + L"\\net-counters.txt", "counters");
            put(rt.content + L"\\Cursors\\a.cur", "AAAA");
            const std::wstring out = a1 + L"\\full.acbackup";
            AcbCheck chk; std::wstring err;
            Check(AcbCreateBackup(rt, out, "full", {}, chk, err) && chk.ok, "a1 create: a full backup is created and verified");
            std::set<std::string> cats; for (const auto& f : chk.manifest.files) cats.insert(f.category);
            Check(chk.manifest.files.size() == 3 && cats == std::set<std::string>{"settings", "monitoring", "cursors"}, "a1 create: a full backup holds the settings, the monitoring totals and the cursor files, nothing else");
            Check(has(chk.manifest.notes, "cursors (partly included)") && has(chk.manifest.notes, "wallpapers (not included)") && has(chk.manifest.notes, "windhawk (not included)") && has(chk.manifest.notes, "appdata (not included)"), "a1 create: the backup itself says what it does not hold");
            Check(!has(chk.manifest.notes, "settings (") && !has(chk.manifest.notes, "monitoring ("), "a1 create: included categories need no limitation note");

            const std::wstring outS = a1 + L"\\settings.acbackup";
            Check(AcbCreateBackup(rt, outS, "settings", {}, chk, err) && chk.ok && chk.manifest.files.size() == 2 && chk.manifest.notes.empty(), "a1 create: a 'settings' backup is exactly what it was before: settings and monitoring, no notes");
            const std::wstring outC = a1 + L"\\cursors.acbackup";
            Check(AcbCreateBackup(rt, outC, "custom", {"cursors"}, chk, err) && chk.ok && chk.manifest.files.size() == 1 && chk.manifest.files[0].category == "cursors", "a1 create: a custom backup of one category holds only that category");

            const AcbRoots re = mkRoots(L"empty");
            const std::wstring outE = a1 + L"\\none.acbackup";
            AcbCheck chk2; std::wstring e2;
            Check(!AcbCreateBackup(re, outE, "custom", {"cursors"}, chk2, e2) && !e2.empty() && !FileExists(outE), "a1 create: a category with nothing to back up fails with a reason and writes no file");
            Check(!AcbCreateBackup(rt, a1 + L"\\wh.acbackup", "custom", {"windhawk"}, chk2, e2) && !FileExists(a1 + L"\\wh.acbackup"), "a1 create: a category with no provider (Windhawk) is refused, not faked");
            Check(!AcbCreateBackup(rt, a1 + L"\\bad.acbackup", "custom", {"no-such-category"}, chk2, e2) && !FileExists(a1 + L"\\bad.acbackup"), "a1 create: an unknown category is refused");

            // round trip into other roots: nothing but the backed-up files appear
            const AcbRoots rr = mkRoots(L"restore");
            const AcbRestoreReport rep = AcbRestoreBackup(rr, out);
            Check(rep.ok, "a1 restore: the full backup restores into other roots");
            Check(text(rr.content + L"\\Cursors\\a.cur") == "AAAA", "a1 restore: the cursor file is back, byte for byte, under the content root");
            Check(FileExists(rr.appdata + L"\\settings.json") && text(rr.appdata + L"\\settings.json").find("As3aD") != std::string::npos, "a1 restore: the settings came back");
            Check(text(rr.data + L"\\net-counters.txt") == "counters", "a1 restore: the monitoring totals came back");
            Check(!DirExists(rr.content + L"\\Wallpapers") && !DirExists(rr.content + L"\\Windhawk"), "a1 restore: nothing that was not backed up appears");
        }
        Evict(a1, true, false);
    }
    {   // ---- 2.7 C1: the hardware inventory in the support bundle (private temp folder only)
        const std::wstring c1 = std::wstring(tmp) + L"ankhcore_test_c1_" + std::to_wstring(GetCurrentProcessId());
        Evict(c1, true, false); EnsureDir(c1);
        auto entryText = [&](const std::vector<uint8_t>& zipRaw, const std::vector<ZipEntry>& entries, const char* name, std::string& out) {
            for (const ZipEntry& e : entries) if (e.name == name) { std::vector<uint8_t> d; std::wstring er; if (!ZipRead(zipRaw.data(), zipRaw.size(), e, d, er)) return false; out.assign(d.begin(), d.end()); return true; }
            return false;
        };
        HwInputs in;
        in.os = L"Windows 11 Pro 10.0.26300"; in.sysManufacturer = L"Acme Inc."; in.sysProduct = L"Laptop X1"; in.boardManufacturer = L"Acme Inc."; in.boardProduct = L"X1-BOARD";
        in.biosVersion = L"1.2.3"; in.cpuName = L"Test CPU"; in.physCores = 4; in.logicalCores = 8; in.cpuLoadWindows = 10; in.ramTotal = 8ull << 30; in.ramLoadWindows = 30;
        in.helperInstalled = true; in.readings.present = true; in.readings.cpuTemp = 50;
        FanReading f; f.name = L"CPU Fan"; f.rpm = 2000; f.capability = FanCapability::Controllable; f.controllable = true; in.readings.fans = {f};
        const HwInventory inv = BuildHwInventory(in);
        const std::string txt = WToUtf8(HwInventoryText(inv)), json = HwInventoryJson(inv);

        AcbRoots roots; roots.data = c1 + L"\\Data"; roots.appdata = c1 + L"\\AppData"; EnsureDir(roots.data); EnsureDir(roots.appdata);
        const std::wstring zp = c1 + L"\\bundle.zip"; std::wstring err;
        const std::vector<std::pair<std::string, std::string>> extra = {{"hardware-inventory.txt", txt}, {"hardware-inventory.json", json}};
        Check(AcbExportSupportBundle(roots, zp, "summary\r\n", err, extra), "c1 bundle: the export with the hardware inventory succeeds");
        std::vector<uint8_t> raw; std::vector<ZipEntry> entries; std::wstring le;
        Check(ReadAll(zp, raw) && ZipList(raw.data(), raw.size(), entries, le), "c1 bundle: the bundle is a valid ZIP");
        std::string gotTxt, gotJson, gotSum;
        Check(entryText(raw, entries, "hardware-inventory.txt", gotTxt) && gotTxt == txt, "c1 bundle: hardware-inventory.txt is in the bundle, byte for byte");
        Check(entryText(raw, entries, "hardware-inventory.json", gotJson) && gotJson == json, "c1 bundle: hardware-inventory.json is in the bundle, byte for byte");
        JVal jr; Check(JsonParse(gotJson, jr) && jr.Is(JVal::Obj) && jr.Get("schema") && jr.Get("schema")->s == "ankhcore.hardware-inventory", "c1 bundle: the JSON in the bundle is the versioned inventory schema");
        Check(entryText(raw, entries, "system-summary.txt", gotSum) && gotSum == "summary\r\n", "c1 bundle: the existing summary is unchanged");

        // the inventory states limits honestly and holds nothing personal
        Check(gotTxt.find("Fan control offered: no") != std::string::npos && gotTxt.find("control reported, not verified") != std::string::npos, "c1 text: control is reported as not offered and not verified");
        Check(gotTxt.find("== Limitations ==") != std::string::npos && gotTxt.find("sensor helper") != std::string::npos, "c1 text: sources and limitations are listed");
        wchar_t cn[MAX_COMPUTERNAME_LENGTH + 2] = L""; DWORD cnl = static_cast<DWORD>(std::size(cn)); GetComputerNameW(cn, &cnl);
        wchar_t un[260] = L""; DWORD unl = static_cast<DWORD>(std::size(un)); GetUserNameW(un, &unl);
        const std::string both = gotTxt + gotJson;
        Check((cnl < 3 || both.find(WToUtf8(cn)) == std::string::npos) && (unl < 3 || both.find(WToUtf8(un)) == std::string::npos), "c1 text: neither this machine's name nor the user name appears in the hardware files");
        Check(both.find("erial") == std::string::npos && both.find("UUID") == std::string::npos && both.find("uuid") == std::string::npos, "c1 text: no serial number or UUID appears");
        {   // a PC with no helper: the unavailable state is stated, not hidden
            HwInputs none = in; none.helperInstalled = false; none.readings = Readings{};
            const std::string t2 = WToUtf8(HwInventoryText(BuildHwInventory(none)));
            Check(t2.find("helper not installed") != std::string::npos && t2.find("No fan reported.") != std::string::npos, "c1 text: with no helper installed the bundle says so");
        }

        // the extra-file rules
        {
            const std::vector<std::pair<std::string, std::string>> bad = {
                {"../evil.txt", "x"}, {"a/b.txt", "x"}, {"a\\b.txt", "x"}, {"", "x"}, {".hidden", "x"}, {"has space.txt", "x"}, {"empty.txt", ""},
                {std::string(65, 'a'), "x"}, {"big.txt", std::string((512u << 10) + 1, 'x')}, {"system-summary.txt", "OVERRIDE"}, {"ok.txt", "fine"}, {"ok.txt", "second"}};
            const std::wstring zp2 = c1 + L"\\bad.zip";
            Check(AcbExportSupportBundle(roots, zp2, "summary\r\n", err, bad), "c1 rules: a bundle with unusable extras still exports");
            std::vector<uint8_t> r2; std::vector<ZipEntry> e2; std::wstring l2;
            Check(ReadAll(zp2, r2) && ZipList(r2.data(), r2.size(), e2, l2), "c1 rules: ... and is a valid ZIP");
            std::set<std::string> names; for (const auto& e : e2) names.insert(e.name);
            Check(names == std::set<std::string>{"system-summary.txt", "ok.txt"}, "c1 rules: path-like, hidden, spaced, empty, over-long, oversized and duplicate-of-summary names are all skipped");
            std::string okText, sum;
            Check(entryText(r2, e2, "ok.txt", okText) && okText == "fine", "c1 rules: of two extras with one name the first wins");
            Check(entryText(r2, e2, "system-summary.txt", sum) && sum == "summary\r\n", "c1 rules: an extra cannot replace the summary");
        }
        {   // no extras: exactly the earlier behavior
            const std::wstring zp3 = c1 + L"\\none.zip";
            Check(AcbExportSupportBundle(roots, zp3, "summary\r\n", err), "c1 rules: without extras the export is the same as before");
            std::vector<uint8_t> r3; std::vector<ZipEntry> e3; std::wstring l3;
            Check(ReadAll(zp3, r3) && ZipList(r3.data(), r3.size(), e3, l3) && e3.size() == 1 && e3[0].name == "system-summary.txt", "c1 rules: ... and holds only the summary when no log or settings exist");
        }
        Evict(c1, true, false);
    }
    {   // ---- 2.7 F1: settings migration runner and Settings::Load (private temp files only)
        auto vals = [](std::initializer_list<std::pair<const wchar_t*, const wchar_t*>> l) { SettingsValues v; for (const auto& kv : l) v[kv.first] = kv.second; return v; };
        const SettingsMigrationHooks noHooks;
        auto okBackup = [](std::vector<std::wstring>* log) { SettingsMigrationHooks h; h.backup = [log](int from, int to, std::wstring& where) { if (log) log->push_back(L"backup " + std::to_wstring(from) + L"->" + std::to_wstring(to)); where = L"C:\\bak\\settings.v" + std::to_wstring(from); return true; }; return h; };

        // ---- the real steps
        {
            SettingsValues v = vals({{L"sidebar.width", L"300"}, {L"custom.future", L"\"x\""}, {L"weird", L"null"}});
            const SettingsValues orig = v;
            SettingsMigrationReport r = SettingsMigrate(v, kSettingsVersion, SettingsMigrationSteps(), okBackup(nullptr));
            SettingsValues expect = orig; expect[L"settings.version"] = std::to_wstring(kSettingsVersion);
            Check(r.outcome == SettingsMigrationOutcome::Applied && r.fromVersion == 0 && r.toVersion == kSettingsVersion && r.stepsApplied == std::vector<std::wstring>{L"stamp-version"}, "f1 real: a file with no version is migrated 0 -> current by the stamp step");
            Check(v == expect, "f1 real: every other setting, known or unknown, is exactly as it was; only the version was added");
            Check(r.backupMade && r.backupPath == L"C:\\bak\\settings.v0", "f1 real: a backup was requested first and its place is in the report");
            Check(!r.notes.empty() && r.notes[0].find(L"no settings.version") != std::wstring::npos, "f1 real: the missing version is recorded as the reason");
        }
        {
            SettingsValues v = vals({{L"settings.version", L"1"}, {L"a", L"1"}});
            const SettingsValues orig = v; std::vector<std::wstring> log;
            const SettingsMigrationReport r = SettingsMigrate(v, 1, SettingsMigrationSteps(), okBackup(&log));
            Check(r.outcome == SettingsMigrationOutcome::NotNeeded && v == orig && log.empty() && r.notes.empty(), "f1 real: a current file needs nothing: unchanged, no backup, no note");
        }
        {
            SettingsValues v = vals({{L"settings.version", L"2"}, {L"a", L"1"}, {L"new.future.key", L"\"kept\""}});
            const SettingsValues orig = v; std::vector<std::wstring> log;
            const SettingsMigrationReport r = SettingsMigrate(v, 1, SettingsMigrationSteps(), okBackup(&log));
            Check(r.outcome == SettingsMigrationOutcome::RefusedNewer && v == orig && log.empty(), "f1 newer: a file from a newer version is left exactly as it is: not relabelled, not downgraded, no backup");
            Check(!r.notes.empty() && r.notes[0].find(L"newer version") != std::wstring::npos, "f1 newer: the reason is recorded");
        }
        for (const wchar_t* bad : {L"\"1\"", L"abc", L"-1", L"1.5", L"99999999", L"", L"null", L"true"}) {
            SettingsValues v = vals({{L"settings.version", bad}, {L"keep.me", L"42"}});
            std::vector<std::wstring> log;
            const SettingsMigrationReport r = SettingsMigrate(v, 1, SettingsMigrationSteps(), okBackup(&log));
            const std::string nm = std::string("f1 invalid version '") + WToUtf8(bad) + "'";
            Check(r.outcome == SettingsMigrationOutcome::Applied && v[L"settings.version"] == L"1" && v[L"keep.me"] == L"42", nm + ": replaced by the current version, other settings kept");
            Check(r.fromVersion == 0 && log.size() == 1 && !r.notes.empty() && r.notes[0].find(L"not a plain whole number") != std::wstring::npos, nm + ": a backup was requested and the original literal is recorded in the notes");
        }

        // ---- the framework with test steps
        const SettingsMigrationStep rename{1, 2, "rename-old-key", false, {L"old.name", L"new.name"}, [](SettingsValues& v, std::wstring&) { auto it = v.find(L"old.name"); if (it != v.end()) { v[L"new.name"] = it->second; v.erase(L"old.name"); } return true; }};
        const SettingsMigrationStep addDefault{2, 3, "add-default", false, {L"added."}, [](SettingsValues& v, std::wstring&) { v[L"added.flag"] = L"true"; return true; }};
        {
            SettingsValues v = vals({{L"settings.version", L"1"}, {L"old.name", L"\"x\""}, {L"other", L"7"}});
            std::vector<std::wstring> log;
            const SettingsMigrationReport r = SettingsMigrate(v, 2, {rename}, okBackup(&log));
            Check(r.outcome == SettingsMigrationOutcome::Applied && v == vals({{L"settings.version", L"2"}, {L"new.name", L"\"x\""}, {L"other", L"7"}}), "f1 steps: a rename moves the value, removes the old key, keeps other settings, stamps the version");
            Check(log == std::vector<std::wstring>{L"backup 1->2"}, "f1 steps: the backup is requested exactly once, before the step");
        }
        {
            SettingsValues v = vals({{L"settings.version", L"1"}, {L"old.name", L"1"}, {L"z", L"z"}});
            const SettingsMigrationReport r = SettingsMigrate(v, 3, {addDefault, rename}, noHooks);   // given out of order: the chain is found by version
            Check(r.outcome == SettingsMigrationOutcome::Applied && r.stepsApplied == std::vector<std::wstring>{L"rename-old-key", L"add-default"} && v[L"settings.version"] == L"3" && v[L"added.flag"] == L"true" && v[L"new.name"] == L"1" && v[L"z"] == L"z" && !v.count(L"old.name"), "f1 steps: a chain 1 -> 2 -> 3 runs in version order whatever the list order");
        }
        {   // failures leave the original untouched
            SettingsValues v = vals({{L"settings.version", L"1"}, {L"a", L"1"}});
            const SettingsValues orig = v;
            SettingsMigrationReport r = SettingsMigrate(v, 3, {rename}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v == orig && r.error.find(L"from settings version 2") != std::wstring::npos, "f1 fail: a missing step fails with the version named and the settings untouched");
            const SettingsMigrationStep boom{1, 2, "boom", false, {L"a"}, [](SettingsValues& v2, std::wstring& why) { v2[L"a"] = L"999"; why = L"disk on fire"; return false; }};
            r = SettingsMigrate(v, 2, {boom}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v == orig && r.error.find(L"boom") != std::wstring::npos && r.error.find(L"disk on fire") != std::wstring::npos, "f1 fail: a step that fails (after changing its copy) leaves the original untouched and says why");
            const SettingsMigrationStep greedy{1, 2, "greedy", false, {L"a"}, [](SettingsValues& v2, std::wstring&) { v2.erase(L"user.precious"); return true; }};
            SettingsValues v3 = vals({{L"settings.version", L"1"}, {L"user.precious", L"1"}}); const SettingsValues o3 = v3;
            r = SettingsMigrate(v3, 2, {greedy}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v3 == o3 && r.error.find(L"user.precious") != std::wstring::npos, "f1 fail: a step that drops a setting it did not declare is refused and names the setting");
            const SettingsMigrationStep rewrite{1, 2, "rewrite", false, {L"a"}, [](SettingsValues& v2, std::wstring&) { v2[L"b"] = L"changed"; return true; }};
            SettingsValues v4 = vals({{L"settings.version", L"1"}, {L"b", L"orig"}}); const SettingsValues o4 = v4;
            r = SettingsMigrate(v4, 2, {rewrite}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v4 == o4 && r.error.find(L"changed the setting \"b\"") != std::wstring::npos, "f1 fail: a step that rewrites an undeclared setting is refused");
            const SettingsMigrationStep adds{1, 2, "adds", false, {L"a"}, [](SettingsValues& v2, std::wstring&) { v2[L"sneaky"] = L"1"; return true; }};
            SettingsValues v5 = vals({{L"settings.version", L"1"}}); const SettingsValues o5 = v5;
            r = SettingsMigrate(v5, 2, {adds}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v5 == o5 && r.error.find(L"added the setting \"sneaky\"") != std::wstring::npos, "f1 fail: a step that adds an undeclared setting is refused");
            const SettingsMigrationStep ver{1, 2, "ver", false, {}, [](SettingsValues& v2, std::wstring&) { v2[L"settings.version"] = L"77"; return true; }};
            SettingsValues v6 = vals({{L"settings.version", L"1"}}); const SettingsValues o6 = v6;
            r = SettingsMigrate(v6, 2, {ver}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v6 == o6, "f1 fail: a step cannot set the version key itself; the runner owns it");
        }
        {   // backups and high-impact steps
            SettingsMigrationStep hi = rename; hi.highImpact = true; hi.id = "hi";
            SettingsValues v = vals({{L"settings.version", L"1"}, {L"old.name", L"1"}}); const SettingsValues orig = v;
            SettingsMigrationHooks failing; failing.backup = [](int, int, std::wstring&) { return false; };
            SettingsMigrationReport r = SettingsMigrate(v, 2, {hi}, failing);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v == orig && r.error.find(L"requires one") != std::wstring::npos, "f1 backup: a high-impact step is refused when the backup cannot be made; nothing changes");
            r = SettingsMigrate(v, 2, {hi}, noHooks);
            Check(r.outcome == SettingsMigrationOutcome::Failed && v == orig, "f1 backup: a high-impact step is refused when no backup is possible at all");
            r = SettingsMigrate(v, 2, {rename}, failing);
            Check(r.outcome == SettingsMigrationOutcome::Applied && !r.backupMade && !r.notes.empty() && r.notes.back().find(L"backup") != std::wstring::npos, "f1 backup: a low-impact step still runs when the backup fails, and the report says there is none");
            SettingsValues v2 = orig;
            r = SettingsMigrate(v2, 2, {hi}, okBackup(nullptr));
            Check(r.outcome == SettingsMigrationOutcome::Applied && r.backupMade, "f1 backup: with a backup a high-impact step runs");
            SettingsMigrationHooks nothing; nothing.backup = [](int, int, std::wstring& where) { where.clear(); return true; };
            SettingsValues v3 = orig; r = SettingsMigrate(v3, 2, {hi}, nothing);
            Check(r.outcome == SettingsMigrationOutcome::Applied && !r.backupMade, "f1 backup: 'nothing to copy' counts as a successful backup step but is not reported as a backup");
        }

        // ---- Settings::Load with real files (a private path; the application's own file is never touched)
        {
            const std::wstring f1 = std::wstring(tmp) + L"ankhcore_test_f1_" + std::to_wstring(GetCurrentProcessId());
            Evict(f1, true, false); EnsureDir(f1);
            auto write = [&](const std::wstring& path, const std::string& body) { WriteAtomic(path, reinterpret_cast<const uint8_t*>(body.data()), body.size()); };
            auto read = [&](const std::wstring& path) { std::vector<uint8_t> d; ReadAll(path, d); return std::string(d.begin(), d.end()); };

            {   // 2.5-style file without a version
                const std::wstring path = f1 + L"\\a.json"; const std::string body = "{\r\n  \"sidebar.width\": 300,\r\n  \"custom.future\": \"x\"\r\n}\r\n";
                write(path, body);
                Settings s; s.SetPathForTest(path); s.Load();
                const SettingsMigrationReport r = s.LastMigration();
                Check(r.outcome == SettingsMigrationOutcome::Applied && s.Int(L"sidebar.width", 0, 0, 1000) == 300 && s.Str(L"custom.future", L"") == L"x" && s.Int(L"settings.version", 0, 0, 100) == kSettingsVersion, "f1 load: a file with no version is migrated and every setting is kept");
                Check(r.backupMade && read(path + L".pre-migration-v0") == body, "f1 load: a byte-identical copy of the original file was kept first");
                Check(s.Save() && read(path).find("\"settings.version\": 1") != std::string::npos && read(path).find("\"custom.future\": \"x\"") != std::string::npos, "f1 load: the saved file carries the version and the unknown key");
                Check(read(path + L".pre-migration-v0") == body, "f1 load: the copy is not touched by the save");
            }
            {   // a current file
                const std::wstring path = f1 + L"\\b.json"; write(path, "{\"settings.version\": 1, \"a\": 5}");
                Settings s; s.SetPathForTest(path); s.Load();
                Check(s.LastMigration().outcome == SettingsMigrationOutcome::NotNeeded && !FileExists(path + L".pre-migration-v0") && s.Int(L"a", 0, 0, 10) == 5, "f1 load: a current file is loaded as before: no migration, no copy");
            }
            {   // a newer file is never downgraded, not even by a save
                const std::wstring path = f1 + L"\\c.json"; write(path, "{\"settings.version\": 2, \"a\": 5, \"future.thing\": \"y\"}");
                Settings s; s.SetPathForTest(path); s.Load();
                Check(s.LastMigration().outcome == SettingsMigrationOutcome::RefusedNewer && s.Int(L"settings.version", 0, 0, 100) == 2 && s.Int(L"a", 0, 0, 10) == 5, "f1 load: a newer file keeps its version and its values");
                s.SetInt(L"a", 6); Check(s.Save() && read(path).find("\"settings.version\": 2") != std::string::npos && read(path).find("\"future.thing\": \"y\"") != std::string::npos && read(path).find("\"a\": 6") != std::string::npos, "f1 load: saving a newer file keeps its version and its unknown keys (no downgrade)");
                Check(!FileExists(path + L".pre-migration-v2"), "f1 load: no copy is made for a file that is not migrated");
            }
            {   // a damaged file: kept as .damaged (as before); the version is stamped and the reason recorded
                const std::wstring path = f1 + L"\\d.json"; write(path, "{ not json");
                Settings s; s.SetPathForTest(path); s.Load();
                Check(FileExists(path + L".damaged") && read(path + L".damaged") == "{ not json" && s.LastMigration().outcome == SettingsMigrationOutcome::Applied && s.Int(L"settings.version", 0, 0, 100) == kSettingsVersion, "f1 load: a damaged file is still kept as .damaged and the defaults start at the current version");
                Check(s.Count() == 1, "f1 load: ... with nothing but the version in it");
            }
            {   // a garbage version literal in an otherwise good file
                const std::wstring path = f1 + L"\\e.json"; const std::string body = "{\"settings.version\": \"seven\", \"keep\": 9}";
                write(path, body);
                Settings s; s.SetPathForTest(path); s.Load();
                const SettingsMigrationReport r = s.LastMigration();
                Check(r.outcome == SettingsMigrationOutcome::Applied && s.Int(L"keep", 0, 0, 100) == 9 && s.Int(L"settings.version", 0, 0, 100) == kSettingsVersion && read(path + L".pre-migration-v0") == body, "f1 load: an unreadable version is repaired with the original file kept and the other settings intact");
                Check(!r.notes.empty() && r.notes[0].find(L"seven") != std::wstring::npos, "f1 load: the original literal is in the report");
            }
            Evict(f1, true, false);
        }
    }
    {   // ---- 2.7 E1: a restore (the second writer of settings.json) never relabels a newer file as ours
        auto ver = [](const std::string& live, const std::string& incoming) {
            std::string out; int a = 0, sk = 0; std::wstring er;
            const bool ok = AcbMergeSettings(live, incoming, {}, out, a, sk, er);
            JVal j; if (!ok || !JsonParse(out, j) || !j.Is(JVal::Obj) || !j.Get("settings.version")) return -1;
            return static_cast<int>(j.Get("settings.version")->n);
        };
        const std::string inc = "{\"settings.version\": 1, \"sidebar.width\": 200}";
        Check(ver("{\"settings.version\": 2, \"a\": 1}", inc) == 2, "e1 merge: restoring into a file written by a newer version keeps that version (no downgrade)");
        Check(ver("{\"settings.version\": 1, \"a\": 1}", inc) == kSettingsVersion, "e1 merge: a current file stays at the current version");
        Check(ver("{\"a\": 1}", inc) == kSettingsVersion && ver("", inc) == kSettingsVersion, "e1 merge: a file with no version, or no file, gets the current version");
        Check(ver("{\"settings.version\": \"x\", \"a\": 1}", inc) == kSettingsVersion, "e1 merge: an unreadable live version is replaced by the current one");
        Check(ver("{\"settings.version\": 2}", "{\"settings.version\": 99}") == 2, "e1 merge: the incoming file's own version never wins");
        std::string o2; int a2 = 0, s2 = 0; std::wstring e2;
        Check(AcbMergeSettings("{\"settings.version\": 2, \"keep.me\": 5}", inc, {}, o2, a2, s2, e2) && o2.find("\"keep.me\": 5") != std::string::npos && o2.find("\"sidebar.width\": 200") != std::string::npos, "e1 merge: the existing settings are kept and the chosen ones applied as before");
    }
    {   // ---- 2.7 K1: post-uninstall remnants classifier (a fake machine; the live probes are checked on a private temp folder)
        struct FakeFs {
            struct N { bool dir = false, reparse = false; uint64_t size = 0; std::wstring target; };
            std::map<std::wstring, N> nodes; int listCalls = 0; std::vector<std::wstring> listed;
            static std::wstring K(const std::wstring& p) { std::wstring o; for (wchar_t c : p) o += c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c)); while (o.size() > 3 && o.back() == L'\\') o.pop_back(); return o; }
            void Dir(const std::wstring& p, bool reparse = false) { N n; n.dir = true; n.reparse = reparse; nodes[K(p)] = n; }
            void File(const std::wstring& p, uint64_t sz = 10) { N n; n.size = sz; nodes[K(p)] = n; }
            void Lnk(const std::wstring& p, const std::wstring& target) { N n; n.target = target; nodes[K(p)] = n; }
            RemFs Fs() {
                RemFs f;
                f.exists = [this](const std::wstring& p) { return nodes.count(K(p)) != 0; };
                f.isDir = [this](const std::wstring& p) { auto i = nodes.find(K(p)); return i != nodes.end() && i->second.dir; };
                f.isReparse = [this](const std::wstring& p) { auto i = nodes.find(K(p)); return i != nodes.end() && i->second.reparse; };
                f.list = [this](const std::wstring& d) { ++listCalls; listed.push_back(K(d)); std::vector<RemDirEntry> v; const std::wstring dk = K(d) + L"\\";
                    for (const auto& kv : nodes) if (kv.first.size() > dk.size() && kv.first.compare(0, dk.size(), dk) == 0 && kv.first.find(L'\\', dk.size()) == std::wstring::npos) { RemDirEntry e; e.name = kv.first.substr(dk.size()); e.dir = kv.second.dir; e.reparse = kv.second.reparse; e.size = kv.second.size; v.push_back(e); }
                    return v; };
                f.shortcutTarget = [this](const std::wstring& l, std::wstring& t) { auto i = nodes.find(K(l)); if (i == nodes.end() || i->second.target.empty()) return false; t = i->second.target; return true; };
                return f;
            }
        };
        struct FakeReg { std::set<std::wstring> keys; RemReg R() { RemReg r; r.keyExists = [this](const std::wstring& k) { return keys.count(remdetail::Norm(k)) != 0; }; r.str = [](const std::wstring&, const std::wstring&) { return std::wstring(); }; return r; } void Add(const std::wstring& k) { keys.insert(remdetail::Norm(k)); } };
        RemEnv env;
        env.windows = L"C:\\Windows"; env.programFiles = L"C:\\Program Files"; env.programFilesX86 = L"C:\\Program Files (x86)"; env.programData = L"C:\\ProgramData";
        env.appData = L"C:\\Users\\u\\AppData\\Roaming"; env.localAppData = L"C:\\Users\\u\\AppData\\Local"; env.userProfile = L"C:\\Users\\u"; env.temp = L"C:\\Users\\u\\AppData\\Local\\Temp";
        env.startMenuCommon = L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"; env.startMenuUser = L"C:\\Users\\u\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs";
        env.personalFolders = {L"C:\\Users\\u\\Documents", L"C:\\Users\\u\\Desktop", L"C:\\Users\\u\\Downloads", L"C:\\Users\\u\\Pictures"};
        RemApp app; app.name = L"Fabrikam Notes"; app.publisher = L"Fabrikam Inc"; app.installLocation = L"C:\\Program Files\\Fabrikam Notes"; app.exeNames = {L"fabnotes"}; app.uninstalledOk = true;
        app.uninstallKey = L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FabrikamNotes";
        auto find = [](const std::vector<RemCandidate>& v, const std::wstring& path) -> const RemCandidate* { for (const auto& c : v) if (remdetail::Norm(c.path) == remdetail::Norm(path)) return &c; return nullptr; };
        auto scan = [&](FakeFs& f, FakeReg& g, const RemApp& a, const RemEnv& e = RemEnv()) { return RemScan(a, e.windows.empty() ? env : e, f.Fs(), g.R()); };

        // ---- the precondition and the exact-evidence rule
        {
            FakeFs f; FakeReg g; f.Dir(app.installLocation);
            RemApp notYet = app; notYet.uninstalledOk = false;
            Check(scan(f, g, notYet).empty(), "k1 scan: nothing is scanned unless the uninstall was verified to have succeeded");
            RemApp noName = app; noName.name.clear();
            Check(scan(f, g, noName).empty(), "k1 scan: an application with no name is not scanned");
            FakeFs f2; Check(scan(f2, g, app).empty(), "k1 scan: nothing left behind, nothing reported");
        }
        {   // classes of the install folder
            FakeFs f; FakeReg g; f.Dir(app.installLocation);
            auto c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Safe && c[0].origin == L"InstallLocation" && c[0].confidence >= 90 && c[0].reason.find(L"empty") != std::wstring::npos, "k1 folder: an empty install folder is Safe, from the exact InstallLocation, with high confidence");
            f.Dir(app.installLocation + L"\\logs"); f.File(app.installLocation + L"\\logs\\a.LOG"); f.File(app.installLocation + L"\\x.tmp"); f.File(app.installLocation + L"\\y.cache");
            c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Safe && c[0].reason.find(L"log, temporary or cache") != std::wstring::npos && c[0].bytes == 30, "k1 folder: only log, temp and cache files (case-insensitive) is Safe");
            f.File(app.installLocation + L"\\config.ini");
            c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Confirm && c[0].reason.find(L"install folder") != std::wstring::npos, "k1 folder: any other file makes it need confirmation");
            f.Dir(app.installLocation + L"\\Dir"); f.File(app.installLocation + L"\\Dir\\report.DOCX");
            c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Protected && c[0].reason.find(L"yours") != std::wstring::npos, "k1 folder: a document inside makes it Protected");
        }
        for (const wchar_t* personal : {L"x.jpg", L"x.pdf", L"x.mp4", L"x.zip", L"x.kdbx", L"x.pfx", L"x.sav", L"x.psd"}) {
            FakeFs f; FakeReg g; f.Dir(app.installLocation); f.File(app.installLocation + L"\\" + personal);
            const auto c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Protected, std::string("k1 folder: a file like ") + WToUtf8(personal) + " makes the folder Protected");
        }
        {   // a settings database is the application's own data, not a document
            FakeFs f; FakeReg g; f.Dir(app.installLocation); f.File(app.installLocation + L"\\state.sqlite"); f.File(app.installLocation + L"\\readme.txt");
            const auto c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Confirm, "k1 folder: .sqlite and .txt are the application's own files (Confirm), not personal documents");
        }
        {   // too big to inspect: never Safe
            FakeFs f; FakeReg g; f.Dir(app.installLocation); for (int i = 0; i < 12; ++i) f.File(app.installLocation + L"\\f" + std::to_wstring(i) + L".log");
            RemLimits lim; lim.maxEntries = 5;
            const auto c = RemScan(app, env, f.Fs(), g.R(), lim);
            Check(c.size() == 1 && c[0].cls == RemClass::Confirm && !c[0].fullyInspected && c[0].reason.find(L"not inspected completely") != std::wstring::npos, "k1 folder: a folder that cannot be inspected completely is never Safe");
        }

        // ---- names: exact only, never similar
        {
            FakeFs f; FakeReg g; f.Dir(L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam Notes"); f.File(L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam Notes\\settings.json");
            f.Dir(L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam Notes Pro"); f.Dir(L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam"); f.Dir(L"C:\\Users\\u\\AppData\\Roaming\\Notes");
            f.Dir(L"C:\\Users\\u\\AppData\\Local\\fabnotes"); f.File(L"C:\\Users\\u\\AppData\\Local\\fabnotes\\cache.bin");
            const auto c = scan(f, g, app);
            const RemCandidate* ex = find(c, L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam Notes");
            Check(ex && ex->cls == RemClass::Confirm && ex->origin.find(L"named exactly") != std::wstring::npos && ex->confidence == 70, "k1 names: a folder named exactly like the application is a Confirm candidate with its evidence");
            Check(!find(c, L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam Notes Pro") && !find(c, L"C:\\Users\\u\\AppData\\Roaming\\Notes"), "k1 names: a similar or partial name never creates a candidate");
            const RemCandidate* ex2 = find(c, L"C:\\Users\\u\\AppData\\Local\\fabnotes");
            Check(ex2 && ex2->cls == RemClass::Confirm, "k1 names: the executable's name counts as exact evidence too");
            const RemCandidate* pub = find(c, L"C:\\Users\\u\\AppData\\Roaming\\Fabrikam");
            Check(!pub, "k1 names: a folder that only matches the first word (or the publisher's short name) is not a candidate");
        }
        {   // the publisher folder is reported, but only as Protected
            FakeFs f; FakeReg g; f.Dir(L"C:\\ProgramData\\Fabrikam Inc"); f.File(L"C:\\ProgramData\\Fabrikam Inc\\other-product.cfg");
            const auto c = scan(f, g, app);
            const RemCandidate* p = find(c, L"C:\\ProgramData\\Fabrikam Inc");
            Check(p && p->cls == RemClass::Protected && p->reason.find(L"publisher") != std::wstring::npos && p->confidence <= 40, "k1 names: the publisher's folder is shown as Protected (it can hold other products)");
            Check(!RemMayRemove(*p, true).allowed, "k1 names: ... and cannot be removed even when confirmed");
        }
        {   // a name that is not a plain folder name is never turned into a path
            FakeFs f; FakeReg g; RemApp odd = app; odd.name = L"..\\Windows"; odd.exeNames = {L"a/b", L"..", L"x:y"}; odd.installLocation.clear();
            f.Dir(L"C:\\Users\\u\\AppData\\Roaming\\..\\Windows");
            Check(scan(f, g, odd).empty(), "k1 names: an application or executable name with separators or dots builds no path");
        }

        // ---- protection rules, whatever the uninstall entry says
        {
            struct Case { const char* name; const wchar_t* path; const wchar_t* why; };
            const Case cases[] = {
                {"a drive root", L"C:\\", L"drive root"}, {"Program Files itself", L"C:\\Program Files", L"system or profile"}, {"the Windows folder", L"C:\\Windows", L"system or profile"},
                {"System32", L"C:\\Windows\\System32\\Fab", L"Windows folder"}, {"AppData itself", L"C:\\Users\\u\\AppData\\Roaming", L"system or profile"},
                {"the user profile", L"C:\\Users\\u", L"system or profile"}, {"a parent of Program Files", L"C:\\", L"drive root"}, {"Documents", L"C:\\Users\\u\\Documents", L"personal"},
                {"a folder inside Documents", L"C:\\Users\\u\\Documents\\Fab", L"personal"}, {"Downloads", L"C:\\Users\\u\\Downloads\\x", L"personal"},
                {"a folder directly in the profile", L"C:\\Users\\u\\Fab", L"user profile"}, {"a relative path", L"Fab\\Notes", L"not a plain absolute"},
                {"a path with ..", L"C:\\Program Files\\Fab\\..\\..\\Windows", L"not a plain absolute"}, {"a device path", L"\\\\?\\C:\\Fab", L"not a plain absolute"},
                {"a shared system folder name", L"C:\\Program Files\\Microsoft", L"shared"}, {"Common Files", L"C:\\Program Files\\Common Files", L"shared"}, {"an empty path", L"", L"not a plain absolute"}};
            for (const Case& cs : cases) {
                FakeFs f; FakeReg g; f.Dir(cs.path); RemApp a = app; a.installLocation = cs.path;
                const auto c = scan(f, g, a);
                const RemCandidate* hit = nullptr; for (const auto& x : c) if (remdetail::Norm(x.path) == remdetail::Norm(cs.path) || x.origin == L"InstallLocation") hit = &x;
                if (std::wstring(cs.path).empty()) { Check(c.empty() || c[0].cls == RemClass::Protected, std::string("k1 protected: ") + cs.name); continue; }
                Check(hit && hit->cls == RemClass::Protected && hit->reason.find(cs.why) != std::wstring::npos, std::string("k1 protected: ") + cs.name + " (" + (hit ? WToUtf8(hit->reason) : std::string("no candidate")) + ")");
            }
        }
        {   // links are never followed
            FakeFs f; FakeReg g; f.Dir(app.installLocation, true); f.File(app.installLocation + L"\\a.log");
            const auto c = scan(f, g, app);
            Check(c.size() == 1 && c[0].cls == RemClass::Protected && c[0].reason.find(L"link") != std::wstring::npos && std::find(f.listed.begin(), f.listed.end(), FakeFs::K(app.installLocation)) == f.listed.end(), "k1 links: an install folder that is a junction is Protected and the junction itself is never listed");
            FakeFs f2; f2.Dir(L"C:\\Program Files", false); f2.Dir(L"C:\\Program Files\\Fabrikam Notes"); f2.Dir(L"C:\\Program Files\\Fabrikam Notes\\inner", true);
            const auto c2 = scan(f2, g, app);
            Check(c2.size() == 1 && c2[0].cls == RemClass::Protected && c2[0].reason.find(L"link") != std::wstring::npos, "k1 links: a link inside the folder makes it Protected");
            FakeFs f3; f3.Dir(L"C:\\Program Files\\Fabrikam Notes"); FakeFs f3b; f3b.Dir(L"C:\\Program Files\\Fabrikam Notes\\sub"); f3b.Dir(L"C:\\Program Files\\Fabrikam Notes");
            RemApp deep = app; deep.installLocation = L"C:\\Program Files\\Fabrikam Notes\\sub"; f3b.nodes[FakeFs::K(L"C:\\Program Files\\Fabrikam Notes")].reparse = true;
            const auto c3 = scan(f3b, g, deep);
            const RemCandidate* sub = find(c3, deep.installLocation);
            Check(sub && sub->cls == RemClass::Protected && sub->reason.find(L"parent folder is a link") != std::wstring::npos, "k1 links: a folder below a link is Protected");
        }
        {   // shared with other applications / in use
            FakeFs f; FakeReg g; f.Dir(app.installLocation); f.Dir(app.installLocation + L"\\plugins\\other");
            RemEnv e = env; e.otherInstallLocations = {app.installLocation + L"\\plugins\\other"};
            auto c = RemScan(app, e, f.Fs(), g.R());
            Check(c.size() == 1 && c[0].cls == RemClass::Protected && c[0].reason.find(L"another installed application") != std::wstring::npos, "k1 shared: a folder that contains another installed application's folder is Protected");
            RemEnv e2 = env; e2.otherInstallLocations = {L"C:\\Program Files"};
            c = RemScan(app, e2, f.Fs(), g.R());
            Check(c.size() == 1 && c[0].cls == RemClass::Protected, "k1 shared: a folder inside another application's install folder is Protected");
            RemEnv e3 = env; e3.runningImages = {app.installLocation + L"\\fabnotes.exe"};
            c = RemScan(app, e3, f.Fs(), g.R());
            Check(c.size() == 1 && c[0].cls == RemClass::Protected && c[0].reason.find(L"running") != std::wstring::npos, "k1 shared: a folder with a running program in it is Protected");
        }
        {   // normalization: the same folder spelled differently classifies the same
            FakeFs f; FakeReg g; f.Dir(app.installLocation); f.File(app.installLocation + L"\\a.log");
            for (const wchar_t* spell : {L"c:/program files/fabrikam notes", L"C:\\PROGRAM FILES\\FABRIKAM NOTES\\", L"C:\\Program Files\\\\Fabrikam Notes"}) {
                RemApp a = app; a.installLocation = spell; const auto c = scan(f, g, a);
                Check(c.size() == 1 && c[0].cls == RemClass::Safe, std::string("k1 paths: ") + WToUtf8(spell) + " classifies like the canonical spelling");
            }
            RemEnv shout = env; shout.programFiles = L"C:\\PROGRAM FILES\\";
            const auto c = RemScan(app, shout, f.Fs(), g.R());
            Check(c.size() == 1 && c[0].cls == RemClass::Safe && c[0].path == app.installLocation, "k1 paths: the same install folder found through the install location and the Program Files name is reported once");
        }

        // ---- shortcuts: the target is the evidence, not the name
        {
            FakeFs f; FakeReg g; const std::wstring sm = env.startMenuCommon;
            f.Dir(sm); f.Dir(sm + L"\\Fabrikam"); f.Dir(app.installLocation);
            f.Lnk(sm + L"\\Fabrikam\\Fabrikam Notes.lnk", app.installLocation + L"\\fabnotes.exe");            // target gone: broken
            f.Lnk(sm + L"\\Notes (renamed).lnk", app.installLocation + L"\\bin\\tool.exe");                      // broken, any name
            f.Lnk(sm + L"\\Fabrikam Notes Alive.lnk", L"C:\\Tools\\other.exe"); f.File(L"C:\\Tools\\other.exe");   // another program that exists
            f.Lnk(sm + L"\\Fabrikam Notes Docs.lnk", L"C:\\Elsewhere\\gone.exe");                                 // broken but not ours
            f.Lnk(sm + L"\\Present.lnk", app.installLocation + L"\\present.exe"); f.File(app.installLocation + L"\\present.exe");   // inside, but the target still exists
            f.Lnk(sm + L"\\Unreadable.lnk", L"");
            f.Lnk(sm + L"\\Tricky.lnk", L"C:\\Program Files\\Fabrikam Notes-Evil\\x.exe");                      // shares only a prefix with the install folder
            const auto c = scan(f, g, app);
            std::set<std::wstring> shortcuts; for (const auto& x : c) if (x.kind == RemKind::Shortcut) shortcuts.insert(remdetail::Norm(x.path));
            Check(shortcuts == std::set<std::wstring>{remdetail::Norm(sm + L"\\Fabrikam\\Fabrikam Notes.lnk"), remdetail::Norm(sm + L"\\Notes (renamed).lnk")}, "k1 shortcuts: exactly the shortcuts whose target was inside the removed install folder and is gone");
            bool allSafe = true; for (const auto& x : c) if (x.kind == RemKind::Shortcut && (x.cls != RemClass::Safe || x.confidence < 80)) allSafe = false;
            Check(allSafe, "k1 shortcuts: they are Safe with high confidence");
            RemApp noLoc = app; noLoc.installLocation.clear();
            std::set<std::wstring> none; for (const auto& x : scan(f, g, noLoc)) if (x.kind == RemKind::Shortcut) none.insert(x.path);
            Check(none.empty(), "k1 shortcuts: without a known install folder no shortcut is a candidate");
        }

        // ---- registry
        {
            FakeFs f; FakeReg g; g.Add(app.uninstallKey); g.Add(L"HKCU\\Software\\Fabrikam Inc\\Fabrikam Notes"); g.Add(L"HKCU\\Software\\Fabrikam Inc"); g.Add(L"HKCU\\Software\\Fabrikam Inc\\Fabrikam Notes Pro"); g.Add(L"HKLM\\SOFTWARE\\Fabrikam Inc\\Fabrikam Notes");
            const auto c = scan(f, g, app);
            std::set<std::wstring> regs; for (const auto& x : c) if (x.kind == RemKind::RegistryKey) regs.insert(remdetail::Norm(x.path));
            Check(regs == std::set<std::wstring>{remdetail::Norm(app.uninstallKey), remdetail::Norm(L"HKCU\\Software\\Fabrikam Inc\\Fabrikam Notes")}, "k1 registry: only the application's own uninstall entry and its exact HKCU\\Software\\<publisher>\\<name> key are candidates");
            bool allConfirm = true; for (const auto& x : c) if (x.kind == RemKind::RegistryKey && x.cls != RemClass::Confirm) allConfirm = false;
            Check(allConfirm, "k1 registry: registry keys are never Safe: they need confirmation");
            RemApp ms = app; ms.publisher = L"Microsoft Corporation"; ms.name = L"Fabrikam Notes"; FakeReg g2; g2.Add(L"HKCU\\Software\\Microsoft Corporation\\Fabrikam Notes");
            const auto c2 = scan(f, g2, ms);
            Check(c2.size() == 1 && c2[0].cls == RemClass::Protected && c2[0].reason.find(L"shared") != std::wstring::npos, "k1 registry: a shared publisher's key is Protected");
            for (const wchar_t* bad : {L"HKLM\\SOFTWARE\\Fabrikam", L"HKCU\\Software\\Classes\\.fab", L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKLM\\SYSTEM\\CurrentControlSet\\Services\\Fab", L"HKCU\\Software"})
                Check(RemClassifyRegKey(bad, L"x", 50, app).cls == RemClass::Protected, std::string("k1 registry: ") + WToUtf8(bad) + " is not a verified application-specific key: Protected");
            RemApp ukSibling = app; ukSibling.uninstallKey = L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
            Check(RemClassifyRegKey(ukSibling.uninstallKey, L"x", 50, ukSibling).cls == RemClass::Protected, "k1 registry: the Uninstall container itself is never a candidate");
        }

        // ---- the gate
        {
            RemCandidate safe; safe.cls = RemClass::Safe; RemCandidate conf; conf.cls = RemClass::Confirm; RemCandidate prot; prot.cls = RemClass::Protected; prot.reason = L"shared";
            Check(RemMayRemove(safe, false).allowed && !RemMayRemove(safe, false).needsConfirmation, "k1 gate: Safe items are allowed");
            Check(!RemMayRemove(conf, false).allowed && RemMayRemove(conf, false).needsConfirmation && RemMayRemove(conf, true).allowed, "k1 gate: Confirm items are allowed only after the person confirmed");
            Check(!RemMayRemove(prot, true).allowed && !RemMayRemove(prot, false).allowed && RemMayRemove(prot, true).reason == L"shared", "k1 gate: Protected items are never allowed, confirmed or not, and say why");
        }
        {   // determinism and order
            FakeFs f; FakeReg g; f.Dir(app.installLocation); f.File(app.installLocation + L"\\c.ini"); f.Dir(L"C:\\ProgramData\\Fabrikam Inc"); f.Dir(L"C:\\ProgramData\\Fabrikam Notes");
            const auto a = scan(f, g, app), b = scan(f, g, app);
            bool same = a.size() == b.size(); for (size_t i = 0; same && i < a.size(); ++i) same = a[i].path == b[i].path && a[i].cls == b[i].cls;
            bool ordered = true; for (size_t i = 1; i < a.size(); ++i) if (static_cast<int>(a[i].cls) < static_cast<int>(a[i - 1].cls)) ordered = false;
            Check(same && ordered && a.size() == 3, "k1 order: the same machine gives the same list, Safe first, then Confirm, then Protected");
        }

        // ---- the read-only live probes on a private temp folder
        {
            const std::wstring base = std::wstring(tmp) + L"ankhcore_test_k1_" + std::to_wstring(GetCurrentProcessId());
            Evict(base, true, false);
            const std::wstring inst = base + L"\\Programs\\Live App"; EnsureDir(inst); EnsureDir(inst + L"\\logs");
            WriteAtomic(inst + L"\\logs\\a.log", reinterpret_cast<const uint8_t*>("x"), 1);
            const std::wstring outside = base + L"\\outside"; EnsureDir(outside); WriteAtomic(outside + L"\\keep.docx", reinterpret_cast<const uint8_t*>("secret"), 6);
            Run(L"cmd.exe", L"/c mklink /J \"" + inst + L"\\link\" \"" + outside + L"\"", true, 20000);
            const RemFs live = RemLiveFs();
            Check(live.exists(inst) && live.isDir(inst) && !live.exists(base + L"\\nope") && live.isReparse(inst + L"\\link") && !live.isReparse(inst), "k1 live fs: exists / isDir / isReparse report a real folder, a missing one and a real junction");
            std::set<std::wstring> names; for (const auto& e : live.list(inst)) names.insert(e.name + (e.reparse ? L" [link]" : e.dir ? L" [dir]" : L""));
            Check(names == std::set<std::wstring>{L"logs [dir]", L"link [link]"}, "k1 live fs: a listing marks the junction as a link");
            RemEnv e2 = env; e2.programFiles = base + L"\\Programs"; e2.windows = base + L"\\win"; e2.userProfile = base + L"\\prof"; e2.appData = base + L"\\roam"; e2.localAppData = base + L"\\local"; e2.programData = base + L"\\pd"; e2.temp = base + L"\\tmp";
            e2.programFilesX86.clear(); e2.startMenuCommon.clear(); e2.startMenuUser.clear(); e2.personalFolders.clear();
            RemApp la = app; la.name = L"Live App"; la.publisher = L"Nobody"; la.installLocation = inst; la.exeNames.clear(); la.uninstallKey.clear();
            const auto c = RemScan(la, e2, live, RemReg{});
            Check(c.size() == 1 && c[0].cls == RemClass::Protected && c[0].reason.find(L"link") != std::wstring::npos, "k1 live scan: an install folder with a junction inside is Protected and the junction's target is never entered");
            Check(FileExists(outside + L"\\keep.docx") && DirExists(outside), "k1 live scan: the scan removed nothing (the junction's target is untouched)");
            const RemReg reg = RemLiveReg();
            Check(reg.keyExists(L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion") && !reg.keyExists(L"HKLM\\SOFTWARE\\Ankh_no_such_key_k1") && !reg.keyExists(L"HKXX\\Software") && !reg.keyExists(L"HKCU"), "k1 live registry: a read-only probe tells an existing key from a missing one and refuses unknown hives");
            Evict(base, true, false);
        }
    }
    {   // ---- 2.7 K3: sequential Multi-Uninstall runner (a mock backend: nothing is installed, removed or run, except two harmless `cmd /c exit N` checks)
        struct Mock {
            std::map<std::wstring, UninstallRunResult> runs; std::map<std::wstring, UnInstalled> installed;
            std::vector<std::wstring> ran, cmds, scanned; std::atomic<int> active{0}, maxActive{0}; std::function<void(const std::wstring&)> onRun; std::set<std::thread::id> runThreads;
            UninstallBackend Be() {
                UninstallBackend b;
                b.run = [this](const UninstallItem& it, const std::wstring& cmd, const std::function<bool()>& cancelled) {
                    const int a = ++active; int m = maxActive.load(); while (a > m && !maxActive.compare_exchange_weak(m, a)) {}
                    ran.push_back(it.id); cmds.push_back(cmd); runThreads.insert(std::this_thread::get_id());
                    if (onRun) onRun(it.id);
                    UninstallRunResult r; r.launched = true; auto f = runs.find(it.id); if (f != runs.end()) r = f->second;
                    if (cancelled && cancelled() && r.launched && !r.cancelled && f != runs.end() && f->second.cancelled) r.cancelled = true;
                    --active; return r; };
                b.verify = [this](const UninstallItem& it) { auto f = installed.find(it.id); return f == installed.end() ? UnInstalled::Gone : f->second; };
                b.scanRemnants = [this](const UninstallItem& it) { scanned.push_back(it.id); RemCandidate c; c.path = L"C:\\left\\" + it.id; c.cls = RemClass::Confirm; return std::vector<RemCandidate>{c}; };
                return b;
            }
        };
        auto mk = [](const wchar_t* id, const wchar_t* name = nullptr) { UninstallItem it; it.id = id; it.name = name ? name : id; it.command = L"\"C:\\Apps\\" + std::wstring(id) + L"\\unins000.exe\" /SILENT"; it.uninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + std::wstring(id); return it; };
        auto states = [](const UninstallBatchReport& r) { std::vector<UnState> v; for (const auto& x : r.results) v.push_back(x.state); return v; };

        // ---- the normal case
        {
            Mock m; UninstallBatchRunner runner; std::vector<UninstallEvent> ev;
            const UninstallBatchReport r = runner.Run({mk(L"A"), mk(L"B"), mk(L"C")}, m.Be(), [&](const UninstallEvent& e) { ev.push_back(e); });
            Check(m.ran == std::vector<std::wstring>{L"A", L"B", L"C"} && m.maxActive == 1, "k3 run: applications are uninstalled one at a time, in the order given, never two at once");
            Check(states(r) == std::vector<UnState>{UnState::Removed, UnState::Removed, UnState::Removed} && r.removed == 3 && !r.restartRequired && !r.cancelled && !r.stoppedEarly, "k3 run: all three verified gone are Removed");
            std::string seq; for (const auto& e : ev) seq += std::to_string(e.index) + (e.state == UnState::Running ? "R" : e.state == UnState::Removed ? "D" : "?") + ",";
            Check(seq == "0R,0D,1R,1D,2R,2D," && ev[0].total == 3, "k3 events: each application reports Running and then its result, in order, with the total");
            Check(m.scanned == std::vector<std::wstring>{L"A", L"B", L"C"} && r.results[1].remnants.size() == 1 && r.results[1].remnants[0].path == L"C:\\left\\B", "k3 remnants: the K1 scan runs after each verified removal and its candidates are kept (nothing is deleted)");
            Check(r.summary.find(L"3 removed") == 0, "k3 summary: the summary starts with the count removed");
            Check(m.runThreads.size() == 1 && *m.runThreads.begin() == std::this_thread::get_id(), "k3 run: everything happens on the calling (worker) thread");
            Check(m.cmds[0] == L"\"C:\\Apps\\A\\unins000.exe\" /SILENT", "k3 run: a normal command is passed on unchanged");
        }
        Check(UninstallBatchRunner().Run({}, Mock().Be()).results.empty() && UninstallBatchRunner().Run({}, Mock().Be()).summary.find(L"0 removed") == 0, "k3 run: an empty batch does nothing and says so");

        // ---- the result is what the inventory says, not the exit code
        {
            Mock m; m.runs[L"nz"].launched = true; m.runs[L"nz"].exitCode = 1;   // non-zero but gone
            m.runs[L"cancelledInWindow"].launched = true; m.installed[L"cancelledInWindow"] = UnInstalled::StillInstalled;   // exit 0 but still listed
            m.runs[L"bad"].launched = true; m.runs[L"bad"].exitCode = 5; m.installed[L"bad"] = UnInstalled::StillInstalled;
            m.runs[L"unk"].launched = true; m.installed[L"unk"] = UnInstalled::Unknown;
            m.runs[L"unkbad"].launched = true; m.runs[L"unkbad"].exitCode = 7; m.installed[L"unkbad"] = UnInstalled::Unknown;
            UninstallBatchRunner runner;
            const UninstallBatchReport r = runner.Run({mk(L"nz"), mk(L"cancelledInWindow"), mk(L"bad"), mk(L"unk"), mk(L"unkbad")}, m.Be());
            Check(states(r) == std::vector<UnState>{UnState::Removed, UnState::NotRemoved, UnState::Failed, UnState::Unverified, UnState::Failed}, "k3 result: gone is Removed whatever the code; exit 0 but still listed is NotRemoved; a failing code with the entry listed is Failed; no verification is 'not verified'");
            Check(r.results[0].detail.find(L"exit code 1") != std::wstring::npos && r.results[0].detail.find(L"no longer listed") != std::wstring::npos, "k3 result: a non-zero code with the entry gone says both");
            Check(r.results[1].detail.find(L"still listed") != std::wstring::npos && r.results[2].detail.find(L"exit code 5") != std::wstring::npos, "k3 result: the reasons are in words");
            Check(r.removed == 1 && r.notRemoved == 1 && r.failed == 2 && r.unverified == 1, "k3 result: the counts keep removed, still listed, failed and not verified apart");
            Check(m.scanned == std::vector<std::wstring>{L"nz"}, "k3 remnants: only a verified removal is scanned (not 'still listed', failed or unverified)");
        }
        // ---- restart: reported, never done
        {
            Mock m; m.runs[L"r1"].launched = true; m.runs[L"r1"].exitCode = 3010; m.runs[L"r2"].launched = true; m.runs[L"r2"].exitCode = 1641;
            m.runs[L"r3"].launched = true; m.runs[L"r3"].exitCode = 3010; m.installed[L"r3"] = UnInstalled::StillInstalled;
            m.runs[L"r4"].launched = true; m.runs[L"r4"].exitCode = 3010; m.installed[L"r4"] = UnInstalled::Unknown;
            const UninstallBatchReport r = UninstallBatchRunner().Run({mk(L"r1"), mk(L"r2"), mk(L"r3"), mk(L"r4")}, m.Be());
            Check(states(r) == std::vector<UnState>{UnState::RemovedRestartRequired, UnState::RemovedRestartRequired, UnState::NotRemoved, UnState::RemovedRestartRequired} && r.restartRequired && r.restart == 3, "k3 restart: 3010 and 1641 are 'removed, restart required'; with the entry still listed it is NotRemoved; the batch reports a restart is needed");
            Check(r.results[0].detail.find(L"does not restart it") != std::wstring::npos, "k3 restart: the result says AnkhCore does not restart Windows");
        }
        Check(UninstallExitWantsRestart(3010) && UninstallExitWantsRestart(1641) && !UninstallExitWantsRestart(0) && !UninstallExitWantsRestart(1603), "k3 restart: only the two Windows 'success, restart' codes ask for a restart");

        // ---- failures, timeouts and the stop policy
        {
            Mock m; m.runs[L"f"].launched = false; m.runs[L"f"].detail = L"Windows error 2";
            const UninstallBatchReport r = UninstallBatchRunner().Run({mk(L"f"), mk(L"g")}, m.Be());
            Check(states(r) == std::vector<UnState>{UnState::Failed, UnState::Removed} && r.results[0].detail.find(L"Windows error 2") != std::wstring::npos, "k3 fail: an uninstaller that cannot start is Failed with the reason, and the next application still runs");
            Mock m2; m2.runs[L"f"].launched = false;
            UninstallOptions stop; stop.stopOnFailure = true;
            const UninstallBatchReport r2 = UninstallBatchRunner().Run({mk(L"f"), mk(L"g"), mk(L"h")}, m2.Be(), {}, stop);
            Check(states(r2) == std::vector<UnState>{UnState::Failed, UnState::NotStarted, UnState::NotStarted} && r2.stoppedEarly && m2.ran == std::vector<std::wstring>{L"f"}, "k3 fail: with stopOnFailure the rest are 'not started' and are not run");
        }
        {
            Mock m; m.runs[L"t"].launched = true; m.runs[L"t"].timedOut = true;
            const UninstallBatchReport r = UninstallBatchRunner().Run({mk(L"a"), mk(L"t"), mk(L"b")}, m.Be());
            Check(states(r) == std::vector<UnState>{UnState::Removed, UnState::TimedOut, UnState::NotStarted} && r.timedOut == 1 && r.stoppedEarly && r.results[2].detail.find(L"did not finish") != std::wstring::npos, "k3 timeout: a timed-out uninstaller may still be running, so the batch stops there by default");
            Check(r.results[1].detail.find(L"final state is unknown") != std::wstring::npos && !m.scanned.empty() && std::find(m.scanned.begin(), m.scanned.end(), L"t") == m.scanned.end(), "k3 timeout: the final state is called unknown and nothing is scanned for it");
            Mock m2; m2.runs[L"t"].launched = true; m2.runs[L"t"].timedOut = true;
            UninstallOptions go; go.stopOnTimeout = false;
            const UninstallBatchReport r2 = UninstallBatchRunner().Run({mk(L"t"), mk(L"b")}, m2.Be(), {}, go);
            Check(states(r2) == std::vector<UnState>{UnState::TimedOut, UnState::Removed}, "k3 timeout: with stopOnTimeout off the batch continues");
        }

        // ---- cancellation
        {
            Mock m; UninstallBatchRunner runner;
            m.onRun = [&](const std::wstring& id) { if (id == L"B") runner.Cancel(); };   // the person presses Cancel while B's uninstaller is running
            const UninstallBatchReport r = runner.Run({mk(L"A"), mk(L"B"), mk(L"C"), mk(L"D")}, m.Be());
            Check(states(r) == std::vector<UnState>{UnState::Removed, UnState::Removed, UnState::Cancelled, UnState::Cancelled} && r.cancelled && r.cancelledCount == 2, "k3 cancel: the running uninstaller finishes normally (it is never killed); the applications not yet started are Cancelled");
            Check(m.ran == std::vector<std::wstring>{L"A", L"B"}, "k3 cancel: nothing after the cancellation is run");
            Mock m2; m2.runs[L"x"].launched = true; m2.runs[L"x"].cancelled = true;
            const UninstallBatchReport r2 = UninstallBatchRunner().Run({mk(L"x")}, m2.Be());
            Check(states(r2) == std::vector<UnState>{UnState::Cancelled} && m2.scanned.empty(), "k3 cancel: a backend that stops before running reports Cancelled and nothing is scanned");
            const UninstallBatchReport r3 = runner.Run({mk(L"Z")}, Mock().Be());
            Check(states(r3) == std::vector<UnState>{UnState::Removed} && !r3.cancelled, "k3 cancel: a new batch starts with a clear cancel flag");
        }

        // ---- single flight
        {
            Mock m; UninstallBatchRunner runner; std::atomic<bool> inside{false}, go{false};
            m.onRun = [&](const std::wstring&) { inside = true; while (!go) Sleep(1); };
            UninstallBatchReport first; std::thread t([&] { first = runner.Run({mk(L"A")}, m.Be()); });
            for (int i = 0; i < 5000 && !inside; ++i) Sleep(1);
            Check(inside && runner.Busy(), "k3 busy: the runner reports it is busy while an uninstaller runs");
            const UninstallBatchReport second = runner.Run({mk(L"B")}, Mock().Be());
            Check(second.refusedBusy && second.results.empty() && second.summary.find(L"already running") != std::wstring::npos, "k3 busy: a second batch while one runs is refused, not queued");
            go = true; t.join();
            Check(first.removed == 1 && !runner.Busy() && m.ran == std::vector<std::wstring>{L"A"}, "k3 busy: the first batch completed, the refused one ran nothing, and the runner is free again");
        }

        // ---- refusals: nothing unsafe is run
        {
            Mock m; UninstallBatchRunner runner;
            auto item = [&](const wchar_t* id, const wchar_t* cmd, bool store = false, const wchar_t* name = nullptr) { UninstallItem it = mk(id, name); it.command = cmd; it.store = store; return it; };
            std::vector<UninstallItem> items = {
                item(L"empty", L""), item(L"amp", L"C:\\A\\u.exe /S & del C:\\x"), item(L"pipe", L"C:\\A\\u.exe | more"), item(L"redir", L"C:\\A\\u.exe > C:\\x"), item(L"caret", L"C:\\A\\u.exe ^& x"),
                item(L"ctrl", L"C:\\A\\u.exe\r\n/S"), item(L"long", std::wstring(2049, L'a').c_str()), item(L"quote", L"\"C:\\A\\u.exe /S"),
                item(L"self", L"C:\\A\\u.exe", false, L"AnkhCore Control Center"), item(L"ankhcore-id", L"C:\\A\\u.exe"), item(L"badstore", L"Fab;rm -rf", true), item(L"storeSpace", L"Fab Notes", true)};
            items[9].id = L"AnkhCore_Setup";
            const UninstallBatchReport r = runner.Run(items, m.Be());
            bool allRefused = true; for (const auto& x : r.results) if (x.state != UnState::Refused) allRefused = false;
            Check(allRefused && m.ran.empty(), "k3 refuse: an empty command, shell operators (& | > ^), control characters, an over-long or unbalanced command, AnkhCore itself and a malformed Store package name are all refused and never run");
            Check(r.refused == 12 && r.results[1].detail.find(L"shell operator") != std::wstring::npos && r.results[8].detail.find(L"cannot remove itself") != std::wstring::npos, "k3 refuse: each refusal says why");
            Mock m2;
            std::vector<UninstallItem> ok = {item(L"q", L"\"C:\\A&B\\u.exe\" /S"), item(L"msi", L"MsiExec.exe /X{11111111-2222-3333-4444-555555555555}"), item(L"store", L"Fab.Notes_1.2.3.0_x64__abc123", true)};
            const UninstallBatchReport r2 = runner.Run(ok, m2.Be());
            Check(m2.ran.size() == 3 && r2.removed == 3, "k3 refuse: an operator inside quotes, an MSI command and a plain Store package name are accepted");
            Check(m2.cmds[1] == L"MsiExec.exe /X{11111111-2222-3333-4444-555555555555} /norestart" && m2.cmds[0] == ok[0].command && m2.cmds[2] == ok[2].command, "k3 restart: msiexec got /norestart; the other commands are unchanged");
            const UninstallBatchReport r3 = runner.Run({mk(L"A"), mk(L"a"), mk(L"B")}, Mock().Be());
            Check(r3.results[1].state == UnState::Refused && r3.results[1].detail.find(L"twice") != std::wstring::npos && r3.removed == 2, "k3 refuse: the same application twice (any letter case) runs once");
        }
        {   // UninstallPrepare
            auto prep = [&](const wchar_t* c, bool store = false) { UninstallItem it = mk(L"p"); it.command = c; it.store = store; return UninstallPrepare(it); };
            Check(prep(L"msiexec /x {G}") == L"msiexec /x {G} /norestart" && prep(L"\"C:\\Windows\\System32\\msiexec.exe\" /x {G}") == L"\"C:\\Windows\\System32\\msiexec.exe\" /x {G} /norestart" && prep(L"  MSIEXEC /i {G}") == L"  MSIEXEC /i {G} /norestart", "k3 prepare: every spelling of msiexec gets /norestart");
            Check(prep(L"msiexec /x {G} /norestart") == L"msiexec /x {G} /norestart" && prep(L"msiexec /x {G} /forcerestart") == L"msiexec /x {G} /forcerestart" && prep(L"msiexec /x {G} /promptrestart") == L"msiexec /x {G} /promptrestart", "k3 prepare: a command that already says what to do about a restart is left alone");
            Check(prep(L"C:\\A\\unins000.exe /S") == L"C:\\A\\unins000.exe /S" && prep(L"Pkg_1.0", true) == L"Pkg_1.0", "k3 prepare: nothing else is modified");
        }
        {   // missing hooks and the mapping to the remnants scanner
            UninstallBackend none;
            const UninstallBatchReport r = UninstallBatchRunner().Run({mk(L"A")}, none);
            Check(r.results[0].state == UnState::Failed && r.results[0].detail.find(L"no way to run") != std::wstring::npos, "k3 hooks: without a run hook nothing can happen and it says so");
            Mock m; UninstallBackend nv = m.Be(); nv.verify = nullptr;
            const UninstallBatchReport r2 = UninstallBatchRunner().Run({mk(L"A")}, nv);
            Check(r2.results[0].state == UnState::Unverified && m.scanned.empty(), "k3 hooks: without a verifier a clean exit is 'not verified', never 'removed'");
            UninstallItem it = mk(L"A", L"Fab"); it.publisher = L"Pub"; it.installLocation = L"C:\\Program Files\\Fab"; it.exeNames = {L"fab"};
            const RemApp ra = UninstallToRemApp(it, true);
            Check(ra.name == L"Fab" && ra.publisher == L"Pub" && ra.installLocation == L"C:\\Program Files\\Fab" && ra.exeNames.size() == 1 && ra.uninstalledOk && !UninstallToRemApp(it, false).uninstalledOk, "k3 remnants: the item maps to the K1 application record; 'uninstalled ok' is set only when the removal was verified");
        }

        // ---- the live backend: two harmless `cmd /c exit N` runs and read-only registry checks (no uninstaller is run)
        {
            const UninstallBackend live = UninstallLiveBackend(20000);
            auto cmdItem = [&](const wchar_t* id, const wchar_t* cmd) { UninstallItem it = mk(id); it.command = cmd; it.uninstallKey = L"SOFTWARE\\Ankh_no_such_key_k3\\" + std::wstring(id); return it; };
            UninstallItem a = cmdItem(L"live1", L"cmd.exe /c exit 3010");
            const UninstallRunResult ra = live.run(a, UninstallPrepare(a), {});
            Check(ra.launched && !ra.timedOut && ra.exitCode == 3010, "k3 live: the real exit code of a harmless command comes back");
            UninstallItem z = cmdItem(L"live2", L"cmd.exe /c exit 0");
            Check(live.run(z, z.command, {}).launched && live.run(z, z.command, {}).exitCode == 0, "k3 live: exit code 0 comes back as 0");
            UninstallItem nx = cmdItem(L"live3", L"C:\\no\\such\\file.exe /S");
            const UninstallRunResult rn = live.run(nx, nx.command, {});
            Check(!rn.launched && !rn.detail.empty(), "k3 live: a command that cannot start is reported as not launched, with the Windows error");
            Check(live.run(a, a.command, [] { return true; }).cancelled, "k3 live: a cancel before the start stops before anything is run");
            UninstallItem real = mk(L"x"); real.uninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion";
            UninstallItem store = mk(L"s"); store.store = true; UninstallItem nokey = mk(L"n"); nokey.uninstallKey.clear();
            Check(live.verify(real) == UnInstalled::StillInstalled && live.verify(a) == UnInstalled::Gone && live.verify(store) == UnInstalled::Unknown && live.verify(nokey) == UnInstalled::Unknown, "k3 live: the verifier tells a listed entry from a missing one (read-only registry) and says 'unknown' where it cannot know");
            const UninstallBatchReport r = UninstallBatchRunner().Run({a}, live);
            Check(r.results[0].state == UnState::RemovedRestartRequired && r.restartRequired, "k3 live: through the runner, exit 3010 with the entry gone is 'removed, restart required' and nothing restarts");
        }
    }
    {   // ---- 2.7 J1: the navigation structure and its routes (pure data: nav_model.h)
        // saved page ids must keep their meaning: the enum is append-only
        struct Id { int v; int want; const char* name; };
        const Id ids[] = {{PDashboard, 0, "Dashboard"}, {PMonitor, 1, "Monitor"}, {PFan, 2, "Fan"}, {PAppearance, 3, "Appearance"}, {PWindhawk, 4, "Windhawk"}, {PTweaks, 5, "Tweaks"}, {POptimize, 6, "Optimize"}, {PApps, 7, "Apps"},
                          {PRecovery, 8, "Recovery"}, {PActivity, 9, "Activity"}, {PAbout, 10, "About"}, {PStartup, 11, "Startup"}, {PDesktop, 12, "Desktop"}, {PSbLayout, 13, "SbLayout"}, {PMonPrefs, 19, "MonPrefs"},
                          {PAppMaterial, 20, "AppMaterial"}, {PAppIcons, 24, "AppIcons"}, {PAppsInstalled, 25, "AppsInstalled"}, {PAppsUninstall, 27, "AppsUninstall"}, {PBackup, 28, "Backup"}, {PRecHistory, 29, "RecHistory"},
                          {PDrivers, 30, "Drivers"}, {PStorage, 31, "Storage"}, {PHardware, 32, "Hardware"}, {PNetwork, 33, "Network"}, {PErrLog, 34, "ErrLog"}, {PSettings, 35, "Settings"}, {PAppsUpdates, 36, "AppsUpdates"}, {PSystemCare, 37, "SystemCare"}, {PProcMgr, 38, "ProcMgr"}, {PPowerToys, 39, "PowerToys"}, {PageCount, 40, "PageCount"}};
        for (const Id& i : ids) Check(i.v == i.want, std::string("j1 ids: ") + i.name + " keeps its saved number (" + std::to_string(i.want) + ")");

        // the rail
        std::vector<std::wstring> names; for (const auto& sc : kNav) names.push_back(sc.name);
        Check(names == std::vector<std::wstring>{L"Dashboard", L"Monitor", L"Sidebar", L"Windows Studio", L"Tweaks", L"Optimize & Repair", L"Apps Manager", L"Recovery", L"Activity", L"About"}, "j1 rail: the ten sections of the 2.7 architecture, in order");
        auto pagesOf = [&](const wchar_t* n) { for (const auto& sc : kNav) if (sc.name == std::wstring(n)) return sc.pages; return std::vector<int>{}; };
        Check(pagesOf(L"Monitor") == std::vector<int>{PMonitor, PHardware, PNetwork, PFan, PStartup}, "j1 rail: Monitor holds System Monitor, Hardware Information, Network Monitor, Fan Control and Startup (no Sidebar)");
        Check(pagesOf(L"Sidebar") == std::vector<int>{PDesktop}, "j1 rail: Sidebar is its own section (the hub of the Sidebar and the Mini Monitor)");
        Check(pagesOf(L"Windows Studio") == std::vector<int>{PAppearance, PWindhawk, PPowerToys}, "j1 rail: Windows Studio holds Appearance & Themes, Windhawk and PowerToys");
        Check(NavParent(PAppsUpdates) == PApps && NavSectionOf(PAppsUpdates) == NavSectionOf(PApps) && !NavIsTopBarPage(PAppsUpdates), "2.7 updater: App Updates is a page of Apps Manager (Back goes to Apps Manager), no longer only a section at the end of Backup");
        Check(pagesOf(L"Apps Manager") == std::vector<int>{PApps} && pagesOf(L"Activity") == std::vector<int>{PActivity, PErrLog}, "j1 rail: Apps Manager and Activity keep their pages");
        for (const auto& sc : kNav) Check(!sc.pages.empty() && sc.name && sc.icon && *sc.icon, std::string("j1 rail: section ") + WToUtf8(sc.name) + " has pages and an icon");

        // every page has exactly one home
        {
            std::map<int, int> homes; for (const auto& sc : kNav) for (int pg : sc.pages) ++homes[pg];
            bool unique = true; for (const auto& kv : homes) if (kv.second != 1) unique = false;
            Check(unique, "j1 routes: no page appears in two rail sections");
            int orphans = 0, subPages = 0, subWithRailParent = 0, wrongSection = 0;
            for (int pg = 0; pg < PageCount; ++pg) {
                const int parent = NavParent(pg);
                if (parent >= 0) { ++subPages; if ((homes.count(parent) || NavIsTopBarPage(parent)) && NavParent(parent) < 0) ++subWithRailParent; if (NavSectionOf(pg) != NavSectionOf(parent)) ++wrongSection; }
                else if (!homes.count(pg) && !NavIsTopBarPage(pg)) ++orphans;
            }
            Check(orphans == 0, "j1 routes: every page is either on the rail or a sub-page (none is unreachable)");
            Check(subPages > 0 && subWithRailParent == subPages && wrongSection == 0, "j1 routes: every sub-page has a rail page (or the top-bar Settings page) as its parent (Back goes there)");
            Check(static_cast<int>(NavOrder().size()) + subPages + 1 == PageCount, "j1 routes: rail pages, sub-pages and the one top-bar page (Settings) are exactly all pages");
            bool inRange = true; for (int pg = 0; pg < PageCount; ++pg) { const int sct = NavSectionOf(pg); if (sct < 0 || sct >= static_cast<int>(kNav.size())) inRange = false; }
            Check(inRange, "j1 routes: every page maps to a valid section");
        }
        // where the pages that moved live now
        Check(kNav[NavSectionOf(PDesktop)].name == std::wstring(L"Sidebar") && kNav[NavSectionOf(PSbWidgets)].name == std::wstring(L"Sidebar") && kNav[NavSectionOf(PMiniSet)].name == std::wstring(L"Sidebar") && kNav[NavSectionOf(PMonPrefs)].name == std::wstring(L"Sidebar"), "j1 routes: the Sidebar pages (layout, widgets, Mini Monitor, monitoring preferences) are in the Sidebar section");
        Check(kNav[NavSectionOf(PWindhawk)].name == std::wstring(L"Windows Studio") && kNav[NavSectionOf(PAppearance)].name == std::wstring(L"Windows Studio"), "j1 routes: Windhawk and Appearance are in Windows Studio");
        for (int pg : {PAppTheme, PAppWallpaper, PAppCursor, PAppIcons}) Check(NavParent(pg) == PAppearance && kNav[NavSectionOf(pg)].name == std::wstring(L"Windows Studio"), "j1 routes: Theme, Wallpaper, Cursor and Icon managers stay under Appearance & Themes, in Windows Studio (" + std::to_string(pg) + ")");
        Check(kNav[NavSectionOf(PHardware)].name == std::wstring(L"Monitor") && kNav[NavSectionOf(PNetwork)].name == std::wstring(L"Monitor") && kNav[NavSectionOf(PFan)].name == std::wstring(L"Monitor") && kNav[NavSectionOf(PStartup)].name == std::wstring(L"Monitor"), "j1 routes: Hardware, Network, Fan and Startup stay in Monitor");
        Check(kNav[NavSectionOf(PBackup)].name == std::wstring(L"Recovery") && kNav[NavSectionOf(PDrivers)].name == std::wstring(L"Optimize & Repair") && kNav[NavSectionOf(PAppsUninstall)].name == std::wstring(L"Apps Manager"), "j1 routes: Recovery, Optimize & Repair and Apps Manager sub-pages are where they were");

        // saved values
        Check(NavPageOrDashboard(-1) == PDashboard && NavPageOrDashboard(PageCount) == PDashboard && NavPageOrDashboard(100000) == PDashboard && NavPageOrDashboard(PHardware) == PHardware && NavPageOrDashboard(0) == PDashboard, "j1 saved: a saved page id outside the range opens the Dashboard; a valid one opens that page");
        struct Open { int saved, layout, want; const char* why; };
        const Open opens[] = {
            {-1, 1, -1, "nothing open stays nothing"}, {-1, 2, -1, "nothing open (new layout)"}, {-5, 1, -1, "garbage below -1"},
            {1, 1, 1, "Monitor (2.5/2.6 index 1) is still Monitor"}, {8, 1, 8, "Activity (2.5/2.6 index 8) is still Activity"}, {1, 2, 1, "Monitor in the new layout"}, {8, 2, 8, "Activity in the new layout"},
            {2, 1, 3, "old Appearance (2) opens Windows Studio (3)"}, {3, 1, 3, "old Windhawk (3) opens Windows Studio (3)"}, {3, 2, 3, "Windows Studio in the new layout"},
            {2, 2, -1, "index 2 in the new layout is Sidebar: a one-page section is never 'open'"}, {0, 1, -1, "Dashboard is a one-page section"}, {9, 1, -1, "About is a one-page section"}, {4, 1, -1, "Tweaks is a one-page section"},
            {10, 1, -1, "an index past the old rail"}, {99, 1, -1, "far past"}, {10, 2, -1, "an index past the new rail"}, {5, 0, -1, "an unknown old layout number is treated as old"}};
        for (const Open& o : opens) Check(NavMigrateOpen(o.saved, o.layout) == o.want, std::string("j1 saved: nav.open ") + std::to_string(o.saved) + " (layout " + std::to_string(o.layout) + "): " + o.why);
        Check(NavMigrateOpen(2, 0) == 3 && NavMigrateOpen(3, 0) == 3, "j1 saved: layout 0 or absent is the old layout");
        {   // whatever the old rail had open, the migrated section exists and has children, or nothing opens
            bool ok = true; for (int old = -3; old < 14; ++old) { const int n = NavMigrateOpen(old, 1); if (n != -1 && (n < 0 || n >= static_cast<int>(kNav.size()) || kNav[static_cast<size_t>(n)].pages.size() < 2)) ok = false; }
            Check(ok, "j1 saved: every old open-section value migrates to a real multi-page section or to 'none'");
        }
        Check(kNavLayoutVersion == 2, "j1 saved: the layout version written with nav.open is 2");
    }
    {   // ---- 2.7 J2: the top-bar Settings page (pure routes: nav_model.h)
        Check(NavIsTopBarPage(PSettings) && !NavIsTopBarPage(PDashboard) && !NavIsTopBarPage(PAppMaterial) && !NavIsTopBarPage(PAppearance), "j2 nav: only Settings is a top-bar page");
        bool onRail = false; for (const auto& sc : kNav) for (int pg : sc.pages) if (pg == PSettings) onRail = true;
        Check(!onRail && NavParent(PSettings) < 0 && kNav.size() == 10, "j2 nav: Settings is not a rail section (the rail keeps its ten sections) and has no parent");
        Check(NavParent(PAppMaterial) == PSettings, "j2 nav: AnkhCore Appearance (the application's own look) is a sub-page of Settings, so its Back control goes to Settings");
        Check(NavParent(PAppTheme) == PAppearance && NavParent(PAppWallpaper) == PAppearance && NavParent(PAppCursor) == PAppearance && NavParent(PAppIcons) == PAppearance, "j2 nav: the Windows managers stay under Appearance & Themes in Windows Studio");
        Check(kNav[NavSectionOf(PSettings)].name == std::wstring(L"Dashboard") && NavSectionOf(PAppMaterial) == NavSectionOf(PSettings), "j2 nav: Settings maps to no rail section of its own (the fallback section is never highlighted: a top-bar page matches no rail row)");
        Check(NavPageOrDashboard(PSettings) == PSettings && NavMigrateOpen(3, 2) == 3, "j2 nav: a saved Settings page opens Settings; the saved open-section migration is unchanged");

        // the categories
        std::vector<std::wstring> cats; for (const auto& c : SettingsCategories()) cats.push_back(c.id);
        Check(cats == std::vector<std::wstring>{L"General", L"Appearance", L"Performance", L"Notifications", L"Updates", L"Privacy", L"Advanced"}, "j2 settings: the seven categories of the 2.7 prompt, in order");
        for (const auto& c : SettingsCategories()) Check(c.id && c.about && wcslen(c.about) > 10, std::string("j2 settings: ") + WToUtf8(c.id) + " says what it is for");
        // the links
        std::map<std::wstring, int> perCat; bool valid = true, noSelf = true, noDup = true; std::set<std::pair<std::wstring, int>> seen;
        for (const auto& l : SettingsLinks()) {
            ++perCat[l.category];
            if (l.page < 0 || l.page >= PageCount) valid = false;
            if (l.page == PSettings) noSelf = false;
            if (!seen.insert({l.category, l.page}).second) noDup = false;
            Check(l.title && l.desc && wcslen(l.title) > 2 && wcslen(l.desc) > 20, std::string("j2 links: '") + WToUtf8(l.title) + "' has a title and a description");
        }
        Check(valid, "j2 links: every link opens a real page");
        Check(noSelf, "j2 links: no link opens Settings itself");
        Check(noDup, "j2 links: no category links to the same page twice");
        bool knownCats = true; for (const auto& kv : perCat) if (std::find(cats.begin(), cats.end(), kv.first) == cats.end()) knownCats = false;
        Check(knownCats, "j2 links: every link belongs to one of the seven categories");
        for (const auto& c : SettingsCategories()) Check(perCat[c.id] >= 1, std::string("j2 links: ") + WToUtf8(c.id) + " has at least one link");
        auto target = [&](const wchar_t* cat, int page) { for (const auto& l : SettingsLinks()) if (std::wstring(l.category) == cat && l.page == page) return true; return false; };
        Check(target(L"General", PStartup) && target(L"Appearance", PAppMaterial) && target(L"Performance", PMonPrefs) && target(L"Notifications", PMonPrefs) && target(L"Updates", PBackup) && target(L"Privacy", PActivity) && target(L"Advanced", PErrLog), "j2 links: each category reaches the existing page that holds its settings");
        bool windowsOnly = true; for (const auto& l : SettingsLinks()) if (l.page == PAppTheme || l.page == PAppWallpaper || l.page == PAppCursor || l.page == PAppIcons || l.page == PWindhawk || l.page == PAppearance) windowsOnly = false;
        Check(windowsOnly, "j2 links: Settings never links to a Windows-customization page (theme, wallpaper, cursor, icons, Windhawk): the two are kept apart");
        // every page reachable from Settings is reachable by Back: the pages it opens exist and have a rail or parent route
        bool routed = true; for (const auto& l : SettingsLinks()) { const int sct = NavSectionOf(l.page); if (sct < 0 || sct >= static_cast<int>(kNav.size())) routed = false; }
        Check(routed, "j2 links: every target has a section");
    }
    {   // ---- 2.7 K2/K4: Apps Manager workflow (selection, batch uninstall, cleanup with recovery) and the bundled UniGetUI working copy
        // inventory -> items
        Check(AppIconExeName(L"\"C:\\Program Files\\Foo\\foo.exe\",0") == L"foo" && AppIconExeName(L"C:\\x\\Bar App.exe,-12") == L"Bar App" && AppIconExeName(L"C:\\x\\unins000.exe").empty() && AppIconExeName(L"C:\\x\\setup.exe").empty() && AppIconExeName(L"C:\\x\\icon.ico").empty() && AppIconExeName(L"").empty(),
              "k4 inventory: the executable name comes from DisplayIcon; uninstaller/setup names and non-exe icons are not evidence");
        {
            AppEntry e; e.name = L"Foo"; e.publisher = L"Acme"; e.uninstall = L"\"C:\\Foo\\unins.exe\" /S"; e.key = L"SOFTWARE\\X\\Uninstall\\Foo"; e.installLocation = L"C:\\Foo"; e.icon = L"C:\\Foo\\foo.exe,0"; e.userHive = true;
            const UninstallItem it = AppToUninstallItem(e);
            Check(it.id == L"win:SOFTWARE\\X\\Uninstall\\Foo" && it.command == e.uninstall && it.installLocation == L"C:\\Foo" && it.uninstallKey == e.key && it.userHive && it.exeNames == std::vector<std::wstring>{L"foo"} && !it.store, "k4 inventory: a desktop entry becomes an uninstall item with its own key, location and hive");
            AppEntry st; st.name = L"Pkg"; st.key = L"Pkg_1.0_x64__abc"; st.uninstall = st.key; st.store = true;
            const UninstallItem sit = AppToUninstallItem(st);
            Check(sit.store && sit.command == st.key && sit.uninstallKey.empty() && sit.id == L"store:Pkg_1.0_x64__abc" && sit.id != it.id, "k4 inventory: a Store app is identified apart from desktop entries and has no registry key to verify");
        }
        // K2 gating with a mock backend
        auto cand = [](const wchar_t* path, RemClass cl, RemKind k = RemKind::Folder) { RemCandidate c; c.path = path; c.cls = cl; c.kind = k; c.reason = L"r"; return c; };
        {
            std::vector<std::wstring> removed; std::set<std::wstring> gone{L"C:\\Gone"};
            CleanBackend be;
            be.exists = [&](const RemCandidate& c) { return !gone.count(c.path); };
            be.remove = [&](const RemCandidate& c, const std::wstring&, std::wstring& why) { if (c.path == L"C:\\Fails") { why = L"in use"; return false; } removed.push_back(c.path); why = L"moved"; return true; };
            const std::vector<RemCandidate> cs = {cand(L"C:\\Safe", RemClass::Safe), cand(L"C:\\NeedsOk", RemClass::Confirm), cand(L"C:\\Protected", RemClass::Protected), cand(L"C:\\Gone", RemClass::Safe), cand(L"C:\\Fails", RemClass::Safe),
                                                  cand(L"C:\\Safe", RemClass::Safe), cand(L"relative\\path", RemClass::Safe), cand(L"C:\\Last", RemClass::Safe)};
            const CleanReport r = RemExecute(cs, {}, be, L"C:\\rec");
            Check(r.removed == 2 && r.failed == 1 && r.missing == 1 && r.refused == 4, "k4 clean: safe items go, a failure does not stop the rest, a gone item is 'already gone', unconfirmed / protected / duplicate / relative items are refused (" + std::to_string(r.removed) + "/" + std::to_string(r.failed) + "/" + std::to_string(r.missing) + "/" + std::to_string(r.refused) + ")");
            Check(removed == std::vector<std::wstring>{L"C:\\Safe", L"C:\\Last"}, "k4 clean: only the safe items reached the backend, in order");
            removed.clear();
            const CleanReport r2 = RemExecute(cs, {L"C:\\NeedsOk", L"C:\\Protected"}, be, L"C:\\rec");
            Check(std::find(removed.begin(), removed.end(), L"C:\\NeedsOk") != removed.end() && std::find(removed.begin(), removed.end(), L"C:\\Protected") == removed.end() && r2.refused == 3, "k4 clean: a confirmed 'needs confirmation' item is cleaned; a Protected item never is, even when 'confirmed'");
            CleanBackend none;
            Check(RemExecute({cand(L"C:\\Safe", RemClass::Safe)}, {}, none, L"C:\\rec").failed == 1, "k4 clean: with no backend nothing is removed and it is reported");
        }
        // the real file operations, in a temp folder (never a user folder)
        {
            namespace fs = std::filesystem;
            const fs::path root = fs::temp_directory_path() / ("ankh_k4_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
            std::error_code ec; fs::create_directories(root / "Leftover" / "sub", ec);
            { std::ofstream(root / "Leftover" / "a.txt") << "data"; std::ofstream(root / "Leftover" / "sub" / "b.txt") << "more"; std::ofstream(root / "single.lnk") << "x"; }
            const std::wstring rec = (root / "recovery").wstring(), folder = (root / "Leftover").wstring(), file = (root / "single.lnk").wstring();
            std::wstring why;
            Check(RemMoveToRecovery(folder, rec, why) && !fs::exists(root / "Leftover") && fs::exists(root / "recovery" / "Leftover" / "sub" / "b.txt"), "k4 files: a folder is MOVED into the recovery folder with its contents (nothing is deleted) (" + WToUtf8(why) + ")");
            Check(RemMoveToRecovery(file, rec, why) && !fs::exists(root / "single.lnk") && fs::exists(root / "recovery" / "single.lnk"), "k4 files: a single file (shortcut) is moved the same way");
            Check(!RemMoveToRecovery((root / "nothing").wstring(), rec, why) && !why.empty(), "k4 files: something that is not there is refused with a reason");
            Check(!RemMoveToRecovery(folder, L"Z:\\nowhere", why) && !fs::exists(root / "Leftover"), "k4 files: a recovery folder on another drive is refused (a move cannot cross drives)");
            fs::create_directories(root / "Other");
            std::vector<std::wstring> problems;
            { std::ofstream(root / "single.lnk") << "new one"; }   // something already stands where the shortcut was
            const int n = RemUndo(rec, problems);
            Check(n == 1 && fs::exists(root / "Leftover" / "sub" / "b.txt") && fs::exists(root / "single.lnk") && problems.size() == 1 && fs::file_size(root / "single.lnk") == 7, "k4 undo: the folder is put back, and a file is NOT overwritten when something new stands in its place (reported)");
            // a link (junction) is never followed or removed
            fs::create_directories(root / "Target");
            const std::wstring link = (root / "Link").wstring();
            Run(SysDir() + L"\\cmd.exe", L"/c mklink /J \"" + link + L"\" \"" + (root / "Target").wstring() + L"\"", true, 15000);
            if (fs::exists(root / "Link")) Check(!RemMoveToRecovery(link, rec, why) && fs::exists(root / "Target") && fs::exists(root / "Link"), "k4 files: a junction is refused and its target untouched");
            else Check(true, "k4 files: (junction could not be created here; skipped)");
            RemoveDirectoryW(link.c_str());
            fs::remove_all(root, ec);
        }
        // the model: selection, single-flight batch with a mock uninstaller, remnants selection, cleanup
        {
            AppsMgr m;
            m.Toggle(L"a"); m.Toggle(L"b"); m.Toggle(L"a");
            Check(m.SelectedCount() == 1 && m.Selected(L"b") && !m.Selected(L"a"), "k4 model: toggling selects and unselects");
            m.ClearSelection(); Check(m.SelectedCount() == 0, "k4 model: clear selection");
            UninstallItem x; x.id = L"win:x"; x.name = L"Xapp"; x.command = L"\"C:\\x\\u.exe\""; x.uninstallKey = L"K\\x"; x.installLocation = L"C:\\x";
            UninstallItem y = x; y.id = L"win:y"; y.name = L"Yapp"; y.uninstallKey = L"K\\y"; y.installLocation = L"C:\\y";
            std::set<std::wstring> installed{L"win:x", L"win:y"};
            UninstallBackend be;
            be.run = [&](const UninstallItem& it, const std::wstring&, const std::function<bool()>&) { UninstallRunResult r; r.launched = true; installed.erase(it.id); return r; };
            be.verify = [&](const UninstallItem& it) { return installed.count(it.id) ? UnInstalled::StillInstalled : UnInstalled::Gone; };
            be.scanRemnants = [&](const UninstallItem& it) { return std::vector<RemCandidate>{cand((L"C:\\" + it.name + L"Dir").c_str(), RemClass::Safe), cand((L"C:\\" + it.name + L"Key").c_str(), RemClass::Confirm, RemKind::RegistryKey), cand(L"C:\\Windows\\Shared", RemClass::Protected)}; };
            std::atomic<int> notes{0};
            Check(m.StartUninstall({x, y}, be, [&] { ++notes; }), "k4 model: a batch starts");
            Check(!m.StartUninstall({x}, be, nullptr), "k4 model: a second batch while one runs is refused");
            for (int i = 0; i < 100 && m.Snapshot().phase < 2; ++i) Sleep(50);
            AppsMgrView v = m.Snapshot();
            Check(v.phase == 2 && v.report.removed == 2 && v.remnants.size() == 6 && notes.load() >= 2, "k4 model: both programs are verified removed and their leftovers are listed (" + std::to_string(v.remnants.size()) + ")");
            int safeSel = 0, confirmSel = 0, protSel = 0; for (const auto& r : v.remnants) { if (r.selected && r.c.cls == RemClass::Safe) ++safeSel; if (r.selected && r.c.cls == RemClass::Confirm) ++confirmSel; if (r.selected && r.c.cls == RemClass::Protected) ++protSel; }
            Check(safeSel == 2 && confirmSel == 0 && protSel == 0, "k4 model: only Safe leftovers start ticked; 'needs confirmation' and Protected do not");
            size_t protIdx = 99, confIdx = 99; for (size_t i = 0; i < v.remnants.size(); ++i) { if (v.remnants[i].c.cls == RemClass::Protected && protIdx == 99) protIdx = i; if (v.remnants[i].c.cls == RemClass::Confirm && confIdx == 99) confIdx = i; }
            m.ToggleRemnant(protIdx); m.ToggleRemnant(confIdx);
            v = m.Snapshot();
            Check(!v.remnants[protIdx].selected && v.remnants[confIdx].selected, "k4 model: a Protected leftover cannot be ticked; a 'needs confirmation' one can");
            std::vector<std::wstring> touched;
            CleanBackend cb; cb.exists = [](const RemCandidate&) { return true; };
            cb.remove = [&](const RemCandidate& c, const std::wstring&, std::wstring& why) { touched.push_back(c.path); why = L"moved"; return true; };
            Check(m.StartClean(cb, true, nullptr), "k4 model: the cleanup starts");
            for (int i = 0; i < 100 && m.Snapshot().phase != 4; ++i) Sleep(50);
            v = m.Snapshot();
            Check(v.phase == 4 && v.clean.removed == 3 && touched.size() == 3 && std::find(touched.begin(), touched.end(), L"C:\\Windows\\Shared") == touched.end(), "k4 model: the two safe leftovers and the confirmed one are cleaned; the protected one never reaches the backend (" + std::to_string(touched.size()) + ")");
            Check(v.remnants.size() == 3, "k4 model: cleaned items leave the list");
            m.Dismiss(); Check(m.Snapshot().phase == 0 && m.Snapshot().remnants.empty(), "k4 model: Done returns to the selection");
        }
        // the bundled UniGetUI working copy (pure file operations on temp folders)
        {
            namespace fs = std::filesystem;
            const fs::path root = fs::temp_directory_path() / ("ankh_ug_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
            const fs::path src = root / "bundled", dst = root / "user";
            std::error_code ec; fs::create_directories(src / "Assets" / "Data", ec);
            { std::ofstream(src / "UniGetUI.exe") << "gui"; std::ofstream(src / "uniget.exe") << "cli"; std::ofstream(src / "ForceUniGetUIPortable") << ""; std::ofstream(src / "Assets" / "Data" / "x.json") << "{}"; }
            Check(UgSourceComplete(src.wstring()) && !UgSourceComplete(dst.wstring()) && !UgCopyReady(src.wstring(), dst.wstring()), "ugbundle: a bundled copy is complete with both programs and the portable marker; no working copy yet");
            std::wstring why;
            Check(UgPrepareCopy(src.wstring(), dst.wstring(), nullptr, &why) && UgCopyReady(src.wstring(), dst.wstring()) && fs::exists(dst / "Assets" / "Data" / "x.json") && fs::exists(dst / "ForceUniGetUIPortable"), "ugbundle: the working copy is made with every file (including a folder named Data inside Assets) and the portable marker (" + WToUtf8(why) + ")");
            fs::create_directories(dst / "Data");
            { std::ofstream(dst / "Data" / "settings.json") << "user"; }
            Check(UgPrepareCopy(src.wstring(), dst.wstring()) && fs::exists(dst / "Data" / "settings.json"), "ugbundle: preparing again keeps the user's own UniGetUI data in the working copy");
            fs::remove(dst / kUgStampName, ec);
            Check(!UgCopyReady(src.wstring(), dst.wstring()), "ugbundle: a copy without its stamp (interrupted) is not ready");
            Check(UgPrepareCopy(src.wstring(), dst.wstring()) && UgCopyReady(src.wstring(), dst.wstring()), "ugbundle: it is completed on the next call");
            fs::remove(src / "ForceUniGetUIPortable", ec);
            Check(!UgSourceComplete(src.wstring()) && !UgPrepareCopy(src.wstring(), (root / "other").wstring(), nullptr, &why) && !why.empty() && !fs::exists(root / "other"), "ugbundle: an incomplete bundled copy is refused with a reason and nothing is copied");
            // detection: the working copy is found as a portable UniGetUI
            { std::ofstream(src / "ForceUniGetUIPortable") << ""; }
            const AcbUniGetInstall ug = AcbDetectUniGetUI({dst.wstring()});
            Check(ug.found && ug.portable && ug.dir == dst.wstring(), "ugbundle: the working copy is detected as a portable UniGetUI");
            fs::remove_all(root, ec);
        }
    }
    {   // ---- 2.8 System Care: exclusions, shortcut scanner, classifier, planner, executor, model, navigation
        Check(NavParent(PSystemCare) == POptimize && NavSectionOf(PSystemCare) == NavSectionOf(POptimize) && !NavIsTopBarPage(PSystemCare), "care nav: System Care is an Optimize & Repair page (Back goes to Optimize & Repair); the ten rail sections are unchanged");
        // exclusions
        const std::set<std::wstring> ig = CareParseIgnore(L" C:\\Users\\X\\a.lnk |browsers||  Recycle ");
        Check(ig.size() == 3 && ig.count(L"browsers") && ig.count(L"recycle") && ig.count(L"c:\\users\\x\\a.lnk") && CareParseIgnore(CareSerializeIgnore(ig)) == ig, "care ignore: entries are trimmed, lower-cased, de-duplicated and round-trip through the stored text");
        Check(CareIsIgnored(ig, L"BROWSERS") && !CareIsIgnored(ig, L"usertemp"), "care ignore: matching is case-insensitive");
        // classifier
        const CareFinding safe = CareClassifyCleanerItem(L"usertemp", L"Your temporary files", L"Temporary", L"Files in your temp folder.", 0, 1000, 0);
        const CareFinding opt = CareClassifyCleanerItem(L"prefetch", L"Prefetch", L"System", L"Prefetch files.", 1, 50, 0);
        const CareFinding ask = CareClassifyCleanerItem(L"recycle", L"Recycle Bin", L"Temporary", L"The Recycle Bin.", 2, 9000, 2);
        Check(safe.cls == CareClass::Cleanable && opt.cls == CareClass::Review && ask.cls == CareClass::Review && ask.evidence.find(L"permanently") != std::wstring::npos && ask.evidence.find(L"2 folder(s) could not be read") != std::wstring::npos && safe.irreversible, "care classify: Safe is Cleanable, Optional and Asks-first are Review required (with the reason), unreadable folders are said so");
        // the broken-shortcut scanner on a mock file system
        std::map<std::wstring, std::vector<RemDirEntry>> tree;
        auto de = [](const wchar_t* n, bool dir = false, bool reparse = false) { RemDirEntry e; e.name = n; e.dir = dir; e.reparse = reparse; e.size = 1000; return e; };
        tree[L"C:\\SM"] = {de(L"Broken.lnk"), de(L"Fine.lnk"), de(L"Note.txt"), de(L"Sub", true), de(L"Linked", true, true), de(L"Var.lnk"), de(L"Unc.lnk"), de(L"Offline.lnk"), de(L"Shell.lnk"), de(L"Unreadable.lnk")};
        tree[L"C:\\SM\\Sub"] = {de(L"InSub.lnk")};
        tree[L"C:\\SM\\Linked"] = {de(L"ShouldNotBeSeen.lnk")};
        std::map<std::wstring, std::wstring> targets = {{L"C:\\SM\\Broken.lnk", L"C:\\Gone\\app.exe"}, {L"C:\\SM\\Fine.lnk", L"C:\\Here\\app.exe"}, {L"C:\\SM\\Var.lnk", L"%ProgramFiles%\\x\\y.exe"},
            {L"C:\\SM\\Unc.lnk", L"\\\\server\\share\\a.exe"}, {L"C:\\SM\\Offline.lnk", L"E:\\usb\\a.exe"}, {L"C:\\SM\\Shell.lnk", L""}, {L"C:\\SM\\Sub\\InSub.lnk", L"C:\\Gone\\two.exe"}, {L"C:\\SM\\Linked\\ShouldNotBeSeen.lnk", L"C:\\Gone\\x.exe"}};
        RemFs mfs;
        mfs.exists = [](const std::wstring& p) { return Lower(p) == L"c:\\here\\app.exe"; };
        mfs.list = [&](const std::wstring& d) { auto it = tree.find(d); return it == tree.end() ? std::vector<RemDirEntry>{} : it->second; };
        mfs.shortcutTarget = [&](const std::wstring& l, std::wstring& t) { auto it = targets.find(l); if (it == targets.end()) return false; t = it->second; return true; };
        CareShortcutEnv senv; senv.roots = {L"C:\\SM", L"C:\\SM"};   // listed twice on purpose
        senv.targetDriveIsFixedAndPresent = [](const std::wstring& t) { return t.size() > 2 && (t[0] == L'C' || t[0] == L'c'); };
        const std::vector<CareFinding> bs = CareScanBrokenShortcuts(senv, mfs);
        std::set<std::wstring> ids; for (const auto& f : bs) ids.insert(f.id);
        Check(bs.size() == 2 && ids.count(L"C:\\SM\\Broken.lnk") && ids.count(L"C:\\SM\\Sub\\InSub.lnk"), "care shortcuts: only shortcuts whose exact target is missing on a present local drive are found (" + std::to_string(bs.size()) + ")");
        Check(!ids.count(L"C:\\SM\\Fine.lnk") && !ids.count(L"C:\\SM\\Var.lnk") && !ids.count(L"C:\\SM\\Unc.lnk") && !ids.count(L"C:\\SM\\Offline.lnk") && !ids.count(L"C:\\SM\\Shell.lnk") && !ids.count(L"C:\\SM\\Unreadable.lnk") && !ids.count(L"C:\\SM\\Linked\\ShouldNotBeSeen.lnk"),
              "care shortcuts: a working target, an unresolved variable, a network path, an offline drive, a shell item, an unreadable shortcut and anything under a link are never judged broken");
        Check(!bs.empty() && bs[0].cls == CareClass::Cleanable && !bs[0].irreversible && bs[0].kind == CareKind::BrokenShortcut && bs[0].evidence.find(L"not deleted") != std::wstring::npos, "care shortcuts: a broken shortcut is Cleanable, undoable (moved, not deleted) and explains why");
        // planner
        std::vector<CareFinding> fl = {safe, opt, ask, bs[0]};
        CareFinding prot = safe; prot.id = L"prot"; prot.title = L"P"; prot.cls = CareClass::Protected;
        CareFinding nr = safe; nr.id = L"nr"; nr.title = L"N"; nr.cls = CareClass::NotReadable;
        fl.push_back(prot); fl.push_back(nr);
        std::set<std::wstring> all; for (const auto& f : fl) all.insert(f.id);
        CarePlan pl = CareBuildPlan(fl, all, {}, {});
        Check(pl.items.size() == 2 && pl.refused.size() == 4, "care plan: ticked Cleanable items enter; Review without confirmation, Protected and unreadable items are refused with a reason (" + std::to_string(pl.items.size()) + "/" + std::to_string(pl.refused.size()) + ")");
        pl = CareBuildPlan(fl, all, {L"recycle"}, {});
        Check(pl.items.size() == 3 && std::any_of(pl.items.begin(), pl.items.end(), [](const CareFinding& f) { return f.id == L"recycle"; }) && std::none_of(pl.items.begin(), pl.items.end(), [](const CareFinding& f) { return f.id == L"prefetch"; }), "care plan: only the Review item the person confirmed is added");
        pl = CareBuildPlan(fl, all, {L"recycle", L"prefetch"}, {L"usertemp"});
        Check(std::none_of(pl.items.begin(), pl.items.end(), [](const CareFinding& f) { return f.id == L"usertemp"; }), "care plan: an ignored item never enters a plan, even when ticked");
        // executor: verified space, not claimed space
        std::map<std::wstring, uint64_t> disk = {{L"usertemp", 1000}, {L"prefetch", 50}, {L"recycle", 9000}, {L"stuck", 700}};
        std::set<std::wstring> moved;
        CareBackend be;
        be.exists = [&](const CareFinding& f) { return f.kind == CareKind::BrokenShortcut ? moved.count(f.id) == 0 : disk.count(f.id) > 0; };
        be.measure = [&](const CareFinding& f) -> uint64_t { return f.kind == CareKind::BrokenShortcut ? 1000 : disk[f.id]; };
        be.clean = [&](const CareFinding& f) -> uint64_t {
            if (f.id == L"usertemp") { disk[f.id] = 200; return 800; }                  // frees 800, 200 remain in use
            if (f.id == L"prefetch") return 50;                                         // CLAIMS 50 but nothing changes
            if (f.id == L"stuck") return 0;
            disk[f.id] = 0; return 9000; };
        be.moveToRecovery = [&](const CareFinding& f, const std::wstring&, std::wstring& why) { moved.insert(f.id); why = L"moved"; return true; };
        CareFinding stuck = safe; stuck.id = L"stuck"; stuck.title = L"Stuck";
        CareFinding gone = safe; gone.id = L"vanished"; gone.title = L"Vanished";
        CarePlan ep; ep.items = {safe, opt, ask, stuck, gone, bs[0]};
        const CareReport er = CareExecute(ep, be, L"C:\\rec");
        auto res = [&](const wchar_t* id) -> const CareResult& { for (const auto& r : er.items) if (r.f.id == id) return r; return er.items[0]; };
        Check(res(L"usertemp").state == CareState::Cleaned && res(L"usertemp").verified == 800 && res(L"usertemp").after == 200 && res(L"usertemp").detail.find(L"remain") != std::wstring::npos, "care exec: verified space is the difference between two measurements, and what remains is said");
        Check(res(L"prefetch").state == CareState::NothingFreed && res(L"prefetch").reported == 50 && res(L"prefetch").verified == 0 && res(L"prefetch").detail.find(L"not counted") != std::wstring::npos, "care exec: space the cleaner CLAIMS but a new measurement does not confirm is not counted");
        Check(res(L"stuck").state == CareState::NothingFreed && res(L"vanished").state == CareState::NothingFreed && res(L"recycle").verified == 9000 && res(L"recycle").state == CareState::Cleaned, "care exec: an item with nothing freed or already gone is reported as such; a fully cleaned one is counted");
        Check(res(bs[0].id.c_str()).state == CareState::Cleaned && moved.count(bs[0].id) == 1 && er.cleaned == 3 && er.verified == 800 + 9000 + 1000, "care exec: a broken shortcut is moved to recovery (not cleaned by the cleaner); the total counts only verified bytes (" + std::to_string(er.verified) + ")");
        CareBackend be2 = be; be2.moveToRecovery = [](const CareFinding&, const std::wstring&, std::wstring& why) { why = L"in use"; return false; };
        disk[L"usertemp"] = 1000;
        moved.clear();
        CarePlan ep2; ep2.items = {bs[0], safe}; const CareReport er2 = CareExecute(ep2, be2, L"C:\\rec");
        Check(er2.failed == 1 && er2.items[0].state == CareState::Failed && er2.items[1].state == CareState::Cleaned, "care exec: one failure does not stop the next item");
        // the model
        {
            CareMgr cm;
            std::vector<std::wstring> progressSeen;
            const bool started = cm.StartScan([&](const std::function<void(size_t, size_t, const std::wstring&)>& prog, const std::atomic<bool>&) { prog(0, 2, L"a"); prog(1, 2, L"b"); return std::vector<CareFinding>{safe, opt, bs[0]}; }, nullptr);
            Check(started && !cm.StartScan([](const std::function<void(size_t, size_t, const std::wstring&)>&, const std::atomic<bool>&) { return std::vector<CareFinding>{}; }, nullptr), "care model: a scan starts and a second one is refused while it runs");
            for (int i = 0; i < 100 && cm.Snapshot().phase < 2; ++i) Sleep(20);
            CareView v = cm.Snapshot();
            Check(v.phase == 2 && v.findings.size() == 3 && v.chosen.count(L"usertemp") == 1 && v.chosen.count(L"prefetch") == 0, "care model: Cleanable items start ticked, Review items do not");
            cm.Toggle(L"prefetch", true); v = cm.Snapshot();
            Check(v.chosen.count(L"prefetch") && v.confirmed.count(L"prefetch"), "care model: ticking a Review item is its confirmation");
            cm.Toggle(L"prefetch", true); v = cm.Snapshot();
            Check(!v.chosen.count(L"prefetch") && !v.confirmed.count(L"prefetch"), "care model: unticking removes the confirmation too");
            cm.Forget(L"usertemp"); v = cm.Snapshot();
            Check(v.findings.size() == 2 && !v.chosen.count(L"usertemp"), "care model: ignoring an item removes it from the results");
            std::map<std::wstring, uint64_t> d2 = {{L"prefetch", 50}};
            CareBackend mb; mb.exists = [](const CareFinding&) { return true; }; mb.measure = [&](const CareFinding& f) -> uint64_t { return f.kind == CareKind::BrokenShortcut ? 100 : d2[f.id]; };
            mb.clean = [&](const CareFinding& f) -> uint64_t { d2[f.id] = 0; return 50; }; mb.moveToRecovery = [](const CareFinding&, const std::wstring&, std::wstring& why) { why = L"moved"; return true; };
            cm.Toggle(L"prefetch", true);
            Check(cm.StartClean({}, mb, [](const CareFinding&) { return std::wstring(L"C:\\rec"); }, nullptr), "care model: the cleanup starts from the ticked items");
            for (int i = 0; i < 100 && cm.Snapshot().phase != 4; ++i) Sleep(20);
            v = cm.Snapshot();
            Check(v.phase == 4 && v.report.cleaned == 2 && v.report.verified == 150 && v.lastRecoveryDir == L"C:\\rec", "care model: the report counts verified bytes and keeps the recovery folder of the moved shortcut");
            Check(v.findings.size() == 1 && v.findings[0].id == L"prefetch" || v.findings.size() == 1, "care model: a moved shortcut leaves the results");
            cm.Dismiss(); Check(cm.Snapshot().phase == 2, "care model: Done returns to the results");
        }
    }
    {   // ---- 2.8 Windows Glass: tokens, the accent follows Windows, and it is the default
        const ThemeSpec wg = WindowsGlassTheme(RGB(0, 120, 212));
        Check(wg.bg == RGB(32, 32, 32) && wg.side == RGB(28, 28, 28) && wg.panel == RGB(43, 43, 43) && wg.panel2 == RGB(50, 50, 50) && wg.overlay == RGB(44, 44, 44) && wg.text == RGB(255, 255, 255), "glass: the layer fills are the Windows 11 dark Mica / Acrylic / card tokens");
        const ThemeSpec wgRed = WindowsGlassTheme(RGB(232, 17, 35)), wgGreen = WindowsGlassTheme(RGB(16, 124, 16));
        Check(wgRed.accent != wg.accent && wgGreen.accent != wg.accent && GetRValue(wgRed.accent) > GetBValue(wgRed.accent) && GetGValue(wgGreen.accent) > GetBValue(wgGreen.accent) && wgRed.panel == wg.panel, "glass: only the accent follows the person's Windows colour (red stays red, green stays green); the surfaces never change");
        Check(wcscmp(ThemeById(L"windows-glass").id, L"windows-glass") == 0 && wcscmp(ThemeById(L"neutral-glass").id, L"neutral-glass") == 0 && wcscmp(ThemeById(L"classic").id, L"classic") == 0 && wcscmp(ThemeById(L"").id, L"windows-glass") == 0, "glass: all three themes stay selectable; empty or unknown means Windows Glass");
        Check(wg.cardRadius < NeutralGlassTheme().cardRadius && ThemeMix(RGB(0, 0, 0), RGB(100, 100, 100), 50) == RGB(50, 50, 50), "glass: rounded Windows 11 corners; the colour mix helper is exact");
        Check(!Settings{}.Has(L"appearance.glassDefaults28"), "glass: a settings file has no 'defaults applied' mark until the application sets it");
    }
    {   // ---- 2.8 fan support: reading and control are decided separately; control follows the model
        Check(FanMakerOf(L"Hewlett-Packard") == L"HP" && FanMakerOf(L"HP") == L"HP" && FanMakerOf(L"Dell Inc.") == L"Dell" && FanMakerOf(L"LENOVO") == L"Lenovo" && FanMakerOf(L"ASUSTeK COMPUTER INC.") == L"ASUS" && FanMakerOf(L"Micro-Star International Co., Ltd.") == L"MSI" &&
              FanMakerOf(L"Acer") == L"Acer" && FanMakerOf(L"GIGABYTE") == L"Gigabyte" && FanMakerOf(L"SAMSUNG ELECTRONICS CO., LTD.") == L"Samsung" && FanMakerOf(L"To Be Filled By O.E.M.").empty() && FanMakerOf(L"").empty(), "fansupport: the maker is recognised from the BIOS name (ten makers); an unknown or placeholder name stays unknown");
        FanSupportFacts base; base.helperRunning = true; base.nbfcInstalled = false;
        { FanSupportFacts f = base; f.manufacturer = L"HP"; f.model = L"HP ZBook Fury 15 G7"; f.isNotebook = true; f.fans = 2; f.fansWithRpm = 2; f.hpBiosFan = true; f.nbfcInstalled = true; f.configsExact = 1; f.activeConfig = L"HP ZBook Fury 15 G7";
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.reading == FanReadState::Live && r.control == FanControlRoute::ConfigExact && r.canControl && r.maker == L"HP" && r.sourcesTried.size() == 5 && r.sourcesTried[1].find(L"reports fan values") != std::wstring::npos, "fansupport: an HP notebook with BIOS fan values and an exact NBFC configuration reads live and controls through the exact-model configuration"); }
        { FanSupportFacts f = base; f.manufacturer = L"Dell Inc."; f.model = L"Latitude 5520"; f.isNotebook = true;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.reading == FanReadState::NotExposed && !r.steps.empty() && r.steps[0].find(L"Dell Command | Monitor") != std::wstring::npos && r.control == FanControlRoute::NbfcMissing && !r.canControl, "fansupport: a Dell notebook with no fan value says not exposed, names Dell Command | Monitor as the way, and is monitoring-only without a configuration");
          Check(r.sourcesTried[2].find(L"not installed or no fan value") != std::wstring::npos && r.sourcesTried[1].find(L"not used") != std::wstring::npos, "fansupport: the sources actually tried are listed per maker (HP interface not used on Dell)"); }
        { FanSupportFacts f = base; f.manufacturer = L"LENOVO"; f.model = L"82JW Legion 5"; f.isNotebook = true; f.configsExact = 1;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.control == FanControlRoute::NbfcMissingMatchExists && !r.canControl && r.steps.size() == 2 && r.steps[1].find(L"does not bundle") != std::wstring::npos && r.steps[0].find(L"Lenovo") != std::wstring::npos, "fansupport: a configuration exists for the model but NBFC is not installed: said plainly, with the licence reason it is not bundled"); }
        { FanSupportFacts f = base; f.manufacturer = L"ASUSTeK"; f.isNotebook = true; f.nbfcInstalled = true; f.configsFamily = 2;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.control == FanControlRoute::ConfigFamily && r.canControl && r.controlLine.find(L"experimental") != std::wstring::npos, "fansupport: only same-family configurations: usable, but marked experimental with the safeguards named");
          f.configsFamily = 0; const FanSupportReport r2 = FanSupportAssess(f);
          Check(r2.control == FanControlRoute::NbfcNoMatch && !r2.canControl && r2.controlLine.find(L"no configuration for this model") != std::wstring::npos, "fansupport: NBFC installed but nothing for this model: no control is offered"); }
        { FanSupportFacts f = base; f.manufacturer = L"Gigabyte Technology Co., Ltd."; f.fans = 3; f.fansWithRpm = 3; f.sensorHelperFan = true; f.directControllable = 3;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.reading == FanReadState::Live && r.control == FanControlRoute::Direct && r.canControl && r.controlLine.find(L"3 fan channel") != std::wstring::npos, "fansupport: a desktop with writable Super I/O channels: live reading, direct control"); }
        { FanSupportFacts f = base; f.fans = 3; f.fansWithRpm = 1; f.sensorHelperFan = true;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.reading == FanReadState::Partial && r.readingLine.find(L"1 of 3") != std::wstring::npos && r.control == FanControlRoute::NoRoute && r.controlLine.find(L"Desktop") != std::wstring::npos, "fansupport: some fans without a value are partial; a desktop without a control channel is monitoring-only with a desktop-specific line"); }
        { FanSupportFacts f = base; f.helperRunning = false;
          const FanSupportReport r = FanSupportAssess(f);
          Check(r.reading == FanReadState::HelperOff && !r.steps.empty() && r.sourcesTried[0].find(L"not running") != std::wstring::npos, "fansupport: with the sensor helper off nothing is claimed and the first step is to start it"); }
        Check(kFanControlEnabled, "fansupport: fan control is enabled in this build (owner decision)");
    }
    {   // ---- 2.8 PawnIO fan readers in the support report
        FanSupportFacts f; f.helperRunning = true; f.manufacturer = L"LENOVO"; f.model = L"ThinkPad T14 Gen 3"; f.isNotebook = true; f.fans = 1; f.fansWithRpm = 1; f.thinkpadEcFan = true;
        FanSupportReport r = FanSupportAssess(f);
        Check(r.reading == FanReadState::Live && r.sourcesTried.size() == 5 && r.sourcesTried[3].find(L"reports fan 1") != std::wstring::npos && r.sourcesTried[4].find(L"not used (not a Dell PC)") != std::wstring::npos, "pawnio fans: a ThinkPad whose embedded controller reports fan 1 is shown as read through the PawnIO module");
        f.fans = 0; f.fansWithRpm = 0; f.thinkpadEcFan = false; r = FanSupportAssess(f);
        Check(r.reading == FanReadState::NotExposed && !r.steps.empty() && r.steps[0].find(L"ThinkPad") != std::wstring::npos && r.sourcesTried[3].find(L"ThinkPad, no value yet") != std::wstring::npos, "pawnio fans: a ThinkPad without a value is told why and what to check");
        f.pawnReadersOff = true; r = FanSupportAssess(f);
        Check(r.sourcesTried[3].find(L"switched off") != std::wstring::npos, "pawnio fans: when the person turned the readers off, that is what the report says");
        FanSupportFacts d; d.helperRunning = true; d.manufacturer = L"Dell Inc."; d.isNotebook = true; d.fans = 2; d.fansWithRpm = 2; d.dellSmmFan = true;
        r = FanSupportAssess(d);
        Check(r.reading == FanReadState::Live && r.sourcesTried[4].find(L"reports fan values") != std::wstring::npos && r.sourcesTried[2].find(L"not installed or no fan value") != std::wstring::npos, "pawnio fans: a Dell read through SMM is shown as such (Dell Command Monitor not needed)");
        d.fans = 0; d.fansWithRpm = 0; d.dellSmmFan = false; r = FanSupportAssess(d);
        Check(!r.steps.empty() && r.steps[0].find(L"tries Dell's system-management interface") != std::wstring::npos && r.steps[0].find(L"Dell Command | Monitor") != std::wstring::npos, "pawnio fans: without a value the Dell step says SMM was tried and names Dell Command | Monitor as the fallback");
    }
    {   // ---- 2.8 AnkhCore's own fan-control engine: the support report routes
        FanSupportFacts f; f.helperRunning = true; f.manufacturer = L"HP"; f.model = L"HP ZBook Studio G3"; f.isNotebook = true; f.fans = 2; f.fansWithRpm = 2; f.hpBiosFan = true; f.configsExact = 1; f.configsFamily = 3; f.ecReady = true; f.nbfcInstalled = false;
        FanSupportReport r = FanSupportAssess(f);
        Check(r.control == FanControlRoute::AnkhEcExact && r.canControl && r.controlLine.find(L"built into AnkhCore") != std::wstring::npos && r.steps.empty(), "ec engine: a notebook with an exact built-in configuration is offered AnkhCore's own control, with no other program needed");
        f.configsExact = 0; r = FanSupportAssess(f);
        Check(r.control == FanControlRoute::AnkhEcFamily && r.canControl && r.controlLine.find(L"experimental") != std::wstring::npos, "ec engine: only a same-family configuration is experimental and says so");
        f.ecActive = true; f.activeConfig = L"HP ZBook Studio G3"; r = FanSupportAssess(f);
        Check(r.control == FanControlRoute::AnkhEcActive && r.canControl && r.controlLine.find(L"BIOS gets the fans back") != std::wstring::npos, "ec engine: when it is running the report says so and what the safeguards are");
        f.ecActive = false; f.ecReady = false; f.configsExact = 1; r = FanSupportAssess(f);
        Check(r.control == FanControlRoute::NbfcMissingMatchExists || r.control == FanControlRoute::NbfcNoMatch || r.control == FanControlRoute::NbfcMissing, "ec engine: without the module (PawnIO missing) the report falls back to the honest old routes");
        f.ecReady = true; f.directControllable = 2; r = FanSupportAssess(f);
        Check(r.control == FanControlRoute::Direct, "ec engine: a channel the sensor helper can set directly always wins over the configuration route");
        Check(std::wstring(FanControlRouteLabel(FanControlRoute::AnkhEcActive)).find(L"AnkhCore engine") != std::wstring::npos && std::wstring(FanControlRouteLabel(FanControlRoute::AnkhEcExact)).find(L"built in") != std::wstring::npos, "ec engine: the labels name the route");
        Readings rd; Check(rd.ecState.empty(), "ec engine: the readings carry the engine state (empty until the helper reports it)");
    }
    {   // ---- 2.8 Process Manager: change engine, restore, rules
        const ProcContext ctx0{5000, 1, L"C:\\Windows", {}, {L"ankhcore", L"ankhsensors"}};
        auto mk = [](DWORD pid, const wchar_t* name, const wchar_t* path) { ProcRaw r; r.pid = pid; r.ppid = 1; r.session = 1; r.name = name; r.path = path; r.workingSet = 100ull << 20; return r; };
        const ProcEntry app = ProcBuildInventory({mk(1500, L"game.exe", L"D:\\Games\\game.exe")}, ctx0).entries[0];
        const ProcEntry crit = ProcBuildInventory({mk(900, L"lsass.exe", L"C:\\Windows\\System32\\lsass.exe")}, ctx0).entries[0];
        std::map<DWORD, ProcState> world; world[1500] = ProcState{true, kProcPrioNormal, 0xFF, 0xFF, 0}; world[900] = ProcState{true, kProcPrioHigh, 0xFF, 0xFF, 0};
        ProcBackend be;
        be.read = [&](DWORD pid) { auto it = world.find(pid); return it == world.end() ? ProcState{} : it->second; };
        be.setPriority = [&](DWORD pid, DWORD pr, std::wstring&) { world[pid].prio = pr; return true; };
        be.setAffinity = [&](DWORD pid, uint64_t m, std::wstring&) { world[pid].affinity = m; return true; };
        be.setEfficiency = [&](DWORD pid, bool e, std::wstring&) { world[pid].eco = e ? 1 : 0; return true; };
        be.terminate = [&](DWORD pid, std::wstring&) { world.erase(pid); return true; };
        ProcHistory hist;
        ProcChange c = ProcApplyChange(app, ProcAction::SetPriority, kProcPrioBelowNormal, false, 5000, be, hist);
        Check(c.ok && world[1500].prio == kProcPrioBelowNormal && hist.count(1500) && hist[1500].before.prio == kProcPrioNormal, "procmgr: a priority change is applied, read back, and the first state is recorded");
        c = ProcApplyChange(app, ProcAction::SetPriority, kProcPrioHigh, false, 5000, be, hist);
        Check(c.refused && world[1500].prio == kProcPrioBelowNormal, "procmgr: a change that needs confirmation is refused until the person confirmed (nothing changed)");
        c = ProcApplyChange(app, ProcAction::SetPriority, kProcPrioHigh, true, 5000, be, hist);
        Check(c.ok && world[1500].prio == kProcPrioHigh && hist[1500].before.prio == kProcPrioNormal, "procmgr: confirmed, it is applied; the recorded state is still the FIRST one");
        Check(ProcApplyChange(app, ProcAction::SetPriority, kProcPrioRealtime, true, 5000, be, hist).refused && world[1500].prio == kProcPrioHigh, "procmgr: the real-time class is never used, even confirmed");
        Check(ProcApplyChange(crit, ProcAction::SetPriority, kProcPrioIdle, true, 5000, be, hist).refused && world[900].prio == kProcPrioHigh, "procmgr: a critical process is never changed");
        c = ProcApplyChange(app, ProcAction::SetAffinity, 0x0F, true, 5000, be, hist);
        Check(c.ok && world[1500].affinity == 0x0F, "procmgr: CPU affinity is applied and read back");
        Check(ProcApplyChange(app, ProcAction::SetAffinity, 0xF00, true, 5000, be, hist).refused && world[1500].affinity == 0x0F, "procmgr: a mask with no existing core is refused, nothing changes");
        c = ProcApplyChange(app, ProcAction::SetEfficiency, 1, true, 5000, be, hist);
        Check(c.ok && world[1500].eco == 1, "procmgr: efficiency mode is applied and read back");
        c = ProcRestore(app, 5000, be, hist);
        Check(c.ok && world[1500].prio == kProcPrioNormal && world[1500].affinity == 0xFF && world[1500].eco == 0 && !hist.count(1500), "procmgr: Restore puts back the priority, the cores and the efficiency mode from before the first change");
        Check(!ProcRestore(app, 5000, be, hist).ok, "procmgr: restoring a process that was never changed says so");
        ProcBackend lie = be; lie.setPriority = [&](DWORD, DWORD, std::wstring&) { return true; };   // claims success, changes nothing
        ProcHistory h2; c = ProcApplyChange(app, ProcAction::SetPriority, kProcPrioIdle, true, 5000, lie, h2);
        Check(!c.ok && c.detail.find(L"not counted") != std::wstring::npos, "procmgr: a change Windows accepted but the process does not show is not counted");
        c = ProcApplyChange(app, ProcAction::Terminate, 0, false, 5000, be, hist);
        Check(c.refused && world.count(1500), "procmgr: ending a process without a confirmation is refused");
        c = ProcApplyChange(app, ProcAction::Terminate, 0, true, 5000, be, hist);
        Check(c.ok && !world.count(1500), "procmgr: ending an ordinary application, confirmed, works and is verified");
        // rules
        const std::vector<ProcRule> rules = ProcParseRules(L"game.exe|16384|15|1|1;Bad;evil..exe|32|0|-1|1;rt.exe|256|0|-1|1;off.exe|64|0|-1|0;game.exe|128|0|-1|1");
        Check(rules.size() == 2 && rules[0].exe == L"game.exe" && rules[0].prio == kProcPrioBelowNormal && rules[0].affinity == 15 && rules[0].eco == 1 && rules[1].exe == L"off.exe" && !rules[1].enabled, "procmgr rules: parsed; garbage, a bad name, the real-time class and a duplicate are dropped");
        Check(ProcParseRules(ProcSerializeRules(rules)).size() == 2 && ProcSerializeRules(rules) == ProcSerializeRules(ProcParseRules(ProcSerializeRules(rules))), "procmgr rules: round trip");
        world[1500] = ProcState{true, kProcPrioNormal, 0xFF, 0xFF, 0}; world[900] = ProcState{true, kProcPrioHigh, 0xFF, 0xFF, 0};
        ProcHistory h3; std::set<std::wstring> done; std::vector<std::wstring> log;
        std::vector<ProcEntry> ents = ProcBuildInventory({mk(1500, L"game.exe", L"D:\\Games\\game.exe"), mk(900, L"lsass.exe", L"C:\\Windows\\System32\\lsass.exe")}, ctx0).entries;
        const std::vector<ProcRule> r2 = {ProcRule{L"game", kProcPrioBelowNormal, 3, 1, true}, ProcRule{L"lsass", kProcPrioIdle, 0, -1, true}};
        int n = ProcApplyRules(r2, ents, 5000, be, h3, done, &log);
        Check(n == 3 && world[1500].prio == kProcPrioBelowNormal && world[1500].affinity == 3 && world[1500].eco == 1 && world[900].prio == kProcPrioHigh, "procmgr rules: a rule sets priority, cores and efficiency on an ordinary application; a protected process is never touched");
        n = ProcApplyRules(r2, ents, 5000, be, h3, done, &log);
        Check(n == 0, "procmgr rules: the same process is not touched again");
        done.clear(); world[1500].prio = kProcPrioNormal; n = ProcApplyRules({ProcRule{L"game", kProcPrioBelowNormal, 0, -1, false}}, ents, 5000, be, h3, done, &log);
        Check(n == 0 && world[1500].prio == kProcPrioNormal, "procmgr rules: a disabled rule does nothing");
    }
    {   // ---- 2.8 PowerToys: the settings text edit (the only thing AnkhCore writes into PowerToys' own file)
        const std::string js = "{\n  \"name\": \"general\",\n  \"startup\": false,\n  \"enabled\": {\n    \"FancyZones\": true,\n    \"AlwaysOnTop\": false,\n    \"Awake\": true\n  },\n  \"x\": {\"enabled\": true}\n}\n";
        Check(PtReadEnabled(js, "FancyZones") == 1 && PtReadEnabled(js, "AlwaysOnTop") == 0 && PtReadEnabled(js, "Nope") == -1 && PtReadEnabled("not json", "FancyZones") == -1, "powertoys: the enabled block is read (on, off, missing, not valid)");
        std::string out;
        Check(PtSetEnabled(js, "AlwaysOnTop", true, out) && PtReadEnabled(out, "AlwaysOnTop") == 1 && PtReadEnabled(out, "FancyZones") == 1 && PtReadEnabled(out, "Awake") == 1 && out.size() == js.size() - 1, "powertoys: one switch changes, the others stay");
        std::string back; PtSetEnabled(out, "AlwaysOnTop", false, back);
        Check(back == js, "powertoys: switching back restores the file byte for byte (formatting and every other setting are untouched)");
        Check(!PtSetEnabled(js, "Nope", true, out) && out.empty() && !PtSetEnabled("{}", "FancyZones", true, out), "powertoys: a key that is not listed, or a file without the block, is refused and nothing is produced");
        const std::string nested = "{\"enabled\": {\"FancyZones\": false}, \"inner\": {\"enabled\": {\"Awake\": true}}}";
        Check(PtReadEnabled(nested, "Awake") == -1 && PtReadEnabled(nested, "FancyZones") == 0, "powertoys: only the first top-level enabled block is used");
        const std::string pad = std::string("{\"enabled\": {\"Awake\": true}}") + std::string(8, '\0');
        Check(PtSetEnabled(pad, "Awake", false, out) && out.size() == pad.size() + 1 && PtReadEnabled(out, "Awake") == 0, "powertoys: a file padded with NUL bytes (a known PowerToys quirk) is handled");
        bool allKnown = !PtModules().empty(); for (const auto& m : PtModules()) allKnown = allKnown && PtKeyAllowed(m.key) && wcslen(m.title) > 3 && wcslen(m.desc) > 10;
        Check(allKnown && !PtKeyAllowed(L"Evil") && PtModules().size() >= 12, "powertoys: AnkhCore manages a fixed list of utilities and refuses any other key");
        Check(NavSectionOf(PPowerToys) == NavSectionOf(PAppearance) && NavParent(PPowerToys) < 0, "powertoys: its page is in Windows Studio on the rail");
    }
    {   // ---- 2.8.3 stabilization: fan control levels
        FanLevelFacts f; f.reading = FanReadState::Live; f.controlSupported = true; f.configsAvailable = 2; f.haveSelection = true; f.selExact = true; f.engineReady = true;
        FanLevels l = FanAssessLevels(f);
        Check(l.monitoring && l.controlSupported && l.configAvailable && l.configVerified && l.operationAvailable && l.viaEngine && !l.viaNbfc && l.reason.empty(),
              "fan 2.8.3: an exact-model configuration with a ready engine: the five levels are true and Apply goes through AnkhCore's own engine (NBFC is not needed)");
        f.engineReady = false; f.engineState = L"unavailable: no driver"; l = FanAssessLevels(f);
        Check(!l.operationAvailable && l.monitoring && l.configAvailable && l.configVerified && l.reason.find(L"cannot reach") != std::wstring::npos && l.reason.find(L"unavailable: no driver") != std::wstring::npos,
              "fan 2.8.3: engine not ready: reading, configuration and verification stay true, Apply is off, and the reason names the engine state");
        f.engineReady = true; f.selExact = false; f.selFamily = true; l = FanAssessLevels(f);
        Check(l.operationAvailable && !l.configVerified && l.viaEngine, "fan 2.8.3: a family configuration is available but never 'verified' (it stays experimental and still needs the confirmation)");
        f.selFamily = false; l = FanAssessLevels(f);
        Check(l.operationAvailable && l.viaEngine && l.unverified && !l.configVerified && l.reason.empty(), "fan 2.8.7: a configuration for another model IS applied when the person chose it (Apply works for any chosen model), flagged unverified so the confirmation warns");
        Check(!FanAssessLevels(FanLevelFacts{}).unverified, "fan 2.8.7: nothing is flagged unverified without a selection");
        {   // the model text repeats the maker on some PCs; that matched nothing
            Check(FanNormalizeModel(L"HP HP ZBook 15 G5") == L"hp zbook 15 g5" && FanNormalizeModel(L"Lenovo  ThinkPad T580") == L"lenovo thinkpad t580" && FanNormalizeModel(L"").empty(), "fan 2.8.7: repeated words in the model text are collapsed");
            const std::string xml = "<FanControlConfigV2><NotebookModel>HP ZBook 15 G3</NotebookModel><FanConfigurations><FanConfiguration><ReadRegister>1</ReadRegister></FanConfiguration></FanConfigurations></FanControlConfigV2>";
            FanCompatCandidate cc;
            Check(ParseNbfcConfigXml(xml, L"HP ZBook 15 G3", L"C:\\x.xml", L"HP HP ZBook 15 G5", cc) && cc.familyMatch && !cc.exactModelMatch, "fan 2.8.7: 'HP HP ZBook 15 G5' now finds the HP ZBook configurations as the same family");
            Check(ParseNbfcConfigXml(xml, L"HP ZBook 15 G3", L"C:\\x.xml", L"HP ZBook 15 G3", cc) && cc.exactModelMatch, "fan 2.8.7: an exact model still matches exactly");
        }
        f.selExact = true; f.testBuild = true; l = FanAssessLevels(f);
        Check(!l.operationAvailable && l.reason.find(L"test build") != std::wstring::npos, "fan 2.8.3: the test build never writes to the fans");
        f.testBuild = false; f.controlEnabled = false; l = FanAssessLevels(f);
        Check(!l.operationAvailable, "fan 2.8.3: with fan control switched off nothing is available");
        f.controlEnabled = true; f.haveSelection = false; l = FanAssessLevels(f);
        Check(!l.operationAvailable && l.reason.find(L"Select") != std::wstring::npos, "fan 2.8.3: no selection, no Apply");
        f.haveSelection = true; f.alreadyActive = true; l = FanAssessLevels(f);
        Check(!l.operationAvailable && l.reason.find(L"already active") != std::wstring::npos, "fan 2.8.3: an active configuration is not applied twice");
        f.alreadyActive = false; f.nbfcInstalled = true; f.selExact = false; f.selFamily = false; l = FanAssessLevels(f);
        Check(l.operationAvailable && l.viaNbfc && !l.viaEngine, "fan 2.8.3: with NBFC installed the old route is unchanged");
        FanLevelFacts none; FanLevels ln = FanAssessLevels(none);
        Check(!ln.monitoring && !ln.controlSupported && !ln.configAvailable && !ln.configVerified && !ln.operationAvailable, "fan 2.8.3: an empty fact set claims nothing");
    }
    {   // ---- 2.8.3: updater Skip and Cancel
        UpdSkipSet sk;
        Check(UpdSkipAlways(sk, "Mozilla.Firefox") && UpdSkipVersion(sk, "Git.Git", "2.50.0") && !UpdSkipAlways(sk, "bad;id") && !UpdSkipAlways(sk, "a=b") && !UpdSkipVersion(sk, "Git.Git", "1;2") && !UpdSkipVersion(sk, "Git.Git", ""),
              "updater skip: only plain ids and versions are stored (nothing that could break the stored text)");
        auto pk = [](const char* id, const char* v, const char* nv) { UpdListedPackage p; p.id = id; p.name = id; p.version = v; p.newVersion = nv; return p; };
        const std::vector<UpdListedPackage> all = {pk("Mozilla.Firefox", "130", "131"), pk("Git.Git", "2.49", "2.50.0"), pk("7zip.7zip", "23", "24")};
        size_t ha = 0, hv = 0; std::vector<UpdListedPackage> vis = UpdFilterSkipped(all, sk, &ha, &hv);
        Check(vis.size() == 1 && vis[0].id == "7zip.7zip" && ha == 1 && hv == 1, "updater skip: 'always' hides the package, 'this version' hides only that offered version");
        std::vector<UpdListedPackage> later = all; later[1].newVersion = "2.51.0";
        vis = UpdFilterSkipped(later, sk);
        Check(vis.size() == 2 && vis[0].id == "Git.Git", "updater skip: a NEWER version of a version-skipped package is offered again");
        sk.always.erase("Mozilla.Firefox");
        Check(UpdFilterSkipped(all, sk).size() == 2, "updater skip: restoring an 'always' entry brings the package back");
        UpdSkipSet rt; rt.always = UpdParseSkipText(UpdSerializeSkipText(sk.always)); rt.versions = UpdParseSkipText(UpdSerializeSkipText(sk.versions));
        Check(rt.always == sk.always && rt.versions == sk.versions && UpdParseSkipText("a;;b\r\n;c").size() == 3, "updater skip: the stored text round-trips and ignores empty entries");
        // cancel between packages
        const std::vector<UpdListedPackage> four = {pk("a.a", "1", "2"), pk("b.b", "1", "2"), pk("c.c", "1", "2"), pk("d.d", "1", "2")};
        int ran = 0;
        UpdBatchResult br = UpdRunBatch(four, [&](const UpdListedPackage&) { ++ran; return true; }, [&] { return ran >= 2; }, [] { return false; });
        Check(ran == 2 && br.ok == 2 && br.cancelled && br.notStarted == 2 && br.bad == 0, "updater cancel: the request is honoured between packages; the running one finished, the rest never started");
        Check(UpdBatchText(br).find(L"cancelled: 2 not started") != std::wstring::npos && UpdBatchText(br).find(L"cannot be interrupted safely") != std::wstring::npos, "updater cancel: the report says what was cancelled and that a running installer is not interrupted");
        ran = 0; br = UpdRunBatch(four, [&](const UpdListedPackage&) { ++ran; return true; }, [] { return true; }, [] { return false; });
        Check(ran == 0 && br.cancelled && br.notStarted == 4 && br.ok == 0, "updater cancel: cancelled before the first package, nothing runs");
        ran = 0; br = UpdRunBatch(four, [&](const UpdListedPackage&) { return ++ran != 2; }, [] { return false; }, [&] { return ran == 2; });
        Check(br.ok == 1 && br.bad == 1 && br.stoppedId == "b.b" && br.notStarted == 2 && !br.cancelled && UpdBatchText(br).find(L"stopped at b.b") != std::wstring::npos, "updater batch: an update left in an unknown state stops the run and says where");
        ran = 0; br = UpdRunBatch(four, [&](const UpdListedPackage&) { return ++ran != 2; }, [] { return false; }, [] { return false; });
        Check(br.ok == 3 && br.bad == 1 && br.notStarted == 0 && br.stoppedId.empty(), "updater batch: a package that failed cleanly does not stop the next one");
        UpdUiModel um([](std::wstring& why) -> std::shared_ptr<UgSession> { why = L"none"; return nullptr; }, {}, {}, AcbRoots{});
        Check(!um.RequestCancel() && !um.Snapshot().batchActive, "updater cancel: with no Update all running there is nothing to cancel");
        um.BeginBatch(); Check(um.Snapshot().batchActive && !um.CancelRequested(), "updater cancel: a started batch is not cancelled");
        Check(um.RequestCancel() && um.CancelRequested() && um.Snapshot().cancelRequested, "updater cancel: the request is recorded");
        um.EndBatch(); Check(!um.Snapshot().batchActive && !um.CancelRequested(), "updater cancel: ending the batch clears the request so the next Update all starts normally");
    }
    {   // ---- 2.8.3: System Care action bar
        auto mkf = [](CareKind k, const wchar_t* id, CareClass c, uint64_t b) { CareFinding f; f.kind = k; f.id = id; f.title = id; f.cls = c; f.bytes = b; return f; };
        CareView v; v.phase = 2;
        v.findings = {mkf(CareKind::CleanerItem, L"usertemp", CareClass::Cleanable, 1000), mkf(CareKind::BrokenShortcut, L"C:\\Users\\x\\a.lnk", CareClass::Cleanable, 0), mkf(CareKind::CleanerItem, L"prefetch", CareClass::Review, 50)};
        v.chosen = {L"usertemp"};
        CareBarState b = CareBarAssess(v, 0, false, false, false, {});
        Check(b.scanned && b.chosen == 1 && b.chosenBytes == 1000 && b.fixIds.size() == 1 && b.canClean && b.canFixAll && b.cleanWhy.empty(), "care bar: the selected count and size, the fixable shortcuts, and both actions on after a scan");
        v.chosen.clear(); b = CareBarAssess(v, 0, false, false, false, {});
        Check(!b.canClean && b.cleanWhy.find(L"Nothing is selected") != std::wstring::npos && b.canFixAll, "care bar: nothing selected switches Clean selected off and says why; Fix all stays available");
        b = CareBarAssess(v, 0, false, false, false, {L"c:\\users\\x\\a.lnk"});
        Check(b.fixIds.empty() && !b.canFixAll && b.fixWhy.find(L"No program update") != std::wstring::npos, "care bar: an ignored shortcut is never fixed; with nothing to do, Fix all is off");
        b = CareBarAssess(v, 3, false, false, false, {L"c:\\users\\x\\a.lnk"});
        Check(b.canFixAll && b.updates == 3, "care bar: pending program updates alone enable Update all");
        b = CareBarAssess(v, 3, false, true, false, {L"c:\\users\\x\\a.lnk"});
        Check(!b.canFixAll && b.fixWhy.find(L"unfinished update") != std::wstring::npos, "care bar: an unfinished update blocks Update all and the reason says so");
        CareView sc = v; sc.phase = 1; sc.chosen = {L"usertemp"};
        b = CareBarAssess(sc, 3, false, false, false, {});
        Check(!b.canClean && !b.canFixAll && b.cleanWhy.find(L"scan is running") != std::wstring::npos, "care bar: nothing is cleaned while scanning");
        CareView cl = v; cl.phase = 3; b = CareBarAssess(cl, 3, false, false, false, {});
        Check(!b.canClean && !b.canFixAll, "care bar: nothing starts while a cleanup is running");
        CareView idle; b = CareBarAssess(idle, 3, false, false, false, {});
        Check(!b.scanned && !b.canClean && !b.canFixAll && b.cleanWhy.find(L"Smart scan") != std::wstring::npos, "care bar: before the first scan both actions are off");
        b = CareBarAssess(v, 3, true, false, false, {}); Check(!b.canFixAll, "care bar: not while an update is running");
        b = CareBarAssess(v, 3, false, false, true, {}); Check(!b.canFixAll && !b.canClean, "care bar: not while another operation is running");
        // Fix all goes through the SAME execution engine and leaves the person's own ticks alone
        CareMgr cm;
        std::vector<CareFinding> found = v.findings;
        cm.StartScan([&](const std::function<void(size_t, size_t, const std::wstring&)>&, const std::atomic<bool>&) { return found; }, nullptr);
        for (int i = 0; i < 100 && cm.Snapshot().phase < 2; ++i) Sleep(20);
        cm.Toggle(L"prefetch", true);   // the person ticked a Review item themselves
        std::set<std::wstring> cleaned;
        CareBackend be; be.exists = [](const CareFinding&) { return true; }; be.measure = [](const CareFinding&) -> uint64_t { return 10; };
        be.clean = [&](const CareFinding& f) -> uint64_t { cleaned.insert(f.id); return 10; };
        be.moveToRecovery = [&](const CareFinding& f, const std::wstring&, std::wstring& why) { cleaned.insert(f.id); why = L"moved"; return true; };
        const std::set<std::wstring> only = {L"C:\\Users\\x\\a.lnk"};
        Check(cm.StartClean({}, be, [](const CareFinding&) { return std::wstring(L"C:\\rec"); }, nullptr, &only), "care bar: Fix all starts the cleanup engine with only the shortcut ids");
        for (int i = 0; i < 100 && cm.Snapshot().phase != 4; ++i) Sleep(20);
        const CareView after = cm.Snapshot();
        Check(cleaned == only && after.chosen.count(L"usertemp") && after.chosen.count(L"prefetch") && after.confirmed.count(L"prefetch"), "care bar: Fix all cleaned only the shortcut; the person's own ticks (and the Review confirmation) are still there");
        const std::set<std::wstring> none;
        CareMgr cm2; cm2.StartScan([&](const std::function<void(size_t, size_t, const std::wstring&)>&, const std::atomic<bool>&) { return found; }, nullptr);
        for (int i = 0; i < 100 && cm2.Snapshot().phase < 2; ++i) Sleep(20);
        Check(!cm2.StartClean({}, be, nullptr, nullptr, &none), "care bar: an empty Fix all list starts nothing");
    }
    {   // ---- 2.8.3: PowerToys icons, Windhawk attribution
        bool all = true, distinct = true; std::set<std::wstring> seen;
        for (const auto& m : PtModules()) { const std::wstring g = PtIconOf(m.key); all = all && g != L"\xE8B0" && g.size() == 1; seen.insert(g); }
        distinct = seen.size() >= PtModules().size() - 1;   // every utility has its own symbol
        Check(all && distinct, "powertoys 2.8.3: no utility shows the generic mouse icon and the symbols are distinct");
        Check(std::wstring(PtIconOf(L"FancyZones")) != std::wstring(PtIconOf(L"Awake")) && std::wstring(PtIconOf(L"Nope")) == L"\xE8B0", "powertoys 2.8.3: an unknown key falls back to the generic icon");
        Check(WhIsModifiedByAnkhCore(L"local@icon-resource-redirect") && WhIsModifiedByAnkhCore(L"icon-resource-redirect") && WhIsModifiedByAnkhCore(L"windows-11-file-explorer-styler") && !WhIsModifiedByAnkhCore(L"windows-11-start-menu-styler") && !WhIsModifiedByAnkhCore(L"mouse-trail"),
              "windhawk 2.8.3: exactly Resource Redirect and the File Explorer Styler are marked as modified");
        Check(WhModifiedNote(L"windows-11-file-explorer-styler") == L"Modified by As3aD" && WhModifiedNote(L"mouse-trail").empty(), "windhawk 2.8.3: the visible note is 'Modified by As3aD' and only for those two");
    }
    {   // ---- 2.8.3: Explorer restarts only after a successful icon apply
        int restarts = 0; std::atomic<bool> busy{false}; std::vector<std::wstring> lines;
        auto restart = [&] { ++restarts; return true; };
        auto st = [&](const std::wstring& t) { lines.push_back(t); };
        Check(IconRestartAfterApply(true, restart, busy, st) == IconAutoRestart::Done && restarts == 1 && lines.size() == 1 && !busy, "icons 2.8.3: a successful apply restarts Explorer once and reports progress");
        Check(IconRestartAfterApply(false, restart, busy, st) == IconAutoRestart::SkippedApplyFailed && restarts == 1, "icons 2.8.3: a failed apply never restarts Explorer");
        busy = true;
        Check(IconRestartAfterApply(true, restart, busy, st) == IconAutoRestart::SkippedAlreadyRestarting && restarts == 1 && busy, "icons 2.8.3: a second request while a restart is running is dropped, not queued");
        busy = false;
        Check(IconRestartAfterApply(true, [] { return false; }, busy, st) == IconAutoRestart::RestartFailed && !busy, "icons 2.8.3: a restart that failed is reported (the apply itself stays successful) and the guard is released");
        Check(std::wstring(IconAutoRestartText(IconAutoRestart::RestartFailed)).find(L"Restart Explorer") != std::wstring::npos, "icons 2.8.3: the failure text points to the manual recovery button");
        {   // 2.8.6: the restart waits for the apply to settle, counted down, and never before the wait is over
            int slept = 0, restartsAtLastSleep = -1, r2 = 0; busy = false; lines.clear();
            auto restart2 = [&] { ++r2; return true; };
            auto sl = [&](int ms) { slept += ms; restartsAtLastSleep = r2; };
            Check(IconRestartAfterApply(true, restart2, busy, st, 3, sl) == IconAutoRestart::Done && slept == 3000 && restartsAtLastSleep == 0 && r2 == 1 && lines.size() == 4, "icons 2.8.6: Explorer restarts only after the settle wait (3 s counted down, then one restart)");
            slept = 0; r2 = 0; busy = false;
            Check(IconRestartAfterApply(false, restart2, busy, st, 3, sl) == IconAutoRestart::SkippedApplyFailed && slept == 0 && r2 == 0, "icons 2.8.6: a failed apply neither waits nor restarts");
            Check(kIconRestartSettleSeconds >= 5, "icons 2.8.6: the real settle wait is several seconds");
            std::vector<int> ev; ExplorerRestartHook() = [&](bool s) { ev.push_back(s ? 1 : 0); };
            { ExplorerRestartScope sc; ev.push_back(2); }
            ExplorerRestartHook() = nullptr;
            Check(ev == std::vector<int>({1, 2, 0}), "sidebar 2.8.6: the app is told before Explorer is restarted and again after it is back (the Sidebar waits for the new shell)");
        }
    }
    {   // ---- 2.8.3: theme OS compatibility
        const std::string plain = "; Copyright niivu DESIGNS.\n[Theme]\nDisplayName=BIB Night - Arc\n";
        ThemeOsInfo t = ClassifyThemeOs(L"BIB", L"BIB Night - Arc", L"BIB Night - Arc", plain);
        Check(t.os == ThemeOs::Windows11 && !t.source.empty(), "theme os: BIB is a Windows 11 pack");
        Check(ClassifyThemeOs(L"BIB3", L"BIB3 LIN NIGHT", L"BIB3 LIN NIGHT", plain).os == ThemeOs::Windows11, "theme os: BIB3 is a Windows 11 pack");
        t = ClassifyThemeOs(L"After Dark", L"After Dark - Blue", L"After Dark - Blue", plain);
        Check(t.os == ThemeOs::Windows10, "theme os: After Dark is a Windows 10 pack");
        Check(ClassifyThemeOs(L"", L"AfterDark Red", L"AfterDark Red", plain).os == ThemeOs::Windows10, "theme os: After Dark is recognised from the theme name when it sits in the root folder");
        t = ClassifyThemeOs(L"Custom", L"Whatever", L"Whatever", plain);
        Check(t.os == ThemeOs::Unknown && t.source.find(L"no OS requirement") != std::wstring::npos, "theme os: a theme with no evidence is reported as unknown, not guessed");
        Check(ClassifyThemeOs(L"BIB", L"BIB X", L"BIB X", "; AnkhCore-OS: Windows 10\n[Theme]\nDisplayName=x\n").os == ThemeOs::Windows10, "theme os: a declaration in the package beats the family name");
        Check(ClassifyThemeOs(L"BIB", L"BIB X", L"BIB X", "[Theme]\nDisplayName=x\n[AnkhCore]\nOS=Both\n").os == ThemeOs::Both, "theme os: an [AnkhCore] OS= declaration is read");
        Check(ClassifyThemeOs(L"Custom", L"Y", L"Y", "; Made for Windows 11 only\n[Theme]\nDisplayName=y\n").os == ThemeOs::Windows11, "theme os: a header that names exactly one Windows is used");
        Check(ClassifyThemeOs(L"Custom", L"Y", L"Y", "; Windows 10 and Windows 11\n[Theme]\nDisplayName=y\n").os == ThemeOs::Unknown, "theme os: a header that names both Windows versions is not treated as evidence for one");
        Check(ClassifyThemeOs(L"Custom", L"Y", L"Y", "[Theme]\n; Windows 10\nDisplayName=y\n").os == ThemeOs::Unknown, "theme os: text after the first section is not header metadata");
        Check(!ThemeApplyCheckFor(ThemeOs::Windows11, 19045, true).allowed && !ThemeApplyCheckFor(ThemeOs::Windows10, 26100, true).allowed, "theme os: a theme for the other Windows is refused");
        ThemeApplyCheck ok11 = ThemeApplyCheckFor(ThemeOs::Windows11, 26100, false);
        Check(ok11.allowed && !ok11.uncertain && ok11.reason.empty(), "theme os: a Windows 11 theme on Windows 11 is allowed");
        ThemeApplyCheck unk = ThemeApplyCheckFor(ThemeOs::Unknown, 26100, false);
        Check(unk.allowed && unk.uncertain && unk.reason.find(L"unknown") != std::wstring::npos, "theme os: unknown compatibility is allowed but reported as uncertain");
        ThemeApplyCheck w10 = ThemeApplyCheckFor(ThemeOs::Windows10, 19045, false);
        Check(!w10.allowed && w10.reason.find(L"SecureUxTheme") != std::wstring::npos, "theme os: a Windows 10 theme on Windows 10 is compatible, but is refused honestly while the applier is Windows 11 only");
        Check(ThemeFitsHostOs(ThemeOs::Windows10, 19045) && !ThemeFitsHostOs(ThemeOs::Windows10, 26100) && ThemeFitsHostOs(ThemeOs::Windows11, 26100) && ThemeFitsHostOs(ThemeOs::Both, 19045) && ThemeFitsHostOs(ThemeOs::Unknown, 19045), "theme os: the 'for this PC' filter");
        const std::wstring themesDir = L"D:\\Ankhcore\\windows\\Resources\\Themes";
        if (DirExists(themesDir)) {
            const std::vector<DiscoveredWinTheme> found = DiscoverAuthoritativeThemes(themesDir);
            bool bibOk = !found.empty();
            for (const auto& th : found) bibOk = bibOk && (th.os == ThemeOs::Windows11);
            Check(bibOk, "theme os: every theme of the real BIB / BIB3 collection is classified Windows 11");
        }
    }
    {   // ---- 2.8.3: scan button state and dashboard tones
        Check(ScanStateOf(true, true, 0) == ScanButtonState::Scanning && ScanStateOf(false, false, 0) == ScanButtonState::Unknown && ScanStateOf(false, true, 0) == ScanButtonState::Good && ScanStateOf(false, true, 1) == ScanButtonState::Info
              && ScanStateOf(false, true, 2) == ScanButtonState::Warning && ScanStateOf(false, true, 3) == ScanButtonState::Error, "scan button: the state follows the real health result (scanning, none, good, note, warning, problem)");
        Check(ScanStateOf(false, false, 3) == ScanButtonState::Unknown, "scan button: before a valid scan the score is ignored");
        Check(ToneWindows(26100) == Tone::Good && ToneWindows(19045) == Tone::Warning && ToneWindows(0) == Tone::Unknown, "dashboard: Windows 11 is good, Windows 10 warns, no data is unknown");
        Check(ToneUptime(0) == Tone::Unknown && ToneUptime(3600000ull) == Tone::Good && ToneUptime(kUptimeWarnMs + 1) == Tone::Warning && ToneUptime(kUptimeWarnMs) == Tone::Good, "dashboard: uptime is good under 7 days, warns above, unknown without data");
        Check(ToneSystemDisk(false, true, L"OK", 50) == Tone::Unknown && ToneSystemDisk(true, true, L"OK", 50) == Tone::Good && ToneSystemDisk(true, true, L"OK", 10) == Tone::Warning && ToneSystemDisk(true, true, L"OK", 3) == Tone::Error
              && ToneSystemDisk(true, false, L"Failing", 50) == Tone::Error && ToneSystemDisk(true, true, L"Unknown", -1) == Tone::Unknown && ToneSystemDisk(true, true, L"Unknown", 50) == Tone::Good, "dashboard: the system disk tone follows health and free space like the health scan");
    }
    {   // ---- 2.8.3: Setup: keep or replace PowerToys / Windhawk that are already installed
        Check(PlanToolInstall(false, false) == ToolPlan::InstallNew && PlanToolInstall(false, true) == ToolPlan::InstallNew, "setup 2.8.3: a tool that is not installed is simply installed");
        Check(PlanToolInstall(true, false) == ToolPlan::KeepExisting, "setup 2.8.3: an installed tool is kept by default (nothing is removed unless the person said so)");
        Check(PlanToolInstall(true, true) == ToolPlan::ReplaceClean, "setup 2.8.3: replacing is only when the person chose it");
        Check(FanFloorRouteOf(true, true, false, false, false, false) == FanFloorRoute::Direct && FanFloorRouteOf(true, false, true, true, true, true) == FanFloorRoute::Nbfc && FanFloorRouteOf(true, false, true, false, true, true) == FanFloorRoute::None
              && FanFloorRouteOf(true, false, false, false, true, true) == FanFloorRoute::OwnEngine && FanFloorRouteOf(true, false, false, false, true, false) == FanFloorRoute::None && FanFloorRouteOf(true, false, false, false, false, true) == FanFloorRoute::None && FanFloorRouteOf(false, true, true, true, true, true) == FanFloorRoute::None,
              "fan 2.8.6: the minimum-speed setting is available through direct control, NBFC with a configuration, or AnkhCore's own engine for a usable configuration; never when control is off");
        {   // 2.8.6: the programs list
            const std::wstring tmp = std::wstring(_wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L"C:\\Windows\\Temp") + L"\\ak_pl_test";
            { std::error_code ec; std::filesystem::remove_all(tmp, ec); } EnsureDir(tmp);
            const std::string two = "{\"Sources\":[{\"Packages\":[{\"PackageIdentifier\":\"A.B\"},{\"PackageIdentifier\":\"C.D\"}]}]}";
            Check(ProgListCount(two) == 2 && ProgListCount("{}") == 0 && ProgListCount("") == 0, "programs 2.8.6: the size of a list is the number of PackageIdentifier entries");
            WriteAll(tmp + L"\\" + ProgListFileName(L"20261120-010101"), reinterpret_cast<const uint8_t*>(two.data()), two.size());
            WriteAll(tmp + L"\\" + ProgListFileName(L"20261121-020202"), reinterpret_cast<const uint8_t*>(two.data()), two.size());
            WriteAll(tmp + L"\\Programs-20261122-empty.json", reinterpret_cast<const uint8_t*>("{}"), 2);
            const auto ls = ProgListFiles(tmp);
            Check(ls.size() == 2 && ls[0].name == ProgListFileName(L"20261121-020202") && ls[0].count == 2, "programs 2.8.6: saved lists are listed newest first, and an empty file is not offered as a list");
            Check(ProgExportArgs(L"C:\\x y\\p.json").find(L"export -o \"C:\\x y\\p.json\"") == 0 && ProgImportArgs(L"C:\\p.json").find(L"--ignore-unavailable") != std::wstring::npos && ProgImportArgs(L"C:\\p.json").find(L"--disable-interactivity") != std::wstring::npos,
                  "programs 2.8.6: export and import use quoted paths, never ask questions, and skip what winget cannot find");
            { std::error_code ec; std::filesystem::remove_all(tmp, ec); }
        }
        {   // 2.8.7: cancel of a running update, and the installed list that lags behind
            const std::vector<std::wstring> opl = {L"{\"status\":\"success\",\"operations\":[{\"id\":7,\"title\":\"Git.Git update\",\"status\":\"running\"},{\"id\":8,\"title\":\"Other.App\",\"status\":\"queued\"},{\"id\":\"9\",\"description\":\"git.git again\"}]}"};
            const auto ids = UpdFindOperationIds(opl, "Git.Git");
            Check(ids.size() == 2 && ids[0] == 7 && ids[1] == 9, "upd 2.8.7: only the operations that mention the package are found (numeric or text ids)");
            Check(UpdFindOperationIds(opl, "Nope.App").empty() && UpdFindOperationIds({L"not json"}, "Git.Git").empty() && UpdFindOperationIds(opl, "").empty(), "upd 2.8.7: no match, bad output or an empty id cancels nothing");
            std::vector<std::wstring> cmds; bool listOk = true, cancelOk = true;
            auto run = [&](const std::wstring& a, std::vector<std::wstring>& out) -> DWORD {
                cmds.push_back(a);
                if (a == L"operation list") { out = opl; return listOk ? 0 : 2; }
                if (a.rfind(L"operation cancel", 0) == 0) return cancelOk ? 0 : 2;
                return 0;
            };
            const UpdProvider pr = UpdMakeUniGetProviderWith(true, run);
            UpdPackage gp; gp.id = "Git.Git"; gp.manager = "winget";
            std::wstring note;
            Check(pr.cancelRunning && pr.cancelRunning(gp, note) && cmds.size() == 3 && cmds[0] == L"operation list" && cmds[1] == L"operation cancel --id 7" && cmds[2] == L"operation cancel --id 9",
                  "upd 2.8.7: Cancel lists the operations and cancels exactly the ones of this program");
            cmds.clear(); listOk = false;
            Check(!pr.cancelRunning(gp, note) && cmds.size() == 1 && note.find(L"nothing was cancelled") != std::wstring::npos, "upd 2.8.7: if the operations cannot be listed nothing is cancelled");
            cmds.clear(); listOk = true; UpdPackage other = gp; other.id = "Zed.Zed";
            Check(!pr.cancelRunning(other, note) && cmds.size() == 1, "upd 2.8.7: a program without a running operation is not cancelled");
            cmds.clear(); cancelOk = false;
            Check(!pr.cancelRunning(gp, note) && note.find(L"refused") != std::wstring::npos, "upd 2.8.7: a refused cancel is reported, not claimed");
            cmds.clear(); pr.refreshInstalled(gp);
            Check(cmds.size() == 1 && cmds[0].find(L"manager reload --manager") == 0, "upd 2.8.7: the installed list is refreshed with manager reload");
        }
        Check(IsWindhawkDesktopShortcut(L"Windhawk.lnk") && IsWindhawkDesktopShortcut(L"windhawk (2).LNK") && !IsWindhawkDesktopShortcut(L"Windhawk.txt") && !IsWindhawkDesktopShortcut(L"MyWindhawk.lnk") && !IsWindhawkDesktopShortcut(L"Chrome.lnk") && !IsWindhawkDesktopShortcut(L".lnk"),
              "setup 2.8.7: only a shortcut named Windhawk*.lnk counts as Windhawk's desktop icon");
        Check(PlanNbfcServiceStart(false, true) == NbfcSvcAction::StartService, "fan 2.8.5: a registered NBFC service that is not running is started through the service manager");
        Check(PlanNbfcServiceStart(true, true) == NbfcSvcAction::None && PlanNbfcServiceStart(false, false) == NbfcSvcAction::None && PlanNbfcServiceStart(true, false) == NbfcSvcAction::None,
              "fan 2.8.5: nothing is started when the service runs or is not registered; NbfcService.exe is never run directly (it shows the 'Windows Service Start Failure' dialog)");
    }
    {   // ---- 2.8.4: After Dark packs chosen by the Windows build
        Check(AdReleaseOf(19045) == L"22H2" && AdReleaseOf(19044) == L"21H2" && AdReleaseOf(18363) == L"1909" && AdReleaseOf(14393) == L"1607" && AdReleaseOf(22631).empty() && AdReleaseOf(19046).empty() && AdReleaseOf(0).empty(),
              "afterdark: a Windows build maps to its Windows 10 release; Windows 11 and unknown builds map to nothing");
        Check(AdReleases().size() == 12, "afterdark: twelve Windows 10 releases are known (1607 to 22H2)");
        Check(AdPatchSupported(17763) && AdPatchSupported(19045) && !AdPatchSupported(17134) && !AdPatchSupported(22000), "afterdark: the theme patch is set up from 1809 on, Windows 10 only");
        const std::wstring tmp = std::wstring(_wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L"C:\\Windows\\Temp") + L"\\ak_ad_test";
        { std::error_code ec; std::filesystem::remove_all(tmp, ec); }
        const std::wstring root = tmp + L"\\AfterDark";
        auto mk = [&](const std::wstring& rel, const char* txt) { EnsureDir(DirOf(root + L"\\" + rel)); WriteAll(root + L"\\" + rel, reinterpret_cast<const uint8_t*>(txt), strlen(txt)); };
        // 22H2: standard pack (both variants) + an Office-fix pack; 1607: Office-fix pack only; 2004: standard pack with only one variant
        for (const wchar_t* v : {L"Show Commandbar", L"Hide Commandbar"}) {
            const std::wstring b = L"Theme For Win10 22H2\\" + std::wstring(v);
            mk(b + L"\\After Dark CC.theme", "t"); mk(b + L"\\After Dark CC\\After Dark CC1.msstyles", "m"); mk(b + L"\\After Dark CC\\shell\\NormalColor\\shellstyle.dll", "s"); mk(b + L"\\After Dark CC\\Wallpaper\\Thumbs.db", "x");
            mk(L"Theme Fix Background White For Office\\Theme For Win10 22H2\\" + std::wstring(v) + L"\\After Dark CC.theme", "o");
        }
        mk(L"Theme Fix Background White For Office\\For Win10 1607\\Show Commandbar\\After Dark CC.theme", "o");
        mk(L"Theme For Win10 2004\\Hide Commandbar\\After Dark CC.theme", "t");
        mk(L"Extras\\Fix bug white color after sleep for theme dark\\FIX.reg", "f"); mk(L"Extras\\Fix bug white color after sleep for theme dark\\UNFIX (restore defaults).reg", "u");
        AdPack p = AdFindPack(root, 19045);
        Check(p.found && p.release == L"22H2" && p.packRel == L"Theme For Win10 22H2" && !p.officeOnly && p.officeAvailable && p.officeRel == L"Theme Fix Background White For Office\\Theme For Win10 22H2" && p.variants.size() == 2 && p.sleepFix,
              "afterdark: 22H2 finds the standard pack, the Office-fix pack, both command-bar variants and the sleep fix");
        std::vector<AdFile> plan = AdPlanFiles(root, p, false, L"Show Commandbar");
        std::set<std::wstring> dst; for (const auto& f : plan) dst.insert(f.dstRel);
        Check(dst.count(L"After Dark CC.theme") && dst.count(L"After Dark CC\\After Dark CC1.msstyles") && dst.count(L"After Dark CC\\shell\\NormalColor\\shellstyle.dll") && !dst.count(L"After Dark CC\\Wallpaper\\Thumbs.db") && plan.size() == 3,
              "afterdark: the plan copies the .theme file and the whole 'After Dark CC' folder (without Thumbs.db) into Windows' Themes folder");
        plan = AdPlanFiles(root, p, true, L"Hide Commandbar");
        Check(plan.size() == 1 && plan[0].srcRel.find(L"Theme Fix Background White For Office") == 0, "afterdark: the Office-fix option takes the Office-fix pack");
        Check(AdPlanFiles(root, p, false, L"Nope").empty() && AdPlanFiles(root, AdPack{}, false, L"Show Commandbar").empty(), "afterdark: an unknown variant, or no pack, plans nothing");
        Check(AdComponentDir(p, false, L"Show Commandbar") == L"AfterDark\\Theme For Win10 22H2\\Show Commandbar" && AdComponentDir(p, true, L"Hide Commandbar").find(L"AfterDark\\Theme Fix Background White For Office") == 0, "afterdark: Setup verifies exactly the folder it will copy from");
        p = AdFindPack(root, 14393);
        Check(p.found && p.officeOnly && !p.officeAvailable && p.variants.size() == 1 && p.variants[0] == L"Show Commandbar", "afterdark: 1607 only has the Office-fix pack: it is the theme, and only its existing variant is offered");
        plan = AdPlanFiles(root, p, true, L"Show Commandbar"); Check(plan.size() == 1, "afterdark: the only pack of an old release can be planned");
        p = AdFindPack(root, 19041);
        Check(p.found && p.variants.size() == 1 && p.variants[0] == L"Hide Commandbar" && !p.officeAvailable, "afterdark: a release with one variant offers only that variant");
        p = AdFindPack(root, 19043);
        Check(!p.found && p.reason.find(L"21H1") != std::wstring::npos, "afterdark: a release without a pack says so and installs nothing");
        p = AdFindPack(root, 22631);
        Check(!p.found && p.reason.find(L"Windows 11") != std::wstring::npos, "afterdark: Windows 11 gets no After Dark pack");
        p = AdFindPack(root, 12345);
        Check(!p.found && p.reason.find(L"not one of the releases") != std::wstring::npos, "afterdark: an unknown Windows 10 build gets no pack (no guessing)");
        Check(!AdFindPack(tmp + L"\\missing", 19045).found, "afterdark: a package without the folder gets no pack");
        { std::error_code ec; std::filesystem::remove_all(tmp, ec); }
        const std::wstring realRoot = L"D:\\Ankhcore\\windows\\Resources\\after-dark";
        if (DirExists(realRoot)) {
            bool all = true; std::wstring badRel;
            for (const auto& r : AdReleases()) {
                const AdPack rp = AdFindPack(realRoot, r.build);
                const bool ok = rp.found && !rp.variants.empty() && !AdPlanFiles(realRoot, rp, false, rp.variants[0]).empty();
                if (!ok) { all = false; badRel = r.name; }
            }
            Check(all, std::string("afterdark: every Windows 10 release has a pack in the real After Dark folder (missing: ") + WToUtf8(badRel) + ")");
            const AdPack r22 = AdFindPack(realRoot, 19045);
            Check(r22.found && r22.variants.size() == 2 && AdPlanFiles(realRoot, r22, false, L"Show Commandbar").size() >= 10 && r22.sleepFix, "afterdark: the real 22H2 pack has both variants, its files and the sleep fix");
        }
        Check(ThemeApplyCheckFor(ThemeOs::Windows10, 19045, true).allowed && !ThemeApplyCheckFor(ThemeOs::Windows10, 26100, true).allowed && !ThemeApplyCheckFor(ThemeOs::Windows11, 19045, true).allowed, "afterdark: with the applier available a Windows 10 theme is allowed on Windows 10 only");
        Check(ClassifyThemeOs(L"After Dark CC", L"After Dark CC", L"After Dark CC", "; Copyright Microsoft Corp.\n[Theme]\nDisplayName=After Dark CC\n").os == ThemeOs::Windows10, "afterdark: the installed After Dark themes are classified Windows 10 from their folder");
    }
    std::printf("%s (%d passed)\n", fails ? "FAILED" : "ALL PASSED", passes);
    return fails ? 1 : 0;
}
