// The home's meshes: name, triangles and world bounds (what names a home ships, for the generator's hints).
#include "hsl_scene.h"
#include "walkgrid.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace {
void xf(const float* m, const float* p, float* o) {
    for (int k = 0; k < 3; ++k) o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
}
void sub(const float* a, const float* b, float* o) { for (int k = 0; k < 3; ++k) o[k] = a[k] - b[k]; }
float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
float len(const float* a) { return std::sqrt(dot(a, a)); }
// How far along dir (a unit vector) from p a ray meets triangle abc, both sides; < 0: it doesn't (within maxT).
float rayHitsTri(const float* p, const float* dir, const float* a, const float* b, const float* c, float maxT) {
    float e1[3], e2[3], h[3], s[3], q[3];
    sub(b, a, e1); sub(c, a, e2);
    cross(dir, e2, h);
    const float det = dot(e1, h);
    if (std::fabs(det) < 1e-12f) return -1;
    const float f = 1 / det;
    sub(p, a, s);
    const float u = f * dot(s, h);
    if (u < 0 || u > 1) return -1;
    cross(s, e1, q);
    const float v = f * dot(dir, q);
    if (v < 0 || u + v > 1) return -1;
    const float t = f * dot(e2, q);
    return t > 0 && t <= maxT ? t : -1;
}
}  // namespace
int main(int argc, char** argv) {
    HslScene s; std::string err;
    if (argc < 2) return 2;
    std::vector<std::string> apks;   // "a.apk+b.apk": a footprint and its vista
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    if (!loadHslScene(apks, s, err)) return 1;
    std::vector<std::string> names;
    for (const MeshData& m : s.meshes) names.push_back(m.name);
    const std::string pre = walkSharedPrefix(names);
    static const char* kHint[] = {"-", "SEAT", "WALL", "BLOCKER", "FLOOR", "CEILING", "FURNITURE", "NATURE"};
    if (const char* sl = std::getenv("SLICE")) {   // (SLICE=y,x0,x1,z0,z1: each face crossing height y in that box, as a segment)
        float y, x0, x1, z0, z1;
        if (std::sscanf(sl, "%f,%f,%f,%f,%f", &y, &x0, &x1, &z0, &z1) != 5) return 2;
        for (const MeshData& m : s.meshes) {
            float m16[16];
            if (m.hasWorldMatrix) std::memcpy(m16, m.worldMatrix, sizeof m16); else trsToMat4(m.transform, m16);
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                float w[3][3];
                for (int k = 0; k < 3; ++k) xf(m16, &m.positions[m.indices[t + k] * 3], w[k]);
                float pts[2][3]; int n = 0;
                for (int k = 0; k < 3 && n < 2; ++k) {
                    const float* a = w[k]; const float* b = w[(k + 1) % 3];
                    if ((a[1] - y) * (b[1] - y) >= 0 || a[1] == b[1]) continue;
                    const float f = (y - a[1]) / (b[1] - a[1]);
                    for (int j = 0; j < 3; ++j) pts[n][j] = a[j] + (b[j] - a[j]) * f;
                    ++n;
                }
                if (n < 2) continue;
                const float cx = (pts[0][0] + pts[1][0]) / 2, cz = (pts[0][2] + pts[1][2]) / 2;
                if (cx < x0 || cx > x1 || cz < z0 || cz > z1) continue;
                float e1[3], e2[3], nn[3];
                sub(w[1], w[0], e1); sub(w[2], w[0], e2); cross(e1, e2, nn);
                const float l = len(nn);
                std::printf("%s  (%.3f %.3f) -> (%.3f %.3f)  facing (%.2f %.2f %.2f)\n", m.name.c_str(), pts[0][0], pts[0][2], pts[1][0], pts[1][2],
                            nn[0] / l, nn[1] / l, nn[2] / l);
            }
        }
        return 0;
    }
    if (const char* ray = std::getenv("RAY")) {   // (RAY=ox,oy,oz,dx,dy,dz: what each mesh it meets is there: its colour, UV)
        float o[3], d[3];
        if (std::sscanf(ray, "%f,%f,%f,%f,%f,%f", &o[0], &o[1], &o[2], &d[0], &d[1], &d[2]) != 6) return 2;
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (float& x : d) x /= l;
        for (const MeshData& m : s.meshes) {
            float m16[16];
            if (m.hasWorldMatrix) std::memcpy(m16, m.worldMatrix, sizeof m16); else trsToMat4(m.transform, m16);
            float best = 1e30f; size_t bt = 0;
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                float w[9];
                for (int k = 0; k < 3; ++k) xf(m16, &m.positions[m.indices[t + k] * 3], w + k * 3);
                const float h = rayHitsTri(o, d, w, w + 3, w + 6, 200.0f);
                if (h >= 0 && h < best) { best = h; bt = t; }
            }
            if (best > 1e29f) continue;
            std::printf("hit %s at %.2f m (%.2f %.2f %.2f)\n", m.name.c_str(), best, o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best);
            for (int k = 0; k < 3; ++k) {
                const u32 v = m.indices[bt + k];
                std::printf("   corner %u: colour", v);
                if (m.colors.size() >= (v + 1) * 4) for (int c = 0; c < 4; ++c) std::printf(" %d", m.colors[v * 4 + c]);
                if (m.uvs.size() >= (v + 1) * 2) std::printf("  uv %.3f %.3f", m.uvs[v * 2], m.uvs[v * 2 + 1]);
                if (m.uvs2.size() >= (v + 1) * 2) std::printf("  uv2 %.3f %.3f", m.uvs2[v * 2], m.uvs2[v * 2 + 1]);
                std::printf("\n");
            }
            {   // its texture per square metre against the mesh's usual (a squeezed face: far more)
                auto dens = [&](size_t t) {
                    float w[9];
                    for (int k = 0; k < 3; ++k) xf(m16, &m.positions[m.indices[t + k] * 3], w + k * 3);
                    float e1[3], e2[3], n[3];
                    sub(w + 3, w, e1); sub(w + 6, w, e2); cross(e1, e2, n);
                    const float A = len(n) * 0.5f;
                    if (m.uvs.size() < m.positions.size() / 3 * 2 || A <= 1e-5f) return std::make_pair(A, -1.0f);
                    const float* a = &m.uvs[m.indices[t] * 2]; const float* b = &m.uvs[m.indices[t + 1] * 2]; const float* c = &m.uvs[m.indices[t + 2] * 2];
                    return std::make_pair(A, std::fabs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * 0.5f / A);
                };
                std::vector<float> all;
                for (size_t t = 0; t + 2 < m.indices.size(); t += 3) { const auto p = dens(t); if (p.second >= 0) all.push_back(p.second); }
                if (!all.empty()) {
                    std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
                    const auto p = dens(bt);
                    std::printf("   face %.3f m2, %.3g uv/m2, the mesh's usual %.3g (x%.1f)\n", p.first, p.second, all[all.size() / 2], p.second / all[all.size() / 2]);
                }
            }
        }
        return 0;
    }
    for (const MeshData& m : s.meshes) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < m.positions.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], m.positions[i]); hi[i % 3] = std::max(hi[i % 3], m.positions[i]); }
        std::printf("%-10s %-60s %7zu tris  x %6.2f..%6.2f  y %5.2f..%5.2f  z %6.2f..%6.2f\n", kHint[walkNameHint(m.name, pre)], (m.path.empty() ? m.name : m.path + " / " + m.name).c_str(), m.indices.size() / 3, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
        if (std::getenv("MAT"))   // (MAT=1: its shader and textures, what it draws with)
            std::printf("           shader %s  tex %s  named %zu  uvs %zu  colors %zu  lm %s%s%s\n", m.shaderPath.c_str(), m.texSrc ? "yes" : "NO", m.namedTex.size(),
                        m.uvs.size() / 2, m.colors.size() / 4, m.lmLdrSrc || !m.lmRGBA.empty() ? "yes" : "no", m.useBlend ? "  blend" : "", m.alphaTest ? "  alphaTest" : "");
        if (std::getenv("MAT"))   // (its baked light: where it comes from)
            std::printf("           uv2 %zu  lmHalf %zu  lmSrc %s (%ux%u hdr %d)  lm %ux%u  hasLightmap %d  so %.3f %.3f %.3f %.3f  namedHdr %zu\n", m.uvs2.size() / 2, m.lmHalf ? m.lmHalf->size() : 0,
                        m.lmSrc ? "yes" : "no", m.lmSrc ? m.lmSrc->w : 0, m.lmSrc ? m.lmSrc->h : 0, m.lmSrc ? m.lmSrc->hdr : 0, m.lmW, m.lmH, m.hasLightmap,
                        m.lmUvScaleOffset[0], m.lmUvScaleOffset[1], m.lmUvScaleOffset[2], m.lmUvScaleOffset[3], m.namedHdr.size());
        if (std::getenv("MAT") && !m.uvs.empty()) {   // (its UVs' extent: past 0..1, a texture that repeats)
            float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
            for (size_t i = 0; i + 1 < m.uvs.size(); i += 2) { u0 = std::min(u0, m.uvs[i]); u1 = std::max(u1, m.uvs[i]); v0 = std::min(v0, m.uvs[i + 1]); v1 = std::max(v1, m.uvs[i + 1]); }
            std::printf("           uv %.2f..%.2f, %.2f..%.2f  tiled %d\n", u0, u1, v0, v1, m.tiled ? 1 : 0);
        }
    }
}
