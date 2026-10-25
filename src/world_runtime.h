#pragma once
// The world layer at run time: what the player stands on and bumps into. Built once per home from HomeWorld:
//   floors — upward faces of floor + walls surfaces, everything not too steep on walkable floors, box tops
//   walls  — steep faces of floor + walls surfaces, every face of wall surfaces (a Wall part is all wall, whatever
//            its slope: Studio's Wall + Floor split leaves 40-45 degree faces in it), plus box / sphere / capsule
//            colliders that block
// (each collider's role says which it gives: see WorldCollider::Role; triggers give neither).
// Both sit in a 1 m grid over XZ, so a query touches a handful of triangles instead of hundreds of thousands.
// A world with a walkable area (HomeWorld::walkGrid / walkGridCache, src/walkgrid.h) answers from it instead: floors,
// walking, edges and teleport landings all come from the one baked area (seams bridged, furniture and ledges
// blocked, the body's radius kept from walls), so Frame Home walks exactly what Frame Studio shows. Box / sphere /
// capsule colliders still report which one a teleport arc hit (seats).
// Frame Home walks and teleports with it; Frame Studio previews the same answers.
#include "core/world.h"
#include "walkgrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

class WorldRuntime {
public:
    struct Tri { float a[3], b[3], c[3]; float n[3]; bool solid = false; };   // solid: a floor that blocks from below

    void build(const HomeWorld& w) {
        floors_.clear(); walls_.clear(); floorGrid_.clear(); wallGrid_.clear(); shapes_.clear();
        grid_ = w.walkGridCache;
        if (!grid_ && !w.walkGrid.empty()) grid_ = decodeWalkGrid(w.walkGrid);
        if (grid_ && grid_->any()) {
            for (const WorldCollider& c : w.colliders)
                if (c.kind != WorldCollider::Mesh && (c.role == WorldCollider::Solid || c.role == WorldCollider::Wall || c.role == WorldCollider::Object)) shapes_.push_back(c);
            hasFloor_ = true;
            return;
        }
        grid_.reset();
        // floorUp: how upright a face must be to stand on (0 = never floor); walls: steep faces block.
        // allWall: every face blocks (the Wall role), not just the steep ones.
        auto addMesh = [&](const std::vector<float>& p, const std::vector<u32>& idx, float floorUp, bool walls, bool solidFloor, bool allWall = false) {
            const size_t nv = p.size() / 3;
            for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                if (idx[t] >= nv || idx[t + 1] >= nv || idx[t + 2] >= nv) continue;
                Tri tr;
                std::copy_n(&p[idx[t] * 3], 3, tr.a); std::copy_n(&p[idx[t + 1] * 3], 3, tr.b); std::copy_n(&p[idx[t + 2] * 3], 3, tr.c);
                const float e1[3] = {tr.b[0] - tr.a[0], tr.b[1] - tr.a[1], tr.b[2] - tr.a[2]}, e2[3] = {tr.c[0] - tr.a[0], tr.c[1] - tr.a[1], tr.c[2] - tr.a[2]};
                float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (L < 1e-9f) continue;
                for (float& v : n) v /= L;
                std::copy_n(n, 3, tr.n);
                tr.solid = solidFloor;
                const float up = std::fabs(n[1]);                      // winding varies: a floor faces up either way
                if (floorUp > 0 && up > floorUp) insert(floors_, floorGrid_, tr);
                else if (walls && (allWall || up < 0.7f)) insert(walls_, wallGrid_, tr);
            }
        };
        if (!w.walkIndices.empty()) addMesh(w.walkPositions, w.walkIndices, 0.01f, false, false);   // legacy walkable triangles
        for (const WorldCollider& c : w.colliders) {
            if (c.role == WorldCollider::Trigger) continue;
            const float floorUp = c.role == WorldCollider::Solid ? 0.7f : c.role == WorldCollider::Floor || c.role == WorldCollider::Walkable ? 0.5f : 0.0f;
            const bool blocks = c.role == WorldCollider::Solid || c.role == WorldCollider::Wall || c.role == WorldCollider::Object;
            if (c.kind == WorldCollider::Mesh) { addMesh(worldMeshPositions(c), c.indices, floorUp, blocks, blocks, c.role == WorldCollider::Wall || c.role == WorldCollider::Object); continue; }
            if (blocks) shapes_.push_back(c);
            if (c.kind == WorldCollider::Box && floorUp > 0) {         // a box's top is floor (steps, platforms, rugs)
                std::vector<float> p;
                const float s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (auto& q : s) {
                    const float l[3] = {q[0] * c.half[0], c.half[1], q[1] * c.half[2]};
                    for (int k = 0; k < 3; ++k) p.push_back(c.world[k] * l[0] + c.world[4 + k] * l[1] + c.world[8 + k] * l[2] + c.world[12 + k]);
                }
                addMesh(p, {0, 1, 2, 0, 2, 3}, 0.01f, false, blocks);
            }
        }
        hasFloor_ = !floors_.empty();
    }

    bool hasFloor() const { return hasFloor_; }
    const WalkGrid* grid() const { return grid_.get(); }        // the walkable area answering, if any

    // Highest floor under (x, z) between y - maxDown and y + maxUp; false when there is none.
    bool floorAt(float x, float z, float y, float maxUp, float maxDown, float& out) const {
        if (grid_) return grid_->floorAt(x, z, y, maxUp, maxDown, out);
        bool found = false;
        float best = -1e30f;
        forCell(floorGrid_, x, z, [&](uint32_t i) {
            const Tri& t = floors_[i];
            float h;
            if (!heightOn(t, x, z, h) || h > y + maxUp || h < y - maxDown) return;
            if (h > best) { best = h; found = true; }
        });
        if (found) out = best;
        return found;
    }

    // Moves a standing body (feet at y, `radius` wide, `height` tall) from (x, z) by (dx, dz), stopping at walls
    // and sliding along them.
    void move(float& x, float& z, float y, float dx, float dz, float radius = 0.2f, float height = 1.7f) const {
        if (grid_) { grid_->move(x, z, y, dx, dz); return; }        // (the area keeps the body's radius from edges)
        for (int pass = 0; pass < 2; ++pass) {                       // try the full step, then slide
            const float nx = x + dx, nz = z + dz;
            float push[3];
            if (!blocked(nx, y, nz, radius, height, push)) { x = nx; z = nz; return; }
            // remove the component of the step that goes into the wall
            const float pl = std::sqrt(push[0] * push[0] + push[2] * push[2]);
            if (pl < 1e-6f) return;
            const float px = push[0] / pl, pz = push[2] / pl, into = dx * px + dz * pz;
            if (into >= 0) return;
            dx -= into * px; dz -= into * pz;
        }
    }

    // First floor hit along a segment (teleport arcs are traced as short segments).
    bool raycastFloor(const float o[3], const float d[3], float maxT, float hit[3]) const {
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-9f) return false;
        const int steps = std::max(1, static_cast<int>(maxT * len / 0.5f));
        float bestT = 1e30f;
        for (int s = 0; s <= steps; ++s) {                           // visit the cells the segment crosses
            const float t = maxT * s / steps;
            const float x = o[0] + d[0] * t, z = o[2] + d[2] * t;
            forCell(floorGrid_, x, z, [&](uint32_t i) {
                float th;
                if (rayTri(o, d, floors_[i], th) && th >= 0 && th <= maxT && th < bestT) bestT = th;
            });
        }
        if (bestT > maxT) return false;
        for (int k = 0; k < 3; ++k) hit[k] = o[k] + d[k] * bestT;
        // a wall in front of the landing blocks it
        return !wallBetween(o, hit);
    }

    // What a straight segment o + d t, t in [0, 1], meets first: a floor from above (Land), anything solid — walls,
    // ceilings and the undersides of solid floors, blocking shapes — (Block), or nothing. Teleport arcs are traced
    // with it segment by segment, so they stop at the first wall instead of flying through it. n: the surface's
    // normal facing the segment's start.
    enum Hit { None = 0, Land = 1, Block = 2 };
    // hitShape (optional): the box / sphere / capsule collider hit, when that's what stopped it.
    Hit trace(const float o[3], const float d[3], float& tOut, float n[3], const WorldCollider** hitShape = nullptr) const {
        if (hitShape) *hitShape = nullptr;
        float best = 2;
        Hit kind = None;
        if (grid_) {
            kind = static_cast<Hit>(grid_->trace(o, d, best, n));
            for (const WorldCollider& c : shapes_) {                   // which chair it was (seats)
                float th, sn[3];
                if (!rayShape(c, o, d, th, sn) || th > 1) continue;
                if (th < best - 1e-4f || (kind != Land && th < best)) { best = th; kind = Block; std::copy_n(sn, 3, n); if (hitShape) *hitShape = &c; }
            }
            tOut = best;
            return kind;
        }
        auto consider = [&](const Tri& tr, bool floor) {
            float th;
            if (!rayTri(o, d, tr, th) || th < 0 || th > 1 || th >= best) return;
            const float side = tr.n[0] * d[0] + tr.n[1] * d[1] + tr.n[2] * d[2] < 0 ? 1.0f : -1.0f;
            const float fn[3] = {tr.n[0] * side, tr.n[1] * side, tr.n[2] * side};
            Hit k = Block;
            if (floor) {
                if (fn[1] > 0) k = Land;                               // onto it from above
                else if (!tr.solid) return;                            // under a walkable area: it doesn't stop you
            }
            best = th; kind = k;
            std::copy_n(fn, 3, n);
        };
        walkCells(o, d, [&](int64_t key) {
            if (auto it = floorGrid_.find(key); it != floorGrid_.end()) for (uint32_t i : it->second) consider(floors_[i], true);
            if (auto it = wallGrid_.find(key); it != wallGrid_.end()) for (uint32_t i : it->second) consider(walls_[i], false);
        });
        for (const auto* g : {&floorGrid_, &wallGrid_}) {
            auto it = g->find(kBig);
            if (it == g->end()) continue;
            for (uint32_t i : it->second) consider(g == &floorGrid_ ? floors_[i] : walls_[i], g == &floorGrid_);
        }
        // Blocking shapes. A box's top is also a floor triangle: landing on it wins a tie.
        for (const WorldCollider& c : shapes_) {
            float th, sn[3];
            if (!rayShape(c, o, d, th, sn) || th > 1) continue;
            if (th < best - 1e-4f || (kind != Land && th < best)) { best = th; kind = Block; std::copy_n(sn, 3, n); if (hitShape) *hitShape = &c; }
        }
        tOut = best;
        return kind;
    }

    // Where a teleport arc that met something at q (h: what it was, n: its normal facing the arc) puts you, into out:
    //   a floor from above: there, or the nearest place you fit within 30 cm (an edge, a seam);
    //   the top of something you can't stand on (a table, a sofa's back): the floor beside it;
    //   a wall, a railing, furniture's side, at any height: the floor at its foot (in front of it);
    //   a ceiling or a floor's underside: the floor under it.
    // Each with headroom over it. False: nowhere you fit within reach of it.
    bool landingFor(Hit h, const float q[3], const float n[3], float out[3]) const {
        auto standAt = [&](float x, float y, float z, float nudge) {
            if (!roomAt(x, y, z, nudge)) return false;
            float f;
            if (floorAt(x, z, y, 0.1f, 0.1f, f)) y = f;
            const float from[3] = {x, y + 0.05f, z}, up[3] = {0, 1.2f, 0};
            float t, hn[3];
            if (trace(from, up, t, hn) == Block) return false;         // no headroom (under a table)
            out[0] = x; out[1] = y; out[2] = z;
            return true;
        };
        float f;
        if (h == Land) {
            if (standAt(q[0], q[1], q[2], 0.3f)) return true;
            return floorAt(q[0], q[2], q[1] - 0.2f, 0.0f, 2.0f, f) && standAt(q[0], f, q[2], 0.6f);   // a top: the floor by it
        }
        if (h != Block) return false;
        if (n[1] < -0.7f) return floorAt(q[0], q[2], q[1] - 0.1f, 0.0f, 3.0f, f) && standAt(q[0], f, q[2], 0.3f);   // under a ceiling
        const float hl = std::sqrt(n[0] * n[0] + n[2] * n[2]);
        if (hl < 1e-4f) return false;
        const float x = q[0] + n[0] / hl * 0.3f, z = q[2] + n[2] / hl * 0.3f;   // a wall or a side: in front of its foot
        return floorAt(x, z, q[1], 0.0f, 3.0f, f) && standAt(x, f, z, 0.3f);
    }

    // Somewhere to stand at (x, y, z): pushed up to `nudge` metres out of walls the body would be in (landing at the
    // foot of a wall or beside a sofa), still on a floor within a step. False: no room.
    bool roomAt(float& x, float y, float& z, float nudge = 0.3f) const {
        if (grid_) return grid_->roomAt(x, y, z, nudge);
        float push[3];
        for (int i = 0; i < 3; ++i) {
            if (!blocked(x, y, z, 0.2f, 1.7f, push)) return true;
            const float pl = std::sqrt(push[0] * push[0] + push[2] * push[2]);
            if (pl < 1e-6f) return false;
            const float nx = x + push[0] / pl * nudge / 3, nz = z + push[2] / pl * nudge / 3;
            float f;
            if (!floorAt(nx, nz, y, 0.1f, 0.1f, f)) return false;
            x = nx; z = nz;
        }
        return !blocked(x, y, z, 0.2f, 1.7f, push);
    }

private:
    std::vector<Tri> floors_, walls_;
    std::unordered_map<int64_t, std::vector<uint32_t>> floorGrid_, wallGrid_;
    std::vector<WorldCollider> shapes_;
    std::shared_ptr<const WalkGrid> grid_;
    bool hasFloor_ = false;

    static int64_t key(int cx, int cz) { return static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32 | static_cast<uint32_t>(cz)); }
    // (clamped, NaN as 0: a damaged file's 1e30 or NaN vertex must not overflow the int, or a cell loop ending at
    // INT_MAX never ends)
    static int cell(float v) {
        const float c = std::floor(v);
        return c > -1e7f && c < 1e7f ? static_cast<int>(c) : c >= 1e7f ? 10000000 : c <= -1e7f ? -10000000 : 0;
    }

    static void insert(std::vector<Tri>& list, std::unordered_map<int64_t, std::vector<uint32_t>>& grid, const Tri& t) {
        const uint32_t id = static_cast<uint32_t>(list.size());
        list.push_back(t);
        const float x0 = std::min({t.a[0], t.b[0], t.c[0]}), x1 = std::max({t.a[0], t.b[0], t.c[0]});
        const float z0 = std::min({t.a[2], t.b[2], t.c[2]}), z1 = std::max({t.a[2], t.b[2], t.c[2]});
        if (x1 - x0 > 400 || z1 - z0 > 400) { grid[kBig].push_back(id); return; }   // one huge ground quad: every query sees it
        for (int cx = cell(x0); cx <= cell(x1); ++cx)
            for (int cz = cell(z0); cz <= cell(z1); ++cz) grid[key(cx, cz)].push_back(id);
    }

    static constexpr int64_t kBig = INT64_MIN;                       // triangles too big to grid
    template <typename F> static void forCell(const std::unordered_map<int64_t, std::vector<uint32_t>>& grid, float x, float z, F&& f) {
        auto it = grid.find(key(cell(x), cell(z)));
        if (it != grid.end()) for (uint32_t i : it->second) f(i);
        auto big = grid.find(kBig);
        if (big != grid.end()) for (uint32_t i : big->second) f(i);
    }

    // Every grid cell the segment o + d t, t in [0, 1], crosses over XZ (Amanatides & Woo), each once.
    template <typename F> static void walkCells(const float o[3], const float d[3], F&& f) {
        int cx = cell(o[0]), cz = cell(o[2]);
        const int ex = cell(o[0] + d[0]), ez = cell(o[2] + d[2]);
        const int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
        const float ix = std::fabs(d[0]) > 1e-9f ? 1 / std::fabs(d[0]) : 1e30f, iz = std::fabs(d[2]) > 1e-9f ? 1 / std::fabs(d[2]) : 1e30f;
        float tx = std::fabs(d[0]) > 1e-9f ? ((sx > 0 ? cx + 1 : cx) - o[0]) / d[0] : 1e30f;
        float tz = std::fabs(d[2]) > 1e-9f ? ((sz > 0 ? cz + 1 : cz) - o[2]) / d[2] : 1e30f;
        for (int guard = 0; guard < 4096; ++guard) {
            f(key(cx, cz));
            if (cx == ex && cz == ez) break;
            if (tx < tz) { if (tx > 1) break; tx += ix; cx += sx; }
            else { if (tz > 1) break; tz += iz; cz += sz; }
        }
    }

    // A segment against a box / sphere / capsule collider (in its frame, as blocked() reads it).
    static bool rayShape(const WorldCollider& c, const float o[3], const float d[3], float& t, float n[3]) {
        float lo[3], ld[3], s2[3];
        const float rel[3] = {o[0] - c.world[12], o[1] - c.world[13], o[2] - c.world[14]};
        for (int a = 0; a < 3; ++a) {
            const float* ax = &c.world[a * 4];
            s2[a] = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
            const float inv = s2[a] > 1e-12f ? 1 / s2[a] : 0;
            lo[a] = (rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2]) * inv;
            ld[a] = (d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2]) * inv;
        }
        float ln[3] = {0, 0, 0};
        t = 1e30f;
        auto sphere = [&](const float cc[3], float r) {
            const float oc[3] = {lo[0] - cc[0], lo[1] - cc[1], lo[2] - cc[2]};
            const float A = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2], B = oc[0] * ld[0] + oc[1] * ld[1] + oc[2] * ld[2];
            const float C = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - r * r, disc = B * B - A * C;
            if (A < 1e-12f || disc < 0 || C < 0) return;               // (starting inside: not a hit)
            const float th = (-B - std::sqrt(disc)) / A;
            if (th < 0 || th >= t) return;
            t = th;
            for (int k = 0; k < 3; ++k) ln[k] = oc[k] + ld[k] * th;
        };
        if (c.kind == WorldCollider::Box) {
            float t0 = 0, t1 = 1;
            int axis = -1;
            float sgn = 0;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(ld[a]) < 1e-12f) { if (std::fabs(lo[a]) > c.half[a]) return false; continue; }
                float ta = (-c.half[a] - lo[a]) / ld[a], tb = (c.half[a] - lo[a]) / ld[a];
                float sa = -1;
                if (ta > tb) { std::swap(ta, tb); sa = 1; }
                if (ta > t0) { t0 = ta; axis = a; sgn = sa; }
                t1 = std::min(t1, tb);
                if (t0 > t1) return false;
            }
            if (axis < 0) return false;                                // starts inside
            t = t0;
            ln[axis] = sgn;
        } else if (c.kind == WorldCollider::Sphere) {
            const float z[3] = {0, 0, 0};
            sphere(z, c.radius);
        } else if (c.kind == WorldCollider::Capsule) {
            const float h = c.length * 0.5f, r = c.radius;
            const float top[3] = {0, h, 0}, bot[3] = {0, -h, 0};
            sphere(top, r); sphere(bot, r);
            const float A = ld[0] * ld[0] + ld[2] * ld[2], B = lo[0] * ld[0] + lo[2] * ld[2], C = lo[0] * lo[0] + lo[2] * lo[2] - r * r;
            const float disc = B * B - A * C;
            if (A > 1e-12f && disc >= 0 && C > 0) {
                const float th = (-B - std::sqrt(disc)) / A, y = lo[1] + ld[1] * th;
                if (th >= 0 && th < t && std::fabs(y) <= h) { t = th; ln[0] = lo[0] + ld[0] * th; ln[1] = 0; ln[2] = lo[2] + ld[2] * th; }
            }
        } else {
            return false;
        }
        if (t > 1) return false;
        // the local normal back to world (scaled axes: divide by the scale)
        float wn[3] = {0, 0, 0};
        for (int a = 0; a < 3; ++a) {
            const float k = s2[a] > 1e-12f ? ln[a] / s2[a] : 0;
            for (int j = 0; j < 3; ++j) wn[j] += c.world[a * 4 + j] * k;
        }
        const float L = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        for (int j = 0; j < 3; ++j) n[j] = L > 1e-9f ? wn[j] / L : (j == 1 ? 1.0f : 0.0f);
        return true;
    }

    static bool heightOn(const Tri& t, float x, float z, float& h) {
        const float d = (t.b[2] - t.c[2]) * (t.a[0] - t.c[0]) + (t.c[0] - t.b[0]) * (t.a[2] - t.c[2]);
        if (std::fabs(d) < 1e-12f) return false;
        const float l1 = ((t.b[2] - t.c[2]) * (x - t.c[0]) + (t.c[0] - t.b[0]) * (z - t.c[2])) / d;
        const float l2 = ((t.c[2] - t.a[2]) * (x - t.c[0]) + (t.a[0] - t.c[0]) * (z - t.c[2])) / d;
        const float l3 = 1 - l1 - l2;
        if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) return false;
        h = l1 * t.a[1] + l2 * t.b[1] + l3 * t.c[1];
        return true;
    }

    static bool rayTri(const float o[3], const float d[3], const Tri& t, float& out) {
        const float e1[3] = {t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]}, e2[3] = {t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
        const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::fabs(det) < 1e-12f) return false;
        const float inv = 1 / det, s[3] = {o[0] - t.a[0], o[1] - t.a[1], o[2] - t.a[2]};
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0 || u > 1) return false;
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0 || u + v > 1) return false;
        out = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        return true;
    }

    // Closest point on a triangle to p (Ericson), for body-vs-wall tests.
    static void closest(const Tri& t, const float p[3], float out[3]) {
        auto sub = [](const float* a, const float* b, float* o) { for (int k = 0; k < 3; ++k) o[k] = a[k] - b[k]; };
        auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        float ab[3], ac[3], ap[3], bp[3], cp[3];
        sub(t.b, t.a, ab); sub(t.c, t.a, ac); sub(p, t.a, ap);
        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) { std::copy_n(t.a, 3, out); return; }
        sub(p, t.b, bp);
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) { std::copy_n(t.b, 3, out); return; }
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) out[k] = t.a[k] + v * ab[k]; return; }
        sub(p, t.c, cp);
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) { std::copy_n(t.c, 3, out); return; }
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) out[k] = t.a[k] + w * ac[k]; return; }
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
            const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            for (int k = 0; k < 3; ++k) out[k] = t.b[k] + w * (t.c[k] - t.b[k]);
            return;
        }
        const float den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
        for (int k = 0; k < 3; ++k) out[k] = t.a[k] + ab[k] * v + ac[k] * w;
    }

    // Is a body at (x, y..y+height, z) inside a wall? `push` = direction out of it (horizontal).
    bool blocked(float x, float y, float z, float r, float height, float push[3]) const {
        push[0] = push[1] = push[2] = 0;
        bool hit = false;
        const float knee = y + 0.35f;                                // low clutter (rugs, thresholds) is stepped over
        for (float hy : {knee, y + height * 0.55f, y + height - 0.1f}) {
            const float p[3] = {x, hy, z};
            for (int cx = cell(x - r); cx <= cell(x + r); ++cx)
                for (int cz = cell(z - r); cz <= cell(z + r); ++cz) {
                    auto it = wallGrid_.find(key(cx, cz));
                    if (it == wallGrid_.end()) continue;
                    for (uint32_t i : it->second) {
                        float q[3];
                        closest(walls_[i], p, q);
                        const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
                        if (dx * dx + dy * dy + dz * dz < r * r) { push[0] += dx; push[2] += dz; hit = true; }
                    }
                }
            for (const WorldCollider& c : shapes_) {
                float inv[3], l[3];   // NOLINT
                const float rel[3] = {p[0] - c.world[12], p[1] - c.world[13], p[2] - c.world[14]};
                for (int a = 0; a < 3; ++a) {                          // local coords (axes may be scaled)
                    const float* ax = &c.world[a * 4];
                    const float s2 = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
                    inv[a] = s2 > 1e-12f ? 1 / s2 : 0;
                    l[a] = (rel[0] * ax[0] + rel[1] * ax[1] + rel[2] * ax[2]) * inv[a];
                }
                const float sx = std::sqrt(c.world[0] * c.world[0] + c.world[1] * c.world[1] + c.world[2] * c.world[2]);
                const float rl = sx > 1e-6f ? r / sx : r;
                bool in = false;
                if (c.kind == WorldCollider::Box) in = std::fabs(l[0]) < c.half[0] + rl && std::fabs(l[1]) < c.half[1] + rl && std::fabs(l[2]) < c.half[2] + rl;
                else if (c.kind == WorldCollider::Sphere) in = l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < (c.radius + rl) * (c.radius + rl);
                else if (c.kind == WorldCollider::Capsule) {
                    const float yy = std::clamp(l[1], -c.length * 0.5f, c.length * 0.5f);
                    in = l[0] * l[0] + (l[1] - yy) * (l[1] - yy) + l[2] * l[2] < (c.radius + rl) * (c.radius + rl);
                }
                if (in) { push[0] += p[0] - c.world[12]; push[2] += p[2] - c.world[14]; hit = true; }   // (tested from the knee up: low steps pass)
            }
        }
        return hit;
    }

    bool wallBetween(const float a[3], const float b[3]) const {
        const float lift[3] = {b[0], b[1] + 0.5f, b[2]};                // test from the target, half a metre up
        const float dl[3] = {a[0] - lift[0], a[1] - lift[1], a[2] - lift[2]};
        const float len = std::sqrt(dl[0] * dl[0] + dl[1] * dl[1] + dl[2] * dl[2]);
        if (len < 1e-6f) return false;
        const int steps = std::max(1, static_cast<int>(len / 0.5f));
        for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / steps * 0.98f;
            const float x = lift[0] + dl[0] * t, z = lift[2] + dl[2] * t;
            bool hit = false;
            forCell(wallGrid_, x, z, [&](uint32_t i) { float th; if (!hit && rayTri(lift, dl, walls_[i], th) && th > 0.02f && th < 0.98f) hit = true; });
            if (hit) return true;
        }
        return false;
    }
};
