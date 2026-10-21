#pragma once
// Walking while the Steam menu is open. SteamVR gives a scene app (Frame Home) no controller input while its
// dashboard — the Steam menu — is up, even when you're looking away from it. frame-switcher, an overlay app that
// always runs, takes the sticks through a global-priority action set (steamvr.globalActionSetPriority) whenever the
// menu is up, Frame Home is the environment, and that hand isn't pointing at a panel, and passes them to Frame Home
// through a shared page (/dev/shm/framehome-sticks). Pointing at the menu still drives the menu.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

struct MenuSticks {
    static constexpr uint32_t kMagic = 0x46534b31;   // "FSK1"
    uint32_t magic;
    std::atomic<uint32_t> seq;          // odd while being written
    double time;                        // CLOCK_MONOTONIC seconds when written (both processes share the clock)
    float lx, ly, rx, ry;               // thumbsticks
    uint8_t left, right;                // which hands are taken for walking / turning
    uint8_t aimValid;
    float aim[12];                      // right hand's aim pose in standing space (3x4, row-major)
};

inline double menuNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

#ifdef _WIN32
// (Windows: a named mapping; kept open for the process's life, as the Linux one stays mapped)
inline MenuSticks* menuSticksMap(bool create) {
    const HANDLE h = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(MenuSticks), L"Local\\FrameHomeSticks")
                            : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\FrameHomeSticks");
    if (!h) return nullptr;
    void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MenuSticks));
    return static_cast<MenuSticks*>(p);
}
#else
inline MenuSticks* menuSticksMap(bool create) {
    const int fd = open("/dev/shm/framehome-sticks", O_RDWR | (create ? O_CREAT : 0), 0600);
    if (fd < 0) return nullptr;
    if (create && ftruncate(fd, sizeof(MenuSticks)) != 0) { close(fd); return nullptr; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(MenuSticks))) { close(fd); return nullptr; }   // not sized yet
    void* p = mmap(nullptr, sizeof(MenuSticks), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? nullptr : static_cast<MenuSticks*>(p);
}
#endif

// A consistent copy (seqlock); false when there is nothing recent (within maxAge seconds).
inline bool menuSticksRead(const MenuSticks* m, MenuSticks& out, double maxAge = 0.15) {
    if (!m || m->magic != MenuSticks::kMagic) return false;
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t a = m->seq.load(std::memory_order_acquire);
        if (a & 1) continue;
        std::memcpy(static_cast<void*>(&out), static_cast<const void*>(m), sizeof out);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m->seq.load(std::memory_order_relaxed) == a) return menuNow() - out.time < maxAge;
    }
    return false;
}

inline void menuSticksWrite(MenuSticks* m, const MenuSticks& v) {
    if (!m) return;
    uint32_t s = m->seq.load(std::memory_order_relaxed);
    if (s & 1) ++s;                                     // a writer killed mid-write left it odd: start even again
    m->seq.store(s + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    m->time = v.time; m->lx = v.lx; m->ly = v.ly; m->rx = v.rx; m->ry = v.ry;
    m->left = v.left; m->right = v.right; m->aimValid = v.aimValid;
    std::memcpy(m->aim, v.aim, sizeof m->aim);
    m->magic = MenuSticks::kMagic;
    std::atomic_thread_fence(std::memory_order_release);
    m->seq.store(s + 2, std::memory_order_release);
}
