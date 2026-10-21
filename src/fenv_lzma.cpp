#include "fenv_lzma.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

extern "C" {
#include "LzmaDec.h"
#include "LzmaEnc.h"
}

namespace {

void* lzAlloc(ISzAllocPtr, size_t size) { return ::operator new(size, std::nothrow); }
void lzFree(ISzAllocPtr, void* p) { ::operator delete(p); }
const ISzAlloc kAlloc = {lzAlloc, lzFree};

struct Progress {
    ICompressProgress vt;
    std::atomic<size_t>* done;
    size_t last = 0, total;
    const std::function<void(size_t, size_t)>* cb;
};
SRes onProgress(ICompressProgressPtr p, UInt64 in, UInt64) {
    Progress* pr = reinterpret_cast<Progress*>(const_cast<ICompressProgress*>(p));   // vt is the first member
    const size_t now = static_cast<size_t>(in);
    if (now > pr->last) { *pr->done += now - pr->last; pr->last = now; }
    if (*pr->cb) (*pr->cb)(pr->done->load(), pr->total);
    return SZ_OK;
}

struct Chunk { size_t at = 0, size = 0; std::vector<uint8_t> packed; uint8_t props[5] = {}; SRes res = SZ_OK; };

void put64(std::vector<uint8_t>& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>(v >> (8 * i))); }
uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }

// Runs fn(i) for i in [0, n) on up to `threads` threads.
template <typename F> void parallel(size_t n, unsigned threads, F&& fn) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::min<size_t>(threads, n); ++t)
        pool.emplace_back([&] { for (size_t i; (i = next++) < n;) fn(i); });
    for (auto& th : pool) th.join();
}

}  // namespace

bool fenvLzmaIsStream(const uint8_t* data, size_t size) {
    return size >= 8 && (std::memcmp(data, "FLZ2", 4) == 0 || std::memcmp(data, "FLZ1", 4) == 0);
}

bool fenvLzmaCompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error, FenvPacking packing,
                      const std::function<void(size_t, size_t)>& progress) {
    // A 32-bit process (Frame Studio for 32-bit Windows / Linux) has 2-4 GB of address space, and the encoder takes
    // ~11.5x its dictionary: 256 MB dictionaries on 8 threads need far more. There: 32 MB at most, two at a time
    // (a little bigger file; the decoder reads the size from the stream, so every build opens it).
    constexpr bool kSmallAddressSpace = sizeof(void*) < 8;
    constexpr uint32_t kMaxDict = kSmallAddressSpace ? (32u << 20) : (256u << 20);
    const unsigned hw = kSmallAddressSpace ? std::min(2u, std::max(1u, std::thread::hardware_concurrency()))
                                           : std::max(1u, std::thread::hardware_concurrency());
    std::vector<Chunk> chunks;
    if (packing == FenvPacking::Fast && size > (48u << 20)) {
        // Up to 8 chunks of at least 32 MB: more would cost size (each chunk starts with an empty model) and memory.
        const size_t n = std::min<size_t>({8, hw, (size + (32u << 20) - 1) / (32u << 20)});
        const size_t step = ((size + n - 1) / n + 15) & ~size_t(15);   // on 16-byte steps: ASTC blocks stay aligned
        for (size_t at = 0; at < size; at += step) { Chunk c; c.at = at; c.size = std::min(step, size - at); chunks.push_back(std::move(c)); }
    } else {
        Chunk c; c.size = size; chunks.push_back(std::move(c));
    }
    std::atomic<size_t> done{0};
    const bool single = chunks.size() == 1;
    parallel(chunks.size(), hw, [&](size_t i) {
        Chunk& c = chunks[i];
        CLzmaEncProps p;
        LzmaEncProps_Init(&p);
        p.level = 9;
        p.dictSize = 1u << 20;
        while (p.dictSize < c.size && p.dictSize < kMaxDict) p.dictSize <<= 1;
        p.lc = 0; p.lp = 4; p.pb = 4;   // ASTC: a 16-byte block's byte position predicts best
        p.algo = 1; p.fb = 64; p.btMode = 1; p.numHashBytes = 4; p.mc = 1u << 30;   // longer fb finds nothing more
        p.numThreads = single ? 2 : 1;  // one stream: the match finder on a second thread
        SizeT len = c.size + c.size / 3 + (1 << 16);
        c.packed.resize(len);
        SizeT propsLen = 5;
        Progress pr{{onProgress}, &done, 0, size, &progress};
        c.res = LzmaEncode(c.packed.data(), &len, data + c.at, c.size, &p, c.props, &propsLen, 0, &pr.vt, &kAlloc, &kAlloc);
        c.packed.resize(c.res == SZ_OK ? len : 0);
    });
    out.clear();
    out.insert(out.end(), {'F', 'L', 'Z', '2'});
    const uint32_t n = static_cast<uint32_t>(chunks.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * i)));
    for (const Chunk& c : chunks) {
        if (c.res != SZ_OK) { error = c.res == SZ_ERROR_MEM ? "not enough memory to compress" : "compression failed"; out.clear(); return false; }
        out.insert(out.end(), c.props, c.props + 5);
        put64(out, c.size);
        put64(out, c.packed.size());
        out.insert(out.end(), c.packed.begin(), c.packed.end());
    }
    return true;
}

bool fenvLzmaDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error) {
    if (!data || !fenvLzmaIsStream(data, size)) { error = "not an LZMA stream"; return false; }
    // The sizes in the header are the file's word: a damaged one must not make us allocate terabytes. A real scene
    // is a few hundred MB and packs at most a few times smaller; 1000x the packed size (or 8 GB) is never one.
    const uint64_t cap = std::min<uint64_t>(uint64_t(8) << 30, static_cast<uint64_t>(size) * 1000 + (1u << 20));
    struct Part { const uint8_t* props; uint64_t raw, packed; const uint8_t* src; size_t at; };
    std::vector<Part> parts;
    uint64_t total = 0;
    if (std::memcmp(data, "FLZ1", 4) == 0) {   // earlier builds: one chunk, size then data
        if (size < 17) { error = "damaged LZMA stream"; return false; }
        parts.push_back({data + 4, get64(data + 9), size - 17, data + 17, 0});
        total = parts[0].raw;
    } else {
        uint32_t n = 0;
        std::memcpy(&n, data + 4, 4);
        size_t at = 8;
        for (uint32_t i = 0; i < n; ++i) {
            if (at + 21 > size) { error = "damaged LZMA stream"; return false; }
            Part p{data + at, get64(data + at + 5), get64(data + at + 13), data + at + 21, static_cast<size_t>(total)};
            if (p.packed > size - at - 21 || p.raw > cap) { error = "damaged LZMA stream"; return false; }
            at += 21 + p.packed;
            total += p.raw;
            if (total > cap) { error = "damaged LZMA stream (claims " + std::to_string(total >> 20) + " MB)"; return false; }
            parts.push_back(p);
        }
    }
    if (total > cap || total > SIZE_MAX) { error = "damaged LZMA stream (claims " + std::to_string(total >> 20) + " MB)"; return false; }
    try {
        out.resize(static_cast<size_t>(total));
    } catch (const std::exception&) {            // bad_alloc / length_error: within the cap, but not in this memory
        out = std::vector<uint8_t>();
        error = "not enough memory to unpack " + std::to_string(total >> 20) + " MB";
        return false;
    }
    std::atomic<bool> ok{true};
    parallel(parts.size(), std::max(1u, std::thread::hardware_concurrency()), [&](size_t i) {
        const Part& p = parts[i];
        SizeT destLen = p.raw, srcLen = p.packed;
        ELzmaStatus status;
        const SRes r = LzmaDecode(out.data() + p.at, &destLen, p.src, &srcLen, p.props, 5, LZMA_FINISH_END, &status, &kAlloc);
        if (r != SZ_OK || destLen != p.raw) ok = false;
    });
    if (!ok) { error = "damaged LZMA stream"; out.clear(); return false; }
    return true;
}
