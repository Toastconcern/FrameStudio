// The world's non-mesh colliders (boxes, spheres, capsules): name, role, size and centre.
#include "hsl_scene.h"
#include <cmath>
#include <cstdio>
int main(int argc, char** argv) {
    HslScene s; std::string err;
    if (!loadHslScene(std::string(argv[1]), s, err)) return 1;
    for (const WorldCollider& c : s.world.colliders) {
        if (c.kind == WorldCollider::Mesh) continue;
        auto len = [&](int a) { return std::sqrt(c.world[a*4]*c.world[a*4] + c.world[a*4+1]*c.world[a*4+1] + c.world[a*4+2]*c.world[a*4+2]); };
        std::printf("  %-36s kind %d role %d  size %.2f x %.2f x %.2f  at %.2f, %.2f, %.2f\n", c.name.c_str(), c.kind, c.role,
                    2 * c.half[0] * len(0), 2 * c.half[1] * len(1), 2 * c.half[2] * len(2), c.world[12], c.world[13], c.world[14]);
    }
}
