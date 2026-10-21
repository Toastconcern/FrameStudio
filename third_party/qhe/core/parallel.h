#pragma once
// A small persistent thread pool for per-frame work split across cores: the CPU animation's skinning and the
// renderer's vertex rebuilds. forEach(n, fn) runs fn(i) for every i < n on the pool and the calling thread, and
// returns when all are done. Items are handed out one at a time (they differ a lot in size). Not re-entrant: a
// forEach inside fn runs serially.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace par {

class Pool {
public:
    static Pool& get() { static Pool p; return p; }

    void forEach(size_t n, const std::function<void(size_t)>& fn) {
        if (n == 0) return;
        if (n == 1 || threads_.empty() || inside_) { for (size_t i = 0; i < n; ++i) fn(i); return; }
        std::unique_lock<std::mutex> busy(run_);   // one forEach at a time (from any thread)
        {
            std::lock_guard<std::mutex> g(m_);
            fn_ = &fn; n_ = n; next_.store(0); left_ = threads_.size();
            ++gen_;
        }
        cv_.notify_all();
        work();
        std::unique_lock<std::mutex> g(m_);
        done_.wait(g, [&] { return left_ == 0; });
        fn_ = nullptr;
    }

    ~Pool() {
        { std::lock_guard<std::mutex> g(m_); stop_ = true; ++gen_; }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }

private:
    Pool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned n = hw > 2 ? std::min(hw - 2, 6u) : 0;   // leave a core for the render thread's other work
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }
    void work() {
        inside_ = true;
        for (size_t i; (i = next_.fetch_add(1)) < n_;) (*fn_)(i);
        inside_ = false;
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> g(m_);
                cv_.wait(g, [&] { return gen_ != seen; });
                seen = gen_;
                if (stop_) return;
            }
            work();
            std::lock_guard<std::mutex> g(m_);
            if (--left_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_, run_;
    std::condition_variable cv_, done_;
    const std::function<void(size_t)>* fn_ = nullptr;
    size_t n_ = 0, left_ = 0;
    std::atomic<size_t> next_{0};
    uint64_t gen_ = 0;
    bool stop_ = false;
    static inline thread_local bool inside_ = false;
};

inline void forEach(size_t n, const std::function<void(size_t)>& fn) { Pool::get().forEach(n, fn); }

}  // namespace par
