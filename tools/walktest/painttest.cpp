// Painted floor (HomeWorld::walkPaint), checked on a made-up home: a 20 x 20 m ground with a bush (cut-out, 1.5 m
// tall), a patch of grass (cut-out, 0.5 m) and a box (solid, 0.8 m) on it.
//   - the bush and the box block; painted floor over the bush goes through it, and a dab that touched the bush's top
//     is on the ground under it; the box still blocks under paint (only see-through plants are walked through)
//   - the shapes: a square, a rectangle and a polygon paint just what's inside them, and erase the same way
//   - world.json keeps every shape, in order (and a world with round dabs only is written as before)
// Exit 1 on any failure.
#include "core/world.h"
#include "scene_bake.h"
#include "walkgrid.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int fails = 0;
void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  - ", detail.c_str());
    if (!ok) ++fails;
}

struct Mesh {
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    void quad(const float* a, const float* b, const float* c, const float* d) {
        const uint32_t base = static_cast<uint32_t>(pos.size() / 3);
        for (const float* p : {a, b, c, d}) pos.insert(pos.end(), p, p + 3);
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) idx.push_back(base + i);
    }
    // a box's six faces
    void box(float x0, float y0, float z0, float x1, float y1, float z1) {
        const float v[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
        quad(v[0], v[1], v[2], v[3]); quad(v[4], v[5], v[6], v[7]);
        quad(v[0], v[1], v[5], v[4]); quad(v[1], v[2], v[6], v[5]);
        quad(v[2], v[3], v[7], v[6]); quad(v[3], v[0], v[4], v[7]);
    }
    WalkSource source(int tag, bool cutout, const char* name) const {
        WalkSource s;
        s.pos = pos.data(); s.verts = pos.size() / 3; s.idx = idx.data(); s.indices = idx.size();
        s.cutout = cutout; s.tag = tag; s.name = name;
        return s;
    }
};

struct Home {
    Mesh ground, bush, grass, box, fence, shrub;
    std::vector<WalkSource> sources;
    Home() {
        const float g[4][3] = {{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}};
        ground.quad(g[0], g[1], g[2], g[3]);
        bush.box(2, 0, -1, 4, 1.5f, 1);       // 2 x 2 m, 1.5 m tall
        for (float t = -0.9f; t <= 0.91f; t += 0.3f) {   // 2 x 2 m of grass, 0.5 m: crossed upright cards
            const float a[3] = {-4, 0, t}, b[3] = {-2, 0, t}, c[3] = {-2, 0.5f, t}, d[3] = {-4, 0.5f, t};
            grass.quad(a, b, c, d);
            const float e[3] = {-3 + t, 0, -1}, f[3] = {-3 + t, 0, 1}, gg[3] = {-3 + t, 0.5f, 1}, hh[3] = {-3 + t, 0.5f, -1};
            grass.quad(e, f, gg, hh);
        }
        box.box(-1, 0, 4, 1, 0.8f, 6);        // a crate
        // a see-through (cut-out) garden fence, 0.9 m: an upright card; and a cut-out shrub card the same shape
        const float f0[3] = {-9, 0, -2.5f}, f1[3] = {-5, 0, -2.5f}, f2[3] = {-5, 0.9f, -2.5f}, f3[3] = {-9, 0.9f, -2.5f};
        fence.quad(f0, f1, f2, f3);
        const float s0[3] = {-9, 0, -3.5f}, s1[3] = {-5, 0, -3.5f}, s2[3] = {-5, 0.9f, -3.5f}, s3[3] = {-9, 0.9f, -3.5f};
        shrub.quad(s0, s1, s2, s3);
        sources = {ground.source(0, false, "Ground"), bush.source(1, true, "Bush_01"), grass.source(2, true, "Grass_01"), box.source(3, false, "Crate"),
                   fence.source(4, true, "SM_Garden_Fence_A"), shrub.source(5, true, "SM_Shrub_Low")};
    }
    std::shared_ptr<WalkGrid> bake(const std::vector<WalkStroke>& paint) const {
        return buildWalkGrid(sources, {{{0, 0, 0}}}, WalkParams{}, nullptr, nullptr, &paint);
    }
};

bool walkableAt(const WalkGrid& g, float x, float z, float y) {
    const bool w = g.walkable(x, z, y);
    if (std::getenv("PAINTDBG")) {
        int cx, cz;
        std::printf("    at (%.2f %.2f %.2f): %s;", x, y, z, w ? "walkable" : "not");
        if (g.cellOf(x, z, cx, cz)) for (const WalkSpan* q = g.begin(cx, cz); q != g.end(cx, cz); ++q) std::printf(" [%.2f..%.2f f%02x c%d]", q->lo, q->hi, q->flags, q->cut);
        std::printf("\n");
    }
    return w;
}

WalkStroke dab(float x, float y, float z, float r, int shape = WalkStroke::Round) {
    WalkStroke k;
    k.pos[0] = x; k.pos[1] = y; k.pos[2] = z; k.radius = r; k.shape = shape;
    return k;
}

}  // namespace

int main() {
    const Home h;
    {
        const auto g = h.bake({});
        check(g && walkableAt(*g, 0, 0, 0), "the ground is walkable");
        check(g && !walkableAt(*g, 3, 0, 0), "a 1.5 m bush blocks");
        check(g && walkableAt(*g, -3, 0, 0), "0.5 m grass is walked through");
        check(g && !walkableAt(*g, 0, 5, 0), "a crate blocks");
        check(g && !walkableAt(*g, -7, -2.5f, 0), "a low see-through fence blocks (built, not grown)");
        check(g && walkableAt(*g, -7, -3.5f, 0), "...a see-through shrub as low is walked through");
    }
    {   // names: whole words, so a street isn't a tree; a stone table is still furniture
        check(walkNameIsStructure("SM_Garden_Fence_A") && walkNameIsStructure("stairRailing") && !walkNameIsStructure("SM_Shrub_Low"),
              "fences and railings are structures, shrubs aren't");
        check(walkNameHint("Stone_Table_01") == kHintFurniture && walkNameHint("Large_Rock_Shelf") == kHintNature,
              "a stone table is furniture; a rock's shelf is rock");
    }
    {   // painted on the ground over the bush (as Studio's brush does now: through the plant)
        const auto g = h.bake({dab(3, 0, 0, 1.4f)});
        check(g && walkableAt(*g, 3, 0, 0) && walkableAt(*g, 2.3f, 0.6f, 0), "floor painted over a bush goes through it");
        check(g && !walkableAt(*g, 3, 0, 1.5f), "...no floor on the bush's top");
    }
    {   // a dab that touched the bush's top (painted before the brush went through plants)
        const auto g = h.bake({dab(3, 1.5f, 0, 1.4f)});
        check(g && walkableAt(*g, 3, 0, 0), "a dab on a bush's top paints the ground under it");
        check(g && !walkableAt(*g, 3, 0, 1.5f), "...and no floor in the air at the bush's top");
    }
    {   // a dab on the grass's top (0.5 m over the ground)
        const auto g = h.bake({dab(-3, 0.5f, 0, 0.8f)});
        check(g && walkableAt(*g, -3, 0, 0) && !walkableAt(*g, -3, 0, 0.5f), "a dab on grass paints the ground under it, nothing in the air");
    }
    {   // a crate isn't a plant: paint doesn't go through it
        const auto g = h.bake({dab(0, 0, 5, 1.4f)});
        check(g && !walkableAt(*g, 0, 5, 0), "painted floor doesn't go through a crate");
    }
    {   // painting off an edge into the air still makes floor there (no plant: nothing moved down)
        const auto g = h.bake({dab(0, 0, -9.8f, 0.3f), dab(0, 0, -10.6f, 0.3f), dab(0, 0, -11.2f, 0.3f)});
        check(g && walkableAt(*g, 0, -11.2f, 0), "floor painted on past an edge is still floor (over nothing)");
    }
    // ── shapes: erase a shape out of the ground, then check inside / outside
    {
        WalkStroke sq = dab(5, 0, 5, 1.0f, WalkStroke::Square);
        sq.erase = true;
        const auto g = h.bake({sq});
        check(g && !walkableAt(*g, 5.8f, 5.8f, 0) && walkableAt(*g, 6.6f, 5, 0), "a square erases its corners (a round one wouldn't)");
    }
    {
        WalkStroke r;
        r.shape = WalkStroke::Rect; r.erase = true;
        r.pos[0] = -8; r.pos[1] = 0; r.pos[2] = -8;
        r.centre[0] = -6; r.centre[1] = -7; r.half[0] = 2; r.half[1] = 1;
        const auto g = h.bake({r});
        check(g && !walkableAt(*g, -6, -7, 0) && !walkableAt(*g, -7.7f, -7.5f, 0) && walkableAt(*g, -6, -5.5f, 0) && walkableAt(*g, -3.5f, -7, 0),
              "a rectangle erases just inside it");
    }
    {
        WalkStroke r;   // turned 45 degrees: a diamond
        r.shape = WalkStroke::Rect; r.erase = true;
        r.pos[0] = 6; r.pos[1] = 0; r.pos[2] = -6;
        r.centre[0] = 6; r.centre[1] = -6; r.half[0] = 1.5f; r.half[1] = 1.5f; r.yaw = 3.14159265f / 4;
        const auto g = h.bake({r});
        check(g && !walkableAt(*g, 6, -6, 0) && !walkableAt(*g, 7.6f, -6, 0) && walkableAt(*g, 7.2f, -4.8f, 0), "a turned rectangle erases its own shape");
    }
    {
        WalkStroke p;   // an L
        p.shape = WalkStroke::Polygon; p.erase = true;
        p.pos[0] = -9; p.pos[1] = 0; p.pos[2] = 3;
        p.outline = {-9, 3, -5, 3, -5, 5, -7, 5, -7, 9, -9, 9};
        const auto g = h.bake({p});
        check(g && !walkableAt(*g, -6, 4, 0) && !walkableAt(*g, -8, 8, 0) && walkableAt(*g, -6, 7.5f, 0), "a polygon erases just inside it (an L, its notch left)");
    }
    {   // painted back: a polygon of floor over the erased L's notch-side, out past the ground's edge
        WalkStroke p;
        p.shape = WalkStroke::Polygon;
        p.pos[0] = 9; p.pos[1] = 0; p.pos[2] = 9;
        p.outline = {9, 9, 12, 9, 12, 12, 9, 12};
        const auto g = h.bake({p});
        check(g && walkableAt(*g, 11, 11, 0), "a polygon paints floor out over nothing, at its level");
    }
    // ── kept: world.json and the baked scene's world
    {
        HomeWorld w;
        w.walkPaint.push_back(dab(1, 0, 1, 0.3f));
        WalkStroke sq = dab(2, 0, 2, 0.5f, WalkStroke::Square); sq.yaw = 0.3f; w.walkPaint.push_back(sq);
        WalkStroke r; r.shape = WalkStroke::Rect; r.pos[1] = 0.2f; r.centre[0] = 3; r.centre[1] = 4; r.half[0] = 1; r.half[1] = 2; r.yaw = 0.5f; r.erase = true; w.walkPaint.push_back(r);
        WalkStroke p; p.shape = WalkStroke::Polygon; p.outline = {0, 0, 1, 0, 1, 1}; w.walkPaint.push_back(p);
        HomeWorld back;
        const std::string j = worldToJson(w);
        const bool read = worldFromJson(j, back);
        bool same = read && back.walkPaint.size() == 4;
        for (size_t i = 0; same && i < 4; ++i) {
            const WalkStroke &a = w.walkPaint[i], &b = back.walkPaint[i];
            same = a.shape == b.shape && a.erase == b.erase && std::fabs(a.pos[1] - b.pos[1]) < 1e-4f && std::fabs(a.yaw - b.yaw) < 1e-4f &&
                   std::fabs(a.centre[0] - b.centre[0]) < 1e-4f && std::fabs(a.half[1] - b.half[1]) < 1e-4f && a.outline == b.outline &&
                   (a.shape == WalkStroke::Rect || a.shape == WalkStroke::Polygon || std::fabs(a.radius - b.radius) < 1e-4f);
        }
        check(same, "world.json keeps every shape, in order");
        // a reader from before shapes: the round dab only (the legacy list)
        const size_t legacy = j.find("\"walkPaint\":[");
        check(legacy != std::string::npos && j.find("],", legacy) - legacy < 40, "...and the old list has the round dab alone", j.substr(legacy, 60));
        HomeWorld round;
        round.walkPaint.push_back(dab(1, 0, 1, 0.3f));
        check(worldToJson(round).find("walkStrokes") == std::string::npos, "a world with round dabs only is written as before");
    }
    {   // "Use Meta's floor" off, and the home's own colliders, are kept through world.json
        HomeWorld w;
        WorldCollider c;
        c.home = true;
        w.colliders.push_back(c);
        w.useHomeCollision = false;
        HomeWorld back;
        check(worldFromJson(worldToJson(w), back) && !back.useHomeCollision && back.colliders.size() == 1 && back.colliders[0].home && !back.hasHomeCollision(),
              "Meta's floor removed stays removed when saved, and its colliders stay the home's");
        HomeWorld on;
        on.colliders.push_back(c);
        check(worldFromJson(worldToJson(on), back) && back.useHomeCollision && back.hasHomeCollision(), "...and used stays used");
    }
    std::printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
