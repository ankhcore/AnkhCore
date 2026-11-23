// AnkhCore 2.5 — Cursor Manager (APP-4): roles, file-name mapping, validation, the Windows scheme
// list and the saved previous scheme. Pure logic on strings and byte buffers; unit-tested in
// tests/test_base.cpp. The registry and the file system are in eng_cursors.h.
//
// Windows has 17 cursor roles (HKCU\Control Panel\Cursors). Packs name their files in two ways:
//   - by the registry role:        Arrow.ani, cross.ani, NO.ani, SizeNWSE.ani
//   - by the Control Panel label:  "[SB] Normal Select v3.0.ani", "[PREMIUM] [TB] Busy v3.2.ani"
// Tags in square brackets and the version are not part of the name. A file whose name carries a
// " - <variant>" suffix, or that lies in a sub-folder, is an alternate: it is listed, never chosen
// automatically. A name that matches no role is reported as unassigned; nothing is guessed.
#pragma once
#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cwctype>

namespace ak {

constexpr int kCurRoles = 17;
struct CurRoleInfo {
    const wchar_t* key;     // value name under Control Panel\Cursors
    const wchar_t* label;   // the name Windows shows in Mouse Properties
    const wchar_t* aero;    // file of the Windows default scheme in %SystemRoot%\cursors ("" = built into Windows)
    const wchar_t* alias;   // further file names seen in real packs, '|' separated, lower case
};
// In the order of a scheme's comma-separated list under Control Panel\Cursors\Schemes.
inline const std::array<CurRoleInfo, kCurRoles>& CurRoleTable() {
    static const std::array<CurRoleInfo, kCurRoles> t = {{
        {L"Arrow",       L"Normal Select",         L"aero_arrow.cur",   L""},
        {L"Help",        L"Help Select",           L"aero_helpsel.cur", L""},
        {L"AppStarting", L"Working In Background", L"aero_working.ani", L""},
        {L"Wait",        L"Busy",                  L"aero_busy.ani",    L""},
        {L"Crosshair",   L"Precision Select",      L"",                 L"cross"},
        {L"IBeam",       L"Text Select",           L"",                 L""},
        {L"NWPen",       L"Handwriting",           L"aero_pen.cur",     L""},
        {L"No",          L"Unavailable",           L"aero_unavail.cur", L""},
        {L"SizeNS",      L"Vertical Resize",       L"aero_ns.cur",      L""},
        {L"SizeWE",      L"Horizontal Resize",     L"aero_ew.cur",      L""},
        {L"SizeNWSE",    L"Diagonal Resize 1",     L"aero_nwse.cur",    L""},
        {L"SizeNESW",    L"Diagonal Resize 2",     L"aero_nesw.cur",    L""},
        {L"SizeAll",     L"Move",                  L"aero_move.cur",    L""},
        {L"UpArrow",     L"Alternate Select",      L"aero_up.cur",      L""},
        {L"Hand",        L"Link Select",           L"aero_link.cur",    L""},
        {L"Pin",         L"Location Select",       L"aero_pin.cur",     L""},
        {L"Person",      L"Person Select",         L"aero_person.cur",  L""},
    }};
    return t;
}

inline std::wstring CurLower(std::wstring s) { for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch)); return s; }
inline std::wstring CurTrim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}
inline std::wstring CurSquash(const std::wstring& s) {   // runs of spaces become one
    std::wstring o; bool sp = false;
    for (wchar_t ch : s) { if (iswspace(ch)) { sp = !o.empty(); continue; } if (sp) o += L' '; sp = false; o += ch; }
    return o;
}

// Role index for a bare name ("normal select", "arrow", "cross"); -1 when it names no role.
inline int CurRoleOfName(const std::wstring& bare) {
    const std::wstring n = CurLower(CurSquash(bare));
    if (n.empty()) return -1;
    const auto& t = CurRoleTable();
    for (int i = 0; i < kCurRoles; ++i) {
        if (n == CurLower(t[i].key) || n == CurLower(t[i].label)) return i;
        std::wstring al = t[i].alias; size_t at = 0;
        while (at <= al.size() && !al.empty()) {
            const size_t e = al.find(L'|', at);
            if (n == al.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at)) return i;
            if (e == std::wstring::npos) break;
            at = e + 1;
        }
    }
    return -1;
}

struct CurName {
    std::wstring base, variant;        // "Normal Select", "Glow"
    std::vector<std::wstring> tags;    // "PREMIUM", "SB", "Stat", in file order
    std::wstring version;              // "3.2"
    bool ani = false, cursorFile = false, premium = false;
    int role = -1;
};
// Takes a file name (no folder) apart. cursorFile is false for anything that is not .ani or .cur.
inline CurName CurParseName(const std::wstring& fileName) {
    CurName r;
    const size_t dot = fileName.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"" : CurLower(fileName.substr(dot));
    r.ani = ext == L".ani"; r.cursorFile = r.ani || ext == L".cur";
    std::wstring stem = dot == std::wstring::npos ? fileName : fileName.substr(0, dot), rest;
    for (size_t i = 0; i < stem.size(); ++i) {   // [tags] anywhere in the name
        if (stem[i] == L'[') {
            const size_t e = stem.find(L']', i);
            if (e != std::wstring::npos) { const std::wstring t = CurTrim(stem.substr(i + 1, e - i - 1)); if (!t.empty()) r.tags.push_back(t); rest += L' '; i = e; continue; }
        }
        rest += stem[i];
    }
    for (const auto& t : r.tags) if (CurLower(t) == L"premium") r.premium = true;
    // " - variant" (the dash stands alone)
    const size_t dash = rest.find(L" - ");
    if (dash != std::wstring::npos) { r.variant = CurSquash(rest.substr(dash + 3)); rest = rest.substr(0, dash); }
    // a version word: v3, v3.2, v3.0.1
    std::wstring out;
    const std::wstring sq = CurSquash(rest);
    size_t at = 0;
    while (at < sq.size()) {
        size_t e = sq.find(L' ', at); if (e == std::wstring::npos) e = sq.size();
        const std::wstring w = sq.substr(at, e - at);
        bool ver = w.size() >= 2 && (w[0] == L'v' || w[0] == L'V') && iswdigit(w[1]);
        for (size_t i = 1; ver && i < w.size(); ++i) ver = iswdigit(w[i]) || w[i] == L'.';
        if (ver && r.version.empty()) r.version = w.substr(1);
        else { if (!out.empty()) out += L' '; out += w; }
        at = e + 1;
    }
    r.base = out;
    r.role = r.cursorFile ? CurRoleOfName(r.base) : -1;
    return r;
}

// .cur: ICONDIR with type 2 and at least one image. .ani: RIFF container of type ACON.
inline bool CurValidHeader(const uint8_t* p, size_t n, bool ani) {
    if (!p) return false;
    if (ani) return n >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' && p[8] == 'A' && p[9] == 'C' && p[10] == 'O' && p[11] == 'N';
    return n >= 22 && p[0] == 0 && p[1] == 0 && p[2] == 2 && p[3] == 0 && (p[4] | (p[5] << 8)) >= 1;
}

struct CurScheme {
    std::wstring name;                          // folder name, or the registered scheme's name
    std::wstring dir;                           // folder; empty for a scheme read from the registry
    bool registered = false;                    // listed under Control Panel\Cursors\Schemes
    std::array<std::wstring, kCurRoles> file;   // per role: path relative to dir (full path when registered); empty = not provided
    std::vector<std::wstring> alternates;       // variants and sub-folder files (relative paths)
    std::vector<std::wstring> unassigned;       // cursor files whose name matches no role
    std::vector<int> aliased;                   // roles filled from an unassigned file by CurSchemeAliases (FT-F01), shown to the user as such
    std::vector<std::wstring> invalid;          // mapped files that are missing or not a cursor (filled by the engine)
    std::vector<std::wstring> tags;             // distinct tags other than PREMIUM / Stat, e.g. "SB", "TB", "PR"
    bool premium = false;
    int Mapped() const { int n = 0; for (const auto& f : file) n += !f.empty(); return n; }
    std::vector<int> Missing() const { std::vector<int> m; for (int i = 0; i < kCurRoles; ++i) if (file[i].empty()) m.push_back(i); return m; }
};

// FT-F01 (owner finding): a pack that names its Normal Select cursor differently leaves Windows with its white arrow.
// An alias is chosen only from evidence about the pack's own files, never invented: Ankhcore Azenis has no Arrow file;
// its only unmapped cursor, button.ani, has the same 32x32 frames, frame count and hotspot (8, 3) as every pointer in
// the pack (checked in the file headers, docs/preview2.1/evidence/FT-F01_azenis_headers.txt), so it is the pack's pointer
// for the missing role. The file is used as it is: nothing is converted, recoloured or copied.
struct CurAlias { const wchar_t* schemeContains; const wchar_t* fileBase; int role; };
// Ankhcore scursor_green (2026-11-15) names its files SCursor_*; its install.inf (shipped in the pack folder) lists the
// registry scheme in role order: pointer, help, work, busy, cross, text, hand(=Handwriting), unavailable, vert, horz,
// dgn1, dgn2, move, alternate, link. The rows below follow that list exactly (15 roles; Pin and Person are not provided).
inline const std::array<CurAlias, 16>& CurSchemeAliases() {
    static const std::array<CurAlias, 16> a = {{
        {L"azenis", L"button", 0 /* Arrow = Normal Select */},
        {L"scursor_green", L"scursor_point", 0}, {L"scursor_green", L"scursor_wtf", 1}, {L"scursor_green", L"scursor_work", 2},
        {L"scursor_green", L"scursor_timer", 3}, {L"scursor_green", L"scursor_x", 4}, {L"scursor_green", L"scursor_text", 5},
        {L"scursor_green", L"scursor_pen", 6}, {L"scursor_green", L"scursor_no", 7}, {L"scursor_green", L"scursor_ud", 8},
        {L"scursor_green", L"scursor_ir", 9}, {L"scursor_green", L"scursor_nw", 10}, {L"scursor_green", L"scursor_ne", 11},
        {L"scursor_green", L"scursor_move", 12}, {L"scursor_green", L"scursor_alt", 13}, {L"scursor_green", L"scursor_hand", 14},
    }};
    return a;
}

// Maps a pack's files to roles. relFiles: paths relative to the scheme folder ("Busy.ani", "Bonus\\x.ani").
// For one role the top-level file without a variant wins; .ani before .cur; then the name order.
inline CurScheme CurMapScheme(const std::wstring& name, const std::wstring& dir, std::vector<std::wstring> relFiles) {
    CurScheme s; s.name = name; s.dir = dir;
    std::sort(relFiles.begin(), relFiles.end(), [](const std::wstring& a, const std::wstring& b) { return CurLower(a) < CurLower(b); });
    std::array<bool, kCurRoles> isAni{};
    for (const auto& rel : relFiles) {
        const size_t sl = rel.find_last_of(L'\\');
        const CurName n = CurParseName(sl == std::wstring::npos ? rel : rel.substr(sl + 1));
        if (!n.cursorFile) continue;
        s.premium |= n.premium;
        for (const auto& t : n.tags) {
            const std::wstring lt = CurLower(t);
            if (lt == L"premium" || lt == L"stat") continue;
            if (std::find(s.tags.begin(), s.tags.end(), t) == s.tags.end()) s.tags.push_back(t);
        }
        const bool top = sl == std::wstring::npos;
        if (n.role < 0) { (top && n.variant.empty() ? s.unassigned : s.alternates).push_back(rel); continue; }
        if (!top || !n.variant.empty()) { s.alternates.push_back(rel); continue; }
        if (s.file[n.role].empty()) { s.file[n.role] = rel; isAni[n.role] = n.ani; }
        else if (n.ani && !isAni[n.role]) { s.alternates.push_back(s.file[n.role]); s.file[n.role] = rel; isAni[n.role] = true; }
        else s.alternates.push_back(rel);
    }
    const std::wstring ln = CurLower(name + L" " + dir);
    for (const CurAlias& al : CurSchemeAliases()) {
        if (!s.file[al.role].empty() || ln.find(al.schemeContains) == std::wstring::npos) continue;
        for (auto it = s.unassigned.begin(); it != s.unassigned.end(); ++it) {
            std::wstring b = CurLower(*it); const size_t dot = b.find_last_of(L'.'); if (dot != std::wstring::npos) b.resize(dot);
            if (b != al.fileBase) continue;
            s.file[al.role] = *it; s.aliased.push_back(al.role); s.unassigned.erase(it); break;
        }
    }
    return s;
}

// A registered scheme's value: up to 17 comma-separated paths in role order (older schemes have fewer).
inline std::array<std::wstring, kCurRoles> CurParseSchemeList(const std::wstring& csv) {
    std::array<std::wstring, kCurRoles> out;
    size_t at = 0;
    for (int i = 0; i < kCurRoles && at <= csv.size(); ++i) {
        const size_t e = csv.find(L',', at);
        out[i] = CurTrim(csv.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at));
        if (e == std::wstring::npos) break;
        at = e + 1;
    }
    return out;
}
inline std::wstring CurBuildSchemeList(const std::array<std::wstring, kCurRoles>& v) {
    std::wstring s;
    for (int i = 0; i < kCurRoles; ++i) { if (i) s += L','; s += v[i]; }
    return s;
}

// "C:\Windows\Cursors\x.ani" -> "%SystemRoot%\Cursors\x.ani" (what Windows itself stores); other paths unchanged.
inline std::wstring CurPortablePath(const std::wstring& full, const std::wstring& winDir) {
    if (!winDir.empty() && full.size() > winDir.size() && CurLower(full.substr(0, winDir.size())) == CurLower(winDir) && full[winDir.size()] == L'\\')
        return L"%SystemRoot%" + full.substr(winDir.size());
    return full;
}

// What is written for a scheme: one value per role. A role the scheme does not provide gets the
// Windows default cursor, so no role is left pointing at another scheme's file.
struct CurState {
    std::wstring name;                           // the key's default value (scheme name)
    uint32_t source = 0; bool hasSource = false; // "Scheme Source": 0 none, 1 user scheme, 2 system scheme
    std::array<std::wstring, kCurRoles> value;
    std::array<bool, kCurRoles> exists{};
    bool operator==(const CurState& o) const { return name == o.name && source == o.source && hasSource == o.hasSource && value == o.value && exists == o.exists; }
};
inline std::wstring CurDefaultValue(int role) {
    const wchar_t* a = CurRoleTable()[role].aero;
    return *a ? std::wstring(L"%SystemRoot%\\cursors\\") + a : std::wstring();
}
inline CurState CurWindowsDefault() {
    CurState s; s.name = L"Windows Default"; s.source = 2; s.hasSource = true;
    for (int i = 0; i < kCurRoles; ++i) { s.value[i] = CurDefaultValue(i); s.exists[i] = true; }
    return s;
}
inline CurState CurTargetState(const CurScheme& sc, const std::wstring& winDir) {
    CurState s; s.name = sc.name; s.source = 1; s.hasSource = true;
    for (int i = 0; i < kCurRoles; ++i) {
        s.exists[i] = true;
        if (sc.file[i].empty()) s.value[i] = CurDefaultValue(i);
        else s.value[i] = CurPortablePath(sc.registered || sc.dir.empty() ? sc.file[i] : sc.dir + L"\\" + sc.file[i], winDir);
    }
    return s;
}

// The saved previous scheme, as text (UTF-16 in memory; the engine stores it as UTF-8):
//   ankhcore-cursor-scheme 1
//   name=<scheme name>
//   source=<0|1|2|->
//   <Role>=<1|0>|<value>          one line per role, all 17, in table order
inline std::wstring CurEncodeState(const CurState& s) {
    std::wstring o = L"ankhcore-cursor-scheme 1\n";
    o += L"name=" + s.name + L"\n";
    o += L"source=" + (s.hasSource ? std::to_wstring(s.source) : std::wstring(L"-")) + L"\n";
    for (int i = 0; i < kCurRoles; ++i) o += std::wstring(CurRoleTable()[i].key) + L"=" + (s.exists[i] ? L"1|" : L"0|") + s.value[i] + L"\n";
    return o;
}
// Strict: the exact header, every role exactly once and in order, no control characters, bounded lengths.
inline bool CurDecodeState(const std::wstring& text, CurState& out) {
    std::vector<std::wstring> lines; size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find(L'\n', at); if (e == std::wstring::npos) e = text.size();
        std::wstring l = text.substr(at, e - at); if (!l.empty() && l.back() == L'\r') l.pop_back();
        lines.push_back(l); at = e + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() != 3 + kCurRoles || lines[0] != L"ankhcore-cursor-scheme 1") return false;
    for (const auto& l : lines) { if (l.size() > 1024) return false; for (wchar_t ch : l) if (ch < 0x20) return false; }
    CurState s;
    if (lines[1].rfind(L"name=", 0) != 0 || lines[2].rfind(L"source=", 0) != 0) return false;
    s.name = lines[1].substr(5);
    const std::wstring src = lines[2].substr(7);
    if (src == L"-") s.hasSource = false;
    else if (src == L"0" || src == L"1" || src == L"2") { s.hasSource = true; s.source = static_cast<uint32_t>(src[0] - L'0'); }
    else return false;
    for (int i = 0; i < kCurRoles; ++i) {
        const std::wstring& l = lines[3 + i];
        const std::wstring pre = std::wstring(CurRoleTable()[i].key) + L"=";
        if (l.rfind(pre, 0) != 0 || l.size() < pre.size() + 2 || l[pre.size() + 1] != L'|') return false;
        const wchar_t ex = l[pre.size()];
        if (ex != L'0' && ex != L'1') return false;
        s.exists[i] = ex == L'1'; s.value[i] = l.substr(pre.size() + 2);
        if (!s.exists[i] && !s.value[i].empty()) return false;
    }
    out = s;
    return true;
}

// Which discovered scheme the current registry state is: every role equal, compared without case
// and with %SystemRoot% expanded by the caller. -1 = none (a custom mix or the Windows default).
inline int CurFindActive(const std::vector<CurState>& targets, const CurState& now) {
    for (size_t t = 0; t < targets.size(); ++t) {
        bool same = true;
        for (int i = 0; i < kCurRoles && same; ++i) same = CurLower(targets[t].value[i]) == CurLower(now.exists[i] ? now.value[i] : std::wstring());
        if (same) return static_cast<int>(t);
    }
    return -1;
}

// Valid role count of a candidate scheme (0 if any mapped file is missing/invalid).
inline int CurSchemeValidRoleCount(const CurScheme& sc) {
    if (!sc.invalid.empty()) return 0;
    return sc.Mapped();
}

// Selects the best valid candidate matching `wantName` (rejecting empty, invalid, or incomplete duplicates).
inline int CurSelectBestNamedScheme(const std::vector<CurScheme>& candidates, const std::wstring& wantName) {
    const std::wstring want = CurLower(CurTrim(wantName));
    int bestIdx = -1;
    int bestValid = 0;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
        const CurScheme& sc = candidates[i];
        const std::wstring nm = CurLower(CurTrim(sc.name));
        const bool nameMatch = (nm == want) || (want == L"as3ad" && (nm == L"as3ad2" || nm.find(L"prismatic") != std::wstring::npos));
        if (!nameMatch) continue;
        const int valid = CurSchemeValidRoleCount(sc);
        if (valid <= 0) continue;
        // Prefer exact name match when valid role counts are equal, and always prefer all 17 roles.
        const int score = valid * 10 + (nm == want ? 5 : 0);
        if (score > bestValid) {
            bestValid = score;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// Deduplicates discovered schemes that share the same display name (keeping the one with the most valid roles)
// or where "As3aD2" is an identical alias of a valid 17-role "As3aD" scheme, without deleting registry entries.
inline std::vector<CurScheme> CurFilterDuplicateNamedSchemes(const std::vector<CurScheme>& schemes) {
    std::vector<CurScheme> out;
    for (const auto& sc : schemes) {
        const std::wstring nm = CurLower(CurTrim(sc.name));
        int existingIdx = -1;
        for (int i = 0; i < static_cast<int>(out.size()); ++i) {
            const std::wstring enm = CurLower(CurTrim(out[i].name));
            if (enm == nm || ((nm == L"as3ad2" && enm == L"as3ad") || (nm == L"as3ad" && enm == L"as3ad2"))) {
                existingIdx = i;
                break;
            }
        }
        if (existingIdx < 0) {
            out.push_back(sc);
        } else {
            const int curScore = CurSchemeValidRoleCount(sc) * 10 + (nm == L"as3ad" ? 5 : 0);
            const std::wstring enm = CurLower(CurTrim(out[existingIdx].name));
            const int oldScore = CurSchemeValidRoleCount(out[existingIdx]) * 10 + (enm == L"as3ad" ? 5 : 0);
            if (curScore > oldScore) {
                out[existingIdx] = sc;
                if (nm == L"as3ad2" && enm == L"as3ad") out[existingIdx].name = L"As3aD";
            }
        }
    }
    return out;
}

// Determines whether first-run default cursor initialization should apply the verified 17-role As3aD scheme.
// Preserves any existing valid user custom cursor configuration.
inline bool CurStateNeedsFirstRunDefault(const CurState& now, int validNonEmptyRoles) {
    const std::wstring nm = CurLower(CurTrim(now.name));
    if (nm.empty() || nm == L"windows default" || nm == L"(none)" || nm.rfind(L"windows ", 0) == 0) return true;   // 2.10: every stock Windows scheme (Default, Aero, Standard, Black ...) counts as "no custom pointers yet"
    // If "As3aD" is set as the name but has fewer than 17 valid roles (e.g. 2/17 Custom mix), repair it.
    if ((nm == L"as3ad" || nm == L"as3ad2") && validNonEmptyRoles < kCurRoles) return true;
    return false;
}

// 2.10 (owner): the exact "As3aD" pointer scheme he uses, built from the packs the setup puts in %SystemRoot%\Cursors. Folder discovery cannot find it on a clean PC
// (the Cursors root itself holds the Windows files, so its sub-folders are never listed), which is why the pointers were not applied automatically.
inline CurScheme CurOwnerDefaultScheme(const std::wstring& cursorsDir) {
    CurScheme sc; sc.name = L"As3aD"; sc.dir = cursorsDir; sc.registered = false;
    const std::wstring P = L"Ankhcore Prismatic\\", D = L"DIM v3.2 - Prismatic [PREMIUM]\\", S = P + L"Bonus\\Statics\\";
    const wchar_t* t = L"[PREMIUM] [PR] ";
    auto ani = [&](const std::wstring& dir, const wchar_t* n) { return dir + t + n + L" v3.2.ani"; };
    auto stat = [&](const wchar_t* n) { return S + L"[PREMIUM] [Stat] [PR] " + n + L" v3.2.cur"; };
    sc.file = {P + L"Bonus\\" + t + L"Link Select v3.2 - Glow.ani", ani(P, L"Help Select"), ani(P, L"Working In Background"), ani(P, L"Busy"), ani(P, L"Precision Select"), ani(P, L"Text Select"),
               ani(P, L"Handwriting"), ani(P, L"Unavailable"), stat(L"Vertical Resize"), stat(L"Horizontal Resize"), stat(L"Diagonal Resize 1"), stat(L"Diagonal Resize 2"), stat(L"Move"),
               ani(P, L"Alternate Select"), ani(P, L"Link Select"), ani(D, L"Location Select"), ani(D, L"Person Select")};
    return sc;
}

}   // namespace ak
