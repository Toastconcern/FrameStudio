#pragma once
// OverlayTexture: hands a CPU-drawn RGBA canvas to a SteamVR overlay through Vulkan images this process owns
// (SetOverlayTexture), instead of SetOverlayRaw. SetOverlayRaw makes SteamVR create a new texture on every call,
// and the dashboard can show that texture missing or half-made for a frame: the panel flickered whenever it
// redrew (pointer moves, stats refreshes). Here the canvas goes into one of three images in turn (staging buffer,
// copy, wait for the copy's fence), so the image SteamVR is showing is never the one being written.
//
// Vulkan is loaded at run time (libvulkan.so.1, vulkan-1.dll): no link dependency, and if anything is missing init()
// fails and the caller keeps using SetOverlayRaw.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <openvr.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

class OverlayTexture {
public:
    static constexpr int kSlots = 3;

    // w x h RGBA8 (sRGB-encoded bytes, like SetOverlayRaw takes). Needs VR_Init done (the HMD's GPU comes from
    // VRSystem()->GetOutputDevice, the extensions from VRCompositor()).
    bool init(uint32_t w, uint32_t h, std::string& err) {
        w_ = w; h_ = h;
#ifdef _WIN32
        if (!(lib_ = LoadLibraryW(L"vulkan-1.dll"))) return fail(err, "no vulkan-1.dll");
        gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib_), "vkGetInstanceProcAddr")));
#else
        if (!(lib_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)) && !(lib_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))) return fail(err, "no libvulkan");
        gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib_, "vkGetInstanceProcAddr"));
#endif
        if (!gipa_) return fail(err, "no vkGetInstanceProcAddr");
#define OT_GLOBAL(n) n = reinterpret_cast<PFN_##n>(gipa_(VK_NULL_HANDLE, #n)); if (!n) return fail(err, "missing " #n)
        OT_GLOBAL(vkCreateInstance);
        OT_GLOBAL(vkEnumerateInstanceExtensionProperties);
#undef OT_GLOBAL
        if (!vr::VRCompositor() || !vr::VRSystem()) return fail(err, "no compositor");
        // Instance, with the extensions SteamVR wants (those this loader has).
        std::vector<std::string> want = split(instanceExtensions());
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> have(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
        std::vector<const char*> ie;
        for (const std::string& e : want)
            if (std::any_of(have.begin(), have.end(), [&](const VkExtensionProperties& p) { return e == p.extensionName; })) ie.push_back(e.c_str());
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "frame-switcher";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = static_cast<uint32_t>(ie.size());
        ici.ppEnabledExtensionNames = ie.data();
        if (VkResult r = vkCreateInstance(&ici, nullptr, &inst_); r != VK_SUCCESS) return fail(err, "vkCreateInstance " + std::to_string(r));
        if (!loadInstance()) return fail(err, "missing Vulkan entry points");
        // The GPU the headset is on (the only one on the Frame, but ask).
        uint64_t out = 0;
        vr::VRSystem()->GetOutputDevice(&out, vr::TextureType_Vulkan, reinterpret_cast<VkInstance_T*>(inst_));
        pd_ = reinterpret_cast<VkPhysicalDevice>(out);
        if (!pd_) {
            uint32_t c = 1;
            VkPhysicalDevice first = VK_NULL_HANDLE;
            vkEnumeratePhysicalDevices(inst_, &c, &first);
            pd_ = first;
        }
        if (!pd_) return fail(err, "no GPU");
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd_, &qn, qf.data());
        qf_ = UINT32_MAX;
        for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf_ = i; break; }
        if (qf_ == UINT32_MAX) return fail(err, "no graphics queue");
        // Device, with SteamVR's device extensions (external memory: SteamVR copies the image out on our queue).
        uint32_t dn = vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(pd_), nullptr, 0);
        std::string dext(dn, '\0');
        if (dn) vr::VRCompositor()->GetVulkanDeviceExtensionsRequired(reinterpret_cast<VkPhysicalDevice_T*>(pd_), &dext[0], dn);
        devExt_ = split(dext.c_str());
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> dhave(en);
        vkEnumerateDeviceExtensionProperties(pd_, nullptr, &en, dhave.data());
        std::vector<const char*> de;
        for (const std::string& e : devExt_)
            if (std::any_of(dhave.begin(), dhave.end(), [&](const VkExtensionProperties& p) { return e == p.extensionName; })) de.push_back(e.c_str());
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qf_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = static_cast<uint32_t>(de.size());
        dci.ppEnabledExtensionNames = de.data();
        if (VkResult r = vkCreateDevice(pd_, &dci, nullptr, &dev_); r != VK_SUCCESS) return fail(err, "vkCreateDevice " + std::to_string(r));
        vkGetDeviceQueue(dev_, qf_, 0, &queue_);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd_, &mp);
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = qf_;
        if (vkCreateCommandPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS) return fail(err, "vkCreateCommandPool");
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(w_) * h_ * 4;
        for (Slot& s : slots_) {
            VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = format();
            ii.extent = {w_, h_, 1};
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(dev_, &ii, nullptr, &s.image) != VK_SUCCESS) return fail(err, "vkCreateImage");
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev_, s.image, &mr);
            if (!allocate(mp, mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, s.imageMem) && !allocate(mp, mr, 0, s.imageMem)) return fail(err, "image memory");
            vkBindImageMemory(dev_, s.image, s.imageMem, 0);
            VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            if (vkCreateBuffer(dev_, &bi, nullptr, &s.staging) != VK_SUCCESS) return fail(err, "vkCreateBuffer");
            vkGetBufferMemoryRequirements(dev_, s.staging, &mr);
            if (!allocate(mp, mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.stagingMem)) return fail(err, "staging memory");
            vkBindBufferMemory(dev_, s.staging, s.stagingMem, 0);
            if (vkMapMemory(dev_, s.stagingMem, 0, bytes, 0, &s.mapped) != VK_SUCCESS) return fail(err, "vkMapMemory");
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(dev_, &ai, &s.cmd) != VK_SUCCESS) return fail(err, "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(dev_, &fi, nullptr, &s.fence) != VK_SUCCESS) return fail(err, "vkCreateFence");
        }
        ok_ = true;
        return true;
    }

    // Copies `rgba` (w x h x 4) into the next image and shows it on `ov`. False: the caller should fall back.
    bool upload(vr::VROverlayHandle_t ov, const uint8_t* rgba, std::string& err) {
        if (!ok_) return false;
        Slot& s = slots_[next_];
        next_ = (next_ + 1) % kSlots;
        // This slot was last shown two uploads ago: its copy finished long since, but don't assume.
        if (vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) { err = "fence timeout"; return false; }
        vkResetFences(dev_, 1, &s.fence);
        std::memcpy(s.mapped, rgba, static_cast<size_t>(w_) * h_ * 4);
        vkResetCommandBuffer(s.cmd, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s.cmd, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = s.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // After SteamVR's copy out of it (on this queue, in TRANSFER_SRC_OPTIMAL) → ready for ours in.
        b.oldLayout = s.used ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = s.used ? VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT : 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy c = {};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w_, h_, 1};
        vkCmdCopyBufferToImage(s.cmd, s.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        // SteamVR takes Vulkan overlay images in TRANSFER_SRC_OPTIMAL (it copies them out).
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(s.cmd);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        if (VkResult r = vkQueueSubmit(queue_, 1, &si, s.fence); r != VK_SUCCESS) { err = "vkQueueSubmit " + std::to_string(r); return false; }
        s.used = true;
        // The whole image is in before SteamVR looks at it.
        if (vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) { err = "copy fence timeout"; return false; }
        vr::VRVulkanTextureData_t vt = {};
        vt.m_nImage = reinterpret_cast<uint64_t>(s.image);
        vt.m_pDevice = reinterpret_cast<VkDevice_T*>(dev_);
        vt.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(pd_);
        vt.m_pInstance = reinterpret_cast<VkInstance_T*>(inst_);
        vt.m_pQueue = reinterpret_cast<VkQueue_T*>(queue_);
        vt.m_nQueueFamilyIndex = qf_;
        vt.m_nWidth = w_;
        vt.m_nHeight = h_;
        vt.m_nFormat = format();
        vt.m_nSampleCount = 1;
        vr::Texture_t tex = {&vt, vr::TextureType_Vulkan, vr::ColorSpace_Auto};
        const vr::EVROverlayError oe = vr::VROverlay()->SetOverlayTexture(ov, &tex);
        if (oe != vr::VROverlayError_None) { err = std::string("SetOverlayTexture: ") + vr::VROverlay()->GetOverlayErrorNameFromEnum(oe); return false; }
        return true;
    }

    // After VR_Shutdown (vrclient still holds the images until then).
    void destroy() {
        if (dev_) {
            vkDeviceWaitIdle(dev_);
            for (Slot& s : slots_) {
                if (s.fence) vkDestroyFence(dev_, s.fence, nullptr);
                if (s.staging) vkDestroyBuffer(dev_, s.staging, nullptr);
                if (s.stagingMem) vkFreeMemory(dev_, s.stagingMem, nullptr);
                if (s.image) vkDestroyImage(dev_, s.image, nullptr);
                if (s.imageMem) vkFreeMemory(dev_, s.imageMem, nullptr);
                s = Slot();
            }
            if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
            vkDestroyDevice(dev_, nullptr);
        }
        if (inst_ && vkDestroyInstance) vkDestroyInstance(inst_, nullptr);
        dev_ = VK_NULL_HANDLE; inst_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
        ok_ = false;
        // libvulkan stays loaded: drivers don't like being unloaded under a live process.
    }
    bool ok() const { return ok_; }

    // The canvas holds sRGB-encoded bytes. UNORM with ColorSpace_Auto (= gamma for 8-bit formats) shows them as
    // SetOverlayRaw did; FH_SWITCHER_SRGB=1 tries the _SRGB format instead.
    static VkFormat format() {
        static const VkFormat f = [] { const char* e = std::getenv("FH_SWITCHER_SRGB"); return e && *e == '1' ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM; }();
        return f;
    }

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory imageMem = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool used = false;
    };

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        for (std::string w; in >> w;) out.push_back(w);
        return out;
    }
    static std::string instanceExtensions() {
        const uint32_t n = vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(nullptr, 0);
        std::string ext(n, '\0');
        if (n) vr::VRCompositor()->GetVulkanInstanceExtensionsRequired(&ext[0], n);
        return ext.c_str();
    }
    bool fail(std::string& err, const std::string& what) {
        err = what;
        destroy();
        return false;
    }
    bool allocate(const VkPhysicalDeviceMemoryProperties& mp, const VkMemoryRequirements& mr, VkMemoryPropertyFlags want, VkDeviceMemory& out) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = i;
                return vkAllocateMemory(dev_, &ai, nullptr, &out) == VK_SUCCESS;
            }
        return false;
    }
    bool loadInstance() {
#define OT_INST(n) n = reinterpret_cast<PFN_##n>(gipa_(inst_, #n)); if (!n) return false
        OT_INST(vkDestroyInstance);
        OT_INST(vkEnumeratePhysicalDevices);
        OT_INST(vkGetPhysicalDeviceQueueFamilyProperties);
        OT_INST(vkGetPhysicalDeviceMemoryProperties);
        OT_INST(vkEnumerateDeviceExtensionProperties);
        OT_INST(vkCreateDevice);
        OT_INST(vkDestroyDevice);
        OT_INST(vkGetDeviceQueue);
        OT_INST(vkDeviceWaitIdle);
        OT_INST(vkCreateImage);
        OT_INST(vkDestroyImage);
        OT_INST(vkGetImageMemoryRequirements);
        OT_INST(vkBindImageMemory);
        OT_INST(vkCreateBuffer);
        OT_INST(vkDestroyBuffer);
        OT_INST(vkGetBufferMemoryRequirements);
        OT_INST(vkBindBufferMemory);
        OT_INST(vkAllocateMemory);
        OT_INST(vkFreeMemory);
        OT_INST(vkMapMemory);
        OT_INST(vkCreateCommandPool);
        OT_INST(vkDestroyCommandPool);
        OT_INST(vkAllocateCommandBuffers);
        OT_INST(vkResetCommandBuffer);
        OT_INST(vkBeginCommandBuffer);
        OT_INST(vkEndCommandBuffer);
        OT_INST(vkCmdPipelineBarrier);
        OT_INST(vkCmdCopyBufferToImage);
        OT_INST(vkQueueSubmit);
        OT_INST(vkCreateFence);
        OT_INST(vkDestroyFence);
        OT_INST(vkWaitForFences);
        OT_INST(vkResetFences);
#undef OT_INST
        return true;
    }

    uint32_t w_ = 0, h_ = 0;
    void* lib_ = nullptr;
    PFN_vkGetInstanceProcAddr gipa_ = nullptr;
    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t qf_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::vector<std::string> devExt_;
    Slot slots_[kSlots];
    int next_ = 0;
    bool ok_ = false;

    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkCreateDevice vkCreateDevice = nullptr;
    PFN_vkDestroyDevice vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
    PFN_vkCreateImage vkCreateImage = nullptr;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory vkBindImageMemory = nullptr;
    PFN_vkCreateBuffer vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
    PFN_vkAllocateMemory vkAllocateMemory = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
    PFN_vkMapMemory vkMapMemory = nullptr;
    PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
    PFN_vkQueueSubmit vkQueueSubmit = nullptr;
    PFN_vkCreateFence vkCreateFence = nullptr;
    PFN_vkDestroyFence vkDestroyFence = nullptr;
    PFN_vkWaitForFences vkWaitForFences = nullptr;
    PFN_vkResetFences vkResetFences = nullptr;
};
