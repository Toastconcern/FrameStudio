#pragma once
// Meta's shaders made cheaper for the GPU before the driver sees them, two ways, both off unless asked for:
//   - FH_SPV_OPT=1: the SPIRV-Tools optimizer's performance recipe (third_party/spirv-tools, built with
//     -DFRAME_SPV_OPT=ON): inlining, dead code removal, constant folding of what spv_specialize.h baked in, copy
//     propagation, if-conversion ... The picture stays the same (at most 1/255 apart), but on the Frame it saved
//     nothing: Turnip's own compiler already does all of it (the same fragment instructions, the same GPU cycles in
//     every home measured, 2026-10-24), while making the modules cost ~22 ms each as a home loaded (4 s for Haven).
//   - FH_SPV_HALF=1: fragment shaders' colour math (what is made from texels and constants alone) marked
//     RelaxedPrecision (Turnip then runs it in 16 bits), never what feeds a texture lookup, a derivative or the depth;
//     =2 also rewrites it as 16-bit floats (SPIRV-Tools). =1 keeps the picture the same (at most 3/255 apart, alone
//     or with FH_SPV_OPT=1); =2 leaves a few specks (up to 3 pixels in a million off by as much as 104/255), so it
//     stays experimental. On the Frame neither saved anything (GPU cycles within +-1% in 9 homes, 2026-10-25): Meta's shaders already mark
//     their own colour math relaxed. Relaxing all float math did save 8% in Desert Terrace, but turned Haven's walls
//     black.
// Every mode's output passes spirv-val (all 198 of Haven Central's shipped and specialized shaders).
// A module made this way carries a mark (an OpSourceExtension "frame-spvopt <flags>"), so one baked into a .fenv by
// Frame Studio with the same switches isn't made again when it loads, and one made with others is made again.
#include <cstdint>
#include <string>
#include <vector>

namespace spvopt {

enum : int {
    kOptimize = 1,   // the performance recipe
    kRelax = 2,      // fragment shaders: colour math RelaxedPrecision (relax() in spv_opt.cpp)
    kHalf = 4,       // ... and the relaxed math rewritten as 16-bit floats (with kRelax; experimental)
};

// What's on: kOptimize with FH_SPV_OPT=1, kRelax with FH_SPV_HALF=1, kRelax|kHalf with =2 (kOptimize and kHalf only
// in a build with SPIRV-Tools).
int flags();

// The flags for one program's shaders (its path in the home): flags(), less kRelax and kHalf when FH_SPV_HALF_ONLY is
// set and the path doesn't contain it (diagnostics: one shader family at a time).
int flagsFor(const std::string& programPath);

// `code` made with `flags` (flags() by default); `code` itself when it already carries the same mark, when there is
// nothing to do, or when the optimizer fails (`err` says why). Thread-safe; results are cached by content.
std::vector<uint32_t> run(const std::vector<uint32_t>& code, int flags, std::string* err = nullptr);
inline std::vector<uint32_t> run(const std::vector<uint32_t>& code) { return run(code, flags()); }
inline std::vector<uint32_t> run(const std::vector<uint32_t>& code, const std::string& programPath) { return run(code, flagsFor(programPath)); }

// The flags a module was made with (its mark), -1 when it carries none.
int markOf(const std::vector<uint32_t>& code);

// Totals since start: modules made, cache hits, failures, and the milliseconds spent making them (all threads).
struct Stats { uint32_t made = 0, cached = 0, failed = 0; double ms = 0; };
Stats stats();

}  // namespace spvopt
