#include "locomotion.h"
#include "hsl_scene.h"
#include <cstdio>
static int fails = 0;
static void ck(bool ok, const char* w, float a = 0, float b = 0, float c = 0) { std::printf("%s  %s  (%.2f %.2f %.2f)\n", ok ? "PASS" : "FAIL", w, a, b, c); fails += !ok; }
int main() {
    HomeWorld w;
    WorldCollider fl; fl.kind = WorldCollider::Mesh; fl.role = WorldCollider::Floor;
    fl.positions = {-10, 0, -10, 10, 0, -10, 10, 0, 10, -10, 0, 10}; fl.indices = {0, 2, 1, 0, 3, 2}; w.colliders.push_back(fl);
    WorldPoint seat; seat.name = "Armchair"; seat.pos[0] = 2; seat.pos[1] = 0.45f; seat.pos[2] = -1; seat.yaw = 1.5708f; w.seats.push_back(seat);
    Locomotion loco;
    loco.setWorld(w);
    float head[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};   // standing, looking down -Z
    auto eye = [&](float* o) { float R[16]; loco.rig(R); for (int k = 0; k < 3; ++k) o[k] = R[k] * head[12] + R[4 + k] * head[13] + R[8 + k] * head[14] + R[12 + k]; };
    auto facing = [&]() { float R[16]; loco.rig(R); const float f[3] = {-(R[8]), 0, -(R[10])}; return std::atan2(-f[0], -f[2]); };
    float e[3];
    loco.testSit(0, head); eye(e);
    ck(loco.seated() == 0, "landing on a seat sits you in it");
    ck(std::fabs(e[0] - 2) < 1e-3f && std::fabs(e[2] + 1) < 1e-3f, "your head is over the seat", e[0], e[1], e[2]);
    ck(std::fabs(e[1] - (0.45f + 0.78f)) < 1e-3f, "your eyes are at seated height over it (standing in the room)", e[1]);
    ck(std::fabs(facing()) < 0.02f, "sitting keeps the way you face (no turn to the seat's front)", facing());
    for (int i = 0; i < 30; ++i) loco.update(1.0f / 90, head);
    eye(e);
    ck(loco.seated() == 0 && std::fabs(e[0] - 2) < 1e-3f, "sitting still keeps you in the seat", e[0], e[1], e[2]);
    head[13] = 1.20f;                                                // you sit down for real
    for (int i = 0; i < 400; ++i) loco.update(1.0f / 90, head);
    eye(e);
    ck(std::fabs(e[1] - 1.23f) < 0.03f, "sitting down for real eases back to seated eye height", e[1]);
    float R0[16]; loco.rig(R0);
    head[12] = 0.3f;                                                 // leaning: stays seated
    loco.update(1.0f / 90, head);
    ck(loco.seated() == 0, "leaning keeps you seated");
    head[12] = 0.8f;                                                 // stepping away in the room
    loco.update(1.0f / 90, head);
    eye(e);
    ck(loco.seated() == -1, "stepping 0.8 m away stands you up", e[0], e[1], e[2]);
    ck(std::fabs(e[1] - 1.20f) < 0.02f, "standing again, the floor is the floor", e[1]);
    head[12] = 0; head[13] = 1.70f;
    loco.testSit(0, head);
    loco.testWalkOff(head);
    eye(e);
    ck(loco.seated() == -1 && std::fabs(e[0] - 1.5f) < 0.02f && std::fabs(e[2] + 1) < 0.02f && std::fabs(e[1] - 1.70f) < 0.02f,
       "walking off steps you out half a metre in front of the seat, standing", e[0], e[1], e[2]);
    {   // the stick in a seat (slide teleports): you dash out to its stand-up spot (none named: half a metre in front)
        Locomotion l2; l2.setWorld(w); l2.setOptions(1, 2, 0, 30, 2);
        float hd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        l2.testSit(0, hd);
        l2.testInput(0, 1, 0, 0);
        l2.update(1.0f / 90, hd);
        l2.testInput(0, 0, 0, 0);
        for (int i = 0; i < 90; ++i) l2.update(1.0f / 90, hd);
        float R2[16]; l2.rig(R2);
        const float ex = R2[0] * hd[12] + R2[4] * hd[13] + R2[8] * hd[14] + R2[12], ey = R2[1] * hd[12] + R2[5] * hd[13] + R2[9] * hd[14] + R2[13],
                    ez = R2[2] * hd[12] + R2[6] * hd[13] + R2[10] * hd[14] + R2[14];
        ck(l2.seated() == -1 && std::fabs(ex - 1.5f) < 0.05f && std::fabs(ez + 1) < 0.05f && std::fabs(ey - 1.70f) < 0.05f,
           "the stick in a seat dashes you out to its stand-up spot, standing", ex, ey, ez);
    }
    {   // falling off a small platform (roomscale) in an ordinary home: you fall, then you're back at the spawn
        HomeWorld w2;
        WorldCollider pl; pl.kind = WorldCollider::Mesh; pl.role = WorldCollider::Solid;
        pl.positions = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1}; pl.indices = {0, 2, 1, 0, 3, 2}; w2.colliders.push_back(pl);
        w2.fallLimit = 15;
        Locomotion l2; l2.setWorld(w2);
        float hd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        for (int i = 0; i < 10; ++i) l2.update(1.0f / 90, hd);
        hd[12] = 2.5f;                                                   // step off the edge in the room
        int frames = 0;
        float lowest = 1e9f;
        while (l2.respawns() == 0 && frames < 900) { l2.update(1.0f / 90, hd); float R[16]; l2.rig(R); lowest = std::min(lowest, R[13]); ++frames; }
        ck(l2.respawns() == 1 && lowest < -14.0f, "stepping off a platform you fall, and after 15 m you're back at the spawn", frames / 90.0f, lowest);
    }
    {   // a home with a gravity zone goes to the gravity walker
        HomeWorld w3;
        WorldCollider s3; s3.kind = WorldCollider::Sphere; s3.role = WorldCollider::Solid; s3.radius = 10; w3.colliders.push_back(s3);
        WorldGravity g3; g3.kind = WorldGravity::Planet; g3.radius = 40; w3.gravity.push_back(g3);
        WorldPoint sp3; sp3.pos[1] = 10; w3.spawns.push_back(sp3);
        Locomotion l3; l3.setWorld(w3);
        float hd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        for (int i = 0; i < 30; ++i) l3.update(1.0f / 90, hd);
        float R[16]; l3.rig(R);
        const float ey = R[4] * hd[12] + R[5] * hd[13] + R[6] * hd[14] + R[13];
        ck(l3.gravityMode() && std::fabs(ey - 11.7f) < 0.05f, "a planet home stands you on the planet (gravity walker)", ey);
    }
    {   // movement options (setOptions), on a plain 20 m floor, nothing near the spawn
        HomeWorld w4;
        w4.colliders.push_back(fl);
        const float dt = 1.0f / 80;
        float hd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
        auto eyeOf = [&](Locomotion& l, float* o) { float R[16]; l.rig(R); for (int k = 0; k < 3; ++k) o[k] = R[k] * hd[12] + R[4 + k] * hd[13] + R[8 + k] * hd[14] + R[12 + k]; };
        auto yawOf = [&](Locomotion& l) { float R[16]; l.rig(R); return std::atan2(R[8], R[10]); };
        auto run = [&](Locomotion& l, int n) { for (int i = 0; i < n; ++i) l.update(dt, hd); };
        // The right hand at chest height, pointing forward and 30 degrees down (row-major 3x4).
        const float aimDown[12] = {1, 0, 0, 0.2f, 0, 0.866f, 0.5f, 1.2f, 0, -0.5f, 0.866f, 0};
        float e0[3], e1[3], t[3];
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(0, 1, 0, 30, 2);
            l.testInput(0, 1, 0, 0); eyeOf(l, e0); run(l, 80); eyeOf(l, e1);
            ck(std::hypot(e1[0] - e0[0], e1[2] - e0[2]) < 1e-4f, "walk off: a full left stick for 1 s doesn't move you", e1[0], e1[2]);
            l.setOptions(1, 1, 0, 30, 3);
            run(l, 80); eyeOf(l, e1);
            // (0.15 s ramp from a standstill: 1 s at full stick covers 3 * (1 - 0.075) m)
            ck(std::fabs(e1[2] - e0[2] + 3 * (1 - grav::kAccel / 2)) < 0.05f && std::fabs(e1[0] - e0[0]) < 1e-3f, "walk on at 3 m/s: 1 s forward goes 3 m (less the ramp up)", e1[0], e1[2]);
        }
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(1, 1, 1, 30, 2);
            const float y0 = yawOf(l);
            eyeOf(l, e0);
            l.testInput(0, 0, 1, 0); run(l, 40);                       // 0.5 s full right
            const float y1 = yawOf(l);
            run(l, 40);                                                // 1 s
            const float y2 = yawOf(l);
            eyeOf(l, e1);
            ck(std::fabs((y1 - y0) + 1.0472f) < 0.01f && std::fabs((y2 - y0) + 2.0944f) < 0.01f, "smooth turn: 60 degrees in 0.5 s, 120 in 1 s (right = clockwise)", y1 - y0, y2 - y0);
            ck(std::hypot(e1[0] - e0[0], e1[2] - e0[2]) < 1e-4f, "smooth turn spins about the head", e1[0], e1[2]);
            l.testInput(0, 0, 0.575f, 0); const float y3 = yawOf(l); run(l, 40);
            ck(std::fabs((yawOf(l) - y3) + 0.5236f) < 0.01f, "half deflection past the deadzone turns half as fast", yawOf(l) - y3);
            l.testInput(0, 0, 0.1f, 0); const float y4 = yawOf(l); run(l, 40);
            ck(std::fabs(yawOf(l) - y4) < 1e-6f, "inside the deadzone: no turn", yawOf(l) - y4);
        }
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(1, 1, 0, 45, 2);
            const float y0 = yawOf(l);
            l.testInput(0, 0, -1, 0); run(l, 20);                      // held: one snap
            const float y1 = yawOf(l);
            l.testInput(0, 0, 0, 0); run(l, 2); l.testInput(0, 0, -1, 0); run(l, 2);
            ck(std::fabs((y1 - y0) - 0.7854f) < 1e-4f && std::fabs((yawOf(l) - y0) - 1.5708f) < 1e-4f, "snap angle 45: a held flick turns 45 degrees once, the next another 45", y1 - y0, yawOf(l) - y0);
        }
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(1, 2, 0, 30, 2);
            l.testInput(0, 0, 0, 1, aimDown); run(l, 3);
            const bool aimed = l.testTarget(t);
            eyeOf(l, e0);
            ck(aimed && t[2] < -1.5f && std::fabs(t[1]) < 1e-3f, "aiming forward and down finds the floor ahead", t[0], t[1], t[2]);
            l.testInput(0, 0, 0, 0); run(l, 10);                       // released; 10 frames = 0.125 s = half the dash
            eyeOf(l, e1);
            const float frac = (e1[2] - e0[2]) / (t[2] - e0[2]);
            ck(l.sliding() && std::fabs(frac - 0.5f) < 0.02f, "slide: halfway through the dash, halfway there", frac);
            run(l, 8); const bool still = l.sliding();
            run(l, 4); eyeOf(l, e1);
            ck(still && !l.sliding() && std::fabs(e1[0] - t[0]) < 1e-3f && std::fabs(e1[2] - t[2]) < 1e-3f && std::fabs(e1[1] - 1.70f) < 1e-3f,
               "slide: after 0.25 s you're standing at the target", e1[0], e1[1], e1[2]);
            l.testInput(1, 0, 0, 0); run(l, 1);   // the stick works again afterwards (sanity)
        }
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(1, 1, 0, 30, 2);                                // instant (no compositor here: no blink delay)
            l.testInput(0, 0, 0, 1, aimDown); run(l, 3); l.testTarget(t);
            l.testInput(0, 0, 0, 0); run(l, 1); eyeOf(l, e1);
            ck(std::fabs(e1[0] - t[0]) < 1e-3f && std::fabs(e1[2] - t[2]) < 1e-3f, "instant teleport lands on release", e1[0], e1[2]);
        }
        {
            Locomotion l; l.setWorld(w4);
            l.setOptions(1, 0, 0, 30, 2);
            eyeOf(l, e0);
            l.testInput(0, 0, 0, 1, aimDown); run(l, 10);
            const bool aimed = l.testTarget(t);
            l.testInput(0, 0, 0, 0); run(l, 30); eyeOf(l, e1);
            ck(!aimed && std::hypot(e1[0] - e0[0], e1[2] - e0[2]) < 1e-4f, "teleport off: the stick forward aims nothing and goes nowhere", e1[0], e1[2]);
            l.setOptions(1, 1, 0, 30, 2);
            l.testInput(0, 0, 0, 1, aimDown); run(l, 2);
            ck(l.testTarget(t), "teleport back on: aiming works again live");
        }
        {   // gravity homes honour them too
            HomeWorld w5;
            WorldCollider s5; s5.kind = WorldCollider::Sphere; s5.role = WorldCollider::Solid; s5.radius = 10; w5.colliders.push_back(s5);
            WorldGravity g5; g5.kind = WorldGravity::Planet; g5.radius = 40; w5.gravity.push_back(g5);
            WorldPoint sp5; sp5.pos[1] = 10; w5.spawns.push_back(sp5);
            Locomotion l; l.setWorld(w5);
            l.setOptions(0, 1, 1, 30, 2);
            run(l, 30); eyeOf(l, e0);
            l.testInput(0, 1, 0, 0); run(l, 80); eyeOf(l, e1);
            ck(std::hypot(e1[0] - e0[0], e1[2] - e0[2]) < 1e-3f, "gravity home, walk off: the stick doesn't move you", e1[0], e1[2]);
            l.setOptions(1, 1, 1, 30, 1);
            run(l, 40); eyeOf(l, e1);
            const float arc = 10.0f * std::atan2(std::hypot(e1[0], e1[2]), e1[1]);
            ck(std::fabs(arc - (0.5f - grav::kAccel / 2)) < 0.03f, "gravity home, walk on at 1 m/s: 0.5 s walks 0.5 m round the planet, less the ramp up", arc);
            float R0[16], R1[16]; l.rig(R0);
            l.testInput(0, 0, 1, 0); run(l, 40); l.rig(R1);
            const float cosA = R0[0] * R1[0] + R0[1] * R1[1] + R0[2] * R1[2];
            ck(std::fabs(std::acos(std::clamp(cosA, -1.0f, 1.0f)) - 1.0472f) < 0.02f, "gravity home, smooth turn: 60 degrees in 0.5 s", std::acos(std::clamp(cosA, -1.0f, 1.0f)));
        }
    }
    // ── Walking feel, teleport limits, seats ──
    auto handPose = [](const float p[3], float dx, float dy, float dz, float* m) {   // at p, pointing along d (3x4 row-major)
        const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float z[3] = {-dx / l, -dy / l, -dz / l};
        float x[3] = {z[2], 0, -z[0]};                                   // up x z
        const float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
        if (xl < 1e-4f) { x[0] = 1; x[2] = 0; } else { x[0] /= xl; x[2] /= xl; }
        const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        for (int r = 0; r < 3; ++r) { m[r * 4] = x[r]; m[r * 4 + 1] = y[r]; m[r * 4 + 2] = z[r]; m[r * 4 + 3] = p[r]; }
    };
    auto quadC = [](int role, std::vector<float> pts) {
        WorldCollider q; q.kind = WorldCollider::Mesh; q.role = role; q.positions = pts; q.indices = {0, 1, 2, 0, 2, 3}; return q;
    };
    const float dt = 1.0f / 80;
    float hd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.70f, 0, 1};
    auto eyeOf = [&](Locomotion& l, float* o) { float R[16]; l.rig(R); for (int k = 0; k < 3; ++k) o[k] = R[k] * hd[12] + R[4 + k] * hd[13] + R[8 + k] * hd[14] + R[12 + k]; };
    auto run = [&](Locomotion& l, int n) { for (int i = 0; i < n; ++i) l.update(dt, hd); };
    HomeWorld flat;
    flat.colliders.push_back(quadC(WorldCollider::Floor, {-20, 0, -20, 20, 0, -20, 20, 0, 20, -20, 0, 20}));
    {
        Locomotion l; l.setWorld(flat); l.setOptions(1, 1, 0, 30, 2);
        float a[3], b[3];
        l.testInput(0, 1, 0, 0);
        eyeOf(l, a); run(l, 1); eyeOf(l, b);
        const float first = std::fabs(b[2] - a[2]) / dt;
        run(l, 5); eyeOf(l, a); run(l, 1); eyeOf(l, b);
        const float mid = std::fabs(b[2] - a[2]) / dt;
        run(l, 20); eyeOf(l, a); run(l, 1); eyeOf(l, b);
        const float full = std::fabs(b[2] - a[2]) / dt;
        ck(first < 0.3f && mid > 0.7f && mid < 1.6f && std::fabs(full - 2) < 0.01f, "walking eases up to speed in ~0.15 s (no instant start)", first, mid, full);
        l.testInput(0, 0, 0, 0);
        eyeOf(l, a); run(l, 1); eyeOf(l, b);
        const float after = std::fabs(b[2] - a[2]) / dt;
        run(l, 10); eyeOf(l, a); run(l, 1); eyeOf(l, b);
        ck(after > 1.5f && std::fabs(b[2] - a[2]) < 1e-5f, "letting go slows you to a stop within ~0.12 s (no instant stop)", after, std::fabs(b[2] - a[2]) / dt);
        l.testInput(0, 0.575f, 0, 0); run(l, 40); eyeOf(l, a); run(l, 1); eyeOf(l, b);
        const float half = std::fabs(b[2] - a[2]) / dt;
        ck(half > 0.5f && half < 0.8f, "half a push walks about a third of full speed (small push = slow)", half);
        l.testInput(0.1f, 0.1f, 0, 0); run(l, 40); eyeOf(l, a); run(l, 1); eyeOf(l, b);
        ck(std::hypot(b[0] - a[0], b[2] - a[2]) < 1e-6f, "inside the round deadzone: no drift");
    }
    {   // a 0.2 m step up 1 m ahead, and a 20 degree ramp beyond it
        HomeWorld w = flat;
        w.colliders.push_back(quadC(WorldCollider::Solid, {-5, 0.2f, -1, 5, 0.2f, -1, 5, 0.2f, -4, -5, 0.2f, -4}));
        const float rise = std::tan(20 * 3.14159f / 180) * 4;
        w.colliders.push_back(quadC(WorldCollider::Floor, {-5, 0.2f, -4, 5, 0.2f, -4, 5, 0.2f + rise, -8, -5, 0.2f + rise, -8}));
        Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 1);
        l.testInput(0, 1, 0, 0);
        float e[3], prev = 1.7f, maxJump = 0;
        float onRampErr = 0;
        for (int i = 0; i < 80 * 7; ++i) {
            run(l, 1); eyeOf(l, e);
            maxJump = std::max(maxJump, std::fabs(e[1] - prev));
            prev = e[1];
            if (e[2] < -4.6f && e[2] > -7.5f) {
                const float surface = 0.2f + rise * (-4 - e[2]) / 4;
                onRampErr = std::max(onRampErr, std::fabs(e[1] - 1.7f - surface));
            }
        }
        ck(maxJump < 0.025f, "a 0.2 m step is eased: never more than 2.5 cm in one frame", maxJump);
        ck(onRampErr < 0.01f, "a ramp is followed exactly: eyes stay 1.7 m over it walking up", onRampErr);
    }
    {   // teleporting: a wall 3 m tall across z = -3 on a 40 m floor
        HomeWorld w = flat;
        w.colliders.push_back(quadC(WorldCollider::Solid, {-10, 0, -3, 10, 0, -3, 10, 3, -3, -10, 3, -3}));
        Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
        float m[12], t[3];
        const float hand[3] = {0.2f, 1.2f, 0};
        handPose(hand, 0, 0.25f, -1, m);                                // lobbed over the wall's top? no: into it
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        const bool overWall = l.testTarget(t);
        ck(!overWall || t[2] > -3, "aiming at the floor behind a wall: nowhere (or in front of it), never behind", overWall, t[2]);
        ck(l.guide().aiming && l.guide().arc.size() >= 6 && l.guide().arc[l.guide().arc.size() - 1] > -3.01f, "the drawn arc stops at the wall", l.guide().arc[l.guide().arc.size() - 1]);
        handPose(hand, 0, 0.1f, -1, m);                                  // at the wall, ~0.6 m up
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        ck(l.testTarget(t) && t[2] > -3 && t[2] < -2.5f && std::fabs(t[1]) < 0.05f, "aiming at a wall: you land at its foot, in front of it", t[1], t[2]);
        handPose(hand, 0, 0, -1, m);                                     // at its foot (hits 0.3 m up): lands in front
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        ck(l.testTarget(t) && t[2] > -3 && t[2] < -2.6f, "aiming at a wall's foot: you land in front of it", t[2]);
        const float atWall[3] = {0.2f, 1.2f, -3.2f};                     // standing at the wall, hand through it
        float hd2[16]; std::copy(hd, hd + 16, hd2); hd2[14] = -2.85f;
        handPose(atWall, 0, -0.5f, -1, m);
        l.testInput(0, 0, 0, 1, m); l.update(dt, hd2); l.update(dt, hd2);
        ck(!l.testTarget(t), "a hand reached through the wall aims nothing on the far side");
        l.testInput(0, 0, 0, 0); run(l, 2);
    }
    {   // a ceiling at 2.2 m with a floor on top (a storey above): aiming up never lands on it
        HomeWorld w = flat;
        w.colliders.push_back(quadC(WorldCollider::Solid, {-10, 2.2f, -10, 10, 2.2f, -10, 10, 2.2f, 10, -10, 2.2f, 10}));
        Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
        float m[12], t[3];
        const float hand[3] = {0.2f, 1.2f, 0};
        handPose(hand, 0, 1, -1, m);
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        ck(!l.testTarget(t) || t[1] < 0.1f, "aiming up into a ceiling: invalid, never onto the storey above", t[1]);
    }
    {   // low things: a 0.3 m kerb at z = -1.4 (the arc hits its face low), a table top, a box marked Wall
        const float hand[3] = {0.2f, 1.2f, 0};
        float m[12], t[3];
        handPose(hand, 0, -0.5f, -0.866f, m);                            // lands ~1.53 m ahead on bare floor
        {
            HomeWorld w = flat;
            w.colliders.push_back(quadC(WorldCollider::Solid, {-5, 0, -1.4f, 5, 0, -1.4f, 5, 0.3f, -1.4f, -5, 0.3f, -1.4f}));
            Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
            l.testInput(0, 0, 0, 1, m); run(l, 2);
            ck(l.testTarget(t) && t[2] > -1.4f && t[2] < -1.0f, "the arc hits a kerb's face low: you land at its foot, in front", t[2]);
        }
        {
            HomeWorld w = flat;                                          // a table top 0.7 m up over z -1.2..-2.2
            w.colliders.push_back(quadC(WorldCollider::Solid, {-1, 0.7f, -1.2f, 1, 0.7f, -1.2f, 1, 0.7f, -2.2f, -1, 0.7f, -2.2f}));
            Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
            l.testInput(0, 0, 0, 1, m); run(l, 2);
            ck(!l.testTarget(t), "an arc landing under a table: no headroom, invalid");
            float m2[12];
            handPose(hand, 0, -0.15f, -1, m2);                           // flatter: comes down on the table top
            l.testInput(0, 0, 0, 1, m2); run(l, 2);
            ck(l.testTarget(t) && std::fabs(t[1] - 0.7f) < 1e-3f, "a solid table top is floor (the existing rule): you can land on it", t[1], t[2]);
        }
        {
            HomeWorld w = flat;
            WorldCollider b; b.kind = WorldCollider::Box; b.role = WorldCollider::Wall;
            b.world[12] = 0; b.world[13] = 0.4f; b.world[14] = -1.7f; b.half[0] = 1; b.half[1] = 0.4f; b.half[2] = 0.5f;
            w.colliders.push_back(b);
            Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
            float m2[12];
            handPose(hand, 0, 0, -1, m2);
            l.testInput(0, 0, 0, 1, m2); run(l, 2);
            ck(!l.testTarget(t) || std::fabs(t[1]) < 0.05f, "a box marked Wall is never landed on (its top): only the floor round it", t[1], t[2]);
            const bool r = l.testTarget(t);
            ck(l.guide().arc.size() >= 6 && l.guide().arc[l.guide().arc.size() - 3 + 1] <= 0.81f, "the arc stops on the Wall box", r, l.guide().arc[l.guide().arc.size() - 2]);
        }
    }
    {   // seats: pointable, a trigger sits you, markers near you
        HomeWorld w = flat;
        WorldPoint s1; s1.name = "Sofa"; s1.pos[0] = 0; s1.pos[1] = 0.45f; s1.pos[2] = -3; s1.yaw = 0; w.seats.push_back(s1);
        WorldPoint s2; s2.name = "Far bench"; s2.pos[0] = 15; s2.pos[1] = 0.45f; s2.pos[2] = 0; w.seats.push_back(s2);
        Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
        const float o[3] = {0.2f, 1.2f, 0}, toSeat[3] = {-0.2f / 3.076f, -0.65f / 3.076f, -3 / 3.076f};
        float t;
        ck(l.seatOnRay(o, toSeat, 10, t) == 0 && t > 2.5f && t < 3.1f, "a ray at the sofa hits its seat volume", t);
        const float away[3] = {1, 0, 0};
        ck(l.seatOnRay(o, away, 10, t) == -1, "a ray elsewhere hits no seat");
        run(l, 1);
        ck(l.guide().seats.empty(), "markers: only seats right by you (the sofa 3 m away and the bench 15 m away aren't shown)", static_cast<float>(l.guide().seats.size()));
        float m[12];
        const float lh[3] = {-0.2f, 1.2f, 0};
        handPose(lh, 0.2f, -0.65f, -3, m);
        l.testInput(0, 0, 0, 0);
        l.testButtons(false, false, m); run(l, 1);
        ck(l.pointedSeat(1) == 0 && l.guide().seats.size() == 1 && l.guide().seats[0].hot && l.guide().pointer, "pointing the left hand at it lights its marker and shows the pointer");
        l.testButtons(false, true, m); run(l, 2);
        float e[3]; eyeOf(l, e);
        ck(l.seated() == 0 && std::fabs(e[2] + 3) < 1e-3f && std::fabs(e[1] - (0.45f + 0.78f)) < 1e-3f, "the trigger while pointing sits you in it", e[0], e[1], e[2]);
        run(l, 1);
        ck(l.guide().seats.empty(), "seated: your own seat's marker is hidden");
        l.testButtons(false, false, nullptr);
        // standing again elsewhere, a slide setting dashes you into it
        Locomotion l2; l2.setWorld(w); l2.setOptions(1, 2, 0, 30, 2);
        l2.testInput(0, 0, 0, 0);
        l2.testButtons(false, false, m); run(l2, 1);
        l2.testButtons(false, true, m); run(l2, 1);
        const bool dashing = l2.sliding();
        run(l2, 30);
        ck(dashing && l2.seated() == 0, "with slide teleports the trigger dashes you into the seat", dashing, static_cast<float>(l2.seated()));
        // the teleport arc onto the seat ball
        Locomotion l3; l3.setWorld(w); l3.setOptions(1, 1, 0, 30, 2);
        bool seatAimed = false;
        for (float up = -0.4f; up <= 0.4f && !seatAimed; up += 0.025f) {   // some aim at it (whatever the arc's speed)
            handPose(o, -0.065f, up, -1, m);
            l3.testInput(0, 0, 0, 1, m); run(l3, 2);
            seatAimed = l3.testTargetSeat() == 0 && l3.guide().seat;
        }
        ck(seatAimed, "the teleport arc hits the seat's volume: it targets the seat", static_cast<float>(l3.testTargetSeat()));
    }
    {   // a home with no world layer: floors and walls come from its drawn meshes (walkWorld -> geometryColliders)
        HslScene sc;
        auto mesh = [&](const char* name, std::vector<float> p) {
            MeshData md; md.name = name; md.positions = p; md.nVerts = 4; md.indices = {0, 1, 2, 0, 2, 3}; sc.meshes.push_back(md);
        };
        mesh("floor", {-20, 0, -20, 20, 0, -20, 20, 0, 20, -20, 0, 20});
        mesh("wall", {-10, 0, -3, 10, 0, -3, 10, 3, -3, -10, 3, -3});
        Locomotion l; l.setWorld(walkWorld(sc)); l.setOptions(1, 1, 0, 30, 2);
        float m[12], t[3];
        const float hand[3] = {0.2f, 1.2f, 0};
        handPose(hand, 0, 0.25f, -1, m);
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        const bool ok = l.testTarget(t);
        ck(!ok || t[2] > -3, "geometry home: the drawn wall stops the arc (never lands behind it)", ok, t[2]);
        handPose(hand, 0, -0.5f, -0.866f, m);
        l.testInput(0, 0, 0, 1, m); run(l, 2);
        ck(l.testTarget(t) && t[2] < 0 && t[2] > -3 && std::fabs(t[1]) < 0.05f, "geometry home: the drawn floor in front is landable", t[1], t[2]);
        l.testInput(0, 1, 0, 0); run(l, 80 * 3);
        float e[3]; eyeOf(l, e);
        ck(e[2] > -3 && e[2] < -2.6f, "geometry home: walking into the drawn wall stops you at it", e[2]);
    }
    {   // a Wall-role part blocks at any slope: a 42 degree face (|n.y| 0.74) used to be neither floor nor wall
        HomeWorld w = flat;
        const float top = 2 * std::tan(42 * 3.14159f / 180);
        w.colliders.push_back(quadC(WorldCollider::Wall, {-5, 0, -1, 5, 0, -1, 5, top, -3, -5, top, -3}));
        Locomotion l; l.setWorld(w); l.setOptions(1, 1, 0, 30, 2);
        l.testInput(0, 1, 0, 0); run(l, 80 * 3);
        float e[3]; eyeOf(l, e);
        ck(e[2] > -1.6f, "walking into a 42 degree Wall-role face stops you", e[2]);
        float m[12], t[3];
        const float hand[3] = {0.2f, 1.2f, 0};
        Locomotion l2; l2.setWorld(w); l2.setOptions(1, 1, 0, 30, 2);
        handPose(hand, 0, 0.1f, -1, m);
        l2.testInput(0, 0, 0, 1, m); run(l2, 2);
        ck(!l2.testTarget(t) || t[2] > -1.0f, "and a teleport arc can't land on or behind it", t[2]);
    }
    {   // Meta's chairs carry a big ball collider: hitting it counts as pointing at the chair
        HomeWorld w = flat;
        WorldPoint s1; s1.name = "Chair"; s1.pos[0] = 0; s1.pos[1] = 0.45f; s1.pos[2] = -4; w.seats.push_back(s1);
        WorldCollider b; b.kind = WorldCollider::Sphere; b.role = WorldCollider::Solid; b.radius = 0.6f;
        b.world[0] = b.world[5] = b.world[10] = 2.45f; b.world[13] = 0.45f; b.world[14] = -4;   // 1.47 m across in world
        w.colliders.push_back(b);
        Locomotion l; l.setWorld(w);
        const float o[3] = {0.2f, 1.2f, 0}, d[3] = {-0.2f / 4.05f, -0.65f / 4.05f, -4 / 4.05f};
        float t;
        ck(l.seatOnRay(o, d, 10, t) == 0 && t < 3.0f, "a ray stopped by the chair's own (big) collider points at the chair", t);
    }
    std::printf("%d failed\n", fails);
    return fails;
}
