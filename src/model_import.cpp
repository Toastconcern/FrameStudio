#include "model_import.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "ufbx.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req_comp);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

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

struct Image { std::vector<u8> rgba; u32 w = 0, h = 0; bool alpha = false; };

bool decode(const u8* data, size_t n, Image& img) {
    int w = 0, h = 0, comp = 0;
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
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) { error = "not a readable glTF file: " + path; return false; }
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) { cgltf_free(data); error = "glTF buffers missing next to " + path; return false; }
    // Accessor / buffer-view ranges, index ranges and node loops are only checked here; the reads below trust them.
    if (cgltf_validate(data) != cgltf_result_success) { cgltf_free(data); error = "malformed glTF (failed validation): " + path; return false; }
    files.push_back(path);
    const fs::path dir = fs::u8path(path).parent_path();
    for (cgltf_size b = 0; b < data->buffers_count; ++b)
        if (data->buffers[b].uri && std::strncmp(data->buffers[b].uri, "data:", 5) != 0) files.push_back((dir / fs::u8path(data->buffers[b].uri)).u8string());
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
                const size_t len = std::strlen(comma + 1), size = len / 4 * 3;
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
            md.positions.resize(pos->count * 3);
            md.uvs.assign(pos->count * 2, 0.0f);
            for (cgltf_size v = 0; v < pos->count; ++v) {
                cgltf_accessor_read_float(pos, v, &md.positions[v * 3], 3);
                if (uv) cgltf_accessor_read_float(uv, v, &md.uvs[v * 2], 2);
            }
            if (col) {
                md.colors.resize(pos->count * 4);
                for (cgltf_size v = 0; v < pos->count; ++v) {
                    float c[4] = {1, 1, 1, 1};
                    cgltf_accessor_read_float(col, v, c, cgltf_num_components(col->type));
                    for (int k = 0; k < 4; ++k) md.colors[v * 4 + k] = static_cast<u8>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
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

}  // namespace

bool isModelPath(const std::string& path) {
    const std::string ext = lower(fs::u8path(path).extension().u8string());
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj";
}

bool importModel(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error) {
    const std::string ext = lower(fs::u8path(path).extension().u8string());
    if (ext == ".gltf" || ext == ".glb") return importGltf(path, meshes, files, error);
    if (ext == ".fbx" || ext == ".obj") return importUfbx(path, meshes, files, error);
    error = "unsupported model type: " + path;
    return false;
}
