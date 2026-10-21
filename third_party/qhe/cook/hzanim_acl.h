#pragma once
// Bridge to the vendored ACL (Animation Compression Library) — libshell's V203 HzAnim clips
// (magic 0xA34912B6) wrap a stock ACL `compressed_tracks` blob (tag 0xAC11AC11) at byte offset
// `*(u32*)&file[16]`. libshell's sub_175F004 (HzAnimAssetHandler) is ACL's decompressor inlined;
// rather than reimplement the NEON bitstream we decode with the real library = byte-exact faithful.
// This header pulls in NO ACL types (ACL lives only in hzanim_acl.cpp) so the renderer/loader TUs
// stay light.
#include <cstdint>
#include <cstddef>
#include <vector>

struct HzAclClip;  // opaque

// ENCODE (HZANIM port): per-joint LOCAL TRS animation -> a V203 HzAnim clip (ACL compressed_tracks wrapped in the
// 0xA34912B6 container). trsLocal[(frame*jointCount + joint)*10] = quat(x,y,z,w), translation(x,y,z), scale(x,y,z);
// parents[joint] = parent index (-1 = root). Returns the full HZAN:ANIM asset bytes (empty on failure).
// shellDistance = the FARTHEST vertex from its joint in JOINT-LOCAL units — ACL evaluates its rotation-error
// tolerance AT this radius. The old fixed 1.0 let cyan's rigs (node scale ~0.01 -> verts 100-500 local units out)
// wobble by centimeters at the real geometry ("animations very shaky"): quantization error legal at 1.0 is
// amplified 100-500x at the actual lever. Pass the real lever so the error budget applies where the verts are.
std::vector<uint8_t> hzAclEncode(const float* trsLocal, const int* parents, int jointCount, int frameCount, float fps,
                                 float shellDistance = 1.0f);

// Parse the HzAnim wrapper, validate the inner ACL compressed_tracks, build a decompression context.
// Copies the bytes it needs, so `file` need not outlive the call. Returns nullptr if not a valid clip.
HzAclClip* hzAclCreate(const uint8_t* file, size_t n);
const std::vector<uint8_t>& hzAclBytes(const HzAclClip*);   // the HzAnim file the clip was made from (baked scenes store it)
void       hzAclDestroy(HzAclClip*);

int   hzAclJointCount(const HzAclClip*);   // = num ACL tracks
float hzAclDuration(const HzAclClip*);     // seconds
float hzAclSampleRate(const HzAclClip*);   // fps

// Sample the clip at time `t` (seconds; wrapped to [0,duration] by ACL seek). Writes, per joint,
// 8 floats into `out`: rotation quaternion (w,x,y,z) then translation (x,y,z) then uniform scale.
// `maxJoints` caps how many joints are written. Returns the number of joints written.
int hzAclSampleLocal(HzAclClip* c, float t, float* out /*maxJoints*8*/, int maxJoints);
