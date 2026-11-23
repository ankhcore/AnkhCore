// Isolated WT-01 unit tests: Tweaks() discovery, categories, confirmation flags,
// RegTweak state detection, and JournalUndoEx rollback under an isolated HKCU sandbox key.
// Never modifies real Windows tweaks or HKLM.
#define ANKH_TEST_BUILD 1
#include "eng_system.h"
#include <cassert>
#include <cstdio>
#include <set>

static void CleanupSandbox(const std::wstring& jpath) {
    const std::wstring k = std::wstring(ak::kPrefKey) + L"\\WT01Sandbox";
    RegDeleteTreeW(HKEY_CURRENT_USER, k.c_str());
    DeleteFileW(jpath.c_str());
}

int main() {
    const std::wstring testJournal = ak::DataDir() + L"\\wt01_test_journal.txt";
    ak::EnsureDir(ak::DataDir());
    ak::JournalPathOverride() = testJournal;
    CleanupSandbox(testJournal);

    ak::RegisterSystemUndo();

    const auto& tw = ak::Tweaks();
    assert(tw.size() == 22);

    std::set<std::wstring> ids;
    std::set<std::wstring> cats;
    const std::set<std::wstring> allowedCats = {
        L"Performance", L"Gaming", L"Privacy", L"Power & Storage", L"Windows UI"
    };

    int wt01Count = 0;
    for (const auto& t : tw) {
        assert(!t.id.empty());
        assert(!t.title.empty());
        assert(!t.desc.empty());
        assert(allowedCats.count(t.category) == 1);
        assert(t.isOn && t.on && t.off);
        assert(ids.insert(t.id).second);
        cats.insert(t.category);
        if (t.confirm) ++wt01Count;
    }
    assert(cats.size() == allowedCats.size());
    assert(wt01Count == 10);

    // Read-only state detection check on real Tweaks() (does not write anything)
    for (const auto& t : tw) {
        (void)t.isOn();
    }

    // Isolated sandbox test for RegTweak + JournalUndoEx rollback
    const std::wstring sbKeyStr = std::wstring(ak::kPrefKey) + L"\\WT01Sandbox";
    const wchar_t* kSandboxKey = sbKeyStr.c_str();
    assert(ak::RegSetDword(HKEY_CURRENT_USER, kSandboxKey, L"TestVal", 1));

    ak::Tweak sb = ak::RegTweak(
        L"wt01_sandbox_test",
        L"Sandbox Test Tweak",
        L"Isolated rollback test",
        {{HKEY_CURRENT_USER, kSandboxKey, L"TestVal", REG_DWORD, 0, nullptr},
         {HKEY_CURRENT_USER, kSandboxKey, L"TestStr", REG_SZ, 0, L"Deny"}},
        false, L"Privacy", true);

    assert(sb.confirm == true);
    assert(sb.isOn() == false);
    assert(sb.off() == false); // unapplied tweak must refuse undo safely
    assert(sb.on() == true);
    assert(sb.isOn() == true);
    assert(ak::RegDword(HKEY_CURRENT_USER, kSandboxKey, L"TestVal", 99) == 0);
    assert(ak::RegStr(HKEY_CURRENT_USER, kSandboxKey, L"TestStr") == L"Deny");

    const bool offOk = sb.off();
    assert(offOk == true);
    assert(sb.isOn() == false);
    assert(ak::RegDword(HKEY_CURRENT_USER, kSandboxKey, L"TestVal", 99) == 1);
    assert(!ak::RegGet(HKEY_CURRENT_USER, kSandboxKey, L"TestStr").exists);

    CleanupSandbox(testJournal);
    std::printf("PASS test_tweaks (22 tweaks, 5 categories, 10 WT-01 confirm-guarded, sandbox rollback OK)\n");
    return 0;
}
