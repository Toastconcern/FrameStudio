// A Clay / glTF home's animation from its APK against the same home baked into a .fenv (anim_bake.h): both loaded, both
// played at the same times (spot times, past the loop's end, and a 1/120 s sweep), every mesh compared after each step —
// positions, uvs, animXform, uvXform, tint, skin joints and box, and whether each rewrite counter moved. Also the
// meshes as loaded (before any step), every field the renderer reads.
//   animcheck <home.apk> <home.fenv> [sweep seconds = 4]
// Exit 0: bit-identical (or within 1e-6); 2: different.
#include "hsl_scene.h"
#include "anim_bake.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Diff {
    size_t compared = 0, exact = 0, near = 0, off = 0;
    double worst = 0;
    std::string where;
};

void cmpF(Diff& d, const float* a, const float* b, size_t n, const std::string& what) {
    for (size_t i = 0; i < n; ++i) {
        ++d.compared;
        if (std::memcmp(&a[i], &b[i], 4) == 0) { ++d.exact; continue; }
        const double e = std::fabs(static_cast<double>(a[i]) - b[i]);
        if (e <= 1e-6) ++d.near;
        else ++d.off;
        if (e > d.worst || std::isnan(e)) { d.worst = std::isnan(e) ? 1e30 : e; d.where = what + "[" + std::to_string(i) + "]"; }
    }
}
template <typename T> void cmpV(Diff& d, const std::vector<T>& a, const std::vector<T>& b, const std::string& what) {
    if (a.size() != b.size()) { ++d.off; d.worst = 1e30; d.where = what + " size " + std::to_string(a.size()) + " vs " + std::to_string(b.size()); return; }
    if constexpr (std::is_same_v<T, float>) cmpF(d, a.data(), b.data(), a.size(), what);
    else { ++d.compared; if (!a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T))) { ++d.off; d.worst = 1e30; d.where = what; } else ++d.exact; }
}

// What the animation writes, and what the renderer then reads.
void cmpAnimated(Diff& d, const MeshData& a, const MeshData& b, const std::string& n) {
    cmpV(d, a.positions, b.positions, n + ".positions");
    cmpV(d, a.uvs, b.uvs, n + ".uvs");
    cmpF(d, a.animXform, b.animXform, 16, n + ".animXform");
    cmpF(d, a.uvXform, b.uvXform, 6, n + ".uvXform");
    cmpF(d, a.curTint, b.curTint, 4, n + ".curTint");
    cmpV(d, a.skinJoints, b.skinJoints, n + ".skinJoints");
    cmpF(d, a.skinBox, b.skinBox, 6, n + ".skinBox");
}

// Everything else a load gives the renderer (indices: `cut` when the bake's optimizer may have thinned the mesh).
void cmpLoaded(Diff& d, const MeshData& a, const MeshData& b, const std::string& n, bool cut) {
    cmpAnimated(d, a, b, n);
    cmpV(d, a.skinBindPos, b.skinBindPos, n + ".skinBindPos");
    cmpV(d, a.boneIndices, b.boneIndices, n + ".boneIndices");
    cmpV(d, a.boneWeights, b.boneWeights, n + ".boneWeights");
    cmpV(d, a.colors, b.colors, n + ".colors");
    cmpV(d, a.uvs2, b.uvs2, n + ".uvs2");
    cmpV(d, a.normals, b.normals, n + ".normals");
    if (!cut) cmpV(d, a.indices, b.indices, n + ".indices");
    cmpF(d, a.worldMatrix, b.worldMatrix, 16, n + ".worldMatrix");
    cmpF(d, a.tint, b.tint, 4, n + ".tint");
    if (a.shaderPath != b.shaderPath || a.useBlend != b.useBlend || a.alphaTest != b.alphaTest || a.hasLightmap != b.hasLightmap ||
        a.dynamicVerts != b.dynamicVerts || a.nVerts != b.nVerts) {
        ++d.off; d.worst = 1e30; d.where = n + " flags";
    }
}

struct Gens { uint32_t pos, uv, skin; };
std::vector<Gens> gens(const HslScene& s) {
    std::vector<Gens> g;
    for (const MeshData& m : s.meshes) g.push_back({m.posGen, m.uvGen, m.skinGen});
    return g;
}

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: animcheck <home.apk> <home.fenv> [sweep seconds]\n"); return 1; }
    const double sweep = argc > 3 ? std::atof(argv[3]) : 4.0;
    HslScene a, b;
    std::string err;
    double t0 = now();
    if (!loadHslScene(std::string(argv[1]), a, err)) { std::fprintf(stderr, "apk: %s\n", err.c_str()); return 1; }
    const double loadA = now() - t0;
    t0 = now();
    if (!loadHslScene(std::string(argv[2]), b, err)) { std::fprintf(stderr, "fenv: %s\n", err.c_str()); return 1; }
    const double loadB = now() - t0;
    std::printf("load: apk %.2f s, fenv %.2f s; %zu meshes; animation %s / %s, %.2f s loop, %zu animated\n", loadA, loadB, a.meshes.size(),
                a.animate ? "yes" : "no", b.animate ? "yes" : "no", a.animDuration, a.animatedMeshes.size());
    if (a.meshes.size() != b.meshes.size() || !a.animate != !b.animate || a.animatedMeshes != b.animatedMeshes || a.animDuration != b.animDuration) {
        std::printf("DIFFERENT: mesh count / animation / animated list / duration\n");
        return 2;
    }
    std::vector<char> target(a.meshes.size(), 0);
    if (a.saveAnimation) {
        std::vector<u8> bytes;
        a.saveAnimation(bytes);
        for (int m : animbake::targets(bytes)) target[m] = 1;
        std::vector<u8> bb;
        b.saveAnimation(bb);
        std::printf("animation data: %.2f MB as parsed, %.2f MB stored; %zu meshes written\n", bytes.size() / 1048576.0, bb.size() / 1048576.0,
                    animbake::targets(bytes).size());
    }
    int bad = 0;
    {
        Diff d;
        size_t cut = 0;
        for (size_t m = 0; m < a.meshes.size(); ++m) {
            const bool thinned = !target[m] && a.meshes[m].indices != b.meshes[m].indices;
            cut += thinned;
            cmpLoaded(d, a.meshes[m], b.meshes[m], std::to_string(m) + ":" + a.meshes[m].name, thinned);
        }
        std::printf("as loaded: %zu values, %zu exact, %zu within 1e-6, %zu off (worst %g at %s); %zu still meshes thinned by the optimizer\n", d.compared,
                    d.exact, d.near, d.off, d.worst, d.where.c_str(), cut);
        bad |= d.off != 0;
    }
    if (!a.animate) return bad ? 2 : 0;
    const float dur = a.animDuration > 0 ? a.animDuration : 10.0f;
    std::vector<float> times = {0.0f, 0.37f, 1.0f, 2.5f, 7.3f, dur - 1e-3f, dur, dur + 0.01f, dur * 1.5f, dur * 2 + 0.25f, 100.3f, 1234.5f, 0.0f};
    for (double t = 0; t < sweep; t += 1.0 / 120) times.push_back(static_cast<float>(t));
    for (double t = dur - 0.5; t < dur + 0.5; t += 1.0 / 120) times.push_back(static_cast<float>(t));
    Diff d;
    size_t genOff = 0;
    double msA = 0, msB = 0;
    for (float t : times) {
        const std::vector<Gens> ga0 = gens(a), gb0 = gens(b);
        double s = now();
        a.animate(t, a.meshes);
        msA += now() - s;
        s = now();
        b.animate(t, b.meshes);
        msB += now() - s;
        const std::vector<Gens> ga1 = gens(a), gb1 = gens(b);
        for (size_t m = 0; m < a.meshes.size(); ++m) {
            if (!target[m]) continue;
            cmpAnimated(d, a.meshes[m], b.meshes[m], "t=" + std::to_string(t) + " " + std::to_string(m) + ":" + a.meshes[m].name);
            if ((ga1[m].pos != ga0[m].pos) != (gb1[m].pos != gb0[m].pos) || (ga1[m].uv != ga0[m].uv) != (gb1[m].uv != gb0[m].uv) ||
                (ga1[m].skin != ga0[m].skin) != (gb1[m].skin != gb0[m].skin)) {
                if (!genOff) std::printf("  rewrite counters differ first at t=%g mesh %zu %s\n", t, m, a.meshes[m].name.c_str());
                ++genOff;
            }
        }
    }
    std::printf("played %zu steps: %zu values, %zu exact, %zu within 1e-6, %zu off (worst %g at %s); rewrite counters differ %zu times; "
                "step %.3f ms (apk) / %.3f ms (fenv)\n", times.size(), d.compared, d.exact, d.near, d.off, d.worst, d.where.c_str(), genOff,
                msA * 1000 / times.size(), msB * 1000 / times.size());
    bad |= d.off != 0 || genOff != 0;
    std::printf("%s\n", bad ? "DIFFERENT" : "IDENTICAL");
    return bad ? 2 : 0;
}
