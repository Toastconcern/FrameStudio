// What a home's authored animation costs a frame (HslScene::animate, run on the CPU before each frame in Frame Studio
// and Frame Home): which meshes it rewrites, how many vertices, and how long a step takes.
#include "hsl_scene.h"
#include <chrono>
#include <cstdio>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) return 1;
    size_t verts = 0, tris = 0;
    for (int m : s.animatedMeshes) {
        const MeshData& md = s.meshes[m];
        verts += md.positions.size() / 3; tris += md.indices.size() / 3;
        std::printf("  animated %-40s %8zu verts %8zu tris\n", md.name.c_str(), md.positions.size() / 3, md.indices.size() / 3);
    }
    size_t all = 0;
    for (const MeshData& md : s.meshes) all += md.indices.size() / 3;
    std::printf("%zu meshes, %zu triangles; %zu animated (%zu verts, %zu tris), loop %.1f s\n", s.meshes.size(), all, s.animatedMeshes.size(), verts, tris, s.animDuration);
    if (!s.animate) { std::printf("no CPU animation\n"); return 0; }
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 90; ++i) s.animate(i / 90.0f, s.meshes);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 90;
    std::printf("animate(): %.2f ms a frame on this PC\n", ms);
}
