// The compositor layer's import of Frame Home's subsampled frames, without SteamVR: one device exports a subsampled
// 2-layer image the way HslRenderer::createExportImages makes it, a second device (VK_EXT_fragment_density_map on,
// its fragmentDensityMap feature off, as the layer makes vrcompositor's) imports it the way frame_home_layer.cpp does
// and writes it into the descriptor sets the layer draws with: each slot's plain and subsampled set from one pool.
//   subsample_import_test [combined-image-sampler pool count = 3 * slots]      exit 0: every step worked
// Build (on the Frame): g++ -O2 -std=c++17 subsample_import_test.cpp -o subsample_import_test -lvulkan
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kSlots = 4, kSize = 2416, kLayers = 2;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_SRGB;
constexpr VkImageUsageFlags kUsage = 0x17;   // as Frame Home's exports in direct mode

bool check(VkResult r, const char* what) {
    std::printf("%-58s %s (%d)\n", what, r == VK_SUCCESS ? "ok" : "FAILED", static_cast<int>(r));
    return r == VK_SUCCESS;
}

VkDevice makeDevice(VkPhysicalDevice pd, uint32_t family) {
    const float priority = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    const char* exts[] = {VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                          VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = 3;
    ci.ppEnabledExtensionNames = exts;
    VkDevice d = VK_NULL_HANDLE;
    return check(vkCreateDevice(pd, &ci, nullptr, &d), "create device (FDM extension, feature off)") ? d : VK_NULL_HANDLE;
}

VkImageCreateInfo exportImageInfo(const void* next) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = next;
    ci.flags = VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFormat;
    ci.extent = {kSize, kSize, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = kLayers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = kUsage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return ci;
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t samplerCount = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 3 * kSlots;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (!check(vkCreateInstance(&ici, nullptr, &inst), "create instance")) return 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    const VkDevice exporter = makeDevice(pd, 0), importer = makeDevice(pd, 0);
    if (!exporter || !importer) return 1;
    auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(exporter, "vkGetMemoryFdKHR"));

    // Frame Home's side: the exported images.
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo eci = exportImageInfo(&ext);
    int fds[kSlots];
    VkDeviceSize sizes[kSlots];
    uint32_t memoryType = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
        VkImage img;
        if (!check(vkCreateImage(exporter, &eci, nullptr, &img), "export: create subsampled image")) return 1;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(exporter, img, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (memoryType = 0; memoryType < mp.memoryTypeCount; ++memoryType)
            if ((req.memoryTypeBits & (1u << memoryType)) && (mp.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = img;
        VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exp.pNext = &ded;
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &exp;
        ai.allocationSize = sizes[i] = req.size;
        ai.memoryTypeIndex = memoryType;
        VkDeviceMemory mem;
        if (!check(vkAllocateMemory(exporter, &ai, nullptr, &mem), "export: allocate") ||
            !check(vkBindImageMemory(exporter, img, mem, 0), "export: bind")) return 1;
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        gi.memory = mem;
        gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (!check(getFd(exporter, &gi, &fds[i]), "export: fd")) return 1;
    }

    // The layer's side (ensureBase): both kinds of set layout, one pool, a set of each kind per slot.
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.flags = VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler subsampledSampler;
    if (!check(vkCreateSampler(importer, &si, nullptr, &subsampledSampler), "layer: subsampled sampler")) return 1;
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    VkDescriptorSetLayout plainLayout, subsampledLayout;
    if (!check(vkCreateDescriptorSetLayout(importer, &li, nullptr, &plainLayout), "layer: plain set layout")) return 1;
    b[0].pImmutableSamplers = &subsampledSampler;
    if (!check(vkCreateDescriptorSetLayout(importer, &li, nullptr, &subsampledLayout), "layer: subsampled set layout")) return 1;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSlots}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2 * kSlots;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    VkDescriptorPool pool;
    std::printf("pool: %u combined image samplers\n", samplerCount);
    if (!check(vkCreateDescriptorPool(importer, &pi, nullptr, &pool), "layer: pool")) return 1;
    VkDescriptorSet plainSets[kSlots], subsampledSets[kSlots];
    for (uint32_t i = 0; i < kSlots; ++i) {
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &plainLayout;
        if (!check(vkAllocateDescriptorSets(importer, &dai, &plainSets[i]), "layer: plain set")) return 2;
        dai.pSetLayouts = &subsampledLayout;
        if (!check(vkAllocateDescriptorSets(importer, &dai, &subsampledSets[i]), "layer: subsampled set")) return 2;
    }

    // The layer's import: the same image, the memory from the fd, a view, the subsampled set written.
    VkImageCreateInfo ici2 = exportImageInfo(&ext);
    for (uint32_t i = 0; i < kSlots; ++i) {
        VkImage img;
        if (!check(vkCreateImage(importer, &ici2, nullptr, &img), "import: create image with the Hello's flags")) return 1;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(importer, img, &req);
        if (req.size > sizes[i]) { std::printf("import: needs %llu bytes, exported %llu: FAILED\n", (unsigned long long)req.size, (unsigned long long)sizes[i]); return 1; }
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imp.fd = fds[i];
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.image = img;
        imp.pNext = &ded;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &imp;
        ai.allocationSize = sizes[i];
        ai.memoryTypeIndex = memoryType;
        VkDeviceMemory mem;
        if (!check(vkAllocateMemory(importer, &ai, nullptr, &mem), "import: allocate from fd") ||
            !check(vkBindImageMemory(importer, img, mem, 0), "import: bind")) return 1;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = kFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kLayers};
        VkImageView view;
        if (!check(vkCreateImageView(importer, &vi, nullptr, &view), "import: view")) return 1;
        VkDescriptorImageInfo ii = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = subsampledSets[i];
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(importer, 1, &w, 0, nullptr);
        std::printf("%-58s ok\n", "import: subsampled set written");
    }
    std::printf("ALL OK\n");
    return 0;
}
