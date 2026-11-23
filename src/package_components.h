// AnkhCore 2.5 — optional components shipped beside the setup (icon themes, the Resource Redirect mod, the
// Taskbar Profile). Pure logic, unit-tested in tests/test_base.cpp.
//
// A test package is a folder:  <setup>.exe, COMPONENTS.sha256, IconThemes\<theme>\..., Mods\..., *.reg
// COMPONENTS.sha256 lists every component file once:  <64 lowercase hex>  <path relative to the package>
// The setup uses a component only if every one of its files is listed, present and unchanged, and no file
// that is not listed sits in it. Lines starting with '#' are comments.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <cwctype>

namespace ak {

struct ComponentManifest {
    std::map<std::wstring, std::string> sha;   // key: relative path, backslashes, lower case
};

// "IconThemes/bigsur\theme.ini" -> "iconthemes\bigsur\theme.ini"; empty when the path is absolute, empty, or leaves the package.
inline std::wstring ComponentKey(std::wstring p) {
    for (auto& ch : p) { if (ch == L'/') ch = L'\\'; ch = static_cast<wchar_t>(towlower(ch)); }
    if (p.empty() || p[0] == L'\\' || (p.size() > 1 && p[1] == L':')) return {};
    size_t at = 0;
    while (at <= p.size()) {
        const size_t e = p.find(L'\\', at);
        const std::wstring part = p.substr(at, e == std::wstring::npos ? std::wstring::npos : e - at);
        if (part.empty() || part == L"." || part == L"..") return {};
        if (e == std::wstring::npos) break;
        at = e + 1;
    }
    return p;
}

inline bool ParseComponentManifest(const std::string& text, ComponentManifest& out, std::wstring& err) {
    out.sha.clear(); err.clear();
    size_t at = 0; int lineNo = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at); if (e == std::string::npos) e = text.size();
        std::string l = text.substr(at, e - at); at = e + 1; ++lineNo;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (lineNo == 1 && l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF) l = l.substr(3);   // UTF-8 BOM
        if (l.empty() || l[0] == '#') continue;
        if (l.size() < 67 || l[64] != ' ' || l[65] != ' ') { err = L"line " + std::to_wstring(lineNo) + L": expected '<sha256>  <path>'"; return false; }
        const std::string hex = l.substr(0, 64);
        for (char c : hex) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { err = L"line " + std::to_wstring(lineNo) + L": the hash is not 64 lower-case hex digits"; return false; }
        std::wstring path;
        for (size_t i = 66; i < l.size(); ++i) {   // UTF-8 path; component names are ASCII, anything else is refused
            const unsigned char c = static_cast<unsigned char>(l[i]);
            if (c < 0x20 || c > 0x7E) { err = L"line " + std::to_wstring(lineNo) + L": the path has characters other than printable ASCII"; return false; }
            path += static_cast<wchar_t>(c);
        }
        const std::wstring key = ComponentKey(path);
        if (key.empty()) { err = L"line " + std::to_wstring(lineNo) + L": the path is absolute or leaves the package"; return false; }
        if (!out.sha.emplace(key, hex).second) { err = L"line " + std::to_wstring(lineNo) + L": the path is listed twice"; return false; }
    }
    if (out.sha.empty()) { err = L"the manifest lists no file"; return false; }
    return true;
}

// Manifest entries inside a folder of the package ("IconThemes\\sweet") or one exact file ("Mods\\x.wh.cpp").
inline std::vector<std::wstring> ComponentEntriesUnder(const ComponentManifest& m, const std::wstring& relPath) {
    std::vector<std::wstring> out;
    const std::wstring k = ComponentKey(relPath);
    if (k.empty()) return out;
    for (const auto& kv : m.sha)
        if (kv.first == k || (kv.first.size() > k.size() && kv.first.compare(0, k.size(), k) == 0 && kv.first[k.size()] == L'\\')) out.push_back(kv.first);
    return out;
}

// Compares what is on disk with the manifest for one component. `present`: relative paths actually found there,
// `hashOf`: the hash of each (empty when unreadable). Returns an empty string when they agree, else the first problem.
template <class HashFn>
inline std::wstring CheckComponent(const ComponentManifest& m, const std::wstring& relPath, const std::vector<std::wstring>& present, HashFn hashOf) {
    const std::vector<std::wstring> listed = ComponentEntriesUnder(m, relPath);
    if (listed.empty()) return L"not listed in COMPONENTS.sha256";
    std::map<std::wstring, bool> seen;
    for (const auto& p : present) {
        const std::wstring k = ComponentKey(p);
        if (k.empty() || !m.sha.count(k)) return L"unlisted file " + p;
        seen[k] = true;
    }
    for (const auto& k : listed) {
        if (!seen.count(k)) return L"missing file " + k;
        if (hashOf(k) != m.sha.at(k)) return L"changed file " + k + L" (SHA-256 differs)";
    }
    return {};
}

}   // namespace ak
