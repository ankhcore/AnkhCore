// eng_proglist.h - 2.8.6: a list of the installed programs, saved with `winget export` and used again with `winget import`.
//
// It is the "backup for programs" the person asked for: the NAMES and ids of the installed programs (so a fresh Windows can get them back), not their data.
// Pure over strings and a directory, so tests run on a temp tree; the real winget calls are made by the page through Run().
#pragma once
#include "base.h"

namespace ak {

inline std::wstring ProgListDir(const std::wstring& backupDir) { return backupDir + L"\\Programs"; }
inline std::wstring ProgListFileName(const std::wstring& stamp) { return L"Programs-" + stamp + L".json"; }
// Every export entry has a "PackageIdentifier"; counting them is the honest size of the list.
inline int ProgListCount(const std::string& json) {
    int n = 0; const std::string key = "\"PackageIdentifier\"";
    for (size_t p = json.find(key); p != std::string::npos; p = json.find(key, p + key.size())) ++n;
    return n;
}
inline std::wstring ProgExportArgs(const std::wstring& path) { return L"export -o \"" + path + L"\" --accept-source-agreements --disable-interactivity"; }
inline std::wstring ProgImportArgs(const std::wstring& path) { return L"import -i \"" + path + L"\" --ignore-unavailable --ignore-versions --accept-package-agreements --accept-source-agreements --disable-interactivity"; }

struct ProgListInfo { std::wstring name, path; int count = 0; unsigned long long bytes = 0; };
// The saved lists, newest first (the stamp in the name sorts as text). A file that is not a list (no entries) is not offered.
inline std::vector<ProgListInfo> ProgListFiles(const std::wstring& dir, size_t maxCount = 6) {
    std::vector<ProgListInfo> out;
    if (!DirExists(dir)) return out;
    std::vector<std::wstring> names = ListDir(dir, false, L"Programs-*.json");
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) > Lower(b); });
    for (const std::wstring& n : names) {
        if (out.size() >= maxCount) break;
        ProgListInfo i; i.name = n; i.path = dir + L"\\" + n;
        const std::string t = ReadText(i.path);
        i.count = ProgListCount(t); i.bytes = t.size();
        if (i.count > 0) out.push_back(i);
    }
    return out;
}

}  // namespace ak
