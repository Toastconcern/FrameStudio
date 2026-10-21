// Teleporting around a home, as Frame Home's arc does (Locomotion::traceArc over WorldRuntime): from random places you
// stand to random floors 1.5-10 m away, is there an aim that lands you within 50 cm of it? Compares the old landing
// rules (a floor from above, a wall's foot only when hit low) at 7 m/s with WorldRuntime::landingFor at 7 and 10 m/s.
#include "hsl_scene.h"
#include "world_runtime.h"
#include <cmath>
#include <cstdio>
#include <random>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) return 1;
    HomeWorld w = walkWorld(s);
    WorldRuntime rt;
    rt.build(w);
    const WalkGrid& g = *w.walkGridCache;
    std::vector<std::array<float, 3>> floors;                         // places you stand (cell centres well inside)
    for (int z = 0; z < g.nz; ++z) for (int x = 0; x < g.nx; ++x)
        for (const WalkSpan* q = g.begin(x, z); q != g.end(x, z); ++q)
            if ((q->flags & kWalkOk) && q->dist >= 20) floors.push_back({g.centerX(x), q->hi, g.centerZ(z)});
    if (floors.empty()) return 1;
    std::mt19937 rng(11);
    std::uniform_int_distribution<size_t> pick(0, floors.size() - 1);
    auto oldLanding = [&](WorldRuntime::Hit h, const float* q, const float* n, float* out) {
        auto landAt = [&](float x, float y, float z) {
            if (!rt.roomAt(x, y, z)) return false;
            float f;
            if (rt.floorAt(x, z, y, 0.1f, 0.1f, f)) y = f;
            const float from[3] = {x, y + 0.05f, z}, up[3] = {0, 1.2f, 0};
            float t, hn[3];
            if (rt.trace(from, up, t, hn) == WorldRuntime::Block) return false;
            out[0] = x; out[1] = y; out[2] = z;
            return true;
        };
        if (h == WorldRuntime::Land) return landAt(q[0], q[1], q[2]);
        if (std::fabs(n[1]) < 0.7f) {
            const float hl = std::hypot(n[0], n[2]);
            float f;
            if (hl > 1e-4f && rt.floorAt(q[0] + n[0] / hl * 0.25f, q[2] + n[2] / hl * 0.25f, q[1], 0.0f, 0.45f, f))
                return landAt(q[0] + n[0] / hl * 0.25f, f, q[2] + n[2] / hl * 0.25f);
        }
        return false;
    };
    // One aim: where it puts you (false: nowhere).
    auto fly = [&](const float* head, const float* o, const float* d, float speed, int steps, bool newRules, float* out) {
        const float hs[3] = {o[0] - head[0], o[1] - head[1], o[2] - head[2]};
        float t, n[3];
        if (rt.trace(head, hs, t, n) == WorldRuntime::Block) return false;
        float p[3] = {o[0], o[1], o[2]}, v[3] = {d[0] * speed, d[1] * speed, d[2] * speed};
        for (int i = 0; i < steps; ++i) {
            const float dt = 0.03f, seg[3] = {v[0] * dt, (v[1] - 4.9f * dt) * dt, v[2] * dt};
            const WorldRuntime::Hit h = rt.trace(p, seg, t, n);
            if (h != WorldRuntime::None) {
                const float q[3] = {p[0] + seg[0] * t, p[1] + seg[1] * t, p[2] + seg[2] * t};
                return newRules ? rt.landingFor(h, q, n, out) : oldLanding(h, q, n, out);
            }
            for (int k = 0; k < 3; ++k) p[k] += seg[k];
            v[1] -= 9.8f * dt;
        }
        return false;
    };
    struct Mode { const char* name; float speed; int steps; bool rules; int ok = 0; };
    Mode modes[] = {{"old rules, 7 m/s", 7, 90, false}, {"new rules, 10 m/s", 10, 90, true}, {"new rules, 12 m/s", 12, 90, true}};
    int seenMiss[3] = {};                                             // missed though it's in plain sight
    std::vector<std::array<float, 6>> examples;
    int pairs = 0, near = 0, far = 0;
    int okNear[3] = {}, okFar[3] = {};
    for (int tries = 0; pairs < 400 && tries < 20000; ++tries) {
        const auto A = floors[pick(rng)], B = floors[pick(rng)];
        const float dx = B[0] - A[0], dz = B[2] - A[2], dist = std::hypot(dx, dz);
        if (dist < 1.5f || dist > 10.0f || std::fabs(B[1] - A[1]) > 3.0f) continue;
        ++pairs;
        (dist < 5 ? near : far)++;
        const float ux = dx / dist, uz = dz / dist;
        const float head[3] = {A[0], A[1] + 1.6f, A[2]}, hand[3] = {A[0] + ux * 0.3f, A[1] + 1.2f, A[2] + uz * 0.3f};
        for (int m = 0; m < 3; ++m) {
            bool hit = false;
            for (float side = 0; side <= 0.12f && !hit; side += 0.04f)          // a little left or right too
                for (int sgn = -1; sgn <= 1 && !hit; sgn += 2) {
                    const float yaw = std::atan2(uz, ux) + sgn * side;
                    for (float pitch = -60; pitch <= 60 && !hit; pitch += 1.0f) {
                        const float cp = std::cos(pitch * 0.0174533f), d[3] = {std::cos(yaw) * cp, std::sin(pitch * 0.0174533f), std::sin(yaw) * cp};
                        float out[3];
                        if (fly(head, hand, d, modes[m].speed, modes[m].steps, modes[m].rules, out) &&
                            std::hypot(out[0] - B[0], out[2] - B[2]) <= 0.5f && std::fabs(out[1] - B[1]) < 0.3f) hit = true;
                    }
                }
            if (hit) { ++modes[m].ok; (dist < 5 ? okNear : okFar)[m]++; }
            else {   // in plain sight: nothing between the eyes and a point just over it
                const float to[3] = {B[0] - head[0], B[1] + 0.3f - head[1], B[2] - head[2]};
                float t, n[3];
                if (rt.trace(head, to, t, n) == WorldRuntime::None || t > 0.97f) {
                    ++seenMiss[m];
                    if (m == 1 && examples.size() < 6) examples.push_back({A[0], A[1], A[2], B[0], B[1], B[2]});
                }
            }
        }
    }
    std::printf("%d pairs (%d within 5 m, %d 5-10 m)\n", pairs, near, far);
    for (int m = 0; m < 3; ++m)
        std::printf("  %-18s reachable %5.1f%%   (within 5 m %5.1f%%, 5-10 m %5.1f%%)\n", modes[m].name, 100.0 * modes[m].ok / std::max(1, pairs),
                    100.0 * okNear[m] / std::max(1, near), 100.0 * okFar[m] / std::max(1, far));
    for (int m = 0; m < 3; ++m) std::printf("  %-18s missed though in plain sight: %d\n", modes[m].name, seenMiss[m]);
    for (const auto& e : examples) std::printf("    e.g. from (%.2f %.2f %.2f) to (%.2f %.2f %.2f)\n", e[0], e[1], e[2], e[3], e[4], e[5]);
    return 0;
}
