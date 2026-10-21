#pragma once
#include "core/types.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

// v210 RENDSHAD shader-name rehydration.
//
// v210+ homes ship SMOL-V shader stages with every SPIR-V debug name stripped (no OpName/OpMemberName).
// The renderer binds vertex inputs, descriptor resources and uniform members BY NAME (introspectSet2,
// the vertex-input role table), so a stripped module binds nothing and its meshes never rasterize
// (Theatre_210: black viewport even in wireframe). libshell itself binds from the RENDSHAD reflection
// tables, not from SPIR-V names — and those tables survive. The names are recoverable from:
//   * root f5  : uniform/push-constant blocks, each with (name, members[(name, packed: offset in low 16)])
//   * root f9  : texture slots, in ascending set-2 image-binding order
//   * root f11 : vertex streams; [0].f2 = attributes {sem | fmt<<8 | idx<<16} in input-location order
//   * the engine's FIXED set-0 (GlobalUniforms) / set-1 (lighting) ABI — byte-identical member offsets
//     between named v206 shaders (haven2025 isotropic) and stripped v210 ones (theater_lightmap).
// rehydrateSpirvNames() rebuilds OpName/OpMemberName from those and inserts them before the module's
// first annotation, so a v210 module introspects exactly like a v206 one. Named modules are untouched.

struct RendShadReflection {
    struct Block { std::string name; std::vector<std::pair<u32, std::string>> members; };  // (byte offset, name)
    std::vector<Block> blocks;             // root f5
    std::vector<std::string> textures;     // root f9, set-2 image binding order
    // Set-2 image bindings of ALL the program's stages, ascending: textures[k] names imageBindings[k]. A stage sees
    // only its own images (a VAT vertex stage reads just binding 7), so ranking within one stage named it textures[0].
    std::vector<u32> imageBindings;
    std::vector<u32> streamAttrs;          // root f11[0].f2, input-location order
};

inline bool parseRendShadReflection(const std::vector<u8>& data, RendShadReflection& R) {
    const u8* d = data.data(); const size_t N = data.size();
    if (N < 8) return false;
    auto rd16 = [&](size_t o) -> u16 { u16 v = 0; if (o + 2 <= N) memcpy(&v, d + o, 2); return v; };
    auto rd32 = [&](size_t o) -> u32 { u32 v = 0; if (o + 4 <= N) memcpy(&v, d + o, 4); return v; };
    auto field = [&](size_t tbl, u32 fi) -> size_t {
        if (!tbl || tbl + 4 > N) return 0;
        i32 so = (i32)rd32(tbl); long long vt = (long long)tbl - so;
        if (vt < 0 || (size_t)vt + 4 > N) return 0;
        u16 vs = rd16((size_t)vt); size_t slot = (size_t)vt + 4 + fi * 2;
        if (slot + 2 > (size_t)vt + vs || slot + 2 > N) return 0;
        u16 fo = rd16(slot); return fo ? tbl + fo : 0;
    };
    auto ref = [&](size_t fp) -> size_t {
        if (!fp || fp + 4 > N) return 0;
        size_t t = fp + rd32(fp); return t < N ? t : 0;
    };
    auto str = [&](size_t fp) -> std::string {
        size_t s = ref(fp); if (!s || s + 4 > N) return {};
        u32 n = rd32(s); if (!n || n > 512 || s + 4 + n > N) return {};
        return std::string(reinterpret_cast<const char*>(d + s + 4), n);
    };
    auto vec = [&](size_t fp, u32& cnt) -> size_t {
        cnt = 0; size_t v = ref(fp); if (!v || v + 4 > N) return 0;
        u32 c = rd32(v); if (c > 4096 || v + 4 + (size_t)c * 4 > N) return 0;
        cnt = c; return v + 4;
    };
    auto elem = [&](size_t base, u32 i) -> size_t { return ref(base + (size_t)i * 4); };

    size_t root = rd32(0); if (root + 4 > N) return false;

    u32 nb = 0; size_t vb = vec(field(root, 5), nb);
    for (u32 i = 0; i < nb; ++i) {
        size_t t = elem(vb, i); if (!t) continue;
        RendShadReflection::Block b; b.name = str(field(t, 0));
        u32 nm = 0; size_t vm = vec(field(t, 2), nm);
        for (u32 j = 0; j < nm; ++j) {
            size_t mt = elem(vm, j); if (!mt) continue;
            std::string mn = str(field(mt, 0)); if (mn.empty()) continue;
            size_t op = field(mt, 2);                       // absent = offset 0 (FlatBuffer default)
            b.members.push_back({ op ? (rd32(op) & 0xFFFFu) : 0u, mn });
        }
        if (!b.name.empty()) R.blocks.push_back(std::move(b));
    }
    u32 nt = 0; size_t vt = vec(field(root, 9), nt);
    for (u32 i = 0; i < nt; ++i) { size_t t = elem(vt, i); R.textures.push_back(t ? str(field(t, 0)) : std::string()); }
    u32 ns = 0; size_t vs = vec(field(root, 11), ns);
    if (ns) {
        size_t t = elem(vs, 0); u32 na = 0; size_t va = vec(field(t, 2), na);
        for (u32 k = 0; k < na; ++k) R.streamAttrs.push_back(rd32(va + (size_t)k * 4));
    }
    return !R.blocks.empty() || !R.textures.empty() || !R.streamAttrs.empty();
}

// Adds the set-2 image (or combined image-sampler) bindings a stage declares to `out` (kept sorted, unique).
inline void spirvSet2ImageBindings(const std::vector<u32>& code, std::vector<u32>& out) {
    if (code.size() < 6 || code[0] != 0x07230203u) return;
    std::map<u32,u32> dset, bind, ptrPointee, arrayElem, typeOp;
    std::vector<std::pair<u32,u32>> vars;                  // (id, ptrType), UniformConstant only
    for (size_t i = 5; i < code.size(); ) {
        u32 w = code[i], op = w & 0xFFFF, wc = w >> 16;
        if (wc == 0 || i + wc > code.size()) return;
        const u32* a = &code[i + 1];
        switch (op) {
            case 71: if (wc >= 4) { if (a[1] == 33) bind[a[0]] = a[2]; else if (a[1] == 34) dset[a[0]] = a[2]; } break;
            case 59: if (wc >= 4 && a[2] == 0) vars.push_back({a[1], a[0]}); break;
            case 32: if (wc >= 4) ptrPointee[a[0]] = a[2]; break;
            case 28: case 29: if (wc >= 3) arrayElem[a[0]] = a[1]; break;
            case 25: case 27: typeOp[a[0]] = op; break;
            default: break;
        }
        i += wc;
    }
    for (auto& v : vars) {
        u32 t = ptrPointee.count(v.second) ? ptrPointee[v.second] : 0;
        for (int g = 0; g < 8 && arrayElem.count(t); ++g) t = arrayElem[t];
        if (!typeOp.count(t) || !dset.count(v.first) || dset[v.first] != 2 || !bind.count(v.first)) continue;
        auto it = std::lower_bound(out.begin(), out.end(), bind[v.first]);
        if (it == out.end() || *it != bind[v.first]) out.insert(it, bind[v.first]);
    }
}

inline bool spirvHasDebugNames(const std::vector<u32>& c) {
    for (size_t i = 5; i < c.size(); ) {
        u32 op = c[i] & 0xFFFF, wc = c[i] >> 16;
        if (wc == 0) break;
        if (op == 5 /*OpName*/ || op == 6 /*OpMemberName*/) return true;
        i += wc;
    }
    return false;
}

inline void rehydrateSpirvNames(std::vector<u32>& code, const RendShadReflection& R) {
    if (code.size() < 6 || code[0] != 0x07230203u || spirvHasDebugNames(code)) return;
    using Members = std::vector<std::pair<u32, std::string>>;
    // Fixed engine ABI (set 0 / set 1), harvested from the named v206 isotropic shader.
    static const Members kGlobalUniforms = {
        {0,"clipFromWorld"},{128,"skyBoxClipFromWorld"},{256,"viewFromWorld"},{384,"worldCameraPos"},
        {416,"lightingParams"},{432,"lightingParams2"},{448,"defaultLightprobes"},{592,"whitePoint"},{596,"time"},
        {600,"debugVisMode"},{604,"reverseZ"},{608,"worldTime"},{612,"configUniform0"},{616,"configUniform1"},
        {620,"configUniform2"},{624,"auxUniform0"},{640,"auxUniform1"},{656,"viewportOffset"},{664,"viewportSize"},
        {672,"fogColor"},{688,"fogStart"},{692,"fogDensity"},{696,"fogLinearScale"},{700,"fogLinearOffset"},
        {704,"skyboxFogParameters"},{720,"reserved0"} };
    static const Members kLightUniforms = {
        {0,"shadowmapMVP"},{64,"distantLightRadiantIntensity"},{80,"distantLightDir"},{96,"numPointLights"},
        {100,"numSpotLights"},{104,"reserved0"},{108,"reserved1"},{112,"iblReflectionTint"},{128,"iblDiffuseTint"},
        {144,"externalAverageLuminance"},{160,"auxLightUniformReserved1"},{168,"iblReflectionMaxAdaptationFactor"},
        {172,"iblReflectionLuminanceInverse"},{176,"auxLightUniformReserved2"},{192,"shadowFlags"},
        {196,"shadowEdgeFadeDistance"},{200,"shadowStrengthDiffuse"},{204,"shadowStrengthSpecular"},
        {208,"shadowDiffuseExtraRolloffMin"},{212,"shadowReceiverOrientationFactor"},{216,"shadowPcfTapCount"},
        {220,"shadowPcfRadius"},{224,"shadowmapDimensions"},{232,"shadowmapDimensionsInverse"},{240,"numDistantLights"},
        {244,"multishadowsReserved1"},{248,"multishadowsReserved2"},{252,"multishadowsReserved3"},
        {256,"distantLightIntensities"},{320,"distantLightDirections"},{384,"shadowmapAtlasRects"},
        {448,"worldToShadowmapTransforms"},{704,"worldAbsoluteOrigin"},{716,"reserved2"},{720,"lightGridDimensions"},
        {736,"lightGridRangeParams"},{752,"lightGridClipFromWorld"},{816,"lightGridViewFromWorld"},
        {880,"lightGridFrustumRatios"},{896,"cascadeOrigins"},{960,"cascadeAnchors"},{1024,"cascadeCount"},
        {1028,"cascadeRatio"},{1032,"cascadeMaxRange"},{1036,"cascadeRatioLog2Reciprocal"},
        {1040,"cascadeCountReciprocal"},{1044,"cascadeBlendWidth"},{1048,"cascadeReserved3"},{1052,"cascadeReserved4"} };
    struct Fixed { const char* var; const char* type; const Members* members; const char* member0; };
    static const std::map<std::pair<u32,u32>, Fixed> kFixed = {
        {{0,0},{"globalUniforms","GlobalUniforms",&kGlobalUniforms,nullptr}},
        {{0,1},{"linearWrapSampler",nullptr,nullptr,nullptr}},
        {{1,0},{"lightUniforms","LightUniforms",&kLightUniforms,nullptr}},
        {{1,1},{"sbLightItems","sbLightItemsTag",nullptr,"elements"}},
        {{1,2},{"globalIblDiffuseCubeMap",nullptr,nullptr,nullptr}},
        {{1,3},{"globalIblReflectionCubeMap",nullptr,nullptr,nullptr}},
        {{1,4},{"globalShadowMapSampler",nullptr,nullptr,nullptr}},
        {{1,5},{"globalShadowMapTexture",nullptr,nullptr,nullptr}},
        {{1,6},{"lightGridCells","LightGridCellsTag",nullptr,"data"}},
    };

    // ── Collect ids, decorations and types ────────────────────────────────────────────────────────
    std::map<u32,u32> loc, dset, bind, ptrPointee, arrayElem, typeOp, memberCount;
    std::map<u32,bool> builtin;
    std::map<u32,std::vector<u32>> structMembers;          // struct id -> member type ids
    std::map<std::pair<u32,u32>,u32> moff;
    std::vector<std::pair<u32,std::pair<u32,u32>>> vars;   // (id, (ptrType, storageClass)) in module order
    u32 execModel = 0xFFFFFFFFu; size_t insertAt = 0;
    for (size_t i = 5; i < code.size(); ) {
        u32 w = code[i], op = w & 0xFFFF, wc = w >> 16;
        if (wc == 0 || i + wc > code.size()) return;            // malformed: leave the module untouched
        const u32* a = &code[i + 1];
        if (!insertAt && ((op >= 71 && op <= 75) || op == 332 || (op >= 19 && op <= 39))) insertAt = i;  // first annotation/type
        switch (op) {
            case 15: if (execModel == 0xFFFFFFFFu && wc >= 2) execModel = a[0]; break;
            case 71:
                if (wc >= 3 && a[1] == 11) builtin[a[0]] = true;
                if (wc >= 4) { if (a[1] == 30) loc[a[0]] = a[2]; else if (a[1] == 33) bind[a[0]] = a[2]; else if (a[1] == 34) dset[a[0]] = a[2]; }
                break;
            case 72: if (wc >= 5 && a[2] == 35) moff[{a[0], a[1]}] = a[3]; break;
            case 59: if (wc >= 4) vars.push_back({a[1], {a[0], a[2]}}); break;
            case 32: if (wc >= 4) ptrPointee[a[0]] = a[2]; break;
            case 28: case 29: if (wc >= 3) { arrayElem[a[0]] = a[1]; typeOp[a[0]] = op; } break;
            case 30: typeOp[a[0]] = op; memberCount[a[0]] = wc - 2; structMembers[a[0]].assign(a + 1, a + (wc - 1)); break;
            case 25: case 26: case 27: typeOp[a[0]] = op; break;
            default: break;
        }
        i += wc;
    }
    if (!insertAt) return;

    // ── Build the debug-name instructions ──────────────────────────────────────────────────────────
    std::vector<u32> names;
    auto appendString = [&](const std::string& s) {
        size_t nw = s.size() / 4 + 1;                            // includes the NUL terminator
        size_t at = names.size(); names.resize(at + nw, 0u);
        memcpy(names.data() + at, s.data(), s.size());
    };
    auto emitName = [&](u32 id, const std::string& s) {
        if (s.empty()) return;
        size_t h = names.size(); names.push_back(0); names.push_back(id); appendString(s);
        names[h] = ((u32)(names.size() - h) << 16) | 5u;
    };
    auto emitMember = [&](u32 type, u32 m, const std::string& s) {
        if (s.empty()) return;
        size_t h = names.size(); names.push_back(0); names.push_back(type); names.push_back(m); appendString(s);
        names[h] = ((u32)(names.size() - h) << 16) | 6u;
    };
    auto baseType = [&](u32 t) { for (int g = 0; g < 8 && arrayElem.count(t); ++g) t = arrayElem[t]; return t; };
    auto structOffsets = [&](u32 st) {
        std::vector<u32> o; auto it = memberCount.find(st); if (it == memberCount.end()) return o;
        for (u32 m = 0; m < it->second; ++m) { auto f = moff.find({st, m}); o.push_back(f == moff.end() ? 0xFFFFFFFFu : f->second); }
        return o;
    };
    auto nameMembers = [&](u32 st, const Members& members) {
        std::vector<u32> o = structOffsets(st);
        for (u32 m = 0; m < o.size(); ++m)
            for (auto& p : members) if (p.first == o[m]) { emitMember(st, m, p.second); break; }
    };
    auto isPushBlock = [](const std::string& n) { return n.find("ushConstants") != std::string::npos; };

    // Vertex inputs: location k <-> vertex-stream attribute k.
    auto vertexInputName = [](u32 attr) -> std::string {
        u32 sem = attr & 0xFF, idx = (attr >> 16) & 0xFF;
        switch (sem) {
            case 0: return "inPos";
            case 1: return "inNorm";
            case 2: return "inTangent";
            case 4: return idx ? "inColor" + std::to_string(idx) : "inColor";
            case 5: return idx == 0 ? "inUv" : idx == 1 ? "inLightmapUv" : "inUv" + std::to_string(idx);
            case 6: case 8: return "inBoneIndices";
            case 7: return "inBoneWeights";
            default: return "inAttr" + std::to_string(sem);
        }
    };

    std::vector<std::pair<u32,u32>> set2Images, set2Samplers;   // (binding, var id)
    for (auto& v : vars) {
        u32 id = v.first, ptrType = v.second.first, sc = v.second.second;
        u32 pointee = ptrPointee.count(ptrType) ? ptrPointee[ptrType] : 0;
        u32 bt = baseType(pointee);
        u32 top = typeOp.count(bt) ? typeOp[bt] : 0;
        if (sc == 1 /*Input*/ && execModel == 0 && loc.count(id) && !builtin.count(id)) {
            u32 l = loc[id];
            if (l < R.streamAttrs.size()) emitName(id, vertexInputName(R.streamAttrs[l]));
            continue;
        }
        if (sc == 9 /*PushConstant*/ && top == 30) {
            emitName(bt, "PushConstants");
            if (memberCount[bt] >= 1) {
                emitMember(bt, 0, "pushConstants");
                // The inner block { worldFromModel, color } carries the per-draw model matrix: name its members
                // from the reflection's "pushConstants" block (renderers that bind by name need worldFromModel).
                u32 inner = structMembers[bt].empty() ? 0 : structMembers[bt][0];
                if (typeOp.count(inner) && typeOp[inner] == 30)
                    for (auto& blk : R.blocks)
                        if (blk.name == "pushConstants") { emitName(inner, "PushConstantsData"); nameMembers(inner, blk.members); break; }
            }
            continue;
        }
        if ((sc != 0 && sc != 2 && sc != 12) || !dset.count(id) || !bind.count(id)) continue;
        u32 s = dset[id], b = bind[id];
        auto fx = kFixed.find({s, b});
        if (s < 2 && fx != kFixed.end()) {
            emitName(id, fx->second.var);
            if (top == 30 && fx->second.type) {
                emitName(bt, fx->second.type);
                if (fx->second.members) nameMembers(bt, *fx->second.members);
                else if (fx->second.member0 && memberCount[bt] >= 1) emitMember(bt, 0, fx->second.member0);
            }
            continue;
        }
        if (s != 2) continue;
        if (top == 30) {
            // Material buffer: the reflection block whose member offsets match this struct's exactly.
            std::vector<u32> o = structOffsets(bt);
            const RendShadReflection::Block* best = nullptr;
            for (auto& blk : R.blocks) {
                if (isPushBlock(blk.name) || blk.members.size() != o.size()) continue;
                bool all = true;
                for (u32 off : o) { bool hit = false; for (auto& p : blk.members) if (p.first == off) { hit = true; break; } if (!hit) { all = false; break; } }
                if (all) { best = &blk; break; }
            }
            static const char* kSet2Default[] = {"matParams", "lightprobesParams", "lightBakerParams"};
            std::string nm = best ? best->name : (b < 3 ? kSet2Default[b] : std::string());
            emitName(id, nm); emitName(bt, nm.empty() ? nm : nm + "Tag");
            if (best) nameMembers(bt, best->members);
        } else if (top == 25 || top == 27) set2Images.push_back({b, id});
        else if (top == 26) set2Samplers.push_back({b, id});
    }
    std::sort(set2Images.begin(), set2Images.end());
    if (!R.imageBindings.empty()) {
        for (auto& im : set2Images) {
            size_t k = std::lower_bound(R.imageBindings.begin(), R.imageBindings.end(), im.first) - R.imageBindings.begin();
            if (k < R.imageBindings.size() && R.imageBindings[k] == im.first && k < R.textures.size()) emitName(im.second, R.textures[k]);
        }
    } else {
        for (size_t k = 0; k < set2Images.size() && k < R.textures.size(); ++k) emitName(set2Images[k].second, R.textures[k]);
    }
    std::sort(set2Samplers.begin(), set2Samplers.end());
    static const char* kSamplers[] = {"linearAniso4xSampler", "linearAniso2xSampler"};
    for (size_t k = 0; k < set2Samplers.size(); ++k) emitName(set2Samplers[k].second, k < 2 ? kSamplers[k] : "linearAniso4xSampler");

    if (!names.empty()) code.insert(code.begin() + (std::ptrdiff_t)insertAt, names.begin(), names.end());
}
