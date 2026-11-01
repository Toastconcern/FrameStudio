#pragma once
// The mesh optimizer's visibility pass (Frame Studio's save, fenv-tool): geometry the player can never see is left out
// of the baked scene. Seen from where: everywhere the player can be (every walkable floor of the walk grid and a band
// past its edges for leaning and room-scale steps, spawns, seats, teleport spots), at eye heights from crouching to
// tiptoe, in every direction (a cube of six 90-degree views per spot). From each spot the solid, still geometry is
// drawn into a cube of triangle ids (a CPU rasterizer, 512 pixels a face): a triangle is seen when it's the nearest at
// some pixel, or ties with the nearest (two surfaces that close draw in an order the bake can't know); a thin one
// (a branch, a rail: pixel centres can miss it) when it touches a pixel where it's nearest. Spots 35 cm
// apart at four heights, checked against renders from random places in between: no pixel changes. What no spot sees
// goes: single triangles of still, solid meshes (the backs of walls, under floors, the hidden side of a surface built
// two-sided, the inside of closed shapes), and whole meshes of anything else (see-through, cut-out, moved by its
// shader) whose widened bounds no spot sees (box against the depths, an edge looked past by a pixel). Also gone, seen
// or not: a solid mesh's triangle drawn over exactly by a later one of the same mesh (coveredTwins). Never touched:
// the sky, animated, skinned and edited meshes. Vertices aren't removed (a shader may number them): only index lists
// shrink.
#include "hsl_scene.h"
#include "walkgrid.h"
#include "home_edits.h"
#include "core/parallel.h"
#include "mesh_simplify.h"

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
#include <thread>
#include <unordered_map>
#include <vector>

struct VisibilityResult {
    std::vector<std::vector<u32>> indices;     // per mesh: what's kept (valid where changed[m])
    std::vector<char> changed;
    std::vector<char> cullBack;                // per mesh: its back isn't drawn (each triangle turned to the side seen)
    std::vector<std::vector<u32>> removed;     // per mesh: the hidden triangles left out (index triples; not the twins), to show
    std::vector<std::vector<float>> normals;   // per changed mesh: its vertex normals pinned (empty: its own stand)
    size_t samples = 0, trisBefore = 0, trisAfter = 0, meshesEmptied = 0, meshesTouched = 0, meshesCulled = 0, trisFlipped = 0, trisMerged = 0;
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

// Whether a shader reads which side of a triangle it's drawing (gl_FrontFacing: lit differently from behind): its
// triangles can't be turned round.
inline bool readsFrontFacing(const std::vector<u32>& code) {
    for (size_t i = 5; i + 3 < code.size();) {
        const u32 words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words) return true;
        if (op == 71 && words >= 4 && code[i + 2] == 11 && code[i + 3] == 17) return true;   // OpDecorate BuiltIn FrontFacing
        if (op == 72 && words >= 5 && code[i + 3] == 11 && code[i + 4] == 17) return true;   // OpMemberDecorate
        i += words;
    }
    return false;
}

// Which side of a triangle (eye-relative corners) faces the eye: 1 its front (clockwise as the eye sees it, the
// renderer's front face: VK_FRONT_FACE_CLOCKWISE under a projection that flips y; checked against renders), 2 its back,
// 3 edge-on (either, a step aside).
inline uint8_t sideFacing(const float (*r)[3]) {
    const float a[3] = {r[1][0] - r[0][0], r[1][1] - r[0][1], r[1][2] - r[0][2]}, b[3] = {r[2][0] - r[0][0], r[2][1] - r[0][1], r[2][2] - r[0][2]};
    const float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    const float toEye = -(n[0] * r[0][0] + n[1] * r[0][1] + n[2] * r[0][2]);
    const float scale = std::sqrt((n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (r[0][0] * r[0][0] + r[0][1] * r[0][1] + r[0][2] * r[0][2]));
    if (std::fabs(toEye) <= scale * 1e-3f) return 3;
    return toEye < 0 ? 1 : 2;
}

// A mesh's vertex normals as the renderer shades it: its own where it has them, else smooth ones from its triangles
// (HslRenderer::buildVertices). Worked out from the triangles as they were, they keep it shaded the same once some
// are left out or turned round.
inline std::vector<float> pinnedNormals(const MeshData& md) {
    const size_t nv = std::min<size_t>(md.nVerts, md.positions.size() / 3);
    std::vector<float> n(nv * 3, 0.0f);
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
        const u32 a = md.indices[t], b = md.indices[t + 1], c = md.indices[t + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        const float* pa = &md.positions[a * 3];
        const float* pb = &md.positions[b * 3];
        const float* pc = &md.positions[c * 3];
        const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        const float f[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 v : {a, b, c}) for (int k = 0; k < 3; ++k) n[v * 3 + k] += f[k];
    }
    for (size_t v = 0; v < nv; ++v) {
        float* p = &n[v * 3];
        const float* own = md.normals.size() >= nv * 3 ? &md.normals[v * 3] : nullptr;
        if (own && (own[0] != 0 || own[1] != 0 || own[2] != 0)) { std::memcpy(p, own, 12); continue; }
        const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (l > 1e-12f) { p[0] /= l; p[1] /= l; p[2] /= l; } else { p[0] = p[2] = 0; p[1] = 1; }
    }
    return n;
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

// One face's depth: per pixel the nearest triangle (depth, which one) and any other within a tie of it, then the "far"
// pyramid the box tests read (level 0: the farthest of each pixel's 3 x 3, so an occluder edge can't hide what a
// slightly different eye would see past it; then halves, farthest).
struct FaceDepth {
    int n = 0, levels = 0;
    std::vector<float> z, tieZ;
    std::vector<int> id, tieId;                 // tris index, -1 none
    std::vector<std::vector<float>> farZ;
    void reset(int size) {
        n = size;
        const size_t count = static_cast<size_t>(n) * n;
        z.assign(count, std::numeric_limits<float>::infinity());
        tieZ.assign(count, std::numeric_limits<float>::infinity());
        id.assign(count, -1);
        tieId.assign(count, -1);
        levels = 0;
        for (int s = n; s >= 1; s /= 2) ++levels;
        farZ.resize(levels);
    }
    // Two surfaces this close in depth (m) draw in an order and with a rounding the bake can't know (meshes aren't drawn
    // in scene order; one surface triangulated twice fights pixel by pixel): both count as seen.
    static float tie(float d) { return d * 2e-5f + 1e-5f; }
    // A triangle's fragment at pixel o, depth d: nearest wins (the later on equal depth, as the renderer draws one mesh).
    void put(size_t o, float d, int tri) {
        if (d <= z[o]) {
            if (z[o] - d <= tie(d)) { tieZ[o] = z[o]; tieId[o] = id[o]; }
            else if (tieZ[o] - d > tie(d)) tieId[o] = -1;
            z[o] = d;
            id[o] = tri;
        } else if (d - z[o] <= tie(z[o]) && d < tieZ[o]) {
            tieZ[o] = d;
            tieId[o] = tri;
        }
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

// Draws triangle `tri` (eye-relative) into face f (pixel centres inside, depth from 1/z).
inline void rasterTriangle(const float (*v)[3], int f, FaceDepth& fd, int tri) {
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
                fd.put(static_cast<size_t>(y) * fd.n + x, 1 / iz, tri);
            }
        }
    }
}

// Whether a thin triangle (eye-relative; under 1.5 pixels across on face f, so pixel centres can miss it: a branch, a
// rail, a cable far off; asked of the ones turned outwards) shows anywhere on face f: every pixel it touches
// (conservative coverage), at its nearest depth over that pixel, against the nearest drawn there (or tied with it).
// Not thin: false (the pixel centres decided). The drawn depths must be complete.
inline bool thinSeen(const float (*v)[3], int f, const FaceDepth& fd) {
    constexpr float kThin = 1.5f;
    float p[8][3];
    const int m = clipToFace(v, 3, f, fd.n, p);
    for (int k = 1; k + 1 < m; ++k) {
        const float* q[4] = {p[0], p[k], p[k + 1], p[0]};
        const float* a = q[0];
        const float* b = q[1];
        const float* c = q[2];
        const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);   // twice the area
        float ex[3], ey[3], slack[3], longest = 0;
        for (int e = 0; e < 3; ++e) {
            ex[e] = q[e + 1][0] - q[e][0];
            ey[e] = q[e + 1][1] - q[e][1];
            slack[e] = 0.5f * (std::fabs(ex[e]) + std::fabs(ey[e]));
            longest = std::max(longest, std::sqrt(ex[e] * ex[e] + ey[e] * ey[e]));
        }
        if (longest > 0 && std::fabs(area) / longest >= kThin) continue;   // wide: the pixel centres see it
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], c[0]}))) - 1), x1 = std::min(fd.n - 1, static_cast<int>(std::floor(std::max({a[0], b[0], c[0]}))) + 1);
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], c[1]}))) - 1), y1 = std::min(fd.n - 1, static_cast<int>(std::floor(std::max({a[1], b[1], c[1]}))) + 1);
        const float nearest = std::min({a[2], b[2], c[2]});
        const bool flat = std::fabs(area) < 1e-6f;   // a line or a point on screen: its nearest depth throughout
        const float s = area >= 0 ? 1.0f : -1.0f;
        float za = 0, dzx = 0, dzy = 0, rise = 0;
        if (!flat) {   // 1/depth across the screen, and its largest rise within half a pixel
            const float ia = 1 / area, zb = 1 / b[2], zc = 1 / c[2];
            za = 1 / a[2];
            dzx = ((zb - za) * (c[1] - a[1]) - (zc - za) * (b[1] - a[1])) * ia;
            dzy = ((zc - za) * (b[0] - a[0]) - (zb - za) * (c[0] - a[0])) * ia;
            rise = 0.5f * (std::fabs(dzx) + std::fabs(dzy));
        }
        for (int y = y0; y <= y1; ++y) {
            const float py = y + 0.5f;
            for (int x = x0; x <= x1; ++x) {
                const float px = x + 0.5f;
                bool in = true;
                for (int e = 0; e < 3 && in; ++e) {
                    const float edge = ex[e] * (py - q[e][1]) - ey[e] * (px - q[e][0]);
                    in = flat ? std::fabs(edge) <= slack[e] + 1e-3f || (ex[e] == 0 && ey[e] == 0) : s * edge + slack[e] >= 0;
                }
                if (!in) continue;
                float d = nearest;
                if (!flat) { const float iz = za + dzx * (px - a[0]) + dzy * (py - a[1]) + rise; if (iz > 0) d = std::max(1 / iz, nearest); }
                const float drawn = fd.z[static_cast<size_t>(y) * fd.n + x];
                if (d <= drawn + FaceDepth::tie(drawn)) return true;
            }
        }
    }
    return false;
}

// Whether a box (its 8 corners, eye-relative) can be seen on face f: its screen box widened a pixel reaches something
// farther than its nearest corner. Reaching behind the eye: seen.
inline bool boxSeenOnFace(const float (*v)[3], int f, const FaceDepth& fd) {
    float u, w;
    float minD = std::numeric_limits<float>::infinity();
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    const float h = fd.n * 0.5f;
    for (int k = 0; k < 8; ++k) {
        const float d = faceOf(v[k], f, u, w);
        if (d < kNear) return true;
        minD = std::min(minD, d);
        const float sx = (u / d + 1) * h, sy = (w / d + 1) * h;
        lo[0] = std::min(lo[0], sx); hi[0] = std::max(hi[0], sx);
        lo[1] = std::min(lo[1], sy); hi[1] = std::max(hi[1], sy);
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

// A solid mesh's triangle in world space: mesh and first index in its index list.
struct Tri { float v[3][3]; u32 mesh, index; };

// The triangles of one mesh (tris[first, last)) another one later in its index list draws over exactly: the same three
// corners, either way round. Every pipeline draws both sides (no culling) and the later of two triangles at the same
// depth wins, so these never show from anywhere: a surface made two-sided by repeating it the other way round, or
// one listed twice. Marked in covered (indexed like tris).
inline void markCoveredTwins(const std::vector<Tri>& tris, size_t first, size_t last, std::vector<char>& covered) {
    using Corner = std::array<float, 3>;
    std::set<std::array<Corner, 3>> later;
    for (size_t i = last; i-- > first;) {
        std::array<Corner, 3> key;
        for (int k = 0; k < 3; ++k) key[k] = {tris[i].v[k][0], tris[i].v[k][1], tris[i].v[k][2]};
        std::sort(key.begin(), key.end());
        if (!later.insert(key).second) covered[i] = 1;
    }
}
}  // namespace visbake

struct VisibilityOptions {
    float spacing = 0.35f;                     // between spots on the floor (m); more where the area ends
    float margin = 0.4f;                       // how far past the area's edges the head goes (leaning, a step in the room)
    float heights[4] = {0.35f, 0.9f, 1.45f, 2.0f};   // eye heights over the floor: crouching to tiptoe
    int face = 512;                            // cube face size (pixels; a power of two): what's never nearest at a pixel goes
    bool cull = true;                          // solid meshes' backs not drawn (each triangle turned to the side seen)
    bool merge = true;                         // what's kept rebuilt with fewer triangles where nothing changes (mesh_simplify.h)
    int speck = 0;                             // won no more than this many pixels from any one spot (and never showed as a
                                               // thin line): left out. 0: only what was ever just tied with what's drawn
                                               // there (it covers the same pixels); 1 already leaves visible specks. -1: none
    int threads = 0;                           // 0: the shared pool (par::forEach); else this many of its own
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
    std::map<std::string, std::array<bool, 3>> progKind;   // shader path -> still vertices, solid fragments, reads FrontFacing
    std::vector<char> turnable(nm, 0);                     // its triangles can be turned round
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
            it = progKind.emplace(md.shaderPath, std::array<bool, 3>{p && !vege && staticVertexShader(p->vert), p && solidFragmentShader(p->frag),
                                                                     !p || readsFrontFacing(p->frag) || readsFrontFacing(p->vert)}).first;
        }
        if (!it->second[0]) continue;
        const bool solid = !md.useBlend && !md.additive && !md.alphaTest && !md.alphaToCoverage && it->second[1];
        turnable[m] = !it->second[2];
        kind[m] = solid ? 2 : 1;
    }

    // World-space triangles: the solid ones (occluders, tested one by one), boxes for the rest.
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
    std::vector<char> covered(tris.size(), 0);
    for (size_t m = 0; m < nm; ++m) markCoveredTwins(tris, firstTri[m], firstTri[m + 1], covered);
    for (size_t i = 0; i < tris.size(); ++i) {   // no area at all (strip stitching): nothing to see from anywhere
        const float* a = tris[i].v[0];
        const float* b = tris[i].v[1];
        const float* c = tris[i].v[2];
        const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]}, w[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        const double n[3] = {u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-24) covered[i] = 1;
    }

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

    // From every spot, six faces, one at a time: the solid triangles drawn in, the ones left showing at any pixel marked
    // seen (and any tied with them), then the see-through meshes' boxes tested against what's there.
    //   First a survey from every 16th spot, for each mesh's outside: the side of it seen most (meshes aren't wound
    // alike: everything was drawn two-sided, so nothing kept them so). Then the thin triangles turned outwards that the
    // pixel centres missed are given a second look; the ones turned in never are (the inside of a closed shape doesn't
    // show, and touching its outline it would always pass).
    // seenSide: per triangle, the sides any spot saw (1 front, 2 back, 4 only edge-on; 0 never seen), and how (16 won a
    // pixel, 8 the thin look).
    std::unique_ptr<std::atomic<uint8_t>[]> seenSide(new std::atomic<uint8_t>[tris.size() ? tris.size() : 1]);
    std::unique_ptr<std::atomic<uint8_t>[]> seenMesh(new std::atomic<uint8_t>[nm ? nm : 1]);
    std::unique_ptr<std::atomic<uint64_t>[]> sidePixels(new std::atomic<uint64_t>[2 * nm + 1]);   // survey: front, back
    // bestShow: per triangle, the most pixels it won from any one spot (a tie or the thin look: none), saturating
    std::unique_ptr<std::atomic<uint16_t>[]> bestShow(new std::atomic<uint16_t>[tris.size() ? tris.size() : 1]);
    for (size_t i = 0; i < tris.size(); ++i) { seenSide[i].store(0, std::memory_order_relaxed); bestShow[i].store(0, std::memory_order_relaxed); }
    for (size_t i = 0; i < nm; ++i) seenMesh[i].store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < 2 * nm + 1; ++i) sidePixels[i].store(0, std::memory_order_relaxed);
    std::vector<uint8_t> outside(nm, 3);   // per mesh: its outer side (1 front, 2 back, 3 either)
    bool survey = true;
    // What one worker reuses from spot to spot.
    struct Scratch {
        FaceDepth fd;
        std::vector<std::array<float, 9>> rel;   // the triangles eye-relative
        std::vector<uint8_t> onFaces, facing;    // the cube faces each is on, the side it turns to the eye
        std::vector<uint64_t> pixels;            // survey: per mesh, pixels won front / back
        std::vector<uint32_t> won;               // this face: pixels each triangle won (only `touched` nonzero)
        std::vector<int> touched;
    };
    const auto fromSpot = [&](size_t si, Scratch& sc) {
        FaceDepth& fd = sc.fd;
        auto& rel = sc.rel;
        auto& onFaces = sc.onFaces;
        auto& facing = sc.facing;
        auto& pixels = sc.pixels;
        const float* e = spots[si].data();
        float v[8][3];
        for (size_t m = 0; m < nm && !survey; ++m) {   // an eye inside a see-through mesh's box sees it
            const auto& b = boxes[m];
            if (kind[m] == 1 && e[0] >= b[0] && e[0] <= b[3] && e[1] >= b[1] && e[1] <= b[4] && e[2] >= b[2] && e[2] <= b[5])
                seenMesh[m].store(1, std::memory_order_relaxed);
        }
        // the triangles eye-relative, the faces each is on and the side it turns to this eye (a covered twin is left
        // out: its twin hides the same). Once one is seen from that side, there's nothing more to learn of it.
        rel.resize(tris.size());
        onFaces.assign(tris.size(), 0);
        facing.resize(tris.size());
        for (size_t i = 0; i < tris.size(); ++i) {
            if (covered[i]) continue;
            float (*r)[3] = reinterpret_cast<float (*)[3]>(rel[i].data());
            for (int k = 0; k < 3; ++k) for (int j = 0; j < 3; ++j) r[k][j] = tris[i].v[k][j] - e[j];
            for (int f = 0; f < 6; ++f) if (!outsideFace(r, 3, f)) onFaces[i] |= static_cast<uint8_t>(1 << f);
            facing[i] = sideFacing(r);
        }
        // (seen edge-on: it covers nothing from either side, so it counts as seen without a side: bit 4)
        const auto known = [&](size_t i) {
            const uint8_t s = seenSide[i].load(std::memory_order_relaxed);
            return facing[i] == 3 ? s != 0 : (s & facing[i]) == facing[i];
        };
        // (and how: 16 it won a pixel outright, 8 the thin look; neither: only ever tied with what's drawn there)
        const auto see = [&](size_t i, uint8_t how) {
            const uint8_t bits = static_cast<uint8_t>((facing[i] == 3 ? 4 : facing[i]) | how);
            if ((seenSide[i].load(std::memory_order_relaxed) & bits) != bits) seenSide[i].fetch_or(bits, std::memory_order_relaxed);
        };
        if (survey) pixels.assign(2 * nm, 0);
        for (int f = 0; f < 6; ++f) {
            fd.reset(opt.face);
            for (size_t i = 0; i < tris.size(); ++i)
                if (onFaces[i] & (1 << f)) rasterTriangle(reinterpret_cast<const float (*)[3]>(rel[i].data()), f, fd, static_cast<int>(i));
            if (survey) {
                for (int t : fd.id) if (t >= 0 && facing[t] != 3) ++pixels[2 * tris[t].mesh + (facing[t] == 2)];
                continue;
            }
            sc.won.resize(tris.size());
            for (size_t o = 0; o < fd.id.size(); ++o) {
                if (fd.id[o] >= 0) {
                    see(static_cast<size_t>(fd.id[o]), 16);
                    if (!sc.won[fd.id[o]]++) sc.touched.push_back(fd.id[o]);
                }
                if (fd.tieId[o] >= 0) see(static_cast<size_t>(fd.tieId[o]), 0);
            }
            for (int t : sc.touched) {
                const uint16_t n = static_cast<uint16_t>(std::min<uint32_t>(sc.won[t], 65535));
                uint16_t best = bestShow[t].load(std::memory_order_relaxed);
                while (n > best && !bestShow[t].compare_exchange_weak(best, n, std::memory_order_relaxed)) {}
                sc.won[t] = 0;
            }
            sc.touched.clear();
            for (size_t i = 0; i < tris.size(); ++i)   // the thin ones turned outwards (or either way: unsure)
                if ((onFaces[i] & (1 << f)) && (facing[i] & outside[tris[i].mesh]) && !(known(i) && (seenSide[i].load(std::memory_order_relaxed) & 24)) &&
                    thinSeen(reinterpret_cast<const float (*)[3]>(rel[i].data()), f, fd))
                    see(i, 8);
            fd.build();
            for (size_t m = 0; m < nm; ++m) {
                if (kind[m] != 1 || seenMesh[m].load(std::memory_order_relaxed)) continue;
                const auto& b = boxes[m];
                for (int k = 0; k < 8; ++k) {
                    v[k][0] = b[(k & 1) ? 3 : 0] - e[0];
                    v[k][1] = b[(k & 2) ? 4 : 1] - e[1];
                    v[k][2] = b[(k & 4) ? 5 : 2] - e[2];
                }
                if (!outsideFace(v, 8, f) && boxSeenOnFace(v, f, fd)) seenMesh[m].store(1, std::memory_order_relaxed);
            }
        }
        if (survey) for (size_t k = 0; k < 2 * nm; ++k) if (pixels[k]) sidePixels[k].fetch_add(pixels[k], std::memory_order_relaxed);
    };
    const auto fromSpots = [&](const std::vector<size_t>& which) {
        if (opt.threads > 0) {   // its own threads: the shared pool stays free for whatever else is running
            std::atomic<size_t> next{0};
            std::vector<std::thread> workers;
            for (int t = 0; t < opt.threads; ++t)
                workers.emplace_back([&] {
                    Scratch sc;
                    for (size_t k; (k = next.fetch_add(1)) < which.size();) fromSpot(which[k], sc);
                });
            for (std::thread& t : workers) t.join();
        } else {   // (the pool's threads live as long as the program: theirs is kept)
            par::forEach(which.size(), [&](size_t k) { thread_local Scratch sc; fromSpot(which[k], sc); });
        }
    };
    std::vector<size_t> every(spots.size()), some;
    for (size_t i = 0; i < spots.size(); ++i) { every[i] = i; if (i % 16 == 0) some.push_back(i); }
    fromSpots(some);
    for (size_t m = 0; m < nm; ++m) {   // a side seen three times as much as the other is the outside
        const uint64_t front = sidePixels[2 * m].load(), back = sidePixels[2 * m + 1].load();
        outside[m] = front >= 3 * back && front > 0 ? 1 : back >= 3 * front && back > 0 ? 2 : 3;
    }
    survey = false;
    fromSpots(every);

    // What's kept: the seen triangles, the seen meshes. A solid mesh's back isn't drawn (cullBack): each triangle seen
    // from one side only turned that side to the front, one seen from both drawn as a pair back to back (the same
    // pixels as before, from either side: one of the two is drawn, the other dropped before it costs a pixel). Not when
    // more than half would be pairs (their extra indices outweigh the backs saved), nor when its shader tells the sides
    // apart (then only if every one is seen from its front already).
    // Seen at all, and more than a speck: won a pixel outright from some spot, more than opt.speck of them, or showed as
    // a thin line (a branch far off). Only ever tied with what's drawn there: what it ties with covers the same pixels.
    const auto keeps = [&](size_t i) -> uint8_t {
        const uint8_t seen = covered[i] ? 0 : seenSide[i].load();
        if (!seen) return 0;
        const bool more = (seen & 8) || ((seen & 16) && static_cast<int>(bestShow[i].load()) > opt.speck);
        return opt.speck < 0 || more ? seen : 0;
    };
    out.cullBack.assign(nm, 0);
    out.removed.assign(nm, {});
    out.normals.assign(nm, {});
    for (size_t m = 0; m < nm; ++m) {
        const MeshData& md = scene.meshes[m];
        if (kind[m] == 1 && !seenMesh[m].load()) {
            out.removed[m] = md.indices;
            out.changed[m] = 1;
            ++out.meshesEmptied;
            continue;
        }
        if (kind[m] != 2) { out.trisAfter += md.indices.size() / 3; continue; }
        size_t kept = 0, pairs = 0, back = 0;
        for (size_t i = firstTri[m]; i < firstTri[m + 1]; ++i) {
            const uint8_t seen = keeps(i);   // (a covered twin: its twin is kept, drawn the same)
            const uint8_t side = seen & 3;
            kept += seen != 0;
            pairs += side == 3;
            back += side == 2;
        }
        const bool cull = opt.cull && kept > 0 && (turnable[m] ? pairs * 2 <= kept : pairs == 0 && back == 0);
        // the kept triangles by the side they're seen from: 0 the front (or only ever edge-on), 1 the back, 2 both (when
        // its back is drawn anyway, all in 0)
        std::vector<u32> group[3];
        for (size_t i = firstTri[m]; i < firstTri[m + 1]; ++i) {
            const uint8_t seen = keeps(i);
            const uint8_t side = seen & 3;
            const u32* t = &md.indices[tris[i].index];
            if (!seen) { if (!covered[i]) out.removed[m].insert(out.removed[m].end(), t, t + 3); continue; }
            std::vector<u32>& g = group[!cull || side <= 1 ? 0 : side == 2 ? 1 : 2];
            g.insert(g.end(), t, t + 3);
        }
        // fewer triangles where nothing changes: each group on its own, its edges kept where they are (they meet the
        // others there)
        if (opt.merge && kept) {
            float wm[16];
            if (md.hasWorldMatrix) std::memcpy(wm, md.worldMatrix, sizeof wm); else trsToMat4(md.transform, wm);
            const meshsimplify::Mesh shape(md, wm, pinnedNormals(md));
            for (std::vector<u32>& g : group) {
                const size_t was = g.size() / 3;
                g = shape.simplify(g);
                out.trisMerged += was - g.size() / 3;
                kept -= was - g.size() / 3;
            }
        }
        // ...then turned to face the side seen, or drawn as a pair back to back
        std::vector<u32>& keep = out.indices[m];
        keep = group[0];
        for (size_t k = 0; k + 2 < group[1].size(); k += 3) keep.insert(keep.end(), {group[1][k], group[1][k + 2], group[1][k + 1]});
        for (size_t k = 0; k + 2 < group[2].size(); k += 3) {
            keep.insert(keep.end(), group[2].begin() + k, group[2].begin() + k + 3);
            keep.insert(keep.end(), {group[2][k], group[2][k + 2], group[2][k + 1]});
        }
        out.trisFlipped += group[1].size() / 3;
        out.trisAfter += kept;
        if (std::getenv("FH_VIS_DEBUG"))
            std::fprintf(stderr, "[optimize] %-50s kept %zu: back only %zu, both sides %zu; outside %d; sides apart %d\n", md.name.c_str(), kept, back, pairs,
                         outside[m], !turnable[m]);
        if (cull) { out.cullBack[m] = 1; ++out.meshesCulled; }
        if (keep == md.indices) { keep.clear(); continue; }   // the same list: unchanged
        out.changed[m] = 1;
        out.normals[m] = pinnedNormals(md);
        if (!kept) ++out.meshesEmptied; else if (kept < md.indices.size() / 3) ++out.meshesTouched;
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (std::getenv("FH_VIS_DEBUG"))   // per mesh: what it was taken for and what's left
        for (size_t m = 0; m < nm; ++m) {
            const MeshData& md = scene.meshes[m];
            std::fprintf(stderr, "[optimize] %-50s %s %6zu -> %6zu tris %s [%s]\n", md.name.c_str(), kind[m] == 2 ? "solid" : kind[m] == 1 ? "whole" : "kept ",
                         md.indices.size() / 3, out.changed[m] ? out.indices[m].size() / 3 : md.indices.size() / 3, out.cullBack[m] ? "one side" : "        ",
                         md.shaderPath.c_str());
        }
    return true;
}
