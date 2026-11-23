// AnkhCore 2.5 — honest operation progress (LOG-2). Pure logic; unit-tested in tests/test_base.cpp.
//
// A progress bar may show exactly one of three things:
//   1. Measured: items finished out of items counted (files written, entries scanned), or the
//      percentage a Windows tool itself prints (SFC, DISM, chkdsk).
//   2. Steps: steps finished out of the steps this run will really perform. The status text says
//      "Step k of n", so the bar is read as position in a list, not as time left.
//   3. Unknown: a moving band. Used whenever nothing above is known, for example while an external
//      program runs. Never a filled bar.
// Not allowed: a number chosen by hand, a full bar for work whose length is unknown, and 100 before
// the work has finished.
#pragma once
#include <algorithm>
#include <string>
#include <cwctype>
#include <cstddef>

namespace ak {

constexpr int kProgressUnknown = -1;

// Items finished out of items counted. 100 only when all are finished; unknown when nothing was counted.
inline int ProgressOfCount(size_t done, size_t total) {
    if (total == 0) return kProgressUnknown;
    if (done >= total) return 100;
    return static_cast<int>(done * 100 / total);   // rounds down: 99 of 100 is 99, never 100
}

// Steps of one run. Next() closes the step in work and opens the following one.
struct OpSteps {
    int total = 1, finished = 0; bool open = false;
    explicit OpSteps(int n) : total(std::max(1, n)) {}
    int Current() const { return std::min(total, finished + 1); }
    void Next() { if (open && finished < total) ++finished; open = true; }
    // Finished steps, plus the measured share of the step in work (partDone of partTotal items).
    int Pct(size_t partDone = 0, size_t partTotal = 0) const {
        if (finished >= total) return 99;   // every step was opened and closed, but the caller has not reported the end yet
        long long num = static_cast<long long>(finished) * 1000;
        if (partTotal) num += static_cast<long long>(std::min(partDone, partTotal)) * 1000 / static_cast<long long>(partTotal);
        return static_cast<int>(std::min<long long>(99, num / (10LL * total)));
    }
    std::wstring Label(const std::wstring& what) const { return L"Step " + std::to_wstring(Current()) + L" of " + std::to_wstring(total) + L": " + what; }
};

// The percentage a console tool prints on a line, or kProgressUnknown.
//   "Verification 45% complete."                         -> 45   (SFC)
//   "[===========         20.0%                  ]"       -> 20   (DISM)
//   "Progress: 12 of 90 done; Stage: 13%; Total: 4%; ..." -> 4    (chkdsk: the total, not the stage)
// A line with several percentages and no "Total:" is ambiguous and gives none. A value above 100 is
// not a progress value and gives none (it is not cut down to 100).
inline int ParseToolPercent(const std::wstring& line) {
    struct Hit { size_t at; int value; };
    Hit hits[8]; int n = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != L'%') continue;
        size_t a = i;
        while (a > 0 && (iswdigit(line[a - 1]) || line[a - 1] == L'.')) --a;
        if (a == i || !iswdigit(line[a])) continue;
        if (a > 0 && (iswalpha(line[a - 1]) || line[a - 1] == L'-')) continue;   // "x86%", "-5%"
        int v = 0; bool over = false;
        for (size_t k = a; k < i && line[k] != L'.'; ++k) { v = v * 10 + (line[k] - L'0'); if (v > 100) { over = true; break; } }
        if (over) continue;
        if (n < 8) hits[n++] = {a, v};
    }
    if (n == 0) return kProgressUnknown;
    std::wstring low = line; for (auto& ch : low) ch = static_cast<wchar_t>(towlower(ch));
    const size_t tot = low.find(L"total:");
    if (tot != std::wstring::npos) { for (int i = 0; i < n; ++i) if (hits[i].at > tot) return hits[i].value; return kProgressUnknown; }
    return n == 1 ? hits[0].value : kProgressUnknown;
}

// What the bar draws inside a track of the given width. Known progress: a fill from the left.
// Unknown: a band of fixed share that travels left to right and wraps; it never covers the track.
struct ProgressFill { int left = 0, right = 0; bool band = false; };
inline ProgressFill ProgressFillFor(int pct, int width, unsigned tickMs, int minFill) {
    ProgressFill f;
    if (width <= 0) return f;
    if (pct < 0) {
        const int w = std::max(1, width * 3 / 10), span = width + w;
        const int x = static_cast<int>(static_cast<unsigned long long>(tickMs % 1600u) * static_cast<unsigned>(span) / 1600u) - w;
        f.band = true; f.left = std::max(0, x); f.right = std::min(width, x + w);
        return f;
    }
    if (pct == 0) return f;
    f.right = std::min(width, std::max(std::min(minFill, width), width * std::min(pct, 100) / 100));
    return f;
}

}   // namespace ak
