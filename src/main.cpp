// Frame Home — the Steam Frame's environment, rendered from Meta HSL homes with each home's own shaders.
//
//   frame-home                                          environment mode: shows the home named in
//                                                       ~/frame-home/current, reloading live when it,
//                                                       the home or its edits change
//   frame-home <home.apk>                               run one home in SteamVR (scene application)
//   frame-home --install | --uninstall                  make Frame Home SteamVR's environment (home app)
//                                                       or give the environment back to SteamVR
//   frame-home --snapshot out.ppm [--cam x,y,z,yaw,pitch] [--size N] <home.apk>
//                                                       headless render of one eye (no SteamVR)
//   frame-home --register <frame-home.vrmanifest>       add an app manifest to SteamVR
#include "debug_geom.h"
#include "home_edits.h"
#include "home_report.h"
#include "hsl_renderer.h"
#include "hsl_scene.h"
#include "scene_bake.h"
#include "locomotion.h"
#include "panel_dock.h"
#include "panorama.h"
#include "core/audio.h"
#include "env_export.h"
#include "perf_settings.h"
#include "thermal.h"
#include "fenv.h"
#include "home_platform.h"
#include "stb_image_write.h"

#include <openvr.h>


#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define LOG(...)  do { std::fprintf(stderr, "[frame-home] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

std::atomic<bool> g_audioMuted{false};   // core/audio.h: the ambience plays silence while set
float g_volume = 1.0f;                   // Settings: all of the home's sound (times each sound's own volume)

namespace {

constexpr float kNear = 0.05f;

// Reversed-Z, infinite-far Vulkan projection (y-down) from view-space tangents (x right, y up).
Mat4 projection(float l, float r, float b, float t) {
    Mat4 m = {};
    m.m[0] = 2.0f / (r - l);
    m.m[8] = (r + l) / (r - l);
    m.m[5] = -2.0f / (t - b);
    m.m[9] = -(t + b) / (t - b);
    m.m[14] = kNear;
    m.m[11] = -1.0f;
    return m;
}

// Column-major rigid transform -> inverse.
Mat4 rigidInverse(const Mat4& a) {
    Mat4 r = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; ++i)
        r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}

Mat4 fromHmd34(const vr::HmdMatrix34_t& h) {
    Mat4 r = {};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) r.m[col * 4 + row] = h.m[row][col];
    r.m[15] = 1;
    return r;
}

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    mat4mul4(a.m, b.m, r.m);
    return r;
}

// Eye view from a world pose (camera-to-world).
EyeView eyeFromPose(const Mat4& cameraToWorld, const Mat4& proj) {
    EyeView e;
    e.proj = proj;
    e.view = rigidInverse(cameraToWorld);
    for (int k = 0; k < 3; ++k) e.position[k] = cameraToWorld.m[12 + k];
    return e;
}

std::vector<std::string> split(const char* s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

namespace fs = std::filesystem;

// SteamVR's home-app slot: the app it keeps running as the world whenever no game is open.
constexpr const char* kHomeAppKey = "openvr.tool.steamvr_environments";

fs::path rootDir() { return homeos::root(); }   // (home_platform.h: ~/frame-home, or a PC's)

std::string readText(const fs::path& p) {
    std::ifstream f(p);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// The home environment mode shows: ~/frame-home/current lists its APKs in envs/, one per line (footprint,
// then vista); else the first home there.
std::vector<std::string> currentHome() {
    const fs::path envs = homeos::envs();
    std::vector<std::string> home;
    std::istringstream lines(readText(rootDir() / "current"));
    std::string name;
    std::error_code ec;
    while (std::getline(lines, name))
        if (!name.empty() && fs::exists(envs / name, ec)) home.push_back((envs / name).string());
    if (!home.empty()) return home;
    std::vector<std::string> all;
    for (const auto& e : fs::directory_iterator(envs, ec))
        if (e.path().extension() == ".apk" || e.path().extension() == ".fenv") all.push_back(e.path().string());
    std::sort(all.begin(), all.end());
    if (!all.empty()) home.push_back(all.front());
    return home;
}

// Modification stamp of a file ("-" when missing); polled to notice new homes and new edits.
std::string stamp(const std::string& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? std::string("-") : std::to_string(static_cast<long long>(t.time_since_epoch().count()));
}

// Every mesh back to the transform and visibility the home shipped with.
void resetMeshes(HslRenderer& r, const HslScene& scene) {
    for (size_t i = 0; i < r.meshCount(); ++i) {
        const MeshData& md = scene.meshes[r.meshSceneIndex(i)];
        float w[16];
        if (md.hasWorldMatrix) std::memcpy(w, md.worldMatrix, 64);
        else trsToMat4(md.transform, w);
        r.setMeshWorld(i, w);
        r.setMeshVisible(i, true);
    }
}

// Frame Studio's edits for a home: "<home>.edits.json" next to it wins; a .fenv carries its own (used when there are
// none beside it). path: the file read.
bool homeEditsFor(const HslScene& scene, const std::string& apk, HomeEdits& edits, std::string& path) {
    if (loadHomeEdits(editsPathFor(apk), edits)) { path = editsPathFor(apk); return true; }
    if (!scene.fenvEdits.empty() && loadHomeEdits(scene.fenvEdits, edits)) { path = scene.fenvEdits; return true; }
    edits = HomeEdits();
    path.clear();
    return false;
}

// Applies Frame Studio's "<home>.edits.json" (moved / deleted objects), when the home has one.
void applyEdits(HslRenderer& r, const HslScene& scene, const std::string& apk) {
    HomeEdits edits;
    std::string path;
    if (!homeEditsFor(scene, apk, edits, path)) return;
    size_t applied = 0;
    for (const ObjectEdit& e : edits.objects) {
        if (e.index < 0 || static_cast<size_t>(e.index) >= scene.meshes.size() || scene.meshes[e.index].name != e.name) continue;
        for (size_t i = 0; i < r.meshCount(); ++i) {
            if (r.meshSceneIndex(i) != e.index) continue;
            r.setMeshWorld(i, e.world);
            r.setMeshVisible(i, !e.deleted);
            ++applied;
        }
    }
    LOG("applied %zu of %zu object edits", applied, edits.objects.size());
}

// ── Sound: the home's own ambience and a background sound added in Frame Studio ──────────────────
// Decoded off the frame loop (a minute of Opus or MP3 is tens of milliseconds); the loop only starts the players.

struct LoopSound {
    std::vector<short> pcm;
    int chs = 0, sr = 0;
    bool any() const { return chs > 0 && !pcm.empty(); }
};

struct HomeSounds {
    LoopSound home, added;              // home: decoded only when it plays (homeVolume > 0)
    HomeSound settings;
    std::string addedPath;              // the added sound's file ("" none)
};

// homeAmbience: the home's own bed (HslScene::ambientOgg), copied by the caller when this runs beside the frame loop.
HomeSounds loadSounds(const std::vector<u8>& homeAmbience, const HomeEdits& edits, const std::string& editsPath) {
    HomeSounds hs;
    hs.settings = edits.sound;
    auto decode = [](const std::vector<u8>& bytes, LoopSound& out, const char* what) {
        const char* fmt = "sound";
        if (bytes.empty()) return;
        if (!AudioPlayer::decode(bytes.data(), bytes.size(), out.pcm, out.chs, out.sr, &fmt)) { LOG("%s: %s doesn't decode", what, fmt); out = LoopSound(); }
    };
    if (hs.settings.homeVolume > 0) decode(homeAmbience, hs.home, "the home's ambience");
    hs.addedPath = soundPathFor(editsPath, hs.settings);
    if (!hs.addedPath.empty()) {
        std::ifstream f(hs.addedPath, std::ios::binary);
        const std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (bytes.empty()) LOG("background sound %s: missing", hs.addedPath.c_str());
        decode(bytes, hs.added, "the background sound");
    }
    return hs;
}

void playSounds(AudioPlayer& home, AudioPlayer& added, const HomeSounds& hs) {
    home.stop();
    added.stop();
    if (hs.home.any()) {
        home.gain = hs.settings.homeVolume * g_volume;
        home.startPCM(hs.home.pcm.data(), hs.home.pcm.size() / hs.home.chs, hs.home.chs, hs.home.sr);
    }
    if (hs.added.any()) {
        added.gain = hs.settings.volume * g_volume;
        added.startPCM(hs.added.pcm.data(), hs.added.pcm.size() / hs.added.chs, hs.added.chs, hs.added.sr, false);   // as it was made
        LOG("background sound %s at %.0f%% (the home's own at %.0f%%)", hs.addedPath.c_str(), hs.settings.volume * 100, hs.settings.homeVolume * 100);
    }
}

// ── Frame timing ────────────────────────────────────────────────────────────────────────────────

// Running min / avg / max / percentile over a window of samples (ms).
struct Window {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    size_t n() const { return v.size(); }
    double avg() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
    double max() const { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }
    double pct(double p) const {
        if (v.empty()) return 0;
        std::vector<double> s = v;
        const size_t k = std::min(s.size() - 1, static_cast<size_t>(p * s.size()));
        std::nth_element(s.begin(), s.begin() + k, s.end());
        return s[k];
    }
    double stddev() const {
        if (v.size() < 2) return 0;
        const double a = avg();
        double s = 0;
        for (double x : v) s += (x - a) * (x - a);
        return std::sqrt(s / (v.size() - 1));
    }
    void clear() { v.clear(); }
};

// FH_PROFILE=1: one line a second of where each frame's time went (CPU phases, GPU, publish cadence).
struct FrameProfile {
    bool on = std::getenv("FH_PROFILE") != nullptr;
    Window pre, cpu, wait, gpu, gpuCopy, loop, anim, dyn, upload, record, publishGap, lead, sleep;
    uint32_t frames = 0, uploads = 0, late = 0, dynRebuilt = 0, dynMeshes = 0, draws = 0, binds = 0;
    uint64_t lastPublish = 0;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    void frame(const HslRenderer& r, double loopMs, double animMs, double gpuMs, bool gpuNew) {
        if (!on) return;
        const HslRenderer::FrameStats& s = r.lastStats();
        ++frames;
        cpu.add(s.totalMs);
        wait.add(s.waitMs);
        loop.add(loopMs);
        anim.add(animMs + s.animMs);
        dyn.add(s.dynamicMs);
        upload.add(s.uploadMs);
        record.add(s.recordMs);
        uploads += s.uploads;
        dynRebuilt += s.dynamicRebuilt;
        dynMeshes = s.dynamicMeshes;
        draws = s.draws;
        binds = s.pipelineBinds;
        if (gpuNew) { gpu.add(gpuMs); gpuCopy.add(r.lastGpuCopyMs()); }
    }
    void published(uint64_t ns, double leadMs, bool wasLate) {
        if (!on) return;
        if (lastPublish) publishGap.add((ns - lastPublish) / 1e6);
        lastPublish = ns;
        lead.add(leadMs);
        late += wasLate ? 1 : 0;
    }
    void report(const char* tag) {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - since).count();
        if (secs < 1.0) return;
        std::fprintf(stderr,
                     "[profile] %s %.0f fps | loop %.2f/%.2f | pre %.2f/%.2f | render %.2f/%.2f (wait %.2f/%.2f, anim %.2f/%.2f, dyn %.2f [%u of %u rebuilt], "
                     "uploads %u %.2f/%.2f, record %.2f/%.2f, %u draws %u pipelines) | gpu %.2f/%.2f/%.2f (copy %.2f) | publish gap %.2f/%.2f sd %.2f, lead %.2f/%.2f, "
                     "late %u (avg/max, gpu avg/p95/max, ms)\n",
                     tag, frames / secs, loop.avg(), loop.max(), pre.avg(), pre.max(), cpu.avg(), cpu.max(), wait.avg(), wait.max(), anim.avg(), anim.max(), dyn.avg(),
                     dynRebuilt, dynMeshes * frames, uploads, upload.avg(), upload.max(), record.avg(), record.max(), draws, binds, gpu.avg(),
                     gpu.pct(0.95), gpu.max(), gpuCopy.avg(), publishGap.avg(), publishGap.max(), publishGap.stddev(), lead.avg(), lead.max(), late);
        for (Window* w : {&pre, &cpu, &wait, &gpu, &gpuCopy, &loop, &anim, &dyn, &upload, &record, &publishGap, &lead, &sleep}) w->clear();
        frames = uploads = late = dynRebuilt = 0;
        since = now;
    }
};

// System mode pacing: one frame for every frame the compositor draws, finished just before it samples ours.
// The layer stamps sampledNs whenever it draws the home; that moment's offset from vsync (the phase) is tracked, and
// each frame starts `lead` (how long a frame takes to make, plus a margin) before the next sample.
struct SystemPacer {
    double period = 1.0 / 90;                  // s
    double phaseNs = -1;                       // compositor sample time after vsync (-1: not seen yet)
    uint64_t lastSampleSeen = 0, lastTarget = 0;
    std::vector<double> leads;                 // recent frame make times (ms): start-to-publish
    size_t leadAt = 0;
    uint64_t vsyncNs = 0;                      // a recent vsync
    static constexpr size_t kLeads = 240;      // two seconds at 120 Hz
    double pct = 0.98, marginMs = 1.0;
    // The lead to start with: a high percentile of recent make times + margin. More than a refresh (a frame that
    // can't be made in one) aims two samples ahead: frames overlap (render() waits for the one before) and every
    // compositor frame still gets a new one, a refresh later.
    double leadMs() const {
        if (leads.empty()) return 6.0;
        std::vector<double> s = leads;
        const size_t k = std::min(s.size() - 1, static_cast<size_t>(s.size() * pct));
        std::nth_element(s.begin(), s.begin() + k, s.end());
        return std::clamp(s[k] + marginMs, 2.0, period * 1000.0 * 1.9);
    }
    void addLead(double ms) { if (leads.size() < kLeads) leads.push_back(ms); else leads[leadAt++ % kLeads] = ms; }
    // Learns the compositor's phase from the layer's stamps.
    void observe(uint64_t sampledNs) {
        if (!vsyncNs || !sampledNs || sampledNs == lastSampleSeen) return;
        lastSampleSeen = sampledNs;
        const double p = period * 1e9;
        double ph = std::fmod(static_cast<double>(static_cast<int64_t>(sampledNs - vsyncNs)), p);
        if (ph < 0) ph += p;
        if (phaseNs < 0) { phaseNs = ph; return; }
        double d = ph - phaseNs;                // unwrapped toward the estimate
        if (d > p / 2) d -= p;
        if (d < -p / 2) d += p;
        // Late stamps (the last of a frame's draws) only ever add: follow quickly down, slowly up.
        phaseNs += d < 0 ? d * 0.5 : d * 0.05;
        phaseNs = std::fmod(phaseNs + p, p);
    }
    // When to start the next frame, the compositor sample it aims at, and how far ahead (s) to predict the pose.
    void plan(uint64_t now, float vsyncToPhotons, uint64_t& start, uint64_t& target, float& predict) {
        const double p = period * 1e9, lead = leadMs() * 1e6;
        if (phaseNs < 0 || !vsyncNs) {         // nothing seen yet: once a refresh, aimed two refreshes ahead
            start = std::max(now, lastTarget + static_cast<uint64_t>(p));
            target = start + static_cast<uint64_t>(p);
            lastTarget = start;
            predict = static_cast<float>(2 * period) + vsyncToPhotons;
            return;
        }
        // The first sample far enough ahead to make a frame for, and never the same one twice.
        double t = static_cast<double>(vsyncNs) + phaseNs;
        while (t < static_cast<double>(now) + lead) t += p;
        while (lastTarget && t < static_cast<double>(lastTarget) + p * 0.5) t += p;
        target = static_cast<uint64_t>(t);
        start = target - static_cast<uint64_t>(lead);
        lastTarget = target;
        // Its compositor frame scans out at the vsync after the sample; light leaves the display vsyncToPhotons later.
        predict = static_cast<float>((target - std::min(start, target)) / 1e9 + (p - phaseNs) / 1e9) + vsyncToPhotons;
    }
};

// System mode: publishes each frame to the compositor layer the moment the GPU has finished it, from a thread of its
// own, so the next frame's CPU work overlaps this one's GPU work and no frame waits a refresh to go out.
struct Publisher {
    struct Job { uint64_t serial; int slot; float vp[2][16]; float eye[2][3]; uint64_t startNs, target; };
    struct Done { uint64_t ns; double leadMs; bool late; };
    HslRenderer* r = nullptr;
    EnvExport* env = nullptr;
    std::thread th;
    std::mutex m;
    std::condition_variable cv, idle;
    std::deque<Job> jobs;
    std::vector<Done> done;
    bool stop = false, working = false;
    void start(HslRenderer& rr, EnvExport& e) {
        r = &rr;
        env = &e;
        th = std::thread([this] {
            std::unique_lock<std::mutex> l(m);
            for (;;) {
                cv.wait(l, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) return;            // stopping, nothing left
                const Job j = jobs.front();
                working = true;
                l.unlock();
                while (!r->waitFrameSerial(j.serial, 100000000ull)) { std::lock_guard<std::mutex> g(m); if (stop) break; }
                env->publish(j.slot, j.vp, j.eye);
                const uint64_t now = envshare::nowNs();
                l.lock();
                jobs.pop_front();
                working = false;
                done.push_back({now, (now - j.startNs) / 1e6, j.target && now > j.target});
                if (jobs.empty()) idle.notify_all();
            }
        });
    }
    void push(const Job& j) { { std::lock_guard<std::mutex> l(m); jobs.push_back(j); } cv.notify_one(); }
    // Every queued frame out (before the images change: a resize, or shutting down).
    void drain() { std::unique_lock<std::mutex> l(m); idle.wait(l, [&] { return jobs.empty() && !working; }); }
    std::vector<Done> take() { std::lock_guard<std::mutex> l(m); std::vector<Done> d; d.swap(done); return d; }
    void finish() {
        if (!th.joinable()) return;
        { std::lock_guard<std::mutex> l(m); stop = true; }
        cv.notify_all();
        th.join();
    }
};

void sleepUntilNs(uint64_t t) {
    if (t <= envshare::nowNs()) return;
    timespec ts;
    ts.tv_sec = static_cast<time_t>(t / 1000000000ull);
    ts.tv_nsec = static_cast<long>(t % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

// ── Headless snapshot (no SteamVR) ──────────────────────────────────────────────────────────────

// Where Frame Home starts you: the first spawn, eyes 1.6 m over its floor, facing its way (cam: x, y, z, yaw; the
// pitch is left as given).
void spawnView(const HslScene& scene, float cam[5]) {
    Locomotion loco;
    loco.setWorld(walkWorld(scene));
    float rig[16];
    loco.rig(rig);
    cam[0] = rig[12]; cam[1] = rig[13] + 1.6f; cam[2] = rig[14];
    cam[3] = std::atan2(rig[8], rig[0]) * 180.0f / 3.14159265f;
    LOG("spawn view: (%.2f %.2f %.2f) yaw %.0f", cam[0], cam[1], cam[2], cam[3]);
}

int runSnapshot(const std::string& out, const std::vector<std::string>& apks, const float camIn[5], uint32_t size, bool fromSpawn) {
    HslScene scene;
    std::string err;
    if (!loadHslScene(apks, scene, err)) { LOG("%s", err.c_str()); return 1; }
    float cam[5] = {camIn[0], camIn[1], camIn[2], camIn[3], camIn[4]};
    if (fromSpawn) spawnView(scene, cam);
    HslRenderer r;
    // FH_FOVEATE=level (1-3) and FH_GAZE=x,y (0..1 of the image): render foveated, looking there (tests foveation).
    if (const char* fv = std::getenv("FH_FOVEATE")) r.foveation = std::atoi(fv) > 0;
    r.exportDirect = std::getenv("FH_DIRECT") != nullptr;   // foveation as system mode does it (the map moved, no offsets)
    if (!r.init(size, size)) return 1;
    if (const char* fv = std::getenv("FH_FOVEATE")) r.setFoveationLevel(std::atoi(fv));
    if (const char* gz = std::getenv("FH_GAZE")) {
        float gx = 0.5f, gy = 0.5f;
        std::sscanf(gz, "%f,%f", &gx, &gy);
        for (uint32_t e = 0; e < 2; ++e) r.setFoveationCenter(e, gx * size, gy * size);
    }
    // FH_ANIM_T=seconds: a Clay / glTF home's animation at that time, through the per-frame vertex path (tests it).
    const char* animT = std::getenv("FH_ANIM_T");
    if (animT) r.setDynamicMeshes(scene.animatedMeshes);
    if (!r.load(scene)) { LOG("nothing renderable"); return 1; }
    if (animT && scene.animate) scene.animate(static_cast<float>(std::atof(animT)), scene.meshes);
    // FH_FRAMES=N: render N frames while textures stream in (the interactive path), instead of waiting for all.
    const int streamFrames = std::getenv("FH_FRAMES") ? std::atoi(std::getenv("FH_FRAMES")) : 0;
    if (!streamFrames) r.finishStreaming();
    applyEdits(r, scene, apks.front());

    const float yaw = cam[3] * 3.14159265f / 180.0f, pitch = cam[4] * 3.14159265f / 180.0f;
    // camera-to-world: yaw about +Y, then pitch about +X; looks down -Z at yaw = pitch = 0.
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    Mat4 pose = {};
    pose.m[0] = cy;            pose.m[1] = 0;   pose.m[2] = -sy;
    pose.m[4] = sy * sp;       pose.m[5] = cp;  pose.m[6] = cy * sp;
    pose.m[8] = sy * cp;       pose.m[9] = -sp; pose.m[10] = cy * cp;
    pose.m[12] = cam[0]; pose.m[13] = cam[1]; pose.m[14] = cam[2]; pose.m[15] = 1;
    const Mat4 proj = projection(-1, 1, -1, 1);                         // 90 degree square eye
    EyeView eyes[2] = {eyeFromPose(pose, proj), eyeFromPose(pose, proj)};
    // FH_TEST_GUIDE=1: draw the locomotion guide as Frame Home would, from this camera: the right hand aims a
    // teleport arc through the home's real world (FH_GUIDE_AIM=pitch,yaw degrees off the view, default -25,0), the
    // left hand points (FH_GUIDE_LEFT=pitch,yaw; none by default), seats within reach show markers.
    // FH_GUIDE_T=seconds: the animation time (dashes, pulses).
    if (std::getenv("FH_TEST_GUIDE")) {
        Locomotion gl;
        gl.setWorld(walkWorld(scene));
        Mat4 rig;
        gl.rig(rig.m);
        const Mat4 headT = mul(rigidInverse(rig), pose);                // tracking space, so rig * head = the camera
        auto handAt = [&](const char* env, float defPitch, float side, float* m34) {
            float pd = defPitch, yd = 0;
            if (env && std::getenv(env)) std::sscanf(std::getenv(env), "%f,%f", &pd, &yd);
            else if (env) return false;
            const float pr = pd * 3.14159265f / 180, yr = yd * 3.14159265f / 180;
            // hand = head * translate(side, -0.35, -0.25) * Ry(yaw) * Rx(pitch)
            const float cyh = std::cos(yr), syh = std::sin(yr), cph = std::cos(pr), sph = std::sin(pr);
            const float L[9] = {cyh, 0, -syh, syh * sph, cph, cyh * sph, syh * cph, -sph, cyh * cph};   // columns
            const float off[3] = {side, -0.35f, -0.25f};
            for (int r3 = 0; r3 < 3; ++r3) {
                for (int c = 0; c < 3; ++c)
                    m34[r3 * 4 + c] = headT.m[r3] * L[c * 3] + headT.m[4 + r3] * L[c * 3 + 1] + headT.m[8 + r3] * L[c * 3 + 2];
                m34[r3 * 4 + 3] = headT.m[12 + r3] + headT.m[r3] * off[0] + headT.m[4 + r3] * off[1] + headT.m[8 + r3] * off[2];
            }
            return true;
        };
        float right[12], left[12];
        if (!handAt("FH_GUIDE_AIM", -25, 0.18f, right)) handAt(nullptr, -25, 0.18f, right);
        const bool hasLeft = handAt("FH_GUIDE_LEFT", 0, -0.18f, left);
        gl.testInput(0, 0, 0, 1, right);
        gl.testButtons(false, false, hasLeft ? left : nullptr);
        for (int i = 0; i < 20; ++i) gl.update(1.0f / 72, headT.m);
        std::vector<GuideVertex> verts;
        const float gt = std::getenv("FH_GUIDE_T") ? static_cast<float>(std::atof(std::getenv("FH_GUIDE_T"))) : 0.3f;
        buildGuide(gl.guide(), &pose.m[12], gt, verts);
        float tg[3];
        const bool ok = gl.testTarget(tg);
        LOG("guide: arc %zu points, %s at (%.2f %.2f %.2f)%s, %zu seat markers, pointer %d; %zu vertices", gl.guide().arc.size() / 3,
            ok ? "lands" : "invalid", tg[0], tg[1], tg[2], gl.testTargetSeat() >= 0 ? " (a seat)" : "", gl.guide().seats.size(),
            gl.guide().pointer ? 1 : 0, verts.size());
        for (const LocoGuide::Seat& st : gl.guide().seats) LOG("  seat marker at (%.2f %.2f %.2f)%s", st.pos[0], st.pos[1], st.pos[2], st.hot ? " (pointed at)" : "");
        r.setGuide(verts);
    }
    // FH_DEBUG_VIEW=1..4 (HslRenderer::DebugView), FH_DEBUG_HIDE=bits, FH_DEBUG_WORLD=1: the Debug page's views.
    if (const char* dv = std::getenv("FH_DEBUG_VIEW")) r.setDebugView(std::atoi(dv));
    if (const char* dh = std::getenv("FH_DEBUG_HIDE")) r.debugHide = static_cast<unsigned>(std::atoi(dh));
    if (std::getenv("FH_DEBUG_WORLD")) {
        Locomotion wl;
        wl.setWorld(walkWorld(scene));
        std::vector<GuideVertex> ov;
        debuggeom::build(wl.world(), wl.walkGrid(), ov);
        LOG("debug world overlay: %zu triangles", ov.size() / 3);
        r.setStaticGuide(ov);
    }
    if (r.debugView() == HslRenderer::kViewCost) {   // a few frames to time every draw
        for (int f = 0; f < 6; ++f) r.render(eyes, 0.0f);
        for (const auto& c : r.costliest(6))
            LOG("  %.3f ms  %s", c.first, c.second >= 0 && static_cast<size_t>(c.second) < scene.meshes.size() ? scene.meshes[c.second].name.c_str() : "?");
    }
    for (int f = 0; f < streamFrames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        r.render(eyes, f / 72.0f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (f % 30 == 0 || ms > 20) LOG("frame %d: %.1f ms, %zu textures still streaming", f, ms, r.texturesPending());
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    r.render(eyes, animT ? static_cast<float>(std::atof(animT)) : 0.0f);   // (HSL homes: their shader clock and rigs too)
    std::vector<u8> rgba;
    r.readback(0, rgba);
    std::ofstream f(out, std::ios::binary);
    f << "P6\n" << size << " " << size << "\n255\n";
    for (size_t i = 0; i < rgba.size(); i += 4) f.write(reinterpret_cast<const char*>(&rgba[i]), 3);
    LOG("wrote %s", out.c_str());
    r.shutdown();
    return 0;
}

// ── Benchmark (no SteamVR) ──────────────────────────────────────────────────────────────────────
// frame-home --bench <footprint.apk> [vista.apk] [--seconds N] [--size WxH | N] [--hz N] [--unpaced]
// Renders both eyes the way system mode does (settings.json's MSAA, filtering and foveation; the export copy into a
// shared slot; the GPU waited for and the frame "published" at once) from a camera turning slowly at the spawn, paced
// to the display rate, and prints what frames cost. The live environment shares the GPU: the numbers include that.
// FH_BENCH_WALK=1: walking about instead (the left stick held forward through the real locomotion, steering slowly
// and turning away from whatever stops it, looking around as it goes), so the frames cover the whole home. The JSON's
// "worst" lists the views of the dearest GPU frames (x,y,z,yaw,pitch in degrees, ms), a few apart, for
// FH_BENCH_CAM=x,y,z,yaw,pitch: the bench held at one view (with FH_BENCH_COST=1, what each draw there costs).
// FH_BENCH_MOVES=1 (with FH_BENCH_WALK): every 8 s, after 4 s of walking, a snap turn, a teleport ahead (aimed with the
// real arc, landing at once) and a fast head turn (180 degrees in half a second): what a visitor does. The JSON's
// "moves" says, for each, how many frames ran late and the slowest; "spikes" the slowest frames (s, ms, move).
static long peakRamMB() { return homeos::peakRamMB(); }

int runBench(const std::vector<std::string>& apks, double seconds, uint32_t w, uint32_t h, float hz, bool unpaced) {
    const fs::path settingsPath = rootDir() / "settings.json";
    PerfSettings perf;
    if (!perf.fromJson(readText(settingsPath))) perf = PerfSettings();
    // The world's own settings over the headset's (FH_BENCH_NOWORLD=1: the headset's alone, as `fenv-tool tune` measures).
    const WorldSettings world = std::getenv("FH_BENCH_NOWORLD") ? WorldSettings() : worldSettingsFor(apks.front());
    float fovStrength = world.foveationStrength(perf);
    perf = world.apply(perf);
    if (!world.empty()) LOG("bench: the world's own settings: %s", world.describe().c_str());
    // FH_BENCH_MSAA=n, FH_BENCH_FOV=0..2, FH_BENCH_FOVLEVEL=1..3, FH_BENCH_FOVSTRENGTH=1..3 (in between too),
    // FH_BENCH_SCALE=x (resolution, of SteamVR's 100%): other settings than the headset's (A/B tests, tuning).
    if (const char* v = std::getenv("FH_BENCH_MSAA")) perf.msaa = std::atoi(v);
    if (const char* v = std::getenv("FH_BENCH_FOV")) perf.foveation = std::atoi(v);
    if (const char* v = std::getenv("FH_BENCH_FOVLEVEL")) { perf.foveationLevel = std::atoi(v); fovStrength = static_cast<float>(perf.foveationLevel); }
    if (const char* v = std::getenv("FH_BENCH_FOVSTRENGTH")) fovStrength = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("FH_BENCH_ANISO")) perf.anisotropy = std::atoi(v);
    perf.clamp();
    if (!w || !h) {
        // SteamVR's 100% (the live instance's size over its scale, in stats.json; else the Frame's 2016) times the
        // resolution: FH_BENCH_SCALE's, the world's own, the live instance's, else settings.json's.
        const std::string st = readText(rootDir() / "stats.json");
        auto num = [&](const char* k) { const size_t p = st.find(k); return p == std::string::npos ? 0.0 : std::atof(st.c_str() + p + std::strlen(k)); };
        const double liveScale = num("\"scale\":");
        double baseW = num("\"width\":"), baseH = num("\"height\":");
        if (baseW > 0 && baseH > 0 && liveScale > 0) { baseW /= liveScale; baseH /= liveScale; }
        else { baseW = baseH = 2016; }
        double scale = liveScale > 0 ? liveScale : perf.autoResolution ? 1.0 : perf.resolution;
        if (world.resolution > 0) scale = world.resolution;
        if (const char* v = std::getenv("FH_BENCH_SCALE")) scale = std::atof(v);
        w = std::max(256u, static_cast<uint32_t>(std::lround(baseW * scale / 8.0)) * 8u);
        h = std::max(256u, static_cast<uint32_t>(std::lround(baseH * scale / 8.0)) * 8u);
    }
    const auto tl0 = std::chrono::steady_clock::now();
    HslScene scene;
    std::string err;
    if (!loadHslScene(apks, scene, err)) { LOG("%s", err.c_str()); return 1; }
    const auto tl1 = std::chrono::steady_clock::now();
    HslRenderer r;
    r.msaa = perf.msaa;
    r.anisotropy = static_cast<float>(perf.anisotropy);
    r.foveation = perf.foveation > 0;
#ifdef _WIN32
    r.exportDirect = false;                           // (a PC: as the scene app draws; there's no system mode)
#else
    r.deviceExtensions = {"VK_KHR_external_memory", "VK_KHR_external_memory_fd"};
    r.exportDirect = !std::getenv("FH_BENCH_COPY");   // as system mode draws (FH_BENCH_COPY=1: copied in, the old way)
#endif
    r.exportSubsampled = !std::getenv("FH_EXPORT_PLAIN");   // likewise (FH_EXPORT_PLAIN=1: our smoothing pass)
    r.pipelineCachePath = (fs::temp_directory_path() / "frame-home-bench-pipelines.bin").string();
    if (const char* pc = std::getenv("FH_BENCH_PIPELINES")) r.pipelineCachePath = pc;
    if (!r.init(w, h)) return 1;
    r.setFoveationStrength(fovStrength);
    r.setDynamicMeshes(scene.animatedMeshes);
    if (std::getenv("FH_BENCH_COST")) r.setDebugView(HslRenderer::kViewCost);
    if (const char* v = std::getenv("FH_BENCH_VIEW")) r.setDebugView(static_cast<HslRenderer::DebugView>(std::atoi(v)));   // A/B: a debug view's cheap shading
    if (const char* v = std::getenv("FH_BENCH_HIDE")) r.debugHide = static_cast<unsigned>(std::atoi(v));   // A/B: parts left out
    const auto tl2 = std::chrono::steady_clock::now();
    if (!r.load(scene)) { LOG("nothing renderable"); return 1; }
    if (std::getenv("FH_BENCH_PRELOAD")) { r.requestAllTextures(); r.finishStreaming(); }   // every texture in before timing
    const auto tl3 = std::chrono::steady_clock::now();
    applyEdits(r, scene, apks.front());
    // FH_BENCH_HIDENAME=part,part: meshes whose name holds one left out (A/B: what one mesh costs); FH_BENCH_LIST=1
    // lists the see-through and cut-out meshes with their triangles and size.
    if (const char* hn = std::getenv("FH_BENCH_HIDENAME"))
        for (size_t i = 0; i < r.meshCount(); ++i)
            for (std::string rest = hn; !rest.empty();) {
                const size_t c = rest.find(',');
                const std::string part = rest.substr(0, c);
                const int si = r.meshSceneIndex(i);
                if (!part.empty() && si >= 0 && static_cast<size_t>(si) < scene.meshes.size() &&
                    scene.meshes[si].name.find(part) != std::string::npos) { r.setMeshVisible(i, false); break; }
                rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
            }
    if (std::getenv("FH_BENCH_LIST"))
        for (size_t mi = 0; mi < r.meshCount(); ++mi) {
            const int si = r.meshSceneIndex(mi);
            if (si < 0 || static_cast<size_t>(si) >= scene.meshes.size()) continue;
            const MeshData& m = scene.meshes[si];
            if (!r.meshBlend(mi) && !m.alphaTest && !m.alphaToCoverage && !r.meshDynamic(mi)) continue;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
                for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], m.positions[i + k]); hi[k] = std::max(hi[k], m.positions[i + k]); }
            std::fprintf(stderr, "list: %-60s %s%s%s%s %7zu tris  %.1f x %.1f x %.1f  [%s]\n", m.name.c_str(), r.meshBlend(mi) == 1 ? "blend " : r.meshBlend(mi) == 2 ? "add " : "",
                         r.meshDynamic(mi) ? "anim " : "",
                         m.alphaToCoverage || m.alphaTest ? "cutout " : "", "", m.indices.size() / 3, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], m.shaderPath.c_str());
        }
    std::vector<HslRenderer::ExportImage> exports;
    uint32_t memType = 0;
    uint8_t uuid[16];
    const bool exported = r.createExportImages(envshare::kSlots, exports, memType, uuid);
    LOG("bench: %ux%u per eye, %dx MSAA, %dx filtering, foveation %s (level %d), %s%.0f Hz, %zu animated meshes", r.width(), r.height(), perf.msaa,
        perf.anisotropy, r.foveationActive() ? "on" : "off", perf.foveationLevel, unpaced ? "unpaced, display " : "paced to ", hz, scene.animatedMeshes.size());
    LOG("bench: load %.2f s parse + %.2f s upload (%zu meshes)", std::chrono::duration<double>(tl1 - tl0).count(),
        std::chrono::duration<double>(tl3 - tl2).count(), r.meshCount());

    Locomotion loco;
    loco.setWorld(walkWorld(scene));
    float rig[16];
    loco.rig(rig);
    const float base[3] = {rig[12], rig[13] + 1.6f, rig[14]};
    const float yaw0 = std::atan2(rig[8], rig[0]);
    const Mat4 proj = projection(-1.12f, 1.12f, -1.12f, 1.12f);
    static const bool walk = std::getenv("FH_BENCH_WALK") != nullptr;
    static const bool moves = walk && std::getenv("FH_BENCH_MOVES") != nullptr;
    if (walk) loco.setOptions(1, moves ? 1 : 0, moves ? 0 : 1, 45, 1.4f);   // smooth walking (and turning, or snap turns and teleports)
    enum Move { kWalk, kSnap, kTeleport, kWhip, kMoves };
    static const char* const kMoveName[kMoves] = {"walk", "snap", "teleport", "whip"};
    struct MoveStat { uint32_t frames = 0, late = 0; double maxMs = 0; } moveStat[kMoves];
    struct Spike { float t; double ms; int move; };
    std::vector<Spike> spikes;
    int move = kWalk, teleports = 0;
    float whipYaw = 0;
    float walked = 0, lastPos[2] = {rig[12], rig[14]}, stuckFor = 0, lastT = 0;
    float fixedCam[5] = {0, 0, 0, 0, 0};
    const bool fixedView = std::getenv("FH_BENCH_CAM") &&
                           std::sscanf(std::getenv("FH_BENCH_CAM"), "%f,%f,%f,%f,%f", &fixedCam[0], &fixedCam[1], &fixedCam[2], &fixedCam[3], &fixedCam[4]) == 5;
    struct View { float gpuMs, v[5]; };
    std::vector<View> views;                   // every frame's view and the GPU time that came back with it
    FrameProfile prof;
    prof.on = true;
    Window make, cpu, gpu, gap;
    const double period = 1.0 / hz;
    uint32_t over = 0, hitches = 0, frames = 0, earlyFrames = 0;
    float animTime = 0;
    const uint64_t t0 = envshare::nowNs();
    uint64_t next = t0, lastDone = 0, gpuSeen = r.gpuResults();
    uint32_t slot = 0;
    // FH_BENCH_FRAMES=n: exactly n frames, each 1/120 s after the one before (the same pictures every run: with
    // FH_BENCH_SHOT=out.ppm, the last one is saved, so ways of making frames can be compared pixel for pixel).
    static const int fixedFrames = std::getenv("FH_BENCH_FRAMES") ? std::atoi(std::getenv("FH_BENCH_FRAMES")) : 0;
    while (fixedFrames > 0 ? frames < static_cast<uint32_t>(fixedFrames) : envshare::nowNs() - t0 < static_cast<uint64_t>(seconds * 1e9)) {
        if (!unpaced) { sleepUntilNs(next); next += static_cast<uint64_t>(period * 1e9); if (next < envshare::nowNs()) next = envshare::nowNs(); }
        const uint64_t fs0 = envshare::nowNs();
        const float t = fixedFrames > 0 ? frames / 120.0f : (fs0 - t0) / 1e9f;
        // Looking around at the spawn: a full turn every 16 s, nodding, swaying a little.
        const float yaw = yaw0 + t * 6.2831853f / 16.0f, pitch = 0.25f * std::sin(t * 0.9f);
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        Mat4 pose = {};
        pose.m[0] = cy;            pose.m[1] = 0;   pose.m[2] = -sy;
        pose.m[4] = sy * sp;       pose.m[5] = cp;  pose.m[6] = cy * sp;
        pose.m[8] = sy * cp;       pose.m[9] = -sp; pose.m[10] = cy * cp;
        pose.m[15] = 1;
        EyeView eyes[2];
        if (fixedView) {
            const float y = fixedCam[3] * 0.01745329f, pi = fixedCam[4] * 0.01745329f;
            const float c = std::cos(y), s = std::sin(y), cq = std::cos(pi), sq = std::sin(pi);
            pose = {};
            pose.m[0] = c;           pose.m[2] = -s;
            pose.m[4] = s * sq;      pose.m[5] = cq;  pose.m[6] = c * sq;
            pose.m[8] = s * cq;      pose.m[9] = -sq; pose.m[10] = c * cq;
            pose.m[12] = fixedCam[0]; pose.m[13] = fixedCam[1]; pose.m[14] = fixedCam[2]; pose.m[15] = 1;
            for (int e = 0; e < 2; ++e) {
                Mat4 p = pose;
                for (int k = 0; k < 3; ++k) p.m[12 + k] += pose.m[k] * (e ? 0.032f : -0.032f);
                eyes[e] = eyeFromPose(p, proj);
            }
        } else if (walk) {
            // The head looks about (side to side every 9 s, nodding) in a rig the stick walks forward; it steers
            // gently, and hard once it hasn't got anywhere for half a second.
            // FH_BENCH_MOVES: the cycle's moves (each a short window; the stick released between them).
            const float ph = std::fmod(t, 8.0f);
            move = !moves || ph < 4.0f ? kWalk : ph < 4.4f ? kSnap : ph < 5.6f ? kTeleport : ph < 6.6f ? kWhip : kWalk;
            if (move == kWhip) {
                const float u = std::clamp((ph - 6.0f) / 0.5f, 0.0f, 1.0f);
                whipYaw = (std::floor(t / 8.0f) + u * u * (3 - 2 * u)) * 3.14159265f;
            } else if (moves) whipYaw = std::floor(t / 8.0f + (ph >= 6.6f ? 1 : 0)) * 3.14159265f;
            const float hy = whipYaw + 0.6f * std::sin(t * 0.7f), hp = 0.2f * std::sin(t * 1.1f);
            const float c = std::cos(hy), s = std::sin(hy), cp = std::cos(hp), sp = std::sin(hp);
            Mat4 head = {};
            head.m[0] = c;           head.m[2] = -s;
            head.m[4] = s * sp;      head.m[5] = cp;  head.m[6] = c * sp;
            head.m[8] = s * cp;      head.m[9] = -sp; head.m[10] = c * cp;
            head.m[13] = 1.6f;       head.m[15] = 1;
            const float dt = t - lastT;
            lastT = t;
            float now[16];
            loco.rig(now);
            const float moved = std::hypot(now[12] - lastPos[0], now[14] - lastPos[1]);
            walked += moved;
            lastPos[0] = now[12]; lastPos[1] = now[14];
            stuckFor = dt > 0 && moved / dt < 0.3f ? stuckFor + dt : 0;
            if (!moves) loco.testInput(0, 1, stuckFor > 0.5f ? 1.0f : 0.35f * std::sin(t * 0.37f), 0);
            else if (move == kSnap) loco.testInput(0, 0, ph < 4.1f ? 1.0f : 0.0f, 0);
            else if (move == kTeleport) {
                // The right hand just below the head, pointing where it looks and 25 degrees down; the stick
                // forward for 0.8 s (the arc), then let go (the jump).
                const float dn = -0.436f, cd = std::cos(dn), sd = std::sin(dn);
                const float fwd[3] = {-s * cd, sd, -c * cd};     // the head looks down -Z
                const float x[3] = {c, 0, -s}, up[3] = {fwd[1] * x[2] - fwd[2] * x[1], fwd[2] * x[0] - fwd[0] * x[2], fwd[0] * x[1] - fwd[1] * x[0]};
                const float aim34[12] = {x[0], up[0], -fwd[0], 0.15f, x[1], up[1], -fwd[1], 1.3f, x[2], up[2], -fwd[2], 0.0f};
                const bool hold = ph < 5.2f;
                loco.testInput(0, 0, 0, hold ? 1.0f : 0.0f, aim34);
                if (!hold && ph - dt < 5.2f) ++teleports;
            } else loco.testInput(0, move == kWhip ? 0.0f : 1.0f, move == kWalk && stuckFor > 0.5f ? 1.0f : move == kWalk ? 0.35f * std::sin(t * 0.37f) : 0.0f, 0);
            loco.update(dt, head.m);
            Mat4 rg;
            loco.rig(rg.m);
            pose = mul(rg, head);
            for (int e = 0; e < 2; ++e) {
                Mat4 p = pose;
                for (int k = 0; k < 3; ++k) p.m[12 + k] += pose.m[k] * (e ? 0.032f : -0.032f);
                eyes[e] = eyeFromPose(p, proj);
            }
        } else {
            for (int e = 0; e < 2; ++e) {
                Mat4 p = pose;
                const float off = (e ? 0.032f : -0.032f) + 0.1f * std::sin(t * 0.7f);
                for (int k = 0; k < 3; ++k) p.m[12 + k] = base[k] + pose.m[k] * off;
                eyes[e] = eyeFromPose(p, proj);
            }
        }
        const auto a0 = std::chrono::steady_clock::now();
        float rt = t;
        static const bool noAnim = std::getenv("FH_BENCH_NOANIM") != nullptr;   // A/B: the home held still
        if (scene.animate && !scene.animatedMeshes.empty() && !noAnim) {
            animTime = std::fmod(t, scene.animDuration > 0 ? scene.animDuration : 60.0f);
            scene.animate(animTime, scene.meshes);
            rt = animTime;
        }
        const double animMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count();
        static const int slots = std::getenv("FH_BENCH_SLOTS") ? std::max(1, std::atoi(std::getenv("FH_BENCH_SLOTS"))) : static_cast<int>(envshare::kSlots);
        static const bool noExport = std::getenv("FH_BENCH_NOEXPORT") != nullptr;   // A/B: no copy into the compositor's images
        r.exportSlot = exported && !noExport ? static_cast<int>(slot++ % static_cast<uint32_t>(slots)) : -1;   // FH_BENCH_SLOTS=n: only the first n
        // FH_BENCH_OVERLAP=1: no wait here (as in system mode, render() waits for the frame before itself);
        // FH_BENCH_EARLY=1: and frames recorded while the GPU still draws the one before.
        static const bool overlap = std::getenv("FH_BENCH_OVERLAP") != nullptr;
        static const bool early = [&] { const bool e = std::getenv("FH_BENCH_EARLY") != nullptr; r.setEarlyRecord(e); return e; }();
        (void)early;
        r.render(eyes, rt);
        if (!overlap) r.waitFrame();
        if (r.deviceLost()) { LOG("bench: device lost (a GPU reset) after %u frames", frames); break; }
        const uint64_t done = envshare::nowNs();
        const double makeMs = (done - fs0) / 1e6;
        make.add(makeMs);
        cpu.add(r.lastStats().totalMs + animMs);
        const bool gpuNew = r.gpuResults() != gpuSeen;
        gpuSeen = r.gpuResults();
        if (gpuNew && frames > 2) gpu.add(r.lastGpuMs());
        if (gpuNew && frames > 2)
            views.push_back({static_cast<float>(r.lastGpuMs()), {pose.m[12], pose.m[13], pose.m[14], std::atan2(pose.m[8], pose.m[10]) * 57.29578f,
                                                                  std::asin(std::clamp(-pose.m[9], -1.0f, 1.0f)) * 57.29578f}});
        if (lastDone) gap.add((done - lastDone) / 1e6);
        lastDone = done;
        prof.frame(r, makeMs, animMs, r.lastGpuMs(), gpuNew);
        prof.published(done, makeMs, makeMs > period * 1000);
        prof.report("bench");
        if (makeMs > period * 1000) ++over;
        if (makeMs > period * 1500) ++hitches;
        if (frames > 2) {
            MoveStat& ms = moveStat[move];
            ++ms.frames;
            ms.late += makeMs > period * 1000 ? 1 : 0;
            ms.maxMs = std::max(ms.maxMs, makeMs);
            if (makeMs > period * 1500) spikes.push_back({t, makeMs, move});
        }
        earlyFrames += r.lastStats().early ? 1 : 0;
        ++frames;
    }
    const double secs = (envshare::nowNs() - t0) / 1e9;
    std::sort(views.begin(), views.end(), [](const View& a, const View& b) { return a.gpuMs > b.gpuMs; });
    std::string worst;
    std::vector<const View*> picked;
    for (const View& v : views) {
        if (picked.size() >= 5) break;
        bool nearOne = false;                  // another picked view within 1.5 m looking within 40 degrees
        for (const View* q : picked) {
            float dy = std::fabs(v.v[3] - q->v[3]);
            if (dy > 180) dy = 360 - dy;
            nearOne |= std::hypot(v.v[0] - q->v[0], v.v[2] - q->v[2]) < 1.5f && dy < 40;
        }
        if (nearOne) continue;
        picked.push_back(&v);
        char b[128];
        std::snprintf(b, sizeof b, "%s[%.2f,%.2f,%.2f,%.0f,%.0f,%.2f]", worst.empty() ? "" : ",", v.v[0], v.v[1], v.v[2], v.v[3], v.v[4], v.gpuMs);
        worst += b;
    }
    std::string movesJson, spikesJson;
    if (moves) {
        for (int m = 0; m < kMoves; ++m) {
            char b[160];
            std::snprintf(b, sizeof b, "%s\"%s\":{\"frames\":%u,\"late\":%u,\"maxMs\":%.2f}", m ? "," : "", kMoveName[m], moveStat[m].frames, moveStat[m].late, moveStat[m].maxMs);
            movesJson += b;
        }
        char b[48];
        std::snprintf(b, sizeof b, ",\"teleports\":%d", teleports);
        movesJson += b;
    }
    std::sort(spikes.begin(), spikes.end(), [](const Spike& a, const Spike& b) { return a.ms > b.ms; });
    for (size_t i = 0; i < spikes.size() && i < 8; ++i) {
        char b[80];
        std::snprintf(b, sizeof b, "%s[%.2f,%.1f,\"%s\"]", i ? "," : "", spikes[i].t, spikes[i].ms, kMoveName[spikes[i].move]);
        spikesJson += b;
    }
    if (const char* shot = std::getenv("FH_BENCH_SHOT")) {
        r.waitFrame();
        std::vector<u8> rgba;
        if (r.readback(0, rgba) && rgba.size() >= static_cast<size_t>(r.width()) * r.height() * 4) {
            std::ofstream f(shot, std::ios::binary);
            f << "P6\n" << r.width() << " " << r.height() << "\n255\n";
            for (size_t i = 0; i < rgba.size(); i += 4) f.write(reinterpret_cast<const char*>(&rgba[i]), 3);
        }
    }
    std::printf("{\"home\":\"%s\",\"width\":%u,\"height\":%u,\"msaa\":%d,\"foveation\":%s,\"fovStrength\":%.2f,\"frames\":%u,\"fps\":%.1f,"
                "\"makeAvg\":%.2f,\"makeP95\":%.2f,\"makeP99\":%.2f,\"makeMax\":%.2f,\"cpuAvg\":%.2f,\"cpuP95\":%.2f,\"cpuMax\":%.2f,"
                "\"gpuP10\":%.2f,\"gpuAvg\":%.2f,\"gpuP95\":%.2f,\"gpuMax\":%.2f,\"gapSd\":%.2f,\"gapMax\":%.2f,\"overBudget\":%u,\"hitches\":%u,"
                "\"parseS\":%.2f,\"uploadS\":%.2f,\"draws\":%u,\"early\":%u,\"walkedM\":%.1f,\"peakRamMB\":%ld,\"worst\":[%s],\"moves\":{%s},\"spikes\":[%s]}\n",
                fs::path(apks.front()).filename().c_str(), r.width(), r.height(), perf.msaa, r.foveationActive() ? "true" : "false", r.foveationStrength(), frames,
                frames / secs, make.avg(), make.pct(0.95), make.pct(0.99), make.max(), cpu.avg(), cpu.pct(0.95), cpu.max(), gpu.pct(0.10), gpu.avg(),
                gpu.pct(0.95), gpu.max(), gap.stddev(), gap.max(), over, hitches, std::chrono::duration<double>(tl1 - tl0).count(),
                std::chrono::duration<double>(tl3 - tl2).count(), r.lastStats().draws, earlyFrames, walked, peakRamMB(), worst.c_str(), movesJson.c_str(), spikesJson.c_str());
    for (const auto& pc : r.perfCounterAverages()) std::fprintf(stderr, "perf: %14.0f  %s\n", pc.second, pc.first.c_str());
    if (r.debugView() == HslRenderer::kViewCost) {   // FH_BENCH_COST=1: where the GPU time went, mesh by mesh
        double sum = 0;
        for (size_t i = 0; i < r.meshCount(); ++i) sum += std::max(0.0f, r.meshCostMs(i));
        std::fprintf(stderr, "cost: %.2f ms in all draws\n", sum);
        for (const auto& c : r.costliest(15)) {
            const MeshData& md = scene.meshes[c.second];
            std::fprintf(stderr, "cost: %6.3f ms  %-60s %s%s %zu tris  [%s]\n", c.first, md.name.c_str(), md.useBlend || md.additive ? "blend " : "",
                         md.alphaTest ? "cutout " : "", md.indices.size() / 3, md.shaderPath.c_str());
        }
    }
    r.destroyExportImages(exports);
    r.shutdown();
    return 0;
}

// Renders a home into the Frame's native environment panorama (equirect PNG) and prints the dome radius.
// A 360-degree picture of the home (equirectangular PNG, width x width/2) from `cam` (or the spawn), and beside it
// <out>.json: the eye height and the dome radius SteamVR's background needs to project it (the panel on a PC sets it).
int runPanorama(const std::string& out, const std::vector<std::string>& apks, const float camIn[5], uint32_t width, bool fromSpawn) {
    HslScene scene;
    std::string err;
    if (!loadHslScene(apks, scene, err)) { LOG("%s", err.c_str()); return 1; }
    float cam[5] = {camIn[0], camIn[1], camIn[2], camIn[3], camIn[4]};
    if (fromSpawn) spawnView(scene, cam);
    HslRenderer r;
    if (!r.init(256, 256)) return 1;
    if (!r.load(scene)) { LOG("nothing renderable"); return 1; }
    r.finishStreaming();
    applyEdits(r, scene, apks.front());
    PanoramaView v;
    std::memcpy(v.eye, cam, 12);
    v.yawDeg = cam[3];
    std::vector<u8> rgb;
    if (!renderPanorama(r, v, width, rgb) || !writePng(out, rgb, width, width / 2)) { LOG("panorama failed"); return 1; }
    char j[512];
    std::snprintf(j, sizeof j, "{\"width\": %u, \"eyeHeight\": %.3f, \"domeRadius\": %.3f}\n", width, fromSpawn ? 1.6f : cam[1], panoramaDomeRadius(r, scene, v.eye));
    std::printf("%s", j);
    if (FILE* f = homeos::openFile(out + ".json", "w")) { std::fputs(j, f); std::fclose(f); }
    r.shutdown();
    return 0;
}

// ── SteamVR ─────────────────────────────────────────────────────────────────────────────────────

VkPhysicalDevice pickVrDevice(VkInstance instance) {
    uint64_t dev = 0;
    vr::VRSystem()->GetOutputDevice(&dev, vr::TextureType_Vulkan, instance);
    return reinterpret_cast<VkPhysicalDevice>(dev);
}

// widen > 1: a wider view than the lens shows (system mode: margin for the compositor's rotation reprojection).
Mat4 eyeProjection(vr::EVREye eye, float widen = 1.0f) {
    // Tangents from OpenVR's own projection (GL style: row 3 = [0 0 -1 0], y up).
    vr::HmdMatrix44_t p = vr::VRSystem()->GetProjectionMatrix(eye, kNear, 100.0f);
    const float l = (p.m[0][2] - 1) / p.m[0][0], r = (p.m[0][2] + 1) / p.m[0][0];
    const float b = (p.m[1][2] - 1) / p.m[1][1], t = (p.m[1][2] + 1) / p.m[1][1];
    return projection(l * widen, r * widen, b * widen, t * widen);
}

// system: Frame Home as the system environment (frame-home --system): not a scene app; it renders for the
// compositor layer, which shows it in Aurora's place (env_share.h, layer/frame_home_layer.cpp).
int runVr(const std::vector<std::string>& apks, bool system = false) {
    if (!homeos::stderrIsTerminal()) {   // started by SteamVR: messages go to ~/frame-home/logs/frame-home.log
        std::error_code ec;
        fs::create_directories(rootDir() / "logs", ec);
        if (homeos::reopenStderr(rootDir() / "logs" / "frame-home.log", std::getenv("FH_RESTARTED") ? "a" : "w")) setvbuf(stderr, nullptr, _IOLBF, 0);
    }
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, system ? vr::VRApplication_Overlay : vr::VRApplication_Scene);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 1; }
    if (!system) vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);

    HslRenderer r;
    uint32_t n = vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(nullptr, 0);
    std::string ext(n, '\0');
    if (n) vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(&ext[0], n);
    r.instanceExtensions = split(ext.c_str());
    r.pickPhysicalDevice = pickVrDevice;
    uint32_t w = 0, h = 0;
    vr::VRSystem()->GetRecommendedRenderTargetSize(&w, &h);

    // Device extensions depend on the physical device, which needs the instance: init in two steps.
    {
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        std::vector<const char*> ie;
        for (auto& e : r.instanceExtensions) ie.push_back(e.c_str());
        ici.enabledExtensionCount = static_cast<uint32_t>(ie.size());
        ici.ppEnabledExtensionNames = ie.data();
        VkInstance probe = VK_NULL_HANDLE;
        if (vkCreateInstance(&ici, nullptr, &probe) != VK_SUCCESS) { LOG("no Vulkan instance"); vr::VR_Shutdown(); return 1; }
#ifdef FH_USE_VOLK
        volkLoadInstanceOnly(probe);               // (the renderer loads its own instance's after)
#endif
        VkPhysicalDevice pd = pickVrDevice(probe);
        uint32_t dn = vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(pd, nullptr, 0);
        std::string dext(dn, '\0');
        if (dn) vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(pd, &dext[0], dn);
        r.deviceExtensions = split(dext.c_str());
        if (system)   // the frames go to the compositor's device as external memory
            for (const char* e : {"VK_KHR_external_memory", "VK_KHR_external_memory_fd"})
                if (std::find(r.deviceExtensions.begin(), r.deviceExtensions.end(), e) == r.deviceExtensions.end()) r.deviceExtensions.push_back(e);
        vkDestroyInstance(probe, nullptr);
    }
    r.pipelineCachePath = (rootDir() / "pipelines.bin").string();
    // Performance settings (settings.json; the Environments panel and Frame Studio edit it).
    const fs::path settingsPath = rootDir() / "settings.json", statsPath = rootDir() / "stats.json";
    PerfSettings perf;
    {
        const std::string j = readText(settingsPath);
        std::error_code ec;
        if (!fs::exists(settingsPath, ec)) std::ofstream(settingsPath) << perf.toJson();
        else if (!perf.fromJson(j)) { perf = PerfSettings(); LOG("settings.json unreadable: defaults for now (the file is left as it is)"); }
    }
    std::string settingsStamp = stamp(settingsPath.string());
    // The shown world's own settings (world_settings.h: a .fenv's, or "<apk>.settings.json") over the headset's:
    // `global` is settings.json, `perf` what applies now. The first home's already decide how the renderer is built.
    PerfSettings global = perf;
    auto worldFor = [](const std::vector<std::string>& home) { return home.empty() ? WorldSettings() : worldSettingsFor(home.front()); };
    WorldSettings world = worldFor(apks.empty() ? currentHome() : apks);
    perf = world.apply(global);
    if (!world.empty()) LOG("the world's own settings: %s", world.describe().c_str());
    auto fovBase = [&] { return world.foveationStrength(global); };   // the foveation strength before any added for the GPU
    g_volume = perf.volume;
    // Debug tools (debug.json, the Environments panel's Debug page): for finding out why a home looks or runs wrong,
    // not for keeping, so a fresh start clears them (a restart for new settings keeps them).
    const fs::path debugPath = rootDir() / "debug.json";
    DebugSettings dbg;
    {
        std::error_code ec;
        if (!std::getenv("FH_RESTARTED")) fs::remove(debugPath, ec);
        else dbg.fromJson(readText(debugPath));
    }
    std::string debugStamp = stamp(debugPath.string());
    int shotSeen = dbg.shot;
    const long startedAt = static_cast<long>(std::time(nullptr));   // picture requests from before this are old
    bool overlayDirty = true;                         // the world overlay is (re)built for the home shown
    r.msaa = perf.msaa;
    r.anisotropy = static_cast<float>(perf.anisotropy);
    r.foveation = perf.foveation > 0;
    // System mode: frames go straight into the compositor's images (no 1.1-1.5 ms copy a frame). FH_EXPORT_COPY=1: the old way.
    r.exportDirect = system && !std::getenv("FH_EXPORT_COPY");
    // Smooth foveation's smoothing done by the compositor as it samples (no pass of ours). FH_EXPORT_PLAIN=1: our pass,
    // as before (also set by the restart below when the layer can't read subsampled images).
    r.exportSubsampled = !std::getenv("FH_EXPORT_PLAIN");
    r.setEarlyRecord(system);   // (frames recorded while the GPU finishes the one before)
    const uint32_t baseW = w, baseH = h;              // SteamVR's recommended size: 100%
    float scale = perf.autoResolution ? 1.0f : perf.resolution;
    auto scaled = [&](uint32_t v) { return std::max(256u, static_cast<uint32_t>(std::lround(v * scale / 8.0f)) * 8u); };
    if (!r.init(scaled(baseW), scaled(baseH))) { vr::VR_Shutdown(); return 1; }
    EnvExport env;
    if (system && (!env.init() || !env.createImages(r))) { LOG("system mode: cannot share frames with the compositor"); vr::VR_Shutdown(); r.shutdown(); return 1; }
    r.setFoveationStrength(fovBase());
    r.setDebugView(dbg.view);
    r.debugHide = dbg.hideBits();
    float hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    if (hz < 30 || hz > 500) hz = 90;
    LOG("settings: resolution %s (%ux%u per eye), %dx MSAA, %dx filtering, foveation %s, %.0f Hz display", perf.autoResolution ? "auto" : "fixed",
        r.width(), r.height(), perf.msaa, perf.anisotropy, !r.foveationActive() ? "off" : perf.foveation == 2 ? "eye tracked" : "fixed", hz);
    // Walking, turning and teleporting through the home's world layer (spawns, floors, walls, seats).
    Locomotion loco;
    if (!loco.init(rootDir() / "input")) LOG("controller input unavailable: walking and teleport are off");
    loco.setOptions(perf.walk, perf.teleport, perf.turn, perf.snapAngle, perf.moveSpeed);
    loco.setRingFallback(!r.guideAvailable());       // Frame Home draws the arc itself; the overlay ring only if it can't
    std::vector<GuideVertex> guideVerts;
    PanelDock dock;                                  // SteamVR panels docking on the home's wall spots

    // Homes are parsed (and their textures decoded) on a worker thread and uploaded between frames,
    // so the headset keeps getting frames while a new home loads.
    const bool follow = apks.empty();                // environment mode: follow ~/frame-home/current
    std::unique_ptr<HslScene> scene, pending;
    HomeWorld pendingWalk;                           // the loaded home's world with its walkable area (worker-made)
    HomeSounds pendingSounds;                        // ...and its sounds, decoded
    HomeReport pendingReport;                        // ...and its report (report.json, for the Debug page)
    AudioPlayer ambience, addedSound;               // the home's own ambience; a background sound added in Frame Studio
    std::string playingSound;                        // the added sound's file + stamp, as playing
    float soundBase[2] = {1, 1};                     // the home's and the added sound's own volumes (times g_volume)
    bool homeSoundOn = false;                        // the home's ambience was decoded (its volume above 0)
    std::thread soundJob;                            // new sounds (edits changed), decoded beside the frame loop
    std::atomic<bool> soundReady{false};
    HomeSounds soundNext;
    std::vector<std::string> shown, pendingHome;
    std::string shownFile, shownEdits, failed, loadErr;
    std::atomic<int> loadState{0};                   // 0 idle, 1 loading, 2 loaded, 3 failed
    std::thread worker;
    // A home's identity + file stamps (footprint and vista), and its edits stamp (keyed by the footprint).
    auto fileSig = [](const std::vector<std::string>& h) {
        std::string s;
        for (const auto& p : h) s += p + "|" + stamp(p) + "\n";
        return s;
    };
    float fade = 0, fadeTarget = 1;                  // system mode: switching homes fades through black
    std::chrono::steady_clock::time_point texturesUntil{};   // system mode: black until the home's textures are in (or then)
    bool uploading = false, uploadDone = false;      // the parsed home going to the GPU between frames
    auto uploadStart = std::chrono::steady_clock::now();
    auto startLoad = [&](const std::vector<std::string>& home) {
        if (worker.joinable()) worker.join();
        pendingHome = home;
        loadState = 1;
        if (system) { fadeTarget = 0; }
        LOG("loading %s%s", home.front().c_str(), home.size() > 1 ? " + vista" : "");
        worker = std::thread([&, home] {
            const auto began = std::chrono::steady_clock::now();
            auto s = std::make_unique<HslScene>();
            std::string e;
            if (loadHslScene(home, *s, e)) {
                // The world layer sent from Frame Studio next to the home replaces the converted one; then the
                // walkable area (read from it, or baked) — here, not on the frame loop: a bake can take a while.
                std::ifstream wf(worldPathFor(home.front()));
                const std::string wj((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
                HomeWorld w;
                if (!wj.empty() && worldFromJson(wj, w)) { s->world = std::move(w); LOG("world layer from %s", worldPathFor(home.front()).c_str()); }
                pendingWalk = walkWorld(*s);
                pendingReport = makeHomeReport(*s, &pendingWalk);
                pendingReport.loadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                HomeEdits edits;
                std::string editsPath;
                homeEditsFor(*s, home.front(), edits, editsPath);
                pendingSounds = loadSounds(s->ambientOgg, edits, editsPath);
                pending = std::move(s);
                loadState = 2;
            } else {
                loadErr = e;
                loadState = 3;
            }
        });
    };
    auto checkHome = [&] {
        const std::vector<std::string> want = follow ? currentHome() : apks;
        if (want.empty()) return;
        const std::string file = fileSig(want) + stamp(worldPathFor(want.front())), edits = stamp(editsPathFor(want.front()));
        if (want != shown || file != shownFile) {
            if (file + edits != failed) startLoad(want);
        } else if (edits != shownEdits && scene) {   // only the edits changed: re-apply in place
            resetMeshes(r, *scene);
            applyEdits(r, *scene, want.front());
            shownEdits = edits;
            // The sounds: volumes change as they play; a new sound (or the home's own back on) is decoded aside.
            HomeEdits e;
            std::string ep;
            homeEditsFor(*scene, want.front(), e, ep);
            const std::string sp = soundPathFor(ep, e.sound), want2 = sp + "|" + stamp(sp);
            soundBase[0] = e.sound.homeVolume;
            soundBase[1] = e.sound.volume;
            if (want2 == playingSound && (e.sound.homeVolume > 0) == homeSoundOn) {
                ambience.gain = e.sound.homeVolume * g_volume;
                addedSound.gain = e.sound.volume * g_volume;
            } else {
                if (soundJob.joinable()) soundJob.join();
                soundReady = false;
                playingSound = want2;
                homeSoundOn = e.sound.homeVolume > 0;
                soundJob = std::thread([&, amb = scene->ambientOgg, e, ep] { soundNext = loadSounds(amb, e, ep); soundReady = true; });
            }
        }
    };
    checkHome();
    if (loadState == 0) LOG("no home to show yet: put one in %s", homeos::envs().string().c_str());
    auto lastCheck = std::chrono::steady_clock::now();

    // System mode draws a little wider than the lens shows: the compositor turns the last frame with your head until the
    // next one, and the margin is what it turns into. It follows how fast your head turns: small while you look about
    // slowly (the pixels go to the view itself: sharper), wider the moment you turn fast (no edge ever shows), easing
    // back after. FH_WIDEN=x fixes it.
    const float widenFixed = std::getenv("FH_WIDEN") ? static_cast<float>(std::atof(std::getenv("FH_WIDEN"))) : 0.0f;
    float widen = system ? (widenFixed > 0 ? widenFixed : 1.12f) : 1.0f;
    Mat4 proj[2] = {eyeProjection(vr::Eye_Left, widen), eyeProjection(vr::Eye_Right, widen)};
    float lastHeadR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double widenSum = 0;
    uint64_t widenN = 0;
    const Mat4 eyeToHead[2] = {fromHmd34(vr::VRSystem()->GetEyeToHeadTransform(vr::Eye_Left)),
                               fromHmd34(vr::VRSystem()->GetEyeToHeadTransform(vr::Eye_Right))};
    auto start = std::chrono::steady_clock::now();      // the shader clock (Animations off holds it)
    const auto loopEpoch = start;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    Mat4 head = {};
    head.m[0] = head.m[5] = head.m[10] = head.m[15] = 1;
    head.m[13] = 1.6f;
    bool running = true;
    uint64_t frames = 0;
    auto lastFrame = std::chrono::steady_clock::now();
    // Auto resolution: the GPU time of the last frames against the display's budget, every 2 s.
    double gpuSum = 0;
    int gpuN = 0;
    auto lastTune = std::chrono::steady_clock::now(), settleUntil = lastTune + std::chrono::seconds(4);
    int calmWindows = 0;                             // auto resolution: calm GPU windows in a row
    float fovExtra = 0;                              // foveation strength added for the GPU budget (half levels)
    ThermalSensors thermal;                          // the headset's temperature, and the strength added to hold it
    ThermalGovernor heat;
    auto lastHeat = std::chrono::steady_clock::now();
    auto applyFoveation = [&] { r.setFoveationStrength(std::min(3.0f, fovBase() + fovExtra + heat.extra())); };
    auto lastDown = lastTune;
    bool gazeOk = false;
    float gazeSmooth[2][2] = {{-1, -1}, {-1, -1}};   // pixels: restarted when the resolution changes
    uint32_t gazeW = 0;
    uint64_t gpuSeen = 0;
    bool restart = false, lostDevice = false;
    float animTime = 0, animLength = 60;
    auto animTick = std::chrono::steady_clock::now();
    SystemPacer pacer;                               // system mode: frames timed to the compositor's
    FrameProfile prof;                               // FH_PROFILE=1: a line a second
    Publisher publisher;                             // system mode: frames out the moment the GPU is done
    // A new render size (Settings, or auto resolution); system mode needs new images for the compositor. When those
    // can't be had (memory), back to the size before; failing that too, no frames go out (Aurora shows) rather
    // than the layer re-aiming the last ones forever.
    bool exportLost = false;
    auto rescale = [&](float to) {
        const float was = scale;
        scale = to;
        r.resize(scaled(baseW), scaled(baseH));
        if (!system) return;
        publisher.drain();
        if (env.createImages(r)) return;
        LOG("no frame images for the compositor at %.0f%%: back to %.0f%%", to * 100, was * 100);
        scale = was;
        r.resize(scaled(baseW), scaled(baseH));
        if (!env.createImages(r)) { LOG("no frame images for the compositor at all: Aurora shows until Frame Home restarts"); exportLost = true; }
    };
    // New settings (settings.json changed, or the shown world's own): resolution and foveation apply now, the rest by
    // starting again (the renderer is built for them). fovWas: the foveation strength before.
    std::string worldStamp;
    auto applySettings = [&](const PerfSettings& next, float fovWas) {
        if (next == perf && std::abs(fovBase() - fovWas) < 1e-3f) return;
        const auto now = std::chrono::steady_clock::now();
        LOG("settings now: %s", next.toJson().c_str());
        if (next.needsRestart(perf)) { restart = true; running = false; }
        // (auto resolution's extra foveation goes with it when Auto is turned off)
        if (std::abs(fovBase() - fovWas) > 1e-3f || next.autoResolution != perf.autoResolution) { fovExtra = 0; applyFoveation(); }
        if (!next.autoResolution && (perf.autoResolution || std::abs(next.resolution - perf.resolution) > 1e-3f)) {
            rescale(next.resolution);
            settleUntil = now + std::chrono::seconds(3);
        }
        loco.setOptions(next.walk, next.teleport, next.turn, next.snapAngle, next.moveSpeed);
        if (std::abs(next.volume - perf.volume) > 1e-3f) {
            g_volume = next.volume;
            ambience.gain = soundBase[0] * g_volume;
            addedSound.gain = soundBase[1] * g_volume;
        }
        perf = next;
    };
    if (system && r.hasFrameSerial()) publisher.start(r, env);
    int statLate = 0, statPublished = 0;              // stats.json: frames handed to the compositor / after the moment it took one
    auto published = [&](uint64_t ns, double leadMs, bool late) {
        if (!(env.sinceSampled() > 1.0)) { pacer.addLead(leadMs); statLate += late ? 1 : 0; ++statPublished; }   // (idle frames aren't timed to anything)
        prof.published(ns, leadMs, late);
    };
    uint64_t frameStartNs = 0, frameTarget = 0;
    const float vsyncToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);
    if (system) LOG("display: %.0f Hz, %.1f ms from vsync to photons", hz, vsyncToPhotons * 1000);
    auto loopStart = std::chrono::steady_clock::now();
    double animMs = 0;
    Window gpuWindow;                                // auto resolution: this window's GPU times
    auto lastStats = std::chrono::steady_clock::now();   // stats.json: written every 2 s from these
    double statGpu = 0, statCpu = 0;
    int statGpuN = 0, statFrames = 0;
    std::vector<GuideVertex> overlayVerts;            // the debug world overlay (built per home)
    float vignetteAmt = 0;                           // comfort vignette, 0..1, eased
    float lastRig[16] = {}, lastRigYaw = 0;
    bool haveLastRig = false;
    float frozenT = -1;                              // Settings > Animations off: the clock the home holds at
    bool dashboard = false;                          // system mode: the Steam menu is open
    uint64_t dashFrames = 0;
    auto dashChecked = std::chrono::steady_clock::now();
    while (running) {
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof(ev)))
            if (ev.eventType == vr::VREvent_Quit) { vr::VRSystem()->AcknowledgeQuit_Exiting(); running = false; }
        if (!running) break;

        // A parsed home goes to the GPU a few milliseconds a frame (system mode: once faded to black), so frames keep
        // flowing at the display rate while it uploads.
        if ((loadState == 2 || loadState == 3) && !uploading && (!system || !scene || fade <= 0.01f)) {
            worker.join();
            if (loadState == 2) {
                r.setDynamicMeshes(pending->animatedMeshes);   // Clay / glTF animation: re-uploaded each frame
                r.beginLoad(*pending);
                scene.reset();                                 // the old home's meshes are gone from the GPU now
                uploading = true;
                uploadStart = std::chrono::steady_clock::now();
            }
        }
        // Nothing of the home on screen (faded to black for the switch, or nobody looking): it goes up in big steps
        // (a long frame shows nobody anything), not the few milliseconds a shown frame can spare. Idle (a frame a
        // second) that took a home 13 s.
        const bool unseen = system && (fade <= 0.01f || env.sinceSampled() > 1.0);
        r.setUploadsUnseen(unseen);
        if (uploading && r.loadStep(system ? (unseen ? 40.0 : std::max(1.0, 1000.0 / hz * 0.45)) : 6.0)) uploading = false, uploadDone = true;
        if (texturesUntil != std::chrono::steady_clock::time_point{} && loadState == 0) {
            const bool late = std::chrono::steady_clock::now() > texturesUntil;
            if (r.texturesPending() == 0 || late) {
                if (late) LOG("textures still loading after 8 s: showing the home anyway");
                texturesUntil = {};
                fadeTarget = 1;
            } else {
                fadeTarget = 0;
            }
        }
        if (uploadDone || loadState == 3) {
            const bool ok = uploadDone && r.meshCount() > 0;
            uploadDone = false;
            const std::string edits = stamp(editsPathFor(pendingHome.front())), file = fileSig(pendingHome) + stamp(worldPathFor(pendingHome.front()));
            const double uploadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - uploadStart).count();
            if (ok) LOG("home uploaded in %.2f s while drawing", uploadSec);
            if (ok) {
                scene = std::move(pending);
                fadeTarget = 1;
                // Every texture now, not as things come into view: turning round never waits on one (no pop-in, no
                // decode in the middle of looking around). System mode stays black until they're in (8 s at most).
                r.requestAllTextures();
                if (system && r.texturesPending() > 0) { fadeTarget = 0; texturesUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8); }
                // The home's ambience (Clay's _BACKGROUND_LOOP.ogg, or an HSL home's FMOD bed), a background sound
                // added in Frame Studio, and the animation clock.
                if (soundJob.joinable()) soundJob.join();       // (the last home's: never played over this one)
                soundReady = false;
                playSounds(ambience, addedSound, pendingSounds);
                soundBase[0] = pendingSounds.settings.homeVolume;
                soundBase[1] = pendingSounds.settings.volume;
                playingSound = pendingSounds.addedPath + "|" + stamp(pendingSounds.addedPath);
                homeSoundOn = pendingSounds.settings.homeVolume > 0;
                pendingSounds = HomeSounds();
                animTime = 0;
                animLength = scene->animDuration > 0 ? scene->animDuration : 60.0f;
                shown = pendingHome;
                shownFile = file;
                shownEdits = edits;
                {   // this world's own settings (none: the headset's)
                    worldStamp = stamp(worldSettingsPathFor(shown.front()));
                    const WorldSettings nw = worldFor(shown);
                    if (nw != world) {
                        const float fovWas = fovBase();
                        world = nw;
                        LOG("the world's own settings: %s", world.describe().c_str());
                        applySettings(world.apply(global), fovWas);
                    }
                }
                applyEdits(r, *scene, shown.front());
                loco.setWorld(std::move(pendingWalk));
                pendingWalk = HomeWorld();
                overlayDirty = true;
                {   // what it's made of and what's wrong with it, for the Debug page (and logs/report.txt to read)
                    pendingReport.loadSec += uploadSec;
                    std::error_code ec;
                    if (FILE* f = homeos::openFile(rootDir() / "report.json.part", "w")) {
                        const std::string j = pendingReport.json();
                        std::fwrite(j.data(), 1, j.size(), f);
                        std::fclose(f);
                        fs::rename(rootDir() / "report.json.part", rootDir() / "report.json", ec);
                    }
                    if (FILE* f = homeos::openFile(rootDir() / "logs" / "report.txt", "w")) {
                        const std::string t = pendingReport.text();
                        std::fwrite(t.data(), 1, t.size(), f);
                        std::fclose(f);
                    }
                    if (pendingReport.worst() >= 1)
                        LOG("report: %zu things to look at (logs/report.txt), first: %s", pendingReport.problems.size(), pendingReport.problems.front().what.c_str());
                    pendingReport = HomeReport();
                }
                dock.setWorld(scene->world);
                LOG("showing %s (%zu spawns, %zu seats, %zu colliders)", shown.front().c_str(), scene->world.spawns.size(),
                    scene->world.seats.size(), scene->world.colliders.size());
            } else {
                LOG("cannot show %s: %s", pendingHome.front().c_str(), loadState == 3 ? loadErr.c_str() : "nothing renderable");
                failed = file + edits;
                if (loadState != 3) r.unload();               // whatever of it did upload
                pending.reset();
            }
            loadState = 0;
            fadeTarget = 1;
        }
        const auto now = std::chrono::steady_clock::now();
        if (soundReady.exchange(false)) {             // sounds decoded after an edit: they play now
            if (soundJob.joinable()) soundJob.join();
            playSounds(ambience, addedSound, soundNext);
            soundNext = HomeSounds();
        }
        if (loadState == 0 && now - lastCheck > std::chrono::seconds(1)) {
            lastCheck = now;
            checkHome();
            // Settings changed? Resolution and foveation strength apply now; the rest needs a restart.
            const std::string st = stamp(settingsPath.string());
            PerfSettings g = global;
            if (st != settingsStamp && g.fromJson(readText(settingsPath))) {   // unreadable (mid-edit): tried again next second
                settingsStamp = st;
                const float fovWas = fovBase();
                global = g;
                applySettings(world.apply(global), fovWas);
            }
            // An APK home's own settings file changed (a .fenv's are in the file: a new one reloads the home).
            const std::string ws = shown.empty() ? std::string() : stamp(worldSettingsPathFor(shown.front()));
            if (!shown.empty() && ws != worldStamp) {
                worldStamp = ws;
                const WorldSettings nw = worldFor(shown);
                if (nw != world) {
                    const float fovWas = fovBase();
                    world = nw;
                    LOG("the world's own settings: %s", world.describe().c_str());
                    applySettings(world.apply(global), fovWas);
                }
            }
            // Debug tools changed? All of them apply now.
            const std::string ds = stamp(debugPath.string());
            DebugSettings nd;
            if (ds != debugStamp && (ds.empty() || nd.fromJson(readText(debugPath)))) {
                debugStamp = ds;
                if (!(nd == dbg)) {
                    LOG("debug: %s", nd.toJson().c_str());
                    if (nd.world != dbg.world) overlayDirty = true;
                    r.setDebugView(nd.view);
                    r.debugHide = nd.hideBits();
                    dbg = nd;
                }
            }
        }
        // Loading, or textures still going to the GPU: its time isn't what the home costs.
        if (loadState != 0 || uploading || r.texturesPending() > 0 || texturesUntil != std::chrono::steady_clock::time_point{})
            settleUntil = now + std::chrono::seconds(3);
        // The Cost view times every draw (it slows the GPU), and the overdraw view draws everything: neither says what the
        // home costs, so auto resolution holds still meanwhile.
        if (r.debugView() == HslRenderer::kViewCost || r.debugView() == HslRenderer::kViewOverdraw) settleUntil = now + std::chrono::seconds(3);
        if (overlayDirty && scene && loadState == 0 && !uploading) {   // the world overlay for the home shown
            overlayDirty = false;
            overlayVerts.clear();
            if (dbg.world) debuggeom::build(loco.world(), loco.walkGrid(), overlayVerts);
            r.setStaticGuide(overlayVerts);
            if (dbg.world) LOG("debug: world overlay %zu triangles", overlayVerts.size() / 3);
            overlayVerts = std::vector<GuideVertex>();
        }
        // System mode: a new size means new images for the compositor, so adjust less often there.
        // (auto resolution in system mode: 8 s, every new size is new images for the compositor; foveation alone: 2 s)
        if (now - lastTune > std::chrono::seconds(system && perf.autoResolution ? 8 : 2)) {
            lastTune = now;
            // (at half rate each frame has two display frames' time)
            const double avg = gpuN ? gpuSum / gpuN : 0, budget = 1000.0 / hz * (system && perf.frameRate == 1 ? 2 : 1);
            gpuSum = 0;
            gpuN = 0;
            const double p95 = gpuWindow.pct(0.95);
            gpuWindow.clear();
            if (perf.autoResolution && avg > 0 && now > settleUntil) {
                // Every new size is new images for the compositor (a hitch), so the size settles: GPU time follows
                // the pixel count (scale squared), each change aims for the middle of the band at once, going down
                // is immediate, going up needs two calm windows in a row, a while after the last step down, and a
                // size predicted to stay in the band. Over budget, a stronger foveation comes first (no new images).
                // System mode shares the GPU with the compositor's own work in the same frame: aim lower.
                const double hi = system ? 0.6 : 0.85, lo = system ? 0.4 : 0.6, mid = (hi + lo) / 2;
                float next = scale;
                if (avg > budget * hi || p95 > budget * (hi + 0.25)) {
                    calmWindows = 0;
                    lastDown = now;
                    if (r.foveationActive() && fovBase() + fovExtra < 3) {
                        fovExtra += 0.5f;
                        applyFoveation();
                        LOG("auto resolution: GPU %.2f ms (p95 %.2f) of %.2f: foveation %.2g first", avg, p95, budget, r.foveationStrength());
                        settleUntil = now + std::chrono::seconds(2);
                    } else {
                        next = std::min(scale - 0.05f, static_cast<float>(scale * std::sqrt(budget * mid / std::max(avg, p95 * 0.8))));
                        next = std::max(next, scale * 0.8f);
                    }
                } else if (avg < budget * lo && ++calmWindows >= 2 && now - lastDown > std::chrono::seconds(45)) {
                    calmWindows = 0;
                    if (fovExtra > 0) {
                        fovExtra = std::max(0.0f, fovExtra - 0.5f);
                        applyFoveation();
                        LOG("auto resolution: GPU %.2f ms of %.2f: foveation back to %.2g", avg, budget, r.foveationStrength());
                        settleUntil = now + std::chrono::seconds(2);
                    } else {
                        next = std::min(scale * 1.10f, static_cast<float>(scale * std::sqrt(budget * mid / avg)));
                    }
                } else if (avg >= budget * lo) {
                    calmWindows = 0;
                }
                next = std::clamp(std::floor(next * 20 + 1e-3f) / 20, PerfSettings::kAutoMin, PerfSettings::kAutoMax);   // 5% steps
                if (std::abs(next - scale) > 1e-3f) {
                    LOG("auto resolution: GPU %.2f ms (p95 %.2f) of %.2f: %.0f%% -> %.0f%%", avg, p95, budget, scale * 100, next * 100);
                    rescale(next);
                    settleUntil = now + std::chrono::seconds(2);
                }
            } else if (!perf.autoResolution && r.foveationActive() && avg > 0 && now > settleUntil) {
                // A fixed resolution stays as chosen, but a home too heavy for it would drop frames (stutter): the edges
                // of view get coarser first (stronger foveation; where you look stays sharp), and go back when there's room.
                const double hi = system ? 0.65 : 0.9, lo = system ? 0.4 : 0.6;
                if ((avg > budget * hi || p95 > budget * (hi + 0.25)) && fovBase() + fovExtra < 3) {
                    fovExtra += 0.5f;
                    applyFoveation();
                    LOG("fixed resolution, GPU %.2f ms (p95 %.2f) of %.2f: foveation %.2g", avg, p95, budget, r.foveationStrength());
                    settleUntil = now + std::chrono::seconds(2);
                    calmWindows = 0;
                    lastDown = now;
                } else if (avg < budget * lo && fovExtra > 0 && ++calmWindows >= 3 && now - lastDown > std::chrono::seconds(10)) {
                    calmWindows = 0;
                    fovExtra = std::max(0.0f, fovExtra - 0.5f);
                    applyFoveation();
                    LOG("fixed resolution, GPU %.2f ms of %.2f: foveation back to %.2g", avg, budget, r.foveationStrength());
                    settleUntil = now + std::chrono::seconds(2);
                } else if (avg >= budget * lo) {
                    calmWindows = 0;
                }
            }
        }
        // The thermal governor, once a second: above the temperature limit the edges of view give up a little detail
        // at a time (the foveation strength), and get it back once the headset has cooled.
        if (thermal.available() && r.foveationActive() && now - lastHeat >= std::chrono::seconds(1)) {
            const float dt = std::chrono::duration<float>(now - lastHeat).count();
            lastHeat = now;
            const float before = heat.extra();
            heat.update(thermal.hottest(), static_cast<float>(perf.tempLimit), dt);
            if (heat.extra() != before) {
                applyFoveation();
                LOG("thermal: %.1f C (limit %d): foveation %.2f", heat.smoothed(), perf.tempLimit, r.foveationStrength());
            }
        }
        // How it's doing, every 2 s, for the Settings and Debug pages and Frame Studio (stats.json).
        if (now - lastStats > std::chrono::seconds(2)) {
            const double secs = std::chrono::duration<double>(now - lastStats).count(), budget = 1000.0 / hz;
            lastStats = now;
            std::string j = "{";
            char b[768];
            if (statGpuN) {
                std::snprintf(b, sizeof b, "\"gpuMs\":%.2f,", statGpu / statGpuN);
                j += b;
            }
            const HslRenderer::FrameStats& rs = r.lastStats();
            std::snprintf(b, sizeof b,
                          "\"budgetMs\":%.2f,\"scale\":%.2f,\"width\":%u,\"height\":%u,\"foveation\":%s,\"eyeTracking\":%s,\"msaa\":%d,"
                          "\"cpuMs\":%.2f,\"fps\":%.1f,\"draws\":%u,\"triangles\":%llu,\"meshes\":%zu,\"drawn\":%zu,\"texturesLoading\":%zu,"
                          "\"ramMB\":%.0f,\"dashboard\":%s,\"debugView\":%d,\"loading\":%s,\"foveationLevel\":%d,\"tempC\":%.1f,\"margin\":%.3f,\"late\":%d,\"shownFps\":%.1f,\"home\":",
                          budget, scale, r.width(), r.height(), r.foveationActive() ? "true" : "false", gazeOk ? "true" : "false", perf.msaa,
                          statFrames ? statCpu / statFrames : 0.0, statFrames / secs, rs.draws, static_cast<unsigned long long>(rs.triangles), r.meshCount(),
                          r.drawnCount(), r.texturesPending(), homeos::ramMB(), dashboard ? "true" : "false", r.debugView(),
                          loadState != 0 || uploading ? "true" : "false", r.foveationActive() ? static_cast<int>(std::lround(r.foveationStrength())) : 0,
                          thermal.available() ? thermal.hottest() : 0.0f, widenN ? widenSum / widenN : widen, statLate, statPublished / secs);
            statLate = statPublished = 0;
            widenSum = 0;
            widenN = 0;
            j += b;
            j += HomeReport::q(shown.empty() ? std::string() : fs::path(shown.front()).filename().string());
            j += ",\"world\":" + HomeReport::q(world.describe()) + ",\"fovStrength\":" + std::to_string(r.foveationStrength());
            if (r.debugView() == HslRenderer::kViewCost && scene) {   // the costliest meshes, for the Debug page
                j += ",\"costly\":[";
                const auto top = r.costliest(8);
                for (size_t i = 0; i < top.size(); ++i) {
                    const int si = top[i].second;
                    const std::string nm = si >= 0 && static_cast<size_t>(si) < scene->meshes.size() ? scene->meshes[si].name : std::string("?");
                    std::snprintf(b, sizeof b, "%s{\"ms\":%.3f,\"name\":", i ? "," : "", top[i].first);
                    j += b + HomeReport::q(nm) + "}";
                }
                j += "]";
            }
            j += "}\n";
            statGpu = 0; statGpuN = 0; statCpu = 0; statFrames = 0;
            if (FILE* f = homeos::openFile(statsPath.string() + ".part", "w")) {
                std::fwrite(j.data(), 1, j.size(), f);
                std::fclose(f);
                std::error_code rec;
                fs::rename(statsPath.string() + ".part", statsPath, rec);
            }
        }

        if (system) {
            // No WaitGetPoses for a process that isn't the scene app: pace to the display and predict the pose for
            // when the compositor shows this frame (it re-aims it by rotation anyway).
            env.poll();
            const bool layer = env.connect();
            if (env.refusedFlags()) {   // an older layer, or a compositor that can't sample them: plain images instead
                LOG("the compositor can't read the frames with flags 0x%x: restarting with plain ones", env.refusedFlags());
                homeos::setEnv("FH_EXPORT_PLAIN", "1");
                restart = true;
                running = false;
            }
            env.state()->show = scene && !exportLost ? 1 : 0;
            const float dtFade = 1.0f / hz / 0.3f;   // 0.3 s fades
            fade = fadeTarget > fade ? std::min(fadeTarget, fade + dtFade) : std::max(fadeTarget, fade - dtFade);
            env.state()->fade = static_cast<uint32_t>(fade * 1000);
            // Nobody looking (a game is running, the compositor is idle, or no layer yet): a frame a second keeps one
            // fresh for the layer (it goes back to Aurora only after 5 s without one) and leaves the GPU to the game;
            // waking every 50 ms catches Aurora's slot being drawn again, and then it's full rate. No layer yet:
            // every 100 ms (it connects once a second).
            const bool idle = !layer || env.sinceSampled() > 1.0;
            // The Steam menu is open: the home is only the dimmed backdrop behind it, so it's drawn every other
            // compositor frame (the layer re-aims the last one) and the GPU goes to the menu and the apps in it.
            // Auto resolution waits it out: the GPU is shared then, and its times would only lower the home's size.
            if (now - dashChecked > std::chrono::milliseconds(250)) {
                dashChecked = now;
                dashboard = vr::VROverlay() && vr::VROverlay()->IsDashboardVisible();
            }
            if (dashboard && perf.menuRate == 0) settleUntil = now + std::chrono::seconds(2);
            // Settings > Frame rate: Half draws every other frame all the time (as Meta's home does).
            if (!idle && ((dashboard && perf.menuRate == 0) || perf.frameRate == 1) && (++dashFrames & 1)) {
                std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / hz));
                continue;
            }
            // (an idle home still loads at full speed: uploading, or its textures' 8 s window)
            const bool loadingIn = uploading || texturesUntil != std::chrono::steady_clock::time_point{};
            if (idle && !layer && !loadingIn) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            else if (idle && !loadingIn)
                for (int i = 0; i < 20 && env.sinceSampled() > 1.0 && running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // One frame per compositor frame, timed to be finished just before the compositor samples it (see
            // SystemPacer): the frame is made (CPU, then GPU) and published the moment the GPU is done.
            pacer.period = 1.0 / hz;
            float sinceVsync = 0;
            uint64_t vsyncFrame = 0;
            if (vr::VRSystem()->GetTimeSinceLastVsync(&sinceVsync, &vsyncFrame) && sinceVsync >= 0 && sinceVsync < 1.0f)
                pacer.vsyncNs = envshare::nowNs() - static_cast<uint64_t>(sinceVsync * 1e9);
            for (const Publisher::Done& d : publisher.take()) published(d.ns, d.leadMs, d.late);
            pacer.observe(env.state()->sampledNs.load());
            uint64_t startNs = 0;
            float predict = 0;
            pacer.plan(envshare::nowNs(), vsyncToPhotons, startNs, frameTarget, predict);
            if (!idle) {
                const uint64_t before = envshare::nowNs();
                sleepUntilNs(startNs);
                if (prof.on) prof.sleep.add((envshare::nowNs() - before) / 1e6);
            }
            frameStartNs = envshare::nowNs();
            if (frameStartNs > startNs) predict = std::max(0.0f, predict - (frameStartNs - startNs) / 1e9f);   // started late
            vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, predict, poses, vr::k_unMaxTrackedDeviceCount);
            r.exportSlot = env.beginFrame();
        } else {
            vr::VRCompositor()->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
            frameStartNs = envshare::nowNs();
        }
        loco.setPoseValid(poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid);
        if (poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
            head = fromHmd34(poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
        const auto frameNow = std::chrono::steady_clock::now();
        const float frameDt = std::chrono::duration<float>(frameNow - lastFrame).count();
        loco.update(frameDt, head.m);
        lastFrame = frameNow;
        Mat4 rig;
        loco.rig(rig.m);
        dock.update(rig.m);
        const Mat4 headWorld = mul(rig, head);
        if (system && widenFixed <= 0) {
            // head turn rate (rad/s) from the rotation since the last frame: angle of R_prev^T * R
            float tr = 0;
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k) tr += lastHeadR[c * 3 + k] * head.m[c * 4 + k];
            for (int c = 0; c < 3; ++c) for (int k = 0; k < 3; ++k) lastHeadR[c * 3 + k] = head.m[c * 4 + k];
            const float ang = std::acos(std::clamp((tr - 1) * 0.5f, -1.0f, 1.0f));
            const float rate = frameDt > 1e-4f ? ang / std::max(frameDt, 1.0f / hz) : 0.0f;
            // The frame is shown up to ~2 refreshes after its pose was predicted (more when only every other one is
            // drawn): the margin covers that much turning at the lens edge (tan grows ~2.5x per radian out there).
            const bool half = (dashboard && perf.menuRate == 0) || perf.frameRate == 1;
            const float hold = (half ? 3.0f : 2.0f) / hz;
            const float want = std::clamp(1.03f + rate * hold * 2.5f, half ? 1.05f : 1.03f, 1.18f);
            const float next = want > widen ? want : std::max(want, widen - frameDt * 0.25f);   // up at once, down over ~0.4 s
            if (std::abs(next - widen) > 0.002f) {
                widen = next;
                for (int e = 0; e < 2; ++e) proj[e] = eyeProjection(e == 0 ? vr::Eye_Left : vr::Eye_Right, widen);
            }
            widenSum += widen;
            ++widenN;
        }
        EyeView eyes[2];
        for (int e = 0; e < 2; ++e) eyes[e] = eyeFromPose(mul(headWorld, eyeToHead[e]), proj[e]);
        if (r.foveationActive()) {
            // Where each eye looks, in its image: the gaze (eye tracked), else straight ahead through the lens.
            float go[3], gt[3];
            gazeOk = perf.foveation == 2 && loco.gaze(go, gt);
            for (int e = 0; e < 2; ++e) {
                const Mat4 eyeT = mul(head, eyeToHead[e]);   // eye pose in standing space
                float d[3] = {0, 0, -1};                     // eye space: straight ahead
                if (gazeOk) {
                    const float wd[3] = {gt[0] - eyeT.m[12], gt[1] - eyeT.m[13], gt[2] - eyeT.m[14]};   // from this eye to the gaze point
                    for (int k = 0; k < 3; ++k) d[k] = eyeT.m[k * 4] * wd[0] + eyeT.m[k * 4 + 1] * wd[1] + eyeT.m[k * 4 + 2] * wd[2];   // R^T * dir
                }
                float px = r.width() * 0.5f, py = r.height() * 0.5f;
                if (d[2] < -1e-3f) {
                    const float* P = proj[e].m;
                    const float nx = (P[0] * d[0] + P[8] * d[2]) / -d[2], ny = (P[5] * d[1] + P[9] * d[2]) / -d[2];
                    px = (std::clamp(nx, -1.0f, 1.0f) * 0.5f + 0.5f) * r.width();
                    py = (std::clamp(ny, -1.0f, 1.0f) * 0.5f + 0.5f) * r.height();
                }
                // A light filter against tracker noise (saccades still land within a frame or two).
                if (gazeSmooth[e][0] < 0 || gazeW != r.width()) { gazeSmooth[e][0] = px; gazeSmooth[e][1] = py; }
                gazeSmooth[e][0] += (px - gazeSmooth[e][0]) * 0.6f;
                gazeSmooth[e][1] += (py - gazeSmooth[e][1]) * 0.6f;
                r.setFoveationCenter(static_cast<uint32_t>(e), gazeSmooth[e][0], gazeSmooth[e][1]);
            }
            gazeW = r.width();
        }
        float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        // Settings > Animations off: the home holds still where it was (its shader clock and its baked loops).
        if (perf.animations == 0) { if (frozenT < 0) frozenT = t; t = frozenT; }
        else if (frozenT >= 0) { start += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(t - frozenT)); t = frozenT; frozenT = -1; }
        {   // Clay / glTF loops are baked on the CPU here (HSL homes animate in their shaders on the running clock)
            const auto an = std::chrono::steady_clock::now();
            if (perf.animations) animTime += std::min(0.1f, std::chrono::duration<float>(an - animTick).count());
            animTick = an;
            if (animLength > 0 && animTime >= animLength) animTime = std::fmod(animTime, animLength);
            if (scene && scene->animate && !scene->animatedMeshes.empty()) {
                if (perf.animations || frozenT < 0 || frames < 2) scene->animate(animTime, scene->meshes);
                t = animTime;
            }
            animMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - an).count();
        }
        if (system) g_audioMuted = env.sinceSampled() > 1.0;   // the environment isn't on screen (a game, passthrough)
        {   // the teleport arc, landing and seat markers, drawn into this frame (so they show in system mode too)
            guideVerts.clear();
            const float gt = std::chrono::duration<float>(frameNow - loopEpoch).count();
            if (perf.seatMarkers) buildGuide(loco.guide(), &headWorld.m[12], gt, guideVerts);
            else {                                   // only the seat you point at (or aim the arc on)
                LocoGuide g = loco.guide();
                g.seats.erase(std::remove_if(g.seats.begin(), g.seats.end(), [](const LocoGuide::Seat& st) { return !st.hot; }), g.seats.end());
                buildGuide(g, &headWorld.m[12], gt, guideVerts);
            }
            // Comfort vignette: how fast the rig (not your body) moves and turns this frame. Jumps (a teleport, a
            // snap turn, a seat) aren't motion you watch, so they don't count.
            const float dtv = std::min(0.05f, std::max(1e-4f, frameDt));
            const float rigYaw = std::atan2(-rig.m[8], rig.m[0]);
            float want = 0;
            if (haveLastRig) {
                const float dx = rig.m[12] - lastRig[12], dy = rig.m[13] - lastRig[13], dz = rig.m[14] - lastRig[14];
                const float move = std::sqrt(dx * dx + dz * dz + dy * dy * 0.25f);
                float dyaw = std::fabs(rigYaw - lastRigYaw);
                if (dyaw > 3.14159f) dyaw = 6.28318f - dyaw;
                if (move < 0.3f) want = std::max(want, std::clamp(move / dtv / 1.6f, 0.0f, 1.0f));
                if (dyaw < 0.3f) want = std::max(want, std::clamp(dyaw / dtv / 1.4f, 0.0f, 1.0f));
            }
            std::copy(rig.m, rig.m + 16, lastRig);
            lastRigYaw = rigYaw;
            haveLastRig = true;
            vignetteAmt += (want - vignetteAmt) * (1 - std::exp(-dtv * (want > vignetteAmt ? 10.0f : 4.0f)));
            if (perf.vignette > 0 && vignetteAmt > 0.01f) {
                const float* hw = headWorld.m;
                const guide::P eye{hw[12], hw[13], hw[14]}, f{-hw[8], -hw[9], -hw[10]}, u{hw[0], hw[1], hw[2]}, w{hw[4], hw[5], hw[6]};
                const float strong = perf.vignette >= 2 ? 1.0f : 0.0f;
                guide::vignette(guideVerts, eye, f, u, w, 0.95f - 0.35f * vignetteAmt - 0.15f * strong, 1.35f - 0.25f * vignetteAmt - 0.2f * strong,
                                std::min(1.0f, vignetteAmt * (0.75f + 0.25f * strong)));
            }
            r.setGuide(guideVerts);
        }
        if (prof.on) prof.pre.add((envshare::nowNs() - frameStartNs) / 1e6);   // this frame's main-thread work before render()
        r.render(eyes, t);
        if (r.deviceLost()) {   // a GPU reset (a hang anywhere on the GPU) took the device: start again with a new one
            LOG("the GPU was reset and this process lost its device: restarting");
            restart = lostDevice = true;
            running = false;
            break;
        }
        statCpu += (envshare::nowNs() - frameStartNs) / 1e6;
        ++statFrames;
        if (dbg.shot != shotSeen) {   // Debug > Save a Picture: the left eye as it was just drawn, to logs/
            shotSeen = dbg.shot;
            std::vector<u8> px;
            if (dbg.shot >= startedAt && r.readback(0, px) && px.size() >= static_cast<size_t>(r.width()) * r.height() * 4) {
                char name[64];
                const std::time_t tt = std::time(nullptr);
                std::strftime(name, sizeof name, "frame-%Y%m%d-%H%M%S.png", std::localtime(&tt));
                const std::string path = (rootDir() / "logs" / name).string();
                std::error_code ec;
                fs::create_directories(rootDir() / "logs", ec);
                const bool ok = stbi_write_png(path.c_str(), static_cast<int>(r.width()), static_cast<int>(r.height()), 4, px.data(), static_cast<int>(r.width() * 4)) != 0;
                LOG("debug: picture %s %s", path.c_str(), ok ? "saved" : "couldn't be saved");
            }
        }

        if (system) {
            // Hand the frame to the compositor layer as soon as the GPU has finished it, with the eye poses it was
            // rendered from (publishing it only after the next frame's render() cost a whole refresh of latency).
            float vpOut[2][16], eyeOut[2][3];
            for (int e = 0; e < 2; ++e) {
                const Mat4 eyeT = mul(head, eyeToHead[e]);   // tracking space
                Mat4 rot = rigidInverse(eyeT);
                rot.m[12] = rot.m[13] = rot.m[14] = 0;
                const Mat4 vp = mul(proj[e], rot);
                std::memcpy(vpOut[e], vp.m, sizeof vpOut[e]);
                for (int k = 0; k < 3; ++k) eyeOut[e][k] = eyeT.m[12 + k];
            }
            if (r.hasFrameSerial()) {
                Publisher::Job j;
                j.serial = r.frameSerial();
                j.slot = r.exportSlot;
                std::memcpy(j.vp, vpOut, sizeof j.vp);
                std::memcpy(j.eye, eyeOut, sizeof j.eye);
                j.startNs = frameStartNs;
                j.target = frameTarget;
                publisher.push(j);
            } else {                                     // no timeline semaphores: wait here
                r.waitFrame();
                env.publish(r.exportSlot, vpOut, eyeOut);
                const uint64_t pubNs = envshare::nowNs();
                published(pubNs, (pubNs - frameStartNs) / 1e6, frameTarget && pubNs > frameTarget);
            }
        }
        for (int e = 0; e < 2 && !system; ++e) {
            vr::VRVulkanTextureArrayData_t vk = {};
            vk.m_nImage = reinterpret_cast<uint64_t>(r.colorImage());
            vk.m_pDevice = r.device();
            vk.m_pPhysicalDevice = r.physicalDevice();
            vk.m_pInstance = r.instance();
            vk.m_pQueue = r.queue();
            vk.m_nQueueFamilyIndex = r.queueFamily();
            vk.m_nWidth = r.width();
            vk.m_nHeight = r.height();
            vk.m_nFormat = VK_FORMAT_R8G8B8A8_SRGB;
            vk.m_nSampleCount = 1;
            vk.m_unArrayIndex = static_cast<uint32_t>(e);
            vk.m_unArraySize = 2;
            vr::Texture_t tex = {&vk, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
            vr::EVRCompositorError ce = vr::VRCompositor()->Submit(e == 0 ? vr::Eye_Left : vr::Eye_Right, &tex, nullptr,
                                                                   vr::Submit_VulkanTextureWithArrayData);
            if (ce != vr::VRCompositorError_None && frames < 3) LOG("Submit(eye %d): %d", e, static_cast<int>(ce));
        }
        const bool gpuNew = r.gpuResults() != gpuSeen;
        if (gpuNew) { gpuSeen = r.gpuResults(); gpuSum += r.lastGpuMs(); ++gpuN; gpuWindow.add(r.lastGpuMs()); statGpu += r.lastGpuMs(); ++statGpuN; }
        {
            const auto le = std::chrono::steady_clock::now();
            prof.frame(r, std::chrono::duration<double, std::milli>(le - loopStart).count(), animMs, r.lastGpuMs(), gpuNew);
            loopStart = le;
            prof.report(system ? (uploading ? "system(loading)" : "system") : "scene");
        }
        if (++frames == 1) LOG("first frame submitted");
        if (frames % 5400 == 0) LOG("%llu frames", static_cast<unsigned long long>(frames));   // alive (about once a minute)
    }
    if (worker.joinable()) worker.join();
    if (soundJob.joinable()) soundJob.join();
    publisher.finish();
    dock.stop();
    // SteamVR first: its client holds our Vulkan device for the frames it composited, and cleaning up after a
    // device we'd already destroyed crashed every exit (game launches, restarts).
    vr::VR_Shutdown();
    if (system) env.release(r);
    r.shutdown();
    if (restart) {   // settings that build the renderer differently: start again as the same program (SteamVR
                     // knows it by its path, so it's still the environment)
        if (lostDevice) std::this_thread::sleep_for(std::chrono::seconds(2));   // (a GPU still resetting: not straight back into it)
        LOG("restarting %s", lostDevice ? "with a new GPU device" : "with the new settings");
        std::fflush(stderr);
        homeos::setEnv("FH_RESTARTED", "1");
        homeos::restartSelf();
    }
    return 0;
}

int runRegister(const char* manifest) {
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Utility);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 1; }
    int rc = 1;
    std::error_code ec;
    if (fs::exists(manifest, ec)) {
        const std::string full = fs::absolute(manifest, ec).u8string();
        vr::EVRApplicationError ae = vr::VRApplications()->AddApplicationManifest(full.c_str(), false);
        LOG("AddApplicationManifest(%s): %s", full.c_str(), vr::VRApplications()->GetApplicationsErrorNameFromEnum(ae));
        rc = ae == vr::VRApplicationError_None ? 0 : 1;
    } else {
        LOG("no such manifest: %s", manifest);
    }
    vr::VR_Shutdown();
    return rc;
}

// SteamVR's own settings that a PC install changes (steamvr section), kept in <root>/steamvr-before.json the first
// time so an uninstall puts back exactly what was there.
struct SteamVrBefore { bool homeApp = false, dome = false; int envMode = 1; std::string background; float camHeight = 1.6f, domeRadius = 0; };
void saveSteamVrBefore() {
    const fs::path f = rootDir() / "steamvr-before.json";
    std::error_code ec;
    if (fs::exists(f, ec)) return;                   // (already saved: what was there before Frame Home, not since)
    vr::IVRSettings* s = vr::VRSettings();
    vr::EVRSettingsError se = vr::VRSettingsError_None;
    char bg[2048] = {};
    s->GetString(vr::k_pch_SteamVR_Section, "background", bg, sizeof bg, &se);
    std::string esc;
    for (const char* c = bg; *c; ++c) { if (*c == '\\' || *c == '"') esc += '\\'; esc += *c; }
    char j[4096];
    std::snprintf(j, sizeof j, "{\"enableHomeApp\": %s, \"environmentMode\": %d, \"background\": \"%s\", \"backgroundUseDomeProjection\": %s, "
                               "\"backgroundCameraHeight\": %.3f, \"backgroundDomeRadius\": %.3f}\n",
                  s->GetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", &se) ? "true" : "false", s->GetInt32(vr::k_pch_SteamVR_Section, "environmentMode", &se),
                  esc.c_str(), s->GetBool(vr::k_pch_SteamVR_Section, "backgroundUseDomeProjection", &se) ? "true" : "false",
                  s->GetFloat(vr::k_pch_SteamVR_Section, "backgroundCameraHeight", &se), s->GetFloat(vr::k_pch_SteamVR_Section, "backgroundDomeRadius", &se));
    fs::create_directories(rootDir(), ec);
    if (FILE* o = homeos::openFile(f, "w")) { std::fputs(j, o); std::fclose(o); }
}
void restoreSteamVrBefore() {
    const fs::path f = rootDir() / "steamvr-before.json";
    std::error_code ec;
    SteamVrBefore b;
    try {
        const tinyjson::Value d = tinyjson::parse(readText(f));
        b.homeApp = d["enableHomeApp"].boolVal; b.envMode = static_cast<int>(d["environmentMode"].asFloat()); b.background = d["background"].asString();
        b.dome = d["backgroundUseDomeProjection"].boolVal; b.camHeight = d["backgroundCameraHeight"].asFloat(); b.domeRadius = d["backgroundDomeRadius"].asFloat();
    } catch (...) {}                                  // (none saved: SteamVR's defaults)
    vr::IVRSettings* s = vr::VRSettings();
    vr::EVRSettingsError se = vr::VRSettingsError_None;
    s->SetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", b.homeApp, &se);
    s->SetInt32(vr::k_pch_SteamVR_Section, "environmentMode", b.envMode, &se);
    s->SetString(vr::k_pch_SteamVR_Section, "background", b.background.c_str(), &se);
    s->SetBool(vr::k_pch_SteamVR_Section, "backgroundUseDomeProjection", b.dome, &se);
    s->SetFloat(vr::k_pch_SteamVR_Section, "backgroundCameraHeight", b.camHeight, &se);
    s->SetFloat(vr::k_pch_SteamVR_Section, "backgroundDomeRadius", b.domeRadius, &se);
    fs::remove(f, ec);
    fs::remove_all(rootDir() / "background", ec);
}

// Makes Frame Home SteamVR's environment: registers it under the home-app key and turns the home app on,
// so vrserver starts it whenever no game is running (and again after one quits). --uninstall reverts both.
// Frame Home takes SteamVR's home-app slot (openvr.tool.steamvr_environments): SteamVR itself runs it as the
// headset's environment whenever no game is running — not a game, not an extra app — and brings it back when a game
// quits. The manifest is written first; with SteamVR not running this returns 3 and the installer (install.sh)
// registers it in SteamVR's config files instead.
int runInstall(bool install) {
    const fs::path manifest = rootDir() / "frame-home.vrmanifest";
    std::string self;
    if (install) {
        self = homeos::selfPath().string();
        if (self.empty()) { LOG("cannot find my own path"); return 1; }
        // The installed copy is what SteamVR runs. The installer runs this from its own folder on purpose: SteamVR
        // knows apps by binary path, and the installed path would make this process pass for the running home.
        std::error_code ec;
        const fs::path installed = rootDir() / "bin" / homeos::exeName("frame-home");
        if (fs::exists(installed, ec)) self = installed.string();
        fs::create_directories(homeos::envs(), ec);
        std::string json = self;                     // (JSON: a Windows path's backslashes doubled)
        for (size_t at = 0; (at = json.find('\\', at)) != std::string::npos; at += 2) json.insert(at, 1, '\\');
        std::ofstream m(manifest);
        m << "{\n  \"source\": \"builtin\",\n  \"applications\": [ {\n"
          << "    \"app_key\": \"" << kHomeAppKey << "\",\n"
          << "    \"launch_type\": \"binary\",\n"
          << "    \"binary_path_linux\": \"" << json << "\",\n"
          << "    \"binary_path_linux_arm\": \"" << json << "\",\n"
          << "    \"binary_path_windows\": \"" << json << "\",\n"
          << "    \"arguments\": \"\",\n"
          << "    \"is_dashboard_overlay\": false,\n"
          // The environment, not an app: kept out of SteamVR's app lists and its dock.
          << "    \"is_hidden\": true,\n"
          << "    \"is_internal\": true,\n"
          << "    \"strings\": { \"en_us\": { \"name\": \"Frame Home\",\n"
          << "      \"description\": \"Meta homes as SteamVR's environment\" } }\n"
          << "  } ]\n}\n";
    }
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Utility);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s (SteamVR not running)", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 3; }
    int rc = 0;
    vr::EVRSettingsError se = vr::VRSettingsError_None;
    if (install) {
        if (vr::VRApplications()->IsApplicationInstalled(kHomeAppKey)) {
            char path[4096] = {};
            vr::EVRApplicationError pe = vr::VRApplicationError_None;
            vr::VRApplications()->GetApplicationPropertyString(kHomeAppKey, vr::VRApplicationProperty_BinaryPath_String, path, sizeof path, &pe);
            if (std::string(path) != self) LOG("replacing the existing environment app (%s)", path);
        }
        vr::EVRApplicationError ae = vr::VRApplications()->AddApplicationManifest(manifest.u8string().c_str(), false);
        LOG("AddApplicationManifest(%s): %s", manifest.u8string().c_str(), vr::VRApplications()->GetApplicationsErrorNameFromEnum(ae));
        if (ae != vr::VRApplicationError_None) rc = 1;
        // An older Frame Home still running: replace it with the installed one.
        const bool replaced = !homeos::runningCopies("frame-home").empty();
        homeos::stopCopies("frame-home");
        const bool wasEnabled = vr::VRSettings()->GetBool(vr::k_pch_SteamVR_Section, "enableHomeApp");
#ifdef _WIN32
        // A Windows PC: SteamVR's classic environment (its background: the panel makes it a picture of the home)
        // instead of Aurora, for the moments before Frame Home draws.
        saveSteamVrBefore();
        vr::VRSettings()->SetInt32(vr::k_pch_SteamVR_Section, "environmentMode", 0, &se);
#endif
        vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", true, &se);
        // Turning the setting on makes SteamVR start the home app itself; when it was already on, start it now
        // unless a game is running (SteamVR brings the home back when the game quits).
        homeos::sleepMs(1500);
        if ((wasEnabled || replaced) && homeos::runningCopies("frame-home").empty() &&
            vr::VRApplications()->GetSceneApplicationState() == vr::EVRSceneApplicationState_None) {
            const vr::EVRApplicationError le = vr::VRApplications()->LaunchApplication(kHomeAppKey);
            LOG("start: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(le));
        }
    } else {
#ifdef _WIN32
        restoreSteamVrBefore();                      // the settings as they were before Frame Home
#else
        vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", false, &se);
#endif
        std::error_code ec;
        if (fs::exists(manifest, ec)) {
            vr::EVRApplicationError ae = vr::VRApplications()->RemoveApplicationManifest(manifest.u8string().c_str());
            LOG("RemoveApplicationManifest: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(ae));
            fs::remove(manifest, ec);                // (the panel takes SteamVR's settings in hand only while this is here)
        }
        // Close a running Frame Home so SteamVR's own environment comes back.
        homeos::stopCopies("frame-home");
    }
    if (se != vr::VRSettingsError_None) { LOG("settings error %d", static_cast<int>(se)); rc = 1; }
    LOG("%s", rc ? "failed" : install ? "Frame Home is now the environment" : "SteamVR's environment restored");
    vr::VR_Shutdown();
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef FH_USE_VOLK
    if (volkInitialize() != VK_SUCCESS) { std::fprintf(stderr, "frame-home: no Vulkan on this system\n"); return 1; }
#endif
    std::string snapshot, panorama;
    std::vector<std::string> apks;                   // footprint [+ vista]
    bool list = false, fromSpawn = false, bench = false, unpaced = false;
    float cam[5] = {0, 1.6f, 0, 0, 0};
    uint32_t size = 1024, sizeH = 0;
    double benchSeconds = 20;
    float benchHz = 120;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--register" && i + 1 < argc) return runRegister(argv[++i]);
        if (a == "--version") { std::printf("frame-home %s, scene format %u\n", __DATE__, kSceneBakeVersion); return 0; }
        if (a == "--install") return runInstall(true);
        if (a == "--uninstall") return runInstall(false);
        if (a == "--list") list = true;
        else if (a == "--panorama" && i + 1 < argc) panorama = argv[++i];
        else if (a == "--snapshot" && i + 1 < argc) snapshot = argv[++i];
        else if (a == "--spawn") fromSpawn = true;
        else if (a == "--cam" && i + 1 < argc) std::sscanf(argv[++i], "%f,%f,%f,%f,%f", &cam[0], &cam[1], &cam[2], &cam[3], &cam[4]);
        else if (a == "--size" && i + 1 < argc) {   // N, or WxH (--bench)
            unsigned sw = 0, sh = 0;
            const int got = std::sscanf(argv[++i], "%ux%u", &sw, &sh);
            size = sw;
            sizeH = got == 2 ? sh : 0;
        }
        else if (a == "--bench") bench = true;
        else if (a == "--seconds" && i + 1 < argc) benchSeconds = std::atof(argv[++i]);
        else if (a == "--hz" && i + 1 < argc) benchHz = static_cast<float>(std::atof(argv[++i]));
        else if (a == "--unpaced") unpaced = true;
        else apks.push_back(a);
    }
    if (list && !apks.empty()) {                     // every mesh in a home: index, kind, name, shader
        {
            HslScene scene;
            std::string err;
            if (!loadHslScene(apks, scene, err)) { LOG("%s", err.c_str()); return 1; }
            for (size_t m = 0; m < scene.meshes.size(); ++m) {
                const MeshData& md = scene.meshes[m];
                std::printf("%3zu overlay=%d sky=%d tex=%ux%u verts=%u uv2=%zu lm=%d %s | %s\n", m, static_cast<int>(md.overlayKind),
                            md.isSkybox, md.hasTexture ? md.texW : 0, md.hasTexture ? md.texH : 0, md.nVerts, md.uvs2.size() / 2,
                            md.hasLightmap, md.name.c_str(), md.shaderPath.c_str());
                if (md.isSkybox && md.hasTexture && std::getenv("FH_DUMP_SKY")) {
                    std::ofstream f(std::getenv("FH_DUMP_SKY"), std::ios::binary);
                    f << "P6\n" << md.texW << " " << md.texH << "\n255\n";
                    for (size_t p = 0; p + 3 < md.texRGBA.size(); p += 4) f.write(reinterpret_cast<const char*>(&md.texRGBA[p]), 3);
                    // cube UVs: where each corner samples
                    for (u32 v = 0; v < md.nVerts && v < 24; ++v)
                        std::printf("    v%u pos (%.2f %.2f %.2f) uv (%.3f %.3f)\n", v, md.positions[v * 3], md.positions[v * 3 + 1],
                                    md.positions[v * 3 + 2], v * 2 + 1 < md.uvs.size() ? md.uvs[v * 2] : -1.f,
                                    v * 2 + 1 < md.uvs.size() ? md.uvs[v * 2 + 1] : -1.f);
                }
            }
            return 0;
        }
    }
    if (bench) {
        if (apks.empty()) { std::fprintf(stderr, "usage: frame-home --bench <footprint.apk> [vista.apk] [--seconds N] [--size WxH] [--hz N] [--unpaced]\n"); return 2; }
        const bool given = std::any_of(argv + 1, argv + argc, [](const char* x) { return std::strcmp(x, "--size") == 0; });
        return runBench(apks, benchSeconds, given ? size : 0, given ? (sizeH ? sizeH : size) : 0, benchHz, unpaced);
    }
    if (!panorama.empty() && !apks.empty()) return runPanorama(panorama, apks, cam, size >= 512 ? size : 8192, fromSpawn);
    if (!snapshot.empty()) {
        if (apks.empty()) {
            std::fprintf(stderr, "usage: frame-home --snapshot out.ppm [--cam x,y,z,yaw,pitch] [--size N] <footprint.apk> [vista.apk]\n");
            return 2;
        }
        return runSnapshot(snapshot, apks, cam, size, fromSpawn);
    }
    if (std::getenv("FH_SYSTEM") || (argc > 1 && std::string(argv[1]) == "--system")) return runVr({}, true);
    return runVr(apks);                              // no home given: environment mode
}
