// Top-down picture of a home's walkable area: green = walkable (brighter = higher), orange = left out (unreached),
// blue = bridged, grey = other solid tops (brighter = higher), red = spawns/hotspots, magenta = seats.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "hsl_scene.h"
#include "world_gen.h"
#include "surface_mesh.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HslScene s; std::string err;
    std::vector<std::string> apks;   // "a.apk+b.apk": a footprint and its vista
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    if (!loadHslScene(apks, s, err)) { std::printf("LOAD FAIL %s\n", err.c_str()); return 1; }
    if (const char* wf = std::getenv("WORLD")) {   // WORLD=<world.json>: a saved world (Studio's edits) in place of the home's
        std::FILE* f = std::fopen(wf, "rb");
        std::string j;
        if (f) { char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) j.append(b, n); std::fclose(f); }
        HomeWorld w;
        if (j.empty() || !worldFromJson(j, w)) { std::printf("WORLD FAIL %s\n", wf); return 1; }
        s.world = w;
        if (s.world.spawns.empty()) {   // as Studio's Auto-generate: a start on the floor, here the largest level's nearest the middle
            WalkReport r0;
            auto g0 = bakeWalkGrid(s, s.world, WalkParams{}, &r0);
            float best = 1e30f;
            WorldPoint sp;
            for (int z = 0; z < g0->nz; ++z) for (int x = 0; x < g0->nx; ++x)
                for (const WalkSpan* q = g0->begin(x, z); q != g0->end(x, z); ++q)
                    if ((q->flags & kWalkOk) && q->dist > 100) {
                        const float d = std::hypot(g0->centerX(x) - (g0->ox + g0->nx * 0.05f), g0->centerZ(z) - (g0->oz + g0->nz * 0.05f));
                        if (d < best) { best = d; sp.pos[0] = g0->centerX(x); sp.pos[1] = q->hi; sp.pos[2] = g0->centerZ(z); }
                    }
            if (best < 1e29f) { s.world.spawns.push_back(sp); std::printf("   spawn added at (%.2f %.2f %.2f)\n", sp.pos[0], sp.pos[1], sp.pos[2]); }
        }
    }
    if (std::getenv("SIMPLIFY")) {   // every drawn mesh simplified as a Surface is: triangles, time and the area, both ways
        const std::vector<WalkSource> src = drawnWalkSources(s, s.world, nullptr, nullptr, false);
        std::vector<std::vector<float>> pos(src.size());
        std::vector<std::vector<uint32_t>> idx(src.size());
        std::vector<WalkSource> simp = src;
        size_t t0 = 0, t1 = 0;
        const auto c0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < src.size(); ++i) {
            pos[i].assign(src[i].pos, src[i].pos + src[i].verts * 3);
            idx[i].assign(src[i].idx, src[i].idx + src[i].indices);
            t0 += idx[i].size() / 3;
            simplifySurfaceMesh(pos[i], idx[i], surfaceCellFor(src[i].world));
            t1 += idx[i].size() / 3;
            simp[i].pos = pos[i].data(); simp[i].verts = pos[i].size() / 3; simp[i].idx = idx[i].data(); simp[i].indices = idx[i].size();
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        WalkReport ra, rb;
        auto ga = buildWalkGrid(src, walkSeeds(s.world), WalkParams{}, &ra);
        auto gb = buildWalkGrid(simp, walkSeeds(s.world), WalkParams{}, &rb);
        int differ = 0, cells = 0;
        for (int cz = 0; cz < ga->nz; ++cz) for (int cx = 0; cx < ga->nx; ++cx) {
            const float x = ga->centerX(cx), z = ga->centerZ(cz);
            for (const WalkSpan* q = ga->begin(cx, cz); q != ga->end(cx, cz); ++q) if (q->flags & kWalkOk) { ++cells; differ += !gb->walkable(x, z, q->hi); }
        }
        std::printf("SIMPLIFY %zu -> %zu triangles (%.0f%%) in %.0f ms; area %.1f -> %.1f m2, %d of %d cells changed; bake %.0f -> %.0f ms\n", t0, t1,
                    100.0 * t1 / std::max<size_t>(1, t0), ms, ra.walkArea, rb.walkArea, differ, cells, ra.ms, rb.ms);
        return 0;
    }
    WalkParams P;
    for (int i = 3; i + 1 < argc; i += 2) {
        float v = std::atof(argv[i + 1]);
        if (!strcmp(argv[i], "cell")) P.cell = v; else if (!strcmp(argv[i], "radius")) P.radius = v; else if (!strcmp(argv[i], "gap")) P.gap = v; else if (!strcmp(argv[i], "step")) P.step = v; else if (!strcmp(argv[i], "meta")) P.metaBounds = v;
    }
    WalkReport r;
    auto g = bakeWalkGrid(s, s.world, P, &r);
    std::printf("%s: walk %.1f m2 (%d areas), unreached %.1f m2 (%d), bridged %.1f, ledge cells %d; tris %zu spans %zu %dx%d; ms total %.0f raster %.0f bridge %.0f\n",
                argv[2], r.walkArea, r.areas, r.unreachedArea, r.unreached, r.bridgedArea, r.ledgeCells, r.triangles, r.spans, r.nx, r.nz, r.ms, r.msRaster, r.msBridge);
    for (size_t i = 0; i < r.blockers.size() && i < 6; ++i) {
        int t = r.blockers[i].first;
        std::printf("   blocker %s: %d\n", t < (int)s.meshes.size() ? s.meshes[t].name.c_str() : s.world.colliders[t - s.meshes.size()].name.c_str(), r.blockers[i].second);
    }
    {   // the outline Studio draws: its area against the bake's
        double A = 0; size_t tris = 0, edges = 0; double elen = 0;
        auto t0 = std::chrono::steady_clock::now();
        g->outline([&](const float* a, const float* b, const float* c, bool) { A += 0.5 * std::fabs((b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2])); ++tris; },
                   [&](const float* a, const float* b) { ++edges; elen += std::hypot(b[0] - a[0], b[2] - a[2]); });
        std::printf("   outline: %.1f m2 in %zu tris, %zu edge pieces (%.0f m), %.1f ms\n", A, tris, edges, elen,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    // timing of runtime queries
    {
        auto t0 = std::chrono::steady_clock::now();
        float x = s.world.spawns.empty() ? 0 : s.world.spawns[0].pos[0], z = s.world.spawns.empty() ? 0 : s.world.spawns[0].pos[2], y = 0, h; int ok = 0;
        for (int i = 0; i < 200000; ++i) { float a = i * 0.001f; g->move(x, z, y, 0.02f * std::cos(a * 7), 0.02f * std::sin(a * 5)); ok += g->floorAt(x, z, y, 0.3f, 0.3f, h); }
        std::printf("   200k move+floorAt: %.1f ms (%d ok)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), ok);
    }
    if (const char* pr = std::getenv("PROBE")) {   // "X,Y;X,Y" image pixels (px = 3)
        for (const char* q = pr; *q;) {
            int X, Y, cx, cz; float wx, wz;
            if (*q == 'w') { if (std::sscanf(q + 1, "%f,%f", &wx, &wz) != 2) break; g->cellOf(wx, wz, cx, cz); X = cx * 3; Y = (g->nz - 1 - cz) * 3; }
            else { if (std::sscanf(q, "%d,%d", &X, &Y) != 2) break; cx = X / 3; cz = g->nz - 1 - Y / 3; }
            if (!g->inside(cx, cz)) { std::printf("   probe outside grid\n"); q = std::strchr(q, ';'); if (!q) break; ++q; continue; }
            std::printf("   probe px(%d,%d) cell(%d,%d) world(%.2f,%.2f):", X, Y, cx, cz, g->centerX(cx), g->centerZ(cz));
            for (const WalkSpan* sp = g->begin(cx, cz); sp != g->end(cx, cz); ++sp) std::printf(" [%.3f..%.3f f%d]", sp->lo, sp->hi, sp->flags);
            std::printf("\n");
            q = std::strchr(q, ';'); if (!q) break; ++q;
        }
    }
    const int px = std::getenv("PX") ? std::atoi(std::getenv("PX")) : 3, W = g->nx * px, H = g->nz * px;
    std::vector<unsigned char> img(size_t(W) * H * 3, 20);
    float ylo = 1e30f, yhi = -1e30f;
    for (auto& sp : g->spans) if (sp.flags & (kWalkOk | kWalkUnreached)) { ylo = std::min(ylo, sp.hi); yhi = std::max(yhi, sp.hi); }
    if (ylo > yhi) { ylo = 0; yhi = 1; }
    auto put = [&](int cx, int cz, int R, int G, int B) {
        for (int j = 0; j < px; ++j) for (int i = 0; i < px; ++i) {
            int X = cx * px + i, Y = (g->nz - 1 - cz) * px + j; if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            unsigned char* p = &img[(size_t(Y) * W + X) * 3]; p[0] = R; p[1] = G; p[2] = B; }
    };
    for (int cz = 0; cz < g->nz; ++cz) for (int cx = 0; cx < g->nx; ++cx) {
        const WalkSpan* ok = nullptr, *un = nullptr, *top = nullptr;
        for (const WalkSpan* sp = g->begin(cx, cz); sp != g->end(cx, cz); ++sp) {
            if (sp->flags & (kWalkOk | kWalkNear)) ok = sp; else if ((sp->flags & kWalkUnreached) && sp->hi < ylo + 2.5f) un = sp;
            if (sp->hi < ylo + 2.5f) top = sp;
        }
        auto lv = [&](float y) { return 0.4f + 0.6f * std::clamp((y - ylo) / std::max(0.5f, yhi - ylo), 0.0f, 1.0f); };
        if (un) { float l = lv(un->hi); put(cx, cz, 240 * l, 140 * l, 20 * l); }
        else if (top) { int v = 50 + 100 * std::clamp((top->hi - ylo) / 3.0f, 0.0f, 1.0f); put(cx, cz, v, v, v); }
        if (!ok) continue;
        for (int j = 0; j < px; ++j) for (int i = 0; i < px; ++i) {   // sub-cell: the smooth edge
            const float wx = g->ox + (cx + (i + 0.5f) / px) * g->p.cell, wz = g->oz + (cz + (j + 0.5f) / px) * g->p.cell;
            if (g->field(wx, wz, ok->hi) < 0) continue;
            int X = cx * px + i, Y = (g->nz - 1 - cz) * px + (px - 1 - j);
            float l = lv(ok->hi); unsigned char* q = &img[(size_t(Y) * W + X) * 3];
            if (ok->flags & kWalkBridged) { q[0] = 40; q[1] = 90 * l; q[2] = 255 * l; } else { q[0] = 40 * l; q[1] = 230 * l; q[2] = 60 * l; }
        }
    }
    auto dot = [&](const float* p, int R, int G, int B) {
        int cx, cz; if (!g->cellOf(p[0], p[2], cx, cz)) return;
        for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) put(cx + dx, cz + dz, R, G, B);
    };
    for (auto& p : s.world.spawns) dot(p.pos, 255, 30, 30);
    for (auto& p : s.world.hotspots) dot(p.pos, 255, 30, 30);
    for (auto& p : s.world.seats) dot(p.pos, 255, 0, 255);
    if (std::getenv("GEN")) {   // World > Auto-generate's items over it
        const auto t0 = std::chrono::steady_clock::now();
        const GenResult gr = generateWorldItems(*g, s.world, drawnWalkSources(s, s.world, nullptr, nullptr, false));
        {   // the walls go in the world: re-baked with them, the area must stay the same (they stand on real faces)
            HomeWorld w2 = s.world;
            const char* only = std::getenv("ONLY");               // ONLY=walls / objects: which of them takes floor
            if (!only || !strcmp(only, "walls")) for (const WorldCollider& c : gr.walls) w2.colliders.push_back(c);
            if (!only || !strcmp(only, "objects")) for (const WorldCollider& c : gr.objects) w2.colliders.push_back(c);
            WalkReport r2;
            std::shared_ptr<const WalkGrid> g2 = bakeWalkGrid(s, w2, P, &r2);
            int lost = 0;                                              // floor the walls and objects took: orange
            for (int cz = 0; cz < g->nz; ++cz) for (int cx = 0; cx < g->nx; ++cx)
                for (const WalkSpan* sp = g->begin(cx, cz); sp != g->end(cx, cz); ++sp) {
                    if (!(sp->flags & kWalkOk)) continue;
                    const float wx = g->ox + (cx + 0.5f) * g->p.cell, wz = g->oz + (cz + 0.5f) * g->p.cell;
                    int qx, qz;
                    bool kept = false;
                    if (g2->cellOf(wx, wz, qx, qz))
                        for (const WalkSpan* t = g2->begin(qx, qz); t != g2->end(qx, qz); ++t) kept |= (t->flags & kWalkOk) && std::fabs(t->hi - sp->hi) < 0.1f;
                    if (!kept) { put(cx, cz, 255, 140, 0); ++lost; }
                }
            std::printf("   lost cells: %d\n", lost);
            std::printf("   re-baked with the walls and objects: %.1f m2 (was %.1f)\n", r2.walkArea, r.walkArea);
        }
        std::printf("   generated in %.1f ms: %zu walls, %zu objects, %zu seats, %zu wall spots, %zu hotspots\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                    gr.walls.size(), gr.objects.size(), gr.seats.size(), gr.spots.size(), gr.hotspots.size());
        auto line = [&](float x0, float z0, float x1, float z1, int R, int G, int B) {
            const int n = static_cast<int>(std::hypot(x1 - x0, z1 - z0) / g->p.cell * px) + 1;
            for (int i = 0; i <= n; ++i) {
                const float t = i / static_cast<float>(n), x = x0 + (x1 - x0) * t, z = z0 + (z1 - z0) * t;
                const int X = static_cast<int>((x - g->ox) / g->p.cell * px), Y = static_cast<int>((g->nz - (z - g->oz) / g->p.cell) * px);
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = X + dx, yy = Y + dy; if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    unsigned char* q = &img[(size_t(yy) * W + xx) * 3]; q[0] = R; q[1] = G; q[2] = B; }
            }
        };
        for (const WorldCollider& c : gr.walls) {
            const float hx = c.half[0], ux = c.world[0], uz = c.world[2];
            line(c.world[12] - ux * hx, c.world[14] - uz * hx, c.world[12] + ux * hx, c.world[14] + uz * hx, 255, 40, 40);
            std::printf("   wall %.1f x %.1f m at (%.2f %.2f %.2f) facing (%.2f %.2f)\n", hx * 2, c.half[1] * 2, c.world[12], c.world[13] - c.half[1], c.world[14], -uz, ux);
        }
        for (const WorldCollider& c : gr.objects) {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < c.positions.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], c.positions[i]); hi[i % 3] = std::max(hi[i % 3], c.positions[i]); }
            std::printf("   object x %.2f..%.2f z %.2f..%.2f y %.2f..%.2f, %zu boxes\n", lo[0], hi[0], lo[2], hi[2], lo[1], hi[1], c.positions.size() / 24);
        }
        for (const WorldCollider& c : gr.objects)                 // objects: their footprint boxes' outlines
            for (size_t i = 0; i + 23 < c.positions.size(); i += 24) {
                const float* v = &c.positions[i];
                const float xa = v[0], xb = v[3], za = v[2], zb = v[14];
                line(xa, za, xb, za, 40, 160, 255); line(xb, za, xb, zb, 40, 160, 255); line(xb, zb, xa, zb, 40, 160, 255); line(xa, zb, xa, za, 40, 160, 255);
            }
        for (const WorldWallSpot& w : gr.spots) {
            const float hx = w.width * 0.5f, ux = w.world[0], uz = w.world[2];
            line(w.world[12] - ux * hx, w.world[14] - uz * hx, w.world[12] + ux * hx, w.world[14] + uz * hx, 255, 230, 0);
            line(w.world[12], w.world[14], w.world[12] + w.world[8] * 0.6f, w.world[14] + w.world[10] * 0.6f, 255, 230, 0);
            std::printf("   wall spot %.1f x %.1f m at (%.2f %.2f %.2f)\n", w.width, w.height, w.world[12], w.world[13], w.world[14]);
        }
        for (const WorldPoint& p : gr.seats) {
            const float fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
            line(p.pos[0] - 0.15f, p.pos[2], p.pos[0] + 0.15f, p.pos[2], 255, 0, 255);
            line(p.pos[0], p.pos[2], p.pos[0] + fx * 0.5f, p.pos[2] + fz * 0.5f, 255, 0, 255);
            std::printf("   seat at (%.2f %.2f %.2f) facing (%.2f %.2f)\n", p.pos[0], p.pos[1], p.pos[2], fx, fz);
        }
        for (const WorldPoint& p : gr.hotspots) {
            const float fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
            line(p.pos[0] - 0.25f, p.pos[2] - 0.25f, p.pos[0] + 0.25f, p.pos[2] + 0.25f, 0, 230, 255);
            line(p.pos[0] - 0.25f, p.pos[2] + 0.25f, p.pos[0] + 0.25f, p.pos[2] - 0.25f, 0, 230, 255);
            line(p.pos[0], p.pos[2], p.pos[0] + fx * 0.8f, p.pos[2] + fz * 0.8f, 0, 230, 255);
        }
    }
    stbi_write_png(argv[2], W, H, 3, img.data(), W * 3);
    return 0;
}
