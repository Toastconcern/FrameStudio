#pragma once
// A structural check of SPIR-V before it reaches the driver (vkCreateShaderModule): a .fenv carries the homes'
// shaders as it was given them, and a damaged or hostile one is undefined behaviour in the driver's compiler (it may
// crash the process or hang the GPU) rather than a refusal. Not a full validator (spirv-val is SPIRV-Tools, megabytes,
// and it refuses some shaders drivers take): what's checked is what keeps a parser in bounds and its id tables sane:
//   - the header: magic (this byte order), SPIR-V 1.0 .. 1.6, an id bound under 4M, schema 0
//   - every instruction's word count: at least 1, and within the module
//   - every result id (the opcodes that have one, from the SPIR-V grammar): non-zero, under the bound, defined once,
//     and every result type an id the module defines
//   - the literal strings of the debug and mode-setting instructions nul-terminated inside their instruction, and
//     the ids those instructions name under the bound
//   - an OpCapability first, one OpMemoryModel, at least one OpEntryPoint, functions opened and closed in order (none
//     inside another), blocks and parameters only inside functions
// Linear in the module: well under a millisecond for a home's biggest shader (tests_loco/spv_check_test.cpp).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spvcheck {

namespace detail {
// (opcode << 2) | (1: has a result id) | (2: has a result type), sorted, for every opcode the SPIR-V headers know
// (third_party/spirv-headers; regenerate with tools/gen_spv_ops.py). An opcode not in it is passed over unchecked.
constexpr uint16_t kOps[] = {
    0, 7, 8, 12, 16, 20, 24, 29, 32, 40, 45, 51, 56, 60, 64, 68, 77, 81, 85, 89, 93, 97, 101, 105, 109, 113, 117, 121,
    125, 129, 133, 137, 141, 145, 149, 153, 156, 167, 171, 175, 179, 183, 187, 195, 199, 203, 207, 211, 219, 223, 224,
    231, 239, 243, 247, 248, 252, 256, 263, 267, 271, 275, 279, 283, 284, 288, 293, 296, 300, 311, 315, 319, 323, 327,
    331, 335, 339, 347, 351, 355, 359, 363, 367, 371, 375, 379, 383, 387, 391, 395, 396, 403, 407, 411, 415, 419, 423,
    427, 431, 439, 443, 447, 451, 455, 459, 463, 467, 471, 475, 479, 483, 487, 491, 495, 499, 507, 511, 515, 519, 523,
    527, 531, 535, 539, 543, 547, 551, 555, 559, 563, 567, 571, 575, 579, 583, 587, 591, 595, 599, 603, 607, 611, 619,
    623, 627, 631, 635, 639, 643, 647, 651, 655, 659, 663, 667, 671, 675, 679, 683, 687, 691, 695, 699, 703, 707, 711,
    715, 719, 723, 727, 731, 735, 739, 743, 747, 751, 755, 759, 763, 767, 779, 783, 787, 791, 795, 799, 803, 807, 811,
    815, 819, 823, 831, 835, 839, 843, 847, 851, 855, 859, 863, 872, 876, 880, 884, 896, 900, 911, 912, 919, 923, 927,
    931, 935, 939, 943, 947, 951, 955, 959, 963, 967, 971, 983, 984, 988, 993, 996, 1000, 1004, 1008, 1012, 1016,
    1020, 1024, 1028, 1039, 1040, 1047, 1051, 1055, 1059, 1063, 1067, 1071, 1075, 1079, 1083, 1087, 1099, 1103, 1107,
    1111, 1115, 1119, 1120, 1124, 1131, 1135, 1139, 1143, 1147, 1148, 1152, 1167, 1171, 1175, 1179, 1183, 1187, 1188,
    1192, 1199, 1203, 1204, 1208, 1215, 1219, 1223, 1227, 1231, 1235, 1239, 1243, 1247, 1251, 1255, 1259, 1263, 1267,
    1268, 1275, 1276, 1283, 1287, 1289, 1295, 1299, 1303, 1307, 1309, 1315, 1316, 1320, 1324, 1328, 1335, 1339, 1343,
    1347, 1351, 1355, 1359, 1363, 1367, 1371, 1375, 1379, 1383, 1387, 1391, 1395, 1399, 1403, 1407, 1411, 1415, 1419,
    1423, 1427, 1431, 1435, 1439, 1443, 1447, 1451, 1455, 1459, 1463, 1467, 1603, 1607, 1611, 1615, 16643, 16647,
    16651, 16653, 16659, 16660, 16667, 16727, 16728, 16735, 16739, 16740, 16744, 16761, 16783, 17664, 17669, 17675,
    17679, 17683, 17687, 17691, 17695, 17699, 17703, 17704, 17711, 17715, 17719, 17723, 17727, 17731, 17735, 17739,
    17780, 17784, 17791, 17792, 17796, 17803, 17807, 17811, 17815, 17819, 17823, 17825, 17831, 17832, 17839, 17843,
    17847, 17851, 17855, 17889, 17892, 17896, 17900, 17904, 17911, 17919, 17923, 17927, 17931, 17935, 17991, 18003,
    18007, 18011, 18015, 18023, 18163, 18167, 18171, 18183, 20003, 20007, 20011, 20015, 20019, 20023, 20027, 20031,
    20047, 20051, 20227, 20299, 20300, 20305, 20315, 20363, 20407, 20413, 20417, 20443, 20447, 20461, 20479, 20484,
    20507, 20508, 20519, 20591, 20595, 20635, 20639, 20996, 21000, 21004, 21011, 21015, 21019, 21023, 21024, 21031,
    21035, 21036, 21040, 21044, 21048, 21052, 21056, 21063, 21064, 21071, 21075, 21079, 21083, 21087, 21091, 21095,
    21099, 21103, 21107, 21111, 21115, 21116, 21120, 21125, 21135, 21153, 21159, 21160, 21164, 21171, 21175, 21176,
    21180, 21187, 21196, 21203, 21207, 21211, 21212, 21216, 21220, 21224, 21231, 21235, 21236, 21240, 21244, 21248,
    21253, 21256, 21260, 21264, 21268, 21272, 21276, 21283, 21284, 21291, 21295, 21299, 21303, 21307, 21311, 21315,
    21319, 21323, 21327, 21331, 21335, 21339, 21340, 21344, 21348, 21352, 21356, 21363, 21365, 21376, 21383, 21387,
    21391, 21395, 21399, 21403, 21407, 21411, 21433, 21439, 21440, 21447, 21451, 21455, 21456, 21460, 21467, 21471,
    21472, 21479, 21481, 21485, 21491, 21495, 21499, 21503, 21507, 21511, 21515, 21519, 21520, 21527, 21531, 21539,
    21563, 21567, 21571, 21575, 21579, 21583, 21587, 21588, 21595, 21711, 21715, 21719, 21723, 21727, 21731, 21735,
    21739, 21743, 21747, 21751, 21755, 21759, 22287, 22291, 22295, 22299, 22303, 22304, 22311, 22312, 22323, 22324,
    22343, 22347, 22351, 22355, 22359, 22363, 22367, 22371, 22375, 22379, 22383, 22387, 22391, 22395, 22403, 22407,
    22437, 22443, 22447, 22459, 22463, 22520, 22527, 22528, 22532, 22799, 22801, 22805, 22809, 22813, 22817, 22821,
    22825, 22829, 22833, 22837, 22841, 22845, 22849, 22855, 22859, 22863, 22867, 22871, 22875, 22879, 22883, 22887,
    22891, 22895, 22899, 22903, 22907, 22911, 22915, 22919, 22923, 22927, 22931, 22935, 22939, 22943, 22947, 22951,
    22955, 22959, 22963, 22967, 22971, 22975, 22979, 22983, 22987, 22991, 22995, 22999, 23003, 23007, 23011, 23015,
    23019, 23023, 23027, 23031, 23035, 23039, 23043, 23047, 23051, 23055, 23059, 23063, 23067, 23071, 23075, 23079,
    23083, 23087, 23091, 23095, 23099, 23103, 23107, 23111, 23115, 23119, 23123, 23127, 23131, 23135, 23139, 23143,
    23147, 23151, 23155, 23159, 23163, 23167, 23171, 23175, 23179, 23183, 23187, 23191, 23195, 23199, 23203, 23207,
    23211, 23215, 23219, 23223, 23227, 23231, 23235, 23239, 23243, 23247, 23251, 23255, 23259, 23263, 23267, 23275,
    23279, 23280, 23363, 23367, 23371, 23375, 23387, 23391, 23395, 23399, 23403, 23407, 23411, 23415, 23419, 23423,
    23427, 23431, 23435, 23439, 23443, 23447, 23451, 23455, 23459, 23463, 23467, 23471, 23475, 23479, 23483, 23487,
    23491, 23495, 23499, 23503, 23507, 23511, 23515, 23519, 23523, 23527, 23531, 23548, 23645, 23649, 23653, 23695,
    23699, 23703, 23707, 23711, 23715, 23719, 23723, 23727, 23731, 23735, 23739, 23755, 23784, 23788, 23799, 24067,
    24071, 24075, 24079, 24083, 24087, 24091, 24095, 24099, 24103, 24107, 24111, 24115, 24119, 24123, 24127, 24131,
    24143, 24345, 24360, 24364, 24368, 24384, 24467, 24471, 24568, 24572, 24583, 24655, 24656, 24663, 24664, 24797,
    24884, 24924, 24928, 24932, 24936, 24940, 24951, 24971, 24979, 24992, 24996, 25000, 25007, 25011, 25015, 25019,
    25035, 25036, 25607, 25611, 25615, 25619, 25623, 25627, 25631, 25635, 25707, 25715, 25716, 26119, 26123, 26127,
    27667, 27671, 27675,
};
// -1: unknown opcode, else its flags.
inline int opFlags(uint32_t op) {
    if (op > 0x3FFF) return -1;
    size_t lo = 0, hi = sizeof kOps / sizeof kOps[0];
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint32_t o = kOps[mid] >> 2u;
        if (o == op) return kOps[mid] & 3;
        if (o < op) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}
// The literal string starting at word `at` of an instruction of `n` words ends (a nul byte) inside it; `after` is the
// word after it.
inline bool stringEnds(const uint32_t* w, uint32_t n, uint32_t at, uint32_t* after = nullptr) {
    for (uint32_t i = at; i < n; ++i) {
        const uint32_t v = w[i];
        if ((v & 0xFFu) == 0 || (v & 0xFF00u) == 0 || (v & 0xFF0000u) == 0 || (v & 0xFF000000u) == 0) {
            if (after) *after = i + 1;
            return true;
        }
    }
    return false;
}
enum : uint32_t {
    OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7, OpExtension = 10, OpExtInstImport = 11,
    OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeForwardPointer = 39,
    OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpDecorate = 71, OpMemberDecorate = 72,
    OpLabel = 248, OpModuleProcessed = 330,
};
}  // namespace detail

// True when `code` (`words` 32-bit words) passes; else false, and `why` says what's wrong.
inline bool check(const uint32_t* code, size_t words, std::string* why = nullptr) {
    using namespace detail;
    auto fail = [&](const char* what, size_t at) {
        if (why) *why = std::string(what) + " (word " + std::to_string(at) + ")";
        return false;
    };
    if (!code || words < 5) return fail("shorter than a SPIR-V header", 0);
    if (code[0] != 0x07230203u) return fail("not SPIR-V (bad magic number)", 0);
    const uint32_t version = code[1];
    if ((version & 0xFF0000FFu) != 0 || (version >> 16) != 1 || ((version >> 8) & 0xFFu) > 6) return fail("unknown SPIR-V version", 1);
    const uint32_t bound = code[3];
    if (bound == 0 || bound > (1u << 22)) return fail("id bound out of range", 3);
    if (code[4] != 0) return fail("non-zero schema", 4);
    std::vector<uint8_t> defined(bound, 0);
    std::vector<uint32_t> typeUses;                        // result types named: checked once every id is known
    uint32_t memoryModels = 0, entryPoints = 0;
    bool inFunction = false, first = true;
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16, op = code[i] & 0xFFFFu;
        if (n == 0) return fail("instruction of 0 words", i);
        if (n > words - i) return fail("instruction runs past the end", i);
        const uint32_t* w = code + i;
        if (first && op != OpCapability) return fail("doesn't start with OpCapability", i);
        first = false;
        const int flags = opFlags(op);
        if (flags > 0) {
            const uint32_t at = (flags & 2) ? 2u : 1u;
            if (n <= at) return fail("instruction too short for its result", i);
            if (flags & 2) {
                if (w[1] == 0 || w[1] >= bound) return fail("result type out of range", i);
                typeUses.push_back(w[1]);
            }
            const uint32_t id = w[at];
            if (id == 0 || id >= bound) return fail("result id out of range", i);
            if (defined[id]) return fail("result id defined twice", i);
            defined[id] = 1;
        }
        switch (op) {
            case OpSourceExtension: case OpExtension: case OpModuleProcessed:
                if (!stringEnds(w, n, 1)) return fail("unterminated string", i);
                break;
            case OpName:
                if (n < 3 || w[1] == 0 || w[1] >= bound || !stringEnds(w, n, 2)) return fail("bad OpName", i);
                break;
            case OpMemberName:
                if (n < 4 || w[1] == 0 || w[1] >= bound || !stringEnds(w, n, 3)) return fail("bad OpMemberName", i);
                break;
            case OpString: case OpExtInstImport:
                if (!stringEnds(w, n, 2)) return fail("unterminated string", i);
                break;
            case OpMemoryModel:
                if (n < 3) return fail("bad OpMemoryModel", i);
                ++memoryModels;
                break;
            case OpEntryPoint: {
                uint32_t after = 0;
                if (n < 4 || w[2] == 0 || w[2] >= bound || !stringEnds(w, n, 3, &after)) return fail("bad OpEntryPoint", i);
                for (uint32_t k = after; k < n; ++k)
                    if (w[k] == 0 || w[k] >= bound) return fail("OpEntryPoint interface id out of range", i);
                ++entryPoints;
                break;
            }
            case OpExecutionMode:
                if (n < 3 || w[1] == 0 || w[1] >= bound) return fail("bad OpExecutionMode", i);
                break;
            case OpDecorate: case OpMemberDecorate:
                if (n < (op == OpDecorate ? 3u : 4u) || w[1] == 0 || w[1] >= bound) return fail("bad decoration", i);
                break;
            case OpTypeForwardPointer:
                if (n < 3 || w[1] == 0 || w[1] >= bound) return fail("bad OpTypeForwardPointer", i);
                break;
            case OpFunction:
                if (inFunction) return fail("function inside a function", i);
                inFunction = true;
                break;
            case OpFunctionEnd:
                if (!inFunction) return fail("OpFunctionEnd outside a function", i);
                inFunction = false;
                break;
            case OpFunctionParameter: case OpLabel:
                if (!inFunction) return fail("block or parameter outside a function", i);
                break;
            default: break;
        }
        i += n;
    }
    if (inFunction) return fail("function not ended", words);
    if (memoryModels != 1) return fail("not exactly one OpMemoryModel", words);
    if (entryPoints == 0) return fail("no OpEntryPoint", words);
    for (uint32_t t : typeUses)
        if (!defined[t]) return fail("a result type that's never defined", words);
    return true;
}
inline bool check(const std::vector<uint32_t>& code, std::string* why = nullptr) { return check(code.data(), code.size(), why); }

}  // namespace spvcheck
