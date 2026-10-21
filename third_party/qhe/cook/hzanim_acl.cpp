// ACL decode bridge — the ONLY TU that includes ACL/RTM. See hzanim_acl.h.
#include "cook/hzanim_acl.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <acl/decompression/decompress.h>
#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/track.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>

using namespace acl;

namespace {
// Captures per-track rotation/translation/scale from acl::decompress_tracks.
struct LocalWriter : public track_writer
{
    rtm::qvvf x[256];
    int count = 0;
    void RTM_SIMD_CALL write_rotation(uint32_t i, rtm::quatf_arg0 r)
    { if (i < 256) { x[i].rotation = r; if ((int)i + 1 > count) count = i + 1; } }
    void RTM_SIMD_CALL write_translation(uint32_t i, rtm::vector4f_arg0 t)
    { if (i < 256) x[i].translation = t; }
    void RTM_SIMD_CALL write_scale(uint32_t i, rtm::vector4f_arg0 s)
    { if (i < 256) x[i].scale = s; }
};
}

struct HzAclClip
{
    std::vector<uint8_t> bytes;                 // owns the whole HzAnim file (ACL blob points into it)
    const compressed_tracks* tracks = nullptr;
    decompression_context<default_transform_decompression_settings> ctx;
};

HzAclClip* hzAclCreate(const uint8_t* file, size_t n)
{
    if (!file || n < 80) return nullptr;
    uint32_t magic  = *(const uint32_t*)(file + 0);
    if (magic != 0xA34912B6u) return nullptr;          // HzAnim clip magic
    uint32_t aclOff = *(const uint32_t*)(file + 16);   // byte offset of the ACL compressed_tracks
    // 64-bit compare (aclOff + 16 wraps in u32); ACL needs its 16-byte alignment (vector storage is 16-aligned).
    if (aclOff % 16 || (uint64_t)aclOff + 16 > n) return nullptr;

    auto* c = new HzAclClip();
    c->bytes.assign(file, file + n);
    c->tracks = reinterpret_cast<const compressed_tracks*>(c->bytes.data() + aclOff);
    // is_valid hashes get_size() bytes and reads tracks_header: both must lie inside the file.
    const uint32_t accSize = c->tracks->get_size();
    if (accSize > n - aclOff || accSize < sizeof(acl_impl::raw_buffer_header) + sizeof(acl_impl::tracks_header)) { delete c; return nullptr; }
    error_result res = c->tracks->is_valid(true);
    if (res.any() || !c->ctx.initialize(*c->tracks)) { delete c; return nullptr; }
    return c;
}

void hzAclDestroy(HzAclClip* c) { delete c; }

const std::vector<uint8_t>& hzAclBytes(const HzAclClip* c) { static const std::vector<uint8_t> none; return c ? c->bytes : none; }

int   hzAclJointCount(const HzAclClip* c) { return c && c->tracks ? (int)c->tracks->get_num_tracks() : 0; }
float hzAclDuration(const HzAclClip* c)   { return c && c->tracks ? c->tracks->get_duration() : 0.0f; }
float hzAclSampleRate(const HzAclClip* c) { return c && c->tracks ? c->tracks->get_sample_rate() : 30.0f; }

int hzAclSampleLocal(HzAclClip* c, float t, float* out, int maxJoints)
{
    if (!c || !c->tracks || !out) return 0;
    float dur = c->tracks->get_duration();          // ACL seek CLAMPS t to [0,dur] (no wrap) -> wrap here so the clip LOOPS
    if (dur > 1e-6f) { t = std::fmod(t, dur); if (t < 0.f) t += dur; }
    c->ctx.seek(t, sample_rounding_policy::none);   // interpolate between the two bracketing samples
    LocalWriter w;
    c->ctx.decompress_tracks(w);
    static const bool conj = std::getenv("HSR_QCONJ") != nullptr;   // test: ACL rotation sense vs skel
    int nj = w.count < maxJoints ? w.count : maxJoints;
    for (int j = 0; j < nj; ++j)
    {
        // ACL/rtm quaternions are (x,y,z,w); our skinning (rs_trs) wants (w,x,y,z).
        float* o = out + j * 8;
        float sgn = conj ? -1.0f : 1.0f;
        o[0] = rtm::quat_get_w(w.x[j].rotation);
        o[1] = sgn * rtm::quat_get_x(w.x[j].rotation);
        o[2] = sgn * rtm::quat_get_y(w.x[j].rotation);
        o[3] = sgn * rtm::quat_get_z(w.x[j].rotation);
        o[4] = rtm::vector_get_x(w.x[j].translation);
        o[5] = rtm::vector_get_y(w.x[j].translation);
        o[6] = rtm::vector_get_z(w.x[j].translation);
        o[7] = rtm::vector_get_x(w.x[j].scale);
    }
    return nj;
}

// ── ENCODE: per-joint LOCAL TRS -> ACL compressed_tracks -> HzAnim 0xA34912B6 wrapper (HZANIM port). ──
std::vector<uint8_t> hzAclEncode(const float* trs, const int* parents, int jointCount, int frameCount, float fps,
                                 float shellDistance)
{
    if (!trs || jointCount < 1 || frameCount < 1) return {};
    if (!(shellDistance >= 1.0f)) shellDistance = 1.0f;   // never LOOSER than the old default (NaN-safe)
    ansi_allocator allocator;
    track_array_qvvf tracks(allocator, (uint32_t)jointCount);
    for (int j = 0; j < jointCount; ++j)
    {
        track_desc_transformf desc;
        desc.output_index  = (uint32_t)j;
        desc.parent_index  = (parents && parents[j] >= 0) ? (uint32_t)parents[j] : k_invalid_track_index;
        desc.precision     = 0.0001f;
        // Error budget applied at the REAL vertex lever, not a fixed 1.0 (the cyan "very shaky" fix):
        // rigs with ~0.01 node scale put verts 100-500 JOINT-LOCAL units out, so quantization error that ACL
        // considered legal at radius 1.0 was amplified 100-500x at the actual geometry = visible wobble.
        desc.shell_distance = shellDistance;
        track_qvvf track = track_qvvf::make_reserve(desc, allocator, (uint32_t)frameCount, fps);
        for (int f = 0; f < frameCount; ++f)
        {
            const float* t = trs + ((size_t)f * jointCount + j) * 10;
            rtm::quatf    rot = rtm::quat_set(t[0], t[1], t[2], t[3]);     // (x,y,z,w)
            rtm::vector4f tr  = rtm::vector_set(t[4], t[5], t[6], 0.0f);
            rtm::vector4f sc  = rtm::vector_set(t[7], t[8], t[9], 0.0f);
            track[f] = rtm::qvv_set(rot, tr, sc);
        }
        tracks[j] = std::move(track);
    }
    compression_settings settings = get_default_compression_settings();
    qvvf_transform_error_metric error_metric;          // transform tracks REQUIRE an error metric (else compress fails)
    settings.error_metric = &error_metric;
    compressed_tracks* out = nullptr;
    output_stats stats;
    error_result result = compress_track_list(allocator, tracks, settings, out, stats);
    if (result.any() || !out) { if (out) allocator.deallocate(out, out->get_size()); return {}; }

    uint32_t S1 = out->get_size();
    // The device's HzAnimAssetInitializer (libshell sub_175F004) parses the @0x10 block as TWO ACL clips: the JOINT
    // (transform) tracks at aclOff, then the FLOAT (scalar) tracks at aclOff+align16(S1), size = @0x14 - align16(S1).
    // A skeletal anim has no scalar tracks, but the device STILL needs a valid empty (num_tracks=0) scalar clip there,
    // else "failed to create float tracks buffer" -> SIGSEGV. This 67-byte clip is a real device empty-float clip.
    static const uint8_t kEmptyFloatClip[67] = {
        0x43,0x00,0x00,0x00, 0x5d,0xc4,0xc2,0xd2, 0x11,0xac,0x11,0xac, 0x0a,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x14,0x00,0x00,0x00, 0x14,0x00,0x00,0x00, 0x14,0x00,0x00,0x00,
        0x14,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x00 };
    uint32_t S2 = (uint32_t)sizeof(kEmptyFloatClip);
    const char* nm = "Take 001"; uint32_t nameLen = 8;
    uint32_t channelMapOff = 48;
    uint32_t aclOff = ((channelMapOff + (uint32_t)jointCount) + 15u) & ~15u;   // ACL blocks 16-aligned, after the channel map
    uint32_t s1a = (S1 + 15u) & ~15u;
    uint32_t blockSize = s1a + S2;                         // @0x14 = align16(jointClipSize) + floatClipSize
    // CRASH FIX (IDA-proven @0x1617f4c and verified against official clips):
    // version>=4 HzAnim clips carry a TRAILING channel-name table AFTER the ACL blocks (past aclOff+blockSize):
    // a u32 count (+1 byte), then `count` u16-length-prefixed channel-name strings. The device's
    // HzAnimAssetInitializer_deserialize reads that u32 count and, if non-zero, loops `std::string::resize(len-1)` +
    // memcpy per name (the SECOND resize @0x16188ac, registered via HzAnimPlayback). EVERY official ver-6 clip
    // (calming butterflies/birds, oceanarium whale) appends EXACTLY 5 zero bytes here (count=0 -> loop skipped). Our
    // cook ended the file exactly at aclOff+blockSize, so the device read `count` from PAST the buffer end = adjacent
    // heap garbage -> when non-zero it resized a string to a bogus length -> std::length_error: basic_string crash-loop
    // -> nuxd fallback. When the heap there happened to be 0 it loaded fine -> "works on SOME cooked homes" (it was
    // data-dependent, never a malformed clip — all header/ACL bytes validate). FIX: append the 5-byte empty table so
    // the count reads 0, byte-structurally identical to the proven official clips. blockSize is UNCHANGED (the official
    // clips also exclude these 5 bytes from blockSize: file == aclOff + blockSize + 5).
    std::vector<uint8_t> b((size_t)aclOff + blockSize + 5, 0);   // +5 = version-6 trailing channel-name table (empty: count=0)
    auto w32 = [&](uint32_t o, uint32_t v){ std::memcpy(b.data() + o, &v, 4); };
    w32(0, 0xA34912B6u); w32(4, 6u); w32(8, (uint32_t)jointCount); w32(12, channelMapOff);   // version 6 (current device HzAnim format; was 3 -> device parsed/rejected the clip -> garbage poses -> mesh collapsed). Header layout is identical, byte-for-byte matched to the calming butterflies' working clip.
    w32(16, aclOff); w32(20, blockSize); w32(24, nameLen);
    b[28] = 0;                                             // @0x1C flag byte (0 in real clips)
    std::memcpy(b.data() + 32, nm, nameLen);               // name @0x20 (device reads the name from +0x20, NOT +0x1C)
    for (int j = 0; j < jointCount; ++j) b[(size_t)channelMapOff + j] = 0x01;   // channel map: 0x01 = transform track
    std::memcpy(b.data() + aclOff, out, S1);               // joint (transform) tracks
    std::memcpy(b.data() + aclOff + s1a, kEmptyFloatClip, S2);   // float (scalar) tracks: empty (num_tracks=0)
    allocator.deallocate(out, S1);
    return b;
}
