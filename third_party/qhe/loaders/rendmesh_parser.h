#pragma once
#include "core/types.h"
#include <cstring>
#include <cmath>
#include <algorithm>

// RENDMESH FlatBuffer parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/MeshAssetHandler.cpp
// + MeshAssetBuilder.cpp (sub_1816448 in libshell_current)
//
// Confirmed 4-level hierarchy:
//   Root → LOD [0] → Part [0] → VS [0] → VB (stride-20) + IB (u16)

inline bool parseRendMesh(const std::vector<u8>& data,
                          std::vector<float>& positions,
                          std::vector<float>& uvs,
                          std::vector<u32>& indices,
                          std::vector<u8>* boneIndices = nullptr,
                          std::vector<u8>* boneWeights = nullptr,
                          const char* label = nullptr,
                          std::vector<float>* uvs2 = nullptr,
                          std::vector<u32>* bonePalette = nullptr,   // bonePalette = ROOT.f2[0].f0 slot->joint name-hash MAP
                          std::vector<u8>* colors = nullptr,          // sem4 per-vertex COLOR (u8x4) = device vertexColor0 (animvege leaf bend-mask, butterfly wing colour)
                          std::vector<float>* uvs3 = nullptr,         // sem5 idx2 (uv2): animvege packed per-vertex flutter phase/pivot
                          std::vector<float>* uvs4 = nullptr,         // sem5 idx3 (uv3): animvege packed per-vertex flutter data
                          std::vector<float>* normals = nullptr,      // sem1 NORMAL (f16x3 / f32x3)
                          std::vector<float>* tangents = nullptr) {   // sem2 TANGENT (f16x4 / f32x4), w = handedness
    if (data.size() < 14 || memcmp(data.data() + 4, "MESH", 4) != 0)
        return false;

    if (data.size() > 0xFFFFFFF0u) return false;   // every offset below is a u32 into this buffer
    auto rootOff = loadUnaligned<u32>(data.data());
    const u64 SZ = data.size();

    // Offsets are file data: all sums in u64 (u32 wraps past the bounds checks), every read bounds-checked.
    auto fieldFo = [&](u32 tablePos, u32 fi) -> u16 {
        if ((u64)tablePos + 4 > SZ) return 0;
        i32 soff = loadUnaligned<i32>(data.data() + tablePos);
        const int64_t vt = (int64_t)tablePos - soff;
        if (vt < 0 || (u64)vt + 4 > SZ) return 0;
        u16 vtSize = loadUnaligned<u16>(data.data() + vt);
        u64 idx = (u64)vt + 4 + (u64)fi * 2;
        if (idx + 2 > (u64)vt + vtSize || idx + 2 > SZ) return 0;
        return loadUnaligned<u16>(data.data() + idx);
    };

    auto followVec = [&](u32 tablePos, u32 fi, u32& baseOut, u32& countOut) -> bool {
        u16 fo = fieldFo(tablePos, fi);
        if (!fo) return false;
        u64 fieldSlot = (u64)tablePos + fo;
        if (fieldSlot + 4 > SZ) return false;
        u32 uoff = loadUnaligned<u32>(data.data() + fieldSlot);
        if (!uoff) return false;
        u64 vecPos = fieldSlot + uoff;
        if (vecPos + 4 > SZ) return false;
        countOut = loadUnaligned<u32>(data.data() + vecPos);
        baseOut = (u32)(vecPos + 4);
        return true;
    };

    auto followElem = [&](u32 elemBase, u32 i, u32& tableOut) -> bool {
        u64 slot = (u64)elemBase + (u64)i * 4;
        if (slot + 4 > SZ) return false;
        u32 uoff = loadUnaligned<u32>(data.data() + slot);
        if (!uoff) return false;
        if (slot + uoff + 4 > SZ) return false;
        tableOut = (u32)(slot + uoff);
        return true;
    };

    auto followByteVec = followVec;   // same layout: [u32 count][bytes]

    // Bone PALETTE (the slot->joint MAP libshell reads, NOT a DFS guess): RENDMESH ROOT field[2] is a
    // vector of skins; skin[0].field[0] is a vector of u32 = MurmurHash3_x86_32(jointName,0), one per
    // palette slot. A vertex's bone INDEX is a slot into this; the slot's hash identifies the skeleton
    // joint by name. (Verified: bird palette=[mover,head,l_wing_01,l_wing_02,r_wing_01,r_wing_02,tail_01]
    // = joints 1,2,3,6,4,7,5; whale palette = its working DFS order. So reading this fixes the bird AND
    // keeps the whale.) The whole skin section is OPTIONAL (static meshes have no f2).
    if (bonePalette) {
        u32 skinBase, skinCount;
        if (followVec(rootOff, 2, skinBase, skinCount) && skinCount) {
            u32 skinTable;
            if (followElem(skinBase, 0, skinTable)) {
                u32 palBase, palCount;
                if (followVec(skinTable, 0, palBase, palCount) && palCount && palCount < 1024
                    && palBase + (size_t)palCount*4 <= data.size()) {
                    bonePalette->resize(palCount);
                    for (u32 i = 0; i < palCount; ++i)
                        (*bonePalette)[i] = loadUnaligned<u32>(data.data() + palBase + i*4);
                }
            }
        }
    }

    // Root -> LOD[0] (root field[1] = lod_uoff)
    u32 lodBase, lodCount;
    if (!followVec(rootOff, 1, lodBase, lodCount) || !lodCount) return false;
    u32 lodTable;
    if (!followElem(lodBase, 0, lodTable)) return false;

    // LOD -> Parts. A mesh can have MULTIPLE parts; prism_wave_a_01 has 2 — reading only Part[0]
    // rendered HALF the mesh (the "ring with a hole / borked" look). Read ALL parts, concatenating
    // verts and offsetting each part's index buffer by the running vertex count.
    u32 partBase, partCount;
    if (!followVec(lodTable, 0, partBase, partCount) || !partCount) return false;
    // A hostile count would spin ~4G iterations of failing parts; slots past the file are invalid anyway.
    partCount = (u32)std::min<u64>(partCount, (SZ - partBase) / 4);

    auto f16tof32 = [](u16 h) -> float {
        u32 s = (h >> 15) & 1;
        u32 e = (h >> 10) & 0x1F;
        u32 m = h & 0x3FF;
        if (e == 0) {
            if (m == 0) return s ? -0.0f : 0.0f;
            while (!(m & 0x400)) { m <<= 1; e--; }
            e++; m &= 0x3FF;
        } else if (e == 31) {
            return m ? NAN : (s ? -INFINITY : INFINITY);
        }
        u32 bits = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    };

    u32 totalV = 0;
    // Flag EVERY dropped part (a part that fails a check is silently lost geometry = a gap/hole). Printed
    // always so nothing is skipped silently. (A mesh can still load OTHER parts and "succeed" overall.)
    auto drop = [&](u32 p, const char* why) {
        fprintf(stderr, "[MESHDROP] %s : part %u/%u DROPPED — %s\n", label ? label : "?", p, partCount, why);
    };
    for (u32 pi = 0; pi < partCount; ++pi) {
        u32 partTable;
        if (!followElem(partBase, pi, partTable)) { drop(pi, "part table uoffset invalid"); continue; }
        // Part -> VS[0] (field[0]=vs_uoff) + IB bytes (field[1]=idx_uoff)
        u32 vsBase, vsCount;
        if (!followVec(partTable, 0, vsBase, vsCount) || !vsCount) { drop(pi, "no vertex-stream vector (field 0)"); continue; }
        u32 vsTable;
        if (!followElem(vsBase, 0, vsTable)) { drop(pi, "VS[0] uoffset invalid"); continue; }
        u32 ibStart, ibCount;
        if (!followByteVec(partTable, 1, ibStart, ibCount) || !ibCount) { drop(pi, "no index buffer (field 1)"); continue; }
        // VS -> vertex_count (field[1]) + VB bytes (field[2])
        u16 vcFo = fieldFo(vsTable, 1);
        if (!vcFo || (u64)vsTable + vcFo + 4 > SZ) { drop(pi, "no vertex_count field"); continue; }
        u32 nVerts = loadUnaligned<u32>(data.data() + vsTable + vcFo);
        u32 vbStart, vbCount;
        if (!followByteVec(vsTable, 2, vbStart, vbCount) || !nVerts) { drop(pi, "no vertex buffer / 0 verts"); continue; }
        if (vbCount % nVerts != 0) { char b[80]; snprintf(b,sizeof b,"vbCount %u not divisible by nVerts %u", vbCount, nVerts); drop(pi, b); continue; }
        u32 stride = vbCount / nVerts;
        // Minimum valid vertex = pos f32x3 (12) + uv f16x2 (4) = 16 bytes. The old `< 20` threshold
        // SKIPPED every stride-16 mesh — that's the rocks/cliffs/heroRock/vistaSand (the vista MOUNTAINS
        // & terrain), which are pos+uv only (no normal). They were dropped entirely ("where are the
        // mountains?"). Accept stride >= 16.
        // Accept stride>=12 (POSITION-ONLY f32x3 meshes: haven2025 blockout walls + emissive fixtures —
        // emissCabinet/Chairs/Shelves/Lights, blktWalls — have NO uv/normal; they were dropped at <16,
        // leaving the home half-built). uv defaults to (0,0) below when there's no room for it.
        // Parts may alias one vertex buffer: cap the running total so a hostile part list can't grow the
        // outputs without bound or wrap the u32 (base+nVerts)*N sizes below. Real homes are far below 64M.
        if ((u64)totalV + nVerts > (1u << 26)) { drop(pi, "vertex total over cap"); continue; }
        if (stride < 12 || vbStart + (size_t)nVerts * stride > data.size()) { char b[96]; snprintf(b,sizeof b,"stride=%u (<12 min) or vbuf OOB (nVerts=%u)", stride, nVerts); drop(pi, b); continue; }
        if (std::getenv("HSR_RMSTRUCT"))
            fprintf(stderr, "[RMSTRUCT] part %u/%u nVerts=%u stride=%u ibCount=%u base=%u\n",
                    pi, partCount, nVerts, stride, ibCount, totalV);

    // ── FAITHFUL vertex format (1:1 with the VS table, NOT auto-detected) ──────────────
    // VS field[3] = a vector of 4-byte attribute structs {u8 semantic, u8 format, u8 index, u8 _}.
    // Walk them, summing each format's byte size, to get each attribute's offset. TEXCOORD0
    // (semantic 5, index 0) is uv0; TEXCOORD1 (sem 5, idx 1) is the lightmap uv1. This is what
    // the renderer's auto-detect could never get: SculptureA's uv0 is at +18 (after a 6-byte
    // f16x3 normal), and the 4-byte-aligned scan only tried 12/16/20/24. (semantic: 0=POSITION
    // 1=NORMAL 5=TEXCOORD; format byte high-nibble≈type 3=f32/2=f16/1=u8, decoded by size below.)
    u32 fmtUv0 = 0xFFFFFFFF, fmtUv1 = 0xFFFFFFFF; u8 uv1FmtByte = 0;
    u32 fmtColor = 0xFFFFFFFF;                              // VS-attr-table sem 4 = per-vertex COLOR (u8x4) offset
    u32 fmtUv2 = 0xFFFFFFFF, fmtUv3 = 0xFFFFFFFF; u8 uv2FmtByte = 0, uv3FmtByte = 0;  // sem5 idx2/idx3 (animvege packed uvs)
    u32 fmtBoneIdx = 0xFFFFFFFF, fmtBoneWgt = 0xFFFFFFFF;   // VS-attr-table bone offsets (sem 8=indices, 7=weights)
    u32 fmtNormal = 0xFFFFFFFF, fmtTangent = 0xFFFFFFFF; u8 normalFmt = 0, tangentFmt = 0;   // sem 1 / sem 2
    {
        u32 fb2, fc2;
        if (followVec(vsTable, 3, fb2, fc2) && fc2 && fc2 < 32 && fb2 + fc2*4 <= data.size()) {
            auto fmtSize = [](u8 f) -> u32 {
                switch (f) {
                    case 0x30: return 4;   case 0x31: return 8;   case 0x32: return 12;  case 0x33: return 16;  // f32 x1..4
                    case 0x20: return 2;   case 0x21: return 4;   case 0x22: return 6;   case 0x23: return 8;   // f16 x1..4
                    case 0x27: return 4;                                                                        // u16x2 (lightmap uv)
                    case 0x10: case 0x11: return 4;                                                             // u8x4 (color/bones)
                    default:   return 4;
                }
            };
            u32 aoff = 0;
            for (u32 ai = 0; ai < fc2; ++ai) {
                const u8* a = data.data() + fb2 + ai*4;
                u8 sem = a[0], fmt = a[1], idx = a[2];
                if (sem == 5 && idx == 0 && fmtUv0 == 0xFFFFFFFF) fmtUv0 = aoff;
                if (sem == 5 && idx == 1 && fmtUv1 == 0xFFFFFFFF) { fmtUv1 = aoff; uv1FmtByte = fmt; }
                if (sem == 4 && fmtColor == 0xFFFFFFFF) fmtColor = aoff;   // per-vertex COLOR (vertexColor0)
                if (sem == 5 && idx == 2 && fmtUv2 == 0xFFFFFFFF) { fmtUv2 = aoff; uv2FmtByte = fmt; }  // animvege uv2
                if (sem == 5 && idx == 3 && fmtUv3 == 0xFFFFFFFF) { fmtUv3 = aoff; uv3FmtByte = fmt; }  // animvege uv3
                // Skinning bones by SEMANTIC (the whale: sem 7 = weights @16, sem 8 = indices @20, stride 24).
                // The fixed stride-28 layout (idx@20/wgt@24, stride>=28) misses these -> skinned=0. (sem 6 is
                // also seen as indices in some streams.)
                if ((sem == 8 || sem == 6) && fmtBoneIdx == 0xFFFFFFFF) fmtBoneIdx = aoff;
                if (sem == 7 && fmtBoneWgt == 0xFFFFFFFF) fmtBoneWgt = aoff;
                if (sem == 1 && fmtNormal == 0xFFFFFFFF) { fmtNormal = aoff; normalFmt = fmt; }
                if (sem == 2 && fmtTangent == 0xFFFFFFFF) { fmtTangent = aoff; tangentFmt = fmt; }
                aoff += fmtSize(fmt);
            }
            if (std::getenv("HSR_RMSTRUCT") && (fmtBoneIdx != 0xFFFFFFFF || fmtBoneWgt != 0xFFFFFFFF)) {
                fprintf(stderr, "[RMFMT] %u attrs:", fc2); u32 ao=0;
                for (u32 ai=0; ai<fc2; ++ai){ const u8* a=data.data()+fb2+ai*4; fprintf(stderr," [sem%u fmt0x%02x idx%u @%u]", a[0],a[1],a[2],ao); ao+=fmtSize(a[1]); }
                fprintf(stderr, "  boneIdx@%d boneWgt@%d\n", (int)fmtBoneIdx, (int)fmtBoneWgt);
            }
            else if (std::getenv("HSR_RMSTRUCT")) {
                fprintf(stderr, "[RMFMT] %u attrs:", fc2); u32 ao=0;
                for (u32 ai=0; ai<fc2; ++ai){ const u8* a=data.data()+fb2+ai*4; fprintf(stderr," [sem%u fmt0x%02x idx%u @%u]", a[0],a[1],a[2],ao); ao+=fmtSize(a[1]); }
                fprintf(stderr, " uv0@%d uv1@%d\n", (int)fmtUv0, (int)fmtUv1);
            }
        }
    }

    // Position is always the first attribute (3x f32 @ +0). UV offset, however,
    // varies by vertex layout: nuxd is stride-20 with UV f16x2 @ +12, but other
    // envs (haven2025) use wider strides (48) with normal/tangent before the UV, so
    // a hardcoded +12 reads garbage. Auto-DETECT the UV attribute: scan every 2-byte
    // aligned slot in [12, stride-4) and score it as f16x2 across sampled vertices —
    // a real texcoord set has values mostly within roughly [-0.05, 1.05] (tiled UVs
    // can exceed 1 but cluster low) and is NOT constant. Pick the best-scoring slot.
    // UV is f16x2 and 4-byte aligned, right after position in most layouts (off 12).
    // Only consider 4-byte-aligned slots (mis-aligned 2-byte reads of color/normal
    // bytes can look UV-like and were being mis-picked, garbling props/pillows).
    // A real texcoord set: nearly all samples finite & in a sane range, AND varies
    // across vertices (constant slots are packed color/normal). Pick the EARLIEST
    // qualifying slot (UV precedes normal/tangent), preferring offset 12.
    u32 uvOff = (fmtUv0 != 0xFFFFFFFF) ? fmtUv0 : 12;   // FAITHFUL VS-format value wins
    // Wide layout (haven2025 / V203 USD, stride 48): pos f32x3 @0 + normal f32x3 @12 + uv0 f16x2 @24
    // (matches the V203 shader's vertex stream pos/norm/uv). The generic f16x2 auto-detect below mis-fires
    // here because the f32 NORMAL bytes @12 reinterpret as plausible f16 UVs -> magenta UV scramble.
    // So FIRST probe for a unit-length f32 normal at @12; if present, uv0 sits right after it at @24.
    bool normAt12 = false;
    if (stride >= 40) {
        normAt12 = true;
        u32 sN = nVerts < 32 ? nVerts : 32;
        for (u32 i = 0; i < sN && normAt12; ++i) {
            float nf[3];
            std::memcpy(nf, data.data() + vbStart + i * stride + 12, sizeof nf);
            float L = std::sqrt(nf[0]*nf[0] + nf[1]*nf[1] + nf[2]*nf[2]);
            if (!(L > 0.9f && L < 1.1f)) normAt12 = false;
        }
    }
    // stride==28 is the VERIFIED skinned layout (pos@0, uv f16x2 @12, color@16, boneIdx@20, boneWgt@24).
    // Do NOT auto-detect there: a skinned surface with TILED UVs (prism_wave, 32x32) has uv values > the
    // detector's [-4,16] range at off 12, so it rejected the real UV and mis-picked the boneIdx bytes as
    // UVs -> garbled texture. Auto-detect only for the variable non-skinned layouts.
    if (fmtUv0 != 0xFFFFFFFF) {
        // uvOff already = the faithful TEXCOORD0 offset from the VS attribute table; skip heuristics.
    } else if (normAt12) {
        uvOff = 24;
    } else if (stride != 28) {
        u32 sampleN = nVerts < 128 ? nVerts : 128;
        u32 chosen = 0xFFFFFFFF;
        for (u32 off = 12; off + 4 <= stride; off += 4) {
            int inRange = 0; float minU=1e9f,maxU=-1e9f,minV=1e9f,maxV=-1e9f;
            for (u32 i = 0; i < sampleN; ++i) {
                const u8* v = data.data() + vbStart + i * stride;
                float u = f16tof32(loadUnaligned<u16>(v + off));
                float w = f16tof32(loadUnaligned<u16>(v + off + 2));
                if (std::isfinite(u) && std::isfinite(w) &&
                    u > -4.0f && u < 16.0f && w > -4.0f && w < 16.0f) {
                    inRange++;
                    if(u<minU)minU=u; if(u>maxU)maxU=u; if(w<minV)minV=w; if(w>maxV)maxV=w;
                }
            }
            bool allValid = (inRange >= (int)(sampleN * 0.95f));
            bool varies = (maxU - minU > 0.02f) || (maxV - minV > 0.02f);
            if (allValid && varies) { chosen = off; break; }  // earliest wins
        }
        uvOff = (chosen != 0xFFFFFFFF) ? chosen : 12;
    }
    // (stride==28 keeps uv0@12 — the verified skinned layout AND haven2025's static props' face uv0.
    //  SculptureA's emissive "checker" is NOT a uv-offset bug: uv0@12 is the [0,1] face UV; the emissive
    //  is an ATLAS so each face samples the whole atlas. The real fix is per-cube atlas-cell remapping.)
    if (const char* uo = std::getenv("HSR_UVOFF")) uvOff = (u32)atoi(uo);   // DIAG: force UV offset

    if (std::getenv("HSR_RMFMT") && label && strstr(label, "culptur") && nVerts >= 2) {
        fprintf(stderr, "[RMFMT] %s part stride=%u uvOff_detected=%u nVerts=%u normAt12=%d\n", label, stride, uvOff, nVerts, (int)normAt12);
        const u8* v0 = data.data() + vbStart;
        const u8* v1 = data.data() + vbStart + stride;
        for (u32 o = 0; o + 4 <= stride; o += 4) {
            float f = loadUnaligned<float>(v0 + o);
            float a0 = f16tof32(loadUnaligned<u16>(v0 + o)), b0 = f16tof32(loadUnaligned<u16>(v0 + o + 2));
            float a1 = f16tof32(loadUnaligned<u16>(v1 + o)), b1 = f16tof32(loadUnaligned<u16>(v1 + o + 2));
            int16_t s0 = loadUnaligned<int16_t>(v0 + o), s2 = loadUnaligned<int16_t>(v0 + o + 2);
            fprintf(stderr, "   @%2u f32=%11.4f  f16v0=(%8.3f,%8.3f) f16v1=(%8.3f,%8.3f) i16=(%6d,%6d)\n",
                    o, f, a0, b0, a1, b1, s0, s2);
        }
    }

        u32 base = totalV;
        positions.resize((base + nVerts) * 3);
        uvs.resize((base + nVerts) * 2);
        for (u32 i = 0; i < nVerts; ++i) {
            const u8* v = data.data() + vbStart + i * stride;
            positions[(base+i)*3+0] = loadUnaligned<float>(v + 0);
            positions[(base+i)*3+1] = loadUnaligned<float>(v + 4);
            positions[(base+i)*3+2] = loadUnaligned<float>(v + 8);
            if (uvOff + 4 <= stride) {   // pos-only (stride-12) meshes have no uv -> (0,0), avoids reading the next vertex
                uvs[(base+i)*2+0] = f16tof32(loadUnaligned<u16>(v + uvOff));
                uvs[(base+i)*2+1] = f16tof32(loadUnaligned<u16>(v + uvOff + 2));
            } else { uvs[(base+i)*2+0] = 0.0f; uvs[(base+i)*2+1] = 0.0f; }
        }

        // TEXCOORD1 (uv1 = lightmap unwrap): the rgmask/lit shaders sample the ONxRNy/RBAoDir mask AND
        // the lightmap at uv1, NOT uv0. Without it the mask was sampled at uv0 (the material unwrap) ->
        // the carpet's design tiled into "multiple circles", the ceiling got white spots, the couch
        // looked dusty. Decode at the FAITHFUL VS-table offset, honouring its format (lightmap uv1 is
        // commonly u16x2 UNORM (fmt 0x27), else f16x2 (0x2x) / f32x2 (0x31)).
        if (uvs2 && fmtUv1 != 0xFFFFFFFF && (u64)fmtUv1 + (uv1FmtByte == 0x31 ? 8 : 4) <= stride) {   // f32x2 reads 8 bytes
            uvs2->resize((base + nVerts) * 2);
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride + fmtUv1;
                float u, w;
                if (uv1FmtByte == 0x27) {        // u16x2 UNORM
                    u = loadUnaligned<u16>(v)     / 65535.0f;
                    w = loadUnaligned<u16>(v + 2) / 65535.0f;
                } else if (uv1FmtByte == 0x31) { // f32x2
                    u = loadUnaligned<float>(v);
                    w = loadUnaligned<float>(v + 4);
                } else {                          // f16x2 (default)
                    u = f16tof32(loadUnaligned<u16>(v));
                    w = f16tof32(loadUnaligned<u16>(v + 2));
                }
                (*uvs2)[(base+i)*2+0] = u; (*uvs2)[(base+i)*2+1] = w;
            }
        }

        // Authored NORMAL / TANGENT (what HSR Studio feeds inNorm / inTangent). Normal-mapped surfaces need the
        // real tangent frame: a constant (1,0,0,+1) tangent bent their normal maps into false reflections.
        auto readVec = [&](std::vector<float>* out, u32 off, u8 fmt, int comps) {
            if (!out) return;
            out->resize(static_cast<size_t>(base + nVerts) * comps, 0.0f);   // zero = this part authors none
            const bool f32 = (fmt & 0xF0) == 0x30, f16 = (fmt & 0xF0) == 0x20;
            const int have = (fmt & 0x0F) + 1;
            if (off == 0xFFFFFFFF || (!f32 && !f16) || off + have * (f32 ? 4 : 2) > stride) return;
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride + off;
                for (int c = 0; c < comps && c < have; ++c)
                    (*out)[(base + i) * comps + c] = f32 ? loadUnaligned<float>(v + c * 4)
                                                         : f16tof32(loadUnaligned<u16>(v + c * 2));
            }
        };
        readVec(normals, fmtNormal, normalFmt, 3);
        readVec(tangents, fmtTangent, tangentFmt, 4);

        // sem4 per-vertex COLOR (u8x4) = the device shader's vertexColor0. libshell multiplies base·vertexColor0;
        // the renderer's VBO was writing WHITE into role 4, so the GREYSCALE butterfly texture stayed white and
        // the animvege leaf bend-mask (packed in the colour) was lost. Extract the real per-vertex colour.
        if (colors && fmtColor != 0xFFFFFFFF && fmtColor + 4 <= stride) {
            colors->resize((base + nVerts) * 4);
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride + fmtColor;
                (*colors)[(base+i)*4+0] = v[0]; (*colors)[(base+i)*4+1] = v[1];
                (*colors)[(base+i)*4+2] = v[2]; (*colors)[(base+i)*4+3] = v[3];
            }
        }
        // sem5 idx2/idx3 (uv2/uv3): animvege packs per-vertex flutter phase/pivot here. The vertex shader bends
        // each leaf using these; without them (mapped to uv0 before) the plants don't flutter as authored.
        auto decExtraUV = [&](std::vector<float>* out, u32 foff, u8 fb){
            if (!out || foff == 0xFFFFFFFF || (u64)foff + (fb == 0x31 ? 8 : 4) > stride) return;   // f32x2 reads 8 bytes
            out->resize((base + nVerts) * 2);
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride + foff; float u, w;
                if      (fb == 0x27) { u = loadUnaligned<u16>(v)/65535.0f; w = loadUnaligned<u16>(v+2)/65535.0f; }
                else if (fb == 0x31) { u = loadUnaligned<float>(v);        w = loadUnaligned<float>(v+4); }
                else                 { u = f16tof32(loadUnaligned<u16>(v)); w = f16tof32(loadUnaligned<u16>(v+2)); }
                (*out)[(base+i)*2+0] = u; (*out)[(base+i)*2+1] = w;
            }
        };
        decExtraUV(uvs3, fmtUv2, uv2FmtByte);
        decExtraUV(uvs4, fmtUv3, uv3FmtByte);

        // Bone indices + weights. VERIFIED stride==28 layout: pos@0(12) + uv@12(4 f16x2) +
        // color@16(4 u8) + boneIdx@20(4 u8) + boneWgt@24(4 u8). Non-skinned parts (stride<28)
        // bind to bone 0 at full weight so they ride the node transform.
        // Only a skinned mesh (some part has the stride>=28 bone layout) gets bones; a purely static
        // mesh keeps boneIndices empty -> static pipeline. A static part inside a skinned mesh binds bone 0.
        // Bones present if the VS attr table declared them (sem 7/8 — whale, stride 24) OR the stride-28 layout.
        bool attrBones = (fmtBoneIdx != 0xFFFFFFFF && fmtBoneWgt != 0xFFFFFFFF &&
                          fmtBoneIdx + 4 <= stride && fmtBoneWgt + 4 <= stride);
        // The sem7/sem8 -> WEIGHTS/INDICES assignment differs by env: nuxd has sem7=weights/sem8=indices,
        // but the V203 whale is the OPPOSITE (sem7=indices @16, sem8=weights @20). Detect from the data —
        // skin weights are u8-normalized (sum ~255 per vertex), indices are small (< joint count). Swap if
        // the offset we tagged "indices" actually holds the ~255-summing weights.
        if (attrBones) {
            double sIdx = 0, sWgt = 0; u32 ns = 0;
            for (u32 i = 0; i < nVerts && ns < 64; ++i, ++ns) {
                const u8* v = data.data() + vbStart + i * stride;
                sIdx += v[fmtBoneIdx]+v[fmtBoneIdx+1]+v[fmtBoneIdx+2]+v[fmtBoneIdx+3];
                sWgt += v[fmtBoneWgt]+v[fmtBoneWgt+1]+v[fmtBoneWgt+2]+v[fmtBoneWgt+3];
            }
            if (ns) { sIdx /= ns; sWgt /= ns; if (sIdx > 128.0 && sWgt < 128.0) std::swap(fmtBoneIdx, fmtBoneWgt); }
        }
        if (boneIndices && boneWeights && (attrBones || stride >= 28 || !boneIndices->empty())) {
            boneIndices->resize((base + nVerts) * 4, 0);
            boneWeights->resize((base + nVerts) * 4, 0);
            for (u32 i = 0; i < nVerts; ++i) {
                const u8* v = data.data() + vbStart + i * stride;
                if (attrBones) {       // FAITHFUL: bone idx/wgt at their VS-attr-table offsets (any stride)
                    for (int k = 0; k < 4; ++k) { (*boneIndices)[(base+i)*4+k] = v[fmtBoneIdx+k]; (*boneWeights)[(base+i)*4+k] = v[fmtBoneWgt+k]; }
                } else if (stride >= 28) {
                    for (int k = 0; k < 4; ++k) { (*boneIndices)[(base+i)*4+k] = v[20+k]; (*boneWeights)[(base+i)*4+k] = v[24+k]; }
                } else {
                    (*boneWeights)[(base+i)*4+0] = 255;  // bind to bone 0
                }
            }
        }

        // Index buffer, offset by this part's vertex base; clamp OOB to base. Meta's cook writes u16; converted
        // homes (desert-terrace, HSR-cooked Clay) write u32. Read as u16, a u32 buffer turns every high half (0)
        // into "vertex 0" and the mesh explodes into spikes converging on one point. Tell them apart by the data:
        // u32 when the part needs it (>= 65536 vertices) or when (nearly) every odd u16 word is zero.
        bool idx32 = false;
        if (ibCount % 4 == 0 && ibCount >= 12) {
            if (nVerts > 65535) idx32 = true;
            else {
                const u32 words = std::min<u32>(ibCount / 2, 4096);
                u32 oddZero = 0, odd = 0;
                for (u32 w = 1; w < words; w += 2) {
                    const u64 off = (u64)ibStart + (u64)w * 2;
                    if (off + 2 > data.size()) break;
                    ++odd;
                    oddZero += loadUnaligned<u16>(data.data() + off) == 0;
                }
                idx32 = odd >= 6 && oddZero * 100 >= odd * 97;
            }
        }
        if (idx32) {
            const u32 nIdx = ibCount / 4;
            for (u32 i = 0; i < nIdx; ++i) {
                const u64 off = (u64)ibStart + (u64)i * 4;
                if (off + 4 > data.size()) break;
                const u32 idx = loadUnaligned<u32>(data.data() + off);
                indices.push_back(idx < nVerts ? base + idx : base);
            }
        } else {
            u32 nIdx = ibCount / 2;
            for (u32 i = 0; i < nIdx; ++i) {
                u64 off = (u64)ibStart + (u64)i * 2;
                if (off + 2 > data.size()) break;
                u16 idx = loadUnaligned<u16>(data.data() + off);
                indices.push_back((idx < nVerts) ? (base + (u32)idx) : base);
            }
        }
        if (std::getenv("HSR_RMSTRUCT")) fprintf(stderr, "[RMSTRUCT]   indices %s\n", idx32 ? "u32" : "u16");
        if (std::getenv("HSR_RMVTX") && stride >= 28 && nVerts > 1000) {
            float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
            for (u32 i = base; i < base+nVerts; ++i) for (int c=0;c<3;c++){ float p=positions[i*3+c]; if(p<mn[c])mn[c]=p; if(p>mx[c])mx[c]=p; }
            fprintf(stderr, "[RMVTX] part%u bounds x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f]\n", pi, mn[0],mx[0],mn[1],mx[1],mn[2],mx[2]);
            for (int s=0;s<4;s++){ const u8* v=data.data()+vbStart+(size_t)s*stride;
                fprintf(stderr,"  v%d pos=(%.3f,%.3f,%.3f) raw=", s,*(const float*)v,*(const float*)(v+4),*(const float*)(v+8));
                for(int b=0;b<28;b++) fprintf(stderr,"%02x",v[b]); fprintf(stderr,"\n"); }
        }
        totalV += nVerts;
    }

    return totalV > 0 && !indices.empty();
}
