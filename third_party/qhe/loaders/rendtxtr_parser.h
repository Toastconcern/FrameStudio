#pragma once
#include "core/types.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include "astcenc.h"

// RENDTXTR header parser — 1:1 replica of
// arvr/projects/mhe/modules/renderer/code/source/asset/RenderTextureAsset.cpp
// (sub_D3AECC in libshell_current)
//
// 64-byte FlatBuffer header + raw ASTC mip data

struct RendtxtrInfo {
    u32 width = 0;
    u32 height = 0;
    u32 mipCount = 0;
    u8  formatCode = 0;
    u32 rawDataOffset = 0;   // byte offset of the ASTC payload within the file
    u32 rawDataLen = 0;      // exact ASTC payload length (from the FlatBuffer vector)
    // True ASTC block footprint — DERIVED from the data length, NOT from formatCode.
    // The block size that makes the full mip-chain byte-count equal the actual payload
    // length is the correct one (works for any env: nuxd, haven2025, ...).
    u32 blockW = 8;
    u32 blockH = 8;
    // Cubemaps (skybox panoramas, IBL reflection maps): f7 = 6 faces, f6 = mips; the payload is
    // MIP-MAJOR (all six faces of mip0, then all six of mip1, ...). faces = 1 for plain 2D textures.
    u32 faces = 1;
    // Uncompressed payload (header f2 = 4: RGBA8, 5: RGBA16F): bytes per texel, else 0 (ASTC). rawDataLen is then
    // mip 0 alone (w*h*rawBpp), which every decoder takes as raw texels.
    u32 rawBpp = 0;
};

// ── FlatBuffer vtable helpers (RENDTXTR header layout differs per env, so resolve
//    fields by INDEX via the vtable instead of hardcoding byte offsets). Field indices
//    are stable across envs: f3=width, f4=height, f6=format, f9=ASTC data vector.
inline u32 fbFieldSlot(const u8* d, size_t n, u32 root, u32 fi) {
    if ((size_t)root + 4 > n) return 0;
    i32 soff = loadUnaligned<i32>(d + root);
    // vtable lives at root - soff; soff may be negative (vtable after table).
    int64_t vt64 = (int64_t)root - (int64_t)soff;
    if (vt64 < 0 || (uint64_t)vt64 + 4 > n) return 0;
    u32 vt = (u32)vt64;
    u16 vtSize = loadUnaligned<u16>(d + vt);
    if ((size_t)vt + vtSize > n) return 0;
    uint64_t bo = (uint64_t)vt + 4 + (uint64_t)fi * 2;
    if (bo + 2 > (uint64_t)vt + vtSize) return 0;
    u16 fo = loadUnaligned<u16>(d + bo);
    if (fo == 0) return 0;
    uint64_t slot = (uint64_t)root + fo;
    if (slot + 2 > n) return 0;     // ensure at least 2 readable bytes at the slot
    return (u32)slot;
}

// All ASTC block footprints the hardware supports, in descending bytes-per-texel.
static constexpr u32 kAstcBlocks[][2] = {
    {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
    {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
};

// Largest texture side accepted from a file: keeps w*h*16 (RGBA32F) and every block count well inside size_t/u32
// math, and rejects hostile headers before they size multi-GB buffers.
static constexpr u32 kMaxTexDim = 16384;

inline bool astcFootprintOk(u32 bw, u32 bh) {
    for (auto& b : kAstcBlocks) if (b[0] == bw && b[1] == bh) return true;
    return false;
}

// Dimensions + footprint every decoder needs before doing size math (callers outside the parser pass these too).
inline bool astcDimsOk(u32 w, u32 h, u32 bw, u32 bh) {
    return w && h && w <= kMaxTexDim && h <= kMaxTexDim && astcFootprintOk(bw, bh);
}

// Total bytes of a full ASTC mip chain (mip0 down to 1x1) for a given block size.
inline u32 astcMipChainBytes(u32 w, u32 h, u32 bw, u32 bh) {
    u64 total = 0;
    u32 mw = w, mh = h;
    while (true) {
        u64 cols = (mw + bw - 1) / bw;
        u64 rows = (mh + bh - 1) / bh;
        total += cols * rows * 16;
        if (mw == 1 && mh == 1) break;
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
    }
    return (u32)total;
}

inline u32 astcMip0Bytes(u32 w, u32 h, u32 bw, u32 bh) {
    if (!bw || !bh) return 0;
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
}

inline u32 astcFullMipCount(u32 w, u32 h) {
    if (!w || !h) return 0;
    u32 count = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        ++count;
    }
    return count;
}

// Exact byte count for a reported prefix of an ASTC mip chain. Cooked Homes
// commonly retain only part of the full chain, so checking only "full" or
// "mip0" loses otherwise byte-perfect source payloads on the next cook.
inline u32 astcMipBytesForCount(u32 w, u32 h, u32 bw, u32 bh, u32 mipCount) {
    if (!w || !h || !bw || !bh || !mipCount) return 0;
    u64 total = 0;
    for (u32 level = 0; level < mipCount; ++level) {
        const u64 cols = (w + bw - 1) / bw;
        const u64 rows = (h + bh - 1) / bh;
        total += cols * rows * 16;
        if (total > 0xFFFFFFFFull) return 0;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return (u32)total;
}

inline bool isExactAstcPayload(const RendtxtrInfo& info) {
    if (!info.blockW || !info.blockH || !info.mipCount || !info.rawDataLen) return false;
    return astcMipBytesForCount(info.width, info.height, info.blockW, info.blockH,
                                info.mipCount) == info.rawDataLen;
}

inline bool parseRendtxtrHeader(const std::vector<u8>& data, RendtxtrInfo& info);

// An HDR map (a lightmap, a reflection panorama) kept as its cooked ASTC chain, for deferred loading: the whole chain
// when the payload is exactly one, else mip 0. Null when the payload isn't ASTC (uncompressed RGBA16F / RGBA8).
inline std::shared_ptr<TexSource> hdrAstcSource(const std::vector<u8>& raw, const RendtxtrInfo& ti) {
    if (!ti.width || !ti.height || !ti.blockW || !ti.blockH) return nullptr;
    const size_t texels = static_cast<size_t>(ti.width) * ti.height;
    if (ti.rawDataLen == texels * 8 || ti.rawDataLen == texels * 4) return nullptr;
    const size_t mip0 = static_cast<size_t>((ti.width + ti.blockW - 1) / ti.blockW) * ((ti.height + ti.blockH - 1) / ti.blockH) * 16;
    const bool exact = ti.faces == 1 && isExactAstcPayload(ti);
    const size_t len = exact ? ti.rawDataLen : mip0;
    if (ti.rawDataLen < mip0 || static_cast<size_t>(ti.rawDataOffset) + len > raw.size()) return nullptr;
    auto s = std::make_shared<TexSource>();
    s->astc.assign(raw.begin() + ti.rawDataOffset, raw.begin() + ti.rawDataOffset + len);
    s->w = ti.width; s->h = ti.height; s->bw = ti.blockW; s->bh = ti.blockH; s->mips = exact ? ti.mipCount : 1; s->hdr = true;
    return s;
}

namespace astc {
inline bool decodeASTC(const u8* rawMips, u32 rawLen, u32 width, u32 height, u32 bw, u32 bh, std::vector<u8>& outRGBA, bool hdr, unsigned threads);
}

// Several block footprints can tile a small texture's payload exactly (8x8 with 4 mips is 112 bytes as 4x4, 5x5 or 6x6
// blocks) and the header doesn't say which. A wrong footprint makes every block an ASTC error block (magenta: the
// Calming vista's pink hero bird, cooked 6x6). Of the candidates, the first whose mip 0 decodes without error texels.
inline bool astcFootprintDecodes(const u8* raw, u32 len, u32 w, u32 h, u32 bw, u32 bh) {
    std::vector<u8> px;
    if (!astc::decodeASTC(raw, len, w, h, bw, bh, px, false, 1)) return false;
    for (size_t q = 0; q + 3 < px.size(); q += 4)
        if (px[q] == 255 && px[q + 1] == 0 && px[q + 2] == 255) return false;
    return true;
}

inline bool parseRendtxtrHeader(const std::vector<u8>& data, RendtxtrInfo& info) {
    if (data.size() < 16) return false;
    if (memcmp(data.data() + 4, "TXTR", 4) != 0) return false;

    const u8* d = data.data();
    size_t n = data.size();
    u32 root = loadUnaligned<u32>(d);
    if ((size_t)root + 4 > n) return false;

    // Resolve fields by vtable index (stable across envs):
    //   f3 = width, f4 = height, f6 = format, f9 = ASTC data (ubyte vector).
    u32 sW = fbFieldSlot(d, n, root, 3);
    u32 sH = fbFieldSlot(d, n, root, 4);
    u32 sF = fbFieldSlot(d, n, root, 6);
    u32 sMip = fbFieldSlot(d, n, root, 7);
    u32 sData = fbFieldSlot(d, n, root, 9);

    info.width      = sW ? loadUnaligned<u16>(d + sW) : 0;
    info.height     = sH ? loadUnaligned<u16>(d + sH) : 0;
    const u32 field6 = sF ? d[sF] : 0;
    const u32 field7 = sMip ? loadUnaligned<u16>(d + sMip) : 0;
    info.formatCode = (u8)field6;
    info.mipCount = 1;
    if (info.width == 0 || info.height == 0 || info.width > kMaxTexDim || info.height > kMaxTexDim) return false;

    // f9 is a FlatBuffer vector: indirect uoffset -> [u32 count][bytes...].
    if (sData && (size_t)sData + 4 <= n) {
        u32 uoff = loadUnaligned<u32>(d + sData);
        uint64_t vecPos = (uint64_t)sData + uoff;
        if (vecPos + 4 <= n) {
            info.rawDataLen = loadUnaligned<u32>(d + vecPos);
            info.rawDataOffset = (u32)(vecPos + 4);
            if ((uint64_t)info.rawDataOffset + info.rawDataLen > n)
                info.rawDataLen = (u32)(n - info.rawDataOffset);
        }
    }
    // Fallback to legacy fixed 64-byte header if the vector wasn't found.
    if (info.rawDataLen == 0) {
        if (n <= 64) return false;          // no payload past the legacy header (n - 64 would wrap)
        info.rawDataOffset = 64;
        info.rawDataLen = (u32)(n - 64);
    }

    // f2 is the pixel format: 4 = RGBA8, 5 = RGBA16F, 15.. = ASTC. Uncompressed textures ship WITH their mip chain
    // (Focused's rocky2 / homefg / midgroundb_02 basecolours: 2048^2 RGBA8 + mips = 22369620 bytes; Horror's
    // t_lantern_unlit_baked_01 / t_vat_basecolor: RGBA16F + mips). No ASTC footprint tiles that, so the formatCode
    // guess below decoded raw texels as ASTC: diagonal streaks and dots over the rocks, black lanterns. A payload
    // that is exactly such a chain (any length of it) is cut to mip 0, which the decoders read as raw texels.
    if (const u32 sFmt = fbFieldSlot(d, n, root, 2)) {
        const u32 bpp = d[sFmt] == 4 ? 4 : d[sFmt] == 5 ? 8 : 0;
        u64 chain = 0;
        for (u32 w = info.width, h = info.height; bpp && chain < info.rawDataLen;) {
            chain += (u64)w * h * bpp;
            if (w == 1 && h == 1) break;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        if (bpp && chain == info.rawDataLen) {
            info.rawBpp = bpp;
            info.rawDataLen = info.width * info.height * bpp;
            info.blockW = info.blockH = 4;     // (unused for raw texels; keeps astcDimsOk's footprint check true)
            info.mipCount = 1;
            return (size_t)info.rawDataOffset + info.rawDataLen <= n;
        }
    }

    u32 rawLen = info.rawDataLen;

    // Two RENDTXTR layouts exist in the wild. Older readers treated f6 as a
    // format and f7 as mipCount; the current Meta-compatible writer stores the
    // mip count in f6 and a constant in f7. Resolve this from the payload rather
    // than trusting either field blindly.
    info.blockW = 0;
    const u32 fullMipCount = astcFullMipCount(info.width, info.height);
    auto tryMipCount = [&](u32 candidate) {
        if (info.blockW || candidate < 1 || candidate > fullMipCount) return;
        std::vector<int> fits;
        for (int k = 0; k < static_cast<int>(sizeof kAstcBlocks / sizeof kAstcBlocks[0]); ++k)
            if (astcMipBytesForCount(info.width, info.height, kAstcBlocks[k][0], kAstcBlocks[k][1], candidate) == rawLen) fits.push_back(k);
        const int first = fits.empty() ? -1 : fits[0];
        int good = -1;
        if (fits.size() > 1 && rawLen <= (256u << 10) && (u64)info.rawDataOffset + rawLen <= n)   // ambiguous: try them
            for (int k : fits)
                if (astcFootprintDecodes(d + info.rawDataOffset, rawLen, info.width, info.height, kAstcBlocks[k][0], kAstcBlocks[k][1])) { good = k; break; }
        const int pick = good >= 0 ? good : first;
        if (good >= 0 && good != first && std::getenv("FH_FOOTPRINT_LOG"))
            std::fprintf(stderr, "[footprint] %ux%u, %u mips, %u bytes: %ux%u blocks, not %ux%u\n", info.width, info.height, candidate, rawLen,
                         kAstcBlocks[good][0], kAstcBlocks[good][1], kAstcBlocks[first][0], kAstcBlocks[first][1]);
        if (pick >= 0) { info.blockW = kAstcBlocks[pick][0]; info.blockH = kAstcBlocks[pick][1]; info.mipCount = candidate; }
    };
    // Cubemap: six faces x the f6-level chain must tile the payload exactly.
    if (field7 == 6 && field6 >= 1 && field6 <= fullMipCount) {
        for (auto& b : kAstcBlocks) {
            if ((u64)astcMipBytesForCount(info.width, info.height, b[0], b[1], field6) * 6 == rawLen) {
                info.blockW = b[0]; info.blockH = b[1]; info.mipCount = field6; info.faces = 6; break;
            }
        }
    }
    tryMipCount(field7);       // legacy layout
    tryMipCount(field6);       // current writer/layout
    // Older assets sometimes omit or misreport f7; retain the full-chain probe.
    if (info.blockW == 0) for (auto& b : kAstcBlocks) {
        if (astcMipChainBytes(info.width, info.height, b[0], b[1]) == rawLen) {
            info.blockW = b[0]; info.blockH = b[1]; info.mipCount = fullMipCount; break;
        }
    }
    // Fallback: maybe only mip0 is present (no chain).
    if (info.blockW == 0) {
        for (auto& b : kAstcBlocks) {
            if (astcMip0Bytes(info.width, info.height, b[0], b[1]) == rawLen) {
                info.blockW = b[0]; info.blockH = b[1]; info.mipCount = 1; break;
            }
        }
    }
    // Last resort: legacy formatCode guess so we still attempt a decode.
    if (info.blockW == 0) {
        astcBlockSize(info.formatCode, info.blockW, info.blockH);
    }
    // Invariant for every caller: payload in bounds, sane dims, real footprint.
    return (size_t)info.rawDataOffset + info.rawDataLen <= n && astcDimsOk(info.width, info.height, info.blockW, info.blockH);
}

// ───────────────────────────────────────────────────────
// ASTC decode via ARM astcenc library (exact hardware match)
// ───────────────────────────────────────────────────────

namespace astc {

// IEEE half -> float (local copy so the parser has no dependency on ibl.h).
inline float astcHalf2Float(uint16_t h) {
    uint32_t s=(h&0x8000u)<<16, e=(h>>10)&0x1F, m=h&0x3FF, bits;
    if (e==0){ if(m==0) bits=s; else { e=127-15+1; while(!(m&0x400)){m<<=1;--e;} m&=0x3FF; bits=s|(e<<23)|(m<<13);} }
    else if (e==31) bits=s|0x7F800000u|(m<<13);
    else bits=s|((e+(127-15))<<23)|(m<<13);
    float f; std::memcpy(&f,&bits,4); return f;
}

// A decode context per (profile, footprint, thread count), kept by each thread: making one builds astcenc's block
// tables, which cost more than decoding a small texture (a fifth of a big home's load went to it).
inline astcenc_context* decodeContext(bool hdr, u32 bw, u32 bh, unsigned threads) {
    struct Kept {
        std::vector<std::pair<uint64_t, astcenc_context*>> c;
        ~Kept() { for (auto& kv : c) astcenc_context_free(kv.second); }
    };
    thread_local Kept kept;
    const uint64_t key = (uint64_t)hdr << 48 | (uint64_t)bw << 32 | (uint64_t)bh << 16 | threads;
    for (auto& kv : kept.c)
        if (kv.first == key) { astcenc_decompress_reset(kv.second); return kv.second; }
    astcenc_config cfg = {};
    if (astcenc_config_init(hdr ? ASTCENC_PRF_HDR : ASTCENC_PRF_LDR, bw, bh, 1, ASTCENC_PRE_MEDIUM, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) != ASTCENC_SUCCESS)
        return nullptr;
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, threads, &ctx, nullptr) != ASTCENC_SUCCESS) return nullptr;
    kept.c.push_back({key, ctx});
    return ctx;
}

// Threads to decode a w x h image with: one for small ones (starting threads costs more than they save).
inline unsigned decodeThreads(u32 width, u32 height, u32 bw, u32 bh, unsigned threads) {
    if (threads) return threads;
    const u64 blocks = (u64)((width + bw - 1) / bw) * ((height + bh - 1) / bh);
    if (blocks < 16384) return 1;
    unsigned N = std::thread::hardware_concurrency(); if (N < 1) N = 1; if (N > 16) N = 16;
    if (const char* e = std::getenv("HSR_ASTC_THREADS")) { int v = atoi(e); if (v >= 1) N = (unsigned)v; }
    return N;
}

inline bool decodeASTC(const u8* rawMips, u32 rawLen, u32 width, u32 height,
                       u32 bw, u32 bh, std::vector<u8>& outRGBA, bool hdr = false, unsigned threads = 0) {
    // ⛔ UNCOMPRESSED RENDTXTR payloads (NOT ASTC): some maps are cooked raw — esp. tiny VAT placeholders like
    // horror's `t_vat_blackblack` (1x1, formatCode 1, 8 bytes = one RGBA16F texel). astcenc rejects them
    // (8 < a 16-byte ASTC block) -> "ASTC decode failed" + the texture was SKIPPED. Detect by exact byte-count
    // (rawLen == w*h*8 -> RGBA16F half-float; == w*h*4 -> RGBA8) and decode directly. Generalizes to any env.
    if (!rawMips || !astcDimsOk(width, height, bw, bh)) return false;
    if (rawLen == (u64)width * height * 8) {
        outRGBA.resize((size_t)width * height * 4);
        for (size_t i = 0; i < (size_t)width * height; ++i)
            for (int c = 0; c < 4; ++c) { uint16_t h; std::memcpy(&h, rawMips + i*8 + c*2, 2);
                float f = astcHalf2Float(h); int v = (int)(std::max(0.f, std::min(1.f, f)) * 255.f + 0.5f); outRGBA[i*4+c] = (u8)v; }
        return true;
    }
    if (rawLen == (u64)width * height * 4) { outRGBA.assign(rawMips, rawMips + (size_t)width*height*4); return true; }
    // Clamp to exactly the first mip's block data — extra mip tails make astcenc reject.
    u32 cols = (width  + bw - 1) / bw;
    u32 rows = (height + bh - 1) / bh;
    u32 mip0Size = cols * rows * 16;
    if (rawLen > mip0Size) rawLen = mip0Size;
    if (rawLen < mip0Size) return false;

    // HDR-encoded ASTC (the .hdr baked lightmaps) MUST use the HDR profile. Decoding them as LDR makes
    // astcenc emit MAGENTA error texels, which the lightmap shader multiplies into the base -> purple ground.
    // Decode ACROSS CORES: astcenc cooperatively decompresses one image from N worker threads (each gets a
    // thread_index 0..N-1 on the same context); big images only (decodeThreads). HSR_ASTC_THREADS overrides N;
    // background streaming workers decode one image each, single-threaded.
    const unsigned N = decodeThreads(width, height, bw, bh, threads);
    astcenc_context* ctx = decodeContext(hdr, bw, bh, N);
    if (!ctx) return false;
    astcenc_error err = ASTCENC_SUCCESS;

    outRGBA.resize((size_t)width * height * 4);
    const astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

    auto runMT = [&](astcenc_image* im) -> astcenc_error {
        if (N <= 1) return astcenc_decompress_image(ctx, rawMips, rawLen, im, &swz, 0);
        std::vector<std::thread> th; std::vector<astcenc_error> e(N, ASTCENC_SUCCESS);
        for (unsigned t = 0; t < N; ++t)
            th.emplace_back([&, t] { e[t] = astcenc_decompress_image(ctx, rawMips, rawLen, im, &swz, t); });
        for (auto& x : th) x.join();
        for (auto ee : e) if (ee != ASTCENC_SUCCESS) return ee;
        return ASTCENC_SUCCESS;
    };

    if (hdr) {
        // Decode HDR ASTC to float, then clamp to [0,1] -> u8 (baked lighting; >1 highlights clamp to white).
        std::vector<float> fbuf((size_t)width * height * 4);
        float* fptr = fbuf.data();
        astcenc_image fimg = {}; fimg.dim_x = width; fimg.dim_y = height; fimg.dim_z = 1;
        fimg.data_type = ASTCENC_TYPE_F32; fimg.data = reinterpret_cast<void**>(&fptr);
        err = runMT(&fimg);
        if (err != ASTCENC_SUCCESS) return false;
        // Baked HDR lightmaps are RADIANCE. The device EXPOSES + ACES-tonemaps them; a raw [0,1] clamp leaves
        // the baked lighting TOO DARK (chair/horror/candle). Apply exposure + ACES filmic to RGB so it reads
        // naturally; alpha passes through linearly. HSR_LMEXP tunes the exposure.
        float lmExp = g_lmExposure; if (const char* e = std::getenv("HSR_LMEXP")) { float ev=(float)atof(e); if (ev>0) lmExp=ev; }
        for (size_t i = 0; i < fbuf.size(); ++i) {
            float v = fbuf[i]; if (v < 0) v = 0;
            if ((i & 3) != 3) { v *= lmExp; v = (v*(2.51f*v+0.03f))/(v*(2.43f*v+0.59f)+0.14f); }
            v = v < 0 ? 0 : (v > 1 ? 1 : v);
            outRGBA[i] = (u8)(v * 255.0f + 0.5f);
        }
        return true;
    }

    u8* slicePtr = outRGBA.data();
    astcenc_image img = {};
    img.dim_x = width;
    img.dim_y = height;
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_U8;
    img.data = reinterpret_cast<void**>(&slicePtr);

    err = runMT(&img);
    return err == ASTCENC_SUCCESS;
}

// Decodes the six mip0 faces of a cubemap (mip-major payload) into one RGBA8 buffer, face after face
// (+X, -X, +Y, -Y, +Z, -Z — Vulkan cube layer order).
inline bool decodeCubeFaces(const u8* raw, u32 rawLen, u32 size, u32 bw, u32 bh, std::vector<u8>& out) {
    if (!raw || !astcDimsOk(size, size, bw, bh)) return false;
    const u32 face = ((size + bw - 1) / bw) * ((size + bh - 1) / bh) * 16;
    if (rawLen < (u64)face * 6) return false;
    out.resize((size_t)size * size * 4 * 6);
    std::vector<u8> one;
    for (u32 f = 0; f < 6; ++f) {
        if (!decodeASTC(raw + (size_t)f * face, face, size, size, bw, bh, one)) return false;
        std::memcpy(out.data() + (size_t)f * size * size * 4, one.data(), (size_t)size * size * 4);
    }
    return true;
}

// Decodes an ASTC (HDR or LDR) mip0 to linear RGBA half floats, unclamped — baked lightmaps keep their
// real radiance for the shader to expose/tonemap itself.
inline bool decodeASTCHalf(const u8* rawMips, u32 rawLen, u32 width, u32 height, u32 bw, u32 bh, std::vector<u16>& out) {
    auto toHalf = [](float f) -> u16 {
        u32 x; std::memcpy(&x, &f, 4);
        u32 sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
        if (e == 0xFF) return (u16)(sign | 0x7C00 | (m ? 0x200 : 0));
        int ex = (int)e - 127 + 15;
        if (ex >= 31) return (u16)(sign | 0x7BFF);
        if (ex <= 0) return (u16)sign;
        return (u16)(sign | (ex << 10) | (m >> 13));
    };
    if (!rawMips || !astcDimsOk(width, height, bw, bh)) return false;
    const size_t n = (size_t)width * height * 4;
    if (rawLen == n * 2) {                 // already RGBA16F
        out.resize(n);
        std::memcpy(out.data(), rawMips, n * 2);
        return true;
    }
    const u32 mip0 = ((width + bw - 1) / bw) * ((height + bh - 1) / bh) * 16;
    if (rawLen < mip0) return false;
    const unsigned N = decodeThreads(width, height, bw, bh, 0);
    astcenc_context* ctx = decodeContext(true, bw, bh, N);
    if (!ctx) return false;
    std::vector<float> f(n);
    float* fp = f.data();
    astcenc_image img = {}; img.dim_x = width; img.dim_y = height; img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_F32; img.data = reinterpret_cast<void**>(&fp);
    const astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    std::vector<std::thread> th; std::vector<astcenc_error> e(N, ASTCENC_SUCCESS);
    for (unsigned t = 0; t < N; ++t) th.emplace_back([&, t] { e[t] = astcenc_decompress_image(ctx, rawMips, mip0, &img, &swz, t); });
    for (auto& x : th) x.join();
    for (auto ee : e) if (ee != ASTCENC_SUCCESS) return false;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = toHalf(f[i] < 0 ? 0 : f[i]);
    return true;
}

} // namespace astc
