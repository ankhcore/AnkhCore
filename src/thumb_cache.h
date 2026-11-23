// FT-2 / A04: thumbnails loaded off the UI thread, kept in a size-capped memory cache and a validated disk cache.
// The UI thread only calls Get(): a memory lookup that returns the pixels or nothing (and queues the load). Decoding a
// theme's preview (a 16 MB bitmap in the worst case) happens on one worker thread. Cache entries are keyed by file path,
// size and mode and validated against the source file's size and time stamp, so a changed picture is never shown stale.
// Disk entries are written atomically under the cache folder; a damaged or mismatching entry is ignored and rebuilt.
#pragma once
#include "ui.h"
#include <condition_variable>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

namespace ak {

struct ThumbPixels {
    std::vector<uint32_t> px;   // premultiplied BGRA, top row first
    int w = 0, h = 0;
    uint64_t id = 0;            // unique for the life of the process, never reused
    size_t Bytes() const { return px.size() * 4 + 64; }
    // Key for the Direct2D device bitmap (D2DSurface::PixelsBitmap): an id with the top bit set can never equal a real pointer
    const void* Key() const { return reinterpret_cast<const void*>(static_cast<uintptr_t>(0xF000000000000000ull | id)); }
};

struct ThumbStamp {
    uint64_t size = 0, mtime = 0; bool ok = false;
    bool operator==(const ThumbStamp& o) const { return ok && o.ok && size == o.size && mtime == o.mtime; }
};
inline ThumbStamp ThumbStatFile(const std::wstring& path) {
    ThumbStamp s; WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return s;
    s.size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    s.mtime = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    s.ok = true; return s;
}
// Boxes are rounded up to 16 px, so a small resize of the window does not decode every picture again.
inline int ThumbQuant(int v) { return v < 16 ? 16 : ((v + 15) / 16) * 16; }
// cover: fill the box and crop the overflow (as the Icon Manager card shows it today); otherwise fit inside it, proportions kept.
inline std::wstring ThumbKeyText(const std::wstring& path, int w, int h, bool cover) {
    return Lower(path) + L"|" + std::to_wstring(ThumbQuant(w)) + L"x" + std::to_wstring(ThumbQuant(h)) + (cover ? L"|c" : L"|f");
}
inline uint64_t ThumbHash(const std::string& s) { uint64_t h = 1469598103934665603ull; for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } return h; }
inline std::wstring ThumbDiskName(const std::wstring& keyText) {
    wchar_t b[32]; swprintf(b, 32, L"%016llx.thumb", static_cast<unsigned long long>(ThumbHash(WToUtf8(keyText)))); return b;
}

// Disk format: "AKT1", w, h (u32), source size, source mtime (u64), key length (u32), key (UTF-8), then w*h*4 pixel bytes.
inline std::vector<uint8_t> ThumbEncode(const ThumbPixels& t, const std::wstring& keyText, const ThumbStamp& st) {
    const std::string key = WToUtf8(keyText);
    std::vector<uint8_t> o; o.reserve(32 + key.size() + t.px.size() * 4);
    auto put = [&](const void* p, size_t n) { const auto* b = static_cast<const uint8_t*>(p); o.insert(o.end(), b, b + n); };
    put("AKT1", 4);
    const uint32_t w = static_cast<uint32_t>(t.w), h = static_cast<uint32_t>(t.h), kl = static_cast<uint32_t>(key.size());
    put(&w, 4); put(&h, 4); put(&st.size, 8); put(&st.mtime, 8); put(&kl, 4); put(key.data(), key.size()); put(t.px.data(), t.px.size() * 4);
    return o;
}
inline bool ThumbDecode(const std::vector<uint8_t>& d, const std::wstring& keyText, const ThumbStamp& st, ThumbPixels& out) {
    if (d.size() < 32 || memcmp(d.data(), "AKT1", 4) != 0) return false;
    uint32_t w, h, kl; uint64_t size, mtime;
    memcpy(&w, d.data() + 4, 4); memcpy(&h, d.data() + 8, 4); memcpy(&size, d.data() + 12, 8); memcpy(&mtime, d.data() + 20, 8); memcpy(&kl, d.data() + 28, 4);
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || size != st.size || mtime != st.mtime) return false;
    const std::string key = WToUtf8(keyText);
    if (kl != key.size() || d.size() != 32ull + kl + static_cast<uint64_t>(w) * h * 4 || memcmp(d.data() + 32, key.data(), kl) != 0) return false;
    out.w = static_cast<int>(w); out.h = static_cast<int>(h); out.px.resize(static_cast<size_t>(w) * h);
    memcpy(out.px.data(), d.data() + 32 + kl, out.px.size() * 4);
    return true;
}

// Memory cache with a byte cap: the least recently used entries go first. Ids of removed entries are collected so the UI
// can drop the matching device bitmaps (DrainEvicted).
class ThumbLru {
public:
    explicit ThumbLru(size_t capBytes = 32u << 20) : cap_(capBytes) {}
    std::shared_ptr<const ThumbPixels> Find(const std::wstring& key) {
        const auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->px;
    }
    void Put(const std::wstring& key, const std::wstring& path, const ThumbStamp& st, std::shared_ptr<const ThumbPixels> px) {
        Erase(key);
        bytes_ += px->Bytes();
        order_.push_front(Entry{key, path, st, std::move(px)});
        map_[key] = order_.begin();
        while (bytes_ > cap_ && order_.size() > 1) Drop(std::prev(order_.end()));
    }
    void Erase(const std::wstring& key) { const auto it = map_.find(key); if (it != map_.end()) Drop(it->second); }
    struct Cached { std::wstring key, path; ThumbStamp stamp; };
    std::vector<Cached> Entries() const { std::vector<Cached> v; for (const auto& e : order_) v.push_back({e.key, e.path, e.stamp}); return v; }
    std::vector<const void*> DrainEvicted() { auto v = std::move(evicted_); evicted_.clear(); return v; }
    size_t Bytes() const { return bytes_; }
    size_t Count() const { return order_.size(); }
    size_t Cap() const { return cap_; }
private:
    struct Entry { std::wstring key, path; ThumbStamp stamp; std::shared_ptr<const ThumbPixels> px; };
    void Drop(std::list<Entry>::iterator it) { bytes_ -= it->px->Bytes(); evicted_.push_back(it->px->Key()); map_.erase(it->key); order_.erase(it); }
    size_t cap_, bytes_ = 0;
    std::list<Entry> order_;
    std::unordered_map<std::wstring, std::list<Entry>::iterator> map_;
    std::vector<const void*> evicted_;
};

class ThumbService {
public:
    struct Stats { std::atomic<int> decodes{0}, diskHits{0}, diskWrites{0}, queued{0}, failures{0}; };
    Stats stats;

    // cacheDir may be empty (memory cache only). onReady is called from the worker thread after each finished load.
    void Start(const std::wstring& cacheDir, std::function<void()> onReady, size_t memCap = 32u << 20, size_t diskMaxFiles = 400) {
        Stop();
        { std::lock_guard<std::mutex> lk(mx_); cacheDir_ = cacheDir; onReady_ = std::move(onReady); lru_ = ThumbLru(memCap); diskMax_ = diskMaxFiles; failed_.clear(); pending_.clear(); jobs_.clear(); stop_ = false; }
        worker_ = std::thread([this] { Loop(); });
    }
    void Stop() {
        { std::lock_guard<std::mutex> lk(mx_); stop_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    ~ThumbService() { Stop(); }

    // UI thread: memory only. Returns the thumbnail, or nothing while it is being prepared (the load is queued once).
    std::shared_ptr<const ThumbPixels> Get(const std::wstring& path, int w, int h, bool cover) {
        const std::wstring key = ThumbKeyText(path, w, h, cover);
        std::lock_guard<std::mutex> lk(mx_);
        if (auto p = lru_.Find(key)) return p;
        if (failed_.count(key) || pending_.count(key) || stop_) return nullptr;
        pending_.insert(key);
        jobs_.push_back(Job{Job::Load, key, path, ThumbQuant(w), ThumbQuant(h), cover});
        ++stats.queued;
        cv_.notify_one();
        return nullptr;
    }
    // Page entry: checks, on the worker, that cached pictures still match their files; changed ones are dropped and, where
    // they failed before, tried again. The pictures stay on screen meanwhile.
    void Revalidate() {
        std::lock_guard<std::mutex> lk(mx_);
        if (stop_ || revalidating_) return;
        revalidating_ = true;
        jobs_.push_back(Job{Job::Revalidate, {}, {}, 0, 0, false});
        cv_.notify_one();
    }
    // True when loading this picture was tried and failed (the card says so instead of "loading" for ever); a page visit retries it.
    bool Failed(const std::wstring& path, int w, int h, bool cover) { std::lock_guard<std::mutex> lk(mx_); return failed_.count(ThumbKeyText(path, w, h, cover)) > 0; }
    std::vector<const void*> DrainEvicted() { std::lock_guard<std::mutex> lk(mx_); return lru_.DrainEvicted(); }
    size_t MemoryBytes() { std::lock_guard<std::mutex> lk(mx_); return lru_.Bytes(); }
    size_t MemoryCount() { std::lock_guard<std::mutex> lk(mx_); return lru_.Count(); }
    bool Idle() { std::lock_guard<std::mutex> lk(mx_); return jobs_.empty() && !busy_; }

private:
    struct Job { enum Kind { Load, Revalidate } kind; std::wstring key, path; int w, h; bool cover; };

    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) break;
                job = std::move(jobs_.front()); jobs_.pop_front(); busy_ = true;
            }
            if (job.kind == Job::Load) DoLoad(job); else DoRevalidate();
            std::function<void()> cb;
            { std::lock_guard<std::mutex> lk(mx_); busy_ = false; cb = onReady_; }
            if (cb) cb();
        }
        CoUninitialize();
    }
    std::wstring DiskPath(const std::wstring& key) const { return cacheDir_.empty() ? std::wstring() : cacheDir_ + L"\\" + ThumbDiskName(key); }

    void DoLoad(const Job& j) {
        std::shared_ptr<ThumbPixels> out;
        const ThumbStamp st = ThumbStatFile(j.path);
        std::wstring dir; size_t diskMax;
        { std::lock_guard<std::mutex> lk(mx_); dir = cacheDir_; diskMax = diskMax_; }
        if (st.ok && st.size <= (256ull << 20)) {
            const std::wstring dp = dir.empty() ? std::wstring() : dir + L"\\" + ThumbDiskName(j.key);
            std::vector<uint8_t> d;
            auto t = std::make_shared<ThumbPixels>();
            if (!dp.empty() && ReadAll(dp, d) && ThumbDecode(d, j.key, st, *t)) { out = t; ++stats.diskHits; }
            else {
                std::vector<uint8_t> src;
                if (ReadAll(j.path, src)) {
                    Image32 im = DecodeImage(src.data(), src.size(), j.w, j.h, j.cover);
                    if (im.bmp) {
                        const Px px = ToPixels(im); DeleteObject(im.bmp);
                        if (!px.p.empty()) {
                            t->w = px.w; t->h = px.h; t->px = px.p; out = t; ++stats.decodes;
                            if (!dp.empty()) {
                                EnsureDir(dir);
                                const std::vector<uint8_t> enc = ThumbEncode(*t, j.key, st);
                                if (WriteAtomic(dp, enc.data(), enc.size())) { ++stats.diskWrites; PruneDisk(dir, diskMax); }
                            }
                        }
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lk(mx_);
        pending_.erase(j.key);
        if (out) {
            static std::atomic<uint64_t> nextId{1};
            out->id = nextId++;
            lru_.Put(j.key, j.path, st, out);
        } else { failed_.insert(j.key); ++stats.failures; }
    }
    void DoRevalidate() {
        std::vector<ThumbLru::Cached> all;
        { std::lock_guard<std::mutex> lk(mx_); all = lru_.Entries(); }
        std::vector<std::wstring> stale;
        for (const auto& e : all) if (!(ThumbStatFile(e.path) == e.stamp)) stale.push_back(e.key);
        std::lock_guard<std::mutex> lk(mx_);
        for (const auto& k : stale) lru_.Erase(k);
        if (!failed_.empty()) failed_.clear();   // a picture that could not be read may be readable now
        revalidating_ = false;
    }
    // Keeps the newest `maxFiles` files of the cache folder.
    static void PruneDisk(const std::wstring& dir, size_t maxFiles) {
        std::vector<std::pair<uint64_t, std::wstring>> files;
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.thumb").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do { files.push_back({(static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime, fd.cFileName}); } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (files.size() <= maxFiles) return;
        std::sort(files.begin(), files.end());
        for (size_t i = 0; i + maxFiles < files.size(); ++i) DeleteFileW((dir + L"\\" + files[i].second).c_str());
    }

    std::mutex mx_; std::condition_variable cv_;
    std::thread worker_;
    std::deque<Job> jobs_;
    std::set<std::wstring> pending_, failed_;
    ThumbLru lru_;
    std::wstring cacheDir_; std::function<void()> onReady_;
    size_t diskMax_ = 400;
    bool stop_ = true, busy_ = false, revalidating_ = false;
};

}  // namespace ak
