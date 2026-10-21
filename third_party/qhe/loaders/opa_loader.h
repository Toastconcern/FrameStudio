#pragma once
// V79 ".opa" loader — the OLD OFFICIAL Meta-home cooked-asset format (e.g. spacestation:
// com.oculus.environment.prod.spacestation__base.apk). Reverse-engineered FAITHFULLY from
// V79 libshell.so (Meta MHE's own core/reflection serializer — "VertexTypeSystem"/
// "dataformat"), NOT guessed. Produces the renderer's MeshData[] (positions/uvs/indices,
// per-node world transform baked into positions), the source-format side of the V79 -> new
// HSR env porter — same role as gltf_loader.h but for cooked .opa homes.
//
// Container nesting: APK -> assets/scene.zip -> cache/android/<name>.fbx.opa (geometry).
//
// .opa MODEL SCHEMA (decompiled: sub_AF0678 MeshData reader + sub_AECEF0/DC74A8/AF0384/...):
//   [OPAA hdr 48B] then payload:
//     u32 version (0x405)        string typeName ("MeshData")
//     NODES:    u32 count; each = [8B lead][string name][i32 parent][T f32x3][R quat f32x4][S f32x3]
//     MATERIALS u32 count; each = reflection obj: {[u8 tag !=0xC8][string field][value]}* [0xC8 end]
//               fields: "Id"=u64, "Path"=string(.mat.asset)
//     MESHES:   u32 count; each entry =
//               string posFmt("RigidPos")  string dataFmt("StdData")
//               u32 listCount(usually 0)
//               u32 submeshCount; each submesh = [u32 ?][u32 firstIndex][u32 indexCount]
//                     [u32 matIndex][AABB min f32x3][AABB max f32x3][u8 bool]
//                     (version>=0x407 inserts 2 extra u32 @ +16/+20)
//               AABB min f32x3 / max f32x3   (whole-mesh)
//               u32 vertCount
//               u32 posBytes;  posBytes of RigidPos  (pos f32x3, stride 12)
//               u32 stdBytes;  stdBytes of StdData   (stride 20, uv f16x4 @ off 8)
//               u32 idxCount
//               string idxType("kUnsignedShort")
//               u32 idxBytes;  idxBytes of u16 indices
//               u16 tail (version>=0x404)
//   String codec (sub_AEF1EC): u16 marker; ==0xFFFF -> [u16 len][bytes] (len==0xFFFF ->
//   [u32 len][bytes]); else marker is an interned-table index (not used by these .opa).
//   Submesh draws indices [firstIndex, firstIndex+indexCount) with material[matIndex];
//   mesh entry[i] uses node[i]'s world transform.

#include "core/types.h"
#include "core/parallel.h"   // par::forEach (skinning across cores)
#include "core/load_progress.h"       // live stage/counter for the loading splash
#include "miniz.h"
#include "loaders/zip_reader_guard.h"
#include "loaders/scene_zip.h"
#include "loaders/rendtxtr_parser.h"   // astc::decodeASTC (KTX/ASTC -> RGBA)
#include "render/ibl.h"               // SpecIbl diffuse irradiance cubemap (RGBA16F KTX)
#include "cook/node_rot_fit.h"      // shared spin/sway fitter (V79->V203 cook) — same core the glTF loader uses
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>

class OpaLoader {
public:
    std::vector<MeshData> meshes;
    bool verbose = true;

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list a; va_start(a, fmt); fprintf(stderr, "[OPA] "); vfprintf(stderr, fmt, a);
        fprintf(stderr, "\n"); va_end(a);
    }

    // IEEE half -> float
    static float h2f(uint16_t h) {
        uint32_t s = (h & 0x8000u) << 16;
        uint32_t e = (h >> 10) & 0x1F;
        uint32_t m = h & 0x3FF;
        uint32_t bits;
        if (e == 0) {
            if (m == 0) { bits = s; }
            else { // subnormal
                e = 127 - 15 + 1;
                while (!(m & 0x400)) { m <<= 1; --e; }
                m &= 0x3FF;
                bits = s | (e << 23) | (m << 13);
            }
        } else if (e == 0x1F) {
            bits = s | 0x7F800000u | (m << 13);
        } else {
            bits = s | ((e - 15 + 127) << 23) | (m << 13);
        }
        float f; memcpy(&f, &bits, 4); return f;
    }

    // ── sequential cursor over the payload (libshell sub_AEF07C semantics) ──
    struct Cur {
        const uint8_t* d = nullptr; size_t n = 0; size_t p = 0; bool ok = true;
        bool avail(size_t k) const { return p + k <= n; }
        uint8_t  u8()  { if (!avail(1)) { ok = false; return 0; } return d[p++]; }
        uint16_t u16() { if (!avail(2)) { ok = false; return 0; } uint16_t v; memcpy(&v, d+p, 2); p += 2; return v; }
        uint32_t u32() { if (!avail(4)) { ok = false; return 0; } uint32_t v; memcpy(&v, d+p, 4); p += 4; return v; }
        int32_t  i32() { return (int32_t)u32(); }
        uint64_t u64() { if (!avail(8)) { ok = false; return 0; } uint64_t v; memcpy(&v, d+p, 8); p += 8; return v; }
        float    f32() { if (!avail(4)) { ok = false; return 0; } float v; memcpy(&v, d+p, 4); p += 4; return v; }
        void     skip(size_t k) { if (!avail(k)) { ok = false; p = n; } else p += k; }
        const uint8_t* at(size_t off) const { return d + off; }
        // sub_AEF1EC string record
        std::string str() {
            uint16_t m = u16();
            if (m != 0xFFFF) { ok = false; return {}; } // interned index — not present in these .opa
            uint32_t len = u16();
            if (len == 0xFFFF) len = u32();
            if (!avail(len)) { ok = false; return {}; }
            std::string s((const char*)d + p, len);
            p += len; return s;
        }
    };

    // ── minimal 4x4 (column-major) for baking node world transforms ──
    struct Mat4 { float m[16]; };
    static Mat4 identity() { Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1.0f; return r; }
    static Mat4 mul(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k*4+row] * b.m[c*4+k];
                r.m[c*4+row] = s;
            }
        return r;
    }
    // column-major 4x4 helpers on raw float[16] (for skeletal skinning matrices)
    static void mat4mul(const float* A, const float* B, float* C) {
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) { float s=0; for(int k=0;k<4;++k) s+=A[k*4+r]*B[c*4+k]; C[c*4+r]=s; }
    }
    static void mat4affineInverse(const float* M, float* out) {
        float a0=M[0],a1=M[1],a2=M[2], a3=M[4],a4=M[5],a5=M[6], a6=M[8],a7=M[9],a8=M[10]; // 3x3 col-major
        float det = a0*(a4*a8-a5*a7) - a3*(a1*a8-a2*a7) + a6*(a1*a5-a2*a4);
        if (det > -1e-12f && det < 1e-12f) { for(int i=0;i<16;++i) out[i]=(i%5==0)?1.f:0.f; return; }
        float id = 1.0f/det;
        float b0=(a4*a8-a5*a7)*id, b1=(a2*a7-a1*a8)*id, b2=(a1*a5-a2*a4)*id;     // inv col-major
        float b3=(a5*a6-a3*a8)*id, b4=(a0*a8-a2*a6)*id, b5=(a2*a3-a0*a5)*id;
        float b6=(a3*a7-a4*a6)*id, b7=(a1*a6-a0*a7)*id, b8=(a0*a4-a1*a3)*id;
        float tx=M[12],ty=M[13],tz=M[14];
        out[0]=b0; out[1]=b1; out[2]=b2; out[3]=0;
        out[4]=b3; out[5]=b4; out[6]=b5; out[7]=0;
        out[8]=b6; out[9]=b7; out[10]=b8; out[11]=0;
        out[12]=-(b0*tx+b3*ty+b6*tz); out[13]=-(b1*tx+b4*ty+b7*tz); out[14]=-(b2*tx+b5*ty+b8*tz); out[15]=1;
    }
    static Mat4 trs(const float t[3], const float q[4], const float s[3]) {
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
        Mat4 r = identity();
        r.m[0]=(1-2*(yy+zz))*s[0]; r.m[1]=(2*(xy+wz))*s[0];   r.m[2]=(2*(xz-wy))*s[0];
        r.m[4]=(2*(xy-wz))*s[1];   r.m[5]=(1-2*(xx+zz))*s[1]; r.m[6]=(2*(yz+wx))*s[1];
        r.m[8]=(2*(xz+wy))*s[2];   r.m[9]=(2*(yz-wx))*s[2];   r.m[10]=(1-2*(xx+yy))*s[2];
        r.m[12]=t[0]; r.m[13]=t[1]; r.m[14]=t[2];
        return r;
    }
    static void xform(const Mat4& M, float x, float y, float z, float out[3]) {
        out[0] = M.m[0]*x + M.m[4]*y + M.m[8]*z + M.m[12];
        out[1] = M.m[1]*x + M.m[5]*y + M.m[9]*z + M.m[13];
        out[2] = M.m[2]*x + M.m[6]*y + M.m[10]*z + M.m[14];
    }

    struct Node { std::string name; int parent=-1; float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; };
    std::vector<Node> nodes;
    std::vector<Mat4> nodeWorld;
    struct Mat { uint64_t id=0; std::string path; };
    std::vector<Mat> materials;

    // Decoded *.png.opa textures (OPAA container -> KTX/ASTC -> RGBA), keyed by their cooked
    // basename (filename minus ".png.opa", lowercased) so ANY home's naming resolves — not
    // just spacestation's "tx_" convention.
    struct Tex { std::string key; mutable std::vector<uint8_t> rgba; uint32_t w=1, h=1; bool hasAlpha=false;
        // Source ASTC mip chain (blocks only, per-mip imageSize prefixes stripped) + footprint, kept for LOSSLESS
        // pass-through: an UNMODIFIED texture cooks its ORIGINAL ASTC verbatim instead of decode→re-encode (double
        // ASTC compression at MEDIUM = "lost definition"). Empty when the source wasn't a plain KTX-ASTC.
        // The chain as one shared source: every mesh using the texture points at it (no per-mesh copies).
        std::shared_ptr<const TexSource> src;
        // With a source chain, `rgba` is only a small mip (alpha scan; the renderer decodes from src) — full-size
        // RGBA of every 4K texture, copied into every mesh using it, was GBs of RAM and paged the whole desktop.
        // full() decodes the base mip on first use (lightmaps are sampled from RGBA).
        mutable bool preview=false;
        mutable uint32_t pvW=0, pvH=0;            // rgba's size when it's a shrunk preview (else w x h)
        uint32_t pw() const { return pvW ? pvW : w; }
        uint32_t ph() const { return pvH ? pvH : h; }
        void halve() {
            const uint32_t sw = pw(), sh = ph(), nw = std::max(1u, sw / 2), nh = std::max(1u, sh / 2);
            std::vector<uint8_t> o((size_t)nw * nh * 4);
            for (uint32_t y = 0; y < nh; ++y) for (uint32_t x = 0; x < nw; ++x) for (int c = 0; c < 4; ++c) {
                const uint32_t x0 = std::min(sw - 1, x * 2), x1 = std::min(sw - 1, x * 2 + 1), y0 = std::min(sh - 1, y * 2), y1 = std::min(sh - 1, y * 2 + 1);
                o[((size_t)y * nw + x) * 4 + c] = (uint8_t)((rgba[((size_t)y0 * sw + x0) * 4 + c] + rgba[((size_t)y0 * sw + x1) * 4 + c] +
                                                              rgba[((size_t)y1 * sw + x0) * 4 + c] + rgba[((size_t)y1 * sw + x1) * 4 + c] + 2) / 4);
            }
            rgba.swap(o); pvW = nw; pvH = nh;
        }
        const std::vector<uint8_t>& full() const {
            if (preview) {
                const size_t mip0 = (size_t)((src->w + src->bw - 1) / src->bw) * ((src->h + src->bh - 1) / src->bh) * 16;
                if (!astc::decodeASTC(src->astc.data(), (uint32_t)std::min(mip0, src->astc.size()), src->w, src->h, src->bw, src->bh, rgba)) rgba.clear();
                preview = false; pvW = pvH = 0;
            }
            return rgba;
        }
    };
    std::vector<Tex> textures;

    // Faithful material metadata, read from the cooked *.mat.txt (libshell's own material
    // descriptions): the home TELLS us blend mode + diffuse texture, so we don't guess.
    // Keyed by the material's lowercased stem (its .mat.asset basename minus ".mat.*").
    struct MatProps {
        bool transparent=false, additive=false, alphaTest=false, doubleSided=false, unlit=true;
        uint64_t diffuseId=0;        // diffuse texture AssetId (-> assetIdToTexBase)
        uint64_t lightmapId=0;       // BAKED lightmap texture AssetId. The interior SHELL meshes
                                     // (helmet/gem/octo/table/floor...) bake their lighting+detail into
                                     // this; without it a no-albedo shell renders as a flat blob.
        std::string diffuseBase;     // OR diffuse texture basename from a Path field (lowercased)
        float diffuseColor[3]={1,1,1}; // 'diffuse' basecolor UNIFORM (flat color when no texture; tint when textured)
        float alpha=1.0f;            // 'alpha' UNIFORM — libshell scales fragment alpha by it. Transparent
                                     // effects (forge flicker=0.27, fog, dust) use <1 to be FAINT overlays;
                                     // ignoring it = full-opacity dark box that occludes everything behind.
        // PBR / SpecIbl uniforms (read verbatim from the cooked .mat.txt) — drive the split-sum IBL of
        // no-albedo metallic/gem shells (divingHelmet metallic=1, rubyGem metallic=0 rough=0, etc).
        float metallic=0.0f, roughness=1.0f;
        float speciblDiffScale=1.0f, speciblSpecScale=1.0f;
        float alphaCutoff=0.5f;      // 'alphatestthreshold' UNIFORM — the AUTHORED AlphaTest discard threshold
                                     // (V79 frag: `if (color.a < AlphaCutoff) discard`). The zen tree canopy
                                     // authors 0.25; hardcoding 0.5 discarded its abundant 0.25-0.5 mid-alpha
                                     // leaf detail = thin holey "blocky" canopy.
        float lightmapPower[3]={1.0f,1.0f,1.0f};  // per-channel lightmap HDR boost (the neon/glow tint)
        bool  isSpecibl=false;       // Shader: SpecIbl
        bool found=false;
    };
    std::unordered_map<std::string, MatProps> matProps;          // material stem -> props
    std::unordered_map<uint64_t, std::string> assetIdToTexBase;  // texture AssetId -> tex basename

    static std::string lc(std::string s){ for(auto&c:s)c=(char)tolower((unsigned char)c); return s; }
    // basename(path) with a trailing ".mat.asset"/".mat.opa"/".mat.txt" (or any ext run) stripped.
    static std::string matStem(const std::string& path) {
        size_t sl = path.find_last_of("/\\");
        std::string b = (sl==std::string::npos)?path:path.substr(sl+1);
        size_t m = b.find(".mat."); if (m!=std::string::npos) b=b.substr(0,m);
        return lc(b);
    }
    const Tex* texByBase(const std::string& base) const {
        std::string b=lc(base);
        for (auto& t : textures) if (t.key==b) return &t;
        return nullptr;
    }

    // KTX1 (ASTC) base mip -> RGBA (same decode the glTF loader uses).
    // If srcAstcOut != null, ALSO extract the full ASTC mip chain (blocks only, imageSize prefixes stripped) +
    // footprint (srcBw/srcBh) + mip count — for the cook's LOSSLESS pass-through of unmodified textures.
    static bool decodeKtxBaseMip(const uint8_t* ktx, size_t n, std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH,
                                 std::vector<uint8_t>* srcAstcOut = nullptr, uint32_t* srcBwOut = nullptr,
                                 uint32_t* srcBhOut = nullptr, uint32_t* srcMipsOut = nullptr,
                                 uint32_t previewSide = 0, uint32_t* pvWOut = nullptr, uint32_t* pvHOut = nullptr) {
        if (n < 64) return false;
        static const uint8_t id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        if (memcmp(ktx, id, 12) != 0) return false;
        auto u32a = [&](size_t o){ uint32_t v; memcpy(&v, ktx+o, 4); return v; };
        uint32_t glInternalFormat = u32a(28);
        uint32_t w = u32a(36), h = u32a(40);
        uint32_t mipCount = u32a(56); if (mipCount < 1) mipCount = 1;
        uint32_t bytesOfKeyValueData = u32a(60);
        size_t off = 64 + bytesOfKeyValueData;
        if (off + 4 > n) return false;
        uint32_t imageSize = u32a(off); size_t base0 = off + 4;
        if (base0 + imageSize > n) imageSize = (uint32_t)(n - base0);
        // Full ASTC footprint table (linear 0x93B0..0x93BD, sRGB 0x93D0..0x93DD, same
        // order of 14 footprints) — libshell reads the footprint from glInternalFormat,
        // so every non-square one (e.g. 8x6 = 0x93B6) must map correctly; defaulting to
        // 8x8 mis-strides the block grid and scrambles the texture.
        static const uint8_t kFootprints[14][2] = {
            {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
            {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
        };
        uint32_t bw = 8, bh = 8;
        int fidx = -1;
        if (glInternalFormat >= 0x93B0 && glInternalFormat <= 0x93BD) fidx = (int)(glInternalFormat - 0x93B0);
        else if (glInternalFormat >= 0x93D0 && glInternalFormat <= 0x93DD) fidx = (int)(glInternalFormat - 0x93D0);
        if (fidx >= 0) { bw = kFootprints[fidx][0]; bh = kFootprints[fidx][1]; }
        outW = w; outH = h;
        // Optional: walk the whole KTX mip chain [imageSize u32][blocks] per level and concatenate the BLOCKS.
        // previewSide: with a good chain, decode only the first mip that fits in previewSide (rgba is then that mip).
        if (srcAstcOut && fidx >= 0) {
            srcAstcOut->clear();
            size_t p = off; uint32_t mw = w, mh = h; bool ok = true;
            size_t pvOff = 0, pvSize = 0; uint32_t pvW = 0, pvH = 0;
            for (uint32_t lvl = 0; lvl < mipCount; ++lvl) {
                if (p + 4 > n) { ok = false; break; }
                uint32_t isz = u32a(p); p += 4;
                if (p + isz > n) { ok = false; break; }
                size_t expect = (size_t)((mw + bw - 1) / bw) * ((mh + bh - 1) / bh) * 16;
                if (isz != expect) { ok = false; break; }   // not a plain single-face ASTC chain -> bail (re-encode)
                if (previewSide && !pvW && (std::max(mw, mh) <= previewSide || lvl + 1 == mipCount)) {
                    pvOff = srcAstcOut->size(); pvSize = isz; pvW = mw; pvH = mh;
                }
                srcAstcOut->insert(srcAstcOut->end(), ktx + p, ktx + p + isz);
                p += isz; mw = mw > 1 ? mw / 2 : 1; mh = mh > 1 ? mh / 2 : 1;
            }
            if (!ok) { srcAstcOut->clear(); }
            else {
                if (srcBwOut) *srcBwOut = bw; if (srcBhOut) *srcBhOut = bh; if (srcMipsOut) *srcMipsOut = mipCount;
                if (pvW && pvOff == 0 && std::max(pvW, pvH) > previewSide * 2) {
                    // No level small enough (cooked without mips: a 6000^2 flipbook): decoding all of it for a
                    // preview took most of a home's load. ASTC blocks stand alone, so the preview is a sparse grid of
                    // the real blocks (one in k each way), decoded together: the same colours and alpha, ~1/k^2 the work.
                    const uint32_t nbx = (pvW + bw - 1) / bw, nby = (pvH + bh - 1) / bh;
                    const uint32_t want = std::max(1u, (previewSide * 2 + bw - 1) / bw);
                    const uint32_t k = std::max(1u, std::max(nbx, nby) / want);
                    const uint32_t mbx = (nbx + k - 1) / k, mby = (nby + k - 1) / k;
                    std::vector<uint8_t> mosaic((size_t)mbx * mby * 16);
                    for (uint32_t y = 0; y < mby; ++y)
                        for (uint32_t x = 0; x < mbx; ++x)
                            memcpy(&mosaic[((size_t)y * mbx + x) * 16], srcAstcOut->data() + ((size_t)(y * k) * nbx + x * k) * 16, 16);
                    if (pvWOut) *pvWOut = mbx * bw;
                    if (pvHOut) *pvHOut = mby * bh;
                    return astc::decodeASTC(mosaic.data(), (uint32_t)mosaic.size(), mbx * bw, mby * bh, bw, bh, rgba);
                }
                if (pvW) {
                    if (pvWOut) *pvWOut = pvW;
                    if (pvHOut) *pvHOut = pvH;
                    return astc::decodeASTC(srcAstcOut->data() + pvOff, (uint32_t)pvSize, pvW, pvH, bw, bh, rgba);
                }
            }
        }
        return astc::decodeASTC(ktx+base0, imageSize, w, h, bw, bh, rgba);
    }

    // ── sanim node-TRS animation (the looping ui_ring / wire motion) ──────────────────
    // sanim.opa (same OPAA reflection format) stores per-node TRS tracks: for each
    // (node, channel in {Translation,Rotation,Scale}): [u32 flag][u32 nKeys][u32 nVals]
    // [nVals f32]. nFrames = nKeys+1, comps = nVals/nFrames (3 for T/S, 4 for R quat).
    // No explicit per-key times -> dense per-frame sampling, so we loop at a fixed fps.
    struct Track { int comps = 0; int nFrames = 0; int effFrames = 0; std::vector<float> v; };
    struct NodeTracks { Track t, r, s; };
    std::unordered_map<std::string, NodeTracks> nodeAnim;
    struct AnimRec { size_t meshIdx; uint32_t nodeIdx; Mat4 parentWorld; std::vector<float> basePos;
                     // animateRuntime: rigid = drawn as md.animXform over the t=0 pose (-1 = not decided yet; 0 = its
                     // node is singular at t=0 or the mesh is also skinned / VAT, so its vertices are rewritten)
                     int rigid = -1; Mat4 rest{}; float restInv[16] = {}; bool atRest = false; };
    // animate(t) rewrites every animated vertex (the cook samplers read them); animateRuntime(t) is the per-frame
    // play: rigid node motion becomes md.animXform, so those meshes' vertices never change.
    bool gpuRigid_ = false;
    void animateRuntime(float t) { gpuRigid_ = true; animate(t); gpuRigid_ = false; }
    static bool put(float& d, float v) { if (d == v) return false; d = v; return true; }
    std::vector<AnimRec> animRecs;
    std::vector<Mat4> nodeWorldAnim;   // per-frame scratch: animated world transform of every node (reused)
    std::vector<Node> animNodes;       // GLOBAL persistent node list — every .fbx.opa appended (parents offset).
                                       // animRec.nodeIdx indexes THIS, NOT the per-mesh `nodes` that parseModel
                                       // CLEARS each call (which made the bird's wing read the waterfall's node).
    int   animMaxFrames = 0;
    float animFps = 30.0f;
    // UV-scroll loop-time cap ("Water flow" slider): boosts a slow track's rate so its loop completes in this many
    // seconds. ⛔ DEFAULT OFF (0 = faithful authored rate): the renderer's animate() CLIP path already plays every
    // UV track at its OWN authored duration (frames/fps — the "moves too fast / doesn't follow" fix), but the COOK's
    // uvScrollRate still applied this boost, making preview and device speeds disagree for long water tracks.
    // Preserve the authored near-static rate by default; opt in through the editor slider if a livelier result is wanted.
    float uvMaxLoopSec = 0.0f;
    // UV-scroll VISIBLE SPEED CAP (UV/sec). The mat.sanim has NO FrameRate field so a UV track inherits the NODE
    // sanim's 30fps (bird/butterfly) — wrong: that makes the fog scroll 3.0 tiles/s & the waterfall 3.75 (user:
    // "wrong speed", far too fast). The real playback is a runtime PlaybackState we can't read, so cap the visible
    // speed to a calm UV/sec, preserving DIRECTION + RELATIVE speed. Shared by cook + renderer (uvScrollRate).
    float uvSpeedMax = 0.5f;

    // ── Skeletal animation (*.skel/*.anim.opa) for skinned meshes ──────────────────────────
    // An .anim clip stores, per frame, one 4x4 SKINNING matrix per joint (frame0 ~= identity ->
    // they're pre-multiplied jointWorld*inverseBind). vertex_world(t) = jointMat[t][bone]*bindVertex.
    struct AnimClip { std::vector<std::string> joints; std::vector<float> mats;   // mats = per-frame LOCAL 4x4
                      std::vector<int> parents; std::vector<float> invBind;       // from .skel (parents + inverse bind WORLD)
                      int numJoints=0, numFrames=0; };
    std::vector<AnimClip> clips;
    std::unordered_map<std::string,std::pair<int,int>> jointToClip;  // jointName -> (clipIdx, jointIdx)
    // .skel.opa bind data, keyed by basename (owl_offset.skel <-> owl_offset.anim): parents + bind LOCAL 4x4.
    std::unordered_map<std::string, std::pair<std::vector<int>, std::vector<float>>> skelData;
    // Per skinned mesh: bind verts + per-vertex 4 (skin-bone, weight) for linear blend skinning,
    // plus the skin's per-bone INVERSE-BIND (from the .skin.opa) and bone->clip-joint map.
    // skinMat[bone] = clipJointWorld[boneClip[bone]] * invBind[bone].
    struct SkinRec { size_t meshIdx; int clipIdx=-1; int nJoints=0; std::vector<float> basePos; // nv*3
                     std::vector<int> jidx; std::vector<float> jw;        // nv*4 each (jidx = SKIN bone idx)
                     std::vector<int> boneClip; std::vector<float> invBind;   // nJoints, nJoints*16
                     bool gpu = false;                // skinned by the renderer (prepareGpuSkin)
                     std::vector<float> jointBox;     // gpu: per joint, the bind-space box of the vertices it moves (6 floats)
                     float unriggedBox[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f}; };   // gpu: the vertices no joint moves
    std::vector<SkinRec> skinRecs;
    std::vector<std::vector<float>> _clipSkin;   // per-frame scratch (reused -> no heap churn/stutter)
    int skinShared_ = -1;                         // two skin records write one mesh (-1: not checked yet): skin serially
    // GPU skinning (Frame Home / Studio): every skinned mesh the renderer can skin itself (<= 256 joints, one record, no
    // other animation writing its vertices) gets its bind pose (skinBindPos) and 8-bit bone indices / weights;
    // animateRuntime then writes only its joint matrices and box, never its vertices. Call before the meshes are drawn.
    void prepareGpuSkin() {
        std::map<size_t, int> per;
        for (auto& sr : skinRecs) ++per[sr.meshIdx];
        std::set<size_t> other;
        for (auto& ar : animRecs) other.insert(ar.meshIdx);
        for (auto& vr : vatRecs) other.insert(vr.meshIdx);
        size_t n = 0;
        for (auto& sr : skinRecs) {
            if (sr.meshIdx >= meshes.size()) continue;
            MeshData& md = meshes[sr.meshIdx];
            const size_t nv = sr.basePos.size() / 3;
            if (sr.nJoints < 1 || sr.nJoints > 256 || per[sr.meshIdx] != 1 || other.count(sr.meshIdx) || sr.jidx.size() < nv*4 ||
                sr.jw.size() < nv*4 || md.positions.size() != nv*3 || md.nVerts != nv) continue;
            md.skinBindPos = sr.basePos;
            md.boneIndices.assign(nv*4, 0);
            md.boneWeights.assign(nv*4, 0);
            sr.jointBox.assign((size_t)sr.nJoints*6, 0.0f);
            for (int b = 0; b < sr.nJoints; ++b) for (int k = 0; k < 3; ++k) { sr.jointBox[b*6+k] = 1e30f; sr.jointBox[b*6+3+k] = -1e30f; }
            for (size_t v = 0; v < nv; ++v) {
                float wsum = 0;
                for (int i = 0; i < 4; ++i) {
                    const float w = sr.jw[v*4+i]; const int b = sr.jidx[v*4+i];
                    if (w > 0.0f && b >= 0 && b < sr.nJoints) wsum += w;
                }
                if (wsum < 1e-4f) {                             // unrigged: weights 0 = the bind position (as on the CPU)
                    for (int k = 0; k < 3; ++k) { sr.unriggedBox[k] = std::min(sr.unriggedBox[k], sr.basePos[v*3+k]); sr.unriggedBox[3+k] = std::max(sr.unriggedBox[3+k], sr.basePos[v*3+k]); }
                    continue;
                }
                int q[4] = {0, 0, 0, 0}, sum = 0, big = -1;
                for (int i = 0; i < 4; ++i) {
                    const float w = sr.jw[v*4+i]; const int b = sr.jidx[v*4+i];
                    if (!(w > 0.0f && b >= 0 && b < sr.nJoints)) continue;
                    q[i] = (int)std::lround(w / wsum * 255.0f); sum += q[i];
                    if (big < 0 || q[i] > q[big]) big = i;
                    md.boneIndices[v*4+i] = (uint8_t)b;
                    float* bx = &sr.jointBox[(size_t)b*6];
                    for (int k = 0; k < 3; ++k) { bx[k] = std::min(bx[k], sr.basePos[v*3+k]); bx[3+k] = std::max(bx[3+k], sr.basePos[v*3+k]); }
                }
                if (big >= 0) q[big] += 255 - sum;              // the weights sum to exactly 1
                for (int i = 0; i < 4; ++i) md.boneWeights[v*4+i] = (uint8_t)std::clamp(q[i], 0, 255);
            }
            sr.gpu = true;
            ++n;
        }
        if (n) log("GPU skinning: %zu of %zu skinned meshes", n, skinRecs.size());
    }

    // mat.sanim "UVTransform" = per-mesh animated 2x3 UV matrix (flipbook / UV-scroll for smoke/
    // fire/dust/fog/particles). libshell drives this as the material's UniformUVOffset/Texm. Keyed
    // by the geo/node name (24/24 match the model nodes).
    struct UVTrack { int nFrames=0; std::vector<float> m;      // m = nFrames * 6 (a,b,c, d,e,f)
                     int scroll = -1; float ru = 0, rv = 0; };   // uvScrollRate's answer for it (-1: not asked yet)
    std::unordered_map<std::string, UVTrack> matUVAnim;
    struct UVAnimRec { size_t meshIdx; std::string node; std::vector<float> baseUV; bool atRest = false; };
    // animateRuntime: this frame's UV matrix M becomes md.uvXform over the base uvs (written once), unless the mesh's
    // lightmap reads the same uvs (no uvs2): then the uvs themselves are rewritten, as animate() does.
    bool uvByMatrix(UVAnimRec& ur, MeshData& md, size_t nuv, const float M[6]) {
        if (!gpuRigid_ || (md.hasLightmap && md.uvs2.size() < nuv*2)) {
            ur.atRest = false;
            static const float I[6] = {1,0,0, 0,1,0};
            std::memcpy(md.uvXform, I, sizeof md.uvXform);
            return false;
        }
        if (!ur.atRest) {
            bool changed = false;
            for (size_t i = 0; i < nuv*2; ++i) changed |= put(md.uvs[i], ur.baseUV[i]);
            if (changed) ++md.uvGen;
            ur.atRest = true;
        }
        std::memcpy(md.uvXform, M, sizeof md.uvXform);
        return true;
    }
    std::vector<UVAnimRec> uvAnimRecs;

    // mat.sanim "MaterialTint" = per-frame RGBA the shader multiplies into the fragment
    // (UniformColor). For fog/dust/flicker the ALPHA animates 0..~0.22, keeping the effect FAINT
    // and pulsing — dropping it makes fog render ~4-5x too dense. Keyed by geo/node name (same as
    // UVTransform). animate(t) samples it into MeshData.curTint; the renderer pushes it as the tint.
    struct TintTrack { int nFrames=0; std::vector<float> rgba; };  // rgba = nFrames * 4
    std::unordered_map<std::string, TintTrack> matTintAnim;
    struct TintRec { size_t meshIdx; std::string node; };
    std::vector<TintRec> tintRecs;

    // SpecIbl diffuse irradiance cubemap (RGBA16F), shipped as `*_diffuse.dds.opa`. The renderer bakes
    // diffuseCube(worldN) into the per-vertex color of `*_specibl` meshes (env-lit, not white/dark).
    ibl::Cubemap iblDiffuse;
    // SpecIbl SPECULAR (roughness-prefiltered) cubemap (RGBA16F), shipped as `*_specular.dds.opa`. We
    // decode mip0 for CPU per-vertex sampling (the sharp env reflection of metallic/gem shells) AND keep
    // the raw bytes for an optional GPU cube upload.
    ibl::Cubemap iblSpecular;
    std::vector<uint8_t> iblSpecularRaw;

    // ── VAT (Vertex Animation Texture): underwater coral/seaweed/fish/jellyfish ──────────────
    // libshell's CoVertexAnimation: a `t_*_vatdata.exr.opa` (cooked as an UNCOMPRESSED RGBA32F KTX,
    // width = #anim-verts, height = #frames) stores a per-frame, per-vertex POSITION OFFSET (frame 0
    // = 0 = rest pose; tiny ±0.1 sway). The mesh's UV1.x (a_texcoords zw@16) is the column (vertex
    // index) into that texture. Each frame: localPos = basePos + vatOffset[frame][col]; the instance
    // node transform places it. Keyed by the geo basename (sm_<X>.fbx <-> t_<X>_vatdata.exr).
    struct VatData { int cols=0, frames=0; std::vector<float> off; };  // off[(f*cols+c)*3 + xyz]
    std::unordered_map<std::string, VatData> vatByBase;
    struct VatRec { size_t meshIdx; std::vector<float> basePos; std::vector<int> col; Mat4 world; const VatData* vd=nullptr; };
    std::vector<VatRec> vatRecs;
    std::string curOpaBase;        // set before each parseModel (composed scene) for VAT matching
    float vatFps = 24.0f;

    bool hasAnimation() const { return (!animRecs.empty() || !skinRecs.empty() || !uvAnimRecs.empty() || !vatRecs.empty()) && (animMaxFrames > 1 || !vatRecs.empty()); }
    float animDuration() const { return animMaxFrames > 1 ? (float)animMaxFrames / animFps : 0.0f; }

    // ── COOK: SKINNED HZANIM extraction (the V79 OPA→V205 port for ALL skinned meshes — was dropped, cook only
    //    did rotation/UV). Builds a device-FAITHFUL HIERARCHICAL skeleton (NOT the flat world-bake): joints = the
    //    clip joints, parents = clip.parents, per-frame trsLocal = clip.mats (already parent-LOCAL, the renderer
    //    composes animWorld[j]=animWorld[parent]*mats[f][j]), bind = inverse(skin invBind) → composed jointBindWorld
    //    (sr.invBind[b]=inverse(jointBindWorld[boneClip[b]]), proven from the renderer's skinning at sub animate()).
    //    boneIdx remaps skin-bone→clip-joint via boneClip. The cook's useHz path (Incredibles-proven) emits HZAN:SKEL+
    //    ACL HZAN:ANIM + AnimatorPlatformComponent. project_hsl_cooker_expose_all_audit.
    struct OpaHzAnim {
        std::vector<float> jointPos, jointQuat, jointScale; std::vector<int> parents;
        std::vector<uint8_t> boneIdx, boneWgt; std::vector<float> trsLocal, restPos;
        int jointCount=0, frameCount=0; float fps=0.f;
        bool ok() const { return jointCount>0 && frameCount>1; }
    };
    // Does this mesh carry a CPU-skinning record (a .skin.opa submesh, device-cooked via HZANIM)? Used by the
    // cook's tint routing: the getTime TINTREPLAY frag stage is NOT reliably driven for skinned-cutout on device,
    // so skinned foliage tints must be baked STATICALLY into COLOR_0 (the base×COLOR0 path always works).
    bool isSkinnedMesh(int meshIdx) const {
        for (auto& r : skinRecs) if ((int)r.meshIdx == meshIdx) return true;
        return false;
    }
    OpaHzAnim extractHzAnim(int meshIdx) {
        OpaHzAnim e;
        const SkinRec* rec=nullptr; for (auto& r : skinRecs) if ((int)r.meshIdx==meshIdx) { rec=&r; break; }
        if (!rec || rec->clipIdx<0 || rec->clipIdx>=(int)clips.size()) return e;
        const AnimClip& clip = clips[rec->clipIdx];
        int nj=clip.numJoints, nf=clip.numFrames;
        if (nj<1 || nf<2 || (int)clip.mats.size() < nf*nj*16) return e;
        // column-major float[16] helpers (match gltf_loader matTrs/mulM)
        auto mul16=[](const float* a,const float* b,float* o){ for(int c=0;c<4;c++)for(int r=0;r<4;r++) o[c*4+r]=a[r]*b[c*4]+a[4+r]*b[c*4+1]+a[8+r]*b[c*4+2]+a[12+r]*b[c*4+3]; };
        auto invAff=[](const float* m,float* o){
            float M00=m[0],M01=m[4],M02=m[8], M10=m[1],M11=m[5],M12=m[9], M20=m[2],M21=m[6],M22=m[10];
            float det=M00*(M11*M22-M12*M21)-M01*(M10*M22-M12*M20)+M02*(M10*M21-M11*M20);
            float id=(det>1e-20f||det<-1e-20f)?1.f/det:0.f;
            o[0]=(M11*M22-M12*M21)*id; o[1]=(M12*M20-M10*M22)*id; o[2]=(M10*M21-M11*M20)*id;
            o[4]=(M02*M21-M01*M22)*id; o[5]=(M00*M22-M02*M20)*id; o[6]=(M01*M20-M00*M21)*id;
            o[8]=(M01*M12-M02*M11)*id; o[9]=(M02*M10-M00*M12)*id; o[10]=(M00*M11-M01*M10)*id;
            o[3]=o[7]=o[11]=0; o[15]=1;
            float tx=m[12],ty=m[13],tz=m[14];
            o[12]=-(o[0]*tx+o[4]*ty+o[8]*tz); o[13]=-(o[1]*tx+o[5]*ty+o[9]*tz); o[14]=-(o[2]*tx+o[6]*ty+o[10]*tz); };
        auto matTrs=[](const float* m,float* q,float* t,float* s){
            t[0]=m[12];t[1]=m[13];t[2]=m[14];
            s[0]=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            // det<0 = MIRRORED node (negative scale — e.g. the *_inside_* searchlight twins): all-positive sqrt
            // scales force the rotation part to absorb the reflection = an IMPROPER matrix whose quaternion is
            // GARBAGE → the mesh cooks rotated/positioned wrong ("out of place"). Negate ONE scale axis (x) so
            // R = M·S⁻¹ is a proper rotation again.
            float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[4]*(m[1]*m[10]-m[2]*m[9]) + m[8]*(m[1]*m[6]-m[2]*m[5]);
            if (det < 0.f) s[0] = -s[0];
            float ix=std::fabs(s[0])>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
            else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
            else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
            else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} };
        e.jointCount=nj; e.frameCount=nf; e.fps = animFps>0.f?animFps:30.f;
        e.parents=clip.parents; if((int)e.parents.size()<nj) e.parents.resize(nj,-1);
        e.restPos=rec->basePos;
        size_t nv=rec->basePos.size()/3;
        // per-vertex bone idx (skin-bone→clip-joint) + normalized u8 weights
        e.boneIdx.assign(nv*4,0); e.boneWgt.assign(nv*4,0);
        for(size_t v=0;v<nv;v++){ float ws=0; for(int c=0;c<4;c++) ws+=rec->jw[v*4+c];
            for(int c=0;c<4;c++){ int sb=rec->jidx[v*4+c]; int cj=(sb>=0&&sb<(int)rec->boneClip.size())?rec->boneClip[sb]:0; if(cj<0||cj>=nj)cj=0;
                e.boneIdx[v*4+c]=(uint8_t)cj; float w=ws>1e-6f?rec->jw[v*4+c]/ws:(c==0?1.f:0.f); int iw=(int)(w*255.f+0.5f); e.boneWgt[v*4+c]=(uint8_t)(iw<0?0:iw>255?255:iw); } }
        // jointBindWorld[j] = inverse(sr.invBind[bone mapping to j]); fallback identity for jointless joints
        std::vector<float> bw((size_t)nj*16,0.f); std::vector<char> have(nj,0);
        for(int b=0;b<rec->nJoints;b++){ int cj=(b<(int)rec->boneClip.size())?rec->boneClip[b]:-1;
            if(cj<0||cj>=nj||have[cj]||(size_t)(b*16+16)>rec->invBind.size()) continue;
            invAff(rec->invBind.data()+(size_t)b*16, bw.data()+(size_t)cj*16); have[cj]=1; }
        for(int j=0;j<nj;j++) if(!have[j]){ float* m=bw.data()+(size_t)j*16; for(int k=0;k<16;k++)m[k]=0; m[0]=m[5]=m[10]=m[15]=1; }
        // bind LOCAL (relative to parent joint) → jointPos/Quat(wxyz)/Scale
        e.jointPos.resize(nj*3); e.jointQuat.resize(nj*4); e.jointScale.resize(nj);
        for(int j=0;j<nj;j++){ float bl[16]; int p=e.parents[j];
            if(p>=0&&p<nj){ float ip[16]; invAff(bw.data()+(size_t)p*16,ip); mul16(ip,bw.data()+(size_t)j*16,bl); }
            else memcpy(bl,bw.data()+(size_t)j*16,64);
            float q[4],t[3],s[3]; matTrs(bl,q,t,s);
            e.jointPos[j*3]=t[0];e.jointPos[j*3+1]=t[1];e.jointPos[j*3+2]=t[2];
            e.jointQuat[j*4]=q[3];e.jointQuat[j*4+1]=q[0];e.jointQuat[j*4+2]=q[1];e.jointQuat[j*4+3]=q[2];
            e.jointScale[j]=(s[0]>1e-4f||s[0]<-1e-4f)?s[0]:1.f; }
        // per-frame local TRS (clip.mats are already parent-local) → trsLocal {qx,qy,qz,qw, t3, s3}
        e.trsLocal.resize((size_t)nf*nj*10);
        for(int f=0;f<nf;f++)for(int j=0;j<nj;j++){ const float* m=clip.mats.data()+((size_t)f*nj+j)*16;
            float q[4],t[3],s[3]; matTrs(m,q,t,s); float* o=e.trsLocal.data()+((size_t)f*nj+j)*10;
            o[0]=q[0];o[1]=q[1];o[2]=q[2];o[3]=q[3]; o[4]=t[0];o[5]=t[1];o[6]=t[2]; o[7]=s[0];o[8]=s[1];o[9]=s[2]; }
        // QUATERNION HEMISPHERE CONTINUITY (same fix as the rigid extractor): fast rotating joints
        // (hummingbird wing flaps, windmill spins) cross polarity in matTrs -> long-arc lerp twitches.
        for (int j = 0; j < nj; j++) for (int f = 1; f < nf; f++) {
            float* q = e.trsLocal.data() + ((size_t)f*nj + j)*10; const float* p = e.trsLocal.data() + ((size_t)(f-1)*nj + j)*10;
            if (q[0]*p[0]+q[1]*p[1]+q[2]*p[2]+q[3]*p[3] < 0.f) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; q[3]=-q[3]; }
        }
        // BUTTERFLY-PARITY (cook-only, renderer unaffected): transform restPos from world-bind space
        // (vertices at entity world position E) to model-local (near origin), matching the official
        // vista butterfly: RENDMESH verts are model-local, root bind = identity, ACL root TRS = world
        // trajectory (for us = E, constant). The device computes skeleton-derived cull bounds by
        // applying each joint's current world matrix to its per-joint vertex extent; world-bind verts
        // cause a double-transform (bw[j] applied to verts already at E → extent at 2E), while
        // model-local verts transform correctly (bw[j] * near-origin ≈ E). extractNodeRigidHzAnim
        // already uses this pattern (restPos = node-LOCAL, bind = identity) and was device-proven.
        if (nj == 1 && have[0]) {
            // 1-JOINT skin whose WORLD placement may live in the BIND, not in the clip trajectory (a SPIN-IN-PLACE:
            // bluehills VE_WESTERN windmill_blades_GEO — clip root T=(0,0,0), the -22 world offset is only in the
            // inverse-bind). Butterfly-parity below (model-local verts + identity root bind + clip-carried world)
            // ASSUMES the clip root carries the world trajectory; for a spin-in-place clip it left the mesh at
            // inv(E)·basePos = model ORIGIN = SPAWN whenever the device was NOT actively driving the 1-joint clip
            // (the "animatedGroup moved to spawn after cook" bug). ROBUST relative form — verts WORLD-baked into the
            // frame-0 rest, identity bind, clip RELATIVE to frame-0 — reproduces the renderer EXACTLY when driven and
            // lands at the correct WORLD rest when frozen/un-driven (identical to extractNodeRigidHzAnim's fix):
            //   R(f) = clip.mats[f][0]·invBind_rep ;  restPos = R(0)·basePos ;  trsLocal[f][0] = clip.mats[f][0]·inv(clip.mats[0][0])
            //   driven:        trsLocal[f][0]·restPos = clip.mats[f][0]·invBind_rep·basePos = renderer (= old butterfly driven)
            //   frozen→identity: restPos = R(0)·basePos = the renderer's frame-0 WORLD rest (the FIX; butterfly gave model-origin)
            float ib0[16]; invAff(bw.data(), ib0);                    // invBind_rep = inverse(bindWorld[0]) = inv(E)
            const float* C0 = clip.mats.data();                       // clip.mats[0][0] (root joint, frame 0)
            float R0[16]; mul16(C0, ib0, R0);                         // R(0) = clip.mats[0][0]·invBind_rep
            for (size_t v = 0; v+2 < e.restPos.size(); v += 3) {      // verts -> frame-0 WORLD rest
                float x=e.restPos[v], y=e.restPos[v+1], z=e.restPos[v+2];
                e.restPos[v]   = R0[0]*x+R0[4]*y+R0[8]*z +R0[12];
                e.restPos[v+1] = R0[1]*x+R0[5]*y+R0[9]*z +R0[13];
                e.restPos[v+2] = R0[2]*x+R0[6]*y+R0[10]*z+R0[14]; }
            float invC0[16]; invAff(C0, invC0);                       // inv(clip.mats[0][0])
            for (int f = 0; f < nf; ++f) {                            // root clip -> RELATIVE to frame-0 world rest
                const float* Cf = clip.mats.data() + (size_t)f*16;    // nj==1 -> joint 0 at frame f
                float rel[16]; mul16(Cf, invC0, rel);
                float q[4],t[3],s[3]; matTrs(rel,q,t,s); float* o=e.trsLocal.data()+(size_t)f*10;
                o[0]=q[0];o[1]=q[1];o[2]=q[2];o[3]=q[3]; o[4]=t[0];o[5]=t[1];o[6]=t[2]; o[7]=s[0];o[8]=s[1];o[9]=s[2]; }
            e.jointPos[0]=0.f;e.jointPos[1]=0.f;e.jointPos[2]=0.f;
            e.jointQuat[0]=1.f;e.jointQuat[1]=0.f;e.jointQuat[2]=0.f;e.jointQuat[3]=0.f;
            e.jointScale[0]=1.f;
        }
        // ROBUST nj>1 (rest-relative reskinning) — GENERALIZES the nj==1 fix above to MULTI-joint skins.
        // The old butterfly-parity baked restPos = inv(E)·basePos (model-LOCAL, near origin). That is only
        // safe when the device is actively DRIVING the clip; whenever it is NOT, the skinning falls back to
        // IDENTITY and those verts render at model ORIGIN ≈ spawn (the bluehills chicken / owl / centralBanner
        // "gray collapsed blob at spawn" bug — m164/m172/m173, whose skeleton roots are world-offset so
        // inv(E)·basePos lands at origin; flags whose roots sit near origin happened to survive). FIX: bake
        // restPos = the fully-skinned FRAME-0 WORLD pose (exactly the renderer's worldRest = Σ wᵢ·clipJointWorld(0)[cjᵢ]·invBind[bᵢ]·basePos),
        // and set each joint's BIND world = clipJointWorld(0)[j] so the device skinMatrix(0)=I. Then:
        //   frozen→identity:  vert = restPos = worldRest                         (correct WORLD placement — THE FIX)
        //   driven:           skinMatrix(f)[j] = clipJointWorld(f)[j]·inv(clipJointWorld(0)[j])
        //                     → single-bound vert(f) = clipJointWorld(f)·invBind·basePos = the renderer EXACTLY
        //                       (multi-bound is the standard rest-relative LBS approximation, visually faithful).
        // clip.mats are parent-LOCAL; renderer composes clipJointWorld[j]=clipJointWorld[parent]·clip.mats[j]
        // (opa_loader animate() L864-871). Renderer path is untouched (it reads rec->basePos/invBind directly).
        else if (nj > 1 && have[0]) {
            std::vector<float> cw0((size_t)nj*16);                    // clipJointWorld(0)[j] (frame-0, world)
            for (int j=0;j<nj;j++){ const float* L = clip.mats.data()+(size_t)j*16;   // frame 0, joint j, parent-local
                int p=e.parents[j];
                if (p<0||p>=j) memcpy(cw0.data()+(size_t)j*16, L, 64);
                else mul16(cw0.data()+(size_t)p*16, L, cw0.data()+(size_t)j*16); }     // cw0[p]·L
            std::vector<float> wr(e.restPos.size());                  // worldRest = renderer's frame-0 skinning
            size_t nvv = rec->basePos.size()/3;
            for (size_t vi=0; vi<nvv; vi++){
                float bx=rec->basePos[vi*3], by=rec->basePos[vi*3+1], bz=rec->basePos[vi*3+2];
                float ox=0,oy=0,oz=0,ws=0;
                for(int i=0;i<4;i++){ float w=rec->jw[vi*4+i]; int b=rec->jidx[vi*4+i];
                    if(w<=0.f||b<0||b>=rec->nJoints) continue;
                    int cj=(b<(int)rec->boneClip.size())?rec->boneClip[b]:-1; if(cj<0||cj>=nj) continue;
                    if((size_t)(b*16+16)>rec->invBind.size()) continue;
                    float M[16]; mul16(cw0.data()+(size_t)cj*16, rec->invBind.data()+(size_t)b*16, M);  // skinMatrix(0)[b]
                    ox+=w*(M[0]*bx+M[4]*by+M[8]*bz+M[12]);
                    oy+=w*(M[1]*bx+M[5]*by+M[9]*bz+M[13]);
                    oz+=w*(M[2]*bx+M[6]*by+M[10]*bz+M[14]); ws+=w; }
                if(ws<1e-4f){ox=bx;oy=by;oz=bz;} else if(ws<0.999f||ws>1.001f){ox/=ws;oy/=ws;oz/=ws;}
                wr[vi*3]=ox; wr[vi*3+1]=oy; wr[vi*3+2]=oz; }
            e.restPos.swap(wr);
            // bindWorld[j] := clipJointWorld(0)[j]  → recompute bind LOCAL TRS so device invBind[j]=inv(cw0[j]).
            for(int j=0;j<nj;j++){ float bl[16]; int p=e.parents[j];
                if(p>=0&&p<nj){ float ip[16]; invAff(cw0.data()+(size_t)p*16,ip); mul16(ip,cw0.data()+(size_t)j*16,bl); }
                else memcpy(bl,cw0.data()+(size_t)j*16,64);
                float q[4],t[3],s[3]; matTrs(bl,q,t,s);
                e.jointPos[j*3]=t[0];e.jointPos[j*3+1]=t[1];e.jointPos[j*3+2]=t[2];
                e.jointQuat[j*4]=q[3];e.jointQuat[j*4+1]=q[0];e.jointQuat[j*4+2]=q[1];e.jointQuat[j*4+3]=q[2];
                e.jointScale[j]=(s[0]>1e-4f||s[0]<-1e-4f)?s[0]:1.f; }
            // trsLocal already = absolute parent-local clip (emitted above), unchanged.
        }
        if (std::getenv("HSR_HZBRANCH")) {
            const char* br = (nj==1&&have[0]) ? "nj1" : (nj>1&&have[0]) ? "nj>1-worldRest" : "DEFAULT(basePos)";
            // how far did reskinning move verts from the raw pre-skin basePos? (0 => restPos==basePos == default/scattered)
            double moved=0; size_t nvv=std::min(e.restPos.size(),rec->basePos.size())/3;
            for(size_t v=0;v<nvv;v++){ double dx=e.restPos[v*3]-rec->basePos[v*3],dy=e.restPos[v*3+1]-rec->basePos[v*3+1],dz=e.restPos[v*3+2]-rec->basePos[v*3+2]; double d=dx*dx+dy*dy+dz*dz; if(d>moved)moved=d; }
            fprintf(stderr,"[HZBRANCH] mesh%d nj=%d have0=%d branch=%s maxVertMovedFromBase=%.3f\n", meshIdx, nj, (int)have[0], br, std::sqrt(moved));
        }
        return e;
    }

    // ── COOK: NON-skinned node-TRANSLATION (cars/train) → a 1-JOINT RIGID HZANIM clip. Reuses the cook's HZANIM
    //    emitter so arbitrary node PATHS port faithfully (the rotation-fit only does pure spin/sway; translation was
    //    DROPPED → cars static on device). joint0 = the node; clip[f] = nodeWorldAnim[node] sampled per frame (WORLD
    //    transform); restPos = node-LOCAL basePos; bind = IDENTITY (invBind=identity → skinMatrix(f)=nodeWorldAnim(f)
    //    → vertex(f)=nodeWorldAnim(f)*basePos = the renderer's node anim). Returns !ok() if the node doesn't TRANSLATE
    //    (origin static) so pure spins keep the lighter getTime() Rodrigues path. project_hsl_opa_anim_port_plan.
    OpaHzAnim extractNodeRigidHzAnim(int meshIdx, bool allowPureRotation = false) {
        OpaHzAnim e;
        if (animMaxFrames < 2) return e;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar || ar->basePos.size() < 9) return e;
        uint32_t node = ar->nodeIdx;
        // PER-NODE loop period — THE faithful fix (matches the renderer; user-confirmed correct). The renderer loops
        // each node track at its OWN nFrames (sampleTrack fmod), so a node's motion repeats every max(effFrames) over
        // its own track + animating ancestors. The GLOBAL animMaxFrames (longest track in the scene = 577 here) is
        // WRONG for a shorter-period node: a 436-frame track sampled over
        // the 577-frame global duration => 1.32 periods => the world position WRAPS mid-clip (sawtooth) => the device
        // loop reset jumps MID-TILE = "speed backward". Sampling over the node's OWN period => exactly one loop =>
        // frame[last]≈frame[0] => seamless. (HSR_NOPERNODELOOP restores the old global behavior.)
        int nodeEff = 0;
        if (!std::getenv("HSR_NOPERNODELOOP"))
            for (int g=0, n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent, ++g) {
                auto it=nodeAnim.find(animNodes[n].name);
                if(it!=nodeAnim.end()){ const NodeTracks&q=it->second;
                    nodeEff=std::max(nodeEff, std::max(q.t.effFrames, std::max(q.r.effFrames, q.s.effFrames))); }
            }
        if (nodeEff < 2) nodeEff = animMaxFrames;   // fallback: no per-node track found -> old global behavior
        float clipDur = (animFps > 0.f) ? (float)nodeEff / animFps : animDuration(); if (clipDur <= 0.f) return e;
        // ALL frames: bake the node path at its FULL native resolution (one sample per source track frame) —
        // FULL PORT, no default cap (rigid clips are small; the old 4096 was arbitrary). HSR_NODECAP opts in.
        int cap = 0x7FFFFFFF; if (const char* e2=std::getenv("HSR_NODECAP")) { int c=atoi(e2); if (c>1) cap=c; }
        int NF = nodeEff > cap ? cap : nodeEff; if (NF < 2) return e;
        auto matTrs=[](const float* m,float* q,float* t,float* s){
            t[0]=m[12];t[1]=m[13];t[2]=m[14];
            s[0]=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            // det<0 = MIRRORED node (negative scale — e.g. the *_inside_* searchlight twins): all-positive sqrt
            // scales force the rotation part to absorb the reflection = an IMPROPER matrix whose quaternion is
            // GARBAGE → the mesh cooks rotated/positioned wrong ("out of place"). Negate ONE scale axis (x) so
            // R = M·S⁻¹ is a proper rotation again.
            float det = m[0]*(m[5]*m[10]-m[6]*m[9]) - m[4]*(m[1]*m[10]-m[2]*m[9]) + m[8]*(m[1]*m[6]-m[2]*m[5]);
            if (det < 0.f) s[0] = -s[0];
            float ix=std::fabs(s[0])>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
            else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
            else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
            else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} };
        // EXCLUSIVE endpoint [0, clipDur): the cooker's loopWrap appends the seam frame (frame0 copy) for
        // cyclic clips — an extractor-side inclusive endpoint DOUBLED it (a held frame every loop). Open
        // paths keep the intentional teleport wrap (cars crossing tile seams).
        std::vector<Mat4> mats((size_t)NF); float o0[3]={0,0,0}, maxd=0.f, rotMaxd=0.f; float rs0[9]={0}; Mat4 W0 = identity();
        for (int f=0; f<NF; f++) { evalAnimNodes(clipDur * (float)f / (float)NF);
            Mat4 w = (node < nodeWorldAnim.size()) ? nodeWorldAnim[node] : identity();
            mats[f]=w; float tx=w.m[12],ty=w.m[13],tz=w.m[14];
            float rs[9]={w.m[0],w.m[1],w.m[2], w.m[4],w.m[5],w.m[6], w.m[8],w.m[9],w.m[10]};   // upper 3x3 (rotation*scale)
            if (f==0){ o0[0]=tx;o0[1]=ty;o0[2]=tz; W0=w; for(int k=0;k<9;k++) rs0[k]=rs[k]; }
            else { float dx=tx-o0[0],dy=ty-o0[1],dz=tz-o0[2]; float d=std::sqrt(dx*dx+dy*dy+dz*dz); if(d>maxd)maxd=d;
                   float rd=0.f; for(int k=0;k<9;k++){ float e2=rs[k]-rs0[k]; rd+=e2*e2; } rd=std::sqrt(rd); if(rd>rotMaxd)rotMaxd=rd; } }
        animate(0.f);   // restore rest pose (geometry bake reads the renderer's GPU model, this just resets the loader)
        // Bail if the node NEITHER translates NOR (when allowed) rotates/scales. Pure single-axis SPINS normally go to the
        // lighter getTime Rodrigues path — BUT a TUMBLE (aurora_noise: w=0 flip-quaternions about a sweeping axis) can't
        // be represented by a single-axis getTime spin, so a UV card with such rotation asks (allowPureRotation) to
        // capture the EXACT per-frame node matrix here = faithful "warped mesh rotates about itself + bends texture".
        if (maxd < 0.01f && !(allowPureRotation && rotMaxd > 0.02f)) return e;
        if (std::getenv("HSR_VERBOSE")) {
            std::string chain; int nodeEff=0;
            for (int g=0,n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent,++g) {
                auto it=nodeAnim.find(animNodes[n].name); int te=0,tn=0;
                if(it!=nodeAnim.end()){ const NodeTracks&q=it->second;
                    te=std::max(q.t.effFrames,std::max(q.r.effFrames,q.s.effFrames)); tn=std::max(q.t.nFrames,std::max(q.r.nFrames,q.s.nFrames)); }
                chain += animNodes[n].name+"(n"+std::to_string(tn)+"/e"+std::to_string(te)+") "; nodeEff=std::max(nodeEff,te);
            }
            fprintf(stderr,"[NODEANIM] mesh%d node=%u period=%d(nodeEff) globalMax=%d maxd=%.0f chain: %s\n", meshIdx, node, nodeEff, animMaxFrames, maxd, chain.c_str());
            // RAW keyframe range of the node's OWN T/R/S tracks — is the huge sweep authored or a decode over-scale?
            { auto it=nodeAnim.find(animNodes[node].name);
              if(it!=nodeAnim.end()){ const auto& q=it->second;
                if(q.t.nFrames>0){ float mn[3]={1e30f,1e30f,1e30f},mx[3]={-1e30f,-1e30f,-1e30f};
                  for(int f=0;f<q.t.nFrames;f++)for(int c=0;c<3;c++){ float v=q.t.v[(size_t)f*3+c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; }
                  fprintf(stderr,"[TRACKRAW] node=%s T(%df) X[%.1f,%.1f] Y[%.1f,%.1f] Z[%.1f,%.1f]  R.nf=%d S.nf=%d\n",
                    animNodes[node].name.c_str(), q.t.nFrames, mn[0],mx[0], mn[1],mx[1], mn[2],mx[2], q.r.nFrames, q.s.nFrames); }
                else fprintf(stderr,"[TRACKRAW] node=%s NO T track (R.nf=%d S.nf=%d) -> sweep from rotation/scale/parent\n",
                    animNodes[node].name.c_str(), q.r.nFrames, q.s.nFrames); } }
        }
        size_t nv = ar->basePos.size()/3;
        // fps over the DOWNSAMPLED frame count so the device loop = the REAL clipDur (the OPA analogue of the
        // 70274aa warp-speed fix). NF<=64 frames are sampled INCLUSIVELY over [0,clipDur] (line ~414) = NF-1
        // intervals, so fps MUST be (NF-1)/clipDur, NOT the global animFps. With animFps a >64-frame path played
        // in (NF-1)/animFps s (e.g. 63/30=2.1s for a real 10s path = ~5x too fast). clipDur>0 + NF>=2 hold above.
        // ── TWO JOINTS: a STATIC root (joint 0) + a MOVING child (joint 1) that carries the whole path. ──────────────
        // THE "train/cars FROZEN on device" fix. A 1-joint clip put the entire node TRANSLATION on the ROOT joint, but
        // V205's AnimatorPlatformComponent does ROOT-MOTION EXTRACTION (libshell strings applyRootMotion /
        // forceRootMotionOnEntityTransform on AnimatorPlatformComponentTranslatorV10): the root joint's TRANSLATION is
        // pulled OUT of the skin pose and applied to the ENTITY transform instead. We cook the entity STATIC, so the
        // extracted translation goes nowhere → the mesh skins at its frame-0 rest every frame = FROZEN IN PLACE (while
        // root-ROTATION clips — spinning discs/doors — animate fine, because root motion only diverts translation+yaw).
        // GROUND TRUTH for the asymmetry: root-rotation cooks loop on device, root-translation cooks freeze. Putting the
        // motion on a CHILD joint (root stays identity) means there is NO root motion to extract → the child bone skins
        // the verts along the full faithful path. HSR_RIGID1JOINT restores the old 1-joint clip.
        bool oneJoint = std::getenv("HSR_RIGID1JOINT") != nullptr;
        e.frameCount=NF; e.fps = (float)NF / clipDur;   // NF frames over [0,clipDur); the cooker's loopWrap appends the seam frame for cyclic clips
        // BIND = IDENTITY (both joints at origin), clip = RELATIVE to frame-0 (W(t)·inv(W0)) on the MOVING joint, verts
        // WORLD-baked into W0. invBind = inverse(bind) = identity, rest = W0·basePos, motion joint world = W(t)·inv(W0):
        //   driven:           vert = W(t)·inv(W0) · (W0·basePos) = W(t)·basePos     (correct full-path anim)
        //   frozen→identity:  vert = I            · (W0·basePos)  = W0·basePos = world rest  (placed correctly when un-driven)
        // (Identical math to the old 1-joint clip, only the motion now lives on a child bone so it survives root-motion extraction.)
        float invW0[16]; mat4affineInverse(W0.m, invW0);
        // ── ACL-CLEAN clip (the "comet thrashes to nonsense positions on device" fix) ────────────────────────────────
        // The old clip stored the mover joint's LOCAL transform as rel = W(f)·inv(W0) against an IDENTITY bind. For a
        // node that ROTATES while offset from the origin (the comets orbit a distant pivot) that decomposition is
        // numerically vicious: rel.translation blows up to THOUSANDS of units (m006 mover maxAbs=10592) with a rotation
        // that exactly cancels it. ACL quantizes translation & rotation SEPARATELY, so a tiny rotation-quat error times
        // the huge offset = wild ±1000u vertex thrash → the comet skins to garbage positions (looked "not animated").
        // FIX: store the mover clip-local as the node's ACTUAL world matrix W(f) (modest translation ~node position) and
        // put W0 in the mover's BIND pose. The device then computes invBind = inv(W0), so skinMatrix = jointWorld(f)·invBind
        // = W(f)·inv(W0) — the SAME skin matrix as before — but the encoded clip now holds small, ACL-friendly numbers.
        // Un-driven fallback stays correct too: skinMatrix = bindWorld·invBind = W0·inv(W0) = I → vert = W0·basePos = world rest.
        // HSR_RIGIDRELBIND restores the old identity-bind / rel-clip encoding.
        float w0q[4], w0t[3], w0s[3]; matTrs(W0.m, w0q, w0t, w0s);
        if (std::getenv("HSR_VERBOSE")) {   // PROOF data for the bind: signed frame-0 scale + per-axis scale range over the loop
            float smn[3]={1e30f,1e30f,1e30f}, smx[3]={-1e30f,-1e30f,-1e30f};
            for (int f=0; f<NF; f++){ float q[4],t[3],s[3]; matTrs(mats[f].m,q,t,s);
                for(int c=0;c<3;c++){ if(s[c]<smn[c])smn[c]=s[c]; if(s[c]>smx[c])smx[c]=s[c]; } }
            fprintf(stderr,"[RIGIDBIND] mesh%d w0t=(%.1f,%.1f,%.1f) w0q=(%.3f,%.3f,%.3f,%.3f) w0s=(%.4f,%.4f,%.4f)%s sRange x[%.4f,%.4f] y[%.4f,%.4f] z[%.4f,%.4f]\n",
                meshIdx, w0t[0],w0t[1],w0t[2], w0q[3],w0q[0],w0q[1],w0q[2], w0s[0],w0s[1],w0s[2],
                (w0s[0]<0.f?" MIRRORED":""), smn[0],smx[0], smn[1],smx[1], smn[2],smx[2]);
        }
        const bool cleanBind = !oneJoint && !std::getenv("HSR_RIGIDRELBIND");
        // BIND SCALE = 1 (the "beams out of place" fix). The device reconstructs invBind from the DECOMPOSED bind
        // TRS with ONE UNIFORM scale per joint, and the ACL clip likewise carries ONE scale channel per joint. The
        // old code baked rest/clip with the EXACT W0 (full per-axis scale) but bound with the uniform average w0su:
        // for a node with NON-UNIFORM or NEGATIVE frame-0 scale (the color_beam stretch/mirror rigs) inv(bind)·W0≠I,
        // and that constant error times the ~2400u node→verts lever = the beam rendered hundreds of units off.
        // FIX: bind with UNIT scale (exactly representable) + store the clip's scale as the per-frame RATIO to the
        // frame-0 scale (below) — constant-scale rigs then have ratio 1 everywhere: nothing for the device's single
        // uniform channel to get wrong, and vert(f) = W(f)·inv(T0·R0)·(W0·basePos) = W(f)·basePos EXACTLY.
        if (oneJoint)        { e.jointCount=1; e.parents={-1}; e.jointPos={0,0,0}; e.jointQuat={1,0,0,0}; e.jointScale={1}; }
        else if (cleanBind)  { e.jointCount=2; e.parents={-1,0};
                               e.jointPos={0,0,0, w0t[0],w0t[1],w0t[2]};
                               e.jointQuat={1,0,0,0, w0q[3],w0q[0],w0q[1],w0q[2]};   // hzJointQuat is (w,x,y,z); matTrs q is (x,y,z,w)
                               e.jointScale={1, 1}; }
        else                 { e.jointCount=2; e.parents={-1,0}; e.jointPos={0,0,0, 0,0,0}; e.jointQuat={1,0,0,0, 1,0,0,0}; e.jointScale={1,1}; }
        const int nj = e.jointCount;                       // 1 (root only) or 2 (static root + moving child)
        const int mj = nj - 1;                             // the MOVING joint index (child if 2-joint, else the root)
        std::vector<float> trsv((size_t)NF*nj*10);
        for (int f=0; f<NF; f++) {
            float q[4],t[3],s[3];
            if (cleanBind) { matTrs(mats[f].m, q, t, s);
                             // scale as the RATIO to frame-0: the frame-0 per-axis (possibly negative/non-uniform)
                             // scale is already IN the world-baked rest verts (rest = W0·basePos), so the clip only
                             // carries the VARIATION. Constant-scale rigs => ratio 1 on every axis => the device's
                             // single uniform scale channel is exact. vert(f) = T(f)R(f)·(s(f)/s0)·s0·basePos = W(f)·basePos.
                             for (int c=0;c<3;c++) s[c] = (std::fabs(w0s[c])>1e-8f) ? s[c]/w0s[c] : 1.f; }
            else { float rel[16]; mat4mul(mats[f].m, invW0, rel); matTrs(rel, q, t, s); }   // legacy rel = W(f)·inv(W0)
            if (nj==2) { float* r=trsv.data()+(size_t)(f*nj+0)*10; r[0]=0;r[1]=0;r[2]=0;r[3]=1; r[4]=0;r[5]=0;r[6]=0; r[7]=1;r[8]=1;r[9]=1; }  // joint0 = STATIC identity (no root motion)
            float* p=trsv.data()+(size_t)(f*nj+mj)*10; p[0]=q[0];p[1]=q[1];p[2]=q[2];p[3]=q[3]; p[4]=t[0];p[5]=t[1];p[6]=t[2]; p[7]=s[0];p[8]=s[1];p[9]=s[2];
        }
        // QUATERNION HEMISPHERE CONTINUITY (the "very shaky" fix): matTrs emits q with an arbitrary sign per
        // frame; a spin crossing 180 deg flips polarity mid-clip and the decoder's neighbor-lerp takes the LONG
        // arc for one interval = a visible twitch every crossing. Keep each joint's quat in the same hemisphere
        // as its previous frame (q and -q are the same rotation; lerp then always takes the short arc).
        for (int j = 0; j < nj; j++) for (int f = 1; f < NF; f++) {
            float* q = &trsv[((size_t)f*nj + j)*10]; const float* p = &trsv[((size_t)(f-1)*nj + j)*10];
            if (q[0]*p[0]+q[1]*p[1]+q[2]*p[2]+q[3]*p[3] < 0.f) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; q[3]=-q[3]; }
        }
        e.restPos.resize(nv*3);                            // verts WORLD-baked into the frame-0 rest (model space == world rest)
        for (size_t v=0; v<nv; v++) { const float* b=&ar->basePos[v*3]; float* o=&e.restPos[v*3];
            o[0]=W0.m[0]*b[0]+W0.m[4]*b[1]+W0.m[8]*b[2]+W0.m[12];
            o[1]=W0.m[1]*b[0]+W0.m[5]*b[1]+W0.m[9]*b[2]+W0.m[13];
            o[2]=W0.m[2]*b[0]+W0.m[6]*b[1]+W0.m[10]*b[2]+W0.m[14]; }
        e.boneIdx.assign(nv*4,0); e.boneWgt.assign(nv*4,0); for (size_t v=0;v<nv;v++){ e.boneIdx[v*4]=(uint8_t)mj; e.boneWgt[v*4]=255; }  // weight every vert to the MOVING joint
        e.trsLocal = std::move(trsv);
        return e;
    }

    // ── COOK: per-frame ROTATION ANGLE replay (the aurora's NON-UNIFORM spin) for the shadergen ROTREPLAY vertex shader.
    //    Samples the node's WORLD rotation over its own loop, derives a fixed axis + the signed angle-about-axis relative
    //    to frame 0 (UNWRAPPED = continuous), + the pivot (node world origin). A getTime VERTEX shader then replays this:
    //    faithful non-uniform rotation, perfectly SMOOTH, and NOT throttled by the skeletal animator's LOD on the big
    //    background sky dome (the device-judder fix). Returns false if the node doesn't actually rotate. ──
    bool cookExtractRotationReplay(int meshIdx, int N, float pivotOut[3], std::vector<float>& quats, float& loopSec) {
        quats.clear();
        const AnimRec* ar=nullptr; for (auto& a:animRecs) if((int)a.meshIdx==meshIdx){ar=&a;break;}
        if (!ar) return false;
        uint32_t node = ar->nodeIdx; if (node >= animNodes.size()) return false;
        int nodeEff=0;
        for (int g=0,n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent,++g){
            auto it=nodeAnim.find(animNodes[n].name);
            if(it!=nodeAnim.end()){const NodeTracks&q=it->second; nodeEff=std::max(nodeEff,std::max(q.t.effFrames,std::max(q.r.effFrames,q.s.effFrames)));}
        }
        if (nodeEff<2) nodeEff=animMaxFrames;
        loopSec = (animFps>0.f)?(float)nodeEff/animFps:animDuration(); if (loopSec<=1e-4f) return false;
        if (N<2) N=2;
        auto matQuat=[](const float* m, float* q){
            float sx=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]), sy=std::sqrt(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]), sz=std::sqrt(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=sx>1e-8f?1/sx:0, iy=sy>1e-8f?1/sy:0, iz=sz>1e-8f?1/sz:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if(tr>0){float S=std::sqrt(tr+1)*2;q[3]=0.25f*S;q[0]=(r5-r7)/S;q[1]=(r6-r2)/S;q[2]=(r1-r3)/S;}
            else if(r0>r4&&r0>r8){float S=std::sqrt(1+r0-r4-r8)*2;q[3]=(r5-r7)/S;q[0]=0.25f*S;q[1]=(r3+r1)/S;q[2]=(r6+r2)/S;}
            else if(r4>r8){float S=std::sqrt(1+r4-r0-r8)*2;q[3]=(r6-r2)/S;q[0]=(r3+r1)/S;q[1]=0.25f*S;q[2]=(r7+r5)/S;}
            else{float S=std::sqrt(1+r8-r0-r4)*2;q[3]=(r1-r3)/S;q[0]=(r6+r2)/S;q[1]=(r7+r5)/S;q[2]=0.25f*S;} };
        evalAnimNodes(0.f);
        Mat4 W0 = (node<nodeWorldAnim.size())?nodeWorldAnim[node]:identity();
        pivotOut[0]=W0.m[12]; pivotOut[1]=W0.m[13]; pivotOut[2]=W0.m[14];
        float q0[4]; matQuat(W0.m,q0); float c0[4]={-q0[0],-q0[1],-q0[2],q0[3]};   // conj(q0)
        quats.resize((size_t)(N+1)*4); float bestMag=0.f;
        for (int f=0; f<=N; f++){
            evalAnimNodes(loopSec*(float)f/(float)N);
            Mat4 W=(node<nodeWorldAnim.size())?nodeWorldAnim[node]:identity();
            float q[4]; matQuat(W.m,q); float r[4];   // rel = q * conj(q0) (xyzw) = EXACT per-frame relative rotation
            r[0]= q[3]*c0[0] + q[0]*c0[3] + q[1]*c0[2] - q[2]*c0[1];
            r[1]= q[3]*c0[1] - q[0]*c0[2] + q[1]*c0[3] + q[2]*c0[0];
            r[2]= q[3]*c0[2] + q[0]*c0[1] - q[1]*c0[0] + q[2]*c0[3];
            r[3]= q[3]*c0[3] - q[0]*c0[0] - q[1]*c0[1] - q[2]*c0[2];
            // NO handedness flip: rel = q*conj(q0) as-is IS the renderer's rotation. The old default FLIP
            // (conjugate) made the cooked dome spin OPPOSITE the source, so the data UV counter-scroll ADDED
            // instead of cancelling -> the aurora texture swept ~half the sky by mid-loop (verified by
            // pinned-time captures: no-flip t=20 == source t=20 exactly; flipped drifted 2x0.24 widths).
            if (std::getenv("HSR_ROTFLIP")) { r[0]=-r[0]; r[1]=-r[1]; r[2]=-r[2]; }   // opt-in for A/B testing only
            // sign-continuity: keep each quat in the same hemisphere as the previous so the shader nlerp takes the short arc
            if (f>0){ float* p=&quats[(size_t)(f-1)*4]; if (r[0]*p[0]+r[1]*p[1]+r[2]*p[2]+r[3]*p[3] < 0){ r[0]=-r[0];r[1]=-r[1];r[2]=-r[2];r[3]=-r[3]; } }
            float* o=&quats[(size_t)f*4]; o[0]=r[0];o[1]=r[1];o[2]=r[2];o[3]=r[3];
            float mag=std::sqrt(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); if (mag>bestMag) bestMag=mag;
        }
        evalAnimNodes(0.f);   // restore rest
        if (bestMag < 0.02f) { quats.clear(); return false; }   // node doesn't actually rotate
        if (std::getenv("HSR_ROTDUMP")) {   // diagnose tumble-vs-spin: print axis+angle of the sampled relative rotation
            fprintf(stderr,"[ROTDUMP] mesh %d node '%s' loop=%.1fs pivot=(%.1f,%.1f,%.1f) N=%d\n",
                meshIdx, (node<animNodes.size()?animNodes[node].name.c_str():"?"), loopSec, pivotOut[0],pivotOut[1],pivotOut[2], N);
            for (int f=0; f<=N; f+=std::max(1,N/12)){
                float* q=&quats[(size_t)f*4]; float w=q[3]; if(w>1)w=1; if(w<-1)w=-1;
                float ang=2.f*std::acos(w)*57.2958f; float s=std::sqrt(1-w*w); float ax=0,ay=0,az=0;
                if(s>1e-5f){ax=q[0]/s;ay=q[1]/s;az=q[2]/s;}
                fprintf(stderr,"  f%3d  ang=%6.1f deg  axis=(%+.2f,%+.2f,%+.2f)\n", f, ang, ax,ay,az);
            }
        }
        return true;
    }

    // ── COOK: node TRANSLATION (cars/train) → the NET world displacement over the clip, for a ShellPoseAnimationComponent
    //    (the FAITHFUL, device-proven V79→V205 node-anim port — NOT a 1-joint skin, which the device's MeshDefinition::fix
    //    REJECTS as a degenerate maxBoneIdx=0 skin → "flatbuffer verification failed" → no render). Mesh stays STATIC
    //    (valid), the entity pose lerps rest→rest+delta. Returns false if the node doesn't translate (pure spin keeps
    //    the getTime() Rodrigues path). delta = the MAX origin displacement over the clip. ──
    bool extractNodeTranslate(int meshIdx, float delta[3]) {
        delta[0]=delta[1]=delta[2]=0.f;
        if (animMaxFrames < 2) return false;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        uint32_t node=ar->nodeIdx; float cd=animDuration(); if (cd<=0.f) return false;
        evalAnimNodes(0.f);
        float o0[3]={0,0,0}; if (node<nodeWorldAnim.size()){ o0[0]=nodeWorldAnim[node].m[12]; o0[1]=nodeWorldAnim[node].m[13]; o0[2]=nodeWorldAnim[node].m[14]; }
        float best[3]={0,0,0}, bestd=0.f; const int NS=24;
        for (int f=1; f<=NS; f++) { evalAnimNodes(cd*(float)f/(float)NS);
            if (node>=nodeWorldAnim.size()) continue;
            float dx=nodeWorldAnim[node].m[12]-o0[0], dy=nodeWorldAnim[node].m[13]-o0[1], dz=nodeWorldAnim[node].m[14]-o0[2];
            float d=dx*dx+dy*dy+dz*dz; if (d>bestd){ bestd=d; best[0]=dx;best[1]=dy;best[2]=dz; } }
        animate(0.f);
        if (bestd < 1e-4f) return false;
        delta[0]=best[0]; delta[1]=best[1]; delta[2]=best[2];
        return true;
    }

    // ── COOK: node SCALE "breathe" (NON-UNIFORM per-axis) → N per-axis FACTOR frames (= scale(t)/scale(0), frame0=(1,1,1))
    //    sampled over the SCALE track's OWN length, for a shadergen::SCALE getTime() shader. pivot = node WORLD origin
    //    (the shader scales in place). Scale is a LOCAL channel, so the factor needs only the node's own scale track.
    //    Returns false if the node has no scale track or it doesn't actually breathe (matches the glTF extractNodeScaleFrames). ──
    bool cookExtractNodeScaleFrames(int meshIdx, int N, std::vector<float>& frameFactors, float& loopSec, float pivot[3]) {
        frameFactors.clear(); loopSec = 0.f; pivot[0]=pivot[1]=pivot[2]=0.f;
        if (animMaxFrames < 2 || N < 2) return false;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        uint32_t node=ar->nodeIdx; if (node >= animNodes.size()) return false;
        auto it = nodeAnim.find(animNodes[node].name);
        if (it == nodeAnim.end() || it->second.s.nFrames <= 1 || it->second.s.comps != 3) return false;   // no real scale track
        const Track& st = it->second.s;
        // loopSec = the SCALE track's OWN length (frames/fps), like the glTF per-clip-duration. animFps = the engine rate.
        loopSec = (animFps > 0.f) ? (float)st.nFrames / animFps : 0.f;
        if (loopSec <= 1e-4f) loopSec = animDuration();
        if (loopSec <= 1e-4f) return false;
        float base[3]={animNodes[node].s[0],animNodes[node].s[1],animNodes[node].s[2]};
        sampleTrack(st, 0.f, animFps, 3, base);                       // t=0 baked scale (clamped first frame)
        for (int c=0;c<3;c++) if (base[c]==0.f) base[c]=1.f;
        frameFactors.assign((size_t)N*3, 1.f); float maxDev=0.f;
        for (int fi=0; fi<N; ++fi){ float s[3]={base[0],base[1],base[2]};
            sampleTrack(st, loopSec*(float)fi/(float)N, animFps, 3, s);
            for (int c=0;c<3;c++){ float f=s[c]/base[c]; frameFactors[(size_t)fi*3+c]=f; float d=f>1.f?f-1.f:1.f-f; if(d>maxDev)maxDev=d; } }
        evalAnimNodes(0.f);   // rest pose -> node world origin = pivot
        if (node < nodeWorldAnim.size()){ pivot[0]=nodeWorldAnim[node].m[12]; pivot[1]=nodeWorldAnim[node].m[13]; pivot[2]=nodeWorldAnim[node].m[14]; }
        animate(0.f);
        return maxDev > 1e-3f;   // actually breathes
    }

    // COOK: does this mesh's node (or any animating ancestor) actually ROTATE or SCALE (beyond identity)? Effect cards
    // (fog/dust) with a UV flipbook AND a node R/S can't be faithfully ported by the getTime translate shader (it only
    // translates; a hierarchical/rotating/far-pivot scale would FLING the card). Such cards take the RIGID-HZANIM path
    // (exact per-frame WORLD matrix via a skeleton) WITH the UV/fade baked into the SKINNED material shader. Pure-translate
    // cards stay on the lighter getTime translate+flipbook+fade path. ── returns true iff a real R or S track is present.
    bool nodeAnimatesRotOrScale(int meshIdx) {
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        for (int n=(int)ar->nodeIdx, g=0; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent, ++g) {
            auto it = nodeAnim.find(animNodes[n].name);
            if (it == nodeAnim.end()) continue;
            const NodeTracks& q = it->second;
            if (q.s.nFrames>1 && q.s.comps==3) {   // scale breathe (per-axis range > 2%)
                float mn[3]={1e9f,1e9f,1e9f}, mx[3]={-1e9f,-1e9f,-1e9f};
                for (int f=0; f<q.s.nFrames; ++f) for (int c=0;c<3;c++){ float v=q.s.v[(size_t)f*3+c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; }
                for (int c=0;c<3;c++) if (mx[c]-mn[c] > 0.02f) return true;
            }
            if (q.r.nFrames>1 && q.r.comps==4) {   // rotation quaternion actually varies
                float maxd=0.f; for (int f=1; f<q.r.nFrames; ++f){ float d=0.f; for(int c=0;c<4;c++){ float e=q.r.v[(size_t)f*4+c]-q.r.v[c]; d+=e*e; } if(d>maxd)maxd=d; }
                if (maxd > 1e-3f) return true;
            }
        }
        return false;
    }
    // node index of a node-animated mesh (-1 if none), and a node's parent — so the cook can find the TRAIN BODY (the
    // direct PARENT node of the train STEAM's node) and route it to the same getTime() clock so the two stay attached.
    int animNodeOf(int meshIdx) const { for (auto& a:animRecs) if ((int)a.meshIdx==meshIdx) return (int)a.nodeIdx; return -1; }
    int animNodeParentOf(int node) const { return (node>=0 && node<(int)animNodes.size()) ? animNodes[node].parent : -1; }
    const char* animNodeNameOf(int meshIdx) const {
        int node = animNodeOf(meshIdx);
        return (node >= 0 && node < (int)animNodes.size()) ? animNodes[node].name.c_str() : "";
    }
    bool animNodeNamed(int meshIdx, const char* name) const {
        int node = animNodeOf(meshIdx);
        return name && node >= 0 && node < (int)animNodes.size() && animNodes[node].name == name;
    }

    // ── COOK: node TRANSLATION (train STEAM / drifting FOG) → N WORLD-space OFFSET frames (delta from the frame-0 world
    //    position, frame0=(0,0,0)) sampled over the node's OWN period, for a shadergen::TRANSLATE getTime() shader. The
    //    device-PROVEN self-looping getTime() VERTEX family (same as ROTATE/SCALE) — used to give a flipbook card its node
    //    motion WITHOUT the skinned (useHz) path that froze the texture. Verts stay world-baked; the shader adds offset(t).
    //    Mirrors the glTF extractNodeTranslation (WORLD delta so the parent scale is honored). Returns false if it doesn't move. ──
    bool cookExtractNodeTranslateFrames(int meshIdx, int N, std::vector<float>& frameOffs, float& loopSec, float forceLoopSec=-1.f) {
        frameOffs.clear(); loopSec = 0.f;
        if (animMaxFrames < 2 || N < 2) return false;
        const AnimRec* ar=nullptr; for (auto& a : animRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        uint32_t node=ar->nodeIdx; if (node >= animNodes.size()) return false;
        // PER-NODE period (matches the renderer's per-track loop + extractNodeRigidHzAnim): max effFrames up the chain.
        int nodeEff=0;
        if (!std::getenv("HSR_NOPERNODELOOP"))
            for (int g=0,n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent,++g){
                auto it=nodeAnim.find(animNodes[n].name);
                if(it!=nodeAnim.end()){ const NodeTracks&q=it->second;
                    nodeEff=std::max(nodeEff, std::max(q.t.effFrames, std::max(q.r.effFrames, q.s.effFrames))); } }
        if (nodeEff < 2) nodeEff = animMaxFrames;
        loopSec = (animFps>0.f) ? (float)nodeEff/animFps : animDuration();
        // SYNC: effect cards (fog/dust) drive UV-flipbook + fade + movement off ONE mat.sanim clock in V79 — pass the
        // mat.sanim loopSec here so the node MOVEMENT is sampled over the SAME period (evalAnimNodes still loops the
        // node's own track internally), locking movement to the fade/flipbook. Fixes desync ("moves too fast / doesn't
        // follow its animation" when translate looped at 3.37s vs UV/fade 5.03s).
        if (forceLoopSec > 1e-4f) loopSec = forceLoopSec;
        if (loopSec <= 1e-4f) return false;
        evalAnimNodes(0.f);
        float w0[3]={0,0,0}; if (node<nodeWorldAnim.size()){ w0[0]=nodeWorldAnim[node].m[12]; w0[1]=nodeWorldAnim[node].m[13]; w0[2]=nodeWorldAnim[node].m[14]; }
        frameOffs.assign((size_t)N*3, 0.f); float maxd=0.f;
        for (int fi=0; fi<N; ++fi){ evalAnimNodes(loopSec*(float)fi/(float)N);   // EXCLUSIVE endpoint [0,loopSec) -> one clean monotonic period
            if (node>=nodeWorldAnim.size()) continue;
            for(int c=0;c<3;c++){ float o=nodeWorldAnim[node].m[12+c]-w0[c]; frameOffs[(size_t)fi*3+c]=o; float ad=o<0?-o:o; if(ad>maxd)maxd=ad; } }
        // DESPIKE lone "there-and-back" outliers: sampleTrack does LINEAR quaternion interp, which between two
        // far-apart key rotations produces a WRONG intermediate orientation for a single sample -> one frame that
        // flings a far-offset node (the comet's t2=-237 blip: t1->t2->t3 deltas point OPPOSITE ways). The desktop's
        // 90fps continuous playback flickers past it, but a cooked N-frame getTime replay would show a teleport.
        // Replace ONLY a frame whose in/out deltas oppose (a there-and-back reversal), both large -> neighbor midpoint.
        // Smooth / monotonic / hold+teleport paths (aligned or ~zero deltas) are untouched. HSR_NODESPIKE disables.
        if (!std::getenv("HSR_NODESPIKE")) for (int fi=1; fi+1<N; ++fi){
            float* c=&frameOffs[(size_t)fi*3]; const float* p=&frameOffs[(size_t)(fi-1)*3]; const float* n=&frameOffs[(size_t)(fi+1)*3];
            float d1[3]={c[0]-p[0],c[1]-p[1],c[2]-p[2]}, d2[3]={n[0]-c[0],n[1]-c[1],n[2]-c[2]};
            float l1=std::sqrt(d1[0]*d1[0]+d1[1]*d1[1]+d1[2]*d1[2]), l2=std::sqrt(d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2]);
            float dot=d1[0]*d2[0]+d1[1]*d2[1]+d1[2]*d2[2];
            if (l1 > 0.1f*maxd && l2 > 0.1f*maxd && dot < -0.3f*l1*l2)   // clearly opposing (>~107 deg) + both large
                { c[0]=0.5f*(p[0]+n[0]); c[1]=0.5f*(p[1]+n[1]); c[2]=0.5f*(p[2]+n[2]); }
        }
        maxd=0.f; for (size_t k=0;k<frameOffs.size();++k){ float a=std::fabs(frameOffs[k]); if(a>maxd)maxd=a; }   // recompute after despike
        animate(0.f);
        return maxd > 1e-3f;   // actually translates (WORLD space)
    }
    // ── COOK: the mat.sanim MaterialTint ALPHA track (per-frame opacity, 0..~0.22 for fog/dust) — the FADE curve the
    //    fragment shader replays so the card fades IN/OUT over the loop (hiding a one-way node-translate's reset +
    //    underground tail). Returns all nFrames alpha values + the track's natural loop seconds. ──
    bool cookExtractTintAlpha(int meshIdx, std::vector<float>& alpha, float& loopSec) {
        alpha.clear(); loopSec = 0.f;
        const TintRec* tr=nullptr; for (auto& r : tintRecs) if ((int)r.meshIdx==meshIdx){ tr=&r; break; }
        if (!tr) return false;
        auto it = matTintAnim.find(tr->node);
        if (it == matTintAnim.end() || it->second.nFrames < 2) return false;
        const TintTrack& tt = it->second;
        alpha.resize(tt.nFrames);
        float amax=0.f; for (int f=0;f<tt.nFrames;f++){ alpha[f]=tt.rgba[(size_t)f*4+3]; if(alpha[f]>amax)amax=alpha[f]; }
        loopSec = (animFps>0.f) ? (float)tt.nFrames/animFps : 0.f;
        return amax > 1e-4f && loopSec > 1e-4f;   // a real (non-zero) fade
    }
    // ── COOK: the FULL mat.sanim MaterialTint RGBA track (per-frame color) — V79's tint COLOR cycling
    //    (stinson fireworks flash colors, city window-light flicker). The renderer plays this live (animate()
    //    -> md.curTint) so the preview was right, but the cook only ported the ALPHA subset as the flipbook
    //    fade -> device showed one static "generic" color. Subsampled to maxN (frame-SNAP replay; flashes are
    //    steps). Returns false when no track exists or nothing varies (static tint = the baked COLOR_0). ──
    bool cookExtractTintRGBA(int meshIdx, int maxN, std::vector<float>& rgba, int& N, float& loopSec) {
        rgba.clear(); N = 0; loopSec = 0.f;
        const TintRec* tr = nullptr; for (auto& r : tintRecs) if ((int)r.meshIdx == meshIdx) { tr = &r; break; }
        if (!tr || maxN < 2) return false;
        auto it = matTintAnim.find(tr->node);
        if (it == matTintAnim.end() || it->second.nFrames < 2) return false;
        const TintTrack& tt = it->second;
        loopSec = (animFps > 0.f) ? (float)tt.nFrames / animFps : 0.f;
        if (loopSec <= 1e-4f) return false;
        N = tt.nFrames <= maxN ? tt.nFrames : maxN;
        rgba.assign((size_t)N * 4, 1.f);
        float mn[4]={1e9f,1e9f,1e9f,1e9f}, mx[4]={-1e9f,-1e9f,-1e9f,-1e9f};
        for (int i = 0; i < N; i++) {
            int src = (tt.nFrames == N) ? i : (int)((double)i * tt.nFrames / (double)N); if (src >= tt.nFrames) src = tt.nFrames - 1;
            for (int c = 0; c < 4; c++) { float v = tt.rgba[(size_t)src*4 + c]; rgba[(size_t)i*4 + c] = v; if (v<mn[c])mn[c]=v; if (v>mx[c])mx[c]=v; }
        }
        float dev = 0.f; for (int c = 0; c < 4; c++) { float d = mx[c]-mn[c]; if (d > dev) dev = d; }
        return dev > 1e-3f;   // actually cycles (any channel)
    }
    // ── COOK: bake a VAT (Vertex Animation Texture) mesh's per-frame WORLD-space offsets for the cooker's
    //    DEVICE-PROVEN useVat path (RGBA16 offset texture + vatunlitbasecolor/vatunlitblend, the Erebor-wisp
    //    pipeline). The underwater corals/seaweed/fish carry REAL source VAT data (t_*_vatdata.exr.opa) that the
    //    renderer plays via vatRecs, but the cook never consumed it → every VAT mesh cooked STATIC ("corals,
    //    fishes, etc most stuff ain't animating"). offset[f][v] = worldRot·(off[srcFrame(f)][col] − off[0][col]);
    //    the static geometry bake is the t=0 pose (animate(0) → base+off[0]) so the shader's `pos += offset(t)`
    //    reproduces the sway exactly, WORLD-space (the baked entity transform is identity). Frames are subsampled
    //    to `frames` over ONE loop (exclusive endpoint; the shader's time-fract wraps seamlessly like the source's
    //    own frame wrap). Returns empty if this mesh has no VAT record (or it's too wide for the offset texture).
    std::vector<float> bakeVAT(int meshIdx, int frames, int& nvOut) {
        nvOut = 0;
        const VatRec* vr = nullptr;
        for (auto& r : vatRecs) if ((int)r.meshIdx == meshIdx) { vr = &r; break; }
        if (!vr || !vr->vd || vr->vd->frames < 2 || vr->vd->cols < 1 || frames < 2) return {};
        const VatData& vd = *vr->vd;
        size_t nv = vr->basePos.size() / 3;
        if (nv < 1 || nv > 8192) return {};   // offset-texture width = vertex count; keep it GPU-sane
        if (vr->col.size() < nv) return {};
        nvOut = (int)nv;
        // rotation(+scale) 3x3 of the instance world matrix — offsets are DIRECTION vectors (no translation)
        const float* w = vr->world.m;
        std::vector<float> out((size_t)frames * nv * 3);
        for (int f = 0; f < frames; ++f) {
            float sf = (float)f * (float)vd.frames / (float)frames;   // [0, vd.frames) exclusive — one clean loop
            int i0 = (int)sf; float frac = sf - (float)i0;
            if (i0 >= vd.frames) { i0 = vd.frames - 1; frac = 0.f; }
            int i1 = (i0 + 1 < vd.frames) ? i0 + 1 : 0;               // wrap = seamless loop (matches animate())
            for (size_t v = 0; v < nv; ++v) {
                int col = vr->col[v]; if (col < 0 || col >= vd.cols) col = 0;
                const float* o0 = &vd.off[((size_t)i0 * vd.cols + col) * 3];
                const float* o1 = &vd.off[((size_t)i1 * vd.cols + col) * 3];
                const float* z0 = &vd.off[(size_t)col * 3];           // frame 0 = the static bake pose
                float dx = o0[0]*(1.f-frac) + o1[0]*frac - z0[0];
                float dy = o0[1]*(1.f-frac) + o1[1]*frac - z0[1];
                float dz = o0[2]*(1.f-frac) + o1[2]*frac - z0[2];
                float* o = &out[((size_t)f * nv + v) * 3];
                o[0] = w[0]*dx + w[4]*dy + w[8]*dz;
                o[1] = w[1]*dx + w[5]*dy + w[9]*dz;
                o[2] = w[2]*dx + w[6]*dy + w[10]*dz;
            }
        }
        return out;
    }
    // ── COOK: bake ANY node-animated mesh's EXACT per-frame WORLD motion as VAT offsets, by sampling the renderer's
    //    own evalAnimNodes(t) (the same path the OPA source draws with). Reproduces the motion BYTE-FOR-BYTE — no
    //    rigid-HZANIM restPos/clip/invBind decomposition (which amplified the stinson city color_beam ~4700u sweep so
    //    it flew out of the building). offset[f][v] = nodeWorld(t_f)·basePos[v] − nodeWorld(0)·basePos[v]; the static
    //    bake is frame 0 (animate(0)), so the device shader's pos += offset(t) == the source EXACTLY. ──
    std::vector<float> bakeNodeAnimVAT(int meshIdx, int frames, int& nvOut, float& loopSec) {
        nvOut = 0; loopSec = 0.f;
        const AnimRec* ar = nullptr; for (auto& a : animRecs) if ((int)a.meshIdx == meshIdx) { ar = &a; break; }
        if (!ar || ar->basePos.size() < 9 || frames < 2) return {};
        size_t nv = ar->basePos.size()/3; if (nv < 1 || nv > 8192) return {};
        uint32_t node = ar->nodeIdx;
        int eff = 0;   // per-node loop period (same as extractNodeRigidHzAnim / the renderer)
        if (!std::getenv("HSR_NOPERNODELOOP"))
            for (int g=0,n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent,++g){
                auto it=nodeAnim.find(animNodes[n].name);
                if(it!=nodeAnim.end()){ const NodeTracks&q=it->second; eff=std::max(eff, std::max(q.t.effFrames, std::max(q.r.effFrames, q.s.effFrames))); } }
        if (eff < 2) eff = animMaxFrames;
        float dur = (animFps>0.f) ? (float)eff/animFps : animDuration(); if (dur <= 0.f) return {};
        loopSec = dur;
        evalAnimNodes(0.f);
        Mat4 w0 = (node < nodeWorldAnim.size()) ? nodeWorldAnim[node] : identity();
        std::vector<float> p0(nv*3);
        for (size_t v=0; v<nv; v++){ float o[3]; xform(w0, ar->basePos[v*3],ar->basePos[v*3+1],ar->basePos[v*3+2], o); p0[v*3]=o[0];p0[v*3+1]=o[1];p0[v*3+2]=o[2]; }
        std::vector<float> out((size_t)frames*nv*3);
        for (int f=0; f<frames; f++){
            evalAnimNodes(dur * (float)f / (float)frames);   // exclusive endpoint -> seamless loop
            Mat4 w = (node < nodeWorldAnim.size()) ? nodeWorldAnim[node] : identity();
            for (size_t v=0; v<nv; v++){ float o[3]; xform(w, ar->basePos[v*3],ar->basePos[v*3+1],ar->basePos[v*3+2], o);
                out[((size_t)f*nv+v)*3]   = o[0]-p0[v*3];
                out[((size_t)f*nv+v)*3+1] = o[1]-p0[v*3+1];
                out[((size_t)f*nv+v)*3+2] = o[2]-p0[v*3+2]; }
        }
        animate(0.f);   // restore rest pose (the static geometry bake reads frame 0)
        nvOut = (int)nv;
        return out;
    }
    // ── COOK: batch-fit every node-animated mesh to a SPIN/SWAY about an axis (the V79->V203 port). Samples each
    //    animated mesh's WORLD positions across the clip via animate(t), runs the shared noderot::fit, and returns
    //    meshIdx -> Result. Leaves the meshes at REST (animate(0)) so the static geometry bake is the t=0 pose. The
    //    cooker's useRot path then ships a getTime() Rodrigues shader. (Node TRANSFORM anims = the bulk of OPA motion;
    //    UV-scroll / skinned / VAT are separate passes.) ──
    void cookExtractRotations(std::unordered_map<size_t, noderot::Result>& out) {
        out.clear();
        if (animRecs.empty() || animMaxFrames < 2) return;
        float globalDur = animDuration(); if (globalDur <= 0.f) return;
        const int NS = 24;
        const size_t budget = 48000000;                  // ≤ ~190MB of position floats in flight at once (no swap/softlock)
        const size_t maxNv  = budget / ((size_t)(NS+1)*3);   // a single mesh bigger than this cooks static (huge meshes aren't the spinning ones)
        // ── PER-NODE PERIOD (THE fix for the windmill_tails "in place but not moving" cook bug) ──────────────────────
        // The fit MUST sample one clean rotation. Sampling a node over the GLOBAL clip (animMaxFrames=2501f/83.4s) when
        // its OWN rotation period is short (windmill spin = 305f/~10s) packs ~8 turns into NS=24 samples → the angles
        // alias and noderot::fit degenerates to a 4°/83s "sway" → the cooked getTime() shader barely moves on device
        // (desktop preview animated fine because it drives the node directly). Sample each mesh over its node's OWN
        // period (max track effFrames up its chain — identical to extractNodeRigidHzAnim/the renderer's per-node loop)
        // → exactly one period → fit recovers the true SPIN. HSR_NOPERNODELOOP restores the old global behavior.
        auto nodePeriodFrames = [&](uint32_t node)->int{
            int eff=0;
            if (!std::getenv("HSR_NOPERNODELOOP"))
                for (int g=0,n=(int)node; n>=0 && n<(int)animNodes.size() && g<32; n=animNodes[n].parent,++g){
                    auto it=nodeAnim.find(animNodes[n].name);
                    if(it!=nodeAnim.end()){ const NodeTracks&q=it->second;
                        eff=std::max(eff, std::max(q.t.effFrames, std::max(q.r.effFrames, q.s.effFrames))); } }
            if (eff < 2) eff = animMaxFrames;
            return eff;
        };
        struct M { size_t idx; size_t nv; uint32_t node; const std::vector<float>* base; float pivot[3]; float dur; };
        std::vector<M> ms; std::unordered_map<size_t,int> seen;
        for (auto& ar : animRecs) { size_t nv = ar.basePos.size()/3; if (nv < 3 || nv > maxNv) continue; if (seen.count(ar.meshIdx)) continue;
            seen[ar.meshIdx]=1; int eff=nodePeriodFrames(ar.nodeIdx); float dur=(animFps>0.f)?(float)eff/animFps:globalDur; if(dur<=0.f)dur=globalDur;
            ms.push_back({ar.meshIdx, nv, ar.nodeIdx, &ar.basePos, {0,0,0}, dur}); }
        if (ms.empty()) { animate(0.f); return; }
        evalAnimNodes(0.f);   // rest pose -> each node's world origin = its pivot
        for (auto& m : ms) if (m.node < nodeWorldAnim.size()) { m.pivot[0]=nodeWorldAnim[m.node].m[12]; m.pivot[1]=nodeWorldAnim[m.node].m[13]; m.pivot[2]=nodeWorldAnim[m.node].m[14]; }
        // Sample EACH mesh over its OWN period (per-mesh node eval — a handful of spin meshes × NS calls, cheap).
        std::vector<std::vector<float>> fr(NS+1);
        for (auto& m : ms) {
            for (int f=0; f<=NS; f++) { evalAnimNodes(m.dur * (float)f / (float)NS);
                Mat4 w=(m.node<nodeWorldAnim.size())?nodeWorldAnim[m.node]:identity();
                fr[f].resize(m.nv*3);
                for (size_t v=0; v<m.nv; v++){ float o[3]; xform(w,(*m.base)[v*3],(*m.base)[v*3+1],(*m.base)[v*3+2],o); fr[f][v*3]=o[0];fr[f][v*3+1]=o[1];fr[f][v*3+2]=o[2]; } }
            std::vector<const float*> fp(NS+1); for (int f=0;f<=NS;f++) fp[f]=fr[f].data();
            noderot::Result r = noderot::fit(fp, m.nv, m.pivot, m.dur); if (r.rotAnim) out[m.idx] = r;
            if (std::getenv("HSR_ROTDIAG")) {
                // DECISIVE diagnostic (exact eval path): node track frame counts + RAW swept angle of the
                // farthest-from-pivot probe vertex over the SAMPLED period (m.dur) AND over the GLOBAL clip.
                // Reveals whether a real spin is being aliased to a tiny "sway" by a wrong period.
                auto rawSweep=[&](float dur)->std::pair<float,float>{ // {accumulated total, peak-from-frame0} radians
                    int ai=-1; float aR=0; const float* P0=nullptr;
                    std::vector<std::vector<float>> g(NS+1);
                    for (int f=0; f<=NS; f++){ evalAnimNodes(dur*(float)f/(float)NS);
                        Mat4 w=(m.node<nodeWorldAnim.size())?nodeWorldAnim[m.node]:identity(); g[f].resize(m.nv*3);
                        for (size_t v=0; v<m.nv; v++){ float o[3]; xform(w,(*m.base)[v*3],(*m.base)[v*3+1],(*m.base)[v*3+2],o); g[f][v*3]=o[0];g[f][v*3+1]=o[1];g[f][v*3+2]=o[2]; } }
                    P0=g[0].data();
                    for (size_t v=0;v<m.nv;v++){ float d[3]={P0[v*3]-m.pivot[0],P0[v*3+1]-m.pivot[1],P0[v*3+2]-m.pivot[2]}; float rr=d[0]*d[0]+d[1]*d[1]+d[2]*d[2]; if(rr>aR){aR=rr;ai=(int)v;} }
                    if (ai<0) return {0.f,0.f};
                    float ax[3]={0,1,0}; // estimate axis from angular momentum at fastest step
                    { int ti=0; float bv=0; for(int f=0;f<NS;f++){ float dx=g[f+1][ai*3]-g[f][ai*3],dy=g[f+1][ai*3+1]-g[f][ai*3+1],dz=g[f+1][ai*3+2]-g[f][ai*3+2]; float s=dx*dx+dy*dy+dz*dz; if(s>bv){bv=s;ti=f;} }
                      double C[3]={0,0,0}; for(size_t v=0;v<m.nv;v++){C[0]+=g[ti][v*3];C[1]+=g[ti][v*3+1];C[2]+=g[ti][v*3+2];} C[0]/=m.nv;C[1]/=m.nv;C[2]/=m.nv;
                      double aa[3]={0,0,0}; for(size_t v=0;v<m.nv;v++){ float ri[3]={(float)(g[ti][v*3]-C[0]),(float)(g[ti][v*3+1]-C[1]),(float)(g[ti][v*3+2]-C[2])}, vi[3]={g[ti+1][v*3]-g[ti][v*3],g[ti+1][v*3+1]-g[ti][v*3+1],g[ti+1][v*3+2]-g[ti][v*3+2]}; aa[0]+=ri[1]*vi[2]-ri[2]*vi[1];aa[1]+=ri[2]*vi[0]-ri[0]*vi[2];aa[2]+=ri[0]*vi[1]-ri[1]*vi[0]; }
                      float l=(float)std::sqrt(aa[0]*aa[0]+aa[1]*aa[1]+aa[2]*aa[2]); if(l>1e-9f){ax[0]=(float)(aa[0]/l);ax[1]=(float)(aa[1]/l);ax[2]=(float)(aa[2]/l);} }
                    auto sang=[&](const float* a,const float* b){ float da=a[0]*ax[0]+a[1]*ax[1]+a[2]*ax[2], db=b[0]*ax[0]+b[1]*ax[1]+b[2]*ax[2]; float pa[3]={a[0]-da*ax[0],a[1]-da*ax[1],a[2]-da*ax[2]},pb[3]={b[0]-db*ax[0],b[1]-db*ax[1],b[2]-db*ax[2]}; float c[3]={pa[1]*pb[2]-pa[2]*pb[1],pa[2]*pb[0]-pa[0]*pb[2],pa[0]*pb[1]-pa[1]*pb[0]}; return std::atan2(c[0]*ax[0]+c[1]*ax[1]+c[2]*ax[2], pa[0]*pb[0]+pa[1]*pb[1]+pa[2]*pb[2]); };
                    float tot=0,peak=0; float r0[3]={g[0][ai*3]-m.pivot[0],g[0][ai*3+1]-m.pivot[1],g[0][ai*3+2]-m.pivot[2]};
                    for(int f=1;f<=NS;f++){ float pr[3]={g[f-1][ai*3]-m.pivot[0],g[f-1][ai*3+1]-m.pivot[1],g[f-1][ai*3+2]-m.pivot[2]}, cu[3]={g[f][ai*3]-m.pivot[0],g[f][ai*3+1]-m.pivot[1],g[f][ai*3+2]-m.pivot[2]}; tot+=sang(pr,cu); float th=sang(r0,cu); if(std::fabs(th)>std::fabs(peak))peak=th; }
                    return {tot,peak};
                };
                std::string nm = (m.idx<meshes.size())?meshes[m.idx].name:std::string();
                int rn=0,re=0; std::string chain;
                for (int g2=0,n=(int)m.node; n>=0 && n<(int)animNodes.size() && g2<32; n=animNodes[n].parent,++g2){
                    auto it=nodeAnim.find(animNodes[n].name);
                    if(it!=nodeAnim.end()){ const NodeTracks&q=it->second; rn=std::max(rn,q.r.nFrames);re=std::max(re,q.r.effFrames);
                        chain += animNodes[n].name+"(r"+std::to_string(q.r.nFrames)+"/e"+std::to_string(q.r.effFrames)+") "; } }
                auto pNode=rawSweep(m.dur); auto pGlob=rawSweep(globalDur);
                fprintf(stderr,"[ROTDIAG] mesh%zu '%s' node=%u dur=%.2fs(rN=%d/rE=%d) FIT{osc=%d omega=%.3f amp=%.1fdeg per=%.2fs} | rawNode tot=%.1fdeg peak=%.1fdeg | rawGlobal(%.1fs) tot=%.1fdeg peak=%.1fdeg | chain: %s\n",
                    m.idx, nm.c_str(), m.node, m.dur, rn, re, (int)r.isOsc, r.omega, r.amp*57.2958f, r.period, pNode.first*57.2958f, pNode.second*57.2958f, globalDur, pGlob.first*57.2958f, pGlob.second*57.2958f, chain.c_str());
            }
        }
        animate(0.f);   // FULL restore (skinning/UV too) -> leave every mesh at rest for the geometry bake
    }

    // ── COOK: mat.sanim UV-SCROLL port. A UVTrack is a per-frame 2x3 affine [a,b,c,d,e,f]; a continuous SCROLL
    //    (water/foam/waterfall) keeps the 2x2 ~ identity and TRANSLATES (c,f). Derive the VISIBLE scroll rate the
    //    rate = avg per-frame UV delta * fps = UV/second (the source's literal visible speed) -> uvRate for a
    //    getTime() uv += rate*time shader. Flipbook atlases (2x2 = cell scale) are a separate pass -> skipped. ──
    // FAITHFUL UV-scroll velocity (UV/sec) for one track: avg per-frame translation delta * animFps. Returns false
    // for a FLIPBOOK atlas (2x2 != identity) or a still track. This is the SINGLE source of truth used by BOTH the
    // cook (cookExtractUVScroll -> getTime() `uv += rate*time` device shader) AND the renderer's animate(), so the
    // live preview scrolls IDENTICALLY to the cooked device. Direction + speed come straight off the animation track
    // (water/foam/waterfall scroll exactly +1 UV tile; the lake spreads it over 10079 frames = a calm ~0.003 UV/s).
    bool uvScrollRate(const UVTrack& tr, float& ru, float& rv) const {
        ru = rv = 0.f;
        if (tr.nFrames < 2 || animFps <= 0.f) return false;
        const float* M0 = tr.m.data();
        if (std::fabs(M0[0]-1.f)>0.05f || std::fabs(M0[4]-1.f)>0.05f || std::fabs(M0[1])>0.05f || std::fabs(M0[3])>0.05f) return false;  // flipbook atlas, not a continuous scroll
        // SWIRL detection (pond_color_rgCombo "water colors swirling" — user-proven): a UV ROTATION lives in
        // the 2x2 part (a,b,d,e) and builds up over the loop while the translation stays ~0 — every earlier
        // check here (and the frame dumps) only looked at the c,f translations and the FIRST frames, so the
        // swirl track summarized as "scroll 0.001/s" = static. If the 2x2 deviates anywhere along the track,
        // this is NOT a scroll: return false so the mesh routes to the exact per-frame UV-MATRIX REPLAY.
        { int step = tr.nFrames > 64 ? tr.nFrames/64 : 1;
          for (int f=step; f<tr.nFrames; f+=step){ const float* M=&tr.m[(size_t)f*6];
              if (std::fabs(M[0]-M0[0])>0.02f || std::fabs(M[1]-M0[1])>0.02f ||
                  std::fabs(M[3]-M0[3])>0.02f || std::fabs(M[4]-M0[4])>0.02f) return false; } }
        // FULL track window (the rgCombo "water colors swirling" fix): the authored track is a u-translation
        // OSCILLATION 0→0.01→0 over 439 frames; the old 256-frame cap saw mostly the RISING half, so the
        // oscillation gate below never triggered and the slosh cooked as a linear 0.001/s drift (= static).
        // Scanning the whole track costs microseconds and lets the gate see the deltas cancel.
        int win = tr.nFrames - 1;
        double sdu=0, sdv=0; int n=0; double sadu=0, sadv=0;
        for (int f=0; f<win; f++) {
            float dc = tr.m[(size_t)(f+1)*6+2] - tr.m[(size_t)f*6+2];
            float df = tr.m[(size_t)(f+1)*6+5] - tr.m[(size_t)f*6+5];
            if (std::fabs(dc)>0.5f || std::fabs(df)>0.5f) continue;   // skip a wrap-around jump
            sdu += dc; sdv += df; ++n;
            sadu += std::fabs(dc); sadv += std::fabs(df);
        }
        if (n < 1) return false;
        double avgDu = sdu / n, avgDv = sdv / n;
        // OSCILLATION test (pond_color_rgCombo "aint animating" fix): a back-and-forth track (water slosh)
        // has per-frame deltas that CANCEL — the signed mean ≈ 0 while the |delta| mean is real, and cooking
        // that mean as a "scroll" freezes the animation (rate 0.001 UV/s = static). If most of the motion is
        // NOT explained by the mean (|mean| << mean|delta|), this is NOT a constant scroll — return false so
        // the mesh falls through to the exact per-frame UV-MATRIX REPLAY.
        { double meanMag = std::sqrt(avgDu*avgDu + avgDv*avgDv), absMag = (sadu + sadv) / n;
          if (absMag > 1e-6 && meanMag < 0.25 * absMag) return false; }
        if (std::fabs(avgDu) > 0.04 || std::fabs(avgDv) > 0.04) return false;   // cell-stepping flipbook, not a scroll (texture names confirm: *_flipbook / *_spritesheet)
        ru = (float)(avgDu * animFps); rv = (float)(avgDv * animFps);   // UV/sec = the track's AUTHORED visible speed at the base fps
        // FAITHFUL: libshell plays the mat.sanim UVTransform by feeding the per-frame 2x3 matrix into the vertex
        // shader's `BaseTextureMtx[2]` uniform (captured MeshShellEnv_runtime.vert) and computing oBaseTexCoord =
        // BaseTextureMtx * TexCoord -- the matrix is applied DIRECTLY (no inversion) at the track's own rate. So the
        // authored (rate, direction) per track IS the correct flow (waterfall = fast down, fog = slow drift). A prior
        // GLOBAL speed-cap (0.5 UV/s) + direction-negate broke it: it slowed the waterfall to a crawl and flipped it to
        // flow UP. REMOVED -- use the authored rate+direction verbatim. Only the SLOW-TRACK boost stays, to lift the
        // near-frozen lake (10079 frames/30fps = 336s loop) into a visible drift; fast tracks are untouched.
        if (uvMaxLoopSec > 0.f) {
            float natLoop = (float)tr.nFrames / animFps;
            if (natLoop > uvMaxLoopSec) { float s = natLoop / uvMaxLoopSec; ru *= s; rv *= s; }
        }
        return std::sqrt(ru*ru + rv*rv) >= 1e-4f;
    }
    // uvScrollRate once per track (it scans the whole track; animate() asks every frame, and a bake stores the answer).
    bool uvScroll(UVTrack& tr, float& ru, float& rv) const {
        if (tr.scroll < 0) tr.scroll = uvScrollRate(tr, tr.ru, tr.rv) ? 1 : 0;
        ru = tr.ru; rv = tr.rv;
        return tr.scroll == 1;
    }
    void cookExtractUVScroll(std::unordered_map<size_t, std::pair<float,float>>& out) {
        out.clear();
        bool uvdbg = std::getenv("HSR_UVDBG") != nullptr;
        if (uvdbg) fprintf(stderr, "[UVDBG] cookExtractUVScroll: %zu uvAnimRecs animFps=%.2f\n", uvAnimRecs.size(), animFps);
        if (uvAnimRecs.empty() || animFps <= 0.f) return;
        for (auto& ur : uvAnimRecs) {
            if (out.count(ur.meshIdx)) continue;
            auto it = matUVAnim.find(ur.node);
            if (it == matUVAnim.end()) continue;
            float ru, rv;
            if (uvScrollRate(it->second, ru, rv)) {
                out[ur.meshIdx] = std::make_pair(ru, rv);
                if (uvdbg) { const char* nm=(ur.meshIdx<meshes.size())?meshes[ur.meshIdx].name.c_str():"?";
                             const float* MLast = it->second.m.data()+(size_t)(it->second.nFrames-1)*6;
                             fprintf(stderr,"[UVDBG] m%zu '%s' nF=%d trans f0=(%.4f,%.4f) fLast=(%.4f,%.4f) -> SCROLL rate=(%.4f,%.4f)/s\n",
                                     ur.meshIdx,nm,it->second.nFrames,it->second.m[2],it->second.m[5],MLast[2],MLast[5],ru,rv); }
            }
        }
    }
    // FLIPBOOK ATLAS tracks: a mat.sanim UVTransform whose 2x2 is identity but whose
    // per-frame OFFSET steps by ~1/cols (a whole cell) instead of a tiny scroll. uvScrollRate() rejects these (returns
    // false) so they are NOT cooked as a continuous scroll. Here we detect them + derive the grid so the cook routes
    // them to the OFFSET flipbook shader (shadergen::FLIPBOOK, az=1) = frame-SNAP, matching the live preview.
    struct FlipRec { int cols, rows, frames; float fps; };
    void cookExtractFlipbook(std::unordered_map<size_t, FlipRec>& out) {
        out.clear();
        if (uvAnimRecs.empty() || animFps <= 0.f) return;
        bool fdbg = std::getenv("HSR_FLIPDBG") != nullptr;
        for (auto& ur : uvAnimRecs) {
            if (out.count(ur.meshIdx)) continue;
            auto it = matUVAnim.find(ur.node);
            if (it == matUVAnim.end() || it->second.nFrames < 2) continue;
            const UVTrack& tr = it->second;
            const float* M0 = tr.m.data();
            int win = tr.nFrames-1; if (win>256) win=256;
            double sdu=0, sdv=0; int n=0;
            for (int f=0; f<win; f++) {
                float dc = tr.m[(size_t)(f+1)*6+2]-tr.m[(size_t)f*6+2];
                float df = tr.m[(size_t)(f+1)*6+5]-tr.m[(size_t)f*6+5];
                if (std::fabs(dc)>0.5f || std::fabs(df)>0.5f) continue;
                sdu+=dc; sdv+=df; ++n;
            }
            double du = n? std::fabs(sdu/n):0.0, dv = n? std::fabs(sdv/n):0.0;
            if (fdbg) { std::string nm=(ur.meshIdx<meshes.size())?meshes[ur.meshIdx].name:std::string();
                fprintf(stderr,"[FLIPDBG] mesh%zu '%s' node='%s' nF=%d 2x2=[%.3f %.3f; %.3f %.3f] du=%.4f dv=%.4f signed(du=%.4f dv=%.4f)\n",
                    ur.meshIdx, nm.c_str(), ur.node.c_str(), tr.nFrames, M0[0],M0[1],M0[3],M0[4], du,dv, n?sdu/n:0.0, n?sdv/n:0.0); }
            if (std::fabs(M0[0]-1.f)>0.05f || std::fabs(M0[4]-1.f)>0.05f) continue;   // not an identity-offset track
            if (n < 1) continue;
            float ru0,rv0; if (uvScrollRate(tr, ru0, rv0)) continue;   // a CONTINUOUS scroll -> NOT a flipbook (cooked as uvscroll). Only cell-stepping grids remain.
            FlipRec fr;
            fr.cols   = (du>1e-4) ? (int)llround(1.0/du) : 1;  if (fr.cols<1) fr.cols=1;
            fr.frames = tr.nFrames;
            // rows = ROUND(frames/cols), NOT ceil. The fog "flashing" bug: a 15-col / 151-frame sheet is a 15×10 grid
            // (15×10=150 +1 loop-seam frame), but ceil(151/15)=11 made the shader divide the V offset by 11 → every cell
            // landed on the wrong row = vertical smear/flash. round(151/15)=10 = the true row count, so row/rows aligns.
            fr.rows   = (int)llround((double)fr.frames/(double)fr.cols);  if (fr.rows<1) fr.rows=1;
            fr.fps    = animFps;
            out[ur.meshIdx] = fr;
        }
    }

    // ── COOK: EXACT per-frame UV (c,f) OFFSETS from a mat.sanim track (the fog/dust "flashing wrong everything" fix).
    //    Instead of deriving a cols×rows grid from the avg step (which mis-grids a fog_spritesheet that isn't a clean
    //    grid, and MISSES large-step cell jumps where the dust offset moves >0.5/frame), replay the track's ACTUAL
    //    per-frame c,f offsets — exactly what the desktop's animate() flipbook branch applies (uv' = baseUV + (c,f)).
    //    ABSOLUTE offsets (not relative), subsampled to N, frame-snapped on device. loopSec = nFrames/animFps. ──
    // ── COOK: the EXACT per-frame FULL 2x3 UV matrices from a mat.sanim track — the SAME data the desktop's animate()
    //    plays (opa_loader animate() flipbook branch: uv' = M[frame]·baseUV, frame = floor(fract(t/loopSec)·nFrames)).
    //    Pass ALL frames (cap maxN) so the device replays the source VERBATIM (no derived cols/rows grid to mis-guess):
    //    handles continuous scroll, sprite-cell atlases AND 2x2 SCALE (dust) uniformly. loopSec = nFrames/animFps (the
    //    desktop's flipSlow=1 rate). Returns false if the matrix doesn't change across frames (a still track). ──
    bool cookExtractUVMatrices(int meshIdx, int maxN, std::vector<float>& mats, int& N, float& loopSec) {
        mats.clear(); N=0; loopSec=0.f;
        if (maxN < 2 || animFps <= 0.f) return false;
        const UVAnimRec* ar=nullptr; for (auto& a : uvAnimRecs) if ((int)a.meshIdx==meshIdx){ ar=&a; break; }
        if (!ar) return false;
        auto it = matUVAnim.find(ar->node);
        if (it == matUVAnim.end() || it->second.nFrames < 2) return false;
        const UVTrack& tr = it->second; int nf = tr.nFrames;
        loopSec = (float)nf / animFps; if (loopSec <= 1e-4f) return false;
        N = nf <= maxN ? nf : maxN;   // ALL source frames (no cell-skipping subsample) unless enormous
        mats.assign((size_t)N*6, 0.f);
        bool anim=false;
        for (int i=0; i<N; i++) {
            int src = (nf==N) ? i : (int)((double)i * nf / (double)N); if (src>=nf) src=nf-1;
            for (int k=0;k<6;k++){ float v = tr.m[(size_t)src*6+k]; mats[(size_t)i*6+k]=v;
                if (i>0 && std::fabs(v - mats[(size_t)k]) > 1e-5f) anim=true; }
        }
        if (std::getenv("HSR_ROTDUMP")) {   // inspect the UVTransform: rotation vs scroll vs scale/warp
            fprintf(stderr,"[UVDUMP] mesh %d node '%s' frames=%d N=%d loop=%.1fs  (rows: m0 m1 m2 / m3 m4 m5 ; u'=m0u+m1v+m2)\n", meshIdx, ar->node.c_str(), nf, N, loopSec);
            for (int i=0;i<N;i+=std::max(1,N/10)){ float*m=&mats[(size_t)i*6];
                float rot=std::atan2(m[3],m[0])*57.2958f; float sc=std::sqrt(m[0]*m[0]+m[3]*m[3]);
                fprintf(stderr,"  f%3d  [%+.3f %+.3f %+.3f / %+.3f %+.3f %+.3f]  ~rot=%.1fdeg scale=%.3f off=(%+.3f,%+.3f)\n",
                    i,m[0],m[1],m[2],m[3],m[4],m[5], rot, sc, m[2],m[5]); }
        }
        return anim;   // the matrix actually changes across frames
    }

    // ── Per-clip frame-rate READ FROM THE COOKED DATA, not hardcoded ─────────────────────────
    // libshell pulls the rate from a NAMED "FrameRate" field (driver sub_2EAEF5C does
    // sub_2EF36E0(asset,"FrameRate") -> f32). We do the same: scan for the OPAA name record
    // [0xFFFF][u16 len=9]["FrameRate"] and take the f32 right after. Returns <=0 when the cook
    // stores no rate -> the caller keeps the engine default (many V79 .sanim/.mat.sanim cooks
    // store NO rate field at all and are sampled at the engine's fixed cook rate).
    static float findFrameRate(const std::vector<uint8_t>& d) {
        static const char K[9] = {'F','r','a','m','e','R','a','t','e'};
        if (d.size() < 21) return -1.0f;
        for (size_t i = 0; i + 17 <= d.size(); ++i) {
            if (d[i] != 0xFF || d[i+1] != 0xFF) continue;
            uint16_t ln; memcpy(&ln, d.data()+i+2, 2);
            if (ln != 9 || memcmp(d.data()+i+4, K, 9) != 0) continue;
            float f; memcpy(&f, d.data()+i+13, 4);
            if (f > 0.5f && f < 480.0f) return f;       // sane fps only
        }
        return -1.0f;
    }

    // Evaluate ONLY the animated node hierarchy at time t -> nodeWorldAnim (NO mesh deform, NO skinning/UV/tint).
    // The cook rotation sampler calls this per frame instead of the full animate() (which skins/UV-transforms EVERY
    // mesh -> far too slow over 33 frames on a large environment).
    void evalAnimNodes(float t) {
        nodeWorldAnim.resize(animNodes.size());
        for (size_t i = 0; i < animNodes.size(); ++i) {
            const Node& nd = animNodes[i];
            float T[3]={nd.t[0],nd.t[1],nd.t[2]};
            float R[4]={nd.r[0],nd.r[1],nd.r[2],nd.r[3]};  // static R already (x,y,z,w)
            float S[3]={nd.s[0],nd.s[1],nd.s[2]};
            auto it = nodeAnim.find(nd.name);
            if (it != nodeAnim.end()) {
                const NodeTracks& nt = it->second;
                sampleTrack(nt.t, t, animFps, 3, T);
                sampleTrack(nt.r, t, animFps, 4, R);
                sampleTrack(nt.s, t, animFps, 3, S);
                if (nt.r.nFrames > 0) { float qw=R[0],qx=R[1],qy=R[2],qz=R[3]; R[0]=qx; R[1]=qy; R[2]=qz; R[3]=qw; }
                float ql = sqrtf(R[0]*R[0]+R[1]*R[1]+R[2]*R[2]+R[3]*R[3]);
                if (ql > 1e-6f) { R[0]/=ql; R[1]/=ql; R[2]/=ql; R[3]/=ql; } else { R[0]=R[1]=R[2]=0; R[3]=1; }
            }
            Mat4 local = trs(T, R, S);
            int par = nd.parent;
            nodeWorldAnim[i] = (par >= 0 && par < (int)i) ? mul(nodeWorldAnim[par], local) : local;
        }
    }

    static void sampleTrack(const Track& tr, float t, float fps, int comps, float* out) {
        if (tr.nFrames <= 0 || tr.comps != comps) return;          // leave caller default
        // Each track loops on its OWN length at the engine rate (NOT stretched to the global longest
        // clip — with a 2501-frame bird path that made a short fan-rotation loop crawl over 83s).
        float f = fmodf(t * fps, (float)tr.nFrames);
        if (f < 0.0f) f += (float)tr.nFrames;
        int i0 = (int)f; float frac = f - (float)i0;
        if (i0 >= tr.nFrames) { i0 = tr.nFrames - 1; frac = 0.0f; }
        int i1 = (i0 + 1 < tr.nFrames) ? i0 + 1 : 0;               // wrap to frame 0 -> smooth loop seam
        // SHORTEST-ARC quaternion lerp (see gltf sampleSampler): antipodal neighbour keys (q vs -q) lerped
        // componentwise pass near |q|=0 = a wild long-arc sweep for that interval, baked into cooked clips.
        float sgn = 1.f;
        if (comps == 4) { float d = 0.f;
            for (int c = 0; c < 4; ++c) d += tr.v[(size_t)i0*comps + c] * tr.v[(size_t)i1*comps + c];
            if (d < 0.f) sgn = -1.f; }
        for (int c = 0; c < comps; ++c)
            out[c] = tr.v[(size_t)i0*comps + c] * (1.0f - frac) + tr.v[(size_t)i1*comps + c] * sgn * frac;
    }
    // Sample each animated node at looped time t and rewrite its mesh's world positions.
    void animate(float t) {
        if (!hasAnimation()) return;
        // ── Evaluate the WHOLE node hierarchy with animation (libshell's scene graph): for each node
        //    world[i] = world[parent] * localTRS(i), where localTRS uses the sanim-sampled TRS when the
        //    node is keyed, else the node's STATIC local transform. This makes a mesh move when ANY
        //    ANCESTOR is animated (e.g. the bird mesh under the animated `birdBody_path` node) — not
        //    only when its own node is keyed. (Nodes are stored parent-before-child, so one pass.)
        if (gpuRigid_ && !animRecs.empty() && animRecs[0].rigid < 0) {
            std::set<size_t> cpu;   // meshes something else also writes
            for (auto& sr : skinRecs) cpu.insert(sr.meshIdx);
            for (auto& vr : vatRecs) cpu.insert(vr.meshIdx);
            evalAnimNodes(0.0f);
            for (auto& ar : animRecs) {
                ar.rest = (ar.nodeIdx < nodeWorldAnim.size()) ? nodeWorldAnim[ar.nodeIdx] : identity();
                const float* M = ar.rest.m;
                const float det = M[0]*(M[5]*M[10]-M[6]*M[9]) - M[4]*(M[1]*M[10]-M[2]*M[9]) + M[8]*(M[1]*M[6]-M[2]*M[5]);
                ar.rigid = (std::fabs(det) > 1e-9f && !cpu.count(ar.meshIdx)) ? 1 : 0;
                mat4affineInverse(M, ar.restInv);
            }
        }
        if (!animRecs.empty()) evalAnimNodes(t);   // node hierarchy -> nodeWorldAnim (shared with the cook sampler)
        for (auto& ar : animRecs) {
            Mat4 m = (ar.nodeIdx < nodeWorldAnim.size()) ? nodeWorldAnim[ar.nodeIdx] : identity();
            MeshData& md = meshes[ar.meshIdx];
            size_t nv = ar.basePos.size() / 3;
            static int birddbg = -1; if (birddbg<0) birddbg = std::getenv("HSR_BIRDDBG")?1:0;
            if (birddbg && nv>0) {
                float bmn[3]={1e9f,1e9f,1e9f}, bmx[3]={-1e9f,-1e9f,-1e9f};
                for (size_t i=0;i<nv;i++) for(int c=0;c<3;c++){float v=ar.basePos[i*3+c]; if(v<bmn[c])bmn[c]=v; if(v>bmx[c])bmx[c]=v;}
                fprintf(stderr,"[BIRDDBG] mesh#%zu node=%u  nodeWt=(%.2f,%.2f,%.2f)  baseAABB=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f)  diag=%.2f\n",
                    ar.meshIdx, ar.nodeIdx, m.m[12],m.m[13],m.m[14], bmn[0],bmn[1],bmn[2], bmx[0],bmx[1],bmx[2],
                    std::sqrt((bmx[0]-bmn[0])*(bmx[0]-bmn[0])+(bmx[1]-bmn[1])*(bmx[1]-bmn[1])+(bmx[2]-bmn[2])*(bmx[2]-bmn[2])));
            }
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            const bool rigid = gpuRigid_ && ar.rigid == 1;
            if (rigid && ar.atRest) { mat4mul(m.m, ar.restInv, md.animXform); continue; }
            const Mat4& at = rigid ? ar.rest : m;
            bool changed = false;
            for (size_t i = 0; i < nv; ++i) {
                float o[3]; xform(at, ar.basePos[i*3], ar.basePos[i*3+1], ar.basePos[i*3+2], o);
                changed |= put(md.positions[i*3], o[0]) | put(md.positions[i*3+1], o[1]) | put(md.positions[i*3+2], o[2]);
            }
            if (changed) ++md.posGen;
            ar.atRest = rigid;
            if (rigid) mat4mul(m.m, ar.restInv, md.animXform);
            else std::memcpy(md.animXform, identity().m, sizeof md.animXform);
        }
        // ── Skeletal LBS (skinned meshes) — libshell's vertex shader, on the CPU:
        //    localPos = Σ_i ( Joints[idx_i] * bindPos ) * weight_i   (i = 0..3) ─────────────────
        // The .anim + .skel store LOCAL (parent-relative) joint poses. For the current frame:
        //   animWorld[j] = animWorld[parent[j]] * animLocal(f)[j]   (parent[j] < j, single pass)
        //   Joints[j]    = animWorld[j] * invBind[j]                (invBind = inverse bind WORLD)
        // (matches V79 libshell Skeleton.cpp: m_jointParents + m_jointLocalPoses).
        auto& clipSkin = _clipSkin; clipSkin.resize(clips.size());   // reused scratch (capacity kept)
        for (size_t ci = 0; ci < clips.size(); ++ci) {
            const AnimClip& clip = clips[ci];
            if (clip.numFrames < 1 || (int)clip.parents.size() < clip.numJoints) { clipSkin[ci].clear(); continue; }
            // Interpolate BETWEEN baked frames (like sampleTrack does for sanim). Snapping to an
            // integer frame made skeletal anims step at the baked fps = stutter (the chicken looked
            // smooth only because it rides the interpolating sanim path). Dense baked frames => small
            // per-frame delta => element-wise LERP of the LOCAL joint matrices is visually smooth.
            // Each clip loops on its OWN length at the engine sample rate (animFps), independent of
            // other clips — NOT stretched to the global longest-clip duration (that desynced/slowed
            // shorter clips). i1 wraps to frame 0 so the loop seam interpolates smoothly.
            float f = fmodf(t * animFps, (float)clip.numFrames);
            if (f < 0.0f) f += (float)clip.numFrames;
            int i0 = (int)f; float frac = f - (float)i0;
            if (i0 < 0) { i0 = 0; frac = 0.0f; }
            if (i0 >= clip.numFrames) { i0 = clip.numFrames - 1; frac = 0.0f; }
            int i1 = (i0 + 1 < clip.numFrames) ? i0 + 1 : 0;
            const float* fm0 = clip.mats.data() + (size_t)i0 * clip.numJoints * 16;
            const float* fm1 = clip.mats.data() + (size_t)i1 * clip.numJoints * 16;
            // Joints[j] = jointWORLD = compose(animLocal, parents). The SkinnedPos verts are
            // authored joint-LOCAL (near origin), so jointWorld places them onto the building +
            // poses them — NO inverse-bind (that would cancel the placement -> owl back at origin).
            clipSkin[ci].resize((size_t)clip.numJoints*16);
            for (int j = 0; j < clip.numJoints; ++j) {
                float L[16];
                for (int k = 0; k < 16; ++k)
                    L[k] = fm0[(size_t)j*16+k]*(1.0f-frac) + fm1[(size_t)j*16+k]*frac;
                int p = clip.parents[j];
                if (p < 0 || p >= j) memcpy(clipSkin[ci].data()+(size_t)j*16, L, 16*sizeof(float));
                else mat4mul(clipSkin[ci].data()+(size_t)p*16, L, clipSkin[ci].data()+(size_t)j*16);
            }
        }
        // Each skinned mesh on its own core (the meshes are independent unless two records share one).
        if (skinShared_ < 0) {
            std::set<size_t> seen;
            skinShared_ = 0;
            for (auto& sr : skinRecs) if (!seen.insert(sr.meshIdx).second) skinShared_ = 1;
        }
        auto skin = [&](size_t si) {
            const SkinRec& sr = skinRecs[si];
            if (sr.clipIdx < 0 || sr.clipIdx >= (int)clips.size()) return;
            const AnimClip& clip = clips[sr.clipIdx];
            if (clipSkin[sr.clipIdx].empty() || sr.nJoints < 1) return;
            const float* cw = clipSkin[sr.clipIdx].data();   // clip joint WORLD matrices (composed)
            // Per skin-bone skinning matrix = clipJointWorld[boneClip[bone]] * invBind[bone].
            // (invBind = the SKIN's per-bone mesh-bind->joint-local; jointWorld places + poses.)
            static thread_local std::vector<float> sm; sm.resize((size_t)sr.nJoints*16);   // reused scratch (capacity kept)
            for (int b = 0; b < sr.nJoints; ++b) {
                int cj = (b < (int)sr.boneClip.size()) ? sr.boneClip[b] : -1;
                if (cj < 0 || cj >= clip.numJoints || (size_t)(b*16+16) > sr.invBind.size()) {
                    for (int k=0;k<16;++k) sm[b*16+k] = (k%5==0)?1.f:0.f; continue;
                }
                mat4mul(cw + (size_t)cj*16, sr.invBind.data()+(size_t)b*16, sm.data()+(size_t)b*16);
            }
            MeshData& md = meshes[sr.meshIdx];
            if (gpuRigid_ && sr.gpu) {                     // the renderer skins it: its joints and the box they move
                const size_t nj = (size_t)sr.nJoints * 16;
                if (md.skinJoints.size() == nj && std::memcmp(md.skinJoints.data(), sm.data(), nj * sizeof(float)) == 0) return;
                md.skinJoints.assign(sm.begin(), sm.begin() + nj);
                float box[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
                for (int b = 0; b < sr.nJoints; ++b) {
                    const float* jb = &sr.jointBox[(size_t)b*6];
                    if (jb[0] > jb[3]) continue;                 // moves no vertex
                    const float* M = sm.data() + (size_t)b*16;
                    for (int c = 0; c < 8; ++c) {
                        const float x = jb[(c&1)?3:0], y = jb[(c&2)?4:1], z = jb[(c&4)?5:2];
                        const float o[3] = {M[0]*x + M[4]*y + M[8]*z + M[12], M[1]*x + M[5]*y + M[9]*z + M[13], M[2]*x + M[6]*y + M[10]*z + M[14]};
                        for (int k = 0; k < 3; ++k) { box[k] = std::min(box[k], o[k]); box[3+k] = std::max(box[3+k], o[k]); }
                    }
                }
                for (int k = 0; k < 3; ++k) { box[k] = std::min(box[k], sr.unriggedBox[k]); box[3+k] = std::max(box[3+k], sr.unriggedBox[3+k]); }
                std::memcpy(md.skinBox, box, sizeof box);
                ++md.skinGen;
                return;
            }
            size_t nv = sr.basePos.size() / 3;
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            bool changed = false;
            for (size_t v = 0; v < nv; ++v) {
                float bx=sr.basePos[v*3], by=sr.basePos[v*3+1], bz=sr.basePos[v*3+2];
                float ox=0, oy=0, oz=0, wsum=0;
                for (int i = 0; i < 4; ++i) {
                    float w = sr.jw[v*4+i]; int b = sr.jidx[v*4+i];
                    if (w <= 0.0f || b < 0 || b >= sr.nJoints) continue;
                    const float* M = sm.data() + (size_t)b*16;   // column-major skinning matrix
                    ox += w * (M[0]*bx + M[4]*by + M[8]*bz + M[12]);
                    oy += w * (M[1]*bx + M[5]*by + M[9]*bz + M[13]);
                    oz += w * (M[2]*bx + M[6]*by + M[10]*bz + M[14]);
                    wsum += w;
                }
                if (wsum < 1e-4f) { ox=bx; oy=by; oz=bz; }            // unrigged vertex -> bind pose
                else if (wsum < 0.999f || wsum > 1.001f) { ox/=wsum; oy/=wsum; oz/=wsum; }  // normalize (weights not guaranteed to sum to 1)
                changed |= put(md.positions[v*3], ox) | put(md.positions[v*3+1], oy) | put(md.positions[v*3+2], oz);
            }
            if (changed) ++md.posGen;
        };
        if (skinShared_) for (size_t si = 0; si < skinRecs.size(); ++si) skin(si);
        else par::forEach(skinRecs.size(), skin);
        // ── mat.sanim UV/flipbook: transform each animated mesh's base UVs by the current frame's
        //    2x3 matrix [a,b,c, d,e,f]: uv' = (a*u + b*v + c, d*u + e*v + f). ──────────────────────
        for (auto& ur : uvAnimRecs) {
            auto it = matUVAnim.find(ur.node);
            if (it == matUVAnim.end() || it->second.nFrames < 1) continue;
            UVTrack& tr = it->second;
            MeshData& md = meshes[ur.meshIdx];
            size_t nuv = ur.baseUV.size() / 2;
            if (md.uvs.size() < nuv*2) md.uvs.resize(nuv*2);
            float ru, rv;
            bool isScroll = uvScroll(tr, ru, rv);
            // FAITHFUL per-animation speed (V79 AnimationLayer sub_1BC7118: phase += speed*dt/clipDuration —
            // EACH clip, node OR texture, advances on its OWN duration/speed). The UV clip's authored
            // duration = its own frame count / the sanim FrameRate. NO artificial cap/stretch (uvMaxLoopSec/
            // flipSlow distorted the authored speed = the "moves too fast / doesn't follow" bug). Sync with
            // the node movement + tint fade is EMERGENT (fog authors node/UV/tint at matching durations),
            // exactly like the device — not forced. Loop = fract (recycle mode!=0 = repeat, sub_1BC70B8).
            float clipSec = (animFps > 0.f) ? (float)tr.nFrames / animFps : 0.f;
            static int legacyAnim = -1; if (legacyAnim<0) legacyAnim = std::getenv("HSR_LEGACYANIM")?1:0;
            if (clipSec > 1e-4f && !legacyAnim) {
                float phase = fmodf(t / clipSec, 1.0f) * (float)tr.nFrames;
                if (phase < 0.0f) phase += (float)tr.nFrames;
                int f0 = (int)phase; if (f0 >= tr.nFrames) f0 = tr.nFrames - 1; if (f0 < 0) f0 = 0;
                int f1 = (f0 + 1 < tr.nFrames) ? f0 + 1 : 0;
                float frac = phase - (float)f0; if (frac < 0.f) frac = 0.f; if (frac > 1.f) frac = 1.f;
                const float* M0 = tr.m.data() + (size_t)f0 * 6;
                const float* M1 = tr.m.data() + (size_t)f1 * 6;
                // LERP a continuous scroll/warp (smooth flow); SNAP a sprite-atlas flipbook (lerp = double image).
                float M[6];
                if (isScroll) for (int k=0;k<6;k++) M[k] = M0[k]*(1.0f-frac) + M1[k]*frac;
                else          for (int k=0;k<6;k++) M[k] = M0[k];
                if (!uvByMatrix(ur, md, nuv, M)) {
                    bool changed = false;
                    for (size_t i = 0; i < nuv; ++i) {
                        float u0 = ur.baseUV[i*2], v0 = ur.baseUV[i*2+1];
                        changed |= put(md.uvs[i*2], M[0]*u0 + M[1]*v0 + M[2]);
                        changed |= put(md.uvs[i*2+1], M[3]*u0 + M[4]*v0 + M[5]);
                    }
                    if (changed) ++md.uvGen;
                }
                static int matdbg = -1; if (matdbg<0) matdbg = std::getenv("HSR_MATDBG")?1:0;
                if (matdbg && nuv>0) fprintf(stderr, "[MATDBG] t=%.2f mesh#%zu '%s' CLIP fr=%d/%d clip=%.2fs %s off=(%.3f,%.3f) uv0=(%.4f,%.4f)\n",
                    t, ur.meshIdx, ur.node.c_str(), f0, tr.nFrames, clipSec, isScroll?"scroll":"atlas", M[2],M[5], md.uvs[0], md.uvs[1]);
                continue;
            }
            if (isScroll) {
                // CONTINUOUS linear UV scroll: effect tracks use an IDENTITY 2x2 plus a constant-velocity offset, e.g.
                // fog dc=0.1/fr & smoke dc=0.125/fr — NOT atlas flipbooks). The faithful play is uv = baseUV + rate*t
                // (UNBOUNDED; the texture REPEAT wrap makes it seamless), which is EXACTLY the device cook's getTime()
                // `uv += rate*time` shader (the SAME uvScrollRate), so the preview matches the headset. Frame-sampling
                // a capped loop instead snaps the UV back to frame0 each loop -> a visible JUMP for the non-integer-tile
                // fog/smoke (fog ends at u=9.9 = a 0.9 backward snap; smoke at 15.875) = the "not animated right" bug.
                // WRAP the offset into one tile (fmod): t is a free-running wall clock, so an UNBOUNDED rate*t grows to
                // hundreds of UV units -> float32 loses sub-texel precision in the GPU interpolator = shimmer/FLASH.
                // fmod keeps it in (-1,1); the texture REPEAT makes the 1->0 wrap seamless (the whole card shifts by a
                // uniform offset, so no torn triangle, no mip change). Device cook scrolls the same rate continuously.
                float du = std::fmod(ru * t, 1.0f), dv = std::fmod(rv * t, 1.0f);
                const float S[6] = {1, 0, du, 0, 1, dv};
                if (!uvByMatrix(ur, md, nuv, S)) {
                    bool changed = false;
                    for (size_t i = 0; i < nuv; ++i) {
                        changed |= put(md.uvs[i*2], ur.baseUV[i*2]   + du);
                        changed |= put(md.uvs[i*2+1], ur.baseUV[i*2+1] + dv);
                    }
                    if (changed) ++md.uvGen;
                }
                static int matdbg = -1; if (matdbg<0) matdbg = std::getenv("HSR_MATDBG")?1:0;
                if (matdbg && nuv>0) fprintf(stderr, "[MATDBG] t=%.2f mesh#%zu '%s' SCROLL rate=(%.4f,%.4f) uv0=(%.4f,%.4f)\n",
                    t, ur.meshIdx, ur.node.c_str(), ru, rv, md.uvs[0], md.uvs[1]);
            } else {
                // FLIPBOOK ATLAS (both scale-flipbooks and effect tracks which step
                // an identity-matrix UV by ~1/cols per frame): the texture is a sprite GRID, the track flips cells.
                // SNAP to the integer frame (interpolating would slide across a cell boundary = blend two sprites =
                // the smeared "all messed" waterfall). flipSlow stretches the loop so the flipbook reads as flowing
                // water, not a frantic flicker (waterfall 64 frames @30fps = 2.1s was "way too fast"); HSR_FLIPSLOW tunes.
                static float flipSlow = -1.f; if (flipSlow < 0.f) { const char* e = std::getenv("HSR_FLIPSLOW"); flipSlow = e ? (float)atof(e) : 1.0f; }
                float loopSec = (animFps > 0.f) ? (float)tr.nFrames / animFps * flipSlow : 0.f;
                if (uvMaxLoopSec > 0.f && loopSec > uvMaxLoopSec) loopSec = uvMaxLoopSec;
                float phase = (loopSec > 1e-4f) ? fmodf(t / loopSec, 1.0f) * (float)tr.nFrames
                                                : fmodf(t * animFps, (float)tr.nFrames);
                if (phase < 0.0f) phase += (float)tr.nFrames;
                int frame = (int)phase;
                if (frame < 0) frame = 0; if (frame >= tr.nFrames) frame = tr.nFrames - 1;
                const float* M = tr.m.data() + (size_t)frame * 6;
                if (!uvByMatrix(ur, md, nuv, M)) {
                    bool changed = false;
                    for (size_t i = 0; i < nuv; ++i) {
                        float u0 = ur.baseUV[i*2], v0 = ur.baseUV[i*2+1];
                        changed |= put(md.uvs[i*2], M[0]*u0 + M[1]*v0 + M[2]);
                        changed |= put(md.uvs[i*2+1], M[3]*u0 + M[4]*v0 + M[5]);
                    }
                    if (changed) ++md.uvGen;
                }
                static int matdbg = -1; if (matdbg<0) matdbg = std::getenv("HSR_MATDBG")?1:0;
                if (matdbg && nuv>0) fprintf(stderr, "[MATDBG] t=%.2f mesh#%zu '%s' FLIP fr=%d/%d M=[%.3f %.3f] uv0=(%.4f,%.4f)\n",
                    t, ur.meshIdx, ur.node.c_str(), frame, tr.nFrames, M[2], M[5], md.uvs[0], md.uvs[1]);
            }
        }
        // ── mat.sanim MaterialTint: per-frame RGBA the shader multiplies into the fragment
        //    (UniformColor). UNLIKE the UV flipbook this is a smooth opacity fade, so LERP between
        //    frames (snapping would make the fog visibly step). This is the fog/dust/flicker
        //    OPACITY (alpha 0..~0.22) — without it fog renders far too dense. ───────────────────────
        for (auto& tr : tintRecs) {
            auto it = matTintAnim.find(tr.node);
            if (it == matTintAnim.end() || it->second.nFrames < 1) continue;
            const TintTrack& tt = it->second;
            if (std::getenv("HSR_TINTDBG")) { static std::set<std::string> seen;
                if (!seen.count(tr.node)) { seen.insert(tr.node);
                    float amn=1e9f,amx=-1e9f; for(int f=0;f<tt.nFrames;f++){float a=tt.rgba[(size_t)f*4+3]; if(a<amn)amn=a; if(a>amx)amx=a;}
                    auto A=[&](float p){int f=(int)(p*(tt.nFrames-1)+0.5f); return tt.rgba[(size_t)f*4+3];};
                    fprintf(stderr,"[TINTDBG] '%s' nf=%d alpha[min=%.3f max=%.3f]  @0=%.3f @25=%.3f @50=%.3f @75=%.3f @100=%.3f\n",
                        tr.node.c_str(), tt.nFrames, amn,amx, A(0),A(.25f),A(.5f),A(.75f),A(1)); } }
            static int legacyAnimT = -1; if (legacyAnimT<0) legacyAnimT = std::getenv("HSR_LEGACYANIM")?1:0;
            float loopSecT;
            if (animFps > 0.f && tt.nFrames > 1 && !legacyAnimT) {
                loopSecT = (float)tt.nFrames / animFps;  // FAITHFUL: the fade plays at ITS OWN authored duration/speed (no 5s cap) — sub_1BC7118 per-clip rate
            } else {
                static float matLoopMaxT = -1.f;
                if (matLoopMaxT < 0.f) { const char* e = std::getenv("HSR_MATLOOP"); matLoopMaxT = e ? (float)atof(e) : 5.0f; }
                loopSecT = (animFps > 0.f) ? (float)tt.nFrames / animFps : 0.f;
                if (matLoopMaxT > 0.f && loopSecT > matLoopMaxT) loopSecT = matLoopMaxT;
            }
            float phase = (loopSecT > 1e-4f) ? fmodf(t / loopSecT, 1.0f) * (float)tt.nFrames
                                             : fmodf(t * animFps, (float)tt.nFrames);
            if (phase < 0.0f) phase += (float)tt.nFrames;
            int f0 = (int)phase; float frac = phase - (float)f0;
            if (f0 >= tt.nFrames) { f0 = tt.nFrames - 1; frac = 0.0f; }
            int f1 = (f0 + 1 < tt.nFrames) ? f0 + 1 : 0;     // wrap for a seamless loop
            const float* a = tt.rgba.data() + (size_t)f0 * 4;
            const float* b = tt.rgba.data() + (size_t)f1 * 4;
            MeshData& md = meshes[tr.meshIdx];
            for (int c = 0; c < 4; ++c) md.curTint[c] = a[c]*(1.0f-frac) + b[c]*frac;
        }
        // ── VAT (Vertex Animation Texture): localPos = basePos + offset[frame][col], then place by
        //    the instance world matrix. Offsets are LERP'd between frames (loops on its own length
        //    at vatFps). This is the coral/seaweed/fish/jellyfish sway. ────────────────────────────
        for (auto& vr : vatRecs) {
            if (!vr.vd || vr.vd->frames < 1 || vr.vd->cols < 1) continue;
            const VatData& vd = *vr.vd;
            float f = fmodf(t * vatFps, (float)vd.frames); if (f < 0.0f) f += (float)vd.frames;
            int i0 = (int)f; float frac = f - (float)i0;
            if (i0 < 0) i0 = 0; if (i0 >= vd.frames) { i0 = vd.frames - 1; frac = 0.0f; }
            int i1 = (i0 + 1 < vd.frames) ? i0 + 1 : 0;       // wrap for a seamless loop
            MeshData& md = meshes[vr.meshIdx];
            size_t nv = vr.basePos.size() / 3;
            if (md.positions.size() < nv*3) md.positions.resize(nv*3);
            bool changed = false;
            for (size_t v = 0; v < nv; ++v) {
                int col = vr.col[v]; if (col < 0 || col >= vd.cols) col = 0;
                const float* o0 = &vd.off[((size_t)i0*vd.cols + col)*3];
                const float* o1 = &vd.off[((size_t)i1*vd.cols + col)*3];
                float lx = vr.basePos[v*3]   + o0[0]*(1.0f-frac) + o1[0]*frac;
                float ly = vr.basePos[v*3+1] + o0[1]*(1.0f-frac) + o1[1]*frac;
                float lz = vr.basePos[v*3+2] + o0[2]*(1.0f-frac) + o1[2]*frac;
                float wp[3]; xform(vr.world, lx, ly, lz, wp);
                changed |= put(md.positions[v*3], wp[0]) | put(md.positions[v*3+1], wp[1]) | put(md.positions[v*3+2], wp[2]);
            }
            if (changed) ++md.posGen;
        }
    }

    // Parse sanim.opa -> nodeAnim (called before parseModel so it can mark animated meshes).
    void loadAnim(const std::vector<uint8_t>& sceneZip) {
        std::vector<uint8_t> sa;
        { mz_zip_archive z; memset(&z, 0, sizeof(z));
          if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
          uint32_t nf = mz_zip_reader_get_num_files(&z); int found = -1;
          for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            // Match the NODE-animation file (...fbx.sanim.opa) but NOT ...fbx.mat.sanim.opa, which
            // ALSO ends in ".sanim.opa" — grabbing that (UV flipbook tracks, no Translation/Rotation)
            // gave "0 animated nodes" so the bird's flight + windmill fans never animated.
            bool isMat = fn.size() >= 14 && fn.compare(fn.size()-14, 14, ".mat.sanim.opa") == 0;
            if (!isMat && fn.size() >= 10 && fn.compare(fn.size()-10, 10, ".sanim.opa") == 0) { found = (int)i; break; } }
          if (found < 0) { mz_zip_reader_end(&z); return; }
          size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, found, &sz, 0);
          mz_zip_reader_end(&z);
          if (!d) return; sa.assign((uint8_t*)d, (uint8_t*)d + sz); mz_free(d); }
        if (sa.size() < 52 || memcmp(sa.data(), "OPAA", 4) != 0) return;
        { float fr = findFrameRate(sa); if (fr > 0.f) { animFps = fr; fprintf(stderr, "[OPA] sanim FrameRate (from data) = %.2f\n", fr); } }
        uint32_t hdr; memcpy(&hdr, sa.data()+16, 4); if (hdr < 48 || hdr >= sa.size()) hdr = 48;
        auto rdName = [&](size_t off, std::string& out) -> size_t {
            if (off + 4 > sa.size() || sa[off] != 0xFF || sa[off+1] != 0xFF) return 0;
            uint16_t ln; memcpy(&ln, sa.data()+off+2, 2);
            if (ln == 0 || ln > 64 || off + 4 + ln > sa.size()) return 0;
            for (uint16_t k = 0; k < ln; ++k) { uint8_t ch = sa[off+4+k]; if (ch < 32 || ch > 126) return 0; }
            out.assign((char*)sa.data()+off+4, ln); return 4u + ln; };
        std::string curNode; size_t p = hdr + 4;
        while (p + 4 <= sa.size()) {
            std::string nm; size_t adv = rdName(p, nm);
            if (!adv) { ++p; continue; }
            if (nm == "Translation" || nm == "Rotation" || nm == "Scale") {
                size_t kp = p + adv;
                if (kp + 12 > sa.size()) { p += adv; continue; }
                uint32_t nKeys, nVals;
                memcpy(&nKeys, sa.data()+kp+4, 4); memcpy(&nVals, sa.data()+kp+8, 4);
                if (std::getenv("HSR_SANIMDBG")) { uint32_t f0; memcpy(&f0, sa.data()+kp, 4);
                    fprintf(stderr,"[SANIM] node='%s' ch=%s hdr@kp[+0]=0x%08X(%u) nKeys=%u nVals=%u  bytes[+0..+3]=%02X %02X %02X %02X\n",
                        curNode.c_str(), nm.c_str(), f0, f0, nKeys, nVals, sa[kp],sa[kp+1],sa[kp+2],sa[kp+3]); }
                // comps come from the CHANNEL, not nKeys: Translation/Scale=3, Rotation=4 (quat); the
                // value block is nVals floats right after the 12B header. Deriving frames from nKeys+1
                // is wrong for flag=1 tracks (the bird's path: nVals=6000, nKeys+1=2001 -> 2001*3≠6000,
                // so it was rejected and the bird never moved). frames = nVals/comps handles both flags.
                int comps = (nm == "Rotation") ? 4 : 3;
                size_t trackEnd = kp + 12 + (size_t)nVals*4;
                if (trackEnd <= sa.size() && nVals >= (uint32_t)comps && (nVals % (uint32_t)comps) == 0) {
                    int nFrames = (int)(nVals / (uint32_t)comps);
                    if (!curNode.empty()) {
                        Track tr; tr.comps = comps; tr.nFrames = nFrames; tr.v.resize(nVals);
                        memcpy(tr.v.data(), sa.data()+kp+12, (size_t)nVals*4);
                        // Recover the authored motion range. Some cooked tracks store the real
                        // keyframes then HOLD the final value to pad out to the master clip length
                        // (e.g. bluehillgoldmine's background train: 625 frames crossing + ~1875
                        // identical "parked" frames). Looping the full track makes the prop sit
                        // still for most of the cycle ("train frozen"). Drop a pure trailing run of
                        // frames identical to the last keyframe so the loop covers only the motion.
                        {
                            // Per-channel trailing-static length (where THIS channel stops changing).
                            // Stored, NOT applied yet — we sync a node's T/R/S together below so they
                            // don't drift out of phase over loops.
                            const float* vv = tr.v.data();
                            auto same = [&](int A,int B){ for(int c=0;c<comps;++c) if (fabsf(vv[(size_t)A*comps+c]-vv[(size_t)B*comps+c])>1e-5f) return false; return true; };
                            int eff = tr.nFrames;
                            while (eff > 2 && same(eff-1, eff-2)) --eff;
                            tr.effFrames = eff;
                        }
                        NodeTracks& ntk = nodeAnim[curNode];
                        if (nm == "Translation") ntk.t = std::move(tr);
                        else if (nm == "Rotation") ntk.r = std::move(tr);
                        else ntk.s = std::move(tr);
                    }
                    p = trackEnd; continue;   // resync (byte-scan) handles any flag=1 trailing key-time data
                }
                p += adv; continue;   // bogus header -> resync by scanning for the next name marker
            }
            if (nm == "UVTransform") {
                // Per-frame 2x3 UV affine [a,b,c, d,e,f] (6 floats/frame) authored INSIDE the node
                // sanim — the material UV scroll / flipbook (fire, smoke, aurora, water). The old
                // parser handled ONLY Translation/Rotation/Scale and silently DROPPED UVTransform,
                // so every such material rendered STATIC ("animations left out": winterwonderland
                // smoke_Geo*, fire_a_placement_*, aurora_noise, pSphere*). Feed matUVAnim[node] so the
                // SAME UV-anim path the .mat.sanim.opa uses (renderer animate() + cook uvscroll/flipbook)
                // drives them — GLOBAL, one code path for every env whose sanim carries UVTransform.
                size_t kp = p + adv;
                if (kp + 12 > sa.size()) { p += adv; continue; }
                uint32_t nKeys, nVals;
                memcpy(&nKeys, sa.data()+kp+4, 4); memcpy(&nVals, sa.data()+kp+8, 4); (void)nKeys;
                size_t trackEnd = kp + 12 + (size_t)nVals*4;
                if (!curNode.empty() && trackEnd <= sa.size() && nVals >= 6 && (nVals % 6) == 0) {
                    UVTrack tr; tr.nFrames = (int)(nVals / 6); tr.m.resize(nVals);
                    memcpy(tr.m.data(), sa.data()+kp+12, (size_t)nVals*4);
                    if (std::getenv("HSR_SANIMDBG"))
                        fprintf(stderr, "[SANIM-UV] node='%s' UVTransform frames=%d\n", curNode.c_str(), tr.nFrames);
                    matUVAnim[curNode] = std::move(tr);
                    p = trackEnd; continue;
                }
                p += adv; continue;
            }
            curNode = nm; p += adv;     // a node name
        }
        // SYNC each node's channels: loop ALL of T/R/S on the SAME length = the latest frame ANY of
        // them is still moving (max of the per-channel trailing-static lengths). Trimming each channel
        // to its OWN trailing-static point (the old bug) desynced them over loops — the winterlodge
        // tram's small rotation track trimmed shorter than its long translation, so over a few cycles
        // the cabin's orientation drifted out of phase with its position ("elevator flew out / went
        // funny / not looping"). A fully-parked node (background train) still trims (all channels
        // become static together, so the shared max is the parked frame). Each animation thus loops
        // independently on its own true length, with its channels kept in lockstep — faithful.
        animMaxFrames = 0;
        for (auto& kv : nodeAnim) {
            NodeTracks& nt = kv.second;
            int e = 2;
            if (nt.t.nFrames > 0) e = std::max(e, nt.t.effFrames);
            if (nt.r.nFrames > 0) e = std::max(e, nt.r.effFrames);
            if (nt.s.nFrames > 0) e = std::max(e, nt.s.effFrames);
            auto clampN = [&](Track& t){ if (t.nFrames > 0) { int d = t.comps ? (int)(t.v.size()/t.comps) : 0; t.nFrames = std::min(e, d); } };
            clampN(nt.t); clampN(nt.r); clampN(nt.s);
            if (e > animMaxFrames) animMaxFrames = e;
        }
        // UVTransform tracks parsed from the NODE sanim above ALSO define animation length. Without
        // this, a UV-ONLY env (rockquarry fc_environment: the sanim carries the waterfall/fire/mist
        // UVTransform flipbooks but few/no T/R/S node tracks) leaves animMaxFrames<=1 -> hasAnimation()
        // returns FALSE -> the ENTIRE OPA animate/stream block is skipped and NOTHING animates (the
        // "waterfalls are animated yet don't animate in render" bug). The reset above recomputes
        // animMaxFrames from nodeAnim ONLY, wiping the per-track bump; fold the UV/tint tracks back in.
        // (.mat.sanim.opa tracks raise animMaxFrames in loadMatAnim, which runs right after this.)
        for (auto& kv : matUVAnim)   if (kv.second.nFrames > animMaxFrames) animMaxFrames = kv.second.nFrames;
        for (auto& kv : matTintAnim) if (kv.second.nFrames > animMaxFrames) animMaxFrames = kv.second.nFrames;
        log("sanim: %zu animated nodes, %zu UV tracks, maxFrames=%d (%.1fs @%.0ffps)",
            nodeAnim.size(), matUVAnim.size(), animMaxFrames, animDuration(), animFps);
    }

    // Parse *.mat.sanim.opa -> matUVAnim: per geo/node a "UVTransform" track = nFrames x 2x3 UV
    // matrix (flipbook/scroll for smoke/fire/dust/fog/particles). Same track encoding as sanim.
    void loadMatAnim(const std::vector<uint8_t>& sceneZip) {
        // A scene can ship MULTIPLE *.mat.sanim.opa: one scene-wide (papercraft.fbx.mat.sanim.opa,
        // holds e.g. the waterCard UVTransform) PLUS per-mesh ones (hummingbird_winguv...). libshell
        // loads each cooked mesh's own material-anim, so we parse them ALL and merge by geo name —
        // grabbing only the first (the old bug) missed animated water -> it rendered the
        // whole 2x2 atlas static (green + black blotches) = the "dark / messed up moving lilypad".
        std::vector<std::vector<uint8_t>> files;
        { mz_zip_archive z; memset(&z, 0, sizeof(z));
          if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
          uint32_t nf = mz_zip_reader_get_num_files(&z);
          for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() >= 14 && fn.compare(fn.size()-14, 14, ".mat.sanim.opa") == 0) {
                size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
                if (d) { files.emplace_back((uint8_t*)d, (uint8_t*)d + sz); mz_free(d); } } }
          mz_zip_reader_end(&z); }
        for (auto& sa : files) {
            if (sa.size() < 52 || memcmp(sa.data(), "OPAA", 4) != 0) continue;
            { float fr = findFrameRate(sa); if (fr > 0.f) animFps = fr; }   // data-driven rate, not hardcoded
            uint32_t hdr; memcpy(&hdr, sa.data()+16, 4); if (hdr < 48 || hdr >= sa.size()) hdr = 48;
            auto rdName = [&](size_t off, std::string& out) -> size_t {
                if (off + 4 > sa.size() || sa[off] != 0xFF || sa[off+1] != 0xFF) return 0;
                uint16_t ln; memcpy(&ln, sa.data()+off+2, 2);
                if (ln == 0 || ln > 64 || off + 4 + ln > sa.size()) return 0;
                for (uint16_t k = 0; k < ln; ++k) { uint8_t ch = sa[off+4+k]; if (ch < 32 || ch > 126) return 0; }
                out.assign((char*)sa.data()+off+4, ln); return 4u + ln; };
            std::string curGeo; size_t p = hdr + 4;
            while (p + 4 <= sa.size()) {
                std::string nm; size_t adv = rdName(p, nm);
                if (!adv) { ++p; continue; }
                if (nm == "UVTransform" || nm == "MaterialTint") {
                    size_t kp = p + adv;
                    if (kp + 12 > sa.size()) { p += adv; continue; }
                    uint32_t nVals; memcpy(&nVals, sa.data()+kp+8, 4);
                    size_t end = kp + 12 + (size_t)nVals*4;
                    if (end > sa.size()) { p += adv; continue; }
                    // UVTransform = a 2x3 UV matrix per frame (6 floats). Derive frame count from the
                    // VALUE block (nVals/6), NOT nKeys+1 — flag=1 tracks store extra key-time data so
                    // nKeys+1 mismatches nVals (same fix loadSanim uses for comps=3/4 node tracks).
                    if (nm == "UVTransform" && nVals >= 6 && (nVals % 6) == 0 && !curGeo.empty()) {
                        UVTrack tr; tr.nFrames = (int)(nVals / 6); tr.m.resize(nVals);
                        memcpy(tr.m.data(), sa.data()+kp+12, (size_t)nVals*4);
                        if (tr.nFrames > animMaxFrames) animMaxFrames = tr.nFrames;
                        matUVAnim[curGeo] = std::move(tr);
                    }
                    // MaterialTint = per-frame RGBA (4 floats/frame); the fog/dust OPACITY animation.
                    if (nm == "MaterialTint" && nVals >= 4 && (nVals % 4) == 0 && !curGeo.empty()) {
                        TintTrack tr; tr.nFrames = (int)(nVals / 4); tr.rgba.resize(nVals);
                        memcpy(tr.rgba.data(), sa.data()+kp+12, (size_t)nVals*4);
                        if (tr.nFrames > animMaxFrames) animMaxFrames = tr.nFrames;
                        matTintAnim[curGeo] = std::move(tr);
                    }
                    p = end; continue;     // track consumed
                }
                curGeo = nm; p += adv;     // a geo/node name
            }
        }
        log("mat.sanim: %zu UV-animated meshes (from %zu files)", matUVAnim.size(), files.size());
    }

    // ── public entry: returns true if this APK is an OPA env we parsed ──
    bool load(const std::string& apkPath) {
        const std::shared_ptr<const std::vector<uint8_t>> sceneZipHold = scenezip::get(apkPath);   // shared: scene_zip.h
        if (!sceneZipHold) return false;
        const std::vector<uint8_t>& sceneZip = *sceneZipHold;
        {
            mz_zip_archive z; memset(&z, 0, sizeof(z));
            if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return false;
            std::string archiveError;
            const bool safe = zipread::validateArchive(
                z, hslcook::zipsafety::kSceneReadLimits, &archiveError);
            mz_zip_reader_end(&z);
            if (!safe) {
                log("rejected unsafe or oversized scene.zip: %s", archiveError.c_str());
                return false;
            }
        }
        // Enumerate geometry *.fbx.opa. A COMPOSED multi-asset scene (underwater/oceanarium) ships
        // MANY world-baked single-mesh *.fbx.opa placed by per-entity files (no instance list /
        // one main model); a normal home ships ONE baked *.fbx.opa with an instance list. Materials
        // (*.mat.opa), skins (*.skin.opa) etc. don't end in ".fbx.opa", so this selects geometry only.
        std::vector<std::string> fbxOpas;
        { mz_zip_archive z; memset(&z,0,sizeof(z));
          if (mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) {
              uint32_t nf = mz_zip_reader_get_num_files(&z);
              for (uint32_t i=0;i<nf;++i){ mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
                  std::string fn(st.m_filename);
                  if (fn.size()>=8 && fn.compare(fn.size()-8,8,".fbx.opa")==0) fbxOpas.push_back(fn); }
              mz_zip_reader_end(&z);
          } }
        if (fbxOpas.empty()) return false;   // not an OPA env
        auto extractNamed = [&](const std::string& nm, std::vector<uint8_t>& out)->bool{
            mz_zip_archive z; memset(&z,0,sizeof(z));
            if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return false;
            size_t sz=0; void* d = mz_zip_reader_extract_file_to_heap(&z, nm.c_str(), &sz, 0);
            mz_zip_reader_end(&z);
            if (!d) return false; out.assign((uint8_t*)d,(uint8_t*)d+sz); mz_free(d); return true;
        };
        g_loadProgress.set("Reading materials...");
        parseAssetMeta(sceneZip);   // .mat.txt (blend modes) + .png.asset (AssetId -> tex) FIRST
        g_loadProgress.set("Decoding textures...");
        loadTextures(sceneZip);
        loadIBL(sceneZip);          // SpecIbl diffuse irradiance cubemap (*_diffuse.dds.opa = RGBA16F KTX)
        loadVatData(sceneZip);      // VAT vertex-animation textures (underwater coral/fish/seaweed)
        g_loadProgress.set("Loading animations...");
        loadAnim(sceneZip);
        loadMatAnim(sceneZip);      // mat.sanim UV/flipbook effect animation (smoke/fire/dust/fog)
        g_loadProgress.set("Building meshes...");
        // Geo base from a *.fbx.opa name (strip path, leading "sm_", trailing ".fbx.opa") -> VAT key.
        auto opaBase = [](const std::string& nm){ size_t s=nm.find_last_of('/'); std::string b=(s==std::string::npos)?nm:nm.substr(s+1);
            std::string l; for(char c:b) l+=(char)tolower((unsigned char)c);
            size_t d=l.find(".fbx.opa"); if(d!=std::string::npos) l=l.substr(0,d);
            if(l.size()>3 && l.compare(0,3,"sm_")==0) l=l.substr(3); return l; };
        bool ok;
        if (fbxOpas.size() > 1) {
            // Composed scene: load + MERGE every world-baked *.fbx.opa (each parseModel appends to
            // meshes; nodes/materials are per-opa state, textures resolve into each MeshData).
            log("composed scene: merging %zu .fbx.opa", fbxOpas.size());
            int n=0; for (auto& nm : fbxOpas) { std::vector<uint8_t> opa; curOpaBase = opaBase(nm);
                if (extractNamed(nm, opa) && parseModel(opa)) ++n; }
            log("composed scene: merged %d/%zu .fbx.opa -> %zu meshes", n, (size_t)fbxOpas.size(), meshes.size());
            ok = (n > 0);
        } else {
            std::vector<uint8_t> opa;
            if (!extractNamed(fbxOpas[0], opa)) return false;
            log("geometry asset: %s (%zu bytes)", fbxOpas[0].c_str(), opa.size());
            ok = parseModel(opa);
        }
        // Skinned meshes (animals/flags/ships). The LBS math is implemented + confirmed from
        // libshell, BUT correct deform/placement needs the true inverse-bind from the .skel (its
        // compact bind layout isn't decoded yet); the .anim joint matrices are WORLD-space, so
        // applying them without inverseBind spikes/mis-places. Until that's cracked we render the
        // skins STATIC at bind pose (coherent geometry, no spikes). Set HSR_OPA_SKIN to try the
        // (still-WIP) skeletal animation. TODO: decode .skel bind transforms -> real inverseBind.
        loadAnimClips(sceneZip);
        loadSkins(sceneZip);
        if (hasAnimation()) animate(0.0f);
        if (std::getenv("HSR_ANIMDBG") && hasAnimation()) {
            auto centroid = [&](size_t mi, float& x, float& y, float& z){
                x=y=z=0; size_t np=meshes[mi].positions.size()/3; if(!np)return;
                for(size_t q=0;q<np;q++){x+=meshes[mi].positions[q*3];y+=meshes[mi].positions[q*3+1];z+=meshes[mi].positions[q*3+2];}
                x/=np;y/=np;z/=np; };
            log("[ANIMDBG] animRecs=%zu uvAnimRecs=%zu skinRecs=%zu animMaxFrames=%d", animRecs.size(), uvAnimRecs.size(), skinRecs.size(), animMaxFrames);
            for (auto& ar : animRecs) {
                animate(40.0f); float x0,y0,z0; centroid(ar.meshIdx,x0,y0,z0);  // a window that was PARKED before the trim
                animate(45.0f); float x1,y1,z1; centroid(ar.meshIdx,x1,y1,z1);
                float d = sqrtf((x1-x0)*(x1-x0)+(y1-y0)*(y1-y0)+(z1-z0)*(z1-z0));
                if (d > 0.01f || meshes[ar.meshIdx].name.find("train")!=std::string::npos || meshes[ar.meshIdx].name.find("rain")!=std::string::npos) {
                    // raw basePos (cooked, pre-transform) centroid to tell LOCAL vs WORLD space
                    float bx=0,by=0,bz=0; size_t bn=ar.basePos.size()/3;
                    for(size_t q=0;q<bn;q++){bx+=ar.basePos[q*3];by+=ar.basePos[q*3+1];bz+=ar.basePos[q*3+2];}
                    if(bn){bx/=bn;by/=bn;bz/=bn;}
                    log("[ANIMDBG] mesh[%zu] node=%d '%s' rawBase=(%.1f,%.1f,%.1f) world0=(%.1f,%.1f,%.1f) move@5s=%.2f", ar.meshIdx, ar.nodeIdx, meshes[ar.meshIdx].name.c_str(), bx,by,bz, x0,y0,z0, d);
                }
            }
            animate(0.0f);
            // Dump the node chain for any 'train' node: local TRS + parent so we can see if it floats.
            for (size_t i = 0; i < nodes.size(); ++i) {
                std::string ln; for (char ch : nodes[i].name) ln += (char)tolower((unsigned char)ch);
                if (ln.find("train")!=std::string::npos || ln.find("track")!=std::string::npos ||
                    ln.find("car_strip_01")!=std::string::npos || ln=="anim_cars") {
                    int par = nodes[i].parent;
                    const auto at=nodeAnim.find(nodes[i].name);
                    const bool keyed=at!=nodeAnim.end();
                    const float* t0=(keyed && at->second.t.v.size()>=3)?at->second.t.v.data():nullptr;
                    log("[ANIMDBG] node[%zu] '%s' parent=%d('%s') localT=(%.2f,%.2f,%.2f) S=(%.3f,%.3f,%.3f) keyed=%d trackT0=(%.2f,%.2f,%.2f)",
                        i, nodes[i].name.c_str(), par, (par>=0&&par<(int)nodes.size())?nodes[par].name.c_str():"-",
                        nodes[i].t[0],nodes[i].t[1],nodes[i].t[2], nodes[i].s[0],nodes[i].s[1],nodes[i].s[2],
                        (int)keyed, t0?t0[0]:0.f,t0?t0[1]:0.f,t0?t0[2]:0.f);
                }
            }
        }
        return ok || !meshes.empty();
    }

private:
    // Read the cooked metadata the home ships so rendering is FAITHFUL + GENERAL (works for any
    // OPA home, not just spacestation): *.mat.txt give per-material blend mode + diffuse texture
    // ref; *.png.asset give each texture's AssetId so a material's diffuse Id resolves to a file.
    void parseAssetMeta(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        auto readText = [&](uint32_t i) -> std::string {
            size_t sz=0; void* d=mz_zip_reader_extract_to_heap(&z,i,&sz,0);
            if(!d) return {}; std::string s((char*)d,sz); mz_free(d); return s;
        };
        auto findU64 = [](const std::string& s, size_t from)->uint64_t{
            size_t i=from; while(i<s.size() && !(s[i]>='0'&&s[i]<='9')) ++i;
            uint64_t v=0; bool any=false; for(;i<s.size()&&s[i]>='0'&&s[i]<='9';++i){v=v*10+(s[i]-'0');any=true;}
            return any?v:0;
        };
        for (uint32_t i=0;i<nf;++i) {
            mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
            std::string fn(st.m_filename);
            if (fn.size()>8 && fn.substr(fn.size()-8)==".mat.txt") {
                std::string s = readText(i);
                MatProps mp; mp.found=true;
                auto flag=[&](const char* key)->bool{
                    size_t p=s.find(key); if(p==std::string::npos) return false;
                    size_t c=s.find(':',p); if(c==std::string::npos) return false;
                    size_t t=s.find_first_not_of(" \t",c+1);
                    return t!=std::string::npos && s.compare(t,4,"true")==0;
                };
                mp.transparent = flag("Transparent");
                mp.additive    = flag("Additive");
                mp.alphaTest   = flag("AlphaTest");
                mp.doubleSided = flag("DoubleSided");
                mp.unlit       = flag("Unlit");
                // Color texture ref: ShellEnv materials name it "diffuse"; SpecIbl name it
                // "basecolor" (some "albedo"). CRITICAL: search ONLY inside the "Textures:" section.
                // SpecIbl materials ALSO declare 'basecolor'/'diffuse' UNIFORMS (in the Uniforms
                // section, with a Value but NO texture Id) — matching those gave Id=0 -> ground/
                // terrain/cavern rendered untextured = the "grey/white where it shouldn't be".
                size_t texSec = s.find("Textures:");
                size_t from = (texSec != std::string::npos) ? texSec : 0;
                size_t dp = std::string::npos;
                for (const char* key : {"Name: diffuse","Name:diffuse","Name: basecolor","Name:basecolor","Name: albedo","Name:albedo"}) {
                    size_t p = s.find(key, from);
                    if (p!=std::string::npos && (dp==std::string::npos || p<dp)) dp = p;
                }
                if (dp!=std::string::npos) {
                    size_t idp = s.find("Id:", dp);
                    if (idp!=std::string::npos && idp < dp+200) mp.diffuseId = findU64(s, idp+3);
                    size_t pp = s.find("Path:", dp);
                    if (pp!=std::string::npos && pp < dp+200) {
                        size_t a=s.find_first_not_of(" \t",pp+5), b=s.find_first_of("\r\n",a);
                        std::string path=s.substr(a, b==std::string::npos?std::string::npos:b-a);
                        size_t sl=path.find_last_of("/\\"); if(sl!=std::string::npos) path=path.substr(sl+1);
                        size_t d2=path.find(".png"); if(d2!=std::string::npos) path=path.substr(0,d2);
                        mp.diffuseBase = lc(path);
                    }
                }
                // BAKED lightmap texture ref (Textures section "Name: lightmap" -> Id). The interior
                // SHELL meshes carry their full baked lighting/detail here; resolve it so no-albedo
                // shells (helmet/gem/...) can use it as their visible surface instead of a flat blob.
                if (texSec != std::string::npos) {
                    size_t lp = s.find("Name: lightmap", texSec);
                    if (lp == std::string::npos) lp = s.find("Name:lightmap", texSec);
                    if (lp != std::string::npos) {
                        size_t idp = s.find("Id:", lp);
                        if (idp!=std::string::npos && idp < lp+200) mp.lightmapId = findU64(s, idp+3);
                    }
                }
                // 'diffuse' basecolor UNIFORM (NOT the Textures-section ref). When a material has
                // no texture this IS its flat color (black_mtl=[0,0,0] -> black/invisible; stars=[0.5];
                // SpecIbl terrain=[1,1,1] tint). Search only inside the Uniforms section.
                size_t uniSec = s.find("Uniforms:");
                if (uniSec != std::string::npos) {
                    size_t dn = std::string::npos;
                    for (const char* key : {"Name: diffuse","Name:diffuse","Name: basecolor","Name:basecolor"}) {
                        size_t p = s.find(key, uniSec);
                        if (p!=std::string::npos && (dn==std::string::npos || p<dn)) dn = p;
                    }
                    if (dn != std::string::npos) {
                        size_t vp = s.find("Value", dn);
                        if (vp!=std::string::npos && vp < dn+60) {
                            size_t lim = s.size();
                            for (const char* k : {"UniformProperty","Textures:"}) { size_t p=s.find(k,vp+6); if(p!=std::string::npos&&p<lim) lim=p; }
                            { size_t p=s.find("Name:",vp+6); if(p!=std::string::npos&&p<lim) lim=p; }
                            int got=0; size_t q=s.find(':',vp); if(q!=std::string::npos) ++q;
                            while (got<3 && q!=std::string::npos && q<lim) {
                                bool num  = (s[q]>='0'&&s[q]<='9');
                                bool sign = (s[q]=='-'||s[q]=='.') && q+1<lim && ((s[q+1]>='0'&&s[q+1]<='9')||s[q+1]=='.');
                                if (!num && !sign) { ++q; continue; }
                                size_t s0=q++; while(q<lim && ((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='e'||s[q]=='E'||s[q]=='-'||s[q]=='+')) ++q;
                                try { mp.diffuseColor[got++] = std::stof(s.substr(s0,q-s0)); } catch(...) {}
                            }
                        }
                    }
                    // 'alpha' UNIFORM (first component). It precedes 'alphatestthreshold' in the
                    // Uniforms list, so the first "Name: alpha" whose next char is whitespace/newline
                    // (NOT the 't' of alphatestthreshold) is the one we want.
                    size_t ap = uniSec;
                    while ((ap = s.find("Name:", ap)) != std::string::npos) {
                        size_t c = ap + 5; while (c<s.size() && (s[c]==' '||s[c]=='\t')) ++c;
                        if (s.compare(c,5,"alpha")==0 && c+5<s.size() && s[c+5]!='t') {
                            size_t vp = s.find("Value", c);
                            if (vp!=std::string::npos && vp < c+60) {
                                size_t q = s.find(':', vp); if (q!=std::string::npos) ++q;
                                while (q<s.size() && !((s[q]>='0'&&s[q]<='9')||((s[q]=='-'||s[q]=='.')&&q+1<s.size()&&((s[q+1]>='0'&&s[q+1]<='9')||s[q+1]=='.')))) ++q;
                                size_t s0=q; while(q<s.size() && ((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='e'||s[q]=='E'||s[q]=='-'||s[q]=='+')) ++q;
                                try { mp.alpha = std::stof(s.substr(s0,q-s0)); } catch(...) {}
                            }
                            break;
                        }
                        ap = c;
                    }
                }
                // PBR/SpecIbl scalars (first vec component) — read VERBATIM from the cooked uniforms,
                // for the no-albedo metallic/gem shells' split-sum IBL (divingHelmet, rubyGem, ...).
                mp.isSpecibl = (s.find("Shader: SpecIbl")!=std::string::npos) || (s.find("Shader:SpecIbl")!=std::string::npos);
                auto uniScalar=[&](const char* name, float defv)->float{
                    size_t nl=std::strlen(name), pos=0;
                    while ((pos=s.find("Name:",pos))!=std::string::npos) {
                        size_t c=pos+5; while(c<s.size()&&(s[c]==' '||s[c]=='\t'))++c;
                        if (s.compare(c,nl,name)==0) {
                            char after = (c+nl<s.size())? s[c+nl] : '\n';
                            if (after=='\n'||after=='\r'||after==' '||after=='\t') {  // exact name, not a prefix
                                size_t vp=s.find("Value",c);
                                if (vp!=std::string::npos && vp<c+80) {
                                    size_t q=s.find('[',vp);
                                    if (q!=std::string::npos) { ++q;
                                        while(q<s.size()&&!((s[q]>='0'&&s[q]<='9')||((s[q]=='-'||s[q]=='.')&&q+1<s.size())))++q;
                                        size_t s0=q; while(q<s.size()&&((s[q]>='0'&&s[q]<='9')||s[q]=='.'||s[q]=='-'||s[q]=='+'||s[q]=='e'||s[q]=='E'))++q;
                                        try { return std::stof(s.substr(s0,q-s0)); } catch(...) {}
                                    }
                                }
                            }
                        }
                        pos=c;
                    }
                    return defv;
                };
                mp.metallic         = uniScalar("metallic", 0.0f);
                mp.roughness        = uniScalar("roughness", 1.0f);
                mp.alphaCutoff      = uniScalar("alphatestthreshold", 0.5f);   // authored AlphaTest discard threshold
                mp.speciblDiffScale = uniScalar("specibldiffusescale", 1.0f);
                mp.speciblSpecScale = uniScalar("speciblspecularscale", 1.0f);
                // lightmappower: per-channel HDR boost on the baked lightmap = the neon/glow tint.
                // Read all 3 components (libshell: lightmapColor *= lightmappower.rgb). "lightmappowertweaks"
                // is the same slot in some materials. uniScalar reads the first component; pull xyz here.
                auto uniVec3=[&](const char* name, float* out){
                    size_t nl=std::strlen(name), pos=0;
                    while ((pos=s.find("Name:",pos))!=std::string::npos) {
                        size_t c=pos+5; while(c<s.size()&&(s[c]==' '||s[c]=='\t'))++c;
                        if (s.compare(c,nl,name)==0) { char a=(c+nl<s.size())?s[c+nl]:'\n';
                            if (a=='\n'||a=='\r'||a==' '||a=='\t') {
                                size_t q=s.find("Value",c); if(q==std::string::npos||q>=c+80){pos=c;continue;}
                                // Read up to 3 numbers between Value and the NEXT property (handles BOTH
                                // inline `Value: [a, b, c]` and multi-line `Value:\n - a\n - b\n - c`).
                                size_t lim=std::min(s.size(), q+160);
                                for (const char* k : {"UniformProperty","Name:","Textures:"}) { size_t p=s.find(k,q+5); if(p!=std::string::npos&&p<lim) lim=p; }
                                int got=0; size_t i=q+5;
                                while (got<3 && i<lim) {
                                    if ((s[i]>='0'&&s[i]<='9')||((s[i]=='-'||s[i]=='.')&&i+1<lim&&((s[i+1]>='0'&&s[i+1]<='9')||s[i+1]=='.'))) {
                                        size_t s0=i++; while(i<lim&&((s[i]>='0'&&s[i]<='9')||s[i]=='.'||s[i]=='-'||s[i]=='+'||s[i]=='e'||s[i]=='E'))++i;
                                        try { out[got++]=std::stof(s.substr(s0,i-s0)); } catch(...){ }
                                    } else ++i;
                                }
                                return got>0;
                            }
                        }
                        pos=c;
                    }
                    return false;
                };
                if (!uniVec3("lightmappower", mp.lightmapPower)) uniVec3("lightmappowertweaks", mp.lightmapPower);
                matProps[matStem(fn)] = mp;
            } else if (fn.size()>10 && fn.substr(fn.size()-10)==".png.asset") {
                std::string s = readText(i);
                size_t ip = s.find("AssetId:");
                uint64_t id = (ip!=std::string::npos) ? findU64(s, ip+8) : 0;
                size_t sl=fn.find_last_of('/'); std::string base=(sl==std::string::npos)?fn:fn.substr(sl+1);
                size_t d2=base.find(".png.asset"); if(d2!=std::string::npos) base=base.substr(0,d2);
                if (id) assetIdToTexBase[id] = lc(base);
            }
        }
        mz_zip_reader_end(&z);
        log("parsed metadata: %zu materials, %zu texture AssetIds", matProps.size(), assetIdToTexBase.size());
    }

    // Find + decode the SpecIbl DIFFUSE irradiance cubemap (`*_diffuse.dds.opa` / `*hdr*diffuse*`,
    // RGBA16F KTX, 6 faces). The renderer uses it to env-light `*_specibl` materials. (The matching
    // `*_specular.dds.opa` is for the reflection layer — TODO.)
    void loadIBL(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z); int found = -1, foundSpec = -1;
        for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename); std::string lo=fn; for(auto&c:lo)c=(char)tolower((unsigned char)c);
            if (lo.size()>8 && lo.compare(lo.size()-8,8,".dds.opa")==0) {
                if (lo.find("diffuse")  != std::string::npos && found < 0)     found = (int)i;
                if (lo.find("specular") != std::string::npos && foundSpec < 0) foundSpec = (int)i;
            }
        }
        if (found >= 0) {
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, found, &sz, 0);
            if (d) { if (ibl::decodeCubemap((const uint8_t*)d, sz, iblDiffuse))
                         log("IBL diffuse cubemap loaded: %d^2 x6 faces", iblDiffuse.size);
                     else log("IBL diffuse cubemap: decode FAILED");
                     mz_free(d); }
        }
        if (foundSpec >= 0) {   // keep the raw RGBA16F specular bytes AND decode mip0 for CPU sampling
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, foundSpec, &sz, 0);
            if (d) { iblSpecularRaw.assign((uint8_t*)d, (uint8_t*)d + sz);
                     if (ibl::decodeCubemap((const uint8_t*)d, sz, iblSpecular))
                         log("IBL specular cubemap decoded: %d^2 x6 (+%zu raw bytes)", iblSpecular.size, iblSpecularRaw.size());
                     else log("IBL specular cubemap raw kept (%zu bytes, mip0 decode FAILED)", iblSpecularRaw.size());
                     mz_free(d); }
        }
        mz_zip_reader_end(&z);
    }

    // Decode EVERY *.png.opa (OPAA container -> KTX at payload offset -> ASTC -> RGBA), keyed by
    // its cooked basename so any home resolves (spacestation "tx_*", rockquarry "*_diffuse", ...).
    void loadTextures(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        g_loadProgress.set("Decoding textures...", 0, (int)nf);
        for (uint32_t i = 0; i < nf; ++i) {
            g_loadProgress.tick((int)i);
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            size_t slash = fn.find_last_of('/');
            std::string base = (slash == std::string::npos) ? fn : fn.substr(slash + 1);
            size_t dot = base.find(".png.opa");
            if (dot == std::string::npos) continue;
            std::string key = lc(base.substr(0, dot));   // full basename (no "tx_" stripping)
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!d) continue;
            if (sz > 48 && memcmp(d, "OPAA", 4) == 0) {
                uint32_t hdr; memcpy(&hdr, (uint8_t*)d + 16, 4); if (hdr < 48 || hdr >= sz) hdr = 48;
                Tex t; t.key = key;
                auto chain = std::make_shared<TexSource>();
                uint32_t pw = 0, ph = 0;
                if (decodeKtxBaseMip((uint8_t*)d + hdr, sz - hdr, t.rgba, t.w, t.h, &chain->astc, &chain->bw, &chain->bh, &chain->mips, 256, &pw, &ph) && !t.rgba.empty()) {
                    if (!chain->astc.empty()) {
                        chain->w = t.w; chain->h = t.h;
                        t.src = chain;
                        t.preview = t.rgba.size() < (size_t)t.w * t.h * 4;
                        if (t.preview && pw && ph && t.rgba.size() >= (size_t)pw * ph * 4) { t.pvW = pw; t.pvH = ph; }
                    }
                    // KEEP the real KTX alpha (we render unlit, outputting texture rgb + a). Blend
                    // mode comes from the material's .mat.txt (Transparent/Additive), not a guess;
                    // hasAlpha stays only as a fallback for materials with no .mat.txt.
                    size_t total = t.rgba.size() / 4, lowA = 0;
                    for (size_t px = 3; px < t.rgba.size(); px += 4) if (t.rgba[px] < 128) ++lowA;
                    t.hasAlpha = total && (lowA * 100 > total * 3);
                    // Cooked without mips, the "preview" is the whole image: kept (and copied into every mesh) that was
                    // GBs. A small copy stays for the alpha checks; full() decodes the real one if it's ever needed.
                    while (t.src && std::max(t.pw(), t.ph()) > 256 && t.rgba.size() >= (size_t)t.pw() * t.ph() * 4) t.halve();
                    if (t.src && t.rgba.size() < (size_t)t.w * t.h * 4) t.preview = true;
                    textures.push_back(std::move(t));
                }
            }
            mz_free(d);
        }
        mz_zip_reader_end(&z);
        log("decoded %zu textures", textures.size());
    }

    // Decode every *_vatdata.exr.opa. Despite the ".exr" name it's cooked as an UNCOMPRESSED
    // RGBA32F KTX (glType GL_FLOAT 5126, glIntFmt GL_RGBA32F 0x8814): width = #anim verts,
    // height = #frames, each texel.xyz = the per-frame vertex POSITION OFFSET (frame0 = 0 = rest).
    // Keyed by the geo base (strip "t_" + "_vatdata.exr") to match the mesh ("sm_<X>.fbx").
    void loadVatData(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z,0,sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        static const uint8_t kid[12]={0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        for (uint32_t i=0;i<nf;++i){ mz_zip_archive_file_stat st; if(!mz_zip_reader_file_stat(&z,i,&st)) continue;
            std::string fn(st.m_filename);
            size_t slash=fn.find_last_of('/'); std::string base=(slash==std::string::npos)?fn:fn.substr(slash+1);
            size_t dot=base.find("_vatdata.exr.opa"); if(dot==std::string::npos) continue;
            std::string key=lc(base.substr(0,dot));
            if(key.size()>2 && key[0]=='t' && key[1]=='_') key=key.substr(2);     // t_<X> -> <X>
            size_t sz=0; void* dd=mz_zip_reader_extract_to_heap(&z,i,&sz,0); if(!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd,(uint8_t*)dd+sz); mz_free(dd);
            uint32_t hdr; if(file.size()<20){continue;} memcpy(&hdr,file.data()+16,4); if(hdr<48||hdr>=file.size()) hdr=48;
            const uint8_t* k=file.data()+hdr; size_t kn=file.size()-hdr;
            if (kn<80 || memcmp(k,kid,12)!=0) continue;
            auto u=[&](size_t o){ uint32_t v; memcpy(&v,k+o,4); return v; };
            uint32_t glType=u(16), w=u(36), h=u(40), kv=u(60);
            size_t off=64+kv+4;                                  // skip header + kvData + u32 imageSize
            if (glType!=5126 || !w || !h) continue;              // expect GL_FLOAT RGBA32F
            if (off + (size_t)w*h*16 > kn) continue;
            VatData vd; vd.cols=(int)w; vd.frames=(int)h; vd.off.resize((size_t)w*h*3);
            const float* src=(const float*)(k+off);
            for (size_t t=0;t<(size_t)w*h;++t){ vd.off[t*3]=src[t*4]; vd.off[t*3+1]=src[t*4+1]; vd.off[t*3+2]=src[t*4+2]; }
            vatByBase[key]=std::move(vd);
        }
        mz_zip_reader_end(&z);
        if (!vatByBase.empty()) log("loaded %zu VAT vertex-animations", vatByBase.size());
    }
    // Match a material base name to a texture by longest common prefix (the names differ:
    // M_station_tubes_a_01 -> tx_station_tubes_a_03, M_ui_ring_a_dblsided -> tx_ui_ring_a_01).
    const Tex* bestTexFor(const std::string& matBaseLower) const {
        const Tex* best = nullptr; size_t bestLen = 0;
        for (auto& t : textures) {
            size_t l = 0;
            while (l < matBaseLower.size() && l < t.key.size() && matBaseLower[l] == t.key[l]) ++l;
            if (l > bestLen) { bestLen = l; best = &t; }
        }
        return (bestLen >= 3) ? best : nullptr;
    }
    static std::string matBaseName(const std::string& path) {
        size_t a = path.find(".M_"); if (a == std::string::npos) return {};
        a += 3; size_t b = path.find(".mat.asset");
        std::string s = (b != std::string::npos && b > a) ? path.substr(a, b - a) : path.substr(a);
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }

    // Find the geometry .opa: a "*.fbx.opa" that is NOT a per-material "*.mat.opa".
    static bool findFbxOpa(const std::vector<uint8_t>& sceneZip, std::vector<uint8_t>& out, std::string& nameOut) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return false;
        u32 nf = mz_zip_reader_get_num_files(&z);
        int best = -1; size_t bestSz = 0; std::string bestName;
        for (u32 i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            auto ends = [&](const char* suf) {
                size_t l = strlen(suf);
                return fn.size() >= l && fn.compare(fn.size()-l, l, suf) == 0;
            };
            if (ends(".fbx.opa") && !ends(".mat.opa")) {
                // pick the largest (the geometry blob dwarfs anything else)
                if ((size_t)st.m_uncomp_size > bestSz) { best = (int)i; bestSz = (size_t)st.m_uncomp_size; bestName = fn; }
            }
        }
        if (best < 0) { mz_zip_reader_end(&z); return false; }
        size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, best, &sz, 0);
        mz_zip_reader_end(&z);
        if (!d) return false;
        out.assign((uint8_t*)d, (uint8_t*)d + sz);
        mz_free(d);
        nameOut = bestName;
        return true;
    }

    void computeNodeWorld() {
        nodeWorld.assign(nodes.size(), identity());
        // nodes are stored parent-before-child here (RootNode first); compute in order,
        // falling back to identity for any out-of-range parent.
        for (size_t i = 0; i < nodes.size(); ++i) {
            Mat4 local = trs(nodes[i].t, nodes[i].r, nodes[i].s);
            int par = nodes[i].parent;
            if (par >= 0 && par < (int)i) nodeWorld[i] = mul(nodeWorld[par], local);
            else nodeWorld[i] = local;
        }
    }

    // ── *.anim.opa skeletal clip: per-frame, per-joint 4x4 SKINNING matrices (pre-multiplied
    // jointWorld*inverseBind; frame0 ~= identity). Confirmed via libshell's skinning vertex shader
    // (linear blend skinning): localPos = Σ Joints[idx_i]*pos * weight_i. ────────────────────
    // .skel.opa (positional, like V79 libshell Skeleton.cpp): ver, extra, jointIds[u64], jointNames
    // [str], jointParents[i32], jointLocalPoses[T3 f32 + R4 f32 (w,x,y,z) + S3 f32]. -> parents + bind LOCAL 4x4.
    bool parseSkel(const std::vector<uint8_t>& file, std::vector<int>& parents, std::vector<float>& bindLocal) {
        if (file.size() < 52 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdr; memcpy(&hdr, file.data()+16, 4); if (hdr < 48 || hdr >= file.size()) hdr = 48;
        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdr;
        uint32_t sver = c.u32();                       // ver
        if (sver >= 0x408) c.u32();                    // extra(=1) — ONLY ver>=0x408; ver 0x405 has none
                                                       // (reading it unconditionally desynced -> 0 joints ->
                                                       //  spacestation's 25 vehicle bones never loaded -> ships frozen)
        uint32_t nIds = c.u32();   for (uint32_t i=0;i<nIds  && c.ok;++i) c.u64();   // jointIds  (skip)
        uint32_t nNm  = c.u32();   for (uint32_t i=0;i<nNm   && c.ok;++i) c.str();   // jointNames(skip)
        uint32_t nPar = c.u32();   parents.resize(nPar); for (uint32_t i=0;i<nPar && c.ok;++i) parents[i]=c.i32();
        uint32_t nPose= c.u32();
        if (!c.ok || nPose != nPar || nPose == 0) return false;
        bindLocal.resize((size_t)nPose*16);
        for (uint32_t j=0;j<nPose && c.ok;++j) {
            float T[3]={c.f32(),c.f32(),c.f32()};
            float Rw=c.f32(),Rx=c.f32(),Ry=c.f32(),Rz=c.f32();   // skel quat = (w,x,y,z)
            float S[3]={c.f32(),c.f32(),c.f32()};
            float q[4]={Rx,Ry,Rz,Rw};                            // trs() wants (x,y,z,w)
            Mat4 m = trs(T, q, S);
            memcpy(bindLocal.data()+(size_t)j*16, m.m, 16*sizeof(float));
        }
        return c.ok;
    }
    void loadAnimClips(const std::vector<uint8_t>& sceneZip) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        auto baseOf = [](const std::string& fn, const char* suf)->std::string{
            size_t sl=fn.find_last_of('/'); std::string b=(sl==std::string::npos)?fn:fn.substr(sl+1);
            size_t sp=b.rfind(suf); if(sp!=std::string::npos) b=b.substr(0,sp); return lc(b); };
        // pass 1: skeletons (parents + bind local)
        for (uint32_t i = 0; i < nf; ++i) { mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".skel.opa") continue;
            size_t sz=0; void* dd=mz_zip_reader_extract_to_heap(&z,i,&sz,0); if(!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd,(uint8_t*)dd+sz); mz_free(dd);
            std::vector<int> par; std::vector<float> bl;
            if (parseSkel(file, par, bl)) skelData[baseOf(fn,".skel.opa")] = { std::move(par), std::move(bl) };
        }
        // pass 2: anim clips, attach the matching skel -> compute inverse bind WORLD
        for (uint32_t i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".anim.opa") continue;
            if (fn.find(".sanim.") != std::string::npos) continue;   // sanim = node TRS (handled elsewhere)
            size_t sz = 0; void* dd = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd, (uint8_t*)dd + sz); mz_free(dd);
            AnimClip clip;
            if (parseAnimClip(file, clip) && clip.numFrames > 1 && clip.numJoints > 0) {
                auto sit = skelData.find(baseOf(fn, ".anim.opa"));
                if (sit != skelData.end() && (int)sit->second.first.size() == clip.numJoints
                                          && (int)sit->second.second.size() == clip.numJoints*16) {
                    clip.parents = sit->second.first;
                    const std::vector<float>& bl = sit->second.second;
                    std::vector<float> bw((size_t)clip.numJoints*16);    // bind WORLD = compose(bindLocal,parents)
                    for (int j=0;j<clip.numJoints;++j) { int p=clip.parents[j];
                        if (p<0||p>=j) memcpy(bw.data()+(size_t)j*16, bl.data()+(size_t)j*16, 16*sizeof(float));
                        else mat4mul(bw.data()+(size_t)p*16, bl.data()+(size_t)j*16, bw.data()+(size_t)j*16); }
                    clip.invBind.resize((size_t)clip.numJoints*16);
                    for (int j=0;j<clip.numJoints;++j) mat4affineInverse(bw.data()+(size_t)j*16, clip.invBind.data()+(size_t)j*16);
                }
                int ci = (int)clips.size();
                for (int j = 0; j < (int)clip.joints.size(); ++j) jointToClip[clip.joints[j]] = {ci, j};
                if (clip.numFrames > animMaxFrames) animMaxFrames = clip.numFrames;
                clips.push_back(std::move(clip));
            }
        }
        mz_zip_reader_end(&z);
        log("loaded %zu skeletal anim clips (%zu skeletons)", clips.size(), skelData.size());
    }
    bool parseAnimClip(const std::vector<uint8_t>& file, AnimClip& clip) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4); if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;
        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdrSize;
        c.u32();                                  // ver
        uint32_t jc = c.u32();
        if (c.p + 2 <= c.n) { uint16_t m; memcpy(&m, c.d+c.p, 2); if (m != 0xFFFF) jc = c.u32(); } // skip extra field
        for (uint32_t j = 0; j < jc && c.ok; ++j) clip.joints.push_back(c.str());
        if (!c.ok) return false;
        // After the joint names there's a skeleton block (parent indices / bind data) before the
        // "Transform" track — scan to it rather than assuming it's immediate.
        size_t tp = std::string::npos;
        for (size_t q = c.p; q + 9 <= c.n; ++q) if (memcmp(c.d+q, "Transform", 9) == 0) { tp = q; break; }
        if (tp == std::string::npos || tp < 4) return false;
        c.p = tp - 4;                             // back to the string record (ffff + len)
        std::string track = c.str();              // "Transform"
        if (!c.ok || track != "Transform") return false;
        c.u32();                                  // flag
        c.u32();                                  // nKeys
        uint32_t nVals = c.u32();
        if (!jc || nVals == 0 || (nVals % (jc*16)) != 0) return false;
        if (!c.avail((size_t)nVals*4)) return false;
        clip.numJoints = (int)jc; clip.numFrames = (int)(nVals / (jc*16));
        clip.mats.resize(nVals);
        memcpy(clip.mats.data(), c.d + c.p, (size_t)nVals*4);   // per-frame LOCAL joint poses
        return true;
    }

    // ── Skinned meshes (*.skin.opa): the horses/owl/chickens/flags/spaceships ──────────────
    // The .skin.opa container = ver, a material ref (Id + Path to a .mat.asset), then the geometry
    // (posFmt "SkinnedPos" [stride 24 = pos f32x3 + weights f16x4 + bone idx u8x4], dataFmt
    // "StdData" [stride 20, uv f16x4 @12], indices "kUnsignedShort"), then a joint name list. We
    // render it at BIND POSE (the SkinnedPos verts are already in model space) so the animals/
    // ships APPEAR; full CPU skeletal skinning (via *.skel/*.anim) is a follow-up. Robust to the
    // exact container header by SCANNING for the "SkinnedPos" posFmt marker.
    void loadSkins(const std::vector<uint8_t>& sceneZip) {
        if (std::getenv("HSR_DUMPNODES")) { log("=== scene nodes: %zu ===", nodes.size());
            for (size_t k=0;k<nodes.size();++k){ const float* W=(k<nodeWorld.size())?nodeWorld[k].m:nullptr;
                log("node[%zu] '%s' par=%d t=(%.2f,%.2f,%.2f)", k, nodes[k].name.c_str(), nodes[k].parent, W?W[12]:0.f, W?W[13]:0.f, W?W[14]:0.f); } }
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, sceneZip.data(), sceneZip.size(), 0)) return;
        uint32_t nf = mz_zip_reader_get_num_files(&z);
        int loaded = 0;
        for (uint32_t i = 0; i < nf; ++i) {
            mz_zip_archive_file_stat st; if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
            std::string fn(st.m_filename);
            if (fn.size() < 9 || fn.substr(fn.size()-9) != ".skin.opa") continue;
            size_t sz = 0; void* dd = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
            if (!dd) continue;
            std::vector<uint8_t> file((uint8_t*)dd, (uint8_t*)dd + sz); mz_free(dd);
            if (parseSkin(file, fn)) ++loaded;
        }
        mz_zip_reader_end(&z);
        log("loaded %d skinned meshes (bind pose)", loaded);
    }

    bool parseSkin(const std::vector<uint8_t>& file, const std::string& fn) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) return false;
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4); if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;
        uint32_t ver; memcpy(&ver, file.data()+hdrSize, 4);
        auto findStr = [&](const char* needle, size_t nlen, size_t from) -> size_t {
            for (size_t i = from; i + nlen <= file.size(); ++i)
                if (memcmp(file.data()+i, needle, nlen) == 0) return i;
            return std::string::npos;
        };
        size_t sp = findStr("SkinnedPos", 10, hdrSize);
        if (sp == std::string::npos) return false;
        // ALL material .mat.asset refs in container order (the material table sits BEFORE the geometry) —
        // a skin can carry MULTIPLE submeshes with DIFFERENT materials (stinson palmtree_grp_a: palmTrunk +
        // palmFronds). The old first-match-only pairing gave EVERY submesh the FIRST material, so the palm
        // FRONDS wore the TRUNK texture ("leaves have wrong skinned texture"). Submesh.matIndex indexes this
        // list; single-material skins are unchanged (matRefs[0]).
        std::vector<std::string> matRefs;
        { size_t from = 0;
          while (true) { size_t m = findStr(".mat.asset", 10, from);
              if (m == std::string::npos || m > sp) break;
              size_t s = m + 10; while (s > 0) { char ch = (char)file[s-1];
                  if (isalnum((unsigned char)ch) || ch=='_'||ch=='-'||ch=='.'||ch=='/') --s; else break; }
              matRefs.push_back(std::string((const char*)file.data()+s, (m+10)-s));
              from = m + 10; } }
        std::string matRef = matRefs.empty() ? std::string() : matRefs[0];
        Cur c; c.d = file.data(); c.n = file.size(); c.p = sp - 4;   // at the posFmt string record
        std::string posFmt = c.str();      // "SkinnedPos"
        std::string dataFmt = c.str();     // "StdData"
        uint32_t listCount = c.u32(); if (listCount != 0) return false;
        uint32_t submeshCount = c.u32();
        struct SubR { uint32_t baseVertex, firstIndex, indexCount, matIndex; };
        std::vector<SubR> subs; subs.reserve(std::min<size_t>(submeshCount, c.n - c.p));   // count is file data: never reserve past the bytes left
        for (uint32_t s = 0; s < submeshCount && c.ok; ++s) {
            uint32_t bv=c.u32(), fi=c.u32(), ic=c.u32(), mi=c.u32();
            // Submesh extras by version — IDENTICAL to parseModel (the working 0x409 rigid path):
            // 0x407/0x408 add 2 u32, 0x409 adds 2 MORE (4 total). The old per-submesh skip(24)+u8 here
            // was wrong (parseModel has no per-submesh AABB) and made every 0x409 skin fail to parse
            // -> "loaded 0 skinned meshes" (affected skinned scenery never reached the GPU).
            if (ver >= 0x407) { c.u32(); c.u32(); }
            if (ver >= 0x409) { c.u32(); c.u32(); }
            c.skip(24); c.u8();   // per-submesh AABB (24) + flag (1) — the SKIN has this, the rigid model doesn't
            subs.push_back({bv, fi, ic, mi});
        }
        c.skip(24); uint32_t vertCount = c.u32(); (void)vertCount;
        uint32_t posBytes = c.u32(); size_t posOff = c.p; c.skip(posBytes);
        uint32_t stdBytes = c.u32(); size_t stdOff = c.p; c.skip(stdBytes);
        uint32_t idxCnt = c.u32(); (void)idxCnt;
        std::string idxType = c.str();
        uint32_t idxBytes = c.u32(); size_t idxOff = c.p; c.skip(idxBytes);
        if (!c.ok || idxType != "kUnsignedShort") return false;
        uint32_t posStride = (posFmt.find("Skinned") != std::string::npos) ? 24 : 12;
        uint32_t nv = posBytes / posStride, nstd = stdBytes / 20, nidx = idxBytes / 2;
        const uint8_t* posP = c.at(posOff); const uint8_t* stdP = c.at(stdOff);
        const uint16_t* idxP = (const uint16_t*)c.at(idxOff);
        // texture/blend resolver — called PER SUBMESH with its OWN material (sub.matIndex -> matRefs)
        auto resolveMat = [&](const std::string& mref, const Tex*& tex, const MatProps*& mp) {
            tex = nullptr; mp = nullptr;
            std::string stem = matStem(mref); auto it = matProps.find(stem); if (it != matProps.end()) mp = &it->second;
            if (mp) { if (!mp->diffuseBase.empty()) tex = texByBase(mp->diffuseBase);
                      if (!tex && mp->diffuseId) { auto a = assetIdToTexBase.find(mp->diffuseId); if (a != assetIdToTexBase.end()) tex = texByBase(a->second); } } };
        const Tex* tex = nullptr; const MatProps* mp = nullptr;
        resolveMat(matRef, tex, mp);
        static int novflip = -1; if (novflip < 0) novflip = std::getenv("HSR_NOVFLIP") ? 1 : 0;
        // Skin's own joint name list (after the index data) -> map each bone index to a clip joint.
        // The skin's joints share names (and usually order) with its .anim clip. bone idx (u8x4)
        // indexes THIS list; we resolve name -> jointToClip to get the (clip, jointInClip).
        // ── Skin tail (libshell-faithful, DETERMINISTIC) ──────────────────────────────────
        // After the index data comes a u16 tail (ver>=0x404, same as the model mesh entry),
        // then THREE count-prefixed lists: jointNames [u32 cnt][cnt str], jointIds
        // [u32 cnt][cnt u64], and the per-bone INVERSE-BIND [u32 cnt][cnt 4x4 col-major].
        // libshell's string reader (sub_AEF1EC) copies raw length-prefixed bytes with NO
        // character filter, so joint names may contain ':' (Maya/FBX namespaces, e.g.
        // "tent_a_cloth_dembones:tentA1_joint" — the Asgard's Wrath DemBones tents). The bone
        // idx (u8x4 in SkinnedPos) indexes THIS jointNames list; resolve name -> jointToClip.
        std::vector<std::string> skinJoints;
        std::vector<float> skinInvBind;
        {
            Cur t = c;                                   // positioned right after the index data
            if (ver >= 0x404) t.skip(2);                 // u16 tail
            uint32_t jnCount = t.u32();
            bool good = t.ok && jnCount >= 1 && jnCount <= 512;
            std::vector<std::string> names;
            for (uint32_t j = 0; good && j < jnCount; ++j) { std::string s = t.str(); if (!t.ok) { good = false; break; } names.push_back(std::move(s)); }
            if (good) { uint32_t jidCount = t.u32(); if (t.ok && jidCount <= 512) t.skip((size_t)jidCount*8); else good = false; }
            if (good) { uint32_t ibCount = t.u32();
                if (t.ok && ibCount == jnCount && t.avail((size_t)ibCount*64)) {
                    skinJoints = std::move(names);
                    skinInvBind.resize((size_t)ibCount*16);
                    memcpy(skinInvBind.data(), t.d + t.p, (size_t)ibCount*64);
                } }
        }
        // Fallback (robust to any container quirk): scan for length-prefixed name records
        // (':' allowed) and take the trailing [u32 cnt][cnt 4x4] invBind block.
        if (skinJoints.empty() || skinInvBind.empty()) {
            skinJoints.clear(); skinInvBind.clear();
            size_t q = idxOff + (size_t)idxBytes;
            while (q + 4 <= file.size() && skinJoints.size() < 512) {
                uint16_t mk; memcpy(&mk, file.data()+q, 2);
                if (mk == 0xFFFF) { uint16_t L; memcpy(&L, file.data()+q+2, 2);
                    if (L >= 2 && L <= 64 && q+4+L <= file.size()) {
                        bool nameish = true; for (uint16_t t=0;t<L;++t){ char ch=(char)file[q+4+t]; if(!(isalnum((unsigned char)ch)||ch=='_'||ch=='-'||ch==':')){nameish=false;break;} }
                        if (nameish) { skinJoints.push_back(std::string((const char*)file.data()+q+4, L)); q += 4+L; continue; } } }
                ++q; }
            int nJ = (int)skinJoints.size();
            if (nJ > 0 && file.size() >= (size_t)nJ*64 + 4) {
                size_t cntOff = file.size() - (size_t)nJ*64 - 4;
                uint32_t cnt; memcpy(&cnt, file.data()+cntOff, 4);
                if (cnt == (uint32_t)nJ) { skinInvBind.resize((size_t)nJ*16); memcpy(skinInvBind.data(), file.data()+cntOff+4, (size_t)nJ*64); }
            }
        }
        // ── Pick the clip that best matches THIS skin's OWN joint set — NOT the last-writer-wins
        //    jointToClip map. When a full-scene skeleton (e.g. winterwonderland `rootnode`, which
        //    duplicates every banner + stringlight joint) is loaded AFTER a mesh's dedicated
        //    sub-skeleton (`banners_VFX`, `strings_lights`), the shared joint names overwrote
        //    jointToClip and bound the skin to the WRONG (superset) clip. Composing the skin's
        //    banners_VFX-bind invBind against the rootnode clip's differently-indexed joints
        //    COLLAPSED the banner to the origin (measured frame-0 centroid (-0.17,-0.07,-1.46)
        //    vs the correct (-5.06,-2.63,-42.71) under banners_VFX) — the "banners broken" +
        //    "stringlights don't animate" bug. The V79 entity graph pairs each CoSkinnedMesh with
        //    its parent CoSkeleton's clip; the joint-set best-match reproduces that GLOBALLY (the
        //    dedicated clip is the one containing all the skin's bones with the FEWEST extras).
        //    HSR_SKINCLIP_NAMEMAP restores the old behavior.
        int skinClip = -1;
        std::vector<int> boneToClipJoint(skinJoints.size(), -1);
        static int nameMap = -1; if (nameMap < 0) nameMap = std::getenv("HSR_SKINCLIP_NAMEMAP") ? 1 : 0;
        if (nameMap) {
            for (size_t b = 0; b < skinJoints.size(); ++b) {
                auto it = jointToClip.find(skinJoints[b]);
                if (it != jointToClip.end()) { boneToClipJoint[b] = it->second.second; if (skinClip < 0) skinClip = it->second.first; }
            }
        } else {
            int bestClip = -1, bestCover = 0, bestExtra = 0;
            for (int ci = 0; ci < (int)clips.size(); ++ci) {
                const AnimClip& cl = clips[ci];
                int cover = 0;
                for (auto& jn : skinJoints)
                    for (int j = 0; j < (int)cl.joints.size(); ++j) if (cl.joints[j] == jn) { ++cover; break; }
                if (cover == 0) continue;
                int extra = (int)cl.joints.size() - cover;
                if (cover > bestCover || (cover == bestCover && extra < bestExtra)) { bestCover = cover; bestExtra = extra; bestClip = ci; }
            }
            if (bestClip >= 0) {
                skinClip = bestClip; const AnimClip& cl = clips[bestClip];
                for (size_t b = 0; b < skinJoints.size(); ++b)
                    for (int j = 0; j < (int)cl.joints.size(); ++j) if (cl.joints[j] == skinJoints[b]) { boneToClipJoint[b] = j; break; }
            }
        }
        bool canSkin = (skinClip >= 0) && !skinInvBind.empty();
        // MaterialTint (mat.sanim) for SKINNED meshes — the track is keyed by the GEO name
        // ('MaterialSplit_2'), the skin by its FILENAME stem ('materialsplit_2.skin.opa'): match
        // case-insensitively. Only the static entry parse attached tints; skins never did, so the
        // stinson zen tree (8 skinned canopy layers, a 35s green→cyan→pink staggered RGBA cycle)
        // cooked UNTINTED = washed-out "generic colors".
        const std::string* skinTintKey = nullptr;
        {
            std::string stem = fn;
            size_t sl = stem.find_last_of('/'); if (sl != std::string::npos) stem = stem.substr(sl + 1);
            size_t fx = stem.rfind(".fbx.");    if (fx != std::string::npos) stem = stem.substr(fx + 5);
            size_t sk2 = stem.find(".skin");    if (sk2 != std::string::npos) stem = stem.substr(0, sk2);
            for (auto& kv : matTintAnim) {
                if (kv.first.size() != stem.size()) continue;
                bool eq = true;
                for (size_t q = 0; q < stem.size(); ++q)
                    if (tolower((unsigned char)kv.first[q]) != tolower((unsigned char)stem[q])) { eq = false; break; }
                if (eq) { skinTintKey = &kv.first; break; }
            }
        }
        int emitted = 0;
        for (auto& sub : subs) {
            if (sub.indexCount == 0 || (uint64_t)sub.firstIndex + sub.indexCount > nidx) continue;   // u64: u32 sum wraps
            // per-SUBMESH material (stinson palm fronds-vs-trunk fix): matIndex indexes the container's own
            // material table; out-of-range or single-material skins keep the first ref (old behavior).
            if (matRefs.size() > 1 && sub.matIndex < matRefs.size())
                resolveMat(matRefs[sub.matIndex], tex, mp);
            MeshData md; md.name = fn;
            if (auto sl = fn.rfind('/'); sl != std::string::npos) md.path = fn.substr(0, sl);
            std::unordered_map<uint32_t,uint32_t> remap; remap.reserve(sub.indexCount);
            md.indices.reserve(sub.indexCount);
            SkinRec sr; sr.clipIdx = skinClip;
            for (uint32_t k = 0; k < sub.indexCount; ++k) {
                uint32_t oi = sub.baseVertex + idxP[sub.firstIndex + k];
                if (oi >= nv) oi = 0;
                auto it = remap.find(oi); uint32_t ni;
                if (it == remap.end()) {
                    ni = (uint32_t)(md.positions.size()/3); remap.emplace(oi, ni);
                    float px, py, pz; memcpy(&px, posP+oi*posStride+0, 4); memcpy(&py, posP+oi*posStride+4, 4); memcpy(&pz, posP+oi*posStride+8, 4);
                    md.positions.push_back(px); md.positions.push_back(py); md.positions.push_back(pz);  // bind verts (model space)
                    float u=0, v=0;
                    if (oi < nstd) { uint16_t hu, hv; memcpy(&hu, stdP+oi*20+12, 2); memcpy(&hv, stdP+oi*20+14, 2); u=h2f(hu); v=h2f(hv); if (!novflip) v=1.0f-v; }
                    md.uvs.push_back(u); md.uvs.push_back(v);
                    // EXTRACT ALL OPA PROPERTIES: StdData a_color u8x4 @8 — authored per-vertex shading
                    // (leaf AO/color variation). The skin path DROPPED it entirely; the preview and the
                    // cook's COLOR_0 both multiply base×vertexColor, so carrying it is the 1:1 port.
                    if (oi < nstd) { const uint8_t* cc = stdP+oi*20+8;
                        md.colors.push_back(cc[0]); md.colors.push_back(cc[1]); md.colors.push_back(cc[2]); md.colors.push_back(cc[3]); }
                    else { md.colors.push_back(255); md.colors.push_back(255); md.colors.push_back(255); md.colors.push_back(255); }
                    if (canSkin) {   // SkinnedPos: weights f16x4 @12, bone idx u8x4 @20 (jidx = SKIN bone)
                        const uint8_t* sv = posP + oi*posStride;
                        for (int w=0; w<4; ++w) {
                            uint16_t hw; memcpy(&hw, sv+12+w*2, 2); float wt = h2f(hw);
                            uint8_t bi = sv[20+w];
                            bool ok = (bi < boneToClipJoint.size() && boneToClipJoint[bi] >= 0);
                            sr.jidx.push_back(ok ? (int)bi : 0); sr.jw.push_back(ok ? wt : 0.0f);
                        }
                    }
                } else ni = it->second;
                md.indices.push_back((u32)ni);
            }
            md.nVerts = (u32)(md.positions.size()/3); md.nIdx = (u32)md.indices.size();
            if (std::getenv("HSR_VERBOSE") && md.uvs.size() >= 12) {   // SOURCE uv values for cook-fidelity diff vs the shipped RENDMESH f16 stream
                fprintf(stderr, "[SKINUV] '%s' uv0..5:", fn.c_str());
                for (int q3=0; q3<6; q3++) fprintf(stderr, " (%.4f,%.4f)", md.uvs[q3*2], md.uvs[q3*2+1]);
                fprintf(stderr, "\n");
            }
            if (std::getenv("HSR_VERBOSE") && md.colors.size() >= 8) {   // authored a_color stats (is it real shading data?)
                int mn[4]={255,255,255,255}, mx[4]={0,0,0,0}; long sum[4]={0,0,0,0}; size_t n4=md.colors.size()/4;
                for (size_t q2=0;q2<n4;q2++) for (int c2=0;c2<4;c2++){ int vv2=md.colors[q2*4+c2]; if(vv2<mn[c2])mn[c2]=vv2; if(vv2>mx[c2])mx[c2]=vv2; sum[c2]+=vv2; }
                fprintf(stderr, "[SKINCOLOR] '%s' a_color r[%d..%d avg %ld] g[%d..%d avg %ld] b[%d..%d avg %ld] a[%d..%d avg %ld]\n",
                        fn.c_str(), mn[0],mx[0],sum[0]/(long)n4, mn[1],mx[1],sum[1]/(long)n4, mn[2],mx[2],sum[2]/(long)n4, mn[3],mx[3],sum[3]/(long)n4);
            }
            if (tex) { md.texW=tex->w; md.texH=tex->h; md.hasTexture=true; md.texRGBA=tex->rgba;
                       md.texSrc=tex->src;
                       md.tint[0]=md.tint[1]=md.tint[2]=1.0f; md.useBlend = mp ? (mp->transparent||mp->additive) : tex->hasAlpha;
                       md.alphaTest = mp ? mp->alphaTest : false; md.alphaCutoff = mp ? mp->alphaCutoff : 0.5f; }
            else     { md.texW=md.texH=1; md.hasTexture=true;   // no texture -> flat diffuse UNIFORM color (not grey)
                       float dr=mp?mp->diffuseColor[0]:1.f, dg=mp?mp->diffuseColor[1]:1.f, db=mp?mp->diffuseColor[2]:1.f;
                       auto cl=[](float x){return (u8)(x<0?0:x>1?255:x*255.f+0.5f);};
                       md.texRGBA={cl(dr),cl(dg),cl(db),255}; }
            if (canSkin) {   // CPU linear-blend skinning every frame (animate()) — bind pose at t=0
                md.dynamicVerts = true;
                sr.meshIdx = meshes.size(); sr.basePos = md.positions;
                sr.boneClip = boneToClipJoint; sr.invBind = skinInvBind; sr.nJoints = (int)skinJoints.size();
                skinRecs.push_back(std::move(sr));
            }
            if (skinTintKey) {   // same attach as the static entry parse (curTint streamed by animate())
                tintRecs.push_back({ meshes.size(), *skinTintKey });
                md.dynamicVerts = true;
                const TintTrack& tt = matTintAnim[*skinTintKey];
                if (tt.nFrames > 0) { md.curTint[0]=tt.rgba[0]; md.curTint[1]=tt.rgba[1];
                                      md.curTint[2]=tt.rgba[2]; md.curTint[3]=tt.rgba[3]; }
                float amn=1e9f, amx=-1e9f;
                for (int f = 0; f < tt.nFrames; ++f) { float a = tt.rgba[(size_t)f*4+3]; if (a<amn)amn=a; if (a>amx)amx=a; }
                if (tt.nFrames > 1 && amx - amn > 0.05f) md.animatedTintAlpha = true;
            }
            meshes.push_back(std::move(md)); ++emitted;
        }
        return emitted > 0;
    }

    bool parseModel(const std::vector<uint8_t>& file) {
        if (file.size() < 48 || memcmp(file.data(), "OPAA", 4) != 0) { log("not an OPAA container"); return false; }
        uint32_t hdrSize; memcpy(&hdrSize, file.data()+16, 4);
        if (hdrSize < 48 || hdrSize >= file.size()) hdrSize = 48;

        Cur c; c.d = file.data(); c.n = file.size(); c.p = hdrSize;
        uint32_t ver = c.u32();
        // Newer cooked versions (seen in 0x408 stock Meta homes, e.g. bluehillgoldmine) insert an
        // extra u32 (=1) between the version and the root type-name string. The type name is always
        // the literal "MeshData", encoded with the long-string marker 0xFFFF — so if 0xFFFF isn't
        // the very next thing, consume the inserted field. (Robust across versions, not hardcoded.)
        if (c.p + 2 <= c.n) {
            uint16_t mark; memcpy(&mark, c.d + c.p, 2);
            if (mark != 0xFFFF) c.skip(4);
        }
        std::string type = c.str();
        log("version=0x%X type=%s", ver, type.c_str());
        if (!c.ok || type != "MeshData") { log("unexpected type"); return false; }

        // ── nodes ──
        uint32_t nodeCount = c.u32();
        nodes.clear(); nodes.reserve(std::min<size_t>(nodeCount, c.n - c.p));   // count is file data: never reserve past the bytes left
        for (uint32_t i = 0; i < nodeCount && c.ok; ++i) {
            Node nd;
            c.skip(8);                 // lead 8 bytes
            nd.name = c.str();
            nd.parent = c.i32();
            for (int k = 0; k < 3; ++k) nd.t[k] = c.f32();
            // OPA stores the quaternion W-FIRST (w,x,y,z) — Meta's cooked-asset convention — but
            // trs() (like glTF) wants (x,y,z,w). Reading it in file order made the RootNode's
            // identity (1,0,0,0=w-first) parse as x=1,w=0 = a 180° rotation about X -> the WHOLE
            // home rendered upside down (spacestation, bluehillgoldmine). Reorder to (x,y,z,w).
            { float qw=c.f32(), qx=c.f32(), qy=c.f32(), qz=c.f32();
              nd.r[0]=qx; nd.r[1]=qy; nd.r[2]=qz; nd.r[3]=qw; }
            for (int k = 0; k < 3; ++k) nd.s[k] = c.f32();
            nodes.push_back(nd);
        }
        if (!c.ok) { log("node parse failed"); return false; }
        computeNodeWorld();
        // Append THIS .fbx.opa's nodes to the persistent global list (parents offset) so the animRecs
        // created below can index their OWN nodes for the whole scene's lifetime (see animNodes decl).
        size_t animNodeBase = animNodes.size();
        for (const Node& nd : nodes) {
            Node n2 = nd;
            if (n2.parent >= 0) n2.parent += (int)animNodeBase;
            animNodes.push_back(n2);
        }
        log("nodes=%u (animNodeBase=%zu)", nodeCount, animNodeBase);

        // ── materials ──
        uint32_t matCount = c.u32();
        materials.clear(); materials.reserve(std::min<size_t>(matCount, c.n - c.p));
        for (uint32_t i = 0; i < matCount && c.ok; ++i) {
            Mat mt;
            while (c.ok) {
                uint8_t tag = c.u8();
                if (tag == 0xC8) break;          // sub_AEF158: 0xC8 == end-of-object
                std::string fn = c.str();
                if (fn == "Id")        mt.id = c.u64();
                else if (fn == "Path") mt.path = c.str();
                else { log("material[%u] unknown field '%s' @%zu — cannot skip", i, fn.c_str(), c.p); return false; }
            }
            materials.push_back(mt);
        }
        if (!c.ok) { log("material parse failed"); return false; }
        log("materials=%u", matCount);

        // ── mesh entries ──
        uint32_t meshCount = c.u32();
        log("meshCount=%u", meshCount);
        // Parse mesh entries as GEOMETRY ONLY (offsets into the file). The mesh<->node placement
        // is the INSTANCE LIST that follows (read below) — NOT a 1:1 entry/node guess.
        struct SubR { uint32_t baseVertex, firstIndex, indexCount, matIndex; };
        struct MeshEntry { size_t posOff=0, stdOff=0, idxOff=0; uint32_t nv=0, nstd=0, nidx=0; bool ok=false; std::vector<SubR> subs; };
        std::vector<MeshEntry> entries; entries.reserve(std::min<size_t>(meshCount, c.n - c.p));
        for (uint32_t e = 0; e < meshCount && c.ok; ++e) {
            std::string posFmt = c.str();
            std::string dataFmt = c.str();
            uint32_t listCount = c.u32();
            if (listCount != 0) { log("entry[%u] AF0A94 list count=%u unsupported", e, listCount); return false; }
            uint32_t submeshCount = c.u32();
            MeshEntry me; me.subs.reserve(std::min<size_t>(submeshCount, c.n - c.p));
            for (uint32_t s = 0; s < submeshCount && c.ok; ++s) {
                // field[0] = baseVertex (drawIndexed vertexOffset; cooked meshes split into
                // <=65535-vert u16-indexable chunks). Then firstIndex, indexCount, matIndex.
                uint32_t baseVertex = c.u32();
                uint32_t firstIndex = c.u32();
                uint32_t indexCount = c.u32();
                uint32_t matIndex   = c.u32();
                if (ver >= 0x407) { c.u32(); c.u32(); }   // 2 extra u32 in newer cooks (0x407/0x408)
                if (ver >= 0x409) { c.u32(); c.u32(); }   // 0x409 (underwater/oceanarium) adds 2 MORE (4 total)
                c.skip(24);                                // submesh AABB (min/max vec3)
                c.u8();                                    // bool
                me.subs.push_back({baseVertex, firstIndex, indexCount, matIndex});
            }
            c.skip(24);                                    // whole-mesh AABB
            uint32_t vertCount = c.u32();                  // (== posBytes/12)
            uint32_t posBytes = c.u32(); me.posOff = c.p; c.skip(posBytes);
            uint32_t stdBytes = c.u32(); me.stdOff = c.p; c.skip(stdBytes);
            uint32_t idxCnt   = c.u32();
            std::string idxType = c.str();
            uint32_t idxBytes = c.u32(); me.idxOff = c.p; c.skip(idxBytes);
            if (ver >= 0x404) c.u16();                      // tail
            if (!c.ok) { log("entry[%u] truncated", e); return false; }
            me.nv = posBytes / 12; me.nstd = stdBytes / 20; me.nidx = idxBytes / 2;
            me.ok = (idxType == "kUnsignedShort");
            (void)vertCount; (void)dataFmt; (void)posFmt; (void)idxCnt;
            log("entry[%u] nv=%u std=%u idx=%u submeshes=%zu%s", e, me.nv, me.nstd, me.nidx,
                me.subs.size(), me.ok ? "" : " (idxType unsupported)");
            entries.push_back(std::move(me));
        }
        if (!c.ok) { log("mesh parse failed"); return false; }

        // ── INSTANCE LIST (libshell sub_AF1D74 -> sub_AF1E88): the AUTHORITATIVE mesh<->node map ──
        // [u32 count] then per instance [u32 nodeIndex][u32 meshIndex] (+3 u32 @ver>=0x407, more
        // @ver>=0x409). libshell draws mesh[meshIndex] at node[nodeIndex]'s transform. ONE mesh can
        // be instanced at MANY nodes (one car mesh -> several animated car_strip nodes); STATIC
        // meshes (vista/interior) land on static nodes -> no more "whole world moves", and props
        // (the fan) get their real node transform (on the roof, not the floor).
        struct Inst { uint32_t node, mesh, cell; };
        std::vector<Inst> instances;   // (nodeIdx, meshIdx, atlasCell)
        int atlasGrid = 1;             // >1 => per-instance variation atlas (remap UV0 into cell)
        {
            uint32_t instCount = c.ok ? c.u32() : 0;
            // Instance record size by version: 0x409 = 11 u32 ([node][mesh] + 9 extra), 0x407/0x408 = 5,
            // older = 2. The 0x409 record is 44 bytes (libshell sub_AF1D74/sub_AF1E88): [0]=node [1]=mesh
            // [3]=rotation [4]=ATLAS CELL INDEX [5]=scale [7..10]=color(rgba). field[4] is the per-instance
            // diffuse-atlas cell: the cooker bakes one texture VARIANT per instance into a square atlas, and
            // each instance samples ITS cell. The mesh's UV0 is authored in [0,1] (one cell); without the
            // remap a single coral samples the WHOLE atlas -> scrambled patchwork (the "bad texture").
            int perInst = (ver >= 0x409) ? 11 : (ver >= 0x407) ? 5 : 2;
            static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
            uint32_t f4max=0; bool f4varies=false; uint32_t f4prev=0;
            for (uint32_t i = 0; i < instCount && c.ok; ++i) {
                uint32_t rec[16] = {0};
                uint32_t nodeIdx = rec[0] = c.u32();
                uint32_t meshIdx = rec[1] = c.u32();
                for (int k = 2; k < perInst; ++k) rec[k] = c.u32();   // skip extra fields (LOD/material override)
                uint32_t cell = (perInst >= 5) ? rec[4] : 0;
                if (perInst >= 5) { if(cell>f4max)f4max=cell; if(i>0 && cell!=f4prev)f4varies=true; f4prev=cell; }
                instances.push_back({nodeIdx, meshIdx, cell});
            }
            // Square atlas grid: cells 0..f4max packed row-major in a ceil(sqrt(N))² grid (matches the
            // decoded atlases: angelfish 2x2, antlercoral 4x4, braincoral 7x7). Only meshes whose cell
            // index actually VARIES across instances are atlas-variation meshes; tiled/static props keep
            // field4==0 (no remap).
            if (f4varies) { int n = (int)f4max + 1; atlasGrid = (int)std::ceil(std::sqrt((double)n)); if (atlasGrid < 1) atlasGrid = 1; }
            log("instances=%zu (perInst=%d) atlasGrid=%d", instances.size(), perInst, atlasGrid);
        }
        // Fallback for cooks with no instance list -> 1:1 entry[i] at node[i].
        if (instances.empty())
            for (uint32_t e = 0; e < (uint32_t)entries.size(); ++e) instances.push_back({e, e, 0});

        // VAT (Vertex Animation Texture) mesh? (curOpaBase is set by the composed-scene loader)
        static int novat = -1; if (novat<0) novat = std::getenv("HSR_NOVAT") ? 1 : 0;
        const VatData* curVat = (!novat && vatByBase.count(curOpaBase)) ? &vatByBase[curOpaBase] : nullptr;

        // OPA scenes frequently instance the same material and texture across many nodes. Large
        // legacy scenes can otherwise retain hundreds of identical decoded 2K/4K RGBA buffers and
        // exhaust memory before Vulkan or the cooker is initialized. Keep one owner for plain,
        // immutable opaque base textures and let subsequent meshes borrow it via texShareSrc.
        // Materials whose pixels can differ stay on the independent path.
        std::unordered_map<const Tex*, int> opaqueBaseTextureOwner;

        // ── emit one renderable MeshData per (instance, submesh) ──
        for (auto& inst : instances) {
            uint32_t nodeIdx = inst.node, meshIdx = inst.mesh;
            // Per-instance diffuse-atlas cell remap (see INSTANCE LIST note): map this instance's
            // UV0 (authored in [0,1]) into its atlas cell so each variant shows ONE coherent texture.
            int aCol = atlasGrid>1 ? (int)(inst.cell % (uint32_t)atlasGrid) : 0;
            int aRow = atlasGrid>1 ? (int)(inst.cell / (uint32_t)atlasGrid) : 0;
            if (meshIdx >= entries.size()) continue;
            MeshEntry& me = entries[meshIdx];
            if (!me.ok || me.subs.empty()) continue;
            // Animated -> keep LOCAL verts; animate() applies the node's full animated world matrix
            // each frame. Static -> bake its full world transform into the verts. A mesh counts as
            // animated if its own node OR ANY ANCESTOR is keyed in the sanim (e.g. the bird mesh sits
            // under the animated `birdBody_path` parent — only the parent moves), matching libshell's
            // scene-graph propagation.
            bool animated = false;
            for (int an = (int)nodeIdx; an >= 0 && an < (int)nodes.size(); an = nodes[an].parent)
                if (nodeAnim.count(nodes[an].name) > 0) { animated = true; break; }
            Mat4 world = animated ? identity()
                                  : ((nodeIdx < nodeWorld.size()) ? nodeWorld[nodeIdx] : identity());
            Mat4 parentWorld = identity();
            if (animated) {
                int par = nodes[nodeIdx].parent;
                if (par >= 0 && par < (int)nodeWorld.size()) parentWorld = nodeWorld[par];
            }
            const uint8_t* posP = c.at(me.posOff);
            const uint8_t* stdP = c.at(me.stdOff);
            const uint16_t* idxP = (const uint16_t*)c.at(me.idxOff);
            uint32_t nv = me.nv, nstd = me.nstd, nidx = me.nidx;
            for (auto& sub : me.subs) {
                if (sub.indexCount == 0) continue;
                if ((uint64_t)sub.firstIndex + sub.indexCount > nidx) { log("inst mesh%u submesh range OOB", meshIdx); continue; }
                MeshData md;
                md.name = (sub.matIndex < materials.size()) ? materials[sub.matIndex].path : (type + ".sub");
                for (int an = (int)nodeIdx, guard = 0; an >= 0 && an < (int)nodes.size() && guard < 64; an = nodes[an].parent, ++guard)   // its node and the ones above
                    if (!nodes[an].name.empty()) md.path = md.path.empty() ? nodes[an].name : nodes[an].name + "/" + md.path;
                std::unordered_map<uint32_t, uint32_t> remap;
                remap.reserve(sub.indexCount);
                md.indices.reserve(sub.indexCount);
                std::vector<int> vatCols;   // VAT: per-emitted-vertex column (UV1.x) into the vatdata
                double aR=0, aG=0, aB=0; uint32_t aN=0;   // StdData a_color average (baked albedo)
                double aASum=0; uint8_t aAMin=255, aAMax=0;   // a_color ALPHA range (DBG: fog opacity?)
                for (uint32_t k = 0; k < sub.indexCount; ++k) {
                    uint32_t oi = sub.baseVertex + idxP[sub.firstIndex + k];  // baseVertex = vertexOffset
                    if (oi >= nv) { oi = 0; }
                    auto it = remap.find(oi);
                    uint32_t ni;
                    if (it == remap.end()) {
                        ni = (uint32_t)(md.positions.size() / 3);
                        remap.emplace(oi, ni);
                        // position (RigidPos f32x3). For VAT meshes keep LOCAL (animate() adds the
                        // per-frame offset then applies the instance world matrix); else world-bake.
                        float px, py, pz; memcpy(&px, posP+oi*12+0, 4); memcpy(&py, posP+oi*12+4, 4); memcpy(&pz, posP+oi*12+8, 4);
                        if (curVat) {
                            md.positions.push_back(px); md.positions.push_back(py); md.positions.push_back(pz);
                            uint16_t hc; memcpy(&hc, stdP+(oi<nstd?oi:0)*20+16, 2); int col=(int)(h2f(hc)+0.5f);  // UV1.x
                            vatCols.push_back((col>=0 && col<curVat->cols) ? col : 0);
                        } else {
                            float wp[3]; xform(world, px, py, pz, wp);
                            md.positions.push_back(wp[0]); md.positions.push_back(wp[1]); md.positions.push_back(wp[2]);
                        }
                        // uv: StdData = a_normal i16x2(@0) + a_tangent i16x2(@4) + a_color
                        // u8x4(@8) + a_texcoords f16x4(@12). UV is at offset 12 (NOT 8 — that's
                        // the color; reading it as f16 gave NaN UVs -> textures sampled garbage).
                        float u = 0, v = 0;
                        if (oi < nstd) {
                            uint16_t hu, hv; memcpy(&hu, stdP+oi*20+12, 2); memcpy(&hv, stdP+oi*20+14, 2);
                            u = h2f(hu); v = h2f(hv);
                            // V-FLIP: libshell's model shader does oTexCoord.y = 1.0 - TexCoord.y
                            // (the cooked texcoords use the bottom-up / OpenGL convention). Without
                            // this the textures map mirrored vertically = "not properly mapped".
                            // (HSR_NOVFLIP disables it for testing.)
                            static int novflip = -1;
                            if (novflip < 0) novflip = std::getenv("HSR_NOVFLIP") ? 1 : 0;
                            if (!novflip) v = 1.0f - v;
                        }
                        // Per-instance atlas cell remap: UV0 in [0,1] -> this instance's cell sub-rect.
                        if (atlasGrid > 1) {
                            float g = (float)atlasGrid;
                            u = (u + (float)aCol) / g;
                            v = (v + (float)aRow) / g;
                        }
                        md.uvs.push_back(u); md.uvs.push_back(v);
                        // uv1 (lightmap unwrap) = a_texcoords.zw @16. Sampled to bake the lightmap for
                        // textured ShellEnv shells. (For VAT meshes @16 is the column index, not a UV —
                        // harmless to store; only used when bakeLightmapVtx is set, which VAT never is.)
                        { float u1=0.f, v1=0.f;
                          if (oi < nstd) { uint16_t hu1,hv1; memcpy(&hu1,stdP+oi*20+16,2); memcpy(&hv1,stdP+oi*20+18,2);
                                           u1=h2f(hu1); v1=h2f(hv1);
                                           static int nv1=-1; if(nv1<0) nv1=std::getenv("HSR_NOVFLIP")?1:0; if(!nv1) v1=1.0f-v1; }
                          md.uvs2.push_back(u1); md.uvs2.push_back(v1); }
                        if (oi < nstd) { const uint8_t* cc = stdP+oi*20+8;   // a_color u8x4 @ offset 8
                            aR += cc[0]; aG += cc[1]; aB += cc[2]; ++aN;
                            aASum += cc[3]; if (cc[3]<aAMin) aAMin=cc[3]; if (cc[3]>aAMax) aAMax=cc[3]; }
                    } else ni = it->second;
                    md.indices.push_back((u32)ni);
                }
                md.nVerts = (u32)(md.positions.size() / 3);
                md.nIdx   = (u32)md.indices.size();
                // placeholder shading until tx_*.png.opa decode lands: stable tint per material
                uint64_t hsh = materials.empty() ? sub.matIndex : (sub.matIndex < materials.size() ? materials[sub.matIndex].id : sub.matIndex);
                md.tint[0] = 0.45f + 0.5f * (((hsh)      & 0xFF) / 255.0f);
                md.tint[1] = 0.45f + 0.5f * (((hsh >> 8) & 0xFF) / 255.0f);
                md.tint[2] = 0.45f + 0.5f * (((hsh >> 16)& 0xFF) / 255.0f);
                // Resolve base texture + blend mode the FAITHFUL way: the material's .mat.txt
                // (libshell's own description) names the diffuse texture (by AssetId or Path) and
                // declares Transparent/Additive. This generalises to ANY OPA home; we fall back to
                // the old name-prefix heuristic only when a material has no .mat.txt.
                const Tex* tex = nullptr;
                const Tex* lmTex = nullptr;
                const MatProps* mp = nullptr;
                if (sub.matIndex < materials.size()) {
                    std::string stem = matStem(materials[sub.matIndex].path);
                    auto it = matProps.find(stem);
                    if (it != matProps.end()) mp = &it->second;
                    if (mp) {
                        if (!mp->diffuseBase.empty())            tex = texByBase(mp->diffuseBase);
                        if (!tex && mp->diffuseId) {
                            auto ai = assetIdToTexBase.find(mp->diffuseId);
                            if (ai != assetIdToTexBase.end()) tex = texByBase(ai->second);
                        }
                        if (mp->lightmapId) {   // resolve the baked lightmap texture
                            auto li = assetIdToTexBase.find(mp->lightmapId);
                            if (li != assetIdToTexBase.end()) lmTex = texByBase(li->second);
                        }
                    }
                    if (!tex) {  // fallback: old prefix heuristic (spacestation-style names)
                        std::string b = matBaseName(materials[sub.matIndex].path);
                        if (!b.empty()) tex = bestTexFor(b);
                    }
                    // DBG (HSR_FOGDBG): decoded texture ALPHA range — the renderer's OWN decode of
                    // the sprite, to prove a transparent material's alpha is read faithfully. The
                    // per-texture scan is gated so it doesn't slow normal loads.
                    static int fogdbg = -1; if (fogdbg<0) fogdbg = std::getenv("HSR_FOGDBG") ? 1 : 0;
                    if (fogdbg) {
                        int txAMin=255, txAMax=0; double txASum=0; size_t txAN=0;
                        if (tex && !tex->rgba.empty())
                            for (size_t q=3; q<tex->rgba.size(); q+=4) { int a=tex->rgba[q];
                                if(a<txAMin)txAMin=a; if(a>txAMax)txAMax=a; txASum+=a; ++txAN; }
                        log("  submesh mat[%u] stem='%s' -> tex '%s' (%ux%u) blend=%d vcolRGB=(%.0f,%.0f,%.0f) vcolA[min=%u max=%u mean=%.0f] texAlpha[min=%d max=%d mean=%.1f]",
                            sub.matIndex, stem.c_str(), tex?tex->key.c_str():"<none>",
                            tex?tex->w:0, tex?tex->h:0, mp?(int)(mp->transparent||mp->additive):-1,
                            aN?aR/aN:0, aN?aG/aN:0, aN?aB/aN:0, aAMin, aAMax, aN?aASum/aN:0,
                            txAN?txAMin:0, txAN?txAMax:0, txAN?txASum/txAN:0.0);
                    } else
                    log("  submesh mat[%u] stem='%s' -> tex '%s' (%ux%u) blend=%d | diffId=%llu lmId=%llu lmTex='%s'",
                        sub.matIndex, stem.c_str(), tex?tex->key.c_str():"<none>",
                        tex?tex->w:0, tex?tex->h:0, mp?(int)(mp->transparent||mp->additive):-1,
                        (unsigned long long)(mp?mp->diffuseId:0), (unsigned long long)(mp?mp->lightmapId:0),
                        lmTex?lmTex->key.c_str():"<none>");
                    // HSR_VATDBG: flag near-BLACK textures + the mesh world centroid, to hunt down
                    // "black rectangle" meshes (a mesh whose UVs land on a black texture region).
                    static int vatdbg2 = -1; if (vatdbg2<0) vatdbg2 = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg2 && tex && !tex->rgba.empty()) {
                        double br=0; size_t np=tex->rgba.size()/4, step=np>4096?np/4096:1, cnt=0;
                        for (size_t q=0;q<np;q+=step){ br+=tex->rgba[q*4]+tex->rgba[q*4+1]+tex->rgba[q*4+2]; ++cnt; }
                        double mean = cnt? br/(cnt*3):0;
                        if (mean < 35.0) {
                            const Mat4& nw=(nodeIdx<nodeWorld.size())?nodeWorld[nodeIdx]:world;
                            double vr=aN?aR/aN:0, vg=aN?aG/aN:0, vb=aN?aB/aN:0;   // avg a_color (vertex colour)
                            log("  DARKTEX texmean=%.1f vcol=(%.0f,%.0f,%.0f) mesh#%zu stem='%s' tex='%s'",
                                mean, vr,vg,vb, meshes.size(), stem.c_str(), tex->key.c_str());
                        }
                    }
                }
                // Face culling — FAITHFUL to the material's `DoubleSided` flag (libshell back-face
                // culls single-sided materials). This matters most for TRANSPARENT cards: a
                // DoubleSided:false fog/smoke card drawn with CULL_NONE blends BOTH faces (front +
                // back), ~doubling its apparent density ("fog too visible"). Respecting the flag culls
                // the back face so each card blends once. Default stays doubleSided=true (CULL_NONE)
                // for materials with no .mat.txt, preserving prior OPA behaviour. HSR_NOOPACULL keeps
                // everything double-sided (fallback if a CW-wound mesh culls its visible face).
                // Scope to TRANSPARENT materials: that's where back-face culling has a real effect
                // (a 2-sided blended card blends both faces). For OPAQUE meshes 2-sided vs culled is
                // visually identical (front face covers the back), so we leave them CULL_NONE to avoid
                // any winding-regression risk on other OPA envs.
                static int noOpaCull = -1; if (noOpaCull<0) noOpaCull = std::getenv("HSR_NOOPACULL") ? 1 : 0;
                if (mp && !noOpaCull && (mp->transparent || mp->additive)) md.doubleSided = mp->doubleSided;
                // SpecIbl materials are env-lit by the IBL cubemap (whether or not they have an albedo).
                bool isSpecibl = false;
                if (sub.matIndex < materials.size()) { std::string mpath = materials[sub.matIndex].path;
                    for (auto& c : mpath) c=(char)tolower((unsigned char)c); isSpecibl = mpath.find("specibl")!=std::string::npos; }
                if (tex) {
                    md.texW = tex->w; md.texH = tex->h; md.hasTexture = true;
                    md.texRGBA = tex->rgba;
                    md.texSrc = tex->src;
                    // CAPTURE-CONFIRMED: no runtime IBL shader exists; textured specibl = diffuse·lightmap
                    // (set below) or diffuse-only. Old IBL path is opt-in via HSR_IBL for A/B comparison.
                    md.iblLit = isSpecibl && std::getenv("HSR_IBL");
                    md.tint[0] = md.tint[1] = md.tint[2] = 1.0f;   // texture carries the color
                    // Blend from the material spec; if no .mat.txt, fall back to the alpha scan.
                    md.useBlend = mp ? (mp->transparent || mp->additive) : tex->hasAlpha;
                    md.additive = mp ? mp->additive : false;   // god-rays/glow -> ADD blend (not alpha)
                    md.alphaTest = mp ? mp->alphaTest : false;  // cutout -> opaque pass + discard
                    md.alphaCutoff = mp ? mp->alphaCutoff : 0.5f;   // AUTHORED discard threshold (zen tree 0.25)
                    // Scale the per-mesh texture alpha by the material 'alpha' UNIFORM (libshell does
                    // this in the shader). A transparent effect with a fully-opaque texture but a low
                    // alpha uniform (forge flicker=0.27) must be a FAINT overlay, not an opaque dark
                    // box that occludes everything behind it. Only for alpha-blended (not additive).
                    if (md.useBlend && !md.additive && mp && mp->alpha < 0.999f) {
                        uint8_t a8 = (uint8_t)(mp->alpha * 255.0f + 0.5f);
                        for (size_t q = 3; q < md.texRGBA.size(); q += 4)
                            md.texRGBA[q] = (uint8_t)((md.texRGBA[q] * a8) / 255);
                    }
                    // TEXTURED interior shell that ALSO has a baked lightmap (concrete/floor/walls):
                    // libshell shades `diffuse · lightmap · lightmappower`. We sample the lightmap at uv1
                    // per vertex, scale by lightmappower, and bake it into the per-vertex colour the frag
                    // multiplies by (diffuse · vColor). lightmappower is the coloured neon/glow boost.
                    // (Only opaque shells; skip blended/additive effects.) Disables IBL (lightmap is the light).
                    if (lmTex && (lmTex->src || !lmTex->full().empty()) && !md.uvs2.empty() && !md.useBlend) {
                        // PER-PIXEL lightmap (libshell ShellEnv = diffuse·lightmap·lightmappower). The V79
                        // shader samples the 'lightmap' sampler (set2 bind3) at uv1 and multiplies; we
                        // pre-bake the per-channel lightmappower into the lightmap texels here (LDR clamp).
                        if (lmTex->src) md.lmLdrSrc = lmTex->src; else md.lmRGBA = lmTex->rgba;   // shared, still compressed
                        md.lmW = lmTex->w; md.lmH = lmTex->h;
                        md.hasLightmap = true; md.iblLit = false; md.bakeLightmapVtx = false;
                        // FAITHFUL (captured MeshShellEnv frag): lit = diffuse·lightmap·lightmappower applied
                        // IN-SHADER (HDR order, clamped once at output) via the per-mesh tint push-constant
                        // (renderer pushes UniformColor = tint*lmPow). Previously lightmappower was pre-baked
                        // into the 8-bit lightmap with an LDR clamp, which CLIPPED the HDR neon boost
                        // (concrete=[3.76,3.21,4.36]) -> washed-out/flat lighting. Keep the lightmap RAW.
                        md.lightmapPower[0]=mp?mp->lightmapPower[0]:1.f;
                        md.lightmapPower[1]=mp?mp->lightmapPower[1]:1.f;
                        md.lightmapPower[2]=mp?mp->lightmapPower[2]:1.f;
                    }
                } else if (lmTex && (lmTex->src || !lmTex->full().empty())) {
                    // No diffuse texture, but a BAKED LIGHTMAP exists. The interior SHELL meshes
                    // (divingHelmet, rubyGem, octo, loft_table/lamp, ...) bake their full lit
                    // appearance + surface detail into the lightmap. libshell shades these as
                    // `colour = basecolorFactor · lightmap`. We use the lightmap AS the base texture
                    // (sampled with uv0 = the lightmap unwrap on these no-albedo merged shells) and
                    // multiply the basecolor factor into it. This replaces the flat IBL blob with the
                    // real baked detail. Do NOT also IBL-light it — the lightmap already IS the light.
                    float lp0 = mp?mp->lightmapPower[0]:1.f, lp1 = mp?mp->lightmapPower[1]:1.f, lp2 = mp?mp->lightmapPower[2]:1.f;
                    static float lmGain = -1.f; if (lmGain<0.f){ const char* g=std::getenv("HSR_LMGAIN"); lmGain = g?(float)atof(g):1.0f; }
                    if (!md.uvs2.empty() && !std::getenv("HSR_LMBASE")) {
                        // FAITHFUL MeshShellEnv: no basecolor texture -> base = BaseColorFactor (white), lit by
                        // the lightmap sampled at uv1 (its proper unwrap) × lightmappower applied IN-SHADER via
                        // the tint push-constant (HDR, clamped once). Replaces lightmap-as-base in the uv0
                        // diffuse slot × guessed 2.5 gain (WRONG uv channel + clipped tone -> "messed up"
                        // loft_lamp). loft_lamp/helmet/gem/octo/pencil are single PBR shells; lightmap = uv1.
                        md.texW = 1; md.texH = 1; md.hasTexture = true;
                        md.texRGBA = {255,255,255,255};
                        if (lmTex->src) md.lmLdrSrc = lmTex->src; else md.lmRGBA = lmTex->rgba;   // shared, still compressed
                        md.lmW = lmTex->w; md.lmH = lmTex->h;
                        md.hasLightmap = true; md.bakeLightmapVtx = false;
                        md.lightmapPower[0]=lp0; md.lightmapPower[1]=lp1; md.lightmapPower[2]=lp2;
                    } else {
                        // Fallback: the lightmap is unwrapped in uv0 here (single UV set) -> use it as the uv0
                        // base, RAW. lightmappower applied IN-SHADER via tint (gm.lmPow) — NOT pre-baked + LDR
                        // clamped, which blew the bright baked shiny detail to WHITE (the "messed up"/textureless
                        // lamp). HSR_LMGAIN (default 1.0 = faithful) is an optional global lift folded into power.
                        md.texW = lmTex->w; md.texH = lmTex->h; md.hasTexture = true;
                        if (lmTex->src) md.texSrc = lmTex->src; else md.texRGBA = lmTex->rgba;
                        md.lightmapPower[0]=lp0*lmGain; md.lightmapPower[1]=lp1*lmGain; md.lightmapPower[2]=lp2*lmGain;
                    }
                    md.tint[0] = md.tint[1] = md.tint[2] = 1.0f;
                    md.iblLit = false; md.iblFullSpec = false;
                    static int lmdbg = -1; if (lmdbg<0) lmdbg = std::getenv("HSR_LMDBG") ? 1 : 0;
                    if (lmdbg) log("  LIGHTMAP-BASE mesh#%zu tex='%s' (%ux%u) lmPow=(%.1f,%.1f,%.1f) gain=%.1f",
                                   meshes.size(), lmTex->key.c_str(), lmTex->w, lmTex->h, lp0,lp1,lp2,lmGain);
                } else {
                    // No diffuse texture. FAITHFUL to libshell's model shader (IDA 0x1e76b0): when a
                    // material has no base-colour texture the fragment colour is the colour UNIFORM
                    // ITSELF — `lowp vec4 color = BaseColorFactor;` with NO vertex-colour modulation.
                    // So a flat ShellEnv material renders as its `diffuse` uniform: oldWest_ember =
                    // (0.78,0.53,0.24) ORANGE (was rendering BLACK because we multiplied by the ember's
                    // black a_color), black_mtl = (0,0,0). The ONLY surfaces that need the StdData
                    // a_color are SpecIbl ground/terrain whose `diffuse` is the neutral [1,1,1] TINT and
                    // whose baked albedo lives in the vertex colour — detect that (tint≈white) and use
                    // the vertex colour there; otherwise the uniform is the colour.
                    md.texW = 1; md.texH = 1; md.hasTexture = true;
                    if (isSpecibl && std::getenv("HSR_WHITEDBG")) {
                        std::string mstem = (sub.matIndex<materials.size()) ? materials[sub.matIndex].path : std::string("?");
                        uint64_t did = mp?mp->diffuseId:0, lid = mp?mp->lightmapId:0;
                        auto di = assetIdToTexBase.find(did); auto li2 = assetIdToTexBase.find(lid);
                        log("  WHITEDBG mesh#%zu mat='%s' mp=%d diffId=%llu(map=%d tex=%d) lmId=%llu(map=%d tex=%d) diffBase='%s'",
                            meshes.size(), mstem.c_str(), (int)(mp!=nullptr),
                            (unsigned long long)did, (int)(di!=assetIdToTexBase.end()), (int)(di!=assetIdToTexBase.end() && texByBase(di->second)!=nullptr),
                            (unsigned long long)lid, (int)(li2!=assetIdToTexBase.end()), (int)(li2!=assetIdToTexBase.end() && texByBase(li2->second)!=nullptr),
                            (mp?mp->diffuseBase.c_str():""));
                    }
                    bool transp = mp && (mp->transparent || mp->additive);
                    float dr = mp ? mp->diffuseColor[0] : 1.f, dg = mp ? mp->diffuseColor[1] : 1.f, db = mp ? mp->diffuseColor[2] : 1.f;
                    bool tint = (dr>0.97f && dg>0.97f && db>0.97f);   // diffuse==white => pure tint, real colour is a_color
                    float vr=1.f, vg=1.f, vb=1.f;
                    if (tint && aN) { vr=(float)aR/aN/255.f; vg=(float)aG/aN/255.f; vb=(float)aB/aN/255.f; }
                    auto cl = [](float x){ return (u8)(x<0?0:x>1?255:x*255.f+0.5f); };
                    if (transp) { md.texRGBA = {255,255,255,0}; md.useBlend = true; }
                    else        { md.texRGBA = {cl(vr*dr), cl(vg*dg), cl(vb*db), 255};
                                  // CAPTURE-CONFIRMED (Frida rip of the live shell across all 26 envs): home
                                  // meshes render ONLY through MeshShellEnv, which has NO IBL/specibl/fresnel
                                  // branch. The env `_ibl/*_hdri_*.dds` cubemaps are baked into the `lightmap`
                                  // (Modmap) at COOK time, not sampled at runtime. So a no-texture specibl mesh
                                  // = BaseColorFactor (the `diffuse` uniform) -> the vr/vg/vb·dr/dg/db colour we
                                  // just wrote, identical to a flat ShellEnv material. The old phantom split-sum
                                  // IBL path rendered WHITE when no cubemap was loaded; keep it opt-in (HSR_IBL).
                                  if (isSpecibl && mp && std::getenv("HSR_IBL")) {
                                      md.iblLit = true; md.iblFullSpec = true;
                                      md.texRGBA = {255,255,255,255};
                                      md.metallic = mp->metallic; md.roughness = mp->roughness;
                                      md.speciblDiffScale = mp->speciblDiffScale; md.speciblSpecScale = mp->speciblSpecScale;
                                      md.albedoFactor[0]=dr; md.albedoFactor[1]=dg; md.albedoFactor[2]=db;
                                  } }
                }
                // Animated node -> stream world positions every frame (renderer keeps the VBO
                // mapped). Record the LOCAL base positions; animate(t) rewrites md.positions.
                if (animated) {
                    md.dynamicVerts = true;
                    animRecs.push_back({ meshes.size(), (uint32_t)(animNodeBase + nodeIdx), parentWorld, md.positions });
                }
                // UV/flipbook material animation (mat.sanim) keyed by this mesh's geo/node name.
                if (nodeIdx < nodes.size() && matUVAnim.count(nodes[nodeIdx].name)) {
                    md.dynamicVerts = true;   // positions stay baked; UVs stream each frame
                    uvAnimRecs.push_back({ meshes.size(), nodes[nodeIdx].name, md.uvs });
                    static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg) {
                        const Mat4& nw = (nodeIdx < nodeWorld.size()) ? nodeWorld[nodeIdx] : parentWorld;
                        float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                        for(size_t q=0;q+1<md.uvs.size();q+=2){float u=md.uvs[q],v=md.uvs[q+1];if(u<u0)u0=u;if(u>u1)u1=u;if(v<v0)v0=v;if(v>v1)v1=v;}
                        log("  UVANIM mesh#%zu node='%s' fr=%d W=(%.1f,%.1f,%.1f) UV0 u[%.2f,%.2f] v[%.2f,%.2f]",
                            meshes.size(), nodes[nodeIdx].name.c_str(), matUVAnim[nodes[nodeIdx].name].nFrames, nw.m[12],nw.m[13],nw.m[14], u0,u1,v0,v1);
                    }
                }
                // MaterialTint (per-frame RGBA opacity) — keyed by the SAME geo/node name. The fog
                // (fog_0X_geo_Y) and dust carry an alpha 0..~0.22 fade; apply it as md.curTint each
                // frame so the fragment shader multiplies it in (faithful: frag = texture * tint).
                if (nodeIdx < nodes.size() && matTintAnim.count(nodes[nodeIdx].name)) {
                    tintRecs.push_back({ meshes.size(), nodes[nodeIdx].name });
                    md.dynamicVerts = true;   // so the renderer streams curTint each frame
                    const TintTrack& tt = matTintAnim[nodes[nodeIdx].name];
                    if (tt.nFrames > 0) { md.curTint[0]=tt.rgba[0]; md.curTint[1]=tt.rgba[1];
                                          md.curTint[2]=tt.rgba[2]; md.curTint[3]=tt.rgba[3]; }
                    // If the tint ALPHA actually fades (min≈0, real swing), this is an ALPHA-BLEND that
                    // relies on alpha — flag it so the renderer never routes it to the hard-additive
                    // (src=ONE) pipeline (which ignores alpha and freezes the fog/dust at full opacity).
                    { float amn=1e9f, amx=-1e9f; for (int f=0; f<tt.nFrames; ++f) { float a=tt.rgba[(size_t)f*4+3]; if(a<amn)amn=a; if(a>amx)amx=a; }
                      if (tt.nFrames > 1 && amx - amn > 0.05f) md.animatedTintAlpha = true; }
                }
                // VAT: keep LOCAL basePos + per-vertex column; animate() adds the per-frame offset
                // and applies the instance world matrix (animate(0) places them at the rest pose).
                if (curVat && vatCols.size() == md.positions.size()/3) {
                    md.dynamicVerts = true;
                    VatRec vr; vr.meshIdx = meshes.size(); vr.basePos = md.positions;
                    vr.col = std::move(vatCols); vr.world = world; vr.vd = curVat;
                    vatRecs.push_back(std::move(vr));
                    if ((int)curVat->frames > animMaxFrames) animMaxFrames = curVat->frames;
                }
                {   // HSR_VATDBG: dump UV0 range for VAT meshes (one atlas cell vs whole atlas?)
                    static int vatdbg = -1; if (vatdbg<0) vatdbg = std::getenv("HSR_VATDBG") ? 1 : 0;
                    if (vatdbg && curVat && !md.uvs.empty()) {
                        float u0=1e9f,u1=-1e9f,v0=1e9f,v1=-1e9f;
                        double cx=0,cy=0,cz=0; size_t nP=md.positions.size()/3;
                        for (size_t q=0;q+1<md.uvs.size();q+=2){ float u=md.uvs[q],v=md.uvs[q+1]; if(u<u0)u0=u;if(u>u1)u1=u;if(v<v0)v0=v;if(v>v1)v1=v; }
                        for (size_t q=0;q<nP;++q){ cx+=md.positions[q*3];cy+=md.positions[q*3+1];cz+=md.positions[q*3+2]; }
                        if(nP){cx/=nP;cy/=nP;cz/=nP;}
                        float wc[3]; xform(world,(float)cx,(float)cy,(float)cz,wc);
                        log("  VATDBG mesh#%zu '%s' UV0 u[%.3f,%.3f] v[%.3f,%.3f] verts=%zu worldC=(%.2f,%.2f,%.2f)", meshes.size(), curOpaBase.c_str(), u0,u1,v0,v1, nP, wc[0],wc[1],wc[2]);
                    }
                }
                const bool shareOpaqueBase = tex && md.hasTexture && !md.useBlend && !md.additive &&
                    !md.alphaTest && !md.hasLightmap && !md.hasNormal && !md.hasOrm &&
                    !md.hasEmissive && !md.hasVat && md.normalRGBA.empty() && md.ormRGBA.empty() &&
                    md.emissiveRGBA.empty() && md.lmRGBA.empty() && md.vatRaw.empty();
                if (shareOpaqueBase) {
                    auto owner = opaqueBaseTextureOwner.find(tex);
                    if (owner == opaqueBaseTextureOwner.end()) {
                        opaqueBaseTextureOwner.emplace(tex, static_cast<int>(meshes.size()));
                    } else {
                        md.texShareSrc = owner->second;
                        std::vector<u8>().swap(md.texRGBA);
                        std::vector<u8>().swap(md.srcAstc);
                        std::vector<u8>().swap(md.astcRaw);
                    }
                }
                meshes.push_back(std::move(md));
            }
        }
        log("emitted %zu renderable submeshes from %zu instances (%zu anim)",
            meshes.size(), instances.size(), animRecs.size());
        return !meshes.empty();
    }
};
