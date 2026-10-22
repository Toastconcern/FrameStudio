#pragma once
// World > Auto-generate, beyond the walkable area: collision for every mesh of the home — each drawn mesh (a vista's
// aren't: scenery) gets one "Auto collision: <mesh>" collider, boxes over the parts of it you could walk into (10 cm
// to 1.8 m over the floor next to it, where nothing of the area is), each column as tall as the mesh stands there and
// kept inside its cells: you get right up to it and no gap you could walk through closes. Nothing is judged to be a
// wall. Shared by Frame Studio, Frame Home and the headless tests (tools/walktest).
// Off unless GenParams asks (guesses, and a wrong one is worse than none):
//   walls      — Wall boxes on flat upright faces that are architecture (named a wall, or 2.4 m+ / up to the ceiling),
//                and along drops inside a tall structure
//   seats      — a top 35-62 cm over the floor with a backrest behind and floor in front, or a stool
//   hotspots   — lookouts, the middles of open spaces
// Everything made is named "Auto …" (Frame Studio replaces them on a re-run).
#include "walkgrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

struct GenParams {
    float wallMinHeight = 0.9f;         // a wall is at least this tall over the floor in front of it (a railing)
    float wallMinRun = 0.6f;            // shortest wall (m)
    float seatLo = 0.35f, seatHi = 0.62f;   // a seat's top over the floor in front of it (m): low lounge sofas to thick cushions
    int maxHotspots = 12;
    // Seats and hotspots are guesses about how people use a place, and a wrong one (a seat on a planter's rim) is worse
    // than none: off unless asked for. Auto-generate makes the floor, the walls and the objects' collision.
    bool seats = false, hotspots = false;
    // Walls (boxes on faces judged to be walls) are guesses too: off. Every mesh gets its own collision
    // instead (GenResult::objects), whatever it is.
    bool walls = false;
};

struct GenResult {
    std::vector<WorldCollider> walls, objects;
    std::vector<WorldPoint> seats, hotspots;
};

namespace world_gen_detail {

constexpr float kCell = 0.1f;                                          // the walls' coverage grid (m)

// The floor of the walkable area at (x, z) nearest height y within tol (Ok or Near), or nullptr.
inline const WalkSpan* areaFloor(const WalkGrid& g, float x, float z, float y, float tol) {
    int cx, cz;
    if (!g.cellOf(x, z, cx, cz)) return nullptr;
    return g.walkNear(cx, cz, y, tol);
}

// Anything solid at (x, z) between heights a and b?
inline bool solidBetween(const WalkGrid& g, float x, float z, float a, float b) {
    int cx, cz;
    if (!g.cellOf(x, z, cx, cz)) return false;
    for (const WalkSpan* s = g.begin(cx, cz); s != g.end(cx, cz); ++s)
        if (s->lo < b && s->hi > a) return true;
    return false;
}

inline float yawFacing(float fx, float fz) { return std::atan2(-fx, -fz); }   // WorldPoint yaw: 0 faces -Z

// A flat upright surface of the home: the drawn triangles in it, rasterized onto its plane.
struct Wall {
    float nx = 0, nz = 0, d = 0;        // horizontal normal and offset: n . (x, z) = d
    float s0 = 0, y0 = 0;               // the coverage grid's corner (along u = (-nz, nx), and height)
    int ns = 0, ny = 0;
    std::vector<uint8_t> cov;           // per cell: covered by a triangle
    bool glass = false;                 // see-through or cut-out (glass, foliage, a hedge): blocks; no panels on it
    WalkHint hint = kHintNone;          // what most of it is named (a wall, a cabinet, ...)
    bool at(int i, int j) const { return i >= 0 && j >= 0 && i < ns && j < ny && cov[static_cast<size_t>(j) * ns + i]; }
    bool covNear(int i, int j) const { return at(i, j) || at(i - 1, j) || at(i + 1, j); }
    float sOf(int i) const { return s0 + (i + 0.5f) * kCell; }
    int jOf(float y) const { return static_cast<int>(std::floor((y - y0) / kCell)); }
    // the share of cells covered in columns [i0, i1] between heights a and b
    float fill(int i0, int i1, float a, float b) const {
        int n = 0, c = 0;
        for (int i = i0; i <= i1; ++i) for (int j = jOf(a); j <= jOf(b); ++j) { ++n; c += at(i, j); }
        return n ? static_cast<float>(c) / n : 0;
    }
};

// One stretch of a wall with the area's floor in front: columns [i0, i1], which side the floor is on, its height, the
// wall's top.
struct Run { const Wall* w; int i0, i1; float side, floor, top; };

}  // namespace world_gen_detail

inline GenResult generateWorldItems(const WalkGrid& g, const HomeWorld& have, const std::vector<WalkSource>& drawn, const GenParams& P = {}) {
    using namespace world_gen_detail;
    GenResult out;
    if (!g.any()) return out;
    const float r = g.p.radius;
    float floorLo = 1e30f, floorHi = -1e30f;
    for (const WalkSpan& s : g.spans) if (s.flags & kWalkOk) { floorLo = std::min(floorLo, s.hi); floorHi = std::max(floorHi, s.hi); }
    const float gx0 = g.ox - 1, gz0 = g.oz - 1, gx1 = g.ox + g.nx * g.p.cell + 1, gz1 = g.oz + g.nz * g.p.cell + 1;

    // What the names say, where: per column, the named things' tops (height, hint) and every hint drawn in it. A name
    // counts as far as it fits the shape: a "fireplace" or a "desk" wider than 3 m is an area merged under one name
    // (the dome's fireplace mesh is its sofa corner too), so its name says nothing.
    const size_t ncol = static_cast<size_t>(g.nx) * g.nz;
    std::vector<std::vector<std::pair<float, uint8_t>>> namedTops(ncol);
    std::vector<uint8_t> namedHere(ncol, 0);                           // bit per WalkHint
    auto sampleNamed = [&](const float* a, const float* b, const float* c, WalkHint h, bool up) {
        const float e = std::max({std::hypot(b[0] - a[0], b[2] - a[2]), std::hypot(c[0] - a[0], c[2] - a[2]), std::hypot(c[0] - b[0], c[2] - b[2])});
        const int n = std::clamp(static_cast<int>(std::ceil(e / (g.p.cell * 0.5f))), 1, 80);
        for (int i = 0; i <= n; ++i)
            for (int j = 0; i + j <= n; ++j) {
                const float u = static_cast<float>(i) / n, v = static_cast<float>(j) / n, w = 1 - u - v;
                const float x = a[0] * w + b[0] * u + c[0] * v, y = a[1] * w + b[1] * u + c[1] * v, z = a[2] * w + b[2] * u + c[2] * v;
                int cx, cz;
                if (!g.cellOf(x, z, cx, cz)) continue;
                const size_t k = static_cast<size_t>(cz) * g.nx + cx;
                namedHere[k] |= static_cast<uint8_t>(1u << h);
                if (!up) continue;
                auto& t = namedTops[k];
                bool dup = false;
                for (const auto& q : t) dup |= q.second == h && std::fabs(q.first - y) < 0.03f;
                if (!dup && t.size() < 16) t.push_back({y, static_cast<uint8_t>(h)});
            }
    };

    // ── The home's flat upright surfaces ──
    std::vector<Wall> walls;
    {
        struct Tri { float v[9]; float nx, nz, d, area; bool glass; WalkHint hint; };
        std::vector<Tri> tris;
        for (const WalkSource& src : drawn) {
            std::vector<float> wp(src.verts * 3);
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (size_t v = 0; v < src.verts; ++v) {
                for (int k = 0; k < 3; ++k) wp[v * 3 + k] = src.world[k] * src.pos[v * 3] + src.world[4 + k] * src.pos[v * 3 + 1] + src.world[8 + k] * src.pos[v * 3 + 2] + src.world[12 + k];
                lo[0] = std::min(lo[0], wp[v * 3]); hi[0] = std::max(hi[0], wp[v * 3]); lo[1] = std::min(lo[1], wp[v * 3 + 2]); hi[1] = std::max(hi[1], wp[v * 3 + 2]);
            }
            WalkHint hint = src.hint;
            if (hint == kHintFurniture && std::max(hi[0] - lo[0], hi[1] - lo[1]) > 3.0f) hint = kHintNone;   // (seats: a row of chairs is chairs)
            for (size_t t = 0; t + 2 < src.indices; t += 3) {
                const uint32_t ia = src.idx[t], ib = src.idx[t + 1], ic = src.idx[t + 2];
                if (ia >= src.verts || ib >= src.verts || ic >= src.verts) continue;
                const float* a = &wp[ia * 3]; const float* b = &wp[ib * 3]; const float* c = &wp[ic * 3];
                const float cx = (a[0] + b[0] + c[0]) / 3, cy = (a[1] + b[1] + c[1]) / 3, cz = (a[2] + b[2] + c[2]) / 3;
                if (cx < gx0 || cx > gx1 || cz < gz0 || cz > gz1 || cy < floorLo - 1 || cy > floorHi + 25) continue;
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (hint != kHintNone && L > 1e-7f) sampleNamed(a, b, c, hint, std::fabs(n[1]) / L > 0.7f);
                if (L < 2e-3f || std::fabs(n[1]) / L > 0.09f) continue;    // upright (within 5 degrees: walls meet floors square), not a speck
                const float h = std::hypot(n[0], n[2]);
                Tri tr;
                std::copy_n(a, 3, tr.v); std::copy_n(b, 3, tr.v + 3); std::copy_n(c, 3, tr.v + 6);
                tr.nx = n[0] / h; tr.nz = n[2] / h; tr.d = tr.nx * cx + tr.nz * cz; tr.area = L * 0.5f; tr.glass = src.noFloor || src.cutout;
                tr.hint = hint;
                tris.push_back(tr);
            }
        }
        // planes: by facing (2 degree bins) and offset (4 cm bins), neighbouring bins joined
        auto binKey = [](int a, long d) { return static_cast<int64_t>(a) * 100000000LL + d; };
        std::unordered_map<int64_t, std::vector<uint32_t>> bins;
        std::unordered_map<int64_t, std::pair<int, long>> binOf;
        for (size_t i = 0; i < tris.size(); ++i) {
            float ang = std::atan2(tris[i].nz, tris[i].nx);
            if (ang < 0) ang += 6.2831853f;
            const int ab = static_cast<int>(std::lround(ang / 0.0349066f)) % 180;
            const long db = std::lround(tris[i].d / 0.04f);
            const int64_t k = binKey(ab, db);
            bins[k].push_back(static_cast<uint32_t>(i));
            binOf[k] = {ab, db};
        }
        std::unordered_map<int64_t, int64_t> parent;
        std::function<int64_t(int64_t)> find = [&](int64_t k) {
            auto it = parent.find(k);
            if (it == parent.end() || it->second == k) return k;
            const int64_t root = find(it->second);
            parent[k] = root;
            return root;
        };
        for (const auto& kv : binOf) {
            for (int da = -1; da <= 1; ++da)
                for (int dd = -1; dd <= 1; ++dd) {
                    const int64_t nk = binKey((kv.second.first + da + 180) % 180, kv.second.second + dd);
                    if (nk != kv.first && bins.count(nk)) { const int64_t a = find(kv.first), b = find(nk); if (a != b) parent[a] = b; }
                }
        }
        std::unordered_map<int64_t, std::vector<uint32_t>> planes;
        for (const auto& kv : bins) { auto& v = planes[find(kv.first)]; v.insert(v.end(), kv.second.begin(), kv.second.end()); }
        for (auto& kv : planes) {
            const std::vector<uint32_t>& ids = kv.second;
            double area = 0, nx = 0, nz = 0, glassArea = 0;
            for (uint32_t i : ids) { area += tris[i].area; nx += tris[i].nx * tris[i].area; nz += tris[i].nz * tris[i].area; glassArea += tris[i].glass ? tris[i].area : 0; }
            if (area < 0.4) continue;
            const double nl = std::hypot(nx, nz);
            if (nl < 1e-6) continue;
            Wall w;
            w.nx = static_cast<float>(nx / nl); w.nz = static_cast<float>(nz / nl);
            double d = 0;
            for (uint32_t i : ids) { const Tri& t = tris[i]; d += (w.nx * (t.v[0] + t.v[3] + t.v[6]) / 3 + w.nz * (t.v[2] + t.v[5] + t.v[8]) / 3) * t.area; }
            w.d = static_cast<float>(d / area);
            w.glass = glassArea > area * 0.5;
            double byHint[8] = {};
            for (uint32_t i : ids) byHint[tris[i].hint] += tris[i].area;
            for (int h = 1; h < 8; ++h) if (byHint[h] > area * 0.6) w.hint = static_cast<WalkHint>(h);
            const float ux = -w.nz, uz = w.nx;
            float smin = 1e30f, smax = -1e30f, ymin = 1e30f, ymax = -1e30f;
            for (uint32_t i : ids)
                for (int k = 0; k < 3; ++k) {
                    const float* p = &tris[i].v[k * 3];
                    smin = std::min(smin, ux * p[0] + uz * p[2]); smax = std::max(smax, ux * p[0] + uz * p[2]);
                    ymin = std::min(ymin, p[1]); ymax = std::max(ymax, p[1]);
                }
            ymax = std::min(ymax, ymin + 25.0f);
            w.s0 = smin; w.y0 = ymin;
            w.ns = std::min(1200, static_cast<int>(std::ceil((smax - smin) / kCell)) + 1);
            w.ny = std::min(260, static_cast<int>(std::ceil((ymax - ymin) / kCell)) + 1);
            w.cov.assign(static_cast<size_t>(w.ns) * w.ny, 0);
            for (uint32_t i : ids) {
                const Tri& t = tris[i];
                if (std::fabs(w.nx * (t.v[0] + t.v[3] + t.v[6]) / 3 + w.nz * (t.v[2] + t.v[5] + t.v[8]) / 3 - w.d) > 0.08f) continue;   // off the plane
                float P2[3][2];
                for (int k = 0; k < 3; ++k) { P2[k][0] = ux * t.v[k * 3] + uz * t.v[k * 3 + 2]; P2[k][1] = t.v[k * 3 + 1]; }
                const float area2 = (P2[1][0] - P2[0][0]) * (P2[2][1] - P2[0][1]) - (P2[2][0] - P2[0][0]) * (P2[1][1] - P2[0][1]);
                if (std::fabs(area2) < 1e-9f) continue;
                const int i0 = std::max(0, static_cast<int>(std::floor((std::min({P2[0][0], P2[1][0], P2[2][0]}) - w.s0) / kCell)));
                const int i1 = std::min(w.ns - 1, static_cast<int>(std::floor((std::max({P2[0][0], P2[1][0], P2[2][0]}) - w.s0) / kCell)));
                const int j0 = std::max(0, w.jOf(std::min({P2[0][1], P2[1][1], P2[2][1]}))), j1 = std::min(w.ny - 1, w.jOf(std::max({P2[0][1], P2[1][1], P2[2][1]})));
                for (int jj = j0; jj <= j1; ++jj)
                    for (int ii = i0; ii <= i1; ++ii) {
                        const float px = w.s0 + (ii + 0.5f) * kCell, py = w.y0 + (jj + 0.5f) * kCell;
                        bool in = true;
                        for (int k = 0; k < 3 && in; ++k) {
                            const float* A = P2[k]; const float* B = P2[(k + 1) % 3];
                            const float e = ((B[0] - A[0]) * (py - A[1]) - (B[1] - A[1]) * (px - A[0])) * (area2 > 0 ? 1 : -1);
                            in = e >= -kCell * 0.75f * std::hypot(B[0] - A[0], B[1] - A[1]);   // (slack: slivers count)
                        }
                        if (in) w.cov[static_cast<size_t>(jj) * w.ns + ii] = 1;
                    }
            }
            walls.push_back(std::move(w));
        }
    }

    // ── Their stretches with the area's floor in front ──
    std::vector<Run> runs;
    for (const Wall& w : walls) {
        const float ux = -w.nz, uz = w.nx;
        struct Col { bool ok = false; float side = 0, floor = 0, top = 0; };
        std::vector<Col> col(w.ns);
        for (int i = 0; i < w.ns; ++i) {
            const float s = w.sOf(i), px = w.nx * w.d + ux * s, pz = w.nz * w.d + uz * s;
            for (float side : {1.0f, -1.0f}) {
                for (float t : {0.22f, 0.3f, 0.42f, 0.6f, 0.85f}) {
                    int cx, cz;
                    if (!g.cellOf(px + w.nx * side * t, pz + w.nz * side * t, cx, cz)) continue;
                    const WalkSpan* f = nullptr;
                    for (const WalkSpan* sp = g.begin(cx, cz); sp != g.end(cx, cz); ++sp)
                        if ((sp->flags & (kWalkOk | kWalkNear)) && sp->hi >= w.y0 - 0.3f && sp->hi <= w.y0 + w.ny * kCell - P.wallMinHeight) { f = sp; break; }
                    if (!f) continue;
                    // the wall comes down to the floor and stands at least wallMinHeight over it, unbroken
                    int jTop = -1, gap = 0;
                    for (int jj = std::max(0, w.jOf(f->hi + 0.05f)); jj < w.ny; ++jj) {
                        if (w.covNear(i, jj)) { jTop = jj; gap = 0; }
                        else if (++gap > 2) break;
                    }
                    if (jTop < w.jOf(f->hi + P.wallMinHeight) || !w.covNear(i, std::max(0, w.jOf(f->hi + 0.25f)))) continue;
                    col[i] = {true, side, f->hi, w.y0 + (jTop + 1) * kCell};
                    break;
                }
                if (col[i].ok) break;
            }
        }
        // stretches: same side, same floor, short gaps bridged; then grown along the wall while it goes on (behind
        // furniture, past the end of the floor in front)
        int i = 0;
        while (i < w.ns) {
            if (!col[i].ok) { ++i; continue; }
            const float side = col[i].side, fl = col[i].floor;
            int i1 = i, last = i;
            std::vector<float> tops{col[i].top};
            for (int k = i + 1; k < w.ns && k - last <= 3; ++k)
                if (col[k].ok && col[k].side == side && std::fabs(col[k].floor - fl) < 0.2f) { last = k; i1 = k; tops.push_back(col[k].top); }
            int a = i, b = i1;
            while (a > 0 && w.fill(a - 1, a - 1, fl + 0.3f, fl + 1.2f) > 0.8f) --a;
            while (b + 1 < w.ns && w.fill(b + 1, b + 1, fl + 0.3f, fl + 1.2f) > 0.8f) ++b;
            std::nth_element(tops.begin(), tops.begin() + tops.size() / 2, tops.end());
            const float top = tops[tops.size() / 2];
            // a wall fills its rectangle (rocks, hedges and props don't), and never stands where you walk: it's split
            // where the area's floor passes through its line (a doorway, an opening you can step through)
            if (w.fill(a, b, fl + 0.15f, std::min(top, fl + 2.4f) - 0.1f) > 0.85f) {
                int k0 = a;
                for (int k = a; k <= b + 1; ++k) {
                    bool open = k > b;
                    if (!open) {
                        const float sk = w.sOf(k), qx = w.nx * (w.d - side * 0.05f) + ux * sk, qz = w.nz * (w.d - side * 0.05f) + uz * sk;
                        open = areaFloor(g, qx, qz, fl, 0.3f) != nullptr;
                    }
                    if (!open) continue;
                    const int e0 = k0 + (k0 > a ? 1 : 0), e1 = k - 1 - (k <= b ? 1 : 0);   // (10 cm clear of an opening)
                    if ((e1 - e0 + 1) * kCell >= P.wallMinRun) runs.push_back({&w, e0, e1, side, fl, top});
                    k0 = k + 1;
                }
            }
            i = std::max(i1, b) + 1;
        }
    }
    // ── What stands on the floor and stops you: columns of blocking things, grouped into pieces ──
    const float cs = g.p.cell;
    const int gnx = g.nx, gnz = g.nz;
    const size_t ncell = static_cast<size_t>(gnx) * gnz;
    std::vector<uint8_t> arch(ncell, 0);                               // columns that are architecture (walls, big structure)
    std::vector<int> objOf(ncell, -1);                                 // the piece a column belongs to
    std::vector<float> ref(ncell, -1e30f), top(ncell, -1e30f);         // its floor, and how tall what's on it stands
    std::vector<uint8_t> away(ncell, 255);                             // how far it is from the area, in cells
    struct Piece { std::vector<size_t> cells; int x0, x1, z0, z1; bool big; };
    std::vector<Piece> pieces;
    {
        const int nx = gnx, nz = gnz;
        // the floor each column stands on: the nearest walkable floor's height, up to 2 m away
        std::vector<int> q;
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
                    if ((s->flags & (kWalkOk | kWalkNear)) && ref[static_cast<size_t>(z) * nx + x] < -1e29f) { ref[static_cast<size_t>(z) * nx + x] = s->hi; away[static_cast<size_t>(z) * nx + x] = 0; q.push_back(z * nx + x); }
        for (size_t h = 0, steps = 0; h < q.size() && steps < 20; ++steps) {
            const size_t end = q.size();
            for (; h < end; ++h) {
                const int c = q[h], x = c % nx, z = c / nx;
                static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : d4) {
                    const int xx = x + d[0], zz = z + d[1];
                    if (!g.inside(xx, zz) || ref[static_cast<size_t>(zz) * nx + xx] > -1e29f) continue;
                    ref[static_cast<size_t>(zz) * nx + xx] = ref[c];
                    away[static_cast<size_t>(zz) * nx + xx] = static_cast<uint8_t>(away[c] + 1);
                    q.push_back(zz * nx + xx);
                }
            }
        }
        // a column that stops you: something between 30 cm (a low rim or a step is the area's edge already) and 1.8 m
        // over its floor, and no floor of the area there
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x) {
                const size_t c = static_cast<size_t>(z) * nx + x;
                const float f = ref[c];
                if (f < -1e29f) continue;
                bool walk = false;
                float t = -1e30f;
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                    if ((s->flags & (kWalkOk | kWalkNear)) && std::fabs(s->hi - f) <= 0.15f) walk = true;
                    if (s->lo < f + 1.8f && s->hi > f + 0.3f) t = std::max(t, std::min(s->hi, f + 4.0f));
                }
                if (!walk && t > -1e29f) top[c] = t;
            }
        // pieces: blocking columns side by side; big ones are structure (a dome's ring, terrain), not objects
        for (size_t c0 = 0; c0 < ncell; ++c0) {
            if (top[c0] < -1e29f || objOf[c0] >= 0) continue;
            Piece P{{c0}, nx, -1, nz, -1, false};
            std::vector<size_t> st{c0};
            objOf[c0] = static_cast<int>(pieces.size());
            while (!st.empty()) {
                const size_t c = st.back();
                st.pop_back();
                const int x = static_cast<int>(c % nx), z = static_cast<int>(c / nx);
                P.x0 = std::min(P.x0, x); P.x1 = std::max(P.x1, x); P.z0 = std::min(P.z0, z); P.z1 = std::max(P.z1, z);
                static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : d4) {
                    const int xx = x + d[0], zz = z + d[1];
                    if (!g.inside(xx, zz)) continue;
                    const size_t n = static_cast<size_t>(zz) * nx + xx;
                    if (top[n] > -1e29f && objOf[n] < 0 && std::fabs(ref[n] - ref[c]) < 0.5f) { objOf[n] = objOf[c0]; st.push_back(n); P.cells.push_back(n); }
                }
            }
            P.big = (P.x1 - P.x0 + 1) * cs > 8 || (P.z1 - P.z0 + 1) * cs > 8 || P.cells.size() * cs * cs > 12;
            pieces.push_back(std::move(P));
        }
    }

    // walls are architecture: named one; else (named nothing that says) a face up to the ceiling (a room's: 1.9 m up or more), or 2.4 m or taller on something that isn't a piece of furniture
    // (a cabinet's, a bookshelf's or a tall screen's front is that object's, not a wall)
    auto behind = [&](const Run& rn, float t, int i, int& cx, int& cz) {
        const Wall& w = *rn.w;
        return g.cellOf(w.nx * (w.d - rn.side * t) + -w.nz * w.sOf(i), w.nz * (w.d - rn.side * t) + w.nx * w.sOf(i), cx, cz);
    };
    runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const Run& rn) {
        const Wall& w = *rn.w;
        const float sm = w.s0 + (rn.i0 + rn.i1 + 1) * 0.5f * kCell, ux = -w.nz, uz = w.nx;
        if (w.hint == kHintWall || w.hint == kHintBlocker) return false;   // named a wall, and flat and upright: one
        if (w.hint == kHintFurniture || w.hint == kHintSeat || w.hint == kHintNature) return true;   // a thing's side
        int furniture = 0, n = 0;                                      // what's behind the face: an object, or structure
        for (int i = rn.i0; i <= rn.i1; ++i)
            for (float t : {0.05f, 0.15f}) {
                int cx, cz;
                if (!behind(rn, t, i, cx, cz)) continue;
                const int o = objOf[static_cast<size_t>(cz) * gnx + cx];
                if (o < 0) continue;
                ++n;
                furniture += !pieces[o].big;
            }
        if (n > 0 && furniture * 2 > n) return true;
        if (rn.top - rn.floor < 1.9f) return true;                     // (under a roof, an awning, a canopy: still low)
        for (float t : {0.15f, 0.4f}) {
            int cx, cz;
            if (!g.cellOf(w.nx * (w.d + rn.side * t) + ux * sm, w.nz * (w.d + rn.side * t) + uz * sm, cx, cz)) continue;
            for (const WalkSpan* q = g.begin(cx, cz); q != g.end(cx, cz); ++q)
                if (q->lo >= rn.top - 0.2f && q->lo <= rn.top + 0.5f) return false;   // a ceiling over it
        }
        return rn.top - rn.floor < 2.4f;
    }), runs.end());
    if (!P.walls) runs.clear();
    for (const Run& rn : runs) {
        const Wall& w = *rn.w;
        const float ux = -w.nz, uz = w.nx, L = (rn.i1 - rn.i0 + 1) * kCell, H = std::clamp(rn.top - rn.floor, P.wallMinHeight, 15.0f);
        const float sm = w.s0 + (rn.i0 + rn.i1 + 1) * 0.5f * kCell, T = 0.1f;
        const float cx = w.nx * w.d + ux * sm - w.nx * rn.side * T * 0.5f, cz = w.nz * w.d + uz * sm - w.nz * rn.side * T * 0.5f;   // behind the face
        WorldCollider c;
        c.name = "Auto wall";
        c.kind = WorldCollider::Box;
        c.role = WorldCollider::Wall;
        const float m[16] = {ux, 0, uz, 0, 0, 1, 0, 0, -uz, 0, ux, 0, cx, rn.floor + H * 0.5f, cz, 1};   // X along it, Y up, Z = X x Y
        std::copy_n(m, 16, c.world);
        c.half[0] = L * 0.5f; c.half[1] = H * 0.5f; c.half[2] = T * 0.5f;
        out.walls.push_back(c);
        for (int i = rn.i0; i <= rn.i1; ++i)                           // its columns are the wall's
            for (float t : {0.03f, 0.1f, 0.18f}) {
                int qx, qz;
                if (behind(rn, t, i, qx, qz)) arch[static_cast<size_t>(qz) * gnx + qx] = 1;
            }
    }

    // ── Collision: every mesh, its own — the parts of it you can walk into (10 cm to 1.8 m over the floor, where
    //    nothing of the area is), each column as tall as that mesh stands in it ──
    {
        std::unordered_map<uint64_t, float> tallest;                   // (mesh, column) -> its highest point there
        for (size_t si = 0; si < drawn.size(); ++si) {
            const WalkSource& src = drawn[si];
            std::vector<float> wp(src.verts * 3);
            for (size_t v = 0; v < src.verts; ++v)
                for (int k = 0; k < 3; ++k) wp[v * 3 + k] = src.world[k] * src.pos[v * 3] + src.world[4 + k] * src.pos[v * 3 + 1] + src.world[8 + k] * src.pos[v * 3 + 2] + src.world[12 + k];
            for (size_t t = 0; t + 2 < src.indices; t += 3) {
                const uint32_t ia = src.idx[t], ib = src.idx[t + 1], ic = src.idx[t + 2];
                if (ia >= src.verts || ib >= src.verts || ic >= src.verts) continue;
                const float* A = &wp[ia * 3]; const float* B = &wp[ib * 3]; const float* C = &wp[ic * 3];
                if (std::max({A[0], B[0], C[0]}) < gx0 || std::min({A[0], B[0], C[0]}) > gx1 || std::max({A[2], B[2], C[2]}) < gz0 ||
                    std::min({A[2], B[2], C[2]}) > gz1 || std::max({A[1], B[1], C[1]}) < floorLo - 0.5f || std::min({A[1], B[1], C[1]}) > floorHi + 2.0f) continue;   // nowhere near the floor
                const float e = std::max({std::hypot(B[0] - A[0], B[2] - A[2]), std::hypot(C[0] - A[0], C[2] - A[2]), std::hypot(C[0] - B[0], C[2] - B[2]),
                                          std::fabs(B[1] - A[1]), std::fabs(C[1] - A[1])});
                const int n = std::clamp(static_cast<int>(std::ceil(e / (cs * 0.5f))), 1, 80);
                for (int i = 0; i <= n; ++i)
                    for (int j = 0; i + j <= n; ++j) {
                        const float u = static_cast<float>(i) / n, v = static_cast<float>(j) / n, w = 1 - u - v;
                        const float x = A[0] * w + B[0] * u + C[0] * v, y = A[1] * w + B[1] * u + C[1] * v, z = A[2] * w + B[2] * u + C[2] * v;
                        int cx, cz;
                        if (!g.cellOf(x, z, cx, cz)) continue;
                        const size_t k = static_cast<size_t>(cz) * gnx + cx;
                        if (top[k] < -1e29f || y < ref[k] + 0.1f || y > ref[k] + 1.8f) continue;   // not where you'd walk into it
                        float& hi = tallest[(static_cast<uint64_t>(si) << 32) | k];
                        hi = std::max(hi == 0 ? -1e30f : hi, std::min(y, top[k]));
                    }
            }
        }
        std::vector<std::vector<std::pair<size_t, float>>> byMesh(drawn.size());
        for (const auto& kv : tallest) byMesh[kv.first >> 32].push_back({static_cast<size_t>(kv.first & 0xffffffffu), kv.second});
        for (size_t si = 0; si < drawn.size(); ++si) {
            auto& mc = byMesh[si];
            if (mc.empty()) continue;
            std::sort(mc.begin(), mc.end());
            const int nx = gnx;
            int z0 = gnz, z1 = -1;
            for (const auto& c : mc) { z0 = std::min(z0, static_cast<int>(c.first / nx)); z1 = std::max(z1, static_cast<int>(c.first / nx)); }
            // its footprint as rectangles (rows of cells, stacked where they line up and stand about as tall)
            std::map<std::pair<int, int>, int> open;                   // (x0, x1) of a run in the last row -> rectangle
            struct Rect { int x0, x1, z0, z1; float top, floor; };
            std::vector<Rect> rects;
            std::vector<std::vector<std::pair<int, float>>> rows(z1 - z0 + 1);
            for (const auto& c : mc) rows[c.first / nx - z0].push_back({static_cast<int>(c.first % nx), c.second});
            for (int r = 0; r <= z1 - z0; ++r) {
                std::sort(rows[r].begin(), rows[r].end());
                std::map<std::pair<int, int>, int> next;
                for (size_t i = 0; i < rows[r].size();) {
                    size_t j = i;
                    float t = rows[r][i].second;
                    while (j + 1 < rows[r].size() && rows[r][j + 1].first == rows[r][j].first + 1 && std::fabs(rows[r][j + 1].second - t) <= 0.2f) { ++j; t = std::max(t, rows[r][j].second); }
                    const int a = rows[r][i].first, b = rows[r][j].first, z = z0 + r;
                    float f = 1e30f;
                    for (int x = a; x <= b; ++x) f = std::min(f, ref[static_cast<size_t>(z) * nx + x]);
                    auto it = open.find({a, b});
                    if (it != open.end() && std::fabs(rects[it->second].top - t) <= 0.2f) {
                        Rect& Q = rects[it->second];
                        Q.z1 = z; Q.top = std::max(Q.top, t); Q.floor = std::min(Q.floor, f);
                        next[{a, b}] = it->second;
                    } else {
                        rects.push_back({a, b, z, z, t, f});
                        next[{a, b}] = static_cast<int>(rects.size()) - 1;
                    }
                    i = j + 1;
                }
                open.swap(next);
            }
            WorldCollider c;
            c.name = "Auto collision" + (drawn[si].name.empty() ? std::string() : ": " + drawn[si].name);
            c.kind = WorldCollider::Mesh;
            c.role = WorldCollider::Wall;                              // stops you; never a floor (no standing on a table)
            c.hidden = true;                                           // (Studio's view: it's the mesh's own shape, not a wall)
            for (const Rect& Q : rects) {
                // a column blocks if anything's in it, so the thing's side can be anywhere in the edge cells: the box keeps
                // inside them (the drawn mesh still stops you where it really is; the box never takes a gap from you)
                const float in = cs * 0.45f;
                const float xa = g.ox + Q.x0 * cs + in, xb = g.ox + (Q.x1 + 1) * cs - in, za = g.oz + Q.z0 * cs + in, zb = g.oz + (Q.z1 + 1) * cs - in;
                const uint32_t b = static_cast<uint32_t>(c.positions.size() / 3);
                for (int k = 0; k < 8; ++k) {
                    c.positions.push_back(k & 1 ? xb : xa);
                    c.positions.push_back(k & 2 ? std::max(Q.top, Q.floor + 0.1f) : Q.floor);
                    c.positions.push_back(k & 4 ? zb : za);
                }
                static const uint32_t f[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
                for (uint32_t i : f) c.indices.push_back(b + i);
            }
            out.objects.push_back(std::move(c));
        }
    }

    // ── Walls where the floor ends inside a structure: a dome's open lattice, a frame over a drop ──
    if (P.walls) {
        std::vector<WorldCollider> encl;
        auto covered = [&](float x, float z) {                         // a room wall already stands there
            for (const WorldCollider& c : out.walls) {
                const float dx = x - c.world[12], dz = z - c.world[14];
                const float a = dx * c.world[0] + dz * c.world[2], b = dx * c.world[8] + dz * c.world[10];
                if (std::fabs(a) <= c.half[0] + 0.1f && std::fabs(b) <= r + 0.35f) return true;
            }
            return false;
        };
        struct ESeg { float a[2], b[2], out[2], h, H; };
        std::vector<ESeg> es;
        g.outline([](const float*, const float*, const float*, bool) {}, [&](const float* a, const float* b) {
            const float dx = b[0] - a[0], dz = b[2] - a[2], L = std::hypot(dx, dz);
            if (L < 1e-5f) return;
            float nx = -dz / L, nz = dx / L;
            const float mx = (a[0] + b[0]) * 0.5f, mz = (a[2] + b[2]) * 0.5f, h = (a[1] + b[1]) * 0.5f;
            if (g.field(mx + nx * 0.03f, mz + nz * 0.03f, h) > g.field(mx - nx * 0.03f, mz - nz * 0.03f, h)) { nx = -nx; nz = -nz; }
            // beyond the edge: a drop or a low rim (nothing there stops you: an object's or a wall's edge has its own
            // collider), with structure over or round it within 1.5 m (a lattice, a frame, a roof)
            bool structure = false;
            for (float t : {r + 0.1f, r + 0.3f, r + 0.7f, r + 1.1f, r + 1.5f}) {
                int cx, cz;
                if (!g.cellOf(mx + nx * t, mz + nz * t, cx, cz)) continue;
                for (const WalkSpan* q = g.begin(cx, cz); q != g.end(cx, cz); ++q) {
                    if ((q->flags & (kWalkOk | kWalkNear)) && t <= r + 1.1f) return;   // floor of the area below: a ledge in
                                                                                       // it, not where the world ends
                    if (t <= r + 0.35f) {
                        if (q->lo < h + 1.8f && q->hi > h + 0.3f) return;                                  // something stops you
                    }
                    structure |= q->hi > h + 1.5f && q->lo < h + 6.0f;      // (taller than a sofa's end or a planter)
                }
            }
            if (!structure || covered(mx + nx * r, mz + nz * r)) return;
            es.push_back({{a[0], a[2]}, {b[0], b[2]}, {nx, nz}, h, 2.5f});
        });
        // straight runs: pieces end to end, simplified, 60 cm or longer
        std::vector<char> used(es.size(), 0);
        auto key = [](const float* p) { return std::make_pair(std::lround(p[0] * 200), std::lround(p[1] * 200)); };
        std::map<std::pair<long, long>, size_t> startAt;
        for (size_t i = 0; i < es.size(); ++i) startAt[key(es[i].a)] = i;
        for (size_t i = 0; i < es.size(); ++i) {
            if (used[i]) continue;
            size_t first = i;
            for (size_t guard = 0; guard < es.size(); ++guard) {       // back to where this stretch starts
                bool found = false;
                for (size_t j = 0; j < es.size() && !found; ++j)
                    if (!used[j] && j != first && key(es[j].b) == key(es[first].a)) { first = j; found = true; }
                if (!found || first == i) break;
            }
            std::vector<size_t> chain;
            for (size_t cur = first; !used[cur];) {
                used[cur] = 1;
                chain.push_back(cur);
                auto it = startAt.find(key(es[cur].b));
                if (it == startAt.end()) break;
                cur = it->second;
            }
            // split into straight pieces: a new piece where the direction turns more than 20 degrees from its start
            size_t k0 = 0;
            while (k0 < chain.size()) {
                const ESeg& s0 = es[chain[k0]];
                const float d0x = s0.b[0] - s0.a[0], d0z = s0.b[1] - s0.a[1], l0 = std::hypot(d0x, d0z);
                size_t k1 = k0;
                float H = s0.H, ox = s0.out[0], oz = s0.out[1], h = s0.h;
                while (k1 + 1 < chain.size()) {
                    const ESeg& sn = es[chain[k1 + 1]];
                    const float ax = sn.b[0] - s0.a[0], az = sn.b[1] - s0.a[1], al = std::hypot(ax, az);
                    if (al < 1e-4f || (ax * d0x + az * d0z) / (al * l0) < 0.94f || std::fabs(sn.h - s0.h) > 0.1f) break;
                    bool hugs = true;                                  // the straight run keeps to the edge (a chord across a
                    for (size_t k = k0; k <= k1 + 1 && hugs; ++k) {    // bend would cut into the floor)
                        const ESeg& sk = es[chain[k]];
                        hugs = std::fabs((sk.b[0] - s0.a[0]) * az - (sk.b[1] - s0.a[1]) * ax) / al <= 0.08f;
                    }
                    if (!hugs) break;
                    ++k1;
                    H = std::min(H, sn.H); ox += sn.out[0]; oz += sn.out[1];
                }
                const ESeg& se = es[chain[k1]];
                const float ax = s0.a[0], az = s0.a[1], bx = se.b[0], bz = se.b[1], L = std::hypot(bx - ax, bz - az);
                if (L >= P.wallMinRun) {
                    const float ux = (bx - ax) / L, uz = (bz - az) / L, ol = std::hypot(ox, oz);
                    const float nxo = ox / ol, nzo = oz / ol, T = 0.1f;
                    const float cx = (ax + bx) * 0.5f + nxo * (r + 0.1f + T * 0.5f), cz = (az + bz) * 0.5f + nzo * (r + 0.1f + T * 0.5f);   // clear of the edge where a straight run cuts a curve
                    WorldCollider c;
                    c.name = "Auto wall";
                    c.kind = WorldCollider::Box;
                    c.role = WorldCollider::Wall;
                    const float m[16] = {ux, 0, uz, 0, 0, 1, 0, 0, -uz, 0, ux, 0, cx, h + H * 0.5f, cz, 1};
                    std::copy_n(m, 16, c.world);
                    c.half[0] = L * 0.5f; c.half[1] = H * 0.5f; c.half[2] = T * 0.5f;
                    encl.push_back(c);
                }
                k0 = k1 + 1;
            }
        }
        for (WorldCollider& c : encl) out.walls.push_back(std::move(c));
    }

    // ── Seats: where on a seat-high top you'd sit — a backrest behind, floor in front — or a free-standing stool ──
    if (P.seats) {
        const float cs = g.p.cell;
        const int nx = g.nx, nz = g.nz, reach = static_cast<int>(std::ceil(1.1f / cs));
        // per span: the floor it's a seat's height over (or none)
        std::vector<float> floorOf(g.spans.size(), -1e30f);
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                    if (!(s->flags & (kWalkUp | kWalkFurniture)) || (s->flags & (kWalkOk | kWalkNear | kWalkWall)) || (s->cut & kWalkCutTop)) continue;
                    if (s + 1 != g.end(x, z) && (s + 1)->lo - s->hi < 1.0f) continue;   // no room to sit up
                    float best = -1e30f;
                    for (int j = z - reach; j <= z + reach; j += 2)
                        for (int i = x - reach; i <= x + reach; i += 2) {
                            if (!g.inside(i, j)) continue;
                            for (const WalkSpan* f = g.begin(i, j); f != g.end(i, j); ++f)
                                if ((f->flags & kWalkOk) && s->hi - f->hi >= P.seatLo && s->hi - f->hi <= P.seatHi) best = std::max(best, f->hi);
                        }
                    floorOf[s - g.spans.data()] = best;
                }
        // what a top's name says, over a piece's cells: a seat, something else (a table, a planter's rim, a floor or a
        // wall's top: never sat on, however it's shaped), or nothing (the shape decides)
        enum Named { kUnnamed, kNamedSeat, kNamedOther };
        // A home that names its seats names them all: there, only what's named a seat is one (a missed seat is better
        // than a seat on a planter's rim or a sofa's back). A home naming none is left to the shapes.
        bool namesSeats = false;
        for (const auto& col : namedTops) for (const auto& t : col) namesSeats |= t.second == kHintSeat;
        auto namedAs = [&](const auto& xs, auto xOf, auto zOf, auto yOf) {
            size_t seat = 0, other = 0;
            for (const auto& c : xs) {
                int best = 0;
                for (const auto& t : namedTops[static_cast<size_t>(zOf(c)) * nx + xOf(c)])
                    if (std::fabs(t.first - yOf(c)) <= 0.12f) best = t.second == kHintSeat ? 2 : std::max(best, 1);
                seat += best == 2; other += best == 1;
            }
            if (seat * 10 >= xs.size() * 4) return kNamedSeat;
            if (seat * 10 < xs.size() && (other * 10 >= xs.size() * 6 || namesSeats)) return kNamedOther;
            return kUnnamed;
        };
        auto seatSpan = [&](int x, int z, float y) -> const WalkSpan* {      // a seat-high top at (x, z) near height y
            if (!g.inside(x, z)) return nullptr;
            for (const WalkSpan* q = g.begin(x, z); q != g.end(x, z); ++q)
                if (floorOf[q - g.spans.data()] > -1e29f && std::fabs(q->hi - y) <= 0.05f) return q;
            return nullptr;
        };
        // The seat-high tops, piece by piece (heights within 6 cm of their neighbour's; up to 12 cm across a
        // piece: cushions sag), each looked at all round its edge: open floor beyond a side is where you sit down
        // (its front), a backrest rises 20-100 cm over it (higher: a wall or a hedge, and it's a ledge), anything
        // else (a drop, a footrest) is neither. A piece with a front and an even backrest is a seat; its seats go
        // along each stretch of front, 60 cm apart, set back towards the backrest, facing out through the front.
        // A "backrest" with the top going on beyond it is something standing on it (a vase on a table): not one.
        enum Side { kInside, kFront, kBack, kWall, kOther };
        std::vector<int> region(g.spans.size(), -1);
        auto regionSpan = [&](int x, int z, int id) -> const WalkSpan* {
            if (!g.inside(x, z)) return nullptr;
            for (const WalkSpan* q = g.begin(x, z); q != g.end(x, z); ++q) if (region[q - g.spans.data()] == id) return q;
            return nullptr;
        };
        int nextId = 0;
        for (int z0 = 0; z0 < nz; ++z0)
            for (int x0 = 0; x0 < nx; ++x0)
                for (const WalkSpan* s0 = g.begin(x0, z0); s0 != g.end(x0, z0); ++s0) {
                    const size_t i0 = s0 - g.spans.data();
                    if (floorOf[i0] < -1e29f || region[i0] >= 0) continue;
                    const int id = nextId++;
                    struct C { int x, z; const WalkSpan* s; };
                    std::vector<C> cells;
                    std::vector<C> st{{x0, z0, s0}};
                    region[i0] = id;
                    float ylo = 1e30f, yhi = -1e30f, fl = -1e30f;
                    bool taken = false;
                    while (!st.empty()) {
                        const C c = st.back();
                        st.pop_back();
                        cells.push_back(c);
                        ylo = std::min(ylo, c.s->hi); yhi = std::max(yhi, c.s->hi); fl = std::max(fl, floorOf[c.s - g.spans.data()]);
                        for (const WorldPoint& o : have.seats)       // one the home already has a seat on stays as it is
                            taken |= std::hypot(o.pos[0] - g.centerX(c.x), o.pos[2] - g.centerZ(c.z)) < 0.35f && std::fabs(o.pos[1] - c.s->hi) < 0.4f;
                        static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : d4)
                            if (const WalkSpan* q = seatSpan(c.x + d[0], c.z + d[1], c.s->hi); q && region[q - g.spans.data()] < 0 && std::fabs(q->hi - c.s->hi) <= 0.06f) {
                                region[q - g.spans.data()] = id;
                                st.push_back({c.x + d[0], c.z + d[1], q});
                            }
                    }
                    if (taken || cells.size() < 12 || cells.size() > 4000 || yhi - ylo > 0.12f) continue;
                    const Named named = namedAs(cells, [](const C& c) { return c.x; }, [](const C& c) { return c.z; }, [](const C& c) { return c.s->hi; });
                    if (named == kNamedOther) continue;
                    // what's beyond each edge
                    bool backNamedSeat = false, backNamedGround = false;   // (set by side(): the backrest found is named a seat,
                                                                           // or plants, rocks, a planter's soil: not a backrest)
                    auto side = [&](const C& c, int dx, int dz, float& top) -> Side {
                        if (regionSpan(c.x + dx, c.z + dz, id)) return kInside;
                        const float t = c.s->hi;
                        for (int k = 1; k <= 5; ++k) {                 // up to 50 cm out: a backrest, a wall, or on
                            const int qx = c.x + dx * k, qz = c.z + dz * k;
                            if (!g.inside(qx, qz)) break;
                            float hi = -1e30f;
                            for (const WalkSpan* q = g.begin(qx, qz); q != g.end(qx, qz); ++q)
                                if (q->lo <= t + 0.3f && q->hi > t - 0.3f && !(q->cut & kWalkCutAll)) hi = std::max(hi, q->hi);
                            if (hi >= t + 0.2f) {
                                top = hi;
                                if (hi > t + 1.0f) return kWall;
                                // the seat's top going on past it: something standing on a surface, not a backrest
                                for (int e = 2; e <= 4; ++e) if (regionSpan(qx + dx * e, qz + dz * e, id)) return kOther;
                                backNamedSeat = backNamedGround = false;
                                for (const auto& n : namedTops[static_cast<size_t>(qz) * nx + qx]) {
                                    backNamedSeat |= n.second == kHintSeat && n.first > t + 0.1f;
                                    backNamedGround |= (n.second == kHintNature || n.second == kHintFloor) && n.first > t + 0.1f;
                                }
                                if (namedHere[static_cast<size_t>(qz) * nx + qx] & (1u << kHintNature)) backNamedGround = true;
                                return kBack;
                            }
                            if (hi < t - 0.1f) break;                  // it drops away
                        }
                        for (int k = 1; k <= 4; ++k) {                 // floor to stand on just beyond: the front
                            int qx = c.x + dx * k, qz = c.z + dz * k;
                            if (!g.inside(qx, qz)) break;
                            for (const WalkSpan* q = g.begin(qx, qz); q != g.end(qx, qz); ++q)
                                if ((q->flags & (kWalkOk | kWalkNear)) && std::fabs(q->hi - fl) <= 0.2f) return kFront;
                        }
                        return kOther;
                    };
                    struct E { int c; int dx, dz; };
                    std::vector<E> fronts;
                    std::vector<float> backTops;
                    size_t seatBacks = 0, groundBacks = 0;             // backrests that are a seat's own (its cushions), or plants
                    std::vector<int> cellOfSpan;
                    for (size_t i = 0; i < cells.size(); ++i) {
                        static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : d4) {
                            float top = 0;
                            const Side sd = side(cells[i], d[0], d[1], top);
                            if (sd == kFront) fronts.push_back({static_cast<int>(i), d[0], d[1]});
                            else if (sd == kBack) { backTops.push_back(top - cells[i].s->hi); seatBacks += backNamedSeat; groundBacks += backNamedGround; }
                        }
                    }
                    // leaning on a seat from behind (a sofa's rim round its back cushions) isn't sitting: the seat's the other side
                    if (named != kNamedSeat && seatBacks * 2 > backTops.size()) continue;
                    // a planter's rim with the plants behind it: a ledge
                    if (named != kNamedSeat && groundBacks * 2 > backTops.size()) continue;
                    // nowhere to sit down from, or no back (named a seat: some of a back will do; else it's a stool's)
                    if (fronts.size() < 3 || backTops.size() < (named == kNamedSeat ? 1u : 3u)) continue;
                    double m = 0, m2 = 0;
                    for (float v : backTops) { m += v; m2 += v * v; }
                    m /= backTops.size();
                    if (named != kNamedSeat && std::sqrt(std::max(0.0, m2 / backTops.size() - m * m)) > 0.1) continue;   // uneven: rocks, plants
                    // stretches of front: front edges whose cells touch
                    std::vector<int> group(fronts.size(), -1);
                    int groups = 0;
                    for (size_t i = 0; i < fronts.size(); ++i) {
                        if (group[i] >= 0) continue;
                        std::vector<size_t> q{i};
                        group[i] = groups;
                        while (!q.empty()) {
                            const size_t a = q.back();
                            q.pop_back();
                            for (size_t b = 0; b < fronts.size(); ++b) {
                                if (group[b] >= 0) continue;
                                const C& ca = cells[fronts[a].c]; const C& cb = cells[fronts[b].c];
                                if (std::abs(ca.x - cb.x) <= 1 && std::abs(ca.z - cb.z) <= 1 && fronts[a].dx * fronts[b].dx + fronts[a].dz * fronts[b].dz >= 0) { group[b] = groups; q.push_back(b); }
                            }
                        }
                        ++groups;
                    }
                    for (int gi = 0; gi < groups; ++gi) {
                        float nxs = 0, nzs = 0, cx = 0, cz = 0;
                        int n = 0;
                        for (size_t i = 0; i < fronts.size(); ++i)
                            if (group[i] == gi) {
                                const C& c = cells[fronts[i].c];
                                nxs += fronts[i].dx; nzs += fronts[i].dz;
                                cx += g.centerX(c.x) + fronts[i].dx * cs * 0.5f; cz += g.centerZ(c.z) + fronts[i].dz * cs * 0.5f;
                                ++n;
                            }
                        const float nl = std::hypot(nxs, nzs);
                        if (n < 3 || nl < 1e-3f) continue;
                        const float Nx = nxs / nl, Nz = nzs / nl, Tx = -Nz, Tz = Nx;
                        cx /= n; cz /= n;
                        float a0 = 1e9f, a1 = -1e9f;
                        for (size_t i = 0; i < fronts.size(); ++i)
                            if (group[i] == gi) {
                                const C& c = cells[fronts[i].c];
                                const float px = g.centerX(c.x) + fronts[i].dx * cs * 0.5f - cx, pz = g.centerZ(c.z) + fronts[i].dz * cs * 0.5f - cz;
                                a0 = std::min(a0, px * Tx + pz * Tz); a1 = std::max(a1, px * Tx + pz * Tz);
                            }
                        const float L = a1 - a0 + cs;
                        if (L < 0.35f) continue;
                        const int k = std::max(1, static_cast<int>(std::floor((L + 0.1f) / 0.6f)));
                        for (int j = 0; j < k; ++j) {
                            const float along = a0 - cs * 0.5f + (j + 0.5f) * L / k;   // evenly along the front
                            const float fx0 = cx + Tx * along, fz0 = cz + Tz * along;
                            // in from the front until the seat ends: a backrest there, or no seat
                            float depth = 0;
                            bool backed = false;
                            for (float d = 0.05f; d <= 2.4f; d += 0.05f) {
                                int qx, qz;
                                if (!g.cellOf(fx0 - Nx * d, fz0 - Nz * d, qx, qz)) break;
                                if (regionSpan(qx, qz, id)) { depth = d; continue; }
                                if (depth <= 0) continue;
                                int cxq, czq;
                                g.cellOf(fx0 - Nx * (depth), fz0 - Nz * (depth), cxq, czq);
                                const WalkSpan* last = regionSpan(cxq, czq, id);
                                float top = 0;
                                backed = last && side({cxq, czq, last}, static_cast<int>(std::lround(-Nx)), static_cast<int>(std::lround(-Nz)), top) == kBack;
                                if (!backed && last) {                 // (diagonal backs: try the two axes)
                                    backed = side({cxq, czq, last}, static_cast<int>(std::lround(-Nx * 1.5f)), 0, top) == kBack ||
                                             side({cxq, czq, last}, 0, static_cast<int>(std::lround(-Nz * 1.5f)), top) == kBack;
                                }
                                break;
                            }
                            if (!backed || depth < 0.25f) continue;
                            const float in = depth <= 1.0f ? depth * 0.5f : depth - 0.35f;   // (a lounger: against the back)
                            const float px = fx0 - Nx * in, pz = fz0 - Nz * in;
                            int qx, qz;
                            if (!g.cellOf(px, pz, qx, qz)) continue;
                            const WalkSpan* q = regionSpan(qx, qz, id);
                            if (!q) continue;
                            bool close = false;
                            for (const WorldPoint& o : have.seats) close |= std::hypot(o.pos[0] - px, o.pos[2] - pz) < 0.5f && std::fabs(o.pos[1] - q->hi) < 0.5f;
                            for (const WorldPoint& o : out.seats) close |= std::hypot(o.pos[0] - px, o.pos[2] - pz) < 0.5f && std::fabs(o.pos[1] - q->hi) < 0.3f;
                            if (close) continue;
                            WorldPoint p;
                            p.name = "Auto seat";
                            p.pos[0] = px; p.pos[1] = q->hi; p.pos[2] = pz;
                            p.yaw = yawFacing(Nx, Nz);
                            p.exit[2] = -(in + 0.3f);                  // stand up straight ahead, off the front (seat-local -Z)
                            out.seats.push_back(p);
                        }
                    }
                }
        // stools and small chairs: a small top standing free on the floor (floor most of the way round, or up to a
        // table or a desk), facing the table beside it, else the open floor
        std::vector<uint8_t> seen(g.spans.size(), 0);
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
                for (const WalkSpan* s0 = g.begin(x, z); s0 != g.end(x, z); ++s0) {
                    const size_t i0 = s0 - g.spans.data();
                    if (floorOf[i0] < -1e29f || seen[i0]) continue;
                    std::vector<std::array<int, 2>> cells;
                    std::vector<std::pair<size_t, std::array<int, 2>>> st{{i0, {x, z}}};
                    seen[i0] = 1;
                    float t = 0, fl = -1e30f, ylo = 1e30f, yhi = -1e30f;
                    while (!st.empty() && cells.size() <= 400) {
                        const auto [si, c] = st.back();
                        st.pop_back();
                        cells.push_back(c);
                        const float h = g.spans[si].hi;
                        t += h; ylo = std::min(ylo, h); yhi = std::max(yhi, h); fl = std::max(fl, floorOf[si]);
                        static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                        for (const auto& d : d4)
                            if (const WalkSpan* q = seatSpan(c[0] + d[0], c[1] + d[1], h); q && !seen[q - g.spans.data()] && std::fabs(q->hi - h) <= 0.03f) {
                                seen[q - g.spans.data()] = 1;
                                st.push_back({static_cast<size_t>(q - g.spans.data()), {c[0] + d[0], c[1] + d[1]}});
                            }
                    }
                    const Named named = namedAs(cells, [](const std::array<int, 2>& c) { return c[0]; }, [](const std::array<int, 2>& c) { return c[1]; },
                                                [&](const std::array<int, 2>&) { return ylo; });
                    if (named == kNamedOther) continue;
                    // 26-60 cm across, level (a cushion sags); named a seat: up to a lounger's size, cushions sag more
                    if (cells.size() < 7 || cells.size() > (named == kNamedSeat ? 400u : 36u) || yhi - ylo > (named == kNamedSeat ? 0.1f : 0.07f)) continue;
                    t /= cells.size();
                    float cx = 0, cz = 0, x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
                    for (const auto& c : cells) {
                        const float wx = g.centerX(c[0]), wz = g.centerZ(c[1]);
                        cx += wx; cz += wz; x0 = std::min(x0, wx); x1 = std::max(x1, wx); z0 = std::min(z0, wz); z1 = std::max(z1, wz);
                    }
                    cx /= cells.size(); cz /= cells.size();
                    const float most = named == kNamedSeat ? 2.0f : 0.55f;
                    if (x1 - x0 > most || z1 - z0 > most || t - fl > 0.52f) continue;
                    bool taken = false;
                    for (const WorldPoint& o : out.seats) taken |= std::hypot(o.pos[0] - cx, o.pos[2] - cz) < 0.6f;
                    for (const WorldPoint& o : have.seats) taken |= std::hypot(o.pos[0] - cx, o.pos[2] - cz) < 0.5f;
                    if (taken) continue;
                    int open = 0;
                    float table[2] = {0, 0}, bestOpen = -1, openDir[2] = {0, -1};
                    const float rad = std::max(x1 - x0, z1 - z0) * 0.5f + 0.25f;
                    for (int k = 0; k < 8; ++k) {
                        const float a = k * 0.785398f, dx = std::cos(a), dz = std::sin(a);
                        open += g.field(cx + dx * rad, cz + dz * rad, fl) > -0.1f;
                        for (float d = 0.3f; d <= 0.8f; d += 0.1f) {
                            int qx, qz;
                            if (!g.cellOf(cx + dx * d, cz + dz * d, qx, qz)) continue;
                            for (const WalkSpan* q = g.begin(qx, qz); q != g.end(qx, qz); ++q)
                                if (q->hi >= fl + 0.65f && q->hi <= fl + 1.15f && (q->flags & (kWalkUp | kWalkFurniture))) { table[0] += dx; table[1] += dz; }
                        }
                        float reachOut = 0;
                        while (reachOut < 3 && g.walkable(cx + dx * (rad + reachOut), cz + dz * (rad + reachOut), fl)) reachOut += 0.2f;
                        if (reachOut > bestOpen) { bestOpen = reachOut; openDir[0] = dx; openDir[1] = dz; }
                    }
                    const float tl = std::hypot(table[0], table[1]);
                    if (open < (named == kNamedSeat ? 2 : tl > 0.5f ? 4 : 6)) continue;   // (a chair pulled up to a desk: the desk's side)
                    WorldPoint p;
                    p.name = "Auto seat";
                    p.pos[0] = cx; p.pos[1] = t; p.pos[2] = cz;
                    p.yaw = tl > 0.5f ? yawFacing(table[0] / tl, table[1] / tl) : yawFacing(openDir[0], openDir[1]);
                    p.exit[2] = -0.55f;
                    out.seats.push_back(p);
                }
    }

    // ── Hotspots: places worth jumping to ──
    if (P.hotspots) {
        std::vector<std::array<float, 3>> taken;
        for (const WorldPoint& p : have.spawns) taken.push_back({p.pos[0], p.pos[1], p.pos[2]});
        for (const WorldPoint& p : have.hotspots) taken.push_back({p.pos[0], p.pos[1], p.pos[2]});
        auto free = [&](float x, float y, float z, float apart) {
            for (const auto& q : taken) if (std::hypot(q[0] - x, q[2] - z) + 2 * std::fabs(q[1] - y) < apart) return false;
            return true;
        };
        auto add = [&](float x, float y, float z, float fx, float fz) {
            if (static_cast<int>(out.hotspots.size()) >= P.maxHotspots) return;
            WorldPoint p;
            p.name = "Auto hotspot";
            p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
            p.yaw = yawFacing(fx, fz);
            out.hotspots.push_back(p);
            taken.push_back({x, y, z});
        };
        // 1. lookouts: where the area's edge is a drop with open air beyond it (a terrace's rail, a balcony)
        {
            struct Edge { float x, y, z, ox, oz; };
            std::vector<Edge> view;
            g.outline([](const float*, const float*, const float*, bool) {}, [&](const float* a, const float* b) {
                const float dx = b[0] - a[0], dz = b[2] - a[2], L = std::hypot(dx, dz);
                if (L < 1e-5f) return;
                float nx = -dz / L, nz = dx / L;
                const float mx = (a[0] + b[0]) * 0.5f, mz = (a[2] + b[2]) * 0.5f, h = (a[1] + b[1]) * 0.5f;
                if (g.field(mx + nx * 0.03f, mz + nz * 0.03f, h) > g.field(mx - nx * 0.03f, mz - nz * 0.03f, h)) { nx = -nx; nz = -nz; }
                for (float t : {r + 0.4f, r + 1.2f, r + 2.5f})
                    if (solidBetween(g, mx + nx * t, mz + nz * t, h + 0.25f, h + 2.0f) || areaFloor(g, mx + nx * t, mz + nz * t, h, 1.0f)) return;
                view.push_back({mx, h, mz, nx, nz});
            });
            std::vector<std::pair<int, size_t>> dense;                 // how much view edge is round each piece
            for (size_t i = 0; i < view.size(); ++i) {
                int n = 0;
                for (const Edge& e : view) n += std::hypot(e.x - view[i].x, e.z - view[i].z) < 1.5f && std::fabs(e.y - view[i].y) < 0.3f;
                if (n >= 12) dense.push_back({n, i});
            }
            std::sort(dense.begin(), dense.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            int lookouts = 0;
            for (const auto& [n, i] : dense) {
                if (lookouts >= 3) break;
                const Edge& e = view[i];
                const float x = e.x - e.ox * (r + 0.5f), z = e.z - e.oz * (r + 0.5f);
                if (!g.walkable(x, z, e.y) || !free(x, e.y, z, 4.0f)) continue;
                add(x, e.y, z, e.ox, e.oz);
                ++lookouts;
            }
        }
        // 2. the middles of open spaces (1.5 m or more from any edge, 6 m apart, five at most), facing the longest view
        {
            std::vector<std::array<float, 4>> cand;
            for (int z = 0; z < g.nz; z += 2)
                for (int x = 0; x < g.nx; x += 2)
                    for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
                        if ((s->flags & kWalkOk) && s->dist >= 150) cand.push_back({g.centerX(x), s->hi, g.centerZ(z), s->dist * 0.01f});
            std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a[3] > b[3]; });
            int centres = 0;
            for (const auto& c : cand) {
                if (static_cast<int>(out.hotspots.size()) >= P.maxHotspots || centres >= 5) break;
                if (!free(c[0], c[1], c[2], 6.0f)) continue;
                ++centres;
                float best = -1, fx = 0, fz = -1;
                for (int k = 0; k < 16; ++k) {
                    const float a = k * 0.392699f, dx = std::cos(a), dz = std::sin(a);
                    float d = 0;
                    while (d < 15 && g.walkable(c[0] + dx * (d + 0.25f), c[2] + dz * (d + 0.25f), c[1])) d += 0.25f;
                    if (d > best) { best = d; fx = dx; fz = dz; }
                }
                add(c[0], c[1], c[2], fx, fz);
            }
        }
    }
    return out;
}
