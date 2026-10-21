// Floors the walkable area leaves out, and what keeps each from it: for every separate left-out floor (> 0.5 m2),
// its size and height, and the nearest walkable floor: how far across and how far up or down.
#include "hsl_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) return 1;
    auto g = bakeWalkGrid(s, s.world, WalkParams{});
    const int nx = g->nx, nz = g->nz;
    const float cs = g->p.cell;
    std::vector<int> region(g->spans.size(), -1);
    struct R { double area = 0; double ySum = 0; int n = 0; float bestD = 1e9f, bestDh = 0, at[3] = {}, to[3] = {}; };
    std::vector<R> regs;
    for (int z = 0; z < nz; ++z) for (int x = 0; x < nx; ++x)
        for (const WalkSpan* sp = g->begin(x, z); sp != g->end(x, z); ++sp) {
            const size_t i = sp - g->spans.data();
            if (!(sp->flags & kWalkUnreached) || region[i] >= 0) continue;
            const int id = (int)regs.size(); regs.emplace_back();
            std::vector<std::pair<int, size_t>> st{{z * nx + x, i}}; region[i] = id;
            while (!st.empty()) {
                auto [c, si] = st.back(); st.pop_back();
                const int cx = c % nx, cz = c / nx;
                R& r = regs[id]; r.area += cs * cs; r.ySum += g->spans[si].hi; ++r.n;
                const int dd[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                for (auto& d : dd) {
                    const int xx = cx + d[0], zz = cz + d[1];
                    if (!g->inside(xx, zz)) continue;
                    for (const WalkSpan* q = g->begin(xx, zz); q != g->end(xx, zz); ++q) {
                        const size_t qi = q - g->spans.data();
                        if ((q->flags & kWalkUnreached) && region[qi] < 0 && std::fabs(q->hi - g->spans[si].hi) <= g->p.step) { region[qi] = id; st.push_back({zz * nx + xx, qi}); }
                    }
                }
                // nearest walkable floor within 1.5 m
                const int rr = (int)std::ceil(1.5f / cs);
                for (int j = cz - rr; j <= cz + rr; j += 1) for (int k = cx - rr; k <= cx + rr; k += 1) {
                    if (!g->inside(k, j)) continue;
                    for (const WalkSpan* q = g->begin(k, j); q != g->end(k, j); ++q) {
                        if (!(q->flags & (kWalkOk | kWalkNear))) continue;
                        const float dh = q->hi - g->spans[si].hi;
                        if (std::fabs(dh) > 1.2f) continue;
                        const float d = std::hypot(k - cx, j - cz) * cs;
                        const float score = d + std::fabs(dh);
                        if (score < regs[id].bestD + std::fabs(regs[id].bestDh)) { regs[id].bestD = d; regs[id].bestDh = dh; regs[id].at[0] = g->centerX(cx); regs[id].at[1] = g->spans[si].hi; regs[id].at[2] = g->centerZ(cz); regs[id].to[0] = g->centerX(k); regs[id].to[1] = q->hi; regs[id].to[2] = g->centerZ(j); }
                    }
                }
            }
        }
    std::sort(regs.begin(), regs.end(), [](const R& a, const R& b) { return a.area > b.area; });
    int shown = 0;
    for (const R& r : regs) {
        if (r.area < 0.5 || r.bestD > 1.4f) continue;
        const char* why = std::fabs(r.bestDh) > g->p.step ? (std::fabs(r.bestDh) < 0.5f ? "STEP a bit high" : "level apart")
                        : r.bestD > g->p.gap + 0.05f ? (r.bestD < 0.6f ? "GAP a bit wide" : "apart") : "narrow / blocked";
        std::printf("  %6.1f m2 at y %5.2f  (%.2f %.2f %.2f): nearest walkable %.2f m across, %+.2f m  -> %s\n", r.area, r.ySum / r.n, r.at[0], r.at[1], r.at[2], r.bestD, r.bestDh, why);
        // what stands between: meshes / colliders whose extent covers the line between them, above the floor
        std::string names;
        const float y0 = std::min(r.at[1], r.to[1]);
        auto test = [&](const float* lo, const float* hi, const std::string& name) {
            for (int k = 0; k <= 8; ++k) {
                const float t = k / 8.0f, x = r.at[0] + (r.to[0] - r.at[0]) * t, z = r.at[2] + (r.to[2] - r.at[2]) * t;
                if (x >= lo[0] - 0.05f && x <= hi[0] + 0.05f && z >= lo[2] - 0.05f && z <= hi[2] + 0.05f && hi[1] > y0 + 0.05f && lo[1] < y0 + 1.6f &&
                    (hi[0] - lo[0]) < 30 && (hi[2] - lo[2]) < 30) { if (names.size() < 300) names += " [" + name + "]"; return; }
            }
        };
        for (const MeshData& md : s.meshes) {
            if (!walkMeshUse(md)) continue;
            float m[16]; if (md.hasWorldMatrix) std::memcpy(m, md.worldMatrix, 64); else trsToMat4(md.transform, m);
            float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
            for (size_t v = 0; v + 2 < md.positions.size(); v += 3) for (int c = 0; c < 3; ++c) { const float w = m[c]*md.positions[v]+m[4+c]*md.positions[v+1]+m[8+c]*md.positions[v+2]+m[12+c]; lo[c]=std::min(lo[c],w); hi[c]=std::max(hi[c],w); }
            test(lo, hi, md.name);
        }
        for (const WorldCollider& c : s.world.colliders) {
            if (c.role == WorldCollider::Trigger) continue;
            std::vector<float> P; std::vector<uint32_t> I;
            if (c.kind == WorldCollider::Mesh) { P.resize(c.positions.size()); for (size_t v = 0; v + 2 < c.positions.size(); v += 3) for (int k = 0; k < 3; ++k) P[v+k] = c.world[k]*c.positions[v]+c.world[4+k]*c.positions[v+1]+c.world[8+k]*c.positions[v+2]+c.world[12+k]; }
            else walkShapeMesh(c, P, I);
            float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
            for (size_t v = 0; v + 2 < P.size(); v += 3) for (int k = 0; k < 3; ++k) { lo[k]=std::min(lo[k],P[v+k]); hi[k]=std::max(hi[k],P[v+k]); }
            test(lo, hi, "collider " + c.name);
        }
        std::printf("        between:%s\n", names.empty() ? " (nothing standing: a drop, a hole or headroom)" : names.c_str());
        if (++shown >= 14) break;
    }
}
