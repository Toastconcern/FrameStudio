// How straight the walkable area's edge runs along a straight wall at an angle: a 20 m floor and a 3 m wall turned by
// each angle, baked; the edge found every 2 cm along the wall, its wobble off a straight line reported (cm).
#include "walkgrid.h"
#include <cmath>
#include <cstdio>
int main() {
    for (float deg : {0.0f, 10.0f, 27.0f, 45.0f, 63.0f}) {
        HomeWorld w;
        WorldCollider fl; fl.kind = WorldCollider::Box; fl.role = WorldCollider::Solid; fl.half[0] = 10; fl.half[1] = 0.05f; fl.half[2] = 10; fl.world[13] = -0.05f;
        WorldCollider wa; wa.kind = WorldCollider::Box; wa.role = WorldCollider::Solid; wa.half[0] = 4; wa.half[1] = 1.5f; wa.half[2] = 0.1f;
        const float a = deg * 0.0174533f, c = std::cos(a), s = std::sin(a);
        wa.world[0] = c; wa.world[2] = -s; wa.world[8] = s; wa.world[10] = c; wa.world[12] = 0.37f; wa.world[13] = 1.5f; wa.world[14] = 0.21f;
        w.colliders = {fl, wa};
        WorldPoint sp; sp.pos[0] = -3; sp.pos[2] = 3; w.spawns.push_back(sp);
        std::vector<WalkSource> src;
        std::vector<std::vector<float>> P;
        std::vector<std::vector<uint32_t>> I;
        walkColliderSources(w, src, P, I);
        WalkParams prm;
        auto g = buildWalkGrid(src, walkSeeds(w), prm, nullptr);
        // along the wall (its x axis), the edge's distance from the wall's face on the +z side (towards the spawn side)
        const float nx = s, nz = c;   // the wall frame's z axis in world
        std::vector<float> off;
        for (float t = -3; t <= 3; t += 0.02f) {
            const float bx = wa.world[12] + c * t, bz = wa.world[14] - s * t;
            float lo = 0.0f, hi = 0.6f;
            if (!g->walkable(bx + nx * hi, bz + nz * hi, 0)) continue;
            for (int k = 0; k < 20; ++k) { const float m = (lo + hi) / 2; (g->walkable(bx + nx * m, bz + nz * m, 0) ? hi : lo) = m; }
            off.push_back(hi);
        }
        double m = 0, v = 0, mn = 1e9, mxv = -1e9;
        for (float o : off) { m += o; mn = std::min<double>(mn, o); mxv = std::max<double>(mxv, o); }
        m /= std::max<size_t>(1, off.size());
        for (float o : off) v += (o - m) * (o - m);
        std::printf("wall at %4.0f deg: edge %.1f cm off its face, wobble %.2f cm rms, %.2f cm peak to peak (%zu samples)\n", deg, (m - 0.1) * 100,
                    std::sqrt(v / std::max<size_t>(1, off.size())) * 100, (mxv - mn) * 100, off.size());
    }
}
