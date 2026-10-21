#pragma once
// Ambient audio: decode a looping sound — the env's own .ogg (Ogg Vorbis with stb_vorbis, Ogg Opus with opusfile
// when built with it) or one added in Frame Studio (also WAV, MP3, FLAC: miniaudio's decoders) — then play the raw
// PCM on a miniaudio device, looping forever, at a volume that can change while it plays. Self-contained.
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#if __has_include(<opusfile.h>)
#include <opusfile.h>
#endif

// Runtime mute for the PC preview audio (a UI toggle in the editor). The data callback outputs silence when set,
// so the looping ambient can be muted/unmuted LIVE without tearing down the audio device. Defined in main.cpp.
extern std::atomic<bool> g_audioMuted;

// stb_vorbis: decode a whole Ogg Vorbis buffer to interleaved int16 PCM (impl in stb_vorbis_impl.c).
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate, short** output);

struct AudioPlayer {
    ma_device device{};
    std::vector<short> pcm;          // interleaved s16, the whole decoded loop
    ma_uint32 channels = 0, sampleRate = 0;
    ma_uint64 totalFrames = 0, cursor = 0;
    bool ok = false;
    std::atomic<float> gain{1.0f};   // volume (0..1, may be changed while playing)

    // Audio thread: copy frames from the decoded PCM, wrapping at the end (seamless loop).
    static void dataCb(ma_device* d, void* out, const void* in, ma_uint32 frames) {
        (void)in;
        AudioPlayer* self = (AudioPlayer*)d->pUserData;
        short* o = (short*)out;
        const ma_uint64 ch = self->channels;
        if (self->totalFrames == 0) { for (ma_uint32 i=0;i<frames*ch;++i) o[i]=0; return; }
        if (g_audioMuted.load(std::memory_order_relaxed)) { for (ma_uint32 i=0;i<frames*ch;++i) o[i]=0; return; }   // UI mute
        const float g = std::max(0.0f, std::min(1.0f, self->gain.load(std::memory_order_relaxed)));
        for (ma_uint32 f = 0; f < frames; ++f) {
            if (self->cursor >= self->totalFrames) self->cursor = 0;
            if (g >= 0.999f) for (ma_uint64 c = 0; c < ch; ++c) o[f*ch + c] = self->pcm[self->cursor*ch + c];
            else for (ma_uint64 c = 0; c < ch; ++c) o[f*ch + c] = (short)(self->pcm[self->cursor*ch + c] * g);
            ++self->cursor;
        }
    }

    // Play already-decoded interleaved s16 PCM on a looping device. The universal entry point: the env's theme is
    // decoded once (any container via audioconv) and handed here, so the preview plays ogg/wav/mp3/flac alike.
    // evenOut: raise a very quiet bed (a home's own; a sound someone chose plays as they made it).
    bool startPCM(const short* samples, ma_uint64 frames, int chs, int sr, bool evenOut = true) {
        if (!samples || frames == 0 || chs <= 0 || sr <= 0) return false;
        stop();
        channels = (ma_uint32)chs; sampleRate = (ma_uint32)sr; totalFrames = frames; cursor = 0;
        pcm.assign(samples, samples + (size_t)frames * chs);
        // Even out loudness: some homes' beds are authored very quiet (FMOD raised them with event volume we don't
        // read). Toward -30 dBFS RMS, at most +24 dB, never clipping.
        if (evenOut) {
            double sum = 0; int peak = 1;
            for (short v : pcm) { sum += (double)v * v; peak = std::max(peak, std::abs((int)v)); }
            const double rms = std::sqrt(sum / (double)pcm.size()) / 32768.0;
            if (rms > 1e-6) {
                double gain = std::min({0.0316 / rms, 16.0, 32767.0 / peak});
                if (gain > 1.05) {
                    for (short& v : pcm) v = (short)std::lround(v * gain);
                    fprintf(stderr, "[AUDIO] quiet bed (%.0f dBFS): raised %.1f dB\n", 20 * std::log10(rms), 20 * std::log10(gain));
                }
            }
        }
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format   = ma_format_s16;
        c.playback.channels = (ma_uint32)chs;
        c.sampleRate        = (ma_uint32)sr;
        c.dataCallback      = dataCb;
        c.pUserData         = this;
        if (ma_device_init(NULL, &c, &device) != MA_SUCCESS) { fprintf(stderr, "[AUDIO] device init failed\n"); return false; }
        if (ma_device_start(&device) != MA_SUCCESS) { ma_device_uninit(&device); return false; }
        ok = true;
        fprintf(stderr, "[AUDIO] looping ambient: %d Hz, %d ch, %llu frames (%.1fs)\n",
                sr, chs, (unsigned long long)frames, frames / (float)(sr > 0 ? sr : 1));
        return true;
    }

    // Interleaved s16 at any channel count -> stereo. 4+ channels: first-order ambisonics (ACN W,Y,Z,X: W carries most
    // energy; the homes' beds, Clay and HSL alike) -> L/R = W +- 0.7 Y; 5.1 / 7.1 (FL FR C LFE SL SR [BL BR], a sound
    // someone added) -> the usual downmix; otherwise a plain quad layout -> average the front/back pairs.
    static void toStereo(const short* raw, size_t frames, int chs, std::vector<short>& out) {
        out.resize(frames * 2);
        if (chs == 1) { for (size_t i = 0; i < frames; ++i) out[i * 2] = out[i * 2 + 1] = raw[i]; return; }
        if (chs == 2) { out.assign(raw, raw + frames * 2); return; }
        double e[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < frames; i += 7) for (int c = 0; c < 4 && c < chs; ++c) e[c] += (double)raw[i * chs + c] * raw[i * chs + c];
        const bool ambi = chs >= 4 && e[0] > 1.5 * e[1] && e[0] > 1.5 * e[2] && e[0] > 1.5 * e[3];
        const bool surround = !ambi && (chs == 6 || chs == 8);
        for (size_t i = 0; i < frames; ++i) {
            const short* s = &raw[i * chs];
            float l, r;
            if (ambi) { l = s[0] + 0.7f * s[1]; r = s[0] - 0.7f * s[1]; }
            else if (surround) {
                const float bl = chs == 8 ? s[6] : 0.0f, br = chs == 8 ? s[7] : 0.0f;
                l = s[0] + 0.707f * s[2] + 0.707f * s[4] + 0.5f * bl;
                r = s[1] + 0.707f * s[2] + 0.707f * s[5] + 0.5f * br;
            } else { l = 0.5f * (s[0] + s[chs >= 3 ? 2 : 0]); r = 0.5f * (s[1] + s[chs >= 4 ? 3 : 1]); }
            out[i * 2] = (short)std::max(-32768.0f, std::min(32767.0f, l));
            out[i * 2 + 1] = (short)std::max(-32768.0f, std::min(32767.0f, r));
        }
        fprintf(stderr, "[AUDIO] %d channels (%s) -> stereo\n", chs, ambi ? "ambisonic" : surround ? "surround" : "quad");
    }

#if __has_include(<opusfile.h>)
    // Ogg OPUS (Clay homes' _BACKGROUND_LOOP.ogg: 48 kHz, often 4-channel ambisonic). opusfile downmixes to stereo.
    // Ogg page CRC (polynomial 0x04c11db7, no reflection, initial 0) over the page with its CRC field zeroed.
    static uint32_t oggCrc(const uint8_t* p, size_t n) {
        uint32_t crc = 0;
        for (size_t i = 0; i < n; ++i) {
            crc ^= (uint32_t)p[i] << 24;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
        }
        return crc;
    }

    static bool decodeOpus(const uint8_t* data, size_t n, std::vector<short>& out) {
        // Meta's ambience is channel-mapping family 255 (4 discrete streams, ambisonic), which opusfile refuses
        // (OP_EIMPL). The streams are plain Opus: relabel the head as family 1 in a copy (re-CRC its Ogg page),
        // read the raw channels, and fold them to stereo here.
        std::vector<uint8_t> buf(data, data + n);
        if (n > 47 && buf[46] == 255 && buf[26] >= 1) {
            buf[46] = 1;
            size_t pageLen = 27 + buf[26];
            for (int s = 0; s < buf[26]; ++s) pageLen += buf[27 + s];
            if (pageLen <= n) {
                std::memset(&buf[22], 0, 4);
                const uint32_t crc = oggCrc(buf.data(), pageLen);
                std::memcpy(&buf[22], &crc, 4);
            }
        }
        int err = 0;
        OggOpusFile* f = op_open_memory(buf.data(), buf.size(), &err);
        if (!f) { fprintf(stderr, "[AUDIO] op_open_memory error %d\n", err); return false; }
        const int chs = op_channel_count(f, -1);
        std::vector<short> raw;
        std::vector<short> tmp(5760 * (size_t)std::max(1, chs));   // 120 ms at 48 kHz
        for (;;) {
            const int got = op_read(f, tmp.data(), (int)tmp.size(), nullptr);
            if (got <= 0) break;
            raw.insert(raw.end(), tmp.begin(), tmp.begin() + (size_t)got * chs);
        }
        op_free(f);
        if (raw.empty() || chs <= 0) return false;
        toStereo(raw.data(), raw.size() / chs, chs, out);
        return true;
    }
#endif

    // Decodes a whole sound file in memory to interleaved s16, at most two channels (more are folded to stereo):
    // Ogg Opus (when built with opusfile), Ogg Vorbis, WAV, MP3 or FLAC. Sounds longer than maxSeconds are cut there
    // (all of it is held in memory to loop seamlessly). what: the format found.
    static bool decode(const uint8_t* data, size_t n, std::vector<short>& out, int& chs, int& sr, const char** what = nullptr,
                       double maxSeconds = 30 * 60) {
        out.clear(); chs = sr = 0;
        if (!data || n < 4) return false;
        if (n > 36 && std::memcmp(data + 28, "OpusHead", 8) == 0) {
#if __has_include(<opusfile.h>)
            if (what) *what = "Ogg Opus";
            if (!decodeOpus(data, n, out)) return false;
            chs = 2; sr = 48000;
            return true;
#else
            fprintf(stderr, "[AUDIO] Ogg Opus: not in this build\n");
            return false;
#endif
        }
        if (std::memcmp(data, "OggS", 4) == 0) {
            if (what) *what = "Ogg Vorbis";
            short* raw = nullptr;
            const int frames = stb_vorbis_decode_memory(data, (int)std::min(n, (size_t)INT32_MAX), &chs, &sr, &raw);
            if (frames <= 0 || !raw || chs <= 0) { if (raw) free(raw); return false; }
            const size_t keep = std::min((size_t)frames, (size_t)(maxSeconds * sr));
            if (chs > 2) { toStereo(raw, keep, chs, out); chs = 2; }
            else out.assign(raw, raw + keep * chs);
            free(raw);
            return true;
        }
        // WAV, MP3, FLAC
        ma_decoder_config dc = ma_decoder_config_init(ma_format_s16, 0, 0);
        ma_decoder dec;
        if (ma_decoder_init_memory(data, n, &dc, &dec) != MA_SUCCESS) return false;
        if (what) *what = std::memcmp(data, "RIFF", 4) == 0 ? "WAV" : std::memcmp(data, "fLaC", 4) == 0 ? "FLAC" : "MP3";
        const int c = (int)dec.outputChannels;
        sr = (int)dec.outputSampleRate;
        std::vector<short> raw;
        std::vector<short> tmp(4096 * (size_t)std::max(1, c));
        const size_t limit = (size_t)(maxSeconds * std::max(1, sr));
        for (size_t got = 0; got < limit;) {
            ma_uint64 read = 0;
            if (ma_decoder_read_pcm_frames(&dec, tmp.data(), 4096, &read) != MA_SUCCESS || read == 0) break;
            read = std::min<ma_uint64>(read, limit - got);
            raw.insert(raw.end(), tmp.begin(), tmp.begin() + (size_t)read * c);
            got += (size_t)read;
        }
        ma_decoder_uninit(&dec);
        if (raw.empty() || c <= 0 || sr <= 0) return false;
        if (c > 2) { toStereo(raw.data(), raw.size() / c, c, out); chs = 2; }
        else { out.swap(raw); chs = c; }
        return true;
    }

    // Decode a sound file (see decode) and play it, looping, at `vol`. evenOut: see startPCM.
    bool start(const uint8_t* data, size_t n, float vol = 1.0f, bool evenOut = true) {
        std::vector<short> out;
        int chs = 0, sr = 0;
        const char* what = "sound";
        if (!decode(data, n, out, chs, sr, &what)) { fprintf(stderr, "[AUDIO] %s decode failed\n", what); return false; }
        gain = vol;
        return startPCM(out.data(), (ma_uint64)(out.size() / chs), chs, sr, evenOut);
    }
    void stop() { if (ok) { ma_device_uninit(&device); ok = false; } }
    ~AudioPlayer() { stop(); }
};
