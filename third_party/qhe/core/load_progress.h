// Global load-progress state shared between the loader worker thread and the UI thread.
// The loaders (scene_loader/gltf_loader/opa_loader) and main's load phase update it at coarse
// checkpoints; the splash painter / editor loading bar read it every frame. Lock-free: the stage
// string must be a STATIC string literal (the reader dereferences it at any time).
#pragma once
#include <atomic>

struct LoadProgress {
    std::atomic<const char*> stage{"Starting..."};
    std::atomic<int>  cur{0};        // items done in this stage (0/0 = indeterminate marquee)
    std::atomic<int>  total{0};
    std::atomic<bool> done{false};   // worker finished (ok or failed)
    std::atomic<bool> failed{false};

    void set(const char* stageLit, int c = 0, int t = 0) {
        cur.store(c, std::memory_order_relaxed);
        total.store(t, std::memory_order_relaxed);
        stage.store(stageLit, std::memory_order_relaxed);
    }
    void tick(int c) { cur.store(c, std::memory_order_relaxed); }
    void bump()      { cur.fetch_add(1, std::memory_order_relaxed); }
};

inline LoadProgress g_loadProgress;
