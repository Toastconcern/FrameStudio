#pragma once
// A world's own render settings: what that one home runs best at on the Steam Frame, over the headset's settings.json.
// A .fenv carries them in its manifest ("settings" in fenv.json); an APK home in envs/ in "<apk>.settings.json"
// beside it. Every field is optional: unset, the headset's own setting applies. Frame Home applies them while the
// world is shown (resolution, foveation strength and frame rate live; MSAA and filtering by restarting itself, as a
// settings change does); `fenv-tool tune` and Frame Studio's Calculate Best Settings work them out on the headset.
#include "perf_settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

struct WorldSettings {
    float resolution = 0;               // a fixed resolution scale (0.5 .. 2); 0: the headset's (fixed or auto)
    int msaa = 0;                       // 1, 2, 4; 0: the headset's
    int anisotropy = 0;                 // 4, 8, 16; 0: the headset's
    float foveation = 0;                // foveation strength, 1 gentle .. 3 strong, in between too; 0: the headset's level
    int frameRate = -1;                 // 0 the display's, 1 half; -1: the headset's
    std::string tuned;                  // how they were found ("" when set by hand)

    bool empty() const { return resolution <= 0 && msaa <= 0 && anisotropy <= 0 && foveation <= 0 && frameRate < 0; }
    bool operator==(const WorldSettings& o) const {
        return std::abs(resolution - o.resolution) < 1e-3f && msaa == o.msaa && anisotropy == o.anisotropy &&
               std::abs(foveation - o.foveation) < 1e-3f && frameRate == o.frameRate && tuned == o.tuned;
    }
    bool operator!=(const WorldSettings& o) const { return !(*this == o); }

    // The headset's settings with this world's on top.
    PerfSettings apply(PerfSettings p) const {
        if (resolution > 0) { p.autoResolution = false; p.resolution = resolution; }
        if (msaa > 0) p.msaa = msaa;
        if (anisotropy > 0) p.anisotropy = anisotropy;
        if (foveation > 0) p.foveationLevel = static_cast<int>(std::lround(foveation));
        if (frameRate >= 0) p.frameRate = frameRate;
        p.clamp();
        return p;
    }
    // The foveation strength to draw with: this world's, else the headset's level.
    float foveationStrength(const PerfSettings& headset) const { return foveation > 0 ? foveation : static_cast<float>(headset.foveationLevel); }

    void clamp() {
        if (!std::isfinite(resolution) || resolution <= 0) resolution = 0;
        else resolution = std::clamp(resolution, 0.5f, 2.0f);
        msaa = msaa <= 0 ? 0 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1;
        anisotropy = anisotropy <= 0 ? 0 : anisotropy >= 16 ? 16 : anisotropy >= 8 ? 8 : 4;
        if (!std::isfinite(foveation) || foveation <= 0) foveation = 0;
        else foveation = std::clamp(foveation, 1.0f, 3.0f);
        frameRate = frameRate < 0 ? -1 : std::min(frameRate, 1);
    }

    // Only the fields that are set ("{}" when none).
    std::string toJson() const {
        std::string j;
        char b[96];
        auto add = [&](const char* k, const char* v) { j += std::string(j.empty() ? "" : ", ") + "\"" + k + "\": " + v; };
        if (resolution > 0) { std::snprintf(b, sizeof b, "%.2f", resolution); add("resolution", b); }
        if (msaa > 0) { std::snprintf(b, sizeof b, "%d", msaa); add("msaa", b); }
        if (anisotropy > 0) { std::snprintf(b, sizeof b, "%d", anisotropy); add("anisotropy", b); }
        if (foveation > 0) { std::snprintf(b, sizeof b, "%.3f", foveation); add("foveation", b); }
        if (frameRate >= 0) { std::snprintf(b, sizeof b, "%d", frameRate); add("frameRate", b); }
        if (!tuned.empty()) {
            std::string q = "\"";
            for (char c : tuned) {
                if (c == '"' || c == '\\') { q += '\\'; q += c; }
                else if (static_cast<unsigned char>(c) >= 0x20) q += c;
            }
            add("tuned", (q + "\"").c_str());
        }
        return "{" + j + "}";
    }
    bool fromValue(const tinyjson::Value& d) {
        if (!d.isObject()) return false;
        *this = WorldSettings();
        // (a file's numbers: inf / NaN / 1e300 as an int is undefined, and NaN would be written back as invalid JSON)
        const auto num = [&](const char* k, double unset) {
            const double v = d[k].asFloat();
            return std::isfinite(v) ? std::min(1e6, std::max(-1e6, v)) : unset;
        };
        if (d.has("resolution")) resolution = static_cast<float>(num("resolution", 0));
        if (d.has("msaa")) msaa = static_cast<int>(num("msaa", 0));
        if (d.has("anisotropy")) anisotropy = static_cast<int>(num("anisotropy", 0));
        if (d.has("foveation")) foveation = static_cast<float>(num("foveation", 0));
        if (d.has("frameRate")) frameRate = static_cast<int>(num("frameRate", -1));
        if (d.has("tuned")) tuned = d["tuned"].asString();
        clamp();
        return true;
    }
    bool fromJson(const std::string& j) {
        const size_t last = j.find_last_not_of(" \t\r\n");
        if (last == std::string::npos || j[last] != '}') return false;
        try { return fromValue(tinyjson::parse(j)); } catch (...) { return false; }
    }

    // "1.40x, 2x MSAA, foveation 1.5" (what is set), or "the headset's settings".
    std::string describe() const {
        if (empty()) return "the headset's settings";
        std::string s;
        char b[64];
        auto add = [&](const char* t) { s += std::string(s.empty() ? "" : ", ") + t; };
        if (resolution > 0) { std::snprintf(b, sizeof b, "resolution %.0f%%", resolution * 100); add(b); }
        if (msaa > 0) { std::snprintf(b, sizeof b, "%dx MSAA", msaa); add(b); }
        if (anisotropy > 0) { std::snprintf(b, sizeof b, "%dx filtering", anisotropy); add(b); }
        if (foveation > 0) { std::snprintf(b, sizeof b, "foveation %.3g", foveation); add(b); }
        if (frameRate >= 0) add(frameRate ? "half frame rate" : "full frame rate");
        return s;
    }
};

// An APK home's settings file, beside it in envs/ (a .fenv keeps them in its manifest instead).
inline std::string worldSettingsPathFor(const std::string& homePath) { return homePath + ".settings.json"; }
