#pragma once
// Wall spots at work: a SteamVR panel (a desktop or game window, any free-floating overlay) let go of near one of
// the home's wall spots docks onto it — flush with the wall, facing the room, no wider than the spot — and stays
// there as you walk and teleport (panels live in tracking space, so a docked one is re-placed from the home's frame
// whenever you move). Grab it away and it's free again, back at its own size. Docked panels are re-placed only when
// the rig moves: setting one every frame would overwrite a grab before it could be seen.
// Meta's homes mark these spots with WallPlacementComponent; Frame Studio edits them (World > Wall Spot).
#include "core/world.h"
#include "overlays.h"

#include <openvr.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class PanelDock {
public:
    ~PanelDock() { stop(); }
    // Before VR_Shutdown: the scanner calls into SteamVR's client library, which shutting down unloads.
    void stop() { stop_ = true; if (scanner_.joinable()) scanner_.join(); }

    void setWorld(const HomeWorld& w) {
        undockAll();
        spots_ = w.wallSpots;
        if (!spots_.empty() && !scanner_.joinable()) scanner_ = std::thread([this] { scanLoop(); });
    }

    // Once per frame with worldFromTracking.
    void update(const float rig[16]) {
        if (spots_.empty()) return;
        std::vector<OverlayInfo> panels;
        { std::lock_guard<std::mutex> l(mutex_); panels = panels_; }
        float inv[16];
        invertRigid(rig, inv);
        const auto now = std::chrono::steady_clock::now();
        vr::IVROverlay* ov = vr::VROverlay();
        // Panels whose app went away: their state (and any spot they held) goes too.
        for (auto it = state_.begin(); it != state_.end();) {
            bool present = false;
            for (const OverlayInfo& p : panels) present |= p.handle == it->first;
            it = present ? std::next(it) : state_.erase(it);
        }
        for (const OverlayInfo& p : panels) {
            if (!ov->IsOverlayVisible(p.handle)) continue;
            vr::VROverlayTransformType tt;
            if (ov->GetOverlayTransformType(p.handle, &tt) != vr::VROverlayError_None || tt != vr::VROverlayTransform_Absolute) continue;
            vr::ETrackingUniverseOrigin origin;
            vr::HmdMatrix34_t cur;
            if (ov->GetOverlayTransformAbsolute(p.handle, &origin, &cur) != vr::VROverlayError_None) continue;
            if (origin != vr::TrackingUniverseStanding) continue;   // the spots are placed in standing space
            State& s = state_[p.handle];
            if (s.spot >= 0) {
                // Docked: still where we put it? Then keep it on the wall as the rig moves; else it was grabbed away.
                if (differs(cur, s.placed, 0.03f)) { undock(p.handle, s, "moved away"); s.still = now; s.last = cur; continue; }
                // Re-placed only once the rig has moved a few millimetres since it was placed (per panel, so one
                // hidden while you teleported is put right when it shows again). Tiny continuous rig changes —
                // seated eye-height easing, step easing — would otherwise overwrite a grab every frame.
                float moved = 0;
                for (int k = 0; k < 16; ++k) moved = std::max(moved, std::fabs(rig[k] - s.rig[k]));
                if (moved < 0.005f) continue;
                vr::HmdMatrix34_t m = spotInTracking(s.spot, inv, s.flip);
                ov->SetOverlayTransformAbsolute(p.handle, origin, &m);
                s.placed = m;
                std::memcpy(s.rig, rig, sizeof s.rig);
                continue;
            }
            if (differs(cur, s.last, 0.002f)) { s.last = cur; s.still = now; continue; }   // still moving
            if (now - s.still < std::chrono::milliseconds(500) || s.cooldown > now) continue;
            // At rest: dock onto the nearest free spot within 0.6 m.
            float pt[3];
            for (int k = 0; k < 3; ++k) pt[k] = rig[k] * cur.m[0][3] + rig[4 + k] * cur.m[1][3] + rig[8 + k] * cur.m[2][3] + rig[12 + k];
            int best = -1;
            float bestD = 0.6f;
            for (size_t i = 0; i < spots_.size(); ++i) {
                if (taken(static_cast<int>(i))) continue;
                const float* c = &spots_[i].world[12];
                const float d = std::sqrt((pt[0] - c[0]) * (pt[0] - c[0]) + (pt[1] - c[1]) * (pt[1] - c[1]) + (pt[2] - c[2]) * (pt[2] - c[2]));
                if (d < bestD) { bestD = d; best = static_cast<int>(i); }
            }
            if (best < 0) { s.cooldown = now + std::chrono::milliseconds(500); continue; }
            // Face the panel the way it was facing (towards the user), whichever way the spot's frame points.
            const float* z = &spots_[best].world[8];
            float pz[3];
            for (int k = 0; k < 3; ++k) pz[k] = rig[k] * cur.m[0][2] + rig[4 + k] * cur.m[1][2] + rig[8 + k] * cur.m[2][2];
            s.flip = z[0] * pz[0] + z[1] * pz[1] + z[2] * pz[2] < 0;
            ov->GetOverlayWidthInMeters(p.handle, &s.width);
            const float fit = std::min(s.width, spots_[best].width);
            ov->SetOverlayWidthInMeters(p.handle, fit);
            vr::HmdMatrix34_t m = spotInTracking(best, inv, s.flip);
            ov->SetOverlayTransformAbsolute(p.handle, origin, &m);
            s.placed = m;
            std::memcpy(s.rig, rig, sizeof s.rig);
            s.spot = best;
            std::fprintf(stderr, "frame-home: docked panel %s on wall spot %s\n", p.key.c_str(), spots_[best].name.c_str());
        }
    }

private:
    struct State {
        int spot = -1;
        bool flip = false;
        float width = 1;                                    // its own width, given back when it leaves the wall
        vr::HmdMatrix34_t placed = {}, last = {};
        float rig[16] = {};                                 // the rig it was last placed for
        std::chrono::steady_clock::time_point still{}, cooldown{};
    };
    std::vector<WorldWallSpot> spots_;
    std::map<vr::VROverlayHandle_t, State> state_;
    std::vector<OverlayInfo> panels_;
    std::mutex mutex_;
    std::thread scanner_;
    std::atomic<bool> stop_{false};

    // The panel list, refreshed every 2 s off the frame loop (a sweep takes ~0.1 s).
    void scanLoop() {
        while (!stop_) {
            std::vector<OverlayInfo> found;
            for (const OverlayInfo& o : listOverlays())
                if (isUserPanel(o) && o.key.rfind("system.", 0) != 0) found.push_back(o);
            { std::lock_guard<std::mutex> l(mutex_); panels_ = std::move(found); }
            for (int i = 0; i < 20 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool taken(int spot) const {
        for (const auto& kv : state_) if (kv.second.spot == spot) return true;
        return false;
    }

    void undock(vr::VROverlayHandle_t h, State& s, const char* why) {
        vr::VROverlay()->SetOverlayWidthInMeters(h, s.width);
        std::fprintf(stderr, "frame-home: panel left wall spot %s (%s)\n", spots_[s.spot].name.c_str(), why);
        s.spot = -1;
        s.cooldown = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }

    void undockAll() {
        for (auto& kv : state_) if (kv.second.spot >= 0) undock(kv.first, kv.second, "new home");
        state_.clear();
    }

    // The spot's frame in tracking space, 1 cm off the wall, optionally turned to face the other way.
    vr::HmdMatrix34_t spotInTracking(int i, const float inv[16], bool flip) const {
        float w[16];
        std::memcpy(w, spots_[i].world, 64);
        for (int c = 0; c < 3; ++c) {   // unit axes (a scaled spot frame would scale the panel)
            const float L = std::sqrt(w[c * 4] * w[c * 4] + w[c * 4 + 1] * w[c * 4 + 1] + w[c * 4 + 2] * w[c * 4 + 2]);
            if (L > 1e-6f) for (int r = 0; r < 3; ++r) w[c * 4 + r] /= L;
        }
        if (flip) for (int r = 0; r < 3; ++r) { w[r] = -w[r]; w[8 + r] = -w[8 + r]; }   // 180 degrees about Y
        for (int r = 0; r < 3; ++r) w[12 + r] += w[8 + r] * 0.01f;
        float t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) t[c * 4 + r] = inv[r] * w[c * 4] + inv[4 + r] * w[c * 4 + 1] + inv[8 + r] * w[c * 4 + 2] + inv[12 + r] * w[c * 4 + 3];
        vr::HmdMatrix34_t m;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m.m[r][c] = t[c * 4 + r];
        return m;
    }

    static void invertRigid(const float m[16], float o[16]) {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o[c * 4 + r] = m[r * 4 + c];
        for (int r = 0; r < 3; ++r) o[12 + r] = -(o[r] * m[12] + o[4 + r] * m[13] + o[8 + r] * m[14]);
        o[3] = o[7] = o[11] = 0; o[15] = 1;
    }

    static bool differs(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b, float eps) {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) if (std::fabs(a.m[r][c] - b.m[r][c]) > eps) return true;
        return false;
    }
};
