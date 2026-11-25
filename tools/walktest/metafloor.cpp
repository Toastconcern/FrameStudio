// Meta's floor against the walkable area, cell by cell: a home walked on Meta's own collision ("Use Meta's floor" on)
// must walk exactly where Meta's floor is. Meta's floor here is worked out from the home's own colliders alone
// (WorldCollider::home), apart from the bake: at each 10 cm cell's middle, the tops of Meta's Solid colliders that
// face up within the bake's steepest floor, with nothing of Meta's inside them, in the way over them (headroom) or
// standing in them (its walls: Object), joined a step or less apart and reached from the start (the spawns, hotspots
// and seats' exits). Disconnected pieces of it (a table's top, a ledge, floor past a wall) are listed apart.
// Each of its cells is then looked up in the bake (field() >= 0 at the cell's middle: what's walked):
//   missing  Meta's floor the bake doesn't walk, and why (left out as unreached or small, peeled as furniture, no
//            floor there for the bake, or a floor too near the edge)
//   extra    walked where Meta has no floor, and why (bridged, a top Meta doesn't reach, nothing of Meta's there)
// metafloor <apk[+vista]> [out.png]   CELLS=1 lists each differing cell; LIST=1 lists Meta's colliders
// The picture: green both, red Meta's floor the bake misses, blue walked where Meta has none, dark orange Meta's floor
// not reached from the start (dim: walked; bright: not), grey other tops of Meta's.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "hsl_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Hit { float y; int col; bool flat, up; };     // a collider's triangle crossing a cell's vertical line

struct Collider {
    const WorldCollider* c;
    int index;                                       // in world.colliders
    bool convex;                                     // box / sphere / capsule: what's between its lowest and highest crossing is inside
    std::vector<float> pos;                          // world space
    std::vector<uint32_t> idx;
};

// What a Meta top at a cell is (for floor: Floor; otherwise why it isn't one)
enum Kind { kFloor = 0, kSteep, kBuried, kNoHeadroom, kInWall, kKinds };
const char* kKindName[kKinds] = {"floor", "too steep", "inside a collider", "no headroom", "in a wall (Object)"};

struct Top { float y; int col; Kind kind; int piece = -1; };

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "metafloor <apk[+vista]> [out.png]\n"); return 1; }
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s;
    std::string err;
    if (!loadHslScene(apks, s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    if (std::getenv("SEATS") && !s.world.spawns.empty()) {
        // Seats are never walked on, on Meta's floor too: two made-up benches 38 cm tall (a step up) by the start, one of
        // Meta's box colliders named as one, and one named nothing that's the collision of a drawn mesh named as one
        const float* p = s.world.spawns[0].pos;
        WorldCollider bench;
        const bool plain = std::strcmp(std::getenv("SEATS"), "plain") == 0;   // SEATS=plain: named as crates (walked on: a step up)
        bench.name = plain ? "crate" : "bench";
        bench.kind = WorldCollider::Box;
        bench.home = true;
        bench.half[0] = 0.6f; bench.half[1] = 0.19f; bench.half[2] = 0.25f;
        bench.world[12] = p[0] + 1.3f; bench.world[13] = p[1] + 0.19f; bench.world[14] = p[2];
        s.world.colliders.push_back(bench);
        MeshData md;
        md.name = plain ? "wooden_crate_01" : "wooden_bench_01";
        for (int k = 0; k < 8; ++k) md.positions.insert(md.positions.end(), {(k & 1 ? 0.6f : -0.6f), k & 2 ? 0.38f : 0.0f, (k & 4 ? 0.25f : -0.25f)});
        static const u32 f[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
        md.indices.assign(f, f + 36);
        md.nVerts = 8; md.nIdx = 36;
        md.hasWorldMatrix = true;
        md.worldMatrix[12] = p[0] - 1.3f; md.worldMatrix[13] = p[1]; md.worldMatrix[14] = p[2];
        WorldCollider copy;
        copy.name = "OSE_grp_17";
        copy.kind = WorldCollider::Mesh;
        copy.home = true;
        std::copy_n(md.worldMatrix, 16, copy.world);
        copy.positions = md.positions;
        copy.indices = md.indices;
        s.meshes.push_back(md);
        s.world.colliders.push_back(copy);
        WalkReport r;
        const auto g = bakeWalkGrid(s, s.world, WalkParams{}, &r);
        int bad = 0;
        for (float dx : {1.3f, -1.3f}) {
            const bool on = g->walkable(p[0] + dx, p[2], p[1] + 0.38f);
            bad += on;
            std::printf("  %s %s's top walkable: %s\n", on ? "FAIL" : "ok  ", dx > 0 ? "the box named bench" : "the collider of the drawn bench", on ? "yes" : "no");
        }
        std::printf("%s (area %.1f m2)\n", bad ? "FAIL: a seat on Meta's floor can be walked on" : "ok: no seat on Meta's floor can be walked on", r.walkArea);
        return bad ? 2 : 0;
    }
    const WalkParams P;
    const float cosSlope = std::cos(P.maxSlope * 3.14159265f / 180);
    // REF=<m>: Meta's floor worked out every REF metres (2 cm: the edge's own shape, against field()) over the bake's
    // grid only; else on the bake's cells, over all of Meta's colliders
    const char* ref = std::getenv("REF");
    const float cs = ref ? std::clamp(static_cast<float>(std::atof(ref)), 0.01f, P.cell) : P.cell;

    // The bake, as the home opens
    WalkReport rep;
    const auto g = bakeWalkGrid(s, s.world, WalkParams{}, &rep);

    // Meta's colliders, in world space
    std::vector<Collider> cols;
    for (size_t i = 0; i < s.world.colliders.size(); ++i) {
        const WorldCollider& c = s.world.colliders[i];
        if (!c.home || c.role == WorldCollider::Trigger) continue;
        Collider k{&c, static_cast<int>(i), c.kind != WorldCollider::Mesh, {}, {}};
        if (c.kind == WorldCollider::Mesh) {
            k.pos.resize(c.positions.size());
            for (size_t v = 0; v + 2 < c.positions.size(); v += 3) walkgrid_detail::xform(c.world, &c.positions[v], &k.pos[v]);
            k.idx.assign(c.indices.begin(), c.indices.end());
        } else {
            walkShapeMesh(c, k.pos, k.idx);
        }
        cols.push_back(std::move(k));
    }
    if (cols.empty()) { std::printf("no Meta colliders: nothing to compare\n"); return 0; }
    if (std::getenv("LIST"))
        for (const Collider& k : cols) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t v = 0; v + 2 < k.pos.size(); v += 3) for (int j = 0; j < 3; ++j) { lo[j] = std::min(lo[j], k.pos[v + j]); hi[j] = std::max(hi[j], k.pos[v + j]); }
            std::printf("collider %3d %-40s kind %d role %d tris %6zu  x %6.2f..%6.2f y %6.2f..%6.2f z %6.2f..%6.2f\n", k.index, k.c->name.substr(0, 40).c_str(),
                        k.c->kind, k.c->role, k.idx.size() / 3, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
        }

    // The cells: over Meta's colliders (within the bake's reach of the start), on the bake's lattice (multiples of cs)
    const std::vector<std::array<float, 3>> seeds = walkSeeds(s.world);
    const float anchor[3] = {seeds.empty() ? 0 : seeds[0][0], seeds.empty() ? 0 : seeds[0][1], seeds.empty() ? 0 : seeds[0][2]};
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (const Collider& k : cols)
        for (size_t v = 0; v + 2 < k.pos.size(); v += 3) {
            lo[0] = std::min(lo[0], k.pos[v]); hi[0] = std::max(hi[0], k.pos[v]);
            lo[1] = std::min(lo[1], k.pos[v + 2]); hi[1] = std::max(hi[1], k.pos[v + 2]);
        }
    lo[0] = std::max(lo[0], anchor[0] - P.reach); hi[0] = std::min(hi[0], anchor[0] + P.reach);
    lo[1] = std::max(lo[1], anchor[2] - P.reach); hi[1] = std::min(hi[1], anchor[2] + P.reach);
    if (ref && g && g->nx > 0) {
        lo[0] = std::max(lo[0], g->ox); hi[0] = std::min(hi[0], g->ox + g->nx * g->p.cell);
        lo[1] = std::max(lo[1], g->oz); hi[1] = std::min(hi[1], g->oz + g->nz * g->p.cell);
    }
    const int ix0 = static_cast<int>(std::floor(lo[0] / cs)) - 1, iz0 = static_cast<int>(std::floor(lo[1] / cs)) - 1;
    const int nx = static_cast<int>(std::floor(hi[0] / cs)) + 2 - ix0, nz = static_cast<int>(std::floor(hi[1] / cs)) + 2 - iz0;
    auto cx = [&](int i) { return (ix0 + i + 0.5f) * cs; };
    auto cz = [&](int j) { return (iz0 + j + 0.5f) * cs; };

    // Every collider triangle crossing each cell's middle
    std::vector<std::vector<Hit>> hits(static_cast<size_t>(nx) * nz);
    for (size_t ci = 0; ci < cols.size(); ++ci) {
        const Collider& k = cols[ci];
        float mid[3] = {0, 0, 0};                                      // (a convex shape: faces point away from its middle)
        for (size_t v = 0; v + 2 < k.pos.size(); v += 3) for (int j = 0; j < 3; ++j) mid[j] += k.pos[v + j];
        for (int j = 0; j < 3; ++j) mid[j] /= std::max<size_t>(1, k.pos.size() / 3);
        for (size_t t = 0; t + 2 < k.idx.size(); t += 3) {
            const size_t nv = k.pos.size() / 3;
            if (k.idx[t] >= nv || k.idx[t + 1] >= nv || k.idx[t + 2] >= nv) continue;
            const float* a = &k.pos[k.idx[t] * 3]; const float* b = &k.pos[k.idx[t + 1] * 3]; const float* c = &k.pos[k.idx[t + 2] * 3];
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (L < 1e-12f) continue;
            if (k.convex) {
                const float tc[3] = {(a[0] + b[0] + c[0]) / 3 - mid[0], (a[1] + b[1] + c[1]) / 3 - mid[1], (a[2] + b[2] + c[2]) / 3 - mid[2]};
                if (n[0] * tc[0] + n[1] * tc[1] + n[2] * tc[2] < 0) for (float& q : n) q = -q;
            }
            const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-12f) continue;                       // seen edge-on from above
            const int i0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]}) / cs - 0.5f)) - ix0);
            const int i1 = std::min(nx - 1, static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}) / cs - 0.5f)) - ix0);
            const int j0 = std::max(0, static_cast<int>(std::floor(std::min({a[2], b[2], c[2]}) / cs - 0.5f)) - iz0);
            const int j1 = std::min(nz - 1, static_cast<int>(std::ceil(std::max({a[2], b[2], c[2]}) / cs - 0.5f)) - iz0);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    const float x = cx(i), z = cz(j);
                    const float u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
                    const float w = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
                    if (u < -1e-5f || w < -1e-5f || u + w > 1 + 1e-5f) continue;
                    const float y = u * a[1] + w * b[1] + (1 - u - w) * c[1];
                    hits[static_cast<size_t>(j) * nx + i].push_back({y, static_cast<int>(ci), std::fabs(n[1]) / L >= cosSlope, n[1] > 0});
                }
        }
    }

    // Meta's tops at each cell, and what they are
    std::vector<std::vector<Top>> tops(hits.size());
    for (size_t cell = 0; cell < hits.size(); ++cell) {
        std::vector<Hit>& hs = hits[cell];
        if (hs.empty()) continue;
        std::sort(hs.begin(), hs.end(), [](const Hit& a, const Hit& b) { return a.y < b.y; });
        // each convex collider's extent on this line (its lowest to highest crossing)
        std::map<int, std::pair<float, float>> span;
        for (const Hit& h : hs) if (cols[h.col].convex) { auto it = span.try_emplace(h.col, h.y, h.y).first; it->second.first = std::min(it->second.first, h.y); it->second.second = std::max(it->second.second, h.y); }
        for (size_t q = 0; q < hs.size(); ++q) {
            const Hit& h = hs[q];
            const WorldCollider& c = *cols[h.col].c;
            if (c.role != WorldCollider::Solid && c.role != WorldCollider::Walkable && c.role != WorldCollider::Floor) continue;
            // a top: a convex shape's highest crossing, or a mesh's face with nothing of Meta's just over it (a slab's
            // underside has its top a few cm up: the bake's spans merge within 10 cm)
            if (cols[h.col].convex && h.y < span[h.col].second - 1e-4f) continue;
            if (!cols[h.col].convex && q + 1 < hs.size() && hs[q + 1].y - h.y <= walkgrid_detail::Raster::kMergeGap) continue;
            if (!tops[cell].empty() && std::fabs(tops[cell].back().y - h.y) < 0.02f) continue;   // (the same top twice)
            Top t{h.y, h.col, kFloor};
            if (!h.flat) t.kind = kSteep;
            for (const auto& [ci, sp] : span)
                if (ci != h.col && sp.first < h.y - 1e-3f && sp.second > h.y + 1e-3f) t.kind = t.kind == kFloor ? kBuried : t.kind;
            if (t.kind == kFloor) {
                // the next thing of Meta's over it, past what merges with it
                float ceil = 1e30f;
                for (size_t r = q + 1; r < hs.size(); ++r) if (hs[r].y > h.y + walkgrid_detail::Raster::kMergeGap) { ceil = hs[r].y; break; }
                for (const auto& [ci, sp] : span) if (ci != h.col && sp.first > h.y + walkgrid_detail::Raster::kMergeGap) ceil = std::min(ceil, sp.first);
                if (ceil - h.y < P.headroom) t.kind = kNoHeadroom;
                for (const auto& [ci, sp] : span)
                    if (cols[ci].c->role == WorldCollider::Object && sp.first < h.y + P.headroom && sp.second > h.y - P.step) t.kind = kInWall;
            }
            tops[cell].push_back(t);
        }
    }

    // Pieces of Meta's floor: neighbours a step or less apart; the ones the start is on are reached
    std::vector<double> pieceArea;
    std::vector<float> pieceY;
    std::vector<std::array<float, 4>> pieceBox;
    {
        std::vector<std::pair<size_t, int>> stack;
        for (size_t cell = 0; cell < tops.size(); ++cell)
            for (size_t k = 0; k < tops[cell].size(); ++k) {
                if (tops[cell][k].kind != kFloor || tops[cell][k].piece >= 0) continue;
                const int id = static_cast<int>(pieceArea.size());
                pieceArea.push_back(0); pieceY.push_back(0); pieceBox.push_back({1e30f, 1e30f, -1e30f, -1e30f});
                tops[cell][k].piece = id;
                stack.assign(1, {cell, static_cast<int>(k)});
                while (!stack.empty()) {
                    const auto [c0, k0] = stack.back();
                    stack.pop_back();
                    const int i = static_cast<int>(c0 % nx), j = static_cast<int>(c0 / nx);
                    const float y = tops[c0][k0].y;
                    pieceArea[id] += cs * cs; pieceY[id] += y * cs * cs;
                    auto& bx = pieceBox[id];
                    bx[0] = std::min(bx[0], cx(i)); bx[1] = std::min(bx[1], cz(j)); bx[2] = std::max(bx[2], cx(i)); bx[3] = std::max(bx[3], cz(j));
                    static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (const auto& d : d4) {
                        const int ii = i + d[0], jj = j + d[1];
                        if (ii < 0 || jj < 0 || ii >= nx || jj >= nz) continue;
                        const size_t c2 = static_cast<size_t>(jj) * nx + ii;
                        for (size_t k2 = 0; k2 < tops[c2].size(); ++k2) {
                            Top& t2 = tops[c2][k2];
                            if (t2.kind != kFloor || t2.piece >= 0 || std::fabs(t2.y - y) > P.step) continue;
                            t2.piece = id;
                            stack.push_back({c2, static_cast<int>(k2)});
                        }
                    }
                }
                pieceY[id] /= pieceArea[id];
            }
    }
    std::vector<uint8_t> reached(pieceArea.size(), 0);
    for (const auto& sd : seeds) {                                     // (as the bake: the floor within 1 m and 0.6 m of it)
        float best = 1e30f;
        int bp = -1;
        const int si = static_cast<int>(std::floor(sd[0] / cs)) - ix0, sj = static_cast<int>(std::floor(sd[2] / cs)) - iz0;
        const int r = static_cast<int>(std::ceil(1.0f / cs));
        for (int j = sj - r; j <= sj + r; ++j)
            for (int i = si - r; i <= si + r; ++i) {
                if (i < 0 || j < 0 || i >= nx || j >= nz) continue;
                for (const Top& t : tops[static_cast<size_t>(j) * nx + i]) {
                    if (t.kind != kFloor || std::fabs(t.y - sd[1]) > 0.6f) continue;
                    const float dd = std::hypot((i - si) * cs, (j - sj) * cs) + std::fabs(t.y - sd[1]);
                    if (dd < best) { best = dd; bp = t.piece; }
                }
            }
        if (bp >= 0) reached[bp] = 1;
    }

    // Inside Meta's walls: the cells a 2D flood from the start reaches without crossing an invisible wall (Object) at
    // any height. A piece with a cell there is inside them (a table's top, a ledge); one without is past them.
    std::vector<uint8_t> walled(hits.size(), 0), inside(hits.size(), 0);
    for (size_t cell = 0; cell < hits.size(); ++cell)
        for (const Hit& h : hits[cell]) walled[cell] |= cols[h.col].c->role == WorldCollider::Object;
    bool enclosed = true;
    {
        std::vector<size_t> stack;
        for (const auto& sd : seeds) {
            const int i = static_cast<int>(std::floor(sd[0] / cs)) - ix0, j = static_cast<int>(std::floor(sd[2] / cs)) - iz0;
            if (i < 0 || j < 0 || i >= nx || j >= nz) continue;
            const size_t c0 = static_cast<size_t>(j) * nx + i;
            if (!walled[c0] && !inside[c0]) { inside[c0] = 1; stack.push_back(c0); }
        }
        while (!stack.empty()) {
            const size_t c0 = stack.back();
            stack.pop_back();
            const int i = static_cast<int>(c0 % nx), j = static_cast<int>(c0 / nx);
            if (i == 0 || j == 0 || i == nx - 1 || j == nz - 1) enclosed = false;
            static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : d4) {
                const int ii = i + d[0], jj = j + d[1];
                if (ii < 0 || jj < 0 || ii >= nx || jj >= nz) continue;
                const size_t c2 = static_cast<size_t>(jj) * nx + ii;
                if (!walled[c2] && !inside[c2]) { inside[c2] = 1; stack.push_back(c2); }
            }
        }
    }
    std::vector<uint8_t> pieceInside(pieceArea.size(), 0);
    for (size_t cell = 0; cell < tops.size(); ++cell)
        for (const Top& t : tops[cell]) if (t.piece >= 0 && inside[cell]) pieceInside[t.piece] = 1;
    {
        double in = 0, inArea = 0;
        for (size_t cell = 0; cell < inside.size(); ++cell) inArea += inside[cell] ? cs * cs : 0;
        for (size_t p = 0; p < pieceArea.size(); ++p) if (pieceInside[p] && !reached[p]) in += pieceArea[p];
        std::printf("Meta's walls %s the start (%.1f m2 inside them); Meta's floor inside them not reached from the start: %.2f m2\n",
                    enclosed ? "enclose" : "don't enclose", inArea, in);
        for (size_t p = 0; p < pieceArea.size(); ++p)
            if (pieceInside[p] && !reached[p])
                std::printf("  inside, not reached: piece %zu %.2f m2 at y %.2f  x %.2f..%.2f z %.2f..%.2f\n", p, pieceArea[p], pieceY[p], pieceBox[p][0], pieceBox[p][2], pieceBox[p][1], pieceBox[p][3]);
    }

    std::printf("bake: %.1f m2 kept in %d area(s), %.1f m2 in %d left out, %.1f m2 bridged\n", rep.walkArea, rep.areas, rep.unreachedArea, rep.unreached, rep.bridgedArea);
    double metaReached = 0, metaOther = 0;
    for (size_t p = 0; p < pieceArea.size(); ++p) (reached[p] ? metaReached : metaOther) += pieceArea[p];
    std::printf("Meta's floor: %.1f m2 reached from the start; %.1f m2 more in %zu piece(s) not reached\n", metaReached, metaOther,
                pieceArea.size() - std::count(reached.begin(), reached.end(), 1));
    for (size_t p = 0; p < pieceArea.size(); ++p)
        if (reached[p] || std::getenv("PIECES"))
            std::printf("  piece %3zu %-11s %7.2f m2 at y %6.2f  x %6.2f..%6.2f z %6.2f..%6.2f\n", p, reached[p] ? "reached" : "not reached", pieceArea[p], pieceY[p],
                        pieceBox[p][0], pieceBox[p][2], pieceBox[p][1], pieceBox[p][3]);
    {
        double byKind[kKinds] = {};
        for (const auto& ts : tops) for (const Top& t : ts) byKind[t.kind] += cs * cs;
        std::printf("Meta's tops that aren't floor:");
        for (int k = 1; k < kKinds; ++k) std::printf(" %s %.1f m2;", kKindName[k], byKind[k]);
        std::printf("\n");
    }

    // Each of Meta's tops against the bake, and each walked cell against Meta's floor
    auto walkedAt = [&](float x, float z, float y, const WalkSpan** span) {
        int gx, gz;
        *span = nullptr;
        if (!g->cellOf(x, z, gx, gz)) return false;
        float bd = 0.06f;
        for (const WalkSpan* q = g->begin(gx, gz); q != g->end(gx, gz); ++q) if (std::fabs(q->hi - y) <= bd) { bd = std::fabs(q->hi - y); *span = q; }
        return g->field(x, z, y) >= 0;
    };
    std::map<std::string, double> missing, extra;
    double both = 0, walkedTotal = 0;
    const bool listCells = std::getenv("CELLS") != nullptr;
    const int PX = ref ? 2 : 4, W = nx * PX, H = nz * PX;
    std::vector<unsigned char> img(static_cast<size_t>(W) * H * 3, 20);
    auto paint = [&](int i, int j, int R, int G, int B) {
        for (int v = 0; v < PX; ++v) for (int u = 0; u < PX; ++u) {
            unsigned char* p = &img[(static_cast<size_t>((nz - 1 - j) * PX + v) * W + i * PX + u) * 3];
            p[0] = static_cast<unsigned char>(R); p[1] = static_cast<unsigned char>(G); p[2] = static_cast<unsigned char>(B);
        }
    };
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            const size_t cell = static_cast<size_t>(j) * nx + i;
            const float x = cx(i), z = cz(j);
            bool painted = false;
            for (const Top& t : tops[cell]) {
                const WalkSpan* sp;
                const bool walked = walkedAt(x, z, t.y, &sp);
                const bool want = t.kind == kFloor && reached[t.piece];
                if (want && walked) { both += cs * cs; if (!painted) paint(i, j, 40, 200, 70); painted = true; continue; }
                if (!want && !walked) {
                    if (!painted) { if (t.kind == kFloor) paint(i, j, 230, 120, 20); else paint(i, j, 70, 70, 70); }
                    painted |= t.kind == kFloor;
                    continue;
                }
                std::string why;
                if (want) {
                    if (!sp) why = "no floor there in the bake";
                    else if (sp->flags & kWalkFurniture) why = "peeled as furniture";
                    else if (sp->flags & kWalkUnreached) why = "left out (unreached / small)";
                    else if (sp->flags & kWalkWall) why = "a wall span (Object near)";
                    else if (sp->flags & kWalkNear) why = "near the edge (field < 0)";
                    else if (sp->flags & kWalkOk) why = "walkable but field < 0 (edge smoothing)";
                    else if (!(sp->flags & kWalkUp)) why = "bake top not up-facing";
                    else why = "bake floor not a candidate (headroom)";
                    missing[why] += cs * cs;
                    if (!painted) paint(i, j, 230, 30, 30);
                } else {
                    why = t.kind != kFloor ? std::string("Meta top ") + kKindName[t.kind] : "Meta floor not reached from the start";
                    if (sp && (sp->flags & kWalkBridged)) why += ", bridged";
                    extra[why] += cs * cs;
                    if (!painted) paint(i, j, 40, 90, 255);
                }
                painted = true;
                if (listCells) std::printf("  %s %6.2f %6.2f y %6.2f  %s (%s)\n", want ? "missing" : "extra  ", x, z, t.y, why.c_str(), cols[t.col].c->name.c_str());
            }
            // walked cells with no top of Meta's there at all
            int gx, gz;
            if (!g->cellOf(x, z, gx, gz)) continue;
            for (const WalkSpan* q = g->begin(gx, gz); q != g->end(gx, gz); ++q) {
                if (!(q->flags & (kWalkOk | kWalkNear)) || g->field(x, z, q->hi) < 0) continue;
                walkedTotal += cs * cs;
                bool meta = false;
                for (const Top& t : tops[cell]) meta |= std::fabs(t.y - q->hi) <= 0.06f;
                if (meta) continue;
                const std::string why = q->flags & kWalkBridged ? "no Meta top there, bridged" : (q->flags & kWalkOk) ? "no Meta top there (spans merged / raster)" : "no Meta top there, edge cell";
                extra[why] += cs * cs;
                if (!painted) paint(i, j, 40, 90, 255);
                painted = true;
                if (listCells) std::printf("  extra   %6.2f %6.2f y %6.2f  %s\n", x, z, q->hi, why.c_str());
            }
        }
    // (walked cells outside Meta's cells: none can be, the grid covers all of Meta's colliders within reach)
    std::printf("walked (field >= 0 at the cell's middle): %.1f m2; on Meta's reached floor: %.1f m2\n", walkedTotal, both);
    double m = 0, e = 0;
    for (const auto& [w, a] : missing) m += a;
    for (const auto& [w, a] : extra) e += a;
    std::printf("missing (Meta's floor not walked): %.2f m2\n", m);
    for (const auto& [w, a] : missing) std::printf("  %6.2f m2  %s\n", a, w.c_str());
    std::printf("extra (walked, not Meta's floor): %.2f m2\n", e);
    for (const auto& [w, a] : extra) std::printf("  %6.2f m2  %s\n", a, w.c_str());
    for (const auto& sd : seeds) {
        const int i = static_cast<int>(std::floor(sd[0] / cs)) - ix0, j = static_cast<int>(std::floor(sd[2] / cs)) - iz0;
        for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) if (i + di >= 0 && j + dj >= 0 && i + di < nx && j + dj < nz) paint(i + di, j + dj, 255, 255, 255);
    }
    if (argc > 2) {   // round the floor reached from the start, 1.5 m more each way
        float b[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        for (size_t p = 0; p < pieceArea.size(); ++p)
            if (reached[p]) { b[0] = std::min(b[0], pieceBox[p][0]); b[1] = std::min(b[1], pieceBox[p][1]); b[2] = std::max(b[2], pieceBox[p][2]); b[3] = std::max(b[3], pieceBox[p][3]); }
        if (b[0] > b[2]) { b[0] = cx(0); b[1] = cz(0); b[2] = cx(nx - 1); b[3] = cz(nz - 1); }
        const int i0 = std::max(0, static_cast<int>(std::floor((b[0] - 1.5f) / cs)) - ix0), i1 = std::min(nx - 1, static_cast<int>(std::floor((b[2] + 1.5f) / cs)) - ix0);
        const int j0 = std::max(0, static_cast<int>(std::floor((b[1] - 1.5f) / cs)) - iz0), j1 = std::min(nz - 1, static_cast<int>(std::floor((b[3] + 1.5f) / cs)) - iz0);
        const int w = (i1 - i0 + 1) * PX, h = (j1 - j0 + 1) * PX;
        std::vector<unsigned char> crop(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y)
            std::memcpy(&crop[static_cast<size_t>(y) * w * 3], &img[(static_cast<size_t>((nz - 1 - j1) * PX + y) * W + i0 * PX) * 3], static_cast<size_t>(w) * 3);
        stbi_write_png(argv[2], w, h, 3, crop.data(), w * 3);
        std::printf("picture: %s (%dx%d, x %.1f..%.1f z %.1f..%.1f, +x right, +z up)\n", argv[2], w, h, cx(i0), cx(i1), cz(j0), cz(j1));
    }
    return m > 0.005 || e > 0.005 ? 2 : 0;
}
