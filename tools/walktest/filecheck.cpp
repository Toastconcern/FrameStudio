// Turns a home's cooked files back into what they were, as Frame Studio's Files section does: the first texture as a
// PNG and the first mesh as an OBJ (into the folder given), with what was read.
#include "loaders/rendtxtr_parser.h"
#include "loaders/rendmesh_parser.h"
#include "miniz.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <cstdio>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_file(&z, argv[1], 0)) return 1;
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, "assets/scene.zip", &n, 0);
    mz_zip_reader_end(&z);
    if (!p) return 1;
    std::vector<u8> sceneZip(static_cast<u8*>(p), static_cast<u8*>(p) + n);
    mz_free(p);
    mz_zip_archive in = {};
    if (!mz_zip_reader_init_mem(&in, sceneZip.data(), sceneZip.size(), 0)) return 1;
    bool tex = false, mesh = false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in) && !(tex && mesh); ++i) {
        mz_zip_archive_file_stat st;
        mz_zip_reader_file_stat(&in, i, &st);
        const std::string name = st.m_filename;
        const bool isTex = !tex && name.find(".png/") != std::string::npos && name.find("/tex_") != std::string::npos;
        const bool isMesh = !mesh && name.find("/__mesh_sub_targets") != std::string::npos && name.find("__material_sub_targets") == std::string::npos;
        if (!isTex && !isMesh) continue;
        std::vector<u8> b(static_cast<size_t>(st.m_uncomp_size));
        mz_zip_reader_extract_to_mem(&in, i, b.data(), b.size(), 0);
        if (isTex) {
            RendtxtrInfo ti;
            std::vector<u8> rgba;
            const bool ok = parseRendtxtrHeader(b, ti) && astc::decodeASTC(b.data() + ti.rawDataOffset, ti.rawDataLen, ti.width, ti.height, ti.blockW, ti.blockH, rgba, false);
            const std::string out = std::string(argv[2]) + "/texture.png";
            if (ok) stbi_write_png(out.c_str(), static_cast<int>(ti.width), static_cast<int>(ti.height), 4, rgba.data(), static_cast<int>(ti.width) * 4);
            std::printf("texture %s: %s %ux%u\n", name.c_str(), ok ? "PNG" : "FAILED", ti.width, ti.height);
            tex = true;
        } else {
            std::vector<float> pos, uv;
            std::vector<u32> idx;
            const bool ok = parseRendMesh(b, pos, uv, idx);
            std::printf("mesh %s: %s %zu vertices, %zu triangles, %zu uvs\n", name.c_str(), ok ? "read" : "FAILED", pos.size() / 3, idx.size() / 3, uv.size() / 2);
            mesh = true;
        }
    }
    mz_zip_reader_end(&in);
    return 0;
}
