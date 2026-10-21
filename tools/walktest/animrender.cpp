// A home rendered at chosen animation times, as Frame Home draws a frame (the scene's animate(t) on the CPU, then the
// frame at t): for comparing a Clay / glTF home's APK with its baked .fenv picture for picture (animcheck compares the
// numbers). Views: from high over the walkable area and from the first spawn at eye height.
//   animrender <home.apk|home.fenv> <out-prefix> <size> <t,t,...>     -> <prefix>_t<T>_v<N>.png
// Runs on lavapipe (VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json) or, built with MinGW, on the PC's GPU.
#include "hsl_renderer.h"
#include "hsl_scene.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

static Mat4 projection(float l, float r, float b, float t) {
    Mat4 m = {};
    m.m[0] = 2.0f / (r - l); m.m[8] = (r + l) / (r - l); m.m[5] = -2.0f / (t - b); m.m[9] = -(t + b) / (t - b); m.m[14] = 0.05f; m.m[11] = -1.0f;
    return m;
}
static Mat4 rigidInverse(const Mat4& a) {
    Mat4 r = {};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; ++i) r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: animrender <home> <out-prefix> <size> <t,t,...>\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);
    HslScene scene;
    std::string err;
    if (!loadHslScene(std::string(argv[1]), scene, err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    std::vector<float> times;
    { std::stringstream ss(argv[4]); std::string t; while (std::getline(ss, t, ',')) times.push_back(static_cast<float>(std::atof(t.c_str()))); }
    const HomeWorld w = walkWorld(scene);
    std::vector<std::array<float, 5>> cams;
    if (const WalkGrid* g = w.walkGridCache.get()) {
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f}, y = 0; int n = 0;
        for (int z = 0; z < g->nz; ++z) for (int x = 0; x < g->nx; ++x) for (const WalkSpan* s = g->begin(x, z); s != g->end(x, z); ++s)
            if (s->flags & kWalkOk) { lo[0] = std::min(lo[0], g->centerX(x)); hi[0] = std::max(hi[0], g->centerX(x)); lo[1] = std::min(lo[1], g->centerZ(z)); hi[1] = std::max(hi[1], g->centerZ(z)); y += s->hi; ++n; }
        if (n) {
            y /= n;
            const float mx = (lo[0] + hi[0]) / 2, mz = (lo[1] + hi[1]) / 2, ext = std::max(hi[0] - lo[0], hi[1] - lo[1]);
            cams.push_back({mx, y + ext * 0.75f, mz + ext * 0.45f, 0, -58});
        }
    }
    {   // from the first spawn (else the area's middle) at eye height, three ways
        float p[3] = {0, 0, 0}, yaw = 0;
        if (!w.spawns.empty()) { std::memcpy(p, w.spawns[0].pos, sizeof p); yaw = w.spawns[0].yaw * 57.2958f; }
        else if (!cams.empty()) { p[0] = cams[0][0]; p[2] = cams[0][2]; }
        for (float turn : {0.0f, 180.0f, 90.0f}) cams.push_back({p[0], p[1] + 1.6f, p[2], yaw + turn, turn == 90 ? 10.0f : -8.0f});
    }
    const uint32_t size = static_cast<uint32_t>(std::atoi(argv[3]));
#ifdef FH_USE_VOLK
    if (volkInitialize() != VK_SUCCESS) { std::printf("no Vulkan loader\n"); return 1; }
#endif
    HslRenderer r;
    r.msaa = 1;
    r.foveation = false;
    if (!r.init(size, size)) { std::printf("no Vulkan\n"); return 1; }
    r.setDynamicMeshes(scene.animatedMeshes);
    if (!r.load(scene)) { std::printf("nothing renderable\n"); return 1; }
    r.finishStreaming();
    for (float t : times) {
        if (scene.animate) scene.animate(t, scene.meshes);
        for (size_t i = 0; i < cams.size(); ++i) {
            const float yaw = cams[i][3] / 57.2958f, pitch = cams[i][4] / 57.2958f;
            const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
            Mat4 pose = {};
            pose.m[0] = cy; pose.m[2] = -sy; pose.m[4] = sy * sp; pose.m[5] = cp; pose.m[6] = cy * sp; pose.m[8] = sy * cp; pose.m[9] = -sp; pose.m[10] = cy * cp;
            pose.m[12] = cams[i][0]; pose.m[13] = cams[i][1]; pose.m[14] = cams[i][2]; pose.m[15] = 1;
            EyeView e;
            e.proj = projection(-1, 1, -1, 1);
            e.view = rigidInverse(pose);
            for (int k = 0; k < 3; ++k) e.position[k] = pose.m[12 + k];
            const EyeView eyes[2] = {e, e};
            for (int f = 0; f < 3; ++f) r.render(eyes, t);
            std::vector<u8> rgba;
            if (!r.readback(0, rgba)) { std::printf("readback failed\n"); return 1; }
            char out[1024];
            std::snprintf(out, sizeof out, "%s_t%g_v%zu.png", argv[2], t, i);
            stbi_write_png(out, size, size, 4, rgba.data(), size * 4);
            std::printf("%s\n", out);
        }
    }
    r.shutdown();
    return 0;
}
