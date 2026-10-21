#include "panorama.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace {

constexpr float kPi = 3.14159265358979f;

// Camera-to-world rotation for a view yawed about +Y then pitched about +X (looks down -Z at 0, 0).
void basis(float yaw, float pitch, float right[3], float up[3], float back[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    right[0] = cy;      right[1] = 0;   right[2] = -sy;
    up[0] = sy * sp;    up[1] = cp;     up[2] = cy * sp;
    back[0] = sy * cp;  back[1] = -sp;  back[2] = cy * cp;
}

struct Face {
    float right[3], up[3], back[3];
    std::vector<u8> rgba;
};

}  // namespace

bool renderPanorama(HslRenderer& r, const PanoramaView& view, uint32_t width, std::vector<u8>& rgb) {
    const uint32_t w = std::max<uint32_t>(64, width & ~1u), h = w / 2;
    // A face spans 90 degrees = w/4 equirect pixels; a little more keeps it sharp after resampling.
    const uint32_t n = std::min<uint32_t>(4096, (w / 4) * 5 / 4);
    r.resize(n, n);

    // Reversed-Z infinite projection with a 90-degree square frustum (what the renderer expects).
    Mat4 proj = {};
    proj.m[0] = 1.0f;
    proj.m[5] = -1.0f;
    proj.m[14] = 0.05f;
    proj.m[11] = -1.0f;

    const float faces[6][2] = {{0, 0}, {kPi * 0.5f, 0}, {kPi, 0}, {-kPi * 0.5f, 0}, {0, kPi * 0.5f}, {0, -kPi * 0.5f}};
    Face f[6];
    for (int i = 0; i < 6; ++i) {
        basis(faces[i][0], faces[i][1], f[i].right, f[i].up, f[i].back);
        EyeView e;
        e.proj = proj;
        // view = inverse(rotation | eye): rows are the basis vectors, translation -R^T * eye.
        Mat4& v = e.view;
        std::memset(v.m, 0, sizeof v.m);
        for (int k = 0; k < 3; ++k) {
            v.m[k * 4 + 0] = f[i].right[k];
            v.m[k * 4 + 1] = f[i].up[k];
            v.m[k * 4 + 2] = f[i].back[k];
        }
        auto dot = [&](const float* a) { return a[0] * view.eye[0] + a[1] * view.eye[1] + a[2] * view.eye[2]; };
        v.m[12] = -dot(f[i].right);
        v.m[13] = -dot(f[i].up);
        v.m[14] = -dot(f[i].back);
        v.m[15] = 1;
        std::memcpy(e.position, view.eye, 12);
        EyeView eyes[2] = {e, e};
        r.render(eyes, 0.0f);
        if (!r.readback(0, f[i].rgba)) return false;
    }

    rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    const float yaw = view.yawDeg * kPi / 180.0f;
    auto rows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            const float lat = (0.5f - (y + 0.5f) / h) * kPi;
            const float cl = std::cos(lat), sl = std::sin(lat);
            for (uint32_t x = 0; x < w; ++x) {
                const float lon = ((x + 0.5f) / w - 0.5f) * 2.0f * kPi - yaw;
                const float d[3] = {std::sin(lon) * cl, sl, -std::cos(lon) * cl};
                // The face this direction points into, and where it lands on that face's image.
                int best = 0;
                float bx = 0, by = 0, bz = -1e9f;
                for (int i = 0; i < 6; ++i) {
                    const float vx = d[0] * f[i].right[0] + d[1] * f[i].right[1] + d[2] * f[i].right[2];
                    const float vy = d[0] * f[i].up[0] + d[1] * f[i].up[1] + d[2] * f[i].up[2];
                    const float vz = -(d[0] * f[i].back[0] + d[1] * f[i].back[1] + d[2] * f[i].back[2]);
                    if (vz > bz) { bz = vz; bx = vx; by = vy; best = i; }
                }
                const float px = (bx / bz * 0.5f + 0.5f) * n - 0.5f, py = (0.5f - by / bz * 0.5f) * n - 0.5f;
                const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, static_cast<int>(n) - 1);
                const int y0i = std::clamp(static_cast<int>(std::floor(py)), 0, static_cast<int>(n) - 1);
                const int x1 = std::min(x0 + 1, static_cast<int>(n) - 1), y1i = std::min(y0i + 1, static_cast<int>(n) - 1);
                const float fx = std::clamp(px - x0, 0.0f, 1.0f), fy = std::clamp(py - y0i, 0.0f, 1.0f);
                const u8* img = f[best].rgba.data();
                u8* out = &rgb[(static_cast<size_t>(y) * w + x) * 3];
                for (int c = 0; c < 3; ++c) {
                    const float a = img[(y0i * n + x0) * 4 + c], b = img[(y0i * n + x1) * 4 + c];
                    const float cc = img[(y1i * n + x0) * 4 + c], dd = img[(y1i * n + x1) * 4 + c];
                    out[c] = static_cast<u8>(std::lround((a + (b - a) * fx) * (1 - fy) + (cc + (dd - cc) * fx) * fy));
                }
            }
        }
    };
    const uint32_t threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (uint32_t t = 0; t < threads; ++t) pool.emplace_back(rows, h * t / threads, h * (t + 1) / threads);
    for (auto& t : pool) t.join();
    return true;
}

float panoramaDomeRadius(const HslRenderer& r, const HslScene& scene, const float eye[3], float fallback) {
    constexpr int kRays = 72;
    std::vector<float> nearest(kRays, 1e30f);
    for (size_t m = 0; m < r.meshCount(); ++m) {
        if (!r.meshVisible(m)) continue;
        const MeshData& md = scene.meshes[r.meshSceneIndex(m)];
        if (md.isSkybox) continue;
        const float* W = r.meshWorld(m);
        const size_t nv = md.positions.size() / 3;
        std::vector<float> P(nv * 3);
        for (size_t v = 0; v < nv; ++v)
            for (int k = 0; k < 3; ++k)
                P[v * 3 + k] = W[k] * md.positions[v * 3] + W[4 + k] * md.positions[v * 3 + 1] + W[8 + k] * md.positions[v * 3 + 2] + W[12 + k];
        for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
            const u32 ia = md.indices[t], ib = md.indices[t + 1], ic = md.indices[t + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            const float* a = &P[ia * 3]; const float* b = &P[ib * 3]; const float* c = &P[ic * 3];
            // Only triangles spanning eye height can be hit by horizontal rays.
            if (std::min({a[1], b[1], c[1]}) > eye[1] || std::max({a[1], b[1], c[1]}) < eye[1]) continue;
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float tv[3] = {eye[0] - a[0], eye[1] - a[1], eye[2] - a[2]};
            for (int i = 0; i < kRays; ++i) {
                const float ang = 2.0f * kPi * i / kRays;
                const float d[3] = {std::sin(ang), 0, -std::cos(ang)};
                const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
                const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                if (std::fabs(det) < 1e-12f) continue;
                const float id = 1.0f / det;
                const float u = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * id;
                if (u < 0 || u > 1) continue;
                const float q[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
                const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * id;
                if (v < 0 || u + v > 1) continue;
                const float dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * id;
                if (dist > 0.05f && dist < nearest[i]) nearest[i] = dist;
            }
        }
    }
    std::vector<float> hits;
    for (float d : nearest) if (d < 500.0f) hits.push_back(d);
    if (hits.size() < kRays / 4) return fallback;
    std::nth_element(hits.begin(), hits.begin() + hits.size() / 2, hits.end());
    return hits[hits.size() / 2];
}

bool writePng(const std::string& path, const std::vector<u8>& rgb, uint32_t w, uint32_t h) {
    stbi_write_png_compression_level = 6;
    return stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 3, rgb.data(), static_cast<int>(w * 3)) != 0;
}
