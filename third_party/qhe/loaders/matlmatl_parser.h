#pragma once
#include "core/types.h"
#include <cstring>
#include <vector>
#include <string>

// MATLMATL FlatBuffer parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/MaterialAssetHandler.cpp
// (sub_B374C4 in libshell)

struct MatlmatlInfo {
    u64 shaderPkg = 0;
    u64 shaderIng = 0;
    u32 shaderTgt = 0;
    u64 texPkg = 0;
    u64 texIng = 0;       // chosen (base-color) texture ing
    u32 texTgt = 0;
    std::vector<u64> texIngs;  // ALL texture ings referenced by this material, in file order
    // The cooked material bakes its final matParams VALUE blob (Tint/LayerRed/LayerBlue/Metallic/
    // Roughness/... tight-packed in .surface param order): u32 size @ +76, raw bytes @ +80.
    std::vector<u8> constBlock;
    // The `constantParameters` vector entries {nameHash, blobOffset, byteSize} — each slices constBlock
    // and binds BY NAME (MurmurHash3 of "matParams."+member) to the shader UBO. See ConstParam.
    std::vector<ConstParam> constParams;
    // Candidate texture-SLOT name-hashes from `textureParameters` (each entry names its sampler slot by
    // MurmurHash3 of the sampler name, e.g. "BaseColor_Tx"/"RBAoDir_Tx"/"LightMap_Tx"). Used to pick the
    // shader VARIANT whose samplers match this material (the env ships several rgmasked variants and the
    // builder otherwise grabs the wrong/largest one). Collected loosely (all entry u32s); only real
    // sampler hashes will match a shader's sampler names.
    std::vector<u32> texSlots;
    // `textureValues` (MATL root field 8 / vtidx10): the FAITHFUL texture bindings. Each entry pairs
    // {samplerNameHash, textureIng} — device binds the texture to the shader sampler whose
    // MurmurHash3_x86_32(samplerName,0) == samplerNameHash (PROVEN: MurmurHash3("baseColorTex")==0xe9c182de).
    // Layout from MaterialDefinition::fix (0xeb6ad4) + MaterialDefinition__2232164 (0x2232164):
    // entry field0 = AssetReference {pkg@0,ing@8,tgt@16}; entry field1 = u32 samplerNameHash.
    // This replaces the old tgt-sentinel byte-scan, which picked the WRONG ing for USD-derived materials
    // (the real diffuse fell into the "unreferenced" texture bucket -> the mesh rendered WHITE).
    std::vector<std::pair<u32,u64>> samplerTex;  // {samplerNameHash, ing}, in file order
    // This MATL is a clone of OUR cook's TRANSPARENT template (cooker/realdome_mat.bin = the unlitblend
    // MATL): the cook only ever patches asset REFS into it, so bytes [0..48) (root offset + magic + vtable)
    // are template-constant. The HSL preview uses this as the FAITHFUL blend signal — paired with an
    // opaque-family shader it is the cook's ADDITIVE-GLOW signature.
    bool isCookBlendTpl = false;
    // This MATL is a clone of OUR cook's OPAQUE SKINNED template (cooker/skinned_mat_v2.bin, matSkin2):
    // authoritative "render OPAQUE with depth-write". Needed because parseRendShadForward's f4-omission
    // heuristic MISREADS skinned surfaces (skinuv/unlitdoublesidedskinned report transp=1) -> every cooked
    // skinned mesh classified alpha-blend/no-depth-write -> overlapping river cards
    // z-fought and composited dark ("wrong placements / wrong lighting"). The cook contract wins.
    bool isCookOpaqueSkinTpl = false;
};

inline bool parseMatlmatl(const std::vector<u8>& data, MatlmatlInfo& info) {
    // Need at least 68 bytes for shader ref (at offsets 48/56/64) + MATL magic at +4
    if (data.size() < 68) return false;
    if (memcmp(data.data() + 4, "MATL", 4) != 0) return false;
    {   // fingerprint of cooker/realdome_mat.bin (unlitblend template) bytes [0..48): root+magic+vtable —
        // constant across every MATL the cook emits from it (only refs @48+/@120+ get patched).
        static const u8 kBlendTpl48[48] = {
            0x1c,0x00,0x00,0x00, 0x4d,0x41,0x54,0x4c, 0x00,0x00,0x00,0x00, 0x10,0x00,0x30,0x00,
            0x00,0x00,0x14,0x00, 0x10,0x00,0x0c,0x00, 0x08,0x00,0x04,0x00, 0x10,0x00,0x00,0x00,
            0x2c,0x00,0x00,0x00, 0x3c,0x00,0x00,0x00, 0x68,0x00,0x00,0x00, 0x02,0x00,0x00,0x00 };
        info.isCookBlendTpl = data.size() >= 48 && memcmp(data.data(), kBlendTpl48, 48) == 0;
        // fingerprint of cooker/skinned_mat_v2.bin (matSkin2, the OPAQUE skinned MATL) bytes [0..48) —
        // refs are patched at 48+/120+ so the header is template-constant (verified against a cooked m701).
        static const u8 kOpaqueSkinTpl48[48] = {
            0x20,0x00,0x00,0x00, 0x4d,0x41,0x54,0x4c, 0x00,0x00,0x16,0x00, 0x2c,0x00,0x2a,0x00,
            0x00,0x00,0x00,0x00, 0x0c,0x00,0x00,0x00, 0x04,0x00,0x00,0x00, 0x10,0x00,0x08,0x00,
            0x16,0x00,0x00,0x00, 0x28,0x00,0x00,0x00, 0x38,0x00,0x00,0x00, 0x64,0x00,0x00,0x00 };
        info.isCookOpaqueSkinTpl = data.size() >= 48 && memcmp(data.data(), kOpaqueSkinTpl48, 48) == 0;
    }

    // Shader ref = MATL root table FIELD INDEX 7 (FlatBuffer vtable), an inline AssetReference struct
    // {pkg u64 @0, ing u64 @8, tgt u32 @16}. This is how libshell's generated accessor reads it — by
    // field index via the vtable, NOT a fixed byte offset. The old hardcoded @56 only worked when the
    // vtable happened to place field 7 at offset 16 (filepos 48, ing@56 — couch/carpet/walls); a material
    // with a different field layout (the VAT bubbles, root=44) puts field 7 at offset 20 (ing@72), so @56
    // read garbage (00000002000000CC) and the vatlitbubble shader never resolved -> bubbles stayed broken.
    {
        u32 root = loadUnaligned<u32>(data.data());
        bool gotShader = false;
        if ((size_t)root + 4 <= data.size()) {
            int32_t soff = loadUnaligned<int32_t>(data.data() + root);
            int64_t vt = (int64_t)root - soff;
            if (vt >= 0 && (size_t)vt + 4 <= data.size()) {
                u16 vtsize = loadUnaligned<u16>(data.data() + vt);
                const u32 SHADER_FIELD = 7;
                if (4 + SHADER_FIELD * 2 + 2 <= vtsize) {
                    u16 fo = loadUnaligned<u16>(data.data() + vt + 4 + SHADER_FIELD * 2);
                    if (fo) {
                        size_t sp = (size_t)root + fo;
                        if (sp + 20 <= data.size() &&
                            loadUnaligned<u32>(data.data() + sp + 16) == 0xA1767FE9u) {  // .surface tgt
                            info.shaderPkg = loadUnaligned<u64>(data.data() + sp);
                            info.shaderIng = loadUnaligned<u64>(data.data() + sp + 8);
                            info.shaderTgt = loadUnaligned<u32>(data.data() + sp + 16);
                            gotShader = true;
                        }
                    }
                }
            }
        }
        if (!gotShader) {   // legacy fallback (v79_test / older layouts)
            info.shaderPkg = loadUnaligned<u64>(data.data() + 48);
            info.shaderIng = loadUnaligned<u64>(data.data() + 56);
            info.shaderTgt = loadUnaligned<u32>(data.data() + 64);
        }
    }

    // Cooked matParams value blob = MATL root table FIELD 5 (a ubyte vector), read via the vtable — the
    // REAL Tint/LayerRed/Metallic/atlasSubDiv/vat_AnimTracksCount values. The old fixed @+76/+80 only worked
    // when the vtable placed field 5 at vecPos 76 (haven2025 root=32 -> data@80). A different layout (the VAT
    // bubbles material, root=44) puts field 5's vector at vecPos 92 (data@96), so @+76 read an empty/garbage
    // block -> atlasSubDivX/Y & vat_AnimTracksCount defaulted to 0 -> the VAT per-vertex column/atlas math
    // collapsed -> bubbles rendered HUGE/spiky. Reading by field index (like the shader ref) fixes it.
    {
        bool got = false;
        u32 root = loadUnaligned<u32>(data.data());
        if ((size_t)root + 4 <= data.size()) {
            int32_t soff = loadUnaligned<int32_t>(data.data() + root);
            int64_t vt = (int64_t)root - soff;
            if (vt >= 0 && (size_t)vt + 4 <= data.size()) {
                u16 vts = loadUnaligned<u16>(data.data() + vt);
                if (4 + 5 * 2 + 2 <= vts) {
                    u16 fo = loadUnaligned<u16>(data.data() + vt + 4 + 5 * 2);
                    if (fo) {
                        size_t fpos = (size_t)root + fo;
                        if (fpos + 4 <= data.size()) {
                            u32 uoff = loadUnaligned<u32>(data.data() + fpos);
                            size_t vp = fpos + uoff;
                            if (vp + 4 <= data.size()) {
                                u32 cnt = loadUnaligned<u32>(data.data() + vp);
                                if (cnt >= 4 && cnt <= 4096 && vp + 4 + cnt <= data.size()) {
                                    info.constBlock.assign(data.begin() + vp + 4, data.begin() + vp + 4 + cnt);
                                    got = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!got && data.size() >= 80) {   // legacy fallback (older layouts where field 5 sits at vecPos 76)
            u32 blkSz = loadUnaligned<u32>(data.data() + 76);
            if (blkSz >= 4 && blkSz <= 1024 && (size_t)80 + blkSz <= data.size())
                info.constBlock.assign(data.begin() + 80, data.begin() + 80 + blkSz);
        }
    }

    // Parse the `constantParameters` vector: a FlatBuffer vector of {u32 nameHash, u32 blobOffset,
    // u32 byteSize} tables. We don't hardcode its field index (schema-version fragile) — instead scan
    // the root table's fields for a vector-of-tables whose entries' (blobOffset,byteSize) pairs EXACTLY
    // TILE [0, constBlock.size()). That uniquely identifies it. Each entry binds to a shader matParams
    // member by MurmurHash3("matParams."+name)==nameHash (reversed from libshell).
    {
        const u8* d = data.data();
        const size_t N = data.size();
        auto rd32 = [&](size_t o) -> u32 { return (o + 4 <= N) ? loadUnaligned<u32>(d + o) : 0; };
        auto rd16 = [&](size_t o) -> u16 { return (o + 2 <= N) ? loadUnaligned<u16>(d + o) : 0; };
        auto rdi32 = [&](size_t o) -> i32 { return (o + 4 <= N) ? loadUnaligned<i32>(d + o) : 0; };
        // table field absolute position (0 if absent)
        auto fieldPos = [&](size_t tbl, u32 fi) -> size_t {
            if (tbl + 4 > N) return 0;
            i32 soff = rdi32(tbl); long long vt = (long long)tbl - soff;
            if (vt < 0 || (size_t)vt + 4 > N) return 0;
            u16 vts = rd16((size_t)vt); size_t slot = (size_t)vt + 4 + fi * 2;
            if (slot + 2 > (size_t)vt + vts || slot + 2 > N) return 0;
            u16 fo = rd16(slot); return fo ? tbl + fo : 0;
        };
        auto u32field = [&](size_t tbl, u32 fi) -> u32 { size_t p = fieldPos(tbl, fi); return p ? rd32(p) : 0; };
        auto tblNFields = [&](size_t tbl) -> u32 {
            if (tbl + 4 > N) return 0; i32 soff = rdi32(tbl); long long vt = (long long)tbl - soff;
            if (vt < 0 || (size_t)vt + 4 > N) return 0; u16 vts = rd16((size_t)vt);
            return vts >= 4 ? (vts - 4) / 2 : 0;
        };

        size_t root = rd32(0);
        size_t blobSz = info.constBlock.size();
        if (root + 4 <= N) {
            i32 rsoff = rdi32(root); long long rvt = (long long)root - rsoff;
            if (rvt >= 0 && (size_t)rvt + 4 <= N) {
                u16 rvts = rd16((size_t)rvt); u32 nfields = rvts >= 4 ? (rvts - 4) / 2 : 0;
                // ── textureValues (root FIELD INDEX 8 / vtidx10): {AssetRef tex @ entry f0, u32 samplerNameHash @ entry f1}
                {
                    size_t tvp = fieldPos(root, 8);
                    if (tvp) {
                        u32 uoff = rd32(tvp); size_t vecPos = (size_t)tvp + uoff;
                        if (vecPos + 4 <= N) {
                            u32 cnt = rd32(vecPos);
                            if (cnt && cnt <= 256) {
                                size_t b2 = vecPos + 4;
                                for (u32 i = 0; i < cnt; ++i) {
                                    size_t slot = b2 + (size_t)i * 4; u32 eo = rd32(slot); if (!eo) continue;
                                    size_t tbl = slot + eo;
                                    size_t arPos = fieldPos(tbl, 0);   // entry f0 = AssetRef {pkg@0,ing@8,tgt@16}
                                    size_t shPos = fieldPos(tbl, 1);   // entry f1 = samplerNameHash (u32)
                                    if (!arPos || arPos + 16 > N) continue;
                                    u64 ing = loadUnaligned<u64>(d + arPos + 8);
                                    u32 hash = shPos ? rd32(shPos) : 0;
                                    if (ing >= 0x100000000ULL) info.samplerTex.push_back({hash, ing});
                                }
                            }
                        }
                    }
                }
                // ── constantValues (root FIELD INDEX 3 / vtidx5): vector of {nameHash@f0, blobOffset@f1,
                //    byteSize@f2}. DETERMINISTIC read — the heuristic "find a vector that tiles the blob"
                //    (below) MISSES embedded USD materials (the static terrain) -> their matParams never
                //    bound -> rgbmasked LayerRed/LayerBlue/Tint stayed at the 1.0 default = WHITE scene.
                if (blobSz && info.constParams.empty()) {
                    size_t cvp = fieldPos(root, 3);
                    if (cvp) {
                        u32 uoff = rd32(cvp); size_t vecPos = (size_t)cvp + uoff;
                        if (vecPos + 4 <= N) {
                            u32 cnt = rd32(vecPos);
                            if (cnt && cnt <= 256) {
                                size_t cb = vecPos + 4; std::vector<ConstParam> cps; bool ok = true;
                                for (u32 i = 0; i < cnt && ok; ++i) {
                                    size_t slot = cb + (size_t)i * 4; u32 eo = rd32(slot); if (!eo) { ok = false; break; }
                                    size_t tbl = slot + eo;
                                    u32 h = u32field(tbl, 0), off = u32field(tbl, 1), sz = u32field(tbl, 2);
                                    if (!(sz == 4 || sz == 8 || sz == 12 || sz == 16) || (size_t)off + sz > blobSz) { ok = false; break; }
                                    cps.push_back({h, off, sz});
                                }
                                if (ok && !cps.empty()) info.constParams = std::move(cps);
                            }
                        }
                    }
                }
                for (u32 fi = 0; fi < nfields; ++fi) {
                    size_t fpos = fieldPos(root, fi);
                    if (!fpos) continue;
                    u32 uoff = rd32(fpos); if (!uoff) continue;
                    size_t vecPos = fpos + uoff; if (vecPos + 4 > N) continue;
                    u32 cnt = rd32(vecPos); if (cnt == 0 || cnt > 256) continue;
                    size_t base = vecPos + 4;
                    if (base + (size_t)cnt * 4 > N) continue;
                    // ── constantParameters: {nameHash, blobOffset, byteSize} entries that TILE the blob.
                    if (blobSz && info.constParams.empty()) {
                        std::vector<ConstParam> cps; cps.reserve(cnt);
                        bool tiles = true; size_t covered = 0;
                        for (u32 i = 0; i < cnt && tiles; ++i) {
                            size_t slot = base + (size_t)i * 4; u32 eo = rd32(slot);
                            if (!eo) { tiles = false; break; }
                            size_t tbl = slot + eo;
                            u32 h = u32field(tbl, 0), off = u32field(tbl, 1), sz = u32field(tbl, 2);
                            // Allow scalar(4)/vec2(8)/vec3(12)/vec4(16). The VAT bubble material has vec2
                            // params (size 8) — rejecting them dropped the WHOLE constantParameters set, so
                            // atlasSubDivX/Y & vat_AnimTracksCount fell back to 0 and the bubbles went spiky.
                            if (!(sz == 4 || sz == 8 || sz == 12 || sz == 16) || (size_t)off + sz > blobSz) { tiles = false; break; }
                            cps.push_back({h, off, sz}); covered += sz;
                        }
                        if (tiles && covered == blobSz) { info.constParams = std::move(cps); continue; }
                    }
                    // ── textureParameters (and any other named vector): collect every entry u32 as a
                    //    candidate sampler-slot hash (only real sampler-name hashes will match a shader).
                    for (u32 i = 0; i < cnt; ++i) {
                        size_t slot = base + (size_t)i * 4; u32 eo = rd32(slot); if (!eo) continue;
                        size_t tbl = slot + eo; u32 nf = tblNFields(tbl); if (nf == 0 || nf > 8) continue;
                        for (u32 f = 0; f < nf; ++f) { u32 v = u32field(tbl, f); if (v >= 0x10000u) info.texSlots.push_back(v); }
                    }
                }
            }
        }
    }

    // Scan for texture ings by searching for tgt sentinel 0x6E4CC522 (all texture assets).
    // Layout: [8B ing][4B tgt=0x6E4CC522]. For pbrlightmap materials the LAST pair is basecolor.
    // v79_test (176B): single texture at ing@128, tgt@136 — correctly matched by this scan.
    // haven2025 (288B+): multiple textures; last is basecolor.
    // Collect ALL texture ings (a material may reference base-color + normal + AO +
    // lightmap). We keep them in file order; the loader picks the BASE-COLOR one by
    // resolving each ing's path and preferring "_basecolor"/"_basecolormetallic" over
    // normal("_onxrny")/AO("_rbaodir")/etc. Picking the wrong one rendered purple.
    static constexpr u32 TEX_TGT = 0x6E4CC522u;
    if (!info.samplerTex.empty()) {
        // FAITHFUL path: textureValues gave us the exact texture ings (in sampler order). Use them so the
        // loader marks them "referenced" (decoded) + binds the right one per sampler. (Fixes USD-derived
        // materials whose real diffuse otherwise fell into the unreferenced bucket -> white meshes.)
        for (auto& st : info.samplerTex) info.texIngs.push_back(st.second);
        info.texIng = info.texIngs.back();
        info.texTgt = TEX_TGT;
        info.texPkg = 0;
    } else {
        for (u32 i = 72; i + 12 <= (u32)data.size(); i += 4) {
            u32 val = loadUnaligned<u32>(data.data() + i);
            if (val == TEX_TGT && i >= 8) {
                u64 ing = loadUnaligned<u64>(data.data() + i - 8);
                if (ing >= 0x100000000ULL) {
                    info.texIngs.push_back(ing);
                    info.texIng = ing;       // default = last (overridden by loader)
                    info.texTgt = TEX_TGT;
                    info.texPkg = 0;
                }
            }
        }
    }

    return true;
}
