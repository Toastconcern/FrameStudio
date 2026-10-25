#pragma once
// The shaders as the Frame's GPU runs them, made once: Frame Studio (scene_bake) stores the results in the .fenv, and
// the renderer makes the same here for a home opened straight from its APK. The renderer checks a baked result
// against what it would make (the key of the uniform values it bakes in) and makes its own when they differ, so a
// .fenv baked by an older build never draws wrong.
//   - invariantPosition: the vertex shader with its position Invariant (a cut-out's depth pass and its shading pass
//     compute every position bit for bit alike)
//   - alphaOnly: the fragment shader writing only its alpha (the cut-out depth pass)
//   - specialize: per material, the uniform values that never change baked in as constants (spv_specialize.h)
//   - optimized: each of those through spv_opt.h when it's switched on, so a .fenv's home loads without making them;
//     the shipped vertex and fragment shaders stay as they are (the renderer lays a program out from them, and a
//     variant the bake didn't make is specialized from them)
// The set-2 (material) uniform buffers are filled here too, by the same code for both, so the bytes a baked shader
// assumes are the bytes the renderer uploads.
#include "core/types.h"
#include "hsl_scene.h"
#include "spv_reflect.h"
#include "spv_specialize.h"
#include "spv_opt.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace shaderbake {

inline std::string lc(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// The vertex shader with its position marked Invariant. (An OpMemberDecorate / OpDecorate ... Invariant after the
// BuiltIn Position one.)
inline std::vector<u32> invariantPosition(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return code;
    std::vector<u32> out(code.begin(), code.begin() + 5);
    out.reserve(code.size() + 4);
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return code;
        out.insert(out.end(), code.begin() + i, code.begin() + i + words);
        if (op == 72 && words == 5 && code[i + 3] == 11 && code[i + 4] == 0)        // OpMemberDecorate s m BuiltIn Position
            out.insert(out.end(), {(4u << 16) | 72u, code[i + 1], code[i + 2], 18u});
        else if (op == 71 && words == 4 && code[i + 2] == 11 && code[i + 3] == 0)   // OpDecorate v BuiltIn Position
            out.insert(out.end(), {(3u << 16) | 71u, code[i + 1], 18u});
        i += words;
    }
    return out;
}

// The fragment shader made to write only its colour's alpha: every whole-vec4 store to the Location 0 output becomes
// (a,a,a,a) of the same value. Discards and everything the alpha depends on stay as they are, so alpha-to-coverage
// picks the same samples; the lighting the colour needed no longer reaches an output and the driver drops it. Empty
// when the output is written any other way (a component at a time).
inline std::vector<u32> alphaOnly(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return {};
    u32 outVar = 0, bound = code[3];
    std::map<u32, u32> vec4Of;                   // OpTypeVector id (4 floats) -> its float type id
    std::map<u32, u32> ptrPointee;               // Output pointer type -> pointee
    std::map<u32, u32> varType;                  // variable -> its pointer type
    std::set<u32> loc0;
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return {};
        if (op == 71 && words == 4 && code[i + 2] == 30 && code[i + 3] == 0) loc0.insert(code[i + 1]);   // Location 0
        if (op == 23 && words == 4 && code[i + 3] == 4) vec4Of[code[i + 1]] = code[i + 2];                // OpTypeVector
        if (op == 32 && words == 4 && code[i + 2] == 3) ptrPointee[code[i + 1]] = code[i + 3];           // OpTypePointer Output
        if (op == 59 && words >= 4 && code[i + 3] == 3 && loc0.count(code[i + 2])) { outVar = code[i + 2]; varType[outVar] = code[i + 1]; }
        i += words;
    }
    if (!outVar || !ptrPointee.count(varType[outVar]) || !vec4Of.count(ptrPointee[varType[outVar]])) return {};
    const u32 vecT = ptrPointee[varType[outVar]], floatT = vec4Of[vecT];
    std::vector<u32> out(code.begin(), code.begin() + 5);
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if ((op == 65 || op == 66) && words >= 4 && code[i + 3] == outVar) return {};       // OpAccessChain into it
        if (op == 62 && words >= 3 && code[i + 1] == outVar) {                               // OpStore out, v
            const u32 a = bound++, v = bound++;
            out.insert(out.end(), {(5u << 16) | 81u, floatT, a, code[i + 2], 3u});           // a = OpCompositeExtract v[3]
            out.insert(out.end(), {(7u << 16) | 80u, vecT, v, a, a, a, a});                  // (a,a,a,a)
            out.insert(out.end(), {(3u << 16) | 62u, outVar, v});
            i += words;
            continue;
        }
        out.insert(out.end(), code.begin() + i, code.begin() + i + words);
        i += words;
    }
    out[3] = bound;
    return out;
}

// A program's descriptor sets as the renderer lays them out: the union of both stages, the larger declaration of a
// block winning, sorted by binding.
inline void mergedSets(const spv::Module& vr, const spv::Module& fr, std::vector<spv::Resource> sets[3]) {
    for (const spv::Module* m : {&vr, &fr}) {
        for (const auto& r : m->resources) {
            if (r.set > 2) continue;
            auto& v = sets[r.set];
            auto f = std::find_if(v.begin(), v.end(), [&](const spv::Resource& x) { return x.binding == r.binding; });
            if (f == v.end()) v.push_back(r);
            else if (f->size < r.size) { f->size = r.size; f->members = r.members; }
        }
    }
    for (int s = 0; s < 3; ++s)
        std::sort(sets[s].begin(), sets[s].end(), [](const spv::Resource& a, const spv::Resource& b) { return a.binding < b.binding; });
}

// Whether the vertex stage reads bone indices (its skinning palette may be an unnamed block).
inline bool boned(const spv::Module& vr) {
    for (const auto& in : vr.inputs) {
        const std::string n = lc(in.name);
        if (contains(n, "boneind") || contains(n, "joint") || contains(n, "indices")) return true;
    }
    return false;
}

// A per-material uniform block, as HSR Studio supplies it: zero, then the material's cooked constants bound by
// MurmurHash3("<block>.<member>") (matParams, lightprobesParams.L00 and lightBakerParams are all authored this way),
// then the entity's per-instance overrides. Returns which members the material authored.
inline std::vector<bool> fillBlock(const spv::Resource& r, const MeshData& md, float* f) {
    const size_t cap = std::max<size_t>(r.size, 16);
    u8* bytes = reinterpret_cast<u8*>(f);
    std::memset(bytes, 0, cap);
    std::vector<bool> authored(r.members.size(), false);
    const std::string block = r.name.empty() ? std::string("matParams") : r.name;
    for (size_t mi = 0; mi < r.members.size(); ++mi) {
        const auto& m = r.members[mi];
        const std::string full = block + "." + m.name;
        const u32 want = murmur3_x86_32(full.data(), full.size(), 0);
        for (const auto& cp : md.constParams) {
            if (cp.nameHash != want) continue;
            if (static_cast<size_t>(cp.blobOffset) + cp.byteSize > md.matParamsBlob.size() || m.offset + cp.byteSize > cap) break;
            std::memcpy(bytes + m.offset, md.matParamsBlob.data() + cp.blobOffset, cp.byteSize);
            authored[mi] = true;
            break;
        }
    }
    const std::string prefix = block + ".";
    for (const auto& ov : md.matOverrides) {
        if (ov.name.rfind(prefix, 0) != 0) continue;
        const std::string member = ov.name.substr(prefix.size());
        for (size_t mi = 0; mi < r.members.size(); ++mi) {
            const auto& m = r.members[mi];
            if (m.name != member || m.offset >= cap) continue;
            const size_t n = std::min<size_t>(std::min<u32>(m.size ? m.size : 4, 16), cap - m.offset);
            if (m.integer) {                     // overrides arrive as floats: store them as the integers the shader reads
                for (size_t k = 0; k * 4 < n; ++k) { const int32_t iv = static_cast<int32_t>(ov.v[k]); std::memcpy(bytes + m.offset + k * 4, &iv, 4); }
            } else {
                std::memcpy(bytes + m.offset, ov.v, n);
            }
            authored[mi] = true;
            break;
        }
    }
    // HSR's one non-zero default: a dielectric's Fresnel F0 at n = 1.5.
    for (size_t mi = 0; mi < r.members.size(); ++mi)
        if (!authored[mi] && r.members[mi].name == "incidentReflectivityF0" && r.members[mi].offset + 4 <= cap) {
            const float f0 = 0.04f;
            std::memcpy(bytes + r.members[mi].offset, &f0, 4);
        }
    return authored;
}

// matParams, as HSR Studio fills it: zero (a value the material does not author is zero, never a guess), then the
// material's cooked constants bound by MurmurHash3("matParams."+member), then the entity's per-instance overrides.
inline void fillMatParams(const spv::Resource& r, const MeshData& md, const std::string& programPath, float* f) {
    fillBlock(r, md, f);
    // Desert Terrace 210's palms (palmtreefoliage) light from the standard lightmap, not their custom HDR one: with it
    // (useCustomLightmap 1, as authored) every trunk lit a flat bright yellow, nothing like the Clay twin's dark,
    // side-lit trunks; with the standard lightmap trunks and fronds match it (checked side by side, 2026-10-20).
    if (programPath.find("palmtreefoliage") != std::string::npos)
        for (const auto& m : r.members)
            if (m.name == "useCustomLightmap" && m.offset + 4 <= std::max<size_t>(r.size, 16)) { const int32_t zero = 0; std::memcpy(reinterpret_cast<u8*>(f) + m.offset, &zero, 4); }
    // FH_MATSET=member=value,...: diagnostics, a material constant overridden for every mesh that has it (ints too).
    if (const char* e = std::getenv("FH_MATSET")) {
        std::string rest = e;
        while (!rest.empty()) {
            const size_t c = rest.find(',');
            const std::string kv = rest.substr(0, c);
            rest = c == std::string::npos ? "" : rest.substr(c + 1);
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            for (const auto& m : r.members)
                if (m.name == kv.substr(0, eq) && m.offset + 4 <= std::max<size_t>(r.size, 16)) {
                    if (m.name.rfind("use", 0) == 0 || m.name.find("Skip") != std::string::npos) {
                        const int32_t v = std::atoi(kv.c_str() + eq + 1);
                        std::memcpy(reinterpret_cast<u8*>(f) + m.offset, &v, 4);
                    } else {
                        f[m.offset / 4] = static_cast<float>(std::atof(kv.c_str() + eq + 1));
                    }
                }
        }
    }
}

enum class Set2 { Material, Bones, Probe, Baker, Instance, Other };

// What a set-2 buffer holds, routed by name. The bone palette: named in most shaders; vista's ibl/ibltint declare it
// as an unnamed runtime array (left zero, every vertex collapsed to the origin and the whole architecture vanished).
inline Set2 set2Kind(const spv::Resource& r, bool bonedProgram) {
    const std::string n = lc(r.name);
    if (contains(n, "skinning") || contains(n, "sbskin") || contains(n, "bonemat") || (bonedProgram && r.name.empty() && r.size == 0 && r.binding != 0))
        return Set2::Bones;
    if (contains(n, "matparam") || (n.empty() && r.binding == 0)) return Set2::Material;
    if (contains(n, "lightprobe")) return Set2::Probe;
    if (contains(n, "lightbaker")) return Set2::Baker;
    if (contains(n, "instance")) return Set2::Instance;
    return Set2::Other;
}

// A set-2 buffer's contents (`f` holds max(r.size, 16) bytes at least), but for the bone palette (the renderer's).
// ambient: the scene's ambient light as the renderer uses it.
inline void fillSet2(const spv::Resource& r, Set2 kind, const MeshData& md, const std::string& programPath, const float ambient[3], float* f) {
    std::memset(f, 0, std::max<size_t>(r.size, 16));
    if (kind == Set2::Material) {
        fillMatParams(r, md, programPath, f);
    } else if (kind == Set2::Probe) {
        // L00 is authored by the material (HSR: "matl"); the higher SH bands are runtime probe data: zero.
        const std::vector<bool> got = fillBlock(r, md, f);
        if (std::find(got.begin(), got.end(), true) == got.end()) {
            for (int k = 0; k < 3; ++k) f[k] = ambient[k] * 3.5449f;
            f[3] = 1;
        }
    } else if (kind == Set2::Baker) {
        // {uvScale, uvOffset}, used as-is (no V-flip: HSR samples the lightmap with the raw UV2). The entity's
        // lightingParams atlas patch outranks the material's: materials author the neutral (1,1)/(0,0), and with it
        // every Theater wall sampled the whole shared 2048 atlas.
        const std::vector<bool> got = fillBlock(r, md, f);
        static const float kNeutral[4] = {1, 1, 0, 0};
        if (std::find(got.begin(), got.end(), true) == got.end() || std::memcmp(md.lmUvScaleOffset, kNeutral, 16) != 0)
            std::memcpy(f, md.lmUvScaleOffset, 16);
        if (const char* e = std::getenv("FH_LBSET"))   // diagnostics: every lightmap's uvScale.xy, uvOffset.xy
            std::sscanf(e, "%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]);
    } else if (kind == Set2::Instance) {
        f[0] = md.atlasCellIndex >= 0 ? md.atlasCellIndex : 0;
        f[1] = md.vatTrackIndex >= 0 ? md.vatTrackIndex : 0;
        f[2] = md.vatRateFactor >= 0 ? md.vatRateFactor : 1;
        f[3] = md.vatTimeOffset >= 0 ? md.vatTimeOffset : 0;
    }
}

// The bytes of the scene-wide uniform buffers that never change while the scene shows, as every program's shaders
// see them: the frame globals (set 0, binding 0: zero but for the white point, and reverseZ with FH_REVERSEZ; the
// matrices, camera, time, viewport and fog change and are never folded) and lightUniforms (set 1: zero but for the
// five members HSR fills with 1, for every program's layout of it).
struct SceneConstants {
    std::vector<u8> globals, globalsFoldable, light;
};

// globalsSize / lightSize: the renderer's buffers. The globals' member list is the largest any program declares.
inline SceneConstants sceneConstants(const std::map<std::string, ShaderProgramSrc>& programs, uint32_t globalsSize, uint32_t lightSize) {
    SceneConstants sc;
    sc.globals.assign(globalsSize, 0);
    sc.globalsFoldable.assign(globalsSize, 1);
    sc.light.assign(lightSize, 0);
    std::vector<spv::Member> globalMembers;
    for (const auto& kv : programs) {
        spv::Module vr, fr;
        if (!spv::reflect(kv.second.vert, vr) || !spv::reflect(kv.second.frag, fr)) continue;
        std::vector<spv::Resource> sets[3];
        mergedSets(vr, fr, sets);
        for (const auto& r : sets[0])
            if (r.binding == 0 && r.kind == spv::ResKind::UniformBuffer && r.members.size() > globalMembers.size()) globalMembers = r.members;
        for (const auto& r : sets[1]) {
            const std::string n = lc(r.name);
            if (r.kind != spv::ResKind::UniformBuffer || contains(n, "item") || contains(n, "grid")) continue;
            for (const auto& m : r.members) {
                const std::string mn = lc(m.name);
                // HSR's derived lightUniforms: these five are 1 (byte 112-156, 168-172), all else zero.
                const bool one = mn == "iblreflectiontint" || mn == "ibldiffusetint" || mn == "externalaverageluminance" ||
                                 mn == "iblreflectionmaxadaptationfactor" || mn == "iblreflectionluminanceinverse";
                if (!one || m.integer || m.offset + m.size > lightSize) continue;
                const float v = 1.0f;
                for (uint32_t k = 0; k * 4 < m.size; ++k) std::memcpy(&sc.light[m.offset + k * 4], &v, 4);
            }
        }
    }
    static const bool revZ = std::getenv("FH_REVERSEZ") != nullptr;
    for (const auto& m : globalMembers) {
        const std::string n = lc(m.name);
        const bool perFrame = contains(n, "clipfromworld") || contains(n, "viewfromworld") || contains(n, "worldcamerapos") || n == "fogcolor" ||
                              n == "fogstart" || n == "fogdensity" || n == "time" || n == "worldtime" || contains(n, "viewportsize");
        if (perFrame || m.offset + m.size > globalsSize) {
            for (uint32_t k = m.offset; k < std::min(m.offset + m.size, globalsSize); ++k) sc.globalsFoldable[k] = 0;
            continue;
        }
        const float one = 1;
        if (n == "whitepoint" || (n == "reversez" && revZ)) std::memcpy(&sc.globals[m.offset], &one, 4);
    }
    return sc;
}

// The fetch for spvspec::specialize: the scene's constant images for sets 0 and 1, the mesh's buffers for set 2.
// set2: binding -> bytes (the bone palette left out).
inline spvspec::Fetch fetchFor(const SceneConstants& sc, const std::vector<spv::Resource> sets[3],
                               const std::map<uint32_t, std::vector<u8>>& set2) {
    return [&sc, sets, &set2](uint32_t set, uint32_t binding, spvspec::BlockBytes& out) -> bool {
        const spv::Resource* res = nullptr;
        for (const auto& r : sets[std::min<uint32_t>(set, 2)]) if (r.binding == binding) res = &r;
        if (!res || res->kind != spv::ResKind::UniformBuffer) return false;
        if (set == 0) {
            out.data = sc.globals.data();
            out.size = static_cast<uint32_t>(sc.globals.size());
            const std::vector<u8>* mask = &sc.globalsFoldable;
            out.foldable = [mask](uint32_t o, uint32_t n) {
                for (uint32_t k = o; k < o + n; ++k) if (k >= mask->size() || !(*mask)[k]) return false;
                return true;
            };
            return true;
        }
        if (set == 1) {                          // lightUniforms (light items and the grid are storage buffers)
            const std::string n = lc(res->name);
            if (contains(n, "item") || contains(n, "grid")) return false;
            out.data = sc.light.data();
            out.size = static_cast<uint32_t>(sc.light.size());
            return true;
        }
        const auto it = set2.find(binding);
        if (it == set2.end()) return false;
        out.data = it->second.data();
        out.size = static_cast<uint32_t>(it->second.size());
        return true;
    };
}

// Whether a baked shader can be used as it is: made with the optimizer flags this run has (spv_opt.h), or by a build
// from before it (the renderer makes that one as it loads). One made otherwise (an A/B run with FH_SPV_OPT or
// FH_SPV_HALF against a .fenv baked without them, or the other way round) is made again from the shipped shader.
inline bool bakedUsable(const std::vector<u32>& code) {
    const int m = spvopt::markOf(code);
    return m < 0 || m == spvopt::flags();
}

// Which float values specialize bakes in: 0 none, 1 zeros, 2 zeros and +-1 (the default; FH_SPEC_FLOATS).
inline int floatsMode() {
    static const int m = std::getenv("FH_SPEC_FLOATS") ? std::atoi(std::getenv("FH_SPEC_FLOATS")) : 2;
    return m;
}

// The key of a mesh's specialization: every byte of the blocks its fragment shader reads constant members of (in
// blocksRead's order), and the float mode. The same in the bake and the renderer for the same values.
inline uint64_t variantKey(const std::vector<std::pair<uint32_t, uint32_t>>& blocks, const spvspec::Fetch& fetch, int floats) {
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](const u8* d, size_t n) { for (size_t k = 0; k < n; ++k) key = (key ^ d[k]) * 0x100000001B3ull; };
    const uint32_t mode = static_cast<uint32_t>(floats);
    mix(reinterpret_cast<const u8*>(&mode), 4);
    for (const auto& sb : blocks) {
        spvspec::BlockBytes bb;
        const uint32_t tag[3] = {sb.first, sb.second, fetch(sb.first, sb.second, bb) ? bb.size : 0};
        mix(reinterpret_cast<const u8*>(tag), sizeof tag);
        if (bb.data) mix(bb.data, bb.size);
        if (bb.foldable)                         // (the globals' mask is part of what was baked in)
            for (uint32_t k = 0; k < bb.size; ++k) { const u8 f = bb.foldable(k, 1) ? 1 : 0; mix(&f, 1); }
    }
    return key;
}

// Frame Studio's half: every program's prepared shaders and, per distinct set of material values its meshes have,
// the specialized fragment shader (and its alpha-only form), stored in the programs for the .fenv.
// programOf: the key in `programs` of the program the renderer draws a mesh with (empty: none).
inline void bakePrograms(const std::vector<MeshData>& meshes, const std::function<std::string(const MeshData&)>& programOf,
                         const float sceneAmbient[3], bool envAmbient, std::map<std::string, ShaderProgramSrc>& programs,
                         uint32_t globalsSize = 1024, uint32_t lightSize = 2048) {
    const SceneConstants sc = sceneConstants(programs, globalsSize, lightSize);
    float ambient[3];
    for (int k = 0; k < 3; ++k) ambient[k] = sceneAmbient[k] * (envAmbient ? 1.0f : 1.8f);
    struct Info { std::vector<spv::Resource> sets[3]; bool boned = false; std::vector<std::pair<uint32_t, uint32_t>> blocks; bool ok = false; };
    std::map<std::string, Info> info;
    for (auto& kv : programs) {
        ShaderProgramSrc& p = kv.second;
        p.vertInvariant = spvopt::run(invariantPosition(p.vert));
        p.fragAlpha = alphaOnly(p.frag);
        if (!p.fragAlpha.empty()) p.fragAlpha = spvopt::run(p.fragAlpha);
        p.variants.clear();
        p.prepared = true;
        Info& in = info[kv.first];
        spv::Module vr, fr;
        if (!spv::reflect(p.vert, vr) || !spv::reflect(p.frag, fr)) continue;
        mergedSets(vr, fr, in.sets);
        in.boned = boned(vr);
        in.blocks = spvspec::blocksRead(p.frag);
        in.ok = true;
    }
    const int floats = floatsMode();
    for (const MeshData& md : meshes) {
        const auto pi = programs.find(programOf(md));
        if (pi == programs.end()) continue;
        const Info& in = info[pi->first];
        if (!in.ok || in.blocks.empty()) continue;
        std::map<uint32_t, std::vector<u8>> set2;
        for (const auto& r : in.sets[2]) {
            if (r.kind != spv::ResKind::UniformBuffer || !r.size) continue;
            const Set2 kind = set2Kind(r, in.boned);
            if (kind == Set2::Bones) continue;
            std::vector<float> f((std::max<size_t>(r.size, 16) + 3) / 4, 0.0f);
            fillSet2(r, kind, md, pi->second.path, ambient, f.data());
            set2[r.binding].assign(reinterpret_cast<const u8*>(f.data()), reinterpret_cast<const u8*>(f.data()) + r.size);
        }
        const spvspec::Fetch fetch = fetchFor(sc, in.sets, set2);
        const uint64_t key = variantKey(in.blocks, fetch, floats);
        ShaderProgramSrc& p = pi->second;
        if (std::any_of(p.variants.begin(), p.variants.end(), [&](const ShaderProgramSrc::Variant& v) { return v.key == key; })) continue;
        uint32_t folded = 0;
        ShaderProgramSrc::Variant v;
        v.key = key;
        v.frag = spvspec::specialize(p.frag, fetch, &folded, -1, floats);
        if (!folded) v.frag.clear();                 // (nothing to bake in: the shader as shipped)
        else v.fragAlpha = alphaOnly(v.frag);   // (from the specialized shader as it is: its whole-colour stores)
        if (!v.frag.empty()) v.frag = spvopt::run(v.frag);
        if (!v.fragAlpha.empty()) v.fragAlpha = spvopt::run(v.fragAlpha);
        p.variants.push_back(std::move(v));
    }
}

}  // namespace shaderbake
