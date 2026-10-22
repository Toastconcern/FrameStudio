// Renders a home headlessly (lavapipe: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json) with its walkable area
// and World > Auto-generate's items drawn over it as Frame Studio's guide pass does:
//   walkable area green, walls red boxes, seats magenta posts + facing bar, hotspots cyan posts
// worldrender <apk[+vista]> <out-prefix> [size] [x,y,z,yaw,pitch ...]   (no views: four from above the area's corners)
#include "hsl_renderer.h"
#include "hsl_scene.h"
#include "world_gen.h"
#include "debug_geom.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cmath>
#include <csignal>
#ifndef _WIN32
#include <execinfo.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <cstdlib>

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

#ifndef _WIN32
static void onCrash(int sig) { void* b[64]; const int n = backtrace(b, 64); backtrace_symbols_fd(b, n, 2); _exit(128 + sig); }
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    std::signal(SIGSEGV, onCrash); std::signal(SIGABRT, onCrash);
#endif
    if (argc < 3) return 2;
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<std::string> apks;
    for (std::string a = argv[1];;) { size_t k = a.find(".apk+"); if (k == std::string::npos) { apks.push_back(a); break; } apks.push_back(a.substr(0, k + 4)); a = a.substr(k + 5); }
    HslScene scene; std::string err;
    if (!loadHslScene(apks, scene, err)) { std::printf("load: %s\n", err.c_str()); return 1; }
    HomeWorld w = scene.world;
    w.walkGridCache = bakeWalkGrid(scene, w, WalkParams{});
    const WalkGrid& g = *w.walkGridCache;
    const GenResult gen = generateWorldItems(g, w, drawnWalkSources(scene, w, nullptr, nullptr, false));
    std::printf("%zu walls, %zu seats, %zu hotspots\n", gen.walls.size(), gen.seats.size(), gen.hotspots.size());

    std::vector<GuideVertex> v;
    auto vert = [&](const float* p, const float* c) { GuideVertex q; std::memcpy(q.pos, p, 12); std::memcpy(q.rgba, c, 16); v.push_back(q); };
    auto tri = [&](const float* a, const float* b, const float* c, const float* col) { vert(a, col); vert(b, col); vert(c, col); };
    auto box = [&](const float* m, const float* h, const float* col) {   // a box in frame m, half extents h
        float p[8][3];
        for (int k = 0; k < 8; ++k) {
            const float l[3] = {(k & 1 ? 1 : -1) * h[0], (k & 2 ? 1 : -1) * h[1], (k & 4 ? 1 : -1) * h[2]};
            for (int j = 0; j < 3; ++j) p[k][j] = m[j] * l[0] + m[4 + j] * l[1] + m[8 + j] * l[2] + m[12 + j];
        }
        static const int f[12][3] = {{0,1,3},{0,3,2},{4,6,7},{4,7,5},{0,4,5},{0,5,1},{2,3,7},{2,7,6},{0,2,6},{0,6,4},{1,5,7},{1,7,3}};
        for (auto& t : f) tri(p[t[0]], p[t[1]], p[t[2]], col);
    };
    const float green[4] = {0.1f, 0.85f, 0.3f, 0.35f}, red[4] = {1, 0.15f, 0.1f, 0.55f}, mag[4] = {1, 0, 1, 0.9f}, cyan[4] = {0, 0.9f, 1, 0.9f};
    g.outline([&](const float* a, const float* b, const float* c, bool) {
        const float A[3] = {a[0], a[1] + 0.03f, a[2]}, B[3] = {b[0], b[1] + 0.03f, b[2]}, C[3] = {c[0], c[1] + 0.03f, c[2]};
        tri(A, B, C, green);
    }, [](const float*, const float*) {});
    for (const WorldCollider& c : gen.walls) box(c.world, c.half, red);
    for (const WorldPoint& p : gen.seats) {
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, p.pos[0], p.pos[1] + 0.3f, p.pos[2], 1}, h[3] = {0.04f, 0.3f, 0.04f};
        box(m, h, mag);
        const float fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
        const float m2[16] = {fz,0,-fx,0, 0,1,0,0, fx,0,fz,0, p.pos[0] + fx * 0.2f, p.pos[1] + 0.02f, p.pos[2] + fz * 0.2f, 1}, h2[3] = {0.03f, 0.02f, 0.2f};
        box(m2, h2, mag);
    }
    for (const WorldPoint& p : gen.hotspots) {
        const float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, p.pos[0], p.pos[1] + 0.75f, p.pos[2], 1}, h[3] = {0.05f, 0.75f, 0.05f};
        box(m, h, cyan);
    }

    const uint32_t size = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 768;
    if (volkInitialize() != VK_SUCCESS) { std::printf("no Vulkan loader\n"); return 1; }
    HslRenderer r;
    r.msaa = 1;                                                        // (lavapipe: plain, no MSAA, no foveation)
    r.foveation = false;
    r.viewCount = 1;                                                   // one view, as Frame Studio draws it
    bool timeFrames = false;                                           // time: GPU time a frame at each camera
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("msaa=", 0) == 0) r.msaa = static_cast<uint32_t>(std::atoi(a.c_str() + 5));
        else if (a == "time") timeFrames = true;
    }
    if (!r.init(size, size)) { std::printf("no Vulkan\n"); return 1; }
    std::printf("load...\n");
    if (!r.load(scene)) { std::printf("nothing renderable\n"); return 1; }
    std::printf("stream...\n");
    r.finishStreaming();
    if (!std::getenv("NOGUIDE")) r.setStaticGuide(v);
    // view=N (HslRenderer::DebugView), hide=bits, world (the debug world overlay instead of the generator's items),
    // anywhere after the size: the Debug page's views (env vars don't pass through interop).
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("view=", 0) == 0) r.setDebugView(std::atoi(a.c_str() + 5));
        else if (a.rfind("hide=", 0) == 0) r.debugHide = static_cast<unsigned>(std::atoi(a.c_str() + 5));
        else if (a == "world") {
            std::vector<GuideVertex> ov;
            debuggeom::build(w, &g, ov);
            std::printf("world overlay: %zu triangles\n", ov.size() / 3);
            r.setStaticGuide(ov);                                      // in place of the generator's items
        }
    }
    std::vector<std::array<float, 5>> cams;
    for (int i = 5; i < argc; ++i) { std::array<float, 5> c{}; if (std::sscanf(argv[i], "%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4]) == 5) cams.push_back(c); }
    std::string mode = argc > 4 ? argv[4] : "";
    if (mode.rfind("view=", 0) == 0 || mode.rfind("hide=", 0) == 0 || mode.rfind("msaa=", 0) == 0 || mode == "world" || mode == "time") mode.clear();
    if (mode == "seats")                                               // each seat from 1.6 m in front of it, looking at it
        for (size_t k = 0; k < gen.seats.size() && k < 12; ++k) {
            const WorldPoint& p = gen.seats[k];
            const float fx = -std::sin(p.yaw), fz = -std::cos(p.yaw);
            cams.push_back({p.pos[0] + fx * 1.6f, p.pos[1] + 1.1f, p.pos[2] + fz * 1.6f, p.yaw * 57.2958f + 180, -28});
        }
    if (cams.empty() && mode.empty()) {   // an overview from high over the area, then eye-level views from spots across it, both ways
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f}, y = 0; int n = 0;
        for (int z = 0; z < g.nz; ++z) for (int x = 0; x < g.nx; ++x) for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
            if (s->flags & kWalkOk) { lo[0] = std::min(lo[0], g.centerX(x)); hi[0] = std::max(hi[0], g.centerX(x)); lo[1] = std::min(lo[1], g.centerZ(z)); hi[1] = std::max(hi[1], g.centerZ(z)); y += s->hi; ++n; }
        y /= std::max(1, n);
        const float mx = (lo[0] + hi[0]) / 2, mz = (lo[1] + hi[1]) / 2, ext = std::max(hi[0] - lo[0], hi[1] - lo[1]);
        cams.push_back({mx, y + ext * 0.75f, mz + ext * 0.45f, 0, -58});
        const int eyeViews = std::getenv("EYES") ? std::atoi(std::getenv("EYES")) : 6;
        for (size_t k = 0; k < gen.hotspots.size() && static_cast<int>(cams.size()) < 1 + eyeViews; ++k) {
            const WorldPoint& p = gen.hotspots[k];
            cams.push_back({p.pos[0], p.pos[1] + 1.6f, p.pos[2], p.yaw * 57.2958f, -12});
            cams.push_back({p.pos[0], p.pos[1] + 1.6f, p.pos[2], p.yaw * 57.2958f + 180, -12});
        }
    }
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
        for (int f = 0; f < (r.debugView() == HslRenderer::kViewCost ? 10 : 3); ++f) r.render(eyes, 10.0f);
        if (timeFrames) {   // the GPU's own time a frame, averaged over 120 frames (after the 3 above settled it)
            double gpuMs = 0, uploadMs = 0, cpuMs = 0;
            HslRenderer::FrameStats sum;
            for (int f = 0; f < 120; ++f) {
                r.render(eyes, 10.0f); r.waitFrame();
                const HslRenderer::FrameStats& st = r.lastStats();
                gpuMs += r.lastGpuMs(); uploadMs += r.lastGpuUploadMs(); cpuMs += st.totalMs - st.waitMs;
                sum.readbackMs += st.readbackMs; sum.animMs += st.animMs; sum.globalsMs += st.globalsMs; sum.dynamicMs += st.dynamicMs;
                sum.uploadMs += st.uploadMs; sum.recordMs += st.recordMs; sum.submitMs += st.submitMs;
            }
            std::printf("   GPU %.3f ms a frame (uploads %.3f ms), CPU %.3f ms, %u draws, %u pipeline binds\n", gpuMs / 120, uploadMs / 120, cpuMs / 120,
                        r.lastStats().draws, r.lastStats().pipelineBinds);
            std::printf("   CPU parts: readback %.3f, animation %.3f, globals %.3f, dynamic %.3f, upload %.3f, record %.3f, submit %.3f ms\n",
                        sum.readbackMs / 120, sum.animMs / 120, sum.globalsMs / 120, sum.dynamicMs / 120, sum.uploadMs / 120, sum.recordMs / 120,
                        sum.submitMs / 120);
        }
        if (r.debugView() == HslRenderer::kViewCost)
            for (const auto& c : r.costliest(12)) std::printf("   %.3f ms  %s\n", c.first, c.second >= 0 ? scene.meshes[c.second].name.c_str() : "?");
        std::vector<u8> rgba;
        if (!r.readback(0, rgba)) { std::printf("readback failed\n"); return 1; }
        const std::string out = std::string(argv[2]) + "_" + std::to_string(i) + ".png";
        stbi_write_png(out.c_str(), size, size, 4, rgba.data(), size * 4);
        std::printf("%s: camera (%.1f %.1f %.1f) yaw %.0f pitch %.0f\n", out.c_str(), cams[i][0], cams[i][1], cams[i][2], cams[i][3], cams[i][4]);
    }
    r.shutdown();
    return 0;
}
