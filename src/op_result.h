// AnkhCore 2.5 — honest operation results (SEC-3: no false success). Pure logic; unit-tested in
// tests/test_base.cpp.
//
// An operation reports success only when every part it attempted succeeded. One failed part makes
// the operation "Failed", and the log says how many parts worked and how many did not. Attempting
// nothing is not a failure, but it is said in words ("nothing to do"), never dressed up as work done.
#pragma once
#include <string>

namespace ak {

struct OpTally {
    int ok = 0, failed = 0;
    void Add(bool good) { if (good) ++ok; else ++failed; }
    int Attempted() const { return ok + failed; }
    bool Success() const { return failed == 0; }
    // "3 services turned off." / "2 of 3 services turned off; 1 failed." / "Nothing to do: no service needed a change."
    std::wstring Text(const std::wstring& one, const std::wstring& many, const std::wstring& done) const {
        if (Attempted() == 0) return L"Nothing to do: no " + one + L" needed a change.";
        if (failed == 0) return std::to_wstring(ok) + L" " + (ok == 1 ? one : many) + L" " + done + L".";
        return std::to_wstring(ok) + L" of " + std::to_wstring(Attempted()) + L" " + many + L" " + done + L"; " + std::to_wstring(failed) + L" failed.";
    }
};

// Journal undo: how many entries were selected and how many of those were really put back.
struct UndoCount {
    int picked = 0, undone = 0;
    int Failed() const { return picked - undone; }
    bool Success() const { return picked == undone; }
    bool Nothing() const { return picked == 0; }
    std::wstring Text() const {
        if (picked == 0) return L"Nothing to undo: no recorded change matches.";
        if (undone == picked) return std::to_wstring(undone) + (undone == 1 ? L" change undone." : L" changes undone.");
        return std::to_wstring(undone) + L" of " + std::to_wstring(picked) + L" changes undone; " + std::to_wstring(Failed()) + L" could not be undone and stay recorded.";
    }
};

}   // namespace ak
