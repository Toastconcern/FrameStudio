#include "scene_bake.h"
#include "visibility_bake.h"
#include "surface_mesh.h"
#include "walkgrid.h"
#include "tex_mips.h"
#include "tex_cache.h"
#include "core/parallel.h"
#include "shader_bake.h"
#include "bake_io.h"
#include "anim_bake.h"
#include <atomic>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace {

using bakeio::Writer;
using bakeio::Reader;

// Shared buffers (one texture chain serves many meshes) are written once, then referenced by index.
template <typename T> struct Table {
    std::map<const T*, u32> index;
    std::vector<const T*> items;
    u32 ref(const std::shared_ptr<const T>& p) {
        if (!p) return 0xFFFFFFFFu;
        auto it = index.find(p.get());
        if (it != index.end()) return it->second;
        index[p.get()] = static_cast<u32>(items.size());
        items.push_back(p.get());
        return static_cast<u32>(items.size() - 1);
    }
};

void writeMesh(Writer& w, const MeshData& m, Table<TexSource>& tex, Table<std::vector<u16>>& half, const std::vector<u32>* indices = nullptr) {
    w.str(m.name);
    w.str(m.path);
    w.u(static_cast<u32>(m.components.size()));
    for (const EnvComponent& c : m.components) {
        w.str(c.cls); w.str(c.shortCls); w.pod(c.version);
        w.u(static_cast<u32>(c.fields.size()));
        for (const auto& kv : c.fields) { w.str(kv.first); w.str(kv.second); }
    }
    w.b(m.isSkybox);
    w.vec(m.positions); w.vec(m.uvs); w.vec(m.colors); w.vec(m.uvs3); w.vec(m.uvs4); w.vec(indices ? *indices : m.indices);
    w.u(m.nVerts); w.u(indices ? static_cast<u32>(indices->size()) : m.nIdx);
    w.vec(m.texRGBA); w.u(m.texW); w.u(m.texH); w.b(m.hasTexture); w.pod(m.texShareSrc);
    w.vec(m.srcAstc); w.u(m.srcAstcBw); w.u(m.srcAstcBh); w.u(m.srcAstcMips);
    w.vec(m.normalRGBA); w.u(m.normalW); w.u(m.normalH); w.b(m.hasNormal);
    w.vec(m.ormRGBA); w.u(m.ormW); w.u(m.ormH); w.b(m.hasOrm);
    w.str(m.ormTexName);
    w.vec(m.emissiveRGBA); w.u(m.emissiveW); w.u(m.emissiveH); w.b(m.hasEmissive);
    w.vec(m.lmRGBA); w.u(m.lmW); w.u(m.lmH); w.b(m.hasLightmap);
    w.u(half.ref(m.lmHalf)); w.u(tex.ref(m.lmSrc)); w.u(tex.ref(m.lmLdrSrc));
    w.vec(m.cubeRGBA); w.u(m.cubeSize); w.u(tex.ref(m.cubeSrc));
    w.u(tex.ref(m.texSrc)); w.u(tex.ref(m.normalSrc)); w.u(tex.ref(m.ormSrc)); w.u(tex.ref(m.emissiveSrc));
    w.u(static_cast<u32>(m.namedTex.size()));
    for (const auto& nt : m.namedTex) { w.u(nt.first); w.u(tex.ref(nt.second)); }
    w.u(static_cast<u32>(m.namedHdr.size()));
    for (const auto& nh : m.namedHdr) { w.u(nh.first); w.u(half.ref(nh.second.half)); w.u(nh.second.w); w.u(nh.second.h); w.u(tex.ref(nh.second.src)); }
    w.vec(m.reflCubeHalf); w.u(m.reflCubeSize); w.u(m.reflCubeMips); w.u(tex.ref(m.reflCubeSrc));
    w.fa(m.lmUvScaleOffset, 4);
    w.vec(m.vatRaw); w.u(m.vatW); w.u(m.vatH); w.b(m.hasVat);
    w.fa(m.tint, 4);
    w.f(m.atlasCellIndex); w.f(m.vatTrackIndex); w.f(m.vatRateFactor); w.f(m.vatTimeOffset);
    w.u(static_cast<u32>(m.matOverrides.size()));
    for (const MatOverride& o : m.matOverrides) { w.str(o.name); w.fa(o.v, 4); }
    w.pod(m.shaderIng); w.str(m.shaderPath); w.str(m.meshPath); w.b(m.tiled);
    w.vec(m.matParamsBlob); w.vec(m.constParams); w.vec(m.texSlots);
    w.vec(m.astcRaw); w.u(m.astcBw); w.u(m.astcBh);
    w.pod(m.transform);
    w.fa(m.worldMatrix, 16); w.b(m.hasWorldMatrix);
    w.vec(m.boneIndices); w.vec(m.boneWeights); w.b(m.hasBones); w.vec(m.bonePalette);
    w.b(m.useBlend); w.b(m.additive); w.b(m.alphaTest); w.f(m.alphaCutoff); w.pod(m.srcAlphaMode); w.f(m.srcFactorA);
    w.pod(m.overlayKind); w.b(m.doubleSided); w.b(m.dynamicVerts); w.pod(m.gltfMeshIndex);
    w.b(m.iblLit); w.b(m.iblFullSpec); w.b(m.bakeLightmapVtx);
    w.vec(m.uvs2); w.b(m.alphaToCoverage); w.vec(m.normals); w.vec(m.tangents);
    w.fa(m.lightmapPower, 3); w.f(m.metallic); w.f(m.roughness); w.f(m.speciblDiffScale); w.f(m.speciblSpecScale);
    w.fa(m.albedoFactor, 3); w.fa(m.curTint, 4); w.b(m.animatedTintAlpha);
    w.fa(m.animXform, 16); w.fa(m.uvXform, 6); w.vec(m.skinBindPos); w.vec(m.skinJoints); w.fa(m.skinBox, 6);
}

void readMesh(Reader& r, MeshData& m, const std::vector<std::shared_ptr<const TexSource>>& tex,
              const std::vector<std::shared_ptr<const std::vector<u16>>>& half, u32 version) {
    const bool v3 = version >= 3;        // v2 (.fenv saved before HDR maps stayed ASTC) still opens
    auto texRef = [&](u32 i) { return i < tex.size() ? tex[i] : nullptr; };
    auto halfRef = [&](u32 i) { return i < half.size() ? half[i] : nullptr; };
    m.name = r.str();
    if (version >= 7) m.path = r.str();
    m.components.resize(r.count(16));
    for (EnvComponent& c : m.components) {
        if (r.bad) return;
        c.cls = r.str(); c.shortCls = r.str(); c.version = r.pod<int>();
        c.fields.resize(r.count(8));
        for (auto& kv : c.fields) { if (r.bad) return; kv.first = r.str(); kv.second = r.str(); }
    }
    m.isSkybox = r.b();
    m.positions = r.vec<float>(); m.uvs = r.vec<float>(); m.colors = r.vec<u8>(); m.uvs3 = r.vec<float>(); m.uvs4 = r.vec<float>();
    m.indices = r.vec<u32>();
    m.nVerts = r.u(); m.nIdx = r.u();
    m.texRGBA = r.vec<u8>(); m.texW = r.u(); m.texH = r.u(); m.hasTexture = r.b(); m.texShareSrc = r.pod<int>();
    m.srcAstc = r.vec<u8>(); m.srcAstcBw = r.u(); m.srcAstcBh = r.u(); m.srcAstcMips = r.u();
    m.normalRGBA = r.vec<u8>(); m.normalW = r.u(); m.normalH = r.u(); m.hasNormal = r.b();
    m.ormRGBA = r.vec<u8>(); m.ormW = r.u(); m.ormH = r.u(); m.hasOrm = r.b();
    m.ormTexName = r.str();
    m.emissiveRGBA = r.vec<u8>(); m.emissiveW = r.u(); m.emissiveH = r.u(); m.hasEmissive = r.b();
    m.lmRGBA = r.vec<u8>(); m.lmW = r.u(); m.lmH = r.u(); m.hasLightmap = r.b();
    m.lmHalf = halfRef(r.u()); if (v3) m.lmSrc = texRef(r.u());
    if (version >= 9) m.lmLdrSrc = texRef(r.u());
    m.cubeRGBA = r.vec<u8>(); m.cubeSize = r.u(); if (v3) m.cubeSrc = texRef(r.u());
    m.texSrc = texRef(r.u()); m.normalSrc = texRef(r.u()); m.ormSrc = texRef(r.u()); m.emissiveSrc = texRef(r.u());
    m.namedTex.resize(r.count(8));
    for (auto& nt : m.namedTex) { if (r.bad) return; nt.first = r.u(); nt.second = texRef(r.u()); }
    m.namedHdr.resize(r.count(v3 ? 20 : 16));
    for (auto& nh : m.namedHdr) { if (r.bad) return; nh.first = r.u(); nh.second.half = halfRef(r.u()); nh.second.w = r.u(); nh.second.h = r.u(); if (v3) nh.second.src = texRef(r.u()); }
    m.reflCubeHalf = r.vec<u16>(); m.reflCubeSize = r.u(); m.reflCubeMips = r.u();
    if (version >= 10) m.reflCubeSrc = texRef(r.u());
    r.fa(m.lmUvScaleOffset, 4);
    m.vatRaw = r.vec<u8>(); m.vatW = r.u(); m.vatH = r.u(); m.hasVat = r.b();
    r.fa(m.tint, 4);
    m.atlasCellIndex = r.f(); m.vatTrackIndex = r.f(); m.vatRateFactor = r.f(); m.vatTimeOffset = r.f();
    m.matOverrides.resize(r.count(20));
    for (MatOverride& o : m.matOverrides) { if (r.bad) return; o.name = r.str(); r.fa(o.v, 4); }
    m.shaderIng = r.pod<u64>(); m.shaderPath = r.str(); m.meshPath = r.str(); m.tiled = r.b();
    m.matParamsBlob = r.vec<u8>(); m.constParams = r.vec<ConstParam>(); m.texSlots = r.vec<u32>();
    m.astcRaw = r.vec<u8>(); m.astcBw = r.u(); m.astcBh = r.u();
    m.transform = r.pod<Transform>();
    r.fa(m.worldMatrix, 16); m.hasWorldMatrix = r.b();
    m.boneIndices = r.vec<u8>(); m.boneWeights = r.vec<u8>(); m.hasBones = r.b(); m.bonePalette = r.vec<u32>();
    m.useBlend = r.b(); m.additive = r.b(); m.alphaTest = r.b(); m.alphaCutoff = r.f(); m.srcAlphaMode = r.pod<int>(); m.srcFactorA = r.f();
    m.overlayKind = r.pod<int>(); m.doubleSided = r.b(); m.dynamicVerts = r.b(); m.gltfMeshIndex = r.pod<int>();
    m.iblLit = r.b(); m.iblFullSpec = r.b(); m.bakeLightmapVtx = r.b();
    m.uvs2 = r.vec<float>(); m.alphaToCoverage = r.b(); m.normals = r.vec<float>(); m.tangents = r.vec<float>();
    r.fa(m.lightmapPower, 3); m.metallic = r.f(); m.roughness = r.f(); m.speciblDiffScale = r.f(); m.speciblSpecScale = r.f();
    r.fa(m.albedoFactor, 3); r.fa(m.curTint, 4); m.animatedTintAlpha = r.b();
    if (version >= 13) { r.fa(m.animXform, 16); r.fa(m.uvXform, 6); m.skinBindPos = r.vec<float>(); m.skinJoints = r.vec<float>(); r.fa(m.skinBox, 6); }
}

// The world layer in binary (world.json is the editable form): mesh colliders and walkable surfaces are
// megabytes of floats, which as text were the largest part of some blobs and slow to parse.
void writePoint(Writer& w, const WorldPoint& p) { w.str(p.name); w.fa(p.pos, 3); w.f(p.yaw); w.fa(p.exit, 3); }
WorldPoint readPoint(Reader& r) { WorldPoint p; p.name = r.str(); r.fa(p.pos, 3); p.yaw = r.f(); r.fa(p.exit, 3); return p; }

// Mesh colliders made from a drawn mesh (Meta's, whose physics mesh is the render mesh) are written as a reference
// to that mesh instead of a second copy of its vertices.
int sameMesh(const WorldCollider& c, const std::vector<MeshData>& meshes) {
    for (size_t m = 0; m < meshes.size(); ++m) {
        const MeshData& md = meshes[m];
        if (md.positions.size() == c.positions.size() && md.indices.size() == c.indices.size() && !c.positions.empty() &&
            std::memcmp(md.positions.data(), c.positions.data(), c.positions.size() * 4) == 0 &&
            std::memcmp(md.indices.data(), c.indices.data(), c.indices.size() * 4) == 0)
            return static_cast<int>(m);
    }
    return -1;
}

void writeWorld(Writer& w, const HomeWorld& wd, const std::vector<MeshData>& meshes, const std::vector<char>* thinned, const std::string& walkGrid) {
    w.str(wd.origin);
    for (const auto* list : {&wd.spawns, &wd.seats, &wd.hotspots}) {
        w.u(static_cast<u32>(list->size()));
        for (const WorldPoint& p : *list) writePoint(w, p);
    }
    w.u(static_cast<u32>(wd.colliders.size()));
    for (const WorldCollider& c : wd.colliders) {
        w.str(c.name); w.pod(c.kind); w.pod(c.role); w.fa(c.world, 16); w.fa(c.half, 3); w.f(c.radius); w.f(c.length);
        int ref = c.kind == WorldCollider::Mesh ? sameMesh(c, meshes) : -1;
        if (ref >= 0 && thinned && static_cast<size_t>(ref) < thinned->size() && (*thinned)[ref]) ref = -1;   // its mesh lost triangles: its own
        w.pod(ref);
        if (ref < 0) { w.vec(c.positions); w.vec(c.indices); }
        w.str(c.mesh);
    }
    w.vec(wd.walkPositions); w.vec(wd.walkIndices);
    w.u(static_cast<u32>(wd.wallSpots.size()));
    for (const WorldWallSpot& ws : wd.wallSpots) { w.str(ws.name); w.fa(ws.world, 16); w.f(ws.width); w.f(ws.height); }
    w.u(static_cast<u32>(wd.gravity.size()));
    for (const WorldGravity& g : wd.gravity) { w.str(g.name); w.pod(g.kind); w.fa(g.world, 16); w.fa(g.half, 3); w.f(g.radius); w.pod(g.priority); }
    w.f(wd.fallLimit);
    w.str(walkGrid);
}

void readWorld(Reader& r, HomeWorld& wd, u32 version, std::vector<std::pair<size_t, int>>& refs) {
    wd = HomeWorld{};
    wd.origin = r.str();
    for (auto* list : {&wd.spawns, &wd.seats, &wd.hotspots})
        for (u32 n = r.u(); n-- && !r.bad;) list->push_back(readPoint(r));
    wd.colliders.resize(r.count(93));
    for (WorldCollider& c : wd.colliders) {
        if (r.bad) break;
        c.name = r.str(); c.kind = r.pod<int>();
        if (version >= 4) c.role = r.pod<int>(); else c.role = r.b() ? WorldCollider::Trigger : WorldCollider::Solid;
        r.fa(c.world, 16); r.fa(c.half, 3); c.radius = r.f(); c.length = r.f();
        const int ref = version >= 5 ? r.pod<int>() : -1;
        if (ref >= 0) refs.emplace_back(&c - wd.colliders.data(), ref);   // filled from the mesh once meshes are read
        else { c.positions = r.vec<float>(); c.indices = r.vec<u32>(); }
        if (version >= 4) c.mesh = r.str();
    }
    wd.walkPositions = r.vec<float>(); wd.walkIndices = r.vec<u32>();
    wd.wallSpots.resize(r.count(76));
    for (WorldWallSpot& ws : wd.wallSpots) { if (r.bad) break; ws.name = r.str(); r.fa(ws.world, 16); ws.width = r.f(); ws.height = r.f(); }
    if (version >= 6) {
        wd.gravity.resize(r.count(92));
        for (WorldGravity& g : wd.gravity) { if (r.bad) break; g.name = r.str(); g.kind = r.pod<int>(); r.fa(g.world, 16); r.fa(g.half, 3); g.radius = r.f(); g.priority = r.pod<int>(); }
        wd.fallLimit = r.f();
    }
    if (version >= 11) {   // its mesh colliders were simplified by the bake
        wd.walkGrid = r.str();
        for (WorldCollider& c : wd.colliders) c.simplified = c.kind == WorldCollider::Mesh;
    }
    worldAdoptLegacyWalkable(wd);
}

constexpr u32 kMagic = 0x4E435346;   // "FSCN"

// The block footprints ASTC has (2D): the renderer divides by them and picks its Vulkan format from them.
bool astcFootprint(u32 bw, u32 bh) {
    static const u8 k[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (const auto& f : k) if (f[0] == bw && f[1] == bh) return true;
    return false;
}

// What the readers index by without checking, checked once here: anything a bake can't have written fails the load.
bool sane(HslScene& s) {
    for (MeshData& m : s.meshes) {
        m.nVerts = static_cast<u32>(std::min<size_t>(m.nVerts, m.positions.size() / 3));
        m.nIdx = static_cast<u32>(std::min<size_t>(m.nIdx, m.indices.size()));
        if (m.cubeSize && (m.cubeSrc ? m.cubeSrc->faces != 6 : m.cubeRGBA.size() / 24 / m.cubeSize < m.cubeSize)) return false;
        if ((!m.skinBindPos.empty() && (m.skinBindPos.size() < static_cast<size_t>(m.nVerts) * 3 || m.boneIndices.size() < static_cast<size_t>(m.nVerts) * 4 ||
                                        m.boneWeights.size() < static_cast<size_t>(m.nVerts) * 4)) ||
            m.skinJoints.size() % 16 || m.skinJoints.size() > 256 * 16)
            return false;
    }
    for (const HslAnimGroup& g : s.anims) {
        const size_t joints = g.skel.joints.size(), keys = static_cast<size_t>(std::max(0, g.clip.nFrames)) * std::max(0, g.clip.nJoints);
        if (g.clip.nFrames < 0 || g.clip.nJoints < 0) return false;
        // empty is fine (sampled as the bind pose); a partial array is damage
        if ((!g.skel.invBind.empty() && g.skel.invBind.size() < joints * 16) || (!g.clip.quats.empty() && g.clip.quats.size() / 4 < keys))
            return false;
    }
    s.meshSource.resize(s.meshes.size(), 0);   // per mesh, always (Studio indexes it by mesh)
    return true;
}

}  // namespace

bool bakeScene(const HslScene& scene, std::vector<u8>& out, std::string& error, const BakeOptions* options) {
    // A Clay / glTF home's animation goes in as data (anim_bake.h), and the meshes it writes as a fresh load has them
    // (the animation at t = 0): Frame Studio plays it, so they may be anywhere in it now. When that moved vertices, the
    // bake works from a copy of the scene with them back (the walkable area and the colliders read the vertices).
    std::vector<u8> anim;
    std::vector<std::pair<size_t, MeshData>> posed;
    if (scene.saveAnimation) {
        scene.saveAnimation(anim);
        if (!animbake::startPose(anim, scene.meshes, posed)) { error = "this home's animation couldn't be stored"; return false; }
    } else if (scene.animate) {
        error = "this home's animation can't be stored";
        return false;
    }
    std::unique_ptr<HslScene> atStart;
    for (const auto& [m, md] : posed)
        if (md.positions.size() != scene.meshes[m].positions.size() ||
            std::memcmp(md.positions.data(), scene.meshes[m].positions.data(), md.positions.size() * sizeof(float))) {
            atStart = std::make_unique<HslScene>(scene);
            for (auto& [i, pmd] : posed) atStart->meshes[i] = std::move(pmd);
            posed.clear();
            break;
        }
    const HslScene& s = atStart ? *atStart : scene;
    std::vector<const MeshData*> meshes(s.meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i) meshes[i] = &s.meshes[i];
    for (const auto& [m, md] : posed) meshes[m] = &md;
    const VisibilityResult* vis = options ? options->visibility : nullptr;
    if (vis && vis->changed.size() != s.meshes.size()) vis = nullptr;
    out.clear();
    // Meshes first into their own buffer: that fills the shared-texture tables, which go ahead of them.
    std::vector<u8> meshBytes;
    Writer mw{meshBytes};
    Table<TexSource> tex;
    Table<std::vector<u16>> half;
    for (size_t i = 0; i < s.meshes.size(); ++i) writeMesh(mw, *meshes[i], tex, half, vis && vis->changed[i] ? &vis->indices[i] : nullptr);

    Writer w{out};
    w.u(kMagic); w.u(kSceneBakeVersion);
    w.u(static_cast<u32>(s.apkPaths.size()));
    for (size_t i = 0; i < s.apkPaths.size(); ++i) {
        w.str(s.apkPaths[i]);
        const HslSourceInfo info = i < s.sources.size() ? s.sources[i] : HslSourceInfo{};
        w.str(info.package); w.str(info.version); w.str(info.format); w.str(info.role);
    }
    w.vec(s.meshSource);
    w.fa(s.ambientRGB, 3); w.b(s.hasEnvAmbient);
    {   // the world's fog: a global the HSTF parser sets while loading (renderer reads it per frame)
        std::lock_guard<std::mutex> l(g_sceneFogMutex);
        w.b(g_sceneFog.has); w.fa(g_sceneFog.color, 4); w.f(g_sceneFog.start); w.f(g_sceneFog.density); w.b(g_sceneFog.travel);
    }
    w.f(s.animDuration); w.vec(s.animatedMeshes);
    w.vec(s.ambientOgg); w.str(s.ambientSource); w.str(s.ambientName);
    {   // the mesh colliders as every load simplifies them (simplifyWorldMeshes): done here once, not at each load
        HomeWorld world = s.world;
        simplifyWorldMeshes(world);
        // The walkable area goes in baked: the optimizer's, else Frame Studio's (as edited there), else the one Frame
        // Home would bake at load (the same bake, with its defaults), so the headset never bakes one (0.3-0.5 s).
        std::string walkGrid = options && !options->walkGrid.empty() ? options->walkGrid : s.world.walkGrid;
        if (walkGrid.empty())
            if (const std::shared_ptr<const WalkGrid> g = bakeWalkGrid(s, world, WalkParams{})) walkGrid = encodeWalkGrid(*g);
        writeWorld(w, world, s.meshes, vis ? &vis->changed : nullptr, walkGrid);
    }
    // The shaders as the Frame's GPU runs them (shader_bake.h): each program's prepared forms, and per distinct set
    // of material values its meshes have, the fragment shader with them baked in.
    std::map<std::string, ShaderProgramSrc> programs = s.programs;
    const auto ts = std::chrono::steady_clock::now();
    shaderbake::bakePrograms(s.meshes, [&](const MeshData& md) -> std::string {
        const ShaderProgramSrc* src = findProgram(s, md.shaderPath);
        if (!src && md.isSkybox)                 // (as the renderer: the shell's skybox shader ships in renderer_module)
            for (const auto& kv : s.programs)
                if (shaderbake::contains(shaderbake::lc(kv.first), "skybox")) { src = &kv.second; break; }
        for (const auto& kv : s.programs) if (&kv.second == src) return kv.first;
        return std::string();
    }, s.ambientRGB, s.hasEnvAmbient, programs);
    {
        size_t specialized = 0;
        for (const auto& kv : programs) for (const auto& v : kv.second.variants) specialized += !v.frag.empty();
        std::fprintf(stderr, "[bake] shaders: %zu programs prepared, %zu specialized for their materials in %.0f ms\n", programs.size(), specialized,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count());
    }
    w.u(static_cast<u32>(programs.size()));
    for (const auto& kv : programs) {
        const ShaderProgramSrc& p = kv.second;
        w.str(kv.first); w.str(p.path); w.vec(p.vert); w.vec(p.frag);
        w.b(p.prepared); w.vec(p.vertInvariant); w.vec(p.fragAlpha);
        w.u(static_cast<u32>(p.variants.size()));
        for (const auto& v : p.variants) { w.pod(v.key); w.vec(v.frag); w.vec(v.fragAlpha); }
    }
    w.u(static_cast<u32>(s.anims.size()));
    for (const HslAnimGroup& g : s.anims) {
        w.u(static_cast<u32>(g.skel.joints.size()));
        for (const RendJoint& j : g.skel.joints) { w.fa(j.pos, 3); w.fa(j.quat, 4); w.f(j.scale); w.pod(j.parent); w.str(j.name); }
        w.vec(g.skel.bind); w.vec(g.skel.invBind);
        w.pod(g.clip.nJoints); w.pod(g.clip.nFrames); w.f(g.clip.fps); w.vec(g.clip.quats); w.vec(g.clip.trans);
        w.vec(g.clip.acl ? hzAclBytes(g.clip.acl) : std::vector<uint8_t>());
        w.pod(g.clip.aclJoints);
        w.vec(g.meshes);
    }
    w.vec(anim);
    w.u(static_cast<u32>(tex.items.size()));
    // Textures shipped without smaller levels get their chain here (tex_mips.h): sampled natively on the Frame at full
    // size instead of being shrunk and rebuilt at every load. FH_NO_BAKE_MIPS=1 keeps them as shipped.
    static const bool bakeMips = !std::getenv("FH_NO_BAKE_MIPS");
    const auto tm0 = std::chrono::steady_clock::now();
    size_t mipped = 0, cached = 0;
    // Each chain from the texture cache when an earlier open or save built it (tex_cache.h), else made here and kept
    // there: a save of the same home again (and every auto-save) makes none. The ones made run a texture per core.
    std::vector<std::shared_ptr<TexSource>> chains(tex.items.size());
    std::atomic<size_t> fromCache{0};
    if (bakeMips)
        par::forEach(tex.items.size(), [&](size_t i) {
            const TexSource& t = *tex.items[i];
            if (!texmips::wants(t)) return;
            if ((chains[i] = texcache::load(t))) { ++fromCache; return; }
            if ((chains[i] = texmips::withMips(t, 1))) texcache::store(*chains[i], texcache::key(t));
        });
    cached = fromCache;
    for (size_t i = 0; i < tex.items.size(); ++i) {
        const TexSource* t = tex.items[i];
        if (chains[i]) { t = chains[i].get(); ++mipped; }
        w.vec(t->astc); w.u(t->w); w.u(t->h); w.u(t->bw); w.u(t->bh); w.u(t->mips); w.b(t->hdr); w.u(t->faces);
    }
    if (mipped)
        std::fprintf(stderr, "[bake] gave %zu textures their smaller levels (%zu from the cache) in %.1f s\n", mipped, cached,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - tm0).count());
    w.u(static_cast<u32>(half.items.size()));
    for (const std::vector<u16>* h : half.items) w.vec(*h);
    w.u(static_cast<u32>(s.meshes.size()));
    out.insert(out.end(), meshBytes.begin(), meshBytes.end());
    return true;
}

bool optimizeForBake(const HslScene& scene, const std::vector<ObjectEdit>* edits, VisibilityResult& vis, BakeOptions& options) {
    vis = VisibilityResult();
    options = BakeOptions();
    if (std::getenv("FH_NO_VIS_CULL")) { vis.skipped = "turned off (FH_NO_VIS_CULL)"; return false; }
    if (scene.animate && !scene.saveAnimation) { vis.skipped = "the home's animation can't be stored (it isn't baked)"; return false; }
    // Everything a CPU animation writes stays whole (and hides nothing): its motion isn't in what's seen from where.
    VisibilityOptions vo;
    if (scene.saveAnimation) {
        std::vector<u8> anim;
        scene.saveAnimation(anim);
        vo.pinned = animbake::targets(anim);
    }
    const HomeWorld w = walkWorld(scene);
    if (!bakeVisibility(scene, w, w.walkGridCache.get(), edits, vis, vo)) {
        std::fprintf(stderr, "[optimize] nothing left out: %s\n", vis.skipped.c_str());
        return false;
    }
    options.visibility = &vis;
    options.walkGrid = encodeWalkGrid(*w.walkGridCache);
    if (std::getenv("FH_VIS_DRYRUN")) {   // diagnostics: everything as optimized (the walkable area stored) but no triangle left out
        vis.changed.assign(vis.changed.size(), 0);
        vis.trisAfter = vis.trisBefore;
    }
    std::fprintf(stderr, "[optimize] from %zu eye spots: %zu of %zu triangles left out (%.1f%%), %zu meshes emptied, %zu thinned, %.1f s\n",
                 vis.samples, vis.trisBefore - vis.trisAfter, vis.trisBefore, vis.trisBefore ? 100.0 * (vis.trisBefore - vis.trisAfter) / vis.trisBefore : 0.0,
                 vis.meshesEmptied, vis.meshesTouched, vis.ms / 1000);
    return true;
}

bool unbakeScene(const u8* data, size_t size, HslScene& s, std::string& error) {
    Reader r{data, data + size};
    if (r.u() != kMagic) { error = "not a baked scene"; return false; }
    const u32 version = r.u();
    if (version < 2 || version > kSceneBakeVersion) { error = "baked scene from a different Frame Studio version"; return false; }
    s = HslScene{};
    s.apkPaths.resize(r.count(20));
    for (size_t i = 0; i < s.apkPaths.size() && !r.bad; ++i) {
        s.apkPaths[i] = r.str();
        HslSourceInfo info;
        info.package = r.str(); info.version = r.str(); info.format = r.str(); info.role = r.str();
        s.sources.push_back(info);
    }
    s.meshSource = r.vec<int>();
    r.fa(s.ambientRGB, 3); s.hasEnvAmbient = r.b();
    SceneFog fog;
    fog.has = r.b(); r.fa(fog.color, 4); fog.start = r.f(); fog.density = r.f(); fog.travel = r.b();
    s.animDuration = r.f(); s.animatedMeshes = r.vec<int>();
    s.ambientOgg = r.vec<u8>(); s.ambientSource = r.str();
    if (version >= 8) s.ambientName = r.str();
    std::vector<std::pair<size_t, int>> meshRefs;
    if (version >= 3) readWorld(r, s.world, version, meshRefs); else worldFromJson(r.str(), s.world);
    for (u32 n = r.u(); n-- && !r.bad;) {
        const std::string key = r.str();
        ShaderProgramSrc p;
        p.path = r.str(); p.vert = r.vec<u32>(); p.frag = r.vec<u32>();
        if (version >= 12) {
            p.prepared = r.b(); p.vertInvariant = r.vec<u32>(); p.fragAlpha = r.vec<u32>();
            p.variants.resize(r.count(20));
            for (auto& v : p.variants) { if (r.bad) break; v.key = r.pod<uint64_t>(); v.frag = r.vec<u32>(); v.fragAlpha = r.vec<u32>(); }
            if (p.prepared && p.vertInvariant.empty()) p.prepared = false;   // (a damaged file: made at load instead)
        }
        s.programs[key] = std::move(p);
    }
    s.anims.resize(r.count(44));
    for (HslAnimGroup& g : s.anims) {
        if (r.bad) break;
        g.skel.joints.resize(r.count(40));
        for (RendJoint& j : g.skel.joints) { if (r.bad) break; r.fa(j.pos, 3); r.fa(j.quat, 4); j.scale = r.f(); j.parent = r.pod<int>(); j.name = r.str(); }
        g.skel.bind = r.vec<float>(); g.skel.invBind = r.vec<float>();
        g.clip.nJoints = r.pod<int>(); g.clip.nFrames = r.pod<int>(); g.clip.fps = r.f(); g.clip.quats = r.vec<float>(); g.clip.trans = r.vec<float>();
        const std::vector<u8> acl = r.vec<u8>();
        g.clip.acl = acl.empty() ? nullptr : hzAclCreate(acl.data(), acl.size());   // owned for the scene's life, as when parsed
        g.clip.aclJoints = r.pod<int>();
        g.meshes = r.vec<int>();
    }
    const std::vector<u8> anim = version >= 13 ? r.vec<u8>() : std::vector<u8>();
    std::vector<std::shared_ptr<const TexSource>> tex(r.count(version >= 3 ? 29 : 25));
    for (auto& t : tex) {
        if (r.bad) break;
        auto ts = std::make_shared<TexSource>();
        ts->astc = r.vec<u8>(); ts->w = r.u(); ts->h = r.u(); ts->bw = r.u(); ts->bh = r.u(); ts->mips = r.u(); ts->hdr = r.b(); if (version >= 3) ts->faces = r.u();
        if (!astcFootprint(ts->bw, ts->bh) || !ts->w || !ts->h || ts->w > 16384 || ts->h > 16384 || (ts->faces != 1 && ts->faces != 6) ||
            (ts->faces == 6 && ts->w != ts->h)) { r.bad = true; break; }
        u32 chain = 1;                           // a mip count past the image's own chain: the levels it can have
        while ((std::max(ts->w, ts->h) >> chain) != 0) ++chain;
        ts->mips = std::min(std::max(ts->mips, 1u), chain);
        t = ts;
    }
    std::vector<std::shared_ptr<const std::vector<u16>>> half(r.count(4));
    for (auto& h : half) { if (r.bad) break; h = std::make_shared<std::vector<u16>>(r.vec<u16>()); }
    s.meshes.resize(r.count(64));
    for (MeshData& m : s.meshes) { if (r.bad) break; readMesh(r, m, tex, half, version); }
    for (const auto& [ci, mi] : meshRefs) {
        if (ci >= s.world.colliders.size() || mi >= static_cast<int>(s.meshes.size())) { r.bad = true; break; }
        s.world.colliders[ci].positions = s.meshes[mi].positions;
        s.world.colliders[ci].indices = s.meshes[mi].indices;
    }
    if (!r.bad && !anim.empty() && !animbake::load(anim, s.meshes.size(), s)) r.bad = true;   // (it plays the meshes as read)
    if (r.bad || !sane(s)) { error = "baked scene is truncated or damaged"; s = HslScene{}; return false; }
    std::lock_guard<std::mutex> l(g_sceneFogMutex);
    g_sceneFog = fog;
    g_deferTextures = true;
    return true;
}
