// Navmeshes: the walkable area as meshes you can edit (WorldCollider::Navmesh). A home with any is walked on them
// alone (bakeWalkGrid); Frame Studio makes them from the baked area (navmeshFromWalkGrid) or a new square of one, and
// their corners are edited like any mesh collider's.
#pragma once
#include "walkgrid.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

// One separately walkable area as a mesh: world-space triangles facing up.
struct NavmeshPiece {
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    double area = 0;                    // m² (of the cells it was made from)
};

// The walkable area as navmesh pieces: each walkable cell's top, joined to its neighbours where you walk from one to
// the next (a ramp's: their corners meet; a step's: each keeps its own edge, its faces stay level), each area that's walkable apart its
// own piece, simplified so its outline and its heights stay within `error` (m). Pieces under minArea (m²) are left out.
inline std::vector<NavmeshPiece> navmeshFromWalkGrid(const WalkGrid& g, float error = 0.03f, double minArea = 0.05) {
    std::vector<NavmeshPiece> out;
    if (g.nx <= 0 || g.nz <= 0 || g.spans.empty()) return out;
    const float cs = g.p.cell;
    // the walkable spans, and where each is
    std::vector<int32_t> spanOf(g.spans.size(), -1);   // span index -> walkable number
    std::vector<int> sx, sz;
    std::vector<float> sh;
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
            for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                if (!(s->flags & kWalkOk)) continue;
                spanOf[static_cast<size_t>(s - g.spans.data())] = static_cast<int32_t>(sx.size());
                sx.push_back(x);
                sz.push_back(z);
                sh.push_back(s->hi);
            }
    const size_t n = sx.size();
    if (!n) return out;
    auto walkAt = [&](int x, int z, float y) -> int32_t {   // the walkable span you step onto there, or -1
        const WalkSpan* s = g.walkNear(x, z, y, g.p.step, kWalkOk);
        return s ? spanOf[static_cast<size_t>(s - g.spans.data())] : -1;
    };
    // the areas (spans you walk between) and the corners they share: 4 a span (0: x z, 1: x+1 z, 2: x+1 z+1, 3: x z+1)
    std::vector<uint32_t> area(n), corner(n * 4);
    std::iota(area.begin(), area.end(), 0u);
    std::iota(corner.begin(), corner.end(), 0u);
    auto root = [](std::vector<uint32_t>& p, uint32_t x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; };
    constexpr float kJoin = 0.06f;   // (neighbours this close in height share their corners: a ramp, a floor's bumps)
    auto join = [&](std::vector<uint32_t>& p, uint32_t a, uint32_t b) { a = root(p, a); b = root(p, b); if (a != b) p[std::max(a, b)] = std::min(a, b); };
    for (size_t i = 0; i < n; ++i) {
        const int32_t r = walkAt(sx[i] + 1, sz[i], sh[i]);
        if (r >= 0) {
            join(area, static_cast<uint32_t>(i), static_cast<uint32_t>(r));
            if (std::fabs(sh[i] - sh[r]) <= kJoin) { join(corner, i * 4 + 1, r * 4 + 0); join(corner, i * 4 + 2, r * 4 + 3); }
        }
        const int32_t d = walkAt(sx[i], sz[i] + 1, sh[i]);
        if (d >= 0) {
            join(area, static_cast<uint32_t>(i), static_cast<uint32_t>(d));
            if (std::fabs(sh[i] - sh[d]) <= kJoin) { join(corner, i * 4 + 3, d * 4 + 0); join(corner, i * 4 + 2, d * 4 + 1); }
        }
    }
    // each joined corner's height: the mean of the spans' round it
    std::vector<double> hsum(n * 4, 0);
    std::vector<uint32_t> hcount(n * 4, 0);
    for (size_t c = 0; c < n * 4; ++c) { const uint32_t r = root(corner, static_cast<uint32_t>(c)); hsum[r] += sh[c / 4]; ++hcount[r]; }
    // each area's mesh
    std::unordered_map<uint32_t, size_t> pieceOf;
    std::vector<std::unordered_map<uint32_t, uint32_t>> vertexOf;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t a = root(area, static_cast<uint32_t>(i));
        auto it = pieceOf.find(a);
        if (it == pieceOf.end()) { it = pieceOf.emplace(a, out.size()).first; out.emplace_back(); vertexOf.emplace_back(); }
        NavmeshPiece& p = out[it->second];
        std::unordered_map<uint32_t, uint32_t>& vo = vertexOf[it->second];
        uint32_t v[4];
        for (int k = 0; k < 4; ++k) {
            const uint32_t r = root(corner, static_cast<uint32_t>(i * 4 + k));
            auto vt = vo.find(r);
            if (vt == vo.end()) {
                vt = vo.emplace(r, static_cast<uint32_t>(p.pos.size() / 3)).first;
                const int cx = sx[i] + (k == 1 || k == 2), cz = sz[i] + (k >= 2);
                p.pos.push_back(g.ox + cx * cs);
                p.pos.push_back(static_cast<float>(hsum[r] / std::max(1u, hcount[r])));
                p.pos.push_back(g.oz + cz * cs);
            }
            v[k] = vt->second;
        }
        // (wound to face up)
        for (uint32_t q : {v[0], v[2], v[1], v[0], v[3], v[2]}) p.idx.push_back(q);
        p.area += static_cast<double>(cs) * cs;
    }
    // simplified: the cells' staircase and their flat runs taken down to what keeps the shape within `error`
    for (NavmeshPiece& p : out) {
        if (p.idx.size() < 6) continue;
        std::vector<uint32_t> s(p.idx.size());
        const size_t m = meshopt_simplify(s.data(), p.idx.data(), p.idx.size(), p.pos.data(), p.pos.size() / 3, 12, 0, error,
                                          meshopt_SimplifyErrorAbsolute, nullptr);
        if (m < 3) continue;
        s.resize(m);
        std::vector<uint32_t> remap(p.pos.size() / 3, UINT32_MAX);
        std::vector<float> pos;
        for (uint32_t& x : s) {
            if (remap[x] == UINT32_MAX) { remap[x] = static_cast<uint32_t>(pos.size() / 3); pos.insert(pos.end(), &p.pos[x * 3], &p.pos[x * 3] + 3); }
            x = remap[x];
        }
        p.pos.swap(pos);
        p.idx.swap(s);
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](const NavmeshPiece& p) { return p.area < minArea || p.idx.size() < 3; }), out.end());
    std::sort(out.begin(), out.end(), [](const NavmeshPiece& a, const NavmeshPiece& b) { return a.area > b.area; });
    return out;
}

// A shape drawn corner by corner (each at its own height) as faces facing up: ear clipping in plan (x, z), so a shape
// that isn't convex (an L, a U) fills right. False: fewer than three corners, or no ear left (it crosses itself).
inline bool navmeshFromPolygon(const std::vector<std::array<float, 3>>& pts, std::vector<float>& pos, std::vector<uint32_t>& idx) {
    pos.clear();
    idx.clear();
    const size_t n = pts.size();
    if (n < 3) return false;
    for (const auto& p : pts) pos.insert(pos.end(), {p[0], p[1], p[2]});
    // in plan, counter-clockwise seen from above (x right, z down the screen: the signed area's sign flips)
    double a2 = 0;
    for (size_t i = 0; i < n; ++i) { const auto& p = pts[i]; const auto& q = pts[(i + 1) % n]; a2 += static_cast<double>(p[0]) * q[2] - static_cast<double>(q[0]) * p[2]; }
    if (std::fabs(a2) < 1e-8) return false;
    std::vector<uint32_t> ring(n);
    for (size_t i = 0; i < n; ++i) ring[i] = static_cast<uint32_t>(a2 > 0 ? i : n - 1 - i);
    auto cross = [&](uint32_t a, uint32_t b, uint32_t c) {
        return (static_cast<double>(pts[b][0]) - pts[a][0]) * (static_cast<double>(pts[c][2]) - pts[a][2]) -
               (static_cast<double>(pts[b][2]) - pts[a][2]) * (static_cast<double>(pts[c][0]) - pts[a][0]);
    };
    auto inside = [&](uint32_t p, uint32_t a, uint32_t b, uint32_t c) { return cross(a, b, p) > 0 && cross(b, c, p) > 0 && cross(c, a, p) > 0; };
    while (ring.size() > 3) {
        bool cut = false;
        for (size_t i = 0; i < ring.size() && !cut; ++i) {
            const uint32_t a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i], c = ring[(i + 1) % ring.size()];
            if (cross(a, b, c) <= 1e-12) continue;   // (a reflex corner, or a straight one)
            bool ear = true;
            for (uint32_t p : ring) if (p != a && p != b && p != c && inside(p, a, b, c)) { ear = false; break; }
            if (!ear) continue;
            idx.insert(idx.end(), {a, b, c});
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
            cut = true;
        }
        if (!cut) { idx.clear(); return false; }
    }
    idx.insert(idx.end(), {ring[0], ring[1], ring[2]});
    // (counter-clockwise in x z is clockwise seen from above: turned to face up)
    for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
    return true;
}
