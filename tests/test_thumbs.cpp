// FT-2 / A04: thumbnail service (src/thumb_cache.h). Real code under test: ThumbService with its worker thread, the memory
// cache with a byte cap, the validated disk cache, DecodeImage (WIC). Pictures are generated in memory and written to a
// temporary folder; nothing on the desktop, in the registry or in settings is touched.
#include "base.h"
#include "ui.h"
#include "thumb_cache.h"
#include "test_pictures.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
using namespace ak;
using namespace testpic;

namespace {
int passes = 0, fails = 0;
void Check(bool ok, const std::string& what) { if (ok) ++passes; else { ++fails; std::printf("FAIL  %s\n", what.c_str()); } }
double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }
template <class F> bool WaitFor(F f, int ms = 8000) { const auto t0 = std::chrono::steady_clock::now(); while (!f()) { if (Ms(t0) > ms) return false; Sleep(10); } return true; }
size_t CountThumbs(const std::wstring& dir) {
    size_t n = 0; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.thumb").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { ++n; } while (FindNextFileW(h, &fd));
    FindClose(h); return n;
}
bool Upright(const ThumbPixels& t) {   // red above blue, sampled away from the border
    return Red(t.px[static_cast<size_t>(t.h / 8) * t.w + t.w / 2]) && Blue(t.px[static_cast<size_t>(t.h * 7 / 8) * t.w + t.w / 2]);
}
// Waits for the picture to be ready (the first Get queues it; the worker fills the memory cache).
std::shared_ptr<const ThumbPixels> GetReady(ThumbService& s, const std::wstring& p, int w, int h, bool cover, int ms = 8000) {
    std::shared_ptr<const ThumbPixels> t = s.Get(p, w, h, cover);
    if (t) return t;
    WaitFor([&] { return s.Idle(); }, ms);
    return s.Get(p, w, h, cover);
}
}  // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // ---------------------------------------------------------------- pure parts
    Check(ThumbQuant(1) == 16 && ThumbQuant(16) == 16 && ThumbQuant(17) == 32 && ThumbQuant(100) == 112 && ThumbQuant(-5) == 16, "quantize: boxes round up to 16 px");
    Check(ThumbKeyText(L"C:\\A\\B.PNG", 100, 90, true) == ThumbKeyText(L"c:\\a\\b.png", 105, 95, true), "key: case-insensitive path, boxes in the same 16 px step share a key");
    Check(ThumbKeyText(L"c:\\a.png", 100, 90, true) != ThumbKeyText(L"c:\\a.png", 100, 90, false) && ThumbKeyText(L"c:\\a.png", 100, 90, true) != ThumbKeyText(L"c:\\a.png", 200, 90, true), "key: cover or fit, and another box, are other pictures");
    Check(ThumbDiskName(L"a") != ThumbDiskName(L"b") && ThumbDiskName(L"a") == ThumbDiskName(L"a") && ThumbDiskName(L"a").size() == 16 + 6, "disk name: stable, distinct, 16 hex digits + .thumb");
    {
        ThumbPixels t; t.w = 3; t.h = 2; t.px = {1, 2, 3, 4, 5, 6};
        ThumbStamp st; st.ok = true; st.size = 100; st.mtime = 12345;
        const std::wstring key = L"c:\\x.png|16x16|c";
        std::vector<uint8_t> enc = ThumbEncode(t, key, st); ThumbPixels back;
        Check(ThumbDecode(enc, key, st, back) && back.w == 3 && back.h == 2 && back.px == t.px, "disk format: a thumbnail survives encode and decode");
        ThumbStamp other = st; other.mtime = 99;
        Check(!ThumbDecode(enc, key, other, back), "disk format: another time stamp (the picture changed) is refused");
        other = st; other.size = 101;
        Check(!ThumbDecode(enc, key, other, back), "disk format: another file size is refused");
        Check(!ThumbDecode(enc, L"c:\\other.png|16x16|c", st, back), "disk format: another key (hash collision) is refused");
        std::vector<uint8_t> cut(enc.begin(), enc.end() - 1);
        Check(!ThumbDecode(cut, key, st, back) && !ThumbDecode({}, key, st, back) && !ThumbDecode(std::vector<uint8_t>(100, 0x41), key, st, back), "disk format: truncated, empty and garbage files are refused");
        std::vector<uint8_t> big = enc; const uint32_t huge = 100000; memcpy(big.data() + 4, &huge, 4);
        Check(!ThumbDecode(big, key, st, back), "disk format: absurd dimensions are refused");
    }
    {   // memory cache: byte cap, least recently used first, ids never reused
        ThumbLru lru(3 * (1000 * 4 + 64));
        std::vector<std::shared_ptr<ThumbPixels>> v;
        for (int i = 0; i < 5; ++i) { auto p = std::make_shared<ThumbPixels>(); p->w = 100; p->h = 10; p->px.assign(1000, i); p->id = 100 + i; v.push_back(p); }
        ThumbStamp st; st.ok = true;
        for (int i = 0; i < 3; ++i) lru.Put(L"k" + std::to_wstring(i), L"p", st, v[i]);
        Check(lru.Count() == 3 && lru.Bytes() <= lru.Cap(), "memory cache: three entries fit the cap");
        lru.Find(L"k0");   // k0 becomes the newest, so k1 is the oldest
        lru.Put(L"k3", L"p", st, v[3]);
        Check(lru.Count() == 3 && lru.Find(L"k1") == nullptr && lru.Find(L"k0") && lru.Find(L"k2") && lru.Find(L"k3"), "memory cache: the least recently used entry goes first");
        const auto ev = lru.DrainEvicted();
        Check(ev.size() == 1 && ev[0] == v[1]->Key() && lru.DrainEvicted().empty(), "memory cache: the removed entry's device-bitmap key is handed back once");
        lru.Put(L"k3", L"p", st, v[4]);   // same key replaced
        const auto ev2 = lru.DrainEvicted();
        Check(ev2.size() == 1 && ev2[0] == v[3]->Key() && lru.Count() == 3, "memory cache: a replaced entry is reported as removed");
        Check(v[0]->Key() != v[1]->Key() && (reinterpret_cast<uintptr_t>(v[0]->Key()) >> 63) == 1, "memory cache: keys are unique and can never equal a real pointer");
        ThumbLru tiny(10); auto p = std::make_shared<ThumbPixels>(); p->w = 10; p->h = 10; p->px.assign(100, 1); p->id = 1; tiny.Put(L"a", L"p", st, p);
        Check(tiny.Count() == 1, "memory cache: one entry larger than the cap is still kept (the picture being shown)");
    }

    // ---------------------------------------------------------------- the service with real files
    wchar_t tb[MAX_PATH] = L""; GetTempPathW(MAX_PATH, tb);
    std::wstring root = tb; while (!root.empty() && root.back() == L'\\') root.pop_back();
    root += L"\\ankh-ft2-thumbs";
    { // clean start
        std::wstring cmd = L"cmd /c rmdir /s /q \"" + root + L"\""; STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { WaitForSingleObject(pi.hProcess, 10000); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    }
    EnsureDir(root);
    const std::wstring cache = root + L"\\cache", pngPath = root + L"\\a.png", bmpPath = root + L"\\big.bmp", missing = root + L"\\none.png";
    const std::vector<uint8_t> png = Encode(GUID_ContainerFormatPng, 640, 360, true);
    const std::vector<uint8_t> bmp = Encode(GUID_ContainerFormatBmp, 3200, 1800, true);   // 17 MB, like the biggest theme preview
    Check(WriteAll(pngPath, png.data(), png.size()) && WriteAll(bmpPath, bmp.data(), bmp.size()), "test pictures written (PNG 640x360, BMP 3200x1800)");

    std::vector<uint32_t> firstPixels;
    {
        ThumbService s; std::atomic<int> ready{0};
        s.Start(cache, [&] { ++ready; });
        auto t0 = std::chrono::steady_clock::now();
        auto miss = s.Get(pngPath, 160, 96, true);
        const double missMs = Ms(t0);
        Check(!miss && s.stats.queued == 1, "service: the first Get returns nothing at once and queues the load");
        Check(!s.Get(pngPath, 160, 96, true) && s.stats.queued == 1, "service: asking again while it loads queues nothing more");
        auto t = GetReady(s, pngPath, 160, 96, true);
        Check(t != nullptr && ready >= 1, "service: the thumbnail arrives and the UI is told (onReady)");
        if (t) {
            Check(t->w == 160 && t->h == 96 && Upright(*t), "service: cover thumbnail has the box size and is upright");
            firstPixels = t->px;
            Check(s.stats.decodes == 1 && s.stats.diskWrites == 1 && s.stats.diskHits == 0 && CountThumbs(cache) == 1, "service: decoded once and written to the disk cache once");
            auto again = s.Get(pngPath, 160, 96, true);
            Check(again == t && s.stats.decodes == 1, "service: the second Get is a memory hit (same object, no work)");
            auto fit = GetReady(s, pngPath, 160, 96, false);
            Check(fit && fit->w == 160 && fit->h == 90 && Upright(*fit) && s.stats.decodes == 2, "service: fit mode keeps the proportions (16:9 inside 160x96 is 160x90), its own cache entry");
            Check(fit && fit->Key() != t->Key(), "service: each picture has its own device-bitmap key");
        }
        // the UI thread is never blocked by a big picture
        t0 = std::chrono::steady_clock::now();
        auto big = s.Get(bmpPath, 300, 170, true);
        const double bigMissMs = Ms(t0);
        Check(!big && bigMissMs < 25.0, "service: Get of a 17 MB bitmap returns at once (" + std::to_string(static_cast<int>(bigMissMs * 1000)) + " us), the decode is off the UI thread");
        t0 = std::chrono::steady_clock::now();
        big = GetReady(s, bmpPath, 300, 170, true);
        std::printf("INFO  17 MB BMP: Get returned in %.2f ms; decoded and ready after %.0f ms on the worker\n", bigMissMs, Ms(t0) + bigMissMs);
        Check(big && big->w == 304 && big->h == 176 && Upright(*big), "service: the 17 MB bitmap becomes a 304x176 upright thumbnail");
        std::printf("INFO  PNG first Get %.3f ms\n", missMs);
        // missing file: no crash, no retry on every paint
        const int q0 = s.stats.queued;
        Check(!s.Get(missing, 160, 96, true), "service: a missing file returns nothing");
        WaitFor([&] { return s.Idle(); });
        const int q1 = s.stats.queued;
        Check(!s.Get(missing, 160, 96, true) && s.stats.queued == q1 && q1 == q0 + 1 && s.stats.failures == 1, "service: a failed picture is remembered and not retried while painting");
        Check(s.Failed(missing, 160, 96, true) && !s.Failed(pngPath, 160, 96, true), "service: Failed() tells a picture that could not be read from one that is fine (the card says so instead of 'loading' for ever)");
        s.Revalidate(); WaitFor([&] { return s.Idle(); });
        s.Get(missing, 160, 96, true); WaitFor([&] { return s.Idle(); });
        Check(s.stats.queued == q1 + 1, "service: a page re-entry (Revalidate) lets a failed picture try again");
        s.Stop();
        Check(s.Get(pngPath, 160, 96, true) != nullptr && s.Get(root + L"\\never-asked.png", 160, 96, true) == nullptr, "service: after Stop the memory cache can still be read and nothing is queued");
    }
    {   // a fresh process life: the disk cache serves it without decoding
        ThumbService s; s.Start(cache, nullptr);
        auto t = GetReady(s, pngPath, 160, 96, true);
        Check(t && s.stats.decodes == 0 && s.stats.diskHits == 1 && t->px == firstPixels, "disk cache: a new service reads the same pixels from disk, no decode");
        s.Stop();
    }
    {   // the picture changed: the stale cache entry is not used
        const std::vector<uint8_t> png2 = Encode(GUID_ContainerFormatPng, 200, 200, false);
        WriteAll(pngPath, png2.data(), png2.size());
        ThumbService s; s.Start(cache, nullptr);
        auto t = GetReady(s, pngPath, 160, 96, true);
        Check(t && s.stats.decodes == 1 && s.stats.diskHits == 0 && t->px != firstPixels && Upright(*t), "disk cache: a changed source file is decoded again, the old entry is not shown");
        // memory entry vs a source that changes while the page is open
        const std::vector<uint8_t> png3 = Encode(GUID_ContainerFormatPng, 300, 100, false);
        WriteAll(pngPath, png3.data(), png3.size());
        auto same = s.Get(pngPath, 160, 96, true);
        Check(same == t, "revalidate: until the check ran, the picture on screen stays (no flicker)");
        s.Revalidate(); WaitFor([&] { return s.Idle(); });
        Check(s.Get(pngPath, 160, 96, true) == nullptr, "revalidate: a changed file's cached picture is dropped");
        auto fresh = GetReady(s, pngPath, 160, 96, true);
        Check(fresh && fresh != t && s.stats.decodes == 2, "revalidate: and the new picture is loaded");
        s.Stop();
    }
    {   // a damaged cache file is ignored and rebuilt; an unusable cache folder only disables the disk cache
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((cache + L"\\*.thumb").c_str(), &fd);
        int damaged = 0;
        if (h != INVALID_HANDLE_VALUE) { do { const std::string junk = "not a thumbnail"; WriteAll(cache + L"\\" + fd.cFileName, reinterpret_cast<const uint8_t*>(junk.data()), junk.size()); ++damaged; } while (FindNextFileW(h, &fd)); FindClose(h); }
        ThumbService s; s.Start(cache, nullptr);
        auto t = GetReady(s, pngPath, 160, 96, true);
        Check(damaged > 0 && t && s.stats.decodes == 1 && s.stats.diskHits == 0, "disk cache: a damaged file is ignored and the picture decoded again");
        s.Stop();
        const std::wstring notDir = root + L"\\afile"; WriteAll(notDir, reinterpret_cast<const uint8_t*>("x"), 1);
        ThumbService s2; s2.Start(notDir + L"\\sub", nullptr);   // cannot be created: it is below a file
        auto t2 = GetReady(s2, pngPath, 160, 96, true);
        Check(t2 && s2.stats.decodes == 1 && s2.stats.diskWrites == 0, "disk cache: an unusable cache folder still shows the picture (memory only)");
        s2.Stop();
    }
    {   // memory cap and disk cap
        const std::wstring c2 = root + L"\\cache2";
        ThumbService s; s.Start(c2, nullptr, 2 * (160 * 96 * 4 + 64) + 10, 3);   // memory: two pictures; disk: three files
        for (int i = 0; i < 6; ++i) GetReady(s, pngPath, 160 + 16 * i, 96, true);
        Check(s.MemoryCount() <= 2 && s.MemoryBytes() <= 2 * (320 * 96 * 4 + 64) + 10, "memory cap: older thumbnails were dropped (" + std::to_string(s.MemoryCount()) + " kept)");
        Check(!s.DrainEvicted().empty(), "memory cap: the dropped pictures' device-bitmap keys were handed back");
        Check(CountThumbs(c2) <= 3 && CountThumbs(c2) >= 1, "disk cap: the cache folder keeps at most 3 files (" + std::to_string(CountThumbs(c2)) + ")");
        s.Stop();
    }
    {   // Stop with work still queued must not hang
        ThumbService s; s.Start(root + L"\\cache3", nullptr);
        for (int i = 0; i < 12; ++i) s.Get(bmpPath, 200 + 16 * i, 120, true);
        const auto t0 = std::chrono::steady_clock::now();
        s.Stop();
        Check(Ms(t0) < 5000, "stop: with 12 big pictures queued it returns after the current one (" + std::to_string(static_cast<int>(Ms(t0))) + " ms)");
    }
    for (const wchar_t* d : {L"\\cache", L"\\cache2", L"\\cache3"}) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((root + d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((root + d + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd)); FindClose(h); }
        RemoveDirectoryW((root + d).c_str());
    }
    for (const wchar_t* f : {L"\\a.png", L"\\big.bmp", L"\\afile"}) DeleteFileW((root + f).c_str());
    RemoveDirectoryW(root.c_str());

    std::printf("%s (%d passed, %d failed)\n", fails ? "FAILED" : "ALL PASSED", passes, fails);
    return fails ? 1 : 0;
}
