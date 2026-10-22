#pragma once
// Frame Home's performance and quality settings: ~/frame-home/settings.json on the Steam Frame. Frame Home reads it
// when it starts and again whenever it changes; the Environments panel (Settings) and Frame Studio (Edit > Settings,
// its Steam Frame part) edit it. Frame Home writes how it's doing to stats.json beside it for those two to show.
#include "core/tinyjson.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

struct PerfSettings {
    // Resolution, as a share of SteamVR's recommended size per eye. Auto keeps each frame inside the display's time
    // budget by moving it between 70% and 150%: sharper whenever the home is light enough.
    bool autoResolution = true;
    float resolution = 1.0f;
    int msaa = 4;                       // anti-aliasing samples: 1 (off), 2, 4
    int anisotropy = 8;                 // texture filtering: 4, 8, 16
    // Foveated rendering: full detail where you look, coarser toward the edges, and the time that frees goes to
    // resolution (with Auto). 0 off, 1 fixed (the lens centre), 2 eye tracked (follows your gaze).
    int foveation = 2;
    int foveationLevel = 2;             // 1 gentle, 2 medium, 3 strong
    // Moving around (applies live).
    int walk = 1;                       // left stick: 0 off, 1 smooth walking
    int teleport = 1;                   // right stick forward: 0 off, 1 instant (a quick blink), 2 slide (glide there)
    int turn = 0;                       // right stick left / right: 0 snap, 1 smooth
    int snapAngle = 30;                 // 30, 45, 90 degrees
    float moveSpeed = 2.0f;             // walking speed, m/s: 1.4 slow, 2 normal, 3 fast
    int vignette = 0;                   // comfort: darken the edges of view while walking or turning smoothly: 0 off, 1 gentle, 2 strong
    int seatMarkers = 1;                // the seats near you shown as markers: 0 hidden (pointing still finds them), 1 shown
    // The home itself (applies live).
    float volume = 1.0f;                // all of the home's sound: 0 .. 1
    int animations = 1;                 // 0: the home holds still (animated homes cost less), 1 plays
    int menuRate = 1;                   // behind the Steam menu: 0 half rate (the GPU goes to the menu and its apps), 1 full
    // The environment's own frame rate: 0 the display's, 1 half (drawn every other frame and turned with your head in
    // between by the compositor layer, as Meta's home runs: each frame gets twice the GPU time).
    int frameRate = 0;
    // The headset's temperature to stay at or under, degrees C (0: no limit). Above it the edges of view get coarser,
    // a little at a time, until it's back (where you look stays sharp). 86 is the Steam Frame's own resting level with
    // SteamVR running; past the mid-90s it slows its GPU to a third.
    int tempLimit = 86;

    static constexpr float kAutoMin = 0.7f, kAutoMax = 1.5f;

    std::string toJson() const {
        char b[640];
        std::snprintf(b, sizeof b,
                      "{\"autoResolution\":%s,\"resolution\":%.2f,\"msaa\":%d,\"anisotropy\":%d,\"foveation\":%d,\"foveationLevel\":%d,"
                      "\"walk\":%d,\"teleport\":%d,\"turn\":%d,\"snapAngle\":%d,\"moveSpeed\":%.1f,\"vignette\":%d,\"seatMarkers\":%d,"
                      "\"volume\":%.2f,\"animations\":%d,\"menuRate\":%d,\"frameRate\":%d,\"tempLimit\":%d}\n",
                      autoResolution ? "true" : "false", resolution, msaa, anisotropy, foveation, foveationLevel, walk, teleport, turn, snapAngle, moveSpeed,
                      vignette, seatMarkers, volume, animations, menuRate, frameRate, tempLimit);
        return b;
    }
    bool fromJson(const std::string& j) {
        // The parser takes a file cut short ("{\"msaa\":2,") as a whole object: a settings file must end its object.
        const size_t last = j.find_last_not_of(" \t\r\n");
        if (last == std::string::npos || j[last] != '}') return false;
        try {
            const tinyjson::Value d = tinyjson::parse(j);
            if (!d.isObject()) return false;   // empty, cut short, or not settings
            if (d.has("autoResolution")) autoResolution = d["autoResolution"].boolVal;
            if (d.has("resolution")) resolution = static_cast<float>(d["resolution"].asFloat());
            if (d.has("msaa")) msaa = static_cast<int>(d["msaa"].asFloat());
            if (d.has("anisotropy")) anisotropy = static_cast<int>(d["anisotropy"].asFloat());
            if (d.has("foveation")) foveation = static_cast<int>(d["foveation"].asFloat());
            if (d.has("foveationLevel")) foveationLevel = static_cast<int>(d["foveationLevel"].asFloat());
            if (d.has("walk")) walk = static_cast<int>(d["walk"].asFloat());
            if (d.has("teleport")) teleport = static_cast<int>(d["teleport"].asFloat());
            if (d.has("turn")) turn = static_cast<int>(d["turn"].asFloat());
            if (d.has("snapAngle")) snapAngle = static_cast<int>(d["snapAngle"].asFloat());
            if (d.has("moveSpeed")) moveSpeed = static_cast<float>(d["moveSpeed"].asFloat());
            if (d.has("vignette")) vignette = static_cast<int>(d["vignette"].asFloat());
            if (d.has("seatMarkers")) seatMarkers = static_cast<int>(d["seatMarkers"].asFloat());
            if (d.has("volume")) volume = static_cast<float>(d["volume"].asFloat());
            if (d.has("animations")) animations = static_cast<int>(d["animations"].asFloat());
            if (d.has("menuRate")) menuRate = static_cast<int>(d["menuRate"].asFloat());
            if (d.has("frameRate")) frameRate = static_cast<int>(d["frameRate"].asFloat());
            if (d.has("tempLimit")) tempLimit = static_cast<int>(d["tempLimit"].asFloat());
        } catch (...) {
            return false;
        }
        clamp();
        return true;
    }
    void clamp() {
        resolution = std::clamp(resolution, 0.5f, 2.0f);
        msaa = msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1;
        anisotropy = anisotropy >= 16 ? 16 : anisotropy >= 8 ? 8 : 4;
        foveation = std::clamp(foveation, 0, 2);
        foveationLevel = std::clamp(foveationLevel, 1, 3);
        walk = std::clamp(walk, 0, 1);
        teleport = std::clamp(teleport, 0, 2);
        turn = std::clamp(turn, 0, 1);
        snapAngle = snapAngle >= 90 ? 90 : snapAngle >= 45 ? 45 : 30;
        if (!std::isfinite(moveSpeed)) moveSpeed = 2.0f;
        moveSpeed = std::clamp(moveSpeed, 0.5f, 4.0f);
        vignette = std::clamp(vignette, 0, 2);
        seatMarkers = std::clamp(seatMarkers, 0, 1);
        if (!std::isfinite(volume)) volume = 1.0f;
        volume = std::clamp(volume, 0.0f, 1.0f);
        animations = std::clamp(animations, 0, 1);
        menuRate = std::clamp(menuRate, 0, 1);
        frameRate = std::clamp(frameRate, 0, 1);
        tempLimit = tempLimit <= 0 ? 0 : std::clamp(tempLimit, 70, 95);
    }
    // What changing from `o` to this needs: true when Frame Home must restart (render targets, samplers and the
    // render pass are built for these); resolution and foveation strength apply live.
    bool needsRestart(const PerfSettings& o) const {
        return msaa != o.msaa || anisotropy != o.anisotropy || (foveation > 0) != (o.foveation > 0);
    }
    bool operator==(const PerfSettings& o) const {
        return autoResolution == o.autoResolution && std::abs(resolution - o.resolution) < 1e-3f && msaa == o.msaa &&
               anisotropy == o.anisotropy && foveation == o.foveation && foveationLevel == o.foveationLevel && walk == o.walk &&
               teleport == o.teleport && turn == o.turn && snapAngle == o.snapAngle && std::abs(moveSpeed - o.moveSpeed) < 1e-3f &&
               vignette == o.vignette && seatMarkers == o.seatMarkers && std::abs(volume - o.volume) < 1e-3f && animations == o.animations &&
               menuRate == o.menuRate && frameRate == o.frameRate && tempLimit == o.tempLimit;
    }
};

// Frame Home's debug tools: ~/frame-home/debug.json, beside settings.json. The Environments panel's Debug page and
// Frame Studio (Debug > Headset Debug) edit it and Frame Home applies it within a second. They're for finding out
// why a home looks or runs wrong, not for keeping: Frame Home clears the file each time it starts.
struct DebugSettings {
    int view = 0;                       // HslRenderer::DebugView: 0 normal, 1 wireframe, 2 meshes, 3 GPU cost, 4 overdraw
    int world = 0;                      // the world layer drawn over the home: walkable area, surfaces, spawns, seats
    int hideSeeThrough = 0, hideCutout = 0, hideSky = 0;   // leave those parts of the home out
    int shot = 0;                       // asks for a picture of what's shown (logs/frame-<time>.png): the time it was asked
                                        // (Unix seconds); one from before Frame Home started is old

    bool any() const { return view || world || hideSeeThrough || hideCutout || hideSky; }
    std::string toJson() const {
        char b[256];
        std::snprintf(b, sizeof b, "{\"view\":%d,\"world\":%d,\"hideSeeThrough\":%d,\"hideCutout\":%d,\"hideSky\":%d,\"shot\":%d}\n",
                      view, world, hideSeeThrough, hideCutout, hideSky, shot);
        return b;
    }
    bool fromJson(const std::string& j) {
        const size_t last = j.find_last_not_of(" \t\r\n");
        if (last == std::string::npos || j[last] != '}') return false;
        try {
            const tinyjson::Value d = tinyjson::parse(j);
            if (!d.isObject()) return false;
            auto get = [&](const char* k, int& v) { if (d.has(k)) v = static_cast<int>(d[k].asFloat()); };
            get("view", view); get("world", world); get("hideSeeThrough", hideSeeThrough); get("hideCutout", hideCutout);
            get("hideSky", hideSky); get("shot", shot);
        } catch (...) {
            return false;
        }
        view = std::clamp(view, 0, 4);
        for (int* v : {&world, &hideSeeThrough, &hideCutout, &hideSky}) *v = std::clamp(*v, 0, 1);
        return true;
    }
    unsigned hideBits() const { return (hideSeeThrough ? 1u : 0u) | (hideCutout ? 2u : 0u) | (hideSky ? 4u : 0u); }   // HslRenderer::DebugHide
    bool operator==(const DebugSettings& o) const {
        return view == o.view && world == o.world && hideSeeThrough == o.hideSeeThrough && hideCutout == o.hideCutout && hideSky == o.hideSky &&
               shot == o.shot;
    }
};
