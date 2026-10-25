#pragma once
// Per-home edits made in Frame Studio (moved / deleted objects), saved as "<home>.apk.edits.json"
// next to the home on the Frame. The home itself stays Meta's untouched APK.
//
//   { "home": "theatre.apk",
//     "objects": [ { "index": 12, "name": "merged_sofa", "deleted": false,
//                    "pivot": [16 floats], "world": [16 floats] } ] }
//
// `index` is the mesh's position in the loaded scene; `name` guards against a different home version.
// `world` is the mesh's final world matrix; `pivot` is the editor's gizmo frame (only Studio uses it).
//
// A background sound added in Frame Studio, and how loud the home's own ambience plays (0: off):
//   "sound": { "file": "haven.apk.sound-1a2b3c4d.mp3", "name": "rain.mp3", "volume": 0.8, "homeVolume": 1 }
// `file` sits next to this edits file (on the Frame beside the home; in a .fenv beside its edits.json): Ogg Vorbis,
// Ogg Opus, WAV, MP3 or FLAC, looped.
//
// What the environment is called and its picture, as you set them in Frame Studio (the headset's Environments panel
// shows them; a .fenv carries them as its name and thumbnail):
//   "title": "Rainy Haven", "image": "haven.apk.image-1a2b3c4d.jpg"   (the image beside this file, like the sound)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct ObjectEdit {
    int index = -1;
    std::string name;
    bool deleted = false;
    float pivot[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

struct HomeSound {
    std::string file;                   // "" = no sound of its own
    std::string name;                   // what the file was called when it was added (shown in Studio)
    float volume = 0.8f;                // 0..1
    float homeVolume = 1;               // the home's own ambience, 0..1
    bool any() const { return !file.empty() || homeVolume != 1; }
};

struct HomeEdits {
    std::string home;
    std::vector<ObjectEdit> objects;
    HomeSound sound;
    std::string title;                  // the environment's name ("" = the home's own)
    std::string image;                  // its picture, a file beside this one ("" = a view of it)
    bool empty() const { return objects.empty() && !sound.any() && title.empty() && image.empty(); }
};

// Sound files a background sound can be.
inline bool isSoundFile(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* x : {".ogg", ".opus", ".oga", ".wav", ".mp3", ".flac"})
        if (name.size() > std::strlen(x) && name.compare(name.size() - std::strlen(x), std::strlen(x), x) == 0) return true;
    return false;
}

// The sound file of edits read from `editsPath` (it sits beside it); "" when there's none.
inline std::string soundPathFor(const std::string& editsPath, const HomeSound& s) {
    if (s.file.empty() || s.file.find_first_of("/\\") != std::string::npos || s.file == ".." || s.file == ".") return {};
    const size_t slash = editsPath.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : editsPath.substr(0, slash + 1)) + s.file;
}

namespace home_edits_detail {

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

// Minimal reader for the subset this file uses: objects, arrays, strings, numbers, booleans.
struct Reader {
    const char* p;
    const char* end;
    void ws() { while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p; }
    bool eat(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }
    bool str(std::string& out) {
        ws();
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                if (*p == 'u' && p + 4 < end) { out += static_cast<char>(std::strtol(std::string(p + 1, 4).c_str(), nullptr, 16)); p += 5; continue; }
                out += *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
                ++p;
            } else {
                out += *p++;
            }
        }
        return eat('"');
    }
    bool num(double& v) {
        ws();
        char* e = nullptr;
        v = std::strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }
    bool boolean(bool& b) {
        ws();
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { b = true; p += 4; return true; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { b = false; p += 5; return true; }
        return false;
    }
    bool floats(float* out, int n) {
        if (!eat('[')) return false;
        for (int i = 0; i < n; ++i) {
            double v;
            if (!num(v)) return false;
            out[i] = static_cast<float>(v);
            if (i + 1 < n && !eat(',')) return false;
        }
        return eat(']');
    }
    // Skips any value (for keys this version does not know). Depth-capped: a hostile file of nested '['
    // would otherwise recurse until the stack overflows.
    bool skip(int depth = 0) {
        ws();
        if (p >= end || depth > 256) return false;
        if (*p == '"') { std::string s; return str(s); }
        if (*p == '{' || *p == '[') {
            const char close = *p == '{' ? '}' : ']';
            ++p;
            if (eat(close)) return true;
            do {
                if (close == '}') { std::string k; if (!str(k) || !eat(':')) return false; }
                if (!skip(depth + 1)) return false;
            } while (eat(','));
            return eat(close);
        }
        bool b; double d;
        return boolean(b) || num(d) || (end - p >= 4 && std::strncmp(p, "null", 4) == 0 && (p += 4, true));
    }
};

}  // namespace home_edits_detail

inline std::string editsPathFor(const std::string& homePath) { return homePath + ".edits.json"; }
// Frame Studio's world layer for a home (spawns, seats, walls, walkable areas: core/world.h), next to it.
inline std::string worldPathFor(const std::string& homePath) { return homePath + ".world.json"; }

inline bool saveHomeEdits(const std::string& path, const HomeEdits& e) {
    using home_edits_detail::quote;
    std::ostringstream o;
    o.precision(9);
    auto mat = [&](const float* m) {
        o << "[";
        for (int i = 0; i < 16; ++i) o << (i ? ", " : "") << m[i];
        o << "]";
    };
    o << "{\n  \"home\": " << quote(e.home) << ",\n  \"objects\": [";
    for (size_t i = 0; i < e.objects.size(); ++i) {
        const ObjectEdit& x = e.objects[i];
        o << (i ? ",\n" : "\n") << "    { \"index\": " << x.index << ", \"name\": " << quote(x.name)
          << ", \"deleted\": " << (x.deleted ? "true" : "false") << ",\n      \"pivot\": ";
        mat(x.pivot);
        o << ",\n      \"world\": ";
        mat(x.world);
        o << " }";
    }
    o << "\n  ]";
    if (e.sound.any())
        o << ",\n  \"sound\": { \"file\": " << quote(e.sound.file) << ", \"name\": " << quote(e.sound.name) << ", \"volume\": " << e.sound.volume << ", \"homeVolume\": " << e.sound.homeVolume << " }";
    if (!e.title.empty()) o << ",\n  \"title\": " << quote(e.title);
    if (!e.image.empty()) o << ",\n  \"image\": " << quote(e.image);
    o << "\n}\n";
    // By its UTF-8 name (a narrow path is the ANSI code page on Windows: C:\Users\José\... wasn't found), written
    // beside it and renamed over it once whole: a crash or a full disk midway never leaves a cut-off edits file.
    namespace fs = std::filesystem;
    const fs::path target = fs::u8path(path), part = fs::u8path(path + ".part");
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << o.str();
        f.close();
        if (!f) { std::error_code ec; fs::remove(part, ec); return false; }
    }
    std::error_code ec;
    fs::rename(part, target, ec);
    if (ec) { fs::remove(part, ec); return false; }
    return true;
}

inline bool loadHomeEdits(const std::string& path, HomeEdits& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    home_edits_detail::Reader r{text.data(), text.data() + text.size()};
    out = HomeEdits{};
    if (!r.eat('{')) return false;
    if (r.eat('}')) return true;
    do {
        std::string key;
        if (!r.str(key) || !r.eat(':')) return false;
        if (key == "home") {
            if (!r.str(out.home)) return false;
        } else if (key == "title") {
            if (!r.str(out.title)) return false;
        } else if (key == "image") {
            if (!r.str(out.image)) return false;
        } else if (key == "objects") {
            if (!r.eat('[')) return false;
            if (r.eat(']')) continue;
            do {
                ObjectEdit x;
                if (!r.eat('{')) return false;
                do {
                    std::string k;
                    if (!r.str(k) || !r.eat(':')) return false;
                    double d;
                    // (an index past int's range, or NaN, is no mesh: -1, never an out-of-range cast)
                    bool ok = k == "index" ? (r.num(d) && (x.index = d >= 0 && d < 2147483647.0 ? static_cast<int>(d) : -1, true))
                            : k == "name" ? r.str(x.name)
                            : k == "deleted" ? r.boolean(x.deleted)
                            : k == "pivot" ? r.floats(x.pivot, 16)
                            : k == "world" ? r.floats(x.world, 16)
                            : r.skip();
                    if (!ok) return false;
                } while (r.eat(','));
                if (!r.eat('}')) return false;
                out.objects.push_back(x);
            } while (r.eat(','));
            if (!r.eat(']')) return false;
        } else if (key == "sound") {
            if (!r.eat('{')) return false;
            if (r.eat('}')) continue;
            do {
                std::string k;
                if (!r.str(k) || !r.eat(':')) return false;
                double d = 0;
                const bool ok = k == "file" ? r.str(out.sound.file)
                              : k == "name" ? r.str(out.sound.name)
                              // (strtod reads "nan", which clamp lets through to the audio gain: a NaN keeps the default)
                              : k == "volume" ? (r.num(d) && (std::isnan(d) || (out.sound.volume = static_cast<float>(std::clamp(d, 0.0, 1.0)), true)))
                              : k == "homeVolume" ? (r.num(d) && (std::isnan(d) || (out.sound.homeVolume = static_cast<float>(std::clamp(d, 0.0, 1.0)), true)))
                              : r.skip();
                if (!ok) return false;
            } while (r.eat(','));
            if (!r.eat('}')) return false;
        } else if (!r.skip()) {
            return false;
        }
    } while (r.eat(','));
    return r.eat('}');
}
