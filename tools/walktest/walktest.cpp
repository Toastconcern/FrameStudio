// Bakes each home's walkable area and checks the spawns/seats/hotspots stand on it.
#include "hsl_scene.h"
#include <cstdio>
#include <cmath>
#include <cstring>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        HslScene s; std::string err;
        if (!loadHslScene(argv[i], s, err)) { std::printf("LOAD FAIL %s: %s\n", argv[i], err.c_str()); ++bad; continue; }
        std::printf("== %s  (origin %s, %zu spawns %zu seats %zu hotspots %zu colliders)\n", argv[i], s.world.origin.c_str(),
                    s.world.spawns.size(), s.world.seats.size(), s.world.hotspots.size(), s.world.colliders.size());
        HomeWorld w = walkWorld(s);
        const WalkGrid* g = w.walkGridCache.get();
        if (!g) { std::printf("  NO GRID\n"); ++bad; continue; }
        auto chk = [&](const char* kind, const std::vector<WorldPoint>& pts) {
            for (const WorldPoint& p : pts) {
                float h = 0; bool ok = g->walkable(p.pos[0], p.pos[2], p.pos[1], &h);
                float f = 0; bool fl = g->floorAt(p.pos[0], p.pos[2], p.pos[1], 0.5f, 2.0f, f);
                std::printf("  %-7s %-28s (%.2f %.2f %.2f) walkable=%d floor=%s\n", kind, p.name.c_str(), p.pos[0], p.pos[1], p.pos[2], ok,
                            fl ? std::to_string(f).c_str() : "none");
                if (!strcmp(kind, "spawn") && !ok) ++bad;
            }
        };
        chk("spawn", w.spawns); chk("hotspot", w.hotspots); chk("seat", w.seats);
        for (const WorldPoint& p : w.seats) {   // a standing spot near each seat (roomAt from the seat at floor height)
            float x = p.pos[0], z = p.pos[2]; bool ok = g->roomAt(x, p.pos[1] - 0.45f, z, 1.2f);
            std::printf("  standby %-28s ok=%d at (%.2f, %.2f) %.2f m away\n", p.name.c_str(), ok, x, z, std::hypot(x - p.pos[0], z - p.pos[2]));
        }
        for (const WorldCollider& c : s.world.colliders) if (strstr(c.name.c_str(), "hair") || strstr(c.name.c_str(), "Couch") || strstr(c.name.c_str(), "Stool")) {
            float sx = std::sqrt(c.world[0]*c.world[0]+c.world[1]*c.world[1]+c.world[2]*c.world[2]);
            float sy = std::sqrt(c.world[4]*c.world[4]+c.world[5]*c.world[5]+c.world[6]*c.world[6]);
            std::printf("  collider %-50s kind=%d role=%d r=%.3f len=%.3f half=(%.2f %.2f %.2f) scale=(%.3f %.3f) at (%.2f %.2f %.2f) tris=%zu\n", c.name.c_str(), c.kind, c.role, c.radius, c.length,
                c.half[0], c.half[1], c.half[2], sx, sy, c.world[12], c.world[13], c.world[14], c.indices.size()/3);
        }
    }
    return bad;
}
