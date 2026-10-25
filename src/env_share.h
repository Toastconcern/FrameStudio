#pragma once
// Frame Home as the system environment: what Frame Home (frame-home --system) and the compositor layer
// (layer/frame_home_layer.cpp, inside vrcompositor) share.
//
// Frame Home renders the home for both eyes into one of kSlots exportable 2-layer images (Vulkan external memory,
// opaque fds) and publishes it here once the GPU has finished it. The layer imports the images on the compositor's
// device and, where the compositor draws Aurora, draws a shader that looks each pixel's direction up in the newest
// published frame (rotation reprojection from the pose that frame was rendered with). No frame, or a stale one,
// and the layer leaves Aurora alone: Frame Home stopping or crashing never blanks the headset.
//
// Hand-over, Linux: Frame Home connects to the layer's unix socket (kSocketName in $XDG_RUNTIME_DIR) and sends one
// Hello with SCM_RIGHTS fds: kSlots image memories, then the shared State (a memfd). A new Hello (new size) replaces
// the old images. Frame Home owns the images (Vulkan external memory, opaque fds) and the layer imports them.
//
// Hand-over, Windows: the compositor is Direct3D 11, which cannot open Vulkan's exported memory (only the reverse),
// so the images' ownership flips while the State stays Frame Home's (as on Linux). Frame Home connects to the layer's
// named pipe (kPipeName) and sends a WinRequest: the size and format it wants, and the shared State's file-mapping
// handle duplicated into the layer's process. The layer, inside vrcompositor, creates kSlots shared D3D11 textures,
// duplicates their NT handles into Frame Home's process, and replies with a WinReply carrying the handle values and
// the device LUID. Frame Home imports the textures into Vulkan (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT) and
// renders the home into them. A new WinRequest (new size) makes the layer build a new generation and reply fresh.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace envshare {

constexpr const char* kSocketName = "framehome-env.sock";                 // Linux: in $XDG_RUNTIME_DIR
constexpr const char* kPipeName = "\\\\.\\pipe\\framehome-env";           // Windows: the layer's named pipe
constexpr const char* kStateMapName = "Local\\framehome-env-state";       // Windows: the shared State mapping (+ a
                                                                          // generation suffix, so a new one is distinct)
constexpr uint32_t kMagic = 0x46484556;   // "FHEV"
constexpr uint32_t kVersion = 6;          // 4: Hello says the images' usage (Frame Home draws straight into them)
                                          // 5: ...and their create flags (subsampled: the compositor smooths foveation)
                                          // 6: the State says how the view was moving with each frame (Motion)
// Four: the compositor reads the newest while Frame Home publishes the one before it finished and renders the next.
constexpr uint32_t kSlots = 4;

// No cross-API GPU lock on the shared textures (as on Linux, where the images carry none): Frame Home publishes a
// slot only once the GPU has finished writing it, and the four slots rotate, so the layer always reads a slot that
// Frame Home finished and isn't touching. A torn read would need the layer to read the very slot being written four
// frames on, which the rotation prevents.
//
// Windows hand-over messages on the pipe (little-endian, fixed size).
struct WinRequest {
    uint32_t magic = kMagic, version = kVersion;
    uint32_t width = 0, height = 0, layers = 2;
    uint32_t format = 0;                  // DXGI_FORMAT the textures should be (87 = R8G8B8A8_UNORM_SRGB)
    uint32_t pid = 0;                     // Frame Home's process id (the layer duplicates the texture handles into it)
    uint32_t generation = 0;              // which set Frame Home is asking to (re)create
    uint64_t state = 0;                   // the shared State's file-mapping handle, duplicated into the layer's process
    uint32_t stateSize = 0;               // sizeof(State) as Frame Home built it (the layer maps that much)
};
struct WinReply {
    uint32_t magic = kMagic, version = kVersion;
    uint32_t ok = 0;                      // 0: the layer couldn't make them (Frame Home keeps waiting)
    uint32_t generation = 0;
    uint64_t texture[kSlots] = {};        // NT handles (duplicated into Frame Home), as integers
    uint8_t deviceLUID[8] = {};           // the compositor's adapter: Frame Home must render on the same one
};

struct Hello {
    uint32_t magic = kMagic, version = kVersion;
    uint32_t width = 0, height = 0, layers = 2;
    uint32_t format = 0;                  // VkFormat of the images (R8G8B8A8_SRGB)
    uint32_t memoryType = 0;              // memory type index the images were allocated from (same GPU)
    uint32_t dedicated = 1;               // allocated as dedicated allocations
    uint64_t memorySize[kSlots] = {};
    uint8_t deviceUUID[16] = {};          // the layer refuses images from another GPU
    uint32_t generation = 0;              // which set of images (a new Hello after a resize is a new generation)
    uint32_t usage = 0;                   // v4: VkImageUsageFlags they were created with (the import must match)
    uint32_t flags = 0;                   // v5: VkImageCreateFlags they were created with (likewise). SUBSAMPLED: the
                                          // foveated pass's own output, read through a subsampled sampler
};
// Older Hellos: the same without the later fields. v3: no `usage` (images made for copying into: TRANSFER_DST |
// SAMPLED); v4: no `flags` (none).
constexpr size_t kHelloV3Size = offsetof(Hello, usage);
constexpr size_t kHelloV4Size = offsetof(Hello, flags);

struct Slot {
    uint64_t frame = 0;                   // frame number published into this slot (0 = never)
    uint32_t generation = 0;              // the image generation it was rendered into (see Hello)
    float viewProj[2][16] = {};           // per eye: projection * view rotation (column-major), tracking space
    float eyePos[2][4] = {};              // per eye: where it was, tracking space (xyz)
};

// v6: how the view was moving when a frame was made, so the layer can carry a frame it shows again (Half rate, or a
// frame that came late) on to where you've got to since. The rig's smooth turn is a rotation about the head, so it's
// carried on exactly; walking moves the eye, which needs depth the layer hasn't got: everything is taken as `depth`
// metres away (what's in front of you), so walls and furniture keep up and only the far distance is a little off.
struct Motion {
    uint64_t targetNs = 0;                // the compositor sample the frame was made for (CLOCK_MONOTONIC; 0: as is)
    float yawRate = 0;                    // the rig turning (smooth turn), rad/s, + = left, about tracking-space up
    float depth = 0;                      // what walking parallax takes everything's distance as (m; 0: no correction)
    float velocity[4] = {};               // the rig walking, tracking space of the frame (m/s, xyz)
};

constexpr uint32_t kStateVersion = 7;     // State::version once Frame Home writes slots under `seq` (v6 wrote kVersion)
struct State {
    uint32_t magic = kMagic, version = kStateVersion;
    std::atomic<uint32_t> latest{0xFFFFFFFFu};   // newest finished slot (0xFFFFFFFF none)
    std::atomic<uint64_t> published{0};          // frame number of `latest`
    std::atomic<uint64_t> publishNs{0};          // CLOCK_MONOTONIC when it was published
    std::atomic<uint64_t> sampledNs{0};          // the layer: when it last drew the home (Frame Home idles if stale)
    std::atomic<uint32_t> fade{1000};            // brightness, 0..1000 (switching homes fades through black)
    std::atomic<uint32_t> show{1};               // 0: Frame Home has nothing to show yet (the layer draws black)
    std::atomic<uint64_t> busyUntilNs{0};        // Frame Home is uploading a home until then: keep the last frame (black)
    Slot slot[kSlots];
    std::atomic<uint32_t> refusedFlags{0};       // the layer: Hello flags it can't import with (Frame Home then sends
                                                 // images without them)
    Motion motion[kSlots];                       // v6: with each slot's frame (written before `latest`, as Slot is)
    // State v7: a sequence count per slot (a seqlock over slot[k] and motion[k]). Odd while Frame Home rewrites them;
    // the layer copies a slot only between two equal even counts, so it never takes half of one frame's pose and half
    // of the next's (a resize rewrites the slot the layer is still showing from the old images). See writeSlot().
    std::atomic<uint32_t> seq[kSlots] = {};
};
// An older Frame Home's State ends before the fields added since (a v5 Hello: before `motion`; a v6 State before
// `seq`). The layer maps only as much as it was sent: reading past the end of the shared memory would crash the
// compositor. The Hello stays v6 (its layout didn't change): `version` in the State says v7, and the memfd's size
// says whether `seq` is there at all, so an old layer with a new Frame Home (and the reverse) still pair up.
constexpr size_t kStateV5Size = offsetof(State, motion);
constexpr size_t kStateV6Size = offsetof(State, seq);

// The layer: how much of the State a Frame Home sent may be mapped (0: less than its Hello's version promises, so
// none), and whether it says how the view moves (v6 Motion). Whether its slots are under sequence counts is known
// once it's mapped: seqlocked().
struct StateLayout { size_t size = 0; bool motion = false; };
inline StateLayout stateLayout(uint32_t helloVersion, uint64_t fileSize) {
    StateLayout l;
    l.motion = helloVersion >= 6;
    const size_t need = l.motion ? kStateV6Size : kStateV5Size;
    if (fileSize >= need) l.size = l.motion && fileSize >= sizeof(State) ? sizeof(State) : need;
    return l;
}
// A State mapped `mapped` bytes long has `seq` and Frame Home writes the slots under it: v7 in size and in what it
// says (a bigger State from something else is still read the v6 way).
inline bool seqlocked(const State* st, size_t mapped) {
    return mapped >= sizeof(State) && st->magic == kMagic && st->version >= kStateVersion;
}

// Word-by-word relaxed atomic copies of the seqlocked slot data (plain memcpy racing the other process is a data
// race; these are the C++ memory model's way to say "may tear, the sequence count catches it").
inline void storeWords(void* dst, const void* src, size_t bytes) {
    uint32_t* d = static_cast<uint32_t*>(dst);
    const uint32_t* s = static_cast<const uint32_t*>(src);
    for (size_t i = 0; i < bytes / 4; ++i) {
#ifdef _MSC_VER
        __iso_volatile_store32(reinterpret_cast<volatile __int32*>(d + i), static_cast<__int32>(s[i]));
#else
        __atomic_store_n(d + i, s[i], __ATOMIC_RELAXED);
#endif
    }
}
inline void loadWords(void* dst, const void* src, size_t bytes) {
    uint32_t* d = static_cast<uint32_t*>(dst);
    const uint32_t* s = static_cast<const uint32_t*>(src);
    for (size_t i = 0; i < bytes / 4; ++i) {
#ifdef _MSC_VER
        d[i] = static_cast<uint32_t>(__iso_volatile_load32(reinterpret_cast<const volatile __int32*>(s + i)));
#else
        d[i] = __atomic_load_n(s + i, __ATOMIC_RELAXED);
#endif
    }
}
static_assert(sizeof(Slot) % 4 == 0 && sizeof(Motion) % 4 == 0, "slot data is copied as 32-bit words");

// Frame Home: rewrites slot k (and its Motion) under its sequence count. Only Frame Home writes, from one thread.
inline void writeSlot(State* st, uint32_t k, const Slot& slot, const Motion& motion) {
    const uint32_t s = st->seq[k].load(std::memory_order_relaxed);
    st->seq[k].store(s | 1u, std::memory_order_relaxed);          // odd: being written
    std::atomic_thread_fence(std::memory_order_release);           // (the odd count is seen before any new word)
    storeWords(&st->slot[k], &slot, sizeof slot);
    storeWords(&st->motion[k], &motion, sizeof motion);
    st->seq[k].store((s | 1u) + 1u, std::memory_order_release);   // even again: whole
}
// Frame Home: only the Motion of slot k (still()).
inline void writeMotion(State* st, uint32_t k, const Motion& motion) {
    Slot cur;
    loadWords(&cur, &st->slot[k], sizeof cur);   // (Frame Home is the only writer: this is what's there)
    writeSlot(st, k, cur, motion);
}

// The layer: a consistent copy of slot k, or false (being rewritten right now, every try). `seqlocked`: the State is
// v7 (has `seq`); an older Frame Home has no count, so two copies that agree are the best that can be had.
inline bool readSlot(const State* st, uint32_t k, bool seqlocked, bool withMotion, Slot& slot, Motion& motion) {
    motion = Motion();
    for (int tries = 0; tries < 8; ++tries) {
        if (seqlocked) {
            const uint32_t s1 = st->seq[k].load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            loadWords(&slot, &st->slot[k], sizeof slot);
            if (withMotion) loadWords(&motion, &st->motion[k], sizeof motion);
            std::atomic_thread_fence(std::memory_order_acquire);   // (the words are read before the second count)
            if (st->seq[k].load(std::memory_order_relaxed) == s1) return true;
        } else {
            Slot a, b;
            Motion ma, mb;
            loadWords(&a, &st->slot[k], sizeof a);
            if (withMotion) loadWords(&ma, &st->motion[k], sizeof ma);
            loadWords(&b, &st->slot[k], sizeof b);
            if (withMotion) loadWords(&mb, &st->motion[k], sizeof mb);
            if (std::memcmp(&a, &b, sizeof a) == 0 && (!withMotion || std::memcmp(&ma, &mb, sizeof ma) == 0)) {
                slot = a;
                motion = ma;
                return true;
            }
        }
    }
    return false;
}

inline uint64_t nowNs() {
#ifdef _WIN32
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

}  // namespace envshare
