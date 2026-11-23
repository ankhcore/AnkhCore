// FT-RH measurement (not a pass/fail test): what the Recovery History costs before and after the fix. Isolated: it is built as a
// test build (ANKH_TEST_BUILD), so the journal and the Recovery folder it writes are the test build's own under
// %LOCALAPPDATA%\AnkhCore-Test, never the installed AnkhCore's. The test journal is saved first and put back at the end.
//   bench_recovery_history.exe [operations] [entries per operation]
#include "base.h"
#include "eng_backup.h"
#ifdef RH_AFTER
#include "journal_view.h"
#endif
#include <chrono>
#include <cstdio>
using namespace ak;

static double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int ops = argc > 1 ? _wtoi(argv[1]) : 200, per = argc > 2 ? _wtoi(argv[2]) : 18;
    const std::wstring jp = JournalPath(), keep = jp + L".bench-saved";
    const bool had = FileExists(jp);
    if (had) CopyFileW(jp.c_str(), keep.c_str(), FALSE);
    DeleteFileW(jp.c_str());
    std::printf("mode %s, %d operations x %d entries = %d journal entries (isolated test journal)\n",
#ifdef RH_AFTER
                "AFTER",
#else
                "BEFORE",
#endif
                ops, per, ops * per);

    // 1. recording: one cursor-scheme-like operation writes `per` values, one journal entry each
    auto t0 = std::chrono::steady_clock::now(); double lastOpMs = 0;
    for (int o = 0; o < ops; ++o) {
        const auto t1 = std::chrono::steady_clock::now();
        const std::wstring title = L"Mouse cursors: scheme " + std::to_wstring(o % 7);
#ifdef RH_AFTER
        JournalOpScope scope(title);
#endif
        for (int i = 0; i < per; ++i) JournalAdd(L"cursors", L"REG", title, {L"HKCU", L"Control Panel\\Cursors", L"Role" + std::to_wstring(i), L"1", L"2", L"2500530079007300740065006d0052006f006f00740025005c000000"});
        lastOpMs = Ms(t1);
    }
    std::printf("record: %.0f ms in total; the last operation (%d entries) took %.1f ms\n", Ms(t0), per, lastOpMs);
    std::printf("journal file: %llu bytes\n", static_cast<unsigned long long>(FolderSize(DirOf(jp)) ? [&] { WIN32_FILE_ATTRIBUTE_DATA a{}; GetFileAttributesExW(jp.c_str(), GetFileExInfoStandard, &a); return (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow; }() : 0ull));

    // 2. what the page needs per refresh and per paint
    t0 = std::chrono::steady_clock::now();
    std::vector<JEntry> all = JournalLoad();
    std::printf("load (refresh after every operation): %.1f ms for %zu entries\n", Ms(t0), all.size());
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) { std::vector<JEntry> copy = all; if (copy.size() == 7) std::printf("x"); }
    std::printf("copy of the journal inside Snapshot() (every paint, before): %.2f ms per copy\n", Ms(t0) / 20);
#ifdef RH_AFTER
    t0 = std::chrono::steady_clock::now();
    const JournalView view = JournalBuildView(all);
    std::printf("group into operations (done once per refresh): %.2f ms -> %zu operation row(s) for %zu entries\n", Ms(t0), view.ops.size(), all.size());
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) { auto shared = std::make_shared<const JournalView>(view); if (!shared) std::printf("x"); }
    std::printf("hand the view to a paint (shared, no copy): %.4f ms\n", Ms(t0) / 20);
    std::printf("rows the page draws per paint: %zu (of %zu operations; the rest on 'Show more')\n", std::min<size_t>(view.ops.size(), 25), view.ops.size());
#else
    std::printf("rows the page draws per paint: %zu (one per journal entry, newest 80)\n", std::min<size_t>(all.size(), 80));
#endif

    // 3. recovery points listed on every paint
    AcbRoots roots = AcbDefaultRoots();
    const std::wstring rec = roots.data + L"\\Recovery";
    const int points = 40;
    for (int p = 0; p < points; ++p) {
        const std::wstring d = rec + L"\\bench-" + std::to_wstring(1000 + p);
        EnsureDir(d + L"\\files\\settings");
        const std::string man = "{\"created\":\"2026-11-15T00:00:00Z\",\"sourceArchive\":\"bench\",\"items\":[{\"dest\":\"appdata:settings.json\"}]}";
        WriteAll(d + L"\\recovery-manifest.json", reinterpret_cast<const uint8_t*>(man.data()), man.size());
        WriteAll(d + L"\\restore-report.txt", reinterpret_cast<const uint8_t*>("Status: PRE-CHANGE"), 18);
        std::vector<uint8_t> blob(20000, 7); for (int f = 0; f < 6; ++f) WriteAll(d + L"\\files\\settings\\s" + std::to_wstring(f) + L".json", blob.data(), blob.size());
    }
    t0 = std::chrono::steady_clock::now();
    size_t n = 0; for (int i = 0; i < 10; ++i) n = AcbListRecoveryPoints(roots).size();
    std::printf("list %zu recovery points (scan, manifests, sizes): %.1f ms per call (the page called this on every paint, before)\n", n, Ms(t0) / 10);

    // clean up what the bench made, restore the test journal
    for (int p = 0; p < points; ++p) Evict(rec + L"\\bench-" + std::to_wstring(1000 + p), true, false);
    DeleteFileW(jp.c_str());
    if (had) { CopyFileW(keep.c_str(), jp.c_str(), FALSE); DeleteFileW(keep.c_str()); }
    return 0;
}
