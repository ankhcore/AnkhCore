// FT-2 / A04 measurement (not a pass/fail test): what painting the Icon Manager cost before and after, on the installed themes.
// Read-only: it reads the installed theme folders (default C:\Program Files\AnkhCore\IconThemes) and writes nothing.
//   bench_icon_manager.exe old   : the paint path before FT-2 (full validation of every card, twice per repaint for the selection)
//   bench_icon_manager.exe new   : the paint path now (cache lookups) plus the one-time worker costs
#include "base.h"
#include "ui.h"
#include "icon_status.h"
#include "thumb_cache.h"
#include <psapi.h>
#include <chrono>
#include <cstdio>
using namespace ak;

static double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
static uint64_t Peak() { PROCESS_MEMORY_COUNTERS m{}; GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof(m)); return m.PeakWorkingSetSize; }

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring mode = argc > 1 ? argv[1] : L"old";
    const std::wstring root = argc > 2 ? argv[2] : L"C:\\Program Files\\AnkhCore\\IconThemes";
    // the cards of the page: one per group; the selected theme (bigsur-dark) is validated once more for the Apply button
    const wchar_t* cards[] = {L"bigsur-dark", L"sweet", L"candy", L"tech", L"pear-blue-dark", L"alienware-invader", L"magnum", L"hud-machine-blue", L"alienware-eclipse"};
    std::printf("mode %ls, root %ls, start peak working set %.1f MB\n", mode.c_str(), root.c_str(), Peak() / 1048576.0);
    if (mode == L"old") {
        for (int rep = 1; rep <= 3; ++rep) {
            auto t0 = std::chrono::steady_clock::now(); int n = 0; uint64_t bytes = 0;
            for (const wchar_t* id : cards) {
                const std::wstring dir = root + L"\\" + id;
                std::vector<std::pair<std::wstring, std::wstring>> pairs; std::wstring err;
                const bool ok = ValidateIconThemeDir(dir, &pairs, &err); ++n;
                for (const auto& kv : pairs) { WIN32_FILE_ATTRIBUTE_DATA a{}; if (GetFileAttributesExW((dir + L"\\" + kv.second).c_str(), GetFileExInfoStandard, &a)) bytes += (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow; }
                if (!ok) std::printf("  %ls: %ls\n", id, err.c_str());
            }
            ValidateIconThemeDir(root + L"\\bigsur-dark"); ++n;   // the selected theme's Apply button
            std::printf("OLD one repaint of the page #%d: %d full validations, %.0f ms on the UI thread, %.0f MB of modules read, peak working set %.1f MB\n", rep, n, Ms(t0), bytes / 1048576.0, Peak() / 1048576.0);
        }
    } else {
        IconStatusCache cache;
        auto t0 = std::chrono::steady_clock::now();
        for (const auto& p : IconPacks()) { const IconThemeStatus s = IconThemeQuickCheck(root + L"\\" + IconThemeFolderName(p.id)); if (!s.installed) std::printf("  %ls: %ls\n", p.id, s.problem.c_str()); }
        std::printf("NEW worker, one time per page visit: quick check of all %zu themes %.1f ms, peak working set %.1f MB\n", IconPacks().size(), Ms(t0), Peak() / 1048576.0);
        // paint path: cache lookups only
        cache.RefreshNow();
        t0 = std::chrono::steady_clock::now();
        volatile int sink = 0;
        for (int i = 0; i < 100000; ++i) for (const wchar_t* id : cards) sink += cache.Get(id).checked ? 1 : 0;
        const double per = Ms(t0) / 100000.0;
        std::printf("NEW one repaint of the page (%zu cache lookups): %.4f ms on the UI thread\n", sizeof(cards) / sizeof(cards[0]), per);
        // previews: decode on the worker, thumbnail sized
        ThumbService svc; svc.Start(L"", nullptr);
        int found = 0; double worst = 0, total = 0;
        for (const auto& p : IconPacks()) {
            const IconThemeStatus s = IconThemeQuickCheck(root + L"\\" + IconThemeFolderName(p.id));
            if (s.preview.empty()) continue;
            ++found;
            WIN32_FILE_ATTRIBUTE_DATA a{}; GetFileAttributesExW(s.preview.c_str(), GetFileExInfoStandard, &a);
            auto t1 = std::chrono::steady_clock::now();
            auto miss = svc.Get(s.preview, 300, 170, true); const double getMs = Ms(t1);
            while (!svc.Idle()) Sleep(1);
            const double dec = Ms(t1);
            auto th = svc.Get(s.preview, 300, 170, true);
            std::printf("  preview %-18ls %6.1f MB file: UI-thread Get %.3f ms, worker decode %.0f ms -> %dx%d thumbnail (%.0f KB)\n", p.id, ((static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) / 1048576.0, getMs, dec, th ? th->w : 0, th ? th->h : 0, th ? th->Bytes() / 1024.0 : 0.0);
            worst = std::max(worst, getMs); total += dec; (void)miss;
        }
        std::printf("NEW previews: %d found, slowest UI-thread Get %.3f ms, all decodes %.0f ms on the worker, memory cache %.2f MB, peak working set %.1f MB\n", found, worst, total, svc.MemoryBytes() / 1048576.0, Peak() / 1048576.0);
        svc.Stop();
    }
    return 0;
}
