// spv_check.h: every shader Frame Home ships passes, and damaged ones (cut short, an instruction running past the end,
// ids past the bound or defined twice, an unterminated string, a function never ended) are caught before the driver.
// Files given on the command line are checked too (all must pass) and timed: tests_loco/spv_check_test *.spv
#include "spv_check.h"
#include "clay_spirv.h"
#include "debug_spirv.h"
#include "guide_spirv.h"
#include "selection_spirv.h"
#include "splat_spirv.h"

#include <chrono>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

static int fails = 0;
static void expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

template <size_t N> static std::vector<uint32_t> words(const uint32_t (&a)[N]) { return std::vector<uint32_t>(a, a + N); }

int main(int argc, char** argv) {
    const std::vector<std::vector<uint32_t>> shipped = {
        words(kClayVertSpirv), words(kClayFragSpirv), words(kClayFragCutoutSpirv), words(kClaySkinnedVertSpirv), words(kClayVColorVertSpirv),
        words(kDebugVertSpirv), words(kDebugFragSpirv), words(kGuideVertSpirv), words(kGuideFragSpirv), words(kSelectionVertSpirv),
        words(kSelectionFragSpirv), words(kSplatVertSpirv), words(kSplatFragSpirv)};
    bool all = true;
    std::string why;
    for (const auto& s : shipped) {
        if (!spvcheck::check(s, &why)) { all = false; std::printf("  shipped shader refused: %s\n", why.c_str()); }
    }
    expect(all, "every built-in shader passes");

    const std::vector<uint32_t>& base = shipped[1];   // the Clay fragment shader
    auto refused = [&](std::vector<uint32_t> code, const char* what) {
        std::string w;
        const bool ok = spvcheck::check(code, &w);
        expect(!ok, what);
        if (!ok) std::printf("  (%s)\n", w.c_str());
    };
    refused({}, "empty: refused");
    refused(std::vector<uint32_t>(base.begin(), base.begin() + 4), "cut inside the header: refused");
    { auto c = base; c[0] = 0x03022307u; refused(c, "byte-swapped magic: refused"); }
    { auto c = base; c[1] = 0x00020000u; refused(c, "SPIR-V 2.0: refused"); }
    { auto c = base; c[3] = 0; refused(c, "id bound 0: refused"); }
    { auto c = base; c[3] = 0xFFFFFFFFu; refused(c, "id bound 4G: refused"); }
    { auto c = base; c[3] = 2; refused(c, "ids past a too-small bound: refused"); }
    { auto c = base; c.resize(c.size() - 1); refused(c, "last instruction cut short: refused"); }
    { auto c = base; c[5] = (0xFFFFu << 16) | (c[5] & 0xFFFFu); refused(c, "an instruction longer than the module: refused"); }
    { auto c = base; c[5] = c[5] & 0xFFFFu; refused(c, "an instruction of 0 words: refused"); }
    {   // the first result id another instruction defines again: find two instructions with results, copy the id
        auto c = base;
        std::vector<size_t> withResult;
        for (size_t i = 5; i < c.size(); i += c[i] >> 16)
            if (spvcheck::detail::opFlags(c[i] & 0xFFFF) == 1) withResult.push_back(i);   // (a result, no type: at word 1)
        expect(withResult.size() >= 2, "(the test shader has two results to make clash)");
        if (withResult.size() >= 2) {
            c[withResult[1] + 1] = c[withResult[0] + 1];
            refused(c, "a result id defined twice: refused");
        }
    }
    {   // the OpEntryPoint's name with its nul overwritten
        auto c = base;
        for (size_t i = 5; i < c.size(); i += c[i] >> 16)
            if ((c[i] & 0xFFFF) == 15) { for (uint32_t k = 3; k < (c[i] >> 16); ++k) c[i + k] = 0x41414141u; break; }
        refused(c, "an entry point name never terminated: refused");
    }
    {   // the module ends inside its function (OpFunctionEnd dropped)
        auto c = base;
        if ((c.back() & 0xFFFF) == 56) { c.pop_back(); refused(c, "a function never ended: refused"); }
    }

    // Shaders given as files (a home's, SteamVR's): all must pass. Timed.
    size_t n = 0, totalWords = 0, bad = 0;
    double ms = 0;
    for (int a = 1; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb");
        if (!f) continue;
        std::vector<uint32_t> c;
        uint32_t w;
        while (std::fread(&w, 4, 1, f) == 1) c.push_back(w);
        std::fclose(f);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = spvcheck::check(c, &why);
        ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++n;
        totalWords += c.size();
        if (!ok) { ++bad; std::printf("  %s refused: %s\n", argv[a], why.c_str()); }
    }
    if (n) {
        std::printf("checked %zu files, %.1f MB of SPIR-V, in %.2f ms\n", n, totalWords * 4 / 1e6, ms);
        expect(bad == 0, "every given shader passes");
    }
    std::printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
