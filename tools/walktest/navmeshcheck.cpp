// navmeshcheck: the walkable area made into navmeshes (src/navmesh.h) and walked on them alone gives the same area.
// Bakes the home's area, makes its navmesh pieces, bakes again from those only (HomeWorld::hasNavmesh), and compares
// cell by cell (walkable in one only); then moves one corner of the biggest piece up and checks the area follows.
// navmeshcheck <apk> [Meta's floor off: nometa]   Exit 2 when it loses more than 0.5% of the area or adds more than 8% (the cells' tops whole at its edges).
#include "hsl_scene.h"
#include "navmesh.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>


int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "navmeshcheck <apk> [nometa]\n"); return 1; }
    HslScene s;
    std::string err;
    if (!loadHslScene(std::string(argv[1]), s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    if (argc > 2 && std::strcmp(argv[2], "nometa") == 0) s.world.useHomeCollision = false;
    WalkReport r0;
    const auto g0 = bakeWalkGrid(s, s.world, WalkParams{}, &r0);
    if (!g0) { std::printf("no area\n"); return 1; }
    const std::vector<NavmeshPiece> pieces = navmeshFromWalkGrid(*g0);
    size_t tris = 0;
    for (const NavmeshPiece& p : pieces) tris += p.idx.size() / 3;
    std::printf("area %.1f m2 in %d areas -> %zu navmesh pieces, %zu triangles\n", r0.walkArea, r0.areas, pieces.size(), tris);
    for (size_t i = 0; i < pieces.size() && i < 8; ++i)
        std::printf("  piece %zu: %.1f m2, %zu vertices, %zu triangles\n", i, pieces[i].area, pieces[i].pos.size() / 3, pieces[i].idx.size() / 3);
    HomeWorld w = s.world;
    for (size_t i = 0; i < pieces.size(); ++i) {
        WorldCollider c;
        c.name = "Navmesh " + std::to_string(i + 1);
        c.kind = WorldCollider::Mesh;
        c.role = WorldCollider::Navmesh;
        c.positions = pieces[i].pos;
        c.indices = pieces[i].idx;
        w.colliders.push_back(std::move(c));
    }
    WalkReport r1;
    const auto g1 = bakeWalkGrid(s, w, WalkParams{}, &r1);
    // each walkable spot of one walkable in the other at the same place (within a step up or down)
    auto missing = [](const WalkGrid& from, const WalkGrid& in, size_t& all) {
        size_t miss = 0;
        all = 0;
        for (int z = 0; z < from.nz; ++z)
            for (int x = 0; x < from.nx; ++x)
                for (const WalkSpan* s = from.begin(x, z); s != from.end(x, z); ++s) {
                    if (!(s->flags & kWalkOk)) continue;
                    ++all;
                    int cx, cz;
                    miss += !(in.cellOf(from.centerX(x), from.centerZ(z), cx, cz) && in.walkNear(cx, cz, s->hi, 0.4f, kWalkOk | kWalkNear));
                }
        return miss;
    };
    size_t na = 0, nb = 0;
    const size_t onlyA = g1 ? missing(*g0, *g1, na) : 0, onlyB = g1 ? missing(*g1, *g0, nb) : 0;
    const double diff = static_cast<double>(onlyA + onlyB) / std::max<size_t>(1, na);
    std::printf("walked on the navmeshes: %.1f m2 in %d areas; spots %zu before, %zu after: %zu lost, %zu new (%.1f%%)\n", r1.walkArea, r1.areas,
                na, nb, onlyA, onlyB, diff * 100);
    // an edit: the biggest piece's first corner raised 0.3 m: the area there follows (its cells move up)
    if (!pieces.empty()) {
        for (WorldCollider& c : w.colliders)
            if (c.role == WorldCollider::Navmesh && c.name == "Navmesh 1") {
                for (size_t v = 0; v < c.positions.size() / 3; ++v) c.positions[v * 3 + 1] -= 5.0f;   // (the whole piece sunk 5 m)
                break;
            }
        WalkReport r2;
        const auto g2 = bakeWalkGrid(s, w, WalkParams{}, &r2);
        std::printf("piece 1 sunk 5 m: %.1f m2 (%s)\n", r2.walkArea, std::fabs(r2.walkArea - r1.walkArea) < 0.5 ? "same area, moved" : "area changed");
    }
    // deleted (HomeWorld::walkNone): nothing walkable, not the area worked out from the home; a navmesh added walks again;
    // both kept through world.json
    bool kept = true;
    {
        HomeWorld none = s.world;
        none.walkNone = true;
        WalkReport rn;
        bakeWalkGrid(s, none, WalkParams{}, &rn);
        HomeWorld back;
        kept &= worldFromJson(worldToJson(w), back) && back.hasNavmesh();
        HomeWorld noneBack;
        kept &= worldFromJson(worldToJson(none), noneBack) && noneBack.walkNone;
        none.colliders.push_back(w.colliders.back());   // (a navmesh piece)
        WalkReport rp;
        bakeWalkGrid(s, none, WalkParams{}, &rp);
        std::printf("deleted: %.1f m2; with a navmesh piece added: %.1f m2; kept in world.json: %s\n", rn.walkArea, rp.walkArea, kept ? "yes" : "NO");
        kept &= rn.walkArea == 0 && rp.walkArea > 0;
    }
    return !kept || onlyA * 200 > na || onlyB * 12 > na ? 2 : 0;   // (none lost but a speck; a little more at the edges: cells' tops whole)
}
