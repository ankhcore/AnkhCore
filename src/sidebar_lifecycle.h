// Sidebar lifecycle (FT-D01 follow-up, "Sidebar Lifecycle & Stability"). Pure decision logic: no Windows calls,
// so every case can be tested without a desktop. The app (src/app.cpp) collects the facts and carries out the answer.
//
// Architecture, from the source: the Full Sidebar is NOT a separate process. It is one window (class AnkhCoreSidebar,
// SidebarProc) owned by the AnkhCore UI thread. In Desktop mode it is a child of Progman (Windows 11 24H2+) or of the
// wallpaper WorkerW, so it is destroyed together with that host when Explorer exits. The only separate process is the
// background engine (--engine), which delivers the once-a-second WM_ENGINE_FRAME; before this change the Sidebar's
// recovery ran only from that frame.
#pragma once
#include <cstdint>

namespace ak {

// Why a Sidebar window is gone. Only Unexpected may start a recovery.
enum class SbEnd { UserClose, Shutdown, Unexpected };
inline SbEnd SbClassifyEnd(bool intendedOn, bool appQuitting, bool closingByUser, bool destroyedByUs) {
    if (appQuitting) return SbEnd::Shutdown;
    if (destroyedByUs || closingByUser || !intendedOn) return SbEnd::UserClose;   // our own SbDestroy, the slide-out, or "off" in the settings
    return SbEnd::Unexpected;                                                      // Explorer restart, a stray WM_CLOSE, a destroyed host, ...
}

// Bounded retries with growing waits: no fixed-rate restart, no endless loop.
constexpr int      kSbRetryMax      = 5;        // consecutive attempts before waiting for a real event
constexpr uint64_t kSbRetryStableMs = 30000;    // a window that lived this long was a success: the count starts over
constexpr uint64_t kSbKickGapMs     = 10000;    // events may re-arm a give-up at most this often

struct SbRetry {
    int      tries = 0;          // consecutive attempts
    uint64_t lastAt = 0;         // time of the last attempt (ms)
    uint64_t nextAt = 0;         // earliest time of the next attempt
    uint64_t gaveUpAt = 0;
    bool     gaveUp = false;
};
inline uint64_t SbBackoffMs(int tries) {   // 1 s, 2 s, 4 s, 8 s, 16 s (capped at 30 s)
    if (tries <= 0) return 0;
    const uint64_t v = 1000ull << (tries > 5 ? 5 : tries - 1);
    return v > 30000 ? 30000 : v;
}
inline bool SbRetryMay(const SbRetry& r, uint64_t now) { return !r.gaveUp && now >= r.nextAt; }
// Call right before each attempt.
inline void SbRetryAttempt(SbRetry& r, uint64_t now) {
    if (r.tries > 0 && now - r.lastAt > kSbRetryStableMs) r.tries = 0;   // the last one held: this is a new incident
    ++r.tries; r.lastAt = now; r.nextAt = now + SbBackoffMs(r.tries);
    if (r.tries >= kSbRetryMax) { r.gaveUp = true; r.gaveUpAt = now; }     // this attempt is the last until a real event
}
inline void SbRetryReset(SbRetry& r) { r = SbRetry(); }                    // the user acted: start clean
// A loss is noticed. One that comes long after the last attempt is a new incident, even after an earlier give-up.
inline void SbRetryLoss(SbRetry& r, uint64_t now) { if (r.tries > 0 && now - r.lastAt > kSbRetryStableMs) SbRetryReset(r); }
// A real event (Explorer restarted, display change, resume): another try is justified, but not more often than the gap.
inline bool SbRetryKick(SbRetry& r, uint64_t now) {
    if (r.gaveUp && now - r.gaveUpAt >= kSbKickGapMs) { r.gaveUp = false; r.tries = 0; r.nextAt = now; return true; }
    if (!r.gaveUp && r.nextAt > now + 1000) { r.nextAt = now + 1000; return true; }   // waiting out a long back-off: the shell just changed, look sooner
    return false;
}
// Healthy for long enough: whatever was counted against it is forgiven, a give-up included.
inline void SbRetryHeal(SbRetry& r, uint64_t now) { if (r.tries > 0 && now - r.lastAt > kSbRetryStableMs) SbRetryReset(r); }
inline bool SbRetryExhausted(const SbRetry& r) { return r.gaveUp; }
// For an already-living Sidebar whose Z-order or visibility was disrupted, never abandon it permanently:
// after a cooldown period, allow one fresh bounded recovery cycle.
inline bool SbRetryAllowCooldown(SbRetry& r, uint64_t now, uint64_t cooldownMs = 10000) {
    if (!r.gaveUp || now < r.gaveUpAt || (now - r.gaveUpAt) < cooldownMs) return false;
    r.gaveUp = false; r.tries = 0; r.nextAt = now;
    return true;
}

enum class SbState { Stopped, Starting, Running, TemporarilyDetached, Recovering, Failed };
inline const wchar_t* SbStateName(SbState s) {
    switch (s) {
    case SbState::Stopped:             return L"Stopped";
    case SbState::Starting:            return L"Starting";
    case SbState::Running:             return L"Running";
    case SbState::TemporarilyDetached: return L"TemporarilyDetached";
    case SbState::Recovering:          return L"Recovering";
    case SbState::Failed:              return L"Failed";
    default:                           return L"Unknown";
    }
}

enum class SbDisappearReason {
    None,
    Destroyed,
    HiddenBySystem,
    OffScreen,
    ClippedRegion,
    DetachedFromHost,
    CoveredByWorkerW,
    InvalidHandle,
    ExplorerRestart
};
inline const wchar_t* SbDisappearReasonName(SbDisappearReason r) {
    switch (r) {
    case SbDisappearReason::None:             return L"None";
    case SbDisappearReason::Destroyed:        return L"Destroyed";
    case SbDisappearReason::HiddenBySystem:   return L"HiddenBySystem";
    case SbDisappearReason::OffScreen:        return L"OffScreen";
    case SbDisappearReason::ClippedRegion:    return L"ClippedRegion";
    case SbDisappearReason::DetachedFromHost: return L"DetachedFromHost";
    case SbDisappearReason::CoveredByWorkerW: return L"CoveredByWorkerW";
    case SbDisappearReason::InvalidHandle:    return L"InvalidHandle";
    case SbDisappearReason::ExplorerRestart:  return L"ExplorerRestart";
    default:                                  return L"Unknown";
    }
}

inline SbState SbComputeState(bool wantOn, bool haveValidWindow, bool visible, bool desktopMode, bool hosted, bool hostAvailable, bool gaveUp) {
    if (!wantOn) return SbState::Stopped;
    if (gaveUp && !haveValidWindow) return SbState::Failed;
    if (!haveValidWindow) return SbState::Recovering;
    if (desktopMode && !hosted) return hostAvailable ? SbState::Recovering : SbState::TemporarilyDetached;
    if (!visible) return SbState::Recovering;
    return SbState::Running;
}

// Unified Sidebar synchronization state for diagnostics and lifecycle verification.
struct SbSyncState {
    SbState lifecycleState = SbState::Stopped;
    SbDisappearReason lastDisappearReason = SbDisappearReason::None;
    bool userRequestedVisible = false;
    bool actualWindowVisible = false;
    bool desktopIntegrationActive = false;
    bool autoHideActive = false;
    bool positionLocked = false;
    std::wstring currentMonitor;
    RECT currentPosition{};
    UINT lastMessage = 0;
};

// What to do when the Sidebar is meant to be on and the window is absent.
struct SbLifeFacts {
    bool wantOn = false;        // the user's choice (settings / toggle)
    bool quitting = false;      // AnkhCore is exiting
    bool closing = false;       // the user's slide-out is running
    bool haveWindow = false;
};
enum class SbLifeAct { Nothing, Wait, Recreate, GiveUp };
struct SbLifeVerdict { SbLifeAct act = SbLifeAct::Nothing; uint64_t waitMs = 0; };
inline SbLifeVerdict SbLifeDecide(const SbLifeFacts& f, const SbRetry& r, uint64_t now) {
    if (f.quitting || !f.wantOn || f.closing || f.haveWindow) return {};
    if (r.gaveUp) return {SbLifeAct::GiveUp, 0};
    if (!SbRetryMay(r, now)) return {SbLifeAct::Wait, r.nextAt - now};
    return {SbLifeAct::Recreate, 0};
}

// The shell events that make a re-check worthwhile (collected by the app; the Sidebar is a child window and
// never receives the display broadcasts itself, so the main window forwards them).
enum class SbShellEvent { ExplorerRestarted, DisplayChanged, WorkAreaChanged, Resumed, SessionUnlocked, ThemeChanged };
inline bool SbEventNeedsReplace(SbShellEvent) { return true; }   // every one of them can move or detach a hosted window
// How long to let the shell settle before looking (Explorer needs a moment to rebuild Progman and the wallpaper layer).
inline uint64_t SbEventSettleMs(SbShellEvent e) {
    switch (e) { case SbShellEvent::ExplorerRestarted: return 1500; case SbShellEvent::Resumed: return 3000; case SbShellEvent::SessionUnlocked: return 1500; case SbShellEvent::ThemeChanged: return 900; default: return 700; }
}

// C5: Tracks Windows session notification registration and deduplicates rapid/duplicate WTS_SESSION_UNLOCK events.
struct SbSessionState {
    bool registered = false;
    bool locked = false;
    uint64_t lastUnlockTick = 0;
    uint64_t debounceMs = 1000;
    uint64_t unlockEvents = 0;
    uint64_t duplicatesIgnored = 0;

    void OnRegistered(bool ok) { registered = ok; }
    void OnUnregistered() { registered = false; locked = false; }

    // Returns true if this unlock event should trigger Sidebar placement + SensorHub re-sync.
    bool OnSessionEvent(bool isLock, bool isUnlock, uint64_t nowMs) {
        if (isLock) { locked = true; return false; }
        if (!isUnlock) return false;
        if (lastUnlockTick != 0 && nowMs >= lastUnlockTick && (nowMs - lastUnlockTick) < debounceMs && !locked) {
            ++duplicatesIgnored;
            return false;
        }
        locked = false;
        lastUnlockTick = nowMs;
        ++unlockEvents;
        return true;
    }
};

}  // namespace ak
