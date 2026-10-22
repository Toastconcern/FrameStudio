#pragma once
// The world layer drawn over the home in the headset (the Debug page's "World layer"): the walkable area as Frame
// Home walks it, the surfaces and colliders, spawns, seats, hotspots and gravity zones, in the colours
// Frame Studio uses for them. Built once per home as guide geometry (guide_geom.h) and appended to the guide each
// frame. Lines are thin crossed ribbons, so they read from any side without being rebuilt as you move.
#include "guide_geom.h"
#include "walkgrid.h"

#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace debuggeom {

using guide::C;
using guide::P;

// A segment as two ribbons at right angles (w wide): visible edge-on from anywhere.
inline void line(std::vector<GuideVertex>& o, P a, P b, C c, float w = 0.012f) {
    const P d = guide::norm(b - a);
    P u = guide::norm(guide::cross(d, std::fabs(d.y) < 0.9f ? P{0, 1, 0} : P{1, 0, 0}));
    P v = guide::cross(d, u);
    u = u * (w * 0.5f); v = v * (w * 0.5f);
    guide::quad(o, a - u, c, b - u, c, b + u, c, a + u, c);
    guide::quad(o, a - v, c, b - v, c, b + v, c, a + v, c);
}

inline P xf(const float m[16], P p) {   // column-major frame
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

inline void box(std::vector<GuideVertex>& o, const float m[16], const float h[3], C c) {
    P k[8];
    for (int i = 0; i < 8; ++i) k[i] = xf(m, {(i & 1) ? h[0] : -h[0], (i & 2) ? h[1] : -h[1], (i & 4) ? h[2] : -h[2]});
    static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& ed : e) line(o, k[ed[0]], k[ed[1]], c);
}

inline void circle(std::vector<GuideVertex>& o, P c, float r, C col, int segs = 32) {
    for (int i = 0; i < segs; ++i) {
        const float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        line(o, c + P{std::cos(a0) * r, 0, std::sin(a0) * r}, c + P{std::cos(a1) * r, 0, std::sin(a1) * r}, col);
    }
}

// A marker on the floor: a ring and an arrow the way it faces (yaw 0 faces -Z), and a post to find it from afar.
inline void marker(std::vector<GuideVertex>& o, const WorldPoint& p, float r, float post, C c) {
    const P at{p.pos[0], p.pos[1] + 0.02f, p.pos[2]};
    circle(o, at, r, c);
    const P f{-std::sin(p.yaw), 0, -std::cos(p.yaw)};
    line(o, at, at + f * (r * 1.6f), c);
    if (post > 0) line(o, at, at + P{0, post, 0}, guide::alpha(c, 0.6f));
}

// Colours as Frame Studio draws them (linear, straight alpha).
constexpr C kWalk{0.15f, 0.85f, 0.35f, 0.32f}, kWalkEdge{0.25f, 1.0f, 0.45f, 0.9f}, kBridged{0.95f, 0.75f, 0.2f, 0.25f};
constexpr C kSolid{0.95f, 0.95f, 0.95f, 0.55f}, kObject{1.0f, 0.55f, 0.25f, 0.7f}, kTrigger{0.6f, 0.6f, 1.0f, 0.45f}, kWalkable{0.3f, 1.0f, 0.5f, 0.7f};
constexpr C kSpawn{0.3f, 0.75f, 1.0f, 0.95f}, kSeat{1.0f, 0.8f, 0.25f, 0.95f}, kHotspot{0.85f, 0.45f, 1.0f, 0.95f};
constexpr C kGravity{1.0f, 0.35f, 0.6f, 0.6f};

// Everything, in world space. Mesh surfaces are drawn as their boundary-free wire (every edge) only up to a budget:
// a mesh collider can be a whole room's geometry.
inline void build(const HomeWorld& w, const WalkGrid* g, std::vector<GuideVertex>& o) {
    o.clear();
    if (g) {
        // (6 cm up: the walkable surface can be the collision under a carpet, and level with the carpet it flickers.)
        // The floor you can stand on, cell by cell, each corner at the height the cells around it share (a ramp or a
        // raked floor is a slope, not steps), and runs along x of flat cells at one height as one quad (a flat floor is
        // a few hundred quads, not one per cell); its edge as the smooth line you walk to.
        const float cs = g->p.cell, lift = 0.06f;
        auto corner = [&](int cx, int cz, float y) {   // the corner at the low x / low z side of cell (cx, cz)
            float sum = 0;
            int n = 0;
            for (int j = -1; j <= 0; ++j)
                for (int i = -1; i <= 0; ++i)
                    if (g->inside(cx + i, cz + j))
                        if (const WalkSpan* q = g->walkNear(cx + i, cz + j, y, g->p.step)) { sum += q->hi; ++n; }
            return n ? sum / n : y;
        };
        for (int z = 0; z < g->nz; ++z) {
            struct Run { int x0, x1; float h; bool bridged; };
            std::vector<Run> open, next;
            const float z0 = g->oz + z * cs, z1 = g->oz + (z + 1) * cs;   // (one way for both neighbours, or a seam shows)
            auto flush = [&](const Run& r) {
                const float x0 = g->ox + r.x0 * cs, x1 = g->ox + (r.x1 + 1) * cs, y = r.h + lift;
                const C col = r.bridged ? kBridged : kWalk;
                guide::quad(o, {x0, y, z0}, col, {x1, y, z0}, col, {x1, y, z1}, col, {x0, y, z1}, col);
            };
            for (int x = 0; x < g->nx; ++x) {
                next.clear();
                for (const WalkSpan* sp = g->begin(x, z); sp != g->end(x, z); ++sp) {
                    if (!(sp->flags & (kWalkOk | kWalkNear)) || !g->walkable(g->centerX(x), g->centerZ(z), sp->hi)) continue;
                    const bool br = (sp->flags & kWalkBridged) != 0;
                    const float h00 = corner(x, z, sp->hi), h10 = corner(x + 1, z, sp->hi), h11 = corner(x + 1, z + 1, sp->hi), h01 = corner(x, z + 1, sp->hi);
                    const bool flat = std::fabs(h00 - h10) < 0.002f && std::fabs(h00 - h11) < 0.002f && std::fabs(h00 - h01) < 0.002f;
                    if (!flat) {   // a slope: this cell alone, its corners where they are
                        const float x0 = g->ox + x * cs, x1 = g->ox + (x + 1) * cs;
                        const C col = br ? kBridged : kWalk;
                        guide::quad(o, {x0, h00 + lift, z0}, col, {x1, h10 + lift, z0}, col, {x1, h11 + lift, z1}, col, {x0, h01 + lift, z1}, col);
                        continue;
                    }
                    bool joined = false;
                    for (Run& r : open)
                        if (r.x1 == x - 1 && std::fabs(r.h - h00) < 0.002f && r.bridged == br) { r.x1 = x; next.push_back(r); r.x1 = -2; joined = true; break; }
                    if (!joined) next.push_back({x, x, h00, br});
                }
                for (const Run& r : open) if (r.x1 != -2) flush(r);   // ended at x - 1
                open.swap(next);
            }
            for (const Run& r : open) flush(r);
        }
        g->outline([](const float*, const float*, const float*, bool) {},
                   [&](const float* a, const float* b) {   // lying flat: it's seen from above
                       const P pa{a[0], a[1] + 0.07f, a[2]}, pb{b[0], b[1] + 0.07f, b[2]};
                       const P u = guide::norm(guide::cross(pb - pa, P{0, 1, 0})) * 0.012f;
                       guide::quad(o, pa - u, kWalkEdge, pb - u, kWalkEdge, pb + u, kWalkEdge, pa + u, kWalkEdge);
                   });
    }
    size_t meshEdges = 0;
    for (const WorldCollider& c : w.colliders) {
        const C col = c.role == WorldCollider::Trigger ? kTrigger : c.role == WorldCollider::Object ? kObject : c.role == WorldCollider::Walkable ? kWalkable : kSolid;
        switch (c.kind) {
            case WorldCollider::Box: box(o, c.world, c.half, col); break;
            case WorldCollider::Sphere: {
                const P ctr = xf(c.world, {0, 0, 0});
                circle(o, ctr, c.radius, col);
                for (int i = 0; i < 24; ++i) {   // and a vertical ring
                    const float a0 = 6.2831853f * i / 24, a1 = 6.2831853f * (i + 1) / 24;
                    line(o, ctr + P{std::cos(a0) * c.radius, std::sin(a0) * c.radius, 0}, ctr + P{std::cos(a1) * c.radius, std::sin(a1) * c.radius, 0}, col);
                }
                break;
            }
            case WorldCollider::Capsule: {
                const P a = xf(c.world, {0, -c.length * 0.5f, 0}), b = xf(c.world, {0, c.length * 0.5f, 0});
                circle(o, a, c.radius, col); circle(o, b, c.radius, col); line(o, a, b, col);
                break;
            }
            default: {   // each edge once (neighbouring triangles share them), as one ribbon facing up
                const std::vector<float> wp = worldMeshPositions(c);
                std::unordered_set<uint64_t> seen;
                auto edge = [&](u32 i0, u32 i1) {
                    if (i0 > i1) std::swap(i0, i1);
                    if (meshEdges >= 25000 || static_cast<size_t>(i1) * 3 + 2 >= wp.size() || !seen.insert((static_cast<uint64_t>(i0) << 32) | i1).second) return;
                    ++meshEdges;
                    const P a{wp[i0 * 3], wp[i0 * 3 + 1], wp[i0 * 3 + 2]}, b{wp[i1 * 3], wp[i1 * 3 + 1], wp[i1 * 3 + 2]};
                    const P d = guide::norm(b - a);
                    const P u = guide::norm(guide::cross(d, std::fabs(d.y) < 0.9f ? P{0, 1, 0} : P{1, 0, 0})) * 0.004f;
                    const C faint = guide::alpha(col, 0.45f);   // a whole room's triangles: quieter than the boxes
                    guide::quad(o, a - u, faint, b - u, faint, b + u, faint, a + u, faint);
                };
                for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                    edge(c.indices[t], c.indices[t + 1]); edge(c.indices[t + 1], c.indices[t + 2]); edge(c.indices[t + 2], c.indices[t]);
                }
                break;
            }
        }
    }
    for (const WorldPoint& p : w.spawns) marker(o, p, 0.3f, 1.8f, kSpawn);
    for (const WorldPoint& p : w.hotspots) marker(o, p, 0.25f, 1.2f, kHotspot);
    for (const WorldPoint& p : w.seats) {
        marker(o, p, 0.18f, 0, kSeat);
        const P at{p.pos[0], p.pos[1], p.pos[2]};
        const P ex{p.pos[0] + p.exit[0], p.pos[1] + p.exit[1] + 0.02f, p.pos[2] + p.exit[2]};
        if (p.exit[0] != 0 || p.exit[2] != 0) { line(o, at, ex, guide::alpha(kSeat, 0.5f)); circle(o, ex, 0.12f, guide::alpha(kSeat, 0.7f)); }
    }
    for (const WorldGravity& z : w.gravity) {
        if (z.kind == WorldGravity::Planet) circle(o, xf(z.world, {0, 0, 0}), z.radius, kGravity, 64);
        else box(o, z.world, z.half, kGravity);
    }
}

}  // namespace debuggeom
