#pragma once
// Mip chains for textures that ship without one. Several Meta homes (Desert, Cascadia) carry 2048-8192 ASTC textures
// with only their full-size level: drawn as they are they shimmer at a distance, so the renderer decodes them at load,
// shrinks them to 2048 and builds the smaller levels in RGBA: blurrier than the original, slow to load, heavy in memory.
// Baking a .fenv gives them a real chain instead: the full-size level stays exactly as shipped, and each smaller one is
// box-filtered from the level above it and encoded to ASTC in the same block size. The GPU then samples them natively,
// full sharpness up close and no shimmer far away.
#include "core/types.h"
#include "loaders/rendtxtr_parser.h"
#include "astcenc.h"

#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

namespace texmips {

// Worth a chain: an LDR 2D texture bigger than 256 with no smaller levels.
inline bool wants(const TexSource& t) {
    return !t.hdr && t.faces == 1 && t.mips <= 1 && std::max(t.w, t.h) > 256 && astcDimsOk(t.w, t.h, t.bw, t.bh) &&
           t.astc.size() >= astcMip0Bytes(t.w, t.h, t.bw, t.bh);
}

// `t` with its full chain, or null when it can't be made (the texture is then kept as it was). threads: encoder
// threads (0 = every core).
inline std::shared_ptr<TexSource> withMips(const TexSource& t, unsigned threads = 0) {
    if (!wants(t)) return nullptr;
    std::vector<u8> level;
    const u32 mip0 = astcMip0Bytes(t.w, t.h, t.bw, t.bh);
    if (!astc::decodeASTC(t.astc.data(), mip0, t.w, t.h, t.bw, t.bh, level, false, 0)) return nullptr;
    astcenc_config cfg = {};
    if (astcenc_config_init(ASTCENC_PRF_LDR, t.bw, t.bh, 1, ASTCENC_PRE_FAST, 0, &cfg) != ASTCENC_SUCCESS) return nullptr;
    unsigned n = threads ? threads : std::thread::hardware_concurrency();
    n = std::clamp(n, 1u, 16u);
    astcenc_context* ctx = nullptr;
    if (astcenc_context_alloc(&cfg, n, &ctx, nullptr) != ASTCENC_SUCCESS) return nullptr;
    auto out = std::make_shared<TexSource>(t);
    out->astc.assign(t.astc.begin(), t.astc.begin() + mip0);   // the full-size level, exactly as shipped
    const u32 levels = astcFullMipCount(t.w, t.h);
    u32 w = t.w, h = t.h;
    bool ok = true;
    const astcenc_swizzle swz = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    for (u32 l = 1; l < levels && ok; ++l) {
        // 2x2 box filter (an odd edge folds its last row / column in)
        const u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        std::vector<u8> next(static_cast<size_t>(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x) {
                const u32 x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1), y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
                for (int c = 0; c < 4; ++c) {
                    const unsigned s = level[(static_cast<size_t>(y0) * w + x0) * 4 + c] + level[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                       level[(static_cast<size_t>(y1) * w + x0) * 4 + c] + level[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<u8>((s + 2) / 4);
                }
            }
        level.swap(next);
        w = nw;
        h = nh;
        const size_t bytes = static_cast<size_t>((w + t.bw - 1) / t.bw) * ((h + t.bh - 1) / t.bh) * 16;
        const size_t at = out->astc.size();
        out->astc.resize(at + bytes);
        void* px = level.data();
        astcenc_image img = {};
        img.dim_x = w; img.dim_y = h; img.dim_z = 1;
        img.data_type = ASTCENC_TYPE_U8;
        img.data = &px;
        std::vector<astcenc_error> e(n, ASTCENC_SUCCESS);
        if (n > 1 && static_cast<size_t>(w) * h >= 256 * 256) {   // big levels on every core, the small ones on one
            std::vector<std::thread> th;
            for (unsigned i = 0; i < n; ++i) th.emplace_back([&, i] { e[i] = astcenc_compress_image(ctx, &img, &swz, out->astc.data() + at, bytes, i); });
            for (auto& x : th) x.join();
        } else {
            // A context made for n threads waits for all n in compress: one thread uses a context of its own.
            astcenc_context* one = nullptr;
            if (astcenc_context_alloc(&cfg, 1, &one, nullptr) != ASTCENC_SUCCESS) { ok = false; break; }
            e[0] = astcenc_compress_image(one, &img, &swz, out->astc.data() + at, bytes, 0);
            astcenc_context_free(one);
        }
        for (astcenc_error x : e) ok = ok && x == ASTCENC_SUCCESS;
        astcenc_compress_reset(ctx);
    }
    astcenc_context_free(ctx);
    if (!ok) return nullptr;
    out->mips = levels;
    return out;
}

}  // namespace texmips
