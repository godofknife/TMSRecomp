// Vulkan plumbing for the NVN backend: loader, device, memory, formats.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "nvn_hle.h"

#include <atomic>
#include <chrono>
#include <map>

namespace vk {

#define VK_FUNCS(X)                                                                                              \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                            \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)          \
    X(vkGetDeviceQueue) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)                          \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)          \
    X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)               \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)         \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkCreateBuffer) X(vkDestroyBuffer)  \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkCreateImage)                   \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImage) X(vkDestroyImageView) X(vkCmdPipelineBarrier)             \
    X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage)            \
    X(vkCmdCopyImage) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments)     \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCreateQueryPool) X(vkResetQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkGetQueryPoolResults) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCreateShaderModule)      \
    X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorSetLayout)                        \
    X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkResetDescriptorPool)      \
    X(vkCreateSampler) X(vkGetMemoryHostPointerPropertiesEXT) X(vkCmdSetBlendConstants)                           \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkMapMemory)                          \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCmdSetCullMode) X(vkCmdSetFrontFace)                               \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable)                           \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdDispatch)                   \
    X(vkCreateComputePipelines) X(vkGetPhysicalDeviceFeatures2) X(vkCmdSetPrimitiveRestartEnable)                \
    X(vkCmdCopyImageToBuffer) X(vkCmdUpdateBuffer) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkMergePipelineCaches) X(vkDestroyPipelineCache) X(vkDestroyPipeline) X(vkDestroyShaderModule) X(vkDestroyPipelineLayout) X(vkDestroyDescriptorSetLayout) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue)

#define VK_DECL(f) extern PFN_##f f;
VK_FUNCS(VK_DECL)
#undef VK_DECL

extern VkDevice dev;
extern PFN_vkCmdSetCheckpointNV g_checkpoint;  // set when NX_CHECKPOINTS is on
extern VkPhysicalDevice phys;
extern VkQueue queue;
extern uint32_t qfam;
extern VkPhysicalDeviceProperties props;
extern bool g_vk_storage_ext_formats, g_vk_pipeline_stats, g_vk_pipe_cache_ctl;  // optional GPU features present (vk.cpp device creation)

void check(VkResult r, const char* what);
#define VKC(x) vk::check((x), #x)
VkFormat depth_format(VkFormat want);  // D24S8 / S8 -> D32_SFLOAT_S8_UINT when this GPU lacks them

// device-local suballocator (ponytail: bump-only in 256 MiB blocks, never freed; add a real allocator when texture churn shows)
VkDeviceMemory alloc(VkMemoryRequirements req, VkMemoryPropertyFlags flags, VkDeviceSize* offset);
void free_mem(VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size);  // back to alloc's free ranges
extern std::atomic<uint64_t> g_mem_blocks_bytes, g_mem_used_bytes;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // full view
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 0, h = 0, layers = 1, levels = 1;
    bool written = false;    // rendered to or uploaded at least once
    uint64_t wgen = 0;       // write generation (vk::mark_written): which of two upscaling twins holds the newer picture
    Image* twin = nullptr;   // upscaling: the other size of this screen-size target (hi <-> lo), nullptr while upscaling is off
    bool from_pool = false;  // contents came from guest memory
    float scale = 1;         // render targets: res_scale(), or the FSR 3 scene scale (lo twins); w, h are the scaled size
    float sx = 1;            // ultrawide: extra width factor of a widened 16:9 screen target (w = game width * sx * scale)
    uint32_t pool_bytes = 0;  // guest storage read by the upload
    uint64_t checked = 0;     // submit serial of the last write-watch check
    std::map<uint64_t, VkImageView> views;  // sampled views with a swizzle / view type (texture views)
    bool replaced = false;   // texture pack: image / view are the pack's (shared per hash), own ones in `own`
    uint64_t pack_wait = 0;  // texture pack: hash whose file a worker is reading (upload again once it is in)
    struct Own { VkImage image; VkImageView view; VkFormat format; uint32_t w, h, levels; VkImageLayout layout; std::map<uint64_t, VkImageView> views;
                 VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize mem_off = 0, mem_size = 0; };  // mem: pack images (new_image_2d)
    Own* own = nullptr;
    VkDeviceMemory mem = VK_NULL_HANDLE;  // image_of's allocation (the texture's own image), for free_mem
    VkDeviceSize mem_off = 0, mem_size = 0;
    uint64_t pack_hash = 0;  // replaced: the pack texture swapped in (its reference)
    int8_t pad_style = -1;  // the button atlas: icon style uploaded (rt::pad_icon_style), -1 other textures
    int8_t resist = -1;     // a resistance badge atlas: icons uploaded (rt::resist_icons_on), -1 other textures
    uint64_t resist_hash = 0;  // that atlas's level-0 hash (vk_replay.cpp draw: badge quads drawn untinted)
    bool pack_file = false;  // the UHD pack has a file for this texture's data (upload again when the pack is switched)
    uint8_t pack_gen = 0;    // pack switch count at that upload
    uint32_t share = 1;      // NVN textures on this image (depth + stencil objects of one surface, image_of)
};
Image::Own new_image_2d(VkFormat f, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags extra = 0);  // vk.cpp: texture packs
bool pack_arrived(Image* im);  // vk_texture.cpp: the pack file im->pack_wait waits for is read (upload again)
void pack_released(Image* im);  // vk_texture.cpp: a replaced image goes away (drops its pack reference)
// vk.cpp: destroy once the GPU is past every submit made so far (views, then image, then memory; any may be null)
void defer_destroy(VkImage image, VkImageView view, std::map<uint64_t, VkImageView> views, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size);
VkImageView sampled_view(nvn::Texture* t, Image* im, const nvn::TexView& v);  // vk.cpp
extern PFN_vkCmdPushDescriptorSetKHR g_push_desc;  // vk.cpp: VK_KHR_push_descriptor, null when the device lacks it
extern uint32_t g_max_push;  // its maxPushDescriptors
float res_scale();  // vk.cpp: NX_RES / NX_RES_SCALE, render target scale (1080p = 1.5)
// HDR output (vk.cpp, setting hdr): swapchain mode 0 SDR, 1 HDR10, 2 scRGB. vk_replay.cpp reports the game's copy of its
// float frame (scene + UI) into a window texture, and render passes into that window texture (they void the note).
extern std::atomic<int> g_hdr_mode;
void hdr_note_copy(uint32_t window_guest, Image* src);
void hdr_note_target(uint32_t guest);
float wide_factor();   // vk.cpp: setting aspect (NX_ASPECT) over 16:9: 1 = the game's 16:9, 1.3125 = 21:9 (1280 -> 1680)
uint64_t vram_bytes();  // vk.cpp: largest device-local heap (NX_VRAM_MB pretends a smaller GPU, for tests)
bool weak_gpu();        // vk.cpp: integrated GPU or under 4 GB of VRAM (weak-PC defaults: res auto, UHD pack auto)
bool upload_from_pool(VkCommandBuffer cb, nvn::Texture* t, Image* im);  // vk_texture.cpp
void check_pool_texture(nvn::Texture* t, Image* im);
bool has_texels(nvn::Texture* t);                                   // WriteTexels data waiting for the image
void apply_texels(VkCommandBuffer cb, nvn::Texture* t, Image* im);  // CPU rewrote it since upload: upload again at next use
extern uint64_t g_submit_serial, g_reuploads;
// per-frame time (us) of slow work on the game thread, for the hitch log (vk.cpp backend_present)
enum { HITCH_TRANSLATE, HITCH_PIPELINE, HITCH_UPLOAD, HITCH_SUBMIT, HITCH_GPU_WAIT, HITCH_IMAGE, HITCH_IMPORT, HITCH_CHECK, HITCH_OTHER_THREAD, HITCH_RENDER_WAIT, HITCH_SLOT_WAIT, HITCH_N };
extern std::atomic<uint32_t> g_hitch_us[HITCH_N];
struct HitchTimer {  // adds the scope's duration to g_hitch_us[k]
    int k;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~HitchTimer() { g_hitch_us[k] += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
};
void staging_reset(int slot);  // the submit slot whose upload ring is used next (its previous GPU work is done)
// GPU timeline: every queue submit signals the next serial. NVN syncs (nvn_hle.cpp) map to serials.
uint64_t gpu_last_submitted();
uint64_t gpu_recording_serial();  // serial the command buffer being recorded will get
bool gpu_wait(uint64_t serial, uint64_t timeout_ns = UINT64_MAX);  // false: timed out
Image* image_of(nvn::Texture* t);  // creates on first use
void transition(VkCommandBuffer cb, Image* im, VkImageLayout to);
extern std::atomic<uint64_t> g_wgen;
inline void mark_written(Image* im) { im->written = true; im->wgen = g_wgen.fetch_add(1, std::memory_order_relaxed) + 1; }

struct GuestBuffer { VkBuffer buf; VkDeviceSize offset; VkDeviceSize size = 0; };  // size: bytes left from offset
void stats_flush();  // drawlog: log the pipeline statistics of the draws just submitted (vk_replay.cpp)
GuestBuffer guest_buffer(uint64_t gpu, uint64_t need = 0);  // pool buffer + offset for a GPU address (pool memory imported zero-copy);
    // need: bytes the GPU reads from there (0: to the pool's end)
void imports_trim();  // once per frame: release guest-memory imports unused for a while (NX_IMPORT_KEEP)
bool gpu_is_amd();  // AMD GPU (vendor 0x1002): see vk_replay.cpp end_rendering (empty passes keep their barrier)

VkFormat vk_format(uint32_t nvn_format, bool* depth = nullptr, bool* stencil = nullptr);
// vk_fsr3.cpp: AMD FSR 3 through the FidelityFX API DLLs (run-time optional). All images GENERAL.
struct Fsr3Frame {
    VkCommandBuffer cb; Image* color; Image* depth; Image* mv; Image* out;  // depth: R32 float copy; mv: RG16 float, pixels
    float jx, jy, sharpness, dt_ms, cam_near, cam_far, fov_y;
    bool reset;
};
PFN_vkGetDeviceProcAddr device_proc_addr();  // vk.cpp
bool fsr3_ready(uint32_t w, uint32_t h);  // context for this output size made (loads the DLLs on first use); false: unavailable
void fsr3_jitter(uint64_t frame, uint32_t render_w, float* jx, float* jy);  // this frame's sub-pixel offset (render pixels)
bool fsr3_dispatch(const Fsr3Frame& f);
// vk_dlss.cpp: NVIDIA DLSS through the driver's NGX core (run-time optional, nothing of NVIDIA compiled in; NOTES s64)
VkInstance g_inst_handle();
PFN_vkGetInstanceProcAddr instance_proc_addr();
void dlss_instance_exts(std::vector<const char*>& out);
void dlss_device_exts(const std::vector<VkExtensionProperties>& have, std::vector<const char*>& out);
bool dlss_ready();  // NGX up and DLSS available (first call brings NGX up)
void dlss_jitter(uint64_t frame, float ratio, float* jx, float* jy);  // ratio: output / render size
bool dlss_dispatch(const Fsr3Frame& f);
// FSR 3 upscaling (vk.cpp, NOTES s63): screen-size render targets have a second, smaller image (lo twin); the frame's scene
// renders on the lo twins and the FXAA draw (FSR's place) switches every twin back to the full-size one.
float upscale_lo_scale();  // the scene scale over the game's 1280x720, 0: upscaling off (settings fx_fxaa + fsr3_quality, live)
void scene_phase(bool lo);  // every twin texture -> its lo / full-size image
bool scene_phase_lo();
Image* upscale_hi(Image* lo);  // the full-size twin of a lo image, null for any other image
bool spatial_upscale_ok();   // linear blits of the scene formats work (3D resolution below 100%)
void upscale_frame_end();  // vk_replay.cpp, after g_present_no++: next frame's phase
void auto_scene_res(bool scene_frame);  // vk.cpp: Auto 3D resolution at a frame end (scene_frame: it had the FXAA draw)
void upscale_apply_setting();  // vk.cpp: the upscaling setting changed -> lo images rebuilt (frame end only)

}  // namespace vk
