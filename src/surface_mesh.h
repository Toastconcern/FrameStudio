#pragma once
// Surfaces made from a home's meshes (Studio's object Surfaces, Frame Home's mesh colliders) are for walking on and
// bumping into, not for looking at: a sofa drawn with 50 000 triangles of stitching and cushion folds collides the same
// as a few thousand. simplifySurfaceMesh clusters the vertices on a grid (2.5 cm by default, a quarter of the walkable
// area's 10 cm cells, so nothing you can walk on, step up or bump into moves noticeably) and keeps one of each
// triangle left: slivers collapsed to a line or a point go, and so does the back of a two-sided face (it lands on
// the front). The vertices left are compacted. It's idempotent: simplifying a simplified mesh changes nothing.
#include "core/world.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

constexpr float kSurfaceCell = 0.025f;      // m

// cell is in the mesh's own units (a surface stored in its object's frame: kSurfaceCell / the frame's scale).
inline void simplifySurfaceMesh(std::vector<float>& pos, std::vector<uint32_t>& idx, float cell) {
    const size_t nv = pos.size() / 3;
    if (!(cell > 0) || idx.size() < 3 || nv == 0) return;
    constexpr int64_t kOff = int64_t(1) << 20;
    auto key = [&](size_t v) -> uint64_t {
        const int64_t q[3] = {static_cast<int64_t>(std::floor(pos[v * 3] / cell)), static_cast<int64_t>(std::floor(pos[v * 3 + 1] / cell)),
                              static_cast<int64_t>(std::floor(pos[v * 3 + 2] / cell))};
        uint64_t k = 0;
        for (int i = 0; i < 3; ++i) k = (k << 21) | static_cast<uint64_t>(std::clamp<int64_t>(q[i] + kOff, 0, (int64_t(1) << 21) - 1));
        return k;
    };
    // clusters: each the average of the vertices in its cell
    std::unordered_map<uint64_t, uint32_t> clusterOf;
    clusterOf.reserve(nv);
    std::vector<uint32_t> remap(nv);
    std::vector<double> sum;
    std::vector<uint32_t> count;
    for (size_t v = 0; v < nv; ++v) {
        auto [it, fresh] = clusterOf.try_emplace(key(v), static_cast<uint32_t>(count.size()));
        if (fresh) { sum.insert(sum.end(), {0.0, 0.0, 0.0}); count.push_back(0); }
        const uint32_t c = it->second;
        remap[v] = c;
        for (int k = 0; k < 3; ++k) sum[c * 3 + k] += pos[v * 3 + k];
        ++count[c];
    }
    std::vector<float> cp(count.size() * 3);
    for (size_t c = 0; c < count.size(); ++c)
        for (int k = 0; k < 3; ++k) cp[c * 3 + k] = static_cast<float>(sum[c * 3 + k] / count[c]);
    // triangles: collapsed ones go, each one once (either winding)
    std::unordered_set<uint64_t> seen;
    seen.reserve(idx.size() / 3);
    std::vector<uint32_t> out;
    out.reserve(idx.size());
    const bool dedupe = count.size() < (size_t(1) << 21);
    const float minArea2 = cell * cell * 0.02f;     // (twice the area)
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] >= nv || idx[t + 1] >= nv || idx[t + 2] >= nv) continue;
        const uint32_t a = remap[idx[t]], b = remap[idx[t + 1]], c = remap[idx[t + 2]];
        if (a == b || b == c || a == c) continue;
        const float* A = &cp[a * 3]; const float* B = &cp[b * 3]; const float* C = &cp[c * 3];
        const float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        if (std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) < minArea2) continue;
        if (dedupe) {
            uint32_t s[3] = {a, b, c};
            std::sort(s, s + 3);
            if (!seen.insert((uint64_t(s[0]) << 42) | (uint64_t(s[1]) << 21) | s[2]).second) continue;
        }
        out.insert(out.end(), {a, b, c});
    }
    // compact: only the clusters still used
    std::vector<uint32_t> used(count.size(), UINT32_MAX);
    std::vector<float> np;
    np.reserve(cp.size());
    for (uint32_t& v : out) {
        if (used[v] == UINT32_MAX) { used[v] = static_cast<uint32_t>(np.size() / 3); np.insert(np.end(), &cp[v * 3], &cp[v * 3] + 3); }
        v = used[v];
    }
    pos.swap(np);
    idx.swap(out);
}

// The cell in a frame's own units: kSurfaceCell over the frame's (largest) scale, so a scaled-up object isn't coarser.
inline float surfaceCellFor(const float* world) {
    float s = 0;
    for (int c = 0; c < 3; ++c) s = std::max(s, std::sqrt(world[c * 4] * world[c * 4] + world[c * 4 + 1] * world[c * 4 + 1] + world[c * 4 + 2] * world[c * 4 + 2]));
    return s > 1e-6f ? kSurfaceCell / s : kSurfaceCell;
}

// The world's mesh colliders, simplified for walking (Meta's stand-ins are the entities' render meshes, and worlds an
// older Studio saved copied whole meshes). Ones already simplified don't change.
inline void simplifyWorldMeshes(HomeWorld& w) {
    size_t before = 0, after = 0;
    for (WorldCollider& c : w.colliders) {
        if (c.kind != WorldCollider::Mesh || c.simplified) continue;
        c.simplified = true;
        if (c.indices.size() <= 3000) continue;
        before += c.indices.size() / 3;
        simplifySurfaceMesh(c.positions, c.indices, surfaceCellFor(c.world));
        after += c.indices.size() / 3;
    }
    if (after < before) std::fprintf(stderr, "[world] mesh colliders simplified: %zu -> %zu triangles\n", before, after);
}

// ── Surfaces you place (boxes and meshes not made from a home object): two that overlap become one ──

// A Box or Mesh collider's triangles in world space, 9 floats each (spheres and capsules: none).
inline std::vector<float> colliderTriangles(const WorldCollider& c) {
    std::vector<float> out;
    auto xf = [&](float x, float y, float z, float* o) {
        for (int k = 0; k < 3; ++k) o[k] = c.world[k] * x + c.world[4 + k] * y + c.world[8 + k] * z + c.world[12 + k];
    };
    if (c.kind == WorldCollider::Box) {
        float v[8][3];
        for (int k = 0; k < 8; ++k) xf((k & 1 ? 1 : -1) * c.half[0], (k & 2 ? 1 : -1) * c.half[1], (k & 4 ? 1 : -1) * c.half[2], v[k]);
        static const int f[36] = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
        for (int i : f) out.insert(out.end(), v[i], v[i] + 3);
    } else if (c.kind == WorldCollider::Mesh) {
        const size_t nv = c.positions.size() / 3;
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t i = c.indices[t + k];
                float p[3] = {0, 0, 0};
                if (i < nv) xf(c.positions[i * 3], c.positions[i * 3 + 1], c.positions[i * 3 + 2], p);
                out.insert(out.end(), p, p + 3);
            }
    }
    return out;
}

namespace surface_detail {
// Does segment p-q cross triangle abc? (Möller-Trumbore on the segment's length)
inline bool segmentHits(const float* p, const float* q, const float* a, const float* b, const float* c) {
    const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1 / det, s[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) * inv;
    if (u < 0 || u > 1) return false;
    const float qv[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
    if (v < 0 || u + v > 1) return false;
    const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    return t >= 0 && t <= 1;
}
// Is p inside the closed triangle soup? (crossings of a ray from p, slightly tilted so it misses edges)
inline bool inside(const std::vector<float>& tris, const float* p) {
    const float far[3] = {p[0] + 1000.0f, p[1] + 0.731f, p[2] + 0.377f};
    int n = 0;
    for (size_t t = 0; t + 8 < tris.size(); t += 9) n += segmentHits(p, far, &tris[t], &tris[t + 3], &tris[t + 6]);
    return n & 1;
}
// The volume a soup encloses (0 for an open sheet: a flat plate has no inside to bury anything in).
inline float volume(const std::vector<float>& t) {
    double v = 0;
    for (size_t i = 0; i + 8 < t.size(); i += 9)
        v += t[i] * (t[i + 4] * t[i + 8] - t[i + 5] * t[i + 7]) - t[i + 1] * (t[i + 3] * t[i + 8] - t[i + 5] * t[i + 6]) + t[i + 2] * (t[i + 3] * t[i + 7] - t[i + 4] * t[i + 6]);
    return static_cast<float>(std::fabs(v) / 6);
}
inline void bounds(const std::vector<float>& tris, float* lo, float* hi) {
    for (int k = 0; k < 3; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
    for (size_t i = 0; i < tris.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], tris[i]); hi[i % 3] = std::max(hi[i % 3], tris[i]); }
}
}  // namespace surface_detail

// Do two triangle soups (colliderTriangles) touch: an edge of one through a face of the other, or one inside the other.
inline bool surfacesTouch(const std::vector<float>& a, const std::vector<float>& b) {
    using namespace surface_detail;
    if (a.empty() || b.empty()) return false;
    float alo[3], ahi[3], blo[3], bhi[3];
    bounds(a, alo, ahi);
    bounds(b, blo, bhi);
    for (int k = 0; k < 3; ++k) if (alo[k] > bhi[k] + 1e-3f || blo[k] > ahi[k] + 1e-3f) return false;
    auto edgesThrough = [](const std::vector<float>& x, const std::vector<float>& y) {
        for (size_t t = 0; t + 8 < x.size(); t += 9)
            for (int e = 0; e < 3; ++e)
                for (size_t u = 0; u + 8 < y.size(); u += 9)
                    if (segmentHits(&x[t + e * 3], &x[t + (e + 1) % 3 * 3], &y[u], &y[u + 3], &y[u + 6])) return true;
        return false;
    };
    return edgesThrough(a, b) || edgesThrough(b, a) || (volume(b) > 1e-6f && inside(b, &a[0])) || (volume(a) > 1e-6f && inside(a, &b[0]));
}

namespace surface_detail {
// The parts of triangle abc outside a box (its 6 planes cut it; what's inside all of them goes), as triangles. On a
// face of the box counts as inside unless `onIsOut` (two surfaces sharing a face: one of them keeps it).
inline void outsideBox(const float* tri, const WorldCollider& box, std::vector<float>& out, bool onIsOut) {
    using Poly = std::vector<std::array<float, 3>>;
    struct Plane { float u[3], c, h, sgn; };
    std::vector<Plane> planes;
    for (int axis = 0; axis < 3; ++axis) {
        float u[3] = {box.world[axis * 4], box.world[axis * 4 + 1], box.world[axis * 4 + 2]};
        const float L = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (L < 1e-9f) { out.insert(out.end(), tri, tri + 9); return; }
        for (float& x : u) x /= L;
        const float c = u[0] * box.world[12] + u[1] * box.world[13] + u[2] * box.world[14], h = box.half[axis] * L;
        for (float sgn : {1.0f, -1.0f}) planes.push_back({{u[0], u[1], u[2]}, c, h, sgn});
    }
    const float eps = onIsOut ? -1e-5f : 1e-5f;   // inside a side: distance past it <= eps
    auto dist = [&](const Plane& p, const float* x) { return p.sgn * (p.u[0] * x[0] + p.u[1] * x[1] + p.u[2] * x[2] - p.c) - p.h; };
    bool allIn = true;
    for (const Plane& p : planes) {
        int outN = 0;
        for (int k = 0; k < 3; ++k) outN += dist(p, tri + k * 3) > eps;
        if (outN == 3) { out.insert(out.end(), tri, tri + 9); return; }   // wholly outside one side: kept whole
        allIn &= outN == 0;
    }
    if (allIn) return;                                                    // wholly inside: buried
    std::vector<Poly> inside{{{tri[0], tri[1], tri[2]}, {tri[3], tri[4], tri[5]}, {tri[6], tri[7], tri[8]}}};
    std::vector<Poly> keep;
    for (const Plane& pl : planes) {
        {   // the part outside this side is kept; the part inside goes on to the next side
            std::vector<Poly> next;
            for (const Poly& P : inside) {
                Poly in, outP;
                for (size_t i = 0; i < P.size(); ++i) {
                    const auto& A = P[i];
                    const auto& B = P[(i + 1) % P.size()];
                    const float da = dist(pl, A.data()) - eps, db = dist(pl, B.data()) - eps;
                    (da <= 0 ? in : outP).push_back(A);
                    if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                        const float t = da / (da - db);
                        const std::array<float, 3> X = {A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, A[2] + (B[2] - A[2]) * t};
                        in.push_back(X);
                        outP.push_back(X);
                    }
                }
                if (outP.size() >= 3) keep.push_back(outP);
                if (in.size() >= 3) next.push_back(in);
            }
            inside.swap(next);
        }
    }
    for (const Poly& P : keep)
        for (size_t i = 1; i + 1 < P.size(); ++i)
            for (const auto* v : {&P[0], &P[i], &P[i + 1]}) out.insert(out.end(), v->begin(), v->end());
}
}  // namespace surface_detail

// Two surfaces as one, in world space. Boxes (and combined surfaces' boxes, kept in WorldCollider::boxes) give the
// exact outside of their union: each box's faces cut where they go into another box (a face two share is kept once,
// the earlier box's). Any other mesh (a sheet, an imported shape) adds its faces less the ones wholly buried in a
// solid part, and the result then can't be cut exactly again (no boxes kept). Welded.
inline WorldCollider combineSurfaces(const WorldCollider& a, const WorldCollider& b) {
    using namespace surface_detail;
    std::vector<WorldCollider> boxes;            // world frames
    std::vector<std::vector<float>> meshes;      // other parts' world triangles
    for (const WorldCollider* x : {&a, &b}) {
        if (x->kind == WorldCollider::Box) { boxes.push_back(*x); continue; }
        if (x->boxes.empty()) { meshes.push_back(colliderTriangles(*x)); continue; }
        for (size_t i = 0; i + 19 <= x->boxes.size(); i += 19) {
            WorldCollider bx;
            bx.kind = WorldCollider::Box;
            float m[16];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) {
                    float v = 0;
                    for (int k = 0; k < 4; ++k) v += x->world[k * 4 + r] * x->boxes[i + c * 4 + k];
                    m[c * 4 + r] = v;
                }
            std::copy_n(m, 16, bx.world);
            std::copy_n(&x->boxes[i + 16], 3, bx.half);
            boxes.push_back(bx);
        }
    }
    WorldCollider c;
    c.name = a.name;
    c.kind = WorldCollider::Mesh;
    c.role = a.role;
    c.hidden = a.hidden && b.hidden;
    c.placed = true;
    auto add = [&](const float* t) {
        for (int k = 0; k < 9; ++k) c.positions.push_back(t[k]);
        const uint32_t i = static_cast<uint32_t>(c.indices.size());
        c.indices.insert(c.indices.end(), {i, i + 1, i + 2});
    };
    std::vector<std::vector<float>> boxTris(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) boxTris[i] = colliderTriangles(boxes[i]);
    auto buriedIn = [&](const float* t, const std::vector<float>& other) {   // wholly inside a solid soup
        if (volume(other) <= 1e-6f) return false;
        for (int k = 0; k < 3; ++k) {   // its corners pulled a little towards the middle (a corner on the face isn't in)
            float p[3];
            for (int j = 0; j < 3; ++j) p[j] = t[k * 3 + j] * 0.98f + (t[j] + t[3 + j] + t[6 + j]) / 3 * 0.02f;
            if (!inside(other, p)) return false;
        }
        return true;
    };
    for (size_t i = 0; i < boxes.size(); ++i) {
        std::vector<float> cur = boxTris[i];
        for (size_t j = 0; j < boxes.size() && !cur.empty(); ++j) {
            if (j == i) continue;
            std::vector<float> next;
            for (size_t t = 0; t + 8 < cur.size(); t += 9) outsideBox(&cur[t], boxes[j], next, i < j);
            cur.swap(next);
        }
        for (size_t t = 0; t + 8 < cur.size(); t += 9) {
            bool buried = false;
            for (const auto& m : meshes) buried |= buriedIn(&cur[t], m);
            if (!buried) add(&cur[t]);
        }
    }
    for (size_t i = 0; i < meshes.size(); ++i) {
        std::vector<float> cur = meshes[i];
        for (size_t j = 0; j < boxes.size() && !cur.empty(); ++j) {
            std::vector<float> next;
            for (size_t t = 0; t + 8 < cur.size(); t += 9) outsideBox(&cur[t], boxes[j], next, false);
            cur.swap(next);
        }
        for (size_t t = 0; t + 8 < cur.size(); t += 9) {
            bool buried = false;
            for (size_t k = 0; k < meshes.size(); ++k) if (k != i) buried |= buriedIn(&cur[t], meshes[k]);
            if (!buried) add(&cur[t]);
        }
    }
    if (meshes.empty())
        for (const WorldCollider& bx : boxes) { c.boxes.insert(c.boxes.end(), bx.world, bx.world + 16); c.boxes.insert(c.boxes.end(), bx.half, bx.half + 3); }
    simplifySurfaceMesh(c.positions, c.indices, 0.001f);   // shared corners welded
    return c;
}
