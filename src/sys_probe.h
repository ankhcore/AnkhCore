// AnkhCore 2.5 — extra Windows values for the Advanced Sidebar: per-core usage, memory detail and each
// fixed drive. Sampled on the engine thread inside its once-a-second tick (no poller of its own) and
// carried to the views in the engine frame. Everything here is read-only.
#pragma once
#include "base.h"
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "pdh.lib")

namespace ak {

struct DriveInfo { wchar_t letter = 0; uint64_t total = 0, free = 0; double readBps = -1, writeBps = -1; };   // -1 = not known
struct SysExtra {
    std::vector<double> cores;   // usage % per logical processor
    int physCores = 0, logical = 0;
    uint64_t memAvail = 0, memCached = 0, commitUsed = 0, commitLimit = 0;
    std::vector<DriveInfo> drives;   // fixed drives, the system drive first
    double diskRead = -1, diskWrite = -1;   // all drives, bytes per second
};

class SysProbe {
public:
    SysProbe() {
        SYSTEM_INFO si; GetSystemInfo(&si); n_ = static_cast<int>(std::min<DWORD>(si.dwNumberOfProcessors, 256));
        DWORD len = 0; GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        std::vector<uint8_t> b(len);
        if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(b.data()), &len))
            for (DWORD o = 0; o < len;) { const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(b.data() + o); if (!e->Size) break; ++phys_; o += e->Size; }
        if (PdhOpenQueryW(nullptr, 0, &q_) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(q_, L"\\LogicalDisk(*)\\Disk Read Bytes/sec", 0, &rd_) != ERROR_SUCCESS) rd_ = nullptr;
            if (PdhAddEnglishCounterW(q_, L"\\LogicalDisk(*)\\Disk Write Bytes/sec", 0, &wr_) != ERROR_SUCCESS) wr_ = nullptr;
            PdhCollectQueryData(q_);
        }
    }
    ~SysProbe() { if (q_) PdhCloseQuery(q_); }
    SysProbe(const SysProbe&) = delete; SysProbe& operator=(const SysProbe&) = delete;

    SysExtra Sample() {
        SysExtra x; x.physCores = phys_; x.logical = n_;
        Cores(x);
        PERFORMANCE_INFORMATION pi{}; pi.cb = sizeof(pi);
        if (GetPerformanceInfo(&pi, sizeof(pi))) {
            const uint64_t pg = pi.PageSize;
            x.memAvail = pi.PhysicalAvailable * pg; x.memCached = pi.SystemCache * pg; x.commitUsed = pi.CommitTotal * pg; x.commitLimit = pi.CommitLimit * pg;
        }
        if ((tick_++ % 5) == 0) Drives();   // capacity changes slowly
        x.drives = drives_;
        Io(x);
        return x;
    }

private:
    struct Times { LARGE_INTEGER idle, kernel, user; };
    void Cores(SysExtra& x) {
        // SystemProcessorPerformanceInformation (8): per-processor idle, kernel (includes idle) and user time.
        std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> now(n_);
        if (n_ <= 0 || NtQuerySystemInformation(static_cast<SYSTEM_INFORMATION_CLASS>(8), now.data(), static_cast<ULONG>(now.size() * sizeof(now[0])), nullptr) != 0) return;
        if (prev_.size() == now.size()) {
            x.cores.resize(now.size());
            for (size_t i = 0; i < now.size(); ++i) {
                const int64_t idle = now[i].IdleTime.QuadPart - prev_[i].idle.QuadPart;
                const int64_t all = (now[i].KernelTime.QuadPart - prev_[i].kernel.QuadPart) + (now[i].UserTime.QuadPart - prev_[i].user.QuadPart);
                x.cores[i] = all > 0 ? std::clamp(100.0 * static_cast<double>(all - idle) / static_cast<double>(all), 0.0, 100.0) : 0.0;
            }
        }
        prev_.resize(now.size());
        for (size_t i = 0; i < now.size(); ++i) prev_[i] = {now[i].IdleTime, now[i].KernelTime, now[i].UserTime};
    }
    void Drives() {
        drives_.clear();
        const wchar_t sys = static_cast<wchar_t>(towupper(WinDir().empty() ? L'C' : WinDir()[0]));
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26 && drives_.size() < 8; ++i) {
            if (!(mask & (1u << i))) continue;
            const wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
            ULARGE_INTEGER fa, tot, fr;
            if (!GetDiskFreeSpaceExW(root, &fa, &tot, &fr) || !tot.QuadPart) continue;
            DriveInfo d; d.letter = root[0]; d.total = tot.QuadPart; d.free = fr.QuadPart;
            if (d.letter == sys) drives_.insert(drives_.begin(), d); else drives_.push_back(d);
        }
    }
    void Io(SysExtra& x) {
        if (!q_ || PdhCollectQueryData(q_) != ERROR_SUCCESS) return;
        auto read = [&](PDH_HCOUNTER c, bool write) {
            if (!c) return;
            DWORD sz = 0, n = 0;
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &n, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return;
            buf_.resize(sz);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf_.data());
            if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &sz, &n, items) != ERROR_SUCCESS) return;
            for (DWORD i = 0; i < n; ++i) {
                const wchar_t* name = items[i].szName; const double v = items[i].FmtValue.doubleValue;
                if (!name || items[i].FmtValue.CStatus > 1 || v < 0) continue;   // 0 valid, 1 new data
                if (!wcscmp(name, L"_Total")) (write ? x.diskWrite : x.diskRead) = v;
                else if (name[0] && name[1] == L':' && !name[2]) for (DriveInfo& d : x.drives) if (d.letter == towupper(name[0])) (write ? d.writeBps : d.readBps) = v;
            }
        };
        read(rd_, false); read(wr_, true);
    }
    int n_ = 0, phys_ = 0; unsigned tick_ = 0;
    std::vector<Times> prev_; std::vector<DriveInfo> drives_; std::vector<uint8_t> buf_;
    PDH_HQUERY q_ = nullptr; PDH_HCOUNTER rd_ = nullptr, wr_ = nullptr;
};

}  // namespace ak
