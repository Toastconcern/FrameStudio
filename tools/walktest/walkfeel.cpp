// Simulated stick walking over a home's walkable area (only the public WalkGrid API, so it builds against old and new):
// random headings held ~1 s, 90 Hz, 1.6 m/s, floor followed like Locomotion. Reports leaving the area, distance kept,
// judder (heading flips of the actual motion while the stick is held still) and stuck frames.
#include "hsl_scene.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <cstdlib>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) return 1;
    HomeWorld w = walkWorld(s);
    const WalkGrid& g = *w.walkGridCache;
    std::vector<std::array<float, 3>> starts;
    for (auto& p : w.spawns) starts.push_back({p.pos[0], p.pos[1], p.pos[2]});
    if (starts.empty()) starts.push_back({0, 0, 0});
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(0, 1);
    long flips = 0, frames = 0, left = 0, stuck = 0, sliding = 0; double want = 0, got = 0, judder = 0; double slideLen = 0;
    const float dt = 1 / 90.0f, speed = 1.6f;
    auto t0 = std::chrono::steady_clock::now();
    for (int run = 0; run < 40; ++run) {
        float x = starts[run % starts.size()][0], y = starts[run % starts.size()][1], z = starts[run % starts.size()][2], f;
        if (!g.roomAt(x, y, z, 1.0f)) continue;
        if (g.floorAt(x, z, y + 0.3f, 0.3f, 1.0f, f)) y = f;
        float hd = U(rng) * 6.2832f, pdx = 0, pdz = 0; bool pslide = false;
        for (int i = 0; i < 2700; ++i) {
            if (i % 90 == 0) { hd = U(rng) * 6.2832f; pdx = pdz = 0; pslide = false; }
            const float dx = std::cos(hd) * speed * dt, dz = std::sin(hd) * speed * dt;
            const float ox = x, oz = z;
            const bool wasIn = g.walkable(x, z, y);
            g.move(x, z, y, dx, dz);
            if (g.floorAt(x, z, y, 0.45f, 1.2f, f)) y = f;
            const float mx = x - ox, mz = z - oz, m = std::hypot(mx, mz), r = std::hypot(dx, dz);
            ++frames; want += r; got += m;
            if (wasIn && !g.walkable(x, z, y)) {
                ++left;
                if (std::getenv("WHY") && left <= 8) {
                    float h1, h2;
                    const float f1 = g.field(x, z, y, &h1), f2 = g.field(ox, oz, y, &h2);
                    std::printf("left at (%.3f %.3f) y %.3f: field %.4f (h %.3f); from (%.3f %.3f) field there %.4f; moved %.3f of %.3f\n", x, z, y, f1, h1, ox, oz, f2, std::hypot(x - ox, z - oz), std::hypot(dx, dz));
                }
            }
            if (m < 0.1f * r) ++stuck;
            if (m > 0.1f * r && m < 0.95f * r) {                     // sliding along an edge
                ++sliding; slideLen += m;
                const float pm = std::hypot(pdx, pdz);
                if (pm > 1e-5f) { const float c = (mx * pdx + mz * pdz) / (m * pm); judder += std::acos(std::clamp(c, -1.0f, 1.0f)); if (pslide && c < 0.866f) ++flips; }
                pslide = true;
            } else pslide = false;
            pdx = mx; pdz = mz;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {   // roomscale: walking and leaning for real (no stick), as Locomotion keeps you out of things: the eyes (1.6 m up,
        // swaying) are pushed back from anything solid, the body out of anything it stands in. Inside: frames where
        // something solid is still nearer the eyes than 12 cm after the push.
        long rf = 0, inside = 0, pushed = 0;
        double pushLen = 0;
        for (int run = 0; run < 40; ++run) {
            float x = starts[run % starts.size()][0], y = starts[run % starts.size()][1], z = starts[run % starts.size()][2], f;
            if (!g.roomAt(x, y, z, 1.0f)) continue;
            if (g.floorAt(x, z, y + 0.3f, 0.3f, 1.0f, f)) y = f;
            float hd = U(rng) * 6.2832f;
            for (int i = 0; i < 2700; ++i) {
                if (i % 60 == 0) hd = U(rng) * 6.2832f;
                x += std::cos(hd) * 0.8f / 90; z += std::sin(hd) * 0.8f / 90;   // walking for real, 0.8 m/s
                const float eyeY = y + 1.6f - 0.5f * (0.5f + 0.5f * std::sin(i * 0.02f));   // and bending down now and then
                float push[2];
                if (g.headPush(x, eyeY, z, 0.15f, push) || g.bodyPush(x, y, z, push)) { x += push[0]; z += push[1]; ++pushed; pushLen += std::hypot(push[0], push[1]); }
                if (g.floorAt(x, z, y, 0.45f, 0.45f, f)) y = f;
                ++rf;
                float p2[2];
                if (g.headPush(x, eyeY, z, 0.12f, p2)) ++inside;
            }
        }
        std::printf("roomscale: frames %ld  pushed %.1f%% (avg %.1f cm)  eyes still within 12 cm of something %ld\n", rf, 100.0 * pushed / std::max(1L, rf),
                    pushed ? 100.0 * pushLen / pushed : 0.0, inside);
    }
    std::printf("frames %ld  left-area %ld  kept %.1f%%  stuck %.1f%%  sliding %.1f%%  judder %.1f deg/m, zig-zag flips %.2f%% of sliding  (%.2f us/frame)\n",
                frames, left, 100 * got / want, 100.0 * stuck / frames, 100.0 * sliding / frames, slideLen > 0 ? judder * 57.3 / slideLen : 0, sliding ? 100.0 * flips / sliding : 0.0, ms * 1000 / frames);
    return 0;
}
