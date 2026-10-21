#pragma once
// v203 HzAnim skeletal animation — parse the env's custom "Skel" skeleton + HZANIM clip and
// produce per-joint skinning matrices for CPU skinning (reused by the renderer's dynamicVerts path).
// Formats reversed from nuxd's __hzanim_skel_sub_targets__/skeleton_0 + __hzanim_anim_sub_targets__/take_001.
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include "core/types.h"
#include "cook/hzanim_acl.h"   // ACL bridge (the real libshell decode; ACL types stay in hzanim_acl.cpp)

struct RendJoint { float pos[3]; float quat[4]; float scale; int parent; std::string name; };
struct RendSkel {
    std::vector<RendJoint> joints;
    std::vector<float> bind;     // 16*nJoints column-major bind (world) matrix
    std::vector<float> invBind;  // 16*nJoints inverse-bind
    bool ok() const { return !joints.empty(); }
};
struct RendClip {
    int nJoints = 0, nFrames = 0; float fps = 24.0f;
    // per (frame*nJoints) a quaternion (w,x,y,z) + a translation (x,y,z) [translation optional / from bind].
    std::vector<float> quats;   // nFrames*nJoints*4
    std::vector<float> trans;   // nFrames*nJoints*3 (may be empty -> use bind pos)
    HzAclClip* acl = nullptr;   // V203: the real ACL clip (preferred over the legacy quats path)
    int aclJoints = 0;
    bool ok() const { return acl != nullptr || (nFrames > 0 && nJoints > 0 && !quats.empty()); }
};

// quat (w,x,y,z) -> column-major 4x4 with translation t and uniform scale s.
inline void rs_trs(const float q[4], const float t[3], float s, float* m) {
    float w=q[0],x=q[1],y=q[2],z=q[3];
    float n = w*w+x*x+y*y+z*z; if (n>1e-12f){ float r=1.0f/std::sqrt(n); w*=r;x*=r;y*=r;z*=r; }
    float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    m[0]=(1-2*(yy+zz))*s; m[1]=(2*(xy+wz))*s;  m[2]=(2*(xz-wy))*s;  m[3]=0;
    m[4]=(2*(xy-wz))*s;   m[5]=(1-2*(xx+zz))*s;m[6]=(2*(yz+wx))*s;  m[7]=0;
    m[8]=(2*(xz+wy))*s;   m[9]=(2*(yz-wx))*s;  m[10]=(1-2*(xx+yy))*s;m[11]=0;
    m[12]=t[0]; m[13]=t[1]; m[14]=t[2]; m[15]=1;
}
inline void rs_mul(const float* a, const float* b, float* o) { // o = a*b (column-major)
    for (int c=0;c<4;c++) for (int r=0;r<4;r++) {
        float s=0; for (int k=0;k<4;k++) s+=a[k*4+r]*b[c*4+k]; o[c*4+r]=s; }
}
inline bool rs_invert(const float* m, float* inv) {
    float a[16]; memcpy(a,m,64);
    inv[0]=a[5]*a[10]*a[15]-a[5]*a[11]*a[14]-a[9]*a[6]*a[15]+a[9]*a[7]*a[14]+a[13]*a[6]*a[11]-a[13]*a[7]*a[10];
    inv[4]=-a[4]*a[10]*a[15]+a[4]*a[11]*a[14]+a[8]*a[6]*a[15]-a[8]*a[7]*a[14]-a[12]*a[6]*a[11]+a[12]*a[7]*a[10];
    inv[8]=a[4]*a[9]*a[15]-a[4]*a[11]*a[13]-a[8]*a[5]*a[15]+a[8]*a[7]*a[13]+a[12]*a[5]*a[11]-a[12]*a[7]*a[9];
    inv[12]=-a[4]*a[9]*a[14]+a[4]*a[10]*a[13]+a[8]*a[5]*a[14]-a[8]*a[6]*a[13]-a[12]*a[5]*a[10]+a[12]*a[6]*a[9];
    inv[1]=-a[1]*a[10]*a[15]+a[1]*a[11]*a[14]+a[9]*a[2]*a[15]-a[9]*a[3]*a[14]-a[13]*a[2]*a[11]+a[13]*a[3]*a[10];
    inv[5]=a[0]*a[10]*a[15]-a[0]*a[11]*a[14]-a[8]*a[2]*a[15]+a[8]*a[3]*a[14]+a[12]*a[2]*a[11]-a[12]*a[3]*a[10];
    inv[9]=-a[0]*a[9]*a[15]+a[0]*a[11]*a[13]+a[8]*a[1]*a[15]-a[8]*a[3]*a[13]-a[12]*a[1]*a[11]+a[12]*a[3]*a[9];
    inv[13]=a[0]*a[9]*a[14]-a[0]*a[10]*a[13]-a[8]*a[1]*a[14]+a[8]*a[2]*a[13]+a[12]*a[1]*a[10]-a[12]*a[2]*a[9];
    inv[2]=a[1]*a[6]*a[15]-a[1]*a[7]*a[14]-a[5]*a[2]*a[15]+a[5]*a[3]*a[14]+a[13]*a[2]*a[7]-a[13]*a[3]*a[6];
    inv[6]=-a[0]*a[6]*a[15]+a[0]*a[7]*a[14]+a[4]*a[2]*a[15]-a[4]*a[3]*a[14]-a[12]*a[2]*a[7]+a[12]*a[3]*a[6];
    inv[10]=a[0]*a[5]*a[15]-a[0]*a[7]*a[13]-a[4]*a[1]*a[15]+a[4]*a[3]*a[13]+a[12]*a[1]*a[7]-a[12]*a[3]*a[5];
    inv[14]=-a[0]*a[5]*a[14]+a[0]*a[6]*a[13]+a[4]*a[1]*a[14]-a[4]*a[2]*a[13]-a[12]*a[1]*a[6]+a[12]*a[2]*a[5];
    inv[3]=-a[1]*a[6]*a[11]+a[1]*a[7]*a[10]+a[5]*a[2]*a[11]-a[5]*a[3]*a[10]-a[9]*a[2]*a[7]+a[9]*a[3]*a[6];
    inv[7]=a[0]*a[6]*a[11]-a[0]*a[7]*a[10]-a[4]*a[2]*a[11]+a[4]*a[3]*a[10]+a[8]*a[2]*a[7]-a[8]*a[3]*a[6];
    inv[11]=-a[0]*a[5]*a[11]+a[0]*a[7]*a[9]+a[4]*a[1]*a[11]-a[4]*a[3]*a[9]-a[8]*a[1]*a[7]+a[8]*a[3]*a[5];
    inv[15]=a[0]*a[5]*a[10]-a[0]*a[6]*a[9]-a[4]*a[1]*a[10]+a[4]*a[2]*a[9]+a[8]*a[1]*a[6]-a[8]*a[2]*a[5];
    float det=a[0]*inv[0]+a[1]*inv[4]+a[2]*inv[8]+a[3]*inv[12];
    if (std::fabs(det)<1e-12f) return false; det=1.0f/det;
    for(int i=0;i<16;i++) inv[i]*=det; return true;
}

// Compose each joint's world bind matrix (chaining parents, which precede children) + its inverse-bind.
inline void rs_buildBind(RendSkel& s) {
    int n = (int)s.joints.size();
    s.bind.resize(16*n); s.invBind.resize(16*n);
    for (int i=0;i<n;i++) {
        float local[16]; rs_trs(s.joints[i].quat, s.joints[i].pos, s.joints[i].scale, local);
        float world[16]; memcpy(world, local, 64);
        if (s.joints[i].parent >= 0 && s.joints[i].parent < n)
            rs_mul(&s.bind[16*s.joints[i].parent], local, world);
        memcpy(&s.bind[16*i], world, 64);
        if (!rs_invert(world, &s.invBind[16*i])) { memset(&s.invBind[16*i],0,64); s.invBind[16*i]=s.invBind[16*i+5]=s.invBind[16*i+10]=s.invBind[16*i+15]=1; }
    }
}

// V203 vista skeleton ("lekS"): magic(4) ver u32 nameLen u32 name | u16 jointCount | per joint:
//   i16 parent, u32 nameLen, name, T f32x3, Q(w,x,y,z) f32x4, S f32x1. HIERARCHICAL (parents precede children),
//   e.g. whale01: global_jnt(-1)->root->chest->{head, l/r_shoulder->elbow->wrist} + tailStart->tail1->tail2->tailEnd.
inline bool parseRendSkelV203(const std::vector<u8>& d, RendSkel& s) {
    auto rd32=[&](size_t o)->u32{ return o+4<=d.size()? *(const u32*)(d.data()+o):0u; };
    auto rd16=[&](size_t o)->u16{ return o+2<=d.size()? *(const u16*)(d.data()+o):(u16)0; };
    auto rdf =[&](size_t o){ float f=0; if(o+4<=d.size()) memcpy(&f,d.data()+o,4); return f; };
    size_t o = 8;                              // after magic + version
    u32 nameLen = rd32(o); o += 4 + nameLen;   // skeleton name ("Skeleton_0")
    if (o + 2 > d.size()) return false;
    u32 jc = rd16(o); o += 2;                  // u16 joint count
    if (jc == 0 || jc > 1024) return false;
    std::vector<RendJoint> js;
    for (u32 i = 0; i < jc; ++i) {
        if (o + 6 > d.size()) return false;
        int parent = (short)rd16(o);
        u32 nl = rd32(o + 2);
        if (nl == 0 || nl > 128 || o + 6 + nl + 32 > d.size()) return false;
        RendJoint j;
        j.name.assign((const char*)d.data() + o + 6, nl);
        for (char ch : j.name) if (ch < 32 || ch > 126) return false;   // sanity: printable joint names
        size_t p = o + 6 + nl;
        j.pos[0]=rdf(p); j.pos[1]=rdf(p+4); j.pos[2]=rdf(p+8);
        j.quat[0]=rdf(p+12); j.quat[1]=rdf(p+16); j.quat[2]=rdf(p+20); j.quat[3]=rdf(p+24);   // w,x,y,z
        j.scale = rdf(p+28); if (j.scale < 1e-4f || j.scale > 1e4f) j.scale = 1.0f;
        j.parent = parent;
        o = p + 32;
        js.push_back(std::move(j));
    }
    s.joints = std::move(js);
    rs_buildBind(s);
    return true;
}

// "Skel" custom binary: magic"Skel"(4) ver u32 nameLen u32 name | then a 4B field | per joint:
//   nameLen u32, name, pos f32x3, quat(w,x,y,z) f32x4, scale f32, parent u16. FLAT (parent usually 0xffff).
inline bool parseRendSkel(const std::vector<u8>& d, RendSkel& s) {
    if (d.size() < 26 || (memcmp(d.data(), "Skel", 4) != 0 && memcmp(d.data(), "lekS", 4) != 0)) return false;
    // V203 vista format (parent-first, u16 jointCount) — the whale/turtle skeletons. Try it first; it
    // returns false cleanly if the bytes are the older nuxd layout, which the code below then handles.
    { RendSkel v; if (parseRendSkelV203(d, v) && v.joints.size() >= 2) { s = std::move(v); return true; } }
    auto rd32=[&](size_t o)->u32{ return o+4<=d.size()? *(const u32*)(d.data()+o):0u; };
    auto rd16=[&](size_t o)->u16{ return o+2<=d.size()? *(const u16*)(d.data()+o):(u16)0; };
    auto rdf=[&](size_t o){ float f=0; if(o+4<=d.size()) memcpy(&f,d.data()+o,4); return f; };
    size_t o = 8;                          // after magic+ver
    u32 nameLen = rd32(o); o += 4 + nameLen; // skeleton name
    o += 4;                                // 4-byte field (count/flags) before joints
    while (o + 4 < d.size()) {
        u32 nl = rd32(o);
        if (nl == 0 || nl > 64 || o + 4 + nl + 30 > d.size()) break;   // joint record = name + 30B
        RendJoint j;
        j.name.assign((const char*)d.data()+o+4, nl);
        if (j.name.compare(0,6,"joint_") != 0 && !s.joints.empty()) break;  // sanity: joints are "joint_*"
        size_t p = o + 4 + nl;
        j.pos[0]=rdf(p); j.pos[1]=rdf(p+4); j.pos[2]=rdf(p+8);
        j.quat[0]=rdf(p+12); j.quat[1]=rdf(p+16); j.quat[2]=rdf(p+20); j.quat[3]=rdf(p+24);
        j.scale = rdf(p+28);  if (j.scale < 1e-4f || j.scale > 1e4f) j.scale = 1.0f;
        j.parent = (int)(short)rd16(p+32);     // 0xffff -> -1
        s.joints.push_back(std::move(j));
        o = p + 34;
    }
    if (s.joints.empty()) return false;
    int n = (int)s.joints.size();
    s.bind.resize(16*n); s.invBind.resize(16*n);
    for (int i=0;i<n;i++) {
        float local[16]; rs_trs(s.joints[i].quat, s.joints[i].pos, s.joints[i].scale, local);
        // flat skeleton -> world == local (parent<0). (If a real hierarchy ever appears, chain here.)
        float world[16]; memcpy(world, local, 64);
        if (s.joints[i].parent >= 0 && s.joints[i].parent < n)
            rs_mul(&s.bind[16*s.joints[i].parent], local, world);
        memcpy(&s.bind[16*i], world, 64);
        if (!rs_invert(world, &s.invBind[16*i])) { memset(&s.invBind[16*i],0,64); s.invBind[16*i]=s.invBind[16*i+5]=s.invBind[16*i+10]=s.invBind[16*i+15]=1; }
    }
    return true;
}

// HZANIM clip "take_001" = hzanim::Clip, format reversed EXACTLY from libshell V203
//   (HzAnimAssetHandler::initAsset sub_175F004, Clip::Clip sub_21A9D64, seek sub_CF79A0):
//   header(32): +0 magic 0xA34912B6, +4 ver(=3), +8 jointCount, +12 channelMapOff, +16 blockOff,
//     +20 blockSize, +24 nameLen; then name; then channel map (jointCount bytes; 0x01 = ROTATION-ONLY).
//   block @blockOff = track header H, offset-table base = H+32: H+16 joints, H+20 totalKeys,
//     H+24 fps(float=24), H+28 flags, H+32 segCount, H+40 frameCount; offset fields (rel H+32):
//     H+68 segment records (segCount*4 u32 = [c0bytes,c1bytes,c2bytes,absByteOff]), H+72 2-bit selector mask,
//     H+76 base-float header, H+80 -> base-quaternion array.
//   Base quats = PLANAR SoA, 4-wide (per group of 4 joints: x0..x3,y0..y3,z0..z3). Each rotation is
//   smallest-three / drop-W: stored (x,y,z), reconstructed w = sqrt(max(0,1-x^2-y^2-z^2)). VALIDATED:
//   all 107 joints decode |xyz|^2<=1. (Per-frame motion lives in the segmented variable-bitrate bitstream
//   at the segment byte offsets; decoded incrementally — see HSR_HZBITS. This always yields the reference pose.)
inline bool parseRendClip(const std::vector<u8>& d, int nJoints, RendClip& c) {
    if (d.size() < 80 || nJoints <= 0) return false;
    // V203: the HzAnim clip wraps a stock ACL `compressed_tracks` — decode it with the real ACL library
    // (byte-exact with libshell's inlined ACL decompressor sub_175F004). Preferred over the legacy path below.
    if (HzAclClip* a = hzAclCreate(d.data(), d.size())) {
        c.acl = a; c.aclJoints = hzAclJointCount(a); c.fps = hzAclSampleRate(a);
        return true;
    }
    if (d.size() < 200) return false;
    auto rd32=[&](size_t o)->u32{ return o+4<=d.size()? *(const u32*)(d.data()+o):0u; };
    auto rdf=[&](size_t o){ float f=0; if(o+4<=d.size()) memcpy(&f,d.data()+o,4); return f; };
    if (rd32(0) != 0xA34912B6u) return false;                 // hzanim clip magic
    int hdrJoints = (int)rd32(8);
    size_t H = rd32(16); if (H==0 || H+96>=d.size()) H=160;    // keyframe block / track header
    size_t obase = H + 32;                                     // offset-table base
    c.fps = rdf(H+24); if (c.fps<1.f||c.fps>240.f) c.fps=24.0f;
    size_t baseQ = obase + rd32(H+80);                         // planar-SoA base quaternions
    int nj = nJoints; if (hdrJoints>0 && hdrJoints<=nj) nj = hdrJoints;
    // Each group of 4 joints = 24 floats: [base x0..3,y0..3,z0..3][range x0..3,y0..3,z0..3].
    if (baseQ + (size_t)((nj+3)/4)*24*4 > d.size()) return false;
    int njc = (hdrJoints>0 && hdrJoints<=4096) ? hdrJoints : nj;  // clip joint count (107)
    c.nJoints = nj;
    // The base+range SoA array stores 24 floats (12 base + 12 range) per COMPLETE 4-joint group, but the
    // final partial group (joints nFull..njc-1, e.g. 104..106 of 107) carries BASE ONLY — verified by the
    // array ending exactly at nFull/4 full groups + one base-only block (no range floats follow). So those
    // joints have no per-frame range and stay at their rest rotation. (Faithful to the cook, not a clamp.)
    int nFull = (njc/4)*4;
    auto getBase=[&](int j, float* base, float* rng){
        int g=j/4, lane=j%4; size_t o = baseQ + (size_t)g*24*4;
        base[0]=rdf(o+lane*4); base[1]=rdf(o+(4+lane)*4); base[2]=rdf(o+(8+lane)*4);
        if (j < nFull) { rng[0]=rdf(o+(12+lane)*4); rng[1]=rdf(o+(16+lane)*4); rng[2]=rdf(o+(20+lane)*4); }
        else           { rng[0]=rng[1]=rng[2]=0.f; }
    };
    // --- per-frame bitstream decode (hzanim, reversed from initAsset/CF79A0) ---
    // Per segment: [njc-byte shared bit-width table][continuous frames, each = x[njc] y[njc] z[njc]],
    // big-endian. value = base + range*(rawBits/(2^bw-1)); bw==31 -> raw 32-bit float. quat=drop-W.
    size_t segrec = obase + rd32(H+68);
    int segCount = (int)rd32(H+32); if (segCount<1 || segCount>4096) segCount=1;
    struct BR { const u8* p; size_t byte, end; int bit;
        u32 read(int n){ u32 v=0; while(n-->0){ if(byte>=end) return v; v=(v<<1)|((p[byte]>>(7-bit))&1u); if(++bit==8){bit=0;++byte;} } return v; } };
    std::vector<float> clipQ;   // (frame, clipJoint) -> w,x,y,z
    int totalFrames=0;
    for (int si=0; si<segCount; ++si){
        size_t segOff = obase + rd32(segrec + 16*si + 12);                    // col3 = byte offset
        size_t segEnd = (si+1<segCount) ? obase + rd32(segrec + 16*(si+1) + 12) : d.size();
        if (segOff + (size_t)njc >= d.size() || segEnd <= segOff + (size_t)njc) continue;
        if (segEnd > d.size()) segEnd = d.size();
        const u8* bw = d.data() + segOff;                                     // njc bit-widths
        size_t sumBw=0; for (int j=0;j<njc;j++) sumBw += (bw[j]>=31?32u:bw[j]);
        if (sumBw==0) continue;
        if ((segEnd - (segOff + njc)) * 8 < 3*sumBw) continue;
        // ONE keyframe per segment. The seek (CF79A0) brackets SEGMENTS and interpolates between them,
        // so the clip is ~segCount keyframes slerped — NOT the dense per-byte frames (those made it jumpy/
        // shaky). Each segment's keyframe = its first decoded pose.
        BR br{ d.data(), segOff + (size_t)njc, segEnd, 0 };
        std::vector<float> X(njc), Y(njc), Z(njc);
        auto rv=[&](u8 w)->float{ if(w==0)return 0.f; if(w>=31){u32 r=br.read(32); float ff; memcpy(&ff,&r,4); return ff;} return (float)br.read(w)/(float)((1u<<w)-1u); };
        for(int j=0;j<njc;j++) X[j]=rv(bw[j]);
        for(int j=0;j<njc;j++) Y[j]=rv(bw[j]);
        for(int j=0;j<njc;j++) Z[j]=rv(bw[j]);
        for(int j=0;j<njc;j++){
            float base[3],rng[3]; getBase(j,base,rng);
            float x=base[0]+rng[0]*X[j], y=base[1]+rng[1]*Y[j], z=base[2]+rng[2]*Z[j];
            float w2=1.f-(x*x+y*y+z*z); float w=w2>0.f?std::sqrt(w2):0.f;
            clipQ.push_back(w); clipQ.push_back(x); clipQ.push_back(y); clipQ.push_back(z);
        }
        ++totalFrames;
    }
    if (totalFrames < 1) return false;
    // The cumulative table (@ H+84) gives each segment-keyframe's frame index; play the segCount
    // keyframes over that span @24fps so the slerp matches the original timing (smooth, gentle ripple).
    u32 lastCum = rd32((size_t)(H+84) + 4u*(segCount-1));
    if (lastCum < (u32)segCount || lastCum > 100000u) lastCum = (u32)segCount*16u;
    c.fps = (float)totalFrames * 24.0f / (float)lastCum;
    // Off-by-one skeleton alignment: skel joint[j] <- clip joint[j-1]; joint[0] = identity root.
    c.nFrames = totalFrames;
    c.quats.assign((size_t)totalFrames*nj*4, 0.f);
    for (int f=0; f<totalFrames; ++f)
        for (int j=0;j<nj;j++){
            float* q=&c.quats[((size_t)f*nj+j)*4];
            int src=j-1;
            if (src<0 || src>=njc){ q[0]=1.f; q[1]=q[2]=q[3]=0.f; continue; }
            const float* sq=&clipQ[((size_t)f*njc+src)*4];
            q[0]=sq[0]; q[1]=sq[1]; q[2]=sq[2]; q[3]=sq[3];
        }
    return true;
}

// Sample the clip at time t -> per-joint skinning matrices (col-major 16 each). skin = jointAnimWorld * invBind.
inline void sampleRendClip(const RendSkel& s, const RendClip& c, float t, std::vector<float>& outSkin) {
    int n = (int)s.joints.size(); outSkin.resize(16*n);
    // A skeleton/clip pair from different (or hostile) files: invBind / quats shorter than the joints and
    // frames claim -> identity skin rather than reading past them.
    const bool sized = s.invBind.size() >= (size_t)16*n &&
        (c.acl || (c.nFrames > 0 && c.nJoints > 0 && c.quats.size() >= (size_t)c.nFrames*c.nJoints*4));
    if (!c.ok() || !sized || std::getenv("HSR_SKINID")) { for(int i=0;i<n;i++){ float* m=&outSkin[16*i]; memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; } return; }
    // V203 ACL path: decode each joint's LOCAL transform (rotation + translation + scale) at time t,
    // compose the hierarchy to animated world, then skin[j] = animWorld[j] * invBind[j]. The ACL clip
    // carries the FULL local transform incl. the swimming root translation — use it directly (the legacy
    // path below assumed rotation-only + bind translation, which froze the whale's swim path).
    if (c.acl) {
        std::vector<float> loc((size_t)n*8, 0.f);
        int nj = hzAclSampleLocal(c.acl, t, loc.data(), n);
        std::vector<float> animWorld(16*n);
        for (int i=0;i<n;i++) {
            float q[4], tr[3], sc;
            if (i < nj) { const float* L=&loc[i*8]; q[0]=L[0];q[1]=L[1];q[2]=L[2];q[3]=L[3]; tr[0]=L[4];tr[1]=L[5];tr[2]=L[6]; sc=L[7]; }
            else { q[0]=1;q[1]=q[2]=q[3]=0; tr[0]=s.joints[i].pos[0];tr[1]=s.joints[i].pos[1];tr[2]=s.joints[i].pos[2]; sc=1.f; }
            if (sc<1e-4f||sc>1e4f) sc=1.0f;
            float localM[16]; rs_trs(q, tr, sc, localM);
            int par = s.joints[i].parent;
            if (par>=0 && par<n) rs_mul(&animWorld[16*par], localM, &animWorld[16*i]);   // parents precede children
            else                 memcpy(&animWorld[16*i], localM, 64);
            rs_mul(&animWorld[16*i], &s.invBind[16*i], &outSkin[16*i]);
        }
        return;
    }
    float frame = t * c.fps; int f0 = (int)frame % c.nFrames; if(f0<0)f0+=c.nFrames; int f1=(f0+1)%c.nFrames; float a=frame-(float)((int)frame);
    int nj = c.nJoints;
    for (int i=0;i<n;i++) {
        int ci = i < nj ? i : nj-1;
        const float* qa=&c.quats[((size_t)f0*nj+ci)*4];
        const float* qb=&c.quats[((size_t)f1*nj+ci)*4];
        float q[4]; float dot=qa[0]*qb[0]+qa[1]*qb[1]+qa[2]*qb[2]+qa[3]*qb[3]; float sgn=dot<0?-1.f:1.f;
        for(int k=0;k<4;k++) q[k]=qa[k]*(1-a)+qb[k]*sgn*a;
        float anim[16]; rs_trs(q, s.joints[i].pos, s.joints[i].scale, anim);   // translation from bind (clip is rotation)
        rs_mul(anim, &s.invBind[16*i], &outSkin[16*i]);
    }
}
