#pragma once
// A video for a screen in a home (Frame Studio's Import Video): the frames as JPEG pictures and the sound as Opus
// packets, in one file (.fvid) that Frame Studio writes when a video is imported and Frame Home plays — so the headset
// needs no video decoder, only the JPEG one it already has, and each frame decodes on its own (any frame, any order:
// looping and seeking cost nothing). A still stretch (a GIF holding a frame) stores the picture once.
//
// Layout (little-endian):
//   "FVID" u32 version  u64 indexAt          the head (16 bytes)
//   the frames' JPEGs, then the sound's packets
//   the index at indexAt:
//     u32 width, height   f32 fps   u32 frames
//     u32 channels (0: no sound, 1, 2)   u32 rate (48000)   u32 packets   u32 samplesPerPacket   u32 preSkip   u64 samples
//     per frame: u64 at, u32 bytes
//     u64 soundAt   per packet: u32 bytes
//
// The reading half (VideoClip::open, frame) is all a model import needs (its first frame is the screen's picture);
// the sound's two halves need libopus (Frame Studio and Frame Home link it; the scene library doesn't).
#include "core/types.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if __has_include(<opus.h>)
#include <opus.h>
#define FVID_HAVE_OPUS 1
#endif

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

inline bool isVideoClipPath(const std::string& path) {
    if (path.size() <= 5) return false;
    std::string ext = path.substr(path.size() - 5);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".fvid";
}

struct VideoClip {
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kRate = 48000;            // the sound is always 48 kHz (Opus's own rate)
    struct Span { uint64_t at; uint32_t bytes; };
    uint32_t width = 0, height = 0, frames = 0;
    float fps = 30;
    uint32_t channels = 0, packets = 0, samplesPerPacket = 960, preSkip = 0;
    uint64_t samples = 0;                               // the sound's length per channel (after preSkip)
    std::vector<Span> spans;
    uint64_t soundAt = 0;
    std::vector<uint32_t> packetBytes;
    std::string path;

    double seconds() const { return fps > 0 ? frames / static_cast<double>(fps) : 0; }
    bool hasSound() const { return channels > 0 && packets > 0; }

    bool open(const std::string& file, std::string* error = nullptr) {
        auto fail = [&](const char* why) { if (error) *error = std::string(why) + ": " + file; return false; };
        std::ifstream f(file, std::ios::binary);
        if (!f) return fail("can't open the video");
        char magic[4];
        uint32_t version = 0, rate = 0;
        uint64_t indexAt = 0;
        if (!f.read(magic, 4) || std::memcmp(magic, "FVID", 4) != 0) return fail("not a Frame video");
        if (!read(f, version) || !read(f, indexAt)) return fail("damaged video");
        if (version > kVersion) return fail("a video from a newer Frame Studio");
        f.seekg(static_cast<std::streamoff>(indexAt));
        if (!read(f, width) || !read(f, height) || !read(f, fps) || !read(f, frames) || !read(f, channels) || !read(f, rate) ||
            !read(f, packets) || !read(f, samplesPerPacket) || !read(f, preSkip) || !read(f, samples))
            return fail("damaged video index");
        if (!width || !height || width > 8192 || height > 8192 || !frames || frames > 10000000 || !(fps > 0 && fps <= 240) ||
            channels > 2 || (channels && rate != kRate) || packets > 100000000)
            return fail("damaged video index");
        spans.resize(frames);
        for (Span& s : spans)
            if (!read(f, s.at) || !read(f, s.bytes) || s.bytes > (64u << 20)) return fail("damaged video index");
        if (!read(f, soundAt)) return fail("damaged video index");
        packetBytes.resize(packets);
        if (packets && !f.read(reinterpret_cast<char*>(packetBytes.data()), static_cast<std::streamsize>(packets * 4ull))) return fail("damaged video index");
        path = file;
        return true;
    }

    // Frame i's JPEG bytes, read from `f` (a stream on the file: one per reading thread).
    bool frameBytes(std::ifstream& f, uint32_t i, std::vector<u8>& out) const {
        if (i >= frames) return false;
        out.resize(spans[i].bytes);
        f.clear();
        f.seekg(static_cast<std::streamoff>(spans[i].at));
        return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
    }
    // Frame i as RGBA8 (width x height). jpeg: scratch for its bytes.
    bool frame(std::ifstream& f, uint32_t i, std::vector<u8>& rgba, std::vector<u8>& jpeg) const {
        if (!frameBytes(f, i, jpeg)) return false;
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &comp, 4);
        if (!px) return false;
        const bool ok = static_cast<uint32_t>(w) == width && static_cast<uint32_t>(h) == height;
        if (ok) rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
        return ok;
    }
    bool frame(uint32_t i, std::vector<u8>& rgba) const {
        std::ifstream f(path, std::ios::binary);
        std::vector<u8> jpeg;
        return f && frame(f, i, rgba, jpeg);
    }
    // The same frame as frame i (a still stretch): no need to decode it again.
    bool sameFrame(uint32_t a, uint32_t b) const { return a < frames && b < frames && spans[a].at == spans[b].at; }

#ifdef FVID_HAVE_OPUS
    // The whole sound as interleaved int16 at 48 kHz (`channels` of them).
    bool sound(std::vector<short>& pcm) const {
        pcm.clear();
        if (!hasSound()) return false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(static_cast<std::streamoff>(soundAt));
        int err = 0;
        OpusDecoder* dec = opus_decoder_create(kRate, static_cast<int>(channels), &err);
        if (!dec || err != OPUS_OK) return false;
        std::vector<u8> packet;
        std::vector<short> block(static_cast<size_t>(5760) * channels);
        pcm.reserve(static_cast<size_t>(samples + preSkip) * channels);
        for (uint32_t p = 0; p < packets; ++p) {
            packet.resize(packetBytes[p]);
            if (!f.read(reinterpret_cast<char*>(packet.data()), static_cast<std::streamsize>(packet.size()))) break;
            const int n = opus_decode(dec, packet.data(), static_cast<opus_int32>(packet.size()), block.data(), 5760, 0);
            if (n > 0) pcm.insert(pcm.end(), block.begin(), block.begin() + static_cast<ptrdiff_t>(n) * channels);
        }
        opus_decoder_destroy(dec);
        const size_t skip = std::min(pcm.size(), static_cast<size_t>(preSkip) * channels);
        pcm.erase(pcm.begin(), pcm.begin() + static_cast<ptrdiff_t>(skip));
        if (pcm.size() > samples * channels) pcm.resize(static_cast<size_t>(samples) * channels);
        return !pcm.empty();
    }
#endif

private:
    template <typename T> static bool read(std::ifstream& f, T& v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v)); }
};

// Writes a .fvid: frames (RGBA8, all one size) as they come, JPEG-encoded by `encodeJpeg` (the caller's encoder:
// Frame Studio's stb_image_write), then the sound, then the index.
struct VideoClipWriter {
    using JpegEncoder = bool (*)(const u8* rgba, uint32_t w, uint32_t h, int quality, std::vector<u8>& out);
    JpegEncoder encodeJpeg = nullptr;
    int quality = 80;

    bool begin(const std::string& file, uint32_t w, uint32_t h, float fps) {
        out_.open(file, std::ios::binary | std::ios::trunc);
        if (!out_) return false;
        w_ = w; h_ = h; fps_ = fps;
        out_.write("FVID", 4);
        put(VideoClip::kVersion);
        put(uint64_t{0});                               // indexAt, patched by finish()
        frames_.clear();
        return static_cast<bool>(out_);
    }
    bool addFrame(const u8* rgba) {
        std::vector<u8> jpeg;
        if (!encodeJpeg || !encodeJpeg(rgba, w_, h_, quality, jpeg)) return false;
        frames_.push_back({static_cast<uint64_t>(out_.tellp()), static_cast<uint32_t>(jpeg.size())});
        out_.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        return static_cast<bool>(out_);
    }
    // A frame already encoded (copied from another clip, or encoded on other threads).
    bool addJpeg(const std::vector<u8>& jpeg) {
        if (jpeg.empty()) return false;
        frames_.push_back({static_cast<uint64_t>(out_.tellp()), static_cast<uint32_t>(jpeg.size())});
        out_.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
        return static_cast<bool>(out_);
    }
    // The frame before again (a still stretch): its bytes, not a second picture.
    bool repeatFrame() {
        if (frames_.empty()) return false;
        frames_.push_back(frames_.back());
        return true;
    }
    size_t framesWritten() const { return frames_.size(); }

#ifdef FVID_HAVE_OPUS
    // The sound: interleaved int16 at 48 kHz, 1 or 2 channels. Once, after the frames.
    bool addSound(const short* pcm, size_t samplesPerChannel, int channels) {
        if (!pcm || !samplesPerChannel || channels < 1 || channels > 2) return true;
        int err = 0;
        OpusEncoder* enc = opus_encoder_create(VideoClip::kRate, channels, OPUS_APPLICATION_AUDIO, &err);
        if (!enc || err != OPUS_OK) return false;
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(channels == 2 ? 128000 : 80000));
        opus_int32 lookahead = 0;
        opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
        channels_ = static_cast<uint32_t>(channels);
        preSkip_ = static_cast<uint32_t>(lookahead);
        samples_ = samplesPerChannel;
        soundAt_ = static_cast<uint64_t>(out_.tellp());
        constexpr int kBlock = 960;                     // 20 ms
        std::vector<short> block(static_cast<size_t>(kBlock) * channels);
        std::vector<u8> packet(4000);
        // the decoder skips the encoder's lookahead at the start: that much more at the end, so nothing is cut off
        const size_t total = samplesPerChannel + static_cast<size_t>(lookahead);
        for (size_t at = 0; at < total; at += kBlock) {
            for (int s = 0; s < kBlock; ++s)
                for (int c = 0; c < channels; ++c) {
                    const size_t i = at + s;
                    block[static_cast<size_t>(s) * channels + c] = i < samplesPerChannel ? pcm[i * channels + c] : 0;
                }
            const opus_int32 n = opus_encode(enc, block.data(), kBlock, packet.data(), static_cast<opus_int32>(packet.size()));
            if (n < 0) { opus_encoder_destroy(enc); return false; }
            out_.write(reinterpret_cast<const char*>(packet.data()), n);
            packetBytes_.push_back(static_cast<uint32_t>(n));
        }
        opus_encoder_destroy(enc);
        return static_cast<bool>(out_);
    }
#endif

    bool finish() {
        if (frames_.empty() || !out_) return false;
        const uint64_t indexAt = static_cast<uint64_t>(out_.tellp());
        put(w_); put(h_); put(fps_); put(static_cast<uint32_t>(frames_.size()));
        put(channels_); put(VideoClip::kRate); put(static_cast<uint32_t>(packetBytes_.size())); put(uint32_t{960}); put(preSkip_);
        put(static_cast<uint64_t>(samples_));
        for (const VideoClip::Span& s : frames_) { put(s.at); put(s.bytes); }
        put(soundAt_);
        for (uint32_t b : packetBytes_) put(b);
        out_.seekp(8);
        put(indexAt);
        out_.close();
        return !out_.fail();
    }

private:
    template <typename T> void put(const T& v) { out_.write(reinterpret_cast<const char*>(&v), sizeof v); }
    std::ofstream out_;
    uint32_t w_ = 0, h_ = 0, channels_ = 0, preSkip_ = 0;
    float fps_ = 30;
    uint64_t soundAt_ = 0;
    size_t samples_ = 0;
    std::vector<VideoClip::Span> frames_;
    std::vector<uint32_t> packetBytes_;
};
