// The home's meshes: name, triangles and world bounds (what names a home ships, for the generator's hints).
#include "hsl_scene.h"
#include "walkgrid.h"
#include <algorithm>
#include <cstdio>
int main(int argc, char** argv) {
    HslScene s; std::string err;
    if (argc < 2) return 2;
    std::vector<std::string> apks;   // "a.apk+b.apk": a footprint and its vista
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    if (!loadHslScene(apks, s, err)) return 1;
    std::vector<std::string> names;
    for (const MeshData& m : s.meshes) names.push_back(m.name);
    const std::string pre = walkSharedPrefix(names);
    static const char* kHint[] = {"-", "SEAT", "WALL", "BLOCKER", "FLOOR", "CEILING", "FURNITURE", "NATURE"};
    for (const MeshData& m : s.meshes) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < m.positions.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], m.positions[i]); hi[i % 3] = std::max(hi[i % 3], m.positions[i]); }
        std::printf("%-10s %-60s %7zu tris  x %6.2f..%6.2f  y %5.2f..%5.2f  z %6.2f..%6.2f\n", kHint[walkNameHint(m.name, pre)], (m.path.empty() ? m.name : m.path + " / " + m.name).c_str(), m.indices.size() / 3, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    }
}
