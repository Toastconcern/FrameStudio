#pragma once
// A fragment shader with the uniform values that never change for one material baked in as constants: an OpLoad of a
// uniform block member reached through constant indices (matParams flags and layers, lightUniforms.iblReflectionTint,
// globals.whitePoint ...) whose value is integer, or floats of exactly 0, 1 or -1, becomes an OpCopyObject of an
// OpConstant / OpConstantComposite holding the exact bytes the buffer holds. The bindings and layouts stay as they were (the buffers are still bound, just no longer read there),
// and the driver's compiler folds the constants and drops what they make dead (a mix() by 0, a multiply by 1, a
// branch on a flag). The same bits reach the same instructions: the picture doesn't change.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

namespace spvspec {

// What a block's bytes are: the data and which of them may be folded (others are left as loads).
struct BlockBytes {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    std::function<bool(uint32_t offset, uint32_t size)> foldable;   // empty: all of it
};
using Fetch = std::function<bool(uint32_t set, uint32_t binding, BlockBytes& out)>;

namespace detail {
enum : uint32_t {
    OpDecorate = 71, OpMemberDecorate = 72, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23,
    OpTypeMatrix = 24, OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpCopyObject = 83,
    DecBlock = 2, DecRowMajor = 4, DecArrayStride = 6, DecMatrixStride = 7, DecBinding = 33, DecDescriptorSet = 34, DecOffset = 35,
    StorageUniform = 2,
};
struct Type { uint32_t op = 0; std::vector<uint32_t> a; };
struct Member { uint32_t offset = 0xFFFFFFFFu, matrixStride = 0; bool rowMajor = false; };
struct Ptr { uint32_t set = 0, binding = 0, offset = 0, type = 0, matrixStride = 0; bool rowMajor = false; };

struct Module {
    std::map<uint32_t, Type> types;
    std::map<uint32_t, std::map<uint32_t, Member>> members;   // struct id -> member -> layout
    std::map<uint32_t, uint32_t> arrayStride, set, binding, constU32;
    std::map<uint32_t, bool> block;
    std::map<uint32_t, Ptr> ptrs;                             // uniform member pointers reached by constant indices
    std::map<uint32_t, uint32_t> ptrPointee;                  // uniform pointer type -> pointee
};

inline bool scan(const std::vector<uint32_t>& code, Module& m) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    for (size_t i = 5; i < code.size();) {
        const uint32_t words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return false;
        const uint32_t* w = &code[i];
        if (op == OpDecorate && words >= 3) {
            if (w[2] == DecDescriptorSet && words >= 4) m.set[w[1]] = w[3];
            else if (w[2] == DecBinding && words >= 4) m.binding[w[1]] = w[3];
            else if (w[2] == DecArrayStride && words >= 4) m.arrayStride[w[1]] = w[3];
            else if (w[2] == DecBlock) m.block[w[1]] = true;
        } else if (op == OpMemberDecorate && words >= 4) {
            Member& mm = m.members[w[1]][w[2]];
            if (w[3] == DecOffset && words >= 5) mm.offset = w[4];
            else if (w[3] == DecMatrixStride && words >= 5) mm.matrixStride = w[4];
            else if (w[3] == DecRowMajor) mm.rowMajor = true;
        } else if (op >= OpTypeBool && op <= OpTypePointer && words >= 2) {
            m.types[w[1]] = {op, std::vector<uint32_t>(w + 2, w + words)};
            if (op == OpTypePointer && words >= 4 && w[2] == StorageUniform) m.ptrPointee[w[1]] = w[3];
        } else if (op == OpConstant && words == 4) {
            const auto t = m.types.find(w[1]);
            if (t != m.types.end() && t->second.op == OpTypeInt && t->second.a.size() >= 1 && t->second.a[0] == 32) m.constU32[w[2]] = w[3];
        } else if (op == OpVariable && words >= 4 && w[3] == StorageUniform) {
            const auto pt = m.ptrPointee.find(w[1]);
            if (pt != m.ptrPointee.end() && m.block.count(pt->second) && m.set.count(w[2]) && m.binding.count(w[2]))
                m.ptrs[w[2]] = {m.set[w[2]], m.binding[w[2]], 0, pt->second, 0, false};
        } else if (op == OpFunction) {
            break;
        }
        i += words;
    }
    return true;
}

inline uint32_t scalarBytes(const Module& m, uint32_t t) {
    const auto it = m.types.find(t);
    if (it == m.types.end()) return 0;
    if ((it->second.op == OpTypeFloat || it->second.op == OpTypeInt) && !it->second.a.empty() && it->second.a[0] == 32) return 4;
    return 0;
}

// An offset moved by `by` bytes, false past kMaxOffset: the strides and indices are the shader's word (an APK's or a
// .fenv's), and a wrapped 32-bit offset would pass the block-size checks and read far outside the block.
constexpr uint64_t kMaxOffset = uint64_t(1) << 30;
inline bool advance(uint32_t& offset, uint64_t by) {
    if (offset + by > kMaxOffset) return false;
    offset = static_cast<uint32_t>(offset + by);
    return true;
}

// One step down an access chain: (type, offset) of member / element `index`.
inline bool step(const Module& m, Ptr& p, uint32_t index) {
    const auto it = m.types.find(p.type);
    if (it == m.types.end()) return false;
    const Type& t = it->second;
    if (t.a.empty()) return false;                // (a type with no operands: a damaged module)
    if (t.op == OpTypeStruct) {
        if (index >= t.a.size()) return false;
        const auto ms = m.members.find(p.type);
        if (ms == m.members.end()) return false;
        const auto mm = ms->second.find(index);
        if (mm == ms->second.end() || mm->second.offset == 0xFFFFFFFFu) return false;
        if (!advance(p.offset, mm->second.offset)) return false;
        p.matrixStride = mm->second.matrixStride;
        p.rowMajor = mm->second.rowMajor;
        p.type = t.a[index];
        return true;
    }
    if (t.op == OpTypeArray) {
        const auto st = m.arrayStride.find(p.type);
        if (st == m.arrayStride.end()) return false;
        if (!advance(p.offset, uint64_t(index) * st->second)) return false;
        p.type = t.a[0];
        return true;
    }
    if (t.op == OpTypeMatrix) {
        if (!p.matrixStride || p.rowMajor) return false;
        if (!advance(p.offset, uint64_t(index) * p.matrixStride)) return false;
        p.type = t.a[0];
        return true;
    }
    if (t.op == OpTypeVector) {
        const uint32_t b = scalarBytes(m, t.a[0]);
        if (!b) return false;
        if (!advance(p.offset, uint64_t(index) * b)) return false;
        p.type = t.a[0];
        return true;
    }
    return false;
}

// The bytes a value of type `type` at `p` spans (0 when it can't be built as a constant).
// (depth: a damaged module's type may contain itself, which would recurse until the stack runs out; budget: arrays of
// arrays of ... each 1024 long would visit 1024^depth members. build() walks only what an extent here walked.)
constexpr int kMaxDepth = 32;
inline uint32_t extent(const Module& m, const Ptr& p, int depth = 0, uint32_t* budget = nullptr) {
    uint32_t mine = 1u << 16;
    if (!budget) budget = &mine;
    if (depth > kMaxDepth || !*budget) return 0;
    --*budget;
    const auto it = m.types.find(p.type);
    if (it == m.types.end()) return 0;
    const Type& t = it->second;
    if (t.op == OpTypeFloat || t.op == OpTypeInt) return scalarBytes(m, p.type);
    if (t.op == OpTypeVector) return t.a.size() >= 2 && t.a[1] <= 4 ? scalarBytes(m, t.a[0]) * t.a[1] : 0;
    uint32_t end = 0, n = 0;
    if (t.op == OpTypeMatrix) n = t.a.size() >= 2 ? t.a[1] : 0;
    else if (t.op == OpTypeArray) { const auto c = m.constU32.find(t.a.size() >= 2 ? t.a[1] : 0); n = c == m.constU32.end() ? 0 : c->second; }
    else if (t.op == OpTypeStruct) n = static_cast<uint32_t>(t.a.size());
    else return 0;
    if (n > 1024) return 0;                   // (as build: no constant that large is made, and a huge count hangs)
    for (uint32_t k = 0; k < n; ++k) {
        Ptr q = p;
        if (!step(m, q, k)) return 0;
        const uint32_t e = extent(m, q, depth + 1, budget);
        if (!e) return 0;
        end = std::max(end, q.offset - p.offset + e);
    }
    return end <= kMaxOffset ? end : 0;
}
}  // namespace detail

// The blocks (set, binding) this shader loads constant-indexed members of: what a specialization depends on.
inline std::vector<std::pair<uint32_t, uint32_t>> blocksRead(const std::vector<uint32_t>& code) {
    detail::Module m;
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (!detail::scan(code, m)) return out;
    for (const auto& p : m.ptrs) {
        const std::pair<uint32_t, uint32_t> sb = {p.second.set, p.second.binding};
        if (std::find(out.begin(), out.end(), sb) == out.end()) out.push_back(sb);
    }
    return out;
}

// `code` with the foldable loads replaced (integers, and floats that are 0, 1 or -1); `folded` counts them. The input
// itself when nothing could be folded.
inline std::vector<uint32_t> specialize(const std::vector<uint32_t>& code, const Fetch& fetch, uint32_t* folded = nullptr, int limit = -1,
                                        int floats = 2) {
    using namespace detail;
    uint32_t count = 0;
    if (!folded) folded = &count;
    *folded = 0;
    Module m;
    if (!scan(code, m)) return code;
    std::map<std::pair<uint32_t, uint32_t>, BlockBytes> blocks;
    std::map<std::pair<uint32_t, uint32_t>, bool> have;
    uint32_t bound = code[3];
    std::vector<uint32_t> consts;                              // new constants, before the first function
    std::map<std::pair<uint32_t, std::vector<uint32_t>>, uint32_t> made;   // (type, words) -> id
    std::function<uint32_t(const Ptr&, const BlockBytes&, int)> build = [&](const Ptr& p, const BlockBytes& bb, int depth) -> uint32_t {
        if (depth > kMaxDepth) return 0;
        const auto it = m.types.find(p.type);
        if (it == m.types.end()) return 0;
        const Type& t = it->second;
        std::vector<uint32_t> parts;
        uint32_t op = OpConstantComposite;
        if (t.op == OpTypeFloat || t.op == OpTypeInt) {
            if (scalarBytes(m, p.type) != 4 || p.offset + 4 > bb.size) return 0;
            uint32_t v;
            std::memcpy(&v, bb.data + p.offset, 4);
            // Floats only when exactly 0, 1 or -1: what they make dead (a multiply by 1, a layer mixed in by 0) goes
            // exactly, while folding any other value let the compiler round an expression its own way (a last bit
            // apart on 0.02% of Desert Terrace's pixels for its palms' colorIntensity), and saves next to nothing.
            if (t.op == OpTypeFloat && floats < 1) return 0;
            if (t.op == OpTypeFloat && (v & 0x7FFFFFFFu) != 0 && (floats < 2 || (v & 0x7FFFFFFFu) != 0x3F800000u)) return 0;
            parts.push_back(v);
            op = OpConstant;
        } else {
            uint32_t n = 0;
            if (t.op == OpTypeVector || t.op == OpTypeMatrix) n = t.a.size() >= 2 ? t.a[1] : 0;
            else if (t.op == OpTypeArray) { const auto c = m.constU32.find(t.a.size() >= 2 ? t.a[1] : 0); n = c == m.constU32.end() ? 0 : c->second; }
            else if (t.op == OpTypeStruct) n = static_cast<uint32_t>(t.a.size());
            if (!n || n > 1024) return 0;
            for (uint32_t k = 0; k < n; ++k) {
                Ptr q = p;
                if (!step(m, q, k)) return 0;
                const uint32_t id = build(q, bb, depth + 1);
                if (!id) return 0;
                parts.push_back(id);
            }
        }
        const auto key = std::make_pair(p.type * 2 + (op == OpConstant ? 1u : 0u), parts);
        if (const auto f = made.find(key); f != made.end()) return f->second;
        const uint32_t id = bound++;
        consts.push_back(((3u + static_cast<uint32_t>(parts.size())) << 16) | op);
        consts.push_back(p.type);
        consts.push_back(id);
        consts.insert(consts.end(), parts.begin(), parts.end());
        made[key] = id;
        return id;
    };
    std::vector<uint32_t> body;                                // everything from the first OpFunction on, rewritten
    size_t firstFunction = code.size();
    for (size_t i = 5; i < code.size();) {
        const uint32_t words = code[i] >> 16, op = code[i] & 0xFFFF;
        if (!words || i + words > code.size()) return code;
        if (op == OpFunction && firstFunction == code.size()) firstFunction = i;
        const uint32_t* w = &code[i];
        if (i >= firstFunction) {
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && words >= 4) {
                const auto base = m.ptrs.find(w[3]);
                if (base != m.ptrs.end()) {
                    Ptr p = base->second;
                    bool ok = true;
                    for (uint32_t k = 4; k < words && ok; ++k) {
                        const auto c = m.constU32.find(w[k]);
                        ok = c != m.constU32.end() && step(m, p, c->second);
                    }
                    if (ok) m.ptrs[w[2]] = p;
                }
            } else if (op == OpLoad && words >= 4) {
                const auto pp = m.ptrs.find(w[3]);
                if (pp != m.ptrs.end() && pp->second.type == w[1] && (limit < 0 || static_cast<int>(*folded) < limit)) {
                    const Ptr& p = pp->second;
                    const auto sb = std::make_pair(p.set, p.binding);
                    if (!have.count(sb)) { BlockBytes bb; have[sb] = fetch(p.set, p.binding, bb); blocks[sb] = bb; }
                    const BlockBytes& bb = blocks[sb];
                    const uint32_t ext = have[sb] ? extent(m, p) : 0;
                    if (ext && p.offset + ext <= bb.size && (!bb.foldable || bb.foldable(p.offset, ext))) {
                        if (const uint32_t id = build(p, bb, 0)) {
                            body.insert(body.end(), {(4u << 16) | OpCopyObject, w[1], w[2], id});
                            ++*folded;
                            i += words;
                            continue;
                        }
                    }
                }
            }
            body.insert(body.end(), w, w + words);
        }
        i += words;
    }
    if (consts.empty()) return code;
    std::vector<uint32_t> out(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(firstFunction));
    out.insert(out.end(), consts.begin(), consts.end());
    out.insert(out.end(), body.begin(), body.end());
    out[3] = bound;
    return out;
}

}  // namespace spvspec
