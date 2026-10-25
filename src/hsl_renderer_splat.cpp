// Gaussian splats in the home renderer (hsl_renderer.h): each splat object's splats in one storage buffer, drawn as one
// instanced quad per splat (tools/splat: the shaders) in the frame's own pass, both eyes at once, after the solid meshes
// and the sky. Back to front by an order sorted on the CPU (splat_sort.h), copied into the order buffer of the command
// buffer being recorded whenever a newer one is ready.
#include "hsl_renderer.h"
#include "splat_spirv.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define LOG(...)  do { std::fprintf(stderr, "[render] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)

namespace {

// As the shader reads it (std430): centre, the covariance's six entries as halves, colour, padding.
struct GpuSplat {
    float pos[3];
    uint32_t cov01, cov23, cov45;
    uint32_t rgba;
    uint32_t pad;
};
static_assert(sizeof(GpuSplat) == 32, "the shader's Splat is 32 bytes");

// Round to nearest, with the small values a covariance has (a 1 mm splat is 1e-6 m^2) kept as subnormals, not zero.
uint16_t half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    x &= 0x7fffffffu;
    if (x >= 0x7f800000u) return static_cast<uint16_t>(sign | 0x7c00u | (x > 0x7f800000u ? 0x200u : 0));   // inf / nan
    if (x >= 0x477ff000u) return static_cast<uint16_t>(sign | 0x7bffu);                                        // past the largest: the largest
    if (x < 0x38800000u) {                                                                                      // subnormal (or zero)
        if (x < 0x33000000u) return static_cast<uint16_t>(sign);
        const uint32_t e = x >> 23, m = (x & 0x7fffffu) | 0x800000u, shift = 126 - e;   // 14 + (127 - 15) - e
        const uint32_t v = m >> shift, rest = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        return static_cast<uint16_t>(sign | (v + (rest > halfway || (rest == halfway && (v & 1)))));
    }
    const uint32_t v = ((x >> 13) - (112u << 10)), rest = x & 0x1fffu;
    return static_cast<uint16_t>(sign | (v + (rest > 0x1000u || (rest == 0x1000u && (v & 1)))));
}

}  // namespace

void HslRenderer::createSplatPipeline() {
    VkDescriptorSetLayoutBinding b[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3; lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(device_, &lci, nullptr, &splatSetLayout_) != VK_SUCCESS) { splatSetLayout_ = VK_NULL_HANDLE; return; }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &splatSetLayout_;
    if (vkCreatePipelineLayout(device_, &pli, nullptr, &splatLayout_) != VK_SUCCESS) { splatLayout_ = VK_NULL_HANDLE; destroySplatPipeline(); return; }
    auto module = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size; ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        return m;
    };
    VkShaderModule vs = module(kSplatVertSpirv, kSplatVertSize), fs = module(kSplatFragSpirv, kSplatFragSize);
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};   // (all from the buffers)
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = samples_;
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;   // hidden by what's solid in front, hiding nothing themselves
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;              // reversed-Z
        VkPipelineColorBlendAttachmentState cb = {};
        cb.colorWriteMask = 0xF;
        cb.blendEnable = VK_TRUE;                                        // premultiplied: the shader multiplied its alpha in
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cb.alphaBlendOp = VK_BLEND_OP_ADD;   // the frame's alpha stays the home's
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1; cbs.pAttachments = &cb;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2; pci.pStages = st; pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
        pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds; pci.pColorBlendState = &cbs;
        pci.pDynamicState = &dsi; pci.layout = splatLayout_; pci.renderPass = renderPass_;
        if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pci, nullptr, &splatPipe_) != VK_SUCCESS) splatPipe_ = VK_NULL_HANDLE;
    }
    if (vs) vkDestroyShaderModule(device_, vs, nullptr);
    if (fs) vkDestroyShaderModule(device_, fs, nullptr);
    if (!splatPipe_) { LOG("splat shaders failed: splats won't draw"); destroySplatPipeline(); }
}

void HslRenderer::destroySplatPipeline() {
    if (splatPipe_) vkDestroyPipeline(device_, splatPipe_, nullptr);
    if (splatLayout_) vkDestroyPipelineLayout(device_, splatLayout_, nullptr);
    if (splatSetLayout_) vkDestroyDescriptorSetLayout(device_, splatSetLayout_, nullptr);
    splatPipe_ = VK_NULL_HANDLE;
    splatLayout_ = VK_NULL_HANDLE;
    splatSetLayout_ = VK_NULL_HANDLE;
}

// Once the meshes are in (loadStep): every splat object of the scene, its covariances worked out here once (in its own
// frame: the shader turns them by its placement), and its sorter started.
void HslRenderer::uploadSplats() {
    splatsUploaded_ = true;
    if (!scene_ || scene_->splats.empty() || !splatPipe_) return;
    const auto t0 = std::chrono::steady_clock::now();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    const size_t most = props.limits.maxStorageBufferRange / sizeof(GpuSplat);   // what one storage buffer may show the shader
    const uint32_t n = static_cast<uint32_t>(scene_->splats.size());
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * n}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * n}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2 * n; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pci, nullptr, &splatPool_) != VK_SUCCESS) { splatPool_ = VK_NULL_HANDLE; LOG("splats: no descriptor pool"); return; }
    size_t total = 0;
    for (const HslSplatObject& so : scene_->splats) {
        splatObjs_.emplace_back();
        SplatGpu& g = splatObjs_.back();
        if (!so.splats || so.splats->empty()) continue;
        const std::vector<Splat>& src = *so.splats;
        const size_t count = std::min(src.size(), most);
        if (count < src.size()) LOG("splats %s: %zu of %zu (the most one buffer holds on this GPU)", so.name.c_str(), count, src.size());
        std::vector<GpuSplat> gpu(count);
        for (int k = 0; k < 3; ++k) { g.localMin[k] = 1e30f; g.localMax[k] = -1e30f; }
        for (size_t i = 0; i < count; ++i) {
            const Splat& s = src[i];
            float cov[6];
            splatCovariance(s, cov);
            GpuSplat& o = gpu[i];
            std::memcpy(o.pos, s.pos, 12);
            o.cov01 = half(cov[0]) | static_cast<uint32_t>(half(cov[1])) << 16;
            o.cov23 = half(cov[2]) | static_cast<uint32_t>(half(cov[3])) << 16;
            o.cov45 = half(cov[4]) | static_cast<uint32_t>(half(cov[5])) << 16;
            o.rgba = s.rgba[0] | s.rgba[1] << 8 | s.rgba[2] << 16 | static_cast<uint32_t>(s.rgba[3]) << 24;
            o.pad = 0;
            for (int k = 0; k < 3; ++k) { g.localMin[k] = std::min(g.localMin[k], s.pos[k]); g.localMax[k] = std::max(g.localMax[k], s.pos[k]); }
        }
        g.splats = createDeviceBuffer(gpu.data(), gpu.size() * sizeof(GpuSplat), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!g.splats.buf) { LOG("splats %s: no memory for them", so.name.c_str()); continue; }
        bool ok = true;
        for (int c = 0; c < 2 && ok; ++c) {
            g.order[c] = createBuffer(count * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            g.ubo[c] = createBuffer(208, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);   // clipFromWorld[2], worldFromModel, viewport
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = splatPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &splatSetLayout_;
            ok = g.order[c].mapped && g.ubo[c].mapped && vkAllocateDescriptorSets(device_, &ai, &g.set[c]) == VK_SUCCESS;
            if (!ok) break;
            VkDescriptorBufferInfo bi[3] = {{g.splats.buf, 0, VK_WHOLE_SIZE}, {g.order[c].buf, 0, VK_WHOLE_SIZE}, {g.ubo[c].buf, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[3] = {};
            for (uint32_t k = 0; k < 3; ++k) {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[k].dstSet = g.set[c]; w[k].dstBinding = k; w[k].descriptorCount = 1;
                w[k].descriptorType = k < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[k].pBufferInfo = &bi[k];
            }
            vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
        }
        if (!ok) {
            LOG("splats %s: no memory for their order", so.name.c_str());
            destroy(g.splats);
            for (int c = 0; c < 2; ++c) { destroy(g.order[c]); destroy(g.ubo[c]); }
            continue;
        }
        g.count = static_cast<uint32_t>(count);
        g.sorter = std::make_unique<SplatSorter>(src, count);
        total += count;
    }
    LOG("uploaded %zu splats in %zu objects (%.0f ms)", total, splatObjs_.size(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void HslRenderer::destroySplats() {
    for (SplatGpu& g : splatObjs_) {
        g.sorter.reset();                           // (its thread stops first)
        destroy(g.splats);
        for (int c = 0; c < 2; ++c) { destroy(g.order[c]); destroy(g.ubo[c]); }
    }
    splatObjs_.clear();
    if (splatPool_) vkDestroyDescriptorPool(device_, splatPool_, nullptr);   // (its sets with it)
    splatPool_ = VK_NULL_HANDLE;
    splatsUploaded_ = false;
    splatHeadSet_ = false;
    splatsDrawn_ = 0;
}

// Inside the frame's render pass, after the solid meshes and the sky. Each object in view gets a new order asked for
// when the head has moved a centimetre or the object has moved since its last one (at once: the first time it's
// drawn, and after a jump of over a metre, a teleport), and draws with the newest order there is.
void HslRenderer::drawSplats(const EyeView eyes[2], const float head[3], const float planes[2][5][4]) {
    splatsDrawn_ = 0;
    if (!splatPipe_ || splatObjs_.empty() || !scene_ || scene_->splats.size() != splatObjs_.size()) return;
    // Debug views: drawn in the ones over the home as itself, not where every mesh is replaced; "see-through" hides them.
    if (debugView_ == kViewMeshes || debugView_ == kViewOverdraw || (debugHide & kHideSeeThrough)) return;
    const bool jumped = splatHeadSet_ && (head[0] - splatHead_[0]) * (head[0] - splatHead_[0]) + (head[1] - splatHead_[1]) * (head[1] - splatHead_[1]) +
                                         (head[2] - splatHead_[2]) * (head[2] - splatHead_[2]) > 1.0f;
    std::memcpy(splatHead_, head, 12);
    splatHeadSet_ = true;
    float clip[32];
    for (int e = 0; e < 2; ++e) mat4mul4(eyes[e].proj.m, eyes[e].view.m, clip + 16 * e);
    const float viewport[4] = {static_cast<float>(width_), static_cast<float>(height_), 1.0f / std::max(1u, width_), 1.0f / std::max(1u, height_)};
    bool bound = false;
    for (size_t i = 0; i < splatObjs_.size(); ++i) {
        SplatGpu& g = splatObjs_[i];
        const HslSplatObject& so = scene_->splats[i];
        if (!g.count || !g.sorter || so.hidden) continue;
        const float* w = so.worldFromModel;
        // its box in the world: off both eyes' views, nothing to draw (and nothing to sort)
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {c & 1 ? g.localMax[0] : g.localMin[0], c & 2 ? g.localMax[1] : g.localMin[1], c & 4 ? g.localMax[2] : g.localMin[2]};
            for (int k = 0; k < 3; ++k) {
                const float v = w[k] * p[0] + w[4 + k] * p[1] + w[8 + k] * p[2] + w[12 + k];
                mn[k] = std::min(mn[k], v);
                mx[k] = std::max(mx[k], v);
            }
        }
        bool seen = false;
        for (int e = 0; e < 2 && !seen; ++e) {
            bool in = true;
            for (int k = 0; k < 5 && in; ++k) {
                const float* pl = planes[e][k];
                in = pl[0] * (pl[0] >= 0 ? mx[0] : mn[0]) + pl[1] * (pl[1] >= 0 ? mx[1] : mn[1]) + pl[2] * (pl[2] >= 0 ? mx[2] : mn[2]) + pl[3] >= 0;
            }
            seen = in;
        }
        if (!seen) continue;
        // a new order: from where the head is now, for where the object is now
        const float dx = head[0] - g.sortEye[0], dy = head[1] - g.sortEye[1], dz = head[2] - g.sortEye[2];
        const bool moved = std::memcmp(g.sortWorld, w, 64) != 0;
        if (!g.sorted || jumped || moved || dx * dx + dy * dy + dz * dz > 1e-4f) {
            g.sorter->request(head, w, !g.sorted || jumped);
            std::memcpy(g.sortEye, head, 12);
            std::memcpy(g.sortWorld, w, 64);
            g.sorted = true;
        }
        uint64_t serial = 0;
        std::shared_ptr<const SplatSorter::Order> order = g.sorter->latest(serial);
        if (!order || order->size() != g.count) continue;
        if (g.orderSerial[cmdIndex_] != serial) {   // this command buffer's copy (the other one's may still be read)
            std::memcpy(g.order[cmdIndex_].mapped, order->data(), order->size() * sizeof(uint32_t));
            g.orderSerial[cmdIndex_] = serial;
        }
        order.reset();                              // (its buffer goes back to the sorter)
        u8* u = static_cast<u8*>(g.ubo[cmdIndex_].mapped);
        std::memcpy(u, clip, 128);
        std::memcpy(u + 128, w, 64);
        std::memcpy(u + 192, viewport, 16);
        if (!bound) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, splatPipe_); bound = true; }
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, splatLayout_, 0, 1, &g.set[cmdIndex_], 0, nullptr);
        vkCmdDraw(cmd_, 4, g.count, 0, 0);
        splatsDrawn_ += g.count;
    }
}
