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

#include <atomic>
#include <chrono>
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
    X(QueueSubmit2KHR) X(QueueWaitIdle) X(DeviceWaitIdle)

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    InstanceData* inst = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    bool samplesSubsampled = false;      // VK_EXT_fragment_density_map is on: Frame Home's foveated frames read as made
#define FH_DECL(n) PFN_vk##n n = nullptr;
    FH_DEVICE_FUNCS(FH_DECL)
#undef FH_DECL
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, InstanceData*> g_physToInstance;   // physical device (dispatch key) -> instance data
std::unordered_map<void*, DeviceData> g_devices;

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

// Our objects on the compositor's device.
struct Ours {
    DeviceData* d = nullptr;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayoutSubsampled = VK_NULL_HANDLE;   // (null: this device can't sample subsampled images)
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, subsampledSampler = VK_NULL_HANDLE;
    // The imported frames (one generation per Hello).
    struct Slot { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
                  VkBuffer ubo = VK_NULL_HANDLE; VkDeviceMemory uboMem = VK_NULL_HANDLE; void* uboMapped = nullptr;
                  VkDescriptorSet set = VK_NULL_HANDLE, setSubsampled = VK_NULL_HANDLE; uint64_t uboFrame = ~0ull; };
    Slot slot[envshare::kSlots];
    bool imported = false;
    bool subsampled = false;      // the imported images are subsampled (drawn with the subsampled twins)
    uint32_t generation = 0;      // of the imported images
    int lastGood = -1;            // newest slot of this generation drawn (shown while Frame Home resizes)
    envshare::State* state = nullptr;
    size_t stateSize = 0;
    uint64_t healthyFrames = 0;
    bool baseReady = false, baseFailed = false;   // ensureBase(): all made / gave up (the rest of the session is Aurora's)
} g_ours;
std::mutex g_oursMutex;
int g_latchSlot = -1;                 // frameSlot(): this compositor frame's pick, when it was made, and whether the
uint64_t g_latchNs = 0;               // home was drawn with it (sampledNs stamped)
bool g_latchStamped = false;

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

void releaseImports() {
    DeviceData* d = g_ours.d;
    if (!d) return;
    for (auto& s : g_ours.slot) {
        if (s.view) d->DestroyImageView(d->device, s.view, nullptr);
        if (s.image) d->DestroyImage(d->device, s.image, nullptr);
        if (s.mem) d->FreeMemory(d->device, s.mem, nullptr);
        s.view = VK_NULL_HANDLE; s.image = VK_NULL_HANDLE; s.mem = VK_NULL_HANDLE;
        s.uboFrame = ~0ull;
    }
    if (g_ours.state) ::munmap(g_ours.state, g_ours.stateSize);
    g_ours.state = nullptr;
    g_ours.imported = false;
    g_ours.lastGood = -1;
}

// One-time objects (shader, set layout, pool, sampler, per-slot UBOs and sets) on the compositor's device.
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
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
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
        logf(g_ours.setLayoutSubsampled ? "subsampled frames: readable" : "subsampled frames: no sampler/layout (plain frames only)");
    }
    // A subsampled combined image sampler takes half again a plain one's room, and Turnip sizes a pool for that only
    // when the device enables the fragmentDensityMap feature (left off: it would cost the compositor elsewhere). So the
    // count asks for the room: kSlots plain + kSlots subsampled = 2.5 * kSlots plain-sized ones (3 * kSlots to spare).
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * envshare::kSlots}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * envshare::kSlots}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2 * envshare::kSlots;   // a set of each kind per slot
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    if (d->CreateDescriptorPool(d->device, &pi, nullptr, &g_ours.pool) != VK_SUCCESS) return false;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (d->CreateSampler(d->device, &si, nullptr, &g_ours.sampler) != VK_SUCCESS) return false;
    for (auto& s : g_ours.slot) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 256;
        bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (d->CreateBuffer(d->device, &bi, nullptr, &s.ubo) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        d->GetBufferMemoryRequirements(d->device, s.ubo, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryTypeFor(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->device, &ai, nullptr, &s.uboMem) != VK_SUCCESS) return false;
        d->BindBufferMemory(d->device, s.ubo, s.uboMem, 0);
        d->MapMemory(d->device, s.uboMem, 0, VK_WHOLE_SIZE, 0, &s.uboMapped);
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = g_ours.pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &g_ours.setLayout;
        if (d->AllocateDescriptorSets(d->device, &dai, &s.set) != VK_SUCCESS) return false;
        if (g_ours.setLayoutSubsampled) {
            dai.pSetLayouts = &g_ours.setLayoutSubsampled;
            if (d->AllocateDescriptorSets(d->device, &dai, &s.setSubsampled) != VK_SUCCESS) {
                s.setSubsampled = VK_NULL_HANDLE;
                g_ours.setLayoutSubsampled = VK_NULL_HANDLE;   // (no twins of that kind are made: plain frames only)
                logf("subsampled frames: no descriptor set (plain frames only)");
            }
        }
    }
    g_ours.baseFailed = false;
    g_ours.baseReady = true;
    logf("ready to replace Aurora");
    return true;
}

// Imports a Hello's images (compositor thread, inside QueueSubmit: nothing of ours is in flight once the queue idles).
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
    // the last ones (the sets in this very submit never point at destroyed images).
    const size_t stateSize = sizeof(envshare::State);
    void* st = ::mmap(nullptr, stateSize, PROT_READ | PROT_WRITE, MAP_SHARED, p.stateFd, 0);
    ::close(p.stateFd);
    p.stateFd = -1;
    if (st == MAP_FAILED) { logf("cannot map the shared state"); closeAll(); return; }
    // Subsampled frames need this device to sample them (the extension, the sampler and the twins made for them):
    // otherwise Frame Home is told, and sends plain ones.
    const bool subsampled = (p.hello.flags & VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) != 0;
    bool subsampledSets = g_ours.setLayoutSubsampled != VK_NULL_HANDLE;
    for (const auto& s : g_ours.slot) subsampledSets &= s.setSubsampled != VK_NULL_HANDLE;
    if (p.hello.flags & ~static_cast<uint32_t>(VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT) || (subsampled && !subsampledSets)) {
        static_cast<envshare::State*>(st)->refusedFlags.store(p.hello.flags);
        logf("frames with create flags 0x%x: can't read them here (Frame Home will send plain ones)", p.hello.flags);
        ::munmap(st, stateSize);
        closeAll();
        return;
    }
    struct Made { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; } made[envshare::kSlots];
    auto dropMade = [&] {
        for (Made& m : made) {
            if (m.view) d->DestroyImageView(d->device, m.view, nullptr);
            if (m.image) d->DestroyImage(d->device, m.image, nullptr);
            if (m.mem) d->FreeMemory(d->device, m.mem, nullptr);
        }
        ::munmap(st, stateSize);
    };
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        Made& s = made[i];
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
        bool ok = d->CreateImage(d->device, &ci, nullptr, &s.image) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements req;
            d->GetImageMemoryRequirements(d->device, s.image, &req);
            VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            imp.fd = p.fds[i];
            VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            ded.image = s.image;
            if (p.hello.dedicated) imp.pNext = &ded;
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.pNext = &imp;
            ai.allocationSize = p.hello.memorySize[i];
            ai.memoryTypeIndex = p.hello.memoryType;
            ok = (req.memoryTypeBits & (1u << p.hello.memoryType)) && req.size <= p.hello.memorySize[i] &&
                 d->AllocateMemory(d->device, &ai, nullptr, &s.mem) == VK_SUCCESS;
            if (ok) p.fds[i] = -1;   // the driver owns it now
            ok = ok && d->BindImageMemory(d->device, s.image, s.mem, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = s.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vi.format = ci.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, p.hello.layers};
            ok = d->CreateImageView(d->device, &vi, nullptr, &s.view) == VK_SUCCESS;
        }
        if (!ok) { logf("importing frame slot %u failed: keeping the last frames", i); dropMade(); closeAll(); return; }
    }
    closeAll();
    std::lock_guard<std::mutex> ol(g_oursMutex);
    // Swap: nothing of the compositor's (any queue) may still be reading the old frames.
    if (g_ours.imported) { d->DeviceWaitIdle(d->device); releaseImports(); }
    g_ours.state = static_cast<envshare::State*>(st);
    g_ours.stateSize = stateSize;
    for (uint32_t i = 0; i < envshare::kSlots; ++i) {
        auto& s = g_ours.slot[i];
        s.image = made[i].image; s.mem = made[i].mem; s.view = made[i].view;
        VkDescriptorImageInfo ii = {g_ours.sampler, s.view, VK_IMAGE_LAYOUT_GENERAL};   // (the subsampled kind's sampler is in its layout)
        VkDescriptorBufferInfo bi = {s.ubo, 0, 256};
        const VkDescriptorSet set = subsampled ? s.setSubsampled : s.set;
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
        w[1].dstSet = set; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[1].pBufferInfo = &bi;
        d->UpdateDescriptorSets(d->device, 2, w, 0, nullptr);
    }
    g_ours.imported = true;
    g_ours.subsampled = subsampled;
    logf("imported Frame Home's frames: %ux%u, usage 0x%x, flags 0x%x (generation %u)", p.hello.width, p.hello.height, p.hello.usage,
         p.hello.flags, p.hello.generation);
    g_ours.generation = p.hello.generation;
    g_ours.lastGood = -1;
    g_latchNs = 0;
    armHealth();
    logf("Frame Home connected: %ux%u frames, %u slots", p.hello.width, p.hello.height, envshare::kSlots);
}

// The slot to draw now, or -1 for Aurora. While Frame Home is connected the home stays up: a late frame, a home
// loading or new images after a resize keep the last good frame on screen (Aurora flashing in looked broken).
// Only Frame Home gone, or frozen for 5 s (hung), brings Aurora back.
std::chrono::steady_clock::time_point g_disconnectedAt;
int liveSlot() {
    if (g_off || !g_ours.imported || !g_ours.state) return -1;
    if (!g_connected)   // Frame Home restarting (a setting that rebuilds the renderer): hold its last frame a moment
        return std::chrono::steady_clock::now() - g_disconnectedAt < std::chrono::seconds(3) ? g_ours.lastGood : -1;
    envshare::State* s = g_ours.state;
    const uint64_t now = envshare::nowNs();
    if (now - s->publishNs.load() > 5000000000ull && now > s->busyUntilNs.load()) return -1;
    const uint32_t k = s->latest.load(std::memory_order_acquire);
    if (k < envshare::kSlots && s->slot[k].generation == g_ours.generation) g_ours.lastGood = static_cast<int>(k);
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
    return g_latchSlot;
}

void fillUbo(int k) {
    auto& slot = g_ours.slot[k];
    envshare::State* s = g_ours.state;
    const envshare::Slot& src = s->slot[k];
    if (slot.uboFrame == src.frame) return;
    float data[16 * 2 + 4 * 2 + 4];
    std::memcpy(data, src.viewProj, sizeof src.viewProj);
    std::memcpy(data + 32, src.eyePos, sizeof src.eyePos);
    data[40] = s->fade.load() / 1000.0f;
    data[41] = s->show.load() ? 1.0f : 0.0f;
    data[42] = data[43] = 0;
    std::memcpy(slot.uboMapped, data, sizeof data);
    slot.uboFrame = src.frame;
}

// ── health: off switch and crash guard ──────────────────────────────────────────────────────────

void startHealth() {
    if (exists(dir() + "/off")) { g_off = true; logf("off: Aurora as usual (delete %s/off to turn Frame Home back on)", dir().c_str()); return; }
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
        g_off = true;
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
                    fds.assign(p, p + cnt);
                }
            // v5: the images' create flags ride along too; v4: their usage (Frame Home draws straight into them), no
            // flags; v3 (an older Frame Home): made for copying into.
            const bool v5 = n == static_cast<ssize_t>(sizeof h) && h.version == envshare::kVersion;
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
        g_disconnectedAt = std::chrono::steady_clock::now();
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

// What to do with a draw: 0 as is, 1 replace with the twin, 2 skip.
int drawAction(VkCommandBuffer cb, Twin& t, int& slot) {
    if (g_off || !g_ours.imported) return 0;
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
    slot = frameSlot();
    if (slot < 0) return 0;
    // Passthrough on: the compositor would blend the home half over the cameras. Step aside for the real view.
    if (envshare::nowNs() - g_cameraNs.load() < 150000000ull) return 2;
    if (!t.plain.pipeline) return 2;
    if (g_ours.subsampled && !t.subsampled.pipeline) return 0;   // (never imported so: import checks the twins can)
    fillUbo(slot);
    if (!g_latchStamped) { g_latchStamped = true; g_ours.state->sampledNs.store(g_latchNs); }   // the moment it took the frame
    return 1;
}

void drawTwin(VkCommandBuffer cb, const Twin& t, int slot, const std::function<void()>& draw) {
    DeviceData* dd = dev(cb);
    VkPipeline original;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        original = g_bound[cb];
    }
    const bool subsampled = g_ours.subsampled;
    const TwinPipeline& k = subsampled ? t.subsampled : t.plain;
    const VkDescriptorSet set = subsampled ? g_ours.slot[slot].setSubsampled : g_ours.slot[slot].set;
    dd->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k.pipeline);
    dd->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, k.layout, 1, 1, &set, 0, nullptr);
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
    int slot = -1;
    const int act = drawAction(cb, t, slot);
    traceDraw(cb, "draw", vc, act);
    switch (act) {
        case 1: drawTwin(cb, t, slot, [&] { dev(cb)->CmdDraw(cb, vc, ic, fv, fi); }); return;
        case 2: return;
        default: dev(cb)->CmdDraw(cb, vc, ic, fv, fi);
    }
}

VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t ic, uint32_t fi, int32_t vo, uint32_t fin) {
    Twin t;
    int slot = -1;
    int act = drawAction(cb, t, slot);
    if (act == 0 && g_starsIndices && n == g_starsIndices && g_ours.imported) {   // the stars, while the home shows
        std::lock_guard<std::mutex> ol(g_oursMutex);
        if (liveSlot() >= 0 && envshare::nowNs() - g_cameraNs.load() > 150000000ull) act = 2;
    }
    traceDraw(cb, "drawIndexed", n, act);
    switch (act) {
        case 1: drawTwin(cb, t, slot, [&] { dev(cb)->CmdDrawIndexed(cb, n, ic, fi, vo, fin); }); return;
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
        if (off != g_off) { g_off = off; logf(off ? "turned off: Aurora as usual" : "turned back on"); }
        if (exists(dir() + "/trace")) { ::unlink((dir() + "/trace").c_str()); logf("=== trace: the next 1500 draws ==="); g_traceDraws = 1500; }
    }
    if (g_off) return;
    importPending(dd, q);
    if (g_ours.imported && liveSlot() >= 0 && ++g_ours.healthyFrames == 900) markHealthy();   // ~10 s showing the home
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    onSubmit(q);
    return dev(q)->QueueSubmit(q, n, si, f);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) {
    onSubmit(q);
    DeviceData* dd = dev(q);
    return (dd->QueueSubmit2 ? dd->QueueSubmit2 : dd->QueueSubmit2KHR)(q, n, si, f);
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
            releaseImports();
            g_ours = Ours();
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
