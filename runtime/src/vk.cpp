#include <algorithm>
// NVN backend on Vulkan 1.3: device and swapchain on the SDL window, memory pools imported zero-copy
// (VK_EXT_external_memory_host), textures as device-local images, recorded NVN commands replayed per submit.
// ponytail: every submit waits for the GPU (vkQueueWaitIdle); pipeline it once frame times are measured.
#include "plat.h"

#include "vk.h"
#include "sharpen_spv.h"
#include "upscale_spv.h"
#include "overlay_spv.h"
#include "hdr_spv.h"
#ifdef _WIN32
#include <dxgi1_6.h>
#endif
#include "vk_shader.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <chrono>
#include <ctime>
#include <thread>
#include <map>
#include <set>

using namespace rt;

namespace nvn { extern std::atomic<uint32_t> g_pipe_shared_hits; extern std::atomic<uint64_t> g_twin_syncs; }  // vk_replay.cpp
namespace rt { SDL_Window* ui_window(); extern std::atomic<int> g_shot_request, g_drawlog_request, g_rt_request, g_user_shot; }

namespace nvn { void dump_recent_draws(); void ms_image_dead(vk::Image* im, VkImage img); }  // vk_replay.cpp
namespace vk {

#define VK_DEF(f) PFN_##f f;
VK_FUNCS(VK_DEF)
#undef VK_DEF

VkDevice dev;
VkPhysicalDevice phys;
VkQueue queue;
uint32_t qfam;
VkPhysicalDeviceProperties props;
static VkInstance g_inst;
static VkSurfaceKHR g_surface;
static VkSwapchainKHR g_swap;
static std::vector<VkImage> g_swap_images;
static VkExtent2D g_swap_extent;
static VkFormat g_swap_format;
// HDR output (setting hdr): 0 SDR, 1 HDR10 (BT.2020 + PQ, 10-bit), 2 scRGB (linear BT.709 half floats)
std::atomic<int> g_hdr_mode{0};
static int g_hdr_req = -1;             // hdr setting the swapchain was made for (present_thread remakes it on a change)
static bool g_colorspace_ext = false;  // VK_EXT_swapchain_colorspace: HDR colour spaces can be offered at all
static float g_hdr_screen_peak = 0;    // nits Windows reports for the window's screen (0: unknown)
// the float image the game copied into the window texture g_hdr_win this frame (vk_replay.cpp; replay thread)
static Image* g_hdr_src = nullptr;
static uint32_t g_hdr_win = 0;
void hdr_note_copy(uint32_t window_guest, Image* src) { g_hdr_win = window_guest, g_hdr_src = src; }
void hdr_note_target(uint32_t guest) { if (guest == g_hdr_win) g_hdr_src = nullptr; }
static VkCommandPool g_pool;
static VkCommandBuffer g_cb;
static VkFence g_fence;
// Submit slots: the game's queue work is submitted without waiting; a slot (command buffer, descriptor pools, upload
// ring) is reused once its submit finished, so up to 3 submits run on the GPU while the game prepares the next frame.
// A timeline semaphore counts finished submits (serials); NVN syncs wait for serials (nvn_hle.cpp).
struct Slot { VkCommandBuffer cb = VK_NULL_HANDLE; uint64_t serial = 0; };
static Slot g_slots[3];
static int g_slot = 0;
static VkSemaphore g_timeline = VK_NULL_HANDLE;
std::atomic<uint32_t> g_hitch_us[HITCH_N];
RenderWait::~RenderWait() { g_hitch_us[HITCH_RENDER_WAIT] += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
static uint64_t g_replay_us_acc = 0;  // replay + submit time since the last frames line (present thread order: game thread only)
static uint64_t g_gpuwait_us_acc = 0;  // game-thread GPU waits since the last frames line (GPU-bound share, s75)
// Timestamps at the start and end of every submit (when the queue has timers): their sum = GPU busy time, used by
// Auto 3D resolution (scene_res = auto) and shown on the frames line with NX_GPU_TIME=1 (debug)
static VkQueryPool g_ts_pool = VK_NULL_HANDLE;
static bool g_ts_written[3];
static double g_gpu_busy_ns_acc = 0;
static std::atomic<uint64_t> g_frame_gpu_ns{0};
static PFN_vkGetPhysicalDeviceMemoryProperties2 g_mem_props2;  // with VK_EXT_memory_budget enabled, else null
// "perf: VRAM" (s124, #11): the driver's budget for this game against what it uses. Over budget, Windows moves memory to
// system RAM, which makes every frame much slower on small cards; the line says so.
static void vram_budget_line() {
    if (!g_mem_props2) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &mb};
    g_mem_props2(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++) {
        if (!(mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || !mb.heapBudget[i]) continue;
        const double use = mb.heapUsage[i] / 1048576.0, bud = mb.heapBudget[i] / 1048576.0;
        log("perf: VRAM %.0f of %.0f MB budget used (heap %u, %.0f MB)%s", use, bud, i, mp.memoryProperties.memoryHeaps[i].size / 1048576.0,
            use > bud ? ": OVER BUDGET, Windows moves memory to system RAM (much slower): lower res / MSAA / texture pack" : "");
        break;
    }
}  // GPU busy ns of submits finished since the last frame end (auto_scene_res)
static uint64_t g_rwait_us_acc = 0, g_slotwait_us_acc = 0;  // game thread blocked on the render thread / render thread on its submit slot
static thread_local bool t_present_thread = false;
static std::atomic<uint64_t> g_serial{0};  // last serial submitted
static VkSemaphore g_acquire_sem, g_render_sem;
static VkQueue g_present_queue = VK_NULL_HANDLE;  // second queue: async present (present_thread); null = present inline
static VkPhysicalDeviceMemoryProperties g_memprops;
static std::mutex g_vk_lock;  // ponytail: one lock around all GPU work; the game submits from one thread anyway

static PFN_vkGetDeviceFaultInfoEXT g_fault_info;  // VK_EXT_device_fault, when the driver has it
PFN_vkCmdSetCheckpointNV g_checkpoint;            // VK_NV_device_diagnostic_checkpoints: a marker per draw
static PFN_vkGetQueueCheckpointDataNV g_checkpoint_data;

static void log_device_fault() {
    if (!g_fault_info || !dev) return;
    VkDeviceFaultCountsEXT n{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (g_fault_info(dev, &n, nullptr) < 0) return;
    std::vector<VkDeviceFaultAddressInfoEXT> addrs(n.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vend(n.vendorInfoCount);
    n.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT fi{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    fi.pAddressInfos = addrs.data();
    fi.pVendorInfos = vend.data();
    if (g_fault_info(dev, &n, &fi) < 0) return;
    log("vk: device fault: %s", fi.description);
    static const char* kinds[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (auto& a : addrs)
        log("vk:   address %llx (+-%llx) %s", (unsigned long long)a.reportedAddress, (unsigned long long)a.addressPrecision,
            a.addressType < 7 ? kinds[a.addressType] : "?");
    for (auto& v : vend) log("vk:   vendor %s code %llx data %llx", v.description, (unsigned long long)v.vendorFaultCode, (unsigned long long)v.vendorFaultData);
}

static void log_checkpoints() {
    if (!g_checkpoint_data) return;
    uint32_t n = 0;
    g_checkpoint_data(queue, &n, nullptr);
    std::vector<VkCheckpointDataNV> cp(n, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
    g_checkpoint_data(queue, &n, cp.data());
    for (auto& c : cp) log("vk: checkpoint stage %#x: draw %llu", (unsigned)c.stage, (unsigned long long)(uintptr_t)c.pCheckpointMarker);
}

void check(VkResult r, const char* what) {
    if (r == VK_ERROR_DEVICE_LOST) { log_device_fault(); log_checkpoints(); ::nvn::dump_recent_draws(); }
    if (r != VK_SUCCESS) fatal("Vulkan: %s failed (%d)", what, (int)r);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    fatal("no Vulkan memory type for bits %x flags %x", bits, want);
}

// 256 MB blocks per memory type, first-fit over each block's free ranges (images of finalized textures give theirs
// back through free_mem). An empty device-local block goes back to the driver when the type has another empty one
// (one spare: no vkAllocateMemory hitch on the next map; user 11-16 "lowest VRAM": MSAA off / mode switches left GBs)
struct MemBlock { VkDeviceMemory mem; VkDeviceSize size; std::map<VkDeviceSize, VkDeviceSize> free; };  // offset -> size
static std::mutex g_mem_lock;
static std::map<uint32_t, std::vector<MemBlock>> g_blocks;
std::atomic<uint64_t> g_mem_blocks_bytes{0}, g_mem_used_bytes{0};
std::atomic<uint64_t> g_wgen{0};

VkDeviceMemory alloc(VkMemoryRequirements req, VkMemoryPropertyFlags flags, VkDeviceSize* offset) {
    uint32_t t = mem_type(req.memoryTypeBits, flags);
    std::lock_guard<std::mutex> l(g_mem_lock);
    // s124 (#11, 920MX 2 GB: 1792 MB in 256 MB blocks for 1050 MB used): under 4 GB of video memory, 64 MB blocks so less
    // is reserved but unused (each memory type keeps a part-filled block). NX_VRAM_BLOCK_MB=<n> overrides.
    static auto block_bytes = [] {
        static const VkDeviceSize b = getenv("NX_VRAM_BLOCK_MB") ? std::clamp<VkDeviceSize>(atoi(getenv("NX_VRAM_BLOCK_MB")), 16, 1024) << 20
                                      : vram_bytes() < (4ull << 30) ? 64ull << 20 : 256ull << 20;
        return b;
    };
    static uint32_t spill = ~0u;  // memory type that took blocks after video memory ran full (see below)
    for (uint32_t tt : {t, spill}) {
    if (tt == ~0u || !(req.memoryTypeBits & (1u << tt)) || (g_memprops.memoryTypes[tt].propertyFlags & flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
    auto& v = g_blocks[tt];
    for (auto& b : v)
        for (auto it = b.free.begin(); it != b.free.end(); ++it) {
            VkDeviceSize o = it->first, s = it->second, a = (o + req.alignment - 1) & ~(req.alignment - 1);
            if (a + req.size > o + s) continue;
            b.free.erase(it);
            if (a > o) b.free[o] = a - o;
            if (a + req.size < o + s) b.free[a + req.size] = o + s - a - req.size;
            g_mem_used_bytes += req.size;
            *offset = a;
            return b.mem;
        }
    }
    VkDeviceSize size = std::max<VkDeviceSize>(block_bytes(), req.size);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, size, t};
    VkDeviceMemory m;
    // NX_VRAM_LIMIT_MB=<n> (test): device-local blocks past n MB fail as if the GPU were full
    static const uint64_t limit = getenv("NX_VRAM_LIMIT_MB") ? strtoull(getenv("NX_VRAM_LIMIT_MB"), nullptr, 10) << 20 : 0;
    uint64_t local = 0;
    for (auto& [bt, bv] : g_blocks)
        if (g_memprops.memoryTypes[bt].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) for (auto& b : bv) local += b.size;
    const bool fake_full = limit && (g_memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && local + size > limit;
    VkResult r = fake_full ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkAllocateMemory(dev, &ai, nullptr, &m);
    // Low-VRAM GPUs (2-4 GB, iGPUs with a small carve-out): a full heap used to end the game here. Try a block of just this
    // size, then any other type the resource allows (system RAM: slower, but the game keeps running).
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
        ai.allocationSize = size = req.size;
        r = fake_full ? r : vkAllocateMemory(dev, &ai, nullptr, &m);
        for (uint32_t i = 0; r != VK_SUCCESS && i < g_memprops.memoryTypeCount; i++) {
            if (i == t || !(req.memoryTypeBits & (1u << i))) continue;
            if (fake_full && (g_memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(g_memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            ai.memoryTypeIndex = i, ai.allocationSize = size = std::max<VkDeviceSize>(block_bytes(), req.size);
            if ((r = vkAllocateMemory(dev, &ai, nullptr, &m)) == VK_SUCCESS) {
                static std::atomic<int> n{0};
                if (n++ < 3) log("vk: video memory full: %llu MB in memory type %u instead of %u (slower)", (unsigned long long)(size >> 20), i, t);
                t = spill = i;
            }
        }
    }
    VKC(r);
    auto& nv = g_blocks[t];
    nv.push_back({m, size, {}});
    if (req.size < size) nv.back().free[req.size] = size - req.size;
    g_mem_blocks_bytes += size;
    g_mem_used_bytes += req.size;
    *offset = 0;
    return m;
}

void free_mem(VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size) {
    std::lock_guard<std::mutex> l(g_mem_lock);
    for (auto& [t, v] : g_blocks)
        for (auto& b : v) {
            if (b.mem != m) continue;
            g_mem_used_bytes -= size;
            auto next = b.free.lower_bound(off);
            if (next != b.free.end() && next->first == off + size) { size += next->second; next = b.free.erase(next); }
            bool merged = false;
            if (next != b.free.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second == off) prev->second += size, merged = true;
            }
            if (!merged) b.free[off] = size;
            const VkMemoryPropertyFlags pf = g_memprops.memoryTypes[t].propertyFlags;
            auto empty = [](const MemBlock& x) { return x.free.size() == 1 && x.free.begin()->first == 0 && x.free.begin()->second == x.size; };
            if (!empty(b) || !(pf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || (pf & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return;
            if (std::count_if(v.begin(), v.end(), empty) < 2) return;
            vkFreeMemory(dev, b.mem, nullptr);
            g_mem_blocks_bytes -= b.size;
            v.erase(v.begin() + (&b - v.data()));
            return;
        }
}

// ------------------------------------------------------------------------------------------------- init
PFN_vkCmdPushDescriptorSetKHR g_push_desc = nullptr;
uint32_t g_max_push = 0;
bool g_vk_storage_ext_formats = true, g_vk_pipeline_stats = true, g_vk_pipe_cache_ctl = false;
static void load_instance_funcs() {
    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
#define VK_LOAD(f) f = (PFN_##f)gipa(g_inst, #f);
    VK_FUNCS(VK_LOAD)
#undef VK_LOAD
}

VkInstance g_inst_handle() { return g_inst; }
PFN_vkGetInstanceProcAddr instance_proc_addr() { return (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr(); }
PFN_vkGetDeviceProcAddr device_proc_addr() {  // for vk_fsr3.cpp (the FidelityFX backend loads its own functions)
    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    return (PFN_vkGetDeviceProcAddr)gipa(g_inst, "vkGetDeviceProcAddr");
}

// HDR: the brightest nits Windows reports for the screen the window is on (DXGI output description), 0 if unknown
static float screen_peak_nits() {
#ifdef _WIN32
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(ui_window()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    IDXGIFactory1* fac = nullptr;
    if (!mon || FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return 0;
    float peak = 0;
    IDXGIAdapter1* ad = nullptr;
    for (UINT a = 0; !peak && fac->EnumAdapters1(a, &ad) == S_OK; a++) {
        IDXGIOutput* out = nullptr;
        for (UINT o = 0; !peak && ad->EnumOutputs(o, &out) == S_OK; o++) {
            IDXGIOutput6* o6 = nullptr;
            DXGI_OUTPUT_DESC1 d;
            if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6)) && SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == mon)
                peak = d.MaxLuminance;
            if (o6) o6->Release();
            out->Release();
        }
        ad->Release();
    }
    fac->Release();
    return peak;
#else
    return 0;
#endif
}

// #17: F11 sometimes left the picture at the wrong scale: a swapchain rebuilt mid-transition (in-between window size)
// was never rebuilt at the final size because Windows drivers need not report OUT_OF_DATE / SUBOPTIMAL after a resize.
// The window's pixel-size event (ui.cpp) now asks for a rebuild at the next present.
static std::atomic<bool> g_swap_resize{false};
void window_resized() { g_swap_resize = true; }  // rt::window_resized forwards here
static void create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, g_surface, &caps));
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, g_surface, &n, fmts.data());
    VkSurfaceFormatKHR f = fmts[0];
    for (auto& x : fmts)
        if (x.format == VK_FORMAT_B8G8R8A8_UNORM && x.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) f = x;
    // HDR (setting hdr): only with the async present path (its staging pass writes the HDR picture) and when the
    // screen offers an HDR format (Windows HDR on); HDR10 first, then scRGB, else SDR as before
    int mode = 0;
    g_hdr_req = rt::hdr_setting();
    if (g_hdr_req && g_present_queue) {
        auto blit_ok = [](VkFormat fm) {  // present_thread blits the staging image into the swapchain image
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys, fm, &fp);
            return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        };
        for (auto& x : fmts)
            if (!mode && x.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && blit_ok(x.format) &&
                (x.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || x.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32))
                f = x, mode = 1;
        for (auto& x : fmts)
            if (!mode && x.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && x.format == VK_FORMAT_R16G16B16A16_SFLOAT && blit_ok(x.format))
                f = x, mode = 2;
        if (mode) g_hdr_screen_peak = screen_peak_nits();
    }
    static int told = -1;
    if (told != mode + 4 * g_hdr_req) {
        told = mode + 4 * g_hdr_req;
        if (mode) log("vk: HDR output on: %s (format %d), screen peak %.0f nits (Windows), paper white %d nits, peak setting %d", mode == 1 ? "HDR10 / PQ" : "scRGB",
                      (int)f.format, g_hdr_screen_peak, rt::hdr_paper_white(), rt::hdr_peak_setting());
        else if (g_hdr_req) log("vk: HDR asked for but %s: SDR output", !g_present_queue ? "no async present queue" : !g_colorspace_ext ? "the driver has no VK_EXT_swapchain_colorspace" : "the screen offers no HDR format (Windows HDR off?)");
    }
    g_hdr_mode = mode;
    g_swap_format = f.format;
    g_swap_extent = caps.currentExtent;
    if (caps.currentExtent.width == 0xFFFFFFFF) {  // Wayland: the surface has no size of its own; take the window's
        int w = 0, h = 0;
        if (SDL_Window* win = rt::ui_window()) SDL_GetWindowSizeInPixels(win, &w, &h);
        g_swap_extent = w > 0 && h > 0 ? VkExtent2D{(uint32_t)w, (uint32_t)h} : VkExtent2D{1280, 720};
        g_swap_extent.width = std::clamp(g_swap_extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        g_swap_extent.height = std::clamp(g_swap_extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    // 3 images so a present never waits for the display (the game is paced by its own vsync clock, ui.cpp)
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = f.format;
    ci.imageColorSpace = f.colorSpace;
    ci.imageExtent = g_swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // Present mode: mailbox by default (shows the newest frame at the next refresh without blocking the game; with
    // VRR the display follows the game's 30/60 fps exactly). FIFO blocked the game for up to a refresh and beat against
    // our vsync clock on a 160 Hz display. NX_PRESENT_MODE=fifo|mailbox|immediate overrides; NX_NO_VSYNC = immediate.
    // vrr (G-SYNC / FreeSync) = FIFO here; ui.cpp caps the frame rate under the refresh (present_thread takes the waits).
    uint32_t nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, g_surface, &nm, nullptr);
    std::vector<VkPresentModeKHR> modes(nm);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, g_surface, &nm, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    const char* want = getenv("NX_NO_VSYNC") ? "immediate" : getenv("NX_PRESENT_MODE") ? getenv("NX_PRESENT_MODE") : "mailbox";
    VkPresentModeKHR pm = !strcmp(want, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR
                        : !strcmp(want, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (!has(pm)) pm = has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swap;
    VkSwapchainKHR s;
    VKC(vkCreateSwapchainKHR(dev, &ci, nullptr, &s));
    if (g_swap) vkDestroySwapchainKHR(dev, g_swap, nullptr);
    g_swap = s;
    vkGetSwapchainImagesKHR(dev, g_swap, &n, nullptr);
    g_swap_images.resize(n);
    vkGetSwapchainImagesKHR(dev, g_swap, &n, g_swap_images.data());
    log("vk: swapchain %ux%u, %u images, present mode %s", g_swap_extent.width, g_swap_extent.height, n,
        ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : ci.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
}

static void init() {
    SDL_Window* win = ui_window();
    if (!SDL_Vulkan_LoadLibrary(nullptr)) fatal("SDL_Vulkan_LoadLibrary: %s", SDL_GetError());
    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    Uint32 next = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&next);
    std::vector<const char*> iext(exts, exts + next);
    iext.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    {  // HDR colour spaces (setting hdr): enabled only where the loader lists the extension
        auto enum_iext = (PFN_vkEnumerateInstanceExtensionProperties)gipa(nullptr, "vkEnumerateInstanceExtensionProperties");
        uint32_t ne = 0;
        if (enum_iext) enum_iext(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ie(ne);
        if (ne) enum_iext(nullptr, &ne, ie.data());
        for (auto& e : ie) g_colorspace_ext |= !strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        bool listed = false;
        for (auto* e : iext) listed |= !strcmp(e, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        if (g_colorspace_ext && !listed) iext.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    dlss_instance_exts(iext);  // nothing unless nvngx_dlss.dll is next to tms.exe
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "TMSRecomp", 1, "TMSRecomp", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)iext.size();
    ici.ppEnabledExtensionNames = iext.data();
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (getenv("NX_VK_VALIDATION")) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
    VKC(vkCreateInstance(&ici, nullptr, &g_inst));
    load_instance_funcs();
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g_inst, &n, pds.data());
    phys = pds[0];
    for (auto p : pds) {  // prefer a discrete GPU
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(p, &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = p; break; }
    }
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &g_memprops);
    log("vk: %s (driver %u.%u), %llu MB video memory", props.deviceName, props.driverVersion >> 22, (props.driverVersion >> 14) & 0xFF,
        (unsigned long long)(vram_bytes() >> 20));
    rt::perfmon_start(props.deviceName, props.vendorID);
    if (!SDL_Vulkan_CreateSurface(win, g_inst, nullptr, &g_surface)) fatal("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qf.data());
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        VkBool32 pres = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, g_surface, &pres);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && pres) { qfam = i; break; }
    }
    if (qfam == UINT32_MAX) fatal("vk: no graphics+present queue");
    float prio[2] = {1.0f, 1.0f};
    // a second queue of the same family for presenting (present_thread), so the game's queue never waits on the display
    const uint32_t nq = qf[qfam].queueCount >= 2 && !getenv("NX_SYNC_PRESENT") ? 2 : 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qfam, nq, prio};
    // depth clip control: NVN's default depth mode clips z to [-w, w] like OpenGL
    const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                          VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_DEVICE_FAULT_EXTENSION_NAME,
                          VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME};
    auto enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)gipa(g_inst, "vkEnumerateDeviceExtensionProperties");
    uint32_t next_n = 0;
    enum_ext(phys, nullptr, &next_n, nullptr);
    std::vector<VkExtensionProperties> have(next_n);
    enum_ext(phys, nullptr, &next_n, have.data());
    bool fault_ext = false;
    for (auto& e : have) fault_ext |= !strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    bool cp_ext = false;  // debug only (NX_CHECKPOINTS): costs a little per draw
    for (auto& e : have) cp_ext |= getenv("NX_CHECKPOINTS") && !strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    if (cp_ext && !fault_ext) dext[3] = dext[4];
    VkPhysicalDeviceFaultFeaturesEXT ffe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    ffe.deviceFault = VK_TRUE;
    VkPhysicalDeviceDepthClipControlFeaturesEXT fdc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, fault_ext ? &ffe : nullptr};
    fdc.depthClipControl = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &fdc};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.scalarBlockLayout = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;  // drawlog pipeline statistics (vk_replay.cpp)
    f12.timelineSemaphore = VK_TRUE;  // submit serials
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.geometryShader = VK_TRUE;
    f2.features.fragmentStoresAndAtomics = VK_TRUE;
    f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
    f2.features.shaderClipDistance = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.dualSrcBlend = VK_TRUE;
    f2.features.logicOp = VK_TRUE;
    f2.features.multiViewport = VK_TRUE;
    f2.features.wideLines = VK_TRUE;
    f2.features.shaderStorageImageExtendedFormats = VK_TRUE;  // SMAA writes the R11G11B10 frame (vk_replay.cpp)
    f2.features.pipelineStatisticsQuery = VK_TRUE;
    f2.features.robustBufferAccess = VK_TRUE;  // out-of-range buffer reads return 0 instead of faulting, like Maxwell
    // PORTABILITY (user rule 11-10): ask only for what this GPU has. Required features / extensions missing -> a clear message
    // (update the driver) instead of a failed vkCreateDevice; optional ones are switched off with a log line.
    {
        if (VK_API_VERSION_MINOR(props.apiVersion) < 3 && VK_API_VERSION_MAJOR(props.apiVersion) == 1)
            fatal("Your GPU driver supports Vulkan %u.%u, TMSRecomp needs Vulkan 1.3.\nPlease update the graphics driver (%s).",
                  VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), props.deviceName);
        std::string missing;
        for (const char* e : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME}) {
            bool ok = false;
            for (auto& h : have) ok |= !strcmp(h.extensionName, e);
            if (!ok) missing += std::string(" ") + e;
        }
        VkPhysicalDeviceDepthClipControlFeaturesEXT hdc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, missing.empty() ? &hdc : nullptr};
        VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &h13};
        VkPhysicalDeviceFeatures2 h2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h12};
        vkGetPhysicalDeviceFeatures2(phys, &h2);
        if (!h13.dynamicRendering) missing += " dynamicRendering";
        if (!h12.timelineSemaphore) missing += " timelineSemaphore";
        if (!h12.scalarBlockLayout) missing += " scalarBlockLayout";
        if (!h2.features.textureCompressionBC) missing += " textureCompressionBC";
        if (missing.empty() && !hdc.depthClipControl) missing += " depthClipControl";
        if (!missing.empty())
            fatal("Your GPU or driver (%s) lacks Vulkan features TMSRecomp needs:%s\nPlease update the graphics driver.", props.deviceName, missing.c_str());
        f12.hostQueryReset &= h12.hostQueryReset;
        if (!h2.features.pipelineStatisticsQuery) f12.hostQueryReset = VK_FALSE;  // debug drawlog only
        // optional: the game never draws wide lines (line width stays 1), logic ops are not used, the rest is used where present
        VkBool32* want = &f2.features.robustBufferAccess;
        const VkBool32* got = &h2.features.robustBufferAccess;
        static const char* const names[] = {"robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
            "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp",
            "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
            "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
            "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended",
            "shaderStorageImageExtendedFormats"};
        for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++)
            if (want[i] && !got[i]) { want[i] = VK_FALSE; log("vk: %s not supported by this GPU: off", names[i]); }
        g_vk_storage_ext_formats = f2.features.shaderStorageImageExtendedFormats;
        g_vk_pipeline_stats = f2.features.pipelineStatisticsQuery;
        // optional: cache-hit pipelines are created inline on the game thread (vk_replay.cpp pipeline()); off = all async
        f13.pipelineCreationCacheControl = g_vk_pipe_cache_ctl = h13.pipelineCreationCacheControl &&
            (!getenv("NX_PIPE_INLINE") || atoi(getenv("NX_PIPE_INLINE")) != 0);
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    std::vector<const char*> dexts(dext, dext + 3 + fault_ext + cp_ext);
    for (auto& e : have)  // s124: Windows' video memory budget for the "perf: VRAM" line (PORTABILITY: optional)
        if (!strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
            dexts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            g_mem_props2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties2");
        }
    dlss_device_exts(have, dexts);
    // optional (s118, Augs' CPU-bound crosswalk): push descriptors write a draw's resources straight into the command
    // buffer (vkCmdPushDescriptorSetKHR) instead of allocate + update + bind per draw. NX_PUSH_DESC=0 off.
    bool push_ext = !getenv("NX_PUSH_DESC") || atoi(getenv("NX_PUSH_DESC")) != 0;
    if (push_ext) {
        bool found = false;
        for (auto& e : have) found |= !strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        push_ext = found;
    }
    if (push_ext) {
        dexts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        VkPhysicalDevicePushDescriptorPropertiesKHR pdp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pdp};
        if (auto f = (PFN_vkGetPhysicalDeviceProperties2)gipa(g_inst, "vkGetPhysicalDeviceProperties2")) f(phys, &p2);
        g_max_push = pdp.maxPushDescriptors;
    }
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    VKC(vkCreateDevice(phys, &dci, nullptr, &dev));
    if (push_ext) g_push_desc = (PFN_vkCmdPushDescriptorSetKHR)gipa(g_inst, "vkCmdPushDescriptorSetKHR");
    if (!g_push_desc) g_max_push = 0;
    log("vk: push descriptors %s (max %u)", g_push_desc ? "on" : "off", g_max_push);
    if (fault_ext) g_fault_info = (PFN_vkGetDeviceFaultInfoEXT)gipa(g_inst, "vkGetDeviceFaultInfoEXT");
    if (cp_ext) {
        g_checkpoint = (PFN_vkCmdSetCheckpointNV)gipa(g_inst, "vkCmdSetCheckpointNV");
        g_checkpoint_data = (PFN_vkGetQueueCheckpointDataNV)gipa(g_inst, "vkGetQueueCheckpointDataNV");
        log("vk: diagnostic checkpoints on");
    }
    log("vk: device fault reporting %s", fault_ext ? "on" : "unavailable");
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    if (nq == 2) vkGetDeviceQueue(dev, qfam, 1, &g_present_queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VKC(vkCreateCommandPool(dev, &pci, nullptr, &g_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, g_pool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKC(vkAllocateCommandBuffers(dev, &cai, &g_cb));
    for (auto& sl : g_slots) VKC(vkAllocateCommandBuffers(dev, &cai, &sl.cb));
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    VKC(vkCreateSemaphore(dev, &tsci, nullptr, &g_timeline));
    if (qf[qfam].timestampValidBits && props.limits.timestampPeriod > 0) {  // PORTABILITY: no timers = Auto 3D resolution stays 100%
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, 6};
        if (vkCreateQueryPool(dev, &qpi, nullptr, &g_ts_pool) != VK_SUCCESS) g_ts_pool = VK_NULL_HANDLE;
        if (getenv("NX_GPU_TIME")) log("vk: GPU timing %s (NX_GPU_TIME)", g_ts_pool ? "on" : "unavailable");
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKC(vkCreateFence(dev, &fci, nullptr, &g_fence));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKC(vkCreateSemaphore(dev, &sci, nullptr, &g_acquire_sem));
    VKC(vkCreateSemaphore(dev, &sci, nullptr, &g_render_sem));
    create_swapchain();
}

// ------------------------------------------------------------------------------------------------- formats
// Depth formats every GPU may not have: AMD's Windows driver has no D24_UNORM_S8_UINT (NVIDIA / Intel do), and S8_UINT is
// optional. Without them the depth image failed to create and the game ended at the first 3D frame. D32_SFLOAT_S8_UINT holds
// the same values (more depth precision). NX_NO_D24=1 forces the fallback (test on a GPU that has D24).
VkFormat depth_format(VkFormat want) {
    static const bool force = getenv("NX_NO_D24") && atoi(getenv("NX_NO_D24"));
    if (want != VK_FORMAT_D24_UNORM_S8_UINT && want != VK_FORMAT_S8_UINT) return want;
    static std::atomic<int> ok[2] = {-1, -1};  // D24S8, S8
    std::atomic<int>& o = ok[want == VK_FORMAT_S8_UINT];
    if (o < 0) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys, want, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        o = !(want == VK_FORMAT_D24_UNORM_S8_UINT && force) && (fp.optimalTilingFeatures & need) == need;
        if (!o) log("vk: %s not supported by this GPU: using D32_SFLOAT_S8_UINT", want == VK_FORMAT_S8_UINT ? "S8_UINT" : "D24_UNORM_S8_UINT");
    }
    return o ? want : VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static VkFormat vk_format_raw(uint32_t f, bool* depth, bool* stencil);
VkFormat vk_format(uint32_t f, bool* depth, bool* stencil) { return depth_format(vk_format_raw(f, depth, stencil)); }
static VkFormat vk_format_raw(uint32_t f, bool* depth, bool* stencil) {
    if (depth) *depth = false;
    if (stencil) *stencil = false;
    switch (f) {
        case 0x01: return VK_FORMAT_R8_UNORM;
        case 0x02: return VK_FORMAT_R8_SNORM;
        case 0x03: return VK_FORMAT_R8_UINT;
        case 0x04: return VK_FORMAT_R8_SINT;
        case 0x05: return VK_FORMAT_R16_SFLOAT;
        case 0x06: return VK_FORMAT_R16_UNORM;
        case 0x07: return VK_FORMAT_R16_SNORM;
        case 0x08: return VK_FORMAT_R16_UINT;
        case 0x09: return VK_FORMAT_R16_SINT;
        case 0x0a: return VK_FORMAT_R32_SFLOAT;
        case 0x0b: return VK_FORMAT_R32_UINT;
        case 0x0c: return VK_FORMAT_R32_SINT;
        case 0x0d: return VK_FORMAT_R8G8_UNORM;
        case 0x0e: return VK_FORMAT_R8G8_SNORM;
        case 0x0f: return VK_FORMAT_R8G8_UINT;
        case 0x10: return VK_FORMAT_R8G8_SINT;
        case 0x11: return VK_FORMAT_R16G16_SFLOAT;
        case 0x12: return VK_FORMAT_R16G16_UNORM;
        case 0x13: return VK_FORMAT_R16G16_SNORM;
        case 0x14: return VK_FORMAT_R16G16_UINT;
        case 0x15: return VK_FORMAT_R16G16_SINT;
        case 0x16: return VK_FORMAT_R32G32_SFLOAT;
        case 0x17: return VK_FORMAT_R32G32_UINT;
        case 0x18: return VK_FORMAT_R32G32_SINT;
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
        case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
        case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
        case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0x2a: return VK_FORMAT_R16G16B16A16_UNORM;
        case 0x2b: return VK_FORMAT_R16G16B16A16_SNORM;
        case 0x2c: return VK_FORMAT_R16G16B16A16_UINT;
        case 0x2d: return VK_FORMAT_R16G16B16A16_SINT;
        case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x2f: return VK_FORMAT_R32G32B32A32_UINT;
        case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
        case 0x31: if (stencil) *stencil = true; return VK_FORMAT_S8_UINT;
        case 0x32: if (depth) *depth = true; return VK_FORMAT_D16_UNORM;
        case 0x33: if (depth) *depth = true; return VK_FORMAT_D24_UNORM_S8_UINT;
        case 0x34: if (depth) *depth = true; return VK_FORMAT_D32_SFLOAT;
        case 0x35: if (depth) *depth = true; if (stencil) *stencil = true; return VK_FORMAT_D24_UNORM_S8_UINT;
        case 0x36: if (depth) *depth = true; if (stencil) *stencil = true; return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
        case 0x39: return VK_FORMAT_A4B4G4R4_UNORM_PACK16;
        case 0x3a: case 0x3b: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case 0x3c: return VK_FORMAT_B5G6R5_UNORM_PACK16;
        case 0x3d: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 0x3e: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 0x3f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
        case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
        case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
        case 0x4a: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 0x4b: return VK_FORMAT_BC5_UNORM_BLOCK;
        case 0x4c: return VK_FORMAT_BC5_SNORM_BLOCK;
        case 0x4d: return VK_FORMAT_BC7_UNORM_BLOCK;
        case 0x4e: return VK_FORMAT_BC7_SRGB_BLOCK;
        case 0x4f: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;  // RGB10A2SN (seen as a vertex format)
        case 0x6a: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
        case 0x6b: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case 0x6c: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        case 0x95: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 0x99: case 0x9a: return VK_FORMAT_B8G8R8A8_UNORM;
        case 0x9b: case 0x9c: return VK_FORMAT_B8G8R8A8_SRGB;
        default:
            if (f >= 0x79 && f <= 0x86) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (f - 0x79));
            if (f >= 0x87 && f <= 0x94) return (VkFormat)(VK_FORMAT_ASTC_4x4_SRGB_BLOCK + 2 * (f - 0x87));
            static std::mutex m;
            static std::set<uint32_t> seen;
            {
                std::lock_guard<std::mutex> l(m);
                if (seen.insert(f).second) log("vk: unmapped NVN format %#x (using RGBA8)", f);
            }
            return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

// ------------------------------------------------------------------------------------------------- resources
// Guest memory imported into Vulkan (pinned, zero-copy). Imports are never released: the game frees and re-creates pools
// at the same addresses on every map/battle load, and importing again cost 50-120 ms per load (hitch log).
// Only what the GPU reads is imported, in 2 MiB windows of the pool (a range that does not fit one window: its own
// import), not whole pools: AMD's kernel driver re-checks every page of every imported (userptr) buffer at each
// submit, and the game's 1.37 GB graphics pool made each vkQueueSubmit take 13.5 ms on a ROG Ally (NVIDIA: 0.04 ms).
// Textures in the pool are uploaded by copy (vk_texture.cpp) and stay outside the imports. NX_IMPORT_WHOLE=1: whole
// pools as before.
// Imports the game has not used for ~5 s (300 frames) are released once the GPU is done with them (imports_trim, per
// frame): the windows of every map and battle visited added up (ROG Ally: 358 MB in 500+ imports after a few battles,
// vkQueueSubmit back to 12 ms). A window is imported again in well under a millisecond when it is used again.
struct Import { uint32_t size; VkBuffer buf; VkDeviceMemory mem; uint64_t serial; uint64_t frame; };
static std::map<uint32_t, std::vector<Import>> g_imports;  // base -> imports at that base, overlapping allowed
static uint64_t g_import_frame = 0, g_import_bytes = 0;

static VkBuffer import_range(uint32_t base, uint32_t size) {
    HitchTimer ht{HITCH_IMPORT};
    mem_make_writable(base, size);  // Linux: the driver pins the pages for writing (watched pages are read-only)
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VKC(vkGetMemoryHostPointerPropertiesEXT(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, GPTR(base), &hp));
    VkExternalMemoryBufferCreateInfo emb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr,
                                         VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer b;
    VKC(vkCreateBuffer(dev, &bci, nullptr, &b));
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr,
                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, GPTR(base)};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size,
                            mem_type(hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory m;
    VKC(vkAllocateMemory(dev, &ai, nullptr, &m));
    VKC(vkBindBufferMemory(dev, b, m, 0));
    g_imports[base].push_back({size, b, m, gpu_recording_serial(), g_import_frame});
    g_import_bytes += size;
    static uint32_t n = 0;
    if (++n <= 20 || (n & (n - 1)) == 0) log("vk: imported guest memory %08x+%x (%u imports so far; %.0f MB imported now)", base, size, n, g_import_bytes / 1048576.0);
    return b;
}

GuestBuffer guest_buffer(uint64_t gpu, uint64_t need) {
    uint32_t off;
    nvn::Pool* p = nvn::pool_of(gpu, &off);
    if (!p) return {VK_NULL_HANDLE, 0};
    const uint64_t addr = p->addr + (uint64_t)off, pool_end = (uint64_t)p->addr + p->size;
    static const bool whole = getenv("NX_IMPORT_WHOLE") && atoi(getenv("NX_IMPORT_WHOLE"));
    if (!need || whole) need = pool_end - addr;  // unknown size: to the end of the pool
    need = std::min<uint64_t>(std::max<uint64_t>(need, 1), pool_end - addr);
    // an import that holds [addr, addr + need): the nearest bases at or below addr (windows overlap only within a few)
    auto it = g_imports.upper_bound((uint32_t)addr);
    for (int k = 0; k < 8 && it != g_imports.begin(); k++) {
        --it;
        for (auto& im : it->second)
            if ((uint64_t)it->first + im.size >= addr + need) {
                im.serial = gpu_recording_serial(), im.frame = g_import_frame;
                return {im.buf, addr - it->first, (uint64_t)it->first + im.size - addr};
            }
    }
    constexpr uint64_t W = 2ull << 20;
    uint64_t lo = addr & ~(W - 1), hi = lo + W;
    if (addr + need > hi) lo = addr & ~0xFFFFull, hi = (addr + need + 0xFFFF) & ~0xFFFFull;  // its own import
    if (whole) lo = p->addr, hi = pool_end;
    lo = std::max<uint64_t>(lo, p->addr & ~0xFFFull);
    hi = std::min<uint64_t>(hi, (pool_end + 0xFFF) & ~0xFFFull);
    VkBuffer b = import_range((uint32_t)lo, (uint32_t)(hi - lo));
    return {b, addr - lo, hi - addr};
}

void imports_trim() {  // once per presented frame (g_vk_lock held)
    static const uint64_t keep = getenv("NX_IMPORT_KEEP") ? std::max(2, atoi(getenv("NX_IMPORT_KEEP"))) : 300;  // frames (test)
    if (++g_import_frame % std::min<uint64_t>(60, keep)) return;
    uint32_t n = 0;
    uint64_t bytes = 0;
    for (auto it = g_imports.begin(); it != g_imports.end();) {
        auto& v = it->second;
        for (size_t i = 0; i < v.size();) {
            Import& im = v[i];
            if (g_import_frame - im.frame > keep && gpu_wait(im.serial, 0)) {
                vkDestroyBuffer(dev, im.buf, nullptr);
                vkFreeMemory(dev, im.mem, nullptr);
                bytes += im.size, n++;
                g_import_bytes -= im.size;
                v[i] = v.back();
                v.pop_back();
            } else i++;
        }
        it = v.empty() ? g_imports.erase(it) : std::next(it);
    }
    if (n) LOGV("vk: released %u unused imports (%.1f MB), %.0f MB imported now", n, bytes / 1048576.0, g_import_bytes / 1048576.0);
    static uint64_t last_log = 0;
    if (g_import_frame - last_log >= 600) last_log = g_import_frame, log("vk: guest memory imported now: %.0f MB", g_import_bytes / 1048576.0);
}

bool gpu_is_amd() { return props.vendorID == 0x1002; }

uint64_t vram_bytes() {
    uint64_t vram = 0;
    for (uint32_t i = 0; i < g_memprops.memoryHeapCount; i++)
        if (g_memprops.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)g_memprops.memoryHeaps[i].size);
    if (const char* e = getenv("NX_VRAM_MB")) vram = std::min(vram, (uint64_t)atoi(e) << 20);
    return vram;
}
bool weak_gpu() {
    return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || vram_bytes() < (4ull << 30);
}

// NX_RES=8k|4k|1440p|1080p|720p or NX_RES_SCALE=n (1..6, fractions allowed): render scale over the game's 1280x720 (4k = 3,
// 1440p = 2, 1080p = 1.5, 720p = 1, the default). s118 (Augs, #11): 1080p was 2x = 2560x1440 downsampled, 78% more pixels
// than a 1080p screen shows (RTX 3050 city: GPU waits 8 ms / frame); render targets already take fractional scales (FSR 3
// lo twins), so 1080p is now exactly 1920x1080.
// Ultrawide (setting aspect, NX_ASPECT: 16:9 default, 21:9, 32:9, auto = the desktop's shape, or a number; user request 11-24):
// the game's 16:9 screen-size render targets are made wider by this factor; scene and post draws into them stretch over the
// width (vk_replay.cpp draw) while the game's cameras get the wider aspect (hle_sys.cpp cam_proj_update), so the 3D image
// shows more at the sides (Hor+). Width rounded to 8 game pixels, at most 32:9. Narrower than 16:9: 1 (the present
// letterboxes). Read once at start.
float wide_factor() {
    static const float f = [] {
        const char* e = getenv("NX_ASPECT");
        if (!e || !*e) return 1.f;
        double a = 16.0 / 9;
        int x = 0, y = 0;
        if (!_stricmp(e, "auto")) {
            if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()); dm && dm->h > 0) a = (double)dm->w / dm->h;
        } else if (sscanf(e, "%d:%d", &x, &y) == 2 && x > 0 && y > 0) a = (double)x / y;
        else if (atof(e) > 0) a = atof(e);
        const double k = std::clamp(a / (16.0 / 9), 1.0, 2.0);
        const int w = (int)lround(1280 * k / 8) * 8;
        const float r = w <= 1288 ? 1.f : w / 1280.f;
        log("vk: aspect %s -> %.4f, screen targets %d x 720 game pixels%s", e, a, (int)lround(1280 * r), r == 1 ? " (16:9)" : "");
        return r;
    }();
    return f;
}
float res_scale() {
    static const float s = [] {
        float n = 1;
        // res = auto (fresh-install default, PORTABILITY rule 11-10): fit the screen, capped by the GPU: integrated GPUs and
        // cards under 3 GB of VRAM 720p (s85 Shibuya: 720p 1.8 GB in blocks, 1080p 2.0 GB), under 8 GB 1080p at most (= 1440p,
        // the same 2x render scale: 4K = 3x, 2.25x the pixels, too heavy for a GTX 1060 / RX 580 class card). The resolved
        // value goes back into NX_RES (F1 shows it).
        if (const char* r = getenv("NX_RES"); r && !_stricmp(r, "auto")) {
            const uint64_t vram = vram_bytes();
            int dh = 1080;
            if (const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) dh = dm->h;
            const char* pick = dh > 1440 ? "4k" : dh > 1080 ? "1440p" : "1080p";
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || vram < (3ull << 30)) pick = "720p";
            else if (vram < (8ull << 30) && strcmp(pick, "4k") == 0) pick = "1440p";
            log("vk: res auto -> %s (screen height %d, %s GPU, %llu MB VRAM)", pick, dh,
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : "discrete", (unsigned long long)(vram >> 20));
            _putenv_s("NX_RES", pick);
        }
        if (const char* r = getenv("NX_RES")) {
            int h = atoi(r);
            if (!_stricmp(r, "4k")) h = 2160;
            if (!_stricmp(r, "8k")) h = 4320;  // supersampling on 4K displays: the present blit halves it (2x2 box)
            n = h > 720 ? (h <= 1080 ? h / 720.f : (float)((h + 719) / 720)) : 1;  // 1080p 1.5x, else whole steps
        }
        if (const char* r = getenv("NX_RES_SCALE")) n = (float)atof(r);
        n = std::clamp(std::round(n * 80) / 80, 1.f, 6.f);  // k / 80: 1280x720 x n stays whole pixels
        log("vk: render scale %gx (%ux%u)", n, (uint32_t)lround(1280 * n), (uint32_t)lround(720 * n));
        return n;
    }();
    return s;
}

// The game gives a depth-stencil surface two NVN textures at the same GPU address: one renders / reads depth, the other
// has depth-stencil mode STENCIL (shadow maps: bc960310 + bc960690, 11-06). They must be one image: with an image each,
// shaders read the character ids from an empty one and every character shadowed itself. Guarded by g_ds_lock.
static std::mutex g_ds_lock;
static std::unordered_map<uint64_t, Image*> g_ds_by_gpu;
static Image* make_image(nvn::Texture* t, VkFormat fmt, bool depth, bool stencil, float force_scale);
static Image* twin_register(nvn::Texture* t, Image* im, VkFormat fmt, bool depth, bool stencil);
Image* image_of(nvn::Texture* t) {
    if (!t) return nullptr;
    if (t->host) return static_cast<Image*>(t->host);
    HitchTimer ht{HITCH_IMAGE};
    bool depth, stencil;
    const VkFormat fmt = vk_format(t->format, &depth, &stencil);
    if ((depth || stencil) && t->gpu) {
        Image* shared = nullptr;
        {
            std::lock_guard<std::mutex> l(g_ds_lock);
            auto it = g_ds_by_gpu.find(t->gpu);
            if (it != g_ds_by_gpu.end() && it->second->format == fmt && it->second->w == (uint32_t)lround(t->width * it->second->scale) &&
                it->second->h == (uint32_t)lround(t->height * it->second->scale)) {
                shared = it->second;
                shared->share++;
                t->host = shared;
            }
        }
        if (shared) return twin_register(t, shared, fmt, depth, stencil);
    }
    return twin_register(t, make_image(t, fmt, depth, stencil, 0), fmt, depth, stencil);
}

// FSR 3 upscaling (setting fsr3_quality, NOTES s63). The game's scene target is also its post-FXAA / UI target, so the same
// NVN texture must be small before the FXAA draw and full size after it: every screen-size render target (1280x720 2D, not
// a display target) has a lo twin, and scene_phase swaps t->host of all of them. A frame starts on the lo twins when the
// frame before had a 3D scene (upscale_frame_end), FSR's dispatch at the FXAA draw switches to the full-size images.
// The mode applies live (user 11-16: "no DLSS fps difference" = the mode was read once at start): screen-size targets
// are always tracked, their lo images exist only while upscaling is on and are rebuilt at a frame end when the
// setting changes (upscale_apply_setting); DLSS / FSR 3 rebuild their feature / context on the new size by themselves.
struct Twin { Image* hi; Image* lo; std::vector<nvn::Texture*> tex; };  // lo: nullptr while upscaling is off
static std::vector<Twin> g_twins;  // g_twin_lock
static std::mutex g_twin_lock;
static bool g_scene_lo;
// 3D resolution (setting scene_res) needs linear blits of the scene formats (PORTABILITY: checked, else stays 100%)
bool spatial_upscale_ok() {
    static const bool ok = [] {
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for (VkFormat f : {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(phys, f, &fp);
            if ((fp.optimalTilingFeatures & need) != need) { log("vk: 3D resolution below 100%% unavailable (format %d cannot be blitted linearly)", (int)f); return false; }
        }
        return true;
    }();
    return ok;
}
// Auto 3D resolution (scene_res = auto, s123, #11): like a console engine's dynamic resolution. The GPU time of 3D
// frames (submit timestamps) is averaged over 30-frame windows and compared with the frame budget of the fps setting:
// over 90% twice in a row = one step down; a step up only when even a fully pixel-bound frame would stay under 80% at the
// larger size for 6 windows (and twice as long after a step up had to be taken back), so it does not swing. Each step is
// the same frame-end rebuild of the lo images as a manual change. Menus / movies / loading (no FXAA draw) are not counted.
static const int k_auto_steps[] = {100, 85, 75, 67, 50};
static int g_auto_step = getenv("NX_AUTO_RES_START") ? std::clamp(atoi(getenv("NX_AUTO_RES_START")), 0, 4) : 0;  // render thread (frame end); debug: start step
static int scene_pct() { return rt::scene_res() > 0 ? rt::scene_res() : k_auto_steps[g_auto_step]; }
static float lo_for_setting() {
    if (!rt::fsr3_setting()) {  // 3D resolution (scene_res) with AA off / FXAA / SMAA: linear upscale at the FXAA draw
        const int p = scene_pct();
        if (p >= 100 || !spatial_upscale_ok()) return 0;
        const float hi = res_scale(), lo = std::max(floorf(hi * p / 100.f * 80) / 80, 0.5f);
        return lo < hi ? lo : 0;
    }
    static const float ratio[5] = {1, 1.5f, 1.7f, 2, 3};  // AMD's Quality / Balanced / Performance / Ultra Performance
    const int m = std::clamp(rt::fsr3_quality(), 0, 4);
    const float hi = res_scale();
    float lo = std::max(floorf(hi / ratio[m] * 80) / 80, 0.5f);  // k / 80: 1280x720 x lo stays whole pixels; #11: below 720p too (weak GPUs at res 720p)
    if (!m || lo >= hi) lo = 0;
    return lo;
}
static std::atomic<float> g_lo{-1.f};  // current scene scale, written under g_twin_lock
static void log_lo(float lo) {
    if (!rt::fsr3_setting() && (rt::scene_res() < 100)) {
        log("vk: 3D resolution %s%d%%: %s (scene %ux%u of %ux%u)", rt::scene_res() ? "" : "auto ", scene_pct(), lo > 0 ? "on" : "off", (uint32_t)(1280 * lo), (uint32_t)(720 * lo),
            (uint32_t)lround(1280 * res_scale()), (uint32_t)lround(720 * res_scale()));
        return;
    }
    log("vk: FSR 3 / DLSS upscaling %s (mode %d, scene %ux%u of %ux%u)", lo > 0 ? "on" : "off", rt::fsr3_quality(),
        (uint32_t)(1280 * lo), (uint32_t)(720 * lo), (uint32_t)lround(1280 * res_scale()), (uint32_t)lround(720 * res_scale()));
}
float upscale_lo_scale() {
    float s = g_lo.load(std::memory_order_relaxed);
    if (s < 0) {
        std::lock_guard<std::mutex> l(g_twin_lock);
        if ((s = g_lo) < 0) g_lo = s = lo_for_setting(), log_lo(s);
    }
    return s;
}
static Image* twin_register(nvn::Texture* t, Image* im, VkFormat fmt, bool depth, bool stencil) {
    if (!(t->flags & 8) || (t->flags & 1) || t->target != 1 || t->width != 1280 || t->height != 720 || t->levels > 1 || im->scale != res_scale())
        return im;
    const float lo = upscale_lo_scale();
    std::lock_guard<std::mutex> l(g_twin_lock);
    auto it = std::find_if(g_twins.begin(), g_twins.end(), [&](const Twin& w) { return w.hi == im; });
    if (it == g_twins.end()) {
        g_twins.push_back({im, lo > 0 ? make_image(t, fmt, depth, stencil, lo) : nullptr, {}});
        it = g_twins.end() - 1;
        if (it->lo) im->twin = it->lo, it->lo->twin = im;
    }
    it->tex.push_back(t);
    if (g_scene_lo && it->lo) t->host = it->lo;
    return static_cast<Image*>(t->host);
}
void scene_phase(bool lo) {
    if (lo == g_scene_lo) return;
    std::lock_guard<std::mutex> l(g_twin_lock);
    g_scene_lo = lo;
    for (Twin& w : g_twins)
        for (nvn::Texture* t : w.tex) t->host = lo && w.lo ? w.lo : w.hi;
}
bool scene_phase_lo() { return g_scene_lo; }
Image* upscale_hi(Image* lo) {
    std::lock_guard<std::mutex> l(g_twin_lock);
    for (Twin& w : g_twins) if (w.lo == lo) return w.hi;
    return nullptr;
}

static Image* make_image(nvn::Texture* t, VkFormat fmt, bool depth, bool stencil, float force_scale) {
    auto* im = new Image;
    im->format = fmt;
    im->aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                       : stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    // render targets (NVN marks them compressible) get res_scale() x the game's size; everything that addresses them
    // in pixels follows (viewports, scissors, copies, shader pixel coordinates through the support buffer)
    // except the small post-process targets (game size <= 640x360: depth-of-field and bloom chains). Their blurs take
    // a few taps at fixed UV spacing: from a 3x sharper source each tap shows its own copy, ringed ghost outlines
    // around characters (user report 2026-10-27, "white halo"). At the game's size they blur as on the Switch.
    // NX_POST_NATIVE=0 scales them too.
    static const bool post_native = !getenv("NX_POST_NATIVE") || atoi(getenv("NX_POST_NATIVE")) != 0;
    bool small_post = post_native && !depth && !stencil && t->width <= 640 && t->height <= 360;
    // NX_SHADOW_NATIVE=1 (debug, off by default): shadow maps (depth, power-of-two size: battle 1024x2048 + 1024x1024,
    // never screen sized) stay at the game's size. Tried for the battle face shadow (s50): at 1x the faces show fine
    // diagonal acne stripes, at 3x they are clean; no dark band on Itsuki in either -> scaled stays default.
    static const bool shadow_native = getenv("NX_SHADOW_NATIVE") && atoi(getenv("NX_SHADOW_NATIVE")) != 0;
    auto pow2 = [](uint32_t v) { return v >= 256 && !(v & (v - 1)); };
    bool shadow = shadow_native && depth && pow2(t->width) && pow2(t->height);
    static int n_shadow = 0;
    if (shadow && (t->flags & 8) && res_scale() > 1 && n_shadow++ < 4) log("vk: shadow map %ux%u kept at game size", t->width, t->height);
    im->scale = force_scale > 0 ? force_scale : (t->flags & 8) && t->target == 1 && !small_post && !shadow ? res_scale() : 1.f;
    im->w = (uint32_t)lround(std::max(1u, t->width) * im->scale);
    // ultrawide: 16:9 screen-size targets (the scene, its post chain, the window textures) get wider; shadow maps and other
    // shapes keep theirs
    if (wide_factor() > 1 && (t->flags & 8) && t->target == 1 && t->width >= 64 && t->width * 9 == t->height * 16) {
        im->w = (uint32_t)lround(t->width * wide_factor() * im->scale);
        im->sx = im->w / (t->width * im->scale);
        static int nw = 0;
        if (nw++ < 40) log("vk: widened %08x %ux%u fmt %d flags %x depth %d (vk fmt %d)", t->guest, t->width, t->height, (int)t->format, t->flags, (int)depth, (int)fmt);
    }
    im->h = (uint32_t)lround(std::max(1u, t->height) * im->scale);
    // NVN targets: 1D 0, 2D 1, 3D 2, 1D array 3, 2D array 4, 2D MS 5, 2D MS array 6, rectangle 7, cube 8, cube array 9
    bool array = t->target == 3 || t->target == 4 || t->target == 6 || t->target == 8 || t->target == 9;
    im->layers = array ? std::max(1u, t->depth) : 1;
    im->levels = std::max(1u, t->levels);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = t->target == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->w, im->h, t->target == 2 ? std::max(1u, t->depth) : 1};
    ci.mipLevels = im->levels;
    ci.arrayLayers = im->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(phys, im->format, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (t->target == 8 || t->target == 9) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VKC(vkCreateImage(dev, &ci, nullptr, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im->image, &req);
    VkDeviceSize off;
    VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &off);
    VKC(vkBindImageMemory(dev, im->image, m, off));
    im->mem = m, im->mem_off = off, im->mem_size = req.size;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im->image;
    vci.viewType = ci.imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                   : (t->target == 8) ? VK_IMAGE_VIEW_TYPE_CUBE
                   : array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = im->format;
    vci.subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, 0, im->levels, 0, im->layers};
    VKC(vkCreateImageView(dev, &vci, nullptr, &im->view));
    im->view_type = vci.viewType;
    if (force_scale > 0) return im;  // a lo twin: t->host stays the full-size image
    t->host = im;
    if ((depth || stencil) && t->gpu) {
        std::lock_guard<std::mutex> l(g_ds_lock);
        g_ds_by_gpu[t->gpu] = im;
    }
    static const bool log_images = getenv("NX_LOG_IMAGES") != nullptr;
    if (log_images) log("vk: image %08x %ux%u (x%g) layers %u levels %u nvn fmt %#x target %u flags %#x", t->guest, im->w, im->h, im->scale, im->layers, im->levels, t->format, t->target, t->flags);
    return im;
}

// The view a shader samples: NVN swizzle (view's, else the texture builder's; ZERO 0, ONE 1, R 2, G 3, B 4, A 5) and
// the view's target when the image allows it (fonts are BC4 registered as 2D arrays with swizzle ONE ONE ONE R).
// Texture packs (vk_texture.cpp): a sampled 2D image of the pack file's format, size and level count. Never freed
// (one per pack texture, shared by every texture with that hash).
Image::Own new_image_2d(VkFormat f, uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags extra) {
    Image::Own o{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra;
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VKC(vkCreateImage(dev, &ci, nullptr, &o.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, o.image, &req);
    VkDeviceSize off;
    VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &off);
    VKC(vkBindImageMemory(dev, o.image, m, off));
    o.mem = m, o.mem_off = off, o.mem_size = req.size;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = o.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = f;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    VKC(vkCreateImageView(dev, &vci, nullptr, &o.view));
    o.format = f, o.w = w, o.h = h, o.levels = levels;
    return o;
}

VkImageView sampled_view(nvn::Texture* t, Image* im, const nvn::TexView& v) {
    const uint32_t* s = (v.flags & 8) ? v.swizzle : t->swizzle;
    uint32_t target = (v.flags & 0x20) ? v.target : t->target;
    bool identity = s[0] == 2 && s[1] == 3 && s[2] == 4 && s[3] == 5;
    if (!s[0] && !s[1] && !s[2] && !s[3]) identity = true;  // builder never set
    VkImageViewType type = target == 2 ? VK_IMAGE_VIEW_TYPE_3D
                         : (target == 8 && im->layers >= 6) ? VK_IMAGE_VIEW_TYPE_CUBE
                         : (target == 9 && im->layers >= 6) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                         : (target == 3 || target == 4 || target == 6) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                         : im->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if ((t->target == 2) != (type == VK_IMAGE_VIEW_TYPE_3D)) return im->view;  // incompatible with the image: keep default
    // depth-stencil mode STENCIL: the view reads the stencil bits (the game's shadow maps keep character ids there and
    // a character's own id counts as lit, so it never shadows itself; read as depth, every face got self-shadow acne)
    const bool stencil = (v.has ? v.ds_mode : t->ds_mode) == 1 && (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (identity && type == im->view_type && !stencil) return im->view;
    uint64_t key = identity ? 0 : (uint64_t)(s[0] & 7) | (s[1] & 7) << 3 | (s[2] & 7) << 6 | (s[3] & 7) << 9;
    key |= (uint64_t)type << 16 | (uint64_t)stencil << 24;
    auto it = im->views.find(key);
    if (it != im->views.end()) return it->second;
    static const VkComponentSwizzle sw[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im->image;
    vci.viewType = type;
    vci.format = im->format;
    if (!identity) vci.components = {sw[s[0] % 6], sw[s[1] % 6], sw[s[2] % 6], sw[s[3] % 6]};
    bool depth = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    uint32_t layers = type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : type == VK_IMAGE_VIEW_TYPE_2D ? 1 : im->layers;
    vci.subresourceRange = {stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : depth ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect, 0,
                            im->levels, 0, layers};
    VkImageView view;
    VKC(vkCreateImageView(dev, &vci, nullptr, &view));
    im->views[key] = view;
    return view;
}

void transition(VkCommandBuffer cb, Image* im, VkImageLayout to) {
    if (im->layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    im->layout = to;
}

}  // namespace vk

// ===================================================================================================== replay
using namespace vk;

namespace nvn {

void replay(VkCommandBuffer cb, const std::vector<Cmd>& cmds);  // vk_replay.cpp
void replay(VkCommandBuffer cb, const std::vector<const Cmd*>& cmds);  // same, a flattened list by pointer
void descriptors_reset(int slot);
void pass_timing_slot(VkCommandBuffer cb, int slot);  // NX_GPU_PASSES (vk_replay.cpp)
void pass_timing_report(uint64_t frames);

void backend_init() {
    std::lock_guard<std::mutex> l(g_vk_lock);
    if (!dev) init();
}

void backend_texture_created(Texture*) {}

// Finalized textures (and ones the game initializes again in place): the image, its views and memory go once the GPU
// is past every submit made so far (+1 for the one being recorded). Before 2026-10-29 none were ever freed: every
// texture of every map, battle and menu stayed in VRAM. NX_FREE_IMAGES=0 keeps them (old behaviour, A/B).
static std::mutex g_dead_lock;
static std::vector<std::pair<uint64_t, Image*>> g_dead;
static std::atomic<uint64_t> g_images_freed{0}, g_image_bytes_freed{0};
void backend_texture_finalized(Texture* t) {
    static const bool keep = getenv("NX_FREE_IMAGES") && !atoi(getenv("NX_FREE_IMAGES"));
    if (!t || !t->host || keep) return;
    {
        std::lock_guard<std::mutex> l(g_twin_lock);
        for (auto w = g_twins.begin(); w != g_twins.end(); ++w) {
            auto i = std::find(w->tex.begin(), w->tex.end(), t);
            if (i == w->tex.end()) continue;
            t->host = w->hi;
            w->tex.erase(i);
            if (w->tex.empty()) {
                if (w->lo) {
                    w->hi->twin = w->lo->twin = nullptr;
                    std::lock_guard<std::mutex> l2(g_dead_lock);
                    g_dead.push_back({vk::gpu_recording_serial() + 1, w->lo});
                }
                g_twins.erase(w);
            }
            break;
        }
    }
    Image* im = static_cast<Image*>(t->host);
    t->host = nullptr;
    {
        std::lock_guard<std::mutex> l(g_ds_lock);
        if (--im->share) return;
        auto it = g_ds_by_gpu.find(t->gpu);
        if (it != g_ds_by_gpu.end() && it->second == im) g_ds_by_gpu.erase(it);
    }
    std::lock_guard<std::mutex> l(g_dead_lock);
    g_dead.push_back({vk::gpu_recording_serial() + 1, im});
}
static void destroy_views(std::map<uint64_t, VkImageView>& v) {
    for (auto& [k, view] : v) vkDestroyImageView(dev, view, nullptr);
    v.clear();
}
struct Loose { uint64_t serial; VkImage image; VkImageView view; std::map<uint64_t, VkImageView> views; VkDeviceMemory mem; VkDeviceSize off, size; };
static std::vector<Loose> g_loose;  // g_dead_lock
}  // namespace nvn
void vk::defer_destroy(VkImage image, VkImageView view, std::map<uint64_t, VkImageView> views, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size) {
    std::lock_guard<std::mutex> l(nvn::g_dead_lock);
    nvn::g_loose.push_back({vk::gpu_recording_serial() + 1, image, view, std::move(views), mem, off, size});
}
// frame end (upscale_frame_end, before the next frame's phase): the upscaling setting changed -> all targets back to full
// size, old lo images freed once the GPU is past them, new ones made at the new scale (none when off)
void vk::auto_scene_res(bool scene_frame) {
    static uint32_t frames = 0, over = 0, under = 0, skip = 0, up_need = 6, since_up = ~0u;
    static uint64_t acc = 0;
    static bool told = false;
    const uint64_t ns = g_frame_gpu_ns.exchange(0);
    if (scene_frame) rt::g_perf_in_game = true;
    if (rt::scene_res() != 0 || rt::fsr3_setting() || !spatial_upscale_ok()) { frames = over = under = 0, acc = 0; return; }
    if (!g_ts_pool) {
        if (!told) told = true, log("vk: Auto 3D resolution: this GPU has no frame timers, it stays at 100%%");
        return;
    }
    if (!scene_frame) return;
    acc += ns;
    if (++frames < 30) return;
    const double ms = acc / 1e6 / frames;
    acc = 0, frames = 0;
    if (since_up != ~0u) since_up++;
    if (skip) { skip--; return; }  // the first window after a step mixes both sizes
    static const double forced = getenv("NX_AUTO_RES_BUDGET") ? atof(getenv("NX_AUTO_RES_BUDGET")) : 0;  // debug: budget in ms
    const int fps = rt::fps_setting();
    const double budget = forced > 0 ? forced : 1000.0 / (fps >= 30 ? fps : 60);
    over = ms > budget * 0.9 ? over + 1 : 0;
    const int cur = k_auto_steps[g_auto_step], up = g_auto_step ? k_auto_steps[g_auto_step - 1] : cur;
    under = g_auto_step && ms * (up * up) / (cur * cur) < budget * 0.8 ? under + 1 : 0;
    int step = g_auto_step;
    if (over >= 2 && g_auto_step < 4) {
        step++;
        if (since_up < 20) up_need = std::min(up_need * 2, 48u);  // a step up taken back within ~10 s: wait longer next time
    } else if (under >= up_need) step--, since_up = 0;
    if (step == g_auto_step) return;
    log("vk: Auto 3D resolution %d%% -> %d%% (GPU %.2f ms per 3D frame, budget %.2f ms)", cur, k_auto_steps[step], ms, budget);
    g_auto_step = step, over = under = 0, skip = 1;
}
void vk::upscale_apply_setting() {
    const float want = lo_for_setting();
    if (want == upscale_lo_scale()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> l(g_twin_lock);
    g_scene_lo = false;
    for (Twin& w : g_twins) {
        for (nvn::Texture* t : w.tex) t->host = w.hi;
        if (w.lo) {
            w.hi->twin = w.lo->twin = nullptr;
            std::lock_guard<std::mutex> l2(nvn::g_dead_lock);
            nvn::g_dead.push_back({vk::gpu_recording_serial() + 1, w.lo});
            w.lo = nullptr;
        }
        if (want > 0) {
            const bool depth = (w.hi->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, stencil = (w.hi->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            w.lo = make_image(w.tex[0], w.hi->format, depth, stencil, want);
            w.hi->twin = w.lo, w.lo->twin = w.hi;
        }
    }
    g_lo = want;
    log_lo(want);
    log("vk: 3D resolution images rebuilt in %.2f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}
namespace nvn {
static void free_dead_images() {  // g_vk_lock held
    std::vector<Image*> done;
    std::vector<Loose> loose;
    {
        std::lock_guard<std::mutex> l(g_dead_lock);
        for (size_t i = 0; i < g_dead.size();)
            if (vk::gpu_wait(g_dead[i].first, 0)) { done.push_back(g_dead[i].second); g_dead[i] = g_dead.back(); g_dead.pop_back(); }
            else i++;
        for (size_t i = 0; i < g_loose.size();)
            if (vk::gpu_wait(g_loose[i].serial, 0)) { loose.push_back(std::move(g_loose[i])); g_loose[i] = std::move(g_loose.back()); g_loose.pop_back(); }
            else i++;
    }
    for (Loose& o : loose) {
        destroy_views(o.views);
        if (o.view) vkDestroyImageView(dev, o.view, nullptr);
        if (o.image) vkDestroyImage(dev, o.image, nullptr);
        if (o.mem) free_mem(o.mem, o.off, o.size);
        g_image_bytes_freed += o.size;
    }
    for (Image* im : done) {
        ms_image_dead(im, im->image);  // its MSAA copy goes too (vk_replay.cpp ms_prune)
        if (im->replaced) pack_released(im);
        destroy_views(im->views);  // a pack-replaced image's views are its own views of the shared pack image
        if (im->replaced) {        // im->image / view are the pack's (shared per hash, kept)
            vkDestroyImageView(dev, im->own->view, nullptr);
            destroy_views(im->own->views);
            vkDestroyImage(dev, im->own->image, nullptr);
        } else {
            vkDestroyImageView(dev, im->view, nullptr);
            vkDestroyImage(dev, im->image, nullptr);
        }
        if (im->mem) free_mem(im->mem, im->mem_off, im->mem_size);
        g_images_freed++;
        g_image_bytes_freed += im->mem_size;
        delete im->own;
        delete im;
    }
}

}  // namespace nvn

namespace vk {
uint64_t gpu_last_submitted() { return g_serial; }
uint64_t gpu_recording_serial() { return g_serial + 1; }
bool gpu_wait(uint64_t serial, uint64_t timeout_ns) {
    if (!serial || !g_timeline) return true;
    uint64_t done = 0;
    if (vkGetSemaphoreCounterValue(dev, g_timeline, &done) == VK_SUCCESS && done >= serial) return true;
    if (!timeout_ns) return false;
    HitchTimer ht{t_present_thread ? HITCH_OTHER_THREAD : HITCH_GPU_WAIT};  // present_thread waits are not game time
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g_timeline, &serial};
    VkResult r = vkWaitSemaphores(dev, &wi, timeout_ns);
    if (r != VK_SUCCESS && r != VK_TIMEOUT) VKC(r);
    return r == VK_SUCCESS;
}
}  // namespace vk

namespace nvn {

// next slot: wait until its previous submit finished, then reuse its command buffer, descriptor pools and upload ring
uint64_t g_cb_begins = 0;  // command buffers begun (vk_replay.cpp draw(): a new one has no graphics state yet)
static void begin() {
    g_cb_begins++;
    g_slot = (g_slot + 1) % 3;
    Slot& sl = g_slots[g_slot];
    { HitchTimer ht{HITCH_SLOT_WAIT}; gpu_wait(sl.serial); }  // also in HITCH_GPU_WAIT: the rest of that is nvnSyncWait
    free_dead_images();
    nvn::descriptors_reset(g_slot);
    staging_reset(g_slot);
    g_cb = sl.cb;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(vkResetCommandBuffer(g_cb, 0));
    VKC(vkBeginCommandBuffer(g_cb, &bi));
    if (g_ts_pool) {  // the slot's previous submit is done (waited above): add its GPU time
        uint64_t ts[2];
        if (g_ts_written[g_slot] && vkGetQueryPoolResults(dev, g_ts_pool, 2 * g_slot, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
            const double ns = (ts[1] - ts[0]) * (double)props.limits.timestampPeriod;
            g_gpu_busy_ns_acc += ns;
            g_frame_gpu_ns += (uint64_t)ns;
        }
        vkCmdResetQueryPool(g_cb, g_ts_pool, 2 * g_slot, 2);
        vkCmdWriteTimestamp(g_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, 2 * g_slot);
        g_ts_written[g_slot] = false;
    }
    pass_timing_slot(g_cb, g_slot);
}
// submit the slot's command buffer (signals the next serial); returns the serial
static uint64_t end_and_submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE) {
    if (g_ts_pool) vkCmdWriteTimestamp(g_cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, 2 * g_slot + 1), g_ts_written[g_slot] = true;
    VKC(vkEndCommandBuffer(g_cb));
    uint64_t serial = g_serial + 1;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore sig[2] = {g_timeline, signal};
    uint64_t sig_val[2] = {serial, 0};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ti.signalSemaphoreValueCount = signal ? 2 : 1;
    ti.pSignalSemaphoreValues = sig_val;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cb;
    if (wait) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait; si.pWaitDstStageMask = &st; }
    si.signalSemaphoreCount = signal ? 2 : 1;
    si.pSignalSemaphores = sig;
    VKC(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    g_slots[g_slot].serial = serial;
    g_serial = serial;
    return serial;
}
static void end_and_wait(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE) {
    gpu_wait(end_and_submit(wait, signal));
    stats_flush();
}

extern int g_log_draws;

// NX_SYNC_SUBMIT=1: wait for every submit as before (A/B test of the asynchronous path)
// Where replay time goes (frames line "submit parts", Linux): waiting for g_vk_lock, begin() (slot wait), replay wall
// time and the thread's own CPU time in it (the rest of replay is time blocked inside the driver), vkQueueSubmit.
static std::atomic<uint64_t> g_part_us[5];  // lock, begin, replay wall, replay cpu, queue submit
static uint64_t thread_cpu_us() {
#ifdef _WIN32
    return 0;
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}
static uint64_t now_us() { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// OP_CALL (nvn_hle.cpp CallCommands, s117): the called list, expanded here on the render thread as pointers to the
// commands (s119: copying each ~80-byte Cmd was ~12% of the render thread at the Shibuya crosswalk); the submit job
// keeps the lists, and they keep the lists they call, so the pointers stay valid through the replay.
static void flatten(const std::vector<Cmd>& in, std::vector<const Cmd*>& out) {
    for (const Cmd& k : in) {
        if (k.op == OP_CALL) {
            flatten(*(const std::vector<Cmd>*)(uintptr_t)((uint64_t)k.a[1] << 32 | k.a[0]), out);
            continue;
        }
        out.push_back(&k);
    }
}
void backend_submit(const std::vector<const std::vector<Cmd>*>& lists) {
    const uint64_t t0 = now_us();
    std::lock_guard<std::mutex> l(g_vk_lock);
    if (!dev) return;
    rt::profile_start(true);  // NX_PROFILE=2
    HitchTimer ht{HITCH_SUBMIT};
    static const bool sync = getenv("NX_SYNC_SUBMIT") != nullptr;
    const uint64_t t1 = now_us();
    begin();
    g_submit_serial++;
    const uint64_t t2 = now_us(), c2 = thread_cpu_us();
    static std::vector<const Cmd*> flat;  // g_vk_lock held
    for (auto* cmds : lists) {
        if (std::none_of(cmds->begin(), cmds->end(), [](const Cmd& k) { return k.op == OP_CALL; })) { replay(g_cb, *cmds); continue; }
        flat.clear();
        flatten(*cmds, flat);
        replay(g_cb, flat);
    }
    const uint64_t t3 = now_us(), c3 = thread_cpu_us();
    if (sync || g_log_draws) end_and_wait();  // drawlog statistics are read after the fence
    else end_and_submit();
    const uint64_t t4 = now_us();
    g_part_us[0] += t1 - t0, g_part_us[1] += t2 - t1, g_part_us[2] += t3 - t2, g_part_us[3] += c3 - c2, g_part_us[4] += t4 - t3;
}

// NX_SHOT_EVERY=<n>: write every n-th presented frame to <data>/shots/frame_NNNNN.bmp (for unattended checks)
// The launcher's Play page picture (user 11-23): the title screen of the player's own game, captured once ~6 s after the
// title music starts (hle_sys.cpp game log) when <data> has no launcher_hero.* yet. Nothing of the game ships with us.
static std::atomic<int> g_hero_shot{0};
}  // namespace nvn
void rt::window_resized() { vk::window_resized(); }
void rt::hero_capture_soon() {
    static std::atomic<bool> once{false};
    if (once.exchange(true) || getenv("NX_NO_HERO")) return;
    for (const char* n : {"/launcher_hero.png", "/launcher_hero.jpg", "/launcher_hero.bmp"})
        if (GetFileAttributesA((rt::g_data_dir + n).c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::thread([] { Sleep(6000); nvn::g_hero_shot = 1; }).detach();
}
namespace nvn {

// out: write there instead of <data>/shots (via <out>.part, so a reader never sees half a file)
static void dump_frame(Image* src, uint64_t n, const char* tag = "frame", const char* out = nullptr) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)src->w * src->h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    static VkBuffer buf = VK_NULL_HANDLE;
    static void* map = nullptr;
    static VkDeviceSize cap = 0;
    if (bci.size > cap) {
        VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        VKC(vkAllocateMemory(dev, &ai, nullptr, &m));
        VKC(vkBindBufferMemory(dev, buf, m, 0));
        VKC(vkMapMemory(dev, m, 0, req.size, 0, &map));
        cap = bci.size;
    }
    begin();
    transition(g_cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {src->w, src->h, 1};
    vkCmdCopyImageToBuffer(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    end_and_wait();
    std::string dir = g_data_dir + "/shots";
    CreateDirectoryA(dir.c_str(), nullptr);
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%05llu.bmp", dir.c_str(), tag, (unsigned long long)n);
    uint32_t w = src->w, h = src->h, sz = w * h * 4;
    uint8_t hdr[54] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
    put(2, 54 + sz); put(10, 54); put(14, 40); put(18, w); put(22, (uint32_t)-(int32_t)h);
    hdr[26] = 1; hdr[28] = 32; put(34, sz);
    std::vector<uint8_t> px((uint8_t*)map, (uint8_t*)map + sz);
    if (src->format == VK_FORMAT_R8G8B8A8_UNORM || src->format == VK_FORMAT_R8G8B8A8_SRGB)
        for (uint32_t i = 0; i < sz; i += 4) std::swap(px[i], px[i + 2]);  // RGBA -> BGRA
    bool hdr_rgb = src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;  // scene colour: decode, clamp to 0..1
    float top = 0;  // brightest channel and how many pixels go over 1 (the part an SDR screen clips; HDR keeps it)
    uint32_t over = 0;
    if (hdr_rgb)
        for (uint32_t i = 0; i < sz; i += 4) {
            uint32_t v;
            memcpy(&v, &px[i], 4);
            auto uf = [](uint32_t bits, int mant) {  // unsigned small float: 5-bit exponent, bias 15
                uint32_t e = bits >> mant, m = bits & ((1u << mant) - 1);
                return e ? ldexpf(1 + m / float(1u << mant), (int)e - 15) : ldexpf(m / float(1u << mant), -14);
            };
            float r = uf(v & 0x7ff, 6), g = uf((v >> 11) & 0x7ff, 6), b = uf(v >> 22, 5);
            const float mx = std::max(r, std::max(g, b));
            top = std::max(top, mx);
            over += mx > 1;
            px[i] =(uint8_t)std::min(255.f, b * 255), px[i + 1] = (uint8_t)std::min(255.f, g * 255);
            px[i + 2] = (uint8_t)std::min(255.f, r * 255), px[i + 3] = 255;
        }
    static const bool lum = getenv("NX_SHOT_LUM") && atoi(getenv("NX_SHOT_LUM"));  // debug: log mean brightness, no file
    if (lum && !out) {
        uint64_t sum = 0, cnt = 0;
        for (uint32_t i = 0; i < sz; i += 4 * 61) sum += px[i] + px[i + 1] + px[i + 2], cnt += 3;
        log("shot lum %s %llu %.1f", tag, (unsigned long long)n, cnt ? double(sum) / cnt : 0.0);
        return;
    }
    if (out) snprintf(path, sizeof path, "%s.part", out);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, 54, f);
    fwrite(px.data(), 1, sz, f);
    fclose(f);
    if (out && !MoveFileExA(path, out, MOVEFILE_REPLACE_EXISTING)) return (void)DeleteFileA(path);
    if (out) snprintf(path, sizeof path, "%s", out);
    if (hdr_rgb) log("vk: %s float target: brightest %.2f, %.2f%% of pixels over 1", path, top, 100.0 * over / (w * h));
    log("vk: wrote %s (%s)", path, hdr_rgb || src->format == VK_FORMAT_R8G8B8A8_UNORM || src->format == VK_FORMAT_R8G8B8A8_SRGB || src->format == VK_FORMAT_B8G8R8A8_UNORM ? "8-bit" : "raw");
}

// ------------------------------------------------------------------------------------------------- async present
// The game thread copies each finished frame into one of 3 staging images and returns; present_thread acquires a
// swapchain image, scales the newest staged frame into it on the second queue and presents. The display (DWM, mailbox)
// can hold an image for a long time before releasing it; that wait now blocks only present_thread. Mailbox rule: a
// frame staged while the previous one is still pending replaces it.
struct Staged {
    Image img;
    uint64_t tick = 0;  // vsync tick to show it on: the tick after the game released the frame
    uint64_t serial = 0;  // GPU serial of the copy into it (present_thread waits for it)
    enum { FREE, PENDING, BUSY } state = FREE;  // BUSY: being staged (game thread) or presented (present_thread)
};
static Staged g_staged[3];
static int g_pending = -1;
static std::mutex g_pres_lock;
static std::condition_variable g_pres_cv;

static void make_staging(Image& im, uint32_t w, uint32_t h, VkFormat fmt) {
    // the old image goes once the GPU is past it (frame size change; HDR on / off switches the format live)
    if (im.image) defer_destroy(im.image, im.view, {}, im.mem, im.mem_off, im.mem_size);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // the sharpening pass writes rgba8 (sharpen_spv.h), the HDR pass rgba16f (hdr_spv.h)
    const bool storage = fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;  // sampled: upscale -> sharpen
    VKC(vkCreateImage(dev, &ci, nullptr, &im.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im.image, &req);
    VkDeviceSize off;
    VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &off);
    VKC(vkBindImageMemory(dev, im.image, m, off));
    im.mem = m, im.mem_off = off, im.mem_size = req.size;
    im.view = VK_NULL_HANDLE;
    if (storage) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = im.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = fmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKC(vkCreateImageView(dev, &vci, nullptr, &im.view));
    }
    im.format = fmt;
    im.w = w;
    im.h = h;
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// The finished picture into the swapchain image at its own shape: a window of another shape (21:9 screen with the 16:9
// setting, Steam Deck 16:10) gets black bars instead of a stretched picture. Within 1% of the window shape: whole window.
static void fit_blit(VkCommandBuffer cb, VkImage dst, uint32_t sw, uint32_t sh, VkImageBlit& b) {
    const uint32_t dw = g_swap_extent.width, dh = g_swap_extent.height;
    const double s = std::min((double)dw / sw, (double)dh / sh);
    uint32_t w = (uint32_t)lround(sw * s), h = (uint32_t)lround(sh * s);
    if (w * 100 >= dw * 99 && h * 100 >= dh * 99) w = dw, h = dh;
    const int32_t x = (int32_t)(dw - w) / 2, y = (int32_t)(dh - h) / 2;
    if (w != dw || h != dh) {
        static const VkClearColorValue black{};
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
        // the clear must finish before the blit writes the picture: without this barrier AMD runs both at once and
        // late clear tiles black out parts of the picture (issue #8: stepped black blocks at the top, 16:10 screen)
        VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.oldLayout = mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image = dst;
        mb.subresourceRange = r;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &mb);
    }
    b.dstOffsets[0] = {x, y, 0};
    b.dstOffsets[1] = {x + (int32_t)w, y + (int32_t)h, 1};
}
static void present_thread() {
    t_present_thread = true;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // its wake-up time is the frame's display time
#endif
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VKC(vkCreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    VKC(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFence fence, acq_fence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKC(vkCreateFence(dev, &fci, nullptr, &fence));
    VKC(vkCreateFence(dev, &fci, nullptr, &acq_fence));
    // GitHub issue #3 (GeForce 920MX laptop, GPU-bound ~18 fps): the game ran, sound played, but only 2-8 frames per 10 s
    // reached the screen (gaps of 7-79 s). A game queue that keeps the GPU full can starve this second queue on some
    // GPUs / drivers. When one present's blit waits over 1 s, present on the game's own queue from then on (g_vk_lock
    // around its submits; frames then wait behind the game's queued work). NX_PRESENT_GAME_QUEUE=1 forces it (tests).
    bool on_game_queue = getenv("NX_PRESENT_GAME_QUEUE") != nullptr;
    if (on_game_queue) log("vk: presenting on the game's queue (NX_PRESENT_GAME_QUEUE)");
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    auto queue_idle = [&] {
        std::unique_lock<std::mutex> vl(g_vk_lock, std::defer_lock);
        if (on_game_queue) vl.lock();
        vkQueueWaitIdle(on_game_queue ? queue : g_present_queue);
    };
    int slow_logs = 0;
    for (;;) {
        int i;
        {
            std::unique_lock<std::mutex> l(g_pres_lock);
            g_pres_cv.wait(l, [] { return g_pending >= 0; });
            i = g_pending;
            g_pending = -1;
            g_staged[i].state = Staged::BUSY;
        }
        g_pres_cv.notify_all();  // vk_wait_present_taken (game thread)
        Image* src = &g_staged[i].img;
        auto t_take = std::chrono::steady_clock::now();
        gpu_wait(g_staged[i].serial);  // the game queue's copy into the staging image
        const double copy_ms = ms_since(t_take);
        // fixed cadence: staging can finish anywhere up to ~15 ms after the frame's tick (the game queue is shared
        // with the next frame's work), so show every frame exactly one tick after release
        const bool asap = getenv("NX_PRESENT_ASAP") || rt::fps_setting() == 0 || rt::fps_setting() > 60;
        auto t_vs = std::chrono::steady_clock::now();
        if (!asap) rt::wait_vsync_tick(g_staged[i].tick);
        const double vsync_ms = ms_since(t_vs);
        auto t_tick = std::chrono::steady_clock::now();
        if (rt::hdr_setting() != g_hdr_req || g_swap_resize.exchange(false)) {  // HDR switched in the F1 menu / window resized
            queue_idle();
            create_swapchain();
        }
        if ((src->format == VK_FORMAT_R16G16B16A16_SFLOAT) != (g_hdr_mode != 0)) {  // staged for the other mode: drop it
            std::lock_guard<std::mutex> l(g_pres_lock);
            g_staged[i].state = Staged::FREE;
            continue;
        }
        uint32_t idx;
        // acquire with a fence waited here on the CPU: a GPU-side semaphore wait on the display's release stalled the
        // game's queue too (both queues share the hardware), up to ~30 ms
        VKC(vkResetFences(dev, 1, &acq_fence));
        VkResult r = vkAcquireNextImageKHR(dev, g_swap, UINT64_MAX, VK_NULL_HANDLE, acq_fence, &idx);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) VKC(vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, UINT64_MAX));
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            queue_idle();
            create_swapchain();
        } else {
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VKC(vkResetCommandBuffer(cb, 0));
            VKC(vkBeginCommandBuffer(cb, &bi));
            Image dst;
            dst.image = g_swap_images[idx];
            transition(cb, &dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = {(int32_t)src->w, (int32_t)src->h, 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            fit_blit(cb, dst.image, src->w, src->h, b);
            vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
            transition(cb, &dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            VKC(vkEndCommandBuffer(cb));
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            (void)st;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &g_render_sem;
            VKC(vkResetFences(dev, 1, &fence));
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &g_render_sem;
            pi.swapchainCount = 1;
            pi.pSwapchains = &g_swap;
            pi.pImageIndices = &idx;
            {
                std::unique_lock<std::mutex> vl(g_vk_lock, std::defer_lock);
                if (on_game_queue) vl.lock();
                VkQueue pq = on_game_queue ? queue : g_present_queue;
                VKC(vkQueueSubmit(pq, 1, &si, fence));
                r = vkQueuePresentKHR(pq, &pi);
            }
            {  // display cadence every 10 s: gaps between presents (vsync ticks between the frames x 16.7 ms each)
                using clk = std::chrono::steady_clock;
                static clk::time_point win = clk::now(), prev = win;
                static double worst = 0, best = 1e9;
                static uint32_t n = 0, off = 0;
                static uint64_t prev_tick = 0;
                clk::time_point now = clk::now();
                double gap = std::chrono::duration<double, std::milli>(now - prev).count();
                double want = (g_staged[i].tick - prev_tick) * 1000.0 / 60;
                prev = now;
                prev_tick = g_staged[i].tick;
                worst = std::max(worst, gap);
                best = std::min(best, gap);
                if (std::abs(gap - want) > 4) off++;  // more than 4 ms off the tick spacing
                static double tick_to_present = 0;
                tick_to_present = std::max(tick_to_present, std::chrono::duration<double, std::milli>(now - t_tick).count());
                n++;
                if (std::chrono::duration<double>(now - win).count() >= 10) {
                    log("present: %u in 10 s, gaps %.1f..%.1f ms, %u off-cadence, tick to present max %.1f ms", n, best, worst,
                        off, tick_to_present);
                    win = now; n = off = 0; worst = 0; best = 1e9; tick_to_present = 0;
                }
            }
            auto t_blit = std::chrono::steady_clock::now();
            VKC(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
            const double blit_ms = ms_since(t_blit);
            if (copy_ms + vsync_ms + blit_ms > 500 && slow_logs < 20) {  // where a slow present waited (issue #3 logs)
                slow_logs++;
                log("present: slow frame: staging copy %.0f ms, vsync wait %.0f ms, blit %.0f ms%s", copy_ms, vsync_ms, blit_ms,
                    on_game_queue ? " (game queue)" : "");
            }
            if (!on_game_queue && blit_ms > 1000) {
                on_game_queue = true;
                log("vk: the present queue waited %.0f ms for the GPU (starved by the game's queue): presenting on the game's queue from now on", blit_ms);
            }
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
                queue_idle();
                create_swapchain();
            }
        }
        std::lock_guard<std::mutex> l(g_pres_lock);
        g_staged[i].state = Staged::FREE;
    }
}

// game thread side: copy the frame into a staging image and hand it to present_thread (g_vk_lock held)
// Sharpening (setting sharpen 0-100, settings menu): a compute pass from the game's frame into the staging image in
// place of the copy (contrast-adaptive, tools/spvgen/Sharpen.cs -> sharpen_spv.h). Only for an R8G8B8A8_UNORM frame
// (the game's); anything else is copied as before.
struct SharpenPass { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSet sets[4]; };  // set per staging slot + shots
// a compute pass from a sampled image (binding 0) into a storage image (binding 1) with push constants
static SharpenPass* image_pass(const uint32_t* spv, size_t spv_size, uint32_t pc_size) {
    auto* p = new SharpenPass{};
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, spv_size, spv};
    VkShaderModule mod;
    VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, b};
    VkDescriptorSetLayout dsl;
    VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pr};
    VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &p->layout));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
    ci.layout = p->layout;
    VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipe));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 4, 2, ps};
    VkDescriptorPool pool;
    VKC(vkCreateDescriptorPool(dev, &dpi, nullptr, &pool));
    VkDescriptorSetLayout l4[4] = {dsl, dsl, dsl, dsl};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 4, l4};
    VKC(vkAllocateDescriptorSets(dev, &ai, p->sets));
    return p;
}
static SharpenPass* sharpen_pass() {
    static SharpenPass* sp = [] {
        auto* p = image_pass(k_sharpen_spv, sizeof k_sharpen_spv, 12);
        log("vk: sharpening pass ready");
        return p;
    }();
    return sp;
}
// bind src (sampled) + dst (storage) in set `slot` of pass p and dispatch over w x h
static void run_image_pass(SharpenPass* p, int slot, Image* src, Image* dst, const void* pc, uint32_t pc_size, uint32_t w, uint32_t h) {
    transition(g_cb, src, VK_IMAGE_LAYOUT_GENERAL);
    transition(g_cb, dst, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorImageInfo ii[2] = {{VK_NULL_HANDLE, src->view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dst->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, p->sets[slot], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[0]},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, p->sets[slot], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[1]}};
    vkUpdateDescriptorSets(dev, 2, wr, 0, nullptr);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &p->sets[slot], 0, nullptr);
    vkCmdPushConstants(g_cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
    vkCmdDispatch(g_cb, (w + 7) / 8, (h + 7) / 8, 1);
}
static bool sharpen(int slot, Image* src, Image& st) {
    int pct = rt::sharpen_setting();
    if (pct <= 0 || !st.view || src->format != VK_FORMAT_R8G8B8A8_UNORM || !src->view) return false;
    struct { int32_t w, h; float peak; } pc{(int32_t)src->w, (int32_t)src->h, -1.0f / (8.0f - 3.0f * std::min(pct, 100) / 100.0f)};
    run_image_pass(sharpen_pass(), slot, src, &st, &pc, sizeof pc, src->w, src->h);
    return true;
}

// Upscaler (setting upscaler, 1 = FSR 1): when the game renders below the window size, an edge-adaptive compute pass
// (tools/spvgen/Upscale.cs -> upscale_spv.h) scales the frame to the window size into the staging image; sharpening
// then runs at the window size (the RCAS half of FSR 1). The present blit is 1:1 then. Off: the present blit scales
// (bilinear) as before.
static bool upscale_wanted(Image* src) {
    return rt::upscaler_setting() == 1 && src->format == VK_FORMAT_R8G8B8A8_UNORM && src->view &&
           (src->w < g_swap_extent.width || src->h < g_swap_extent.height);
}
static void upscale(int slot, Image* src, Image& st) {
    static SharpenPass* up = [] {
        auto* p = image_pass(k_upscale_spv, sizeof k_upscale_spv, 16);
        log("vk: upscaling pass ready (FSR 1 style)");
        return p;
    }();
    static Image tmp[4];  // the upscaled frame before sharpening, per slot
    Image* out = &st;
    const bool sharp = rt::sharpen_setting() > 0;
    if (sharp) {
        if (tmp[slot].w != st.w || tmp[slot].h != st.h) make_staging(tmp[slot], st.w, st.h, st.format);
        out = &tmp[slot];
    }
    int32_t pc[4] = {(int32_t)src->w, (int32_t)src->h, (int32_t)st.w, (int32_t)st.h};
    run_image_pass(up, slot, src, out, pc, sizeof pc, st.w, st.h);
    if (!sharp) return;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    sharpen(slot, out, st);
}

// Voice subtitles (subs.cpp): a compute pass blends the subtitle bitmap over the staging image in place
// (tools/spvgen/Overlay.cs -> overlay_spv.h). The bitmap goes through a host-visible buffer per staging slot, written
// only when the bitmap changes. shortcut: a slot reused while still pending (mailbox overwrite) may have its buffer
// rewritten while the GPU reads it: at worst one torn subtitle frame at a change; fence per slot if it ever shows.
struct OverlayPass {
    VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSet sets[4];  // per staging slot + shots, like the sharpening
    VkBuffer buf[4] = {}; VkDeviceMemory mem[4] = {}; uint32_t* map[4] = {}; VkDeviceSize cap[4] = {};
    std::shared_ptr<const rt::SubsBitmap> shown[4];
};
static OverlayPass* overlay_pass() {
    static OverlayPass* op = [] {
        auto* p = new OverlayPass{};
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_overlay_spv, k_overlay_spv};
        VkShaderModule mod;
        VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
        VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, b};
        VkDescriptorSetLayout dsl;
        VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pr};
        VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &p->layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
        ci.layout = p->layout;
        VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipe));
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 4, 2, ps};
        VkDescriptorPool pool;
        VKC(vkCreateDescriptorPool(dev, &dpi, nullptr, &pool));
        VkDescriptorSetLayout l4[4] = {dsl, dsl, dsl, dsl};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 4, l4};
        VKC(vkAllocateDescriptorSets(dev, &ai, p->sets));
        log("vk: subtitle overlay pass ready");
        return p;
    }();
    return op;
}
// the subtitle bitmap in slot's host-visible buffer (written when it changed); null when larger than the frame
static VkBuffer subs_buffer(int slot, const Image& st, const std::shared_ptr<const rt::SubsBitmap>& b) {
    OverlayPass* op = overlay_pass();
    const VkDeviceSize need = (VkDeviceSize)b->w * b->h * 4;
    // s114: a 4K P5 panel + a notice took ~18 MB and neither was drawn. The buffer now holds a whole frame; a bigger frame
    // (resolution change) gets a new one. ponytail: the old one is leaked (an in-flight frame may still read it), once per change
    if (op->buf[slot] && need > op->cap[slot] && need <= (VkDeviceSize)st.w * st.h * 4) op->buf[slot] = VK_NULL_HANDLE, op->shown[slot] = nullptr;
    if (!op->buf[slot]) {  // dedicated memory (the shared blocks may already be mapped elsewhere)
        const VkDeviceSize cap = std::max<VkDeviceSize>(16ull << 20, (VkDeviceSize)st.w * st.h * 4);  // the whole frame fits
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &op->buf[slot]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, op->buf[slot], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                 mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKC(vkAllocateMemory(dev, &mai, nullptr, &op->mem[slot]));
        VKC(vkBindBufferMemory(dev, op->buf[slot], op->mem[slot], 0));
        void* m;
        VKC(vkMapMemory(dev, op->mem[slot], 0, cap, 0, &m));
        op->map[slot] = (uint32_t*)m, op->cap[slot] = cap;
    }
    if (need > op->cap[slot]) return VK_NULL_HANDLE;  // larger than the frame: not drawn
    if (op->shown[slot] != b) memcpy(op->map[slot], b->rgba.data(), need), op->shown[slot] = b;
    return op->buf[slot];
}
static void overlay_subs(int slot, Image& st, std::shared_ptr<const rt::SubsBitmap> b) {
    if (!b || !st.view) return;
    OverlayPass* op = overlay_pass();
    const VkDeviceSize need = (VkDeviceSize)b->w * b->h * 4;
    if (!subs_buffer(slot, st, b)) return;
    // after the copy or the sharpening pass into st
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    ib.oldLayout = st.layout;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = st.image;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    st.layout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo bi{op->buf[slot], 0, need};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, st.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, op->sets[slot], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, op->sets[slot], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii}};
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, op->pipe);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, op->layout, 0, 1, &op->sets[slot], 0, nullptr);
    int32_t pc[4] = {(int32_t)b->x, (int32_t)b->y, (int32_t)b->w, (int32_t)b->h};
    vkCmdPushConstants(g_cb, op->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(g_cb, (b->w + 7) / 8, (b->h + 7) / 8, 1);
}

// HDR output (setting hdr, swapchain HDR10 / scRGB): one compute pass (tools/spvgen/Hdr.cs -> hdr_spv.h) from the game's
// float frame (scene + UI before its 8-bit copy: highlights above SDR white kept) into the RGBA16F staging image at the
// window size: bilinear scale, subtitles over it, nits (paper white), highlight roll-off to the screen's peak, PQ or
// scRGB. The present blit then only places it (fit_blit bars). Replaces upscale / sharpen / copy / overlay_subs in HDR.
// HDR scene snapshot (s121): vk_replay.cpp blits the 3D scene into this right after the game's FXAA draw (before its gamma
// pass and the UI); hdr_pass expands only pixels the UI left alone. RGBA16F (sampleable); the blit's format support is
// checked (PORTABILITY), else null: HDR without the expansion, as before.
static Image g_hdr_scene;
static std::atomic<bool> g_hdr_scene_fresh{false};
Image* hdr_scene_target(uint32_t w, uint32_t h, VkFormat from) {
    static std::map<VkFormat, bool> ok;
    if (!ok.count(from)) {
        VkFormatProperties a{}, b{};
        vkGetPhysicalDeviceFormatProperties(phys, from, &a);
        vkGetPhysicalDeviceFormatProperties(phys, VK_FORMAT_R16G16B16A16_SFLOAT, &b);
        ok[from] = (a.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (b.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        if (!ok[from]) log("vk: HDR scene snapshot: format %d cannot be blitted: highlights stay as the game made them", (int)from);
    }
    if (!ok[from] || (!rt::hdr_boost() && !rt::hdr_color())) return nullptr;
    if (g_hdr_scene.w != w || g_hdr_scene.h != h || !g_hdr_scene.image) make_staging(g_hdr_scene, w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    g_hdr_scene_fresh = true;
    return &g_hdr_scene;
}
static void hdr_pass(int slot, Image* src, Image& st, const std::shared_ptr<const rt::SubsBitmap>& subs) {
    struct Pass { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSet sets[4]; VkBuffer dummy; };
    static Pass* hp = [] {
        auto* p = new Pass{};
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_hdr_spv, k_hdr_spv};
        VkShaderModule mod;
        VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
        VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, b};
        VkDescriptorSetLayout dsl;
        VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 60};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl, 1, &pr};
        VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &p->layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
        ci.layout = p->layout;
        VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p->pipe));
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 4, 3, ps};
        VkDescriptorPool pool;
        VKC(vkCreateDescriptorPool(dev, &dpi, nullptr, &pool));
        VkDescriptorSetLayout l4[4] = {dsl, dsl, dsl, dsl};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, pool, 4, l4};
        VKC(vkAllocateDescriptorSets(dev, &ai, p->sets));
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};  // bound when there is no subtitle (never read)
        bci.size = 16;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &p->dummy));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, p->dummy, &req);
        // own memory: from alloc()'s shared image blocks (packed by alignment only, not bufferImageGranularity) the
        // first live switch to HDR lost the device every time (s119, A/B in an isolated data folder; granularity =
        // likely cause, not proven)
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory dm;
        VKC(vkAllocateMemory(dev, &mai, nullptr, &dm));
        VKC(vkBindBufferMemory(dev, p->dummy, dm, 0));
        log("vk: HDR output pass ready");
        return p;
    }();
    VkBuffer sb = subs ? subs_buffer(slot, st, subs) : VK_NULL_HANDLE;
    struct { int32_t sw, sh, dw, dh, bx, by, bw, bh; float paper, knee, peak; int32_t mode; float itm, sat; int32_t scene; } pc{};
    pc.sw = (int32_t)src->w, pc.sh = (int32_t)src->h, pc.dw = (int32_t)st.w, pc.dh = (int32_t)st.h;
    if (sb) pc.bx = subs->x, pc.by = subs->y, pc.bw = subs->w, pc.bh = subs->h;
    pc.peak = rt::hdr_peak_setting() > 0 ? (float)rt::hdr_peak_setting() : g_hdr_screen_peak >= 200 ? g_hdr_screen_peak : 1000.f;
    pc.paper = std::min((float)rt::hdr_paper_white(), pc.peak);
    pc.knee = std::min(std::max(pc.paper, 0.5f * pc.peak), 0.9f * pc.peak);  // below it highlights stay exact, above they roll off
    pc.mode = g_hdr_mode == 2 ? 1 : 0;
    static const float k_itm[5] = {0, 0.35f, 0.6f, 0.8f, 1.0f}, k_sat[5] = {0, 0.08f, 0.16f, 0.25f, 0.35f};
    pc.itm = k_itm[std::clamp(rt::hdr_boost(), 0, 4)], pc.sat = k_sat[std::clamp(rt::hdr_color(), 0, 4)];
    // the scene snapshot of this frame (vk_replay.cpp at the FXAA draw), same size as the frame: else no expansion
    const bool scene = g_hdr_scene_fresh.exchange(false) && g_hdr_scene.image && g_hdr_scene.w == src->w && g_hdr_scene.h == src->h;
    pc.scene = scene ? 1 : 0;
    transition(g_cb, src, VK_IMAGE_LAYOUT_GENERAL);
    transition(g_cb, &st, VK_IMAGE_LAYOUT_GENERAL);
    if (scene) transition(g_cb, &g_hdr_scene, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, src->view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, st.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, scene ? g_hdr_scene.view : src->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo bi{sb ? sb : hp->dummy, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[4] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, hp->sets[slot], 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[0]},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, hp->sets[slot], 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[1]},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, hp->sets[slot], 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, hp->sets[slot], 3, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[2]}};
    vkUpdateDescriptorSets(dev, 4, wr, 0, nullptr);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, hp->pipe);
    vkCmdBindDescriptorSets(g_cb, VK_PIPELINE_BIND_POINT_COMPUTE, hp->layout, 0, 1, &hp->sets[slot], 0, nullptr);
    vkCmdPushConstants(g_cb, hp->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(g_cb, (st.w + 7) / 8, (st.h + 7) / 8, 1);
}

// debug (NX_HDR_SHOT=1, then NX_CONTROL "shot"): the HDR staging image read back: luminance statistics in nits in the log
// and shots/hdr_NNNNN.bmp (nits / paper white, clipped, 2.2 gamma: an SDR view of the HDR picture)
static std::atomic<bool> g_hdr_shot{false};
static VkBuffer g_hrb_buf = VK_NULL_HANDLE;
static void* g_hrb_map = nullptr;
static VkDeviceSize g_hrb_cap = 0;
static void hdr_readback_record(Image& st) {
    const VkDeviceSize size = (VkDeviceSize)st.w * st.h * 8;
    if (size > g_hrb_cap) {  // shortcut: a smaller old buffer is leaked (debug only)
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &g_hrb_buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, g_hrb_buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        VKC(vkAllocateMemory(dev, &ai, nullptr, &m));
        VKC(vkBindBufferMemory(dev, g_hrb_buf, m, 0));
        VKC(vkMapMemory(dev, m, 0, req.size, 0, &g_hrb_map));
        g_hrb_cap = size;
    }
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {st.w, st.h, 1};
    vkCmdCopyImageToBuffer(g_cb, st.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_hrb_buf, 1, &c);
}
static void hdr_readback_finish(const Image& st) {
    auto half = [](uint16_t h) {
        const uint32_t e = (h >> 10) & 31, m = h & 1023;
        const float v = e ? ldexpf(1 + m / 1024.f, (int)e - 15) : ldexpf(m / 1024.f, -14);
        return (h & 0x8000) ? -v : v;
    };
    auto pq_nits = [](float v) {  // SMPTE ST 2084 inverse
        const float p = powf(std::max(v, 0.f), 1 / 78.84375f);
        return 10000 * powf(std::max(p - 0.8359375f, 0.f) / (18.8515625f - 18.6875f * p), 1 / 0.1593017578125f);
    };
    const bool pq = g_hdr_mode == 1;
    const float paper = (float)rt::hdr_paper_white();
    const uint32_t n = st.w * st.h;
    std::vector<float> y(n);
    std::vector<uint8_t> px((size_t)n * 4);
    const uint16_t* s = (const uint16_t*)g_hrb_map;
    double sum = 0;
    uint32_t over = 0;
    for (uint32_t i = 0; i < n; i++) {
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = pq ? pq_nits(half(s[4 * i + k])) : half(s[4 * i + k]) * 80;
        y[i] = pq ? 0.2627f * c[0] + 0.6780f * c[1] + 0.0593f * c[2] : 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        sum += y[i];
        over += y[i] > paper * 1.05f;
        if (pq) {  // preview in BT.709 (BT.2087 inverse)
            const float r = 1.6605f * c[0] - 0.5876f * c[1] - 0.0728f * c[2], g = -0.1246f * c[0] + 1.1329f * c[1] - 0.0083f * c[2],
                        b = -0.0182f * c[0] - 0.1006f * c[1] + 1.1187f * c[2];
            c[0] = r, c[1] = g, c[2] = b;
        }
        for (int k = 0; k < 3; k++) px[4 * i + 2 - k] = (uint8_t)lround(255 * powf(std::clamp(c[k] / paper, 0.f, 1.f), 1 / 2.2f));
        px[4 * i + 3] = 255;
    }
    std::vector<float> sorted(y);
    std::sort(sorted.begin(), sorted.end());
    log("hdr shot: %ux%u %s, luminance nits: max %.0f, 99.9%% %.0f, 99%% %.0f, median %.0f, mean %.1f; %.2f%% of pixels over paper white %.0f",
        st.w, st.h, pq ? "PQ" : "scRGB", sorted[n - 1], sorted[(size_t)(n * 0.999)], sorted[(size_t)(n * 0.99)], sorted[n / 2], sum / n,
        100.0 * over / n, paper);
    static int k = 0;
    char path[512];
    snprintf(path, sizeof path, "%s/shots/hdr_%05d.bmp", g_data_dir.c_str(), ++k);
    uint8_t hdr[54] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
    put(2, 54 + n * 4); put(10, 54); put(14, 40); put(18, st.w); put(22, (uint32_t)-(int32_t)st.h);
    hdr[26] = 1; hdr[28] = 32; put(34, n * 4);
    if (FILE* f = fopen(path, "wb")) fwrite(hdr, 1, 54, f), fwrite(px.data(), 1, px.size(), f), fclose(f);
}

// screenshots show what the player sees: with sharpening on, the sharpened frame; subtitles on top
static Image* shot_source(Image* src) {
    static Image img;
    auto subs = rt::frame_overlay(src->w, src->h);
    if ((rt::sharpen_setting() <= 0 && !subs) || src->format != VK_FORMAT_R8G8B8A8_UNORM) return src;
    if (img.w != src->w || img.h != src->h || img.format != src->format) make_staging(img, src->w, src->h, src->format);
    begin();
    if (!sharpen(3, src, img)) {
        transition(g_cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(g_cb, &img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {src->w, src->h, 1};
        vkCmdCopyImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    overlay_subs(3, img, subs);
    end_and_wait();
    return &img;
}

static void present_async(Image* src, Image* hsrc) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(present_thread).detach(); });
    int i = -1;
    {
        std::lock_guard<std::mutex> l(g_pres_lock);
        for (int k = 0; k < 3 && i < 0; k++)
            if (g_staged[k].state == Staged::FREE) i = k;
        if (i < 0) i = g_pending;  // none free: overwrite the frame still waiting (mailbox)
        if (i < 0) return;         // cannot happen with 3 images and one presenting
        if (g_pending == i) g_pending = -1;
        g_staged[i].state = Staged::BUSY;
    }
    Image& st = g_staged[i].img;
    // NX_PRESENT_DELAY=n (vsync ticks after release, default 1): more absorbs slow staging copies, adds n/60 s latency
    static const uint64_t delay = getenv("NX_PRESENT_DELAY") ? strtoull(getenv("NX_PRESENT_DELAY"), nullptr, 10) : 1;
    g_staged[i].tick = rt::g_pace_tick + delay;
    // HDR: the game's float frame when it was copied into this window texture this frame, else the window texture
    // (copied first into a sampleable image: window textures are not made for sampling; sampling one lost the device)
    static Image win_copy[3];
    Image* hs = hsrc;
    const bool hdr = g_hdr_mode && (hsrc || src->format == VK_FORMAT_R8G8B8A8_UNORM);
    if (g_hdr_mode && !hdr) {
        static bool told = false;
        if (!told) told = true, log("vk: HDR: window texture format %d cannot be read; frames are dropped", (int)src->format);
    }
    const bool up = (hdr && g_swap_extent.width && g_swap_extent.height) || upscale_wanted(src);  // minimized: 0 x 0
    uint32_t ow = up ? g_swap_extent.width : src->w, oh = up ? g_swap_extent.height : src->h;
    if (up) {  // upscale to the picture's own shape inside the window (fit_blit adds the bars)
        const double s = std::min((double)ow / src->w, (double)oh / src->h);
        const uint32_t w = (uint32_t)lround(src->w * s), h = (uint32_t)lround(src->h * s);
        if (w * 100 < ow * 99 || h * 100 < oh * 99) ow = w, oh = h;
    }
    const VkFormat sf = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : src->format;
    if (st.w != ow || st.h != oh || st.format != sf) make_staging(st, ow, oh, sf);
    if (hdr && !hs) {
        Image& wc = win_copy[i];
        if (wc.w != src->w || wc.h != src->h || wc.format != src->format) make_staging(wc, src->w, src->h, src->format);
        hs = &wc;
    }
    begin();
    if (hdr) {
        static int told = -1;  // which source the HDR picture comes from (log on change)
        if (told != (hsrc != nullptr)) told = hsrc != nullptr, log("vk: HDR source: %s (format %d, %ux%u)", hsrc ? "the game's float frame" : "the 8-bit window texture (no float copy this frame)", (int)hs->format, hs->w, hs->h);
        if (!hsrc) {
            transition(g_cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            transition(g_cb, hs, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {src->w, src->h, 1};
            vkCmdCopyImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hs->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        }
        hdr_pass(i, hs, st, rt::frame_overlay(st.w, st.h));
    } else if (up) {
        upscale(i, src, st);
    } else if (!sharpen(i, src, st)) {
        transition(g_cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(g_cb, &st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {src->w, src->h, 1};
        vkCmdCopyImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    if (!hdr) overlay_subs(i, st, rt::frame_overlay(st.w, st.h));
    transition(g_cb, &st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const bool rb = hdr && g_hdr_shot.exchange(false);  // debug: NX_CONTROL "shot" with NX_HDR_SHOT=1
    if (rb) hdr_readback_record(st);
    g_staged[i].serial = end_and_submit();
    if (rb) gpu_wait(g_staged[i].serial), hdr_readback_finish(st);
    {
        std::lock_guard<std::mutex> l(g_pres_lock);
        if (g_pending >= 0) g_staged[g_pending].state = Staged::FREE;  // replaced by this newer frame
        g_staged[i].state = Staged::PENDING;
        g_pending = i;
    }
    g_pres_cv.notify_all();
}

// Variable frame rate (fps 0 / N > 60) back-pressure. USER 11-13 (240 fps, 160 Hz G-SYNC display): attack animations "stutter like a
// camera shutter". The display path took only ~152 frames/s (presents exactly 6.6 ms apart, also with minimized test windows: the
// driver's present limit, e.g. NVIDIA's G-SYNC / low-latency cap of refresh - refresh^2/3600) while the game made 240, so the
// newest-frame pick showed game steps of 4.2 and 8.3 ms in turn: uneven motion. The game now waits (at most max_ms) until
// present_thread has taken the previous frame, so it runs at the rate frames are shown and each frame's dt is the display step.
// Where presents never block (no driver limit) nothing changes. NX_PRESENT_BACKPRESSURE=0: off.
void vk_wait_present_taken(double max_ms) {
    static const bool on = !getenv("NX_PRESENT_BACKPRESSURE") || atoi(getenv("NX_PRESENT_BACKPRESSURE")) != 0;
    if (!on || !g_present_queue) return;
    std::unique_lock<std::mutex> l(g_pres_lock);
    g_pres_cv.wait_for(l, std::chrono::duration<double, std::milli>(max_ms), [] { return g_pending < 0; });
}

extern Texture* g_last_rt;
static double g_present_block_max = 0, g_block_part[3];  // max ms: whole, acquire, blit wait, vkQueuePresent  // ms the present path (acquire, blit, present) blocked the game, frame stats
extern std::unordered_map<Texture*, uint64_t> g_draws_into;
extern uint64_t g_ops_since_present[];
extern uint32_t g_present_no;
void backend_present(Texture* t) {
    std::lock_guard<std::mutex> l(g_vk_lock);
    {  // hitch log: a game frame over 50 ms, with the time of the slow work inside it (game thread)
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        uint32_t h[HITCH_N];
        for (int k = 0; k < HITCH_N; k++) h[k] = g_hitch_us[k].exchange(0);
        g_replay_us_acc += h[HITCH_SUBMIT];  // per-10 s "replay" stat on the frames line (CPU cost of draw replay, s71)
        g_gpuwait_us_acc += h[HITCH_GPU_WAIT];
        g_rwait_us_acc += h[HITCH_RENDER_WAIT], g_slotwait_us_acc += h[HITCH_SLOT_WAIT];
        static const double hitch_ms = getenv("NX_HITCH_MS") ? atof(getenv("NX_HITCH_MS")) : 50;  // NX_HITCH_MS: log threshold
        if (ms > hitch_ms)
            log("hitch %.0f ms: shader translation %.0f, pipeline creation %.0f, texture uploads %.0f, GPU waits %.0f, images %.0f, pool "
                "imports %.0f, texture write checks %.0f ms; replay + submit %.0f ms in all, render thread waits %.0f, slot waits %.0f (rest: game code, loading); %llu draws, %llu texture binds", ms,
                h[HITCH_TRANSLATE] / 1e3, h[HITCH_PIPELINE] / 1e3, h[HITCH_UPLOAD] / 1e3, h[HITCH_GPU_WAIT] / 1e3,
                h[HITCH_IMAGE] / 1e3, h[HITCH_IMPORT] / 1e3, h[HITCH_CHECK] / 1e3, h[HITCH_SUBMIT] / 1e3, h[HITCH_RENDER_WAIT] / 1e3, h[HITCH_SLOT_WAIT] / 1e3,
                (unsigned long long)(g_ops_since_present[OP_DRAW] + g_ops_since_present[OP_DRAW_INDEXED] +
                                     g_ops_since_present[OP_DRAW_INSTANCED] + g_ops_since_present[OP_DRAW_INDEXED_INSTANCED]),
                (unsigned long long)g_ops_since_present[OP_TEXTURE]);
    }
    // NX_DEBUG_PRESENT=<n>: log draw/op counts of 6 presents starting at present n (1 = the first ones)
    static uint64_t dbg = 0, dbg_from = getenv("NX_DEBUG_PRESENT") ? strtoull(getenv("NX_DEBUG_PRESENT"), nullptr, 10) : 0;
    if (t && dbg_from && ++dbg >= dbg_from && dbg < dbg_from + 6) {
        std::string s;
        char b[48];
        for (int i = 0; i <= OP_UNKNOWN; i++) if (g_ops_since_present[i]) { snprintf(b, sizeof b, " op%d:%llu", i, (unsigned long long)g_ops_since_present[i]); s += b; }
        log("present %08x (%ux%u fmt %#x): %llu draws into it; ops since last present:%s", t->guest, t->width, t->height, t->format,
            (unsigned long long)g_draws_into[t], s.c_str());
        std::string top;
        for (auto& [tex, n] : g_draws_into) { snprintf(b, sizeof b, " %08x(%ux%u):%llu", tex->guest, tex->width, tex->height, (unsigned long long)n); top += b; }
        log("  draws per target:%s", top.c_str());
    }
    // NX_LOG_DRAWS=<n>: log the state and bindings of every draw of frame n (the frame after present n-1)
    extern int g_log_draws;
    static uint64_t ld_at = getenv("NX_LOG_DRAWS") ? strtoull(getenv("NX_LOG_DRAWS"), nullptr, 10) : 0, ld_n = 0;
    static int live_drawlog = 0;  // NX_CONTROL "drawlog [n]": frames still to log
    ++ld_n;
    if (ld_at && ld_n + 1 == ld_at) { g_log_draws = 20000; log("NX_LOG_DRAWS: frame %llu", (unsigned long long)ld_at); }
    else if ((ld_at && ld_n == ld_at) || (live_drawlog && --live_drawlog == 0)) g_log_draws = 0;
    else if (live_drawlog) g_log_draws = 20000, log("NX_CONTROL drawlog: frame %llu", (unsigned long long)ld_n + 1);
    if (int n = g_drawlog_request.exchange(0)) { g_log_draws = 20000; live_drawlog = n; log("NX_CONTROL drawlog: frame %llu", (unsigned long long)ld_n + 1); }
    // NX_DUMP_RT=<n>: at present n, also write every colour target drawn into since the last present (rt_<guest>_n.bmp)
    static uint64_t rt_at = getenv("NX_DUMP_RT") ? strtoull(getenv("NX_DUMP_RT"), nullptr, 10) : 0, rt_n = 0;
    static bool live_rt = false;  // NX_CONTROL "rt": dump at the next present
    ++rt_n;
    bool dump_rt = (rt_at && rt_n == rt_at) || live_rt;
    live_rt = g_rt_request.exchange(0) > 0;
    if (dump_rt && dev)
        for (auto& [tex, cnt] : g_draws_into) {
            Image* im = image_of(tex);
            if (!(im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || im->layers != 1) continue;
            char tag[48];
            snprintf(tag, sizeof tag, "rt_%08x_%ux%u_d%llu", tex->guest, im->w, im->h, (unsigned long long)cnt);
            dump_frame(im, rt_n, tag);
        }
    g_draws_into.clear();
    g_present_no++;
    vk::imports_trim();
    mem_next_frame();
    vk::upscale_frame_end();
    memset(g_ops_since_present, 0, sizeof(uint64_t) * (OP_UNKNOWN + 1));
    if (getenv("NX_PRESENT_LAST") && g_last_rt) t = g_last_rt;
    // HDR: the float frame noted for this window texture this frame (one frame only)
    Image* hdr_src = t && g_hdr_src && t->guest == g_hdr_win ? g_hdr_src : nullptr;
    g_hdr_src = nullptr;
    if (!dev || !t) return;
    {  // frame time stats every 10 s: presents/s, worst present-to-present gap
        using clk = std::chrono::steady_clock;
        static clk::time_point win = clk::now(), prev = win;
        static uint32_t n = 0;
        static double worst = 0;
        clk::time_point now = clk::now();
        worst = std::max(worst, std::chrono::duration<double, std::milli>(now - prev).count());
        clk::time_point prev_gap = prev;
        prev = now;
        n++;
        double secs = std::chrono::duration<double>(now - win).count();
        static uint32_t over40 = 0;
        if (std::chrono::duration<double, std::milli>(now - prev_gap).count() > 40) over40++;
        static std::vector<float> gaps;  // every gap of the window, for the 1% / 0.1% low
        gaps.push_back(std::chrono::duration<float, std::milli>(now - prev_gap).count());
        if (secs >= 10) {
            std::sort(gaps.begin(), gaps.end(), std::greater<float>());
            auto low = [&](size_t k) { k = std::max<size_t>(k, 1); double s = 0; for (size_t i = 0; i < k; i++) s += gaps[i]; return 1000.0 * k / s; };
            log("frames: 1%% low %.1f fps, 0.1%% low %.1f fps, median %.2f ms, p99 %.2f ms (%zu frames)", low(gaps.size() / 100), low(gaps.size() / 1000),
                gaps[gaps.size() / 2], gaps[gaps.size() / 100], gaps.size());
            const double med_gap = gaps[gaps.size() / 2], p99_gap = gaps[gaps.size() / 100];  // perf line below
            gaps.clear();
            log("frames: %.1f/s, worst gap %.1f ms, %u gaps > 40 ms, %llu late vsyncs, longest frame %.1f ms, present blocked %.1f ms max (acquire %.1f, blit %.1f, present %.1f)",
                n / secs, worst, over40, (unsigned long long)rt::g_late_vsyncs.exchange(0),
                rt::g_frame_work_max_us.exchange(0) / 1000.0, g_present_block_max, g_block_part[0], g_block_part[1], g_block_part[2]);
            log("frames: replay + submit %.2f ms per frame (%.0f%% of the time), GPU waits %.2f ms per frame (%.2f for a submit slot), pacing waits %.2f ms per frame, render thread waits %.2f ms per frame", n ? g_replay_us_acc / 1e3 / n : 0.0,
                g_replay_us_acc / 1e4 / secs, n ? g_gpuwait_us_acc / 1e3 / n : 0.0, n ? g_slotwait_us_acc / 1e3 / n : 0.0, n ? rt::g_pace_us.exchange(0) / 1e3 / n : 0.0, n ? g_rwait_us_acc / 1e3 / n : 0.0);
            if (g_ts_pool && n) {  // s124 (#11): what limits the frame rate, in plain words
                // typical frame = the median gap (loading hitches only move the mean); GPU / render thread are means
                const double frame = med_gap, gpu = g_gpu_busy_ns_acc / 1e6 / n, rep = g_replay_us_acc / 1e3 / n;
                const int fs = rt::auto30_active() ? 30 : rt::fps_setting();  // Auto 30 holding 30: that is the target
                const double want = fs > 0 ? 1000.0 / fs : 0;
                const char* why = want && frame < want * 1.1 ? "on target"
                                  : gpu > frame * 0.8     ? "GPU-bound: the graphics card is the limit (lower 3D resolution, res, MSAA or effects)"
                                  : rep > frame * 0.8     ? "CPU-bound: the render thread is the limit"
                                                          : "CPU-bound: the game thread is the limit";
                log("perf: typical frame %.1f ms (1%% slowest %.1f ms), GPU busy %.1f ms, render thread %.1f ms per frame: %s%s", frame, p99_gap, gpu, rep,
                    why, want && p99_gap > want * 2.5 ? "; with hitches (loading / shader builds / spikes)" : "");
            }
            vram_budget_line();
            pass_timing_report(n);
            if (n) log("frames: submit parts per frame: lock wait %.2f ms, begin %.2f, replay %.2f ms (on CPU %.2f), vkQueueSubmit %.2f",
                       g_part_us[0].exchange(0) / 1e3 / n, g_part_us[1].exchange(0) / 1e3 / n, g_part_us[2].exchange(0) / 1e3 / n,
                       g_part_us[3].exchange(0) / 1e3 / n, g_part_us[4].exchange(0) / 1e3 / n);
            g_replay_us_acc = 0, g_gpuwait_us_acc = 0, g_gpu_busy_ns_acc = 0, g_rwait_us_acc = 0, g_slotwait_us_acc = 0;
            uint32_t tn = g_translate_n.exchange(0), th = g_translate_hits.exchange(0), pn = g_pipeline_n.exchange(0);
            if (tn || th || pn || nvn::g_pipe_shared_hits)
                log("frames: shaders %u translated (%.0f ms) + %u from cache, %u pipelines created (%.0f ms, %u inline cache hits) + %u shared by content, programs %u new (%u moved code) + %u shared", tn,
                    g_translate_us.exchange(0) / 1000.0, th, pn, g_pipeline_us.exchange(0) / 1000.0, g_pipeline_inline.exchange(0), nvn::g_pipe_shared_hits.exchange(0), g_programs_new.exchange(0),
                    g_programs_moved.exchange(0), g_programs_shared.exchange(0));
            log("frames: VRAM %.0f MB in blocks, %.0f MB used; %llu images freed (%.0f MB) so far; RAM %.0f MB private; %llu upscaling twin syncs", g_mem_blocks_bytes / 1048576.0,
                g_mem_used_bytes / 1048576.0, (unsigned long long)g_images_freed.load(), g_image_bytes_freed / 1048576.0, rt::process_private_mb(),
                (unsigned long long)nvn::g_twin_syncs.load());
            pipeline_cache_save();
            rt::mem_regions_report();
            g_block_part[0] = g_block_part[1] = g_block_part[2] = 0;
            win = now; n = 0; worst = 0; over40 = 0; g_present_block_max = 0;
        }
    }
    Image* src = image_of(t);
    static uint64_t presents = 0;
    static uint64_t every = getenv("NX_SHOT_EVERY") ? strtoull(getenv("NX_SHOT_EVERY"), nullptr, 10) : 0;
    if (every && (++presents % every) == 0) dump_frame(src, presents);
    static uint64_t live = 0;
    static const std::string live_tag = getenv("NX_SHOT_TAG") ? getenv("NX_SHOT_TAG") : "live";  // test rigs running side by side
    if (g_shot_request > 0) {  // NX_CONTROL "shot" / "shots N"
        g_shot_request--, dump_frame(shot_source(src), ++live, live_tag.c_str());
        static const bool hdr_shot = getenv("NX_HDR_SHOT") && atoi(getenv("NX_HDR_SHOT"));
        if (hdr_shot) g_hdr_shot = true;
    }
    if (g_hero_shot.exchange(0)) dump_frame(shot_source(src), 0, "hero", (g_data_dir + "/launcher_hero.bmp").c_str());
    if (g_user_shot.exchange(0)) {  // F12: <data>/shots/screenshot_<date>_<time>_N.bmp
        static uint64_t user = 0;
        char tag[40];
        time_t now = time(nullptr);
        strftime(tag, sizeof tag, "screenshot_%Y%m%d_%H%M%S", localtime(&now));
        dump_frame(shot_source(src), ++user, tag);
    }
    auto t_block = std::chrono::steady_clock::now();
    if (g_present_queue) {
        present_async(src, hdr_src);
        g_present_block_max = std::max(g_present_block_max,
                                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_block).count());
        return;
    }
    if (g_swap_resize.exchange(false)) vkDeviceWaitIdle(dev), create_swapchain();
    // One graphics queue (AMD: RADV and AMD's Windows driver; Intel): the present runs here, on the game's queue. It
    // does not wait for the GPU (it used to: every frame then waited for all queued GPU work and the display's release,
    // ~16 ms of a 30 ms frame on a ROG Ally). Semaphores per submit slot (acquire; begin() waited for the slot's last
    // submit, so its wait is done) and per swapchain image (render; that image is re-acquired only after its present).
    // NX_SYNC_PRESENT=2: wait as before.
    static const bool wait_gpu = getenv("NX_SYNC_PRESENT") && atoi(getenv("NX_SYNC_PRESENT")) == 2;
    static VkSemaphore acq_sems[3], rend_sems[16];
    auto sem = [](VkSemaphore& s) {
        if (!s) { VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(dev, &sci, nullptr, &s)); }
        return s;
    };
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    begin();
    const VkSemaphore acq = sem(acq_sems[g_slot]);
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(dev, g_swap, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || idx >= 16) {
        end_and_wait();  // the slot's empty command buffer
        vkDeviceWaitIdle(dev);
        create_swapchain();
        return;
    }
    const VkSemaphore rend = sem(rend_sems[idx]);
    g_block_part[0] = std::max(g_block_part[0], ms_since(t_block));
    auto t_part = std::chrono::steady_clock::now();
    Image dst;
    dst.image = g_swap_images[idx];
    transition(g_cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(g_cb, &dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // NVN's window origin is lower-left (SetWindowOriginMode 1 = upper-left?): flip if the image comes out upside down
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)src->w, (int32_t)src->h, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    fit_blit(g_cb, dst.image, src->w, src->h, b);
    vkCmdBlitImage(g_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &b, VK_FILTER_LINEAR);
    transition(g_cb, &dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (wait_gpu) end_and_wait(acq, rend);
    else end_and_submit(acq, rend), stats_flush();
    g_block_part[1] = std::max(g_block_part[1], ms_since(t_part));
    t_part = std::chrono::steady_clock::now();
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rend;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(queue, &pi);
    g_block_part[2] = std::max(g_block_part[2], ms_since(t_part));
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(dev); create_swapchain(); }
    g_present_block_max = std::max(g_present_block_max,
                                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_block).count());
}

}  // namespace nvn
