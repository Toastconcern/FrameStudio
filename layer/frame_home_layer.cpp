// Frame Home compositor layer: runs inside SteamVR's vrcompositor (a Vulkan implicit layer) and makes Frame Home the
// system environment in Aurora's place.
//
// The compositor draws Aurora itself (no app), which is why Aurora has no dock entry, shows at boot, during game
// loads and behind the Steam menu, keeps the controllers visible and works with passthrough. This layer keeps all of
// that and swaps only the picture: every pipeline built with Aurora's vertex shader gets a twin with env_ps.frag,
// which shows the newest frame Frame Home (frame-home --system) rendered of your home (see src/env_share.h). Aurora's
// other pieces (pipelines whose fragment shader is an Aurora one on another vertex shader) are skipped while it shows.
//
// Safety:
//   active only in a process named "vrcompositor"; everywhere else a pure pass-through
//   no published frame, or one older than half a second (Frame Home stopped or crashed): Aurora as usual
//   ~/frame-home/layer/off: pass-through (checked at start and every second)
//   if the compositor dies twice in a row with the layer swapping (it never got healthy), it writes `off` itself
//   removing ~/.local/share/vulkan/implicit_layer.d/frame_home_layer.json unloads it entirely
#include "../src/env_share.h"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <functional>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define FH_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

// ── process / files ─────────────────────────────────────────────────────────────────────────────

bool isCompositor() {
    static int v = -1;
    if (v < 0) {
        char name[64] = {};
        FILE* f = std::fopen("/proc/self/comm", "r");
        if (f) { if (!std::fgets(name, sizeof name, f)) name[0] = 0; std::fclose(f); }
        v = std::strncmp(name, "vrcompositor", 12) == 0;
    }
    return v == 1;
}

std::string dir() {
    const char* h = std::getenv("HOME");
    return std::string(h && *h ? h : "/home/steamos") + "/frame-home/layer";
}

bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }

std::mutex g_logMutex;
FILE* g_log = nullptr;

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> l(g_logMutex);
    if (!g_log) {
        ::mkdir(dir().c_str(), 0755);
        g_log = std::fopen((dir() + "/layer.log").c_str(), "a");
        if (!g_log) return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::vector<char> readFile(const std::string& p) {
    std::vector<char> buf;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return buf;
    char tmp[65536];
    size_t r;
    while ((r = std::fread(tmp, 1, sizeof tmp, f)) > 0) buf.insert(buf.end(), tmp, tmp + r);
    std::fclose(f);
    return buf;
}

// ── SteamVR shader identification (content hash of /opt/steamvr/resources/shaders/vulkan/*.spv) ──

uint64_t fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

std::unordered_map<uint64_t, std::string>& knownShaders() {
    static std::unordered_map<uint64_t, std::string> m;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* d = "/opt/steamvr/resources/shaders/vulkan";
        DIR* dp = ::opendir(d);
        if (!dp) return;
        while (dirent* e = ::readdir(dp)) {
            std::string n = e->d_name;
            if (n.size() < 5 || n.compare(n.size() - 4, 4, ".spv") != 0) continue;
            std::vector<char> buf = readFile(std::string(d) + "/" + n);
            if (!buf.empty()) m[fnv(buf.data(), buf.size())] = n.substr(0, n.size() - 4);
        }
        ::closedir(dp);
    });
    return m;
}

// ── dispatch ────────────────────────────────────────────────────────────────────────────────────

void* key(const void* dispatchable) { return *static_cast<void* const*>(dispatchable); }

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
};

#define FH_DEVICE_FUNCS(X)                                                                                                  \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines)              \
    X(DestroyPipeline) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorSetLayout)                        \
    X(CreateDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateSampler) X(CreateImage)               \
    X(DestroyImage) X(CreateImageView) X(DestroyImageView) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory)  \
    X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory)   \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDraw) X(CmdDrawIndexed) X(QueueSubmit) X(QueueSubmit2)                \
    X(QueueSubmit2KHR) X(QueueWaitIdle) X(DeviceWaitIdle) X(DestroyDescriptorPool) X(DestroyDescriptorSetLayout) X(DestroySampler) \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(ResetCommandBuffer) X(ResetCommandPool)        \
    X(DestroyCommandPool) X(CreateCommandPool) X(EndCommandBuffer) X(CmdPipelineBarrier) X(GetDeviceQueue2)

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;   // our own command buffers get the loader's dispatch with it
    bool samplesSubsampled = false;      // VK_EXT_fragment_density_map is on: Frame Home's foveated frames read as made
#define FH_DECL(n) PFN_vk##n n = nullptr;
    FH_DEVICE_FUNCS(FH_DECL)
#undef FH_DECL
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, InstanceData*> g_physToInstance;   // physical device (dispatch key) -> instance data
std::unordered_map<void*, DeviceData> g_devices;
std::unordered_map<VkQueue, uint32_t> g_queueFamily;          // (g_mutex) each queue the compositor got, its family

DeviceData* dev(const void* dispatchable) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_devices.find(key(dispatchable));
    return it == g_devices.end() ? nullptr : &it->second;
}

// ── state: Aurora pipelines and their twins ─────────────────────────────────────────────────────

std::unordered_map<VkShaderModule, std::string> g_modules;
struct LayoutInfo { std::vector<VkDescriptorSetLayout> sets; std::vector<VkPushConstantRange> push; };
std::unordered_map<VkPipelineLayout, LayoutInfo> g_layouts;
// Each Aurora pipeline's twin comes in two kinds, for the two kinds of frame Frame Home may send: plain images, and
// subsampled ones (its foveated pass's own output, read through a subsampled sampler that smooths the coarse regions).
// A subsampled sampler must be fixed in the set layout, so each kind has its own set layout and pipeline.
struct TwinPipeline { VkPipeline pipeline = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; };
struct Twin { TwinPipeline plain, subsampled; };
std::unordered_map<VkPipeline, Twin> g_twins;             // Aurora pipeline -> ours
std::unordered_set<VkPipeline> g_skip;                     // other Aurora pieces: not drawn while the home shows
std::unordered_set<VkPipeline> g_camera;                   // passthrough (the cameras' reprojection)
std::atomic<uint64_t> g_cameraNs{0};                       // when passthrough was last drawn
// Passthrough drawn in the last 150 ms. Signed: another thread's command buffer may stamp g_cameraNs between our
// clock read and its load, and that is "just now", not 584 years ago (unsigned, it flashed the home over the cameras).
bool cameraRecent() {
    const uint64_t c = g_cameraNs.load();
    return c && static_cast<int64_t>(envshare::nowNs() - c) < 150000000ll;
}
// Aurora's stars are a render model (rendermodels/stars), drawn with the generic model pipeline the controllers use
// too: they're told apart by their index count, read from the model itself.
uint32_t g_starsIndices = 0;

uint32_t starsIndexCount() {
    std::vector<char> obj = readFile("/opt/steamvr/resources/rendermodels/stars/stars.obj");
    uint32_t n = 0;
    for (size_t i = 0; i + 1 < obj.size(); ++i) {
        if ((i == 0 || obj[i - 1] == '\n') && obj[i] == 'f' && obj[i + 1] == ' ') {
            int corners = 0;
            for (size_t j = i + 1; j < obj.size() && obj[j] != '\n'; ++j)
                if (obj[j] != ' ' && (obj[j - 1] == ' ')) ++corners;
            if (corners >= 3) n += static_cast<uint32_t>(corners - 2) * 3;
        }
    }
    return n;
}
std::unordered_map<VkCommandBuffer, VkPipeline> g_bound;   // graphics pipeline bound per command buffer
std::unordered_map<VkPipeline, std::string> g_names;       // vertex + fragment shader names (trace)
std::atomic<int> g_traceDraws{0};                           // ~/frame-home/layer/trace: log this many draws
std::atomic<bool> g_off{false};
std::atomic<bool> g_startedOff{false};   // off as this compositor started: none of our pipelines exist

// Our objects on the compositor's device.
//
// Each Hello's images are a generation (Gen): the images, their views, and descriptor sets of their own, written
// once when the generation is made and never again. A descriptor set bound in a recorded command buffer must not be
// rewritten or freed until that command buffer is done with (re-begun, reset or freed: the spec forbids any of those
// while it's still pending on the GPU, so by then the GPU is done with it too). So a new generation never touches the
// old one's sets; the old one is retired, and destroyed once no command buffer that drew with it is left.
//
// The images are external memory written on Frame Home's queue: each frame Frame Home finishes is released to
// VK_QUEUE_FAMILY_EXTERNAL (src/hsl_renderer.cpp), and the compositor's queue must acquire it before sampling it, or
// what it reads is undefined (on some GPUs, caches or compression state another process left behind). The acquire is
// one pipeline barrier per slot, recorded once per generation and queue family (acquire[]), and submitted just ahead of
// the first compositor command buffer that draws a frame newer than the slot last acquired (acquiredFrame[]): once per
// Frame Home frame, never per draw. GENERAL to GENERAL on both sides (no layout transition), so an older Frame Home
// that doesn't release pairs with this layer as before, and an older layer that doesn't acquire with a newer Frame Home.
struct Use { uint32_t slots = 0; uint64_t frame[envshare::kSlots] = {}; };   // a command buffer's draws: which slots, which frames
struct Gen {
    uint32_t generation = 0;
    uint32_t layers = 1;
    bool subsampled = false;                              // drawn with the subsampled twins (and their set layout)
    VkImage image[envshare::kSlots] = {};
    VkDeviceMemory mem[envshare::kSlots] = {};
    VkImageView view[envshare::kSlots] = {};
    VkDescriptorPool pool = VK_NULL_HANDLE;               // this generation's own (its sets go with it)
    VkDescriptorSet set[envshare::kSlots] = {};
    std::unordered_map<VkCommandBuffer, Use> users;       // recorded with these sets and not yet re-begun/reset/freed
    VkCommandBuffer acquire[envshare::kSlots] = {};       // the slot's acquire barrier (for acquireFamily[k])
    uint32_t acquireFamily[envshare::kSlots] = {};
    uint64_t acquiredFrame[envshare::kSlots] = {};        // the Frame Home frame last acquired in the slot (0: none)
};
constexpr size_t kMaxRetired = 2;   // a compositor that never re-records a command buffer can't pile generations up

struct Ours {
    DeviceData* d = nullptr;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayoutSubsampled = VK_NULL_HANDLE;   // (null: this device can't sample subsampled images)
    VkSampler sampler = VK_NULL_HANDLE, subsampledSampler = VK_NULL_HANDLE;
    // Each slot's uniforms are a ring of kUboRing blocks, one per compositor frame (a dynamic offset picks it): what a
    // frame is drawn with changes every compositor frame (the turn carried on, the fade), and the block the last one is
    // still drawing with on the GPU mustn't change under it. Shared by every generation (only the offset is bound).
    struct Ubo { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr; };
    Ubo ubo[envshare::kSlots];
    Gen* cur = nullptr;                                   // the generation drawn now
    std::vector<Gen*> retired;                            // older ones, until no command buffer uses them
    uint32_t generation = 0;      // of the imported images
    int lastGood = -1;            // newest slot of this generation drawn (shown while Frame Home resizes)
    // What each slot was rendered with, copied (consistently: env_share.h readSlot) when the layer last saw it newest
    // in this generation. Drawn from this copy, never from the shared slot: after a resize Frame Home rewrites the
    // slot the layer is still showing from the old images with the new generation's pose.
    envshare::Slot shown[envshare::kSlots];
    envshare::Motion shownMotion[envshare::kSlots];
    envshare::State* state = nullptr;
    size_t stateSize = 0;
    bool hasMotion = false;       // the State says how the view moves (a v6 Frame Home; Motion)
    bool seqlocked = false;       // the State's slots are written under sequence counts (a v7 State)
    uint64_t healthyFrames = 0;
    bool baseReady = false, baseFailed = false;   // ensureBase(): all made / gave up (the rest of the session is Aurora's)
    std::unordered_map<uint32_t, VkCommandPool> acquirePools;   // per queue family: the generations' acquire[] command buffers
    uint64_t acquires = 0;                                      // frames acquired (logged as the device goes)
} g_ours;
std::mutex g_oursMutex;
std::atomic<bool> g_imported{false};   // g_ours.cur is set (read unlocked by the draw hooks as a quick "nothing to do")
int g_latchSlot = -1;                 // frameSlot(): this compositor frame's pick, when it was made, and whether the
uint64_t g_latchNs = 0;               // home was drawn with it (sampledNs stamped, its uniforms filled in)
bool g_latchStamped = false;
constexpr uint32_t kUboRing = 8, kUboStride = 256;   // uniform blocks per slot (compositor frames in flight are fewer)
uint32_t g_latchRing = 0;             // this compositor frame's block
std::unordered_map<VkCommandBuffer, VkCommandPool> g_cbPool;   // (g_oursMutex) which pool each command buffer is from

// A Hello from Frame Home waiting to be imported on the compositor's thread.
struct Pending { bool have = false; envshare::Hello hello; int fds[envshare::kSlots] = {-1, -1, -1, -1}; int stateFd = -1; } g_pending;
std::mutex g_pendingMutex;
std::atomic<bool> g_connected{false};

uint32_t memoryTypeFor(DeviceData* d, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    d->inst->GetPhysicalDeviceMemoryProperties(d->physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// A descriptor pool with room for one generation's sets of the given kind, and the sets. A subsampled combined image
// sampler takes half again a plain one's room, and Turnip sizes a pool for that only when the device enables the
// fragmentDensityMap feature (left off: it would cost the compositor elsewhere). So the count asks for the room
// (2 * kSlots plain-sized ones covers kSlots subsampled).
bool makeSets(DeviceData* d, VkDescriptorSetLayout layout, VkDescriptorPool& pool, VkDescriptorSet* sets) {
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * envshare::kSlots}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, envshare::kSlots}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = envshare::kSlots;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pool = VK_NULL_HANDLE;
    if (d->CreateDescriptorPool(d->device, &pi, nullptr, &pool) != VK_SUCCESS) { pool = VK_NULL_HANDLE; return false; }
    VkDescriptorSetLayout layouts[envshare::kSlots];
    for (auto& l : layouts) l = layout;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = envshare::kSlots;
    dai.pSetLayouts = layouts;
    if (d->AllocateDescriptorSets(d->device, &dai, sets) != VK_SUCCESS) {
        d->DestroyDescriptorPool(d->device, pool, nullptr);
        pool = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < envshare::kSlots; ++i) sets[i] = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// (g_oursMutex held, once it has acquire command buffers) None of those is pending by now: each was submitted ahead
// of a command buffer among the generation's users, and those are done.
void destroyGen(DeviceData* d, Gen* g) {
    if (!g) return;
    for (uint32_t i = 0; i < envshare::kSlots; ++i)
        if (g->acquire[i]) {
            auto p = g_ours.acquirePools.find(g->acquireFamily[i]);
            if (p != g_ours.acquirePools.end()) d->FreeCommandBuffers(d->device, p->second, 1, &g->acquire[i]);
        }
    if (g->pool) d->DestroyDescriptorPool(d->device, g->pool, nullptr);   // (its sets with it)
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        if (g->view[i]) d->DestroyImageView(d->device, g->view[i], nullptr);
        if (g->image[i]) d->DestroyImage(d->device, g->image[i], nullptr);
        if (g->mem[i]) d->FreeMemory(d->device, g->mem[i], nullptr);
    }
    delete g;
}

// (g_oursMutex held) Destroys the retired generations no command buffer uses any more. More than kMaxRetired still
// in use (command buffers recorded once and never re-recorded): the oldest goes after the device idles, as the layer
// always did before generations had their own sets.
void collectRetired() {
    DeviceData* d = g_ours.d;
    if (!d) return;
    auto& r = g_ours.retired;
    for (size_t i = 0; i < r.size();) {
        if (r[i]->users.empty()) { destroyGen(d, r[i]); r.erase(r.begin() + static_cast<std::ptrdiff_t>(i)); }
        else ++i;
    }
    if (r.size() > kMaxRetired) {
        logf("generation %u: %zu command buffers still hold it; waiting for the GPU to free it", r.front()->generation, r.front()->users.size());
        d->DeviceWaitIdle(d->device);
        destroyGen(d, r.front());
        r.erase(r.begin());
    }
}

// A command buffer re-begun, reset or freed (none of which may happen while it's pending): no generation is held by it.
void forgetCommandBuffer(VkCommandBuffer cb) {
    std::lock_guard<std::mutex> ol(g_oursMutex);
    if (g_ours.cur) g_ours.cur->users.erase(cb);
    for (Gen* g : g_ours.retired) g->users.erase(cb);
}
void forgetPool(VkCommandPool pool, bool destroyed) {
    std::lock_guard<std::mutex> ol(g_oursMutex);
    auto drop = [&](Gen* g) { for (auto it = g->users.begin(); it != g->users.end();) { auto p = g_cbPool.find(it->first); if (p != g_cbPool.end() && p->second == pool) it = g->users.erase(it); else ++it; } };
    if (g_ours.cur) drop(g_ours.cur);
    for (Gen* g : g_ours.retired) drop(g);
    if (destroyed) for (auto it = g_cbPool.begin(); it != g_cbPool.end();) it = it->second == pool ? g_cbPool.erase(it) : std::next(it);
}

// (g_oursMutex held) The current generation stops being drawn: it waits among the retired for its command buffers.
void retireCurrent() {
    if (g_ours.cur) g_ours.retired.push_back(g_ours.cur);
    g_ours.cur = nullptr;
    g_imported = false;
    if (g_ours.state) ::munmap(g_ours.state, g_ours.stateSize);
    g_ours.state = nullptr;
    g_ours.lastGood = -1;
}

// One-time objects (shader, set layouts, sampler, per-slot UBOs) on the compositor's device.
bool ensureBase(DeviceData* d) {
    if (g_ours.baseReady) return g_ours.d == d;               // made on the compositor's device: only that one draws
    if (g_ours.baseFailed) return false;
    g_ours.d = d;
    g_ours.baseFailed = true;                                  // until everything below is made
    std::vector<char> spv = readFile(dir() + "/env_ps.spv");
    if (spv.empty() || spv.size() % 4) { logf("no env_ps.spv: Aurora stays"); return false; }
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spv.size();
    smi.pCode = reinterpret_cast<const uint32_t*>(spv.data());
    if (d->CreateShaderModule(d->device, &smi, nullptr, &g_ours.fragment) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (d->CreateDescriptorSetLayout(d->device, &li, nullptr, &g_ours.setLayout) != VK_SUCCESS) return false;
    if (d->samplesSubsampled) {   // the subsampled kind: the same, its sampler fixed in the layout (the spec requires it)
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (d->CreateSampler(d->device, &si, nullptr, &g_ours.subsampledSampler) == VK_SUCCESS) {
            VkDescriptorSetLayoutBinding sb[2] = {b[0], b[1]};
            sb[0].pImmutableSamplers = &g_ours.subsampledSampler;
            VkDescriptorSetLayoutCreateInfo sli = li;
            sli.pBindings = sb;
            if (d->CreateDescriptorSetLayout(d->device, &sli, nullptr, &g_ours.setLayoutSubsampled) != VK_SUCCESS)
                g_ours.setLayoutSubsampled = VK_NULL_HANDLE;
        }
        // Can a generation's worth of that kind really be allocated here? (no: no twins of that kind are made)
        VkDescriptorPool probe = VK_NULL_HANDLE;
        VkDescriptorSet probeSets[envshare::kSlots];
        if (g_ours.setLayoutSubsampled && !makeSets(d, g_ours.setLayoutSubsampled, probe, probeSets)) {
            d->DestroyDescriptorSetLayout(d->device, g_ours.setLayoutSubsampled, nullptr);
            g_ours.setLayoutSubsampled = VK_NULL_HANDLE;
            logf("subsampled frames: no descriptor set (plain frames only)");
        }
        if (probe) d->DestroyDescriptorPool(d->device, probe, nullptr);
        logf(g_ours.setLayoutSubsampled ? "subsampled frames: readable" : "subsampled frames: no sampler/layout (plain frames only)");
    }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d->CreateSampler(d->device, &si, nullptr, &g_ours.sampler) != VK_SUCCESS) return false;
    for (auto& u : g_ours.ubo) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kUboRing * kUboStride;   // (256: no device needs uniform offsets aligned to more)
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (d->CreateBuffer(d->device, &bi, nullptr, &u.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        d->GetBufferMemoryRequirements(d->device, u.buffer, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryTypeFor(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->device, &ai, nullptr, &u.mem) != VK_SUCCESS) return false;
        d->BindBufferMemory(d->device, u.buffer, u.mem, 0);
        if (d->MapMemory(d->device, u.mem, 0, VK_WHOLE_SIZE, 0, &u.mapped) != VK_SUCCESS) return false;
    }
    g_ours.baseFailed = false;
    g_ours.baseReady = true;
    logf("ready to replace Aurora");
    return true;
}

// Imports a Hello's images (compositor thread, inside QueueSubmit) as a new generation beside the one showing.
void armHealth();

void importPending(DeviceData* d, VkQueue) {
    Pending p;
    for (int& fd : p.fds) fd = -1;
    {
        std::lock_guard<std::mutex> l(g_pendingMutex);
        if (!g_pending.have) return;
        p = g_pending;
        g_pending = Pending();
    }
    auto closeAll = [&] { for (int fd : p.fds) if (fd >= 0) ::close(fd); if (p.stateFd >= 0) ::close(p.stateFd); };
    if (!ensureBase(d)) { closeAll(); return; }                // (another device of the compositor's: not ours)
    // Same GPU?
    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    pp.pNext = &idp;
    d->inst->GetPhysicalDeviceProperties2(d->physical, &pp);
    if (std::memcmp(idp.deviceUUID, p.hello.deviceUUID, 16) != 0) { logf("frames from another GPU: ignored"); closeAll(); return; }
    // The new frames are made beside the ones showing, which stay until all of them import: a failed import keeps
    // the last ones.
    // How much State there is: a v5 Frame Home's ends before Motion, a v6 one's before the sequence counts (v7).
    struct stat sst = {};
    const bool statOk = ::fstat(p.stateFd, &sst) == 0 && sst.st_size >= 0;
    const envshare::StateLayout layout = envshare::stateLayout(p.hello.version, statOk ? static_cast<uint64_t>(sst.st_size) : 0);
    const bool hasMotion = layout.motion;
    const size_t stateSize = layout.size;
    // The State must really be that large: touching a mapping past the end of its file is SIGBUS, which would take
    // the compositor down. And the images must be ones Vulkan can make (a memory type bit, layers to draw).
    if (!statOk || !stateSize || p.hello.memoryType >= 32 || p.hello.layers < 1 ||
        p.hello.layers > 2 || !p.hello.width || !p.hello.height) {
        logf("bad hello from Frame Home (state %lld bytes, memory type %u, %u layers, %ux%u): ignored", static_cast<long long>(sst.st_size),
             p.hello.memoryType, p.hello.layers, p.hello.width, p.hello.height);
        closeAll();
        return;
    }
    void* st = ::mmap(nullptr, stateSize, PROT_READ | PROT_WRITE, MAP_SHARED, p.stateFd, 0);
    ::close(p.stateFd);
    p.stateFd = -1;
    if (st == MAP_FAILED) { logf("cannot map the shared state"); closeAll(); return; }
    // Sequence counts only when the State is v7 throughout (an older Frame Home's slots are copied twice and compared).
    const bool seqlocked = envshare::seqlocked(static_cast<envshare::State*>(st), stateSize);
    // Subsampled frames need this device to sample them (the extension, the sampler and the twins made for them):
    // otherwise Frame Home is told, and sends plain ones.
    const bool subsampled = (p.hello.flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) != 0;
    if (p.hello.flags & ~static_cast<uint32_t>(VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) || (subsampled && !g_ours.setLayoutSubsampled)) {
        static_cast<envshare::State*>(st)->refusedFlags.store(p.hello.flags);
        logf("frames with create flags 0x%x: can't read them here (Frame Home will send plain ones)", p.hello.flags);
        ::munmap(st, stateSize);
        closeAll();
        return;
    }
    Gen* g = new Gen();
    g->generation = p.hello.generation;
    g->layers = p.hello.layers;
    g->subsampled = subsampled;
    auto dropMade = [&] { destroyGen(d, g); ::munmap(st, stateSize); };
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &ext;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(p.hello.format);
        ci.extent = {p.hello.width, p.hello.height, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = p.hello.layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = p.hello.usage;                     // exactly as Frame Home created them (the import must match)
        ci.flags = p.hello.flags;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ok = d->CreateImage(d->device, &ci, nullptr, &g->image[i]) == VK_SUCCESS;
        if (!ok) g->image[i] = VK_NULL_HANDLE;
        if (ok) {
            VkMemoryRequirements req;
            d->GetImageMemoryRequirements(d->device, g->image[i], &req);
            VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            imp.fd = p.fds[i];
            VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            ded.image = g->image[i];
            if (p.hello.dedicated) imp.pNext = &ded;
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.pNext = &imp;
            ai.allocationSize = p.hello.memorySize[i];
            ai.memoryTypeIndex = p.hello.memoryType;
            ok = (req.memoryTypeBits & (1u << p.hello.memoryType)) && req.size <= p.hello.memorySize[i] &&
                 d->AllocateMemory(d->device, &ai, nullptr, &g->mem[i]) == VK_SUCCESS;
            if (ok) p.fds[i] = -1;   // the driver owns it now
            else g->mem[i] = VK_NULL_HANDLE;
            ok = ok && d->BindImageMemory(d->device, g->image[i], g->mem[i], 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = g->image[i];
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vi.format = ci.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, p.hello.layers};
            ok = d->CreateImageView(d->device, &vi, nullptr, &g->view[i]) == VK_SUCCESS;
            if (!ok) g->view[i] = VK_NULL_HANDLE;
        }
        if (!ok) { logf("importing frame slot %u failed: keeping the last frames", i); dropMade(); closeAll(); return; }
    }
    closeAll();
    // This generation's own sets, written now, before any command buffer can bind them (and never written again).
    if (!makeSets(d, subsampled ? g_ours.setLayoutSubsampled : g_ours.setLayout, g->pool, g->set)) {
        logf("no descriptor sets for the new frames: keeping the last frames");
        dropMade();
        return;
    }
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        VkDescriptorImageInfo ii = {g_ours.sampler, g->view[i], VK_IMAGE_LAYOUT_GENERAL};   // (the subsampled kind's sampler is in its layout)
        VkDescriptorBufferInfo bi = {g_ours.ubo[i].buffer, 0, kUboStride};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = g->set[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
        w[1].dstSet = g->set[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[1].pBufferInfo = &bi;
        d->UpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }
    std::lock_guard<std::mutex> ol(g_oursMutex);
    // Swap: the old generation stays alive (images, views, sets) until the command buffers drawn with it are done.
    retireCurrent();
    g_ours.cur = g;
    g_ours.state = static_cast<envshare::State*>(st);
    g_ours.stateSize = stateSize;
    g_ours.hasMotion = hasMotion;
    g_ours.seqlocked = seqlocked;
    g_ours.generation = p.hello.generation;
    g_ours.lastGood = -1;
    for (auto& s : g_ours.shown) s = envshare::Slot();
    for (auto& m : g_ours.shownMotion) m = envshare::Motion();
    g_latchNs = 0;
    g_imported = true;
    collectRetired();
    logf("imported Frame Home's frames: %ux%u, usage 0x%x, flags 0x%x (generation %u%s)", p.hello.width, p.hello.height, p.hello.usage,
         p.hello.flags, p.hello.generation, seqlocked ? "" : ", an older Frame Home: slots without sequence counts");
    armHealth();
    logf("Frame Home connected: %ux%u frames, %u slots", p.hello.width, p.hello.height, envshare::kSlots);
}

// The slot to draw now, or -1 for Aurora. While Frame Home is connected the home stays up: a late frame, a home
// loading or new images after a resize keep the last good frame on screen (Aurora flashing in looked broken).
// Only Frame Home gone, or frozen for 5 s (hung), brings Aurora back.
// (written by the socket thread, read by the compositor's: steady_clock ticks, atomic)
std::atomic<std::chrono::steady_clock::rep> g_disconnectedAt{0};
int liveSlot() {   // (g_oursMutex held)
    if (g_off || !g_ours.cur || !g_ours.state) return -1;
    if (!g_connected)   // Frame Home restarting (a setting that rebuilds the renderer): hold its last frame a moment
        return std::chrono::steady_clock::now() - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(g_disconnectedAt.load())) <
                       std::chrono::seconds(3) ? g_ours.lastGood : -1;
    envshare::State* s = g_ours.state;
    const uint64_t now = envshare::nowNs();
    // (signed: a frame published between reading the clock and reading publishNs is newer than `now`, not 584 years old)
    if (static_cast<int64_t>(now - s->publishNs.load()) > 5000000000ll && now > s->busyUntilNs.load()) return -1;
    const uint32_t k = s->latest.load(std::memory_order_acquire);
    // The newest slot, if it's of the images we have: copied whole (a slot being rewritten keeps the last copy).
    envshare::Slot sl;
    envshare::Motion m;
    if (k < envshare::kSlots && envshare::readSlot(s, k, g_ours.seqlocked, g_ours.hasMotion, sl, m) && sl.frame &&
        sl.generation == g_ours.generation) {
        g_ours.shown[k] = sl;
        g_ours.shownMotion[k] = m;
        g_ours.lastGood = static_cast<int>(k);
    }
    return g_ours.lastGood;
}

// The slot for this compositor frame: picked at its first draw of the home and kept for the rest of the frame's draws,
// so both eyes and every Aurora piece show the same Frame Home frame even when a new one is published mid-recording.
// The pick is when the compositor takes the home: sampledNs (Frame Home times its frames to finish just before it).
constexpr uint64_t kLatchNs = 4000000ull;   // a compositor frame's draws are recorded well within this
int frameSlot() {
    const uint64_t now = envshare::nowNs();
    if (g_latchNs && now - g_latchNs < kLatchNs) return g_latchSlot;
    g_latchSlot = liveSlot();
    g_latchNs = now;
    g_latchStamped = false;
    g_latchRing = (g_latchRing + 1) % kUboRing;
    return g_latchSlot;
}

// Slot k's uniforms for this compositor frame (block `ring`): the frame's eye rotations, the brightness now, and how
// far to carry the frame on: by the rig's turn (exactly) and its walking (roughly) since the sample it was made for.
// Frame Home late or stopped: carried on 40 ms at most, so a frame left behind never keeps turning.
// From the layer's own copy of the slot (liveSlot), which matches the images drawn.
void fillUbo(int k, uint32_t ring) {
    envshare::State* s = g_ours.state;
    const envshare::Slot& src = g_ours.shown[k];
    float data[16 * 2 + 4 * 2 + 4 + 4 + 4] = {};
    std::memcpy(data, src.viewProj, sizeof src.viewProj);
    std::memcpy(data + 32, src.eyePos, sizeof src.eyePos);
    data[40] = s->fade.load() / 1000.0f;
    data[41] = s->show.load() ? 1.0f : 0.0f;
    float turn = 0, depth = 0, dt = 0;
    const envshare::Motion m = g_ours.shownMotion[k];
    if (m.targetNs) {
        dt = std::max(-0.02f, std::min(0.04f, static_cast<float>(static_cast<int64_t>(g_latchNs - m.targetNs) / 1e9)));
        turn = m.yawRate * dt;
        depth = m.depth;
    }
    data[44] = std::cos(turn);
    data[45] = std::sin(turn);
    for (int c = 0; c < 3; ++c) data[48 + c] = depth > 0 ? m.velocity[c] * dt : 0.0f;
    // The walk carried on stays well short of the depth it's taken against (a dash is metres a second): the shader
    // looks along dir * depth + walk, which turns sideways, then backwards, as the walk nears the depth.
    const float walked = std::sqrt(data[48] * data[48] + data[49] * data[49] + data[50] * data[50]);
    if (!(walked <= 0.5f * depth)) for (int c = 0; c < 3; ++c) data[48 + c] = walked > 0 && depth > 0 ? data[48 + c] * (0.5f * depth / walked) : 0.0f;
    if (!std::isfinite(data[44]) || !std::isfinite(data[45])) { data[44] = 1; data[45] = 0; }   // (a NaN turn rate: no turn)
    data[51] = depth;
    std::memcpy(static_cast<char*>(g_ours.ubo[k].mapped) + ring * kUboStride, data, sizeof data);
}

// ── health: off switch and crash guard ──────────────────────────────────────────────────────────

void startHealth() {
    if (exists(dir() + "/off")) { g_off = g_startedOff = true; logf("off: Aurora as usual (delete %s/off and restart SteamVR to turn Frame Home back on)", dir().c_str()); return; }
    // `starting` left over = the last compositor died after the layer took Frame Home's frames in (armHealth) and
    // before it got healthy showing them. A compositor that never got that far, or closed cleanly, doesn't count.
    int crashes = 0;
    if (exists(dir() + "/starting")) {
        std::vector<char> c = readFile(dir() + "/crashes");
        crashes = c.empty() ? 1 : std::atoi(std::string(c.begin(), c.end()).c_str()) + 1;
        if (FILE* f = std::fopen((dir() + "/crashes").c_str(), "w")) { std::fprintf(f, "%d\n", crashes); std::fclose(f); }
    }
    if (crashes >= 2) {
        if (FILE* f = std::fopen((dir() + "/off").c_str(), "w")) std::fclose(f);
        ::unlink((dir() + "/starting").c_str());
        g_off = g_startedOff = true;
        logf("the compositor stopped twice before getting healthy: turned myself off (Aurora as usual)");
        return;
    }
}

std::atomic<bool> g_armed{false};
void armHealth() {
    if (g_armed.exchange(true)) return;
    if (FILE* f = std::fopen((dir() + "/starting").c_str(), "w")) std::fclose(f);
}

void markHealthy() {
    ::unlink((dir() + "/starting").c_str());
    ::unlink((dir() + "/crashes").c_str());
    logf("healthy: showing Frame Home for a while without trouble");
}

// ── Frame Home's connection (a thread of ours inside the compositor) ────────────────────────────

void serve() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(rt && *rt ? rt : "/run/user/1000") + "/" + envshare::kSocketName;
    int srv = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) return;
    sockaddr_un a = {};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
    ::unlink(path.c_str());
    if (::bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(srv, 2) != 0) { logf("cannot listen on %s", path.c_str()); ::close(srv); return; }
    logf("waiting for Frame Home on %s", path.c_str());
    for (;;) {
        int c = ::accept4(srv, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        g_connected = true;
        for (;;) {
            envshare::Hello h;
            iovec iov = {&h, sizeof h};
            char ctrl[CMSG_SPACE(sizeof(int) * (envshare::kSlots + 1))] = {};
            msghdr m = {};
            m.msg_iov = &iov;
            m.msg_iovlen = 1;
            m.msg_control = ctrl;
            m.msg_controllen = sizeof ctrl;
            const ssize_t n = ::recvmsg(c, &m, MSG_CMSG_CLOEXEC);
            if (n <= 0) break;                                   // Frame Home gone: Aurora until it's back
            std::vector<int> fds;
            for (cmsghdr* cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                    const size_t cnt = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                    const int* p = reinterpret_cast<const int*>(CMSG_DATA(cm));
                    fds.insert(fds.end(), p, p + cnt);           // (all of them: each is closed below if unused)
                }
            // v6: the State carries Motion (the Hello is v5's); v5: the images' create flags ride along too; v4: their
            // usage (Frame Home draws straight into them), no flags; v3 (an older Frame Home): made for copying into.
            const bool v5 = n == static_cast<ssize_t>(sizeof h) && (h.version == envshare::kVersion || h.version == 5);
            const bool v4 = n == static_cast<ssize_t>(envshare::kHelloV4Size) && h.version == 4;
            const bool v3 = n == static_cast<ssize_t>(envshare::kHelloV3Size) && h.version == 3;
            if (v3) h.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (v3 || v4) h.flags = 0;
            if ((!v5 && !v4 && !v3) || h.magic != envshare::kMagic || fds.size() != envshare::kSlots + 1) {
                logf("bad hello from Frame Home (%zd bytes, %zu fds)", n, fds.size());
                for (int fd : fds) ::close(fd);
                continue;
            }
            std::lock_guard<std::mutex> l(g_pendingMutex);
            for (int fd : g_pending.fds) if (fd >= 0) ::close(fd);
            if (g_pending.stateFd >= 0) ::close(g_pending.stateFd);
            g_pending.have = true;
            g_pending.hello = h;
            for (uint32_t i = 0; i < envshare::kSlots; ++i) g_pending.fds[i] = fds[i];
            g_pending.stateFd = fds[envshare::kSlots];
        }
        g_disconnectedAt = std::chrono::steady_clock::now().time_since_epoch().count();
        g_connected = false;
        ::close(c);
        logf("Frame Home disconnected: Aurora until it's back");
    }
}

// ── intercepts ──────────────────────────────────────────────────────────────────────────────────

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* out) {
    VkResult r = dev(d)->CreateShaderModule(d, ci, a, out);
    if (r == VK_SUCCESS) {
        auto& known = knownShaders();
        auto it = known.find(fnv(ci->pCode, ci->codeSize));
        std::lock_guard<std::mutex> l(g_mutex);
        g_modules[*out] = it != known.end() ? it->second : std::string();
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* a, VkPipelineLayout* out) {
    VkResult r = dev(d)->CreatePipelineLayout(d, ci, a, out);
    if (r == VK_SUCCESS) {
        LayoutInfo li;
        li.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
        li.push.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
        std::lock_guard<std::mutex> l(g_mutex);
        g_layouts[*out] = li;
    }
    return r;
}

std::string moduleName(VkShaderModule m) {
    auto it = g_modules.find(m);
    return it == g_modules.end() ? std::string() : it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                       const VkAllocationCallbacks* a, VkPipeline* out) {
    DeviceData* dd = dev(d);
    VkResult r = dd->CreateGraphicsPipelines(d, c, n, ci, a, out);
    if (r != VK_SUCCESS || g_off) return r;
    for (uint32_t i = 0; i < n; ++i) {
        std::string vs, fs;
        int fsIndex = -1;
        LayoutInfo li;
        {
            std::lock_guard<std::mutex> l(g_mutex);
            for (uint32_t s = 0; s < ci[i].stageCount; ++s) {
                if (ci[i].pStages[s].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = moduleName(ci[i].pStages[s].module);
                if (ci[i].pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) { fs = moduleName(ci[i].pStages[s].module); fsIndex = static_cast<int>(s); }
            }
            auto it = g_layouts.find(ci[i].layout);
            if (it != g_layouts.end()) li = it->second;
        }
        {
            std::lock_guard<std::mutex> l(g_mutex);
            g_names[out[i]] = vs + " + " + fs;
        }
        if (vs.find("tracked_camera") != std::string::npos) {
            std::lock_guard<std::mutex> l(g_mutex);
            g_camera.insert(out[i]);
        }
        if (vs == "distort_geom_aurora_vs" && fsIndex >= 0) {
            if (li.sets.size() != 1 || !ensureBase(dd)) { logf("Aurora pipeline (%s) with %zu sets: left alone", fs.c_str(), li.sets.size()); continue; }
            // Twin: same everything, our fragment shader, and our set 1 (of the given kind) on their layout.
            auto makeTwin = [&](VkDescriptorSetLayout ourSet) -> TwinPipeline {
                TwinPipeline t;
                std::vector<VkDescriptorSetLayout> sets = li.sets;
                sets.push_back(ourSet);
                VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                lci.setLayoutCount = static_cast<uint32_t>(sets.size());
                lci.pSetLayouts = sets.data();
                lci.pushConstantRangeCount = static_cast<uint32_t>(li.push.size());
                lci.pPushConstantRanges = li.push.data();
                if (dd->CreatePipelineLayout(d, &lci, nullptr, &t.layout) != VK_SUCCESS) return TwinPipeline();
                std::vector<VkPipelineShaderStageCreateInfo> stages(ci[i].pStages, ci[i].pStages + ci[i].stageCount);
                stages[fsIndex].module = g_ours.fragment;
                stages[fsIndex].pName = "main";
                stages[fsIndex].pSpecializationInfo = nullptr;
                VkGraphicsPipelineCreateInfo tci = ci[i];
                tci.stageCount = static_cast<uint32_t>(stages.size());
                tci.pStages = stages.data();
                tci.layout = t.layout;
                tci.flags &= ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
                tci.basePipelineHandle = VK_NULL_HANDLE;
                tci.basePipelineIndex = -1;
                if (dd->CreateGraphicsPipelines(d, c, 1, &tci, nullptr, &t.pipeline) != VK_SUCCESS) {
                    dd->DestroyPipelineLayout(d, t.layout, nullptr);
                    return TwinPipeline();
                }
                return t;
            };
            Twin t;
            t.plain = makeTwin(g_ours.setLayout);
            if (!t.plain.pipeline) { logf("twin for %s failed: left alone", fs.c_str()); continue; }
            if (g_ours.setLayoutSubsampled) t.subsampled = makeTwin(g_ours.setLayoutSubsampled);   // (none: plain frames only)
            std::lock_guard<std::mutex> l(g_mutex);
            g_twins[out[i]] = t;
            logf("Aurora pipeline %s: Frame Home twin ready%s", fs.c_str(), t.subsampled.pipeline ? " (plain and subsampled frames)" : "");
        } else if (fs.find("aurora") != std::string::npos || vs.find("floor") != std::string::npos || vs == "distort_panorama_vs") {
            // Aurora's floor (its lighting and the ring at its edge) and its stars (the motes skybox, drawn as a
            // panorama). Not the "reflect" passes: those draw the menu and dock.
            std::lock_guard<std::mutex> l(g_mutex);
            g_skip.insert(out[i]);
            logf("Aurora piece %s + %s: hidden while Frame Home shows", vs.c_str(), fs.c_str());
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks* a) {
    DeviceData* dd = dev(d);
    Twin t;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_twins.find(p);
        if (it != g_twins.end()) { t = it->second; g_twins.erase(it); }
        g_skip.erase(p);
        g_camera.erase(p);
        g_names.erase(p);   // (else every pipeline the compositor ever made keeps its name string)
    }
    for (const TwinPipeline& k : {t.plain, t.subsampled}) {
        if (k.pipeline) dd->DestroyPipeline(d, k.pipeline, nullptr);
        if (k.layout) dd->DestroyPipelineLayout(d, k.layout, nullptr);
    }
    dd->DestroyPipeline(d, p, a);
}

VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> l(g_mutex);
        g_bound[cb] = p;
    }
    dev(cb)->CmdBindPipeline(cb, bp, p);
}

// What to do with a draw: 0 as is, 1 replace with the twin (with `set`, of the kind `subsampled`), 2 skip. A twin
// drawn holds the generation whose set it binds until the command buffer is re-begun, reset or freed (Gen::users).
int drawAction(VkCommandBuffer cb, Twin& t, VkDescriptorSet& set, bool& subsampled, uint32_t& uboOffset) {
    if (g_off || !g_imported) return 0;
    VkPipeline p;
    bool skip;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_bound.find(cb);
        if (it == g_bound.end()) return 0;
        p = it->second;
        if (g_camera.count(p)) { g_cameraNs.store(envshare::nowNs()); return 0; }
        auto tw = g_twins.find(p);
        if (tw != g_twins.end()) t = tw->second;
        skip = g_skip.count(p) != 0;
    }
    if (!t.plain.pipeline && !skip) return 0;
    std::lock_guard<std::mutex> ol(g_oursMutex);
    const int slot = frameSlot();
    if (slot < 0 || !g_ours.cur) return 0;
    // Passthrough on: the compositor would blend the home half over the cameras. Step aside for the real view.
    if (cameraRecent()) return 2;
    if (!t.plain.pipeline) return 2;
    Gen* g = g_ours.cur;
    if (g->subsampled && !t.subsampled.pipeline) return 0;   // (never imported so: import checks the twins can)
    if (!g_latchStamped) {   // the moment it took the frame
        g_latchStamped = true;
        g_ours.state->sampledNs.store(g_latchNs);
        fillUbo(slot, g_latchRing);
    }
    uboOffset = g_latchRing * kUboStride;
    set = g->set[slot];
    subsampled = g->subsampled;
    Use& u = g->users[cb];
    u.slots |= 1u << slot;
    u.frame[slot] = g_ours.shown[slot].frame;   // (the frame it shows: acquired ahead of this command buffer)
    return 1;
}

void drawTwin(VkCommandBuffer cb, const Twin& t, VkDescriptorSet set, bool subsampled, uint32_t uboOffset, const std::function<void()>& draw) {
    DeviceData* dd = dev(cb);
    VkPipeline original;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        original = g_bound[cb];
    }
    const TwinPipeline& k = subsampled ? t.subsampled : t.plain;
    dd->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k.pipeline);
    dd->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k.layout, 1, 1, &set, 1, &uboOffset);
    draw();
    dd->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, original);
}

void traceDraw(VkCommandBuffer cb, const char* kind, uint32_t n, int action) {
    if (g_traceDraws.load() <= 0) return;
    --g_traceDraws;
    std::string name;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_bound.find(cb);
        if (it != g_bound.end()) { auto nm = g_names.find(it->second); if (nm != g_names.end()) name = nm->second; }
    }
    logf("  %s %u [%s] %s", kind, n, action == 1 ? "HOME" : action == 2 ? "hidden" : "kept", name.c_str());
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    Twin t;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool subsampled = false;
    uint32_t off = 0;
    const int act = drawAction(cb, t, set, subsampled, off);
    traceDraw(cb, "draw", vc, act);
    switch (act) {
        case 1: drawTwin(cb, t, set, subsampled, off, [&] { dev(cb)->CmdDraw(cb, vc, ic, fv, fi); }); return;
        case 2: return;
        default: dev(cb)->CmdDraw(cb, vc, ic, fv, fi);
    }
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi, int32_t vo, uint32_t fin) {
    Twin t;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool subsampled = false;
    uint32_t off = 0;
    int act = drawAction(cb, t, set, subsampled, off);
    if (act == 0 && g_starsIndices && n == g_starsIndices && g_imported) {   // the stars, while the home shows
        std::lock_guard<std::mutex> ol(g_oursMutex);
        if (liveSlot() >= 0 && !cameraRecent()) act = 2;
    }
    traceDraw(cb, "drawIndexed", n, act);
    switch (act) {
        case 1: drawTwin(cb, t, set, subsampled, off, [&] { dev(cb)->CmdDrawIndexed(cb, n, ic, fi, vo, fin); }); return;
        case 2: return;
        default: dev(cb)->CmdDrawIndexed(cb, n, ic, fi, vo, fin);
    }
}

std::chrono::steady_clock::time_point g_lastOffCheck;

void onSubmit(VkQueue q) {
    DeviceData* dd = dev(q);
    if (!dd) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastOffCheck > std::chrono::seconds(1)) {
        g_lastOffCheck = now;
        const bool off = exists(dir() + "/off");
        if (off != g_off) {
            g_off = off;
            // Off since the compositor started: no pipelines of ours were made, so nothing changes until it restarts.
            logf(off ? "turned off: Aurora as usual" : g_startedOff ? "turned back on: shows after SteamVR restarts (off since the compositor started)" : "turned back on");
        }
        if (exists(dir() + "/trace")) { ::unlink((dir() + "/trace").c_str()); logf("=== trace: the next 1500 draws ==="); g_traceDraws = 1500; }
    }
    if (!g_off) importPending(dd, q);
    std::lock_guard<std::mutex> ol(g_oursMutex);   // (liveSlot updates lastGood, as the draw hooks do on their threads)
    if (g_ours.d == dd) collectRetired();          // old generations no command buffer holds any more
    if (g_off) return;
    if (g_ours.cur && liveSlot() >= 0 && ++g_ours.healthyFrames == 900) markHealthy();   // ~10 s showing the home
}

// ── command buffers: when a generation's sets are no longer used ────────────────────────────────

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    VkResult r = dev(d)->AllocateCommandBuffers(d, ai, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i) g_cbPool[out[i]] = ai->commandPool;
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    for (uint32_t i = 0; i < n; ++i) {
        if (!cbs[i]) continue;
        forgetCommandBuffer(cbs[i]);
        {
            std::lock_guard<std::mutex> ol(g_oursMutex);
            g_cbPool.erase(cbs[i]);
        }
        std::lock_guard<std::mutex> l(g_mutex);
        g_bound.erase(cbs[i]);   // (a handle the driver hands out again is a new command buffer)
    }
    dev(d)->FreeCommandBuffers(d, pool, n, cbs);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    forgetCommandBuffer(cb);   // (begin resets a recorded one: what it drew before is gone)
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_bound.erase(cb);
    }
    return dev(cb)->BeginCommandBuffer(cb, bi);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    forgetCommandBuffer(cb);
    return dev(cb)->ResetCommandBuffer(cb, f);
}

VKAPI_ATTR VkResult VKAPI_CALL ResetCommandPool(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags f) {
    forgetPool(pool, false);
    return dev(d)->ResetCommandPool(d, pool, f);
}

VKAPI_ATTR void VKAPI_CALL DestroyCommandPool(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks* a) {
    forgetPool(pool, true);
    dev(d)->DestroyCommandPool(d, pool, a);
}

// ── acquiring Frame Home's frames (see Gen) ─────────────────────────────────────────────────────

// (g_oursMutex held) Slot k's acquire command buffer for queue family `family`, recorded once; null if it can't be
// made (or the slot's was made for another family: one compositor queue family draws the home).
VkCommandBuffer acquireCommandBuffer(DeviceData* d, Gen* g, uint32_t k, uint32_t family) {
    if (g->acquire[k]) return g->acquireFamily[k] == family ? g->acquire[k] : VK_NULL_HANDLE;
    auto pit = g_ours.acquirePools.find(family);
    if (pit == g_ours.acquirePools.end()) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        if (!d->CreateCommandPool || d->CreateCommandPool(d->device, &pci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        pit = g_ours.acquirePools.emplace(family, pool).first;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pit->second;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (d->AllocateCommandBuffers(d->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    // A command buffer a layer makes itself carries the loader's dispatch like the device's (layers below look it up).
    if (!d->setLoaderData) *reinterpret_cast<void**>(cb) = *reinterpret_cast<void**>(d->device);   // (an old loader)
    else if (d->setLoaderData(d->device, cb) != VK_SUCCESS) { d->FreeCommandBuffers(d->device, pit->second, 1, &cb); return VK_NULL_HANDLE; }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;   // (submitted again with each new frame in the slot)
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;       // as Frame Home released it (and as an older one left it)
    b.srcAccessMask = 0;                                       // (an acquire: the release made Frame Home's writes available)
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.dstQueueFamilyIndex = family;
    b.image = g->image[k];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, g->layers};
    if (d->BeginCommandBuffer(cb, &bi) != VK_SUCCESS) { d->FreeCommandBuffers(d->device, pit->second, 1, &cb); return VK_NULL_HANDLE; }
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (d->EndCommandBuffer(cb) != VK_SUCCESS) { d->FreeCommandBuffers(d->device, pit->second, 1, &cb); return VK_NULL_HANDLE; }
    g->acquire[k] = cb;
    g->acquireFamily[k] = family;
    static bool said = false;
    if (!said) { said = true; logf("Frame Home's frames are acquired from its queue before they're sampled (queue family %u)", family); }
    return cb;
}

// The acquires to submit just ahead of a batch of the compositor's command buffers: one per slot whose frame one of
// them draws and that isn't acquired yet. None (the usual case: a frame is acquired once) appends nothing.
template <class CommandBufferAt>
void acquiresFor(DeviceData* d, uint32_t family, uint32_t n, CommandBufferAt at, std::vector<VkCommandBuffer>& out) {
    auto visit = [&](Gen* g) {
        if (g->users.empty()) return;
        for (uint32_t i = 0; i < n; ++i) {
            auto u = g->users.find(at(i));
            if (u == g->users.end()) continue;
            for (uint32_t k = 0; k < envshare::kSlots; ++k) {
                if (!(u->second.slots & (1u << k)) || !u->second.frame[k] || g->acquiredFrame[k] == u->second.frame[k]) continue;
                if (VkCommandBuffer a = acquireCommandBuffer(d, g, k, family)) { out.push_back(a); ++g_ours.acquires; }
                g->acquiredFrame[k] = u->second.frame[k];
            }
        }
    };
    for (Gen* g : g_ours.retired) visit(g);
    if (g_ours.cur) visit(g_ours.cur);
}

uint32_t queueFamilyOf(VkQueue q) {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_queueFamily.find(q);
    return it == g_queueFamily.end() ? VK_QUEUE_FAMILY_IGNORED : it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    onSubmit(q);
    DeviceData* dd = dev(q);
    if (!g_imported) return dd->QueueSubmit(q, n, si, f);   // (nothing of ours drawn: no frames imported)
    const uint32_t family = queueFamilyOf(q);
    std::vector<std::vector<VkCommandBuffer>> acq(n);
    bool any = false;
    if (family != VK_QUEUE_FAMILY_IGNORED) {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        if (g_ours.d == dd)
            for (uint32_t i = 0; i < n; ++i) {
                acquiresFor(dd, family, si[i].commandBufferCount, [&](uint32_t c) { return si[i].pCommandBuffers[c]; }, acq[i]);
                any |= !acq[i].empty();
            }
    }
    if (!any) return dd->QueueSubmit(q, n, si, f);
    // Each batch that needs some gets a batch of its acquires just before it (no semaphores: they wait on nothing,
    // and the barriers order the batch's sampling after them in submission order).
    std::vector<VkSubmitInfo> batches;
    batches.reserve(2 * n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!acq[i].empty()) {
            VkSubmitInfo a = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            a.commandBufferCount = static_cast<uint32_t>(acq[i].size());
            a.pCommandBuffers = acq[i].data();
            batches.push_back(a);
        }
        batches.push_back(si[i]);
    }
    return dd->QueueSubmit(q, static_cast<uint32_t>(batches.size()), batches.data(), f);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) {
    onSubmit(q);
    DeviceData* dd = dev(q);
    auto submit = dd->QueueSubmit2 ? dd->QueueSubmit2 : dd->QueueSubmit2KHR;
    if (!g_imported) return submit(q, n, si, f);
    const uint32_t family = queueFamilyOf(q);
    std::vector<std::vector<VkCommandBuffer>> acq(n);
    bool any = false;
    if (family != VK_QUEUE_FAMILY_IGNORED) {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        if (g_ours.d == dd)
            for (uint32_t i = 0; i < n; ++i) {
                acquiresFor(dd, family, si[i].commandBufferInfoCount, [&](uint32_t c) { return si[i].pCommandBufferInfos[c].commandBuffer; }, acq[i]);
                any |= !acq[i].empty();
            }
    }
    if (!any) return submit(q, n, si, f);
    std::vector<std::vector<VkCommandBufferSubmitInfo>> infos(n);
    std::vector<VkSubmitInfo2> batches;
    batches.reserve(2 * n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!acq[i].empty()) {
            for (VkCommandBuffer c : acq[i]) {
                VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
                ci.commandBuffer = c;
                infos[i].push_back(ci);
            }
            VkSubmitInfo2 a = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            a.commandBufferInfoCount = static_cast<uint32_t>(infos[i].size());
            a.pCommandBufferInfos = infos[i].data();
            batches.push_back(a);
        }
        batches.push_back(si[i]);
    }
    return submit(q, static_cast<uint32_t>(batches.size()), batches.data(), f);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice d, uint32_t family, uint32_t index, VkQueue* q) {
    dev(d)->GetDeviceQueue(d, family, index, q);
    std::lock_guard<std::mutex> l(g_mutex);
    if (*q) g_queueFamily[*q] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2* info, VkQueue* q) {
    dev(d)->GetDeviceQueue2(d, info, q);
    std::lock_guard<std::mutex> l(g_mutex);
    if (*q) g_queueFamily[*q] = info->queueFamilyIndex;
}

// ── instance / device plumbing ──────────────────────────────────────────────────────────────────

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    auto* link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = static_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    InstanceData id;
    id.instance = *out;
    id.gipa = gipa;
    id.DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
    id.GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(*out, "vkGetPhysicalDeviceMemoryProperties"));
    id.GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(*out, "vkGetPhysicalDeviceProperties2"));
    if (!id.GetPhysicalDeviceProperties2)
        id.GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(*out, "vkGetPhysicalDeviceProperties2KHR"));
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_instances[key(*out)] = id;
    }
    // Physical devices share the instance's dispatch key: remember which instance data they belong to.
    if (isCompositor()) logf("vrcompositor instance created (layer active, pid %d)", static_cast<int>(::getpid()));
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks* a) {
    PFN_vkDestroyInstance destroy;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        destroy = g_instances[key(inst)].DestroyInstance;
        g_instances.erase(key(inst));
    }
    destroy(inst, a);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    auto* link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = static_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (auto* l = static_cast<const VkLayerDeviceCreateInfo*>(ci->pNext); l; l = static_cast<const VkLayerDeviceCreateInfo*>(l->pNext))
        if (l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && l->function == VK_LOADER_DATA_CALLBACK) setLoaderData = l->u.pfnSetDeviceLoaderData;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    // The compositor's device gets VK_EXT_fragment_density_map when the GPU has it, so it can read Frame Home's
    // foveated frames as they were drawn (subsampled). If the device won't create with it, it's made as asked.
    bool samplesSubsampled = false;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (isCompositor()) {
        std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
        bool enabled = false;
        for (const char* e : exts) enabled |= !std::strcmp(e, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
        bool available = false;
        InstanceData* inst = nullptr;
        {
            std::lock_guard<std::mutex> l(g_mutex);
            auto it = g_instances.find(key(pd));
            if (it != g_instances.end()) inst = &it->second;
        }
        auto enumerate = inst ? reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst->instance, "vkEnumerateDeviceExtensionProperties")) : nullptr;
        uint32_t n = 0;
        if (enumerate && enumerate(pd, nullptr, &n, nullptr) == VK_SUCCESS) {
            std::vector<VkExtensionProperties> props(n);
            if (enumerate(pd, nullptr, &n, props.data()) == VK_SUCCESS)
                for (const VkExtensionProperties& e : props) available |= !std::strcmp(e.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
        }
        samplesSubsampled = enabled;
        if (!enabled && available) {
            exts.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
            VkDeviceCreateInfo withFdm = *ci;
            withFdm.enabledExtensionCount = static_cast<uint32_t>(exts.size());
            withFdm.ppEnabledExtensionNames = exts.data();
            r = create(pd, &withFdm, a, out);
            samplesSubsampled = r == VK_SUCCESS;
            if (r != VK_SUCCESS) logf("compositor device with VK_EXT_fragment_density_map failed (%d): made as asked", static_cast<int>(r));
        }
    }
    if (r != VK_SUCCESS) r = create(pd, ci, a, out);   // (not the compositor, or made as asked)
    if (r != VK_SUCCESS) return r;
    DeviceData dd;
    dd.samplesSubsampled = samplesSubsampled;
    dd.device = *out;
    dd.physical = pd;
    dd.gdpa = gdpa;
    dd.setLoaderData = setLoaderData;
#define FH_LOAD(n) dd.n = reinterpret_cast<PFN_vk##n>(gdpa(*out, "vk" #n));
    FH_DEVICE_FUNCS(FH_LOAD)
#undef FH_LOAD
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_instances.find(key(pd));                 // a physical device carries its instance's dispatch key
        dd.inst = it != g_instances.end() ? &it->second : nullptr;
        g_devices[key(*out)] = dd;
    }
    if (isCompositor()) {
        logf("vrcompositor device created");
        static std::once_flag once;
        std::call_once(once, [] {
            startHealth();
            g_starsIndices = starsIndexCount();
            logf("Aurora's stars: %u indices", g_starsIndices);
            if (!g_off) std::thread(serve).detach();
        });
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice d, const VkAllocationCallbacks* a) {
    PFN_vkDestroyDevice destroy;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        destroy = g_devices[key(d)].DestroyDevice;
    }
    {
        std::lock_guard<std::mutex> ol(g_oursMutex);
        if (g_ours.d && g_ours.d->device == d) {
            // (the device's work is all done by now: the spec requires it of vkDestroyDevice's caller)
            retireCurrent();
            for (Gen* g : g_ours.retired) destroyGen(g_ours.d, g);
            g_ours.retired.clear();
            g_cbPool.clear();
            for (auto& p : g_ours.acquirePools) g_ours.d->DestroyCommandPool(d, p.second, nullptr);   // (after the generations' command buffers)
            if (g_ours.acquires) logf("%llu Frame Home frames acquired", static_cast<unsigned long long>(g_ours.acquires));
            // ensureBase()'s objects go with the device (a compositor that makes a new device gets them made again).
            DeviceData* od = g_ours.d;
            for (auto& u : g_ours.ubo) {
                if (u.buffer) od->DestroyBuffer(d, u.buffer, nullptr);
                if (u.mem) od->FreeMemory(d, u.mem, nullptr);   // (unmapped by freeing)
            }
            if (g_ours.setLayout) od->DestroyDescriptorSetLayout(d, g_ours.setLayout, nullptr);
            if (g_ours.setLayoutSubsampled) od->DestroyDescriptorSetLayout(d, g_ours.setLayoutSubsampled, nullptr);
            if (g_ours.sampler) od->DestroySampler(d, g_ours.sampler, nullptr);
            if (g_ours.subsampledSampler) od->DestroySampler(d, g_ours.subsampledSampler, nullptr);
            if (g_ours.fragment) od->DestroyShaderModule(d, g_ours.fragment, nullptr);
            g_ours = Ours();
            g_latchNs = 0;
            g_latchSlot = -1;
            // a clean close (SteamVR quitting or restarting the compositor): not a crash
            if (g_armed.exchange(false)) ::unlink((dir() + "/starting").c_str());
        }
    }
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_devices.erase(key(d));
    }
    destroy(d, a);
}

// Hooks handed out by name. Outside vrcompositor only the create/destroy plumbing is hooked.
PFN_vkVoidFunction intercept(const char* name) {
#define FH_HOOK(n) if (!std::strcmp(name, "vk" #n)) return reinterpret_cast<PFN_vkVoidFunction>(n);
    FH_HOOK(CreateInstance) FH_HOOK(DestroyInstance) FH_HOOK(CreateDevice) FH_HOOK(DestroyDevice)
    if (!isCompositor()) return nullptr;
    FH_HOOK(CreateShaderModule) FH_HOOK(CreatePipelineLayout) FH_HOOK(CreateGraphicsPipelines) FH_HOOK(DestroyPipeline)
    FH_HOOK(CmdBindPipeline) FH_HOOK(CmdDraw) FH_HOOK(CmdDrawIndexed) FH_HOOK(QueueSubmit)
    FH_HOOK(AllocateCommandBuffers) FH_HOOK(FreeCommandBuffers) FH_HOOK(BeginCommandBuffer) FH_HOOK(ResetCommandBuffer)
    FH_HOOK(ResetCommandPool) FH_HOOK(DestroyCommandPool) FH_HOOK(GetDeviceQueue) FH_HOOK(GetDeviceQueue2)
#undef FH_HOOK
    if (!std::strcmp(name, "vkQueueSubmit2") || !std::strcmp(name, "vkQueueSubmit2KHR")) return reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2);
    return nullptr;
}

}  // namespace

FH_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frameHome_GetDeviceProcAddr(VkDevice d, const char* name) {
    DeviceData* dd = dev(d);
    if (!dd) return nullptr;
    PFN_vkVoidFunction next = dd->gdpa(d, name);
    if (!next) return nullptr;                       // never expose a hook the driver doesn't have
    PFN_vkVoidFunction f = intercept(name);
    return f ? f : next;
}

FH_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL frameHome_GetInstanceProcAddr(VkInstance inst, const char* name) {
    if (!std::strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(frameHome_GetInstanceProcAddr);
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(frameHome_GetDeviceProcAddr);
    if (!std::strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
    if (!inst) return nullptr;
    PFN_vkGetInstanceProcAddr next;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_instances.find(key(inst));
        if (it == g_instances.end()) return nullptr;
        next = it->second.gipa;
    }
    if (PFN_vkVoidFunction f = intercept(name)) return f;
    return next(inst, name);
}

FH_EXPORT VKAPI_ATTR VkResult VKAPI_CALL frameHome_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = frameHome_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = frameHome_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
