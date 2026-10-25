#pragma once
// Frame pacing for Frame Studio's window: one frame per display refresh while the window is in use, a few per
// second when it's in the background, and none at all once nothing on screen can change (no input for a moment,
// nothing animating, loading or saving) until the next input arrives.
//
// Without it the UI loop drew as fast as the GPU allowed (MAILBOX presents never block): ~75% of an RTX 3060 for a
// home sitting still on a 164 Hz monitor.
//
// The wait happens before the frame reads input, so pacing adds no input latency: the frame starts as late as it can.
// On Windows the frames are locked to the display's refreshes (DWM's vblank clock): each starts its own measured
// cost (plus a millisecond to spare) before a refresh, so it's on screen at that refresh. A plain timer at the
// refresh rate drifts against the display: some refreshes got two frames and the next none (a hitch every few
// seconds), and input waited up to a whole refresh for no reason. Only while DWM's clock keeps to a steady grid
// (a virtual or idle display's doesn't): the plain timer otherwise, or always with FS_PACE=timer.
// The window code (Win32 or GLFW) supplies what the pacer can't know: the display's rate, whether the window has
// focus, and how to sleep until input arrives.
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    // Frames per second while in the background (another window has focus).
    static constexpr double kBackgroundFps = 30.0;
    // After the last input, keep drawing at full rate this long (ImGui hover effects, tooltips and drags settle).
    static constexpr double kSettleSeconds = 1.0;
    // Idle waits wake at least this often, so background work's progress (saving, uploading) still shows.
    static constexpr double kIdleWakeSeconds = 0.25;
    // Locked to the refresh: a frame starts this much before the refresh it's for, beyond what frames have cost lately.
    static constexpr double kSpareMs = 1.0;

    // isFocused: the window has the keyboard. waitForInput(seconds): sleep until input arrives or the time is up.
    FramePacer(std::function<bool()> isFocused, std::function<void(double)> waitForInput)
        : isFocused_(std::move(isFocused)), waitForInput_(std::move(waitForInput)) {
#ifdef _WIN32
        // High-resolution waitable timer (Windows 10 1803+); a plain one (~1-15 ms granularity) otherwise.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcHz_ = static_cast<double>(f.QuadPart);
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        clockBase_ = Clock::now();
        qpcBase_ = q.QuadPart;
        const char* pace = std::getenv("FS_PACE");
        lockToVblank_ = !(pace && std::strcmp(pace, "timer") == 0);
#endif
        lastInput_ = nextFrame_ = Clock::now();
    }
    ~FramePacer() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // FS_FPS=n overrides the display's rate (0: unlimited, the old behaviour, for comparisons).
    void setFpsOverride(double fps) { fpsOverride_ = fps; }

    // The refresh rate of the display the window is on (the window code tells it again when the window moves).
    void setDisplayFps(double fps) { displayFps_ = fps > 1 ? fps : 60.0; }
    double displayFps() const { return displayFps_; }

    // Call for every input from the user (keyboard, mouse, resize, focus, file drop).
    void noteInput() { lastInput_ = Clock::now(); }

    // What the frame just drawn cost, from its start to its picture being done (CPU, then the GPU's part): how early
    // the next one starts. The second-worst of the last 64 frames counts (one stall misses its refresh whatever the
    // lead; a lead set by it would add latency to every frame after), leaving out frames longer than a refresh.
    void noteFrameCost(double ms) {
        if (!(ms > 0)) return;
        costs_[costAt_++ % kCosts] = ms;
        double top = 0, second = 0;
        const double cap = 1000.0 / displayFps_;
        for (double c : costs_) {
            if (c >= cap) continue;
            if (c > top) { second = top; top = c; } else if (c > second) second = c;
        }
        costMs_ = second > 0 ? second : top > 0 ? top : costMs_;
    }
    double frameCostMs() const { return costMs_; }

    // The display's last refresh and its period, from DWM's clock (Windows): false when there's none to lock to.
    bool vblank(Clock::time_point& last, double& periodMs) const {
#ifdef _WIN32
        DWM_TIMING_INFO ti;
        std::memset(&ti, 0, sizeof ti);
        ti.cbSize = sizeof ti;
        if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0) return false;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        const double agoMs = (static_cast<double>(q.QuadPart) - static_cast<double>(ti.qpcVBlank)) * 1000.0 / qpcHz_;
        // (from one fixed pairing of the two clocks: the same refresh is the same time_point every call)
        const long long hz = static_cast<long long>(qpcHz_), c = static_cast<long long>(ti.qpcVBlank) - qpcBase_;
        last = clockBase_ + std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(c / hz * 1000000000LL + c % hz * 1000000000LL / hz));
        periodMs = static_cast<double>(ti.qpcRefreshPeriod) * 1000.0 / qpcHz_;
        return periodMs > 2.0 && periodMs < 100.0 && agoMs > -periodMs && agoMs < 1000.0;
#else
        (void)last; (void)periodMs;
        return false;
#endif
    }

    // Blocks until the next frame is due. `sceneChanging`: something on screen moves by itself (animation playing,
    // a home loading, a save's progress, a fly-through). An idle wait ends early when input arrives.
    void waitForNextFrame(bool sceneChanging) {
        const bool focused = isFocused_();
        const bool settled = std::chrono::duration<double>(Clock::now() - lastInput_).count() > kSettleSeconds;
        if (settled && !sceneChanging) {
            // Nothing can change until the user does something: sleep until input.
            waitForInput_(kIdleWakeSeconds);
            nextFrame_ = Clock::now() - period(displayFps_);   // the frame after waking starts at once
            targetVblank_ = Clock::time_point();
            return;
        }
        const double fps = fpsOverride_ >= 0 ? fpsOverride_ : focused ? displayFps_ : kBackgroundFps;
        if (fps <= 0) return;
        Clock::time_point last;
        double periodMs = 0;
        if (lockToVblank_ && fpsOverride_ < 0 && focused && vblank(last, periodMs) && std::fabs(1000.0 / periodMs - displayFps_) < displayFps_ * 0.25 &&
            onGrid(last, periodMs)) {
            // The first refresh this frame can make (its cost from now), and never the one the last frame was for.
            const auto per = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(periodMs));
            const auto lead = std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double, std::milli>(std::clamp(costMs_ + kSpareMs, 1.5, periodMs - 0.5)));
            const Clock::time_point now = Clock::now();
            Clock::time_point v = last + per;
            while (v < now + lead) v += per;
            if (targetVblank_ != Clock::time_point() && v < targetVblank_ + per / 2) v += per;
            targetVblank_ = v;
            nextFrame_ = v - lead;
            sleepUntil(nextFrame_);
            return;
        }
        targetVblank_ = Clock::time_point();
        // A frame that ran late starts the next period from now instead of rushing to catch up.
        nextFrame_ = std::max(nextFrame_ + period(fps), Clock::now());
        // Input arriving meanwhile waits for the frame (at most one refresh): waking for each event would let a
        // 1000 Hz mouse drive the frame rate.
        sleepUntil(nextFrame_);
    }

    // The refresh the frame now starting is for (none when not locked to them).
    Clock::time_point targetVblank() const { return targetVblank_; }

private:
    static constexpr int kCosts = 64;

    // Whether DWM's refreshes keep to a grid: each new one a whole number of periods (to 0.3 ms) after the one
    // before, four in a row.
    bool onGrid(Clock::time_point v, double periodMs) {
        if (v == lastSeenVblank_) return gridRun_ >= 4;
        if (lastSeenVblank_ != Clock::time_point()) {
            const double d = std::chrono::duration<double, std::milli>(v - lastSeenVblank_).count(), k = std::round(d / periodMs);
            gridRun_ = k >= 1 && std::fabs(d - k * periodMs) < 0.3 ? std::min(gridRun_ + 1, 8) : 0;
        }
        lastSeenVblank_ = v;
        return gridRun_ >= 4;
    }

    static Clock::duration period(double fps) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    }

    void sleepUntil(Clock::time_point when) {
        auto wait = when - Clock::now();
        if (wait <= Clock::duration::zero()) return;
#ifdef _WIN32
        // Windows' Sleep has ~1-15 ms granularity: a high-resolution timer to within a few tenths of a millisecond
        // (it wakes up to ~0.5 ms late), then a short spin to the moment itself.
        const auto early = std::chrono::microseconds(600);
        if (timer_ && wait > early) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(wait - early).count() / 100);   // relative, 100 ns units
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, INFINITE);
        }
        while (Clock::now() < when) YieldProcessor();
        return;
#endif
        std::this_thread::sleep_until(when);
    }

    std::function<bool()> isFocused_;
    std::function<void(double)> waitForInput_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    double qpcHz_ = 1;
    long long qpcBase_ = 0;
    Clock::time_point clockBase_;
#endif
    bool lockToVblank_ = false;
    double displayFps_ = 60;
    double fpsOverride_ = -1;
    double costMs_ = 4.0;   // (until frames say otherwise)
    double costs_[kCosts] = {};
    unsigned costAt_ = 0;
    int gridRun_ = 0;
    Clock::time_point lastInput_, nextFrame_, targetVblank_, lastSeenVblank_;
};
