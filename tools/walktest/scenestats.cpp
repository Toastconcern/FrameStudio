// What a home costs to draw: meshes, triangles, texture memory, blended (see-through) meshes and their triangles, the
// biggest meshes and textures.
#include <set>
#include "hsl_scene.h"
#include <algorithm>
#include <cstdio>
int main(int argc, char** argv) {
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene s; std::string err;
    if (!loadHslScene(apks, s, err)) { std::printf("LOAD FAIL %s\n", err.c_str()); return 1; }
    size_t tris = 0, blendTris = 0, blend = 0, texBytes = 0, lm = 0, alphaTest = 0, skinned = 0;
    std::vector<std::pair<size_t, std::string>> big, tex;
    for (const MeshData& md : s.meshes) {
        const size_t t = md.indices.size() / 3;
        tris += t;
        if (md.useBlend || md.additive) { ++blend; blendTris += t; }
        alphaTest += md.alphaTest;
        skinned += !md.boneIndices.empty();
        lm += md.hasLmHdr() || md.hasLightmap;
        big.push_back({t, md.name});
        const size_t b = md.texRGBA.size();
        texBytes += b;
        if (b) tex.push_back({b, md.name + " " + std::to_string(md.texW) + "x" + std::to_string(md.texH) + (md.texSrc ? " +src" : " no src") + (md.useBlend ? " blend" : "")});
    }
    std::set<const TexSource*> srcs; size_t srcBytes = 0;
    for (const MeshData& md : s.meshes)
        if (md.texSrc && srcs.insert(md.texSrc.get()).second) srcBytes += md.texSrc->astc.size();
    // Everything a loaded home holds, by kind (shared things counted once).
    std::set<const void*> seen;
    auto once = [&](const void* p) { return p && seen.insert(p).second; };
    auto srcB = [&](const std::shared_ptr<const TexSource>& t) -> size_t { return t && once(t.get()) ? t->astc.size() : 0; };
    size_t geo = 0, rgbaOther = 0, lmB = 0, cube = 0, refl = 0, astcAll = 0, half = 0, srcAstcB = 0;
    for (const MeshData& md : s.meshes) {
        geo += (md.positions.size() + md.uvs.size() + md.uvs3.size() + md.uvs4.size()) * 4 + md.indices.size() * 4 + md.colors.size();
        rgbaOther += md.normalRGBA.size() + md.ormRGBA.size() + md.emissiveRGBA.size() + md.vatRaw.size();
        lmB += md.lmRGBA.size() + (md.lmHalf && once(md.lmHalf.get()) ? md.lmHalf->size() * 2 : 0);
        cube += md.cubeRGBA.size();
        refl += md.reflCubeHalf.size() * 2;
        srcAstcB += md.srcAstc.size();
        astcAll += srcB(md.texSrc) + srcB(md.normalSrc) + srcB(md.ormSrc) + srcB(md.emissiveSrc) + srcB(md.lmSrc) + srcB(md.cubeSrc) + srcB(md.lmLdrSrc);
        for (const auto& n : md.namedTex) astcAll += srcB(n.second);
        for (const auto& n : md.namedHdr) { astcAll += srcB(n.second.src); if (n.second.half && once(n.second.half.get())) half += n.second.half->size() * 2; }
    }
    std::printf("compressed sources: %zu, %.0f MB ASTC\n", srcs.size(), srcBytes / 1048576.0);
    std::printf("held MB: geometry %.0f, other RGBA %.0f, lightmaps %.0f, cubes %.0f, refl %.0f, all ASTC %.0f, half %.0f, srcAstc %.0f\n",
                geo / 1048576.0, rgbaOther / 1048576.0, lmB / 1048576.0, cube / 1048576.0, refl / 1048576.0, astcAll / 1048576.0, half / 1048576.0, srcAstcB / 1048576.0);
    std::sort(big.rbegin(), big.rend());
    std::sort(tex.rbegin(), tex.rend());
    std::printf("%zu meshes, %zu tris; %zu blended (%zu tris), %zu cut-out, %zu skinned, %zu lightmapped; base textures ~%.0f MB RGBA\n",
                s.meshes.size(), tris, blend, blendTris, alphaTest, skinned, lm, texBytes / 1048576.0);
    for (size_t i = 0; i < big.size() && i < 5; ++i) std::printf("   mesh %8zu tris  %s\n", big[i].first, big[i].second.c_str());
    for (size_t i = 0; i < tex.size() && i < 4; ++i) std::printf("   tex %6.1f MB  %s\n", tex[i].first / 1048576.0, tex[i].second.c_str());
}
