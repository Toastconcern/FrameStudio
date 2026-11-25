// grasscheck: what each mesh of a home is as grass. For every mesh drawn see-through, named as a plant or ground, or
// with a mostly green texture: its name's verdict, how it's drawn, its shape (how much faces up or stands upright, how
// tall, in how many separate pieces) and its texture where its faces are (how much is green, how much is cut away),
// and what the walkable area takes it for now (walkgrid.h WalkSource: cut-out = grown, walked through; noFloor).
// grasscheck <apk> [all]   (all: every mesh, not only the grass-like ones)
#include "hsl_scene.h"
#include "loaders/rendtxtr_parser.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

const TexSource* baseTexture(const MeshData& md) {
    auto usable = [](const TexSource* t) { return t && t->faces == 1 && !t->hdr && t->w >= 16 && t->h >= 16; };
    if (usable(md.texSrc.get())) return md.texSrc.get();
    const TexSource* best = nullptr;
    for (const auto& nt : md.namedTex) {
        const TexSource* t = nt.second.get();
        if (!usable(t) || t == md.lmLdrSrc.get() || t == md.lmSrc.get()) continue;
        if (!best || static_cast<size_t>(t->w) * t->h > static_cast<size_t>(best->w) * best->h) best = t;
    }
    return best;
}

// The texture's level at most 256 across, decoded (RGBA8).
bool smallLevel(const TexSource& t, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h) {
    size_t offset = 0;
    w = t.w; h = t.h;
    uint32_t level = 0;
    auto bytes = [&](uint32_t lw, uint32_t lh) { return static_cast<size_t>((lw + t.bw - 1) / t.bw) * ((lh + t.bh - 1) / t.bh) * 16; };
    for (; level + 1 < t.mips && std::max(w, h) > 256; ++level) { offset += bytes(w, h); w = std::max(1u, w / 2); h = std::max(1u, h / 2); }
    size_t n = bytes(w, h);
    if (level == 0 && t.astc.size() == static_cast<size_t>(w) * h * 4) n = t.astc.size();
    if (offset + n > t.astc.size()) return false;
    return astc::decodeASTC(t.astc.data() + offset, static_cast<u32>(n), w, h, t.bw, t.bh, px, false, 0) && px.size() >= static_cast<size_t>(w) * h * 4;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "grasscheck <apk> [all]\n"); return 1; }
    const bool all = argc > 2 && std::strcmp(argv[2], "all") == 0;
    HslScene s;
    std::string err;
    if (!loadHslScene(std::string(argv[1]), s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    const std::vector<WalkSource> src = drawnWalkSources(s, s.world);
    std::vector<const WalkSource*> srcOf(s.meshes.size(), nullptr);
    for (const WalkSource& w : src) if (w.tag >= 0 && static_cast<size_t>(w.tag) < srcOf.size()) srcOf[w.tag] = &w;
    std::printf("%-44s %-6s %-6s %5s %5s %5s %6s %6s %5s %5s  %s\n", "mesh", "name", "drawn", "up%", "vert%", "tall", "pieces", "area", "green", "holes", "walk now");
    std::mt19937 rng(7);
    for (size_t m = 0; m < s.meshes.size(); ++m) {
        const MeshData& md = s.meshes[m];
        if (md.indices.empty() || md.positions.empty() || md.isSkybox) continue;
        const std::vector<std::string> words = walkNameWords(md.name);
        const char* nameV = walkNameIsGround(md.name) ? "ground" : walkNameIsPlant(words, true) ? "soft" : walkNameIsPlant(words) ? "plant" : "-";
        const char* drawn = md.alphaTest ? "cutout" : md.useBlend ? "blend" : "solid";
        float world[16];
        if (md.hasWorldMatrix) std::memcpy(world, md.worldMatrix, sizeof world); else trsToMat4(md.transform, world);
        const size_t nv = md.positions.size() / 3;
        std::vector<float> wp(nv * 3);
        for (size_t v = 0; v < nv; ++v)
            for (int k = 0; k < 3; ++k) wp[v * 3 + k] = world[k] * md.positions[v * 3] + world[4 + k] * md.positions[v * 3 + 1] + world[8 + k] * md.positions[v * 3 + 2] + world[12 + k];
        double area = 0, up = 0, vert = 0;
        std::vector<double> triArea(md.indices.size() / 3, 0);
        std::vector<uint32_t> parent(nv);
        std::iota(parent.begin(), parent.end(), 0u);
        auto root = [&](uint32_t x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            const uint32_t a = md.indices[t], b = md.indices[t + 1], c = md.indices[t + 2];
            if (a >= nv || b >= nv || c >= nv) continue;
            parent[root(b)] = root(a); parent[root(c)] = root(a);
            const float* p = &wp[a * 3]; const float* q = &wp[b * 3]; const float* r = &wp[c * 3];
            const float e1[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]}, e2[3] = {r[0] - p[0], r[1] - p[1], r[2] - p[2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const double L = std::sqrt(static_cast<double>(n[0]) * n[0] + static_cast<double>(n[1]) * n[1] + static_cast<double>(n[2]) * n[2]);
            triArea[t / 3] = L * 0.5;
            area += L * 0.5;
            if (L > 0 && std::fabs(n[1]) / L > 0.9) up += L * 0.5;
            if (L > 0 && std::fabs(n[1]) / L < 0.4) vert += L * 0.5;
        }
        // pieces (by shared vertex indices, and by position too: cards are often split per vertex) and their heights
        std::vector<float> plo(nv, 1e30f), phi(nv, -1e30f);
        for (size_t v = 0; v < nv; ++v) { const uint32_t r0 = root(static_cast<uint32_t>(v)); plo[r0] = std::min(plo[r0], wp[v * 3 + 1]); phi[r0] = std::max(phi[r0], wp[v * 3 + 1]); }
        std::vector<float> heights;
        for (size_t v = 0; v < nv; ++v) if (root(static_cast<uint32_t>(v)) == v && phi[v] >= plo[v]) heights.push_back(phi[v] - plo[v]);
        std::sort(heights.begin(), heights.end());
        const float tall = heights.empty() ? 0 : heights[heights.size() / 2];
        // the texture where its faces are: area-weighted samples
        float green = -1, holes = -1;
        if (const TexSource* t = baseTexture(md); t && md.uvs.size() >= nv * 2 && area > 0) {
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0;
            if (smallLevel(*t, px, w, h)) {
                std::discrete_distribution<size_t> pick(triArea.begin(), triArea.end());
                std::uniform_real_distribution<float> u01(0, 1);
                int g = 0, seen = 0, cut = 0;
                for (int i = 0; i < 1500; ++i) {
                    const size_t tri = pick(rng);
                    float a = u01(rng), b = u01(rng);
                    if (a + b > 1) { a = 1 - a; b = 1 - b; }
                    const uint32_t i0 = md.indices[tri * 3], i1 = md.indices[tri * 3 + 1], i2 = md.indices[tri * 3 + 2];
                    float uu = md.uvs[i0 * 2] + a * (md.uvs[i1 * 2] - md.uvs[i0 * 2]) + b * (md.uvs[i2 * 2] - md.uvs[i0 * 2]);
                    float vv = md.uvs[i0 * 2 + 1] + a * (md.uvs[i1 * 2 + 1] - md.uvs[i0 * 2 + 1]) + b * (md.uvs[i2 * 2 + 1] - md.uvs[i0 * 2 + 1]);
                    uu -= std::floor(uu); vv -= std::floor(vv);
                    const uint32_t x = std::min(w - 1, static_cast<uint32_t>(uu * w)), y = std::min(h - 1, static_cast<uint32_t>(vv * h));
                    const uint8_t* c = &px[(static_cast<size_t>(y) * w + x) * 4];
                    if (c[3] < 128) { ++cut; continue; }
                    ++seen;
                    const int r = c[0], gg = c[1], bb = c[2], mx = std::max({r, gg, bb}), mn = std::min({r, gg, bb});
                    if (gg == mx && mx > 30 && (mx - mn) > 0.18f * mx && gg > r + 4) ++g;   // (hue green to yellow-green, not grey)
                }
                if (seen) green = static_cast<float>(g) / seen;
                holes = static_cast<float>(cut) / 1500;
            }
        }
        const WalkSource* w = srcOf[m];
        const bool grassy = *nameV != '-' || md.alphaTest || md.useBlend || green > 0.3f;
        if (!all && !grassy) continue;
        const char* now = !w ? "not walked" : w->cutout ? "grown: walked through" : w->noFloor ? "blocks, never floor" : "solid: floor/wall";
        std::string name = md.name.size() > 44 ? md.name.substr(md.name.size() - 44) : md.name;
        std::printf("%-44s %-6s %-6s %5.0f %5.0f %5.2f %6zu %6.1f %5s %5s  %s\n", name.c_str(), nameV, drawn, area > 0 ? 100 * up / area : 0, area > 0 ? 100 * vert / area : 0,
                    tall, heights.size(), area, green < 0 ? "-" : (std::to_string(static_cast<int>(green * 100)) + "%").c_str(),
                    holes < 0 ? "-" : (std::to_string(static_cast<int>(holes * 100)) + "%").c_str(), now);
    }
    // Auto-generate's order: what's grass (by its look) labelled Walk through (a Trigger surface linked to it, as
    // Studio's), then the area made: it leaves them out
    {
        WalkReport before, after;
        bakeWalkGrid(s, s.world, WalkParams{}, &before);
        HomeWorld w = s.world;
        int labelled = 0;
        for (size_t m = 0; m < s.meshes.size(); ++m) {
            const MeshData& md = s.meshes[m];
            if (md.indices.empty() || md.isSkybox) continue;
            float f[16];
            if (md.hasWorldMatrix) std::memcpy(f, md.worldMatrix, sizeof f); else trsToMat4(md.transform, f);
            const MeshLook look = meshLook(md, f);
            if (!look.grass || look.flat || walkNameIsStructure(md.name)) continue;
            WorldCollider c;
            c.name = md.name;
            c.kind = WorldCollider::Mesh;
            c.role = WorldCollider::Trigger;
            c.mesh = std::to_string(m) + ":" + md.name;
            std::copy_n(f, 16, c.world);
            c.positions = md.positions;
            c.indices = md.indices;
            w.colliders.push_back(std::move(c));
            ++labelled;
        }
        bakeWalkGrid(s, w, WalkParams{}, &after);
        std::printf("\nAuto-generate: %d grass meshes labelled Walk through; navmesh %.1f m2 before, %.1f m2 with them walked through\n", labelled, before.walkArea, after.walkArea);
    }
    return 0;
}
