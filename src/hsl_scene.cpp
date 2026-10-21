#include "hsl_scene.h"
#include "world_gen.h"
#include "surface_mesh.h"

#include "loaders/scene_loader.h"
#include "loaders/rendshad_parser.h"
#include "loaders/opa_loader.h"
#include "loaders/gltf_loader.h"
#include "clay_spirv.h"
#include "core/fsb5_vorbis.h"
#include "fenv.h"
#include "model_import.h"
#include "scene_bake.h"
#include "anim_bake.h"

#include <fstream>
#include <iterator>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <unordered_map>
#include <memory>
#include <set>
#include <thread>

namespace {

// Reads a whole zip entry; empty on failure.
std::vector<u8> extract(mz_zip_archive& z, int index) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, static_cast<mz_uint>(index), &st)) return {};
    std::vector<u8> out(static_cast<size_t>(st.m_uncomp_size));   // unpacked straight into place
    if (!mz_zip_reader_extract_to_mem(&z, static_cast<mz_uint>(index), out.data(), out.size(), 0)) return {};
    return out;
}

std::string surfaceName(const std::string& path) {
    size_t s = path.find(".surface");
    if (s == std::string::npos) return {};
    size_t b = path.rfind('/', s);
    return path.substr(b == std::string::npos ? 0 : b + 1, s + 8 - (b == std::string::npos ? 0 : b + 1));
}

// Scans assets/scene.zip: every RENDSHAD forward pass (shader programs) and the merged lightprobe
// ambient (lightprobes_merged.lprb, field2 = global SH; L00 / 3.5449 = ambient radiance).
void scanSceneZip(const std::string& apkPath, HslScene& scene) {
    const std::shared_ptr<const std::vector<u8>> sceneZip = scenezip::get(apkPath);   // the loader's copy
    if (!sceneZip || sceneZip->empty()) return;

    mz_zip_archive z = {};
    if (!mz_zip_reader_init_mem(&z, sceneZip->data(), sceneZip->size(), 0)) return;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string name = st.m_filename;
        std::string base = name.substr(name.find_last_of('/') + 1);

        // Any "<name>.surface/<hash>/shader*" entry: older homes keep theirs under meta/clay_shaders/0.0.1/, not /shaders/.
        if (name.find(".surface/") != std::string::npos && base.rfind("shader", 0) == 0) {
            std::vector<u8> data = extract(z, static_cast<int>(i));
            std::vector<SpirvBlob> blobs;
            if (!parseRendShadForward(data, blobs)) { std::fprintf(stderr, "[scene] shader %s: no forward pass parsed\n", name.c_str()); continue; }
            ShaderProgramSrc p;
            p.path = name.rfind("content/", 0) == 0 ? name.substr(8) : name;
            for (auto& b : blobs) (b.stageType == 0 ? p.vert : p.frag) = std::move(b.code);
            if (!p.vert.empty() && !p.frag.empty()) scene.programs[p.path] = std::move(p);
            else std::fprintf(stderr, "[scene] shader %s: %zu blob(s), vertex %s, fragment %s\n", name.c_str(), blobs.size(),
                              p.vert.empty() ? "missing" : "ok", p.frag.empty() ? "missing" : "ok");
        } else if (name.find("lightprobes_merged.lprb") != std::string::npos) {
            std::vector<u8> d = extract(z, static_cast<int>(i));
            if (d.size() < 80 || std::memcmp(d.data() + 4, "LPRB", 4) != 0) continue;
            auto rd32 = [&](size_t o) { u32 v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; };
            auto rd16 = [&](size_t o) { u16 v = 0; if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2); return v; };
            u32 root = rd32(0);
            u32 vt = root - rd32(root);
            if (rd16(vt) < 10) continue;
            u16 f2 = rd16(vt + 8);
            if (!f2) continue;
            u32 vec = root + f2 + rd32(root + f2);
            if (vec + 16 > d.size() || rd32(vec) < 3) continue;
            for (int c = 0; c < 3; ++c) {
                float v; std::memcpy(&v, d.data() + vec + 4 + c * 4, 4);
                scene.ambientRGB[c] = v / 3.5449f;
            }
            scene.hasEnvAmbient = true;
        }
    }
    mz_zip_reader_end(&z);
}

// AndroidManifest.xml (binary XML): the <manifest> element's package and versionName attributes.
static void readManifest(const std::string& apkPath, std::string& package, std::string& version) {
    mz_zip_archive apk = {};
    if (!mz_zip_reader_init_file(&apk, apkPath.c_str(), 0)) return;
    const int mi = mz_zip_reader_locate_file(&apk, "AndroidManifest.xml", nullptr, 0);
    const std::vector<u8> x = mi >= 0 ? extract(apk, mi) : std::vector<u8>();
    mz_zip_reader_end(&apk);
    auto rd16 = [&](size_t o) -> u32 { return o + 2 <= x.size() ? x[o] | x[o + 1] << 8 : 0; };
    auto rd32 = [&](size_t o) -> u32 { return o + 4 <= x.size() ? rd16(o) | rd16(o + 2) << 16 : 0; };
    if (x.size() < 16 || rd16(0) != 0x0003 || rd16(8) != 0x0001) return;
    // String pool: count, flags (0x100 = UTF-8), offsets.
    const size_t pool = 8, count = rd32(pool + 8), poolEnd = pool + rd32(pool + 4);
    const bool utf8 = rd32(pool + 16) & 0x100;
    const size_t strings = pool + rd32(pool + 20);
    auto str = [&](u32 i) -> std::string {
        if (i >= count) return {};
        size_t o = strings + rd32(pool + 28 + i * 4);
        std::string out;
        if (utf8) {
            if (o < x.size() && (x[o] & 0x80)) ++o;           // UTF-16 length (1-2 bytes), then the UTF-8 byte length
            ++o;
            size_t n = o < x.size() ? x[o] : 0;
            if (n & 0x80) { n = (n & 0x7F) << 8 | (o + 1 < x.size() ? x[o + 1] : 0); ++o; }
            ++o;
            if (o + n <= x.size()) out.assign(reinterpret_cast<const char*>(&x[o]), n);
        } else {
            size_t n = rd16(o);
            if (n & 0x8000) { n = (n & 0x7FFF) << 16 | rd16(o + 2); o += 2; }
            o += 2;
            for (size_t k = 0; k < n && o + 2 * k + 2 <= x.size(); ++k) { const u32 c = rd16(o + 2 * k); out += c < 0x80 ? static_cast<char>(c) : '?'; }
        }
        return out;
    };
    for (size_t c = poolEnd; c + 36 <= x.size();) {
        const u32 type = rd16(c), size = rd32(c + 4);
        if (size < 8) break;
        if (type == 0x0102 && str(rd32(c + 20)) == "manifest") {   // START_ELEMENT
            const size_t attrs = c + 16 + rd16(c + 24), stride = rd16(c + 26), n = rd16(c + 28);
            for (size_t a = 0; a < n; ++a) {
                const size_t at = attrs + a * stride;
                const std::string name = str(rd32(at + 4));
                const u32 raw = rd32(at + 8);
                const std::string value = raw != 0xFFFFFFFFu ? str(raw) : x.size() > at + 15 && x[at + 15] == 0x03 ? str(rd32(at + 16)) : std::string();
                if (name == "package") package = value;
                if (name == "versionName") version = value;
            }
            break;
        }
        c += size;
    }
}

}  // namespace

// The built-in programs Clay (.opa) homes are drawn with (tools/gen_clay_shader.py): libshell builds its Clay
// shaders natively, so - like Quest Home Editor - an unlit texture x vertex colour x tint program with real alpha,
// and its cut-out twin for AlphaTest materials.
constexpr const char* kClayProgram = "builtin/clay.surface/0/shader";
constexpr const char* kClayCutoutProgram = "builtin/clay_cutout.surface/0/shader";
constexpr const char* kClaySkinnedProgram = "builtin/clay_skinned.surface/0/shader";             // skinned on the GPU
constexpr const char* kClaySkinnedCutoutProgram = "builtin/clay_skinned_cutout.surface/0/shader";

// The home's looping ambience: an Ogg Vorbis entry of scene.zip (Clay homes ship _BACKGROUND_LOOP.ogg).
static std::vector<u8> findAmbientOgg(const std::string& apkPath, std::string& source, std::string& name) {
    const std::shared_ptr<const std::vector<u8>> zipData = scenezip::get(apkPath);   // the loader's copy
    std::vector<u8> best;
    mz_zip_archive z = {};
    if (!zipData || zipData->empty() || !mz_zip_reader_init_mem(&z, zipData->data(), zipData->size(), 0)) return {};
    bool bestIsLoop = false;
    std::vector<int> banks;                         // HSL homes: the FMOD banks hold the same bed (see fsb5_vorbis.h)
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        std::string n = st.m_filename;
        for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.find(".bank/") != std::string::npos && n.find(".strings.bank/") == std::string::npos)
            banks.push_back(static_cast<int>(i));  // the home's own bank and shared ones (nativespacesmain)
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".ogg") != 0) continue;
        const bool loop = n.find("background") != std::string::npos || n.find("loop") != std::string::npos;
        if (!best.empty() && (bestIsLoop || !loop)) continue;
        best = extract(z, static_cast<int>(i));
        bestIsLoop = loop;
        name = st.m_filename;                                          // its file, without the folders and ".ogg"
        if (const size_t sl = name.find_last_of('/'); sl != std::string::npos) name = name.substr(sl + 1);
        if (name.size() > 4) name.resize(name.size() - 4);
    }
    if (!best.empty()) source = "Ogg loop";
    if (best.empty() && !banks.empty()) {
        std::vector<std::vector<u8>> data;
        std::vector<const std::vector<u8>*> ptrs;
        for (int b : banks) data.push_back(extract(z, b));
        for (const auto& d : data) ptrs.push_back(&d);
        std::string why;
        best = fsb5::ambienceOgg(ptrs, &why);
        std::fprintf(stderr, "[scene] ambience from the FMOD bank: %s (%s)\n", best.empty() ? "unsupported" : "Ogg Vorbis rebuilt",
                     why.c_str());
        if (!best.empty()) {
            source = "FMOD bank";
            const size_t q0 = why.find('"'), q1 = q0 == std::string::npos ? q0 : why.find('"', q0 + 1);   // the sound's name in the bank
            name = q1 != std::string::npos ? why.substr(q0 + 1, q1 - q0 - 1) : std::string();
        }
    }
    mz_zip_reader_end(&z);
    return best;
}

// Desert Terrace 210's palms (desertterrace_palms_v8.fbx, palmtreefoliage) ship wound inside-out and without
// normals. Their shader discards back faces (doubleSided 0), so the trunks drew their far, inner walls (black) and
// the fronds lost their lit side. Rendered with the order reversed, trunks and fronds match the textures. Only this
// shader reads gl_FrontFacing among the homes checked, and Meta's own meshes (authored normals) wind correctly.
static bool insideOutPalms(const MeshData& md) {
    if (md.shaderPath.find("palmtreefoliage") == std::string::npos) return false;
    for (float n : md.normals) if (n != 0) return false;
    return true;
}

// Which meshes a loader's animate() changes: sample the loop at a few times and compare each mesh's
// positions / uvs / tint / rigid motion with the t=0 pose. Only those re-upload vertices every frame.
static std::vector<int> findAnimatedMeshes(const std::function<void(float, std::vector<MeshData>&)>& animate,
                                           std::vector<MeshData>& meshes, float duration) {
    auto sig = [](const MeshData& m) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const u8* b = static_cast<const u8*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        mix(m.positions.data(), m.positions.size() * 4);
        mix(m.uvs.data(), m.uvs.size() * 4);
        mix(m.curTint, sizeof m.curTint);
        mix(m.animXform, sizeof m.animXform);
        mix(m.uvXform, sizeof m.uvXform);
        mix(m.skinJoints.data(), m.skinJoints.size() * 4);
        return h;
    };
    animate(0.0f, meshes);
    std::vector<uint64_t> base(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i) base[i] = sig(meshes[i]);
    std::vector<char> moved(meshes.size(), 0);
    const float d = duration > 0 ? duration : 10.0f;
    for (float t : {0.1f, 0.5f, d * 0.37f, d * 0.71f}) {
        animate(t, meshes);
        for (size_t i = 0; i < meshes.size(); ++i) if (sig(meshes[i]) != base[i]) moved[i] = 1;
    }
    animate(0.0f, meshes);
    std::vector<int> out;
    for (size_t i = 0; i < meshes.size(); ++i) if (moved[i]) out.push_back(static_cast<int>(i));
    return out;
}

static void addClayPrograms(HslScene& scene) {
    ShaderProgramSrc p;
    p.path = kClayProgram;
    p.vert.assign(std::begin(kClayVertSpirv), std::end(kClayVertSpirv));
    p.frag.assign(std::begin(kClayFragSpirv), std::end(kClayFragSpirv));
    scene.programs[p.path] = p;
    p.path = kClayCutoutProgram;
    p.frag.assign(std::begin(kClayFragCutoutSpirv), std::end(kClayFragCutoutSpirv));
    scene.programs[p.path] = p;
    p.path = kClaySkinnedCutoutProgram;
    p.vert.assign(std::begin(kClaySkinnedVertSpirv), std::end(kClaySkinnedVertSpirv));
    scene.programs[p.path] = p;
    p.path = kClaySkinnedProgram;
    p.frag.assign(std::begin(kClayFragSpirv), std::end(kClayFragSpirv));
    scene.programs[p.path] = p;
}

// A .fenv: its baked scene when it has one (instant), else its homes and models through the normal loaders.
static bool loadFenv(const std::string& path, HslScene& scene, std::string& error) {
    FenvManifest m;
    std::vector<std::string> sources, models;
    std::string edits;
    if (!fenvExtract(path, fenvCacheDir(), m, sources, models, edits, error)) return false;
    if (!m.scene.empty()) {
        std::ifstream f(std::filesystem::u8path(m.scene), std::ios::binary | std::ios::ate);
        std::vector<u8> blob(f ? static_cast<size_t>(f.tellg()) : 0);   // in one read (an istreambuf copy was 750 ms for 316 MB)
        f.seekg(0);
        if (!blob.empty() && !f.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()))) blob.clear();
        if (!unbakeScene(blob.data(), blob.size(), scene, error)) { error += ": " + path; return false; }
        scene.sourceFiles.resize(scene.apkPaths.size());
    } else {
        std::vector<std::string> all = sources;
        all.insert(all.end(), models.begin(), models.end());
        if (all.empty()) { error = "empty environment: " + path; return false; }
        if (!loadHslScene(all, scene, error)) return false;
    }
    scene.fenvPath = path;
    scene.fenvEdits = edits;
    if (!m.world.empty()) {   // the environment's own world layer (edited in Frame Studio) replaces the converted one
        std::ifstream wf(std::filesystem::u8path(m.world));
        const std::string wj((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
        HomeWorld w;
        if (worldFromJson(wj, w)) scene.world = std::move(w);
    }
    std::fprintf(stderr, "[scene] environment %s (%s): %zu meshes\n", m.name.c_str(), m.scene.empty() ? "from its homes" : "baked", scene.meshes.size());
    return true;
}

bool loadHslScene(const std::vector<std::string>& apkPaths, HslScene& scene, std::string& error) {
    if (apkPaths.size() == 1 && isFenvPath(apkPaths[0])) {
        if (!loadFenv(apkPaths[0], scene, error)) return false;
        simplifyWorldMeshes(scene.world);
        return true;
    }
    scene = HslScene{};
    scene.apkPaths = apkPaths;
    g_deferTextures = true;
    const scenezip::Keep keepSceneZips(apkPaths);    // each scene.zip unpacked once for the whole load
    {
        std::lock_guard<std::mutex> l(g_sceneFogMutex);
        g_sceneFog = SceneFog{};
    }                     // textures stay compressed; the renderer streams them in
    const auto t0 = std::chrono::steady_clock::now();
    // Footprint and vista parse in parallel (each decodes its own textures).
    std::vector<std::unique_ptr<SceneLoader>> loaders(apkPaths.size());
    std::vector<std::unique_ptr<OpaLoader>> clays(apkPaths.size());     // pre-HSL Clay homes (cascadia.apk...)
    std::vector<std::unique_ptr<GltfLoader>> gltfs(apkPaths.size());    // the oldest custom homes (_world_model.gltf.ovrscene)
    std::vector<char> ok(apkPaths.size(), 0);
    std::vector<std::vector<MeshData>> models(apkPaths.size());           // imported Blender / Unity models
    std::vector<std::string> modelErrors(apkPaths.size());
    scene.sourceFiles.resize(apkPaths.size());
    std::vector<std::thread> threads;
    for (size_t i = 0; i < apkPaths.size(); ++i) {
        loaders[i] = std::make_unique<SceneLoader>();
        loaders[i]->verbose = std::getenv("FH_VERBOSE") != nullptr;
        threads.emplace_back([&, i] {
            if (isModelPath(apkPaths[i])) {
                if (importModel(apkPaths[i], models[i], scene.sourceFiles[i], modelErrors[i])) ok[i] = 4;
                return;
            }
            if (loaders[i]->load(apkPaths[i])) { ok[i] = 1; return; }
            clays[i] = std::make_unique<OpaLoader>();                 // not HSL: a Clay (.opa) home?
            if (clays[i]->load(apkPaths[i])) { ok[i] = 2; return; }
            clays[i].reset();
            gltfs[i] = std::make_unique<GltfLoader>();                // not .opa: a Clay .ovrscene home (glTF in scene.zip: SideQuest custom homes)?
            if (gltfs[i]->load(apkPaths[i])) ok[i] = 3;
            else gltfs[i].reset();
        });
    }
    for (auto& t : threads) t.join();
    for (size_t i = 0; i < apkPaths.size(); ++i)
        if (!ok[i]) {
            error = !modelErrors[i].empty() ? modelErrors[i] : "not a loadable home (HSL v203+, Clay .opa, glTF .ovrscene) or model (glTF, GLB, FBX, OBJ): " + apkPaths[i];
            return false;
        }
    for (size_t i = 0; i < apkPaths.size(); ++i) {
        HslSourceInfo info;
        readManifest(apkPaths[i], info.package, info.version);
        info.format = ok[i] == 1 ? "HSL" : ok[i] == 2 ? "Clay (.opa)" : ok[i] == 3 ? "Clay (.ovrscene)" : "Model";
        if (ok[i] == 4) {
            const std::string file = apkPaths[i].substr(apkPaths[i].find_last_of("/\\") + 1);
            info.package = file;
            std::string ext = file.substr(file.find_last_of('.') + 1);
            for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            info.format = "Model (" + ext + ")";
        }
        // Split homes (Haven-era) ship com.meta.shell.env.footprint.<room> + ...vista.<world>; "prod.vista" is
        // Cascadia, a whole home. Without a package, the file name decides.
        std::string n = info.package.empty() ? apkPaths[i].substr(apkPaths[i].find_last_of("/\\") + 1) : info.package;
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool named = !info.package.empty();
        info.role = ok[i] == 4 ? "Model" : ok[i] != 1 ? "Home"
                  : n.find(named ? ".footprint." : "footprint") != std::string::npos ? "Footprint"
                  : n.find(named ? ".vista." : "vista") != std::string::npos ? "Vista" : "Home";
        scene.sources.push_back(info);
    }
    const auto t1 = std::chrono::steady_clock::now();
    // Baked lightmaps: each APK's meshes against its own overrides, then the companion's (the vista
    // ships the footprint's lightmaps and vice versa).
    for (size_t i = 0; i < loaders.size(); ++i) {
        loaders[i]->applyLightmapOverrides(loaders[i]->meshes);
        for (size_t j = 0; j < loaders.size(); ++j)
            if (j != i) loaders[i]->applyLightmapOverrides(loaders[i]->meshes, loaders[j].get());
    }
    size_t lightmapped = 0;
    for (size_t i = 0; i < loaders.size(); ++i) {
        if (ok[i] == 4) {                                               // imported model: the built-in unlit programs
            addClayPrograms(scene);
            for (MeshData& md : models[i]) {
                md.shaderPath = md.alphaTest ? kClayCutoutProgram : kClayProgram;
                scene.meshes.push_back(std::move(md));
                scene.meshSource.push_back(static_cast<int>(i));
            }
            std::fprintf(stderr, "[scene] model %s: %zu parts\n", apkPaths[i].c_str(), models[i].size());
            continue;
        }
        if (clays[i] || gltfs[i]) {                                     // Clay / glTF home: built-in programs
            addClayPrograms(scene);
            // The home animates here (below: the first home, the rest only models): its skinned meshes skin on the GPU.
            if (clays[i] && clays[i]->hasAnimation() && i == 0 && std::count(ok.begin(), ok.end(), 4) + 1 == static_cast<long>(apkPaths.size()))
                clays[i]->prepareGpuSkin();
            // Base textures: one shared, still-compressed source per owner. The loader lets meshes cut from the same
            // part BORROW the owner's texture (texShareSrc) and empties their own copy — unresolved, those uploaded
            // nothing and drew white. Every user of a texture now points at the same source: decoded once in the
            // background, uploaded once.
            std::vector<MeshData>& cm = clays[i] ? clays[i]->meshes : gltfs[i]->meshes;
            std::vector<std::shared_ptr<TexSource>> srcOf(cm.size());
            for (size_t j = 0; j < cm.size(); ++j) {
                MeshData& o = cm[j];
                if (o.srcAstc.empty() || !o.srcAstcBw || !o.srcAstcBh || o.texW <= 1) continue;
                auto t = std::make_shared<TexSource>();
                t->astc = std::move(o.srcAstc);
                o.srcAstc.clear();
                t->w = o.texW; t->h = o.texH; t->bw = o.srcAstcBw; t->bh = o.srcAstcBh;
                t->mips = std::max<u32>(1, o.srcAstcMips);
                srcOf[j] = t;
            }
            for (size_t j = 0; j < cm.size(); ++j) {
                MeshData& md = cm[j];
                const size_t owner = md.texShareSrc >= 0 && static_cast<size_t>(md.texShareSrc) < cm.size() ? md.texShareSrc : j;
                if (srcOf[owner]) {
                    md.texSrc = srcOf[owner];
                    std::vector<u8>().swap(md.texRGBA);
                } else if (owner != j && !md.texSrc && cm[owner].texSrc) {
                    md.texSrc = cm[owner].texSrc;                         // glTF: the owner's compressed source
                } else if (owner != j && md.texRGBA.empty()) {
                    md.texRGBA = cm[owner].texRGBA;                       // no compressed source: copy the pixels
                }
                if (md.texSrc && !std::getenv("FH_CLAYDBG")) std::vector<u8>().swap(md.texRGBA);   // the loader's small preview: drawn from texSrc

            }
            size_t n = 0;
            for (auto& md : cm) {
                if (!md.texSrc && md.texW * md.texH <= 1)                 // flat fallback colour: no real texture
                    std::fprintf(stderr, "[clay] untextured '%s' (%u verts) colour %u,%u,%u,%u lightmap %d\n", md.name.c_str(), md.nVerts,
                                 md.texRGBA.size() >= 4 ? md.texRGBA[0] : 0, md.texRGBA.size() >= 4 ? md.texRGBA[1] : 0,
                                 md.texRGBA.size() >= 4 ? md.texRGBA[2] : 0, md.texRGBA.size() >= 4 ? md.texRGBA[3] : 0, (int)md.hasLightmap);
                if (!md.skinBindPos.empty()) md.shaderPath = md.alphaTest ? kClaySkinnedCutoutProgram : kClaySkinnedProgram;
                else md.shaderPath = md.alphaTest ? kClayCutoutProgram : kClayProgram;
                std::memcpy(md.curTint, md.tint, sizeof md.curTint);
                if (md.hasLightmap)                                       // ShellEnv: diffuse x lightmap x lightmappower
                    for (int c = 0; c < 3; ++c) md.curTint[c] *= md.lightmapPower[c];
                else if (std::getenv("FH_CLAYDBG")) {                     // diagnostics: what lights a mesh without a lightmap?
                    double avg[3] = {0, 0, 0};
                    const size_t px = md.texRGBA.size() / 4;
                    for (size_t q = 0; q < px; q += std::max<size_t>(1, px / 4096))
                        for (int c = 0; c < 3; ++c) avg[c] += md.texRGBA[q * 4 + c];
                    const double cnt = static_cast<double>(px / std::max<size_t>(1, px / 4096));
                    std::fprintf(stderr, "[clay] no lightmap: '%s' tex %ux%u avg %.0f,%.0f,%.0f blend %d iblLit %d\n", md.name.c_str(), md.texW,
                                 md.texH, avg[0] / std::max(1.0, cnt), avg[1] / std::max(1.0, cnt), avg[2] / std::max(1.0, cnt),
                                 (int)md.useBlend, (int)md.iblLit);
                    if (avg[0] + avg[1] + avg[2] == 0) {
                        size_t nz = 0;
                        for (size_t q = 0; q < md.texRGBA.size(); ++q) nz += md.texRGBA[q] != 0;
                        std::vector<u8> re;
                        const bool ok = !md.srcAstc.empty() &&
                                        astc::decodeASTC(md.srcAstc.data(), static_cast<u32>(md.srcAstc.size()), md.texW, md.texH,
                                                         md.srcAstcBw, md.srcAstcBh, re, false, 1);
                        size_t nz2 = 0;
                        for (u8 b : re) nz2 += b != 0;
                        std::fprintf(stderr, "[clay]   texRGBA %zu bytes, %zu non-zero; src %zu bytes (%ux%u blocks) re-decode %s, %zu non-zero\n",
                                     md.texRGBA.size(), nz, md.srcAstc.size(), md.srcAstcBw, md.srcAstcBh, ok ? "ok" : "failed", nz2);
                    }
                }
                scene.meshes.push_back(std::move(md));
                scene.meshSource.push_back(static_cast<int>(i));
                ++n;
            }
            std::fprintf(stderr, "[scene] %s home %s: %zu meshes\n", clays[i] ? "Clay" : "glTF .ovrscene", apkPaths[i].c_str(), n);
            // The loader keeps evaluating the animation (anim_bake.h: as a .fenv's does); it works on the scene's copy of its
            // meshes (a single Clay / glTF home: the scene's meshes ARE that loader's meshes, same order).
            // Models may follow the home: they sit after its meshes, which the animator indexes from 0.
            if (i == 0 && std::count(ok.begin(), ok.end(), 4) + 1 == static_cast<long>(apkPaths.size())) {
                if (clays[i] && clays[i]->hasAnimation()) {
                    std::shared_ptr<OpaLoader> L(std::move(clays[i]));
                    scene.animDuration = L->animDuration();
                    animbake::adopt(scene, L);
                } else if (gltfs[i] && gltfs[i]->hasAnimation()) {
                    std::shared_ptr<GltfLoader> L(std::move(gltfs[i]));
                    scene.animDuration = L->animDuration;
                    animbake::adopt(scene, L);
                }
                if (scene.animate) {
                    scene.animatedMeshes = findAnimatedMeshes(scene.animate, scene.meshes, scene.animDuration);
                    std::fprintf(stderr, "[scene] animation: %.1f s loop, %zu animated meshes\n", scene.animDuration,
                                 scene.animatedMeshes.size());
                }
            }
            continue;
        }
        const int base = static_cast<int>(scene.meshes.size());
        for (auto& g : loaders[i]->animGroups) {
            if (!(g.skel.ok() && g.clip.ok() && !g.skinRecs.empty())) continue;
            HslAnimGroup a;
            a.skel = g.skel;
            a.clip = g.clip;
            for (const auto& r : g.skinRecs) a.meshes.push_back(base + static_cast<int>(r.meshIdx));
            scene.anims.push_back(std::move(a));
        }
        {   // the world layer: the footprint's spawns first, then the vista's additions
            HomeWorld& w = loaders[i]->world;
            auto add = [](auto& to, auto& from) { to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())); };
            add(scene.world.spawns, w.spawns); add(scene.world.seats, w.seats); add(scene.world.hotspots, w.hotspots);
            add(scene.world.colliders, w.colliders); add(scene.world.wallSpots, w.wallSpots);
            if (!scene.world.empty()) scene.world.origin = "meta";
        }
        for (auto& md : loaders[i]->meshes) {
            if (md.hasLightmap) ++lightmapped;
            if (insideOutPalms(md)) for (size_t t = 0; t + 2 < md.indices.size(); t += 3) std::swap(md.indices[t + 1], md.indices[t + 2]);
            scene.meshes.push_back(std::move(md));
            scene.meshSource.push_back(static_cast<int>(i));
        }
        scanSceneZip(apkPaths[i], scene);
    }
    // The ambience is the vista's (the world outside you hear: Calming's, Central's), the footprint's only without one
    // (Haven's footprint carries a stand-in bed of its own, Central's).
    for (auto p = apkPaths.rbegin(); p != apkPaths.rend(); ++p)
        if (scene.ambientOgg.empty()) scene.ambientOgg = findAmbientOgg(*p, scene.ambientSource, scene.ambientName);
    // HSL rigs: the timeline spans the longest clip.
    if (scene.animDuration <= 0)
        for (const auto& a : scene.anims)
            if (a.clip.nFrames > 1 && a.clip.fps > 0) scene.animDuration = std::max(scene.animDuration, a.clip.nFrames / a.clip.fps);
    const auto t2 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[scene] %zu APK(s), %zu meshes (%zu lightmapped), %zu shader programs, ambient %s (%.3f, %.3f, %.3f); "
                 "parse %.1fs, lightmaps+shaders %.1fs, %zu animated rigs\n",
                 apkPaths.size(), scene.meshes.size(), lightmapped, scene.programs.size(),
                 scene.hasEnvAmbient ? "from env" : "synthesized", scene.ambientRGB[0], scene.ambientRGB[1], scene.ambientRGB[2],
                 std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count(), scene.anims.size());
    if (scene.meshes.empty()) { error = "home has no meshes"; return false; }
    simplifyWorldMeshes(scene.world);
    return true;
}

std::vector<WorldCollider> geometryColliders(const HslScene& scene) {
    std::vector<WorldCollider> out;
    for (const MeshData& md : scene.meshes) {
        if (md.isSkybox || md.useBlend || md.additive || md.overlayKind || md.indices.empty()) continue;
        WorldCollider c;
        c.name = md.name;
        c.kind = WorldCollider::Mesh;
        float m[16];
        if (md.hasWorldMatrix) std::memcpy(m, md.worldMatrix, sizeof m); else trsToMat4(md.transform, m);
        c.positions.resize(md.positions.size());
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v + 2 < md.positions.size(); v += 3)
            for (int k = 0; k < 3; ++k) {
                const float x = m[k] * md.positions[v] + m[4 + k] * md.positions[v + 1] + m[8 + k] * md.positions[v + 2] + m[12 + k];
                c.positions[v + k] = x;
                lo[k] = std::min(lo[k], x);
                hi[k] = std::max(hi[k], x);
            }
        if (hi[0] - lo[0] > 200 || hi[1] - lo[1] > 200 || hi[2] - lo[2] > 200) continue;   // the vista's backdrop
        c.indices = md.indices;
        simplifySurfaceMesh(c.positions, c.indices, kSurfaceCell);   // (world space)
        out.push_back(std::move(c));
    }
    return out;
}

int walkMeshUse(const MeshData& md) {
    if (md.isSkybox || md.additive || md.overlayKind || md.indices.empty() || md.positions.empty()) return 0;
    if (!md.useBlend) return 1;
    // See-through: glass blocks; fog, light shafts, glow cards, spray and the like are only drawn.
    std::string n = md.name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* fx : {"fog", "cloud", "mist", "haze", "smoke", "ray", "shaft", "beam", "glow", "light", "splash", "waterfall", "particle",
                           "dust", "flare", "halo", "volum", "spray", "steam", "fx", "card", "decal", "shadow", "water", "sky"})
        if (n.find(fx) != std::string::npos) return 0;
    return 2;
}

std::vector<WalkSource> drawnWalkSources(const HslScene& scene, const HomeWorld& world, const std::vector<const float*>* meshWorld,
                                         const std::vector<char>* removed, bool leaveLinked) {
    // meshes a collider stands for (Studio's surfaces: "<mesh index>:<name>") are left to it (the bake)
    std::vector<char> linked(scene.meshes.size(), 0);
    if (leaveLinked)
        for (const WorldCollider& c : world.colliders) {
            if (c.mesh.empty() || c.mesh.find("#floor") != std::string::npos) continue;   // (a drawn floor: the mesh still counts)
            const size_t colon = c.mesh.find(':');
            char* endp = nullptr;
            const long i = std::strtol(c.mesh.c_str(), &endp, 10);
            if (colon != std::string::npos && endp == c.mesh.c_str() + colon && i >= 0 && static_cast<size_t>(i) < linked.size()) linked[i] = 1;
        }
    // skinned creatures (birds, butterflies) move about: not something to bump into. (Clay's animatedMeshes also
    // lists floors whose tint or UVs animate, so it isn't used.)
    for (const HslAnimGroup& a : scene.anims) for (int m : a.meshes) if (m >= 0 && static_cast<size_t>(m) < linked.size()) linked[m] = 1;
    std::vector<WalkSource> src;
    src.reserve(scene.meshes.size());
    std::vector<std::string> names;
    for (const MeshData& md : scene.meshes) names.push_back(md.name);
    const std::string prefix = walkSharedPrefix(names);
    for (size_t m = 0; m < scene.meshes.size(); ++m) {
        const MeshData& md = scene.meshes[m];
        const int use = walkMeshUse(md);
        if (!use || linked[m] || (removed && m < removed->size() && (*removed)[m])) continue;
        if (scene.apkPaths.size() > 1 && m < scene.meshSource.size() && scene.meshSource[m] != 0) continue;   // a vista: scenery, never walked
        WalkSource s;
        s.pos = md.positions.data(); s.verts = md.positions.size() / 3; s.idx = md.indices.data(); s.indices = md.indices.size();
        if (meshWorld && m < meshWorld->size() && (*meshWorld)[m]) std::memcpy(s.world, (*meshWorld)[m], sizeof s.world);
        else if (md.hasWorldMatrix) std::memcpy(s.world, md.worldMatrix, sizeof s.world);
        else trsToMat4(md.transform, s.world);
        s.noFloor = use == 2;
        if (md.alphaTest) {   // cut-out: foliage (blocks; never floor) unless it's laid flat like ground (raked sand, a grass mat)
            double up = 0, all = 0;
            for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
                float p[3][3];
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) {
                    const uint32_t v = md.indices[t + k];
                    if (v * 3 + 2 >= md.positions.size()) { ok = false; break; }
                    for (int j = 0; j < 3; ++j) p[k][j] = s.world[j] * md.positions[v * 3] + s.world[4 + j] * md.positions[v * 3 + 1] + s.world[8 + j] * md.positions[v * 3 + 2] + s.world[12 + j];
                }
                if (!ok) continue;
                const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                all += L * 0.5f;
                if (L > 0 && std::fabs(n[1]) / L > 0.9f) up += L * 0.5f;
            }
            s.cutout = !(up >= 4.0 && up >= all * 0.8);
        }
        s.maxSize = 200;                                               // bigger: the vista's backdrop
        s.tag = static_cast<int>(m);
        s.hint = walkNameHint(md.name, prefix);
        s.name = md.name;
        src.push_back(s);
    }
    return src;
}

namespace {

// The home's invisible walls: tall solid boxes (Meta's, of any name) with nothing drawn on their sides. They keep a
// Quest user in the spot the home was made to be seen from; a box that is a drawn wall's collision is kept.
std::vector<char> invisibleWalls(const HomeWorld& world, const std::vector<WalkSource>& drawn) {
    std::vector<char> out(world.colliders.size(), 0);
    struct Cand { size_t i; float lo[3], hi[3]; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < world.colliders.size(); ++i) {
        const WorldCollider& c = world.colliders[i];
        if (c.kind != WorldCollider::Box || c.role != WorldCollider::Solid || !c.mesh.empty()) continue;
        const float up = std::sqrt(c.world[4] * c.world[4] + c.world[5] * c.world[5] + c.world[6] * c.world[6]);
        if (2 * c.half[1] * up < 1.5f) continue;                            // a wall (a low ramp or kerb stays)
        Cand k{i, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        for (int v = 0; v < 8; ++v) {
            const float l[3] = {(v & 1 ? 1 : -1) * c.half[0], (v & 2 ? 1 : -1) * c.half[1], (v & 4 ? 1 : -1) * c.half[2]};
            for (int j = 0; j < 3; ++j) {
                const float w = c.world[j] * l[0] + c.world[4 + j] * l[1] + c.world[8 + j] * l[2] + c.world[12 + j];
                k.lo[j] = std::min(k.lo[j], w - 0.3f); k.hi[j] = std::max(k.hi[j], w + 0.3f);
            }
        }
        cands.push_back(k);
    }
    if (cands.empty()) return out;
    // the drawn triangles near them, in a 1 m hash
    std::vector<std::array<float, 9>> tris;
    std::unordered_map<int64_t, std::vector<uint32_t>> cell;
    auto key = [](int x, int y, int z) { return (static_cast<int64_t>(x & 0x1fffff) << 42) | (static_cast<int64_t>(y & 0x1fffff) << 21) | (z & 0x1fffff); };
    for (const WalkSource& s : drawn) {
        std::vector<float> w(s.verts * 3);
        for (size_t v = 0; v < s.verts; ++v)
            for (int j = 0; j < 3; ++j) w[v * 3 + j] = s.world[j] * s.pos[v * 3] + s.world[4 + j] * s.pos[v * 3 + 1] + s.world[8 + j] * s.pos[v * 3 + 2] + s.world[12 + j];
        for (size_t t = 0; t + 2 < s.indices; t += 3) {
            std::array<float, 9> tr;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            bool bad = false;
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = s.idx[t + k];
                if (v >= s.verts) { bad = true; break; }
                for (int j = 0; j < 3; ++j) { tr[k * 3 + j] = w[v * 3 + j]; lo[j] = std::min(lo[j], w[v * 3 + j]); hi[j] = std::max(hi[j], w[v * 3 + j]); }
            }
            if (bad) continue;
            bool close = false;
            for (const Cand& c : cands) close |= lo[0] <= c.hi[0] && hi[0] >= c.lo[0] && lo[1] <= c.hi[1] && hi[1] >= c.lo[1] && lo[2] <= c.hi[2] && hi[2] >= c.lo[2];
            if (!close || hi[0] - lo[0] > 40 || hi[1] - lo[1] > 40 || hi[2] - lo[2] > 40) continue;
            const uint32_t id = static_cast<uint32_t>(tris.size());
            tris.push_back(tr);
            for (int x = static_cast<int>(std::floor(lo[0])); x <= static_cast<int>(std::floor(hi[0])); ++x)
                for (int y = static_cast<int>(std::floor(lo[1])); y <= static_cast<int>(std::floor(hi[1])); ++y)
                    for (int z = static_cast<int>(std::floor(lo[2])); z <= static_cast<int>(std::floor(hi[2])); ++z) cell[key(x, y, z)].push_back(id);
        }
    }
    auto dist2 = [](const float* p, const std::array<float, 9>& t) {   // point to triangle (Ericson), squared
        const float* a = &t[0]; const float* b = &t[3]; const float* c = &t[6];
        float ab[3], ac[3], ap[3];
        for (int k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; }
        auto dot = [](const float* u, const float* v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
        float q[3];
        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
        float bp[3], cp[3];
        for (int k = 0; k < 3; ++k) { bp[k] = p[k] - b[k]; cp[k] = p[k] - c[k]; }
        const float d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
        const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
        if (d1 <= 0 && d2 <= 0) std::copy_n(a, 3, q);
        else if (d3 >= 0 && d4 <= d3) std::copy_n(b, 3, q);
        else if (d6 >= 0 && d5 <= d6) std::copy_n(c, 3, q);
        else if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) q[k] = a[k] + v * ab[k]; }
        else if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) q[k] = a[k] + w * ac[k]; }
        else if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; ++k) q[k] = b[k] + w * (c[k] - b[k]); }
        else { const float den = 1 / (va + vb + vc), v = vb * den, w = vc * den; for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * v + ac[k] * w; }
        return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
    };
    for (const Cand& k : cands) {
        const WorldCollider& c = world.colliders[k.i];
        int samples = 0, drawnHits = 0;
        for (int face = 0; face < 4; ++face) {                         // its four sides (local +-X, +-Z)
            const int ax = face < 2 ? 0 : 2, other = face < 2 ? 2 : 0;
            const float sgn = face & 1 ? -1.0f : 1.0f;
            const int na = std::clamp(static_cast<int>(2 * c.half[other] / 0.3f), 1, 12), nb = std::clamp(static_cast<int>(2 * c.half[1] / 0.3f), 1, 12);
            for (int i = 0; i < na; ++i)
                for (int j = 0; j < nb; ++j) {
                    float l[3];
                    l[ax] = sgn * c.half[ax];
                    l[other] = ((i + 0.5f) / na * 2 - 1) * c.half[other];
                    l[1] = ((j + 0.5f) / nb * 2 - 1) * c.half[1];
                    float p[3];
                    for (int q = 0; q < 3; ++q) p[q] = c.world[q] * l[0] + c.world[4 + q] * l[1] + c.world[8 + q] * l[2] + c.world[12 + q];
                    ++samples;
                    auto it = cell.find(key(static_cast<int>(std::floor(p[0])), static_cast<int>(std::floor(p[1])), static_cast<int>(std::floor(p[2]))));
                    if (it == cell.end()) continue;
                    for (uint32_t t : it->second) if (dist2(p, tris[t]) < 0.15f * 0.15f) { ++drawnHits; break; }
                }
        }
        out[k.i] = drawnHits < samples * 0.2f;
    }
    return out;
}

}  // namespace

std::shared_ptr<const WalkGrid> bakeWalkGrid(const HslScene& scene, const HomeWorld& world, const WalkParams& params, WalkReport* report,
                                             const std::vector<const float*>* meshWorld, const std::vector<char>* removed) {
    std::vector<WalkSource> src = drawnWalkSources(scene, world, meshWorld, removed, true);
    std::vector<std::vector<float>> sp;
    std::vector<std::vector<uint32_t>> si;
    const size_t meshSources = src.size();
    // Meta's mesh colliders are mostly copies of drawn meshes (their own collision meshes don't read): one is enough.
    auto keyOf = [](const float* m, const float* pos, size_t verts, size_t indices) {
        float a[3], b[3];
        for (int k = 0; k < 3; ++k) {
            a[k] = m[k] * pos[0] + m[4 + k] * pos[1] + m[8 + k] * pos[2] + m[12 + k];
            const float* q = pos + (verts - 1) * 3;
            b[k] = m[k] * q[0] + m[4 + k] * q[1] + m[8 + k] * q[2] + m[12 + k];
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "%zu/%zu/%.2f,%.2f,%.2f/%.2f,%.2f,%.2f", verts, indices, a[0], a[1], a[2], b[0], b[1], b[2]);
        return std::string(buf);
    };
    std::set<std::string> seen;
    for (size_t i = 0; i < meshSources; ++i) if (src[i].verts) seen.insert(keyOf(src[i].world, src[i].pos, src[i].verts, src[i].indices));
    HomeWorld cw;
    cw.colliders.reserve(world.colliders.size());
    const std::vector<char> invisible = params.metaBounds < 0.5f ? invisibleWalls(world, drawnWalkSources(scene, world, meshWorld, removed, false))
                                                                  : std::vector<char>(world.colliders.size(), 0);
    for (size_t ci = 0; ci < world.colliders.size(); ++ci) {
        const WorldCollider& c = world.colliders[ci];
        if (invisible[ci]) {                                           // walked past: left out (keeps report tags)
            WorldCollider stub;
            stub.role = WorldCollider::Trigger;
            cw.colliders.push_back(std::move(stub));
            continue;
        }
        if (c.kind == WorldCollider::Mesh && c.role == WorldCollider::Solid && c.positions.size() >= 3 &&
            !seen.insert(keyOf(c.world, c.positions.data(), c.positions.size() / 3, c.indices.size())).second) {
            WorldCollider stub;                                        // (keeps the indices: report tags)
            stub.role = WorldCollider::Trigger;
            cw.colliders.push_back(std::move(stub));
            continue;
        }
        cw.colliders.push_back(c);
    }
    walkColliderSources(cw, src, sp, si, static_cast<int>(scene.meshes.size()));
    // (a vista's meshes aren't sources, so the colliders' extent counts like the meshes': a home whose floor and
    // furniture are all Studio surfaces is all colliders)
    return buildWalkGrid(src, walkSeeds(world), params, report);
}

// Baked walkable areas, kept between opens (a big home's bake is most of a second): keyed by this build, every file
// the home was read from (path, size, time) and its world layer, so any change bakes afresh. Under
// %LOCALAPPDATA%\FrameStudio\cache\walk or ~/.cache/frame-home/walk; the 300 most recent are kept.
static std::filesystem::path walkCacheDir() {
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA")) return std::filesystem::u8path(std::string(l) + "\\FrameStudio\\cache\\walk");
#endif
    if (const char* x = std::getenv("XDG_CACHE_HOME")) return std::filesystem::u8path(std::string(x) + "/frame-home/walk");
    const char* h = std::getenv("HOME");
    return std::filesystem::u8path(std::string(h ? h : "/tmp") + "/.cache/frame-home/walk");
}

static std::filesystem::path walkCachePath(const HslScene& scene, const HomeWorld& w) {
    uint64_t k = 1469598103934665603ull;
    auto mix = [&](const std::string& t) { for (unsigned char c : t) k = (k ^ c) * 1099511628211ull; k = (k ^ 0xff) * 1099511628211ull; };
    mix(__DATE__ " " __TIME__);   // this build's bake
    std::vector<std::string> files = scene.apkPaths;
    for (const auto& v : scene.sourceFiles) files.insert(files.end(), v.begin(), v.end());
    if (!scene.fenvPath.empty()) files.push_back(scene.fenvPath);
    for (const std::string& f : files) {
        std::error_code ec;
        const std::filesystem::path p = std::filesystem::u8path(f);
        mix(f);
        mix(std::to_string(std::filesystem::file_size(p, ec)));
        mix(std::to_string(static_cast<long long>(std::filesystem::last_write_time(p, ec).time_since_epoch().count())));
    }
    mix(worldToJson(w));
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.fwg", static_cast<unsigned long long>(k));
    return walkCacheDir() / name;
}

static void pruneWalkCache() {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> f;
    for (const auto& e : std::filesystem::directory_iterator(walkCacheDir(), ec))
        if (e.path().extension() == ".fwg") f.push_back({e.last_write_time(ec), e.path()});
    if (f.size() <= 300) return;
    std::sort(f.begin(), f.end());
    for (size_t i = 0; i + 300 < f.size(); ++i) std::filesystem::remove(f[i].second, ec);
}

static HomeWorld finishWalkWorld(const HslScene& scene, HomeWorld w);

HomeWorld walkWorld(const HslScene& scene) {
    HomeWorld w = scene.world;
    simplifyWorldMeshes(w);   // (a world.json merged in after loading: Frame Home's from Studio; simplified ones don't change)
    if (!w.walkGridCache && !w.walkGrid.empty()) {
        w.walkGridCache = decodeWalkGrid(w.walkGrid);
        if (!w.walkGridCache) std::fprintf(stderr, "[walk] the saved walkable area doesn't read: baking one\n");
    }
    static const bool noWalkCache = std::getenv("FH_NO_WALK_CACHE") != nullptr;
    const std::filesystem::path cached = w.walkGridCache || noWalkCache ? std::filesystem::path() : walkCachePath(scene, w);
    if (!w.walkGridCache && !cached.empty()) {
        std::ifstream f(cached, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!text.empty() && (w.walkGridCache = decodeWalkGrid(text))) {
            std::error_code ec;
            std::filesystem::last_write_time(cached, std::filesystem::file_time_type::clock::now(), ec);
            std::fprintf(stderr, "[walk] walkable area from the cache: %.1f m2\n", w.walkGridCache->walkArea);
            return finishWalkWorld(scene, std::move(w));
        }
    }
    if (!w.walkGridCache) {
        WalkReport r;
        w.walkGridCache = bakeWalkGrid(scene, w, WalkParams{}, &r);
        std::fprintf(stderr, "[walk] walkable area baked: %.1f m2 in %d area(s) (%d left out, %.1f m2), %.1f m2 of seams bridged; %zu triangles, %dx%d cells, %zu spans, %.0f ms\n",
                     r.walkArea, r.areas, r.unreached, r.unreachedArea, r.bridgedArea, r.triangles, r.nx, r.nz, r.spans, r.ms);
        if (w.walkGridCache && !cached.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(cached.parent_path(), ec);
            const std::filesystem::path tmp = cached.string() + ".part";
            { std::ofstream o(tmp, std::ios::binary); o << encodeWalkGrid(*w.walkGridCache); }
            std::filesystem::rename(tmp, cached, ec);
            if (ec) std::filesystem::remove(tmp, ec);
            pruneWalkCache();
        }
        // (a home no one has set up in Frame Studio walks on its meshes as they are: flat enough is floor, steeper
        // blocks — what Studio's Auto-generate makes their surfaces; nothing more is added)
    } else {
        std::fprintf(stderr, "[walk] walkable area saved with the home: %.1f m2\n", w.walkGridCache->walkArea);
    }
    return finishWalkWorld(scene, std::move(w));
}

// (the rest of walkWorld: what doesn't come from the walk bake)
static HomeWorld finishWalkWorld(const HslScene& scene, HomeWorld w) {
    // Gravity homes walk on colliders (GravityWalker): the geometry's floors and walls stand in while nothing blocks.
    if (!w.gravity.empty()) {
        bool solid = false;
        for (const WorldCollider& c : w.colliders) solid |= c.role == WorldCollider::Solid || c.role == WorldCollider::Wall || c.role == WorldCollider::Object;
        if (!solid) {
            std::vector<WorldCollider> geo = geometryColliders(scene);
            w.colliders.insert(w.colliders.end(), std::make_move_iterator(geo.begin()), std::make_move_iterator(geo.end()));
        }
    }
    return w;
}

const ShaderProgramSrc* findProgram(const HslScene& scene, const std::string& shaderPath) {
    if (shaderPath.empty()) return nullptr;
    auto it = scene.programs.find(shaderPath);
    if (it != scene.programs.end()) return &it->second;
    const std::string want = surfaceName(shaderPath);
    if (want.empty()) return nullptr;
    for (const auto& kv : scene.programs)
        if (surfaceName(kv.first) == want) return &kv.second;
    return nullptr;
}
