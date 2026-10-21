#pragma once
// Stereo Vulkan renderer for Meta HSL homes that runs each home's OWN shaders (RENDSHAD forward pass).
// Every resource a shader declares is bound by its SPIR-V name, the way libshell binds them:
//   set0  globalUniforms (both eyes' clipFromWorld / viewFromWorld / camera, time) + linearWrapSampler
//   set1  lighting: lightUniforms, sbLightItems, lightGridCells, IBL cubes, shadow map + compare sampler
//   set2  per material: matParams (cooked constants bound by MurmurHash3 name), lightprobesParams,
//         lightBakerParams, instance, skinning, samplers, and textures routed by name
// Both eyes render in ONE multiview pass into a 2-layer target, then go to vrcompositor as layers 0/1.
#include <unordered_set>
#include "guide_geom.h"
#include "hsl_scene.h"
#include "shader_bake.h"
#include "spv_reflect.h"

#ifdef FH_USE_VOLK
#include <volk.h>                       // Windows: Vulkan entry points loaded at runtime (no SDK needed)
#else
#include <vulkan/vulkan.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Mat4 { float m[16]; };           // column-major

// Largest texture side decoded for GPUs without native ASTC (Frame Studio's setting; 0 = no limit). Applies to
// textures decoded from then on.
void setTextureCap(uint32_t side);

struct EyeView {
    Mat4 proj;                          // clip-from-view (Vulkan, reversed-Z)
    Mat4 view;                          // view-from-world
    float position[3];                  // eye position in world
};

class HslRenderer {
public:
    // Device setup on the GPU and with the extensions SteamVR requires.
    bool init(uint32_t eyeWidth, uint32_t eyeHeight);
    // Uploads a home (replacing any previous one).
    bool load(const HslScene& scene);
    // Incremental load: beginLoad, then loadStep(ms) once per frame until it returns true.
    void beginLoad(const HslScene& scene);
    bool loadStep(double budgetMs);
    float loadProgress() const;
    void unload();
    // Renders both eyes; the result is left in `colorImage()` layers 0 (left) / 1 (right), TRANSFER_SRC layout.
    void render(const EyeView eyes[2], float timeSec);
    // Copies one eye layer of the last frame to RGBA8 (sRGB-encoded) — headless snapshots / tests.
    bool readback(uint32_t layer, std::vector<u8>& rgba);
    void shutdown();
    // New eye size (window viewports); programs, pipelines and the loaded home are kept.
    void resize(uint32_t eyeWidth, uint32_t eyeHeight);
    // UNORM 2D view of one eye layer: the frame's sRGB bytes as-is, for display in a UI.
    VkImageView layerView(uint32_t layer) const { return layerViews_[layer]; }
    // A picture for the UI (an environment's thumbnail): its RGBA8 bytes as-is, like layerView. Kept until
    // releasePicture (once no frame in flight still draws it) or shutdown.
    VkImageView createPicture(const u8* rgba, uint32_t w, uint32_t h);
    void releasePicture(VkImageView view);

    // Texture streaming: deferred (still-compressed) textures are uploaded as ASTC when the GPU samples it,
    // otherwise decoded on background threads and swapped in a few per frame (render() does this).
    size_t texturesPending();
    // The previous frame is still on the GPU: a UI that skips this frame instead of calling render() never
    // blocks (WebGPU never makes HSR Studio's page wait either).
    bool gpuBusy() const;
    // Load-time: decode and upload every texture of the home now (not as objects come into view), so moving and
    // looking around never waits on a texture. Progress 0..1; render() uploads with a larger budget meanwhile.
    void requestAllTextures();
    float textureProgress();
    size_t drawnCount() const { return drawn_; }   // meshes that passed culling last frame
    void finishStreaming();                 // blocks until every queued texture is in (snapshots)

    // Editing: the meshes that were uploaded (draw order), each tied to its HslScene::meshes index.
    size_t meshCount() const { return meshes_.size(); }
    int meshSceneIndex(size_t i) const { return meshes_[i].sceneIndex; }
    const float* meshWorld(size_t i) const { return meshes_[i].world; }
    void setMeshWorld(size_t i, const float world[16]);
    bool meshVisible(size_t i) const { return meshes_[i].visible; }
    int meshBlend(size_t i) const { return meshes_[i].blend; }        // 0 opaque, 1 alpha, 2 additive (as drawn)
    bool meshDynamic(size_t i) const { return meshes_[i].dynamic; }
    // The home isn't on screen (faded out, nobody looking): textures stream in big steps.
    void setUploadsUnseen(bool unseen) { uploadsUnseen_ = unseen; }
    void setMeshVisible(size_t i, bool visible) { meshes_[i].visible = visible; }
    // Scene meshes whose vertices change every frame (HslScene::animatedMeshes). Set before beginLoad().
    void setDynamicMeshes(const std::vector<int>& sceneIndices);
    // Highlight one mesh (its faces tinted, every triangle edge drawn) on the GPU; -1 clears. Built once per call,
    // nothing per frame on the CPU.
    void setSelection(int mesh);
    // Guide geometry: world-space triangles drawn over the home, unlit and alpha blended; depth tested so walls hide
    // them, with a faint copy where they do. False from guideAvailable(): the pipelines couldn't be built (draws nothing).
    // Two kinds, both drawn every frame until replaced ({} clears):
    //  - setGuide: what changes from frame to frame (guide_geom.h: the teleport arc, landing and seat markers, the
    //    vignette). Copied into a host-visible buffer each frame, so keep it small.
    //  - setStaticGuide: what stays put for many frames (the walkable area, debug_geom.h's world layer). Uploaded once
    //    into video memory. Waits for the GPU when it replaces geometry (a hitch), so not for every frame.
    void setGuide(const std::vector<GuideVertex>& tris) { guideVerts_ = tris; }
    void setStaticGuide(const std::vector<GuideVertex>& tris);
    // System mode: frames recorded while the GPU finishes the one before (see render()). FH_NO_EARLY_RECORD=1: never.
    void setEarlyRecord(bool on) { earlyRecord_ = on; }
    bool guideAvailable() const { return guide_.visible != VK_NULL_HANDLE; }
    const std::string& meshProgram(size_t i) const { return programs_[meshes_[i].program].path; }

    // Debug views (Frame Studio's View > Debug View, the Environments panel's Debug page): how a home is put together
    // and what it costs, drawn from each mesh's positions alone (skinned meshes in their rest pose).
    //   Wireframe  every triangle edge over the home (see-through meshes blue, cut-out green)
    //   Meshes     each mesh in its own colour, shaded by its slope (no textures or lighting)
    //   Cost       the home tinted by what each mesh costs the GPU: green from costScaleMs / 50 down, yellow, red from
    //              costScaleMs up. Timed per draw on desktop GPUs; on tile-based ones (the Frame) by drawing each mesh
    //              twice in turn and timing what that adds, so a full round takes a couple of seconds
    //              (measured per draw, which slows the GPU a little while it's on)
    //   Overdraw   every mesh added up, nothing hidden: bright where many surfaces cover the same pixels
    enum DebugView { kViewNormal = 0, kViewWireframe, kViewMeshes, kViewCost, kViewOverdraw, kDebugViews };
    void setDebugView(int view);
    int debugView() const { return debugView_; }
    bool wireframeAvailable() const { return wireOk_; }     // the GPU draws lines (fillModeNonSolid)
    // Leaves parts of the home out, to find the part that's slow or wrong (bits, applies from the next frame).
    enum DebugHide : unsigned { kHideSeeThrough = 1, kHideCutout = 2, kHideSky = 4, kHideAnimated = 8, kHideSolid = 16 };
    unsigned debugHide = 0;
    float costScaleMs = 0.25f;
    // Cost view: a mesh's GPU time (ms, smoothed), by draw index; < 0 not measured.
    float meshCostMs(size_t i) const { return i < cost_.size() ? cost_[i] : -1.0f; }
    // The n costliest meshes as last measured: (ms, HslScene::meshes index), costliest first.
    std::vector<std::pair<float, int>> costliest(size_t n) const;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkImage colorImage() const { return resolve_.image; }
    // Frame Home as the system environment (env_share.h): n images like the eye images that another process (the
    // compositor) can import, as opaque fds it takes ownership of. Recreate after resize(). render() copies the eye
    // images into exportSlot (when >= 0) at the end of the frame; waitFrame() blocks until that frame is done.
    struct ExportImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize size = 0; int fd = -1; };
    bool createExportImages(uint32_t n, std::vector<ExportImage>& out, uint32_t& memoryType, uint8_t deviceUUID[16]);
    // Set before init(): frames are drawn straight into the export images (their MSAA resolve lands there) instead of
    // being copied in at the end of the frame (1.1-1.5 ms a frame on the Frame). Foveation then redraws its density
    // map where you look each frame rather than offsetting it (the images can't carry the offset flag across).
    bool exportDirect = false;
    // Set before init(), with exportDirect: with smooth foveation the export images are the foveated pass's own
    // subsampled output, and the compositor layer reads them through a subsampled sampler, which smooths the coarse
    // regions as it samples. That replaces our full-screen smoothing pass (~0.65 ms a frame at 2416x2416 per eye).
    bool exportSubsampled = false;
    bool exportsSubsampled() const { return exportDirect && exportSubsampled && smoothFov_; }   // (known after init())
    uint32_t exportUsage() const;                       // the export images' usage flags (the layer imports them alike)
    uint32_t exportCreateFlags() const;                 // ...and their VkImageCreateFlags (likewise)
    void destroyExportImages(std::vector<ExportImage>& images);
    int exportSlot = -1;
    void waitFrame();
    // Frames counted on the GPU (a timeline semaphore, when the device has them): render() signals frameSerial() once
    // that frame is done, and waitFrameSerial() may be called from another thread (system mode publishes from one).
    bool hasFrameSerial() const { return timeline_ != VK_NULL_HANDLE; }
    uint64_t frameSerial() const { return serial_; }
    bool waitFrameSerial(uint64_t serial, uint64_t timeoutNs);
    // The GPU was reset under this device (a hang anywhere on the GPU can take it): render() does nothing from then
    // on, and only a new device (Frame Home restarts itself) draws again.
    bool deviceLost() const { return lost_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    float clearColor[3] = {0, 0, 0};     // linear; shows wherever the home draws nothing
    std::string pipelineCachePath;       // compiled pipelines saved here between runs (empty = no file)
    uint32_t viewCount = 2;              // set before init(): 2 = both eyes (VR), 1 = a single view (editor, half the work)
    // Performance options, set before init() (Frame Home / Studio settings):
    int msaa = 4;                         // samples: 1, 2, 4 or 8 (the largest the GPU has up to this)
    float anisotropy = 4;                 // texture filtering, 1..16 (clamped to the GPU's limit)
    // Foveated rendering: full detail where you look, coarser toward the edges (VK_EXT_fragment_density_map with
    // VK_QCOM_fragment_density_map_offset to move it each frame). Used only when the GPU has both.
    bool foveation = false;
    // Strength 1 (gentle) .. 3 (strong); where each eye looks, in pixels of its image (from eye tracking, or the
    // lens centre). Both take effect from the next frame.
    void setFoveationLevel(int level);
    // The same on a continuous scale (0 = full detail everywhere, 1-3 = the levels): the rings move smoothly with it
    // (the thermal governor turns it a little at a time). Steps of 1/32.
    void setFoveationStrength(float strength);
    float foveationStrength() const { return fovStrength_; }
    void setFoveationCenter(uint32_t eye, float px, float py) { if (eye < 2) { fovCenter_[eye][0] = px; fovCenter_[eye][1] = py; } }
    bool foveationActive() const { return fdmOn_; }

    // Provided by the caller (OpenVR knows which GPU and extensions the compositor needs).
    std::vector<std::string> instanceExtensions, deviceExtensions;
    VkPhysicalDevice (*pickPhysicalDevice)(VkInstance) = nullptr;

private:
    // memOffset/memSize/pooled: a range of a shared MemBlock (see allocMemory) rather than its own allocation.
    struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0;
                    VkDeviceSize memOffset = 0, memSize = 0; bool pooled = false; };
    struct Image  { VkImage image = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
                    VkDeviceSize memOffset = 0, memSize = 0; bool pooled = false; };
    // Suballocation, as Dawn (HSR Studio's WebGPU) does it: thousands of small vkAllocateMemory calls make the
    // Windows video memory manager stall the queue for seconds. Buffers and images keep separate blocks.
    struct MemBlock { VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize size = 0; uint32_t type = 0; bool linear = false;
                      void* mapped = nullptr; std::map<VkDeviceSize, VkDeviceSize> free; };
    struct MemRange { VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize offset = 0, size = 0; void* mapped = nullptr; bool pooled = false; };
    std::vector<MemBlock> blocks_;
    std::mutex allocMutex_;
    MemRange allocMemory(const VkMemoryRequirements& req, uint32_t type, bool linear, bool map);
    void freeMemory(VkDeviceMemory mem, VkDeviceSize offset, VkDeviceSize size, bool pooled, bool mapped);
    uint32_t hostMemoryType(uint32_t bits);

    enum class Role { Pos, Normal, Uv0, Uv1, Color, BoneIdx, BoneWgt, Tangent, Zero, Uv2, Uv3 };
    struct VertexAttr { uint32_t location; VkFormat format; uint32_t offset, size; Role role; bool integer; };

    struct Program {
        std::string path;
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModule fsAlpha = VK_NULL_HANDLE;    // fs writing (a,a,a,a): its colour work left for the compiler to drop
        std::vector<VertexAttr> attrs;
        uint32_t stride = 0;
        std::vector<spv::Resource> sets[3];         // merged vert+frag, sorted by binding
        VkDescriptorSetLayout setLayouts[3] = {};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet set0 = VK_NULL_HANDLE, set1 = VK_NULL_HANDLE;
        uint32_t pushSize = 0;
        int pcWorld = -1, pcColor = -1, pcUvU = -1, pcUvV = -1;             // push-constant member offsets
        std::map<uint32_t, VkPipeline> pipelines;   // key: blend mode * 2 + cull
        bool multiview = false;
        bool lightBaker = false;                    // has lightBakerParams (lightmap atlas uvScale/uvOffset)
        bool clay = false;                          // a built-in Clay shader: fragment alpha = texture alpha x tint alpha
        // Specialized fragment shaders (spv_specialize.h): the uniform values constant for a material baked in. One per
        // distinct set of those values, shared by the meshes that have it; each with its own pipelines.
        struct Variant { VkShaderModule fs = VK_NULL_HANDLE, fsAlpha = VK_NULL_HANDLE; std::map<uint32_t, VkPipeline> pipelines; };
        const ShaderProgramSrc* src = nullptr;      // the scene's (its baked variants)
        bool boned = false;                         // its vertex stage reads bone indices
        std::vector<std::pair<uint32_t, uint32_t>> foldBlocks;   // (set, binding) of the blocks it reads constant members of
        std::vector<Variant> variants;
        std::map<uint64_t, int> variantFor;         // hash of those blocks' bytes -> variant (-1: nothing folds)
    };

    struct GpuMesh {
        int program = -1;
        bool cutOnce = false;                        // a cut-out drawn in one pass (its shader has no alpha-only form)
        int variant = -1;                            // its Program::variants entry (-1: the shader as shipped)
        int sceneIndex = -1;
        bool visible = true;
        Buffer vbo, ibo;
        uint32_t indexCount = 0;
        std::vector<Image> images;
        std::vector<Buffer> ubos;
        VkDescriptorSet set2 = VK_NULL_HANDLE;
        float world[16];
        float color[4] = {1, 1, 1, 1};
        int blend = 0;                              // 0 opaque, 1 alpha, 2 additive
        bool cull = false, skybox = false;
        float centroid[3] = {0, 0, 0};
        float localCentroid[3] = {0, 0, 0};
        float localMin[3] = {0, 0, 0}, localMax[3] = {0, 0, 0};
        float worldMin[3] = {0, 0, 0}, worldMax[3] = {0, 0, 0};
        bool inView = true;                         // passed the view-frustum test this frame
        std::vector<std::pair<const TexSource*, bool>> wants;   // streamed textures still to request
        int skinUbo = -1;                           // index in ubos of the sbSkinningMatrices buffer
        bool alphaToCoverage = false;               // cut-out foliage (SHAD pass state)
        bool dynamic = false;                       // animated on the CPU: host-visible vertices, rewritten per frame
        uint32_t posGen = 0, uvGen = 0;             // the MeshData posGen / uvGen its vertex buffer holds
        uint32_t skinGen = 0;                       // the MeshData skinGen its skinning buffer holds
        float baseWorld[16];                        // its own world; world = baseWorld * anim
        float anim[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // MeshData::animXform last applied
        float uvXform[6] = {1, 0, 0, 0, 1, 0};      // MeshData::uvXform (pushed as uvTransformU / V)
        bool debug = false;                         // FH_DEBUG matched: cull state logged once
        std::string debugName;
    };

    // Vulkan plumbing
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    Buffer createDeviceBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy(Buffer& b);
    void destroy(Image& i);
    std::vector<Image> pictures_;                               // createPicture's
    void retire(Buffer& b);
    VkCommandBuffer recordInto_ = VK_NULL_HANDLE;              // set while uploads ride in the frame's commands
    std::vector<Buffer> frameStaging_;                          // their staging, freed after that frame
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxQueries = 8192;   // the Cost view times every draw (2 per eye each)
    VkQueryPool queryPool_ = VK_NULL_HANDLE;
public:
    float lastGpuMs() const { return gpuMs_; }
    // FH_PERF=name,name...: the GPU's own counters (VK_KHR_performance_query; on Turnip with TU_DEBUG=perfc) whose
    // names contain one of these, summed over the frames read back; FH_PERF=list logs every counter there is.
    std::vector<std::pair<std::string, double>> perfCounterAverages() const;
    uint64_t gpuResults() const { return gpuResults_; }   // counts lastGpuMs updates (a frame without one keeps the old value)
    // CPU side of the last render() (FH_PROFILE / --bench): where its time went, in ms.
    struct FrameStats {
        double waitMs = 0, readbackMs = 0, animMs = 0, globalsMs = 0, dynamicMs = 0, uploadMs = 0, recordMs = 0, submitMs = 0, totalMs = 0;
        uint32_t uploads = 0, dynamicRebuilt = 0, dynamicMeshes = 0, draws = 0, pipelineBinds = 0;
        uint64_t triangles = 0;                             // drawn this frame (per eye)
        bool early = false;                                 // recorded while the GPU still drew the frame before
    };
    const FrameStats& lastStats() const { return stats_; }
    float lastGpuUploadMs() const { return gpuUploadMs_; }  // parts of lastGpuMs: texture uploads, and the copy of the
    float lastGpuCopyMs() const { return gpuCopyMs_; }      // frame into the compositor's slot (system mode)
private:
    FrameStats stats_;
    // Load-time uploads (loadStep) ride in one command buffer per step instead of a queue round trip each.
    VkCommandBuffer loadCmd_ = VK_NULL_HANDLE;
    VkDeviceSize loadStaged_ = 0;                               // staging bytes recorded into loadCmd_
    std::vector<Buffer> loadStaging_;                           // ...and their staging, freed when it's flushed
    void beginUploadBatch();
    void flushUploadBatch();
    float timestampPeriod_ = 0;
    bool fdmOn_ = false;                                    // foveation running (render pass has a density map)
    Image fdm_;                                             // the density map: 2x the eye image, fovea in the middle
    uint32_t fdmTexel_ = 32, fdmGranularity_ = 8, fdmGranularityY_ = 8, fdmW_ = 0, fdmH_ = 0;
    VkFormat fdmFormat_ = VK_FORMAT_UNDEFINED;
    int fovLevel_ = 2;
    float fovStrength_ = 2.0f;
    float fovCenter_[2][2] = {{-1, -1}, {-1, -1}};
    void writeDensityMap();
    float gpuUploadMs_ = 0, gpuCopyMs_ = 0;
    float gpuMs_ = 0, gpuSum_ = 0, gpuMax_ = 0;             // GPU time of the last frame, and a running average
    int gpuFrames_ = 0;
    uint64_t gpuResults_ = 0;
    uint32_t profNext_ = 0;
    std::vector<std::pair<size_t, uint32_t>> profDraws_;      // mesh, first query
    bool profFrame_ = false;                                  // frame timestamps written (last 3 queries)
    uint32_t lastUploads_ = 0, prevUploads_ = 0;              // textures recorded this frame / in the frame being waited on
    double lastUploadMB_ = 0, prevUploadMB_ = 0;
    template <typename F> void oneShot(F&& record);
    Image createImage2D(uint32_t w, uint32_t h, uint32_t layers, uint32_t mips, VkSampleCountFlagBits samples,
                        VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImageViewType viewType,
                        VkImageCreateFlags flags = 0);
    Image createTexture(const u8* rgba, uint32_t w, uint32_t h, bool srgb);
    Image createTextureFmt(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt);
    Image createSolidCube(const float top[3], const float side[3], const float bottom[3]);
    Image createSolid3D(const u8 rgba[4]);
    // `bytes`: what `faces` holds; a cube whose levels need more is refused (Image{}), never over-read.
    Image createCubeTexture(const void* faces, size_t bytes, uint32_t size, VkFormat fmt = VK_FORMAT_R8G8B8A8_SRGB, uint32_t mips = 1,
                            uint32_t levels = 0);
    Image createShadowDepth();

    void createTargets();
    void createSharedResources();
    int programFor(const ShaderProgramSrc& src);
    // Cut-out (alpha-to-coverage) meshes in two passes: kCutDepth writes their depth from fsAlpha (Meta's shader
    // computing only its alpha), kCutShade shades them with depth writes off, the front-most sample alone passing.
    enum CutPass { kCutNone = 0, kCutDepth = 1, kCutShade = 2 };
    VkPipeline pipelineFor(Program& p, int blend, bool cull, bool sky = false, bool a2c = false, CutPass cut = kCutNone, int variant = -1);
    // The variant of p for a mesh whose set-2 uniform buffers hold `set2` (binding -> bytes): made on first use.
    int variantFor(Program& p, const std::map<uint32_t, std::vector<u8>>& set2);
    shaderbake::SceneConstants sceneConsts_;    // the scene-wide uniform bytes that never change (shader_bake.h)
    size_t specMade_ = 0, specBaked_ = 0;       // this load's specialized shaders: made at load / from the .fenv
    void writeSharedSets(Program& p);
    VkImageView textureFor(const spv::Resource& r, GpuMesh& gm, const MeshData& md);
    bool uploadMesh(const MeshData& md, int sceneIndex);
    void updateGlobals(const EyeView eyes[2], float timeSec);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkQueryPool perfPool_ = VK_NULL_HANDLE;
    std::vector<std::string> perfNames_;
    std::vector<VkPerformanceCounterStorageKHR> perfStorage_;
    std::vector<double> perfSum_;
    uint32_t perfFrames_ = 0;
    bool perfRecorded_ = false;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t serial_ = 0;
    std::atomic<bool> lost_{false};             // (set by the publisher thread's waits too)
    bool fenceUnsignalled_ = false;             // the last submit failed: its fence never signals
    std::vector<VkImage> exportImages_;          // createExportImages: render() copies into exportImages_[exportSlot]
    VkRenderPass exportPass_ = VK_NULL_HANDLE;   // exportDirect: renderPass_ ending in GENERAL, for the export images
    std::vector<VkImageView> exportViews_;
    std::vector<VkFramebuffer> exportFbs_;       // per export image
    int lastExportSlot_ = -1;                    // the export image the last frame was drawn into (readback)
    bool fdmOffset_ = false;                     // foveation moved by VK_QCOM_fragment_density_map_offset
    bool fdmInit_ = false;                       // the density map has been written (its layout is FRAGMENT_DENSITY_MAP_OPTIMAL)
    int64_t fdmKey_ = -1;                        // the density map's centres and level as last written (not rewritten unchanged)
    void createExportFramebuffers();
    void destroyExportFramebuffers();
    void densityTexels(float cx, float cy, std::vector<float>& dens) const;   // two per texel: across, down
    void packDensity(const std::vector<float>& dens, uint8_t* dst) const;   // in the map's format
    std::vector<bool> exportInited_;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    // Selection overlay (selection_spirv.h): edges as a line list + faces, solid colour from a 1x1 texture.
    struct Selection {
        int mesh = -1;
        Buffer vbo, edges, tris, ubo;
        uint32_t edgeCount = 0, triCount = 0;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
        VkPipeline lines = VK_NULL_HANDLE, faces = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Image color;
    } sel_;
    void createSelectionPipelines();
    // Guide geometry: setGuide's goes into a host-visible vertex buffer per command buffer, rewritten each frame;
    // setStaticGuide's into one device-local buffer. `hidden` draws where the home is in front, faintly.
    struct Guide {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline visible = VK_NULL_HANDLE, hidden = VK_NULL_HANDLE;
        Buffer vbo[2];                              // per command buffer: one is written while the other may be read
        Buffer staticVbo;                           // setStaticGuide's, in video memory
        uint32_t staticVertexCount = 0;
    } guide_;
    std::vector<GuideVertex> guideVerts_;
    void createGuidePipelines();
    void destroyGuide();
    void drawGuide(const EyeView eyes[2]);
    void destroySelection();
    // Debug views (setDebugView): one pipeline per vertex layout and mode, and the draws' matrices and colours in
    // one host-visible buffer (indexed by firstInstance), rewritten each frame after the fence.
    enum DebugMode { kDbgSolid = 0, kDbgTint, kDbgWire, kDbgAdd };
    struct Debug {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        Buffer draws;
        std::map<uint64_t, VkPipeline> pipes;       // (stride, position offset, format, mode)
        uint32_t used = 0, capacity = 0;
    } dbg_;
    int debugView_ = kViewNormal;
    bool wireOk_ = false;
    std::vector<float> cost_;
    bool tiler_ = false;                                    // tile-based GPU: the Cost view draws meshes twice to time them
    int timedSlot_ = -1;                                    // the export slot of the frame being timed
    int doubled_ = -1;                                      // drawn twice in the last frame (timed at the next render())
    size_t costNext_ = 0;
    uint64_t costFrame_ = 0;
    static constexpr uint32_t kCostSamples = 6, kCostBase = 48;
    std::vector<float> costMin_;                            // per mesh: its last kCostSamples doubled-frame times
    std::vector<uint32_t> costSeen_;
    float costBaseRing_[kCostBase] = {1e9f};                // the last baseline frames (nothing doubled)
    uint32_t costBaseNext_ = 0;
    std::vector<std::pair<double, size_t>> opaqueOrder_;      // render(): this frame's opaque draw order (kept: no allocation)
    bool createDebug();
    void destroyDebug();
    VkPipeline debugPipeline(const Program& p, int mode);
    void drawDebug(const GpuMesh& gm, int mode, const float color[4], const EyeView eyes[2], bool nudge);
    std::unordered_set<int> dynamicScene_;
    // cpu: tints, matrices, bounds and which meshes rebuild (no GPU memory); upload: those rebuilds and the bones,
    // into what the GPU reads (only once the frame before is done with them).
    void refreshDynamicMeshes(bool cpu, bool upload);
    struct DynJob { GpuMesh* gm; const MeshData* md; bool pos, uv; };
    std::vector<DynJob> dynJobs_;
    std::vector<GpuMesh*> skinCopies_;              // GPU-skinned meshes whose bones changed
    bool canRecordEarly();
    int64_t fdmWantKey() const;                     // the foveation map this frame wants (fdmKey_ when it has it)
    void fdmCentres(int32_t cx[2], int32_t cy[2]) const;
    VkCommandBuffer cmds_[2] = {};
    int cmdIndex_ = 0;
    bool earlyRecord_ = false, fdmCopyRecorded_ = false;
    void buildVertices(const Program& p, const MeshData& md, std::vector<u8>& vbo, std::vector<float>& normals) const;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    float maxAniso_ = 1;
    uint32_t maxImageDim_ = 4096, maxCubeDim_ = 4096;   // the GPU's limits: larger sizes from a file are refused
    bool anisotropy_ = false;
    uint32_t width_ = 0, height_ = 0;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    Image colorMs_, depth_, resolve_;
    // Smooth foveation: the foveated pass draws into sub_ (subsampled: the driver keeps its coarse regions coarse),
    // and one full-screen pass reads it through the subsampled sampler (filtering between the coarse pixels) into
    // resolve_ or the compositor's image. Without it the coarse regions were stretched into blocks.
    bool smoothFov_ = false;
    Image sub_;
    VkRenderPass fovPass_ = VK_NULL_HANDLE;
    VkFramebuffer fovFb_ = VK_NULL_HANDLE;
    VkSampler subSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout fovSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout fovLayout_ = VK_NULL_HANDLE;
    VkPipeline fovPipe_ = VK_NULL_HANDLE;
    VkDescriptorPool fovPool_ = VK_NULL_HANDLE;
    VkDescriptorSet fovSet_ = VK_NULL_HANDLE;
    VkRenderPass colorOnlyPass(VkImageLayout finalLayout) const;   // one 1-sample colour attachment, both views
    void createFovPass();
    void recordFovPass(VkFramebuffer fb, VkRenderPass pass);
    VkImageView layerViews_[2] = {};

    VkSampler wrapSampler_ = VK_NULL_HANDLE, anisoSampler_ = VK_NULL_HANDLE, shadowSampler_ = VK_NULL_HANDLE;
    Image nullTex_;
    Image noMetal_;                         // white, alpha 0: BaseColorMetallic placeholder (never a chrome flash)
    size_t texturesRequested_ = 0;          // requestAllTextures: how many were queued (progress)
    bool preloading_ = false;               // the initial everything-at-once texture load is running
    Image white_, black_, flatNormal_, whiteCube_, white3D_, iblDiffuse_, iblReflection_, shadowMap_;
    Buffer globals_, lightUniforms_, lightItems_, lightGrid_;
    uint32_t globalsSize_ = 1024;
    std::vector<spv::Member> globalMembers_;

    std::vector<Program> programs_;
    std::map<std::string, int> programIndex_;
    std::vector<GpuMesh> meshes_;
    std::map<uint64_t, Image> textureCache_;        // content hash -> shared texture (freed on unload)
    const HslScene* scene_ = nullptr;
    size_t loadNext_ = 0, loadSkipped_ = 0;
    bool loadDone_ = true;
    std::chrono::steady_clock::time_point loadStart_;
    float ambient_[3] = {1, 1, 1};

    // ── Texture streaming ──
    using TexKey = std::pair<const TexSource*, bool>;           // source, sRGB
    struct TexSlot { size_t mesh; uint32_t binding, count; VkDescriptorType type; VkSampler sampler; };
    struct TexJob { std::shared_ptr<const TexSource> src; bool srgb; uint64_t gen; };
    // staging: its own buffer (load-time path) or, for streamed textures, the shared upload ring at stagingOffset;
    // pixels: a streamed texture's decoded texels, copied into the ring when its upload is recorded.
    struct PreparedTex { Image img; Buffer staging; VkDeviceSize stagingOffset = 0; std::vector<u8> pixels; uint32_t w = 0, h = 0, mips = 1; };
    struct TexDone { TexKey key; uint64_t gen; PreparedTex prep; bool ok; };
    PreparedTex prepareTexture(const void* pixels, uint32_t w, uint32_t h, VkFormat fmt, bool ownStaging = true);
    // One persistent upload buffer, reused every frame (as WebGPU's writeTexture does): per-texture staging
    // allocations made Windows page memory in and out and held the GPU queue for seconds.
    static constexpr VkDeviceSize kRingSize = 256ull << 20;
    Buffer ring_;
    VkDeviceSize ringHead_ = 0;
    void recordTextureUpload(VkCommandBuffer c, const PreparedTex& t);
    VkImageView streamTexture(const std::shared_ptr<const TexSource>& src, bool srgb, VkImageView placeholder);
    void pumpTextures(double budgetMs);
    void startWorkers();
    void stopWorkers();
    VkFormat astcFormat(u32 bw, u32 bh, bool srgb) const;
    Image createAstcTexture(const TexSource& src, VkFormat fmt);
    bool astcLdr_ = false;                                      // GPU samples ASTC (the Frame's Adreno)
    bool astcHdr_ = false;                                      // ...and HDR-profile ASTC (lightmaps, panoramas) too
    // An HDR map (lightmap, panorama): uploaded once per source, as ASTC when the GPU can, else RGBA16F.
    // exact: data the shader reads texel by texel (a vertex-animation table): never shrunk.
    VkImageView hdrTexture(const std::vector<u16>* half, const std::shared_ptr<const TexSource>& src, u32 w, u32 h, bool exact = false);
    std::map<TexKey, VkImageView> streamed_;                    // in GPU memory
    std::map<TexKey, std::vector<TexSlot>> waiting_;            // descriptors showing a placeholder until it lands
    std::map<TexKey, std::shared_ptr<const TexSource>> pendingSrc_; // not decoded until something using it is in view
    std::map<TexKey, bool> requested_;
    size_t drawn_ = 0;
    bool uploadsUnseen_ = false;
    void updateBounds(GpuMesh& gm);
    void placeMesh(GpuMesh& gm);
    void measureMesh(GpuMesh& gm, const MeshData& md, uint32_t nv);
    void updateAnimations(float timeSec);
    std::map<int, size_t> sceneToMesh_;                         // HslScene::meshes index -> meshes_ index
    void requestTextures(GpuMesh& gm);
    bool slotPending_ = false;                                  // textureFor queued one: uploadMesh records the slot
    TexKey pendingKey_{};
    std::vector<std::shared_ptr<const TexSource>> keepAlive_;
    std::deque<TexJob> jobs_;
    std::vector<TexDone> done_;
    std::mutex jobMutex_;
    std::condition_variable jobCv_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    uint64_t generation_ = 0;                                   // bumped on unload: stale results are dropped
};
