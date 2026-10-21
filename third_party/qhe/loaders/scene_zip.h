#pragma once
// An APK's assets/scene.zip, unpacked once and shared by everything that reads it while a home loads: the HSL,
// Clay and glTF loaders, the shader / light-probe scan and the ambience search each unpacked their own copy (a
// 610 MB scene.zip was five copies at once, 5 GB at the peak). loadHslScene holds a SceneZipKeep for the whole
// load; without one (a loader used on its own) each call unpacks afresh and the copy goes with its last user.
#include "core/types.h"
#include "loaders/zip_reader_guard.h"
#include "miniz.h"
#ifdef QHE_HAVE_LIBDEFLATE
#include <libdeflate.h>
#include <cstdio>
#endif

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scenezip {

struct Entry {
    std::mutex m;                                       // one unpack per APK; others asking for it wait for that one
    std::shared_ptr<const std::vector<u8>> data;
    bool done = false;
};
struct Cache {
    std::mutex m;
    std::map<std::string, std::shared_ptr<Entry>> kept;   // apk path -> its scene.zip, while a Keep holds the path
    std::map<std::string, int> keepers;
};
inline Cache& cache() { static Cache c; return c; }

#ifdef QHE_HAVE_LIBDEFLATE
// A deflated entry inflated in one go with libdeflate (several times miniz's speed: unpacking a 300 MB scene.zip
// was over half a big home's load), CRC checked. False: not done (stored, unreadable): miniz then does it.
inline bool inflateWhole(const std::string& apkPath, const mz_zip_archive_file_stat& st, std::vector<u8>& out) {
    if (st.m_method != MZ_DEFLATED || st.m_comp_size == 0 || st.m_comp_size > (4ull << 30)) return false;
    FILE* f = std::fopen(apkPath.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    unsigned char lh[30];
    if (std::fseek(f, static_cast<long>(st.m_local_header_ofs), SEEK_SET) == 0 && std::fread(lh, 1, 30, f) == 30 &&
        lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04) {
        const unsigned nameLen = lh[26] | lh[27] << 8, extraLen = lh[28] | lh[29] << 8;
        std::vector<u8> in(static_cast<size_t>(st.m_comp_size));
        if (std::fseek(f, static_cast<long>(st.m_local_header_ofs + 30 + nameLen + extraLen), SEEK_SET) == 0 &&
            std::fread(in.data(), 1, in.size(), f) == in.size()) {
            if (libdeflate_decompressor* d = libdeflate_alloc_decompressor()) {
                size_t got = 0;
                ok = libdeflate_deflate_decompress(d, in.data(), in.size(), out.data(), out.size(), &got) == LIBDEFLATE_SUCCESS &&
                     got == out.size() && libdeflate_crc32(0, out.data(), out.size()) == st.m_crc32;
                libdeflate_free_decompressor(d);
            }
        }
    }
    std::fclose(f);
    return ok;
}
#endif

inline std::shared_ptr<const std::vector<u8>> unpack(const std::string& apkPath) {
    mz_zip_archive z = {};
    if (!mz_zip_reader_init_file(&z, apkPath.c_str(), 0)) return nullptr;
    std::shared_ptr<std::vector<u8>> out;
    const int idx = zipread::validateArchive(z, hslcook::zipsafety::kApkReadLimits) ? mz_zip_reader_locate_file(&z, "assets/scene.zip", nullptr, 0) : -1;
    mz_zip_archive_file_stat st;
    if (idx >= 0 && mz_zip_reader_file_stat(&z, static_cast<mz_uint>(idx), &st)) {
        out = std::make_shared<std::vector<u8>>(static_cast<size_t>(st.m_uncomp_size));   // straight into place: no second copy
#ifdef QHE_HAVE_LIBDEFLATE
        if (!inflateWhole(apkPath, st, *out))
#endif
        if (!mz_zip_reader_extract_to_mem(&z, static_cast<mz_uint>(idx), out->data(), out->size(), 0)) out.reset();
    }
    mz_zip_reader_end(&z);
    return out;
}

// The scene.zip of a (validated) APK; null when it can't be opened, is unsafe or has none.
inline std::shared_ptr<const std::vector<u8>> get(const std::string& apkPath) {
    std::shared_ptr<Entry> e;
    {
        Cache& c = cache();
        std::lock_guard<std::mutex> l(c.m);
        if (!c.keepers.count(apkPath)) e = nullptr;
        else if (auto& k = c.kept[apkPath]) e = k;
        else e = k = std::make_shared<Entry>();
    }
    if (!e) return unpack(apkPath);
    std::lock_guard<std::mutex> l(e->m);
    if (!e->done) { e->data = unpack(apkPath); e->done = true; }
    return e->data;
}

// Keeps each APK's scene.zip for its lifetime once something has read it.
struct Keep {
    std::vector<std::string> paths;
    explicit Keep(const std::vector<std::string>& p) : paths(p) {
        std::lock_guard<std::mutex> l(cache().m);
        for (const auto& s : paths) ++cache().keepers[s];
    }
    ~Keep() {
        std::lock_guard<std::mutex> l(cache().m);
        for (const auto& s : paths)
            if (--cache().keepers[s] <= 0) { cache().keepers.erase(s); cache().kept.erase(s); }
    }
    Keep(const Keep&) = delete;
    Keep& operator=(const Keep&) = delete;
};

}  // namespace scenezip
