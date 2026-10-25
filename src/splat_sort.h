#pragma once
// The draw order of one splat object: splats blend back to front, so the order follows the viewer. Sorted on the CPU by
// each splat's distance from one point (the midpoint between the eyes: one order serves both eyes), as 16-bit keys and a
// counting sort (two passes over the splats: a few ms for a million on the Frame). A worker thread of its own sorts
// while frames go on drawing the last order; the newest request wins (one asked for while it sorts waits, the ones
// before it are dropped). Orders are recycled from a few buffers, never allocated per sort.
#include "splat_import.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class SplatSorter {
public:
    using Order = std::vector<uint32_t>;

    // The first `count` splats of `splats` (their positions are copied: the splats may go once this is made).
    SplatSorter(const std::vector<Splat>& splats, size_t count) {
        pos_.resize(count * 3);
        for (size_t i = 0; i < count; ++i) std::memcpy(&pos_[i * 3], splats[i].pos, 12);
        worker_ = std::thread([this] { run(); });
    }
    ~SplatSorter() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    SplatSorter(const SplatSorter&) = delete;
    SplatSorter& operator=(const SplatSorter&) = delete;

    // An order for a viewer at `eye` (world) with the object placed at `world` (column-major): on the worker, or (now)
    // here before returning: the first frame that draws it, a teleport.
    void request(const float eye[3], const float world[16], bool now) {
        const uint64_t id = ++requests_;
        if (now) {
            std::shared_ptr<Order> out = take();
            sort(pos_, eye, world, here_, *out);
            publish(out, id);
            return;
        }
        { std::lock_guard<std::mutex> l(m_); std::memcpy(want_.eye, eye, 12); std::memcpy(want_.world, world, 64); want_.id = id; }
        cv_.notify_one();
    }
    // The newest order finished (empty: none yet) and its serial (a new one each time it changes).
    std::shared_ptr<const Order> latest(uint64_t& serial) const {
        std::lock_guard<std::mutex> l(m_);
        serial = doneId_;
        return done_;
    }
    size_t count() const { return pos_.size() / 3; }

    // Farthest first from `eye`, the positions placed by `world`: 16-bit keys over the range of distances there are.
    struct Scratch { std::vector<float> dist; std::vector<uint16_t> keys; std::vector<uint32_t> counts; };
    static void sort(const std::vector<float>& pos, const float eye[3], const float* w, Scratch& s, Order& out) {
        const size_t n = pos.size() / 3;
        out.resize(n);
        s.keys.resize(n);
        s.dist.resize(n);
        if (!n) return;
        std::vector<float>& dist = s.dist;
        std::vector<uint16_t>& keys = s.keys;
        std::vector<uint32_t>& counts = s.counts;
        float lo = 1e30f, hi = 0;
        for (size_t i = 0; i < n; ++i) {
            const float* p = &pos[i * 3];
            const float x = w[0] * p[0] + w[4] * p[1] + w[8] * p[2] + w[12] - eye[0];
            const float y = w[1] * p[0] + w[5] * p[1] + w[9] * p[2] + w[13] - eye[1];
            const float z = w[2] * p[0] + w[6] * p[1] + w[10] * p[2] + w[14] - eye[2];
            const float d = std::sqrt(x * x + y * y + z * z);
            dist[i] = d;
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        const float scale = hi > lo ? 65535.0f / (hi - lo) : 0.0f;
        counts.assign(65536, 0);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t k = static_cast<uint16_t>(65535 - static_cast<uint32_t>((dist[i] - lo) * scale));   // far = 0: first
            keys[i] = k;
            ++counts[k];
        }
        uint32_t at = 0;
        for (uint32_t& c : counts) { const uint32_t k = c; c = at; at += k; }
        for (size_t i = 0; i < n; ++i) out[counts[keys[i]]++] = static_cast<uint32_t>(i);
    }

private:
    struct Want { float eye[3] = {0, 0, 0}; float world[16] = {}; uint64_t id = 0; };

    void run() {
        Scratch scratch;
        uint64_t doneWant = 0;
        for (;;) {
            Want w;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || want_.id != doneWant; });
                if (stop_) return;
                w = want_;
                doneWant = w.id;
            }
            std::shared_ptr<Order> out = take();
            sort(pos_, w.eye, w.world, scratch, *out);
            publish(out, w.id);
        }
    }
    // A buffer nobody holds (the renderer drops an order once it's copied), else a new one: three in use at most.
    std::shared_ptr<Order> take() {
        std::lock_guard<std::mutex> l(m_);
        for (auto& b : pool_) if (b.use_count() == 1) return b;
        pool_.push_back(std::make_shared<Order>());
        return pool_.back();
    }
    // The newest request's order wins: one that finishes after a newer one (a slow sort, then one made here) is dropped.
    void publish(const std::shared_ptr<Order>& out, uint64_t id) {
        std::lock_guard<std::mutex> l(m_);
        if (id <= doneId_) return;
        done_ = out;
        doneId_ = id;
    }

    std::vector<float> pos_;
    Scratch here_;                      // request(now)'s (the renderer's thread)
    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    Want want_;
    std::atomic<uint64_t> requests_{0};
    std::vector<std::shared_ptr<Order>> pool_;
    std::shared_ptr<const Order> done_;
    uint64_t doneId_ = 0;
};
