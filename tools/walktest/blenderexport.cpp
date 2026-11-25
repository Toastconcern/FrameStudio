// blenderexport <apk[+vista]|fenv> <out.glb> [threads] [maxTextureSide]: the home exported for Blender
// (src/blender_export.h) as Frame Studio exports it: every mesh where it's drawn, the walkable area baked as Studio
// bakes it. Both files are then read back with cgltf (parsed, buffers loaded, validated) and checked against the
// scene: objects, triangles, world bounds, every image decodes, the walk layer's triangles, colliders and points.
// Exit 0 when every check holds.
#include "blender_export.h"
#include "hsl_scene.h"
#include "walkgrid.h"
#include "spv_reflect.h"
#include "loaders/rendtxtr_parser.h"
#include "cgltf.h"
#include "stb_image.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++failures;
}
template <typename... A> std::string fmt(const char* f, A... a) {   // (every argument printed as a double)
    char s[256];
    std::snprintf(s, sizeof s, f, static_cast<double>(a)...);
    return s;
}

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= path.size()) {
        size_t b = path.find('/', a);
        if (b == std::string::npos) b = path.size();
        if (b > a) out.push_back(path.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

size_t validTriangles(const MeshData& md) {
    const size_t n = md.positions.size() / 3;
    size_t t = 0;
    for (size_t i = 0; i + 2 < md.indices.size(); i += 3) t += md.indices[i] < n && md.indices[i + 1] < n && md.indices[i + 2] < n;
    return t;
}

cgltf_data* readBack(const std::string& path) {
    cgltf_options o = {};
    cgltf_data* d = nullptr;
    if (cgltf_parse_file(&o, path.c_str(), &d) != cgltf_result_success) { check(false, "parse", path); return nullptr; }
    check(cgltf_load_buffers(&o, d, path.c_str()) == cgltf_result_success, "buffers load");
    check(cgltf_validate(d) == cgltf_result_success, "cgltf_validate");
    return d;
}

size_t primitiveTriangles(const cgltf_mesh& m) {
    size_t t = 0;
    for (size_t p = 0; p < m.primitives_count; ++p)
        t += (m.primitives[p].indices ? m.primitives[p].indices->count : 0) / 3;
    return t;
}

const cgltf_node* findNode(const cgltf_data* d, const char* name) {
    for (size_t i = 0; i < d->nodes_count; ++i)
        if (d->nodes[i].name && std::strcmp(d->nodes[i].name, name) == 0) return &d->nodes[i];
    return nullptr;
}

const cgltf_accessor* attribute(const cgltf_primitive& p, cgltf_attribute_type type, int index) {
    for (size_t a = 0; a < p.attributes_count; ++a)
        if (p.attributes[a].type == type && p.attributes[a].index == index) return p.attributes[a].data;
    return nullptr;
}

u16 toHalf(float f) {
    u32 x;
    std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    const int ex = static_cast<int>(e) - 127 + 15;
    if (ex >= 31) return static_cast<u16>(sign | 0x7BFF);
    if (ex <= 0) return static_cast<u16>(sign);
    return static_cast<u16>(sign | static_cast<u32>(ex) << 10 | m >> 13);
}

// The colour of a decoded PNG image at (u, v).
bool texel(const cgltf_image& im, float u, float v, unsigned char out[4]) {
    const cgltf_buffer_view* bv = im.buffer_view;
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load_from_memory(static_cast<const unsigned char*>(bv->buffer->data) + bv->offset, static_cast<int>(bv->size), &w, &h, &c, 4);
    if (!px) return false;
    const int x = std::clamp(static_cast<int>(u * w), 0, w - 1), y = std::clamp(static_cast<int>(v * h), 0, h - 1);
    std::memcpy(out, px + (static_cast<size_t>(y) * w + x) * 4, 4);
    stbi_image_free(px);
    return true;
}

// A small made-up home for what the test homes don't have: a baked HDR lightmap on an atlas patch, a cube-mapped sky,
// a mesh over 65535 vertices (32-bit indices), a sheared object (baked into its vertices), a mirrored one, a mesh two
// objects share, every collider kind, seats with exits and a hotspot.
int synthetic(const std::string& glbPath) {
    HslScene s;
    s.apkPaths = {"synthetic.apk"};
    // 0: a 2 m quad, checker base, HDR lightmap (a ramp 0..2) on the atlas patch scale 0.5, offset 0.25
    MeshData quad;
    quad.name = "Quad";
    quad.positions = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    quad.normals = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    quad.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    quad.uvs2 = quad.uvs;
    quad.indices = {0, 2, 1, 0, 3, 2};
    quad.texW = quad.texH = 8;
    quad.hasTexture = true;
    for (int i = 0; i < 64; ++i)
        for (int c = 0; c < 4; ++c) quad.texRGBA.push_back(c == 3 ? 255 : ((i % 8 + i / 8) % 2 ? 230 : 40));
    auto lm = std::make_shared<std::vector<u16>>();
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            for (int c = 0; c < 4; ++c) lm->push_back(toHalf(c == 3 ? 1.0f : 2.0f * x / 31));
    quad.lmHalf = lm;
    quad.lmW = quad.lmH = 32;
    quad.hasLightmap = true;
    const float atlas[4] = {0.5f, 0.5f, 0.25f, 0.25f};
    std::memcpy(quad.lmUvScaleOffset, atlas, sizeof atlas);
    s.meshes.push_back(quad);
    // 1: the sky, a sphere drawn with a six-colour cube (+X red, -X cyan, +Y white, -Y black, +Z blue, -Z yellow)
    MeshData sky;
    sky.name = "SkyDome";
    sky.isSkybox = true;
    constexpr int kLon = 32, kLat = 16;
    for (int j = 0; j <= kLat; ++j)
        for (int i = 0; i <= kLon; ++i) {
            const float lat = 3.14159265f * (0.5f - static_cast<float>(j) / kLat), lon = 2 * 3.14159265f * i / kLon;
            sky.positions.insert(sky.positions.end(), {50 * std::cos(lat) * std::cos(lon), 50 * std::sin(lat), 50 * std::cos(lat) * std::sin(lon)});
        }
    for (int j = 0; j < kLat; ++j)
        for (int i = 0; i < kLon; ++i) {
            const u32 a = static_cast<u32>(j * (kLon + 1) + i), b = a + kLon + 1;
            sky.indices.insert(sky.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    static const u8 kFace[6][3] = {{255, 0, 0}, {0, 255, 255}, {255, 255, 255}, {0, 0, 0}, {0, 0, 255}, {255, 255, 0}};
    sky.cubeSize = 16;
    for (int f = 0; f < 6; ++f)
        for (int t = 0; t < 256; ++t) sky.cubeRGBA.insert(sky.cubeRGBA.end(), {kFace[f][0], kFace[f][1], kFace[f][2], 255});
    s.meshes.push_back(sky);
    // 2: a 300 x 300 vertex terrain (90000 vertices), a flat colour as a 1x1 texture
    MeshData big;
    big.name = "Terrain";
    constexpr int kSide = 300;
    for (int z = 0; z < kSide; ++z)
        for (int x = 0; x < kSide; ++x) big.positions.insert(big.positions.end(), {x * 0.1f, std::sin(x * 0.05f), z * 0.1f});
    for (int z = 0; z + 1 < kSide; ++z)
        for (int x = 0; x + 1 < kSide; ++x) {
            const u32 a = static_cast<u32>(z * kSide + x), b = a + kSide;
            big.indices.insert(big.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    big.texRGBA = {200, 100, 50, 255};
    big.hasTexture = true;
    s.meshes.push_back(big);

    std::vector<blender::ExportObject> objects(5);
    objects[0].mesh = 0; objects[0].name = "Quad"; objects[0].folders = {"Room", "Floor"};
    objects[1].mesh = 0; objects[1].name = "Quad copy"; objects[1].folders = {"Room"}; objects[1].world[12] = 5;
    objects[2].mesh = 0; objects[2].name = "Quad mirrored"; objects[2].world[0] = -1; objects[2].world[12] = 10;
    objects[3].mesh = 2; objects[3].name = "Terrain sheared"; objects[3].world[4] = 0.5f;   // x += 0.5 y
    objects[4].mesh = 1; objects[4].name = "SkyDome";

    HomeWorld& w = s.world;
    WorldCollider box;
    box.name = "Table";
    box.kind = WorldCollider::Box;
    const float c30 = std::cos(0.5236f), s30 = std::sin(0.5236f);
    const float boxWorld[16] = {c30, 0, -s30, 0, 0, 1, 0, 0, s30, 0, c30, 0, 0, 1, 0, 1};
    std::memcpy(box.world, boxWorld, sizeof boxWorld);
    box.half[0] = 1; box.half[1] = 2; box.half[2] = 3;
    w.colliders.push_back(box);
    WorldCollider ball;
    ball.kind = WorldCollider::Sphere; ball.radius = 0.5f; ball.world[12] = 3;
    w.colliders.push_back(ball);
    WorldCollider pill;
    pill.name = "Post"; pill.kind = WorldCollider::Capsule; pill.radius = 0.3f; pill.length = 1.2f; pill.world[14] = 3;
    w.colliders.push_back(pill);
    WorldCollider ramp;
    ramp.name = "Ramp"; ramp.kind = WorldCollider::Mesh;
    ramp.positions = {0, 0, 0, 2, 1, 0, 2, 1, 2, 0, 0, 2};
    ramp.indices = {0, 1, 2, 0, 2, 3};
    w.colliders.push_back(ramp);
    WorldCollider zone = box;
    zone.name = "Door trigger"; zone.role = WorldCollider::Trigger;
    w.colliders.push_back(zone);
    WorldPoint spawn;
    spawn.pos[0] = 1; spawn.pos[2] = 2; spawn.yaw = 1.0f;
    w.spawns.push_back(spawn);
    WorldPoint seat;
    seat.name = "Sofa"; seat.pos[1] = 0.45f; seat.yaw = 3.14159265f; seat.exit[2] = -0.5f;
    w.seats.push_back(seat);
    seat.name = "Chair"; seat.pos[0] = 2; seat.exit[2] = 0;
    w.seats.push_back(seat);
    WorldPoint spot;
    spot.name = "Window"; spot.pos[0] = -3;
    w.hotspots.push_back(spot);

    // a walkable area from the quad and the terrain
    const auto grid = bakeWalkGrid(s, w, WalkParams{});
    blender::ExportStats st;
    std::string err;
    const std::string navPath = blender::navmeshPathFor(glbPath);
    if (!exportBlender(s, objects, grid.get(), w, glbPath, navPath, err, blender::ExportOptions{}, &st)) { std::fprintf(stderr, "export failed: %s\n", err.c_str()); return 1; }
    std::printf("synthetic: %zu nodes, %zu meshes, %zu materials, %zu images, %zu triangles; walk layer %zu triangles, %zu colliders, %zu points\n", st.nodes,
                st.meshes, st.materials, st.images, st.triangles, st.navTriangles, st.colliders, st.points);

    std::printf("%s\n", glbPath.c_str());
    cgltf_data* d = readBack(glbPath);
    if (!d) return 1;
    const cgltf_node* q0 = findNode(d, "Quad");
    const cgltf_node* q1 = findNode(d, "Quad copy");
    const cgltf_node* q2 = findNode(d, "Quad mirrored");
    const cgltf_node* terrain = findNode(d, "Terrain sheared");
    const cgltf_node* skyNode = findNode(d, "Sky");
    check(q0 && q1 && q2 && terrain && skyNode, "every object's node (the sky named Sky)");
    if (!(q0 && q1 && q2 && terrain && skyNode)) return 3;
    check(q0->parent && q0->parent->name && std::strcmp(q0->parent->name, "Floor") == 0 && q0->parent->parent && q1->parent == q0->parent->parent,
          "folders as nodes (Room / Floor)");
    check(q0->mesh == q1->mesh && q2->mesh == q0->mesh, "a mesh two objects share is written once");
    check(q2->has_scale && q2->scale[0] < 0, "the mirrored object keeps its negative scale", fmt("scale %.2f %.2f %.2f", q2->scale[0], q2->scale[1], q2->scale[2]));
    check(!terrain->has_matrix && !terrain->has_translation && !terrain->has_rotation && !terrain->has_scale, "the sheared object's node has no transform");
    {
        const cgltf_primitive& p = terrain->mesh->primitives[0];
        const cgltf_accessor* pos = attribute(p, cgltf_attribute_type_position, 0);
        // the shear baked in: x + 0.5 y at its largest
        float sheared = -1e9f;
        for (size_t v = 0; v + 2 < big.positions.size(); v += 3) sheared = std::max(sheared, big.positions[v] + 0.5f * big.positions[v + 1]);
        check(p.indices->component_type == cgltf_component_type_r_32u, "32-bit indices for 90000 vertices");
        check(std::fabs(pos->max[0] - sheared) < 1e-4f, "the shear is in the vertices", fmt("x max %.4f, expected %.4f", pos->max[0], sheared));
        const cgltf_material* m = p.material;
        check(m && std::fabs(m->pbr_metallic_roughness.base_color_factor[0] - 0.578f) < 0.01f && std::fabs(m->pbr_metallic_roughness.base_color_factor[2] - 0.0319f) < 0.005f &&
                  !m->pbr_metallic_roughness.base_color_texture.texture,
              "a 1x1 texture becomes the base colour factor (linear)");
    }
    {
        const cgltf_primitive& p = q0->mesh->primitives[0];
        const cgltf_material* m = p.material;
        check(m && m->pbr_metallic_roughness.base_color_texture.texture && m->occlusion_texture.texture && m->occlusion_texture.texcoord == 1,
              "the quad: checker base, lightmap on UV1");
        const cgltf_accessor* uv1 = attribute(p, cgltf_attribute_type_texcoord, 1);
        float a[2] = {0, 0}, c[2] = {0, 0};
        if (uv1) { cgltf_accessor_read_float(uv1, 0, a, 2); cgltf_accessor_read_float(uv1, 2, c, 2); }
        check(uv1 && std::fabs(a[0] - 0.25f) < 1e-5f && std::fabs(c[0] - 0.75f) < 1e-5f, "UV1 moved onto its atlas patch", fmt("%.3f,%.3f .. %.3f,%.3f", a[0], a[1], c[0], c[1]));
        unsigned char lo[4] = {0}, mid[4] = {0}, hi[4] = {0};
        if (m && m->occlusion_texture.texture) {
            const cgltf_image& im = *m->occlusion_texture.texture->image;
            texel(im, 0.0f, 0.5f, lo); texel(im, 0.25f, 0.5f, mid); texel(im, 1.0f, 0.5f, hi);
        }
        // radiance 0 -> 0; 0.5 -> sRGB 188; 1 and over -> 255
        check(lo[0] == 0 && std::abs(mid[0] - 188) <= 6 && hi[0] == 255, "the lightmap: linear radiance clamped, sRGB", fmt("%.0f %.0f %.0f", lo[0], mid[0], hi[0]));
    }
    {
        const cgltf_primitive& p = skyNode->mesh->primitives[0];
        const cgltf_accessor* pos = attribute(p, cgltf_attribute_type_position, 0);
        const cgltf_accessor* uv = attribute(p, cgltf_attribute_type_texcoord, 0);
        // the vertex furthest along +X should land at u 0.75 (the panorama's +X), and the seam's copies past u 1
        float best = -1e9f, at[2] = {0, 0}, umax = 0;
        for (size_t v = 0; v < pos->count; ++v) {
            float x[3], t[2];
            cgltf_accessor_read_float(pos, v, x, 3);
            cgltf_accessor_read_float(uv, v, t, 2);
            umax = std::max(umax, t[0]);
            if (x[0] > best) { best = x[0]; at[0] = t[0]; at[1] = t[1]; }
        }
        check(std::fabs(at[0] - 0.75f) < 0.01f && std::fabs(at[1] - 0.5f) < 0.01f, "the sky's +X at the panorama's +X", fmt("uv %.3f %.3f", at[0], at[1]));
        check(umax > 1.0f, "the panorama's seam split (u past 1)", fmt("u max %.3f", umax));
        const cgltf_material* m = p.material;
        unsigned char px[6][4] = {};
        if (m && m->pbr_metallic_roughness.base_color_texture.texture) {
            const cgltf_image& im = *m->pbr_metallic_roughness.base_color_texture.texture->image;
            texel(im, 0.75f, 0.5f, px[0]);     // +X red
            texel(im, 0.25f, 0.5f, px[1]);     // -X cyan
            texel(im, 0.5f, 0.01f, px[2]);     // +Y white
            texel(im, 0.5f, 0.99f, px[3]);     // -Y black
            texel(im, 0.01f, 0.5f, px[4]);     // +Z blue
            texel(im, 0.5f, 0.5f, px[5]);      // -Z yellow
        }
        bool same = true;
        for (int f = 0; f < 6; ++f)
            for (int c = 0; c < 3; ++c) same = same && std::abs(px[f][c] - kFace[f][c]) < 8;
        std::string seen;
        for (int f = 0; f < 6; ++f) seen += fmt(f ? "  %.0f,%.0f,%.0f" : "%.0f,%.0f,%.0f", px[f][0], px[f][1], px[f][2]);
        check(same, "the sky panorama's six directions (+X -X +Y -Y +Z -Z)", seen);
    }
    cgltf_free(d);

    std::printf("%s\n", navPath.c_str());
    d = readBack(navPath);
    if (!d) return 1;
    const cgltf_node* nav = findNode(d, "Navmesh");
    check(!grid || (nav && nav->mesh), "a Navmesh node", grid ? fmt("%.0f triangles", st.navTriangles) : "no walkable area baked");
    const cgltf_node* colliders = findNode(d, "Colliders");
    check(colliders && colliders->children_count == 5, "five colliders");
    if (const cgltf_node* t = findNode(d, "Table")) {
        const cgltf_accessor* pos = attribute(t->mesh->primitives[0], cgltf_attribute_type_position, 0);
        float m[16];
        cgltf_node_transform_world(t, m);
        check(pos->max[0] == 1 && pos->max[1] == 2 && pos->max[2] == 3 && std::fabs(m[0] - c30) < 1e-5f && std::fabs(m[13] - 1) < 1e-5f,
              "the box: its half extents in the mesh, its frame on the node");
    }
    const cgltf_node* trig = findNode(d, "Door trigger");
    check(trig && trig->mesh->primitives[0].material && std::strcmp(trig->mesh->primitives[0].material->name, "Trigger") == 0, "a trigger in the trigger material");
    const cgltf_node* sofa = findNode(d, "Seat Sofa");
    check(sofa && sofa->children_count == 1 && findNode(d, "Seat Chair") && findNode(d, "Seat Chair")->children_count == 0, "seats, the sofa with its exit");
    if (sofa && sofa->children_count == 1) {
        float m[16];
        cgltf_node_transform_world(sofa->children[0], m);   // exit (0,0,-0.5) turned by pi: in front of the sofa's back, +Z
        check(std::fabs(m[12]) < 1e-4f && std::fabs(m[14] - 0.5f) < 1e-4f, "the exit turned with the seat", fmt("%.3f %.3f %.3f", m[12], m[13], m[14]));
    }
    check(findNode(d, "Spawn") && findNode(d, "Hotspot Window"), "the spawn and the hotspot");
    cgltf_free(d);
    std::printf(failures ? "%d check(s) failed\n" : "all checks hold\n", failures);
    return failures ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--synthetic") return synthetic(argv[2]);
    if (argc < 3) {
        std::fprintf(stderr, "usage: blenderexport <apk[+vista]|fenv> <out.glb> [threads] [maxTextureSide]\n"
                             "       blenderexport --synthetic <out.glb>   (a made-up home with what the test homes lack)\n");
        return 2;
    }
    std::vector<std::string> apks;   // "a.apk+b.apk": a footprint and its vista
    for (std::string a = argv[1];;) {
        const size_t k = a.find(".apk+");
        if (k == std::string::npos) { apks.push_back(a); break; }
        apks.push_back(a.substr(0, k + 4));
        a = a.substr(k + 5);
    }
    const std::string glbPath = argv[2], navPath = blender::navmeshPathFor(glbPath);
    blender::ExportOptions opt;
    if (argc > 3) opt.threads = static_cast<unsigned>(std::atoi(argv[3]));
    if (argc > 4) opt.maxTextureSide = static_cast<unsigned>(std::atoi(argv[4]));

    auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    HslScene s;
    std::string err;
    if (!loadHslScene(apks, s, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
    std::printf("loaded %zu meshes, %zu colliders, %zu spawns, %zu seats, %zu hotspots (%.1f s)\n", s.meshes.size(), s.world.colliders.size(),
                s.world.spawns.size(), s.world.seats.size(), s.world.hotspots.size(), seconds());

    // Every mesh as Studio has it on opening: drawn with its own world matrix, in the folders its file puts it in.
    std::vector<blender::ExportObject> objects;
    size_t expectObjects = 0, expectTriangles = 0, skies = 0;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < s.meshes.size(); ++i) {
        const MeshData& md = s.meshes[i];
        blender::ExportObject o;
        o.mesh = static_cast<int>(i);
        if (md.hasWorldMatrix) std::memcpy(o.world, md.worldMatrix, sizeof o.world);
        else trsToMat4(md.transform, o.world);
        o.name = md.name;
        o.folders = split(md.path);
        objects.push_back(o);
        const size_t t = validTriangles(md);
        if (!t) continue;
        ++expectObjects;
        expectTriangles += t;
        skies += md.isSkybox;
        for (size_t v = 0; v + 2 < md.positions.size(); v += 3)
            for (int r = 0; r < 3; ++r) {
                const float w = o.world[r] * md.positions[v] + o.world[4 + r] * md.positions[v + 1] + o.world[8 + r] * md.positions[v + 2] + o.world[12 + r];
                if (std::isfinite(w)) { lo[r] = std::min(lo[r], w); hi[r] = std::max(hi[r], w); }
            }
    }

    if (std::getenv("BLENDEREXPORT_MESHES"))   // what each mesh brings: its textures, lightmap and how it's drawn
        for (const MeshData& md : s.meshes) {
            bool baseNamed = false;
            for (const auto& nt : md.namedTex) baseNamed = baseNamed || nt.second == md.texSrc;
            std::printf("  mesh %-40.40s named %zu base %s%s lm %s uv1 %d sky %d cube %u emissive %d blend %d cut %d tint %.2f %.2f %.2f %.2f\n", md.name.c_str(),
                        md.namedTex.size(), md.texSrc ? "src" : md.texRGBA.empty() ? "none" : "rgba", baseNamed ? " (named)" : "",
                        md.lmSrc ? "hdr" : md.lmHalf ? "half" : md.lmLdrSrc ? "ldr" : md.hasLightmap ? "rgba" : "-", !md.uvs2.empty(), md.isSkybox, md.cubeSize,
                        md.hasEmissive, md.useBlend, md.alphaTest, md.tint[0], md.tint[1], md.tint[2], md.tint[3]);
            const ShaderProgramSrc* p = findProgram(s, md.shaderPath);   // its shader's samplers (* bound by the mesh)
            spv::Module m;
            if (!md.namedTex.empty() && p && spv::reflect(p->frag, m)) {
                std::string names;
                for (const spv::Resource& r : m.resources) {
                    if (r.kind != spv::ResKind::SampledImage && r.kind != spv::ResKind::CombinedImageSampler) continue;
                    const u32 h = murmur3_x86_32(r.name.data(), r.name.size(), 0);
                    bool bound = false;
                    for (const auto& nt : md.namedTex) bound = bound || nt.first == h;
                    names += " " + std::to_string(r.set) + ":" + (r.name.empty() ? "?" : r.name) + (bound ? "*" : "");
                }
                std::printf("       samplers%s\n", names.c_str());
            }
            // BLENDEREXPORT_DUMP=<dir>: the textures of meshes named BLENDEREXPORT_DUMP_MESH, as PPM (RGB) + PGM (alpha)
            const char* dir = std::getenv("BLENDEREXPORT_DUMP");
            const char* only = std::getenv("BLENDEREXPORT_DUMP_MESH");
            if (dir && only && md.name.find(only) != std::string::npos) {
                std::vector<std::pair<std::string, const TexSource*>> all;
                if (p && spv::reflect(p->frag, m))
                    for (const spv::Resource& r : m.resources)
                        for (const auto& nt : md.namedTex)
                            if (nt.first == murmur3_x86_32(r.name.data(), r.name.size(), 0)) all.push_back({r.name, nt.second.get()});
                if (md.texSrc) all.push_back({"loaderBase", md.texSrc.get()});
                for (const auto& t : all) {
                    std::vector<u8> px;
                    const TexSource& x = *t.second;
                    if (x.faces != 1 || !astc::decodeASTC(x.astc.data(), static_cast<u32>(x.astc.size()), x.w, x.h, x.bw, x.bh, px, false, 1)) continue;
                    const std::string base = std::string(dir) + "/" + md.name + "_" + t.first;
                    FILE* f = std::fopen((base + ".ppm").c_str(), "wb");
                    FILE* a = std::fopen((base + "_alpha.pgm").c_str(), "wb");
                    if (!f || !a) continue;
                    std::fprintf(f, "P6\n%u %u\n255\n", x.w, x.h);
                    std::fprintf(a, "P5\n%u %u\n255\n", x.w, x.h);
                    for (size_t q = 0; q + 3 < px.size(); q += 4) { std::fwrite(&px[q], 1, 3, f); std::fwrite(&px[q + 3], 1, 1, a); }
                    std::fclose(f);
                    std::fclose(a);
                    std::printf("       dumped %s (%ux%u%s)\n", base.c_str(), x.w, x.h, x.hdr ? ", HDR" : "");
                }
            }
        }

    t0 = std::chrono::steady_clock::now();
    const auto grid = bakeWalkGrid(s, s.world, WalkParams{});
    size_t outlineTris = 0;
    if (grid) grid->outline([&](const float*, const float*, const float*, bool) { ++outlineTris; }, [](const float*, const float*) {});
    std::printf("walkable area baked: %d x %d cells, %zu outline triangles (%.1f s)\n", grid ? grid->nx : 0, grid ? grid->nz : 0, outlineTris, seconds());

    t0 = std::chrono::steady_clock::now();
    blender::ExportStats st;
    int lastPct = -10;
    opt.progress = [&](float f, const char* what) {
        const int pct = static_cast<int>(f * 100);
        if (pct >= lastPct + 10) { std::printf("  %3d%% %s\n", pct, what); std::fflush(stdout); lastPct = pct; }
    };
    if (!exportBlender(s, objects, grid.get(), s.world, glbPath, navPath, err, opt, &st)) { std::fprintf(stderr, "export failed: %s\n", err.c_str()); return 1; }
    std::printf("exported in %.1f s: %zu nodes, %zu meshes, %zu materials, %zu images (%zu failed), %zu triangles, %zu skipped\n", seconds(), st.nodes,
                st.meshes, st.materials, st.images, st.imagesFailed, st.triangles, st.skipped);
    std::printf("walk layer: %zu triangles, %zu vertices, %zu colliders, %zu points\n", st.navTriangles, st.navVertices, st.colliders, st.points);

    // ── the environment, read back ──
    std::printf("%s\n", glbPath.c_str());
    cgltf_data* d = readBack(glbPath);
    if (!d) return 1;
    size_t meshNodes = 0, nodeTriangles = 0, textured = 0, imagesOk = 0, blended = 0, masked = 0, withLightmap = 0, withEmissive = 0, uv1 = 0;
    float glo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, ghi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < d->nodes_count; ++i) {
        const cgltf_node& n = d->nodes[i];
        if (!n.mesh) continue;
        ++meshNodes;
        nodeTriangles += primitiveTriangles(*n.mesh);
        float m[16];
        cgltf_node_transform_world(&n, m);
        for (size_t p = 0; p < n.mesh->primitives_count; ++p) {
            const cgltf_primitive& pr = n.mesh->primitives[p];
            for (size_t a = 0; a < pr.attributes_count; ++a) {
                if (pr.attributes[a].type == cgltf_attribute_type_texcoord && pr.attributes[a].index == 1) ++uv1;
                if (pr.attributes[a].type != cgltf_attribute_type_position) continue;
                const cgltf_accessor* acc = pr.attributes[a].data;
                for (size_t v = 0; v < acc->count; ++v) {
                    float q[3];
                    cgltf_accessor_read_float(acc, v, q, 3);
                    for (int r = 0; r < 3; ++r) {
                        const float w = m[r] * q[0] + m[4 + r] * q[1] + m[8 + r] * q[2] + m[12 + r];
                        glo[r] = std::min(glo[r], w);
                        ghi[r] = std::max(ghi[r], w);
                    }
                }
            }
        }
    }
    for (size_t i = 0; i < d->materials_count; ++i) {
        const cgltf_material& m = d->materials[i];
        textured += m.pbr_metallic_roughness.base_color_texture.texture != nullptr;
        withLightmap += m.occlusion_texture.texture != nullptr;
        withEmissive += m.emissive_texture.texture != nullptr;
        blended += m.alpha_mode == cgltf_alpha_mode_blend;
        masked += m.alpha_mode == cgltf_alpha_mode_mask;
    }
    size_t texels = 0, pngBytes = 0;
    for (size_t i = 0; i < d->images_count; ++i) {
        const cgltf_buffer_view* bv = d->images[i].buffer_view;
        if (!bv) continue;
        const unsigned char* data = static_cast<const unsigned char*>(bv->buffer->data) + bv->offset;
        int w = 0, h = 0, c = 0;
        if (unsigned char* px = stbi_load_from_memory(data, static_cast<int>(bv->size), &w, &h, &c, 4)) {
            ++imagesOk;
            texels += static_cast<size_t>(w) * h;
            pngBytes += bv->size;
            stbi_image_free(px);
        }
    }
    std::printf("  %zu nodes (%zu with a mesh), %zu meshes, %zu materials (%zu textured, %zu lightmapped, %zu emissive, %zu blended, %zu cut out), %zu images "
                "(%.1f Mtexels, %.1f MB PNG), %zu primitives with UV1\n",
                d->nodes_count, meshNodes, d->meshes_count, d->materials_count, textured, withLightmap, withEmissive, blended, masked, d->images_count,
                texels / 1e6, pngBytes / 1048576.0, uv1);
    check(meshNodes == expectObjects, "a node per drawable object", fmt("%.0f nodes, %.0f objects", meshNodes, expectObjects));
    check(nodeTriangles == expectTriangles, "every triangle", fmt("%.0f in the file, %.0f in the scene", nodeTriangles, expectTriangles));
    check(d->materials_count == st.materials && d->images_count == st.images, "materials and images as reported");
    check(imagesOk == d->images_count, "every image decodes", fmt("%.0f of %.0f", imagesOk, d->images_count));
    check(st.imagesFailed == 0, "every texture exported", fmt("%.0f failed", st.imagesFailed));
    float worst = 0;
    for (int r = 0; r < 3; ++r) worst = std::max({worst, std::fabs(glo[r] - lo[r]), std::fabs(ghi[r] - hi[r])});
    check(worst < 1e-3f * std::max(1.0f, std::max(std::fabs(hi[0] - lo[0]), std::fabs(hi[2] - lo[2]))), "world bounds",
          fmt("x %.2f..%.2f  y %.2f..%.2f", glo[0], ghi[0], glo[1], ghi[1]) + fmt("  z %.2f..%.2f (off by %.5f)", glo[2], ghi[2], worst));
    const cgltf_node* sky = findNode(d, "Sky");
    check(!skies || sky, "a node named Sky", fmt("%.0f sky meshes", skies));
    cgltf_free(d);

    // ── the walk layer, read back ──
    std::printf("%s\n", navPath.c_str());
    d = readBack(navPath);
    if (!d) return 1;
    const cgltf_node* nav = findNode(d, "Navmesh");
    check(!grid || outlineTris == 0 || (nav && nav->mesh), "a Navmesh node");
    if (nav && nav->mesh) {
        const size_t tris = primitiveTriangles(*nav->mesh);
        check(tris == st.navTriangles && tris <= outlineTris && tris + outlineTris / 100 >= outlineTris, "the walkable area's triangles",
              fmt("%.0f of %.0f outline triangles, %.0f vertices", tris, outlineTris, st.navVertices));
        const cgltf_accessor* pos = nav->mesh->primitives[0].attributes[0].data;
        const float x0 = grid->ox, z0 = grid->oz, x1 = grid->ox + grid->nx * grid->p.cell, z1 = grid->oz + grid->nz * grid->p.cell;
        check(pos->has_min && pos->min[0] >= x0 - 1e-3f && pos->max[0] <= x1 + 1e-3f && pos->min[2] >= z0 - 1e-3f && pos->max[2] <= z1 + 1e-3f,
              "the Navmesh lies within the grid", fmt("x %.2f..%.2f  z %.2f..%.2f", pos->min[0], pos->max[0], pos->min[2], pos->max[2]));
    }
    size_t colliders = 0;
    if (const cgltf_node* c = findNode(d, "Colliders")) colliders = c->children_count;
    size_t expectColliders = 0;
    for (const WorldCollider& c : s.world.colliders)
        expectColliders += c.kind != WorldCollider::Mesh || c.indices.size() >= 3;
    check(colliders == expectColliders, "a node per collider", fmt("%.0f of %.0f", colliders, expectColliders));
    auto children = [&](const char* group) { const cgltf_node* g = findNode(d, group); return g ? g->children_count : 0; };
    check(children("Spawns") == s.world.spawns.size() && children("Seats") == s.world.seats.size() && children("Hotspots") == s.world.hotspots.size(),
          "spawns, seats and hotspots", fmt("%.0f, %.0f, %.0f", children("Spawns"), children("Seats"), children("Hotspots")));
    if (!s.world.spawns.empty()) {
        const cgltf_node* sp = findNode(d, s.world.spawns[0].name.empty() ? "Spawn" : ("Spawn " + s.world.spawns[0].name).c_str());
        const WorldPoint& p = s.world.spawns[0];
        bool same = sp != nullptr;
        if (sp) {
            // the node's -Z turned by its rotation: the way the spawn faces, (-sin yaw, 0, -cos yaw)
            float m[16];
            cgltf_node_transform_world(sp, m);
            same = std::fabs(m[12] - p.pos[0]) < 1e-4f && std::fabs(m[13] - p.pos[1]) < 1e-4f && std::fabs(m[14] - p.pos[2]) < 1e-4f &&
                   std::fabs(-m[8] + std::sin(p.yaw)) < 1e-4f && std::fabs(-m[10] + std::cos(p.yaw)) < 1e-4f;
        }
        check(same, "the spawn's place and facing", fmt("at %.2f %.2f %.2f, yaw %.3f", p.pos[0], p.pos[1], p.pos[2], p.yaw));
    }
    cgltf_free(d);
    std::printf(failures ? "%d check(s) failed\n" : "all checks hold\n", failures);
    return failures ? 3 : 0;
}
