// save <home> <file>: bake and write the saved form. check <home> <file>: read a saved form (old FWG1 or new) and
// compare it with a fresh bake here, point by point; and that the new form round-trips exactly.
#include "hsl_scene.h"
#include <cstdio>
#ifdef WALK_OLD
#define SAMEDIST(i) true
#else
#define SAMEDIST(i) (rt->spans[i].dist == g->spans[i].dist)
#endif
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[2];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) return 1;
    auto g = bakeWalkGrid(s, s.world, WalkParams{});
    if (!strcmp(argv[1], "save")) { std::ofstream(argv[3]) << encodeWalkGrid(*g); return 0; }
    std::stringstream ss; ss << std::ifstream(argv[3]).rdbuf();
    auto old = decodeWalkGrid(ss.str());
    if (!old) { std::printf("DECODE FAILED\n"); return 1; }
    auto rt = decodeWalkGrid(encodeWalkGrid(*g));
    bool same = rt && rt->spans.size() == g->spans.size() && rt->nx == g->nx;
    for (size_t i = 0; same && i < g->spans.size(); ++i) same = rt->spans[i].flags == g->spans[i].flags && SAMEDIST(i) && std::fabs(rt->spans[i].hi - g->spans[i].hi) < 0.006f;
    std::mt19937 rng(1); std::uniform_real_distribution<float> U(0, 1);
    long n = 0, agree = 0;
    for (int i = 0; i < 400000; ++i) {
        const float x = g->ox + U(rng) * g->nx * g->p.cell, z = g->oz + U(rng) * g->nz * g->p.cell;
        for (const WalkSpan* sp = g->begin(0, 0); false;) (void)sp;
        int cx, cz; if (!g->cellOf(x, z, cx, cz)) continue;
        const WalkSpan* w = g->walkNear(cx, cz, 0, 1e9f); if (!w) continue;   // a floor level here
        ++n; agree += g->walkable(x, z, w->hi) == old->walkable(x, z, w->hi);
    }
    std::printf("saved form reads: %s; new round trip exact: %s; walkable agreement with the fresh bake: %.2f%% of %ld points near the area\n",
                old ? "yes" : "no", same ? "yes" : "NO", 100.0 * agree / std::max(1L, n), n);
    return 0;
}
