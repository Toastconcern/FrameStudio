#include "model_import.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "ufbx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" float* stbi_loadf_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* retval_from_stbi_load);
extern "C" int stbi_info_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp);

// At most this many pixels decoded (16384 x 8192, a 16K panorama): a small damaged file can claim a picture of
// gigabytes. (stb itself also refuses a side over 16384: stb_image_impl.c.)
constexpr unsigned long long kMaxPixels = 16384ull * 8192ull;
static bool sizeOk(const unsigned char* data, size_t n) {
    int w = 0, h = 0, c = 0;
    return data && n > 0 && n <= 0x7fffffff && stbi_info_from_memory(data, static_cast<int>(n), &w, &h, &c) && w > 0 && h > 0 &&
           static_cast<unsigned long long>(w) * static_cast<unsigned long long>(h) <= kMaxPixels;
}

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<u8> readFile(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// cgltf's file reads, by the UTF-8 path as a wide name on Windows (its own fopen takes the ANSI code page, so a
// model under C:\Users\Zoë\ wasn't found).
cgltf_result gltfRead(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return cgltf_result_file_not_found;
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (size && *size) {                                   // a buffer's declared length: as cgltf's own, short is an error
        if (bytes.size() < *size) return cgltf_result_io_error;
        bytes.resize(*size);
    }
    void* out = std::malloc(std::max<size_t>(bytes.size(), 1));
    if (!out) return cgltf_result_out_of_memory;
    if (!bytes.empty()) std::memcpy(out, bytes.data(), bytes.size());
    if (size) *size = bytes.size();
    *data = out;
    return cgltf_result_success;
}
void gltfRelease(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

// An accessor's floats, `n` a vertex, sparse substitutions applied (cgltf_accessor_read_float leaves sparse
// accessors as zeros). `comps` floats a vertex; vertices past the accessor's own count stay `fill`.
std::vector<float> unpackFloats(const cgltf_accessor* a, size_t vertices, size_t comps, float fill) {
    std::vector<float> out(vertices * comps, fill);
    if (!a || cgltf_num_components(a->type) != comps) return out;
    std::vector<float> all(a->count * comps);
    if (cgltf_accessor_unpack_floats(a, all.data(), all.size()) != all.size()) return out;
    std::copy_n(all.begin(), std::min(all.size(), out.size()), out.begin());
    return out;
}

struct Image { std::vector<u8> rgba; u32 w = 0, h = 0; bool alpha = false; };

bool decode(const u8* data, size_t n, Image& img) {
    int w = 0, h = 0, comp = 0;
    if (!sizeOk(data, n)) return false;
    unsigned char* px = stbi_load_from_memory(data, static_cast<int>(n), &w, &h, &comp, 4);
    if (!px) return false;
    img.w = static_cast<u32>(w);
    img.h = static_cast<u32>(h);
    img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    for (size_t i = 3; i < img.rgba.size() && !img.alpha; i += 4) img.alpha = img.rgba[i] < 250;
    return true;
}

// A finished part: texture (or a flat colour), colour factor, blend mode.
void finish(MeshData& md, const Image* img, const float color[4], bool blend, bool cutout, float cutoff) {
    md.nVerts = static_cast<u32>(md.positions.size() / 3);
    md.nIdx = static_cast<u32>(md.indices.size());
    md.hasTexture = true;
    if (img && !img->rgba.empty()) {
        md.texRGBA = img->rgba;
        md.texW = img->w;
        md.texH = img->h;
    } else {
        md.texRGBA = {255, 255, 255, 255};
        md.texW = md.texH = 1;
    }
    for (int k = 0; k < 4; ++k) md.tint[k] = md.curTint[k] = color[k];
    md.useBlend = blend;
    md.alphaTest = cutout;
    md.alphaCutoff = cutoff;
    md.doubleSided = true;
}

bool importGltf(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    cgltf_options opt = {};
    opt.file.read = gltfRead;
    opt.file.release = gltfRelease;
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) { error = "not a readable glTF file: " + path; return false; }
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) { cgltf_free(data); error = "glTF buffers missing next to " + path; return false; }
    // Accessor / buffer-view ranges, index ranges and node loops are only checked here; the reads below trust them.
    if (cgltf_validate(data) != cgltf_result_success) { cgltf_free(data); error = "malformed glTF (failed validation): " + path; return false; }
    files.push_back(path);
    const fs::path dir = fs::u8path(path).parent_path();
    for (cgltf_size b = 0; b < data->buffers_count; ++b)
        if (data->buffers[b].uri && std::strncmp(data->buffers[b].uri, "data:", 5) != 0) {
            std::string rel = data->buffers[b].uri;   // (as written in the file: "my%20model.bin" is "my model.bin")
            cgltf_decode_uri(rel.data());
            rel.resize(std::strlen(rel.c_str()));
            files.push_back((dir / fs::u8path(rel)).u8string());
        }
    std::map<const cgltf_image*, Image> images;
    auto image = [&](const cgltf_texture_view& view) -> const Image* {
        if (!view.texture || !view.texture->image) return nullptr;
        const cgltf_image* im = view.texture->image;
        auto it = images.find(im);
        if (it != images.end()) return &it->second;
        Image& out = images[im];
        if (im->buffer_view && im->buffer_view->buffer->data) {
            const cgltf_buffer_view& bv = *im->buffer_view;
            if (bv.offset > bv.buffer->size || bv.size > bv.buffer->size - bv.offset) return &out;   // range past the buffer
            const u8* p = static_cast<const u8*>(im->buffer_view->buffer->data) + im->buffer_view->offset;
            decode(p, im->buffer_view->size, out);
        } else if (im->uri && std::strncmp(im->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(im->uri, ',');
            if (comma) {
                // (the '=' padding isn't data: cgltf's decoder fails on it, so the length leaves it out)
                const size_t len = std::strlen(comma + 1);
                size_t pad = 0;
                while (pad < 2 && pad < len && comma[len - pad] == '=') ++pad;
                const size_t size = len / 4 * 3 - std::min(pad, len / 4 * 3);
                void* bytes = nullptr;
                if (cgltf_load_buffer_base64(&opt, size, comma + 1, &bytes) == cgltf_result_success) {
                    decode(static_cast<const u8*>(bytes), size, out);
                    std::free(bytes);
                }
            }
        } else if (im->uri) {
            std::string rel = im->uri;
            cgltf_decode_uri(rel.data());
            rel.resize(std::strlen(rel.c_str()));
            const std::string full = (dir / fs::u8path(rel)).u8string();
            const std::vector<u8> bytes = readFile(full);
            if (!bytes.empty() && decode(bytes.data(), bytes.size(), out)) files.push_back(full);
        }
        return &out;
    };
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (!node.mesh) continue;
        float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = node.mesh->primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor *pos = nullptr, *uv = nullptr, *col = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& at = prim.attributes[a];
                if (at.type == cgltf_attribute_type_position) pos = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
                else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
            }
            if (!pos || pos->count == 0) continue;
            MeshData md;
            md.name = std::string(node.name ? node.name : node.mesh->name ? node.mesh->name : "mesh") + (node.mesh->primitives_count > 1 ? "_" + std::to_string(p) : "");
            for (const cgltf_node* a = node.parent; a; a = a->parent)   // the nodes above it in the file
                if (a->name) md.path = md.path.empty() ? std::string(a->name) : std::string(a->name) + "/" + md.path;
            md.positions = unpackFloats(pos, pos->count, 3, 0.0f);
            md.uvs = unpackFloats(uv, pos->count, 2, 0.0f);
            if (col) {
                const size_t nc = cgltf_num_components(col->type);
                const std::vector<float> c = unpackFloats(col, pos->count, nc, 1.0f);
                md.colors.resize(pos->count * 4);
                for (cgltf_size v = 0; v < pos->count; ++v)
                    for (size_t k = 0; k < 4; ++k)
                        md.colors[v * 4 + k] = static_cast<u8>(std::clamp(k < nc ? c[v * nc + k] : 1.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            if (prim.indices) {
                md.indices.resize(prim.indices->count);
                // Clamp out-of-range indices (degenerate triangle) rather than reading past the vertex arrays.
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    md.indices[i] = static_cast<u32>(std::min<cgltf_size>(cgltf_accessor_read_index(prim.indices, i), pos->count - 1));
            } else {
                md.indices.resize(pos->count);
                for (cgltf_size i = 0; i < pos->count; ++i) md.indices[i] = static_cast<u32>(i);
            }
            md.indices.resize(md.indices.size() / 3 * 3);              // only whole triangles (a damaged file's count may not be)
            if (md.indices.empty()) continue;
            std::memcpy(md.worldMatrix, world, sizeof world);
            md.hasWorldMatrix = true;
            float color[4] = {1, 1, 1, 1};
            const Image* img = nullptr;
            bool blend = false, cutout = false;
            float cutoff = 0.5f;
            if (prim.material) {
                const cgltf_material& m = *prim.material;
                if (m.has_pbr_metallic_roughness) {
                    std::memcpy(color, m.pbr_metallic_roughness.base_color_factor, sizeof color);
                    img = image(m.pbr_metallic_roughness.base_color_texture);
                } else if (m.has_pbr_specular_glossiness) {
                    std::memcpy(color, m.pbr_specular_glossiness.diffuse_factor, sizeof color);
                    img = image(m.pbr_specular_glossiness.diffuse_texture);
                }
                blend = m.alpha_mode == cgltf_alpha_mode_blend;
                cutout = m.alpha_mode == cgltf_alpha_mode_mask;
                cutoff = m.alpha_cutoff;
                if (m.unlit && !img) {}   // unlit materials draw the same way here
            }
            finish(md, img, color, blend, cutout, cutoff);
            meshes.push_back(std::move(md));
        }
    }
    cgltf_free(data);
    if (meshes.empty()) { error = "no triangle meshes in " + path; return false; }
    return true;
}

bool importUfbx(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;   // (an OBJ whose .mtl is missing opens, untextured, instead of not at all)
    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &err);
    if (!scene) {
        char msg[512];
        ufbx_format_error(msg, sizeof msg, &err);
        error = "couldn't read " + path + ": " + msg;
        return false;
    }
    files.push_back(path);
    const fs::path dir = fs::u8path(path).parent_path();
    const bool flipV = true;                     // FBX / OBJ texture space starts bottom-left; images load top-first
    std::map<const ufbx_texture*, Image> images;
    auto image = [&](const ufbx_texture* t) -> const Image* {
        if (!t) return nullptr;
        auto it = images.find(t);
        if (it != images.end()) return &it->second;
        Image& out = images[t];
        if (t->content.size) {
            decode(static_cast<const u8*>(t->content.data), t->content.size, out);
            return &out;
        }
        for (const ufbx_string& s : {t->filename, t->absolute_filename, t->relative_filename}) {
            if (!s.length) continue;
            std::string f(s.data, s.length);
            std::vector<u8> bytes = readFile(f);
            if (bytes.empty()) { f = (dir / fs::u8path(f).filename()).u8string(); bytes = readFile(f); }   // moved next to the model
            if (!bytes.empty() && decode(bytes.data(), bytes.size(), out)) { files.push_back(f); break; }
        }
        return &out;
    };
    std::vector<uint32_t> tri;
    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        const ufbx_mesh* mesh = node->mesh;
        if (!mesh) continue;
        tri.resize(mesh->max_face_triangles * 3);
        const ufbx_matrix& g = node->geometry_to_world;
        const float world[16] = {static_cast<float>(g.m00), static_cast<float>(g.m10), static_cast<float>(g.m20), 0,
                                 static_cast<float>(g.m01), static_cast<float>(g.m11), static_cast<float>(g.m21), 0,
                                 static_cast<float>(g.m02), static_cast<float>(g.m12), static_cast<float>(g.m22), 0,
                                 static_cast<float>(g.m03), static_cast<float>(g.m13), static_cast<float>(g.m23), 1};
        for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
            const ufbx_mesh_part& part = mesh->material_parts.data[pi];
            if (part.num_triangles == 0) continue;
            MeshData md;
            md.name = std::string(node->name.data, node->name.length) + (mesh->material_parts.count > 1 ? "_" + std::to_string(pi) : "");
            for (const ufbx_node* a = node->parent; a && !a->is_root; a = a->parent)   // the nodes above it in the file
                if (a->name.length) md.path = md.path.empty() ? std::string(a->name.data, a->name.length) : std::string(a->name.data, a->name.length) + "/" + md.path;
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                const ufbx_face face = mesh->faces.data[part.face_indices.data[fi]];
                const uint32_t n = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
                for (uint32_t k = 0; k < n * 3; ++k) {
                    const uint32_t ix = tri[k];
                    const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                    md.positions.insert(md.positions.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
                    if (mesh->vertex_uv.exists) {
                        const ufbx_vec2 t = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                        md.uvs.insert(md.uvs.end(), {static_cast<float>(t.x), flipV ? 1.0f - static_cast<float>(t.y) : static_cast<float>(t.y)});
                    } else {
                        md.uvs.insert(md.uvs.end(), {0.0f, 0.0f});
                    }
                    if (mesh->vertex_color.exists) {
                        const ufbx_vec4 c = ufbx_get_vertex_vec4(&mesh->vertex_color, ix);
                        for (double v : {c.x, c.y, c.z, c.w}) md.colors.push_back(static_cast<u8>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5));
                    }
                    md.indices.push_back(static_cast<u32>(md.indices.size()));
                }
            }
            std::memcpy(md.worldMatrix, world, sizeof world);
            md.hasWorldMatrix = true;
            float color[4] = {1, 1, 1, 1};
            const Image* img = nullptr;
            bool blend = false;
            const ufbx_material* mat = pi < mesh->materials.count ? mesh->materials.data[pi] : nullptr;
            if (mat) {
                const ufbx_material_map& base = mat->pbr.base_color.has_value ? mat->pbr.base_color : mat->fbx.diffuse_color;
                if (base.has_value) for (int k = 0; k < 3; ++k) color[k] = static_cast<float>(base.value_vec4.v[k]);
                if (mat->pbr.base_factor.has_value && mat->pbr.base_factor.value_real > 0)
                    for (int k = 0; k < 3; ++k) color[k] *= static_cast<float>(mat->pbr.base_factor.value_real);
                img = image(mat->pbr.base_color.texture ? mat->pbr.base_color.texture : mat->fbx.diffuse_color.texture);
                if (img && img->rgba.size() > 4) color[0] = color[1] = color[2] = 1.0f;   // the texture carries the colour
                const double opacity = mat->pbr.opacity.has_value ? mat->pbr.opacity.value_real : 1.0;
                color[3] = static_cast<float>(opacity);
                blend = opacity < 0.999 || (mat->pbr.opacity.texture != nullptr);
            }
            finish(md, img, color, blend, img && img->alpha && !blend, 0.5f);
            meshes.push_back(std::move(md));
        }
    }
    ufbx_free_scene(scene);
    if (meshes.empty()) { error = "no meshes in " + path; return false; }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return true;
}

// One mesh of triangles (3 positions each, already Y-up) drawn flat white, or in its vertex colours when it has them.
void finishPlain(MeshData& md, const std::string& path) {
    md.name = fs::u8path(path).stem().u8string();
    md.uvs.resize(md.positions.size() / 3 * 2, 0.0f);
    md.indices.resize(md.positions.size() / 3);
    for (size_t i = 0; i < md.indices.size(); ++i) md.indices[i] = static_cast<u32>(i);
    const float white[4] = {1, 1, 1, 1};
    finish(md, nullptr, white, false, false, 0.5f);
}

// STL (3D printing, CAD): binary (80-byte header, a count, 50 bytes a triangle) or ASCII ("solid ... facet ...
// vertex x y z"). Its axes are Z-up, as CAD's are: turned to Y-up. No units are recorded; taken as metres (the
// Scale field fixes a millimetre file: 0.001).
bool importStl(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    const std::vector<u8> b = readFile(path);
    if (b.size() < 15) { error = "not an STL file: " + path; return false; }
    MeshData md;
    auto vertex = [&](float x, float y, float z) { md.positions.insert(md.positions.end(), {x, z, -y}); };
    uint32_t count = 0;
    if (b.size() >= 84) std::memcpy(&count, &b[80], 4);
    if (b.size() >= 84 && static_cast<uint64_t>(count) * 50 + 84 == b.size()) {   // binary: the size says so exactly
        md.positions.reserve(static_cast<size_t>(count) * 9);
        for (uint32_t t = 0; t < count; ++t) {
            float v[9];
            std::memcpy(v, &b[84 + static_cast<size_t>(t) * 50 + 12], sizeof v);   // (after the facet normal)
            for (int k = 0; k < 3; ++k) vertex(v[k * 3], v[k * 3 + 1], v[k * 3 + 2]);
        }
    } else {
        std::istringstream in(std::string(b.begin(), b.end()));
        std::string word;
        while (in >> word) {
            if (word != "vertex") continue;
            float x = 0, y = 0, z = 0;
            if (!(in >> x >> y >> z)) { error = "damaged STL file: " + path; return false; }
            vertex(x, y, z);
        }
        md.positions.resize(md.positions.size() / 9 * 9);           // only whole triangles
    }
    for (float v : md.positions) if (!std::isfinite(v)) { error = "damaged STL file (bad numbers): " + path; return false; }
    if (md.positions.empty()) { error = "no triangles in " + path; return false; }
    finishPlain(md, path);
    files.push_back(path);
    meshes.push_back(std::move(md));
    return true;
}

// PLY (scanners, photogrammetry, MeshLab): ASCII or binary, any element layout. The vertices' x y z, colours
// (red green blue [alpha], 0..255 or 0..1) and texture coordinates (s t / u v / texture_u texture_v) when present, and
// the faces' index lists, fanned into triangles. Axes as stored (scanners mostly write Y-up). A PLY with Gaussian
// splat properties, or only points, isn't a mesh: said so.
bool importPly(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    const std::vector<u8> b = readFile(path);
    const std::string bad = "damaged or unsupported PLY file: " + path;
    struct Prop { std::string name, type, countType; bool list = false; };
    struct Element { std::string name; uint64_t count = 0; std::vector<Prop> props; };
    std::vector<Element> els;
    int format = -1;                                    // 0 ascii, 1 binary little-endian, 2 binary big-endian
    size_t pos = 0;
    {   // the header: text lines up to "end_header"
        const std::string head(b.begin(), b.begin() + std::min<size_t>(b.size(), 1 << 16));
        const size_t end = head.find("end_header");
        if (head.rfind("ply", 0) != 0 || end == std::string::npos) { error = "not a PLY file: " + path; return false; }
        pos = head.find('\n', end);
        if (pos == std::string::npos) { error = bad; return false; }
        ++pos;
        std::istringstream in(head.substr(0, end));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string w;
            ls >> w;
            if (w == "format") {
                std::string f;
                ls >> f;
                format = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1;
            } else if (w == "element") {
                Element e;
                ls >> e.name >> e.count;
                els.push_back(e);
            } else if (w == "property" && !els.empty()) {
                Prop p;
                ls >> p.type;
                if (p.type == "list") { p.list = true; ls >> p.countType >> p.type; }
                ls >> p.name;
                els.back().props.push_back(p);
            }
        }
    }
    if (format < 0) { error = bad; return false; }
    for (const Element& e : els)
        for (const Prop& p : e.props)
            if (e.name == "vertex" && (p.name == "f_dc_0" || p.name == "scale_0")) {
                error = "this PLY holds Gaussian splats, which Frame Studio can't open yet: " + path;
                return false;
            }
    auto size = [](const std::string& t) -> int {
        if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
        if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
        if (t == "int" || t == "uint" || t == "int32" || t == "uint32" || t == "float" || t == "float32") return 4;
        if (t == "double" || t == "float64") return 8;
        return 0;
    };
    std::istringstream ascii(format == 0 ? std::string(b.begin() + pos, b.end()) : std::string());
    bool ok = true;
    auto read = [&](const std::string& t) -> double {   // one value of type t, at pos (binary) or the next word (ASCII)
        if (format == 0) { double v = 0; if (!(ascii >> v)) ok = false; return v; }
        const int n = size(t);
        if (n == 0 || pos + n > b.size()) { ok = false; return 0; }
        u8 raw[8];
        std::memcpy(raw, &b[pos], n);
        pos += n;
        if (format == 2) std::reverse(raw, raw + n);
        if (t == "char" || t == "int8") return static_cast<int8_t>(raw[0]);
        if (t == "uchar" || t == "uint8") return raw[0];
        if (t == "short" || t == "int16") { int16_t v; std::memcpy(&v, raw, 2); return v; }
        if (t == "ushort" || t == "uint16") { uint16_t v; std::memcpy(&v, raw, 2); return v; }
        if (t == "int" || t == "int32") { int32_t v; std::memcpy(&v, raw, 4); return v; }
        if (t == "uint" || t == "uint32") { uint32_t v; std::memcpy(&v, raw, 4); return v; }
        if (t == "float" || t == "float32") { float v; std::memcpy(&v, raw, 4); return v; }
        double v; std::memcpy(&v, raw, 8); return v;
    };
    std::vector<float> xyz, uv;
    std::vector<u8> rgba;
    std::vector<u32> tris;
    bool colours = false, coords = false;
    for (const Element& e : els) {
        // a hostile count: never more items than bytes left (each takes at least one)
        if (e.count > b.size()) { error = bad; return false; }
        const bool vert = e.name == "vertex", face = e.name == "face";
        if (vert) {
            xyz.reserve(e.count * 3);
            for (const Prop& p : e.props) {
                colours |= p.name == "red";
                coords |= p.name == "s" || p.name == "u" || p.name == "texture_u";
            }
        }
        for (uint64_t i = 0; i < e.count && ok; ++i) {
            float v[3] = {0, 0, 0}, t[2] = {0, 0}, c[4] = {1, 1, 1, 1};
            for (const Prop& p : e.props) {
                if (p.list) {
                    const double n = read(p.countType);
                    if (!ok || n < 0 || n > 1024) { ok = false; break; }
                    std::vector<u32> poly(static_cast<size_t>(n));
                    for (u32& k : poly) {   // (a negative, huge or NaN index is no vertex: refused below, never an out-of-range cast)
                        const double d = read(p.type);
                        k = d >= 0 && d < 4294967295.0 ? static_cast<u32>(d) : UINT32_MAX;
                    }
                    if (face && (p.name == "vertex_indices" || p.name == "vertex_index"))
                        for (size_t k = 2; k < poly.size(); ++k) tris.insert(tris.end(), {poly[0], poly[k - 1], poly[k]});
                    continue;
                }
                const double x = read(p.type);
                if (!vert) continue;
                const bool byteColour = size(p.type) == 1;
                if (p.name == "x") v[0] = static_cast<float>(x);
                else if (p.name == "y") v[1] = static_cast<float>(x);
                else if (p.name == "z") v[2] = static_cast<float>(x);
                else if (p.name == "s" || p.name == "u" || p.name == "texture_u") t[0] = static_cast<float>(x);
                else if (p.name == "t" || p.name == "v" || p.name == "texture_v") t[1] = static_cast<float>(x);
                else if (p.name == "red" || p.name == "green" || p.name == "blue" || p.name == "alpha") {
                    // 0..255 (uchar), 0..65535 (ushort) or 0..1 (float)
                    const double unit = byteColour ? x / 255 : size(p.type) == 2 && p.type.find('u') == 0 ? x / 65535 : x;
                    c[p.name == "red" ? 0 : p.name == "green" ? 1 : p.name == "blue" ? 2 : 3] = static_cast<float>(unit);
                }
            }
            if (vert) {
                xyz.insert(xyz.end(), v, v + 3);
                uv.insert(uv.end(), {t[0], 1.0f - t[1]});
                // Scanners and MeshLab store display (sRGB) colours; the shader works in linear light, as glTF's COLOR_0 is.
                for (int k = 0; k < 3; ++k) {
                    const float s = std::clamp(c[k], 0.0f, 1.0f);
                    c[k] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
                }
                for (float k : c) rgba.push_back(static_cast<u8>(std::clamp(k, 0.0f, 1.0f) * 255.0f + 0.5f));
            }
        }
        if (!ok) { error = bad; return false; }
    }
    for (float v : xyz) if (!std::isfinite(v)) { error = "damaged PLY file (bad numbers): " + path; return false; }
    if (tris.empty()) { error = "this PLY has only points (a point cloud), not faces: Frame Studio can't open those yet: " + path; return false; }
    MeshData md;
    for (u32 k : tris) {
        if (k >= xyz.size() / 3) { error = bad; return false; }
        md.positions.insert(md.positions.end(), &xyz[k * 3], &xyz[k * 3] + 3);
        if (colours) md.colors.insert(md.colors.end(), &rgba[k * 4], &rgba[k * 4] + 4);
    }
    finishPlain(md, path);
    for (size_t i = 3; i < md.colors.size() && !md.useBlend; i += 4) md.useBlend = md.colors[i] < 250;   // see-through vertices: blended
    if (coords) for (size_t i = 0; i < tris.size(); ++i) { md.uvs[i * 2] = uv[tris[i] * 2]; md.uvs[i * 2 + 1] = uv[tris[i] * 2 + 1]; }
    files.push_back(path);
    meshes.push_back(std::move(md));
    return true;
}

// A panorama sky (equirectangular: 360 x 180 degrees, width = 2 x height) made into the six faces of a sky cube, each
// a quad with its own picture, drawn as a home's own skybox is (the unit cube follows the head, behind everything).
// The middle of the picture faces -Z (ahead). Radiance .hdr is shown at its own exposure (clipped at white).
bool importSkyPanorama(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    const std::vector<u8> b = readFile(path);
    if (b.empty()) { error = "can't read the panorama " + path; return false; }
    if (!sizeOk(b.data(), b.size())) { error = "not a picture Frame Studio can read (.hdr, .jpg, .png), or larger than 16384 x 8192: " + path; return false; }
    const bool hdr = b.size() > 10 && (std::memcmp(b.data(), "#?RADIANCE", 10) == 0 || std::memcmp(b.data(), "#?RGBE", 6) == 0);
    int w = 0, h = 0, comp = 0;
    std::vector<float> px;                                   // RGB: linear (HDR) or 0..1 as stored (sRGB pictures)
    if (hdr) {
        float* f = stbi_loadf_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &comp, 3);
        if (f) { px.assign(f, f + static_cast<size_t>(w) * h * 3); stbi_image_free(f); }
    } else {
        unsigned char* c = stbi_load_from_memory(b.data(), static_cast<int>(b.size()), &w, &h, &comp, 3);
        if (c) {
            px.resize(static_cast<size_t>(w) * h * 3);
            for (size_t i = 0; i < px.size(); ++i) px[i] = c[i] / 255.0f;
            stbi_image_free(c);
        }
    }
    if (px.empty() || w < 8 || h < 4) { error = "not a picture Frame Studio can read (.hdr, .jpg, .png): " + path; return false; }
    // A face spans 90 degrees = a quarter of the width; at most 2048 (8K panoramas), at least 64.
    const u32 n = static_cast<u32>(std::clamp(w / 4, 64, 2048));
    auto sample = [&](const float d[3], float out[3]) {     // bilinear, wrapping round, clamped at the poles
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float lon = std::atan2(d[0], -d[2]), lat = std::asin(std::clamp(d[1] / len, -1.0f, 1.0f));
        const float x = (0.5f + lon / 6.2831853f) * w - 0.5f, y = (0.5f - lat / 3.14159265f) * h - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        auto at = [&](int xx, int yy, int c) {
            xx = ((xx % w) + w) % w;
            yy = std::clamp(yy, 0, h - 1);
            return px[(static_cast<size_t>(yy) * w + xx) * 3 + c];
        };
        for (int c = 0; c < 3; ++c)
            out[c] = (at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx) * (1 - fy) + (at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx) * fy;
    };
    struct Face { const char* name; float f[3], r[3], u[3]; };
    static const Face faces[6] = {
        {"ahead", {0, 0, -1}, {1, 0, 0}, {0, 1, 0}},  {"behind", {0, 0, 1}, {-1, 0, 0}, {0, 1, 0}},
        {"right", {1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   {"left", {-1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {"up", {0, 1, 0}, {1, 0, 0}, {0, 0, 1}},      {"down", {0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    };
    const std::string stem = fs::u8path(path).stem().stem().u8string();   // ("dusk.sky.hdr": "dusk")
    const float white[4] = {1, 1, 1, 1};
    for (const Face& fc : faces) {
        Image img;
        img.w = img.h = n;
        img.rgba.resize(static_cast<size_t>(n) * n * 4);
        // Texel centres run edge to edge (the outer ones ON the cube's edges) and the quad's UVs stop half a texel in,
        // so neighbouring faces meet on the same colours: no seam, and the wrapping sampler never reaches across.
        for (u32 j = 0; j < n; ++j)
            for (u32 i = 0; i < n; ++i) {
                const float a = -1.0f + 2.0f * i / (n - 1), bb = 1.0f - 2.0f * j / (n - 1);
                const float d[3] = {fc.f[0] + a * fc.r[0] + bb * fc.u[0], fc.f[1] + a * fc.r[1] + bb * fc.u[1], fc.f[2] + a * fc.r[2] + bb * fc.u[2]};
                float c[3];
                sample(d, c);
                u8* o = &img.rgba[(static_cast<size_t>(j) * n + i) * 4];
                for (int k = 0; k < 3; ++k) {
                    float s = std::clamp(c[k], 0.0f, 1.0f);
                    if (hdr) s = s <= 0.0031308f ? s * 12.92f : 1.055f * std::pow(s, 1.0f / 2.4f) - 0.055f;   // linear -> sRGB
                    o[k] = static_cast<u8>(s * 255.0f + 0.5f);
                }
                o[3] = 255;
            }
        MeshData md;
        md.name = "Sky " + std::string(fc.name);
        md.path = stem + " (panorama sky)";
        const float hf = 0.5f / n;
        const float corner[4][4] = {{-1, 1, hf, hf}, {1, 1, 1 - hf, hf}, {1, -1, 1 - hf, 1 - hf}, {-1, -1, hf, 1 - hf}};
        for (const auto& q : corner) {
            for (int k = 0; k < 3; ++k) md.positions.push_back(fc.f[k] + q[0] * fc.r[k] + q[1] * fc.u[k]);
            md.uvs.insert(md.uvs.end(), {q[2], q[3]});
        }
        md.indices = {0, 2, 1, 0, 3, 2};
        finish(md, &img, white, false, false, 0.5f);
        md.isSkybox = true;
        meshes.push_back(std::move(md));
    }
    files.push_back(path);
    return true;
}

}  // namespace

bool isSkyPanoramaPath(const std::string& path) {
    const std::string name = lower(fs::u8path(path).filename().u8string());
    for (const char* e : {".sky.hdr", ".sky.jpg", ".sky.jpeg", ".sky.png"})
        if (name.size() > std::strlen(e) && name.compare(name.size() - std::strlen(e), std::string::npos, e) == 0) return true;
    return false;
}

bool isModelPath(const std::string& path) {
    const std::string ext = lower(fs::u8path(path).extension().u8string());
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj" || ext == ".stl" || ext == ".ply" || isSkyPanoramaPath(path);
}

bool importModel(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    // Each runs on a loader thread of its own: a model too big for memory (bad_alloc, length_error) must come back as
    // an error, not escape the thread and end the program (with the open home's unsaved changes).
    const size_t meshesBefore = meshes.size(), filesBefore = files.size();
    try {
        if (isSkyPanoramaPath(path)) return importSkyPanorama(path, meshes, files, error);
        const std::string ext = lower(fs::u8path(path).extension().u8string());
        if (ext == ".gltf" || ext == ".glb") return importGltf(path, meshes, files, error);
        if (ext == ".fbx" || ext == ".obj") return importUfbx(path, meshes, files, error);
        if (ext == ".stl") return importStl(path, meshes, files, error);
        if (ext == ".ply") return importPly(path, meshes, files, error);
        error = "unsupported model type: " + path;
        return false;
    } catch (const std::exception&) {
        meshes.resize(meshesBefore);
        files.resize(filesBefore);
        error = "not enough memory to open " + path;
        return false;
    }
}
