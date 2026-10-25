// spv_opt.h: Meta's shaders through the SPIRV-Tools optimizer, or made relaxed precision.
#include "spv_opt.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#ifdef FH_HAVE_SPV_OPT
#include "spirv-tools/optimizer.hpp"
#endif

namespace spvopt {

namespace {

constexpr const char* kMark = "frame-spvopt ";

enum : uint32_t {
    OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7,
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypePointer = 32, OpFunction = 54,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67,
    OpDecorate = 71, OpMemberDecorate = 72, OpDecorationGroup = 73, OpGroupDecorate = 74, OpGroupMemberDecorate = 75,
    OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82, OpCopyObject = 83,
    OpImageSampleImplicitLod = 87, OpImageGather = 96, OpImageDrefGather = 97, OpImageRead = 98, OpImageQueryLod = 105,
    OpConvertSToF = 111, OpConvertUToF = 112, OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136,
    OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142, OpDot = 148, OpSelect = 169, OpDPdx = 207, OpFwidthCoarse = 215,
    OpPhi = 245, OpImageSparseSampleImplicitLod = 305, OpImageSparseRead = 320, OpModuleProcessed = 330,
    OpExecutionModeId = 331, OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633,
    DecRelaxedPrecision = 0, DecBuiltIn = 11, BuiltInFragDepth = 22, ModelFragment = 4, StorageFunction = 7,
    OpUndef = 1, OpConstantTrue = 41, OpConstantNull = 46,
    GlslFAbs = 4, GlslFloor = 8, GlslCeil = 9, GlslFract = 10, GlslPow = 26, GlslExp2 = 29, GlslSqrt = 31, GlslFMin = 37,
    GlslFMax = 40, GlslFClamp = 43, GlslFMix = 46, GlslStep = 48, GlslSmoothStep = 49, GlslLength = 66, GlslNormalize = 69,
};

bool preamble(uint32_t op) {   // capabilities .. debug names: where the mark is looked for
    return op == OpSourceContinued || op == OpSource || op == OpSourceExtension || op == OpName || op == OpMemberName ||
           op == OpString || op == OpExtension || op == OpExtInstImport || op == OpMemoryModel || op == OpEntryPoint ||
           op == OpExecutionMode || op == OpCapability || op == OpModuleProcessed || op == OpExecutionModeId;
}
bool annotation(uint32_t op) {
    return (op >= OpDecorate && op <= OpGroupMemberDecorate) || op == OpDecorateId || op == OpDecorateString ||
           op == OpMemberDecorateString;
}

// The words of a literal string, nul-terminated and padded.
std::vector<uint32_t> literal(const std::string& s) {
    std::vector<uint32_t> w(s.size() / 4 + 1, 0);
    std::memcpy(w.data(), s.data(), s.size());
    return w;
}

// `code` with its mark: an OpSourceExtension, after the module's own OpString/OpSource* and before its debug names
// (where the spec puts it). Not OpModuleProcessed, the instruction meant for it: that needs SPIR-V 1.1, and Meta's
// shaders are 1.0 (spirv-val rejects them with one).
std::vector<uint32_t> withMark(const std::vector<uint32_t>& code, int flags) {
    size_t at = 5;
    while (at < code.size() && (code[at] >> 16) && preamble(code[at] & 0xFFFF) && (code[at] & 0xFFFF) != OpName &&
           (code[at] & 0xFFFF) != OpMemberName && (code[at] & 0xFFFF) != OpModuleProcessed)
        at += code[at] >> 16;
    const std::vector<uint32_t> s = literal(kMark + std::to_string(flags));
    std::vector<uint32_t> out(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(at));
    out.push_back(((1u + static_cast<uint32_t>(s.size())) << 16) | OpSourceExtension);
    out.insert(out.end(), s.begin(), s.end());
    out.insert(out.end(), code.begin() + static_cast<std::ptrdiff_t>(at), code.end());
    return out;
}

uint64_t hashOf(const std::vector<uint32_t>& code, int flags) {
    uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(flags);
    for (uint32_t w : code) h = (h ^ w) * 0x100000001B3ull;
    return h ^ code.size();
}

// kRelax: a fragment shader's colour math decorated RelaxedPrecision (16-bit is enough; see "Relaxed" below), but
// never a value that reaches a texture lookup's coordinates (or lod, gradients, offsets, depth reference), a
// derivative, or the depth output: half precision on a UV is a visibly blocky texture, and on a derivative a wrong
// mip. Everything stored to a function variable that a kept value is loaded from is kept too. The input when it
// isn't a fragment shader; empty when the module can't be read.
std::vector<uint32_t> relax(const std::vector<uint32_t>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return {};
    struct Ins { size_t at; uint32_t op, words; };
    std::vector<Ins> ins;
    for (size_t i = 5; i < code.size();) {
        const uint32_t words = code[i] >> 16;
        if (!words || i + words > code.size()) return {};
        ins.push_back({i, code[i] & 0xFFFF, words});
        i += words;
    }
    bool fragment = false;
    uint32_t glsl = 0, entryPoints = 0;
    std::set<uint32_t> float32, relaxedAlready, fragDepth;
    std::map<uint32_t, uint32_t> varStorage;        // variable -> storage class
    std::map<uint32_t, size_t> def;                 // result id -> instruction index (function bodies)
    std::map<uint32_t, uint32_t> root;              // pointer -> the variable it points into
    std::map<uint32_t, std::vector<uint32_t>> storedTo;   // variable -> the values stored to it
    bool inFunction = false;
    for (size_t k = 0; k < ins.size(); ++k) {
        const uint32_t* w = &code[ins[k].at];
        const uint32_t op = ins[k].op, n = ins[k].words;
        if (op == OpEntryPoint && n >= 2) { ++entryPoints; fragment = w[1] == ModelFragment; }
        else if (op == OpExtInstImport && n >= 3 && !std::strncmp(reinterpret_cast<const char*>(w + 2), "GLSL.std.450", 12)) glsl = w[1];
        else if (op == OpTypeFloat && n >= 3 && w[2] == 32) float32.insert(w[1]);
        else if ((op == OpTypeVector || op == OpTypeMatrix) && n >= 4 && float32.count(w[2])) float32.insert(w[1]);
        else if (op == OpDecorate && n >= 3 && w[2] == DecRelaxedPrecision) relaxedAlready.insert(w[1]);
        else if (op == OpDecorate && n >= 4 && w[2] == DecBuiltIn && w[3] == BuiltInFragDepth) fragDepth.insert(w[1]);
        else if (op == OpVariable && n >= 4) { varStorage[w[2]] = w[3]; root[w[2]] = w[2]; }
        if (op == OpFunction) inFunction = true;
        if (!inFunction) continue;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain) && n >= 4 && root.count(w[3])) root[w[2]] = root[w[3]];
        if (op == OpStore && n >= 3 && root.count(w[1])) storedTo[root[w[1]]].push_back(w[2]);
        const bool noResult = op == OpStore || op == 63 /*CopyMemory*/ || op == 99 /*ImageWrite*/ || (op >= 246 && op <= 255) ||   // (merges, branches, returns)
                              op == 8 /*Line*/ || op == 224 || op == 225 /*barriers*/;
        if (n >= 3 && !noResult) def[w[2]] = k;  // (a result id, for every op that has one)
    }
    if (!fragment || entryPoints != 1) return code;
    // The values to keep at full precision, and everything they are made from.
    std::set<uint32_t> keep;
    std::vector<uint32_t> work;
    auto seed = [&](uint32_t id) { if (keep.insert(id).second) work.push_back(id); };
    for (const Ins& in : ins) {
        const uint32_t* w = &code[in.at];
        const bool image = (in.op >= OpImageSampleImplicitLod && in.op <= OpImageRead) || in.op == OpImageQueryLod ||
                           (in.op >= OpImageSparseSampleImplicitLod && in.op <= OpImageSparseRead);
        if (image) for (uint32_t j = 4; j < in.words; ++j) seed(w[j]);
        if (in.op >= OpDPdx && in.op <= OpFwidthCoarse && in.words >= 4) seed(w[3]);
        if (in.op == OpStore && in.words >= 3 && root.count(w[1]) && fragDepth.count(root[w[1]])) seed(w[2]);
    }
    while (!work.empty()) {
        const uint32_t id = work.back();
        work.pop_back();
        const auto d = def.find(id);
        if (d == def.end()) continue;
        const Ins& in = ins[d->second];
        const uint32_t* w = &code[in.at];
        for (uint32_t j = 3; j < in.words; ++j) if (def.count(w[j])) seed(w[j]);   // (literals that look like ids: kept too)
        if (in.op == OpLoad && in.words >= 4 && root.count(w[3]) && varStorage[root[w[3]]] == StorageFunction)
            for (uint32_t v : storedTo[root[w[3]]]) seed(v);
    }
    // Relaxed: colour math only. A value is relaxed when it is made (by an operation that can't overflow 16 bits from
    // inputs that fit them) from texture lookups, constants, values Meta's own shader already marks relaxed, and
    // other relaxed values, and is not kept above. Anything made from an interpolated input or a uniform (a
    // position, a view vector, a light's distance) stays 32-bit: relaxing every float op instead turned Haven's
    // lit walls black (2026-10-24), the view and light math overflowing or losing its precision in 16 bits.
    std::set<uint32_t> relaxed, constants;
    for (const Ins& in : ins)
        if (in.words >= 3 && (in.op == OpUndef || (in.op >= OpConstantTrue && in.op <= OpConstantNull))) constants.insert(code[in.at + 2]);
    auto source = [&](uint32_t id) {               // an operand a relaxed value may be made from
        return relaxed.count(id) || constants.count(id) || relaxedAlready.count(id);
    };
    auto colourOp = [&](const Ins& in, const uint32_t* w) {
        const uint32_t op = in.op;
        if (op == OpFNegate || op == OpFAdd || op == OpFSub || op == OpFMul || (op >= OpVectorTimesScalar && op <= OpDot) ||
            op == OpCopyObject || op == OpCompositeConstruct)
            return true;
        if (op == OpExtInst && in.words >= 5 && w[3] == glsl)
            switch (w[4]) {
                case GlslFAbs: case GlslFloor: case GlslCeil: case GlslFract: case GlslSqrt: case GlslPow: case GlslExp2:
                case GlslFMin: case GlslFMax: case GlslFClamp: case GlslFMix: case GlslStep: case GlslSmoothStep:
                case GlslLength: case GlslNormalize: return true;
                default: return false;
            }
        return op == OpVectorShuffle || op == OpCompositeExtract || op == OpCompositeInsert || op == OpSelect || op == OpPhi ||
               (op >= OpImageSampleImplicitLod && op <= OpImageGather) || op == OpLoad;
    };
    // The value operands of one instruction (not its literals, a select's condition or a phi's blocks).
    auto operands = [&](const Ins& in, const uint32_t* w, std::vector<uint32_t>& ids) {
        ids.clear();
        const uint32_t op = in.op, n = in.words;
        if (op == OpVectorShuffle || op == OpCompositeInsert) { if (n >= 5) ids = {w[3], w[4]}; }
        else if (op == OpCompositeExtract) { if (n >= 4) ids = {w[3]}; }
        else if (op == OpSelect) { if (n >= 6) ids = {w[4], w[5]}; }
        else if (op == OpPhi) { for (uint32_t j = 3; j + 1 < n; j += 2) ids.push_back(w[j]); }
        else if (op == OpExtInst) { for (uint32_t j = 5; j < n; ++j) ids.push_back(w[j]); }
        else for (uint32_t j = 3; j < n; ++j) ids.push_back(w[j]);
    };
    std::vector<uint32_t> ids;
    for (bool changed = true; changed;) {         // (to a fixed point: phis and function variables loop back)
        changed = false;
        for (const Ins& in : ins) {
            const uint32_t* w = &code[in.at];
            if (in.words < 3 || !float32.count(w[1]) || keep.count(w[2]) || relaxed.count(w[2]) || !colourOp(in, w)) continue;
            bool ok = true;
            if (in.op >= OpImageSampleImplicitLod && in.op <= OpImageGather) ok = true;   // (a texel: what colour math starts from)
            else if (in.op == OpLoad) {                // a function variable's value: relaxed when all stored to it is
                ok = in.words >= 4 && root.count(w[3]) && varStorage[root[w[3]]] == StorageFunction;
                if (ok) for (uint32_t v : storedTo[root[w[3]]]) ok = ok && source(v);
            } else {
                operands(in, w, ids);
                for (uint32_t id : ids) ok = ok && source(id);
            }
            if (ok) { relaxed.insert(w[2]); changed = true; }
        }
    }
    std::vector<uint32_t> decorations;
    for (uint32_t id : relaxed)
        if (!relaxedAlready.count(id) && def.count(id) && ins[def[id]].op != OpLoad)
            decorations.insert(decorations.end(), {(3u << 16) | OpDecorate, id, DecRelaxedPrecision});
    if (decorations.empty()) return code;
    size_t at = 5;   // after the module's own annotations
    while (at < code.size() && (code[at] >> 16) && (preamble(code[at] & 0xFFFF) || annotation(code[at] & 0xFFFF))) at += code[at] >> 16;
    std::vector<uint32_t> out(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(at));
    out.insert(out.end(), decorations.begin(), decorations.end());
    out.insert(out.end(), code.begin() + static_cast<std::ptrdiff_t>(at), code.end());
    return out;
}

std::mutex g_mutex;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_cache;
Stats g_stats;

}  // namespace

int markOf(const std::vector<uint32_t>& code) {
    if (code.size() < 5 || code[0] != 0x07230203u) return -1;
    const size_t len = std::strlen(kMark);
    for (size_t i = 5; i < code.size();) {
        const uint32_t words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size() || !preamble(op)) break;
        if ((op == OpSourceExtension || op == OpModuleProcessed) && (words - 1) * 4 > len) {
            const char* s = reinterpret_cast<const char*>(&code[i + 1]);
            if (!std::strncmp(s, kMark, len) && std::memchr(s, 0, (words - 1) * 4)) return std::atoi(s + len);
        }
        i += words;
    }
    return -1;
}

int flags() {
    static const int f = [] {
        auto on = [](const char* name) { const char* v = std::getenv(name); return v && *v && *v != '0'; };
        int v = 0;
#ifdef FH_HAVE_SPV_OPT
        if (on("FH_SPV_OPT")) v |= kOptimize;
#endif
        if (const char* h = std::getenv("FH_SPV_HALF")) v |= std::atoi(h) >= 2 ? (kRelax | kHalf) : std::atoi(h) == 1 ? kRelax : 0;
#ifndef FH_HAVE_SPV_OPT
        v &= ~kHalf;                             // (the 16-bit rewrite is SPIRV-Tools'; relaxing alone is ours)
        (void)on;
#endif
        return v;
    }();
    return f;
}

int flagsFor(const std::string& programPath) {
    static const char* only = std::getenv("FH_SPV_HALF_ONLY");
    const int f = flags();
    return only && programPath.find(only) == std::string::npos ? f & ~(kRelax | kHalf) : f;
}

Stats stats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stats;
}

std::vector<uint32_t> run(const std::vector<uint32_t>& code, int flags, std::string* err) {
    if (!flags || code.size() < 5 || code[0] != 0x07230203u || markOf(code) == flags) return code;
    const uint64_t key = hashOf(code, flags);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (const auto it = g_cache.find(key); it != g_cache.end()) {
            ++g_stats.cached;
            return it->second.empty() ? code : it->second;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    std::vector<uint32_t> out = code;
    bool ok = true;
#ifdef FH_HAVE_SPV_OPT
    // The shaders' own SPIR-V version picks the Vulkan the validator holds them to (1.3 is Vulkan 1.1's, 1.5 1.2's).
    const spv_target_env env = code[1] > 0x10500u ? SPV_ENV_VULKAN_1_3 : code[1] > 0x10300u ? SPV_ENV_VULKAN_1_2 : SPV_ENV_VULKAN_1_1;
    auto consumer = [&why](spv_message_level_t level, const char*, const spv_position_t& pos, const char* msg) {
        if (level <= SPV_MSG_ERROR && why.empty()) why = std::string(msg) + " (word " + std::to_string(pos.index) + ")";
    };
#endif
    // Relaxed first, on Meta's module as it ships: relax() reads its function variables and its own RelaxedPrecision
    // marks, which the optimizer folds away. Relaxing after the optimizer turned Haven's lit walls black (2026-10-25).
    bool relaxed = false;
    if (flags & kRelax) {
        std::vector<uint32_t> r = relax(out);
        ok = !r.empty();
        relaxed = ok && r.size() != out.size();
        if (!ok) why = "relax: unreadable module";
        else out.swap(r);
    }
#ifdef FH_HAVE_SPV_OPT
    spvtools::OptimizerOptions options;
    options.set_run_validator(true);             // (a module it can't read is left as it is, not optimized wrong)
    if (ok && (flags & kOptimize)) {
        spvtools::Optimizer opt(env);
        opt.SetMessageConsumer(consumer);
        // The interface (inputs, outputs, bindings) as it was: the renderer lays out vertex streams and descriptor
        // sets from the shipped shaders.
        opt.RegisterPerformancePasses(true);
        std::vector<uint32_t> o;
        ok = opt.Run(out.data(), out.size(), &o, options);
        if (ok) out.swap(o);
    }
    if (ok && relaxed && (flags & kHalf)) {
        spvtools::Optimizer opt(env);
        opt.SetMessageConsumer(consumer);
        opt.RegisterPass(spvtools::CreateConvertRelaxedToHalfPass())
           .RegisterPass(spvtools::CreateSimplificationPass())
           .RegisterPass(spvtools::CreateRedundancyEliminationPass())
           .RegisterPass(spvtools::CreateAggressiveDCEPass());
        std::vector<uint32_t> o;
        ok = opt.Run(out.data(), out.size(), &o, options);
        if (ok) out.swap(o);
    }
#else
    (void)relaxed;
#endif
    if (ok) out = withMark(out, flags);
    if (const char* dir = std::getenv("FH_SPV_OPT_DUMP")) {   // diagnostics: every module before and after, to spirv-val
        char base[64];
        std::snprintf(base, sizeof base, "/%016llx", static_cast<unsigned long long>(key));
        for (const auto& [ext, c] : {std::pair<const char*, const std::vector<uint32_t>*>{".in.spv", &code}, {ok ? ".out.spv" : ".failed.spv", &out}})
            if (FILE* f = std::fopen((std::string(dir) + base + ext).c_str(), "wb")) { std::fwrite(c->data(), 4, c->size(), f); std::fclose(f); }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!ok && err) *err = why.empty() ? std::string("optimizer failed") : why;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.ms += ms;
    if (ok) ++g_stats.made; else ++g_stats.failed;
    if (!ok) { g_cache[key].clear(); return code; }
    return g_cache[key] = out;
}

}  // namespace spvopt
