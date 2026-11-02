#pragma once
// Frame Studio's Import Video: any video or moving picture made into a .fvid (src/video_clip.h) that Frame Home plays.
//   GIF                        stb_image's decoder (every platform), its frame timings kept
//   MP4, MOV, M4V, WMV, AVI,   Windows: Media Foundation (the codecs Windows has: H.264, HEVC with its extension,
//   MKV, WEBM, ...             VP9 / AV1 with theirs); anywhere, and when that can't read it: ffmpeg, if it's installed
// The picture is brought down to at most kMaxSide across (the longer side) and at most kMaxFps frames a second; the
// sound becomes 48 kHz, stereo or mono as it was. Runs on a worker thread; progress 0..1 and a stop flag come in.
#include "video_clip.h"
#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#undef interface                        // (objbase.h's macro: "interface" is an ordinary word in Studio)
#endif

namespace videoconvert {

constexpr uint32_t kMaxSide = 1280;     // 1280 x 720 for a 16:9 video: sharp on a wall-sized screen, ~2 MB/s
constexpr float kMaxFps = 30;
constexpr int kQuality = 82;

inline bool isVideoFile(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* x : {".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".wmv", ".mpg", ".mpeg", ".gif", ".apng", ".3gp", ".ogv", ".ts"})
        if (name.size() > std::strlen(x) && name.compare(name.size() - std::strlen(x), std::strlen(x), x) == 0) return true;
    return false;
}
inline const std::vector<std::string>& videoPatterns() {
    static const std::vector<std::string> p = {"*.mp4", "*.m4v", "*.mov", "*.webm", "*.mkv", "*.avi", "*.wmv", "*.mpg", "*.mpeg", "*.gif", "*.3gp", "*.ogv", "*.ts"};
    return p;
}

struct Progress {
    std::atomic<float> done{0};
    std::atomic<bool> stop{false};
};

namespace detail {

inline bool jpeg(const u8* rgba, uint32_t w, uint32_t h, int quality, std::vector<u8>& out) {
    out.clear();
    return stbi_write_jpg_to_func([](void* ctx, void* data, int size) {
        auto* o = static_cast<std::vector<u8>*>(ctx);
        o->insert(o->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    }, &out, static_cast<int>(w), static_cast<int>(h), 4, rgba, quality) != 0;
}

// The size it's made at: within kMaxSide, its shape kept, both sides even (the JPEG's chroma halves stay whole).
inline void targetSize(uint32_t w, uint32_t h, uint32_t& ow, uint32_t& oh) {
    const float s = std::min(1.0f, static_cast<float>(kMaxSide) / std::max(w, h));
    ow = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(w * s)) & ~1u);
    oh = std::max<uint32_t>(2, static_cast<uint32_t>(std::lround(h * s)) & ~1u);
}

// RGBA downscale by area (each output pixel the average of the source pixels it covers): no shimmer on fine detail.
inline void resize(const u8* src, uint32_t sw, uint32_t sh, size_t srcStride, std::vector<u8>& dst, uint32_t dw, uint32_t dh) {
    dst.resize(static_cast<size_t>(dw) * dh * 4);
    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; ++y) std::memcpy(&dst[static_cast<size_t>(y) * dw * 4], src + y * srcStride, static_cast<size_t>(dw) * 4);
        return;
    }
    const float fx = static_cast<float>(sw) / dw, fy = static_cast<float>(sh) / dh;
    for (uint32_t y = 0; y < dh; ++y) {
        const uint32_t y0 = static_cast<uint32_t>(y * fy), y1 = std::max(y0 + 1, std::min(sh, static_cast<uint32_t>((y + 1) * fy)));
        for (uint32_t x = 0; x < dw; ++x) {
            const uint32_t x0 = static_cast<uint32_t>(x * fx), x1 = std::max(x0 + 1, std::min(sw, static_cast<uint32_t>((x + 1) * fx)));
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t yy = y0; yy < y1; ++yy) {
                const u8* p = src + yy * srcStride + static_cast<size_t>(x0) * 4;
                for (uint32_t xx = x0; xx < x1; ++xx, p += 4) for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            u8* o = &dst[(static_cast<size_t>(y) * dw + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<u8>(sum[c] / n);
        }
    }
}

// Interleaved int16 from any rate and channel count to 48 kHz, 1 or 2 channels (linear: speech and music alike).
inline std::vector<short> to48k(const std::vector<short>& in, int rate, int channels, int& outChannels) {
    outChannels = std::min(channels, 2);
    std::vector<short> out;
    if (in.empty() || rate <= 0 || channels <= 0) return out;
    const size_t frames = in.size() / channels;
    const double step = static_cast<double>(rate) / VideoClip::kRate;
    const size_t n = static_cast<size_t>(frames / step);
    out.resize(n * outChannels);
    for (size_t i = 0; i < n; ++i) {
        const double at = i * step;
        const size_t a = std::min(frames - 1, static_cast<size_t>(at)), b = std::min(frames - 1, a + 1);
        const float t = static_cast<float>(at - a);
        for (int c = 0; c < outChannels; ++c) {
            // a 5.1 file's front left / right (and mono stays mono)
            const float va = in[a * channels + c], vb = in[b * channels + c];
            out[i * outChannels + c] = static_cast<short>(std::lround(va + (vb - va) * t));
        }
    }
    return out;
}

// Frames in order, JPEG-encoded a few at a time on every core, written in order.
struct FrameSink {
    VideoClipWriter& out;
    std::vector<std::vector<u8>> batch;
    bool lastWasRepeat = false;
    size_t written = 0;
    explicit FrameSink(VideoClipWriter& w) : out(w) {}
    std::vector<std::pair<bool, size_t>> order;   // (repeat of the frame before, index into batch)
    void add(const std::vector<u8>& rgba) { order.push_back({false, batch.size()}); batch.push_back(rgba); if (batch.size() >= 16) flush(); }
    void repeat() { order.push_back({true, 0}); if (order.size() >= 64) flush(); }
    bool flush() {
        std::vector<std::future<std::vector<u8>>> jobs;
        for (auto& f : batch)
            jobs.push_back(std::async(std::launch::async, [&f, this] {
                std::vector<u8> j;
                jpeg(f.data(), wOf(), hOf(), kQuality, j);
                return j;
            }));
        std::vector<std::vector<u8>> enc;
        for (auto& j : jobs) enc.push_back(j.get());
        bool ok = true;
        for (const auto& [rep, i] : order) ok = ok && (rep ? out.repeatFrame() : out.addJpeg(enc[i]));
        written += order.size();
        batch.clear();
        order.clear();
        return ok;
    }
    uint32_t w = 0, h = 0;
    uint32_t wOf() const { return w; }
    uint32_t hOf() const { return h; }
};

// ── GIF ──
inline bool fromGif(const std::string& in, const std::string& out, Progress& pr, std::string& err) {
    std::ifstream f(std::filesystem::u8path(in), std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.empty()) { err = "can't read " + in; return false; }
    int* delays = nullptr;
    int w = 0, h = 0, n = 0, comp = 0;
    u8* px = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()), &delays, &w, &h, &n, &comp, 4);
    if (!px || n <= 0) { err = "not a GIF that reads: " + in; return false; }
    // browsers' rule: a delay of 10 ms or less is shown as 100 ms
    std::vector<double> at(static_cast<size_t>(n) + 1, 0);
    int shortest = 100000;
    for (int i = 0; i < n; ++i) {
        const int d = delays && delays[i] > 10 ? delays[i] : 100;
        shortest = std::min(shortest, d);
        at[i + 1] = at[i] + d / 1000.0;
    }
    const float fps = n == 1 ? 1.0f : std::clamp(1000.0f / shortest, 1.0f, kMaxFps);
    uint32_t ow, oh;
    targetSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h), ow, oh);
    VideoClipWriter wr;
    wr.encodeJpeg = jpeg;
    if (!wr.begin(out, ow, oh, fps)) { stbi_image_free(px); stbi_image_free(delays); err = "can't write " + out; return false; }
    FrameSink sink(wr);
    sink.w = ow; sink.h = oh;
    const size_t frames = std::max<size_t>(1, static_cast<size_t>(std::lround(at[n] * fps)));
    int prev = -1;
    std::vector<u8> scaled;
    for (size_t k = 0; k < frames && !pr.stop; ++k) {
        const double t = (k + 0.5) / fps;
        int g = static_cast<int>(std::upper_bound(at.begin(), at.end(), t) - at.begin()) - 1;
        g = std::clamp(g, 0, n - 1);
        if (g == prev) sink.repeat();
        else {
            resize(px + static_cast<size_t>(g) * w * h * 4, static_cast<uint32_t>(w), static_cast<uint32_t>(h), static_cast<size_t>(w) * 4, scaled, ow, oh);
            for (size_t p = 3; p < scaled.size(); p += 4) scaled[p] = 255;   // (a screen shows no holes)
            sink.add(scaled);
        }
        prev = g;
        pr.done = static_cast<float>(k + 1) / frames;
    }
    stbi_image_free(px);
    stbi_image_free(delays);
    if (pr.stop) { err = "stopped"; return false; }
    if (!sink.flush() || !wr.finish()) { err = "can't write " + out; return false; }
    return true;
}

// ── A decoder that gives frames as they come (time in seconds, RGBA top-down), and the sound at the end ──
struct Decoded {
    uint32_t w = 0, h = 0;
    float fps = 30;
    double duration = 0;
    std::vector<short> sound;   // interleaved
    int rate = 0, channels = 0;
};

// Turns the source's frames (any timing) into kMaxFps-or-less at a steady rate: each output tick takes the latest
// source frame shown by then; a run of ticks on one source frame stores it once.
struct Retimer {
    FrameSink& sink;
    float fps;
    uint32_t ow, oh;
    std::vector<u8> last, scaled;
    bool any = false, lastWritten = false;
    size_t tick = 0;
    Retimer(FrameSink& s, float f, uint32_t w, uint32_t h) : sink(s), fps(f), ow(w), oh(h) {}
    void frame(double t, const u8* rgba, uint32_t w, uint32_t h, size_t stride) {
        while (any && (tick + 0.5) / fps < t) emit();
        resize(rgba, w, h, stride, last, ow, oh);
        any = true;
        lastWritten = false;
    }
    void emit() {
        if (lastWritten) sink.repeat(); else { sink.add(last); lastWritten = true; }
        ++tick;
    }
    void end(double duration) {
        if (!any) return;
        const size_t want = std::max<size_t>(tick + 1, static_cast<size_t>(std::lround(duration * fps)));
        while (tick < want) emit();
    }
};

#ifdef _WIN32
template <typename T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

inline bool fromMediaFoundation(const std::string& in, const std::string& out, Progress& pr, std::string& err) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));   // (the worker's own thread)
    struct Uninit { bool on; ~Uninit() { if (on) CoUninitialize(); } } uninit{com};
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { err = "Media Foundation isn't available"; return false; }
    struct Shutdown { ~Shutdown() { MFShutdown(); } } shutdown;
    Com<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // decodes to RGB32 for us
    const int wn = MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(wn, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.c_str(), -1, wpath.data(), wn);
    Com<IMFSourceReader> rd;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), attr.p, &rd))) { err = "Windows can't read this video (no codec for it?)"; return false; }
    const DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), kAudio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    {
        Com<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(rd->SetCurrentMediaType(kVideo, nullptr, t.p))) { err = "Windows can't decode this video's picture (no codec for it?)"; return false; }
    }
    bool sound = true;
    {
        Com<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        sound = SUCCEEDED(rd->SetCurrentMediaType(kAudio, nullptr, t.p));
        rd->SetStreamSelection(kAudio, sound ? TRUE : FALSE);
    }
    rd->SetStreamSelection(kVideo, TRUE);
    UINT32 w = 0, h = 0, num = 30, den = 1;
    LONG stride = 0;
    MFVideoArea aperture{};
    bool cropped = false;
    {
        Com<IMFMediaType> t;
        rd->GetCurrentMediaType(kVideo, &t);
        MFGetAttributeSize(t.p, MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.p, MF_MT_FRAME_RATE, &num, &den);
        UINT32 s = 0;
        if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(s);
        else stride = static_cast<LONG>(w * 4);
        UINT32 size = 0;
        cropped = SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof aperture, &size));
    }
    int rate = 0, channels = 0;
    if (sound) {
        Com<IMFMediaType> t;
        rd->GetCurrentMediaType(kAudio, &t);
        UINT32 v = 0;
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v); rate = static_cast<int>(v);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v); channels = static_cast<int>(v);
        sound = rate > 0 && channels > 0;
    }
    double duration = 0;
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(rd->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var))) duration = var.uhVal.QuadPart / 1e7;
        PropVariantClear(&var);
    }
    if (!w || !h) { err = "this video has no picture size"; return false; }
    // what's shown of the decoded picture (a 1080 line video decodes as 1088)
    uint32_t cx = 0, cy = 0, cw = w, ch = h;
    if (cropped && aperture.Area.cx > 0 && aperture.Area.cy > 0) {
        cx = static_cast<uint32_t>(std::max(0, static_cast<int>(aperture.OffsetX.value)));
        cy = static_cast<uint32_t>(std::max(0, static_cast<int>(aperture.OffsetY.value)));
        cw = std::min<uint32_t>(w - cx, static_cast<uint32_t>(aperture.Area.cx));
        ch = std::min<uint32_t>(h - cy, static_cast<uint32_t>(aperture.Area.cy));
    }
    const float fps = std::clamp(den ? static_cast<float>(num) / den : 30.0f, 1.0f, kMaxFps);
    uint32_t ow, oh;
    targetSize(cw, ch, ow, oh);
    VideoClipWriter wr;
    wr.encodeJpeg = jpeg;
    if (!wr.begin(out, ow, oh, fps)) { err = "can't write " + out; return false; }
    FrameSink sink(wr);
    sink.w = ow; sink.h = oh;
    Retimer rt(sink, fps, ow, oh);
    std::vector<short> pcm;
    std::vector<u8> rgba(static_cast<size_t>(cw) * ch * 4);
    bool videoDone = false, audioDone = !sound;
    double lastT = 0;
    while (!(videoDone && audioDone) && !pr.stop) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        Com<IMFSample> sample;
        if (FAILED(rd->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &ts, &sample))) break;
        Com<IMFMediaType> nt;
        GUID major{};
        if (SUCCEEDED(rd->GetCurrentMediaType(stream, &nt))) nt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {           // that stream is done (the other may run on)
            rd->SetStreamSelection(stream, FALSE);
            if (major == MFMediaType_Video) videoDone = true; else audioDone = true;
            if (!sample) continue;
        }
        if (flags & MF_SOURCE_READERF_ERROR) break;
        if (!sample) continue;
        Com<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len))) continue;
        if (major == MFMediaType_Audio) {
            pcm.insert(pcm.end(), reinterpret_cast<short*>(data), reinterpret_cast<short*>(data) + len / 2);
        } else if (major == MFMediaType_Video) {
            // RGB32 is BGRX; a negative stride is bottom-up
            const LONG absStride = std::abs(stride);
            if (static_cast<size_t>(absStride) * h <= len) {
                for (uint32_t y = 0; y < ch; ++y) {
                    const uint32_t sy = stride < 0 ? h - 1 - (cy + y) : cy + y;
                    const BYTE* row = data + static_cast<size_t>(sy) * absStride + static_cast<size_t>(cx) * 4;
                    u8* o = &rgba[static_cast<size_t>(y) * cw * 4];
                    for (uint32_t x = 0; x < cw; ++x) { o[x * 4] = row[x * 4 + 2]; o[x * 4 + 1] = row[x * 4 + 1]; o[x * 4 + 2] = row[x * 4]; o[x * 4 + 3] = 255; }
                }
                lastT = ts / 1e7;
                rt.frame(lastT, rgba.data(), cw, ch, static_cast<size_t>(cw) * 4);
                if (duration > 0) pr.done = static_cast<float>(std::min(0.95, lastT / duration));
            }
        }
        buf->Unlock();
    }
    if (pr.stop) { err = "stopped"; return false; }
    if (!rt.any) { err = "no frames came out of this video"; return false; }
    rt.end(std::max(duration, lastT + 1.0 / fps));
    if (!sink.flush()) { err = "can't write " + out; return false; }
    int oc = 0;
    const std::vector<short> s48 = to48k(pcm, rate, channels, oc);
    if (!s48.empty() && !wr.addSound(s48.data(), s48.size() / oc, oc)) { err = "the sound couldn't be packed"; return false; }
    if (!wr.finish()) { err = "can't write " + out; return false; }
    return true;
}
#endif

// ── ffmpeg, when it's installed (on PATH) ──
inline FILE* openPipe(const std::string& cmd) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, w.data(), n);
    return _wpopen(w.c_str(), L"rb");
#else
    return popen(cmd.c_str(), "r");
#endif
}
inline int closePipe(FILE* f) {
#ifdef _WIN32
    return _pclose(f);
#else
    return pclose(f);
#endif
}
inline std::string shellQuoted(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    return o + "'";
#endif
}
inline std::string readAll(const std::string& cmd) {
    std::string o;
    if (FILE* f = openPipe(cmd)) {
        char b[4096];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) o.append(b, n);
        closePipe(f);
    }
    return o;
}

inline bool fromFfmpeg(const std::string& in0, const std::string& out, Progress& pr, std::string& err) {
    std::string in = in0;
#ifdef _WIN32
    // cmd.exe expands %NAME% even inside quotes: a file named so is read through a copy with a plain name
    struct Copy { std::filesystem::path p; ~Copy() { std::error_code ec; if (!p.empty()) std::filesystem::remove(p, ec); } } copy;
    if (in.find('%') != std::string::npos) {
        std::error_code ec;
        copy.p = std::filesystem::temp_directory_path(ec) / ("frame-video-import" + std::filesystem::u8path(in).extension().u8string());
        std::filesystem::copy_file(std::filesystem::u8path(in), copy.p, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { copy.p.clear(); err = "can't read " + in0; return false; }
        in = copy.p.u8string();
    }
    const std::string quiet = " 2>NUL";
#else
    const std::string quiet = " 2>/dev/null";
#endif
    // its picture's size, frame rate and length
    const std::string probe = readAll("ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate:format=duration -of default=nw=1 " +
                                      shellQuoted(in) + quiet);
    auto field = [&](const char* k) {
        const size_t at = probe.find(std::string(k) + "=");
        return at == std::string::npos ? std::string() : probe.substr(at + std::strlen(k) + 1, probe.find('\n', at) - at - std::strlen(k) - 1);
    };
    const uint32_t w = static_cast<uint32_t>(std::atoi(field("width").c_str())), h = static_cast<uint32_t>(std::atoi(field("height").c_str()));
    if (!w || !h) { err = "this video doesn't read (and ffmpeg isn't installed or can't read it either)"; return false; }
    float fps = 30;
    {
        const std::string r = field("avg_frame_rate");
        const size_t s = r.find('/');
        const double a = std::atof(r.c_str()), b = s == std::string::npos ? 1 : std::atof(r.c_str() + s + 1);
        if (a > 0 && b > 0) fps = static_cast<float>(a / b);
    }
    fps = std::clamp(fps, 1.0f, kMaxFps);
    const double duration = std::atof(field("duration").c_str());
    uint32_t ow, oh;
    targetSize(w, h, ow, oh);
    VideoClipWriter wr;
    wr.encodeJpeg = jpeg;
    if (!wr.begin(out, ow, oh, fps)) { err = "can't write " + out; return false; }
    FrameSink sink(wr);
    sink.w = ow; sink.h = oh;
    char vf[128];
    std::snprintf(vf, sizeof vf, "fps=%.4f,scale=%u:%u:flags=area", fps, ow, oh);
    FILE* v = openPipe("ffmpeg -v error -i " + shellQuoted(in) + " -an -vf " + vf + " -f rawvideo -pix_fmt rgba -" + quiet);
    if (!v) { err = "ffmpeg didn't start"; return false; }
    std::vector<u8> frame(static_cast<size_t>(ow) * oh * 4), prev;
    size_t k = 0;
    const size_t expect = duration > 0 ? static_cast<size_t>(duration * fps) : 0;
    while (!pr.stop && std::fread(frame.data(), 1, frame.size(), v) == frame.size()) {
        if (frame == prev) sink.repeat(); else { sink.add(frame); prev = frame; }
        ++k;
        if (expect) pr.done = static_cast<float>(std::min(0.95, static_cast<double>(k) / expect));
    }
    closePipe(v);
    if (pr.stop) { err = "stopped"; return false; }
    if (!k) { err = "ffmpeg gave no frames for this video"; return false; }
    if (!sink.flush()) { err = "can't write " + out; return false; }
    std::vector<short> pcm;
    if (FILE* a = openPipe("ffmpeg -v error -i " + shellQuoted(in) + " -vn -f s16le -ac 2 -ar 48000 -" + quiet)) {
        short b[4096];
        size_t n;
        while ((n = std::fread(b, 2, 4096, a)) > 0) pcm.insert(pcm.end(), b, b + n);
        closePipe(a);
    }
    if (!pcm.empty() && !wr.addSound(pcm.data(), pcm.size() / 2, 2)) { err = "the sound couldn't be packed"; return false; }
    if (!wr.finish()) { err = "can't write " + out; return false; }
    return true;
}

}  // namespace detail

// Makes `out` (.fvid) from `in`. False with `err` (and no `out`) when it can't.
inline bool convert(const std::string& in, const std::string& out, Progress& pr, std::string& err) {
    std::string ext = std::filesystem::u8path(in).extension().u8string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool ok = false;
    if (ext == ".gif") ok = detail::fromGif(in, out, pr, err);
    else {
#ifdef _WIN32
        ok = detail::fromMediaFoundation(in, out, pr, err);
        if (!ok && !pr.stop) {
            std::string e2;
            pr.done = 0;
            ok = detail::fromFfmpeg(in, out, pr, e2);
            if (!ok && e2.find("isn't installed") == std::string::npos) err = e2;
        }
#else
        ok = detail::fromFfmpeg(in, out, pr, err);
#endif
    }
    std::error_code ec;
    if (!ok) std::filesystem::remove(std::filesystem::u8path(out), ec);
    pr.done = 1;
    return ok;
}

// A sound (anything AudioPlayer::decode reads: Ogg, Opus, WAV, MP3, FLAC) put on a clip that has none (a GIF), or in
// place of its own: `out` is `in` with that sound. decode: the caller's decoder (interleaved int16, its rate).
using SoundDecoder = std::function<bool(const std::vector<u8>& bytes, std::vector<short>& pcm, int& channels, int& rate)>;
inline bool withSound(const std::string& in, const std::string& soundFile, const std::string& out, const SoundDecoder& decode, std::string& err) {
    VideoClip clip;
    if (!clip.open(in, &err)) return false;
    std::ifstream sf(std::filesystem::u8path(soundFile), std::ios::binary);
    const std::vector<u8> bytes((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
    std::vector<short> pcm;
    int channels = 0, rate = 0;
    if (bytes.empty() || !decode(bytes, pcm, channels, rate) || pcm.empty()) { err = "that sound doesn't decode: " + soundFile; return false; }
    int oc = 0;
    std::vector<short> s48 = detail::to48k(pcm, rate, channels, oc);
    // the clip loops with the picture: the sound is made the picture's length (cut, or repeated to fill it)
    const size_t want = static_cast<size_t>(std::llround(clip.seconds() * VideoClip::kRate));
    if (want && !s48.empty()) {
        std::vector<short> fit(want * oc);
        const size_t have = s48.size() / oc;
        for (size_t i = 0; i < want; ++i) for (int c = 0; c < oc; ++c) fit[i * oc + c] = s48[(i % have) * oc + c];
        s48.swap(fit);
    }
    VideoClipWriter wr;
    wr.encodeJpeg = detail::jpeg;
    if (!wr.begin(out, clip.width, clip.height, clip.fps)) { err = "can't write " + out; return false; }
    std::ifstream f(std::filesystem::u8path(in), std::ios::binary);
    std::vector<u8> jpeg;
    for (uint32_t i = 0; i < clip.frames; ++i) {
        if (i && clip.sameFrame(i, i - 1)) { wr.repeatFrame(); continue; }
        if (!clip.frameBytes(f, i, jpeg) || !wr.addJpeg(jpeg)) { err = "can't copy the picture of " + in; return false; }
    }
    if (!wr.addSound(s48.data(), s48.size() / oc, oc) || !wr.finish()) { err = "can't write " + out; return false; }
    return true;
}

}  // namespace videoconvert
