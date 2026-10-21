#pragma once
// Moving around a home in Frame Home, the way Meta's homes do it:
//   you start at the home's spawn point, its floor on your real floor, facing where the home faces
//   left stick walks (head-relative, moveSpeed m/s at full push, default 2; a light push strolls), easing up to speed
//     and to a stop in a fraction of a second; walls stop you and slide you along them, the floor follows ramps
//     exactly and eases you up and down steps (roomscale walking too). Walking off (setOptions walk 0): the stick
//     does nothing.
//   right stick left / right turns about your head: snap turns of snapAngle degrees (30 / 45 / 90) on a flick, or
//     smooth turning (up to 120 degrees/s, by how far the stick is pushed)
//   right stick forward aims a teleport arc from the right hand, drawn by Frame Home itself (guide(), guide_geom.h)
//     with a marker where you'll land. The arc stops at the first thing it meets: a floor you can stand on (or a
//     seat) is the landing; a wall, a ceiling or the side of furniture means nowhere — unless it's low, then you land
//     at its foot. Let go to go there: instantly behind a quick fade to black (a blink), or as a 0.25 s
//     eased dash along the ground (slide). Landing near a teleport hotspot snaps onto it. Teleport off: the stick
//     forward does nothing. In gravity homes a slide is an instant blink (the walker re-orients you on landing).
//   seats show a faint marker near you; pointing a hand at one lights it up (with a short pointer line), and pulling
//     that hand's trigger sits you in it (a slide or a blink, as teleports go).
//   landing on a seat sits you in it: eyes at a seated height over the seat, facing its way. Seated you can turn
//     (snap turn, or turn your head) but stay put; walking, teleporting or stepping more than half a metre away
//     stands you up — beside the seat (its exit point) when you walk off.
//   with the Steam menu open the controllers belong to the menu: nothing moves you until it's closed.
//   falling (off an edge, roomscale) is real; falling further than the world's fallLimit puts you back at the spawn.
//   homes with gravity zones (walls you walk up, planets) are walked by GravityWalker (gravity.h) instead.
// The rig is worldFromTracking: SteamVR's standing space (y = 0 on your floor) placed into the home.
#include "gravity.h"
#include "guide_geom.h"
#include "world_runtime.h"

#include <openvr.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class Locomotion {
public:
    // Input actions (written next to Frame Home on first run) and the teleport ring overlay.
    bool init(const std::filesystem::path& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(dir, ec);
        const char* binding = R"({ "bindings": { "/actions/main": {
  "sources": [
    { "path": "/user/hand/left/input/thumbstick", "mode": "joystick", "inputs": { "position": { "output": "/actions/main/in/left_stick" } } },
    { "path": "/user/hand/right/input/thumbstick", "mode": "joystick", "inputs": { "position": { "output": "/actions/main/in/right_stick" } } },
    { "path": "/user/hand/left/input/trigger", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/left_trigger" } } },
    { "path": "/user/hand/right/input/trigger", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/right_trigger" } } } ],
  "poses": [
    { "output": "/actions/main/in/left_aim", "path": "/user/hand/left/pose/tip" },
    { "output": "/actions/main/in/right_aim", "path": "/user/hand/right/pose/tip" } ] } },
  "controller_type": "%TYPE%", "description": "Frame Home: walk, turn, teleport", "name": "Frame Home" })";
        const char* types[] = {"frame_controller", "knuckles", "oculus_touch", "vive_cosmos_controller"};
        std::string defaults;
        for (const char* t : types) {
            std::string b = binding;
            b.replace(b.find("%TYPE%"), 6, t);
            std::ofstream(dir / (std::string("bindings_") + t + ".json")) << b;
            defaults += std::string(defaults.empty() ? "" : ",\n    ") + R"({ "controller_type": ")" + t + R"(", "binding_url": "bindings_)" + t + R"(.json" })";
        }
        // The headset's eye tracker (foveated rendering follows your gaze).
        std::ofstream(dir / "bindings_frame_hmd.json") << R"({ "bindings": { "/actions/main": {
  "eyetracking": [ { "path": "/user/head/eyetracking", "output": "/actions/main/in/gaze" } ] } },
  "controller_type": "frame_hmd", "description": "Frame Home: eye tracking", "name": "Frame Home" })";
        defaults += R"(,
    { "controller_type": "frame_hmd", "binding_url": "bindings_frame_hmd.json" })";
        std::ofstream(dir / "actions.json") << R"({
  "default_bindings": [
    )" << defaults << R"( ],
  "actions": [
    { "name": "/actions/main/in/left_stick", "type": "vector2" },
    { "name": "/actions/main/in/right_stick", "type": "vector2" },
    { "name": "/actions/main/in/left_aim", "type": "pose" },
    { "name": "/actions/main/in/right_aim", "type": "pose" },
    { "name": "/actions/main/in/left_trigger", "type": "boolean" },
    { "name": "/actions/main/in/right_trigger", "type": "boolean" },
    { "name": "/actions/main/in/gaze", "type": "eyetracking" } ],
  "action_sets": [ { "name": "/actions/main", "usage": "leftright" } ],
  "localization": [ { "language_tag": "en_US", "/actions/main": "Frame Home",
    "/actions/main/in/left_stick": "Walk", "/actions/main/in/right_stick": "Turn and teleport",
    "/actions/main/in/left_aim": "Left hand", "/actions/main/in/right_aim": "Teleport aim",
    "/actions/main/in/left_trigger": "Sit: left hand", "/actions/main/in/right_trigger": "Sit: right hand", "/actions/main/in/gaze": "Where you look" } ]
})";
        vr::IVRInput* in = vr::VRInput();
        if (!in || in->SetActionManifestPath(fs::absolute(dir / "actions.json").c_str()) != vr::VRInputError_None) return false;
        in->GetActionSetHandle("/actions/main", &set_);
        in->GetActionHandle("/actions/main/in/left_stick", &leftStick_);
        in->GetActionHandle("/actions/main/in/right_stick", &rightStick_);
        in->GetActionHandle("/actions/main/in/right_aim", &rightAim_);
        in->GetActionHandle("/actions/main/in/left_aim", &leftAim_);
        in->GetActionHandle("/actions/main/in/right_trigger", &trigger_[0]);
        in->GetActionHandle("/actions/main/in/left_trigger", &trigger_[1]);
        in->GetActionHandle("/actions/main/in/gaze", &gaze_);
        // The landing ring: a flat overlay placed in the room each frame while aiming — only when Frame Home can't draw
        // the arc itself (setRingFallback).
        if (vr::VROverlay() && vr::VROverlay()->CreateOverlay("framehome.teleport", "Teleport", &ring_) == vr::VROverlayError_None) {
            const int n = 128;
            std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1, r = std::sqrt(dx * dx + dy * dy);
                    const float ring = std::clamp(1 - std::fabs(r - 0.82f) / 0.12f, 0.0f, 1.0f), dot = std::clamp((0.18f - r) / 0.05f, 0.0f, 1.0f);
                    const float a = std::max(ring, dot);
                    uint8_t* p = &px[(static_cast<size_t>(y) * n + x) * 4];
                    p[0] = 255; p[1] = 168; p[2] = 70; p[3] = static_cast<uint8_t>(a * 230);
                }
            vr::VROverlay()->SetOverlayRaw(ring_, px.data(), n, n, 4);
            vr::VROverlay()->SetOverlayWidthInMeters(ring_, 0.55f);
        }
        return ok_ = true;
    }

    // A new home: its world, and the player at its spawn.
    void setWorld(HomeWorld world) {
        world_ = std::move(world);
        runtime_.build(world_);
        fallLimit_ = world_.fallLimit > 1 ? world_.fallLimit : 30.0f;
        gravityMode_ = !world_.gravity.empty();
        toSpawn();
        if (gravityMode_) walker_.build(world_);
    }
    const HomeWorld& world() const { return world_; }            // as walked (the debug overlay draws it)
    const WalkGrid* walkGrid() const { return runtime_.grid(); }

    // Back to the first spawn, standing, nothing under way (a new home, or a fall too far).
    void toSpawn() {
        float x = 0, z = 0, y = 0, yaw = 0;
        if (!world_.spawns.empty()) { const WorldPoint& s = world_.spawns[0]; x = s.pos[0]; y = s.pos[1]; z = s.pos[2]; yaw = s.yaw; }
        float f;
        if (runtime_.floorAt(x, z, y + 0.5f, 0.6f, 3.0f, f)) y = f;
        rigYaw_ = yaw; rigX_ = x; rigZ_ = z; floorY_ = targetFloor_ = y;
        seated_ = -1;
        aiming_ = valid_ = false;                            // a teleport aimed in the last home doesn't land here
        seat_ = -1;
        vel_[0] = vel_[1] = stepV_ = 0;
        pointed_[0] = pointed_[1] = -1;
        guide_ = LocoGuide();
        if (ring_ && vr::VROverlay()) vr::VROverlay()->HideOverlay(ring_);
        if (blinkT_ >= 0) blinkIn();                         // a jump pending in the last home is dropped, eyes open
        blinkT_ = slideT_ = -1;
        fallV_ = fallen_ = 0;
    }

    // Movement options (Settings), applied live. walk: 0 off, 1 smooth. teleport: 0 off, 1 instant (blink), 2 slide.
    // turn: 0 snap (by snapAngleDeg), 1 smooth. moveSpeed: m/s at full stick.
    void setOptions(int walk, int teleport, int turn, int snapAngleDeg, float moveSpeed) {
        walk_ = walk; teleport_ = teleport; turn_ = turn;
        snap_ = static_cast<float>(std::clamp(snapAngleDeg, 5, 180)) * 0.01745329f;
        speed_ = std::clamp(moveSpeed, 0.1f, 10.0f);
        walker_.setSpeed(speed_);
        if (teleport_ == 0 && aiming_) dropAim();               // an arc being aimed when teleport is switched off
    }

    bool gravityMode() const { return gravityMode_; }
    int respawns() const { return respawns_ + (gravityMode_ ? walker_.respawns() : 0); }

    int seated() const { return seated_; }
    // Where you look, in standing space, for the frame being rendered (false: no eye tracking right now — none on
    // this headset, eyes closed, or the Steam menu has the input).
    bool gaze(float origin[3], float target[3]) const {
        if (!ok_ || !gaze_) return false;
        vr::VREyeTrackingData_t e = {};
        if (vr::VRInput()->GetEyeTrackingDataForNextFrame(gaze_, vr::TrackingUniverseStanding, &e, sizeof e) != vr::VRInputError_None ||
            !e.bActive || !e.bValid)
            return false;
        for (int k = 0; k < 3; ++k) { origin[k] = e.vGazeOrigin.v[k]; target[k] = e.vGazeTarget.v[k]; }
        return true;
    }
    void setPoseValid(bool v) { poseValid_ = v; }
    // Test hooks (tools/loco_test): what landing on a seat, and walking off it, do.
    void testSit(int seat, const float headT[16]) { float R[16], h[3]; rig(R); apply(R, &headT[12], h); sit(seat, R, headT, h); }
    void testWalkOff(const float headT[16]) { float R[16], h[3]; rig(R); apply(R, &headT[12], h); if (seated_ >= 0) standUp(h, true); }
    // Sticks (and the right hand's aim pose, 3x4 row-major as OpenVR gives it) without SteamVR; from now on update()
    // reads these instead of the controllers.
    void testInput(float lx, float ly, float rx, float ry, const float* aim34 = nullptr) {
        test_ = true;
        testIn_[0] = lx; testIn_[1] = ly; testIn_[2] = rx; testIn_[3] = ry;
        testAimOk_ = aim34 != nullptr;
        if (aim34) std::copy(aim34, aim34 + 12, testAim_);
    }
    // The triggers (0 right, 1 left) and the left hand's aim pose, as testInput's.
    void testButtons(bool rightTrigger, bool leftTrigger, const float* leftAim34 = nullptr) {
        test_ = true;
        testTrig_[0] = rightTrigger; testTrig_[1] = leftTrigger;
        testLeftOk_ = leftAim34 != nullptr;
        if (leftAim34) std::copy(leftAim34, leftAim34 + 12, testLeft_);
    }
    bool testTarget(float t[3]) const { std::copy(target_, target_ + 3, t); return aiming_ && valid_; }
    int testTargetSeat() const { return aiming_ && valid_ ? seat_ : -1; }
    int pointedSeat(int hand) const { return pointed_[hand & 1]; }   // 0 right, 1 left; -1: none
    // A straight ray (world) against the seats' pointable volumes, stopped by walls and floors in front: the seat
    // hit, or -1, and how far along (metres). Hitting the chair itself (its back, from behind) counts as the seat.
    int seatOnRay(const float o[3], const float dir[3], float maxLen, float& t) const {
        const float d[3] = {dir[0] * maxLen, dir[1] * maxLen, dir[2] * maxLen};
        float ts, tb = 2, n[3];
        const WorldCollider* shape = nullptr;
        int i = seatOnSegment(o, d, ts);
        if (!runtime_.hasFloor() || runtime_.trace(o, d, tb, n, &shape) == WorldRuntime::None) tb = 2;
        if (i >= 0 && ts <= tb) { t = ts * maxLen; return i; }
        if (tb > 1) return -1;
        const float q[3] = {o[0] + d[0] * tb, o[1] + d[1] * tb, o[2] + d[2] * tb};
        if ((i = seatOf(q, shape)) >= 0) t = tb * maxLen;
        return i;
    }
    // What to draw this frame (after update()): the arc and its landing, seat markers, the pointer.
    const LocoGuide& guide() const { return guide_; }
    // Frame Home draws the guide itself: the SteamVR ring overlay only stands in when it can't (fallback = true).
    void setRingFallback(bool on) { ringOn_ = on; if (!on && ring_ && vr::VROverlay()) vr::VROverlay()->HideOverlay(ring_); }
    bool sliding() const { return slideT_ >= 0; }

    // worldFromTracking (column-major).
    void rig(float m[16]) const {
        if (gravityMode_) {   // the walker's frame, with the tracking floor lifted by lift_ (an unset floor)
            walker_.rig(m);
            for (int k = 0; k < 3; ++k) m[12 + k] += m[4 + k] * lift_;
            return;
        }
        const float c = std::cos(rigYaw_), s = std::sin(rigYaw_);
        const float r[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, rigX_, seated_ >= 0 ? seatRigY_ : floorY_ + lift_, rigZ_, 1};
        std::copy(r, r + 16, m);
    }

    // Once per frame, with the head's tracking-space pose (column-major, as from OpenVR).
    void update(float dt, const float headT[16]) {
        dt = std::min(dt, 0.05f);
        checkEyeHeight(dt, headT[13]);
        guide_.seats.clear();
        guide_.pointer = false;
        if (gravityMode_ && !ok_ && !test_) {   // no controllers: the walker still holds you to the ground (and lets you fall)
            float h[16];
            std::copy(headT, headT + 16, h);
            h[13] += lift_;
            walker_.update(dt, h, 0, 0, 0);
            return;
        }
        float R[16];
        rig(R);
        float head[3];
        apply(R, &headT[12], head);
        if (ok_ || test_) {
            vr::InputAnalogActionData_t l = {}, r = {};
            bool trig[2] = {false, false};
            if (ok_) {
                vr::VRActiveActionSet_t active = {};
                active.ulActionSet = set_;
                vr::VRInput()->UpdateActionState(&active, sizeof active, 1);
                vr::VRInput()->GetAnalogActionData(leftStick_, &l, sizeof l, vr::k_ulInvalidInputValueHandle);
                vr::VRInput()->GetAnalogActionData(rightStick_, &r, sizeof r, vr::k_ulInvalidInputValueHandle);
                for (int h = 0; h < 2; ++h) {
                    vr::InputDigitalActionData_t b = {};
                    if (trigger_[h] && vr::VRInput()->GetDigitalActionData(trigger_[h], &b, sizeof b, vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None)
                        trig[h] = b.bActive && b.bState;
                }
                if (l.bActive != ownInput_) { ownInput_ = l.bActive; std::fprintf(stderr, "frame-home: controller input %s\n", ownInput_ ? "ours" : "taken (Steam menu open?)"); }
            } else {
                l.bActive = r.bActive = true;
                l.x = testIn_[0]; l.y = testIn_[1]; r.x = testIn_[2]; r.y = testIn_[3];
                trig[0] = testTrig_[0]; trig[1] = testTrig_[1];
            }
            // The Steam menu open: the controllers are the menu's (pointing at and clicking the dock), not the home's.
            // An aim in progress is dropped, not jumped to (the stick reads as let go).
            menuAim_ = false;
            bool menu = false;
            if (ok_ && vr::VROverlay() && vr::VROverlay()->IsDashboardVisible()) {
                l.bActive = r.bActive = false;
                menu = true;
                if (aiming_) dropAim();
            }
            // Walk: the stick in the direction the head faces (flattened). Seated, it stands you up first. Walking
            // off, the stick is dead (a teleport-only user with a worn stick doesn't drift); mid-dash or blink too.
            const bool walk = l.bActive && walk_ != 0 && slideT_ < 0 && blinkT_ < 0;
            float lx = 0, ly = 0;
            if (walk) grav::shapeStick(l.x, l.y, lx, ly);
            if (gravityMode_) { updateGravity(dt, headT, lx, ly, r); return; }
            // Walking and teleport both off: the stick still stands you up from a seat (else only stepping away would)
            if (seated_ >= 0 && walk_ == 0 && teleport_ == 0 && l.bActive) {
                float sx = 0, sy = 0;
                grav::shapeStick(l.x, l.y, sx, sy);
                if ((sx != 0 || sy != 0) && slideT_ < 0 && blinkT_ < 0) { leaveSeat(headT, head, R); rig(R); apply(R, &headT[12], head); }
            }
            if ((lx != 0 || ly != 0) && seated_ >= 0) { leaveSeat(headT, head, R); rig(R); apply(R, &headT[12], head); }
            // The velocity eases toward the stick's (grav::approach); a jump or a seat stops it dead.
            grav::V want;
            if (lx != 0 || ly != 0) {
                float fwd[3];
                const float hz[3] = {-headT[8], -headT[9], -headT[10]};   // the head's -Z in tracking space
                dir(R, hz, fwd);
                const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
                if (fl > 1e-4f) {
                    const float fx = fwd[0] / fl, fz = fwd[2] / fl;
                    want = grav::V((fx * ly - fz * lx) * speed_, 0, (fz * ly + fx * lx) * speed_);
                }
            }
            grav::V v(vel_[0], 0, vel_[1]);
            if (slideT_ >= 0 || blinkT_ >= 0 || seated_ >= 0 || walk_ == 0) v = grav::V();
            else grav::approach(v, want, speed_, dt);
            vel_[0] = v.x; vel_[1] = v.z;
            if (v.x != 0 || v.z != 0) {
                float x = head[0], z = head[2];
                const float dx = v.x * dt, dz = v.z * dt;
                if (runtime_.hasFloor()) runtime_.move(x, z, floorY_, dx, dz); else { x += dx; z += dz; }
                float f;
                if (!runtime_.hasFloor() || runtime_.floorAt(x, z, floorY_, 0.45f, 1.2f, f)) {   // never walk off an edge
                    rigX_ += x - head[0]; rigZ_ += z - head[2];
                    vel_[0] = (x - head[0]) / dt; vel_[1] = (z - head[2]) / dt;   // what a wall left of it: no pent-up push
                    head[0] = x; head[2] = z;
                } else {
                    vel_[0] = vel_[1] = 0;
                }
            }
            // Turn, about the head (so you spin in place, not round the room's origin); momentum turns with you.
            const float rx = r.bActive ? r.x : 0, ry = r.bActive ? r.y : 0;
            if (const float a = turnBy(dt, rx, ry)) {
                const float c = std::cos(a), s = std::sin(a);
                const float ox = rigX_ - head[0], oz = rigZ_ - head[2];
                rigX_ = head[0] + c * ox + s * oz;
                rigZ_ = head[2] - s * ox + c * oz;
                rigYaw_ += a;
                const float vx = vel_[0], vz = vel_[1];
                vel_[0] = c * vx + s * vz; vel_[1] = -s * vx + c * vz;
                rig(R);
            }
            // Teleport: aim while the stick is forward, go on release (one jump at a time).
            if (teleport_ == 0) { if (aiming_) dropAim(); }
            else if (ry > 0.7f && std::fabs(rx) < 0.6f) aim(R, head, dt);
            else if (aiming_ && ry < 0.4f) {
                aiming_ = false;
                guide_.aiming = false;
                if (ring_) vr::VROverlay()->HideOverlay(ring_);
                if (valid_ && slideT_ < 0 && blinkT_ < 0) {
                    std::copy(target_, target_ + 3, jump_);          // aiming again mid-blink mustn't move it
                    jumpSeat_ = seat_;
                    startJump(headT, head, R, teleport_ == 2);
                }
            }
            // Seats: pointing a hand at one lights it; that hand's trigger (pressed now) sits you in it.
            for (int h = 0; h < 2; ++h) {
                const bool press = trig[h] && !trigHeld_[h] && !menu;
                trigHeld_[h] = trig[h];
                pointed_[h] = -1;
                if (menu || (h == 0 && aiming_) || world_.seats.empty()) continue;   // the right hand is aiming the arc
                float m[12];
                if (!(h == 0 ? handPose(m) : leftPose(m))) continue;
                float o[3], d[3];
                const float po[3] = {m[3], m[7], m[11]}, pd[3] = {-m[2], -m[6], -m[10]};
                apply(R, po, o);
                dir(R, pd, d);
                float t;
                const int i = seatOnRay(o, d, kPointReach, t);
                if (i < 0 || i == seated_) continue;
                pointed_[h] = i;
                if (!guide_.pointer) {
                    guide_.pointer = true;
                    // to the seat itself (the ray may have stopped on the chair's collider, short of it)
                    const float* sp = world_.seats[i].pos;
                    for (int k = 0; k < 3; ++k) { guide_.pointerFrom[k] = o[k]; guide_.pointerTo[k] = sp[k] + (k == 1 ? 0.08f : 0.0f); }
                }
                if (press && slideT_ < 0 && blinkT_ < 0) {
                    seatTarget(i, jump_);
                    jumpSeat_ = i;
                    if (aiming_) dropAim();
                    std::fprintf(stderr, "frame-home: trigger on %s\n", world_.seats[i].name.c_str());
                    startJump(headT, head, R, teleport_ == 2);
                }
            }
        }
        // Seat markers: the seats right by you (fading in from 1.2 m to 2 m away), and one you point or aim at from
        // anywhere; never the one you're in.
        for (size_t i = 0; i < world_.seats.size(); ++i) {
            const WorldPoint& st = world_.seats[i];
            if (static_cast<int>(i) == seated_) continue;
            const float dx = st.pos[0] - head[0], dz = st.pos[2] - head[2], dist = std::hypot(dx, dz);
            const bool hot = pointed_[0] == static_cast<int>(i) || pointed_[1] == static_cast<int>(i) || (aiming_ && valid_ && seat_ == static_cast<int>(i));
            const float near = std::clamp((kMarkerReach - dist) / (kMarkerReach - 1.2f), 0.0f, 1.0f);
            if (!hot && near <= 0) continue;
            guide_.seats.push_back({{st.pos[0], st.pos[1], st.pos[2]}, st.yaw, hot, near});
        }
        // A blink: the jump happens once the view is black, then it fades back in.
        if (blinkT_ >= 0 && (blinkT_ -= dt) <= 0) { blinkT_ = -1; land(headT, head); blinkIn(); }
        // A dash in progress: eased (slow, fast, slow) so it starts and stops without a jolt; the floor under the
        // way isn't followed (the dash may cross a gap or a step the arc flew over).
        if (slideT_ >= 0) {
            const float t0 = slideT_, t1 = std::min(1.0f, slideT_ + dt / kSlide), de = ease(t1) - ease(t0);
            rigX_ += slideDX_ * de; rigZ_ += slideDZ_ * de;
            head[0] += slideDX_ * de; head[2] += slideDZ_ * de;
            floorY_ = targetFloor_ = slideY0_ + (jump_[1] - slideY0_) * ease(t1);
            fallV_ = fallen_ = stepV_ = 0;
            slideT_ = t1;
            if (t1 >= 1) {
                slideT_ = -1;
                floorY_ = targetFloor_ = jump_[1];
                if (jumpSeat_ >= 0) { rig(R); sit(jumpSeat_, R, headT, head); }
            }
            lastHead_[0] = head[0]; lastHead_[1] = head[2];
            return;
        }
        // Never inside anything: the eyes stay kHeadClear from whatever is solid at their height, and the body doesn't
        // stand in a coffee table, however you got there (the stick slides along edges already; this is moving for
        // real, leaning, bending over a table). The world moves back instead.
        if (const WalkGrid* g = runtime_.grid(); g && seated_ < 0) {
            float push[2];
            if (g->headPush(head[0], head[1], head[2], kHeadClear, push) ||
                g->bodyPush(head[0], floorY_, head[2], push)) {
                rigX_ += push[0]; rigZ_ += push[1];
                head[0] += push[0]; head[2] += push[1];
            }
        }
        const float walked = std::hypot(head[0] - lastHead_[0], head[2] - lastHead_[1]);   // stick and roomscale
        lastHead_[0] = head[0]; lastHead_[1] = head[2];
        if (seated_ >= 0) {
            // Seated: turning is free, moving isn't. Leaning is fine; stepping away (roomscale) stands you up there.
            const WorldPoint& st = world_.seats[seated_];
            if (std::hypot(head[0] - st.pos[0], head[2] - st.pos[2]) > 0.55f) standUp(head, false);
            else {
                // Sitting down (or up) for real slowly brings the eyes back to the seated height; quick head
                // movements still show.
                const float want = st.pos[1] + kSeatedEye - headT[13];
                seatRigY_ += (want - seatRigY_) * std::min(1.0f, dt * 0.7f);
                return;
            }
        }
        // The floor under the head (stick or roomscale walking). A ramp is followed exactly; a step is eased
        // (critically damped: no jolt as it starts or ends). Nothing within a step below: you fall, and land on
        // whatever is under you; too far, and you're back at the spawn.
        // The floor is looked for within a step of where you are (down stairs: the next tread, never the floor under the
        // staircase), then a hand's width round (a stair's edge, a seam): only with none there do you fall.
        float f;
        bool on = runtime_.hasFloor() && runtime_.floorAt(head[0], head[2], floorY_, 0.45f, 0.45f, f);
        if (!on && runtime_.grid()) {
            float nx = head[0], nz = head[2];
            on = runtime_.grid()->roomAt(nx, floorY_, nz, 0.2f) && runtime_.floorAt(nx, nz, floorY_, 0.45f, 0.45f, f);
        }
        if (on) {
            const float d = f - targetFloor_;
            if (std::fabs(d) <= walked * grav::kRampSlope + 0.004f) floorY_ += d;
            targetFloor_ = f;
            fallV_ = fallen_ = 0;
        } else if (runtime_.hasFloor()) {
            stepV_ = 0;
            fallV_ = std::min(fallV_ + 9.8f * dt, 55.0f);
            const float stepD = fallV_ * dt;
            if (runtime_.floorAt(head[0], head[2], floorY_, 0.0f, stepD + 0.05f, f)) { floorY_ = targetFloor_ = f; fallV_ = fallen_ = 0; return; }
            floorY_ -= stepD;
            targetFloor_ = floorY_;
            fallen_ += stepD;
            if (fallen_ > fallLimit_) {
                ++respawns_;
                std::fprintf(stderr, "frame-home: fell %.0f m: back to the spawn\n", fallen_);
                toSpawn();
            }
            return;
        }
        const float left = targetFloor_ - floorY_;
        floorY_ = targetFloor_ - grav::smoothDamp(left, 0, stepV_, left > 0 ? grav::kStepUp : grav::kStepDown, dt);
    }

private:
    // Scale is true metres end to end (Meta's homes are authored in metres, floor at y = 0), so the home looks its
    // real size only when the tracking floor is the real floor. A headset whose standing space has no floor (its
    // origin at the head) puts the eyes at floor level and every room looks giant: after 2 s of tracking with the
    // eyes under 0.6 m, lift them to a standing 1.65 m. FH_EYE_HEIGHT=<metres> forces an eye height; 0 = never lift.
    void checkEyeHeight(float dt, float headY) {
        if (liftDecided_ || headY == 0 || !poseValid_) return;   // only real head poses count (not asleep / lost)
        static const char* forced = std::getenv("FH_EYE_HEIGHT");
        if (forced) {
            const float want = static_cast<float>(std::atof(forced));
            lift_ = want > 0 ? want - headY : 0;
            liftDecided_ = true;
            std::fprintf(stderr, "frame-home: eye height forced to %.2f m (tracking %.2f m)\n", want, headY);
            return;
        }
        lowTime_ = headY < 0.6f ? lowTime_ + dt : 0;
        seenTime_ += dt;
        if (lowTime_ > 2.0f) {
            lift_ = 1.65f - headY;
            liftDecided_ = true;
            std::fprintf(stderr, "frame-home: tracking floor looks unset (eyes %.2f m over it): lifting to 1.65 m\n", headY);
        } else if (seenTime_ > 4.0f) {
            liftDecided_ = true;                             // a real floor: true scale, no lift
        }
    }

    // Sits in seat i: the head over the seat, still facing the way you were, eyes kSeatedEye over its surface (whatever
    // your real height: you may be standing in the room).
    static constexpr float kSeatedEye = 0.78f;
    static constexpr float kArcSpeed = 10.0f;    // the teleport arc's launch speed (m/s)
    static constexpr float kHeadClear = 0.15f;   // the eyes never come closer than this to anything solid (m)
    void sit(int i, const float R[16], const float headT[16], float head[3]) {
        const WorldPoint& st = world_.seats[i];
        rigX_ += st.pos[0] - head[0];
        rigZ_ += st.pos[2] - head[2];
        head[0] = st.pos[0]; head[2] = st.pos[2];
        (void)R;
        seatRigY_ = st.pos[1] + kSeatedEye - headT[13];
        seated_ = i;
        std::fprintf(stderr, "frame-home: sitting in %s\n", st.name.c_str());
    }

    // Moving the stick in a seat: out to its stand-up spot (Frame Studio's "stand up here"; half a metre in front when
    // it names none), the way a teleport goes there — a dash with slide teleports, else a blink or a jump.
    void leaveSeat(const float headT[16], float head[3], const float R[16]) {
        if (seated_ < 0) return;
        const WorldPoint& st = world_.seats[seated_];
        const float c = std::cos(st.yaw), s = std::sin(st.yaw);
        float ex = st.exit[0], ez = st.exit[2];
        if (std::hypot(ex, ez) < 0.05f) { ex = 0; ez = -0.5f; }
        jump_[0] = st.pos[0] + c * ex + s * ez;
        jump_[2] = st.pos[2] - s * ex + c * ez;
        float f;
        if (!runtime_.hasFloor() || !runtime_.floorAt(jump_[0], jump_[2], st.pos[1], 0.3f, 2.0f, f)) { standUp(head, true); return; }
        jump_[1] = f;
        jumpSeat_ = -1;
        std::fprintf(stderr, "frame-home: getting up from %s\n", st.name.c_str());
        startJump(headT, head, R, teleport_ == 2);
        if (slideT_ < 0 && blinkT_ < 0) seated_ = -1;                 // (a jump: done; a dash or a blink: under way)
    }

    // Leaves the seat. byStick: you walked off — you step out at the seat's exit point (beside it, or half a metre
    // in front when the seat has none). Otherwise you stood up where your head is.
    void standUp(float head[3], bool byStick) {
        const WorldPoint& st = world_.seats[seated_];
        seated_ = -1;
        float to[3] = {head[0], 0, head[2]};
        if (byStick) {
            const float c = std::cos(st.yaw), s = std::sin(st.yaw);
            float ex = st.exit[0], ez = st.exit[2];
            if (std::hypot(ex, ez) < 0.05f) { ex = 0; ez = -0.5f; }       // forward (-Z) when the seat names no exit
            to[0] = st.pos[0] + c * ex + s * ez;
            to[2] = st.pos[2] - s * ex + c * ez;
        }
        float f = st.pos[1] - 0.45f;
        if (runtime_.hasFloor() && runtime_.floorAt(to[0], to[2], st.pos[1], 0.3f, 2.0f, f)) {}
        else if (byStick) { to[0] = head[0]; to[2] = head[2]; runtime_.floorAt(to[0], to[2], st.pos[1], 0.3f, 2.0f, f); }
        rigX_ += to[0] - head[0];
        rigZ_ += to[2] - head[2];
        head[0] = to[0]; head[2] = to[2];
        floorY_ = targetFloor_ = f;
        std::fprintf(stderr, "frame-home: stood up from %s\n", st.name.c_str());
    }

    // Gravity zones: the same sticks and teleport, but with the walker's up.
    void updateGravity(float dt, const float headT[16], float lx, float ly, const vr::InputAnalogActionData_t& r) {
        float h[16];
        std::copy(headT, headT + 16, h);
        h[13] += lift_;
        const float rx = r.bActive ? r.x : 0, ry = r.bActive ? r.y : 0;
        walker_.update(dt, h, lx, ly, turnBy(dt, rx, ry));
        // Teleport: aim along the walker's gravity, go on release. A slide is a blink here: the landing may turn
        // your up (onto a wall), which a straight glide can't follow.
        if (teleport_ == 0) { if (aiming_) dropAim(); }
        else if (ry > 0.7f && std::fabs(rx) < 0.6f) {
            if (!aiming_) guide_.aimTime = 0;
            aiming_ = true;
            valid_ = false;
            guide_.aimTime += dt;
            float m[12];
            guide_.arc.clear();
            if (handPose(m)) {
                const float po[3] = {m[3], m[7] + lift_, m[11]}, pd[3] = {-m[2], -m[6], -m[10]};
                valid_ = walker_.aim(po, pd, target_, targetN_, &guide_.arc, &h[12]);
            }
            guide_.aiming = !guide_.arc.empty();
            guide_.valid = valid_;
            guide_.seat = false;
            std::copy(target_, target_ + 3, guide_.target);
            std::copy(targetN_, targetN_ + 3, guide_.normal);
            if (ring_ && ringOn_) {
                if (!valid_) vr::VROverlay()->HideOverlay(ring_);
                else {   // tracking up is the walker's up, so the ring lies flat in tracking space
                    const grav::V t = walker_.toTracking(grav::V(target_));
                    vr::HmdMatrix34_t tm = {{{1, 0, 0, t.x}, {0, 0, 1, t.y + 0.02f - lift_}, {0, -1, 0, t.z}}};
                    vr::VROverlay()->SetOverlayTransformAbsolute(ring_, vr::TrackingUniverseStanding, &tm);
                    vr::VROverlay()->ShowOverlay(ring_);
                }
            }
        } else if (aiming_ && ry < 0.4f) {
            aiming_ = false;
            guide_.aiming = false;
            if (ring_) vr::VROverlay()->HideOverlay(ring_);
            if (valid_ && blinkT_ < 0) {
                std::copy(target_, target_ + 3, jump_);
                std::copy(targetN_, targetN_ + 3, jumpN_);
                if (blinkOut()) blinkT_ = kBlinkOut;
                else walker_.teleport(jump_, h, jumpN_);
            }
        }
        if (blinkT_ >= 0 && (blinkT_ -= dt) <= 0) { blinkT_ = -1; walker_.teleport(jump_, h, jumpN_); blinkIn(); }
    }

    // This frame's turn (radians, + = left) from the right stick's x. Snap: one snap_ per flick (the stick must
    // come back near centre to arm the next). Smooth: up to kTurnRate past the deadzone, not while aiming (an arc
    // held slightly off-centre shouldn't spin you).
    static constexpr float kTurnRate = 2.0944f;              // 120 degrees/s
    float turnBy(float dt, float rx, float ry) {
        if (turn_ == 1) return aiming_ || std::fabs(ry) > 0.7f ? 0.0f : -dead(rx) * kTurnRate * dt;
        if (!turnArmed_ && std::fabs(rx) < 0.3f) turnArmed_ = true;
        if (turnArmed_ && std::fabs(rx) > 0.75f && std::fabs(ry) < 0.6f) { turnArmed_ = false; return rx > 0 ? -snap_ : snap_; }
        return 0;
    }

    // Instant teleports blink: fade to black, jump, fade back. In system mode Frame Home isn't the scene app and
    // the compositor may ignore the fade; then the blink is just a 80 ms wait.
    static constexpr float kBlinkOut = 0.08f, kSlide = 0.25f;
    bool blinkOut() const { if (!ok_ || !vr::VRCompositor()) return false; vr::VRCompositor()->FadeToColor(kBlinkOut, 0, 0, 0, 1); return true; }
    void blinkIn() const { if (ok_ && vr::VRCompositor()) vr::VRCompositor()->FadeToColor(0.12f, 0, 0, 0, 0); }
    static float ease(float t) { return t * t * (3 - 2 * t); }

    // Off to jump_ (and into seat jumpSeat_ when >= 0): a 0.25 s dash (slide), or a blink — instant behind a fade.
    void startJump(const float headT[16], float head[3], const float R[16], bool slide) {
        vel_[0] = vel_[1] = 0;
        if (slide) {
            // The dash: the rig glides by the head's offset to the target, its height from where you are
            // (standing, or a seat's eye height) to the target's floor. The arc found that floor.
            slideT_ = 0;
            slideDX_ = jump_[0] - head[0]; slideDZ_ = jump_[2] - head[2];
            slideY0_ = R[13] - lift_;
            seated_ = -1;
        } else if (blinkOut()) blinkT_ = kBlinkOut;
        else land(headT, head);                                  // no compositor of ours: just jump
    }

    // Puts the head over jump_ on its floor (a jumped-to seat sits you in it).
    void land(const float headT[16], float head[3]) {
        seated_ = -1;                                            // teleporting off a seat stands you up
        rigX_ += jump_[0] - head[0];
        rigZ_ += jump_[2] - head[2];
        floorY_ = targetFloor_ = jump_[1];
        fallV_ = fallen_ = stepV_ = 0;
        vel_[0] = vel_[1] = 0;
        head[0] = jump_[0]; head[2] = jump_[2];
        if (jumpSeat_ >= 0) { float R[16]; rig(R); sit(jumpSeat_, R, headT, head); }
    }

    void dropAim() { aiming_ = valid_ = guide_.aiming = false; if (ring_ && vr::VROverlay()) vr::VROverlay()->HideOverlay(ring_); }

    // The right hand's aim (3x4 row-major, tracking space): the controller, the test's, or the menu's.
    bool handPose(float m[12]) const {
        if (test_) { if (testAimOk_) std::copy(testAim_, testAim_ + 12, m); return testAimOk_; }
        vr::InputPoseActionData_t pose = {};
        if (vr::VRInput()->GetPoseActionDataForNextFrame(rightAim_, vr::TrackingUniverseStanding, &pose, sizeof pose, vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None &&
            pose.bActive && pose.pose.bPoseIsValid) { std::memcpy(m, pose.pose.mDeviceToAbsoluteTracking.m, 12 * sizeof(float)); return true; }
        if (menuAim_) { std::memcpy(m, menuAimPose_, 12 * sizeof(float)); return true; }
        return false;
    }
    bool leftPose(float m[12]) const {
        if (test_) { if (testLeftOk_) std::copy(testLeft_, testLeft_ + 12, m); return testLeftOk_; }
        vr::InputPoseActionData_t pose = {};
        if (!leftAim_ || vr::VRInput()->GetPoseActionDataForNextFrame(leftAim_, vr::TrackingUniverseStanding, &pose, sizeof pose, vr::k_ulInvalidInputValueHandle) != vr::VRInputError_None ||
            !pose.bActive || !pose.pose.bPoseIsValid) return false;
        std::memcpy(m, pose.pose.mDeviceToAbsoluteTracking.m, 12 * sizeof(float));
        return true;
    }

    GravityWalker walker_;
    float targetN_[3] = {0, 1, 0};                        // the teleport target's surface normal
    bool gravityMode_ = false;
    float fallV_ = 0, fallen_ = 0, fallLimit_ = 30;
    int respawns_ = 0;
    int seated_ = -1;
    float seatRigY_ = 0;
    bool ownInput_ = false;
    bool menuAim_ = false;
    float menuAimPose_[12] = {};
    float lift_ = 0, lowTime_ = 0, seenTime_ = 0;
    bool poseValid_ = true;
    bool liftDecided_ = false;
    bool ok_ = false;
    vr::VRActionSetHandle_t set_ = 0;
    vr::VRActionHandle_t leftStick_ = 0, rightStick_ = 0, rightAim_ = 0, leftAim_ = 0, gaze_ = 0;
    vr::VRActionHandle_t trigger_[2] = {0, 0};           // 0 right, 1 left
    bool trigHeld_[2] = {false, false};                  // last frame's (a press is the edge)
    int pointed_[2] = {-1, -1};                          // the seat each hand points at
    vr::VROverlayHandle_t ring_ = 0;
    bool ringOn_ = true;
    LocoGuide guide_;
    float vel_[2] = {0, 0};                              // walking velocity (world x, z), eased
    float stepV_ = 0;                                    // a step's easing rate
    float lastHead_[2] = {0, 0};
    // Seats: a ball of kSeatRadius over each seat point is what the arc and pointing hands hit (and the chair's own
    // surfaces within kSeatOwn of it); markers show within kMarkerReach, pointing reaches kPointReach.
    static constexpr float kSeatRadius = 0.35f, kSeatOwn = 0.65f, kSeatLift = 0.1f, kMarkerReach = 2.0f, kPointReach = 10.0f;   // markers: only seats right by you
    HomeWorld world_;
    WorldRuntime runtime_;
    float rigX_ = 0, rigZ_ = 0, rigYaw_ = 0, floorY_ = 0, targetFloor_ = 0;
    bool turnArmed_ = true, aiming_ = false, valid_ = false;
    float target_[3] = {0, 0, 0};
    int seat_ = -1;
    // Options (setOptions) and the jump in flight: where it lands (copied from the aim on release), a blink's time
    // to black, a dash's progress 0..1 (-1: none).
    int walk_ = 1, teleport_ = 1, turn_ = 0;
    float snap_ = 0.5236f, speed_ = 2.0f;
    float jump_[3] = {0, 0, 0}, jumpN_[3] = {0, 1, 0};
    int jumpSeat_ = -1;
    float blinkT_ = -1, slideT_ = -1, slideDX_ = 0, slideDZ_ = 0, slideY0_ = 0;
    bool test_ = false, testAimOk_ = false, testLeftOk_ = false, testTrig_[2] = {false, false};
    float testIn_[4] = {}, testAim_[12] = {}, testLeft_[12] = {};

    // Where you stand to sit in seat i: over it, on the floor under it (the seat's own top when it is one).
    void seatTarget(int i, float t[3]) const {
        const WorldPoint& s = world_.seats[i];
        t[0] = s.pos[0]; t[1] = s.pos[1] - 0.45f; t[2] = s.pos[2];
        float f;
        if (runtime_.floorAt(t[0], t[2], s.pos[1], 0.2f, 2.0f, f)) t[1] = f;
    }
    // The seat whose furniture a surface hit at q likely belongs to (within kSeatOwn of the seat point), or -1.
    int seatNear(const float q[3]) const {
        int best = -1;
        float bd = kSeatOwn * kSeatOwn;
        for (size_t i = 0; i < world_.seats.size(); ++i) {
            const float* p = world_.seats[i].pos;
            const float dx = q[0] - p[0], dy = q[1] - (p[1] + kSeatLift), dz = q[2] - p[2], dd = dx * dx + dy * dy + dz * dz;
            if (dd < bd) { bd = dd; best = static_cast<int>(i); }
        }
        return best;
    }
    // seatNear, or the seat whose own collider `shape` is (centred on it: Meta gives chairs a ball or box that can
    // be well over a metre across).
    int seatOf(const float q[3], const WorldCollider* shape) const {
        const int i = seatNear(q);
        return i >= 0 || !shape ? i : seatNear(&shape->world[12]);
    }
    // The first seat ball a segment o + d t, t in [0, 1], enters, and where (t).
    int seatOnSegment(const float o[3], const float d[3], float& t) const {
        int best = -1;
        t = 2;
        for (size_t i = 0; i < world_.seats.size(); ++i) {
            const float* p = world_.seats[i].pos;
            const float oc[3] = {o[0] - p[0], o[1] - (p[1] + kSeatLift), o[2] - p[2]};
            const float A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], B = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
            const float C = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - kSeatRadius * kSeatRadius, disc = B * B - A * C;
            if (A < 1e-12f || disc < 0 || C < 0) continue;                      // (a hand inside one isn't pointing)
            const float th = (-B - std::sqrt(disc)) / A;
            if (th >= 0 && th <= 1 && th < t) { t = th; best = static_cast<int>(i); }
        }
        return best;
    }

    static float dead(float v) { return std::fabs(v) < 0.15f ? 0.0f : (v - std::copysign(0.15f, v)) / 0.85f; }
    static void apply(const float m[16], const float p[3], float o[3]) { for (int k = 0; k < 3; ++k) o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k]; }
    static void dir(const float m[16], const float d[3], float o[3]) { for (int k = 0; k < 3; ++k) o[k] = m[k] * d[0] + m[4 + k] * d[1] + m[8 + k] * d[2]; }
    float headYaw(const float R[16], const float headT[16]) const {
        float f[3];
        const float hz[3] = {-headT[8], -headT[9], -headT[10]};
        dir(R, hz, f);
        return std::atan2(-f[0], -f[2]);
    }

    // The teleport arc from the right hand: a parabola (10 m/s: about 10 m on the flat) traced in 30 ms segments until
    // it meets something; a seat's ball sits you in it, and where else it ends puts you somewhere if it can
    // (WorldRuntime::landingFor: a floor, the floor by a table's top, a wall's or furniture's foot, the floor under a
    // ceiling). Leaves don't stop it. Measured (tools/walktest/teleportfeel): every floor in sight 1.5-10 m away is
    // reachable.
    void aim(const float R[16], const float head[3], float dt) {
        if (!aiming_) guide_.aimTime = 0;
        aiming_ = true;
        valid_ = false;
        seat_ = -1;
        guide_.aimTime += dt;
        guide_.arc.clear();
        targetN_[0] = targetN_[2] = 0; targetN_[1] = 1;
        float m[12];
        if (handPose(m)) {
            const float po[3] = {m[3], m[7], m[11]}, pd[3] = {-m[2], -m[6], -m[10]};
            float o[3], d[3];
            apply(R, po, o);
            dir(R, pd, d);
            traceArc(head, o, d);
        }
        if (valid_ && seat_ < 0) {                                        // snap onto a seat aimed right at, or a hotspot
            float best = 0.3f;
            for (size_t i = 0; i < world_.seats.size(); ++i) {
                const float* s = world_.seats[i].pos, dd = std::hypot(s[0] - target_[0], s[2] - target_[2]);
                if (dd < best) { best = dd; seat_ = static_cast<int>(i); }
            }
            if (seat_ >= 0) seatTarget(seat_, target_);
            else {
                for (const WorldPoint& h : world_.hotspots)
                    if (std::hypot(h.pos[0] - target_[0], h.pos[2] - target_[2]) < 1.0f) { target_[0] = h.pos[0]; target_[1] = h.pos[1]; target_[2] = h.pos[2]; break; }
            }
        }
        if (seat_ >= 0) { targetN_[0] = targetN_[2] = 0; targetN_[1] = 1; }
        guide_.aiming = !guide_.arc.empty();
        guide_.valid = valid_;
        guide_.seat = seat_ >= 0;
        std::copy(target_, target_ + 3, guide_.target);
        std::copy(targetN_, targetN_ + 3, guide_.normal);
        if (!ring_ || !ringOn_) return;
        if (!valid_) { vr::VROverlay()->HideOverlay(ring_); return; }
        // tracking = rig^-1 * world; the ring lies flat (rotated -90 degrees about X).
        const float c = std::cos(rigYaw_), s = std::sin(rigYaw_);
        float Rn[16];
        rig(Rn);
        const float wx = target_[0] - rigX_, wy = target_[1] + 0.02f - Rn[13], wz = target_[2] - rigZ_;
        const float tx = c * wx - s * wz, tz = s * wx + c * wz;
        vr::HmdMatrix34_t t = {{{1, 0, 0, tx}, {0, 0, 1, wy}, {0, -1, 0, tz}}};
        vr::VROverlay()->SetOverlayTransformAbsolute(ring_, vr::TrackingUniverseStanding, &t);
        vr::VROverlay()->ShowOverlay(ring_);
    }

    // The arc's flight from o along d (world), into guide_.arc; sets valid_, target_, targetN_, seat_.
    void traceArc(const float head[3], const float o[3], const float d[3]) {
        auto keep = [&](const float* q) { guide_.arc.insert(guide_.arc.end(), q, q + 3); };
        keep(o);
        if (!runtime_.hasFloor()) return;                                 // nothing to land on anywhere
        {   // a hand reached through a wall (standing right at it) doesn't aim from the far side
            const float hs[3] = {o[0] - head[0], o[1] - head[1], o[2] - head[2]};
            float t, n[3];
            if (runtime_.trace(head, hs, t, n) == WorldRuntime::Block) return;
        }
        float p[3] = {o[0], o[1], o[2]}, v[3] = {d[0] * kArcSpeed, d[1] * kArcSpeed, d[2] * kArcSpeed};
        for (int i = 0; i < 90; ++i) {
            const float dt = 0.03f;
            const float seg[3] = {v[0] * dt, (v[1] - 4.9f * dt) * dt, v[2] * dt};
            float t, n[3], ts;
            const WorldCollider* shape = nullptr;
            const WorldRuntime::Hit h = runtime_.trace(p, seg, t, n, &shape);
            const int si = seatOnSegment(p, seg, ts);
            if (si >= 0 && (h == WorldRuntime::None || ts <= t)) {
                const float q[3] = {p[0] + seg[0] * ts, p[1] + seg[1] * ts, p[2] + seg[2] * ts};
                keep(q);
                seat_ = si;
                seatTarget(si, target_);
                valid_ = true;
                return;
            }
            if (h != WorldRuntime::None) {
                const float q[3] = {p[0] + seg[0] * t, p[1] + seg[1] * t, p[2] + seg[2] * t};
                keep(q);
                if (h == WorldRuntime::Block && (seat_ = seatOf(q, shape)) >= 0) {   // the chair's back or arm: the chair
                    seatTarget(seat_, target_);
                    valid_ = true;
                } else if (runtime_.landingFor(h, q, n, target_)) {
                    valid_ = true;
                    const bool onIt = h == WorldRuntime::Land && std::fabs(target_[1] - q[1]) < 0.05f;
                    for (int k = 0; k < 3; ++k) targetN_[k] = onIt ? n[k] : (k == 1 ? 1.0f : 0.0f);
                }
                return;
            }
            for (int k = 0; k < 3; ++k) p[k] += seg[k];
            keep(p);
            v[1] -= 9.8f * dt;
            if (p[1] < floorY_ - 30) return;
        }
    }
};
