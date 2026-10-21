// Invisible walls (2 x 2.5 x 0.1 m Object boxes) at the spawn at 0..165 degrees: the walkable area sampled every 2 cm
// round each, and the closest walkable sample to the wall's footprint (XZ). The area may run up to a wall's face, never
// into it: exit 2 when any sample is inside one.
//   wallcheck <home> [drawn]      drawn: the wall stands on a drawn floor (a Walkable box) too
#include "hsl_scene.h"
#include "walkgrid.h"
#include <cmath>
#include <cstdio>
int main(int argc, char** argv) {
    HslScene s; std::string err;
    if (!loadHslScene(std::string(argv[1]), s, err)) return 1;
    const float* sp = s.world.spawns[0].pos;
    const bool drawn = argc > 2;                                      // on a drawn floor (Walkable box) too
    float worst = 1e9f;
    for (int deg = 0; deg < 180; deg += 15) {
        const float a = deg * 3.14159265f / 180, c = std::cos(a), sn = std::sin(a);
        HomeWorld w = s.world;
        if (drawn) {
            WorldCollider f; f.kind = WorldCollider::Box; f.role = WorldCollider::Walkable; f.half[0] = f.half[2] = 1.5f; f.half[1] = 0.02f;
            f.world[12] = sp[0]; f.world[13] = sp[1] + 0.02f; f.world[14] = sp[2];
            w.colliders.push_back(f);
        }
        WorldCollider b; b.kind = WorldCollider::Box; b.role = WorldCollider::Object; b.half[0] = 1.0f; b.half[1] = 1.25f; b.half[2] = 0.05f;
        const float m[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, sp[0] + 0.013f, sp[1] + 1.25f, sp[2] + 0.027f, 1};   // off the cell grid
        for (int k = 0; k < 16; ++k) b.world[k] = m[k];
        w.colliders.push_back(b);
        auto g = bakeWalkGrid(s, w, WalkParams{});
        float closest = 1e9f;
        for (float dz = -1.6f; dz <= 1.6f; dz += 0.02f)
            for (float dx = -1.6f; dx <= 1.6f; dx += 0.02f) {
                const float x = m[12] + dx, z = m[14] + dz;
                if (!g->walkable(x, z, sp[1] + 0.1f)) continue;
                // distance to the wall's footprint (a rectangle 2 x 0.1 in its own frame)
                const float lx = dx * c - dz * sn, lz = dx * sn + dz * c;
                const float ox = std::fabs(lx) - 1.0f, oz = std::fabs(lz) - 0.05f;
                const float d = ox > 0 || oz > 0 ? std::hypot(std::max(ox, 0.0f), std::max(oz, 0.0f)) : std::max(ox, oz);
                closest = std::min(closest, d);
            }
        std::printf("%3d deg: closest walkable %.3f m from the wall\n", deg, closest);
        worst = std::min(worst, closest);
    }
    std::printf("worst %.3f m\n", worst);
    return worst < 0 ? 2 : 0;
}
