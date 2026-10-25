#include <atomic>
#include "hsl_renderer.h"
#include "selection_spirv.h"
#include "guide_spirv.h"
#include "debug_spirv.h"
#include <unordered_set>
#include <set>

#include "loaders/rendtxtr_parser.h"
#include "core/parallel.h"             // par::forEach: animated meshes rebuilt across cores
#include "fov_spirv.h"                 // the smooth-foveation pass
#include "shader_bake.h"               // prepared and specialized shaders (made at bake, or here for APK homes)
#include "spv_opt.h"                   // the SPIR-V optimizer over every program's shaders
#include "spv_check.h"                 // SPIR-V from a .fenv checked before the driver sees it
#include "tex_cache.h"                   // mip chains for textures shipped without one, built once

#ifdef _WIN32
#include <windows.h>                   // SetThreadPriority for the decode workers
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <unordered_map>

#define LOG(...)  do { std::fprintf(stderr, "[render] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#define VK_OK(x)  ([&]() { VkResult r_ = (x); if (r_ != VK_SUCCESS) { LOG("%s failed: %d", #x, r_); return false; } return true; }())

namespace {

// SPIR-V a home brought (its .fenv, or made from it) fit to hand the driver: spv_check.h. A malformed module is said
// and its meshes skipped, as a module the driver refuses is.
bool shaderOk(const std::vector<uint32_t>& code, const std::string& program) {
    std::string why;
    if (spvcheck::check(code, &why)) return true;
    LOG("shader of %s is malformed SPIR-V (%s): not handed to the driver", program.c_str(), why.c_str());
    return false;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

float fromHalf(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {                                   // subnormal
            int e = -1;
            uint32_t m = man;
            do { m <<= 1; ++e; } while (!(m & 0x400u));
            bits = sign | static_cast<uint32_t>(127 - 15 - e) << 23 | (m & 0x3FFu) << 13;
        }
    } else if (exp == 31) bits = sign | 0x7F800000u | man << 13;
    else bits = sign | (exp + 112) << 23 | man << 13;
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Largest texture side kept on a GPU without native ASTC (the PC editor). HSR Studio likewise keeps most textures
// at 512-2048 and full 4K only where it fills the screen; 4K RGBA after ASTC decode pushed VRAM into paging.
// FH_MAX_TEX overrides (0 = no cap).
std::atomic<uint32_t> g_textureCap{0xFFFFFFFEu};   // unset: FH_MAX_TEX or 2048 (setTextureCap, below)
// Halves an RGBA8 image (2x2 box; an odd last row / column folds into its neighbour).
void halveRGBA(std::vector<u8>& px, u32& w, u32& h) {
    const u32 nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<u8> out(static_cast<size_t>(nw) * nh * 4);
    for (u32 y = 0; y < nh; ++y)
        for (u32 x = 0; x < nw; ++x) {
            const u32 x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1), y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
            for (int c = 0; c < 4; ++c)
                out[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<u8>(
                    (px[(static_cast<size_t>(y0) * w + x0) * 4 + c] + px[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                     px[(static_cast<size_t>(y1) * w + x0) * 4 + c] + px[(static_cast<size_t>(y1) * w + x1) * 4 + c] + 2) / 4);
        }
    px.swap(out);
    w = nw;
    h = nh;
}

uint32_t maxDecodedSide() {
    static const uint32_t envCap = [] {
        const char* e = std::getenv("FH_MAX_TEX");
        return e ? static_cast<uint32_t>(std::atoi(e)) : 2048u;
    }();
    const uint32_t set = g_textureCap.load(), cap = set == 0xFFFFFFFEu ? envCap : set;
    return cap ? cap : 0xFFFFFFFFu;
}

uint16_t toHalf(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, exp = (x >> 23) & 0xFF, mant = x & 0x7FFFFF;
    if (exp == 0xFF) return static_cast<uint16_t>(sign | 0x7C00 | (mant ? 0x200 : 0));
    int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    if (e <= 0) return static_cast<uint16_t>(sign);
    return static_cast<uint16_t>(sign | (e << 10) | (mant >> 13));
}

VkDescriptorType descriptorType(spv::ResKind k) {
    switch (k) {
        case spv::ResKind::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case spv::ResKind::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case spv::ResKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case spv::ResKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case spv::ResKind::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case spv::ResKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// FH_DEBUG=1 prints the bindings of the first meshes; FH_DEBUG=<name part> those of matching meshes.
static std::atomic<int> g_allocCount{0};       // live-ish vkAllocateMemory count (diagnostics)

bool debugMesh(const MeshData& md) {
    static const char* d = std::getenv("FH_DEBUG");
    static int shown = 0;
    if (!d) return false;
    if (std::string(d) == "1") return shown++ < 6;
    for (std::string rest = d; !rest.empty();) {             // comma-separated name parts
        const size_t c = rest.find(',');
        const std::string part = rest.substr(0, c);
        if (!part.empty() && md.name.find(part) != std::string::npos) return true;
        rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
    }
    return false;
}

bool isBuffer(spv::ResKind k) { return k == spv::ResKind::UniformBuffer || k == spv::ResKind::StorageBuffer; }

// FH_SKY_FIRST=1: the old sky order (first, no depth test) for comparisons.
bool skyFirst() { static const bool v = std::getenv("FH_SKY_FIRST") != nullptr; return v; }

}  // namespace

// ── Vulkan plumbing ──────────────────────────────────────────────────────────────────────────────

uint32_t HslRenderer::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return 0;
}

// Staging / uniform memory: plain system memory, never the small device-local BAR heap (filling that with
// texture staging made the driver evict and stall).
uint32_t HslRenderer::hostMemoryType(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want &&
            !(mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    return memoryType(bits, want);
}

HslRenderer::MemRange HslRenderer::allocMemory(const VkMemoryRequirements& req, uint32_t type, bool linear, bool map) {
    static constexpr VkDeviceSize kBlock = 256ull << 20, kDedicated = 128ull << 20;
    MemRange r;
    if (req.size >= kDedicated) {                // big render targets / 4K textures: their own allocation
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        const auto t0 = std::chrono::steady_clock::now();
        if (vkAllocateMemory(device_, &ai, nullptr, &r.mem) != VK_SUCCESS) return MemRange{};
        LOG("dedicated %.1f MB: %.1f ms", req.size / 1048576.0, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        ++g_allocCount;
        r.size = req.size;
        if (map) vkMapMemory(device_, r.mem, 0, VK_WHOLE_SIZE, 0, &r.mapped);
        return r;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 16);
    std::lock_guard<std::mutex> l(allocMutex_);
    auto take = [&](MemBlock& b) -> bool {
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            const VkDeviceSize start = it->first, len = it->second;
            const VkDeviceSize at = (start + align - 1) / align * align;
            if (at + req.size > start + len) continue;
            b.free.erase(it);
            if (at > start) b.free[start] = at - start;
            if (at + req.size < start + len) b.free[at + req.size] = start + len - (at + req.size);
            r.mem = b.mem;
            r.offset = at;
            r.size = req.size;
            r.mapped = b.mapped ? static_cast<u8*>(b.mapped) + at : nullptr;
            r.pooled = true;
            return true;
        }
        return false;
    };
    for (MemBlock& b : blocks_)
        if (b.type == type && b.linear == linear && (b.mapped != nullptr) == map && take(b)) return r;
    MemBlock nb;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = kBlock;
    ai.memoryTypeIndex = type;
    const auto t0 = std::chrono::steady_clock::now();
    if (vkAllocateMemory(device_, &ai, nullptr, &nb.mem) != VK_SUCCESS) return MemRange{};
    LOG("memory block %zu: type %u %s, %.1f ms", blocks_.size(), type, linear ? "buffers" : "images",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    ++g_allocCount;
    nb.size = kBlock;
    nb.type = type;
    nb.linear = linear;
    if (map) vkMapMemory(device_, nb.mem, 0, VK_WHOLE_SIZE, 0, &nb.mapped);
    nb.free[0] = kBlock;
    blocks_.push_back(std::move(nb));
    take(blocks_.back());
    return r;
}

void HslRenderer::freeMemory(VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, bool pooled, bool mapped) {
    if (!mem) return;
    if (!pooled) {
        if (mapped) vkUnmapMemory(device_, mem);
        vkFreeMemory(device_, mem, nullptr);
        --g_allocCount;
        return;
    }
    std::lock_guard<std::mutex> l(allocMutex_);
    for (MemBlock& b : blocks_) {
        if (b.mem != mem) continue;
        auto next = b.free.lower_bound(offset);
        if (next != b.free.end() && offset + size == next->first) {       // merge with the range after
            size += next->second;
            next = b.free.erase(next);
        }
        if (next != b.free.begin()) {                                     // and the range before
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) { prev->second += size; return; }
        }
        b.free[offset] = size;
        return;
    }
}

HslRenderer::Buffer HslRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    // Failure (out of memory, a size from a damaged file) is an empty Buffer{}: callers skip what needed it.
    if (!size || vkCreateBuffer(device_, &ci, nullptr, &b.buf) != VK_SUCCESS) { LOG("buffer of %llu bytes refused", static_cast<unsigned long long>(size)); return Buffer{}; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buf, &req);
    const uint32_t type = hostVisible ? hostMemoryType(req.memoryTypeBits)
                                      : memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const MemRange m = allocMemory(req, type, true, hostVisible);
    b.mem = m.mem;
    b.memOffset = m.offset;
    b.memSize = m.size;
    b.pooled = m.pooled;
    if (hostVisible) b.mapped = m.mapped;
    if (!b.mem || (hostVisible && !b.mapped) || vkBindBufferMemory(device_, b.buf, b.mem, b.memOffset) != VK_SUCCESS) {
        LOG("buffer of %llu bytes: no memory", static_cast<unsigned long long>(size));
        destroy(b);
        return Buffer{};
    }
    if (hostVisible) std::memset(b.mapped, 0, static_cast<size_t>(size));
    return b;
}

HslRenderer::Buffer HslRenderer::createDeviceBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!staging.buf) return Buffer{};
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    Buffer b = createBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    if (!b.buf) { destroy(staging); return Buffer{}; }
    oneShot([&](VkCommandBuffer c) {
        VkBufferCopy region = {0, 0, size};
        vkCmdCopyBuffer(c, staging.buf, b.buf, 1, &region);
    });
    retire(staging);
    return b;
}

void HslRenderer::destroy(Buffer& b) {
    if (b.buf) vkDestroyBuffer(device_, b.buf, nullptr);
    freeMemory(b.mem, b.memOffset, b.memSize, b.pooled, b.mapped != nullptr);
    b = Buffer{};
}

// A staging buffer an upload still reads: freed now, or (recorded into the frame) once that frame is done.
void HslRenderer::retire(Buffer& b) {
    if (recordInto_ && recordInto_ == loadCmd_) { loadStaging_.push_back(b); loadStaged_ += b.size; }
    else if (recordInto_) frameStaging_.push_back(b);
    else destroy(b);
    b = Buffer{};
}

// Load-time batching: every upload of the meshes a loadStep brings in is recorded into one command buffer and
// submitted once (each mesh's buffers and textures used to be a submit + queue-idle wait of their own: thousands
// of GPU round trips per home).
void HslRenderer::beginUploadBatch() {
    if (loadCmd_ || recordInto_) return;        // already batching, or inside a frame (uploads ride in it)
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &loadCmd_) != VK_SUCCESS) { loadCmd_ = VK_NULL_HANDLE; return; }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(loadCmd_, &bi);
    recordInto_ = loadCmd_;
    loadStaged_ = 0;
}

void HslRenderer::flushUploadBatch() {
    if (!loadCmd_) return;
    // Buffer copies carry no barrier of their own: make every transfer write visible to whatever reads it next.
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(loadCmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(loadCmd_);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &loadCmd_;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &loadCmd_);
    loadCmd_ = VK_NULL_HANDLE;
    recordInto_ = VK_NULL_HANDLE;
    for (Buffer& b : loadStaging_) destroy(b);
    loadStaging_.clear();
    loadStaged_ = 0;
}

void HslRenderer::destroy(Image& i) {
    if (i.view) vkDestroyImageView(device_, i.view, nullptr);
    if (i.image) vkDestroyImage(device_, i.image, nullptr);
    freeMemory(i.mem, i.memOffset, i.memSize, i.pooled, false);
    i = Image{};
}

VkImageView HslRenderer::createPicture(const u8* rgba, uint32_t w, uint32_t h) {
    Image i = createTexture(rgba, w, h, false);
    if (!i.view) return VK_NULL_HANDLE;
    pictures_.push_back(i);
    return i.view;
}

void HslRenderer::releasePicture(VkImageView view) {
    for (size_t k = 0; k < pictures_.size(); ++k)
        if (pictures_[k].view == view) { destroy(pictures_[k]); pictures_.erase(pictures_.begin() + static_cast<std::ptrdiff_t>(k)); return; }
}

template <typename F> void HslRenderer::oneShot(F&& record) {
    if (recordInto_) {                          // streaming: ride along in the frame's command buffer, no stall
        record(recordInto_);
        return;
    }
    VkCommandBuffer c;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &ai, &c);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    record(c);
    vkEndCommandBuffer(c);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, pool_, 1, &c);
}

static void imageBarrier(VkCommandBuffer c, VkImage img, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mips,
                         uint32_t layers, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA,
                         VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, baseMip, mips, 0, layers};
    vkCmdPipelineBarrier(c, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// A queue family ownership transfer of a frame shared with the compositor (all its layers, GENERAL throughout): the
// release half (from our family to VK_QUEUE_FAMILY_EXTERNAL) or the acquire half (the reverse).
static void ownershipBarrier(VkCommandBuffer c, VkImage img, uint32_t layers, uint32_t fromFamily, uint32_t toFamily,
                             VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = fromFamily;
    b.dstQueueFamilyIndex = toFamily;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(c, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

HslRenderer::Image HslRenderer::createImage2D(uint32_t w, uint32_t h, uint32_t layers, uint32_t mips,
                                              VkSampleCountFlagBits samples, VkFormat fmt, VkImageUsageFlags usage,
                                              VkImageAspectFlags aspect, VkImageViewType viewType,
                                              VkImageCreateFlags flags) {
    Image img;
    // Sizes reach here from files: refuse what Vulkan forbids (a zero side, past the GPU's limit, a mip chain longer
    // than the image's, a non-square cube) rather than hand it to the driver. Failure is an empty Image{}.
    const bool cube = viewType == VK_IMAGE_VIEW_TYPE_CUBE;
    const uint32_t side = std::max(w, h), limit = cube ? maxCubeDim_ : maxImageDim_;
    uint32_t chain = 1;
    while (chain < 32 && (side >> chain)) ++chain;
    if (!w || !h || side > limit || !mips || mips > chain || !layers || (cube && (w != h || layers % 6))) {
        LOG("image %ux%u x%u, %u mips refused", w, h, layers, mips);
        return Image{};
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = viewType == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(device_, &ci, nullptr, &img.image) != VK_SUCCESS) { LOG("image %ux%u: create failed", w, h); return Image{}; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    bool lazy = false;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
        uint32_t lz = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
        if (mp.memoryTypes[lz].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) { type = lz; lazy = true; }
    }
    if (lazy || (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};   // render targets: their own memory
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &ai, nullptr, &img.mem) == VK_SUCCESS) ++g_allocCount;
        else img.mem = VK_NULL_HANDLE;
        img.memSize = req.size;
    } else {
        const MemRange m = allocMemory(req, type, false, false);
        img.mem = m.mem;
        img.memOffset = m.offset;
        img.memSize = m.size;
        img.pooled = m.pooled;
    }
    if (!img.mem || vkBindImageMemory(device_, img.image, img.mem, img.memOffset) != VK_SUCCESS) {
        LOG("image %ux%u: no memory", w, h);
        destroy(img);
        return Image{};
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = viewType;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, mips, 0, layers};
    if (vkCreateImageView(device_, &vi, nullptr, &img.view) != VK_SUCCESS) { img.view = VK_NULL_HANDLE; destroy(img); return Image{}; }
    return img;
}

HslRenderer::Image HslRenderer::createTexture(const u8* rgba, uint32_t w, uint32_t h, bool srgb) {
    return createTextureFmt(rgba, w, h, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
}

// Allocation half of a texture upload — safe on any thread (the streaming workers run it): the image and a
// staging buffer already holding the pixels. RGBA8 or RGBA16F; the mip chain is blitted when recorded.
HslRenderer::PreparedTex HslRenderer::prepareTexture(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt, bool ownStaging) {
    PreparedTex t;
    t.w = w;
    t.h = h;
    const size_t bpp = fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    if (!w || !h) return t;                      // (log2(0) below); t.img stays empty: the caller skips it
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(physical_, fmt, &fp);
    const bool blit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                      (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) &&
                      (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    t.mips = blit ? static_cast<uint32_t>(std::floor(std::log2(std::max(w, h)))) + 1 : 1;
    t.img = createImage2D(w, h, 1, t.mips, VK_SAMPLE_COUNT_1_BIT, fmt,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D);
    if (!t.img.image) return t;
    if (ownStaging) {
        t.staging = createBuffer(static_cast<VkDeviceSize>(w) * h * bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        if (!t.staging.buf) { destroy(t.img); return t; }
        std::memcpy(t.staging.mapped, pixels, static_cast<size_t>(w) * h * bpp);
    }
    return t;
}

// Recording half: copy mip 0, blit the chain, leave every level shader-readable.
void HslRenderer::recordTextureUpload(VkCommandBuffer c, const PreparedTex& t) {
    const VkImage image = t.img.image;
    const uint32_t mips = t.mips;
    imageBarrier(c, image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {};
    copy.bufferOffset = t.stagingOffset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t.w, t.h, 1};
    vkCmdCopyBufferToImage(c, t.staging.buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t mw = static_cast<int32_t>(t.w), mh = static_cast<int32_t>(t.h);
    for (uint32_t m = 1; m < mips; ++m) {
        imageBarrier(c, image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl = {};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        bl.srcOffsets[1] = {mw, mh, 1};
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        bl.dstOffsets[1] = {mw, mh, 1};
        vkCmdBlitImage(c, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_LINEAR);
        imageBarrier(c, image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    imageBarrier(c, image, VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
}

// RGBA8 or RGBA16F texture with a blit-generated mip chain.
HslRenderer::Image HslRenderer::createTextureFmt(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt) {
    PreparedTex t = prepareTexture(pixels, w, h, fmt);
    if (!t.img.image) return Image{};
    oneShot([&](VkCommandBuffer c) { recordTextureUpload(c, t); });
    retire(t.staging);
    return t.img;
}

// 1x1-per-face cube: +Y top, -Y bottom, the four sides `side` (a hemisphere light: bright sky, dim ground).
HslRenderer::Image HslRenderer::createSolidCube(const float top[3], const float side[3], const float bottom[3]) {
    Image img = createImage2D(1, 1, 6, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    uint16_t px[6][4];
    const float* faces[6] = {side, side, top, bottom, side, side};   // +X -X +Y -Y +Z -Z
    for (int f = 0; f < 6; ++f) {
        for (int c = 0; c < 3; ++c) px[f][c] = toHalf(faces[f][c]);
        px[f][3] = toHalf(1.0f);
    }
    Buffer staging = createBuffer(sizeof(px), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, px, sizeof(px));
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copies[6] = {};
        for (uint32_t f = 0; f < 6; ++f) {
            copies[f].bufferOffset = f * 8;
            copies[f].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
            copies[f].imageExtent = {1, 1, 1};
        }
        vkCmdCopyBufferToImage(c, staging.buf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies);
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    retire(staging);
    return img;
}

// Cubemap from six faces (+X -X +Y -Y +Z -Z): RGBA8 sRGB, or RGBA16F linear HDR.
HslRenderer::Image HslRenderer::createCubeTexture(const void* faces, size_t bytes, uint32_t size, VkFormat fmt, uint32_t mips,
                                                  uint32_t levels) {
    // `faces`: mip-major, six faces per level (level m is size>>m on a side). `levels` past `mips`: the rest of the
    // chain blitted from the last given level.
    mips = std::max(1u, mips);
    uint32_t chain = 1;                          // a file's mip count past the full chain: keep the levels there are
    while (chain < 32 && (size >> chain)) ++chain;
    mips = std::min(mips, chain);
    levels = std::min(std::max(levels, mips), chain);
    const VkDeviceSize bpp = fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < mips; ++m) { const VkDeviceSize s = std::max(1u, size >> m); total += s * s * bpp * 6; }
    if (!faces || !size || total > bytes) { LOG("cube %u^2 (%u mips): %zu bytes, %llu needed", size, mips, bytes, static_cast<unsigned long long>(total)); return Image{}; }
    Image img = createImage2D(size, size, 6, levels, VK_SAMPLE_COUNT_1_BIT, fmt,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  (levels > mips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    if (!img.image) return Image{};
    Buffer staging = createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!staging.buf) { destroy(img); return Image{}; }
    std::memcpy(staging.mapped, faces, static_cast<size_t>(total));
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t s = std::max(1u, size >> m);
        for (uint32_t f = 0; f < 6; ++f) {
            VkBufferImageCopy c = {};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, f, 1};
            c.imageExtent = {s, s, 1};
            copies.push_back(c);
            off += static_cast<VkDeviceSize>(s) * s * bpp;
        }
    }
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 6, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(c, staging.buf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copies.size()), copies.data());
        for (uint32_t m = mips; m < levels; ++m) {   // each face of level m from level m-1
            imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            const int32_t from = static_cast<int32_t>(std::max(1u, size >> (m - 1))), to = static_cast<int32_t>(std::max(1u, size >> m));
            VkImageBlit bl = {};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 6};
            bl.srcOffsets[1] = {from, from, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 6};
            bl.dstOffsets[1] = {to, to, 1};
            vkCmdBlitImage(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                           VK_FILTER_LINEAR);
            imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    retire(staging);
    return img;
}

HslRenderer::Image HslRenderer::createSolid3D(const u8 rgba[4]) {
    Image img = createImage2D(1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_VIEW_TYPE_3D);
    Buffer staging = createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, rgba, 4);
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(c, staging.buf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    retire(staging);
    return img;
}

// 1x1 depth image cleared to 1.0, sampled through a compare-ALWAYS sampler: every shadow test passes (lit).
HslRenderer::Image HslRenderer::createShadowDepth() {
    Image img = createImage2D(1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                              VK_IMAGE_VIEW_TYPE_2D);
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue one = {1.0f, 0};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    return img;
}

// ── Device ───────────────────────────────────────────────────────────────────────────────────────

bool HslRenderer::init(uint32_t eyeWidth, uint32_t eyeHeight) {
    width_ = eyeWidth;
    height_ = eyeHeight;

    std::vector<const char*> iext;
    for (auto& e : instanceExtensions) iext.push_back(e.c_str());
    // Vulkan through a translation layer (MoltenVK on macOS) lists its GPU only to apps that ask for "portability"
    // devices. Drivers without the extension (Windows, Linux, the Frame) don't need it.
    bool portability = false;
    {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
        for (const VkExtensionProperties& e : have) portability |= !std::strcmp(e.extensionName, "VK_KHR_portability_enumeration");
        const bool asked = std::any_of(iext.begin(), iext.end(), [](const char* e) { return !std::strcmp(e, "VK_KHR_portability_enumeration"); });
        if (portability && !asked) iext.push_back("VK_KHR_portability_enumeration");   // (the window may have asked already)
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Frame Home";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (portability) ici.flags |= 0x00000001;   // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(iext.size());
    ici.ppEnabledExtensionNames = iext.data();
    if (!VK_OK(vkCreateInstance(&ici, nullptr, &instance_))) return false;
#ifdef FH_USE_VOLK
    volkLoadInstance(instance_);
#endif

    physical_ = pickPhysicalDevice ? pickPhysicalDevice(instance_) : VK_NULL_HANDLE;
    if (!physical_) { uint32_t n = 1; vkEnumeratePhysicalDevices(instance_, &n, &physical_); }
    if (!physical_) { LOG("no Vulkan GPU"); return false; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    VkSampleCountFlags ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples_ = VK_SAMPLE_COUNT_1_BIT;
    for (VkSampleCountFlagBits c : {VK_SAMPLE_COUNT_8_BIT, VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_2_BIT})
        if (static_cast<int>(c) <= msaa && (ok & c)) { samples_ = c; break; }
    maxAniso_ = props.limits.maxSamplerAnisotropy;
    maxImageDim_ = props.limits.maxImageDimension2D;
    maxCubeDim_ = props.limits.maxImageDimensionCube;

    // Meta's shaders use float16/int8/16-bit storage and multiview: enable every core feature the GPU has.
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    // HDR ASTC (the lightmaps and panoramas, kept compressed): an extension on Vulkan 1.2.
    VkPhysicalDeviceTextureCompressionASTCHDRFeaturesEXT fHdr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES_EXT};
    bool hasHdrExt = false, hasFdm = false, hasFdmOffset = false;
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT fFdm = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM fFdmOff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_QCOM};
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, exts.data());
        for (const auto& e : exts) {
            hasHdrExt |= std::strcmp(e.extensionName, VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME) == 0;
            hasFdm |= std::strcmp(e.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0;
            hasFdmOffset |= std::strcmp(e.extensionName, VK_QCOM_FRAGMENT_DENSITY_MAP_OFFSET_EXTENSION_NAME) == 0;
        }
    }
    // The optional features asked about, chained after the 1.2 ones (only those the device has extensions for).
    auto chainQuery = [&]() {
        void** next = &f12.pNext;
        *next = nullptr;
        if (hasHdrExt) { *next = &fHdr; next = &fHdr.pNext; *next = nullptr; }
        if (foveation && hasFdm) { *next = &fFdm; next = &fFdm.pNext; *next = nullptr; }
        if (foveation && hasFdm && hasFdmOffset) { *next = &fFdmOff; next = &fFdmOff.pNext; *next = nullptr; }
    };
    chainQuery();
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(physical_, &f2);
    anisotropy_ = f2.features.samplerAnisotropy == VK_TRUE;
    wireOk_ = f2.features.fillModeNonSolid == VK_TRUE;   // (every feature the device has is enabled with f2)
    // Tile-based GPUs (Qualcomm, Arm, Imagination, Apple) shade after all draws are binned: a timestamp around one
    // draw sees only its binning, so the Cost view measures them by drawing a mesh twice instead.
    tiler_ = props.vendorID == 0x5143 || props.vendorID == 0x13B5 || props.vendorID == 0x1010 || props.vendorID == 0x106B;
    astcLdr_ = f2.features.textureCompressionASTC_LDR == VK_TRUE && !(std::getenv("FH_NO_ASTC") && std::string(std::getenv("FH_NO_ASTC")) == "1");
    astcHdr_ = astcLdr_ && hasHdrExt && fHdr.textureCompressionASTC_HDR == VK_TRUE;
    // Rendering straight into the compositor's images (exportDirect), the map itself moves with the gaze (rewritten each
    // frame): the images then need no density-map-offset flag, which the compositor's device couldn't import.
    fdmOffset_ = !exportDirect && hasFdmOffset && fFdmOff.fragmentDensityMapOffset;
    fdmOn_ = foveation && hasFdm && fFdm.fragmentDensityMap && fFdm.fragmentDensityMapNonSubsampledImages && (exportDirect || fdmOffset_);
    if (!fdmOn_) fdmOffset_ = false;
    if (fdmOn_) {   // its texel size and how finely the map can be moved
        VkPhysicalDeviceFragmentDensityMapPropertiesEXT pf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
        VkPhysicalDeviceFragmentDensityMapOffsetPropertiesQCOM po = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_PROPERTIES_QCOM};
        pf.pNext = &po;
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &pf;
        vkGetPhysicalDeviceProperties2(physical_, &p2);
        fdmTexel_ = std::max(32u, pf.minFragmentDensityTexelSize.width);
        // A density map format the driver takes (Turnip: only 16- or 32-bit float pairs, not R8G8_UNORM).
        fdmFormat_ = VK_FORMAT_UNDEFINED;
        for (VkFormat f : {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32G32_SFLOAT}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(physical_, f, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) { fdmFormat_ = f; break; }
        }
        if (fdmFormat_ == VK_FORMAT_UNDEFINED) fdmOn_ = false;
        fdmGranularity_ = std::max(1u, po.fragmentDensityOffsetGranularity.width);
        fdmGranularityY_ = std::max(1u, po.fragmentDensityOffsetGranularity.height);
        fFdm.fragmentDensityMapDynamic = VK_FALSE;
        fFdm.fragmentDensityMapNonSubsampledImages = VK_TRUE;
    }
    // What gets enabled: only the optional features in use.
    hasHdrExt = astcHdr_;
    if (!fdmOn_) foveation = false;
    hasFdmOffset = fdmOffset_;                   // (its features only chained when its extension is enabled)
    chainQuery();
    if (!f11.multiview) LOG("warning: GPU lacks multiview");
    LOG("GPU %s, eye %ux%u, %ux MSAA, float16=%d int8=%d, ASTC textures %s", props.deviceName, width_, height_,
        static_cast<unsigned>(samples_), f12.shaderFloat16, f12.shaderInt8, astcHdr_ ? "native (HDR too)" : astcLdr_ ? "native" : "decoded in the background");
    if (fdmOn_ && fdmOffset_) LOG("foveated rendering on (density texel %u px, offsets in steps of %u px)", fdmTexel_, fdmGranularity_);
    else if (fdmOn_) LOG("foveated rendering on (density texel %u px, the map redrawn where you look each frame)", fdmTexel_);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, qf.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queueFamily_ = i; break; }
    if (queueFamily_ == UINT32_MAX) { LOG("no graphics queue"); return false; }

    std::vector<const char*> dext;
    for (auto& e : deviceExtensions) dext.push_back(e.c_str());
    {   // a portability device (MoltenVK) must say it knows it is one
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, have.data());
        for (const VkExtensionProperties& e : have)
            if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset")) dext.push_back("VK_KHR_portability_subset");
    }
    if (astcHdr_) dext.push_back(VK_EXT_TEXTURE_COMPRESSION_ASTC_HDR_EXTENSION_NAME);
    if (fdmOn_) dext.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    if (fdmOffset_) dext.push_back(VK_QCOM_FRAGMENT_DENSITY_MAP_OFFSET_EXTENSION_NAME);
#ifdef _WIN32
    // Windows system mode: import the compositor's Direct3D 11 textures to render the home into them.
    if (importWin32)
        for (const char* e : {"VK_KHR_external_memory", "VK_KHR_external_memory_win32"})
            if (std::none_of(dext.begin(), dext.end(), [&](const char* d) { return !std::strcmp(d, e); })) dext.push_back(e);
#endif
    // FH_PERF: the GPU's counters, for the bench (see perfCounterAverages).
    std::vector<uint32_t> perfIdx;
    VkPhysicalDevicePerformanceQueryFeaturesKHR perfFeat = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR};
    if (const char* want = std::getenv("FH_PERF")) {
        auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR>(
            vkGetInstanceProcAddr(instance_, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR"));
        uint32_t n = 0;
        if (enumerate && enumerate(physical_, queueFamily_, &n, nullptr, nullptr) == VK_SUCCESS && n) {
            std::vector<VkPerformanceCounterKHR> cs(n, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR});
            std::vector<VkPerformanceCounterDescriptionKHR> ds(n, {VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR});
            enumerate(physical_, queueFamily_, &n, cs.data(), ds.data());
            for (uint32_t i = 0; i < n; ++i) {
                const std::string name = ds[i].name;
                if (std::string(want) == "list") { LOG("perf counter %u: %s (%s)", i, ds[i].name, ds[i].category); continue; }
                for (std::string rest = want; !rest.empty();) {
                    const size_t c = rest.find(',');
                    const std::string part = rest.substr(0, c);
                    if (!part.empty() && name.find(part) != std::string::npos) {
                        perfIdx.push_back(i); perfNames_.push_back(name); perfStorage_.push_back(cs[i].storage);
                        break;
                    }
                    rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
                }
            }
            auto passes = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR>(
                vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR"));
            while (!perfIdx.empty() && passes) {     // only what one pass can count
                VkQueryPoolPerformanceCreateInfoKHR pc = {VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR};
                pc.queueFamilyIndex = queueFamily_;
                pc.counterIndexCount = static_cast<uint32_t>(perfIdx.size());
                pc.pCounterIndices = perfIdx.data();
                uint32_t np = 0;
                passes(physical_, &pc, &np);
                if (np <= 1) break;
                LOG("perf: %s left out (more than one pass)", perfNames_.back().c_str());
                perfIdx.pop_back(); perfNames_.pop_back(); perfStorage_.pop_back();
            }
        } else {
            LOG("perf: no counters (on Turnip, TU_DEBUG=perfc)");
        }
        if (!perfIdx.empty()) {
            dext.push_back(VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME);
            perfFeat.performanceCounterQueryPools = VK_TRUE;
            perfFeat.pNext = f2.pNext;
            f2.pNext = &perfFeat;
        }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dext.size());
    dci.ppEnabledExtensionNames = dext.data();
    if (!VK_OK(vkCreateDevice(physical_, &dci, nullptr, &device_))) return false;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
#ifdef _WIN32
    if (importWin32) {   // the adapter this device is on, so the compositor can check it's the one it draws with
        VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        pp.pNext = &idp;
        vkGetPhysicalDeviceProperties2(physical_, &pp);
        if (idp.deviceLUIDValid) std::memcpy(deviceLUID_, idp.deviceLUID, 8);
    }
#endif
    if (!perfIdx.empty()) {
        VkQueryPoolPerformanceCreateInfoKHR pc = {VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR};
        pc.queueFamilyIndex = queueFamily_;
        pc.counterIndexCount = static_cast<uint32_t>(perfIdx.size());
        pc.pCounterIndices = perfIdx.data();
        VkQueryPoolCreateInfo qp = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.pNext = &pc;
        qp.queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR;
        qp.queryCount = 1;
        auto lock = reinterpret_cast<PFN_vkAcquireProfilingLockKHR>(vkGetDeviceProcAddr(device_, "vkAcquireProfilingLockKHR"));
        VkAcquireProfilingLockInfoKHR li = {VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR};
        li.timeout = UINT64_MAX;
        if (lock && lock(device_, &li) == VK_SUCCESS && vkCreateQueryPool(device_, &qp, nullptr, &perfPool_) == VK_SUCCESS) {
            perfSum_.assign(perfNames_.size(), 0.0);
            LOG("perf: counting %zu GPU counters", perfNames_.size());
        } else {
            perfPool_ = VK_NULL_HANDLE;
            LOG("perf: counters unavailable");
        }
    }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 2;
    vkAllocateCommandBuffers(device_, &ai, cmds_);
    cmd_ = cmds_[0];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fci, nullptr, &fence_);
    if (f12.timelineSemaphore) {                 // enabled with the rest of the 1.2 features
        VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tci.initialValue = 0;
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &tci;
        if (vkCreateSemaphore(device_, &sci, nullptr, &timeline_) != VK_SUCCESS) timeline_ = VK_NULL_HANDLE;
    }

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32768},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 16384;
    dpci.poolSizeCount = static_cast<uint32_t>(sizeof(sizes) / sizeof(sizes[0]));
    dpci.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_);

    {   // Pipeline cache: compiled shader pipelines persist across runs.
        std::vector<char> data;
        if (!pipelineCachePath.empty()) {
            if (FILE* f = texcache::open(std::filesystem::u8path(pipelineCachePath), false)) {   // (UTF-8: wide on Windows)
                std::fseek(f, 0, SEEK_END);
                const long n = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (n > 0) { data.resize(static_cast<size_t>(n)); if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear(); }
                std::fclose(f);
            }
        }
        VkPipelineCacheCreateInfo pc = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pc.initialDataSize = data.size();
        pc.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_) != VK_SUCCESS) {
            pc.initialDataSize = 0;              // stale / other driver: start empty
            pc.pInitialData = nullptr;
            vkCreatePipelineCache(device_, &pc, nullptr, &pipelineCache_);
        }
    }

    createTargets();
    createSharedResources();
    createGuidePipelines();
    createSplatPipeline();
    ring_ = createBuffer(kRingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    // Per-draw GPU timestamps: a slow frame names the objects that cost it.
    if (props.limits.timestampComputeAndGraphics && props.limits.timestampPeriod > 0 && !std::getenv("FH_NO_PROF")) {
        timestampPeriod_ = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qp = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = kMaxQueries;
        vkCreateQueryPool(device_, &qp, nullptr, &queryPool_);
    }
    startWorkers();
    return true;
}

// Multiview stereo target: 2-layer MSAA colour + depth, resolved into the 2-layer image the compositor reads.
void HslRenderer::createTargets() {
    const bool ms = samples_ != VK_SAMPLE_COUNT_1_BIT;
    const VkFormat color = VK_FORMAT_R8G8B8A8_SRGB;
    // With foveation every attachment of the pass must allow the density map to be moved.
    const VkImageCreateFlags fdmFlag = fdmOffset_ ? VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_QCOM : 0;
    static const bool blocky = std::getenv("FH_BLOCKY_FOV") != nullptr;   // A/B: the coarse regions stretched, as before
    smoothFov_ = fdmOn_ && !blocky;
    const VkImageCreateFlags subFlag = smoothFov_ ? VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT : 0;
    if (smoothFov_)
        sub_ = createImage2D(width_, height_, viewCount, 1, VK_SAMPLE_COUNT_1_BIT, color,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_VIEW_TYPE_2D_ARRAY, fdmFlag | subFlag);
    resolve_ = createImage2D(width_, height_, viewCount, 1, VK_SAMPLE_COUNT_1_BIT, color,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | fdmFlag);
    for (uint32_t l = 0; l < viewCount; ++l) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = resolve_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, l, 1};
        vkCreateImageView(device_, &vi, nullptr, &layerViews_[l]);
    }
    if (ms) colorMs_ = createImage2D(width_, height_, viewCount, 1, samples_, color,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, fdmFlag | subFlag);
    depth_ = createImage2D(width_, height_, viewCount, 1, samples_, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY, fdmFlag | subFlag);
    if (fdmOn_) {   // offset: twice the eye image each way, so moving it never uncovers an edge; else the eye image's size
        const uint32_t k = fdmOffset_ ? 2 : 1;
        fdmW_ = (k * width_ + fdmTexel_ - 1) / fdmTexel_;
        fdmH_ = (k * height_ + fdmTexel_ - 1) / fdmTexel_;
        fdmKey_ = -1;
        fdmInit_ = false;
        // One layer per view: with multiview the per-view offsets must match the map view's layer count.
        fdm_ = createImage2D(fdmW_, fdmH_, viewCount, 1, VK_SAMPLE_COUNT_1_BIT, fdmFormat_,
                             VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             VK_IMAGE_VIEW_TYPE_2D_ARRAY, fdmFlag);
        writeDensityMap();
    }

    // The pass, built for where its last colour attachment goes: TRANSFER_SRC for resolve_ (copied out, read back), or
    // GENERAL for the compositor's images (exportDirect: the frame lands in them, nothing copied). The two passes differ
    // only in that layout, so they're compatible: the same pipelines draw in both.
    auto makePass = [&](VkImageLayout finalColor, bool external) -> VkRenderPass {
    VkRenderPass made = VK_NULL_HANDLE;
    std::vector<VkAttachmentDescription> att;
    VkAttachmentDescription c = {};
    c.format = color;
    c.samples = samples_;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    c.storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    c.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    c.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    c.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    c.finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : finalColor;
    att.push_back(c);
    VkAttachmentDescription d = c;
    d.format = VK_FORMAT_D32_SFLOAT;
    d.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att.push_back(d);
    if (ms) {
        VkAttachmentDescription r = c;
        r.samples = VK_SAMPLE_COUNT_1_BIT;
        r.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        r.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        r.finalLayout = finalColor;
        att.push_back(r);
    }
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref = {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    sub.pResolveAttachments = ms ? &rref : nullptr;
    VkSubpassDependency dep = {};
    dep.srcSubpass = 0;
    dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // One dependency for every kind (copied out, read by the smoothing pass, or the compositor's): render passes are
    // compatible only when their dependencies are identical too, and the same pipelines draw in all of them
    // (different ones were VUID-vkCmdDrawIndexed-renderPass-02684 on every export-pass draw).
    (void)external;
    dep.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    const uint32_t viewMask = (1u << viewCount) - 1, correlation = viewMask;   // both eyes in one pass (VR), or one view (editor)
    VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &viewMask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &correlation;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.pNext = &mv;
    rp.attachmentCount = static_cast<uint32_t>(att.size());
    rp.pAttachments = att.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (fdmOn_) {
        // The same pass, as a RenderPass2 with the density map as its last attachment (the v1 structs can't name it).
        auto a2 = [](const VkAttachmentDescription& a) {
            VkAttachmentDescription2 o = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
            o.format = a.format; o.samples = a.samples; o.loadOp = a.loadOp; o.storeOp = a.storeOp;
            o.stencilLoadOp = a.stencilLoadOp; o.stencilStoreOp = a.stencilStoreOp; o.initialLayout = a.initialLayout; o.finalLayout = a.finalLayout;
            return o;
        };
        std::vector<VkAttachmentDescription2> att2;
        for (const auto& a : att) att2.push_back(a2(a));
        VkAttachmentDescription2 fd = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        fd.format = fdmFormat_;
        fd.samples = VK_SAMPLE_COUNT_1_BIT;
        fd.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        fd.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        fd.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fd.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        fd.initialLayout = fd.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        att2.push_back(fd);
        auto r2 = [](const VkAttachmentReference& r, VkImageAspectFlags aspect) {
            VkAttachmentReference2 o = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
            o.attachment = r.attachment; o.layout = r.layout; o.aspectMask = aspect;
            return o;
        };
        const VkAttachmentReference2 c2 = r2(cref, VK_IMAGE_ASPECT_COLOR_BIT), d2 = r2(dref, VK_IMAGE_ASPECT_DEPTH_BIT), rs2 = r2(rref, VK_IMAGE_ASPECT_COLOR_BIT);
        VkSubpassDescription2 sub2 = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        sub2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub2.viewMask = viewMask;
        sub2.colorAttachmentCount = 1;
        sub2.pColorAttachments = &c2;
        sub2.pDepthStencilAttachment = &d2;
        sub2.pResolveAttachments = ms ? &rs2 : nullptr;
        VkSubpassDependency2 dep2 = {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
        dep2.srcSubpass = dep.srcSubpass; dep2.dstSubpass = dep.dstSubpass;
        dep2.srcStageMask = dep.srcStageMask; dep2.dstStageMask = dep.dstStageMask;
        dep2.srcAccessMask = dep.srcAccessMask; dep2.dstAccessMask = dep.dstAccessMask;
        VkRenderPassFragmentDensityMapCreateInfoEXT fdmInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
        fdmInfo.fragmentDensityMapAttachment = {static_cast<uint32_t>(att2.size() - 1), VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
        VkRenderPassCreateInfo2 rp2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
        rp2.pNext = &fdmInfo;
        rp2.attachmentCount = static_cast<uint32_t>(att2.size());
        rp2.pAttachments = att2.data();
        rp2.subpassCount = 1;
        rp2.pSubpasses = &sub2;
        rp2.dependencyCount = 1;
        rp2.pDependencies = &dep2;
        rp2.correlatedViewMaskCount = 1;
        rp2.pCorrelatedViewMasks = &correlation;
        if (vkCreateRenderPass2(device_, &rp2, nullptr, &made) != VK_SUCCESS) {   // render without foveation
            LOG("foveated render pass failed: foveation off");
            made = VK_NULL_HANDLE;
            fdmOn_ = false;
            destroy(fdm_);
        }
    }
    if (!made) vkCreateRenderPass(device_, &rp, nullptr, &made);
    return made;
    };
    // (kept across resize(); smooth foveation: the foveated pass ends readable by the full-screen pass, and the frames
    // for the compositor come from that pass)
    if (!renderPass_) renderPass_ = makePass(smoothFov_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
    // (the export pass: the foveated pass itself, or with smooth foveation our smoothing pass, unless the compositor
    // smooths as it samples)
    if (exportDirect && !exportPass_)
        exportPass_ = smoothFov_ && !exportsSubsampled() ? colorOnlyPass(VK_IMAGE_LAYOUT_GENERAL) : makePass(VK_IMAGE_LAYOUT_GENERAL, true);
    if (smoothFov_ && !fdmOn_) {                 // the foveated pass couldn't be made: no subsampled targets either
        smoothFov_ = false;
        destroy(sub_);
    }
    const VkImageView out = smoothFov_ ? sub_.view : resolve_.view;
    std::vector<VkImageView> views = ms ? std::vector<VkImageView>{colorMs_.view, depth_.view, out}
                                        : std::vector<VkImageView>{out, depth_.view};
    if (fdmOn_) views.push_back(fdm_.view);
    VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = renderPass_;
    fb.attachmentCount = static_cast<uint32_t>(views.size());
    fb.pAttachments = views.data();
    fb.width = width_;
    fb.height = height_;
    fb.layers = 1;
    vkCreateFramebuffer(device_, &fb, nullptr, &framebuffer_);
    if (smoothFov_) {
        createFovPass();
        VkFramebufferCreateInfo ff = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ff.renderPass = fovPass_;
        ff.attachmentCount = 1;
        ff.pAttachments = &resolve_.view;
        ff.width = width_;
        ff.height = height_;
        ff.layers = 1;
        vkCreateFramebuffer(device_, &ff, nullptr, &fovFb_);
        VkDescriptorImageInfo ii = {VK_NULL_HANDLE, sub_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = fovSet_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        if (fovSet_) vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }
}

// A pass with one single-sample colour attachment over both views (multiview), cleared by nothing (every pixel is
// written), ending in finalLayout: the smooth-foveation pass's targets.
VkRenderPass HslRenderer::colorOnlyPass(VkImageLayout finalLayout) const {
    VkAttachmentDescription c = {};
    c.format = VK_FORMAT_R8G8B8A8_SRGB;
    c.samples = VK_SAMPLE_COUNT_1_BIT;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    c.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    c.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    c.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    c.finalLayout = finalLayout;
    VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;    // the foveated pass's writes before this one reads them, and the
    deps[0].dstSubpass = 0;                      // target's layout transition before this pass writes it
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    // (the same whichever the final layout: passes differing in a dependency aren't compatible, and one pipeline draws
    // in both kinds)
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    const uint32_t viewMask = (1u << viewCount) - 1;
    VkRenderPassMultiviewCreateInfo mv = {VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &viewMask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &viewMask;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.pNext = &mv;
    rp.attachmentCount = 1;
    rp.pAttachments = &c;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VkRenderPass made = VK_NULL_HANDLE;
    vkCreateRenderPass(device_, &rp, nullptr, &made);
    return made;
}

// The smooth-foveation pass's pipeline: tools/fovpass (one triangle; the frame read through the subsampled sampler,
// which must be immutable in its set layout).
void HslRenderer::createFovPass() {
    if (fovPipe_) return;
    if (!fovPass_) fovPass_ = colorOnlyPass(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkSamplerCreateInfo s = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT;
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &s, nullptr, &subSampler_);
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &subSampler_};
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    vkCreateDescriptorSetLayout(device_, &dl, nullptr, &fovSetLayout_);
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &fovSetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(device_, &pl, nullptr, &fovLayout_);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCreateDescriptorPool(device_, &dp, nullptr, &fovPool_);
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = fovPool_;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &fovSetLayout_;
    vkAllocateDescriptorSets(device_, &da, &fovSet_);
    auto module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule vs = module(kFovPassVert, sizeof kFovPassVert), fs = module(kFovPassFrag, sizeof kFovPassFrag);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb = {};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1; cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi; pci.layout = fovLayout_; pci.renderPass = fovPass_;
    if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &fovPipe_) != VK_SUCCESS) fovPipe_ = VK_NULL_HANDLE;
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    LOG("smooth foveation: %s", fovPipe_ ? "the coarse regions filtered (subsampled targets)" : "pipeline failed");
}

void HslRenderer::recordFovPass(VkFramebuffer fb, VkRenderPass pass) {
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass;
    rp.framebuffer = fb;
    rp.renderArea = {{0, 0}, {width_, height_}};
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp = {0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    const VkRect2D sc = {{0, 0}, {width_, height_}};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, fovPipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, fovLayout_, 0, 1, &fovSet_, 0, nullptr);
    const float inv[2] = {1.0f / width_, 1.0f / height_};
    vkCmdPushConstants(cmd_, fovLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8, inv);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
}

// The density map: full density in a circle round the middle, then half, then a quarter (per axis) toward the
// edges; stronger levels make the full-detail circle smaller. Distances are in eye-image widths, from the map's
// centre, which the per-eye offset puts where that eye looks.
// The density for each map texel (across, down), full detail around (cx, cy) (in map texels), then coarser step by
// step with distance: the pixel area halves at each of four rings, the vertical density first and then the horizontal
// (1x1, 1x2, 2x2, 2x4, 4x4 pixels), so no step doubles both at once and the falloff reads as a smooth blur, not bands.
// fovStrength_ places the rings: at 1, 2 and 3 the full-detail radius and the cost are those of the old three levels.
void HslRenderer::densityTexels(float cx, float cy, std::vector<float>& dens) const {
    static const float kFull[4] = {1.60f, 0.62f, 0.46f, 0.34f}, kHalf[4] = {2.20f, 0.95f, 0.74f, 0.58f};   // per level: full / old half radius
    const float st = std::clamp(fovStrength_, 0.0f, 3.0f);
    const int lo = std::min(2, static_cast<int>(st));
    const float t = st - lo;
    const float r0 = kFull[lo] + (kFull[lo + 1] - kFull[lo]) * t, rh = kHalf[lo] + (kHalf[lo + 1] - kHalf[lo]) * t;
    const float r4 = rh + (rh - r0) * 0.5f;           // the last ring: a quarter of the resolution each way past it
    dens.resize(static_cast<size_t>(fdmW_) * fdmH_ * 2);
    const float scale = static_cast<float>(fdmTexel_) / width_;   // texels -> image widths
    for (uint32_t y = 0; y < fdmH_; ++y)
        for (uint32_t x = 0; x < fdmW_; ++x) {
            const float dx = (x + 0.5f - cx) * scale, dy = (y + 0.5f - cy) * scale;   // round in pixels
            const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;   // 1 = half an image width away
            const int k = st <= 0.0f ? 0 : std::clamp(static_cast<int>((r - r0) / (r4 - r0) * 4.0f) + (r >= r0 ? 1 : 0), 0, 4);
            static const float kAcross[5] = {1, 1, 0.5f, 0.5f, 0.25f}, kDown[5] = {1, 0.5f, 0.5f, 0.25f, 0.25f};
            const size_t i = (static_cast<size_t>(y) * fdmW_ + x) * 2;
            dens[i] = kAcross[k];
            dens[i + 1] = kDown[k];
        }
}

void HslRenderer::packDensity(const std::vector<float>& dens, uint8_t* dst) const {
    const size_t texels = static_cast<size_t>(fdmW_) * fdmH_;
    const size_t bpt = fdmFormat_ == VK_FORMAT_R32G32_SFLOAT ? 8 : fdmFormat_ == VK_FORMAT_R16G16_SFLOAT ? 4 : 2;
    for (size_t i = 0; i < texels && i * 2 + 1 < dens.size(); ++i) {
        uint8_t* p = dst + i * bpt;
        const float a = dens[i * 2], d = dens[i * 2 + 1];
        if (bpt == 8) { std::memcpy(p, &a, 4); std::memcpy(p + 4, &d, 4); }
        else if (bpt == 4) { const uint16_t ha = toHalf(a), hd = toHalf(d); std::memcpy(p, &ha, 2); std::memcpy(p + 2, &hd, 2); }
        else { p[0] = static_cast<uint8_t>(std::lround(a * 255)); p[1] = static_cast<uint8_t>(std::lround(d * 255)); }
    }
}

void HslRenderer::writeDensityMap() {
    if (!fdmOn_ || !fdm_.image) return;
    if (!fdmOffset_) { fdmKey_ = -1; return; }   // (rewritten where you look by render(), each frame it changes)
    std::vector<float> dens;
    densityTexels(fdmW_ * 0.5f, fdmH_ * 0.5f, dens);
    // In the map's format (two channels: density across, density down), once per layer (view).
    const size_t texels = static_cast<size_t>(fdmW_) * fdmH_;
    const size_t bpt = fdmFormat_ == VK_FORMAT_R32G32_SFLOAT ? 8 : fdmFormat_ == VK_FORMAT_R16G16_SFLOAT ? 4 : 2;
    std::vector<uint8_t> px(texels * bpt * viewCount);
    for (uint32_t l = 0; l < viewCount; ++l) packDensity(dens, &px[l * texels * bpt]);
    Buffer staging = createBuffer(px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped, px.data(), px.size());
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy cp = {};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, viewCount};
        cp.imageExtent = {fdmW_, fdmH_, 1};
        vkCmdCopyBufferToImage(c, staging.buf, fdm_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        imageBarrier(c, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
    });
    retire(staging);
}

void HslRenderer::setFoveationLevel(int level) {
    level = std::clamp(level, 0, 3);
    fovLevel_ = level;
    setFoveationStrength(static_cast<float>(level));
}

void HslRenderer::setFoveationStrength(float strength) {
    strength = std::round(std::clamp(strength, 0.0f, 3.0f) * 32.0f) / 32.0f;
    if (strength == fovStrength_) return;
    fovStrength_ = strength;
    fovLevel_ = static_cast<int>(std::lround(strength));
    if (fdmOn_ && device_ && fdmOffset_) { vkDeviceWaitIdle(device_); writeDensityMap(); }
    fdmKey_ = -1;
}

// Samplers, fallback textures and the shared lighting buffers (values match the editor's tuned desktop path).
void HslRenderer::createSharedResources() {
    VkSamplerCreateInfo s = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    s.maxLod = VK_LOD_CLAMP_NONE;
    if (const char* e = std::getenv("FH_MIN_LOD")) s.minLod = static_cast<float>(std::atof(e));   // A/B: texture bandwidth
    vkCreateSampler(device_, &s, nullptr, &wrapSampler_);
    s.anisotropyEnable = anisotropy_ ? VK_TRUE : VK_FALSE;
    s.maxAnisotropy = anisotropy_ ? std::clamp(anisotropy, 1.0f, std::max(1.0f, maxAniso_)) : 1.0f;
    vkCreateSampler(device_, &s, nullptr, &anisoSampler_);
    VkSamplerCreateInfo sh = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sh.magFilter = sh.minFilter = VK_FILTER_LINEAR;
    sh.addressModeU = sh.addressModeV = sh.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sh.compareEnable = VK_TRUE;
    sh.compareOp = VK_COMPARE_OP_ALWAYS;        // no shadow pass: every comparison reads "lit"
    sh.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    vkCreateSampler(device_, &sh, nullptr, &shadowSampler_);

    const u8 white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255}, flatN[4] = {128, 128, 255, 255};
    white_ = createTexture(white, 1, 1, true);
    black_ = createTexture(black, 1, 1, true);
    const u8 none[4] = {0, 0, 0, 0};
    nullTex_ = createTexture(none, 1, 1, true);          // the VrShell null texture: black, transparent
    const u8 noMetal[4] = {255, 255, 255, 0};
    noMetal_ = createTexture(noMetal, 1, 1, true);
    flatNormal_ = createTexture(flatN, 1, 1, false);
    const float one[3] = {1, 1, 1};
    whiteCube_ = createSolidCube(one, one, one);
    white3D_ = createSolid3D(white);
    shadowMap_ = createShadowDepth();

    // Some homes' shaders declare these blocks as storage buffers (validation: VUID-VkWriteDescriptorSet-00331).
    globals_ = createBuffer(globalsSize_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    lightUniforms_ = createBuffer(2048, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    lightItems_ = createBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    lightGrid_ = createBuffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
}

// ── Programs ─────────────────────────────────────────────────────────────────────────────────────

int HslRenderer::programFor(const ShaderProgramSrc& src) {
    auto it = programIndex_.find(src.path);
    if (it != programIndex_.end()) return it->second;
    programIndex_[src.path] = -1;

    Program p;
    p.path = src.path;
    p.clay = p.path.rfind("builtin/clay", 0) == 0;
    if (const char* dir = std::getenv("FH_DUMP_SPV")) {   // diagnostics: every program's SPIR-V, <dir>/<shader>.vert|frag.spv
        std::string n = src.path;
        for (char& c : n) if (c == '/' || c == '\\' || c == ':') c = '_';
        for (const auto& [ext, code] : {std::pair<const char*, const std::vector<u32>*>{".vert.spv", &src.vert}, {".frag.spv", &src.frag}})
            if (FILE* f = std::fopen((std::string(dir) + "/" + n + ext).c_str(), "wb")) { std::fwrite(code->data(), 4, code->size(), f); std::fclose(f); }
    }
    spv::Module vr, fr;
    if (!spv::reflect(src.vert, vr) || !spv::reflect(src.frag, fr)) { LOG("reflect failed: %s", src.path.c_str()); return -1; }
    p.multiview = vr.multiview;

    // Vertex layout from the shader's own inputs, roles by name (the stream semantic names).
    uint32_t off = 0;
    std::vector<spv::Input> ins = vr.inputs;
    std::sort(ins.begin(), ins.end(), [](const spv::Input& a, const spv::Input& b) { return a.location < b.location; });
    for (const auto& in : ins) {
        std::string n = lower(in.name);
        VertexAttr a = {in.location, VK_FORMAT_UNDEFINED, 0, 0, Role::Zero, in.integer};
        if (has(n, "pos")) a.role = Role::Pos;
        else if (has(n, "norm")) a.role = Role::Normal;
        else if (has(n, "tangent")) a.role = Role::Tangent;
        else if (has(n, "lightmap") || has(n, "uv1")) a.role = Role::Uv1;
        else if (has(n, "uv2")) a.role = Role::Uv2;
        else if (has(n, "uv3")) a.role = Role::Uv3;
        else if (has(n, "uv") || has(n, "texcoord")) a.role = Role::Uv0;
        else if (has(n, "boneind") || has(n, "joint") || has(n, "indices")) a.role = Role::BoneIdx;
        else if (has(n, "bonewe") || has(n, "weight")) a.role = Role::BoneWgt;
        else if (has(n, "color") || has(n, "colour")) a.role = Role::Color;
        if (a.role == Role::Color || a.role == Role::BoneIdx || a.role == Role::BoneWgt) {
            a.format = in.integer ? VK_FORMAT_R8G8B8A8_UINT : VK_FORMAT_R8G8B8A8_UNORM;
            a.size = 4;
        } else if (in.integer) {
            static const VkFormat u[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
            a.format = u[std::min(in.components, 4u) - 1];
            a.size = 4 * in.components;
        } else if (in.width == 16) {
            static const VkFormat h[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
            a.format = h[std::min(in.components, 4u) - 1];
            a.size = 2 * in.components;
        } else {
            static const VkFormat f[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
            a.format = f[std::min(in.components, 4u) - 1];
            a.size = 4 * in.components;
        }
        a.offset = off;
        off += (a.size + 3) & ~3u;
        p.attrs.push_back(a);
    }
    p.stride = off ? off : 4;

    // Descriptor sets: union of both stages.
    shaderbake::mergedSets(vr, fr, p.sets);
    p.boned = shaderbake::boned(vr);
    for (int s = 0; s < 3; ++s) {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        for (const auto& r : p.sets[s])
            lb.push_back({r.binding, descriptorType(r.kind), r.count, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = static_cast<uint32_t>(lb.size());
        lci.pBindings = lb.data();
        vkCreateDescriptorSetLayout(device_, &lci, nullptr, &p.setLayouts[s]);
    }
    for (const auto& r : p.sets[0])
        if (r.binding == 0 && isBuffer(r.kind) && r.members.size() > globalMembers_.size()) globalMembers_ = r.members;
    for (const auto& r : p.sets[2])
        if (isBuffer(r.kind) && has(lower(r.name), "lightbaker")) p.lightBaker = true;

    p.pushSize = (std::max(vr.pushSize, fr.pushSize) + 3) & ~3u;
    for (const spv::Module* m : {&vr, &fr})
        for (const auto& pm : m->pushMembers) {
            std::string n = lower(pm.name);
            if (has(n, "worldfrommodel") && p.pcWorld < 0) p.pcWorld = static_cast<int>(pm.offset);
            else if (has(n, "color") && p.pcColor < 0) p.pcColor = static_cast<int>(pm.offset);
            else if (n == "uvtransformu") p.pcUvU = static_cast<int>(pm.offset);   // (the built-in Clay shader's)
            else if (n == "uvtransformv") p.pcUvV = static_cast<int>(pm.offset);
        }
    // Unnamed push block: the engine's fixed layout is pushConstants { mat4 worldFromModel @0; vec4 color @64 }.
    if (p.pcWorld < 0 && p.pushSize >= 64) p.pcWorld = 0;
    if (p.pcColor < 0 && p.pushSize >= 80) p.pcColor = 64;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, p.pushSize};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 3;
    plci.pSetLayouts = p.setLayouts;
    plci.pushConstantRangeCount = p.pushSize ? 1 : 0;
    plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device_, &plci, nullptr, &p.layout);

    // (through the SPIR-V optimizer first: spv_opt.h; a module a .fenv carries made is used as it is)
    auto module = [&](const std::vector<u32>& shipped) {
        const std::vector<u32> code = spvopt::run(shipped, p.path);
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        VkShaderModule sm = VK_NULL_HANDLE;
        // (a .fenv's SPIR-V is checked before the driver sees it: spv_check.h)
        if (code.empty() || !shaderOk(code, p.path) || vkCreateShaderModule(device_, &ci, nullptr, &sm) != VK_SUCCESS) sm = VK_NULL_HANDLE;
        return sm;
    };
    // (prepared by the bake for a .fenv: shader_bake.h)
    p.vs = module(src.prepared && shaderbake::bakedUsable(src.vertInvariant) ? src.vertInvariant : shaderbake::invariantPosition(src.vert));
    p.fs = module(src.frag);
    p.src = &src;
    p.foldBlocks = spvspec::blocksRead(src.frag);
    const std::vector<u32> alpha = src.prepared && shaderbake::bakedUsable(src.fragAlpha) ? src.fragAlpha : shaderbake::alphaOnly(src.frag);
    if (!alpha.empty()) p.fsAlpha = module(alpha);
    if (!p.vs || !p.fs) {                        // SPIR-V the driver rejects: this program's meshes are skipped
        LOG("shader module failed: %s", src.path.c_str());
        if (p.vs) vkDestroyShaderModule(device_, p.vs, nullptr);
        if (p.fs) vkDestroyShaderModule(device_, p.fs, nullptr);
        vkDestroyPipelineLayout(device_, p.layout, nullptr);
        for (auto l : p.setLayouts) vkDestroyDescriptorSetLayout(device_, l, nullptr);
        return -1;
    }

    VkDescriptorSetLayout l01[2] = {p.setLayouts[0], p.setLayouts[1]};
    VkDescriptorSet s01[2];
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = l01;
    vkAllocateDescriptorSets(device_, &ai, s01);
    p.set0 = s01[0];
    p.set1 = s01[1];

    LOG("program %s: %zu inputs (stride %u), sets %zu/%zu/%zu, push %u, multiview %d", src.path.c_str(),
        p.attrs.size(), p.stride, p.sets[0].size(), p.sets[1].size(), p.sets[2].size(), p.pushSize, p.multiview);
    for (const auto& a : p.attrs)
        LOG("    attr loc %u role %d fmt %d off %u size %u int %d", a.location, static_cast<int>(a.role), a.format, a.offset, a.size, a.integer);
    programs_.push_back(std::move(p));
    int idx = static_cast<int>(programs_.size()) - 1;
    programIndex_[src.path] = idx;
    writeSharedSets(programs_[idx]);
    return idx;
}

// set0 / set1 are the same for every mesh of a program: globals, lighting, IBL and shadow resources.
void HslRenderer::writeSharedSets(Program& p) {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::vector<VkDescriptorBufferInfo>> bufs;
    std::vector<std::vector<VkDescriptorImageInfo>> imgs;
    bufs.reserve(64);
    imgs.reserve(64);
    for (int s = 0; s < 2; ++s) {
        for (const auto& r : p.sets[s]) {
            std::string n = lower(r.name);
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = s == 0 ? p.set0 : p.set1;
            w.dstBinding = r.binding;
            w.descriptorCount = r.count;
            w.descriptorType = descriptorType(r.kind);
            if (isBuffer(r.kind)) {
                Buffer* b = &lightUniforms_;
                if (s == 0) b = &globals_;
                if (s == 1 && b == &lightUniforms_) {
                    static bool listed = false;
                    if (!listed) {
                        listed = true;
                        for (const auto& m : r.members) LOG("lightUniforms member %s @%u (%u bytes)", m.name.c_str(), m.offset, m.size);
                    }
                    for (const auto& m : r.members) {
                        const std::string mn = lower(m.name);
                        // HSR's derived lightUniforms: these five are 1 (byte 112-156, 168-172), all else zero.
                        const bool one = mn == "iblreflectiontint" || mn == "ibldiffusetint" || mn == "externalaverageluminance" ||
                                         mn == "iblreflectionmaxadaptationfactor" || mn == "iblreflectionluminanceinverse";
                        if (!one || m.integer || m.offset + m.size > lightUniforms_.size) continue;
                        float* lf = reinterpret_cast<float*>(static_cast<u8*>(lightUniforms_.mapped) + m.offset);
                        for (uint32_t k = 0; k * 4 < m.size; ++k) lf[k] = 1.0f;
                    }
                }
                else if (has(n, "item")) b = &lightItems_;
                else if (has(n, "grid") || r.kind == spv::ResKind::StorageBuffer) b = &lightGrid_;
                bufs.emplace_back(r.count, VkDescriptorBufferInfo{b->buf, 0, VK_WHOLE_SIZE});
                w.pBufferInfo = bufs.back().data();
            } else {
                VkDescriptorImageInfo ii = {};
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (r.kind == spv::ResKind::Sampler || r.kind == spv::ResKind::CombinedImageSampler)
                    ii.sampler = has(n, "shadow") ? shadowSampler_ : wrapSampler_;
                if (r.kind != spv::ResKind::Sampler) {
                    if (r.depthImage || has(n, "shadow")) ii.imageView = shadowMap_.view;
                    else if (r.dim == spv::ImageDim::Cube) ii.imageView = has(n, "diff") ? whiteCube_.view : iblReflection_.view;
                    else if (r.dim == spv::ImageDim::D3) ii.imageView = white3D_.view;
                    else ii.imageView = white_.view;
                }
                imgs.emplace_back(r.count, ii);
                w.pImageInfo = imgs.back().data();
            }
            if (r.kind == spv::ResKind::StorageImage) continue;
            writes.push_back(w);
        }
    }
    if (!writes.empty()) vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

VkPipeline HslRenderer::pipelineFor(Program& p, int blend, bool cull, bool sky, bool a2c, CutPass cut, int variant) {
    const uint32_t key = static_cast<uint32_t>(blend) * 2 + (cull ? 1 : 0) + (sky ? 16 : 0) + (a2c ? 32 : 0) + static_cast<uint32_t>(cut) * 64;
    Program::Variant* var = variant >= 0 && static_cast<size_t>(variant) < p.variants.size() ? &p.variants[variant] : nullptr;
    std::map<uint32_t, VkPipeline>& pipes = var ? var->pipelines : p.pipelines;
    auto it = pipes.find(key);
    if (it != pipes.end()) return it->second;

    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = p.vs; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].pName = "main";
    stages[1].module = var ? (cut == kCutDepth ? var->fsAlpha : var->fs) : (cut == kCutDepth ? p.fsAlpha : p.fs);
    if (!stages[1].module) return VK_NULL_HANDLE;
    VkVertexInputBindingDescription vb = {0, p.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> va;
    for (const auto& a : p.attrs) va.push_back({a.location, 0, a.format, a.offset});
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(va.size());
    vi.pVertexAttributeDescriptions = va.data();
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // Meta's meshes wind front faces clockwise in Vulkan clip space (its projection flips Y). Shaders that light
    // back faces differently (gl_FrontFacing: the palms' backfaceDarken/translucency) depend on this.
    rs.frontFace = std::getenv("FH_CCW") ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo msi = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msi.rasterizationSamples = samples_;
    // Cut-out foliage: the SHAD pass state enables alpha-to-coverage for the animvege surfaces (HSR Studio's
    // world packets: exactly those pipelines). Matched by path until the SHAD pass-state table is decoded.
    msi.alphaToCoverageEnable = samples_ != VK_SAMPLE_COUNT_1_BIT && blend == 0 && (a2c || has(lower(p.path), "animvege")) ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // The sky goes behind everything. Meta's skybox shader places it at the clip-space depth the native renderer
    // expects, which is not ours, so its depth comes from the viewport instead: drawn after the opaque meshes with
    // the depth range pinned to the far plane (0, reversed-Z) it fills exactly the samples nothing covers, and the
    // covered ones are rejected before shading (drawn first with no depth test, it was shaded across the whole view).
    dsi.depthTestEnable = sky && skyFirst() ? VK_FALSE : VK_TRUE;
    dsi.depthWriteEnable = blend == 0 && !sky && cut != kCutShade ? VK_TRUE : VK_FALSE;
    dsi.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;     // reversed-Z: clear 0, near -> 1
    // A cut-out's shading pass: the samples whose depth is its own (the depth pass wrote it there through the same
    // alpha-to-coverage mask, and nothing nearer covered it) are exactly the ones alpha-to-coverage with GREATER_OR_EQUAL
    // let through, so EQUAL alone picks them: the same picture, and without alpha-to-coverage the GPU tests depth
    // before shading (Turnip ran it after: every frond layer was shaded). Desert Terrace 2-4% less GPU time.
    // FH_CUT_GE=1: the old way (A/B).
    static const bool cutEqual = std::getenv("FH_CUT_GE") == nullptr;
    if (cutEqual && cut == kCutShade && msi.alphaToCoverageEnable) {
        msi.alphaToCoverageEnable = VK_FALSE;
        dsi.depthCompareOp = VK_COMPARE_OP_EQUAL;
    }
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = cut == kCutDepth ? 0 : 0xF;
    if (blend != 0) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = blend == 2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = blend == 2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vps;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &msi;
    gci.pDepthStencilState = &dsi;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &ds;
    gci.layout = p.layout;
    gci.renderPass = renderPass_;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &gci, nullptr, &pipe);
    if (r != VK_SUCCESS) LOG("pipeline for %s failed: %d", p.path.c_str(), r);
    pipes[key] = pipe;
    return pipe;
}

// ── Meshes ───────────────────────────────────────────────────────────────────────────────────────


VkImageView HslRenderer::textureFor(const spv::Resource& r, GpuMesh& gm, const MeshData& md) {
    // Content-deduplicated upload: many meshes share one material texture (each MeshData holds its own copy).
    auto uploadRaw = [&](const void* px, size_t bytes, u32 w, u32 h, VkFormat fmt) -> VkImageView {
        const size_t bpp = fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        if (!px || !w || !h || bytes / bpp / w < h) return white_.view;   // (divided: w * h can overflow)
        // Hash ALL of it: sampling every Nth byte made mostly-flat maps (normals, ORM) of the same size collide,
        // so one mesh got another's texture.
        uint64_t key = 1469598103934665603ull ^ (static_cast<uint64_t>(w) << 32 | h) ^ (static_cast<uint64_t>(fmt) * 0x9E3779B97F4A7C15ull);
        const u8* b = static_cast<const u8*>(px);
        size_t i = 0;
        for (; i + 8 <= bytes; i += 8) {
            uint64_t v;
            std::memcpy(&v, b + i, 8);
            key = (key ^ v) * 0x100000001B3ull;
            key ^= key >> 29;
        }
        for (; i < bytes; ++i) key = (key ^ b[i]) * 1099511628211ull;
        auto it = textureCache_.find(key);
        if (it != textureCache_.end()) return it->second.view;
        Image img = createTextureFmt(px, w, h, fmt);
        if (!img.view) return white_.view;
        textureCache_[key] = img;
        return img.view;
    };
    auto upload = [&](const std::vector<u8>& rgba, u32 w, u32 h, bool srgb) -> VkImageView {
        return uploadRaw(rgba.data(), rgba.size(), w, h, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    };
    // Baked HDR lightmaps are shared atlases (up to 4096^2): one upload per atlas, keyed by identity — hashing
    // 128 MB again for every mesh that samples it froze loading for seconds.
    auto uploadLm = [&]() -> VkImageView { return hdrTexture(md.lmHalf.get(), md.lmSrc, md.lmW, md.lmH); };
    // Clay's 8-bit lightmaps: shared and still compressed (streamed like base maps), else the decoded pixels.
    auto uploadLdrLm = [&]() -> VkImageView {
        if (!md.lmLdrSrc) return upload(md.lmRGBA, md.lmW, md.lmH, true);
        VkImageView v = streamTexture(md.lmLdrSrc, true, white_.view);
        if (slotPending_) gm.wants.push_back(pendingKey_);
        return v;
    };
    const std::string n = lower(r.name);
    if (r.dim == spv::ImageDim::Cube) {
        // The skybox's own colour cubemap; otherwise the shared IBL cubes.
        if (md.cubeSize && !has(n, "diff") && !has(n, "refl")) {
            if (md.cubeSrc) {                    // kept as ASTC: sampled as-is, or decoded here (PC)
                const VkFormat fmt = astcLdr_ ? astcFormat(md.cubeSrc->bw, md.cubeSrc->bh, true) : VK_FORMAT_UNDEFINED;
                std::vector<u8> faces;
                if (fmt == VK_FORMAT_UNDEFINED &&
                    !astc::decodeCubeFaces(md.cubeSrc->astc.data(), static_cast<u32>(md.cubeSrc->astc.size()), md.cubeSize,
                                           md.cubeSrc->bw, md.cubeSrc->bh, faces))
                    return whiteCube_.view;
                Image img = fmt != VK_FORMAT_UNDEFINED ? createAstcTexture(*md.cubeSrc, fmt)
                                                       : createCubeTexture(faces.data(), faces.size(), md.cubeSize);
                if (!img.view) return whiteCube_.view;
                gm.images.push_back(img);
                return img.view;
            }
            Image img = createCubeTexture(md.cubeRGBA.data(), md.cubeRGBA.size(), md.cubeSize);
            if (!img.view) return whiteCube_.view;
            gm.images.push_back(img);
            return img.view;
        }
        return has(n, "diff") ? whiteCube_.view : iblReflection_.view;   // HSR: diffuse IBL = neutral white
    }
    if (r.dim == spv::ImageDim::D3) return white3D_.view;
    // Vertex-animation texture: the loader's per-vertex offsets per frame (half floats). A VAT the material binds by
    // sampler name instead (Cascadia's rigid-piece centerpiece/tram: position + rotation per piece per frame) is
    // found by hash below.
    if (has(n, "vat") && md.hasVat && md.vatRaw.size() >= static_cast<size_t>(md.vatW) * md.vatH * 8) {
        if (debugMesh(md)) {
            auto texel = [&](u32 x, u32 y) {
                std::string o;
                for (int c = 0; c < 4; ++c) {
                    uint16_t h; std::memcpy(&h, &md.vatRaw[(static_cast<size_t>(y) * md.vatW + x) * 8 + c * 2], 2);
                    char b[24]; std::snprintf(b, sizeof b, "%.4g ", fromHalf(h)); o += b;
                }
                return o;
            };
            LOG("  set2 '%s' <- VAT %ux%u half floats; texel (0,0) %s (0,1) %s", r.name.c_str(), md.vatW, md.vatH, texel(0, 0).c_str(),
                md.vatH > 1 ? texel(0, 1).c_str() : "-");
            if (md.vatW > 26 && md.vatH > 1) LOG("    (26,0) %s (26,1) %s", texel(26, 0).c_str(), texel(26, 1).c_str());
        }
        Image img = createTextureFmt(md.vatRaw.data(), md.vatW, md.vatH, VK_FORMAT_R16G16B16A16_SFLOAT);
        if (!img.view) return black_.view;       // zero offset = rest pose
        gm.images.push_back(img);
        return img.view;
    }
    if (r.depthImage || has(n, "shadow")) return shadowMap_.view;
    // The baked lightmap comes from the entity's lightingParams (it outranks the material's placeholder,
    // renderer_module/missing_lightmap.png, which is bound by hash otherwise).
    if (has(n, "lightmap") && md.hasLmHdr())
        return uploadLm();
    // Device-faithful: the material texture whose sampler-name hash matches this binding.
    {
        const u32 h = murmur3_x86_32(r.name.data(), r.name.size(), 0);
        for (const auto& nt : md.namedTex) {
            if (nt.first != h || !nt.second) continue;
            const bool linear = has(n, "normal") || has(n, "onxrny") || has(n, "orm") || has(n, "rbao") || has(n, "mask") ||
                                has(n, "noise") || has(n, "rough") || has(n, "metal") || has(n, "flow") || has(n, "_ao");
            VkImageView ph = has(n, "emissive") ? black_.view : (has(n, "normal") || has(n, "onxrny")) ? flatNormal_.view
                           : has(n, "metallic") ? noMetal_.view : white_.view;
            if (debugMesh(md))
                LOG("  set2 '%s' <- texture %ux%u, %ux%u blocks, %u mips, hdr %d, %zu bytes, %s", r.name.c_str(), nt.second->w, nt.second->h, nt.second->bw,
                    nt.second->bh, nt.second->mips, (int)nt.second->hdr, nt.second->astc.size(), linear ? "linear" : "sRGB");
            VkImageView v = streamTexture(nt.second, !linear, ph);
            if (slotPending_) gm.wants.push_back(pendingKey_);
            return v;
        }
    }
    {   // HDR maps the material binds by sampler name (reflection panoramas): RGBA16F, one upload per image.
        const u32 h = murmur3_x86_32(r.name.data(), r.name.size(), 0);
        for (const auto& nh : md.namedHdr)
            if (nh.first == h && (nh.second.half || nh.second.src)) {
                if (debugMesh(md))
                    LOG("  set2 '%s' <- HDR %ux%u (%s)", r.name.c_str(), nh.second.w, nh.second.h, nh.second.half ? "half floats" : "ASTC source");
                return hdrTexture(nh.second.half.get(), nh.second.src, nh.second.w, nh.second.h, has(n, "vat"));
            }
    }
    if (debugMesh(md)) {
        std::string have;
        for (const auto& nt : md.namedTex) have += " tex:" + std::to_string(nt.first);
        for (const auto& nh : md.namedHdr) have += " hdr:" + std::to_string(nh.first);
        LOG("  set2 '%s' (hash %u) has no texture; the mesh has%s", r.name.c_str(), murmur3_x86_32(r.name.data(), r.name.size(), 0), have.c_str());
    }
    if (has(n, "vat")) return black_.view;       // zero offset = rest pose
    {   // HSR binds an authored texture to every material slot: any slot reaching here is a loader miss
        static int misses = 0;
        if (misses < 60 && !md.namedTex.empty()) {
            ++misses;
            LOG("texture miss: '%s' slot '%s' (namedTex %zu, shader %s)", md.name.c_str(), r.name.c_str(), md.namedTex.size(), md.shaderPath.c_str());
        }
    }
    // HSR Studio's rule for a sampler the material does not bind: the VrShell null texture (black, transparent),
    // or neutral white when the material authors no textures at all. Runtime-provided maps come first.
    // Only for materials that bind textures by sampler name (v206+). Older homes (Cascadia's clay_shaders) carry
    // no sampler-name table: their textures come by role below.
    if (!md.namedTex.empty()) {
        if (has(n, "lightmap")) {
            if (md.hasLmHdr()) return uploadLm();
            if (md.hasLightmap) return uploadLdrLm();
            return white_.view;                  // no baked light shipped for this mesh: neutral, never black
        }
        return nullTex_.view;
    }
    if (has(n, "lightmap") && md.hasLmHdr())
        return uploadLm();
    if (!md.hasTexture) return white_.view;
    const bool isBase = has(n, "basecolor") || has(n, "albedo") || has(n, "diffuse");
    // RBAoDir (baked light: R, B, AO, light dir; sampled with UV2) is its own slot next to ONxRNy.
    const bool isRbao = has(n, "rbao");
    const bool isNorm = !isRbao && (has(n, "onxrny") || has(n, "normal") || has(n, "_nx"));
    const bool isLm = has(n, "lightmap");
    const bool isOrm = has(n, "orm") || has(n, "rough") || has(n, "metal") || has(n, "_ao") || has(n, "occl");
    const bool isEmissive = has(n, "emissive");
    // A map either arrived decoded (RGBA) or deferred (still-compressed TexSource, streamed in).
    auto map = [&](const std::shared_ptr<const TexSource>& src, const std::vector<u8>& rgba, u32 w, u32 h, bool srgb,
                   VkImageView placeholder) -> VkImageView {
        if (src) {
            VkImageView v = streamTexture(src, srgb, placeholder);
            if (slotPending_) gm.wants.push_back(pendingKey_);     // decoded once this mesh is in view
            return v;
        }
        return upload(rgba, w, h, srgb);
    };
    VkImageView v = VK_NULL_HANDLE;
    if (isRbao) v = md.hasOrm ? map(md.ormSrc, md.ormRGBA, md.ormW, md.ormH, false, white_.view) : white_.view;
    else if (isEmissive) v = md.hasEmissive ? map(md.emissiveSrc, md.emissiveRGBA, md.emissiveW, md.emissiveH, true, black_.view) : black_.view;
    else if (isNorm) v = md.hasNormal ? map(md.normalSrc, md.normalRGBA, md.normalW, md.normalH, false, flatNormal_.view)
                       : md.hasOrm ? map(md.ormSrc, md.ormRGBA, md.ormW, md.ormH, false, flatNormal_.view) : flatNormal_.view;
    else if (isLm) {
        if (md.hasLmHdr()) v = uploadLm();
        else if (md.hasLightmap) v = uploadLdrLm();
        else v = white_.view;
    }
    else if (isBase || has(n, "noise")) v = md.hasTexture ? map(md.texSrc, md.texRGBA, md.texW, md.texH, true, white_.view) : white_.view;
    else if (isOrm) v = md.hasOrm ? map(md.ormSrc, md.ormRGBA, md.ormW, md.ormH, false, white_.view) : white_.view;
    return v ? v : white_.view;
}

// ── Texture streaming ────────────────────────────────────────────────────────────────────────────

VkFormat HslRenderer::astcFormat(u32 bw, u32 bh, bool srgb) const {
    static const struct { u32 w, h; VkFormat unorm, srgb; } kFormats[] = {
        {4, 4, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
        {5, 4, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_SRGB_BLOCK},
        {5, 5, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_SRGB_BLOCK},
        {6, 5, VK_FORMAT_ASTC_6x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_SRGB_BLOCK},
        {6, 6, VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_6x6_SRGB_BLOCK},
        {8, 5, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_SRGB_BLOCK},
        {8, 6, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_SRGB_BLOCK},
        {8, 8, VK_FORMAT_ASTC_8x8_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_SRGB_BLOCK},
        {10, 5, VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x5_SRGB_BLOCK},
        {10, 6, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_SRGB_BLOCK},
        {10, 8, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_SRGB_BLOCK},
        {10, 10, VK_FORMAT_ASTC_10x10_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_SRGB_BLOCK},
        {12, 10, VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x10_SRGB_BLOCK},
        {12, 12, VK_FORMAT_ASTC_12x12_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_SRGB_BLOCK},
    };
    for (const auto& f : kFormats)
        if (f.w == bw && f.h == bh) return srgb ? f.srgb : f.unorm;
    return VK_FORMAT_UNDEFINED;
}

// The cooked ASTC chain uploaded as-is: no CPU decode at all (GPUs with textureCompressionASTC_LDR).
HslRenderer::Image HslRenderer::createAstcTexture(const TexSource& src, VkFormat fmt) {
    const bool cube = src.faces == 6;
    const uint32_t faces = cube ? 6 : 1;
    if (!src.bw || !src.bh || src.astc.empty()) return Image{};
    // A chain longer than the image's own (from a damaged file) is cut to the levels the image has.
    uint32_t chain = 1;
    while (chain < 32 && (std::max(src.w, src.h) >> chain)) ++chain;
    const uint32_t mips = std::min(std::max(1u, src.mips), chain);
    Image img = createImage2D(src.w, src.h, faces, mips, VK_SAMPLE_COUNT_1_BIT, fmt,
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                              cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
    if (!img.image) return Image{};
    Buffer staging = createBuffer(src.astc.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    if (!staging.buf) { destroy(img); return Image{}; }
    std::memcpy(staging.mapped, src.astc.data(), src.astc.size());
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = std::max(1u, src.w >> m), h = std::max(1u, src.h >> m);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>((w + src.bw - 1) / src.bw) * ((h + src.bh - 1) / src.bh) * 16;
        if (off + bytes * faces > src.astc.size()) break;
        for (uint32_t f = 0; f < faces; ++f) {
            VkBufferImageCopy c = {};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, f, 1};
            c.imageExtent = {w, h, 1};
            copies.push_back(c);
            off += bytes;
        }
    }
    oneShot([&](VkCommandBuffer c) {
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, faces, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(c, staging.buf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copies.size()), copies.data());
        imageBarrier(c, img.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    retire(staging);
    return img;
}

VkImageView HslRenderer::hdrTexture(const std::vector<u16>* half, const std::shared_ptr<const TexSource>& src, u32 w, u32 h, bool exact) {
    const uint64_t key = reinterpret_cast<uintptr_t>(half ? static_cast<const void*>(half) : src.get()) * 2 + 1;
    auto it = textureCache_.find(key);
    if (it != textureCache_.end()) return it->second.view;
    if (!half && src && astcHdr_) {
        const VkFormat unorm = astcFormat(src->bw, src->bh, false);
        if (unorm != VK_FORMAT_UNDEFINED) {      // the SFLOAT block formats follow the UNORM table's order
            const VkFormat fmt = static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT + (unorm - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2);
            Image img = createAstcTexture(*src, fmt);
            if (!img.view) return white_.view;
            keepAlive_.push_back(src);
            textureCache_[key] = img;
            return img.view;
        }
    }
    std::vector<uint16_t> decoded;
    const uint16_t* px = half ? half->data() : nullptr;
    if (!half) {
        // No HDR ASTC sampling (the PC): decode the chain's first level within maxDecodedSide().
        if (!src || astcFormat(src->bw, src->bh, false) == VK_FORMAT_UNDEFINED) return white_.view;   // (divides by bw, bh)
        size_t off = 0;
        auto levelBytes = [&](u32 lw, u32 lh) { return static_cast<size_t>((lw + src->bw - 1) / src->bw) * ((lh + src->bh - 1) / src->bh) * 16; };
        w = src->w; h = src->h;
        for (u32 m = 0; m + 1 < src->mips && std::max(w, h) > maxDecodedSide(); ++m) {
            off += levelBytes(w, h);
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
        }
        if (off + levelBytes(w, h) > src->astc.size() ||
            !astc::decodeASTCHalf(src->astc.data() + off, static_cast<u32>(levelBytes(w, h)), w, h, src->bw, src->bh, decoded))
            return white_.view;
        px = decoded.data();
    }
    if (!w || !h || (half ? half->size() : decoded.size()) / 4 / w < h) return white_.view;
    std::vector<uint16_t> small;
    // (A vertex-animation table shrunk this way averaged neighbouring frames and pieces: Cascadia's 98x2400
    // centerpiece table became 49x1200 and every piece of the mobile over the pool vanished.)
    while (!exact && std::max(w, h) > maxDecodedSide() && w > 1 && h > 1) {
        const u32 nw = w / 2, nh = h / 2;
        std::vector<uint16_t> out(static_cast<size_t>(nw) * nh * 4);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](u32 sx, u32 sy) { return fromHalf(px[(static_cast<size_t>(sy) * w + sx) * 4 + c]); };
                    const float v = 0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                    out[(static_cast<size_t>(y) * nw + x) * 4 + c] = toHalf(v);
                }
        small = std::move(out);
        px = small.data();
        w = nw;
        h = nh;
    }
    Image img = createTextureFmt(px, w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!img.view) return white_.view;
    textureCache_[key] = img;
    return img.view;
}

// A deferred texture: uploaded now as ASTC when the GPU can sample it; otherwise `placeholder` now and the
// decoded texture once a worker finishes (pumpTextures patches the descriptor).
VkImageView HslRenderer::streamTexture(const std::shared_ptr<const TexSource>& src, bool srgb, VkImageView placeholder) {
    const TexKey key{src.get(), srgb};
    auto it = streamed_.find(key);
    if (it != streamed_.end()) return it->second;
    // Cooked without mips and big: sampled as-is it shimmers and costs bandwidth at every distance. Its ASTC chain
    // from the texture cache (tex_cache.h) when an earlier open built one; else decoded (in the background, within
    // maxDecodedSide) with RGBA mips, while the cache builds its chain for next time.
    std::shared_ptr<const TexSource> use = src;
    bool noMips = src->mips <= 1 && std::max(src->w, src->h) > 256;
    static const bool noTexCache = std::getenv("FH_NO_TEX_CACHE") != nullptr;
    if (noMips && astcLdr_ && srgb && !noTexCache) {   // (colour textures: linear ones may hold HDR blocks)
        if (std::shared_ptr<TexSource> chain = texcache::load(*src)) { use = chain; noMips = false; }
        else texcache::Builder::get().add(src);
    } else if (noMips && !noTexCache) {
        texcache::Builder::get().add(src);   // (not drawn from here, but ready for saving: a .fenv bakes every chain)
    }
    const VkFormat fmt = astcLdr_ && !noMips ? astcFormat(use->bw, use->bh, srgb) : VK_FORMAT_UNDEFINED;
    if (fmt != VK_FORMAT_UNDEFINED) {
        Image img = createAstcTexture(*use, fmt);
        if (!img.view) return streamed_[key] = placeholder;   // refused (damaged sizes): the placeholder for good
        textureCache_[reinterpret_cast<uintptr_t>(src.get()) * 2 + (srgb ? 1 : 0)] = img;
        keepAlive_.push_back(use);
        streamed_[key] = img.view;
        return img.view;
    }
    if (fmt == VK_FORMAT_UNDEFINED && astcFormat(src->bw, src->bh, false) == VK_FORMAT_UNDEFINED)
        return streamed_[key] = placeholder;     // no ASTC footprint: nothing a worker could decode
    slotPending_ = true;
    pendingKey_ = key;
    if (!pendingSrc_.count(key)) {               // first user: decoded when a mesh using it comes into view
        keepAlive_.push_back(src);
        pendingSrc_[key] = src;
    }
    return placeholder;
}

void HslRenderer::startWorkers() {
    // Half the cores at most, at low priority: decoding must never take time from the frame being drawn.
    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned n = std::max(1u, std::min(6u, hw / 2));
    for (unsigned i = 0; i < n; ++i)
        workers_.emplace_back([this] {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
            setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
            for (;;) {
                TexJob job;
                {
                    std::unique_lock<std::mutex> l(jobMutex_);
                    jobCv_.wait(l, [&] { return stopping_ || !jobs_.empty(); });
                    if (stopping_) return;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                TexDone d{{job.src.get(), job.srgb}, job.gen, {}, false};
                if (astcFormat(job.src->bw, job.src->bh, false) == VK_FORMAT_UNDEFINED) {   // (levelBytes divides by them)
                    std::lock_guard<std::mutex> l(jobMutex_);
                    done_.push_back(std::move(d));
                    continue;
                }
                // The cooked chain already holds every mip: start from the first one within maxDecodedSide().
                u32 w = job.src->w, h = job.src->h;
                size_t off = 0;
                auto levelBytes = [&](u32 lw, u32 lh) {
                    return static_cast<size_t>((lw + job.src->bw - 1) / job.src->bw) * ((lh + job.src->bh - 1) / job.src->bh) * 16;
                };
                for (u32 m = 0; m + 1 < job.src->mips && std::max(w, h) > maxDecodedSide(); ++m) {
                    const size_t next = off + levelBytes(w, h);
                    if (next + levelBytes(std::max(1u, w / 2), std::max(1u, h / 2)) > job.src->astc.size()) break;
                    off = next;
                    w = std::max(1u, w / 2);
                    h = std::max(1u, h / 2);
                }
                const size_t lvl = std::min(levelBytes(w, h), job.src->astc.size() - std::min(off, job.src->astc.size()));
                std::vector<u8> rgba;
                // Linear (UNORM) textures — normals, ORM, masks — may carry HDR-encoded ASTC blocks, which the LDR
                // profile turns into magenta error texels (a corrupt ORM made the converted Cascadia's floor a
                // mirror). Decode them with the HDR profile, unclamped, then clamp to 8 bits (HSR Studio decodes
                // every UNORM ASTC texture through float too). Colour (sRGB) textures start on the LDR path.
                bool decoded = false;
                if (!job.srgb) {
                    std::vector<uint16_t> half;
                    if (astc::decodeASTCHalf(job.src->astc.data() + off, static_cast<u32>(lvl), w, h, job.src->bw, job.src->bh, half) &&
                        half.size() >= static_cast<size_t>(w) * h * 4) {
                        rgba.resize(static_cast<size_t>(w) * h * 4);
                        for (size_t q = 0; q < rgba.size(); ++q) {
                            const float v = fromHalf(half[q]);
                            rgba[q] = static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                        }
                        decoded = true;
                    }
                }
                if (decoded || astc::decodeASTC(job.src->astc.data() + off, static_cast<u32>(lvl), w, h, job.src->bw, job.src->bh, rgba, false, 1)) {
                    // A colour texture can be HDR-encoded too (the Focused vista's sky, water and glow): the LDR decode
                    // then fills it with magenta error texels. Spot that (a lot of exact-magenta texels) and re-decode
                    // through the HDR profile, sRGB-encoding the clamped linear colour so the sRGB view samples it
                    // right; alpha is linear either way.
                    if (job.srgb && !decoded && rgba.size() >= static_cast<size_t>(w) * h * 4) {
                        size_t mag = 0;
                        for (size_t q = 0; q + 3 < rgba.size(); q += 4)
                            if (rgba[q] > 250 && rgba[q + 1] < 8 && rgba[q + 2] > 250) ++mag;
                        std::vector<uint16_t> half;
                        if (mag * 4 > static_cast<size_t>(w) * h &&      // a quarter or more of the texels are the error colour
                            astc::decodeASTCHalf(job.src->astc.data() + off, static_cast<u32>(lvl), w, h, job.src->bw, job.src->bh, half) &&
                            half.size() >= static_cast<size_t>(w) * h * 4) {
                            auto toSrgb = [](float c) { c = std::clamp(c, 0.0f, 1.0f); return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
                            for (size_t q = 0; q < rgba.size(); ++q) {
                                const float v = fromHalf(half[q]);
                                rgba[q] = static_cast<u8>(std::lround(((q & 3) == 3 ? std::clamp(v, 0.0f, 1.0f) : toSrgb(v)) * 255.0f));
                            }
                        }
                    }
                    // A texture cooked without mips (a 6000^2 flipbook) arrives full size: brought within the cap here.
                    while (std::max(w, h) > maxDecodedSide() && rgba.size() >= static_cast<size_t>(w) * h * 4) halveRGBA(rgba, w, h);
                    // The image and its filled staging buffer are made here too: the frame only records the copy.
                    d.prep = prepareTexture(nullptr, w, h,
                                            job.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, false);
                }
                if (d.prep.img.image && rgba.size() >= static_cast<size_t>(w) * h * 4) {
                    if (const char* dir = std::getenv("FH_DUMP_TEX")) {     // diagnostics: every decoded texture as PPM
                        static std::atomic<int> n{0};
                        char path[512];
                        std::snprintf(path, sizeof path, "%s/%03d_%ux%u_%s_bw%u.ppm", dir, n++, w, h, job.srgb ? "srgb" : "lin", job.src->bw);
                        if (FILE* fp = std::fopen(path, "wb")) {
                            std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
                            for (size_t q = 0; q + 3 < rgba.size(); q += 4) std::fwrite(&rgba[q], 1, 3, fp);
                            std::fclose(fp);
                        }
                    }
                    d.prep.pixels = std::move(rgba);
                    d.ok = true;
                } else if (d.prep.img.image) {
                    destroy(d.prep.img);
                }
                std::lock_guard<std::mutex> l(jobMutex_);
                done_.push_back(std::move(d));
            }
        });
}

void HslRenderer::stopWorkers() {
    {
        std::lock_guard<std::mutex> l(jobMutex_);
        stopping_ = true;
    }
    jobCv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
}

// Uploads finished decodes and points every waiting descriptor at them (call between frames).
void HslRenderer::pumpTextures(double budgetMs) {
    std::vector<TexDone> ready;
    {
        std::lock_guard<std::mutex> l(jobMutex_);
        ready.swap(done_);
    }
    const auto start = std::chrono::steady_clock::now();
    size_t i = 0;
    for (; i < ready.size(); ++i) {
        if (i && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs) break;
        TexDone& d = ready[i];
        if (d.gen != generation_ || !d.ok) {
            if (d.ok) { destroy(d.prep.img); destroy(d.prep.staging); }
            if (d.gen == generation_) waiting_.erase(d.key);
            continue;
        }
        const VkDeviceSize bytes = d.prep.pixels.size();
        const VkDeviceSize at = (ringHead_ + 255) & ~VkDeviceSize(255);
        if (bytes > kRingSize) {                 // cannot happen for <= 8K RGBA8; own staging just in case
            d.prep.staging = createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            if (!d.prep.staging.buf) { destroy(d.prep.img); waiting_.erase(d.key); continue; }   // placeholder stays
            std::memcpy(d.prep.staging.mapped, d.prep.pixels.data(), static_cast<size_t>(bytes));
            d.prep.stagingOffset = 0;
            oneShot([&](VkCommandBuffer c) { recordTextureUpload(c, d.prep); });
            retire(d.prep.staging);
        } else {
            if (at + bytes > kRingSize) break;   // ring full this frame: the rest next frame
            std::memcpy(static_cast<u8*>(ring_.mapped) + at, d.prep.pixels.data(), static_cast<size_t>(bytes));
            ringHead_ = at + bytes;
            d.prep.staging = ring_;              // borrowed, never freed here
            d.prep.stagingOffset = at;
            oneShot([&](VkCommandBuffer c) { recordTextureUpload(c, d.prep); });
            d.prep.staging = Buffer{};
            if (!recordInto_) ringHead_ = 0;     // oneShot waited for the GPU: the ring is free again
        }
        d.prep.pixels.clear();
        d.prep.pixels.shrink_to_fit();
        ++lastUploads_;
        lastUploadMB_ += static_cast<double>(bytes) / (1024.0 * 1024.0);
        Image img = d.prep.img;
        textureCache_[reinterpret_cast<uintptr_t>(d.key.first) * 2 + (d.key.second ? 1 : 0)] = img;
        streamed_[d.key] = img.view;
        auto w = waiting_.find(d.key);
        if (w == waiting_.end()) continue;
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<std::vector<VkDescriptorImageInfo>> infos;
        infos.reserve(w->second.size());
        for (const TexSlot& s : w->second) {
            if (s.mesh >= meshes_.size()) continue;
            infos.emplace_back(s.count, VkDescriptorImageInfo{s.sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet = meshes_[s.mesh].set2;
            wr.dstBinding = s.binding;
            wr.descriptorCount = s.count;
            wr.descriptorType = s.type;
            wr.pImageInfo = infos.back().data();
            writes.push_back(wr);
        }
        if (!writes.empty()) vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        waiting_.erase(w);
    }
    if (i < ready.size()) {                      // over budget: the rest next frame
        std::lock_guard<std::mutex> l(jobMutex_);
        done_.insert(done_.begin(), std::make_move_iterator(ready.begin() + i), std::make_move_iterator(ready.end()));
    }
}

size_t HslRenderer::texturesPending() { return waiting_.size(); }

void HslRenderer::createSelectionPipelines() {
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2; lci.pBindings = b;
    vkCreateDescriptorSetLayout(device_, &lci, nullptr, &sel_.layout);
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &sel_.layout;
    vkCreatePipelineLayout(device_, &pli, nullptr, &sel_.pipeLayout);

    const u8 orange[4] = {0xff, 0x8a, 0x1f, 0xff};                 // Blender's selection orange (sRGB)
    sel_.color = createTexture(orange, 1, 1, true);
    sel_.ubo = createBuffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &sel_.layout;
    vkAllocateDescriptorSets(device_, &ai, &sel_.set);
    VkDescriptorBufferInfo bi = {sel_.ubo.buf, 0, 64};
    VkDescriptorImageInfo ii = {anisoSampler_, sel_.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = sel_.set; w[0].dstBinding = 0; w[0].descriptorCount = 1; w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &bi;
    w[1].dstSet = sel_.set; w[1].dstBinding = 1; w[1].descriptorCount = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);

    auto module = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size; ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        return m;
    };
    VkShaderModule vs = module(kSelectionVertSpirv, kSelectionVertSize), fs = module(kSelectionFragSpirv, kSelectionFragSize);
    if (!vs || !fs) {                            // no highlight rather than a pipeline over a null module
        LOG("selection shaders failed");
        if (vs) vkDestroyShaderModule(device_, vs, nullptr);
        if (fs) vkDestroyShaderModule(device_, fs, nullptr);
        destroySelection();
        return;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE; ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cb = {};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1; cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi; pci.layout = sel_.pipeLayout; pci.renderPass = renderPass_;
    // edges: opaque lines
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &sel_.lines);
    // faces: a faint tint (blend factor = constant alpha)
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    cb.blendEnable = VK_TRUE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA; cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cb.alphaBlendOp = VK_BLEND_OP_ADD;
    cbs.blendConstants[3] = 0.12f;
    vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &sel_.faces);
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
}

void HslRenderer::setSelection(int mesh) {
    if (mesh == sel_.mesh) return;
    const auto t0 = std::chrono::steady_clock::now();
    if (!sel_.lines) createSelectionPipelines();
    if (!sel_.lines || !sel_.faces) return;
    if (sel_.vbo.buf || sel_.edges.buf || sel_.tris.buf) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);   // the last frame may still draw the old one
        destroy(sel_.vbo); destroy(sel_.edges); destroy(sel_.tris);
    }
    sel_.mesh = -1;
    sel_.edgeCount = sel_.triCount = 0;
    if (mesh < 0 || static_cast<size_t>(mesh) >= meshes_.size() || !scene_) return;
    const MeshData& md = scene_->meshes[meshes_[mesh].sceneIndex];
    const size_t nv = md.positions.size() / 3;
    if (!nv || md.indices.size() < 3) return;
    std::vector<float> v(nv * 5, 0.0f);
    for (size_t i = 0; i < nv; ++i) std::memcpy(&v[i * 5], &md.positions[i * 3], 12);
    // Unique edges (shared by two triangles: drawn once). Sorted keys, not a hash set: a few times faster, and
    // selecting a big mesh in Studio held up a frame for it.
    std::vector<u32> edges, tris;
    std::vector<uint64_t> keys;
    keys.reserve(md.indices.size());
    tris.reserve(md.indices.size());
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
        const u32 idx[3] = {md.indices[t], md.indices[t + 1], md.indices[t + 2]};
        if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv) continue;
        tris.insert(tris.end(), idx, idx + 3);
        for (int e = 0; e < 3; ++e) {
            const u32 a = std::min(idx[e], idx[(e + 1) % 3]), b = std::max(idx[e], idx[(e + 1) % 3]);
            keys.push_back(static_cast<uint64_t>(a) << 32 | b);
        }
    }
    if (tris.empty()) return;
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    edges.reserve(keys.size() * 2);
    for (uint64_t k : keys) { edges.push_back(static_cast<u32>(k >> 32)); edges.push_back(static_cast<u32>(k)); }
    const bool batch = !loadCmd_ && !recordInto_;   // the three uploads in one submit (one wait, not three)
    if (batch) beginUploadBatch();
    sel_.vbo = createDeviceBuffer(v.data(), v.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    sel_.edges = createDeviceBuffer(edges.data(), edges.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    sel_.tris = createDeviceBuffer(tris.data(), tris.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (batch) flushUploadBatch();
    if (!sel_.vbo.buf || !sel_.edges.buf || !sel_.tris.buf) { destroy(sel_.vbo); destroy(sel_.edges); destroy(sel_.tris); return; }
    sel_.edgeCount = static_cast<uint32_t>(edges.size());
    sel_.triCount = static_cast<uint32_t>(tris.size());
    sel_.mesh = mesh;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 3.0) LOG("selection highlight built in %.1f ms (%u edges, %u triangles)", ms, sel_.edgeCount / 2, sel_.triCount / 3);
}

void HslRenderer::createGuidePipelines() {
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 128};   // clipFromWorld[2]
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &guide_.layout) != VK_SUCCESS) { guide_.layout = VK_NULL_HANDLE; return; }
    auto module = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size; ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        return m;
    };
    VkShaderModule vs = module(kGuideVertSpirv, kGuideVertSize), fs = module(kGuideFragSpirv, kGuideFragSize);
    if (vs && fs) {
        // spec constant 0: the alpha scale (1 in front of the home, faint behind it)
        float alphaScale = 1.0f;
        VkSpecializationMapEntry me = {0, 0, sizeof(float)};
        VkSpecializationInfo si = {1, &me, sizeof(float), &alphaScale};
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main"; st[0].pSpecializationInfo = &si;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
        VkVertexInputBindingDescription vb = {0, sizeof(GuideVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples_;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;   // never writes: the guide doesn't hide itself
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;              // reversed-Z: in front of the home
        VkPipelineColorBlendAttachmentState cb = {};
        cb.colorWriteMask = 0xF;
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cb.alphaBlendOp = VK_BLEND_OP_ADD;   // the frame's alpha stays the home's
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1; cbs.pAttachments = &cb;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
        pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cbs;
        pci.pDynamicState = &dsi; pci.layout = guide_.layout; pci.renderPass = renderPass_;
        if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &guide_.visible) != VK_SUCCESS) guide_.visible = VK_NULL_HANDLE;
        // Behind the home (a sofa between you and the landing): still there, at a quarter strength.
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        alphaScale = 0.25f;
        if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &guide_.hidden) != VK_SUCCESS) guide_.hidden = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(device_, vs, nullptr);
    if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    if (!guide_.visible) { LOG("guide shaders failed: no teleport arc"); destroyGuide(); }
}

void HslRenderer::setStaticGuide(const std::vector<GuideVertex>& tris) {
    if (!device_) return;
    if (guide_.staticVbo.buf) { vkDeviceWaitIdle(device_); destroy(guide_.staticVbo); }
    guide_.staticVertexCount = 0;
    static const bool noGuide = std::getenv("FH_NO_GUIDE") != nullptr;   // diagnostics: the home alone (no walk-area overlay)
    if (tris.empty() || !guide_.visible || noGuide) return;
    guide_.staticVbo = createDeviceBuffer(tris.data(), tris.size() * sizeof(GuideVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (guide_.staticVbo.buf) guide_.staticVertexCount = static_cast<uint32_t>(tris.size());
}

void HslRenderer::destroyGuide() {
    for (Buffer& b : guide_.vbo) destroy(b);
    destroy(guide_.staticVbo);
    if (guide_.visible) vkDestroyPipeline(device_, guide_.visible, nullptr);
    if (guide_.hidden) vkDestroyPipeline(device_, guide_.hidden, nullptr);
    if (guide_.layout) vkDestroyPipelineLayout(device_, guide_.layout, nullptr);
    guide_ = Guide{};
}

// Inside the frame's render pass, after the home (into this command buffer's own guide buffer).
void HslRenderer::drawGuide(const EyeView eyes[2]) {
    if (!guide_.visible) return;
    if (guide_.staticVertexCount) {
        float vp[32];
        for (int e = 0; e < 2; ++e) mat4mul4(eyes[e].proj.m, eyes[e].view.m, vp + 16 * e);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd_, 0, 1, &guide_.staticVbo.buf, &zero);
        vkCmdPushConstants(cmd_, guide_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof vp, vp);
        if (guide_.hidden) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, guide_.hidden); vkCmdDraw(cmd_, guide_.staticVertexCount, 1, 0, 0); }
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, guide_.visible);
        vkCmdDraw(cmd_, guide_.staticVertexCount, 1, 0, 0);
    }
    if (guideVerts_.empty()) return;
    const VkDeviceSize bytes = guideVerts_.size() * sizeof(GuideVertex);
    // This command buffer's own copy: the other one's may still be drawn from (a frame recorded early).
    Buffer& vbo = guide_.vbo[cmdIndex_];
    if (vbo.size < bytes) {
        destroy(vbo);
        vbo = createBuffer(std::max<VkDeviceSize>(bytes * 3 / 2, 64 << 10), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        if (!vbo.mapped) return;
    }
    std::memcpy(vbo.mapped, guideVerts_.data(), static_cast<size_t>(bytes));
    float vp[32];
    for (int e = 0; e < 2; ++e) mat4mul4(eyes[e].proj.m, eyes[e].view.m, vp + 16 * e);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &vbo.buf, &zero);
    vkCmdPushConstants(cmd_, guide_.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof vp, vp);
    const uint32_t n = static_cast<uint32_t>(guideVerts_.size());
    if (guide_.hidden) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, guide_.hidden); vkCmdDraw(cmd_, n, 1, 0, 0); }
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, guide_.visible);
    vkCmdDraw(cmd_, n, 1, 0, 0);
}

// ── Debug views ──

void HslRenderer::setDebugView(int view) {
    view = std::clamp(view, 0, static_cast<int>(kDebugViews) - 1);
    if (view == kViewWireframe && !wireOk_) view = kViewMeshes;   // no line drawing on this GPU: the closest thing
    if (view != kViewNormal && !dbg_.layout && !createDebug()) view = kViewNormal;
    if (view != debugView_) LOG("debug view %d", view);
    if (view != kViewCost) cost_.clear();
    costMin_.clear(); costSeen_.clear(); std::fill(std::begin(costBaseRing_), std::end(costBaseRing_), 1e9f);
    doubled_ = -1;
    debugView_ = view;
}

std::vector<std::pair<std::string, double>> HslRenderer::perfCounterAverages() const {
    std::vector<std::pair<std::string, double>> out;
    for (size_t i = 0; i < perfNames_.size() && perfFrames_; ++i) out.push_back({perfNames_[i], perfSum_[i] / perfFrames_});
    return out;
}

std::vector<std::pair<float, int>> HslRenderer::costliest(size_t n) const {
    std::vector<std::pair<float, int>> out;
    for (size_t i = 0; i < cost_.size() && i < meshes_.size(); ++i)
        if (cost_[i] > 0) out.push_back({cost_[i], meshes_[i].sceneIndex});
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (out.size() > n) out.resize(n);
    return out;
}

bool HslRenderer::createDebug() {
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1; lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &dbg_.setLayout) != VK_SUCCESS) { dbg_.setLayout = VK_NULL_HANDLE; return false; }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &dbg_.setLayout;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &dbg_.layout) != VK_SUCCESS) { dbg_.layout = VK_NULL_HANDLE; destroyDebug(); return false; }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &dbg_.setLayout;
    if (vkAllocateDescriptorSets(device_, &ai, &dbg_.set) != VK_SUCCESS) { dbg_.set = VK_NULL_HANDLE; destroyDebug(); return false; }
    auto module = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size; ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        return m;
    };
    dbg_.vs = module(kDebugVertSpirv, kDebugVertSize);
    dbg_.fs = module(kDebugFragSpirv, kDebugFragSize);
    if (!dbg_.vs || !dbg_.fs) { LOG("debug view shaders failed"); destroyDebug(); return false; }
    return true;
}

void HslRenderer::destroyDebug() {
    for (auto& kv : dbg_.pipes) if (kv.second) vkDestroyPipeline(device_, kv.second, nullptr);
    destroy(dbg_.draws);
    if (dbg_.vs) vkDestroyShaderModule(device_, dbg_.vs, nullptr);
    if (dbg_.fs) vkDestroyShaderModule(device_, dbg_.fs, nullptr);
    if (dbg_.set) vkFreeDescriptorSets(device_, descPool_, 1, &dbg_.set);
    if (dbg_.layout) vkDestroyPipelineLayout(device_, dbg_.layout, nullptr);
    if (dbg_.setLayout) vkDestroyDescriptorSetLayout(device_, dbg_.setLayout, nullptr);
    dbg_ = Debug{};
}

// The program's vertex buffer read for its positions only: one pipeline per layout and mode (VK_NULL_HANDLE when the
// program has no position attribute).
VkPipeline HslRenderer::debugPipeline(const Program& p, int mode) {
    const VertexAttr* pos = nullptr;
    for (const VertexAttr& a : p.attrs) if (a.role == Role::Pos && !a.integer) { pos = &a; break; }
    if (!pos || !dbg_.layout) return VK_NULL_HANDLE;
    const uint64_t key = (static_cast<uint64_t>(p.stride) << 40) | (static_cast<uint64_t>(pos->offset) << 24) |
                         (static_cast<uint64_t>(pos->format) << 4) | static_cast<uint64_t>(mode);
    if (auto it = dbg_.pipes.find(key); it != dbg_.pipes.end()) return it->second;
    const int shade = mode == kDbgSolid || mode == kDbgTint ? 1 : 0;
    VkSpecializationMapEntry me = {0, 0, sizeof(int)};
    VkSpecializationInfo si = {1, &me, sizeof(int), &shade};
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = dbg_.vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = dbg_.fs; st[1].pName = "main"; st[1].pSpecializationInfo = &si;
    VkVertexInputBindingDescription vb = {0, p.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va = {0, 0, pos->format, pos->offset};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = mode == kDbgWire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = mode == kDbgAdd ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable = mode == kDbgSolid ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reversed-Z
    VkPipelineColorBlendAttachmentState cb = {};
    cb.colorWriteMask = 0xF;
    cb.blendEnable = mode == kDbgSolid ? VK_FALSE : VK_TRUE;
    cb.srcColorBlendFactor = mode == kDbgAdd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
    cb.dstColorBlendFactor = mode == kDbgAdd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cb.alphaBlendOp = VK_BLEND_OP_ADD;
    if (mode == kDbgSolid) { cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1; cbs.pAttachments = &cb;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dsi; pci.layout = dbg_.layout; pci.renderPass = renderPass_;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    dbg_.pipes[key] = pipe;
    return pipe;
}

// One mesh in a debug mode: its matrices and colour go in the next slot of the draws buffer (render() made room).
// nudge: a hair toward the eye, to sit on the surface the home drew (wireframe, tint).
void HslRenderer::drawDebug(const GpuMesh& gm, int mode, const float color[4], const EyeView eyes[2], bool nudge) {
    if (dbg_.used >= dbg_.capacity || !dbg_.draws.mapped || gm.program < 0) return;
    VkPipeline pipe = debugPipeline(programs_[gm.program], mode);
    if (!pipe) return;
    float* d = static_cast<float*>(dbg_.draws.mapped) + dbg_.used * 52;   // Draw: mat4 clipFromModel[2], mat4 world, vec4 colour
    for (int e = 0; e < 2; ++e) {
        float vp[16];
        mat4mul4(eyes[e].proj.m, eyes[e].view.m, vp);
        mat4mul4(vp, gm.world, d + 16 * e);
        if (nudge) for (int c = 0; c < 4; ++c) d[16 * e + c * 4 + 2] *= 1.0015f;   // reversed-Z: nearer
    }
    std::memcpy(d + 32, gm.world, 64);
    std::memcpy(d + 48, color, 16);
    VkDeviceSize zero = 0;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, dbg_.layout, 0, 1, &dbg_.set, 0, nullptr);
    vkCmdBindVertexBuffers(cmd_, 0, 1, &gm.vbo.buf, &zero);
    vkCmdBindIndexBuffer(cmd_, gm.ibo.buf, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd_, gm.indexCount, 1, 0, 0, dbg_.used);
    ++dbg_.used;
}

void HslRenderer::destroySelection() {
    destroy(sel_.vbo); destroy(sel_.edges); destroy(sel_.tris); destroy(sel_.ubo); destroy(sel_.color);
    if (sel_.lines) vkDestroyPipeline(device_, sel_.lines, nullptr);
    if (sel_.faces) vkDestroyPipeline(device_, sel_.faces, nullptr);
    if (sel_.pipeLayout) vkDestroyPipelineLayout(device_, sel_.pipeLayout, nullptr);
    if (sel_.layout) vkDestroyDescriptorSetLayout(device_, sel_.layout, nullptr);
    sel_ = Selection{};
}

void HslRenderer::setDynamicMeshes(const std::vector<int>& sceneIndices) {
    dynamicScene_.clear();
    dynamicScene_.insert(sceneIndices.begin(), sceneIndices.end());
}

// The previous frame is finished (its fence waited), so the mapped vertex buffers are free to rewrite.
void HslRenderer::refreshDynamicMeshes(bool cpu, bool upload) {
    if (!scene_) return;
    std::vector<DynJob>& jobs = dynJobs_;
    if (cpu) {   // (an early-recorded frame ran this part before it was recorded, and uploads after)
    jobs.clear();
    skinCopies_.clear();
    stats_.dynamicMeshes = stats_.dynamicRebuilt = 0;
    static const bool traceOn = std::getenv("FH_DYN_TRACE") != nullptr;   // diagnostics: what re-uploads (one frame in 600)
    static uint32_t traceFrame = 0;
    const bool trace = traceOn && traceFrame++ % 600 == 300;
    for (auto& gm : meshes_) {
        if (!gm.dynamic || gm.sceneIndex < 0 || static_cast<size_t>(gm.sceneIndex) >= scene_->meshes.size()) continue;
        const MeshData& md = scene_->meshes[gm.sceneIndex];
        ++stats_.dynamicMeshes;
        std::memcpy(gm.color, md.curTint, sizeof gm.color);
        std::memcpy(gm.uvXform, md.uvXform, sizeof gm.uvXform);
        if (md.skinGen != gm.skinGen && gm.skinUbo >= 0 && !md.skinJoints.empty()) {   // skinned on the GPU: its joints
            gm.skinGen = md.skinGen;
            skinCopies_.push_back(&gm);
            for (int k = 0; k < 3; ++k) {
                gm.localMin[k] = md.skinBox[k]; gm.localMax[k] = md.skinBox[3 + k];
                gm.localCentroid[k] = 0.5f * (md.skinBox[k] + md.skinBox[3 + k]);
            }
            placeMesh(gm);
        }
        if (std::memcmp(gm.anim, md.animXform, sizeof gm.anim) != 0) {   // rigid motion: a matrix, no vertices
            std::memcpy(gm.anim, md.animXform, sizeof gm.anim);
            placeMesh(gm);
        }
        // Most animated meshes hold still for part of the loop (and a tint-only or rigid animation never changes
        // them): rebuild the vertices only when the loader changed the positions or uvs, and only that half.
        static const bool noSkip = std::getenv("FH_NO_DYN_SKIP") != nullptr;   // A/B: rebuild every frame (the old way)
        const bool posChanged = md.posGen != gm.posGen || noSkip, uvChanged = md.uvGen != gm.uvGen || noSkip;
        if (!posChanged && !uvChanged) continue;
        gm.posGen = md.posGen; gm.uvGen = md.uvGen;
        ++stats_.dynamicRebuilt;
        if (trace)
            LOG("dyn rebuild '%s': %u verts, %s%s", md.name.c_str(), md.nVerts, posChanged ? "positions " : "", uvChanged ? "uvs" : "");
        if (posChanged) measureMesh(gm, md, static_cast<u32>(std::min<size_t>(md.nVerts, md.positions.size() / 3)));   // (culling)
        jobs.push_back({&gm, &md, posChanged, uvChanged});
    }
    }
    if (!upload) return;
    for (GpuMesh* g : skinCopies_) {
        const MeshData& md = scene_->meshes[g->sceneIndex];
        Buffer& b = g->ubos[g->skinUbo];
        std::memcpy(b.mapped, md.skinJoints.data(), std::min<size_t>(md.skinJoints.size() * sizeof(float), static_cast<size_t>(b.size)));
    }
    // The rebuilds (skinned meshes: thousands of vertices each) on all cores: each writes only its own mesh.
    par::forEach(jobs.size(), [&](size_t j) {
        GpuMesh& gm = *jobs[j].gm;
        const MeshData& md = *jobs[j].md;
        const bool posChanged = jobs[j].pos, uvChanged = jobs[j].uv;
        static thread_local std::vector<u8> vbo;
        static thread_local std::vector<float> normals;
        const Program& p = programs_[gm.program];
        const u32 nv = static_cast<u32>(std::min<size_t>(md.nVerts, md.positions.size() / 3));
        static const bool fullRebuild = std::getenv("FH_DYN_FULL") != nullptr;   // A/B: the whole interleave
        if (fullRebuild || !gm.vbo.mapped || static_cast<VkDeviceSize>(nv) * p.stride > gm.vbo.size) {
            buildVertices(p, md, vbo, normals);
            if (vbo.size() <= gm.vbo.size && gm.vbo.mapped) std::memcpy(gm.vbo.mapped, vbo.data(), vbo.size());
            return;
        }
        // Only what an animation changes (positions, uvs, and the smooth normals that follow the positions) is
        // rewritten, in place: colours, tangents, bones and the rest stay as uploaded.
        bool computed = false;
        for (const auto& a : p.attrs)
            if (a.role == Role::Normal && md.normals.size() < static_cast<size_t>(nv) * 3) computed = true;
        if (!posChanged) computed = false;   // (the normals follow the positions)
        else if (!computed)
            for (const auto& a : p.attrs)
                if (a.role == Role::Normal)
                    for (u32 v = 0; v < nv && !computed; ++v)
                        computed = md.normals[v * 3] == 0 && md.normals[v * 3 + 1] == 0 && md.normals[v * 3 + 2] == 0;
        if (computed && posChanged) {
            normals.assign(static_cast<size_t>(nv) * 3, 0.0f);
            for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
                const u32 a = md.indices[t], b = md.indices[t + 1], c = md.indices[t + 2];
                if (a >= nv || b >= nv || c >= nv) continue;
                const float* pa = &md.positions[a * 3]; const float* pb = &md.positions[b * 3]; const float* pc = &md.positions[c * 3];
                const float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                for (u32 v : {a, b, c}) for (int k = 0; k < 3; ++k) normals[v * 3 + k] += n[k];
            }
            for (u32 v = 0; v < nv; ++v) {
                float* n = &normals[v * 3];
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[1] = 1; }   // (as buildVertices)
            }
        }
        u8* base = static_cast<u8*>(gm.vbo.mapped);
        for (const auto& a : p.attrs) {
            if (a.integer || (a.role != Role::Pos && a.role != Role::Normal && a.role != Role::Uv0 && a.role != Role::Uv1)) continue;
            if ((a.role == Role::Pos || a.role == Role::Normal) ? !posChanged : !uvChanged) continue;
            if (a.role == Role::Uv1 && md.uvs2.size() >= static_cast<size_t>(nv) * 2) continue;   // its own UV2: not animated
            const bool half = a.format >= VK_FORMAT_R16_SFLOAT && a.format <= VK_FORMAT_R16G16B16A16_SFLOAT;
            for (u32 v = 0; v < nv; ++v) {
                float tmp[4] = {0, 0, 0, 0};
                switch (a.role) {
                    case Role::Pos: std::memcpy(tmp, &(md.skinBindPos.size() >= static_cast<size_t>(nv) * 3 ? md.skinBindPos : md.positions)[v * 3], 12); break;
                    case Role::Normal: {
                        const float* an = !computed ? &md.normals[v * 3] : nullptr;
                        std::memcpy(tmp, an ? an : &normals[v * 3], 12);
                        break;
                    }
                    default:   // Uv0, or Uv1 falling back to the uvs (as buildVertices does)
                        if (v * 2 + 1 < md.uvs.size()) { tmp[0] = md.uvs[v * 2]; tmp[1] = md.uvs[v * 2 + 1]; }
                        break;
                }
                u8* out = base + static_cast<size_t>(v) * p.stride + a.offset;
                if (half) { for (uint32_t k = 0; k < a.size / 2; ++k) { const uint16_t h = toHalf(k < 4 ? tmp[k] : 0); std::memcpy(out + k * 2, &h, 2); } }
                else std::memcpy(out, tmp, std::min<uint32_t>(a.size, 16));
            }
        }
    });
}

bool HslRenderer::gpuBusy() const { return fence_ && vkGetFenceStatus(device_, fence_) == VK_NOT_READY; }

void HslRenderer::requestAllTextures() {
    for (auto& gm : meshes_) requestTextures(gm);
    texturesRequested_ = waiting_.size();
    preloading_ = texturesRequested_ > 0;
    LOG("preloading %zu textures", texturesRequested_);
}

float HslRenderer::textureProgress() {
    if (!texturesRequested_) return 1.0f;
    return 1.0f - static_cast<float>(std::min(waiting_.size(), texturesRequested_)) / static_cast<float>(texturesRequested_);
}

void HslRenderer::finishStreaming() {
    for (auto& gm : meshes_) requestTextures(gm);   // everything, not just what is in view
    while (!waiting_.empty() && !workers_.empty()) {
        pumpTextures(1e9);
        if (!waiting_.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Skeletal animation: sample each rig's clip and write its joints' skin matrices into every mesh it drives.
void HslRenderer::updateAnimations(float timeSec) {
    if (!scene_ || scene_->anims.empty()) return;
    std::vector<float> skin;
    for (const HslAnimGroup& g : scene_->anims) {
        sampleRendClip(g.skel, g.clip, timeSec, skin);
        const size_t joints = std::min<size_t>(skin.size() / 16, 256);
        for (int si : g.meshes) {
            auto it = sceneToMesh_.find(si);
            if (it == sceneToMesh_.end()) continue;
            GpuMesh& gm = meshes_[it->second];
            if (gm.skinUbo < 0) continue;
            Buffer& b = gm.ubos[gm.skinUbo];
            std::memcpy(b.mapped, skin.data(), std::min<size_t>(joints * 64, static_cast<size_t>(b.size)));
        }
    }
}

// World-space bounds of a mesh (its local box through its world matrix), for culling.
void HslRenderer::updateBounds(GpuMesh& gm) {
    for (int k = 0; k < 3; ++k) { gm.worldMin[k] = 1e30f; gm.worldMax[k] = -1e30f; }
    for (int c = 0; c < 8; ++c) {
        const float p[3] = {c & 1 ? gm.localMax[0] : gm.localMin[0], c & 2 ? gm.localMax[1] : gm.localMin[1], c & 4 ? gm.localMax[2] : gm.localMin[2]};
        for (int k = 0; k < 3; ++k) {
            const float v = gm.world[k] * p[0] + gm.world[4 + k] * p[1] + gm.world[8 + k] * p[2] + gm.world[12 + k];
            gm.worldMin[k] = std::min(gm.worldMin[k], v);
            gm.worldMax[k] = std::max(gm.worldMax[k], v);
        }
    }
}

// Queues the background decode of every streamed texture this mesh is still showing a placeholder for.
void HslRenderer::requestTextures(GpuMesh& gm) {
    static const bool noStream = std::getenv("FH_NO_STREAM") != nullptr;   // diagnostics: placeholders only
    if (gm.wants.empty() || noStream) return;
    std::lock_guard<std::mutex> l(jobMutex_);
    for (const TexKey& key : gm.wants) {
        if (requested_.count(key)) continue;
        auto src = pendingSrc_.find(key);
        if (src == pendingSrc_.end()) continue;
        requested_[key] = true;
        jobs_.push_back({src->second, key.second, generation_});
    }
    gm.wants.clear();
    jobCv_.notify_all();
}

// The vertices a mesh really has: nVerts as loaded, never past its positions (a damaged file can claim more).
static u32 meshVerts(const MeshData& md) { return static_cast<u32>(std::min<size_t>(md.nVerts, md.positions.size() / 3)); }

static double g_loadPipeMs = 0;                 // pipeline-compile share of the last uploadMesh (load timing log)

// The interleaved vertex data `p` reads for `md` (and the smooth normals it was built with): at upload, and every
// frame again for animated meshes.
void HslRenderer::buildVertices(const Program& p, const MeshData& md, std::vector<u8>& vbo, std::vector<float>& normals) const {
    const u32 nv = meshVerts(md);
    // (GPU-skinned: the vertex buffer carries the bind pose, the joints place it)
    const std::vector<float>& pos = md.skinBindPos.size() >= static_cast<size_t>(nv) * 3 ? md.skinBindPos : md.positions;
    // Smooth normals from the triangles (MeshData carries positions/uvs, not normals).
    normals.assign(static_cast<size_t>(nv) * 3, 0.0f);
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
        u32 a = md.indices[t], b = md.indices[t + 1], c = md.indices[t + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        const float* pa = &md.positions[a * 3]; const float* pb = &md.positions[b * 3]; const float* pc = &md.positions[c * 3];
        float e1[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]}, e2[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (u32 v : {a, b, c}) for (int k = 0; k < 3; ++k) normals[v * 3 + k] += n[k];
    }
    for (u32 v = 0; v < nv; ++v) {
        float* n = &normals[v * 3];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[1] = 1; }
    }

    // Interleave every input the program declares, at the offsets/format the pipeline expects.
    vbo.assign(static_cast<size_t>(nv) * p.stride, 0);
    for (u32 v = 0; v < nv; ++v) {
        u8* dst = vbo.data() + static_cast<size_t>(v) * p.stride;
        for (const auto& a : p.attrs) {
            float tmp[4] = {0, 0, 0, 0};
            u8 b4[4] = {0, 0, 0, 0};
            int nfl = 0;
            switch (a.role) {
                case Role::Pos: std::memcpy(tmp, &pos[v * 3], 12); nfl = 3; break;
                case Role::Normal: {                      // authored, as HSR feeds inNorm; computed only if absent
                    const float* an = md.normals.size() >= static_cast<size_t>(nv) * 3 ? &md.normals[v * 3] : nullptr;
                    std::memcpy(tmp, an && (an[0] != 0 || an[1] != 0 || an[2] != 0) ? an : &normals[v * 3], 12);
                    nfl = 3; break;
                }
                case Role::Tangent: {                     // authored xyzw (w = handedness)
                    const float* at = md.tangents.size() >= static_cast<size_t>(nv) * 4 ? &md.tangents[v * 4] : nullptr;
                    if (at && (at[0] != 0 || at[1] != 0 || at[2] != 0)) std::memcpy(tmp, at, 16);
                    else { tmp[0] = 1; tmp[3] = 1; }
                    nfl = 4; break;
                }
                case Role::Uv0:
                    if (v * 2 + 1 < md.uvs.size()) { tmp[0] = md.uvs[v * 2]; tmp[1] = md.uvs[v * 2 + 1]; }
                    nfl = 2; break;
                case Role::Uv1: {
                    // UV2 (lightmap / RBAoDir UV) is stored with V flipped relative to UV1. Shaders with
                    // lightBakerParams get the flip through that atlas transform instead (see set2 fill).
                    const bool own = md.uvs2.size() >= static_cast<size_t>(nv) * 2;
                    const auto& uv = own ? md.uvs2 : md.uvs;   // raw, as HSR Studio feeds inLightmapUv
                    if (v * 2 + 1 < uv.size()) { tmp[0] = uv[v * 2]; tmp[1] = uv[v * 2 + 1]; }
                    nfl = 2; break;
                }
                case Role::Uv2: if (v * 2 + 1 < md.uvs3.size()) { tmp[0] = md.uvs3[v * 2]; tmp[1] = md.uvs3[v * 2 + 1]; } nfl = 2; break;
                case Role::Uv3: if (v * 2 + 1 < md.uvs4.size()) { tmp[0] = md.uvs4[v * 2]; tmp[1] = md.uvs4[v * 2 + 1]; } nfl = 2; break;
                case Role::Color:
                    if (md.colors.size() >= static_cast<size_t>(nv) * 4) std::memcpy(b4, &md.colors[v * 4], 4);
                    else b4[0] = b4[1] = b4[2] = b4[3] = 255;                    break;
                case Role::BoneIdx: if (md.boneIndices.size() >= static_cast<size_t>(nv) * 4) std::memcpy(b4, &md.boneIndices[v * 4], 4); break;
                case Role::BoneWgt:
                    if (md.boneWeights.size() >= static_cast<size_t>(nv) * 4) std::memcpy(b4, &md.boneWeights[v * 4], 4);
                    else b4[0] = 255;
                    break;
                case Role::Zero: break;
            }
            u8* out = dst + a.offset;
            if (a.size == 4 && (a.role == Role::Color || a.role == Role::BoneIdx || a.role == Role::BoneWgt)) {
                std::memcpy(out, b4, 4);
            } else if (a.format >= VK_FORMAT_R16_SFLOAT && a.format <= VK_FORMAT_R16G16B16A16_SFLOAT) {
                for (uint32_t k = 0; k < a.size / 2; ++k) { uint16_t h = toHalf(k < 4 ? tmp[k] : 0); std::memcpy(out + k * 2, &h, 2); }
            } else if (!a.integer) {
                std::memcpy(out, tmp, std::min<uint32_t>(a.size, 16));
            }
            (void)nfl;
        }
    }
}

bool HslRenderer::uploadMesh(const MeshData& md, int sceneIndex) {
    if (md.overlayKind) return false;                       // navmesh / collision proxies: never drawn
    if (const char* skip = std::getenv("FH_SKIP"))               // diagnostics: leave out meshes of shaders matching any of a,b,c
        for (std::string rest = skip; !rest.empty();) {
            const size_t c = rest.find(',');
            const std::string part = rest.substr(0, c);
            if (!part.empty() && md.shaderPath.find(part) != std::string::npos) return false;
            rest = c == std::string::npos ? std::string() : rest.substr(c + 1);
        }
    if (meshVerts(md) == 0 || md.indices.empty()) { LOG("skip '%s': no geometry", md.name.c_str()); return false; }
    const ShaderProgramSrc* src = findProgram(*scene_, md.shaderPath);
    if (!src && md.isSkybox)                                // the shell's skybox shader ships in renderer_module
        for (const auto& kv : scene_->programs)
            if (has(lower(kv.first), "skybox")) { src = &kv.second; break; }
    if (!src) { LOG("skip '%s': no shader program for '%s'", md.name.c_str(), md.shaderPath.c_str()); return false; }
    const int pi = programFor(*src);
    if (pi < 0) { LOG("skip '%s': program failed", md.name.c_str()); return false; }
    Program& p = programs_[pi];

    GpuMesh gm;
    gm.program = pi;
    gm.sceneIndex = sceneIndex;
    const u32 nv = meshVerts(md);
    // Indices past the vertices (a damaged file) would read beyond the vertex buffer on the GPU: those
    // triangles collapse to vertex 0 (degenerate, not drawn). Valid meshes keep their own index list.
    const std::vector<u32>* indices = &md.indices;
    std::vector<u32> clamped;
    if (std::any_of(md.indices.begin(), md.indices.end(), [nv](u32 i) { return i >= nv; })) {
        LOG("mesh '%s': indices past its %u vertices dropped", md.name.c_str(), nv);
        clamped = md.indices;
        clamped.resize(clamped.size() / 3 * 3);
        for (size_t t = 0; t < clamped.size(); t += 3)
            if (clamped[t] >= nv || clamped[t + 1] >= nv || clamped[t + 2] >= nv) clamped[t] = clamped[t + 1] = clamped[t + 2] = 0;
        indices = &clamped;
    }
    // A buffer the GPU refused: this mesh is left out (and nothing it registered stays behind).
    auto drop = [&](const char* why) {
        LOG("skip '%s': %s", md.name.c_str(), why);
        flushUploadBatch();                                 // its recorded copies run before its buffers go
        destroy(gm.vbo);
        destroy(gm.ibo);
        for (auto& b : gm.ubos) destroy(b);
        for (auto& i : gm.images) destroy(i);
        if (gm.set2) vkFreeDescriptorSets(device_, descPool_, 1, &gm.set2);
        for (auto w = waiting_.begin(); w != waiting_.end();) {   // (an empty entry would hold finishStreaming forever)
            w->second.erase(std::remove_if(w->second.begin(), w->second.end(), [&](const TexSlot& t) { return t.mesh == meshes_.size(); }),
                            w->second.end());
            w = w->second.empty() ? waiting_.erase(w) : std::next(w);
        }
        return false;
    };

    std::vector<float> normals;
    std::vector<u8> vbo;
    buildVertices(p, md, vbo, normals);
    if (debugMesh(md)) {
        gm.debug = true; gm.debugName = md.name;
        LOG("  mesh '%s' shader %s: %u verts, stride %u, uv2 %zu, lmUvScaleOffset %.4f,%.4f,%.4f,%.4f, lightmap %ux%u",
            md.name.c_str(), md.shaderPath.c_str(), nv, p.stride, md.uvs2.size() / 2, md.lmUvScaleOffset[0], md.lmUvScaleOffset[1],
            md.lmUvScaleOffset[2], md.lmUvScaleOffset[3], md.lmW, md.lmH);
        LOG("  mesh '%s' blend %d (useBlend %d additive %d) colors %zu", md.name.c_str(), md.useBlend ? (md.additive ? 2 : 1) : 0,
            (int)md.useBlend, (int)md.additive, md.colors.size() / 4);
        if (!md.colors.empty()) {
            size_t gb = 0, n = md.colors.size() / 4;
            double avg[4] = {0, 0, 0, 0};
            for (size_t q = 0; q < n; ++q) {
                gb += md.colors[q * 4 + 1] > 0 || md.colors[q * 4 + 2] > 0;
                for (int c = 0; c < 4; ++c) avg[c] += md.colors[q * 4 + c];
            }
            LOG("  mesh '%s' vertex colours: %.1f%% with green or blue, average %.0f,%.0f,%.0f,%.0f", md.name.c_str(), 100.0 * gb / std::max<size_t>(1, n),
                avg[0] / n, avg[1] / n, avg[2] / n, avg[3] / n);
        }
        {
            float w[16];
            if (md.hasWorldMatrix) std::memcpy(w, md.worldMatrix, 64); else trsToMat4(md.transform, w);
            LOG("  mesh '%s' world %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f | t %.3f %.3f %.3f", md.name.c_str(), w[0], w[1], w[2],
                w[4], w[5], w[6], w[8], w[9], w[10], w[12], w[13], w[14]);
        }
        for (const auto& a : p.attrs) LOG("    attr @%u size %u role %d fmt %d", a.offset, a.size, static_cast<int>(a.role), static_cast<int>(a.format));
        for (u32 v = 0; v < std::min<u32>(nv, 4); ++v)
            LOG("    v%u pos %.4f,%.4f,%.4f n %.4f,%.4f,%.4f uv %.4f,%.4f uv2 %.4f,%.4f", v, md.positions[v * 3], md.positions[v * 3 + 1],
                md.positions[v * 3 + 2], normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2],
                v * 2 + 1 < md.uvs.size() ? md.uvs[v * 2] : -1.f, v * 2 + 1 < md.uvs.size() ? md.uvs[v * 2 + 1] : -1.f,
                v * 2 + 1 < md.uvs2.size() ? md.uvs2[v * 2] : -1.f, v * 2 + 1 < md.uvs2.size() ? md.uvs2[v * 2 + 1] : -1.f);
        for (u32 v = 0; v < std::min<u32>(nv, 2); ++v)
            if (md.normals.size() >= static_cast<size_t>(nv) * 3 && md.tangents.size() >= static_cast<size_t>(nv) * 4)
                LOG("    v%u authored n %.4f,%.4f,%.4f t %.4f,%.4f,%.4f,%.0f", v, md.normals[v * 3], md.normals[v * 3 + 1], md.normals[v * 3 + 2],
                    md.tangents[v * 4], md.tangents[v * 4 + 1], md.tangents[v * 4 + 2], md.tangents[v * 4 + 3]);
    }
    if (dynamicScene_.count(sceneIndex)) {                  // animated: host-visible, rewritten every frame
        gm.vbo = createBuffer(vbo.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        if (gm.vbo.buf) std::memcpy(gm.vbo.mapped, vbo.data(), vbo.size());
        gm.dynamic = true;
    } else {
        gm.vbo = createDeviceBuffer(vbo.data(), vbo.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    gm.ibo = createDeviceBuffer(indices->data(), indices->size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    gm.indexCount = static_cast<uint32_t>(indices->size());
    if (!gm.vbo.buf || !gm.ibo.buf) return drop("no memory for its vertices");

    if (md.hasWorldMatrix) std::memcpy(gm.world, md.worldMatrix, 64);
    else trsToMat4(md.transform, gm.world);
    std::memcpy(gm.baseWorld, gm.world, 64);
    gm.posGen = md.posGen; gm.uvGen = md.uvGen;   // (the vertices just built)
    std::memcpy(gm.uvXform, md.uvXform, sizeof gm.uvXform);
    std::memcpy(gm.color, md.curTint, 16);
    gm.blend = md.useBlend ? (md.additive ? 2 : 1) : 0;
    // Cut-outs: an opaque surface whose shader samples an alpha mask (desert balloons), or a pass the SHAD marks
    // alpha-to-coverage (palm foliage), is a solid, depth-writing cut-out, never a see-through blend.
    bool maskCutout = false;
    for (const auto& r : p.sets[2]) if (has(lower(r.name), "alphamask")) maskCutout = true;
    gm.cull = false;                                    // HSR Studio: cullMode none for every pipeline
    gm.skybox = md.isSkybox;
    gm.alphaToCoverage = (md.alphaToCoverage || (maskCutout && gm.blend == 0)) && !std::getenv("FH_NO_A2C");
    if (gm.alphaToCoverage && gm.blend == 1) gm.blend = 0;
    {   // one line per shader: the pipeline state this home's meshes get
        static std::set<std::string> seen;
        if (seen.insert(md.shaderPath).second)
            LOG("  pipeline state %s: blend %d a2c %d (first mesh '%s')", md.shaderPath.c_str(), gm.blend, gm.alphaToCoverage ? 1 : 0, md.name.c_str());
    }
    // Compile this mesh's pipeline now, while loading: compiling on first sight froze the frame whenever new
    // objects came into view (the pipeline cache makes it near-instant on later opens).
    measureMesh(gm, md, nv);
    std::map<uint32_t, std::vector<u8>> set2Bytes;   // its constant uniform buffers, for variantFor
    std::vector<uint64_t> lookSig;                    // what its material buffers and textures are (duplicate check)

    // set2: per-material resources, routed by name.
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.setLayouts[2];
    if (vkAllocateDescriptorSets(device_, &ai, &gm.set2) != VK_SUCCESS) { gm.set2 = VK_NULL_HANDLE; return drop("no descriptor set"); }
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<std::vector<VkDescriptorBufferInfo>> bufs;
    std::vector<std::vector<VkDescriptorImageInfo>> imgs;
    bufs.reserve(p.sets[2].size());
    imgs.reserve(p.sets[2].size());
    for (const auto& r : p.sets[2]) {
        std::string n = lower(r.name);
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = gm.set2;
        w.dstBinding = r.binding;
        w.descriptorCount = r.count;
        w.descriptorType = descriptorType(r.kind);
        if (isBuffer(r.kind)) {
            // (routed and filled by shader_bake.h, as the bake fills them for the shaders it specializes)
            const shaderbake::Set2 kind = shaderbake::set2Kind(r, p.boned);
            const bool skinning = kind == shaderbake::Set2::Bones;
            VkDeviceSize size = skinning ? 256 * 64 : std::max<VkDeviceSize>(256, (r.size + 255) & ~255u);
            Buffer b = createBuffer(size, r.kind == spv::ResKind::StorageBuffer ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                                                                 : VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
            if (!b.buf) return drop("no memory for its material buffers");
            float* f = static_cast<float*>(b.mapped);
            if (skinning) gm.skinUbo = static_cast<int>(gm.ubos.size());
            if (debugMesh(md)) {
                std::string ms;
                for (const auto& m : r.members) ms += m.name + "@" + std::to_string(m.offset) + " ";
                LOG("  set2.b%u buffer '%s' (%u bytes): %s", r.binding, r.name.c_str(), r.size, ms.c_str());
            }
            if (skinning) {
                for (int j = 0; j < 256; ++j) { float* m = f + j * 16; m[0] = m[5] = m[10] = m[15] = 1; }
                if (!md.skinJoints.empty())
                    std::memcpy(f, md.skinJoints.data(), std::min<size_t>(md.skinJoints.size(), 256 * 16) * sizeof(float));   // Clay: posed
            } else {
                shaderbake::fillSet2(r, kind, md, p.path, ambient_, f);
                if (kind == shaderbake::Set2::Material && debugMesh(md)) {
                    std::string vals;
                    for (const auto& m : r.members) { char b[64]; std::snprintf(b, sizeof b, "%s=%.3g ", m.name.c_str(), f[m.offset / 4]); vals += b; }
                    LOG("  matParams '%s': %s", md.name.c_str(), vals.c_str());
                }
            }
            if (debugMesh(md) && !skinning) {
                std::string all;
                char num[32];
                for (uint32_t k = 0; k * 4 < r.size && k < 64; ++k) { std::snprintf(num, sizeof num, "%s%g", k ? (k % 4 ? "," : ", ") : "", f[k]); all += num; }
                LOG("  set2.b%u '%s' = %s", r.binding, r.name.c_str(), all.c_str());
            }
            bufs.emplace_back(r.count, VkDescriptorBufferInfo{b.buf, 0, VK_WHOLE_SIZE});
            w.pBufferInfo = bufs.back().data();
            gm.ubos.push_back(b);
            if (!skinning && r.kind == spv::ResKind::UniformBuffer && r.size)
                set2Bytes[r.binding].assign(static_cast<const u8*>(b.mapped), static_cast<const u8*>(b.mapped) + r.size);
            if (!skinning) {
                uint64_t h = 0xcbf29ce484222325ull ^ r.binding;
                const u8* bytes = static_cast<const u8*>(b.mapped);
                for (size_t k = 0, n = std::min<size_t>(r.size, static_cast<size_t>(b.size)); k < n; ++k) h = (h ^ bytes[k]) * 0x100000001B3ull;
                lookSig.push_back(h);
            }
        } else {
            VkDescriptorImageInfo ii = {};
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (r.kind == spv::ResKind::Sampler || r.kind == spv::ResKind::CombinedImageSampler)
                ii.sampler = has(n, "shadow") ? shadowSampler_ : anisoSampler_;
            if (r.kind != spv::ResKind::Sampler) {
                slotPending_ = false;
                ii.imageView = textureFor(r, gm, md);
                // (a placeholder stands for the texture it waits for: two meshes are alike when they wait for the same one)
                lookSig.push_back(reinterpret_cast<uintptr_t>(ii.imageView) ^ (static_cast<uint64_t>(r.binding) << 56));
                if (slotPending_) {                  // a placeholder for now: patch this binding when it lands
                    lookSig.push_back(reinterpret_cast<uintptr_t>(pendingKey_.first) * 2 + (pendingKey_.second ? 1 : 0));
                    waiting_[pendingKey_].push_back({meshes_.size(), r.binding, r.count, descriptorType(r.kind), ii.sampler});
                    slotPending_ = false;
                }
                if (debugMesh(md))
                    LOG("  set2.b%u '%s' dim=%d -> %s", r.binding, r.name.c_str(), static_cast<int>(r.dim),
                        ii.imageView == white_.view ? "white" : ii.imageView == black_.view ? "black"
                        : ii.imageView == flatNormal_.view ? "flatNormal" : ii.imageView == white3D_.view ? "white3D" : "texture");
            }
            imgs.emplace_back(r.count, ii);
            w.pImageInfo = imgs.back().data();
        }
        if (r.kind == spv::ResKind::StorageImage) continue;
        writes.push_back(w);
    }
    if (!writes.empty()) vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    gm.variant = variantFor(p, set2Bytes);
    // Compile this mesh's pipeline now, while loading: compiling on first sight froze the frame whenever new
    // objects came into view (the pipeline cache makes it near-instant on later opens).
    const auto tp0 = std::chrono::steady_clock::now();
    pipelineFor(p, gm.blend, gm.cull, gm.skybox, gm.alphaToCoverage, kCutNone, gm.variant);
    if (gm.alphaToCoverage && gm.blend == 0 && (gm.variant >= 0 ? p.variants[gm.variant].fsAlpha : p.fsAlpha)) {
        // ...and a cut-out's two passes (render(): its depth from the alpha-only shader, then shaded): made on first
        // sight, they were a compile in the middle of a frame the first time a fern came into view.
        pipelineFor(p, gm.blend, gm.cull, gm.skybox, gm.alphaToCoverage, kCutDepth, gm.variant);
        pipelineFor(p, gm.blend, gm.cull, gm.skybox, gm.alphaToCoverage, kCutShade, gm.variant);
    }
    g_loadPipeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp0).count();
    // An exact copy of a solid mesh already in (the same vertices in the same place, the same shader, material
    // values and textures): some APKs ship a part twice (the hand tutorials' chapters and "archive" copies), and the
    // copy, at the same depth, shaded every one of its pixels a second time to the same colour. It stays loaded (a
    // home edit can hide or move the first one) but isn't drawn while the first shows it (render()).
    static const bool noDedupe = std::getenv("FH_NO_DEDUPE") != nullptr;   // A/B: every copy drawn
    if (!noDedupe && !gm.dynamic && !gm.skybox && gm.blend == 0 && gm.skinUbo < 0 && !md.hasVat) {
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&h](const void* p, size_t n) {
            const u8* b = static_cast<const u8*>(p);
            for (size_t k = 0; k < n; ++k) h = (h ^ b[k]) * 0x100000001B3ull;
        };
        mix(vbo.data(), vbo.size());
        mix(indices->data(), indices->size() * 4);
        mix(gm.world, 64); mix(gm.color, 16); mix(gm.uvXform, sizeof gm.uvXform);
        const int state[4] = {gm.program, gm.variant, gm.alphaToCoverage ? 1 : 0, gm.cull ? 1 : 0};
        mix(state, sizeof state);
        mix(lookSig.data(), lookSig.size() * 8);
        const auto it = dedupe_.find(h);
        if (it == dedupe_.end()) {
            dedupe_[h] = meshes_.size();
        } else {   // (the hash matched: the geometry checked in full before trusting it)
            const GpuMesh& o = meshes_[it->second];
            const MeshData& om = scene_->meshes[o.sceneIndex];
            if (o.program == gm.program && o.variant == gm.variant && o.indexCount == gm.indexCount && !std::memcmp(o.world, gm.world, 64) &&
                om.positions == md.positions && om.uvs == md.uvs && om.indices == md.indices) {
                gm.dupOf = static_cast<int>(it->second);
                ++loadDuplicates_;
            }
        }
    }
    // (see-through ones too: the hand tutorials' thousand-odd plants are alpha-blended; FH_NO_BLEND_BATCH=1 for A/B)
    static const bool noBlendBatch = std::getenv("FH_NO_BLEND_BATCH") != nullptr;
    if (p.clay && !gm.dynamic && !gm.skybox && (gm.blend == 0 || !noBlendBatch) && gm.skinUbo < 0 && !md.hasVat && gm.dupOf < 0) {   // (buildBatches)
        uint64_t h = 0xcbf29ce484222325ull;
        auto mix = [&h](const void* q, size_t n) {
            const u8* b = static_cast<const u8*>(q);
            for (size_t k = 0; k < n; ++k) h = (h ^ b[k]) * 0x100000001B3ull;
        };
        const int state[5] = {gm.program, gm.variant, gm.alphaToCoverage ? 1 : 0, gm.cull ? 1 : 0, gm.blend};
        mix(state, sizeof state); mix(gm.color, 16); mix(gm.uvXform, sizeof gm.uvXform);
        mix(lookSig.data(), lookSig.size() * 8);
        gm.lookHash = h | 1;
    }
    sceneToMesh_[sceneIndex] = meshes_.size();
    meshes_.push_back(std::move(gm));
    return true;
}

// A mesh's fragment shader with the uniform values that never change for it baked in (shader_bake.h): its material's
// buffers (set 2), the scene's lighting (set 1) and the frame globals every frame writes the same (set 0). The
// compiler then drops what those make dead (a layer mixed in by 0, a branch on a flag). A .fenv carries them made
// (the bake's, found by the key of these values); a home opened from its APK gets them made here. FH_NO_SPECIALIZE=1:
// the shaders as shipped.
int HslRenderer::variantFor(Program& p, const std::map<uint32_t, std::vector<u8>>& set2) {
    static const bool off = std::getenv("FH_NO_SPECIALIZE") != nullptr;
    if (off || p.foldBlocks.empty() || !p.src || p.src->frag.empty()) return -1;
    static const char* only = std::getenv("FH_SPEC_ONLY");                 // diagnostics: programs whose path holds this alone
    static const int limit = std::getenv("FH_SPEC_LIMIT") ? std::atoi(std::getenv("FH_SPEC_LIMIT")) : -1;   // the first n loads
    if (only && p.path.find(only) == std::string::npos) return -1;
    const int floats = shaderbake::floatsMode();
    const spvspec::Fetch fetch = shaderbake::fetchFor(sceneConsts_, p.sets, set2);
    const uint64_t key = shaderbake::variantKey(p.foldBlocks, fetch, floats) ^ (limit >= 0 ? 0x9E3779B97F4A7C15ull * (limit + 1) : 0);
    if (const auto it = p.variantFor.find(key); it != p.variantFor.end()) return it->second;
    auto module = [&](const std::vector<u32>& specialized) {
        const std::vector<u32> c = spvopt::run(specialized, p.path);   // (spv_opt.h)
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = c.size() * 4;
        ci.pCode = c.data();
        VkShaderModule sm = VK_NULL_HANDLE;
        if (c.empty() || !shaderOk(c, p.path) || vkCreateShaderModule(device_, &ci, nullptr, &sm) != VK_SUCCESS) sm = VK_NULL_HANDLE;
        return sm;
    };
    std::vector<u32> made, madeAlpha;
    const std::vector<u32>* code = nullptr;
    const std::vector<u32>* alpha = nullptr;
    bool baked = false;
    for (const auto& bv : p.src->variants)
        if (bv.key == key && limit < 0 && shaderbake::bakedUsable(bv.frag)) { code = &bv.frag; alpha = &bv.fragAlpha; baked = true; break; }
    uint32_t folded = 0;
    if (!baked) {
        made = spvspec::specialize(p.src->frag, fetch, &folded, limit, floats);
        if (folded) madeAlpha = shaderbake::alphaOnly(made);   // (its own: the cut-out depth pass must cover what it shades)
        else made.clear();
        code = &made;
        alpha = &madeAlpha;
        ++specMade_;
    } else {
        ++specBaked_;
    }
    int v = -1;
    if (!code->empty()) {
        Program::Variant var;
        var.fs = module(*code);
        if (var.fs) {
            if (!alpha->empty()) var.fsAlpha = module(*alpha);
            if (const char* dir = std::getenv("FH_DUMP_SPV")) {   // diagnostics: the specialized shader beside the shipped one
                std::string name = p.path;
                for (char& ch : name) if (ch == '/' || ch == '\\') ch = '_';
                char path[512];
                std::snprintf(path, sizeof path, "%s/%s.var%zu.frag.spv", dir, name.c_str(), p.variants.size());
                if (FILE* f = std::fopen(path, "wb")) { std::fwrite(code->data(), 4, code->size(), f); std::fclose(f); }
            }
            v = static_cast<int>(p.variants.size());
            p.variants.push_back(std::move(var));
            static int logged = 0;
            if (logged++ < 40) LOG("specialized %s: variant %d (%s)", p.path.c_str(), v, baked ? "baked" : "made now");
        }
    }
    p.variantFor[key] = v;
    return v;
}

bool HslRenderer::load(const HslScene& scene) {
    beginLoad(scene);
    while (!loadStep(1e9)) {}
    return !meshes_.empty() || !splatObjs_.empty();
}

void HslRenderer::beginLoad(const HslScene& scene) {
    unload();
    loadDone_ = false;
    scene_ = &scene;
    // Real env ambient when the home ships lightprobes; otherwise the editor's synthesized warm white x1.8.
    for (int k = 0; k < 3; ++k) ambient_[k] = scene.ambientRGB[k] * (scene.hasEnvAmbient ? 1.0f : 1.8f);

    // Lighting (set1), as HSR Studio does it: the engine fills lightUniforms / light items / the light grid at
    // runtime, and a buffer the runtime would fill is ZERO here (no invented sun). Homes are lit by what they ship:
    // baked lightmaps and RBAoDir maps, light probes, and the sky's reflection cube.
    // (zero but for the five HSR fills with 1, for every program's layout of it: the bytes the specialized shaders
    // assume, written once for all of them)
    sceneConsts_ = shaderbake::sceneConstants(scene.programs, globalsSize_, static_cast<uint32_t>(lightUniforms_.size));
    specMade_ = specBaked_ = 0;
    std::memcpy(lightUniforms_.mapped, sceneConsts_.light.data(), std::min<size_t>(sceneConsts_.light.size(), static_cast<size_t>(lightUniforms_.size)));
    std::memset(lightItems_.mapped, 0, static_cast<size_t>(lightItems_.size));
    std::memset(lightGrid_.mapped, 0, static_cast<size_t>(lightGrid_.size));

    const float top[3] = {ambient_[0], ambient_[1], ambient_[2]};
    const float side[3] = {ambient_[0] * 0.65f, ambient_[1] * 0.63f, ambient_[2] * 0.58f};
    const float bottom[3] = {ambient_[0] * 0.30f, ambient_[1] * 0.27f, ambient_[2] * 0.22f};
    iblDiffuse_ = createSolidCube(top, side, bottom);
    // Reflections: the world's own globalIblReflectionCubeMap (SkyboxPlatformComponent.reflectionMap) when it
    // ships one; otherwise a hemisphere estimate from the ambient light.
    const MeshData* sky = nullptr;
    for (const auto& md : scene.meshes) if (md.reflCubeSize) { sky = &md; break; }
    if (sky) {
        const uint32_t size = sky->reflCubeSize, mips = sky->reflCubeMips;
        const auto t0 = std::chrono::steady_clock::now();
        // The shader picks a level by roughness over every level the cube has: Meta's cubes are cooked down to 16^2
        // (256^2, 5 levels; a streamed one like Cascadia's has 1024^2 and 512^2 in its streams, which the loader puts
        // in front). One that still arrives with fewer (an environment baked before the loader did that: mip 0 alone,
        // every floor a mirror) gets the rest of that chain blitted.
        uint32_t levels = 1;
        while ((size >> levels) >= 16) ++levels;
        const TexSource* src = sky->reflCubeSrc.get();
        const VkFormat unorm = src ? astcFormat(src->bw, src->bh, false) : VK_FORMAT_UNDEFINED;
        const char* how = "ASTC HDR";
        if (src && astcHdr_ && mips >= levels && unorm != VK_FORMAT_UNDEFINED)   // complete: sampled as cooked
            iblReflection_ = createAstcTexture(*src, static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK_EXT + (unorm - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2));
        if (!iblReflection_.view) {
            std::vector<u16> decoded;            // still ASTC HDR: every level's faces, decoded across cores
            if (src && unorm != VK_FORMAT_UNDEFINED) {
                std::vector<size_t> in, out;     // per face of every level: its ASTC offset and half-float offset
                size_t a = 0, h = 0;
                for (uint32_t m = 0; m < mips; ++m) {
                    const uint32_t s = std::max(1u, size >> m);
                    const size_t blocks = static_cast<size_t>((s + src->bw - 1) / src->bw) * ((s + src->bh - 1) / src->bh) * 16;
                    for (int f = 0; f < 6; ++f) { in.push_back(a); out.push_back(h); a += blocks; h += static_cast<size_t>(s) * s * 4; }
                }
                in.push_back(a);
                if (a <= src->astc.size()) {
                    decoded.resize(h);
                    std::atomic<bool> ok{true};
                    par::forEach(out.size(), [&](size_t i) {
                        const uint32_t s = std::max(1u, size >> (i / 6));
                        std::vector<u16> face;
                        if (astc::decodeASTCHalf(src->astc.data() + in[i], static_cast<u32>(in[i + 1] - in[i]), s, s, src->bw, src->bh, face) &&
                            face.size() == static_cast<size_t>(s) * s * 4)
                            std::memcpy(decoded.data() + out[i], face.data(), face.size() * 2);
                        else
                            ok = false;
                    });
                    if (!ok) decoded.clear();
                }
            }
            const std::vector<u16>& faces = src ? decoded : sky->reflCubeHalf;
            iblReflection_ = createCubeTexture(faces.data(), faces.size() * 2, size, VK_FORMAT_R16G16B16A16_SFLOAT, mips, levels);
            how = "RGBA16F";
        }
        LOG("IBL reflection cube from the skybox reflectionMap (%u^2, %u mips, %u levels, %s, %.0f ms)", size, mips, std::max(mips, levels), how,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (!iblReflection_.view) {                  // none shipped, or the shipped one was refused
        const float rtop[3] = {ambient_[0] * 0.95f, ambient_[1] * 0.92f, ambient_[2] * 0.88f};
        const float rbot[3] = {ambient_[0] * 0.40f, ambient_[1] * 0.37f, ambient_[2] * 0.32f};
        iblReflection_ = createSolidCube(rtop, side, rbot);
    }

    if (!sel_.lines) createSelectionPipelines();   // now, not on the first click (a 70 ms hitch)
    loadNext_ = 0;
    loadSkipped_ = 0;
    loadDuplicates_ = 0;
    dedupe_.clear();
    loadStart_ = std::chrono::steady_clock::now();
}

// Uploads meshes until `budgetMs` is spent; true once every mesh is in. Lets a window stay responsive
// (and show the home filling in) while a large footprint + vista goes to the GPU.
bool HslRenderer::loadStep(double budgetMs) {
    if (!scene_) return true;
    const auto start = std::chrono::steady_clock::now();
    static const bool noBatch = std::getenv("FH_NO_UPLOAD_BATCH") != nullptr;   // diagnostics: the old one-at-a-time path
    while (loadNext_ < scene_->meshes.size()) {
        const auto tm = std::chrono::steady_clock::now();
        g_loadPipeMs = 0;
        if (!noBatch) beginUploadBatch();
        if (!uploadMesh(scene_->meshes[loadNext_], static_cast<int>(loadNext_))) ++loadSkipped_;
        if (loadStaged_ > (64ull << 20)) flushUploadBatch();   // bounded staging memory
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm).count();
        if (took > 30.0)
            LOG("slow mesh upload %.1f ms (pipeline %.1f): '%s' [%s]", took, g_loadPipeMs,
                scene_->meshes[loadNext_].name.c_str(), scene_->meshes[loadNext_].shaderPath.c_str());
        ++loadNext_;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budgetMs) break;
    }
    flushUploadBatch();
    if (loadNext_ < scene_->meshes.size()) return false;
    if (!splatsUploaded_) uploadSplats();               // (hsl_renderer_splat.cpp)
    if (loadNext_ == scene_->meshes.size() && !loadDone_) {
        loadDone_ = true;
        buildBatches();
        size_t variants = 0;
        for (const auto& p : programs_) variants += p.variants.size();
        size_t batched = 0;
        for (const Batch& b : batches_) batched += b.members.size();
        LOG("uploaded %zu meshes (%zu skipped, %zu exact copies of another not drawn, %zu drawn as %zu batches), %zu shader programs (%zu specialized: %zu baked, %zu made now) in %.1fs",
            meshes_.size(), loadSkipped_, loadDuplicates_, batched, batches_.size(), programs_.size(), variants, specBaked_, specMade_, std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart_).count());
        if (spvopt::flags()) {
            const spvopt::Stats st = spvopt::stats();
            LOG("spir-v optimizer (flags %d): %u modules made in %.0f ms, %u reused, %u failed", spvopt::flags(), st.made, st.ms, st.cached, st.failed);
        }
    }
    return true;
}

int HslRenderer::addMesh(int sceneIndex) {
    if (!device_ || !scene_ || !loadDone_ || recordInto_ || sceneIndex < 0 || sceneIndex >= static_cast<int>(scene_->meshes.size())) return -1;
    beginUploadBatch();
    const bool ok = uploadMesh(scene_->meshes[sceneIndex], sceneIndex);
    flushUploadBatch();
    loadNext_ = scene_->meshes.size();                     // (nothing more for loadStep to bring in)
    if (!ok) return -1;
    GpuMesh& gm = meshes_.back();
    gm.batch = -1;                                         // drawn alone (batches are made once, at load)
    gm.dupOf = -1;                                         // (a copy pasted over its original still draws once moved)
    requestTextures(gm);
    return static_cast<int>(meshes_.size()) - 1;
}

void HslRenderer::reloadSplats() {
    if (!device_ || !scene_) return;
    vkDeviceWaitIdle(device_);
    destroySplats();
    uploadSplats();
}

float HslRenderer::loadProgress() const {
    if (!scene_ || scene_->meshes.empty()) return 1.0f;
    return static_cast<float>(loadNext_) / static_cast<float>(scene_->meshes.size());
}

void HslRenderer::unload() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    for (Buffer& b : frameStaging_) destroy(b);
    frameStaging_.clear();
    destroySplats();
    for (auto& m : meshes_) {
        destroy(m.vbo);
        destroy(m.ibo);
        for (auto& b : m.ubos) destroy(b);
        for (auto& i : m.images) destroy(i);
        if (m.set2) vkFreeDescriptorSets(device_, descPool_, 1, &m.set2);
    }
    destroy(sel_.vbo); destroy(sel_.edges); destroy(sel_.tris);   // the selection pointed into the old home
    sel_.mesh = -1;
    sel_.edgeCount = sel_.triCount = 0;
    meshes_.clear();
    for (Batch& b : batches_) {   // (its set2 is its first part's, freed above; a sorted one's ibo is one of its sortIbo)
        destroy(b.gm.vbo);
        if (b.sorted) { for (Buffer& sb : b.sortIbo) destroy(sb); } else destroy(b.gm.ibo);
    }
    batches_.clear();
    dedupe_.clear();
    cost_.clear();
    costMin_.clear(); costSeen_.clear(); std::fill(std::begin(costBaseRing_), std::end(costBaseRing_), 1e9f);
    doubled_ = -1;
    sceneToMesh_.clear();
    {                                            // drop queued / finished decodes of the old home
        std::lock_guard<std::mutex> l(jobMutex_);
        ++generation_;
        jobs_.clear();
        for (TexDone& d : done_) if (d.ok) { destroy(d.prep.img); destroy(d.prep.staging); }
        done_.clear();
    }
    streamed_.clear();
    waiting_.clear();
    pendingSrc_.clear();
    requested_.clear();
    keepAlive_.clear();
    for (auto& kv : textureCache_) destroy(kv.second);
    textureCache_.clear();
    for (auto& p : programs_) {
        for (auto& kv : p.pipelines) if (kv.second) vkDestroyPipeline(device_, kv.second, nullptr);
        VkDescriptorSet s[2] = {p.set0, p.set1};
        vkFreeDescriptorSets(device_, descPool_, 2, s);
        vkDestroyPipelineLayout(device_, p.layout, nullptr);
        for (auto l : p.setLayouts) vkDestroyDescriptorSetLayout(device_, l, nullptr);
        vkDestroyShaderModule(device_, p.vs, nullptr);
        vkDestroyShaderModule(device_, p.fs, nullptr);
        if (p.fsAlpha) vkDestroyShaderModule(device_, p.fsAlpha, nullptr);
        for (auto& v : p.variants) {
            for (auto& kv : v.pipelines) if (kv.second) vkDestroyPipeline(device_, kv.second, nullptr);
            if (v.fs) vkDestroyShaderModule(device_, v.fs, nullptr);
            if (v.fsAlpha) vkDestroyShaderModule(device_, v.fsAlpha, nullptr);
        }
    }
    programs_.clear();
    programIndex_.clear();
    destroy(iblDiffuse_);
    destroy(iblReflection_);
    scene_ = nullptr;
}

void HslRenderer::setMeshWorld(size_t i, const float world[16]) {
    GpuMesh& gm = meshes_[i];
    if (gm.batch >= 0 && std::memcmp(gm.baseWorld, world, 64) != 0) batches_[gm.batch].active = false;   // (drawn alone again)
    std::memcpy(gm.baseWorld, world, 64);
    placeMesh(gm);
}

// The mesh's local centroid and box from its positions, then placed.
void HslRenderer::measureMesh(GpuMesh& gm, const MeshData& md, uint32_t nv) {
    nv = static_cast<uint32_t>(std::min<size_t>(nv, md.positions.size() / 3));
    double c[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) { gm.localMin[k] = 1e30f; gm.localMax[k] = -1e30f; }
    for (u32 v = 0; v < nv; ++v)
        for (int k = 0; k < 3; ++k) {
            const float x = md.positions[v * 3 + k];
            c[k] += x;
            gm.localMin[k] = std::min(gm.localMin[k], x);
            gm.localMax[k] = std::max(gm.localMax[k], x);
        }
    for (int k = 0; k < 3; ++k) gm.localCentroid[k] = nv ? static_cast<float>(c[k] / nv) : 0.0f;
    placeMesh(gm);
}

// world = baseWorld * anim, and the centroid and bounds that follow it.
void HslRenderer::placeMesh(GpuMesh& gm) {
    mat4mul4(gm.baseWorld, gm.anim, gm.world);
    const float* w = gm.world;
    for (int k = 0; k < 3; ++k)
        gm.centroid[k] = w[k] * gm.localCentroid[0] + w[4 + k] * gm.localCentroid[1] + w[8 + k] * gm.localCentroid[2] + w[12 + k];
    updateBounds(gm);
}

// Static batching (Batch, hsl_renderer.h): the solid Clay meshes uploadMesh gave a lookHash, grouped by that look and by
// the 4 m cell their centre is in, each group of two or more made one mesh: their vertices moved into the world (the
// shader then places them with an identity matrix), their indices one after another. Big meshes stay alone (one draw
// is nothing next to their pixels, and they'd be held twice in memory).
void HslRenderer::buildBatches() {
    batches_.clear();
    static const bool off = std::getenv("FH_NO_BATCH") != nullptr;   // A/B: every mesh its own draw
    if (off || !scene_) return;
    constexpr u32 kMaxPartVerts = 16384, kMaxBatchVerts = 262144;
    // See-through parts are sorted back to front as one: a smaller cell keeps that order close to theirs one by one.
    static const float kSolidCell = 4.0f, kBlendCell = std::getenv("FH_BLEND_CELL") ? static_cast<float>(std::atof(std::getenv("FH_BLEND_CELL"))) : 2.0f;
    std::map<std::tuple<uint64_t, int, int, int>, std::vector<size_t>> groups;
    for (size_t i = 0; i < meshes_.size(); ++i) {
        const GpuMesh& gm = meshes_[i];
        if (!gm.lookHash || gm.dupOf >= 0 || !gm.visible || gm.sceneIndex < 0 || static_cast<size_t>(gm.sceneIndex) >= scene_->meshes.size()) continue;
        if (meshVerts(scene_->meshes[gm.sceneIndex]) > kMaxPartVerts) continue;
        const float kCell = gm.blend ? std::max(0.25f, kBlendCell) : kSolidCell;
        groups[{gm.lookHash, static_cast<int>(std::floor(gm.centroid[0] / kCell)), static_cast<int>(std::floor(gm.centroid[1] / kCell)),
                static_cast<int>(std::floor(gm.centroid[2] / kCell))}].push_back(i);
    }
    std::vector<u8> vbo, all;
    std::vector<float> normals;
    std::vector<u32> indices;
    for (auto& g : groups) {
        std::vector<size_t>& parts = g.second;
        if (parts.size() < 2) continue;
        const GpuMesh& first = meshes_[parts.front()];
        const Program& p = programs_[first.program];
        int posOff = -1, nrmOff = -1;   // only float positions (and normals) can be moved; anything else stays alone
        bool movable = true;
        for (const auto& a : p.attrs) {
            if (a.role == Role::Pos) { if (a.format == VK_FORMAT_R32G32B32_SFLOAT || a.format == VK_FORMAT_R32G32B32A32_SFLOAT) posOff = static_cast<int>(a.offset); else movable = false; }
            if (a.role == Role::Normal) { if (a.format == VK_FORMAT_R32G32B32_SFLOAT || a.format == VK_FORMAT_R32G32B32A32_SFLOAT) nrmOff = static_cast<int>(a.offset); else movable = false; }
        }
        if (!movable || posOff < 0) continue;
        for (size_t start = 0; start < parts.size();) {
            // as many parts as fit in one batch
            all.clear();
            indices.clear();
            std::vector<size_t> members;
            std::vector<std::pair<u32, u32>> ranges;
            u32 verts = 0;
            for (; start < parts.size(); ++start) {
                const GpuMesh& gm = meshes_[parts[start]];
                const MeshData& md = scene_->meshes[gm.sceneIndex];
                const u32 nv = meshVerts(md);
                if (verts + nv > kMaxBatchVerts && !members.empty()) break;
                if (std::any_of(md.indices.begin(), md.indices.end(), [nv](u32 i) { return i >= nv; })) continue;   // (damaged: alone)
                buildVertices(p, md, vbo, normals);
                if (vbo.size() != static_cast<size_t>(nv) * p.stride) continue;
                // world position; normal by the inverse transpose (the cofactors), as the shader would have
                const float* w = gm.world;
                const float cof[9] = {w[5] * w[10] - w[6] * w[9], w[6] * w[8] - w[4] * w[10], w[4] * w[9] - w[5] * w[8],
                                      w[2] * w[9] - w[1] * w[10], w[0] * w[10] - w[2] * w[8], w[1] * w[8] - w[0] * w[9],
                                      w[1] * w[6] - w[2] * w[5], w[2] * w[4] - w[0] * w[6], w[0] * w[5] - w[1] * w[4]};
                for (u32 v = 0; v < nv; ++v) {
                    u8* base = vbo.data() + static_cast<size_t>(v) * p.stride;
                    float q[3];
                    std::memcpy(q, base + posOff, 12);
                    const float o[3] = {w[0] * q[0] + w[4] * q[1] + w[8] * q[2] + w[12], w[1] * q[0] + w[5] * q[1] + w[9] * q[2] + w[13],
                                        w[2] * q[0] + w[6] * q[1] + w[10] * q[2] + w[14]};
                    std::memcpy(base + posOff, o, 12);
                    if (nrmOff >= 0) {
                        std::memcpy(q, base + nrmOff, 12);
                        float n[3] = {cof[0] * q[0] + cof[3] * q[1] + cof[6] * q[2], cof[1] * q[0] + cof[4] * q[1] + cof[7] * q[2],
                                      cof[2] * q[0] + cof[5] * q[1] + cof[8] * q[2]};
                        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; }
                        std::memcpy(base + nrmOff, n, 12);
                    }
                }
                all.insert(all.end(), vbo.begin(), vbo.end());
                ranges.push_back({static_cast<u32>(indices.size()), static_cast<u32>(md.indices.size())});
                for (u32 i : md.indices) indices.push_back(verts + i);
                verts += nv;
                members.push_back(parts[start]);
            }
            if (members.size() < 2) continue;
            Batch b;
            GpuMesh& bm = b.gm;
            bm.program = first.program; bm.variant = first.variant; bm.set2 = first.set2;
            std::memcpy(bm.color, first.color, 16);
            std::memcpy(bm.uvXform, first.uvXform, sizeof bm.uvXform);
            bm.blend = first.blend; bm.cull = first.cull; bm.alphaToCoverage = first.alphaToCoverage;
            b.sorted = first.blend != 0;
            const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            std::memcpy(bm.baseWorld, id, 64);
            for (int k = 0; k < 3; ++k) { bm.localMin[k] = 1e30f; bm.localMax[k] = -1e30f; }
            for (size_t m : members)
                for (int k = 0; k < 3; ++k) {
                    bm.localMin[k] = std::min(bm.localMin[k], meshes_[m].worldMin[k]);
                    bm.localMax[k] = std::max(bm.localMax[k], meshes_[m].worldMax[k]);
                }
            for (int k = 0; k < 3; ++k) bm.localCentroid[k] = 0.5f * (bm.localMin[k] + bm.localMax[k]);
            placeMesh(bm);
            beginUploadBatch();
            bm.vbo = createDeviceBuffer(all.data(), all.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            if (b.sorted) {   // (written each frame the order changes: sortBatch)
                for (Buffer& sb : b.sortIbo) sb = createBuffer(indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
                if (b.sortIbo[0].mapped && b.sortIbo[1].mapped) bm.ibo = b.sortIbo[0];
                b.indices = indices;
                b.ranges = std::move(ranges);
            } else {
                bm.ibo = createDeviceBuffer(indices.data(), indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            }
            flushUploadBatch();
            bm.indexCount = b.sorted ? 0 : static_cast<uint32_t>(indices.size());
            if (!bm.vbo.buf || !bm.ibo.buf) {   // (no memory: the parts draw alone)
                destroy(bm.vbo);
                if (b.sorted) { for (Buffer& sb : b.sortIbo) destroy(sb); } else destroy(bm.ibo);
                continue;
            }
            for (size_t m : members) meshes_[m].batch = static_cast<int>(batches_.size());
            b.members = std::move(members);
            batches_.push_back(std::move(b));
        }
    }
}

// A see-through batch this frame (render(), after culling, meshes' inView still their own): the parts in view, far to
// near by their centres as render() orders see-through meshes, their indices written in that order into this command
// buffer's index buffer when it holds anything else (it was last read two frames ago: that frame's fence was waited).
void HslRenderer::sortBatch(Batch& b, const float head[3]) {
    std::vector<std::pair<float, u32>> order;
    order.reserve(b.members.size());
    for (u32 k = 0; k < b.members.size(); ++k) {
        const GpuMesh& gm = meshes_[b.members[k]];
        if (!gm.inView) continue;
        float d = 0;
        for (int c = 0; c < 3; ++c) d += (gm.centroid[c] - head[c]) * (gm.centroid[c] - head[c]);
        order.push_back({d, k});
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    const int s = cmdIndex_ & 1;
    std::vector<u32>& was = b.written[s];
    bool same = was.size() == order.size();
    for (size_t k = 0; same && k < order.size(); ++k) same = was[k] == order[k].second;
    if (!same) {
        u32* dst = static_cast<u32*>(b.sortIbo[s].mapped);
        u32 n = 0;
        was.clear();
        for (const auto& o : order) {
            const auto& r = b.ranges[o.second];
            std::memcpy(dst + n, b.indices.data() + r.first, static_cast<size_t>(r.second) * 4);
            n += r.second;
            was.push_back(o.second);
        }
        b.writtenCount[s] = n;
    }
    b.gm.ibo = b.sortIbo[s];
    b.gm.indexCount = b.writtenCount[s];
}

void HslRenderer::resize(uint32_t eyeWidth, uint32_t eyeHeight) {
    if (!device_ || (eyeWidth == width_ && eyeHeight == height_) || !eyeWidth || !eyeHeight) return;
    vkDeviceWaitIdle(device_);
    for (VkImageView& v : layerViews_) { if (v) vkDestroyImageView(device_, v, nullptr); v = VK_NULL_HANDLE; }
    vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    destroyExportFramebuffers();                 // (they hold the old attachments: new ones come with new export images)
    if (fovFb_) { vkDestroyFramebuffer(device_, fovFb_, nullptr); fovFb_ = VK_NULL_HANDLE; }
    for (Image* i : {&colorMs_, &depth_, &resolve_, &fdm_, &sub_}) destroy(*i);
    width_ = eyeWidth;
    height_ = eyeHeight;
    createTargets();
}

// ── Frame ────────────────────────────────────────────────────────────────────────────────────────

void HslRenderer::updateGlobals(const EyeView eyes[2], float timeSec) {
    u8* g = static_cast<u8*>(globals_.mapped);
    std::memset(g, 0, globalsSize_);
    auto put = [&](uint32_t off, const void* src, size_t n) { if (off + n <= globalsSize_) std::memcpy(g + off, src, n); };
    Mat4 vp[2], skyVp[2];
    for (int e = 0; e < 2; ++e) {
        mat4mul4(eyes[e].proj.m, eyes[e].view.m, vp[e].m);
        Mat4 rot = eyes[e].view;                        // the sky: rotation only, it never gets closer
        rot.m[12] = rot.m[13] = rot.m[14] = 0;
        mat4mul4(eyes[e].proj.m, rot.m, skyVp[e].m);
    }
    for (const auto& m : globalMembers_) {
        const std::string n = lower(m.name);
        const bool pair = m.size >= 128;
        if (has(n, "skyboxclipfromworld")) {
            put(m.offset, skyVp[0].m, 64);
            if (pair) put(m.offset + 64, skyVp[1].m, 64);
        } else if (has(n, "clipfromworld")) {           // clipFromWorld[2]
            put(m.offset, vp[0].m, 64);
            if (pair) put(m.offset + 64, vp[1].m, 64);
        } else if (has(n, "viewfromworld")) {
            put(m.offset, eyes[0].view.m, 64);
            if (pair) put(m.offset + 64, eyes[1].view.m, 64);
        } else if (has(n, "worldcamerapos")) {
            float p0[4] = {eyes[0].position[0], eyes[0].position[1], eyes[0].position[2], 1};
            float p1[4] = {eyes[1].position[0], eyes[1].position[1], eyes[1].position[2], 1};
            put(m.offset, p0, 16);
            if (m.size >= 32) put(m.offset + 16, p1, 16);
        } else if (n == "reversez") {                    // HSR: 1 (the corpus only authors Greater depth compares)
            float one = 1;
            // HSR sets 1 with its own sky projection; with this renderer's head-locked sky cube, 1 clips the sky away.
            static const bool revZ = std::getenv("FH_REVERSEZ") != nullptr;
            if (revZ) put(m.offset, &one, 4);
        } else if (n == "fogcolor" || n == "fogstart" || n == "fogdensity") {
            SceneFog fog;
            {
                std::lock_guard<std::mutex> l(g_sceneFogMutex);
                fog = g_sceneFog;
            }
            static const bool noFog = std::getenv("FH_NO_FOG") != nullptr;
            if (noFog) fog = SceneFog{};
            static bool fogLogged = false;
            if (!fogLogged) {
                fogLogged = true;
                for (const auto& gmem : globalMembers_) LOG("globals member %s @%u (%u bytes)", gmem.name.c_str(), gmem.offset, gmem.size);
                LOG("fog: has %d travel %d color %.3f %.3f %.3f %.3f start %.3f density %.5f (member %s %u bytes)", fog.has, fog.travel,
                    fog.color[0], fog.color[1], fog.color[2], fog.color[3], fog.start, fog.density, m.name.c_str(), m.size);
            }
            if (n == "fogcolor") put(m.offset, fog.color, std::min<size_t>(16, m.size ? m.size : 16));
            else put(m.offset, n == "fogstart" ? &fog.start : &fog.density, 4);
        } else if (n == "whitepoint") {
            float one = 1; put(m.offset, &one, 4);
        } else if (n == "time" || n == "worldtime") {
            put(m.offset, &timeSec, 4);
        } else if (has(n, "viewportsize")) {
            float s[2] = {static_cast<float>(width_), static_cast<float>(height_)}; put(m.offset, s, 8);
        } else if (has(n, "defaultlightprobes")) {
            // left zero, as in HSR Studio (not part of the frame globals it fills)
        }
    }
}

// Each eye's full-detail centre on the density map grid, and the map they and the level make.
void HslRenderer::fdmCentres(int32_t cx[2], int32_t cy[2]) const {
    for (uint32_t e = 0; e < 2; ++e) {
        const uint32_t ee = std::min(e, viewCount - 1);
        const float px = fovCenter_[ee][0] >= 0 ? fovCenter_[ee][0] : width_ * 0.5f, py = fovCenter_[ee][1] >= 0 ? fovCenter_[ee][1] : height_ * 0.5f;
        cx[e] = static_cast<int32_t>(std::lround(px / fdmTexel_));
        cy[e] = static_cast<int32_t>(std::lround(py / fdmTexel_));
    }
}

int64_t HslRenderer::fdmWantKey() const {
    int32_t cx[2], cy[2];
    fdmCentres(cx, cy);
    return (static_cast<int64_t>(std::lround(fovStrength_ * 32)) << 48) | (static_cast<int64_t>(cx[0] & 0xFFF) << 36) | (static_cast<int64_t>(cy[0] & 0xFFF) << 24) |
           (static_cast<int64_t>(cx[1] & 0xFFF) << 12) | static_cast<int64_t>(cy[1] & 0xFFF);
}

// Whether this frame can be recorded before the last one is done (render()): nothing it records may change what
// that frame still reads. Not with decoded textures to upload (the upload ring, descriptor sets), debug views or a
// selection (their buffers), or a density map to rewrite or just rewritten (Turnip reads it as a pass is recorded).
bool HslRenderer::canRecordEarly() {
    static const bool off = std::getenv("FH_NO_EARLY_RECORD") != nullptr;
    static const bool why = std::getenv("FH_EARLY_WHY") != nullptr;   // diagnostics: what keeps frames from it
    static uint32_t asked = 0;
    auto no = [&](const char* reason) {
        if (why && asked++ % 300 == 0) LOG("frame not recorded early: %s", reason);
        return false;
    };
    if (!earlyRecord_ || off) return false;
    if (fenceUnsignalled_ || !scene_ || !loadDone_) return no("not loaded");
    if (preloading_) return no("textures still to load");   // (others wait to be seen: requesting one records nothing)
    if (debugView_ != kViewNormal || sel_.mesh >= 0) return no("a debug view or selection");
    {
        std::lock_guard<std::mutex> l(jobMutex_);   // (decoded textures waiting to go up)
        if (!done_.empty()) return no("decoded textures to upload");
    }
    if (fdmOn_ && !fdmOffset_ && fdm_.image && (!fdmInit_ || fdmCopyRecorded_ || fdmWantKey() != fdmKey_)) return no("the density map changes");
    return true;
}

void HslRenderer::render(const EyeView eyes[2], float timeSec) {
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto t0 = clk::now();
    if (lost_) return;                          // the GPU reset and took this device with it: nothing draws again
    clk::time_point t1, ta, tb, tg, tc, t2, t3;
    uint32_t pipeBinds = 0, draws = 0;
    uint64_t tris = 0;
    // Early recording (system mode, steady state): this frame is culled and recorded while the GPU still draws the
    // last one, and only then does it wait for it, write what the GPU reads (globals, bones, animated vertices) and
    // go: the GPU waits a few tenths of a millisecond between frames, not the whole recording. Anything that changes
    // what the last frame may still read (texture uploads, a new foveation map, debug views) records the usual way.
    const bool early = canRecordEarly();
    cmdIndex_ ^= 1;                             // two command buffers: one can be recorded while the other runs
    cmd_ = cmds_[cmdIndex_];
    auto prep = [&]() -> bool {
    // (a frame that failed to submit never signals its fence: waiting for it would never return)
    if (!fenceUnsignalled_ && vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_ERROR_DEVICE_LOST) {
        lost_ = true;
        LOG("GPU device lost (the GPU was reset): this renderer can't draw any more");
        return false;
    }
    fenceUnsignalled_ = false;
    vkResetFences(device_, 1, &fence_);
    fdmCopyRecorded_ = false;                   // (the frame that rewrote the density map is done)
    if (perfPool_ && perfRecorded_) {
        std::vector<VkPerformanceCounterResultKHR> res(perfNames_.size());
        if (vkGetQueryPoolResults(device_, perfPool_, 0, 1, res.size() * sizeof(res[0]), res.data(), res.size() * sizeof(res[0]), 0) == VK_SUCCESS) {
            for (size_t i = 0; i < res.size(); ++i) {
                const VkPerformanceCounterStorageKHR st = perfStorage_[i];
                perfSum_[i] += st == VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR ? res[i].int32 : st == VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR ? static_cast<double>(res[i].int64)
                             : st == VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR ? res[i].uint32 : st == VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR ? static_cast<double>(res[i].uint64)
                             : st == VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR ? res[i].float32 : res[i].float64;
            }
            ++perfFrames_;
        }
        perfRecorded_ = false;
    }
    t1 = clk::now();
    if (queryPool_ && profFrame_) {
        // kMaxQueries-4: start, -3: uploads done, -2: draws done (render pass ended), -1: end (after the export copy)
        uint64_t ft[4] = {};
        if (vkGetQueryPoolResults(device_, queryPool_, kMaxQueries - 4, 4, sizeof ft, ft, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double up = (ft[1] - ft[0]) * timestampPeriod_ * 1e-6, dr = (ft[3] - ft[1]) * timestampPeriod_ * 1e-6;
            gpuUploadMs_ = static_cast<float>(up);
            gpuCopyMs_ = static_cast<float>((ft[3] - ft[2]) * timestampPeriod_ * 1e-6);
            if (up + dr > 30.0 || ms(t0, t1) > 40.0)
                LOG("GPU frame: uploads %.1f ms (%u textures, %.1f MB), draws %.1f ms; CPU waited %.1f ms; %d allocations so far", up, prevUploads_, prevUploadMB_, dr, ms(t0, t1), g_allocCount.load());
            gpuMs_ = static_cast<float>(up + dr);
            // Cost view on a tiler: the frame just measured either drew mesh doubled_ twice (its cost is what that
            // added) or nothing twice (the baseline). Other work on the GPU (the compositor, another app) only ever adds
            // to a frame's time, so both sides take the least of their recent frames: what the frame costs on its own.
            if (debugView_ == kViewCost && tiler_) {
                if (cost_.size() != meshes_.size()) { cost_.assign(meshes_.size(), -1.0f); costMin_.assign(meshes_.size() * kCostSamples, 1e9f); costSeen_.assign(meshes_.size(), 0); }
                const float ms = static_cast<float>(dr);
                static const bool trace = std::getenv("FH_COST_TRACE") != nullptr;
                static int traced = 0;
                if (trace && traced++ < 60) LOG("cost trace: doubled %d frame %.3f ms (slot %d)", doubled_, ms, timedSlot_);
                if (doubled_ < 0) costBaseRing_[costBaseNext_++ % kCostBase] = ms;
                else if (static_cast<size_t>(doubled_) < cost_.size()) {
                    const size_t i = static_cast<size_t>(doubled_);
                    costMin_[i * kCostSamples + costSeen_[i]++ % kCostSamples] = ms;
                    float base = 1e9f, with = 1e9f;
                    for (float b : costBaseRing_) base = std::min(base, b);
                    for (uint32_t k = 0; k < kCostSamples; ++k) with = std::min(with, costMin_[i * kCostSamples + k]);
                    if (base < 1e8f && costSeen_[i] >= 3) cost_[i] = std::max(0.0f, with - base);   // a few samples before it shows
                }
            }
            ++gpuResults_;
            gpuSum_ += gpuMs_; gpuMax_ = std::max(gpuMax_, gpuMs_);
            if (++gpuFrames_ == 900) {   // every ~10 s at 90 Hz: what a frame costs the GPU here
                LOG("GPU: %.2f ms a frame on average, %.2f ms at worst (%ux%u per eye, %ux MSAA)", gpuSum_ / gpuFrames_, gpuMax_, width_, height_, static_cast<unsigned>(samples_));
                gpuSum_ = gpuMax_ = 0; gpuFrames_ = 0;
            }
        }
    }
    profFrame_ = false;
    prevUploads_ = lastUploads_;
    prevUploadMB_ = lastUploadMB_;
    if (queryPool_ && !profDraws_.empty()) {
        std::vector<uint64_t> ts(profNext_, 0);
        if (vkGetQueryPoolResults(device_, queryPool_, 0, profNext_, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double total = 0;
            std::vector<std::pair<double, size_t>> cost;
            if (debugView_ == kViewCost && cost_.size() != meshes_.size()) cost_.assign(meshes_.size(), -1.0f);
            for (const auto& pd : profDraws_) {
                const double d = (ts[pd.second + viewCount] - ts[pd.second]) * timestampPeriod_ * 1e-6;
                total += d;
                cost.push_back({d, pd.first});
                if (debugView_ == kViewCost && pd.first < cost_.size()) {   // smoothed: one frame's timings jitter
                    float& c = cost_[pd.first];
                    c = c < 0 ? static_cast<float>(d) : c * 0.85f + static_cast<float>(d) * 0.15f;
                }
            }
            if ((total > 30.0 || ms(t0, t1) > 40.0) && debugView_ != kViewCost) {
                std::sort(cost.begin(), cost.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string top;
                for (size_t k = 0; k < cost.size() && k < 6; ++k) {
                    const GpuMesh& gm = meshes_[cost[k].second];
                    char b[256];
                    std::snprintf(b, sizeof b, "\n    %.1f ms  %s  [%s]", cost[k].first,
                                  scene_ ? scene_->meshes[gm.sceneIndex].name.c_str() : "?", programs_[gm.program].path.c_str());
                    top += b;
                }
                LOG("GPU frame %.1f ms over %zu draws; costliest:%s", total, profDraws_.size(), top.c_str());
            }
        }
    }
    profDraws_.clear();
    profNext_ = 0;
    for (Buffer& b : frameStaging_) destroy(b);  // the previous frame's uploads are done
    frameStaging_.clear();
    ringHead_ = 0;                              // ...so is everything they read from the upload ring
    dbg_.used = 0;                              // ...and the debug views' draws
    if (debugView_ != kViewNormal) {
        const uint32_t want = static_cast<uint32_t>(meshes_.size()) * 2 + 16;
        if (dbg_.capacity < want || !dbg_.draws.mapped) {
            destroy(dbg_.draws);
            dbg_.capacity = 0;
            dbg_.draws = createBuffer(static_cast<VkDeviceSize>(want) * 208, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            if (dbg_.draws.mapped) {
                dbg_.capacity = want;
                VkDescriptorBufferInfo bi = {dbg_.draws.buf, 0, VK_WHOLE_SIZE};
                VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = dbg_.set; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
                vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
            }
        }
    }
    ta = clk::now();
    updateAnimations(timeSec);                  // bone matrices for skinned meshes (the GPU is idle here)
    tb = clk::now();
    {
        EyeView late[2];
        updateGlobals(latePose && latePose(late) ? late : eyes, timeSec);
    }
    tg = clk::now();
    refreshDynamicMeshes(!early, true);         // animated Clay / glTF meshes: this frame's vertices + tint
    tc = clk::now();
    stats_.waitMs = ms(t0, t1);
    stats_.readbackMs = ms(t1, ta);
    stats_.animMs = ms(ta, tb);
    stats_.globalsMs = ms(tb, tg);
    stats_.dynamicMs = ms(tg, tc);
    if (ms(t1, tc) > 20.0)
        LOG("slow prep: profiler readback %.1f, animations %.1f, globals %.1f ms", ms(t1, ta), ms(ta, tb), ms(tb, tc));
    return true;
    };
    auto record = [&]() {

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    const bool perfThis = perfPool_ && !perfRecorded_;   // (one frame's counters in flight at a time)
    if (perfThis) { vkCmdResetQueryPool(cmd_, perfPool_, 0, 1); vkCmdBeginQuery(cmd_, perfPool_, 0, 0); }
    // Streamed textures that finished decoding: copied in this frame's command buffer, before the draws that use
    // them — no queue stall (the old path waited for the whole GPU per texture).
    if (queryPool_) {
        vkCmdResetQueryPool(cmd_, queryPool_, 0, kMaxQueries);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, kMaxQueries - 4);
    }
    recordInto_ = cmd_;
    t2 = clk::now();
    lastUploads_ = 0;
    lastUploadMB_ = 0;
    if (!early) pumpTextures(preloading_ ? (uploadsUnseen_ ? 40.0 : 14.0) : 3.0);
    if (preloading_ && waiting_.empty()) {
        preloading_ = false;
        LOG("all %zu textures loaded", texturesRequested_);
        {   // Video memory this process may keep resident vs. what it uses (VK_EXT_memory_budget): over budget,
            // Windows demotes allocations to system RAM and every draw that samples them crawls over PCIe.
            VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 mp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            mp2.pNext = &bud;
            vkGetPhysicalDeviceMemoryProperties2(physical_, &mp2);
            for (uint32_t hI = 0; hI < mp2.memoryProperties.memoryHeapCount; ++hI)
                if (mp2.memoryProperties.memoryHeaps[hI].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    LOG("VRAM heap %u: this app uses %.0f MB of a %.0f MB budget (heap %.0f MB)", hI, bud.heapUsage[hI] / 1048576.0,
                        bud.heapBudget[hI] / 1048576.0, mp2.memoryProperties.memoryHeaps[hI].size / 1048576.0);
        }
    }
    recordInto_ = VK_NULL_HANDLE;
    if (queryPool_) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, kMaxQueries - 3);
    t3 = clk::now();
    VkClearValue clears[3] = {};
    clears[0].color = {{clearColor[0], clearColor[1], clearColor[2], 1}};
    static const char* clearEnv = std::getenv("FH_CLEAR");
    if (const char* c = clearEnv)                         // debug: tell "drawn black" from "not drawn"
        std::sscanf(c, "%f,%f,%f", &clears[0].color.float32[0], &clears[0].color.float32[1], &clears[0].color.float32[2]);
    if (debugView_ == kViewOverdraw) clears[0].color = {{0, 0, 0, 1}};               // layers add up from black
    else if (debugView_ == kViewMeshes) clears[0].color = {{0.03f, 0.03f, 0.035f, 1}};
    clears[1].depthStencil = {0.0f, 0};                 // reversed-Z: far = 0
    clears[2].color = clears[0].color;
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    // exportDirect: the frame goes straight into the compositor's image for this slot (no copy at the end).
    const bool direct = exportDirect && exportSlot >= 0 && static_cast<size_t>(exportSlot) < exportFbs_.size();
    lastExportSlot_ = direct ? exportSlot : -1;
    if (fdmOn_ && !fdmOffset_ && fdm_.image) {
        // Foveation without offsets: the density map itself is redrawn with each eye's full-detail centre where it
        // looks, whenever that moves by a map texel (or the level changes); otherwise the last one stays.
        int32_t cx[2], cy[2];
        fdmCentres(cx, cy);
        const int64_t key = fdmWantKey();
        const size_t texels = static_cast<size_t>(fdmW_) * fdmH_;
        const size_t bpt = fdmFormat_ == VK_FORMAT_R32G32_SFLOAT ? 8 : fdmFormat_ == VK_FORMAT_R16G16_SFLOAT ? 4 : 2;
        const VkDeviceSize bytes = texels * bpt * viewCount, at = (ringHead_ + 15) & ~VkDeviceSize(15);
        if (!fdmInit_) {
            // The first map since the targets were made goes in before this frame is recorded: the driver reads the map
            // as the pass is recorded, so one copied in by this frame's commands would leave this frame with none (black).
            std::vector<uint8_t> px(bytes);
            std::vector<float> dens;
            for (uint32_t l = 0; l < viewCount; ++l) {
                densityTexels(cx[std::min(l, 1u)] + 0.0f, cy[std::min(l, 1u)] + 0.0f, dens);
                packDensity(dens, &px[l * texels * bpt]);
            }
            Buffer staging = createBuffer(px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            if (staging.mapped) {
                std::memcpy(staging.mapped, px.data(), px.size());
                oneShot([&](VkCommandBuffer c) {
                    imageBarrier(c, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy cp = {};
                    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, viewCount};
                    cp.imageExtent = {fdmW_, fdmH_, 1};
                    vkCmdCopyBufferToImage(c, staging.buf, fdm_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
                    imageBarrier(c, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
                });
                destroy(staging);
                fdmInit_ = true;
                fdmKey_ = key;
            }
        }
        if (key != fdmKey_ && at + bytes <= kRingSize && ring_.mapped) {
            uint8_t* dst = static_cast<uint8_t*>(ring_.mapped) + at;
            std::vector<float> dens;
            for (uint32_t l = 0; l < viewCount; ++l) {
                densityTexels(cx[std::min(l, 1u)] + 0.0f, cy[std::min(l, 1u)] + 0.0f, dens);
                packDensity(dens, dst + l * texels * bpt);
            }
            ringHead_ = at + bytes;
            imageBarrier(cmd_, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount,
                         fdmKey_ == -1 && !fdmInit_ ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy cp = {};
            cp.bufferOffset = at;
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, viewCount};
            cp.imageExtent = {fdmW_, fdmH_, 1};
            vkCmdCopyBufferToImage(cmd_, ring_.buf, fdm_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            imageBarrier(cmd_, fdm_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
            fdmKey_ = key;
            fdmInit_ = true;
            fdmCopyRecorded_ = true;
        }
    }
    // Our smoothing pass runs after the foveated one unless the compositor smooths the export as it samples it.
    const bool smoothingPass = smoothFov_ && !(direct && exportsSubsampled());
    rp.renderPass = direct && !smoothingPass ? exportPass_ : renderPass_;
    rp.framebuffer = direct && !smoothingPass ? exportFbs_[exportSlot] : framebuffer_;
    rp.renderArea = {{0, 0}, {width_, height_}};
    rp.clearValueCount = samples_ != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = {0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    VkRect2D sc = {{0, 0}, {width_, height_}};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);

    const float head[3] = {(eyes[0].position[0] + eyes[1].position[0]) * 0.5f,
                           (eyes[0].position[1] + eyes[1].position[1]) * 0.5f,
                           (eyes[0].position[2] + eyes[1].position[2]) * 0.5f};
    // View-frustum culling (either eye): only what can be seen is drawn, and only its textures stream in.
    float planes[2][5][4];
    for (int e = 0; e < 2; ++e) {
        float m[16];
        mat4mul4(eyes[e].proj.m, eyes[e].view.m, m);
        auto row = [&](int r, float* o) { for (int c = 0; c < 4; ++c) o[c] = m[c * 4 + r]; };
        float r0[4], r1[4], r2[4], r3[4];
        row(0, r0); row(1, r1); row(2, r2); row(3, r3);
        for (int c = 0; c < 4; ++c) {
            planes[e][0][c] = r3[c] + r0[c];         // left
            planes[e][1][c] = r3[c] - r0[c];         // right
            planes[e][2][c] = r3[c] + r1[c];         // bottom / top (Vulkan y)
            planes[e][3][c] = r3[c] - r1[c];
            planes[e][4][c] = r3[c] - r2[c];         // near (reversed-Z: z <= w); infinite far
        }
    }
    auto inFrustum = [&](const GpuMesh& gm, int e) {
        for (int i = 0; i < 5; ++i) {
            const float* pl = planes[e][i];
            const float x = pl[0] >= 0 ? gm.worldMax[0] : gm.worldMin[0];
            const float y = pl[1] >= 0 ? gm.worldMax[1] : gm.worldMin[1];
            const float z = pl[2] >= 0 ? gm.worldMax[2] : gm.worldMin[2];
            if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0) return false;
        }
        return true;
    };
    drawn_ = 0;
    static int cullFrame = 0;
    const int frameNo_ = ++cullFrame;
    for (auto& gm : meshes_) {
        gm.inView = gm.skybox || (gm.visible && (inFrustum(gm, 0) || inFrustum(gm, 1)));   // (animated: bounds kept current)
        if (gm.dupOf >= 0 && gm.inView) {   // an exact copy of a mesh that already draws these pixels (uploadMesh)
            const GpuMesh& o = meshes_[gm.dupOf];
            if (o.visible && !std::memcmp(o.world, gm.world, 64)) gm.inView = false;
        }
        if (gm.inView && gm.visible) { ++drawn_; requestTextures(gm); }
        if (gm.debug && frameNo_ == 120)
            LOG("[cull] %s eye %.2f %.2f %.2f | visible %d inView %d dynamic %d | world box %.2f %.2f %.2f .. %.2f %.2f %.2f",
                gm.debugName.c_str(), eyes[0].position[0], eyes[0].position[1], eyes[0].position[2], gm.visible, gm.inView, gm.dynamic,
                gm.worldMin[0], gm.worldMin[1], gm.worldMin[2], gm.worldMax[0], gm.worldMax[1], gm.worldMax[2]);
    }
    // A batch draws when any of its parts is in view, in their place (the debug views show the parts themselves).
    for (Batch& b : batches_) {
        b.gm.inView = false;
        if (!b.active || debugView_ != kViewNormal) continue;
        for (size_t m : b.members) b.gm.inView |= meshes_[m].inView;
        if (b.sorted && b.gm.inView) {   // see-through: the parts in view, in their order
            sortBatch(b, head);
            b.gm.inView = b.gm.indexCount > 0;
        }
        for (size_t m : b.members) meshes_[m].inView = false;
    }
    auto meshAt = [&](size_t k) -> GpuMesh& { return k < meshes_.size() ? meshes_[k] : batches_[k - meshes_.size()].gm; };

    // Redundant state is not re-sent: consecutive meshes of one shader share its pipeline and its set 0 / set 1
    // (the same layout keeps them bound), so only set 2 and the push constants change between them.
    VkPipeline boundPipe = VK_NULL_HANDLE;
    VkPipelineLayout boundLayout = VK_NULL_HANDLE;
    VkDescriptorSet boundSet0 = VK_NULL_HANDLE, boundSet1 = VK_NULL_HANDLE;
    VkBuffer boundVbo = VK_NULL_HANDLE, boundIbo = VK_NULL_HANDLE;
    pipeBinds = 0; draws = 0;
    alignas(16) u8 pcBuf[256];
    static const bool noBindCache = std::getenv("FH_NO_BIND_CACHE") != nullptr;   // A/B: every bind re-sent (the old way)
    // Cost view on a tiler: two frames in three draw one mesh in view twice (its pixels again, at the same depth: the
    // picture doesn't change), going round all of them; the next render() reads what each added when its frame is
    // timed. (Three, not two: anything else on the GPU at half rate would make every other frame look dearer.)
    doubled_ = -1;
    if (debugView_ == kViewCost && tiler_ && (++costFrame_ % 3) != 0) {
        for (size_t k = 0; k < meshes_.size(); ++k) {
            costNext_ = (costNext_ + 1) % std::max<size_t>(1, meshes_.size());
            const GpuMesh& gm = meshes_[costNext_];
            if (gm.visible && gm.inView && !gm.skybox) { doubled_ = static_cast<int>(costNext_); break; }
        }
    }
    tris = 0;
    const bool replaced = debugView_ == kViewMeshes || debugView_ == kViewOverdraw;   // the home isn't drawn as itself
    auto hiddenByDebug = [&](const GpuMesh& gm) {
        return ((debugHide & kHideSeeThrough) && gm.blend != 0 && !gm.skybox) || ((debugHide & kHideCutout) && gm.alphaToCoverage) ||
               ((debugHide & kHideSky) && gm.skybox) || ((debugHide & kHideAnimated) && gm.dynamic) ||
               ((debugHide & kHideSolid) && gm.blend == 0 && !gm.skybox && !gm.alphaToCoverage);
    };
    auto draw = [&](GpuMesh& gm, CutPass cut = kCutNone) {
        if (!gm.visible || !gm.inView || replaced || (debugHide && hiddenByDebug(gm))) return;
        // A see-through Clay mesh faded all the way out (its tint alpha 0) leaves every pixel as it was: not drawn.
        if (gm.blend != 0 && gm.color[3] <= 0.0f && programs_[gm.program].clay) return;
        if (cut != kCutDepth) tris += gm.indexCount / 3;
        Program& p = programs_[gm.program];
        VkPipeline pipe = pipelineFor(p, gm.blend, gm.cull, gm.skybox, gm.alphaToCoverage, cut, gm.variant);
        if (!pipe) return;
        if (noBindCache) boundPipe = VK_NULL_HANDLE, boundLayout = VK_NULL_HANDLE, boundVbo = boundIbo = VK_NULL_HANDLE;
        if (pipe != boundPipe) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); boundPipe = pipe; ++pipeBinds; }
        if (p.layout != boundLayout || p.set0 != boundSet0 || p.set1 != boundSet1) {
            VkDescriptorSet sets[3] = {p.set0, p.set1, gm.set2};
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 3, sets, 0, nullptr);
            boundLayout = p.layout; boundSet0 = p.set0; boundSet1 = p.set1;
        } else {
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 2, 1, &gm.set2, 0, nullptr);
        }
        ++draws;
        if (p.pushSize) {
            std::vector<u8> heapPc;
            u8* pc = pcBuf;
            if (p.pushSize > sizeof pcBuf) { heapPc.assign(p.pushSize, 0); pc = heapPc.data(); }
            else std::memset(pcBuf, 0, p.pushSize);
            float world[16];
            std::memcpy(world, gm.world, 64);
            if (gm.skybox) {                            // unit sky cube follows the head, scaled far out
                const float S = 5000.0f;
                std::memset(world, 0, 64);
                world[0] = world[5] = world[10] = S;
                world[12] = head[0]; world[13] = head[1]; world[14] = head[2]; world[15] = 1;
            }
            if (p.pcWorld >= 0 && static_cast<uint32_t>(p.pcWorld) + 64 <= p.pushSize) std::memcpy(&pc[p.pcWorld], world, 64);
            if (p.pcColor >= 0 && static_cast<uint32_t>(p.pcColor) + 16 <= p.pushSize) std::memcpy(&pc[p.pcColor], gm.color, 16);
            if (p.pcUvU >= 0 && p.pcUvV >= 0 && static_cast<uint32_t>(std::max(p.pcUvU, p.pcUvV)) + 16 <= p.pushSize) {
                std::memcpy(&pc[p.pcUvU], gm.uvXform, 12);
                std::memcpy(&pc[p.pcUvV], gm.uvXform + 3, 12);
            }
            vkCmdPushConstants(cmd_, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, p.pushSize, pc);
        }
        VkDeviceSize zero = 0;
        if (gm.vbo.buf != boundVbo) { vkCmdBindVertexBuffers(cmd_, 0, 1, &gm.vbo.buf, &zero); boundVbo = gm.vbo.buf; }
        if (gm.ibo.buf != boundIbo) { vkCmdBindIndexBuffer(cmd_, gm.ibo.buf, 0, VK_INDEX_TYPE_UINT32); boundIbo = gm.ibo.buf; }
        // Inside a multiview pass each timestamp takes viewCount consecutive queries.
        // Per-draw timestamps serialize the GPU (each waits for the work before it): opt-in only.
        static const bool profDraws = std::getenv("FH_PROF_DRAWS") != nullptr;
        const bool prof = cut != kCutDepth && (profDraws || (debugView_ == kViewCost && !tiler_)) && queryPool_ && profNext_ + 2 * viewCount <= kMaxQueries - 4 - viewCount;
        if (prof) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool_, profNext_);
        vkCmdDrawIndexed(cmd_, gm.indexCount, 1, 0, 0, 0);
        if (cut != kCutDepth && doubled_ >= 0 && &gm == &meshes_[doubled_]) vkCmdDrawIndexed(cmd_, gm.indexCount, 1, 0, 0, 0);
        if (prof) {
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, profNext_ + viewCount);
            profDraws_.push_back({static_cast<size_t>(&gm - meshes_.data()), profNext_});
            profNext_ += 2 * viewCount;
        }
    };
    if (skyFirst()) for (auto& gm : meshes_) if (gm.skybox) draw(gm);
    // Opaque: solid surfaces in the file's order, then the cut-out ones (leaves, fences: they discard, which can switch a
    // tiler's early depth rejection off for the draws after them). FH_ORDER=0: all in the file's order; 1: solid nearest
    // first (A/B: on the Frame's Adreno, sorting cost more in shader changes than it saved).
    static const int opaqueMode = std::getenv("FH_ORDER") ? std::atoi(std::getenv("FH_ORDER")) : 2;
    if (opaqueMode == 0) {
        for (auto& gm : meshes_) if (gm.blend == 0 && !gm.skybox) draw(gm);
        for (auto& b : batches_) if (b.gm.blend == 0) draw(b.gm);
    } else {
        opaqueOrder_.clear();
        for (size_t i = 0; i < meshes_.size() + batches_.size(); ++i) {   // (past the meshes: the batches, at their first part)
            const GpuMesh& gm = meshAt(i);
            if (gm.blend != 0 || gm.skybox || !gm.visible || !gm.inView) continue;
            float d = static_cast<float>(i < meshes_.size() ? i : batches_[i - meshes_.size()].members.front());
            if (opaqueMode == 1) {   // distance to the box (0 inside it): a big floor around you is nearest, not its far-off centre
                d = 0;
                for (int k = 0; k < 3; ++k) {
                    const float e = std::max({gm.worldMin[k] - head[k], 0.0f, head[k] - gm.worldMax[k]});
                    d += e * e;
                }
            }
            opaqueOrder_.push_back({(gm.alphaToCoverage ? 1e12 : 0.0) + d, i});   // (double: the index survives the offset)
        }
        std::sort(opaqueOrder_.begin(), opaqueOrder_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        // The cut-outs (last in the order) in two passes: their depth from the alpha-only shader, then Meta's shader
        // with depth writes off, so overlapping fronds and cards are shaded once per sample, by the front-most one
        // (alpha-to-coverage keeps the GPU from rejecting hidden samples before shading: each frond behind another
        // was lit in full). The same samples, the same front-most surface: the same picture.
        static const bool twoPass = !std::getenv("FH_NO_CUT_PREPASS");
        size_t firstCut = opaqueOrder_.size();
        if (twoPass)
            for (size_t k = 0; k < opaqueOrder_.size(); ++k)
                if (meshAt(opaqueOrder_[k].second).alphaToCoverage) { firstCut = k; break; }
        for (size_t k = 0; k < firstCut; ++k) draw(meshAt(opaqueOrder_[k].second));
        for (size_t k = firstCut; k < opaqueOrder_.size(); ++k) {
            GpuMesh& gm = meshAt(opaqueOrder_[k].second);
            const Program& cp = programs_[gm.program];
            if (gm.variant >= 0 ? !cp.variants[gm.variant].fsAlpha : !cp.fsAlpha) gm.cutOnce = true;   // no alpha-only shader: drawn the one way
            else draw(gm, kCutDepth);
        }
        for (size_t k = firstCut; k < opaqueOrder_.size(); ++k) {
            GpuMesh& gm = meshAt(opaqueOrder_[k].second);
            draw(gm, gm.cutOnce ? kCutNone : kCutShade);
        }
    }
    if (!skyFirst()) {
        VkViewport skyVp = vp;
        skyVp.minDepth = skyVp.maxDepth = 0.0f;
        bool set = false;
        for (auto& gm : meshes_)
            if (gm.skybox) {
                if (!set) { vkCmdSetViewport(cmd_, 0, 1, &skyVp); set = true; }
                draw(gm);
            }
        if (set) vkCmdSetViewport(cmd_, 0, 1, &vp);
    }
    drawSplats(eyes, head, planes);                     // Gaussian splats: over what's solid and the sky, under what's see-through
    if (splatsDrawn_) boundPipe = VK_NULL_HANDLE, boundLayout = VK_NULL_HANDLE;   // (they bound their own)
    std::vector<std::pair<float, size_t>> order;
    for (size_t i = 0; i < meshes_.size() + batches_.size(); ++i) {   // (past the meshes: the see-through batches, at their box's centre)
        const GpuMesh& gm = meshAt(i);
        if (gm.blend == 0 || gm.skybox || (i >= meshes_.size() && !gm.inView)) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) d += (gm.centroid[k] - head[k]) * (gm.centroid[k] - head[k]);
        order.push_back({d, i});
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& o : order) draw(meshAt(o.second));

    if (debugView_ != kViewNormal && dbg_.draws.mapped) {
        // Each mesh's colour: its own (a hash of its index), its cost, or its kind.
        auto shown = [&](const GpuMesh& gm) { return gm.visible && gm.inView && !gm.skybox && !(debugHide && hiddenByDebug(gm)); };
        auto hashColor = [](uint32_t i, float* c) {
            uint32_t h = i * 2654435761u;
            h ^= h >> 15; h *= 0x2c1b3c6dU; h ^= h >> 12;
            const float hue = (h & 1023) / 1023.0f * 6.0f, sat = 0.45f + ((h >> 10) & 255) / 255.0f * 0.4f;
            const float x = 1 - std::fabs(std::fmod(hue, 2.0f) - 1);
            float r = 0, g = 0, b = 0;
            switch (static_cast<int>(hue) % 6) { case 0: r = 1; g = x; break; case 1: r = x; g = 1; break; case 2: g = 1; b = x; break;
                                                 case 3: g = x; b = 1; break; case 4: r = x; b = 1; break; default: r = 1; b = x; }
            c[0] = 0.9f * (1 - sat + sat * r); c[1] = 0.9f * (1 - sat + sat * g); c[2] = 0.9f * (1 - sat + sat * b);
        };
        if (debugView_ == kViewWireframe) {
            for (auto& gm : meshes_) {
                if (!shown(gm)) continue;
                const float c[4] = {gm.blend ? 0.35f : gm.alphaToCoverage ? 0.45f : 0.92f, gm.blend ? 0.7f : 0.95f, gm.blend ? 1.0f : gm.alphaToCoverage ? 0.45f : 0.92f, 0.55f};
                drawDebug(gm, kDbgWire, c, eyes, true);
            }
        } else if (debugView_ == kViewMeshes) {
            for (size_t pass = 0; pass < 2; ++pass)              // solid ones first, see-through over them
                for (size_t i = 0; i < meshes_.size(); ++i) {
                    GpuMesh& gm = meshes_[i];
                    if (!shown(gm) || (gm.blend != 0) != (pass == 1)) continue;
                    float c[4] = {0, 0, 0, pass ? 0.5f : 1.0f};
                    hashColor(static_cast<uint32_t>(gm.sceneIndex >= 0 ? gm.sceneIndex : static_cast<int>(i)), c);
                    drawDebug(gm, pass ? kDbgTint : kDbgSolid, c, eyes, false);
                    tris += gm.indexCount / 3;
                }
        } else if (debugView_ == kViewCost) {
            for (size_t i = 0; i < meshes_.size(); ++i) {
                GpuMesh& gm = meshes_[i];
                if (!shown(gm)) continue;
                const float ms = i < cost_.size() ? cost_[i] : -1.0f;
                float c[4] = {0.5f, 0.5f, 0.5f, 0.75f};                 // not measured (yet)
                if (ms >= 0) {   // green -> yellow -> red at costScaleMs, by orders of magnitude from 1/50 of it
                    const float lo = std::max(costScaleMs, 1e-4f) / 50;
                    const float t = std::clamp(std::log(std::max(ms, lo) / lo) / std::log(50.0f), 0.0f, 1.0f);
                    c[0] = std::min(1.0f, t * 2); c[1] = std::min(1.0f, 2 - t * 2) * 0.85f + 0.1f; c[2] = 0.12f;
                }
                drawDebug(gm, kDbgTint, c, eyes, true);
            }
        } else if (debugView_ == kViewOverdraw) {
            for (auto& gm : meshes_) {
                if (!shown(gm)) continue;
                const float c[4] = {0.09f, 0.045f, 0.018f, 1};          // a dozen layers is white
                drawDebug(gm, kDbgAdd, c, eyes, false);
                tris += gm.indexCount / 3;
            }
        }
    }

    if (sel_.mesh >= 0 && static_cast<size_t>(sel_.mesh) < meshes_.size() && meshes_[sel_.mesh].visible) {
        float vp[16], mvp[16];
        mat4mul4(eyes[0].proj.m, eyes[0].view.m, vp);
        mat4mul4(vp, meshes_[sel_.mesh].world, mvp);
        for (int c = 0; c < 4; ++c) mvp[c * 4 + 2] *= 1.0015f;   // a hair toward the eye (reversed-Z): on the surface, not in it
        std::memcpy(sel_.ubo.mapped, mvp, 64);
        VkDeviceSize zero = 0;
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, sel_.pipeLayout, 0, 1, &sel_.set, 0, nullptr);
        vkCmdBindVertexBuffers(cmd_, 0, 1, &sel_.vbo.buf, &zero);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, sel_.faces);
        vkCmdBindIndexBuffer(cmd_, sel_.tris.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_, sel_.triCount, 1, 0, 0, 0);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, sel_.lines);
        vkCmdBindIndexBuffer(cmd_, sel_.edges.buf, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_, sel_.edgeCount, 1, 0, 0, 0);
    }
    drawGuide(eyes);

    if (fdmOn_ && fdmOffset_) {   // foveation: each eye's full-detail centre moved to where it looks (the map's middle on that point)
        VkOffset2D offs[2] = {};
        for (uint32_t e = 0; e < viewCount && e < 2; ++e) {
            const float cxp = fovCenter_[e][0] >= 0 ? fovCenter_[e][0] : width_ * 0.5f, cyp = fovCenter_[e][1] >= 0 ? fovCenter_[e][1] : height_ * 0.5f;
            const float mx = fdmW_ * fdmTexel_ * 0.5f, my = fdmH_ * fdmTexel_ * 0.5f;
            const int gx = static_cast<int>(fdmGranularity_), gy = static_cast<int>(fdmGranularityY_);
            offs[e].x = static_cast<int32_t>(std::lround((cxp - mx) / gx)) * gx;
            offs[e].y = static_cast<int32_t>(std::lround((cyp - my) / gy)) * gy;
        }
        VkSubpassFragmentDensityMapOffsetEndInfoQCOM off = {VK_STRUCTURE_TYPE_SUBPASS_FRAGMENT_DENSITY_MAP_OFFSET_END_INFO_QCOM};
        off.fragmentDensityOffsetCount = viewCount;
        off.pFragmentDensityOffsets = offs;
        VkSubpassEndInfo end = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        end.pNext = &off;
        vkCmdEndRenderPass2(cmd_, &end);
    } else {
        vkCmdEndRenderPass(cmd_);
    }
    if (smoothingPass && fovPipe_) {             // the foveated frame, its coarse regions filtered, where it goes
        if (direct) recordFovPass(exportFbs_[exportSlot], exportPass_);
        else recordFovPass(fovFb_, fovPass_);
    }
    if (queryPool_) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, kMaxQueries - 2);
    if (!direct && exportSlot >= 0 && static_cast<size_t>(exportSlot) < exportImages_.size()) {   // the frame for the compositor (system mode), copied
        VkImage dst = exportImages_[exportSlot];
        // GENERAL between frames: the layout both processes agree on (the compositor samples it as GENERAL). What the
        // slot held is discarded (UNDEFINED): the copy overwrites all of it, and the compositor never hands it back.
        imageBarrier(cmd_, dst, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(cmd_, resolve_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, viewCount, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy cp = {};
        cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, viewCount};
        cp.dstSubresource = cp.srcSubresource;
        cp.extent = {width_, height_, 1};
        vkCmdCopyImage(cmd_, resolve_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    }
    // The finished frame goes to the compositor: its image released from our queue family to the outside one (the
    // compositor's layer acquires it before sampling, layer/frame_home_layer.cpp). GENERAL to GENERAL, so this is no
    // layout transition and an older layer that doesn't acquire reads it exactly as before. The next frame into this
    // slot discards it (UNDEFINED) rather than taking it back.
    if (exportSlot >= 0 && static_cast<size_t>(exportSlot) < exportImages_.size())
        ownershipBarrier(cmd_, exportImages_[exportSlot], viewCount, queueFamily_, VK_QUEUE_FAMILY_EXTERNAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (queryPool_) { vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool_, kMaxQueries - 1); profFrame_ = true; }
    timedSlot_ = exportSlot;
    if (perfThis) { vkCmdEndQuery(cmd_, perfPool_, 0); perfRecorded_ = true; }
    vkEndCommandBuffer(cmd_);
    };
    if (early) {                                // record now; wait for the last frame after
        refreshDynamicMeshes(true, false);
        record();
        if (!prep()) return;
        t1 = t0;                                // (the wait is after the recording here)
    } else {
        if (!prep()) return;
        record();
    }
    stats_.early = early;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    const uint64_t signal = serial_ + 1;
    if (timeline_) {
        tsi.signalSemaphoreValueCount = 1;
        tsi.pSignalSemaphoreValues = &signal;
        si.pNext = &tsi;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline_;
    }
    VkPerformanceQuerySubmitInfoKHR psi = {VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR};
    if (perfPool_) { psi.pNext = si.pNext; si.pNext = &psi; }
    const auto t4 = clk::now();
    const VkResult submitted = vkQueueSubmit(queue_, 1, &si, fence_);
    if (submitted == VK_SUCCESS) { if (timeline_) serial_ = signal; }
    else {
        fenceUnsignalled_ = true;
        if (submitted == VK_ERROR_DEVICE_LOST) lost_ = true;
        LOG("frame not submitted: %d%s", static_cast<int>(submitted), lost_ ? " (device lost: the GPU was reset)" : "");
    }
    const auto t5 = clk::now();
    stats_.uploadMs = ms(t2, t3);
    stats_.recordMs = ms(t3, t4);
    stats_.submitMs = ms(t4, t5);
    stats_.totalMs = ms(t0, t5);
    stats_.uploads = lastUploads_;
    stats_.draws = draws;
    stats_.pipelineBinds = pipeBinds;
    stats_.triangles = tris;
    // Slow-frame report (stderr / studio.log): where the time went.
    if (ms(t0, t5) > 40.0)
        LOG("slow frame %.1f ms: wait-GPU %.1f, prep %.1f, texture uploads %.1f, record %.1f, submit %.1f (%zu drawn)",
            ms(t0, t5), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5), drawn_);
}

void HslRenderer::waitFrame() {
    if (lost_ || fenceUnsignalled_) return;
    if (vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX) == VK_ERROR_DEVICE_LOST) lost_ = true;
}

bool HslRenderer::waitFrameSerial(uint64_t serial, uint64_t timeoutNs) {
    if (!timeline_) return true;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &serial;
    const VkResult r = vkWaitSemaphores(device_, &wi, timeoutNs);
    if (r == VK_ERROR_DEVICE_LOST) lost_ = true;
    return r == VK_SUCCESS;
}

bool HslRenderer::createExportImages(uint32_t n, std::vector<ExportImage>& out, uint32_t& memoryTypeOut, uint8_t deviceUUID[16]) {
    destroyExportImages(out);
    auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR"));
    if (!getFd) { LOG("system mode: no VK_KHR_external_memory_fd"); return false; }
    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    pp.pNext = &idp;
    vkGetPhysicalDeviceProperties2(physical_, &pp);
    std::memcpy(deviceUUID, idp.deviceUUID, 16);
    for (uint32_t i = 0; i < n; ++i) {
        ExportImage e;
        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &ext;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_SRGB;
        ci.extent = {width_, height_, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = viewCount;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = exportUsage();
        ci.flags = exportCreateFlags();
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &ci, nullptr, &e.image) != VK_SUCCESS) { destroyExportImages(out); return false; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, e.image, &req);
        const uint32_t type = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = e.image;
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exp.pNext = &ded;
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &exp;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device_, &ai, nullptr, &e.mem) != VK_SUCCESS || vkBindImageMemory(device_, e.image, e.mem, 0) != VK_SUCCESS) {
            out.push_back(e);
            destroyExportImages(out);
            return false;
        }
        e.size = req.size;
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gi.memory = e.mem;
        gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (getFd(device_, &gi, &e.fd) != VK_SUCCESS) e.fd = -1;
        memoryTypeOut = type;
        out.push_back(e);
        if (e.fd < 0) { destroyExportImages(out); return false; }
    }
    exportImages_.clear();
    for (const ExportImage& e : out) exportImages_.push_back(e.image);
    if (exportDirect) createExportFramebuffers();
    return true;
}

#ifdef _WIN32
// Windows system mode: the compositor made kSlots shared Direct3D 11 textures (R8G8B8A8_UNORM_SRGB, `n` layers) at
// this render size and duplicated their NT handles into us. Each becomes a VkImage we render the home into; render()
// copies the eye frame into exportImages_[exportSlot] through the ordinary (non-direct) copy path. No cross-API lock:
// a slot is published only once its frame is retired and the four rotate, as on Linux.
bool HslRenderer::importWin32Images(uint32_t n, const uint64_t handles[], uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(device_);                          // an earlier generation may still be in flight
    for (VkImage img : exportImages_) vkDestroyImage(device_, img, nullptr);
    for (VkDeviceMemory m : importMem_) vkFreeMemory(device_, m, nullptr);
    exportImages_.clear();
    importMem_.clear();
    exportSlot = -1;
    exportDirect = false;                               // copied in (the imported textures aren't our render targets)
    for (uint32_t i = 0; i < n; ++i) {
        VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.pNext = &ext;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_SRGB;            // == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = viewCount;                     // 2 layers, one per eye
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(device_, &ci, nullptr, &image) != VK_SUCCESS) { LOG("system mode: import image %u failed", i); return false; }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, image, &req);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = image;
        VkImportMemoryWin32HandleInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        imp.pNext = &ded;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle = reinterpret_cast<void*>(static_cast<uintptr_t>(handles[i]));
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS || vkBindImageMemory(device_, image, mem, 0) != VK_SUCCESS) {
            LOG("system mode: import memory %u failed (adapter mismatch, or the compositor's textures went away)", i);
            if (mem) vkFreeMemory(device_, mem, nullptr);
            vkDestroyImage(device_, image, nullptr);
            return false;
        }
        exportImages_.push_back(image);
        importMem_.push_back(mem);
    }
    LOG("system mode: imported %u compositor textures (%ux%u, %u layers)", n, w, h, viewCount);
    return true;
}
#endif

uint32_t HslRenderer::exportUsage() const {
    return VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           (exportDirect ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u);
}

uint32_t HslRenderer::exportCreateFlags() const {
    return exportsSubsampled() ? VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT : 0u;
}

// exportDirect: a framebuffer per export image, its last colour attachment that image (the resolve, or the colour itself
// without MSAA). None of them (and exportSlot frames are copied instead) if the pass or a view can't be made.
void HslRenderer::createExportFramebuffers() {
    destroyExportFramebuffers();
    if (!exportPass_) return;
    const bool ms = samples_ != VK_SAMPLE_COUNT_1_BIT;
    for (VkImage img : exportImages_) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = VK_FORMAT_R8G8B8A8_SRGB;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, viewCount};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) { destroyExportFramebuffers(); return; }
        exportViews_.push_back(v);
        std::vector<VkImageView> views = ms ? std::vector<VkImageView>{colorMs_.view, depth_.view, v} : std::vector<VkImageView>{v, depth_.view};
        if (fdmOn_) views.push_back(fdm_.view);
        if (smoothFov_ && !exportsSubsampled()) views = {v};   // our smoothing pass writes it (exportPass_ is colorOnlyPass)
        VkFramebufferCreateInfo fb = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = exportPass_;
        fb.attachmentCount = static_cast<uint32_t>(views.size());
        fb.pAttachments = views.data();
        fb.width = width_;
        fb.height = height_;
        fb.layers = 1;
        VkFramebuffer f = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &f) != VK_SUCCESS) { destroyExportFramebuffers(); return; }
        exportFbs_.push_back(f);
    }
    LOG("frames drawn straight into the compositor's %zu images (no copy)", exportFbs_.size());
}

void HslRenderer::destroyExportFramebuffers() {
    for (VkFramebuffer f : exportFbs_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : exportViews_) vkDestroyImageView(device_, v, nullptr);
    exportFbs_.clear();
    exportViews_.clear();
    lastExportSlot_ = -1;
}

void HslRenderer::destroyExportImages(std::vector<ExportImage>& images) {
    if (images.empty()) return;
    vkDeviceWaitIdle(device_);
    for (ExportImage& e : images) {
#ifndef _WIN32
        if (e.fd >= 0) ::close(e.fd);   // export is a Frame Home (Linux) thing
#endif
        if (e.image) vkDestroyImage(device_, e.image, nullptr);
        if (e.mem) vkFreeMemory(device_, e.mem, nullptr);
    }
    destroyExportFramebuffers();
    images.clear();
    exportImages_.clear();
    exportSlot = -1;
}

bool HslRenderer::readback(uint32_t layer, std::vector<u8>& rgba) {
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    const VkDeviceSize size = static_cast<VkDeviceSize>(width_) * height_ * 4;
    Buffer b = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!b.buf) return false;
    oneShot([&](VkCommandBuffer c) {
        VkBufferImageCopy copy = {};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        copy.imageExtent = {width_, height_, 1};
        if (lastExportSlot_ >= 0 && static_cast<size_t>(lastExportSlot_) < exportImages_.size()) {   // exportDirect: the frame is there
            // (released to the compositor: taken back for the copy, then handed over again)
            VkImage img = exportImages_[lastExportSlot_];
            ownershipBarrier(c, img, viewCount, VK_QUEUE_FAMILY_EXTERNAL, queueFamily_, 0, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyImageToBuffer(c, img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &copy);
            ownershipBarrier(c, img, viewCount, queueFamily_, VK_QUEUE_FAMILY_EXTERNAL, 0, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else vkCmdCopyImageToBuffer(c, resolve_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &copy);
    });
    rgba.assign(static_cast<u8*>(b.mapped), static_cast<u8*>(b.mapped) + size);
    destroy(b);
    return true;
}

void HslRenderer::shutdown() {
    if (!device_) return;
    stopWorkers();
    unload();
    destroyGuide();
    destroySplatPipeline();
    destroyDebug();
    for (VkImageView v : layerViews_) if (v) vkDestroyImageView(device_, v, nullptr);
    for (Image& i : pictures_) destroy(i);
    pictures_.clear();
    for (Image* i : {&colorMs_, &depth_, &resolve_, &fdm_, &sub_, &white_, &black_, &nullTex_, &noMetal_, &flatNormal_, &whiteCube_, &white3D_, &shadowMap_}) destroy(*i);
    if (fovFb_) vkDestroyFramebuffer(device_, fovFb_, nullptr);
    if (fovPipe_) vkDestroyPipeline(device_, fovPipe_, nullptr);
    if (fovLayout_) vkDestroyPipelineLayout(device_, fovLayout_, nullptr);
    if (fovPool_) vkDestroyDescriptorPool(device_, fovPool_, nullptr);
    if (fovSetLayout_) vkDestroyDescriptorSetLayout(device_, fovSetLayout_, nullptr);
    if (subSampler_) vkDestroySampler(device_, subSampler_, nullptr);
    if (fovPass_) vkDestroyRenderPass(device_, fovPass_, nullptr);
    for (Buffer* b : {&globals_, &lightUniforms_, &lightItems_, &lightGrid_, &ring_}) destroy(*b);
    for (MemBlock& b : blocks_) {
        if (b.mapped) vkUnmapMemory(device_, b.mem);
        vkFreeMemory(device_, b.mem, nullptr);
    }
    blocks_.clear();
    vkDestroySampler(device_, wrapSampler_, nullptr);
    vkDestroySampler(device_, anisoSampler_, nullptr);
    vkDestroySampler(device_, shadowSampler_, nullptr);
    vkDestroyFramebuffer(device_, framebuffer_, nullptr);
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (exportPass_) vkDestroyRenderPass(device_, exportPass_, nullptr);
#ifdef _WIN32
    for (VkImage img : exportImages_) vkDestroyImage(device_, img, nullptr);   // imported compositor textures
    for (VkDeviceMemory m : importMem_) vkFreeMemory(device_, m, nullptr);
    exportImages_.clear();
    importMem_.clear();
#endif
    if (pipelineCache_) {
        size_t n = 0;
        if (!pipelineCachePath.empty() && vkGetPipelineCacheData(device_, pipelineCache_, &n, nullptr) == VK_SUCCESS && n) {
            std::vector<char> data(n);
            if (vkGetPipelineCacheData(device_, pipelineCache_, &n, data.data()) == VK_SUCCESS)
                if (FILE* f = texcache::open(std::filesystem::u8path(pipelineCachePath), true)) { std::fwrite(data.data(), 1, n, f); std::fclose(f); }
        }
        vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    }
    if (queryPool_) vkDestroyQueryPool(device_, queryPool_, nullptr);
    if (perfPool_) {
        vkDestroyQueryPool(device_, perfPool_, nullptr);
        if (auto unlock = reinterpret_cast<PFN_vkReleaseProfilingLockKHR>(vkGetDeviceProcAddr(device_, "vkReleaseProfilingLockKHR"))) unlock(device_);
    }
    destroySelection();
    vkDestroyDescriptorPool(device_, descPool_, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    if (timeline_) vkDestroySemaphore(device_, timeline_, nullptr);
    timeline_ = VK_NULL_HANDLE;
    vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
    vkDestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
}

void setTextureCap(uint32_t side) { g_textureCap = side; }
