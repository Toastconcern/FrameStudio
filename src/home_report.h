#pragma once
// A home's health report: what it's made of, what it costs and what's wrong with it, from the loaded scene alone
// (no GPU). fenv-tool report prints it, Frame Studio shows it (Debug > Home Report), and Frame Home writes it to
// ~/frame-home/report.json after each load for the Environments panel's Debug page.
#include "hsl_scene.h"
#include "walkgrid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct HomeProblem {
    int level = 1;                      // 0 worth knowing, 1 costs speed or looks, 2 broken
    std::string what;                   // one line
    int mesh = -1;                      // the HslScene::meshes index it's about, if one
};

struct HomeReport {
    std::string name, format;
    size_t meshes = 0, triangles = 0, vertices = 0, empty = 0;
    size_t seeThrough = 0, seeThroughTris = 0, cutout = 0, skinned = 0, animated = 0, lightmapped = 0, sky = 0, programs = 0;
    size_t textures = 0;                // distinct
    double textureMB = 0, decodedMB = 0, geometryMB = 0;   // compressed (as the GPU gets it when it can), decoded RGBA, vertices + indices
    size_t spawns = 0, seats = 0, hotspots = 0, colliders = 0, wallSpots = 0, gravity = 0;
    double walkArea = -1;               // m², < 0: no walkable area known
    double loadSec = 0;                 // set by the caller
    struct Mesh { size_t tris; int mesh; };
    struct Tex { unsigned w, h, mips; double mb; size_t users; int mesh; };
    std::vector<Mesh> biggestMeshes;    // most triangles first
    std::vector<Tex> biggestTextures;   // most memory first
    std::vector<HomeProblem> problems;  // worst first
    std::vector<std::string> meshNames; // by HslScene::meshes index (for the lists above)

    int worst() const { int w = -1; for (const auto& p : problems) w = std::max(w, p.level); return w; }
    std::string meshName(int i) const { return i >= 0 && static_cast<size_t>(i) < meshNames.size() ? meshNames[i] : std::string(); }

    std::string text() const {
        std::string o;
        char b[512];
        std::snprintf(b, sizeof b, "%s%s%s\n", name.c_str(), format.empty() ? "" : "  ·  ", format.c_str());
        o += b;
        std::snprintf(b, sizeof b, "%zu meshes, %.2f M triangles, %.2f M vertices%s\n", meshes, triangles / 1e6, vertices / 1e6,
                      loadSec > 0 ? ("; loaded in " + num(loadSec, 1) + " s").c_str() : "");
        o += b;
        std::snprintf(b, sizeof b, "see-through %zu (%.2f M triangles), cut-out %zu, skinned %zu, animated %zu, lightmapped %zu, sky %zu, %zu shaders\n",
                      seeThrough, seeThroughTris / 1e6, cutout, skinned, animated, lightmapped, sky, programs);
        o += b;
        std::snprintf(b, sizeof b, "%zu textures: %.0f MB compressed, %.0f MB decoded; geometry %.0f MB\n", textures, textureMB, decodedMB, geometryMB);
        o += b;
        std::snprintf(b, sizeof b, "world: %zu spawns, %zu seats, %zu hotspots, %zu surfaces, %zu wall spots, %zu gravity zones", spawns, seats, hotspots,
                      colliders, wallSpots, gravity);
        o += b;
        o += walkArea >= 0 ? "; walkable " + num(walkArea, 0) + " m²\n" : "\n";
        if (!biggestMeshes.empty()) o += "most triangles:\n";
        for (const Mesh& m : biggestMeshes) { std::snprintf(b, sizeof b, "  %9zu  %s\n", m.tris, meshName(m.mesh).c_str()); o += b; }
        if (!biggestTextures.empty()) o += "biggest textures:\n";
        for (const Tex& t : biggestTextures) {
            std::snprintf(b, sizeof b, "  %6.1f MB  %ux%u, %u mip%s, %zu mesh%s  %s\n", t.mb, t.w, t.h, t.mips, t.mips == 1 ? "" : "s", t.users,
                          t.users == 1 ? "" : "es", meshName(t.mesh).c_str());
            o += b;
        }
        if (problems.empty()) o += "no problems found\n";
        for (const HomeProblem& p : problems) o += std::string(p.level >= 2 ? "PROBLEM  " : p.level == 1 ? "warning  " : "note     ") + p.what + "\n";
        return o;
    }
    std::string json() const {
        std::string o = "{\"name\":" + q(name) + ",\"format\":" + q(format);
        auto n = [&](const char* k, double v) { o += ",\""; o += k; o += "\":" + num(v, v == std::floor(v) ? 0 : 2); };
        n("meshes", meshes); n("triangles", triangles); n("vertices", vertices); n("seeThrough", seeThrough); n("seeThroughTris", seeThroughTris);
        n("cutout", cutout); n("skinned", skinned); n("animated", animated); n("lightmapped", lightmapped); n("programs", programs);
        n("textures", textures); n("textureMB", std::round(textureMB)); n("decodedMB", std::round(decodedMB)); n("geometryMB", std::round(geometryMB));
        n("spawns", spawns); n("seats", seats); n("colliders", colliders); n("wallSpots", wallSpots); n("walkArea", std::round(walkArea));
        n("loadSec", std::round(loadSec * 10) / 10);
        o += ",\"biggestMeshes\":[";
        for (size_t i = 0; i < biggestMeshes.size(); ++i) o += (i ? ",{\"name\":" : "{\"name\":") + q(meshName(biggestMeshes[i].mesh)) + ",\"tris\":" + num(biggestMeshes[i].tris, 0) + "}";
        o += "],\"problems\":[";
        for (size_t i = 0; i < problems.size(); ++i)
            o += (i ? ",{\"level\":" : "{\"level\":") + num(problems[i].level, 0) + ",\"what\":" + q(problems[i].what) + ",\"mesh\":" + q(meshName(problems[i].mesh)) + "}";
        return o + "]}\n";
    }

    static std::string num(double v, int digits) { char b[64]; std::snprintf(b, sizeof b, "%.*f", digits, std::isfinite(v) ? v : 0.0); return b; }
    static std::string q(const std::string& s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
            else if (c >= 0x20) o += static_cast<char>(c);
        }
        return o + "\"";
    }
};

// `walk`: the world as Frame Home walks it (walkWorld), for the walkable area and the spawns on it; null to leave
// those out.
inline HomeReport makeHomeReport(const HslScene& s, const HomeWorld* walk = nullptr) {
    HomeReport r;
    if (!s.apkPaths.empty()) {
        std::string f = s.fenvPath.empty() ? s.apkPaths.front() : s.fenvPath;
        const size_t sl = f.find_last_of("/\\");
        r.name = sl == std::string::npos ? f : f.substr(sl + 1);
    }
    if (!s.sources.empty()) r.format = s.sources.front().format + (s.sources.front().role.empty() ? "" : " " + s.sources.front().role);
    r.meshes = s.meshes.size();
    r.programs = s.programs.size();
    r.animated = s.animatedMeshes.size();
    r.meshNames.reserve(s.meshes.size());
    // Textures: each compressed source once (meshes share them), decoded RGBA per mesh (or its share source).
    struct TexUse { unsigned w, h, mips; double mb; size_t users; int mesh; };
    std::map<const void*, TexUse> tex;
    auto useSrc = [&](const std::shared_ptr<const TexSource>& t, int mesh) {
        if (!t) return;
        auto [it, fresh] = tex.try_emplace(t.get(), TexUse{t->w, t->h, t->mips, t->astc.size() / 1048576.0, 0, mesh});
        ++it->second.users;
        if (fresh) r.textureMB += it->second.mb;
    };
    auto useRgba = [&](const std::vector<u8>& px, unsigned w, unsigned h, int mesh) {
        if (px.empty()) return;
        auto [it, fresh] = tex.try_emplace(px.data(), TexUse{w, h, 1, px.size() / 1048576.0, 0, mesh});
        ++it->second.users;
        if (fresh) r.decodedMB += it->second.mb;
    };
    size_t nanMeshes = 0, badIndexMeshes = 0, degenerateMeshes = 0;
    for (size_t i = 0; i < s.meshes.size(); ++i) {
        const MeshData& m = s.meshes[i];
        const int mi = static_cast<int>(i);
        r.meshNames.push_back(m.name.empty() ? "(unnamed " + std::to_string(i) + ")" : m.name);
        const size_t tris = m.indices.size() / 3, verts = m.positions.size() / 3;
        r.triangles += tris;
        r.vertices += verts;
        if (!tris) ++r.empty;
        r.geometryMB += ((m.positions.size() + m.uvs.size() + m.uvs3.size() + m.uvs4.size()) * 4 + m.indices.size() * 4 + m.colors.size()) / 1048576.0;
        if (m.useBlend || m.additive) { ++r.seeThrough; r.seeThroughTris += tris; }
        r.cutout += m.alphaTest;
        r.skinned += !m.boneIndices.empty();
        r.lightmapped += m.hasLmHdr() || m.hasLightmap || m.lmLdrSrc != nullptr;
        r.sky += m.isSkybox;
        r.biggestMeshes.push_back({tris, mi});
        for (const auto* t : {&m.texSrc, &m.normalSrc, &m.ormSrc, &m.emissiveSrc, &m.lmSrc, &m.lmLdrSrc, &m.cubeSrc}) useSrc(*t, mi);
        for (const auto& nt : m.namedTex) useSrc(nt.second, mi);
        for (const auto& nh : m.namedHdr) useSrc(nh.second.src, mi);
        if (m.texShareSrc < 0) {
            if (!m.texSrc) useRgba(m.texRGBA, m.texW, m.texH, mi);
            useRgba(m.normalRGBA, m.normalW, m.normalH, mi);
            useRgba(m.ormRGBA, m.ormW, m.ormH, mi);
            useRgba(m.emissiveRGBA, m.emissiveW, m.emissiveH, mi);
            useRgba(m.lmRGBA, m.lmW, m.lmH, mi);
        }
        // Broken geometry: positions that aren't numbers, indices past the vertices, triangles with no area.
        bool nan = false;
        for (float v : m.positions) if (!std::isfinite(v)) { nan = true; break; }
        bool badIndex = false;
        size_t degenerate = 0;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const u32 a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
            if (a >= verts || b >= verts || c >= verts) { badIndex = true; break; }
            if (a == b || b == c || a == c) { ++degenerate; continue; }
            const float* pa = &m.positions[a * 3]; const float* pb = &m.positions[b * 3]; const float* pc = &m.positions[c * 3];
            const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2], vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
            const float cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            if (cx * cx + cy * cy + cz * cz < 1e-18f) ++degenerate;
        }
        if (nan && nanMeshes++ < 5) r.problems.push_back({2, "Positions that aren't numbers in " + r.meshNames.back() + ": it may not draw, or draw as spikes", mi});
        if (badIndex && badIndexMeshes++ < 5) r.problems.push_back({2, "Triangles pointing past the vertices in " + r.meshNames.back() + ": it can't be drawn safely", mi});
        if (tris >= 1000 && degenerate * 5 > tris && degenerateMeshes++ < 3)
            r.problems.push_back({0, r.meshNames.back() + ": " + std::to_string(degenerate * 100 / tris) + "% of its triangles have no area (wasted work)", mi});
        if (tris > 500000)
            r.problems.push_back({1, r.meshNames.back() + " alone has " + HomeReport::num(tris / 1e6, 2) + " M triangles", mi});
        if ((m.useBlend || m.additive) && tris > 60000)
            r.problems.push_back({1, "See-through " + r.meshNames.back() + " has " + std::to_string(tris / 1000) + " k triangles: blended surfaces are drawn back to front, every layer paid for", mi});
    }
    r.textures = tex.size();
    // Textures the GPU can't sample well, one line per kind: the count, and the biggest of them.
    size_t huge = 0, noMips = 0;
    const TexUse* hugeTop = nullptr;
    const TexUse* noMipsTop = nullptr;
    std::map<const void*, bool> base;                  // compressed base textures (the ones that get their mips made)
    for (const MeshData& m : s.meshes) if (m.texSrc) base[m.texSrc.get()] = true;
    for (const auto& kv : tex) {
        const TexUse& t = kv.second;
        r.biggestTextures.push_back({t.w, t.h, t.mips, t.mb, t.users, t.mesh});
        const unsigned side = std::max(t.w, t.h);
        if (side > 4096) { ++huge; if (!hugeTop || t.mb > hugeTop->mb) hugeTop = &t; }
        if (t.mips <= 1 && side > 256 && base.count(kv.first)) { ++noMips; if (!noMipsTop || t.mb > noMipsTop->mb) noMipsTop = &t; }
    }
    auto dims = [](const TexUse* t) { return std::to_string(t->w) + "x" + std::to_string(t->h); };
    if (noMips)
        r.problems.push_back({1, std::to_string(noMips) + (noMips == 1 ? " texture has" : " textures have") + " no smaller versions (the biggest " +
                                     dims(noMipsTop) + ", " + r.meshName(noMipsTop->mesh) + "): Frame Home makes them at load, which loads slower",
                              noMipsTop->mesh});
    if (huge)
        r.problems.push_back({0, std::to_string(huge) + (huge == 1 ? " texture is" : " textures are") + " bigger than 4096 (" + dims(hugeTop) + ", " +
                                     r.meshName(hugeTop->mesh) + "): GPUs that can't read them compressed get them at 2048", hugeTop->mesh});
    std::sort(r.biggestMeshes.begin(), r.biggestMeshes.end(), [](const auto& a, const auto& b) { return a.tris > b.tris; });
    if (r.biggestMeshes.size() > 8) r.biggestMeshes.resize(8);
    std::sort(r.biggestTextures.begin(), r.biggestTextures.end(), [](const auto& a, const auto& b) { return a.mb > b.mb; });
    if (r.biggestTextures.size() > 8) r.biggestTextures.resize(8);
    // The whole home.
    if (r.triangles > 1500000)
        r.problems.push_back({1, HomeReport::num(r.triangles / 1e6, 1) + " M triangles in all: the Frame keeps its frame rate best under about 1 M in view"});
    if (r.meshes > 2500) r.problems.push_back({1, std::to_string(r.meshes) + " separate meshes: each one in view is a draw, and draws cost CPU time every frame"});
    if (r.seeThroughTris > 300000)
        r.problems.push_back({1, HomeReport::num(r.seeThroughTris / 1e6, 2) + " M see-through triangles: overdraw is likely what's slow (try Debug view: Overdraw)"});
    if (r.textureMB + r.decodedMB > 1500)
        r.problems.push_back({1, HomeReport::num(r.textureMB + r.decodedMB, 0) + " MB of textures: loads slowly and leaves little memory for the apps in the Steam menu"});
    if (r.empty) r.problems.push_back({0, std::to_string(r.empty) + " meshes with no triangles"});
    // The world.
    const HomeWorld& w = walk ? *walk : s.world;
    r.spawns = w.spawns.size(); r.seats = w.seats.size(); r.hotspots = w.hotspots.size(); r.colliders = w.colliders.size();
    r.wallSpots = w.wallSpots.size(); r.gravity = w.gravity.size();
    const WalkGrid* g = walk ? walk->walkGridCache.get() : nullptr;
    float floorY = 0;
    if (w.spawns.empty() && w.gravity.empty() && (!g || !g->floorAt(0, 0, 0, 0.5f, 1.0f, floorY)))
        r.problems.push_back({g ? 1 : 0, "No spawn point: you start at the home's centre, which isn't on walkable floor"});
    if (g) {
        r.walkArea = g->walkArea;
        if (w.gravity.empty() && g->walkArea < 2) r.problems.push_back({2, "Almost nowhere to walk (" + HomeReport::num(g->walkArea, 1) + " m²): the floor wasn't found"});
        for (size_t i = 0; i < w.spawns.size() && w.gravity.empty(); ++i) {
            const WorldPoint& sp = w.spawns[i];
            float h = 0;
            if (!g->floorAt(sp.pos[0], sp.pos[2], sp.pos[1], 0.5f, 1.0f, h))
                r.problems.push_back({1, "Spawn " + (sp.name.empty() ? std::to_string(i) : sp.name) + " isn't on walkable floor: you may start stuck or falling"});
        }
    }
    std::stable_sort(r.problems.begin(), r.problems.end(), [](const HomeProblem& a, const HomeProblem& b) { return a.level > b.level; });
    return r;
}
