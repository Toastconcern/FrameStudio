// Export to Blender (blender_export.h): the home and its walk layer as two glTF 2.0 binary files.
#include "blender_export.h"

#include "loaders/rendtxtr_parser.h"   // astc::decodeASTC / decodeASTCHalf / decodeCubeFaces
#include "spv_reflect.h"
#include "walkgrid.h"
#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

namespace fs = std::filesystem;

namespace blender {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// glTF constants
constexpr int kFloat = 5126, kUShort = 5123, kUInt = 5125;
constexpr int kArrayBuffer = 34962, kElementBuffer = 34963;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// ── Small maths (column-major 4x4, as Frame stores them) ──────────────────────────────────────────

void transformPoint(const float m[16], const float* p, float* o) {
    for (int r = 0; r < 3; ++r) o[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

// The inverse transpose of m's 3x3 (column-major 3x3): what turns normals. False when m flattens a direction.
bool normalMatrix(const float m[16], float n[9]) {
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) return false;
    const float k = 1.0f / det;
    // the inverse's transpose is the cofactor matrix over det; stored column-major
    n[0] = (e * i - f * h) * k; n[3] = -(d * i - f * g) * k; n[6] = (d * h - e * g) * k;
    n[1] = -(b * i - c * h) * k; n[4] = (a * i - c * g) * k; n[7] = -(a * h - b * g) * k;
    n[2] = (b * f - c * e) * k; n[5] = -(a * f - c * d) * k; n[8] = (a * e - b * d) * k;
    return true;
}

float determinant3(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

// A node transform: translation, rotation (quaternion xyzw) and scale.
struct Trs {
    float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
};

// m as translation * rotation * scale, which is what a glTF node holds. False when m shears or projects (a parent's
// non-uniform scale under a turned child): such an object's matrix is baked into its vertices instead.
bool decompose(const float m[16], Trs& out) {
    if (std::fabs(m[3]) > 1e-5f || std::fabs(m[7]) > 1e-5f || std::fabs(m[11]) > 1e-5f || std::fabs(m[15] - 1) > 1e-5f) return false;
    float c[3][3];
    for (int k = 0; k < 3; ++k)
        for (int r = 0; r < 3; ++r) c[k][r] = m[k * 4 + r];
    for (int k = 0; k < 3; ++k) {
        out.s[k] = std::sqrt(c[k][0] * c[k][0] + c[k][1] * c[k][1] + c[k][2] * c[k][2]);
        if (!(out.s[k] > 1e-8f) || !std::isfinite(out.s[k])) return false;
        for (int r = 0; r < 3; ++r) c[k][r] /= out.s[k];
    }
    if (determinant3(m) < 0) {                  // mirrored: a negative scale on x keeps the rotation proper
        out.s[0] = -out.s[0];
        for (int r = 0; r < 3; ++r) c[0][r] = -c[0][r];
    }
    auto dot = [&](int a, int b) { return c[a][0] * c[b][0] + c[a][1] * c[b][1] + c[a][2] * c[b][2]; };
    if (std::fabs(dot(0, 1)) > 1e-3f || std::fabs(dot(0, 2)) > 1e-3f || std::fabs(dot(1, 2)) > 1e-3f) return false;
    for (int r = 0; r < 3; ++r) out.t[r] = m[12 + r];
    // rotation matrix (row r, column k) = c[k][r] to a quaternion
    const float r00 = c[0][0], r11 = c[1][1], r22 = c[2][2], r01 = c[1][0], r10 = c[0][1], r02 = c[2][0], r20 = c[0][2], r12 = c[2][1], r21 = c[1][2];
    float* q = out.q;
    const float tr = r00 + r11 + r22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q[3] = 0.25f * s; q[0] = (r21 - r12) / s; q[1] = (r02 - r20) / s; q[2] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1 + r00 - r11 - r22) * 2;
        q[3] = (r21 - r12) / s; q[0] = 0.25f * s; q[1] = (r01 + r10) / s; q[2] = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1 + r11 - r00 - r22) * 2;
        q[3] = (r02 - r20) / s; q[0] = (r01 + r10) / s; q[1] = 0.25f * s; q[2] = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1 + r22 - r00 - r11) * 2;
        q[3] = (r10 - r01) / s; q[0] = (r02 + r20) / s; q[1] = (r12 + r21) / s; q[2] = 0.25f * s;
    }
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 0)) return false;
    for (int k = 0; k < 4; ++k) q[k] /= l;
    return true;
}

// Per-vertex normals from the faces round each vertex (area-weighted).
std::vector<float> faceNormals(const std::vector<float>& pos, const std::vector<u32>& idx) {
    std::vector<float> n(pos.size(), 0.0f);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const float* a = &pos[idx[t] * 3];
        const float* b = &pos[idx[t + 1] * 3];
        const float* c = &pos[idx[t + 2] * 3];
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float f[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 3; ++r) n[idx[t + k] * 3 + r] += f[r];
    }
    return n;
}

// Unit length in place; a zero normal becomes +Y.
void normalize3(float* n) {
    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 1e-12f && std::isfinite(l)) { n[0] /= l; n[1] /= l; n[2] /= l; }
    else { n[0] = 0; n[1] = 1; n[2] = 0; }
}

// ── JSON ──────────────────────────────────────────────────────────────────────────────────────────

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') { o += '\\'; o += ch; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += ch;
    }
    return o + "\"";
}
std::string number(double v) {
    if (!std::isfinite(v)) v = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}
std::string floats(const float* v, size_t n) {
    std::string o = "[";
    for (size_t i = 0; i < n; ++i) o += (i ? "," : "") + number(v[i]);
    return o + "]";
}
// "name":[a,b,...] or nothing when empty (glTF forbids empty arrays)
std::string jsonArray(const char* name, const std::vector<std::string>& items) {
    if (items.empty()) return "";
    std::string o = std::string(",\"") + name + "\":[";
    for (size_t i = 0; i < items.size(); ++i) o += (i ? "," : "") + items[i];
    return o + "]";
}

// ── PNG (deflated with miniz; each row with the filter that leaves the smallest bytes, as libpng picks) ───────────

void putBE32(std::vector<u8>& o, uint32_t v) {
    o.push_back(static_cast<u8>(v >> 24)); o.push_back(static_cast<u8>(v >> 16)); o.push_back(static_cast<u8>(v >> 8)); o.push_back(static_cast<u8>(v));
}
void pngChunk(std::vector<u8>& out, const char type[4], const u8* data, size_t n) {
    putBE32(out, static_cast<uint32_t>(n));
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    putBE32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, out.data() + at, n + 4)));
}

// rgba (w x h, 4 bytes a texel) as a PNG: RGB when alpha isn't wanted (or is opaque everywhere), else RGBA.
bool encodePng(const std::vector<u8>& rgba, u32 w, u32 h, bool keepAlpha, std::vector<u8>& out) {
    if (!w || !h || rgba.size() < static_cast<size_t>(w) * h * 4) return false;
    if (keepAlpha) {
        keepAlpha = false;
        for (size_t i = 3; i < rgba.size(); i += 4)
            if (rgba[i] != 255) { keepAlpha = true; break; }
    }
    const int ch = keepAlpha ? 4 : 3;
    const size_t stride = static_cast<size_t>(w) * ch;
    std::vector<u8> raw(static_cast<size_t>(h) * (stride + 1));
    std::vector<u8> cur(stride), prev(stride, 0), best(stride), cand(stride);
    for (u32 y = 0; y < h; ++y) {
        const u8* src = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (u32 x = 0; x < w; ++x)
            for (int c = 0; c < ch; ++c) cur[static_cast<size_t>(x) * ch + c] = src[static_cast<size_t>(x) * 4 + c];
        uint64_t bestCost = UINT64_MAX;
        u8 bestFilter = 0;
        for (u8 f = 0; f < 5; ++f) {
            uint64_t cost = 0;
            for (size_t i = 0; i < stride; ++i) {
                const int a = i >= static_cast<size_t>(ch) ? cur[i - ch] : 0, b = prev[i], c = i >= static_cast<size_t>(ch) ? prev[i - ch] : 0;
                int p = 0;
                switch (f) {
                    case 1: p = a; break;
                    case 2: p = b; break;
                    case 3: p = (a + b) / 2; break;
                    case 4: {
                        const int e = a + b - c, pa = std::abs(e - a), pb = std::abs(e - b), pc = std::abs(e - c);
                        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                        break;
                    }
                    default: break;
                }
                cand[i] = static_cast<u8>(cur[i] - p);
                cost += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(cand[i]))));
            }
            if (cost < bestCost) { bestCost = cost; bestFilter = f; best.swap(cand); }
        }
        u8* row = raw.data() + static_cast<size_t>(y) * (stride + 1);
        row[0] = bestFilter;
        std::memcpy(row + 1, best.data(), stride);
        prev.swap(cur);
    }
    mz_ulong zlen = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<u8> z(zlen);
    if (mz_compress2(z.data(), &zlen, raw.data(), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK) return false;
    out.clear();
    static const u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.insert(out.end(), kSignature, kSignature + 8);
    std::vector<u8> ihdr;
    putBE32(ihdr, w); putBE32(ihdr, h);
    ihdr.push_back(8);                                   // bits per channel
    ihdr.push_back(keepAlpha ? 6 : 2);                   // RGBA / RGB
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    pngChunk(out, "IHDR", ihdr.data(), ihdr.size());
    pngChunk(out, "IDAT", z.data(), zlen);
    pngChunk(out, "IEND", nullptr, 0);
    return true;
}

// ── Pixels ────────────────────────────────────────────────────────────────────────────────────────

struct Pixels {
    std::vector<u8> rgba;
    u32 w = 0, h = 0;
};

// Half the size (2x2 box filter; an odd edge folds its last row / column in).
void halve(Pixels& p) {
    const u32 nw = std::max(1u, p.w / 2), nh = std::max(1u, p.h / 2);
    std::vector<u8> o(static_cast<size_t>(nw) * nh * 4);
    for (u32 y = 0; y < nh; ++y)
        for (u32 x = 0; x < nw; ++x) {
            const u32 x0 = std::min(p.w - 1, x * 2), x1 = std::min(p.w - 1, x * 2 + 1), y0 = std::min(p.h - 1, y * 2), y1 = std::min(p.h - 1, y * 2 + 1);
            for (int c = 0; c < 4; ++c) {
                const unsigned s = p.rgba[(static_cast<size_t>(y0) * p.w + x0) * 4 + c] + p.rgba[(static_cast<size_t>(y0) * p.w + x1) * 4 + c] +
                                   p.rgba[(static_cast<size_t>(y1) * p.w + x0) * 4 + c] + p.rgba[(static_cast<size_t>(y1) * p.w + x1) * 4 + c];
                o[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<u8>((s + 2) / 4);
            }
        }
    p.rgba.swap(o);
    p.w = nw;
    p.h = nh;
}
void fitWithin(Pixels& p, unsigned maxSide) {
    while (std::max(p.w, p.h) > std::max(1u, maxSide)) halve(p);
}

u8 srgbByte(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return static_cast<u8>(std::lround((c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f) * 255.0f));
}
float srgbToLinear(u8 v) {
    const float c = v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Linear half-float RGBA as 8-bit sRGB colour (clamped to 0..1), alpha linear.
void fromHalf(const u16* half, u32 w, u32 h, bool opaque, Pixels& out) {
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t q = 0; q < out.rgba.size(); ++q) {
        const float v = astc::astcHalf2Float(half[q]);
        out.rgba[q] = (q & 3) == 3 ? (opaque ? 255 : static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f))) : srgbByte(v);
    }
}

// Where in t's chain the first level no bigger than maxSide is (a chain without one: mip 0, shrunk after).
bool levelWithin(const TexSource& t, unsigned maxSide, size_t& off, u32& w, u32& h, size_t& bytes) {
    if (!t.bw || !t.bh || !t.w || !t.h || t.astc.empty()) return false;
    auto levelBytes = [&](u32 lw, u32 lh) { return static_cast<size_t>((lw + t.bw - 1) / t.bw) * ((lh + t.bh - 1) / t.bh) * 16; };
    off = 0;
    w = t.w;
    h = t.h;
    for (u32 m = 0; m + 1 < t.mips && std::max(w, h) > maxSide; ++m) {
        const size_t next = off + levelBytes(w, h);
        if (next + levelBytes(std::max(1u, w / 2), std::max(1u, h / 2)) > t.astc.size()) break;
        off = next;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    bytes = std::min(levelBytes(w, h), t.astc.size() - off);
    // a texture cooked uncompressed (RGBA8 / RGBA16F, one level): astc::decodeASTC knows it by its exact size
    const size_t texels = static_cast<size_t>(t.w) * t.h;
    if (t.mips <= 1 && (t.astc.size() == texels * 4 || t.astc.size() == texels * 8)) bytes = t.astc.size();
    return bytes > 0;
}

// A colour texture (sRGB) as RGBA8, as the renderer's streaming workers decode it: the LDR profile, and the HDR one
// when that leaves it magenta (colour textures cooked HDR: the Focused vista's sky and water).
bool decodeColour(const TexSource& t, unsigned maxSide, Pixels& out) {
    if (t.faces != 1) return false;
    size_t off = 0, bytes = 0;
    u32 w = 0, h = 0;
    if (!levelWithin(t, maxSide, off, w, h, bytes)) return false;
    const u8* data = t.astc.data() + off;
    std::vector<u16> half;
    if (t.hdr) {
        if (!astc::decodeASTCHalf(data, static_cast<u32>(bytes), w, h, t.bw, t.bh, half) || half.size() < static_cast<size_t>(w) * h * 4) return false;
        fromHalf(half.data(), w, h, false, out);
    } else {
        if (!astc::decodeASTC(data, static_cast<u32>(bytes), w, h, t.bw, t.bh, out.rgba, false, 1) || out.rgba.size() < static_cast<size_t>(w) * h * 4)
            return false;
        out.w = w;
        out.h = h;
        size_t magenta = 0;
        for (size_t q = 0; q + 3 < out.rgba.size(); q += 4)
            if (out.rgba[q] > 250 && out.rgba[q + 1] < 8 && out.rgba[q + 2] > 250) ++magenta;
        if (magenta * 4 > static_cast<size_t>(w) * h && astc::decodeASTCHalf(data, static_cast<u32>(bytes), w, h, t.bw, t.bh, half) &&
            half.size() >= static_cast<size_t>(w) * h * 4)
            fromHalf(half.data(), w, h, false, out);
    }
    fitWithin(out, maxSide);
    return true;
}

// A baked HDR lightmap as 8-bit sRGB (radiance clamped to 0..1).
bool decodeHdrLightmap(const TexSource& t, unsigned maxSide, Pixels& out) {
    if (t.faces != 1) return false;
    size_t off = 0, bytes = 0;
    u32 w = 0, h = 0;
    if (!levelWithin(t, maxSide, off, w, h, bytes)) return false;
    std::vector<u16> half;
    if (!astc::decodeASTCHalf(t.astc.data() + off, static_cast<u32>(bytes), w, h, t.bw, t.bh, half) || half.size() < static_cast<size_t>(w) * h * 4)
        return false;
    fromHalf(half.data(), w, h, true, out);
    fitWithin(out, maxSide);
    return true;
}

// A cube (six RGBA8 faces of size^2: +X -X +Y -Y +Z -Z) unwrapped to an equirectangular panorama, 2:1. Its u runs
// round +Y from -Z (u = 0.5) through +X; v from straight up (0) to straight down (1). skyUv() is the same mapping.
void cubeToPanorama(const std::vector<u8>& faces, u32 size, unsigned maxSide, Pixels& out) {
    out.w = std::max(8u, std::min(std::max(1u, maxSide), size * 4));
    out.h = out.w / 2;
    out.rgba.resize(static_cast<size_t>(out.w) * out.h * 4);
    for (u32 y = 0; y < out.h; ++y) {
        const float lat = (0.5f - (y + 0.5f) / out.h) * kPi;
        for (u32 x = 0; x < out.w; ++x) {
            const float lon = ((x + 0.5f) / out.w - 0.5f) * 2 * kPi;
            const float d[3] = {std::sin(lon) * std::cos(lat), std::sin(lat), -std::cos(lon) * std::cos(lat)};
            // the face and its (s, t) as Vulkan samples a cube
            const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
            int face;
            float sc, tc, ma;
            if (ax >= ay && ax >= az) { face = d[0] > 0 ? 0 : 1; ma = ax; sc = d[0] > 0 ? -d[2] : d[2]; tc = -d[1]; }
            else if (ay >= az) { face = d[1] > 0 ? 2 : 3; ma = ay; sc = d[0]; tc = d[1] > 0 ? d[2] : -d[2]; }
            else { face = d[2] > 0 ? 4 : 5; ma = az; sc = d[2] > 0 ? d[0] : -d[0]; tc = -d[1]; }
            const float s = (sc / ma + 1) * 0.5f * size - 0.5f, t = (tc / ma + 1) * 0.5f * size - 0.5f;
            const int s0 = std::clamp(static_cast<int>(std::floor(s)), 0, static_cast<int>(size) - 1), t0 = std::clamp(static_cast<int>(std::floor(t)), 0, static_cast<int>(size) - 1);
            const int s1 = std::min(s0 + 1, static_cast<int>(size) - 1), t1 = std::min(t0 + 1, static_cast<int>(size) - 1);
            const float fs = std::clamp(s - s0, 0.0f, 1.0f), ft = std::clamp(t - t0, 0.0f, 1.0f);
            const u8* f = faces.data() + static_cast<size_t>(face) * size * size * 4;
            for (int c = 0; c < 4; ++c) {
                const float a = f[(static_cast<size_t>(t0) * size + s0) * 4 + c], b = f[(static_cast<size_t>(t0) * size + s1) * 4 + c];
                const float e = f[(static_cast<size_t>(t1) * size + s0) * 4 + c], g = f[(static_cast<size_t>(t1) * size + s1) * 4 + c];
                const float v = (a * (1 - fs) + b * fs) * (1 - ft) + (e * (1 - fs) + g * fs) * ft;
                out.rgba[(static_cast<size_t>(y) * out.w + x) * 4 + c] = static_cast<u8>(std::lround(std::clamp(v, 0.0f, 255.0f)));
            }
        }
    }
}

// Where a direction lands in cubeToPanorama's picture.
void skyUv(const float* d, float* uv) {
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 1e-12f)) { uv[0] = 0.5f; uv[1] = 0.5f; return; }
    uv[0] = std::atan2(d[0], -d[2]) / (2 * kPi) + 0.5f;
    uv[1] = 0.5f - std::asin(std::clamp(d[1] / l, -1.0f, 1.0f)) / kPi;
}

// ── Textures: each distinct image decoded and encoded once ──────────────────────────────────────────

struct TexJob {
    enum Kind { Colour, HdrLightmap, Rgba, HalfLightmap, Cube, CubeRgba };
    Kind kind = Colour;
    const TexSource* src = nullptr;               // Colour, HdrLightmap, Cube
    const std::vector<u8>* rgba = nullptr;        // Rgba, CubeRgba
    const std::vector<u16>* half = nullptr;       // HalfLightmap
    u32 w = 0, h = 0;                             // Rgba, HalfLightmap: its size; CubeRgba: the face size
    std::string name;
    bool alpha = false;                           // a material reads its alpha (blended or cut out)
    // the result
    int view = -1;                                // its PNG's bufferView, -1: couldn't be decoded
    bool translucent = false;                     // some texel's alpha is under 255
};

class TexTable {
public:
    // The job for this image (one per distinct source), made the first time it's asked for.
    int add(TexJob::Kind kind, const void* identity, uint64_t contentHash, const TexJob& job) {
        const auto key = std::make_tuple(static_cast<int>(kind), reinterpret_cast<uintptr_t>(identity), contentHash);
        auto it = index_.find(key);
        if (it != index_.end()) return it->second;
        jobs.push_back(job);
        jobs.back().kind = kind;
        return index_[key] = static_cast<int>(jobs.size() - 1);
    }
    std::vector<TexJob> jobs;

private:
    std::map<std::tuple<int, uintptr_t, uint64_t>, int> index_;
};

// The loader gives every mesh its own copy of a decoded texture: those are told apart by what's in them.
uint64_t contentHash(const std::vector<u8>& v, u32 w, u32 h) {
    uint64_t k = 1469598103934665603ull ^ (static_cast<uint64_t>(w) << 32 | h);
    size_t i = 0;
    for (; i + 8 <= v.size(); i += 8) {
        uint64_t x;
        std::memcpy(&x, v.data() + i, 8);
        k = (k ^ x) * 0x100000001B3ull;
        k ^= k >> 29;
    }
    for (; i < v.size(); ++i) k = (k ^ v[i]) * 1099511628211ull;
    return k;
}

bool decodeJob(const TexJob& j, unsigned maxSide, Pixels& px) {
    switch (j.kind) {
        case TexJob::Colour: return j.src && decodeColour(*j.src, maxSide, px);
        case TexJob::HdrLightmap: return j.src && decodeHdrLightmap(*j.src, maxSide, px);
        case TexJob::Rgba:
            if (!j.rgba || !j.w || !j.h || j.rgba->size() < static_cast<size_t>(j.w) * j.h * 4) return false;
            px.rgba.assign(j.rgba->begin(), j.rgba->begin() + static_cast<size_t>(j.w) * j.h * 4);
            px.w = j.w;
            px.h = j.h;
            fitWithin(px, maxSide);
            return true;
        case TexJob::HalfLightmap:
            if (!j.half || !j.w || !j.h || j.half->size() < static_cast<size_t>(j.w) * j.h * 4) return false;
            fromHalf(j.half->data(), j.w, j.h, true, px);
            fitWithin(px, maxSide);
            return true;
        case TexJob::Cube: {
            if (!j.src || j.src->faces != 6 || !j.src->w) return false;
            std::vector<u8> faces;
            if (!astc::decodeCubeFaces(j.src->astc.data(), static_cast<u32>(j.src->astc.size()), j.src->w, j.src->bw, j.src->bh, faces)) return false;
            cubeToPanorama(faces, j.src->w, maxSide, px);
            return true;
        }
        case TexJob::CubeRgba:
            if (!j.rgba || !j.w || j.rgba->size() < static_cast<size_t>(j.w) * j.w * 4 * 6) return false;
            cubeToPanorama(*j.rgba, j.w, maxSide, px);
            return true;
    }
    return false;
}

// ── The .glb being written: JSON pieces, and the binary chunk spooled to a file beside it ───────────────────

class Glb {
public:
    explicit Glb(std::string path) : path_(std::move(path)) {}
    ~Glb() {
        if (bin_.is_open()) bin_.close();
        std::error_code ec;
        if (!binPath_.empty()) fs::remove(binPath_, ec);
    }

    bool begin(std::string& error) {
        try {
            binPath_ = fs::u8path(path_ + ".bin.part");
        } catch (...) {
            error = "the file name isn't valid UTF-8: " + path_;
            return false;
        }
        bin_.open(binPath_, std::ios::binary | std::ios::trunc);
        if (!bin_) { error = "can't write beside " + path_; return false; }
        return true;
    }

    // Data appended to the binary chunk (4-byte aligned) as a bufferView; thread-safe. -1 when the write fails.
    int view(const void* data, size_t bytes, int target = 0) {
        std::lock_guard<std::mutex> l(mutex_);
        static const char kZero[4] = {0, 0, 0, 0};
        if (size_ % 4) { bin_.write(kZero, static_cast<std::streamsize>(4 - size_ % 4)); size_ += 4 - size_ % 4; }
        const size_t at = size_;
        bin_.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        if (!bin_) return -1;
        size_ += bytes;
        std::string v = "{\"buffer\":0,\"byteOffset\":" + std::to_string(at) + ",\"byteLength\":" + std::to_string(bytes);
        if (target) v += ",\"target\":" + std::to_string(target);
        views.push_back(v + "}");
        return static_cast<int>(views.size() - 1);
    }

    // An accessor over a whole bufferView. min / max: per component (POSITION needs them).
    int accessor(int view, int componentType, size_t count, const char* type, int components, const float* mn = nullptr, const float* mx = nullptr) {
        std::string a = "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(componentType) +
                        ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (mn && mx) a += ",\"min\":" + floats(mn, static_cast<size_t>(components)) + ",\"max\":" + floats(mx, static_cast<size_t>(components));
        accessors.push_back(a + "}");
        return static_cast<int>(accessors.size() - 1);
    }

    // Float vertex data (count x components) as an accessor; -1 on a write failure.
    int floatAttribute(const std::vector<float>& v, int components, const char* type, bool bounds = false) {
        const size_t count = v.size() / static_cast<size_t>(components);
        const int bv = view(v.data(), v.size() * sizeof(float), kArrayBuffer);
        if (bv < 0) return -1;
        if (!bounds) return accessor(bv, kFloat, count, type, components);
        float mn[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX}, mx[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (size_t i = 0; i < v.size(); ++i) {
            const int c = static_cast<int>(i % static_cast<size_t>(components));
            mn[c] = std::min(mn[c], v[i]);
            mx[c] = std::max(mx[c], v[i]);
        }
        return accessor(bv, kFloat, count, type, components, mn, mx);
    }

    // Triangle indices, 16-bit when every index fits.
    int indexAccessor(const std::vector<u32>& idx, size_t vertexCount) {
        if (vertexCount <= 0xFFFF) {
            std::vector<uint16_t> s(idx.size());
            for (size_t i = 0; i < idx.size(); ++i) s[i] = static_cast<uint16_t>(idx[i]);
            const int bv = view(s.data(), s.size() * 2, kElementBuffer);
            return bv < 0 ? -1 : accessor(bv, kUShort, s.size(), "SCALAR", 1);
        }
        const int bv = view(idx.data(), idx.size() * 4, kElementBuffer);
        return bv < 0 ? -1 : accessor(bv, kUInt, idx.size(), "SCALAR", 1);
    }

    struct Node {
        std::string name;
        int mesh = -1;
        Trs trs;
        std::vector<int> children;
        std::string extras;                       // a JSON object, or ""
    };
    int node(const std::string& name, int mesh = -1, const Trs& trs = Trs()) {
        nodes.push_back(Node{name, mesh, trs, {}, ""});
        return static_cast<int>(nodes.size() - 1);
    }
    void parent(int child, int to) {
        if (to < 0) roots.push_back(child);
        else nodes[static_cast<size_t>(to)].children.push_back(child);
    }

    // Writes the file: the JSON chunk, then the spooled binary chunk; replaces `path` only once it's whole.
    bool finish(const std::string& sceneName, const std::string& topLevel, std::string& error) {
        bin_.close();
        if (!bin_) { error = "writing " + path_ + " failed (disk full?)"; return false; }
        std::vector<std::string> nodeJson;
        for (const Node& n : nodes) {
            std::string j = "{\"name\":" + quoted(n.name);
            if (n.mesh >= 0) j += ",\"mesh\":" + std::to_string(n.mesh);
            const Trs& t = n.trs;
            if (t.t[0] != 0 || t.t[1] != 0 || t.t[2] != 0) j += ",\"translation\":" + floats(t.t, 3);
            if (t.q[0] != 0 || t.q[1] != 0 || t.q[2] != 0 || t.q[3] != 1) j += ",\"rotation\":" + floats(t.q, 4);
            if (t.s[0] != 1 || t.s[1] != 1 || t.s[2] != 1) j += ",\"scale\":" + floats(t.s, 3);
            if (!n.children.empty()) {
                j += ",\"children\":[";
                for (size_t i = 0; i < n.children.size(); ++i) j += (i ? "," : "") + std::to_string(n.children[i]);
                j += "]";
            }
            if (!n.extras.empty()) j += ",\"extras\":" + n.extras;
            nodeJson.push_back(j + "}");
        }
        std::string rootList;
        for (size_t i = 0; i < roots.size(); ++i) rootList += (i ? "," : "") + std::to_string(roots[i]);
        std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Frame Studio\"},\"scene\":0,\"scenes\":[{\"name\":" + quoted(sceneName) +
                           (roots.empty() ? std::string() : ",\"nodes\":[" + rootList + "]") + "}]";
        json += jsonArray("nodes", nodeJson) + jsonArray("meshes", meshes) + jsonArray("materials", materials) + topLevel +
                jsonArray("accessors", accessors) + jsonArray("bufferViews", views);
        if (size_) json += ",\"buffers\":[{\"byteLength\":" + std::to_string(size_) + "}]";
        json += "}";
        while (json.size() % 4) json += ' ';
        const size_t binPadded = (size_ + 3) / 4 * 4;
        const uint64_t total = 12 + 8 + json.size() + (size_ ? 8 + binPadded : 0);
        if (total > 0xFFFFFFFFull) { error = "the export is over 4 GB, more than a .glb can hold: lower the texture size"; return false; }

        fs::path out, part;
        try {
            out = fs::u8path(path_);
            part = fs::u8path(path_ + ".part");
        } catch (...) {
            error = "the file name isn't valid UTF-8: " + path_;
            return false;
        }
        {
            std::ofstream f(part, std::ios::binary | std::ios::trunc);
            std::ifstream in(binPath_, std::ios::binary);
            if (!f || !in) { error = "can't write " + path_; return false; }
            auto u32le = [&](uint32_t v) { const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, 4); };
            f.write("glTF", 4);
            u32le(2);
            u32le(static_cast<uint32_t>(total));
            u32le(static_cast<uint32_t>(json.size()));
            f.write("JSON", 4);
            f.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (size_) {
                u32le(static_cast<uint32_t>(binPadded));
                f.write("BIN\0", 4);
                std::vector<char> buf(4 << 20);
                while (in) {
                    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                    f.write(buf.data(), in.gcount());
                }
                static const char kZero[4] = {0, 0, 0, 0};
                f.write(kZero, static_cast<std::streamsize>(binPadded - size_));
            }
            f.flush();
            if (!f) {
                f.close();
                std::error_code ec;
                fs::remove(part, ec);
                error = "writing " + path_ + " failed (disk full?)";
                return false;
            }
        }
        std::error_code ec;
        fs::rename(part, out, ec);
        if (ec) {
            fs::remove(part, ec);
            error = "can't replace " + path_ + " (open in another program?)";
            return false;
        }
        return true;
    }

    std::vector<std::string> views, accessors, meshes, materials;
    std::vector<Node> nodes;
    std::vector<int> roots;

private:
    std::string path_;
    fs::path binPath_;
    std::ofstream bin_;
    size_t size_ = 0;
    std::mutex mutex_;
};

// A material as exported: what makes two meshes' materials the same one.
struct MaterialDesc {
    int base = -1, emissive = -1, lightmap = -1;  // TexTable jobs
    float factor[4] = {1, 1, 1, 1};
    int alphaMode = 0;                            // 0 OPAQUE, 1 MASK, 2 BLEND
    float cutoff = 0.5f;
    bool doubleSided = true;
    bool operator<(const MaterialDesc& o) const {
        return std::tie(base, emissive, lightmap, factor[0], factor[1], factor[2], factor[3], alphaMode, cutoff, doubleSided) <
               std::tie(o.base, o.emissive, o.lightmap, o.factor[0], o.factor[1], o.factor[2], o.factor[3], o.alphaMode, o.cutoff, o.doubleSided);
    }
};

// What a home's shader samples: its fragment stage's 2D image names (the material's, set 2, when it has one).
class ShaderSamplers {
public:
    explicit ShaderSamplers(const HslScene& scene) : scene_(scene) {}
    const std::vector<std::string>& of(const std::string& shaderPath) {
        auto it = cache_.find(shaderPath);
        if (it != cache_.end()) return it->second;
        std::vector<std::string>& names = cache_[shaderPath];
        const ShaderProgramSrc* p = findProgram(scene_, shaderPath);
        spv::Module m;
        if (!p || p->frag.empty() || !spv::reflect(p->frag, m)) return names;
        bool material = false;
        for (const spv::Resource& r : m.resources) material = material || r.set == 2;
        for (const spv::Resource& r : m.resources)
            if ((r.kind == spv::ResKind::SampledImage || r.kind == spv::ResKind::CombinedImageSampler) && r.dim == spv::ImageDim::D2 &&
                (!material || r.set == 2) && !r.name.empty())
                names.push_back(r.name);
        return names;
    }

private:
    const HslScene& scene_;
    std::map<std::string, std::vector<std::string>> cache_;
};

// The texture a material binds to this sampler (MeshData::namedTex: v206+ homes bind by sampler name, as the
// renderer's textureFor does).
const TexSource* boundTo(const MeshData& md, const std::string& sampler) {
    const u32 h = murmur3_x86_32(sampler.data(), sampler.size(), 0);
    for (const auto& nt : md.namedTex)
        if (nt.first == h && nt.second && nt.second->faces == 1) return nt.second.get();
    return nullptr;
}

// ...under the first sampler whose name has one of `words`.
const TexSource* namedTexture(const MeshData& md, const std::vector<std::string>& samplers, std::initializer_list<const char*> words) {
    for (const std::string& s : samplers) {
        const std::string n = lower(s);
        bool match = false;
        for (const char* w : words) match = match || has(n, w);
        if (!match) continue;
        if (const TexSource* t = boundTo(md, s)) return t;
    }
    return nullptr;
}

// A sampler that reads something other than the surface's colour: data maps (sampled linear by the renderer), baked or
// emitted light, and the runtime's own maps.
bool notColourSampler(const std::string& sampler) {
    const std::string n = lower(sampler);
    for (const char* w : {"normal", "onxrny", "orm", "rbao", "mask", "noise", "rough", "metal", "flow", "_ao", "lightmap", "emissive", "shadow",
                          "refl", "ibl", "vat", "cube", "pano"})
        if (has(n, w)) return true;
    return false;
}

// The base colour as the renderer draws it: the shader's BaseColor / albedo / diffuse sampler; else the loader's base
// map when the shader samples it; else the first colour sampler the material binds (palm leaves: detailTex, which
// carries the cut-out); else the loader's base map.
const TexSource* baseTexture(const MeshData& md, const std::vector<std::string>& samplers) {
    const TexSource* loaders = md.texSrc && md.texSrc->faces == 1 ? md.texSrc.get() : nullptr;
    if (md.namedTex.empty()) return loaders;
    if (const TexSource* t = namedTexture(md, samplers, {"basecolor", "albedo", "diffuse"})) return t;
    for (const std::string& s : samplers)
        if (loaders && boundTo(md, s) == loaders) return loaders;
    for (const std::string& s : samplers)
        if (!notColourSampler(s))
            if (const TexSource* t = boundTo(md, s)) return t;
    return loaders;
}

// Placeholders bound where a mesh has no baked light (renderer_module/missing_lightmap.png, 64x64 grey; white 4x4):
// nothing worth exporting. A real lightmap atlas is far bigger.
bool placeholderLightmap(const TexSource& t) { return t.w <= 64 && t.h <= 64; }

bool usableRgba(const std::vector<u8>& v, u32 w, u32 h) { return w && h && v.size() >= static_cast<size_t>(w) * h * 4; }

// The sky drawn from a cubemap: its colour cube is exported as a panorama, and the dome mapped onto it by direction.
bool skyFromCube(const MeshData& md) {
    return md.isSkybox && md.cubeSize && ((md.cubeSrc && md.cubeSrc->faces == 6) || md.cubeRGBA.size() >= static_cast<size_t>(md.cubeSize) * md.cubeSize * 24);
}

// A mesh's material: its textures as the renderer picks them, its tint, and how it's drawn.
MaterialDesc describeMaterial(const MeshData& md, const std::string& name, ShaderSamplers& samplers, TexTable& tex, const ExportOptions& opt) {
    MaterialDesc d;
    for (int c = 0; c < 4; ++c) d.factor[c] = std::clamp(std::isfinite(md.tint[c]) ? md.tint[c] : 1.0f, 0.0f, 1.0f);
    if (md.alphaTest || md.alphaToCoverage) { d.alphaMode = 1; d.cutoff = md.alphaTest ? std::clamp(md.alphaCutoff, 0.0f, 1.0f) : 0.5f; }
    else if (md.useBlend) d.alphaMode = 2;
    d.doubleSided = !md.cullBack;

    TexJob job;
    job.name = name;
    if (skyFromCube(md)) {
        if (md.cubeSrc && md.cubeSrc->faces == 6) { job.src = md.cubeSrc.get(); d.base = tex.add(TexJob::Cube, md.cubeSrc.get(), 0, job); }
        else { job.rgba = &md.cubeRGBA; job.w = md.cubeSize; d.base = tex.add(TexJob::CubeRgba, nullptr, contentHash(md.cubeRGBA, md.cubeSize, 6), job); }
        return d;
    }

    // base colour
    const std::vector<std::string> names = md.namedTex.empty() ? std::vector<std::string>() : samplers.of(md.shaderPath);
    const TexSource* base = baseTexture(md, names);
    if (base) {
        job.src = base;
        d.base = tex.add(TexJob::Colour, base, 0, job);
    } else if (md.hasTexture && usableRgba(md.texRGBA, md.texW, md.texH)) {
        if (static_cast<size_t>(md.texW) * md.texH == 1) {   // a flat colour (an untextured Clay part): into the factor
            for (int c = 0; c < 3; ++c) d.factor[c] *= srgbToLinear(md.texRGBA[static_cast<size_t>(c)]);
            d.factor[3] *= md.texRGBA[3] / 255.0f;
        } else {
            job.rgba = &md.texRGBA; job.w = md.texW; job.h = md.texH;
            d.base = tex.add(TexJob::Rgba, nullptr, contentHash(md.texRGBA, md.texW, md.texH), job);
        }
    }
    if (d.base >= 0 && d.alphaMode != 0) tex.jobs[static_cast<size_t>(d.base)].alpha = true;

    // emissive
    job = TexJob();
    job.name = name + " emissive";
    const TexSource* emissive = md.namedTex.empty() ? nullptr : namedTexture(md, names, {"emissive"});
    if (!emissive && md.namedTex.empty() && md.hasEmissive && md.emissiveSrc) emissive = md.emissiveSrc.get();
    if (emissive) {
        job.src = emissive;
        d.emissive = tex.add(TexJob::Colour, emissive, 0, job);
    } else if (md.namedTex.empty() && md.hasEmissive && usableRgba(md.emissiveRGBA, md.emissiveW, md.emissiveH)) {
        job.rgba = &md.emissiveRGBA; job.w = md.emissiveW; job.h = md.emissiveH;
        d.emissive = tex.add(TexJob::Rgba, nullptr, contentHash(md.emissiveRGBA, md.emissiveW, md.emissiveH), job);
    }

    // the baked lightmap (on UV1), when there's a UV1 to read it with
    job = TexJob();
    job.name = name + " lightmap";
    if (opt.lightmaps && md.uvs2.size() >= md.positions.size() / 3 * 2 && !md.positions.empty()) {
        // as the renderer binds a lightmap slot: the baked HDR one, else the material's own by sampler name, else Clay's
        const TexSource* named = md.namedTex.empty() ? nullptr : namedTexture(md, names, {"lightmap"});
        if (named && placeholderLightmap(*named)) named = nullptr;
        if (md.lmSrc && md.lmSrc->faces == 1) { job.src = md.lmSrc.get(); d.lightmap = tex.add(TexJob::HdrLightmap, md.lmSrc.get(), 0, job); }
        else if (md.lmHalf && md.lmW && md.lmH) { job.half = md.lmHalf.get(); job.w = md.lmW; job.h = md.lmH; d.lightmap = tex.add(TexJob::HalfLightmap, md.lmHalf.get(), 0, job); }
        else if (named) { job.src = named; d.lightmap = tex.add(named->hdr ? TexJob::HdrLightmap : TexJob::Colour, named, 0, job); }
        else if (md.lmLdrSrc && md.lmLdrSrc->faces == 1) { job.src = md.lmLdrSrc.get(); d.lightmap = tex.add(TexJob::Colour, md.lmLdrSrc.get(), 0, job); }
        else if (md.hasLightmap && usableRgba(md.lmRGBA, md.lmW, md.lmH)) {
            job.rgba = &md.lmRGBA; job.w = md.lmW; job.h = md.lmH;
            d.lightmap = tex.add(TexJob::Rgba, nullptr, contentHash(md.lmRGBA, md.lmW, md.lmH), job);
        }
    }
    return d;
}

// The material's glTF. A blended or cut-out material that's opaque everywhere (its base texture's alpha all 255, or
// no texture, and its factor's alpha 1: most of a home's "blended" floors and walls) is written OPAQUE: it looks the
// same, and Blender draws blended materials unsorted against each other.
std::string materialJson(const std::string& name, MaterialDesc d, const std::vector<int>& textureOf, const std::vector<TexJob>& jobs) {
    auto texture = [&](int job) { return job >= 0 && job < static_cast<int>(textureOf.size()) ? textureOf[static_cast<size_t>(job)] : -1; };
    const bool translucentBase = texture(d.base) >= 0 && jobs[static_cast<size_t>(d.base)].translucent;
    if (d.alphaMode != 0 && d.factor[3] >= 1 && !translucentBase) d.alphaMode = 0;
    std::string j = "{\"name\":" + quoted(name) + ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + floats(d.factor, 4);
    if (texture(d.base) >= 0) j += ",\"baseColorTexture\":{\"index\":" + std::to_string(texture(d.base)) + "}";
    j += ",\"metallicFactor\":0,\"roughnessFactor\":1}";
    if (texture(d.emissive) >= 0) j += ",\"emissiveTexture\":{\"index\":" + std::to_string(texture(d.emissive)) + "},\"emissiveFactor\":[1,1,1]";
    if (texture(d.lightmap) >= 0) j += ",\"occlusionTexture\":{\"index\":" + std::to_string(texture(d.lightmap)) + ",\"texCoord\":1}";
    if (d.alphaMode == 1) j += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":" + number(d.cutoff);
    else if (d.alphaMode == 2) j += ",\"alphaMode\":\"BLEND\"";
    if (d.doubleSided) j += ",\"doubleSided\":true";
    return j + "}";
}

// A plain material for the walk layer's helpers.
std::string flatMaterial(const char* name, float r, float g, float b, float a) {
    const float c[4] = {r, g, b, a};
    return std::string("{\"name\":") + quoted(name) + ",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + floats(c, 4) +
           ",\"metallicFactor\":0,\"roughnessFactor\":1},\"alphaMode\":\"BLEND\",\"doubleSided\":true}";
}

// One triangle mesh's vertex data, ready to be written.
struct MeshBuild {
    std::vector<float> pos, nrm, uv0, uv1;
    std::vector<u32> idx;
};

// A primitive from a build; -1 when nothing could be written.
int addMesh(Glb& glb, const std::string& name, const MeshBuild& b, int material) {
    const size_t n = b.pos.size() / 3;
    const int p = glb.floatAttribute(b.pos, 3, "VEC3", true);
    const int nr = glb.floatAttribute(b.nrm, 3, "VEC3");
    const int t0 = b.uv0.size() == n * 2 ? glb.floatAttribute(b.uv0, 2, "VEC2") : -1;
    const int t1 = b.uv1.size() == n * 2 ? glb.floatAttribute(b.uv1, 2, "VEC2") : -1;
    const int ix = glb.indexAccessor(b.idx, n);
    if (p < 0 || nr < 0 || ix < 0) return -1;
    std::string attrs = "\"POSITION\":" + std::to_string(p) + ",\"NORMAL\":" + std::to_string(nr);
    if (t0 >= 0) attrs += ",\"TEXCOORD_0\":" + std::to_string(t0);
    if (t1 >= 0) attrs += ",\"TEXCOORD_1\":" + std::to_string(t1);
    std::string prim = "{\"attributes\":{" + attrs + "},\"indices\":" + std::to_string(ix);
    if (material >= 0) prim += ",\"material\":" + std::to_string(material);
    glb.meshes.push_back("{\"name\":" + quoted(name) + ",\"primitives\":[" + prim + "}]}");
    return static_cast<int>(glb.meshes.size() - 1);
}

// The mesh's vertices as exported: `bake` (non-null) is a matrix applied to them (one a node can't hold); the
// lightmap's atlas transform is applied to UV1 when the material reads its lightmap there. False: no triangles.
bool buildMesh(const MeshData& md, const float* bake, bool lightmapUv, MeshBuild& b) {
    const size_t n = md.positions.size() / 3;
    if (!n) return false;
    b.idx.reserve(md.indices.size());
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3)
        if (md.indices[t] < n && md.indices[t + 1] < n && md.indices[t + 2] < n) b.idx.insert(b.idx.end(), md.indices.begin() + t, md.indices.begin() + t + 3);
    if (b.idx.empty()) return false;
    b.pos.assign(md.positions.begin(), md.positions.begin() + n * 3);
    // normals: the authored ones, the faces' where a vertex has none
    b.nrm.assign(n * 3, 0.0f);
    const bool authored = md.normals.size() >= n * 3;
    bool missing = !authored;
    if (authored)
        for (size_t v = 0; v < n; ++v) {
            const float* a = &md.normals[v * 3];
            if (a[0] * a[0] + a[1] * a[1] + a[2] * a[2] > 1e-8f) std::copy(a, a + 3, &b.nrm[v * 3]);
            else missing = true;
        }
    if (missing) {
        const std::vector<float> f = faceNormals(b.pos, b.idx);
        for (size_t v = 0; v < n; ++v)
            if (b.nrm[v * 3] == 0 && b.nrm[v * 3 + 1] == 0 && b.nrm[v * 3 + 2] == 0) std::copy(&f[v * 3], &f[v * 3] + 3, &b.nrm[v * 3]);
    }
    if (bake) {
        float nm[9];
        const bool turnNormals = normalMatrix(bake, nm);
        for (size_t v = 0; v < n; ++v) {
            float o[3];
            transformPoint(bake, &b.pos[v * 3], o);
            std::copy(o, o + 3, &b.pos[v * 3]);
            if (turnNormals) {
                const float* a = &b.nrm[v * 3];
                const float r[3] = {nm[0] * a[0] + nm[3] * a[1] + nm[6] * a[2], nm[1] * a[0] + nm[4] * a[1] + nm[7] * a[2], nm[2] * a[0] + nm[5] * a[1] + nm[8] * a[2]};
                std::copy(r, r + 3, &b.nrm[v * 3]);
            }
        }
        if (determinant3(bake) < 0)                // mirrored: the front stays the front
            for (size_t t = 0; t + 2 < b.idx.size(); t += 3) std::swap(b.idx[t + 1], b.idx[t + 2]);
    }
    for (size_t v = 0; v < n; ++v) normalize3(&b.nrm[v * 3]);
    if (md.uvs.size() >= n * 2) b.uv0.assign(md.uvs.begin(), md.uvs.begin() + n * 2);
    if (md.uvs2.size() >= n * 2) {
        b.uv1.assign(md.uvs2.begin(), md.uvs2.begin() + n * 2);
        if (lightmapUv)                            // where this mesh's patch sits in a shared lightmap atlas
            for (size_t v = 0; v < n; ++v) {
                b.uv1[v * 2] = b.uv1[v * 2] * md.lmUvScaleOffset[0] + md.lmUvScaleOffset[2];
                b.uv1[v * 2 + 1] = b.uv1[v * 2 + 1] * md.lmUvScaleOffset[1] + md.lmUvScaleOffset[3];
            }
    }
    for (float& f : b.uv0) if (!std::isfinite(f)) f = 0;
    for (float& f : b.uv1) if (!std::isfinite(f)) f = 0;
    for (float& f : b.pos) if (!std::isfinite(f)) f = 0;
    return true;
}

// The sky dome mapped onto its cube's panorama by direction from its middle (`world`: the dome's turn), with the
// triangles across the panorama's seam given their own vertices past u = 1 (the texture repeats) so none smear.
bool buildSky(const MeshData& md, const float* world, const float* bake, MeshBuild& b) {
    MeshBuild plain;
    if (!buildMesh(md, bake, false, plain)) return false;
    const size_t n = plain.pos.size() / 3;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t v = 0; v < n; ++v)
        for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], plain.pos[v * 3 + c]); hi[c] = std::max(hi[c], plain.pos[v * 3 + c]); }
    const float mid[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    std::vector<float> uv(n * 2);
    for (size_t v = 0; v < n; ++v) {
        const float l[3] = {plain.pos[v * 3] - mid[0], plain.pos[v * 3 + 1] - mid[1], plain.pos[v * 3 + 2] - mid[2]};
        float d[3];                                // turned as the dome is in the world (baked already: as it is)
        if (bake) std::copy(l, l + 3, d);
        else for (int r = 0; r < 3; ++r) d[r] = world[r] * l[0] + world[4 + r] * l[1] + world[8 + r] * l[2];
        skyUv(d, &uv[v * 2]);
    }
    b.pos = plain.pos;
    b.nrm = plain.nrm;
    b.uv0 = uv;
    for (size_t t = 0; t + 2 < plain.idx.size(); t += 3) {
        u32 tri[3] = {plain.idx[t], plain.idx[t + 1], plain.idx[t + 2]};
        float umin = 1, umax = 0;
        for (u32 v : tri) { umin = std::min(umin, uv[v * 2]); umax = std::max(umax, uv[v * 2]); }
        if (umax - umin > 0.5f)                    // across the seam: its low side moved past 1
            for (u32& v : tri)
                if (uv[v * 2] < 0.5f) {
                    const u32 copy = static_cast<u32>(b.pos.size() / 3);
                    b.pos.insert(b.pos.end(), &plain.pos[v * 3], &plain.pos[v * 3] + 3);
                    b.nrm.insert(b.nrm.end(), &plain.nrm[v * 3], &plain.nrm[v * 3] + 3);
                    b.uv0.push_back(uv[v * 2] + 1);
                    b.uv0.push_back(uv[v * 2 + 1]);
                    v = copy;
                }
        b.idx.insert(b.idx.end(), tri, tri + 3);
    }
    return true;
}

}  // namespace

// ── The environment ───────────────────────────────────────────────────────────────────────────────

bool exportScene(const HslScene& scene, const std::vector<ExportObject>& objects, const std::string& path, std::string& error,
                 const ExportOptions& opt, ExportStats* statsOut) {
    ExportStats stats;
    auto progress = [&](float f, const char* what) { if (opt.progress) opt.progress(f, what); };
    Glb glb(path);
    if (!glb.begin(error)) return false;

    // Every object's material first (which textures, which look), shared by look.
    progress(0, "Materials");
    ShaderSamplers samplers(scene);
    TexTable tex;
    std::map<int, MaterialDesc> descOf;            // by scene mesh
    std::map<MaterialDesc, int> materialOf;
    std::vector<MaterialDesc> materials;
    std::vector<std::string> materialNames;
    std::map<int, int> materialOfMesh;
    for (const ExportObject& o : objects) {
        if (o.mesh < 0 || static_cast<size_t>(o.mesh) >= scene.meshes.size() || descOf.count(o.mesh)) continue;
        const MeshData& md = scene.meshes[static_cast<size_t>(o.mesh)];
        if (md.positions.size() < 9 || md.indices.size() < 3) continue;   // nothing to draw: no material for it
        const std::string name = md.isSkybox ? std::string("Sky") : o.name.empty() ? md.name : o.name;
        const MaterialDesc d = describeMaterial(md, name, samplers, tex, opt);
        descOf[o.mesh] = d;
        auto it = materialOf.find(d);
        if (it == materialOf.end()) {
            it = materialOf.emplace(d, static_cast<int>(materials.size())).first;
            materials.push_back(d);
            materialNames.push_back(name);
        }
        materialOfMesh[o.mesh] = it->second;
    }

    // The nodes and meshes: a node per folder, one per object; a scene mesh used by several objects written once.
    progress(0.02f, "Meshes");
    size_t skies = 0;
    for (const ExportObject& o : objects)
        if (o.mesh >= 0 && static_cast<size_t>(o.mesh) < scene.meshes.size() && scene.meshes[static_cast<size_t>(o.mesh)].isSkybox) ++skies;
    int skyGroup = -1;
    if (skies > 1) { skyGroup = glb.node("Sky"); glb.parent(skyGroup, -1); }
    std::map<std::string, int> folderNode;
    auto folderOf = [&](const std::vector<std::string>& folders) {
        int at = -1;
        std::string key;
        for (const std::string& f : folders) {
            if (f.empty()) continue;
            key += f + '\x1f';
            auto it = folderNode.find(key);
            if (it == folderNode.end()) {
                const int n = glb.node(f);
                glb.parent(n, at);
                it = folderNode.emplace(key, n).first;
            }
            at = it->second;
        }
        return at;
    };
    std::map<int, int> meshOf;                     // scene mesh -> glTF mesh (not baked)
    for (float& v : stats.lo) v = FLT_MAX;
    for (float& v : stats.hi) v = -FLT_MAX;
    size_t done = 0;
    for (const ExportObject& o : objects) {
        if (opt.progress && ++done % 64 == 0) progress(0.02f + 0.18f * done / objects.size(), "Meshes");
        if (o.mesh < 0 || static_cast<size_t>(o.mesh) >= scene.meshes.size()) { ++stats.skipped; continue; }
        const MeshData& md = scene.meshes[static_cast<size_t>(o.mesh)];
        Trs trs;
        const bool asNode = decompose(o.world, trs);
        const bool sky = skyFromCube(md);
        const int material = materialOfMesh.count(o.mesh) ? materialOfMesh[o.mesh] : -1;
        const MaterialDesc& desc = descOf[o.mesh];
        int mesh = -1;
        if (asNode && meshOf.count(o.mesh)) mesh = meshOf[o.mesh];
        else {
            MeshBuild b;
            const bool built = sky ? buildSky(md, o.world, asNode ? nullptr : o.world, b) : buildMesh(md, asNode ? nullptr : o.world, desc.lightmap >= 0, b);
            if (!built) { ++stats.skipped; continue; }
            mesh = addMesh(glb, md.isSkybox ? std::string("Sky") : o.name.empty() ? md.name : o.name, b, material);
            if (mesh < 0) { error = "writing " + path + " failed (disk full?)"; return false; }
            stats.triangles += b.idx.size() / 3;
            if (asNode) meshOf[o.mesh] = mesh;
            else trs = Trs();
        }
        {   // world bounds
            const size_t n = md.positions.size() / 3;
            for (size_t v = 0; v < n; ++v) {
                float w[3];
                transformPoint(o.world, &md.positions[v * 3], w);
                for (int c = 0; c < 3; ++c)
                    if (std::isfinite(w[c])) { stats.lo[c] = std::min(stats.lo[c], w[c]); stats.hi[c] = std::max(stats.hi[c], w[c]); }
            }
        }
        if (md.isSkybox) {
            const int n = glb.node(skies > 1 ? (o.name.empty() ? md.name : o.name) : std::string("Sky"), mesh, trs);
            glb.parent(n, skyGroup);
        } else {
            const int n = glb.node(o.name.empty() ? md.name : o.name, mesh, trs);
            glb.parent(n, folderOf(o.folders));
        }
    }
    if (stats.lo[0] > stats.hi[0]) for (int c = 0; c < 3; ++c) stats.lo[c] = stats.hi[c] = 0;

    // The textures: decoded and encoded on several threads, each PNG appended as it's done.
    progress(0.2f, "Textures");
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::max(1u, std::min<unsigned>(opt.threads ? opt.threads : std::min(hw, 8u), static_cast<unsigned>(std::max<size_t>(1, tex.jobs.size()))));
    std::atomic<size_t> next{0}, finished{0};
    auto work = [&](bool report) {
        for (;;) {
            const size_t i = next++;
            if (i >= tex.jobs.size()) return;
            TexJob& j = tex.jobs[i];
            Pixels px;
            std::vector<u8> png;
            if (decodeJob(j, std::max(1u, opt.maxTextureSide), px)) {
                for (size_t q = 3; q < px.rgba.size() && !j.translucent; q += 4) j.translucent = px.rgba[q] != 255;
                if (encodePng(px.rgba, px.w, px.h, j.alpha, png)) j.view = glb.view(png.data(), png.size());
            }
            ++finished;
            if (report) progress(0.2f + 0.75f * finished / std::max<size_t>(1, tex.jobs.size()), "Textures");
        }
    };
    {
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, false);
        work(true);                                // (this thread too: progress is reported from it)
        for (std::thread& t : pool) t.join();
    }
    std::vector<int> textureOf(tex.jobs.size(), -1);
    std::vector<std::string> images, textures;
    for (size_t i = 0; i < tex.jobs.size(); ++i) {
        const TexJob& j = tex.jobs[i];
        if (j.view < 0) { ++stats.imagesFailed; continue; }
        textureOf[i] = static_cast<int>(textures.size());
        images.push_back("{\"name\":" + quoted(j.name) + ",\"mimeType\":\"image/png\",\"bufferView\":" + std::to_string(j.view) + "}");
        textures.push_back("{\"sampler\":0,\"source\":" + std::to_string(images.size() - 1) + "}");
    }
    for (size_t m = 0; m < materials.size(); ++m) glb.materials.push_back(materialJson(materialNames[m], materials[m], textureOf, tex.jobs));

    progress(0.95f, "Writing");
    std::string top = jsonArray("textures", textures) + jsonArray("images", images);
    if (!textures.empty()) top += ",\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}]";
    std::string sceneName = "Home";
    try {
        if (!scene.apkPaths.empty()) sceneName = fs::u8path(scene.apkPaths[0]).stem().u8string();
    } catch (...) {
    }
    if (!glb.finish(sceneName, top, error)) return false;
    stats.nodes = glb.nodes.size();
    stats.meshes = glb.meshes.size();
    stats.materials = glb.materials.size();
    stats.images = images.size();
    progress(1, "Done");
    if (statsOut) {
        ExportStats& s = *statsOut;
        s.nodes = stats.nodes; s.meshes = stats.meshes; s.materials = stats.materials; s.images = stats.images; s.triangles = stats.triangles;
        s.skipped = stats.skipped; s.imagesFailed = stats.imagesFailed;
        std::copy(stats.lo, stats.lo + 3, s.lo);
        std::copy(stats.hi, stats.hi + 3, s.hi);
    }
    return true;
}

// ── The walk layer ────────────────────────────────────────────────────────────────────────────────

namespace {

// A box of half extents h round the origin: four vertices a face, so each face is flat.
void boxMesh(const float* h, MeshBuild& b) {
    static const int kAxis[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1}};   // normal, u, v
    for (int f = 0; f < 6; ++f) {
        const int n = kAxis[f][0], u = kAxis[f][1], v = kAxis[f][2];
        const float sign = f % 2 ? -1.0f : 1.0f;
        const u32 at = static_cast<u32>(b.pos.size() / 3);
        static const float kCorner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : kCorner) {
            float p[3], nr[3] = {0, 0, 0};
            p[n] = sign * h[n];
            p[u] = c[0] * h[u];
            p[v] = c[1] * h[v];
            nr[n] = sign;
            b.pos.insert(b.pos.end(), p, p + 3);
            b.nrm.insert(b.nrm.end(), nr, nr + 3);
        }
        // anticlockwise seen from outside (u x v = n on the + faces)
        if (sign > 0) b.idx.insert(b.idx.end(), {at, at + 1, at + 2, at, at + 2, at + 3});
        else b.idx.insert(b.idx.end(), {at, at + 2, at + 1, at, at + 3, at + 2});
    }
}

// A capsule along Y: radius r, its two hemispheres' centres `length` apart (a sphere at 0).
void capsuleMesh(float r, float length, MeshBuild& b) {
    constexpr int kSides = 16, kRings = 4;         // rings per hemisphere
    const float half = std::max(0.0f, length) * 0.5f;
    // rows from the top pole to the bottom one: the upper hemisphere raised by half, the lower lowered
    for (int hemi = 0; hemi < 2; ++hemi)
        for (int i = 0; i <= kRings; ++i) {
            const float lat = hemi == 0 ? kPi / 2 * (1 - static_cast<float>(i) / kRings) : -kPi / 2 * static_cast<float>(i) / kRings;
            const float y = r * std::sin(lat) + (hemi == 0 ? half : -half), ring = r * std::cos(lat);
            for (int s = 0; s <= kSides; ++s) {
                const float lon = 2 * kPi * s / kSides;
                const float n[3] = {std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
                b.pos.insert(b.pos.end(), {ring * std::cos(lon), y, ring * std::sin(lon)});
                b.nrm.insert(b.nrm.end(), n, n + 3);
            }
        }
    const u32 rows = 2 * (kRings + 1), cols = kSides + 1;
    for (u32 y = 0; y + 1 < rows; ++y)
        for (u32 x = 0; x < static_cast<u32>(kSides); ++x) {
            const u32 a = y * cols + x, c = a + cols;
            b.idx.insert(b.idx.end(), {a, a + 1, c + 1, a, c + 1, c});
        }
}

const char* kindName(int k) {
    switch (k) {
        case WorldCollider::Box: return "box";
        case WorldCollider::Sphere: return "sphere";
        case WorldCollider::Capsule: return "capsule";
        default: return "mesh";
    }
}
const char* roleName(int r) {
    switch (r) {
        case WorldCollider::Trigger: return "trigger";
        case WorldCollider::Object: return "object";
        case WorldCollider::Walkable: return "walkable";
        case WorldCollider::Navmesh: return "navmesh";
        default: return "solid";
    }
}

// A spawn / seat / hotspot: an empty at its place, turned about +Y by its yaw (0 faces -Z, as in the world).
int pointNode(Glb& glb, const std::string& name, const WorldPoint& p) {
    Trs t;
    std::copy(p.pos, p.pos + 3, t.t);
    t.q[1] = std::sin(p.yaw * 0.5f);
    t.q[3] = std::cos(p.yaw * 0.5f);
    const int n = glb.node(name, -1, t);
    glb.nodes[static_cast<size_t>(n)].extras = "{\"yaw\":" + number(p.yaw) + "}";
    return n;
}

}  // namespace

bool exportNavmesh(const WalkGrid* walk, const HomeWorld& world, const std::string& path, std::string& error, ExportStats* stats) {
    Glb glb(path);
    if (!glb.begin(error)) return false;
    glb.materials.push_back(flatMaterial("Navmesh", 0.10f, 0.78f, 0.30f, 0.45f));
    glb.materials.push_back(flatMaterial("Collider", 0.90f, 0.20f, 0.15f, 0.30f));
    glb.materials.push_back(flatMaterial("Trigger", 0.95f, 0.80f, 0.10f, 0.25f));
    size_t navTris = 0, navVerts = 0;

    // The walkable area: WalkGrid::outline's triangles (Studio's green overlay), welded (0.1 mm) into one mesh whose
    // faces all face up.
    if (walk) {
        MeshBuild b;
        std::map<std::tuple<long long, long long, long long>, u32> weld;
        auto vertex = [&](const float* p) {
            const auto key = std::make_tuple(std::llround(p[0] * 1e4f), std::llround(p[1] * 1e4f), std::llround(p[2] * 1e4f));
            auto it = weld.find(key);
            if (it != weld.end()) return it->second;
            const u32 i = static_cast<u32>(b.pos.size() / 3);
            b.pos.insert(b.pos.end(), p, p + 3);
            weld.emplace(key, i);
            return i;
        };
        walk->outline(
            [&](const float* a, const float* bb, const float* c, bool) {
                u32 t[3] = {vertex(a), vertex(bb), vertex(c)};
                if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) return;
                const float ny = (bb[2] - a[2]) * (c[0] - a[0]) - (bb[0] - a[0]) * (c[2] - a[2]);   // (b - a) x (c - a), y
                if (ny < 0) std::swap(t[1], t[2]);
                b.idx.insert(b.idx.end(), t, t + 3);
            },
            [](const float*, const float*) {});
        if (!b.idx.empty()) {
            b.nrm = faceNormals(b.pos, b.idx);
            for (size_t v = 0; v < b.pos.size() / 3; ++v) normalize3(&b.nrm[v * 3]);
            const int mesh = addMesh(glb, "Navmesh", b, 0);
            if (mesh < 0) { error = "writing " + path + " failed (disk full?)"; return false; }
            glb.parent(glb.node("Navmesh", mesh), -1);
            navTris = b.idx.size() / 3;
            navVerts = b.pos.size() / 3;
        }
    }

    // The colliders, each a mesh in its own frame.
    size_t colliders = 0;
    if (!world.colliders.empty()) {
        const int group = glb.node("Colliders");
        glb.parent(group, -1);
        for (size_t i = 0; i < world.colliders.size(); ++i) {
            const WorldCollider& c = world.colliders[i];
            MeshBuild b;
            switch (c.kind) {
                case WorldCollider::Box: boxMesh(c.half, b); break;
                case WorldCollider::Sphere: capsuleMesh(c.radius, 0, b); break;
                case WorldCollider::Capsule: capsuleMesh(c.radius, c.length, b); break;
                default: {
                    const size_t n = c.positions.size() / 3;
                    b.pos.assign(c.positions.begin(), c.positions.begin() + n * 3);
                    for (size_t t = 0; t + 2 < c.indices.size(); t += 3)
                        if (c.indices[t] < n && c.indices[t + 1] < n && c.indices[t + 2] < n) b.idx.insert(b.idx.end(), c.indices.begin() + t, c.indices.begin() + t + 3);
                    b.nrm = faceNormals(b.pos, b.idx);
                    for (size_t v = 0; v < n; ++v) normalize3(&b.nrm[v * 3]);
                }
            }
            if (b.idx.empty()) continue;
            Trs trs;
            if (!decompose(c.world, trs)) {              // sheared: the frame goes into the vertices
                MeshData md;
                md.positions = b.pos;
                md.normals = b.nrm;
                md.indices = b.idx;
                b = MeshBuild();
                if (!buildMesh(md, c.world, false, b)) continue;
                trs = Trs();
            }
            const std::string name = c.name.empty() ? "Collider " + std::to_string(i + 1) : c.name;
            const int mesh = addMesh(glb, name, b, c.trigger() ? 2 : 1);
            if (mesh < 0) { error = "writing " + path + " failed (disk full?)"; return false; }
            const int n = glb.node(name, mesh, trs);
            std::string extras = std::string("{\"kind\":\"") + kindName(c.kind) + "\",\"role\":\"" + roleName(c.role) + "\"";
            if (c.kind == WorldCollider::Box) extras += ",\"half\":" + floats(c.half, 3);
            if (c.kind == WorldCollider::Sphere || c.kind == WorldCollider::Capsule) extras += ",\"radius\":" + number(c.radius);
            if (c.kind == WorldCollider::Capsule) extras += ",\"length\":" + number(c.length);
            glb.nodes[static_cast<size_t>(n)].extras = extras + "}";
            glb.parent(n, group);
            ++colliders;
        }
    }

    // Spawns, seats and hotspots: empties.
    size_t points = 0;
    auto pointGroup = [&](const char* group, const std::vector<WorldPoint>& list, const char* label) {
        if (list.empty()) return;
        const int g = glb.node(group);
        glb.parent(g, -1);
        for (size_t i = 0; i < list.size(); ++i) {
            const WorldPoint& p = list[i];
            std::string name = label;
            if (!p.name.empty()) name += " " + p.name;
            else if (i) name += " " + std::to_string(i + 1);
            const int n = pointNode(glb, name, p);
            glb.parent(n, g);
            if (std::hypot(p.exit[0], p.exit[2]) > 1e-3f || std::fabs(p.exit[1]) > 1e-3f) {   // a seat's exit: where standing up puts you
                Trs t;
                std::copy(p.exit, p.exit + 3, t.t);
                glb.parent(glb.node(name + " exit", -1, t), n);
            }
            ++points;
        }
    };
    pointGroup("Spawns", world.spawns, "Spawn");
    pointGroup("Seats", world.seats, "Seat");
    pointGroup("Hotspots", world.hotspots, "Hotspot");

    if (!glb.finish("Walk layer", "", error)) return false;
    if (stats) {
        stats->navTriangles = navTris;
        stats->navVertices = navVerts;
        stats->colliders = colliders;
        stats->points = points;
    }
    return true;
}

std::string navmeshPathFor(const std::string& glbPath) {
    std::string base = glbPath;
    if (base.size() >= 4 && lower(base.substr(base.size() - 4)) == ".glb") base.resize(base.size() - 4);
    return base + "_navmesh.glb";
}

}  // namespace blender

bool exportBlender(const HslScene& scene, const std::vector<blender::ExportObject>& objects, const WalkGrid* walk, const HomeWorld& world,
                   const std::string& glbPath, const std::string& navmeshPath, std::string& error, const blender::ExportOptions& options,
                   blender::ExportStats* stats) {
    if (!blender::exportScene(scene, objects, glbPath, error, options, stats)) return false;
    if (navmeshPath.empty()) return true;
    return blender::exportNavmesh(walk, world, navmeshPath, error, stats);
}
