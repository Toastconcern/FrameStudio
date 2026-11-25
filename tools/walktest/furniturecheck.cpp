// Tables and seats are never walked on: the walkable area as a home opens (Frame Home's defaults), checked two ways.
//  1. Meshes named as seats or furniture (a sofa, a chair, a table, a desk): their tops, sampled every 5 cm, against
//     the area. Walkable where its piece of the area (level within 5 cm, under 4 m² or under 90 cm across) stands a
//     seat's height or more (30 cm to 1.3 m) straight over the floor beside it is a failure (a loft's floor merged
//     into its couch's mesh isn't).
//  2. By shape, whatever the names say: every piece of the area raised like that, with the mesh under it, listed
//     (a sofa or a table whose mesh says nothing: Udim05_mat).
// furniturecheck <apk[+vista]>   exit 2: a named seat or table has walkable floor on it
#include "hsl_scene.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {

struct Tri { float a[3], b[3], c[3]; int src; };

const char* hintName(WalkHint h) {
    switch (h) {
        case kHintSeat: return "seat";
        case kHintFurniture: return "furniture";
        case kHintFloor: return "floor";
        case kHintWall: return "wall";
        case kHintBlocker: return "blocker";
        case kHintCeiling: return "ceiling";
        case kHintNature: return "nature";
        default: return "-";
    }
}

// The height of triangle t's plane over (x, z), if (x, z) is inside it.
bool heightOn(const Tri& t, float x, float z, float& y) {
    const float d = (t.b[2] - t.c[2]) * (t.a[0] - t.c[0]) + (t.c[0] - t.b[0]) * (t.a[2] - t.c[2]);
    if (std::fabs(d) < 1e-9f) return false;
    const float u = ((t.b[2] - t.c[2]) * (x - t.c[0]) + (t.c[0] - t.b[0]) * (z - t.c[2])) / d;
    const float v = ((t.c[2] - t.a[2]) * (x - t.c[0]) + (t.a[0] - t.c[0]) * (z - t.c[2])) / d;
    if (u < -1e-4f || v < -1e-4f || u + v > 1 + 1e-4f) return false;
    y = u * t.a[1] + v * t.b[1] + (1 - u - v) * t.c[1];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "furniturecheck <apk[+vista]>\n"); return 1; }
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s;
    std::string err;
    if (!loadHslScene(apks, s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    if (std::getenv("NOMETA")) s.world.useHomeCollision = false;   // Studio's "Use Meta's floor" off
    if (std::getenv("ADDITIVE"))   // the meshes drawn as glows that add light (scene_loader: blended with one alpha)
        for (const MeshData& md : s.meshes)
            if (md.additive) std::printf("additive %s (%zu triangles)\n", md.name.c_str(), md.indices.size() / 3);
    if (std::getenv("META")) {     // what the home's makers set up for walking: navmesh / collision meshes, colliders
        for (const MeshData& md : s.meshes)
            if (md.overlayKind) std::printf("meta %s mesh %s (%zu triangles)\n", md.overlayKind == 1 ? "navmesh" : "collision", md.name.c_str(), md.indices.size() / 3);
        int byKind[8] = {}, byRole[8] = {};
        for (const WorldCollider& c : s.world.colliders) { ++byKind[std::clamp(static_cast<int>(c.kind), 0, 7)]; ++byRole[std::clamp(c.role, 0, 7)]; }
        std::printf("meta colliders %zu: kinds box %d sphere %d capsule %d mesh %d; roles solid %d trigger %d object %d walkable %d\n",
                    s.world.colliders.size(), byKind[WorldCollider::Box], byKind[WorldCollider::Sphere], byKind[WorldCollider::Capsule],
                    byKind[WorldCollider::Mesh], byRole[WorldCollider::Solid], byRole[WorldCollider::Trigger], byRole[WorldCollider::Object],
                    byRole[WorldCollider::Walkable]);
        std::printf("meta spawns %zu seats %zu hotspots %zu\n", s.world.spawns.size(), s.world.seats.size(), s.world.hotspots.size());
    }
    if (std::getenv("NAMES"))      // every mesh's name, for checking name rules against a real home
        for (const MeshData& md : s.meshes) std::printf("name %s\n", md.name.c_str());
    if (const char* at = std::getenv("AT")) {   // AT="x,z,y": the walk sources with a triangle over (x, z), up to 3 m over y
        float px = 0, pz = 0, py = 0;
        std::sscanf(at, "%f,%f,%f", &px, &pz, &py);
        for (const WalkSource& src : drawnWalkSources(s, s.world)) {
            float lo = 1e30f, hi = -1e30f;
            for (size_t t = 0; t + 2 < src.indices; t += 3) {
                float v[3][3];
                for (int k = 0; k < 3; ++k) walkgrid_detail::xform(src.world, src.pos + src.idx[t + k] * 3, v[k]);
                const float d1 = (px - v[1][0]) * (v[0][2] - v[1][2]) - (v[0][0] - v[1][0]) * (pz - v[1][2]);
                const float d2 = (px - v[2][0]) * (v[1][2] - v[2][2]) - (v[1][0] - v[2][0]) * (pz - v[2][2]);
                const float d3 = (px - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (pz - v[0][2]);
                if ((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)) continue;
                for (int k = 0; k < 3; ++k) { lo = std::min(lo, v[k][1]); hi = std::max(hi, v[k][1]); }
            }
            if (lo <= hi && hi >= py - 0.5f && lo <= py + 3) std::printf("over %s%s  y %.2f..%.2f (its triangles there)\n", src.cutout ? "(cut-out) " : "", src.name.c_str(), lo, hi);
        }
    }
    WalkReport rep;
    const auto g = bakeWalkGrid(s, s.world, WalkParams{}, &rep);
    if (!g) { std::printf("no walkable area\n"); return 0; }
    std::printf("walkable %.1f m2\n", rep.walkArea);
    if (std::getenv("CELLS"))      // each walkable cell (its middle and floor height), for diffing two bakes
        for (int z = 0; z < g->nz; ++z)
            for (int x = 0; x < g->nx; ++x)
                for (const WalkSpan* q = g->begin(x, z); q != g->end(x, z); ++q)
                    if (q->flags & kWalkOk) std::printf("cell %.2f %.2f %.2f\n", g->centerX(x), g->centerZ(z), q->hi);
    if (const char* d = std::getenv("DUMP")) {   // DUMP="x0,z0,x1,z1,y": each cell's top span up to 2 m over y (walkable *, edge +, furniture f, other .), cm
        float r[5] = {0, 0, 0, 0, 0};
        std::sscanf(d, "%f,%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3], &r[4]);
        for (float z = r[1]; z <= r[3]; z += g->p.cell) {
            std::string line;
            for (float x = r[0]; x <= r[2]; x += g->p.cell) {
                int cx, cz;
                char cell[16] = "     ";
                if (g->cellOf(x, z, cx, cz)) {
                    const WalkSpan* best = nullptr;
                    for (const WalkSpan* q = g->begin(cx, cz); q != g->end(cx, cz); ++q)
                        if (q->hi <= r[4] + 2.0f && (!best || q->hi > best->hi)) best = q;
                    if (best) std::snprintf(cell, sizeof cell, "%c%4d", best->flags & kWalkOk ? '*' : best->flags & kWalkNear ? '+' : best->flags & kWalkFurniture ? 'f' : '.',
                                            static_cast<int>(std::lround(best->hi * 100)));
                }
                line += cell;
            }
            std::printf("%6.1f %s\n", z, line.c_str());
        }
        return 0;
    }

    const std::vector<WalkSource> src = drawnWalkSources(s, s.world);
    std::vector<Tri> up;                                   // every face that faces up, world space
    for (size_t i = 0; i < src.size(); ++i) {
        const WalkSource& w = src[i];
        std::vector<float> wp(w.verts * 3);
        for (size_t v = 0; v < w.verts; ++v)
            for (int j = 0; j < 3; ++j) wp[v * 3 + j] = w.world[j] * w.pos[v * 3] + w.world[4 + j] * w.pos[v * 3 + 1] + w.world[8 + j] * w.pos[v * 3 + 2] + w.world[12 + j];
        for (size_t t = 0; t + 2 < w.indices; t += 3) {
            const uint32_t ia = w.idx[t], ib = w.idx[t + 1], ic = w.idx[t + 2];
            if (ia >= w.verts || ib >= w.verts || ic >= w.verts) continue;
            Tri tr;
            std::memcpy(tr.a, &wp[ia * 3], 12); std::memcpy(tr.b, &wp[ib * 3], 12); std::memcpy(tr.c, &wp[ic * 3], 12);
            const float e1[3] = {tr.b[0] - tr.a[0], tr.b[1] - tr.a[1], tr.b[2] - tr.a[2]}, e2[3] = {tr.c[0] - tr.a[0], tr.c[1] - tr.a[1], tr.c[2] - tr.a[2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (L < 1e-9f || std::fabs(n[1]) / L < 0.85f) continue;
            tr.src = static_cast<int>(i);
            up.push_back(tr);
        }
    }

    // A walkable span's piece of the area: raised (furniture-like) or not, worked out once per piece
    const int gnx = g->nx;
    std::vector<int8_t> raisedPiece(g->spans.size(), -1);
    struct Piece { double area = 0, sx = 0, sz = 0, y = 0; float drop = 0; int drops = 0; };
    auto isRaised = [&](int cx0, int cz0, const WalkSpan* s0, Piece* out = nullptr) {
        const size_t i0 = s0 - g->spans.data();
        if (raisedPiece[i0] >= 0) return raisedPiece[i0] == 1;
        static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        std::vector<std::pair<int, size_t>> st{{cz0 * gnx + cx0, i0}}, all;
        raisedPiece[i0] = 0;
        double area = 0, edge = 0, sx = 0, sz = 0, sy = 0;
        int drops = 0;
        float deepest = 0;
        while (!st.empty()) {
            const auto [c, si] = st.back();
            st.pop_back();
            all.push_back({c, si});
            const int cx = c % gnx, cz = c / gnx;
            const float y = g->spans[si].hi;
            area += g->p.cell * g->p.cell;
            sx += g->centerX(cx); sz += g->centerZ(cz); sy += y;
            bool drop = false;
            for (const auto& dd : d4) {
                const int xx = cx + dd[0], zz = cz + dd[1];
                if (!g->inside(xx, zz)) { edge += g->p.cell; continue; }
                bool same = false;
                const WalkSpan* top = nullptr;
                for (const WalkSpan* q = g->begin(xx, zz); q != g->end(xx, zz); ++q) {
                    if ((q->flags & kWalkOk) && std::fabs(q->hi - y) <= 0.05f) {
                        same = true;
                        const size_t qi = q - g->spans.data();
                        if (raisedPiece[qi] < 0) { raisedPiece[qi] = 0; st.push_back({zz * gnx + xx, qi}); }
                    }
                    if (q->hi <= y + 0.25f) top = q;
                }
                if (same) continue;
                edge += g->p.cell;
                // out from the edge up to 30 cm (a cushion's rounded front, a table's lip): the first floor a seat's
                // height or more below, with nothing standing over this top's level on the way (a backrest, a rail)
                for (int k = 1; k <= 3; ++k) {
                    const int ox = cx + dd[0] * k, oz = cz + dd[1] * k;
                    if (!g->inside(ox, oz)) break;
                    const WalkSpan* t = nullptr;
                    for (const WalkSpan* q = g->begin(ox, oz); q != g->end(ox, oz); ++q) if (q->hi <= y + 0.25f) t = q;
                    const WalkSpan* above = t ? t + 1 : g->begin(ox, oz);
                    if (above != g->end(ox, oz) && above->lo <= y + 0.25f) break;   // (something stands there)
                    if (!t) break;
                    if ((t->flags & (kWalkOk | kWalkNear | kWalkUnreached)) && t->hi <= y - 0.3f && t->hi >= y - 1.3f) { drop = true; deepest = std::max(deepest, y - t->hi); break; }
                    if (t->flags & (kWalkOk | kWalkNear)) break;           // a floor less than a seat's height down: a step
                }
            }
            drops += drop;
        }
        const bool raised = drops >= 2 && (area < 4.0 || 2 * area / std::max(edge, 0.1) < 0.9);
        if (out) {
            const double n = static_cast<double>(all.size());
            *out = Piece{area, sx / n, sz / n, sy / n, deepest, drops};
        }
        for (const auto& [c, si] : all) raisedPiece[si] = raised ? 1 : 0;
        return raised;
    };

    // 1. Named seats and furniture: their tops against the area
    std::vector<std::string> names;
    for (const MeshData& md : s.meshes) names.push_back(md.name);
    const std::string prefix = walkSharedPrefix(names);
    struct Named { double top = 0, walked = 0; float lo = 1e30f, hi = -1e30f, floor = 1e30f, sx = 0, sz = 0; int n = 0; };
    std::map<int, Named> named;                            // per source: its top, and how much of it is walkable (m²)
    for (const Tri& t : up) {
        const WalkSource& w = src[t.src];
        const WalkHint h = walkNameHint(w.name, prefix);
        if (h != kHintSeat && h != kHintFurniture) continue;
        const float lo[2] = {std::min({t.a[0], t.b[0], t.c[0]}), std::min({t.a[2], t.b[2], t.c[2]})};
        const float hi[2] = {std::max({t.a[0], t.b[0], t.c[0]}), std::max({t.a[2], t.b[2], t.c[2]})};
        constexpr float d = 0.05f;
        for (float x = std::floor(lo[0] / d) * d + d / 2; x <= hi[0]; x += d)
            for (float z = std::floor(lo[1] / d) * d + d / 2; z <= hi[1]; z += d) {
                float y;
                if (!heightOn(t, x, z, y)) continue;
                auto& acc = named[t.src];
                acc.top += d * d;
                int cx, cz;
                if (!g->cellOf(x, z, cx, cz)) continue;
                // walkable at its top, and that top stands over a floor (not a rug, a mat or a platform's own top)
                const WalkSpan* sp = g->walkNear(cx, cz, y, 0.04f, kWalkOk);
                if (!sp) continue;
                if (!isRaised(cx, cz, sp)) continue;
                float fl = 1e30f;
                for (int k = 0; k < 8; ++k) {
                    static const int dd[8][2] = {{3, 0}, {-3, 0}, {0, 3}, {0, -3}, {6, 0}, {-6, 0}, {0, 6}, {0, -6}};
                    if (const WalkSpan* q = g->walkNear(cx + dd[k][0], cz + dd[k][1], y - 0.8f, 0.5f, kWalkOk | kWalkNear)) fl = std::min(fl, q->hi);
                }
                acc.walked += d * d;
                acc.lo = std::min(acc.lo, y); acc.hi = std::max(acc.hi, y); acc.floor = std::min(acc.floor, fl);
                acc.sx += x; acc.sz += z; ++acc.n;
            }
    }
    int bad = 0;
    std::printf("named seats and furniture: %zu\n", named.size());
    for (const auto& [i, a] : named) {
        const bool walked = a.walked >= 0.02;
        bad += walked;
        std::printf("  %s %-10s %-60s top %6.2f m2, walkable %.2f m2", walked ? "FAIL" : "ok  ", hintName(walkNameHint(src[i].name, prefix)),
                    src[i].name.substr(0, 60).c_str(), a.top, a.walked);
        if (walked) std::printf(" at %.2f-%.2f m, round (%.1f, %.1f), floor at %.2f", a.lo, a.hi, a.sx / a.n, a.sz / a.n, a.floor);
        std::printf("\n");
    }

    // 2. By shape: every raised piece of the area, whatever it's named
    int raised = 0;
    std::fill(raisedPiece.begin(), raisedPiece.end(), static_cast<int8_t>(-1));   // (afresh: part 1 saw only the named tops)
    for (int z0 = 0; z0 < g->nz; ++z0)
        for (int x0 = 0; x0 < g->nx; ++x0)
            for (const WalkSpan* s0 = g->begin(x0, z0); s0 != g->end(x0, z0); ++s0) {
                const size_t i0 = s0 - g->spans.data();
                if (!(s0->flags & kWalkOk) || raisedPiece[i0] >= 0) continue;
                Piece pc;
                if (!isRaised(x0, z0, s0, &pc) || pc.area < 0.05) continue;
                int under = -1;                                // what's under its middle
                float best = 0.06f;
                for (const Tri& t : up) {
                    float ty;
                    if (heightOn(t, static_cast<float>(pc.sx), static_cast<float>(pc.sz), ty) && std::fabs(ty - pc.y) < best) { best = std::fabs(ty - pc.y); under = t.src; }
                }
                ++raised;
                std::printf("  RAISED %5.2f m2 at (%6.2f %5.2f %6.2f), up to %.2f m straight over the floor beside it (%d cells): %s [%s]\n", pc.area, pc.sx, pc.y, pc.sz,
                            pc.drop, pc.drops, under >= 0 ? src[under].name.substr(0, 70).c_str() : "(nothing drawn there)",
                            under >= 0 ? hintName(walkNameHint(src[under].name, prefix)) : "-");
            }
    std::printf("raised pieces of floor (by shape): %d\n", raised);
    std::printf("%s\n", bad ? "FAIL: a seat or table can be walked on" : "ok: no named seat or table can be walked on");
    return bad ? 2 : 0;
}
