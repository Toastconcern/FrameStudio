#pragma once
// Plays a home's video screens (MeshData::video, src/video_clip.h) in Frame Home and in Frame Studio's view: each
// screen's frames decode on a thread of their own and go to its picture (HslRenderer::setVideoFrame), and every
// screen's sound plays through one audio device, each at the loudness its audio area gives where you are — all of it
// within videoNear of the screen, fading out to nothing at videoFar — easing there over about a second, so walking
// in the sound comes up slowly and walking away it dies away. A screen's sound also leans to the side it's on.
// A clip loops; with sound, the sound is the clock (the picture follows it), else the time since it started.
#include "hsl_renderer.h"
#include "hsl_scene.h"
#include "video_clip.h"
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

extern std::atomic<bool> g_audioMuted;      // core/audio.h: silence while set (a game on screen, the UI's mute)

class VideoScreens {
public:
    ~VideoScreens() { stop(); }

    // The scene's screens as the renderer draws them, those shown (call once the home is loaded, and again when one is
    // added, hidden or shown). Sound decodes here (a minute of
    // Opus is a few tens of milliseconds): call it off the frame loop when that matters.
    void start(HslRenderer& r, const HslScene& scene) {
        stop();
        for (size_t i = 0; i < r.meshCount(); ++i) {
            if (!r.meshIsVideo(i) || !r.meshVisible(i)) continue;   // (one hidden or deleted plays nothing)
            const int sm = r.meshSceneIndex(i);
            if (sm < 0 || static_cast<size_t>(sm) >= scene.meshes.size()) continue;
            const MeshData& md = scene.meshes[sm];
            playing_.push_back({i, md.video});
            auto s = std::make_unique<Screen>();
            std::string err;
            if (!s->clip.open(md.video, &err)) { std::fprintf(stderr, "[video] %s\n", err.c_str()); continue; }
            s->draw = i;
            s->sceneMesh = sm;
            s->loopSamples = static_cast<uint64_t>(std::llround(s->clip.seconds() * VideoClip::kRate));
#ifdef FVID_HAVE_OPUS
            if (s->clip.hasSound() && s->clip.sound(s->pcm)) s->channels = s->clip.channels;
#endif
            if (s->pcm.empty()) s->channels = 0;
            s->settings(md);
            Screen* raw = s.get();
            s->decoder = std::thread([raw] { raw->decodeLoop(); });
            std::fprintf(stderr, "[video] %s: %ux%u, %.1f s at %.0f fps, %s\n", md.name.c_str(), s->clip.width, s->clip.height, s->clip.seconds(),
                         s->clip.fps, s->channels ? "with sound" : "no sound");
            screens_.push_back(std::move(s));
        }
        started_ = std::chrono::steady_clock::now();
        bool sound = false;
        for (const auto& s : screens_) sound = sound || s->channels > 0;
        if (sound) openDevice();
    }

    // The screens as they are now: started again only when which ones play changed (one added, removed, hidden, shown,
    // or given another clip), so an unrelated edit neither restarts them nor decodes their sound again; otherwise their
    // audio-area settings are read again.
    void sync(HslRenderer& r, const HslScene& scene) {
        std::vector<std::pair<size_t, std::string>> now;
        for (size_t i = 0; i < r.meshCount(); ++i) {
            if (!r.meshIsVideo(i) || !r.meshVisible(i)) continue;
            const int sm = r.meshSceneIndex(i);
            if (sm >= 0 && static_cast<size_t>(sm) < scene.meshes.size()) now.push_back({i, scene.meshes[sm].video});
        }
        if (now != playing_) start(r, scene);
        else refreshSettings(scene);
    }

    void stop() {
        playing_.clear();
        if (deviceOn_) { ma_device_uninit(&device_); deviceOn_ = false; }
        for (auto& s : screens_) {
            { std::lock_guard<std::mutex> l(s->m); s->quit = true; }
            s->cv.notify_all();
            if (s->decoder.joinable()) s->decoder.join();
        }
        screens_.clear();
    }

    bool any() const { return !screens_.empty(); }
    size_t framesShown() const { return framesShown_; }   // new pictures given the renderer so far (tests)
    // How loud screen i is heard now (0..1, as it eases; tests).
    float heard(size_t i) const { return i < screens_.size() ? screens_[i]->target.load() : 0.0f; }
    size_t count() const { return screens_.size(); }

    // The scene's audio-area settings again (Frame Studio changed them).
    void refreshSettings(const HslScene& scene) {
        for (auto& s : screens_)
            if (static_cast<size_t>(s->sceneMesh) < scene.meshes.size()) s->settings(scene.meshes[s->sceneMesh]);
    }

    // Each frame: where you hear from (the head; right: its right, for which side a screen's sound leans to) and the
    // volume over everything (0..1; muted: silence). New frames go to the renderer.
    void update(HslRenderer& r, const float listener[3], const float right[3], float volume, bool muted) {
        if (screens_.empty()) return;
        master_.store(muted ? 0.0f : std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        for (auto& sp : screens_) {
            Screen& s = *sp;
            if (s.draw >= r.meshCount()) continue;
            if (!r.meshVisible(s.draw)) { s.target.store(0, std::memory_order_relaxed); continue; }   // (hidden or deleted: silent, not decoded)
            const float* w = r.meshWorld(s.draw);
            const float d[3] = {w[12] - listener[0], w[13] - listener[1], w[14] - listener[2]};
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float nearR = std::max(0.0f, s.nearR.load()), farR = std::max(nearR + 0.01f, s.farR.load());
            float t = std::clamp((farR - dist) / (farR - nearR), 0.0f, 1.0f);
            t = t * t * (3 - 2 * t);                                   // (smoothstep: no corner at either edge)
            s.target.store(t * std::clamp(s.volume.load(), 0.0f, 1.0f), std::memory_order_relaxed);
            const float side = dist > 1e-3f ? (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / dist : 0.0f;
            s.pan.store(std::clamp(side, -1.0f, 1.0f) * 0.6f, std::memory_order_relaxed);
            // the frame for now
            // (the sound is the clock only while a device plays it: without one, the picture keeps time by itself)
            const double at = s.channels && deviceOn_ ? s.cursor.load(std::memory_order_relaxed) / static_cast<double>(VideoClip::kRate) : std::fmod(now, std::max(1e-3, s.clip.seconds()));
            const uint32_t want = std::min(s.clip.frames - 1, static_cast<uint32_t>(at * s.clip.fps));
            {
                std::lock_guard<std::mutex> l(s.m);
                if (s.fresh) {
                    if (r.setVideoFrame(s.draw, s.ready)) ++framesShown_;   // (handed over: the decoder makes a new one)
                    s.shown = s.readyIndex;
                    s.fresh = false;
                }
                if (want != s.shown && want != s.want && !(s.shown < s.clip.frames && s.clip.sameFrame(want, s.shown))) {
                    s.want = want;
                    s.cv.notify_one();
                }
            }
        }
    }

private:
    struct Screen {
        VideoClip clip;
        size_t draw = 0;
        int sceneMesh = -1;
        std::vector<short> pcm;                 // the whole sound, interleaved
        uint32_t channels = 0;
        uint64_t loopSamples = 0;               // the clip's length (the sound loops with the picture)
        std::atomic<uint64_t> cursor{0};        // where the sound is (samples per channel), advanced by the device
        std::atomic<float> nearR{2}, farR{8}, volume{1}, target{0}, pan{0};
        float gain = 0;                         // the audio thread's own, easing towards target
        // the decoder
        std::thread decoder;
        std::mutex m;
        std::condition_variable cv;
        uint32_t want = UINT32_MAX, shown = UINT32_MAX, readyIndex = UINT32_MAX;
        std::vector<u8> ready;
        bool fresh = false, quit = false;

        void settings(const MeshData& md) { nearR = md.videoNear; farR = md.videoFar; volume = md.videoVolume; }

        void decodeLoop() {
            std::ifstream f(clip.path, std::ios::binary);
            std::vector<u8> rgba, jpeg;
            for (;;) {
                uint32_t i;
                {
                    std::unique_lock<std::mutex> l(m);
                    cv.wait(l, [&] { return quit || (want != UINT32_MAX && want != readyIndex); });
                    if (quit) return;
                    i = want;
                }
                const bool ok = f && clip.frame(f, i, rgba, jpeg);
                std::lock_guard<std::mutex> l(m);
                if (ok) { ready.swap(rgba); readyIndex = i; fresh = true; }
                else readyIndex = i;                                   // (a damaged frame: the one before stays up)
            }
        }
    };

    static void mix(ma_device* d, void* out, const void*, ma_uint32 frames) {
        VideoScreens* self = static_cast<VideoScreens*>(d->pUserData);
        float* o = static_cast<float*>(out);
        std::fill(o, o + static_cast<size_t>(frames) * 2, 0.0f);
        const float master = g_audioMuted.load(std::memory_order_relaxed) ? 0.0f : self->master_.load(std::memory_order_relaxed);
        // easing: about a second from silence to full (per sample: 1 - e^(-1 / (0.35 s * rate)))
        constexpr float kEase = 1.0f / (0.35f * VideoClip::kRate);
        for (auto& sp : self->screens_) {
            Screen& s = *sp;
            if (!s.channels || s.pcm.empty() || !s.loopSamples) continue;
            const float target = s.target.load(std::memory_order_relaxed) * master;
            const float pan = s.pan.load(std::memory_order_relaxed);
            const float lw = std::min(1.0f, 1.0f - pan), rw = std::min(1.0f, 1.0f + pan);
            uint64_t c = s.cursor.load(std::memory_order_relaxed);
            const size_t have = s.pcm.size() / s.channels;
            for (ma_uint32 k = 0; k < frames; ++k) {
                s.gain += (target - s.gain) * kEase;
                if (c >= s.loopSamples) c = 0;
                if (c < have && s.gain > 1e-4f) {
                    const short* p = &s.pcm[c * s.channels];
                    const float l = p[0] / 32768.0f, r = s.channels > 1 ? p[1] / 32768.0f : l;
                    o[k * 2] += l * s.gain * lw;
                    o[k * 2 + 1] += r * s.gain * rw;
                }
                ++c;
            }
            s.cursor.store(c, std::memory_order_relaxed);
        }
        for (ma_uint32 k = 0; k < frames * 2; ++k) o[k] = std::clamp(o[k], -1.0f, 1.0f);
    }

    void openDevice() {
        ma_device_config c = ma_device_config_init(ma_device_type_playback);
        c.playback.format = ma_format_f32;
        c.playback.channels = 2;
        c.sampleRate = VideoClip::kRate;
        c.dataCallback = mix;
        c.pUserData = this;
        if (ma_device_init(nullptr, &c, &device_) != MA_SUCCESS) { std::fprintf(stderr, "[video] no audio device: screens play silent\n"); return; }
        if (ma_device_start(&device_) != MA_SUCCESS) { ma_device_uninit(&device_); return; }
        deviceOn_ = true;
    }

    std::vector<std::unique_ptr<Screen>> screens_;
    std::vector<std::pair<size_t, std::string>> playing_;   // (draw index, clip) of each screen started
    ma_device device_{};
    bool deviceOn_ = false;
    std::atomic<float> master_{1};
    size_t framesShown_ = 0;
    std::chrono::steady_clock::time_point started_;
};
