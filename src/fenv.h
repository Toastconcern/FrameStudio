#pragma once
// .fenv — a Frame environment in one file: the home's APKs (stored, not recompressed: they are already zips),
// Frame Studio's object edits, imported models (glTF / GLB / FBX / OBJ from Blender or Unity, with the texture
// files they reference) and a thumbnail, described by fenv.json:
//
//   { "format": "fenv", "version": 1, "name": "Haven + Calming",
//     "sources": [ { "file": "apks/haven2025.apk", "role": "Footprint", "format": "HSL",
//                    "package": "com.meta.shell.env.footprint.haven2025", "version": "206.0.0.0.70" } ],
//     "models":  [ { "file": "models/chair/chair.glb" } ],
//     "edits": "edits.json", "thumbnail": "thumb.png" }
//
// Frame Studio writes and opens it; Frame Home and the switcher on the Steam Frame load it directly. Opening
// extracts the members once into a cache folder keyed by the file's path, size and time, then loads them from there.
// A member named "<file>.lz" is an LZMA stream (fenv_lzma.h) and is unpacked to <file> — the baked scene is stored
// so ("scene.bin.lz"): the zip's own deflate is far weaker on ASTC textures.
#include "core/tinyjson.h"
#include "world_settings.h"
#ifdef _WIN32
#include <process.h>
#include <windows.h>                    // LockFileEx (the builds define NOMINMAX / WIN32_LEAN_AND_MEAN)
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include "fenv_lzma.h"
#include "miniz.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <exception>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

struct FenvSource {
    std::string file, role, format, package, version;
};

struct FenvManifest {
    std::string name;
    std::vector<FenvSource> sources;    // footprint first, then the vista (or one whole home)
    std::vector<std::string> models;    // imported model files (member paths)
    std::string edits, thumbnail;       // member paths, empty when absent
    std::string scene;                  // the baked scene (scene_bake.h): when present it IS the home, sources are info
    std::string world;                  // world.json (core/world.h): spawns, seats, walls, walkable areas
    WorldSettings settings;             // the world's own render settings (world_settings.h): none set = the headset's
};

namespace fenv_detail {

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

inline std::string manifestJson(const FenvManifest& m) {
    std::string j = "{\n  \"format\": \"fenv\",\n  \"version\": 1,\n  \"name\": " + quote(m.name) + ",\n  \"sources\": [";
    for (size_t i = 0; i < m.sources.size(); ++i) {
        const FenvSource& s = m.sources[i];
        j += std::string(i ? "," : "") + "\n    { \"file\": " + quote(s.file) + ", \"role\": " + quote(s.role) + ", \"format\": " +
             quote(s.format) + ", \"package\": " + quote(s.package) + ", \"version\": " + quote(s.version) + " }";
    }
    j += "\n  ],\n  \"models\": [";
    for (size_t i = 0; i < m.models.size(); ++i) j += std::string(i ? "," : "") + "\n    { \"file\": " + quote(m.models[i]) + " }";
    j += "\n  ]";
    if (!m.edits.empty()) j += ",\n  \"edits\": " + quote(m.edits);
    if (!m.thumbnail.empty()) j += ",\n  \"thumbnail\": " + quote(m.thumbnail);
    if (!m.scene.empty()) j += ",\n  \"scene\": " + quote(m.scene);
    if (!m.world.empty()) j += ",\n  \"world\": " + quote(m.world);
    if (!m.settings.empty() || !m.settings.tuned.empty()) j += ",\n  \"settings\": " + m.settings.toJson();
    return j + "\n}\n";
}

// Everything one .fenv may unpack to (its members, LZMA ones as unpacked): a damaged or hostile file must not
// fill the disk or memory. The largest homes are a few hundred MB.
constexpr uint64_t kExtractBudget = uint64_t(4) << 30;

// Empty when missing or larger than `limit` (the zip's declared size, which miniz holds the inflate to).
inline std::vector<uint8_t> readMember(mz_zip_archive& z, const char* name, uint64_t limit = uint64_t(64) << 20) {
    std::vector<uint8_t> out;
    const int i = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    mz_zip_archive_file_stat st;
    if (i < 0 || !mz_zip_reader_file_stat(&z, static_cast<mz_uint>(i), &st) || st.m_uncomp_size > limit || st.m_comp_size > limit) return out;
    size_t n = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, static_cast<mz_uint>(i), &n, 0);
    if (!p) return out;
    try { out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + n); } catch (const std::exception&) { out = {}; }
    mz_free(p);
    return out;
}

// A short tag for a file's full path: two "haven.fenv" in different folders get different cache folders.
inline std::string pathTag(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path full = std::filesystem::weakly_canonical(p, ec);
    if (ec || full.empty()) full = std::filesystem::absolute(p, ec).lexically_normal();
    std::string s = full.u8string();
#ifdef _WIN32
    for (char& c : s) { if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); else if (c == '/') c = '\\'; }   // one file, however spelled
#endif
    uint64_t h = 1469598103934665603ull;     // FNV-1a
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    char b[16];
    std::snprintf(b, sizeof b, "%08x", static_cast<unsigned>(h ^ (h >> 32)));
    return b;
}

// One unpacker per cache key at a time, across processes (Frame Home and the switcher may open the same
// environment together; on Windows, two Studio windows).
struct ExtractLock {
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov = {};
    explicit ExtractLock(const std::filesystem::path& p) {
        h = CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE && !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    }
    ~ExtractLock() { if (h != INVALID_HANDLE_VALUE) { UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov); CloseHandle(h); } }
#else
    int fd;
    explicit ExtractLock(const std::filesystem::path& p) : fd(open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) { if (fd >= 0) flock(fd, LOCK_EX); }
    ~ExtractLock() { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
#endif
    ExtractLock(const ExtractLock&) = delete;
    ExtractLock& operator=(const ExtractLock&) = delete;
};

inline long processId() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// A member path is safe to extract: relative, no "..", no drive letters.
inline bool safeMember(const std::string& m) {
    if (m.empty() || m[0] == '/' || m[0] == '\\' || m.find(':') != std::string::npos) return false;
    for (size_t a = 0; a <= m.size();) {
        size_t b = m.find_first_of("/\\", a);
        if (b == std::string::npos) b = m.size();
        if (m.compare(a, b - a, "..") == 0 && b - a == 2) return false;
        a = b + 1;
    }
    return true;
}

}  // namespace fenv_detail

// Where opened .fenv files are unpacked: FRAME_FENV_CACHE, else %LOCALAPPDATA%\FrameStudio\cache (Windows) or
// $XDG_CACHE_HOME / ~/.cache /frame-home/fenv.
inline std::string fenvCacheDir() {
    if (const char* e = std::getenv("FRAME_FENV_CACHE")) return e;
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA")) return std::string(l) + "\\FrameStudio\\cache";
#endif
    if (const char* x = std::getenv("XDG_CACHE_HOME")) return std::string(x) + "/frame-home/fenv";
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.cache/frame-home/fenv";
}

inline bool isFenvPath(const std::string& p) {
    return p.size() > 5 && (p.compare(p.size() - 5, 5, ".fenv") == 0 || p.compare(p.size() - 5, 5, ".FENV") == 0);
}

inline bool fenvParseManifest(const std::string& json, FenvManifest& m) {
    try {
        const tinyjson::Value doc = tinyjson::parse(json);
        if (!doc.isObject() || doc["format"].asString() != "fenv") return false;
        m = FenvManifest{};
        m.name = doc["name"].asString();
        for (size_t i = 0; i < doc["sources"].size(); ++i) {
            const auto& s = doc["sources"][i];
            FenvSource src;
            src.file = s["file"].asString(); src.role = s["role"].asString(); src.format = s["format"].asString();
            src.package = s["package"].asString(); src.version = s["version"].asString();
            if (fenv_detail::safeMember(src.file)) m.sources.push_back(src);
        }
        for (size_t i = 0; i < doc["models"].size(); ++i) {
            const std::string f = doc["models"][i]["file"].asString();
            if (fenv_detail::safeMember(f)) m.models.push_back(f);
        }
        if (doc.has("edits") && fenv_detail::safeMember(doc["edits"].asString())) m.edits = doc["edits"].asString();
        if (doc.has("thumbnail") && fenv_detail::safeMember(doc["thumbnail"].asString())) m.thumbnail = doc["thumbnail"].asString();
        if (doc.has("scene") && fenv_detail::safeMember(doc["scene"].asString())) m.scene = doc["scene"].asString();
        if (doc.has("world") && fenv_detail::safeMember(doc["world"].asString())) m.world = doc["world"].asString();
        if (doc.has("settings")) m.settings.fromValue(doc["settings"]);
        return !m.sources.empty() || !m.models.empty() || !m.scene.empty();
    } catch (...) {
        return false;
    }
}

// Reads fenv.json (and, if asked, the thumbnail bytes) without extracting anything else.
inline bool fenvReadManifest(const std::string& path, FenvManifest& m, std::vector<uint8_t>* thumb = nullptr) {
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) return false;
    const std::vector<uint8_t> j = fenv_detail::readMember(z, "fenv.json");
    bool ok = !j.empty() && fenvParseManifest(std::string(j.begin(), j.end()), m);
    if (ok && thumb && !m.thumbnail.empty()) *thumb = fenv_detail::readMember(z, m.thumbnail.c_str());
    mz_zip_reader_end(&z);
    return ok;
}

// Extracts every member into <cacheRoot>/<name>-<path tag>-<size>-<time>/ (once) and returns the sources' and
// models' paths there, plus the edits file ("" when the environment has none).
inline bool fenvExtract(const std::string& path, const std::string& cacheRoot, FenvManifest& m,
                        std::vector<std::string>& sources, std::vector<std::string>& models, std::string& editsPath,
                        std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path src = fs::u8path(path);
    const auto size = fs::file_size(src, ec);
    // (a 128-bit count on some standard libraries: no to_string for it, and 64 bits hold any file's date)
    const long long time = static_cast<long long>(fs::last_write_time(src, ec).time_since_epoch().count());
    if (!fenvReadManifest(path, m)) { error = "not a Frame environment (.fenv): " + path; return false; }
    const std::string stem = src.stem().u8string(), key = stem + "-" + fenv_detail::pathTag(src);
    const fs::path root = fs::u8path(cacheRoot);
    const fs::path dir = root / (key + "-" + std::to_string(size) + "-" + std::to_string(time));
    const fs::path done = dir / ".complete";
    fs::create_directories(root, ec);
    const fenv_detail::ExtractLock lock(root / (key + ".lock"));
    if (!fs::exists(done, ec)) {
        // Earlier versions of this environment (same file, other size / time) are stale now, as are folders of the
        // naming before the path tag ("<name>-<size>-<time>") and unpacks that never finished (".tmp-<pid>": the
        // lock is ours, so nobody is still writing one).
        const auto digits = [](std::string t) {               // the time part can be negative
            if (!t.empty() && t[0] == '-') t.erase(0, 1);
            return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos;
        };
        const auto sizeTime = [&](const std::string& rest) {  // exactly "<size>-<time>"
            const size_t dash = rest.find('-');
            return dash != std::string::npos && digits(rest.substr(0, dash)) && digits(rest.substr(dash + 1));
        };
        for (const auto& e : fs::directory_iterator(root, ec)) {
            if (!e.is_directory(ec) || e.path() == dir) continue;
            const std::string f = e.path().filename().u8string();
            // "haven-1a2b3c4d-123-456" is this haven's, "haven-calming-..." and another folder's haven are not
            bool ours = f.rfind(key + "-", 0) == 0;
            if (ours) {
                std::string rest = f.substr(key.size() + 1);
                const size_t t = rest.find(".tmp-");
                if (t != std::string::npos && digits(rest.substr(t + 5))) rest.erase(t);
                ours = sizeTime(rest);
            }
            const bool legacy = f.rfind(stem + "-", 0) == 0 && sizeTime(f.substr(stem.size() + 1));
            if (ours || legacy) fs::remove_all(e.path(), ec);
        }
        // Unpacked beside the cache folder and renamed into place once whole: a crash or a full disk midway
        // leaves no folder that looks finished.
        const fs::path tmp = fs::u8path(dir.u8string() + ".tmp-" + std::to_string(fenv_detail::processId()));
        fs::remove_all(tmp, ec);
        fs::remove_all(dir, ec);
        fs::create_directories(tmp, ec);
        mz_zip_archive z = {};
        if (!mz_zip_reader_init_file(&z, path.c_str(), 0)) { error = "can't open " + path; fs::remove_all(tmp, ec); return false; }
        const mz_uint count = mz_zip_reader_get_num_files(&z);
        uint64_t claimed = 0;                     // what the zip says it holds, before anything is written
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st;
            if (mz_zip_reader_file_stat(&z, i, &st) && !st.m_is_directory) claimed += std::max(st.m_comp_size, st.m_uncomp_size);
        }
        bool ok = claimed <= fenv_detail::kExtractBudget;
        if (!ok) error = path + " claims " + std::to_string(claimed >> 20) + " MB unpacked: damaged";
        uint64_t written = 0;
        for (mz_uint i = 0; i < count && ok; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            const std::string name = st.m_filename;
            if (!fenv_detail::safeMember(name)) continue;
            const fs::path out = tmp / fs::u8path(name);
            fs::create_directories(out.parent_path(), ec);
            if (name.size() > 3 && name.compare(name.size() - 3, 3, ".lz") == 0) {
                std::string why;
                try {
                    std::vector<uint8_t> packed = fenv_detail::readMember(z, name.c_str(), fenv_detail::kExtractBudget - written), raw;
                    ok = !packed.empty() && fenvLzmaDecompress(packed.data(), packed.size(), raw, why);
                    packed = std::vector<uint8_t>();
                    if (ok && raw.size() > fenv_detail::kExtractBudget - written) { ok = false; why = "larger than any environment"; }
                    if (ok) {
                        written += raw.size();
                        const fs::path plain = tmp / fs::u8path(name.substr(0, name.size() - 3));
                        std::ofstream f(plain, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
                        ok = static_cast<bool>(f);
                    }
                } catch (const std::exception&) {   // bad_alloc: the scene doesn't fit in this memory
                    ok = false;
                    why = "not enough memory";
                }
                if (!ok) error = "couldn't unpack " + name + " from " + path + (why.empty() ? "" : ": " + why);
                continue;
            }
            written += std::max(st.m_comp_size, st.m_uncomp_size);
            ok = written <= fenv_detail::kExtractBudget && mz_zip_reader_extract_to_file(&z, i, out.u8string().c_str(), 0);
            if (!ok) error = "couldn't extract " + name + " from " + path;
        }
        mz_zip_reader_end(&z);
        if (ok) {
            std::ofstream f(tmp / ".complete");
            f << "ok\n";
            f.close();                            // (closed before the rename: Windows won't move a folder with open files)
            ok = static_cast<bool>(f);
            if (!ok) error = "couldn't write the cache for " + path;
        }
        if (ok) {
            fs::rename(tmp, dir, ec);
            ok = !ec;
            if (!ok) error = "couldn't finish the cache for " + path + ": " + ec.message();
        }
        if (!ok) { fs::remove_all(tmp, ec); return false; }
    }
    sources.clear();
    models.clear();
    for (const FenvSource& s : m.sources)
        if (fs::exists(dir / fs::u8path(s.file), ec)) sources.push_back((dir / fs::u8path(s.file)).u8string());
    for (const std::string& f : m.models) models.push_back((dir / fs::u8path(f)).u8string());
    editsPath = m.edits.empty() ? std::string() : (dir / fs::u8path(m.edits)).u8string();
    if (!m.scene.empty()) m.scene = (dir / fs::u8path(m.scene)).u8string();   // now the extracted file
    if (!m.world.empty()) m.world = (dir / fs::u8path(m.world)).u8string();
    return true;
}

// One member to write: its path inside the .fenv and either a file on disk or bytes.
struct FenvMember {
    std::string name, file;
    std::vector<uint8_t> bytes;
    bool store = false;                 // already compressed (APKs, PNG/JPG, GLB): stored as-is
    bool fast = false;                  // large: deflated at the fastest level (inflates as fast; saving is 2x quicker)
    bool lzma = false;                  // packed with LZMA as "<name>.lz" (the baked scene: much smaller than deflate)
    FenvPacking packing = FenvPacking::Smallest;
};

// progress(done, total): LZMA members, as they are packed (from the packing threads).
inline bool fenvWrite(const std::string& outPath, const FenvManifest& m, const std::vector<FenvMember>& members, std::string& error,
                      const std::function<void(size_t, size_t)>& progress = {}) {
    namespace fs = std::filesystem;
    const std::string tmp = outPath + ".part";
    mz_zip_archive z = {};
    if (!mz_zip_writer_init_file_v2(&z, tmp.c_str(), 0, 0)) { error = "can't write " + outPath; return false; }
    const std::string json = fenv_detail::manifestJson(m);
    bool ok = mz_zip_writer_add_mem(&z, "fenv.json", json.data(), json.size(), MZ_DEFAULT_COMPRESSION);
    for (const FenvMember& f : members) {
        if (!ok) break;
        const mz_uint level = f.store ? MZ_NO_COMPRESSION : f.fast ? MZ_BEST_SPEED : MZ_DEFAULT_COMPRESSION;
        if (f.lzma) {
            std::vector<uint8_t> packed;
            std::string why;
            ok = fenvLzmaCompress(f.bytes.data(), f.bytes.size(), packed, why, f.packing, progress) &&
                 mz_zip_writer_add_mem(&z, (f.name + ".lz").c_str(), packed.data(), packed.size(), MZ_NO_COMPRESSION);
            if (!ok) error = "couldn't pack " + f.name + (why.empty() ? "" : ": " + why);
            continue;
        }
        ok = f.file.empty() ? mz_zip_writer_add_mem(&z, f.name.c_str(), f.bytes.data(), f.bytes.size(), level)
                            : mz_zip_writer_add_file(&z, f.name.c_str(), f.file.c_str(), nullptr, 0, level);
        if (!ok) error = "couldn't add " + (f.file.empty() ? f.name : f.file);
    }
    ok = ok && mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    std::error_code ec;
    if (!ok) { fs::remove(fs::u8path(tmp), ec); if (error.empty()) error = "couldn't finish " + outPath; return false; }
    fs::rename(fs::u8path(tmp), fs::u8path(outPath), ec);
    if (ec) { fs::remove(fs::u8path(outPath), ec); fs::rename(fs::u8path(tmp), fs::u8path(outPath), ec); }
    if (ec) { error = "couldn't replace " + outPath + ": " + ec.message(); return false; }
    return true;
}

// A new manifest for an existing .fenv, every other member copied over as it is (nothing re-packed): how a world's
// settings change without saving the whole world again.
inline bool fenvRewriteManifest(const std::string& path, const FenvManifest& m, std::string& error) {
    namespace fs = std::filesystem;
    mz_zip_archive in = {}, out = {};
    if (!mz_zip_reader_init_file(&in, path.c_str(), 0)) { error = "can't read " + path; return false; }
    const std::string tmp = path + ".part";
    if (!mz_zip_writer_init_file_v2(&out, tmp.c_str(), 0, 0)) { mz_zip_reader_end(&in); error = "can't write " + tmp; return false; }
    const std::string json = fenv_detail::manifestJson(m);
    bool ok = mz_zip_writer_add_mem(&out, "fenv.json", json.data(), json.size(), MZ_DEFAULT_COMPRESSION);
    const mz_uint n = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; ok && i < n; ++i) {
        char name[512];
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        if (std::strcmp(name, "fenv.json") == 0) continue;
        ok = mz_zip_writer_add_from_zip_reader(&out, &in, i);
        if (!ok) error = std::string("couldn't copy ") + name;
    }
    ok = ok && mz_zip_writer_finalize_archive(&out);
    mz_zip_writer_end(&out);
    mz_zip_reader_end(&in);
    std::error_code ec;
    if (!ok) { fs::remove(fs::u8path(tmp), ec); if (error.empty()) error = "couldn't finish " + tmp; return false; }
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
    if (ec) { fs::remove(fs::u8path(tmp), ec); error = "couldn't replace " + path + ": " + ec.message(); return false; }
    return true;
}

// A home's own settings: a .fenv's manifest, else "<apk>.settings.json" beside it (none: all unset).
inline WorldSettings worldSettingsFor(const std::string& homePath) {
    WorldSettings w;
    if (isFenvPath(homePath)) {
        FenvManifest m;
        if (fenvReadManifest(homePath, m)) w = m.settings;
        return w;
    }
    std::FILE* f = std::fopen(worldSettingsPathFor(homePath).c_str(), "rb");
    if (!f) return w;
    std::string j;
    char b[4096];
    for (size_t k; (k = std::fread(b, 1, sizeof b, f)) > 0;) j.append(b, k);
    std::fclose(f);
    if (!w.fromJson(j)) w = WorldSettings();
    return w;
}

// Stores them: into the .fenv's manifest, or the APK's settings file (removed when none are set).
inline bool setWorldSettings(const std::string& homePath, const WorldSettings& w, std::string& error) {
    if (isFenvPath(homePath)) {
        FenvManifest m;
        if (!fenvReadManifest(homePath, m)) { error = "not a Frame environment (.fenv): " + homePath; return false; }
        m.settings = w;
        return fenvRewriteManifest(homePath, m, error);
    }
    const std::string p = worldSettingsPathFor(homePath);
    std::error_code ec;
    if (w.empty() && w.tuned.empty()) { std::filesystem::remove(std::filesystem::u8path(p), ec); return true; }
    const std::string j = w.toJson() + "\n", tmp = p + ".part";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f || std::fwrite(j.data(), 1, j.size(), f) != j.size()) { if (f) std::fclose(f); error = "can't write " + p; return false; }
    std::fclose(f);
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(p), ec);
    if (ec) { error = "can't write " + p + ": " + ec.message(); return false; }
    return true;
}

