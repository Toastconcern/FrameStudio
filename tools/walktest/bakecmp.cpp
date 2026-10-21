// Two homes (APK or .fenv) baked as they load (scene_bake.h, no optimizer), byte for byte: a .fenv's scene re-baked from
// it is the bake of its APK when nothing was lost (an older build's .fenv: nothing changed in what it stores).
//   bakecmp <home.apk|home.fenv> <home.apk|home.fenv> [t]      exit 0: identical, 2: different
// t: the first home's animation is played to t seconds first, as Frame Studio has it when it saves: baked as it is then,
// and (Studio's optimizer path) after animbake::toStart; both must still be the second's bake.
#include "hsl_scene.h"
#include "scene_bake.h"
#include "anim_bake.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static bool same(const std::vector<u8>& a, const std::vector<u8>& b, const char* what) {
    size_t at = 0;
    while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
    if (at == a.size() && at == b.size()) { std::printf("IDENTICAL%s: %zu bytes\n", what, a.size()); return true; }
    std::printf("DIFFERENT%s: %zu vs %zu bytes, first difference at byte %zu\n", what, a.size(), b.size(), at);
    return false;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: bakecmp <home> <home> [t]\n"); return 1; }
    HslScene s[2];
    std::vector<u8> blob[2];
    for (int i = 0; i < 2; ++i) {
        std::string err;
        if (!loadHslScene(std::string(argv[1 + i]), s[i], err)) { std::fprintf(stderr, "%s: %s\n", argv[1 + i], err.c_str()); return 1; }
        if (i == 0 && argc > 3 && s[0].animate) s[0].animate(static_cast<float>(std::atof(argv[3])), s[0].meshes);
        if (!bakeScene(s[i], blob[i], err)) { std::fprintf(stderr, "%s: %s\n", argv[1 + i], err.c_str()); return 1; }
    }
    bool ok = same(blob[0], blob[1], argc > 3 ? " (played, then baked)" : "");
    if (argc > 3 && s[0].saveAnimation) {
        HslScene copy = s[0];
        animbake::toStart(copy);
        std::vector<u8> b;
        std::string err;
        if (!bakeScene(copy, b, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        ok &= same(b, blob[1], " (played, put back, baked)");
    }
    return ok ? 0 : 2;
}
