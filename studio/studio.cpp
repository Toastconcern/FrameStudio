// Frame Studio — the desktop side of Frame Home (Windows, Linux, macOS).
// Opens a Meta home, renders it with the home's own shaders (the same renderer the Frame runs),
// lets you move / rotate / scale / hide / delete its objects, and sends it to the Frame over SSH.
//
//   Viewport   MMB orbit · Shift+MMB pan · Ctrl+MMB or wheel zoom · Alt+LMB orbit
//              RMB + WASD/QE fly (wheel = speed) · LMB select
//   Keys       G/R/S move/rotate/scale tool · X/Del delete · H hide · Alt+H unhide all
//              F frame selected · Home frame all · Numpad 0 / P player view
//              Ctrl+Z undo · Ctrl+Shift+Z / Ctrl+Y redo · Ctrl+O open · Ctrl+S save edits
#include "home_edits.h"
#include "perf_settings.h"
#include "hsl_renderer.h"
#include "home_report.h"
#include "core/audio.h"
#include "core/fsb5_vorbis.h"
#include "loaders/rendtxtr_parser.h"
#include "loaders/rendmesh_parser.h"
#include "hsl_scene.h"
#include "world_gen.h"
#include "surface_mesh.h"
#include "fenv.h"
#include "fenv_bundle.h"
#include "scene_bake.h"
#include "anim_bake.h"
#include "visibility_bake.h"
#include "model_import.h"
#include "world_runtime.h"
#include "frame_pacer.h"
#include "platform.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

#include <imgui.h>
#include <imgui_internal.h>              // the self-test reads ImGui's ID-conflict detector
#include <ImGuizmo.h>                   // needs imgui.h first
#include <backends/imgui_impl_vulkan.h>
#ifdef _WIN32
#include <backends/imgui_impl_win32.h>
#include <windows.h>                    // the window itself (the rest of the system is behind platform.h)
#include <dwmapi.h>
#include <shellapi.h>
#else
#ifndef GLFW_INCLUDE_NONE   // (CMake defines it too)
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <dlfcn.h>
#include <backends/imgui_impl_glfw.h>
#endif

#include <algorithm>
#include <cstdarg>
#include <deque>
#include <functional>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

namespace fs = std::filesystem;

std::atomic<bool> g_audioMuted{false};   // core/audio.h: the timeline's mute

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kNear = 0.05f;
constexpr float kFovY = 60.0f * kPi / 180.0f;
constexpr const char* kFrameHost = "frame";             // default headset: ~/.ssh/config alias (Install window edits it)
constexpr const char* kFrameEnvDir = "frame-home/envs";  // relative to the Frame user's home

// ── Strings / files ─────────────────────────────────────────────────────────────────────────────

using platform::widen;
using platform::narrow;

// Name a home gets on the Frame: lowercase, spaces -> '-', only [a-z0-9._-].
std::string frameName(const std::string& path) {
    std::string base = narrow(fs::path(widen(path)).filename().wstring()), out;
    bool dropped = false;
    for (char c : base) {
        if (static_cast<unsigned char>(c) & 0x80) { dropped = true; continue; }   // non-ASCII (UTF-8 bytes)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') out += c;
        else if (c == ' ' && !out.empty() && out.back() != '-') out += '-';
    }
    if (dropped) {   // "日本.fenv" and "中国.fenv" must not both become ".fenv": a short hash of the real name keeps them apart
        uint32_t h = 2166136261u;
        for (char c : base) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
        const size_t dot = out.rfind('.');
        std::string stem = dot == std::string::npos ? out : out.substr(0, dot), ext = dot == std::string::npos ? "" : out.substr(dot);
        while (!stem.empty() && (stem.back() == '-' || stem.back() == '.')) stem.pop_back();
        char tag[16];
        std::snprintf(tag, sizeof tag, "%06x", h & 0xffffff);
        out = (stem.empty() ? std::string("home") : stem) + "-" + tag + ext;
    }
    return out.empty() ? "home.apk" : out;
}

// A home is a footprint (the room) plus a vista (the world outside: its lighting, sky and view); the footprint loads
// first. Meta names them "Shell Env Footprint …" and "Shell Env Vista …".
bool nameHas(const std::string& p, const char* word) {
    std::string n = narrow(fs::path(widen(p)).filename().wstring());
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n.find(word) != std::string::npos;
}
bool isFootprintApk(const std::string& p) { return nameHas(p, "footprint"); }
bool isVistaApk(const std::string& p) { return nameHas(p, "vista") && !nameHas(p, "footprint"); }
std::vector<std::string> footprintFirst(std::vector<std::string> apks) {
    std::stable_partition(apks.begin(), apks.end(), isFootprintApk);
    return apks;
}
// The vistas beside a footprint (its folder), to pair it with.
std::vector<std::string> vistasBeside(const std::string& footprint) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(widen(footprint)).parent_path(), ec)) {
        const std::string f = narrow(e.path().wstring());
        if (e.is_regular_file(ec) && e.path().extension() == L".apk" && isVistaApk(f)) out.push_back(f);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// "Shell Env Footprint Haven2025 Quest3s-buck_206.0.0.0.70_974700778.apk" -> "Haven2025"
std::string prettyHome(const std::string& apk) {
    std::string n = narrow(fs::path(widen(apk)).stem().wstring());
    const size_t us = n.find('_');
    if (us != std::string::npos) n = n.substr(0, us);
    for (const char* pre : {"Shell Env Footprint ", "Shell Env Vista ", "Shell Env "})
        if (n.rfind(pre, 0) == 0) n = n.substr(std::strlen(pre));
    for (const char* suf : {" Quest3s-buck", " Quest3s", " Quest3", "-buck"}) {
        const size_t p = n.find(suf);
        if (p != std::string::npos) n = n.substr(0, p);
    }
    return n.empty() ? narrow(fs::path(widen(apk)).stem().wstring()) : n;
}

std::string displayName(const std::vector<std::string>& apks) {
    std::string n;
    for (const auto& p : apks) n += (n.empty() ? "" : " + ") + prettyHome(p);
    return n;
}

// The folder Frame Studio runs from (the Frame payload ships beside the exe, in frame/).
fs::path exeDir() { return platform::exeDir(); }

// Studio's own files (settings, edits, recent homes, auto-saves): %APPDATA%\FrameStudio, ~/.local/share/FrameStudio,
// ~/Library/Application Support/FrameStudio.
fs::path dataDir() {
    fs::path p = platform::userDataDir() / L"FrameStudio";
    std::error_code ec;
    fs::create_directories(p / L"edits", ec);
    return p;
}

bool g_selfTestRun = false;              // --selftest: saves go to a scratch library, never into My Environments

// My Environments: Documents/Frame Studio/Environments, where what you make is saved (File > Save). A self-test
// saves into <dataDir>/selftest/library instead (it saves its test home several times a run).
fs::path libraryDir() {
    const fs::path p = g_selfTestRun ? dataDir() / L"selftest" / L"library" : platform::documentsDir() / L"Frame Studio" / L"Environments";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// Runs a command line with no console window; stdout + stderr land in `out`. Returns the exit code.
// (Studio builds its command lines as wide strings: these pass them on as UTF-8.)
int run(const std::wstring& cmdline, const std::wstring& cwd, std::string& out) { return platform::run(narrow(cmdline), fs::path(cwd), out); }

// ssh / scp, quoted, with a trailing space, ready to prefix options.
std::wstring sshTool(const wchar_t* name) { return widen(platform::sshTool(narrow(name).c_str())); }

// For a command line: nothing in it a shell could read into.
std::string base64(const std::string& in) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) | (i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0);
        enc += b64[(n >> 18) & 63]; enc += b64[(n >> 12) & 63];
        enc += i + 1 < in.size() ? b64[(n >> 6) & 63] : '='; enc += i + 2 < in.size() ? b64[n & 63] : '=';
    }
    return enc;
}

std::string lastLine(const std::string& s) {
    size_t e = s.find_last_not_of("\r\n ");
    if (e == std::string::npos) return {};
    size_t b = s.find_last_of('\n', e);
    return s.substr(b == std::string::npos ? 0 : b + 1, e + 1 - (b == std::string::npos ? 0 : b + 1));
}

// ── Column-major 4x4 math ───────────────────────────────────────────────────────────────────────

void identity(float* m) { std::memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }

void mul(const float* a, const float* b, float* o) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    std::memcpy(o, r, 64);
}

void translation(float* m, float x, float y, float z) { identity(m); m[12] = x; m[13] = y; m[14] = z; }

bool invert(const float* m, float* out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return false;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] / det;
    return true;
}

void point(const float* m, const float* p, float* o) {
    float r[3];
    for (int k = 0; k < 3; ++k) r[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
    std::memcpy(o, r, 12);
}

void direction(const float* m, const float* d, float* o) {
    float r[3];
    for (int k = 0; k < 3; ++k) r[k] = m[k] * d[0] + m[4 + k] * d[1] + m[8 + k] * d[2];
    std::memcpy(o, r, 12);
}

// Reversed-Z, infinite-far Vulkan projection (y-down) from view-space tangents — what the renderer expects.
Mat4 rendererProjection(float tx, float ty) {
    Mat4 m = {};
    m.m[0] = 1.0f / tx;
    m.m[5] = -1.0f / ty;
    m.m[14] = kNear;
    m.m[11] = -1.0f;
    return m;
}

// OpenGL-style projection for ImGuizmo and screen-space overlays.
void glProjection(float tx, float ty, float* m) {
    const float f = 2000.0f;
    std::memset(m, 0, 64);
    m[0] = 1.0f / tx;
    m[5] = 1.0f / ty;
    m[10] = -(f + kNear) / (f - kNear);
    m[11] = -1.0f;
    m[14] = -2.0f * f * kNear / (f - kNear);
}

// ── Camera ──────────────────────────────────────────────────────────────────────────────────────

struct Camera {
    float pivot[3] = {0, 1.6f, -2.0f};
    float yaw = 0, pitch = 0, dist = 2.0f;

    void forward(float* f) const {
        f[0] = -std::sin(yaw) * std::cos(pitch);
        f[1] = std::sin(pitch);
        f[2] = -std::cos(yaw) * std::cos(pitch);
    }
    void eye(float* e) const {
        float f[3];
        forward(f);
        for (int k = 0; k < 3; ++k) e[k] = pivot[k] - f[k] * dist;
    }
    // camera-to-world (looks down -Z at yaw = pitch = 0)
    void pose(float* m) const {
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        m[0] = cy;       m[1] = 0;   m[2] = -sy;      m[3] = 0;
        m[4] = sy * sp;  m[5] = cp;  m[6] = cy * sp;  m[7] = 0;
        m[8] = sy * cp;  m[9] = -sp; m[10] = cy * cp; m[11] = 0;
        eye(&m[12]);
        m[15] = 1;
    }
    void playerView() {
        yaw = pitch = 0;
        dist = 2.0f;
        pivot[0] = 0; pivot[1] = 1.6f; pivot[2] = -dist;
    }
};

// ── Objects (one per uploaded mesh) ─────────────────────────────────────────────────────────────

bool byFileLayout = true;               // the Explorer's folders: the file's own layout (else Studio's groups by kind)

struct Object {
    size_t mesh = 0;                    // renderer mesh index
    int scene = -1;                     // HslScene::meshes index
    std::string name, surface;
    bool hidden = false, deleted = false, skybox = false;
    float pivot[16], pivot0[16];        // gizmo frame (now / as loaded); world = pivot * offset
    float offset[16];
    float lmin[3] = {0, 0, 0}, lmax[3] = {0, 0, 0};
    size_t triangles = 0;
    int source = 0;                     // HslScene::apkPaths index: 0 footprint, 1 vista
    int category = 0;                   // Category (outliner group)
    std::string label;                  // display name (name minus the cook's Root_Xform_/SM_/LBG_ prefixes)
    std::vector<std::string> folders;   // where it sits in its file (MeshData::path), outermost first
};

// Outliner groups, in display order.
enum Category { kArchitecture, kProps, kPlants, kLights, kTerrain, kSky, kCategoryCount };
const char* kCategoryNames[kCategoryCount] = {"Architecture", "Furniture & Props", "Plants", "Lights & Glow", "Terrain & Water", "Sky"};

bool anyOf(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words) if (s.find(w) != std::string::npos) return true;
    return false;
}

int categorize(const std::string& name, const std::string& surface, bool skybox) {
    if (skybox) return kSky;
    std::string n = name + " " + surface;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (anyOf(n, {"emiss", "light", "lamp", "glow", "candle", "sconce", "globe", "lantern", "neon"})) return kLights;
    if (anyOf(n, {"plant", "leaves", "leaf", "tree", "bush", "grass", "flower", "branch", "vege", "fern", "vine", "moss"})) return kPlants;
    if (anyOf(n, {"terrain", "ground", "rock", "boulder", "cliff", "mountain", "water", "lake", "river", "waterfall", "sand",
                  "island", "ocean", "sea", "fog", "cloud", "hill", "dirt", "gravel", "shore"})) return kTerrain;
    if (anyOf(n, {"wall", "floor", "ceiling", "cieling", "beam", "trim", "window", "door", "platform", "moat", "blocker", "blkt",
                  "stair", "column", "pillar", "roof", "slat", "panel", "arch", "frame_", "baseboard", "tray", "portal"})) return kArchitecture;
    return kProps;
}

std::string cleanName(std::string n) {
    // Clay (.opa) meshes are named "<export>.fbx.<material>[_pbr...]": keep the material part.
    for (const char* ext : {".fbx.", ".FBX.", ".gltf.", ".glb."}) {
        const size_t f = n.rfind(ext);
        if (f != std::string::npos && f + std::strlen(ext) < n.size()) { n = n.substr(f + std::strlen(ext)); break; }
    }
    for (const char* pre : {"M_", "m_", "MI_", "mi_"})
        if (n.rfind(pre, 0) == 0 && n.size() > std::strlen(pre)) { n = n.substr(std::strlen(pre)); break; }
    for (const char* suf : {".mat.asset", ".mat", "_pbr"})
        if (n.size() > std::strlen(suf) && n.compare(n.size() - std::strlen(suf), std::strlen(suf), suf) == 0)
            n.erase(n.size() - std::strlen(suf));
    for (bool again = true; again;) {
        again = false;
        for (const char* pre : {"Root_Xform_", "root_xform_", "SM_", "sm_", "LBG_", "lbg_", "Geo_", "geo_"})
            if (n.rfind(pre, 0) == 0 && n.size() > std::strlen(pre)) { n = n.substr(std::strlen(pre)); again = true; }
    }
    return n;
}

// The environment's background sound in Studio: its settings (HomeSound: also how loud the home's own ambience
// plays) and the chosen file's bytes, shared so undo steps hold them without copies.
struct SoundState {
    HomeSound s;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::string name;                   // the file it came from, as shown
};

struct UndoStep {
    size_t object;
    float pivot[16];
    bool hidden, deleted;
    int group = 0;                      // steps sharing a non-zero group undo / redo together (one click, one Ctrl+Z)
    std::shared_ptr<HomeWorld> world;   // a world-layer edit: the whole layer as it was (object fields unused)
    std::shared_ptr<SoundState> sound;  // a sound edit: the sound as it was (object fields unused)
};

// The world layer's kinds of things (Studio's world editor).
enum WorldKind { kSpawn, kSeat, kHotspot, kCollider, kWallSpot, kGravity, kWorldKinds };
const char* kWorldNames[kWorldKinds] = {"Spawns", "Seats", "Teleport Hotspots", "Walls & Surfaces", "Wall Spots", "Gravity Zones"};

// What the viewport can show, each with its own switch (View > Show, the explorer's World headers).
enum ShowCat { kShowSpawns, kShowSeats, kShowHotspots, kShowFloors, kShowWalls, kShowSolidMeshes, kShowTriggers, kShowWallSpots, kShowGravity, kShowWalkArea, kShowUnreached, kShowCats };
const char* kShowNames[kShowCats] = {"Spawn points", "Seats", "Teleport hotspots", "Drawn floors", "Surface shapes", "Surface & collider meshes", "Triggers", "Wall spots", "Gravity zones", "Walkable area", "Floors out of reach"};
const uint32_t kShowColors[kShowCats] = {0x3fd2ff, 0x5fe07a, 0xffd84a, 0x4cd964, 0xff7a2e, 0xffa033, 0x9a9a9a, 0x5b8def, 0xc07aff, 0x2ec756, 0x808080};

// A gravity zone's kind (WorldGravity::Kind), as Studio names and explains it.
const char* kGravityNames[3] = {"Direction", "Planet", "Walk on any surface"};
const char* kGravityHelp[3] = {
    "Inside the box, down is the box's -Y: rotate the zone to choose which way is down (a wall or the ceiling as the floor).",
    "Down points at the centre within the pull radius: walk all the way round it (made by older versions of Frame Studio).",
    "Inside the box, the surface under your feet is the floor: walk off the floor up a wall and across the ceiling."};

// A collider's role, as Studio names and explains it. There's one kind of surface: you walk on it where it's flat
// enough, and where it's too steep to walk up it's a wall (WorldCollider::Solid). A thing that isn't a floor (a vase,
// a sofa, a lamp) gets a Collider: it stops you, you never stand on it (Object). Or you walk through it (Trigger).
// (Older files' Floor and Wall roles are read as Solid: core/world.h.)
const char* kRoleNames[6] = {"Surface", "Surface", "Surface", "Walk through", "Collider", "Drawn floor"};
const char* kRoleHelp[6] = {
    "Walk on it where it's flat enough; where it's too steep to walk up, it's a wall.",
    "Walk on it where it's flat enough; where it's too steep to walk up, it's a wall.",
    "Walk on it where it's flat enough; where it's too steep to walk up, it's a wall.",
    "Doesn't block or carry you (Meta's trigger volumes).",
    "Stops you: you don't walk through it, and never stand on it.",
    "A floor drawn on a thing (Draw Floor): you can always stand on it, even on furniture."};
const uint32_t kRoleColors[6] = {0xffa033, 0x4cd964, 0xff7a2e, 0x9a9a9a, 0xb07aff, 0x4cd964};

// What an object is for the player: a Surface (one linked Solid collider of all its triangles: walkable where it's
// flat enough, a wall where it's too steep to walk up), walked through, or left to the walkable area (Auto).
enum SurfaceMode { kSurfNone, kSurfSolid, kSurfObject, kSurfTrigger, kSurfModes };
const char* kSurfNames[kSurfModes] = {"Auto", "Surface", "Collider", "Walk through"};
const char* kSurfHelp[kSurfModes] = {
    "Automatic: the walkable area decides from its shape (floors you can reach and stand under headroom; anything in the way blocks).",
    "Walk on it where it's flat enough; where it's too steep to walk up, it's a wall.",
    "A thing, not a floor: it stops you (you don't walk through it) and you never stand on it.",
    "Left out: you walk through it (leaves, curtains, light shafts). A trigger volume."};
constexpr float kFloorCos = 0.766f;        // cos 40 deg: steeper than this is wall, never floor
constexpr uint32_t kFloorTint = 0x4cd964, kWallTint = 0xff7a2e;
const char* kAutoPrefix = "Auto ";        // surfaces World > Auto-generate made (a re-run replaces them)

enum class Tool { Move, Rotate, Scale, DrawFloor };

// ── Theme ───────────────────────────────────────────────────────────────────────────────────────

ImVec4 hex(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// Frame Studio's palette, laid out the way Blender works: areas on a near-black gutter, each with a header strip,
// panels on lighter boxes. The values are a theme's (see ThemeDef / applyThemeDef below) — the themes and their
// colours come from MetaDB (toastconcern.github.io). Runtime, not constexpr, so a theme can be switched live;
// everything drawn reads these, so changing them re-colours the whole editor.
uint32_t kPanel = 0x1c1c1c, kList = 0x191919, kListAlt = 0x1e1e1e, kBar = 0x151515, kWidget = 0x2b2b2b;
uint32_t kWidgetHover = 0x373737, kWidgetActive = 0x3d3d3d;
uint32_t kBlue = 0x2f6fe0, kBlueHover = 0x4886ef, kBlueActive = 0x265cbb, kSelect = 0x293a5a;
uint32_t kOrange = 0xffa033, kText = 0xe6e6e6, kDim = 0x9a9a9a, kHeading = 0xc3c3c3;
uint32_t kLine = 0x2f2f2f, kSplit = 0x0e0e0e;                       // hairlines between things, and the gutter between areas
uint32_t kHeader = 0x1a1a1a, kPanelBox = 0x242424;                   // an area's header strip; a Properties panel
uint32_t kViewBg = 0x2c2c2c;                                         // the 3D viewport behind the home
uint32_t kToolBg = 0x191919, kToolHover = 0x202020;                 // the viewport's toolbar and navigation buttons
uint32_t kDanger = 0x4a2420, kDangerHover = 0xd4463a;               // Delete, Remove, Cancel the load
uint32_t kGo = 0x245c33, kGoHover = 0x74c98c;                       // Draw Floor, the chosen walk preset
uint32_t kPopup = 0x181818, kFieldActive = 0x151515;                // a dropdown's background; a field being typed in
uint32_t kScroll = 0x4a4a4a, kScrollHover = 0x5c5c5c, kScrollActive = 0x6d6d6d, kRowAlt = 0x242424;

ImU32 col(uint32_t rgb, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(hex(rgb, a)); }

// Blend two packed RGB colours (t: 0 = a, 1 = b). Used to derive the editor's many greys from a theme's few tokens.
uint32_t mix(uint32_t a, uint32_t b, float t) {
    auto ch = [&](int sh) { const int x = (a >> sh) & 0xFF, y = (b >> sh) & 0xFF; return static_cast<uint32_t>(std::lround(x + (y - x) * t)) & 0xFF; };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// A theme, as MetaDB (toastconcern.github.io) defines it: a handful of tokens the whole editor is derived from.
struct ThemeDef {
    const char* name;
    uint32_t bg, panel, field, border, line, hover, text, muted, link, blue, blueHi, green, red, warn, warnHi;
};
// The MetaDB themes (css/style.css :root[data-theme]). Dark first (the default).
const ThemeDef kThemes[] = {
    {"Dark",        0x151515, 0x1c1c1c, 0x1c1c1c, 0x3d3d3d, 0x2f2f2f, 0x202020, 0xe6e6e6, 0x9a9a9a, 0x7fb3f2, 0x2f6fe0, 0x4886ef, 0x74c98c, 0xef8078, 0xc2352a, 0xd4463a},
    {"Midnight",    0x0e1626, 0x152034, 0x152034, 0x3d5177, 0x243350, 0x1b2840, 0xb8c2d4, 0x8794ab, 0x8fbcf5, 0x2f6fe0, 0x4886ef, 0x7fd396, 0xf08b83, 0xc2352a, 0xd4463a},
    {"Forest",      0x102a1c, 0x17351f, 0x17351f, 0x4f8a63, 0x2e5138, 0x1e4128, 0xdcefdd, 0x9dc0a4, 0x93dfa9, 0x2e7d4a, 0x25683c, 0x8ade9f, 0xf79184, 0xb3382a, 0x962c20},
    {"Forest Deep", 0x0e1611, 0x151f18, 0x151f18, 0x4d7a5c, 0x2b3a31, 0x1b271e, 0xd7e3d8, 0x94a998, 0x8ccfa2, 0x2c7548, 0x23603a, 0x85d398, 0xef8a80, 0xb3382a, 0x962c20},
    {"Ember",       0x2e1f16, 0x3a2820, 0x3a2820, 0x96795e, 0x573c2a, 0x452f24, 0xf4e2cf, 0xc0a48b, 0xf2ad60, 0xa35516, 0x8a4611, 0x93d193, 0xf79184, 0xb3382a, 0x962c20},
    {"Terminal",    0x030a05, 0x0a1a0e, 0x0a1a0e, 0x3f7a4c, 0x17361f, 0x102616, 0x6fe887, 0x4fb567, 0x8dfba6, 0x186b2f, 0x125223, 0x79e089, 0xff8f80, 0xa8291d, 0x8a2018},
    {"Contrast",    0x000000, 0x121212, 0x121212, 0x8a8a8a, 0x606060, 0x1e1e1e, 0xffffff, 0xd4d4d4, 0x9ecbff, 0x1d5fd6, 0x174fb4, 0x8ae8a3, 0xff9b93, 0xc2352a, 0xa02219},
    {"Paper",       0xf2e7d3, 0xfdf6e8, 0xfdf6e8, 0x8a785f, 0xcdb99a, 0xe6d7ba, 0x2b2620, 0x665b4c, 0x1f5591, 0x2f5d96, 0x24507f, 0x2b6136, 0xa3291a, 0xa3291a, 0x872015},
    {"Light",       0xffffff, 0xfafafa, 0xffffff, 0xc4c4c4, 0xe0e0e0, 0xf6f6f6, 0x1a1a1a, 0x6b6b6b, 0x1a4f9c, 0x1a5fd0, 0x144ba8, 0x1f6b34, 0xa32218, 0xa32218, 0x8a1d14},
};
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));
int g_theme = 0;   // the chosen theme (index into kThemes), saved in studio-settings.txt

// Derives the whole palette above from a theme's tokens, so a few colours colour the whole editor.
void applyThemeColors(const ThemeDef& t) {
    kBar = t.bg;
    kSplit = mix(t.bg, 0x000000, 0.18f);
    kPanel = t.panel;
    kList = mix(t.bg, t.panel, 0.55f);
    kListAlt = mix(t.panel, t.hover, 0.6f);
    kHeader = mix(t.bg, t.panel, 0.5f);
    kPanelBox = mix(t.panel, t.hover, 0.7f);
    kWidget = mix(t.panel, t.border, 0.30f);
    kWidgetHover = mix(t.panel, t.border, 0.50f);
    kWidgetActive = mix(t.panel, t.border, 0.68f);
    kViewBg = mix(t.bg, t.border, 0.45f);
    kToolBg = mix(t.bg, t.panel, 0.7f);
    kToolHover = t.hover;
    kLine = t.line;
    kText = t.text;
    kHeading = mix(t.muted, t.text, 0.55f);
    kDim = t.muted;
    kBlue = t.blue;
    kBlueHover = t.blueHi;
    kBlueActive = mix(t.blue, t.bg, 0.30f);
    kSelect = mix(t.panel, t.blue, 0.40f);
    kDanger = mix(t.panel, t.warn, 0.55f);
    kDangerHover = t.warnHi;
    kGo = mix(t.green, t.bg, 0.35f);
    kGoHover = t.green;
    kPopup = mix(t.bg, t.panel, 0.7f);
    kFieldActive = mix(t.bg, t.panel, 0.35f);
    kScroll = mix(t.panel, t.muted, 0.35f);
    kScrollHover = mix(t.panel, t.muted, 0.52f);
    kScrollActive = mix(t.panel, t.muted, 0.68f);
    kRowAlt = mix(t.panel, t.text, 0.05f);
    kOrange = 0xffa033;   // Frame Studio's mark and the selected-object outline stay orange (Blender's convention)
}

#include "inter_regular.h"           // Inter (SIL OFL 1.1: third_party/inter/LICENSE.txt), Blender's interface font
#include "inter_semibold.h"

// Fonts beside the body text (main loads them; null when the Windows font is missing, then the body font is used).
ImFont* g_fontBold = nullptr;           // section headers, the brand
ImFont* g_fontTitle = nullptr;          // the start screen's title
ImFont* fontOr(ImFont* f) { return f ? f : ImGui::GetFont(); }

// A primary (accent) button: the one thing a panel or screen is for. Pop with ImGui::PopStyleColor(3).
void pushPrimary() {
    ImGui::PushStyleColor(ImGuiCol_Button, hex(kBlue));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(kBlueHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(kBlueActive));
}
// A button that throws something away. Pop with ImGui::PopStyleColor(3).
void pushDanger() {
    ImGui::PushStyleColor(ImGuiCol_Button, hex(kDanger));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(kDangerHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(kDanger));
}

void applyTheme(float scale) {
    applyThemeColors(kThemes[std::clamp(g_theme, 0, kThemeCount - 1)]);   // the chosen theme's colours into the palette
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6;               // areas and floating windows: Blender's rounded corners
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 5;
    s.GrabRounding = 3;
    s.TabRounding = 4;
    s.ScrollbarRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.DockingSeparatorSize = 3;         // the gutter between areas
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.FramePadding = ImVec2(7, 3);      // a widget 20 px high: Blender's unit
    s.ItemSpacing = ImVec2(4, 3);
    s.ItemInnerSpacing = ImVec2(4, 4);
    s.WindowPadding = ImVec2(10, 8);
    s.CellPadding = ImVec2(5, 2);
    s.ScrollbarSize = 8;
    s.GrabMinSize = 8;
    s.IndentSpacing = 14;
    s.TabBarBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(0, 4);
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.DisabledAlpha = 0.45f;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = hex(kText);
    c[ImGuiCol_TextDisabled] = hex(kDim);
    c[ImGuiCol_WindowBg] = hex(kPanel);
    c[ImGuiCol_ChildBg] = hex(0, 0);
    c[ImGuiCol_PopupBg] = hex(kPopup, 0.98f);
    c[ImGuiCol_Border] = hex(kLine);
    c[ImGuiCol_BorderShadow] = hex(0, 0);
    c[ImGuiCol_FrameBg] = hex(kWidget);
    c[ImGuiCol_FrameBgHovered] = hex(kWidgetHover);
    c[ImGuiCol_FrameBgActive] = hex(kFieldActive);      // a field being typed in: dark
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = hex(kHeader);
    c[ImGuiCol_MenuBarBg] = hex(kBar);
    c[ImGuiCol_ScrollbarBg] = hex(0, 0);
    c[ImGuiCol_ScrollbarGrab] = hex(kScroll);
    c[ImGuiCol_ScrollbarGrabHovered] = hex(kScrollHover);
    c[ImGuiCol_ScrollbarGrabActive] = hex(kScrollActive);
    c[ImGuiCol_CheckMark] = hex(kText);
    c[ImGuiCol_SliderGrab] = hex(kBlue);
    c[ImGuiCol_SliderGrabActive] = hex(kBlueHover);
    c[ImGuiCol_Button] = hex(kWidget);
    c[ImGuiCol_ButtonHovered] = hex(kWidgetHover);
    c[ImGuiCol_ButtonActive] = hex(kBlue);
    c[ImGuiCol_Header] = hex(kSelect);
    c[ImGuiCol_HeaderHovered] = hex(kBlue, 0.85f);       // menu items light up blue
    c[ImGuiCol_HeaderActive] = hex(kBlue);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = hex(kLine);
    c[ImGuiCol_SeparatorActive] = hex(kBlue);
    c[ImGuiCol_ResizeGrip] = hex(0, 0);
    c[ImGuiCol_ResizeGripHovered] = hex(kBlue, 0.6f);
    c[ImGuiCol_ResizeGripActive] = hex(kBlue);
    c[ImGuiCol_Tab] = hex(kBar);
    c[ImGuiCol_TabHovered] = hex(kWidgetHover);
    c[ImGuiCol_TabSelected] = hex(kHeader);
    c[ImGuiCol_TabSelectedOverline] = hex(0, 0);
    c[ImGuiCol_TabDimmed] = hex(kBar);
    c[ImGuiCol_TabDimmedSelected] = hex(kHeader);
    c[ImGuiCol_TabDimmedSelectedOverline] = hex(0, 0);
    c[ImGuiCol_DockingPreview] = hex(kBlue, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = hex(kSplit);
    c[ImGuiCol_TableHeaderBg] = hex(kHeader);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = hex(kLine);
    c[ImGuiCol_TableRowBg] = hex(0, 0);
    c[ImGuiCol_TableRowBgAlt] = hex(kRowAlt);
    c[ImGuiCol_PlotHistogram] = hex(kBlue);
    c[ImGuiCol_PlotHistogramHovered] = hex(kBlueHover);
    c[ImGuiCol_TextSelectedBg] = hex(kBlue, 0.45f);
    c[ImGuiCol_DragDropTarget] = hex(kOrange);
    c[ImGuiCol_NavHighlight] = hex(kBlue);
    c[ImGuiCol_ModalWindowDimBg] = hex(0x000000, 0.55f);
}

// ── The app ─────────────────────────────────────────────────────────────────────────────────────

struct Studio {
    // The window it's shown in, as the window code (Win32 or GLFW) provides it.
    struct WindowHooks {
        std::function<void(const std::string&)> setTitle;
        std::function<void()> requestClose;                  // as if its close box was clicked (asks to save first)
        // The window's picture as shown (UI included), RGBA, top row first; false when it can't be read back.
        std::function<bool(std::vector<u8>& rgba, int& width, int& height)> capture;
    } window;
    float scale = 1.0f;
    HslRenderer renderer;
    VkSampler viewSampler = VK_NULL_HANDLE;
    VkDescriptorSet viewTex = VK_NULL_HANDLE;
    uint32_t viewW = 0, viewH = 0;          // renderer size
    ImVec2 viewPos, viewSize;               // viewport rect (screen pixels) from the last UI frame
    bool rendered = false;                  // the renderer image holds a frame this tick
    float renderMs = 0;                     // status bar: smoothed 3D frame time

    // Home
    std::unique_ptr<HslScene> scene;
    // Animation timeline (bottom of the viewport): the clock every animation reads, and the home's ambience.
    float animTime = 0, animLength = 0;
    bool animPlaying = true;
    std::chrono::steady_clock::time_point animTick = std::chrono::steady_clock::now();
    AudioPlayer ambience;
    AudioPlayer addedSound;                 // the background sound added to the environment (soundPanel)
    SoundState sound;
    std::vector<std::string> homePaths;     // footprint [+ vista]
    std::string homeName;
    std::vector<Object> objects;
    int selected = -1;
    bool dirty = false;
    std::vector<UndoStep> undo, redo;
    int undoGroup = 0, nextUndoGroup = 1;   // pushUndo tags steps with undoGroup while a batch is open
    std::vector<std::vector<std::string>> recent;   // File > Open Recent: each entry footprint [+ vista]
    bool showControls = false;              // Help > Controls
    bool testing = false;                   // --selftest: leave the recent list and edits alone
    struct TestEye { ImVec2 at; std::vector<size_t> members; };
    std::vector<TestEye> testEyes;          // --selftest: the explorer's eye buttons this frame (folders and rows)
    float animBase = 0;                     // completed timeline loops: HSL shaders read one continuous clock
    // World editor: the selected world item (kind -1 = none), what is drawn, and whether the layer was edited.
    int wKind = -1, wIndex = -1;
    bool showWorld = true;
    // What the viewport shows. View only: the per-item eyes (hidden) are kept with the world layer.
    bool showCat[kShowCats] = {true, true, true, true, true, false, false, true, true, true, true};
    bool showMeshes = true;                 // the home's own objects
    bool isolating = false;                 // View > Show Only Selected (I)
    int isoObject = -1, isoKind = -1, isoIndex = -1;
    bool gizmoHot = false;                  // the mouse was over the gizmo last frame (see viewport())
    HomeWorld dragBase;                     // the world layer as a gizmo drag began: scaling resizes from it
    std::shared_ptr<HomeWorld> fieldBefore; // properties: the world layer as the active field was clicked
    bool fieldPushed = false;               // ...and whether its undo step is already in
    bool worldEdited = false;
    // Save Environment runs in the background (baking a big home takes seconds).
    std::thread saver;
    std::atomic<bool> saving{false};
    std::string saveStatus;
    std::string savedEnvironment;           // the last .fenv written (recent list, Send to Frame)
    std::atomic<bool> saveFailed{false};    // the last Save didn't make it: still unsaved
    bool fastPack = true;                   // File > Faster Saving: parallel LZMA chunks (~1% bigger; fast-pack.txt), on unless turned off
    bool optimizeMeshes = true;             // File > Leave Out What Can't Be Seen (visibility_bake.h; optimize-meshes.txt), on unless turned off
    // Edit > Settings: this PC's (studio-settings.txt) and the headset's (frame-home/settings.json over ssh).
    struct StudioPerf { float viewScale = 1.0f; int msaa = 4; int textureCap = 2048; int anisotropy = 8; };
    StudioPerf studioPerf, studioPerfAtStart;
    bool showSettings = false;
    PerfSettings headsetPerf;
    std::string headsetStats, headsetStatus = "Not read from the headset yet";   // statusMutex
    std::atomic<bool> headsetLoaded{false};
    PerfSettings headsetPerfOnHeadset;      // what the headset has, as last read or written
    std::atomic<bool> headsetBusy{false};
    std::thread headsetThread;
    // Debug menu: the 3D view's debug views, the home report, and the headset's debug tools (frame-home/debug.json).
    int debugView = HslRenderer::kViewNormal;
    unsigned debugHide = 0;
    bool showReport = false;
    std::unique_ptr<HomeReport> report;     // for reportScene
    const HslScene* reportScene = nullptr;
    bool showHeadsetDebug = false;
    DebugSettings headsetDebug;             // statusMutex
    std::string headsetDebugStatus = "Not read from the headset yet", headsetReport;   // statusMutex
    long headsetClock = 0;                  // the headset's clock minus this PC's, seconds (picture requests use its time)
    std::atomic<bool> headsetDebugBusy{false}, headsetDebugLoaded{false};
    std::thread headsetDebugThread;

    // Background load
    std::thread loader;
    std::atomic<int> loadState{0};          // 0 idle, 1 loading, 2 ready, 3 failed
    bool uploading = false;                 // meshes still streaming to the GPU (a few per frame)
    bool texLoading = false;                // then every texture, before the home is shown
    int benchFrames = 0;                    // FS_BENCH_PICK: frames to wait (viewport sized) before the click benchmark
    std::unique_ptr<HslScene> loaded;
    std::vector<std::string> loadingPaths;
    std::string loadError;
    // A load that takes too long can be stopped. The loader can't be interrupted mid-parse, so a stopped one is
    // let go (detached) and whatever it finishes with is thrown away: each load has a generation number, and
    // only the current generation publishes its result.
    std::atomic<uint64_t> loadGen{0};
    std::mutex loadMutex;
    std::chrono::steady_clock::time_point loadStarted{};
    float loadPromptAfter = 45;             // seconds of loading before asking whether to keep waiting
    bool loadPromptOpen = false;
    bool loadAbandoned = false;             // a stopped loader may still be running: exit without destructors

    // Send to Frame
    std::thread sender;
    std::atomic<bool> sending{false};
    std::mutex statusMutex;
    std::string frameStatus = "Not sent yet";
    char frameHost[128] = {};               // ssh target for the headset (frame-host.txt; default kFrameHost)

    // Headset > Install on Steam Frame: Frame Home + environments, as the headset's environment
    std::thread installer;
    std::atomic<bool> installing{false};
    std::string installStatus = "Not installed from this PC yet";   // statusMutex
    std::vector<std::string> installEnvs;   // .fenv files to send
    bool installOpenHome = true;

    // View
    Camera cam;
    Tool tool = Tool::Move;
    bool localSpace = false;
    float flySpeed = 2.0f;
    int drag = -1;                          // mouse button dragging the camera (0 alt-orbit, 1 fly, 2 MMB)
    bool flying = false;
    ImVec2 clickPos;
    bool clickArmed = false;
    bool gizmoWasUsing = false;
    std::vector<float> clip;                // selection wire: clip-space vertices of the selected mesh
    char filter[128] = {};
    std::set<int> collapsed;                // outliner groups the user folded
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    // ── Home lifecycle ──

    void setStatus(const std::string& s) { std::lock_guard<std::mutex> l(statusMutex); frameStatus = s; }
    void setInstallStatus(const std::string& s) { std::lock_guard<std::mutex> l(statusMutex); installStatus = s; }
    std::string getInstallStatus() { std::lock_guard<std::mutex> l(statusMutex); return installStatus; }

    void loadSettings() {
        loadHost();
        if (FILE* f = platform::openFile((dataDir() / L"studio-settings.txt").c_str(), "rb")) {
            char line[128];
            while (std::fgets(line, sizeof line, f)) {
                float v = 0;
                char key[64] = {};
                if (std::sscanf(line, "%63[^=]=%f", key, &v) != 2) continue;
                const std::string k = key;
                if (k == "viewScale") studioPerf.viewScale = std::clamp(v, 0.25f, 1.0f);
                else if (k == "msaa") studioPerf.msaa = static_cast<int>(v);
                else if (k == "textureCap") studioPerf.textureCap = static_cast<int>(v);
                else if (k == "anisotropy") studioPerf.anisotropy = static_cast<int>(v);
                else if (k == "theme") g_theme = std::clamp(static_cast<int>(v), 0, kThemeCount - 1);
            }
            std::fclose(f);
        }
        studioPerfAtStart = studioPerf;
        setTextureCap(static_cast<uint32_t>(studioPerf.textureCap));
        if (FILE* f = platform::openFile((dataDir() / L"fast-pack.txt").c_str(), "rb")) { fastPack = std::fgetc(f) == '1'; std::fclose(f); }
        if (FILE* f = platform::openFile((dataDir() / L"optimize-meshes.txt").c_str(), "rb")) { optimizeMeshes = std::fgetc(f) == '1'; std::fclose(f); }
    }

    void saveStudioPerf() {
        if (FILE* f = platform::openFile((dataDir() / L"studio-settings.txt").c_str(), "wb")) {
            std::fprintf(f, "viewScale=%.2f\nmsaa=%d\ntextureCap=%d\nanisotropy=%d\ntheme=%d\n", studioPerf.viewScale, studioPerf.msaa, studioPerf.textureCap, studioPerf.anisotropy, g_theme);
            std::fclose(f);
        }
        setTextureCap(static_cast<uint32_t>(studioPerf.textureCap));
    }

    // The headset's settings and stats, read (or written, when `write`) over ssh on a worker thread.
    void headsetSettings(bool write) {
        if (headsetBusy.exchange(true)) return;
        if (headsetThread.joinable()) headsetThread.join();
        const std::string host8 = hostArg(), json = headsetPerf.toJson();
        headsetThread = std::thread([this, write, host8, json] {
            const std::wstring ssh = sshTool(L"ssh") + L"-o BatchMode=yes -o ConnectTimeout=8 " + widen(host8) + L" ";
            std::string out;
            if (write) {   // base64: nothing in the command line for a shell to read into
                const std::string enc = base64(json);
                const int rc = run(ssh + L"\"echo " + widen(enc) + L" | base64 -d > frame-home/settings.json.part && mv -f frame-home/settings.json.part frame-home/settings.json\"", L"", out);
                std::lock_guard<std::mutex> l(statusMutex);
                if (rc == 0) { headsetStatus = "Saved on the headset: Frame Home applies it now"; headsetPerf.fromJson(json); headsetPerfOnHeadset = headsetPerf; }
                else { headsetStatus = "Couldn't save: " + lastLine(out); headsetPerf = headsetPerfOnHeadset; }   // show what it still has
                headsetBusy = false;
                return;
            }
            // Markers around each file: ssh's own messages share the output, and a missing file is just empty.
            const int rc = run(ssh + L"\"echo @@settings; cat frame-home/settings.json 2>/dev/null; echo; echo @@stats; cat frame-home/stats.json 2>/dev/null; true\"", L"", out);
            std::lock_guard<std::mutex> l(statusMutex);
            const size_t a = out.find("@@settings\n"), sep = out.find("@@stats");
            if (rc != 0 || a == std::string::npos || sep == std::string::npos || sep < a) {
                headsetStatus = "Can't reach the headset (" + host8 + "): " + lastLine(out); headsetBusy = false; return; }
            PerfSettings p;
            if (p.fromJson(out.substr(a + 11, sep - a - 11))) { headsetPerf = p; headsetPerfOnHeadset = p; headsetLoaded = true; }
            headsetStats.clear();
            try {
                const tinyjson::Value st = tinyjson::parse(out.substr(sep + 7));
                if (!st.isObject() || !st.has("gpuMs")) throw std::runtime_error("no stats");
                char line[200];
                std::snprintf(line, sizeof line, "%.1f of %.1f ms a frame  \u00b7  %d x %d per eye (%.0f%%)  \u00b7  foveation %s",
                              st["gpuMs"].asFloat(), st["budgetMs"].asFloat(), static_cast<int>(st["width"].asFloat()), static_cast<int>(st["height"].asFloat()),
                              st["scale"].asFloat() * 100, !st["foveation"].boolVal ? "off" : st["eyeTracking"].boolVal ? "following your eyes" : "on (eyes not tracked right now)");
                headsetStats = line;
            } catch (...) {}
            headsetStatus = headsetLoaded ? "Read from the headset" : "The headset has no settings yet (install Frame Home first)";
            headsetBusy = false;
        });
    }

    void settingsWindow() {
        if (!showSettings) return;
        ImGui::SetNextWindowSize(ImVec2(560 * scale, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Settings", &showSettings, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
        const float lw = 170 * scale;
        auto choice = [&](const char* label, const char* const* opts, int n, int cur, const char* tip) -> int {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));   // not the "##id" part
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(lw);
            int picked = -1;
            ImGui::PushID(label);
            for (int i = 0; i < n; ++i) {
                if (i) ImGui::SameLine();
                const bool on = i == cur;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, hex(kBlue));
                if (ImGui::Button(opts[i])) picked = i;
                if (on) ImGui::PopStyleColor();
            }
            ImGui::PopID();
            return picked;
        };
        ImGui::SeparatorText("This PC (Frame Studio)");
        {
            static const char* scales[] = {"50%", "75%", "100%"};
            int cur = studioPerf.viewScale < 0.6f ? 0 : studioPerf.viewScale < 0.9f ? 1 : 2;
            if (int p = choice("Viewport resolution", scales, 3, cur, "Draws the 3D view at a share of its size: lower is faster on big homes."); p >= 0) {
                studioPerf.viewScale = p == 0 ? 0.5f : p == 1 ? 0.75f : 1.0f; saveStudioPerf(); }
            static const char* aa[] = {"Off", "2x", "4x"};
            cur = studioPerf.msaa >= 4 ? 2 : studioPerf.msaa >= 2 ? 1 : 0;
            if (int p = choice("Anti-aliasing", aa, 3, cur, "Smooths jagged edges in the 3D view."); p >= 0) { studioPerf.msaa = p == 2 ? 4 : p == 1 ? 2 : 1; saveStudioPerf(); }
            static const char* caps[] = {"1024", "2048", "4096", "Full"};
            cur = studioPerf.textureCap == 1024 ? 0 : studioPerf.textureCap == 2048 ? 1 : studioPerf.textureCap == 4096 ? 2 : 3;
            if (int p = choice("Texture limit", caps, 4, cur, "Largest texture size kept on this PC's graphics card. Lower uses less video memory."); p >= 0) {
                studioPerf.textureCap = p == 0 ? 1024 : p == 1 ? 2048 : p == 2 ? 4096 : 0; saveStudioPerf(); }
            static const char* af[] = {"4x", "8x", "16x"};
            cur = studioPerf.anisotropy >= 16 ? 2 : studioPerf.anisotropy >= 8 ? 1 : 0;
            if (int p = choice("Texture filtering", af, 3, cur, "Keeps floors and walls sharp at an angle."); p >= 0) { studioPerf.anisotropy = p == 2 ? 16 : p == 1 ? 8 : 4; saveStudioPerf(); }
            const bool restartNeeded = studioPerf.msaa != studioPerfAtStart.msaa || studioPerf.anisotropy != studioPerfAtStart.anisotropy;
            ImGui::TextDisabled("%s", restartNeeded ? "Anti-aliasing and texture filtering apply the next time Frame Studio starts."
                                                    : "The texture limit applies to homes opened from now on.");
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Steam Frame (Frame Home)");
        {
            std::string status, stats;
            bool loaded;
            PerfSettings hp;
            {
                std::lock_guard<std::mutex> l(statusMutex);
                status = headsetStatus; stats = headsetStats; loaded = headsetLoaded; hp = headsetPerf;
            }
            ImGui::BeginDisabled(headsetBusy || !loaded);
            bool changed = false;
            static const char* res[] = {"Auto", "80%", "100%", "120%", "140%"};
            int cur = hp.autoResolution ? 0 : hp.resolution < 0.9f ? 1 : hp.resolution < 1.1f ? 2 : hp.resolution < 1.3f ? 3 : 4;
            if (int p = choice("Resolution", res, 5, cur, "Auto keeps every frame on time and sharpens whenever the home is light enough (70-150%)."); p >= 0) {
                hp.autoResolution = p == 0; if (p > 0) hp.resolution = 0.6f + 0.2f * p; changed = true; }
            static const char* fr[] = {"Full", "Half"};
            if (int p = choice("Frame rate", fr, 2, hp.frameRate, "Half draws the environment every other frame and turns it with your head in between, as Meta's home does: "
                               "each frame gets twice the GPU time."); p >= 0) { hp.frameRate = p; changed = true; }
            static const char* aa[] = {"Off", "2x", "4x"};
            cur = hp.msaa >= 4 ? 2 : hp.msaa >= 2 ? 1 : 0;
            if (int p = choice("Anti-aliasing##hs", aa, 3, cur, nullptr); p >= 0) { hp.msaa = p == 2 ? 4 : p == 1 ? 2 : 1; changed = true; }
            static const char* af[] = {"4x", "8x", "16x"};
            cur = hp.anisotropy >= 16 ? 2 : hp.anisotropy >= 8 ? 1 : 0;
            if (int p = choice("Texture filtering##hs", af, 3, cur, nullptr); p >= 0) { hp.anisotropy = p == 2 ? 16 : p == 1 ? 8 : 4; changed = true; }
            static const char* fov[] = {"Off", "Fixed", "Eye tracked"};
            if (int p = choice("Foveated rendering", fov, 3, hp.foveation, "Full detail where you look, coarser toward the edges, using the Steam Frame's eye tracking. "
                               "The time it frees goes to resolution (with Auto)."); p >= 0) { hp.foveation = p; changed = true; }
            static const char* lv[] = {"Gentle", "Medium", "Strong"};
            ImGui::BeginDisabled(hp.foveation == 0);
            if (int p = choice("Foveation strength", lv, 3, hp.foveationLevel - 1, nullptr); p >= 0) { hp.foveationLevel = p + 1; changed = true; }
            static const char* tl[] = {"Off", "80\u00b0", "86\u00b0", "90\u00b0"};
            cur = hp.tempLimit <= 0 ? 0 : hp.tempLimit <= 82 ? 1 : hp.tempLimit <= 88 ? 2 : 3;
            if (int p = choice("Keep cool", tl, 4, cur, "Above this temperature the edges of view give up a little detail at a time until the headset cools "
                               "(where you look stays sharp). A hot Steam Frame slows its GPU to a third."); p >= 0) {
                hp.tempLimit = p == 0 ? 0 : p == 1 ? 80 : p == 2 ? 86 : 90; changed = true; }
            ImGui::EndDisabled();
            ImGui::Spacing();
            ImGui::TextDisabled("Moving around");
            static const char* walk[] = {"Off", "Smooth"};
            if (int p = choice("Walking##hs", walk, 2, hp.walk, "Left stick: walk where you look. Off for teleport only."); p >= 0) { hp.walk = p; changed = true; }
            static const char* tp[] = {"Off", "Instant", "Slide"};
            if (int p = choice("Teleport##hs", tp, 3, hp.teleport, "Right stick forward aims, let go to go there: in a blink, or gliding there."); p >= 0) { hp.teleport = p; changed = true; }
            static const char* tn[] = {"Snap", "Smooth"};
            if (int p = choice("Turning##hs", tn, 2, hp.turn, "Right stick left / right."); p >= 0) { hp.turn = p; changed = true; }
            static const char* sa[] = {"30\u00b0", "45\u00b0", "90\u00b0"};
            ImGui::BeginDisabled(hp.turn != 0);
            if (int p = choice("Snap angle##hs", sa, 3, hp.snapAngle >= 90 ? 2 : hp.snapAngle >= 45 ? 1 : 0, nullptr); p >= 0) { hp.snapAngle = p == 2 ? 90 : p == 1 ? 45 : 30; changed = true; }
            ImGui::EndDisabled();
            static const char* sp[] = {"Slow", "Normal", "Fast"};
            if (int p = choice("Walking speed##hs", sp, 3, hp.moveSpeed < 1.7f ? 0 : hp.moveSpeed < 2.5f ? 1 : 2, nullptr); p >= 0) {
                hp.moveSpeed = p == 0 ? 1.4f : p == 1 ? 2.0f : 3.0f; changed = true; }
            static const char* vg[] = {"Off", "Gentle", "Strong"};
            if (int p = choice("Comfort vignette", vg, 3, hp.vignette, "Darkens the edges of view while the stick moves or turns you. Helps if smooth walking makes you queasy.");
                p >= 0) { hp.vignette = p; changed = true; }
            static const char* sm[] = {"Shown", "Hidden"};
            if (int p = choice("Seat markers", sm, 2, hp.seatMarkers ? 0 : 1, "Markers on the seats near you. Hidden, pointing at a seat still finds it."); p >= 0) {
                hp.seatMarkers = p == 0 ? 1 : 0; changed = true; }
            ImGui::Spacing();
            ImGui::TextDisabled("The environment");
            static const char* vol[] = {"Off", "25%", "50%", "75%", "100%"};
            if (int p = choice("Volume", vol, 5, static_cast<int>(std::lround(hp.volume * 4)), "All of the environment's sound."); p >= 0) { hp.volume = p * 0.25f; changed = true; }
            static const char* an[] = {"Play", "Hold still"};
            if (int p = choice("Animations", an, 2, hp.animations ? 0 : 1, "Hold still stops the environment's moving parts; animated environments then cost less."); p >= 0) {
                hp.animations = p == 0 ? 1 : 0; changed = true; }
            static const char* mr[] = {"Half rate", "Full rate"};
            if (int p = choice("Behind the Steam menu", mr, 2, hp.menuRate, "With the menu open the environment is only its backdrop. Half rate leaves the GPU to the menu and its apps.");
                p >= 0) { hp.menuRate = p; changed = true; }
            ImGui::EndDisabled();
            if (changed) {
                { std::lock_guard<std::mutex> l(statusMutex); headsetPerf = hp; headsetStatus = "Saving\u2026"; }
                headsetSettings(true);
            }
            if (!stats.empty()) ImGui::TextDisabled("Now on the headset: %s", stats.c_str());
            ImGui::BeginDisabled(headsetBusy);
            if (ImGui::Button(headsetBusy ? "Working\u2026" : "Read from Headset")) headsetSettings(false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::TextDisabled("Anti-aliasing, filtering and foveation on / off restart Frame Home (a few seconds).");
        }
        ImGui::End();
    }

    // ── Debug ──

    // The object showing scene mesh si, selected and framed (the report's lists point at meshes).
    void selectSceneMesh(int si) {
        for (size_t i = 0; i < objects.size(); ++i)
            if (objects[i].scene == si && !objects[i].deleted) { selected = static_cast<int>(i); wKind = wIndex = -1; frameSelected(); return; }
    }

    // Debug > Home Report: what the open home is made of, what it costs and what's wrong with it. Rows that name a mesh
    // select it.
    void reportWindow() {
        if (!showReport) return;
        if (!scene) { showReport = false; return; }
        if (!report || reportScene != scene.get()) {
            report = std::make_unique<HomeReport>(makeHomeReport(*scene, scene->world.walkGridCache ? &scene->world : nullptr));
            reportScene = scene.get();
        }
        ImGui::SetNextWindowSize(ImVec2(640 * scale, 560 * scale), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Home Report", &showReport, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
        const HomeReport& r = *report;
        ImGui::TextUnformatted(r.name.c_str());
        if (!r.format.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", r.format.c_str()); }
        ImGui::TextWrapped("%zu meshes, %.2f M triangles (%zu see-through, %zu cut-out, %zu animated), %zu shaders", r.meshes, r.triangles / 1e6, r.seeThrough,
                           r.cutout, r.animated, r.programs);
        ImGui::TextWrapped("%zu textures: %.0f MB compressed, %.0f MB decoded; geometry %.0f MB", r.textures, r.textureMB, r.decodedMB, r.geometryMB);
        if (r.walkArea >= 0) ImGui::TextWrapped("%.0f m² to walk on; %zu spawns, %zu seats, %zu surfaces, %zu wall spots", r.walkArea, r.spawns, r.seats, r.colliders, r.wallSpots);
        else ImGui::TextWrapped("%zu spawns, %zu seats, %zu surfaces, %zu wall spots", r.spawns, r.seats, r.colliders, r.wallSpots);
        if (ImGui::Button("Check Again")) report.reset();
        ImGui::SameLine();
        if (ImGui::Button("Copy as Text")) ImGui::SetClipboardText(r.text().c_str());
        ImGui::BeginChild("##report", ImVec2(0, 0), ImGuiChildFlags_None);
        auto row = [&](const std::string& text, int mesh, uint32_t color, int id) {
            ImGui::PushID(id);
            ImGui::PushStyleColor(ImGuiCol_Text, hex(color));
            if (ImGui::Selectable(text.c_str(), mesh >= 0 && selected >= 0 && objects[selected].scene == mesh) && mesh >= 0) selectSceneMesh(mesh);
            ImGui::PopStyleColor();
            if (mesh >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Select %s", r.meshName(mesh).c_str());
            ImGui::PopID();
        };
        ImGui::SeparatorText("Problems");
        if (r.problems.empty()) ImGui::TextDisabled("Nothing wrong found.");
        for (size_t i = 0; i < r.problems.size(); ++i) {
            const HomeProblem& pr = r.problems[i];
            row(std::string(pr.level >= 2 ? "Broken: " : pr.level == 1 ? "" : "Note: ") + pr.what, pr.mesh, pr.level >= 2 ? 0xe06c66 : pr.level == 1 ? kText : kDim,
                static_cast<int>(i));
        }
        if (debugView == HslRenderer::kViewCost) {
            ImGui::SeparatorText("Costliest right now (this PC's GPU, both eyes' share)");
            const auto top = renderer.costliest(10);
            if (top.empty()) ImGui::TextDisabled("Measuring…");
            for (size_t i = 0; i < top.size(); ++i) {
                char b[64];
                std::snprintf(b, sizeof b, "%7.3f ms  ", top[i].first);
                row(b + (top[i].second >= 0 && static_cast<size_t>(top[i].second) < scene->meshes.size() ? scene->meshes[top[i].second].name : std::string("?")),
                    top[i].second, kText, 1000 + static_cast<int>(i));
            }
        }
        ImGui::SeparatorText("Most triangles");
        for (size_t i = 0; i < r.biggestMeshes.size(); ++i) {
            char b[64];
            std::snprintf(b, sizeof b, "%9zu  ", r.biggestMeshes[i].tris);
            row(b + r.meshName(r.biggestMeshes[i].mesh), r.biggestMeshes[i].mesh, kText, 2000 + static_cast<int>(i));
        }
        ImGui::SeparatorText("Biggest textures");
        for (size_t i = 0; i < r.biggestTextures.size(); ++i) {
            const HomeReport::Tex& t = r.biggestTextures[i];
            char b[128];
            std::snprintf(b, sizeof b, "%6.1f MB  %ux%u, %u mip%s, %zu mesh%s  ", t.mb, t.w, t.h, t.mips, t.mips == 1 ? "" : "s", t.users, t.users == 1 ? "" : "es");
            row(b + r.meshName(t.mesh), t.mesh, kText, 3000 + static_cast<int>(i));
        }
        ImGui::EndChild();
        ImGui::End();
    }

    // The headset's debug tools over ssh (on a worker thread): 0 read debug.json and the report, 1 write debug.json,
    // 2 ask for a picture and bring it here (opened when it arrives).
    void headsetDebugCall(int action) {
        if (headsetDebugBusy.exchange(true)) return;
        if (headsetDebugThread.joinable()) headsetDebugThread.join();
        DebugSettings d;
        {
            std::lock_guard<std::mutex> l(statusMutex);
            d = headsetDebug;
            headsetDebugStatus = action == 0 ? "Reading…" : action == 1 ? "Saving…" : "Asking for a picture…";
        }
        if (action == 2) d.shot = static_cast<int>(std::time(nullptr) + headsetClock);   // in the headset's time
        const std::string host8 = hostArg();
        headsetDebugThread = std::thread([this, action, d, host8] {
            const std::wstring ssh = sshTool(L"ssh") + L"-o BatchMode=yes -o ConnectTimeout=8 " + widen(host8) + L" ";
            auto done = [&](const std::string& st) { std::lock_guard<std::mutex> l(statusMutex); headsetDebugStatus = st; headsetDebugBusy = false; };
            std::string out;
            if (action == 0) {
                const int rc = run(ssh + L"\"echo @@debug; cat frame-home/debug.json 2>/dev/null; echo; echo @@time; date +%s; echo @@report; cat frame-home/logs/report.txt 2>/dev/null; true\"", L"", out);
                const size_t a = out.find("@@debug\n"), t = out.find("@@time\n"), rp = out.find("@@report\n");
                if (rc != 0 || a == std::string::npos || t == std::string::npos || rp == std::string::npos) return done("Can't reach the headset (" + host8 + "): " + lastLine(out));
                DebugSettings got;
                if (!got.fromJson(out.substr(a + 8, t - a - 8))) got = DebugSettings();   // none: everything off
                const long headsetNow = std::atol(out.c_str() + t + 7);
                std::lock_guard<std::mutex> l(statusMutex);
                headsetDebug = got;
                if (headsetNow > 0) headsetClock = headsetNow - static_cast<long>(std::time(nullptr));
                headsetReport = out.substr(rp + 9);
                headsetDebugLoaded = true;
                headsetDebugStatus = "Read from the headset";
                headsetDebugBusy = false;
                return;
            }
            const int rc = run(ssh + L"\"echo " + widen(base64(d.toJson())) + L" | base64 -d > frame-home/debug.json.part && mv -f frame-home/debug.json.part frame-home/debug.json\"", L"", out);
            if (rc != 0) return done("Couldn't save: " + lastLine(out));
            { std::lock_guard<std::mutex> l(statusMutex); headsetDebug = d; }
            if (action == 1) return done("Saved: Frame Home shows it within a second");
            // The picture: Frame Home takes it on its next frame; then the newest one comes here.
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            out.clear();
            if (run(ssh + L"\"ls -t frame-home/logs/frame-*.png 2>/dev/null | head -1\"", L"", out) != 0) return done("Couldn't look for the picture: " + lastLine(out));
            std::string remote = lastLine(out);
            if (remote.find("frame-") == std::string::npos || remote.find(".png") == std::string::npos) return done("No picture yet: is the environment showing?");
            for (char c : remote) if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr("/-_.", c)) return done("Unexpected picture name: " + remote);
            const fs::path dir = dataDir() / L"pictures";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string name = remote.substr(remote.find_last_of('/') + 1);
            out.clear();
            const std::wstring scp = sshTool(L"scp") + L"-q -o BatchMode=yes -o ConnectTimeout=8 ";
            if (run(scp + widen(host8) + L":" + widen(remote) + L" \"" + widen(name) + L"\"", dir.wstring(), out) != 0) return done("Couldn't copy the picture: " + lastLine(out));
            platform::openPath(narrow(fs::path((dir / widen(name)).c_str()).wstring()));
            done("Picture saved: " + narrow((dir / widen(name)).wstring()));
        });
    }

    // Debug > Headset Debug: the Environments panel's Debug page, from here.
    void headsetDebugWindow() {
        if (!showHeadsetDebug) return;
        ImGui::SetNextWindowSize(ImVec2(600 * scale, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Headset Debug", &showHeadsetDebug, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
        DebugSettings d;
        std::string status, rep;
        {
            std::lock_guard<std::mutex> l(statusMutex);
            d = headsetDebug; status = headsetDebugStatus; rep = headsetReport;
        }
        const float lw = 170 * scale;
        auto choice = [&](const char* label, const char* const* opts, int n, int cur, const char* tip) -> int {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(lw);
            int picked = -1;
            ImGui::PushID(label);
            for (int i = 0; i < n; ++i) {
                if (i) ImGui::SameLine();
                const bool on = i == cur;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, hex(kBlue));
                if (ImGui::Button(opts[i])) picked = i;
                if (on) ImGui::PopStyleColor();
            }
            ImGui::PopID();
            return picked;
        };
        ImGui::BeginDisabled(headsetDebugBusy || !headsetDebugLoaded);
        bool changed = false;
        static const char* views[] = {"Normal", "Wireframe", "Meshes", "GPU cost", "Overdraw"};
        if (int p = choice("View##hd", views, 5, d.view, "What Frame Home draws: the environment, or how it's made and what it costs."); p >= 0) { d.view = p; changed = true; }
        static const char* wl[] = {"Hidden", "Shown"};
        if (int p = choice("World layer##hd", wl, 2, d.world, "Where you can walk (green), surfaces and colliders, spawns, seats, wall spots and gravity zones."); p >= 0) {
            d.world = p; changed = true; }
        static const char* lo[] = {"Shown", "Left out"};
        if (int p = choice("See-through##hd", lo, 2, d.hideSeeThrough, "Glass, water, glow and fog."); p >= 0) { d.hideSeeThrough = p; changed = true; }
        if (int p = choice("Cut-out##hd", lo, 2, d.hideCutout, "Leaves, grass and fences."); p >= 0) { d.hideCutout = p; changed = true; }
        if (int p = choice("Sky##hd", lo, 2, d.hideSky, nullptr); p >= 0) { d.hideSky = p; changed = true; }
        ImGui::EndDisabled();
        if (changed) {
            { std::lock_guard<std::mutex> l(statusMutex); headsetDebug = d; }
            headsetDebugCall(1);
        }
        ImGui::BeginDisabled(headsetDebugBusy || !headsetDebugLoaded);
        if (ImGui::Button("Take a Picture")) headsetDebugCall(2);
        ImGui::SameLine();
        ImGui::BeginDisabled(!d.any());
        if (ImGui::Button("Turn Debug Views Off")) { { std::lock_guard<std::mutex> l(statusMutex); const int shot = headsetDebug.shot; headsetDebug = DebugSettings(); headsetDebug.shot = shot; } headsetDebugCall(1); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(headsetDebugBusy);
        if (ImGui::Button(headsetDebugBusy ? "Working…" : "Read from Headset##hd")) headsetDebugCall(0);
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", status.c_str());
        if (!rep.empty()) {
            ImGui::SeparatorText("The environment showing on the headset");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 560 * scale);
            ImGui::TextUnformatted(rep.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("Debug views are off again whenever Frame Home restarts.");
        ImGui::End();
    }

    void loadHost() {
        std::snprintf(frameHost, sizeof frameHost, "%s", kFrameHost);
        if (FILE* f = platform::openFile((dataDir() / L"frame-host.txt").c_str(), "rb")) {
            char line[128] = {};
            if (std::fgets(line, sizeof line, f)) {
                std::string h = line;
                while (!h.empty() && std::isspace(static_cast<unsigned char>(h.back()))) h.pop_back();
                if (!h.empty()) std::snprintf(frameHost, sizeof frameHost, "%s", h.c_str());
            }
            std::fclose(f);
        }
    }
    void saveHost() {
        if (FILE* f = platform::openFile((dataDir() / L"frame-host.txt").c_str(), "wb")) { std::fprintf(f, "%s\n", frameHost); std::fclose(f); }
    }
    // The ssh target, safe for a command line (an alias or user@host: nothing a shell could act on).
    std::string hostArg() const {
        std::string h;
        for (const char* c = frameHost; *c; ++c)
            if (std::isalnum(static_cast<unsigned char>(*c)) || std::strchr("@.-_:", *c)) h += *c;
        return h.empty() ? kFrameHost : h;
    }
    std::string status() { std::lock_guard<std::mutex> l(statusMutex); return frameStatus; }

    // File > Open Recent, kept in recent.txt (one home per line, its APKs separated by '|').
    void loadRecent() {
        recent.clear();
        FILE* f = platform::openFile((dataDir() / L"recent.txt").c_str(), "rb");
        if (!f) return;
        char line[8192];
        while (std::fgets(line, sizeof line, f) && recent.size() < 8) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            std::vector<std::string> paths;
            for (size_t a = 0, b; a < l.size(); a = b + 1) {
                b = l.find('|', a);
                if (b == std::string::npos) b = l.size();
                if (b > a) paths.push_back(l.substr(a, b - a));
            }
            if (!paths.empty()) recent.push_back(paths);
        }
        std::fclose(f);
    }

    void addRecent(const std::vector<std::string>& paths) {
        if (testing) return;
        recent.erase(std::remove(recent.begin(), recent.end(), paths), recent.end());
        recent.insert(recent.begin(), paths);
        if (recent.size() > 8) recent.resize(8);
        FILE* f = platform::openFile((dataDir() / L"recent.txt").c_str(), "wb");
        if (!f) return;
        for (const auto& r : recent) {
            std::string l;
            for (const auto& p : r) l += (l.empty() ? "" : "|") + p;
            std::fprintf(f, "%s\n", l.c_str());
        }
        std::fclose(f);
    }

    // One line per APK: "Vista · HSL · v210.0.0.0.502".
    std::string sourceLine(size_t i) const {
        if (!scene || i >= scene->sources.size()) return {};
        const HslSourceInfo& s = scene->sources[i];
        return s.role + " \u00b7 " + s.format + (s.version.empty() ? "" : " \u00b7 " + std::string(std::isdigit(static_cast<unsigned char>(s.version[0])) ? "v" : "") + s.version);
    }

    fs::path worldFile() const { return dataDir() / L"edits" / widen(frameName(homePaths.front()) + ".world.json"); }

    fs::path editsFile() const { return dataDir() / L"edits" / widen(frameName(homePaths.front()) + ".edits.json"); }

    void updateTitle() {
        std::wstring t = L"Frame Studio";
        if (!homeName.empty()) t = widen(shownName()) + (dirty ? L"*" : L"") + L" \u2014 Frame Studio";
        if (window.setTitle) window.setTitle(narrow(t));
    }

    bool confirmDiscard() {
        if (!dirty) return true;
        const platform::Answer r = platform::ask("Frame Studio", "Save your changes to " + shownName() + " first?", true);
        if (r == platform::Answer::Cancel) return false;
        if (r == platform::Answer::Yes) saveToLibrary();                               // (the environment saves from its own copies)
        return true;
    }

    // A footprint opened on its own is paired with a vista (it's only the room: the lighting, sky and world outside
    // ship in a vista): the one it was last opened with, else the one beside it, else you pick one of those beside it.
    std::string vistaFootprint;                 // waiting for its vista to be picked
    std::vector<std::string> vistaChoices;
    bool pairVista(std::vector<std::string>& paths) {
        if (paths.size() != 1 || !isFootprintApk(paths[0])) return true;
        for (const auto& r : recent)
            if (r.size() == 2 && r[0] == paths[0] && fs::exists(fs::path(widen(r[1])))) { paths.push_back(r[1]); return true; }
        const std::vector<std::string> v = vistasBeside(paths[0]);
        if (v.size() == 1) { paths.push_back(v[0]); return true; }
        if (v.empty()) return true;                                    // none: the room alone (the banner offers Add Vista)
        vistaFootprint = paths[0];
        vistaChoices = v;
        return false;
    }
    void vistaPicker() {
        if (vistaChoices.empty()) return;
        if (!ImGui::IsPopupOpen("Pick a vista")) ImGui::OpenPopup("Pick a vista");
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Pick a vista", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::Text("%s is a footprint: the room.", prettyHome(vistaFootprint).c_str());
        ImGui::TextDisabled("Its lighting, sky and the world outside come from a vista. They become one home (one file on the Frame).");
        ImGui::Spacing();
        std::vector<std::string> pick;
        for (const auto& v : vistaChoices)
            if (ImGui::Button((prettyHome(v) + "##" + v).c_str(), ImVec2(260 * scale, 0))) pick = {vistaFootprint, v};
        ImGui::Spacing();
        if (ImGui::Button("The room alone", ImVec2(260 * scale, 0))) pick = {vistaFootprint};
        if (!pick.empty()) { vistaChoices.clear(); vistaFootprint.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        if (pick.size() == 2) open(pick);
        else if (pick.size() == 1) open(pick, false);
    }

    void open(std::vector<std::string> paths, bool pair = true) {
        if (paths.size() == 1 && isFenvBundlePath(paths[0])) { openBundle(paths[0]); return; }   // a bundle: its environments, listed
        if (paths.empty() || loadState == 1 || saving) return;
        if (pair) carry = Carry();                                     // another environment: nothing carries over
        if (pair && !pairVista(paths)) return;                         // asked which vista: opens from the picker
        if (!confirmDiscard()) return;
        if (loader.joinable()) loader.join();
        paths = footprintFirst(paths);
        loadingPaths = paths;
        loadError.clear();
        loadState = 1;
        loadStarted = std::chrono::steady_clock::now();
        loadPromptAfter = 45;
        const uint64_t gen = ++loadGen;
        loader = std::thread([this, paths, gen] {
            auto s = std::make_unique<HslScene>();
            std::string err;
            const bool ok = loadHslScene(paths, *s, err);
            std::lock_guard<std::mutex> l(loadMutex);
            if (gen != loadGen) return;                   // stopped meanwhile: drop it
            if (ok) { loaded = std::move(s); loadState = 2; }
            else { loadError = err.empty() ? "could not load the home" : err; loadState = 3; }
        });
    }

    // Stops a load in progress: the background parse (let go, its result dropped) or the GPU upload after it.
    // Whatever home was open before the load stays closed (opening one already replaced it).
    void stopLoading() {
        {
            std::lock_guard<std::mutex> l(loadMutex);
            ++loadGen;
            if (loadState == 1 && loader.joinable()) { loader.detach(); loadAbandoned = true; }
            else if (loader.joinable()) loader.join();
            loadState = 0;
            loaded.reset();
        }
        if (uploading || texLoading || scene) {
            vkDeviceWaitIdle(renderer.device());
            renderer.unload();
            ambience.stop();
            addedSound.stop();
            sound = SoundState();
            scene.reset();
            objects.clear();
            undo.clear();
            redo.clear();
            homePaths.clear();
            homeName.clear();
            selected = -1;
            wKind = wIndex = -1;
            dirty = worldEdited = false;
            uploading = texLoading = false;
            updateTitle();
        }
        loadPromptOpen = false;
        std::fprintf(stderr, "[studio] loading stopped by the user\n");
    }

    bool isLoading() const { return loadState == 1 || uploading || texLoading; }
    // Whether the window's picture can change without any input: the frame pacer stops drawing when it can't.
    bool viewChangesByItself() const { return isLoading() || saving || flying || (scene && animPlaying); }

    // After loadPromptAfter seconds: keep waiting (asked again a minute later), stop loading, or close Studio now.
    void loadPrompt() {
        if (!isLoading()) {   // the load finished (or failed) with the question up: take it down
            if (loadPromptOpen && ImGui::BeginPopupModal("Still loading", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
            loadPromptOpen = false;
            return;
        }
        const float secs = std::chrono::duration<float>(std::chrono::steady_clock::now() - loadStarted).count();
        if (!loadPromptOpen && secs > loadPromptAfter) { loadPromptOpen = true; ImGui::OpenPopup("Still loading"); }
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Still loading", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
        ImGui::PushTextWrapPos(440 * scale);
        const std::string what = loadingPaths.empty() ? std::string("this home") : displayName(loadingPaths);
        ImGui::TextWrapped("%s has been loading for %.0f seconds (%s).", what.c_str(), secs,
                           loadState == 1 ? "reading and converting it" : uploading ? "uploading it to the graphics card" : "loading its textures");
        ImGui::TextDisabled("Big homes with a vista can take a while on the first open.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Keep Waiting", ImVec2(130 * scale, 0))) { loadPromptAfter = secs + 60; loadPromptOpen = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Stop Loading", ImVec2(130 * scale, 0))) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); stopLoading(); return; }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, hex(kDanger));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(kDangerHover));
        if (ImGui::Button("Close Frame Studio", ImVec2(150 * scale, 0))) {
            // Now, not after the load: nothing of a home still loading can be lost, and waiting is what stalled.
            std::fprintf(stderr, "[studio] closed by the user while loading\n");
            std::fflush(stderr);
            std::_Exit(0);
        }
        ImGui::PopStyleColor(2);
        ImGui::EndPopup();
    }

    // A footprint (the room) gets its baked lighting, sky and outside world from a vista: add one to it.
    void addVistaDialog() {
        if (homePaths.empty()) return;
        const std::vector<std::string> picked = platform::openFiles("Add a vista (e.g. Shell Env Vista Calming)", {{"Meta vista", {"*.apk"}}}, false);
        if (picked.empty()) return;
        carryAcrossReload();                                           // the same environment, with a vista now
        dirty = false;
        open({homePaths.front(), picked.front()}, false);
    }

    bool footprintAlone() const {
        if (homePaths.size() != 1) return false;
        std::string n = homePaths.front();
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return n.find("footprint") != std::string::npos;
    }

    // Pick the footprint, or the footprint and its vista together (Ctrl-click both).
    void openDialog() {
        const std::vector<std::string> picked = platform::openFiles("Open a home (footprint + vista together), an environment (.fenv), a bundle or models",
            {{"Homes, environments and models", {"*.apk", "*.fenv", "*.fbundle", "*.glb", "*.gltf", "*.fbx", "*.obj"}}, {"Meta home", {"*.apk"}},
             {"Frame environment", {"*.fenv"}}, {"Environment bundle", {"*.fbundle"}}, {"Model from Blender / Unity", {"*.glb", "*.gltf", "*.fbx", "*.obj"}}}, true);
        if (picked.empty()) return;
        open(picked);
    }

    // Main thread: upload the finished load and build the object list.
    // Diagnostics (FS_BENCH_PICK=1): click a 6x4 grid over the viewport without touching the mouse, timing the
    // pick and the selection highlight each click builds.
    void benchPick() {
        double worstPick = 0, worstSel = 0;
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 6; ++gx) {
                const float x = viewPos.x + viewSize.x * (gx + 0.5f) / 6, y = viewPos.y + viewSize.y * (gy + 0.5f) / 4;
                const auto a = std::chrono::steady_clock::now();
                const int hit = pick(x, y);
                const auto b = std::chrono::steady_clock::now();
                renderer.setSelection(hit >= 0 ? static_cast<int>(objects[hit].mesh) : -1);
                const auto c = std::chrono::steady_clock::now();
                const double pm = std::chrono::duration<double, std::milli>(b - a).count(), sm = std::chrono::duration<double, std::milli>(c - b).count();
                worstPick = std::max(worstPick, pm);
                worstSel = std::max(worstSel, sm);
                std::fprintf(stderr, "[bench] click (%d,%d) -> %s [%s]: pick %.1f ms, highlight %.1f ms\n", gx, gy,
                             hit >= 0 ? objects[hit].name.c_str() : "nothing",
                             hit >= 0 ? scene->meshes[objects[hit].scene].shaderPath.c_str() : "", pm, sm);
            }
        // Leave the biggest hit selected (highlight drawn every frame from now on) to exercise the overlay draw.
        int best = -1;
        size_t bestTris = 0;
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 6; ++gx) {
                const int hit = pick(viewPos.x + viewSize.x * (gx + 0.5f) / 6, viewPos.y + viewSize.y * (gy + 0.5f) / 4);
                if (hit < 0) continue;
                const size_t t = scene->meshes[objects[hit].scene].indices.size();
                if (t > bestTris) { bestTris = t; best = hit; }
            }
        selected = best;
        std::fprintf(stderr, "[bench] leaving '%s' selected (%zu indices)\n", best >= 0 ? objects[best].label.c_str() : "nothing", bestTris);
        std::fprintf(stderr, "[bench] worst pick %.1f ms, worst highlight %.1f ms\n", worstPick, worstSel);
    }

    void finishLoad() {
        if (benchFrames > 0 && --benchFrames == 0) benchPick();
        if (!saving && saver.joinable()) {
            saver.join();
            if (!savedEnvironment.empty()) { recentInfo.erase(savedEnvironment + "|"); addRecent({savedEnvironment}); savedEnvironment.clear(); }
            if (saveFailed.exchange(false)) { dirty = true; updateTitle(); }
        }
        if (!saving && saveQueued && scene && !uploading) { saveQueued = false; saveToLibrary(); }   // a Save asked for mid auto-save
        if (!bundleBusy && bundler.joinable()) { bundler.join(); bundleFinished(); }
        if (scene && dirty && !saving && !uploading && loadState == 0 && std::chrono::steady_clock::now() - autosavedAt > std::chrono::minutes(5)) {
            autosaveVersion();
            autosavedAt = std::chrono::steady_clock::now();
        }
        if (loadState == 3) {
            if (loader.joinable()) loader.join();
            loadState = 0;
            carry = Carry();                                           // (a reload that failed: its changes went with it)
            platform::message("Couldn't open this home", loadError, platform::Icon::Warning);
            return;
        }
        if (uploading) {                                  // a few milliseconds of GPU upload per frame
            if (!renderer.loadStep(12.0)) return;
            uploading = false;
            buildObjects();
            renderer.requestAllTextures();                // everything at once: nothing streams in while moving
            texLoading = renderer.texturesPending() > 0;
            if (!texLoading && std::getenv("FS_BENCH_PICK")) benchFrames = 30;
            return;
        }
        if (texLoading) {
            if (renderer.texturesPending() == 0) {
                texLoading = false;
                if (std::getenv("FS_BENCH_PICK")) benchFrames = 30;
            }
            return;
        }
        if (loadState != 2) return;
        if (loader.joinable()) loader.join();
        loadState = 0;
        renderer.unload();
        scene = std::move(loaded);
        windSign = 2;                       // read again off this home's floor
        walkShown.reset(); walkReport = WalkReport{}; walkSig = 0; walkPending = walkSaveNext = false; walkGuideDirty = true;
        objects.clear();
        undo.clear();
        redo.clear();
        selected = -1;
        wKind = wIndex = -1;
        isolating = false;
        worldEdited = false;
        dirty = false;
        homePaths = loadingPaths;
        homeName = displayName(homePaths);
        if (!carry.any) envFile = homePaths.size() == 1 && isFenvPath(homePaths[0]) ? homePaths[0] : std::string();   // a home or models: a new one
        autosavedAt = std::chrono::steady_clock::now();
        addRecent(homePaths);
        renderer.setDynamicMeshes(scene->animatedMeshes);
        renderer.beginLoad(*scene);
        animTime = animBase = 0;
        animPlaying = true;
        animLength = scene->animDuration > 0 ? scene->animDuration : 60.0f;   // no authored loop: a minute of shader time
        sound = SoundState();                         // (the saved one comes back with the edits)
        restartSounds();
        uploading = true;
        cam.playerView();
        updateTitle();
    }

    // Once every mesh is on the GPU: the editable object list, saved edits, and the view.
    void buildObjects() {
        if (renderer.meshCount() == 0)
            platform::message("Frame Studio", "Nothing in this home could be drawn with its shaders.", platform::Icon::Warning);
        for (size_t i = 0; i < renderer.meshCount(); ++i) {
            const MeshData& md = scene->meshes[renderer.meshSceneIndex(i)];
            Object o;
            o.mesh = i;
            o.scene = renderer.meshSceneIndex(i);
            o.source = scene->meshSource[o.scene];
            o.name = md.name.empty() ? "mesh " + std::to_string(o.scene) : md.name;
            const std::string& prog = renderer.meshProgram(i);
            size_t s = prog.find(".surface");
            size_t b = s == std::string::npos ? std::string::npos : prog.rfind('/', s);
            o.surface = s == std::string::npos ? prog : prog.substr(b == std::string::npos ? 0 : b + 1, s - (b == std::string::npos ? 0 : b + 1));
            o.skybox = md.isSkybox;
            o.triangles = md.indices.size() / 3;
            o.category = categorize(o.name, o.surface, o.skybox);
            o.label = cleanName(o.name);
            o.folders.clear();
            for (size_t a = 0; a <= md.path.size();) {                 // a template instance and its root often share
                const size_t b = std::min(md.path.find('/', a), md.path.size());   // a name: one folder, not two
                const std::string seg = md.path.substr(a, b - a);
                if (!seg.empty() && (o.folders.empty() || o.folders.back() != seg)) o.folders.push_back(seg);
                a = b + 1;
            }
            for (int k = 0; k < 3; ++k) { o.lmin[k] = 1e30f; o.lmax[k] = -1e30f; }
            for (size_t v = 0; v + 2 < md.positions.size(); v += 3)
                for (int k = 0; k < 3; ++k) {
                    o.lmin[k] = std::min(o.lmin[k], md.positions[v + k]);
                    o.lmax[k] = std::max(o.lmax[k], md.positions[v + k]);
                }
            // Gizmo frame at the object's visual centre (many homes bake world positions into the vertices).
            const float* w = renderer.meshWorld(i);
            float c[3] = {(o.lmin[0] + o.lmax[0]) * 0.5f, (o.lmin[1] + o.lmax[1]) * 0.5f, (o.lmin[2] + o.lmax[2]) * 0.5f}, wc[3];
            point(w, c, wc);
            translation(o.pivot0, wc[0], wc[1], wc[2]);
            std::memcpy(o.pivot, o.pivot0, 64);
            float inv[16];
            translation(inv, -wc[0], -wc[1], -wc[2]);
            mul(inv, w, o.offset);
            objects.push_back(o);
        }
        loadEdits();
        updateTitle();
    }

    // What an environment is comes from its file alone: a .fenv's world layer, edits, sound, name and picture; a
    // Meta home or models opened fresh, as they ship. Nothing is kept on the side between opens. The one exception is
    // a reload of the open home (Add Vista, Add Models): its unsaved changes are carried across it.
    struct Carry { bool any = false; std::vector<ObjectEdit> objects; std::string world, title, image; SoundState sound; WorldSettings settings; } carry;
    std::vector<ObjectEdit> currentObjectEdits() const {
        std::vector<ObjectEdit> v;
        for (const Object& o : objects) {
            if (!o.deleted && std::memcmp(o.pivot, o.pivot0, 64) == 0) continue;
            ObjectEdit x;
            x.index = o.scene; x.name = o.name; x.deleted = o.deleted;
            std::memcpy(x.pivot, o.pivot, 64);
            mul(o.pivot, o.offset, x.world);
            v.push_back(x);
        }
        return v;
    }
    void carryAcrossReload() {
        if (!scene) return;
        carry.any = true;
        carry.objects = currentObjectEdits();
        carry.world = worldToJson(scene->world);
        carry.title = envTitle; carry.image = envImage;
        carry.sound = sound;
        carry.settings = worldSettings;
    }

    void loadEdits() {
        std::vector<ObjectEdit> objs;
        if (carry.any) {                                               // the same environment, reloaded: as it was
            HomeWorld w;
            if (worldFromJson(carry.world, w)) { scene->world = std::move(w); worldEdited = true; }
            prepareWorld();
            objs = carry.objects;
            envTitle = carry.title; envImage = carry.image;
            worldSettings = carry.settings;
            sound = carry.sound;
            restartSounds();
            carry = Carry();
            dirty = true;
        } else {
            prepareWorld();                                            // the file's own world layer (or the home's)
            HomeEdits fe;
            const bool own = !scene->fenvEdits.empty() && loadHomeEdits(scene->fenvEdits, fe);
            loadSound(own ? fe.sound : HomeSound(), own ? scene->fenvEdits : std::string());
            envTitle = own ? fe.title : std::string();
            envImage.clear();
            worldSettings = WorldSettings();
            FenvManifest m;
            std::vector<uint8_t> thumb;
            if (openHomeIsFenv() && fenvReadManifest(homePaths[0], m, &thumb)) {
                worldSettings = m.settings;
                if (envTitle.empty() && m.name != homeName) envTitle = m.name;
                if (own && !fe.image.empty() && !thumb.empty()) {          // its picture: the thumbnail it carries
                    std::error_code ec;
                    fs::create_directories(dataDir() / L"edits", ec);
                    std::ofstream out(dataDir() / L"edits" / fs::u8path(fe.image), std::ios::binary);
                    out.write(reinterpret_cast<const char*>(thumb.data()), static_cast<std::streamsize>(thumb.size()));
                    if (out) envImage = fe.image;
                }
            }
            if (own) objs = fe.objects;
        }
        for (const ObjectEdit& x : objs)
            for (Object& o : objects)
                if (o.scene == x.index && o.name == x.name) {
                    std::memcpy(o.pivot, x.pivot, 64);
                    o.deleted = x.deleted;
                    apply(o);
                }
        unifySurfaces();
    }

    // Older saves split an object into a Floor part and a Wall part, and copied every triangle of the mesh: one
    // simplified Surface now (the roles already read as Solid). An already simplified surface is left as it is.
    void unifySurfaces() {
        bool any = false;
        for (const Object& o : objects) {
            const std::vector<int> parts = surfaceParts(o);
            if (parts.empty()) continue;
            const bool split = parts.size() > 1 && surfaceMode(o) != kSurfTrigger;
            const bool full = parts.size() == 1 && scene->world.colliders[parts[0]].indices.size() == scene->meshes[o.scene].indices.size() &&
                              scene->meshes[o.scene].indices.size() > 3000;
            if (!split && !full) continue;
            const std::string name = scene->world.colliders[parts[0]].name;
            makeSurface(o, surfaceMode(o), autoMade(o));
            if (!split) for (int p : surfaceParts(o)) scene->world.colliders[p].name = name;   // (kept as you named it)
            any = true;
        }
        if (any) worldEdited = true;
    }

    // Saving is saving the environment (File > Save): there's no other copy of its edits.
    bool saveEdits() { saveToLibrary(); return true; }

    // ── The background sound (soundPanel): any sound file, looped behind the home, here and in Frame Home ──
    // It's saved beside the edits as "<home>.sound-<hash>.<ext>" (Send to Frame copies it beside the home, a .fenv
    // packs it beside its edits.json), and the edits name it with its volume and the home's own ambience's.

    void restartSounds() {
        ambience.stop();
        addedSound.stop();
        if (!scene) return;
        if (sound.s.homeVolume > 0 && !scene->ambientOgg.empty()) ambience.start(scene->ambientOgg.data(), scene->ambientOgg.size(), sound.s.homeVolume);
        if (sound.bytes && !sound.bytes->empty()) addedSound.start(sound.bytes->data(), sound.bytes->size(), sound.s.volume, false);
    }

    void loadSound(const HomeSound& s, const std::string& editsPath) {
        sound = SoundState();
        sound.s = s;
        const std::string path = soundPathFor(editsPath, s);
        sound.s.file.clear();
        if (!path.empty()) {
            std::ifstream f(fs::path(widen(path)), std::ios::binary);
            auto b = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (b->empty()) std::fprintf(stderr, "[studio] the background sound %s is missing\n", path.c_str());
            else { sound.bytes = std::move(b); sound.s.file = s.file; sound.name = s.name.empty() ? s.file : s.name; }
        }
        restartSounds();
    }

    // Writes the sound beside Studio's edits (once: its name carries its hash) and clears out this home's old ones.
    bool saveSoundFile() {
        const fs::path dir = editsFile().parent_path();
        const std::wstring prefix = widen(frameName(homePaths.front()) + ".sound-");
        std::error_code ec;
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            const std::wstring n = f.path().filename().wstring();
            if (n.rfind(prefix, 0) == 0 && n != widen(sound.s.file)) fs::remove(f.path(), ec);
        }
        if (!sound.bytes) return true;
        const fs::path out = dir / widen(sound.s.file);
        if (fs::exists(out, ec) && fs::file_size(out, ec) == sound.bytes->size()) return true;
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(sound.bytes->data()), static_cast<std::streamsize>(sound.bytes->size()));
        return static_cast<bool>(f);
    }

    void pushSoundUndo(const SoundState& before) {
        UndoStep u = {};
        u.group = undoGroup;
        u.sound = std::make_shared<SoundState>(before);
        undo.push_back(u);
        redo.clear();
        dirty = true;
        updateTitle();
    }

    // A sound file chosen (or dropped on the window) becomes the background sound.
    void addSoundFile(const std::string& path) {
        if (!scene) return;
        const fs::path fp = fs::path(widen(path));
        std::error_code ec;
        const auto size = fs::file_size(fp, ec);
        if (ec || size == 0 || size > (512ull << 20)) {
            platform::message("Frame Studio", "Couldn't read " + narrow(fp.filename().wstring()) + (size > (512ull << 20) ? " (over 512 MB)." : "."), platform::Icon::Warning);
            return;
        }
        std::ifstream f(fp, std::ios::binary);
        auto bytes = std::make_shared<std::vector<uint8_t>>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<short> pcm;
        int chs = 0, sr = 0;
        const char* fmt = "sound";
        if (bytes->empty() || !AudioPlayer::decode(bytes->data(), bytes->size(), pcm, chs, sr, &fmt)) {
            platform::message("Frame Studio", narrow(fp.filename().wstring()) + " isn't a sound Frame Studio can play.\n\nIt takes Ogg (Vorbis or Opus), WAV, MP3 and FLAC files.",
                              platform::Icon::Warning);
            return;
        }
        uint32_t h = 2166136261u;                       // FNV-1a: a new file gets a new name (the Frame sees it change)
        for (uint8_t b : *bytes) h = (h ^ b) * 16777619u;
        std::string ext = narrow(fp.extension().wstring());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        char hex[16];
        std::snprintf(hex, sizeof hex, "%08x", h);
        const SoundState before = sound;
        sound.bytes = std::move(bytes);
        sound.name = narrow(fp.filename().wstring());
        sound.s.name = sound.name;
        sound.s.file = frameName(homePaths.front()) + ".sound-" + hex + ext;
        pushSoundUndo(before);
        addedSound.stop();
        addedSound.gain = sound.s.volume;
        addedSound.startPCM(pcm.data(), pcm.size() / chs, chs, sr, false);
        const int secs = static_cast<int>(pcm.size() / chs / std::max(1, sr));
        char msg[300];
        std::snprintf(msg, sizeof msg, "Background sound: %s (%s, %d:%02d)", sound.name.c_str(), fmt, secs / 60, secs % 60);
        setStatus(msg);
    }

    void chooseSound() {
        const std::vector<std::string> picked = platform::openFiles("Background sound for this environment",
            {{"Sounds", {"*.mp3", "*.ogg", "*.oga", "*.opus", "*.wav", "*.flac"}}}, false);
        if (!picked.empty()) addSoundFile(picked.front());
    }

    // In ENVIRONMENT: how loud the home's own ambience plays, and a background sound of your own.
    void soundPanel() {
        const SoundState before = sound;
        auto percent = [&](const char* id, float& v, const char* on, const char* off, const char* tip, AudioPlayer& p) {
            int pc = static_cast<int>(std::lround(v * 100));
            ImGui::SetNextItemWidth(-1);
            const bool changed = ImGui::SliderInt(id, &pc, 0, 100, pc ? on : off);
            if (ImGui::IsItemActivated()) pushSoundUndo(before);   // a drag is one undo step
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            if (!changed) return;
            const bool wasOff = v <= 0;
            v = pc / 100.0f;
            if (wasOff && v > 0 && !p.ok) restartSounds();      // switched back on: decode it again
            else p.gain = v;
            dirty = true;
            updateTitle();
        };
        if (!scene->ambientOgg.empty())
            percent("##homevolume", sound.s.homeVolume, "Home's own sound %d%%", "Home's own sound: off",
                    "How loud the home's own ambience plays (in Frame Home too). Off: only your background sound.", ambience);
        ImGui::Spacing();
        if (sound.bytes) {
            ImGui::PushStyleColor(ImGuiCol_Text, hex(kOrange));
            ImGui::TextWrapped("♪ %s", sound.name.c_str());
            ImGui::PopStyleColor();
            if (!addedSound.ok && ImGui::IsItemHovered()) ImGui::SetTooltip("It isn't playing here (no audio device?): see studio.log");
            percent("##soundvolume", sound.s.volume, "Background sound %d%%", "Background sound: silent",
                    "How loud your background sound plays, here and in Frame Home.", addedSound);
            const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Replace…", ImVec2(w, 0))) chooseSound();
            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(w, 0))) {
                pushSoundUndo(before);
                sound.bytes.reset();
                sound.s.file.clear();
                sound.s.name.clear();
                sound.name.clear();
                addedSound.stop();
                setStatus("Background sound removed");
            }
        } else {
            if (ImGui::Button("Add Background Sound…", ImVec2(-1, 0))) chooseSound();
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Loops behind the home, on the Frame too. Ogg, Opus, WAV, MP3 or FLAC — or drop one on the window.");
            ImGui::PopTextWrapPos();
        }
    }

    void closeHome() {
        // Not mid-load: the upload steps in finishLoad still use the scene.
        if (!scene || saving || uploading || texLoading || loadState == 1 || !confirmDiscard()) return;
        if (loader.joinable() && loadState != 1) loader.join();
        vkDeviceWaitIdle(renderer.device());
        renderer.unload();
        ambience.stop();
        addedSound.stop();
        sound = SoundState();
        scene.reset();
        walkShown.reset(); walkReport = WalkReport{}; walkSig = 0; walkPending = walkSaveNext = false;
        renderer.setStaticGuide({}); walkGuideOn = false;
        objects.clear();
        undo.clear();
        redo.clear();
        homePaths.clear();
        homeName.clear();
        selected = -1;
        wKind = wIndex = -1;
        dirty = worldEdited = false;
        updateTitle();
    }

    // The start screen (nothing open): open a home, an environment or models, or pick up a recent one.
    struct RecentInfo { std::string title, kind; };
    std::map<std::string, RecentInfo> recentInfo;
    const RecentInfo& recentDetails(const std::vector<std::string>& paths) {
        std::string key;
        for (const auto& p : paths) key += p + "|";
        auto it = recentInfo.find(key);
        if (it != recentInfo.end()) return it->second;
        RecentInfo info;
        info.title = displayName(paths);
        if (paths.size() == 1 && isFenvPath(paths[0])) {
            FenvManifest m;
            if (fenvReadManifest(paths[0], m)) {
                info.title = m.name.empty() ? info.title : m.name;
                info.kind = "Frame environment" + std::string(m.scene.empty() ? "" : " \u00b7 baked") + (m.world.empty() ? "" : " \u00b7 world layer") +
                            (m.models.empty() ? "" : " \u00b7 " + std::to_string(m.models.size()) + " models");
            } else info.kind = "Frame environment (missing)";
        } else {
            size_t models = 0;
            for (const auto& p : paths) models += isModelPath(p);
            info.kind = models == paths.size() ? "Models" : paths.size() - models > 1 ? "Footprint + vista" : "Home APK";
            if (models && models < paths.size()) info.kind += " + " + std::to_string(models) + " models";
        }
        std::error_code ec;
        for (const auto& p : paths) if (!fs::exists(fs::path(widen(p)), ec)) { info.kind += " \u00b7 file missing"; break; }
        return recentInfo[key] = info;
    }

    // A row on the start screen: a tile with the kind's initial, the name, and what it is. True when clicked.
    bool startCard(ImDrawList* dl, const char* id, const ImVec2& a, float w, float h, const RecentInfo& info) {
        const ImVec2 b(a.x + w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        dl->AddRectFilled(a, b, col(held ? 0x33363c : hov ? 0x34373d : 0x2a2c30), 8 * scale);
        dl->AddRect(a, b, col(hov ? kBlue : kLine, hov ? 0.85f : 0.6f), 8 * scale, 0, 1.0f);
        // The tile: green for a Frame environment, blue for a Meta home, violet for models, grey when a file is gone.
        const bool missing = info.kind.find("missing") != std::string::npos;
        const bool env = info.kind.rfind("Frame environment", 0) == 0, models = info.kind.rfind("Models", 0) == 0;
        const uint32_t tint = missing ? 0x6b6f76 : env ? 0x3f9a5c : models ? 0x8a63d2 : 0x3f7bd6;
        const char* letter = env ? "E" : models ? "M" : "H";
        const float t = h - 16 * scale;
        const ImVec2 t0(a.x + 8 * scale, a.y + 8 * scale), t1(t0.x + t, t0.y + t);
        dl->AddRectFilled(t0, t1, col(tint, 0.22f), 6 * scale);
        ImFont* bf = fontOr(g_fontBold);
        const ImVec2 ls = bf->CalcTextSizeA(bf->FontSize, FLT_MAX, 0, letter);
        dl->AddText(bf, bf->FontSize, ImVec2((t0.x + t1.x - ls.x) * 0.5f, (t0.y + t1.y - ls.y) * 0.5f), col(tint == 0x6b6f76 ? kDim : 0xffffff, 0.9f), letter);
        const float tx = t1.x + 12 * scale, lh = ImGui::GetTextLineHeight();
        const float ty = a.y + (h - 2 * lh - 2 * scale) * 0.5f;
        dl->PushClipRect(a, ImVec2(b.x - 28 * scale, b.y), true);
        dl->AddText(ImVec2(tx, ty), col(missing ? kDim : kText), info.title.c_str());
        dl->AddText(ImVec2(tx, ty + lh + 2 * scale), col(kDim), info.kind.c_str());
        dl->PopClipRect();
        if (hov) dl->AddText(ImVec2(b.x - 22 * scale, a.y + (h - lh) * 0.5f), col(kDim), "\u203a");
        return clicked;
    }

    void startScreen() {
        // A quieter backdrop than the 3D view's grey, so the cards read as the screen's content.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(viewPos, ImVec2(viewPos.x + viewSize.x, viewPos.y + viewSize.y), col(0x25272b));
        const float w = std::min(viewSize.x - 80 * scale, 720 * scale), x0 = viewPos.x + (viewSize.x - w) * 0.5f;
        float y = viewPos.y + std::max(36 * scale, viewSize.y * 0.10f);
        ImFont* tf = fontOr(g_fontTitle);
        const float titleSize = g_fontTitle ? tf->FontSize : ImGui::GetFontSize() * 2.0f;
        dl->AddText(tf, titleSize, ImVec2(x0, y), col(kText), "Frame Studio");
        {   // the brand mark: a small orange bar under the title
            const float tw = tf->CalcTextSizeA(titleSize, FLT_MAX, 0, "Frame Studio").x;
            dl->AddRectFilled(ImVec2(x0 + 1 * scale, y + titleSize * 1.12f), ImVec2(x0 + std::min(tw, 44 * scale), y + titleSize * 1.12f + 3 * scale), col(kOrange), 2 * scale);
        }
        y += titleSize * 1.12f + 14 * scale;
        dl->AddText(ImVec2(x0, y), col(kDim), "Open a Meta home (HSL, Clay or glTF), a Frame environment, or models from Blender / Unity.");
        y += ImGui::GetTextLineHeight() + 22 * scale;
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        const float bigH = ImGui::GetFrameHeight() * 1.7f;
        const ImVec2 big(w * 0.62f, bigH), side(w - big.x - 10 * scale, bigH);
        pushPrimary();
        if (ImGui::Button("Open Home, Environment or Model\u2026", big)) openDialog();
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ctrl+O \u00b7 or drop files on the window");
        ImGui::SameLine(0, 10 * scale);
        if (ImGui::Button("Controls (F1)", side)) showControls = true;
        y += big.y + 30 * scale;
        const float rowH = ImGui::GetTextLineHeight() * 2.0f + 22 * scale;
        auto listTitle = [&](const char* t) {
            ImGui::SetCursorScreenPos(ImVec2(x0, y));
            caption(t);
        };
        {   // My Environments: what you've made, newest first
            listTitle("MY ENVIRONMENTS");
            const char* folder = "Show the Folder";
            ImGui::SetCursorScreenPos(ImVec2(x0 + w - ImGui::CalcTextSize(folder).x - ImGui::GetStyle().FramePadding.x * 2, y - 1 * scale));
            if (ImGui::SmallButton(folder)) platform::openPath(narrow(fs::path(libraryDir().c_str()).wstring()));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", narrow(libraryDir().wstring()).c_str());
            y += ImGui::GetTextLineHeight() + 10 * scale;
            static std::vector<std::string> mine;
            static auto listed = std::chrono::steady_clock::time_point();
            if (std::chrono::steady_clock::now() - listed > std::chrono::seconds(2)) { mine = libraryEnvs(); listed = std::chrono::steady_clock::now(); }
            if (mine.empty()) {
                dl->AddText(ImVec2(x0, y), col(0x6b6f76), "Nothing yet. Open a home, make it yours, and Save (Ctrl+S): it's kept here.");
                y += ImGui::GetTextLineHeight() + 22 * scale;
            }
            for (size_t r = 0; r < mine.size() && r < 6; ++r) {
                const RecentInfo& info = recentDetails({mine[r]});
                ImGui::PushID(static_cast<int>(r) + 7000);
                const bool clicked = startCard(dl, "##mine", ImVec2(x0, y), w, rowH, info);
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hov) ImGui::SetTooltip("%s", mine[r].c_str());
                if (clicked) { open({mine[r]}); return; }
                y += rowH + 6 * scale;
            }
            y += 18 * scale;
        }
        listTitle("RECENT");
        y += ImGui::GetTextLineHeight() + 10 * scale;
        if (recent.empty()) {
            dl->AddText(ImVec2(x0, y), col(0x6b6f76), "Nothing yet. Homes and environments you open show up here.");
            return;
        }
        for (size_t r = 0; r < recent.size(); ++r) {
            const RecentInfo& info = recentDetails(recent[r]);
            ImGui::PushID(static_cast<int>(r) + 9000);
            const bool clicked = startCard(dl, "##recent", ImVec2(x0, y), w, rowH, info);
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) {
                std::string tip;
                for (const auto& p : recent[r]) tip += (tip.empty() ? "" : "\n") + p;
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (clicked) { const auto paths = recent[r]; open(paths); }
            y += rowH + 6 * scale;
            if (y > viewPos.y + viewSize.y - rowH) break;
        }
    }

    // Adds Blender / Unity models to the open home (it reloads with them; edits are kept by index).
    void importModelDialog() {
        if (!scene) return;
        if (homePaths.size() == 1 && isFenvPath(homePaths[0])) {
            platform::message("Frame Studio", "This environment is a saved .fenv. Open its original home to add models, or add models to a new environment.");
            return;
        }
        const std::vector<std::string> picked = platform::openFiles("Add models from Blender or Unity", {{"Models", {"*.glb", "*.gltf", "*.fbx", "*.obj"}}}, true);
        if (picked.empty()) return;
        std::vector<std::string> paths = homePaths;
        paths.insert(paths.end(), picked.begin(), picked.end());
        carryAcrossReload();
        dirty = false;                                                 // (carried: nothing to ask about)
        open(paths, false);
    }

    // File > Save (Ctrl+S): the environment, into its own file: an environment opened saves back into itself; a Meta
    // home or models opened is a new one, saved into My Environments under its name the first time.
    std::string envFile;                    // the .fenv it saves to ("" = new: named on its first save)
    void saveToLibrary() {
        if (saving) { saveQueued = true; return; }                     // after the one under way (an auto-save)
        if (!scene || uploading) return;
        if (envFile.empty()) {
            std::wstring name = widen(shownName());
            for (wchar_t& c : name) if (std::wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'-';
            fs::path p = libraryDir() / (name + L".fenv");
            for (int n = 2; fs::exists(p); ++n) p = libraryDir() / (name + L" " + std::to_wstring(n) + L".fenv");
            envFile = narrow(p.wstring());
        }
        saveEnvironment(envFile);
        dirty = false;                                                 // (back on if the save fails)
        updateTitle();
    }
    // Auto-save: every 5 minutes with unsaved changes, a version of it in My Environments\Versions\<name>\, the last
    // 10 kept. The environment's own file only changes when you save it.
    bool saveQueued = false;
    std::chrono::steady_clock::time_point autosavedAt;
    fs::path versionsDir() const {
        std::wstring name = widen(shownName());
        for (wchar_t& c : name) if (std::wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'-';
        return libraryDir() / L"Versions" / name;
    }
    std::vector<fs::path> versions() const {
        std::vector<fs::path> v;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(versionsDir(), ec))
            if (e.is_regular_file(ec) && e.path().extension() == L".fenv") v.push_back(e.path());
        std::sort(v.rbegin(), v.rend());                               // (named by time: newest first)
        return v;
    }
    void autosaveVersion() {
        std::error_code ec;
        fs::create_directories(versionsDir(), ec);
        const std::time_t t = std::time(nullptr);
        const std::tm tm = platform::localTime(t);
        wchar_t stamp[64];
        std::wcsftime(stamp, 64, L"%Y-%m-%d %H-%M-%S", &tm);
        saveEnvironment(narrow((versionsDir() / (std::wstring(stamp) + L".fenv")).wstring()), false);
        const std::vector<fs::path> v = versions();
        for (size_t i = 10; i < v.size(); ++i) fs::remove(v[i], ec);
    }

    // The environments in My Environments, newest first.
    std::vector<std::string> libraryEnvs() const {
        std::vector<std::pair<fs::file_time_type, std::string>> v;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(libraryDir(), ec))
            if (e.is_regular_file(ec) && e.path().extension() == L".fenv") v.push_back({e.last_write_time(ec), narrow(e.path().wstring())});
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> out;
        for (auto& x : v) out.push_back(std::move(x.second));
        return out;
    }

    // File > Save As: the open home with its edits, world layer, models and a thumbnail, as one .fenv anywhere.
    void saveEnvironmentDialog() {
        if (!scene || saving || uploading) return;
        std::string name = shownName();
        for (char& c : name) if (c == '+' || c == '/' || c == '\\' || c == ':') c = '-';
        const std::string path = platform::saveFile("Save as a Frame environment", name + ".fenv", {{"Frame environment", {"*.fenv"}}}, "fenv");
        if (path.empty()) return;
        envFile = path;                                                // Save goes there from now on
        saveEnvironment(envFile);
        dirty = false;
        updateTitle();
    }

    // remember: list it under Open Recent (not for the installer's own copies).
    void saveEnvironment(const std::string& out, bool remember = true) {
        // Everything the file needs is gathered and baked here, on the UI thread: the worker only packs and writes,
        // from its own copies, so editing, loading another home or closing this one meanwhile can't pull the scene
        // out from under it. Baking is a memory copy (well under a second); packing is the slow part.
        HomeEdits e;
        e.home = narrow(fs::path(widen(out)).filename().wstring());
        e.objects = currentObjectEdits();
        e.image = envImage;
        e.sound = sound.s;                               // the background sound rides with the edits
        e.title = envTitle;
        const fs::path tmpEdits = dataDir() / L"edits" / L"_fenv_edits.json";
        const bool hasEdits = !e.empty() && saveHomeEdits(narrow(tmpEdits.wstring()), e);
        if (!e.empty() && !hasEdits) {   // never report "Saved" for a file missing its edits
            std::lock_guard<std::mutex> l(statusMutex);
            saveStatus = "Couldn't save: the object edits couldn't be written to " + narrow(tmpEdits.wstring());
            return;
        }
        std::vector<u8> thumb = imageThumb();             // the picture chosen for it, else the view from where you start
        if (thumb.empty()) thumb = spawnThumb();
        if (saver.joinable()) saver.join();
        saving = true;
        { std::lock_guard<std::mutex> l(statusMutex); saveStatus = "Saving " + narrow(fs::path(widen(out)).filename().wstring()) + "\u2026"; }
        const std::string name = shownName(), worldJson = worldToJson(scene->world), editsPath = hasEdits ? narrow(tmpEdits.wstring()) : std::string();
        const WorldSettings settings = worldSettings;
        const FenvPacking packing = fastPack ? FenvPacking::Fast : FenvPacking::Smallest;
        std::string bakeErr;
        std::vector<u8> blob;
        // The mesh optimizer (what nobody can see from anywhere they can be is left out) takes a while: it runs on the
        // worker, on a copy of the scene taken now, and bakes there; otherwise the bake is here (a memory copy).
        std::shared_ptr<HslScene> toOptimize;
        std::vector<ObjectEdit> objectEdits = e.objects;
        bool baked = false;
        if (optimizeMeshes) {
            toOptimize = std::make_shared<HslScene>(*scene);
            // A Clay / glTF home's animation: read here (this thread plays it), the copy keeps that and plays nothing
            if (scene->saveAnimation) {
                std::vector<u8> anim;
                scene->saveAnimation(anim);
                toOptimize->saveAnimation = [anim = std::move(anim)](std::vector<u8>& out) { out = anim; };
                toOptimize->animate = nullptr;
            }
            baked = true;
        } else {
            baked = bakeScene(*scene, blob, bakeErr);
        }
        std::vector<std::string> apkPaths = scene->apkPaths;
        std::vector<HslSourceInfo> sources = scene->sources;
        std::vector<std::vector<std::string>> sourceFiles = scene->sourceFiles;
        std::shared_ptr<const std::vector<uint8_t>> soundBytes = sound.bytes;
        const std::string soundFile = sound.s.file;
        saver = std::thread([this, out, name, worldJson, editsPath, settings, thumb, packing, remember, baked, soundBytes, soundFile, blob = std::move(blob),
                             apkPaths = std::move(apkPaths), sources = std::move(sources), sourceFiles = std::move(sourceFiles), toOptimize,
                             objectEdits = std::move(objectEdits)]() mutable {
            if (toOptimize) {
                { std::lock_guard<std::mutex> l(statusMutex); saveStatus = "Saving: finding what can't be seen from anywhere you can be\u2026"; }
                VisibilityResult vis;
                BakeOptions opt;
                animbake::toStart(*toOptimize);   // its animated meshes as a load has them (the optimizer reads the vertices)
                const bool optimized = optimizeForBake(*toOptimize, objectEdits.empty() ? nullptr : &objectEdits, vis, opt);
                std::string err;
                if (!bakeScene(*toOptimize, blob, err, optimized ? &opt : nullptr)) baked = false;
                toOptimize.reset();
                if (optimized)
                    std::fprintf(stderr, "save: %zu of %zu triangles can't be seen from anywhere you can be: left out (%zu meshes emptied, %.1f s)\n",
                                 vis.trisBefore - vis.trisAfter, vis.trisBefore, vis.meshesEmptied, vis.ms / 1000);
            }
            FenvManifest m;
            m.name = name;
            m.settings = settings;
            std::vector<FenvMember> members;
            std::string err;
            if (baked) { FenvMember b; b.name = m.scene = "scene.bin"; b.bytes = std::move(blob); b.lzma = true; b.packing = packing; members.push_back(std::move(b)); }
            for (size_t i = 0; i < apkPaths.size(); ++i) {
                const HslSourceInfo info = i < sources.size() ? sources[i] : HslSourceInfo{};
                const std::string& path = apkPaths[i];
                if (info.role == "Model") {
                    if (baked) continue;
                    const fs::path base = fs::u8path(path).parent_path();
                    const std::string dir = "models/" + fs::u8path(path).stem().u8string() + "/";
                    const std::vector<std::string> files = i < sourceFiles.size() && !sourceFiles[i].empty() ? sourceFiles[i] : std::vector<std::string>{path};
                    for (const std::string& f : files) {
                        std::error_code ec;
                        fs::path rel = fs::relative(fs::u8path(f), base, ec);
                        if (ec || rel.empty() || rel.u8string().rfind("..", 0) == 0) rel = fs::u8path(f).filename();
                        FenvMember mm; mm.name = dir + rel.generic_u8string(); mm.file = f;
                        if (fs::equivalent(fs::u8path(f), fs::u8path(path), ec)) m.models.push_back(mm.name);
                        members.push_back(mm);
                    }
                    continue;
                }
                FenvSource src;
                src.file = "apks/" + fs::u8path(path).filename().u8string();
                src.role = info.role; src.format = info.format; src.package = info.package; src.version = info.version;
                m.sources.push_back(src);
                if (!baked) { FenvMember a; a.name = src.file; a.file = path; a.store = true; members.push_back(a); }
            }
            if (!baked) {   // a baked scene carries the world layer itself (in binary): no second, text copy
                FenvMember wj; wj.name = m.world = "world.json"; wj.bytes.assign(worldJson.begin(), worldJson.end()); members.push_back(std::move(wj));
            }
            if (!editsPath.empty()) { FenvMember ed; ed.name = m.edits = "edits.json"; ed.file = editsPath; members.push_back(ed); }
            if (soundBytes && !soundFile.empty()) {   // beside edits.json, which names it
                FenvMember snd;
                snd.name = soundFile;
                snd.bytes = *soundBytes;
                snd.store = soundFile.size() < 4 || soundFile.compare(soundFile.size() - 4, 4, ".wav") != 0;   // already compressed
                members.push_back(std::move(snd));
            }
            if (!thumb.empty()) { FenvMember t; t.name = m.thumbnail = "thumb.png"; t.bytes = thumb; t.store = true; members.push_back(std::move(t)); }
            const std::string file = fs::u8path(out).filename().u8string();
            std::atomic<int> shown{-1};          // called from the packing threads
            const bool ok = fenvWrite(out, m, members, err, [&](size_t done, size_t total) {
                const int pct = total ? static_cast<int>(done * 100 / total) : 0;
                if (shown.exchange(pct) == pct) return;
                std::lock_guard<std::mutex> l(statusMutex);
                saveStatus = "Saving " + file + "\u2026 " + std::to_string(pct) + "%";
            });
            std::error_code ec;
            const double mb = ok ? fs::file_size(fs::u8path(out), ec) / 1048576.0 : 0;
            char msg[512];
            if (ok) std::snprintf(msg, sizeof msg, "Saved %s (%.0f MB, %s)", fs::u8path(out).filename().u8string().c_str(), mb, baked ? "baked: opens fast" : "with its homes");
            else std::snprintf(msg, sizeof msg, "Couldn't save: %s", err.c_str());
            if (!ok) std::fprintf(stderr, "[studio] %s (%s)\n", msg, out.c_str());
            { std::lock_guard<std::mutex> l(statusMutex); saveStatus = msg; }
            if (ok && remember) savedEnvironment = out;
            if (!ok && remember) saveFailed = true;
            saving = false;
        });
    }

    // Headset > Send to Frame: the environment as it is now (saved or not), as one file, becomes the headset's current one.
    void sendToFrame() {
        if (!scene || sending || uploading) return;
        sendAsOneFile();
    }

    // Bytes (a sound, a file of the home), saved to a file of your choice under `name`.
    void saveSoundAs(const std::vector<uint8_t>& bytes, std::string name) {
        for (char& c : name) if (std::strchr("<>:\"/\\|?*", c)) c = '-';
        const std::string ext = narrow(fs::path(widen(name)).extension().wstring());   // ".ogg"
        std::vector<platform::FileFilter> filters;
        if (ext.size() > 1) filters.push_back({ext.substr(1) + " file", {"*" + ext}});
        const std::string path = platform::saveFile("Save as", name, filters, ext.size() > 1 ? ext.substr(1) : std::string());
        if (path.empty()) return;
        std::ofstream f(fs::path(widen(path)), std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        setStatus(f ? "Saved " + narrow(fs::path(widen(path)).filename().wstring()) : "Couldn't save the sound");
    }

    // ── Its name and picture: what the headset's Environments panel shows for it (a .fenv's name and thumbnail). The
    //    picture is copied beside the edits as "<home>.image-<hash>.<ext>", like the background sound. ──
    // ── Its own settings on the headset (world_settings.h), carried in the .fenv: by hand, or Calculate Best Settings
    //    (the world sent to the headset, walked there at candidate settings by `fenv-tool tune`, the best kept here). ──
    WorldSettings worldSettings;
    std::thread tuner;
    std::atomic<bool> tuning{false}, stopTune{false};
    std::string tuneStatus;                 // (statusMutex)
    void setTuneStatus(const std::string& s) { std::lock_guard<std::mutex> l(statusMutex); tuneStatus = s; }
    void calculateBestSettings() {
        if (!scene || tuning || saving) return;
        if (tuner.joinable()) tuner.join();
        std::error_code ec;
        fs::create_directories(dataDir() / L"install", ec);
        const std::string fenv = narrow((dataDir() / L"install" / L"_tune.fenv").wstring());
        fs::remove(fs::path(widen(fenv)), ec);
        const WorldSettings keep = worldSettings;
        worldSettings = WorldSettings();                               // measured as the headset's settings alone
        saveEnvironment(fenv, false);
        worldSettings = keep;
        tuning = true;
        const std::string host8 = hostArg();
        tuner = std::thread([this, fenv, host8] {
            const std::wstring ssh = sshTool(L"ssh") + L"-o BatchMode=yes -o ConnectTimeout=8 " + widen(host8) + L" ";
            const std::wstring scp = sshTool(L"scp") + L"-q -o BatchMode=yes -o ConnectTimeout=8 ";
            setTuneStatus("Baking the world to measure…");
            while (saving) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const fs::path f = fs::path(widen(fenv));
            std::error_code ec;
            if (!fs::exists(f, ec)) { setTuneStatus("Couldn't bake the world to measure it"); tuning = false; return; }
            std::string out;
            setTuneStatus("Copying it to the headset\u2026");
            int rc = run(ssh + L"\"mkdir -p /tmp/frame-tune && rm -f /tmp/frame-tune/log\"", L"", out);
            if (rc != 0) { setTuneStatus("Can't reach the headset (" + host8 + "): " + lastLine(out)); tuning = false; return; }
            out.clear();
            rc = run(scp + L"\"" + f.filename().wstring() + L"\" " + widen(host8) + L":/tmp/frame-tune/tune.fenv", f.parent_path().wstring(), out);
            fs::remove(f, ec);
            if (rc != 0) { setTuneStatus("Copy failed: " + lastLine(out)); tuning = false; return; }
            // Measured on the headset (a few minutes: each try is a walk through the world, the headset cooled first),
            // in the background there, its last line read every few seconds.
            out.clear();
            rc = run(ssh + L"\"nohup sh -c 'frame-home/bin/fenv-tool tune --dry-run /tmp/frame-tune/tune.fenv > /tmp/frame-tune/log 2>&1; echo @@done >> /tmp/frame-tune/log' > /dev/null 2>&1 &\"", L"", out);
            if (rc != 0) { setTuneStatus("Couldn't start measuring: " + lastLine(out)); tuning = false; return; }
            std::string log;
            for (int i = 0; i < 600 && !stopTune; ++i) {   // (30 minutes at most)
                std::this_thread::sleep_for(std::chrono::seconds(3));
                log.clear();
                if (run(ssh + L"\"cat /tmp/frame-tune/log\"", L"", log) != 0) continue;
                if (log.find("@@done") != std::string::npos) break;
                const size_t t = log.rfind("tune: ");
                if (t != std::string::npos) setTuneStatus("On the headset: " + log.substr(t + 6, log.find('\n', t) - t - 6));
            }
            out.clear();
            run(ssh + L"\"rm -rf /tmp/frame-tune\"", L"", out);
            const size_t j = log.rfind("{\"settings\":");
            WorldSettings w;
            bool ok = false;
            if (j != std::string::npos) {
                try {
                    const tinyjson::Value v = tinyjson::parse(log.substr(j, log.find('\n', j) - j));
                    ok = w.fromValue(v["settings"]);
                } catch (...) {}
            }
            if (!ok) {
                setTuneStatus(log.find("usage:") != std::string::npos ? "The headset's Frame Home is too old to measure: install it again (Install on Steam Frame)"
                                                                      : "Measuring didn't finish: " + lastLine(log));
                tuning = false;
                return;
            }
            {
                std::lock_guard<std::mutex> l(statusMutex);
                tunedResult = w;
                tunedReady = true;
                tuneStatus = "Best on the headset: " + w.describe() + (w.tuned.empty() ? "" : "  \u00b7  " + w.tuned);
            }
            tuning = false;
        });
    }
    WorldSettings tunedResult;              // (statusMutex) taken up by the UI thread
    bool tunedReady = false;

    std::string envTitle;                   // "" = the home's own name
    std::string envImage;                   // file name in the edits folder ("" = a view of it, taken when saved)
    std::string shownName() const { return envTitle.empty() ? homeName : envTitle; }
    void chooseImage() {
        const std::vector<std::string> picked = platform::openFiles("A picture for this environment",
            {{"Pictures", {"*.png", "*.jpg", "*.jpeg", "*.bmp"}}}, false);
        if (picked.empty()) return;
        const fs::path src(widen(picked.front()));
        std::ifstream in(src, std::ios::binary);
        const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, c = 0;
        unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 4);
        if (!px) { platform::message("Frame Studio", "That isn't a picture Frame Studio can read: " + narrow(src.filename().wstring()), platform::Icon::Warning); return; }
        stbi_image_free(px);
        uint32_t hsh = 2166136261u;
        for (u8 b : bytes) hsh = (hsh ^ b) * 16777619u;
        char hx[16];
        std::snprintf(hx, sizeof hx, "%08x", hsh);
        std::string ext = narrow(src.extension().wstring());
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::string name = frameName(homePaths.front()) + ".image-" + hx + ext;
        std::error_code ec;
        fs::create_directories(editsFile().parent_path(), ec);
        std::ofstream out(editsFile().parent_path() / fs::u8path(name), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) { platform::message("Frame Studio", "Couldn't copy the picture beside the edits.", platform::Icon::Warning); return; }
        envImage = name;
        dirty = true;
        updateTitle();
    }
    // A .fenv's thumbnail when it has no picture of its own: the home as you first see it in Frame Home, from the first
    // spawn (standing on the floor under it, eyes 1.6 m up, facing its way, a little down). One frame drawn from there
    // without the selection or the walkable area, read back at 512 px wide as PNG; the view is put back after. No
    // spawn: the view as it is.
    std::vector<u8> spawnThumb() {
        std::vector<u8> png;
        const Camera kept = cam;
        if (scene && !scene->world.spawns.empty()) {
            const WorldPoint& sp = scene->world.spawns[0];
            float floor = sp.pos[1];
            float f;
            if (floorUnder(sp.pos[0], sp.pos[2], sp.pos[1] + 0.5f, f)) floor = f;
            cam.yaw = sp.yaw;
            cam.pitch = -5.0f * kPi / 180.0f;
            cam.dist = 0.5f;
            float fwd[3];
            cam.forward(fwd);
            const float eye[3] = {sp.pos[0], floor + 1.6f, sp.pos[2]};
            for (int k = 0; k < 3; ++k) cam.pivot[k] = eye[k] + fwd[k] * cam.dist;
            renderer.setSelection(-1);
            if (walkGuideOn) { renderer.setStaticGuide({}); walkGuideOn = false; walkGuideDirty = true; }   // (back next frame)
            renderViewport();
        }
        std::vector<u8> rgba;
        const uint32_t w = renderer.width(), h = renderer.height();
        if (rendered && w > 8 && h > 8 && renderer.readback(0, rgba) && rgba.size() >= static_cast<size_t>(w) * h * 4) {
            const uint32_t tw = 512, th = std::max<uint32_t>(1, h * tw / w);
            std::vector<u8> small(static_cast<size_t>(tw) * th * 4);
            for (uint32_t y = 0; y < th; ++y)
                for (uint32_t x = 0; x < tw; ++x) {
                    const u8* src = &rgba[(static_cast<size_t>(y * h / th) * w + x * w / tw) * 4];
                    u8* dst = &small[(static_cast<size_t>(y) * tw + x) * 4];
                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
                }
            stbi_write_png_to_func([](void* ctx, void* data, int size) {
                auto* v = static_cast<std::vector<u8>*>(ctx);
                v->insert(v->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
            }, &png, tw, th, 4, small.data(), tw * 4);
        }
        cam = kept;
        return png;
    }

    // The picture as the .fenv's thumbnail: 512 px wide, PNG (empty: none chosen, or it doesn't read).
    std::vector<u8> imageThumb() const {
        std::vector<u8> png;
        if (envImage.empty()) return png;
        std::ifstream in(editsFile().parent_path() / fs::u8path(envImage), std::ios::binary);
        const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, c = 0;
        unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 4);
        if (!px) return png;
        const int tw = std::min(512, w), th = std::max(1, h * tw / std::max(1, w));
        std::vector<u8> small(static_cast<size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {   // box filter over the source pixels each covers
                const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw), y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
                uint32_t acc[4] = {}, n = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx, ++n)
                        for (int k = 0; k < 4; ++k) acc[k] += px[(static_cast<size_t>(yy) * w + xx) * 4 + k];
                for (int k = 0; k < 4; ++k) small[(static_cast<size_t>(y) * tw + x) * 4 + k] = static_cast<u8>(k == 3 ? 255 : acc[k] / n);
            }
        stbi_image_free(px);
        stbi_write_png_to_func([](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<u8>*>(ctx);
            v->insert(v->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
        }, &png, tw, th, 4, small.data(), tw * 4);
        return png;
    }

    // A footprint with its vista goes to the Frame as one home in one file: the pair baked into a .fenv with the
    // edits, world layer and background sound (what Save Environment writes). The pair's separate copies an earlier
    // Send left there go, so the headset lists it once.
    void sendAsOneFile() {
        if (saving) { setStatus("Wait for the environment being saved to finish, then send."); return; }
        if (sender.joinable()) sender.join();
        std::error_code ec;
        fs::create_directories(dataDir() / L"install", ec);
        const std::string fenv = narrow((dataDir() / L"install" / widen(frameName(homeName + ".fenv"))).wstring());
        fs::remove(fs::path(widen(fenv)), ec);                         // a failed save must not send the last one
        saveEnvironment(fenv, false);
        sending = true;
        std::vector<std::string> old;                                  // the pair as separately sent before
        for (const auto& p : homePaths) old.push_back(frameName(p));
        const std::string editsName = frameName(homePaths.front()), host8 = hostArg();
        sender = std::thread([this, fenv, old, editsName, host8] {
            const std::wstring ssh = sshTool(L"ssh") + L"-o BatchMode=yes -o ConnectTimeout=8 ";
            const std::wstring scp = sshTool(L"scp") + L"-q -o BatchMode=yes -o ConnectTimeout=8 ";
            const std::wstring host = widen(host8), dir = widen(kFrameEnvDir);
            setStatus("Baking the home and its vista into one file\u2026");
            while (saving) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const fs::path f = fs::path(widen(fenv));
            if (!fs::exists(f)) { setStatus("Not sent: the home couldn't be saved as one file"); sending = false; return; }
            const std::string name = frameName(fenv);
            const std::wstring wname = widen(name);
            std::string out;
            setStatus("Connecting to the Frame\u2026");
            int rc = run(ssh + host + L" \"mkdir -p " + dir + L"\"", L"", out);
            if (rc != 0) { setStatus("Can't reach the Frame: " + lastLine(out)); sending = false; return; }
            std::error_code ec2;
            char msg[200];
            std::snprintf(msg, sizeof msg, "Copying %s (%.0f MB)\u2026", name.c_str(), fs::file_size(f, ec2) / 1048576.0);
            setStatus(msg);
            out.clear();
            rc = run(scp + L"\"" + f.filename().wstring() + L"\" " + host + L":" + dir + L"/" + wname + L".part", f.parent_path().wstring(), out);
            if (rc == 0) rc = run(ssh + host + L" \"mv -f " + dir + L"/" + wname + L".part " + dir + L"/" + wname + L"\"", L"", out);
            if (rc != 0) { setStatus("Copy failed: " + lastLine(out)); sending = false; return; }
            std::wstring rm = L"rm -f";
            for (const auto& o : old) if (o != name) rm += L" " + dir + L"/'" + widen(o) + L"'";
            rm += L" " + dir + L"/'" + widen(editsName) + L".edits.json' " + dir + L"/'" + widen(editsName) + L".world.json' " + dir + L"/'" + widen(editsName) + L"'.sound-*";
            out.clear();
            run(ssh + host + L" \"" + rm + L"\"", L"", out);
            out.clear();
            rc = run(ssh + host + L" \"printf '" + wname + L"\\n' > frame-home/current\"", L"", out);
            if (rc != 0) { setStatus("Couldn't set the current home: " + lastLine(out)); sending = false; return; }
            setStatus("On the Frame: " + name + (old.size() > 1 ? " (the room and its vista, one file)" : " (one file)"));
            sending = false;
        });
    }

    // ── Editing ──

    void apply(Object& o) {
        float w[16];
        mul(o.pivot, o.offset, w);
        renderer.setMeshWorld(o.mesh, w);
        renderer.setMeshVisible(o.mesh, objectShown(o));
        followSurface(o);
    }

    bool objectShown(const Object& o) const {
        if (o.hidden || o.deleted) return false;
        if (isolating) return static_cast<int>(&o - objects.data()) == isoObject;
        return showMeshes;
    }

    void refreshVisibility() {
        for (const Object& o : objects) renderer.setMeshVisible(o.mesh, objectShown(o));
    }

    // View > Show Only Selected: the selected object or world item alone, until pressed again.
    void toggleIsolate() {
        if (isolating) isolating = false;
        else if (selected >= 0) { isolating = true; isoObject = selected; isoKind = isoIndex = -1; }
        else if (wKind >= 0 && wIndex >= 0) { isolating = true; isoObject = -1; isoKind = wKind; isoIndex = wIndex; }
        refreshVisibility();
    }

    // ── Surfaces made from the home's objects (floors, walls, wall + floor) ──
    // A mesh surface keeps the object's own triangles (the ones its role uses) in the object's frame, so it
    // follows the object when moved.

    static std::string objectKey(const Object& o) { return std::to_string(o.scene) + ":" + o.name; }

    std::vector<int> surfaceParts(const Object& o) const {
        std::vector<int> v;
        if (!scene) return v;
        const std::string key = objectKey(o);
        const auto& cs = scene->world.colliders;
        for (size_t i = 0; i < cs.size(); ++i)
            if (cs[i].kind == WorldCollider::Mesh && cs[i].mesh == key) v.push_back(static_cast<int>(i));
        return v;
    }

    int surfaceIndex(const Object& o) const { const std::vector<int> v = surfaceParts(o); return v.empty() ? -1 : v[0]; }

    // The object's SurfaceMode, read off the roles of its parts.
    int surfaceMode(const Object& o) const {
        const std::vector<int> parts = surfaceParts(o);
        if (parts.empty()) return kSurfNone;
        for (int i : parts) if (scene->world.colliders[i].role == WorldCollider::Trigger) return kSurfTrigger;
        for (int i : parts) if (scene->world.colliders[i].role == WorldCollider::Object) return kSurfObject;
        return kSurfSolid;
    }

    bool autoMade(const Object& o) const {
        for (int i : surfaceParts(o)) if (scene->world.colliders[i].name.rfind(kAutoPrefix, 0) == 0) return true;
        return false;
    }

    // Opaque, solid, not a distant backdrop or the sky.
    bool canBeSurface(const Object& o) const {
        if (!scene || o.deleted || o.skybox || o.scene < 0 || o.scene >= static_cast<int>(scene->meshes.size())) return false;
        const MeshData& md = scene->meshes[o.scene];
        if (md.useBlend || md.additive || md.overlayKind || md.indices.empty() || md.positions.empty()) return false;
        float mn[3], mx[3];
        worldBounds(o, mn, mx);
        return mx[0] - mn[0] < 200 && mx[1] - mn[1] < 200 && mx[2] - mn[2] < 200;
    }

    // Each triangle of an object in world space: f(offset into its index list, a, b, c, unit front normal, area).
    template <typename F> void worldTris(const Object& o, F&& f) const {
        const MeshData& md = scene->meshes[o.scene];
        float m[16];
        mul(o.pivot, o.offset, m);
        const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
        const float flip = det < 0 ? -1.0f : 1.0f;      // a mirrored object winds the other way round
        const size_t nv = md.positions.size() / 3;
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            const u32 ia = md.indices[t], ib = md.indices[t + 1], ic = md.indices[t + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            float a[3], b[3], c[3];
            point(m, &md.positions[ia * 3], a); point(m, &md.positions[ib * 3], b); point(m, &md.positions[ic * 3], c);
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (L < 1e-12f) continue;
            for (float& v : n) v *= flip / L;
            f(t, a, b, c, n, 0.5f * L);
        }
    }

    // Which way the home's triangles wind (Studio draws both sides, so nothing else says): +1 when (b-a)x(c-a)
    // points out of a face's front, -1 the other way, 0 unknown (up and down faces then count alike). Read once per
    // home off its main floor, the lowest big level surface: it is only ever seen from above. (The biggest one can
    // be the ceiling, seen from below.)
    int windSign = 2;                       // 2: not read yet
    int windingSign() {
        if (windSign != 2) return windSign;
        if (!scene || objects.empty()) return 0;
        std::map<int, std::pair<double, double>> levels;   // 10 cm bins: area facing up, down
        for (const Object& o : objects) {
            if (o.hidden || !canBeSurface(o) || o.category == kPlants) continue;
            worldTris(o, [&](size_t, const float* a, const float* b, const float* c, const float* n, float area) {
                if (std::fabs(n[1]) < 0.95f) return;
                auto& l = levels[static_cast<int>(std::floor((a[1] + b[1] + c[1]) / 3 / 0.1f))];
                (n[1] > 0 ? l.first : l.second) += area;
            });
        }
        double big = 0;
        for (const auto& kv : levels) big = std::max(big, kv.second.first + kv.second.second);
        windSign = 0;
        for (const auto& kv : levels)
            if (kv.second.first + kv.second.second >= big * 0.25) {
                if (kv.second.first > 2 * kv.second.second) windSign = 1;
                else if (kv.second.second > 2 * kv.second.first) windSign = -1;
                break;
            }
        return windSign;
    }

    // Floor (1), wall (2) or neither (0, facing down) for a world-space front normal.
    static int faceClass(const float* n, int sign) {
        const float up = sign ? n[1] * sign : std::fabs(n[1]);
        if (up > kFloorCos) return 1;
        return std::fabs(n[1]) < kFloorCos ? 2 : 0;
    }

    void classifyObject(const Object& o, std::vector<u32>& floor, std::vector<u32>& wall) {
        const int sign = windingSign();
        worldTris(o, [&](size_t t, const float*, const float*, const float*, const float* n, float) {
            const int k = faceClass(n, sign);
            if (k == 1) floor.push_back(static_cast<u32>(t)); else if (k == 2) wall.push_back(static_cast<u32>(t));
        });
    }

    // A linked mesh surface of some of an object's triangles (offsets into its index list), vertices compacted.
    WorldCollider surfacePart(const Object& o, const std::vector<u32>& tris, int role, const std::string& name, bool simplify = true) const {
        const MeshData& md = scene->meshes[o.scene];
        WorldCollider c;
        c.name = name;
        c.kind = WorldCollider::Mesh;
        c.role = role;
        c.mesh = objectKey(o);
        mul(o.pivot, o.offset, c.world);
        std::vector<u32> remap(md.positions.size() / 3, UINT32_MAX);
        c.indices.reserve(tris.size() * 3);
        for (u32 t : tris)
            for (int k = 0; k < 3; ++k) {
                const u32 v = md.indices[t + k];
                if (remap[v] == UINT32_MAX) { remap[v] = static_cast<u32>(c.positions.size() / 3); c.positions.insert(c.positions.end(), &md.positions[v * 3], &md.positions[v * 3] + 3); }
                c.indices.push_back(remap[v]);
            }
        if (simplify) simplifySurfaceMesh(c.positions, c.indices, surfaceCellFor(c.world));   // for walking on, not for looking at
        return c;
    }

    std::string partName(const Object& o, bool autoName, bool collider = false) const {
        return autoName ? std::string(kAutoPrefix) + (collider ? "collider · " : "surface · ") + o.label : o.label;
    }

    // Gives an object its surface for `mode`, in place of any it had (kept at its place in the list). No undo step:
    // callers take one for the whole batch.
    void makeSurface(const Object& o, int mode, bool autoName = false) {
        HomeWorld& w = scene->world;
        const std::vector<int> old = surfaceParts(o);
        bool hidden = false;
        for (int i : old) hidden |= w.colliders[i].hidden;
        const size_t at = old.empty() ? w.colliders.size() : static_cast<size_t>(old[0]);
        for (auto it = old.rbegin(); it != old.rend(); ++it) w.colliders.erase(w.colliders.begin() + *it);
        if (mode == kSurfNone) return;
        std::vector<WorldCollider> parts;
        std::vector<u32> all;
        for (size_t t = 0; t + 2 < scene->meshes[o.scene].indices.size(); t += 3) all.push_back(static_cast<u32>(t));
        const int role = mode == kSurfTrigger ? WorldCollider::Trigger : mode == kSurfObject ? WorldCollider::Object : WorldCollider::Solid;
        parts.push_back(surfacePart(o, all, role, mode == kSurfTrigger ? o.label : partName(o, autoName, mode == kSurfObject)));
        for (WorldCollider& c : parts) c.hidden = hidden;
        w.colliders.insert(w.colliders.begin() + static_cast<std::ptrdiff_t>(std::min(at, w.colliders.size())), parts.begin(), parts.end());
    }

    void setSurfaces(const std::vector<size_t>& objs, int mode) {
        if (!scene) return;
        pushWorldUndo();
        for (size_t i : objs)
            if (mode == kSurfNone || canBeSurface(objects[i])) makeSurface(objects[i], mode);
        worldReplaced();
    }

    // Every visible object that can be one (World > All Meshes).
    std::vector<size_t> surfaceCandidates() const {
        std::vector<size_t> v;
        for (size_t i = 0; i < objects.size(); ++i)
            if (!objects[i].hidden && canBeSurface(objects[i])) v.push_back(i);
        return v;
    }

    void followSurface(const Object& o) {
        if (!scene) return;
        for (int at : surfaceParts(o)) mul(o.pivot, o.offset, scene->world.colliders[at].world);
        if (const int f = drawnFloorIndex(o); f >= 0) mul(o.pivot, o.offset, scene->world.colliders[f].world);
    }

    // ── Draw Floor: a floor drawn on an object's faces, by hand (the brush) or all its flat faces at once. It's its own
    //    collider (Walkable, linked as "<object>#floor"): always floor, even on a table; the object still blocks. ──
    float floorBrush = 0.3f;                // the brush's radius (m)
    bool painting = false;                  // a stroke is going (one undo step)
    bool brushShown = false;
    float brushAt[3] = {};
    int drawnFloorIndex(const Object& o) const {
        if (!scene) return -1;
        const std::string key = objectKey(o) + "#floor";
        for (size_t i = 0; i < scene->world.colliders.size(); ++i) if (scene->world.colliders[i].mesh == key) return static_cast<int>(i);
        return -1;
    }
    // Which of the object's triangles its drawn floor has (matched by their centres in the object's own frame).
    std::vector<char> drawnFloorTris(const Object& o) const {
        const MeshData& md = scene->meshes[o.scene];
        std::vector<char> on(md.indices.size() / 3, 0);
        const int f = drawnFloorIndex(o);
        if (f < 0) return on;
        auto key = [](const float* a, const float* b, const float* c) {
            uint64_t k = 1469598103934665603ull;
            for (int i = 0; i < 3; ++i) { k ^= static_cast<uint64_t>(std::llround((a[i] + b[i] + c[i]) / 3 * 10000.0)); k *= 1099511628211ull; }
            return k;
        };
        std::unordered_map<uint64_t, size_t> of;
        const size_t nv = md.positions.size() / 3;
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            if (md.indices[t] >= nv || md.indices[t + 1] >= nv || md.indices[t + 2] >= nv) continue;
            of.emplace(key(&md.positions[md.indices[t] * 3], &md.positions[md.indices[t + 1] * 3], &md.positions[md.indices[t + 2] * 3]), t / 3);
        }
        const WorldCollider& c = scene->world.colliders[f];
        const size_t cv = c.positions.size() / 3;
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
            if (c.indices[t] >= cv || c.indices[t + 1] >= cv || c.indices[t + 2] >= cv) continue;
            auto it = of.find(key(&c.positions[c.indices[t] * 3], &c.positions[c.indices[t + 1] * 3], &c.positions[c.indices[t + 2] * 3]));
            if (it != of.end()) on[it->second] = 1;
        }
        return on;
    }
    void writeDrawnFloor(const Object& o, const std::vector<char>& on) {
        auto& cs = scene->world.colliders;
        std::vector<u32> tris;
        for (size_t t = 0; t < on.size(); ++t) if (on[t]) tris.push_back(static_cast<u32>(t * 3));
        const int f = drawnFloorIndex(o);
        if (tris.empty()) { if (f >= 0) cs.erase(cs.begin() + f); }
        else {
            WorldCollider c = surfacePart(o, tris, WorldCollider::Walkable, o.label + " \u00b7 drawn floor", false);
            c.mesh = objectKey(o) + "#floor";
            if (f >= 0) { c.hidden = cs[f].hidden; cs[f] = std::move(c); } else cs.push_back(std::move(c));
        }
        walkPending = true;
        dirty = worldEdited = true;
    }
    // The brush: the object's faces facing up within the radius of the point (and the one under it) go in the
    // drawn floor, or out of it (erase).
    void paintFloor(size_t i, const float* at, float radius, bool erase, long hitTri) {
        const Object& o = objects[i];
        std::vector<char> on = drawnFloorTris(o);
        bool changed = false;
        worldTris(o, [&](size_t t, const float* a, const float* b, const float* c, const float* n, float) {
            const size_t k = t / 3;
            if (on[k] == !erase) return;
            if (!erase && std::fabs(n[1]) < 0.5f) return;              // a floor faces up (either winding)
            const float m[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
            const float d = std::hypot(m[0] - at[0], m[2] - at[2]);
            if (static_cast<long>(k) != hitTri && (d > radius || std::fabs(m[1] - at[1]) > std::max(radius, 0.3f))) return;
            on[k] = !erase;
            changed = true;
        });
        if (changed) writeDrawnFloor(o, on);
    }
    // All of it at once: every face flat enough to stand on (or none).
    void fillDrawnFloor(size_t i, bool fill) {
        const Object& o = objects[i];
        std::vector<char> on(scene->meshes[o.scene].indices.size() / 3, 0);
        if (fill) worldTris(o, [&](size_t t, const float*, const float*, const float*, const float* n, float) { on[t / 3] = std::fabs(n[1]) >= 0.866f; });
        pushWorldUndo();
        writeDrawnFloor(o, on);
    }

    // After a move / rotate / scale of an object: its surface keeps every triangle and follows it (followSurface);
    // which parts are flat enough to walk on is the walkable area's to work out, so nothing changes here.
    void transformDone(size_t) {}

    // Floor and wall area of an object's surfaces (m²), for its panel and the viewport legend. Cached per object
    // and layer state: a big mesh is a few hundred thousand triangles.
    struct AreaCache { int obj = -1; size_t colliders = 0, tris = 0; float m[16] = {}; double floor = 0, wall = 0; } areaCache;
    void surfaceAreas(size_t i, double& floor, double& wall) {
        const std::vector<int> parts = surfaceParts(objects[i]);
        size_t tris = 0;
        for (int p : parts) tris += scene->world.colliders[p].indices.size();
        AreaCache& ac = areaCache;
        if (ac.obj == static_cast<int>(i) && ac.colliders == scene->world.colliders.size() && ac.tris == tris &&
            (parts.empty() || std::memcmp(ac.m, scene->world.colliders[parts[0]].world, 64) == 0)) { floor = ac.floor; wall = ac.wall; return; }
        ac = AreaCache{};
        ac.obj = static_cast<int>(i); ac.colliders = scene->world.colliders.size(); ac.tris = tris;
        if (!parts.empty()) std::memcpy(ac.m, scene->world.colliders[parts[0]].world, 64);
        for (int p : parts) {
            const WorldCollider& c = scene->world.colliders[p];
            const std::vector<float> wp = worldMeshPositions(c);
            const size_t nv = wp.size() / 3;
            for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                const u32 ia = c.indices[t], ib = c.indices[t + 1], ic = c.indices[t + 2];
                if (ia >= nv || ib >= nv || ic >= nv) continue;
                const float* a = &wp[ia * 3]; const float* b = &wp[ib * 3]; const float* d = &wp[ic * 3];
                const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (L < 1e-12f) continue;
                // what Frame Home makes of it (WorldRuntime: floor parts above ~60 deg up, walls below ~45)
                const float up = std::fabs(n[1]) / L;
                const bool isFloor = c.role == WorldCollider::Floor ? up > 0.5f : c.role == WorldCollider::Solid && up > 0.7f;
                const bool isWall = (c.role == WorldCollider::Wall || c.role == WorldCollider::Solid) && up < 0.7f;
                if (isFloor) ac.floor += 0.5 * L; else if (isWall) ac.wall += 0.5 * L;
            }
        }
        floor = ac.floor; wall = ac.wall;
    }

    // ── The walkable area (src/walkgrid.h) ──
    // Frame Home walks one baked area: floors you can reach, furniture and walls in the way, seams bridged, the body's
    // radius kept from every edge. The viewport shows it (green: walkable; blue: seams it bridged; orange: its edges;
    // grey: floors you can't get to) and it follows every edit: objects moved or deleted, surfaces set, spawns placed.
    // Until World > Auto-generate Walkable Area (or a change to its settings) saves it with the home, Studio shows what
    // Frame Home bakes by itself when it loads the home; once saved (world.json) it's re-baked here after each edit,
    // so Frame Home walks exactly what you see.
    WalkParams walkEdit;                    // the settings: the saved area's, else the defaults Frame Home uses
    std::shared_ptr<const WalkGrid> walkShown;
    WalkReport walkReport;
    uint64_t walkSig = 0;
    std::chrono::steady_clock::time_point walkChanged;
    bool walkPending = false, walkSaveNext = false, walkSliding = false, walkGuideDirty = true, walkGuideOn = false;
    float walkBakeMs = 0;
    size_t walkGuideVerts = 0;              // --selftest: what the viewport draws of it

    bool walkSaved() const { return scene && !scene->world.walkGrid.empty(); }

    // Bakes the area from the objects as they are now (moved, deleted) and the world layer's surfaces and spawns.
    // The home's drawn meshes as they stand now (moved, deleted), for World > Auto-generate's walls and seats.
    std::vector<WalkSource> drawnNow() const {
        std::vector<const float*> mw(scene->meshes.size(), nullptr);
        std::vector<char> removed(scene->meshes.size(), 0);
        std::vector<std::array<float, 16>> mats(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) {
            const Object& o = objects[i];
            if (o.scene < 0 || o.scene >= static_cast<int>(scene->meshes.size())) continue;
            mul(o.pivot, o.offset, mats[i].data());
            mw[o.scene] = mats[i].data();
            removed[o.scene] = o.deleted;
        }
        return drawnWalkSources(*scene, scene->world, &mw, &removed, false);   // (the matrices are copied in)
    }

    std::shared_ptr<const WalkGrid> bakeWalk(const WalkParams& p, WalkReport* rep) {
        std::vector<const float*> mw(scene->meshes.size(), nullptr);
        std::vector<char> removed(scene->meshes.size(), 0);
        std::vector<std::array<float, 16>> mats(objects.size());
        for (size_t i = 0; i < objects.size(); ++i) {
            const Object& o = objects[i];
            if (o.scene < 0 || o.scene >= static_cast<int>(scene->meshes.size())) continue;
            mul(o.pivot, o.offset, mats[i].data());
            mw[o.scene] = mats[i].data();
            removed[o.scene] = o.deleted;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::shared_ptr<const WalkGrid> g = bakeWalkGrid(*scene, scene->world, p, rep, &mw, &removed);
        walkBakeMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return g;
    }

    // Everything the area is made from: when it changes, the area is baked again.
    uint64_t walkSignature() const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
        for (const Object& o : objects) { mix(o.pivot, 64); mix(&o.deleted, 1); }
        const HomeWorld& w = scene->world;
        for (const WorldCollider& c : w.colliders) {
            mix(&c.kind, 4); mix(&c.role, 4); mix(c.world, 64); mix(c.half, 12); mix(&c.radius, 4); mix(&c.length, 4);
            const size_t n[2] = {c.positions.size(), c.indices.size()};
            mix(n, sizeof n); mix(c.mesh.data(), c.mesh.size());
        }
        for (const auto* v : {&w.spawns, &w.seats, &w.hotspots}) for (const WorldPoint& p : *v) { mix(p.pos, 12); mix(&p.yaw, 4); mix(p.exit, 12); }
        const bool saved = !w.walkGrid.empty();
        mix(&saved, 1);
        mix(&walkEdit, sizeof walkEdit);
        return h;
    }

    // Each frame: keep the area current (a moment after the last edit, never mid-drag) and in the viewport.
    void updateWalk() {
        if (!scene || objects.empty() || uploading) {
            if (walkGuideOn) { renderer.setStaticGuide({}); walkGuideOn = false; }
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!walkSliding && !walkSaveNext) {   // the settings follow the world: an undo can bring back a saved area, or none
            WalkParams want;
            if (walkSaved()) {
                if (!scene->world.walkGridCache) scene->world.walkGridCache = decodeWalkGrid(scene->world.walkGrid);
                if (scene->world.walkGridCache) want = scene->world.walkGridCache->p;
            }
            if (std::memcmp(&want, &walkEdit, sizeof want) != 0) walkEdit = want;
        }
        const uint64_t sig = walkSignature();
        if (sig != walkSig) { walkSig = sig; walkPending = true; walkChanged = now; }
        if (walkPending && !walkSliding && !ImGuizmo::IsUsingAny() && (!walkShown || now - walkChanged > std::chrono::milliseconds(250))) rebakeWalk();
        const bool on = walkShown && showWorld && (showCat[kShowWalkArea] || showCat[kShowUnreached]) && !(isolating && isoObject < 0 && isoKind >= 0);
        if (on != walkGuideOn || (on && walkGuideDirty)) {
            std::vector<GuideVertex> v;
            if (on) v = walkGuide();
            walkGuideVerts = v.size();
            renderer.setStaticGuide(v);   // it changes only with the walk area: uploaded once, not every frame
            walkGuideOn = on;
            walkGuideDirty = false;
        }
    }

    void rebakeWalk() {
        walkPending = false;
        const bool save = walkSaved() || walkSaveNext;
        WalkReport rep;
        std::shared_ptr<const WalkGrid> g = bakeWalk(save ? walkEdit : WalkParams{}, &rep);
        walkReport = rep;
        walkShown = g;
        walkGuideDirty = true;
        if (save && g) {
            std::string enc = encodeWalkGrid(*g);
            if (enc != scene->world.walkGrid) {
                scene->world.walkGrid = std::move(enc);
                dirty = worldEdited = true;
                scene->world.origin = "studio";
                updateTitle();
            }
            scene->world.walkGridCache = g;
        }
        walkSaveNext = false;
        walkSig = walkSignature();
    }

    // Changes to the settings save the area with the home (one undo step per change).
    void walkSettingsChanged() { walkSaveNext = true; walkPending = true; walkChanged = std::chrono::steady_clock::now(); }

    // A Frame Home walker on the area as shown (placing things on the floor, the self-test).
    // The floor under (x, z) from height y down (up to 0.5 m above it): the walkable area's, else the home's own
    // surfaces there (a spot beyond the area: past a home's invisible walls, or out of reach).
    bool floorUnder(float x, float z, float y, float& out) {
        WorldRuntime rt;
        walkRuntime(rt);
        if (rt.floorAt(x, z, y, 0.5f, 5.0f, out)) return true;
        if (!walkShown) return false;                    // (that was the surfaces already)
        HomeWorld w = scene->world;
        w.walkGrid.clear();
        w.walkGridCache.reset();
        WorldRuntime surfaces;
        surfaces.build(w);
        return surfaces.floorAt(x, z, y, 0.5f, 5.0f, out);
    }

    void walkRuntime(WorldRuntime& rt) {
        HomeWorld w = scene->world;
        w.walkGridCache = walkShown;
        if (!walkShown) w = walkWorld(*scene);
        rt.build(w);
    }

    // The viewport's picture of it (the renderer's guide pass: depth-tested, a quarter strength behind things).
    std::vector<GuideVertex> walkGuide() const {
        std::vector<GuideVertex> v;
        const WalkGrid& g = *walkShown;
        const float cs = g.p.cell, lift = 0.025f;
        auto vert = [&](float x, float y, float z, const float* c) { GuideVertex q; q.pos[0] = x; q.pos[1] = y; q.pos[2] = z; std::memcpy(q.rgba, c, 16); v.push_back(q); };
        auto quad = [&](const float* a, const float* b, const float* c, const float* d, const float* col) {
            vert(a[0], a[1], a[2], col); vert(b[0], b[1], b[2], col); vert(c[0], c[1], c[2], col);
            vert(a[0], a[1], a[2], col); vert(c[0], c[1], c[2], col); vert(d[0], d[1], d[2], col);
        };
        static const float kCol[3][4] = {{0.10f, 0.78f, 0.30f, 0.40f}, {0.10f, 0.55f, 1.00f, 0.60f}, {0.50f, 0.50f, 0.50f, 0.22f}};
        static const float kEdge[4] = {1.00f, 0.45f, 0.08f, 0.95f};
        const bool showWalk = showCat[kShowWalkArea], showGrey = showCat[kShowUnreached];
        // the walkable area as it's walked: its smooth outline (WalkGrid::outline), with the edge as a strip on the
        // floor and a low kerb (seen from above and from the side)
        if (showWalk)
            g.outline([&](const float* a, const float* b, const float* c, bool bridged) {
                          const float* col = kCol[bridged ? 1 : 0];
                          vert(a[0], a[1] + lift, a[2], col); vert(b[0], b[1] + lift, b[2], col); vert(c[0], c[1] + lift, c[2], col);
                      },
                      [&](const float* a, const float* b) {
                          float nx = -(b[2] - a[2]), nz = b[0] - a[0];
                          const float L = std::sqrt(nx * nx + nz * nz);
                          if (L < 1e-5f) return;
                          nx /= L; nz /= L;
                          const float mx = (a[0] + b[0]) * 0.5f, mz = (a[2] + b[2]) * 0.5f, my = (a[1] + b[1]) * 0.5f;
                          if (g.field(mx + nx * 0.02f, mz + nz * 0.02f, my) < g.field(mx - nx * 0.02f, mz - nz * 0.02f, my)) { nx = -nx; nz = -nz; }
                          const float w = 0.035f, y0 = a[1] + lift + 0.003f, y1 = b[1] + lift + 0.003f;
                          const float e0[3] = {a[0], y0, a[2]}, e1[3] = {b[0], y1, b[2]};
                          const float i0[3] = {a[0] + nx * w, y0, a[2] + nz * w}, i1[3] = {b[0] + nx * w, y1, b[2] + nz * w};
                          quad(e0, e1, i1, i0, kEdge);
                          const float t0[3] = {e0[0], y0 + 0.06f, e0[2]}, t1[3] = {e1[0], y1 + 0.06f, e1[2]};
                          quad(e0, e1, t1, t0, kEdge);
                      });
        // floors left out (can't be reached): runs along x of cells at one height
        if (!showGrey) return v;
        struct Run { int x0, x1; float h; };
        std::vector<Run> open, next;
        for (int z = 0; z < g.nz; ++z) {
            const float za = g.oz + z * cs, zb = za + cs;
            open.clear();
            auto flush = [&](const Run& r) {
                const float x0 = g.ox + r.x0 * cs, x1 = g.ox + (r.x1 + 1) * cs, y = r.h + lift;
                const float a[3] = {x0, y, za}, b[3] = {x1, y, za}, c[3] = {x1, y, zb}, d[3] = {x0, y, zb};
                quad(a, b, c, d, kCol[2]);
            };
            for (int x = 0; x < g.nx; ++x) {
                next.clear();
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                    if (!(s->flags & kWalkUnreached)) continue;
                    bool joined = false;
                    for (Run& r : open)
                        if (r.x1 == x - 1 && std::fabs(r.h - s->hi) < 0.03f) { r.x1 = x; next.push_back(r); r.x1 = -100; joined = true; break; }
                    if (!joined) next.push_back({x, x, s->hi});
                }
                for (const Run& r : open) if (r.x1 != -100 && r.x1 == x - 1) flush(r);
                open.swap(next);
            }
            for (const Run& r : open) flush(r);
        }
        return v;
    }

    // ── World > Auto-generate Walkable Area ──
    // Bakes the area with the current settings and saves it with the home, replacing what the old Floors and Walls
    // generator made ("Auto floor / wall" surfaces). A home with no spawn gets one in the middle of its biggest
    // floor. Surfaces you set on objects (Floor, Wall, Walk through) override what it decides. One undo step.
    struct AutoReport {
        bool ok = false;
        int replaced = 0, kept = 0;
        bool spawnAdded = false;
        int walls = 0, objects = 0, colliders = 0, seats = 0, spots = 0, hotspots = 0;   // world items made
        WalkReport walk;
        float ms = 0;
    } autoReport;
    bool autoSummaryOpen = false;           // open the summary next frame
    ImVec2 autoUndoAt{-1, -1}, autoDoneAt{-1, -1};   // --selftest: where the summary's buttons are

    static bool isAuto(const std::string& name) { return name.rfind(kAutoPrefix, 0) == 0; }

    // The middle of a walkable area's main floor: the walkable spot nearest the centre of its biggest level.
    static bool walkMiddle(const WalkGrid& g, float out[3]) {
        std::map<int, double> levels;
        for (const WalkSpan& s : g.spans) if (s.flags & kWalkOk) levels[static_cast<int>(std::floor(s.hi / 0.1f))] += 1;
        if (levels.empty()) return false;
        int main = levels.begin()->first;
        for (const auto& kv : levels) if (kv.second > levels[main]) main = kv.first;
        double sx = 0, sz = 0, n = 0;
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x)
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
                    if ((s->flags & kWalkOk) && static_cast<int>(std::floor(s->hi / 0.1f)) == main) { sx += g.centerX(x); sz += g.centerZ(z); n += 1; }
        const float cx = static_cast<float>(sx / n), cz = static_cast<float>(sz / n);
        float best = 1e30f;
        for (int z = 0; z < g.nz; ++z)
            for (int x = 0; x < g.nx; ++x)
                for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                    if (!(s->flags & kWalkOk) || static_cast<int>(std::floor(s->hi / 0.1f)) != main) continue;
                    // away from edges: all 8 neighbours walkable too
                    bool inner = true;
                    for (int k = 0; k < 9 && inner; ++k) inner = g.walkNear(x + k % 3 - 1, z + k / 3 - 1, s->hi, 0.05f) != nullptr;
                    const float d = std::hypot(g.centerX(x) - cx, g.centerZ(z) - cz) + (inner ? 0 : 5);
                    if (d < best) { best = d; out[0] = g.centerX(x); out[1] = s->hi; out[2] = g.centerZ(z); }
                }
        return true;
    }

    void autoGenerate() {
        if (!scene) return;
        AutoReport r;
        pushWorldUndo();
        HomeWorld& w = scene->world;
        const size_t n0 = w.colliders.size();
        w.colliders.erase(std::remove_if(w.colliders.begin(), w.colliders.end(), [](const WorldCollider& c) { return isAuto(c.name); }), w.colliders.end());
        r.replaced = static_cast<int>(n0 - w.colliders.size());
        w.spawns.erase(std::remove_if(w.spawns.begin(), w.spawns.end(), [](const WorldPoint& p) { return p.name == "Auto spawn"; }), w.spawns.end());
        // the walls, seats, wall spots and hotspots it made last time go too (yours stay)
        auto autoPoint = [](const WorldPoint& p) { return isAuto(p.name); };
        w.seats.erase(std::remove_if(w.seats.begin(), w.seats.end(), autoPoint), w.seats.end());
        w.hotspots.erase(std::remove_if(w.hotspots.begin(), w.hotspots.end(), autoPoint), w.hotspots.end());
        w.wallSpots.erase(std::remove_if(w.wallSpots.begin(), w.wallSpots.end(), [](const WorldWallSpot& s) { return isAuto(s.name); }), w.wallSpots.end());
        for (const Object& o : objects) if (!o.deleted && surfaceIndex(o) >= 0) ++r.kept;
        const auto t0 = std::chrono::steady_clock::now();
        {   // Everything that can be a floor is one: a mesh you stand on somewhere (the floor, stairs, a rug, a deck,
            // terrain) is a Surface, walkable where it's flat enough. Everything else (furniture, vases, plants in
            // pots, lamps: the random objects) gets a Collider, so you don't walk through it and never stand on it.
            // Not the ones you gave a surface, a vista's (scenery), or see-through and cut-out ones (glass, foliage:
            // the bake keeps them blocking but never floor already).
            std::vector<char> plain(scene->meshes.size(), 0);
            for (const WalkSource& src : drawnNow())
                if (src.tag >= 0 && static_cast<size_t>(src.tag) < plain.size()) plain[src.tag] = !src.noFloor && !src.cutout;
            // A mesh is a floor where its faces that face up lie on the floors the area has (in reach or not), at
            // least 0.1 m² of them: by shape, not by which mesh the bake credits (a home's own collision mesh often
            // lies exactly on its drawn floor).
            WalkReport fr;
            const std::shared_ptr<const WalkGrid> g0 = bakeWalk(walkEdit, &fr);
            auto onFloor = [&](const float* p) {
                int cx, cz;
                if (!g0 || !g0->cellOf(p[0], p[2], cx, cz)) return false;
                for (const WalkSpan* q = g0->begin(cx, cz); q != g0->end(cx, cz); ++q)
                    if ((q->flags & (kWalkOk | kWalkNear | kWalkUnreached)) && std::fabs(q->hi - p[1]) <= 0.08f) return true;
                return false;
            };
            for (const Object& o : objects) {
                if (o.deleted || o.hidden || surfaceIndex(o) >= 0 || !canBeSurface(o) || !plain[o.scene]) continue;
                std::vector<u32> all;
                for (size_t t = 0; t + 2 < scene->meshes[o.scene].indices.size(); t += 3) all.push_back(static_cast<u32>(t));
                double floorArea = 0;
                worldTris(o, [&](size_t, const float* a, const float* b, const float* c, const float* n, float area) {
                    if (floorArea >= 0.1 || std::fabs(n[1]) < 0.866f) return;
                    const float m[3] = {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3};
                    if (onFloor(m)) floorArea += area;
                    else if (area > 0.05f) {                           // a big face: where its corners are, too
                        int on = 0;
                        for (const float* v : {a, b, c}) {
                            const float q[3] = {v[0] * 0.8f + m[0] * 0.2f, v[1] * 0.8f + m[1] * 0.2f, v[2] * 0.8f + m[2] * 0.2f};
                            on += onFloor(q);
                        }
                        floorArea += area * on / 3;
                    }
                });
                const bool floor = floorArea >= 0.1;
                w.colliders.push_back(surfacePart(o, all, floor ? WorldCollider::Solid : WorldCollider::Object,
                                                  std::string(kAutoPrefix) + (floor ? "surface · " : "collider · ") + o.label));
                ++(floor ? r.objects : r.colliders);
            }
        }
        if (w.spawns.empty()) {                                        // a start in the middle of the main floor
            WalkReport rep;
            std::shared_ptr<const WalkGrid> g = bakeWalk(walkEdit, &rep);
            float at[3];
            if (g && walkMiddle(*g, at)) {
                WorldPoint p;
                p.name = "Auto spawn";
                std::memcpy(p.pos, at, 12);
                w.spawns.push_back(p);
                r.spawnAdded = true;
            }
        }
        walkSaveNext = true;
        rebakeWalk();
        r.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        r.walk = walkReport;
        r.ok = walkShown && walkShown->any();
        if (!r.ok) {                                                   // nothing to stand on: change nothing
            scene->world = *undo.back().world;
            undo.pop_back();
            walkPending = true;
        }
        autoReport = r;
        autoSummaryOpen = true;
        worldReplaced();
        wKind = wIndex = -1;
    }

    // What stops you at the area's edges, by kind of object (the summary and the panel).
    struct WalkBlame { int objects[kCategoryCount + 1] = {}; std::string names[kCategoryCount + 1]; int homeShapes = 0; float ledges = 0; };
    WalkBlame walkBlame(const WalkReport& rep) const {
        WalkBlame b;
        std::vector<int> objOf(scene->meshes.size(), -1);
        for (size_t i = 0; i < objects.size(); ++i) if (objects[i].scene >= 0 && objects[i].scene < static_cast<int>(objOf.size())) objOf[objects[i].scene] = static_cast<int>(i);
        std::set<int> seen;
        for (const auto& bl : rep.blockers) {
            int obj = -1;
            const int t = bl.first;
            if (t < static_cast<int>(scene->meshes.size())) obj = objOf[t];
            else if (t - scene->meshes.size() < scene->world.colliders.size()) {
                const WorldCollider& c = scene->world.colliders[t - scene->meshes.size()];
                for (size_t i = 0; i < objects.size() && obj < 0; ++i) if (!c.mesh.empty() && objectKey(objects[i]) == c.mesh) obj = static_cast<int>(i);
                if (obj < 0) { ++b.homeShapes; continue; }
            }
            if (obj < 0 || !seen.insert(obj).second) continue;
            const int cat = std::clamp(objects[obj].category, 0, kCategoryCount - 1);
            if (b.objects[cat]++ < 3) b.names[cat] += (b.names[cat].empty() ? "" : ", ") + objects[obj].label;
        }
        b.ledges = rep.ledgeCells * (walkShown ? walkShown->p.cell : 0.1f);
        return b;
    }

    // The result, with its own Undo (the whole run is one step).
    void autoSummaryWindow() {
        const char* title = "Walkable Area";
        if (autoSummaryOpen) { ImGui::OpenPopup(title); autoSummaryOpen = false; }
        autoUndoAt = autoDoneAt = ImVec2(-1, -1);
        const ImGuiIO& io = ImGui::GetIO();
        // Centred every frame, not only as it appears: its report settles its height over the first frames, and a popup
        // placed before that grew past the bottom of a short window (Done and Undo out of reach).
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
        const AutoReport& r = autoReport;
        auto swatch = [&](uint32_t c) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float s = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + s * 0.15f), ImVec2(p.x + s * 0.7f, p.y + s * 0.85f), col(c), 2 * scale);
            ImGui::Dummy(ImVec2(s * 0.7f, s));
            ImGui::SameLine();
        };
        if (!r.ok) {
            ImGui::TextUnformatted("No walkable floor found: nothing level with headroom over it near the start.");
            ImGui::TextDisabled("Nothing was changed. Mark the floor's objects with Surface › Floor, or add a spawn point on it.");
            if (ImGui::Button("OK", ImVec2(120 * scale, 0))) ImGui::CloseCurrentPopup();
            autoDoneAt = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
            ImGui::EndPopup();
            return;
        }
        const WalkReport& w = r.walk;
        // The report scrolls inside the window when the window is short; Done and Undo stay below it, always in view.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttonsH = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2;
        const float titleH = ImGui::GetFontSize() + style.FramePadding.y * 2;
        const float maxReportH = std::max(120.0f * scale, io.DisplaySize.y - titleH - buttonsH - style.WindowPadding.y * 2 - 40 * scale);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxReportH));
        ImGui::BeginChild("report", ImVec2(490 * scale, 0), ImGuiChildFlags_AutoResizeY);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 470 * scale);
        swatch(0x2ec756);
        ImGui::Text("You can walk on %.0f m²%s", w.walkArea, w.areas > 1 ? ("  (" + std::to_string(w.areas) + " separate areas)").c_str() : "");
        if (w.bridgedArea > 0.05) { swatch(0x1a8cff); ImGui::Text("%.1f m² of seams and small gaps bridged (up to %.0f cm across)", w.bridgedArea, walkEdit.gap * 100); }
        swatch(0xff7314);
        ImGui::TextUnformatted("Edges: you stop a body's width from these");
        const WalkBlame b = walkBlame(w);
        ImGui::Indent(22 * scale);
        static const char* kCatHow[kCategoryCount] = {"walls & architecture", "furniture & props", "plants", "lights & glow", "terrain & water", "sky"};
        for (int c = 0; c < kCategoryCount; ++c)
            if (b.objects[c]) ImGui::Text("%d %s%s", b.objects[c], kCatHow[c], b.names[c].empty() ? "" : ("  ·  " + b.names[c] + (b.objects[c] > 3 ? "…" : "")).c_str());
        if (b.homeShapes) ImGui::Text("%d of the home's own collision shapes", b.homeShapes);
        if (b.ledges > 0.5f) ImGui::Text("%.0f m of ledges and drops (higher than a %.0f cm step)", b.ledges, walkEdit.step * 100);
        ImGui::Unindent(22 * scale);
        if (w.unreached) {
            swatch(0x808080);
            ImGui::Text("%d floor%s you can't reach from the start left out (%.0f m²: table tops, roofs, behind walls)", w.unreached, w.unreached == 1 ? "" : "s", w.unreachedArea);
        }
        if (r.spawnAdded) { swatch(kShowColors[kShowSpawns]); ImGui::TextUnformatted("A spawn point in the middle of the main floor"); }
        if (r.walls) { swatch(kShowColors[kShowWalls]); ImGui::Text("%d wall%s on the room's walls and round its open edges", r.walls, r.walls == 1 ? "" : "s"); }
        if (r.objects) { swatch(kShowColors[kShowSolidMeshes]); ImGui::Text("%d floor mesh%s made surfaces: walkable where flat, blocking where too steep", r.objects, r.objects == 1 ? "" : "es"); }
        if (r.colliders) { swatch(kRoleColors[WorldCollider::Object]); ImGui::Text("%d object%s given a collider: you don't walk through %s", r.colliders, r.colliders == 1 ? "" : "s", r.colliders == 1 ? "it" : "them"); }
        if (r.seats) { swatch(kShowColors[kShowSeats]); ImGui::Text("%d seat%s on the chairs, sofas and stools it found", r.seats, r.seats == 1 ? "" : "s"); }
        if (r.spots) { swatch(kShowColors[kShowWallSpots]); ImGui::Text("%d wall spot%s for panels, on large flat walls", r.spots, r.spots == 1 ? "" : "s"); }
        if (r.hotspots) { swatch(kShowColors[kShowHotspots]); ImGui::Text("%d teleport hotspot%s spread over the floor", r.hotspots, r.hotspots == 1 ? "" : "s"); }
        if (r.walls + r.objects + r.colliders + r.seats + r.spots + r.hotspots) ImGui::TextDisabled("They're all named \"Auto …\" in the World list: edit or delete any; running it again replaces them.");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, hex(kDim));
        if (r.kept) ImGui::Text(r.kept == 1 ? "1 object keeps the surface you gave it (Surface or Walk through)" : "%d objects keep the surfaces you gave them (Surface or Walk through)", r.kept);
        if (r.replaced) ImGui::Text("Replaced %d surfaces from the last run", r.replaced);
        ImGui::Text("Step %.0f cm · headroom %.2f m · body radius %.0f cm · baked in %.0f ms", walkEdit.step * 100, walkEdit.headroom, walkEdit.radius * 100, r.ms);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::TextDisabled("Saved with the home: Frame Home walks exactly this. It updates as you edit; fix any object with its Surface buttons (or right-click it).");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::Spacing();
        if (ImGui::Button("Done", ImVec2(120 * scale, 0))) ImGui::CloseCurrentPopup();
        autoDoneAt = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        ImGui::SameLine();
        if (ImGui::Button("Undo", ImVec2(120 * scale, 0))) { step(undo, redo); ImGui::CloseCurrentPopup(); }
        autoUndoAt = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        ImGui::EndPopup();
    }

    // The panel's WALKABLE AREA section: what Frame Home walks, its settings, and the button.
    void walkPanel() {
        if (!scene) return;
        const WalkReport& w = walkReport;
        ImGui::PushTextWrapPos(0);
        if (!walkShown) ImGui::TextDisabled("Working it out…");
        else {
            ImGui::Text("%.0f m² to walk on%s", w.walkArea, w.areas > 1 ? (" in " + std::to_string(w.areas) + " areas").c_str() : "");
            if (w.unreached) { ImGui::SameLine(); ImGui::TextDisabled("· %d out of reach", w.unreached); }
            ImGui::TextDisabled("%s", walkSaved() ? "Saved with the home: Frame Home walks exactly this (it updates as you edit)."
                                                  : "Not saved yet: Frame Home works out the same from the home when it loads it. Auto-generate (or change a setting) to save it with your edits.");
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        // presets, then the settings
        struct Preset { const char* name; const char* tip; WalkParams p; };
        static const Preset presets[3] = {
            {"Open", "Walk close to things, up higher steps and over wider gaps: small rooms, cluttered homes.", [] { WalkParams p; p.radius = 0.05f; p.gap = 0.35f; p.step = 0.45f; p.headroom = 1.4f; return p; }()},
            {"Standard", "Frame Home's defaults.", WalkParams{}},
            {"Careful", "Keep well clear of edges, low steps only, only bridge seams.", [] { WalkParams p; p.radius = 0.25f; p.gap = 0.15f; p.step = 0.2f; p.headroom = 1.8f; return p; }()}};
        const float bw = (ImGui::GetContentRegionAvail().x - 8 * scale) / 3;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 4 * scale);
            const WalkParams& p = presets[i].p;
            const bool on = std::fabs(p.step - walkEdit.step) < 1e-4f && std::fabs(p.radius - walkEdit.radius) < 1e-4f && std::fabs(p.gap - walkEdit.gap) < 1e-4f && std::fabs(p.headroom - walkEdit.headroom) < 1e-4f;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? hex(kGo) : hex(kWidget, 0.55f));
            if (ImGui::Button(presets[i].name, ImVec2(bw, 0)) && !on) { pushWorldUndo(); const float mb = walkEdit.metaBounds; walkEdit = p; walkEdit.metaBounds = mb; walkSettingsChanged(); }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", presets[i].tip);
        }
        walkSliding = false;
        auto slider = [&](const char* name, float* v, float lo, float hi, float unit, const char* fmt, const char* tip) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", name);
            ImGui::SameLine(88 * scale);
            float shown = *v * unit;
            ImGui::SetNextItemWidth(-1);
            const bool changed = ImGui::SliderFloat((std::string("##walk") + name).c_str(), &shown, lo * unit, hi * unit, fmt);
            if (ImGui::IsItemActivated()) pushWorldUndo();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            walkSliding |= ImGui::IsItemActive();
            if (changed) { *v = shown / unit; walkSettingsChanged(); }
        };
        slider("Step up", &walkEdit.step, 0.05f, 0.6f, 100, "%.0f cm", "The highest step you walk up or down. Higher is a ledge: you can't climb it or walk off it.");
        slider("Headroom", &walkEdit.headroom, 1.0f, 2.4f, 1, "%.2f m", "The clear height a floor needs. Lower than this under something (a table, a shelf, a low beam): not walkable.");
        slider("Body radius", &walkEdit.radius, 0.0f, 0.5f, 100, "%.0f cm", "How close you get to walls, furniture and ledges.");
        slider("Bridge gaps", &walkEdit.gap, 0.0f, 0.6f, 100, "up to %.0f cm", "Seams and holes in the floor up to this wide are walked over (at the floor's height), so you never drop into a crack.");
        bool stop = walkEdit.metaBounds >= 0.5f;
        if (ImGui::Checkbox("Stop at Meta's invisible walls", &stop)) { pushWorldUndo(); walkEdit.metaBounds = stop ? 1.0f : 0.0f; walkSettingsChanged(); }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Quest homes are made to be seen from one spot: invisible walls keep you there (a terrace's far end,\n"
                              "Space Station's other rooms, most of Cascadia). Off (the default), you walk to everything modelled beyond them.");
        if (ImGui::Button(walkSaved() ? "Auto-generate Again" : "Auto-generate Walkable Surfaces", ImVec2(-1, 0))) autoGenerate();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Bakes where you can walk from everything solid in the home and saves it with it (one undo step),\n"
                              "and adds walls along its edges, seats on chairs and sofas, wall spots for panels and teleport hotspots.\n"
                              "Floors need headroom; furniture, plants, railings and walls block; steps up to the step height connect;\n"
                              "seams are bridged; floors you can't reach from the start are left out. Object Surfaces override it.");
    }

    // Mesh surfaces get their pivot at their centre (Meta's and geometry ones come in world space, which put the
    // gizmo at the world origin). The triangles stay exactly where they were.
    static void recenterMesh(WorldCollider& c) {
        if (c.kind != WorldCollider::Mesh || !c.mesh.empty() || c.positions.size() < 3) return;
        static const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (std::memcmp(c.world, I, 64) != 0) return;   // already in its own frame (Meta's colliders: the mesh's vertices as-is)
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v + 2 < c.positions.size(); v += 3)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c.positions[v + k]); hi[k] = std::max(hi[k], c.positions[v + k]); }
        const float ctr[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
        if (std::fabs(ctr[0]) + std::fabs(ctr[1]) + std::fabs(ctr[2]) < 1e-4f) return;
        for (size_t v = 0; v + 2 < c.positions.size(); v += 3)
            for (int k = 0; k < 3; ++k) c.positions[v + k] -= ctr[k];
        float t[16], nw[16];
        translation(t, ctr[0], ctr[1], ctr[2]);
        mul(c.world, t, nw);
        std::memcpy(c.world, nw, 64);
    }

    void prepareWorld() {
        if (!scene) return;
        for (WorldCollider& c : scene->world.colliders) recenterMesh(c);
    }

    void pushUndo(size_t i, const float* pivotBefore = nullptr) {
        UndoStep u;
        u.object = i;
        std::memcpy(u.pivot, pivotBefore ? pivotBefore : objects[i].pivot, 64);
        u.hidden = objects[i].hidden;
        u.deleted = objects[i].deleted;
        u.group = undoGroup;
        undo.push_back(u);
        redo.clear();
        dirty = true;
        updateTitle();
    }

    // ── World layer (spawns, seats, hotspots, walls, wall spots, walkable areas) ──

    int worldCount(int kind) const {
        if (!scene) return 0;
        const HomeWorld& w = scene->world;
        switch (kind) {
            case kSpawn: return static_cast<int>(w.spawns.size());
            case kSeat: return static_cast<int>(w.seats.size());
            case kHotspot: return static_cast<int>(w.hotspots.size());
            case kCollider: return static_cast<int>(w.colliders.size());
            case kWallSpot: return static_cast<int>(w.wallSpots.size());
            case kGravity: return static_cast<int>(w.gravity.size());
            default: return 0;
        }
    }

    int worldCat(int kind, int i) const {
        switch (kind) {
            case kSpawn: return kShowSpawns;
            case kSeat: return kShowSeats;
            case kHotspot: return kShowHotspots;
            case kWallSpot: return kShowWallSpots;
            case kGravity: return kShowGravity;
            default: {
                const WorldCollider& c = scene->world.colliders[i];
                if (c.role == WorldCollider::Trigger) return kShowTriggers;
                if (c.role == WorldCollider::Floor || c.role == WorldCollider::Walkable) return kShowFloors;
                if ((c.role == WorldCollider::Solid || c.role == WorldCollider::Object) && c.kind == WorldCollider::Mesh) return kShowSolidMeshes;
                return kShowWalls;
            }
        }
    }

    bool* worldHidden(int kind, int i) {
        if (WorldPoint* p = worldPoint(kind, i)) return &p->hidden;
        if (kind == kCollider) return &scene->world.colliders[i].hidden;
        if (kind == kWallSpot) return &scene->world.wallSpots[i].hidden;
        if (kind == kGravity) return &scene->world.gravity[i].hidden;
        return nullptr;
    }

    // Drawn and clickable in the viewport.
    bool worldShown(int kind, int i) {
        if (!showWorld) return false;
        if (isolating) return kind == isoKind && i == isoIndex;
        const bool* h = worldHidden(kind, i);
        return showCat[worldCat(kind, i)] && !(h && *h);
    }

    WorldPoint* worldPoint(int kind, int i) {
        HomeWorld& w = scene->world;
        std::vector<WorldPoint>* v = kind == kSpawn ? &w.spawns : kind == kSeat ? &w.seats : kind == kHotspot ? &w.hotspots : nullptr;
        return v && i >= 0 && i < static_cast<int>(v->size()) ? &(*v)[i] : nullptr;
    }

    std::string worldName(int kind, int i) {
        if (WorldPoint* p = worldPoint(kind, i)) return p->name.empty() ? std::string(kWorldNames[kind]) + " " + std::to_string(i + 1) : p->name;
        if (kind == kCollider) {
            const WorldCollider& c = scene->world.colliders[i];
            static const char* shape[] = {"box", "sphere", "capsule", "mesh"};
            return (c.name.empty() ? std::string("surface ") + std::to_string(i + 1) : c.name) + " (" + shape[std::clamp(c.kind, 0, 3)] + ")";
        }
        if (kind == kWallSpot) { const WorldWallSpot& ws = scene->world.wallSpots[i]; return ws.name.empty() ? "wall spot " + std::to_string(i + 1) : ws.name; }
        if (kind == kGravity) {
            const WorldGravity& g = scene->world.gravity[i];
            return (g.name.empty() ? "gravity " + std::to_string(i + 1) : g.name) + " (" + kGravityNames[std::clamp(g.kind, 0, 2)] + ")";
        }
        return {};
    }

    // An item's frame for the gizmo (points: position + yaw; shapes: their own matrix). False: not movable.
    bool worldMatrix(int kind, int i, float* m) {
        if (WorldPoint* p = worldPoint(kind, i)) {
            const float c = std::cos(p->yaw), sn = std::sin(p->yaw);
            const float r[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, p->pos[0], p->pos[1], p->pos[2], 1};
            std::memcpy(m, r, 64);
            return true;
        }
        if (kind == kCollider) {
            WorldCollider& c = scene->world.colliders[i];
            if (!c.mesh.empty()) return false;   // follows its object: move the object
            recenterMesh(c);
            std::memcpy(m, c.world, 64);
            return true;
        }
        if (kind == kWallSpot) { std::memcpy(m, scene->world.wallSpots[i].world, 64); return true; }
        if (kind == kGravity) { std::memcpy(m, scene->world.gravity[i].world, 64); return true; }
        return false;
    }

    // Splits a frame into its rotation + translation (out) and per-axis scale.
    static void unscale(const float* in, float* out, float* sc) {
        std::memcpy(out, in, 64);
        for (int k = 0; k < 3; ++k) {
            sc[k] = std::sqrt(in[k * 4] * in[k * 4] + in[k * 4 + 1] * in[k * 4 + 1] + in[k * 4 + 2] * in[k * 4 + 2]);
            if (sc[k] > 1e-6f) for (int r = 0; r < 3; ++r) out[k * 4 + r] /= sc[k];
        }
    }

    // From the gizmo. Shapes stay unscaled frames: scaling resizes them (box size, radius, panel size) from their
    // size when the drag began, so the gizmo's running scale never compounds. Meshes keep the scale in their frame.
    void setWorldMatrix(int kind, int i, const float* m) {
        if (WorldPoint* p = worldPoint(kind, i)) {
            p->pos[0] = m[12]; p->pos[1] = m[13]; p->pos[2] = m[14];
            p->yaw = std::atan2(m[8], m[10]);
        } else if (kind == kCollider) {
            WorldCollider& c = scene->world.colliders[i];
            if (c.kind == WorldCollider::Mesh) { std::memcpy(c.world, m, 64); return; }
            const WorldCollider& b = i < static_cast<int>(dragBase.colliders.size()) ? dragBase.colliders[i] : c;
            float sc[3];
            unscale(m, c.world, sc);
            if (c.kind == WorldCollider::Box) for (int k = 0; k < 3; ++k) c.half[k] = std::max(0.01f, b.half[k] * sc[k]);
            else if (c.kind == WorldCollider::Sphere) c.radius = std::max(0.01f, b.radius * (sc[0] + sc[1] + sc[2]) / 3);
            else { c.radius = std::max(0.01f, b.radius * (sc[0] + sc[2]) * 0.5f); c.length = std::max(0.0f, b.length * sc[1]); }
        } else if (kind == kWallSpot) {
            WorldWallSpot& ws = scene->world.wallSpots[i];
            const WorldWallSpot& b = i < static_cast<int>(dragBase.wallSpots.size()) ? dragBase.wallSpots[i] : ws;
            float sc[3];
            unscale(m, ws.world, sc);
            ws.width = std::max(0.05f, b.width * sc[0]);
            ws.height = std::max(0.05f, b.height * sc[1]);
        } else if (kind == kGravity) {   // scaling resizes the region (box) or the pull (planet)
            WorldGravity& g = scene->world.gravity[i];
            const WorldGravity& b = i < static_cast<int>(dragBase.gravity.size()) ? dragBase.gravity[i] : g;
            float sc[3];
            unscale(m, g.world, sc);
            if (g.kind == WorldGravity::Planet) g.radius = std::max(0.5f, b.radius * (sc[0] + sc[1] + sc[2]) / 3);
            else for (int k = 0; k < 3; ++k) g.half[k] = std::max(0.1f, b.half[k] * sc[k]);
        }
    }

    // A gravity zone around the view's centre.
    void addGravity(int kind) {
        if (!scene) return;
        pushWorldUndo();
        WorldGravity g;
        g.kind = kind;
        g.name = std::string(kind == WorldGravity::Planet ? "Planet gravity " : kind == WorldGravity::Surface ? "Surface walking " : "Gravity ") +
                 std::to_string(scene->world.gravity.size() + 1);
        translation(g.world, cam.pivot[0], cam.pivot[1], cam.pivot[2]);
        g.priority = static_cast<int>(scene->world.gravity.size());   // a newer zone wins where they overlap
        scene->world.gravity.push_back(g);
        wKind = kGravity;
        wIndex = worldCount(kGravity) - 1;
        selected = -1;
    }

    // World > Add > Invisible Wall: 2 m wide and 2.5 m tall, standing on the floor where the camera looks, across
    // the view. Frame Home never draws it; it stops you (Object), and the walkable area it stands in is taken away
    // (the walk grid never stands anyone in a wall's columns).
    void addInvisibleWall() {
        if (!scene) return;
        pushWorldUndo();
        HomeWorld& w = scene->world;
        const float x = cam.pivot[0], z = cam.pivot[2];
        float y = cam.pivot[1] - 1.6f;
        {   // on the floor under the pivot when there is one
            float f;
            if (floorUnder(x, z, cam.pivot[1], f)) y = f;
        }
        float fwd[3];
        cam.forward(fwd);
        float fx = fwd[0], fz = fwd[2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl < 1e-4f) { fx = 0; fz = 1; } else { fx /= fl; fz /= fl; }
        WorldCollider c;
        int n = 1;
        for (const WorldCollider& o : w.colliders) n += isInvisibleWall(o);
        c.name = "Invisible wall " + std::to_string(n);
        c.kind = WorldCollider::Box;
        c.role = WorldCollider::Object;
        c.placed = true;
        c.half[0] = 1.0f; c.half[1] = 1.25f; c.half[2] = 0.05f;
        const float m[16] = {fz, 0, -fx, 0,  0, 1, 0, 0,  fx, 0, fz, 0,  x, y + c.half[1], z, 1};   // local X across the view
        std::copy_n(m, 16, c.world);
        w.colliders.push_back(c);
        wKind = kCollider;
        wIndex = static_cast<int>(w.colliders.size()) - 1;
        selected = -1;
        walkPending = true;
    }
    static bool isInvisibleWall(const WorldCollider& c) { return c.kind != WorldCollider::Mesh && c.role == WorldCollider::Object; }

    // New items go where the camera looks, on the floor below it.
    void addWorld(int kind) {
        if (!scene) return;
        pushWorldUndo();
        HomeWorld& w = scene->world;
        const float x = cam.pivot[0], z = cam.pivot[2];
        float y = cam.pivot[1] - 1.6f;
        {   // drop onto the floor under the pivot when there is one
            float f;
            if (floorUnder(x, z, cam.pivot[1], f)) y = f;
        }
        WorldPoint p;
        p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
        p.yaw = cam.yaw;
        if (kind == kSpawn) { p.name = "Spawn " + std::to_string(w.spawns.size() + 1); w.spawns.push_back(p); }
        else if (kind == kSeat) { p.name = "Seat " + std::to_string(w.seats.size() + 1); p.pos[1] = y + 0.45f; p.exit[2] = 0.5f; w.seats.push_back(p); }
        else if (kind == kHotspot) { p.name = "Hotspot " + std::to_string(w.hotspots.size() + 1); w.hotspots.push_back(p); }
        else if (kind == kCollider) {
            WorldCollider c;
            int n = 1;
            for (const WorldCollider& o : w.colliders) n += o.kind != WorldCollider::Mesh && o.role == WorldCollider::Solid;
            c.name = "Surface " + std::to_string(n);
            c.kind = WorldCollider::Box;
            c.role = WorldCollider::Solid;                            // walk on its top; its sides are too steep: walls
            c.placed = true;
            c.half[0] = 1.0f; c.half[1] = 0.1f; c.half[2] = 1.0f;
            translation(c.world, x, y - 0.08f, z);
            w.colliders.push_back(c);
            wKind = kCollider;
            wIndex = combineTouching(static_cast<int>(w.colliders.size()) - 1);
            selected = -1;
            return;
        } else if (kind == kWallSpot) {
            WorldWallSpot ws;
            ws.name = "Wall spot " + std::to_string(w.wallSpots.size() + 1);
            translation(ws.world, x, y + 1.4f, z);
            w.wallSpots.push_back(ws);
        }
        wKind = kind;
        wIndex = worldCount(kind) - 1;
        selected = -1;
    }

    // Points the camera at a world item (double-click in the explorer, F).
    void frameWorld(int kind, int i) {
        float m[16], mn[3], mx[3];
        if (WorldPoint* p = worldPoint(kind, i)) {
            for (int k = 0; k < 3; ++k) { mn[k] = p->pos[k] - 1; mx[k] = p->pos[k] + 1; }
        } else if (kind == kCollider) {
            const WorldCollider& c = scene->world.colliders[i];
            for (int k = 0; k < 3; ++k) { mn[k] = 1e30f; mx[k] = -1e30f; }
            if (c.kind == WorldCollider::Mesh) {
                const std::vector<float> wp = worldMeshPositions(c);
                for (size_t v = 0; v + 2 < wp.size(); v += 3)
                    for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], wp[v + k]); mx[k] = std::max(mx[k], wp[v + k]); }
                if (wp.empty()) for (int k = 0; k < 3; ++k) mn[k] = mx[k] = c.world[12 + k];
            } else {
                const float r = c.kind == WorldCollider::Box ? std::max({c.half[0], c.half[1], c.half[2]}) : c.radius + c.length * 0.5f;
                for (int k = 0; k < 3; ++k) { mn[k] = c.world[12 + k] - r; mx[k] = c.world[12 + k] + r; }
            }
        } else if (kind == kGravity) {
            const WorldGravity& g = scene->world.gravity[i];
            const float r = g.kind == WorldGravity::Planet ? g.radius : std::max({g.half[0], g.half[1], g.half[2]});
            for (int k = 0; k < 3; ++k) { mn[k] = g.world[12 + k] - r; mx[k] = g.world[12 + k] + r; }
        } else if (kind == kWallSpot && worldMatrix(kind, i, m)) {
            const WorldWallSpot& ws = scene->world.wallSpots[i];
            const float r = std::max(ws.width, ws.height) * 0.5f;
            for (int k = 0; k < 3; ++k) { mn[k] = m[12 + k] - r; mx[k] = m[12 + k] + r; }
        } else return;
        frameBounds(mn, mx);
    }

    // Frame Home walks on the home's own geometry while no surface blocks (see walkWorld).
    bool geometryStandsIn() const {
        for (const WorldCollider& c : scene->world.colliders)
            if (c.role == WorldCollider::Solid || c.role == WorldCollider::Wall) return false;
        return true;
    }

    void deleteWorld() {
        if (!scene || wKind < 0 || wIndex < 0 || wIndex >= worldCount(wKind)) return;
        pushWorldUndo();
        HomeWorld& w = scene->world;
        switch (wKind) {
            case kSpawn: w.spawns.erase(w.spawns.begin() + wIndex); break;
            case kSeat: w.seats.erase(w.seats.begin() + wIndex); break;
            case kHotspot: w.hotspots.erase(w.hotspots.begin() + wIndex); break;
            case kCollider: w.colliders.erase(w.colliders.begin() + wIndex); break;
            case kWallSpot: w.wallSpots.erase(w.wallSpots.begin() + wIndex); break;
            case kGravity: w.gravity.erase(w.gravity.begin() + wIndex); break;
        }
        if (isolating && isoKind == wKind) {   // keep Show Only Selected on the same item (or end it with it)
            if (isoIndex == wIndex) { isolating = false; refreshVisibility(); }
            else if (isoIndex > wIndex) --isoIndex;
        }
        wKind = wIndex = -1;
    }

    // The world layer was replaced wholesale (undo, clear, bulk surface changes): indices into it no longer
    // name the same items.
    // A surface you placed that now overlaps another one you placed becomes one with it (boxes put together into a
    // room are one surface; the faces buried inside each other go). Surfaces made from the home's objects follow their
    // object, and the home's own colliders are its: neither is combined. Returns the index the surface ends up at.
    static bool combinable(const WorldCollider& c) {
        return c.placed && c.mesh.empty() && c.role == WorldCollider::Solid && (c.kind == WorldCollider::Box || c.kind == WorldCollider::Mesh);
    }
    int combineTouching(int i) {
        auto& cs = scene->world.colliders;
        if (i < 0 || i >= static_cast<int>(cs.size()) || !combinable(cs[i])) return i;
        for (bool again = true; again;) {
            again = false;
            const std::vector<float> ti = colliderTriangles(cs[i]);
            for (int j = 0; j < static_cast<int>(cs.size()); ++j) {
                if (j == i || !combinable(cs[j]) || !surfacesTouch(ti, colliderTriangles(cs[j]))) continue;
                const int lo = std::min(i, j), hi = std::max(i, j);
                WorldCollider m = combineSurfaces(cs[lo], cs[hi]);   // named after the one placed first
                cs[lo] = std::move(m);
                cs.erase(cs.begin() + hi);
                i = lo;
                again = true;
                walkPending = true;
                break;
            }
        }
        return i;
    }

    void worldReplaced() {
        if (isolating && isoKind >= 0) { isolating = false; refreshVisibility(); }
        if (wKind >= 0 && wIndex >= worldCount(wKind)) wKind = wIndex = -1;
    }

    void pushWorldUndo() {
        if (!scene) return;
        UndoStep u = {};
        u.group = undoGroup;
        u.world = std::make_shared<HomeWorld>(scene->world);
        undo.push_back(u);
        redo.clear();
        dirty = worldEdited = true;
        scene->world.origin = "studio";
        updateTitle();
    }

    // A batch of edits (hide a folder, unhide all, revert) that undoes in one step.
    void beginBatch() { undoGroup = nextUndoGroup++; }
    void endBatch() { undoGroup = 0; }

    void step(std::vector<UndoStep>& from, std::vector<UndoStep>& to) {
        if (from.empty()) return;
        const int group = from.back().group;
        do {
            UndoStep u = from.back();
            from.pop_back();
            if (u.sound) {                                   // a sound edit: the sound as it was
                UndoStep back = {};
                back.group = group;
                back.sound = std::make_shared<SoundState>(sound);
                to.push_back(back);
                const bool same = sound.bytes == u.sound->bytes && (sound.s.homeVolume > 0) == (u.sound->s.homeVolume > 0);
                sound = *u.sound;
                if (same) { ambience.gain = sound.s.homeVolume; addedSound.gain = sound.s.volume; }   // only volumes
                else restartSounds();
                continue;
            }
            if (u.world) {                                   // a world-layer edit: swap the whole layer back
                UndoStep back = {};
                back.group = group;
                back.world = std::make_shared<HomeWorld>(scene->world);
                to.push_back(back);
                scene->world = *u.world;
                worldEdited = true;
                worldReplaced();
                continue;
            }
            Object& o = objects[u.object];
            UndoStep back = {u.object, {}, o.hidden, o.deleted, group};
            std::memcpy(back.pivot, o.pivot, 64);
            to.push_back(back);
            std::memcpy(o.pivot, u.pivot, 64);
            o.hidden = u.hidden;
            o.deleted = u.deleted;
            apply(o);
            if (o.deleted && selected == static_cast<int>(u.object)) selected = -1;
        } while (group != 0 && !from.empty() && from.back().group == group);
        dirty = true;
        updateTitle();
    }

    void deleteSelected() {
        if (selected < 0) return;
        beginBatch();
        const bool floor = drawnFloorIndex(objects[selected]) >= 0;
        if (surfaceIndex(objects[selected]) >= 0 || floor) {
            pushWorldUndo();
            makeSurface(objects[selected], kSurfNone);
            if (floor) scene->world.colliders.erase(scene->world.colliders.begin() + drawnFloorIndex(objects[selected]));
        }
        pushUndo(selected);
        objects[selected].deleted = true;
        apply(objects[selected]);
        endBatch();
        selected = -1;
    }

    void toggleHidden(size_t i) {
        pushUndo(i);
        objects[i].hidden = !objects[i].hidden;
        apply(objects[i]);
    }

    // Every object back to how the home shipped (undoable).
    void revertAll() {
        beginBatch();
        for (size_t i = 0; i < objects.size(); ++i) {
            Object& o = objects[i];
            if (!o.deleted && !o.hidden && std::memcmp(o.pivot, o.pivot0, 64) == 0) continue;
            pushUndo(i);
            std::memcpy(o.pivot, o.pivot0, 64);
            o.deleted = o.hidden = false;
            apply(o);
        }
        endBatch();
    }

    void unhideAll() {
        beginBatch();
        for (size_t i = 0; i < objects.size(); ++i)
            if (objects[i].hidden && !objects[i].deleted) { pushUndo(i); objects[i].hidden = false; apply(objects[i]); }
        endBatch();
    }

    void worldBounds(const Object& o, float* mn, float* mx) const {
        float w[16];
        mul(o.pivot, o.offset, w);
        for (int k = 0; k < 3; ++k) { mn[k] = 1e30f; mx[k] = -1e30f; }
        for (int c = 0; c < 8; ++c) {
            float p[3] = {c & 1 ? o.lmax[0] : o.lmin[0], c & 2 ? o.lmax[1] : o.lmin[1], c & 4 ? o.lmax[2] : o.lmin[2]}, q[3];
            point(w, p, q);
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], q[k]); mx[k] = std::max(mx[k], q[k]); }
        }
    }

    void frameBounds(const float* mn, const float* mx) {
        for (int k = 0; k < 3; ++k) cam.pivot[k] = (mn[k] + mx[k]) * 0.5f;
        float r = 0.5f * std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) + (mx[2] - mn[2]) * (mx[2] - mn[2]));
        cam.dist = std::max(0.5f, r / std::tan(kFovY * 0.5f) * 1.1f);
    }

    void frameSelected() {
        if (selected < 0) return;
        float mn[3], mx[3];
        worldBounds(objects[selected], mn, mx);
        frameBounds(mn, mx);
    }

    void frameAll() {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (const Object& o : objects) {
            if (o.deleted || o.hidden || o.skybox) continue;
            float a[3], b[3];
            worldBounds(o, a, b);
            if (b[0] - a[0] > 500 || b[1] - a[1] > 500 || b[2] - a[2] > 500) continue;   // distant backdrops
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], a[k]); mx[k] = std::max(mx[k], b[k]); }
            any = true;
        }
        if (any) frameBounds(mn, mx);
    }

    // The ray under a viewport pixel (world space; d not unit).
    void mouseRay(float px, float py, float o[3], float d[3]) const {
        const float ty = std::tan(kFovY * 0.5f), tx = ty * viewSize.x / std::max(1.0f, viewSize.y);
        const float u = (px - viewPos.x) / std::max(1.0f, viewSize.x), v = (py - viewPos.y) / std::max(1.0f, viewSize.y);
        float pose[16];
        cam.pose(pose);
        const float dc[3] = {(2 * u - 1) * tx, (1 - 2 * v) * ty, -1};
        direction(pose, dc, d);
        for (int i = 0; i < 3; ++i) o[i] = pose[12 + i];
    }
    // Where that ray meets the level plane at height y (false: it doesn't, in front).
    bool mouseOnLevel(float px, float py, float y, float out[3]) const {
        float o[3], d[3];
        mouseRay(px, py, o, d);
        if (std::fabs(d[1]) < 1e-6f) return false;
        const float t = (y - o[1]) / d[1];
        if (t <= 0 || t > 1e4f) return false;
        for (int i = 0; i < 3; ++i) out[i] = o[i] + d[i] * t;
        return true;
    }
    // Where you stand up from a seat (Frame Home: its exit, turned by its yaw; half a metre in front when it has none),
    // on the floor below it.
    void seatExitWorld(const WorldPoint& st, float out[3]) const {
        const float c = std::cos(st.yaw), s = std::sin(st.yaw);
        float ex = st.exit[0], ez = st.exit[2];
        if (std::hypot(ex, ez) < 0.05f) { ex = 0; ez = -0.5f; }
        out[0] = st.pos[0] + c * ex + s * ez;
        out[2] = st.pos[2] - s * ex + c * ez;
        out[1] = st.pos[1] - 0.45f;
        if (walkShown) { float f; if (walkShown->floorAt(out[0], out[2], st.pos[1], 0.3f, 2.0f, f)) out[1] = f; }
    }
    // Dragging things in the view: press on a world item (a wall, a seat, a spawn...), a home object, or a seat's
    // stand-up spot, and move the mouse: it slides over its own level, one undo step.
    enum DragKind { kDragNone, kDragWorld, kDragObject, kDragExit };
    int itemDrag = kDragNone, dragIndex = -1, dragKindW = -1;
    bool dragMoved = false;
    float dragY = 0, dragGrab[2] = {0, 0};
    ImVec2 dragFrom;

    // Where on object `only` the viewport pixel is (world point and its triangle), if it's on it.
    bool pickOn(int only, float px, float py, float* hit, long* tri = nullptr) {
        float t;
        long k;
        if (pickRay(px, py, only, &t, &k) != only) return false;
        const float ty = std::tan(kFovY * 0.5f), tx = ty * viewSize.x / viewSize.y;
        const float u = (px - viewPos.x) / viewSize.x, v = (py - viewPos.y) / viewSize.y;
        float pose[16];
        cam.pose(pose);
        const float dc[3] = {(2 * u - 1) * tx, (1 - 2 * v) * ty, -1};
        float dir[3];
        direction(pose, dc, dir);
        for (int i = 0; i < 3; ++i) hit[i] = pose[12 + i] + dir[i] * t;
        if (tri) *tri = k;
        return true;
    }

    // Nearest visible object under a viewport pixel (ray against its triangles, in object space).
    int pick(float px, float py) { return pickRay(px, py, -1, nullptr, nullptr); }
    // The same, against one object only (only >= 0); outT: the distance along the (world) ray, outTri its triangle.
    int pickRay(float px, float py, int only, float* outT, long* outTri) {
        if (!scene || viewSize.x < 1 || viewSize.y < 1) return -1;
        const float ty = std::tan(kFovY * 0.5f), tx = ty * viewSize.x / viewSize.y;
        const float u = (px - viewPos.x) / viewSize.x, v = (py - viewPos.y) / viewSize.y;
        float pose[16];
        cam.pose(pose);
        const float dc[3] = {(2 * u - 1) * tx, (1 - 2 * v) * ty, -1};
        float origin[3] = {pose[12], pose[13], pose[14]}, dir[3];
        direction(pose, dc, dir);
        int best = -1;
        float bestT = 1e30f;
        long bestTri = -1;
        for (size_t i = 0; i < objects.size(); ++i) {
            const Object& o = objects[i];
            if (o.deleted || o.hidden || o.skybox || (only >= 0 && static_cast<int>(i) != only)) continue;
            float w[16], inv[16];
            mul(o.pivot, o.offset, w);
            if (!invert(w, inv)) continue;
            float lo[3], ld[3];
            point(inv, origin, lo);
            direction(inv, dir, ld);
            // slab test against the local bounds
            float t0 = 0, t1 = bestT;
            bool miss = false;
            for (int k = 0; k < 3 && !miss; ++k) {
                if (std::fabs(ld[k]) < 1e-12f) { miss = lo[k] < o.lmin[k] || lo[k] > o.lmax[k]; continue; }
                float a = (o.lmin[k] - lo[k]) / ld[k], b = (o.lmax[k] - lo[k]) / ld[k];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
                miss = t0 > t1;
            }
            if (miss) continue;
            const MeshData& md = scene->meshes[o.scene];
            const float* P = md.positions.data();
            const size_t nv = md.positions.size() / 3;
            for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
                const u32 ia = md.indices[t], ib = md.indices[t + 1], ic = md.indices[t + 2];
                if (ia >= nv || ib >= nv || ic >= nv) continue;
                const float* a = P + ia * 3; const float* b = P + ib * 3; const float* c = P + ic * 3;
                float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                float pv[3] = {ld[1] * e2[2] - ld[2] * e2[1], ld[2] * e2[0] - ld[0] * e2[2], ld[0] * e2[1] - ld[1] * e2[0]};
                float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                if (std::fabs(det) < 1e-12f) continue;
                float id = 1.0f / det;
                float tv[3] = {lo[0] - a[0], lo[1] - a[1], lo[2] - a[2]};
                float bu = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id;
                if (bu < 0 || bu > 1) continue;
                float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                float bv = (ld[0] * qv[0] + ld[1] * qv[1] + ld[2] * qv[2]) * id;
                if (bv < 0 || bu + bv > 1) continue;
                float t_ = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id;
                if (t_ > 1e-4f && t_ < bestT) { bestT = t_; best = static_cast<int>(i); bestTri = static_cast<long>(t / 3); }
            }
        }
        if (outT) *outT = bestT;
        if (outTri) *outTri = bestTri;
        return best;
    }

    // ── Per frame ──

    void renderViewport() {
        rendered = false;
        const uint32_t w = static_cast<uint32_t>(std::max(0.0f, viewSize.x * studioPerf.viewScale)),
                       h = static_cast<uint32_t>(std::max(0.0f, viewSize.y * studioPerf.viewScale));   // Settings: viewport resolution
        if (!scene || w < 8 || h < 8) return;
        if (w != viewW || h != viewH) {
            renderer.resize(w, h);
            if (viewTex) ImGui_ImplVulkan_RemoveTexture(viewTex);
            viewTex = ImGui_ImplVulkan_AddTexture(viewSampler, renderer.layerView(0), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            viewW = w;
            viewH = h;
        }
        const float ty = std::tan(kFovY * 0.5f), tx = ty * w / h;
        float pose[16], view[16];
        cam.pose(pose);
        invert(pose, view);
        EyeView eye;
        eye.proj = rendererProjection(tx, ty);
        std::memcpy(eye.view.m, view, 64);
        std::memcpy(eye.position, &pose[12], 12);
        EyeView eyes[2] = {eye, eye};
        const auto r0 = std::chrono::steady_clock::now();
        {   // the timeline clock drives every animation: Clay / glTF on the CPU, HSL rigs + shaders on the GPU
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.1f, std::chrono::duration<float>(now - animTick).count());
            animTick = now;
            if (animPlaying) {
                animTime += dt;
                if (animLength > 0 && animTime >= animLength) {
                    animBase += animLength * std::floor(animTime / animLength);
                    animTime = std::fmod(animTime, animLength);
                }
            }
            if (scene->animate && !scene->animatedMeshes.empty()) scene->animate(animTime, scene->meshes);
        }
        // Clay / glTF loops are baked on the CPU above. HSL shaders (water, flipbooks, VAT, sway) and rigs read the
        // clock the way the headset does, never wrapping: wrapping it made every shader animation jump each loop.
        renderer.render(eyes, scene->animate ? animTime : animBase + animTime);
        // Smoothed cost of the 3D frame (includes waiting for the previous one on the GPU).
        renderMs = renderMs * 0.9f + 0.1f * std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - r0).count();
        rendered = true;
    }

    void shortcuts() {
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput || flying) return;
        const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) openDialog();
        if (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveEnvironmentDialog();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveToLibrary();
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) showControls = !showControls;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) openSearch();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) viewMaximized = !viewMaximized;
        // Mid gizmo drag or field edit: no undo / redo, delete or selection changes (Ctrl is also the snap key, and a
        // selection change mid-drag left ImGuizmo dragging the next item).
        if (ImGuizmo::IsUsingAny() || gizmoWasUsing || ImGui::IsAnyItemActive()) return;
        if (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Z)) step(undo, redo);
        if ((ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Z)) || (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))) step(redo, undo);
        // Blender's numpad views (Ctrl: from the opposite side)
        if (scene && ImGui::IsKeyPressed(ImGuiKey_Keypad1, false)) viewFrom(ctrl ? kPi : 0, 0);
        if (scene && ImGui::IsKeyPressed(ImGuiKey_Keypad3, false)) viewFrom(ctrl ? -kPi * 0.5f : kPi * 0.5f, 0);
        if (scene && ImGui::IsKeyPressed(ImGuiKey_Keypad7, false)) viewFrom(0, ctrl ? -kPi * 0.5f + 0.001f : kPi * 0.5f - 0.001f);
        if (ctrl) return;
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) showSidebar = !showSidebar;
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) showToolbar = !showToolbar;
        if (!scene) return;
        if (shift && ImGui::IsKeyPressed(ImGuiKey_A, false)) { ImGui::OpenPopup("##addAtMouse"); return; }
        if (alt && ImGui::IsKeyPressed(ImGuiKey_A, false)) { selected = -1; wKind = wIndex = -1; }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) animPlaying = !animPlaying;
        if (shift && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false)) { animTime = 0; animTick = std::chrono::steady_clock::now(); }
        if (shift && ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) { animTime = animLength; animTick = std::chrono::steady_clock::now(); }
        if (ImGui::IsKeyPressed(ImGuiKey_G, false)) tool = Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) tool = Tool::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) tool = Tool::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_B, false)) tool = Tool::DrawFloor;
        if (tool == Tool::DrawFloor && ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) floorBrush = std::max(0.05f, floorBrush / 1.25f);
        if (tool == Tool::DrawFloor && ImGui::IsKeyPressed(ImGuiKey_RightBracket)) floorBrush = std::min(5.0f, floorBrush * 1.25f);
        // X asks first (Blender's "OK? Delete"); Delete deletes
        if (ImGui::IsKeyPressed(ImGuiKey_X, false) && (selected >= 0 || wKind >= 0)) ImGui::OpenPopup("##deleteAsk");
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) { if (wKind >= 0) deleteWorld(); else deleteSelected(); }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) showWorld = !showWorld;
        if (ImGui::IsKeyPressed(ImGuiKey_I, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDivide, false)) toggleIsolate();
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) { if (alt) unhideAll(); else if (selected >= 0) toggleHidden(selected); }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false)) { if (wKind >= 0) frameWorld(wKind, wIndex); else frameSelected(); }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) frameAll();
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad0, false) || ImGui::IsKeyPressed(ImGuiKey_P, false)) cam.playerView();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (isolating) toggleIsolate();
            selected = -1; wKind = wIndex = -1;
        }
    }

    // Eye toggle drawn like Blender's outliner restriction column.
    bool eyeButton(const char* id, bool open) {
        const float sz = ImGui::GetFrameHeight();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(sz, sz * 0.8f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + sz * 0.5f, p.y + sz * 0.4f);
        ImU32 color = col(ImGui::IsItemHovered() ? 0xffffff : open ? 0xc8c8c8 : 0x6a6a6a);
        if (open) {
            dl->AddEllipse(c, ImVec2(sz * 0.30f, sz * 0.17f), color, 0, 0, 1.4f * scale);
            dl->AddCircleFilled(c, sz * 0.09f, color);
        } else {
            dl->PathArcTo(ImVec2(c.x, c.y - sz * 0.12f), sz * 0.30f, 0.25f * kPi, 0.75f * kPi, 12);
            dl->PathStroke(color, 0, 1.4f * scale);
        }
        return clicked;
    }

    // The name a set of repeated objects shares: trailing numbers, "(n)", "INST", "LOD", "copy" and a "_v" version
    // prefix are dropped ("LightFixtureB3" -> "LightFixtureB", "plant BB10 13 INST(1)" -> "plant BB").
    static std::string familyOf(std::string n) {
        auto strip = [&](void) -> bool {
            const size_t len = n.size();
            while (!n.empty() && (n.back() == ' ' || n.back() == '_' || n.back() == '-' || n.back() == '.')) n.pop_back();
            if (!n.empty() && n.back() == ')') { const size_t o = n.rfind('('); if (o != std::string::npos) n.erase(o); }
            while (!n.empty() && std::isdigit(static_cast<unsigned char>(n.back()))) n.pop_back();
            for (const char* suf : {"inst", "INST", "Inst", "lod", "LOD", "copy", "Copy"}) {
                const size_t sl = std::strlen(suf);
                if (n.size() > sl && n.compare(n.size() - sl, sl, suf) == 0) { n.erase(n.size() - sl); break; }
            }
            if (n.size() > 2 && (n.back() == 'v' || n.back() == 'V') && !std::isalpha(static_cast<unsigned char>(n[n.size() - 2]))) n.pop_back();
            return n.size() != len;
        };
        const std::string orig = n;
        while (strip()) {}
        return n.size() >= 3 ? n : orig;
    }

    // ── Icons: the viewport toolbar's tools, the editors, the Properties tabs, navigation and playback ──
    enum class ToolIcon {
        Move, Rotate, Scale, DrawFloor, Global, Local, PlayerView, FrameAll,
        Viewport, Outliner, Properties, Timeline,                  // the editors (an area's editor button)
        Object, Walk, Scene, Bundle, Headset,                      // the Properties tabs
        Zoom, Pan, Play, Pause, SkipStart, SkipEnd, Sound, Muted   // navigation, playback
    };

    // A small line icon in a square of side `size` centred on `c`.
    void drawToolIcon(ImDrawList* dl, ToolIcon icon, ImVec2 c, float size, ImU32 color) {
        const float r = size * 0.5f, t = std::max(1.0f, 1.6f * scale);
        auto arrowHead = [&](ImVec2 tip, ImVec2 dir) {   // dir: unit vector the arrow points along
            const float l = r * 0.32f;
            const ImVec2 back(tip.x - dir.x * l, tip.y - dir.y * l), side(-dir.y * l * 0.7f, dir.x * l * 0.7f);
            dl->AddTriangleFilled(tip, ImVec2(back.x + side.x, back.y + side.y), ImVec2(back.x - side.x, back.y - side.y), color);
        };
        switch (icon) {
            case ToolIcon::Move: {
                dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), color, t);
                dl->AddLine(ImVec2(c.x, c.y - r * 0.8f), ImVec2(c.x, c.y + r * 0.8f), color, t);
                arrowHead(ImVec2(c.x + r, c.y), ImVec2(1, 0)); arrowHead(ImVec2(c.x - r, c.y), ImVec2(-1, 0));
                arrowHead(ImVec2(c.x, c.y - r), ImVec2(0, -1)); arrowHead(ImVec2(c.x, c.y + r), ImVec2(0, 1));
                break;
            }
            case ToolIcon::Rotate: {
                dl->PathArcTo(c, r * 0.72f, -1.2f, 3.9f, 24);
                dl->PathStroke(color, 0, t);
                arrowHead(ImVec2(c.x + std::cos(-1.2f) * r * 0.72f, c.y + std::sin(-1.2f) * r * 0.72f), ImVec2(0.93f, 0.36f));
                break;
            }
            case ToolIcon::Scale: {
                dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.1f), ImVec2(c.x + r * 0.1f, c.y + r * 0.8f), color, 1.5f * scale, 0, t);
                dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.1f), ImVec2(c.x + r * 0.7f, c.y - r * 0.7f), color, t);
                arrowHead(ImVec2(c.x + r * 0.85f, c.y - r * 0.85f), ImVec2(0.707f, -0.707f));
                break;
            }
            case ToolIcon::DrawFloor: {
                dl->AddLine(ImVec2(c.x + r * 0.75f, c.y - r * 0.75f), ImVec2(c.x - r * 0.15f, c.y + r * 0.15f), color, t * 1.6f);
                dl->AddCircleFilled(ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), r * 0.32f, color);
                dl->AddLine(ImVec2(c.x - r * 0.95f, c.y + r * 0.9f), ImVec2(c.x + r * 0.2f, c.y + r * 0.9f), color, t);
                break;
            }
            case ToolIcon::Global: {
                dl->AddCircle(c, r * 0.8f, color, 24, t);
                dl->AddEllipse(c, ImVec2(r * 0.35f, r * 0.8f), color, 0, 20, t);
                dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), color, t);
                break;
            }
            case ToolIcon::Local: {   // a little cube: its own axes
                const float a = r * 0.55f, d = r * 0.3f;
                dl->AddRect(ImVec2(c.x - a, c.y - a + d), ImVec2(c.x + a - d, c.y + a), color, 0, 0, t);
                dl->AddLine(ImVec2(c.x - a, c.y - a + d), ImVec2(c.x - a + d, c.y - a), color, t);
                dl->AddLine(ImVec2(c.x + a - d, c.y - a + d), ImVec2(c.x + a, c.y - a), color, t);
                dl->AddLine(ImVec2(c.x + a - d, c.y + a), ImVec2(c.x + a, c.y + a - d), color, t);
                dl->AddLine(ImVec2(c.x - a + d, c.y - a), ImVec2(c.x + a, c.y - a), color, t);
                dl->AddLine(ImVec2(c.x + a, c.y - a), ImVec2(c.x + a, c.y + a - d), color, t);
                break;
            }
            case ToolIcon::PlayerView: {   // a head and shoulders
                dl->AddCircle(ImVec2(c.x, c.y - r * 0.35f), r * 0.3f, color, 16, t);
                dl->PathArcTo(ImVec2(c.x, c.y + r * 0.85f), r * 0.65f, 3.4f, 6.02f, 16);
                dl->PathStroke(color, 0, t);
                break;
            }
            case ToolIcon::Viewport: {   // a cube in perspective
                const float a = r * 0.55f, d = r * 0.32f;
                const ImVec2 f0(c.x - a, c.y - a + d), f1(c.x + a - d, c.y + a);
                dl->AddRect(f0, f1, color, 0, 0, t);
                dl->AddLine(f0, ImVec2(f0.x + d, f0.y - d), color, t);
                dl->AddLine(ImVec2(f1.x, f0.y), ImVec2(f1.x + d, f0.y - d), color, t);
                dl->AddLine(ImVec2(f1.x, f1.y), ImVec2(f1.x + d, f1.y - d), color, t);
                dl->AddLine(ImVec2(f0.x + d, f0.y - d), ImVec2(f1.x + d, f0.y - d), color, t);
                dl->AddLine(ImVec2(f1.x + d, f0.y - d), ImVec2(f1.x + d, f1.y - d), color, t);
                break;
            }
            case ToolIcon::Outliner: {   // an indented list
                for (int k = 0; k < 3; ++k) {
                    const float y = c.y + (k - 1) * r * 0.6f, x0 = c.x - r * 0.75f + (k ? r * 0.35f : 0);
                    dl->AddRectFilled(ImVec2(x0, y - r * 0.12f), ImVec2(x0 + r * 0.24f, y + r * 0.12f), color, 1.0f);
                    dl->AddLine(ImVec2(x0 + r * 0.4f, y), ImVec2(c.x + r * 0.8f, y), color, t);
                }
                break;
            }
            case ToolIcon::Properties: {   // a wrench
                dl->AddLine(ImVec2(c.x - r * 0.6f, c.y + r * 0.6f), ImVec2(c.x + r * 0.15f, c.y - r * 0.15f), color, t * 1.8f);
                dl->PathArcTo(ImVec2(c.x + r * 0.35f, c.y - r * 0.35f), r * 0.38f, 2.4f, 2.4f + 4.7f, 16);
                dl->PathStroke(color, 0, t * 1.4f);
                break;
            }
            case ToolIcon::Timeline: {   // a clock
                dl->AddCircle(c, r * 0.78f, color, 24, t);
                dl->AddLine(c, ImVec2(c.x, c.y - r * 0.5f), color, t);
                dl->AddLine(c, ImVec2(c.x + r * 0.38f, c.y), color, t);
                break;
            }
            case ToolIcon::Object: {   // a small cube in Frame Studio's orange
                const float a = r * 0.5f, d = r * 0.28f;
                const ImU32 o = col(kOrange);
                dl->AddRectFilled(ImVec2(c.x - a, c.y - a + d), ImVec2(c.x + a - d, c.y + a), o, 1.5f * scale);
                dl->AddQuadFilled(ImVec2(c.x - a, c.y - a + d), ImVec2(c.x - a + d, c.y - a), ImVec2(c.x + a, c.y - a), ImVec2(c.x + a - d, c.y - a + d), col(0xffc070));
                dl->AddQuadFilled(ImVec2(c.x + a - d, c.y - a + d), ImVec2(c.x + a, c.y - a), ImVec2(c.x + a, c.y + a - d), ImVec2(c.x + a - d, c.y + a), col(0xc77a20));
                break;
            }
            case ToolIcon::Walk: {   // two footprints
                for (int k = -1; k <= 1; k += 2) {
                    const ImVec2 f(c.x + k * r * 0.32f, c.y + k * r * 0.22f);
                    dl->AddEllipseFilled(f, ImVec2(r * 0.22f, r * 0.4f), col(0x5fd068));
                    dl->AddCircleFilled(ImVec2(f.x, f.y - r * 0.55f), r * 0.14f, col(0x5fd068));
                }
                break;
            }
            case ToolIcon::Scene: {   // a framed landscape: the environment's picture
                const ImVec2 a(c.x - r * 0.8f, c.y - r * 0.6f), b(c.x + r * 0.8f, c.y + r * 0.6f);
                dl->AddRectFilled(a, b, col(0x2e4a78), 2.0f * scale);
                dl->AddTriangleFilled(ImVec2(a.x + r * 0.1f, b.y - r * 0.1f), ImVec2(c.x - r * 0.2f, c.y - r * 0.15f), ImVec2(c.x + r * 0.35f, b.y - r * 0.1f), col(0x8fb4ff));
                dl->AddTriangleFilled(ImVec2(c.x, b.y - r * 0.1f), ImVec2(c.x + r * 0.4f, c.y + r * 0.05f), ImVec2(b.x - r * 0.1f, b.y - r * 0.1f), col(0xc8d8ff));
                dl->AddCircleFilled(ImVec2(c.x + r * 0.45f, c.y - r * 0.28f), r * 0.14f, col(kOrange));
                dl->AddRect(a, b, color, 2.0f * scale, 0, 1.0f);
                break;
            }
            case ToolIcon::Bundle: {   // a box
                dl->AddRect(ImVec2(c.x - r * 0.75f, c.y - r * 0.35f), ImVec2(c.x + r * 0.75f, c.y + r * 0.7f), color, 1.5f * scale, 0, t);
                dl->AddRectFilled(ImVec2(c.x - r * 0.85f, c.y - r * 0.7f), ImVec2(c.x + r * 0.85f, c.y - r * 0.3f), color, 1.5f * scale);
                dl->AddLine(ImVec2(c.x - r * 0.25f, c.y + r * 0.05f), ImVec2(c.x + r * 0.25f, c.y + r * 0.05f), color, t);
                break;
            }
            case ToolIcon::Headset: {   // goggles and a strap
                dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.4f), ImVec2(c.x + r * 0.8f, c.y + r * 0.35f), color, 4.0f * scale, 0, t);
                dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.1f), color, t);
                dl->AddLine(ImVec2(c.x + r * 0.1f, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.1f), color, t);
                dl->AddLine(ImVec2(c.x - r * 0.8f, c.y - r * 0.1f), ImVec2(c.x - r * 1.0f, c.y - r * 0.1f), color, t);
                dl->AddLine(ImVec2(c.x + r * 0.8f, c.y - r * 0.1f), ImVec2(c.x + r * 1.0f, c.y - r * 0.1f), color, t);
                break;
            }
            case ToolIcon::Zoom: {   // a magnifier
                dl->AddCircle(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), r * 0.45f, color, 20, t);
                dl->AddLine(ImVec2(c.x + r * 0.18f, c.y + r * 0.18f), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f), color, t * 1.6f);
                break;
            }
            case ToolIcon::Pan: {   // an open hand
                dl->AddRectFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.05f), ImVec2(c.x + r * 0.45f, c.y + r * 0.7f), color, r * 0.3f);
                for (int k = 0; k < 4; ++k) {
                    const float x = c.x - r * 0.36f + k * r * 0.24f;
                    dl->AddRectFilled(ImVec2(x - r * 0.08f, c.y - r * (k == 0 || k == 3 ? 0.45f : 0.7f)), ImVec2(x + r * 0.08f, c.y + r * 0.1f), color, r * 0.08f);
                }
                break;
            }
            case ToolIcon::Play:
                dl->AddTriangleFilled(ImVec2(c.x - r * 0.4f, c.y - r * 0.6f), ImVec2(c.x - r * 0.4f, c.y + r * 0.6f), ImVec2(c.x + r * 0.6f, c.y), color);
                break;
            case ToolIcon::Pause:
                dl->AddRectFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.55f), ImVec2(c.x - r * 0.15f, c.y + r * 0.55f), color, 1.0f);
                dl->AddRectFilled(ImVec2(c.x + r * 0.15f, c.y - r * 0.55f), ImVec2(c.x + r * 0.5f, c.y + r * 0.55f), color, 1.0f);
                break;
            case ToolIcon::SkipStart:
            case ToolIcon::SkipEnd: {
                const float k = icon == ToolIcon::SkipEnd ? 1.0f : -1.0f;
                dl->AddTriangleFilled(ImVec2(c.x - k * r * 0.45f, c.y - r * 0.55f), ImVec2(c.x - k * r * 0.45f, c.y + r * 0.55f), ImVec2(c.x + k * r * 0.35f, c.y), color);
                dl->AddRectFilled(ImVec2(c.x + k * r * 0.35f - r * 0.1f, c.y - r * 0.55f), ImVec2(c.x + k * r * 0.35f + r * 0.1f, c.y + r * 0.55f), color);
                break;
            }
            case ToolIcon::Sound:
            case ToolIcon::Muted: {   // a speaker, with waves or a cross
                dl->AddRectFilled(ImVec2(c.x - r * 0.75f, c.y - r * 0.22f), ImVec2(c.x - r * 0.45f, c.y + r * 0.22f), color);
                dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y), ImVec2(c.x - r * 0.05f, c.y - r * 0.6f), ImVec2(c.x - r * 0.05f, c.y + r * 0.6f), color);
                if (icon == ToolIcon::Sound) {
                    dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.4f, -0.8f, 0.8f, 10); dl->PathStroke(color, 0, t);
                    dl->PathArcTo(ImVec2(c.x - r * 0.05f, c.y), r * 0.72f, -0.8f, 0.8f, 12); dl->PathStroke(color, 0, t);
                } else {
                    dl->AddLine(ImVec2(c.x + r * 0.2f, c.y - r * 0.3f), ImVec2(c.x + r * 0.8f, c.y + r * 0.3f), color, t);
                    dl->AddLine(ImVec2(c.x + r * 0.2f, c.y + r * 0.3f), ImVec2(c.x + r * 0.8f, c.y - r * 0.3f), color, t);
                }
                break;
            }
            case ToolIcon::FrameAll: {   // four corners
                const float a = r * 0.8f, l = r * 0.35f;
                for (int sx = -1; sx <= 1; sx += 2)
                    for (int sy = -1; sy <= 1; sy += 2) {
                        const ImVec2 k(c.x + sx * a, c.y + sy * a);
                        dl->AddLine(k, ImVec2(k.x - sx * l, k.y), color, t);
                        dl->AddLine(k, ImVec2(k.x, k.y - sy * l), color, t);
                    }
                break;
            }
        }
    }

    // Right-click menu for an object (viewport and explorer): what it is for the player first, then the usual.
    bool objMenuOpen = false;               // --selftest: the viewport's object menu is showing
    int menuObject = -1;
    ImVec2 surfButtonAt[kSurfModes];                 // --selftest: the object panel's Surface buttons
    bool surfScroll = false;                         // --selftest: scroll the panel to them (a small window hides them)
    bool rArmed = false;                    // right button down on the viewport: a click opens the menu, a drag flies
    float rMoved = 0;
    void surfaceItems(const std::vector<size_t>& objs) {
        int mode = -2;                      // the one they share (-1: mixed)
        size_t can = 0;
        for (size_t i : objs) { if (!canBeSurface(objects[i])) continue; ++can; const int m = surfaceMode(objects[i]); mode = mode == -2 ? m : mode == m ? m : -1; }
        ImGui::TextDisabled(objs.size() > 1 ? "Surface (all %zu)" : "Surface", can);
        for (int m = 0; m < kSurfModes; ++m) {
            const uint32_t tint = m == kSurfSolid ? 0xc8b45a : m == kSurfObject ? 0xb07aff : kText;
            ImGui::PushStyleColor(ImGuiCol_Text, hex(can ? tint : kDim));
            const bool hit = ImGui::MenuItem((std::string("    ") + kSurfNames[m]).c_str(), nullptr, mode == m, can > 0);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", can ? kSurfHelp[m] : "Not solid (see-through, glow, sky or a far backdrop)");
            if (hit && mode != m) {
                std::vector<size_t> v;
                for (size_t i : objs) if (canBeSurface(objects[i])) v.push_back(i);
                setSurfaces(v, m);
            }
        }
    }
    void objectMenu(size_t i) {
        ImGui::TextUnformatted(objects[i].label.c_str());
        ImGui::Separator();
        surfaceItems({i});
        ImGui::Separator();
        if (ImGui::MenuItem("Frame", "F")) { selected = static_cast<int>(i); wKind = wIndex = -1; frameSelected(); }
        if (ImGui::MenuItem(objects[i].hidden ? "Unhide" : "Hide", "H")) toggleHidden(i);
        if (ImGui::MenuItem(isolating ? "Show All" : "Show Only This", "I")) { selected = static_cast<int>(i); wKind = wIndex = -1; toggleIsolate(); }
        if (ImGui::MenuItem("Delete", "X")) { selected = static_cast<int>(i); wKind = wIndex = -1; deleteSelected(); }
    }

    // ── Files: everything inside the open home's files, by what each thing was. Meta's homes keep each file as it
    //    was cooked: "<the original file>/<hash>/<cooked form>" ("t_lamp.png/8d1b8474/tex_r676"), a scene's
    //    meshes and materials inside it ("shell.usda/__mesh_sub_targets___ms61/root_xform_sm_lbg_vasec_wxuu").
    //    Here the original names and folders show, a scene's meshes, materials and collision meshes grouped under
    //    it, each with what it is; saved, each becomes what it was again (a texture a PNG, a mesh an OBJ, a sound
    //    bank its .bank or the sound in it), or exactly as stored. Read in the background the first time. ──
    enum SaveAs { kSaveRaw, kSavePng, kSaveObj };
    struct FileEntry {
        std::string name, kind;             // its original name, and what it is ("Texture", "Mesh", "Sound bank")
        uint64_t size = 0;
        std::vector<std::string> chain;     // the file, then the entries into the archives (as stored)
        std::string saveName;               // what it was
        int saveAs = kSaveRaw;
    };
    struct FileDir { std::map<std::string, FileDir> dirs; std::vector<FileEntry> files; uint64_t bytes = 0; size_t count = 0; };
    struct FileTree { std::string file; FileDir root; };
    std::vector<FileTree> fileTrees;
    std::string filesFor;                   // the home they were read for
    std::mutex filesMutex;
    std::thread filesReader;
    std::atomic<bool> filesBusy{false};
    static std::string lowerOf(std::string n) { for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return n; }
    static std::string extOf(const std::string& n) { const size_t d = n.find_last_of('.'); return d == std::string::npos ? std::string() : lowerOf(n.substr(d)); }
    static bool isArchiveName(const std::string& n) {
        const std::string e = extOf(n);
        return e == ".zip" || e == ".ovrscene" || e == ".fenv" || e == ".apk";
    }
    // A file that was something before it was cooked: a texture, a scene, a sound bank, a template, a shader...
    static bool isSourceName(const std::string& n) {
        static const std::set<std::string> k = {".png", ".jpg", ".jpeg", ".tga", ".exr", ".hdr", ".bank", ".usda", ".usd", ".usdc", ".fbx",
                                                ".hstf", ".surface", ".wav", ".ogg", ".gltf", ".glb", ".json", ".jsonc", ".ktx", ".mat", ".anim"};
        return k.count(extOf(n)) != 0;
    }
    static bool isHashSeg(const std::string& s) { return s.size() == 8 && s.find_first_not_of("0123456789abcdef") == std::string::npos; }
    // "root_xform_sm_lbg_vasec_wxuu" -> "root_xform_sm_lbg_vasec" (the cook's 4-character tag)
    static std::string untagged(const std::string& s) {
        if (s.size() > 5 && s[s.size() - 5] == '_' && s.substr(s.size() - 4).find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz") == std::string::npos)
            return s.substr(0, s.size() - 5);
        return s;
    }
    // "__mesh_sub_targets___ms61" -> "Meshes"
    static std::string groupName(const std::string& s) {
        if (s.rfind("__mesh_sub_targets", 0) == 0) return "Meshes";
        if (s.rfind("__material_sub_targets", 0) == 0) return "Materials";
        if (s.rfind("__phys_mesh_sub_targets", 0) == 0) return "Collision meshes";
        if (s.rfind("__phys_convex_mesh_sub_targets", 0) == 0) return "Convex collision meshes";
        if (s.rfind("__tex_sub_targets", 0) == 0) return "Texture streams";
        if (s.rfind("__", 0) == 0 && s.find("_sub_targets") != std::string::npos) {
            std::string g = s.substr(2, s.find("_sub_targets") - 2);
            for (char& c : g) if (c == '_') c = ' ';
            if (!g.empty()) g[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(g[0])));
            return g + "s";
        }
        return s;
    }
    static std::string kindOfExt(const std::string& e) {
        if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".ktx" || e == ".exr" || e == ".hdr") return "Picture";
        if (e == ".ogg" || e == ".wav" || e == ".mp3" || e == ".opus" || e == ".flac") return "Sound";
        if (e == ".bank") return "Sound bank";
        if (e == ".gltf" || e == ".glb" || e == ".fbx" || e == ".obj") return "Model";
        if (e == ".hstf") return "Template";
        if (e == ".json" || e == ".jsonc" || e == ".txt" || e == ".xml") return "Text";
        if (e == ".dex") return "App code";
        return "File";
    }
    // Where a stored entry goes in the tree, and what it is.
    static void describe(const std::string& path, std::vector<std::string>& folders, FileEntry& f) {
        std::vector<std::string> segs;
        for (size_t a = 0; a <= path.size();) { const size_t b = std::min(path.find('/', a), path.size()); if (b > a) segs.push_back(path.substr(a, b - a)); a = b + 1; }
        if (segs.empty()) return;
        size_t k = segs.size() - 1;
        for (size_t i = 0; i + 1 < segs.size(); ++i) if (isSourceName(segs[i])) { k = i; break; }
        folders.assign(segs.begin(), segs.begin() + static_cast<std::ptrdiff_t>(k));
        const std::string asset = segs[k];
        if (k + 1 == segs.size()) {                                    // a plain file, as it is
            f.name = f.saveName = asset;
            f.kind = kindOfExt(extOf(asset));
            return;
        }
        std::vector<std::string> rest;
        for (size_t i = k + 1; i < segs.size(); ++i) if (!isHashSeg(segs[i])) rest.push_back(segs[i]);
        const std::string leaf = rest.empty() ? segs.back() : rest.back(), type = leaf.substr(0, leaf.find('_'));
        const std::string stem = asset.substr(0, asset.find_last_of('.')), e = extOf(asset);
        if (rest.size() <= 1 && rest.size() == 1 && untagged(leaf) == type) {   // the file itself, cooked
            f.name = asset;
            if (type == "tex") {
                f.kind = "Texture";
                if (e == ".exr" || e == ".hdr") f.saveName = asset;     // (HDR: kept as stored)
                else { f.saveName = stem + ".png"; f.saveAs = kSavePng; }
            } else if (type == "bank") { f.kind = "Sound bank"; f.saveName = asset; }
            else if (type == "shader") { f.kind = "Shader"; f.saveName = asset; }
            else if (type == "template") { f.kind = e == ".hstf" ? "Template" : "Template (from " + e.substr(1) + ")"; f.saveName = e == ".hstf" ? asset : stem + ".hstf"; }
            else { f.kind = "Cooked " + type; f.saveName = asset; }
            return;
        }
        folders.push_back(asset);                                       // a scene: what it holds, grouped
        for (size_t i = 0; i + 1 < rest.size(); ++i) folders.push_back(groupName(rest[i]));
        f.name = untagged(leaf);
        const std::string group = rest.size() > 1 ? groupName(rest[rest.size() - 2]) : std::string();
        if (group == "Meshes") { f.kind = "Mesh"; f.saveName = f.name + ".obj"; f.saveAs = kSaveObj; }
        else if (group == "Materials") { f.kind = "Material"; f.saveName = f.name + ".material"; }
        else if (group == "Collision meshes" || group == "Convex collision meshes") {
            f.kind = group == "Collision meshes" ? "Collision mesh" : "Convex collision mesh";
            f.saveName = f.name + ".collision";
        } else if (group == "Texture streams") { f.kind = "Texture stream (more detail for " + asset + ")"; f.saveName = stem + "." + f.name; }
        else { f.kind = group.empty() ? "Part" : group; f.saveName = f.name; }
    }
    // An archive's entries into `dir` (archives inside it become folders, two levels deep).
    static void listArchive(mz_zip_archive& z, const std::vector<std::string>& chain, FileDir& dir, int depth) {
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st) || st.m_is_directory) continue;
            const std::string path = st.m_filename;
            std::vector<std::string> ch = chain;
            ch.push_back(path);
            std::vector<std::string> folders;
            FileEntry f;
            f.size = st.m_uncomp_size;
            f.chain = ch;
            describe(path, folders, f);
            FileDir* d = &dir;
            for (const auto& fo : folders) d = &d->dirs[fo];
            if (depth < 2 && isArchiveName(f.name) && st.m_uncomp_size < (1ull << 31)) {
                std::vector<uint8_t> bytes(static_cast<size_t>(st.m_uncomp_size));
                mz_zip_archive in = {};
                if (mz_zip_reader_extract_to_mem(&z, i, bytes.data(), bytes.size(), 0) && mz_zip_reader_init_mem(&in, bytes.data(), bytes.size(), 0)) {
                    listArchive(in, ch, d->dirs[f.name], depth + 1);
                    mz_zip_reader_end(&in);
                    continue;
                }
            }
            d->files.push_back(std::move(f));
        }
    }
    static void tally(FileDir& d) {
        d.bytes = 0; d.count = d.files.size();
        for (const auto& f : d.files) d.bytes += f.size;
        for (auto& kv : d.dirs) { tally(kv.second); d.bytes += kv.second.bytes; d.count += kv.second.count; }
    }
    void readFiles() {
        std::string key;
        for (const auto& p : homePaths) key += p + "|";
        if (key == filesFor || filesBusy) return;
        if (filesReader.joinable()) filesReader.join();
        filesFor = key;
        filesBusy = true;
        { std::lock_guard<std::mutex> l(filesMutex); fileTrees.clear(); }
        filesReader = std::thread([this, paths = homePaths, key] {
            std::vector<FileTree> trees;
            for (const auto& p : paths) {
                FileTree t;
                t.file = p;
                mz_zip_archive z = {};
                if (mz_zip_reader_init_file(&z, p.c_str(), 0)) { listArchive(z, {p}, t.root, 0); mz_zip_reader_end(&z); }
                else {                                                 // a model file: itself
                    std::error_code ec;
                    FileEntry f;
                    f.name = f.saveName = narrow(fs::path(widen(p)).filename().wstring());
                    f.kind = kindOfExt(extOf(f.name));
                    f.size = fs::file_size(fs::path(widen(p)), ec);
                    f.chain = {p};
                    t.root.files.push_back(f);
                }
                tally(t.root);
                trees.push_back(std::move(t));
            }
            std::lock_guard<std::mutex> l(filesMutex);
            if (filesFor == key) fileTrees = std::move(trees);
            filesBusy = false;
        });
    }
    // A file's bytes as stored, opened down its chain of archives.
    static std::vector<uint8_t> readFileEntry(const std::vector<std::string>& chain) {
        std::vector<uint8_t> bytes;
        if (chain.empty()) return bytes;
        if (chain.size() == 1) { std::ifstream f(fs::path(widen(chain[0])), std::ios::binary); bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); return bytes; }
        mz_zip_archive z = {};
        if (!mz_zip_reader_init_file(&z, chain[0].c_str(), 0)) return bytes;
        std::vector<uint8_t> outer;
        for (size_t k = 1; k < chain.size(); ++k) {
            size_t n = 0;
            void* p = mz_zip_reader_extract_file_to_heap(&z, chain[k].c_str(), &n, 0);
            mz_zip_reader_end(&z);
            if (!p) return {};
            bytes.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + n);
            mz_free(p);
            if (k + 1 == chain.size()) return bytes;
            outer = std::move(bytes);
            z = {};
            if (!mz_zip_reader_init_mem(&z, outer.data(), outer.size(), 0)) return {};
        }
        return bytes;
    }
    static std::string sizeText(uint64_t b) {
        char t[32];
        if (b >= 1048576) std::snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
        else if (b >= 1024) std::snprintf(t, sizeof t, "%.0f KB", b / 1024.0);
        else std::snprintf(t, sizeof t, "%llu B", static_cast<unsigned long long>(b));
        return t;
    }
    static bool isSoundKind(const std::string& k) { return k == "Sound" || k == "Sound bank"; }
    // Saves a file: as what it was (asStored false), or exactly as stored; a sound bank's sound as an Ogg (asOgg).
    void saveFileCopy(const FileEntry& f, bool asStored, bool asOgg = false) {
        std::vector<uint8_t> bytes = readFileEntry(f.chain);
        if (bytes.empty()) { setStatus("Couldn't read " + f.name); return; }
        std::string name = asStored ? f.chain.back().substr(f.chain.back().find_last_of('/') + 1) : f.saveName;
        if (asOgg) {                                                   // the sound in an FMOD bank, as the Ogg it plays
            std::string why;
            bytes = fsb5::ambienceOgg(std::vector<const std::vector<uint8_t>*>{&bytes}, &why);
            if (bytes.empty()) { setStatus("No sound Frame Studio can play in " + f.name); return; }
            const size_t q0 = why.find('"'), q1 = q0 == std::string::npos ? q0 : why.find('"', q0 + 1);
            name = (q1 != std::string::npos ? why.substr(q0 + 1, q1 - q0 - 1) : f.name) + ".ogg";
        } else if (!asStored && f.saveAs == kSavePng) {               // a cooked texture: the picture again
            RendtxtrInfo ti;
            std::vector<uint8_t> rgba;
            if (!parseRendtxtrHeader(bytes, ti) || static_cast<uint64_t>(ti.rawDataOffset) + ti.rawDataLen > bytes.size() ||
                !astc::decodeASTC(bytes.data() + ti.rawDataOffset, ti.rawDataLen, ti.width, ti.height, ti.blockW, ti.blockH, rgba, false) ||
                rgba.size() < static_cast<size_t>(ti.width) * ti.height * 4) {
                setStatus("Couldn't turn " + f.name + " back into a picture: saving it as stored");
                name = f.chain.back().substr(f.chain.back().find_last_of('/') + 1);
            } else {
                std::vector<uint8_t> png;
                stbi_write_png_to_func([](void* ctx, void* data, int size) {
                    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
                }, &png, static_cast<int>(ti.width), static_cast<int>(ti.height), 4, rgba.data(), static_cast<int>(ti.width) * 4);
                bytes = std::move(png);
            }
        } else if (!asStored && f.saveAs == kSaveObj) {               // a cooked mesh: an OBJ of it
            std::vector<float> pos, uv;
            std::vector<u32> idx;
            if (!parseRendMesh(bytes, pos, uv, idx) || pos.empty()) {
                setStatus("Couldn't read " + f.name + " as a mesh: saving it as stored");
                name = f.chain.back().substr(f.chain.back().find_last_of('/') + 1);
            } else {
                std::string o = "# " + f.name + ", from " + f.chain.back() + " (Frame Studio)\no " + f.name + "\n";
                char ln[128];
                for (size_t v = 0; v + 2 < pos.size(); v += 3) { std::snprintf(ln, sizeof ln, "v %.6g %.6g %.6g\n", pos[v], pos[v + 1], pos[v + 2]); o += ln; }
                const bool hasUv = uv.size() / 2 == pos.size() / 3;
                if (hasUv) for (size_t v = 0; v + 1 < uv.size(); v += 2) { std::snprintf(ln, sizeof ln, "vt %.6g %.6g\n", uv[v], 1.0f - uv[v + 1]); o += ln; }
                for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                    const u32 a = idx[t] + 1, b = idx[t + 1] + 1, c = idx[t + 2] + 1;
                    if (hasUv) std::snprintf(ln, sizeof ln, "f %u/%u %u/%u %u/%u\n", a, a, b, b, c, c);
                    else std::snprintf(ln, sizeof ln, "f %u %u %u\n", a, b, c);
                    o += ln;
                }
                bytes.assign(o.begin(), o.end());
            }
        }
        saveSoundAs(bytes, name);
    }
    void filesSection(float h) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(0x2a2333));
        const bool open = collapsed.count(3001) != 0;                  // closed until opened (reading takes a moment)
        if (ImGui::Selectable("##fileshdr", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h))) {
            if (open) collapsed.erase(3001); else collapsed.insert(3001);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Everything inside the home's files, under the names they had before Meta cooked them.\nRight-click one to save it as what it was (a texture a PNG, a mesh an OBJ), or exactly as stored.");
        dl->AddText(ImVec2(p.x + 8 * scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), col(0xc9a8ff), open ? "▾  Files" : "▸  Files");
        if (!open) return;
        readFiles();
        std::lock_guard<std::mutex> l(filesMutex);
        if (fileTrees.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * scale);
            ImGui::TextDisabled(filesBusy ? "Reading the files…" : "No files");
            return;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14 * scale);
        std::function<void(const FileDir&)> dir = [&](const FileDir& d) {
            for (const auto& kv : d.dirs) {
                ImGui::PushID(kv.first.c_str());
                const bool group = kv.first == "Meshes" || kv.first == "Materials" || kv.first == "Collision meshes" ||
                                   kv.first == "Convex collision meshes" || kv.first == "Texture streams";
                if (group) ImGui::PushStyleColor(ImGuiCol_Text, hex(0x9ab8e0));
                const bool o = ImGui::TreeNodeEx("##dir", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", kv.first.c_str());
                if (group) ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled("%zu · %s", kv.second.count, sizeText(kv.second.bytes).c_str());
                if (o) { dir(kv.second); ImGui::TreePop(); }
                ImGui::PopID();
            }
            for (size_t i = 0; i < d.files.size(); ++i) {
                const FileEntry& f = d.files[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Text, hex(isSoundKind(f.kind) ? kOrange : 0xd8d8d8));
                ImGui::TreeNodeEx("##file", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", f.name.c_str());
                ImGui::PopStyleColor();
                const bool hov = ImGui::IsItemHovered();
                const bool menu = ImGui::BeginPopupContextItem("##filemenu");
                ImGui::SameLine();
                ImGui::TextDisabled("%s · %s", f.kind.c_str(), sizeText(f.size).c_str());
                std::string stored;
                for (size_t k = 0; k < f.chain.size(); ++k) stored += (k ? "\n  › " : "") + (k ? f.chain[k] : narrow(fs::path(widen(f.chain[k])).filename().wstring()));
                if (hov) ImGui::SetTooltip("%s (%s)\nStored in the home as:\n%s", f.name.c_str(), f.kind.c_str(), stored.c_str());
                if (menu) {
                    ImGui::TextDisabled("%s · %s", f.name.c_str(), f.kind.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem(("Save as " + f.saveName + "…").c_str())) saveFileCopy(f, false);
                    if (f.saveAs != kSaveRaw || f.saveName != f.chain.back().substr(f.chain.back().find_last_of('/') + 1))
                        if (ImGui::MenuItem("Save as Stored (cooked)…")) saveFileCopy(f, true);
                    if (f.kind == "Sound bank" && ImGui::MenuItem("Save Its Sound as Ogg…")) saveFileCopy(f, false, true);
                    if (ImGui::MenuItem("Copy Where It's Stored")) ImGui::SetClipboardText(stored.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        };
        for (size_t t = 0; t < fileTrees.size(); ++t) {
            ImGui::PushID(static_cast<int>(t) + 31000);
            const bool o = ImGui::TreeNodeEx("##src", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen, "%s",
                                             narrow(fs::path(widen(fileTrees[t].file)).filename().wstring()).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%zu files · %s", fileTrees[t].root.count, sizeText(fileTrees[t].root.bytes).c_str());
            if (o) { dir(fileTrees[t].root); ImGui::TreePop(); }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    void outliner(float height) {
        testEyes.clear();
        // (its display mode and filter are in the Outliner's header)
        ImGui::BeginChild("outliner", ImVec2(0, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(kList));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("rows", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        std::string f = filter;
        for (char& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!scene || objects.empty()) {
            ImGui::SetCursorPos(ImVec2(10 * scale, 8 * scale));
            ImGui::TextDisabled(scene ? "Loading…" : "No home open");
        }
        auto matches = [&](const Object& o) {
            if (o.deleted) return false;
            if (f.empty()) return true;
            std::string n = o.label + " " + o.name;
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return n.find(f) != std::string::npos;
        };
        int row = 0;
        const float h = ImGui::GetFrameHeight(), indent = 14 * scale;
        // Rows outside the list's visible part take their space and draw nothing: a home with thousands of objects
        // drew every one of them every frame (storybook: ~1.5 ms of UI a frame).
        auto rowVisible = [&]() {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::IsRectVisible(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + h))) return true;
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
            return false;
        };
        // A collapsible header row (arrow, label, count) with an eye that shows/hides everything inside it.
        // Folders, object rows and world groups each get their own ID namespace: a folder key (src * 100 + category)
        // and an object index are both small numbers, and sharing one space gave a folder's eye the same ID as an
        // object's (ImGui's "conflicting ID" error; the folder's eye didn't hide it).
        auto header = [&](int key, const std::string& text, int depth, const std::vector<size_t>& members, bool strong) -> bool {
            const bool open = !f.empty() || !collapsed.count(key);
            if (!rowVisible()) return open;   // scrolled out of view: only its space
            ImGui::PushID("folder");
            ImGui::PushID(key);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(strong ? 0x202020 : 0x262626));
            ImGui::SetCursorScreenPos(p);
            if (ImGui::Selectable("##hdr", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h)) && f.empty()) {
                if (open) collapsed.insert(key); else collapsed.erase(key);
            }
            if (ImGui::BeginPopupContextItem("##hdrmenu")) {   // one surface role for everything in the folder
                ImGui::TextUnformatted(text.c_str());
                ImGui::Separator();
                surfaceItems(members);
                ImGui::EndPopup();
            }
            const float x = p.x + 8 * scale + depth * indent, cy = p.y + h * 0.5f, a = h * 0.16f;
            if (open) dl->AddTriangleFilled(ImVec2(x - a, cy - a * 0.6f), ImVec2(x + a, cy - a * 0.6f), ImVec2(x, cy + a * 0.8f), col(kDim));
            else dl->AddTriangleFilled(ImVec2(x - a * 0.6f, cy - a), ImVec2(x - a * 0.6f, cy + a), ImVec2(x + a * 0.8f, cy), col(kDim));
            const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
            {   // folder: a tab and a body, open folders drawn hollow
                const float fx = x + 11 * scale, fw = 13 * scale, fh = 9 * scale, fy = cy - fh * 0.5f + 1 * scale;
                const ImU32 fc = col(strong ? 0xd9a441 : 0xb98f45);
                dl->AddRectFilled(ImVec2(fx, fy - 2 * scale), ImVec2(fx + fw * 0.45f, fy + 1 * scale), fc, 1.5f * scale);
                if (open) dl->AddRect(ImVec2(fx, fy), ImVec2(fx + fw, fy + fh), fc, 1.5f * scale, 0, 1.3f * scale);
                else dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + fw, fy + fh), fc, 1.5f * scale);
            }
            const std::string count = std::to_string(members.size());
            const ImVec2 cs = ImGui::CalcTextSize(count.c_str());
            // Long folder names stop before the count and the eye instead of running under them.
            dl->PushClipRect(p, ImVec2(p.x + w - h - cs.x - 16 * scale, p.y + h), true);
            dl->AddText(ImVec2(x + 30 * scale, ty), col(strong ? kText : 0xc8c8c8), text.c_str());
            dl->PopClipRect();
            dl->AddText(ImVec2(p.x + w - h - cs.x - 10 * scale, ty), col(kDim), count.c_str());
            bool anyShown = false;
            for (size_t i : members) anyShown |= !objects[i].hidden;
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - h - 4 * scale, p.y + h * 0.1f));
            const ImVec2 eyeAt = ImGui::GetCursorScreenPos();
            if (eyeButton("eye", anyShown)) {
                beginBatch();
                for (size_t i : members)            // anything shown: hide the rest of them too; all hidden: show them all
                    if (objects[i].hidden != anyShown) toggleHidden(i);
                endBatch();
            }
            if (testing && ImGui::IsRectVisible(eyeAt, ImVec2(eyeAt.x + h, eyeAt.y + h * 0.8f))) testEyes.push_back({ImVec2(eyeAt.x + h * 0.5f, eyeAt.y + h * 0.4f), members});
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
            ImGui::PopID();
            ImGui::PopID();
            return open;
        };
        auto objectRow = [&](size_t i, int depth) {
            if (!rowVisible()) { ++row; return; }   // scrolled out of view: only its space (and its stripe parity)
            Object& o = objects[i];
            ImGui::PushID("object");
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            if (row++ & 1) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(kListAlt));
            const bool sel = selected == static_cast<int>(i);
            ImGui::SetCursorScreenPos(p);
            if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, h))) {
                selected = static_cast<int>(i);
                wKind = wIndex = -1;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameSelected();
            }
            if (ImGui::IsItemHovered() && o.label != o.name) ImGui::SetTooltip("%s", o.name.c_str());
            if (ImGui::BeginPopupContextItem("##rowmenu")) {
                if (selected != static_cast<int>(i)) { selected = static_cast<int>(i); wKind = wIndex = -1; }
                objectMenu(i);
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float ix = p.x + 10 * scale + depth * indent, iy = p.y + h * 0.5f, r = h * 0.2f;
            dl->AddTriangleFilled(ImVec2(ix, iy - r), ImVec2(ix + r * 0.95f, iy + r * 0.7f), ImVec2(ix - r * 0.95f, iy + r * 0.7f),
                                  col(o.hidden ? kDim : kOrange));
            dl->PushClipRect(p, ImVec2(p.x + w - h - 6 * scale, p.y + h), true);
            dl->AddText(ImVec2(ix + 12 * scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                        col(o.hidden ? kDim : sel ? 0xffffff : kText), o.label.c_str());
            dl->PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(p.x + w - h - 4 * scale, p.y + h * 0.1f));
            const ImVec2 eyeAt = ImGui::GetCursorScreenPos();
            if (eyeButton("eye", !o.hidden)) toggleHidden(i);
            if (testing && ImGui::IsRectVisible(eyeAt, ImVec2(eyeAt.x + h, eyeAt.y + h * 0.8f))) testEyes.push_back({ImVec2(eyeAt.x + h * 0.5f, eyeAt.y + h * 0.4f), {i}});
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
            ImGui::PopID();
            ImGui::PopID();
        };
        // Footprint / Vista  >  the folders of the file, as it's laid out (or by kind: Architecture, Furniture & Props,
        // Plants, Lights & Glow, Terrain & Water, Sky)  >  objects
        const int sources = scene ? static_cast<int>(scene->apkPaths.size()) : 0;
        // The file's own folders: each folder's subfolders, then its objects, in the order the file has them.
        std::function<void(const std::vector<size_t>&, size_t, int, const std::string&)> layoutTree =
            [&](const std::vector<size_t>& members, size_t level, int depth, const std::string& keyPath) {
                std::vector<std::string> order;
                std::map<std::string, std::vector<size_t>> sub;
                std::vector<size_t> here;
                for (size_t i : members) {
                    if (objects[i].folders.size() <= level) { here.push_back(i); continue; }
                    const std::string& k = objects[i].folders[level];
                    if (!sub.count(k)) order.push_back(k);
                    sub[k].push_back(i);
                }
                for (const std::string& k : order) {
                    const std::string kp = keyPath + "/" + k;
                    const int key = static_cast<int>(std::hash<std::string>()(kp) & 0x3fffffff) | 0x40000000;
                    if (header(key, k, depth, sub[k], depth == 0)) layoutTree(sub[k], level + 1, depth + 1, kp);
                }
                for (size_t i : here) objectRow(i, depth);
            };
        for (int src = 0; src < sources; ++src) {
            std::vector<size_t> inSource;
            for (size_t i = 0; i < objects.size(); ++i) if (objects[i].source == src && matches(objects[i])) inSource.push_back(i);
            if (inSource.empty()) continue;
            int depth = 0;
            if (sources > 1) {
                const std::string title = std::string(src == 0 ? "Footprint · " : "Vista · ") +
                                          prettyHome(scene->apkPaths[src]);
                if (!header(1000 + src, title, 0, inSource, true)) continue;
                depth = 1;
            }
            if (byFileLayout) { layoutTree(inSource, 0, depth, std::to_string(src)); continue; }
            for (int cat = 0; cat < kCategoryCount; ++cat) {
                std::vector<size_t> members;
                for (size_t i : inSource) if (objects[i].category == cat) members.push_back(i);
                if (members.empty()) continue;
                if (!header(src * 100 + cat, kCategoryNames[cat], depth, members, false)) continue;
                // Repeated objects (LightFixtureB, B2, B3...; plantBB10_7_INST, _13_INST(1)...) share a folder.
                std::map<std::string, std::vector<size_t>> fam;
                std::vector<std::string> order;
                for (size_t i : members) {
                    const std::string k = familyOf(objects[i].label);
                    if (!fam.count(k)) order.push_back(k);
                    fam[k].push_back(i);
                }
                for (const std::string& k : order) {
                    const std::vector<size_t>& v = fam[k];
                    if (v.size() < 2) { objectRow(v[0], depth + 1); continue; }
                    const int key = static_cast<int>(std::hash<std::string>()(std::to_string(src * 100 + cat) + "/" + k) & 0x3fffffff) | 0x40000000;
                    if (header(key, k, depth + 1, v, false))
                        for (size_t i : v) objectRow(i, depth + 2);
                }
            }
        }
        // World: the home's spawns, seats, hotspots, walls and wall spots (Studio's own format, from Meta's when converted).
        if (scene && !objects.empty()) {
            int total = 0;
            for (int k = 0; k < kWorldKinds; ++k) total += worldCount(k);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            hdl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(0x1d2330));
            const bool open = !collapsed.count(2000);
            if (ImGui::Selectable("##worldhdr", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h))) { if (open) collapsed.insert(2000); else collapsed.erase(2000); }
            const std::string title = std::string(open ? "\u25be  " : "\u25b8  ") + "World" + (scene->world.origin == "meta" ? "  \u00b7 converted from Meta" : scene->world.origin == "studio" ? "  \u00b7 edited" : "");
            hdl->AddText(ImVec2(p.x + 8 * scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), col(0x8fb4ff), title.c_str());
            const std::string cnt = std::to_string(total);
            hdl->AddText(ImVec2(p.x + w - ImGui::CalcTextSize(cnt.c_str()).x - 10 * scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), col(kDim), cnt.c_str());
            if (open) {
                // One group per kind of thing: its eye shows / hides the whole group in the viewport, each row's eye
                // that item alone.
                for (int cat = 0; cat < kShowCats; ++cat) {
                    std::vector<std::pair<int, int>> items;
                    for (int k = 0; k < kWorldKinds; ++k)
                        for (int i = 0, n = worldCount(k); i < n; ++i)
                            if (worldCat(k, i) == cat) items.emplace_back(k, i);
                    if (items.empty()) continue;
                    ImGui::PushID("worldgroup");
                    ImGui::PushID(cat);
                    const ImVec2 gp = ImGui::GetCursorScreenPos();
                    const bool gopen = !collapsed.count(2100 + cat);
                    if (ImGui::Selectable("##grp", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(w, h))) { if (gopen) collapsed.insert(2100 + cat); else collapsed.erase(2100 + cat); }
                    const float ty = gp.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
                    hdl->AddCircleFilled(ImVec2(gp.x + 20 * scale, gp.y + h * 0.5f), 4 * scale, col(kShowColors[cat], showCat[cat] ? 1.0f : 0.3f));
                    const std::string gt = std::string(gopen ? "\u25be " : "\u25b8 ") + kShowNames[cat];
                    hdl->AddText(ImVec2(gp.x + 30 * scale, ty), col(showCat[cat] ? kText : kDim), gt.c_str());
                    const std::string gc = std::to_string(items.size());
                    hdl->AddText(ImVec2(gp.x + w - h - ImGui::CalcTextSize(gc.c_str()).x - 10 * scale, ty), col(kDim), gc.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(gp.x + w - h - 4 * scale, gp.y + h * 0.1f));
                    if (eyeButton("geye", showCat[cat])) showCat[cat] = !showCat[cat];
                    ImGui::SetCursorScreenPos(ImVec2(gp.x, gp.y + h));
                    if (gopen) {
                        const size_t shown = std::min<size_t>(items.size(), 300);
                        for (size_t r = 0; r < shown; ++r) {
                            const int k = items[r].first, i = items[r].second;
                            ImGui::PushID(static_cast<int>(r));
                            const ImVec2 rp = ImGui::GetCursorScreenPos();
                            const bool sel = wKind == k && wIndex == i;
                            bool* hid = worldHidden(k, i);
                            if (ImGui::Selectable("##wrow", sel, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, h))) {
                                wKind = k; wIndex = i; selected = -1;
                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameWorld(k, i);
                            }
                            hdl->PushClipRect(rp, ImVec2(rp.x + w - h - 6 * scale, rp.y + h), true);
                            hdl->AddText(ImVec2(rp.x + 40 * scale, rp.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                                         col(hid && *hid ? kDim : sel ? 0xffffff : kText), worldName(k, i).c_str());
                            hdl->PopClipRect();
                            ImGui::SetCursorScreenPos(ImVec2(rp.x + w - h - 4 * scale, rp.y + h * 0.1f));
                            if (hid && eyeButton("weye", !*hid)) { *hid = !*hid; worldEdited = dirty = true; updateTitle(); }
                            ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + h));
                            ImGui::PopID();
                        }
                        if (items.size() > shown) { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40 * scale); ImGui::TextDisabled("\u2026 %zu more", items.size() - shown); }
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
                if (total == 0) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14 * scale);
                    ImGui::TextDisabled("None yet \u2014 World menu \u203a Add");
                }
            }
        }
        if (scene && !objects.empty()) filesSection(h);
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndChild();
    }

    // A panel's section header: a small semibold caption with a hairline under it, a little air above it.
    // A panel, as Blender's Properties draws them: a title strip (an arrow, the title) and its fields on a lighter
    // rounded box. In Properties the boxes go behind the fields (a draw-list channel under them, filled in when the
    // panel ends); anywhere else a section is its title and a hairline.
    ImDrawList* panelList = nullptr;        // the draw list the panels being drawn go on (null: plain sections)
    float panelTop = -1;                    // the open panel's top (screen y), or -1
    void beginPanels() {
        panelList = ImGui::GetWindowDrawList();
        panelList->ChannelsSplit(2);
        panelList->ChannelsSetCurrent(1);
        panelTop = -1;
    }
    void closePanel() {
        if (!panelList || panelTop < 0 || panelList != ImGui::GetWindowDrawList()) return;
        const float x0 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x - 6 * scale;
        const float x1 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x + 6 * scale;
        const float y1 = ImGui::GetCursorScreenPos().y + 3 * scale;
        panelList->ChannelsSetCurrent(0);
        panelList->AddRectFilled(ImVec2(x0, panelTop), ImVec2(x1, y1), col(kPanelBox), 5 * scale);
        panelList->ChannelsSetCurrent(1);
        panelTop = -1;
        ImGui::Dummy(ImVec2(0, 10 * scale));
    }
    void endPanels() {
        if (!panelList) return;
        if (panelList == ImGui::GetWindowDrawList()) closePanel();
        panelList->ChannelsMerge();
        panelList = nullptr;
    }
    // "WALKABLE AREA" -> "Walkable Area": Blender's panels are titled in title case
    static std::string titleCase(const char* label) {
        std::string t = label;
        bool start = true;
        for (char& c : t) {
            if (std::isalpha(static_cast<unsigned char>(c))) { c = static_cast<char>(start ? std::toupper(static_cast<unsigned char>(c)) : std::tolower(static_cast<unsigned char>(c))); start = false; }
            else start = c == ' ' || c == '-';
        }
        return t;
    }
    void section(const char* label) {
        const std::string title = titleCase(label);
        if (panelList && panelList == ImGui::GetWindowDrawList()) {
            closePanel();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight() + 2 * scale;
            panelTop = p.y - 2 * scale;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float ax = p.x + 5 * scale, ay = p.y + h * 0.5f - 1 * scale, a = 3.5f * scale;   // the ▾
            dl->AddTriangleFilled(ImVec2(ax - a, ay - a * 0.6f), ImVec2(ax + a, ay - a * 0.6f), ImVec2(ax, ay + a * 0.8f), col(0xc0c0c0));
            dl->AddText(ImVec2(p.x + 16 * scale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f - 1 * scale), col(kText), title.c_str());
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h));
            return;
        }
        if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 6 * scale));
        caption(title.c_str());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), col(kLine), 1.0f);
        ImGui::Dummy(ImVec2(0, 5 * scale));
    }
    // The caption alone (section headers, the explorer's title, the start screen's lists).
    void caption(const char* label) {
        ImGui::PushFont(fontOr(g_fontBold));
        ImGui::PushStyleColor(ImGuiCol_Text, hex(kHeading));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // ── The Inspector (right): the selection's and the environment's settings, one tab each ──
    enum class InspectorTab { Selection, World, Environment, Bundle, Headset };
    InspectorTab inspectorTab = InspectorTab::Selection;
    int lastSelected = -1, lastWorldKind = -1, lastWorldIndex = -1;   // selecting something new shows its tab

    // Angles read back from a transform come out as -0.0001, shown as "-0": within `half` of zero they're shown as 0.
    static void tidyZero(float* v, float half) { for (int k = 0; k < 3; ++k) if (std::fabs(v[k]) < half) v[k] = 0.0f; }

    // A field's label, right-aligned in a column on its left, as Blender lays out properties; the field takes the rest.
    void fieldLabel(const char* text) {
        const float x0 = ImGui::GetCursorPosX();
        const float colW = std::clamp(ImGui::GetContentRegionAvail().x * 0.38f, 64 * scale, 140 * scale);
        const float tw = ImGui::CalcTextSize(text).x;
        ImGui::AlignTextToFramePadding();
        ImGui::SetCursorPosX(x0 + std::max(0.0f, colW - tw - 8 * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xb8b8b8));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::SameLine(x0 + colW);
        ImGui::SetNextItemWidth(-1);
    }

    // Properties: the tabs down its left edge as icons (Blender's), the chosen tab's panels beside them.
    void inspector(float height) {
        if (selected != lastSelected || wKind != lastWorldKind || wIndex != lastWorldIndex) {
            if (selected >= 0 || wKind >= 0) inspectorTab = InspectorTab::Selection;
            lastSelected = selected; lastWorldKind = wKind; lastWorldIndex = wIndex;
        }
        static const struct { InspectorTab tab; ToolIcon icon; const char* tip; } kTabs[] = {
            {InspectorTab::Selection, ToolIcon::Object, "Object: the object or world item you selected"},
            {InspectorTab::World, ToolIcon::Walk, "World: where you can walk, the walkable area and its settings"},
            {InspectorTab::Environment, ToolIcon::Scene, "Environment: its name, picture, sound, what it's made of, and its headset settings"},
            {InspectorTab::Bundle, ToolIcon::Bundle, "Bundle: several environments packed into one file, to share or install together"},
            {InspectorTab::Headset, ToolIcon::Headset, "Headset: send it to the Steam Frame, or install Frame Home there"},
        };
        forceInspectorTab = false;
        const float colW = 34 * scale, side = 26 * scale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(kBar));
        ImGui::BeginChild("tabs", ImVec2(colW, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::SetCursorPos(ImVec2((colW - side) * 0.5f, 6 * scale));
        for (const auto& t : kTabs) {
            ImGui::PushID(static_cast<int>(t.tab));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##tab", ImVec2(side, side))) inspectorTab = t.tab;
            const bool on = inspectorTab == t.tab, hot = ImGui::IsItemHovered();
            if (on) dl->AddRectFilled(ImVec2(p.x - 3 * scale, p.y), ImVec2(p.x + side + (colW - side) * 0.5f + 1, p.y + side), col(kPanel), 5 * scale,
                                      ImDrawFlags_RoundCornersLeft);
            else if (hot) dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), col(kWidget), 5 * scale);
            drawToolIcon(dl, t.icon, ImVec2(p.x + side * 0.5f, p.y + side * 0.5f), side * 0.6f, col(on ? 0xffffff : 0xb0b0b0));
            if (hot) ImGui::SetTooltip("%s", t.tip);
            ImGui::PopID();
            ImGui::SetCursorPosX((colW - side) * 0.5f);
            ImGui::Dummy(ImVec2(0, 2 * scale));
        }
        roundAreaBottom(4);
        ImGui::EndChild();
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 8 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * scale, 3 * scale));
        ImGui::BeginChild("inspectorBody", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding);
        beginPanels();
        switch (inspectorTab) {
            case InspectorTab::Selection: selectionTab(); break;
            case InspectorTab::World: worldTab(); break;
            case InspectorTab::Environment: environmentTab(); break;
            case InspectorTab::Bundle: bundleTab(); break;
            case InspectorTab::Headset: headsetTab(); break;
        }
        endPanels();
        roundAreaBottom(8);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    bool forceInspectorTab = false;         // inspectorTab was set in code (the icon column shows it as it is)
    void showInspectorTab(InspectorTab t) { inspectorTab = t; forceInspectorTab = true; }

    // Selected: the world item or object picked in the view or the Explorer.
    void selectionTab() {
        if (scene && wKind >= 0 && wIndex >= 0 && wIndex < worldCount(wKind)) {
            section("WORLD ITEM");
            ImGui::TextUnformatted(worldName(wKind, wIndex).c_str());
            ImGui::TextDisabled("%s", kWorldNames[wKind]);
            ImGui::Spacing();
            bool changed = false, letGo = false;   // letGo: a field edited and let go
            std::shared_ptr<HomeWorld> before;   // a one-shot edit (a combo, a checkbox): its undo step, pushed below
            // Fields and drags: the layer as the field was activated, kept across frames and pushed as one undo step
            // the first time the value really changes (just clicking a field leaves the history alone).
            auto track = [&]() {
                if (ImGui::IsItemActivated() && !fieldBefore) fieldBefore = std::make_shared<HomeWorld>(scene->world);
                if (ImGui::IsItemEdited()) {
                    changed = true;
                    if (fieldBefore && !fieldPushed) { UndoStep u = {}; u.world = fieldBefore; undo.push_back(u); redo.clear(); fieldPushed = true; }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) letGo = true;
                if (ImGui::IsItemDeactivated()) { fieldBefore.reset(); fieldPushed = false; }
            };
            if (WorldPoint* pt = worldPoint(wKind, wIndex)) {
                char nm[128];
                std::snprintf(nm, sizeof nm, "%s", pt->name.c_str());
                fieldLabel("Name");
                if (ImGui::InputText("##wname", nm, sizeof nm)) { pt->name = nm; changed = true; }
                track();
                fieldLabel("Position");
                ImGui::DragFloat3("##wpos", pt->pos, 0.01f, 0, 0, "%.2f m"); track();
                float deg = pt->yaw * 180.0f / kPi;
                fieldLabel("Facing");
                if (ImGui::DragFloat("##wyaw", &deg, 1.0f, -360, 360, "%.0f\u00b0")) pt->yaw = deg * kPi / 180.0f;
                track();
                if (wKind == kSeat) { fieldLabel("Stand up at"); ImGui::DragFloat3("##wexit", pt->exit, 0.01f, 0, 0, "%.2f m"); track(); }
            } else if (wKind == kCollider) {
                WorldCollider& c = scene->world.colliders[wIndex];
                static const char* kinds[] = {"Box", "Sphere", "Capsule", "Mesh"};
                fieldLabel("Role");
                const char* roles[4] = {"Surface", c.kind == WorldCollider::Mesh ? "Collider" : "Invisible wall", "Walk through", "Drawn floor"};
                static const int roleOf[4] = {WorldCollider::Solid, WorldCollider::Object, WorldCollider::Trigger, WorldCollider::Walkable};
                int role = c.role == WorldCollider::Trigger ? 2 : c.role == WorldCollider::Object ? 1 : c.role == WorldCollider::Walkable ? 3 : 0;
                if (ImGui::Combo("##wrole", &role, roles, 4) && roleOf[role] != c.role) {
                    before = std::make_shared<HomeWorld>(scene->world);
                    c.role = roleOf[role];
                    changed = true;
                }
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", kRoleHelp[std::clamp(c.role, 0, 5)]);
                ImGui::PopTextWrapPos();
                fieldLabel("Shape");
                ImGui::Text("%s%s", kinds[std::clamp(c.kind, 0, 3)],
                            c.kind == WorldCollider::Mesh ? (" \u00b7 " + std::to_string(c.indices.size() / 3) + " triangles").c_str() : "");
                if (!c.mesh.empty()) {
                    // Made from a home object: it follows the object, so the object is what moves.
                    int obj = -1;
                    for (size_t o = 0; o < objects.size(); ++o) if (objectKey(objects[o]) == c.mesh) obj = static_cast<int>(o);
                    ImGui::TextDisabled("Made from %s \u2014 it moves with it.", obj >= 0 ? objects[obj].label.c_str() : "a home object");
                    if (obj >= 0 && ImGui::Button("Select the Object")) { selected = obj; wKind = wIndex = -1; }
                } else {
                    float t[3], r[3], sc[3];
                    ImGuizmo::DecomposeMatrixToComponents(c.world, t, r, sc);
                    bool moved = false;
                    fieldLabel("Location"); moved |= ImGui::DragFloat3("##wloc", t, 0.01f, 0, 0, "%.2f m"); track();
                    fieldLabel("Rotation"); tidyZero(r, 0.001f); moved |= ImGui::DragFloat3("##wrot", r, 0.5f, 0, 0, "%.0f\u00b0"); track();
                    if (c.kind == WorldCollider::Mesh) { fieldLabel("Scale"); moved |= ImGui::DragFloat3("##wscl", sc, 0.01f, 0.01f, 1000.0f, "%.2f"); track(); }
                    if (moved) ImGuizmo::RecomposeMatrixFromComponents(t, r, sc, c.world);
                    if (c.kind == WorldCollider::Box) {
                        float size[3] = {c.half[0] * 2, c.half[1] * 2, c.half[2] * 2};
                        fieldLabel("Size");
                        if (ImGui::DragFloat3("##wsize", size, 0.01f, 0.02f, 200.0f, "%.2f m")) for (int k = 0; k < 3; ++k) c.half[k] = std::max(0.01f, size[k] * 0.5f);
                        track();
                    }
                    if (c.kind == WorldCollider::Sphere || c.kind == WorldCollider::Capsule) { fieldLabel("Radius"); ImGui::DragFloat("##wrad", &c.radius, 0.01f, 0.01f, 50.0f, "%.2f m"); track(); }
                    if (c.kind == WorldCollider::Capsule) { fieldLabel("Length"); ImGui::DragFloat("##wlen", &c.length, 0.01f, 0.0f, 50.0f, "%.2f m"); track(); }
                }
            } else if (wKind == kWallSpot) {
                WorldWallSpot& ws = scene->world.wallSpots[wIndex];
                float t[3], r[3], sc[3];
                ImGuizmo::DecomposeMatrixToComponents(ws.world, t, r, sc);
                bool moved = false;
                fieldLabel("Location"); moved |= ImGui::DragFloat3("##wsloc", t, 0.01f, 0, 0, "%.2f m"); track();
                fieldLabel("Rotation"); tidyZero(r, 0.001f); moved |= ImGui::DragFloat3("##wsrot", r, 0.5f, 0, 0, "%.0f\u00b0"); track();
                if (moved) { sc[0] = sc[1] = sc[2] = 1; ImGuizmo::RecomposeMatrixFromComponents(t, r, sc, ws.world); }
                fieldLabel("Width"); ImGui::DragFloat("##wwid", &ws.width, 0.01f, 0.1f, 20.0f, "%.2f m"); track();
                fieldLabel("Height"); ImGui::DragFloat("##whei", &ws.height, 0.01f, 0.1f, 20.0f, "%.2f m"); track();
            }
            if (wKind == kGravity) {
                WorldGravity& g = scene->world.gravity[wIndex];
                char nm[128];
                std::snprintf(nm, sizeof nm, "%s", g.name.c_str());
                fieldLabel("Name");
                if (ImGui::InputText("##gname", nm, sizeof nm)) { g.name = nm; changed = true; }
                track();
                fieldLabel("Kind");
                int kind = std::clamp(g.kind, 0, 2);
                if (ImGui::BeginCombo("##gkind", kGravityNames[kind])) {
                    for (int k = 0; k < 3; ++k) {
                        if (k == WorldGravity::Planet && kind != k) continue;   // planet zones are no longer made (older files keep theirs)
                        if (ImGui::Selectable(kGravityNames[k], k == kind) && k != g.kind) { before = std::make_shared<HomeWorld>(scene->world); g.kind = k; changed = true; }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", kGravityHelp[std::clamp(g.kind, 0, 2)]);
                ImGui::PopTextWrapPos();
                float t[3], r[3], sc[3];
                ImGuizmo::DecomposeMatrixToComponents(g.world, t, r, sc);
                bool moved = false;
                fieldLabel(g.kind == WorldGravity::Planet ? "Centre" : "Location"); moved |= ImGui::DragFloat3("##gloc", t, 0.01f, 0, 0, "%.2f m"); track();
                if (g.kind != WorldGravity::Planet) { fieldLabel("Rotation"); tidyZero(r, 0.001f); moved |= ImGui::DragFloat3("##grot", r, 0.5f, 0, 0, "%.0f\u00b0"); track(); }
                if (moved) { sc[0] = sc[1] = sc[2] = 1; ImGuizmo::RecomposeMatrixFromComponents(t, r, sc, g.world); }
                if (g.kind == WorldGravity::Planet) { fieldLabel("Pull radius"); ImGui::DragFloat("##grad", &g.radius, 0.1f, 0.5f, 5000.0f, "%.1f m"); track(); }
                else {
                    float size[3] = {g.half[0] * 2, g.half[1] * 2, g.half[2] * 2};
                    fieldLabel("Size");
                    if (ImGui::DragFloat3("##gsize", size, 0.05f, 0.2f, 5000.0f, "%.1f m")) for (int k = 0; k < 3; ++k) g.half[k] = std::max(0.1f, size[k] * 0.5f);
                    track();
                }
                fieldLabel("Priority"); ImGui::DragInt("##gpri", &g.priority, 0.1f, -100, 100); track();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where zones overlap, the higher priority one decides down.");
            }
            if (bool* hid = worldHidden(wKind, wIndex)) {
                bool vis = !*hid;
                if (ImGui::Checkbox("Visible", &vis)) { *hid = !vis; worldEdited = dirty = true; updateTitle(); }
                ImGui::SameLine();
            }
            if (ImGui::Button(isolating ? "Show All (I)" : "Show Only This (I)")) toggleIsolate();
            if (before) {   // one undo step per drag / field
                UndoStep u = {};
                u.world = before;
                undo.push_back(u);
                redo.clear();
            }
            if (changed) { dirty = worldEdited = true; scene->world.origin = "studio"; updateTitle(); }
            if (letGo && wKind == kCollider) {   // moved or resized by a field: overlapping placed surfaces become one
                const int was = static_cast<int>(scene->world.colliders.size());
                wIndex = combineTouching(wIndex);
                if (static_cast<int>(scene->world.colliders.size()) != was) worldReplaced();
            }
            ImGui::Spacing();
            pushDanger();
            if (ImGui::Button("Delete")) deleteWorld();
            ImGui::PopStyleColor(3);
        }

        section("OBJECT");
        if (selected >= 0) {
            Object& o = objects[selected];
            ImGui::TextUnformatted(o.name.c_str());
            ImGui::TextDisabled("%s \u00b7 %zu triangles", o.surface.c_str(), o.triangles);
            ImGui::Spacing();
            float t[3], r[3], s[3];
            ImGuizmo::DecomposeMatrixToComponents(o.pivot, t, r, s);
            bool changed = false;
            bool done = false;              // a field let go: its surfaces split again if it turned
            fieldLabel("Location");
            changed |= ImGui::DragFloat3("##loc", t, 0.01f, 0, 0, "%.3f m");
            if (ImGui::IsItemActivated()) pushUndo(selected);
            done |= ImGui::IsItemDeactivatedAfterEdit();
            fieldLabel("Rotation");
            tidyZero(r, 0.001f);
            changed |= ImGui::DragFloat3("##rot", r, 0.5f, 0, 0, "%.1f\u00b0");
            if (ImGui::IsItemActivated()) pushUndo(selected);
            done |= ImGui::IsItemDeactivatedAfterEdit();
            fieldLabel("Scale");
            changed |= ImGui::DragFloat3("##scl", s, 0.01f, 0.001f, 1000.0f, "%.3f");
            if (ImGui::IsItemActivated()) pushUndo(selected);
            done |= ImGui::IsItemDeactivatedAfterEdit();
            if (changed) {
                ImGuizmo::RecomposeMatrixFromComponents(t, r, s, o.pivot);
                apply(o);
            }
            if (done) transformDone(static_cast<size_t>(selected));
            // What this object is for the player: walk on it, bump into it, both, or neither. The buttons set it;
            // the viewport tints what each part gives (floor green, wall orange) while it's selected.
            if (canBeSurface(o)) {
                const int mode = surfaceMode(o);
                ImGui::Spacing();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Surface");
                const float sp = 4 * scale, avail = ImGui::GetContentRegionAvail().x;
                float tw[kSurfModes], sum = 0;
                for (int m = 0; m < kSurfModes; ++m) sum += tw[m] = ImGui::CalcTextSize(kSurfNames[m]).x;
                const float pad = std::max(4 * scale, (avail - sum - (kSurfModes - 1) * sp) / kSurfModes);
                for (int m = 0; m < kSurfModes; ++m) {
                    if (m) ImGui::SameLine(0, sp);
                    const bool on = mode == m;
                    const uint32_t tint = m == kSurfSolid ? 0x7a6a2a : m == kSurfObject ? 0x5e3f8a : m == kSurfTrigger ? 0x3a5a7a : 0x4a4a4a;
                    ImGui::PushStyleColor(ImGuiCol_Button, on ? hex(tint) : hex(kWidget, 0.55f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? hex(tint) : hex(kWidget));
                    ImGui::PushStyleColor(ImGuiCol_Text, on ? hex(0xffffff) : hex(0xc8c8c8));
                    const std::string id = std::string(kSurfNames[m]) + "##surf" + std::to_string(m);
                    if (ImGui::Button(id.c_str(), ImVec2(tw[m] + pad, 0)) && (!on || (m != kSurfNone && autoMade(o)))) setSurfaces({static_cast<size_t>(selected)}, m);
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSurfHelp[m]);
                    if (testing) surfButtonAt[m] = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
                }
                if (testing && surfScroll) { ImGui::SetScrollHereY(0.5f); surfScroll = false; }
                ImGui::PushTextWrapPos(0);
                if (mode == kSurfNone)
                    ImGui::TextDisabled("%s", kSurfHelp[kSurfNone]);
                else {
                    ImGui::TextDisabled("%s", kSurfHelp[mode]);
                    double fa = 0, wa = 0;
                    surfaceAreas(static_cast<size_t>(selected), fa, wa);
                    auto swatch = [&](uint32_t c) {
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const float sz = ImGui::GetTextLineHeight();
                        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + sz * 0.2f), ImVec2(p.x + sz * 0.6f, p.y + sz * 0.8f), col(c), 2 * scale);
                        ImGui::Dummy(ImVec2(sz * 0.6f, sz));
                        ImGui::SameLine(0, 4 * scale);
                    };
                    if (mode == kSurfSolid) {
                        swatch(kFloorTint); ImGui::Text("flat enough %.1f m\u00b2", fa);
                        ImGui::SameLine(0, 12 * scale);
                        swatch(kWallTint); ImGui::Text("too steep %.1f m\u00b2", wa);
                    }
                    if (autoMade(o)) ImGui::TextDisabled("Made by Auto-generate Walkable Surfaces (a re-run replaces it; click its button to make it yours).");
                }
                ImGui::PopTextWrapPos();
                // a floor drawn on it by hand: always walkable, even on furniture
                ImGui::Spacing();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Floor");
                ImGui::SameLine();
                const int df = drawnFloorIndex(o);
                if (ImGui::Button(tool == Tool::DrawFloor ? "Drawing (B)" : "Draw (B)")) tool = Tool::DrawFloor;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag over the object in the view to draw walkable floor on it; Shift+drag erases.");
                ImGui::SameLine();
                if (ImGui::Button("Fill Flat Faces")) fillDrawnFloor(static_cast<size_t>(selected), true);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every face of it flat enough to stand on becomes walkable floor.");
                if (df >= 0) {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) fillDrawnFloor(static_cast<size_t>(selected), false);
                    double a = 0;
                    const WorldCollider& c = scene->world.colliders[df];
                    const std::vector<float> wp = worldMeshPositions(c);
                    for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                        const float* p0 = &wp[c.indices[t] * 3]; const float* p1 = &wp[c.indices[t + 1] * 3]; const float* p2 = &wp[c.indices[t + 2] * 3];
                        a += 0.5 * std::fabs((p1[0] - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (p1[2] - p0[2]));
                    }
                    ImGui::TextDisabled("%.1f m\u00b2 of floor drawn on it", a);
                }
            }
            ImGui::Spacing();
            bool visible = !o.hidden;
            if (ImGui::Checkbox("Visible", &visible)) toggleHidden(selected);
            ImGui::SameLine();
            if (ImGui::Button("Reset")) { pushUndo(selected); std::memcpy(o.pivot, o.pivot0, 64); apply(o); transformDone(static_cast<size_t>(selected)); }
            ImGui::SameLine();
            if (ImGui::Button(isolating ? "Show All" : "Only This")) toggleIsolate();
            ImGui::SameLine();
            pushDanger();
            if (ImGui::Button("Delete")) deleteSelected();
            ImGui::PopStyleColor(3);
        } else {
            ImGui::TextDisabled(scene ? "Nothing selected.\nClick an object in the 3D view or the Explorer." : "Open a home to start.");
        }

    }

    // World: the walkable area, as Frame Home walks it.
    void worldTab() {
        section("WALKABLE AREA");
        walkPanel();

    }

    // Environment: its name and picture, what it's made of, saving, and its own headset settings.
    void environmentTab() {
        section("ENVIRONMENT");
        if (scene) {
            fieldLabel("Name");
            {
                char nb[128];
                std::snprintf(nb, sizeof nb, "%s", envTitle.c_str());
                if (ImGui::InputTextWithHint("##envname", homeName.c_str(), nb, sizeof nb)) { envTitle = nb; dirty = true; updateTitle(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("What it's called on the headset (Environments panel) and in a saved .fenv");
            }
            fieldLabel("Picture");
            if (ImGui::Button(envImage.empty() ? "Choose\u2026##envimg" : "Change\u2026##envimg")) chooseImage();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its picture on the headset (Environments panel); without one, a view of it is taken when it's saved");
            if (!envImage.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Remove##envimg")) { envImage.clear(); dirty = true; updateTitle(); }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", envImage.substr(envImage.rfind(".image-") == std::string::npos ? 0 : envImage.rfind(".image-") + 1).c_str());
            } else {
                ImGui::SameLine();
                ImGui::TextDisabled("a view of it");
            }
            ImGui::Spacing();
            ImGui::TextWrapped("%s", homeName.c_str());
            for (size_t i = 0; i < scene->sources.size(); ++i) {
                const HslSourceInfo& src = scene->sources[i];
                ImGui::PushStyleColor(ImGuiCol_Text, hex(kOrange));
                ImGui::TextUnformatted(sourceLine(i).c_str());
                ImGui::PopStyleColor();
                if (!src.package.empty()) ImGui::TextDisabled("%s", src.package.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scene->apkPaths[i].c_str());
            }
            if (scene->sources.size() == 1 && scene->sources[0].role == "Footprint")
                ImGui::TextDisabled("Room only: its lighting, sky and outside world ship in a vista.");
            ImGui::Spacing();
            size_t live = 0, hidden = 0, deleted = 0, lightmapped = 0;
            for (const Object& o : objects) { if (o.deleted) ++deleted; else if (o.hidden) ++hidden; else ++live; }
            for (const MeshData& md : scene->meshes) lightmapped += md.hasLightmap;
            const bool clay = scene->animate != nullptr;
            fieldLabel("Objects");
            ImGui::Text("%zu shown, %zu hidden, %zu deleted", live, hidden, deleted);
            fieldLabel("Shaders");
            ImGui::Text(clay ? "built-in unlit (Clay)" : "%zu of the home's own", scene->programs.size());
            fieldLabel("Lighting");
            ImGui::Text("%s", lightmapped ? (std::to_string(lightmapped) + " baked lightmaps").c_str()
                                          : clay ? "baked into textures" : scene->hasEnvAmbient ? "light probes" : "estimated ambient");
            fieldLabel("Animation");
            if (clay) ImGui::Text("%zu animated objects, %.0f s loop", scene->animatedMeshes.size(), scene->animDuration);
            else ImGui::Text("%zu rigs + shader motion", scene->anims.size());
            ImGui::Spacing();
            ImGui::BeginDisabled(!dirty);
            if (ImGui::Button(envFile.empty() ? "Save to My Environments" : "Save")) saveToLibrary();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled(dirty ? "unsaved changes" : "saved");
        } else {
            if (ImGui::Button("Open Home\u2026", ImVec2(-1, 0))) openDialog();
            ImGui::TextDisabled("Or drop an .apk on the window.");
        }

        soundSection();
        if (scene) {
            section("WORLD SETTINGS");
            {
                std::lock_guard<std::mutex> l(statusMutex);
                if (tunedReady) { tunedReady = false; if (tunedResult != worldSettings) { worldSettings = tunedResult; dirty = true; updateTitle(); } }
            }
            const WorldSettings was = worldSettings;
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("On the headset it uses %s", worldSettings.describe().c_str());
            ImGui::PopTextWrapPos();
            fieldLabel("Resolution");
            {
                static const float kRes[] = {0, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f};
                char cur[32];
                if (worldSettings.resolution > 0) std::snprintf(cur, sizeof cur, "%.0f%%", worldSettings.resolution * 100);
                if (ImGui::BeginCombo("##wsres", worldSettings.resolution > 0 ? cur : "The headset's")) {
                    for (float v : kRes) {
                        char t[32];
                        if (v > 0) std::snprintf(t, sizeof t, "%.0f%%", v * 100);
                        if (ImGui::Selectable(v > 0 ? t : "The headset's", std::abs(worldSettings.resolution - v) < 1e-3f)) worldSettings.resolution = v;
                    }
                    ImGui::EndCombo();
                }
            }
            fieldLabel("Foveation");
            {
                bool own = worldSettings.foveation > 0;
                if (ImGui::Checkbox("##wsfovown", &own)) worldSettings.foveation = own ? 1.0f : 0.0f;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Its own foveation strength (off: the headset's level)");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                ImGui::BeginDisabled(!own);
                float fv = own ? worldSettings.foveation : 1.0f;
                if (ImGui::SliderFloat("##wsfov", &fv, 1.0f, 3.0f, own ? "%.2f" : "the headset's")) worldSettings.foveation = std::round(fv * 4) / 4;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("1 gentle .. 3 strong: where you look stays sharp, the edges of view get coarser (cheaper)");
            }
            fieldLabel("MSAA");
            {
                static const int kMsaa[] = {0, 1, 2, 4};
                static const char* kMsaaNames[] = {"The headset's", "Off", "2x", "4x"};
                int at = 0;
                for (int i = 0; i < 4; ++i) if (kMsaa[i] == worldSettings.msaa) at = i;
                if (ImGui::Combo("##wsmsaa", &at, kMsaaNames, 4)) worldSettings.msaa = kMsaa[at];
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Different from the headset's: Frame Home restarts itself to show this world");
            }
            fieldLabel("Frame rate");
            {
                static const char* kRates[] = {"The headset's", "Full", "Half"};
                int at = worldSettings.frameRate + 1;
                if (ImGui::Combo("##wsrate", &at, kRates, 3)) worldSettings.frameRate = at - 1;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Half: drawn every other display frame, turned with your head in between (twice the GPU time a frame)");
            }
            if (worldSettings != was) { worldSettings.tuned.clear(); worldSettings.clamp(); dirty = true; updateTitle(); }
            ImGui::BeginDisabled(tuning || saving || sending);
            if (ImGui::Button(tuning ? "Calculating\u2026" : "Calculate Best Settings")) calculateBestSettings();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("On the headset: walks through the world at 120 Hz at a few settings and keeps the sharpest one that holds\n"
                                  "its frame rate (at most 1%% of frames late). Your MSAA stays; foveation moves first, then resolution.\n"
                                  "Takes a few minutes; the headset cools between tries.");
            ImGui::SameLine();
            ImGui::BeginDisabled(worldSettings.empty() && worldSettings.tuned.empty());
            if (ImGui::Button("Use Headset Settings")) { worldSettings = WorldSettings(); setTuneStatus(""); dirty = true; updateTitle(); }
            ImGui::EndDisabled();
            {
                std::lock_guard<std::mutex> l(statusMutex);
                const std::string t = !tuneStatus.empty() ? tuneStatus : worldSettings.tuned;
                if (!t.empty()) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", t.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }

    }

    // The Environment tab's sound: the home's own ambience and a background sound added to it.
    void soundSection() {
        if (!scene) return;
        section("SOUND");
        fieldLabel("Sound");
        ImGui::Text("%s", scene->ambientSource.empty() ? "none of its own" : scene->ambientSource == "FMOD bank" ? "FMOD bank (ambience bed)" : "Ogg loop");
        soundPanel();
        {   // the sounds it uses, to keep: the home's own ambience (an Ogg file) and the one added to it
            const bool own = scene && !scene->ambientOgg.empty();
            ImGui::BeginDisabled(!own);
            if (ImGui::Button("Save Its Sound\u2026")) saveSoundAs(scene->ambientOgg, (scene->ambientName.empty() ? shownName() + " ambience" : scene->ambientName) + ".ogg");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", own ? "The home's own ambience as it plays (Ogg Vorbis)" : "This home has no sound of its own");
            if (sound.bytes) {
                ImGui::SameLine();
                if (ImGui::Button("Save the Added Sound\u2026")) saveSoundAs(*sound.bytes, sound.name.empty() ? sound.s.file : sound.name);
            }
        }
    }

    // Headset: send the environment to the Steam Frame, or install Frame Home there.
    void headsetTab() {
        section("STEAM FRAME");
        ImGui::TextDisabled("ssh %s  \u2192  ~/%s", frameHost, kFrameEnvDir);
        ImGui::Spacing();
        ImGui::BeginDisabled(!scene || sending);
        pushPrimary();
        if (ImGui::Button(sending ? "Sending\u2026" : "Send to Frame", ImVec2(-1, ImGui::GetFrameHeight() * 1.4f))) sendToFrame();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", status().c_str());
        ImGui::PopTextWrapPos();
        section("INSTALL FRAME HOME");
        installPanel();
        if (pcAvailable()) { section("THIS PC"); pcPanel(); }
    }

    // ── This PC: Frame Home as SteamVR's environment on the computer Studio runs on (the installer beside Studio,
    // pc/: install-frame-home-pc.ps1 on Windows, install.sh on Linux). It shows My Environments, so what's saved here
    // is in VR straight away; the Environments panel in the Steam menu switches them.
    std::thread pcJob;
    std::atomic<bool> pcBusy{false};
    std::string pcStatus;                    // (statusMutex)
    fs::path pcPayload() const { return exeDir() / L"pc"; }
    bool pcAvailable() const {
#if defined(_WIN32)
        return fs::exists(pcPayload() / L"install-frame-home-pc.ps1");
#elif defined(__APPLE__)
        return false;                        // (no SteamVR for macOS)
#else
        return fs::exists(pcPayload() / L"install.sh");
#endif
    }
    static fs::path pcRoot() {
#ifdef _WIN32
        const char* l = std::getenv("LOCALAPPDATA");
        return fs::u8path(l ? l : "") / L"Frame Home";
#else
        const char* h = std::getenv("HOME");
        return fs::u8path(h ? h : "") / "frame-home";
#endif
    }
    bool pcInstalled() const { std::error_code ec; return fs::exists(pcRoot() / L"frame-switcher.vrmanifest", ec); }
    void pcInstall(bool uninstall) {
        if (pcBusy) return;
        if (pcJob.joinable()) pcJob.join();
        pcBusy = true;
        { std::lock_guard<std::mutex> l(statusMutex); pcStatus = uninstall ? "Giving SteamVR its own environment back\u2026" : "Installing\u2026"; }
        pcJob = std::thread([this, uninstall] {
#ifdef _WIN32
            const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + narrow((pcPayload() / L"install-frame-home-pc.ps1").wstring()) + "\"" +
                                    (uninstall ? " -Uninstall" : "");
#else
            const std::string cmd = "sh \"" + narrow((pcPayload() / L"install.sh").wstring()) + "\"" + (uninstall ? " uninstall" : "");
#endif
            std::string out;
            const int rc = platform::run(cmd, pcPayload(), out);
            std::string msg = lastLine(out);   // the installer's verdict: its last "frame-install:" line
            if (const size_t at = out.rfind("frame-install: "); at != std::string::npos) msg = lastLine(out.substr(at + 15, out.find('\n', at) == std::string::npos ? std::string::npos : out.find('\n', at) - at - 15));
            if (!msg.empty()) msg[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(msg[0])));
            { std::lock_guard<std::mutex> l(statusMutex); pcStatus = rc == 0 ? msg : "Failed: " + (msg.empty() ? std::to_string(rc) : msg); }
            pcBusy = false;
        });
    }
    void pcPanel() {
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("SteamVR on this computer: your environments (My Environments) become its environment, switched in the "
                           "Steam menu's Environments panel. SteamVR's own settings are put back exactly when you remove it.");
        ImGui::Spacing();
        const bool installed = pcInstalled();
        ImGui::BeginDisabled(pcBusy);
        if (!installed) pushPrimary();
        if (ImGui::Button(pcBusy ? "Working\u2026###pcinst" : installed ? "Update Frame Home on This PC###pcinst" : "Install Frame Home on This PC###pcinst",
                          ImVec2(-1, ImGui::GetFrameHeight() * (installed ? 1.0f : 1.4f))))
            pcInstall(false);
        if (!installed) ImGui::PopStyleColor(3);
        if (installed && ImGui::Button("Remove from This PC", ImVec2(-1, 0))) pcInstall(true);
        ImGui::EndDisabled();
        std::string st;
        { std::lock_guard<std::mutex> l(statusMutex); st = pcStatus; }
        if (!st.empty()) { ImGui::Spacing(); ImGui::TextWrapped("%s", st.c_str()); }
        ImGui::PopTextWrapPos();
    }



    // The selected object's own mesh: every triangle edge, Blender orange, plus a soft fill of its faces.
    void drawSelection(ImDrawList* dl, const float* viewProj) {
        if (selected < 0 || !scene) return;
        const Object& o = objects[selected];
        if (o.hidden || o.deleted) return;
        const MeshData& md = scene->meshes[o.scene];
        float w[16], mvp[16];
        mul(o.pivot, o.offset, w);
        mul(viewProj, w, mvp);
        const size_t nv = md.positions.size() / 3;
        clip.resize(nv * 4);
        for (size_t v = 0; v < nv; ++v) {
            const float* p = &md.positions[v * 3];
            for (int k = 0; k < 4; ++k) clip[v * 4 + k] = mvp[k] * p[0] + mvp[4 + k] * p[1] + mvp[8 + k] * p[2] + mvp[12 + k];
        }
        auto screen = [&](const float* c) {
            return ImVec2(viewPos.x + (c[0] / c[3] * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - c[1] / c[3] * 0.5f) * viewSize.y);
        };
        dl->PushClipRect(viewPos, ImVec2(viewPos.x + viewSize.x, viewPos.y + viewSize.y), true);
        const size_t tris = md.indices.size() / 3;
        const size_t step = tris > 6000 ? tris / 6000 + 1 : 1;       // dense meshes: a lighter wire (keeps the frame fast)
        // Faces first (a faint tint), then the wire on top.
        for (size_t t = 0; t < tris; t += step) {
            const u32 a = md.indices[t * 3], b = md.indices[t * 3 + 1], c = md.indices[t * 3 + 2];
            if (a >= nv || b >= nv || c >= nv) continue;
            if (clip[a * 4 + 3] < kNear || clip[b * 4 + 3] < kNear || clip[c * 4 + 3] < kNear) continue;
            dl->AddTriangleFilled(screen(&clip[a * 4]), screen(&clip[b * 4]), screen(&clip[c * 4]), col(kOrange, 0.10f));
        }
        const ImU32 wire = col(kOrange, 0.85f);
        const float thick = 1.0f * scale;
        for (size_t t = 0; t < tris; t += step) {
            const u32 idx[3] = {md.indices[t * 3], md.indices[t * 3 + 1], md.indices[t * 3 + 2]};
            for (int e = 0; e < 3; ++e) {
                const u32 a = idx[e], b = idx[(e + 1) % 3];
                if (a >= nv || b >= nv) continue;
                float pa[4], pb[4];
                std::memcpy(pa, &clip[a * 4], 16);
                std::memcpy(pb, &clip[b * 4], 16);
                if (pa[3] < kNear && pb[3] < kNear) continue;
                if (pa[3] < kNear || pb[3] < kNear) {                    // clip at the near plane
                    float* in = pa[3] < kNear ? pb : pa;
                    float* out = pa[3] < kNear ? pa : pb;
                    const float k = (kNear - in[3]) / (out[3] - in[3]);
                    for (int q = 0; q < 4; ++q) out[q] = in[q] + (out[q] - in[q]) * k;
                }
                dl->AddLine(screen(pa), screen(pb), wire, thick);
            }
        }
        dl->PopClipRect();
    }

    // Blender-style axis indicator in the viewport's top-right corner.
    void drawAxes(ImDrawList* dl, const float* view) {
        const float r = 34 * scale;
        const ImVec2 c(viewPos.x + viewSize.x - sidebarWidth() - r - 18 * scale, viewPos.y + r + 18 * scale);
        struct Axis { float x, y, z; uint32_t color; const char* name; };
        Axis axes[3];
        const uint32_t colors[3] = {0xff3352, 0x8bdc00, 0x2890ff};
        const char* names[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) axes[a] = {view[a * 4], view[a * 4 + 1], view[a * 4 + 2], colors[a], names[a]};
        std::sort(axes, axes + 3, [](const Axis& a, const Axis& b) { return a.z < b.z; });
        dl->AddCircleFilled(c, r + 12 * scale, col(0x000000, 0.18f));
        for (const Axis& a : axes) {
            ImVec2 e(c.x + a.x * r, c.y - a.y * r);
            dl->AddLine(c, e, col(a.color), 2.0f * scale);
            dl->AddCircleFilled(e, 8 * scale, col(a.color));
            ImVec2 ts = ImGui::CalcTextSize(a.name);
            dl->AddText(ImVec2(e.x - ts.x * 0.5f, e.y - ts.y * 0.5f), col(0x111111), a.name);
        }
    }

    // The world layer drawn over the 3D view (x-ray: always visible): spawns cyan, seats green, hotspots yellow,
    // walls red (triggers dim), wall spots blue, walkable areas green. The selected item is drawn brighter.
    void drawWorld(ImDrawList* dl, const float* viewProj) {
        if (!scene || !showWorld) return;
        const HomeWorld& w = scene->world;
        auto project = [&](const float* p, ImVec2& out) -> bool {
            float c[4];
            for (int k = 0; k < 4; ++k) c[k] = viewProj[k] * p[0] + viewProj[4 + k] * p[1] + viewProj[8 + k] * p[2] + viewProj[12 + k];
            if (c[3] < kNear) return false;
            out = ImVec2(viewPos.x + (c[0] / c[3] * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - c[1] / c[3] * 0.5f) * viewSize.y);
            return true;
        };
        // Lines are clipped at the near plane: an edge running behind the camera (a big wall you stand in) is cut
        // where it leaves the view instead of being dropped or flung across the screen.
        auto line = [&](const float* a, const float* b, ImU32 color, float t) {
            float ca[4], cb[4];
            for (int k = 0; k < 4; ++k) {
                ca[k] = viewProj[k] * a[0] + viewProj[4 + k] * a[1] + viewProj[8 + k] * a[2] + viewProj[12 + k];
                cb[k] = viewProj[k] * b[0] + viewProj[4 + k] * b[1] + viewProj[8 + k] * b[2] + viewProj[12 + k];
            }
            if (ca[3] < kNear && cb[3] < kNear) return;
            if (ca[3] < kNear || cb[3] < kNear) {
                float* in = ca[3] < kNear ? cb : ca;
                float* out = ca[3] < kNear ? ca : cb;
                const float f = (kNear - in[3]) / (out[3] - in[3]);
                for (int k = 0; k < 4; ++k) out[k] = in[k] + (out[k] - in[k]) * f;
            }
            auto screen = [&](const float* c) { return ImVec2(viewPos.x + (c[0] / c[3] * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - c[1] / c[3] * 0.5f) * viewSize.y); };
            dl->AddLine(screen(ca), screen(cb), color, t);
        };
        auto xf = [](const float* m, float x, float y, float z, float* o) { for (int k = 0; k < 3; ++k) o[k] = m[k] * x + m[4 + k] * y + m[8 + k] * z + m[12 + k]; };
        dl->PushClipRect(viewPos, ImVec2(viewPos.x + viewSize.x, viewPos.y + viewSize.y), true);
        const float th = 1.5f * scale;
        // Points: a floor ring, a facing arrow and the name.
        auto points = [&](const std::vector<WorldPoint>& v, int kind, uint32_t color, float radius) {
            for (size_t i = 0; i < v.size(); ++i) {
                if (!worldShown(kind, static_cast<int>(i))) continue;
                const WorldPoint& p = v[i];
                const bool sel = wKind == kind && wIndex == static_cast<int>(i), hov = hoverKind == kind && hoverIndex == static_cast<int>(i);
                const ImU32 c = hov && !sel ? col(0xfff2b0) : col(color, sel ? 1.0f : 0.8f);
                float prev[3] = {p.pos[0] + radius, p.pos[1], p.pos[2]};
                for (int k = 1; k <= 24; ++k) {
                    const float a = k * 2 * kPi / 24, cur[3] = {p.pos[0] + radius * std::cos(a), p.pos[1], p.pos[2] + radius * std::sin(a)};
                    line(prev, cur, c, sel || hov ? th * 2 : th);
                    std::memcpy(prev, cur, 12);
                }
                const float fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
                const float tip[3] = {p.pos[0] + fx * radius * 1.8f, p.pos[1], p.pos[2] + fz * radius * 1.8f};
                line(p.pos, tip, c, sel ? th * 2 : th);
                if (kind == kSeat) {   // a little chair: seat square and back
                    const float bx[3] = {p.pos[0] - fx * 0.25f, p.pos[1], p.pos[2] - fz * 0.25f}, top[3] = {bx[0], bx[1] + 0.5f, bx[2]};
                    line(bx, top, c, th);
                }
                ImVec2 sp;
                const float lab[3] = {p.pos[0], p.pos[1] + 0.15f, p.pos[2]};
                if (project(lab, sp)) dl->AddText(ImVec2(sp.x + 6 * scale, sp.y - 8 * scale), c, (p.name.empty() ? kWorldNames[kind] : p.name).c_str());
            }
        };
        points(w.spawns, kSpawn, kShowColors[kShowSpawns], 0.3f);
        points(w.seats, kSeat, kShowColors[kShowSeats], 0.25f);
        points(w.hotspots, kHotspot, kShowColors[kShowHotspots], 0.4f);
        if (wKind == kSeat && wIndex >= 0 && wIndex < static_cast<int>(w.seats.size())) {   // where you stand up: drag it
            const WorldPoint& st = w.seats[wIndex];
            float ex[3], prev[3];
            seatExitWorld(st, ex);
            const float from[3] = {st.pos[0], ex[1] + 0.02f, st.pos[2]}, at[3] = {ex[0], ex[1] + 0.02f, ex[2]};
            line(from, at, col(0x7cf0a0, 0.7f), th);
            for (int k = 0; k <= 24; ++k) {
                const float a = k * 2 * kPi / 24, cur[3] = {ex[0] + 0.18f * std::cos(a), ex[1] + 0.02f, ex[2] + 0.18f * std::sin(a)};
                if (k) line(prev, cur, col(0x7cf0a0), th * 1.5f);
                std::memcpy(prev, cur, 12);
            }
            ImVec2 sp;
            if (project(at, sp)) dl->AddText(ImVec2(sp.x + 10 * scale, sp.y - 8 * scale), col(0x7cf0a0), "stand up here (drag)");
        }
        if (brushShown) {   // the Draw Floor brush, flat on the object under the mouse
            float prev[3];
            for (int k = 0; k <= 32; ++k) {
                const float a = k * 2 * kPi / 32, cur[3] = {brushAt[0] + floorBrush * std::cos(a), brushAt[1] + 0.01f, brushAt[2] + floorBrush * std::sin(a)};
                if (k) line(prev, cur, col(ImGui::GetIO().KeyShift ? 0xff5050 : kFloorTint), th * 1.5f);
                std::memcpy(prev, cur, 12);
            }
        }
        // Surfaces in their role's colour. Walkable floors are filled where you can stand; the rest are wireframes.
        // Big meshes are thinned to a budget of triangles per frame so the viewport stays smooth.
        // The selected object's own surfaces are always drawn, filled: floor parts green, wall parts orange.
        const std::string selKey = selected >= 0 && selected < static_cast<int>(objects.size()) && objectShown(objects[selected]) ? objectKey(objects[selected]) : "";
        size_t meshTris = 0;
        for (size_t i = 0; i < w.colliders.size(); ++i)
            if (w.colliders[i].kind == WorldCollider::Mesh && worldShown(kCollider, static_cast<int>(i))) meshTris += w.colliders[i].indices.size() / 3;
        const size_t budgetStep = meshTris > 30000 ? meshTris / 30000 + 1 : 1;
        double legendFloor = -1, legendWall = -1;
        // The selected object's parts last, its floor on top: on a sofa the seat reads green over the orange sides.
        std::vector<size_t> order, selFloors;
        for (size_t i = 0; i < w.colliders.size(); ++i) if (selKey.empty() || w.colliders[i].mesh != selKey) order.push_back(i);
        for (size_t i = 0; i < w.colliders.size(); ++i)
            if (!selKey.empty() && w.colliders[i].mesh == selKey) (w.colliders[i].role == WorldCollider::Floor ? selFloors : order).push_back(i);
        order.insert(order.end(), selFloors.begin(), selFloors.end());
        for (size_t i : order) {
            const WorldCollider& c = w.colliders[i];
            const bool ofSel = !selKey.empty() && (c.mesh == selKey || c.mesh == selKey + "#floor");
            if (ofSel ? c.hidden : !worldShown(kCollider, static_cast<int>(i))) continue;   // (a part's own eye still hides it)
            const bool sel = wKind == kCollider && wIndex == static_cast<int>(i), hov = hoverKind == kCollider && hoverIndex == static_cast<int>(i);
            const uint32_t rc = kRoleColors[std::clamp(c.role, 0, 5)];
            const ImU32 color = col(sel ? 0xffffff : hov ? 0xfff2b0 : rc, sel || hov ? 1.0f : c.role == WorldCollider::Trigger ? 0.45f : 0.8f);
            const float t = sel || hov ? th * 2 : th;
            if (c.kind == WorldCollider::Box) {
                float v[8][3];
                for (int k = 0; k < 8; ++k) xf(c.world, (k & 1 ? 1 : -1) * c.half[0], (k & 2 ? 1 : -1) * c.half[1], (k & 4 ? 1 : -1) * c.half[2], v[k]);
                static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : e) line(v[ed[0]], v[ed[1]], color, t);
                if (c.role == WorldCollider::Floor || c.role == WorldCollider::Solid) {   // its top is floor
                    ImVec2 a, b, cc, d;
                    if (project(v[2], a) && project(v[3], b) && project(v[7], cc) && project(v[6], d))
                        dl->AddQuadFilled(a, b, cc, d, col(kRoleColors[WorldCollider::Floor], 0.18f));
                }
            } else if (c.kind == WorldCollider::Sphere || c.kind == WorldCollider::Capsule) {
                const float hl = c.kind == WorldCollider::Capsule ? c.length * 0.5f : 0.0f;
                for (float yy : {-hl, hl}) {
                    float prev[3];
                    xf(c.world, c.radius, yy, 0, prev);
                    for (int k = 1; k <= 20; ++k) {
                        const float a = k * 2 * kPi / 20;
                        float cur[3];
                        xf(c.world, c.radius * std::cos(a), yy, c.radius * std::sin(a), cur);
                        line(prev, cur, color, t);
                        std::memcpy(prev, cur, 12);
                    }
                }
                for (float sx : {-1.0f, 1.0f}) { float a[3], b[3]; xf(c.world, sx * c.radius, -hl, 0, a); xf(c.world, sx * c.radius, hl, 0, b); line(a, b, color, t); }
            } else if (c.kind == WorldCollider::Mesh) {
                // Each face as Frame Home takes it (WorldRuntime): a floor surface stands you on faces over ~60 deg
                // up, a wall blocks with faces under ~45, Floor + walls does both by slope; triggers are wireframes.
                const size_t nv = c.positions.size() / 3, tris = c.indices.size() / 3;
                const size_t step = sel || ofSel ? std::max<size_t>(1, tris / 30000 + 1) : budgetStep;
                const bool hot = sel || ofSel;
                const bool edges = tris / step < 4000;   // a dense mesh's edges would paint it solid
                for (size_t q = 0; q < tris; q += step) {
                    const u32 ia = c.indices[q * 3], ib = c.indices[q * 3 + 1], ic = c.indices[q * 3 + 2];
                    if (ia >= nv || ib >= nv || ic >= nv) continue;
                    float a[3], b[3], d[3];
                    xf(c.world, c.positions[ia * 3], c.positions[ia * 3 + 1], c.positions[ia * 3 + 2], a);
                    xf(c.world, c.positions[ib * 3], c.positions[ib * 3 + 1], c.positions[ib * 3 + 2], b);
                    xf(c.world, c.positions[ic * 3], c.positions[ic * 3 + 1], c.positions[ic * 3 + 2], d);
                    if (c.role == WorldCollider::Trigger) { line(a, b, color, 1.0f); line(b, d, color, 1.0f); line(d, a, color, 1.0f); continue; }
                    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
                    const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (L < 1e-9f) continue;
                    const float up = std::fabs(n[1]) / L;
                    const bool obj = c.role == WorldCollider::Object;   // a collider: all of it stops you
                    const bool drawn = c.role == WorldCollider::Floor || c.role == WorldCollider::Walkable;
                    const bool isFloor = !obj && (drawn ? up > 0.5f : c.role == WorldCollider::Solid && up > 0.7f);
                    const bool isWall = obj || (!isFloor && !drawn && up < 0.7f);
                    if (!isFloor && !isWall) continue;
                    ImVec2 pa, pb, pc;
                    if (!project(a, pa) || !project(b, pb) || !project(d, pc)) continue;
                    const uint32_t tint = obj ? kRoleColors[WorldCollider::Object] : isFloor ? kFloorTint : kWallTint;
                    dl->AddTriangleFilled(pa, pb, pc, col(tint, hot ? (isFloor ? 0.55f : 0.32f) : isFloor ? 0.22f : 0.13f));
                    if (edges || sel) dl->AddTriangle(pa, pb, pc, col(sel ? 0xffffff : tint, hot ? 0.85f : isFloor ? 0.5f : 0.22f), 1.0f);
                }
                if (ofSel) legendFloor = 0;
                ImVec2 cp;   // the pivot: click it to select
                if (c.mesh.empty() && project(&c.world[12], cp)) {
                    const float r = (sel ? 6.0f : 4.5f) * scale;
                    dl->AddQuadFilled(ImVec2(cp.x, cp.y - r), ImVec2(cp.x + r, cp.y), ImVec2(cp.x, cp.y + r), ImVec2(cp.x - r, cp.y), col(sel ? 0xffffff : rc));
                }
            }
        }
        if (legendFloor < 0 && walkGuideOn && walkShown) {   // the walkable area's colours, above the timeline
            char a[64];
            std::snprintf(a, sizeof a, "Walkable  %.0f m\u00b2", walkReport.walkArea);
            const char* items[4] = {a, "Bridged seam", "Edge", "Out of reach"};
            const uint32_t cols[4] = {0x2ec756, 0x1a8cff, 0xff7314, 0x808080};
            const float lh = ImGui::GetTextLineHeight(), sw = lh * 0.7f, gap = 8 * scale;
            float wsum = 0;
            for (const char* t : items) wsum += ImGui::CalcTextSize(t).x + sw + gap * 2;
            const ImVec2 p0(viewPos.x + 10 * scale, viewPos.y + viewSize.y - 34 * scale - lh - 22 * scale);
            dl->AddRectFilled(p0, ImVec2(p0.x + wsum + 10 * scale, p0.y + lh + 12 * scale), col(kBar, 0.9f), 6 * scale);
            float x = p0.x + 8 * scale;
            const float y = p0.y + 6 * scale;
            for (int k = 0; k < 4; ++k) {
                dl->AddRectFilled(ImVec2(x, y + lh * 0.15f), ImVec2(x + sw, y + lh * 0.85f), col(cols[k]), 2 * scale);
                x += sw + gap * 0.6f;
                dl->AddText(ImVec2(x, y), col(0xc8c8c8), items[k]);
                x += ImGui::CalcTextSize(items[k]).x + gap * 1.4f;
            }
        }
        if (legendFloor >= 0) {   // what the tint means, above the timeline
            surfaceAreas(static_cast<size_t>(selected), legendFloor, legendWall);
            char ft[64], wt[64];
            std::snprintf(ft, sizeof ft, "Floor  %.1f m\u00b2", legendFloor);
            std::snprintf(wt, sizeof wt, "Wall  %.1f m\u00b2", legendWall);
            const float lh = ImGui::GetTextLineHeight(), sw = lh * 0.7f, gap = 8 * scale;
            const char* head = kSurfNames[surfaceMode(objects[selected])];
            const float wHead = ImGui::CalcTextSize(head).x, wf = ImGui::CalcTextSize(ft).x, ww = ImGui::CalcTextSize(wt).x;
            const ImVec2 p0(viewPos.x + 10 * scale, viewPos.y + viewSize.y - 34 * scale - lh - 22 * scale);
            const ImVec2 p1(p0.x + wHead + wf + ww + 2 * sw + 6 * gap + 10 * scale, p0.y + lh + 12 * scale);
            dl->AddRectFilled(p0, p1, col(kBar, 0.9f), 6 * scale);
            float x = p0.x + 8 * scale;
            const float y = p0.y + 6 * scale;
            dl->AddText(ImVec2(x, y), col(kText), head);
            x += wHead + 2 * gap;
            for (int k = 0; k < 2; ++k) {
                dl->AddRectFilled(ImVec2(x, y + lh * 0.15f), ImVec2(x + sw, y + lh * 0.85f), col(k ? kWallTint : kFloorTint), 2 * scale);
                x += sw + gap * 0.6f;
                dl->AddText(ImVec2(x, y), col(0xc8c8c8), k ? wt : ft);
                x += (k ? ww : wf) + gap * 1.4f;
            }
        }
        for (size_t i = 0; i < w.wallSpots.size(); ++i) {
            if (!worldShown(kWallSpot, static_cast<int>(i))) continue;
            const WorldWallSpot& ws = w.wallSpots[i];
            const bool sel = wKind == kWallSpot && wIndex == static_cast<int>(i), hov = hoverKind == kWallSpot && hoverIndex == static_cast<int>(i);
            float v[4][3];
            const float hw = ws.width * 0.5f, hh = ws.height * 0.5f;
            xf(ws.world, -hw, -hh, 0, v[0]); xf(ws.world, hw, -hh, 0, v[1]); xf(ws.world, hw, hh, 0, v[2]); xf(ws.world, -hw, hh, 0, v[3]);
            for (int k = 0; k < 4; ++k) line(v[k], v[(k + 1) % 4], hov && !sel ? col(0xfff2b0) : col(0x5b8def, sel ? 1.0f : 0.8f), sel || hov ? th * 2 : th);
        }
        // Gravity zones: the region, and arrows showing which way is down.
        for (size_t i = 0; i < w.gravity.size(); ++i) {
            if (!worldShown(kGravity, static_cast<int>(i))) continue;
            const WorldGravity& g = w.gravity[i];
            const bool sel = wKind == kGravity && wIndex == static_cast<int>(i);
            const ImU32 color = col(sel ? 0xffffff : kShowColors[kShowGravity], sel ? 1.0f : 0.75f);
            const float t = sel ? th * 2 : th;
            auto arrow = [&](const float* from, const float* to) {
                line(from, to, color, t);
                const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                const float L = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (L < 1e-4f) return;
                float sd[3] = {-d[2], 0, d[0]};                 // a side direction for the head
                const float sl = std::sqrt(sd[0] * sd[0] + sd[2] * sd[2]);
                if (sl < 1e-4f) { sd[0] = 1; sd[2] = 0; } else { sd[0] /= sl; sd[2] /= sl; }
                for (float sgn : {-1.0f, 1.0f}) {
                    const float h[3] = {to[0] - d[0] / L * L * 0.2f + sd[0] * L * 0.1f * sgn, to[1] - d[1] / L * L * 0.2f, to[2] - d[2] / L * L * 0.2f + sd[2] * L * 0.1f * sgn};
                    line(to, h, color, t);
                }
            };
            if (g.kind == WorldGravity::Planet) {
                const float* c = &g.world[12];
                for (int axis = 0; axis < 3; ++axis) {   // three great circles of the pull radius
                    float prev[3];
                    for (int k = 0; k <= 48; ++k) {
                        const float a = k * 2 * kPi / 48, cs = std::cos(a) * g.radius, sn = std::sin(a) * g.radius;
                        const float p[3] = {c[0] + (axis == 0 ? 0 : cs), c[1] + (axis == 1 ? 0 : axis == 0 ? cs : sn), c[2] + (axis == 2 ? 0 : sn)};
                        if (k) line(prev, p, col(sel ? 0xffffff : kShowColors[kShowGravity], sel ? 0.8f : 0.35f), 1.0f);
                        std::memcpy(prev, p, 12);
                    }
                }
                for (int k = 0; k < 6; ++k) {             // arrows pointing in at the centre
                    const float d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    const float from[3] = {c[0] + d[k][0] * g.radius, c[1] + d[k][1] * g.radius, c[2] + d[k][2] * g.radius};
                    const float to[3] = {c[0] + d[k][0] * g.radius * 0.7f, c[1] + d[k][1] * g.radius * 0.7f, c[2] + d[k][2] * g.radius * 0.7f};
                    arrow(from, to);
                }
            } else {
                float v[8][3];
                for (int k = 0; k < 8; ++k) xf(g.world, (k & 1 ? 1 : -1) * g.half[0], (k & 2 ? 1 : -1) * g.half[1], (k & 4 ? 1 : -1) * g.half[2], v[k]);
                static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                for (auto& ed : e) line(v[ed[0]], v[ed[1]], color, t);
                if (g.kind == WorldGravity::Direction) {   // down: the frame's -Y
                    const float hs = std::min({g.half[0], g.half[1], g.half[2]}) * 0.8f;
                    float from[3], to[3];
                    xf(g.world, 0, hs, 0, from);
                    xf(g.world, 0, -hs, 0, to);
                    arrow(from, to);
                }
            }
            ImVec2 sp;
            if (project(&g.world[12], sp)) {
                const float r = (sel ? 6.0f : 4.5f) * scale;
                dl->AddQuadFilled(ImVec2(sp.x, sp.y - r), ImVec2(sp.x + r, sp.y), ImVec2(sp.x, sp.y + r), ImVec2(sp.x - r, sp.y), color);
                dl->AddText(ImVec2(sp.x + 8 * scale, sp.y - 8 * scale), color, (g.name.empty() ? std::string(kGravityNames[std::clamp(g.kind, 0, 2)]) : g.name).c_str());
            }
        }
        dl->PopClipRect();
    }

    // The world item under a click: points and shape centres within a few pixels.
    // The world item under a viewport pixel: a wall or surface box anywhere on it (a placed mesh on its triangles, a
    // wall spot on its panel), a spawn, seat or hotspot within a finger's width of its marker (seats: the chair drawn),
    // a gravity zone at its centre. Markers win over a box they're in front of. hit: the point grabbed (dragging keeps
    // it under the mouse).
    bool worldUnder(float mx, float my, const float* viewProj, int& kind, int& index, float hit[3]) {
        if (!scene || !showWorld) return false;
        const HomeWorld& w = scene->world;
        float o[3], d[3];
        mouseRay(mx, my, o, d);
        const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& v : d) v /= dl;
        // shapes: the nearest along the ray
        float bestT = 1e30f;
        int vk = -1, vi = -1;
        auto boxHit = [&](const float* world, const float* half, float& t) {   // ray against a box in its frame
            float inv[16], lo[3], ld[3];
            if (!invert(world, inv)) return false;
            point(inv, o, lo);
            direction(inv, d, ld);
            float t0 = 0, t1 = 1e30f;
            for (int k = 0; k < 3; ++k) {
                if (std::fabs(ld[k]) < 1e-9f) { if (std::fabs(lo[k]) > half[k]) return false; continue; }
                float a = (-half[k] - lo[k]) / ld[k], b = (half[k] - lo[k]) / ld[k];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a); t1 = std::min(t1, b);
                if (t0 > t1) return false;
            }
            if (t0 <= 0) return false;                                 // inside it (a room-sized volume): not a click on it
            t = t0;
            return true;
        };
        for (size_t i = 0; i < w.colliders.size(); ++i) {
            const WorldCollider& c = w.colliders[i];
            if (!c.mesh.empty() || !worldShown(kCollider, static_cast<int>(i))) continue;   // (object surfaces: click the object)
            // anywhere on it: boxes and what you placed; the home's triggers and big collision meshes by their centre
            if (c.role == WorldCollider::Trigger || (c.kind == WorldCollider::Mesh && !c.placed)) continue;
            float t = 1e30f;
            bool ok = false;
            if (c.kind == WorldCollider::Box) ok = boxHit(c.world, c.half, t);
            else if (c.kind == WorldCollider::Sphere || c.kind == WorldCollider::Capsule) {
                const float h[3] = {c.radius, c.radius + (c.kind == WorldCollider::Capsule ? c.length * 0.5f : 0.0f), c.radius};
                ok = boxHit(c.world, h, t);
            } else if (c.kind == WorldCollider::Mesh && c.indices.size() <= 60000) {
                const std::vector<float> tris = colliderTriangles(c);
                for (size_t k = 0; k + 8 < tris.size(); k += 9) {
                    const float* A = &tris[k]; const float* B = &tris[k + 3]; const float* C = &tris[k + 6];
                    const float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
                    const float pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                    const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                    if (std::fabs(det) < 1e-12f) continue;
                    const float id = 1 / det, tv[3] = {o[0] - A[0], o[1] - A[1], o[2] - A[2]};
                    const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * id;
                    if (u < 0 || u > 1) continue;
                    const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                    const float v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * id;
                    if (v < 0 || u + v > 1) continue;
                    const float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * id;
                    if (tt > 0 && tt < t) { t = tt; ok = true; }
                }
            }
            if (ok && t < bestT) { bestT = t; vk = kCollider; vi = static_cast<int>(i); }
        }
        for (size_t i = 0; i < w.wallSpots.size(); ++i) {               // the panel's rectangle
            const WorldWallSpot& ws = w.wallSpots[i];
            if (!worldShown(kWallSpot, static_cast<int>(i))) continue;
            const float h[3] = {ws.width * 0.5f, ws.height * 0.5f, 0.05f};
            float t;
            if (boxHit(ws.world, h, t) && t < bestT) { bestT = t; vk = kWallSpot; vi = static_cast<int>(i); }
        }
        // markers: the nearest on screen within reach, unless a shape is well in front of it
        float bestPx = 20 * scale, depth = 0;
        int pk = -1, pi = -1;
        auto marker = [&](int k, int i, const float* p, float reach) {
            float c[4];
            for (int j = 0; j < 4; ++j) c[j] = viewProj[j] * p[0] + viewProj[4 + j] * p[1] + viewProj[8 + j] * p[2] + viewProj[12 + j];
            if (c[3] < kNear) return;
            const float sx = viewPos.x + (c[0] / c[3] * 0.5f + 0.5f) * viewSize.x, sy = viewPos.y + (0.5f - c[1] / c[3] * 0.5f) * viewSize.y;
            const float dd = std::hypot(sx - mx, sy - my);
            if (dd < bestPx && dd < reach) { bestPx = dd; pk = k; pi = i; depth = (p[0] - o[0]) * d[0] + (p[1] - o[1]) * d[1] + (p[2] - o[2]) * d[2]; }
        };
        auto pointsOf = [&](const std::vector<WorldPoint>& v, int k, float r) {
            for (size_t i = 0; i < v.size(); ++i) {
                if (!worldShown(k, static_cast<int>(i))) continue;
                const WorldPoint& q = v[i];
                marker(k, static_cast<int>(i), q.pos, 20 * scale);
                const float fx = -std::sin(q.yaw), fz = -std::cos(q.yaw);
                const float tip[3] = {q.pos[0] + fx * r * 1.8f, q.pos[1], q.pos[2] + fz * r * 1.8f};
                marker(k, static_cast<int>(i), tip, 14 * scale);
                if (k == kSeat) {                                       // the chair's back
                    const float back[3] = {q.pos[0] - fx * 0.25f, q.pos[1] + 0.3f, q.pos[2] - fz * 0.25f};
                    marker(k, static_cast<int>(i), back, 16 * scale);
                }
            }
        };
        pointsOf(w.spawns, kSpawn, 0.3f);
        pointsOf(w.seats, kSeat, 0.25f);
        pointsOf(w.hotspots, kHotspot, 0.4f);
        for (size_t i = 0; i < w.gravity.size(); ++i) if (worldShown(kGravity, static_cast<int>(i))) marker(kGravity, static_cast<int>(i), &w.gravity[i].world[12], 14 * scale);
        for (size_t i = 0; i < w.colliders.size(); ++i) {              // the rest by their centre
            const WorldCollider& c = w.colliders[i];
            if (c.mesh.empty() && worldShown(kCollider, static_cast<int>(i)) && (c.role == WorldCollider::Trigger || (c.kind == WorldCollider::Mesh && !c.placed)))
                marker(kCollider, static_cast<int>(i), &c.world[12], 14 * scale);
        }
        if (pk >= 0 && (vk < 0 || depth <= bestT + 0.5f)) {
            kind = pk; index = pi;
            float m[16];
            if (worldMatrix(pk, pi, m)) { hit[0] = m[12]; hit[1] = m[13]; hit[2] = m[14]; }
            return true;
        }
        if (vk < 0) return false;
        kind = vk; index = vi;
        for (int k = 0; k < 3; ++k) hit[k] = o[k] + d[k] * bestT;
        return true;
    }
    bool pickWorld(float mx, float my, const float* viewProj) {
        int k, i;
        float hit[3];
        if (!worldUnder(mx, my, viewProj, k, i, hit)) return false;
        wKind = k; wIndex = i; selected = -1;
        return true;
    }
    int hoverKind = -1, hoverIndex = -1;    // the world item under the mouse (highlighted: click or drag it)

    void viewport(float width, float height) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("viewport", ImVec2(width, height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        viewPos = ImGui::GetCursorScreenPos();
        viewSize = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();

        // ImGuizmo only grabs when no ImGui item is hovered, and this full-viewport button always is: while the
        // mouse is over the gizmo (as of last frame) the button steps aside so the gizmo can be dragged.
        const ImVec2 end(viewPos.x + viewSize.x, viewPos.y + viewSize.y);
        bool overView;
        if (gizmoHot) {
            ImGui::Dummy(ImVec2(std::max(1.0f, viewSize.x), std::max(1.0f, viewSize.y)));
            overView = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(viewPos, end);
        } else {
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##view", ImVec2(std::max(1.0f, viewSize.x), std::max(1.0f, viewSize.y)),
                                   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            overView = ImGui::IsItemHovered();
        }
        const float tlH = 0;                            // (the timeline has its own row under the view)
        const bool overTimeline = scene && io.MousePos.y >= viewPos.y + viewSize.y - tlH && io.MousePos.y <= viewPos.y + viewSize.y &&
                                  io.MousePos.x >= viewPos.x && io.MousePos.x <= viewPos.x + viewSize.x;
        const bool hovered = overView && !overTimeline;
        dl->AddRectFilled(viewPos, end, col(kViewBg));
        if (rendered && viewTex) dl->AddImage((ImTextureID)viewTex, viewPos, end);   // (a pointer on 64-bit Vulkan, an integer on 32-bit: as ImGui's backend casts it)

        const float ty = std::tan(kFovY * 0.5f), tx = ty * std::max(1.0f, viewSize.x) / std::max(1.0f, viewSize.y);
        float pose[16], view[16], proj[16], viewProj[16];
        cam.pose(pose);
        invert(pose, view);
        glProjection(tx, ty, proj);
        mul(proj, view, viewProj);

        // Gizmo (not while the camera is being dragged or Alt is held: Alt + drag orbits)
        bool overGizmo = false, gizmoShown = false;
        const bool gizmoAllowed = !io.KeyAlt && drag < 0;
        float worldM[16];
        if (scene && wKind >= 0 && wIndex >= 0 && wIndex < worldCount(wKind) && worldShown(wKind, wIndex) && worldMatrix(wKind, wIndex, worldM)) {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetGizmoSizeClipSpace(0.14f);      // a bit bigger than ImGuizmo's default: easier to grab
            ImGuizmo::SetDrawlist(dl);
            ImGuizmo::SetRect(viewPos.x, viewPos.y, viewSize.x, viewSize.y);
            const bool point = wKind == kSpawn || wKind == kSeat || wKind == kHotspot;
            ImGuizmo::OPERATION op = tool == Tool::Move ? ImGuizmo::TRANSLATE : tool == Tool::Rotate ? (point ? ImGuizmo::ROTATE_Y : ImGuizmo::ROTATE) : ImGuizmo::SCALE;
            if (point && op == ImGuizmo::SCALE) op = ImGuizmo::TRANSLATE;
            const bool snap = io.KeyCtrl;
            const float snapValues[3] = {tool == Tool::Rotate ? 15.0f : 0.25f, tool == Tool::Rotate ? 15.0f : 0.25f, tool == Tool::Rotate ? 15.0f : 0.25f};
            ImGuizmo::Enable(gizmoAllowed || ImGuizmo::IsUsing());
            gizmoShown = true;
            const bool changed = ImGuizmo::Manipulate(view, proj, op, localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, worldM, nullptr, snap ? snapValues : nullptr);
            const bool using_ = ImGuizmo::IsUsing();
            if (using_ && !gizmoWasUsing) { pushWorldUndo(); dragBase = scene->world; }   // one undo step per drag, taken before it moves
            if (changed) setWorldMatrix(wKind, wIndex, worldM);
            if (!using_ && gizmoWasUsing && wKind == kCollider) {   // let go: overlapping placed surfaces become one (same undo step)
                const int was = static_cast<int>(scene->world.colliders.size());
                wIndex = combineTouching(wIndex);
                if (static_cast<int>(scene->world.colliders.size()) != was) worldReplaced();
            }
            gizmoWasUsing = using_;
            overGizmo = ImGuizmo::IsOver() || using_;
        } else if (scene && selected >= 0 && objectShown(objects[selected]) && tool != Tool::DrawFloor) {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetGizmoSizeClipSpace(0.14f);      // a bit bigger than ImGuizmo's default: easier to grab
            ImGuizmo::SetDrawlist(dl);
            ImGuizmo::SetRect(viewPos.x, viewPos.y, viewSize.x, viewSize.y);
            const ImGuizmo::OPERATION op = tool == Tool::Move ? ImGuizmo::TRANSLATE : tool == Tool::Rotate ? ImGuizmo::ROTATE : ImGuizmo::SCALE;
            Object& o = objects[selected];
            float before[16];
            std::memcpy(before, o.pivot, 64);
            const bool snap = io.KeyCtrl;
            const float snapValues[3] = {tool == Tool::Rotate ? 15.0f : tool == Tool::Scale ? 0.1f : 0.25f,
                                         tool == Tool::Rotate ? 15.0f : tool == Tool::Scale ? 0.1f : 0.25f,
                                         tool == Tool::Rotate ? 15.0f : tool == Tool::Scale ? 0.1f : 0.25f};
            ImGuizmo::Enable(gizmoAllowed || ImGuizmo::IsUsing());
            gizmoShown = true;
            if (ImGuizmo::Manipulate(view, proj, op, localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, o.pivot, nullptr, snap ? snapValues : nullptr))
                apply(o);
            const bool using_ = ImGuizmo::IsUsing();
            if (using_ && !gizmoWasUsing) pushUndo(selected, before);     // one undo step per drag
            if (!using_ && gizmoWasUsing) transformDone(static_cast<size_t>(selected));
            gizmoWasUsing = using_;
            overGizmo = ImGuizmo::IsOver() || using_;
        }
        if (!gizmoShown && gizmoWasUsing) {   // the selection went away mid-drag: end the drag, don't carry it over
            ImGuizmo::Enable(false);
            ImGuizmo::Enable(true);
            gizmoWasUsing = false;
        }
        gizmoHot = overGizmo;
        // The highlight is drawn by the renderer on the GPU (built once per selection; the old per-frame CPU
        // wireframe made the whole app lag on big meshes like the floor).
        {
            const bool show = selected >= 0 && objectShown(objects[selected]);
            renderer.setSelection(show ? static_cast<int>(objects[selected].mesh) : -1);
        }
        if (scene) drawWorld(dl, viewProj);
        if (scene) drawAxes(dl, view);
        if (scene && isolating) {   // say why everything else vanished, and how to get it back
            const char* msg = "Showing only the selection  \u00b7  I or Esc shows everything";
            const ImVec2 ts = ImGui::CalcTextSize(msg);
            const ImVec2 a(viewPos.x + (viewSize.x - ts.x) * 0.5f - 10 * scale, viewPos.y + 46 * scale);
            dl->AddRectFilled(a, ImVec2(a.x + ts.x + 20 * scale, a.y + ts.y + 10 * scale), col(0x1d2330, 0.92f), 6 * scale);
            dl->AddText(ImVec2(a.x + 10 * scale, a.y + 5 * scale), col(0x8fb4ff), msg);
        }

        // Camera navigation
        const ImVec2 md = io.MouseDelta;
        if (hovered && drag < 0) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) drag = 2;
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) drag = 1;
            else if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) drag = 0;
        }
        if (drag >= 0 && !ImGui::IsMouseDown(drag == 2 ? ImGuiMouseButton_Middle : drag == 1 ? ImGuiMouseButton_Right : ImGuiMouseButton_Left)) drag = -1;
        flying = drag == 1;
        float fwd[3], right[3] = {pose[0], pose[1], pose[2]}, up[3] = {pose[4], pose[5], pose[6]};
        cam.forward(fwd);
        if (drag == 2 && io.KeyShift) {                 // pan
            const float k = cam.dist * 2 * ty / std::max(1.0f, viewSize.y);
            for (int i = 0; i < 3; ++i) cam.pivot[i] += (-md.x * right[i] + md.y * up[i]) * k;
        } else if (drag == 2 && io.KeyCtrl) {           // zoom
            cam.dist = std::clamp(cam.dist * std::pow(1.01f, md.y), 0.05f, 5000.0f);
        } else if (drag == 2 || drag == 0) {            // orbit
            cam.yaw -= md.x * 0.006f;
            cam.pitch = std::clamp(cam.pitch - md.y * 0.006f, -1.55f, 1.55f);
        } else if (drag == 1) {                         // fly: look + WASD
            float e[3];
            cam.eye(e);
            cam.yaw -= md.x * 0.004f;
            cam.pitch = std::clamp(cam.pitch - md.y * 0.004f, -1.55f, 1.55f);
            cam.forward(fwd);
            const float sy = std::sin(cam.yaw), cy = std::cos(cam.yaw);
            float r2[3] = {cy, 0, -sy};
            float move[3] = {0, 0, 0};
            auto add = [&](const float* v, float s) { for (int i = 0; i < 3; ++i) move[i] += v[i] * s; };
            if (ImGui::IsKeyDown(ImGuiKey_W)) add(fwd, 1);
            if (ImGui::IsKeyDown(ImGuiKey_S)) add(fwd, -1);
            if (ImGui::IsKeyDown(ImGuiKey_D)) add(r2, 1);
            if (ImGui::IsKeyDown(ImGuiKey_A)) add(r2, -1);
            if (ImGui::IsKeyDown(ImGuiKey_E)) move[1] += 1;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) move[1] -= 1;
            if (io.MouseWheel != 0) flySpeed = std::clamp(flySpeed * std::pow(1.2f, io.MouseWheel), 0.1f, 100.0f);
            const float sp = flySpeed * io.DeltaTime * (io.KeyShift ? 4.0f : 1.0f);
            for (int i = 0; i < 3; ++i) cam.pivot[i] = e[i] + move[i] * sp + fwd[i] * cam.dist;
        }
        if (hovered && drag != 1 && io.MouseWheel != 0)
            cam.dist = std::clamp(cam.dist * std::pow(0.85f, io.MouseWheel), 0.05f, 5000.0f);

        // A right click that didn't fly the camera (no drag, no WASD): the object menu for what's under the mouse.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) { rArmed = true; rMoved = 0; }
        if (rArmed && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            rMoved += std::fabs(md.x) + std::fabs(md.y);
            for (ImGuiKey k : {ImGuiKey_W, ImGuiKey_A, ImGuiKey_S, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E}) if (ImGui::IsKeyDown(k)) rMoved += 100;
        }
        if (rArmed && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            rArmed = false;
            if (rMoved < 5 * scale) {
                const int hit = pick(io.MousePos.x, io.MousePos.y);
                if (hit >= 0) { selected = menuObject = hit; wKind = wIndex = -1; ImGui::OpenPopup("##objmenu"); }
            }
        }
        objMenuOpen = false;
        if (ImGui::BeginPopup("##objmenu")) {
            objMenuOpen = true;
            if (menuObject >= 0 && menuObject < static_cast<int>(objects.size())) objectMenu(static_cast<size_t>(menuObject));
            ImGui::EndPopup();
        }

        // What's under the mouse: highlighted, with a hand to say it can be clicked or dragged.
        hoverKind = hoverIndex = -1;
        if (hovered && itemDrag == kDragNone && !overGizmo && tool != Tool::DrawFloor && drag < 0 && scene) {
            float hh[3];
            int k, i;
            if (worldUnder(io.MousePos.x, io.MousePos.y, viewProj, k, i, hh)) { hoverKind = k; hoverIndex = i; ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
            else if (wKind == kSeat) {                                  // the selected seat's stand-up ring
                const WorldPoint* st = worldPoint(kSeat, wIndex);
                float ex[3];
                ImVec2 hs;
                if (st) { seatExitWorld(*st, ex); if (screenOf(ex, hs) && std::hypot(hs.x - io.MousePos.x, hs.y - io.MousePos.y) < 14 * scale) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); }
            }
        }
        if (itemDrag != kDragNone && dragMoved) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        // Dragging an item: pressed on it, then moved more than a few pixels (a click alone selects, below).
        if (hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overGizmo && tool != Tool::DrawFloor && scene) {
            itemDrag = kDragNone;
            dragMoved = false;
            dragFrom = io.MousePos;
            ImVec2 hs;
            float ex[3];
            const WorldPoint* seat = wKind == kSeat ? worldPoint(kSeat, wIndex) : nullptr;
            if (seat) seatExitWorld(*seat, ex);
            float hit[3];
            if (seat && screenOf(ex, hs) && std::hypot(hs.x - io.MousePos.x, hs.y - io.MousePos.y) < 12 * scale) {
                itemDrag = kDragExit; dragIndex = wIndex; dragY = ex[1];
            } else {
                int k, i;
                float grab[3];
                if (worldUnder(io.MousePos.x, io.MousePos.y, viewProj, k, i, grab)) {   // (the click below selects; this arms a drag)
                    float m[16];
                    if (worldMatrix(k, i, m)) {                         // the point grabbed stays under the mouse
                        itemDrag = kDragWorld; dragKindW = k; dragIndex = i; dragY = grab[1];
                        dragGrab[0] = m[12] - grab[0]; dragGrab[1] = m[14] - grab[2];
                    }
                } else {
                    long tri = -1;
                    float on[3];
                    const int o = pick(io.MousePos.x, io.MousePos.y);
                    if (o >= 0 && pickOn(o, io.MousePos.x, io.MousePos.y, on, &tri)) {
                        itemDrag = kDragObject; dragIndex = o; dragY = on[1];
                        dragGrab[0] = objects[o].pivot[12] - on[0]; dragGrab[1] = objects[o].pivot[14] - on[2];
                    }
                }
            }
        }
        if (itemDrag != kDragNone && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!dragMoved && std::hypot(io.MousePos.x - dragFrom.x, io.MousePos.y - dragFrom.y) > 5 * scale) {
                dragMoved = true;
                clickArmed = false;                                    // a drag, not a click
                if (itemDrag == kDragObject) { pushUndo(dragIndex); selected = dragIndex; wKind = wIndex = -1; }
                else { pushWorldUndo(); dragBase = scene->world; wKind = itemDrag == kDragExit ? kSeat : dragKindW; wIndex = dragIndex; selected = -1; }
            }
            float hit[3];
            if (dragMoved && mouseOnLevel(io.MousePos.x, io.MousePos.y, dragY, hit)) {
                if (itemDrag == kDragWorld && dragIndex < worldCount(dragKindW)) {
                    float m[16];
                    if (worldMatrix(dragKindW, dragIndex, m)) {
                        m[12] = hit[0] + dragGrab[0]; m[14] = hit[2] + dragGrab[1];
                        float f;                                       // spawns and hotspots stay on the floor (up and down steps)
                        if ((dragKindW == kSpawn || dragKindW == kHotspot) && walkShown && walkShown->floorAt(m[12], m[14], m[13], 0.5f, 1.0f, f)) m[13] = f;
                        setWorldMatrix(dragKindW, dragIndex, m);
                    }
                } else if (itemDrag == kDragExit && dragIndex < static_cast<int>(scene->world.seats.size())) {
                    WorldPoint& st = scene->world.seats[dragIndex];      // back into the seat's own frame (exit is seat-local)
                    const float c = std::cos(st.yaw), sn = std::sin(st.yaw), dx = hit[0] - st.pos[0], dz = hit[2] - st.pos[2];
                    st.exit[0] = c * dx - sn * dz;
                    st.exit[2] = sn * dx + c * dz;
                } else if (itemDrag == kDragObject && dragIndex < static_cast<int>(objects.size())) {
                    Object& ob = objects[dragIndex];
                    ob.pivot[12] = hit[0] + dragGrab[0]; ob.pivot[14] = hit[2] + dragGrab[1];
                    apply(ob);
                }
                dirty = worldEdited = true;
                updateTitle();
            }
        }
        if (itemDrag != kDragNone && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (dragMoved) {
                if (itemDrag == kDragObject) transformDone(static_cast<size_t>(dragIndex));
                else if (itemDrag == kDragWorld && dragKindW == kCollider) {
                    const int was = static_cast<int>(scene->world.colliders.size());
                    wIndex = combineTouching(dragIndex);
                    if (static_cast<int>(scene->world.colliders.size()) != was) worldReplaced();
                }
                walkPending = true;                                    // the area follows what moved
            }
            itemDrag = kDragNone;
        }
        // Draw Floor: drag over the selected object (Shift: erase); a click off it selects as usual.
        brushShown = false;
        if (tool == Tool::DrawFloor && scene && selected >= 0 && hovered && !io.KeyAlt && drag < 0 && objectShown(objects[selected])) {
            long tri = -1;
            const bool on = pickOn(selected, io.MousePos.x, io.MousePos.y, brushAt, &tri);
            brushShown = on;
            if (on && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { painting = true; pushWorldUndo(); }
            if (on && painting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) paintFloor(static_cast<size_t>(selected), brushAt, floorBrush, io.KeyShift, tri);
        }
        if (painting && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) painting = false;
        // Selection: a left click that did not drag and was not on the gizmo.
        if (hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overGizmo && !painting) { clickArmed = true; clickPos = io.MousePos; }
        if (clickArmed && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            clickArmed = false;
            const float dx = io.MousePos.x - clickPos.x, dy = io.MousePos.y - clickPos.y;
            if (dx * dx + dy * dy < 25 * scale * scale) {
                const auto p0 = std::chrono::steady_clock::now();
                if (!pickWorld(io.MousePos.x, io.MousePos.y, viewProj)) { wKind = wIndex = -1; selected = pick(io.MousePos.x, io.MousePos.y); }
                const double pms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
                if (pms > 3.0) std::fprintf(stderr, "[studio] pick took %.1f ms\n", pms);
            }
        }

        // Draw Floor's brush size, beside the toolbar under the view's name while that tool is on.
        if (scene && tool == Tool::DrawFloor) {
            const ImVec2 at(viewPos.x + (showToolbar ? 52 : 10) * scale, viewPos.y + 50 * scale);
            const float w = 180 * scale;
            dl->AddRectFilled(at, ImVec2(at.x + w + 8 * scale, at.y + ImGui::GetFrameHeight() + 8 * scale), col(kBar, 0.85f), 6 * scale);
            ImGui::SetCursorScreenPos(ImVec2(at.x + 4 * scale, at.y + 4 * scale));
            ImGui::SetNextItemWidth(w);
            ImGui::SliderFloat("##brush", &floorBrush, 0.05f, 3.0f, "brush %.2f m", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brush size \u00b7 [ and ] change it \u00b7 Shift+drag erases");
        }
        if (scene) {
            if (flying) {
                char b[64];
                std::snprintf(b, sizeof b, "Fly \u00b7 %.1f m/s (wheel)", flySpeed);
                dl->AddText(ImVec2(viewPos.x + 12 * scale, end.y - tlH - ImGui::GetTextLineHeight() - 10 * scale), col(kText), b);
            }
            // A footprint on its own has no baked lighting, sky or outside world: those ship in a vista.
            if (footprintAlone() && !uploading) {
                const char* msg = "This footprint gets its lighting, sky and outside world from a vista.";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                const float bw = ImGui::CalcTextSize("Add Vista\u2026").x + 24 * scale;
                const float total = ts.x + 12 * scale + bw;
                const ImVec2 p(viewPos.x + (viewSize.x - total) * 0.5f, viewPos.y + 48 * scale);
                dl->AddRectFilled(ImVec2(p.x - 14 * scale, p.y - 8 * scale), ImVec2(p.x + total + 14 * scale, p.y + ImGui::GetFrameHeight() + 8 * scale),
                                  col(kBar, 0.94f), 6 * scale);
                dl->AddText(ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - ts.y) * 0.5f), col(kText), msg);
                ImGui::SetCursorScreenPos(ImVec2(p.x + ts.x + 12 * scale, p.y));
                pushPrimary();
                if (ImGui::Button("Add Vista\u2026", ImVec2(bw, 0))) addVistaDialog();
                ImGui::PopStyleColor(3);
            }
            // Textures still streaming in (decoded in the background as their objects come into view).
            if (const size_t n = renderer.texturesPending()) {
                char b[96];
                std::snprintf(b, sizeof b, "Streaming %zu textures \u00b7 %zu objects in view", n, renderer.drawnCount());
                const ImVec2 ts = ImGui::CalcTextSize(b);
                dl->AddText(ImVec2(end.x - ts.x - 12 * scale, end.y - tlH - ts.y - 10 * scale), col(kDim), b);
            }
        } else if (loadState != 1) {
            startScreen();
        }
        if (loadState == 1 || uploading || texLoading) {
            // Bottom-centre pill: reading the APKs, then a progress bar while meshes and textures go to the GPU.
            const std::string msg = texLoading ? "Loading textures\u2026" : uploading ? "Uploading to the GPU\u2026"
                                                                                   : "Reading " + displayName(loadingPaths) + "\u2026";
            const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
            const float w = std::max(ts.x, 260 * scale), barH = 4 * scale;
            const ImVec2 p(viewPos.x + (viewSize.x - w) * 0.5f, end.y - ts.y - barH - 40 * scale);
            dl->AddRectFilled(ImVec2(p.x - 16 * scale, p.y - 10 * scale), ImVec2(p.x + w + 16 * scale, p.y + ts.y + barH + 18 * scale),
                              col(kBar, 0.92f), 6 * scale);
            dl->AddText(p, col(kText), msg.c_str());
            const ImVec2 b0(p.x, p.y + ts.y + 8 * scale), b1(p.x + w, b0.y + barH);
            dl->AddRectFilled(b0, b1, col(kWidget), barH);
            const bool det = uploading || texLoading;
            const float f = texLoading ? renderer.textureProgress() : uploading ? renderer.loadProgress()
                                      : 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);   // indeterminate
            const float x0 = det ? b0.x : b0.x + (w * 0.7f) * (0.5f - 0.5f * std::cos(static_cast<float>(ImGui::GetTime()) * 2.0f));
            dl->AddRectFilled(ImVec2(x0, b0.y), ImVec2(det ? b0.x + w * f : x0 + w * 0.3f, b1.y), col(kBlue), barH);
        }
        viewportToolbar();
        viewportInfo();
        viewportNavigation(pose, ty);
        viewportSidebar();
        if (!viewMaximized) roundAreaBottom(4 | 8);
        ImGui::EndChild();
    }

    // ── The window, Blender's way ───────────────────────────────────────────────────────────────────────
    // The top bar (menus and workspace tabs), the areas docked under it (3D Viewport, Outliner, Properties, Timeline:
    // each with a header whose editor button changes what the area shows, and drags it elsewhere to dock), and the
    // status bar. The gutters between areas resize them. A workspace is a layout of the areas for one job.
    enum class Editor { Viewport, Outliner, Properties, Timeline, Count };
    static constexpr int kEditors = static_cast<int>(Editor::Count);
    static constexpr const char* kEditorWindow[kEditors] = {"3D Viewport###viewport", "Outliner###outliner", "Properties###properties", "Timeline###timeline"};
    static constexpr const char* kEditorName[kEditors] = {"3D Viewport", "Outliner", "Properties", "Timeline"};
    static constexpr ToolIcon kEditorIcon[kEditors] = {ToolIcon::Viewport, ToolIcon::Outliner, ToolIcon::Properties, ToolIcon::Timeline};
    bool editorShown[kEditors] = {true, true, true, true};
    ImGuiID editorDock[kEditors] = {};      // the dock node each was in last frame
    ImGuiID editorDockNext[kEditors] = {};  // to go into this node (the editor menu swapped two areas' editors)
    ImGuiID dockspaceId = 0;
    ImVec2 editorButtonAt[kEditors] = {};   // --selftest: each area's editor button, to drag it by
    ImVec2 areaCentre[kEditors] = {};       // --selftest: each area's middle, to drop one on

    static constexpr int kWorkspaces = 3;
    static constexpr const char* kWorkspaceName[kWorkspaces] = {"Layout", "Walking", "Headset"};
    int workspace = 0;
    bool layoutWanted = false;              // build the workspace's layout (a switch, Window > Reset Layout, a first run)

    bool viewMaximized = false;             // Ctrl+Space: the 3D Viewport fills the window
    bool showToolbar = true, showSidebar = false;   // the viewport's T toolbar and N sidebar
    int sidebarTab = 0;                     // Item, Tool, View
    bool searchOpen = false;                // F3: menu search (asked for; searchShowing: on screen)
    bool searchShowing = false;
    char searchText[96] = {};
    int searchPick = 0;

    // Each area's header: an editor button (its menu changes the editor here; drag it to move the area), then the
    // editor's own menus and buttons. Drawn in the window's menu bar, so headers line up with Blender's height.
    void editorButton(Editor e) {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        const float fh = ImGui::GetFrameHeight();
        ImGui::PushID("editor");
        ImGui::PushStyleColor(ImGuiCol_Button, hex(0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(kWidgetHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(kWidget));
        if (ImGui::Button("##type", ImVec2(fh * 1.55f, fh))) ImGui::OpenPopup("##editorType");
        ImGui::PopStyleColor(3);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        editorButtonAt[static_cast<int>(e)] = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        drawToolIcon(dl, kEditorIcon[static_cast<int>(e)], ImVec2(a.x + fh * 0.5f, (a.y + b.y) * 0.5f), fh * 0.62f, col(kText));
        const float ax = b.x - fh * 0.38f, ay = (a.y + b.y) * 0.5f, s = fh * 0.12f;   // the little ▾
        dl->AddTriangleFilled(ImVec2(ax - s, ay - s * 0.5f), ImVec2(ax + s, ay - s * 0.5f), ImVec2(ax, ay + s * 0.7f), col(kDim));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6 * scale) && win->DockNode) {
            ImGui::CloseCurrentPopup();
            ImGui::StartMouseMovingWindowOrNode(win, win->DockNode, true);   // undock and carry it, as dragging a tab
        }
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s\nClick: show another editor here \u00b7 Drag: move this area and dock it elsewhere", kEditorName[static_cast<int>(e)]);
        if (ImGui::BeginPopup("##editorType")) {
            ImGui::TextDisabled("Editor Type");
            ImGui::Separator();
            for (int k = 0; k < kEditors; ++k) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::MenuItem((std::string("      ") + kEditorName[k]).c_str(), nullptr, k == static_cast<int>(e))) swapEditor(e, static_cast<Editor>(k));
                drawToolIcon(ImGui::GetWindowDrawList(), kEditorIcon[k], ImVec2(p.x + fh * 0.45f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1), fh * 0.55f, col(kText));
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // The editor menu chose `to` for the area showing `from`: they swap places (one hidden is shown here instead).
    void swapEditor(Editor from, Editor to) {
        const int f = static_cast<int>(from), t = static_cast<int>(to);
        if (f == t) return;
        if (editorShown[t] && editorDock[t]) { editorDockNext[t] = editorDock[f]; editorDockNext[f] = editorDock[t]; }
        else { editorShown[t] = true; editorDockNext[t] = editorDock[f]; editorShown[f] = false; }
    }

    // A workspace's areas, as Blender lays its default ones out: the Outliner over Properties down the right, the
    // Timeline under the 3D Viewport.
    void buildLayout(ImGuiID id, ImVec2 size) {
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(id, size);
        const float timelineFrac = std::clamp(78 * scale / std::max(1.0f, size.y), 0.08f, 0.3f);
        ImGuiID main = 0, right = 0, top = 0, bottom = 0, view = 0, timeline = 0;
        for (bool& b : editorShown) b = true;
        if (workspace == 1) {                     // Walking: the view large, Properties (World) with the Outliner as a tab
            right = ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.30f, nullptr, &main);
            timeline = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, timelineFrac, nullptr, &view);
            ImGui::DockBuilderDockWindow(kEditorWindow[2], right);
            ImGui::DockBuilderDockWindow(kEditorWindow[1], right);
            showInspectorTab(InspectorTab::World);
            showWorld = true;
        } else if (workspace == 2) {              // Headset: Properties (Headset) wide, the Outliner under it
            right = ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.40f, nullptr, &main);
            bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.30f, nullptr, &top);
            timeline = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, timelineFrac, nullptr, &view);
            ImGui::DockBuilderDockWindow(kEditorWindow[2], top);
            ImGui::DockBuilderDockWindow(kEditorWindow[1], bottom);
            showInspectorTab(InspectorTab::Headset);
        } else {                                  // Layout
            right = ImGui::DockBuilderSplitNode(id, ImGuiDir_Right, 0.25f, nullptr, &main);
            top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.38f, nullptr, &bottom);
            timeline = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, timelineFrac, nullptr, &view);
            ImGui::DockBuilderDockWindow(kEditorWindow[1], top);
            ImGui::DockBuilderDockWindow(kEditorWindow[2], bottom);
        }
        ImGui::DockBuilderDockWindow(kEditorWindow[0], view);
        ImGui::DockBuilderDockWindow(kEditorWindow[3], timeline);
        ImGui::DockBuilderFinish(id);
        for (ImGuiID& d : editorDockNext) d = 0;
    }

    void setWorkspace(int w) {
        if (w == workspace && !layoutWanted) return;
        workspace = w;
        layoutWanted = true;
        if (FILE* f = platform::openFile((dataDir() / L"workspace.txt").c_str(), "wb")) { std::fprintf(f, "%d\n", w); std::fclose(f); }
    }

    // ── The top bar: Frame Studio's menus, then the workspace tabs ──
    void topBar() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7 * scale, 5 * scale));
        const bool bar = ImGui::BeginMainMenuBar();
        ImGui::PopStyleVar(2);
        if (!bar) return;
        {   // the mark: Frame Studio's orange ring, where Blender has its logo
            const float x = ImGui::GetCursorScreenPos().x + 2 * scale, r = 6 * scale;
            const float cy = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f;
            ImGui::GetWindowDrawList()->AddCircle(ImVec2(x + r, cy), r, col(kOrange), 20, 2.2f * scale);
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x + r, cy), r * 0.38f, col(kOrange));
            ImGui::Dummy(ImVec2(r * 2 + 8 * scale, 0));
        }
        fileMenu();
        editMenu();
        if (ImGui::BeginMenu("Headset")) { headsetMenu(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Window")) { windowMenu(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Controls", "F1", showControls)) showControls = !showControls;
            if (ImGui::MenuItem("Menu Search", "F3")) openSearch();
            ImGui::EndMenu();
        }
        // The workspace tabs: the active one raised like Blender's
        ImGui::Dummy(ImVec2(14 * scale, 0));
        for (int w = 0; w < kWorkspaces; ++w) {
            const bool on = w == workspace;
            const ImVec2 ts = ImGui::CalcTextSize(kWorkspaceName[w]);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float bh = ImGui::GetWindowHeight() - 6 * scale, bw = ts.x + 18 * scale;
            ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetWindowPos().y + 3 * scale));
            ImGui::PushID(w);
            if (ImGui::InvisibleButton("##ws", ImVec2(bw, bh))) setWorkspace(w);
            const bool hot = ImGui::IsItemHovered();
            ImGui::PopID();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (on || hot) dl->AddRectFilled(a, b, col(on ? kPanel : kHeader), 4 * scale);
            dl->AddText(ImVec2(a.x + (bw - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f), col(on ? 0xffffff : kDim), kWorkspaceName[w]);
            ImGui::SameLine(0, 2 * scale);
        }
        if (scene) {   // the open home's name, at the right end as Blender shows its scene
            const std::string name = shownName() + (dirty ? " *" : "");
            const float w = ImGui::CalcTextSize(name.c_str()).x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * scale, ImGui::GetWindowWidth() - w - 16 * scale));
            ImGui::TextDisabled("%s", name.c_str());
        }
        ImGui::EndMainMenuBar();
    }

    void fileMenu() {
        if (!ImGui::BeginMenu("File")) return;
        if (ImGui::MenuItem("Open\u2026", "Ctrl+O")) openDialog();
        if (ImGui::BeginMenu("Open Recent", !recent.empty())) {
            for (size_t r = 0; r < recent.size(); ++r) {
                ImGui::PushID(static_cast<int>(r));
                if (ImGui::MenuItem(displayName(recent[r]).c_str())) { const auto paths = recent[r]; open(paths); }
                if (ImGui::IsItemHovered()) {
                    std::string tip;
                    for (const auto& p : recent[r]) tip += (tip.empty() ? "" : "\n") + p;
                    ImGui::SetTooltip("%s", tip.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        {
            const std::vector<std::string> mine = libraryEnvs();
            if (ImGui::BeginMenu("My Environments")) {
                if (mine.empty()) ImGui::TextDisabled("None yet: Save (Ctrl+S) puts what you make here");
                for (size_t r = 0; r < mine.size(); ++r) {
                    ImGui::PushID(static_cast<int>(r) + 5000);
                    if (ImGui::MenuItem(recentDetails({mine[r]}).title.c_str())) open({mine[r]});
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mine[r].c_str());
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Show the Folder")) platform::openPath(narrow(fs::path(libraryDir().c_str()).wstring()));
                ImGui::EndMenu();
            }
        }
        if (ImGui::MenuItem("Revert", nullptr, false, scene != nullptr)) revertAll();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back to the home as it shipped, without your changes");
        ImGui::Separator();
        if (ImGui::MenuItem(envFile.empty() ? "Save to My Environments" : "Save", "Ctrl+S", false, scene != nullptr && !saving && !uploading)) saveToLibrary();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", envFile.empty() ? "A new environment: saved into My Environments (Documents\\Frame Studio\\Environments) under its name"
                                                    : ("Into " + envFile).c_str());
        if (ImGui::MenuItem("Save As\u2026", "Ctrl+Shift+S", false, scene != nullptr && !saving && !uploading)) saveEnvironmentDialog();
        if (ImGui::BeginMenu("Auto-saved Versions", scene != nullptr)) {
            const std::vector<fs::path> v = versions();
            if (v.empty()) ImGui::TextDisabled("None yet: every 5 minutes with unsaved changes, one is kept (the last 10)");
            for (size_t i = 0; i < v.size(); ++i) {
                ImGui::PushID(static_cast<int>(i) + 8000);
                if (ImGui::MenuItem(narrow(v[i].stem().wstring()).c_str())) open({narrow(v[i].wstring())});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens this version on its own (Save As to make it the one you keep)");
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Saving")) {
            if (ImGui::MenuItem("Faster Saving", nullptr, fastPack)) {
                fastPack = !fastPack;
                if (FILE* f = platform::openFile((dataDir() / L"fast-pack.txt").c_str(), "wb")) { std::fputs(fastPack ? "1\n" : "0\n", f); std::fclose(f); }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Packs environments on all CPU cores: about 3x quicker to save, about 1%% bigger.\nOff: the smallest file. Either way nothing is lost.");
            if (ImGui::MenuItem("Leave Out What Can't Be Seen", nullptr, optimizeMeshes)) {
                optimizeMeshes = !optimizeMeshes;
                if (FILE* f = platform::openFile((dataDir() / L"optimize-meshes.txt").c_str(), "wb")) { std::fputs(optimizeMeshes ? "1\n" : "0\n", f); std::fclose(f); }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When saving: geometry nobody can see from anywhere they can walk, sit or teleport to (the backs of\n"
                                  "things, what's behind walls) is left out, so the headset draws less. Moved and deleted objects are kept whole.\n"
                                  "Saving takes up to a minute longer.");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Import")) {
            if (ImGui::MenuItem("Vista\u2026", nullptr, false, scene != nullptr && homePaths.size() == 1)) addVistaDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The world outside a footprint: its lighting, sky and scenery");
            if (ImGui::MenuItem("Models (.glb .gltf .fbx .obj)\u2026", nullptr, false, scene != nullptr && !uploading)) importModelDialog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Bundle")) { bundleMenuItems(); ImGui::EndMenu(); }
        ImGui::Separator();
        if (ImGui::MenuItem("Close Home", nullptr, false, scene != nullptr && !saving && !uploading && !texLoading && loadState != 1)) closeHome();
        if (ImGui::MenuItem("Quit", "Alt+F4") && window.requestClose) window.requestClose();
        ImGui::EndMenu();
    }

    void bundleMenuItems() {
        if (ImGui::MenuItem("New Bundle", nullptr, false, !bundleBusy)) newBundle();
        if (ImGui::MenuItem("Open Bundle\u2026", nullptr, false, !bundleBusy)) openBundleDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Add the Open Environment", nullptr, false, !openEnvFile().empty() && !bundleBusy)) addToBundle({openEnvFile()});
        if (ImGui::MenuItem("Add Environments\u2026", nullptr, false, !bundleBusy)) addToBundleDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Save Bundle", nullptr, false, !bundleMembers.empty() && !bundleBusy)) { if (bundleFile.empty()) saveBundleDialog(); else saveBundle(bundleFile); }
        if (ImGui::MenuItem("Save Bundle As\u2026", nullptr, false, !bundleMembers.empty() && !bundleBusy)) saveBundleDialog();
        if (ImGui::MenuItem("Install on Steam Frame\u2026", nullptr, false, !bundleFile.empty() && !bundleDirty)) installBundle();
        ImGui::Separator();
        if (ImGui::MenuItem("Bundle Panel")) showInspectorTab(InspectorTab::Bundle);
    }

    void editMenu() {
        if (!ImGui::BeginMenu("Edit")) return;
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo.empty())) step(undo, redo);
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo.empty())) step(redo, undo);
        ImGui::Separator();
        if (ImGui::MenuItem("Menu Search\u2026", "F3")) openSearch();
        ImGui::Separator();
        if (ImGui::MenuItem("Preferences\u2026")) { showSettings = true; if (!headsetLoaded && !headsetBusy) headsetSettings(false); }
        ImGui::EndMenu();
    }

    void headsetMenu() {
        if (ImGui::MenuItem("Send to Frame", nullptr, false, scene != nullptr && !sending)) sendToFrame();
        ImGui::Separator();
        if (ImGui::MenuItem("Install on Steam Frame\u2026")) openInstall();
        if (ImGui::MenuItem("Headset Panel")) showInspectorTab(InspectorTab::Headset);
        ImGui::Separator();
        if (ImGui::MenuItem("Headset Debug\u2026", nullptr, showHeadsetDebug)) {
            showHeadsetDebug = !showHeadsetDebug;
            if (showHeadsetDebug && !headsetDebugLoaded && !headsetDebugBusy) headsetDebugCall(0);
        }
    }

    void windowMenu() {
        if (ImGui::MenuItem("Toggle Maximize Area", "Ctrl+Space", viewMaximized)) viewMaximized = !viewMaximized;
        ImGui::Separator();
        for (int e = 0; e < kEditors; ++e)
            if (ImGui::MenuItem(kEditorName[e], nullptr, editorShown[e])) editorShown[e] = !editorShown[e];
        ImGui::Separator();
        if (ImGui::BeginMenu("Workspace")) {
            for (int w = 0; w < kWorkspaces; ++w) if (ImGui::MenuItem(kWorkspaceName[w], nullptr, w == workspace)) setWorkspace(w);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Theme")) {   // the MetaDB (toastconcern.github.io) themes
            for (int i = 0; i < kThemeCount; ++i)
                if (ImGui::MenuItem(kThemes[i].name, nullptr, i == g_theme)) { g_theme = i; applyTheme(scale); saveStudioPerf(); }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Reset Layout")) { layoutWanted = true; viewMaximized = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The workspace's areas back where they started");
        ImGui::Separator();
        if (ImGui::MenuItem("Home Report\u2026", nullptr, showReport, scene != nullptr && !uploading)) { showReport = !showReport; if (showReport) report.reset(); }
    }

    // ── The 3D Viewport's header menus (also Shift+A's Add, the object's right-click menu, F3) ──
    void viewMenu() {
        if (ImGui::MenuItem("Toolbar", "T", showToolbar)) showToolbar = !showToolbar;
        if (ImGui::MenuItem("Sidebar", "N", showSidebar)) showSidebar = !showSidebar;
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "Numpad .", false, selected >= 0 || wKind >= 0)) { if (wKind >= 0) frameWorld(wKind, wIndex); else frameSelected(); }
        if (ImGui::MenuItem("Frame All", "Home", false, scene != nullptr)) frameAll();
        ImGui::Separator();
        if (ImGui::MenuItem("Player View", "Numpad 0")) cam.playerView();
        if (ImGui::BeginMenu("Viewpoint")) {
            if (ImGui::MenuItem("Top", "Numpad 7")) viewFrom(0, kPi * 0.5f - 0.001f);
            if (ImGui::MenuItem("Bottom", "Ctrl+Numpad 7")) viewFrom(0, -kPi * 0.5f + 0.001f);
            ImGui::Separator();
            if (ImGui::MenuItem("Front", "Numpad 1")) viewFrom(0, 0);
            if (ImGui::MenuItem("Back", "Ctrl+Numpad 1")) viewFrom(kPi, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Right", "Numpad 3")) viewFrom(kPi * 0.5f, 0);
            if (ImGui::MenuItem("Left", "Ctrl+Numpad 3")) viewFrom(-kPi * 0.5f, 0);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Local View (Show Only Selected)", "Numpad /", isolating, isolating || selected >= 0 || wKind >= 0)) toggleIsolate();
        if (ImGui::MenuItem("Show Everything")) {
            showMeshes = showWorld = true;
            for (bool& b : showCat) b = true;
            isolating = false;
            refreshVisibility();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Toggle Maximize Area", "Ctrl+Space", viewMaximized)) viewMaximized = !viewMaximized;
    }

    // Blender's numpad views: from the front (-Z towards +Z), the right, the top.
    void viewFrom(float yaw, float pitch) {
        cam.yaw = yaw;
        cam.pitch = std::clamp(-pitch, -1.55f, 1.55f);
    }

    void selectMenu() {
        if (ImGui::MenuItem("None", "Alt+A", false, selected >= 0 || wKind >= 0)) { selected = -1; wKind = wIndex = -1; }
        if (ImGui::MenuItem("Hide Selected", "H", false, selected >= 0)) toggleHidden(selected);
        if (ImGui::MenuItem("Reveal Hidden", "Alt+H", false, scene != nullptr)) unhideAll();
    }

    void addMenuItems() {
        const bool on = scene != nullptr;
        if (ImGui::MenuItem("Spawn Point", nullptr, false, on)) addWorld(kSpawn);
        if (ImGui::MenuItem("Seat", nullptr, false, on)) addWorld(kSeat);
        if (ImGui::MenuItem("Teleport Hotspot", nullptr, false, on)) addWorld(kHotspot);
        ImGui::Separator();
        if (ImGui::MenuItem("Surface", nullptr, false, on)) addWorld(kCollider);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A 2 x 2 m box: scale it into a floor, a step or a wall");
        if (ImGui::MenuItem("Invisible Wall", nullptr, false, on)) addInvisibleWall();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops the player, never drawn on the headset. Scale and turn it to fit;\nwhere it stands is no longer walkable.");
        if (ImGui::MenuItem("Wall Spot", nullptr, false, on)) addWorld(kWallSpot);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where SteamVR's panels dock");
        ImGui::Separator();
        if (ImGui::BeginMenu("Gravity Zone", on)) {
            if (ImGui::MenuItem("Direction")) addGravity(WorldGravity::Direction);
            if (ImGui::MenuItem("Walk on Any Surface")) addGravity(WorldGravity::Surface);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Models (.glb .gltf .fbx .obj)\u2026", nullptr, false, on && !uploading)) importModelDialog();
        if (ImGui::MenuItem("Vista\u2026", nullptr, false, on && homePaths.size() == 1)) addVistaDialog();
    }

    void objectMenuItems() {
        const bool on = scene != nullptr;
        if (ImGui::BeginMenu("Transform", on)) {
            if (ImGui::MenuItem("Move", "G", tool == Tool::Move)) tool = Tool::Move;
            if (ImGui::MenuItem("Rotate", "R", tool == Tool::Rotate)) tool = Tool::Rotate;
            if (ImGui::MenuItem("Scale", "S", tool == Tool::Scale)) tool = Tool::Scale;
            ImGui::EndMenu();
        }
        const bool objSel = on && selected >= 0 && canBeSurface(objects[selected]);
        if (ImGui::BeginMenu("Surface", objSel)) {
            const int mode = surfaceMode(objects[selected]);
            for (int m = 0; m < kSurfModes; ++m) {
                if (ImGui::MenuItem(m == kSurfNone ? "Auto (from its shape)" : kSurfNames[m], nullptr, mode == m) && mode != m) setSurfaces({static_cast<size_t>(selected)}, m);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kSurfHelp[m]);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("All Visible Meshes", on)) {
            const std::vector<size_t> all = surfaceCandidates();
            ImGui::TextDisabled("%zu meshes (hidden ones are left out)", all.size());
            if (ImGui::MenuItem("Make Surfaces")) setSurfaces(all, kSurfSolid);
            ImGui::Separator();
            if (ImGui::MenuItem("Remove Mesh Surfaces")) {
                pushWorldUndo();
                auto& cs = scene->world.colliders;
                cs.erase(std::remove_if(cs.begin(), cs.end(), [](const WorldCollider& c) { return !c.mesh.empty(); }), cs.end());
                worldReplaced();
                wKind = wIndex = -1;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Auto-generate Walkable Surfaces", nullptr, false, on)) autoGenerate();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bakes where you can walk from everything solid in the home and saves it with it:\nfloors with headroom you can reach from the start; furniture, plants and walls block; seams are bridged.\nObject Surfaces (Surface, Walk through) override it. Settings: Properties \u203a World.");
        ImGui::Separator();
        if (ImGui::BeginMenu("Show/Hide", on)) {
            if (ImGui::MenuItem("Hide Selected", "H", false, selected >= 0)) toggleHidden(selected);
            if (ImGui::MenuItem("Reveal Hidden", "Alt+H")) unhideAll();
            if (ImGui::MenuItem("Show Only Selected", "Numpad /", isolating, isolating || selected >= 0 || wKind >= 0)) toggleIsolate();
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "X", false, selected >= 0 || wKind >= 0)) { if (wKind >= 0) deleteWorld(); else deleteSelected(); }
        if (ImGui::MenuItem("Clear World Layer", nullptr, false, on && !scene->world.empty())) {
            pushWorldUndo(); scene->world = HomeWorld{}; scene->world.origin = "studio"; worldReplaced(); wKind = wIndex = -1;
        }
    }

    // The viewport's shading and overlays, at the right of its header: the world layer (and its kinds), then the
    // view: Material (the home as the headset draws it), Solid (each mesh its colour), Wireframe; the rest in ▾.
    void viewportHeaderRight() {
        const float fh = ImGui::GetFrameHeight();
        const float w = fh * 1.2f;
        const float total = w * 5 + 14 * scale;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8 * scale, ImGui::GetWindowContentRegionMax().x - total));
        auto iconToggle = [&](const char* id, bool on, const char* tip, auto&& draw) {
            ImGui::PushStyleColor(ImGuiCol_Button, hex(on ? kBlue : kWidget));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(on ? kBlueHover : kWidgetHover));
            const bool hit = ImGui::Button(id, ImVec2(w, fh));
            ImGui::PopStyleColor(2);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            draw(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), fh * 0.3f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(0, 1);
            return hit;
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        (void)dl;
        if (iconToggle("##ovl", showWorld, "World layer (W): spawns, seats, surfaces, walls and the walkable area over the view\nRight-click: which kinds",
                       [&](ImDrawList* d, ImVec2 c, float r) {   // stacked layers
                           for (int k = 1; k >= -1; --k) {
                               const float y = c.y + k * r * 0.45f;
                               d->AddQuadFilled(ImVec2(c.x - r, y), ImVec2(c.x, y - r * 0.5f), ImVec2(c.x + r, y), ImVec2(c.x, y + r * 0.5f), col(0xffffff, k == -1 ? 1.0f : 0.55f));
                           }
                       }))
            showWorld = !showWorld;
        if (ImGui::BeginPopupContextItem("##ovlkinds")) {
            ImGui::TextDisabled("Overlays");
            ImGui::Separator();
            for (int k = 0; k < kShowCats; ++k) {
                ImGui::PushStyleColor(ImGuiCol_Text, hex(kShowColors[k]));
                ImGui::MenuItem(kShowNames[k], nullptr, &showCat[k]);
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("The Home's Meshes", nullptr, showMeshes)) { showMeshes = !showMeshes; refreshVisibility(); }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 8 * scale);
        // Blender's shading spheres: wire, solid, material
        auto sphere = [&](ImDrawList* d, ImVec2 c, float r, int kind) {
            if (kind == 0) {
                d->AddCircle(c, r, col(0xffffff), 20, 1.2f * scale);
                d->AddEllipse(c, ImVec2(r * 0.45f, r), col(0xffffff), 0, 16, 1.0f * scale);
                d->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col(0xffffff), 1.0f * scale);
            } else if (kind == 1) {
                d->AddCircleFilled(c, r, col(0xb8b8b8), 20);
                d->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.45f, col(0xe8e8e8), 16);
            } else {
                d->AddCircleFilled(c, r, col(0x6f8fbf), 20);
                d->AddCircleFilled(ImVec2(c.x + r * 0.15f, c.y + r * 0.15f), r * 0.75f, col(0x4d6a99), 16);
                d->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.25f, col(0xffffff), 12);
            }
        };
        auto shade = [&](const char* id, int v, const char* tip, int kind) {
            const bool off = v == HslRenderer::kViewWireframe && !renderer.wireframeAvailable();
            ImGui::BeginDisabled(off);
            if (iconToggle(id, debugView == v, off ? "This GPU can't draw lines." : tip,
                           [&](ImDrawList* d, ImVec2 c, float r) { sphere(d, c, r * 1.25f, kind); })) {
                debugView = v; renderer.setDebugView(v); debugView = renderer.debugView();
            }
            ImGui::EndDisabled();
        };
        shade("##shWire", HslRenderer::kViewWireframe, "Wireframe: every triangle edge over the home", 0);
        shade("##shSolid", HslRenderer::kViewMeshes, "Solid: each mesh in its own colour, shaded by its slope", 1);
        shade("##shMat", 0, "Material: the home as the headset draws it", 2);
        ImGui::PushStyleColor(ImGuiCol_Button, hex(kWidget));
        if (ImGui::Button("\u25be##shMore", ImVec2(fh * 0.8f, fh))) ImGui::OpenPopup("##shading");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shading: GPU cost, overdraw, what's left out");
        if (ImGui::BeginPopup("##shading")) {
            ImGui::TextDisabled("Viewport Shading");
            ImGui::Separator();
            static const char* views[] = {"Material", "Wireframe", "Solid", "GPU Cost", "Overdraw"};
            static const char* tips[] = {"The home as the headset draws it.", "Every triangle edge over the home: see-through meshes blue, cut-out green.",
                                         "Each mesh in its own colour, shaded by its slope: no textures or lighting.",
                                         "Tinted by what each mesh costs this PC's GPU: green cheap, yellow, red costly. The Home Report lists the costliest.",
                                         "Everything added up, nothing hidden: bright where many surfaces cover the same pixels."};
            for (int v = 0; v < HslRenderer::kDebugViews; ++v) {
                const bool off = v == HslRenderer::kViewWireframe && !renderer.wireframeAvailable();
                if (ImGui::MenuItem(views[v], nullptr, debugView == v, !off)) { debugView = v; renderer.setDebugView(v); debugView = renderer.debugView(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", off ? "This GPU can't draw lines." : tips[v]);
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Leave Out")) {
                static const char* parts[] = {"See-through Surfaces", "Cut-out Surfaces", "Sky", "Animated Meshes"};
                for (int b = 0; b < 4; ++b) {
                    const unsigned bit = 1u << b;
                    if (ImGui::MenuItem(parts[b], nullptr, (debugHide & bit) != 0)) { debugHide ^= bit; renderer.debugHide = debugHide; }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }

    // ── The areas ──
    // Blender's areas have rounded corners over the gutter: the gutter's colour filled into each corner of `a`..`b`
    // (bits: 1 top-left, 2 top-right, 4 bottom-left, 8 bottom-right), on top of whatever was drawn there.
    void roundCorners(ImDrawList* dl, ImVec2 a, ImVec2 b, int corners) {
        const float r = 6 * scale;
        const ImU32 c = col(kSplit);
        const ImVec2 k[4] = {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b};
        for (int i = 0; i < 4; ++i) {
            if (!(corners & (1 << i))) continue;
            const float sx = (i & 1) ? -1.0f : 1.0f, sy = (i & 2) ? -1.0f : 1.0f;   // into the area
            dl->PathLineTo(k[i]);
            const ImVec2 centre(k[i].x + sx * r, k[i].y + sy * r);
            const float a0 = std::atan2(-sy, 0.0f), a1 = std::atan2(0.0f, -sx);   // from the edge along y to the edge along x
            dl->PathArcTo(centre, r, a0, a0 + std::remainder(a1 - a0, 2 * kPi), 8);
            dl->PathFillConcave(c);
        }
    }
    void endArea() {
        ImGuiWindow* w = ImGui::GetCurrentWindow();
        roundCorners(ImGui::GetWindowDrawList(), w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), 15);
        ImGui::End();
    }
    // The bottom corners again from inside a child that fills the area's bottom (children draw over their window).
    void roundAreaBottom(int corners) {
        const ImGuiWindow* root = ImGui::GetCurrentWindow()->RootWindow;   // the area's own window
        roundCorners(ImGui::GetWindowDrawList(), root->Pos, ImVec2(root->Pos.x + root->Size.x, root->Pos.y + root->Size.y), corners);
    }

    // A dropdown in a header: as wide as its longest choice needs.
    bool headerDropdown(const char* id, const char* value, float width) {
        ImGui::SetNextItemWidth(width);
        return ImGui::BeginCombo(id, value);
    }

    // Begins an editor's area: its window docked in the dockspace, its header in the menu bar. False: not shown.
    bool beginArea(Editor e) {
        const int k = static_cast<int>(e);
        if (!editorShown[k]) return false;
        if (editorDockNext[k]) { ImGui::SetNextWindowDockID(editorDockNext[k], ImGuiCond_Always); editorDockNext[k] = 0; }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7 * scale, 4 * scale));   // the header's height: Blender's 26 px
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, hex(kHeader));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, hex(e == Editor::Outliner ? kList : kPanel));
        const bool visible = ImGui::Begin(kEditorWindow[k], nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                                   ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        editorDock[k] = ImGui::GetWindowDockID();
        areaCentre[k] = ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f);
        if (!visible) { ImGui::End(); return false; }
        if (ImGui::BeginMenuBar()) {
            editorButton(e);
            switch (e) {
                case Editor::Viewport: viewportHeader(); break;
                case Editor::Outliner: outlinerHeader(); break;
                case Editor::Properties: propertiesHeader(); break;
                case Editor::Timeline: timelineHeader(); break;
                default: break;
            }
            ImGui::EndMenuBar();
        }
        return true;
    }

    void viewportHeader() {
        {   // the mode: arranging things, or painting walkable floor onto one
            const char* mode = tool == Tool::DrawFloor ? "Draw Floor" : "Arrange";
            ImGui::PushStyleColor(ImGuiCol_FrameBg, hex(kWidget));
            if (headerDropdown("##mode", mode, ImGui::CalcTextSize("Draw Floor").x + 30 * scale)) {
                if (ImGui::Selectable("Arrange", tool != Tool::DrawFloor)) { if (tool == Tool::DrawFloor) tool = Tool::Move; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select, move, turn and scale objects and world items");
                if (ImGui::Selectable("Draw Floor", tool == Tool::DrawFloor)) tool = Tool::DrawFloor;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint walkable floor onto the selected object (B)");
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();
        }
        if (ImGui::BeginMenu("View")) { viewMenu(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Select")) { selectMenu(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Add")) { addMenuItems(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu("Object")) { objectMenuItems(); ImGui::EndMenu(); }
        {   // transform orientation, centred-ish as Blender's
            ImGui::SameLine(0, 18 * scale);
            if (headerDropdown("##orient", localSpace ? "Local" : "Global", ImGui::CalcTextSize("Global").x + 30 * scale)) {
                if (ImGui::Selectable("Global", !localSpace)) localSpace = false;
                if (ImGui::Selectable("Local", localSpace)) localSpace = true;
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transform orientation: the gizmo along the world's axes, or the object's own");
        }
        viewportHeaderRight();
    }

    void outlinerHeader() {
        if (headerDropdown("##display", byFileLayout ? "As in the File" : "By Kind", ImGui::CalcTextSize("As in the File").x + 30 * scale)) {
            if (ImGui::Selectable("As in the File", byFileLayout)) byFileLayout = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Folders as the home's file lays them out");
            if (ImGui::Selectable("By Kind", !byFileLayout)) byFileLayout = false;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Folders by kind: Architecture, Furniture & Props, Plants\u2026");
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 4 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, hex(kBar));
        ImGui::InputTextWithHint("##filter", "\u2315  Filter", filter, sizeof filter);
        ImGui::PopStyleColor();
    }

    void propertiesHeader() {
        static const char* names[] = {"Object", "World", "Environment", "Bundle", "Headset"};
        ImGui::TextDisabled("%s", names[static_cast<int>(inspectorTab)]);
    }

    void timelineHeader() {
        if (ImGui::BeginMenu("Playback")) {
            if (ImGui::MenuItem(animPlaying ? "Pause" : "Play", "Space")) animPlaying = !animPlaying;
            if (ImGui::MenuItem("Jump to Start", "Shift+Left")) { animTime = 0; animTick = std::chrono::steady_clock::now(); }
            if (ImGui::MenuItem("Jump to End", "Shift+Right")) { animTime = animLength; animTick = std::chrono::steady_clock::now(); }
            ImGui::Separator();
            const bool muted = g_audioMuted.load();
            if (ImGui::MenuItem("Sound", nullptr, !muted)) g_audioMuted.store(!muted);
            ImGui::EndMenu();
        }
        const float fh = ImGui::GetFrameHeight();
        const float mid = (ImGui::GetWindowContentRegionMax().x - fh * 3.3f) * 0.5f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8 * scale, mid));
        auto transport = [&](const char* id, ToolIcon ic, const char* tip) {
            const bool hit = ImGui::Button(id, ImVec2(fh * 1.1f, fh));
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            drawToolIcon(ImGui::GetWindowDrawList(), ic, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), fh * 0.5f, col(kText));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            ImGui::SameLine(0, 1);
            return hit;
        };
        ImGui::BeginDisabled(!scene);
        if (transport("##start", ToolIcon::SkipStart, "Jump to the start (Shift+Left)")) { animTime = 0; animTick = std::chrono::steady_clock::now(); }
        if (transport("##play", animPlaying ? ToolIcon::Pause : ToolIcon::Play, animPlaying ? "Pause the home's animations (Space)" : "Play the home's animations (Space)"))
            animPlaying = !animPlaying;
        if (transport("##end", ToolIcon::SkipEnd, "Jump to the end (Shift+Right)")) { animTime = animLength; animTick = std::chrono::steady_clock::now(); }
        ImGui::EndDisabled();
        // the right: the time now, and the sound
        const bool muted = g_audioMuted.load();
        const bool playing = ambience.ok || addedSound.ok;
        char now[48];
        std::snprintf(now, sizeof now, "%.1f / %.1f s", animTime, animLength);
        const float tw = ImGui::CalcTextSize(now).x + fh * 1.4f + 12 * scale;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8 * scale, ImGui::GetWindowContentRegionMax().x - tw));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", now);
        ImGui::SameLine(0, 6 * scale);
        ImGui::BeginDisabled(!playing);
        if (ImGui::Button("##sound", ImVec2(fh * 1.2f, fh))) g_audioMuted.store(!muted);
        {
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            drawToolIcon(ImGui::GetWindowDrawList(), playing && !muted ? ToolIcon::Sound : ToolIcon::Muted, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), fh * 0.55f, col(kText));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", addedSound.ok ? (muted ? "Sound off \u00b7 click to hear it" : "Your background sound (and the home's own) \u00b7 click to mute")
                                  : ambience.ok ? (muted ? "Sound off \u00b7 click to hear it" : "The home's ambience \u00b7 click to mute")
                                  : scene && !scene->ambientOgg.empty() ? "The ambience couldn't be played (no audio device?) \u2014 see studio.log"
                                                                       : "This home ships no ambience");
    }

    // The Timeline's body: a ruler in seconds, the playhead (blue, its time in a box at the top), click or drag to
    // scrub through the home's animation loop.
    void timelineBody() {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
        const ImVec2 b(a.x + avail.x, a.y + avail.y);
        if (avail.x < 20 || avail.y < 10) return;
        const float pad = 14 * scale, x0 = a.x + pad, x1 = b.x - pad, rulerH = ImGui::GetFrameHeight();
        dl->AddRectFilled(a, b, col(kList));
        dl->AddRectFilled(a, ImVec2(b.x, a.y + rulerH), col(kHeader));
        if (!scene) {   // no home open: an empty timeline with a quiet hint — no ruler, no stray "0"
            const char* hint = "Open a home to see its animation";
            const ImVec2 ts = ImGui::CalcTextSize(hint);
            dl->AddText(ImVec2((a.x + b.x) * 0.5f - ts.x * 0.5f, a.y + rulerH + (b.y - a.y - rulerH - ts.y) * 0.5f), col(kDim), hint);
            return;
        }
        const float len = std::max(0.1f, animLength);
        // the loop's range, lighter
        dl->AddRectFilled(ImVec2(x0, a.y + rulerH), ImVec2(x1, b.y), col(kListAlt));
        // ticks: every second, a label where they fit
        const float pxPerS = (x1 - x0) / len;
        float stepS = 1;
        for (float st : {1.0f, 2.0f, 5.0f, 10.0f, 15.0f, 30.0f, 60.0f, 120.0f}) { stepS = st; if (st * pxPerS >= 46 * scale) break; }
        for (float t = 0; t <= len + 1e-3f; t += stepS) {
            const float x = x0 + t * pxPerS;
            dl->AddLine(ImVec2(x, a.y + rulerH), ImVec2(x, b.y), col(kList), 1.0f);
            char lb[16];
            std::snprintf(lb, sizeof lb, "%g", t);
            const ImVec2 ts = ImGui::CalcTextSize(lb);
            dl->AddText(ImVec2(x - ts.x * 0.5f, a.y + (rulerH - ts.y) * 0.5f), col(kDim), lb);
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##scrub", avail);
        if (scene && ImGui::IsItemActive()) {
            animTime = std::clamp((ImGui::GetIO().MousePos.x - x0) / pxPerS, 0.0f, len);
            animTick = std::chrono::steady_clock::now();
        }
        if (!scene) return;
        const float px = x0 + std::clamp(animTime, 0.0f, len) * pxPerS;
        dl->AddLine(ImVec2(px, a.y + rulerH), ImVec2(px, b.y), col(kBlue), 2.0f * scale);
        char now[16];
        std::snprintf(now, sizeof now, "%.1f", animTime);
        const ImVec2 ts = ImGui::CalcTextSize(now);
        const ImVec2 ba(px - ts.x * 0.5f - 6 * scale, a.y + 2 * scale), bb(px + ts.x * 0.5f + 6 * scale, a.y + rulerH - 2 * scale);
        dl->AddRectFilled(ba, bb, col(kBlue), 4 * scale);
        dl->AddText(ImVec2(px - ts.x * 0.5f, a.y + (rulerH - ts.y) * 0.5f), col(0xffffff), now);
    }

    // ── The 3D Viewport's overlays: the toolbar (T), the navigation buttons, the view's name, the sidebar (N) ──
    void viewportToolbar() {
        if (!showToolbar || !scene) return;
        const float side = 32 * scale, gap = 2 * scale, pad = 3 * scale;
        const int n = 4;
        const ImVec2 a(viewPos.x + 6 * scale, viewPos.y + 6 * scale);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + side + pad * 2, a.y + n * side + (n - 1) * gap + pad * 2 + 6 * scale), col(kToolBg, 0.92f), 6 * scale);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad));
        struct T { const char* id; ToolIcon icon; Tool t; const char* tip; };
        static const T tools[] = {{"##tMove", ToolIcon::Move, Tool::Move, "Move (G) \u00b7 hold Ctrl to snap"},
                                  {"##tRotate", ToolIcon::Rotate, Tool::Rotate, "Rotate (R) \u00b7 hold Ctrl to snap 15\u00b0"},
                                  {"##tScale", ToolIcon::Scale, Tool::Scale, "Scale (S) \u00b7 hold Ctrl to snap"}};
        for (const T& t : tools) if (toolbarButton(t.id, t.icon, tool == t.t, t.tip, side)) tool = t.t;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, ImGui::GetCursorScreenPos().y + 6 * scale));
        if (toolbarButton("##tFloor", ToolIcon::DrawFloor, tool == Tool::DrawFloor,
                          "Draw Floor (B) \u00b7 select an object, then drag over it to draw walkable floor on it\nShift+drag erases \u00b7 [ and ] size the brush", side, kGo))
            tool = Tool::DrawFloor;
    }

    bool toolbarButton(const char* id, ToolIcon icon, bool on, const char* tip, float side, uint32_t onColor = kBlue) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(side, side));
        const bool hot = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (on || hot) dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side), col(on ? onColor : kToolHover), 4 * scale);
        drawToolIcon(dl, icon, ImVec2(p.x + side * 0.5f, p.y + side * 0.5f), side * 0.52f, col(on ? 0xffffff : hot ? kText : 0xc8c8c8));
        if (hot) ImGui::SetTooltip("%s", tip);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + side + 2 * scale));
        return clicked;
    }

    float sidebarWidth() const { return showSidebar && scene ? 250 * scale : 0.0f; }

    // Under the axes, Blender's navigation buttons: drag the magnifier to zoom, the hand to pan; the person: the
    // player's view; the corners: frame everything.
    void viewportNavigation(const float* pose, float ty) {
        if (!scene) return;
        const float r = 14 * scale, x = viewPos.x + viewSize.x - sidebarWidth() - 52 * scale;
        float y = viewPos.y + 100 * scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();
        auto button = [&](const char* id, ToolIcon ic, const char* tip) {
            ImGui::SetCursorScreenPos(ImVec2(x - r, y - r));
            ImGui::InvisibleButton(id, ImVec2(r * 2, r * 2));
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
            dl->AddCircleFilled(ImVec2(x, y), r, col(hot ? 0x5a5a5a : 0x000000, hot ? 0.8f : 0.22f), 24);
            drawToolIcon(dl, ic, ImVec2(x, y), r * 1.1f, col(0xe0e0e0));
            if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("%s", tip);
            y += r * 2 + 6 * scale;
        };
        button("##navZoom", ToolIcon::Zoom, "Zoom: drag up and down");
        if (ImGui::IsItemActive()) cam.dist = std::clamp(cam.dist * std::pow(1.01f, io.MouseDelta.y), 0.05f, 5000.0f);
        button("##navPan", ToolIcon::Pan, "Pan: drag");
        if (ImGui::IsItemActive()) {
            const float k = cam.dist * 2 * ty / std::max(1.0f, viewSize.y);
            for (int i = 0; i < 3; ++i) cam.pivot[i] += (-io.MouseDelta.x * pose[i] + io.MouseDelta.y * pose[4 + i]) * k;
        }
        button("##navPlayer", ToolIcon::PlayerView, "Player view (Numpad 0): from the middle of the home, at head height");
        if (ImGui::IsItemClicked()) cam.playerView();
        button("##navAll", ToolIcon::FrameAll, "Frame everything (Home)");
        if (ImGui::IsItemClicked()) frameAll();
    }

    // Top-left over the view: a small chip naming the home and what's selected (and the view when it's special).
    void viewportInfo() {
        if (!scene) return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::string what = shownName();
        if (selected >= 0) what += "  \u203a  " + objects[selected].label;
        else if (wKind >= 0 && wIndex >= 0 && wIndex < worldCount(wKind)) what += "  \u203a  " + worldName(wKind, wIndex);
        if (isolating) what += "   (only this)";
        if (flying) what += "   \u00b7 flying";
        const ImVec2 ts = ImGui::CalcTextSize(what.c_str());
        const ImVec2 a(viewPos.x + (showToolbar ? 52 * scale : 10 * scale), viewPos.y + 8 * scale);
        dl->AddRectFilled(a, ImVec2(a.x + ts.x + 20 * scale, a.y + ts.y + 8 * scale), col(kToolBg, 0.82f), (ts.y + 8 * scale) * 0.5f);
        dl->AddCircleFilled(ImVec2(a.x + 9 * scale, a.y + (ts.y + 8 * scale) * 0.5f), 2.5f * scale, col(kOrange));
        dl->AddText(ImVec2(a.x + 15 * scale, a.y + 4 * scale), col(kText), what.c_str());
    }

    // The sidebar (N): Item (the selection's settings), Tool (the active tool's), View (the camera's). Its tabs run
    // down its right edge, as Blender's.
    void viewportSidebar() {
        const float w = sidebarWidth();
        if (w <= 0) return;
        const float tabW = 22 * scale;
        const ImVec2 a(viewPos.x + viewSize.x - w, viewPos.y + 4 * scale), b(viewPos.x + viewSize.x - 4 * scale, viewPos.y + viewSize.y - 4 * scale);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(b.x - tabW, b.y), col(kPanel, 0.96f), 6 * scale);
        static const char* tabs[] = {"Item", "Tool", "View"};
        float ty = a.y + 6 * scale;
        for (int t = 0; t < 3; ++t) {   // the tab labels, written upwards
            const ImVec2 ts = ImGui::CalcTextSize(tabs[t]);
            const ImVec2 ta(b.x - tabW, ty), tb(b.x, ty + ts.x + 16 * scale);
            ImGui::SetCursorScreenPos(ta);
            ImGui::PushID(t);
            if (ImGui::InvisibleButton("##sbtab", ImVec2(tb.x - ta.x, tb.y - ta.y))) sidebarTab = t;
            ImGui::PopID();
            const bool on = sidebarTab == t, hot = ImGui::IsItemHovered();
            dl->AddRectFilled(ta, tb, col(on ? kPanel : hot ? kWidget : kHeader, 0.96f), 4 * scale, ImDrawFlags_RoundCornersRight);
            const int v0 = dl->VtxBuffer.Size;
            dl->AddText(ImVec2(0, 0), col(on ? 0xffffff : kDim), tabs[t]);
            for (int v = v0; v < dl->VtxBuffer.Size; ++v) {   // turn it 90 degrees anticlockwise into the tab
                ImDrawVert& vx = dl->VtxBuffer[v];
                const float px = vx.pos.x, py = vx.pos.y;
                vx.pos = ImVec2(ta.x + (tabW - ts.y) * 0.5f + py, tb.y - 8 * scale - px);
            }
            ty = tb.y + 2 * scale;
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x + 8 * scale, a.y + 8 * scale));
        ImGui::BeginChild("##sidebar", ImVec2(w - tabW - 16 * scale, b.y - a.y - 16 * scale), ImGuiChildFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * scale, 3 * scale));
        if (sidebarTab == 0) selectionTab();
        else if (sidebarTab == 1) {
            section("ACTIVE TOOL");
            static const char* names[] = {"Move", "Rotate", "Scale", "Draw Floor"};
            fieldLabel("Tool");
            ImGui::TextUnformatted(names[static_cast<int>(tool)]);
            fieldLabel("Orientation");
            ImGui::TextUnformatted(localSpace ? "Local" : "Global");
            fieldLabel("Snap");
            ImGui::TextDisabled("hold Ctrl");
            if (tool == Tool::DrawFloor) {
                fieldLabel("Brush");
                ImGui::SliderFloat("##sbBrush", &floorBrush, 0.05f, 3.0f, "%.2f m", ImGuiSliderFlags_Logarithmic);
            }
        } else {
            section("VIEW");
            fieldLabel("Fly speed");
            ImGui::DragFloat("##sbFly", &flySpeed, 0.05f, 0.1f, 100.0f, "%.1f m/s");
            fieldLabel("Distance");
            ImGui::DragFloat("##sbDist", &cam.dist, 0.05f, 0.05f, 5000.0f, "%.2f m");
            section("OVERLAYS");
            ImGui::Checkbox("World layer (W)", &showWorld);
            if (ImGui::Checkbox("The home's meshes", &showMeshes)) refreshVisibility();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    // ── Blender's keys beyond the editing ones: Shift+A adds, X asks before deleting, F3 searches every command ──
    void addPopup() {
        if (ImGui::BeginPopup("##addAtMouse")) {
            ImGui::TextDisabled("Add");
            ImGui::Separator();
            addMenuItems();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##deleteAsk")) {
            ImGui::TextDisabled("OK?");
            if (ImGui::MenuItem("Delete")) { if (wKind >= 0) deleteWorld(); else deleteSelected(); }
            ImGui::EndPopup();
        }
    }

    struct Command { std::string name, keys; bool enabled; std::function<void()> run; };
    std::vector<Command> commands() {
        const bool on = scene != nullptr, sel = selected >= 0 || wKind >= 0;
        std::vector<Command> c = {
            {"Open", "Ctrl+O", true, [this] { openDialog(); }},
            {"Save", "Ctrl+S", on && !saving, [this] { saveToLibrary(); }},
            {"Save As", "Ctrl+Shift+S", on && !saving, [this] { saveEnvironmentDialog(); }},
            {"Undo", "Ctrl+Z", !undo.empty(), [this] { step(undo, redo); }},
            {"Redo", "Ctrl+Shift+Z", !redo.empty(), [this] { step(redo, undo); }},
            {"Add Spawn Point", "", on, [this] { addWorld(kSpawn); }},
            {"Add Seat", "", on, [this] { addWorld(kSeat); }},
            {"Add Teleport Hotspot", "", on, [this] { addWorld(kHotspot); }},
            {"Add Surface", "", on, [this] { addWorld(kCollider); }},
            {"Add Invisible Wall", "", on, [this] { addInvisibleWall(); }},
            {"Add Wall Spot", "", on, [this] { addWorld(kWallSpot); }},
            {"Add Gravity Zone", "", on, [this] { addGravity(WorldGravity::Direction); }},
            {"Import Models", "", on && !uploading, [this] { importModelDialog(); }},
            {"Add Vista", "", on && homePaths.size() == 1, [this] { addVistaDialog(); }},
            {"Auto-generate Walkable Surfaces", "", on, [this] { autoGenerate(); }},
            {"Delete", "X", sel, [this] { if (wKind >= 0) deleteWorld(); else deleteSelected(); }},
            {"Hide Selected", "H", selected >= 0, [this] { toggleHidden(selected); }},
            {"Reveal Hidden", "Alt+H", on, [this] { unhideAll(); }},
            {"Local View (Show Only Selected)", "Numpad /", on && (isolating || sel), [this] { toggleIsolate(); }},
            {"Frame Selected", "Numpad .", sel, [this] { if (wKind >= 0) frameWorld(wKind, wIndex); else frameSelected(); }},
            {"Frame All", "Home", on, [this] { frameAll(); }},
            {"Player View", "Numpad 0", on, [this] { cam.playerView(); }},
            {"Toggle World Layer", "W", on, [this] { showWorld = !showWorld; }},
            {"Toggle Toolbar", "T", true, [this] { showToolbar = !showToolbar; }},
            {"Toggle Sidebar", "N", true, [this] { showSidebar = !showSidebar; }},
            {"Toggle Maximize Area", "Ctrl+Space", true, [this] { viewMaximized = !viewMaximized; }},
            {"Move Tool", "G", on, [this] { tool = Tool::Move; }},
            {"Rotate Tool", "R", on, [this] { tool = Tool::Rotate; }},
            {"Scale Tool", "S", on, [this] { tool = Tool::Scale; }},
            {"Draw Floor", "B", on, [this] { tool = Tool::DrawFloor; }},
            {"Play / Pause", "Space", on, [this] { animPlaying = !animPlaying; }},
            {"Send to Frame", "", on && !sending, [this] { sendToFrame(); }},
            {"Install Frame Home on the Steam Frame", "", true, [this] { openInstall(); }},
            {"New Bundle", "", !bundleBusy, [this] { newBundle(); }},
            {"Open Bundle", "", !bundleBusy, [this] { openBundleDialog(); }},
            {"Home Report", "", on, [this] { showReport = true; report.reset(); }},
            {"Preferences", "", true, [this] { showSettings = true; if (!headsetLoaded && !headsetBusy) headsetSettings(false); }},
            {"Reset Layout", "", true, [this] { layoutWanted = true; }},
            {"Controls", "F1", true, [this] { showControls = true; }},
        };
        for (int w = 0; w < kWorkspaces; ++w) c.push_back({std::string("Workspace: ") + kWorkspaceName[w], "", true, [this, w] { setWorkspace(w); }});
        return c;
    }

    void openSearch() { searchOpen = true; searchText[0] = 0; searchPick = 0; }

    void searchPopup() {
        if (searchOpen) { ImGui::OpenPopup("##search"); searchOpen = false; }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.18f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(420 * scale, 0));
        searchShowing = ImGui::BeginPopup("##search");
        if (!searchShowing) return;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, hex(kBar));
        if (ImGui::InputTextWithHint("##q", "\u2315  Search menus", searchText, sizeof searchText)) searchPick = 0;
        ImGui::PopStyleColor();
        std::string q = searchText;
        for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::vector<Command> all = commands(), hits;
        for (Command& c : all) {   // every word typed is in the name
            std::string n = c.name;
            for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            bool ok = true;
            for (size_t i = 0; i < q.size() && ok;) {
                const size_t sp = q.find(' ', i);
                const std::string word = q.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                if (!word.empty() && n.find(word) == std::string::npos) ok = false;
                i = sp == std::string::npos ? q.size() : sp + 1;
            }
            if (ok && c.enabled) hits.push_back(std::move(c));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchPick = std::min(searchPick + 1, static_cast<int>(hits.size()) - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchPick = std::max(searchPick - 1, 0);
        int run = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) run = searchPick;
        for (size_t i = 0; i < hits.size() && i < 14; ++i) {
            if (ImGui::Selectable(hits[i].name.c_str(), static_cast<int>(i) == searchPick)) run = static_cast<int>(i);
            if (!hits[i].keys.empty()) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(hits[i].keys.c_str()).x);
                ImGui::TextDisabled("%s", hits[i].keys.c_str());
            }
        }
        if (hits.empty()) ImGui::TextDisabled("Nothing matches");
        if (run >= 0 && run < static_cast<int>(hits.size())) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            hits[static_cast<size_t>(run)].run();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ── The status bar: what the mouse buttons do (left), the scene's numbers (right) ──
    void statusBar(float y, float h) {
        ImGui::SetNextWindowPos(ImVec2(0, y));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale, (h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, hex(kBar));
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // a little mouse with the button that does it lit
        auto mouseHint = [&](int button, const char* what) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = 9 * scale, hh = 13 * scale, cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
            const ImVec2 a(p.x, cy - hh * 0.5f), b(p.x + w, cy + hh * 0.5f);
            dl->AddRect(a, b, col(kDim), 4 * scale, 0, 1.0f);
            const float mx = (a.x + b.x) * 0.5f, my = a.y + hh * 0.42f;
            if (button == 0) dl->AddRectFilled(ImVec2(a.x + 1, a.y + 1), ImVec2(mx, my), col(kText), 3 * scale, ImDrawFlags_RoundCornersTopLeft);
            else if (button == 1) dl->AddRectFilled(ImVec2(mx, a.y + 1), ImVec2(b.x - 1, my), col(kText), 3 * scale, ImDrawFlags_RoundCornersTopRight);
            else dl->AddRectFilled(ImVec2(mx - 1.2f * scale, a.y + 2.5f * scale), ImVec2(mx + 1.2f * scale, my - 0.5f * scale), col(kText));
            ImGui::Dummy(ImVec2(w + 4 * scale, 0));
            ImGui::SameLine(0, 0);
            ImGui::TextDisabled("%s", what);
            ImGui::SameLine(0, 16 * scale);
        };
        if (scene && tool == Tool::DrawFloor) { mouseHint(0, "Draw floor (Shift: erase)"); mouseHint(2, "Rotate View"); mouseHint(1, "Fly"); }
        else if (scene) { mouseHint(0, "Select"); mouseHint(2, "Rotate View"); mouseHint(1, "Context Menu / Fly"); }
        else ImGui::TextDisabled("Frame Studio");
        std::string right;
        {
            std::string saveMsg;
            { std::lock_guard<std::mutex> l(statusMutex); saveMsg = saveStatus; }
            if (!saveMsg.empty()) right += saveMsg + "  |  ";
        }
        if (scene) {
            size_t count = 0;
            for (const Object& o : objects) count += !o.deleted;
            char b[160];
            std::snprintf(b, sizeof b, "%zu objects  |  %.0f fps  |  ", count, ImGui::GetIO().Framerate);
            right += b;
        }
        right += "Frame: " + status();
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 12 * scale));
        ImGui::TextDisabled("%s", right.c_str());
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }

    // The window, each frame: the top bar, the areas in their dockspace (or the 3D Viewport alone, maximized), the
    // status bar, then the floating windows and popups.
    void ui() {
        shortcuts();
        topBar();
        ImGuiIO& io = ImGui::GetIO();
        const float top = ImGui::GetFrameHeight() + 4 * scale, barH = ImGui::GetFrameHeight() + 2 * scale;
        const ImVec2 at(0, top), size(io.DisplaySize.x, std::max(1.0f, io.DisplaySize.y - top - barH));
        ImGui::SetNextWindowPos(at);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3 * scale, 3 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, hex(kSplit));
        ImGui::Begin("##areas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        dockspaceId = ImGui::GetID("StudioAreas");
        if (layoutWanted || !ImGui::DockBuilderGetNode(dockspaceId)) {
            buildLayout(dockspaceId, ImGui::GetContentRegionAvail());
            layoutWanted = false;
        }
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_AutoHideTabBar | ImGuiDockNodeFlags_NoWindowMenuButton);
        ImGui::End();
        if (viewMaximized) {   // Ctrl+Space: the 3D Viewport over every other area until pressed again
            ImGui::SetNextWindowPos(ImVec2(at.x + 3 * scale, at.y + 3 * scale));
            ImGui::SetNextWindowSize(ImVec2(size.x - 6 * scale, size.y - 6 * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7 * scale, 4 * scale));
            ImGui::PushStyleColor(ImGuiCol_MenuBarBg, hex(kHeader));
            ImGui::Begin("##maximized", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            if (ImGui::BeginMenuBar()) {
                ImGui::PushID("max");
                const float fh = ImGui::GetFrameHeight();
                if (ImGui::Button("Back to Previous", ImVec2(0, fh))) viewMaximized = false;
                ImGui::PopID();
                viewportHeader();
                ImGui::EndMenuBar();
            }
            viewport(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
            ImGui::End();
        } else {
            if (beginArea(Editor::Viewport)) { viewport(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y); endArea(); }
            if (beginArea(Editor::Outliner)) { outliner(ImGui::GetContentRegionAvail().y); endArea(); }
            if (beginArea(Editor::Properties)) { inspector(ImGui::GetContentRegionAvail().y); endArea(); }
            if (beginArea(Editor::Timeline)) { timelineBody(); endArea(); }
        }
        statusBar(io.DisplaySize.y - barH, barH);
        addPopup();
        searchPopup();
        controlsWindow();
        settingsWindow();
        reportWindow();
        headsetDebugWindow();
        loadPrompt();
        vistaPicker();
        autoSummaryWindow();
    }

    // ── Headset > Install on Steam Frame ──
    // Puts Frame Home on the headset as its environment: SteamVR's home-app slot, which SteamVR itself runs whenever
    // no game is running (in place of the default environment, not as a game or an extra app), plus the chosen
    // environments. The headset side is frame\install.sh beside this exe, with Frame Home's arm64 binaries.

    void openInstall() { showInspectorTab(InspectorTab::Headset); }

    // The install list starts from the environments saved recently (once: after that it's yours).
    bool installEnvsSeeded = false;
    void seedInstallEnvs() {
        if (installEnvsSeeded) return;
        installEnvsSeeded = true;
        if (installEnvs.empty())
            for (const auto& r : recent)
                if (r.size() == 1 && isFenvPath(r[0]) && fs::exists(fs::path(widen(r[0])))) installEnvs.push_back(r[0]);
    }

    bool openHomeIsFenv() const { return homePaths.size() == 1 && isFenvPath(homePaths[0]); }

    void addInstallEnvDialog() {
        const std::vector<std::string> picked = platform::openFiles("Environments to install on the Steam Frame",
            {{"Environments and bundles", {"*.fenv", "*.fbundle"}}, {"Frame environment", {"*.fenv"}}, {"Environment bundle", {"*.fbundle"}}}, true);
        for (const auto& f : picked)
            if (std::find(installEnvs.begin(), installEnvs.end(), f) == installEnvs.end()) installEnvs.push_back(f);
    }

    void startInstall(bool uninstall) {
        if (installing) return;
        if (saving) { setInstallStatus("Wait for the environment being saved to finish, then install."); return; }
        saveHost();
        std::vector<std::string> envs;
        std::string openFenv;
        if (!uninstall) {
            for (const auto& e : installEnvs) if (fs::exists(fs::path(widen(e)))) envs.push_back(e);
            if (installOpenHome && scene) {
                if (openHomeIsFenv()) {
                    if (std::find(envs.begin(), envs.end(), homePaths[0]) == envs.end()) envs.insert(envs.begin(), homePaths[0]);
                } else if (!uploading) {         // the open home, saved as a .fenv for the headset
                    std::error_code ec;
                    fs::create_directories(dataDir() / L"install", ec);
                    openFenv = narrow((dataDir() / L"install" / widen(frameName(homeName + ".fenv"))).wstring());
                    fs::remove(fs::path(widen(openFenv)), ec);   // a failed save must not ship the last install's copy
                    saveEnvironment(openFenv, false);
                }
            }
        }
        if (installer.joinable()) installer.join();
        installing = true;
        const std::string host8 = hostArg();
        installer = std::thread([this, uninstall, envs, openFenv, host8]() mutable {
            auto done = [&](const std::string& m) { setInstallStatus(m); installing = false; };
            const std::wstring host = widen(host8);
            const std::wstring ssh = sshTool(L"ssh") + L"-o BatchMode=yes -o ConnectTimeout=8 " + host + L" ";
            const std::wstring scp = sshTool(L"scp") + L"-q -o BatchMode=yes -o ConnectTimeout=8 ";
            const fs::path payload = exeDir() / L"frame";
            if (!uninstall && !fs::exists(payload / L"install.sh"))
                return done("Frame Home's headset files are missing: expected " + narrow((payload / L"install.sh").wstring()));
            if (!openFenv.empty()) {
                setInstallStatus("Saving the open home as an environment\u2026");
                while (saving) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!fs::exists(fs::path(widen(openFenv)))) return done("Couldn't save the open home as an environment");
                envs.insert(envs.begin(), openFenv);
            }
            std::string out;
            setInstallStatus("Connecting to " + host8 + "\u2026");
            int rc = run(ssh + L"\"mkdir -p frame-home/envs frame-home/.install && uname -m\"", L"", out);
            if (rc != 0) return done("Can't reach the headset (" + host8 + "): " + lastLine(out) +
                                     (out.find("could not start") != std::string::npos ? " (install OpenSSH or Git for Windows)" : ""));
            if (lastLine(out) != "aarch64") return done(host8 + " isn't a Steam Frame (it reports " + lastLine(out) + ")");
            if (!uninstall) {
                setInstallStatus("Copying Frame Home\u2026");
                out.clear();
                rc = run(ssh + L"\"rm -rf frame-home/.install/frame\"", L"", out);
                // Relative source from its own folder: scp would read "C:\\..." as a host named C.
                if (rc == 0) rc = run(scp + L"-r frame " + host + L":frame-home/.install/", exeDir().wstring(), out);
                if (rc != 0) return done("Copying Frame Home failed: " + lastLine(out));
                for (size_t i = 0; i < envs.size(); ++i) {
                    const fs::path f = fs::path(widen(envs[i]));
                    const std::wstring name = widen(frameName(envs[i]));
                    out.clear();
                    rc = run(ssh + L"\"stat -c %s frame-home/envs/" + name + L" 2>/dev/null || echo 0\"", L"", out);
                    std::error_code ec;
                    const unsigned long long local = fs::file_size(f, ec);
                    if (rc == 0 && std::strtoull(lastLine(out).c_str(), nullptr, 10) == local) continue;   // already there
                    char msg[256];
                    std::snprintf(msg, sizeof msg, "Copying %s (%zu of %zu, %.0f MB)\u2026", narrow(f.filename().wstring()).c_str(), i + 1, envs.size(), local / 1048576.0);
                    setInstallStatus(msg);
                    out.clear();
                    rc = run(scp + L"\"" + f.filename().wstring() + L"\" " + host + L":frame-home/envs/" + name + L".part", f.parent_path().wstring(), out);
                    if (rc == 0) rc = run(ssh + L"\"mv -f frame-home/envs/" + name + L".part frame-home/envs/" + name + L"\"", L"", out);
                    if (rc != 0) return done("Copying " + narrow(f.filename().wstring()) + " failed: " + lastLine(out));
                }
            }
            setInstallStatus(uninstall ? "Restoring SteamVR's environment\u2026" : "Installing on the headset\u2026");
            out.clear();
            rc = run(ssh + L"\"sed -i 's/\\r$//' frame-home/.install/frame/install.sh 2>/dev/null; sh frame-home/.install/frame/install.sh" +
                     (uninstall ? L" uninstall" : L"") + L"\"", L"", out);
            if (uninstall && rc != 0 && out.find("No such file") != std::string::npos)   // never installed from here
                rc = run(ssh + L"\"frame-home/bin/frame-home --uninstall\"", L"", out);
            // The installer's own verdict (its tools' log lines can arrive after it).
            std::string msg = lastLine(out);
            if (const size_t at = out.rfind("frame-install: "); at != std::string::npos)
                msg = lastLine(out.substr(at + 15, out.find('\n', at) == std::string::npos ? std::string::npos : out.find('\n', at) - at - 15));
            if (rc != 0) return done("Failed: " + msg);
            // The first environment is the one shown (a bundle's are named once the headset unpacks it).
            const auto shown = std::find_if(envs.begin(), envs.end(), [](const std::string& e) { return !isFenvBundlePath(e); });
            if (!uninstall && shown != envs.end()) {
                out.clear();
                if (run(ssh + L"\"printf '" + widen(frameName(*shown)) + L"\\n' > frame-home/current\"", L"", out) != 0)
                    return done("Installed, but couldn't choose the first environment: " + lastLine(out));
            }
            if (msg.empty()) msg = uninstall ? "SteamVR's environment restored" : "Installed";
            msg[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(msg[0])));
            done(msg + (uninstall || envs.empty() ? "" : " (" + std::to_string(envs.size()) + (envs.size() == 1 ? " environment)" : " environments)")));
        });
    }

    // The Headset tab's install part: Frame Home onto the headset, with environments.
    void installPanel() {
        seedInstallEnvs();
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("Installs Frame Home on the headset as its environment. SteamVR runs it natively in place of the default "
                           "environment whenever no game is running, and brings it back when a game quits. It is not a game or an extra app.");
        ImGui::Spacing();
        ImGui::BeginDisabled(installing);
        fieldLabel("Headset");
        ImGui::InputText("##installHost", frameHost, sizeof frameHost);
        ImGui::TextDisabled("an ssh alias, or steamos@<address>");
        ImGui::Spacing();
        ImGui::SeparatorText("Environments to install");
        if (scene) {
            const std::string label = "The open home (" + homeName + ")" + (openHomeIsFenv() ? "" : ", saved as .fenv");
            ImGui::Checkbox(label.c_str(), &installOpenHome);
        }
        int remove = -1;
        for (size_t i = 0; i < installEnvs.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("\u2715")) remove = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::TextUnformatted(narrow(fs::path(widen(installEnvs[i])).filename().wstring()).c_str());
            ImGui::PopID();
        }
        if (remove >= 0) installEnvs.erase(installEnvs.begin() + remove);
        if (installEnvs.empty() && !scene) ImGui::TextDisabled("None yet: Frame Home alone (it keeps the environments already on the headset).");
        if (ImGui::Button("Add .fenv or .fbundle\u2026")) addInstallEnvDialog();
        ImGui::Spacing();
        ImGui::Separator();
        pushPrimary();
        if (ImGui::Button(installing ? "Installing\u2026" : "Install Frame Home", ImVec2(-1, ImGui::GetFrameHeight() * 1.4f))) startInstall(false);
        ImGui::PopStyleColor(3);
        if (ImGui::Button("Restore SteamVR's Environment", ImVec2(-1, 0))) startInstall(true);
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextWrapped("%s", getInstallStatus().c_str());
        ImGui::PopTextWrapPos();
    }

    // ── Bundles (Inspector › Bundle, the Bundle menu): several environments packed into one .fbundle ──
    // Each member stays its own environment; on the headset Frame Home unpacks a bundle into them. A member is a .fenv
    // on disk, or one inside the bundle that was opened (unpacked when it's needed: opened, saved into another bundle).
    struct BundleMember {
        std::string path;                    // the .fenv, or the opened bundle
        std::string member;                  // inside that bundle: its file there; empty for a .fenv on disk
        std::string name;                    // what the headset calls it
        uint64_t size = 0;
        VkImageView thumbView = VK_NULL_HANDLE;
        VkDescriptorSet thumb = VK_NULL_HANDLE;
        float thumbAspect = 0;               // its picture's height / width; 0: not read yet, < 0: it has none
    };
    std::vector<BundleMember> bundleMembers;
    std::string bundleFile;                  // the .fbundle opened or last saved; empty for a new one
    std::string bundleName;
    bool bundleDirty = false;
    std::thread bundler;                     // saving and unpacking, off the UI thread
    std::atomic<bool> bundleBusy{false};
    std::string bundleStatus;                // (statusMutex)
    std::vector<std::string> bundleOpenLater;   // a member unpacked to open: opened by the UI thread (statusMutex)

    void setBundleStatus(const std::string& s) { std::lock_guard<std::mutex> l(statusMutex); bundleStatus = s; }

    // Thumbnails are GPU pictures: freed together, once the frames that drew them are done.
    void releaseBundleThumbs(std::vector<BundleMember>& members) {
        bool any = false;
        for (const auto& m : members) any |= m.thumbView != VK_NULL_HANDLE;
        if (!any) return;
        vkDeviceWaitIdle(renderer.device());
        for (auto& m : members) {
            if (m.thumb) ImGui_ImplVulkan_RemoveTexture(m.thumb);
            if (m.thumbView) renderer.releasePicture(m.thumbView);
            m.thumb = VK_NULL_HANDLE;
            m.thumbView = VK_NULL_HANDLE;
            m.thumbAspect = 0;
        }
    }

    // Its picture, read from the .fenv (or from inside the bundle) the first time the row is drawn, shrunk to 160 px.
    void loadBundleThumb(BundleMember& m) {
        m.thumbAspect = -1;
        FenvManifest fm;
        std::vector<uint8_t> png;
        const bool ok = m.member.empty() ? fenvReadManifest(m.path, fm, &png) : fenvBundleReadEnvironment(m.path, m.member, fm, &png);
        if (!ok || png.empty()) return;
        int w = 0, h = 0, c = 0;
        unsigned char* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &c, 4);
        if (!px) return;
        const int tw = std::min(160, w), th = std::max(1, h * tw / std::max(1, w));
        std::vector<u8> small(static_cast<size_t>(tw) * th * 4);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {   // box filter over the source pixels each covers
                const int x0 = x * w / tw, x1 = std::max(x0 + 1, (x + 1) * w / tw), y0 = y * h / th, y1 = std::max(y0 + 1, (y + 1) * h / th);
                uint32_t acc[4] = {}, n = 0;
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx, ++n)
                        for (int k = 0; k < 4; ++k) acc[k] += px[(static_cast<size_t>(yy) * w + xx) * 4 + k];
                for (int k = 0; k < 4; ++k) small[(static_cast<size_t>(y) * tw + x) * 4 + k] = static_cast<u8>(k == 3 ? 255 : acc[k] / n);
            }
        stbi_image_free(px);
        m.thumbView = renderer.createPicture(small.data(), static_cast<uint32_t>(tw), static_cast<uint32_t>(th));
        if (!m.thumbView) return;
        m.thumb = ImGui_ImplVulkan_AddTexture(viewSampler, m.thumbView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m.thumbAspect = static_cast<float>(th) / static_cast<float>(tw);
    }

    // Unsaved changes to the bundle: true when it's fine to drop them.
    bool confirmBundleDiscard() {
        if (!bundleDirty || bundleMembers.empty()) return true;
        const platform::Answer a = platform::ask("Frame Studio", "Save the bundle \"" + shownBundleName() + "\" first?", true);
        if (a == platform::Answer::Cancel) return false;
        if (a == platform::Answer::Yes) { saveBundleDialog(); return false; }   // saving runs in the background: try again after
        return true;
    }

    std::string shownBundleName() const { return bundleName.empty() ? std::string("New bundle") : bundleName; }

    void newBundle() {
        if (bundleBusy || !confirmBundleDiscard()) return;
        releaseBundleThumbs(bundleMembers);
        bundleMembers.clear();
        bundleFile.clear();
        bundleName.clear();
        bundleDirty = false;
        setBundleStatus("");
        showInspectorTab(InspectorTab::Bundle);
    }

    void openBundle(const std::string& path) {
        if (bundleBusy || !confirmBundleDiscard()) return;
        FenvBundleManifest bm;
        std::string err;
        if (!fenvBundleReadManifest(path, bm, &err)) {
            platform::message("Frame Studio", "Couldn't open the bundle " + narrow(fs::path(widen(path)).filename().wstring()) + (err.empty() ? "" : ": " + err),
                              platform::Icon::Warning);
            return;
        }
        releaseBundleThumbs(bundleMembers);
        bundleMembers.clear();
        for (const auto& e : bm.environments) {
            BundleMember m;
            m.path = path;
            m.member = e.file;
            m.name = e.name;
            m.size = e.size;
            bundleMembers.push_back(std::move(m));
        }
        bundleFile = path;
        bundleName = bm.name.empty() ? narrow(fs::path(widen(path)).stem().wstring()) : bm.name;
        bundleDirty = false;
        setBundleStatus("");
        showInspectorTab(InspectorTab::Bundle);
    }

    void openBundleDialog() {
        const std::vector<std::string> picked = platform::openFiles("Open an environment bundle", {{"Environment bundle", {"*.fbundle"}}}, false);
        if (!picked.empty()) openBundle(picked.front());
    }

    // Adds .fenv files (one already in the bundle isn't added twice).
    void addToBundle(const std::vector<std::string>& paths) {
        for (const auto& p : paths) {
            if (!isFenvPath(p)) continue;
            bool there = false;
            for (const auto& m : bundleMembers) there |= m.member.empty() && fs::path(widen(m.path)) == fs::path(widen(p));
            if (there) continue;
            FenvManifest fm;
            if (!fenvReadManifest(p, fm)) {
                platform::message("Frame Studio", "This isn't an environment Frame Studio can read: " + narrow(fs::path(widen(p)).filename().wstring()), platform::Icon::Warning);
                continue;
            }
            BundleMember m;
            m.path = p;
            m.name = fm.name.empty() ? narrow(fs::path(widen(p)).stem().wstring()) : fm.name;
            std::error_code ec;
            m.size = fs::file_size(fs::path(widen(p)), ec);
            bundleMembers.push_back(std::move(m));
            bundleDirty = true;
        }
        showInspectorTab(InspectorTab::Bundle);
    }

    void addToBundleDialog() {
        addToBundle(platform::openFiles("Environments to add to the bundle", {{"Frame environments", {"*.fenv"}}}, true));
    }

    // The open environment as a .fenv: the file it was opened from or last saved to.
    std::string openEnvFile() const { return openHomeIsFenv() ? homePaths[0] : envFile; }

    void removeFromBundle(size_t i) {
        std::vector<BundleMember> gone(bundleMembers.begin() + static_cast<std::ptrdiff_t>(i), bundleMembers.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        releaseBundleThumbs(gone);
        bundleMembers.erase(bundleMembers.begin() + static_cast<std::ptrdiff_t>(i));
        bundleDirty = true;
    }

    // Members inside a bundle are unpacked here when they're needed (a scratch folder, emptied each time).
    fs::path bundleScratch() const { return dataDir() / L"bundle-unpacked"; }

    // Opens one member: a .fenv straight away, one inside the bundle once it's unpacked.
    void openBundleMember(size_t i) {
        const BundleMember m = bundleMembers[i];
        if (m.member.empty()) { open({m.path}); return; }
        if (bundleBusy) return;
        if (bundler.joinable()) bundler.join();
        bundleBusy = true;
        setBundleStatus("Unpacking " + m.name + "\u2026");
        const fs::path dir = bundleScratch() / L"open";
        bundler = std::thread([this, m, dir] {
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir, ec);
            std::vector<std::string> out;
            std::string err;
            if (!fenvBundleExtract(m.path, narrow(dir.wstring()), out, err, m.member) || out.empty()) setBundleStatus("Couldn't unpack " + m.name + ": " + err);
            else {
                std::lock_guard<std::mutex> l(statusMutex);
                bundleStatus.clear();
                bundleOpenLater = out;
            }
            bundleBusy = false;
        });
    }

    void saveBundleDialog() {
        if (bundleBusy || bundleMembers.empty()) return;
        std::string name = shownBundleName();
        for (char& c : name) if (c == '+' || c == '/' || c == '\\' || c == ':') c = '-';
        const std::string path = platform::saveFile("Save the bundle", name + ".fbundle", {{"Environment bundle", {"*.fbundle"}}}, "fbundle");
        if (!path.empty()) saveBundle(path);
    }

    // Packs the members into `out` in the background. Members from inside a bundle are unpacked first (the bundle
    // being replaced may be the one they're in).
    void saveBundle(const std::string& out) {
        if (bundleBusy || bundleMembers.empty()) return;
        if (bundler.joinable()) bundler.join();
        bundleBusy = true;
        const std::vector<BundleMember> members = bundleMembers;
        const std::string name = shownBundleName();
        const fs::path scratch = bundleScratch() / L"save";
        bundler = std::thread([this, members, name, out, scratch] {
            std::error_code ec;
            fs::remove_all(scratch, ec);
            std::vector<std::string> files;
            std::string err;
            for (const auto& m : members) {
                if (m.member.empty()) { files.push_back(m.path); continue; }
                setBundleStatus("Unpacking " + m.name + "\u2026");
                const fs::path dir = scratch / std::to_wstring(files.size());   // one folder each: same-named members stay apart
                fs::create_directories(dir, ec);
                std::vector<std::string> got;
                if (!fenvBundleExtract(m.path, narrow(dir.wstring()), got, err, m.member) || got.empty()) {
                    setBundleStatus("Couldn't save the bundle: unpacking " + m.name + " failed: " + err);
                    bundleBusy = false;
                    return;
                }
                files.push_back(got.front());
            }
            const bool ok = fenvBundleWrite(out, files, name, err, [this](uint64_t done, uint64_t total) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "Saving the bundle\u2026 %.0f%%", total ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 100.0);
                setBundleStatus(msg);
            });
            fs::remove_all(scratch, ec);
            std::error_code sec;
            const double mb = static_cast<double>(fs::file_size(fs::path(widen(out)), sec)) / 1048576.0;
            char msg[512];
            if (ok) std::snprintf(msg, sizeof msg, "Saved %s (%zu environments, %.0f MB)", narrow(fs::path(widen(out)).filename().wstring()).c_str(), files.size(), mb);
            {
                std::lock_guard<std::mutex> l(statusMutex);
                bundleStatus = ok ? std::string(msg) : "Couldn't save the bundle: " + err;
                if (ok) bundleSavedTo = out;
            }
            bundleBusy = false;
        });
    }
    std::string bundleSavedTo;               // set by the saver (statusMutex); the UI thread then makes it the bundle's file

    // Unpacks the opened bundle into My Environments.
    void unpackBundle() {
        if (bundleBusy || bundleFile.empty() || bundleDirty) return;
        if (bundler.joinable()) bundler.join();
        bundleBusy = true;
        const std::string file = bundleFile;
        const fs::path dir = libraryDir();
        bundler = std::thread([this, file, dir] {
            std::vector<std::string> out;
            std::string err;
            const bool ok = fenvBundleExtract(file, narrow(dir.wstring()), out, err, {}, [this](uint64_t done, uint64_t total) {
                char msg[96];
                std::snprintf(msg, sizeof msg, "Unpacking\u2026 %.0f%%", total ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 100.0);
                setBundleStatus(msg);
            });
            setBundleStatus(ok ? "Unpacked into My Environments (" + std::to_string(out.size()) + (out.size() == 1 ? " environment)" : " environments)")
                               : "Couldn't unpack the bundle: " + err);
            bundleBusy = false;
        });
    }

    // The saved bundle goes onto the install list, and the Headset tab opens to install it.
    void installBundle() {
        if (bundleFile.empty() || bundleDirty) return;
        seedInstallEnvs();
        if (std::find(installEnvs.begin(), installEnvs.end(), bundleFile) == installEnvs.end()) installEnvs.push_back(bundleFile);
        showInspectorTab(InspectorTab::Headset);
    }

    // What the background work left for the UI thread: a member to open, a bundle file saved.
    void bundleFinished() {
        std::vector<std::string> openNow;
        std::string saved;
        {
            std::lock_guard<std::mutex> l(statusMutex);
            openNow.swap(bundleOpenLater);
            saved.swap(bundleSavedTo);
        }
        if (!saved.empty()) {
            bundleFile = saved;
            bundleDirty = false;
            if (bundleName.empty()) bundleName = narrow(fs::path(widen(saved)).stem().wstring());
        }
        if (!openNow.empty()) open(openNow);
    }

    // Inspector › Bundle.
    void bundleTab() {
        section("BUNDLE");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Several environments in one file, to share or install together. Each stays its own environment on the headset.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::BeginDisabled(bundleBusy);
        fieldLabel("Name");
        {
            char nb[128];
            std::snprintf(nb, sizeof nb, "%s", bundleName.c_str());
            if (ImGui::InputTextWithHint("##bundlename", "New bundle", nb, sizeof nb)) { bundleName = nb; bundleDirty = true; }
        }
        if (!bundleFile.empty()) {
            ImGui::TextDisabled("%s%s", narrow(fs::path(widen(bundleFile)).filename().wstring()).c_str(), bundleDirty ? " (changed)" : "");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bundleFile.c_str());
        }

        ImGui::SeparatorText("Environments");
        if (bundleMembers.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("None yet. Add the open environment, environments from My Environments, or any .fenv.");
            ImGui::PopTextWrapPos();
        }
        const float thumbW = 64 * scale, rowH = std::max(thumbW * 9 / 16, ImGui::GetTextLineHeight() * 2 + 4 * scale);
        int remove = -1, openIt = -1, moveUp = -1;
        uint64_t total = 0;
        for (size_t i = 0; i < bundleMembers.size(); ++i) {
            BundleMember& m = bundleMembers[i];
            total += m.size;
            if (m.thumbAspect == 0) loadBundleThumb(m);
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 t1(a.x + thumbW, a.y + rowH);
            if (m.thumb) {   // fitted into the slot, centred
                const float h = std::min(rowH, thumbW * m.thumbAspect), w = h / m.thumbAspect;
                const ImVec2 p((a.x + t1.x - w) * 0.5f, (a.y + t1.y - h) * 0.5f);
                dl->AddImage((ImTextureID)m.thumb, p, ImVec2(p.x + w, p.y + h));
            } else dl->AddRectFilled(a, t1, col(0x3f9a5c, 0.22f), 4 * scale);
            const float tx = t1.x + 8 * scale, lh = ImGui::GetTextLineHeight();
            const float right = ImGui::GetContentRegionMax().x + ImGui::GetWindowPos().x;
            dl->PushClipRect(ImVec2(tx, a.y), ImVec2(right - 52 * scale, a.y + rowH), true);
            dl->AddText(ImVec2(tx, a.y + rowH * 0.5f - lh - 1 * scale), col(kText), m.name.c_str());
            char sub[64];
            std::snprintf(sub, sizeof sub, "%.0f MB%s", static_cast<double>(m.size) / 1048576.0, m.member.empty() ? "" : " \u00b7 in the bundle");
            dl->AddText(ImVec2(tx, a.y + rowH * 0.5f + 1 * scale), col(kDim), sub);
            dl->PopClipRect();
            if (ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, right - a.x - 52 * scale), rowH)) ) {}
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s\nDouble-click to open it", m.name.c_str(), m.member.empty() ? m.path.c_str() : (m.path + " \u203a " + m.member).c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) openIt = static_cast<int>(i);
            }
            ImGui::SameLine(0, 4 * scale);
            ImGui::BeginGroup();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("\u25b2")) moveUp = static_cast<int>(i);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Move up: the first one is the one shown after installing");
            if (ImGui::SmallButton("\u2715")) remove = static_cast<int>(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Take it out of the bundle (the file itself stays)");
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 2 * scale));
        }
        if (moveUp > 0) { std::swap(bundleMembers[static_cast<size_t>(moveUp)], bundleMembers[static_cast<size_t>(moveUp) - 1]); bundleDirty = true; }
        if (remove >= 0) removeFromBundle(static_cast<size_t>(remove));
        if (openIt >= 0) openBundleMember(static_cast<size_t>(openIt));
        if (!bundleMembers.empty()) ImGui::TextDisabled("%zu environment%s \u00b7 %.0f MB", bundleMembers.size(), bundleMembers.size() == 1 ? "" : "s", static_cast<double>(total) / 1048576.0);

        ImGui::Spacing();
        const std::string here = openEnvFile();
        ImGui::BeginDisabled(here.empty());
        if (ImGui::Button("Add the Open Environment", ImVec2(-1, 0))) addToBundle({here});
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", here.empty() ? "Save it first (Ctrl+S): a bundle holds saved environments"
                                                 : dirty ? "As it was last saved: save first (Ctrl+S) to include your latest changes" : here.c_str());
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("My Environments \u25be", ImVec2(half, 0))) ImGui::OpenPopup("##mine");
        if (ImGui::BeginPopup("##mine")) {
            const std::vector<std::string> mine = libraryEnvs();
            if (mine.empty()) ImGui::TextDisabled("None yet: Save (Ctrl+S) puts what you make here");
            for (size_t r = 0; r < mine.size(); ++r) {
                ImGui::PushID(static_cast<int>(r));
                if (ImGui::MenuItem(recentDetails({mine[r]}).title.c_str())) addToBundle({mine[r]});
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add .fenv\u2026", ImVec2(-1, 0))) addToBundleDialog();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::BeginDisabled(bundleMembers.empty());
        pushPrimary();
        if (ImGui::Button(bundleFile.empty() ? "Save Bundle\u2026" : "Save Bundle", ImVec2(-1, ImGui::GetFrameHeight() * 1.4f))) {
            if (bundleFile.empty()) saveBundleDialog(); else saveBundle(bundleFile);
        }
        ImGui::PopStyleColor(3);
        if (ImGui::Button("Save Bundle As\u2026", ImVec2(-1, 0))) saveBundleDialog();
        ImGui::EndDisabled();
        const bool saved = !bundleFile.empty() && !bundleDirty;
        ImGui::BeginDisabled(!saved);
        if (ImGui::Button("Install on Steam Frame\u2026", ImVec2(-1, 0))) installBundle();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", saved ? "Adds it to the Headset tab's install list: Frame Home unpacks it there into its environments"
                                          : "Save the bundle first");
        if (ImGui::Button("Unpack into My Environments", ImVec2(-1, 0))) unpackBundle();
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        std::string st;
        { std::lock_guard<std::mutex> l(statusMutex); st = bundleStatus; }
        if (!st.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(st.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // A world point on the viewport (false behind the camera).
    bool screenOf(const float* p, ImVec2& out) const {
        const float ty = std::tan(kFovY * 0.5f), tx = ty * std::max(1.0f, viewSize.x) / std::max(1.0f, viewSize.y);
        float pose[16], view[16], proj[16], vp[16];
        cam.pose(pose);
        invert(pose, view);
        glProjection(tx, ty, proj);
        mul(proj, view, vp);
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = vp[k] * p[0] + vp[4 + k] * p[1] + vp[8 + k] * p[2] + vp[12 + k];
        if (c[3] < kNear) return false;
        out = ImVec2(viewPos.x + (c[0] / c[3] * 0.5f + 0.5f) * viewSize.x, viewPos.y + (0.5f - c[1] / c[3] * 0.5f) * viewSize.y);
        return true;
    }

    // Help > Controls (F1).
    void controlsWindow() {
        if (!showControls) return;
        ImGui::SetNextWindowSize(ImVec2(420 * scale, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::Begin("Controls", &showControls, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
        static const char* rows[][2] = {
            {"Orbit", "Middle drag, or Alt + left drag"}, {"Pan", "Shift + middle drag"}, {"Zoom", "Wheel, or Ctrl + middle drag"},
            {"Fly", "Hold right mouse + W A S D, Q / E down / up"}, {"Fly speed", "Wheel while flying, Shift = 4x"},
            {"Select", "Left click (viewport or explorer)"}, {"Move / Rotate / Scale", "G / R / S, hold Ctrl to snap"},
            {"Frame selected / all", "F / Home (or double-click a row)"}, {"Player view", "Numpad 0 or P"},
            {"Hide / reveal hidden", "H / Alt + H"}, {"Delete", "X (asks) or Delete"}, {"Undo / redo", "Ctrl + Z / Ctrl + Shift + Z"},
            {"Open / save edits", "Ctrl + O / Ctrl + S"}, {"Show only selected", "I (I or Esc shows everything)"},
            {"Show / hide world layer", "W (each kind: View \u203a Show, or the eyes in the explorer)"},
            {"Floor / wall from a mesh", "Properties \u203a Object \u203a Surface, or right-click it"},
            {"Every mesh walkable where flat, blocking where steep", "Object \u203a Auto-generate Walkable Surfaces"},
            {"Add at the view's centre", "Shift + A"}, {"Find any command", "F3"},
            {"Toolbar / sidebar", "T / N"}, {"Front / right / top view", "Numpad 1 / 3 / 7"},
            {"Maximize the 3D Viewport", "Ctrl + Space"}, {"Play / pause", "Space"},
            {"Deselect", "Esc or Alt + A"}};
        if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", r[0]);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r[1]);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }
};


// ── Self-test (Frame Studio.exe --selftest <home files>) ────────────────────────────────────────────
// Drives the real UI frame by frame — mouse moves, presses and drags fed to ImGui exactly like a user's, on the
// actual gizmo and viewport — and checks what changed: moving, rotating and scaling objects and world items,
// surfaces made from meshes, what is shown, undo. Writes selftest\report.txt (+ screenshots) next to the edits
// folder and quits. Nothing is saved to the home's edits.

struct SelfTest {
    Studio& a;
    std::vector<std::string> paths;
    fs::path dir;
    FILE* report = nullptr;
    int passes = 0, fails = 0;

    // Input for the coming frames: one batch per frame.
    struct In { bool move = false; ImVec2 pos; int button = -1; bool down = false; ImGuiKey key = ImGuiKey_None; bool ctrl = false; };
    std::deque<In> queue;
    ImVec2 mouse{-1, -1};

    // The script: each stage runs once its wait is over; it returns the frames to wait before the next one,
    // or -1 to run again next frame (polling).
    std::vector<std::function<int()>> stages;
    size_t stage = 0;
    int wait = 30;
    int polls = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    // Scratch shared between stages.
    std::vector<Studio::TestEye> eyes;
    int folderEye = -1, single = -1;
    int obj = -1;
    float before[16] = {}, objWorld[16] = {};
    ImVec2 probeHit{-1, -1};
    size_t collidersBefore = 0, spawnsBefore = 0, splitBefore = 0;
    int autoCount = 0;
    std::vector<std::string> autoNames;
    std::string firstGrid;
    float wallAt[3] = {};
    std::shared_ptr<HomeWorld> worldBefore;

    SelfTest(Studio& app, std::vector<std::string> p) : a(app), paths(std::move(p)) {
        dir = dataDir() / L"selftest";
        std::error_code ec;
        fs::create_directories(dir, ec);
        report = platform::openFile((dir / L"report.txt").c_str(), "wb");
        a.testing = true;
        log("Frame Studio self-test");
        for (const auto& h : paths) log("home: %s", h.c_str());
        build();
    }

    void log(const char* fmt, ...) {
        char b[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (report) { std::fprintf(report, "%s\n", b); std::fflush(report); }
        std::fprintf(stderr, "[selftest] %s\n", b);
    }
    void check(bool ok, const char* what, const std::string& detail = {}) {
        (ok ? passes : fails)++;
        log("%s  %s%s%s", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  — ", detail.c_str());
    }
    static std::string f3(const float* v) { char b[96]; std::snprintf(b, sizeof b, "(%.3f %.3f %.3f)", v[0], v[1], v[2]); return b; }
    static float colLen(const float* m, int c) { return std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]); }
    static float dist3(const float* p, const float* q) { return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])); }

    void moveTo(ImVec2 p) { In i; i.move = true; i.pos = p; queue.push_back(i); }
    void press(bool down) { In i; i.button = 0; i.down = down; queue.push_back(i); }
    void key(ImGuiKey k, bool ctrl = false) { In i; i.key = k; i.ctrl = ctrl; queue.push_back(i); }
    void pressB(int b, bool down) { In i; i.button = b; i.down = down; queue.push_back(i); }
    void click(ImVec2 p) { moveTo(p); moveTo(p); press(true); queue.push_back(In{}); press(false); queue.push_back(In{}); }
    // A left-button drag in small steps, the way a hand does it.
    void drag(ImVec2 from, ImVec2 to, int steps = 12) {
        moveTo(from); moveTo(from);
        press(true);
        In hold; queue.push_back(hold);
        for (int k = 1; k <= steps; ++k) moveTo(ImVec2(from.x + (to.x - from.x) * k / steps, from.y + (to.y - from.y) * k / steps));
        moveTo(to);
        press(false);
        queue.push_back(In{}); queue.push_back(In{});
    }

    // Called between the backend's NewFrame and ImGui::NewFrame: these events land last, so they win.
    void inject(ImGuiIO& io) {
        io.ConfigInputTrickleEventQueue = false;
        if (mouse.x >= 0) io.AddMousePosEvent(mouse.x, mouse.y);
        if (queue.empty()) return;
        const In i = queue.front();
        queue.pop_front();
        if (i.move) { mouse = i.pos; io.AddMousePosEvent(mouse.x, mouse.y); }
        if (i.button >= 0) io.AddMouseButtonEvent(i.button, i.down);
        if (i.key != ImGuiKey_None) {
            if (i.ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(i.key, true);
            pendingUp.push_back({i.key, i.ctrl});
        } else {
            for (auto& u : pendingUp) { io.AddKeyEvent(u.first, false); if (u.second) io.AddKeyEvent(ImGuiMod_Ctrl, false); }
            pendingUp.clear();
        }
    }
    std::vector<std::pair<ImGuiKey, bool>> pendingUp;

    int idConflictFrames = 0;
    std::string idConflictWhere;
    std::chrono::steady_clock::time_point pollStart;   // when the stage being polled started waiting

    // After the UI of a frame.
    void afterUi() {
        if (ImGui::GetCurrentContext()->DebugDrawIdConflicts != 0) {   // ImGui's "conflicting ID" error, anywhere
            if (!idConflictFrames++) { char b[96]; std::snprintf(b, sizeof b, "first at mouse (%.0f, %.0f), stage %zu", mouse.x, mouse.y, stage); idConflictWhere = b; }
        }
        if (!queue.empty()) return;                      // input still playing
        if (wait > 0) { --wait; return; }
        if (stage >= stages.size()) return;
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(6)) { check(false, "finished in time"); stage = stages.size(); finish(); return; }
        const int w = stages[stage]();
        if (w < 0) {   // still waiting: a minute, but at least 3000 frames (a software GPU draws ~30 a second, and a gizmo
                       // probe takes a frame a step); 5 minutes at most whatever the frame rate
            if (polls++ == 0) pollStart = std::chrono::steady_clock::now();
            const auto waited = std::chrono::steady_clock::now() - pollStart;
            if ((waited > std::chrono::seconds(60) && polls > 3000) || waited > std::chrono::minutes(5)) {
                check(false, "a stage timed out", std::to_string(stage)); polls = 0; ++stage;
            }
            return;
        }
        polls = 0;
        wait = w;
        ++stage;
    }

    // The whole window as shown (UI, gizmo and world overlays included).
    void screenshot(const char* name) {
        std::vector<u8> rgba;
        int w = 0, h = 0;
        if (!a.window.capture || !a.window.capture(rgba, w, h) || w < 8 || h < 8) { log("(no screenshot %s)", name); return; }
        const fs::path file = dir / widen(std::string(name) + ".png");
        const std::string path = narrow(file.wstring());
        if (FILE* f = platform::openFile(file.c_str(), "wb")) {
            stbi_write_png_to_func([](void* ctx, void* data, int size) { std::fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE*>(ctx)); },
                                   f, w, h, 4, rgba.data(), w * 4);
            std::fclose(f);
        }
        log("screenshot %s", path.c_str());
    }

    // Walks the mouse outwards from the gizmo centre along `dir` until ImGuizmo reports a handle under it.
    // Stage helper: returns -1 while probing; sets probeHit (x < 0 = none) when done.
    float probeR = 0;
    ImVec2 probeC, probeD;
    void startProbe(ImVec2 c, ImVec2 d, float r0) {
        const float L = std::max(1e-3f, std::sqrt(d.x * d.x + d.y * d.y));
        probeC = c; probeD = ImVec2(d.x / L, d.y / L); probeR = r0; probeHit = ImVec2(-1, -1);
        moveTo(ImVec2(c.x + probeD.x * r0, c.y + probeD.y * r0));
        queue.push_back(In{}); queue.push_back(In{});
    }
    std::vector<ImVec2> probeDirs;                       // further directions to try when one finds nothing
    float probeR0 = 0;
    int probe() {
        if (a.gizmoHot) { probeHit = mouse; return 0; }
        probeR += 4 * a.scale;
        if (probeR > 320 * a.scale) {
            if (probeDirs.empty()) return 0;
            const ImVec2 d = probeDirs.back();
            probeDirs.pop_back();
            startProbe(probeC, d, probeR0);
            return -1;
        }
        moveTo(ImVec2(probeC.x + probeD.x * probeR, probeC.y + probeD.y * probeR));
        queue.push_back(In{}); queue.push_back(In{});    // ImGuizmo's hover is a frame behind
        return -1;
    }

    bool gizmoCentre(ImVec2& c, ImVec2& xDir, ImVec2& yDir, ImVec2& zDir) {
        float m[16];
        if (a.wKind >= 0) { if (!a.worldMatrix(a.wKind, a.wIndex, m)) return false; }
        else if (a.selected >= 0) std::memcpy(m, a.objects[a.selected].pivot, 64);
        else return false;
        const float o[3] = {m[12], m[13], m[14]};
        if (!a.screenOf(o, c)) return false;
        ImVec2 e;
        const float px[3] = {o[0] + 0.3f, o[1], o[2]}, py[3] = {o[0], o[1] + 0.3f, o[2]}, pz[3] = {o[0], o[1], o[2] + 0.3f};
        if (a.screenOf(px, e)) xDir = ImVec2(e.x - c.x, e.y - c.y);
        if (a.screenOf(py, e)) yDir = ImVec2(e.x - c.x, e.y - c.y);
        if (a.screenOf(pz, e)) zDir = ImVec2(e.x - c.x, e.y - c.y);
        return true;
    }

    // Probe for a handle along a screen direction, then drag it by `delta` pixels. Adds the stages.
    void gizmoDrag(const char* what, Tool tool, int axis /* 0 x, 1 y, 2 z, 3 centre, 4 rotate ring */, ImVec2 delta) {
        stages.push_back([this, tool] { a.tool = tool; a.localSpace = false; return 3; });
        stages.push_back([this, axis] {
            ImVec2 c, dx, dy, dz;
            if (!gizmoCentre(c, dx, dy, dz)) { probeHit = ImVec2(-1, -1); probeR = 1e9f; return 0; }
            const ImVec2 d = axis == 0 ? dx : axis == 1 ? dy : axis == 2 ? dz : axis == 4 ? ImVec2(0.7f, -0.7f) : ImVec2(1, 0);
            probeR0 = axis == 3 ? 0.0f : 14 * a.scale;
            probeDirs.clear();
            if (axis == 4) for (int k = 0; k < 8; ++k) probeDirs.push_back(ImVec2(std::cos(k * kPi / 4 + 0.3f), std::sin(k * kPi / 4 + 0.3f)));
            else if (axis < 3) probeDirs.push_back(ImVec2(-d.x, -d.y));
            log("  gizmo at (%.0f, %.0f), probing towards (%.1f, %.1f); view (%.0f, %.0f) %.0f x %.0f", c.x, c.y, d.x, d.y, a.viewPos.x, a.viewPos.y, a.viewSize.x, a.viewSize.y);
            startProbe(c, d, probeR0);
            return 0;
        });
        stages.push_back([this] { return probe(); });
        stages.push_back([this, what, delta] {
            if (probeHit.x < 0) {
                char st[200];
                std::snprintf(st, sizeof st, "no gizmo handle found under the mouse (world item %d #%d, object %d, tool %d, popup %s, %zu zones)", a.wKind, a.wIndex,
                              a.selected, static_cast<int>(a.tool), ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) ? "open" : "none",
                              a.scene ? a.scene->world.gravity.size() : 0);
                check(false, what, st);
                screenshot("gizmo_not_found");
                return 0;
            }
            if (a.wKind >= 0) { float m[16]; a.worldMatrix(a.wKind, a.wIndex, m); std::memcpy(before, m, 64); }
            else std::memcpy(before, a.objects[a.selected].pivot, 64);
            drag(probeHit, ImVec2(probeHit.x + delta.x, probeHit.y + delta.y));
            return 4;
        });
    }

    float* current(float* m) {
        if (a.wKind >= 0) { if (!a.worldMatrix(a.wKind, a.wIndex, m)) std::memcpy(m, before, 64); }
        else if (a.selected >= 0) std::memcpy(m, a.objects[a.selected].pivot, 64);
        return m;
    }

    void build() {
        // ── A slow load asks; Stop Loading stops it, and the stopped load's result never shows up
        stages.push_back([this] { a.open(paths, false); a.loadPromptAfter = 0.05f; return 1; });
        stages.push_back([this] {
            if (!a.loadPromptOpen && a.isLoading()) return -1;
            check(a.loadPromptOpen, "a load that takes too long asks whether to keep waiting");
            return 3;                                     // drawn from the next frame
        });
        stages.push_back([this] {
            screenshot("00_still_loading");
            a.stopLoading();
            check(a.loadState == 0 && !a.scene && !a.isLoading(), "Stop Loading stops it");
            return 270;                                   // 3 s: long enough for the stopped load to have finished
        });
        stages.push_back([this] {
            check(a.loadState == 0 && !a.scene, "the stopped load doesn't appear later");
            return 1;
        });
        // ── Load
        stages.push_back([this] {
            if (isFootprintApk(paths[0])) {   // a footprint is paired with a vista beside it (the test opens the room alone)
                const std::vector<std::string> v = vistasBeside(paths[0]);
                check(!v.empty() && isVistaApk(v[0]), "the vistas beside a footprint are found to pair it with", std::to_string(v.size()) + (v.empty() ? "" : ", e.g. " + prettyHome(v[0])));
            }
            a.open(paths, false);
            return 5;
        });
        stages.push_back([this] {
            if (a.loadState != 0 || !a.scene || a.uploading || a.texLoading || a.objects.empty() || !a.rendered) return -1;
            check(true, "home loaded", std::to_string(a.objects.size()) + " objects, " + std::to_string(a.scene->world.colliders.size()) + " surfaces");
            for (const fs::path& f : {a.worldFile(), a.editsFile()}) {   // put back as found when the test ends
                std::ifstream in(f, std::ios::binary);
                saved.push_back({f, static_cast<bool>(in), std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())});
            }
            a.frameAll();
            return 20;
        });
        stages.push_back([this] { screenshot("01_loaded"); return 1; });
        // ── Blender's window: an area dragged by its editor button docks elsewhere; Reset Layout puts it back;
        // workspaces; F3, Shift+A and X open their menus
        stages.push_back([this] {
            const int o = static_cast<int>(Studio::Editor::Outliner), v = static_cast<int>(Studio::Editor::Viewport);
            check(a.editorDock[o] != 0 && a.editorDock[v] != 0 && a.editorDock[o] != a.editorDock[v], "the areas start in Blender's layout (each its own)");
            outlinerDockWas = a.editorDock[o];
            const ImVec2 from = a.editorButtonAt[o], to = a.areaCentre[v];
            moveTo(from); moveTo(from); press(true); queue.push_back(In{});
            for (int k = 1; k <= 30; ++k) moveTo(ImVec2(from.x + (to.x - from.x) * k / 30, from.y + (to.y - from.y) * k / 30));
            for (int k = 0; k < 8; ++k) moveTo(to);   // over the target while the docking preview shows
            press(false);
            queue.push_back(In{}); queue.push_back(In{});
            return 4;
        });
        stages.push_back([this] {
            const int o = static_cast<int>(Studio::Editor::Outliner), v = static_cast<int>(Studio::Editor::Viewport);
            check(a.editorDock[o] != outlinerDockWas && a.editorDock[o] == a.editorDock[v], "dragging the Outliner by its editor button docks it with the 3D Viewport",
                  a.editorDock[o] == outlinerDockWas ? "it stayed where it was" : a.editorDock[o] == 0 ? "it floats, undocked" : "");
            screenshot("01_area_moved");
            a.layoutWanted = true;                      // Window > Reset Layout
            return 3;
        });
        stages.push_back([this] {
            const int o = static_cast<int>(Studio::Editor::Outliner), v = static_cast<int>(Studio::Editor::Viewport);
            check(a.editorDock[o] != 0 && a.editorDock[o] != a.editorDock[v], "Window \u203a Reset Layout puts the areas back");
            a.setWorkspace(1);
            return 3;
        });
        stages.push_back([this] {
            check(a.workspace == 1 && a.inspectorTab == Studio::InspectorTab::World, "the Walking workspace shows Properties \u203a World");
            screenshot("01_walking_workspace");
            a.setWorkspace(0);
            key(ImGuiKey_F3);
            return 3;
        });
        stages.push_back([this] {
            check(a.searchShowing, "F3 opens Menu Search");
            key(ImGuiKey_Escape);
            return 2;
        });
        stages.push_back([this] {
            check(!a.searchShowing, "...and Esc closes it");
            return 1;
        });

        // ── Pick a mid-sized object, centred in view
        stages.push_back([this] {
            float best = 1e30f;
            for (size_t i = 0; i < a.objects.size(); ++i) {
                const Object& o = a.objects[i];
                if (!a.canBeSurface(o) || o.hidden || o.category == kPlants) continue;
                float mn[3], mx[3];
                a.worldBounds(o, mn, mx);
                const float d = dist3(mn, mx);
                if (d <= 0.4f || std::fabs(d - 2.0f) >= best) continue;
                std::vector<u32> fl, wl;                      // furniture-like: something to stand on and sides
                a.classifyObject(o, fl, wl);
                if (fl.size() < 4 || wl.size() < 4) continue;
                best = std::fabs(d - 2.0f);
                obj = static_cast<int>(i);
            }
            check(obj >= 0, "found an object to edit", obj >= 0 ? a.objects[obj].label : "");
            if (obj < 0) { stage = stages.size() - 2; return 0; }
            a.selected = obj; a.wKind = a.wIndex = -1;
            a.frameSelected();
            a.cam.dist *= 1.6f;
            return 15;
        });

        // ── Objects: move (centre), move on X, rotate, scale — through the gizmo, with the mouse
        gizmoDrag("object: drag the gizmo centre", Tool::Move, 3, ImVec2(70, 30));
        stages.push_back([this] {
            float m[16]; current(m);
            const float d = dist3(&m[12], &before[12]);
            check(d > 0.02f, "object moves with the gizmo (screen drag)", f3(&before[12]) + " -> " + f3(&m[12]));
            return 2;
        });
        gizmoDrag("object: drag the X arrow", Tool::Move, 0, ImVec2(80, 0));
        stages.push_back([this] {
            float m[16]; current(m);
            const float dx = m[12] - before[12], dy = m[13] - before[13], dz = m[14] - before[14];
            check(std::fabs(dx) > 0.02f && std::fabs(dy) < 1e-3f && std::fabs(dz) < 1e-3f, "object moves along X only on the X arrow",
                  f3(&before[12]) + " -> " + f3(&m[12]));
            return 2;
        });
        gizmoDrag("object: drag the rotate ring", Tool::Rotate, 4, ImVec2(40, 60));
        stages.push_back([this] {
            float m[16]; current(m);
            float rot = 0;
            for (int k = 0; k < 11; ++k) if (k % 4 != 3) rot += std::fabs(m[k] - before[k]);
            check(rot > 0.05f && dist3(&m[12], &before[12]) < 1e-3f, "object rotates with the gizmo (and stays put)", "change " + std::to_string(rot));
            return 2;
        });
        gizmoDrag("object: drag the X scale handle", Tool::Scale, 0, ImVec2(60, 0));
        stages.push_back([this] {
            float m[16]; current(m);
            check(std::fabs(colLen(m, 0) - colLen(before, 0)) > 0.05f, "object scales with the gizmo",
                  std::to_string(colLen(before, 0)) + " -> " + std::to_string(colLen(m, 0)));
            return 2;
        });
        stages.push_back([this] { screenshot("02_object_edited"); return 1; });
        // Each drag is one undo step: four Ctrl+Z bring it back.
        stages.push_back([this] { for (int k = 0; k < 4; ++k) { key(ImGuiKey_Z, true); queue.push_back(In{}); } return 3; });
        stages.push_back([this] {
            const Object& o = a.objects[obj];
            float d = 0;
            for (int k = 0; k < 16; ++k) d += std::fabs(o.pivot[k] - o.pivot0[k]);
            check(d < 1e-3f, "Ctrl+Z x4 undoes the four drags", "difference " + std::to_string(d));
            return 2;
        });
        // Keyboard tools
        stages.push_back([this] { key(ImGuiKey_R); queue.push_back(In{}); return 1; });
        stages.push_back([this] { check(a.tool == Tool::Rotate, "R picks Rotate"); key(ImGuiKey_S); queue.push_back(In{}); return 1; });
        stages.push_back([this] { check(a.tool == Tool::Scale, "S picks Scale"); key(ImGuiKey_G); queue.push_back(In{}); return 1; });
        stages.push_back([this] { check(a.tool == Tool::Move, "G picks Move"); return 1; });
        // Clicking the object in the viewport selects it (and empty space doesn't keep the gizmo stuck)
        stages.push_back([this] {
            a.selected = -1;
            float mn[3], mx[3];
            a.worldBounds(a.objects[obj], mn, mx);
            const float c[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
            ImVec2 sp;
            if (a.screenOf(c, sp)) { moveTo(sp); moveTo(sp); press(true); queue.push_back(In{}); press(false); queue.push_back(In{}); }
            return 3;
        });
        stages.push_back([this] {
            check(a.selected >= 0, "clicking in the viewport selects an object", a.selected >= 0 ? a.objects[a.selected].label : "nothing");
            a.selected = obj;
            return 2;
        });

        // ── The explorer: every eye button hovered (no ImGui ID conflicts), and folders hide what's in them
        stages.push_back([this] {
            eyes = a.testEyes;
            check(!eyes.empty(), "the explorer shows eye buttons", std::to_string(eyes.size()) + " visible");
            for (const auto& e : eyes) { moveTo(e.at); queue.push_back(In{}); queue.push_back(In{}); }
            return 2;
        });
        stages.push_back([this] {
            check(idConflictFrames == 0, "no ImGui ID conflicts while hovering every eye", idConflictWhere);
            folderEye = -1;
            for (size_t k = 0; k < eyes.size(); ++k) if (eyes[k].members.size() > 1) { folderEye = static_cast<int>(k); break; }
            check(folderEye >= 0, "found a folder to hide");
            if (folderEye < 0) return 0;
            const ImVec2 p = eyes[folderEye].at;
            moveTo(p); moveTo(p); press(true); queue.push_back(In{}); press(false); queue.push_back(In{}); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            if (folderEye < 0) return 0;
            int hidden = 0;
            for (size_t i : eyes[folderEye].members) hidden += a.objects[i].hidden;
            check(hidden == static_cast<int>(eyes[folderEye].members.size()), "clicking a folder's eye hides everything in it",
                  std::to_string(hidden) + " of " + std::to_string(eyes[folderEye].members.size()));
            const ImVec2 p = eyes[folderEye].at;
            moveTo(p); press(true); queue.push_back(In{}); press(false); queue.push_back(In{}); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            if (folderEye < 0) return 0;
            int shown = 0;
            for (size_t i : eyes[folderEye].members) shown += !a.objects[i].hidden;
            check(shown == static_cast<int>(eyes[folderEye].members.size()), "clicking it again shows them all");
            // a single row's eye
            for (const auto& e : eyes)
                if (e.members.size() == 1) {
                    single = static_cast<int>(e.members[0]);
                    moveTo(e.at); moveTo(e.at); press(true); queue.push_back(In{}); press(false); queue.push_back(In{}); queue.push_back(In{});
                    break;
                }
            return 3;
        });
        stages.push_back([this] {
            if (single >= 0) {
                check(a.objects[single].hidden, "an object's eye hides just it", a.objects[single].label);
                a.toggleHidden(static_cast<size_t>(single));
            }
            return 1;
        });

        // ── Surfaces from meshes
        stages.push_back([this] {
            collidersBefore = a.scene->world.colliders.size();
            a.setSurfaces({static_cast<size_t>(obj)}, kSurfSolid);
            const int at = a.surfaceIndex(a.objects[obj]);
            check(at >= 0 && a.scene->world.colliders[at].role == WorldCollider::Solid && a.surfaceParts(a.objects[obj]).size() == 1, "object becomes a surface (one)");
            return 2;
        });
        gizmoDrag("surface object: move", Tool::Move, 3, ImVec2(-60, 20));
        stages.push_back([this] {
            const int at = a.surfaceIndex(a.objects[obj]);
            float w[16];
            mul(a.objects[obj].pivot, a.objects[obj].offset, w);
            float d = 0;
            if (at >= 0) for (int k = 0; k < 16; ++k) d += std::fabs(a.scene->world.colliders[at].world[k] - w[k]);
            check(at >= 0 && d < 1e-4f, "its surface follows the object when moved");
            WorldRuntime rt;
            rt.build(walkWorld(*a.scene));
            check(rt.hasFloor(), "Frame Home's walker finds floor");
            return 2;
        });
        stages.push_back([this] {
            a.deleteSelected();
            check(a.surfaceIndex(a.objects[obj]) < 0 && a.objects[obj].deleted, "deleting the object removes its surface");
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            check(!a.objects[obj].deleted && a.surfaceIndex(a.objects[obj]) >= 0, "one Ctrl+Z brings back both");
            return 1;
        });
        stages.push_back([this] {
            const std::vector<size_t> all = a.surfaceCandidates();
            const size_t n0 = a.scene->world.colliders.size();
            linkedBefore = 0;
            for (const WorldCollider& c : a.scene->world.colliders) linkedBefore += !c.mesh.empty();
            a.setSurfaces(all, kSurfSolid);
            size_t linked = 0;
            for (const WorldCollider& c : a.scene->world.colliders) linked += !c.mesh.empty() && c.role == WorldCollider::Solid;
            check(linked == all.size() && !all.empty(), "All Visible Meshes > Make Surfaces", std::to_string(all.size()) + " meshes, " + std::to_string(n0) + " -> " + std::to_string(a.scene->world.colliders.size()) + " surfaces");
            a.selected = -1; a.wKind = a.wIndex = -1;
            a.frameAll();
            return 15;
        });
        stages.push_back([this] { screenshot("03_all_walkable"); return 1; });
        stages.push_back([this] {
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            size_t linked = 0;
            std::string which;
            for (const WorldCollider& c : a.scene->world.colliders)
                if (!c.mesh.empty()) { ++linked; which += (which.empty() ? "" : ", ") + c.name + " [" + c.mesh + "]"; }
            check(linked == linkedBefore, "Ctrl+Z undoes making them all surfaces",
                  std::to_string(linked) + " mesh surfaces, " + std::to_string(linkedBefore) + " before: " + which);
            return 1;
        });

        // ── Surface from the object panel: walk on its flat parts, its steep parts stop you; the right-click menu; a turn
        stages.push_back([this] {
            a.selected = obj; a.wKind = a.wIndex = -1;
            a.frameSelected();
            a.cam.dist *= 1.6f;
            a.cam.pitch = -0.6f;            // from above: the floor part shows
            for (ImVec2& p : a.surfButtonAt) p = ImVec2(-1, -1);
            a.surfScroll = true;                           // in view, however small the window
            return 10;
        });
        stages.push_back([this] {
            a.setSurfaces({static_cast<size_t>(obj)}, kSurfNone);
            if (a.surfButtonAt[kSurfSolid].x < 0) { check(false, "the object panel shows the Surface buttons"); return 0; }
            click(a.surfButtonAt[kSurfSolid]);
            return 4;
        });
        stages.push_back([this] {
            const std::vector<int> parts = a.surfaceParts(a.objects[obj]);
            const size_t total = a.scene->meshes[a.objects[obj].scene].indices.size() / 3;
            const size_t got = parts.size() == 1 ? a.scene->world.colliders[parts[0]].indices.size() / 3 : 0;
            check(parts.size() == 1 && a.scene->world.colliders[parts[0]].role == WorldCollider::Solid && a.surfaceMode(a.objects[obj]) == kSurfSolid && got > 0 && got <= total,
                  "Surface makes it one surface, simplified for walking", std::to_string(got) + " of " + std::to_string(total) + " triangles");
            // Frame Home's walker: stands on its flat top, is stopped by its steep side.
            HomeWorld hw;
            for (int p : parts) hw.colliders.push_back(a.scene->world.colliders[p]);
            WorldRuntime rt;
            rt.build(hw);
            float bestF = 0, bestW = 0, fc[3] = {}, wc[3] = {}, wn[3] = {};
            for (const WorldCollider& c : hw.colliders) {
                const std::vector<float> wp = worldMeshPositions(c);
                for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                    const float* p0 = &wp[c.indices[t] * 3]; const float* p1 = &wp[c.indices[t + 1] * 3]; const float* p2 = &wp[c.indices[t + 2] * 3];
                    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                    const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (L < 1e-9f) continue;
                    const bool fl = n[1] / L > 0.9f;                       // flat enough to stand on
                    if (!fl && std::fabs(n[1]) / L > 0.2f) continue;      // else a near-vertical one to walk into
                    float& best = fl ? bestF : bestW;
                    if (L * 0.5f <= best) continue;
                    best = L * 0.5f;
                    float* cc = fl ? fc : wc;
                    for (int k = 0; k < 3; ++k) cc[k] = (p0[k] + p1[k] + p2[k]) / 3;
                    if (!fl) for (int k = 0; k < 3; ++k) wn[k] = n[k] / L;
                }
            }
            float h = 0;
            check(bestF > 0 && rt.floorAt(fc[0], fc[2], fc[1] + 0.05f, 0.1f, 0.2f, h) && std::fabs(h - fc[1]) < 0.02f, "Frame Home stands on its flat part", f3(fc));
            if (bestW > 0) {
                const float hl = std::sqrt(wn[0] * wn[0] + wn[2] * wn[2]);
                const float nx = wn[0] / hl, nz = wn[2] / hl;
                float x = wc[0] + nx * 0.5f, z = wc[2] + nz * 0.5f;
                const float x0 = x, z0 = z;
                for (int k = 0; k < 20; ++k) rt.move(x, z, wc[1] - 0.935f, -nx * 0.05f, -nz * 0.05f);   // frame-sized steps, as walking does
                const float went = (x0 - x) * nx + (z0 - z) * nz;
                check(went < 0.4f, "...and its too-steep part stops you", "moved " + std::to_string(went) + " m of 1 m towards it");
            } else check(false, "...and its too-steep part stops you", "no upright face");
            screenshot("09_wall_floor");
            return 1;
        });
        // A right click on the object (no drag) opens its menu.
        stages.push_back([this] {
            float mn[3], mx[3];
            a.worldBounds(a.objects[obj], mn, mx);
            const float c[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
            ImVec2 sp;
            if (!a.screenOf(c, sp)) { check(false, "right-click opens the object menu", "object off screen"); return 0; }
            moveTo(sp); moveTo(sp); pressB(1, true); queue.push_back(In{}); pressB(1, false); queue.push_back(In{});
            return 4;
        });
        stages.push_back([this] {
            check(a.objMenuOpen && a.menuObject >= 0 && a.selected == a.menuObject, "right-clicking an object opens its menu (Auto / Surface / Walk through)",
                  a.menuObject >= 0 ? a.objects[a.menuObject].label : "");
            screenshot("10_object_menu");
            key(ImGuiKey_Escape); queue.push_back(In{});
            return 3;
        });
        // Turning it 90 deg: its surface turns with it (still one, every triangle), and one Ctrl+Z undoes the turn.
        stages.push_back([this] {
            a.selected = obj;
            Object& o = a.objects[obj];
            std::memcpy(before, o.pivot, 64);
            a.pushUndo(obj);
            float rx[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1}, t[16], ti[16], m[16];
            translation(t, o.pivot[12], o.pivot[13], o.pivot[14]);
            translation(ti, -o.pivot[12], -o.pivot[13], -o.pivot[14]);
            mul(rx, ti, m); mul(t, m, m); mul(m, o.pivot, o.pivot);
            a.apply(o);
            a.transformDone(obj);
            const std::vector<int> parts = a.surfaceParts(a.objects[obj]);
            float w[16];
            mul(o.pivot, o.offset, w);
            float d = 0;
            if (parts.size() == 1) for (int k = 0; k < 16; ++k) d += std::fabs(a.scene->world.colliders[parts[0]].world[k] - w[k]);
            check(parts.size() == 1 && d < 1e-4f, "turning it on its side turns its surface with it (still one)");
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            check(std::memcmp(a.objects[obj].pivot, before, 64) == 0 && a.surfaceParts(a.objects[obj]).size() == 1, "one Ctrl+Z undoes the turn");
            a.deleteSelected();
            check(a.surfaceParts(a.objects[obj]).empty(), "deleting a Surface object removes its surface");
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            check(!a.objects[obj].deleted && a.surfaceParts(a.objects[obj]).size() == 1, "Ctrl+Z brings the object and its surface back");
            return 1;
        });

        // ── Draw Floor: fill its flat faces (walkable even on furniture), the brush erases, one Ctrl+Z each
        stages.push_back([this] {
            // Its own object: furniture-like, inside the walkable area's reach (a home's invisible walls keep the
            // area to its play space; one beyond them is never stood on), else the one edited above.
            a.rebakeWalk();
            drawObj = obj;
            float best = 1e30f;
            for (size_t i = 0; i < a.objects.size() && a.walkShown; ++i) {
                const Object& o = a.objects[i];
                if (!a.canBeSurface(o) || o.hidden || o.category == kPlants) continue;
                float mn[3], mx[3];
                a.worldBounds(o, mn, mx);
                const float d = dist3(mn, mx);
                int cx, cz;
                if (d <= 0.4f || std::fabs(d - 1.5f) >= best || !a.walkShown->cellOf((mn[0] + mx[0]) * 0.5f, (mn[2] + mx[2]) * 0.5f, cx, cz)) continue;
                std::vector<u32> fl, wl;
                a.classifyObject(o, fl, wl);
                if (fl.size() < 4) continue;
                best = std::fabs(d - 1.5f);
                drawObj = static_cast<int>(i);
            }
            const size_t partsBefore = a.surfaceParts(a.objects[drawObj]).size();
            a.fillDrawnFloor(static_cast<size_t>(drawObj), true);
            const int f = a.drawnFloorIndex(a.objects[drawObj]);
            check(f >= 0 && a.scene->world.colliders[f].role == WorldCollider::Walkable && a.surfaceParts(a.objects[drawObj]).size() == partsBefore,
                  "Fill Flat Faces draws a floor on it (its own part; the surface stays)", f >= 0 ? std::to_string(a.scene->world.colliders[f].indices.size() / 3) + " triangles" : "none");
            // its drawn faces: walkable (where there's headroom over them) once the area is baked
            a.rebakeWalk();
            int faces = 0, walk = 0;
            float at[3] = {};
            if (f >= 0 && a.walkShown) {
                const WorldCollider& c = a.scene->world.colliders[f];
                const std::vector<float> wp = worldMeshPositions(c);
                for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                    const float* p0 = &wp[c.indices[t] * 3]; const float* p1 = &wp[c.indices[t + 1] * 3]; const float* p2 = &wp[c.indices[t + 2] * 3];
                    const float m[3] = {(p0[0] + p1[0] + p2[0]) / 3, (p0[1] + p1[1] + p2[1]) / 3, (p0[2] + p1[2] + p2[2]) / 3};
                    float h = 0;
                    ++faces;
                    if (a.walkShown->floorAt(m[0], m[2], m[1] + 0.05f, 0.1f, 0.1f, h)) { if (!walk++) std::copy_n(m, 3, at); }
                    else if (!faces || at[1] == 0) std::copy_n(m, 3, at);
                }
            }
            check(walk > 0, "...and you can stand on it", std::to_string(walk) + " of " + std::to_string(faces) + " faces, e.g. " + f3(at));
            std::copy_n(at, 3, wallAt);
            return 2;
        });
        stages.push_back([this] {
            a.pushWorldUndo();
            a.paintFloor(static_cast<size_t>(drawObj), wallAt, 100.0f, true, -1);   // a brush bigger than it: all erased
            check(a.drawnFloorIndex(a.objects[drawObj]) < 0, "the brush with Shift erases it");
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            check(a.drawnFloorIndex(a.objects[drawObj]) >= 0, "Ctrl+Z brings the drawn floor back");
            a.pushWorldUndo();
            a.paintFloor(static_cast<size_t>(drawObj), wallAt, 0.2f, false, -1);
            a.fillDrawnFloor(static_cast<size_t>(drawObj), false);
            check(a.drawnFloorIndex(a.objects[drawObj]) < 0, "Clear takes it away");
            a.rebakeWalk();
            return 2;
        });

        // ── World > Auto-generate Walkable Area: one step, a summary, saved for Frame Home, a re-run replaces it
        stages.push_back([this] {
            collidersBefore = a.scene->world.colliders.size();
            spawnsBefore = a.scene->world.spawns.size();
            check(!a.walkSaved() && a.walkShown && a.walkShown->any(), "before any edit, the viewport previews the area Frame Home would bake",
                  a.walkShown ? std::to_string(static_cast<int>(a.walkReport.walkArea)) + " m2" : "none");
            a.selected = -1; a.wKind = a.wIndex = -1;
            a.frameAll();
            a.autoGenerate();
            return 6;
        });
        stages.push_back([this] {
            const auto& r = a.autoReport;
            int autos = 0;
            for (const WorldCollider& c : a.scene->world.colliders)   // the meshes' surfaces it made: Solid, one per mesh
                autos += Studio::isAuto(c.name) && !c.mesh.empty() && (c.role == WorldCollider::Solid || c.role == WorldCollider::Object) && c.kind == WorldCollider::Mesh;
            char d[240];
            std::snprintf(d, sizeof d, "%.1f m2 in %d areas, %d out of reach (%.0f m2), %.1f m2 bridged, %zu blockers, %d ledge cells, %.0f ms, spawn %s",
                          r.walk.walkArea, r.walk.areas, r.walk.unreached, r.walk.unreachedArea, r.walk.bridgedArea, r.walk.blockers.size(), r.walk.ledgeCells, r.ms,
                          r.spawnAdded ? "added" : "kept");
            check(r.ok && r.walk.walkArea > 1 && a.walkSaved() && autos == r.objects + r.colliders && r.objects > 0 && r.colliders > 0, "Auto-generate bakes the walkable area and saves it with the home", d);
            check(a.surfaceMode(a.objects[obj]) == kSurfSolid && !a.autoMade(a.objects[obj]), "...and keeps the surface set by hand");
            {   // the world items: walls (Wall-role boxes), objects' collision (meshes) and wall spots; no seats or hotspots
                int walls = 0, objs = 0, seats = 0, spots = 0, hots = 0;
                for (const WorldCollider& c : a.scene->world.colliders) {
                    walls += c.name == "Auto wall" && c.role == WorldCollider::Wall && c.kind == WorldCollider::Box;
                    objs += (c.name.rfind("Auto surface", 0) == 0 && c.role == WorldCollider::Solid) || (c.name.rfind("Auto collider", 0) == 0 && c.role == WorldCollider::Object);
                }
                for (const WorldPoint& p : a.scene->world.seats) seats += p.name == "Auto seat";
                for (const WorldWallSpot& p : a.scene->world.wallSpots) spots += p.name == "Auto wall spot";
                for (const WorldPoint& p : a.scene->world.hotspots) hots += p.name == "Auto hotspot";
                check(walls == 0 && r.walls == 0 && objs == r.objects + r.colliders && r.objects > 0 && spots == 0 && seats == 0 && hots == 0,
                      "...and makes floor meshes surfaces and the other objects colliders (no walls, wall spots, seats or hotspots)",
                      std::to_string(r.objects) + " surfaces, " + std::to_string(r.colliders) + " colliders, " + std::to_string(spots) + " wall spots, " + std::to_string(seats) +
                          " seats, " + std::to_string(hots) + " hotspots");
                genCounts = {walls, objs, spots, seats + hots};   // (objs: surfaces + colliders)
                collidersAfter = a.scene->world.colliders.size();
            }
            firstGrid = a.scene->world.walkGrid;
            // Frame Home's side: world.json as written, read back, walked on
            HomeWorld back;
            const bool read = worldFromJson(worldToJson(a.scene->world), back);
            WorldRuntime rt;
            rt.build(back);
            const WorldPoint sp = back.spawns.empty() ? WorldPoint{} : back.spawns[0];
            float h = 0;
            check(read && rt.grid() && !back.spawns.empty() && rt.floorAt(sp.pos[0], sp.pos[2], sp.pos[1] + 0.3f, 0.45f, 1.2f, h),
                  "Frame Home reads the saved area from world.json and finds floor at the spawn", f3(sp.pos) + ", " + std::to_string(a.scene->world.walkGrid.size()) + " bytes");
            check(a.autoDoneAt.x >= 0, "the summary shows");
            screenshot("11_walk_summary");
            if (a.autoDoneAt.x >= 0) click(a.autoDoneAt);
            return 4;
        });
        stages.push_back([this] { check(a.autoDoneAt.x < 0, "Done closes the summary"); return 10; });
        stages.push_back([this] {
            check(a.walkGuideOn && a.walkGuideVerts > 0, "the viewport shows the area (walkable, edges, out of reach)", std::to_string(a.walkGuideVerts) + " vertices");
            screenshot("12_walk_area");
            return 1;
        });
        // A Wall surface always blocks: a small one dropped in the middle of the floor cuts the area there.
        stages.push_back([this] {
            float m[3];
            if (!a.walkShown || !Studio::walkMiddle(*a.walkShown, m)) { check(false, "a surface box placed on the floor blocks there", "no middle"); return 1; }
            std::copy_n(m, 3, wallAt);
            const bool before = a.walkShown->walkable(m[0], m[2], m[1]);
            WorldCollider c;
            c.name = "selftest surface"; c.kind = WorldCollider::Box; c.role = WorldCollider::Solid;
            c.half[0] = c.half[2] = 0.3f; c.half[1] = 0.6f;            // 1.2 m high: its sides are too steep and too tall to step up
            translation(c.world, m[0], m[1] + 0.6f, m[2]);
            a.scene->world.colliders.push_back(c);
            a.rebakeWalk();
            const bool after = a.walkShown->walkable(m[0], m[2], m[1]), beside = a.walkShown->walkable(m[0] + 0.9f, m[2], m[1]) || a.walkShown->walkable(m[0] - 0.9f, m[2], m[1]);
            check(before && !after && beside, "a 1.2 m surface box blocks where it stands (only there)", f3(m));
            a.scene->world.colliders.pop_back();
            a.rebakeWalk();
            check(a.walkShown->walkable(m[0], m[2], m[1]) && a.scene->world.walkGrid == firstGrid, "...and taking it away gives the same area back");
            return 2;
        });
        stages.push_back([this] {
            a.autoGenerate();
            return 6;
        });
        stages.push_back([this] {
            int autoSpawns = 0;
            for (const WorldPoint& p : a.scene->world.spawns) autoSpawns += p.name == "Auto spawn";
            const auto& r = a.autoReport;
            check(r.ok && a.scene->world.walkGrid == firstGrid && autoSpawns <= 1 && a.scene->world.colliders.size() == collidersAfter &&
                  std::array<int, 4>{r.walls, r.objects + r.colliders, r.spots, r.seats + r.hotspots} == genCounts,
                  "re-running gives the same area and items (no duplicate spawns, surfaces, walls or objects)", std::to_string(a.scene->world.spawns.size()) + " spawns");
            if (a.autoUndoAt.x >= 0) click(a.autoUndoAt);   // the summary's Undo: just this run
            return 4;
        });
        stages.push_back([this] {
            check(a.walkSaved() && a.scene->world.walkGrid == firstGrid && a.autoDoneAt.x < 0, "the summary's Undo takes back the re-run alone");
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            check(!a.walkSaved() && a.scene->world.colliders.size() == collidersBefore && a.scene->world.spawns.size() == spawnsBefore,
                  "one Ctrl+Z removes the saved area (and its spawn)", std::to_string(a.scene->world.colliders.size()) + " surfaces, " + std::to_string(collidersBefore) + " before");
            return 15;
        });
        stages.push_back([this] {
            check(a.walkShown && !a.walkSaved(), "...and the viewport goes back to the preview");
            return 1;
        });

        // ── World items: surface boxes, moved / scaled / rotated with the gizmo
        stages.push_back([this] {
            a.selected = obj;
            a.frameSelected();
            a.cam.dist *= 2.0f;
            a.addWorld(kCollider);
            check(a.wKind == kCollider && a.scene->world.colliders[a.wIndex].role == WorldCollider::Solid, "World > Add > Surface selects it");
            return 10;
        });
        gizmoDrag("surface: move", Tool::Move, 3, ImVec2(50, 25));
        stages.push_back([this] {
            float m[16]; current(m);
            check(dist3(&m[12], &before[12]) > 0.02f, "surface moves with the gizmo", f3(&before[12]) + " -> " + f3(&m[12]));
            return 2;
        });
        stages.push_back([this] {
            if (a.wKind == kCollider) a.scene->world.colliders[a.wIndex].world[14] += 60;   // the first one out of the way (else they'd become one)
            a.addWorld(kCollider);
            check(a.wKind == kCollider && a.scene->world.colliders[a.wIndex].role == WorldCollider::Solid, "World > Add > Surface makes a surface box");
            std::memcpy(before, a.scene->world.colliders[a.wIndex].half, 12);
            return 6;
        });
        gizmoDrag("wall: scale X", Tool::Scale, 0, ImVec2(70, 0));
        stages.push_back([this] {
            const WorldCollider& c = a.scene->world.colliders[a.wIndex];
            check(std::fabs(c.half[0] - 1.0f) > 0.05f && std::fabs(colLen(c.world, 0) - 1) < 1e-3f, "wall resizes with the scale gizmo (frame stays unscaled)",
                  "half x " + std::to_string(c.half[0]) + ", frame scale " + std::to_string(colLen(c.world, 0)));
            return 2;
        });
        gizmoDrag("wall: rotate", Tool::Rotate, 4, ImVec2(40, 60));
        stages.push_back([this] {
            float m[16]; current(m);
            float rot = 0;
            for (int k = 0; k < 11; ++k) if (k % 4 != 3) rot += std::fabs(m[k] - before[k]);
            check(rot > 0.05f, "wall rotates with the gizmo", "change " + std::to_string(rot));
            return 2;
        });
        gizmoDrag("wall: move", Tool::Move, 3, ImVec2(-40, -20));
        stages.push_back([this] {
            float m[16]; current(m);
            check(dist3(&m[12], &before[12]) > 0.02f, "wall moves with the gizmo");
            screenshot("04_world_items");
            return 2;
        });
        stages.push_back([this] {
            a.addWorld(kSpawn);
            return 6;
        });
        gizmoDrag("spawn: move", Tool::Move, 3, ImVec2(40, 20));
        stages.push_back([this] {
            float m[16]; current(m);
            check(dist3(&m[12], &before[12]) > 0.02f, "spawn point moves with the gizmo");
            return 2;
        });
        // A spawn turns about the vertical: its ring is a flat circle, edge-on from a level camera. Look down on it.
        stages.push_back([this] { a.cam.pitch = std::min(a.cam.pitch, -0.7f); return 3; });
        gizmoDrag("spawn: turn", Tool::Rotate, 4, ImVec2(40, 60));
        stages.push_back([this] {
            const WorldPoint* p = a.worldPoint(a.wKind, a.wIndex);
            float m[16]; current(m);
            check(p && std::fabs(m[0] - before[0]) + std::fabs(m[8] - before[8]) > 0.02f, "spawn point turns with the gizmo");
            return 2;
        });

        // ── Placed surfaces that overlap become one (walls crossing in a T); one Ctrl+Z splits them again
        stages.push_back([this] {
            auto& cs = a.scene->world.colliders;
            const size_t n0 = cs.size();
            float m[3] = {0, 0, 0};
            if (a.walkShown) Studio::walkMiddle(*a.walkShown, m);
            auto wall = [&](float x, float z, float hx, float hz) {
                WorldCollider c;
                c.name = "selftest wall"; c.kind = WorldCollider::Box; c.role = WorldCollider::Solid; c.placed = true;
                c.half[0] = hx; c.half[1] = 1.25f; c.half[2] = hz;
                translation(c.world, m[0] + x, m[1] + 1.25f, m[2] + z);
                return c;
            };
            a.pushWorldUndo();
            cs.push_back(wall(0, 0, 2, 0.1f));
            cs.push_back(wall(1, 1, 0.1f, 2));
            const int at = a.combineTouching(static_cast<int>(cs.size()) - 1);
            const WorldCollider* c = at >= 0 && at < static_cast<int>(cs.size()) ? &cs[at] : nullptr;
            const std::vector<float> tris = c ? colliderTriangles(*c) : std::vector<float>();
            const float vol = surface_detail::volume(tris);
            check(cs.size() == n0 + 1 && c && c->kind == WorldCollider::Mesh && c->boxes.size() == 38 && std::fabs(vol - 3.9f) < 0.01f,
                  "two placed walls crossing become one surface (their union)", std::to_string(vol) + " m3, " + std::to_string(tris.size() / 9) + " triangles");
            cs.push_back(wall(20, 0, 0.5f, 0.5f));
            const size_t n1 = cs.size();
            a.combineTouching(static_cast<int>(cs.size()) - 1);
            check(cs.size() == n1, "...one placed apart from them stays its own");
            cs.pop_back();
            key(ImGuiKey_Z, true); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            int walls = 0;
            for (const WorldCollider& c : a.scene->world.colliders) walls += c.name == "selftest wall";
            check(walls == 0, "Ctrl+Z takes them away");
            return 1;
        });

        // ── What is shown
        stages.push_back([this] {
            const int walk = static_cast<int>(a.scene->world.colliders.size()) - 2;   // the walkable area added above
            a.showCat[kShowWalls] = false;
            const bool hid = !a.worldShown(kCollider, walk);
            a.showCat[kShowWalls] = true;
            const bool back = a.worldShown(kCollider, walk);
            check(hid && back, "hiding Surface shapes hides surface boxes, showing brings them back");
            a.scene->world.colliders[walk].hidden = true;
            const bool itemHid = !a.worldShown(kCollider, walk);
            a.scene->world.colliders[walk].hidden = false;
            check(itemHid, "an item's eye hides just that item");
            a.wKind = a.wIndex = -1;
            a.selected = obj;
            key(ImGuiKey_I); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            int shownOthers = 0;
            for (size_t i = 0; i < a.objects.size(); ++i) if (static_cast<int>(i) != obj && a.objectShown(a.objects[i])) ++shownOthers;
            check(a.isolating && a.objectShown(a.objects[obj]) && shownOthers == 0, "I shows only the selected object", std::to_string(shownOthers) + " others still shown");
            screenshot("05_only_selected");
            key(ImGuiKey_Escape); queue.push_back(In{});
            return 3;
        });
        stages.push_back([this] {
            int shown = 0;
            for (const Object& o : a.objects) shown += a.objectShown(o);
            check(!a.isolating && shown > 1, "Esc shows everything again", std::to_string(shown) + " objects shown");
            a.showMeshes = false; a.refreshVisibility();
            int s2 = 0;
            for (const Object& o : a.objects) s2 += a.objectShown(o);
            a.showMeshes = true; a.refreshVisibility();
            check(s2 == 0, "View > The Home's Meshes hides them all (world layer alone)");
            return 2;
        });
        // ── The world layer survives a save / load round trip (JSON and the baked .fenv form)
        stages.push_back([this] {
            const std::string j = worldToJson(a.scene->world);
            HomeWorld w2;
            const bool ok = worldFromJson(j, w2);
            size_t same = 0, surfaces = 0, linked = 0, linkedNow = 0;
            for (size_t i = 0; i < w2.colliders.size() && i < a.scene->world.colliders.size(); ++i) same += w2.colliders[i].role == a.scene->world.colliders[i].role;
            for (const WorldCollider& c : w2.colliders) { surfaces += c.role == WorldCollider::Solid; linked += !c.mesh.empty(); }
            for (const WorldCollider& c : a.scene->world.colliders) linkedNow += !c.mesh.empty();
            check(ok && w2.colliders.size() == a.scene->world.colliders.size() && same == w2.colliders.size() && surfaces >= 2 && linked == linkedNow && linked > 0,
                  "world.json keeps roles and links", std::to_string(surfaces) + " surfaces, " + std::to_string(linked) + " linked");
            return 1;
        });
        // ── Gravity zones
        stages.push_back([this] {
            a.selected = obj;
            a.frameSelected();
            a.cam.dist *= 2.0f;
            a.addGravity(WorldGravity::Direction);
            check(a.wKind == kGravity && a.scene->world.gravity.size() == 1, "World > Add > Gravity Zone adds and selects it");
            return 8;
        });
        gizmoDrag("gravity zone: move", Tool::Move, 3, ImVec2(50, 25));
        stages.push_back([this] {
            float m[16]; current(m);
            check(dist3(&m[12], &before[12]) > 0.02f, "gravity zone moves with the gizmo");
            return 2;
        });
        gizmoDrag("gravity zone: scale X", Tool::Scale, 0, ImVec2(60, 0));
        stages.push_back([this] {
            if (a.scene->world.gravity.empty()) { check(false, "gravity zone resizes with the scale gizmo", "the zone is gone"); return 1; }
            const WorldGravity& g = a.scene->world.gravity[0];
            check(std::fabs(g.half[0] - 5) > 0.1f && std::fabs(colLen(g.world, 0) - 1) < 1e-3f, "gravity zone resizes with the scale gizmo", "half x " + std::to_string(g.half[0]));
            return 2;
        });
        gizmoDrag("gravity zone: rotate (which way is down)", Tool::Rotate, 4, ImVec2(40, 60));
        stages.push_back([this] {
            if (a.scene->world.gravity.empty()) { check(false, "rotating the zone turns its down direction", "the zone is gone"); return 1; }
            const WorldGravity& g = a.scene->world.gravity[0];
            check(std::fabs(g.world[5] - 1) > 0.01f, "rotating the zone turns its down direction", "up y " + std::to_string(g.world[5]));
            a.scene->world.fallLimit = 12;
            const std::string j = worldToJson(a.scene->world);
            HomeWorld w2;
            check(worldFromJson(j, w2) && w2.gravity.size() == 1 && std::fabs(w2.fallLimit - 12) < 1e-3f && w2.gravity[0].kind == WorldGravity::Direction,
                  "world.json keeps gravity zones and the respawn distance");
            screenshot("06_gravity_zone");
            return 1;
        });
        // ── Invisible walls: one placed on walkable floor takes that floor away, and stops Frame Home's walker; one
        // through a drawn floor takes that away too
        stages.push_back([this] {
            a.rebakeWalk();
            if (!a.walkShown || a.scene->world.spawns.empty()) { check(false, "World > Add > Invisible Wall", "no walkable area or spawn"); return 1; }
            const WorldPoint& sp = a.scene->world.spawns[0];
            float h = 0;
            if (!a.walkShown->walkable(sp.pos[0], sp.pos[2], sp.pos[1] + 0.05f, &h)) { check(false, "World > Add > Invisible Wall", "the spawn isn't walkable"); return 1; }
            invisibleWallAt[0] = sp.pos[0]; invisibleWallAt[1] = h; invisibleWallAt[2] = sp.pos[2];
            const size_t before = a.scene->world.colliders.size();
            a.cam.pivot[0] = invisibleWallAt[0]; a.cam.pivot[1] = invisibleWallAt[1] + 1.0f; a.cam.pivot[2] = invisibleWallAt[2];
            a.addInvisibleWall();
            const WorldCollider& w = a.scene->world.colliders.back();
            check(a.scene->world.colliders.size() == before + 1 && Studio::isInvisibleWall(w) && std::fabs(w.world[13] - (h + 1.25f)) < 0.01f,
                  "World > Add > Invisible Wall stands one on the floor", w.name);
            a.rebakeWalk();
            check(!a.walkShown->walkable(invisibleWallAt[0], invisibleWallAt[2], invisibleWallAt[1] + 0.05f), "...and where it stands is no longer walkable");
            // Frame Home's walker, a metre in front of it, walking straight at it.
            const float nx = w.world[8], nz = w.world[10];
            for (int grid = 1; grid >= 0; --grid) {
                WorldRuntime rt;
                if (grid) a.walkRuntime(rt);
                else { HomeWorld hw = a.scene->world; hw.walkGrid.clear(); hw.walkGridCache.reset(); rt.build(hw); }   // (no area: its shapes)
                float x = invisibleWallAt[0] - nx * 1.0f, z = invisibleWallAt[2] - nz * 1.0f;
                for (int k = 0; k < 40; ++k) rt.move(x, z, invisibleWallAt[1], nx * 0.05f, nz * 0.05f);
                const float past = (x - invisibleWallAt[0]) * nx + (z - invisibleWallAt[2]) * nz;   // > 0: through it
                check(past < -0.04f, grid ? "Frame Home's walker can't go through it" : "...nor without a walkable area (its shape alone)",
                      "stopped " + std::to_string(-past) + " m before it");
            }
            a.step(a.undo, a.redo);
            check(a.scene->world.colliders.size() == before, "Ctrl+Z takes the wall away");
            // A drawn floor there instead, then a wall through that.
            WorldCollider f;
            f.name = "selftest drawn floor";
            f.kind = WorldCollider::Box;
            f.role = WorldCollider::Walkable;
            f.half[0] = f.half[2] = 1.0f; f.half[1] = 0.02f;
            translation(f.world, invisibleWallAt[0], invisibleWallAt[1] + 0.02f, invisibleWallAt[2]);
            a.scene->world.colliders.push_back(f);
            a.rebakeWalk();
            const bool drawnWalkable = a.walkShown->walkable(invisibleWallAt[0], invisibleWallAt[2], invisibleWallAt[1] + 0.1f);
            a.addInvisibleWall();
            a.rebakeWalk();
            check(drawnWalkable && !a.walkShown->walkable(invisibleWallAt[0], invisibleWallAt[2], invisibleWallAt[1] + 0.1f),
                  "a wall through a drawn floor takes that floor away", drawnWalkable ? "" : "the drawn floor wasn't walkable to begin with");
            screenshot("07_invisible_wall");
            a.step(a.undo, a.redo);
            auto& cs = a.scene->world.colliders;
            cs.erase(std::remove_if(cs.begin(), cs.end(), [](const WorldCollider& c) { return c.name == "selftest drawn floor"; }), cs.end());
            check(cs.size() == before, "...and the one through the drawn floor");
            a.rebakeWalk();
            return 1;
        });
        // ── A background sound: added, undone, saved with the edits, back on reopening, packed in a .fenv
        stages.push_back([this] {
            std::error_code ec;
            const fs::path dir = dataDir() / L"selftest";
            fs::create_directories(dir, ec);
            soundWav = dir / L"selftest tone.wav";
            {   // 2 s of a stereo chord, 16-bit 44.1 kHz
                const int sr = 44100, n = sr * 2;
                std::vector<int16_t> pcm(static_cast<size_t>(n) * 2);
                for (int i = 0; i < n; ++i) {
                    pcm[i * 2] = static_cast<int16_t>(9000 * std::sin(2 * kPi * 220 * i / sr));
                    pcm[i * 2 + 1] = static_cast<int16_t>(9000 * std::sin(2 * kPi * 330 * i / sr));
                }
                auto u32 = [](std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
                auto u16 = [](std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
                std::ofstream f(soundWav, std::ios::binary);
                const uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
                f.write("RIFF", 4); u32(f, 36 + data); f.write("WAVEfmt ", 8); u32(f, 16); u16(f, 1); u16(f, 2); u32(f, sr); u32(f, sr * 4); u16(f, 4); u16(f, 16);
                f.write("data", 4); u32(f, data); f.write(reinterpret_cast<const char*>(pcm.data()), data);
            }
            // the user's own sound for this home (if any) is put back as found
            const std::wstring prefix = widen(frameName(a.homePaths.front()) + ".sound-");
            for (const auto& e : fs::directory_iterator(a.editsFile().parent_path(), ec))
                if (e.path().filename().wstring().rfind(prefix, 0) == 0) {
                    std::ifstream in(e.path(), std::ios::binary);
                    saved.push_back({e.path(), true, std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>())});
                }
            a.addSoundFile(narrow(soundWav.wstring()));
            check(a.sound.bytes && a.sound.name == "selftest tone.wav" && a.sound.s.file.find(".sound-") != std::string::npos && a.sound.s.file.size() > 4 &&
                  a.sound.s.file.compare(a.sound.s.file.size() - 4, 4, ".wav") == 0 && a.dirty, "a background sound can be added", a.sound.s.file);
            if (a.addedSound.ok) check(true, "...and plays in the preview");
            else log("  (the background sound isn't playing here: no audio device)");
            return 3;
        });
        stages.push_back([this] {
            const SoundState before = a.sound;
            a.pushSoundUndo(before);
            a.sound.s.volume = 0.3f;
            a.sound.s.homeVolume = 0;
            a.step(a.undo, a.redo);
            check(std::fabs(a.sound.s.volume - 0.8f) < 1e-4f && a.sound.s.homeVolume == 1 && a.sound.bytes == before.bytes, "Ctrl+Z puts the volumes back");
            a.pushSoundUndo(a.sound);
            a.sound = SoundState{HomeSound{}, nullptr, ""};
            a.addedSound.stop();
            a.step(a.undo, a.redo);
            check(a.sound.bytes == before.bytes && a.sound.name == before.name, "Ctrl+Z brings a removed sound back");
            a.sound.s.volume = 0.6f;
            a.sound.s.homeVolume = 0.25f;
            check(a.saveEdits(), "Save Edits saves it");   // (Ctrl+S: into the library's .fenv, in the background)
            return 2;
        });
        stages.push_back([this] {
            if (a.saving) return -1;
            // The saved environment's edits, as Frame Home and a reopen read them.
            FenvManifest m;
            std::vector<std::string> sources, models;
            std::string edits, err;
            const fs::path cache = dataDir() / L"selftest" / L"fenv-cache-save";
            const bool ok = !a.envFile.empty() && fenvExtract(a.envFile, narrow(cache.wstring()), m, sources, models, edits, err);
            HomeEdits e;
            const bool read = ok && loadHomeEdits(edits, e);
            std::error_code ec;
            const fs::path file = fs::path(widen(read ? soundPathFor(edits, e.sound) : std::string()));
            check(read && e.sound.name == "selftest tone.wav" && std::fabs(e.sound.volume - 0.6f) < 1e-4f &&
                  std::fabs(e.sound.homeVolume - 0.25f) < 1e-4f && a.sound.bytes && fs::file_size(file, ec) == a.sound.bytes->size(),
                  "...in the saved environment, with the file beside its edits", ok ? narrow(file.filename().wstring()) : err);
            a.sound = SoundState();
            a.loadSound(e.sound, edits);
            check(a.sound.bytes && a.sound.bytes->size() == fs::file_size(soundWav, ec) && a.sound.name == "selftest tone.wav" && std::fabs(a.sound.s.volume - 0.6f) < 1e-4f,
                  "reopening the home brings it back");
            soundFenv = dataDir() / L"selftest" / L"selftest-sound.fenv";
            a.saveEnvironment(narrow(soundFenv.wstring()), false);
            return 2;
        });
        stages.push_back([this] {
            if (a.saving) return -1;
            FenvManifest m;
            std::vector<std::string> sources, models;
            std::string edits, err;
            const fs::path cache = dataDir() / L"selftest" / L"fenv-cache";
            const bool ok = fenvExtract(narrow(soundFenv.wstring()), narrow(cache.wstring()), m, sources, models, edits, err);
            HomeEdits e;
            const bool read = ok && loadHomeEdits(edits, e);
            const std::string snd = read ? soundPathFor(edits, e.sound) : std::string();
            std::vector<uint8_t> bytes;
            {   // (closed before the folder is removed: Windows keeps an open file)
                std::ifstream in(fs::path(widen(snd)), std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            check(read && !snd.empty() && a.sound.bytes && bytes == *a.sound.bytes && std::fabs(e.sound.homeVolume - 0.25f) < 1e-4f,
                  "a saved environment (.fenv) carries it as Frame Home reads it", ok ? e.sound.file : err);
            std::error_code ec;
            fs::remove(soundFenv, ec);
            fs::remove_all(cache, ec);
            fs::remove_all(dataDir() / L"selftest" / L"fenv-cache-save", ec);
            fs::remove(soundWav, ec);
            if (!soundSaved.empty()) { bool kept = false; for (const SavedFile& f : saved) kept |= f.path == soundSaved; if (!kept) fs::remove(soundSaved, ec); }
            a.addedSound.stop();
            a.sound = SoundState();
            return 1;
        });
        // ── Bundles: the saved environment packed into a .fbundle, opened again and unpacked
        stages.push_back([this] {
            if (a.saving) return -1;
            bundleFile = dataDir() / L"selftest" / L"selftest.fbundle";
            std::error_code ec;
            fs::remove(bundleFile, ec);
            a.newBundle();
            libraryBefore = a.libraryEnvs().size();
            a.addToBundle({a.envFile, a.envFile});
            check(a.bundleMembers.size() == 1 && a.bundleDirty && a.inspectorTab == Studio::InspectorTab::Bundle,
                  "Bundle \u203a Add the Open Environment lists it once", std::to_string(a.bundleMembers.size()) + " listed");
            a.bundleName = "Self-test bundle";
            return 3;   // the tab draws it, reading its picture
        });
        stages.push_back([this] {
            check(a.bundleMembers.size() == 1 && a.bundleMembers[0].thumbAspect > 0, "...with its picture");
            screenshot("09_bundle");
            a.saveBundle(narrow(bundleFile.wstring()));
            return 2;
        });
        stages.push_back([this] {
            if (a.bundleBusy || a.bundler.joinable()) return -1;
            std::string err;
            check(a.bundleFile == narrow(bundleFile.wstring()) && !a.bundleDirty && fenvBundleValidate(a.bundleFile, err), "Save Bundle writes a valid .fbundle",
                  err.empty() ? narrow(bundleFile.filename().wstring()) : err);
            a.openBundle(narrow(bundleFile.wstring()));
            check(a.bundleMembers.size() == 1 && !a.bundleMembers[0].member.empty() && a.bundleName == "Self-test bundle", "Open Bundle lists what's in it",
                  a.bundleMembers.empty() ? "" : a.bundleMembers[0].member);
            a.unpackBundle();
            return 2;
        });
        stages.push_back([this] {
            if (a.bundleBusy || a.bundler.joinable()) return -1;
            std::string st;
            { std::lock_guard<std::mutex> l(a.statusMutex); st = a.bundleStatus; }
            check(st.rfind("Unpacked into My Environments (1 ", 0) == 0 && a.libraryEnvs().size() == libraryBefore, "Unpack puts it in My Environments (the same file isn't copied twice)", st);
            a.bundleDirty = false;
            a.newBundle();
            std::error_code ec;
            fs::remove(bundleFile, ec);
            return 2;
        });
        // ── Edit > Settings: this PC's options, and the headset's read over ssh (nothing is written there)
        stages.push_back([this] { a.showSettings = true; a.headsetSettings(false); return 5; });
        stages.push_back([this] {
            if (a.headsetBusy) return -1;
            std::string status;
            bool loaded;
            { std::lock_guard<std::mutex> l(a.statusMutex); status = a.headsetStatus; loaded = a.headsetLoaded; }
            check(loaded, "Settings reads the headset's settings", status);
            return 10;
        });
        stages.push_back([this] { screenshot("08_settings"); a.showSettings = false; return 2; });
        stages.push_back([this] {
            check(idConflictFrames == 0, "no ImGui ID conflicts anywhere during the test", idConflictWhere);
            log("done: %d passed, %d failed", passes, fails);
            finish();
            return 0;
        });
    }

    struct SavedFile { fs::path path; bool existed; std::string bytes; };
    std::vector<SavedFile> saved;

    fs::path soundWav, soundSaved, soundFenv;
    fs::path imageFenv;
    fs::path bundleFile;
    int drawObj = -1;                                  // the Draw Floor test's object
    ImGuiID outlinerDockWas = 0;                       // the area-dragging test: where the Outliner was
    float invisibleWallAt[3] = {};                     // the invisible-wall test's spot (on the spawn's floor)
    size_t libraryBefore = 0;                          // My Environments' count before the bundle is unpacked into it
    std::string titleWas, imageWas;
    std::array<int, 4> genCounts{};                    // Auto-generate's walls, objects, wall spots, seats + hotspots (first run)
    size_t collidersAfter = 0;
    size_t linkedBefore = 0;                           // mesh-linked surfaces before Make Walkable Floors (the home links its own)          // the background-sound test's files (removed when it ends)

    void finish() {
        for (const SavedFile& f : saved) {
            std::error_code ec;
            if (f.existed) std::ofstream(f.path, std::ios::binary) << f.bytes;
            else fs::remove(f.path, ec);
        }
        if (report) { std::fclose(report); report = nullptr; }
        a.dirty = false;                                 // nothing of the test is kept
        if (a.window.requestClose) a.window.requestClose();
    }
};
SelfTest* g_test = nullptr;

// ── The window: Vulkan swapchain (ImGui's helper), the frame loop, and the system's window ──────────────────
// runStudio() is the same everywhere; a StudioWindow is the system's window behind it: Win32Window on Windows,
// GlfwWindow on Linux and macOS.

Studio* g_app = nullptr;
FramePacer* g_pacer = nullptr;      // the frame loop's, while it runs (the window reports input and changes to it)
ImGui_ImplVulkanH_Window g_wd;
bool g_rebuild = false;
uint32_t g_resizeW = 0, g_resizeH = 0;   // a new size for the swapchain, in pixels (0: none pending)

void frameRender(Studio& app, ImDrawData* draw) {
    VkDevice dev = app.renderer.device();
    VkSemaphore acquired = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore done = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].RenderCompleteSemaphore;
    const auto a0 = std::chrono::steady_clock::now();
    VkResult err = vkAcquireNextImageKHR(dev, g_wd.Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &g_wd.FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    ImGui_ImplVulkanH_Frame* fd = &g_wd.Frames[g_wd.FrameIndex];
    const auto a1 = std::chrono::steady_clock::now();
    vkWaitForFences(dev, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    const auto a2 = std::chrono::steady_clock::now();
    const double acqMs = std::chrono::duration<double, std::milli>(a1 - a0).count(), fenMs = std::chrono::duration<double, std::milli>(a2 - a1).count();
    if (acqMs + fenMs > 30.0) std::fprintf(stderr, "[studio] slow UI frame: acquire %.1f ms, UI fence %.1f ms\n", acqMs, fenMs);
    vkResetFences(dev, 1, &fd->Fence);
    vkResetCommandPool(dev, fd->CommandPool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->CommandBuffer, &bi);

    // The 3D frame (left the renderer in TRANSFER_SRC) is sampled by the UI, then handed back.
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = app.renderer.colorImage();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, app.renderer.viewCount};
    if (app.rendered) {
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_wd.RenderPass;
    rp.framebuffer = fd->Framebuffer;
    rp.renderArea.extent.width = g_wd.Width;
    rp.renderArea.extent.height = g_wd.Height;
    rp.clearValueCount = 1;
    rp.pClearValues = &g_wd.ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);
    if (app.rendered) {
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(fd->CommandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(fd->CommandBuffer);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &fd->CommandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    vkQueueSubmit(app.renderer.queue(), 1, &si, fd->Fence);
}

void framePresent(Studio& app) {
    if (g_rebuild) return;
    VkSemaphore done = g_wd.FrameSemaphores[g_wd.SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_wd.Swapchain;
    pi.pImageIndices = &g_wd.FrameIndex;
    VkResult err = vkQueuePresentKHR(app.renderer.queue(), &pi);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) g_rebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR) return;
    g_wd.SemaphoreIndex = (g_wd.SemaphoreIndex + 1) % g_wd.SemaphoreCount;
}

// The window's close box (or File > Quit): false when the user chose to stay (unsaved changes, Cancel).
bool allowClose() {
    if (!g_app) return true;
    if (!g_app->confirmDiscard()) return false;
    if (g_app->installing)
        platform::message("Frame Studio", "Frame Home is still being installed on the headset. Frame Studio will close when the copy finishes.");
    if (g_app->pcBusy)
        platform::message("Frame Studio", "Frame Home is still being installed on this PC. Frame Studio will close when it's done.");
    if (!g_app->confirmBundleDiscard()) return false;
    if (g_app->bundleBusy)
        platform::message("Frame Studio", "A bundle is still being written. Frame Studio will close when it's done.");
    if (g_app->saving)   // closing waits for it (an unfinished .fenv would be lost)
        platform::message("Frame Studio", "An environment is still being saved. Frame Studio will close as soon as it's written.");
    return true;
}

// Files dropped on the window: a sound file becomes the background sound, anything else opens.
void filesDropped(const std::vector<std::string>& files) {
    if (!g_app || files.empty()) return;
    if (files.size() == 1 && isSoundFile(files[0]) && g_app->scene) g_app->addSoundFile(files[0]);
    else g_app->open(files);
}

// The system's window, as runStudio() uses it.
class StudioWindow {
public:
    virtual ~StudioWindow() = default;
    virtual void* nativeHandle() = 0;                    // dialogs' owner (HWND on Windows)
    virtual float uiScale() = 0;                         // the display's scale for the UI (1 = 96 dpi)
    virtual float pixelDensity() { return 1.0f; }        // pixels per UI unit (2 on a Mac's Retina display)
    virtual std::vector<std::string> vulkanInstanceExtensions() = 0;
    virtual VkSurfaceKHR createSurface(VkInstance instance) = 0;
    virtual void pixelSize(int& width, int& height) = 0; // the swapchain's size
    virtual void show() = 0;
    virtual void initImGui() = 0;
    virtual void newImGuiFrame() = 0;
    virtual void shutdownImGui() = 0;
    virtual bool pumpEvents() = 0;                       // handles what arrived; false once the window has closed
    virtual bool minimized() = 0;
    virtual bool focused() = 0;
    virtual void waitForInput(double seconds) = 0;
    virtual double displayFps() = 0;                     // the refresh rate of the display it's on
    virtual Studio::WindowHooks hooks() = 0;
    virtual void startHangWatchdog() {}                  // (Windows: asks to force-close a window hung for 20 s)
    virtual void stopHangWatchdog() {}
};

// The UI fonts: the platform's own UI typeface when it's there (Segoe UI, the SF / Helvetica family, Noto / DejaVu),
// in regular and bold, plus one with the arrows and symbols the explorer uses.
struct FontFiles { std::vector<const char*> regular, bold, symbols; };
FontFiles fontFiles() {
#if defined(_WIN32)
    return {{"C:\\Windows\\Fonts\\segoeui.ttf"}, {"C:\\Windows\\Fonts\\seguisb.ttf"}, {"C:\\Windows\\Fonts\\seguisym.ttf"}};
#elif defined(__APPLE__)
    return {{"/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"},
            {"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"},
            {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf"}};
#else
    // Noto Sans (most desktops), else Liberation Sans (Arial's metrics), else DejaVu Sans (wide, but everywhere). Ubuntu's
    // own fonts ship as variable fonts only, which draw at one weight here.
    return {{"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
             "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"},
            {"/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf", "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
             "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf",
             "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
             "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"},
            {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf"}};
#endif
}
const char* firstExisting(const std::vector<const char*>& files) {
    std::error_code ec;
    for (const char* f : files) if (fs::exists(f, ec)) return f;
    return nullptr;
}

// `density`: pixels per UI unit. Fonts are made at that many pixels and drawn back at UI size, so text stays sharp
// on a Retina display.
void loadFonts(float scale, float density) {
    ImGuiIO& io = ImGui::GetIO();
    scale *= density;
    io.FontGlobalScale = 1.0f / density;
    const FontFiles files = fontFiles();
    const char* font = firstExisting(files.regular);
    const char* bold = firstExisting(files.bold);
    const char* symbols = firstExisting(files.symbols);
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x2190, 0x2193, 0x203A, 0x203A, 0};
    static const ImWchar symbolRanges[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0};
    auto addFont = [&](const char* file, float px) -> ImFont* {
        ImFontConfig fc;
        fc.OversampleH = 2;
        ImFont* f = io.Fonts->AddFontFromFileTTF(file, px, &fc, ranges);
        if (f && symbols) {   // the few glyphs the UI font lacks (the explorer's arrows, the note, the cross), merged in
            ImFontConfig mc;
            mc.MergeMode = true;
            mc.OversampleH = 2;
            mc.GlyphOffset = ImVec2(0, 1.0f * scale);
            io.Fonts->AddFontFromFileTTF(symbols, px, &mc, symbolRanges);
        }
        return f;
    };
    // Inter, as Blender draws its interface, built in (the same on every system); the platform's own UI font only if
    // that fails. The few symbols Inter lacks come from the platform's symbol font, merged in.
    auto addInter = [&](const char* base85, float px) -> ImFont* {
        ImFontConfig fc;
        fc.OversampleH = 2;
        ImFont* f = io.Fonts->AddFontFromMemoryCompressedBase85TTF(base85, px, &fc, ranges);
        if (f && symbols) {
            ImFontConfig mc;
            mc.MergeMode = true;
            mc.OversampleH = 2;
            mc.GlyphOffset = ImVec2(0, 1.0f * scale);
            io.Fonts->AddFontFromFileTTF(symbols, px, &mc, symbolRanges);
        }
        return f;
    };
    if (!addInter(inter_regular_compressed_data_base85, 14.0f * scale) && (!font || !addFont(font, 14.0f * scale))) io.Fonts->AddFontDefault();
    g_fontBold = addInter(inter_semibold_compressed_data_base85, 14.0f * scale);
    g_fontTitle = addInter(inter_semibold_compressed_data_base85, 28.0f * scale);
    if (!g_fontBold && bold) { g_fontBold = addFont(bold, 14.0f * scale); g_fontTitle = addFont(bold, 28.0f * scale); }
}

int fail(const std::string& what) {
    platform::message("Frame Studio", what, platform::Icon::Error);
    return 1;
}

// Everything between the window opening and closing, the same on every platform. `args`: the command line after the
// program's name (homes to open, or --selftest and its homes).
int runStudio(StudioWindow& window, const std::vector<std::string>& args) {
    const fs::path data = dataDir();
    const float scale = window.uiScale();
    platform::setDialogOwner(window.nativeHandle());
    g_selfTestRun = !args.empty() && args[0] == "--selftest";   // (before anything saves)
    auto app = std::make_unique<Studio>();
    g_app = app.get();
    app->window = window.hooks();
    app->scale = scale;
    app->loadRecent();
    app->loadSettings();
    HslRenderer& r = app->renderer;
    r.instanceExtensions = window.vulkanInstanceExtensions();
    r.deviceExtensions = {"VK_KHR_swapchain"};
    r.clearColor[0] = r.clearColor[1] = r.clearColor[2] = 0.0467f;   // Blender's viewport grey (#3d3d3d)
    r.pipelineCachePath = narrow((data / L"pipelines.bin").wstring());   // shaders compile once, then load from here
    r.viewCount = 1;                                                 // the editor shows one view: draw it once, not per eye
    r.msaa = app->studioPerf.msaa;                                   // Edit > Settings
    r.anisotropy = static_cast<float>(app->studioPerf.anisotropy);
    if (!r.init(64, 64)) return fail("Couldn't start Vulkan on this GPU. See " + narrow((data / L"studio.log").wstring()) + ".");
    r.costScaleMs = 0.05f;                     // Debug > GPU Cost: a desktop GPU is several times the Frame's

    VkSurfaceKHR surface = window.createSurface(r.instance());
    if (!surface) return fail("Couldn't create a Vulkan surface.");
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(r.physicalDevice(), r.queueFamily(), surface, &present);
    if (!present) return fail("This GPU's graphics queue can't present to a window.");

    int pixelW = 0, pixelH = 0;
    window.pixelSize(pixelW, pixelH);
    g_wd.Surface = surface;
    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    g_wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(r.physicalDevice(), surface, formats, 2, VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    // MAILBOX (3 images): the newest frame replaces a waiting one, so a late compositor never blocks the queue
    // the 3D renderer shares. FIFO is the fallback every driver has. FS_PRESENT=fifo forces vsync-blocking.
    const char* pm = std::getenv("FS_PRESENT");
    const bool fifo = pm && std::string(pm) == "fifo";
    const VkPresentModeKHR modes[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
    g_wd.PresentMode = fifo ? VK_PRESENT_MODE_FIFO_KHR : ImGui_ImplVulkanH_SelectPresentMode(r.physicalDevice(), surface, modes, 3);
    std::fprintf(stderr, "[studio] present mode %d\n", static_cast<int>(g_wd.PresentMode));
    g_wd.ClearEnable = true;
    g_wd.ClearValue.color = {{0.11f, 0.11f, 0.11f, 1}};
    ImGui_ImplVulkanH_CreateOrResizeWindow(r.instance(), r.physicalDevice(), r.device(), &g_wd, r.queueFamily(), nullptr, pixelW, pixelH, 3);

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool uiPool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(r.device(), &dpci, nullptr, &uiPool);
    VkSamplerCreateInfo smp = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(r.device(), &smp, nullptr, &app->viewSampler);

    window.show();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The areas' layout, kept between runs (the self-test always starts from the default one).
    static std::string ini = narrow((data / L"areas.ini").wstring());
    io.IniFilename = g_selfTestRun ? nullptr : ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;
    if (FILE* f = platform::openFile((data / L"workspace.txt").c_str(), "rb")) {   // the workspace last used
        int w = 0;
        if (std::fscanf(f, "%d", &w) == 1 && w >= 0 && w < Studio::kWorkspaces && !g_selfTestRun) app->workspace = w;
        std::fclose(f);
    }
    applyTheme(scale);
    loadFonts(scale, window.pixelDensity());
    window.initImGui();
    ImGui_ImplVulkan_InitInfo ii = {};
    ii.Instance = r.instance();
    ii.PhysicalDevice = r.physicalDevice();
    ii.Device = r.device();
    ii.QueueFamily = r.queueFamily();
    ii.Queue = r.queue();
    ii.DescriptorPool = uiPool;
    ii.RenderPass = g_wd.RenderPass;
    ii.MinImageCount = 2;
    ii.ImageCount = g_wd.ImageCount;
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplVulkan_Init(&ii);
    applyTheme(scale);   // again: window.initImGui() resets ImGui's style, so the saved theme is applied after it

    if (const char* tab = std::getenv("FS_INSPECTOR_TAB")) {   // diagnostics / screenshots: start on this Inspector tab
        const std::string t = tab;
        app->showInspectorTab(t == "world" ? Studio::InspectorTab::World : t == "environment" ? Studio::InspectorTab::Environment
                              : t == "sound" ? Studio::InspectorTab::Environment : t == "bundle" ? Studio::InspectorTab::Bundle : t == "headset" ? Studio::InspectorTab::Headset
                                                                                            : Studio::InspectorTab::Selection);
    }
    std::unique_ptr<SelfTest> selftest;
    if (args.size() > 1 && args[0] == "--selftest") {
        selftest = std::make_unique<SelfTest>(*app, std::vector<std::string>(args.begin() + 1, args.end()));
        g_test = selftest.get();
    } else if (!args.empty()) {
        app->open(args);
    }

    window.startHangWatchdog();
    FramePacer pacer([&window] { return window.focused(); }, [&window](double seconds) { window.waitForInput(seconds); });
    pacer.setDisplayFps(window.displayFps());
    if (const char* fps = std::getenv("FS_FPS")) pacer.setFpsOverride(std::atof(fps));
    g_pacer = &pacer;
    std::fprintf(stderr, "[studio] display %.0f Hz\n", pacer.displayFps());
    const bool logFrameRate = std::getenv("FS_STATS") != nullptr;

    bool waitingForGpu = false;      // the last pass found the GPU still busy: this one is the same frame, already paced
    for (;;) {
        if (!waitingForGpu) pacer.waitForNextFrame(app->viewChangesByItself() || g_test != nullptr);
        waitingForGpu = false;
        if (!window.pumpEvents()) break;
        if (window.minimized()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if (g_resizeW && g_resizeH) {
            ImGui_ImplVulkan_SetMinImageCount(3);
            ImGui_ImplVulkanH_CreateOrResizeWindow(r.instance(), r.physicalDevice(), r.device(), &g_wd, r.queueFamily(), nullptr,
                                                   static_cast<int>(g_resizeW), static_cast<int>(g_resizeH), 3);   // as at startup
            g_wd.FrameIndex = 0;
            g_wd.SemaphoreIndex = 0;                   // the new swapchain may have fewer semaphores
            g_resizeW = g_resizeH = 0;
            g_rebuild = false;
        } else if (g_rebuild) {
            int w = 0, h = 0;
            window.pixelSize(w, h);
            g_resizeW = static_cast<uint32_t>(w);
            g_resizeH = static_cast<uint32_t>(h);
            continue;
        }

        // Never wait on the GPU here: while it is still busy with the last frame (the system paging in a fresh
        // texture block, say), keep handling input and try again in a moment.
        if (r.gpuBusy()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); waitingForGpu = true; continue; }

        using clk = std::chrono::steady_clock;
        const auto f0 = clk::now();
        app->finishLoad();
        app->updateWalk();
        const auto f1 = clk::now();
        app->renderViewport();
        const auto f2 = clk::now();

        ImGui_ImplVulkan_NewFrame();
        window.newImGuiFrame();
        if (g_test) g_test->inject(ImGui::GetIO());
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        app->ui();
        if (g_test) g_test->afterUi();
        ImGui::Render();
        const auto f3 = clk::now();
        ImDrawData* draw = ImGui::GetDrawData();
        if (draw->DisplaySize.x > 0 && draw->DisplaySize.y > 0) {
            frameRender(*app, draw);
            framePresent(*app);
        }
        const auto f4 = clk::now();
        if (const char* hang = std::getenv("FS_TEST_HANG")) {   // diagnostics: freeze the UI thread once (tests the watchdog)
            static bool hung = false;
            if (!hung) { hung = true; std::this_thread::sleep_for(std::chrono::seconds(std::atoi(hang))); }
        }
        auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (logFrameRate) {   // FS_STATS=1: the frame rate and where a frame's CPU time goes, every 5 s
            static int framesCounted = 0;
            static double viewMsSum = 0, uiMsSum = 0, presentMsSum = 0, gpuMsSum = 0;
            static clk::time_point windowStart = f0;
            ++framesCounted; gpuMsSum += app->renderer.lastGpuMs(); viewMsSum += ms(f1, f2); uiMsSum += ms(f2, f3); presentMsSum += ms(f3, f4);
            if (const double secs = ms(windowStart, f4) / 1000; secs >= 5) {
                std::fprintf(stderr, "[studio] %.1f fps (display %.0f Hz), 3D view %ux%u: 3D %.2f ms (GPU %.2f ms), UI %.2f ms, present %.2f ms a frame\n",
                             framesCounted / secs, pacer.displayFps(), app->viewW, app->viewH, viewMsSum / framesCounted, gpuMsSum / framesCounted,
                             uiMsSum / framesCounted, presentMsSum / framesCounted);
                std::fflush(stderr);
                framesCounted = 0; viewMsSum = uiMsSum = presentMsSum = gpuMsSum = 0; windowStart = f4;
            }
        }
        if (ms(f0, f4) > 40.0)
            std::fprintf(stderr, "[studio] slow frame %.1f ms: load step %.1f, 3D %.1f, UI %.1f, present %.1f\n",
                         ms(f0, f4), ms(f0, f1), ms(f1, f2), ms(f2, f3), ms(f3, f4));
    }

    g_pacer = nullptr;
    vkDeviceWaitIdle(r.device());
    window.stopHangWatchdog();
    if (app->saver.joinable()) app->saver.join();
    if (app->loadState == 1 || app->loadAbandoned) {   // a home still being read: nothing to lose, and waiting for it is what stalls
        std::fflush(stderr);
        std::_Exit(0);
    }
    if (app->loader.joinable()) app->loader.join();
    if (app->sender.joinable()) app->sender.join();
    app->stopTune = true;
    if (app->tuner.joinable()) app->tuner.join();
    if (app->filesReader.joinable()) app->filesReader.join();
    if (app->installer.joinable()) app->installer.join();
    if (app->pcJob.joinable()) app->pcJob.join();
    if (app->bundler.joinable()) app->bundler.join();
    if (app->headsetThread.joinable()) app->headsetThread.join();
    if (app->headsetDebugThread.joinable()) app->headsetDebugThread.join();
    if (app->loadAbandoned) {   // a stopped loader may still be running and would touch the app as it's torn down
        std::fflush(stderr);
        std::_Exit(0);
    }
    ImGui_ImplVulkan_Shutdown();
    window.shutdownImGui();
    ImGui::DestroyContext();
    ImGui_ImplVulkanH_DestroyWindow(r.instance(), r.device(), &g_wd, nullptr);
    vkDestroySampler(r.device(), app->viewSampler, nullptr);
    vkDestroyDescriptorPool(r.device(), uiPool, nullptr);
    r.shutdown();
    g_app = nullptr;
    return 0;
}

#ifdef _WIN32
// ── Windows: a Win32 window ──────────────────────────────────────────────────────────────────────────────────

// The refresh rate of the display a window is on.
double displayFpsOf(HWND hwnd) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof info;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
        DEVMODEW mode = {};
        mode.dmSize = sizeof mode;
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            return static_cast<double>(mode.dmDisplayFrequency);
    }
    return 60.0;
}

LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_pacer) {   // window changes count as input (the picture must follow), and may move it to another display
        if (msg == WM_SIZE || msg == WM_ACTIVATE || msg == WM_DROPFILES || msg == WM_MOUSELEAVE) g_pacer->noteInput();
        if (msg == WM_DISPLAYCHANGE || msg == WM_EXITSIZEMOVE || msg == WM_DPICHANGED) { g_pacer->noteInput(); g_pacer->setDisplayFps(displayFpsOf(hWnd)); }
    }
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {900, 560};
            return 0;
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            std::vector<std::string> files;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                std::wstring file(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');   // long paths: no MAX_PATH cut
                if (DragQueryFileW(drop, i, file.data(), static_cast<UINT>(file.size()))) files.push_back(narrow(file.c_str()));
            }
            DragFinish(drop);
            filesDropped(files);
            return 0;
        }
        case WM_CLOSE:
            if (allowClose()) DestroyWindow(hWnd);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;      // Alt alone must not open the system menu
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

class Win32Window : public StudioWindow {
public:
    explicit Win32Window(HINSTANCE inst) : inst_(inst) {
        ImGui_ImplWin32_EnableDpiAwareness();
        scale_ = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
        wc_ = {sizeof wc_, CS_CLASSDC, wndProc, 0, 0, inst, LoadIconW(nullptr, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */),
               LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */), nullptr, nullptr, L"FrameStudio", nullptr};
        RegisterClassExW(&wc_);
        // 1600 x 960, but never bigger than the screen's free area (so the taskbar can't cover the status bar), centred on it.
        RECT work = {0, 0, static_cast<LONG>(1600 * scale_), static_cast<LONG>(960 * scale_)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int winW = std::min(static_cast<int>(1600 * scale_), static_cast<int>(work.right - work.left));
        const int winH = std::min(static_cast<int>(960 * scale_), static_cast<int>(work.bottom - work.top));
        hwnd_ = CreateWindowW(wc_.lpszClassName, L"Frame Studio", WS_OVERLAPPEDWINDOW,
                              work.left + (work.right - work.left - winW) / 2, work.top + (work.bottom - work.top - winH) / 2,
                              winW, winH, nullptr, nullptr, inst, nullptr);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        DragAcceptFiles(hwnd_, TRUE);
    }
    ~Win32Window() override {
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        UnregisterClassW(wc_.lpszClassName, wc_.hInstance);
    }

    void* nativeHandle() override { return hwnd_; }
    float uiScale() override { return scale_; }
    std::vector<std::string> vulkanInstanceExtensions() override { return {"VK_KHR_surface", "VK_KHR_win32_surface"}; }
    VkSurfaceKHR createSurface(VkInstance instance) override {
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = inst_;
        sci.hwnd = hwnd_;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        return vkCreateWin32SurfaceKHR(instance, &sci, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
    }
    void pixelSize(int& width, int& height) override {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        width = rc.right - rc.left;
        height = rc.bottom - rc.top;
    }
    void show() override { ShowWindow(hwnd_, SW_SHOWDEFAULT); UpdateWindow(hwnd_); }
    void initImGui() override { ImGui_ImplWin32_Init(hwnd_); }
    void newImGuiFrame() override { ImGui_ImplWin32_NewFrame(); }
    void shutdownImGui() override { ImGui_ImplWin32_Shutdown(); }
    bool pumpEvents() override {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            const bool fromUser = (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) ||
                                  (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) ||
                                  (msg.message >= WM_NCMOUSEMOVE && msg.message <= WM_NCXBUTTONDBLCLK);
            if (fromUser && g_pacer) g_pacer->noteInput();
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) quit = true;
        }
        return !quit;
    }
    bool minimized() override { return IsIconic(hwnd_); }
    bool focused() override { return GetForegroundWindow() == hwnd_; }
    void waitForInput(double seconds) override {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(seconds * 1000), QS_ALLINPUT);
    }
    double displayFps() override { return displayFpsOf(hwnd_); }
    Studio::WindowHooks hooks() override {
        Studio::WindowHooks h;
        h.setTitle = [this](const std::string& title) { SetWindowTextW(hwnd_, widen(title).c_str()); };
        h.requestClose = [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); };
        h.capture = [this](std::vector<u8>& rgba, int& w, int& h) { return capture(rgba, w, h); };
        return h;
    }

    // When the window stops responding (Windows calls it hung after 5 s without handling messages) for 20 s, ask
    // from this thread — the UI thread can't — whether to force Frame Studio closed. "No": not asked again for a minute.
    void startHangWatchdog() override {
        watchdog_ = std::thread([this] {
            int hungFor = 0;
            auto quietUntil = std::chrono::steady_clock::now();
            while (!watchdogStop_) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!IsWindow(hwnd_)) continue;
                hungFor = IsHungAppWindow(hwnd_) ? hungFor + 1 : 0;
                if (hungFor < 15 || std::chrono::steady_clock::now() < quietUntil) continue;
                const int r = MessageBoxW(nullptr,
                    L"Frame Studio hasn't responded for 20 seconds. It may still be working on something big "
                    L"(opening, saving or uploading a home).\n\nForce it closed? Unsaved edits will be lost.",
                    L"Frame Studio isn't responding", MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
                if (r == IDYES && IsHungAppWindow(hwnd_)) {   // not if it came back while the box was up
                    std::fprintf(stderr, "[studio] force closed (not responding)\n");
                    std::fflush(stderr);
                    std::_Exit(1);
                }
                quietUntil = std::chrono::steady_clock::now() + std::chrono::seconds(60);
                hungFor = 0;
            }
        });
    }
    void stopHangWatchdog() override {
        watchdogStop_ = true;
        if (watchdog_.joinable()) watchdog_.join();
    }

private:
    // The whole window as shown (UI, gizmo and world overlays included), for the self-test's screenshots.
    bool capture(std::vector<u8>& rgba, int& w, int& h) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
        if (w < 1 || h < 1) return false;
        HDC wdc = GetDC(hwnd_), mdc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
        HGDIOBJ old = SelectObject(mdc, bmp);
        PrintWindow(hwnd_, mdc, 0x2 /* PW_RENDERFULLCONTENT */ | PW_CLIENTONLY);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        rgba.assign(static_cast<size_t>(w) * h * 4, 0);
        GetDIBits(mdc, bmp, 0, h, rgba.data(), &bi, DIB_RGB_COLORS);
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        ReleaseDC(hwnd_, wdc);
        for (size_t q = 0; q + 3 < rgba.size(); q += 4) { std::swap(rgba[q], rgba[q + 2]); rgba[q + 3] = 255; }   // BGRA -> RGBA
        return true;
    }

    HINSTANCE inst_;
    WNDCLASSEXW wc_ = {};
    HWND hwnd_ = nullptr;
    float scale_ = 1;
    std::thread watchdog_;
    std::atomic<bool> watchdogStop_{false};
};

#else
// ── Linux and macOS: a GLFW window ───────────────────────────────────────────────────────────────────────────

class GlfwWindow : public StudioWindow {
public:
    GlfwWindow() {
        glfwInitVulkanLoader(vkGetInstanceProcAddr);   // (volk's: GLFW then needs no Vulkan library of its own)
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef __APPLE__
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
        // 1600 x 960 (in the display's own units), never bigger than the screen's free area, centred on it.
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        int wx = 0, wy = 0, ww = 1600, wh = 960;
        if (monitor) glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
        const float s = contentScale(monitor);
        const int winW = std::min(static_cast<int>(1600 * s), ww), winH = std::min(static_cast<int>(960 * s), wh);
        window_ = glfwCreateWindow(winW, winH, "Frame Studio", nullptr, nullptr);
        if (!window_) return;
        glfwSetWindowPos(window_, wx + (ww - winW) / 2, wy + (wh - winH) / 2);
        glfwSetWindowSizeLimits(window_, 900, 560, GLFW_DONT_CARE, GLFW_DONT_CARE);
        // Ours first: ImGui's backend (initImGui) chains to them. Every input counts for the frame pacer.
        glfwSetKeyCallback(window_, [](GLFWwindow*, int, int, int, int) { noteInput(); });
        glfwSetCharCallback(window_, [](GLFWwindow*, unsigned) { noteInput(); });
        glfwSetMouseButtonCallback(window_, [](GLFWwindow*, int, int, int) { noteInput(); });
        glfwSetCursorPosCallback(window_, [](GLFWwindow*, double, double) { noteInput(); });
        glfwSetCursorEnterCallback(window_, [](GLFWwindow*, int) { noteInput(); });
        glfwSetScrollCallback(window_, [](GLFWwindow*, double, double) { noteInput(); });
        glfwSetWindowFocusCallback(window_, [](GLFWwindow*, int) { noteInput(); });
        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow*, int w, int h) {
            noteInput();
            if (w > 0 && h > 0) { g_resizeW = static_cast<uint32_t>(w); g_resizeH = static_cast<uint32_t>(h); }
        });
        glfwSetWindowPosCallback(window_, [](GLFWwindow* w, int, int) {   // moved, maybe to another display
            if (auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(w)); self && g_pacer) g_pacer->setDisplayFps(self->displayFps());
        });
        glfwSetDropCallback(window_, [](GLFWwindow*, int count, const char** paths) {
            noteInput();
            filesDropped(std::vector<std::string>(paths, paths + count));
        });
        glfwSetWindowCloseCallback(window_, [](GLFWwindow* w) { if (!allowClose()) glfwSetWindowShouldClose(w, GLFW_FALSE); });
        glfwSetWindowUserPointer(window_, this);
    }
    ~GlfwWindow() override {
        if (window_) glfwDestroyWindow(window_);
        glfwTerminate();
    }
    bool ok() const { return window_ != nullptr; }

    void* nativeHandle() override { return window_; }
    float uiScale() override {
#ifdef __APPLE__
        return 1.0f;   // macOS works in points and draws them at the display's density itself
#else
        return contentScale(glfwGetPrimaryMonitor());
#endif
    }
    float pixelDensity() override {
#ifdef __APPLE__
        return contentScale(glfwGetPrimaryMonitor());   // 2 on Retina
#else
        return 1.0f;
#endif
    }
    std::vector<std::string> vulkanInstanceExtensions() override {
        uint32_t n = 0;
        const char** names = glfwGetRequiredInstanceExtensions(&n);
        return names ? std::vector<std::string>(names, names + n) : std::vector<std::string>();
    }
    VkSurfaceKHR createSurface(VkInstance instance) override {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        return glfwCreateWindowSurface(instance, window_, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
    }
    void pixelSize(int& width, int& height) override { glfwGetFramebufferSize(window_, &width, &height); }
    void show() override { glfwShowWindow(window_); }
    void initImGui() override { ImGui_ImplGlfw_InitForVulkan(window_, true); }
    void newImGuiFrame() override { ImGui_ImplGlfw_NewFrame(); }
    void shutdownImGui() override { ImGui_ImplGlfw_Shutdown(); }
    bool pumpEvents() override {
        glfwPollEvents();
        return !glfwWindowShouldClose(window_);
    }
    bool minimized() override { return glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0; }
    bool focused() override { return glfwGetWindowAttrib(window_, GLFW_FOCUSED) != 0; }
    void waitForInput(double seconds) override { glfwWaitEventsTimeout(seconds); }
    double displayFps() override {
        // the display holding the window's centre
        int x = 0, y = 0, w = 0, h = 0, count = 0;
        glfwGetWindowPos(window_, &x, &y);
        glfwGetWindowSize(window_, &w, &h);
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            int mx = 0, my = 0;
            glfwGetMonitorPos(monitors[i], &mx, &my);
            const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
            if (mode && x + w / 2 >= mx && x + w / 2 < mx + mode->width && y + h / 2 >= my && y + h / 2 < my + mode->height && mode->refreshRate > 1)
                return mode->refreshRate;
        }
        const GLFWvidmode* primary = glfwGetPrimaryMonitor() ? glfwGetVideoMode(glfwGetPrimaryMonitor()) : nullptr;
        return primary && primary->refreshRate > 1 ? primary->refreshRate : 60.0;
    }
    Studio::WindowHooks hooks() override {
        Studio::WindowHooks h;
        h.setTitle = [this](const std::string& title) { glfwSetWindowTitle(window_, title.c_str()); };
        h.requestClose = [this] { if (allowClose()) glfwSetWindowShouldClose(window_, GLFW_TRUE); };
        // (no capture: the self-test notes its screenshots as skipped)
        return h;
    }

private:
    static void noteInput() { if (g_pacer) g_pacer->noteInput(); }
    static float contentScale(GLFWmonitor* monitor) {
        float sx = 1, sy = 1;
        if (monitor) glfwGetMonitorContentScale(monitor, &sx, &sy);
        return std::max(1.0f, sx);
    }

    GLFWwindow* window_ = nullptr;
};
#endif

}  // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    _wfreopen((dataDir() / L"studio.log").c_str(), L"w", stderr);
    std::vector<std::string> args;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int a = 1; a < argc; ++a) args.push_back(narrow(argv[a]));
        LocalFree(argv);
    }
    if (volkInitialize() != VK_SUCCESS) return fail("Vulkan isn't available on this PC. Update your graphics driver.");
    Win32Window window(inst);
    return runStudio(window, args);
}
#else
// Vulkan: the system's loader, else (macOS) the MoltenVK the app bundle carries in Contents/Frameworks, used directly.
bool loadVulkan() {
    if (volkInitialize() == VK_SUCCESS) return true;
#ifdef __APPLE__
    const fs::path moltenVk = exeDir().parent_path() / "Frameworks" / "libMoltenVK.dylib";
    if (void* lib = dlopen(moltenVk.c_str(), RTLD_NOW | RTLD_LOCAL))
        if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))) {
            volkInitializeCustom(gipa);
            return true;
        }
#endif
    return false;
}

int main(int argc, char** argv) {
    if (!std::freopen((dataDir() / "studio.log").c_str(), "w", stderr)) std::perror("studio.log");   // (logs to the terminal then)
    std::vector<std::string> args(argv + 1, argv + argc);
#ifdef __APPLE__
    // Started from Finder, macOS may add a "-psn_..." process serial number argument: not a home.
    args.erase(std::remove_if(args.begin(), args.end(), [](const std::string& a) { return a.rfind("-psn_", 0) == 0; }), args.end());
#endif
    if (!loadVulkan()) return fail("Vulkan isn't available on this computer. Install your graphics driver's Vulkan support.");
    GlfwWindow window;
    if (!window.ok()) return fail("Couldn't open a window (GLFW).");
    return runStudio(window, args);
}
#endif
