#pragma once
// Frame pacing for Frame Studio's window: one frame per display refresh while the window is in use, a few per
// second when it's in the background, and none at all once nothing on screen can change (no input for a moment,
// nothing animating, loading or saving) until the next input arrives.
//
// Without it the UI loop drew as fast as the GPU allowed (MAILBOX presents never block): ~75% of an RTX 3060 for a
// home sitting still on a 164 Hz monitor.
//
// The wait happens before the frame reads input, so pacing adds no input latency: the frame starts as late as it can.
// The window code (Win32 or GLFW) supplies what the pacer can't know: the display's rate, whether the window has
// focus, and how to sleep until input arrives.
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
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

    // isFocused: the window has the keyboard. waitForInput(seconds): sleep until input arrives or the time is up.
    FramePacer(std::function<bool()> isFocused, std::function<void(double)> waitForInput)
        : isFocused_(std::move(isFocused)), waitForInput_(std::move(waitForInput)) {
#ifdef _WIN32
        // High-resolution waitable timer (Windows 10 1803+); a plain one (~1-15 ms granularity) otherwise.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
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

    // Blocks until the next frame is due. `sceneChanging`: something on screen moves by itself (animation playing,
    // a home loading, a save's progress, a fly-through). An idle wait ends early when input arrives.
    void waitForNextFrame(bool sceneChanging) {
        const bool focused = isFocused_();
        const bool settled = std::chrono::duration<double>(Clock::now() - lastInput_).count() > kSettleSeconds;
        if (settled && !sceneChanging) {
            // Nothing can change until the user does something: sleep until input.
            waitForInput_(kIdleWakeSeconds);
            nextFrame_ = Clock::now() - period(displayFps_);   // the frame after waking starts at once
            return;
        }
        const double fps = fpsOverride_ >= 0 ? fpsOverride_ : focused ? displayFps_ : kBackgroundFps;
        if (fps <= 0) return;
        // A frame that ran late starts the next period from now instead of rushing to catch up.
        nextFrame_ = std::max(nextFrame_ + period(fps), Clock::now());
        // Input arriving meanwhile waits for the frame (at most one refresh): waking for each event would let a
        // 1000 Hz mouse drive the frame rate.
        sleepUntil(nextFrame_);
    }

private:
    static Clock::duration period(double fps) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    }

    void sleepUntil(Clock::time_point when) {
        const auto wait = when - Clock::now();
        if (wait <= Clock::duration::zero()) return;
#ifdef _WIN32
        // Windows' Sleep has ~1-15 ms granularity: a high-resolution timer hits the refresh.
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);   // relative, 100 ns units
        if (timer_ && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer_, INFINITE); return; }
#endif
        std::this_thread::sleep_until(when);
    }

    std::function<bool()> isFocused_;
    std::function<void(double)> waitForInput_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
    double displayFps_ = 60;
    double fpsOverride_ = -1;
    Clock::time_point lastInput_, nextFrame_;
};
