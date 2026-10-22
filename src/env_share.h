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
#include <ctime>

namespace envshare {

constexpr const char* kSocketName = "framehome-env.sock";                 // Linux: in $XDG_RUNTIME_DIR
constexpr const char* kPipeName = "\\\\.\\pipe\\framehome-env";           // Windows: the layer's named pipe
constexpr const char* kStateMapName = "Local\\framehome-env-state";       // Windows: the shared State mapping (+ a
                                                                          // generation suffix, so a new one is distinct)
constexpr uint32_t kMagic = 0x46484556;   // "FHEV"
constexpr uint32_t kVersion = 5;          // 4: Hello says the images' usage (Frame Home draws straight into them)
                                          // 5: ...and their create flags (subsampled: the compositor smooths foveation)
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

struct State {
    uint32_t magic = kMagic, version = kVersion;
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
};

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
