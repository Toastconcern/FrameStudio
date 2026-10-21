#pragma once
#include "core/types.h"
#include <vector>
#include <cstring>
#include <limits>
#include <utility>

// Parses content/assets.manifest — ASMH v2 binary with FlatBuffer encoding.
//
// Binary layout:
//   +0:  "ASMH" magic
//   +4:  version u32 (= 2)
//   +8:  padding u64 (zeros)
//   +16: root_table_offset u32 — offset of root FlatBuffer table from position 16
//
// Root table uses standard FlatBuffer vtable layout.  One of its fields is
// the CONTENT vector: each element holds (pkg u64, ing u64, tgt u32, path str).
//
// CONTENT entry layout (relative to element table start):
//   +0:  soffset i32
//   +4:  pkg_hash u64
//   +12: ing u64
//   +20: tgt u32
//   +24: zeros u64
//   +32: path_str_ref i32 — signed offset from this field to FlatBuffer string header
//
// FlatBuffer vector elements store SIGNED offsets relative to the element's
// own position: table_address = element_position + i32(element_position).
//
// Strategy: walk vtable fields; the first field whose first entry has a valid
// ing (>= 2^32) and a content path ("<publisher>/.../<target>") is the CONTENT field.
// Official homes publish under "meta/", community homes under their own namespace
// (e.g. "zasha5234steam/theaterhome/0.0.1/...") — both are valid entries.

inline bool isAsmhContentPath(const std::string& path) {
    return !path.empty() && path.find('/') != std::string::npos;
}

// A lossless view of one CONTENT entry.  Unlike AssetMap, this representation
// deliberately does not manufacture pkg=0 fallback keys: one manifest record
// produces exactly one AsmhRecord.
struct AsmhRecord {
    u64 pkg = 0;
    u64 ing = 0;
    u32 tgt = 0;
    std::string path;
    u32 fourcc = 0;
    u32 subtype = 0;
    u32 size = 0;
};

// Parses every record in the ASMH-v2 CONTENT vector, including the complete
// 8-byte asset type and declared payload size.  Parsing is transactional: a
// malformed record makes the whole read fail and leaves `out` empty.  This is
// intentional for callers that will rewrite a manifest and must not silently
// drop records.
inline bool parseAsmhRecords(const std::vector<u8>& data,
                             std::vector<AsmhRecord>& out) {
    out.clear();
    if (data.size() < 0x40 ||
        data.size() > static_cast<size_t>(std::numeric_limits<u32>::max()))
        return false;
    if (std::memcmp(data.data(), "ASMH", 4) != 0) return false;

    const size_t sz = data.size();
    auto contains = [&](size_t offset, size_t bytes) -> bool {
        return offset <= sz && bytes <= sz - offset;
    };
    auto readU16 = [&](size_t offset, u16& value) -> bool {
        if (!contains(offset, sizeof(value))) return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    };
    auto readU32 = [&](size_t offset, u32& value) -> bool {
        if (!contains(offset, sizeof(value))) return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    };
    auto readI32 = [&](size_t offset, i32& value) -> bool {
        if (!contains(offset, sizeof(value))) return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    };
    auto readU64 = [&](size_t offset, u64& value) -> bool {
        if (!contains(offset, sizeof(value))) return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    };
    auto addU32Offset = [&](size_t base, u32 relative, size_t& target) -> bool {
        if (base > sz || static_cast<size_t>(relative) > sz - base) return false;
        target = base + static_cast<size_t>(relative);
        return true;
    };
    auto addI32Offset = [&](size_t base, i32 relative, size_t& target) -> bool {
        const int64_t candidate = static_cast<int64_t>(base) +
                                  static_cast<int64_t>(relative);
        if (candidate < 0 || static_cast<uint64_t>(candidate) > sz) return false;
        target = static_cast<size_t>(candidate);
        return true;
    };
    auto vtableFor = [&](size_t table, size_t& vtable) -> bool {
        i32 distance = 0;
        if (!readI32(table, distance)) return false;
        const int64_t candidate = static_cast<int64_t>(table) -
                                  static_cast<int64_t>(distance);
        if (candidate < 0 || static_cast<uint64_t>(candidate) > sz) return false;
        vtable = static_cast<size_t>(candidate);
        return true;
    };
    auto readString = [&](size_t stringHeader, std::string& value) -> bool {
        u32 length = 0;
        if (!readU32(stringHeader, length) || length == 0 || length > 1024)
            return false;
        const size_t payload = stringHeader + sizeof(u32); // header was in-bounds
        if (!contains(payload, static_cast<size_t>(length))) return false;
        const char* begin = reinterpret_cast<const char*>(data.data() + payload);
        if (std::memchr(begin, '\0', length) != nullptr) return false;
        value.assign(begin, length);
        return true;
    };

    u32 version = 0;
    if (!readU32(4, version) || version != 2) return false;

    // Offset 16 is relative to its own address.
    u32 rootRelative = 0;
    size_t rootTable = 0;
    if (!readU32(16, rootRelative) || rootRelative == 0 ||
        !addU32Offset(16, rootRelative, rootTable) || !contains(rootTable, 4))
        return false;

    size_t rootVtable = 0;
    if (!vtableFor(rootTable, rootVtable)) return false;
    u16 rootVtableSize = 0, rootObjectSize = 0;
    if (!readU16(rootVtable, rootVtableSize) ||
        !readU16(rootVtable + 2, rootObjectSize) ||
        rootVtableSize < 4 || (rootVtableSize & 1u) != 0 ||
        rootObjectSize < 4 ||
        !contains(rootVtable, rootVtableSize) ||
        !contains(rootTable, rootObjectSize))
        return false;

    const size_t fieldCount = (rootVtableSize - 4u) / 2u;

    // Decode one entry only after validating the exact ASMH-v2 CONTENT table
    // shape used by both official manifests (object size 64) and our writer
    // (object size 52).  The four field offsets are stable across both.
    auto readRecord = [&](size_t table, AsmhRecord& record) -> bool {
        size_t entryVtable = 0;
        if (!contains(table, 48) || !vtableFor(table, entryVtable)) return false;

        u16 vtableSize = 0, objectSize = 0;
        if (!readU16(entryVtable, vtableSize) ||
            !readU16(entryVtable + 2, objectSize) ||
            vtableSize < 12 || (vtableSize & 1u) != 0 || objectSize < 48 ||
            !contains(entryVtable, vtableSize) || !contains(table, objectSize))
            return false;

        u16 keyOffset = 0, pathOffset = 0, typeOffset = 0, sizeOffset = 0;
        if (!readU16(entryVtable + 4, keyOffset) ||
            !readU16(entryVtable + 6, pathOffset) ||
            !readU16(entryVtable + 8, typeOffset) ||
            !readU16(entryVtable + 10, sizeOffset) ||
            keyOffset != 4 || pathOffset != 32 ||
            typeOffset != 36 || sizeOffset != 44)
            return false;

        AsmhRecord candidate;
        if (!readU64(table + 4, candidate.pkg) ||
            !readU64(table + 12, candidate.ing) ||
            !readU32(table + 20, candidate.tgt) ||
            !readU32(table + 36, candidate.fourcc) ||
            !readU32(table + 40, candidate.subtype) ||
            !readU32(table + 44, candidate.size) ||
            candidate.ing < 0x100000000ULL)
            return false;

        i32 pathRelative = 0;
        size_t pathHeader = 0;
        if (!readI32(table + 32, pathRelative) || pathRelative <= 0 ||
            !addI32Offset(table + 32, pathRelative, pathHeader) ||
            !readString(pathHeader, candidate.path) ||
            !isAsmhContentPath(candidate.path))
            return false;

        record = std::move(candidate);
        return true;
    };

    // The first root field whose first element validates as a CONTENT record
    // is the CONTENT vector.  This preserves the proven layout-discovery logic
    // while making all arithmetic explicit and overflow-safe.
    for (size_t fieldIndex = 0; fieldIndex < fieldCount; ++fieldIndex) {
        u16 fieldOffset = 0;
        if (!readU16(rootVtable + 4 + fieldIndex * 2, fieldOffset)) return false;
        if (fieldOffset == 0) continue;
        if (fieldOffset > rootObjectSize - sizeof(u32)) continue;

        const size_t field = rootTable + fieldOffset; // bounded by object check
        u32 vectorRelative = 0;
        size_t vector = 0;
        if (!readU32(field, vectorRelative) || vectorRelative == 0 ||
            !addU32Offset(field, vectorRelative, vector) || !contains(vector, 4))
            continue;

        u32 count = 0;
        if (!readU32(vector, count) || count == 0 || count > 10000) continue;
        const size_t elementSlots = vector + sizeof(u32); // header is in-bounds
        if (!contains(elementSlots, static_cast<size_t>(count) * sizeof(u32)))
            continue;

        auto tableAt = [&](size_t index, size_t& table) -> bool {
            const size_t slot = elementSlots + index * sizeof(u32);
            i32 relative = 0;
            return readI32(slot, relative) && relative > 0 &&
                   addI32Offset(slot, relative, table);
        };

        size_t firstTable = 0;
        AsmhRecord firstRecord;
        if (!tableAt(0, firstTable) || !readRecord(firstTable, firstRecord))
            continue;

        std::vector<AsmhRecord> records;
        records.reserve(count);
        records.push_back(std::move(firstRecord));
        for (size_t index = 1; index < count; ++index) {
            size_t table = 0;
            AsmhRecord record;
            if (!tableAt(index, table) || !readRecord(table, record)) {
                out.clear();
                return false;
            }
            records.push_back(std::move(record));
        }

        out = std::move(records);
        return true;
    }

    return false;
}

inline bool parseAsmh(const std::vector<u8>& data, AssetMap& out) {
    if (data.size() < 0x40) return false;
    if (memcmp(data.data(), "ASMH", 4) != 0) return false;

    // All offset arithmetic in 64-bit: every offset is file data, and u32 sums (or i32 adds) wrap past the
    // bounds checks. Signed table offsets go through int64_t so a negative result is rejected, not wrapped.
    const u64 SZ = data.size();
    auto in = [&](int64_t o, u64 n) { return o >= 0 && (u64)o <= SZ && n <= SZ - (u64)o; };

    auto u16at = [&](int64_t o) -> u16 {
        if (!in(o, 2)) return 0;
        u16 v; memcpy(&v, data.data() + o, 2); return v;
    };
    auto u32at = [&](int64_t o) -> u32 {
        if (!in(o, 4)) return 0;
        u32 v; memcpy(&v, data.data() + o, 4); return v;
    };
    auto i32at = [&](int64_t o) -> i32 {
        if (!in(o, 4)) return 0;
        i32 v; memcpy(&v, data.data() + o, 4); return v;
    };
    auto u64at = [&](int64_t o) -> u64 {
        if (!in(o, 8)) return 0;
        u64 v; memcpy(&v, data.data() + o, 8); return v;
    };
    auto fbStr = [&](int64_t o) -> std::string {
        if (!in(o, 4)) return {};
        u32 len = u32at(o);
        if (!len || len > 1024 || !in(o + 4, len)) return {};
        return {reinterpret_cast<const char*>(data.data() + o + 4), len};
    };

    // Navigate to the root FlatBuffer table.
    // Offset 16 stores a u32 that, added to 16, gives the root table position.
    int64_t rootTablePos = (int64_t)u32at(16) + 16;
    if (!in(rootTablePos, 4)) return false;

    int64_t vtable = rootTablePos - (int64_t)i32at(rootTablePos);
    if (!in(vtable, 4)) return false;

    u16 vtSize  = u16at(vtable);
    u32 nFields = (vtSize > 4) ? (vtSize - 4) / 2 : 0;

    // Scan all vtable fields for the CONTENT vector.
    // Identified by: first entry has ing >= 2^32 and a content path.
    for (u32 fi = 0; fi < nFields && out.empty(); ++fi) {
        int64_t foffPos = vtable + 4 + (int64_t)fi * 2;
        if (!in(foffPos, 2)) break;
        u16 fieldOff = u16at(foffPos);
        if (!fieldOff) continue;

        int64_t fieldAbs = rootTablePos + fieldOff;
        if (!in(fieldAbs, 4)) continue;
        u32 vecRel = u32at(fieldAbs);
        if (!vecRel) continue;
        int64_t vecAbs = fieldAbs + vecRel;
        if (!in(vecAbs, 4)) continue;

        u32 count = u32at(vecAbs);
        if (count == 0 || count > 10000) continue;

        // Validate first element as a CONTENT entry.
        int64_t e0pos = vecAbs + 4;
        if (!in(e0pos, 4)) continue;
        int64_t t0 = e0pos + i32at(e0pos);
        if (!in(t0, 36)) continue;

        u64 ing0 = u64at(t0 + 12);
        if (ing0 < 0x100000000ULL) continue;  // must be a large random-looking ID

        i32 pref0 = i32at(t0 + 32);
        if (pref0 <= 0 || (u64)pref0 > SZ) continue;   // string offset is within the (possibly large) manifest, not a fixed 4096
        std::string path0 = fbStr(t0 + 32 + pref0);
        if (!isAsmhContentPath(path0)) continue;

        // This field is the CONTENT section — parse all entries.
        for (u32 i = 0; i < count; ++i) {
            int64_t ep = vecAbs + 4 + (int64_t)i * 4;
            if (!in(ep, 4)) continue;
            int64_t t = ep + i32at(ep);
            if (!in(t, 36)) continue;

            u64 pkg = u64at(t + 4);
            u64 ing = u64at(t + 12);
            u32 tgt = u32at(t + 20);
            if (ing < 0x100000000ULL) continue;

            i32 pref = i32at(t + 32);
            if (pref <= 0 || (u64)pref > SZ) continue;
            std::string path = fbStr(t + 32 + pref);
            if (!isAsmhContentPath(path)) continue;

            // Insert with real pkg AND with pkg=0 so lookups succeed regardless
            // of which packageOrRemoteId the HSTF JSON stores.
            AssetKey k; k.ing = ing; k.tgt = tgt;
            k.pkg = pkg; out[k] = path;
            k.pkg = 0;   out[k] = path;
        }
    }

    return !out.empty();
}
