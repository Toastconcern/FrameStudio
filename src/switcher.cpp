// frame-switcher — Frame Home's environment switcher: a SteamVR dashboard panel listing every environment (on the
// Steam Frame ~/frame-home/envs; on a PC, Frame Studio's My Environments: home_platform.h) — Frame environments .fenv,
// home APKs, and footprint + vista pairs from pairs.txt — with thumbnails. Point and click one: `current` changes and
// Frame Home switches to it live. A bundle of environments (.fbundle) put there is unpacked into its .fenv files,
// which the panel then lists one by one.
//
//   frame-switcher                 the dashboard panel (SteamVR starts it: see --install)
//   frame-switcher --install       register with SteamVR and start it with SteamVR from now on
//   frame-switcher --uninstall     the other way round (a PC's uninstaller)
//   frame-switcher --list          the environments, the current one marked with *
//   frame-switcher --set NAME      switch (NAME: the file name, or a number from --list)
//   frame-switcher --thumbs        render missing thumbnails (frame-home --snapshot, from the spawn)
//   frame-switcher --delete NAME   delete an environment (like the panel's delete button; NAME as for --set)
//   frame-switcher --unpack-bundles   unpack the bundles (.fbundle) in envs now (the panel does it by itself)
//   frame-switcher --test-delete DIR   self-check of deleting, in a fake root made in DIR (must not exist yet)
#include "fenv.h"
#include "fenv_bundle.h"
#include "overlay_texture.h"
#include "overlays.h"
#include "perf_settings.h"
#include "home_platform.h"

#include <openvr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <csignal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

#define LOG(...) do { std::fprintf(stderr, "[frame-switcher] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppKey = "framehome.switcher";
constexpr int W = 1280, H = 800;

fs::path rootDir() { return homeos::root(); }   // (home_platform.h: ~/frame-home, or a PC's)

std::string readText(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Image { std::vector<uint8_t> rgba; int w = 0, h = 0; };

Image loadImage(const fs::path& p) {
    Image img;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() > 16 && b[0] == 'P' && b[1] == '6') {       // frame-home --snapshot writes binary PPM
        std::istringstream hdr(std::string(b.begin(), b.begin() + std::min<size_t>(b.size(), 64)));
        std::string magic;
        int w = 0, h = 0, maxv = 0;
        hdr >> magic >> w >> h >> maxv;
        const size_t off = static_cast<size_t>(hdr.tellg()) + 1;
        if (w > 0 && h > 0 && off + static_cast<size_t>(w) * h * 3 <= b.size()) {
            img.w = w; img.h = h;
            img.rgba.resize(static_cast<size_t>(w) * h * 4);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                img.rgba[i * 4] = b[off + i * 3]; img.rgba[i * 4 + 1] = b[off + i * 3 + 1]; img.rgba[i * 4 + 2] = b[off + i * 3 + 2]; img.rgba[i * 4 + 3] = 255;
            }
        }
        return img;
    }
    if (b.empty()) return img;
    int w = 0, h = 0, c = 0;
    if (unsigned char* px = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &c, 4)) {
        img.w = w; img.h = h;
        img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
    }
    return img;
}

Image decodeBytes(const std::vector<uint8_t>& b) {
    Image img;
    int w = 0, h = 0, c = 0;
    if (!b.empty())
        if (unsigned char* px = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &c, 4)) {
            img.w = w; img.h = h;
            img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
        }
    return img;
}

// ── The environments ──

struct Env {
    std::string name, kind;             // shown
    std::vector<std::string> files;     // what goes into `current` (names in envs/)
    std::string key;                    // `current` contents, one file per line
    Image thumb;
    Image fitted;                       // thumb scaled to the card (made when first drawn)
};

std::string pretty(std::string stem) {
    // Meta's shell homes: "shell-env-vista-calming-quest3s-buck_206.0.0.0.70_974701191" -> "calming"
    if (stem.rfind("shell-env-", 0) == 0) {
        stem = stem.substr(10);
        for (const char* role : {"footprint-", "vista-"})
            if (stem.rfind(role, 0) == 0) stem = stem.substr(std::strlen(role));
        if (const size_t q = stem.find("-quest"); q != std::string::npos && q > 0) stem = stem.substr(0, q);
    }
    // package names: "com.oculus.environment.prod.courtyard" -> "courtyard"
    if (stem.rfind("com.", 0) == 0 && stem.find_last_of('.') + 1 < stem.size()) stem = stem.substr(stem.find_last_of('.') + 1);
    // trailing version numbers: "_206.0.0.0.70_974701191"
    for (size_t u; (u = stem.find_last_of('_')) != std::string::npos && u > 0 && u + 1 < stem.size() &&
                   stem.find_first_not_of("0123456789.", u + 1) == std::string::npos;)
        stem.erase(u);
    for (char& c : stem) if (c == '_' || c == '-') c = ' ';
    if (!stem.empty()) stem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stem[0])));
    return stem;
}

fs::path thumbPath(const std::string& key) {
    std::string n = key;
    for (char& c : n) if (c == '\n' || c == '/') c = '+';
    return homeos::thumbs() / (n + ".ppm");
}

std::vector<Env> scanEnvs() {
    const fs::path envs = homeos::envs();
    std::vector<Env> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(envs, ec)) {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file(ec) && (ext == ".fenv" || ext == ".apk")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    // Footprint + vista pairs the user (or Frame Studio) listed: "haven2025.apk calming.apk" per line.
    std::istringstream pairs(readText(rootDir() / "pairs.txt"));
    std::string line;
    while (std::getline(pairs, line)) {
        std::istringstream l(line);
        std::string a, b;
        if (!(l >> a >> b) || !fs::exists(envs / a, ec) || !fs::exists(envs / b, ec)) continue;
        Env e;
        e.name = pretty(fs::path(a).stem().string()) + " + " + pretty(fs::path(b).stem().string());
        e.kind = "Footprint + Vista";
        e.files = {a, b};
        out.push_back(e);
    }
    for (const fs::path& p : files) {
        Env e;
        e.files = {p.filename().string()};
        if (p.extension() == ".fenv") {
            FenvManifest m;
            std::vector<uint8_t> thumb;
            if (!fenvReadManifest(p.string(), m, &thumb)) continue;
            e.name = m.name.empty() ? pretty(p.stem().string()) : m.name;
            e.kind = m.sources.size() > 1 ? "Frame environment, footprint + vista" : "Frame environment";
            e.thumb = decodeBytes(thumb);
        } else {
            e.name = pretty(p.stem().string());
            const std::string fn = p.filename().string();
            e.kind = fn.find("-vista-") != std::string::npos ? "Meta home, vista only" : fn.find("-footprint-") != std::string::npos ? "Meta home, footprint only" : "Meta home";
        }
        out.push_back(e);
    }
    for (Env& e : out) {
        for (const auto& f : e.files) e.key += f + "\n";
        if (e.thumb.rgba.empty()) e.thumb = loadImage(thumbPath(e.key));
    }
    return out;
}

std::string currentKey() {
    std::istringstream lines(readText(rootDir() / "current"));
    std::string key, n;
    while (std::getline(lines, n)) if (!n.empty()) key += n + "\n";
    return key;
}

bool setCurrent(const Env& e) {
    const fs::path tmp = rootDir() / "current.part";
    {
        std::ofstream f(tmp);
        for (const auto& n : e.files) f << n << "\n";
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, rootDir() / "current", ec);     // atomic: Frame Home never reads half a file
    if (ec) return false;
    LOG("switched to %s", e.name.c_str());
    return true;
}

// ── Deleting an environment ──
// Removes the card's files from envs/ (both of a footprint + vista pair), their Frame Studio companions
// (<file>.world.json, <file>.edits.json), the thumbnails naming them, the pairs.txt lines naming them and, for a .fenv,
// its unpack folders in the cache. Never touches anything but plain environment file names directly in envs/. When
// `current` names a deleted file it is moved to another environment first (or emptied when none is left).

// A name in envs/ that may be deleted: a plain .fenv / .apk file name, no path parts.
bool deletableName(const std::string& n) {
    if (n.empty() || n.size() > 255 || n[0] == '.' || n.find_first_of("/\\\n\r") != std::string::npos || n.find("..") != std::string::npos) return false;
    const std::string ext = fs::path(n).extension().string();
    return ext == ".fenv" || ext == ".apk";
}

// Writes `current` (atomically, like setCurrent); an empty list empties it.
bool writeCurrent(const std::vector<std::string>& files) {
    const fs::path tmp = rootDir() / "current.part";
    {
        std::ofstream f(tmp);
        for (const auto& n : files) f << n << "\n";
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, rootDir() / "current", ec);
    return !ec;
}

// The .fenv cache entries of one envs/ file: "<stem>-<path tag>-<size>-<time>" folders and "<stem>-<path tag>.lock".
// The tag hashes the full path, so the name must be computed while the file still exists.
struct CacheDoomed { fs::path root; std::string key; };
CacheDoomed cacheEntriesOf(const fs::path& file) {
    return {fs::u8path(fenvCacheDir()), file.stem().u8string() + "-" + fenv_detail::pathTag(file)};
}
int removeCacheEntries(const CacheDoomed& c) {
    if (c.key.size() < 10) return 0;
    const auto digits = [](std::string t) {
        if (!t.empty() && t[0] == '-') t.erase(0, 1);
        return !t.empty() && t.find_first_not_of("0123456789") == std::string::npos;
    };
    std::error_code ec;
    int n = 0;
    {
        // An unpack of this file running right now (the background preparer, Frame Home) finishes first.
        const fenv_detail::ExtractLock lock(c.root / (c.key + ".lock"));
        std::vector<fs::path> doomed;
        for (const auto& e : fs::directory_iterator(c.root, ec)) {
            const std::string fn = e.path().filename().u8string();
            if (fn.size() <= c.key.size() + 1 || fn.compare(0, c.key.size() + 1, c.key + "-") != 0) continue;
            const std::string rest = fn.substr(c.key.size() + 1);       // exactly "<size>-<time>"
            const size_t dash = rest.find('-');
            if (dash == std::string::npos || !digits(rest.substr(0, dash)) || !digits(rest.substr(dash + 1))) continue;
            if (!e.is_directory(ec) || e.is_symlink(ec)) continue;
            doomed.push_back(e.path());
        }
        for (const fs::path& d : doomed) if (fs::remove_all(d, ec) > 0 && !ec) ++n;
    }
    fs::remove(c.root / (c.key + ".lock"), ec);
    return n;
}

struct DeleteResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> removed;           // what went, relative to ~/frame-home (cache folders: "cache/<name>")
    bool currentChanged = false;
    std::string newCurrent;                     // the environment shown now ("" when none is left)
};

// `all` is the list the card came from (where a new current one is picked from). cacheInBackground: the cache
// folders go in a thread of their own (they can be big, and an unpack in progress is waited for).
DeleteResult deleteEnv(const Env& e, const std::vector<Env>& all, bool cacheInBackground) {
    DeleteResult r;
    const fs::path envs = homeos::envs();
    std::error_code ec;
    if (e.files.empty()) { r.error = "nothing to delete"; return r; }
    for (const auto& f : e.files) {
        if (!deletableName(f) || (envs / f).parent_path() != envs) { r.error = "refusing to delete \"" + f + "\""; return r; }
        const fs::file_status st = fs::symlink_status(envs / f, ec);
        if (!fs::is_regular_file(st) && !fs::is_symlink(st)) { r.error = f + " is not there"; return r; }
    }
    auto doomed = [&](const std::string& f) { return std::find(e.files.begin(), e.files.end(), f) != e.files.end(); };

    // 1. Move `current` off it first, so Frame Home never points at a missing file.
    std::vector<std::string> cur;
    {
        std::istringstream lines(readText(rootDir() / "current"));
        for (std::string n; std::getline(lines, n);) if (!n.empty()) cur.push_back(n);
    }
    if (std::any_of(cur.begin(), cur.end(), doomed)) {
        const Env* next = nullptr;
        for (const Env& o : all)
            if (std::none_of(o.files.begin(), o.files.end(), doomed) &&
                std::all_of(o.files.begin(), o.files.end(), [&](const std::string& f) { return fs::exists(envs / f, ec); })) { next = &o; break; }
        if (!(next ? setCurrent(*next) : writeCurrent({}))) { r.error = "can't change ~/frame-home/current"; return r; }
        r.currentChanged = true;
        r.newCurrent = next ? next->name : "";
    }

    // 2. The files (cache names first: they hash the path of the file as it exists).
    std::vector<CacheDoomed> caches;
    for (const auto& f : e.files) if (fs::path(f).extension() == ".fenv") caches.push_back(cacheEntriesOf(envs / f));
    for (const auto& f : e.files) {
        if (!fs::remove(envs / f, ec) || ec) { r.error = "couldn't delete " + f + (ec ? ": " + ec.message() : ""); return r; }
        r.removed.push_back("envs/" + f);
        for (const char* comp : {".world.json", ".edits.json"})
            if (fs::remove(envs / (f + comp), ec)) r.removed.push_back("envs/" + f + comp);
    }

    // 3. pairs.txt: lines naming a deleted file go (rewritten whole, renamed into place).
    std::vector<std::string> pairKeys;          // the thumbnails of those pairs go too
    {
        const std::string text = readText(rootDir() / "pairs.txt");
        std::istringstream lines(text);
        std::string kept, line;
        bool changed = false;
        while (std::getline(lines, line)) {
            std::istringstream l(line);
            std::string a, b;
            if ((l >> a >> b) && (doomed(a) || doomed(b))) { pairKeys.push_back(a + "\n" + b + "\n"); changed = true; continue; }
            kept += line + "\n";
        }
        if (changed) {
            const fs::path tmp = rootDir() / "pairs.txt.part";
            bool ok;
            { std::ofstream o(tmp); o << kept; o.close(); ok = !o.fail(); }
            if (ok) fs::rename(tmp, rootDir() / "pairs.txt", ec);
            if (!ok || ec) LOG("couldn't update pairs.txt");
            else r.removed.push_back("pairs.txt (" + std::to_string(pairKeys.size()) + (pairKeys.size() == 1 ? " line)" : " lines)"));
        }
    }

    // 4. Thumbnails: the card's, each file's own, each pair's that named one.
    std::vector<std::string> keys = {e.key};
    for (const auto& f : e.files) keys.push_back(f + "\n");
    keys.insert(keys.end(), pairKeys.begin(), pairKeys.end());
    for (const auto& k : keys) {
        const fs::path t = thumbPath(k);
        if (t.parent_path() == homeos::thumbs() && fs::remove(t, ec)) r.removed.push_back("thumbnail " + t.filename().string());
    }

    // 5. The unpacked copies (they'd be cleaned up eventually anyway).
    if (!caches.empty()) {
        if (cacheInBackground) std::thread([caches] { for (const auto& c : caches) removeCacheEntries(c); }).detach();
        else for (const auto& c : caches) for (int i = removeCacheEntries(c); i > 0; --i) r.removed.push_back("cache/" + c.key + "-*");
    }
    LOG("deleted %s (%zu files)", e.name.c_str(), r.removed.size());
    r.ok = true;
    return r;
}

// ── Drawing (software, into the overlay's RGBA buffer) ──

constexpr float kPi = 3.14159265f;

uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int s) { const float x = (a >> s & 255), y = (b >> s & 255); return static_cast<uint32_t>(x + (y - x) * t + 0.5f) << s; };
    return ch(16) | ch(8) | ch(0);
}

struct Canvas {
    std::vector<uint8_t> px = std::vector<uint8_t>(static_cast<size_t>(W) * H * 4);
    int cx0 = 0, cy0 = 0, cx1 = W, cy1 = H;     // clip rectangle (exclusive max)
    void clip(float x0, float y0, float x1, float y1) {
        cx0 = std::clamp(static_cast<int>(x0), 0, W); cy0 = std::clamp(static_cast<int>(y0), 0, H);
        cx1 = std::clamp(static_cast<int>(std::ceil(x1)), 0, W); cy1 = std::clamp(static_cast<int>(std::ceil(y1)), 0, H);
    }
    void noClip() { cx0 = 0; cy0 = 0; cx1 = W; cy1 = H; }
    void blend(int x, int y, uint32_t rgb, float a) {
        if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1 || a <= 0.002f) return;
        uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
        if (a >= 0.998f) { p[0] = rgb >> 16; p[1] = rgb >> 8; p[2] = rgb; p[3] = 255; return; }
        const float r = (rgb >> 16 & 255), g = (rgb >> 8 & 255), b = (rgb & 255);
        p[0] = static_cast<uint8_t>(p[0] + (r - p[0]) * a + 0.5f);
        p[1] = static_cast<uint8_t>(p[1] + (g - p[1]) * a + 0.5f);
        p[2] = static_cast<uint8_t>(p[2] + (b - p[2]) * a + 0.5f);
        p[3] = 255;
    }
    // A row of pixels [xa, xb) at y, already inside the clip.
    void span(int xa, int xb, int y, uint32_t rgb, float a) {
        if (xb <= xa) return;
        uint8_t* p = &px[(static_cast<size_t>(y) * W + xa) * 4];
        if (a >= 0.998f) {
            const uint8_t r = rgb >> 16, g = rgb >> 8, b = rgb;
            for (int x = xa; x < xb; ++x, p += 4) { p[0] = r; p[1] = g; p[2] = b; p[3] = 255; }
            return;
        }
        const uint32_t ai = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 256.0f), ni = 256 - ai;
        const uint32_t r = (rgb >> 16 & 255) * ai, g = (rgb >> 8 & 255) * ai, b = (rgb & 255) * ai;
        for (int x = xa; x < xb; ++x, p += 4) {
            p[0] = static_cast<uint8_t>((p[0] * ni + r) >> 8); p[1] = static_cast<uint8_t>((p[1] * ni + g) >> 8); p[2] = static_cast<uint8_t>((p[2] * ni + b) >> 8); p[3] = 255;
        }
    }
    void clear(uint32_t rgb) {
        for (size_t i = 0; i < px.size(); i += 4) { px[i] = rgb >> 16; px[i + 1] = rgb >> 8; px[i + 2] = rgb; px[i + 3] = 255; }
    }
    // Coverage of a rounded rectangle at pixel centre (px, py): rt the top corners' radius, rb the bottom ones'.
    static float cover(float px, float py, float x0, float y0, float x1, float y1, float rt, float rb) {
        const float r = py < (y0 + y1) * 0.5f ? rt : rb;
        const float dx = std::max({x0 + r - px, px - (x1 - r), 0.0f}), dy = std::max({y0 + r - py, py - (y1 - r), 0.0f});
        float d;
        if (dx > 0 && dy > 0) d = std::sqrt(dx * dx + dy * dy) - r;
        else {   // straight edges: distance to the nearest side
            d = std::max({x0 - px, px - x1, y0 - py, py - y1});
        }
        return std::clamp(0.5f - d, 0.0f, 1.0f);
    }
    void rect(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float a = 1.0f, float rb = -1) {
        if (rb < 0) rb = r;
        const int ya = std::max(static_cast<int>(std::floor(y0)), cy0), yb = std::min(static_cast<int>(std::ceil(y1)), cy1);
        const int xa = std::max(static_cast<int>(std::floor(x0)), cx0), xb = std::min(static_cast<int>(std::ceil(x1)), cx1);
        const float rmax = std::max(r, rb);
        for (int y = ya; y < yb; ++y) {
            // pixels surely inside skip the distance maths
            const bool innerRow = y >= y0 + 1 && y + 1 <= y1 - 1;
            const bool midRow = y >= y0 + rmax + 1 && y + 1 <= y1 - rmax - 1;
            const float in0 = midRow ? x0 + 1 : x0 + rmax + 1, in1 = midRow ? x1 - 1 : x1 - rmax - 1;
            int sa = xb, sb = xb;   // the solid span [sa, sb)
            if (innerRow) { sa = std::clamp(static_cast<int>(std::ceil(in0)), xa, xb); sb = std::clamp(static_cast<int>(std::floor(in1)), sa, xb); }
            for (int x = xa; x < sa; ++x) blend(x, y, rgb, a * cover(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r, rb));
            span(sa, sb, y, rgb, a);
            for (int x = sb; x < xb; ++x) blend(x, y, rgb, a * cover(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r, rb));
        }
    }
    // An outline `t` thick inside the rounded rectangle's edge.
    void ring(float x0, float y0, float x1, float y1, float r, float t, uint32_t rgb, float a = 1.0f) {
        const int ya = std::max(static_cast<int>(std::floor(y0)), cy0), yb = std::min(static_cast<int>(std::ceil(y1)), cy1);
        const int xa = std::max(static_cast<int>(std::floor(x0)), cx0), xb = std::min(static_cast<int>(std::ceil(x1)), cx1);
        const float ri = std::max(0.0f, r - t);
        for (int y = ya; y < yb; ++y) {
            const bool straightRow = y >= y0 + r + 1 && y + 1 <= y1 - r - 1;     // left / right edges only
            const bool hollowRow = y >= y0 + t + 1 && y + 1 <= y1 - t - 1;
            for (int x = xa; x < xb; ++x) {
                if ((straightRow && x >= x0 + t + 1 && x + 1 <= x1 - t - 1) || (hollowRow && x >= x0 + r + 1 && x + 1 <= x1 - r - 1)) {
                    x = std::max(x, static_cast<int>(straightRow ? x1 - t - 1 : x1 - r - 1) - 1);   // skip the hollow middle
                    continue;
                }
                const float c = cover(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r, r) - cover(x + 0.5f, y + 0.5f, x0 + t, y0 + t, x1 - t, y1 - t, ri, ri);
                blend(x, y, rgb, a * c);
            }
        }
    }
    // Vertical fade of one colour, alpha a0 at y0 to a1 at y1.
    void fade(float x0, float y0, float x1, float y1, uint32_t rgb, float a0, float a1) {
        for (int y = std::max(static_cast<int>(y0), cy0); y < std::min(static_cast<int>(std::ceil(y1)), cy1); ++y) {
            const float t = (y + 0.5f - y0) / std::max(1.0f, y1 - y0), a = a0 + (a1 - a0) * t;
            if (a > 0.002f) span(std::max(static_cast<int>(x0), cx0), std::min(static_cast<int>(std::ceil(x1)), cx1), y, rgb, a);
        }
    }
    // An image already sized to the box (see fit), with rounded corners; dim < 1 darkens it.
    void blit(const Image& img, float x0, float y0, float rt, float rb, float dim = 1.0f) {
        if (img.rgba.empty()) return;
        const float x1 = x0 + img.w, y1 = y0 + img.h, rmax = std::max(rt, rb);
        const int ix0 = static_cast<int>(x0), iy0 = static_cast<int>(y0);
        const uint32_t d256 = static_cast<uint32_t>(std::clamp(dim, 0.0f, 1.0f) * 256.0f);
        for (int y = std::max(iy0, cy0); y < std::min(iy0 + img.h, cy1); ++y) {
            const bool cornerRow = y < y0 + rmax + 1 || y > y1 - rmax - 1;
            const uint8_t* row = &img.rgba[static_cast<size_t>(y - iy0) * img.w * 4];
            uint8_t* dst = &px[(static_cast<size_t>(y) * W) * 4];
            for (int x = std::max(ix0, cx0); x < std::min(ix0 + img.w, cx1); ++x) {
                const uint8_t* s = row + (x - ix0) * 4;
                const float c = cornerRow ? cover(x + 0.5f, y + 0.5f, x0, y0, x1, y1, rt, rb) : 1.0f;
                if (c >= 0.998f) {
                    uint8_t* p = dst + x * 4;
                    p[0] = static_cast<uint8_t>((s[0] * d256) >> 8); p[1] = static_cast<uint8_t>((s[1] * d256) >> 8); p[2] = static_cast<uint8_t>((s[2] * d256) >> 8); p[3] = 255;
                } else {
                    blend(x, y, static_cast<uint32_t>(s[0] * dim) << 16 | static_cast<uint32_t>(s[1] * dim) << 8 | static_cast<uint32_t>(s[2] * dim), c);
                }
            }
        }
    }
    void circle(float cx, float cy, float r, uint32_t rgb, float a = 1.0f) { rect(cx - r, cy - r, cx + r, cy + r, r, rgb, a); }
    // A spinner: an arc `span` radians long whose head is at angle `head`, fading out toward its tail.
    void arc(float cx, float cy, float r, float t, float head, float span, uint32_t rgb, float a = 1.0f) {
        for (int y = static_cast<int>(cy - r - t); y <= static_cast<int>(cy + r + t); ++y)
            for (int x = static_cast<int>(cx - r - t); x <= static_cast<int>(cx + r + t); ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = std::sqrt(dx * dx + dy * dy);
                const float radial = std::clamp(t * 0.5f + 0.5f - std::abs(d - r), 0.0f, 1.0f);
                if (radial <= 0) continue;
                float rel = std::fmod(head - std::atan2(dy, dx), 2 * kPi);
                if (rel < 0) rel += 2 * kPi;
                const float along = rel < span ? 1.0f - rel / span : 0.0f;
                blend(x, y, rgb, a * radial * (0.12f + 0.88f * along));
            }
    }
};

// `img` scaled to w x h, cropped to that aspect ("cover"), bilinear.
Image fit(const Image& img, int w, int h) {
    Image out;
    if (img.rgba.empty() || w <= 0 || h <= 0) return out;
    out.w = w; out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    const float ba = static_cast<float>(w) / h, ia = static_cast<float>(img.w) / img.h;
    float sx0 = 0, sy0 = 0, sw = static_cast<float>(img.w), sh = static_cast<float>(img.h);
    if (ia > ba) { sw = img.h * ba; sx0 = (img.w - sw) * 0.5f; } else { sh = img.w / ba; sy0 = (img.h - sh) * 0.5f; }
    for (int y = 0; y < h; ++y) {
        const float fy = std::clamp(sy0 + (y + 0.5f) / h * sh - 0.5f, 0.0f, img.h - 1.0f);
        const int y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, img.h - 1);
        const float ty = fy - y0;
        for (int x = 0; x < w; ++x) {
            const float fx = std::clamp(sx0 + (x + 0.5f) / w * sw - 0.5f, 0.0f, img.w - 1.0f);
            const int x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, img.w - 1);
            const float tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                auto at = [&](int xx, int yy) { return static_cast<float>(img.rgba[(static_cast<size_t>(yy) * img.w + xx) * 4 + c]); };
                const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                out.rgba[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
    return out;
}

struct Font {
    std::vector<uint8_t> ttf;
    stbtt_fontinfo info{};
    float scale = 1, size = 0;
    int ascent = 0;
    bool ok = false;
    struct Glyph { int w = 0, h = 0, xo = 0, yo = 0; float adv = 0; std::vector<uint8_t> bm; };
    mutable std::map<int, Glyph> glyphs;     // rasterised once
    bool load(const char* path, float px) {
        std::ifstream f(path, std::ios::binary);
        ttf.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (ttf.empty() || !stbtt_InitFont(&info, ttf.data(), 0)) return false;
        size = px;
        scale = stbtt_ScaleForPixelHeight(&info, px);
        int d, g;
        stbtt_GetFontVMetrics(&info, &ascent, &d, &g);
        ascent = static_cast<int>(ascent * scale);
        glyphs.clear();
        return ok = true;
    }
    // Loads the first of `paths` that exists.
    bool load(std::initializer_list<const char*> paths, float px) {
        for (const char* p : paths) if (load(p, px)) return true;
        return false;
    }
    const Glyph& glyph(int c) const {
        auto it = glyphs.find(c);
        if (it != glyphs.end()) return it->second;
        Glyph g;
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, c, &adv, &lsb);
        g.adv = adv * scale;
        if (unsigned char* bm = stbtt_GetCodepointBitmap(&info, scale, scale, c, &g.w, &g.h, &g.xo, &g.yo)) {
            g.bm.assign(bm, bm + static_cast<size_t>(g.w) * g.h);
            stbtt_FreeBitmap(bm, nullptr);
        }
        return glyphs.emplace(c, std::move(g)).first->second;
    }
    // UTF-8 decode (enough for names: ASCII, Latin-1, the middle dot, the ellipsis)
    static std::vector<int> codes(const std::string& s) {
        std::vector<int> out;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = s[i];
            if (c < 0x80) { out.push_back(c); ++i; }
            else if ((c >> 5) == 6 && i + 1 < s.size()) { out.push_back((c & 31) << 6 | (s[i + 1] & 63)); i += 2; }
            else if ((c >> 4) == 14 && i + 2 < s.size()) { out.push_back((c & 15) << 12 | (s[i + 1] & 63) << 6 | (s[i + 2] & 63)); i += 3; }
            else ++i;
        }
        return out;
    }
    float width(const std::vector<int>& cs) const {
        if (!ok) return 0;
        float w = 0;
        for (size_t i = 0; i < cs.size(); ++i) {
            w += glyph(cs[i]).adv;
            if (i + 1 < cs.size()) w += stbtt_GetCodepointKernAdvance(&info, cs[i], cs[i + 1]) * scale;
        }
        return w;
    }
    float width(const std::string& s) const { return width(codes(s)); }
    // Draws `s` with its top at y; cut at maxX with an ellipsis.
    void draw(Canvas& cv, float x, float y, const std::string& s, uint32_t rgb, float maxX = 1e9f, float alpha = 1.0f) const {
        if (!ok) return;
        std::vector<int> cs = codes(s);
        if (x + width(cs) > maxX) {
            const float ell = glyph(0x2026).adv;
            std::vector<int> cut;
            float w = 0;
            for (int c : cs) {
                if (x + w + glyph(c).adv + ell > maxX) break;
                w += glyph(c).adv;
                cut.push_back(c);
            }
            while (!cut.empty() && cut.back() == ' ') cut.pop_back();
            cut.push_back(0x2026);
            cs = cut;
        }
        float pen = x;
        const int base = static_cast<int>(std::lround(y)) + ascent;
        for (size_t i = 0; i < cs.size(); ++i) {
            const Glyph& g = glyph(cs[i]);
            const int gx = static_cast<int>(std::lround(pen)) + g.xo;
            for (int yy = 0; yy < g.h; ++yy)
                for (int xx = 0; xx < g.w; ++xx)
                    if (const uint8_t v = g.bm[static_cast<size_t>(yy) * g.w + xx]) cv.blend(gx + xx, base + g.yo + yy, rgb, alpha * v / 255.0f);
            pen += g.adv;
            if (i + 1 < cs.size()) pen += stbtt_GetCodepointKernAdvance(&info, cs[i], cs[i + 1]) * scale;
        }
    }
    // Vertically centred on cy (on the capital letters).
    void drawMid(Canvas& cv, float x, float cy, const std::string& s, uint32_t rgb, float maxX = 1e9f, float alpha = 1.0f) const {
        draw(cv, x, cy - ascent + size * 0.36f, s, rgb, maxX, alpha);
    }
    // Word wrap to maxW.
    std::vector<std::string> wrap(const std::string& s, float maxW) const {
        std::vector<std::string> lines;
        std::istringstream in(s);
        std::string word, line;
        while (in >> word) {
            const std::string t = line.empty() ? word : line + " " + word;
            if (!line.empty() && width(t) > maxW) { lines.push_back(line); line = word; }
            else line = t;
        }
        if (!line.empty()) lines.push_back(line);
        return lines;
    }
};

// ── The panel ──

// Colours (sRGB): a calm dark theme with one warm accent.
// Neutral greys; the accent is only ever the one primary action (Start Frame Home when it isn't running). What's
// selected or showing is marked in the text colour, not the accent.
constexpr uint32_t kBg = 0x111214, kSurface = 0x1b1c1f, kSurfaceHi = 0x26282c, kSunken = 0x0c0d0e, kLine = 0x2c2e33,
                   kText = 0xeceae6, kMuted = 0x9c9a95, kFaint = 0x6c6a66, kAccent = 0xe8a050, kOnAccent = 0x1c1307, kError = 0xd9534f;
constexpr float kR = 8;                 // corner radius: cards, buttons, controls

// What the Debug page shows: Frame Home's stats.json (every 2 s) and report.json (after each load), as lines.
struct DebugInfo {
    std::vector<std::string> now;                       // how it's running
    std::vector<std::string> costly;                    // GPU cost view: the costliest meshes
    std::string home;                                   // the report's summary
    std::vector<std::pair<int, std::string>> problems;  // (level, what), worst first
    bool operator==(const DebugInfo& o) const { return now == o.now && costly == o.costly && home == o.home && problems == o.problems; }
};

DebugInfo readDebugInfo() {
    DebugInfo d;
    char b[256];
    try {
        const tinyjson::Value st = tinyjson::parse(readText(rootDir() / "stats.json"));
        if (st.isObject() && st.has("budgetMs")) {
            const float budget = st["budgetMs"].asFloat();
            if (st.has("gpuMs")) {
                std::snprintf(b, sizeof b, "GPU %.1f ms of %.1f a frame%s", st["gpuMs"].asFloat(), budget, st["gpuMs"].asFloat() > budget * 1.03f ? " (too slow)" : "");
                std::string line = b;
                if (st.has("cpuMs")) {   // (a Frame Home from before the Debug page doesn't say)
                    std::snprintf(b, sizeof b, "  ·  CPU %.1f ms  ·  %.0f frames a second", st["cpuMs"].asFloat(), st["fps"].asFloat());
                    line += b;
                }
                d.now.push_back(line);
            }
            if (st.has("draws")) {
                std::snprintf(b, sizeof b, "%d draws, %.2f M triangles  ·  %d of %d meshes in view  ·  %d x %d per eye (%.0f%%)",
                              static_cast<int>(st["draws"].asFloat()), st["triangles"].asFloat() / 1e6, static_cast<int>(st["drawn"].asFloat()),
                              static_cast<int>(st["meshes"].asFloat()), static_cast<int>(st["width"].asFloat()), static_cast<int>(st["height"].asFloat()),
                              st["scale"].asFloat() * 100);
                d.now.push_back(b);
                static const char* fovNames[4] = {"off", "gentle", "medium", "strong"};
                const int fl = std::clamp(static_cast<int>(st["foveationLevel"].asFloat()), 0, 3);
                std::snprintf(b, sizeof b, "Memory %.0f MB  ·  %d textures still loading  ·  foveation %s%s%s", st["ramMB"].asFloat(),
                              static_cast<int>(st["texturesLoading"].asFloat()), st.has("foveationLevel") ? fovNames[fl] : "?",
                              st["loading"].boolVal ? "  ·  loading an environment" : "", st["dashboard"].boolVal ? "  ·  Steam menu open" : "");
                d.now.push_back(b);
            }
            if (st.has("costly"))
                for (size_t i = 0; i < st["costly"].size(); ++i) {
                    std::snprintf(b, sizeof b, "%6.3f ms  ", st["costly"][i]["ms"].asFloat());
                    d.costly.push_back(b + st["costly"][i]["name"].asString());
                }
        }
    } catch (...) {}
    try {
        const tinyjson::Value r = tinyjson::parse(readText(rootDir() / "report.json"));
        if (r.isObject() && r.has("meshes")) {
            std::snprintf(b, sizeof b, "%s  ·  %d meshes, %.2f M triangles, %d see-through  ·  %.0f MB of textures  ·  loaded in %.1f s",
                          r["format"].asString().c_str(), static_cast<int>(r["meshes"].asFloat()), r["triangles"].asFloat() / 1e6,
                          static_cast<int>(r["seeThrough"].asFloat()), r["textureMB"].asFloat() + r["decodedMB"].asFloat(), r["loadSec"].asFloat());
            d.home = r["name"].asString() + "\n" + b;
            for (size_t i = 0; i < r["problems"].size(); ++i)
                d.problems.push_back({static_cast<int>(r["problems"][i]["level"].asFloat()), r["problems"][i]["what"].asString()});
        }
    } catch (...) {}
    return d;
}

struct Panel {
    Canvas cv;
    Font title, head, body, label, small, tiny, glyphFont;
    std::vector<Env> envs;
    std::string current;
    std::string toast;
    bool toastError = false;
    double toastUntil = 0;
    int page = 0;                       // 0 environments, 1 settings, 2 debug
    PerfSettings perf;
    std::string stats;                  // a line from Frame Home's stats.json
    DebugSettings dbg;                  // debug.json: the Debug page's views
    DebugInfo info;                     // ...and what it shows of stats.json and report.json
    static constexpr int kShotId = 9200, kDebugOffId = 9201;
    // Frame Home (frame-home --system), as the panel's loop sees it; the header's button restarts or starts it.
    enum class Home { Running, Stopped, TurnedOff, Restarting };
    Home home = Home::Running;
    bool restartAsked = false;          // the button was clicked: the loop restarts or starts it
    static constexpr int kHomeId = 9100;

    // Layout
    static constexpr int kCols = 3, kPad = 40, kTop = 112, kGap = 24, kThumbH = 216, kInfoH = 84, kCardH = kThumbH + kInfoH;
    static int cardW() { return (W - kPad * 2 - (kCols - 1) * kGap) / kCols; }

    // Clickable things, collected as they're drawn. ids: tabs 9000 + page, settings options row * 100 + option,
    // environment cards kCardId + index, and on a card its delete button (kTrashId), and while it asks "Delete?"
    // its Delete (kDelId) and Cancel (kCancelId) buttons.
    static constexpr int kCardId = 10000, kTrashId = 30000, kDelId = 50000, kCancelId = 70000, kIdSpan = 20000;
    static int cardOf(int id) { return id >= kCardId && id < kCancelId + kIdSpan ? (id - kCardId) % kIdSpan : -1; }
    struct Hot { float x0, y0, x1, y1; int id; };
    std::vector<Hot> hots;
    int hotHover = -1;
    int hotAt(float x, float y) const {
        for (const Hot& h : hots) if (x >= h.x0 && x < h.x1 && y >= h.y0 && y < h.y1) return h.id;
        return -1;
    }
    void addHot(float x0, float y0, float x1, float y1, int id, bool inContent) {
        if (inContent) { y0 = std::max(y0, static_cast<float>(kTop)); y1 = std::min(y1, static_cast<float>(H)); }
        if (y1 > y0) hots.push_back({x0, y0, x1, y1, id});
    }

    // The pointer, pressing, dragging and the scroll of each page (pixels, eased toward target).
    float mx = -1, my = -1;
    bool pointerIn = false, pressed = false, dragging = false;
    float pressX = 0, pressY = 0, dragFrom = 0;
    int pressHot = -1;
    std::vector<std::pair<double, float>> dragTrail;   // (time, y) of the last moves: the fling speed
    struct Scroll { float pos = 0, target = 0, vel = 0, content = 0; double active = -10; };
    // Fling after a drag: speed gain, top speed (px/s) and how fast it slows (1/s; lower coasts further).
    // FH_SCROLL_FLING_GAIN / _MAX / _DECAY override.
    float kFlingGain = 1.0f, kFlingMax = 12000.0f, kFlingDecay = 2.2f;
    Scroll sc[3];
    float viewH() const { return static_cast<float>(H - kTop); }
    float maxScroll(int p) const { return std::max(0.0f, sc[p].content - viewH()); }
    std::vector<float> hoverT;          // per card, 0..1, eased
    std::string confirmKey;             // the card asking "Delete?" (its key; "" none)
    double confirmStart = 0;
    std::string switchingKey;           // the card clicked: a spinner on it until switchUntil
    double switchStart = 0, switchUntil = 0;
    double now = 0;                     // seconds, steady clock
    bool needRedraw = false;            // set by render when what's under the pointer changed

    static double clockNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    // ── Input ──
    bool onMove(float x, float y) {
        mx = x; my = y; pointerIn = true;
        if (pressed && !dragging && std::abs(y - pressY) > 10 && pressY >= kTop) {   // a drag, not a click
            dragging = true;
            pressHot = -1;
            pressY = y;
            dragFrom = sc[page].pos;
        }
        if (dragging) {
            Scroll& s = sc[page];
            float v = dragFrom - (y - pressY);
            const float m = maxScroll(page);
            if (v < 0) v *= 0.35f; else if (v > m) v = m + (v - m) * 0.35f;   // rubber band past the ends
            s.pos = s.target = v;
            s.vel = 0;
            s.active = now;
            dragTrail.push_back({clockNow(), y});   // the real time of the move (several can arrive in one poll)
            while (dragTrail.size() > 2 && dragTrail.back().first - dragTrail.front().first > 0.1) dragTrail.erase(dragTrail.begin());
            hotHover = -1;
            return true;
        }
        const int h = hotAt(x, y);
        if (h != hotHover) { hotHover = h; return true; }
        return false;
    }
    bool onDown(float x, float y) {
        mx = x; my = y; pointerIn = true;
        pressed = true; dragging = false;
        pressX = x; pressY = y;
        pressHot = hotAt(x, y);
        dragTrail.assign(1, {clockNow(), y});
        Scroll& s = sc[page];
        if (s.vel != 0) { s.vel = 0; s.target = std::clamp(s.pos, 0.0f, maxScroll(page)); }   // a press catches a fling
        return true;
    }
    // Returns the id clicked (press and release on the same thing, no drag), else -1.
    int onUp(float x, float y) {
        const bool wasDrag = dragging;
        pressed = dragging = false;
        if (wasDrag) {
            Scroll& s = sc[page];
            if (dragTrail.size() >= 2) {
                const auto& a = dragTrail.front();
                const auto& b = dragTrail.back();
                const double dt = b.first - a.first;
                if (dt > 0.008 && clockNow() - b.first < 0.1) s.vel = std::clamp(static_cast<float>(-(b.second - a.second) / dt) * kFlingGain, -kFlingMax, kFlingMax);
            }
            if (std::abs(s.vel) < 60) s.vel = 0;
            s.active = now;
            hotHover = hotAt(x, y);
            return -1;
        }
        const int id = hotAt(x, y);
        const int was = pressHot;
        pressHot = -1;
        if (id < 0 && was < 0) confirmKey.clear();   // a click on nothing: the "Delete?" question goes away
        return id >= 0 && id == was ? id : -1;
    }
    void onLeave() {
        pointerIn = false;
        if (dragging) { dragging = false; sc[page].active = now; }
        pressed = false;
        pressHot = hotHover = -1;
    }
    void onScroll(float dy) {   // pixels, positive = further down the list
        if (dragging) return;
        Scroll& s = sc[page];
        s.vel = 0;
        s.target = std::clamp(s.target + dy, 0.0f, maxScroll(page));
        s.active = now;
    }

    // Advances the animations by dt; true while anything still moves (another frame wanted).
    bool tick(double dt) {
        bool moving = false;
        const float fdt = static_cast<float>(std::min(dt, 0.1));
        for (int p = 0; p < 2; ++p) {
            Scroll& s = sc[p];
            const float m = maxScroll(p);
            if (!(dragging && p == page)) {
                if (s.vel != 0) {   // a fling: coast and slow down
                    s.target += s.vel * fdt;
                    s.vel *= std::exp(-fdt * kFlingDecay);
                    if (std::abs(s.vel) < 20) s.vel = 0;
                    if (s.target < 0 || s.target > m) { s.target = std::clamp(s.target, 0.0f, m); s.vel = 0; }
                    s.pos = s.target;
                    moving = true;
                } else if (s.target < 0 || s.target > m) {
                    s.target = std::clamp(s.target, 0.0f, m);
                }
                const float d = s.target - s.pos;
                if (std::abs(d) > 0.25f) { s.pos += d * (1 - std::exp(-fdt * 32.0f)); moving = true; }   // catches up in ~30 ms
                else if (d != 0) { s.pos = s.target; moving = true; }
            }
            if (now - s.active < 1.2) moving = true;    // the scroll bar fades out
        }
        hoverT.resize(envs.size(), 0.0f);
        for (size_t i = 0; i < hoverT.size(); ++i) {
            const float want = page == 0 && cardOf(hotHover) == static_cast<int>(i) ? 1.0f : 0.0f, d = want - hoverT[i];
            if (std::abs(d) > 0.01f) { hoverT[i] += d * (1 - std::exp(-fdt * 20.0f)); moving = true; }
            else if (d != 0) { hoverT[i] = want; moving = true; }
        }
        if (!switchingKey.empty()) { moving = true; if (now >= switchUntil) switchingKey.clear(); }
        if (!toast.empty()) { moving = true; if (now >= toastUntil) toast.clear(); }
        if (!confirmKey.empty() && now - confirmStart < 0.3) moving = true;   // the question fades in
        return moving;
    }

    void say(const std::string& s, bool error = false, double seconds = 2.5) {
        toast = s; toastError = error; toastUntil = now + seconds;
    }

    // ── Settings ──
    // Rows 0-4 graphics, 5-9 and 10-11 movement, 12-14 the home, 15-19 the Debug page.
    static constexpr int kRowCount = 22;
    static constexpr const char* kRows[kRowCount] = {"Resolution", "Anti-aliasing", "Texture filtering", "Foveated rendering", "Foveation strength",
                                                     "Walking", "Teleport", "Turning", "Snap turn angle", "Walking speed",
                                                     "Comfort vignette", "Seat markers", "Volume", "Animations", "Behind the Steam menu",
                                                     "View", "World layer", "See-through surfaces", "Cut-out surfaces", "Sky", "Frame rate", "Keep cool"};
    static constexpr const char* kHelp[kRowCount] = {
        "Auto keeps every frame on time and sharpens whenever the home is light enough.",
        "Smooths jagged edges. 4x looks best; lower is faster.",
        "Keeps floors and walls sharp at an angle. Higher looks better.",
        "Full detail where you look, coarser toward the edges: frees the GPU for more resolution.",
        "How quickly detail falls off away from where you look.",
        "Left stick: walk smoothly in the direction you push it.",
        "Right stick forward, aim, let go. Instant blinks you there; Slide glides.",
        "Right stick left and right.",
        "How far each snap turn goes.",
        "How fast smooth walking moves you.",
        "Darkens the edges of view while the stick moves or turns you. Helps if smooth walking makes you queasy.",
        "Markers on the seats near you. Hidden, pointing at a seat still finds it.",
        "All of the environment's sound.",
        "Hold still stops the environment's moving parts; animated environments then cost less.",
        "With the menu open the environment is only its backdrop. Half rate leaves the GPU to the menu and its apps.",
        "Wireframe: every triangle edge. Meshes: each in its own colour. GPU cost: green is cheap, red is costly. Overdraw: bright where surfaces pile up.",
        "Where you can walk (green), surfaces and colliders, spawns, seats, wall spots and gravity zones.",
        "Glass, water, glow and fog. Leave them out to see whether they're what's slow.",
        "Leaves, grass and fences. Leave them out to see whether they're what's slow.",
        "The sky around the environment.",
        "Half draws the environment every other frame and turns it with your head in between, as Meta's home does: each frame gets twice the GPU time.",
        "Above this temperature the edges of view give up a little detail at a time until the headset cools (where you look stays sharp). A hot headset slows its GPU."};
    std::vector<std::string> options(int row) const {
        switch (row) {
            case 0: return {"Auto", "80%", "100%", "120%", "140%"};
            case 1: return {"Off", "2x", "4x"};
            case 2: return {"4x", "8x", "16x"};
            case 3: return {"Off", "Fixed", "Eye tracked"};
            case 4: return {"Gentle", "Medium", "Strong"};
            case 5: return {"Off", "Smooth"};
            case 6: return {"Off", "Instant", "Slide"};
            case 7: return {"Snap", "Smooth"};
            case 8: return {"30°", "45°", "90°"};
            case 9: return {"Slow", "Normal", "Fast"};
            case 10: return {"Off", "Gentle", "Strong"};
            case 11: return {"Shown", "Hidden"};
            case 12: return {"Off", "25%", "50%", "75%", "100%"};
            case 13: return {"Play", "Hold still"};
            case 14: return {"Half rate", "Full rate"};
            case 15: return {"Normal", "Wireframe", "Meshes", "GPU cost", "Overdraw"};
            case 16: return {"Hidden", "Shown"};
            case 20: return {"Full", "Half"};
            case 21: return {"Off", "80°", "86°", "90°"};
            default: return {"Shown", "Left out"};
        }
    }
    int selected(int row) const {
        switch (row) {
            case 0: if (perf.autoResolution) return 0; return perf.resolution < 0.9f ? 1 : perf.resolution < 1.1f ? 2 : perf.resolution < 1.3f ? 3 : 4;
            case 1: return perf.msaa >= 4 ? 2 : perf.msaa >= 2 ? 1 : 0;
            case 2: return perf.anisotropy >= 16 ? 2 : perf.anisotropy >= 8 ? 1 : 0;
            case 3: return perf.foveation;
            case 4: return perf.foveationLevel - 1;
            case 5: return perf.walk;
            case 6: return perf.teleport;
            case 7: return perf.turn;
            case 8: return perf.snapAngle >= 90 ? 2 : perf.snapAngle >= 45 ? 1 : 0;
            case 9: return perf.moveSpeed < 1.7f ? 0 : perf.moveSpeed < 2.5f ? 1 : 2;
            case 10: return perf.vignette;
            case 11: return perf.seatMarkers ? 0 : 1;
            case 12: return static_cast<int>(std::lround(perf.volume * 4));
            case 13: return perf.animations ? 0 : 1;
            case 14: return perf.menuRate;
            case 15: return dbg.view;
            case 16: return dbg.world;
            case 17: return dbg.hideSeeThrough;
            case 18: return dbg.hideCutout;
            case 20: return perf.frameRate;
            case 21: return perf.tempLimit <= 0 ? 0 : perf.tempLimit <= 82 ? 1 : perf.tempLimit <= 88 ? 2 : 3;
            default: return dbg.hideSky;
        }
    }
    void choose(int row, int opt) {
        switch (row) {
            case 0: perf.autoResolution = opt == 0; if (opt > 0) perf.resolution = 0.6f + 0.2f * opt; break;
            case 1: perf.msaa = opt == 2 ? 4 : opt == 1 ? 2 : 1; break;
            case 2: perf.anisotropy = opt == 2 ? 16 : opt == 1 ? 8 : 4; break;
            case 3: perf.foveation = opt; break;
            case 4: perf.foveationLevel = opt + 1; break;
            case 5: perf.walk = opt; break;
            case 6: perf.teleport = opt; break;
            case 7: perf.turn = opt; break;
            case 8: perf.snapAngle = opt == 2 ? 90 : opt == 1 ? 45 : 30; break;
            case 9: perf.moveSpeed = opt == 0 ? 1.4f : opt == 1 ? 2.0f : 3.0f; break;
            case 10: perf.vignette = opt; break;
            case 11: perf.seatMarkers = opt == 0 ? 1 : 0; break;
            case 12: perf.volume = opt * 0.25f; break;
            case 13: perf.animations = opt == 0 ? 1 : 0; break;
            case 14: perf.menuRate = opt; break;
            case 15: dbg.view = opt; break;
            case 16: dbg.world = opt; break;
            case 17: dbg.hideSeeThrough = opt; break;
            case 18: dbg.hideCutout = opt; break;
            case 20: perf.frameRate = opt; break;
            case 21: perf.tempLimit = opt == 0 ? 0 : opt == 1 ? 80 : opt == 2 ? 86 : 90; break;
            default: dbg.hideSky = opt; break;
        }
        perf.clamp();
    }
    bool dimmed(int row) const {
        return ((row == 4 || row == 21) && perf.foveation == 0) || (row == 8 && perf.turn == 1) || (row == 9 && perf.walk == 0) ||
               (row == 10 && perf.walk == 0 && perf.turn == 0 && perf.teleport != 2);
    }
    // The segmented control's option widths.
    std::vector<float> segWidths(const std::vector<std::string>& opts) const {
        std::vector<float> w;
        for (const auto& o : opts) w.push_back(std::max(label.width(o) + 36.0f, 84.0f));
        return w;
    }
    // One settings row at content y (screen y = y - scroll); returns its height.
    float settingsRow(int row, float x0, float x1, float y, bool draw) {
        const std::vector<std::string> opts = options(row);
        const std::vector<float> ws = segWidths(opts);
        float segW = 8;
        for (float w : ws) segW += w;
        const float textMax = x1 - 28 - segW - 32 - (x0 + 28);
        const std::vector<std::string> help = small.wrap(kHelp[row], textMax);
        const float h = std::max(96.0f, 30.0f + 30 + help.size() * 26.0f + 18);
        if (!draw) return h;
        const float sy = kTop + y - sc[page].pos;
        if (sy > H || sy + h < kTop) return h;
        const bool dim = dimmed(row);
        body.draw(cv, x0 + 28, sy + 22, kRows[row], dim ? kMuted : kText, x0 + 28 + textMax);
        for (size_t i = 0; i < help.size(); ++i) small.draw(cv, x0 + 28, sy + 58 + i * 26.0f, help[i], dim ? kFaint : kMuted);
        // segmented control, right-aligned and centred in the row
        const float cy = sy + h * 0.5f, segX = x1 - 28 - segW;
        cv.rect(segX, cy - 26, segX + segW, cy + 26, kR, kSunken);
        const int sel = selected(row);
        float x = segX + 4;
        for (size_t o = 0; o < opts.size(); ++o) {
            const int id = row * 100 + static_cast<int>(o);
            const bool on = static_cast<int>(o) == sel, hov = hotHover == id;
            if (on) cv.rect(x, cy - 22, x + ws[o], cy + 22, kR - 2, dim ? 0x3a3b3e : kText);
            else if (hov) cv.rect(x, cy - 22, x + ws[o], cy + 22, kR - 2, kSurfaceHi);
            const uint32_t tc = on ? (dim ? kText : kBg) : hov ? kText : kMuted;
            label.drawMid(cv, x + (ws[o] - label.width(opts[o])) * 0.5f, cy, opts[o], tc);
            addHot(x, cy - 24, x + ws[o], cy + 24, id, true);
            x += ws[o];
        }
        return h;
    }
    // A titled group of rows in one rounded card; returns the content y after it.
    float settingsGroup(const char* name, const std::string& sub, const std::vector<int>& rows, float y) {
        const float x0 = kPad, x1 = W - kPad;
        const float sy = kTop + y - sc[page].pos;
        head.draw(cv, x0 + 4, sy, name, kText);
        if (!sub.empty()) small.draw(cv, x0 + 4 + head.width(name) + 16, sy + 8, sub, kFaint, x1);
        y += 50;
        float total = 0;
        for (int r : rows) total += settingsRow(r, x0, x1, 0, false);
        const float cy0 = kTop + y - sc[page].pos;
        cv.rect(x0, cy0, x1, cy0 + total, kR, kSurface);
        float ry = y;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) { const float ly = kTop + ry - sc[page].pos; cv.rect(x0 + 28, ly, x1 - 28, ly + 1, 0, kLine); }
            ry += settingsRow(rows[i], x0, x1, ry, true);
        }
        return y + total;
    }
    void renderSettings() {
        float y = 12;
        y = settingsGroup("Movement", "Applies right away", {5, 6, 7, 8, 9, 10, 11}, y);
        y += 36;
        y = settingsGroup("Environment", "Applies right away", {12, 13}, y);
        y += 36;
        y = settingsGroup("Graphics", stats.empty() ? "Performance and quality" : stats, {0, 20, 1, 2, 3, 4, 21, 14}, y);
        y += 20;
        const std::vector<std::string> note = small.wrap("Anti-aliasing, texture filtering and turning foveation on or off restart Frame Home (a few seconds).", W - 2 * kPad - 8);
        for (const auto& l : note) { small.draw(cv, kPad + 4, kTop + y - sc[1].pos, l, kFaint); y += 26; }
        sc[1].content = y + 40;
    }

    // ── Debug ──
    // A titled card of text lines (colour, text), wrapped; returns the content y after it.
    float infoCard(const char* name, const std::string& sub, const std::vector<std::pair<uint32_t, std::string>>& lines, float y) {
        const float x0 = kPad, x1 = W - kPad, tx0 = x0 + 28, tx1 = x1 - 28;
        const float sy = kTop + y - sc[page].pos;
        head.draw(cv, x0 + 4, sy, name, kText);
        if (!sub.empty()) small.draw(cv, x0 + 4 + head.width(name) + 16, sy + 8, sub, kFaint, x1);
        y += 50;
        std::vector<std::pair<uint32_t, std::string>> wrapped;
        for (const auto& l : lines)
            for (const auto& w : small.wrap(l.second, tx1 - tx0)) wrapped.push_back({l.first, w});
        const float h = 24 + wrapped.size() * 30.0f + 14;
        const float cy0 = kTop + y - sc[page].pos;
        cv.rect(x0, cy0, x1, cy0 + h, kR, kSurface);
        for (size_t i = 0; i < wrapped.size(); ++i) small.draw(cv, tx0, cy0 + 22 + i * 30.0f, wrapped[i].second, wrapped[i].first, tx1);
        return y + h;
    }
    // A neutral button at content y (left x0); shown dimmed and not clickable when !enabled. Returns its width.
    float button(float x0, float y, const char* text, int id, bool enabled) {
        const float sy = kTop + y - sc[page].pos, bw = label.width(text) + 40;
        const bool hov = enabled && hotHover == id;
        cv.rect(x0, sy, x0 + bw, sy + 48, kR, !enabled ? kSurface : hov ? 0x34363b : kSurfaceHi);
        label.drawMid(cv, x0 + 20, sy + 24, text, enabled ? kText : kFaint);
        if (enabled) addHot(x0, sy, x0 + bw, sy + 48, id, true);
        return bw;
    }
    void renderDebug() {
        float y = 12;
        y = settingsGroup("Views", "Applies right away; off again when Frame Home restarts", {15, 16}, y);
        y += 36;
        y = settingsGroup("Leave out", "To find the part of an environment that's slow", {17, 18, 19}, y);
        y += 36;
        {
            std::vector<std::pair<uint32_t, std::string>> l;
            for (const auto& n : info.now) l.push_back({kMuted, n});
            if (l.empty()) l.push_back({kFaint, "Frame Home hasn't reported yet."});
            y = infoCard("Now", "Every 2 seconds", l, y);
        }
        if (dbg.view == 3) {
            y += 36;
            std::vector<std::pair<uint32_t, std::string>> l;
            for (const auto& c : info.costly) l.push_back({kMuted, c});
            if (l.empty()) l.push_back({kFaint, "Measuring…"});
            y = infoCard("Costliest meshes", "GPU time each, both eyes", l, y);
        }
        y += 36;
        {
            std::vector<std::pair<uint32_t, std::string>> l;
            if (info.home.empty()) l.push_back({kFaint, "No report yet: it's written when an environment loads."});
            else {
                const size_t nl = info.home.find('\n');
                l.push_back({kText, info.home.substr(0, nl)});
                if (nl != std::string::npos) l.push_back({kMuted, info.home.substr(nl + 1)});
                if (info.problems.empty()) l.push_back({kMuted, "Nothing wrong found."});
                for (const auto& pr : info.problems)
                    l.push_back({pr.first >= 2 ? kError : pr.first == 1 ? kText : kMuted, std::string(pr.first >= 2 ? "Broken: " : pr.first == 1 ? "" : "Note: ") + pr.second});
            }
            y = infoCard("This environment", "Its report (logs/report.txt on the headset)", l, y);
        }
        y += 28;
        float x = kPad;
        x += button(x, y, "Save a Picture", kShotId, true) + 16;
        button(x, y, "Turn Debug Views Off", kDebugOffId, dbg.any());
        y += 48 + 14;
        for (const auto& l : small.wrap("Pictures go to ~/frame-home/logs on the headset. GPU cost times every draw, which makes the GPU a little slower while it's on.", W - 2 * kPad - 8)) {
            small.draw(cv, kPad + 4, kTop + y - sc[page].pos, l, kFaint);
            y += 26;
        }
        sc[2].content = y + 40;
    }

    // ── Environments ──
    float envContent() const {
        const int rows = (static_cast<int>(envs.size()) + kCols - 1) / kCols;
        return rows ? 8 + rows * (kCardH + kGap) - kGap + 40 : 0;
    }
    void card(size_t i) {
        Env& e = envs[i];
        const int cw = cardW();
        const float x0 = static_cast<float>(kPad + (i % kCols) * (cw + kGap));
        const float y0 = kTop + 8 + static_cast<float>(i / kCols) * (kCardH + kGap) - sc[0].pos;
        const float x1 = x0 + cw, y1 = y0 + kCardH, imgB = y0 + kThumbH;
        if (y0 > H + 4 || y1 < kTop - 4) return;
        const bool cur = e.key == current, switching = e.key == switchingKey;
        const float hv = i < hoverT.size() ? hoverT[i] : 0.0f;
        const bool pressing = pressed && pressHot == 10000 + static_cast<int>(i);
        cv.rect(x0, y0, x1, y1, kR, mix(kSurface, kSurfaceHi, hv));
        if (!e.thumb.rgba.empty()) {
            if (e.fitted.w != cw || e.fitted.h != kThumbH) e.fitted = fit(e.thumb, cw, kThumbH);
            cv.blit(e.fitted, x0, y0, kR, 0, pressing ? 0.85f : 1.0f);
        } else {   // no picture yet: a plain panel with the name's first letter
            cv.rect(x0, y0, x1, imgB, kR, kSunken, 1.0f, 0);
            const std::vector<int> cs = Font::codes(e.name);
            std::string letter = cs.empty() ? "?" : std::string(1, static_cast<char>(cs[0] < 0x80 ? std::toupper(cs[0]) : '?'));
            glyphFont.draw(cv, (x0 + x1) * 0.5f - glyphFont.width(letter) * 0.5f, (y0 + imgB) * 0.5f - glyphFont.size * 0.5f, letter, kFaint);
        }
        if (switching) {   // switching: darken the picture, spin
            cv.rect(x0, y0, x1, imgB, kR, 0x000000, 0.55f, 0);
            cv.arc((x0 + x1) * 0.5f, (y0 + imgB) * 0.5f, 22, 4, static_cast<float>((now - switchStart) * 2 * kPi), 4.2f, kText);
        }
        body.draw(cv, x0 + 20, imgB + 14, e.name, kText, x1 - 18);
        if (switching) small.draw(cv, x0 + 20, imgB + 50, "Switching…", kText, x1 - 18);
        else if (cur) small.draw(cv, x0 + 20, imgB + 50, "Showing now", kText, x1 - 18);
        else small.draw(cv, x0 + 20, imgB + 50, e.kind, kMuted, x1 - 18);
        const int ii = static_cast<int>(i);
        if (!confirmKey.empty() && e.key == confirmKey) {   // "Delete <name>?" over the card, Delete / Cancel
            const float a = static_cast<float>(std::clamp((now - confirmStart) / 0.15, 0.0, 1.0));
            cv.rect(x0, y0, x1, y1, kR, kSurface, a);
            const float tx0 = x0 + 24, tx1 = x1 - 24;
            const std::vector<std::string> q = body.wrap("Delete " + e.name + "?", tx1 - tx0);
            float ty = y0 + 30;
            for (size_t l = 0; l < q.size() && l < 2; ++l, ty += 34) body.draw(cv, tx0, ty, q[l], kText, tx1, a);
            std::string what = e.files.size() > 1 ? "Removes both files of the pair from the headset." : "This removes it from the headset.";
            if (cur) what += " Another environment will show instead.";
            for (const auto& l : small.wrap(what, tx1 - tx0)) { small.draw(cv, tx0, ty + 6, l, kMuted, tx1, a); ty += 26; }
            const float by1 = y1 - 22, by0 = by1 - 54, mid = (x0 + x1) * 0.5f;
            const bool hc = hotHover == kCancelId + ii, hd = hotHover == kDelId + ii;
            cv.rect(tx0, by0, mid - 6, by1, kR, hc ? 0x34363b : kSurfaceHi, a);
            label.drawMid(cv, (tx0 + mid - 6) * 0.5f - label.width("Cancel") * 0.5f, (by0 + by1) * 0.5f, "Cancel", kText, 1e9f, a);
            cv.rect(mid + 6, by0, tx1, by1, kR, hd ? 0xe46a66 : kError, a);
            label.drawMid(cv, (mid + 6 + tx1) * 0.5f - label.width("Delete") * 0.5f, (by0 + by1) * 0.5f, "Delete", 0xffffff, 1e9f, a);
            addHot(tx0, by0, mid - 6, by1, kCancelId + ii, true);
            addHot(mid + 6, by0, tx1, by1, kDelId + ii, true);
            cv.ring(x0, y0, x1, y1, kR, 2, kError, a);
        } else {
            if (cur) cv.ring(x0 - 3, y0 - 3, x1 + 3, y1 + 3, kR + 3, 2, kText);
            else if (hv > 0.01f) cv.ring(x0, y0, x1, y1, kR, 1, 0x5c5e63, hv);
            else cv.ring(x0, y0, x1, y1, kR, 1, kLine);
            if (hv > 0.01f && !switching) {   // the delete button, top right of the picture, while pointed at
                const float bx = x1 - 34, by = y0 + 34;
                const bool ht = hotHover == kTrashId + ii;
                cv.rect(bx - 22, by - 22, bx + 22, by + 22, kR, ht ? kError : 0x0c0d0e, (ht ? 1.0f : 0.7f) * hv);
                const uint32_t ic = 0xffffff;
                const float ia = (ht ? 1.0f : 0.85f) * hv;
                cv.rect(bx - 3, by - 12, bx + 3, by - 9, 1, ic, ia);           // handle
                cv.rect(bx - 10, by - 9, bx + 10, by - 6.5f, 1, ic, ia);       // lid
                cv.rect(bx - 7.5f, by - 4.5f, bx + 7.5f, by + 11, 2.5f, ic, ia);   // can
                for (float sx : {-3.5f, 0.0f, 3.5f})                           // its ribs
                    cv.rect(bx + sx - 0.8f, by - 2, bx + sx + 0.8f, by + 8.5f, 0, ht ? kError : 0x2c2e33, ia);
                addHot(bx - 26, by - 26, bx + 26, by + 26, kTrashId + ii, true);
            }
        }
        addHot(x0, y0, x1, y1, kCardId + ii, true);
    }
    void renderEnvs() {
        sc[0].content = envContent();
        if (envs.empty()) {
            const float cy = kTop + (H - kTop) * 0.42f;
            const std::string a = "No environments yet", b = "Send one from Frame Studio: Headset > Install on Steam Frame.";
            head.draw(cv, (W - head.width(a)) * 0.5f, cy - 40, a, kText);
            small.draw(cv, (W - small.width(b)) * 0.5f, cy + 8, b, kMuted);
            return;
        }
        for (size_t i = 0; i < envs.size(); ++i) card(i);
    }

    // ── The whole panel ──
    void render() {
        hots.clear();
        cv.noClip();
        cv.clear(kBg);
        // Content, scrolled, under the header.
        cv.clip(0, kTop, W, H);
        if (page == 0) renderEnvs(); else if (page == 1) renderSettings(); else renderDebug();
        cv.fade(0, H - 28, W, H, kBg, 0.0f, maxScroll(page) - sc[page].pos > 2 ? 0.9f : 0.0f);   // more below
        cv.fade(0, kTop, W, kTop + 22, 0x000000, sc[page].pos > 1 ? 0.35f : 0.0f, 0.0f);      // shadow under the header
        scrollbar();
        cv.noClip();
        // Header: the two pages as tabs (the open one underlined), and on the right Frame Home's state and button.
        {
            const char* names[3] = {"Environments", "Settings", "Debug"};
            const float y0 = 30, y1 = 84, cy = (y0 + y1) * 0.5f;
            float x = kPad;
            for (int t = 0; t < 3; ++t) {
                const bool on = page == t, hov = hotHover == 9000 + t;
                const float w = head.width(names[t]);
                head.drawMid(cv, x, cy - 2, names[t], on ? kText : hov ? 0xc4c2bd : kFaint);
                if (on) cv.rect(x, y1 - 6, x + w, y1 - 3, 1, kText);
                addHot(x - 12, y0, x + w + 12, y1, 9000 + t, false);
                x += w + 40;
            }
            homeControl(W - kPad, cy);
            cv.rect(0, kTop - 1, W, kTop, 0, kLine, sc[page].pos > 1 ? 1.0f : 0.0f);
        }
        drawToast();
        // What's under the pointer may have moved (scrolling): hover follows on the next frame.
        const int h = pointerIn && !dragging ? hotAt(mx, my) : -1;
        needRedraw = h != hotHover;
        hotHover = h;
    }
    // Frame Home's state and its button, right-aligned at x1. Running: a neutral Restart (it reloads the home from
    // its file). Not running, or turned off by the layer's crash guard: Start / Turn On, the page's one primary
    // action. Restarting: shown, not clickable.
    void homeControl(float x1, float cy) {
        const char* text = "";
        const char* button = "";
        bool enabled = true, primary = false;
        switch (home) {
            case Home::Running: text = "Frame Home is running"; button = "Restart Frame Home"; break;
            case Home::Stopped: text = "Frame Home isn't running"; button = "Start Frame Home"; primary = true; break;
            case Home::Restarting: text = ""; button = "Restarting…"; enabled = false; break;
            case Home::TurnedOff: text = "Off: it failed to start twice"; button = "Turn Frame Home On"; primary = true; break;
        }
        const float bw = label.width(button) + 40, bx0 = x1 - bw, by0 = cy - 24, by1 = cy + 24;
        const bool hov = enabled && hotHover == kHomeId;
        const uint32_t fill = !enabled ? kSurface : primary ? (hov ? 0xf0b26a : kAccent) : hov ? 0x34363b : kSurfaceHi;
        cv.rect(bx0, by0, x1, by1, kR, fill);
        label.drawMid(cv, bx0 + 20, cy, button, !enabled ? kFaint : primary ? kOnAccent : kText);
        if (enabled) addHot(bx0, by0, x1, by1, kHomeId, false);
        if (*text) small.drawMid(cv, bx0 - 20 - small.width(text), cy, text, kFaint);
    }
    void scrollbar() {
        const Scroll& s = sc[page];
        const float m = maxScroll(page);
        if (m <= 0) return;
        const float trackY0 = kTop + 10, trackY1 = H - 10, track = trackY1 - trackY0;
        const float len = std::max(56.0f, track * viewH() / s.content);
        const float t = std::clamp(s.pos / m, 0.0f, 1.0f);
        // past the ends (rubber band) the thumb shrinks
        const float over = s.pos < 0 ? -s.pos : s.pos > m ? s.pos - m : 0.0f;
        const float l = std::max(24.0f, len - over * 0.5f);
        const float y0 = trackY0 + (track - l) * t;
        const double since = now - s.active;
        const bool hot = since < 0.8 || dragging;
        const float a = hot ? 0.55f : since < 1.2 ? 0.55f - static_cast<float>(since - 0.8) / 0.4f * 0.3f : 0.25f;
        const float x1 = W - 10, x0 = x1 - (hot ? 8 : 6);
        cv.rect(x0, trackY0, x1, trackY1, 4, 0xffffff, 0.05f);
        cv.rect(x0, y0, x1, y0 + l, 4, 0xffffff, a);
    }
    void drawToast() {
        if (toast.empty()) return;
        const float a = static_cast<float>(std::clamp(std::min((toastUntil - now) / 0.25, 1.0), 0.0, 1.0));
        const float tw = body.width(toast) + 56, y1 = H - 24, y0 = y1 - 52;
        cv.rect((W - tw) * 0.5f, y0, (W + tw) * 0.5f, y1, kR, toastError ? kError : 0x2c2e33, 0.97f * a);
        body.drawMid(cv, (W - tw) * 0.5f + 28, (y0 + y1) * 0.5f, toast, kText, 1e9f, a);
    }

    // ── Actions ──
    void loadFonts() {
        const char *bold = "/usr/share/fonts/noto/NotoSans-Bold.ttf", *semi = "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                   *medium = "/usr/share/fonts/noto/NotoSans-Medium.ttf", *regular = "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                   *dejaB = "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", *deja = "/usr/share/fonts/TTF/DejaVuSans.ttf";
        title.load({bold, dejaB, deja}, 44);
        head.load({bold, semi, dejaB, deja}, 30);
        body.load({semi, medium, bold, deja}, 26);
        label.load({semi, medium, bold, deja}, 22);
        small.load({regular, deja}, 20);
        tiny.load({bold, semi, dejaB, deja}, 17);
        glyphFont.load({bold, dejaB, deja}, 96);
    }
    // Saves the settings like Frame Home expects (whole file, renamed into place: it applies them live).
    void saveSettings(const PerfSettings& before) {
        bool ok;
        {
            std::ofstream out(rootDir() / "settings.json.part");
            out << perf.toJson();
            out.close();
            ok = !out.fail();
        }
        std::error_code ec;
        if (ok) fs::rename(rootDir() / "settings.json.part", rootDir() / "settings.json", ec);
        if (!ok || ec) { perf = before; say("Couldn't save (is ~/frame-home writable?)", true, 3); }
        else say(perf.needsRestart(before) ? "Saved · Frame Home restarts with it" : "Saved");
    }
    // debug.json, the same way (Frame Home applies it within a second).
    bool saveDebug(const DebugSettings& before, const char* done) {
        bool ok;
        {
            std::ofstream out(rootDir() / "debug.json.part");
            out << dbg.toJson();
            out.close();
            ok = !out.fail();
        }
        std::error_code ec;
        if (ok) fs::rename(rootDir() / "debug.json.part", rootDir() / "debug.json", ec);
        if (!ok || ec) { dbg = before; say("Couldn't save (is ~/frame-home writable?)", true, 3); return false; }
        say(done);
        return true;
    }
    // Deletes card i (after "Delete?" was answered), then shows what's left.
    void removeEnv(size_t i) {
        const Env e = envs[i];
        const DeleteResult r = deleteEnv(e, envs, true);
        if (!r.ok) { say("Couldn't delete " + e.name + (r.error.empty() ? "" : " (" + r.error + ")"), true, 4); return; }
        envs = scanEnvs();
        current = currentKey();
        hoverT.assign(envs.size(), 0.0f);
        if (switchingKey == e.key) switchingKey.clear();
        sc[0].content = envContent();
        sc[0].target = std::clamp(sc[0].target, 0.0f, maxScroll(0));
        hotHover = -1;
        say("Deleted " + e.name + (r.currentChanged && !r.newCurrent.empty() ? "  ·  now showing " + r.newCurrent : ""), false, 3);
    }
    void click(int id) {
        if (!confirmKey.empty()) {   // asking "Delete?": its buttons answer; a click on the card itself does nothing
            const int c = cardOf(id);
            const bool same = c >= 0 && static_cast<size_t>(c) < envs.size() && envs[c].key == confirmKey;
            if (same && id >= kDelId && id < kDelId + kIdSpan) { removeEnv(static_cast<size_t>(c)); return; }
            confirmKey.clear();
            if (same) return;        // Cancel, or the card
        }
        if (id == kHomeId) {
            if (home != Home::Restarting) restartAsked = true;
            return;
        }
        if (id >= 9000 && id < 9003) {
            if (page != id - 9000) { page = id - 9000; hotHover = -1; sc[page].active = now - 10; if (page == 2) info = readDebugInfo(); }
            return;
        }
        if (id >= kTrashId && id < kTrashId + kIdSpan) {
            const size_t i = static_cast<size_t>(id - kTrashId);
            if (page != 0 || i >= envs.size()) return;
            confirmKey = envs[i].key;
            confirmStart = now;
            return;
        }
        if (id >= kCardId && id < kCardId + kIdSpan) {
            const size_t i = static_cast<size_t>(id - kCardId);
            if (page != 0 || i >= envs.size()) return;
            const Env& e = envs[i];
            if (!switchingKey.empty() && switchingKey == e.key) return;
            if (e.key == current && switchingKey.empty()) { say("Already showing " + e.name); return; }
            if (!setCurrent(e)) { say("Couldn't switch (is ~/frame-home writable?)", true, 3); return; }
            current = currentKey();
            // Frame Home fades out, loads and fades in: a few seconds.
            switchingKey = e.key;
            switchStart = now;
            switchUntil = now + 3.2;
            return;
        }
        if (id == kShotId) {
            const DebugSettings before = dbg;
            dbg.shot = static_cast<int>(std::time(nullptr));   // a request newer than Frame Home's start
            saveDebug(before, "Saving a picture to ~/frame-home/logs");
            return;
        }
        if (id == kDebugOffId) {
            const DebugSettings before = dbg;
            dbg = DebugSettings();
            dbg.shot = before.shot;
            if (!(dbg == before)) saveDebug(before, "Debug views off");
            return;
        }
        const int row = id / 100;
        const bool debugRow = row >= 15 && row <= 19;
        if (page == 1 && id >= 0 && row < kRowCount && !debugRow) {
            const PerfSettings before = perf;
            choose(id / 100, id % 100);
            if (!(perf == before)) saveSettings(before);
        }
        if (page == 2 && id >= 0 && debugRow) {
            const DebugSettings before = dbg;
            choose(id / 100, id % 100);
            if (!(dbg == before)) saveDebug(before, "Applied");
        }
    }
};

// What Frame Home reports in stats.json (GPU time against the frame budget, the resolution it settled on), as a
// line for the Settings page; empty while it hasn't reported.
std::string statsLine() {
    try {
        const tinyjson::Value st = tinyjson::parse(readText(rootDir() / "stats.json"));
        if (!st.isObject() || !st.has("gpuMs")) return {};
        char line[240];
        const float gpu = st["gpuMs"].asFloat(), budget = st["budgetMs"].asFloat();
        // Over the frame's time, frames come late and the home stutters: say so, and what helps.
        std::snprintf(line, sizeof line, "%s%.1f of %.1f ms a frame, %d x %d per eye (%.0f%%), eye tracking %s", gpu > budget * 1.03f ? "Too slow, lower Resolution: " : "",
                      gpu, budget, static_cast<int>(st["width"].asFloat()), static_cast<int>(st["height"].asFloat()), st["scale"].asFloat() * 100,
                      !st["foveation"].boolVal ? "not used" : st["eyeTracking"].boolVal ? "on" : "waiting for your eyes");
        return line;
    } catch (...) {
        return {};
    }
}

volatile sig_atomic_t g_quit = 0;

// Bundles (.fbundle, fenv_bundle.h) in envs/ become the environments they carry: each is unpacked beside them as
// its own .fenv (an environment already there is kept: the same one is skipped, a different one of the same name
// makes this one "<name> 2.fenv"), then the bundle is deleted, as everything it holds is in envs/ now. One that
// can't be unpacked (damaged, or still being copied in) stays and is tried again once its size or time changes.
void unpackBundles() {
    static std::set<std::string> failed;                // "<file>|<size>|<time>" of bundles that didn't unpack
    std::error_code ec;
    const fs::path envs = homeos::envs();
    for (const auto& e : fs::directory_iterator(envs, ec)) {
        if (!e.is_regular_file(ec) || !isFenvBundlePath(e.path().string())) continue;
        const std::string state = e.path().filename().string() + "|" + std::to_string(e.file_size(ec)) + "|" +
                                  std::to_string(static_cast<long long>(e.last_write_time(ec).time_since_epoch().count()));
        if (failed.count(state)) continue;
        std::vector<std::string> unpacked;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!fenvBundleExtract(e.path().string(), envs.string(), unpacked, err)) {
            LOG("can't unpack the bundle %s: %s", e.path().filename().string().c_str(), err.c_str());
            failed.insert(state);
            continue;
        }
        fs::remove(e.path(), ec);
        LOG("unpacked the bundle %s into %zu environments (%.1f s)", e.path().filename().string().c_str(), unpacked.size(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
}

// New environments are unpacked into the .fenv cache in the background as soon as they appear (a big one takes
// ~10 s on the Frame), so switching to one is as quick as switching back to it. Bundles are opened up first.
void prepareEnvironments() {
    static std::atomic<bool> busy{false}, again{false};
    again = true;                                       // files added while a run is going get a run of their own
    if (busy.exchange(true)) return;
    std::thread([] {
        homeos::lowerPriority(15);
        for (;;) {
        while (again.exchange(false)) try {
        unpackBundles();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(homeos::envs(), ec)) {
            if (e.path().extension() != ".fenv") continue;
            FenvManifest m;
            std::vector<std::string> sources, models;
            std::string edits, err;
            const auto t0 = std::chrono::steady_clock::now();
            if (!fenvExtract(e.path().string(), fenvCacheDir(), m, sources, models, edits, err)) { LOG("can't unpack %s: %s", e.path().filename().string().c_str(), err.c_str()); continue; }
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (sec > 0.5) LOG("unpacked %s ahead of time (%.1f s)", e.path().filename().string().c_str(), sec);
        }
        } catch (const std::exception& ex) { LOG("unpacking ahead of time stopped: %s", ex.what()); }   // a damaged file must not kill the panel
        busy = false;
        if (!again || busy.exchange(true)) break;   // asked again just as this run ended: go round once more
        }
    }).detach();
}

int runPanel() {
    signal(SIGINT, [](int) { g_quit = 1; });
    signal(SIGTERM, [](int) { g_quit = 1; });
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Overlay);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 1; }
    vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid, icon = vr::k_ulOverlayHandleInvalid;
    vr::EVROverlayError oe = vr::VROverlay()->CreateDashboardOverlay(kAppKey, "Environments", &ov, &icon);
    if (oe != vr::VROverlayError_None) { LOG("CreateDashboardOverlay: %s", vr::VROverlay()->GetOverlayErrorNameFromEnum(oe)); vr::VR_Shutdown(); return 1; }
    vr::VROverlay()->SetOverlayWidthInMeters(ov, 2.4f);
    vr::VROverlay()->SetOverlayInputMethod(ov, vr::VROverlayInputMethod_Mouse);
    const vr::HmdVector2_t mouseScale = {{static_cast<float>(W), static_cast<float>(H)}};
    vr::VROverlay()->SetOverlayMouseScale(ov, &mouseScale);
    {   // dashboard tab icon: a small orange tile
        std::vector<uint8_t> ic(64 * 64 * 4);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
            uint8_t* p = &ic[(y * 64 + x) * 4];
            const bool on = (x > 10 && x < 54 && y > 14 && y < 50) && !(x > 16 && x < 48 && y > 20 && y < 44 && (y - 20) * 32 > (x - 16) * 24);
            p[0] = on ? 240 : 0; p[1] = on ? 148 : 0; p[2] = on ? 58 : 0; p[3] = on ? 255 : 0;
        }
        vr::VROverlay()->SetOverlayRaw(icon, ic.data(), 64, 64, 4);
    }
    Panel panel;
    panel.loadFonts();
    std::string lastScan;
    auto rescan = [&]() {
        std::error_code ec;
        std::string sig;
        for (const auto& e : fs::directory_iterator(homeos::envs(), ec)) sig += e.path().filename().string() + std::to_string(static_cast<long long>(fs::last_write_time(e.path(), ec).time_since_epoch().count()));
        for (const auto& e : fs::directory_iterator(homeos::thumbs(), ec)) sig += e.path().filename().string() + std::to_string(static_cast<long long>(fs::last_write_time(e.path(), ec).time_since_epoch().count()));
        sig += readText(rootDir() / "pairs.txt") + "|" + currentKey();
        if (sig == lastScan) return false;
        lastScan = sig;
        prepareEnvironments();
        panel.envs = scanEnvs();
        panel.current = currentKey();
        return true;
    };
    rescan();
    panel.perf.fromJson(readText(rootDir() / "settings.json"));
    panel.dbg.fromJson(readText(rootDir() / "debug.json"));
    // With the Steam menu open its controllers are the menu's. Earlier versions took the sticks through SteamVR's
    // overlay input override, which stopped the pointer clicking the dock: turn it back off.
    {
        vr::EVRSettingsError se = vr::VRSettingsError_None;
        if (vr::VRSettings()->GetBool(vr::k_pch_SteamVR_Section, "globalActionSetPriority", &se)) {
            vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "globalActionSetPriority", false, &se);
            LOG("turned SteamVR's overlay input override (globalActionSetPriority) back off");
        }
    }
    // Two ways Frame Home takes SteamVR's environment:
    //  - the system environment (the Steam Frame, and a Linux PC with the compositor layer installed: ~/frame-home/
    //    layer): the layer (layer/frame_home_layer.cpp) draws it in Aurora's place and frame-home --system renders it.
    //    This panel starts with SteamVR, so it keeps that process running and makes sure SteamVR's home app is off.
    //  - SteamVR's home app (a Windows PC, whose compositor isn't Vulkan): SteamVR runs Frame Home itself whenever no
    //    game is open. Here the panel makes sure that's on, and that the moments before Frame Home draws (SteamVR
    //    starting, a game quitting) show the home too: SteamVR's background is a 360-degree picture of it
    //    (keepBackground), with the classic environment instead of Aurora.
#ifdef _WIN32
    const bool systemEnv = false;
#else
    const bool systemEnv = fs::exists(rootDir() / "layer");
#endif
    // (Frame Home uninstalled from a PC: its manifest is gone, and SteamVR's settings are left as the uninstall put them)
    const bool homeInstalled = systemEnv || fs::exists(rootDir() / "frame-home.vrmanifest");
    if (!systemEnv && homeInstalled) {
        vr::EVRSettingsError se = vr::VRSettingsError_None;
        if (!vr::VRSettings()->GetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", &se)) {
            vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", true, &se);
            LOG("Frame Home is SteamVR's home app: turned it on");
        }
        if (vr::VRSettings()->GetInt32(vr::k_pch_SteamVR_Section, "environmentMode", &se) != 0)
            vr::VRSettings()->SetInt32(vr::k_pch_SteamVR_Section, "environmentMode", 0, &se);   // the background, not Aurora
    } else {
        vr::EVRSettingsError se = vr::VRSettingsError_None;
        if (vr::VRSettings()->GetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", &se)) {
            vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "enableHomeApp", false, &se);
            LOG("Frame Home is the system environment now: SteamVR's home app turned off");
        }
        if (vr::VRSettings()->GetInt32(vr::k_pch_SteamVR_Section, "environmentMode", &se) != 1) {
            vr::VRSettings()->SetInt32(vr::k_pch_SteamVR_Section, "environmentMode", 1, &se);   // Aurora's pipelines are what the layer replaces
        }
    }
    if (systemEnv) {   // its own SteamVR identity (hidden: no list, no dock), so its controller bindings are its own
        const fs::path m = rootDir() / "frame-environment.vrmanifest";
        std::ofstream(m) << "{\n  \"source\": \"builtin\",\n  \"applications\": [ {\n    \"app_key\": \"framehome.environment\",\n"
                            "    \"launch_type\": \"binary\",\n    \"binary_path_linux\": \"" << (rootDir() / "bin" / "frame-home").string() << "\",\n"
                            "    \"binary_path_linux_arm\": \"" << (rootDir() / "bin" / "frame-home").string() << "\",\n    \"arguments\": \"--system\",\n"
                            "    \"is_dashboard_overlay\": true,\n    \"is_hidden\": true,\n"
                            "    \"strings\": { \"en_us\": { \"name\": \"Frame Home\", \"description\": \"The environment\" } }\n  } ]\n}\n";
        const vr::EVRApplicationError ae = vr::VRApplications()->AddApplicationManifest(fs::absolute(m).u8string().c_str(), false);
        if (ae != vr::VRApplicationError_None) LOG("registering the environment: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(ae));
    }
    homeos::Pid systemPid = 0;
    bool adopted = false;                            // systemPid isn't our child (a panel before this one started it)
    auto lastSpawn = std::chrono::steady_clock::now() - std::chrono::minutes(1);
    auto keepSystem = [&](std::chrono::steady_clock::time_point now) {
        if (!systemEnv) return;                       // SteamVR runs Frame Home itself (its home app)
#ifndef _WIN32
        if (systemPid > 0 && adopted) {
            if (kill(systemPid, 0) == 0) return;
            LOG("frame-home --system (pid %d) stopped: restarting it", static_cast<int>(systemPid));
            systemPid = -1;
            adopted = false;
        } else if (systemPid > 0) {
            int st = 0;
            if (waitpid(systemPid, &st, WNOHANG) != systemPid) return;
            LOG("frame-home --system stopped (%s %d): restarting it", WIFSIGNALED(st) ? "signal" : "exit", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
            systemPid = -1;
        }
        std::error_code ec;
        if (fs::exists(rootDir() / "layer" / "off", ec)) return;               // the layer is off: Aurora
        if (now - lastSpawn < std::chrono::seconds(5)) return;                // a crash loop doesn't spin
        lastSpawn = now;
        // Still running from a panel before this one (the panel restarted): keep it, never a second renderer.
        if (const std::vector<pid_t> running = homeos::runningWith("frame-home", "--system"); !running.empty()) {
            systemPid = running.front();
            adopted = true;
            LOG("frame-home --system already running (pid %d): keeping it", static_cast<int>(systemPid));
            return;
        }
        const std::string bin = (rootDir() / "bin" / "frame-home").string();
        const pid_t pid = fork();
        if (pid == 0) {
            setsid();
            setenv("STEAMVR_APPKEY", "framehome.environment", 1);   // its own app to SteamVR, not ours (our input is legacy)
            execl(bin.c_str(), bin.c_str(), "--system", static_cast<char*>(nullptr));
            _exit(127);
        }
        if (pid > 0) { systemPid = pid; LOG("started frame-home --system (pid %d)", static_cast<int>(pid)); }
#else
        (void)now;
#endif
    };
    // The panel's pixels go to SteamVR in Vulkan images of our own (overlay_texture.h): SetOverlayRaw recreated the
    // texture on every redraw and the panel flickered. SetOverlayRaw stays as the fallback.
    OverlayTexture tex;
    bool useTex = false;
    {
        std::string err;
        useTex = tex.init(W, H, err);
        if (useTex) LOG("panel texture: Vulkan, %d images (%s)", OverlayTexture::kSlots, OverlayTexture::format() == VK_FORMAT_R8G8B8A8_SRGB ? "sRGB" : "UNORM");
        else LOG("panel texture: Vulkan unavailable (%s), using SetOverlayRaw", err.c_str());
    }
    // Thumbstick / trackpad scrolling while pointing at the panel. Smooth events are preferred; discrete ones only
    // count while no smooth ones arrive (SteamVR may send both).
    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    auto envFloat = [](const char* name, float def) { const char* v = std::getenv(name); return v && *v ? static_cast<float>(std::atof(v)) : def; };
    // Phone-like speed: smooth deltas x420 (was 120) with a generous per-event cap; a discrete notch is one card row.
    const float kSmoothPx = envFloat("FH_SCROLL_SMOOTH", 200.0f), kSmoothCap = envFloat("FH_SCROLL_CAP", 300.0f),
                kDiscretePx = envFloat("FH_SCROLL_DISCRETE", static_cast<float>(Panel::kCardH + Panel::kGap) * 0.5f);
    panel.kFlingGain = envFloat("FH_SCROLL_FLING_GAIN", panel.kFlingGain);
    panel.kFlingMax = envFloat("FH_SCROLL_FLING_MAX", panel.kFlingMax);
    panel.kFlingDecay = std::max(0.3f, envFloat("FH_SCROLL_FLING_DECAY", panel.kFlingDecay));
    LOG("scrolling: smooth x%.0f (cap %.0f px), discrete %.0f px a notch, fling gain %.2f max %.0f decay %.2f", kSmoothPx, kSmoothCap, kDiscretePx,
        panel.kFlingGain, panel.kFlingMax, panel.kFlingDecay);
    // The header's button: Restart stops frame-home --system (keepSystem starts a new one as soon as it's gone);
    // Start / Turn On clears the layer's off switch (the layer checks it every second) and starts it now.
    homeos::Pid restartingPid = 0;
    std::chrono::steady_clock::time_point restartAt{}, killAt{};
    bool restarting = false;
    auto homeRunning = [&] { return systemEnv ? systemPid > 0 && homeos::alive(systemPid) : !homeos::runningCopies("frame-home").empty(); };
    auto homeTick = [&](std::chrono::steady_clock::time_point now) {
        std::error_code ec;
        if (panel.restartAsked) {
            panel.restartAsked = false;
            restarting = true;
            restartAt = now;
            restartingPid = systemPid;
            if (!systemEnv) {   // SteamVR's home app: end it and have SteamVR start it again
                LOG("restarting Frame Home (SteamVR's home app) from the panel");
                homeos::stopCopies("frame-home");
                const vr::EVRApplicationError le = vr::VRApplications()->LaunchApplication("openvr.tool.steamvr_environments");
                if (le != vr::VRApplicationError_None) LOG("start: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(le));
                lastSpawn = now;
            } else {
                if (fs::exists(rootDir() / "layer" / "off", ec)) {
                    fs::remove(rootDir() / "layer" / "off", ec);
                    fs::remove(rootDir() / "layer" / "crashes", ec);
                    LOG("Frame Home turned back on from the panel");
                }
                if (systemPid > 0 && homeos::alive(systemPid)) {
                    LOG("restarting frame-home --system (pid %d) from the panel", static_cast<int>(systemPid));
                    homeos::stop(systemPid, false);
                    killAt = now + std::chrono::seconds(4);
                }
                lastSpawn = now - std::chrono::minutes(1);   // no 5 s wait: a new one as soon as the old one is gone
                keepSystem(now);
            }
        }
        if (systemEnv && restarting && restartingPid > 0 && systemPid == restartingPid && now > killAt && homeos::alive(restartingPid)) {
            LOG("frame-home --system (pid %d) didn't stop: killing it", static_cast<int>(restartingPid));
            homeos::stop(restartingPid, true);
            killAt = now + std::chrono::seconds(10);
        }
        // Restarting until a new process has been up a moment (it loads the home and fades in), 20 s at most.
        if (restarting && (now - restartAt > std::chrono::seconds(20) ||
                           (systemEnv ? systemPid > 0 && systemPid != restartingPid && now - lastSpawn > std::chrono::seconds(2)
                                      : homeRunning() && now - lastSpawn > std::chrono::seconds(2))))
            restarting = false;
        const bool alive = homeRunning();
        const Panel::Home h = restarting ? Panel::Home::Restarting : systemEnv && fs::exists(rootDir() / "layer" / "off", ec) ? Panel::Home::TurnedOff
                            : alive ? Panel::Home::Running : Panel::Home::Stopped;
        if (h != panel.home) { panel.home = h; return true; }
        return false;
    };
    // SteamVR's background, when Frame Home is its home app (a Windows PC): a 360-degree picture of the current home
    // from its spawn, so the moments before Frame Home draws (SteamVR starting, a game quitting) show the home and
    // not Aurora. Made by frame-home --panorama off the frame loop, at low priority, whenever the home changes; set
    // here once it's written (a new file each time: SteamVR reloads a background when its path changes).
    std::string backgroundFor;                       // the home key + file times the background shows
    std::thread backgroundJob;
    std::atomic<bool> backgroundDone{false};
    std::string backgroundPng;                       // (the job's result, read once backgroundDone)
    auto keepBackground = [&] {
        if (systemEnv || !homeInstalled) return;
        if (backgroundDone.exchange(false)) {
            if (backgroundJob.joinable()) backgroundJob.join();
            float eye = 1.6f, dome = 0;
            const std::string j = readText(backgroundPng + ".json");
            try { const tinyjson::Value d = tinyjson::parse(j); eye = d["eyeHeight"].asFloat(); dome = d["domeRadius"].asFloat(); } catch (...) {}
            vr::EVRSettingsError se = vr::VRSettingsError_None;
            vr::VRSettings()->SetString(vr::k_pch_SteamVR_Section, "background", fs::u8path(backgroundPng).u8string().c_str(), &se);
            vr::VRSettings()->SetBool(vr::k_pch_SteamVR_Section, "backgroundUseDomeProjection", true, &se);
            vr::VRSettings()->SetFloat(vr::k_pch_SteamVR_Section, "backgroundCameraHeight", eye, &se);
            vr::VRSettings()->SetFloat(vr::k_pch_SteamVR_Section, "backgroundDomeRadius", dome, &se);
            LOG("SteamVR's background: %s (eyes %.2f m, dome %.1f m)%s", fs::u8path(backgroundPng).filename().string().c_str(), eye, dome,
                se == vr::VRSettingsError_None ? "" : " (a setting was refused)");
            std::error_code ec;   // the earlier pictures go
            std::vector<fs::path> old;
            for (const auto& e : fs::directory_iterator(rootDir() / "background", ec))
                if (e.path() != fs::path(backgroundPng) && e.path() != fs::path(backgroundPng + ".json")) old.push_back(e.path());
            for (const fs::path& o : old) fs::remove(o, ec);
        }
        if (backgroundJob.joinable()) return;        // one at a time
        const std::vector<Env> envs = scanEnvs();
        if (envs.empty()) return;
        const std::string cur = currentKey();
        const Env* e = &envs.front();
        for (const Env& x : envs) if (x.key == cur) e = &x;
        std::string want = e->key;
        std::error_code ec;
        for (const auto& f : e->files) want += std::to_string(static_cast<long long>(fs::last_write_time(homeos::envs() / f, ec).time_since_epoch().count()));
        if (want == backgroundFor) return;
        backgroundFor = want;
        fs::create_directories(rootDir() / "background", ec);
        static int n = 0;
        backgroundPng = (rootDir() / "background" / ("home-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(++n) + ".png")).string();
        std::vector<std::string> args = {(rootDir() / "bin" / homeos::exeName("frame-home")).string(), "--panorama", backgroundPng, "--size", "4096", "--spawn"};
        for (const auto& f : e->files) args.push_back((homeos::envs() / f).string());
        LOG("making SteamVR's background from %s", e->name.c_str());
        backgroundJob = std::thread([&, args] {
            const int rc = homeos::run(args, 15, true);
            if (rc != 0) LOG("the background picture failed (%d)", rc);
            else backgroundDone = true;
        });
    };
    // Thumbnails for environments that have none of their own (frame-switcher --thumbs: one at a time, low priority,
    // only while nobody is looking at Frame Home), once a run.
    {
        bool missing = false;
        for (const Env& e : scanEnvs()) missing |= e.thumb.rgba.empty();
        if (missing) homeos::run({homeos::selfPath().string(), "--thumbs"}, 19, false);
    }
    double lastSmooth = -10;
    int scrollLogs = 0;
    bool dirty = true, wasVisible = false;
    auto lastPoll = std::chrono::steady_clock::now(), lastHomeCheck = lastPoll;
    double lastTick = Panel::clockNow(), lastUpload = 0;
    while (!g_quit) {
        if (const auto t = std::chrono::steady_clock::now(); t - lastHomeCheck > std::chrono::milliseconds(500) || panel.restartAsked) {
            lastHomeCheck = t;
            if (!panel.restartAsked) keepSystem(t);
            dirty |= homeTick(t);
            keepBackground();
        }
        panel.now = Panel::clockNow();
        vr::VREvent_t ev;
        while (vr::VROverlay()->PollNextOverlayEvent(ov, &ev, sizeof ev)) {
            const float mx = ev.data.mouse.x, my = H - ev.data.mouse.y;   // GL space: bottom left is 0,0
            switch (ev.eventType) {
                case vr::VREvent_MouseMove: dirty |= panel.onMove(mx, my); break;
                case vr::VREvent_MouseButtonDown:
                    if (ev.data.mouse.button == vr::VRMouseButton_Left) dirty |= panel.onDown(mx, my);
                    break;
                case vr::VREvent_MouseButtonUp:
                    if (ev.data.mouse.button == vr::VRMouseButton_Left) {
                        if (const int id = panel.onUp(mx, my); id >= 0) panel.click(id);
                        dirty = true;
                    }
                    break;
                case vr::VREvent_FocusLeave: panel.onLeave(); dirty = true; break;
                case vr::VREvent_ScrollSmooth:
                case vr::VREvent_ScrollDiscrete: {
                    const bool smooth = ev.eventType == vr::VREvent_ScrollSmooth;
                    if (smooth) lastSmooth = panel.now;
                    else if (panel.now - lastSmooth < 0.5) break;
                    if (scrollLogs < 12) { ++scrollLogs; LOG("scroll %s: dy %.4f (viewport %.3f)", smooth ? "smooth" : "discrete", ev.data.scroll.ydelta, ev.data.scroll.viewportscale); }
                    // ydelta > 0 scrolls up (toward the top of the list)
                    const float px = smooth ? std::clamp(-ev.data.scroll.ydelta * kSmoothPx, -kSmoothCap, kSmoothCap) : -ev.data.scroll.ydelta * kDiscretePx;
                    panel.onScroll(px);
                    dirty = true;
                    break;
                }
                case vr::VREvent_Quit: g_quit = 1; break;
                default: break;
            }
        }
        // Process-wide events (SteamVR asking us to quit).
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) if (ev.eventType == vr::VREvent_Quit) g_quit = 1;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPoll > std::chrono::seconds(2)) {
            lastPoll = now;
            if (rescan()) { dirty = true; panel.sc[0].content = panel.envContent(); }
            PerfSettings ps;
            if (ps.fromJson(readText(rootDir() / "settings.json")) && !(ps == panel.perf)) { panel.perf = ps; if (panel.page == 1) dirty = true; }
            const std::string line = statsLine();
            if (panel.stats != line) { panel.stats = line; if (panel.page == 1) dirty = true; }
            DebugSettings ds;   // (Frame Studio may change it too; Frame Home clears it when it starts)
            const std::string dj = readText(rootDir() / "debug.json");
            if (dj.empty()) ds = DebugSettings(), ds.shot = panel.dbg.shot;
            if ((dj.empty() || ds.fromJson(dj)) && !(ds == panel.dbg)) { panel.dbg = ds; if (panel.page == 2) dirty = true; }
            if (panel.page == 2) {
                DebugInfo di = readDebugInfo();
                if (!(di == panel.info)) { panel.info = std::move(di); dirty = true; }
            }
        }
        const bool visible = vr::VROverlay()->IsDashboardVisible();
        if (visible && !wasVisible) dirty = true;   // the texture is kept, but show what changed while hidden
        wasVisible = visible;
        panel.now = Panel::clockNow();
        const bool moving = panel.tick(panel.now - lastTick);
        lastTick = panel.now;
        // Redraw only when something visible changed; up to 90 times a second, so scrolling keeps up with the hand.
        if (visible && (dirty || moving) && panel.now - lastUpload >= 1.0 / 90) {
            lastUpload = panel.now;
            panel.render();
            std::string err;
            if (useTex && !tex.upload(ov, panel.cv.px.data(), err)) {
                LOG("panel texture: %s; using SetOverlayRaw from now on", err.c_str());
                useTex = false;
            }
            if (!useTex) vr::VROverlay()->SetOverlayRaw(ov, panel.cv.px.data(), W, H, 4);
            dirty = panel.needRedraw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(!visible ? 16 : moving || dirty ? 2 : 8));
    }
    if (systemEnv && systemPid > 0) homeos::stop(systemPid, false);   // SteamVR is going: so is the environment
    if (backgroundJob.joinable()) backgroundJob.join();
    vr::VR_Shutdown();
    tex.destroy();   // after VR_Shutdown: vrclient holds the images until then
    return 0;
}

int runInstall() {
    std::string self = homeos::selfPath().string();
    if (self.empty()) return 1;
    std::error_code ec;   // the installed copy is what SteamVR runs (see frame-home --install)
    const fs::path installed = rootDir() / "bin" / homeos::exeName("frame-switcher");
    if (fs::exists(installed, ec)) self = installed.string();
    for (size_t at = 0; (at = self.find('\\', at)) != std::string::npos; at += 2) self.insert(at, 1, '\\');   // (JSON)
    const fs::path manifest = rootDir() / "frame-switcher.vrmanifest";
    std::ofstream(manifest) << "{\n  \"source\": \"builtin\",\n  \"applications\": [ {\n    \"app_key\": \"" << kAppKey
                            << "\",\n    \"launch_type\": \"binary\",\n    \"binary_path_linux\": \"" << self
                            << "\",\n    \"binary_path_windows\": \"" << self
                            << "\",\n    \"binary_path_linux_arm\": \"" << self
                            << "\",\n    \"arguments\": \"\",\n    \"is_dashboard_overlay\": true,\n"
                               "    \"strings\": { \"en_us\": { \"name\": \"Environments\",\n"
                               "      \"description\": \"Switch SteamVR's environment (Frame Home)\" } }\n  } ]\n}\n";
    if (!homeos::steamVrRunning()) { LOG("SteamVR isn't running: the installer registers the panel in its config files"); return 3; }
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Utility);
    // 3: SteamVR not running — the installer registers the manifest in SteamVR's config files instead.
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s (SteamVR not running)", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 3; }
    vr::EVRApplicationError ae = vr::VRApplications()->AddApplicationManifest(fs::absolute(manifest).u8string().c_str(), false);
    LOG("AddApplicationManifest: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(ae));
    vr::EVRApplicationError al = vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, true);
    LOG("auto-launch with SteamVR: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(al));
    // A running (older) panel is replaced by this binary.
    homeos::stopCopies("frame-switcher");
    const vr::EVRApplicationError le = vr::VRApplications()->LaunchApplication(kAppKey);
    LOG("start: %s", vr::VRApplications()->GetApplicationsErrorNameFromEnum(le));
    vr::VR_Shutdown();
    return ae == vr::VRApplicationError_None ? 0 : 1;
}

// The panel off SteamVR: no longer started with it, its manifest gone, a running one closed.
int runUninstall() {
    const fs::path manifest = rootDir() / "frame-switcher.vrmanifest";
    if (!homeos::steamVrRunning()) return 3;          // (connecting would start it: the uninstaller edits its files)
    vr::EVRInitError ierr = vr::VRInitError_None;
    vr::VR_Init(&ierr, vr::VRApplication_Utility);
    if (ierr != vr::VRInitError_None) { LOG("VR_Init: %s (SteamVR not running)", vr::VR_GetVRInitErrorAsEnglishDescription(ierr)); return 3; }
    vr::VRApplications()->SetApplicationAutoLaunch(kAppKey, false);
    std::error_code ec;
    if (fs::exists(manifest, ec)) vr::VRApplications()->RemoveApplicationManifest(fs::absolute(manifest).u8string().c_str());
    fs::remove(manifest, ec);
    homeos::stopCopies("frame-switcher");
    vr::VR_Shutdown();
    LOG("the Environments panel is off SteamVR");
    return 0;
}

// Is somebody looking at Frame Home? It draws at the display's rate then, and a frame a second when nobody is (the
// headset is off, or a game runs). A stats.json older than 10 s: Frame Home isn't running.
bool frameHomeShowing() {
    std::error_code ec;
    const fs::path statsPath = rootDir() / "stats.json";
    const auto written = fs::last_write_time(statsPath, ec);
    if (ec || fs::file_time_type::clock::now() - written > std::chrono::seconds(10)) return false;
    try {
        return tinyjson::parse(readText(statsPath))["fps"].asFloat() > 5;
    } catch (...) {
        return false;
    }
}

// Renders a thumbnail for every environment that has none (Frame Home's headless snapshot, low priority). Each one
// loads and draws a whole home on the GPU, which nice can't hold back: they're made only while nobody is looking at
// Frame Home, so they never stutter the home you're in (29 at once took 7 minutes of stutter, 2026-10-21).
int runThumbs() {
    std::error_code ec;
    fs::create_directories(homeos::thumbs(), ec);
    const std::string bin = (rootDir() / "bin" / homeos::exeName("frame-home")).string();
    int made = 0;
    for (const Env& e : scanEnvs()) {
        if (!e.thumb.rgba.empty()) continue;
        bool waited = false;
        while (frameHomeShowing()) {
            if (!waited) LOG("thumbnails wait while Frame Home is being looked at");
            waited = true;
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        const fs::path out = thumbPath(e.key);
        // From where Frame Home starts you: the first spawn, eyes 1.6 m up, facing its way, 5 degrees down.
        std::vector<std::string> args = {bin, "--snapshot", out.string(), "--size", "384", "--spawn", "--cam", "0,1.6,0,0,-5"};
        for (const auto& f : e.files) args.push_back((homeos::envs() / f).string());
        LOG("thumbnail: %s", e.name.c_str());
        if (homeos::run(args, 19, true) == 0) ++made;
    }
    LOG("%d thumbnails made", made);
    return 0;
}

// Self-check of deleteEnv in a made-up ~/frame-home under `dir` (HOME and the .fenv cache are pointed there).
int runTestDelete(const fs::path& dirIn) {
    std::error_code ec;
    if (fs::exists(dirIn, ec)) { std::fprintf(stderr, "test-delete: %s exists; give a new folder\n", dirIn.c_str()); return 2; }
    fs::create_directories(dirIn, ec);
    const fs::path dir = fs::canonical(dirIn, ec);
    homeos::setEnv("HOME", dir.string().c_str());
    homeos::setEnv("FRAME_FENV_CACHE", (dir / "cache").string().c_str());
    const fs::path root = rootDir(), envs = root / "envs", th = envs / ".thumbs", cache = dir / "cache";
    if (root != dir / "frame-home") return 2;
    fs::create_directories(th, ec);
    fs::create_directories(cache, ec);
    auto put = [](const fs::path& p, const std::string& s = "x") { std::ofstream(p, std::ios::binary) << s; };
    for (const char* f : {"alpha.apk", "foot.apk", "vista.apk", "beta.apk", "gamma.fenv", "gammax.fenv"}) put(envs / f);
    put(envs / "alpha.apk.world.json"); put(envs / "alpha.apk.edits.json"); put(envs / "foot.apk.world.json");
    put(th / "alpha.apk+.ppm"); put(th / "foot.apk+vista.apk+.ppm"); put(th / "foot.apk+.ppm"); put(th / "beta.apk+.ppm");
    put(root / "pairs.txt", "foot.apk vista.apk\nbeta.apk missing.apk\n");
    put(root / "current", "alpha.apk\n");
    put(root / "keep.txt"); put(dir / "outside.apk");
    const std::string gkey = cacheEntriesOf(envs / "gamma.fenv").key, xkey = cacheEntriesOf(envs / "gammax.fenv").key;
    fs::create_directories(cache / (gkey + "-123-456") / "sub", ec); put(cache / (gkey + "-123-456") / "sub" / "scene.bin");
    fs::create_directories(cache / (gkey + "-1--4645286059281504627"), ec); put(cache / (gkey + ".lock"), "");
    fs::create_directories(cache / (xkey + "-9-9"), ec); fs::create_directories(cache / (gkey + "-junk"), ec);
    fs::create_directories(cache / "gamma-deadbeef-1-2", ec);
    int fails = 0, checks = 0;
    auto check = [&](bool ok, const std::string& what) { ++checks; if (!ok) ++fails; std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str()); };
    auto mk = [](std::vector<std::string> files, const std::string& name) { Env e; e.files = std::move(files); e.name = name; for (auto& f : e.files) e.key += f + "\n"; return e; };
    auto has = [&](const fs::path& p) { return fs::exists(fs::symlink_status(p, ec)); };

    // Refusals: nothing outside envs/, no path parts, environment files only, must exist.
    for (const std::string bad : {"../outside.apk", "../keep.txt", "..", "a/b.apk", "/etc/passwd", "keep.txt", ".thumbs", "alpha.apk.world.json", "nope.apk", ""}) {
        const DeleteResult r = deleteEnv(mk({bad}, bad), {}, false);
        check(!r.ok, "refuses \"" + bad + "\" (" + r.error + ")");
    }
    check(has(dir / "outside.apk") && has(root / "keep.txt") && has(envs / "alpha.apk.world.json"), "refusals left everything alone");
    std::vector<Env> all = scanEnvs();
    check(all.size() == 5, "scan: 1 pair + 4 APKs (" + std::to_string(all.size()) + ")");   // .fenv fakes don't parse: made by hand below

    // The current one: current moves to another first.
    {
        const Env* a = nullptr;
        for (const Env& e : all) if (e.key == "alpha.apk\n") a = &e;
        check(a != nullptr, "found alpha");
        if (a) {
            const DeleteResult r = deleteEnv(*a, all, false);
            check(r.ok, "deletes alpha " + r.error);
            check(r.currentChanged && !currentKey().empty() && currentKey().find("alpha") == std::string::npos, "current moved off alpha to \"" + r.newCurrent + "\" (" + currentKey().substr(0, currentKey().size() - 1) + ")");
            check(!has(envs / "alpha.apk") && !has(envs / "alpha.apk.world.json") && !has(envs / "alpha.apk.edits.json") && !has(th / "alpha.apk+.ppm"), "alpha's file, world, edits and thumbnail gone");
            check(!has(root / "current.part"), "no current.part left over");
        }
    }
    // A footprint + vista pair: both files, the pairs.txt line, the thumbnails.
    {
        all = scanEnvs();
        const Env* p = nullptr;
        for (const Env& e : all) if (e.files.size() == 2) p = &e;
        check(p != nullptr, "found the pair card");
        if (p) {
            put(root / "current", p->key);
            const DeleteResult r = deleteEnv(*p, all, false);
            check(r.ok, "deletes the pair " + r.error);
            check(!has(envs / "foot.apk") && !has(envs / "vista.apk") && !has(envs / "foot.apk.world.json"), "both files of the pair (and the world) gone");
            check(!has(th / "foot.apk+vista.apk+.ppm") && !has(th / "foot.apk+.ppm"), "the pair's thumbnails gone");
            const std::string pairs = readText(root / "pairs.txt");
            check(pairs == "beta.apk missing.apk\n", "pairs.txt keeps the other line only: \"" + pairs + "\"");
            check(currentKey() != p->key && !currentKey().empty(), "current moved off the pair");
        }
    }
    // A single file named in a pairs.txt line: the line goes too.
    {
        const DeleteResult r = deleteEnv(mk({"beta.apk"}, "Beta"), scanEnvs(), false);
        check(r.ok && !has(envs / "beta.apk") && !has(th / "beta.apk+.ppm"), "deletes beta and its thumbnail");
        check(readText(root / "pairs.txt").empty(), "the pair line naming beta went");
    }
    // A .fenv: its cache folders (only its own) go.
    {
        put(root / "current", "gamma.fenv\n");
        const DeleteResult r = deleteEnv(mk({"gamma.fenv"}, "Gamma"), {mk({"gamma.fenv"}, "Gamma"), mk({"gammax.fenv"}, "Gammax")}, false);
        check(r.ok && !has(envs / "gamma.fenv"), "deletes gamma.fenv");
        check(!has(cache / (gkey + "-123-456")) && !has(cache / (gkey + "-1--4645286059281504627")) && !has(cache / (gkey + ".lock")), "its cache folders and lock gone");
        check(has(cache / (xkey + "-9-9")) && has(cache / (gkey + "-junk")) && has(cache / "gamma-deadbeef-1-2"), "other cache folders kept");
        check(currentKey() == "gammax.fenv\n", "current moved to gammax");
    }
    // The last one: current is emptied.
    {
        const DeleteResult r = deleteEnv(mk({"gammax.fenv"}, "Gammax"), {mk({"gammax.fenv"}, "Gammax")}, false);
        check(r.ok && r.currentChanged && r.newCurrent.empty() && currentKey().empty() && has(root / "current"), "the last one: current emptied");
    }
    check(has(dir / "outside.apk") && has(root / "keep.txt") && has(root / "pairs.txt") && has(th), "nothing outside envs/ touched");
    std::printf("%d of %d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string a = argc > 1 ? argv[1] : "";
    if (a == "--install") return runInstall();
    if (a == "--thumbs") return runThumbs();
    if (a == "--uninstall") return runUninstall();
    if (a == "--unpack-bundles") { unpackBundles(); return 0; }
    if (a == "--test-delete" && argc > 2) return runTestDelete(argv[2]);
    if (a == "--render" && argc > 2) {   // the panel as it looks now, to a PPM (no SteamVR needed)
        // frame-switcher --render out.ppm [hover] [page] [scroll px] [switching card]
        //   (FH_RENDER_CONFIRM=n: card n asks "Delete?"; FH_RENDER_TOAST=text: a toast)
        Panel panel;
        panel.loadFonts();
        panel.envs = scanEnvs();
        panel.current = currentKey();
        panel.now = Panel::clockNow();
        const int hover = argc > 3 ? std::atoi(argv[3]) : -1;
        if (argc > 4) panel.page = std::clamp(std::atoi(argv[4]), 0, 2);
        panel.perf.fromJson(readText(rootDir() / "settings.json"));
        panel.dbg.fromJson(readText(rootDir() / "debug.json"));
        panel.info = readDebugInfo();
        panel.stats = statsLine();
        panel.hoverT.assign(panel.envs.size(), 0.0f);
        // page 0: hover is a card number, or a raw id (>= 10000: e.g. 30000 + n is card n's delete button)
        const int hoverId = panel.page == 0 && hover >= 0 ? (hover >= Panel::kCardId ? hover : Panel::kCardId + hover) : -1;
        if (const int c = Panel::cardOf(hoverId); c >= 0 && c < static_cast<int>(panel.envs.size())) { panel.hotHover = hoverId; panel.hoverT[c] = 1; }
        if (const char* c = std::getenv("FH_RENDER_CONFIRM"); c && *c && std::atoi(c) >= 0 && std::atoi(c) < static_cast<int>(panel.envs.size())) {
            panel.confirmKey = panel.envs[std::atoi(c)].key;   // card c asking "Delete?"
            panel.confirmStart = panel.now - 1;
        }
        if (const char* t = std::getenv("FH_RENDER_TOAST"); t && *t) panel.say(t);
        if (const char* h = std::getenv("FH_RENDER_HOME"); h && *h) panel.home = static_cast<Panel::Home>(std::clamp(std::atoi(h), 0, 3));
        if (argc > 6 && std::atoi(argv[6]) >= 0 && std::atoi(argv[6]) < static_cast<int>(panel.envs.size())) {
            panel.switchingKey = panel.current = panel.envs[std::atoi(argv[6])].key;
            panel.switchStart = panel.now - 0.3;
        }
        panel.render();                       // once to lay out (content height), then as it shows
        if (argc > 5) {
            Panel::Scroll& s = panel.sc[panel.page];
            s.pos = s.target = std::clamp(static_cast<float>(std::atof(argv[5])), 0.0f, panel.maxScroll(panel.page));
            s.active = panel.now;
        }
        if (const char* n = std::getenv("FH_RENDER_BENCH")) {   // time the drawing alone
            const auto t0 = std::chrono::steady_clock::now();
            const int reps = std::max(1, std::atoi(n));
            for (int i = 0; i < reps; ++i) panel.render();
            std::fprintf(stderr, "render: %.2f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps);
        }
        if (panel.page != 0 && hover >= 0) panel.hotHover = hover;   // settings: hover an option id (row * 100 + option)
        else if (panel.page == 0 && hover >= 0) panel.hotHover = hoverId;
        panel.render();                       // (draws with that hover; no pointer, so it's cleared after)
        FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (size_t q = 0; q + 3 < panel.cv.px.size(); q += 4) std::fwrite(&panel.cv.px[q], 1, 3, f);
        std::fclose(f);
        return 0;
    }
    if (a == "--list" || a == "--set" || a == "--delete") {
        const std::vector<Env> envs = scanEnvs();
        const std::string cur = currentKey();
        if (a == "--list") {
            for (size_t i = 0; i < envs.size(); ++i)
                std::printf("%c %2zu  %-40s %-28s %s\n", envs[i].key == cur ? '*' : ' ', i + 1, envs[i].name.c_str(), envs[i].kind.c_str(),
                            [&] { std::string f; for (const auto& x : envs[i].files) f += (f.empty() ? "" : " + ") + x; return f; }().c_str());
            return 0;
        }
        if (argc < 3) { std::fprintf(stderr, "usage: frame-switcher %s NAME|NUMBER\n", a.c_str()); return 2; }
        const std::string want = argv[2];
        for (size_t i = 0; i < envs.size(); ++i) {
            std::string files;
            for (const auto& x : envs[i].files) files += (files.empty() ? "" : "+") + x;
            if (want == std::to_string(i + 1) || want == envs[i].files.front() || want == files || want == envs[i].name) {
                if (a == "--set") return setCurrent(envs[i]) ? 0 : 1;
                const DeleteResult r = deleteEnv(envs[i], envs, false);
                if (!r.ok) { std::fprintf(stderr, "frame-switcher: %s\n", r.error.c_str()); return 1; }
                for (const auto& x : r.removed) std::printf("removed %s\n", x.c_str());
                if (r.currentChanged) std::printf("now showing: %s\n", r.newCurrent.empty() ? "(nothing left)" : r.newCurrent.c_str());
                return 0;
            }
        }
        std::fprintf(stderr, "frame-switcher: no environment \"%s\" (see --list)\n", want.c_str());
        return 1;
    }
    return runPanel();
}
