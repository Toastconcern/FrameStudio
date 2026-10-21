// fenv-tool — convert homes and models into Frame environments (.fenv).
//
//   fenv-tool pack out.fenv [--name NAME] [--edits edits.json] [--thumb thumb.png] [--keep-apks] [--fast] [--no-optimize] <input>...
//       --fast: pack in parallel chunks (several times quicker, ~3% bigger; see fenv_lzma.h)
//       --no-optimize: keep what can't be seen from anywhere you can be (visibility_bake.h; pack, convert, upgrade)
//       inputs: a home APK (HSL, Clay .opa, glTF .ovrscene), a footprint + its vista, models (.glb .gltf .fbx
//       .obj). Every home is baked (scene.bin: opens several times faster; a Clay / glTF home's animation goes in as
//       data, anim_bake.h). --keep-apks stores the source APKs as well.
//   fenv-tool info <file.fenv | file.fbundle>   a bundle: its environments, their names and sizes
//   fenv-tool bundle out.fbundle [--name NAME] file.fenv...   several environments in one file, to share or install
//                                            together (fenv_bundle.h); each stays its own environment
//   fenv-tool unbundle file.fbundle outdir   the bundle's environments as .fenv files (one there already is kept:
//                                            the same one is skipped, a different one makes this "name 2.fenv")
//   fenv-tool convert outdir <input>...      one .fenv per input ("a.apk+b.apk" pairs a footprint with a vista)
//   fenv-tool upgrade file.fenv...           re-bake a .fenv's scene with this build, everything else kept (one that
//                                            loads from its homes is baked: its APKs, models and world.json give way)
//   fenv-tool report [--json] <input>...     what a home is made of, what it costs and what's wrong with it (exit 3: broken)
//   fenv-tool settings <home.fenv | home.apk> [JSON | --clear]   a world's own settings (world_settings.h), shown or set
//   fenv-tool tune [--seconds S] [--late F] [--cool C] [--dry-run] <home.fenv | home.apk>
//       the world's best settings on this headset (walked by frame-home --bench, paced at the display rate), stored in
//       the .fenv (or <apk>.settings.json); prints them as JSON last
#include "fenv.h"
#include "fenv_bundle.h"
#include "home_report.h"
#include "tex_mips.h"
#include "scene_bake.h"
#include "visibility_bake.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <set>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

FenvPacking g_packing = FenvPacking::Smallest;
bool g_optimize = true;

// The mesh optimizer for a bake (the object edits named keep their meshes whole).
bool optimize(const HslScene& scene, const std::string& editsFile, VisibilityResult& vis, BakeOptions& opt) {
    if (!g_optimize) return false;
    HomeEdits e;
    const bool hasEdits = !editsFile.empty() && loadHomeEdits(editsFile, e);
    return optimizeForBake(scene, hasEdits ? &e.objects : nullptr, vis, opt);
}

std::string stem(const std::string& p) { return fs::u8path(p).stem().u8string(); }

std::string displayName(const HslScene& s, const std::vector<std::string>& inputs) {
    std::string n;
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string part = i < s.sources.size() && !s.sources[i].package.empty() ? s.sources[i].package : stem(inputs[i]);
        const size_t dot = part.find_last_of('.');
        if (dot != std::string::npos && part.find("com.") == 0) part = part.substr(dot + 1);
        n += (n.empty() ? "" : " + ") + part;
    }
    return n;
}

int pack(const std::string& out, const std::vector<std::string>& inputs, std::string name, const std::string& editsFile,
         const std::string& thumbFile, bool keepApks) {
    const auto t0 = std::chrono::steady_clock::now();
    HslScene scene;
    std::string err;
    if (!loadHslScene(inputs, scene, err)) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    FenvManifest m;
    m.name = name.empty() ? displayName(scene, inputs) : name;
    std::vector<FenvMember> members;
    std::vector<u8> blob;
    VisibilityResult vis;
    BakeOptions opt;
    const bool optimized = optimize(scene, editsFile, vis, opt);
    const bool baked = bakeScene(scene, blob, err, optimized ? &opt : nullptr);
    if (baked) {
        m.scene = "scene.bin";
        FenvMember b;
        b.name = m.scene;
        b.bytes = std::move(blob);
        b.lzma = true;                   // LZMA (fenv_lzma.h): unpacked once, into the .fenv cache
        b.packing = g_packing;
        members.push_back(std::move(b));
    }
    for (size_t i = 0; i < inputs.size(); ++i) {
        const HslSourceInfo info = i < scene.sources.size() ? scene.sources[i] : HslSourceInfo{};
        if (info.role == "Model") {
            if (baked) continue;         // its meshes are in scene.bin
            const fs::path base = fs::u8path(inputs[i]).parent_path();
            const std::string dir = "models/" + stem(inputs[i]) + "/";
            const std::vector<std::string>& files = i < scene.sourceFiles.size() ? scene.sourceFiles[i] : std::vector<std::string>{inputs[i]};
            for (const std::string& f : files) {
                std::error_code ec;
                fs::path rel = fs::relative(fs::u8path(f), base, ec);
                if (ec || rel.empty() || rel.u8string().find("..") == 0) rel = fs::u8path(f).filename();
                FenvMember mm;
                mm.name = dir + rel.generic_u8string();
                mm.file = f;
                members.push_back(mm);
                if (fs::equivalent(fs::u8path(f), fs::u8path(inputs[i]), ec)) m.models.push_back(mm.name);
            }
            continue;
        }
        FenvSource src;
        src.file = "apks/" + fs::u8path(inputs[i]).filename().u8string();
        src.role = info.role; src.format = info.format; src.package = info.package; src.version = info.version;
        m.sources.push_back(src);
        if (!baked || keepApks) {
            FenvMember a;
            a.name = src.file;
            a.file = inputs[i];
            a.store = true;
            members.push_back(a);
        }
    }
    if (!editsFile.empty()) { m.edits = "edits.json"; FenvMember e; e.name = m.edits; e.file = editsFile; members.push_back(e); }
    if (!thumbFile.empty()) { m.thumbnail = "thumb.png"; FenvMember t; t.name = m.thumbnail; t.file = thumbFile; t.store = true; members.push_back(t); }
    if (!fenvWrite(out, m, members, err)) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    std::error_code ec;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s: \"%s\", %zu meshes, %s, %.0f MB, %.1f s\n", out.c_str(), m.name.c_str(), scene.meshes.size(),
                baked ? "baked" : ("from its homes (" + err + ")").c_str(), fs::file_size(fs::u8path(out), ec) / 1048576.0, s);
    return 0;
}

// A bundle's environments: name, member file and size, each read from the bundle in place.
int bundleInfo(const std::string& path) {
    FenvBundleManifest bundle;
    std::string err;
    if (!fenvBundleReadManifest(path, bundle, &err)) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    std::printf("%s\n  bundle: %s, %zu environment%s\n", path.c_str(), bundle.name.c_str(), bundle.environments.size(), bundle.environments.size() == 1 ? "" : "s");
    int rc = 0;
    for (const FenvBundleEntry& e : bundle.environments) {
        FenvManifest m;
        std::vector<u8> thumb;
        const bool readable = fenvBundleReadEnvironment(path, e.file, m, &thumb, &err);
        std::printf("  %-36s %8.1f MB  %s%s\n", e.name.c_str(), e.size / 1048576.0, e.file.c_str(),
                    !readable ? "  (damaged)" : thumb.empty() ? "" : "  (thumbnail)");
        if (!readable) rc = 1;
    }
    return rc;
}

int bundle(const std::string& out, const std::vector<std::string>& inputs, const std::string& name) {
    std::string err;
    int shown = -10;
    const bool ok = fenvBundleWrite(out, inputs, name, err, [&](uint64_t done, uint64_t total) {
        const int percent = total ? static_cast<int>(done * 100 / total) : 100;
        if (percent / 10 != shown / 10) { shown = percent; std::fprintf(stderr, "\r  %d%%", percent); }
    });
    std::fprintf(stderr, "\r       \r");
    if (!ok) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    return bundleInfo(out);
}

int unbundle(const std::string& path, const std::string& outDir) {
    std::vector<std::string> unpacked;
    std::string err;
    if (!fenvBundleExtract(path, outDir, unpacked, err)) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    for (const std::string& f : unpacked) std::printf("%s\n", f.c_str());
    return 0;
}

int info(const std::string& path) {
    if (isFenvBundlePath(path)) return bundleInfo(path);
    FenvManifest m;
    std::vector<u8> thumb;
    if (!fenvReadManifest(path, m, &thumb)) { std::fprintf(stderr, "fenv-tool: not a .fenv: %s\n", path.c_str()); return 1; }
    std::printf("%s\n  name: %s\n  %s\n", path.c_str(), m.name.c_str(), m.scene.empty() ? "loads from its homes" : "baked scene");
    for (const FenvSource& s : m.sources) std::printf("  %-9s %-17s %s %s\n", s.role.c_str(), s.format.c_str(), s.package.c_str(), s.version.c_str());
    for (const std::string& f : m.models) std::printf("  model     %s\n", f.c_str());
    std::printf("  edits: %s, thumbnail: %s\n", m.edits.empty() ? "no" : "yes", thumb.empty() ? "no" : "yes");
    std::printf("  settings: %s%s\n", m.settings.describe().c_str(), m.settings.tuned.empty() ? "" : (" (" + m.settings.tuned + ")").c_str());
    return 0;
}

// Re-bakes a .fenv's scene with this build (smaller texture levels baked in, tex_mips.h), keeping everything else in it
// (world, edits, sound, picture, models) byte for byte. One whose textures all have their levels already is left alone.
// One that loads from its homes (saved before Clay / glTF animation could be baked) is baked: the scene replaces its
// APKs, models and world.json (the world as it loads goes in the scene).
int upgrade(const std::string& path) {
    FenvManifest m;
    if (!fenvReadManifest(path, m)) { std::fprintf(stderr, "fenv-tool: not a .fenv: %s\n", path.c_str()); return 1; }
    const bool fromHomes = m.scene.empty();
    const auto t0 = std::chrono::steady_clock::now();
    HslScene scene;
    std::string err;
    if (!loadHslScene(std::vector<std::string>{path}, scene, err)) { std::fprintf(stderr, "fenv-tool: %s: %s\n", path.c_str(), err.c_str()); return 1; }
    size_t want = 0;
    {
        std::set<const TexSource*> seen;
        auto look = [&](const std::shared_ptr<const TexSource>& t) { if (t && seen.insert(t.get()).second && texmips::wants(*t)) ++want; };
        for (const MeshData& md : scene.meshes) {
            for (const auto* t : {&md.texSrc, &md.normalSrc, &md.ormSrc, &md.emissiveSrc, &md.lmLdrSrc}) look(*t);
            for (const auto& nt : md.namedTex) look(nt.second);
        }
    }
    VisibilityResult vis;
    BakeOptions opt;
    const bool optimized = optimize(scene, scene.fenvEdits, vis, opt) && vis.trisAfter < vis.trisBefore;
    if (!want && !optimized && !fromHomes) { std::printf("%s: already up to date\n", path.c_str()); return 0; }
    std::vector<u8> blob;
    if (!bakeScene(scene, blob, err, optimized ? &opt : nullptr)) { std::fprintf(stderr, "fenv-tool: %s: %s\n", path.c_str(), err.c_str()); return 1; }
    std::vector<FenvMember> members;
    const std::string world = m.world;
    if (fromHomes) { m.scene = "scene.bin"; m.world.clear(); m.models.clear(); }
    FenvMember b;
    b.name = m.scene;
    b.bytes = std::move(blob);
    b.lzma = true;
    b.packing = g_packing;
    members.push_back(std::move(b));
    {   // everything else, as it is
        mz_zip_archive z = {};
        if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) { std::fprintf(stderr, "fenv-tool: can't open %s\n", path.c_str()); return 1; }
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            const std::string name = st.m_filename;
            if (name == "fenv.json" || name == m.scene || name == m.scene + ".lz") continue;
            if (fromHomes && (name.rfind("apks/", 0) == 0 || name.rfind("models/", 0) == 0 || name == world)) continue;   // in the scene now
            FenvMember f;
            f.name = name;
            f.bytes = fenv_detail::readMember(z, name.c_str(), uint64_t(4) << 30);
            if (f.bytes.size() != st.m_uncomp_size) { mz_zip_reader_end(&z); std::fprintf(stderr, "fenv-tool: can't read %s in %s\n", name.c_str(), path.c_str()); return 1; }
            f.store = st.m_method == 0;   // stored before (APKs, pictures): stored again
            members.push_back(std::move(f));
        }
        mz_zip_reader_end(&z);
    }
    if (!fenvWrite(path, m, members, err)) { std::fprintf(stderr, "fenv-tool: %s\n", err.c_str()); return 1; }
    std::error_code ec;
    std::printf("%s: %s%zu textures given their smaller levels, %.0f MB, %.1f s\n", path.c_str(), fromHomes ? "baked from its homes, " : "", want, fs::file_size(fs::u8path(path), ec) / 1048576.0,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

// The files one home argument names: a footprint + vista written as a+b, or one file. A file that exists is taken
// whole even when its name holds a '+' (a converted pair's .fenv is named footprint+vista.fenv).
std::vector<std::string> homeFiles(const std::string& arg) {
    std::error_code ec;
    if (fs::is_regular_file(fs::u8path(arg), ec)) return {arg};
    std::vector<std::string> files;
    for (size_t p = 0, q; p <= arg.size(); p = q + 1) {
        q = arg.find('+', p);
        if (q == std::string::npos) q = arg.size();
        if (q > p) files.push_back(arg.substr(p, q - p));
    }
    return files;
}

// What a home is made of, what it costs and what's wrong with it (home_report.h): a footprint + vista as a+b.
int report(const std::string& arg, bool json) {
    const std::vector<std::string> group = homeFiles(arg);
    const auto t0 = std::chrono::steady_clock::now();
    HslScene scene;
    std::string err;
    if (!loadHslScene(group, scene, err)) { std::fprintf(stderr, "fenv-tool: %s: %s\n", arg.c_str(), err.c_str()); return 1; }
    const double loadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const HomeWorld walk = walkWorld(scene);
    HomeReport r = makeHomeReport(scene, &walk);
    r.loadSec = loadSec;
    const std::string out = json ? r.json() : r.text();
    std::fwrite(out.data(), 1, out.size(), stdout);
    return r.worst() >= 2 ? 3 : 0;
}


#ifndef _WIN32
// ── tune: a world's best settings, measured on this headset ──
// Frame Home's bench walks the world (FH_BENCH_WALK + FH_BENCH_MOVES: walking, snap turns, teleports, fast head turns)
// at candidate settings, paced at the display rate beside the live compositor, as the headset runs it. A candidate
// keeps up when the walk holds the display's rate with at most `--late` of its frames late. Your MSAA is kept: the
// foveation strength moves first (lower when there's room: sharper edges; higher when not), then the resolution, then
// half frame rate as the last resort.

// One paced walk through the world (frame-home --bench at the display rate, beside the live compositor as on the
// headset): whether it kept up. GPU cycles from the perf counters can't judge that: Adreno's counters are GPU-wide, so
// they also count the compositor's work, and an unpaced run doesn't meet the compositor at all.
struct TuneRun {
    bool ok = false;
    double fps = 0, gpuAvg = 0, gpuP95 = 0;
    int frames = 0, late = 0, hitches = 0;      // late: frames over the display period; hitches: over 1.5 of it
    double lateShare() const { return frames > 0 ? static_cast<double>(late) / frames : 1.0; }
};

// The hottest thermal sensor, in thousandths of a degree (0: none readable).
long hottest() {
    long t = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/thermal", ec)) {
        if (e.path().filename().u8string().rfind("thermal_zone", 0) != 0) continue;
        if (std::FILE* f = std::fopen((e.path() / "temp").c_str(), "rb")) {
            long v = 0;
            if (std::fscanf(f, "%ld", &v) == 1) t = std::max(t, v);
            std::fclose(f);
        }
    }
    return t;
}

std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// One paced walk through the world at these settings (the world's own settings left out).
TuneRun tuneRun(const std::string& bench, const std::string& home, float strength, float scale, int seconds, long coolBelow) {
    TuneRun r;
    // Waits for the headset to cool first: a hot GPU runs slower, and every candidate should start alike.
    for (int waited = 0; coolBelow > 0 && hottest() >= coolBelow && waited < 600; waited += 5) {
        if (waited % 30 == 0) { std::printf("tune: cooling (%.1f C)\n", hottest() / 1000.0); std::fflush(stdout); }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    // One GPU benchmark at a time on this headset.
    const int lock = ::open("/tmp/frame-gpu.lock", O_RDWR | O_CREAT, 0666);
    if (lock >= 0) ::flock(lock, LOCK_EX);
    char env[512];
    std::snprintf(env, sizeof env,
                  "FH_BENCH_WALK=1 FH_BENCH_MOVES=1 FH_BENCH_NOWORLD=1 FH_BENCH_OVERLAP=1 FH_BENCH_EARLY=1 "
                  "FH_BENCH_FOVSTRENGTH=%.3f FH_BENCH_SCALE=%.3f",
                  strength, scale);
    const std::string cmd = std::string(env) + " " + shellQuote(bench) + " --bench --seconds " + std::to_string(seconds) + " " +
                            shellQuote(home) + " 2>&1";
    if (std::FILE* p = ::popen(cmd.c_str(), "r")) {
        char line[8192];
        while (std::fgets(line, sizeof line, p)) {
            const std::string l = line;
            if (l.rfind("{\"home\"", 0) != 0) continue;
            try {
                const tinyjson::Value j = tinyjson::parse(l);
                r.fps = j["fps"].asFloat();
                r.frames = static_cast<int>(j["frames"].asFloat());
                r.late = static_cast<int>(j["overBudget"].asFloat());
                r.hitches = static_cast<int>(j["hitches"].asFloat());
                r.gpuAvg = j["gpuAvg"].asFloat();
                r.gpuP95 = j["gpuP95"].asFloat();
                r.ok = r.frames > 0;
            } catch (...) {}
        }
        ::pclose(p);
    }
    if (lock >= 0) { ::flock(lock, LOCK_UN); ::close(lock); }
    return r;
}

// The best-looking settings this headset keeps up with in this world, found by walking it (tuneRun) at candidates:
// first the headset's own; if they keep up, sharper foveation while it still does; if not, stronger foveation, then
// lower resolution, and as a last resort half frame rate. "Keeps up": the display's rate, at most `maxLate` of the
// frames late.
int tune(const std::string& home, const std::string& benchArg, int seconds, double maxLate, long coolBelow, bool dryRun, double hz) {
    std::error_code ec;
    std::string bench = benchArg;
    if (bench.empty()) bench = (fs::read_symlink("/proc/self/exe", ec).parent_path() / "frame-home").u8string();
    if (!fs::exists(fs::u8path(bench), ec)) { std::fprintf(stderr, "fenv-tool tune: no frame-home beside fenv-tool (--bench PATH)\n"); return 1; }
    const char* homeDir = std::getenv("HOME");
    PerfSettings headset;
    {
        std::string j;
        if (std::FILE* f = std::fopen((std::string(homeDir ? homeDir : ".") + "/frame-home/settings.json").c_str(), "rb")) {
            char b[4096];
            for (size_t k; (k = std::fread(b, 1, sizeof b, f)) > 0;) j.append(b, k);
            std::fclose(f);
        }
        if (!headset.fromJson(j)) headset = PerfSettings();
    }
    const float s0 = static_cast<float>(headset.foveationLevel), scale0 = headset.autoResolution ? 1.0f : headset.resolution;
    std::printf("tune: %s: the headset's resolution %.0f%%, %dx MSAA, foveation %.0f; walking it %d s at each try (%.0f fps, at most %.1f%% late)\n",
                fs::u8path(home).filename().u8string().c_str(), scale0 * 100, headset.msaa, s0, seconds, hz, maxLate * 100);
    std::fflush(stdout);
    std::map<std::pair<int, int>, TuneRun> seen;
    auto keepsUp = [&](const TuneRun& r) { return r.ok && r.fps >= hz * 0.998 && r.lateShare() <= maxLate; };
    auto measure = [&](float s, float sc) {
        const std::pair<int, int> key{static_cast<int>(std::lround(s * 100)), static_cast<int>(std::lround(sc * 100))};
        auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        const TuneRun r = tuneRun(bench, home, s, sc, seconds, coolBelow);
        if (r.ok) std::printf("tune: foveation %.2f, resolution %.0f%%: %.1f fps, %d of %d frames late, GPU %.2f ms (p95 %.2f) (%s)\n", s, sc * 100,
                              r.fps, r.late, r.frames, r.gpuAvg, r.gpuP95, keepsUp(r) ? "keeps up" : "too much");
        else std::printf("tune: foveation %.2f, resolution %.0f%%: the bench didn't run\n", s, sc * 100);
        std::fflush(stdout);
        seen[key] = r;
        return r;
    };
    float s = s0, sc = scale0;
    int frameRate = -1;
    TuneRun at = measure(s, sc);
    if (!at.ok) { std::fprintf(stderr, "fenv-tool tune: the bench didn't run (%s)\n", bench.c_str()); return 1; }
    if (keepsUp(at)) {   // room to spare: sharper edges, half a step at a time
        for (float t = s - 0.5f; t >= 1.0f - 1e-3f; t -= 0.5f) {
            const TuneRun r = measure(t, sc);
            if (!keepsUp(r)) break;
            s = t; at = r;
        }
    } else {
        bool ok = false;
        for (float t = s + 0.5f; t <= 3.0f + 1e-3f && !ok; t += 0.5f) { const TuneRun r = measure(t, sc); s = t; at = r; ok = keepsUp(r); }
        for (float c = sc - 0.1f; !ok && c >= std::max(0.7f, scale0 * 0.75f) - 1e-3f; c -= 0.1f) { const TuneRun r = measure(s, c); sc = c; at = r; ok = keepsUp(r); }
        if (!ok) frameRate = 1;   // drawn every other display frame: twice the time each
    }
    WorldSettings w;
    if (std::abs(s - s0) > 1e-3f) w.foveation = s;
    if (std::abs(sc - scale0) > 1e-3f) w.resolution = sc;
    if (frameRate >= 0) w.frameRate = frameRate;
    char when[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(when, sizeof when, "%Y-%m-%d", std::localtime(&now));
    char note[256];
    std::snprintf(note, sizeof note, "tuned %s: %.1f fps walking, %.1f%% of frames late%s", when, at.fps, at.lateShare() * 100,
                  frameRate == 1 ? ", even so over: half frame rate" : "");
    w.tuned = note;
    w.clamp();
    std::printf("tune: best: %s (%s)\n", w.describe().c_str(), w.tuned.c_str());
    if (!dryRun) {
        std::string err;
        if (!setWorldSettings(home, w, err)) { std::fprintf(stderr, "fenv-tool tune: %s\n", err.c_str()); return 1; }
        std::printf("tune: stored in %s\n", isFenvPath(home) ? home.c_str() : worldSettingsPathFor(home).c_str());
    }
    std::printf("{\"settings\": %s, \"fps\": %.1f, \"lateShare\": %.4f}\n", w.toJson().c_str(), at.fps, at.lateShare());
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "info")) return info(argv[2]);
    if (argc >= 4 && !std::strcmp(argv[1], "bundle")) {
        std::string name;
        std::vector<std::string> inputs;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
            else inputs.push_back(argv[i]);
        }
        if (!isFenvBundlePath(argv[2])) { std::fprintf(stderr, "fenv-tool bundle: the bundle's name must end in .fbundle: %s\n", argv[2]); return 2; }
        return bundle(argv[2], inputs, name);
    }
    if (argc >= 4 && !std::strcmp(argv[1], "unbundle")) return unbundle(argv[2], argv[3]);
    if (argc >= 3 && !std::strcmp(argv[1], "upgrade")) {
        for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--no-optimize")) g_optimize = false;
        int rc = 0;
        for (int i = 2; i < argc; ++i) if (std::strcmp(argv[i], "--no-optimize")) rc = std::max(rc, upgrade(argv[i]));
        return rc;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "report")) {
        int rc = 0;
        const bool json = argc >= 4 && !std::strcmp(argv[2], "--json");
        for (int i = json ? 3 : 2; i < argc; ++i) { if (i > (json ? 3 : 2) && !json) std::printf("\n"); rc = std::max(rc, report(argv[i], json)); }
        return rc;
    }
    if (argc >= 4 && !std::strcmp(argv[1], "pack")) {
        std::string name, edits, thumb;
        bool keep = false;
        std::vector<std::string> inputs;
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--name" && i + 1 < argc) name = argv[++i];
            else if (a == "--edits" && i + 1 < argc) edits = argv[++i];
            else if (a == "--thumb" && i + 1 < argc) thumb = argv[++i];
            else if (a == "--keep-apks") keep = true;
            else if (a == "--fast") g_packing = FenvPacking::Fast;
            else if (a == "--no-optimize") g_optimize = false;
            else inputs.push_back(a);
        }
        if (inputs.empty()) { std::fprintf(stderr, "fenv-tool pack: no inputs\n"); return 2; }
        return pack(argv[2], inputs, name, edits, thumb, keep);
    }
    if (argc >= 4 && !std::strcmp(argv[1], "convert")) {
        int failed = 0;
        std::error_code ec;
        fs::create_directories(fs::u8path(argv[2]), ec);
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--no-optimize") { g_optimize = false; continue; }
            const std::vector<std::string> group = homeFiles(a);
            std::string base;
            for (const auto& g : group) base += (base.empty() ? "" : "+") + stem(g);
            failed += pack((fs::u8path(argv[2]) / fs::u8path(base + ".fenv")).u8string(), group, "", "", "", false) != 0;
        }
        return failed ? 1 : 0;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "settings")) {   // shown, or set from JSON ("{}" / --clear: none)
        if (argc == 3) { const WorldSettings w = worldSettingsFor(argv[2]); std::printf("%s\n", w.toJson().c_str()); return 0; }
        WorldSettings w;
        if (std::strcmp(argv[3], "--clear") != 0 && !w.fromJson(argv[3])) { std::fprintf(stderr, "fenv-tool settings: not settings JSON: %s\n", argv[3]); return 2; }
        std::string err;
        if (!setWorldSettings(argv[2], w, err)) { std::fprintf(stderr, "fenv-tool settings: %s\n", err.c_str()); return 1; }
        std::printf("%s: %s\n", argv[2], w.describe().c_str());
        return 0;
    }
#ifndef _WIN32
    if (argc >= 3 && !std::strcmp(argv[1], "tune")) {
        std::string bench, home;
        int seconds = 30;                   // each candidate's walk
        double maxLate = 0.01, hz = 120;    // keeping up: the display's rate with at most 1% of frames late
        long cool = 80000;                  // each candidate starts below 80 C (as the bench scripts do)
        bool dry = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--bench" && i + 1 < argc) bench = argv[++i];
            else if (a == "--seconds" && i + 1 < argc) seconds = std::clamp(std::atoi(argv[++i]), 5, 300);
            else if (a == "--late" && i + 1 < argc) maxLate = std::clamp(std::atof(argv[++i]), 0.0, 0.5);
            else if (a == "--hz" && i + 1 < argc) hz = std::clamp(std::atof(argv[++i]), 30.0, 240.0);
            else if (a == "--cool" && i + 1 < argc) cool = static_cast<long>(std::atof(argv[++i]) * 1000);
            else if (a == "--dry-run") dry = true;
            else home = a;
        }
        if (home.empty()) { std::fprintf(stderr, "fenv-tool tune: which world?\n"); return 2; }
        return tune(home, bench, seconds, maxLate, cool, dry, hz);
    }
#endif
    std::fprintf(stderr,
                 "usage:\n  fenv-tool pack out.fenv [--name NAME] [--edits edits.json] [--thumb thumb.png] [--keep-apks] [--fast] <home.apk|vista.apk|model>...\n"
                 "  fenv-tool info <file.fenv | file.fbundle>\n"
                 "  fenv-tool bundle out.fbundle [--name NAME] file.fenv...   several environments in one file\n"
                 "  fenv-tool unbundle file.fbundle outdir   its environments as .fenv files\n"
                 "  fenv-tool convert outdir <home.apk | footprint.apk+vista.apk | model>...\n"
                 "  fenv-tool upgrade file.fenv...   re-bake with this build (keeps world, edits, sound, picture)\n"
                 "  fenv-tool report [--json] <home.apk | home.fenv | footprint.apk+vista.apk>...   what it costs and what's wrong with it\n"
                 "  fenv-tool settings <home.fenv | home.apk> [JSON | --clear]   a world's own settings, shown or set\n"
                 "  fenv-tool tune [--seconds 30] [--late 0.01] [--cool 80] [--dry-run] <home.fenv | home.apk>   its best settings on this headset, stored in it\n");
    return 2;
}
