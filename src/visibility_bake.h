#pragma once
// The mesh optimizer's visibility pass (Frame Studio's save, fenv-tool): geometry the player can never see is left out
// of the baked scene. Seen from where: everywhere the player can be (every walkable floor of the walk grid and a band
// past its edges for leaning and room-scale steps, spawns, seats, teleport spots), at eye heights from crouching to
// tiptoe, in every direction (a cube of six 90-degree views per spot). From each spot the solid, still geometry is
// drawn into a depth cube (a CPU rasterizer); a triangle is seen when its screen box, widened a pixel, reaches past
// that depth anywhere (conservative: coverage is the box, depth its nearest corner, the occluders shrunk by a pixel).
// What no spot sees goes: single triangles of still, solid meshes (the backs of walls, under floors, behind them),
// and whole meshes of anything else (see-through, cut-out, moved by its shader) whose widened bounds no spot sees.
// The seen triangles' neighbours stay too (a ring round everything seen). Never touched: the sky, animated, skinned
// and edited meshes. Vertices aren't removed (a shader may number them): only index lists shrink.
#include "hsl_scene.h"
#include "walkgrid.h"
#include "home_edits.h"
#include "core/parallel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

struct VisibilityResult {
    std::vector<std::vector<u32>> indices;     // per mesh: what's kept (valid where changed[m])
    std::vector<char> changed;
    size_t samples = 0, trisBefore = 0, trisAfter = 0, meshesEmptied = 0, meshesTouched = 0;
    double ms = 0;
    std::string skipped;                       // why nothing was done ("" when it ran)
};

namespace visbake {

// Whether a vertex shader puts vertices where the mesh data says (and leaves them there): it reads no clock, view or
// other per-frame value (only clipFromWorld, the camera position for fog, reverseZ) and no texture (vertex animation
// textures). A bone palette it may read (vista's ibl skins every mesh) is identity for a mesh nothing animates: the
// caller rules out the animated ones. SPIR-V scan of its access chains.
inline bool staticVertexShader(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    std::map<u32, u32> set, binding, pointee, varPtr, varStorage, constant;
    std::map<std::pair<u32, u32>, u32> memberOffset;
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return false;
        const u32* a = &code[i + 1];
        if ((op >= 87 && op <= 98) || (op >= 305 && op <= 320)) return false;              // image sampling / fetch / read
        if (op == 71 && words >= 4 && a[1] == 34) set[a[0]] = a[2];                       // DescriptorSet
        if (op == 71 && words >= 4 && a[1] == 33) binding[a[0]] = a[2];                   // Binding
        if (op == 72 && words >= 5 && a[2] == 35) memberOffset[{a[0], a[1]}] = a[3];      // member Offset
        if (op == 32 && words >= 4) pointee[a[0]] = a[2];                                 // OpTypePointer
        if (op == 43 && words >= 4) constant[a[1]] = a[2];                                // OpConstant
        if (op == 59 && words >= 4) { varPtr[a[1]] = a[0]; varStorage[a[1]] = a[2]; }    // OpVariable
        i += words;
    }
    u32 globals = 0;
    for (const auto& v : varStorage)
        if (v.second == 2 && set.count(v.first) && set[v.first] == 0 && binding.count(v.first) && binding[v.first] == 0) globals = v.first;
    const u32 globalsStruct = globals ? pointee[varPtr[globals]] : 0;
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        const u32* a = &code[i + 1];
        if ((op == 65 || op == 66) && words >= 4) {                                       // OpAccessChain
            const u32 base = a[2];
            const auto st = varStorage.find(base);
            if (st != varStorage.end()) {
                if (base == globals) {
                    if (words < 5 || !constant.count(a[3])) return false;
                    const auto off = memberOffset.find({globalsStruct, constant[a[3]]});
                    if (off == memberOffset.end()) return false;
                    if (off->second != 0 && off->second != 384 && off->second != 604) return false;   // clipFromWorld, worldCameraPos, reverseZ
                }
            }
        }
        i += words;
    }
    return true;
}

// Whether a fragment shader leaves coverage and depth to the rasterizer (no discard, no depth or mask written): an
// opaque mesh drawn with it hides what's behind it everywhere it covers.
inline bool solidFragmentShader(const std::vector<u32>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    for (size_t i = 5; i < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return false;
        if (op == 252 || op == 4416 || op == 5380) return false;
        if ((op == 71 && words >= 4 && code[i + 2] == 11 && (code[i + 3] == 22 || code[i + 3] == 20)) ||
            (op == 72 && words >= 5 && code[i + 3] == 11 && (code[i + 4] == 22 || code[i + 4] == 20)))
            return false;
        i += words;
    }
    return true;
}

constexpr float kNear = 0.02f;

// Cube face f (0..5: +X -X +Y -Y +Z -Z) of a point v relative to the eye: its depth along the face's axis, and the two
// across (u, w). In view when |u|, |w| <= depth.
inline float faceOf(const float* v, int f, float& u, float& w) {
    const int ax = f >> 1;
    u = v[(ax + 1) % 3];
    w = v[(ax + 2) % 3];
    return (f & 1) ? -v[ax] : v[ax];
}

// One face's depth: the nearest occluder per pixel, then the "far" pyramid the tests read (level 0: the farthest of each
// pixel's 3 x 3, so an occluder edge can't hide what a slightly different eye would see past it; then halves, farthest).
struct FaceDepth {
    int n = 0, levels = 0;
    std::vector<float> z;
    std::vector<std::vector<float>> farZ;
    void reset(int size) {
        n = size;
        z.assign(static_cast<size_t>(n) * n, std::numeric_limits<float>::infinity());
        levels = 0;
        for (int s = n; s >= 1; s /= 2) ++levels;
        farZ.resize(levels);
    }
    void build() {
        farZ[0].resize(z.size());
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                float m = 0;
                for (int j = std::max(0, y - 1); j <= std::min(n - 1, y + 1); ++j)
                    for (int i = std::max(0, x - 1); i <= std::min(n - 1, x + 1); ++i) m = std::max(m, z[static_cast<size_t>(j) * n + i]);
                farZ[0][static_cast<size_t>(y) * n + x] = m;
            }
        for (int l = 1, s = n / 2; l < levels; ++l, s /= 2) {
            const int ps = s * 2;
            farZ[l].resize(static_cast<size_t>(s) * s);
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x) {
                    const float* a = &farZ[l - 1][static_cast<size_t>(y * 2) * ps + x * 2];
                    farZ[l][static_cast<size_t>(y) * s + x] = std::max(std::max(a[0], a[1]), std::max(a[ps], a[ps + 1]));
                }
        }
    }
    // Whether anything in pixels [x0, x1] x [y0, y1] lies farther than depth d (something there at d could be seen).
    bool farther(int x0, int y0, int x1, int y1, float d) const {
        int l = 0;
        while (l + 1 < levels && ((x1 >> l) - (x0 >> l) > 2 || (y1 >> l) - (y0 >> l) > 2)) ++l;
        const int s = n >> l;
        const float lim = d * (1.0f - 1e-4f) - 1e-4f;
        for (int y = y0 >> l; y <= (y1 >> l) && y < s; ++y)
            for (int x = x0 >> l; x <= (x1 >> l) && x < s; ++x)
                if (farZ[l][static_cast<size_t>(y) * s + x] > lim) return true;
        return false;
    }
};

// Whether points (eye-relative) are all off face f's view on the same side (in front of the eye too: depth >= kNear).
inline bool outsideFace(const float (*v)[3], int count, int f) {
    int out[5] = {0, 0, 0, 0, 0};
    for (int k = 0; k < count; ++k) {
        float u, w;
        const float d = faceOf(v[k], f, u, w);
        out[0] += d < kNear; out[1] += u > d; out[2] += u < -d; out[3] += w > d; out[4] += w < -d;
    }
    for (int k = 0; k < 5; ++k) if (out[k] == count) return true;
    return false;
}

// A polygon (eye-relative points) clipped to depth >= kNear on face f, as screen points (pixels) and depths.
inline int clipToFace(const float (*v)[3], int count, int f, int n, float (*out)[3]) {
    float in[8][3], d[8];
    for (int k = 0; k < count; ++k) {
        float u, w;
        d[k] = faceOf(v[k], f, u, w);
        in[k][0] = u; in[k][1] = w; in[k][2] = d[k];
    }
    int m = 0;
    for (int k = 0; k < count; ++k) {
        const int j = (k + 1) % count;
        const bool a = d[k] >= kNear, b = d[j] >= kNear;
        if (a) { std::memcpy(out[m], in[k], sizeof out[m]); ++m; }
        if (a != b) {
            const float t = (kNear - d[k]) / (d[j] - d[k]);
            for (int c = 0; c < 3; ++c) out[m][c] = in[k][c] + (in[j][c] - in[k][c]) * t;
            out[m][2] = kNear;
            ++m;
        }
    }
    const float h = n * 0.5f;
    for (int k = 0; k < m; ++k) { out[k][0] = (out[k][0] / out[k][2] + 1) * h; out[k][1] = (out[k][1] / out[k][2] + 1) * h; }
    return m;
}

// Fills an occluder triangle (eye-relative) into face f's nearest depths (pixel centres inside, depth from 1/z).
inline void rasterOccluder(const float (*v)[3], int f, FaceDepth& fd) {
    float p[8][3];
    const int m = clipToFace(v, 3, f, fd.n, p);
    for (int k = 1; k + 1 < m; ++k) {
        const float* a = p[0];
        const float* b = p[k];
        const float* c = p[k + 1];
        float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
        if (std::fabs(area) < 1e-6f) continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]})))), x1 = std::min(fd.n - 1, static_cast<int>(std::ceil(std::max({a[0], b[0], c[0]}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], c[1]})))), y1 = std::min(fd.n - 1, static_cast<int>(std::ceil(std::max({a[1], b[1], c[1]}))));
        if (x0 > x1 || y0 > y1) continue;
        const float ia = 1 / area, za = 1 / a[2], zb = 1 / b[2], zc = 1 / c[2];
        for (int y = y0; y <= y1; ++y) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f;
                const float w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) * ia;
                const float w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) * ia;
                const float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float iz = w0 * za + w1 * zb + w2 * zc;
                if (iz <= 0) continue;
                float& dst = fd.z[static_cast<size_t>(y) * fd.n + x];
                dst = std::min(dst, 1 / iz);
            }
        }
    }
}

// Whether a polygon (eye-relative: a triangle, or a box's 8 corners as a cloud) can be seen on face f: its screen box
// widened a pixel reaches something farther than its nearest point. Partly behind the eye: seen if on the face at all.
inline bool seenOnFace(const float (*v)[3], int count, int f, const FaceDepth& fd, bool cloud) {
    float u, w;
    float minD = std::numeric_limits<float>::infinity();
    bool behind = false;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    const float h = fd.n * 0.5f;
    for (int k = 0; k < count; ++k) {
        const float d = faceOf(v[k], f, u, w);
        if (d < kNear) { behind = true; continue; }
        minD = std::min(minD, d);
        const float sx = (u / d + 1) * h, sy = (w / d + 1) * h;
        lo[0] = std::min(lo[0], sx); hi[0] = std::max(hi[0], sx);
        lo[1] = std::min(lo[1], sy); hi[1] = std::max(hi[1], sy);
    }
    if (behind) {
        if (cloud) return true;                  // a box round the eye, or reaching behind it: seen
        float p[8][3];
        const int m = clipToFace(v, count, f, fd.n, p);
        if (m < 3) return false;
        lo[0] = lo[1] = 1e30f; hi[0] = hi[1] = -1e30f;
        for (int k = 0; k < m; ++k) { lo[0] = std::min(lo[0], p[k][0]); hi[0] = std::max(hi[0], p[k][0]); lo[1] = std::min(lo[1], p[k][1]); hi[1] = std::max(hi[1], p[k][1]); }
        minD = kNear;
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(lo[0])) - 1), x1 = std::min(fd.n - 1, static_cast<int>(std::floor(hi[0])) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(lo[1])) - 1), y1 = std::min(fd.n - 1, static_cast<int>(std::floor(hi[1])) + 1);
    if (x0 > x1 || y0 > y1) return false;
    return fd.farther(x0, y0, x1, y1, minD);
}

// Whether an eye at height y over cell (cx, cz) is out in the open: off the grid, or clear of every solid span there.
inline bool eyeFree(const WalkGrid& g, int cx, int cz, float y) {
    if (!g.inside(cx, cz)) return true;
    for (const WalkSpan* s = g.begin(cx, cz); s != g.end(cx, cz); ++s)
        if (y >= s->lo - 0.08f && y <= s->hi + 0.08f) return false;
    return true;
}

}  // namespace visbake

struct VisibilityOptions {
    float spacing = 0.35f;                     // between spots on the floor (m); more where the area ends
    float margin = 0.4f;                       // how far past the area's edges the head goes (leaning, a step in the room)
    float heights[4] = {0.35f, 0.9f, 1.45f, 2.0f};   // eye heights over the floor: crouching to tiptoe
    int face = 256;                            // depth cube face size (pixels; a power of two)
    size_t maxSpots = 9000;                    // more than this: wider spacing
    std::vector<int> pinned;                   // meshes kept whole besides the animated ones (all a CPU animation writes)
};

// The pass. `grid`: the home's walkable area (walkWorld); `edits`: Frame Studio's object edits (their meshes are kept
// whole and hide nothing). False (result.skipped says why) when it can't vouch for the home.
inline bool bakeVisibility(const HslScene& scene, const HomeWorld& world, const WalkGrid* grid, const std::vector<ObjectEdit>* edits,
                           VisibilityResult& out, const VisibilityOptions& opt = VisibilityOptions()) {
    using namespace visbake;
    const auto t0 = std::chrono::steady_clock::now();
    out = VisibilityResult();
    const size_t nm = scene.meshes.size();
    out.indices.resize(nm);
    out.changed.assign(nm, 0);
    if (!grid || grid->walkArea <= 0) { out.skipped = "no walkable area"; return false; }
    if (!world.gravity.empty()) { out.skipped = "gravity zones (you can stand on walls)"; return false; }

    // What each mesh is: 2 still and solid (triangles go, and it hides what's behind it), 1 still but see-through or
    // cut-out (goes whole, hides nothing), 0 kept whole.
    std::vector<char> kind(nm, 0);
    std::vector<char> pinned(nm, 0);
    for (int m : scene.animatedMeshes) if (m >= 0 && static_cast<size_t>(m) < nm) pinned[m] = 1;
    for (int m : opt.pinned) if (m >= 0 && static_cast<size_t>(m) < nm) pinned[m] = 1;
    for (const HslAnimGroup& a : scene.anims) for (int m : a.meshes) if (m >= 0 && static_cast<size_t>(m) < nm) pinned[m] = 1;
    if (edits) for (const ObjectEdit& e : *edits) if (e.index >= 0 && static_cast<size_t>(e.index) < nm) pinned[e.index] = 1;
    std::map<std::string, std::pair<bool, bool>> progKind;   // shader path -> (still vertices, solid fragments)
    for (size_t m = 0; m < nm; ++m) {
        const MeshData& md = scene.meshes[m];
        if (pinned[m] || md.isSkybox || md.overlayKind || md.dynamicVerts || md.hasVat || !md.skinJoints.empty() || md.indices.size() < 3 ||
            md.positions.empty())
            continue;
        auto it = progKind.find(md.shaderPath);
        if (it == progKind.end()) {
            const ShaderProgramSrc* p = findProgram(scene, md.shaderPath);
            std::string path = md.shaderPath;
            for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool vege = path.find("vege") != std::string::npos || path.find("veg_") != std::string::npos || path.find("billboard") != std::string::npos;
            it = progKind.emplace(md.shaderPath, std::make_pair(p && !vege && staticVertexShader(p->vert), p && solidFragmentShader(p->frag))).first;
        }
        if (!it->second.first) continue;
        const bool solid = !md.useBlend && !md.additive && !md.alphaTest && !md.alphaToCoverage && it->second.second;
        kind[m] = solid ? 2 : 1;
    }

    // World-space triangles: the solid ones (occluders, tested one by one), boxes for the rest.
    struct Tri { float v[3][3]; u32 mesh, index; };
    std::vector<Tri> tris;
    std::vector<std::array<float, 6>> boxes(nm);
    std::vector<size_t> firstTri(nm + 1, 0);
    for (size_t m = 0; m < nm; ++m) {
        firstTri[m] = tris.size();
        const MeshData& md = scene.meshes[m];
        out.trisBefore += md.indices.size() / 3;
        if (!kind[m]) continue;
        float wm[16];
        if (md.hasWorldMatrix) std::memcpy(wm, md.worldMatrix, sizeof wm); else trsToMat4(md.transform, wm);
        auto xf = [&](u32 vi, float* o) {
            const float* p = &md.positions[static_cast<size_t>(vi) * 3];
            for (int j = 0; j < 3; ++j) o[j] = wm[j] * p[0] + wm[4 + j] * p[1] + wm[8 + j] * p[2] + wm[12 + j];
        };
        std::array<float, 6> bb = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            Tri tr;
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                const u32 vi = md.indices[t + k];
                if (static_cast<size_t>(vi) * 3 + 2 >= md.positions.size()) { ok = false; break; }
                xf(vi, tr.v[k]);
                for (int j = 0; j < 3; ++j) { bb[j] = std::min(bb[j], tr.v[k][j]); bb[3 + j] = std::max(bb[3 + j], tr.v[k][j]); }
            }
            if (!ok) { kind[m] = 0; break; }
            if (kind[m] == 2) { tr.mesh = static_cast<u32>(m); tr.index = static_cast<u32>(t); tris.push_back(tr); }
        }
        if (!kind[m]) { tris.resize(firstTri[m]); continue; }
        // a see-through / cut-out mesh's box, widened (a little sway, an offset the material adds)
        for (int j = 0; j < 3; ++j) { const float g = 0.25f + 0.1f * (bb[3 + j] - bb[j]); bb[j] -= g; bb[3 + j] += g; }
        boxes[m] = bb;
    }
    firstTri[nm] = tris.size();

    // Where the eyes can be.
    const WalkParams& P = grid->p;
    std::vector<std::array<float, 3>> spots;
    auto addHeights = [&](float x, float y, float z, const float* hs, int n) { for (int k = 0; k < n; ++k) spots.push_back({x, y + hs[k], z}); };
    float spacing = opt.spacing;
    for (int pass = 0; pass < 6; ++pass) {
        spots.clear();
        const int step = std::max(1, static_cast<int>(std::lround(spacing / P.cell)));
        for (int cz = 0; cz < grid->nz; ++cz)
            for (int cx = 0; cx < grid->nx; ++cx)
                for (const WalkSpan* s = grid->begin(cx, cz); s != grid->end(cx, cz); ++s) {
                    if (!(s->flags & kWalkOk)) continue;
                    static const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    bool edge = false;
                    for (const auto& d : nb)
                        if (!grid->walkNear(cx + d[0], cz + d[1], s->hi, P.step, kWalkOk)) {
                            edge = true;
                            // past the edge: the head over it (a lean, a step in the room), as far as the margin goes
                            // before the eye would be in something (a wall, the sofa: from inside, everything shows)
                            if ((cx + cz) % std::max(1, step / 2) != 0) continue;
                            const int reach = std::max(1, static_cast<int>(std::lround(opt.margin / P.cell)));
                            for (float hh : opt.heights) {
                                const float y = s->hi + hh;
                                int k = 0;
                                while (k < reach && eyeFree(*grid, cx + d[0] * (k + 1), cz + d[1] * (k + 1), y)) ++k;
                                if (k > 0) spots.push_back({grid->centerX(cx + d[0] * k), y, grid->centerZ(cz + d[1] * k)});
                            }
                        }
                    if (edge ? (cx + cz) % std::max(1, step / 2) == 0 : (cx % step == 0 && cz % step == 0))
                        addHeights(grid->centerX(cx), s->hi, grid->centerZ(cz), opt.heights, 4);
                }
        if (spots.size() <= opt.maxSpots) break;
        spacing *= std::sqrt(static_cast<float>(spots.size()) / opt.maxSpots) * 1.05f;
    }
    static const float seatH[3] = {0.5f, 0.8f, 1.1f};
    for (const WorldPoint& p : world.seats) addHeights(p.pos[0], p.pos[1], p.pos[2], seatH, 3);
    for (const auto* list : {&world.spawns, &world.hotspots})
        for (const WorldPoint& p : *list) addHeights(p.pos[0], p.pos[1], p.pos[2], opt.heights, 4);
    if (const char* e = std::getenv("FH_VIS_SPOTS")) {   // diagnostics: only the first n spots (spread evenly)
        const size_t n = std::max<size_t>(1, std::strtoul(e, nullptr, 10));
        if (n < spots.size()) { std::vector<std::array<float, 3>> few; for (size_t i = 0; i < n; ++i) few.push_back(spots[i * spots.size() / n]); spots.swap(few); }
    }
    out.samples = spots.size();
    if (spots.empty()) { out.skipped = "nowhere to stand"; return false; }

    // From every spot, six faces: occluders in, then what's seen marked.
    std::unique_ptr<std::atomic<uint8_t>[]> seenTri(new std::atomic<uint8_t>[tris.size() ? tris.size() : 1]);
    std::unique_ptr<std::atomic<uint8_t>[]> seenMesh(new std::atomic<uint8_t>[nm ? nm : 1]);
    for (size_t i = 0; i < tris.size(); ++i) seenTri[i].store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < nm; ++i) seenMesh[i].store(0, std::memory_order_relaxed);
    const int faceN = opt.face;
    par::forEach(spots.size(), [&](size_t si) {
        thread_local FaceDepth fds[6];
        const float* e = spots[si].data();
        for (int f = 0; f < 6; ++f) fds[f].reset(faceN);
        float v[8][3];
        for (const Tri& t : tris) {
            for (int k = 0; k < 3; ++k) for (int j = 0; j < 3; ++j) v[k][j] = t.v[k][j] - e[j];
            for (int f = 0; f < 6; ++f) if (!outsideFace(v, 3, f)) rasterOccluder(v, f, fds[f]);
        }
        for (int f = 0; f < 6; ++f) fds[f].build();
        for (size_t i = 0; i < tris.size(); ++i) {
            if (seenTri[i].load(std::memory_order_relaxed)) continue;
            for (int k = 0; k < 3; ++k) for (int j = 0; j < 3; ++j) v[k][j] = tris[i].v[k][j] - e[j];
            for (int f = 0; f < 6; ++f)
                if (!outsideFace(v, 3, f) && seenOnFace(v, 3, f, fds[f], false)) { seenTri[i].store(1, std::memory_order_relaxed); break; }
        }
        for (size_t m = 0; m < nm; ++m) {
            if (kind[m] != 1 || seenMesh[m].load(std::memory_order_relaxed)) continue;
            const auto& b = boxes[m];
            if (e[0] >= b[0] && e[0] <= b[3] && e[1] >= b[1] && e[1] <= b[4] && e[2] >= b[2] && e[2] <= b[5]) { seenMesh[m].store(1); continue; }
            for (int k = 0; k < 8; ++k) {
                v[k][0] = b[(k & 1) ? 3 : 0] - e[0];
                v[k][1] = b[(k & 2) ? 4 : 1] - e[1];
                v[k][2] = b[(k & 4) ? 5 : 2] - e[2];
            }
            for (int f = 0; f < 6; ++f)
                if (!outsideFace(v, 8, f) && seenOnFace(v, 8, f, fds[f], true)) { seenMesh[m].store(1, std::memory_order_relaxed); break; }
        }
    });

    // What's kept: the seen triangles and one ring round them (triangles touching a seen one's corner), seen meshes.
    for (size_t m = 0; m < nm; ++m) {
        const MeshData& md = scene.meshes[m];
        if (kind[m] == 1 && !seenMesh[m].load()) {
            out.changed[m] = 1;
            ++out.meshesEmptied;
            continue;
        }
        if (kind[m] != 2) { out.trisAfter += md.indices.size() / 3; continue; }
        std::set<std::array<float, 3>> corners;
        bool all = true;
        for (size_t i = firstTri[m]; i < firstTri[m + 1]; ++i) {
            if (!seenTri[i].load()) { all = false; continue; }
            for (int k = 0; k < 3; ++k) {
                const float* p = &md.positions[static_cast<size_t>(md.indices[tris[i].index + k]) * 3];
                corners.insert({p[0], p[1], p[2]});
            }
        }
        if (all) { out.trisAfter += md.indices.size() / 3; continue; }
        std::vector<u32>& keep = out.indices[m];
        for (size_t i = firstTri[m]; i < firstTri[m + 1]; ++i) {
            const u32 t = tris[i].index;
            bool k = seenTri[i].load() != 0;
            static const bool noRing = std::getenv("FH_VIS_NORING") != nullptr;   // diagnostics: the seen ones alone
            for (int c = 0; c < 3 && !k && !noRing; ++c) {
                const float* p = &md.positions[static_cast<size_t>(md.indices[t + c]) * 3];
                k = corners.count({p[0], p[1], p[2]}) != 0;
            }
            if (k) keep.insert(keep.end(), md.indices.begin() + t, md.indices.begin() + t + 3);
        }
        out.changed[m] = 1;
        out.trisAfter += keep.size() / 3;
        if (keep.empty()) ++out.meshesEmptied; else ++out.meshesTouched;
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (std::getenv("FH_VIS_DEBUG"))   // per mesh: what it was taken for and what's left
        for (size_t m = 0; m < nm; ++m) {
            const MeshData& md = scene.meshes[m];
            std::fprintf(stderr, "[optimize] %-50s %s %6zu -> %6zu tris  [%s]\n", md.name.c_str(), kind[m] == 2 ? "solid" : kind[m] == 1 ? "whole" : "kept ",
                         md.indices.size() / 3, out.changed[m] ? out.indices[m].size() / 3 : md.indices.size() / 3, md.shaderPath.c_str());
        }
    return true;
}
