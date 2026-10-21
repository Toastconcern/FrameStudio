#pragma once
// Minimal SPIR-V reflection for Meta HSL shaders: vertex inputs, descriptor resources (by set/binding,
// with names and uniform-block member offsets), push-constant block, image dimensionality and the
// MultiView capability. Names come from OpName/OpMemberName — present in v206 shaders and restored for
// stripped v210 ones by spirv_rehydrate.h before reflection.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace spv {

enum class ResKind { UniformBuffer, StorageBuffer, Sampler, SampledImage, CombinedImageSampler, StorageImage };
enum class ImageDim { D2, D3, Cube };

struct Member { std::string name; uint32_t offset = 0, size = 0; bool integer = false; };   // integer: int/uint/bool scalar, vector or array

struct Resource {
    ResKind kind = ResKind::UniformBuffer;
    uint32_t set = 0, binding = 0, count = 1;
    std::string name;                   // variable name, else block type name
    uint32_t size = 0;                  // buffers: byte size of the block (0 for runtime arrays)
    std::vector<Member> members;        // buffers: top-level members
    ImageDim dim = ImageDim::D2;        // images
    bool depthImage = false;
};

struct Input { uint32_t location = 0; std::string name; uint32_t components = 1; bool integer = false; uint32_t width = 32; };

struct Module {
    uint32_t stage = 0xFFFFFFFFu;       // SPIR-V execution model: 0 vertex, 4 fragment
    bool multiview = false;
    std::vector<Input> inputs;          // vertex stage only
    std::vector<Resource> resources;
    uint32_t pushSize = 0;
    std::vector<Member> pushMembers;    // flattened one level: "pushConstants.worldFromModel"
};

inline std::string readString(const uint32_t* w, size_t n) {
    std::string s(reinterpret_cast<const char*>(w), n * 4);
    return s.substr(0, s.find('\0'));
}

inline bool reflect(const std::vector<uint32_t>& code, Module& out) {
    if (code.size() < 5 || code[0] != 0x07230203u) return false;
    struct Type { uint32_t op = 0; std::vector<uint32_t> a; };
    std::map<uint32_t, Type> types;
    std::map<uint32_t, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> deco;                       // id -> decoration -> value
    std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, uint32_t>> mdeco; // (struct, member) -> deco -> value
    std::map<uint32_t, uint32_t> constants;                                       // id -> 32-bit value
    struct Var { uint32_t id, ptrType, storage; };
    std::vector<Var> vars;

    for (size_t i = 5; i < code.size();) {
        uint32_t op = code[i] & 0xFFFF, wc = code[i] >> 16;
        if (wc == 0 || i + wc > code.size()) return false;
        const uint32_t* a = &code[i + 1];
        uint32_t n = wc - 1;
        switch (op) {
            case 17: if (n >= 1 && a[0] == 4439) out.multiview = true; break;            // OpCapability MultiView
            case 15: if (out.stage == 0xFFFFFFFFu && n >= 1) out.stage = a[0]; break;   // OpEntryPoint
            case 5:  if (n >= 2) names[a[0]] = readString(a + 1, n - 1); break;          // OpName
            case 6:  if (n >= 3) memberNames[{a[0], a[1]}] = readString(a + 2, n - 2); break;
            case 71: if (n >= 2) deco[a[0]][a[1]] = n >= 3 ? a[2] : 1; break;            // OpDecorate
            case 72: if (n >= 3) mdeco[{a[0], a[1]}][a[2]] = n >= 4 ? a[3] : 1; break;  // OpMemberDecorate
            case 43: if (n >= 3) constants[a[1]] = a[2]; break;                          // OpConstant
            case 59: if (n >= 3) vars.push_back({a[1], a[0], a[2]}); break;              // OpVariable
            default:
                if ((op >= 19 && op <= 32) || op == 27) {                                // OpType*
                    if (n >= 1) { Type t; t.op = op; t.a.assign(a + 1, a + n); types[a[0]] = t; }
                }
                break;
        }
        i += wc;
    }

    // Malformed modules can make types self-referential (recursion forever) or a DAG of wide structs (exponential
    // walk): cap both the nesting depth and the total work. Real HSL types nest a handful of levels.
    constexpr int kMaxTypeDepth = 32;
    size_t typeBudget = 1u << 20;
    // Byte size of a type as laid out in a buffer block (uses Offset/ArrayStride/MatrixStride decorations).
    std::function<uint32_t(uint32_t, uint32_t, int)> sizeOf;
    sizeOf = [&](uint32_t id, uint32_t matrixStride, int depth) -> uint32_t {
        if (depth > kMaxTypeDepth || !typeBudget) return 0;
        --typeBudget;
        auto it = types.find(id);
        if (it == types.end()) return 0;
        const Type& t = it->second;
        switch (t.op) {
            case 21: case 22: return t.a.empty() ? 4 : t.a[0] / 8;                       // int / float
            case 20: return 4;                                                           // bool
            case 23: return t.a.size() >= 2 ? sizeOf(t.a[0], 0, depth + 1) * t.a[1] : 0; // vector
            case 24: {                                                                   // matrix (columns)
                if (t.a.empty()) return 0;
                uint32_t cols = t.a.size() >= 2 ? t.a[1] : 4;
                return matrixStride ? matrixStride * cols : sizeOf(t.a[0], 0, depth + 1) * cols;
            }
            case 28: {                                                                   // array
                if (t.a.empty()) return 0;
                uint32_t len = t.a.size() >= 2 && constants.count(t.a[1]) ? constants[t.a[1]] : 1;
                auto d = deco.find(id);
                uint32_t stride = d != deco.end() && d->second.count(6) ? d->second.at(6) : sizeOf(t.a[0], matrixStride, depth + 1);
                return stride * len;
            }
            case 29: return 0;                                                           // runtime array
            case 30: {                                                                   // struct
                uint32_t end = 0;
                for (uint32_t m = 0; m < t.a.size(); ++m) {
                    auto md = mdeco.find({id, m});
                    uint32_t off = md != mdeco.end() && md->second.count(35) ? md->second.at(35) : 0;
                    uint32_t ms = md != mdeco.end() && md->second.count(7) ? md->second.at(7) : 0;
                    end = std::max(end, off + sizeOf(t.a[m], ms, depth + 1));
                }
                return end;
            }
            default: return 0;
        }
    };
    // int / uint / bool at the bottom of a vector / matrix / array type
    std::function<bool(uint32_t, int)> isInteger = [&](uint32_t id, int depth) -> bool {
        if (depth > kMaxTypeDepth) return false;
        auto it = types.find(id);
        if (it == types.end()) return false;
        const Type& t = it->second;
        if (t.op == 20 || t.op == 21) return true;
        if ((t.op == 23 || t.op == 24 || t.op == 28 || t.op == 29) && !t.a.empty()) return isInteger(t.a[0], depth + 1);
        return false;
    };
    auto membersOf = [&](uint32_t structId) {
        std::vector<Member> ms;
        auto it = types.find(structId);
        if (it == types.end() || it->second.op != 30) return ms;
        for (uint32_t m = 0; m < it->second.a.size(); ++m) {
            auto md = mdeco.find({structId, m});
            Member mm;
            mm.offset = md != mdeco.end() && md->second.count(35) ? md->second.at(35) : 0;
            uint32_t mstride = md != mdeco.end() && md->second.count(7) ? md->second.at(7) : 0;
            mm.size = sizeOf(it->second.a[m], mstride, 0);
            mm.integer = isInteger(it->second.a[m], 0);
            auto nm = memberNames.find({structId, m});
            mm.name = nm != memberNames.end() ? nm->second : std::string();
            ms.push_back(mm);
        }
        return ms;
    };
    auto pointee = [&](uint32_t ptrType) -> uint32_t {
        auto it = types.find(ptrType);
        return it != types.end() && it->second.op == 32 && it->second.a.size() >= 2 ? it->second.a[1] : 0;
    };

    for (const Var& v : vars) {
        uint32_t ty = pointee(v.ptrType);
        auto dv = deco.find(v.id);
        const bool builtin = dv != deco.end() && dv->second.count(11);
        if (v.storage == 1 /*Input*/ && out.stage == 0 && !builtin) {
            if (dv == deco.end() || !dv->second.count(30)) continue;
            Input in;
            in.location = dv->second.at(30);
            in.name = names.count(v.id) ? names[v.id] : std::string();
            auto t = types.find(ty);
            uint32_t scalar = ty;
            if (t != types.end() && t->second.op == 23 && t->second.a.size() >= 2) { in.components = t->second.a[1]; scalar = t->second.a[0]; }
            auto st = types.find(scalar);
            if (st != types.end()) { in.integer = st->second.op == 21; in.width = st->second.a.empty() ? 32 : st->second.a[0]; }
            out.inputs.push_back(in);
            continue;
        }
        if (v.storage == 9 /*PushConstant*/) {
            out.pushSize = std::max(out.pushSize, sizeOf(ty, 0, 0));
            for (const Member& m : membersOf(ty)) {
                // One level of nesting: pushConstants { worldFromModel, color }.
                auto mt = types.find(ty);
                uint32_t idx = 0;
                for (; idx < mt->second.a.size(); ++idx) {
                    auto md = mdeco.find({ty, idx});
                    if ((md != mdeco.end() && md->second.count(35) ? md->second.at(35) : 0) == m.offset) break;
                }
                uint32_t inner = idx < mt->second.a.size() ? mt->second.a[idx] : 0;
                auto it = types.find(inner);
                if (it != types.end() && it->second.op == 30) {
                    for (Member sm : membersOf(inner)) { sm.offset += m.offset; sm.name = m.name + "." + sm.name; out.pushMembers.push_back(sm); }
                } else {
                    out.pushMembers.push_back(m);
                }
            }
            continue;
        }
        if (v.storage != 0 && v.storage != 2 && v.storage != 12) continue;
        if (dv == deco.end() || !dv->second.count(34) || !dv->second.count(33)) continue;
        Resource r;
        r.set = dv->second.at(34);
        r.binding = dv->second.at(33);
        r.name = names.count(v.id) ? names[v.id] : std::string();
        uint32_t base = ty;
        auto bt = types.find(base);
        if (bt != types.end() && (bt->second.op == 28 || bt->second.op == 29) && !bt->second.a.empty()) {
            if (bt->second.op == 28 && bt->second.a.size() >= 2 && constants.count(bt->second.a[1])) r.count = constants[bt->second.a[1]];
            base = bt->second.a[0];
            bt = types.find(base);
        }
        if (bt == types.end()) continue;
        switch (bt->second.op) {
            case 30: {
                auto db = deco.find(base);
                bool bufferBlock = db != deco.end() && db->second.count(3);
                r.kind = (v.storage == 12 || bufferBlock) ? ResKind::StorageBuffer : ResKind::UniformBuffer;
                r.size = sizeOf(base, 0, 0);
                r.members = membersOf(base);
                if (r.name.empty() && names.count(base)) r.name = names[base];
                break;
            }
            case 26: r.kind = ResKind::Sampler; break;
            case 25: case 27: {
                if (bt->second.op == 27 && bt->second.a.empty()) continue;
                uint32_t img = bt->second.op == 27 ? bt->second.a[0] : base;
                auto it = types.find(img);
                if (it == types.end() || it->second.a.size() < 6) continue;
                uint32_t dim = it->second.a[1];
                r.dim = dim == 3 ? ImageDim::Cube : dim == 2 ? ImageDim::D3 : ImageDim::D2;
                r.depthImage = it->second.a[2] == 1;
                r.kind = bt->second.op == 27 ? ResKind::CombinedImageSampler
                       : it->second.a[5] == 2 ? ResKind::StorageImage : ResKind::SampledImage;
                break;
            }
            default: continue;
        }
        out.resources.push_back(r);
    }
    return true;
}

}  // namespace spv
