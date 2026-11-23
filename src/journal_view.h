// FT-RH: the change journal as operations. The journal stores one entry per registry value (or service, or file), so a single
// cursor-scheme apply is 18-20 entries with the same title, and the Recovery History listed every one of them as its own row.
// Measured on the owner's journal (2026-11-15): 322 entries were 54 operations, 18 operations accounted for 286 rows.
// An operation is identified by its op id (the id of its first entry, recorded since FT-RH), never by its displayed name, so two
// different operations with the same name stay two rows. Entries written before FT-RH have no op id; they are grouped by a rule
// that needs consecutive ids, the same group and title, and at most 2 seconds between neighbours. Nothing is deleted or rewritten.
// Pure: no Windows calls (tests: tests\test_base.cpp).
#pragma once
#include "base.h"
#include <map>
#include <set>

namespace ak {

inline long long JournalTimeSeconds(const std::wstring& t) {   // "YYYY-MM-DD HH:MM:SS" -> seconds on a common scale; -1 when it is not that
    if (t.size() < 19 || t[4] != L'-' || t[7] != L'-' || t[13] != L':' || t[16] != L':') return -1;
    auto num = [&](size_t a, size_t n) { int v = 0; for (size_t i = a; i < a + n; ++i) { if (t[i] < L'0' || t[i] > L'9') return -1; v = v * 10 + (t[i] - L'0'); } return v; };
    int y = num(0, 4); const int m = num(5, 2), d = num(8, 2), hh = num(11, 2), mm = num(14, 2), ss = num(17, 2);
    if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || mm < 0 || ss < 0) return -1;
    y -= m <= 2; const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (static_cast<long long>(era) * 146097 + doe - 719468) * 86400 + hh * 3600 + mm * 60 + ss;
}

inline bool JournalIsInformationalKind(const std::wstring& kind) {
    return kind == L"RECOVERY_POINT" || kind == L"SNAPSHOT_RESTORE";
}

struct JournalOpRow {
    int key = 0;                        // the op id when it was recorded, else the id of the first entry
    bool exact = false;                 // recorded op id (true) or grouped from older entries by the rule (false)
    bool informational = false;         // RECOVERY_POINT / SNAPSHOT_RESTORE: excluded from Undo and Undo All (B2 / FC-28)
    std::vector<size_t> idx;            // indexes into the entries the view was built from, oldest first
    std::vector<int> ids;               // their ids
    std::wstring title, group, time, kind;    // of the first entry
    int active = 0, undone = 0;
};
struct JournalView {
    std::vector<JournalOpRow> ops;      // newest operation first
    size_t entries = 0; int activeEntries = 0;
};

inline JournalView JournalBuildView(const std::vector<JEntry>& all) {
    JournalView v; v.entries = all.size();
    std::vector<JournalOpRow> rows; std::map<int, size_t> byOp;
    size_t prevLegacy = static_cast<size_t>(-1);   // the row the previous entry joined, when that entry had no op id
    for (size_t i = 0; i < all.size(); ++i) {
        const JEntry& e = all[i];
        const bool info = JournalIsInformationalKind(e.kind);
        if (e.state == L"A" && !info) ++v.activeEntries;
        size_t row;
        if (e.op != 0) {
            const auto it = byOp.find(e.op);
            if (it != byOp.end()) row = it->second;
            else { JournalOpRow r; r.key = e.op; r.exact = true; r.informational = info; r.title = e.title; r.group = e.group; r.time = e.time; r.kind = e.kind; rows.push_back(r); row = rows.size() - 1; byOp[e.op] = row; }
            prevLegacy = static_cast<size_t>(-1);
        } else {
            bool join = false;
            if (prevLegacy != static_cast<size_t>(-1) && i > 0) {
                const JEntry& p = all[i - 1];
                const long long a = JournalTimeSeconds(p.time), b = JournalTimeSeconds(e.time);
                join = p.op == 0 && e.id == p.id + 1 && e.group == p.group && e.title == p.title && a >= 0 && b >= a && b - a <= 2;
            }
            if (join) row = prevLegacy;
            else { JournalOpRow r; r.key = e.id; r.exact = false; r.informational = info; r.title = e.title; r.group = e.group; r.time = e.time; r.kind = e.kind; rows.push_back(r); row = rows.size() - 1; }
            prevLegacy = row;
        }
        JournalOpRow& r = rows[row];
        r.idx.push_back(i); r.ids.push_back(e.id);
        if (!info) (e.state == L"A" ? r.active : r.undone)++;
    }
    v.ops.assign(rows.rbegin(), rows.rend());
    return v;
}

// The entries of one operation, for undoing it as a whole (never picks informational kinds).
inline std::function<bool(const JEntry&)> JournalPickOp(const JournalOpRow& row) {
    const std::set<int> ids(row.ids.begin(), row.ids.end());
    return [ids](const JEntry& e) { return !JournalIsInformationalKind(e.kind) && ids.count(e.id) != 0; };
}

// Picks every undoable entry for "Undo all", excluding RECOVERY_POINT / SNAPSHOT_RESTORE (B2 / FC-28).
inline std::function<bool(const JEntry&)> JournalPickUndoAll() {
    return [](const JEntry& e) { return !JournalIsInformationalKind(e.kind); };
}

}  // namespace ak
