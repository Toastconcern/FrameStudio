// Walked through means not there: an object set to Walk through (Studio's Trigger surface linked to it, "<mesh>:<name>")
// gives exactly the walkable area it gives deleted, in a home walked on its meshes and in one walked on Meta's colliders
// (deleted there: its mesh and Meta's collision of it). Each object is checked twice: as it is, and with a tall box
// nothing's drawn on standing against its side (invisibleWalls: a walked-through object is no wall drawn on the box).
// Then Meta's trigger bodies: a trigger box at the start, Meta's and one placed in Studio, gives the area without it.
// The objects: the ones that stop you most at the area's edge (WalkReport::blockers) and the floor under the start.
// walkthrough <apk> [objects]   NOMETA=1: Meta's floor off. Exit 2 when any pair differs.
#include "hsl_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

// The area as cells: (x, z, floor height) in cm -> its distance to the edge (cm), every walkable or edge floor
using Cells = std::map<std::tuple<long, long, long>, int>;
Cells cellsOf(const WalkGrid& g) {
    Cells c;
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
            for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
                if (s->flags & (kWalkOk | kWalkNear))
                    c[{std::lround(g.centerX(x) * 100), std::lround(g.centerZ(z) * 100), std::lround(s->hi * 100)}] = s->dist;
    return c;
}

// How many cells the two differ in (walkable in one only, or at another distance to the edge)
int differ(const Cells& a, const Cells& b) {
    int n = 0;
    for (const auto& [k, d] : a) { const auto it = b.find(k); n += it == b.end() || it->second != d; }
    for (const auto& [k, d] : b) n += a.find(k) == a.end();
    return n;
}

void frameOf(const MeshData& md, float* f) {
    if (md.hasWorldMatrix) std::memcpy(f, md.worldMatrix, sizeof(float) * 16); else trsToMat4(md.transform, f);
}

// A made-up drawn box (a cupboard, a table): 12 triangles, its foot's middle at (x, y, z)
MeshData boxMesh(const char* name, float hx, float hy, float hz, float x, float y, float z) {
    MeshData md;
    md.name = name;
    for (int k = 0; k < 8; ++k) md.positions.insert(md.positions.end(), {(k & 1 ? 1 : -1) * hx, k & 2 ? 2 * hy : 0.0f, (k & 4 ? 1 : -1) * hz});
    static const u32 f[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    md.indices.assign(f, f + 36);
    md.nVerts = 8;
    md.nIdx = 36;
    md.hasWorldMatrix = true;
    md.worldMatrix[12] = x; md.worldMatrix[13] = y; md.worldMatrix[14] = z;
    return md;
}

// Studio's Walk through on mesh m: a Trigger surface linked to it, its own triangles in its frame
WorldCollider walkThroughOf(const MeshData& md, int m) {
    WorldCollider t;
    t.name = md.name;
    t.kind = WorldCollider::Mesh;
    t.role = WorldCollider::Trigger;
    t.mesh = std::to_string(m) + ":" + md.name;
    frameOf(md, t.world);
    t.positions = md.positions;
    t.indices.assign(md.indices.begin(), md.indices.end());
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "walkthrough <apk> [objects]\n"); return 1; }
    const int want = argc > 2 ? std::max(1, std::atoi(argv[2])) : 6;
    HslScene s;
    std::string err;
    if (!loadHslScene(std::string(argv[1]), s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    if (std::getenv("NOMETA")) s.world.useHomeCollision = false;
    const bool meta = s.world.hasHomeCollision();
    const size_t nm = s.meshes.size();
    WalkReport rep;
    const auto g0 = bakeWalkGrid(s, s.world, WalkParams{}, &rep);
    std::printf("%s: %.1f m2, walked on %s\n", argv[1], rep.walkArea, meta ? "Meta's colliders" : "its meshes");
    // the start: the first spawn, else the spot of the area with 2 m of room round it nearest its middle
    float start[3] = {0, 0, 0};
    bool haveStart = !s.world.spawns.empty();
    if (haveStart) std::copy_n(s.world.spawns[0].pos, 3, start);
    else {
        float best = 1e30f;
        const float mx = g0->ox + g0->nx * g0->p.cell * 0.5f, mz = g0->oz + g0->nz * g0->p.cell * 0.5f;
        for (int z = 0; z < g0->nz; ++z)
            for (int x = 0; x < g0->nx; ++x)
                for (const WalkSpan* q = g0->begin(x, z); q != g0->end(x, z); ++q)
                    if ((q->flags & kWalkOk) && q->dist >= 200 && std::hypot(g0->centerX(x) - mx, g0->centerZ(z) - mz) < best) {
                        best = std::hypot(g0->centerX(x) - mx, g0->centerZ(z) - mz);
                        start[0] = g0->centerX(x); start[1] = q->hi; start[2] = g0->centerZ(z);
                        haveStart = true;
                    }
    }

    // the drawn mesh a blocker is (walked on Meta's colliders: the one a collider is the collision of)
    auto meshOf = [&](int tag) -> int {
        if (tag < 0) return -1;
        if (static_cast<size_t>(tag) < nm) return tag;
        const size_t ci = static_cast<size_t>(tag) - nm;
        if (ci >= s.world.colliders.size()) return -1;
        for (size_t m = 0; m < nm; ++m) {
            float f[16];
            frameOf(s.meshes[m], f);
            if (walkColliderOfMesh(s.world.colliders[ci], f, s.meshes[m].positions)) return static_cast<int>(m);
        }
        return -1;
    };
    std::vector<int> objects;
    for (const auto& [tag, n] : rep.blockers) {
        const int m = meshOf(tag);
        if (m >= 0 && std::find(objects.begin(), objects.end(), m) == objects.end()) objects.push_back(m);
        if (static_cast<int>(objects.size()) >= want) break;
    }
    if (haveStart) {                                                   // the floor under the start
        const float* p = start;
        for (const WalkSource& src : drawnWalkSources(s, s.world)) {
            bool under = false;
            for (size_t t = 0; t + 2 < src.indices && !under; t += 3) {
                float v[3][3];
                for (int k = 0; k < 3; ++k) walkgrid_detail::xform(src.world, src.pos + src.idx[t + k] * 3, v[k]);
                const float d = (v[1][2] - v[2][2]) * (v[0][0] - v[2][0]) + (v[2][0] - v[1][0]) * (v[0][2] - v[2][2]);
                if (std::fabs(d) < 1e-12f) continue;
                const float u = ((v[1][2] - v[2][2]) * (p[0] - v[2][0]) + (v[2][0] - v[1][0]) * (p[2] - v[2][2])) / d;
                const float w = ((v[2][2] - v[0][2]) * (p[0] - v[2][0]) + (v[0][0] - v[2][0]) * (p[2] - v[2][2])) / d;
                if (u < 0 || w < 0 || u + w > 1) continue;
                under = std::fabs(u * v[0][1] + w * v[1][1] + (1 - u - w) * v[2][1] - p[1]) < 0.3f;
            }
            if (under && src.tag >= 0 && std::find(objects.begin(), objects.end(), src.tag) == objects.end()) { objects.push_back(src.tag); break; }
        }
    }

    int bad = 0;
    auto check = [&](const char* what, const HomeWorld& a, const std::vector<char>* removedA, const HomeWorld& b, const std::vector<char>* removedB) {
        WalkReport ra, rb;
        const auto ga = bakeWalkGrid(s, a, WalkParams{}, &ra, nullptr, removedA);
        const auto gb = bakeWalkGrid(s, b, WalkParams{}, &rb, nullptr, removedB);
        const int n = differ(cellsOf(*ga), cellsOf(*gb));
        bad += n > 0;
        std::printf("  %s %-58s walk through %7.2f m2, deleted %7.2f m2, %d cells differ\n", n ? "FAIL" : "ok  ", what, ra.walkArea, rb.walkArea, n);
    };
    for (int m : objects) {
        const MeshData& md = s.meshes[m];
        float f[16];
        frameOf(md, f);
        HomeWorld through = s.world;
        through.colliders.push_back(walkThroughOf(md, m));
        // deleted: its mesh gone, and Meta's collision of it
        HomeWorld gone = s.world;
        if (meta)
            gone.colliders.erase(std::remove_if(gone.colliders.begin(), gone.colliders.end(), [&](const WorldCollider& c) {
                return c.home && walkColliderOfMesh(c, f, md.positions);
            }), gone.colliders.end());
        std::vector<char> removed(nm, 0);
        removed[m] = 1;
        const std::string name = std::to_string(m) + " " + md.name.substr(0, 40);
        check(name.c_str(), through, nullptr, gone, &removed);
        // a tall box nothing's drawn on, against its +x side (the side's middle, from its foot up 2 m)
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v + 2 < md.positions.size(); v += 3) {
            float w[3];
            walkgrid_detail::xform(f, &md.positions[v], w);
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], w[k]); hi[k] = std::max(hi[k], w[k]); }
        }
        WorldCollider box;
        box.name = "test wall";
        box.kind = WorldCollider::Box;
        box.role = WorldCollider::Solid;
        box.half[0] = 0.2f; box.half[1] = 1.0f; box.half[2] = std::clamp((hi[2] - lo[2]) * 0.5f, 0.3f, 3.0f);
        box.world[12] = hi[0] + 0.2f; box.world[13] = lo[1] + 1.0f; box.world[14] = (lo[2] + hi[2]) * 0.5f;
        through.colliders.push_back(box);
        gone.colliders.push_back(box);
        check((name + " + a box at its side").c_str(), through, nullptr, gone, &removed);
    }
    // Made-up objects by the start, so each way an object could still count is tried whatever the home has there:
    //  a cupboard (drawn, 1 x 2 x 1 m) round a tall box collider nothing else is drawn on: invisibleWalls. Set to walk
    //  through, the cupboard is no wall drawn on the box, which is then walked past as it is with the cupboard deleted.
    //  a table (drawn, 1 x 0.75 x 1 m) with Meta's collision of it (one of the home's mesh colliders, its own
    //  triangles): walked on Meta's colliders, setting the table to walk through leaves that out as deleting both does.
    if (haveStart) {
        const float* p = start;
        const int cup = static_cast<int>(s.meshes.size()), table = cup + 1;
        s.meshes.push_back(boxMesh("test cupboard", 0.5f, 1.0f, 0.5f, p[0] + 1.3f, p[1], p[2]));
        s.meshes.push_back(boxMesh("test table", 0.5f, 0.375f, 0.5f, p[0] - 1.3f, p[1], p[2]));
        const size_t n = s.meshes.size();
        WorldCollider box;
        box.name = "test wall";
        box.kind = WorldCollider::Box;
        box.half[0] = box.half[2] = 0.45f; box.half[1] = 1.0f;
        box.world[12] = p[0] + 1.3f; box.world[13] = p[1] + 1.0f; box.world[14] = p[2];
        WorldCollider copy = walkThroughOf(s.meshes[table], table);
        copy.name = "test table (Meta's collision)";
        copy.role = WorldCollider::Solid;
        copy.mesh.clear();
        copy.home = true;
        auto without = [&](std::initializer_list<int> ms) { std::vector<char> r(n, 0); for (int m : ms) r[m] = 1; return r; };
        {
            HomeWorld a = s.world, b = s.world;
            a.colliders.push_back(box);
            b.colliders.push_back(box);
            a.colliders.push_back(walkThroughOf(s.meshes[cup], cup));
            const std::vector<char> ra = without({table}), rb = without({cup, table});
            WalkReport r0;
            const int counts = differ(cellsOf(*bakeWalkGrid(s, b, WalkParams{}, &r0, nullptr, &ra)), cellsOf(*bakeWalkGrid(s, b, WalkParams{}, &r0, nullptr, &rb)));
            std::printf("  (the cupboard round the box, drawn: %d cells differ from it deleted)\n", counts);
            check("a cupboard round a tall box nothing else is drawn on", a, &ra, b, &rb);
        }
        if (meta || !s.world.useHomeCollision) {
            HomeWorld a = s.world, b = s.world;
            a.colliders.push_back(copy);
            a.colliders.push_back(walkThroughOf(s.meshes[table], table));
            const std::vector<char> ra = without({cup}), rb = without({cup, table});
            HomeWorld solid = s.world;
            solid.colliders.push_back(copy);
            WalkReport r0;
            const int counts = differ(cellsOf(*bakeWalkGrid(s, solid, WalkParams{}, &r0, nullptr, &ra)), cellsOf(*bakeWalkGrid(s, b, WalkParams{}, &r0, nullptr, &rb)));
            std::printf("  (the table with Meta's collision of it: %d cells differ from both deleted)\n", counts);
            check("a table with Meta's collision of it", a, &ra, b, &rb);
        }
        s.meshes.resize(cup);
    }
    // Meta's trigger bodies, and one placed in Studio: a 1 x 2 x 1 m trigger box a metre from the start
    if (haveStart)
        for (bool home : {true, false}) {
            HomeWorld with = s.world;
            WorldCollider tb;
            tb.name = home ? "Meta trigger" : "Studio trigger";
            tb.kind = WorldCollider::Box;
            tb.role = WorldCollider::Trigger;
            tb.home = home;
            tb.half[0] = tb.half[2] = 0.5f; tb.half[1] = 1.0f;
            tb.world[12] = start[0] + 1.0f; tb.world[13] = start[1] + 1.0f; tb.world[14] = start[2];
            with.colliders.push_back(tb);
            check(home ? "a trigger box of Meta's by the start" : "a trigger box placed by the start", with, nullptr, s.world, nullptr);
        }
    std::printf(bad ? "FAIL: %d walked-through object(s) shape the area\n" : "ok: what's walked through doesn't shape the area\n", bad);
    return bad ? 2 : 0;
}
