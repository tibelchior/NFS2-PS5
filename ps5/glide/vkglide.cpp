// Vulkan implementation of the Glide 2.x subset NFS II SE uses (replaces repo/src/Glide2x.c + Glide2x/OpenGL2.c).
// Same behaviour as the OpenGL 2 version: perspective-correct textured triangles with a 1/w depth buffer,
// per-vertex table fog, alpha test at 16/255, additive or alpha blending, clamp-to-edge bilinear textures.
// The frame is rendered offscreen at kScale x 640x480, read back, and handed to the platform presenter
// (PS5: ut_ps5_present; host test: optional PPM dump). The game is single threaded as far as Glide is concerned.
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <string>

#ifdef NFS_PS5
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
struct MhVulkanDevice {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    bool anisotropy, line_fill;
};
bool ut_ps5_vulkan_device(MhVulkanDevice *out);                                   // ps5_present.cpp
void ut_ps5_present(const void *pixels, int width, int height, int pitch);        // ps5_present.cpp
void ut_ps5_log(const char *text);
extern "C" void lowmem_stats(unsigned long *, unsigned long *, unsigned long *);
extern "C" unsigned long lowmem_failures(void);                                                // ps5_io.cpp
}
#else
#include <dlfcn.h>
#include <unistd.h>
#include <SDL2/SDL.h>
extern "C" SDL_Window *sdlWin;
#endif

#include "glide_vert.h"
#include "glide_frag.h"

// Referenced by Wrapper.c's error reporter.
extern "C" {
int32_t contextError = 0, shaderError = 0, framebufferError = 0;
}

namespace {

constexpr int kGlideW = 640, kGlideH = 480;
constexpr uint32_t kBigLayers = 512;   // 256x256 layers for textures larger than 64 (128 MB)
constexpr uint32_t kSmallLayers = 3072; // 64x64 layers for the many small ones (50 MB)
constexpr uint32_t kSmallSize = 64;
uint32_t kLayers = kBigLayers + kSmallLayers;   // layer ids: 0..kBigLayers-1 big, then small           // 256x256 RGBA8 layers (335 MB); every live Glide texture owns one (the game keeps ~1100 alive)
constexpr uint32_t kLayerSize = 256;
constexpr VkDeviceSize kStagingSize = 48u << 20;
constexpr uint32_t kMaxVertices = 1u << 21;
constexpr uint32_t kTextureMem = 0x200000;
constexpr uint32_t kVertexSnap = 0x0C0000;

// Glide constants (Glide2x.h)
constexpr int32_t GR_TEXFMT_P_8 = 0x5, GR_TEXFMT_RGB_565 = 0xa, GR_TEXFMT_ARGB_1555 = 0xb, GR_TEXFMT_ARGB_4444 = 0xc;
constexpr int32_t GR_BLEND_ONE = 0x4, GR_BLEND_ONE_MINUS_SRC_ALPHA = 0x5;
constexpr int32_t GR_COMBINE_OTHER_TEXTURE = 0x1;
constexpr int32_t GR_FOG_DISABLE = 0x0, GR_FOG_WITH_TABLE = 0x2;
constexpr uint32_t GR_LFB_WRITE_ONLY = 0x01, GR_TEXTABLE_PALETTE = 0x2;
constexpr int32_t GR_TEXTURECLAMP_WRAP = 0;
constexpr int GR_FOG_TABLE_SIZE = 64;

struct GrTmuVertex { float sow, tow, oow; };
struct GrVertex {
    float x, y, z, r, g, b, ooz, a, oow;
    GrTmuVertex tmuvtx[2];
};
struct GrTexInfo { int32_t smallLod, largeLod, aspectRatio, format; uint32_t data; };
struct GrLfbInfo { int32_t size; uint32_t lfbPtr; uint32_t strideInBytes; int32_t writeMode, origin; };

struct Vtx {
    float x, y, z;
    float s, t, q;
    uint8_t col[4];
    uint8_t fog, pad[3];
};
static_assert(sizeof(Vtx) == 32, "vertex layout");

struct TextureInfo {
    uint8_t *data;
    uint32_t palHash;
    int32_t fmt;
    uint32_t size;
    int32_t layer;
    uint32_t stored;   // edge of the texture as stored in its layer (the game's size, at most kLayerSize)
};

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
#ifdef NFS_PS5
    char line[560];
    snprintf(line, sizeof line, "[glide-vk] %s", text);
    ut_ps5_log(line);
#else
    fprintf(stderr, "[glide-vk] %s\n", text);
#endif
}

#define VK_FUNCS(X) \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkInvalidateMappedMemoryRanges) X(vkCreateSampler) X(vkCreateRenderPass) X(vkCreateFramebuffer) \
    X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)

struct Fn {
#define DECL(n) PFN_##n n = nullptr;
    VK_FUNCS(DECL)
#undef DECL
};

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
};

// ---- state ---------------------------------------------------------------------------------------------
Fn f;
Gpu gpu;
VkPhysicalDeviceMemoryProperties memProps{};
bool ready = false;
int kScale = 2;
int outW = 0, outH = 0;

VkCommandPool pool;
VkCommandBuffer cbUpload, cbMain;
VkFence fence;
VkImage colorImg, depthImg, texImg, texImgS;
VkImageView colorView, depthView, texView, texViewS;
VkSampler sampler;
VkRenderPass pass;
VkFramebuffer fb;
VkBuffer vtxBuf, stagingBuf, readBuf;
uint8_t *vtxMap, *stagingMap, *readMap;
VkDescriptorSetLayout dsl;
VkDescriptorPool dpool;
VkDescriptorSet dset;
VkPipelineLayout playout;
VkPipeline pipes[4];

bool upRecording = false, mainRecording = false, passOpen = false, scissorDirty = true;
int boundPipe = -1;
uint32_t vtxCount = 0, drawFirst = 0;
VkDeviceSize stagingUsed = 0;
std::vector<int32_t> freeBig, freeSmall, pendingLayers;
VkRect2D clipRect{};

// Glide state
bool texEnabled = false, fogEnabled = false, depthMask = true;
int32_t blendDst = GR_BLEND_ONE_MINUS_SRC_ALPHA;
float fogColor[3] = {0, 0, 0};
float gammaValue = 1.0f;
int32_t curLayer = 0;
bool curBig = false;
uint32_t curSize = 256;

uint8_t fogTable[0x10000];
uint8_t *lfb = nullptr;
uint8_t textureMem[kTextureMem];
const uint32_t *palette = nullptr;
#ifndef NFS_PS5
static std::vector<float> dbgT1;   // host trace: second-TMU coordinates per vertex (s,t)
#endif
uint32_t palCopy[256], palCopyHash;
TextureInfo *srcTi = nullptr;   // currently selected texture when paletted
uint32_t srcSize = 0;
TextureInfo textures[kTextureMem >> 2];
std::map<uint32_t, uint32_t> liveTex;
struct Stat { unsigned downloads = 0, uploads = 0, sources = 0; };
std::map<uint32_t, Stat> texStats;   // key: format<<16 | largeLod<<8 | aspect
unsigned statOverlapRetired = 0, statLayerFallback = 0, statUploadSkipped = 0, statDrawCalls = 0, statTriangles = 0;
std::map<std::string, unsigned> callStats;   // "function(args)" -> times the game called it
void logCall(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logCall(const char *fmt, ...) {
    char text[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    ++callStats[text];
}
unsigned statTmu1Verts = 0;
bool wrapS = false, wrapT = false;   // grTexClampMode: GR_TEXTURECLAMP_WRAP repeats the texture
int dbgDepthFunc = 3, dbgChroma = 0;
struct LayerInfo { int fmt; unsigned avg[4], minA, maxA; };
LayerInfo layerInfo[3584];   // per texture layer: what was uploaded (for the frame trace)   // last values the game set (for the frame trace)
inline uint32_t statKey(int fmt, int lod, int aspect) { return (uint32_t(fmt) & 0xff) << 16 | (uint32_t(lod) & 0xff) << 8 | (uint32_t(aspect) & 0xff); }   // start address -> bytes of every texture in texture memory
unsigned long frameCount = 0;

bool check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return true;
    say("%s failed: %d", what, int(r));
    return false;
}

bool memoryType(uint32_t bits, VkMemoryPropertyFlags want, uint32_t &out) {
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want) {
            out = i;
            return true;
        }
    return false;
}

bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool cached, VkBuffer &buf, uint8_t *&map) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(f.vkCreateBuffer(gpu.device, &bi, nullptr, &buf), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    f.vkGetBufferMemoryRequirements(gpu.device, buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    const VkMemoryPropertyFlags base = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!(cached && memoryType(req.memoryTypeBits, base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, mai.memoryTypeIndex)) &&
        !memoryType(req.memoryTypeBits, base, mai.memoryTypeIndex)) {
        say("no host-visible memory");
        return false;
    }
    VkDeviceMemory mem;
    if (!check(f.vkAllocateMemory(gpu.device, &mai, nullptr, &mem), "vkAllocateMemory(buffer)")) return false;
    if (!check(f.vkBindBufferMemory(gpu.device, buf, mem, 0), "vkBindBufferMemory")) return false;
    void *p = nullptr;
    if (!check(f.vkMapMemory(gpu.device, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) return false;
    map = static_cast<uint8_t *>(p);
    return true;
}

bool makeImage(VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags usage, VkImageAspectFlags aspect,
               VkImage &img, VkImageView &view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(f.vkCreateImage(gpu.device, &ii, nullptr, &img), "vkCreateImage")) return false;
    VkMemoryRequirements req;
    f.vkGetImageMemoryRequirements(gpu.device, img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    if (!memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mai.memoryTypeIndex) &&
        !memoryType(req.memoryTypeBits, 0, mai.memoryTypeIndex)) {
        say("no image memory");
        return false;
    }
    VkDeviceMemory mem;
    if (!check(f.vkAllocateMemory(gpu.device, &mai, nullptr, &mem), "vkAllocateMemory(image)")) return false;
    if (!check(f.vkBindImageMemory(gpu.device, img, mem, 0), "vkBindImageMemory")) return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, layers};
    return check(f.vkCreateImageView(gpu.device, &vi, nullptr, &view), "vkCreateImageView");
}

void imageBarrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t layers, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    f.vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Both texture arrays change layout together.
void texBarriers(VkCommandBuffer cb, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags srcStage,
                 VkPipelineStageFlags dstStage) {
    imageBarrier(cb, texImg, VK_IMAGE_ASPECT_COLOR_BIT, kBigLayers, from, to, src, dst, srcStage, dstStage);
    imageBarrier(cb, texImgS, VK_IMAGE_ASPECT_COLOR_BIT, kSmallLayers, from, to, src, dst, srcStage, dstStage);
}

// ---- Vulkan entry points -------------------------------------------------------------------------------
bool loadFunctions() {
#ifdef NFS_PS5
    auto gipa = vk_icdGetInstanceProcAddr;
    auto getInst = [&](const char *n) { return gipa(gpu.instance, n); };
#else
    static void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        say("libvulkan.so.1 not found");
        return false;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    auto getInst = [&](const char *n) { return gipa(gpu.instance, n); };
#endif
    f.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInst("vkGetDeviceProcAddr"));
    f.vkGetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getInst("vkGetPhysicalDeviceMemoryProperties"));
    if (!f.vkGetDeviceProcAddr || !f.vkGetPhysicalDeviceMemoryProperties) return false;
    bool ok = true;
#define LOAD(n) \
    if (!f.n) { \
        f.n = reinterpret_cast<PFN_##n>(f.vkGetDeviceProcAddr(gpu.device, #n)); \
        if (!f.n) { say("missing " #n); ok = false; } \
    }
    VK_FUNCS(LOAD)
#undef LOAD
    return ok;
}

bool acquireGpu() {
#ifdef NFS_PS5
    // The presenter owns the device; it only exists after its first frame, so show a black one now.
    static std::vector<uint32_t> black(size_t(outW) * outH, 0);
    ut_ps5_present(black.data(), outW, outH, outW * 4);
    MhVulkanDevice d{};
    if (!ut_ps5_vulkan_device(&d)) {
        say("presenter has no Vulkan device");
        return false;
    }
    gpu.instance = d.instance;
    gpu.physical = d.physical;
    gpu.device = d.device;
    gpu.queue = d.queue;
    gpu.family = d.family;
    return true;
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        say("libvulkan.so.1 not found");
        return false;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (!check(createInstance(&ici, nullptr, &gpu.instance), "vkCreateInstance")) return false;
    auto enumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(gpu.instance, "vkEnumeratePhysicalDevices"));
    auto qfProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(gpu.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(gpu.instance, "vkCreateDevice"));
    auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(gipa(gpu.instance, "vkGetDeviceQueue"));
    uint32_t n = 1;
    if (enumPhys(gpu.instance, &n, &gpu.physical) < 0 || n == 0) {
        say("no Vulkan device");
        return false;
    }
    uint32_t qn = 8;
    VkQueueFamilyProperties qp[8];
    qfProps(gpu.physical, &qn, qp);
    gpu.family = 0xffffffffu;
    for (uint32_t i = 0; i < qn; ++i)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gpu.family = i; break; }
    if (gpu.family == 0xffffffffu) return false;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dq.queueFamilyIndex = gpu.family;
    dq.queueCount = 1;
    dq.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dq;
    if (!check(createDevice(gpu.physical, &dci, nullptr, &gpu.device), "vkCreateDevice")) return false;
    getQueue(gpu.device, gpu.family, 0, &gpu.queue);
    return true;
#endif
}

VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (!check(f.vkCreateShaderModule(gpu.device, &ci, nullptr, &m), "vkCreateShaderModule")) shaderError = 1;
    return m;
}

bool setup() {
    const char *s = getenv("NFS_SCALE");
    kScale = s ? std::max(1, std::min(4, atoi(s))) : 2;
    outW = kGlideW * kScale;
    outH = kGlideH * kScale;
    if (!acquireGpu() || !loadFunctions()) {
        contextError = 1;
        return false;
    }
    f.vkGetPhysicalDeviceMemoryProperties(gpu.physical, &memProps);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gpu.family;
    if (!check(f.vkCreateCommandPool(gpu.device, &pci, nullptr, &pool), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (!check(f.vkAllocateCommandBuffers(gpu.device, &cai, &cbUpload), "cmdbuf") ||
        !check(f.vkAllocateCommandBuffers(gpu.device, &cai, &cbMain), "cmdbuf"))
        return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(f.vkCreateFence(gpu.device, &fci, nullptr, &fence), "vkCreateFence")) return false;

    if (!makeImage(VK_FORMAT_B8G8R8A8_UNORM, outW, outH, 1,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, colorImg, colorView) ||
        !makeImage(VK_FORMAT_D32_SFLOAT, outW, outH, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT, depthImg, depthView) ||
        !makeImage(VK_FORMAT_R8G8B8A8_UNORM, kLayerSize, kLayerSize, kBigLayers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, texImg, texView) ||
        !makeImage(VK_FORMAT_R8G8B8A8_UNORM, kSmallSize, kSmallSize, kSmallLayers, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, texImgS, texViewS)) {
        framebufferError = 1;
        return false;
    }
    if (!makeBuffer(sizeof(Vtx) * VkDeviceSize(kMaxVertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false, vtxBuf, vtxMap) ||
        !makeBuffer(kStagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, stagingBuf, stagingMap) ||
        !makeBuffer(VkDeviceSize(outW) * outH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, readBuf, readMap))
        return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (!check(f.vkCreateSampler(gpu.device, &sci, nullptr, &sampler), "vkCreateSampler")) return false;

    // render pass: one colour + one depth attachment, contents kept between passes (partial clears are legal in Glide)
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1] = dep[0];
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = dep[0].dstStageMask;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = dep[0].dstAccessMask;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 2;
    rpci.pDependencies = dep;
    if (!check(f.vkCreateRenderPass(gpu.device, &rpci, nullptr, &pass), "vkCreateRenderPass")) return false;
    VkImageView views[2] = {colorView, depthView};
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = pass;
    fbci.attachmentCount = 2;
    fbci.pAttachments = views;
    fbci.width = outW;
    fbci.height = outH;
    fbci.layers = 1;
    if (!check(f.vkCreateFramebuffer(gpu.device, &fbci, nullptr, &fb), "vkCreateFramebuffer")) return false;

    // descriptors + pipelines
    VkDescriptorSetLayoutBinding bind[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        bind[b].binding = b;
        bind[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind[b].descriptorCount = 1;
        bind[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bind;
    if (!check(f.vkCreateDescriptorSetLayout(gpu.device, &dlci, nullptr, &dsl), "vkCreateDescriptorSetLayout")) return false;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (!check(f.vkCreateDescriptorPool(gpu.device, &dpci, nullptr, &dpool), "vkCreateDescriptorPool")) return false;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    if (!check(f.vkAllocateDescriptorSets(gpu.device, &dai, &dset), "vkAllocateDescriptorSets")) return false;
    VkDescriptorImageInfo dii[2] = {{sampler, texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                    {sampler, texViewS, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wds[2]{};
    for (uint32_t b = 0; b < 2; ++b) {
        wds[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[b].dstSet = dset;
        wds[b].dstBinding = b;
        wds[b].descriptorCount = 1;
        wds[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds[b].pImageInfo = &dii[b];
    }
    f.vkUpdateDescriptorSets(gpu.device, 2, wds, 0, nullptr);

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 40};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (!check(f.vkCreatePipelineLayout(gpu.device, &plci, nullptr, &playout), "vkCreatePipelineLayout")) return false;

    VkShaderModule vs = shader(spv_glide_vert, sizeof spv_glide_vert), fs = shader(spv_glide_frag, sizeof spv_glide_frag);
    if (!vs || !fs) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};
    VkVertexInputBindingDescription vb{0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[4] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                               {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                               {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 24},
                                               {3, 0, VK_FORMAT_R8_UNORM, 28}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    for (int i = 0; i < 4; ++i) {
        const bool dstOne = i & 1, dwrite = i & 2;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = dwrite ? VK_TRUE : VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = dstOne ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &dss;
        gpci.pColorBlendState = &cb;
        gpci.pDynamicState = &ds;
        gpci.layout = playout;
        gpci.renderPass = pass;
        if (!check(f.vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipes[i]), "vkCreateGraphicsPipelines")) {
            shaderError = 1;
            return false;
        }
    }

    // one-time: put every image into the layout the frame loop expects
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(cbMain, &bi);
    imageBarrier(cbMain, colorImg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(cbMain, colorImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &cr);
    imageBarrier(cbMain, colorImg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    imageBarrier(cbMain, depthImg, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearDepthStencilValue dz{1.0f, 0};
    VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    f.vkCmdClearDepthStencilImage(cbMain, depthImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dz, 1, &dr);
    imageBarrier(cbMain, depthImg, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    texBarriers(cbMain, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    f.vkEndCommandBuffer(cbMain);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cbMain;
    if (!check(f.vkQueueSubmit(gpu.queue, 1, &si, fence), "initial submit")) return false;
    f.vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
    f.vkResetFences(gpu.device, 1, &fence);

    // fresh renderer: no texture owns a layer yet (zero-initialised entries would claim layer 0)
    freeBig.clear();
    freeSmall.clear();
    pendingLayers.clear();
    liveTex.clear();
    for (auto &t : textures) { t.layer = -1; t.data = nullptr; t.palHash = 0; t.size = 0; t.stored = 0; }
    for (int32_t i = int32_t(kBigLayers) - 1; i >= 0; --i) freeBig.push_back(i);
    for (int32_t i = int32_t(kSmallLayers) - 1; i >= 0; --i) freeSmall.push_back(int32_t(kBigLayers) + i);
    clipRect = {{0, 0}, {uint32_t(outW), uint32_t(outH)}};
    say("ready: %dx%d (x%d), %u big + %u small texture layers", outW, outH, kScale, kBigLayers, kSmallLayers);
    return true;
}

// ---- command recording ---------------------------------------------------------------------------------
void beginUpload() {
    if (upRecording) return;
    f.vkResetCommandBuffer(cbUpload, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(cbUpload, &bi);
    texBarriers(cbUpload, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    upRecording = true;
}

void beginMain() {
    if (mainRecording) return;
    f.vkResetCommandBuffer(cbMain, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f.vkBeginCommandBuffer(cbMain, &bi);
    mainRecording = true;
}

void ensurePass() {
    beginMain();
    if (passOpen) return;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass;
    rp.framebuffer = fb;
    rp.renderArea = {{0, 0}, {uint32_t(outW), uint32_t(outH)}};
    f.vkCmdBeginRenderPass(cbMain, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, float(outW), float(outH), 0.0f, 1.0f};
    f.vkCmdSetViewport(cbMain, 0, 1, &vp);
    VkDeviceSize off = 0;
    f.vkCmdBindVertexBuffers(cbMain, 0, 1, &vtxBuf, &off);
    passOpen = true;
    boundPipe = -1;
    scissorDirty = true;
}

void drawTriangles() {
    const uint32_t n = vtxCount - drawFirst;
    if (!n) return;
    ensurePass();
    const int idx = (blendDst == GR_BLEND_ONE ? 1 : 0) | (depthMask ? 2 : 0);
    if (idx != boundPipe) {
        f.vkCmdBindPipeline(cbMain, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[idx]);
        f.vkCmdBindDescriptorSets(cbMain, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset, 0, nullptr);
        boundPipe = idx;
    }
    if (scissorDirty) {
        f.vkCmdSetScissor(cbMain, 0, 1, &clipRect);
        scissorDirty = false;
    }
    struct PC { float fog[4]; float tex, fogOn, layer, size, big, wrap; } pc = {
        {fogColor[0], fogColor[1], fogColor[2], 1.0f}, texEnabled ? 1.0f : 0.0f, fogEnabled ? 1.0f : 0.0f, float(curLayer), float(curSize), curBig ? 1.0f : 0.0f, float((wrapS ? 1 : 0) + (wrapT ? 2 : 0))};
    f.vkCmdPushConstants(cbMain, playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
#ifndef NFS_PS5
    {
        static long traceFrame = getenv("NFS_TRACE_FRAME") ? atol(getenv("NFS_TRACE_FRAME")) : -1;
        if (traceFrame >= 0 && long(frameCount) == traceFrame) {
            const Vtx *a = reinterpret_cast<const Vtx *>(vtxMap) + drawFirst, *b = a + n - 1;
            const LayerInfo &li = layerInfo[curBig ? curLayer : curLayer + int(kBigLayers)];
            { float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f; for (const Vtx *v = a; v <= b; ++v) { x0 = v->x < x0 ? v->x : x0; x1 = v->x > x1 ? v->x : x1; y0 = v->y < y0 ? v->y : y0; y1 = v->y > y1 ? v->y : y1; }
              fprintf(stderr, "BOX %.0f %.0f %.0f %.0f ", double(x0), double(y0), double(x1), double(y1));
              fprintf(stderr, "VT"); for (const Vtx *v = a; v <= b && v < a + 6; ++v) fprintf(stderr, " (%.0f,%.0f s%.3f t%.3f q%.4f | tmu1 %.3f %.3f)", double(v->x), double(v->y), double(v->q != 0 ? v->s / v->q : 0), double(v->q != 0 ? v->t / v->q : 0), double(v->q), double(dbgT1[size_t(v - reinterpret_cast<const Vtx *>(vtxMap)) * 2]), double(dbgT1[size_t(v - reinterpret_cast<const Vtx *>(vtxMap)) * 2 + 1])); fprintf(stderr, " | "); }
            fprintf(stderr, "TRACE fmt=%d avg=%u,%u,%u,%u a=%u..%u n=%u blend=%s tex=%d big=%d layer=%d size=%.0f depthfn=%d depthmask=%d chroma=%d wrap=%d fog=%d first rgba=%u,%u,%u,%u z=%.4f last rgba=%u,%u,%u,%u z=%.4f\n",
                    li.fmt, li.avg[0], li.avg[1], li.avg[2], li.avg[3], li.minA, li.maxA, n / 3, blendDst == GR_BLEND_ONE ? "ADD" : "ALPHA", int(texEnabled), int(curBig), curLayer, double(curSize), dbgDepthFunc, int(depthMask), dbgChroma, (wrapS ? 1 : 0) + (wrapT ? 2 : 0),
                    int(fogEnabled), a->col[0], a->col[1], a->col[2], a->col[3], a->z, b->col[0], b->col[1], b->col[2], b->col[3], b->z);
        }
    }
#endif
    f.vkCmdDraw(cbMain, n, 1, drawFirst, 0);
    statDrawCalls++; statTriangles += n / 3;
    drawFirst = vtxCount;
}

void dumpFrame() {
    const char *dir = getenv("NFS_DUMP");
    if (!dir) return;
    static int every = getenv("NFS_DUMP_EVERY") ? atoi(getenv("NFS_DUMP_EVERY")) : 30;
    if (frameCount % unsigned(every)) return;
    char path[512];
    snprintf(path, sizeof path, "%s/frame%05lu.ppm", dir, frameCount);
    if (FILE *fp = fopen(path, "wb")) {
        fprintf(fp, "P6\n%d %d\n255\n", outW, outH);
        std::vector<uint8_t> rgb(size_t(outW) * outH * 3);
        for (size_t i = 0; i < size_t(outW) * outH; ++i) {
            rgb[i * 3 + 0] = readMap[i * 4 + 2];
            rgb[i * 3 + 1] = readMap[i * 4 + 1];
            rgb[i * 3 + 2] = readMap[i * 4 + 0];
        }
        fwrite(rgb.data(), 1, rgb.size(), fp);
        fclose(fp);
    }
}

// Submit everything recorded so far and wait for it. With `present`, the colour image is read back and shown.
void flushFrame(bool present) {
    if (!ready) return;
    drawTriangles();
    if (passOpen) {
        f.vkCmdEndRenderPass(cbMain);
        passOpen = false;
    }
    const bool haveWork = upRecording || mainRecording;
    if (present) {
        beginMain();
        imageBarrier(cbMain, colorImg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {uint32_t(outW), uint32_t(outH), 1};
        f.vkCmdCopyImageToBuffer(cbMain, colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuf, 1, &copy);
        imageBarrier(cbMain, colorImg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    if (upRecording || mainRecording) {
        VkCommandBuffer cbs[2];
        uint32_t n = 0;
        if (upRecording) {
            texBarriers(cbUpload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            f.vkEndCommandBuffer(cbUpload);
            cbs[n++] = cbUpload;
        }
        if (mainRecording) {
            f.vkEndCommandBuffer(cbMain);
            cbs[n++] = cbMain;
        }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = n;
        si.pCommandBuffers = cbs;
        if (check(f.vkQueueSubmit(gpu.queue, 1, &si, fence), "vkQueueSubmit")) {
            f.vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, UINT64_MAX);
            f.vkResetFences(gpu.device, 1, &fence);
        }
        upRecording = mainRecording = false;
    }
    (void)haveWork;
    for (int32_t l : pendingLayers) (l < int32_t(kBigLayers) ? freeBig : freeSmall).push_back(l);
    pendingLayers.clear();
    stagingUsed = 0;
    vtxCount = drawFirst = 0;
    scissorDirty = true;
    boundPipe = -1;

    if (present) {
        ++frameCount;
        dumpFrame();
        if (frameCount % 300 == 1) {
#ifdef NFS_PS5
            unsigned long b, u, z; lowmem_stats(&b, &u, &z);
            say("frame %lu: low memory %lu of %lu MB used, %lu failed allocations", frameCount, u >> 20, z >> 20, lowmem_failures());
#endif
            say("frame %lu: %u draw calls, %u triangles (since last report), layers free %zu pending %zu live textures %zu, overlap-retired %u, fallback-layer draws %u, uploads skipped %u",
                frameCount, statDrawCalls, statTriangles, freeBig.size() + freeSmall.size(), pendingLayers.size(), liveTex.size(), statOverlapRetired, statLayerFallback, statUploadSkipped);
            statDrawCalls = statTriangles = 0;
            say("  vertices with second-TMU coordinates: %u", statTmu1Verts);
            for (const auto &kv : callStats) say("  call %s x%u", kv.first.c_str(), kv.second);
            for (const auto &kv : texStats)
                say("  tex fmt %#x lod %u aspect %u: %u downloads, %u sources, %u uploads", kv.first >> 16, (kv.first >> 8) & 255, kv.first & 255, kv.second.downloads, kv.second.sources, kv.second.uploads);
        }
#ifdef NFS_PS5
        if (gammaValue != 1.0f) {
            uint8_t lut[256];
            for (int i = 0; i < 256; ++i) lut[i] = uint8_t(std::min(255.0f, 255.0f * powf(i / 255.0f, 1.0f / gammaValue) + 0.5f));
            uint8_t *p = readMap;
            for (size_t i = 0, e = size_t(outW) * outH; i < e; ++i, p += 4) {
                p[0] = lut[p[0]];
                p[1] = lut[p[1]];
                p[2] = lut[p[2]];
            }
        }
        ut_ps5_present(readMap, outW, outH, outW * 4);
#else
        // host test: show the frame in the game window (no GL window exists, so the window surface is free)
        if (SDL_Window *win = sdlWin) {
            if (SDL_Surface *dst = SDL_GetWindowSurface(win)) {
                SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(readMap, outW, outH, 32, outW * 4, SDL_PIXELFORMAT_ARGB8888);
                if (src) { SDL_BlitScaled(src, nullptr, dst, nullptr); SDL_FreeSurface(src); }
                SDL_UpdateWindowSurface(win);
            }
        }
#endif
    }
}

// ---- textures ------------------------------------------------------------------------------------------
// Queue an upload of `size`x`size` RGBA8 texels as a fresh layer for `ti` (the old layer is released once the frame is done).
void uploadTexels(TextureInfo *ti, const uint32_t *texels) {
    if (!ready) return;
    const uint32_t size = ti->size;
    if (size == 0 || size > kLayerSize) return;
    ti->stored = size;
    const bool small = size <= kSmallSize;
    std::vector<int32_t> &freeList = small ? freeSmall : freeBig;
    const VkDeviceSize bytes = VkDeviceSize(size) * size * 4;
    // an overflow flush frees pending layers, so decide the new layer before retiring the old one
    if (stagingUsed + bytes > kStagingSize || freeList.empty()) flushFrame(false);
    if (freeList.empty()) { static int warned; if (warned++ < 5) say("out of %s texture layers", small ? "small" : "big"); statUploadSkipped++; return; }
    const int32_t layer = freeList.back();
    freeList.pop_back();
    texStats[statKey(ti->fmt, ti->size == 256 ? 0 : ti->size == 128 ? 1 : ti->size == 64 ? 2 : ti->size == 32 ? 3 : ti->size == 16 ? 4 : 5, 255)].uploads++;
    if (ti->layer >= 0) pendingLayers.push_back(ti->layer);
    ti->layer = layer;
    memcpy(stagingMap + stagingUsed, texels, bytes);
#ifndef NFS_PS5
    if (const char *td = getenv("NFS_TEXDUMP")) {   // host: every upload as raw RGBA, newest per layer
        char pth[512]; snprintf(pth, sizeof pth, "%s/L%d_s%u_f%d.rgba", td, layer, size, ti->fmt);
        if (FILE *fp = fopen(pth, "wb")) { fwrite(texels, 1, bytes, fp); fclose(fp); }
    }
#endif
    {
        LayerInfo &li = layerInfo[layer];
        li.fmt = ti->fmt; li.minA = 255; li.maxA = 0;
        unsigned long sum[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < size * size; ++i) {
            const uint8_t *px = reinterpret_cast<const uint8_t *>(&texels[i]);
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
            if (px[3] < li.minA) li.minA = px[3];
            if (px[3] > li.maxA) li.maxA = px[3];
        }
        for (int c = 0; c < 4; ++c) li.avg[c] = unsigned(sum[c] / (size * size));
    }
    beginUpload();
    VkBufferImageCopy copy{};
    copy.bufferOffset = stagingUsed;
    copy.bufferRowLength = size;
    copy.bufferImageHeight = size;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(small ? layer - int32_t(kBigLayers) : layer), 1};
    copy.imageExtent = {size, size, 1};
    f.vkCmdCopyBufferToImage(cbUpload, stagingBuf, small ? texImgS : texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    stagingUsed += (bytes + 255) & ~VkDeviceSize(255);
}

inline uint8_t e5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
inline uint8_t e6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }

void convertAndUpload(TextureInfo *ti) {
    static uint32_t tmp[256 * 256];
    const uint32_t n = ti->size * ti->size;
    const uint16_t *src = reinterpret_cast<const uint16_t *>(ti->data);
    switch (ti->fmt) {
        case GR_TEXFMT_RGB_565:
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = src[i];
                tmp[i] = e5(v >> 11) | (e6((v >> 5) & 63) << 8) | (e5(v & 31) << 16) | 0xff000000u;
            }
            break;
        case GR_TEXFMT_ARGB_1555:
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = src[i];
                tmp[i] = e5((v >> 10) & 31) | (e5((v >> 5) & 31) << 8) | (e5(v & 31) << 16) | ((v & 0x8000) ? 0xff000000u : 0u);
            }
            break;
        case GR_TEXFMT_ARGB_4444:
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t v = src[i];
                tmp[i] = (((v >> 8) & 15) * 17) | ((((v >> 4) & 15) * 17) << 8) | ((v & 15) * 17u << 16) | ((((v >> 12) & 15) * 17u) << 24);
            }
            break;
        default:
            return;
    }
    uploadTexels(ti, tmp);
}

}  // namespace

// =====================================================================================================
// Glide API (the game calls these with pointer-sized arguments)
// =====================================================================================================
extern "C" {

void grAlphaBlendFunction(intptr_t rgb_sf, intptr_t rgb_df, intptr_t alpha_sf, intptr_t alpha_df) {
    logCall("grAlphaBlendFunction(%d,%d,%d,%d)", int(rgb_sf), int(rgb_df), int(alpha_sf), int(alpha_df));
    drawTriangles();
    if (int32_t(rgb_df) == GR_BLEND_ONE || int32_t(rgb_df) == GR_BLEND_ONE_MINUS_SRC_ALPHA) blendDst = int32_t(rgb_df);
}
void grAlphaCombine(intptr_t function, intptr_t factor, intptr_t local, intptr_t other, intptr_t invert) {
    logCall("grAlphaCombine(fn %d,factor %d,local %d,other %d,inv %d)", int(function), int(factor), int(local), int(other), int(invert));
    drawTriangles();
    texEnabled = int32_t(other) == GR_COMBINE_OTHER_TEXTURE;
}
void grAlphaTestFunction(intptr_t f) { logCall("grAlphaTestFunction(%d)", int(f)); }
void grAlphaTestReferenceValue(intptr_t) {}
void grClipWindow(intptr_t minX, intptr_t minY, intptr_t maxX, intptr_t maxY) {
    drawTriangles();
    int32_t x0 = int32_t(uint32_t(minX)) * kScale, y0 = int32_t(uint32_t(minY)) * kScale;
    int32_t x1 = int32_t(uint32_t(maxX)) * kScale, y1 = int32_t(uint32_t(maxY)) * kScale;
    x0 = std::max(0, std::min(outW, x0));
    x1 = std::max(0, std::min(outW, x1));
    y0 = std::max(0, std::min(outH, y0));
    y1 = std::max(0, std::min(outH, y1));
    clipRect = {{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
    scissorDirty = true;
}
void grBufferClear(intptr_t color, intptr_t alpha, intptr_t depth) {
    if (!ready) return;
    drawTriangles();
    ensurePass();
    const uint32_t c = uint32_t(color);
    VkClearAttachment ca[2]{};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].colorAttachment = 0;
    ca[0].clearValue.color.float32[0] = ((c >> 16) & 0xff) / 255.0f;
    ca[0].clearValue.color.float32[1] = ((c >> 8) & 0xff) / 255.0f;
    ca[0].clearValue.color.float32[2] = (c & 0xff) / 255.0f;
    ca[0].clearValue.color.float32[3] = (uint8_t(alpha)) / 255.0f;
    ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca[1].clearValue.depthStencil = {(uint16_t(depth)) / 65535.0f, 0};
    VkClearRect rect{clipRect, 0, 1};
    if (rect.rect.extent.width && rect.rect.extent.height) f.vkCmdClearAttachments(cbMain, 2, ca, 1, &rect);
}
void grChromakeyMode(intptr_t m) { dbgChroma = int(m); logCall("grChromakeyMode(%d)", int(m)); }
void grChromakeyValue(intptr_t) {}
void grBufferSwap(intptr_t) { flushFrame(true); }
void grColorCombine(intptr_t a, intptr_t b, intptr_t c, intptr_t d, intptr_t e) { logCall("grColorCombine(fn %d,factor %d,local %d,other %d,inv %d)", int(a), int(b), int(c), int(d), int(e)); }
void grCullMode(intptr_t m) { logCall("grCullMode(%d)", int(m)); }
void grDepthBiasLevel(intptr_t l) { logCall("grDepthBiasLevel(%d)", int(int16_t(l))); }
void grDepthBufferFunction(intptr_t f) { dbgDepthFunc = int(f); logCall("grDepthBufferFunction(%d)", int(f)); }
void grDepthBufferMode(intptr_t m) { logCall("grDepthBufferMode(%d)", int(m)); }
void grDepthMask(intptr_t mask) {
    drawTriangles();
    depthMask = int32_t(mask) != 0;
}
void grDitherMode(intptr_t) {}

static inline uint8_t byteOf(float v) { return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : uint8_t(v)); }

static void pushVertex(const GrVertex *v) {
#ifndef NFS_PS5
    if (dbgT1.size() < size_t(vtxCount + 1) * 2) dbgT1.resize((size_t(vtxCount) + 1) * 2 + 4096);
    dbgT1[size_t(vtxCount) * 2] = v->tmuvtx[1].sow / (v->oow != 0.0f ? v->oow : 1.0f) / 256.0f; dbgT1[size_t(vtxCount) * 2 + 1] = v->tmuvtx[1].tow / (v->oow != 0.0f ? v->oow : 1.0f) / 256.0f;
#endif
    Vtx &o = *reinterpret_cast<Vtx *>(vtxMap + size_t(vtxCount) * sizeof(Vtx));
    o.x = v->x - float(kVertexSnap);
    o.y = v->y - float(kVertexSnap);
    o.z = v->oow;
    o.s = v->tmuvtx[0].sow / 256.0f;
    o.t = v->tmuvtx[0].tow / 256.0f;
    o.q = v->oow;
    if (v->tmuvtx[1].sow != 0.0f || v->tmuvtx[1].tow != 0.0f) ++statTmu1Verts;
    o.col[0] = byteOf(v->r);
    o.col[1] = byteOf(v->g);
    o.col[2] = byteOf(v->b);
    o.col[3] = byteOf(v->a);
    const float w = v->oow > 0.0f ? 1.0f / v->oow : 65535.0f;
    o.fog = uint8_t(255 - fogTable[uint16_t(w > 65535.0f ? 65535.0f : w)]);
    o.pad[0] = o.pad[1] = o.pad[2] = 0;
    ++vtxCount;
}

void grDrawTriangle(intptr_t a, intptr_t b, intptr_t c) {
    if (!ready) return;
    if (vtxCount + 3 > kMaxVertices) flushFrame(false);
    pushVertex(reinterpret_cast<const GrVertex *>(uintptr_t(uint32_t(a))));
    pushVertex(reinterpret_cast<const GrVertex *>(uintptr_t(uint32_t(b))));
    pushVertex(reinterpret_cast<const GrVertex *>(uintptr_t(uint32_t(c))));
}
void grDrawLine(intptr_t a, intptr_t b) {
    if (!ready) return;
    if (vtxCount + 6 > kMaxVertices) flushFrame(false);
    const GrVertex *p = reinterpret_cast<const GrVertex *>(uintptr_t(uint32_t(a)));
    const GrVertex *q = reinterpret_cast<const GrVertex *>(uintptr_t(uint32_t(b)));
    // a two-pixel wide quad
    float dx = q->x - p->x, dy = q->y - p->y;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float nx = -dy / len, ny = dx / len;  // half width 1px
    GrVertex v[4] = {*p, *p, *q, *q};
    v[0].x += nx; v[0].y += ny;
    v[1].x -= nx; v[1].y -= ny;
    v[2].x += nx; v[2].y += ny;
    v[3].x -= nx; v[3].y -= ny;
    const bool savedTex = texEnabled;
    drawTriangles();
    texEnabled = false;
    pushVertex(&v[0]); pushVertex(&v[1]); pushVertex(&v[2]);
    pushVertex(&v[2]); pushVertex(&v[1]); pushVertex(&v[3]);
    drawTriangles();
    texEnabled = savedTex;
}
void grFogColorValue(intptr_t color) {
    drawTriangles();
    const uint32_t c = uint32_t(color);
    fogColor[0] = ((c >> 16) & 0xff) / 255.0f;
    fogColor[1] = ((c >> 8) & 0xff) / 255.0f;
    fogColor[2] = (c & 0xff) / 255.0f;
}
void grFogMode(intptr_t mode) {
    drawTriangles();
    if (int32_t(mode) == GR_FOG_DISABLE) fogEnabled = false;
    else if (int32_t(mode) == GR_FOG_WITH_TABLE) fogEnabled = true;
}
void grGammaCorrectionValue(float value) { gammaValue = value > 0.0f ? value : 1.0f; }
void grGlideInit(void) {}
void grGlideShutdown(void) {
    if (ready) {
        flushFrame(false);
        f.vkDeviceWaitIdle(gpu.device);
    }
    palette = nullptr; srcTi = nullptr; srcSize = 0; palCopyHash = 0;
}
int32_t grLfbLock(intptr_t type, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t info_) {
    GrLfbInfo *info = reinterpret_cast<GrLfbInfo *>(uintptr_t(uint32_t(info_)));
    memset(info, 0, sizeof *info);
    if (uint32_t(type) == GR_LFB_WRITE_ONLY) {
        lfb = static_cast<uint8_t *>(malloc(640 * 480 * 2));
        info->lfbPtr = uint32_t(uintptr_t(lfb));
        info->strideInBytes = 2;
        return 1;
    }
    return 0;
}
int32_t grLfbUnlock(intptr_t, intptr_t) {
    free(lfb);
    lfb = nullptr;
    return 1;
}
void grRenderBuffer(intptr_t) {}
void grSstIdle(void) {}
int32_t grSstIsBusy(void) { return 0; }
int32_t grSstQueryHardware(intptr_t) { return 1; }
void grSstSelect(intptr_t) {}
uint32_t grSstStatus(void) { return 0x0FFFF03F; }
void grSstWinClose(void) {}
int32_t grSstWinOpen(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t) {
    if (!ready) {
        ready = setup();
        if (!ready) {
            say("Vulkan setup failed");
            raise(SIGABRT);
        }
    }
    grClipWindow(0, 0, 640, 480);
    return 1;
}
uint32_t grTexCalcMemRequired(intptr_t, intptr_t lodmax, intptr_t, intptr_t fmt) {
    uint32_t size = 256 >> int32_t(lodmax);
    size *= size;
    if (int32_t(fmt) != GR_TEXFMT_P_8) size <<= 1;
    return size;
}
void grTexClampMode(intptr_t, intptr_t sClamp, intptr_t tClamp) {
    logCall("grTexClampMode(s %d,t %d)", int(sClamp), int(tClamp));
    drawTriangles();
    wrapS = int32_t(sClamp) == GR_TEXTURECLAMP_WRAP;
    wrapT = int32_t(tClamp) == GR_TEXTURECLAMP_WRAP;
}
void grTexCombine(intptr_t tmu, intptr_t a, intptr_t b, intptr_t c, intptr_t d, intptr_t e, intptr_t f) { logCall("grTexCombine(tmu %d,rgb %d/%d,alpha %d/%d,inv %d/%d)", int(tmu), int(a), int(b), int(c), int(d), int(e), int(f)); }
void grTexCombineFunction(intptr_t tmu, intptr_t f) { logCall("grTexCombineFunction(tmu %d,fn %d)", int(tmu), int(f)); }
void grTexDownloadMipMap(intptr_t, intptr_t startAddress_, intptr_t, intptr_t info_) {
    const uint32_t startAddress = uint32_t(startAddress_) & (kTextureMem - 1);
    const GrTexInfo *info = reinterpret_cast<const GrTexInfo *>(uintptr_t(uint32_t(info_)));
    TextureInfo *ti = &textures[startAddress >> 2];
    texStats[statKey(info->format, info->largeLod, info->aspectRatio)].downloads++;
    const uint32_t newSize = 256 >> info->largeLod;
    const uint32_t bytes = newSize * newSize * (info->format == GR_TEXFMT_P_8 ? 1 : 2);
    if (startAddress + bytes > kTextureMem) return;
    drawTriangles();
    // Texture memory is overwritten: every texture it overlaps is gone, give its layer back.
    for (auto it = liveTex.begin(); it != liveTex.end();) {
        const uint32_t a = it->first, e = it->first + it->second;
        const bool overlap = a < startAddress + bytes && startAddress < e;
        if (!overlap) { ++it; continue; }
        TextureInfo *old = &textures[a >> 2];
        statOverlapRetired++;
        if (old->layer >= 0) { pendingLayers.push_back(old->layer); old->layer = -1; }
        old->palHash = 0;
        it = liveTex.erase(it);
    }
    liveTex[startAddress] = bytes;
    ti->data = &textureMem[startAddress];
    ti->palHash = 0;
    ti->fmt = info->format;
    ti->size = newSize;
    memcpy(ti->data, reinterpret_cast<const void *>(uintptr_t(info->data)), bytes);
    if (ti->fmt != GR_TEXFMT_P_8) convertAndUpload(ti);
}
// Glide copies the palette when it is downloaded and applies it when drawing: snapshot it here, and if the texture that is
// currently selected is paletted, re-colour it right away (the game may switch palettes without selecting the texture again).
static void recolour(TextureInfo *ti, uint32_t size) {
    static uint32_t tmp[256 * 256];
    const uint32_t n = size * size;
    ti->size = size;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = palCopy[ti->data[i]];
        tmp[i] = ((v >> 16) & 0x000000FF) | ((v << 16) & 0x00FF0000) | (v & 0xFF00FF00);
    }
    uploadTexels(ti, tmp);
    ti->palHash = palCopyHash;
}
void grTexDownloadTable(intptr_t, intptr_t type, intptr_t data) {
    if (uint32_t(type) != GR_TEXTABLE_PALETTE) return;
    drawTriangles();
    const uint32_t *src = reinterpret_cast<const uint32_t *>(uintptr_t(uint32_t(data)));
    uint32_t h = 2166136261u;
    for (int i = 0; i < 256; ++i) { palCopy[i] = src[i]; h = (h ^ src[i]) * 16777619u; }
    palCopyHash = h ? h : 1;
    palette = palCopy;
    if (srcTi && srcTi->data && srcTi->fmt == GR_TEXFMT_P_8 && srcTi->palHash != palCopyHash && srcSize) recolour(srcTi, srcSize);
}
void grTexFilterMode(intptr_t, intptr_t, intptr_t) {}
uint32_t grTexMaxAddress(intptr_t) { return kTextureMem; }
uint32_t grTexMinAddress(intptr_t) { return 0; }
void grTexMipMapMode(intptr_t, intptr_t, intptr_t) {}
void grTexSource(intptr_t tmu, intptr_t startAddress_, intptr_t, intptr_t info_) {
    if (int(tmu) != 0) logCall("grTexSource(tmu %d) fmt %d", int(tmu), int(reinterpret_cast<const GrTexInfo *>(uintptr_t(uint32_t(info_)))->format));
    const uint32_t startAddress = uint32_t(startAddress_) & (kTextureMem - 1);
    const GrTexInfo *info = reinterpret_cast<const GrTexInfo *>(uintptr_t(uint32_t(info_)));
    TextureInfo *ti = &textures[startAddress >> 2];
    texStats[statKey(info->format, info->largeLod, info->aspectRatio)].sources++;
    drawTriangles();
    srcTi = info->format == GR_TEXFMT_P_8 ? ti : nullptr;
    srcSize = 256u >> info->largeLod;
    if (info->format == GR_TEXFMT_P_8 && palette && (ti->palHash != palCopyHash || ti->layer < 0) && ti->data) recolour(ti, srcSize);
    if (ti->layer < 0) {
        static unsigned nolayer; ++nolayer;
        if (nolayer < 40 || nolayer % 500 == 0) say("source without layer #%u: addr %#x fmt %d size %u info fmt %d lod %d/%d aspect %d data %p", nolayer, startAddress, ti->fmt, ti->size, int(info->format), int(info->largeLod), int(info->smallLod), int(info->aspectRatio), (void *)ti->data);
    }
    if (ti->layer < 0) statLayerFallback++;
    const int32_t id = ti->layer >= 0 ? ti->layer : 0;
    curBig = id < int32_t(kBigLayers);
    curLayer = curBig ? id : id - int32_t(kBigLayers);
    curSize = ti->stored ? ti->stored : (ti->size ? std::min<uint32_t>(ti->size, kLayerSize) : kLayerSize);
}

// ---- fog tables (copied from the OpenGL version) --------------------------------------------------------
void grFogTable(intptr_t ft_) {
    const uint8_t *ft = reinterpret_cast<const uint8_t *>(uintptr_t(uint32_t(ft_)));
    static const uint32_t intStartEnd[GR_FOG_TABLE_SIZE + 1] = {
        1, 1, 1, 1, 2, 2, 2, 3, 4, 4, 5, 6, 8, 9, 11, 13, 16, 19, 22, 26, 32, 38, 45, 53,
        64, 76, 90, 107, 128, 152, 181, 215, 256, 304, 362, 430, 512, 608, 724, 861,
        1024, 1217, 1448, 1722, 2048, 2435, 2896, 3444, 4096, 4870, 5792, 6888, 8192,
        9741, 11585, 13777, 16384, 19483, 23170, 27554, 32768, 38967, 46340, 55108, 65536};
    static const uint32_t intEndMinusStart[GR_FOG_TABLE_SIZE] = {
        0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 2, 2, 3, 3, 3, 4, 6, 6, 7, 8, 11, 12, 14,
        17, 21, 24, 29, 34, 41, 48, 58, 68, 82, 96, 116, 137, 163, 193, 231, 274, 326,
        387, 461, 548, 652, 774, 922, 1096, 1304, 1549, 1844, 2192, 2607, 3099, 3687,
        4384, 5214, 6199, 7373, 8768, 10428};
    uint8_t table[GR_FOG_TABLE_SIZE + 1];
    memcpy(table, ft, GR_FOG_TABLE_SIZE);
    table[GR_FOG_TABLE_SIZE] = 255;
    for (int i = 0; i < GR_FOG_TABLE_SIZE; ++i)
        for (uint32_t j = intStartEnd[i]; j < intStartEnd[i + 1]; ++j)
            fogTable[j & 0xffff] = uint8_t(table[i] + (table[i + 1] - table[i]) * int32_t(j - intStartEnd[i]) / int32_t(intEndMinusStart[i] ? intEndMinusStart[i] : 1));
}
void guFogGenerateExp(intptr_t fogtable_, float density) {
    uint8_t *fogtable = reinterpret_cast<uint8_t *>(uintptr_t(uint32_t(fogtable_)));
    static const float tableIndexToW[GR_FOG_TABLE_SIZE] = {
        1.000000f, 1.142857f, 1.333333f, 1.600000f, 2.000000f, 2.285714f, 2.666667f, 3.200000f,
        4.000000f, 4.571429f, 5.333333f, 6.400000f, 8.000000f, 9.142858f, 10.666667f, 12.800000f,
        16.000000f, 18.285715f, 21.333334f, 25.600000f, 32.000000f, 36.571430f, 42.666668f, 51.200001f,
        64.000000f, 73.142860f, 85.333336f, 102.400002f, 128.000000f, 146.285721f, 170.666672f, 204.800003f,
        256.000000f, 292.571442f, 341.333344f, 409.600006f, 512.000000f, 585.142883f, 682.666687f, 819.200012f,
        1024.000000f, 1170.285767f, 1365.333374f, 1638.400024f, 2048.000000f, 2340.571533f, 2730.666748f, 3276.800049f,
        4096.000000f, 4681.143066f, 5461.333496f, 6553.600098f, 8192.000000f, 9362.286133f, 10922.666992f, 13107.200195f,
        16384.000000f, 18724.572266f, 21845.333984f, 26214.400391f, 32768.000000f, 37449.144531f, 43690.667969f, 52428.800781f};
    float dp = density * tableIndexToW[GR_FOG_TABLE_SIZE - 1];
    const float scale = 255.0f / (1.0f - powf(2.71828182845904523536f, -dp));
    for (int i = 0; i < GR_FOG_TABLE_SIZE; ++i) {
        dp = density * tableIndexToW[i];
        float v = (1.0f - powf(2.71828182845904523536f, -dp)) * scale;
        fogtable[i] = uint8_t(v > 255.0f ? 255.0f : (v < 0.0f ? 0.0f : v));
    }
}

}  // extern "C"
