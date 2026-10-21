#pragma once
// A disk cache of mip chains for textures shipped without one. The first time a home opens, such a texture is drawn
// as before (decoded, shrunk to maxDecodedSide, RGBA mips: slow, heavy in memory) while its real ASTC chain is built
// in the background (tex_mips.h: the full-size level exactly as shipped) and saved here; every later open uploads
// that chain compressed, with nothing decoded. Entries are keyed by the texture's own bytes, so any home (APK, .fenv,
// model) that carries the same texture shares one. Kept under 2 GB (the least recently used go first).
#include "tex_mips.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace texcache {

// FRAME_TEX_CACHE, else %LOCALAPPDATA%\FrameStudio\cache\texmips (Windows) or ~/.cache/frame-home/texmips.
inline std::filesystem::path dir() {
    if (const char* e = std::getenv("FRAME_TEX_CACHE")) return std::filesystem::u8path(e);
#ifdef _WIN32
    if (const char* l = std::getenv("LOCALAPPDATA")) return std::filesystem::u8path(std::string(l) + "\\FrameStudio\\cache\\texmips");
#endif
    if (const char* x = std::getenv("XDG_CACHE_HOME")) return std::filesystem::u8path(std::string(x) + "/frame-home/texmips");
    const char* h = std::getenv("HOME");
    return std::filesystem::u8path(std::string(h ? h : "/tmp") + "/.cache/frame-home/texmips");
}

inline uint64_t key(const TexSource& t) {
    uint64_t k = 1469598103934665603ull;
    auto mix = [&](uint64_t x) { k = (k ^ x) * 0x100000001B3ull; k ^= k >> 29; };
    mix(t.w); mix(t.h); mix(t.bw); mix(t.bh);
    const size_t n = std::min<size_t>(t.astc.size(), astcMip0Bytes(t.w, t.h, t.bw, t.bh));
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t x; std::memcpy(&x, t.astc.data() + i, 8); mix(x); }
    for (; i < n; ++i) mix(t.astc[i]);
    mix(n);
    return k;
}

inline std::filesystem::path pathFor(uint64_t k) {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.ftxm", static_cast<unsigned long long>(k));
    return dir() / name;
}

struct Header { char magic[4]; uint32_t version, w, h, bw, bh, mips; uint64_t bytes; };

// The cached chain for `t`, or null.
inline std::shared_ptr<TexSource> load(const TexSource& t) {
    if (!texmips::wants(t)) return nullptr;
    const std::filesystem::path p = pathFor(key(t));
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return nullptr;
    Header hd = {};
    std::shared_ptr<TexSource> out;
    if (std::fread(&hd, sizeof hd, 1, f) == 1 && std::memcmp(hd.magic, "FTXM", 4) == 0 && hd.version == 1 && hd.w == t.w && hd.h == t.h &&
        hd.bw == t.bw && hd.bh == t.bh && hd.mips == astcFullMipCount(t.w, t.h) && hd.bytes < (1ull << 31)) {
        auto s = std::make_shared<TexSource>();
        s->w = t.w; s->h = t.h; s->bw = t.bw; s->bh = t.bh; s->mips = hd.mips; s->hdr = t.hdr; s->faces = 1;
        s->astc.resize(static_cast<size_t>(hd.bytes));
        if (std::fread(s->astc.data(), 1, s->astc.size(), f) == s->astc.size()) out = std::move(s);
    }
    std::fclose(f);
    if (out) {   // used: the last to be pruned
        std::error_code ec;
        std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now(), ec);
    }
    return out;
}

inline void prune(uint64_t limit) {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    uint64_t total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir(), ec)) {
        if (e.path().extension() != ".ftxm") continue;
        const uint64_t s = e.file_size(ec);
        total += s;
        files.push_back({e.last_write_time(ec), e.path()});
    }
    if (total <= limit) return;
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        if (total <= limit) break;
        const uint64_t s = std::filesystem::file_size(f.second, ec);
        if (std::filesystem::remove(f.second, ec)) total -= s;
    }
}

inline bool store(const TexSource& chain, uint64_t k) {
    std::error_code ec;
    std::filesystem::create_directories(dir(), ec);
    const std::filesystem::path p = pathFor(k), tmp = p.string() + ".part";
    FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    Header hd = {{'F', 'T', 'X', 'M'}, 1, chain.w, chain.h, chain.bw, chain.bh, chain.mips, chain.astc.size()};
    const bool ok = std::fwrite(&hd, sizeof hd, 1, f) == 1 && std::fwrite(chain.astc.data(), 1, chain.astc.size(), f) == chain.astc.size();
    if (std::fclose(f) != 0 || !ok) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, p, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    prune(2ull << 30);
    return true;
}

// Builds and saves `t`'s chain on a background thread at the lowest priority, one texture at a time, two encoder
// threads (never in the way of a frame). Each texture once per run.
class Builder {
public:
    static Builder& get() { static Builder b; return b; }
    void add(std::shared_ptr<const TexSource> t) {
        if (!t || !texmips::wants(*t)) return;
        std::lock_guard<std::mutex> l(m_);
        if (!queued_.insert(t.get()).second) return;
        q_.push_back(std::move(t));
        if (!th_.joinable()) th_ = std::thread([this] { run(); });
        cv_.notify_one();
    }
    size_t pending() { std::lock_guard<std::mutex> l(m_); return q_.size() + (busy_ ? 1 : 0); }
    ~Builder() {   // never holds up an exit: a texture half encoded is left as a .part file
        { std::lock_guard<std::mutex> l(m_); stop_ = true; q_.clear(); }
        cv_.notify_all();
        if (th_.joinable()) th_.detach();
    }

private:
    void run() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);   // (its encoder threads inherit it)
#endif
        for (;;) {
            std::shared_ptr<const TexSource> t;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || !q_.empty(); });
                if (stop_) return;
                t = std::move(q_.front());
                q_.pop_front();
                busy_ = true;
            }
            const uint64_t k = key(*t);
            std::error_code ec;
            if (t.use_count() > 1 && !std::filesystem::exists(pathFor(k), ec))   // (held only here: its home is closed)
                if (std::shared_ptr<TexSource> chain = texmips::withMips(*t, 2)) store(*chain, k);
            std::lock_guard<std::mutex> l(m_);
            busy_ = false;
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<const TexSource>> q_;
    std::set<const TexSource*> queued_;
    std::thread th_;
    bool stop_ = false, busy_ = false;
};

}  // namespace texcache
