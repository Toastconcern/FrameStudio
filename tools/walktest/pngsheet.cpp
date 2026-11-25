// pngsheet: the same square cut from several pictures, scaled up and set side by side (before/after crops).
// pngsheet <out.png> <x> <y> <radius> <scale> <in.png> [in.png ...]   (a missing picture leaves its square grey)
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 7) { std::fprintf(stderr, "usage: pngsheet <out.png> <x> <y> <radius> <scale> <in.png>...\n"); return 2; }
    const int cx = std::atoi(argv[2]), cy = std::atoi(argv[3]), R = std::max(1, std::atoi(argv[4])), S = std::max(1, std::atoi(argv[5]));
    const int n = argc - 6, cw = 2 * R * S, gap = 4, ow = n * cw + (n - 1) * gap;
    std::vector<unsigned char> o(static_cast<size_t>(ow) * cw * 3, 255);
    for (int i = 0; i < n; ++i) {
        int w = 0, h = 0, c = 0;
        unsigned char* img = nullptr;
        if (FILE* f = std::fopen(argv[6 + i], "rb")) {   // (the library's stb_image has no stdio: read it here)
            std::vector<unsigned char> bytes;
            unsigned char buf[65536];
            for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) bytes.insert(bytes.end(), buf, buf + got);
            std::fclose(f);
            img = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &c, 3);
        }
        for (int y = 0; y < cw; ++y)
            for (int x = 0; x < cw; ++x) {
                unsigned char* d = &o[(static_cast<size_t>(y) * ow + i * (cw + gap) + x) * 3];
                if (!img) { d[0] = d[1] = d[2] = 128; continue; }
                const int sx = std::clamp(cx - R + x / S, 0, w - 1), sy = std::clamp(cy - R + y / S, 0, h - 1);
                const unsigned char* s = &img[(static_cast<size_t>(sy) * w + sx) * 3];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        if (img) stbi_image_free(img);
    }
    return stbi_write_png(argv[1], ow, cw, 3, o.data(), ow * 3) ? 0 : 1;
}
