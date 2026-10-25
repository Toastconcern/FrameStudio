#include "splat_import.h"

#include "miniz.h"
#include "zstd.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace {

namespace fs = std::filesystem;

constexpr float kShC0 = 0.28209479177387814f;          // degree-0 spherical harmonic: base colour = 0.5 + C0 * dc
constexpr uint64_t kMaxSplats = 20'000'000;            // far beyond any capture; a hostile count can't run memory out

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// (NaN, from a damaged file, is 0: clamp lets it through, and NaN to a byte is undefined)
uint8_t unit(float v) { return v > 0 ? static_cast<uint8_t>(std::min(v, 1.0f) * 255.0f + 0.5f) : 0; }
float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// A finished splat: the rotation normalised (files store it loosely), nothing non-finite (a damaged file).
bool keep(Splat& s) {
    float n = 0;
    for (float q : s.rot) n += q * q;
    if (!(n > 1e-12f) || !std::isfinite(n)) return false;
    n = 1.0f / std::sqrt(n);
    for (float& q : s.rot) q *= n;
    for (int k = 0; k < 3; ++k) if (!std::isfinite(s.pos[k]) || !std::isfinite(s.scale[k])) return false;
    return true;
}

// The 3DGS .ply and .splat use the training cameras' axes (x right, y down, z forward): turned 180 degrees about x
// into Frame's (y up, z toward the viewer). Quaternion: q' = (1,0,0,0 as xyzw) * q.
void fromRdf(Splat& s) {
    s.pos[1] = -s.pos[1];
    s.pos[2] = -s.pos[2];
    const float x = s.rot[0], y = s.rot[1], z = s.rot[2], w = s.rot[3];
    s.rot[0] = w; s.rot[1] = -z; s.rot[2] = y; s.rot[3] = -x;
}

// ── 3DGS .ply: binary (little- or big-endian) or ASCII vertices; x y z, f_dc_0..2 (colour SH), opacity (logit),
// scale_0..2 (log), rot_0..3 (w x y z). Other properties (normals, f_rest_*) are skipped by their size. ──
bool loadPly(const std::vector<uint8_t>& b, SplatCloud& out, std::string& error) {
    const std::string head(b.begin(), b.begin() + std::min<size_t>(b.size(), 1 << 16));
    const size_t end = head.find("end_header");
    if (head.rfind("ply", 0) != 0 || end == std::string::npos || head.find('\n', end) == std::string::npos) { error = "not a PLY file"; return false; }
    size_t pos = head.find('\n', end) + 1;
    struct Prop { std::string name, type; bool list = false; };
    std::vector<std::pair<std::string, uint64_t>> elements;
    std::vector<std::vector<Prop>> props;
    int format = -1;
    std::istringstream in(head.substr(0, end));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string w;
        ls >> w;
        if (w == "format") { std::string f; ls >> f; format = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1; }
        else if (w == "element") { std::string n; uint64_t c = 0; ls >> n >> c; elements.push_back({n, c}); props.emplace_back(); }
        else if (w == "property" && !props.empty()) { Prop p; ls >> p.type; if (p.type == "list") { p.list = true; std::string a; ls >> a >> p.type; } ls >> p.name; props.back().push_back(p); }
    }
    auto size = [](const std::string& t) -> int {
        if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
        if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
        if (t == "int" || t == "uint" || t == "int32" || t == "uint32" || t == "float" || t == "float32") return 4;
        if (t == "double" || t == "float64") return 8;
        return 0;
    };
    if (format < 0 || elements.empty() || elements[0].first != "vertex") { error = "unsupported splat PLY (it must start with its vertices)"; return false; }
    const uint64_t count = elements[0].second;
    const std::vector<Prop>& vp = props[0];
    int stride = 0;
    for (const Prop& p : vp) { if (p.list || size(p.type) == 0) { error = "unsupported splat PLY property " + p.name; return false; } stride += size(p.type); }
    if (count > kMaxSplats || (format != 0 && pos + count * static_cast<uint64_t>(stride) > b.size()) ||
        (format == 0 && count * vp.size() > b.size() - pos)) {   // (ASCII: each value at least a character)
        error = "damaged splat PLY (fewer bytes than splats)";
        return false;
    }
    auto index = [&](const char* n) { for (size_t i = 0; i < vp.size(); ++i) if (vp[i].name == n) return static_cast<int>(i); return -1; };
    // each looked up by name (tools write them in different orders)
    const int ix = index("x"), iy = index("y"), iz = index("z"), iop = index("opacity");
    const int idc[3] = {index("f_dc_0"), index("f_dc_1"), index("f_dc_2")}, isc[3] = {index("scale_0"), index("scale_1"), index("scale_2")};
    const int iro[4] = {index("rot_0"), index("rot_1"), index("rot_2"), index("rot_3")};
    bool all = ix >= 0 && iy >= 0 && iz >= 0 && iop >= 0;
    for (int k = 0; k < 3; ++k) all = all && idc[k] >= 0 && isc[k] >= 0;
    for (int k = 0; k < 4; ++k) all = all && iro[k] >= 0;
    if (!all) { error = "not a Gaussian splat PLY (no f_dc / opacity / scale / rot)"; return false; }
    std::istringstream ascii(format == 0 ? std::string(b.begin() + pos, b.end()) : std::string());
    std::vector<float> v(vp.size());
    out.splats.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        for (size_t k = 0; k < vp.size(); ++k) {
            if (format == 0) { double d = 0; if (!(ascii >> d)) { error = "damaged splat PLY"; return false; } v[k] = static_cast<float>(d); continue; }
            const int n = size(vp[k].type);
            uint8_t raw[8];
            std::memcpy(raw, &b[pos], n);
            pos += n;
            if (format == 2) std::reverse(raw, raw + n);
            const std::string& t = vp[k].type;
            if (t == "float" || t == "float32") { float f; std::memcpy(&f, raw, 4); v[k] = f; }
            else if (n == 8) { double d; std::memcpy(&d, raw, 8); v[k] = static_cast<float>(d); }
            else if (t == "uchar" || t == "uint8") v[k] = raw[0];
            else if (t == "char" || t == "int8") v[k] = static_cast<int8_t>(raw[0]);
            else if (n == 2) { int16_t s; uint16_t u; std::memcpy(&s, raw, 2); std::memcpy(&u, raw, 2); v[k] = t[0] == 'u' ? u : s; }
            else { int32_t s; uint32_t u; std::memcpy(&s, raw, 4); std::memcpy(&u, raw, 4); v[k] = t[0] == 'u' ? static_cast<float>(u) : static_cast<float>(s); }
        }
        Splat s;
        s.pos[0] = v[ix]; s.pos[1] = v[iy]; s.pos[2] = v[iz];
        for (int k = 0; k < 3; ++k) {
            s.scale[k] = std::exp(v[isc[k]]);
            s.rgba[k] = unit(0.5f + kShC0 * v[idc[k]]);
        }
        s.rgba[3] = unit(sigmoid(v[iop]));
        s.rot[0] = v[iro[1]]; s.rot[1] = v[iro[2]]; s.rot[2] = v[iro[3]]; s.rot[3] = v[iro[0]];   // file: w x y z
        fromRdf(s);
        if (keep(s)) out.splats.push_back(s);
    }
    out.format = "3DGS .ply";
    return true;
}

// ── .splat (antimatter15's web viewer): 32 bytes a splat: position 3 x f32, scale 3 x f32 (linear), colour RGBA u8
// (opacity in A), rotation 4 x u8 (w x y z, (v - 128) / 128). The 3DGS axes, as the .ply it was made from. ──
bool loadDotSplat(const std::vector<uint8_t>& b, SplatCloud& out, std::string& error) {
    if (b.empty() || b.size() % 32 != 0) { error = "damaged .splat file (not whole 32-byte splats)"; return false; }
    const size_t n = b.size() / 32;
    if (n > kMaxSplats) { error = "too many splats"; return false; }
    out.splats.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &b[i * 32];
        Splat s;
        std::memcpy(s.pos, p, 12);
        std::memcpy(s.scale, p + 12, 12);
        std::memcpy(s.rgba, p + 24, 4);
        s.rot[3] = (p[28] - 128) / 128.0f;
        s.rot[0] = (p[29] - 128) / 128.0f;
        s.rot[1] = (p[30] - 128) / 128.0f;
        s.rot[2] = (p[31] - 128) / 128.0f;
        fromRdf(s);
        if (keep(s)) out.splats.push_back(s);
    }
    out.format = ".splat";
    return true;
}

// ── .spz (Niantic, MIT). Version 2 and 3: gzip of a 16-byte header (magic "NGSP", version, count, SH degree,
// fractional bits, flags) then, splat by splat in columns: positions (24-bit fixed point), opacities, colours,
// scales, rotations, spherical harmonics. Version 4: the same columns, each its own zstd stream, after a plain
// 32-byte header and a table of the streams' sizes. Stored in Frame's own axes (RUB, as glTF). ──
// The raw deflate after the gzip header, unpacked in pieces. `limit(sofar)` says how much is wanted at most (it may
// tighten once the start says what the file holds): the unpacking stops there, so a small file that unpacks to
// gigabytes (a decompression bomb) can't run memory out. What's past the limit isn't unpacked (and isn't needed).
bool gunzip(const std::vector<uint8_t>& b, std::vector<uint8_t>& out, const std::function<size_t(const std::vector<uint8_t>&)>& limit) {
    if (b.size() < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8) return false;
    const uint8_t flags = b[3];
    size_t p = 10;
    if (flags & 4) { if (p + 2 > b.size()) return false; p += 2 + (b[p] | (b[p + 1] << 8)); }   // extra field
    if (flags & 8) { while (p < b.size() && b[p]) ++p; ++p; }                                    // file name
    if (flags & 16) { while (p < b.size() && b[p]) ++p; ++p; }                                   // comment
    if (flags & 2) p += 2;                                                                       // header CRC
    if (p >= b.size() || b.size() - p > 0xffffffffu) return false;
    mz_stream s = {};
    if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) return false;   // raw deflate
    s.next_in = &b[p];
    s.avail_in = static_cast<unsigned int>(b.size() - p);
    out.clear();
    bool ok = false;
    for (;;) {
        const size_t want = limit(out);
        if (out.size() >= want) { out.resize(want); ok = true; break; }
        const size_t at = out.size();
        out.resize(std::min(want, at + std::max<size_t>(at, size_t(1) << 16)));
        s.next_out = out.data() + at;
        s.avail_out = static_cast<unsigned int>(out.size() - at);
        const int rc = mz_inflate(&s, MZ_NO_FLUSH);
        out.resize(out.size() - s.avail_out);
        if (rc == MZ_STREAM_END) { ok = true; break; }
        if (rc != MZ_OK) break;                                        // damaged, or cut short
    }
    mz_inflateEnd(&s);
    return ok;
}

constexpr uint32_t kSpzMagic = 0x5053474e;                   // "NGSP"
constexpr size_t kShDim[5] = {0, 3, 8, 15, 24};             // SH coefficients a colour channel, by degree
constexpr size_t kShDimMax = 24;

// The columns of a .spz, `count` splats each, whichever version they came from.
struct SpzColumns {
    const uint8_t *pos, *alpha, *color, *scale, *rot;
    uint32_t count;
    int fracBits;
    bool smallestThree;                                     // version 3 on: 4-byte rotations
};

void decodeSpz(const SpzColumns& c, SplatCloud& out) {
    const float fixed = 1.0f / static_cast<float>(1 << std::min(c.fracBits, 24));
    constexpr float kColorScale = 0.15f;   // how .spz packs the SH base colour into a byte
    out.splats.reserve(c.count);
    for (size_t i = 0; i < c.count; ++i) {
        Splat s;
        for (size_t k = 0; k < 3; ++k) {
            const uint8_t* q = c.pos + (i * 3 + k) * 3;
            int32_t f = q[0] | (q[1] << 8) | (q[2] << 16);
            if (f & 0x800000) f |= ~0xffffff;                               // sign-extend 24 bits
            s.pos[k] = static_cast<float>(f) * fixed;
            s.scale[k] = std::exp(c.scale[i * 3 + k] / 16.0f - 10.0f);
            const float dc = (c.color[i * 3 + k] / 255.0f - 0.5f) / kColorScale;
            s.rgba[k] = unit(0.5f + kShC0 * dc);
        }
        s.rgba[3] = c.alpha[i];                                             // already the opacity (0..255)
        if (c.smallestThree) {   // "smallest three": 2 bits the largest component, then three 10-bit (9 + sign) others
            const uint8_t* r = c.rot + i * 4;
            uint32_t v = r[0] | (r[1] << 8) | (r[2] << 16) | (static_cast<uint32_t>(r[3]) << 24);
            const int largest = static_cast<int>(v >> 30);
            constexpr uint32_t mask = (1u << 9) - 1;
            float sum = 0;
            for (int k = 3; k >= 0; --k) {
                if (k == largest) continue;
                const float m = static_cast<float>(v & mask) / mask * 0.70710678f;
                const bool neg = (v >> 9) & 1;
                v >>= 10;
                s.rot[k] = neg ? -m : m;
                sum += m * m;
            }
            s.rot[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
        } else {
            float sum = 0;
            for (size_t k = 0; k < 3; ++k) { s.rot[k] = c.rot[i * 3 + k] / 127.5f - 1.0f; sum += s.rot[k] * s.rot[k]; }
            s.rot[3] = std::sqrt(std::max(0.0f, 1.0f - sum));
        }
        if (keep(s)) out.splats.push_back(s);
    }
    out.format = ".spz";
}

// Version 2 and 3: one gzip of the header and every column.
bool loadSpzGzip(const std::vector<uint8_t>& gz, SplatCloud& out, std::string& error) {
    std::vector<uint8_t> b;
    // At most what the header says the columns take (once the 16 header bytes are in), never past the largest a
    // file may hold.
    constexpr size_t kMostBytes = 16 + static_cast<size_t>(kMaxSplats) * (9 + 1 + 3 + 3 + 4 + kShDimMax * 3);
    const auto limit = [&](const std::vector<uint8_t>& sofar) -> size_t {
        if (sofar.size() < 16) return kMostBytes;
        uint32_t v, n;
        std::memcpy(&v, &sofar[4], 4); std::memcpy(&n, &sofar[8], 4);
        if (n > kMaxSplats || sofar[12] > 4) return 16;               // refused below
        return 16 + static_cast<size_t>(n) * (9 + 1 + 3 + 3 + (v >= 3 ? 4 : 3) + kShDim[sofar[12]] * 3);
    };
    if (!gunzip(gz, b, limit) || b.size() < 16) { error = "damaged .spz file (not gzip)"; return false; }
    uint32_t magic, version, count;
    std::memcpy(&magic, &b[0], 4); std::memcpy(&version, &b[4], 4); std::memcpy(&count, &b[8], 4);
    const int shDegree = b[12], fracBits = b[13];
    if (magic != kSpzMagic) { error = "not a .spz file"; return false; }
    if (version < 2 || version > 3) { error = "unsupported .spz version " + std::to_string(version); return false; }
    if (count > kMaxSplats || shDegree > 4) { error = "damaged .spz file"; return false; }
    const size_t n = count, rotBytes = version >= 3 ? 4 : 3;
    const size_t need = 16 + n * (9 + 1 + 3 + 3 + rotBytes + kShDim[shDegree] * 3);
    if (b.size() < need) { error = "damaged .spz file (cut short)"; return false; }
    SpzColumns c;
    c.pos = &b[16];
    c.alpha = c.pos + n * 9;
    c.color = c.alpha + n;
    c.scale = c.color + n * 3;
    c.rot = c.scale + n * 3;
    c.count = count;
    c.fracBits = fracBits;
    c.smallestThree = version >= 3;
    decodeSpz(c, out);
    return true;
}

// Version 4: a plain 32-byte header (magic, version, count, SH degree, fractional bits, flags, stream count, where
// the table of streams starts; any extensions sit between), the table (each stream: u64 packed size, u64 unpacked
// size), then the zstd streams back to back: positions, opacities, colours, scales, rotations, SH (none at degree 0).
bool loadSpzZstd(const std::vector<uint8_t>& b, SplatCloud& out, std::string& error) {
    if (b.size() < 32) { error = "damaged .spz file (cut short)"; return false; }
    uint32_t version, count, toc;
    std::memcpy(&version, &b[4], 4); std::memcpy(&count, &b[8], 4); std::memcpy(&toc, &b[16], 4);
    const int shDegree = b[12], fracBits = b[13], numStreams = b[15];
    if (version != 4) { error = "unsupported .spz version " + std::to_string(version); return false; }
    // a hostile count can't run memory out: at most 1024:1 on the 9 position bytes a splat (Niantic's own check)
    if (count == 0 || count > kMaxSplats || shDegree > 4 || static_cast<uint64_t>(count) * 9 > static_cast<uint64_t>(b.size()) * 1024) {
        error = "damaged .spz file";
        return false;
    }
    const size_t n = count;
    const size_t expect[6] = {n * 9, n, n * 3, n * 3, n * 4, n * kShDim[shDegree] * 3};
    const int streams = expect[5] ? 6 : 5;
    if (numStreams != streams || toc < 32 || static_cast<uint64_t>(toc) + 16ull * numStreams > b.size()) {
        error = "damaged .spz file (stream table)";
        return false;
    }
    std::vector<uint8_t> col[5];
    size_t at = static_cast<size_t>(toc) + 16u * numStreams;
    for (int i = 0; i < numStreams; ++i) {
        uint64_t packed, unpacked;
        std::memcpy(&packed, &b[toc + 16u * i], 8);
        std::memcpy(&unpacked, &b[toc + 16u * i + 8], 8);
        if (unpacked != expect[i] || packed > b.size() - at) { error = "damaged .spz file (stream table)"; return false; }
        if (i < 5) {   // the SH past the base colour aren't kept, so aren't unpacked
            col[i].resize(expect[i]);
            const size_t got = ZSTD_decompress(col[i].data(), col[i].size(), &b[at], static_cast<size_t>(packed));
            if (ZSTD_isError(got) || got != expect[i]) { error = "damaged .spz file (zstd)"; return false; }
        }
        at += static_cast<size_t>(packed);
    }
    SpzColumns c;
    c.pos = col[0].data();
    c.alpha = col[1].data();
    c.color = col[2].data();
    c.scale = col[3].data();
    c.rot = col[4].data();
    c.count = count;
    c.fracBits = fracBits;
    c.smallestThree = true;
    decodeSpz(c, out);
    return true;
}

bool loadSpz(const std::vector<uint8_t>& b, SplatCloud& out, std::string& error) {
    uint32_t magic = 0;
    if (b.size() >= 4) std::memcpy(&magic, b.data(), 4);
    if (magic == kSpzMagic) return loadSpzZstd(b, out, error);   // version 4 on: not gzipped
    return loadSpzGzip(b, out, error);
}

}  // namespace

bool isSplatPath(const std::string& path) {
    const std::string ext = lower(fs::u8path(path).extension().u8string());
    if (ext == ".splat" || ext == ".spz") return true;
    if (ext != ".ply") return false;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    std::string head(4096, '\0');
    in.read(&head[0], static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    const size_t end = head.find("end_header");
    return end != std::string::npos && head.find("f_dc_0") < end;
}

bool loadSplats(const std::string& path, SplatCloud& out, std::string& error) {
    const std::vector<uint8_t> b = readFile(path);
    if (b.empty()) { out = SplatCloud{}; error = "couldn't read " + path; return false; }
    const bool ok = loadSplatBytes(b, lower(fs::u8path(path).extension().u8string()), out, error);
    if (!ok) error += ": " + path;
    return ok;
}

std::vector<uint8_t> writeSpz(const std::vector<Splat>& splats) {
    const uint32_t n = static_cast<uint32_t>(splats.size());
    std::vector<uint8_t> raw(16 + static_cast<size_t>(n) * 19);   // position 9, opacity 1, colour 3, scale 3, rotation 3
    const uint32_t magic = 0x5053474e, version = 2;
    std::memcpy(&raw[0], &magic, 4); std::memcpy(&raw[4], &version, 4); std::memcpy(&raw[8], &n, 4);
    raw[12] = 0;                                   // SH degree: base colour only
    raw[13] = 12;                                  // fractional bits: 1/4096 m
    uint8_t* pp = &raw[16];
    uint8_t* pa = pp + n * 9;
    uint8_t* pc = pa + n;
    uint8_t* ps = pc + n * 3;
    uint8_t* pr = ps + n * 3;
    auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    for (uint32_t i = 0; i < n; ++i) {
        const Splat& s = splats[i];
        for (int k = 0; k < 3; ++k) {
            const long f = std::clamp(std::lround(s.pos[k] * 4096.0f), -0x7fffffL, 0x7fffffL);
            pp[(i * 3 + k) * 3] = static_cast<uint8_t>(f & 0xff);
            pp[(i * 3 + k) * 3 + 1] = static_cast<uint8_t>((f >> 8) & 0xff);
            pp[(i * 3 + k) * 3 + 2] = static_cast<uint8_t>((f >> 16) & 0xff);
            const float dc = (s.rgba[k] / 255.0f - 0.5f) / kShC0;
            pc[i * 3 + k] = byte((dc * 0.15f + 0.5f) * 255.0f);
            ps[i * 3 + k] = byte((std::log(std::max(s.scale[k], 1e-6f)) + 10.0f) * 16.0f);
        }
        pa[i] = s.rgba[3];
        const float sign = s.rot[3] < 0 ? -1.0f : 1.0f;   // w is rebuilt as the positive root: store q or -q to match
        for (int k = 0; k < 3; ++k) pr[i * 3 + k] = byte((sign * s.rot[k] + 1.0f) * 127.5f);
    }
    // gzip: header, raw deflate, CRC-32 and size of the unpacked bytes
    std::vector<uint8_t> out = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    size_t z = 0;
    void* d = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &z, 128 | TDEFL_GREEDY_PARSING_FLAG);
    if (!d) return {};
    out.insert(out.end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + z);
    mz_free(d);
    const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, raw.data(), raw.size())), size = static_cast<uint32_t>(raw.size());
    for (uint32_t v : {crc, size}) for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
    return out;
}

void splatCovariance(const Splat& s, float cov[6]) {
    const float x = s.rot[0], y = s.rot[1], z = s.rot[2], w = s.rot[3];
    const float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    float m[3][3];                                    // R S
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) m[i][j] = r[i][j] * s.scale[j];
    auto dot = [&](int a, int b) { return m[a][0] * m[b][0] + m[a][1] * m[b][1] + m[a][2] * m[b][2]; };   // (R S)(R S)^T
    cov[0] = dot(0, 0); cov[1] = dot(0, 1); cov[2] = dot(0, 2); cov[3] = dot(1, 1); cov[4] = dot(1, 2); cov[5] = dot(2, 2);
}

bool loadSplatBytes(const std::vector<uint8_t>& b, const std::string& ext, SplatCloud& out, std::string& error) {
    out = SplatCloud{};
    bool ok = false;
    try {
        ok = ext == ".spz" ? loadSpz(b, out, error) : ext == ".splat" ? loadDotSplat(b, out, error) : loadPly(b, out, error);
    } catch (const std::exception&) {
        error = "not enough memory for the splats";
        ok = false;
    }
    if (ok && out.splats.empty()) { error = "no splats in the file"; ok = false; }
    if (!ok) out.splats.clear();
    return ok;
}
