// Isolated WT-02 unit tests: CleanItems() discovery, IsProtectedCleanupPath(),
// SafeCleanDir() dry-run, locked-file skipping, and junction/symlink boundary safety.
// Uses only an isolated sandbox under DataDir(); never touches real user/system files.
#define ANKH_TEST_BUILD 1
#include "eng_system.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <stdexcept>

static bool WriteDummyFile(const std::wstring& path, const char* data, DWORD len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, data, len, &w, nullptr) && w == len;
    CloseHandle(h);
    return ok;
}

// The isolated test build refuses ak::Run, so "mklink /J" never ran in the first version of this test and the junction checks were
// vacuous. A directory junction is created natively instead (FSCTL_SET_REPARSE_POINT, mount-point reparse data).
static bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    const std::wstring sub = L"\\??\\" + target, print = target;
    const size_t subBytes = sub.size() * 2, printBytes = print.size() * 2;
    std::vector<unsigned char> buf(8 + 8 + subBytes + 2 + printBytes + 2, 0);
    auto put16 = [&](size_t at, unsigned v) { buf[at] = static_cast<unsigned char>(v & 0xFF); buf[at + 1] = static_cast<unsigned char>(v >> 8); };
    const unsigned dataLen = static_cast<unsigned>(8 + subBytes + 2 + printBytes + 2);
    buf[0] = 0x03; buf[1] = 0x00; buf[2] = 0x00; buf[3] = 0xA0;   // IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003
    put16(4, dataLen);
    put16(8, 0); put16(10, static_cast<unsigned>(subBytes));
    put16(12, static_cast<unsigned>(subBytes + 2)); put16(14, static_cast<unsigned>(printBytes));
    memcpy(&buf[16], sub.c_str(), subBytes);
    memcpy(&buf[16 + subBytes + 2], print.c_str(), printBytes);
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(link.c_str()); return false; }
    DWORD got = 0; const bool ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &got, nullptr) != 0;
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok;
}
static bool IsReparse(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT); }

int main() {
    // 1. CleanItems() discovery & categories
    const auto& items = ak::CleanItems();
    assert(items.size() == 17);
    std::set<std::wstring> ids, cats;
    const std::set<std::wstring> allowedCats = {L"Temporary", L"Caches", L"Logs", L"System"};
    for (const auto& it : items) {
        assert(!it.id.empty() && !it.title.empty() && !it.desc.empty());
        assert(allowedCats.count(it.category) == 1);
        assert(it.size && it.clean);
        assert(ids.insert(it.id).second);
        cats.insert(it.category);
    }
    assert(cats.size() == allowedCats.size());

    // 2. Protected path validation
    assert(ak::IsProtectedCleanupPath(L""));
    assert(ak::IsProtectedCleanupPath(L"C:\\"));
    assert(ak::IsProtectedCleanupPath(L"C:\\Windows"));
    assert(ak::IsProtectedCleanupPath(L"C:\\Windows\\System32"));
    assert(ak::IsProtectedCleanupPath(L"C:\\Temp\\..\\Windows"));
    assert(ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_Documents)));
    assert(ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_Documents) + L"\\Sub"));
    assert(ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_LocalAppData)));

    const std::wstring sbRoot = ak::DataDir() + L"\\wt02_sandbox";
    const std::wstring sbCache = sbRoot + L"\\cache";
    const std::wstring sbSub = sbCache + L"\\sub";
    const std::wstring sbOutside = sbRoot + L"\\outside_target";
    ak::Evict(sbRoot, true, false);
    ak::EnsureDir(sbSub);
    ak::EnsureDir(sbOutside);
    assert(!ak::IsProtectedCleanupPath(sbCache));

    assert(WriteDummyFile(sbCache + L"\\a.tmp", "12345", 5));
    assert(WriteDummyFile(sbSub + L"\\b.log", "1234567890", 10));
    assert(WriteDummyFile(sbCache + L"\\locked.dat", "LOCKED", 6));
    assert(WriteDummyFile(sbOutside + L"\\keep.txt", "PROTECTED", 9));

    // Create a directory junction inside sbCache -> sbOutside (to verify reparse boundary)
    const std::wstring juncPath = sbCache + L"\\link_out";
    assert(MakeJunction(juncPath, sbOutside) && IsReparse(juncPath));   // a real junction, so the checks below are not vacuous

    // 3. Dry-run scan
    ak::CleanStats dry;
    const uint64_t dryBytes = ak::SafeCleanDir(sbCache, dry, true);
    assert(dryBytes == 21);
    assert(dry.files == 3);
    assert(ak::FileExists(sbCache + L"\\a.tmp"));
    assert(ak::FileExists(sbOutside + L"\\keep.txt"));

    // 4. Live clean with exclusive lock on locked.dat + junction skip
    HANDLE hLock = CreateFileW((sbCache + L"\\locked.dat").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(hLock != INVALID_HANDLE_VALUE);

    ak::CleanStats live;
    const uint64_t freed = ak::SafeCleanDir(sbCache, live, false);
    CloseHandle(hLock);

    assert(freed == 15);
    assert(live.files == 2);
    assert(live.skipped == 1);
    assert(!ak::FileExists(sbCache + L"\\a.tmp"));
    assert(!ak::FileExists(sbSub + L"\\b.log"));
    assert(ak::FileExists(sbCache + L"\\locked.dat"));
    assert(ak::FileExists(sbOutside + L"\\keep.txt")); // target outside junction MUST remain untouched

    // ---- WT02-M1: CleanStats.bytes counts every file exactly once, however deeply nested
    assert(dry.bytes == 21);                 // 5 + 10 + 6, not 31 (the nested file was counted twice before)
    assert(live.bytes == 15);                // only what was really deleted
    {
        const std::wstring deep = sbRoot + L"\\deep";
        ak::EnsureDir(deep + L"\\a\\b\\c");
        assert(WriteDummyFile(deep + L"\\r.txt", "1", 1));
        assert(WriteDummyFile(deep + L"\\a\\a.txt", "22", 2));
        assert(WriteDummyFile(deep + L"\\a\\b\\b.txt", "333", 3));
        assert(WriteDummyFile(deep + L"\\a\\b\\c\\c.txt", "4444", 4));
        ak::CleanStats d2; const uint64_t r = ak::SafeCleanDir(deep, d2, true);
        assert(r == 10 && d2.bytes == 10 && d2.files == 4);
        ak::CleanStats d3; assert(ak::SafeCleanDir(deep, d3, false) == 10 && d3.bytes == 10 && d3.files == 4 && !ak::FileExists(deep + L"\\a\\b\\c\\c.txt"));
    }

    // ---- WT02-M2: canonical protected paths
    const std::wstring win = ak::WinDir(), pf = ak::KnownFolder(FOLDERID_ProgramFiles), pf86 = ak::KnownFolder(FOLDERID_ProgramFilesX86);
    assert(ak::IsProtectedCleanupPath(L"C:/Windows/System32"));                      // forward slashes
    assert(ak::IsProtectedCleanupPath(L"\\\\?\\C:\\"));                              // extended prefix, drive root
    assert(ak::IsProtectedCleanupPath(L"\\\\?\\C:\\Windows\\System32\\drivers"));    // extended prefix, below System32
    assert(ak::IsProtectedCleanupPath(win + L"\\System32\\drivers"));                // subtree, not only the folder itself
    assert(ak::IsProtectedCleanupPath(win + L"\\SysWOW64\\x"));
    assert(ak::IsProtectedCleanupPath(win + L"\\WinSxS\\a\\b"));
    assert(ak::IsProtectedCleanupPath(win + L"\\SYSTEM32\\"));                       // case and trailing separator
    assert(ak::IsProtectedCleanupPath(win + L"\\System32\\.\\drivers"));             // "." component
    assert(ak::IsProtectedCleanupPath(win + L"\\\\System32\\\\drivers"));            // doubled separators
    assert(ak::IsProtectedCleanupPath(pf + L"\\Some App") && ak::IsProtectedCleanupPath(pf86 + L"\\Some App\\x") && ak::IsProtectedCleanupPath(pf));
    assert(ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_Downloads) + L"\\x"));
    assert(ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_Profile)) && ak::IsProtectedCleanupPath(ak::DirOf(ak::KnownFolder(FOLDERID_Profile))));   // profile and C:\Users
    assert(ak::IsProtectedCleanupPath(L"subdir") && ak::IsProtectedCleanupPath(L"cache\\x") && ak::IsProtectedCleanupPath(L".\\x") && ak::IsProtectedCleanupPath(L"C:dir"));   // relative
    assert(ak::IsProtectedCleanupPath(L"C:") && ak::IsProtectedCleanupPath(L"D:\\") && ak::IsProtectedCleanupPath(L"\\\\server\\share\\x") && ak::IsProtectedCleanupPath(L"\\\\?\\UNC\\server\\share\\x") && ak::IsProtectedCleanupPath(L"\\\\.\\C:"));   // malformed / network / device
    assert(ak::IsProtectedCleanupPath(L"C:\\a\\..\\b") && ak::IsProtectedCleanupPath(L"C:\\a\\b*") && ak::IsProtectedCleanupPath(L"C:\\a:stream") && ak::IsProtectedCleanupPath(L"C:\\a|b"));
    { wchar_t shortPf[MAX_PATH] = L""; if (GetShortPathNameW(pf.c_str(), shortPf, MAX_PATH) > 0 && _wcsicmp(shortPf, pf.c_str()) != 0) assert(ak::IsProtectedCleanupPath(std::wstring(shortPf) + L"\\x")); }   // 8.3 form
    // not protected: unrelated names that merely start like a protected one, and the folders the cleaner is meant to empty
    assert(!ak::IsProtectedCleanupPath(win + L"\\System32x\\a"));
    assert(!ak::IsProtectedCleanupPath(win + L"\\Temp") && !ak::IsProtectedCleanupPath(win + L"\\Logs\\CBS"));
    assert(!ak::IsProtectedCleanupPath(sbCache) && !ak::IsProtectedCleanupPath(L"\\\\?\\" + sbCache) && !ak::IsProtectedCleanupPath(pf + L"x\\a"));
    assert(!ak::IsProtectedCleanupPath(ak::KnownFolder(FOLDERID_LocalAppData) + L"\\Temp"));
    { std::wstring c; assert(ak::CleanCanonicalPath(L"C:/Windows//System32/./", c) && c == ak::Lower(win) + L"\\system32"); assert(!ak::CleanCanonicalPath(L"x", c) && c.empty()); }

    // ---- WT02-L1: DeleteFiles has the same protections
    {
        const std::wstring df = sbRoot + L"\\df";
        ak::EnsureDir(df); ak::EnsureDir(df + L"\\keepdir.log");
        assert(WriteDummyFile(df + L"\\one.log", "1234", 4) && WriteDummyFile(df + L"\\ro.log", "12", 2) && WriteDummyFile(df + L"\\other.txt", "9", 1));
        SetFileAttributesW((df + L"\\ro.log").c_str(), FILE_ATTRIBUTE_READONLY);
        assert(ak::FilesSize(df, L"*.log") == 6);                                      // the folder named *.log is not counted
        assert(ak::FilesSize(df, L"..\\*") == 0 && ak::FilesSize(df, L"C:\\*") == 0);  // a pattern is a name pattern only
        assert(ak::DeleteFiles(df, L"..\\*.txt") == 0 && ak::FileExists(df + L"\\other.txt"));
        ak::CleanStats ds; const uint64_t del = ak::DeleteFiles(df, L"*.log", &ds);
        assert(del == 6 && ds.files == 2 && ds.bytes == 6 && !ak::FileExists(df + L"\\one.log") && !ak::FileExists(df + L"\\ro.log") && ak::FileExists(df + L"\\other.txt"));   // read-only file removed; other pattern kept
        assert(ak::DirExists(df + L"\\keepdir.log"));                                  // a folder is never deleted by DeleteFiles
        assert(ak::DeleteFiles(win + L"\\System32", L"*.nonexistent_wt02") == 0 && ak::DeleteFiles(L"relative", L"*.log") == 0 && ak::DeleteFiles(L"C:\\", L"*.nonexistent_wt02") == 0);   // protected / malformed: refused
        // the test-build sandbox guard inside DeleteFiles itself: a plain unprotected folder outside the test area is refused
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        const std::wstring outside = std::wstring(tmp) + L"ankh_wt02_guard_" + std::to_wstring(GetCurrentProcessId());
        if (CreateDirectoryW(outside.c_str(), nullptr)) {
            assert(WriteDummyFile(outside + L"\\canary.log", "x", 1));
            assert(ak::DeleteFiles(outside, L"*.log") == 0 && ak::FileExists(outside + L"\\canary.log"));
            DeleteFileW((outside + L"\\canary.log").c_str()); RemoveDirectoryW(outside.c_str());   // my own test file, removed directly
        }
        // links are skipped: a junction named like the pattern is not followed or deleted
        const std::wstring jd = df + L"\\jd";
        ak::EnsureDir(jd);
        const std::wstring jl = jd + L"\\link.log";
        assert(MakeJunction(jl, sbOutside) && IsReparse(jl));
        ak::CleanStats js; assert(ak::DeleteFiles(jd, L"*.log", &js) == 0 && ak::FileExists(sbOutside + L"\\keep.txt"));
        RemoveDirectoryW(jl.c_str());
    }

    // ---- WT02-L2: a folder that turned into a junction is not entered
    {
        ak::CleanStats js; const uint64_t r = ak::SafeCleanDir(juncPath, js, true, 1);
        assert(r == 0 && js.reparseSkipped == 1 && js.files == 0 && ak::FileExists(sbOutside + L"\\keep.txt"));
    }

    // ---- WT02-L3: "Clean selected" acts on the selected items inside the category filter only
    {
        std::vector<bool> sel(items.size(), false);
        size_t selTemp = 0, selLogs = 0;
        for (size_t i = 0; i < items.size(); ++i) { sel[i] = true; selTemp += items[i].category == L"Temporary"; selLogs += items[i].category == L"Logs"; }
        assert(ak::CleanSelectedInScope(items, sel, L"All").size() == items.size());
        assert(ak::CleanSelectedInScope(items, sel, L"Logs").size() == selLogs && selLogs > 0 && selLogs < items.size());
        for (size_t i : ak::CleanSelectedInScope(items, sel, L"Logs")) assert(items[i].category == L"Logs");
        sel.assign(items.size(), false); for (size_t i = 0; i < items.size(); ++i) if (items[i].category == L"Temporary") sel[i] = true;
        assert(ak::CleanSelectedInScope(items, sel, L"Logs").empty() && ak::CleanSelectedInScope(items, sel, L"Temporary").size() == selTemp);   // selected items hidden by the filter are not included
    }

    // ---- Anubis System Health Cleanup Preview: read-only, missing vs access-denied, states, separation, background worker
    {
        const std::wstring pvDir = sbRoot + L"\\preview_ro";
        const std::wstring pvSub = pvDir + L"\\nested";
        const std::wstring pvMissing = sbRoot + L"\\does_not_exist";
        ak::EnsureDir(pvSub);
        assert(WriteDummyFile(pvDir + L"\\a.tmp", "123456", 6));
        assert(WriteDummyFile(pvSub + L"\\b.log", "1234", 4));

        auto mkItem = [](const wchar_t* id, const wchar_t* title, const wchar_t* cat,
                         std::function<uint64_t(uint64_t&)> szA) -> ak::CleanItem {
            return {id, title, title, cat, ak::Tag::Safe, true, [szA] { uint64_t d = 0; return szA(d); }, [] { return 0ull; }, szA};
        };

        // 1. Read-only & accurate size + missing directory does not count as error
        std::vector<ak::CleanItem> nonEmpty = {
            mkItem(L"t1", L"Temp 1", L"Temporary", [&](uint64_t& d) { ak::CleanStats s; uint64_t b = ak::SafeCleanDir(pvDir, s, true); d += s.accessDenied; return b; }),
            mkItem(L"t2", L"Missing", L"Caches", [&](uint64_t& d) { return ak::FolderSize(pvMissing, L"*", 0, &d); })
        };
        const ak::CleanupScanReport r1 = ak::RunCleanupPreviewScan(nonEmpty);
        assert(r1.completed && !r1.cancelled && r1.totalBytes == 10 && r1.categoriesWithData == 1 && r1.inaccessibleDirs == 0);
        assert(r1.status == ak::CleanupPreviewStatus::CleanupAvailable);
        assert(ak::FileExists(pvDir + L"\\a.tmp") && ak::FileExists(pvSub + L"\\b.log"));   // strictly read-only

        // 2. Empty cleanup -> Clean
        std::vector<ak::CleanItem> emptyOnly = {
            mkItem(L"e1", L"Empty", L"Temporary", [&](uint64_t& d) { return ak::FolderSize(pvMissing, L"*", 0, &d); })
        };
        const ak::CleanupScanReport rClean = ak::RunCleanupPreviewScan(emptyOnly);
        assert(rClean.completed && rClean.totalBytes == 0 && rClean.categoriesWithData == 0 && rClean.inaccessibleDirs == 0);
        assert(rClean.status == ak::CleanupPreviewStatus::Clean);

        // 3. Access Denied -> PartialScan (never Clean)
        std::vector<ak::CleanItem> partial = {
            mkItem(L"p1", L"Data", L"Temporary", [&](uint64_t& d) { ak::CleanStats s; uint64_t b = ak::SafeCleanDir(pvDir, s, true); d += s.accessDenied; return b; }),
            mkItem(L"p2", L"Denied", L"Logs", [](uint64_t& d) { d += 1; return 0ull; })
        };
        const ak::CleanupScanReport rPart = ak::RunCleanupPreviewScan(partial);
        assert(rPart.completed && rPart.totalBytes == 10 && rPart.inaccessibleDirs == 1 && !rPart.warnings.empty());
        assert(rPart.status == ak::CleanupPreviewStatus::PartialScan);

        std::vector<ak::CleanItem> deniedZero = { mkItem(L"d0", L"DeniedOnly", L"Logs", [](uint64_t& d) { d += 1; return 0ull; }) };
        const ak::CleanupScanReport rDen0 = ak::RunCleanupPreviewScan(deniedZero);
        assert(rDen0.completed && rDen0.totalBytes == 0 && rDen0.inaccessibleDirs == 1 && rDen0.status == ak::CleanupPreviewStatus::PartialScan);

        // 4. Failed scan (empty items or all items fail) -> ScanFailed
        const ak::CleanupScanReport rEmptyItems = ak::RunCleanupPreviewScan(std::vector<ak::CleanItem>{});
        assert(!rEmptyItems.completed && rEmptyItems.status == ak::CleanupPreviewStatus::ScanFailed);
        std::vector<ak::CleanItem> throwing = { mkItem(L"bad", L"Throw", L"Logs", [](uint64_t&) -> uint64_t { throw std::runtime_error("boom"); }) };
        const ak::CleanupScanReport rFail = ak::RunCleanupPreviewScan(throwing);
        assert(!rFail.completed && rFail.scannedItems == 0 && rFail.status == ak::CleanupPreviewStatus::ScanFailed);

        // 5. UI state labels & Health vs Cleanup separation
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::NotScanned)) == L"Not Scanned");
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::Scanning)) == L"Scanning");
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::Clean)) == L"Clean");
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::CleanupAvailable)) == L"Cleanup Available");
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::PartialScan)) == L"Partial Scan");
        assert(std::wstring(ak::CleanupPreviewLabel(ak::CleanupPreviewStatus::ScanFailed)) == L"Scan Failed");
        ak::Usage hu{}; hu.diskTotal = 1000; hu.diskFree = 500;
        const ak::Health hGood = ak::RunHealthScan(hu, {L"WinDefend", true, true, true, 1}, {L"SSD", L"M", L"Healthy", true}, { {L"RP", L"2026", 1} });
        assert(hGood.score == 0 && r1.status == ak::CleanupPreviewStatus::CleanupAvailable);   // healthy PC can still have cleanup
        const ak::Health hCrit = ak::RunHealthScan(hu, {L"", false, false, false, 0}, {L"SSD", L"M", L"Unhealthy", false}, {});
        assert(hCrit.score == 3 && rClean.status == ak::CleanupPreviewStatus::Clean);          // critical PC can have 0 cleanup

        // 6. Background worker completion, duplicate-scan guard, and clean shutdown
        HWND msgWnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        assert(msgWnd != nullptr);
        ak::HealthScanController ctrl;
        std::vector<ak::CleanItem> bgItems = nonEmpty;
        assert(ctrl.Start(msgWnd, WM_APP + 16, hu, &bgItems));
        assert(!ctrl.Start(msgWnd, WM_APP + 16, hu, &bgItems));   // duplicate concurrent scan prevented
        MSG msg{}; bool gotMsg = false;
        const DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 15000) {
            if (PeekMessageW(&msg, msgWnd, WM_APP + 16, WM_APP + 16, PM_REMOVE)) {
                auto* b = reinterpret_cast<ak::HealthScanBundle*>(msg.lParam);
                assert(b && b->cleanup.completed && b->cleanup.totalBytes == 10 && b->cleanup.status == ak::CleanupPreviewStatus::CleanupAvailable);
                delete b;
                gotMsg = true;
                break;
            }
            Sleep(20);
        }
        assert(gotMsg);
        ctrl.Shutdown();
        // Cancellation / shutdown during active scan
        std::vector<ak::CleanItem> slowItems = { mkItem(L"s1", L"Slow", L"Temporary", [](uint64_t&) { Sleep(150); return 0ull; }) };
        assert(ctrl.Start(msgWnd, WM_APP + 16, hu, &slowItems));
        ctrl.Shutdown();
        assert(!ctrl.IsScanning());
        DestroyWindow(msgWnd);
    }

    // Cleanup sandbox
    RemoveDirectoryW(juncPath.c_str());
    ak::Evict(sbRoot, true, false);
    std::printf("PASS test_cleaner (17 CleanItems, 4 categories, protected-path/dry-run/lock/junction + WT02 M1/M2/L1/L2/L3 regressions OK)\n");
    return 0;
}
