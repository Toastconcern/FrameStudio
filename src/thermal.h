#pragma once
// The headset's temperature, for Frame Home's thermal governor: the hottest of the GPU and CPU sensors the kernel
// lists in /sys/class/thermal (thermal_zone*/type "gpuss-*", "cpu*", "cpuss*"), in degrees C. The Steam Frame throttles
// its GPU clock (903 MHz down to ~310) once these reach the mid-90s, and every frame then costs up to three times as
// long: keeping them at the headset's ordinary level keeps the clock, and the frame rate, where they belong.
// Elsewhere (the PC, a desktop Linux with no such zones) it reads nothing and the governor stays out of the way.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class ThermalSensors {
public:
    ThermalSensors() {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/sys/class/thermal", ec)) {
            const std::string name = e.path().filename().string();
            if (name.rfind("thermal_zone", 0) != 0) continue;
            std::ifstream t(e.path() / "type");
            std::string type;
            std::getline(t, type);
            if (type.rfind("gpuss", 0) == 0 || type.rfind("cpu", 0) == 0) zones_.push_back((e.path() / "temp").string());
        }
    }
    bool available() const { return !zones_.empty(); }
    // The hottest zone now, in degrees C (0 when there are none).
    float hottest() const {
        float best = 0;
        for (const std::string& z : zones_) {
            FILE* f = std::fopen(z.c_str(), "r");
            if (!f) continue;
            long milli = 0;
            if (std::fscanf(f, "%ld", &milli) == 1) best = std::max(best, milli / 1000.0f);
            std::fclose(f);
        }
        return best;
    }

private:
    std::vector<std::string> zones_;
};

// Holds the temperature at a limit by adding foveation strength (coarser toward the edges of view, where you aren't
// looking: the cheapest work to give up) a little at a time while it's above, and handing it back once it's below.
// The headset heats and cools over minutes, so it steps slowly and waits while the temperature is already falling
// (the last step is still taking effect): otherwise it overshoots to the strongest setting and stays there. Smoothed so
// a passing spike doesn't move it; each step small enough that the change isn't seen.
class ThermalGovernor {
public:
    // limit: degrees C to stay at or under (0: off). Called about once a second.
    float update(float tempC, float limit, float dtSec) {
        if (limit <= 0 || tempC <= 0) { extra_ = 0; return 0; }
        smooth_ = smooth_ <= 0 ? tempC : smooth_ + (tempC - smooth_) * std::min(1.0f, dtSec / 8.0f);
        trend_ = trend_ <= 0 ? smooth_ : trend_ + (smooth_ - trend_) * std::min(1.0f, dtSec / 30.0f);   // ~30 s behind
        const bool falling = smooth_ < trend_ - 0.2f;
        since_ += dtSec;
        if (smooth_ > limit + 0.5f && !falling && since_ >= 6.0f) {   // hot and not yet easing: a little more off the edges
            extra_ = std::min(kMax, extra_ + 0.0625f);
            since_ = 0;
        } else if (smooth_ < limit - 1.0f && extra_ > 0 && since_ >= 8.0f) {   // under the limit again: give it back
            extra_ = std::max(0.0f, extra_ - 0.0625f);
            since_ = 0;
        }
        return extra_;
    }
    float extra() const { return extra_; }
    float smoothed() const { return smooth_; }
    static constexpr float kMax = 2.0f;                      // foveation strength it may add (on top of the setting)

private:
    float extra_ = 0, smooth_ = 0, trend_ = 0, since_ = 0;
};
