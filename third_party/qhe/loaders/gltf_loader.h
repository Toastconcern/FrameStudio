#pragma once
// V79 ".gltf.ovrscene" loader — standard glTF 2.0 (V9.gltf + V9.bin + ASTC8x8 KTX1
// textures), the raw format old VR-env APKs ship (e.g. The Incredibles). Produces the
// renderer's MeshData[] (positions/uvs/indices + decoded base-color RGBA), with each
// node's world transform baked into the positions. This is the source-format side of the
// V79 -> new-system env editor/porter.
//
// Container nesting: APK -> assets/scene.zip -> _WORLD_MODEL.gltf.ovrscene (zip) ->
//   V9.gltf (JSON), V9.bin (buffer), *.ktx (ASTC 8x8).

#include "core/types.h"
#include "core/load_progress.h"   // live stage/counter for the loading splash
#include "core/tinyjson.h"
#include "loaders/gltf_material_rules.h"
#include "loaders/rendtxtr_parser.h"   // astc::decodeASTC
#include "loaders/zip_reader_guard.h"
#include "loaders/scene_zip.h"
#include "miniz.h"
#define STBI_NO_STDIO
#include "stb_image.h"         // JPEG skybox panoramas (impl in src/stb_image_impl.c)
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <utility>   // std::swap (mirrored-instance winding flip)

class GltfLoader {
public:
    std::vector<MeshData> meshes;
    bool verbose = true;

    // ── glTF skeletal animation (self-contained CPU skinning — our own code, not
    //    borrowed from libshell). animate(t) streams skinned positions per frame. ──
    struct GNode { float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1}; int parent=-1; std::string name; };
    struct GSkin { std::vector<int> joints; std::vector<float> ibm; /*joints*16*/ };
    struct GSampler { std::vector<float> times; std::vector<float> vals; int comps=0; bool step=false; };
    struct GChannel { int node=-1, path=0, sampler=-1; };  // path: 0=T 1=R 2=S
    struct SkinnedRec { int meshIdx=-1, skin=-1; u32 nv=0;
                        std::vector<float> basePos;  // nv*3 (local)
                        std::vector<u8>  jidx;       // nv*4
                        std::vector<float> jw; };     // nv*4
    // A NON-skinned mesh attached to an animated node (or a node with an animated ancestor):
    // its positions are kept LOCAL and the node's world matrix is applied per-frame (rigid
    // node animation — e.g. a rotating object, the AmongUs dropship parts). Without this such
    // envs bake the node transform once at load -> they never move.
    struct NodeAnimRec { int meshIdx=-1, nodeIdx=-1; std::vector<float> basePos; // basePos = local nv*3
                         // animateRuntime: rigid = drawn as md.animXform over the t=0 pose (-1 = not decided yet;
                         // 0 = its node is singular at t=0 or the mesh is also skinned: vertices rewritten)
                         int rigid = -1; float rest[16] = {}, restInv[16] = {}; bool atRest = false; };
    // animate(t) rewrites every animated vertex (the cook samplers read them); animateRuntime(t) is the per-frame
    // play: rigid node motion becomes md.animXform, so those meshes' vertices never change.
    bool gpuRigid_ = false;
    void animateRuntime(float t) { gpuRigid_ = true; animate(t); gpuRigid_ = false; }
    static bool put(float& d, float v) { if (d == v) return false; d = v; return true; }
    std::vector<GNode> gnodes;
    std::vector<GSkin> gskins;
    std::vector<GChannel> gchannels;
    std::vector<GSampler> gsamplers;
    std::vector<SkinnedRec> skinned;
    mutable std::vector<float> nodePeriodv;   // per-node loop period = latest key among the node's OWN channels (lazy)
    // Build the per-node loop periods (V79 per-track semantics; 0 = no wrap). Shared by animate() AND every
    // cook extractor so the BAKE samples channels exactly like the preview plays them — the bake used raw t,
    // so a joint channel ending at 13.3s CLAMPED (froze) inside a 166.8s clip while the preview WRAPPED it
    // (bubbles clusters: source keeps rotating, cook rotated once then froze = "not even close to the
    // original rotation"). Late-start channels stay unwrapped (global-choreography segments).
    void ensureNodePeriods() const {
        if (nodePeriodv.size() == gnodes.size()) return;
        nodePeriodv.assign(gnodes.size(), 0.f);
        for (auto& ch : gchannels)
            if (ch.node>=0 && (size_t)ch.node<gnodes.size() && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                auto& tv = gsamplers[ch.sampler].times;
                if (!tv.empty() && tv.back() > nodePeriodv[ch.node]) nodePeriodv[ch.node] = tv.back(); }
        for (auto& ch : gchannels)
            if (ch.node>=0 && (size_t)ch.node<gnodes.size() && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                auto& tv = gsamplers[ch.sampler].times;
                if (!tv.empty() && tv.front() > 0.05f) nodePeriodv[ch.node] = 0.f; }
    }
    // Per-node wrapped sample time: each node's channels loop on the node's OWN track length (lockstep T/R/S),
    // exactly like the OPA sampleTrack rule. HSR_GLTF_GLOBALLOOP reverts to the single global wrap.
    float chTime(int node, float t) const {
        static int gLoopGlobal = -1; if (gLoopGlobal<0) gLoopGlobal = std::getenv("HSR_GLTF_GLOBALLOOP")?1:0;
        if (gLoopGlobal) return t;
        ensureNodePeriods();
        if (node>=0 && (size_t)node<nodePeriodv.size()){
            float p = nodePeriodv[node];
            if (p > 1e-4f && p < animDuration - 1e-3f) return fmodf(t, p); }
        return t;
    }
    std::vector<NodeAnimRec> nodeAnimRecs;
    // Re-anchor for animated "screen" skins authored away from their target (Rick&Morty TV: the
    // glTF puts the TV_Anim flipbook node by the WINDOW, libshell repositions it onto the TV at
    // runtime). HSR_REANCHOR="dx,dy,dz" translates skinned meshes onto the TV. (Off by default.)
    float reanchor[3] = {0,0,0};
    float animDuration = 0.0f;
    bool hasAnimation() const { return animDuration > 0.0f && (!skinned.empty() || !nodeAnimRecs.empty()); }

    void log(const char* fmt, ...) {
        if (!verbose) return;
        va_list a; va_start(a, fmt); fprintf(stderr, "[GLTF] "); vfprintf(stderr, fmt, a);
        fprintf(stderr, "\n"); va_end(a);
    }

    // ── small zip helper: read one entry from an in-memory zip ──
    static bool zipIsSafe(const void* zipData, size_t zipSize,
                          const hslcook::zipsafety::ArchiveReadLimits& limits,
                          std::string* error = nullptr) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, zipData, zipSize, 0)) return false;
        const bool safe = zipread::validateArchive(z, limits, error);
        mz_zip_reader_end(&z);
        return safe;
    }

    static std::vector<u8> zipRead(const void* zipData, size_t zipSize, const std::string& name) {
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, zipData, zipSize, 0)) return {};
        int idx = mz_zip_reader_locate_file(&z, name.c_str(), nullptr, 0);
        std::vector<u8> out;
        if (idx >= 0) {
            size_t sz = 0; void* d = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
            if (d) { out.assign((u8*)d, (u8*)d + sz); mz_free(d); }
        }
        mz_zip_reader_end(&z);
        return out;
    }
    // list entries ending with suffix
    static std::vector<std::string> zipList(const void* zipData, size_t zipSize) {
        std::vector<std::string> names;
        mz_zip_archive z; memset(&z, 0, sizeof(z));
        if (!mz_zip_reader_init_mem(&z, zipData, zipSize, 0)) return names;
        u32 n = mz_zip_reader_get_num_files(&z);
        for (u32 i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&z, i, &st)) names.push_back(st.m_filename);
        }
        mz_zip_reader_end(&z);
        return names;
    }

    // ── KTX1 (ASTC) → RGBA mip0 ──
    static bool decodeKtxBaseMip(const std::vector<u8>& ktx, std::vector<u8>& rgba, u32& outW, u32& outH) {
        if (ktx.size() < 64) return false;
        static const u8 id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        if (memcmp(ktx.data(), id, 12) != 0) return false;
        auto u32a = [&](size_t o){ return loadUnaligned<u32>(ktx.data()+o); };
        u32 glInternalFormat = u32a(28);
        u32 w = u32a(36), h = u32a(40);
        u32 bytesOfKeyValueData = u32a(60);
        size_t off = 64 + bytesOfKeyValueData;        // first mip: [u32 imageSize][data]
        if (off + 4 > ktx.size()) return false;
        u32 imageSize = u32a(off); off += 4;
        if (off + imageSize > ktx.size()) imageSize = (u32)(ktx.size() - off);
        // ASTC block footprint from glInternalFormat. The GL ASTC enum is a regular
        // sequence: linear LDR = 0x93B0..0x93BD, sRGB = 0x93D0..0x93DD, both in the
        // SAME order of 14 footprints. libshell reads the footprint straight from the
        // KTX glInternalFormat — so we must cover ALL of them, not just the square
        // ones. The previous switch defaulted every non-square footprint (e.g. the
        // dome floor atlas, ASTC_8x6 = 0x93B6) to 8x8, which mis-strides the block
        // grid and scrambles the whole texture (512x683 blocks decoded as 512x512).
        static const u8 kFootprints[14][2] = {
            {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
            {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
        };
        u32 bw = 8, bh = 8;
        int idx = -1;
        if (glInternalFormat >= 0x93B0 && glInternalFormat <= 0x93BD) idx = (int)(glInternalFormat - 0x93B0);
        else if (glInternalFormat >= 0x93D0 && glInternalFormat <= 0x93DD) idx = (int)(glInternalFormat - 0x93D0);
        if (idx >= 0) { bw = kFootprints[idx][0]; bh = kFootprints[idx][1]; }
        if (!astc::decodeASTC(ktx.data()+off, imageSize, w, h, bw, bh, rgba)) return false;
        outW = w; outH = h;
        return true;
    }

    // ── KTX1 (ASTC) -> its cooked mip chain, still compressed (the renderer uploads ASTC natively, or decodes in the
    //    background). Each level is [u32 imageSize][blocks]; the prefixes are dropped. ──
    static std::shared_ptr<TexSource> ktxSource(const std::vector<u8>& ktx) {
        if (ktx.size() < 64) return nullptr;
        static const u8 id[12] = {0xAB,'K','T','X',' ','1','1',0xBB,'\r','\n',0x1A,'\n'};
        if (memcmp(ktx.data(), id, 12) != 0) return nullptr;
        auto u32a = [&](size_t o){ u32 v; memcpy(&v, ktx.data()+o, 4); return v; };
        const u32 fmt = u32a(28), w = u32a(36), h = u32a(40), faces = u32a(52), levels = std::max<u32>(1, u32a(56));
        int idx = -1;
        if (fmt >= 0x93B0 && fmt <= 0x93BD) idx = (int)(fmt - 0x93B0);
        else if (fmt >= 0x93D0 && fmt <= 0x93DD) idx = (int)(fmt - 0x93D0);
        if (idx < 0 || faces > 1 || !w || !h) return nullptr;
        static const u8 kFootprints[14][2] = {
            {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},
            {10,5},{10,6},{10,8},{10,10},{12,10},{12,12}
        };
        auto src = std::make_shared<TexSource>();
        src->w = w; src->h = h; src->bw = kFootprints[idx][0]; src->bh = kFootprints[idx][1];
        size_t off = 64 + (size_t)u32a(60);
        u32 lw = w, lh = h, n = 0;
        for (; n < levels && off + 4 <= ktx.size(); ++n) {
            const size_t want = (size_t)((lw + src->bw - 1) / src->bw) * ((lh + src->bh - 1) / src->bh) * 16;
            const u32 size = u32a(off); off += 4;
            if (size != want || off + size > ktx.size()) break;
            src->astc.insert(src->astc.end(), ktx.begin() + off, ktx.begin() + off + size);
            off += (size + 3) & ~3u;
            lw = std::max(1u, lw / 2); lh = std::max(1u, lh / 2);
        }
        if (!n) return nullptr;
        src->mips = n;
        return src;
    }

    // ── PNG (8-bit, colortype 0/2/3/4/6, non-interlaced) -> RGBA. Many V79 envs (e.g. Luigi's
    //    Mansion) reference image/png by uri instead of KTX/ASTC; without this they were all-gray
    //    (textures never decoded). Uses miniz tinfl for the zlib IDAT inflate. ──
    static bool decodePNG(const std::vector<u8>& png, std::vector<u8>& rgba, u32& outW, u32& outH) {
        if (png.size()<8 || png[0]!=0x89 || png[1]!='P' || png[2]!='N' || png[3]!='G') return false;
        auto rd32=[&](size_t o)->u32{ return ((u32)png[o]<<24)|((u32)png[o+1]<<16)|((u32)png[o+2]<<8)|(u32)png[o+3]; };
        size_t p=8; u32 w=0,h=0; int bitDepth=0,colorType=0,interlace=0;
        std::vector<u8> idat, palette, trns;
        while (p+8 <= png.size()) {
            u32 len=rd32(p);
            char t0=(char)png[p+4],t1=(char)png[p+5],t2=(char)png[p+6],t3=(char)png[p+7];
            size_t d=p+8; if (d+len > png.size()) break;
            if (t0=='I'&&t1=='H'&&t2=='D'&&t3=='R'&&len>=13){ w=rd32(d); h=rd32(d+4); bitDepth=png[d+8]; colorType=png[d+9]; interlace=png[d+12]; }
            else if (t0=='P'&&t1=='L'&&t2=='T'&&t3=='E') palette.assign(png.begin()+d, png.begin()+d+len);
            else if (t0=='t'&&t1=='R'&&t2=='N'&&t3=='S') trns.assign(png.begin()+d, png.begin()+d+len);
            else if (t0=='I'&&t1=='D'&&t2=='A'&&t3=='T') idat.insert(idat.end(), png.begin()+d, png.begin()+d+len);
            else if (t0=='I'&&t1=='E'&&t2=='N'&&t3=='D') break;
            p = d + len + 4;  // + CRC
        }
        // dims cap keeps (stride+1)*h below size_t overflow (a wrapped product would pass the length check)
        if (!w||!h||w>16384||h>16384||idat.empty()||bitDepth!=8||interlace!=0) return false;
        int ch = (colorType==2)?3:(colorType==6)?4:(colorType==0)?1:(colorType==4)?2:(colorType==3)?1:0;
        if (!ch) return false;
        size_t rawLen=0;
        void* raw = tinfl_decompress_mem_to_heap(idat.data(), idat.size(), &rawLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (!raw) return false;
        const u8* f=(const u8*)raw;
        size_t stride=(size_t)w*ch;
        if (rawLen < (stride+1)*h) { mz_free(raw); return false; }
        std::vector<u8> img((size_t)w*h*ch);
        auto pae=[](int a,int b,int c)->int{ int pp=a+b-c,pa=pp>a?pp-a:a-pp,pb=pp>b?pp-b:b-pp,pc=pp>c?pp-c:c-pp; return (pa<=pb&&pa<=pc)?a:(pb<=pc?b:c); };
        for (u32 y=0;y<h;++y){
            const u8* s=f+(size_t)y*(stride+1); u8 filt=s[0]; s++;
            u8* dst=img.data()+(size_t)y*stride; const u8* prev=(y>0)?img.data()+(size_t)(y-1)*stride:nullptr;
            for (size_t x=0;x<stride;++x){
                int a=(x>=(size_t)ch)?dst[x-ch]:0, b=prev?prev[x]:0, c=(prev&&x>=(size_t)ch)?prev[x-ch]:0, val=s[x];
                if(filt==1)val+=a; else if(filt==2)val+=b; else if(filt==3)val+=(a+b)/2; else if(filt==4)val+=pae(a,b,c);
                dst[x]=(u8)val;
            }
        }
        mz_free(raw);
        rgba.resize((size_t)w*h*4);
        for (size_t i=0;i<(size_t)w*h;++i){
            u8 r=200,g=200,bb=200,a=255;
            if (ch==3){r=img[i*3];g=img[i*3+1];bb=img[i*3+2];}
            else if (ch==4){r=img[i*4];g=img[i*4+1];bb=img[i*4+2];a=img[i*4+3];}
            else if (ch==2){r=g=bb=img[i*2];a=img[i*2+1];}
            else if (colorType==3){int idx=img[i]; if((size_t)idx*3+2<palette.size()){r=palette[idx*3];g=palette[idx*3+1];bb=palette[idx*3+2];} if((size_t)idx<trns.size())a=trns[idx];}
            else {r=g=bb=img[i];}
            rgba[i*4]=r; rgba[i*4+1]=g; rgba[i*4+2]=bb; rgba[i*4+3]=a;
        }
        outW=w; outH=h; return true;
    }

    // ── 4x4 matrix helpers (column-major, glTF convention) ──
    struct Mat4 { float m[16]; };
    static Mat4 identity() { Mat4 r{}; r.m[0]=r.m[5]=r.m[10]=r.m[15]=1; return r; }
    static Mat4 mul(const Mat4&a, const Mat4&b){ Mat4 r{}; for(int c=0;c<4;c++)for(int row=0;row<4;row++){float s=0;for(int k=0;k<4;k++)s+=a.m[k*4+row]*b.m[c*4+k]; r.m[c*4+row]=s;} return r; }
    static Mat4 fromTRS(const float t[3], const float q[4], const float s[3]) {
        float x=q[0],y=q[1],z=q[2],w=q[3];
        float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z), r02=2*(x*z+w*y);
        float r10=2*(x*y+w*z), r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
        float r20=2*(x*z-w*y), r21=2*(y*z+w*x), r22=1-2*(x*x+y*y);
        Mat4 r{};
        r.m[0]=r00*s[0]; r.m[1]=r10*s[0]; r.m[2]=r20*s[0];  r.m[3]=0;
        r.m[4]=r01*s[1]; r.m[5]=r11*s[1]; r.m[6]=r21*s[1];  r.m[7]=0;
        r.m[8]=r02*s[2]; r.m[9]=r12*s[2]; r.m[10]=r22*s[2]; r.m[11]=0;
        r.m[12]=t[0];    r.m[13]=t[1];    r.m[14]=t[2];     r.m[15]=1;
        return r;
    }
    static void xformPoint(const Mat4& m, float x, float y, float z, float out[3]) {
        out[0]=m.m[0]*x+m.m[4]*y+m.m[8]*z+m.m[12];
        out[1]=m.m[1]*x+m.m[5]*y+m.m[9]*z+m.m[13];
        out[2]=m.m[2]*x+m.m[6]*y+m.m[10]*z+m.m[14];
    }

    // Any stb_image-supported format -> RGBA: JPEG (skybox panoramas, e.g. hogwarts pano.jpg) AND the
    // PNG variants our minimal decodePNG rejects — sub-8-bit palette (riftdark/riftlight 1-bit 1024²
    // grid_plane_007.png shipped an UNTEXTURED floor), 16-bit, interlaced. A 20 MB equirect panorama
    // can decode to a very large image; cap the longest side at 4096 (box-downsample) so the GPU
    // upload stays sane while the sky still looks crisp on a sphere.
    static bool decodeSTB(const std::vector<u8>& jpg, std::vector<u8>& rgba, u32& outW, u32& outH) {
        int w=0,h=0,comp=0;
        stbi_uc* px = stbi_load_from_memory(jpg.data(), (int)jpg.size(), &w, &h, &comp, 4);
        if (!px || w<=0 || h<=0) { if (px) stbi_image_free(px); return false; }
        const int CAP = 4096;
        int mx = w>h ? w : h;
        if (mx > CAP) {
            int nw = (int)((int64_t)w*CAP/mx), nh = (int)((int64_t)h*CAP/mx);
            if (nw<1) nw=1; if (nh<1) nh=1;
            rgba.resize((size_t)nw*nh*4);
            for (int y=0;y<nh;++y){ int sy=(int)((int64_t)y*h/nh);
                for (int x=0;x<nw;++x){ int sx=(int)((int64_t)x*w/nw);
                    const stbi_uc* s=px+((size_t)sy*w+sx)*4; u8* d=rgba.data()+((size_t)y*nw+x)*4;
                    d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3]; } }
            outW=(u32)nw; outH=(u32)nh;
        } else {
            rgba.assign(px, px+(size_t)w*h*4); outW=(u32)w; outH=(u32)h;
        }
        stbi_image_free(px);
        return true;
    }

    // Entry: load from the APK path. Returns true if it IS a V79 gltf.ovrscene scene.
    bool load(const std::string& apkPath) {
        const std::shared_ptr<const std::vector<u8>> sceneZipHold = scenezip::get(apkPath);   // shared: scene_zip.h
        if (!sceneZipHold) return false;
        const std::vector<u8>& sceneZip = *sceneZipHold;
        {
            std::string archiveError;
            if (!zipIsSafe(sceneZip.data(), sceneZip.size(),
                           hslcook::zipsafety::kSceneReadLimits, &archiveError)) {
                log("rejected unsafe or oversized scene.zip: %s", archiveError.c_str());
                return false;
            }
        }

        // Find the *.gltf.ovrscene inside scene.zip
        std::string ovrName;
        for (auto& n : zipList(sceneZip.data(), sceneZip.size()))
            if (n.size()>9 && n.substr(n.size()-9)==".ovrscene") { ovrName=n; break; }
        if (ovrName.empty()) return false;   // not a V79 ovrscene env
        log("V79 ovrscene: %s", ovrName.c_str());
        auto ovr = zipRead(sceneZip.data(), sceneZip.size(), ovrName);
        if (ovr.empty()) return false;
        {
            std::string archiveError;
            if (!zipIsSafe(ovr.data(), ovr.size(),
                           hslcook::zipsafety::kSceneReadLimits, &archiveError)) {
                log("rejected unsafe or oversized ovrscene: %s", archiveError.c_str());
                return false;
            }
        }

        // Inside the ovrscene zip: V9.gltf + V9.bin + *.ktx
        std::string gltfName, binName;
        std::unordered_map<std::string, std::string> ktxByName;  // image uri -> entry name
        std::vector<std::string> ovrEntries = zipList(ovr.data(), ovr.size());
        for (auto& n : ovrEntries) {
            std::string low=n; for(auto&c:low)c=(char)tolower((unsigned char)c);
            if (low.size()>5 && low.substr(low.size()-5)==".gltf") gltfName=n;
            else if (low.size()>4 && low.substr(low.size()-4)==".bin") binName=n;
        }
        if (gltfName.empty()) { log("no .gltf inside ovrscene"); return false; }
        auto gltfBytes = zipRead(ovr.data(), ovr.size(), gltfName);
        std::string gltfJson((char*)gltfBytes.data(), gltfBytes.size());
        std::vector<u8> bin = binName.empty() ? std::vector<u8>{} : zipRead(ovr.data(), ovr.size(), binName);
        log("gltf=%s (%zuB) bin=%s (%zuB)", gltfName.c_str(), gltfBytes.size(), binName.c_str(), bin.size());

        tinyjson::Value root;
        try { root = tinyjson::parse(gltfJson); } catch(...) { log("gltf JSON parse failed"); return false; }

        // Decode all images (KTX) up front -> RGBA cache by image index.
        // Deferred (Frame Home, Studio): KTX stays compressed, one source shared by every mesh using it. Decoding every
        // image to RGBA and copying it into each mesh cost the apk-release homes 0.4-1.2 GB and made them lag.
        struct Img { std::vector<u8> rgba; u32 w=1,h=1; bool ok=false; std::shared_ptr<TexSource> src; std::vector<u8> ktx; };
        std::vector<Img> images;
        if (root.has("images")) {
            const auto& imgs = root["images"];
            g_loadProgress.set("Decoding textures...", 0, (int)imgs.size());
            for (size_t i=0;i<imgs.size();++i) {
                g_loadProgress.tick((int)i);
                Img im;
                std::string uri = imgs[i].has("uri") ? imgs[i]["uri"].asString() : "";
                if (!uri.empty()) {
                    auto kd = zipRead(ovr.data(), ovr.size(), uri);
                    std::string lu=uri; for(auto&c:lu)c=(char)tolower((unsigned char)c);
                    auto endsWith=[&](const char* s){ size_t n=strlen(s); return lu.size()>n && lu.compare(lu.size()-n,n,s)==0; };
                    bool isPng = endsWith(".png");
                    bool isJpg = endsWith(".jpg") || endsWith(".jpeg");
                    bool ok=false;
                    if (!kd.empty()) {
                        // JPEG (skybox panoramas, e.g. hogwarts pano.jpg) via stb_image; PNG (Luigi's
                        // etc.) custom decoder; else KTX/ASTC. Try the likely one first, then fall back.
                        if (isJpg) ok = decodeSTB(kd, im.rgba, im.w, im.h);
                        if (!ok && g_deferTextures && !isPng && (im.src = ktxSource(kd))) {
                            ok = true; im.w = im.src->w; im.h = im.src->h; im.ktx = std::move(kd);
                        }
                        if (!ok) {
                            if (isPng) ok = decodePNG(kd, im.rgba, im.w, im.h) || decodeKtxBaseMip(kd, im.rgba, im.w, im.h)
                                              || decodeSTB(kd, im.rgba, im.w, im.h);   // stb catches PNGs decodePNG can't (1-bit palette etc.)
                            else       ok = decodeKtxBaseMip(kd, im.rgba, im.w, im.h) || decodePNG(kd, im.rgba, im.w, im.h)
                                              || decodeSTB(kd, im.rgba, im.w, im.h);
                        }
                    }
                    im.ok=ok;
                    if (ok) log("  image[%zu] %s %ux%u%s", i, uri.c_str(), im.w, im.h, isJpg?" (jpg)":(isPng?" (png)":""));
                    else    log("  image[%zu] %s decode FAILED", i, uri.c_str());
                }
                images.push_back(std::move(im));
            }
        }
        g_loadProgress.set("Building meshes...");
        // material -> image index (via baseColorTexture -> textures[].source)
        auto matBaseImage = [&](int matIdx) -> int {
            if (matIdx < 0 || !root.has("materials")) return -1;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return -1;
            const auto& m = mats[matIdx];
            int texIdx = -1;
            if (m.has("pbrMetallicRoughness") && m["pbrMetallicRoughness"].has("baseColorTexture"))
                texIdx = (int)m["pbrMetallicRoughness"]["baseColorTexture"]["index"].asInt();
            else if (m.has("emissiveTexture")) texIdx = (int)m["emissiveTexture"]["index"].asInt();
            if (texIdx < 0 || !root.has("textures")) return -1;
            const auto& texs = root["textures"];
            if ((size_t)texIdx >= texs.size()) return -1;
            if (texs[texIdx].has("source")) return (int)texs[texIdx]["source"].asInt();
            return -1;
        };
        // Solid material color for textureless materials: glTF baseColorFactor (else
        // emissiveFactor) — LINEAR floats, so convert to sRGB bytes for the sRGB texture.
        // (The Incredibles "buildings"/logo/cubes are solid emissive-colored materials.)
        auto lin2srgb = [](float c)->u8 {
            c = c<0?0:(c>1?1:c);
            float s = (c<=0.0031308f) ? 12.92f*c : 1.055f*powf(c,1.f/2.4f)-0.055f;
            int v=(int)(s*255.f+0.5f); return (u8)(v<0?0:v>255?255:v);
        };
        auto matSolidColor = [&](int matIdx, u8 out[4]) -> bool {
            out[0]=out[1]=out[2]=200; out[3]=255;
            if (matIdx < 0 || !root.has("materials")) return false;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return false;
            const auto& m = mats[matIdx];
            const tinyjson::Value* f = nullptr;
            if (m.has("pbrMetallicRoughness") && m["pbrMetallicRoughness"].has("baseColorFactor"))
                f = &m["pbrMetallicRoughness"]["baseColorFactor"];
            else if (m.has("emissiveFactor")) f = &m["emissiveFactor"];
            if (!f) return false;
            for (int i=0;i<3 && i<(int)f->size();++i) out[i]=lin2srgb((float)(*f)[i].asFloat());
            if (f->size()>=4) out[3]=(u8)((float)(*f)[3].asFloat()*255.f+0.5f);
            return true;
        };
        // Transparency: glTF alphaMode == "BLEND" (or a translucent baseColorFactor alpha)
        // -> alpha-blend this primitive (e.g. the Incredibles purple environment dome).
        // alphaMode == "MASK" is NOT blend: it is the AUTHORED alpha-test CUTOUT (discard below
        // alphaCutoff) that draws in the OPAQUE pass and WRITES DEPTH (libshell HAS_ALPHACUTOFF /
        // MeshShellEnvMaskDepthWrite). It used to be "approximated with alpha-blend" from before the
        // renderer had a discard pipeline; that sent authored-MASK fences/gates/grass/vines to the
        // no-depth-write blend pass whenever the bimodal-texture rescue missed (>=5% mid-alpha texels)
        // -> per-mesh back-to-front sorting artifacts (sqgraveyard "depth issue"). matIsMask now routes
        // them to the cutout pipeline (and the cook ships them as depth-writing cutout MATLs).
        auto matIsBlend = [&](int matIdx) -> bool {
            if (matIdx < 0 || !root.has("materials")) return false;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return false;
            const auto& m = mats[matIdx];
            if (m.has("alphaMode")) { std::string am = m["alphaMode"].asString(); if (am=="BLEND") return true; if (am=="MASK") return false; }
            if (m.has("pbrMetallicRoughness") && m["pbrMetallicRoughness"].has("baseColorFactor")) {
                const auto& f = m["pbrMetallicRoughness"]["baseColorFactor"];
                if (f.size() >= 4 && (float)f[3].asFloat() < 0.99f) return true;
            }
            return false;
        };
        // Authored alpha-test cutout (alphaMode == "MASK") + its authored discard threshold
        // (glTF spec default 0.5; the cook honors the exact value, em.alphaCutoff).
        auto matIsMask = [&](int matIdx) -> bool {
            if (matIdx < 0 || !root.has("materials")) return false;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return false;
            const auto& m = mats[matIdx];
            return m.has("alphaMode") && m["alphaMode"].asString()=="MASK";
        };
        auto matAlphaCutoff = [&](int matIdx) -> float {
            if (matIdx < 0 || !root.has("materials")) return 0.5f;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return 0.5f;
            const auto& m = mats[matIdx];
            float c = m.has("alphaCutoff") ? (float)m["alphaCutoff"].asFloat() : 0.5f;
            return (c > 0.f && c < 1.f) ? c : 0.5f;
        };
        // glTF face culling: a material is single-sided unless doubleSided==true (spec default false).
        // Single-sided -> the renderer back-face culls (libshell-faithful). The cel-shading OUTLINE
        // material (inverted black hull) is single-sided; culling it makes it show as edge rims
        // instead of a solid blob that blacks out the whole interior.
        auto matIsDoubleSided = [&](int matIdx) -> bool {
            if (matIdx < 0 || !root.has("materials")) return true;   // no material -> treat as 2-sided
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return true;
            const auto& m = mats[matIdx];
            return m.has("doubleSided") && m["doubleSided"].asBool();
        };
        // ADDITIVE glow: a BLEND material driven by an EMISSION shader (emissiveFactor ~[1,1,1]) is a self-lit
        // GLOW (warp streaks, nebula haze, force-field, fire), NOT alpha-glass. Alpha-blended faint on dark
        // backdrops -> nearly invisible on device. Such meshes must
        // ADD light (src+dst), not alpha-lerp. Real transparent glass exports emissiveFactor=[0,0,0] -> stays
        // alpha. (Blender's glTF exporter writes emissiveFactor=[1,1,1] for an Emission/unlit material.)
        auto matIsEmissiveGlow = [&](int matIdx) -> bool {
            if (matIdx < 0 || !root.has("materials")) return false;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return false;
            const auto& m = mats[matIdx];
            if (!m.has("emissiveFactor")) return false;
            const auto& ef = m["emissiveFactor"];
            float mx = 0.f; for (int i = 0; i < 3 && i < (int)ef.size(); ++i) { float v = (float)ef[i].asFloat(); if (v > mx) mx = v; }
            return mx >= 0.5f;
        };
        // A BLEND material with a baseColorTexture is a TRANSLUCENT TEXTURED surface (the omnidroid SHIELD: Shield2-Shield
        // alpha = the transparency), NOT a pure additive glow. Its texture's ALPHA is authoritative -> keep ALPHA-BLEND with
        // the NATIVE alpha (no opacity-lift, no additive). Only a textureless emissive BLEND (pure Emission) is the additive glow.
        auto matHasBaseTex = [&](int matIdx) -> bool {
            if (matIdx < 0 || !root.has("materials")) return false;
            const auto& mats = root["materials"];
            if ((size_t)matIdx >= mats.size()) return false;
            const auto& m = mats[matIdx];
            return m.has("pbrMetallicRoughness") && m["pbrMetallicRoughness"].has("baseColorTexture");
        };

        const auto& accessors  = root.has("accessors") ? root["accessors"] : tinyjson::Value();
        const auto& bufferViews= root.has("bufferViews") ? root["bufferViews"] : tinyjson::Value();
        // Does [off, off + (count-1)*stride + elem) lie inside bin? Every offset/count/stride is file data, so the
        // math is overflow-safe (subtractions only after the bound is known).
        auto binRangeOk = [&](size_t off, u64 count, size_t stride, size_t elem) -> bool {
            if (!count) return true;
            if (off > bin.size() || elem > bin.size() - off) return false;
            return stride == 0 ? count == 1 : count - 1 <= (bin.size() - off - elem) / stride;
        };
        auto jsonSize = [](const tinyjson::Value& v) -> size_t {   // negative/huge -> SIZE_MAX (fails range checks)
            const int64_t x = v.asInt(); return x < 0 ? SIZE_MAX : (size_t)x;
        };
        // read a numeric accessor as floats (handles VEC2/VEC3, float or normalized int)
        auto readAccessorF = [&](int acc, int comps, std::vector<float>& out) -> u32 {
            out.clear();
            if (acc < 0 || (size_t)acc >= accessors.size()) return 0;
            const auto& a = accessors[acc];
            int bv = a.has("bufferView") ? (int)a["bufferView"].asInt() : -1;
            if (bv < 0 || (size_t)bv >= bufferViews.size()) return 0;
            const auto& v = bufferViews[bv];
            size_t bvOff = v.has("byteOffset") ? jsonSize(v["byteOffset"]) : 0;
            size_t stride = v.has("byteStride") ? jsonSize(v["byteStride"]) : 0;
            size_t accOff = a.has("byteOffset") ? jsonSize(a["byteOffset"]) : 0;
            int ct = (int)a["componentType"].asInt();   // 5126 float,5123 u16,5125 u32,5121 u8,5122 i16,5120 i8
            const int64_t count64 = a["count"].asInt();
            if (count64 < 0 || count64 > 0xFFFFFFFFll || bvOff > SIZE_MAX / 2 || accOff > SIZE_MAX / 2 || stride > 255) return 0;
            u32 count = (u32)count64;
            // glTF "normalized": integer accessors map to [0,1] (unsigned) or [-1,1] (signed).
            // CRITICAL for skinning: WEIGHTS_0 is often normalized u8/u16 — read raw (0..255) it
            // blows skinned verts to infinity (the "chaos" on rigged characters). Also applies to
            // normalized UV/COLOR. (JOINTS_0 is never normalized -> raw indices, handled by flag.)
            bool norm = a.has("normalized") && a["normalized"].asBool();
            size_t compSize = (ct==5126||ct==5125)?4:(ct==5123||ct==5122)?2:1;
            if (!stride) stride = compSize*comps;
            if (!binRangeOk(bvOff + accOff, count, stride, compSize*comps)) return 0;   // malformed: treat as absent
            out.resize((size_t)count*comps);
            const u8* base = bin.data() + bvOff + accOff;
            for (u32 i=0;i<count;++i) {
                const u8* p = base + (size_t)i*stride;
                for (int c=0;c<comps;++c) {
                    const u8* e = p + c*compSize;
                    float f=0;
                    if (ct==5126) f = loadUnaligned<float>(e);
                    else if (ct==5123) { f = loadUnaligned<u16>(e); if (norm) f /= 65535.0f; }
                    else if (ct==5125) f = (float)loadUnaligned<u32>(e);
                    else if (ct==5121) { f = *e; if (norm) f /= 255.0f; }
                    else if (ct==5122) { f = loadUnaligned<int16_t>(e); if (norm) { f /= 32767.0f; if (f<-1.f) f=-1.f; } }
                    else { f = (float)loadUnaligned<int8_t>(e); if (norm) { f /= 127.0f; if (f<-1.f) f=-1.f; } }
                    out[(size_t)i*comps+c] = f;
                }
            }
            return count;
        };
        auto readIndices = [&](int acc, std::vector<u32>& out) {
            out.clear();
            if (acc < 0 || (size_t)acc >= accessors.size()) return;
            const auto& a = accessors[acc];
            int bv = (int)a["bufferView"].asInt();
            if (bv < 0 || (size_t)bv >= bufferViews.size()) return;
            const auto& v = bufferViews[bv];
            size_t bvOff = v.has("byteOffset") ? jsonSize(v["byteOffset"]) : 0;
            size_t accOff = a.has("byteOffset") ? jsonSize(a["byteOffset"]) : 0;
            int ct = (int)a["componentType"].asInt();
            const int64_t count64 = a["count"].asInt();
            if (count64 < 0 || count64 > 0xFFFFFFFFll || bvOff > SIZE_MAX / 2 || accOff > SIZE_MAX / 2) return;
            u32 count = (u32)count64;
            const size_t isz = ct==5125 ? 4 : ct==5123 ? 2 : 1;
            if (!binRangeOk(bvOff + accOff, count, isz, isz)) return;
            const u8* p = bin.data() + bvOff + accOff;
            out.resize(count);
            for (u32 i=0;i<count;++i)
                out[i] = (ct==5125) ? loadUnaligned<u32>(p+i*4)        // UNSIGNED_INT (no truncation!)
                       : (ct==5123) ? (u32)loadUnaligned<u16>(p+i*2)   // UNSIGNED_SHORT
                                    : (u32)p[i];                                   // UNSIGNED_BYTE
        };

        const auto& nodes = root.has("nodes") ? root["nodes"] : tinyjson::Value();
        const auto& gmeshes = root.has("meshes") ? root["meshes"] : tinyjson::Value();

        // ── Parse skeleton/animation state for CPU skinning ──
        gnodes.assign(nodes.size(), GNode{});
        for (size_t i=0;i<nodes.size();++i) {
            const auto& nd = nodes[i];
            if (nd.has("name"))        gnodes[i].name=nd["name"].asString();
            if (nd.has("translation")) for(int k=0;k<3;k++) gnodes[i].t[k]=(float)nd["translation"][k].asFloat();
            if (nd.has("rotation"))    for(int k=0;k<4;k++) gnodes[i].r[k]=(float)nd["rotation"][k].asFloat();
            if (nd.has("scale"))       for(int k=0;k<3;k++) gnodes[i].s[k]=(float)nd["scale"][k].asFloat();
            if (nd.has("children")) for(size_t c=0;c<nd["children"].size();++c) {
                int ch=(int)nd["children"][c].asInt(); if(ch>=0&&(size_t)ch<gnodes.size()) gnodes[ch].parent=(int)i;
            }
        }
        // TV-animation RE-ANCHOR: the Rick&Morty livingroom glTF authors its animated screen node
        // (`TV_Anim`, a 100-frame skinned flipbook) over by the WINDOW (~+Z), not on the TV; libshell
        // repositions it onto the TV at runtime (not in the glTF/markup). Replicate that: when the
        // `TV_Anim` node is present, shift the skinned screen onto the TV. HSR_REANCHOR overrides.
        for (size_t i=0;i<root["nodes"].size();++i) {
            if (root["nodes"][i].has("name") && root["nodes"][i]["name"].asString()=="TV_Anim") {
                reanchor[0]=-3.2f; reanchor[1]=-0.2f; reanchor[2]=-2.0f;
                log("glTF: TV_Anim animated screen -> re-anchored onto the TV (%.2f,%.2f,%.2f)", reanchor[0],reanchor[1],reanchor[2]);
                break;
            }
        }
        if (root.has("skins")) {
            const auto& sk = root["skins"];
            for (size_t i=0;i<sk.size();++i) {
                GSkin g;
                if (sk[i].has("joints")) for(size_t j=0;j<sk[i]["joints"].size();++j) g.joints.push_back((int)sk[i]["joints"][j].asInt());
                if (sk[i].has("inverseBindMatrices")) readAccessorF((int)sk[i]["inverseBindMatrices"].asInt(), 16, g.ibm);
                gskins.push_back(std::move(g));
            }
        }
        // Read ALL animation clips, not just [0]. V79 ambient envs split their motion across
        // many clips (e.g. AmongUs: clip0=navRing spin, clips1-7=the crewmates) and play them
        // ALL at once. Merge every clip's samplers+channels onto one timeline (sampler indices
        // offset per clip); loop at the longest clip duration. (Reading only clip0 = most of an
        // env's animation silently missing — the AmongUs "no animation" bug.)
        if (root.has("animations")) {
            const auto& anims = root["animations"];
            for (size_t ai=0; ai<anims.size(); ++ai) {
                const auto& an = anims[ai];
                int samplerBase = (int)gsamplers.size();
                if (an.has("samplers")) for (size_t s=0;s<an["samplers"].size();++s) {
                    const auto& sm = an["samplers"][s];
                    GSampler gs;
                    readAccessorF((int)sm["input"].asInt(), 1, gs.times);
                    const auto& outA = accessors[(int)sm["output"].asInt()];
                    std::string tp = outA.has("type") ? outA["type"].asString() : "VEC3";
                    gs.comps = (tp=="VEC4")?4:(tp=="SCALAR")?1:3;
                    // glTF interpolation: STEP = hold the previous keyframe (no blend). Flipbooks
                    // (e.g. the Rick&Morty animated-TV: 100 STEP scale tracks toggling frame-quads
                    // 0<->1) MUST step — LINEAR-blending them fades/ghosts frames into each other
                    // ("jumping images, not proper animation"). CUBICSPLINE we approximate as LINEAR.
                    gs.step = sm.has("interpolation") && sm["interpolation"].asString()=="STEP";
                    readAccessorF((int)sm["output"].asInt(), gs.comps, gs.vals);
                    // sampleSampler indexes vals[key*comps]: drop keys whose output values are missing.
                    if (gs.times.size() > gs.vals.size() / gs.comps) gs.times.resize(gs.vals.size() / gs.comps);
                    if (gs.times.empty()) gs.vals.clear();
                    for (float tt : gs.times) if (tt>animDuration) animDuration=tt;
                    gsamplers.push_back(std::move(gs));
                }
                if (an.has("channels")) for (size_t c=0;c<an["channels"].size();++c) {
                    const auto& ch = an["channels"][c];
                    GChannel gc; gc.sampler = samplerBase + (int)ch["sampler"].asInt();
                    gc.node=(int)ch["target"]["node"].asInt();
                    std::string path=ch["target"]["path"].asString();
                    gc.path = (path=="rotation")?1:(path=="scale")?2:0;
                    gchannels.push_back(gc);
                }
            }
            log("animation: %zu clips, %zu channels, %zu samplers, duration=%.2fs",
                anims.size(), gchannels.size(), gsamplers.size(), animDuration);
        }

        // Which nodes are animated (directly targeted by a channel, OR have an animated
        // ancestor)? A mesh under such a node must NOT have its transform baked — it gets the
        // node's world matrix streamed per frame.
        std::vector<char> nodeDirectAnim(gnodes.size(), 0);
        for (auto& ch : gchannels) if (ch.node>=0 && (size_t)ch.node<nodeDirectAnim.size()) nodeDirectAnim[ch.node]=1;
        auto nodeAnimated = [&](int n) -> bool {
            int guard=0;
            while (n>=0 && (size_t)n<gnodes.size() && guard++<4096) {
                if (nodeDirectAnim[n]) return true;
                n = gnodes[n].parent;
            }
            return false;
        };

        // Emit a MeshData for each primitive of a node's mesh, world transform baked in.
        // Children come from the file: a cycle would recurse forever and a deep chain would overflow the stack.
        std::vector<char> onPath(nodes.size(), 0);
        int visitDepth = 0;
        // path: the nodes above it in the file, '/'-separated (MeshData::path)
        std::function<void(int, const Mat4&, const std::string&)> visit = [&](int nodeIdx, const Mat4& parent, const std::string& path) {
            if (nodeIdx < 0 || (size_t)nodeIdx >= nodes.size() || onPath[nodeIdx] || visitDepth >= 256) return;
            struct PathGuard { char& on; int& d; PathGuard(char& o, int& dd) : on(o), d(dd) { on = 1; ++d; } ~PathGuard() { on = 0; --d; } } guard(onPath[nodeIdx], visitDepth);
            const auto& nd = nodes[nodeIdx];
            Mat4 local = identity();
            if (nd.has("matrix")) {
                const auto& mm = nd["matrix"];
                for (int i=0;i<16 && i<(int)mm.size();++i) local.m[i]=(float)mm[i].asFloat();
            } else {
                float t[3]={0,0,0}, q[4]={0,0,0,1}, s[3]={1,1,1};
                if (nd.has("translation")) for(int i=0;i<3;i++) t[i]=(float)nd["translation"][i].asFloat();
                if (nd.has("rotation"))    for(int i=0;i<4;i++) q[i]=(float)nd["rotation"][i].asFloat();
                if (nd.has("scale"))       for(int i=0;i<3;i++) s[i]=(float)nd["scale"][i].asFloat();
                local = fromTRS(t,q,s);
            }
            Mat4 world = mul(parent, local);
            // glTF spec: when the determinant of a node's GLOBAL transform is NEGATIVE (a mirrored
            // instance — 3ds Max-style exports bake mirrors as scale -1, e.g. sqgraveyard's
            // Gravestones_*/Names_R/Nmaes_L/Walls/tombGrave: rot180X x scale(1,-1,1) = a Z-mirror),
            // the winding order of derived triangle faces MUST be inverted. We bake `world` into the
            // vertex positions, so the flip must be baked into the INDICES too; without it back-face
            // culling eats the mirrored meshes' FRONT faces (see-through stones, "depth issue") and
            // one-sided decals show only their BACK — the flat name plaques faced DOWN, readable only
            // mirrored from below while z-fighting the slab ("text on wrong side").
            float wdet = world.m[0]*(world.m[5]*world.m[10]-world.m[6]*world.m[9])
                       - world.m[4]*(world.m[1]*world.m[10]-world.m[2]*world.m[9])
                       + world.m[8]*(world.m[1]*world.m[6]-world.m[2]*world.m[5]);
            bool mirroredWinding = wdet < 0.f;
            if (nd.has("mesh")) {
                int mi = (int)nd["mesh"].asInt();
                if ((size_t)mi < gmeshes.size()) {
                    const auto& prims = gmeshes[mi]["primitives"];
                    std::string nm = nd.has("name") ? nd["name"].asString()
                                   : (gmeshes[mi].has("name") ? gmeshes[mi]["name"].asString() : "mesh");
                    for (size_t p=0;p<prims.size();++p) {
                        const auto& prim = prims[p];
                        const auto& attr = prim["attributes"];
                        int posAcc = attr.has("POSITION") ? (int)attr["POSITION"].asInt() : -1;
                        int uvAcc  = attr.has("TEXCOORD_0") ? (int)attr["TEXCOORD_0"].asInt() : -1;
                        if (posAcc < 0) continue;
                        MeshData md; md.name = nm; md.path = path;
                        std::vector<float> pos; u32 nv = readAccessorF(posAcc, 3, pos);
                        if (!nv) continue;
                        // A mesh is SKINNED only if it has JOINTS_0/WEIGHTS_0 with ACTUAL non-zero weights. A mesh can carry
                        // empty JOINTS_0/WEIGHTS_0 (all-zero weights) that the exporter auto-added — that is NOT a real skin
                        // and must render STATIC. The incredibles "Logo" (Circle.007) is exactly this: node.skin=None AND every
                        // weight is 0.0 (verified) -> static. (The old "inflates via a scaling joint" note was a misdiagnosis;
                        // zero weights can't deform.) Cooking a zero-weight mesh as skinned makes the device reject it
                        // (MeshDefinition::fix verification fail). When node.skin is absent but real weights exist, fall back to
                        // skin[0]. [[project_hsl_v203_animation_format]]
                        int skinIdx = nd.has("skin") ? (int)nd["skin"].asInt() : (gskins.empty() ? -1 : 0);
                        std::vector<float> jf, wf;
                        bool isSkinned = false;
                        if (skinIdx >= 0 && attr.has("JOINTS_0") && attr.has("WEIGHTS_0")) {
                            readAccessorF((int)attr["WEIGHTS_0"].asInt(), 4, wf);
                            for (float w : wf) if (w > 1e-4f) { isSkinned = true; break; }   // any real weight -> skinned
                        }
                        md.positions.resize((size_t)nv*3);
                        if (isSkinned) {
                            // Don't bake: keep LOCAL positions; capture skin data. animate(t)
                            // streams skinned world positions per frame (bind pose at t=0).
                            SkinnedRec rec; rec.skin=skinIdx; rec.nv=nv; rec.basePos=pos;
                            readAccessorF((int)attr["JOINTS_0"].asInt(), 4, jf);
                            rec.jidx.resize((size_t)nv*4); rec.jw.resize((size_t)nv*4);
                            for (u32 i=0;i<nv*4;++i){ rec.jidx[i]=(i<jf.size())?(u8)(jf[i]+0.5f):0; rec.jw[i]=(i<wf.size())?wf[i]:0.f; }
                            // Loose (unweighted) verts are left AS-AUTHORED by default (faithful). Two ways to repair the
                            // "static whale-eyes" case: the interactive right-click "Bind loose verts to nearest joint"
                            // (applyLooseVertBindForMesh, live) or HSR_AUTOSKINWEIGHT at load (headless/scripted). Both call
                            // the same autoBindLooseVertsRec.
                            if (std::getenv("HSR_AUTOSKINWEIGHT")) {
                                int f=autoBindLooseVertsRec(rec);
                                if (f && std::getenv("HSR_VERBOSE")) fprintf(stderr, "[GLTF-SKIN] '%s' auto-bound %d UNWEIGHTED verts -> nearest weighted neighbor\n", nd.has("name")?nd["name"].asString().c_str():"?", f);
                            }
                            rec.meshIdx=(int)meshes.size();
                            md.dynamicVerts=true; md.gltfMeshIndex=(int)skinned.size();
                            for (u32 i=0;i<nv*3;++i) md.positions[i]=pos[i];   // placeholder; animate fixes
                            skinned.push_back(std::move(rec));
                        } else if (nodeAnimated(nodeIdx)) {
                            // Rigid node animation: keep LOCAL positions, stream node world per frame.
                            NodeAnimRec nrec; nrec.meshIdx=(int)meshes.size(); nrec.nodeIdx=nodeIdx; nrec.basePos=pos;
                            md.dynamicVerts=true;
                            for (u32 i=0;i<nv*3;++i) md.positions[i]=pos[i];   // local placeholder; animate fixes
                            nodeAnimRecs.push_back(std::move(nrec));
                        } else {
                            for (u32 i=0;i<nv;++i) {
                                float o[3]; xformPoint(world, pos[i*3], pos[i*3+1], pos[i*3+2], o);
                                md.positions[i*3]=o[0]; md.positions[i*3+1]=o[1]; md.positions[i*3+2]=o[2];
                            }
                        }
                        std::vector<float> uv;
                        if (uvAcc>=0) readAccessorF(uvAcc, 2, uv);
                        md.uvs.resize((size_t)nv*2);
                        // Read UVs exactly as authored — libshell's ModelFile_glTF.cpp does NO
                        // U/V flip and GlTexture.cpp uploads the KTX as-is. (The dome floor's
                        // "wrong textures" turned out to be an ASTC-footprint bug in the KTX
                        // decoder — 8x6 blocks mis-strided as 8x8 — not a UV flip.)
                        for (u32 i=0;i<nv;++i){
                            md.uvs[i*2]   = (i*2  <uv.size()) ? uv[i*2]   : 0.f;
                            md.uvs[i*2+1] = (i*2+1<uv.size()) ? uv[i*2+1] : 0.f; }
                        if (prim.has("indices")) readIndices((int)prim["indices"].asInt(), md.indices);
                        else { md.indices.resize(nv); for(u32 i=0;i<nv;++i) md.indices[i]=i; }
                        // Malformed index data: an index past the vertex count would read past every vertex
                        // array downstream -> clamp it into range (degenerate triangle).
                        for (u32& ix : md.indices) if (ix >= nv) ix = nv - 1;
                        // Mirrored instance (negative-det world, baked above) -> invert triangle winding
                        // per the glTF spec. Skinned meshes keep authored winding (their positions come
                        // from joint matrices, not this bake).
                        if (mirroredWinding && !isSkinned)
                            for (size_t t=0; t+2<md.indices.size(); t+=3) std::swap(md.indices[t+1], md.indices[t+2]);
                        md.nVerts=nv; md.nIdx=(u32)md.indices.size();
                        // base color texture
                        int matIdx = prim.has("material") ? (int)prim["material"].asInt() : -1;
                        int imgIdx = matBaseImage(matIdx);
                        if (imgIdx>=0 && (size_t)imgIdx<images.size() && images[imgIdx].ok) {
                            Img& im = images[imgIdx];
                            md.texW=im.w; md.texH=im.h; md.hasTexture=true;
                            if (im.src) md.texSrc = im.src; else md.texRGBA = im.rgba;
                            // FORCE-FIELD GLOW: an UNLIT shader shows only base color, so a material whose emissive is a flat
                            // CONSTANT (emissiveFactor, NO emissiveTexture) loses its glow. The omnidroid SHIELD = a faint
                            // Shield texture + a purple emissive [0.74,0.20,1.0]; baked as base+emissive it becomes a visible
                            // glowing translucent force-field. SKIP when emissive is a texture (the body: emissive=base,
                            // already shown) or there's no base texture (solid-color Logo keeps its factor color).
                            if (matIdx>=0 && root.has("materials") && (size_t)matIdx<root["materials"].size()) {
                                const auto& mm = root["materials"][matIdx];
                                if (mm.has("emissiveFactor") && !mm.has("emissiveTexture")) {
                                    const auto& ef = mm["emissiveFactor"];
                                    float e[3]={ ef.size()>0?(float)ef[0].asFloat():0.f, ef.size()>1?(float)ef[1].asFloat():0.f, ef.size()>2?(float)ef[2].asFloat():0.f };
                                    if (md.texSrc && (e[0]>0.004f||e[1]>0.004f||e[2]>0.004f)) {
                                        // The glow edits pixels: this mesh gets its own decoded copy.
                                        u32 dw=0, dh=0;
                                        if (decodeKtxBaseMip(im.ktx, md.texRGBA, dw, dh)) { md.texSrc.reset(); md.texW=dw; md.texH=dh; }
                                    }
                                    if (!md.texSrc && (e[0]>0.004f||e[1]>0.004f||e[2]>0.004f)) {
                                        // glow luminance — drives both the baked color AND, for BLEND meshes, the OPACITY:
                                        // a glowing surface should stay VISIBLE through alpha-blend, so lift alpha toward
                                        // opaque proportional to the glow (the omnidroid SHIELD's texture alpha ~25% made it
                                        // semi-invisible). GENERAL: applies to ANY constant-emissive blend mesh in any env.
                                        float lum = 0.299f*e[0]+0.587f*e[1]+0.114f*e[2]; if (lum>1.f) lum=1.f; if (lum<0.f) lum=0.f;
                                        // Don't lift opacity for a TRANSLUCENT TEXTURED surface (the SHIELD): its texture alpha
                                        // IS the intended transparency — lifting it (prior fix) made the purple dome OPAQUE (user).
                                        bool blendMat = mm.has("alphaMode") && mm["alphaMode"].asString()=="BLEND" && !matHasBaseTex(matIdx);
                                        const float GLOW = 0.4f;   // gentle glow — full-strength emissive baked into base was TOO BRIGHT (user). HSR_GLOW overrides.
                                        float gk = std::getenv("HSR_GLOW") ? (float)atof(std::getenv("HSR_GLOW")) : GLOW;
                                        for (size_t p=0;p+3<md.texRGBA.size();p+=4) {
                                            for (int k=0;k<3;k++){ int vv=md.texRGBA[p+k]+(int)(e[k]*255.f*gk+0.5f); md.texRGBA[p+k]=(u8)(vv>255?255:vv); }
                                            if (blendMat){ int a=md.texRGBA[p+3]; md.texRGBA[p+3]=(u8)(a+(int)((255-a)*lum*gk+0.5f)); }  // lift opacity by glow
                                        }
                                    }
                                }
                            }
                        } else {
                            // No texture: use the material's solid baseColorFactor/emissive color.
                            u8 c[4]; matSolidColor(matIdx, c);
                            md.texRGBA={c[0],c[1],c[2],c[3]}; md.texW=md.texH=1; md.hasTexture=true;
                        }
                        md.alphaTest = matIsMask(matIdx);  // authored MASK -> opaque-pass depth-writing cutout (discard)
                        if (md.alphaTest) md.alphaCutoff = matAlphaCutoff(matIdx);
                        md.useBlend = !md.alphaTest && matIsBlend(matIdx);  // alpha-blend transparent materials
                        // TEX-AUDIT provenance: the RAW authored mode + factor alpha, untouched by any heuristic.
                        md.srcAlphaMode = 0; md.srcFactorA = 1.f;
                        if (matIdx >= 0 && root.has("materials") && (size_t)matIdx < root["materials"].size()) {
                            const auto& mm2 = root["materials"][matIdx];
                            if (mm2.has("alphaMode")) { std::string am2 = mm2["alphaMode"].asString();
                                md.srcAlphaMode = (am2=="BLEND") ? 2 : (am2=="MASK") ? 1 : 0; }
                            if (mm2.has("pbrMetallicRoughness") && mm2["pbrMetallicRoughness"].has("baseColorFactor")) {
                                const auto& f2 = mm2["pbrMetallicRoughness"]["baseColorFactor"];
                                if (f2.size() >= 4) md.srcFactorA = (float)f2[3].asFloat();
                            }
                        } else md.srcAlphaMode = -1;
                        md.additive = md.useBlend && matIsEmissiveGlow(matIdx) && !matHasBaseTex(matIdx);  // emissive BLEND = additive glow ONLY for a textureLESS pure-Emission material; a textured BLEND (the SHIELD) keeps alpha-blend so its texture alpha = the transparency (additive discarded it = opaque purple dome)
                        md.doubleSided = matIsDoubleSided(matIdx);  // single-sided -> back-face cull
                        // This mesh's OWN base-color tint (glTF baseColorFactor; identity-white for a plain textured
                        // mesh). The cooker uses it for the skinned material instead of borrowing nuxd's "motes"
                        // particle tint (1,1,0,0 alpha-0) which zeroed the droid/shield on-device.
                        md.tint[0]=md.tint[1]=md.tint[2]=md.tint[3]=1.0f;
                        if (matIdx>=0 && root.has("materials") && (size_t)matIdx<root["materials"].size()) {
                            const auto& mm = root["materials"][matIdx];
                            gltfmaterial::TextureTintInput ti;
                            ti.usesBaseColorTexture = mm.has("pbrMetallicRoughness")
                                && mm["pbrMetallicRoughness"].has("baseColorTexture");
                            ti.usesEmissiveTexture = !ti.usesBaseColorTexture && mm.has("emissiveTexture");
                            if (mm.has("pbrMetallicRoughness") && mm["pbrMetallicRoughness"].has("baseColorFactor")) {
                                const auto& bcf = mm["pbrMetallicRoughness"]["baseColorFactor"];
                                ti.hasBaseColorFactor = true;
                                for (int i=0;i<4 && i<(int)bcf.size();++i) ti.baseColorFactor[i]=(float)bcf[i].asFloat();
                            }
                            if (mm.has("emissiveFactor")) {
                                const auto& ef = mm["emissiveFactor"];
                                ti.hasEmissiveFactor = true;
                                for (int i=0;i<3 && i<(int)ef.size();++i) ti.emissiveFactor[i]=(float)ef[i].asFloat();
                            }
                            const auto tint = gltfmaterial::selectedTextureTint(ti);
                            for (int i=0;i<4;++i) md.tint[i]=tint[i];
                        }
                        md.transform.rot[3]=1.f;  // identity (world already baked in)
                        meshes.push_back(std::move(md));
                    }
                }
            }
            if (nd.has("children")) {
                const std::string me = nd.has("name") ? nd["name"].asString() : "node " + std::to_string(nodeIdx);
                const std::string sub = path.empty() ? me : path + "/" + me;
                for (size_t c=0;c<nd["children"].size();++c) visit((int)nd["children"][c].asInt(), world, sub);
            }
        };
        // Roots: scene.nodes, else all nodes.
        int sceneIdx = root.has("scene") ? (int)root["scene"].asInt() : 0;
        if (root.has("scenes") && (size_t)sceneIdx < root["scenes"].size() && root["scenes"][sceneIdx].has("nodes")) {
            const auto& rn = root["scenes"][sceneIdx]["nodes"];
            for (size_t i=0;i<rn.size();++i) visit((int)rn[i].asInt(), identity(), std::string());
        } else {
            for (size_t i=0;i<nodes.size();++i) visit((int)i, identity(), std::string());
        }
        if (hasAnimation()) animate(0.0f);   // bind pose for the initial upload
        log("V79 loaded: %zu mesh primitives (%zu skinned, anim=%.2fs)",
            meshes.size(), skinned.size(), animDuration);
        return !meshes.empty();
    }

    // Sample a sampler at time t (LINEAR; NLERP for vec4 quaternions). Clamps to ends.
    void sampleSampler(const GSampler& s, float t, float out[4]) const {
        int n = (int)s.times.size();
        for (int c=0;c<s.comps;++c) out[c] = s.vals.empty()?0.f:s.vals[c];
        if (n==0) return;
        if (t <= s.times[0]) { for(int c=0;c<s.comps;++c) out[c]=s.vals[c]; return; }
        if (t >= s.times[n-1]) { for(int c=0;c<s.comps;++c) out[c]=s.vals[(size_t)(n-1)*s.comps+c]; return; }
        int i=0; while (i<n-1 && !(s.times[i]<=t && t<s.times[i+1])) ++i;
        const float* v0=&s.vals[(size_t)i*s.comps]; const float* v1=&s.vals[(size_t)(i+1)*s.comps];
        if (s.step) { for(int c=0;c<s.comps;++c) out[c]=v0[c]; return; }   // hold prev keyframe (flipbook)
        float t0=s.times[i], t1=s.times[i+1];
        float a = (t1>t0) ? (t-t0)/(t1-t0) : 0.f;
        // SHORTEST-ARC quaternion nlerp (the cyan windmill "glitchy and shaking" fix): exporters store some
        // neighbouring rotation keys with FLIPPED sign (q and -q are the same rotation — cyan node8 has
        // antipodal pairs at t=2.708/5.292/5.583, node34/36 too). A raw componentwise lerp between antipodal
        // keys passes near |q|=0, and the normalize turns that interval into a wild long-arc sweep + holds —
        // which the 30fps clip resample then BAKES into the cooked animation (decoded clip showed 9x-speed
        // spikes and frozen frames at exactly those key times, while the source keys are a uniform 2.02deg/key).
        // Negating v1 when dot<0 lerps the short arc, matching what every real engine's nlerp does.
        float sgn = 1.f;
        if (s.comps==4 && (v0[0]*v1[0]+v0[1]*v1[1]+v0[2]*v1[2]+v0[3]*v1[3]) < 0.f) sgn = -1.f;
        for (int c=0;c<s.comps;++c) out[c]=v0[c]+(v1[c]*sgn-v0[c])*a;
        if (s.comps==4) { // normalize quaternion
            float l=sqrtf(out[0]*out[0]+out[1]*out[1]+out[2]*out[2]+out[3]*out[3]);
            if (l>1e-8f) for(int c=0;c<4;c++) out[c]/=l;
        }
    }

    // CPU skeletal skinning: sample the clip at time t, build joint matrices, skin each
    // skinned mesh's vertices into meshes[rec.meshIdx].positions (the renderer streams
    // those into the persistently-mapped VBO each frame). Our own code — no libshell.
    // Every node's world matrix at t (seconds, already wrapped to the loop): the sampled channels over the rest TRS.
    std::vector<Mat4> nodeWorlds(float t) {
        std::vector<GNode> cur = gnodes;
        for (auto& ch : gchannels) {
            if (ch.node<0 || (size_t)ch.node>=cur.size() || ch.sampler<0 || (size_t)ch.sampler>=gsamplers.size()) continue;
            float v[4]={0,0,0,1};
            sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), v);
            if (ch.path==0) { cur[ch.node].t[0]=v[0]; cur[ch.node].t[1]=v[1]; cur[ch.node].t[2]=v[2]; }
            else if (ch.path==1) { cur[ch.node].r[0]=v[0]; cur[ch.node].r[1]=v[1]; cur[ch.node].r[2]=v[2]; cur[ch.node].r[3]=v[3]; }
            else { cur[ch.node].s[0]=v[0]; cur[ch.node].s[1]=v[1]; cur[ch.node].s[2]=v[2]; }
        }
        std::vector<Mat4> world(cur.size()); std::vector<char> done(cur.size(),0);
        // depth cap: parent chains come from the file (done[] already breaks cycles).
        std::function<Mat4(int,int)> wm = [&](int n, int depth)->Mat4 {
            if (n<0 || (size_t)n>=cur.size() || depth > 256) return identity();
            if (done[n]) return world[n];
            done[n]=1;
            Mat4 local = fromTRS(cur[n].t, cur[n].r, cur[n].s);
            world[n] = (cur[n].parent>=0) ? mul(wm(cur[n].parent, depth+1), local) : local;
            return world[n];
        };
        for (size_t n=0;n<cur.size();++n) wm((int)n, 0);
        return world;
    }

    static bool affineInverse(const Mat4& M, Mat4& out) {
        const float* a = M.m;
        const float det = a[0]*(a[5]*a[10]-a[6]*a[9]) - a[4]*(a[1]*a[10]-a[2]*a[9]) + a[8]*(a[1]*a[6]-a[2]*a[5]);
        if (std::fabs(det) < 1e-9f) { out = identity(); return false; }
        const float id = 1.0f / det;
        float* b = out.m;
        b[0]=(a[5]*a[10]-a[6]*a[9])*id; b[1]=(a[2]*a[9]-a[1]*a[10])*id; b[2]=(a[1]*a[6]-a[2]*a[5])*id; b[3]=0;
        b[4]=(a[6]*a[8]-a[4]*a[10])*id; b[5]=(a[0]*a[10]-a[2]*a[8])*id; b[6]=(a[2]*a[4]-a[0]*a[6])*id; b[7]=0;
        b[8]=(a[4]*a[9]-a[5]*a[8])*id;  b[9]=(a[1]*a[8]-a[0]*a[9])*id;  b[10]=(a[0]*a[5]-a[1]*a[4])*id; b[11]=0;
        b[12]=-(b[0]*a[12]+b[4]*a[13]+b[8]*a[14]); b[13]=-(b[1]*a[12]+b[5]*a[13]+b[9]*a[14]);
        b[14]=-(b[2]*a[12]+b[6]*a[13]+b[10]*a[14]); b[15]=1;
        return true;
    }

    void animate(float t) {
        if (!hasAnimation()) return;
        { static int rd=-1; if(rd<0){ rd=0; if(const char* r=std::getenv("HSR_REANCHOR")) sscanf(r,"%f,%f,%f",&reanchor[0],&reanchor[1],&reanchor[2]); } }
        if (animDuration > 0.0f) t = fmodf(t, animDuration);
        // PER-NODE loop period (V79 sanim semantics: every track loops on ITS OWN length — the OPA
        // sampleTrack/per-node-period rule). glTF samplers CLAMP past their last key, so a short Blender
        // action inside a longer scene timeline PARKED for the remainder of every global loop (doctorwho
        // Phonebox: 4s tumble in a 16.7s scene = 12.7s frozen, "it should be looping"). chTime() shares the
        // SAME rule with every cook extractor so the bake samples channels exactly like the preview plays them.
        const std::vector<Mat4> world = nodeWorlds(t);
        if (gpuRigid_ && !nodeAnimRecs.empty() && nodeAnimRecs[0].rigid < 0) {
            std::vector<char> cpu(meshes.size(), 0);   // meshes skinning also writes
            for (auto& rec : skinned) if (rec.meshIdx >= 0 && (size_t)rec.meshIdx < cpu.size()) cpu[rec.meshIdx] = 1;
            const std::vector<Mat4> w0 = nodeWorlds(0.0f);
            for (auto& rec : nodeAnimRecs) {
                const Mat4 r = (rec.nodeIdx>=0 && (size_t)rec.nodeIdx<w0.size()) ? w0[rec.nodeIdx] : identity();
                Mat4 inv;
                const bool ok = affineInverse(r, inv) && rec.meshIdx >= 0 && (size_t)rec.meshIdx < cpu.size() && !cpu[rec.meshIdx];
                std::memcpy(rec.rest, r.m, 64); std::memcpy(rec.restInv, inv.m, 64);
                rec.rigid = ok ? 1 : 0;
            }
        }
        for (auto& rec : skinned) {
            if (rec.skin<0 || (size_t)rec.skin>=gskins.size()) continue;
            const GSkin& sk = gskins[rec.skin];
            std::vector<Mat4> jm(sk.joints.size());
            for (size_t j=0;j<sk.joints.size();++j) {
                Mat4 ibm; for(int k=0;k<16;k++) ibm.m[k]=((size_t)j*16+k<sk.ibm.size())?sk.ibm[j*16+k]:(k%5==0?1.f:0.f);
                int jn = sk.joints[j];
                jm[j] = mul((jn>=0&&(size_t)jn<world.size())?world[jn]:identity(), ibm);
            }
            { static int jd=-1; if(jd<0) jd=std::getenv("HSR_JMDBG")?1:0;
              if (jd && rec.skin==0 && rec.nv > 1427 && !jm.empty()) {
                  fprintf(stderr,"[JMDBG] mesh%d t=%.3f jm0.c0=(%.3f,%.3f,%.3f) jm0.c1=(%.3f,%.3f,%.3f) jm0.t=(%.2f,%.2f,%.2f)\n",
                      rec.meshIdx, t, jm[0].m[0],jm[0].m[1],jm[0].m[2], jm[0].m[4],jm[0].m[5],jm[0].m[6], jm[0].m[12],jm[0].m[13],jm[0].m[14]);
                  fprintf(stderr,"[JMDBG]   vert1427 slots: j=(%d,%d,%d,%d) w=(%.3f,%.3f,%.3f,%.3f) base=(%.2f,%.2f,%.2f) out=(%.2f,%.2f,%.2f)\n",
                      rec.jidx[1427*4],rec.jidx[1427*4+1],rec.jidx[1427*4+2],rec.jidx[1427*4+3],
                      rec.jw[1427*4],rec.jw[1427*4+1],rec.jw[1427*4+2],rec.jw[1427*4+3],
                      rec.basePos[1427*3],rec.basePos[1427*3+1],rec.basePos[1427*3+2],
                      meshes[rec.meshIdx].positions.size()>1427*3+2?meshes[rec.meshIdx].positions[1427*3]:0.f,
                      meshes[rec.meshIdx].positions.size()>1427*3+2?meshes[rec.meshIdx].positions[1427*3+1]:0.f,
                      meshes[rec.meshIdx].positions.size()>1427*3+2?meshes[rec.meshIdx].positions[1427*3+2]:0.f);
              } }
            auto& out = meshes[rec.meshIdx].positions;
            bool changed = out.size() != (size_t)rec.nv*3;
            out.resize((size_t)rec.nv*3);
            for (u32 v=0; v<rec.nv; ++v) {
                float px=rec.basePos[v*3], py=rec.basePos[v*3+1], pz=rec.basePos[v*3+2];
                float ox=0,oy=0,oz=0, wsum=0;
                for (int c=0;c<4;++c) {
                    float w=rec.jw[v*4+c]; if (w<=0) continue;
                    int j=rec.jidx[v*4+c]; if ((size_t)j>=jm.size()) continue;
                    float o[3]; xformPoint(jm[j], px,py,pz, o);
                    ox+=w*o[0]; oy+=w*o[1]; oz+=w*o[2]; wsum+=w;
                }
                // Normalize by the weight sum: glTF doesn't guarantee weights sum to 1 (japan's koi
                // sum to ~1.1-1.5), and `Σ w·pos` without the divide pushes every vert to 1.5× its
                // position = stretched-ribbon "broken skeleton". libshell normalizes; pre-normalized
                // rigs (wsum≈1) are unaffected.
                if (wsum>1e-5f){ ox/=wsum; oy/=wsum; oz/=wsum; } else { ox=px; oy=py; oz=pz; }
                changed |= put(out[v*3], ox+reanchor[0]) | put(out[v*3+1], oy+reanchor[1]) | put(out[v*3+2], oz+reanchor[2]);
            }
            if (changed) ++meshes[rec.meshIdx].posGen;
            static int sdbg=-1; if(sdbg<0) sdbg=std::getenv("HSR_SKINDBG")?1:0;
            if (sdbg) {
                float lo[3]={1e9f,1e9f,1e9f},hi[3]={-1e9f,-1e9f,-1e9f};
                // bbox of only the VISIBLE verts (skinned to a bone whose scale!=0) vs all
                for (u32 v=0; v<rec.nv; ++v) for(int c=0;c<3;++c){ float x=out[v*3+c]; if(x<lo[c])lo[c]=x; if(x>hi[c])hi[c]=x; }
                log("[SKINDBG] mesh#%d nv=%u skinned bbox=[%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f] (extent %.2f,%.2f,%.2f)",
                    rec.meshIdx, rec.nv, lo[0],lo[1],lo[2], hi[0],hi[1],hi[2], hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2]);
            }
        }
        // Rigid node animation: transform each animated node's mesh by the node's world matrix.
        for (auto& rec : nodeAnimRecs) {
            Mat4 m = (rec.nodeIdx>=0 && (size_t)rec.nodeIdx<world.size()) ? world[rec.nodeIdx] : identity();
            MeshData& md = meshes[rec.meshIdx];
            const bool rigid = gpuRigid_ && rec.rigid == 1;
            Mat4 rest, restInv; std::memcpy(rest.m, rec.rest, 64); std::memcpy(restInv.m, rec.restInv, 64);
            if (rigid && rec.atRest) { std::memcpy(md.animXform, mul(m, restInv).m, sizeof md.animXform); continue; }
            const Mat4& at = rigid ? rest : m;
            auto& out = md.positions;
            size_t nv = rec.basePos.size()/3;
            bool changed = out.size() != nv*3;
            out.resize(nv*3);
            for (size_t v=0; v<nv; ++v) {
                float o[3]; xformPoint(at, rec.basePos[v*3], rec.basePos[v*3+1], rec.basePos[v*3+2], o);
                changed |= put(out[v*3], o[0]) | put(out[v*3+1], o[1]) | put(out[v*3+2], o[2]);
            }
            if (changed) ++md.posGen;
            rec.atRest = rigid;
            std::memcpy(md.animXform, (rigid ? mul(m, restInv) : identity()).m, sizeof md.animXform);
        }
    }

    // Bake a VAT (vertex animation texture) for an animated-node mesh. The static export already bakes REST-world
    // geometry, so these WORLD-space offsets (animatedPos(f) - restPos) add straight on top with an identity entity
    // transform (vatunlitbasecolor does pos += sample). Returns offsets[(f*nv + v)*3]; nvOut=vertexCount; empty if
    // meshIdx isn't an animated node mesh. animate() has side effects on meshes[].positions — restored to rest after.
    std::vector<float> bakeVAT(int meshIdx, int frames, int& nvOut) {
        nvOut = 0;
        const NodeAnimRec* rec = nullptr;
        for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { rec = &r; break; }
        if (!rec || frames < 2 || animDuration <= 0.0f) return {};
        size_t nv = rec->basePos.size() / 3; nvOut = (int)nv;
        animate(0.0f);
        std::vector<float> rest = meshes[meshIdx].positions;                 // rest world positions
        if (rest.size() < nv * 3) return {};
        std::vector<float> off((size_t)frames * nv * 3, 0.0f);
        for (int f = 0; f < frames; f++) {
            animate((float)f / (float)frames * animDuration);
            const auto& p = meshes[meshIdx].positions;
            size_t n = std::min((size_t)nv * 3, std::min(p.size(), rest.size()));
            for (size_t i = 0; i < n; i++) off[(size_t)f * nv * 3 + i] = p[i] - rest[i];
        }
        animate(0.0f);                                                       // restore rest pose for the static export
        return off;
    }

    // RATIONAL-LCM multi-period loop (the "reset is fucked up" harmonic-chain fix): distinct channel periods
    // along a chain USED to force the GLOBAL master-timeline bake — but a non-integer master/period ratio then
    // wraps each channel MID-CYCLE at the clip seam = a visible snap every master loop ON DEVICE (doctorwho
    // Phonebox: 1.1667s+2.3333s channels in a 16.67s master = 16.67/2.333 = 7.14 cycles, seam snaps 29% deep;
    // the real V79 runtime loops every track on its OWN length — no master reset, no snap). When the periods
    // share a small RATIONAL LCM (2.3333s here: 2x1.1667 = 1x2.3333), bake THAT — every channel closes exactly
    // at the clip end = seamless forever. Irrational beats (bubbles' spread trails) return 0 -> master bake.
    float rationalLcmPeriod(const std::vector<float>& periods) const {
        if (periods.size() < 2) return 0.f;
        float pmax = 0.f; for (float p : periods) if (p > pmax) pmax = p;
        if (pmax <= 1e-4f) return 0.f;
        float cap = std::max(animDuration * 2.f, 60.f);
        for (int n = 1; n <= 64; n++) {
            float T = (float)n * pmax; if (T > cap) break;
            bool ok = true;
            for (float p : periods) { if (p <= 1e-4f) continue;
                float r = T / p; float ri = floorf(r + 0.5f);
                if (ri < 1.f || std::fabs(r - ri) > 0.01f * r) { ok = false; break; } }
            if (ok) return T;
        }
        return 0.f;
    }
    // ── FLIPBOOK OBSERVED-SEQUENCE extraction (FLIP-VERIFY 2026-07-08): the collapsed spritesheet shader used
    //    to ASSUME row-major cell order at flipFrames/animDuration fps — WRONG for most real sheets (Rick&Morty
    //    TV: 23/100 frames matched; cube_maze torches: 0/64, each with its OWN phase stagger; zelda fires:
    //    27 frames spread over the 60s GLOBAL timeline = 0.45fps instead of their own loop). Sample the SOURCE
    //    animation over the mesh's OWN clip period and record WHICH cell is visible per frame — shipped as
    //    per-frame 2x3 UV matrices (flipUVMats -> editFragUvMatrixReplay, the exact frame-snap replay). ──
    bool flipbookCellSequence(int meshIdx, int cols, int rows, std::vector<float>& mats6, int& N, float& loopSec) {
        mats6.clear(); N = 0; loopSec = 0.f;
        if (meshIdx < 0 || (size_t)meshIdx >= meshes.size() || cols < 1 || rows < 1) return false;
        // relevant node set: the skin's joints + their ancestors (spritesheet flipbooks are skinned), or the
        // rigid anim node chain — same clip-period rules as extractHzAnim (stagger/multi-period -> global).
        std::vector<char> relN(gnodes.size(), 0);
        bool found = false;
        for (auto& r : skinned) if (r.meshIdx == meshIdx && r.skin >= 0 && (size_t)r.skin < gskins.size()) {
            for (int jn : gskins[r.skin].joints)
                for (int n = jn, g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g) relN[n] = 1;
            found = true; break;
        }
        if (!found) for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) {
            for (int n = r.nodeIdx, g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g) relN[n] = 1;
            found = true; break;
        }
        if (!found) return false;
        float clipDur = 0.f, latestStart = 0.f, firstEnd = -1.f; bool multiPeriod = false; float keyRate = 0.f;   // NO 30fps floor (see extractNodeRigidHzAnim)
        for (auto& ch : gchannels)
            if (ch.node >= 0 && (size_t)ch.node < relN.size() && relN[ch.node]
                && ch.sampler >= 0 && (size_t)ch.sampler < gsamplers.size()) { auto& tv = gsamplers[ch.sampler].times;
                if (!tv.empty()){ if (tv.back() > clipDur) clipDur = tv.back(); if (tv.front() > latestStart) latestStart = tv.front();
                    if (firstEnd < 0.f) firstEnd = tv.back(); else if (std::fabs(tv.back()-firstEnd) > 0.1f) multiPeriod = true;
                    if (tv.size() >= 2 && tv.back()-tv.front() > 1e-4f) { float r2 = (float)(tv.size()-1)/(tv.back()-tv.front()); if (r2 > keyRate) keyRate = r2; } } }
        if (keyRate > 240.f) keyRate = 240.f; if (keyRate < 1.f) keyRate = 30.f;
        if (clipDur <= 1e-4f || latestStart > 0.05f || multiPeriod) clipDur = animDuration;
        if (clipDur <= 1e-4f) return false;
        int n2 = (int)(clipDur * keyRate + 0.5f); if (n2 < 2) n2 = 2; if (n2 > 4096) n2 = 4096;
        const MeshData& md = meshes[meshIdx];
        size_t nq = md.positions.size()/12;
        if (nq < 2 || md.uvs.size() < nq*8) return false;
        auto tA = [](const float* a, const float* b, const float* c){
            float u[3]={b[0]-a[0],b[1]-a[1],b[2]-a[2]}, w[3]={c[0]-a[0],c[1]-a[1],c[2]-a[2]};
            float cx=u[1]*w[2]-u[2]*w[1], cy=u[2]*w[0]-u[0]*w[2], cz=u[0]*w[1]-u[1]*w[0];
            return 0.5f*sqrtf(cx*cx+cy*cy+cz*cz); };
        mats6.reserve((size_t)n2*6);
        bool varies = false; int cell0 = -1;
        for (int i = 0; i < n2; i++) {
            animate((float)(i + 0.5f) * clipDur / (float)n2);   // frame-window CENTER (the replay frame-snaps)
            int best = -1; float bestA = -1.f;
            for (size_t q = 0; q < nq; q++) {
                const float* p0v=&md.positions[q*12]; const float* p1v=&md.positions[q*12+3];
                const float* p2v=&md.positions[q*12+6]; const float* p3v=&md.positions[q*12+9];
                float a = tA(p0v,p1v,p2v)+tA(p0v,p2v,p3v);
                if (a > bestA) { bestA = a; best = (int)q; }
            }
            float u0=1e9f, v0=1e9f;
            for (int c = 0; c < 4; c++) { float u=md.uvs[((size_t)best*4+c)*2], v=md.uvs[((size_t)best*4+c)*2+1];
                if (u<u0) u0=u; if (v<v0) v0=v; }
            int col = (int)(u0*cols + 0.5f); if (col < 0) col = 0; if (col >= cols) col = cols-1;
            int row = (int)(v0*rows + 0.5f); if (row < 0) row = 0; if (row >= rows) row = rows-1;
            if (cell0 < 0) cell0 = row*cols+col; else if (row*cols+col != cell0) varies = true;
            mats6.push_back(1.f/(float)cols); mats6.push_back(0.f); mats6.push_back((float)col/(float)cols);
            mats6.push_back(0.f); mats6.push_back(1.f/(float)rows); mats6.push_back((float)row/(float)rows);
        }
        animate(0.f);   // RESTORE the t=0 bake — later meshes in buildExportMeshes bake md.positions
        if (!varies) { mats6.clear(); return false; }   // single-cell "flipbook" = static, no replay
        N = n2; loopSec = clipDur;
        return true;
    }
    // ── HZANIM extraction: a skinned glTF mesh -> skeleton (hierarchical bind TRS) + per-vertex bones + the clip
    //    (per-joint LOCAL TRS sampled at `frames`). Feeds encodeHzSkel + hzAclEncode + the skinned RENDMESH. ──
    struct HzAnimExport {
        std::vector<float> jointPos;     // jointCount*3   (bind local translation)
        std::vector<float> jointQuat;    // jointCount*4   (bind local rotation, W,X,Y,Z for HZAN:SKEL)
        std::vector<float> jointScale;   // jointCount     (bind local uniform scale)
        std::vector<int>   parents;      // jointCount     (parent joint index, -1 = root)
        std::vector<uint8_t> boneIdx, boneWgt;  // nv*4 each (sem7 indices, sem8 u8 weights)
        std::vector<float> trsLocal;     // frames*jointCount*10 (quat X,Y,Z,W + translation3 + scale3) for ACL
        std::vector<float> restPos;      // nv*3 REST mesh/model-space positions (NOT world-baked — skinning needs these)
        int jointCount = 0, frameCount = 0; float fps = 0.f;
        bool ok() const { return jointCount > 0 && frameCount > 1; }
    };
    // True if this mesh's NODE has a TRS animation (no skin) — the candidate for a non-skeletal flipbook/material anim.
    bool isNodeAnimated(int meshIdx) const {
        for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) return true;
        return false;
    }
    const char* nodeNameOfMesh(int meshIdx) const {
        for (auto& r : nodeAnimRecs)
            if (r.meshIdx == meshIdx && r.nodeIdx >= 0 && (size_t)r.nodeIdx < gnodes.size())
                return gnodes[r.nodeIdx].name.c_str();
        return "";
    }
    // True only if the node animation ACTUALLY changes the transform. Some V79 exports bake a
    // 50-key CONSTANT T/R/S track onto EVERY object = "animated" but motionless; without this gate they all get a getTime
    // wisp PULSE shader -> the whole env "breathes"/moves on device. Returns false for constant (static) tracks.
    // Walks the ANCESTOR chain like every extractor's worldAt does: a mesh whose motion comes ONLY from an animated
    // parent (doctorwho's vortex torus: node0 has no channels, its parents pTorus1/group6 carry the T+R) previously
    // failed this gate and cooked STATIC — the whole visible animation layer silently missing on device.
    bool nodeAnimMoves(int meshIdx) const {
        int nodeIdx = -1; for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; break; }
        if (nodeIdx < 0) return false;
        for (int nd = nodeIdx, g = 0; nd >= 0 && (size_t)nd < gnodes.size() && g < 64; nd = gnodes[nd].parent, ++g)
            for (auto& ch : gchannels) {
                if (ch.node != nd || ch.sampler < 0 || (size_t)ch.sampler >= gsamplers.size()) continue;
                const GSampler& s = gsamplers[ch.sampler]; int n = (int)s.times.size(); if (n < 2) continue;
                for (int c = 0; c < s.comps; ++c) { float mn=1e30f, mx=-1e30f;
                    for (int k = 0; k < n; ++k){ float v=s.vals[(size_t)k*s.comps+c]; if(v<mn)mn=v; if(v>mx)mx=v; }
                    if (mx - mn > 1e-4f) return true; }   // some component actually moves
            }
        return false;
    }
    // Dump the V79 node's animation channels for a node-anim mesh (HSR_VERBOSE) — so we can replicate the wisp SCALE
    // pulse FAITHFULLY (exact amplitude/period/keyframe shape) instead of guessing a sinusoid.
    void dumpNodeAnimTrack(int meshIdx) const {
        int nodeIdx = -1; for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; break; }
        if (nodeIdx < 0) return;
        for (auto& ch : gchannels) {
            if (ch.node != nodeIdx || ch.sampler < 0 || (size_t)ch.sampler >= gsamplers.size()) continue;
            const GSampler& s = gsamplers[ch.sampler]; int n = (int)s.times.size();
            const char* pn = ch.path == 0 ? "T" : ch.path == 1 ? "R" : "S";
            float mn[4] = {1e30f,1e30f,1e30f,1e30f}, mx[4] = {-1e30f,-1e30f,-1e30f,-1e30f};
            for (int k = 0; k < n; k++) for (int c = 0; c < s.comps; c++) { float v = s.vals[(size_t)k*s.comps+c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; }
            fprintf(stderr, "[V79ANIM] mesh%d node%d '%s' path=%s keys=%d t0=%.3f tN=%.3f step=%d comps=%d min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)\n",
                    meshIdx, nodeIdx, gnodes[nodeIdx].name.c_str(), pn, n, n?s.times[0]:0.f, n?s.times[n-1]:0.f, s.step?1:0, s.comps, mn[0],mn[1],mn[2], mx[0],mx[1],mx[2]);
            if (ch.path == 2) for (int k = 0; k < n && k < 20; k++)
                fprintf(stderr, "    [k%02d] t=%.3f s=(%.4f,%.4f,%.4f)\n", k, s.times[k],
                        s.vals[(size_t)k*s.comps], s.vals[(size_t)k*s.comps+1], s.vals[(size_t)k*s.comps+2]);
        }
    }
    // Extract the V79 node SCALE track for a node-anim mesh as pose-animation endpoints (the FAITHFUL non-skeletal wisp
    // port -> ShellPoseAnimationComponent). The cooked geometry is baked at the REST (t=0) pose, so we return the per-axis
    // scale min/max RELATIVE to the t=0 scale: the device's pose anim then drives the entity scale start<->end, reproducing
    // the V79 flicker in place. Returns false if the node has no scale channel. duration = the V79 loop length (seconds).
    bool extractNodeScaleAnim(int meshIdx, float startScale[3], float endScale[3], float& duration) const {
        int nodeIdx = -1; for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; break; }
        if (nodeIdx < 0) return false;
        const GSampler* ss = nullptr;
        for (auto& ch : gchannels)
            if (ch.node == nodeIdx && ch.path == 2 && ch.sampler >= 0 && (size_t)ch.sampler < gsamplers.size()) { ss = &gsamplers[ch.sampler]; break; }
        if (!ss || ss->comps < 3 || ss->times.empty()) return false;
        float mn[3] = {1e30f,1e30f,1e30f}, mx[3] = {-1e30f,-1e30f,-1e30f};
        for (size_t k = 0; k < ss->times.size(); ++k) for (int c = 0; c < 3; ++c) { float v = ss->vals[k*(size_t)ss->comps+c]; if(v<mn[c])mn[c]=v; if(v>mx[c])mx[c]=v; }
        const float* rest = &ss->vals[0];   // t=0 scale (clamped first key) = what's baked into the cooked geometry
        for (int c = 0; c < 3; ++c) { float r = (rest[c] != 0.f) ? rest[c] : 1.f; startScale[c] = mn[c] / r; endScale[c] = mx[c] / r; }
        // the SCALE track's OWN length (per-node loop, like the rigid/translate extractors) — global fallback;
        // STAGGER GUARD: late-start channel = a global-choreography segment, keep the global duration.
        duration = (ss->times.size()>=2 && ss->times.back() > 1e-4f && ss->times.front() <= 0.05f) ? ss->times.back() : animDuration;
        return true;
    }
    // GENERAL node TRANSLATION port: sample the node's translation channel at N uniform frames over the loop, as OFFSETS
    // from the t=0 (baked) translation. The cooker generates a shadergen::TRANSLATE shader that replays them with piecewise-
    // linear interpolation -> faithfully ports any translation animation, not a pattern guess.
    // Returns false if the node has no translation channel or it doesn't actually move.
    bool extractNodeTranslation(int meshIdx, int N, std::vector<float>& frameOffs, float& loopSec) const {
        int nodeIdx = -1; for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; break; }
        if (nodeIdx < 0 || N < 2 || nodeIdx >= (int)gnodes.size()) return false;
        // PER-NODE loop period (node + ANCESTORS — worldAt composes the whole chain, so an animated parent's
        // motion must fit inside the replay window too), NOT the global animDuration: sampling a short action
        // over the global timeline bakes the sampler-CLAMPED parked tail into the replay (doctorwho Phonebox:
        // 2.33s tumble in a 16.7s scene = 14s frozen per loop, "should be looping"). STAGGER GUARD: a channel
        // that STARTS late is a segment of the global choreography — keep the GLOBAL loop (ensemble phasing).
        loopSec = 0.f; float latestStart = 0.f, firstEnd = -1.f; bool multiPeriod = false;
        std::vector<float> chainPeriods;
        for (int n = nodeIdx, g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g)
            for (auto& ch : gchannels) if (ch.node==n && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                auto& tv=gsamplers[ch.sampler].times;
                if (!tv.empty()){ if (tv.back()>loopSec) loopSec=tv.back(); if (tv.front()>latestStart) latestStart=tv.front();
                    if (firstEnd < 0.f) firstEnd = tv.back(); else if (std::fabs(tv.back()-firstEnd) > 0.1f) multiPeriod = true;
                    bool nw = true; for (float p : chainPeriods) if (std::fabs(p-tv.back()) <= 0.05f) { nw = false; break; }
                    if (nw) chainPeriods.push_back(tv.back()); } }
        if (loopSec <= 1e-4f || latestStart > 0.05f) loopSec = animDuration;
        else if (multiPeriod) { float lcm = rationalLcmPeriod(chainPeriods); loopSec = (lcm > 1e-4f) ? lcm : animDuration; }   // harmonic chain -> seamless LCM loop (the reset-snap fix)
        if (loopSec <= 1e-4f) return false;
        // Compute the node's full composed WORLD transform at time t (sample EVERY ancestor's T/R/S channel). The cook bakes
        // the geometry in WORLD space (md.positions x gm.model), so the offset MUST be the WORLD translation delta — using
        // the LOCAL channel delta ignored the parent SCALE and made the screens slide ~10x too far ("they go beyond").
        auto trsMat=[](const float* t,const float* r,const float* sc,float* mm){ float x=r[0],y=r[1],z=r[2],ww=r[3];
            mm[0]=(1-2*(y*y+z*z))*sc[0]; mm[1]=(2*(x*y+ww*z))*sc[0]; mm[2]=(2*(x*z-ww*y))*sc[0]; mm[3]=0;
            mm[4]=(2*(x*y-ww*z))*sc[1]; mm[5]=(1-2*(x*x+z*z))*sc[1]; mm[6]=(2*(y*z+ww*x))*sc[1]; mm[7]=0;
            mm[8]=(2*(x*z+ww*y))*sc[2]; mm[9]=(2*(y*z-ww*x))*sc[2]; mm[10]=(1-2*(x*x+y*y))*sc[2]; mm[11]=0;
            mm[12]=t[0]; mm[13]=t[1]; mm[14]=t[2]; mm[15]=1; };
        auto mulMat=[](const float* a,const float* b,float* o){ for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        std::function<void(int,float,float*)> worldAt=[&](int n,float t,float* out){
            float tt[3]={gnodes[n].t[0],gnodes[n].t[1],gnodes[n].t[2]}, q[4]={gnodes[n].r[0],gnodes[n].r[1],gnodes[n].r[2],gnodes[n].r[3]}, sc[3]={gnodes[n].s[0],gnodes[n].s[1],gnodes[n].s[2]};
            for (auto& ch : gchannels) if (ch.node==n && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                float o4[4]; sampleSampler(gsamplers[ch.sampler], chTime(n, t), o4);   // per-node wrap = bake matches the preview (V79 per-track loop)
                if (ch.path==0){ tt[0]=o4[0]; tt[1]=o4[1]; tt[2]=o4[2]; }
                else if (ch.path==1){ q[0]=o4[0]; q[1]=o4[1]; q[2]=o4[2]; q[3]=o4[3]; }
                else if (ch.path==2){ sc[0]=o4[0]; sc[1]=o4[1]; sc[2]=o4[2]; } }
            float loc[16]; trsMat(tt,q,sc,loc);
            if (gnodes[n].parent>=0 && (size_t)gnodes[n].parent<gnodes.size()){ float pw[16]; worldAt(gnodes[n].parent,t,pw); mulMat(pw,loc,out); } else memcpy(out,loc,64); };
        float w0[16]; worldAt(nodeIdx, 0.f, w0);
        frameOffs.assign((size_t)N*3, 0.f); float maxd = 0.f;
        for (int fi=0; fi<N; ++fi){ float wt[16]; worldAt(nodeIdx, loopSec * (float)fi/(float)N, wt);
            for(int c=0;c<3;c++){ float o = wt[12+c]-w0[12+c]; frameOffs[(size_t)fi*3+c]=o; float ad=o<0?-o:o; if(ad>maxd)maxd=ad; } }
        return maxd > 1e-3f;   // actually moves (WORLD space)
    }
    // GENERAL node SCALE port (Erebor "breathe" wisps — NON-UNIFORM per-axis): sample the node's composed LOCAL scale at
    // N uniform frames over the clip, as per-axis FACTORS relative to the t=0 (baked) scale (frame 0 = (1,1,1)). The cooker
    // generates a shadergen::SCALE shader that replays them with piecewise-linear interpolation -> FAITHFULLY reproduces the
    // per-axis amplitudes (which the single hand-rolled wispscale.surface could not). loopSec = the SCALE node's OWN track
    // length (per-clip-duration, like the 70274aa warp fix), NOT the global animDuration. pivot = the node's WORLD origin
    // (so the scale pivots in place). Scale is a LOCAL channel — parent scale is constant relative to the factor, so the
    // factor = scale(t)/scale(0) needs only the node's own scale track (unlike translation, which needs the world delta).
    // Returns false if the node has no scale channel or it doesn't actually breathe.
    bool extractNodeScaleFrames(int meshIdx, int N, std::vector<float>& frameFactors, float& loopSec, float pivot[3]) const {
        int nodeIdx = -1; for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; break; }
        if (nodeIdx < 0 || N < 2 || nodeIdx >= (int)gnodes.size()) return false;
        const GSampler* ss = nullptr;   // the node's OWN scale channel
        for (auto& ch : gchannels)
            if (ch.node==nodeIdx && ch.path==2 && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()) { ss=&gsamplers[ch.sampler]; break; }
        if (!ss || ss->comps < 3 || ss->times.empty()) return false;
        // loopSec = the SCALE track's OWN length (per-clip), falling back to the global animDuration.
        loopSec = (ss->times.size()>=2) ? (ss->times.back()-ss->times.front()) : 0.f;
        if (loopSec <= 1e-4f) loopSec = animDuration;
        if (loopSec <= 1e-4f) return false;
        // t=0 baked scale = the channel value at the clip's first time (clamped). factor(t) = scale(t)/scale(0).
        float base[3]; { float o4[4]; sampleSampler(*ss, ss->times.front(), o4); for (int c=0;c<3;c++) base[c] = (o4[c]!=0.f)?o4[c]:1.f; }
        frameFactors.assign((size_t)N*3, 1.f); float maxDev = 0.f;
        for (int fi=0; fi<N; ++fi){ float o4[4]; sampleSampler(*ss, ss->times.front() + loopSec*(float)fi/(float)N, o4);
            for (int c=0;c<3;c++){ float f = o4[c]/base[c]; frameFactors[(size_t)fi*3+c]=f; float d=f>1.f?f-1.f:1.f-f; if(d>maxDev)maxDev=d; } }
        // pivot = the node's WORLD origin at t=0 (the cook recenters geometry here so the scale pivots in place).
        pivot[0]=pivot[1]=pivot[2]=0.f;
        { float tt[3]={gnodes[nodeIdx].t[0],gnodes[nodeIdx].t[1],gnodes[nodeIdx].t[2]};
          // walk parents at t=0 to compose the world origin (matches extractNodeTranslation's worldAt(node,0))
          auto trsMat=[](const float* t,const float* r,const float* sc,float* mm){ float x=r[0],y=r[1],z=r[2],ww=r[3];
              mm[0]=(1-2*(y*y+z*z))*sc[0]; mm[1]=(2*(x*y+ww*z))*sc[0]; mm[2]=(2*(x*z-ww*y))*sc[0]; mm[3]=0;
              mm[4]=(2*(x*y-ww*z))*sc[1]; mm[5]=(1-2*(x*x+z*z))*sc[1]; mm[6]=(2*(y*z+ww*x))*sc[1]; mm[7]=0;
              mm[8]=(2*(x*z+ww*y))*sc[2]; mm[9]=(2*(y*z-ww*x))*sc[2]; mm[10]=(1-2*(x*x+y*y))*sc[2]; mm[11]=0;
              mm[12]=t[0]; mm[13]=t[1]; mm[14]=t[2]; mm[15]=1; };
          auto mulMat=[](const float* a,const float* b,float* o){ for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
          std::function<void(int,float*)> worldAt0=[&](int n,float* out){
              float ttn[3]={gnodes[n].t[0],gnodes[n].t[1],gnodes[n].t[2]}, q[4]={gnodes[n].r[0],gnodes[n].r[1],gnodes[n].r[2],gnodes[n].r[3]}, sc[3]={gnodes[n].s[0],gnodes[n].s[1],gnodes[n].s[2]};
              for (auto& ch : gchannels) if (ch.node==n && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                  float o4[4]; sampleSampler(gsamplers[ch.sampler], 0.f, o4);
                  if (ch.path==0){ ttn[0]=o4[0]; ttn[1]=o4[1]; ttn[2]=o4[2]; }
                  else if (ch.path==1){ q[0]=o4[0]; q[1]=o4[1]; q[2]=o4[2]; q[3]=o4[3]; }
                  else if (ch.path==2){ sc[0]=o4[0]; sc[1]=o4[1]; sc[2]=o4[2]; } }
              float loc[16]; trsMat(ttn,q,sc,loc);
              if (gnodes[n].parent>=0 && (size_t)gnodes[n].parent<gnodes.size()){ float pw[16]; worldAt0(gnodes[n].parent,pw); mulMat(pw,loc,out); } else memcpy(out,loc,64); };
          (void)tt; float w0[16]; worldAt0(nodeIdx, w0); pivot[0]=w0[12]; pivot[1]=w0[13]; pivot[2]=w0[14]; }
        return maxDev > 1e-3f;   // actually breathes
    }
    // ── COOK: NON-skinned node-animated glTF mesh → 2-joint RIGID HZANIM (STATIC root + MOVING child carrying the
    //    node's EXACT per-frame composed WORLD matrix) — the OPA extractNodeRigidHzAnim port for glTF envs (cyan/
    //    dome/rifthome). The old extractor chain fitted a node anim to rotation-ONLY (SWAY/SPIN Rodrigues) or
    //    translation-ONLY (16-frame replay), DROPPING the other channel: cyan's gears/mills/boat/rocket/shadow
    //    cards all animate T+R TOGETHER (verified [V79ANIM]: gearAnim_Top T range ~280u + 56° R; millAnim_02
    //    T ±500u + 45° yaw), so the cooked motion was rotation-about-a-fitted-axis only = "wrong animation on
    //    windmill, shadows, gears". This replays the FULL T+R+S faithfully on the device-proven skinned path:
    //    2 JOINTS so V205's root-motion extraction can't eat the translation (root stays identity, the child
    //    moves — the OPA train/cars fix), BIND = W0 on the mover so the device computes invBind = inv(W0) and the
    //    ACL clip stores W(f) directly = small ACL-friendly numbers (the OPA comet ACL-clean fix):
    //      driven:  vert = W(f)·inv(W0) · (W0·basePos) = W(f)·basePos       (exact node animation)
    //      frozen:  vert = W0·inv(W0)   · (W0·basePos) = W0·basePos          (correct world rest when un-driven)
    //    requireRotation (default): return !ok() unless the node BOTH translates AND rotates — pure translations
    //    keep the lighter getTime TRANSLATE replay, pure spins the getTime Rodrigues shader (both device-proven;
    //    no regression on sliding screens, orbiting bodies, or wisps). HSR_NOGLTFRIGID disables the whole path.
    HzAnimExport extractNodeRigidHzAnim(int meshIdx, bool requireRotation = true) {
        HzAnimExport e;
        const NodeAnimRec* rec = nullptr;
        for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { rec = &r; break; }
        if (!rec || rec->basePos.size() < 9) return e;
        int node = rec->nodeIdx; if (node < 0 || (size_t)node >= gnodes.size()) return e;
        // PER-NODE clip duration = the latest key time among channels animating this node or any ancestor (the
        // node's own loop, matching the OPA per-node-period fix — sampling a short-period node over the global
        // clip aliases its motion). Falls back to the global animDuration when no channel is found.
        float clipDur = 0.f, latestStart = 0.f, firstEnd = -1.f; bool multiPeriod = false; float srcKeyRate = 0.f;   // NO 30fps floor: grid-aligned bake at the source's OWN rate is exact at any rate (30-floor resampled 25/s rigs OFF-grid = corner-cut turns)
        std::vector<float> chainPeriods;
        for (int n = node, g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g)
            for (auto& ch : gchannels)
                if (ch.node == n && ch.sampler >= 0 && (size_t)ch.sampler < gsamplers.size()) {
                    auto& tv = gsamplers[ch.sampler].times;
                    if (!tv.empty()){ if (tv.back() > clipDur) clipDur = tv.back(); if (tv.front() > latestStart) latestStart = tv.front();
                        if (firstEnd < 0.f) firstEnd = tv.back(); else if (std::fabs(tv.back()-firstEnd) > 0.1f) multiPeriod = true;
                        bool nw = true; for (float p : chainPeriods) if (std::fabs(p-tv.back()) <= 0.05f) { nw = false; break; }
                        if (nw) chainPeriods.push_back(tv.back());
                        if (tv.size() >= 2 && tv.back()-tv.front() > 1e-4f) {   // native key rate (including 60/s sawtooths; see extractHzAnim) + MIN-gap for non-uniform tracks
                            float r = (float)(tv.size()-1)/(tv.back()-tv.front()); if (r > srcKeyRate) srcKeyRate = r;
                            float mg = 1e9f; for (size_t kk = 1; kk < tv.size(); kk++){ float g = tv[kk]-tv[kk-1]; if (g > 1e-5f && g < mg) mg = g; }
                            if (mg < 1e8f) { float r2 = 1.f/mg; if (r2 > 120.f) r2 = 120.f; if (r2 > srcKeyRate) srcKeyRate = r2; } } }
                }
        if (srcKeyRate > 240.f) srcKeyRate = 240.f; if (srcKeyRate < 1.f) srcKeyRate = 30.f;
        // STAGGER GUARD (late-start = global-choreography segment -> master timeline). MULTI-PERIOD chain:
        // try the RATIONAL LCM first (phonebox 1.1667+2.3333 -> 2.3333s, seamless forever — the "reset is
        // fucked up" fix); only an irrational beat falls back to the master-timeline bake (one reshuffle/loop).
        if (clipDur <= 1e-4f || latestStart > 0.05f) clipDur = animDuration;
        else if (multiPeriod) {
            float lcm = rationalLcmPeriod(chainPeriods);
            // IRRATIONAL BEAT (no small LCM): do NOT fall back to the global animDuration. For a CHILD node that
            // rides a parent's motion (steam_void mesh0.001/.002: a 4.1s own-spin UNDER mesh0's 29s orbit), an
            // animDuration clip (60s = some OTHER mesh's length) loops OUT OF PHASE with the 29s parent -> the child
            // DRIFTS off the orbit every parent-loop ("small anim out of place"). Keep the LONGEST chain period
            // (already in clipDur) so the DOMINANT macro-motion — the parent orbit — loops CLEANLY and the child
            // stays locked to its parent/siblings; only the shorter sub-spin takes a small seam snap. HSR_MPMASTER=animDur.
            if (lcm > 1e-4f) clipDur = lcm;
            else if (std::getenv("HSR_MPMASTER")) clipDur = animDuration;   // opt back into the old master-timeline bake
            // else: keep clipDur = the longest chain period (the parent/dominant loop)
        }
        if (clipDur <= 1e-4f) return e;
        // NATIVE key-rate sampling (min 30fps), floor 64 (short clips), NO cap — FULL PORT (the old 256 "device
        // clip size limit" was a misdiagnosis — official whale clips are 240 KB and the device loads them fine).
        // GRID-ALIGNED when dense: fps = the SOURCE key rate exactly (not NF/clipDur), so bake frame f lands ON
        // source key f — the bake's frame-lerp IS the source's key-lerp (for streak teleports, an off-grid
        // 61.2fps resample of the 60/s keys drifted through the teleport window = mid-lerp ghost frames).
        int natNF = (int)(clipDur * srcKeyRate + 0.5f);
        int NF; float bakeFps;
        if (natNF >= 64) { NF = natNF; bakeFps = srcKeyRate; }
        else             { NF = 64;    bakeFps = 64.f / clipDur; }
        // Full composed WORLD matrix of the node at time t (every ancestor's channels sampled) — same math as
        // extractNodeTranslation's worldAt (world-space so parent scale/rotation are honored).
        auto trsMat = [](const float* t, const float* r, const float* sc, float* mm){ float x=r[0],y=r[1],z=r[2],ww=r[3];
            mm[0]=(1-2*(y*y+z*z))*sc[0]; mm[1]=(2*(x*y+ww*z))*sc[0]; mm[2]=(2*(x*z-ww*y))*sc[0]; mm[3]=0;
            mm[4]=(2*(x*y-ww*z))*sc[1]; mm[5]=(1-2*(x*x+z*z))*sc[1]; mm[6]=(2*(y*z+ww*x))*sc[1]; mm[7]=0;
            mm[8]=(2*(x*z+ww*y))*sc[2]; mm[9]=(2*(y*z-ww*x))*sc[2]; mm[10]=(1-2*(x*x+y*y))*sc[2]; mm[11]=0;
            mm[12]=t[0]; mm[13]=t[1]; mm[14]=t[2]; mm[15]=1; };
        auto mulMat = [](const float* a, const float* b, float* o){
            for (int c=0;c<4;c++) for (int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        std::function<void(int,float,float*)> worldAt = [&](int n, float t, float* out){
            float tt[3]={gnodes[n].t[0],gnodes[n].t[1],gnodes[n].t[2]}, q[4]={gnodes[n].r[0],gnodes[n].r[1],gnodes[n].r[2],gnodes[n].r[3]}, sc[3]={gnodes[n].s[0],gnodes[n].s[1],gnodes[n].s[2]};
            for (auto& ch : gchannels) if (ch.node==n && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                float o4[4]; sampleSampler(gsamplers[ch.sampler], chTime(n, t), o4);   // per-node wrap = bake matches the preview (V79 per-track loop)
                if (ch.path==0){ tt[0]=o4[0]; tt[1]=o4[1]; tt[2]=o4[2]; }
                else if (ch.path==1){ q[0]=o4[0]; q[1]=o4[1]; q[2]=o4[2]; q[3]=o4[3]; }
                else if (ch.path==2){ sc[0]=o4[0]; sc[1]=o4[1]; sc[2]=o4[2]; } }
            float loc[16]; trsMat(tt,q,sc,loc);
            if (gnodes[n].parent>=0 && (size_t)gnodes[n].parent<gnodes.size()){ float pw[16]; worldAt(gnodes[n].parent,t,pw); mulMat(pw,loc,out); }
            else memcpy(out,loc,64); };
        auto matTrs = [](const float* m, float* q /*xyzw*/, float* t, float* s){
            t[0]=m[12]; t[1]=m[13]; t[2]=m[14];
            s[0]=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=s[0]>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if (tr>0){ float S=sqrtf(tr+1)*2; q[3]=0.25f*S; q[0]=(r5-r7)/S; q[1]=(r6-r2)/S; q[2]=(r1-r3)/S; }
            else if (r0>r4&&r0>r8){ float S=sqrtf(1+r0-r4-r8)*2; q[3]=(r5-r7)/S; q[0]=0.25f*S; q[1]=(r3+r1)/S; q[2]=(r6+r2)/S; }
            else if (r4>r8){ float S=sqrtf(1+r4-r0-r8)*2; q[3]=(r6-r2)/S; q[0]=(r3+r1)/S; q[1]=0.25f*S; q[2]=(r7+r5)/S; }
            else { float S=sqrtf(1+r8-r0-r4)*2; q[3]=(r1-r3)/S; q[0]=(r6+r2)/S; q[1]=(r7+r5)/S; q[2]=0.25f*S; } };
        // EXCLUSIVE endpoint [0, clipDur): the cooker's loopWrap already appends the seam frame (frame0 copy)
        // for cyclic clips — adding an inclusive endpoint HERE doubled it (two identical end frames = a 33ms
        // hold every loop, visible on the fast windmill). Open paths keep the intentional teleport wrap.
        // CENTROID travel over the clip = the ORBIT detector. A mesh whose node ORIGIN barely moves (maxd~0)
        // can still have its GEOMETRY sweep a huge arc if its centroid is OFFSET from the rotation pivot
        // (Interloper: node origin at (0,0,0) but its blob centroid orbits the skybox pivot 340u). maxd can't
        // see that, so the old gate mislabeled the orbiting comet a "big in-place spin" and dumped it to the
        // getTime ROTREPLAY shader (renders the compound orbit+own-tumble WRONG on device — "2 directions").
        // Use the CENTROID (not a far vertex): a dome SPINNING IN PLACE has far verts that sweep but a centroid
        // that stays put (stays on ROTREPLAY, avoiding animator LOD judder); an ORBITER's centroid travels
        // (routes to the device-proven RIGID-HZANIM comet path). W is affine so centroid(f)=W(f)·meanLocal.
        float meanLocal[3]={0,0,0}; { size_t nvv=rec->basePos.size()/3;
            if (nvv){ double s0=0,s1=0,s2=0; for (size_t v=0;v<nvv;v++){ s0+=rec->basePos[v*3]; s1+=rec->basePos[v*3+1]; s2+=rec->basePos[v*3+2]; }
                      meanLocal[0]=(float)(s0/nvv); meanLocal[1]=(float)(s1/nvv); meanLocal[2]=(float)(s2/nvv); } }
        // DENSIFY FAST SPINNERS: the device NLERPs the ACL joint clip between bake frames. If the WORLD rotation
        // between consecutive frames is large, NLERP diverges from the true SLERP arc -> verts far from the spin
        // axis err by (angle-gap × radius) (synthwave Cylinder.008 spins fast at 30fps -> ~51° gap -> 36u rim error,
        // ANIM-VERIFY maxErr 36). Sample the composed rotation finely, SUM the per-step angle = the TOTAL angular
        // travel, and raise NF so each bake step rotates < ~7° (NLERP≈SLERP there). Slow spins (small travel) are
        // UNCHANGED, so the grid-aligned common case is untouched; a fast spinner's smooth translation resamples off
        // -grid harmlessly (the Star-Trek sawtooth-translation teleport is not a fast spinner, so it never triggers).
        // MIRROR GUARD for the two spin heuristics below: a node with a NEGATIVE-determinant world matrix (outerwilds
        // planets carry a uniform NEGATIVE scale -51) is a rotoinversion — matTrs extracts a POSITIVE scale + a GARBAGE
        // quaternion, so per-frame "rotation" deltas are pure noise. That wrecked both passes (densify blew NF to 146k
        // on a bogus ~1M° travel; unwrap counted 144k phantom "resets"). Skip both for mirrored nodes — their RELATIVE
        // -clip variant already carries the flip. det(W_3x3) sign = the flip test (scale magnitude is irrelevant).
        // SCALE-INDEPENDENT flip test: NORMALIZE each column (strip scale) so det = ±1 = proper/mirror regardless of
        // scale magnitude, and read it at the first NON-degenerate frame (japan's fast fans have a degenerate frame-0
        // scale from the flame-fix path — a raw det(W0)≈0 there would wrongly exclude a genuinely proper spinner).
        bool spinProper = true;
        for (int s=0; s<4; s++){ float W[16]; worldAt(node, clipDur*(float)s/4.f, W);
            float c0=sqrtf(W[0]*W[0]+W[1]*W[1]+W[2]*W[2]), c1=sqrtf(W[4]*W[4]+W[5]*W[5]+W[6]*W[6]), c2=sqrtf(W[8]*W[8]+W[9]*W[9]+W[10]*W[10]);
            if (c0<1e-5f||c1<1e-5f||c2<1e-5f) continue;   // degenerate frame — sign unreliable, try the next
            float a0=W[0]/c0,a1=W[1]/c0,a2=W[2]/c0, b0=W[4]/c1,b1=W[5]/c1,b2=W[6]/c1, e0=W[8]/c2,e1=W[9]/c2,e2=W[10]/c2;
            float dN = a0*(b1*e2-b2*e1) + a1*(b2*e0-b0*e2) + a2*(b0*e1-b1*e0);
            spinProper = dN > 0.f; break; }
        if (!std::getenv("HSR_NOSPINDENSIFY") && spinProper) {
            int SN = NF > 200 ? NF : 200; float pq[4]={0,0,0,1}; double angTravel=0; bool have=false;
            for (int s = 0; s <= SN; s++) { float W[16]; worldAt(node, clipDur*(float)s/(float)SN, W);
                float q[4],tt[3],ss[3]; matTrs(W,q,tt,ss);
                if (have){ double d=pq[0]*q[0]+pq[1]*q[1]+pq[2]*q[2]+pq[3]*q[3]; if(d<0)d=-d; if(d>1)d=1; angTravel += 2.0*acos(d); }
                pq[0]=q[0];pq[1]=q[1];pq[2]=q[2];pq[3]=q[3]; have=true; }
            int needNF = (int)(angTravel / (7.0*3.14159265358979/180.0) + 0.5);
            if (needNF > 8192) needNF = 8192;   // safety cap: no real spinner needs >160 spins of frames
            if (needNF > NF) {
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[GLTF-RIGID] mesh%d node%d SPIN-DENSIFY: %.0f° travel -> NF %d->%d\n", meshIdx, node, angTravel*57.2957795, NF, needNF);
                NF = needNF; bakeFps = (float)NF / clipDur;
            }
        }
        float cen0[3]={0,0,0}, vertTravel=0.f;   // vertTravel = centroid travel (orbit magnitude)
        std::vector<float> mats((size_t)NF*16);
        float o0[3]={0,0,0}, maxd=0.f, rotMaxd=0.f, sclMaxd=0.f, rs0[9]={0}, sc0[3]={1,1,1};
        for (int f = 0; f < NF; f++) {
            float* w = &mats[(size_t)f*16];
            worldAt(node, (float)f/bakeFps, w);   // grid-aligned sample times (frame f = source key f when dense)
            { float wp[3]={ w[0]*meanLocal[0]+w[4]*meanLocal[1]+w[8]*meanLocal[2]+w[12],
                            w[1]*meanLocal[0]+w[5]*meanLocal[1]+w[9]*meanLocal[2]+w[13],
                            w[2]*meanLocal[0]+w[6]*meanLocal[1]+w[10]*meanLocal[2]+w[14] };
              if (f==0){ cen0[0]=wp[0]; cen0[1]=wp[1]; cen0[2]=wp[2]; }
              else { float dx=wp[0]-cen0[0],dy=wp[1]-cen0[1],dz=wp[2]-cen0[2]; float d=sqrtf(dx*dx+dy*dy+dz*dz); if(d>vertTravel)vertTravel=d; } }
            // NORMALIZED 3x3 (scale stripped per column) — cyan's nodes carry a ~0.01 world scale, so the raw
            // rotation*scale Frobenius read 100x too small (63° looked like 0.0148) and every gear/mill/boat
            // failed the rotation gate. Column-normalizing measures the PURE rotation deviation; the stripped
            // column norms give the SCALE deviation (dome's amber embers: 0.001→1.0 pop-in/out).
            float rs[9]={w[0],w[1],w[2], w[4],w[5],w[6], w[8],w[9],w[10]}, cs[3];
            for (int c=0;c<3;c++){ float l=sqrtf(rs[c*3]*rs[c*3]+rs[c*3+1]*rs[c*3+1]+rs[c*3+2]*rs[c*3+2]); cs[c]=l;
                                   if (l>1e-8f){ rs[c*3]/=l; rs[c*3+1]/=l; rs[c*3+2]/=l; } }
            if (f==0){ o0[0]=w[12];o0[1]=w[13];o0[2]=w[14]; for(int k=0;k<9;k++) rs0[k]=rs[k]; for(int c=0;c<3;c++) sc0[c]=cs[c]>1e-8f?cs[c]:1.f; }
            else { float dx=w[12]-o0[0],dy=w[13]-o0[1],dz=w[14]-o0[2]; float d=sqrtf(dx*dx+dy*dy+dz*dz); if(d>maxd)maxd=d;
                   float rd=0.f; for(int k=0;k<9;k++){ float e2=rs[k]-rs0[k]; rd+=e2*e2; } rd=sqrtf(rd); if(rd>rotMaxd)rotMaxd=rd;
                   for(int c=0;c<3;c++){ float sd=cs[c]/sc0[c]-1.f; if(sd<0)sd=-sd; if(sd>sclMaxd)sclMaxd=sd; } }
        }
        // COMBINED T+(R or S), plus SMALL pure-rotation meshes (the gear-shadow CLOCK-DESYNC fix): a pure-R
        // mesh whose siblings are RIGID (animator clock) must NOT ride the getTime ROTREPLAY shader clock —
        // the two clocks share no phase, so cyan's gear SHADOW drifted against its gear. Small meshes (world
        // span < 50m) join the rigid/animator clock family; only BIG pure-rotation domes (OW skybox class)
        // keep the shader replay, where the animator's LOD judder is the greater evil.
        // ORBIT = the centroid sweeps through space even though the node origin doesn't (vertTravel = centroid
        // travel, large). Such a mesh MUST take the rigid clip (exact per-frame world matrix) — the getTime
        // ROTREPLAY shader (single fixed pivot + a lone quat sequence) renders a compound orbit+own-tumble wrong
        // on device. A dome spinning in place has centroid travel ~0 and stays on ROTREPLAY.
        bool orbits = vertTravel > 2.0f;
        // pureRotOk = a genuinely BIG IN-PLACE spinner (skybox dome): keep it on the ROTREPLAY shader to avoid
        // the animator's LOD judder. Excludes orbiters (they go rigid) — that's the Interloper fix.
        bool pureRotOk = false;
        if (maxd < 0.01f && !orbits && (rotMaxd >= 0.02f || sclMaxd >= 0.05f)) {
            float lever = 0.f;
            for (size_t v = 0; v + 2 < rec->basePos.size(); v += 3) {
                float lx=rec->basePos[v], ly=rec->basePos[v+1], lz=rec->basePos[v+2];
                float l = sqrtf(lx*lx+ly*ly+lz*lz); if (l > lever) lever = l; }
            float ws = sqrtf(mats[0]*mats[0]+mats[1]*mats[1]+mats[2]*mats[2]);   // W0 column-0 scale ~ node world scale
            pureRotOk = lever * (ws > 1e-8f ? ws : 1.f) < 50.f;
        }
        if ((maxd < 0.01f && !pureRotOk && !orbits) || (requireRotation && rotMaxd < 0.02f && sclMaxd < 0.05f)) {
            if (std::getenv("HSR_VERBOSE"))
                fprintf(stderr, "[GLTF-RIGID] mesh%d node%d REJECT travel=%.4f vertTravel=%.4f rotDev=%.4f sclDev=%.4f dur=%.2fs (needs T>=0.01 %s)\n",
                        meshIdx, node, maxd, vertTravel, rotMaxd, sclMaxd, clipDur, requireRotation ? "AND (R>=0.02 or S>=0.05)" : "");
            return e;
        }
        // ── CONTINUOUS SPIN UNWRAP (synthwave/steam_void far spinners, maxErr 36). A node whose OWN rotation loops
        //    a sub-360° MONOTONIC segment (Cylinder.008: 120° over 1.667s, repeated ~20× inside the 33s clip) bakes
        //    as a SAWTOOTH. The device NLERPs every internal reset (120°->0°) as a fast BACKWARD flick, while the
        //    V79 source resets INSTANTLY — invisible on the 3-fold-symmetric mesh the 120°=360/3 loop was authored
        //    for, so the source reads as a smooth 72°/s spin and our flick is the visible bug. A single ACL clip
        //    can only teleport at ITS OWN loop, never at 20 internal ones, so the only faithful device motion is a
        //    CONTINUOUS forward spin: detect the recurring big backward jumps and re-accumulate the rotation as one
        //    monotonic spin (identical on the symmetric mesh, 20 flicks -> at most 1 at the clip seam). HSR_NOSPINUNWRAP off.
        //    (ANIM-VERIFY scores this "worse" — it demands the device reproduce the per-vertex sawtooth, which no
        //    interpolating GPU animator can; the smooth spin is the correct visual, the metric is the artifact here.)
        if (!std::getenv("HSR_NOSPINUNWRAP") && NF >= 8 && spinProper) {
            std::vector<float> qs((size_t)NF*4), ts((size_t)NF*3), ss((size_t)NF*3);
            for (int f=0; f<NF; f++) matTrs(&mats[(size_t)f*16], &qs[(size_t)f*4], &ts[(size_t)f*3], &ss[(size_t)f*3]);
            std::vector<float> ang(NF, 0.f); int resets=0; double sumNorm=0; int nNorm=0;
            const float RESET = 0.5236f;   // 30°: a one-frame jump this big is a wrap, not real motion
            for (int f=1; f<NF; f++) {
                float* q=&qs[(size_t)f*4]; float* p=&qs[(size_t)(f-1)*4];
                float dt=p[0]*q[0]+p[1]*q[1]+p[2]*q[2]+p[3]*q[3];
                if (dt<0){ q[0]=-q[0];q[1]=-q[1];q[2]=-q[2];q[3]=-q[3]; dt=-dt; }   // hemisphere-continuous
                if (dt>1)dt=1; float a=2.f*acosf(dt); ang[f]=a;
                if (a>RESET) resets++; else { sumNorm+=a; nNorm++; }
            }
            // Only a RECURRING sawtooth (>=2 resets) riding a real underlying spin qualifies; a lone sharp turn or a
            // static node never triggers, so legit rigid clips are untouched.
            if (resets>=2 && nNorm>0 && sumNorm/nNorm>1e-4) {
                auto qmul=[](const float*a,const float*b,float*o){
                    float x=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
                    float y=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
                    float z=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
                    float w=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]; o[0]=x;o[1]=y;o[2]=z;o[3]=w; };
                std::vector<float> Q((size_t)NF*4); for(int k=0;k<4;k++) Q[k]=qs[k];
                float prevD[4]={0,0,0,1}; bool haveD=false;
                for (int f=1; f<NF; f++) {
                    float cp[4]={-qs[(size_t)(f-1)*4],-qs[(size_t)(f-1)*4+1],-qs[(size_t)(f-1)*4+2],qs[(size_t)(f-1)*4+3]};
                    float d[4]; qmul(&qs[(size_t)f*4], cp, d);           // world incremental rotation q[f]*conj(q[f-1])
                    if (d[3]<0){ d[0]=-d[0];d[1]=-d[1];d[2]=-d[2];d[3]=-d[3]; }
                    if (ang[f]>RESET && haveD) { d[0]=prevD[0];d[1]=prevD[1];d[2]=prevD[2];d[3]=prevD[3]; }  // continue, don't reset
                    else { prevD[0]=d[0];prevD[1]=d[1];prevD[2]=d[2];prevD[3]=d[3]; haveD=true; }
                    float nq[4]; qmul(d, &Q[(size_t)(f-1)*4], nq);
                    float l=sqrtf(nq[0]*nq[0]+nq[1]*nq[1]+nq[2]*nq[2]+nq[3]*nq[3]); if(l>1e-8f)for(int k=0;k<4;k++)nq[k]/=l;
                    for(int k=0;k<4;k++) Q[(size_t)f*4+k]=nq[k];
                }
                for (int f=0; f<NF; f++) trsMat(&ts[(size_t)f*3], &Q[(size_t)f*4], &ss[(size_t)f*3], &mats[(size_t)f*16]);
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[GLTF-RIGID] mesh%d node%d SPIN-UNWRAP: %d internal resets -> continuous spin\n", meshIdx, node, resets);
            }
        }
        const float* W0 = &mats[0];
        // ── IDENTITY-BIND + RELATIVE clip M(f) = W(f)·inv(W0) (the mirror / non-uniform-scale fix, ANIM-VERIFY
        //    2026-07-08): the old scheme (bind = matTrs(W0) with ONE uniform scale, clip = matTrs(W(f))) LOSES
        //    W0's mirror (outerwilds planets: uniform NEGATIVE scale -51 — TRS can't represent a rotoinversion,
        //    err up to 800u) and its non-uniform scale (sqgraveyard zeds S=(1.07,1.07,0.83): the HZAN:SKEL bind
        //    carries one uniform scale -> ~0.37u distortion). M(f) is the node's RELATIVE motion — mirror and
        //    static scale cancel in W(f)·inv(W0) exactly, so it IS clean TRS for any rigid node animation —
        //    and restPos keeps the FULL W0 bake (mirror included):
        //      device: vert = C(f)·invBind·rest = M(f)·I·(W0·base) = W(f)·base   (exact, any W0)
        //    M(0)=I so the clip starts at identity (ACL-friendly small numbers; deltas, not world coords).
        auto affInv44 = [](const float* m, float* o){   // affine 4x4 inverse (column-major)
            float a=m[0],b=m[4],c=m[8], d=m[1],e2=m[5],f2=m[9], g2=m[2],h=m[6],i2=m[10];
            float det=a*(e2*i2-f2*h)-b*(d*i2-f2*g2)+c*(d*h-e2*g2); float id=(std::fabs(det)>1e-12f)?1.f/det:0.f;
            o[0]=(e2*i2-f2*h)*id; o[4]=(c*h-b*i2)*id; o[8]=(b*f2-c*e2)*id;
            o[1]=(f2*g2-d*i2)*id; o[5]=(a*i2-c*g2)*id; o[9]=(c*d-a*f2)*id;
            o[2]=(d*h-e2*g2)*id;  o[6]=(b*g2-a*h)*id;  o[10]=(a*e2-b*d)*id;
            o[3]=o[7]=o[11]=0.f; o[15]=1.f;
            o[12]=-(o[0]*m[12]+o[4]*m[13]+o[8]*m[14]);
            o[13]=-(o[1]*m[12]+o[5]*m[13]+o[9]*m[14]);
            o[14]=-(o[2]*m[12]+o[6]*m[13]+o[10]*m[14]); };
        float invW0[16]; affInv44(W0, invW0);
        auto mul44 = [](const float* a, const float* b, float* o){
            for (int c=0;c<4;c++) for (int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        // ── SCHEME SELECTION: is W0 representable as a uniform-positive-scale TRS bind? If YES (the common
        //    case), keep the DEVICE-PROVEN W0-bind + W(f) clip: matTrs(W(f)) decomposes T·R·S(f) cleanly, so
        //    per-frame ANIMATED axis-aligned scale ports exactly (erebor's breathing wisps / tinyplanet fires —
        //    the identity-bind scheme conjugates their scale through the rotation = SHEAR the TRS clip can't
        //    carry, ANIM-VERIFY regressed them 0.14-0.46u). If NO (mirror or non-uniform static scale), use the
        //    identity-bind path above. Neither scheme is universal — the selector picks per node.
        float w0q[4], w0t[3], w0s[3]; matTrs(W0, w0q, w0t, w0s);
        float w0su = (w0s[0]+w0s[1]+w0s[2])/3.f;
        float w0det = W0[0]*(W0[5]*W0[10]-W0[6]*W0[9]) - W0[4]*(W0[1]*W0[10]-W0[2]*W0[9]) + W0[8]*(W0[1]*W0[6]-W0[2]*W0[5]);
        bool bindClean = w0det > 0.f;
        if (bindClean) for (int c = 0; c < 3; c++)
            if (std::fabs(w0s[c]-w0su) > 1e-3f*std::max(std::fabs(w0su), 1e-3f)) { bindClean = false; break; }
        if (bindClean) {   // orthogonality: recompose R·su and diff the 3x3 (a sheared W0 must NOT take this path)
            float x=w0q[0],y=w0q[1],z=w0q[2],w=w0q[3];
            float rc[9]={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y), 2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x), 2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
            for (int c=0;c<3 && bindClean;c++) for (int rr=0;rr<3;rr++)
                if (std::fabs(rc[c*3+rr]*w0su - W0[c*4+rr]) > 1e-3f*std::max(std::fabs(w0su),1.f)) { bindClean = false; break; }
        }
        // ── SCHEME SELECTION (pick by SHEAR of the raw per-frame world matrix, NOT by W0 uniformity) ──
        // variant 0 = UNIT bind (Tref·Rref) + per-axis scale-RATIO clip + Wref-baked rest. Handles uniform OR
        //   NON-uniform axis-aligned scale (per-axis ratio) at ANY tiny world scale (unit bind -> invBind never
        //   blows up). REQUIRES W(f) to be a clean TRS (matTrs = T·R·diag(S), no shear).
        // variant 1 = identity bind + RELATIVE M(f)=W(f)·inv(W0) clip + W0-baked rest (cancels a MIRROR/sheared
        //   W0). ⚠ inv(W0) EXPLODES when W0 scale is tiny -> M(f) translation ~|T0|/s0 huge -> ACL quantization
        //   thrash = the mesh TELEPORTS out of place (rifthome flamecube2-5: node scale 1.2 × 0.01 root = ~0.012
        //   world -> M(f) transl ~333). So v1 is ONLY for genuine shear, never merely non-uniform scale.
        // variant 2 = identity bind + RAW W(f) clip + LOCAL rest (non-uniform W0 AND animated scale, shear case).
        // A clean-TRS node (all the flames/leaves: translation + axis-aligned scale, zero shear) ALWAYS takes the
        // stable variant 0; only a truly SHEARED W(f) (a parent's non-uniform scale × this node's rotation) falls
        // back to v1/v2. (bindClean is kept only for the [GLTF-RIGID] verbose log below.)
        auto shearOf = [&](bool useM) -> float {
            float worst = 0.f; int cf[4] = {0, NF/3, (2*NF)/3, NF-1};
            for (int ci = 0; ci < 4; ci++) {
                float M[16];
                if (useM) mul44(&mats[(size_t)cf[ci]*16], invW0, M); else memcpy(M, &mats[(size_t)cf[ci]*16], 64);
                float q[4], t[3], s[3]; matTrs(M, q, t, s);
                float x=q[0],y=q[1],z=q[2],w=q[3], rc[9]={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y), 2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x), 2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
                for (int c=0;c<3;c++) for (int rr=0;rr<3;rr++){ float dv=std::fabs(rc[c*3+rr]*s[c]-M[c*4+rr]); if (dv>worst) worst=dv; }
            }
            return worst; };
        int variant = bindClean ? 0 : 1;                                  // proven baseline (275/275): uniform clean W0 -> v0
        if (!bindClean) {
            float shRaw = shearOf(false), shM = shearOf(true);
            if (shM > 1e-2f && shRaw < shM) variant = 2;                  // sheared W0 -> raw W(f) + LOCAL rest (as before)
            else if (w0su < 0.1f && shRaw < 1e-3f) variant = 0;           // TARGETED: non-uniform but CLEAN-TRS at TINY world
        }                                                                  //   scale -> v0 ref-frame (flamecube2-5; v1's M(f)
                                                                          //   =W(f)·inv(W0) explodes at tiny scale -> teleport)
        // (Far-from-origin SPINNERS — synthwave/steam_void huge distant cylinders, |W0t|~880 + rotDev 2.8, maxErr ~36 —
        // are NOT variant-fixable: decompErr=0 rules out shear, and both v0 and v2 reconstruct the same ~2.7% error on a
        // ~1300u motion. It's a deeper reconstruction/precision limit for very large far spinning meshes; left as-is.)
        // BIND SCALE = 1 + clip carries the per-axis scale RATIO to a REFERENCE frame (the OPA "beams out of
        // place" fix, ported to glTF — rifthome lantern flames "shoot in from the sides / drag"). The device forms
        // invBind from the DECOMPOSED bind TRS; binding with the frame's world scale is FATAL when it is tiny
        // (flame ~1e-5, from the ~0.01 scene-root × 0.001 node scale): invBind = inv(W) blows up (scale 1/s,
        // translation |T|/s ~7400) and the device amplifies every ACL quant error by it -> the mesh thrashes.
        // FIX: bind with UNIT scale (invBind = inv(Tref·Rref), scale 1, NO amplification) and store the mover
        // clip's scale as the per-frame RATIO s(f)/sref. Then vert = clip·inv(Tref·Rref)·(Wref·base) = W(f)·base
        // EXACTLY, WORLD-baked rest STAYS at the hearth (variant-2's LOCAL rest sat at the origin = the flame
        // "shot in from the sides"), anchors keep ±50000 (bind scale 1 -> no cull collapse).
        // REFERENCE = frame 0, UNLESS its scale is degenerate (flame frame-0 scale ~1e-5, invisible): then s(f)/s0
        // EXPLODES past ACL's encodable range and the whole clip (T,R,S) decodes to GARBAGE. Use the MAX-scale
        // frame instead -> ratio stays in [0,1] and ACL is exact; the driven skin is unchanged (ref cancels).
        // HSR_RIGIDRAWSCALE restores the old raw-scale / scaled-bind encoding.
        bool ratioScale = (variant == 0) && w0su < 0.1f && !std::getenv("HSR_RIGIDRAWSCALE");   // ONLY tiny-world-scale v0 nodes; normal-scale keep the proven raw encoding
        float refQ[4]={w0q[0],w0q[1],w0q[2],w0q[3]}, refT[3]={w0t[0],w0t[1],w0t[2]}, refS[3]={w0s[0],w0s[1],w0s[2]};
        float Wref[16]; memcpy(Wref, W0, 64);
        if (ratioScale && w0su < 0.1f) {                 // frame-0 scale degenerate -> reference the MAX-scale frame
            int refF = 0; float best = -1.f;
            for (int f = 0; f < NF; f++) { float q[4],t[3],s[3]; matTrs(&mats[(size_t)f*16], q,t,s);
                float g = std::cbrt(std::fabs(s[0]*s[1]*s[2])); if (g > best) { best = g; refF = f; } }
            memcpy(Wref, &mats[(size_t)refF*16], 64); matTrs(Wref, refQ, refT, refS);
            if (std::getenv("HSR_VERBOSE"))
                fprintf(stderr, "[GLTF-RIGID] mesh%d node%d degenerate frame-0 scale %.2g -> REF frame %d (scale %.4f)\n",
                        meshIdx, node, w0su, refF, std::cbrt(std::fabs(refS[0]*refS[1]*refS[2])));
        }
        e.jointCount = 2; e.parents = {-1, 0};
        if (variant == 0) {
            e.jointPos   = {0,0,0, refT[0],refT[1],refT[2]};
            e.jointQuat  = {1,0,0,0, refQ[3],refQ[0],refQ[1],refQ[2]};   // (w,x,y,z) for HZAN:SKEL; matTrs q is (x,y,z,w)
            e.jointScale = {1, ratioScale ? 1.f : ((w0su>1e-4f||w0su<-1e-4f)?w0su:1.f)};
        } else {
            e.jointPos   = {0,0,0, 0,0,0};
            e.jointQuat  = {1,0,0,0, 1,0,0,0};   // both joints bind at IDENTITY -> device invBind = I
            e.jointScale = {1, 1};
        }
        e.trsLocal.resize((size_t)NF*2*10);
        float decompErr = 0.f;   // TRS-representability residual of the mover clip (shear would show here)
        for (int f = 0; f < NF; f++) {
            float M[16];
            if (variant == 1) mul44(&mats[(size_t)f*16], invW0, M); // clip = relative M(f) (mirror/non-uniform W0 cancelled)
            else              memcpy(M, &mats[(size_t)f*16], 64);   // clip = W(f) directly (v0: invBind=inv(T0·R0) cancels; v2: LOCAL rest)
            float q[4], t[3], s[3]; matTrs(M, q, t, s);
            if (ratioScale) for (int c=0;c<3;c++) s[c] = (std::fabs(refS[c])>1e-8f) ? s[c]/refS[c] : 1.f;   // clip scale = ratio to the REFERENCE frame (bind is UNIT scale)
            float* r = &e.trsLocal[(size_t)(f*2+0)*10]; r[0]=0;r[1]=0;r[2]=0;r[3]=1; r[4]=0;r[5]=0;r[6]=0; r[7]=1;r[8]=1;r[9]=1;   // joint0 = STATIC identity (no root motion to extract)
            float* p = &e.trsLocal[(size_t)(f*2+1)*10]; p[0]=q[0];p[1]=q[1];p[2]=q[2];p[3]=q[3]; p[4]=t[0];p[5]=t[1];p[6]=t[2]; p[7]=s[0];p[8]=s[1];p[9]=s[2];
            float x=q[0],y=q[1],z=q[2],w=q[3], rc[9]={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y), 2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x), 2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
            for (int c=0;c<3;c++) for (int rr=0;rr<3;rr++){ float rawS=ratioScale?s[c]*refS[c]:s[c]; float dv=std::fabs(rc[c*3+rr]*rawS-M[c*4+rr]); if (dv>decompErr) decompErr=dv; }   // shear check uses RAW scale (clip stores the ratio)
        }
        if (decompErr > 1e-2f)
            fprintf(stderr, "[GLTF-RIGID] mesh%d node%d WARN mover clip not clean TRS (shear residual %.4f, variant=%d) — motion will deviate\n", meshIdx, node, decompErr, variant);
        // ONE-WAY PERIOD FIX ("reset is fucked up": drifting ensembles): a one-way clip ships WITHOUT the
        // cooker's seam frame, so its device duration was (NF-1)/fps = ONE SOURCE FRAME SHORT of the real
        // period — every loop the teleport fires 1/fps early and the mesh drifts out of phase against its
        // siblings/source (shark_reef fish school: 33ms/loop). Append ONE frame sampled at the TRUE clip end
        // (clipDur-ε — NOT clipDur, where per-node wrap would give the frame-0 pose = a backward-slide lerp)
        // -> duration NF/fps = clipDur exactly. Cyclic clips are untouched (the cooker's appended frame-0 seam
        // already lands their duration on clipDur).
        {
            float drift = 0.f;
            const float* d0 = &e.trsLocal[10];                       // mover joint, frame 0
            const float* dL = &e.trsLocal[((size_t)(NF-1)*2+1)*10];  // mover joint, last frame
            for (int c = 4; c < 7; c++) { float d = d0[c]-dL[c]; if (d<0) d=-d; if (d>drift) drift=d; }
            if (drift > 5.0f) {   // same threshold as the cooker's oneWay split
                float M[16], W[16]; worldAt(node, clipDur - 1e-4f, W);
                if (variant == 1) mul44(W, invW0, M); else memcpy(M, W, 64);
                float q[4], t[3], s[3]; matTrs(M, q, t, s);
                if (ratioScale) for (int c=0;c<3;c++) s[c] = (std::fabs(refS[c])>1e-8f) ? s[c]/refS[c] : 1.f;
                e.trsLocal.resize((size_t)(NF+1)*2*10);
                float* r = &e.trsLocal[(size_t)(NF*2+0)*10]; r[0]=0;r[1]=0;r[2]=0;r[3]=1; r[4]=0;r[5]=0;r[6]=0; r[7]=1;r[8]=1;r[9]=1;
                float* p = &e.trsLocal[(size_t)(NF*2+1)*10]; p[0]=q[0];p[1]=q[1];p[2]=q[2];p[3]=q[3]; p[4]=t[0];p[5]=t[1];p[6]=t[2]; p[7]=s[0];p[8]=s[1];p[9]=s[2];
                NF += 1;
            }
        }
        // QUATERNION HEMISPHERE CONTINUITY (the cyan "very shaky" fix): matTrs's per-frame quat sign is
        // arbitrary; a mill/gear spin crossing 180 deg flips polarity and the decoder's neighbor-lerp takes the
        // LONG arc for one interval = a twitch every crossing. Keep the mover's quats sign-continuous.
        for (int f = 1; f < NF; f++) {
            float* q = &e.trsLocal[((size_t)f*2 + 1)*10]; const float* p = &e.trsLocal[((size_t)(f-1)*2 + 1)*10];
            if (q[0]*p[0]+q[1]*p[1]+q[2]*p[2]+q[3]*p[3] < 0.f) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; q[3]=-q[3]; }
        }
        size_t nv = rec->basePos.size()/3;
        if (variant == 2) e.restPos = rec->basePos;   // LOCAL rest: the raw W(f) clip carries the full placement (vert = W(f)·base)
        else {
            e.restPos.resize(nv*3);   // verts WORLD-baked into the REFERENCE-frame rest (Wref = W0 except the degenerate-scale case; v1 uses W0)
            for (size_t v = 0; v < nv; v++) { const float* b = &rec->basePos[v*3]; float* o = &e.restPos[v*3];
                o[0]=Wref[0]*b[0]+Wref[4]*b[1]+Wref[8]*b[2]+Wref[12];
                o[1]=Wref[1]*b[0]+Wref[5]*b[1]+Wref[9]*b[2]+Wref[13];
                o[2]=Wref[2]*b[0]+Wref[6]*b[1]+Wref[10]*b[2]+Wref[14]; }
        }
        e.boneIdx.assign(nv*4, 0); e.boneWgt.assign(nv*4, 0);
        for (size_t v = 0; v < nv; v++) { e.boneIdx[v*4]=1; e.boneWgt[v*4]=255; }   // weight every vert to the MOVING joint
        e.frameCount = NF; e.fps = bakeFps;   // NF frames at the SOURCE key rate (grid-aligned); the cooker's loopWrap appends the seam frame for cyclic clips
        if (std::getenv("HSR_VERBOSE"))
            fprintf(stderr, "[GLTF-RIGID] mesh%d node%d NF=%d fps=%.1f dur=%.2fs travel=%.2f rotDev=%.2f sclDev=%.2f bindClean=%d W0t=(%.1f,%.1f,%.1f) decompErr=%.4f\n",
                    meshIdx, node, NF, bakeFps, clipDur, maxd, rotMaxd, sclMaxd, (int)bindClean, W0[12], W0[13], W0[14], decompErr);
        return e;
    }
    // ── COOK: EXACT per-frame QUATERNION rotation replay for a node-animated glTF mesh — the OPA
    //    cookExtractRotationReplay port (shadergen::ROTREPLAY, same consumer). For rotation-ONLY node anims whose
    //    motion is NOT a clean continuous spin or gentle sway: cyan's gears ramp 63° MONOTONICALLY per 8.3s loop
    //    (source wraps invisibly on the gear's symmetry), which the single-cosine SWAY fit turned into a one-sided
    //    rocking = "wrong animation on gears". Replay the node's ACTUAL relative world rotation instead — faithful
    //    shape + phase, self-looping getTime() vertex shader (no animator LOD). Returns (N+1)*4 xyzw quats rel to
    //    frame 0 (hemisphere-continuous for the shader's nlerp), the node's world pivot, and the per-node loop.
    bool cookExtractRotationReplay(int meshIdx, int N, float pivotOut[3], std::vector<float>& quats, float& loopSec) {
        quats.clear(); loopSec = 0.f;
        const NodeAnimRec* rec = nullptr;
        for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { rec = &r; break; }
        if (!rec || N < 2) return false;
        int node = rec->nodeIdx; if (node < 0 || (size_t)node >= gnodes.size()) return false;
        float clipDur = 0.f;   // per-node loop = latest key among the node's + ancestors' channels (same as the rigid path)
        for (int n = node, g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g)
            for (auto& ch : gchannels)
                if (ch.node == n && ch.sampler >= 0 && (size_t)ch.sampler < gsamplers.size()) {
                    auto& tv = gsamplers[ch.sampler].times;
                    if (!tv.empty() && tv.back() > clipDur) clipDur = tv.back();
                }
        if (clipDur <= 1e-4f) clipDur = animDuration;
        if (clipDur <= 1e-4f) return false;
        loopSec = clipDur;
        auto trsMat = [](const float* t, const float* r, const float* sc, float* mm){ float x=r[0],y=r[1],z=r[2],ww=r[3];
            mm[0]=(1-2*(y*y+z*z))*sc[0]; mm[1]=(2*(x*y+ww*z))*sc[0]; mm[2]=(2*(x*z-ww*y))*sc[0]; mm[3]=0;
            mm[4]=(2*(x*y-ww*z))*sc[1]; mm[5]=(1-2*(x*x+z*z))*sc[1]; mm[6]=(2*(y*z+ww*x))*sc[1]; mm[7]=0;
            mm[8]=(2*(x*z+ww*y))*sc[2]; mm[9]=(2*(y*z-ww*x))*sc[2]; mm[10]=(1-2*(x*x+y*y))*sc[2]; mm[11]=0;
            mm[12]=t[0]; mm[13]=t[1]; mm[14]=t[2]; mm[15]=1; };
        auto mulMat = [](const float* a, const float* b, float* o){
            for (int c=0;c<4;c++) for (int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        std::function<void(int,float,float*)> worldAt = [&](int n, float t, float* out){
            float tt[3]={gnodes[n].t[0],gnodes[n].t[1],gnodes[n].t[2]}, q[4]={gnodes[n].r[0],gnodes[n].r[1],gnodes[n].r[2],gnodes[n].r[3]}, sc[3]={gnodes[n].s[0],gnodes[n].s[1],gnodes[n].s[2]};
            for (auto& ch : gchannels) if (ch.node==n && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){
                float o4[4]; sampleSampler(gsamplers[ch.sampler], chTime(n, t), o4);   // per-node wrap = bake matches the preview (V79 per-track loop)
                if (ch.path==0){ tt[0]=o4[0]; tt[1]=o4[1]; tt[2]=o4[2]; }
                else if (ch.path==1){ q[0]=o4[0]; q[1]=o4[1]; q[2]=o4[2]; q[3]=o4[3]; }
                else if (ch.path==2){ sc[0]=o4[0]; sc[1]=o4[1]; sc[2]=o4[2]; } }
            float loc[16]; trsMat(tt,q,sc,loc);
            if (gnodes[n].parent>=0 && (size_t)gnodes[n].parent<gnodes.size()){ float pw[16]; worldAt(gnodes[n].parent,t,pw); mulMat(pw,loc,out); }
            else memcpy(out,loc,64); };
        auto matQuat = [](const float* m, float* q /*xyzw*/){   // normalized 3x3 -> quat (scale stripped)
            float sx=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]), sy=sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]), sz=sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=sx>1e-8f?1/sx:0, iy=sy>1e-8f?1/sy:0, iz=sz>1e-8f?1/sz:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if (tr>0){ float S=sqrtf(tr+1)*2; q[3]=0.25f*S; q[0]=(r5-r7)/S; q[1]=(r6-r2)/S; q[2]=(r1-r3)/S; }
            else if (r0>r4&&r0>r8){ float S=sqrtf(1+r0-r4-r8)*2; q[3]=(r5-r7)/S; q[0]=0.25f*S; q[1]=(r3+r1)/S; q[2]=(r6+r2)/S; }
            else if (r4>r8){ float S=sqrtf(1+r4-r0-r8)*2; q[3]=(r6-r2)/S; q[0]=(r3+r1)/S; q[1]=0.25f*S; q[2]=(r7+r5)/S; }
            else { float S=sqrtf(1+r8-r0-r4)*2; q[3]=(r1-r3)/S; q[0]=(r6+r2)/S; q[1]=(r7+r5)/S; q[2]=0.25f*S; } };
        float W0[16]; worldAt(node, 0.f, W0);
        pivotOut[0]=W0[12]; pivotOut[1]=W0[13]; pivotOut[2]=W0[14];
        // RELATIVE rotation from M(f) = W(f)·inv(W0), NOT quat(W)·conj(quat(W0)): a MIRRORED node (uniform
        // negative scale — outerwilds' planets/Interloper all ship S=-22..-56) has det(3x3)<0 and matQuat of
        // it is garbage (a rotoinversion has no quaternion). The mirror CANCELS in W(f)·inv(W0), leaving the
        // clean relative rotation. Identical result for positive-scale nodes (no regression; aurora lesson:
        // still NO handedness flip anywhere).
        float invW0r[16]; { const float* m=W0; float a=m[0],b=m[4],c=m[8], d=m[1],e2=m[5],f2=m[9], g2=m[2],h=m[6],i2=m[10];
            float det=a*(e2*i2-f2*h)-b*(d*i2-f2*g2)+c*(d*h-e2*g2); float id=(std::fabs(det)>1e-12f)?1.f/det:0.f;
            invW0r[0]=(e2*i2-f2*h)*id; invW0r[4]=(c*h-b*i2)*id; invW0r[8]=(b*f2-c*e2)*id;
            invW0r[1]=(f2*g2-d*i2)*id; invW0r[5]=(a*i2-c*g2)*id; invW0r[9]=(c*d-a*f2)*id;
            invW0r[2]=(d*h-e2*g2)*id;  invW0r[6]=(b*g2-a*h)*id;  invW0r[10]=(a*e2-b*d)*id;
            invW0r[3]=invW0r[7]=invW0r[11]=0.f; invW0r[15]=1.f;
            invW0r[12]=-(invW0r[0]*m[12]+invW0r[4]*m[13]+invW0r[8]*m[14]);
            invW0r[13]=-(invW0r[1]*m[12]+invW0r[5]*m[13]+invW0r[9]*m[14]);
            invW0r[14]=-(invW0r[2]*m[12]+invW0r[6]*m[13]+invW0r[10]*m[14]); }
        quats.resize((size_t)(N+1)*4); float bestMag = 0.f;
        for (int f = 0; f <= N; f++) {
            float W[16]; worldAt(node, clipDur*(float)f/(float)N, W);
            float M[16]; mulMat(W, invW0r, M);
            float r[4]; matQuat(M, r);   // rel rotation (xyzw), mirror/static-scale cancelled
            if (f > 0) { float* p = &quats[(size_t)(f-1)*4];   // hemisphere continuity -> the shader nlerp takes the short arc
                if (r[0]*p[0]+r[1]*p[1]+r[2]*p[2]+r[3]*p[3] < 0) { r[0]=-r[0]; r[1]=-r[1]; r[2]=-r[2]; r[3]=-r[3]; } }
            float* o = &quats[(size_t)f*4]; o[0]=r[0]; o[1]=r[1]; o[2]=r[2]; o[3]=r[3];
            float mag = sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); if (mag > bestMag) bestMag = mag;
        }
        if (bestMag < 0.02f) { quats.clear(); return false; }   // node doesn't actually rotate
        return true;
    }
    // Extract a uniform NODE Y-ROTATION track (Outer Wilds skybox = +Y 333s; Interloper = -Y 81s). Walks the parent
    // chain so a mesh INHERITS an animated ancestor's spin — the OW planets are children of the rotating skybox node,
    // so they ORBIT origin (the skybox node's pivot), not spin in place. Returns period (one revolution, s), dir
    // (+1/-1), and the rotation node's WORLD pivot. The cooker bakes the geometry RELATIVE to that pivot + puts the
    // entity at the pivot, so the getTime() Y-rotation shader spins the node-mesh in place AND orbits its children.
    // GENERAL node rotation: world-space AXIS (unit), signed OMEGA (rad/s), and the PIVOT (the rotation node's world
    // origin). Works for ANY axis (Dragon Ball Snake Way's TILTED King Kai planet) AND compound spins (OW Interloper =
    // own spin + orbiting skybox node). Method: sample the FULL composed world transform at t=0 and t=dt; each vertex's
    // world DISPLACEMENT is perpendicular to the rotation axis, so axis = cross(dispA, dispB) — robust to mirror/
    // negative scale (baked into the rest pose, cancels in M1·M0^-1). The cooker generates a Rodrigues shader for it.
    bool extractNodeRotation(int meshIdx, float axis[3], float& omega, float pivot[3],
                             bool& isOsc, float& amp, float& period) const {
        isOsc=false; amp=0.f; period=0.f; omega=0.f;
        int nodeIdx = -1; const std::vector<float>* base = nullptr;
        for (auto& r : nodeAnimRecs) if (r.meshIdx == meshIdx) { nodeIdx = r.nodeIdx; base = &r.basePos; break; }
        if (nodeIdx < 0 || !base || base->size() < 9 || animDuration <= 0.f) return false;
        int rotNode = -1;          // nearest ancestor (incl. self) with a rotation channel -> the pivot node
        for (int n = nodeIdx; n >= 0 && (size_t)n < gnodes.size(); n = gnodes[n].parent) {
            bool has=false; for (auto& ch : gchannels) if (ch.node==n && ch.path==1){ has=true; break; }
            if (has){ rotNode=n; break; }
        }
        if (rotNode < 0) return false;
        // FREEZE rotNode's animated ANCESTORS at t=0 so we isolate rotNode's OWN rotation. Otherwise a node that both
        // spins itself AND rides a spinning ancestor (OW Interloper = node 3 spin under node 8 'skybox' Y-spin) gets a
        // COMPOUND net axis that no longer passes through the mesh -> a flat billboard offset from rotNode tumbles
        // edge-on out of view (invisible). The planets are unaffected: node 8 IS their rotNode (no own channel), so they
        // still inherit/orbit it. (A node legitimately inheriting an ancestor's spin has NO own rotation channel.)
        std::vector<char> freeze(gnodes.size(), 0);
        for (int n = gnodes[rotNode].parent; n >= 0 && (size_t)n < gnodes.size(); n = gnodes[n].parent) freeze[n]=1;
        float clipDur = 0.f;       // the rotation NODE's OWN clip length = the period basis (NOT the global animDuration;
        for (auto& ch : gchannels) // sampling past a clip's last key clamps -> a bogus average for compound/short clips)
            if (ch.node==rotNode && ch.path==1 && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){ auto& tv=gsamplers[ch.sampler].times; if(!tv.empty()&&tv.back()>clipDur) clipDur=tv.back(); }
        if (clipDur <= 1e-4f) clipDur = animDuration;
        auto trsMat=[](const float* t,const float* r,const float* sc,float* mm){ float x=r[0],y=r[1],z=r[2],ww=r[3];
            mm[0]=(1-2*(y*y+z*z))*sc[0]; mm[1]=(2*(x*y+ww*z))*sc[0]; mm[2]=(2*(x*z-ww*y))*sc[0]; mm[3]=0;
            mm[4]=(2*(x*y-ww*z))*sc[1]; mm[5]=(1-2*(x*x+z*z))*sc[1]; mm[6]=(2*(y*z+ww*x))*sc[1]; mm[7]=0;
            mm[8]=(2*(x*z+ww*y))*sc[2]; mm[9]=(2*(y*z-ww*x))*sc[2]; mm[10]=(1-2*(x*x+y*y))*sc[2]; mm[11]=0;
            mm[12]=t[0]; mm[13]=t[1]; mm[14]=t[2]; mm[15]=1; };
        auto mulMat=[](const float* a,const float* b,float* o){ for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        std::function<void(int,float,float*)> worldAt=[&](int n,float t,float* out){
            float teff = (n>=0 && (size_t)n<freeze.size() && freeze[n]) ? 0.f : t;   // freeze animated ancestors -> isolate rotNode's own spin
            float q[4]={gnodes[n].r[0],gnodes[n].r[1],gnodes[n].r[2],gnodes[n].r[3]};
            for (auto& ch : gchannels) if (ch.node==n && ch.path==1 && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){ float o4[4]; sampleSampler(gsamplers[ch.sampler], teff, o4); for(int c=0;c<4;c++)q[c]=o4[c]; break; }
            float loc[16]; trsMat(gnodes[n].t, q, gnodes[n].s, loc);
            if (gnodes[n].parent>=0 && (size_t)gnodes[n].parent<gnodes.size()){ float pw[16]; worldAt(gnodes[n].parent,t,pw); mulMat(pw,loc,out); } else memcpy(out,loc,64); };
        auto xf=[](const float* m, const float* p, float* o){ for(int k=0;k<3;k++) o[k]=m[k]*p[0]+m[4+k]*p[1]+m[8+k]*p[2]+m[12+k]; };
        auto cross=[](const float* a,const float* b,float* o){ o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; };
        auto dot=[](const float* a,const float* b){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
        auto nrm=[](float* a){ float l=sqrtf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); if(l>1e-9f){a[0]/=l;a[1]/=l;a[2]/=l;} return l; };
        float pm[16]; worldAt(rotNode, 0.f, pm); pivot[0]=pm[12]; pivot[1]=pm[13]; pivot[2]=pm[14];
        size_t nv = base->size()/3;
        float m0[16]; worldAt(nodeIdx, 0.f, m0);
        int ai=-1; float aR=0.f;                            // probe vertex A = farthest from pivot (cleanest arc)
        for (size_t v=0; v<nv; v++){ float p[3]={(*base)[v*3],(*base)[v*3+1],(*base)[v*3+2]}, w[3]; xf(m0,p,w);
            float r=(w[0]-pivot[0])*(w[0]-pivot[0])+(w[1]-pivot[1])*(w[1]-pivot[1])+(w[2]-pivot[2])*(w[2]-pivot[2]); if(r>aR){aR=r;ai=(int)v;} }
        if (ai<0 || aR<1e-2f) return false;
        float pa[3]={(*base)[ai*3],(*base)[ai*3+1],(*base)[ai*3+2]};
        // Sample A's WORLD position across the rotation node's OWN clip -> the radius vector r(t)=pos(t)-pivot traces an
        // arc. A continuous spin sweeps a full turn (|total swept| big); a sway swings out and back (zero net, bounded
        // peak). Sampling the whole clip (not just t=0) is REQUIRED: a (1-cos) sway has ZERO velocity at t=0, so a single
        // dt step there reads a bogus near-static "slow spin" (the Snake Way 1047s bug). Classify by the accumulated sweep.
        const int NS=96;
        std::vector<float> R((size_t)(NS+1)*3);
        for (int i=0;i<=NS;i++){ float mm[16]; worldAt(nodeIdx, clipDur*(float)i/(float)NS, mm); float w[3]; xf(mm,pa,w);
            R[(size_t)i*3]=w[0]-pivot[0]; R[(size_t)i*3+1]=w[1]-pivot[1]; R[(size_t)i*3+2]=w[2]-pivot[2]; }
        float* r0=&R[0]; float r0l=sqrtf(dot(r0,r0)); if (r0l<1e-3f) return false;
        // AXIS from TANGENTIAL displacements (always perp to the axis -> robust even for off-equator vertices, unlike
        // r0 x r(t) which tilts by the axial component). Take the step of MAX speed (a (1-cos) sway has zero velocity at
        // t=0): dA x dB over two probe vertices at that step = the rotation axis.
        int ti=0; float bestV=0.f;                          // step with the largest probe-A displacement = max angular speed
        for (int i=0;i<NS;i++){ float dx=R[(size_t)(i+1)*3]-R[(size_t)i*3], dy=R[(size_t)(i+1)*3+1]-R[(size_t)i*3+1], dz=R[(size_t)(i+1)*3+2]-R[(size_t)i*3+2]; float s=dx*dx+dy*dy+dz*dz; if(s>bestV){bestV=s;ti=i;} }
        if (bestV < 1e-12f) return false;                   // never moves
        float ta=clipDur*(float)ti/(float)NS, tb=clipDur*(float)(ti+1)/(float)NS;
        float mA[16], mB[16]; worldAt(nodeIdx, ta, mA); worldAt(nodeIdx, tb, mB);
        // TWO INDEPENDENT motion kinds (the OW planets ORBIT the skybox node; the skybox/Snake-Way planet SPIN IN
        // PLACE). Decide by whether the CENTROID travels: sample the centroid trajectory over the clip.
        const int KC=24;
        std::vector<float> Cc((size_t)(KC+1)*3);
        for (int i=0;i<=KC;i++){ float mm[16]; worldAt(nodeIdx, clipDur*(float)i/(float)KC, mm);
            double sx=0,sy=0,sz=0; for(size_t v=0;v<nv;v++){ float w[3]; xf(mm,&(*base)[v*3],w); sx+=w[0];sy+=w[1];sz+=w[2]; }
            Cc[(size_t)i*3]=(float)(sx/(double)nv); Cc[(size_t)i*3+1]=(float)(sy/(double)nv); Cc[(size_t)i*3+2]=(float)(sz/(double)nv); }
        float Cm[3]={0,0,0}; for(int i=0;i<=KC;i++){ Cm[0]+=Cc[(size_t)i*3]; Cm[1]+=Cc[(size_t)i*3+1]; Cm[2]+=Cc[(size_t)i*3+2]; }
        Cm[0]/=(KC+1);Cm[1]/=(KC+1);Cm[2]/=(KC+1);
        float orbitSpan=0.f; for(int i=0;i<=KC;i++){ float d[3]={Cc[(size_t)i*3]-Cm[0],Cc[(size_t)i*3+1]-Cm[1],Cc[(size_t)i*3+2]-Cm[2]}; float s=dot(d,d); if(s>orbitSpan)orbitSpan=s; } orbitSpan=sqrtf(orbitSpan);
        float bodySpan=0.f; for(size_t v=0;v<nv;v++){ float w[3]; xf(m0,&(*base)[v*3],w); float d[3]={w[0]-Cc[0],w[1]-Cc[1],w[2]-Cc[2]}; float s=dot(d,d); if(s>bodySpan)bodySpan=s; } bodySpan=sqrtf(bodySpan);
        float ax[3]={0,0,0};
        if (orbitSpan > bodySpan && orbitSpan > 1e-2f) {
            // ORBIT: axis = normal of the centroid's swept plane (measured from the ORBIT CENTER Cm, so it's immune to
            // the off-equator tilt AND to slow-rotation per-step noise that made the 333s planets' axis wobble off-Y).
            for (int i=0;i<KC;i++){ float a[3]={Cc[(size_t)i*3]-Cm[0],Cc[(size_t)i*3+1]-Cm[1],Cc[(size_t)i*3+2]-Cm[2]},
                b[3]={Cc[(size_t)(i+1)*3]-Cm[0],Cc[(size_t)(i+1)*3+1]-Cm[1],Cc[(size_t)(i+1)*3+2]-Cm[2]}, c[3]; cross(a,b,c); ax[0]+=c[0];ax[1]+=c[1];ax[2]+=c[2]; }
        } else {
            // SPIN IN PLACE: axis = ANGULAR MOMENTUM L = sum (p_i - C) x v_i at the max-velocity step (C = vertex
            // centroid). L is PARALLEL to omega for any rigid rotation and clean for SOLID meshes (skybox dome, sphere).
            float C[3]={0,0,0}; std::vector<float> P0((size_t)nv*3), VV((size_t)nv*3);
            for (size_t v=0; v<nv; v++){ float p[3]={(*base)[v*3],(*base)[v*3+1],(*base)[v*3+2]}, q0[3],q1[3]; xf(mA,p,q0); xf(mB,p,q1);
                P0[v*3]=q0[0];P0[v*3+1]=q0[1];P0[v*3+2]=q0[2]; VV[v*3]=q1[0]-q0[0];VV[v*3+1]=q1[1]-q0[1];VV[v*3+2]=q1[2]-q0[2];
                C[0]+=q0[0];C[1]+=q0[1];C[2]+=q0[2]; }
            C[0]/=(float)nv;C[1]/=(float)nv;C[2]/=(float)nv;
            for (size_t v=0; v<nv; v++){ float ri[3]={P0[v*3]-C[0],P0[v*3+1]-C[1],P0[v*3+2]-C[2]}, c[3]; cross(ri,&VV[v*3],c); ax[0]+=c[0];ax[1]+=c[1];ax[2]+=c[2]; }
        }
        if (nrm(ax) < 1e-6f) return false;
        auto signedAng=[&](const float* a,const float* b)->float{ float da=dot(a,ax),db=dot(b,ax);
            float pA[3]={a[0]-da*ax[0],a[1]-da*ax[1],a[2]-da*ax[2]}, pB[3]={b[0]-db*ax[0],b[1]-db*ax[1],b[2]-db*ax[2]};
            float c[3]; cross(pA,pB,c); return atan2f(dot(c,ax), dot(pA,pB)); };
        float total=0.f, peak=0.f, maxR=0.f, minR=1e30f;    // total = net signed sweep; peak = signed extreme vs r0
        for (int i=1;i<=NS;i++){ total += signedAng(&R[(size_t)(i-1)*3], &R[(size_t)i*3]);
            float th = signedAng(r0, &R[(size_t)i*3]); if (fabsf(th)>fabsf(peak)) peak=th;
            float rl=sqrtf(dot(&R[(size_t)i*3],&R[(size_t)i*3])); if(rl>maxR)maxR=rl; if(rl<minR)minR=rl; }
        if (maxR-minR > 0.15f*r0l + 1e-3f) return false;     // radius must be ~constant -> a pivot rotation, not a translation
        // Canonicalize the axis (dominant component positive) so the DIRECTION lives unambiguously in the SIGN of
        // omega/amp — deterministic across meshes that share one physical rotation (the cross-product axis sign is
        // otherwise arbitrary, e.g. the OW planets came out +Y vs -Y for the same orbit). total/peak flip with it.
        int dom=0; if (fabsf(ax[1])>fabsf(ax[dom])) dom=1; if (fabsf(ax[2])>fabsf(ax[dom])) dom=2;
        if (ax[dom] < 0.f) { ax[0]=-ax[0]; ax[1]=-ax[1]; ax[2]=-ax[2]; total=-total; peak=-peak; }
        axis[0]=ax[0]; axis[1]=ax[1]; axis[2]=ax[2];
        if (fabsf(total) > 4.712f) {                         // > ~270deg accumulated -> CONTINUOUS spin
            omega = total/clipDur; isOsc=false; period=clipDur; return fabsf(omega) > 1e-4f;   // signed omega = CW/CCW direction
        }
        isOsc=true; amp=peak; period=clipDur; omega=0.f;     // bounded swing -> OSCILLATION (sway); signed amp = swing direction
        return fabsf(amp) > 0.02f;                            // >~1deg sway, else treat as static
    }
    // Repair UNWEIGHTED skin verts (weightSum~0) — the whale-eyes fix: a modeler left the eye spheres with no skin
    // weight so they froze at bind pose while the skeleton swam the body. COPY the full skin (jidx+jw) of the NEAREST
    // ALREADY-WEIGHTED vertex, so each loose vert moves EXACTLY like the surface next to it (the eyes ride the head's
    // own blend). Binding to a single nearest JOINT tore the eye apart — adjacent verts landed on different joints
    // that pull apart ("eye moving out"); inheriting a neighbour's weights keeps the piece rigid + seamless. Idempotent.
    int autoBindLooseVertsRec(SkinnedRec& rec) {
        if (rec.skin<0 || (size_t)rec.skin>=gskins.size()) return 0;
        if (rec.jw.size() < (size_t)rec.nv*4 || rec.jidx.size() < (size_t)rec.nv*4 || rec.basePos.size() < (size_t)rec.nv*3) return 0;
        // indices of weighted "donor" verts
        std::vector<u32> donor; donor.reserve(rec.nv);
        for (u32 u=0; u<rec.nv; ++u) if (rec.jw[u*4]+rec.jw[u*4+1]+rec.jw[u*4+2]+rec.jw[u*4+3] > 0.1f) donor.push_back(u);
        if (donor.empty()) return 0;   // nothing weighted to copy from
        int fixed=0;
        for (u32 v=0; v<rec.nv; ++v){
            if (rec.jw[v*4]+rec.jw[v*4+1]+rec.jw[v*4+2]+rec.jw[v*4+3] > 0.1f) continue;   // already weighted
            const float* p=&rec.basePos[v*3]; float best=1e30f; u32 bw=donor[0];
            for (u32 u : donor){ const float* q=&rec.basePos[u*3]; float dx=p[0]-q[0],dy=p[1]-q[1],dz=p[2]-q[2]; float d=dx*dx+dy*dy+dz*dz; if(d<best){best=d;bw=u;} }
            for (int k=0;k<4;k++){ rec.jidx[v*4+k]=rec.jidx[bw*4+k]; rec.jw[v*4+k]=rec.jw[bw*4+k]; } ++fixed;
        }
        return fixed;
    }
    // Right-click "Bind loose verts": apply the loose-vert bind to the skinned mesh at this editor/gltf mesh index.
    int applyLooseVertBindForMesh(int meshIdx) {
        int n=0; for (auto& r : skinned) if (r.meshIdx==meshIdx) n += autoBindLooseVertsRec(r);
        return n;
    }
    // How many UNWEIGHTED (static) verts a skinned mesh has — the menu shows/enables the action only when >0.
    int looseVertCount(int meshIdx) {
        for (auto& r : skinned) if (r.meshIdx==meshIdx && r.jw.size()>=(size_t)r.nv*4) {
            int c=0; for (u32 v=0; v<r.nv; ++v) if (r.jw[v*4]+r.jw[v*4+1]+r.jw[v*4+2]+r.jw[v*4+3] <= 0.1f) ++c; return c; }
        return 0;
    }
    HzAnimExport extractHzAnim(int meshIdx, int frames) {
        HzAnimExport e;
        const SkinnedRec* rec = nullptr;
        for (auto& r : skinned) if (r.meshIdx == meshIdx) { rec = &r; break; }
        if (!rec || rec->skin < 0 || (size_t)rec->skin >= gskins.size() || animDuration <= 0.f || frames < 2) return e;
        const GSkin& sk = gskins[rec->skin];
        int nj = (int)sk.joints.size(); if (nj < 1) return e;
        e.restPos = rec->basePos;   // rest mesh-space positions (skinning maps these -> animated world; world-baked positions fling it off-screen)
        std::vector<int> nodeToJoint(gnodes.size(), -1);
        for (int j = 0; j < nj; ++j) if ((size_t)sk.joints[j] < gnodes.size()) nodeToJoint[sk.joints[j]] = j;
        e.jointCount = nj;
        // The skin's OWN clip length — NOT the global animDuration. A 0.83s warp sampled over a 60s scene-max plays its
        // motion then FREEZES for 59s before looping = the "warp too slow / wrong speed" bug. = the latest keytime among
        // channels animating THIS skin's joints AND THEIR ANCESTORS (bubbles: each cluster's 166.8s RISE lives on the
        // animated instancer PARENT above the skin — scanning only joint channels truncated the clip to the joints'
        // 13-29s wobble = 5004 frames at fps 273-375, every cluster looping the first sliver of its path near spawn =
        // "bubbles cramming in one place" + "Instancer_8 ain't moving after cook"). STAGGER GUARD: a contributing
        // channel that STARTS late (>0.05s) is a segment of the global choreography, not a self-loop — keep the
        // GLOBAL duration so the ensemble phasing survives.
        float clipDur = 0.f, latestStart = 0.f; bool multiPeriod = false; float srcKeyRate = 0.f;   // NO 30fps floor (see extractNodeRigidHzAnim)
        std::vector<float> chainPeriods;
        { std::vector<char> relN(gnodes.size(), 0);
          for (int j = 0; j < nj; ++j)
              for (int n = sk.joints[j], g = 0; n >= 0 && (size_t)n < gnodes.size() && g < 64; n = gnodes[n].parent, ++g) relN[n] = 1;
          float firstEnd = -1.f;
          for (auto& ch : gchannels)
              if (ch.node>=0 && (size_t)ch.node<relN.size() && relN[ch.node]
                  && ch.sampler>=0 && (size_t)ch.sampler<gsamplers.size()){ auto& tv=gsamplers[ch.sampler].times;
                  if (!tv.empty()){ if (tv.back()>clipDur) clipDur=tv.back(); if (tv.front()>latestStart) latestStart=tv.front();
                      if (firstEnd < 0.f) firstEnd = tv.back();
                      else if (std::fabs(tv.back()-firstEnd) > 0.1f) multiPeriod = true;
                      bool nw = true; for (float p : chainPeriods) if (std::fabs(p-tv.back()) <= 0.05f) { nw = false; break; }
                      if (nw) chainPeriods.push_back(tv.back());
                      // native key rate of the DENSEST contributing channel — the clip must be baked at
                      // (at least) this rate or fast per-key motion aliases (see below). NON-UNIFORM tracks
                      // (Blender key-optimized exports) hide sharp corner keys between the average-rate grid
                      // (quiet_place: isolated 2.7u corner-cut spikes) — honor the MIN key gap too (<=120/s).
                      if (tv.size() >= 2 && tv.back()-tv.front() > 1e-4f) {
                          float r = (float)(tv.size()-1)/(tv.back()-tv.front()); if (r > srcKeyRate) srcKeyRate = r;
                          float mg = 1e9f; for (size_t kk = 1; kk < tv.size(); kk++){ float g = tv[kk]-tv[kk-1]; if (g > 1e-5f && g < mg) mg = g; }
                          if (mg < 1e8f) { float r2 = 1.f/mg; if (r2 > 120.f) r2 = 120.f; if (r2 > srcKeyRate) srcKeyRate = r2; } } } }
          if (srcKeyRate > 240.f) srcKeyRate = 240.f; if (srcKeyRate < 1.f) srcKeyRate = 30.f; }   // sanity cap (badly-authored micro-key tracks)
        // MULTI-PERIOD skin (bubbles trails: each joint/bubble loops on its OWN channel length — the periods
        // BEAT against each other, which is what SPREADS the trail). A HARMONIC chain (periods share a small
        // rational LCM) bakes the LCM — every channel closes at the clip end = seamless forever (the "reset
        // is fucked up" fix). An IRRATIONAL beat has no finite seamless loop — bake the FULL master timeline:
        // inside it every joint wraps on its own period exactly like the preview (chTime), and the only
        // artifact is one ensemble reshuffle per master loop (166.8s) instead of never.
        if (clipDur <= 1e-4f || latestStart > 0.05f) clipDur = animDuration;
        else if (multiPeriod) { float lcm = rationalLcmPeriod(chainPeriods); clipDur = (lcm > 1e-4f) ? lcm : animDuration; }
        // NATIVE sampling density: bake the clip at the SOURCE'S OWN key rate (srcKeyRate, from the densest
        // contributing channel), not a hardcoded 30/s. Both directions matter: the caller sizes `frames` off
        // the GLOBAL timeline (166.8s x 30fps = 5004 — a 16.7s clip was oversampled 10x = 3MB ACL for 500
        // real keys), AND the old fixed 30/s resample halved 60-key/s sources (fast streak tracks: 50
        // keys/0.83s SAWTOOTH per bone — the 26-frame bake lerped ACROSS each teleport wrap = streaks
        // snapping mid-flight, ANIM-VERIFY err 50u of a 103u span). At the native rate the bake's lerp
        // between frames IS the source's own lerp between keys — exact by construction.
        { int nat = (int)(clipDur * srcKeyRate + 0.5f); if (nat < 2) nat = 2; frames = nat; }
        e.jointPos.resize(nj*3); e.jointQuat.resize(nj*4); e.jointScale.resize(nj); e.parents.resize(nj);
        // Joints can be parented through INTERMEDIATE non-joint nodes (rig groups): the glTF node's LOCAL transform is
        // then NOT relative to the parent JOINT, so storing it directly + dropping the broken parent link gives false
        // roots + a wrong bind -> CURSED skinning (wrong joints, garbage deform). Fix: bake each joint's full WORLD
        // transform (compose the whole node hierarchy) for both bind AND clip, emit as ROOTS -> the renderer/device
        // compute worldAnim·inverse(worldBind) = correct standard glTF skinning regardless of intermediate nodes.
        auto trsM = [](const float* t, const float* r, const float* s, float* m){   // r=quat xyzw -> column-major mat4
            float x=r[0],y=r[1],z=r[2],w=r[3];
            m[0]=(1-2*(y*y+z*z))*s[0]; m[1]=(2*(x*y+w*z))*s[0];   m[2]=(2*(x*z-w*y))*s[0];   m[3]=0;
            m[4]=(2*(x*y-w*z))*s[1];   m[5]=(1-2*(x*x+z*z))*s[1]; m[6]=(2*(y*z+w*x))*s[1];   m[7]=0;
            m[8]=(2*(x*z+w*y))*s[2];   m[9]=(2*(y*z-w*x))*s[2];   m[10]=(1-2*(x*x+y*y))*s[2];m[11]=0;
            m[12]=t[0]; m[13]=t[1]; m[14]=t[2]; m[15]=1; };
        auto mulM = [](const float* a, const float* b, float* o){
            for (int c=0;c<4;c++) for (int rr=0;rr<4;rr++) o[c*4+rr]=a[rr]*b[c*4]+a[4+rr]*b[c*4+1]+a[8+rr]*b[c*4+2]+a[12+rr]*b[c*4+3]; };
        std::function<void(const std::vector<GNode>&,int,float*)> worldM = [&](const std::vector<GNode>& nd,int n,float* out){
            float loc[16]; trsM(nd[n].t, nd[n].r, nd[n].s, loc);
            if (nd[n].parent>=0 && (size_t)nd[n].parent<nd.size()){ float pw[16]; worldM(nd,nd[n].parent,pw); mulM(pw,loc,out); }
            else memcpy(out,loc,64); };
        // HZHIER (default): build a proper MODEL-SPACE joint hierarchy like every working skinned env (calming
        // butterflies: root->body->wings, LOCAL transforms near origin). The device skins skinMatrix = animPose *
        // inverseBind, building the bind by COMPOSING the hierarchy; a flat all-roots skeleton with WORLD transforms
        // (the old world-bake) gives no hierarchy to compose -> inverseBind can't cancel the huge world translation ->
        // mesh double-transforms OFF-SCREEN. Fix: each joint's transform = LOCAL relative to its nearest JOINT ancestor
        // (compose the intermediate non-joint nodes into it), emit the real parent index. Mesh restPos stays model-space.
        bool hzHier = !std::getenv("HSR_HZWORLDBAKE");   // default ON; HSR_HZWORLDBAKE = old flat world-bake (PC-sim-only)
        // nearest JOINT ancestor of node n (-1 = root); composes the local transform relative to it (no inverse needed).
        auto parentJointOf = [&](const std::vector<GNode>& nd, int n)->int {
            for (int p = nd[n].parent; p>=0 && (size_t)p<nd.size(); p = nd[p].parent) if (nodeToJoint[p]>=0) return nodeToJoint[p];
            return -1; };
        auto localRelToParentJoint = [&](const std::vector<GNode>& nd, int n, int pn, float* out){
            std::vector<int> path; for (int p=n; p!=pn && p>=0; p=nd[p].parent) path.push_back(p);
            float acc[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            for (int k=(int)path.size()-1; k>=0; --k){ float loc[16]; trsM(nd[path[k]].t,nd[path[k]].r,nd[path[k]].s,loc); float tmp[16]; mulM(acc,loc,tmp); memcpy(acc,tmp,64); }
            memcpy(out,acc,64); };
        auto matTrs = [](const float* m, float* q /*xyzw*/, float* t, float* s){
            t[0]=m[12]; t[1]=m[13]; t[2]=m[14];
            s[0]=sqrtf(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]); s[1]=sqrtf(m[4]*m[4]+m[5]*m[5]+m[6]*m[6]); s[2]=sqrtf(m[8]*m[8]+m[9]*m[9]+m[10]*m[10]);
            float ix=s[0]>1e-8f?1/s[0]:0, iy=s[1]>1e-8f?1/s[1]:0, iz=s[2]>1e-8f?1/s[2]:0;
            float r0=m[0]*ix,r1=m[1]*ix,r2=m[2]*ix, r3=m[4]*iy,r4=m[5]*iy,r5=m[6]*iy, r6=m[8]*iz,r7=m[9]*iz,r8=m[10]*iz;
            float tr=r0+r4+r8;
            if (tr>0){ float S=sqrtf(tr+1)*2; q[3]=0.25f*S; q[0]=(r5-r7)/S; q[1]=(r6-r2)/S; q[2]=(r1-r3)/S; }
            else if (r0>r4&&r0>r8){ float S=sqrtf(1+r0-r4-r8)*2; q[3]=(r5-r7)/S; q[0]=0.25f*S; q[1]=(r3+r1)/S; q[2]=(r6+r2)/S; }
            else if (r4>r8){ float S=sqrtf(1+r4-r0-r8)*2; q[3]=(r6-r2)/S; q[0]=(r3+r1)/S; q[1]=0.25f*S; q[2]=(r7+r5)/S; }
            else { float S=sqrtf(1+r8-r0-r4)*2; q[3]=(r1-r3)/S; q[0]=(r6+r2)/S; q[1]=(r7+r5)/S; q[2]=0.25f*S; } };
        // For the BIND reference, treat scale-0 nodes (e.g. the Shield assembly, collapsed at rest) as scale-1 so the
        // joint's world POSITION propagates correctly instead of collapsing to its parent's origin — otherwise the
        // Shield's bind lands at ~(0,0,0) and skins OFF the droid. The clip keeps the real 0->1 scale (drives the pop).
        std::vector<GNode> bindNodes = gnodes;
        for (auto& g : bindNodes) for (int k=0;k<3;k++) if (g.s[k]<1e-3f && g.s[k]>-1e-3f) g.s[k]=1.0f;
        // PER-JOINT IBM BIND (unofficial_bubbles fields "go in center"): the FILE's inverseBindMatrices can
        // differ from the rebuilt node-hierarchy bind PER JOINT (Maya instancer rigs bake each bubble's offset
        // into its IBM — measured up to 43u on 60/61 joints while joint 0 matched exactly, so the old
        // joint0-only residual check saw nothing and the global fold couldn't represent it anyway). Shipping
        // inv(nodeBind) as the inverse-bind then skinned every bubble toward its joint origin. FIX: when any
        // joint's IBM deviates, derive the shipped bind from the IBMs themselves — bindWorld'_j = inv(IBM_j) —
        // so the device's composed inverse-bind == IBM_j and skinning is EXACTLY the source math W_j(t)·IBM_j.
        auto affInv = [](const float* m, float* o){   // affine 4x4 inverse (column-major)
            float a=m[0],b=m[4],c=m[8], d=m[1],e2=m[5],f2=m[9], g2=m[2],h=m[6],i2=m[10];
            float det=a*(e2*i2-f2*h)-b*(d*i2-f2*g2)+c*(d*h-e2*g2); float id=(std::fabs(det)>1e-12f)?1.f/det:0.f;
            o[0]=(e2*i2-f2*h)*id; o[4]=(c*h-b*i2)*id; o[8]=(b*f2-c*e2)*id;
            o[1]=(f2*g2-d*i2)*id; o[5]=(a*i2-c*g2)*id; o[9]=(c*d-a*f2)*id;
            o[2]=(d*h-e2*g2)*id;  o[6]=(b*g2-a*h)*id;  o[10]=(a*e2-b*d)*id;
            o[3]=o[7]=o[11]=0.f; o[15]=1.f;
            o[12]=-(o[0]*m[12]+o[4]*m[13]+o[8]*m[14]);
            o[13]=-(o[1]*m[12]+o[5]*m[13]+o[9]*m[14]);
            o[14]=-(o[2]*m[12]+o[6]*m[13]+o[10]*m[14]); };
        bool ibmBind = false; std::vector<float> ibmBW;   // per-joint bindWorld' = inv(IBM_j)
        if (sk.ibm.size() >= (size_t)nj*16 && !std::getenv("HSR_NOIBMBIND")) {
            float maxDev = 0.f;
            for (int j = 0; j < nj; ++j) {
                float bw[16]; worldM(bindNodes, sk.joints[j], bw);
                float R[16]; mulM(bw, &sk.ibm[(size_t)j*16], R);
                for (int k=0;k<16;k++){ float id=(k%5==0)?1.f:0.f; float dv=std::fabs(R[k]-id); if (dv>maxDev) maxDev=dv; }
            }
            if (maxDev > 1e-3f) {
                ibmBind = true; ibmBW.resize((size_t)nj*16);
                for (int j = 0; j < nj; ++j) affInv(&sk.ibm[(size_t)j*16], &ibmBW[(size_t)j*16]);
                if (std::getenv("HSR_VERBOSE"))
                    fprintf(stderr, "[HZIBM] mesh %d: PER-JOINT IBM bind (max node-bind deviation %.2f) — shipping inv(IBM) binds\n", meshIdx, maxDev);
            }
        }
        for (int j = 0; j < nj; ++j) {
            int n = sk.joints[j];
            int pj = hzHier ? parentJointOf(bindNodes, n) : -1;
            float lm[16];
            if (ibmBind) {
                if (hzHier && pj >= 0) { float pinv[16]; affInv(&ibmBW[(size_t)pj*16], pinv); mulM(pinv, &ibmBW[(size_t)j*16], lm); }   // local = inv(B_parent)·B_j
                else memcpy(lm, &ibmBW[(size_t)j*16], 64);
            }
            else if (hzHier && pj >= 0) localRelToParentJoint(bindNodes, n, sk.joints[pj], lm);   // LOCAL bind relative to parent JOINT
            else worldM(bindNodes, n, lm);                                                   // root: full world transform
            float q[4], t[3], s[3]; matTrs(lm, q, t, s);
            e.parents[j] = pj;
            if (hzHier && pj > j && std::getenv("HSR_VERBOSE")) fprintf(stderr, "[HZHIER] WARN joint %d parent %d (parent after child — needs reorder)\n", j, pj);
            e.jointPos[j*3]=t[0]; e.jointPos[j*3+1]=t[1]; e.jointPos[j*3+2]=t[2];
            e.jointQuat[j*4]=q[3]; e.jointQuat[j*4+1]=q[0]; e.jointQuat[j*4+2]=q[1]; e.jointQuat[j*4+3]=q[2];   // xyzw -> wxyz
            float us=s[0];   // uniform scale; bind 0 (Shield) -> 1 to avoid singular inverse-bind; the clip still drives the 0->1 pop
            e.jointScale[j] = (us > 1e-3f || us < -1e-3f) ? us : 1.0f;
        }
        // ── IBM RESIDUAL: real glTF skinning is Σw·W_j(t)·IBM_j — the port rebuilds the bind from the
        // NODE hierarchy and ships inverse(bindWorld_j) instead of the file's IBM_j. Equal on well-formed
        // rigs; Maya-exported rigs (japan's kois) bake the UNIT CONVERSION into the IBMs ONLY (joint nodes
        // s=1, mesh verts in giant cm-ish units, IBM carries the shrink) -> the cooked fish skinned HUGE.
        // Fold the residual R = bindWorld_j·IBM_j (a GLOBAL conversion, same for every joint) into the
        // REST VERTS: v' = R·v, so shippedSkin(t)·v' == sourceSkin(t)·v exactly.
        if (sk.ibm.size() >= (size_t)nj*16 && !ibmBind) {   // GLOBAL residual fold — superseded by the per-joint IBM bind above
            float bw[16]; worldM(bindNodes, sk.joints[0], bw);
            float R[16]; mulM(bw, &sk.ibm[0], R);
            float dev=0.f; for (int k=0;k<16;k++){ float id=(k%5==0)?1.f:0.f; float d=std::fabs(R[k]-id); if(d>dev)dev=d; }
            if (dev > 1e-3f) {
                for (size_t v=0; v+2 < e.restPos.size(); v+=3) {
                    float x=e.restPos[v],y=e.restPos[v+1],z=e.restPos[v+2];
                    e.restPos[v]  =R[0]*x+R[4]*y+R[8]*z+R[12];
                    e.restPos[v+1]=R[1]*x+R[5]*y+R[9]*z+R[13];
                    e.restPos[v+2]=R[2]*x+R[6]*y+R[10]*z+R[14];
                }
                if (std::getenv("HSR_VERBOSE"))
                    fprintf(stderr, "[HZIBM] mesh %d: IBM residual folded into rest verts (dev=%.4f scale~%.5f)\n",
                            meshIdx, dev, sqrtf(R[0]*R[0]+R[1]*R[1]+R[2]*R[2]));
            }
        }
        e.boneIdx.resize((size_t)rec->nv*4); e.boneWgt.resize((size_t)rec->nv*4);
        for (u32 v = 0; v < rec->nv; ++v) {
            float wsum=0; for (int c=0;c<4;c++) wsum += rec->jw[v*4+c];
            // SUM-PRESERVING u8 quantization (the zelda flag 0.4%-inflation fix): independent rounding of
            // 0.5/0.5 gives 128+128=256 -> the device's Σ(w/255)·S·v scales the vert by 256/255 = a constant
            // 6cm offset at |v|~11. Largest-remainder rounding keeps the total EXACTLY 255 (Σw = 1).
            float wf[4]; for (int c = 0; c < 4; ++c) wf[c] = wsum > 1e-6f ? rec->jw[v*4+c]/wsum : (c==0?1.f:0.f);
            int iw[4], tot = 0; float fr[4];
            for (int c = 0; c < 4; ++c) { float x = wf[c]*255.f; iw[c] = (int)x; fr[c] = x - (float)iw[c]; tot += iw[c]; }
            for (int rem = 255 - tot; rem > 0; --rem) { int b = 0; for (int c = 1; c < 4; ++c) if (fr[c] > fr[b]) b = c; iw[b]++; fr[b] = -1.f; }
            for (int c = 0; c < 4; ++c) { e.boneIdx[v*4+c] = rec->jidx[v*4+c]; e.boneWgt[v*4+c] = (uint8_t)(iw[c]<0?0:(iw[c]>255?255:iw[c])); }
        }
        e.frameCount = frames; e.fps = srcKeyRate;   // GRID-ALIGNED: frame f sampled at f/keyRate = ON source key f (exact key-lerp)
        e.trsLocal.resize((size_t)frames*nj*10);
        for (int f = 0; f < frames; ++f) {
            float t = (float)f / srcKeyRate;
            std::vector<GNode> cur = gnodes;
            for (auto& ch : gchannels) {
                if (ch.node<0||(size_t)ch.node>=cur.size()||ch.sampler<0||(size_t)ch.sampler>=gsamplers.size()) continue;
                // chTime: each channel wraps at ITS node's own period — raw t CLAMPED short joint channels
                // inside a long clip (bubbles: joints rotate 13.3s then froze for 150s while the preview
                // kept them wrapping = "not even close to the original rotation"). Bake == preview now.
                float val[4]={0,0,0,1}; sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), val);
                if (ch.path==0){cur[ch.node].t[0]=val[0];cur[ch.node].t[1]=val[1];cur[ch.node].t[2]=val[2];}
                else if (ch.path==1){cur[ch.node].r[0]=val[0];cur[ch.node].r[1]=val[1];cur[ch.node].r[2]=val[2];cur[ch.node].r[3]=val[3];}
                else {cur[ch.node].s[0]=val[0];cur[ch.node].s[1]=val[1];cur[ch.node].s[2]=val[2];}
            }
            for (int j = 0; j < nj; ++j) {
                int n = sk.joints[j]; float* o = &e.trsLocal[((size_t)f*nj+j)*10];
                float lm[16];
                if (hzHier && e.parents[j] >= 0) localRelToParentJoint(cur, n, sk.joints[e.parents[j]], lm);  // LOCAL per-frame rel parent joint
                else worldM(cur, n, lm);                                                                      // root: world transform
                float q[4], t[3], s[3]; matTrs(lm, q, t, s);
                o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; o[3]=q[3];             // quat xyzw (ACL)
                o[4]=t[0]; o[5]=t[1]; o[6]=t[2];
                o[7]=s[0]; o[8]=s[1]; o[9]=s[2];
            }
        }
        // ONE-WAY PERIOD FIX (see extractNodeRigidHzAnim): a one-way skin ships without the cooker's seam
        // frame -> device duration (frames-1)/fps = one source frame SHORT of the real period -> the teleport
        // fires 1/fps early every loop and the mesh drifts against its siblings (shark_reef fish: 33ms/loop,
        // the school dephases forever). Append one frame at the TRUE end (clipDur-ε) -> duration = clipDur.
        {
            float drift = 0.f;
            for (int j = 0; j < nj; ++j) { const float* a0 = &e.trsLocal[(size_t)j*10]; const float* aL = &e.trsLocal[((size_t)(frames-1)*nj+j)*10];
                for (int c = 4; c < 7; c++) { float d = a0[c]-aL[c]; if (d<0) d=-d; if (d>drift) drift=d; } }
            if (drift > 5.0f) {
                float t = clipDur - 1e-4f;
                std::vector<GNode> cur = gnodes;
                for (auto& ch : gchannels) {
                    if (ch.node<0||(size_t)ch.node>=cur.size()||ch.sampler<0||(size_t)ch.sampler>=gsamplers.size()) continue;
                    float val[4]={0,0,0,1}; sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), val);
                    if (ch.path==0){cur[ch.node].t[0]=val[0];cur[ch.node].t[1]=val[1];cur[ch.node].t[2]=val[2];}
                    else if (ch.path==1){cur[ch.node].r[0]=val[0];cur[ch.node].r[1]=val[1];cur[ch.node].r[2]=val[2];cur[ch.node].r[3]=val[3];}
                    else {cur[ch.node].s[0]=val[0];cur[ch.node].s[1]=val[1];cur[ch.node].s[2]=val[2];}
                }
                e.trsLocal.resize((size_t)(frames+1)*nj*10);
                for (int j = 0; j < nj; ++j) {
                    int n = sk.joints[j]; float* o = &e.trsLocal[((size_t)frames*nj+j)*10];
                    float lm[16];
                    if (hzHier && e.parents[j] >= 0) localRelToParentJoint(cur, n, sk.joints[e.parents[j]], lm);
                    else worldM(cur, n, lm);
                    float q[4], t2[3], s[3]; matTrs(lm, q, t2, s);
                    const float* pv = &e.trsLocal[((size_t)(frames-1)*nj+j)*10];   // hemisphere continuity vs previous frame
                    if (q[0]*pv[0]+q[1]*pv[1]+q[2]*pv[2]+q[3]*pv[3] < 0.f) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; q[3]=-q[3]; }
                    o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; o[3]=q[3]; o[4]=t2[0]; o[5]=t2[1]; o[6]=t2[2]; o[7]=s[0]; o[8]=s[1]; o[9]=s[2];
                }
                frames += 1; e.frameCount = frames;
            }
        }
        // ── SINGLE-ROOT PORT: the device's SkinnedMesh build requires the skeleton to have EXACTLY ONE root joint
        //    (avatar BodyTrackingHierarchy literally errors "multiple or no root joints ... must have exactly one"; the
        //    env's Clay SkinnedMesh rejects a multi-root forest -> asset null -> invisible droid). The glTF often has
        //    several disconnected root joints (the Incredibles droid: 3-4 — its rig parents them through non-joint nodes,
        //    so they read as roots). Re-parent every EXTRA root under the FIRST root, recomputing its LOCAL transform for
        //    BOTH bind and every clip frame as inverse(root.world)*joint.world -> the WORLD pose stays byte-identical.
        //    Not a rig change: same joints, same poses, same motion, just the single tree the device demands.
        if (hzHier && nj > 1) {
            int rootJ = -1; std::vector<int> orphans;
            for (int j = 0; j < nj; ++j) if (e.parents[j] < 0) { if (rootJ < 0) rootJ = j; else orphans.push_back(j); }
            if (rootJ >= 0 && !orphans.empty()) {
                auto invAffine = [](const float* m, float* o){   // inverse of an affine TRS mat4 (column-major)
                    float a00=m[0],a01=m[4],a02=m[8], a10=m[1],a11=m[5],a12=m[9], a20=m[2],a21=m[6],a22=m[10];
                    float det=a00*(a11*a22-a12*a21)-a01*(a10*a22-a12*a20)+a02*(a10*a21-a11*a20);
                    float id=(det>1e-12f||det<-1e-12f)?1.f/det:0.f;
                    float i00=(a11*a22-a12*a21)*id,i01=(a02*a21-a01*a22)*id,i02=(a01*a12-a02*a11)*id;
                    float i10=(a12*a20-a10*a22)*id,i11=(a00*a22-a02*a20)*id,i12=(a02*a10-a00*a12)*id;
                    float i20=(a10*a21-a11*a20)*id,i21=(a01*a20-a00*a21)*id,i22=(a00*a11-a01*a10)*id;
                    o[0]=i00;o[1]=i10;o[2]=i20;o[3]=0; o[4]=i01;o[5]=i11;o[6]=i21;o[7]=0; o[8]=i02;o[9]=i12;o[10]=i22;o[11]=0;
                    float tx=m[12],ty=m[13],tz=m[14];
                    o[12]=-(i00*tx+i01*ty+i02*tz); o[13]=-(i10*tx+i11*ty+i12*tz); o[14]=-(i20*tx+i21*ty+i22*tz); o[15]=1; };
                auto bindM = [&](int j, float* m){ float q[4]={e.jointQuat[j*4+1],e.jointQuat[j*4+2],e.jointQuat[j*4+3],e.jointQuat[j*4]}; float s3[3]={e.jointScale[j],e.jointScale[j],e.jointScale[j]}; trsM(&e.jointPos[j*3], q, s3, m); };
                float Rw[16], Rwi[16]; bindM(rootJ, Rw); invAffine(Rw, Rwi);
                for (int j : orphans) {   // BIND: re-express orphan relative to the single root
                    float Jw[16]; bindM(j, Jw); float L[16]; mulM(Rwi, Jw, L);
                    float q[4], t[3], s[3]; matTrs(L, q, t, s);
                    e.jointPos[j*3]=t[0]; e.jointPos[j*3+1]=t[1]; e.jointPos[j*3+2]=t[2];
                    e.jointQuat[j*4]=q[3]; e.jointQuat[j*4+1]=q[0]; e.jointQuat[j*4+2]=q[1]; e.jointQuat[j*4+3]=q[2];   // xyzw->wxyz
                    float us=s[0]; e.jointScale[j]=(us>1e-3f||us<-1e-3f)?us:1.0f;
                }
                for (int f = 0; f < frames; ++f) {   // CLIP: same re-parent per frame (trsLocal quat = xyzw)
                    auto clipM = [&](int j, float* m){ float* o=&e.trsLocal[((size_t)f*nj+j)*10]; float q[4]={o[0],o[1],o[2],o[3]}; float s3[3]={o[7],o[8],o[9]}; float tt[3]={o[4],o[5],o[6]}; trsM(tt, q, s3, m); };
                    float Rwf[16], Rwfi[16]; clipM(rootJ, Rwf); invAffine(Rwf, Rwfi);
                    for (int j : orphans) {
                        float Jwf[16]; clipM(j, Jwf); float L[16]; mulM(Rwfi, Jwf, L);
                        float q[4], t[3], s[3]; matTrs(L, q, t, s);
                        float* o=&e.trsLocal[((size_t)f*nj+j)*10];
                        o[0]=q[0]; o[1]=q[1]; o[2]=q[2]; o[3]=q[3]; o[4]=t[0]; o[5]=t[1]; o[6]=t[2]; o[7]=s[0]; o[8]=s[1]; o[9]=s[2];
                    }
                }
                for (int j : orphans) e.parents[j] = rootJ;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr, "[HZ1ROOT] mesh%d: reparented %d orphan root(s) under joint %d -> single-root tree\n", meshIdx, (int)orphans.size(), rootJ);
            }
        }
        if (std::getenv("HSR_HZDUMP")) for (int f=0; f<frames; f += (frames>10?frames/10:1)) {
            const float* o=&e.trsLocal[((size_t)f*nj+(nj-1))*10];
            fprintf(stderr,"[HZDUMP] f%d shieldJ%d worldT=(%.2f,%.2f,%.2f) S=(%.3f,%.3f,%.3f)\n", f, nj-1, o[4],o[5],o[6], o[7],o[8],o[9]);
        }
        // NO CENTERING by default. The verifier reject was NEVER the far-from-origin bounds — it was the ROOT.f0
        // marker type (now fixed). restPos + the skeleton are already in the droid's WORLD space (exactly what the
        // renderer skins correctly); the device drives the skinned mesh from the skeleton and ignores the entity
        // transform, so centering to origin + re-placing via the entity transform DROPS the placement -> droid
        // off-screen. Feed the renderer's exact uncentered rig + identity entity = droid at its world position.
        // (HSR_HZCENTER re-enables the old centering for comparison.)
        if (std::getenv("HSR_HZCENTER")) {
            float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
            for (size_t v=0; v+2<e.restPos.size(); v+=3) for(int k=0;k<3;k++){ float p=e.restPos[v+k]; if(p<mn[k])mn[k]=p; if(p>mx[k])mx[k]=p; }
            float cen[3]; for(int k=0;k<3;k++) cen[k]=0.5f*(mn[k]+mx[k]);
            for (size_t v=0; v+2<e.restPos.size(); v+=3){ e.restPos[v]-=cen[0]; e.restPos[v+1]-=cen[1]; e.restPos[v+2]-=cen[2]; }
            for (int j=0;j<nj;++j) if (e.parents[j]<0){ e.jointPos[j*3]-=cen[0]; e.jointPos[j*3+1]-=cen[1]; e.jointPos[j*3+2]-=cen[2]; }
            for (int f=0;f<frames;++f) for (int j=0;j<nj;++j) if (e.parents[j]<0){ float* o=&e.trsLocal[((size_t)f*nj+j)*10]; o[4]-=cen[0]; o[5]-=cen[1]; o[6]-=cen[2]; }
        }
        // ── BUTTERFLY-PARITY (default ON): model-local verts + IDENTITY root bind ───────────────
        //  DEVICE BUG (confirmed via the render-trace MCP): animated meshes disappear because the
        //  device computes the skinned mesh's CULL bounds from per-joint vertex extents × the
        //  ANIMATED joint world matrices. With restPos in WORLD space and the ROOT joint bind = the
        //  full world transform (the worldM root above + the world-space rest pose), those per-joint
        //  boxes land ~at the entity world position AND swing with the animation → the cull AABB is
        //  both MISPLACED (~25m off) and OSCILLATING (±several m/frame) → it leaves the stereo frustum
        //  → FRUSTUM-CULL → flicker/vanish. Official meshes (e.g. nuxd, the vista butterfly) never do
        //  this: their verts are MODEL-LOCAL with an IDENTITY root bind and the world placement is
        //  carried entirely by the animation clip. So normalise to that exact shape: transform restPos
        //  by inverse(root world-bind) → model-local (everything relative to the single root), and zero
        //  the root joint's bind TRS to identity. Children already store parent-JOINT-relative local
        //  binds, and trsLocal is untouched (the root still streams its world transform per frame), so
        //  the rendered pose is byte-identical — only the cull bounds become model-local-tight and
        //  correctly placed. Same fix as opa_loader::extractHzAnim. HSR_HZWORLDROOT reverts for A/B.
        if (!std::getenv("HSR_HZWORLDROOT") && !std::getenv("HSR_HZCENTER")) {
            int rj = -1; for (int j = 0; j < nj; ++j) if (e.parents[j] < 0) { rj = j; break; }
            if (rj >= 0) {
                auto invAff = [](const float* m, float* o){
                    float a00=m[0],a01=m[4],a02=m[8], a10=m[1],a11=m[5],a12=m[9], a20=m[2],a21=m[6],a22=m[10];
                    float det=a00*(a11*a22-a12*a21)-a01*(a10*a22-a12*a20)+a02*(a10*a21-a11*a20);
                    float id=(det>1e-12f||det<-1e-12f)?1.f/det:0.f;
                    float i00=(a11*a22-a12*a21)*id,i01=(a02*a21-a01*a22)*id,i02=(a01*a12-a02*a11)*id;
                    float i10=(a12*a20-a10*a22)*id,i11=(a00*a22-a02*a20)*id,i12=(a02*a10-a00*a12)*id;
                    float i20=(a10*a21-a11*a20)*id,i21=(a01*a20-a00*a21)*id,i22=(a00*a11-a01*a10)*id;
                    o[0]=i00;o[1]=i10;o[2]=i20;o[3]=0; o[4]=i01;o[5]=i11;o[6]=i21;o[7]=0; o[8]=i02;o[9]=i12;o[10]=i22;o[11]=0;
                    float tx=m[12],ty=m[13],tz=m[14];
                    o[12]=-(i00*tx+i01*ty+i02*tz); o[13]=-(i10*tx+i11*ty+i12*tz); o[14]=-(i20*tx+i21*ty+i22*tz); o[15]=1; };
                float q[4]={e.jointQuat[rj*4+1],e.jointQuat[rj*4+2],e.jointQuat[rj*4+3],e.jointQuat[rj*4]}; // wxyz->xyzw
                float s3[3]={e.jointScale[rj],e.jointScale[rj],e.jointScale[rj]};
                float Rw[16]; trsM(&e.jointPos[rj*3], q, s3, Rw);          // root world bind (rootless: local==world)
                float Rwi[16]; invAff(Rw, Rwi);
                for (size_t v=0; v+2<e.restPos.size(); v+=3){              // world rest -> model-local (root frame)
                    float x=e.restPos[v],y=e.restPos[v+1],z=e.restPos[v+2];
                    e.restPos[v]  =Rwi[0]*x+Rwi[4]*y+Rwi[8]*z +Rwi[12];
                    e.restPos[v+1]=Rwi[1]*x+Rwi[5]*y+Rwi[9]*z +Rwi[13];
                    e.restPos[v+2]=Rwi[2]*x+Rwi[6]*y+Rwi[10]*z+Rwi[14]; }
                e.jointPos[rj*3]=0; e.jointPos[rj*3+1]=0; e.jointPos[rj*3+2]=0;   // root bind = identity
                e.jointQuat[rj*4]=1; e.jointQuat[rj*4+1]=0; e.jointQuat[rj*4+2]=0; e.jointQuat[rj*4+3]=0;
                e.jointScale[rj]=1.f;
                if (std::getenv("HSR_VERBOSE")) fprintf(stderr,"[HZMODELLOCAL] mesh%d root j%d: verts->model-local + identity root bind (cull-bounds fix)\n",meshIdx,rj);
            }
        }
        // ── SELF-VALIDATION + FLAT-EXACT fallback (ANIM-VERIFY 2026-07-08): emulate the DEVICE's decode of the
        //    final shipped tables (uniform-scale TRS binds composed+inverted, clip locals composed, weighted skin)
        //    against the SOURCE skinning S_j(t)=W_j(t)·IBM_j at 4 bake frames. The hierarchical scheme is EXACT for
        //    well-formed rigs, but an IBM with NON-UNIFORM scale or SHEAR (steam_void 'Bake': node-bind deviation
        //    17.5 — Maya bakes sim transforms into IBMs) cannot round-trip through the pos/quat/uniform-scale bind
        //    -> verts orbit off the source. When validation fails, REBUILD FLAT-EXACT: one synthetic identity root,
        //    every source joint a child whose per-frame local IS its full skinning matrix S_j(t) (bind=identity ->
        //    device plays S_j verbatim; model-local rest verts). HSR_NOFLATEXACT reverts.
        if (!std::getenv("HSR_NOFLATEXACT") && frames >= 2) {
            auto invAffV = [](const float* m, float* o){
                float a00=m[0],a01=m[4],a02=m[8], a10=m[1],a11=m[5],a12=m[9], a20=m[2],a21=m[6],a22=m[10];
                float det=a00*(a11*a22-a12*a21)-a01*(a10*a22-a12*a20)+a02*(a10*a21-a11*a20);
                float id=(det>1e-12f||det<-1e-12f)?1.f/det:0.f;
                float i00=(a11*a22-a12*a21)*id,i01=(a02*a21-a01*a22)*id,i02=(a01*a12-a02*a11)*id;
                float i10=(a12*a20-a10*a22)*id,i11=(a00*a22-a02*a20)*id,i12=(a02*a10-a00*a12)*id;
                float i20=(a10*a21-a11*a20)*id,i21=(a01*a20-a00*a21)*id,i22=(a00*a11-a01*a10)*id;
                o[0]=i00;o[1]=i10;o[2]=i20;o[3]=0; o[4]=i01;o[5]=i11;o[6]=i21;o[7]=0; o[8]=i02;o[9]=i12;o[10]=i22;o[11]=0;
                float tx=m[12],ty=m[13],tz=m[14];
                o[12]=-(i00*tx+i01*ty+i02*tz); o[13]=-(i10*tx+i11*ty+i12*tz); o[14]=-(i20*tx+i21*ty+i22*tz); o[15]=1; };
            u32 nvv = rec->nv; u32 probe[8]; for (int k=0;k<8;k++) probe[k]=(u32)((size_t)(nvv-1)*k/7);
            // make sure a ZERO-WEIGHT vert (if any) is probed: the source pins those at basePos (animate's
            // wsum guard) while the shipped tables force-bind them to joint 0 — invisible to spread probes
            // when rare (steam_void 'Bake': the whale's zero-weight rim verts orbited with joint 0).
            for (u32 v = 0; v < nvv; ++v) {
                float ws = rec->jw[(size_t)v*4]+rec->jw[(size_t)v*4+1]+rec->jw[(size_t)v*4+2]+rec->jw[(size_t)v*4+3];
                if (ws <= 1e-5f) { probe[7] = v; break; }   // same guard as animate()'s wsum>1e-5
            }
            // measure(): device-decode the CURRENT e.* tables at 4 bake frames and diff vs source skinning.
            float spanE = 0.f;
            auto measure = [&]() -> float {
                int J = e.jointCount;
                std::vector<float> sbw2((size_t)J*16), sinv2((size_t)J*16);
                for (int j = 0; j < J; ++j) {
                    float q[4]={e.jointQuat[j*4+1],e.jointQuat[j*4+2],e.jointQuat[j*4+3],e.jointQuat[j*4]};
                    float s3[3]={e.jointScale[j],e.jointScale[j],e.jointScale[j]};
                    float L[16]; trsM(&e.jointPos[j*3], q, s3, L);
                    if (e.parents[j] >= 0) mulM(&sbw2[(size_t)e.parents[j]*16], L, &sbw2[(size_t)j*16]);
                    else memcpy(&sbw2[(size_t)j*16], L, 64);
                    invAffV(&sbw2[(size_t)j*16], &sinv2[(size_t)j*16]);
                }
                float maxE = 0.f, tp0[8*3]; spanE = 0.f;
                int checkF[9] = {0, frames/8, frames/4, (3*frames)/8, frames/2, (5*frames)/8, (3*frames)/4, (7*frames)/8, frames-1};   // 9 frames (4 missed the horror ghost's mid-pose bind error)
                for (int ci = 0; ci < 9; ++ci) {
                    int f = checkF[ci]; float t = std::min((float)f / srcKeyRate, clipDur - 1e-4f);   // appended one-way end frame sits AT clipDur
                    std::vector<float> cw((size_t)J*16), skn((size_t)J*16);
                    for (int j = 0; j < J; ++j) {
                        float* o = &e.trsLocal[((size_t)f*J+j)*10];
                        float q[4]={o[0],o[1],o[2],o[3]}, s3[3]={o[7],o[8],o[9]};
                        float L[16]; trsM(&o[4], q, s3, L);
                        if (e.parents[j] >= 0) mulM(&cw[(size_t)e.parents[j]*16], L, &cw[(size_t)j*16]);
                        else memcpy(&cw[(size_t)j*16], L, 64);
                        mulM(&cw[(size_t)j*16], &sinv2[(size_t)j*16], &skn[(size_t)j*16]);
                    }
                    std::vector<GNode> cur = gnodes;
                    for (auto& ch : gchannels) {
                        if (ch.node<0||(size_t)ch.node>=cur.size()||ch.sampler<0||(size_t)ch.sampler>=gsamplers.size()) continue;
                        float val[4]={0,0,0,1}; sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), val);
                        if (ch.path==0){cur[ch.node].t[0]=val[0];cur[ch.node].t[1]=val[1];cur[ch.node].t[2]=val[2];}
                        else if (ch.path==1){cur[ch.node].r[0]=val[0];cur[ch.node].r[1]=val[1];cur[ch.node].r[2]=val[2];cur[ch.node].r[3]=val[3];}
                        else {cur[ch.node].s[0]=val[0];cur[ch.node].s[1]=val[1];cur[ch.node].s[2]=val[2];}
                    }
                    for (int k = 0; k < 8; ++k) {
                        u32 v = probe[k];
                        float sp[3]={0,0,0};   // shipped: e tables (boneWgt pre-normalized u8)
                        const float* rp = &e.restPos[(size_t)v*3];
                        for (int c = 0; c < 4; ++c) { float w = e.boneWgt[(size_t)v*4+c]/255.f; if (w<=0.f) continue;
                            int j = e.boneIdx[(size_t)v*4+c]; if (j>=J) j=0; const float* S=&skn[(size_t)j*16];
                            sp[0]+=w*(S[0]*rp[0]+S[4]*rp[1]+S[8]*rp[2]+S[12]);
                            sp[1]+=w*(S[1]*rp[0]+S[5]*rp[1]+S[9]*rp[2]+S[13]);
                            sp[2]+=w*(S[2]*rp[0]+S[6]*rp[1]+S[10]*rp[2]+S[14]); }
                        float tp[3]={0,0,0}, wsum=0.f;   // truth: W_j(t)·IBM_j on basePos, weight-normalized
                        const float* bp = &rec->basePos[(size_t)v*3];
                        for (int c = 0; c < 4; ++c) { float w = rec->jw[(size_t)v*4+c]; if (w<=0.f) continue;
                            int j = rec->jidx[(size_t)v*4+c]; if (j>=nj) continue;
                            float Wn[16]; worldM(cur, sk.joints[j], Wn);
                            float S[16];
                            if (sk.ibm.size() >= (size_t)(j+1)*16) mulM(Wn, &sk.ibm[(size_t)j*16], S); else memcpy(S, Wn, 64);
                            tp[0]+=w*(S[0]*bp[0]+S[4]*bp[1]+S[8]*bp[2]+S[12]);
                            tp[1]+=w*(S[1]*bp[0]+S[5]*bp[1]+S[9]*bp[2]+S[13]);
                            tp[2]+=w*(S[2]*bp[0]+S[6]*bp[1]+S[10]*bp[2]+S[14]); wsum+=w; }
                        if (wsum>1e-5f){ tp[0]/=wsum; tp[1]/=wsum; tp[2]/=wsum; } else { tp[0]=bp[0]; tp[1]=bp[1]; tp[2]=bp[2]; }
                        if (ci==0){ tp0[k*3]=tp[0]; tp0[k*3+1]=tp[1]; tp0[k*3+2]=tp[2]; }
                        else { float sx=tp[0]-tp0[k*3],sy=tp[1]-tp0[k*3+1],sz=tp[2]-tp0[k*3+2];
                               float s2=sqrtf(sx*sx+sy*sy+sz*sz); if (s2>spanE) spanE=s2; }
                        float dx=sp[0]-tp[0],dy=sp[1]-tp[1],dz=sp[2]-tp[2];
                        float e2=sqrtf(dx*dx+dy*dy+dz*dz); if (e2>maxE) maxE=e2;
                    }
                }
                return maxE;
            };
            float maxE = measure();
            if (maxE > std::max(0.02f, 0.02f*spanE) && nj + 1 <= 255) {   // u8 bone indices bound the flat scheme
                fprintf(stderr, "[HZFLAT] mesh%d hierarchical tables DEVIATE from source skinning (maxErr=%.3f span=%.2f) -> FLAT-EXACT rebuild (identity binds, clip = full S_j(t))\n", meshIdx, maxE, spanE);
                int nj2 = nj + 1;
                e.jointCount = nj2;
                e.parents.assign((size_t)nj2, 0); e.parents[0] = -1;
                e.jointPos.assign((size_t)nj2*3, 0.f);
                e.jointQuat.assign((size_t)nj2*4, 0.f); for (int j = 0; j < nj2; ++j) e.jointQuat[(size_t)j*4] = 1.f;   // wxyz identity
                e.jointScale.assign((size_t)nj2, 1.f);
                e.restPos = rec->basePos;   // raw model space (no folds; S_j carries everything)
                for (u32 v = 0; v < nvv; ++v) {
                    float ws = rec->jw[(size_t)v*4]+rec->jw[(size_t)v*4+1]+rec->jw[(size_t)v*4+2]+rec->jw[(size_t)v*4+3];
                    if (ws <= 1e-5f) {   // ZERO/NEAR-ZERO-WEIGHT vert (same 1e-5 guard as animate): pinned at basePos — bind it to the
                        for (int c = 0; c < 4; ++c) { e.boneIdx[(size_t)v*4+c] = 0; e.boneWgt[(size_t)v*4+c] = (uint8_t)(c==0?255:0); }   // synthetic STATIC identity root (exactly basePos, matches animate's wsum guard)
                        continue;
                    }
                    int iw[4], tot = 0; float fr[4];   // sum-preserving u8 (Σ = 255 exactly; see the bind builder)
                    for (int c = 0; c < 4; ++c) { float x = rec->jw[(size_t)v*4+c]/ws*255.f; iw[c] = (int)x; fr[c] = x - (float)iw[c]; tot += iw[c]; }
                    for (int rem = 255 - tot; rem > 0; --rem) { int b = 0; for (int c = 1; c < 4; ++c) if (fr[c] > fr[b]) b = c; iw[b]++; fr[b] = -1.f; }
                    for (int c = 0; c < 4; ++c) {
                        int j = rec->jidx[(size_t)v*4+c]; e.boneIdx[(size_t)v*4+c] = (uint8_t)((j+1 < nj2) ? j+1 : 0);
                        e.boneWgt[(size_t)v*4+c] = (uint8_t)(iw[c]<0?0:(iw[c]>255?255:iw[c]));
                    }
                }
                e.trsLocal.assign((size_t)frames*nj2*10, 0.f);
                float decompE = 0.f;
                for (int f = 0; f < frames; ++f) {
                    float t = std::min((float)f / srcKeyRate, clipDur - 1e-4f);   // appended one-way end frame sits AT clipDur
                    std::vector<GNode> cur = gnodes;
                    for (auto& ch : gchannels) {
                        if (ch.node<0||(size_t)ch.node>=cur.size()||ch.sampler<0||(size_t)ch.sampler>=gsamplers.size()) continue;
                        float val[4]={0,0,0,1}; sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), val);
                        if (ch.path==0){cur[ch.node].t[0]=val[0];cur[ch.node].t[1]=val[1];cur[ch.node].t[2]=val[2];}
                        else if (ch.path==1){cur[ch.node].r[0]=val[0];cur[ch.node].r[1]=val[1];cur[ch.node].r[2]=val[2];cur[ch.node].r[3]=val[3];}
                        else {cur[ch.node].s[0]=val[0];cur[ch.node].s[1]=val[1];cur[ch.node].s[2]=val[2];}
                    }
                    { float* o = &e.trsLocal[((size_t)f*nj2)*10]; o[3]=1.f; o[7]=o[8]=o[9]=1.f; }   // joint0 identity
                    for (int j = 0; j < nj; ++j) {
                        float Wn[16]; worldM(cur, sk.joints[j], Wn);
                        float S[16];
                        if (sk.ibm.size() >= (size_t)(j+1)*16) mulM(Wn, &sk.ibm[(size_t)j*16], S); else memcpy(S, Wn, 64);
                        float q[4], tt[3], s3[3]; matTrs(S, q, tt, s3);
                        float* o = &e.trsLocal[((size_t)f*nj2 + j + 1)*10];
                        o[0]=q[0];o[1]=q[1];o[2]=q[2];o[3]=q[3]; o[4]=tt[0];o[5]=tt[1];o[6]=tt[2]; o[7]=s3[0];o[8]=s3[1];o[9]=s3[2];
                        float x=q[0],y=q[1],z=q[2],w=q[3], rc[9]={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y), 2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x), 2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y)};
                        for (int c=0;c<3;c++) for (int rr=0;rr<3;rr++){ float dv=std::fabs(rc[c*3+rr]*s3[c]-S[c*4+rr]); if (dv>decompE) decompE=dv; }
                    }
                    // hemisphere continuity per joint (same rule as everywhere)
                    if (f > 0) for (int j = 1; j < nj2; ++j) {
                        float* q = &e.trsLocal[((size_t)f*nj2 + j)*10]; const float* p = &e.trsLocal[((size_t)(f-1)*nj2 + j)*10];
                        if (q[0]*p[0]+q[1]*p[1]+q[2]*p[2]+q[3]*p[3] < 0.f) { q[0]=-q[0]; q[1]=-q[1]; q[2]=-q[2]; q[3]=-q[3]; }
                    }
                }
                if (decompE > 1e-2f)
                    fprintf(stderr, "[HZFLAT] mesh%d WARN S_j(t) not clean TRS (shear residual %.4f) — best-possible TRS approximation shipped\n", meshIdx, decompE);
                float postE = measure();
                fprintf(stderr, "[HZFLAT] mesh%d post-rebuild residual maxErr=%.4f (span=%.2f)\n", meshIdx, postE, spanE);
                if (std::getenv("HSR_FLATDBG")) {   // joint-0 forensics: shipped clip local vs direct S at one frame
                    int f = frames/3; float t = (float)f/srcKeyRate;
                    std::vector<GNode> cur = gnodes;
                    for (auto& ch : gchannels) {
                        if (ch.node<0||(size_t)ch.node>=cur.size()||ch.sampler<0||(size_t)ch.sampler>=gsamplers.size()) continue;
                        float val[4]={0,0,0,1}; sampleSampler(gsamplers[ch.sampler], chTime(ch.node, t), val);
                        if (ch.path==0){cur[ch.node].t[0]=val[0];cur[ch.node].t[1]=val[1];cur[ch.node].t[2]=val[2];}
                        else if (ch.path==1){cur[ch.node].r[0]=val[0];cur[ch.node].r[1]=val[1];cur[ch.node].r[2]=val[2];cur[ch.node].r[3]=val[3];}
                        else {cur[ch.node].s[0]=val[0];cur[ch.node].s[1]=val[1];cur[ch.node].s[2]=val[2];}
                    }
                    float Wn[16]; worldM(cur, sk.joints[0], Wn);
                    float S[16]; if (sk.ibm.size()>=16) mulM(Wn, &sk.ibm[0], S); else memcpy(S, Wn, 64);
                    const float* o = &e.trsLocal[((size_t)f*nj2 + 1)*10];
                    fprintf(stderr, "[FLATDBG] f=%d t=%.2f joint0: Wn.t=(%.2f,%.2f,%.2f) S.t=(%.2f,%.2f,%.2f) S.c0=(%.3f,%.3f,%.3f) clip.t=(%.2f,%.2f,%.2f) clip.q=(%.3f,%.3f,%.3f,%.3f) clip.s=(%.3f,%.3f,%.3f) ibm.t=(%.2f,%.2f,%.2f)\n",
                            f, t, Wn[12],Wn[13],Wn[14], S[12],S[13],S[14], S[0],S[1],S[2], o[4],o[5],o[6], o[0],o[1],o[2],o[3], o[7],o[8],o[9],
                            sk.ibm.size()>=16?sk.ibm[12]:0.f, sk.ibm.size()>=16?sk.ibm[13]:0.f, sk.ibm.size()>=16?sk.ibm[14]:0.f);
                }
            } else if (std::getenv("HSR_VERBOSE") && maxE > 0.001f)
                fprintf(stderr, "[HZCHECK] mesh%d hierarchical tables OK (maxErr=%.4f span=%.2f)\n", meshIdx, maxE, spanE);
        }
        // (No geometry perturbation: the verifier reject was the ROOT.f0 marker encoding — an empty vector where the
        // device wants a u16 scalar — NOT the geometry. Positions are free; use the exact centered rest pose.)
        if (std::getenv("HSR_HZDBG")) {
            fprintf(stderr,"[HZDBG] mesh%d skin%d nj=%d frames=%d gchannels=%d animDur=%.2f\n",meshIdx,rec->skin,nj,frames,(int)gchannels.size(),animDuration);
            for (int j=0;j<nj;++j){ float mn[10],mx[10]; for(int k=0;k<10;k++){mn[k]=1e9f;mx[k]=-1e9f;}
                for(int f=0;f<frames;++f){float*o=&e.trsLocal[((size_t)f*nj+j)*10];for(int k=0;k<10;k++){if(o[k]<mn[k])mn[k]=o[k];if(o[k]>mx[k])mx[k]=o[k];}}
                fprintf(stderr,"   j%d node%d dRot=%.3f dTrans=%.3f scale[%.2f..%.2f]\n",j,sk.joints[j],
                    (mx[0]-mn[0])+(mx[1]-mn[1])+(mx[2]-mn[2])+(mx[3]-mn[3]),(mx[4]-mn[4])+(mx[5]-mn[5])+(mx[6]-mn[6]),mn[7],mx[7]); }
            int nc=0; for(auto&ch:gchannels) if(nodeToJoint[ch.node<0?0:(ch.node>=(int)nodeToJoint.size()?0:ch.node)]>=0) nc++;
            fprintf(stderr,"   channels targeting skin joints: %d / %d\n",nc,(int)gchannels.size());
        }
        return e;
    }
};
