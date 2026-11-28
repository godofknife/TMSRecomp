// Replay of recorded NVN commands into a Vulkan command buffer: render targets, clears, copies, draws.
// State encodings (bit positions) come from the SDK's NVN state setters (NOTES.md "NVN"); enum orders that are
// still assumptions are marked "assumed" and need checking on screen.
#include "plat.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <map>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <set>
#include <unordered_set>

#include "vk.h"
#include "vk_shader.h"
#include "smaa0_spv.h"
#include "smaa1_spv.h"
#include "smaa2_spv.h"
#include "smaa3_spv.h"
#include "msxv_spv.h"
#include "msxf_spv.h"
#include "motion_spv.h"
#include "mvcheck_spv.h"

using namespace rt;
using namespace vk;

namespace rt { extern std::atomic<int64_t> g_btl_open_ms; }  // hle_sys.cpp: steady-clock ms of the last battle opening
namespace nvn {

// Graphics state last recorded into the current command buffer by draw(): an unchanged pipeline, viewport, scissor,
// blend constants, stencil reference or vertex buffer is not recorded again (issue #11, Augs' crosswalk: ~24 Vulkan
// calls per draw, 16 of them vertex buffer binds; the driver was ~30% of the render thread's busy time). Other code
// that records graphics state calls cbs_forget(). NX_CB_CACHE=0: record everything every draw (A/B).
extern uint64_t g_cb_begins;  // vk.cpp begin()
uint64_t pool_gen();          // nvn_hle.cpp: changes when any pool is created or finalized
struct CbState {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t gen = 0;
    bool valid = false;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkViewport vp{};
    VkRect2D sc{};
    float blend[4]{};
    uint32_t stencil = 0;
    VkBuffer vb[16]{};
    VkDeviceSize vo[16]{};
};
static CbState g_cbs;
static void cbs_forget() { g_cbs.valid = false; }
static bool cbs_fresh(VkCommandBuffer cb) {  // true: g_cbs holds what cb has recorded
    static const bool on = !getenv("NX_CB_CACHE") || atoi(getenv("NX_CB_CACHE")) != 0;
    if (on && g_cbs.valid && g_cbs.cb == cb && g_cbs.gen == g_cb_begins) return true;
    g_cbs.valid = false, g_cbs.cb = cb, g_cbs.gen = g_cb_begins;
    return false;
}

struct Binding { uint64_t addr = 0, size = 0; };
static void vb_gpu_wrote(uint64_t addr, uint64_t size);  // vertex data in video memory (below): the GPU writes there
static void vb_frame_end();  // ... its frame-end cleanup
struct State {
    Texture* colors[8] = {};
    uint32_t ncolors = 0;
    Texture* depth = nullptr;
    bool rendering = false;
    VkViewport vp{0, 0, 1280, 720, 0, 1};
    VkRect2D scissor{{0, 0}, {1280, 720}};
    Program* programs[6] = {};  // per NVN stage
    uint8_t blend[8][8] = {};  // per target
    uint32_t color = 0x08030000, channel = 0xFFFFFFFF, polygon = 0x14;
    uint32_t ds[2] = {0x22, 0x11181118};
    std::vector<uint32_t> attribs, streams;  // packed attrib words; stream (stride, divisor) pairs
    Binding vbuf[16];
    Binding ubuf[6][18];
    Binding sbuf[6][16];
    uint64_t tex[6][32] = {};
    uint32_t tex_pool = 0, smp_pool = 0;
    uint64_t draws = 0, drawn = 0;
    float blend_color[4] = {};
    uint32_t stencil_ref = 0;
    float alpha_ref = 0;
};
static State g_st;
Texture* g_last_rt = nullptr;  // NX_PRESENT_LAST: debug view of the last colour target drawn to
std::unordered_map<Texture*, uint64_t> g_draws_into;  // per-present debug counters
uint64_t g_ops_since_present[OP_UNKNOWN + 1];
uint32_t g_present_no = 0;  // "skipshadow 2" alternates casters off / on per frame; UI smoothing's frame edge
static uint64_t g_stats_clears = 0, g_stats_copies = 0, g_stats_tex_draws = 0, g_stats_tex_written = 0;

// ponytail: every image lives in GENERAL and a full barrier separates passes and copies; split barriers by hazard
// once a profile says they cost
// s115 (#11, GeForce 920MX): it costs. Fortuna Office switches the scene between two depth buffers on one colour target
// 25-59 times a frame; each switch ended our pass with this barrier = ~0.25 ms of GPU idle each on that card (15 of 40 ms
// per frame). Now a pass that ends because the game set other targets defers its barrier (end_rendering defer): the
// targets it wrote go to g_unsynced and the barrier comes before anything reads one of them as a texture (bind_resources),
// before any copy / clear / dispatch / upload (they all call end_rendering first) and at the end of the replay. Pass to
// pass attachment writes rely on the GPU's in-order raster output. 1x passes only (MSAA keeps the old rule).
// NX_PASS_BARRIER=1: the old barrier at every pass end (A/B, or a driver that shows wrong pictures with the new rule).
static bool g_bar_pending = false;
static uint32_t g_ms_samples = 1;       // sample count of the pass being recorded (pipeline())
static std::vector<Image*> g_unsynced;  // attachments written by passes since the last full barrier
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    g_bar_pending = false;
    g_unsynced.clear();
}
static bool unsynced(const Image* im) {
    return g_bar_pending && std::find(g_unsynced.begin(), g_unsynced.end(), im) != g_unsynced.end();
}
// Upscaling twins (vk.cpp twin_register): a screen-size target written while the frame ran on one size and read while it
// runs on the other (s86: the battle-start field capture is copied at the scene size, the transition's later frames have
// no 3D scene, run full size and read the never-written full-size image = black at 4K + DLSS / FSR 3). The newer twin is
// scaled into the read one first (colour linear; depth / stencil nearest). Outside a rendering pass. NX_TWIN_SYNC=0 off (debug).
// Depth: a pass after the upscale point that depth-tests (the field KO mark over the player, USER 11-21 video: it vanished
// or showed cut into slices with DLSS) tested against the full-size depth the scene never drew this frame.
static void ms_stale(Image* im);
std::atomic<uint64_t> g_twin_syncs{0};  // vk.cpp stats line
static void twin_sync(VkCommandBuffer cb, Image* im) {
    static const bool on = !getenv("NX_TWIN_SYNC") || atoi(getenv("NX_TWIN_SYNC")) != 0;
    Image* o = im ? im->twin : nullptr;
    if (!on || !o || o->wgen <= im->wgen || !o->written || o->aspect != im->aspect || o->format != im->format) return;
    const bool colour = im->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
    if (!colour) {  // depth blits need the format's blit support (D24S8 has it on NVIDIA; checked, not assumed)
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(vk::phys, im->format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            static bool said = false;
            if (!said) said = true, log("vk: depth twin format %d cannot be blitted: not synced", (int)im->format);
            return;
        }
    }
    transition(cb, o, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    full_barrier(cb);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {im->aspect, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)o->w, (int32_t)o->h, 1};
    b.dstOffsets[1] = {(int32_t)im->w, (int32_t)im->h, 1};
    vkCmdBlitImage(cb, o->image, VK_IMAGE_LAYOUT_GENERAL, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, colour ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    full_barrier(cb);
    im->written = true;
    im->wgen = o->wgen;
    ms_stale(im);
    if (g_twin_syncs++ < 8) log("vk: upscaling twin %ux%u -> %ux%u synced (written at the other size)", o->w, o->h, im->w, im->h);
}
// NX_GPU_PASSES=1 (debug, s75): a GPU timestamp at every pass start and end; the time from one mark to the next goes to
// the first mark's label ("pass <target>" = inside it, "after <target>" = barriers, copies, dispatches until the next).
// Results of a slot are read when vk.cpp reuses it (pass_timing_slot); the frames line prints the top labels.
static VkQueryPool g_pt_pool = VK_NULL_HANDLE;
static constexpr uint32_t PT_N = 4096;
static std::vector<std::string> g_pt_labels[3];
static int g_pt_slot = 0;
static std::map<std::string, std::pair<double, uint64_t>> g_pt_acc;  // label -> ns, count
static void pt_mark(VkCommandBuffer cb, std::string label) {
    if (!g_pt_pool || g_pt_labels[g_pt_slot].size() >= PT_N) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt_pool, g_pt_slot * PT_N + (uint32_t)g_pt_labels[g_pt_slot].size());
    g_pt_labels[g_pt_slot].push_back(std::move(label));
}
void pass_timing_slot(VkCommandBuffer cb, int slot) {  // vk.cpp begin(): the slot's last submit is done
    static const bool on = getenv("NX_GPU_PASSES") != nullptr;
    if (!on) return;
    if (!g_pt_pool) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, 3 * PT_N};
        VKC(vkCreateQueryPool(dev, &qpi, nullptr, &g_pt_pool));
    }
    auto& lb = g_pt_labels[slot];
    if (lb.size() >= 2) {
        std::vector<uint64_t> ts(lb.size());
        if (vkGetQueryPoolResults(dev, g_pt_pool, slot * PT_N, (uint32_t)lb.size(), ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            for (size_t i = 0; i + 1 < lb.size(); i++)
                if (ts[i + 1] >= ts[i]) { auto& a = g_pt_acc[lb[i]]; a.first += (ts[i + 1] - ts[i]) * (double)props.limits.timestampPeriod; a.second++; }
    }
    lb.clear();
    g_pt_slot = slot;
    vkCmdResetQueryPool(cb, g_pt_pool, slot * PT_N, PT_N);
}
void pass_timing_report(uint64_t frames) {  // frames line (vk.cpp): top 14 labels by GPU ms per frame
    if (g_pt_acc.empty() || !frames) return;
    std::vector<std::pair<double, std::string>> v;
    double all = 0;
    for (auto& [k, a] : g_pt_acc) v.push_back({a.first, k}), all += a.first;
    std::sort(v.rbegin(), v.rend());
    log("gpu passes: %.2f ms per frame between marks (top 14 by time):", all / 1e6 / frames);
    static const size_t top = getenv("NX_GPU_PASSES") && atoi(getenv("NX_GPU_PASSES")) >= 2 ? 40 : 14;
    for (size_t i = 0; i < v.size() && i < top; i++)
        log("  %6.3f ms/frame  %5.1f/frame  %s", v[i].first / 1e6 / frames, g_pt_acc[v[i].second].second / (double)frames, v[i].second.c_str());
    g_pt_acc.clear();
}
static std::string pt_target();
// NX_PASS_SEQ=<present n> (debug, s75): log frame n's passes, resolves and expands in order (targets, samples, draws)
static uint32_t g_seq_draws = 0, g_seq_dsuse = 0;
std::atomic<int> g_seq_request{0};  // NX_CONTROL "seq": log the next frame's pass order
static bool seq_on() {
    static uint32_t at = getenv("NX_PASS_SEQ") ? (uint32_t)atoi(getenv("NX_PASS_SEQ")) : 0;
    if (g_seq_request.exchange(0)) at = g_present_no + 1;
    return at && g_present_no == at;
}
// draws / clears recorded since begin_rendering. A pass that wrote nothing needs no barrier after it (issue #11: a
// draw's texture upload / layout change ends the pass the draw just began; 67 passes per battle frame, 7.7 ms per frame
// between passes on a GeForce 920MX). NX_EMPTY_PASS_BARRIER=1 puts the barrier back, =0 takes it away.
// AMD keeps it by default: every pass ends with storeOp STORE on its targets, a write even with no draws, and AMD
// stores colour compression metadata (DCC) there. Without the barrier a later read sees half-updated blocks: coloured
// speckles in battle and on character screens (ROG Ally, RADV; RADV_DEBUG=nodcc hid it). NVIDIA / Intel unaffected.
static uint32_t g_pass_writes = 0;
// Depth targets live in DEPTH_STENCIL_ATTACHMENT_OPTIMAL from their first pass on (issue #11, s120): in GENERAL the
// driver keeps depth uncompressed (8K office test: switching GENERAL -> optimal -> GENERAL per pass cost +0.7..1.7 ms,
// i.e. a decompress each time). Every other use (sampling, copies, clears, blits, MSAA resolve / expand, FSR 3) already
// goes through transition() to GENERAL first, and the next pass moves it back. A depth a draw samples while it is the
// pass's own depth (a feedback read) stays GENERAL for good (g_ds_feedback). NX_DS_OPT=0 = all GENERAL as before.
static bool ds_opt() { static const bool on = !getenv("NX_DS_OPT") || atoi(getenv("NX_DS_OPT")) != 0; return on; }
static Image* g_pass_depth = nullptr;
static std::unordered_set<Image*> g_ds_feedback;
static const char* g_break = "";  // debug (ctl seq): why the next end_rendering ends the pass
static void end_rendering(VkCommandBuffer cb, bool defer = false) {  // defer: see g_unsynced
    const char* why = g_break;
    g_break = "";
    if (g_st.rendering) {
        vkCmdEndRendering(cb); g_st.rendering = false;
        g_pass_depth = nullptr;
        if (seq_on()) log("seq:   end, %u draws, %u use depth / stencil (%s)", g_seq_draws, g_seq_dsuse, why);
        if (g_pt_pool) pt_mark(cb, "after " + pt_target());
        static const bool old = getenv("NX_PASS_BARRIER") && atoi(getenv("NX_PASS_BARRIER")) != 0;
        static const bool empty_barrier = getenv("NX_EMPTY_PASS_BARRIER") ? atoi(getenv("NX_EMPTY_PASS_BARRIER")) != 0
                                                                     : vk::gpu_is_amd();  // see g_pass_writes
        if (g_pass_writes == 0 && g_ms_samples == 1 && !old && !empty_barrier) {
            if (g_bar_pending && !defer) full_barrier(cb);  // an earlier pass's deferred barrier the caller relies on
            return;
        }
        if (defer && !old && g_ms_samples == 1) {
            for (uint32_t i = 0; i < g_st.ncolors; i++)
                if (Image* im = image_of(g_st.colors[i]); im && !unsynced(im)) g_unsynced.push_back(im);
            if (Image* im = image_of(g_st.depth); im && !unsynced(im)) g_unsynced.push_back(im);
            g_bar_pending = true;
            return;
        }
        full_barrier(cb);
    } else if (g_bar_pending && !defer) {
        full_barrier(cb);
    }
}

// MSAA (setting msaa 0 / 2 / 4 / 8): the 3D scene passes (a depth target at render scale plus colour targets of the
// same size) draw into multisampled twins of their targets; shadow maps (no colour), post and UI passes stay 1x.
// The single-sample images get the resolved result lazily (ms_flush: an empty rendering pass with resolve
// attachments, colour average / integer and depth-stencil sample 0) before anything else touches them: another pass,
// a copy, clear, dispatch, sampling, the SMAA pass, the end of the replay. A twin whose content no longer matches its
// image (written by a 1x pass or a copy) makes the next pass on it run at 1x, unless that pass clears it first (the
// clears after OP_RENDER_TARGETS are looked ahead: g_clear_ahead).
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l);
static VkSampler sampler_of(const Sampler& s);
struct MsTwin {
    VkImage img = VK_NULL_HANDLE, src = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize off = 0, size = 0;
    uint32_t w = 0, h = 0, samples = 0;
    VkFormat f = VK_FORMAT_UNDEFINED;
    bool valid = false;  // holds the image's current content
};
// twins are keyed by Image*; a new image at the same address replaces the twin (size / format / VkImage checked).
// Freed (ms_prune, start of a replay) when their image goes away and all at once when the sample count changes
// (setting, FSR 3 / DLSS switched on): before 11-16 they were never freed, 4K 4x toggles left +2.3 GB (user's nx.log).
static std::unordered_map<Image*, MsTwin> g_ms;
static std::mutex g_ms_dead_lock;
static std::vector<std::pair<Image*, VkImage>> g_ms_dead;  // g_ms_dead_lock: images vk.cpp freed (ms_image_dead)
static std::vector<Image*> g_ms_dirty;  // images whose twin is newer (attachments of the last MSAA pass)
static uint32_t g_clear_ahead = 0;      // OP_RENDER_TARGETS: bit i colour i, bit 8 depth: cleared before the first draw
static std::unordered_map<Image*, uint32_t> g_ms_1x_frame;  // colour target -> frame (g_present_no) its scene pass ran 1x
static std::unordered_set<Image*> g_scene_depth;  // depths a scene pass (colour + depth, same size) has used: depth-only passes on them take MSAA
static bool ms_int_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
            return true;
        default: return false;
    }
}
static bool g_temporal_bad;  // FSR 3 / DLSS could not start: anti-aliasing fell back to SMAA, MSAA stays useful
static uint32_t ms_wanted() {  // the setting, limited to what the GPU supports for colour and depth
    if (rt::fsr3_setting() && !g_temporal_bad) return 1;  // FSR 3 / DLSS already anti-alias: 8x under them is only cost (and VRAM: twins are never freed)
    const uint32_t s = (uint32_t)rt::msaa_setting();
    if (s < 2) return 1;
    const VkSampleCountFlags ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                                  props.limits.framebufferStencilSampleCounts;
    uint32_t n = s >= 8 ? 8 : s >= 4 ? 4 : 2;
    while (n > 1 && !(ok & n)) n >>= 1;
    // VRAM cap (PORTABILITY rule): twins are never freed; a run needs ~12 screen-sized ones (s76, Shibuya + tutorial battle
    // at 4K 8x: 12 twins = 2.87 GB). Keep them under a quarter of the largest device-local heap: 4K 8x needs ~13 GB of
    // VRAM, 4K 4x ~7 GB, 1080p 8x ~3.5 GB; below that the sample count halves.
    static const uint64_t budget = [] {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint64_t vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max(vram, (uint64_t)mp.memoryHeaps[i].size);
        if (const char* e = getenv("NX_MSAA_VRAM_MB")) vram = (uint64_t)atoi(e) << 20;  // debug: test the cap as a smaller GPU
        return vram / 4;
    }();
    const uint64_t px = (uint64_t)(1280.0 * 720 * res_scale() * res_scale());
    const uint32_t want = n;
    while (n > 1 && 12 * px * 4 * n > budget) n >>= 1;
    static bool told = false;
    if (n < want && !told) { told = true; log("vk: MSAA %ux -> %ux (VRAM budget %llu MB for the MSAA copies)", want, n, (unsigned long long)(budget >> 20)); }
    return n;
}
static MsTwin& ms_twin(VkCommandBuffer cb, Image* im, uint32_t samples) {
    MsTwin& t = g_ms[im];
    if (t.img && (t.w != im->w || t.h != im->h || t.f != im->format || t.samples != samples || t.src != im->image)) {
        defer_destroy(t.img, t.view, {}, t.mem, t.off, t.size);
        t = MsTwin{};
    }
    if (t.img) return t;
    const bool depth = (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->w, im->h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    VKC(vkCreateImage(dev, &ci, nullptr, &t.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, t.img, &req);
    t.mem = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t.off);
    t.size = req.size;
    VKC(vkBindImageMemory(dev, t.img, t.mem, t.off));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = im->format;
    vci.subresourceRange = {im->aspect, 0, 1, 0, 1};
    VKC(vkCreateImageView(dev, &vci, nullptr, &t.view));
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                            t.img, {im->aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    t.w = im->w, t.h = im->h, t.f = im->format, t.samples = samples, t.src = im->image;
    static int n = 0;
    static uint64_t total = 0;  // VRAM taken by twins so far (never freed, see g_ms): low-VRAM reports
    total += req.size;
    if (n++ < 24) log("vk: MSAA %ux twin %ux%u fmt %d (%s), twins %llu MB", samples, im->w, im->h, (int)im->format,
                      depth ? "depth" : "colour", (unsigned long long)(total >> 20));
    return t;
}
static bool ms_is_dirty(Image* im) { return std::find(g_ms_dirty.begin(), g_ms_dirty.end(), im) != g_ms_dirty.end(); }
void ms_image_dead(Image* im, VkImage img) {  // vk.cpp free_dead_images, any thread
    std::lock_guard<std::mutex> l(g_ms_dead_lock);
    g_ms_dead.push_back({im, img});
}
static void ms_free(MsTwin& t) {
    if (t.img) defer_destroy(t.img, t.view, {}, t.mem, t.off, t.size);
    t = MsTwin{};
}
// start of a replay: nothing is dirty (the last replay ended with ms_flush), so dropping twins loses no content
static void ms_prune() {
    static std::vector<std::pair<Image*, VkImage>> dead;
    dead.clear();
    {
        std::lock_guard<std::mutex> l(g_ms_dead_lock);
        dead.swap(g_ms_dead);
    }
    for (auto& [im, img] : dead) {
        auto it = g_ms.find(im);
        if (it != g_ms.end() && it->second.src != img) continue;  // the address holds a newer image already
        if (it != g_ms.end()) ms_free(it->second), g_ms.erase(it);
        g_ms_1x_frame.erase(im);
        g_scene_depth.erase(im);
    }
    static uint32_t last = 0;
    const uint32_t s = ms_wanted();
    if (s == last) return;
    if (last && !g_ms.empty()) log("vk: MSAA %ux -> %ux: %zu MSAA copies freed", last, s, g_ms.size());
    for (auto& [im, t] : g_ms) ms_free(t);
    g_ms.clear();
    last = s;
}
// resolve dirty twins into their images (outside a rendering pass): all of them, or only `only` (those of them that are
// dirty). One resolve pass per size, at most 8 colours and one depth each (s78: Shibuya alternates two scene depths on
// one colour target; the old resolve-everything put both depths in one pass and dropped the second one's dirty mark).
static void ms_flush(VkCommandBuffer cb, const char* why = "pass", Image* const* only = nullptr, uint32_t nonly = 0) {
    if (g_ms_dirty.empty()) return;
    static std::vector<Image*> todo;
    todo.clear();
    for (Image* im : g_ms_dirty)
        if (!only || std::find(only, only + nonly, im) != only + nonly) todo.push_back(im);
    for (Image* im : todo) g_ms_dirty.erase(std::find(g_ms_dirty.begin(), g_ms_dirty.end(), im));
    while (!todo.empty()) {
        VkRenderingAttachmentInfo ca[8]{};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.layerCount = 1;
        uint32_t n = 0;
        const uint32_t w = todo[0]->w, h = todo[0]->h;
        for (size_t i = 0; i < todo.size();) {
            Image* im = todo[i];
            MsTwin& t = g_ms[im];
            const bool depth = (im->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
            if (t.img && (im->w != w || im->h != h || (depth ? ri.pDepthAttachment || ri.pStencilAttachment : n == 8))) { i++; continue; }  // next pass
            todo.erase(todo.begin() + i);
            if (!t.img) continue;
            VkRenderingAttachmentInfo& a = depth ? da : ca[n];
            a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            a.imageView = t.view;
            a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.resolveMode = depth || ms_int_format(im->format) ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
            a.resolveImageView = im->view;
            a.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
            if (depth) {
                if (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
                if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
            } else {
                n++;
            }
        }
        if (!n && !ri.pDepthAttachment && !ri.pStencilAttachment) continue;
        ri.colorAttachmentCount = n;
        ri.pColorAttachments = ca;
        ri.renderArea = {{0, 0}, {w, h}};
        if (seq_on()) log("seq: resolve %ux%u colours %u depth %d (%s)", w, h, n, ri.pDepthAttachment != nullptr, why);
        if (g_pt_pool) pt_mark(cb, "resolve " + std::to_string(w) + "x" + std::to_string(h) + " c" + std::to_string(n) + (ri.pDepthAttachment ? "+d" : "") + " " + why);
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
        full_barrier(cb);
        if (g_pt_pool) pt_mark(cb, "after resolve");
    }
}
static void ms_flush_one(VkCommandBuffer cb, const char* why, Image* a, Image* b = nullptr) {  // only these, if dirty
    Image* l[2] = {a, b};
    if ((a && ms_is_dirty(a)) || (b && ms_is_dirty(b))) ms_flush(cb, why, l, 2);
}
// copy the 1x image into every sample of its twin (outside a rendering pass)
static bool g_ms_expand_bad;  // the expand pipeline failed to build: passes needing it stay 1x
// draw the 1x image into its twin (one full-screen triangle, tools/spvgen/MsExpand.cs; outside a rendering pass)
static bool ms_expand(VkCommandBuffer cb, Image* im, MsTwin& t) {
    if (g_ms_expand_bad) return false;
    static VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout layout;
    static std::map<std::pair<VkFormat, uint32_t>, VkPipeline> pipes;
    if (!vs) {
        VkShaderModuleCreateInfo a{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_msxv_spv, k_msxv_spv};
        VkShaderModuleCreateInfo b{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_msxf_spv, k_msxf_spv};
        if (vkCreateShaderModule(dev, &a, nullptr, &vs) != VK_SUCCESS || vkCreateShaderModule(dev, &b, nullptr, &fs) != VK_SUCCESS) {
            log("vk: MSAA expand shaders failed: those passes stay 1x");
            g_ms_expand_bad = true;
            return false;
        }
        VkDescriptorSetLayoutBinding bd{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &bd};
        VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &dsl};
        VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &layout));
    }
    VkPipeline& pipe = pipes[{im->format, t.samples}];
    if (!pipe) {
        VkPipelineShaderStageCreateInfo st[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = (VkSampleCountFlagBits)t.samples;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dyn};
        VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        pr.colorAttachmentCount = 1;
        pr.pColorAttachmentFormats = &im->format;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
        pci.stageCount = 2, pci.pStages = st, pci.pVertexInputState = &vi, pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp, pci.pRasterizationState = &rs, pci.pMultisampleState = &ms, pci.pDepthStencilState = &dss;
        pci.pColorBlendState = &cbs, pci.pDynamicState = &ds, pci.layout = layout;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
            log("vk: MSAA expand pipeline failed (fmt %d): those passes stay 1x", (int)im->format);
            pipe = VK_NULL_HANDLE;
            g_ms_expand_bad = true;
            return false;
        }
        log("vk: MSAA expand pipeline fmt %d x%u ready", (int)im->format, t.samples);
    }
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSet set = alloc_set(dsl);
    VkDescriptorImageInfo ii{sampler_of(Sampler{}), im->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = t.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &ca;
    ri.renderArea = {{0, 0}, {im->w, im->h}};
    full_barrier(cb);
    if (seq_on()) log("seq: expand %p %ux%u fmt %d", (void*)im, im->w, im->h, (int)im->format);
    if (g_pt_pool) pt_mark(cb, "expand " + std::to_string(im->w) + "x" + std::to_string(im->h) + " fmt " + std::to_string((int)im->format));
    vkCmdBeginRendering(cb, &ri);
    VkViewport v{0, 0, (float)im->w, (float)im->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &ri.renderArea);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    cbs_forget();
    full_barrier(cb);
    if (g_pt_pool) pt_mark(cb, "after expand");
    t.valid = true;
    static uint64_t n = 0;
    if (n++ % 2000 == 0) log("vk: MSAA expands %llu", (unsigned long long)n);
    return true;
}
// the image's twin no longer matches it (written outside MSAA)
static void ms_stale(Image* im) {
    auto it = g_ms.find(im);
    if (it != g_ms.end()) it->second.valid = false;
}

static void begin_rendering(VkCommandBuffer cb) {
    if (g_st.rendering) return;
    VkRenderingAttachmentInfo ca[8]{};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = 0x7FFFFFFF, h = 0x7FFFFFFF, n = 0;
    // MSAA for this pass? (see MsTwin)
    Image* atts[9] = {};
    uint32_t natt = 0;
    for (uint32_t i = 0; i < g_st.ncolors; i++) if (Image* im = image_of(g_st.colors[i])) atts[natt++] = im;
    Image* dimg = image_of(g_st.depth);
    uint32_t samples = ms_wanted();
    if (samples > 1) {
        // depth-only passes (natt 0: the depth clear before the scene) take MSAA too, so the depth twin stays valid
        // for the scene pass (a 1x depth clear made it stale: scene at 1x, s75 finding). Only depths a scene pass used
        // before: shadow maps are res-scaled too (battle 1024x2048 x3 at 4K = ~600 MB as an 8x twin) and gain nothing.
        bool ok = dimg && dimg->scale == res_scale() && dimg->layers == 1 && dimg->levels == 1 &&
                  (natt > 0 || g_scene_depth.count(dimg));
        for (uint32_t i = 0; ok && i < g_st.ncolors; i++) {
            Image* im = image_of(g_st.colors[i]);
            if (!im) continue;  // unbound slot
            ok = im->w == dimg->w && im->h == dimg->h && im->layers == 1 && im->levels == 1 && im->view;
        }
        const bool scene = ok;  // a 3D scene pass: MSAA unless a twin cannot hold its content
        if (scene && natt) {
            if (g_scene_depth.size() > 64) g_scene_depth.clear();  // stale Image* keys (a scene pass re-adds its depth)
            g_scene_depth.insert(dimg);
        }
        if (ok) {  // every twin must hold its image's content, or the image is cleared before the first draw
            for (uint32_t i = 0; ok && i < g_st.ncolors; i++) {
                Image* im = image_of(g_st.colors[i]);
                if (!im) continue;
                auto it = g_ms.find(im);
                const bool valid = it != g_ms.end() && it->second.valid && it->second.samples == samples && it->second.src == im->image;
                const bool expand = !valid && im->written && !(g_clear_ahead >> i & 1);
                // s75 (Shibuya 4K 8x): the main scene ran 1x (its depth at 1x) between one-draw passes on a second depth;
                // each switch cost a full expand + resolve (~4 ms per frame, ~45% of the GPU frame). A target whose scene
                // pass already ran 1x this frame stays 1x for the rest of it (no expand).
                static const bool pingpong = getenv("NX_MSAA_PINGPONG") != nullptr;  // debug: the old rule (A/B)
                auto o = g_ms_1x_frame.find(im);
                ok = !expand || (!g_ms_expand_bad && (pingpong || o == g_ms_1x_frame.end() || o->second != g_present_no));  // else 1x
            }
            auto it = g_ms.find(dimg);
            const bool dvalid = it != g_ms.end() && it->second.valid && it->second.samples == samples && it->second.src == dimg->image;
            ok = ok && (dvalid || !dimg->written || (g_clear_ahead >> 8 & 1));
        }
        static int dbg = getenv("NX_MSAA_LOG") ? atoi(getenv("NX_MSAA_LOG")) : 0;  // debug: why passes stay 1x
        if (dbg > 0 && dimg && natt) {
            dbg--;
            Image* c0 = atts[0];
            log("vk: MSAA pass? %s (expand bad %d) colours %u c0 %ux%u fmt %d written %d | depth %ux%u scale %u/%u layers %u levels %u written %d | clear ahead %#x",
                ok ? "yes" : "no", (int)g_ms_expand_bad, natt, c0->w, c0->h, (int)c0->format, c0->written, dimg->w, dimg->h, (uint32_t)dimg->scale, (uint32_t)res_scale(),
                dimg->layers, dimg->levels, dimg->written, g_clear_ahead);
        }
        if (!ok) samples = 1;
        if (scene && samples == 1 && g_ms_1x_frame.size() > 256) g_ms_1x_frame.clear();  // stale Image* keys
        if (scene && samples == 1)
            for (uint32_t i = 0; i < g_st.ncolors; i++)
                if (Image* im = image_of(g_st.colors[i])) g_ms_1x_frame[im] = g_present_no;
    }
    if (dimg) atts[natt++] = dimg;
    // A 1x pass loads its targets: resolve those whose twin is newer. An MSAA pass continues on the twins. Other dirty
    // twins stay unresolved until something reads their image (sampled, copied, 1x pass, SMAA, end of the replay).
    // s78: Shibuya 4K 8x switched between two scene depths on one colour ~25 times a frame and resolved colour + depth
    // at each switch (old rule: any change of targets) = 5-7 ms of a 15.5 ms GPU frame.
    static const bool flush_all = getenv("NX_MSAA_FLUSH_ALL") != nullptr;  // debug: the old rule (A/B)
    if (flush_all) {
        bool same = samples > 1 && g_ms_dirty.size() == natt;
        for (uint32_t i = 0; same && i < natt; i++) same = ms_is_dirty(atts[i]);
        if (!same) ms_flush(cb);
    } else if (samples == 1 && !g_ms_dirty.empty()) {
        ms_flush(cb, "pass", atts, natt);
    }
    g_ms_samples = samples;
    for (uint32_t i = 0; i < natt; i++) {
        if (samples == 1) { ms_stale(atts[i]); continue; }
        MsTwin& t = ms_twin(cb, atts[i], samples);
        const bool colour = !(atts[i]->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        if (colour && !t.valid && atts[i]->written && !(g_clear_ahead >> i & 1) && !ms_is_dirty(atts[i])) ms_expand(cb, atts[i], t);
        t.valid = true;
        if (!ms_is_dirty(atts[i])) g_ms_dirty.push_back(atts[i]);
    }
    for (uint32_t i = 0; i < g_st.ncolors; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (Image* im = image_of(g_st.colors[i])) {
            transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
            ca[i].imageView = samples > 1 ? g_ms[im].view : im->view;
            ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            vk::mark_written(im);
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            w = std::min(w, im->w);
            h = std::min(h, im->h);
        }
        n = i + 1;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.layerCount = 1;
    ri.colorAttachmentCount = n;
    ri.pColorAttachments = ca;
    if (Image* im = image_of(g_st.depth)) {
        // upscaling twins only (MSAA is off then; other depths keep their old written logic): the other size is newer
        if (samples == 1 && im->twin && im->twin->wgen > im->wgen && !(g_clear_ahead >> 8 & 1)) twin_sync(cb, im);
        const VkImageLayout dl = ds_opt() && samples == 1 && !g_ds_feedback.count(im) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        transition(cb, im, dl);
        if (dl != VK_IMAGE_LAYOUT_GENERAL) g_pass_depth = im;
        if (im->twin) vk::mark_written(im);
        da.imageView = samples > 1 ? g_ms[im].view : im->view;
        da.imageLayout = dl;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
        w = std::min(w, im->w);
        h = std::min(h, im->h);
    }
    if (w == 0x7FFFFFFF) return;
    ri.renderArea = {{0, 0}, {w, h}};
    if (seq_on()) {
        std::string c;
        char t[80];
        for (uint32_t i = 0; i < g_st.ncolors; i++)
            if (Image* im = image_of(g_st.colors[i])) snprintf(t, sizeof t, " c%u=%p(%ux%u f%d)", i, (void*)im, im->w, im->h, (int)im->format), c += t;
        Image* d = image_of(g_st.depth);
        log("seq: pass x%u%s depth=%p(%ux%u f%d)%s clear_ahead %#x", samples, c.c_str(), (void*)d, d ? d->w : 0, d ? d->h : 0, d ? (int)d->format : 0, d ? (d->scale == res_scale() ? "" : " (not scene scale)") : "", g_clear_ahead);
        g_seq_draws = 0, g_seq_dsuse = 0;
    }
    if (g_pt_pool) pt_mark(cb, "pass " + pt_target());
    vkCmdBeginRendering(cb, &ri);
    g_st.rendering = true;
    g_pass_writes = 0;
}
static std::string pt_target() {  // the pass's first colour (or depth) target: size, format, samples
    Texture* t = nullptr;
    for (uint32_t i = 0; i < g_st.ncolors && !t; i++) t = g_st.colors[i];
    char b[96];
    Image* im = t ? image_of(t) : image_of(g_st.depth);
    if (!im) return "?";
    snprintf(b, sizeof b, "%s %ux%u fmt %d x%u c%u", t ? "col" : "depth-only", im->w, im->h, (int)im->format, g_ms_samples, g_st.ncolors);
    return b;
}

bool g_ui_phase = false;  // ultrawide: the frame's draws are UI now
static bool g_draw_picture;  // ultrawide: bind_resources saw a 16:9 picture texture (title / menu artwork, movie frame)
static uint32_t g_draw_art_w, g_draw_art_h;  // ultrawide: the largest fragment texture from the game's files (not rendered), any shape
static bool g_draw_rendered;  // ultrawide: a texture this draw samples was rendered by the GPU (minimap mask, screen copies)
static uint32_t g_ui_target;  // ultrawide: the texture the game copies into a window texture (scene + UI), 0 before the first
// ultrawide width factor of the bound render targets (1 unless they are widened 16:9 screen targets)
static float rt_sx() {
    for (uint32_t i = 0; i < g_st.ncolors; i++)
        if (Image* im = image_of(g_st.colors[i])) return im->sx;
    Image* d = image_of(g_st.depth);
    return d ? d->sx : 1;
}
// scale of the bound render targets (res_scale() or 1; all targets of a pass share it: same NVN flags)
static float rt_scale() {
    for (uint32_t i = 0; i < g_st.ncolors; i++)
        if (Image* im = image_of(g_st.colors[i])) return im->scale;
    Image* d = image_of(g_st.depth);
    return d ? d->scale : 1;
}

// Ryujinx's support buffer (set 0 binding 0 in every translated shader): gl_FragCoord is divided by render_scale[0],
// texelFetch coordinates are multiplied by render_scale[1 + texture index] and textureSize divided by it. Fragment
// textures come first, vertex ones after them (frag_scale_count); a negative fragment scale picks the sub-pixel from
// gl_FragCoord. Contents repeat across draws, so each distinct one is written once into a host-visible buffer.
struct SupportBuffer {
    int32_t alpha_test[4], is_bgra[8][4];
    float viewport_inverse[4], viewport_size[4];
    int32_t frag_scale_count[4];
    float render_scale[73][4];
    int32_t tfe_offset[4], tfe_vertex_count[4];
};
static_assert(sizeof(SupportBuffer) == 1392, "Ryujinx SupportBuffer layout");
static VkDescriptorBufferInfo support_buffer(const std::vector<float>& key) {  // rt scale, vp w, vp h, frag count, x factor, scales
    static std::map<std::vector<float>, VkDescriptorBufferInfo> made;
    static VkBuffer buf = VK_NULL_HANDLE;
    static uint8_t* map = nullptr;
    static VkDeviceSize used = 0;
    const VkDeviceSize slot = 1536, cap = slot * 4096;  // 1536: a multiple of every minUniformBufferOffsetAlignment
    static std::vector<float> last_key;  // runs of draws share the key: skip the map walk (replay thread only, like made)
    static VkDescriptorBufferInfo last_info{};
    if (last_info.buffer && key == last_key) return last_info;
    auto it = made.find(key);
    if (it != made.end()) { last_key = key, last_info = it->second; return last_info; }
    if (!buf) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf, &req);
        VkDeviceSize off;
        VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &off);
        VKC(vkBindBufferMemory(dev, buf, m, off));
        void* p;
        VKC(vkMapMemory(dev, m, off, cap, 0, &p));
        map = (uint8_t*)p;
    }
    if (used + slot > cap) {  // shortcut: never seen; reuses the first slot (an older draw's scales) when full
        static bool told = false;
        if (!told) log("vk: support buffer variants full (%llu)", (unsigned long long)(cap / slot)), told = true;
        return {buf, 0, sizeof(SupportBuffer)};
    }
    SupportBuffer sb{};
    float s = key[0], w = key[1], h = key[2];
    sb.viewport_inverse[0] = w ? 2 / w : 0;
    sb.viewport_inverse[1] = h ? 2 / h : 0;
    sb.viewport_inverse[2] = 1;
    sb.viewport_size[0] = w;
    sb.viewport_size[1] = h;
    sb.viewport_size[2] = key[4];  // FragCoord.x divisor besides the scale (ultrawide sx; translator patch)
    sb.frag_scale_count[0] = (int32_t)key[3];
    for (int i = 0; i < 73; i++) sb.render_scale[i][0] = 1;
    sb.render_scale[0][0] = s;
    for (size_t i = 5; i < key.size() && i - 4 < 73; i++) sb.render_scale[i - 4][0] = key[i];
    memcpy(map + used, &sb, sizeof sb);
    VkDescriptorBufferInfo info{buf, used, sizeof(SupportBuffer)};
    used += slot;
    made.emplace(key, info);
    return info;
}

// ------------------------------------------------------------------------------------------------- enums
static VkCompareOp cmp_op(uint32_t f) {  // NVN: 1 never .. 8 always (GL order; default depth func 2 = less)
    static const VkCompareOp t[] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                    VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                    VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return t[f < 9 ? f : 0];
}
static VkBlendFactor blend_factor(uint32_t f) {  // NVN: 1 zero, 2 one, ... (default src 2, dst 1); order assumed GL
    static const VkBlendFactor t[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC1_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    return f < sizeof t / sizeof t[0] ? t[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(uint32_t e) {  // NVN: 1 add, 2 subtract, 3 reverse subtract, 4 min, 5 max (assumed)
    static const VkBlendOp t[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                  VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return t[e < 6 ? e : 0];
}
static VkPrimitiveTopology topology(uint32_t m) {  // NVNdrawPrimitive, GL order (assumed)
    switch (m) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // 4 triangles; 7 quads are converted
    }
}
static VkFormat vertex_format_raw(uint32_t f);
// 3-component 8 / 16-bit vertex formats are optional in Vulkan (AMD's Windows driver lacks several): without the vertex-buffer
// feature the pipeline would fail. Use the 4-component format of the same size per component instead (the shader reads the
// same x, y, z; the extra component is the next bytes of the vertex, as other Switch emulators do).
static VkFormat vertex_supported(VkFormat v);
static VkFormat vertex_format(uint32_t f) { return vertex_supported(vertex_format_raw(f)); }
static VkFormat vertex_supported(VkFormat v) {
    static std::mutex m;
    static std::map<VkFormat, VkFormat> use;
    std::lock_guard<std::mutex> l(m);
    auto it = use.find(v);
    if (it != use.end()) return it->second;
    VkFormat r = v;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(vk::phys, v, &fp);
    static const bool force = getenv("NX_NO_RGB_VERTEX") && atoi(getenv("NX_NO_RGB_VERTEX"));
    const bool rgb = (v >= VK_FORMAT_R8G8B8_UNORM && v <= VK_FORMAT_R8G8B8_SRGB) || (v >= VK_FORMAT_R16G16B16_UNORM && v <= VK_FORMAT_R16G16B16_SFLOAT);
    if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) || (force && rgb)) {
        if (v >= VK_FORMAT_R8G8B8_UNORM && v <= VK_FORMAT_R8G8B8_SRGB) r = (VkFormat)(VK_FORMAT_R8G8B8A8_UNORM + (v - VK_FORMAT_R8G8B8_UNORM));
        else if (v >= VK_FORMAT_R16G16B16_UNORM && v <= VK_FORMAT_R16G16B16_SFLOAT) r = (VkFormat)(VK_FORMAT_R16G16B16A16_UNORM + (v - VK_FORMAT_R16G16B16_UNORM));
        log("vk: vertex format %d not supported by this GPU: using %d", (int)v, (int)r);
    }
    use[v] = r;
    return r;
}
static VkFormat vertex_format_raw(uint32_t f) {
    switch (f) {
        case 0x19: return VK_FORMAT_R8G8B8_UNORM;
        case 0x1a: return VK_FORMAT_R8G8B8_SNORM;
        case 0x1b: return VK_FORMAT_R8G8B8_UINT;
        case 0x1c: return VK_FORMAT_R8G8B8_SINT;
        case 0x1d: return VK_FORMAT_R16G16B16_SFLOAT;
        case 0x1e: return VK_FORMAT_R16G16B16_UNORM;
        case 0x1f: return VK_FORMAT_R16G16B16_SNORM;
        case 0x20: return VK_FORMAT_R16G16B16_UINT;
        case 0x21: return VK_FORMAT_R16G16B16_SINT;
        case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
        case 0x23: return VK_FORMAT_R32G32B32_UINT;
        case 0x24: return VK_FORMAT_R32G32B32_SINT;
        default: break;
    }
    if (f >= 0x51 && f <= 0x68) {  // integer -> float conversions, (unsigned, signed) pairs from R8 to RGBA32 (assumed)
        static const VkFormat t[] = {VK_FORMAT_R8_USCALED, VK_FORMAT_R8_SSCALED, VK_FORMAT_R16_USCALED, VK_FORMAT_R16_SSCALED,
                                     VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8_SSCALED,
                                     VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
                                     VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R16G16B16_USCALED,
                                     VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
                                     VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R16G16B16A16_USCALED,
                                     VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
        return t[f - 0x51];
    }
    return vk_format(f);
}

static uint8_t attr_type(VkFormat f) {  // Ryujinx AttributeType of the shader input a vertex format needs
    switch (f) {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_A2B10G10R10_SINT_PACK32:
            return 1;
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return 2;
        default:
            return 0;
    }
}

// ------------------------------------------------------------------------------------------------- pipelines
// Async pipeline compiles (setting async_pipelines, default on; NX_ASYNC_PIPELINES=0 = compile on the replay thread).
struct PipeJob {
    std::string share;  // g_pipe_shared key ("" = not shared)
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkVertexInputAttributeDescription> va;
    std::vector<VkVertexInputBindingDescription> vb;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[4];
    VkPipelineDynamicStateCreateInfo ds;
    VkFormat cf[8];
    VkPipelineRenderingCreateInfo pr;
    VkGraphicsPipelineCreateInfo pci;
    std::atomic<bool> done{false};
    bool warm = false;  // prewarm job: only fills the driver cache, the pipeline is destroyed at once
    VkPipeline result = VK_NULL_HANDLE;
    void fix() {  // the copies point at each other, not at the caller's locals
        vi.pVertexBindingDescriptions = vb.data(), vi.pVertexAttributeDescriptions = va.data();
        vp.pNext = &dcc, cb.pAttachments = cba, ds.pDynamicStates = dyn, pr.pColorAttachmentFormats = cf;
        pci.pNext = &pr, pci.pStages = stages.data(), pci.pVertexInputState = &vi, pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp, pci.pRasterizationState = &rs, pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &dss, pci.pColorBlendState = &cb, pci.pDynamicState = &ds;
    }
};
static std::map<Combo*, std::map<std::string, PipeJob*>> g_pipe_pending;  // replay thread only
static std::mutex g_pipe_lock;
static std::condition_variable g_pipe_cv, g_pipe_done_cv;
static std::deque<PipeJob*> g_pipe_queue;
static bool async_pipelines() {
    static const bool on = [] { const char* e = getenv("NX_ASYNC_PIPELINES"); return !e || atoi(e) != 0; }();
    return on;
}
static void pipe_worker() {
    // 2-core / 4-thread CPUs (#11): a compile storm after an update must not take time from the game / replay threads;
    // a late pipeline only skips a draw. Failure (no right) keeps normal priority.
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        PipeJob* j;
        {
            std::unique_lock<std::mutex> l(g_pipe_lock);
            g_pipe_cv.wait(l, [] { return !g_pipe_queue.empty(); });
            j = g_pipe_queue.front();
            g_pipe_queue.pop_front();
        }
        auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateGraphicsPipelines(dev, pipeline_cache(), 1, &j->pci, nullptr, &j->result);  // the cache is internally synchronized
        if (!j->warm) {
            g_pipeline_us += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g_pipeline_n++;
        } else if (j->result) {
            vkDestroyPipeline(dev, j->result, nullptr);
        }
        g_pipelines_total++;
        if (r != VK_SUCCESS) log("vk: pipeline creation failed (%d)", (int)r), j->result = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> l(g_pipe_lock);
            j->done = true;
        }
        g_pipe_done_cv.notify_all();
    }
}
static void pipeline_prewarm();
static void pipe_workers_start() {  // first pipeline request: workers + prewarm (also when every pipeline is an inline hit)
    static const bool started = [] {
        pipeline_cache();  // created once here, not by two workers at once
        const unsigned n = std::max(2u, std::min(6u, std::thread::hardware_concurrency() / 2));
        for (unsigned i = 0; i < n; i++) std::thread(pipe_worker).detach();
        log("vk: %u pipeline compile threads", n);
        std::thread(pipeline_prewarm).detach();
        return true;
    }();
    (void)started;
}
static void pipe_jobs_push(PipeJob* j) {
    pipe_workers_start();
    {
        std::lock_guard<std::mutex> l(g_pipe_lock);
        if (j->warm) g_pipe_queue.push_back(j);
        else g_pipe_queue.push_front(j);  // the game's own draws go before prewarm jobs
    }
    g_pipe_cv.notify_one();
}

// ------------------------------------------------------------------------------------------------- pipeline recipes
// <data>/cache/pipelines.recipes: every pipeline the game has made (shaders by SPIR-V hash + all fixed state). At start,
// the workers create them all again into the driver's pipeline cache (prewarm), so a place seen in any earlier run - or
// in a recipe file shipped with a release, with the matching shaders.bin - neither stalls nor pops in on a fresh driver
// cache (other users, a driver update). pipelines.bin alone is driver-specific; recipes work on any GPU.
struct RecipeStage { uint32_t vk_stage, in_pipeline; uint64_t layout_hash, module_hash; };
struct RecipeFixed {
    uint32_t nstages, nva, nvb, ncolors, topology, neg_one_to_one;
    VkPipelineRasterizationStateCreateInfo rs;  // pNext stored as null
    VkPipelineDepthStencilStateCreateInfo dss;
    VkPipelineColorBlendAttachmentState cba[8];
    VkFormat cf[8], depth, stencil;
};
static const uint32_t k_recipe_magic = 0x31525054;  // "TPR1"
static std::string recipes_path() { return g_data_dir + "/cache/pipelines.recipes"; }
static std::vector<std::string> recipes_read(bool with_seed) {
    std::vector<std::string> out;
    for (const std::string& p : {with_seed ? seed_dir() + "/pipelines.recipes" : std::string(), recipes_path()})
    if (FILE* f = p.empty() ? nullptr : fopen(p.c_str(), "rb")) {
        uint32_t magic = 0, n;
        if (fread(&magic, 4, 1, f) == 1 && magic == k_recipe_magic)
            while (fread(&n, 4, 1, f) == 1 && n < (1u << 20)) {
                std::string r(n, '\0');
                if (fread(r.data(), 1, n, f) != n) break;
                out.push_back(std::move(r));
            }
        fclose(f);
    }
    return out;
}
static void recipe_record(Combo* pv, const std::vector<VkPipelineShaderStageCreateInfo>& stages,
                          const std::vector<VkVertexInputAttributeDescription>& va, const std::vector<VkVertexInputBindingDescription>& vb,
                          const VkPipelineInputAssemblyStateCreateInfo& ia, const VkPipelineViewportDepthClipControlCreateInfoEXT& dcc,
                          const VkPipelineRasterizationStateCreateInfo& rs, const VkPipelineDepthStencilStateCreateInfo& dss,
                          const VkPipelineColorBlendAttachmentState cba[8], const VkPipelineRenderingCreateInfo& pr) {
    static FILE* f = nullptr;
    static std::set<std::string> known;
    static bool opened = false;
    if (!opened) {
        opened = true;
        for (auto& r : recipes_read(false)) known.insert(std::move(r));
        CreateDirectoryA((g_data_dir + "/cache").c_str(), nullptr);
        bool fresh = known.empty();  // missing or unreadable file: start it again
        f = fopen(recipes_path().c_str(), fresh ? "wb" : "ab");
        if (f && fresh) fwrite(&k_recipe_magic, 4, 1, f);
    }
    if (!f) return;
    std::vector<RecipeStage> rst;
    for (auto* s : pv->stages) {  // layout: every stage of the combo, as combo_of made it
        RecipeStage r{(uint32_t)s->vk_stage, 0, module_hash(s->module), 0};
        if (!r.layout_hash) return;
        for (auto& ps : stages)
            if (ps.stage == s->vk_stage) r.in_pipeline = 1, r.module_hash = module_hash(ps.module);
        if (r.in_pipeline && !r.module_hash) return;
        rst.push_back(r);
    }
    RecipeFixed fx{};
    fx.nstages = (uint32_t)rst.size(), fx.nva = (uint32_t)va.size(), fx.nvb = (uint32_t)vb.size();
    fx.ncolors = pr.colorAttachmentCount, fx.topology = ia.topology, fx.neg_one_to_one = dcc.negativeOneToOne;
    fx.rs = rs, fx.rs.pNext = nullptr, fx.dss = dss, fx.dss.pNext = nullptr;
    memcpy(fx.cba, cba, sizeof fx.cba);
    for (uint32_t i = 0; i < pr.colorAttachmentCount && i < 8; i++) fx.cf[i] = pr.pColorAttachmentFormats[i];
    fx.depth = pr.depthAttachmentFormat, fx.stencil = pr.stencilAttachmentFormat;
    std::string r((const char*)&fx, sizeof fx);
    r.append((const char*)rst.data(), rst.size() * sizeof(RecipeStage));
    r.append((const char*)va.data(), va.size() * sizeof va[0]);
    r.append((const char*)vb.data(), vb.size() * sizeof vb[0]);
    // MSAA variant: trailing sample count (older builds skip these longer records; prewarm builds them at the current setting)
    if (g_ms_samples > 1) r.append((const char*)&g_ms_samples, 4);
    if (!known.insert(r).second) return;
    uint32_t n = (uint32_t)r.size();
    fwrite(&n, 4, 1, f);
    fwrite(r.data(), 1, n, f);
    fflush(f);
}
static void pipeline_prewarm() {
    const char* e = getenv("NX_PREWARM");
    if (e && !atoi(e)) return;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::string> recipes = recipes_read(true);
    const uint32_t ms_samples_now = ms_wanted();
    std::sort(recipes.begin(), recipes.end());  // seed + own: each recipe once
    recipes.erase(std::unique(recipes.begin(), recipes.end()), recipes.end());
    std::map<uint64_t, VkShaderModule> modules;
    std::map<std::string, std::pair<VkDescriptorSetLayout, VkPipelineLayout>> layouts;
    std::vector<PipeJob*> jobs;
    size_t missing = 0;
    for (auto& r : recipes) {
        if (r.size() < sizeof(RecipeFixed)) continue;
        RecipeFixed fx;
        memcpy(&fx, r.data(), sizeof fx);
        const size_t base = sizeof fx + fx.nstages * sizeof(RecipeStage) + fx.nva * sizeof(VkVertexInputAttributeDescription) +
                            fx.nvb * sizeof(VkVertexInputBindingDescription);
        if (fx.ncolors > 8 || (r.size() != base && r.size() != base + 4)) continue;
        // MSAA recipe: built at today's sample count (8x seen, 4x set now: the 4x pipeline); MSAA off: the 1x one
        const uint32_t samples = r.size() == base + 4 ? ms_samples_now : 1;
        std::vector<RecipeStage> rst(fx.nstages);
        memcpy(rst.data(), r.data() + sizeof fx, rst.size() * sizeof(RecipeStage));
        PipeJob* j = new PipeJob;
        j->warm = true;
        std::vector<std::pair<VkShaderStageFlagBits, const Translated*>> ls;
        std::string lkey;
        bool ok = true;
        for (auto& s : rst) {
            const Translated* lt = translation_by_hash(s.layout_hash);
            if (!lt) { ok = false; break; }
            ls.push_back({(VkShaderStageFlagBits)s.vk_stage, lt});
            lkey.append((const char*)&s.vk_stage, 4).append((const char*)&s.layout_hash, 8);
            if (!s.in_pipeline) continue;
            VkShaderModule& m = modules[s.module_hash];
            if (!m) {
                const Translated* t = translation_by_hash(s.module_hash);
                if (!t) { ok = false; break; }
                m = make_module(*t, false);  // destroyed after the prewarm
            }
            j->stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, (VkShaderStageFlagBits)s.vk_stage, m, "main"});
        }
        if (!ok || j->stages.empty()) { missing++; delete j; continue; }
        auto& lay = layouts[lkey];
        if (!lay.second) make_layout(ls, &lay.first, &lay.second);
        const char* p = r.data() + sizeof fx + rst.size() * sizeof(RecipeStage);
        j->va.resize(fx.nva), j->vb.resize(fx.nvb);
        memcpy(j->va.data(), p, fx.nva * sizeof j->va[0]);
        memcpy(j->vb.data(), p + fx.nva * sizeof j->va[0], fx.nvb * sizeof j->vb[0]);
        // recipes may come from another GPU (shipped seed): the formats this one lacks, as the live path converts them
        for (auto& a : j->va) a.format = vertex_supported(a.format);
        fx.depth = fx.depth ? depth_format(fx.depth) : fx.depth, fx.stencil = fx.stencil ? depth_format(fx.stencil) : fx.stencil;
        // the same create infos pipeline() builds, from the recorded state
        j->vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        j->vi.vertexBindingDescriptionCount = fx.nvb, j->vi.vertexAttributeDescriptionCount = fx.nva;
        j->ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        j->ia.topology = (VkPrimitiveTopology)fx.topology;
        j->dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
        j->dcc.negativeOneToOne = fx.neg_one_to_one;
        j->vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        j->vp.viewportCount = j->vp.scissorCount = 1;
        j->rs = fx.rs, j->dss = fx.dss;
        j->ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        j->ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
        memcpy(j->cba, fx.cba, sizeof fx.cba), memcpy(j->cf, fx.cf, sizeof fx.cf);
        j->cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        j->cb.attachmentCount = fx.ncolors;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                      VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        memcpy(j->dyn, dyn, sizeof dyn);
        j->ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        j->ds.dynamicStateCount = 4;
        j->pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        j->pr.colorAttachmentCount = fx.ncolors;
        j->pr.depthAttachmentFormat = fx.depth, j->pr.stencilAttachmentFormat = fx.stencil;
        j->pci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        j->pci.stageCount = (uint32_t)j->stages.size();
        j->pci.layout = lay.second;
        j->fix();
        jobs.push_back(j);
    }
    if (recipes.empty()) return;
    log("vk: prewarm: %zu pipeline recipes, %zu queued, %zu without shaders in shaders.bin", recipes.size(), jobs.size(), missing);
    for (auto* j : jobs) pipe_jobs_push(j);
    {
        std::unique_lock<std::mutex> l(g_pipe_lock);
        g_pipe_done_cv.wait(l, [&] {
            for (auto* j : jobs)
                if (!j->done) return false;
            return true;
        });
    }
    for (auto* j : jobs) delete j;
    for (auto& [h, m] : modules) vkDestroyShaderModule(dev, m, nullptr);
    // layouts stay: make_layout shares one pair per binding list with the live combos
    log("vk: prewarm done: %zu pipelines in %.1f s", jobs.size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    pipeline_cache_save();
}
// The pipeline once its job is done (waits a few ms per frame for it, 6 ms in all per 16 ms), else null: skip the draw.
// Pipelines shared by content (s85). The game loads the same shaders at new addresses (battles: enemy / effect shaders
// each time), which makes new programs and combos (program_of keys on the code address: the constant data and vertex
// re-translations read the program's own memory). Each combo then created all its pipelines again: one mash battle made
// ~10000 pipelines (all driver cache hits) and the process kept ~2.7 GB more host memory for the rest of the run. Now a
// pipeline is also found by its content: the state key + the SPIR-V hash of every stage module. Combos with the same
// translations have identically defined layouts, which Vulkan treats as compatible, so the shared VkPipeline binds
// with either layout. Replay thread only (as g_pipe_pending). NX_PIPE_SHARE=0: off.
static std::unordered_map<std::string, VkPipeline> g_pipe_shared;
std::atomic<uint32_t> g_pipe_shared_hits{0};  // frame stats (vk.cpp)

static VkPipeline pipe_wait(Combo* pv, const std::string& key) {
    static auto window = std::chrono::steady_clock::now();
    static std::chrono::microseconds spent{0};
    static uint32_t skipped = 0;
    auto& pend = g_pipe_pending[pv];
    PipeJob* j = pend[key];
    auto now = std::chrono::steady_clock::now();
    if (now - window > std::chrono::milliseconds(16)) window = now, spent = {};
    if (!j->done && spent < std::chrono::milliseconds(6)) {
        HitchTimer ht{HITCH_PIPELINE};
        std::unique_lock<std::mutex> l(g_pipe_lock);
        g_pipe_done_cv.wait_for(l, std::chrono::milliseconds(6) - spent, [j] { return j->done.load(); });
        spent += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - now);
    }
    if (!j->done) {
        if ((++skipped & 255) == 1) log("vk: draws waiting for a pipeline compile: %u so far", skipped);
        return VK_NULL_HANDLE;
    }
    VkPipeline p = j->result;
    pv->pipelines[key] = p;
    if (p && !j->share.empty()) g_pipe_shared.emplace(j->share, p);
    pend.erase(key);
    delete j;
    return p;
}
static VkPipeline pipeline(Combo* pv, uint32_t mode) {
    // the key is every byte of state the pipeline is built from (kept per thread: one heap allocation less per draw)
    thread_local std::string key;
    key.clear();
    auto put = [&](const void* p, size_t n) { key.append((const char*)p, n); };
    VkFormat cf[8] = {};
    for (uint32_t i = 0; i < g_st.ncolors; i++) if (Image* im = image_of(g_st.colors[i])) cf[i] = im->format;
    Image* dim = image_of(g_st.depth);
    VkFormat df = dim ? dim->format : VK_FORMAT_UNDEFINED;
    put(cf, sizeof cf); put(&df, sizeof df); put(&g_st.ncolors, 4); put(&mode, 4);
    if (g_ms_samples > 1) put(&g_ms_samples, 4);  // MSAA variant (1x keys unchanged: same cache entries as before)
    const uint32_t alpha_test = (g_st.color >> 24 & 0xF) >= 1 && (g_st.color >> 24 & 0xF) <= 7 ? g_st.color >> 24 & 0xF : 0;
    if (alpha_test) put(&g_st.alpha_ref, 4);  // only then: other pipelines keep their keys
    put(g_st.blend, sizeof g_st.blend); put(&g_st.color, 4); put(&g_st.channel, 4); put(g_st.ds, 8); put(&g_st.polygon, 4);
    put(g_st.attribs.data(), 4 * g_st.attribs.size());
    put(g_st.streams.data(), 4 * g_st.streams.size());
    if (pv->last_pipe && pv->last_key == key) return pv->last_pipe;
    auto it = pv->pipelines.find(key);
    if (it != pv->pipelines.end()) {
        if (it->second) pv->last_key = key, pv->last_pipe = it->second;
        return it->second;
    }
    if (auto pc = g_pipe_pending.find(pv); pc != g_pipe_pending.end() && pc->second.count(key)) return pipe_wait(pv, key);
    static const bool pipe_dbg = getenv("NX_PIPE_DEBUG") != nullptr;
    if (pipe_dbg && pv->pipelines.size() >= 4) {  // which key bytes differ from an existing pipeline of this combo
        const std::string& o = pv->pipelines.begin()->first;
        std::string d;
        char b[32];
        for (size_t i = 0; i < std::max(o.size(), key.size()) && d.size() < 200; i++)
            if (i >= o.size() || i >= key.size() || o[i] != key[i]) {
                snprintf(b, sizeof b, " %zu:%02x>%02x", i, i < o.size() ? (uint8_t)o[i] : 0, i < key.size() ? (uint8_t)key[i] : 0);
                d += b;
            }
        log("vk: pipeline miss combo %p (%zu) len %zu/%zu blend %zu:%s", (void*)pv, pv->pipelines.size(), o.size(), key.size(), sizeof g_st.blend, d.c_str());
    }

    // integer vertex formats (bone indices are RGBA8UI) need integer shader inputs: Ryujinx AttributeType 1 sint, 2 uint
    uint8_t attr_types[32] = {};
    for (uint32_t i = 0; i < g_st.attribs.size() && i < 32; i++)
        if (uint32_t f = (g_st.attribs[i] >> 21) & 0x3FF; (g_st.attribs[i] & 0x40) && !(f >= 0x51 && f <= 0x68))  // not the *2F conversions
            attr_types[i] = attr_type(vertex_format(f));
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    bool has_vs = false;
    for (auto* s : pv->stages) {
        if (s->vk_stage == VK_SHADER_STAGE_COMPUTE_BIT) continue;
        has_vs |= s->vk_stage == VK_SHADER_STAGE_VERTEX_BIT;
        VkShaderModule m = s->vk_stage == VK_SHADER_STAGE_VERTEX_BIT ? vertex_module(*s, attr_types)
                         : s->vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT && alpha_test ? alpha_module(*s, alpha_test, g_st.alpha_ref) : s->module;
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, s->vk_stage, m, "main"});
    }
    if (!has_vs) return VK_NULL_HANDLE;
    static const bool share_on = !getenv("NX_PIPE_SHARE") || atoi(getenv("NX_PIPE_SHARE")) != 0;
    std::string share;
    if (share_on) {
        share = key;
        for (auto& st : stages) {
            const uint64_t h = module_hash(st.module);
            if (!h) { share.clear(); break; }
            share.append((const char*)&st.stage, sizeof st.stage).append((const char*)&h, sizeof h);
        }
        if (!share.empty())
            if (auto sh = g_pipe_shared.find(share); sh != g_pipe_shared.end()) {
                g_pipe_shared_hits++;
                pv->pipelines[key] = sh->second;
                return sh->second;
            }
    }
    // vertex input: attrib word = stream (0-4) | enabled (6) | relative offset (7-20) | format (21-30)
    std::vector<VkVertexInputAttributeDescription> va;
    std::vector<VkVertexInputBindingDescription> vb;
    uint32_t used_streams = 0;
    for (uint32_t i = 0; i < g_st.attribs.size(); i++) {
        uint32_t w = g_st.attribs[i];
        if (!(w & 0x40)) continue;
        uint32_t s = w & 0x1F;
        va.push_back({i, s, vertex_format((w >> 21) & 0x3FF), (w >> 7) & 0x3FFF});
        used_streams |= 1u << s;
    }
    for (uint32_t s = 0; s < 16; s++)
        if (used_streams & (1u << s)) {
            uint32_t stride = 2 * s < g_st.streams.size() ? g_st.streams[2 * s] : 0;
            uint32_t div = 2 * s + 1 < g_st.streams.size() ? g_st.streams[2 * s + 1] : 0;
            vb.push_back({s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)vb.size();
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)va.size();
    vi.pVertexAttributeDescriptions = va.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology(mode);
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = g_depth_mode == 0;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, &dcc};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    // polygon state: cull face (0-1: 0 none, 1 front, 2 back, 3 both), front face (2: 1 = CCW), mode (3-4)
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = ((g_st.polygon >> 3) & 3) == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = (VkCullModeFlags)(g_st.polygon & 3);
    rs.frontFace = (g_st.polygon & 4) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = (VkSampleCountFlagBits)g_ms_samples;
    // depth-stencil: word0 depth test (0), depth write (1), stencil test (2), depth func (4-7); word1 stencil
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = (g_st.ds[0] & 1) && dim;
    dss.depthWriteEnable = ((g_st.ds[0] >> 1) & 1) && dim;
    dss.depthCompareOp = cmp_op((g_st.ds[0] >> 4) & 0xF);
    dss.stencilTestEnable = ((g_st.ds[0] >> 2) & 1) && dim && (dim->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    dss.front.compareOp = cmp_op(g_st.ds[1] & 0xF);
    dss.back.compareOp = cmp_op((g_st.ds[1] >> 16) & 0xF);
    dss.front.compareMask = dss.back.compareMask = 0xFF;
    dss.front.writeMask = dss.back.writeMask = 0xFF;
    if (dss.stencilTestEnable) {  // only here: pipelines without the test keep their driver-cache hash
        // NVN word1, front low half / back high half: func 0-3, fail 4-7, depth pass 8-11, depth fail 12-15 (driver
        // nvnDepthStencilStateSetStencilOp 0x21bcdf4). Enum 1 keep, 2 zero, 3 replace, 4 incr, 5 decr, 6 invert, 7 incr
        // wrap, 8 decr wrap. Shadow casters REPLACE on pass: the character ids in the map keep a character out of its
        // own shadow (face shader: lit if id == c7[2].x); unwritten they gave blotchy dark faces (user 11-06).
        const auto sop = [](uint32_t n) {
            static const VkStencilOp t[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
            return t[n < 9 ? n : 0];
        };
        dss.front.failOp = sop(g_st.ds[1] >> 4 & 0xF), dss.front.passOp = sop(g_st.ds[1] >> 8 & 0xF), dss.front.depthFailOp = sop(g_st.ds[1] >> 12 & 0xF);
        dss.back.failOp = sop(g_st.ds[1] >> 20 & 0xF), dss.back.passOp = sop(g_st.ds[1] >> 24 & 0xF), dss.back.depthFailOp = sop(g_st.ds[1] >> 28 & 0xF);
    }
    // colour blend: ColorState blend-enable bits 8-15; BlendState word0 eq colour 24-26 / alpha 28-30, word1 factors
    VkPipelineColorBlendAttachmentState cba[8]{};
    for (uint32_t i = 0; i < g_st.ncolors; i++) {
        uint32_t w0, w1;
        memcpy(&w0, g_st.blend[i], 4);
        memcpy(&w1, g_st.blend[i] + 4, 4);
        if (!w0 && !w1) { w0 = 0x110c0000; w1 = 0x01020102; }
        cba[i].blendEnable = (g_st.color >> (8 + i)) & 1;
        cba[i].srcColorBlendFactor = blend_factor(w1 & 0x7F);
        cba[i].dstColorBlendFactor = blend_factor((w1 >> 8) & 0x7F);
        cba[i].srcAlphaBlendFactor = blend_factor((w1 >> 16) & 0x7F);
        cba[i].dstAlphaBlendFactor = blend_factor((w1 >> 24) & 0x7F);
        cba[i].colorBlendOp = blend_op((w0 >> 24) & 7);
        cba[i].alphaBlendOp = blend_op((w0 >> 28) & 7);
        cba[i].colorWriteMask = (g_st.channel >> (4 * i)) & 0xF;
        // NX_DEBUG_SKIN: only draws with RGBA8UI bone indices at location 7 (multi-bone skinning), without depth test
        static const bool dbg_skin = getenv("NX_DEBUG_SKIN") != nullptr;
        bool skin = dbg_skin && g_st.attribs.size() > 7 && (g_st.attribs[7] & 0x40) && ((g_st.attribs[7] >> 21) & 0x3FF) == 0x27;
        if (skin) dss.depthTestEnable = VK_FALSE;
        static const bool dbg_blend = getenv("NX_DEBUG_BLEND") != nullptr;
        if (dbg_blend || skin) {  // every covered pixel becomes the blend constant (magenta): tests geometry only
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
            cba[i].dstColorBlendFactor = cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba[i].colorBlendOp = cba[i].alphaBlendOp = VK_BLEND_OP_ADD;
            cba[i].colorWriteMask = 0xF;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = g_st.ncolors;
    cb.pAttachments = cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 4;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = g_st.ncolors;
    pr.pColorAttachmentFormats = cf;
    bool hasd = dim && (dim->aspect & VK_IMAGE_ASPECT_DEPTH_BIT), hass = dim && (dim->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    pr.depthAttachmentFormat = hasd ? df : VK_FORMAT_UNDEFINED;
    pr.stencilAttachmentFormat = hass ? df : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr};
    pci.stageCount = (uint32_t)stages.size();
    pci.pStages = stages.data();
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &dss;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &ds;
    pci.layout = pv->layout;
    recipe_record(pv, stages, va, vb, ia, dcc, rs, dss, cba, pr);
    if (async_pipelines() && g_vk_pipe_cache_ctl) {
        // Already in the pipeline cache (prewarm, an earlier run): create it here, ~40 us. Handing these to a worker and
        // waiting cost ~10 ms per frame when hundreds appear at once (Shibuya walk, s80). Real compiles still go async.
        pipe_workers_start();
        HitchTimer ht{HITCH_PIPELINE};
        auto t0 = std::chrono::steady_clock::now();
        VkPipeline p = VK_NULL_HANDLE;
        pci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        VkResult r = vkCreateGraphicsPipelines(dev, pipeline_cache(), 1, &pci, nullptr, &p);
        pci.flags &= ~VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        if (r == VK_SUCCESS && p) {
            g_pipeline_us += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            g_pipeline_n++, g_pipelines_total++, g_pipeline_inline++;
            pv->pipelines[key] = p;
            if (!share.empty()) g_pipe_shared.emplace(share, p);
            return p;
        }
    }
    if (async_pipelines()) {
        // A new pipeline the driver has not compiled before took 50-150 ms here, one after another, so entering a new
        // area stalled the frame for seconds in all (user run 10-30: 73 of 81 hitches, 11 s). Now worker threads
        // compile (several at once) and the frame waits only a few ms for them; a draw whose pipeline is not ready
        // yet is skipped (shows a frame or a few later) instead of stopping the game. Driver cache hits are ready
        // within the wait, so known content looks the same as before.
        PipeJob* j = new PipeJob;
        j->stages = stages, j->va = va, j->vb = vb, j->vi = vi, j->ia = ia, j->dcc = dcc, j->vp = vp, j->rs = rs, j->ms = ms;
        j->dss = dss, j->cb = cb, j->ds = ds, j->pr = pr, j->pci = pci;
        memcpy(j->cba, cba, sizeof cba), memcpy(j->cf, cf, sizeof cf), memcpy(j->dyn, dyn, sizeof dyn);
        j->fix();
        j->share = share;
        g_pipe_pending[pv][key] = j;
        pipe_jobs_push(j);
        return pipe_wait(pv, key);
    }
    VkPipeline p = VK_NULL_HANDLE;
    HitchTimer ht{HITCH_PIPELINE};
    auto t0 = std::chrono::steady_clock::now();
    VkResult r = vkCreateGraphicsPipelines(dev, pipeline_cache(), 1, &pci, nullptr, &p);
    g_pipeline_us += (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    g_pipeline_n++;
    g_pipelines_total++;
    if (r != VK_SUCCESS) log("vk: pipeline creation failed (%d)", (int)r);
    pv->pipelines[key] = p;
    if (p && !share.empty()) g_pipe_shared.emplace(share, p);
    return p;
}

// ------------------------------------------------------------------------------------------------- descriptors
static std::vector<VkDescriptorPool> g_dpool_sets[3];  // per submit slot (vk.cpp), reset when the slot is reused
static std::vector<VkDescriptorPool>* g_dpools_p = &g_dpool_sets[0];
static size_t g_dpool_cur = 0;
void descriptors_reset(int slot) {
    g_dpools_p = &g_dpool_sets[slot];
    for (auto p : (*g_dpools_p)) vkResetDescriptorPool(dev, p, 0);
    g_dpool_cur = 0;
}
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l) {
    for (;;) {
        if (g_dpool_cur == (*g_dpools_p).size()) {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}};
            VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            ci.maxSets = 4096;
            ci.poolSizeCount = 4;
            ci.pPoolSizes = sizes;
            VkDescriptorPool p;
            VKC(vkCreateDescriptorPool(dev, &ci, nullptr, &p));
            (*g_dpools_p).push_back(p);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, (*g_dpools_p)[g_dpool_cur], 1, &l};
        VkDescriptorSet s;
        if (vkAllocateDescriptorSets(dev, &ai, &s) == VK_SUCCESS) return s;
        g_dpool_cur++;
    }
}

// one lookup per texture binding: hashed on the builder words, no allocation (s71: the string key cost heap time per draw)
struct SamplerKey {
    uint32_t w[18];
    bool operator==(const SamplerKey& o) const { return !memcmp(w, o.w, sizeof w); }
};
struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : k.w) h = (h ^ v) * 1099511628211ull;
        return (size_t)h;
    }
};
static VkSampler sampler_of(const Sampler& s) {
    static_assert(sizeof(SamplerKey::w) == sizeof(s.words), "sampler key covers the builder copy");
    static std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> cache;
    SamplerKey key;
    memcpy(key.w, s.words, sizeof key.w);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    // builder copy: min @4, mag @8, wrap s/t/r @0xc..0x14, lod clamp @0x18/0x1c, bias @0x20, compare @0x24/0x28,
    // max anisotropy @0x3c (word indexes = offset / 4); filter / wrap enum orders assumed GL-like
    const uint32_t* w = s.words;
    auto wrap = [](uint32_t m) {
        switch (m) {
            case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    uint32_t minf = w[1], magf = w[2];
    ci.magFilter = magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (minf == 1 || minf == 3 || minf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (minf == 4 || minf == 5) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(w[3]);
    ci.addressModeV = wrap(w[4]);
    ci.addressModeW = wrap(w[5]);
    float mn, mx, bias, aniso;
    memcpy(&mn, &w[6], 4); memcpy(&mx, &w[7], 4); memcpy(&bias, &w[8], 4); memcpy(&aniso, &w[15], 4);
    ci.minLod = mn;
    ci.maxLod = (minf <= 1) ? 0.25f : mx;
    ci.mipLodBias = bias;
    static const bool lod0 = getenv("NX_LOD0") != nullptr;  // debug: sample level 0 only (tells bad mip data apart)
    if (lod0) ci.minLod = ci.maxLod = 0;
    if (aniso > 1.0f) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min(aniso, 16.0f); }
    if (w[9]) { ci.compareEnable = VK_TRUE; ci.compareOp = cmp_op(w[10]); }
    // border colour @0x2c..0x38 (float or int bits; 0x3f800000 = 1.0f): only Vulkan's fixed colours, others logged
    bool one[4], zero[4];
    for (int i = 0; i < 4; i++) { one[i] = w[11 + i] == 0x3f800000u || w[11 + i] == 1u; zero[i] = w[11 + i] == 0; }
    if (one[0] && one[1] && one[2] && one[3]) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (zero[0] && zero[1] && zero[2] && one[3]) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (w[9] && one[0]) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // depth compare reads only R
    else if (!(zero[0] && zero[1] && zero[2] && zero[3]))
        log("vk: sampler border colour %08x %08x %08x %08x not supported, using transparent black", w[11], w[12], w[13], w[14]);
    VkSampler smp;
    VKC(vkCreateSampler(dev, &ci, nullptr, &smp));
    cache[key] = smp;
    return smp;
}

// texture / sampler pool lookups live in nvn_hle.cpp
bool pool_texture(uint32_t pool, uint32_t id, Texture** tex, TexView* view);
bool pool_sampler(uint32_t pool, uint32_t id, Sampler* s);

// ------------------------------------------------------------------------------------------------- draws
static VkBuffer g_quad_ib = VK_NULL_HANDLE;
static void quad_indices(VkCommandBuffer cb, uint32_t first, uint32_t count, uint32_t* n_out) {
    // ponytail: one host-visible index buffer for quad lists (0,1,2, 0,2,3 per quad), sized for 64k quads
    static VkDeviceSize cap = 0;
    static uint32_t* map = nullptr;
    if (!g_quad_ib) {
        cap = 65536ull * 6 * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &g_quad_ib));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, g_quad_ib, &req);
        VkDeviceSize off;
        VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &off);
        VKC(vkBindBufferMemory(dev, g_quad_ib, m, off));
        void* p;
        VKC(vkMapMemory(dev, m, off, cap, 0, &p));
        map = (uint32_t*)p;
        for (uint32_t q = 0; q < 65536; q++) {
            uint32_t b = 4 * q;
            uint32_t* o = map + 6 * q;
            o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
        }
    }
    vkCmdBindIndexBuffer(cb, g_quad_ib, 0, VK_INDEX_TYPE_UINT32);
    *n_out = count / 4 * 6;
    (void)first;
}

static uint64_t g_skip[6];  // no program / no render target / no pipeline / u8 indices / other
int g_log_draws = 3;  // log resource bindings of the first draws that have any (NX_LOG_DRAWS: a whole frame)
static bool g_log_bindings = false;

// Storage buffers reach Maxwell code as addresses in driver constant buffer c0; the translator reports where
// (slot | word offset << 8). Measured on the compute light-binning shader: uniform buffer i at c0 + 0x210 + 16 i (glslc
// reaches uniform buffers past the 8 hardware slots this way), storage buffer i at c0 + 0x310 + 16 i. Graphics stages
// (measured on forward+ material fragment shaders, which read the light list the game binds as storage buffer 0):
// storage buffer i at c0 + 0x510 + 16 i.
static Binding global_binding(uint32_t nst, int32_t handle) {
    uint32_t slot = handle & 0xFF, off = (uint32_t)(handle >> 8) * 4;
    uint32_t sbase = nst == 5 ? 0x310 : 0x510;  // NVN stage 5 = compute
    if (slot != 0 || off < 0x210) return {};
    if (off < 0x310) return g_st.ubuf[nst][(off - 0x210) / 16];
    if (off < sbase) return {};
    uint32_t i = (off - sbase) / 16;
    return i < 16 ? g_st.sbuf[nst][i] : Binding{};
}

static VkDescriptorSet bind_resources(VkCommandBuffer cb, Combo* pv);
static uint64_t g_dispatches = 0;
static void dispatch(VkCommandBuffer cb, const Cmd& k) {  // groups x, y, z
    Program* progs[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, g_st.programs[5]};
    Combo* pv = combo_of(progs);
    if (!pv || pv->stages.empty()) return;
    VkPipeline& p = pv->pipelines["compute"];
    if (!p) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, pv->stages[0]->module, "main"};
        ci.layout = pv->layout;
        if (vkCreateComputePipelines(dev, pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) { log("vk: compute pipeline creation failed"); return; }
    }
    VkDescriptorSet set = bind_resources(cb, pv);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pv->layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, std::max(1u, k.a[0]), std::max(1u, k.a[1]), std::max(1u, k.a[2]));
    full_barrier(cb);
    if (g_dispatches++ < 3) log("vk: dispatch %ux%ux%u", k.a[0], k.a[1], k.a[2]);
}

// one descriptor set with every resource of every stage in the combo, filled from the current bindings
// The game's FXAA pass (after tone mapping, before the UI) takes its pixel size from uniform 1 in 1280x720 units:
// fragment 48 bytes {0.5/w, 0.5/h, 2/w, 2/h, 1/w, 1/h, 0, sharpness 8, thresholds...}, vertex 16 bytes {1/w, 1/h}.
// At a render scale s that filters s target pixels wide: a soft image and bright/dark halos along edges (user report,
// 4K). Divide them by s in guest memory for this draw (the values are rewritten every frame; a rescaled block no
// longer matches, so it is never scaled twice). Settings menu "Anti-aliasing" off (fx_fxaa = 0): the edge thresholds
// (words 8, 9: 0.125 and 0.05 in FXAA 3.11 console terms) go to 100, so every pixel takes FXAA's early "no edge" exit
// and passes through unchanged.
// the FXAA draw's fragment constants (guest address; 0: another draw), vert_out: its vertex constants. Matches only
// unscaled constants, i.e. before this frame's fxaa_rescale.
static uint32_t fxaa_frag(Combo* pv, uint32_t* vert_out = nullptr) {
    auto is = [](uint32_t a, int i, float d) { float x; uint32_t w = ld32(a + 4 * i); memcpy(&x, &w, 4); return fabsf(x * d - 1) < 1e-3f; };
    uint32_t frag = 0, vert = 0;
    for (auto* st : pv->stages) {
        uint32_t nst = nvn_stage(st->stage);
        Binding b = g_st.ubuf[nst][1];
        if (!b.addr) continue;
        uint32_t a = (uint32_t)b.addr;
        if (nst == 1 && b.size >= 24 && is(a, 0, 2560) && is(a, 1, 1440) && is(a, 2, 640) && is(a, 3, 360) && is(a, 4, 1280) && is(a, 5, 720)) frag = a;
        if (nst == 0 && b.size >= 8 && is(a, 0, 1280) && is(a, 1, 720)) vert = a;
    }
    if (vert_out) *vert_out = vert;
    return frag;
}
static bool g_smaa_ran;  // draw(): SMAA did this FXAA draw's work (fxaa_rescale makes FXAA a pass-through)
static void fxaa_rescale(Combo* pv, float s) {
    uint32_t vert = 0;
    const uint32_t frag = fxaa_frag(pv, &vert);
    if (!frag) return;
    static bool told = false;
    if (!told) {
        float f[12];
        for (int i = 0; i < 12; i++) { uint32_t w = ld32(frag + 4 * i); memcpy(&f[i], &w, 4); }
        log("vk: FXAA pass found (scale %.0f): fragment constants %g %g %g %g %g %g %g %g %g %g %g %g", s, f[0], f[1], f[2], f[3],
            f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
        told = true;
    }
    if (!fx_enabled(FX_FXAA) || g_smaa_ran) { float off = 100; uint32_t w; memcpy(&w, &off, 4); st32(frag + 32, w); st32(frag + 36, w); }
    if (s <= 1) return;
    auto div = [s](uint32_t a, int n) { for (int i = 0; i < n; i++) { float x; uint32_t w = ld32(a + 4 * i); memcpy(&x, &w, 4); x /= s; memcpy(&w, &x, 4); st32(a + 4 * i, w); } };
    div(frag, 6);
    if (vert) div(vert, 2);
}

// SMAA (setting smaa, Anti-aliasing = SMAA): at the game's FXAA draw (after tone mapping, before the UI) our three
// compute passes (tools/spvgen/Smaa.cs -> smaa0/1/2_spv.h: edges, blend weights, neighbour blend) rewrite the FXAA
// input in place (from a copy of it), and FXAA itself becomes a pass-through (thresholds, fxaa_rescale). The input is
// B10G11R11_UFLOAT (measured 11-05; blend variant smaa3) or R8G8B8A8_UNORM (smaa2); anything else keeps FXAA.
struct SmaaPass { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; };
static SmaaPass smaa_pipe(const uint32_t* spv, size_t size) {
    SmaaPass p{};
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, size, spv};
    VkShaderModule mod;
    VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
    // sampled inputs as combined image samplers: the per-frame descriptor pools (alloc_set) hold that kind
    VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, b};
    VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &p.dsl));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &p.dsl, 1, &pr};
    VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &p.layout));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
    ci.layout = p.layout;
    VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipe));
    return p;
}
}  // namespace nvn
std::atomic<int> rt::g_shadow_detail{getenv("NX_SHADOW_DETAIL") ? atoi(getenv("NX_SHADOW_DETAIL")) : 0};  // setting shadow_detail: 0 full, 1 low
std::atomic<int> rt::g_skip_shadow{0};  // NX_CONTROL "skipshadow N" (debug): 1 casters off, 2 off every other frame
namespace nvn {
static bool smaa_in_place(VkCommandBuffer cb, Image* im) {
    static SmaaPass pass[4];
    static bool made = false;
    static Image::Own copy{}, edges{}, weights{};
    if (!made) {
        pass[0] = smaa_pipe(k_smaa0_spv, sizeof k_smaa0_spv);
        pass[1] = smaa_pipe(k_smaa1_spv, sizeof k_smaa1_spv);
        pass[2] = smaa_pipe(k_smaa2_spv, sizeof k_smaa2_spv);
        pass[3] = smaa_pipe(k_smaa3_spv, sizeof k_smaa3_spv);
        made = true;
        log("vk: SMAA passes ready");
    }
    if (copy.w != im->w || copy.h != im->h || copy.format != im->format) {  // the game's frame size changed: new work images (old ones once idle)
        for (Image::Own* o : {&copy, &edges, &weights})
            if (o->image) defer_destroy(o->image, o->view, {}, o->mem, o->mem_off, o->mem_size);
        copy = new_image_2d(im->format, im->w, im->h, 1);
        edges = new_image_2d(VK_FORMAT_R8G8B8A8_UNORM, im->w, im->h, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        weights = new_image_2d(VK_FORMAT_R8G8B8A8_UNORM, im->w, im->h, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        VkImageMemoryBarrier ib[3];
        Image::Own* o3[3] = {&copy, &edges, &weights};
        for (int i = 0; i < 3; i++) {
            ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     o3[i]->image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            o3[i]->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 3, ib);
        log("vk: SMAA work images %ux%u", im->w, im->h);
    }
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    full_barrier(cb);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {im->w, im->h, 1};
    vkCmdCopyImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
    // threshold 0.05 on luma; up to 32 pixels each way along an edge (NX_SMAA_STEPS)
    static const int steps = getenv("NX_SMAA_STEPS") ? std::clamp(atoi(getenv("NX_SMAA_STEPS")), 1, 256) : 32;
    struct { int32_t w, h; float thr; int32_t steps; } pc{(int32_t)im->w, (int32_t)im->h, 0.05f, steps};
    const VkImageView in_a[3] = {copy.view, edges.view, copy.view}, in_b[3] = {copy.view, copy.view, weights.view},
                      out[3] = {edges.view, weights.view, im->view};
    const VkSampler smp = sampler_of(Sampler{});
    for (int i = 0; i < 3; i++) {
        VkDescriptorSet set = alloc_set(pass[i].dsl);
        VkDescriptorImageInfo ii[3] = {{smp, in_a[i], VK_IMAGE_LAYOUT_GENERAL}, {smp, in_b[i], VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, out[i], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[3];
        for (int j = 0; j < 3; j++)
            wr[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, (uint32_t)j, 0, 1,
                     j < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[j], nullptr, nullptr};
        vkUpdateDescriptorSets(dev, 3, wr, 0, nullptr);
        const SmaaPass& ps = i == 2 && im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? pass[3] : pass[i];
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps.layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, ps.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        vkCmdDispatch(cb, (im->w + 7) / 8, (im->h + 7) / 8, 1);
        full_barrier(cb);
    }
    return true;
}
// the FXAA draw's input image when SMAA should run on it (else null)
static Image* smaa_target(Combo* pv) {
    if ((!rt::smaa_setting() && !rt::fsr3_setting()) || pv->stages.empty() || pv->stages[0]->stage == 0 || !fxaa_frag(pv)) return nullptr;
    if (!g_vk_storage_ext_formats) {  // SMAA / FSR 3 / DLSS write the B10G11R11 frame as a storage image: the game's FXAA instead
        static bool said = false;
        if (!said) said = true, log("vk: SMAA / FSR 3 / DLSS need shaderStorageImageExtendedFormats: using FXAA");
        return nullptr;
    }
    for (auto* st : pv->stages) {
        if (nvn_stage(st->stage) != 1) continue;
        for (auto& r : st->t.res) {
            if (r.kind != 2) continue;
            int idx = r.handle >= 8 ? ((r.handle - 8) / 2) % 32 : 0;
            uint64_t h = g_st.tex[1][idx];
            Texture* t = nullptr;
            TexView tv;
            pool_texture(g_st.tex_pool, (uint32_t)(h & 0xFFFFF), &t, &tv);
            Image* im = image_of(t);
            const bool ok = im && (im->format == VK_FORMAT_R8G8B8A8_UNORM || im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) && im->view && im->layers == 1 && im->levels == 1 &&
                            im->written && !im->replaced;
            static bool told = false;
            if (!told) {
                told = true;
                log("vk: SMAA input %ux%u vk fmt %d%s", im ? im->w : 0, im ? im->h : 0, im ? (int)im->format : 0, ok ? "" : " (not usable: FXAA stays)");
            }
            return ok ? im : nullptr;
        }
    }
    return nullptr;
}

// 3D resolution (setting scene_res, #11): the scene ran on the lo twins; at the game's FXAA draw its input (the lo scene
// colour) is blitted linearly onto the full-size twin and every screen target switches to full size, so FXAA / SMAA, the
// UI and the present run sharp. Other lo targets read later sync through twin_sync (the same path FSR 3 uses).
static void spatial_upscale(VkCommandBuffer cb, Combo* pv) {
    Image* lo = nullptr;
    for (auto* st : pv->stages) {
        if (nvn_stage(st->stage) != 1 || lo) continue;
        for (auto& r : st->t.res) {
            if (r.kind != 2) continue;
            int idx = r.handle >= 8 ? ((r.handle - 8) / 2) % 32 : 0;
            Texture* t = nullptr;
            TexView tv;
            pool_texture(g_st.tex_pool, (uint32_t)(g_st.tex[1][idx] & 0xFFFFF), &t, &tv);
            lo = image_of(t);
            break;
        }
    }
    g_break = "3d resolution";
    end_rendering(cb);
    if (Image* hi = lo ? vk::upscale_hi(lo) : nullptr; hi && lo->written && hi->format == lo->format) {
        Image* only[1] = {lo};
        ms_flush(cb, "3d resolution", only, 1);
        transition(cb, lo, VK_IMAGE_LAYOUT_GENERAL);
        transition(cb, hi, VK_IMAGE_LAYOUT_GENERAL);
        full_barrier(cb);
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)lo->w, (int32_t)lo->h, 1};
        b.dstOffsets[1] = {(int32_t)hi->w, (int32_t)hi->h, 1};
        vkCmdBlitImage(cb, lo->image, VK_IMAGE_LAYOUT_GENERAL, hi->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        full_barrier(cb);
        vk::mark_written(hi);
    }
    vk::scene_phase(false);
    cbs_forget();
}

// AMD FSR 3 (Anti-aliasing = FSR 3, setting fx_fxaa 3; NOTES s62): temporal anti-aliasing at native size (like DLAA)
// at the game's FXAA draw, in place of FXAA / SMAA, through vk_fsr3.cpp.
// - Camera: the 3D scene's vertex uniform 1 (736 bytes, measured 11-08) starts with the target size in game pixels,
//   near, far; floats 88..103 are the view-projection (row-major, NVN NDC: +Y up, z -1..1), float 61 = cot(fov y / 2),
//   floats 8..10 the camera position. The first such bind per frame whose size matches its colour target is the scene
//   camera (shadow maps bind the same block with their own size).
// - Jitter: draws that bind that camera get a sub-pixel viewport offset (FSR's sequence), the rest (post, UI) none.
// - Motion vectors: tools/spvgen/Motion.cs from the scene depth and last / this frame's view-projection (camera motion;
//   moving characters get no own vectors yet: FSR's own disocclusion / shading-change logic handles them).
struct SceneCam { float vp[16], near_ = 0, far_ = 0, cot_half_fov = 0, pos[3]; Texture* depth = nullptr; uint32_t frame = ~0u; };
static SceneCam g_cam;
static bool g_draw_jitter;            // bind_resources: this draw binds the scene camera
static float g_jx, g_jy;              // this frame's jitter (target pixels), 0 while FSR 3 is off
static uint64_t g_fsr_frames;         // jitter sequence index
static bool g_fsr_live;               // the last FSR 3 frame worked (jitter on)
static float ldf(uint32_t a) { float x; uint32_t w = ld32(a); memcpy(&x, &w, 4); return x; }
// Anti-aliasing = DLSS (fsr3_setting 2) and NGX came up; otherwise the same pass runs FSR 3
static bool use_dlss() { return rt::fsr3_setting() == 2 && vk::dlss_ready(); }
static bool temporal_ready(uint32_t w, uint32_t h) {
    const bool ok = use_dlss() || vk::fsr3_ready(w, h);
    g_temporal_bad = !ok;
    return ok;
}
static void scene_cam_bind(uint32_t a, uint64_t size) {
    static int camlog = getenv("NX_CAM_LOG") ? atoi(getenv("NX_CAM_LOG")) : 0;  // debug: N camera blocks logged
    if (camlog > 0 && size == 736 && g_st.colors[0]) {
        camlog--;
        std::string m;
        for (int i = 0; i < 128; i++) { char b[24]; snprintf(b, sizeof b, "%s%g", i % 16 ? " " : "\n  ", ldf(a + 4 * i)); m += b; }
        log("cam: block %08x target %ux%u frame %u:%s", a, g_st.colors[0]->width, g_st.colors[0]->height, (uint32_t)g_present_no, m.c_str());
#ifdef _WIN32  // NX_CAM_FIND walks guest memory with VirtualQuery: a Windows-only debugging aid
        // debug NX_CAM_FIND=frame: at that frame, guest words equal to this block's P00 with P11 5 floats later
        static uint32_t find_at = getenv("NX_CAM_FIND") ? atoi(getenv("NX_CAM_FIND")) : 0;
        if (find_at && g_present_no >= find_at) {
            find_at = 0;
            const uint32_t p00 = ld32(a + 4 * 56), p11 = ld32(a + 4 * 61);
            MEMORY_BASIC_INFORMATION mi;
            int n = 0;
            for (uint8_t* p = (uint8_t*)GUEST_BASE; p < (uint8_t*)GUEST_BASE + 0x100000000ull && VirtualQuery(p, &mi, sizeof mi);
                 p = (uint8_t*)mi.BaseAddress + mi.RegionSize) {
                if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
                const uint32_t* w = (const uint32_t*)mi.BaseAddress;
                for (size_t i = 0; i + 5 < mi.RegionSize / 4 && n < 40; i++)
                    if (w[i] == p00 && w[i + 5] == p11) {
                        const uint32_t ga = (uint32_t)((uint8_t*)&w[i] - (uint8_t*)GUEST_BASE);
                        if (ga >> 24 == (a >> 24) || ga >> 24 == (a >> 24) + 1 || ga >> 24 == (a >> 24) - 1) continue;  // uniform rings
                        log("cam: P00 copy at %08x%s", ga, ga >= a && ga < a + 736 ? " (this block)" : ""), n++;
                    }
            }
        }
#endif
    }
    if (size != 736 || !g_st.colors[0] || rt::fsr3_setting() == 0) return;
    const float w = ldf(a), h = ldf(a + 4);
    if (w != (float)g_st.colors[0]->width || h != (float)g_st.colors[0]->height) return;
    g_draw_jitter = true;
    if (g_cam.frame == g_present_no) return;
    g_cam.frame = g_present_no;
    for (int i = 0; i < 16; i++) g_cam.vp[i] = ldf(a + 4 * (88 + i));
    for (int i = 0; i < 3; i++) g_cam.pos[i] = ldf(a + 4 * (8 + i));
    g_cam.near_ = ldf(a + 8), g_cam.far_ = ldf(a + 12), g_cam.cot_half_fov = ldf(a + 4 * 61);
    g_cam.depth = g_st.depth;
    g_jx = g_jy = 0;
    static const bool no_jitter = getenv("NX_FSR3_NOJITTER") != nullptr;  // debug
    if (g_fsr_live && !no_jitter) {
        if (use_dlss()) vk::dlss_jitter(g_fsr_frames, vk::upscale_lo_scale() > 0 && vk::scene_phase_lo() ? rt_scale() / vk::upscale_lo_scale() : 1.f, &g_jx, &g_jy);
        else vk::fsr3_jitter(g_fsr_frames, image_of(g_st.colors[0])->w, &g_jx, &g_jy);
    }
}
static bool invert4(const double* m, double* out) {  // row-major 4x4, Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[4 * r + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c && a[r][c] != 0) { const double f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[4 * r + c] = a[r][c + 4];
    return true;
}
static bool g_up_ok;  // upscaling: FSR 3 made its context at the full size (the DLLs are there)
Image* hdr_scene_target(uint32_t w, uint32_t h, VkFormat from);  // vk.cpp: HDR scene snapshot image, or null
uint64_t g_spatial_frame = ~0ull - 1;  // 3D resolution: the last frame with the game's FXAA draw
uint64_t g_fxaa_frame = ~0ull - 1;     // the last frame with the game's FXAA draw (a 3D scene), any setting
}  // namespace nvn
void vk::upscale_frame_end() {
    nvn::vb_frame_end();
    vk::auto_scene_res(nvn::g_fxaa_frame + 1 == nvn::g_present_no);
    vk::upscale_apply_setting();  // FSR 3 / DLSS mode or anti-aliasing changed: lo images rebuilt before the next frame
    // FSR 3 / DLSS: once their context exists and the last frame had the 3D camera; 3D resolution (spatial): once the last
    // frame had the game's FXAA draw (= a 3D scene frame), whose input the linear upscale fills
    vk::scene_phase(vk::upscale_lo_scale() > 0 && (rt::fsr3_setting() ? nvn::g_up_ok && nvn::g_cam.frame + 1 == nvn::g_present_no
                                                                       : nvn::g_spatial_frame + 1 == nvn::g_present_no));
}
namespace nvn {
static bool fsr3_in_place(VkCommandBuffer cb, Image* im) {
    Image* hi = nullptr;  // upscaling: the full-size FXAA input FSR writes (im = its lo twin)
    static const bool started = temporal_ready(im->w, im->h);  // NGX / FidelityFX start here, on the title screen: NGX takes ~3 s, was a freeze on the first in-game frame at native size
    (void)started;
    if (vk::upscale_lo_scale() > 0) {
        if (!vk::scene_phase_lo()) {  // a first 3D frame ran at full size: SMAA for it, FSR from the next frame on
            g_up_ok = temporal_ready(im->w, im->h);
            return false;
        }
        if (!(hi = vk::upscale_hi(im))) return false;
    }
    Image* dimg = image_of(g_cam.depth);
    if (g_cam.frame != g_present_no || !dimg || dimg->w != im->w || dimg->h != im->h || !(dimg->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    Image* osz = hi ? hi : im;  // output size
    if (!temporal_ready(osz->w, osz->h)) { g_fsr_live = false; g_up_ok = false; return false; }
    static SmaaPass mvp{};
    static Image::Own mv{}, dep{}, out{};
    static float prev_vp[16], prev_pos[3];
    static uint32_t prev_frame = ~0u;
    static auto prev_t = std::chrono::steady_clock::now();
    if (!mvp.pipe) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_motion_spv, k_motion_spv};
        VkShaderModule mod;
        VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                             {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, b};
        VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &mvp.dsl));
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &mvp.dsl, 1, &pr};
        VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &mvp.layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
        ci.layout = mvp.layout;
        VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &mvp.pipe));
        log("vk: FSR 3 motion vector pass ready");
    }
    if (mv.w != im->w || mv.h != im->h || out.w != osz->w || out.h != osz->h || out.format != im->format) {
        for (Image::Own* o : {&mv, &dep, &out})
            if (o->image) defer_destroy(o->image, o->view, {}, o->mem, o->mem_off, o->mem_size);
        mv = new_image_2d(VK_FORMAT_R16G16_SFLOAT, im->w, im->h, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        dep = new_image_2d(VK_FORMAT_R32_SFLOAT, im->w, im->h, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        out = new_image_2d(im->format, osz->w, osz->h, 1, VK_IMAGE_USAGE_STORAGE_BIT);
        VkImageMemoryBarrier ib[3];
        Image::Own* o3[3] = {&mv, &dep, &out};
        for (int i = 0; i < 3; i++) {
            ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                     o3[i]->image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            o3[i]->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 3, ib);
        prev_frame = ~0u;
        log("vk: FSR 3 work images %ux%u", im->w, im->h);
    }
    // reset the history at a cut: first frame, a gap, or the camera jumped far in one frame
    const float dx = g_cam.pos[0] - prev_pos[0], dy = g_cam.pos[1] - prev_pos[1], dz = g_cam.pos[2] - prev_pos[2];
    // ... or turned sharply: a cut inside one room moves little. VP row 3 (w) = the view direction; 15 degrees in one frame is no camera move
    const float* a = g_cam.vp + 12; const float* b = prev_vp + 12;
    const float la = a[0] * a[0] + a[1] * a[1] + a[2] * a[2], lb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
    const float dot = la > 0 && lb > 0 ? (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / sqrtf(la * lb) : 1.f;
    const float dist2 = dx * dx + dy * dy + dz * dz;
    static uint32_t prev_w = 0;  // ... or the scene size changed (upscaling mode switched live)
    const bool reset = prev_frame == ~0u || g_cam.frame != prev_frame + 1 || dist2 > 200.f * 200.f || dot < 0.966f || prev_w != im->w;
    prev_w = im->w;
    if (reset) {
        static int logged = 0;
        if (logged++ < 200) log("vk: temporal history reset (frame gap %d, moved %.0f, view dot %.3f)", (int)(g_cam.frame - prev_frame), sqrtf(dist2), dot);
        memcpy(prev_vp, g_cam.vp, sizeof prev_vp);
    }
    double cur[16], inv[16], pm[16];
    for (int i = 0; i < 16; i++) cur[i] = g_cam.vp[i];
    if (!invert4(cur, inv)) return false;
    struct { float m[16]; int32_t w, h; float jx, jy; } pc;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double v = 0;
            for (int k = 0; k < 4; k++) v += (double)prev_vp[4 * r + k] * inv[4 * k + c];
            pm[4 * r + c] = v;
            pc.m[4 * r + c] = (float)v;
        }
    pc.w = (int32_t)im->w, pc.h = (int32_t)im->h, pc.jx = g_jx, pc.jy = g_jy;
    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
    transition(cb, dimg, VK_IMAGE_LAYOUT_GENERAL);
    full_barrier(cb);
    VkDescriptorSet set = alloc_set(mvp.dsl);
    VkDescriptorImageInfo ii[3] = {{sampler_of(Sampler{}), dimg->view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, mv.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dep.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[3];
    for (int j = 0; j < 3; j++)
        wr[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, (uint32_t)j, 0, 1,
                 j == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[j], nullptr, nullptr};
    vkUpdateDescriptorSets(dev, 3, wr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mvp.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, mvp.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, mvp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (im->w + 7) / 8, (im->h + 7) / 8, 1);
    full_barrier(cb);
    // debug NX_MV_CHECK=s: show 4 x |this frame - last frame at (pixel + s x vector)| (bilinear) instead of FSR: with the
    // right vectors s = 1 is darkest (0 = no vectors, 0.9 / 1.1 for comparison; tools/spvgen/Motion.cs MvCheck)
    static const float mv_check = getenv("NX_MV_CHECK") ? (float)atof(getenv("NX_MV_CHECK")) : -1.f;  // vector scale
    if (mv_check >= 0 && !hi && im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        static SmaaPass ck{};
        static Image::Own prevc{};
        if (!ck.pipe) {
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof k_mvcheck_spv, k_mvcheck_spv};
            VkShaderModule mod;
            VKC(vkCreateShaderModule(dev, &smi, nullptr, &mod));
            VkDescriptorSetLayoutBinding b[4] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                                 {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                                 {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
                                                 {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
            VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 4, b};
            VKC(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &ck.dsl));
            VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &ck.dsl, 1, &pr};
            VKC(vkCreatePipelineLayout(dev, &pli, nullptr, &ck.layout));
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main"};
            ci.layout = ck.layout;
            VKC(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &ck.pipe));
            prevc = new_image_2d(im->format, im->w, im->h, 1);
            VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                    prevc.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        }
        VkDescriptorSet cs = alloc_set(ck.dsl);
        const VkSampler smp = sampler_of(Sampler{});
        VkDescriptorImageInfo ci4[4] = {{smp, im->view, VK_IMAGE_LAYOUT_GENERAL}, {smp, prevc.view, VK_IMAGE_LAYOUT_GENERAL},
                                        {smp, mv.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet cw[4];
        for (int j = 0; j < 4; j++)
            cw[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cs, (uint32_t)j, 0, 1,
                     j < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ci4[j], nullptr, nullptr};
        vkUpdateDescriptorSets(dev, 4, cw, 0, nullptr);
        struct { int32_t w, h; float scale, pad; } cpc{(int32_t)im->w, (int32_t)im->h, mv_check, 0};
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ck.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ck.layout, 0, 1, &cs, 0, nullptr);
        vkCmdPushConstants(cb, ck.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cpc, &cpc);
        vkCmdDispatch(cb, (im->w + 7) / 8, (im->h + 7) / 8, 1);
        full_barrier(cb);
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {im->w, im->h, 1};
        vkCmdCopyImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, prevc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        full_barrier(cb);
        vkCmdCopyImage(cb, out.image, VK_IMAGE_LAYOUT_GENERAL, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        full_barrier(cb);
        memcpy(prev_vp, g_cam.vp, sizeof prev_vp);
        memcpy(prev_pos, g_cam.pos, sizeof prev_pos);
        prev_frame = g_cam.frame;
        return true;
    }
    auto now = std::chrono::steady_clock::now();
    const float dt_ms = std::clamp(std::chrono::duration<float, std::milli>(now - prev_t).count(), 1.f, 100.f);
    prev_t = now;
    Image mvi, depi, outi;  // the work images for vk_fsr3.cpp (all GENERAL)
    mvi.image = mv.image, mvi.view = mv.view, mvi.format = mv.format, mvi.w = mv.w, mvi.h = mv.h;
    depi.image = dep.image, depi.view = dep.view, depi.format = dep.format, depi.w = dep.w, depi.h = dep.h;
    outi.image = out.image, outi.view = out.view, outi.format = out.format, outi.w = out.w, outi.h = out.h;
    static const float sharp = getenv("NX_FSR3_SHARPNESS") ? (float)atof(getenv("NX_FSR3_SHARPNESS")) : 0.f;
    vk::Fsr3Frame f{cb, im, &depi, &mvi, &outi, g_jx, g_jy, sharp, dt_ms, g_cam.near_, g_cam.far_,
                    g_cam.cot_half_fov > 0 ? 2.f * atanf(1.f / g_cam.cot_half_fov) : 1.f, reset};
    g_fsr_live = use_dlss() ? vk::dlss_dispatch(f) : vk::fsr3_dispatch(f);
    if (!g_fsr_live) { g_up_ok = false; return false; }
    full_barrier(cb);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {osz->w, osz->h, 1};
    if (hi) { transition(cb, hi, VK_IMAGE_LAYOUT_GENERAL); full_barrier(cb); }
    vkCmdCopyImage(cb, out.image, VK_IMAGE_LAYOUT_GENERAL, osz->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(cb);
    if (hi) {  // the FXAA draw, UI and present run on the full-size images from here
        vk::mark_written(hi);
        vk::scene_phase(false);
    }
    memcpy(prev_vp, g_cam.vp, sizeof prev_vp);
    memcpy(prev_pos, g_cam.pos, sizeof prev_pos);
    prev_frame = g_cam.frame;
    g_fsr_frames++;
    static uint64_t n = 0;
    if (n++ % 3000 == 0 || n < 4)
        log("vk: FSR 3 frames %llu (jitter %.3f %.3f, reset %d, near %g far %g, reprojection row0 %g %g %g %g)", (unsigned long long)n,
            g_jx, g_jy, (int)reset, g_cam.near_, g_cam.far_, pm[0], pm[1], pm[2], pm[3]);
    return true;
}

static uint64_t g_resist_hash;  // bind_resources: the draw samples a resistance atlas drawn with icons (its hash)
static VkDescriptorSet bind_resources(VkCommandBuffer cb, Combo* pv) {
    VkDescriptorSet set = pv->push ? VK_NULL_HANDLE : alloc_set(pv->set_layout);
    g_resist_hash = 0;
    // per-draw lists, kept per thread (s72: their heap allocations were ~4% of the game thread in big areas)
    thread_local std::vector<VkWriteDescriptorSet> writes;
    thread_local std::vector<VkDescriptorBufferInfo> bufs;
    thread_local std::vector<VkDescriptorImageInfo> imgs;
    writes.clear(), bufs.clear(), imgs.clear();
    size_t nres = 0;
    for (auto* st : pv->stages) nres += st->t.res.size();
    g_log_bindings = g_log_draws > 0;
    g_draw_jitter = false;
    g_draw_picture = false;
    g_draw_art_w = g_draw_art_h = 0;
    g_draw_rendered = false;
    bufs.reserve(nres + 1);
    imgs.reserve(nres * 4);
    thread_local std::vector<float> tex_scale[6];  // per Ryujinx stage, in the shader's texture order
    for (auto& v : tex_scale) v.clear();
    if (!pv->stages.empty() && pv->stages[0]->stage != 0) fxaa_rescale(pv, (float)rt_scale());
    for (auto* stp : pv->stages) {
        const StageVk& st = *stp;
        uint32_t nst = nvn_stage(st.stage);
        for (auto& r : st.t.res) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, r.binding, 0, 1};
            if (r.kind == 0 || r.kind == 1) {
                // NVN uniform binding N -> Maxwell constant buffer slot N + 3 (slots 0-2 belong to the driver);
                // measured: fragment shader reads c4 where the game binds uniform 1
                int idx = r.kind == 0 ? r.slot - 3 : r.handle;
                Binding b = r.kind == 1 ? global_binding(nst, r.handle) : idx >= 0 && idx < 18 ? g_st.ubuf[nst][idx] : Binding{};
                if (r.kind == 0 && r.slot == 1) b = {st.consts, st.consts_size};  // c1 = the shader's constant data
                if (r.kind == 0 && nst == 0 && idx == 1 && b.addr) scene_cam_bind((uint32_t)b.addr, b.size);
                GuestBuffer gb = guest_buffer(b.addr, b.size ? b.size : 65536);
                if (!gb.buf) {
                    static std::set<std::pair<VkShaderModule, int>> told;
                    if (told.insert({st.module, r.slot}).second)
                        log("vk: stage %u %s slot %d (nvn index %d) unbound (addr %llx): dummy zeros", st.stage, r.kind ? "ssbo" : "ubo", r.slot, idx, (unsigned long long)b.addr);
                    static VkBuffer dummy = VK_NULL_HANDLE;
                    if (!dummy) {
                        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                        bci.size = 65536;
                        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                        VKC(vkCreateBuffer(dev, &bci, nullptr, &dummy));
                        VkMemoryRequirements req;
                        vkGetBufferMemoryRequirements(dev, dummy, &req);
                        VkDeviceSize off;
                        VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &off);
                        VKC(vkBindBufferMemory(dev, dummy, m, off));
                        // zeroed: shaders may take loop counts from an unbound buffer (garbage could hang the GPU)
                        bool was = g_st.rendering;
                        g_break = "dummy buffer";
                        end_rendering(cb);
                        vkCmdFillBuffer(cb, dummy, 0, VK_WHOLE_SIZE, 0);
                        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                        if (was) begin_rendering(cb);
                    }
                    gb = {dummy, 0};
                    b.size = 65536;
                }
                uint64_t cap = r.kind == 0 ? 65536 : UINT64_MAX;  // uniform ranges stop at 64 KiB (maxUniformBufferRange)
                uint64_t range = std::min<uint64_t>({b.size ? b.size : 65536, cap, gb.size ? gb.size : 65536});
                bufs.push_back({gb.buf, gb.offset, range});
                if (r.kind == 1 && b.addr) vb_gpu_wrote(b.addr, range);  // storage buffers may be written
                w.descriptorType = r.kind == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &bufs.back();
                if (g_log_bindings) {
                    std::string f;
                    char fb[24];
                    for (int q = 0; q < (b.size && b.size <= 512 ? (int)(b.size / 4) : 24) && b.addr; q++) { uint32_t w = ld32((uint32_t)b.addr + 4 * q); float x; memcpy(&x, &w, 4); snprintf(fb, sizeof fb, " %g", x); f += fb; }
                    log("  bind stage %u %s slot %d <- nvn index %d addr %llx size %llu:%s", st.stage, r.kind ? "ssbo" : "ubo", r.slot, idx, (unsigned long long)b.addr, (unsigned long long)b.size, f.c_str());
                    static const int full_slot = getenv("NX_LOG_UBO_FULL") ? atoi(getenv("NX_LOG_UBO_FULL")) : -1;  // debug
                    if (b.addr && r.kind == 0 && r.slot == full_slot && b.size > 512 && b.size <= 65536)
                        for (uint32_t q0 = 0; q0 < b.size / 4; q0 += 64) {
                            std::string g;
                            for (uint32_t q = q0; q < std::min<uint32_t>(q0 + 64, (uint32_t)(b.size / 4)); q++) {
                                uint32_t w = ld32((uint32_t)b.addr + 4 * q); float x; memcpy(&x, &w, 4); snprintf(fb, sizeof fb, " %g", x); g += fb;
                            }
                            log("    full +%#x:%s", q0 * 4, g.c_str());
                        }
                    if (b.addr && b.size >= 4096) {  // bone palettes (48-byte 3x4 matrices): which are non-zero
                        std::string nz;
                        int count = 0;
                        for (uint32_t m = 0; m < b.size / 48; m++) {
                            bool any = false;
                            for (int q = 0; q < 12; q++) any |= ld32((uint32_t)b.addr + 48 * m + 4 * q) != 0;
                            if (any && count++ < 40) { snprintf(fb, sizeof fb, " %u", m); nz += fb; }
                        }
                        log("    %d non-zero 48-byte rows:%s", count, nz.c_str());
                    }
                    if (b.addr && r.slot == 8 && b.size >= 368)
                        log("    c8[22] = %d %d %d %d", (int)ld32((uint32_t)b.addr + 352), (int)ld32((uint32_t)b.addr + 356), (int)ld32((uint32_t)b.addr + 360), (int)ld32((uint32_t)b.addr + 364));
                }
            } else if (r.kind == 2) {
                // bound textures: one 64-bit handle (two words) per NVN binding, starting at word 8 (measured: the
                // movie shader reads handles 8 and 10 where the game binds texture 0 and 1)
                int idx = r.handle >= 8 ? ((r.handle - 8) / 2) % 32 : 0;
                uint64_t h = g_st.tex[nst][idx];
                Texture* t = nullptr;
                Sampler s{};
                TexView tv;
                pool_texture(g_st.tex_pool, (uint32_t)(h & 0xFFFFF), &t, &tv);
                bool have_smp = pool_sampler(g_st.smp_pool, (uint32_t)((h >> 20) & 0xFFF), &s);
                Image* im = image_of(t);
                if (im && unsynced(im)) {  // a target an earlier pass wrote with its barrier deferred (g_unsynced)
                    bool was = g_st.rendering;
                    g_break = "unsynced";
                    end_rendering(cb);
                    if (was) begin_rendering(cb);
                }
                if (im && !g_ms_dirty.empty() && ms_is_dirty(im)) {  // sampled while its MSAA twin is newer
                    if (seq_on()) log("seq:   draw samples dirty %p (%ux%u f%d) -> flush", (void*)im, im->w, im->h, (int)im->format);
                    bool was = g_st.rendering;
                    g_break = "ms dirty";
                    end_rendering(cb);
                    ms_flush(cb, "sampled", &im, 1);
                    if (was) begin_rendering(cb);
                }
                if (im) check_pool_texture(t, im);
                if (im && t && has_texels(t)) {  // CPU-written texels (outside the pass)
                    bool was = g_st.rendering;
                    g_break = "texels";
                    end_rendering(cb);
                    apply_texels(cb, t, im);
                    if (was) begin_rendering(cb);
                }
                if (im && im->twin && im->twin->wgen > im->wgen) {  // upscaling: the other size holds the newer picture
                    bool was = g_st.rendering;
                    g_break = "twin";
                    end_rendering(cb);
                    twin_sync(cb, im);
                    if (was) begin_rendering(cb);
                }
                if (im && im->pack_wait && pack_arrived(im)) im->written = false;  // its UHD pack file is read now
                if (im && !im->written && t) {  // data the game wrote into the pool: upload it (outside the pass)
                    bool was = g_st.rendering;
                    g_break = "pool upload";
                    end_rendering(cb);
                    upload_from_pool(cb, t, im);
                    if (was) begin_rendering(cb);
                }
                g_stats_tex_draws++;
                if (im && im->written) g_stats_tex_written++;
                if (im && im->resist == 1) g_resist_hash = im->resist_hash;
                if (g_log_bindings) log("  bind stage %u tex cbuf %d handle %d <- nvn index %d handle %llx -> tex %08x %ux%u nvn fmt %#x target %u flags %#x swizzle %x%x%x%x%s", st.stage, r.slot, r.handle, idx, (unsigned long long)h, t ? t->guest : 0, t ? t->width : 0, t ? t->height : 0, t ? t->format : 0, t ? t->target : 0, t ? t->flags : 0, t ? t->swizzle[0] : 0, t ? t->swizzle[1] : 0, t ? t->swizzle[2] : 0, t ? t->swizzle[3] : 0, im && im->from_pool ? " (pool)" : im && im->written ? " (rendered)" : "");
                if (g_log_bindings) log("    sampler %u%s: min %u mag %u wrap %u %u %u cmp %u op %u border %08x %08x %08x %08x aniso %08x reduction %u", (uint32_t)((h >> 20) & 0xFFF), have_smp ? "" : " (NOT FOUND)", s.words[1], s.words[2], s.words[3], s.words[4], s.words[5], s.words[9], s.words[10], s.words[11], s.words[12], s.words[13], s.words[14], s.words[15], s.words[16]);
                if (!im) {
                    static Texture dummy_t;
                    static bool made = false;
                    if (!made) { dummy_t.width = dummy_t.height = 4; dummy_t.levels = 1; dummy_t.format = 0x25; dummy_t.target = 1; made = true; }
                    im = image_of(&dummy_t);
                }
                if (im->layout != VK_IMAGE_LAYOUT_GENERAL && g_st.rendering && im == g_pass_depth && g_ds_feedback.insert(im).second)
                    log("vk: depth %ux%u sampled while it is the pass's depth: kept in GENERAL", im->w, im->h);
                if (im->layout != VK_IMAGE_LAYOUT_GENERAL) {  // never-written image: give it a layout outside the pass
                    bool was = g_st.rendering;
                    g_break = "layout";
                    end_rendering(cb);
                    transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
                    if (was) begin_rendering(cb);
                }
                tex_scale[st.stage].push_back(t ? (float)im->scale : 1.0f);
                if (im && im->written && !im->from_pool) g_draw_rendered = true;
                // ultrawide: a fragment texture that is a 16:9 picture (>= 640 x 360, not a widened screen target)
                if (st.stage == 5 && t && t->width >= 640 && t->height >= 360 && (int)(t->width * 9) - (int)(t->height * 16) <= (int)t->height &&
                    (int)(t->height * 16) - (int)(t->width * 9) <= (int)t->height && im->sx == 1)
                    g_draw_picture = true;
                if (st.stage == 5 && t && im->sx == 1 && !(im->written && !im->from_pool) && t->width * t->height > g_draw_art_w * g_draw_art_h)
                    g_draw_art_w = t->width, g_draw_art_h = t->height;
                imgs.push_back({sampler_of(s), t ? sampled_view(t, im, tv) : im->view, VK_IMAGE_LAYOUT_GENERAL});
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.descriptorCount = 1;
                w.pImageInfo = &imgs.back();
            } else {
                continue;
            }
            writes.push_back(w);
        }
    }
    bool compute = !pv->stages.empty() && pv->stages[0]->stage == 0;
    float rs = compute ? 1.0f : (float)rt_scale();
    thread_local std::vector<float> key;
    // ultrawide (s104): shaders that address screen copies by gl_FragCoord (Unity / Mirage fades and ghosts, weapon
    // reveal) expect the game's 1280 wide x; a widened target's x is divided by its factor as well (patched translator)
    key.assign({rs, g_st.vp.width, g_st.vp.height, (float)tex_scale[5].size(), compute ? 1.0f : rt_sx()});
    for (float x : compute ? tex_scale[0] : tex_scale[5]) key.push_back(x > 1 && rs > 1 ? -x : x);
    if (!compute) key.insert(key.end(), tex_scale[1].begin(), tex_scale[1].end());
    bufs.push_back(support_buffer(key));
    VkWriteDescriptorSet sw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    sw.pBufferInfo = &bufs.back();
    writes.push_back(sw);
    if (pv->push) {  // VK_NULL_HANDLE: the callers skip vkCmdBindDescriptorSets
        g_push_desc(cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, pv->layout, 0, (uint32_t)writes.size(), writes.data());
        return VK_NULL_HANDLE;
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    return set;
}

// the last draws replayed, dumped when the GPU is lost (which draw hung it)
struct RecentDraw { uint32_t code[2], op, mode, count, target; };
static RecentDraw g_recent[1024];
static uint32_t g_recent_n;
void dump_recent_draws() {
    for (uint32_t i = g_recent_n > 1024 ? g_recent_n - 1024 : 0; i < g_recent_n; i++) {
        const RecentDraw& d = g_recent[i % 1024];
        log("vk: recent draw %u: vs %08x fs %08x op %u mode %u count %u target %08x", i, d.code[0], d.code[1], d.op, d.mode, d.count, d.target);
    }
}

// drawlog: per-draw pipeline statistics, read after the submit's fence (every submit waits): vertex shader invocations,
// clipper input / output primitives, fragment shader invocations. Tells "vertices land off screen / NaN" from "fragments die".
static VkQueryPool g_stats_pool = VK_NULL_HANDLE;
static std::vector<std::string> g_stats_pending;
void stats_flush_impl() {
    if (g_stats_pending.empty()) return;
    uint32_t n = (uint32_t)g_stats_pending.size();
    std::vector<uint64_t> r(4 * n);
    if (vkGetQueryPoolResults(dev, g_stats_pool, 0, n, r.size() * 8, r.data(), 32, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++)
            log("stats %s: vs %llu, clip in %llu out %llu, fs %llu", g_stats_pending[i].c_str(), (unsigned long long)r[4 * i],
                (unsigned long long)r[4 * i + 1], (unsigned long long)r[4 * i + 2], (unsigned long long)r[4 * i + 3]);
    vkResetQueryPool(dev, g_stats_pool, 0, n);
    g_stats_pending.clear();
}

// UI smoothing (setting ui_smooth, default on; user 11-06: above 60 fps the UI must be smoother, never faster). Widgets
// count whole 60 Hz ticks and the game rebuilds the same sprites every frame, so a 240 Hz screen shows 60 Hz steps. Each
// sprite is one 4-vertex draw (36-byte vertices: pos 3, colour 4, uv 2 floats; matrix in vertex ubo index 1, ortho behind it),
// written fresh into the frame's ring. Between two ticks the corners and colours glide from the previous tick's sprite
// (same texture + uv = same sprite) to the current one, so the UI shows up to one tick (16 ms) late. A sprite
// without a match (new, flipbook step) or that jumped far stays as drawn: a wrong pair can only last one tick.
struct UiSprite { uint64_t key; float x[4], y[4], col[4][4]; };
static std::vector<UiSprite> g_ui_t0, g_ui_t1;  // sprites of the previous tick, of the current one
// ultrawide field HUD corners (s105, user screenshot): the day chain (one ring picture in two 108 x 108 quads at
// -72..144 x -5..103, made to be cut by the screen edge) with the day circle, digit, partner portrait and sparkles sits
// in the top-left; the SCENE banner (920..1290 x 14..78, runs past the edge), radar diamonds and area-name text in the
// top-right, the party panel below it in dungeons. Centred, their edge cut lands inside the picture. While the chain or
// the SCENE banner was drawn in the last 3 frames (the field HUD is up; battle and menus have neither), UI draws whose screen box lies in one of those corners move to the
// real screen edge: 0 = left, 2 = right, 1 = centred (everything else).
static uint32_t g_chain_frame = ~0u, g_pane_frame = ~0u;  // last frames with the chain / SCENE banner, with a screen-size pane
static float g_ui_box[5];  // ultrawide: the last field_hud_anchor draw box in game pixels (x0, x1, model x scale, x offset) + valid (36-byte panes)
static int field_hud_anchor(const Cmd& k, uint32_t stride) {
    // plain draws: first, count; indexed text (op 21): glyph quads, 4 vertices per 6 indices from the base vertex
    const bool idx = k.op == OP_DRAW_INDEXED && stride == 28;
    g_ui_box[4] = 0;
    if (k.op != OP_DRAW && !idx) return 1;
    const uint32_t first = idx ? k.a[5] : k.a[1], n = idx ? k.a[2] / 6 * 4 : k.a[2];
    const uint32_t u = (uint32_t)g_st.ubuf[0][1].addr, v0 = (uint32_t)g_st.vbuf[0].addr + stride * first;
    if (!u || !g_st.vbuf[0].addr || n < 4 || n > 512) return 1;
    // screen position in game pixels: 36-byte panes = model rows 0 / 1 (+ ortho 2/1280 at float 16); 28-byte text =
    // a ready-made matrix (rows 0 / 1 in clip space, x scale 2/1280)
    float m[8];
    for (int i = 0; i < 8; i++) m[i] = ldf(u + 4 * i);
    bool clip;
    if (stride == 36 && ld32(u + 64) == 0x3acccccd) clip = false;
    else if (stride == 28 && ld32(u) == 0x3acccccd) clip = true;
    else return 1;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        const float vx = ldf(v0 + stride * i), vy = ldf(v0 + stride * i + 4);
        float x = m[0] * vx + m[1] * vy + m[3], y = m[4] * vx + m[5] * vy + m[7];
        if (clip) x = (x + 1) * 640, y = (1 - y) * 360;
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (!clip && m[1] == 0 && m[0] > 0) g_ui_box[0] = x0, g_ui_box[1] = x1, g_ui_box[2] = m[0], g_ui_box[3] = m[3], g_ui_box[4] = 1;
    // the chain: 12 vertices, identity model, 216 x 108 starting left of the screen near the top
    if (!clip && n == 12 && m[0] == 1 && m[5] == 1 && m[1] == 0 && m[4] == 0 && x0 < 0 && x0 > -108 &&
        fabsf(x1 - x0 - 216) < 0.5f && fabsf(y1 - y0 - 108) < 0.5f && y0 < 20)
        g_chain_frame = g_present_no;
    // ... and its spin-in when the CALL ring opens (18 vertices, 7 frames, to -64..189 x -158..148; user video 11-24)
    if (!clip && n == 18 && m[0] == 1 && m[5] == 1 && m[1] == 0 && m[4] == 0 && x0 < 20 && y0 < 0 && x1 < 200 && y1 < 160)
        g_chain_frame = g_present_no;
    // the SCENE banner (dungeons show no chain): 12 vertices, identity model, top right, running past the right edge
    if (!clip && n == 12 && m[0] == 1 && m[5] == 1 && m[1] == 0 && m[4] == 0 && x0 >= 900 && x1 > 1280 && y1 <= 95)
        g_chain_frame = g_present_no;
    static const int alog = getenv("NX_UW_ANCHOR_LOG") ? atoi(getenv("NX_UW_ANCHOR_LOG")) : 0;  // debug: UI draw boxes near the top corners (2: all)
    if ((alog && y0 < 480 && (x0 < 400 || x1 > 880)) || alog >= 2)
        log("uw anchor: frame %u n %u m %.3f %.3f %.3f %.3f box %.1f %.1f %.1f %.1f%s", g_present_no, n, m[0], m[1], m[3], m[7], x0, y0, x1, y1, clip ? " text" : "");
    if (g_chain_frame == ~0u || g_present_no - g_chain_frame > 3) return 1;  // 3: gone with the HUD (battle, menus)
    if (g_pane_frame != ~0u && g_present_no - g_pane_frame <= 3) return 1;  // Topic menu / dim over the field: its own layout
    if (x1 <= 200 && y1 <= 160) return 0;  // 200 / 160: the CALL portrait pops to 72..153 x 10..91, the spin-in to 189 x 148
    if (x0 >= 900 && y1 <= 500) return 2;  // SCENE banner, radar, area name, party panel (dungeons; third member to y 488)
    return 1;
}

// ultrawide: host-visible ring of vertex memory for extra UI draws (grid / edge extension); 1 MiB, reused after ~100+
// frames (well past the frames in flight). Returns the write pointer; *buf / *off for vkCmdBindVertexBuffers.
static uint8_t* ui_ring(uint32_t need, VkBuffer* out_buf, VkDeviceSize* out_off) {
    static VkBuffer buf = VK_NULL_HANDLE;
    static uint8_t* map = nullptr;
    static uint32_t used = 0;
    const uint32_t cap = 1 << 20;
    if (!buf) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = cap;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t t = 0;
        while (t < mp.memoryTypeCount && !((req.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want)) t++;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, t};
        VkDeviceMemory m;
        VKC(vkAllocateMemory(dev, &ai, nullptr, &m));  // own allocation: mapped once, no block shared with other maps
        VKC(vkBindBufferMemory(dev, buf, m, 0));
        void* p;
        VKC(vkMapMemory(dev, m, 0, cap, 0, &p));
        map = (uint8_t*)p;
    }
    if (used + need > cap) used = 0;
    *out_buf = buf, *out_off = used;
    uint8_t* p = map + used;
    used += (need + 255) & ~255u;
    return p;
}

// ultrawide: a centred pane (36-byte vertices, axis-aligned model) that runs past one 16:9 edge (RESULTS: Exp / Money /
// Items bars and the FOUND! stripe to x 1460, the MAIN CAST band from x -92; s107) is clipped there (clip space ends at the
// 16:9 edge). Drawn again into that side bar only: vertices shifted by the bar width S toward the middle, viewport moved
// back by S, so the part past the edge shows as it is (no stretch, no seam). stretch (a pane across the whole width: HELP
// bar -10..1290, its pill inside stays put): only the vertices inside the 16:9 area shift, so the pane's end reaches the
// screen edge (its texture stretches in the bar; fine for plain bars).
static void ui_edge_extend(VkCommandBuffer cb, const Cmd& k, const VkViewport& vp, const VkRect2D& sc, float rs, float ox, int side, bool stretch) {
    const uint32_t n = k.a[2], v0 = (uint32_t)g_st.vbuf[0].addr + 36 * k.a[1];
    const float S = ox / rs, d = side * S / g_ui_box[2];  // side -1: left bar, 1: right bar; d in model units
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t* out = ui_ring(36 * n, &buf, &off);
    for (uint32_t i = 0; i < n; i++) {
        for (int j = 0; j < 9; j++) { const uint32_t v = ld32(v0 + 36 * i + 4 * j); memcpy(out + 36 * i + 4 * j, &v, 4); }
        float x;
        memcpy(&x, out + 36 * i, 4);
        const float px = g_ui_box[2] * x + g_ui_box[3];  // game pixels
        if (!stretch || (side > 0 ? px <= 1280 : px >= 0)) x -= d;
        memcpy(out + 36 * i, &x, 4);
    }
    VkViewport v2 = vp;
    v2.x += side * S * rs;
    VkRect2D c2 = sc;
    // the bar: [edge, edge + ox) in target pixels (the draw's own scissor is the 16:9 area: its x range is not used)
    const float edge = side > 0 ? ox + 1280 * rs : 0;
    c2.offset.x = (int32_t)floorf(edge), c2.extent.width = (uint32_t)ceilf(ox);
    vkCmdSetViewport(cb, 0, 1, &v2);
    vkCmdSetScissor(cb, 0, 1, &c2);
    vkCmdBindVertexBuffers(cb, 0, 1, &buf, &off);
    vkCmdDraw(cb, n, 1, 0, 0);
    GuestBuffer gb = guest_buffer(g_st.vbuf[0].addr, g_st.vbuf[0].size);
    if (gb.buf) vkCmdBindVertexBuffers(cb, 0, 1, &gb.buf, &gb.offset);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cbs_forget();  // vertex buffer 0 is not restored when it was the zero buffer
}

// ultrawide SELECT checker (see the call in draw()): vertices for the bar in a host-visible ring of its own memory
static void ui_grid_extend(VkCommandBuffer cb, const Cmd& k, const VkViewport& vp, const VkRect2D& sc, float rs, float ox) {
    const uint32_t ub = (uint32_t)g_st.ubuf[0][1].addr, v0 = (uint32_t)g_st.vbuf[0].addr + 36 * k.a[1];
    // identity model (floats 0, 5 = 1; 1, 3, 4, 7, 12, 13 = 0) + ortho x scale 2 / 1280 (float 16): vertices are game pixels
    if (!ub || !g_st.vbuf[0].addr || g_st.streams.empty() || g_st.streams[0] != 36 || ld32(ub + 64) != 0x3acccccd || ldf(ub) != 1 ||
        ldf(ub + 20) != 1 || ldf(ub + 4) != 0 || ldf(ub + 12) != 0 || ldf(ub + 16) != 0 || ldf(ub + 28) != 0 || ldf(ub + 48) != 0 || ldf(ub + 52) != 0)
        return;
    // tiles: the first one's size; full ones start on its grid, cropped ones (right / bottom end, e.g. a 66 x 60 piece at a
    // half step) are smaller; the grid must start left of x = 0
    const uint32_t nq = k.a[2] / 6;
    float w = 0, h = 0, gx = 0, gy = 0;
    auto on = [](float v, float p) { const float r = fmodf(fabsf(v), p); return r < 0.01f || p - r < 0.01f; };
    for (uint32_t q = 0; q < nq; q++) {
        float a = 1e9f, b = -1e9f, c = 1e9f, d = -1e9f;
        for (uint32_t i = 0; i < 6; i++) {
            const float x = ldf(v0 + 36 * (6 * q + i)), y = ldf(v0 + 36 * (6 * q + i) + 4);
            a = std::min(a, x), b = std::max(b, x), c = std::min(c, y), d = std::max(d, y);
        }
        if (q == 0) w = b - a, h = d - c, gx = a, gy = c;
        const bool full = b - a > w - 0.01f && d - c > h - 0.01f;
        if (w < 16 || h < 16 || b - a > w + 0.01f || d - c > h + 0.01f || (full && !(on(a - gx, w) && on(c - gy, h))) || a < gx - 0.01f) return;
    }
    if (gx >= 0 || gx <= -w || nq < 8) return;  // 8+: a real tile grid (SELECT: 15-22); the field day chain (2 quads) is one picture
    const int reps = (int)ceilf((ox / rs + gx) / w);  // tiles per row left of the first one, up to the target's edge
    if (reps < 0 || reps > 64) return;
    // first-column tiles (start at gx < 0, clipped at the 16:9 edge) again as they are (r = 0) and at gx - r w; shifted
    // right by the bar width T so they sit inside the viewport (clip space ends at the 16:9 edge), viewport moved left by T
    const float T = ox / rs;
    uint32_t nv = 0;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t* out = ui_ring(36 * 6 * nq * (reps + 1), &buf, &off);
    for (uint32_t q = 0; q < nq; q++) {
        bool first = true;  // a first-column tile: no vertex right of gx + w
        for (uint32_t i = 0; i < 6; i++) first &= ldf(v0 + 36 * (6 * q + i)) <= gx + w + 0.01f;
        if (!first) continue;
        for (int r = 0; r <= reps; r++)
            for (uint32_t i = 0; i < 6; i++, nv++) {
                uint8_t* o = out + 36 * nv;
                for (int j = 0; j < 9; j++) { const uint32_t v = ld32(v0 + 36 * (6 * q + i) + 4 * j); memcpy(o + 4 * j, &v, 4); }
                float x;
                memcpy(&x, o, 4);
                x += T - r * w;
                memcpy(o, &x, 4);
            }
    }
    if (!nv) return;
    VkViewport v2 = vp;
    v2.x -= T * rs;
    VkRect2D c2 = sc;
    c2.offset.x = 0, c2.extent.width = (uint32_t)floorf(ox);
    vkCmdSetViewport(cb, 0, 1, &v2);
    vkCmdSetScissor(cb, 0, 1, &c2);
    vkCmdBindVertexBuffers(cb, 0, 1, &buf, &off);
    vkCmdDraw(cb, nv, 1, 0, 0);
    GuestBuffer gb = guest_buffer(g_st.vbuf[0].addr, g_st.vbuf[0].size);
    if (gb.buf) vkCmdBindVertexBuffers(cb, 0, 1, &gb.buf, &gb.offset);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cbs_forget();  // vertex buffer 0 is not restored when it was the zero buffer
}

struct UiMatch { std::vector<uint32_t> idx; };  // unused ones first
static std::unordered_map<uint64_t, UiMatch> g_ui_match;  // g_ui_t0 by key
static std::vector<uint8_t> g_ui_used;
static uint32_t g_ui_dbg[2];
static void ui_sprite_smooth(const Cmd& k) {
    if (k.op != OP_DRAW || k.a[2] != 4 || !g_st.vbuf[0].addr || g_st.streams.empty() || g_st.streams[0] != 36 || g_st.attribs.size() <= 8 ||
        g_st.attribs[0] != 0x04400040 || g_st.attribs[3] != 0x05c00640 || g_st.attribs[8] != 0x02c00e40)
        return;
    static const bool step_log = getenv("NX_UI_STEP_LOG") != nullptr;  // debug: sprites that change between UI ticks = widgets on no UI gate
    if (!rt::ui_smooth() && !step_log) return;
    const uint32_t ub = (uint32_t)g_st.ubuf[0][1].addr, v0 = (uint32_t)g_st.vbuf[0].addr + 36 * k.a[1];
    if (!ub || ld32(ub + 64) != 0x3acccccd) return;  // ortho x scale 2 / 1280: a screen-space sprite
    if (v0 - ub + 0x400000 > 0x800000) return;       // not in the frame's ring: static data must stay as it is
    static uint32_t seen_present = ~0u, seen_tick = ~0u;
    extern uint32_t g_replay_ui_tick;  // nvn_hle.cpp: the UI clock at this frame's submit (the game thread may be a frame ahead)
    extern float g_replay_ui_frac;
    const uint32_t tick = g_replay_ui_tick;
    const float frac = g_replay_ui_frac;
    if (seen_present != g_present_no) {
        seen_present = g_present_no;
        if (seen_tick != tick) {
            seen_tick = tick;
            g_ui_t0.swap(g_ui_t1);
            g_ui_match.clear();
            for (uint32_t i = 0; i < g_ui_t0.size(); i++) g_ui_match[g_ui_t0[i].key].idx.push_back(i);
        }
        static const bool dbg = getenv("NX_UI_SMOOTH_LOG") != nullptr;
        if (dbg && !(g_present_no & 255)) log("ui smooth: %zu sprites, %u matched, %u glided, frac %.2f tick %u", g_ui_t1.size(), g_ui_dbg[0], g_ui_dbg[1], frac, tick);
        g_ui_dbg[0] = g_ui_dbg[1] = 0;
        g_ui_t1.clear();
        g_ui_used.assign(g_ui_t0.size(), 0);
    }
    auto fl = [](uint32_t a) { float x; uint32_t w = ld32(a); memcpy(&x, &w, 4); return x; };
    const float a = fl(ub), b = fl(ub + 4), c = fl(ub + 8), tx = fl(ub + 12), d = fl(ub + 16), e = fl(ub + 20), f = fl(ub + 24), ty = fl(ub + 28);
    UiSprite s;
    s.key = g_st.tex[1][0];
    for (int i : {0, 3}) s.key = s.key * 0x100000001b3ull ^ ld32(v0 + 36 * i + 28), s.key = s.key * 0x100000001b3ull ^ ld32(v0 + 36 * i + 32);
    float z[4];
    for (int i = 0; i < 4; i++) {
        const uint32_t v = v0 + 36 * i;
        const float px = fl(v), py = fl(v + 4);
        z[i] = fl(v + 8);
        s.x[i] = a * px + b * py + c * z[i] + tx, s.y[i] = d * px + e * py + f * z[i] + ty;
        for (int j = 0; j < 4; j++) s.col[i][j] = fl(v + 12 + 4 * j);
    }
    g_ui_t1.push_back(s);
    if (step_log) {
        static std::unordered_map<uint64_t, UiSprite> prev, cur;
        struct Hit { uint32_t n; float x, y; uint32_t u, v; };
        static std::unordered_map<uint64_t, Hit> hits;
        static uint32_t pres = ~0u, ptick = ~0u, ticks = 0;
        if (pres != g_present_no) {
            const bool same_tick = ptick == tick;
            if (same_tick && !prev.empty()) {
                for (auto& [key, n] : cur) {
                    auto p = prev.find(key);
                    float d = p == prev.end() ? 999 : 0;
                    if (p != prev.end()) for (int i = 0; i < 4; i++) {
                        d = std::max({d, fabsf(p->second.x[i] - n.x[i]), fabsf(p->second.y[i] - n.y[i])});
                        for (int j = 0; j < 4; j++) d = std::max(d, fabsf(p->second.col[i][j] - n.col[i][j]) * 100);
                    }
                    if (d > 0.01f) {
                        auto& h = hits[key];
                        h.n++, h.x = n.x[0], h.y = n.y[0], h.u = (uint32_t)(n.key >> 0);
                    }
                }
            }
            prev.swap(cur), cur.clear();
            ptick = tick, pres = g_present_no;
            if (!(g_present_no % 240)) {
                for (auto& [key, h] : hits) if (h.n >= 8) log("ui step: sprite %016llx at %.0f,%.0f changed %u frames between ticks", (unsigned long long)key, h.x, h.y, h.n);
                log("ui step: --- %zu sprites hit, frame sprites %zu", hits.size(), g_ui_t1.size());
                hits.clear();
            }
        }
        cur[s.key] = s;
    }
    if (!rt::ui_smooth() || frac >= 0.999f) return;
    auto m = g_ui_match.find(s.key);
    if (m == g_ui_match.end()) return;
    // Per axis, a sprite moves less than its own extent per tick (USER 11-10: the level-up -> NEW SKILL stripe wipe flickered:
    // equal stripes of one colour sit in several rows and slide in fast, so the nearest equal sprite could be a stripe of
    // another row and the glide pulled stripes up / down, uncovering the screen behind on every other frame).
    auto ext = [](const UiSprite& q, bool y) {
        float lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 4; i++) lo = std::min(lo, y ? q.y[i] : q.x[i]), hi = std::max(hi, y ? q.y[i] : q.x[i]);
        return hi - lo;
    };
    const float sw = ext(s, false), sh = ext(s, true);
    int best = -1;  // nearest: a scrolling strip of equal sprites wraps, list order would slide every sprite back one step
    float best_d = 1e30f;
    for (uint32_t i : m->second.idx) {
        const float dx = fabsf(g_ui_t0[i].x[0] - s.x[0]), dy = fabsf(g_ui_t0[i].y[0] - s.y[0]);
        if (dx > std::max({sw, ext(g_ui_t0[i], false), 4.0f}) || dy > std::max({sh, ext(g_ui_t0[i], true), 4.0f})) continue;
        if (!g_ui_used[i] && dx + dy < best_d) best = (int)i, best_d = dx + dy;
    }
    if (best < 0) return;
    g_ui_used[best] = 1;
    const UiSprite& o = g_ui_t0[best];
    g_ui_dbg[0]++;
    float moved = 0, faded = 0;
    for (int i = 0; i < 4; i++) {
        moved = std::max({moved, fabsf(o.x[i] - s.x[i]), fabsf(o.y[i] - s.y[i])});
        for (int j = 0; j < 4; j++) faded = std::max(faded, fabsf(o.col[i][j] - s.col[i][j]));
    }
    // a sprite moves less than its own size per tick; farther = another sprite of the same look (sparkle stars around the Action
    // portrait come and go at different spots: gliding one to the other slid stars across, USER 11-08 "glitchy stars uncapped")
    float size = 0;
    for (int i = 1; i < 4; i++)
        size = std::max({size, fabsf(s.x[i] - s.x[0]), fabsf(s.y[i] - s.y[0]), fabsf(o.x[i] - o.x[0]), fabsf(o.y[i] - o.y[0])});
    const float det = a * e - b * d;
    if ((moved < 0.01f && faded < 0.002f) || moved > 320 || moved > std::max(size, 4.0f) || fabsf(det) < 1e-6f) return;
    g_ui_dbg[1]++;
    for (int i = 0; i < 4; i++) {
        const uint32_t v = v0 + 36 * i;
        const float X = o.x[i] + (s.x[i] - o.x[i]) * frac - tx - c * z[i], Y = o.y[i] + (s.y[i] - o.y[i]) * frac - ty - f * z[i];
        const float px = (e * X - b * Y) / det, py = (a * Y - d * X) / det;
        uint32_t w;
        memcpy(&w, &px, 4), st32(v, w);
        memcpy(&w, &py, 4), st32(v + 4, w);
        for (int j = 0; j < 4; j++) {
            const float q = o.col[i][j] + (s.col[i][j] - o.col[i][j]) * frac;
            memcpy(&w, &q, 4), st32(v + 12 + 4 * j, w);
        }
    }
}
// Vertex data in video memory (s126, Augs' crosswalk / #11): every memory pool is system RAM imported zero-copy, so
// the GPU read each vertex over PCIe every frame (crosswalk: ~2 M vertices; 3.35 -> 1.80 ms GPU here with the streams
// in video memory, more on x4 / x8 laptop and budget cards). A vertex stream (address, size) that the CPU left alone for
// 2 frames gets a device-local copy (one copy command, a pass break, at most 64 per frame); later frames bind the copy
// while the CPU write ticks (memory.cpp) show no write to its pages since it was taken. A written stream is copied again
// at its next use; one written in 3 checks in a row stays in system RAM for 10 s (per-frame rings, skinning). Ranges the
// GPU itself writes (storage buffers, buffer copies, inline updates) are never copied. Budget min(768 MB, VRAM / 5);
// copies unused for ~20 s are freed. PORTABILITY: needs the write ticks (Windows); else and with NX_VB_VRAM=0 as before.
struct VbEntry {
    uint64_t addr = 0, size = 0;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize moff = 0, msize = 0;
    uint32_t gen = 0;                       // write tick when the content was taken (or the watch started)
    uint32_t gaddr = 0, span_lo = 0, span_len = 0;  // guest address, its pool (for the write ticks)
    uint64_t pgen = ~0ull;                  // pool_gen() those were looked up at
    uint64_t checked = ~0ull, used = 0, dyn_until = 0;
    uint8_t quiet = 0, writes = 0;          // frames without a write / checks in a row with one
    bool gpu = false, stale = false;
};
static std::unordered_map<uint64_t, VbEntry> g_vbc;
static std::map<uint64_t, uint64_t> g_vb_gpu;  // GPU-written ranges [start, end)
static uint64_t g_vb_bytes = 0, g_vb_epoch = 1;  // epoch: bumped when entries are erased (stream caches drop pointers)
struct VbDead { uint64_t serial; VkBuffer buf; VkDeviceMemory mem; VkDeviceSize off, size; };
static std::vector<VbDead> g_vb_dead;
static VkBuffer g_vb_use[16];  // this draw: the device-local copy per stream, or null (system RAM)
static uint64_t g_vb_use_addr[16], g_vb_use_size[16];  // the stream each g_vb_use was decided for
static bool vb_on() {
    static const bool on = (!getenv("NX_VB_VRAM") || atoi(getenv("NX_VB_VRAM")) != 0) && mem_write_ticks_ok();
    static bool told = false;
    if (!told) told = true, log("vk: vertex data in video memory %s", on ? "on" : "off (NX_VB_VRAM=0 or no write ticks on this OS)");
    return on;
}
static uint64_t vb_key(uint64_t addr, uint64_t size) { return addr * 0x9E3779B97F4A7C15ull ^ size; }
static bool vb_overlaps_gpu(uint64_t a, uint64_t n) {
    auto it = g_vb_gpu.upper_bound(a + n - 1);  // first range starting after our end
    return it != g_vb_gpu.begin() && std::prev(it)->second > a;
}
static void vb_release(VbEntry& e) {
    if (!e.buf) return;
    g_vb_dead.push_back({vk::gpu_recording_serial() + 1, e.buf, e.mem, e.moff, e.msize});
    g_vb_bytes -= e.msize;
    e.buf = VK_NULL_HANDLE, e.mem = VK_NULL_HANDLE;
}
// the GPU writes [addr, addr+size): streams there stay in system RAM (where the GPU writes)
static void vb_gpu_wrote(uint64_t addr, uint64_t size) {
    if (!addr || !size || !vb_on()) return;
    if (vb_overlaps_gpu(addr, size)) {  // usually the same range again: covered?
        auto it = std::prev(g_vb_gpu.upper_bound(addr));
        if (it->first <= addr && it->second >= addr + size) return;
    }
    uint64_t lo = addr, hi = addr + size;
    for (auto it = g_vb_gpu.lower_bound(lo); it != g_vb_gpu.end() && it->first <= hi;) hi = std::max(hi, it->second), it = g_vb_gpu.erase(it);
    if (auto it = g_vb_gpu.lower_bound(lo); it != g_vb_gpu.begin() && std::prev(it)->second >= lo) lo = std::prev(it)->first, hi = std::max(hi, std::prev(it)->second), g_vb_gpu.erase(std::prev(it));
    g_vb_gpu[lo] = hi;
    for (auto& [k, e] : g_vbc)
        if (!e.gpu && e.addr < hi && e.addr + e.size > lo) e.gpu = true, vb_release(e);
}
static void vb_copy(VkCommandBuffer cb, VbEntry& e, const GuestBuffer& gb) {
    if (!e.buf) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = e.size;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev, &bci, nullptr, &e.buf) != VK_SUCCESS) { e.buf = VK_NULL_HANDLE; return; }
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, e.buf, &req);
        e.mem = vk::alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &e.moff);
        e.msize = req.size;
        VKC(vkBindBufferMemory(dev, e.buf, e.mem, e.moff));
        g_vb_bytes += e.msize;
    }
    g_break = "vertex copy";
    end_rendering(cb);
    VkBufferCopy c{gb.offset, 0, e.size};
    vkCmdCopyBuffer(cb, gb.buf, e.buf, 1, &c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    e.stale = false;
}
// before the draw's pass begins: per stream, decide system RAM or the copy (copying first when needed)
static uint64_t g_vbs_us = 0, g_vbs_new = 0, g_vbs_checks = 0, g_vbs_pages = 0, g_vbs_harv = 0, g_vbs_harv_us = 0, g_vbs_look = 0;  // debug NX_VB_STATS
static void vb_prepare(VkCommandBuffer cb) {
    static const bool vbs = getenv("NX_VB_STATS") != nullptr;
    const auto vbs_t0 = vbs ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    struct VbsEnd { bool on; std::chrono::steady_clock::time_point t; ~VbsEnd() { if (on) g_vbs_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count(); } } vbs_end{vbs, vbs_t0};
    static const uint64_t cap = getenv("NX_VB_CAP_MB") ? strtoull(getenv("NX_VB_CAP_MB"), nullptr, 10) << 20 : std::min<uint64_t>(768ull << 20, vk::vram_bytes() / 5);
    static uint64_t copies_frame = ~0ull;
    static uint32_t copies = 0;
    if (copies_frame != g_present_no) copies_frame = g_present_no, copies = 0;
    thread_local uint64_t c_addr[16], c_size[16], c_epoch[16];
    thread_local VbEntry* c_e[16];
    for (uint32_t s = 0; s < 16; s++) {
        g_vb_use[s] = VK_NULL_HANDLE;
        const uint64_t addr = g_st.vbuf[s].addr, size = g_st.vbuf[s].size;
        g_vb_use_addr[s] = addr, g_vb_use_size[s] = size;
        if (!addr || size < 1024 || size > (64u << 20)) continue;
        VbEntry* e;
        if (c_e[s] && c_addr[s] == addr && c_size[s] == size && c_epoch[s] == g_vb_epoch) e = c_e[s];
        else {
            const uint64_t key = vb_key(addr, size);
            g_vbs_look++;
            auto it = g_vbc.find(key);
            if (it == g_vbc.end()) {
                if (g_vbc.size() > 200000) continue;
                it = g_vbc.emplace(key, VbEntry{}).first, g_vbs_new++;
                it->second.addr = addr, it->second.size = size, it->second.gpu = vb_overlaps_gpu(addr, size);
            }
            if (it->second.addr != addr || it->second.size != size) continue;  // hash collision: system RAM
            e = &it->second;
            c_e[s] = e, c_addr[s] = addr, c_size[s] = size, c_epoch[s] = g_vb_epoch;
        }
        e->used = g_present_no;
        if (e->gpu) continue;
        if (e->checked != g_present_no) {  // once per frame: did the CPU write it?
            e->checked = g_present_no;
            if (e->pgen != pool_gen()) {  // pools are made again on loads: look the guest range up again
                uint32_t off;
                nvn::Pool* p = nvn::pool_of(addr, &off);
                e->pgen = pool_gen(), e->span_len = 0;
                if (p) e->gaddr = p->addr + off, e->span_lo = p->addr, e->span_len = p->size;
            }
            if (!e->span_len) continue;
            g_vbs_checks++, g_vbs_pages += size >> 12;
            const uint32_t now = mem_write_tick(e->gaddr, (uint32_t)size, g_present_no);  // its 2 MB chunks, once per frame
            const bool written = !e->gen || mem_written_tick(e->gaddr, (uint32_t)size) >= e->gen;
            if (written) {
                e->quiet = 0, e->gen = now, e->stale = e->buf != VK_NULL_HANDLE;
                if (++e->writes >= 3) e->dyn_until = g_present_no + 600, vb_release(*e);
            } else e->quiet = (uint8_t)std::min(e->quiet + 1, 200), e->writes = 0;
        }
        if (g_present_no < e->dyn_until) continue;
        if (e->buf && !e->stale) { g_vb_use[s] = e->buf; continue; }
        if (e->quiet < 2 && !e->stale) continue;  // new or just written: system RAM this frame
        if (copies >= 64 || (!e->buf && g_vb_bytes + e->size > cap)) continue;
        GuestBuffer gb = guest_buffer(addr);
        if (!gb.buf || gb.size < size) continue;
        vb_copy(cb, *e, gb);
        if (e->buf) g_vb_use[s] = e->buf, copies++;
    }
}
// frame end (vk::upscale_frame_end): free copies the GPU is done with; every ~5 s drop streams unused for ~20 s
static void vb_frame_end() {
    for (size_t i = 0; i < g_vb_dead.size();)
        if (vk::gpu_wait(g_vb_dead[i].serial, 0)) {
            vkDestroyBuffer(dev, g_vb_dead[i].buf, nullptr);
            vk::free_mem(g_vb_dead[i].mem, g_vb_dead[i].off, g_vb_dead[i].size);
            g_vb_dead[i] = g_vb_dead.back(), g_vb_dead.pop_back();
        } else i++;
    if (getenv("NX_VB_STATS") && g_present_no % 600 == 0)
        log("vb stats per frame: harvest calls %.3f ms, prepare %.3f ms, lookups %.0f, new %.0f, checks %.0f, pages %.0f, harvests %.0f, entries %zu", g_vbs_harv_us / 600e3, g_vbs_us / 600e3,
            g_vbs_look / 600.0, g_vbs_new / 600.0, g_vbs_checks / 600.0, g_vbs_pages / 600.0, g_vbs_harv / 600.0, g_vbc.size()),
            g_vbs_harv_us = g_vbs_us = g_vbs_look = g_vbs_new = g_vbs_checks = g_vbs_pages = g_vbs_harv = 0;
    if (g_present_no % 300) return;
    size_t freed = 0;
    for (auto it = g_vbc.begin(); it != g_vbc.end();)
        if (g_present_no - it->second.used > (it->second.buf ? 1200u : 120u)) vb_release(it->second), it = g_vbc.erase(it), freed++;
        else ++it;
    if (freed) g_vb_epoch++;
    static uint64_t next_log = 0;
    if (g_present_no >= next_log && !g_vbc.empty()) {
        next_log = g_present_no + 3600;
        size_t n = 0;
        for (auto& [k, e] : g_vbc) n += e.buf != VK_NULL_HANDLE;
        log("vk: vertex data in video memory: %zu of %zu streams, %.0f MB", n, g_vbc.size(), g_vb_bytes / 1048576.0);
    }
}
static void draw(VkCommandBuffer cb, const Cmd& k) {
    g_seq_draws++;
    if (g_st.ds[0] & 7) g_seq_dsuse++;
    g_st.draws++;
    Program* progs[6] = {g_st.programs[0], g_st.programs[1], g_st.programs[2], g_st.programs[3], g_st.programs[4], nullptr};
    g_recent[g_recent_n++ % 1024] = {{progs[0] ? (uint32_t)progs[0]->stages[0].code : 0, progs[1] ? (uint32_t)progs[1]->stages[0].code : 0},
                                   k.op, k.a[0], k.a[1] ? k.a[1] : k.a[2], g_st.colors[0] ? g_st.colors[0]->guest : 0};
    if (vk::g_checkpoint) vk::g_checkpoint(cb, (void*)(uintptr_t)(g_recent_n - 1));
    Combo* pv = combo_of(progs);  // a bound compute program is not part of draws
    if (!pv || pv->stages.empty()) { g_skip[0]++; return; }
    static uint64_t skip_lo = 0, skip_hi = 0;  // debug: NX_SKIP_CODE=lo-hi (hex) drops draws with shader code there
    static bool skip_init = [] {
        if (const char* e = getenv("NX_SKIP_CODE")) { skip_lo = strtoull(e, nullptr, 16); if (const char* d = strchr(e, '-')) skip_hi = strtoull(d + 1, nullptr, 16); }
        return true;
    }();
    (void)skip_init;
    if (skip_hi)
        for (int i = 0; i < 5; i++)
            for (int j = 0; progs[i] && j < 6; j++)
                if (progs[i]->stages[j].code >= skip_lo && progs[i]->stages[j].code < skip_hi) { g_skip[5]++; return; }
    // debug: NX_SKIP_SHADOW=1 drops shadow-map casters (depth-only draws into a power-of-two depth target), so every
    // shadow lookup sees "lit"
    static const bool skip_shadow_env = getenv("NX_SKIP_SHADOW") && atoi(getenv("NX_SKIP_SHADOW")) != 0;
    if ((skip_shadow_env || rt::g_skip_shadow == 1 || (rt::g_skip_shadow == 2 && (g_present_no & 1))) && !g_st.colors[0] && g_st.depth) {
        const uint32_t w = g_st.depth->width, h = g_st.depth->height;
        if (w >= 256 && h >= 256 && !(w & (w - 1)) && !(h & (h - 1))) { g_skip[5]++; return; }
    }
    // Shadow detail Low (setting shadow_detail = 1, weak PCs): town / field frames draw two shadow maps, the near one first
    // (Shibuya / Daitama 1024x2048: tree, bins, characters) and a smaller far one (1024x1024: 187 of the crosswalk's 1445
    // draws, no visible difference near the player, shots shs1 / sh0 s119). Casters into a smaller depth-only power-of-two
    // target after a larger one in the same frame are skipped; the game still clears that map, so it reads as lit. A frame
    // with one map, or the smaller one first, keeps everything.
    if (rt::g_shadow_detail == 1 && !g_st.colors[0] && g_st.depth) {
        const uint32_t w = g_st.depth->width, h = g_st.depth->height;
        static uint32_t frame = ~0u, first_area = 0;
        if (w >= 256 && h >= 256 && !(w & (w - 1)) && !(h & (h - 1))) {
            if (frame != g_present_no) frame = g_present_no, first_area = w * h;
            else if (w * h < first_area) { g_skip[5]++; return; }
        }
    }
    // Character rim (setting fx_char_rim): the game's outline shell around characters is the mesh again with front faces
    // culled, alpha-blended in its vertex colour (light grey / lavender, e.g. 9d9fb0 on Itsuki's jacket), so it shows as
    // a pale line on dark floors (user 11-03, save 2 Observatory door). Off skips those draws (cull front + src alpha /
    // one minus src alpha colour blend: only the shells use it, drawlog 11-03).
    if (!fx_enabled(FX_CHAR_RIM) && (g_st.polygon & 3) == 1 && g_st.blend[0][4] == 5 && g_st.blend[0][5] == 6 &&
        g_st.blend[0][6] == 1 && g_st.blend[0][7] == 2) { g_skip[5]++; return; }
    const bool is_fx = fxaa_frag(pv) != 0;
    if (is_fx) g_fxaa_frame = g_present_no;
    const bool hdr_fx = vk::g_hdr_mode && is_fx;  // HDR: snapshot this FXAA draw's output (below)
    if (!rt::fsr3_setting() && vk::upscale_lo_scale() > 0 && is_fx) {  // 3D resolution: upscale, then full size
        g_spatial_frame = g_present_no;
        if (vk::scene_phase_lo()) spatial_upscale(cb, pv);
    }
    g_smaa_ran = false;
    if (Image* aa = smaa_target(pv)) {
        g_break = "smaa";
        end_rendering(cb);
        ms_flush(cb, "smaa");
        ms_stale(aa);
        g_smaa_ran = (rt::fsr3_setting() && fsr3_in_place(cb, aa)) || smaa_in_place(cb, aa);  // FSR 3 unavailable: SMAA
        cbs_forget();  // FSR 3 / DLSS record into cb with their own state
    }
    if (vb_on()) vb_prepare(cb);
    begin_rendering(cb);
    if (!g_st.rendering) { g_skip[1]++; return; }
    g_pass_writes++;
    static const bool pt_draws = getenv("NX_GPU_PASSES") && atoi(getenv("NX_GPU_PASSES")) >= 2;  // debug: per shader pair
    if (pt_draws && g_pt_pool) {
        char b[64];
        snprintf(b, sizeof b, "draw vs %08x fs %08x", progs[0] ? (uint32_t)progs[0]->stages[0].code : 0, progs[1] ? (uint32_t)progs[1]->stages[1].code : 0);
        pt_mark(cb, b);
    }
    uint32_t mode = k.a[0];
    VkPipeline p = pipeline(pv, mode);
    if (!p) { g_skip[2]++; return; }
    VkDescriptorSet set = bind_resources(cb, pv);
    if (g_log_bindings) {
        g_log_draws--;
        std::string a;
        char b[64];
        for (size_t i = 0; i < g_st.attribs.size(); i++) if (g_st.attribs[i] & 0x40) { snprintf(b, sizeof b, " [%zu]=%08x", i, g_st.attribs[i]); a += b; }
        log("  state: stencil ref %u alpha ref %g color %08x channel %08x ds %08x %08x polygon %08x blend0 %02x%02x%02x%02x%02x%02x%02x%02x ncolors %u depth %p", g_st.stencil_ref, g_st.alpha_ref, g_st.color, g_st.channel, g_st.ds[0], g_st.ds[1], g_st.polygon, g_st.blend[0][0], g_st.blend[0][1], g_st.blend[0][2], g_st.blend[0][3], g_st.blend[0][4], g_st.blend[0][5], g_st.blend[0][6], g_st.blend[0][7], g_st.ncolors, (void*)g_st.depth);
        log("  viewport %g %g %g %g scissor %d %d %u %u target0 %08x (vk fmt %d)", g_st.vp.x, g_st.vp.y, g_st.vp.width, g_st.vp.height, g_st.scissor.offset.x, g_st.scissor.offset.y, g_st.scissor.extent.width, g_st.scissor.extent.height, g_st.colors[0] ? g_st.colors[0]->guest : 0, g_st.colors[0] ? (int)image_of(g_st.colors[0])->format : 0);
        for (uint32_t i = 1; i < g_st.ncolors && i < 8; i++)
            if (g_st.colors[i]) log("    target%u %08x (vk fmt %d)", i, g_st.colors[i]->guest, (int)image_of(g_st.colors[i])->format);
        std::string strides;
        for (size_t i = 0; i < g_st.streams.size() / 2 && i < 4; i++) { snprintf(b, sizeof b, " %u", g_st.streams[2 * i]); strides += b; }
        log("  draw op %d mode %u args %u %u %u; attribs%s; strides%s; vbuf0 %llx vbuf1 %llx; vs %08x fs %08x", k.op, k.a[0], k.a[1], k.a[2], k.a[3], a.c_str(),
            strides.c_str(), (unsigned long long)g_st.vbuf[0].addr, (unsigned long long)g_st.vbuf[1].addr,
            progs[0] ? (uint32_t)progs[0]->stages[0].code : 0, progs[1] ? (uint32_t)progs[1]->stages[1].code : 0);
        {
            std::string sl;
            for (auto* st : pv->stages) { snprintf(b, sizeof b, " %u:%08x", st->stage, st->src); sl += b; }
            log("  stages (ryujinx stage:code)%s", sl.c_str());
        }
        // each stage's code once, for offline translation: analysis/code_<src>_<ryujinx stage>_<consts off>_<consts size>.bin
        static std::set<uint32_t> dumped;
        for (auto* st : pv->stages)
            if (st->src && dumped.insert(st->src).second) {
                snprintf(b, sizeof b, "/analysis/code_%08x_%u_%x_%x.bin", st->src, st->stage, st->src_consts, st->consts_size);
                if (FILE* f = fopen((g_data_dir + b).c_str(), "wb")) { fwrite(GPTR(st->src), 1, 0x20000, f); fclose(f); }
            }
        if (g_st.vbuf[0].addr) {
            std::string v;
            for (int i = 0; i < 16; i++) { float f; uint32_t w = ld32((uint32_t)g_st.vbuf[0].addr + 4 * i); memcpy(&f, &w, 4); snprintf(b, sizeof b, " %g", f); v += b; }
            log("  vbuf0 floats:%s", v.c_str());
            std::string h;
            for (int i = 0; i < 12; i++) { snprintf(b, sizeof b, " %08x", ld32((uint32_t)g_st.vbuf[0].addr + 4 * i)); h += b; }
            log("  vbuf0 words:%s", h.c_str());
            if (g_st.vbuf[1].addr) {
                std::string h1;
                for (int i = 0; i < 25; i++) { snprintf(b, sizeof b, " %08x", ld32((uint32_t)g_st.vbuf[1].addr + 4 * i)); h1 += b; }
                log("  vbuf1 words:%s", h1.c_str());
            }
            if ((k.op == OP_DRAW_INDEXED || k.op == OP_DRAW_INDEXED_INSTANCED) && k.a[1] == 1 && k.a[3]) {  // 16-bit indices
                uint32_t lo = 0xFFFF, hi = 0;
                std::string ix;
                for (uint32_t i = 0; i < k.a[2]; i++) {
                    uint32_t x = ld16(k.a[3] + 2 * i);
                    lo = std::min(lo, x), hi = std::max(hi, x);
                    if (i < 12) { snprintf(b, sizeof b, " %u", x); ix += b; }
                }
                log("  indices %u..%u (base vertex %d):%s", lo, hi, (int32_t)k.a[5], ix.c_str());
            }
            // multi-bone draws: uniforms 4/8/9 (vertex stage) + c1 + 8 vertices -> analysis/skin_<vbuf0>.bin, for checking
            // the skinning on the CPU (tools: scratch). Layout: c4 0x200, c8 0x1c0, c9 0x3000, vbuf0 160 bytes.
            if (g_st.attribs.size() > 7 && ((g_st.attribs[7] >> 21) & 0x3FF) == 0x27) {
                snprintf(b, sizeof b, "/analysis/skin_%08x.bin", (uint32_t)g_st.vbuf[0].addr);
                if (FILE* f = fopen((g_data_dir + b).c_str(), "wb")) {
                    auto put = [&](uint64_t a, uint32_t n) { std::vector<uint8_t> z(n); if (a) memcpy(z.data(), GPTR((uint32_t)a), n); fwrite(z.data(), 1, n, f); };
                    put(g_st.ubuf[0][1].addr, 0x200); put(g_st.ubuf[0][5].addr, 0x1c0); put(g_st.ubuf[0][6].addr, 0x3000);
                    put(g_st.vbuf[0].addr, 160);
                    fclose(f);
                }
            }
        }
    }
    ui_sprite_smooth(k);
    // Resistance icons: the battle target panel draws each badge as its own 4-vertex sprite (pos 3, colour 4, uv 2 floats)
    // tinted dark grey (0.263) for the white letters, which turns the colour icons near black. Badge sprites with a grey
    // tint get white (alpha kept) in the guest vertex data, which the game writes again every frame.
    if (g_resist_hash && k.op == OP_DRAW && k.a[2] == 4 && g_st.vbuf[0].addr && !g_st.streams.empty() && g_st.streams[0] == 36 &&
        g_st.attribs.size() > 8 && g_st.attribs[3] == 0x05c00640 && g_st.attribs[8] == 0x02c00e40) {
        const uint32_t v0 = (uint32_t)g_st.vbuf[0].addr + 36 * k.a[1];
        auto fl = [](uint32_t a) { float x; uint32_t w = ld32(a); memcpy(&x, &w, 4); return x; };
        float u = 0, v = 0;
        for (int i = 0; i < 4; i++) u += fl(v0 + 36 * i + 28) / 4, v += fl(v0 + 36 * i + 32) / 4;
        const float r = fl(v0 + 12);
        if (r < 0.99f && r == fl(v0 + 16) && r == fl(v0 + 20) && rt::resist_icon_cell(g_resist_hash, u, v))
            for (int i = 0; i < 4; i++)
                for (int c = 0; c < 3; c++) st32(v0 + 36 * i + 12 + 4 * c, 0x3f800000);
    }
    const bool fresh = cbs_fresh(cb);
    if (!fresh || g_cbs.pipe != p) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pv->layout, 0, 1, &set, 0, nullptr);
    // NVN (like D3D) maps NDC +Y to the top of the target; Vulkan maps -Y there. A negative-height viewport flips Y
    // and with it the winding, so the game's front-face / cull settings keep their meaning.
    VkViewport vp = g_st.vp;
    const float rs = rt_scale();  // the game's pixels -> target pixels
    // ultrawide: widened targets stretch the game's x over their width (scene, post, the FXAA pass that brings the scene
    // into the window texture); the UI (every other draw into a window texture) keeps the game's 16:9 in the middle
    // The game draws scene and UI into the same target and copies it to the window: the UI phase starts after the FXAA
    // draw (tone-mapped scene in) and lasts to the present; a frame without that draw (menus, title) is all UI once no
    // FXAA draw came for 3 frames. Centred: the UI's layout draws (triangles, 36-byte vertices) into the main target (the
    // FXAA output); full-screen passes after FXAA (4-vertex strips, 24-byte vertices) and draws into other widened
    // targets (the minimap's, composited later by a centred layout quad) stretch like the scene.
    const float sx = rt_sx();
    if (vk::wide_factor() > 1 && g_st.colors[0] && sx == 1) {  // ultrawide debug: big targets drawn into that stay 16:9-sized
        static std::unordered_set<uint32_t> seen;
        Texture* t = g_st.colors[0];
        if (t->width >= 320 && t->height >= 180 && seen.size() < 64 && seen.insert(t->guest).second)
            log("uw: draw into unwidened %08x %ux%u fmt %d flags %x target %d", t->guest, t->width, t->height, (int)t->format, t->flags, (int)t->target);
    }
    bool ui = false;
    bool screen_art = false;  // ultrawide: a centred picture larger than the screen (no edge extension: it would copy art into the bars)
    int corner = 1;  // ultrawide UI placement: 0 left edge, 1 centred, 2 right edge (field_hud_anchor)
    if (vk::wide_factor() > 1) {
        static uint32_t phase_frame = ~0u, last_fxaa = 0;
        if (phase_frame != g_present_no) phase_frame = g_present_no, g_ui_phase = g_present_no - last_fxaa > 3;
        const bool fx = fxaa_frag(pv) != 0;
        // UI vertex formats (s103 log): layout panes 36 bytes (triangle lists, or 4-vertex strips for sprites), text glyphs
        // 28 bytes; full-screen passes are 24-byte strips (-1..1). A 36-byte 4-vertex pane as big as the screen (fades,
        // flashes) stretches over the width when a 3D scene is under it.
        const uint32_t stride = g_st.streams.empty() ? 0 : g_st.streams[0];
        // 12 / 8-byte vertices after FXAA: 2D effects (battle start banner: a picture cut into triangles, and its shatter;
        // a transition near the first field load); none during battle (s107 log). 16:9 art: centred, not stretched
        // (depth test off: at 4K / without FXAA the whole frame counts as UI phase and the 3D scene is 12-byte too; s107b)
        const bool depth_on = g_st.depth && (g_st.ds[0] & 1);
        bool layout = stride == 36 || stride == 28 || stride == 20 || ((stride == 12 || stride == 8) && !depth_on);
        // ... and its SURPRISE ATTACK / FIRST STRIKE text: 16-byte instanced sprites (op 22), within ~6 s of the battle opening
        // (btl_open_chance). Only then: the same kind of draw is field / battle 3D effects at other times (s107 log)
        if (stride == 16 && k.op == OP_DRAW_INSTANCED) {
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            layout = !depth_on && ::rt::g_btl_open_ms && now - ::rt::g_btl_open_ms < 6000;
        }
        // a 4-vertex strip, or a 6-vertex triangle pair (Topic / Bloom Palace dim pane -10..1290 x -10..730, s104)
        const uint32_t nv = k.a[0] == 5 && k.a[2] == 4 ? 4 : k.a[0] == 4 && k.a[2] == 6 && stride != 20 ? 6 : 0;
        if (layout && nv && g_st.vbuf[0].addr) {
            const uint32_t vb = (uint32_t)g_st.vbuf[0].addr;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (uint32_t i = 0; i < nv; i++) {
                const float x = ldf(vb + i * stride), y = ldf(vb + i * stride + 4);
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
            // a screen-size pane: artwork / movie frames (a 16:9 picture texture: title, camp background) stay 16:9 in the
            // middle; fades, flashes and screen copies (a widened target) stretch
            // (pixels, or -1..1 clip space: the white loading pane / fade, 28-byte vs 00979000, user video s104)
            // (exactly -1..1: minimap pieces use small local coordinates such as -17..17)
            auto unit = [](float lo, float hi) { return lo <= -0.99f && lo >= -1.01f && hi >= 0.99f && hi <= 1.01f; };
            if ((x1 - x0 >= 1270 && y1 - y0 >= 710) || (unit(x0, x1) && unit(y0, y1))) {
                // artwork of another shape drawn undistorted over more than the screen (issue #10: Artists > Profile, a
                // 2048 x 1024 portrait sheet at -384..1664 x -152..872) is a picture too: texture shape = quad shape. Wide
                // sheets only (>= 3:2): a 2048 x 2048 quad growing over the screen when a menu opens keeps stretching
                const float qw = x1 - x0, qh = y1 - y0;
                const bool art = g_draw_art_w >= 512 && g_draw_art_h >= 256 && g_draw_art_w * 2 >= g_draw_art_h * 3 && qw >= 1270 &&
                                 fabsf(g_draw_art_w * qh - g_draw_art_h * qw) <= 0.03f * g_draw_art_h * qw;
                static const bool artlog = getenv("NX_UW_ART_LOG") != nullptr;  // debug: each screen-size quad's texture
                if (artlog) log("uw: screen quad %g..%g x %g..%g tex %ux%u picture %d art %d", x0, x1, y0, y1, g_draw_art_w, g_draw_art_h, (int)g_draw_picture, (int)art);
                layout = g_draw_picture || art, screen_art = art, g_pane_frame = g_present_no;  // a menu dim / fade: field HUD corners off (field_hud_anchor)
            }
        } else if (stride == 20) {
            layout = false;  // 20-byte vertices: only the screen-size movie quads above
        }
        const bool dtest = g_st.depth && (g_st.ds[0] & 1);  // debug log only
        ui = sx > 1 && g_ui_phase && !fx && layout && g_st.colors[0] && g_st.colors[0]->guest == g_ui_target;
        if (ui && (stride == 36 || stride == 28)) corner = field_hud_anchor(k, stride);
        if (fx) last_fxaa = g_present_no, g_ui_phase = true;  // from the next draw on
        static int uwlog = getenv("NX_UW_LOG") ? atoi(getenv("NX_UW_LOG")) : 0;  // debug: UI-phase draws into the main target
        static const bool uwfx = getenv("NX_UW_LOG_FX") != nullptr;  // debug: only UI-phase draws with 12 / 8-byte vertices (effects)
        if (uwlog > 0 && g_st.colors[0] && (!uwfx || (g_ui_phase && (stride == 12 || stride == 8 || stride == 16))) && g_present_no > (uint32_t)atoi(getenv("NX_UW_FRAME") ? getenv("NX_UW_FRAME") : "1500")) {
            uwlog--;
            std::string v;
            const uint32_t vb = (uint32_t)g_st.vbuf[0].addr, stride = g_st.streams.empty() ? 0 : g_st.streams[0];
            static const uint32_t allv = getenv("NX_UW_ALLV") ? 12 : 4;  // debug: up to 12 vertices + the model matrix
            for (uint32_t i = 0; vb && i < std::min<uint32_t>(allv, std::max<uint32_t>(4, k.a[2])); i++) { char b[64]; snprintf(b, sizeof b, " (%g %g %g)", ldf(vb + i * stride), ldf(vb + i * stride + 4), ldf(vb + i * stride + 8)); v += b; }
            if (allv > 4 && g_st.ubuf[0][1].addr) { char b[160]; const uint32_t u = (uint32_t)g_st.ubuf[0][1].addr; snprintf(b, sizeof b, " m [%g %g %g %g | %g %g %g %g] p0 %g", ldf(u), ldf(u + 4), ldf(u + 8), ldf(u + 12), ldf(u + 16), ldf(u + 20), ldf(u + 24), ldf(u + 28), ldf(u + 64)); v += b; }
            log("uw: frame %u dt %d ui %d tgt %08x main %08x op %d mode %u count %u stride %u vs %08x fx %d layout %d centred %d vb %08x:%s", g_present_no, (int)dtest, (int)g_ui_phase, g_st.colors[0]->guest, g_ui_target, k.op, k.a[0], k.a[2], stride,
                progs[0] ? (uint32_t)progs[0]->stages[0].code : 0, (int)fx, (int)layout, (int)ui, vb, v.c_str());
        }
    }
    const float rsx = ui ? rs : rs * sx, ox = ui ? g_st.colors[0]->width * (sx - 1) / 2 * rs : 0, oxd = ox * corner;
    vp.x = vp.x * rsx + oxd, vp.y *= rs, vp.width *= rsx, vp.height *= rs;
    vp.y += vp.height;
    vp.height = -vp.height;
    if (g_draw_jitter) vp.x += g_jx, vp.y += g_jy;  // FSR 3 sub-pixel jitter (scene camera draws only)
    if (!fresh || memcmp(&g_cbs.vp, &vp, sizeof vp)) vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = g_st.scissor;
    sc.offset.x = (int32_t)floorf(std::max(0, sc.offset.x) * rsx + oxd);
    sc.offset.y = (int32_t)floorf(std::max(0, sc.offset.y) * rs);
    sc.extent.width = (uint32_t)ceilf(std::min<uint32_t>(sc.extent.width, 16384) * rsx);
    sc.extent.height = (uint32_t)ceilf(std::min<uint32_t>(sc.extent.height, 16384) * rs);
    if (!fresh || memcmp(&g_cbs.sc, &sc, sizeof sc)) vkCmdSetScissor(cb, 0, 1, &sc);
    static const float magenta[4] = {1, 0, 1, 1};
    static const bool dbg_magenta = getenv("NX_DEBUG_BLEND") || getenv("NX_DEBUG_SKIN");  // getenv per draw cost ~15% CPU
    const float* bc = dbg_magenta ? magenta : g_st.blend_color;
    if (!fresh || memcmp(g_cbs.blend, bc, sizeof g_cbs.blend)) vkCmdSetBlendConstants(cb, bc);
    if (!fresh || g_cbs.stencil != g_st.stencil_ref) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_st.stencil_ref);
    static VkBuffer zero_vb = VK_NULL_HANDLE;
    if (!zero_vb) {  // stands in for unbound streams: reading an unbound binding can lose the device
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = 1 << 20;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VKC(vkCreateBuffer(dev, &bci, nullptr, &zero_vb));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, zero_vb, &req);
        VkDeviceSize off;
        VkDeviceMemory m = alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &off);
        VKC(vkBindBufferMemory(dev, zero_vb, m, off));
    }
    VkBuffer vb[16];
    VkDeviceSize vo[16];
    uint32_t lo = 16, hi = 0;  // the changed streams: one bind for [lo, hi)
    // the 16 stream lookups (guest_buffer -> pool_of) per draw were ~7% of the replay thread: streams whose address is
    // unchanged since the last draw on this thread reuse the answer until any pool is created or finalized
    thread_local uint64_t vb_addr[16], vb_size[16], vb_gen = 0, vb_frame = ~0ull;
    thread_local VkBuffer vb_buf[16];
    thread_local VkDeviceSize vb_off[16];
    if (const uint64_t pg = pool_gen(); pg != vb_gen) vb_gen = pg, memset(vb_buf, 0, sizeof vb_buf);
    // Linux: guest_buffer imports are released when unused for a while (vk::imports_trim, NX_IMPORT_KEEP); a cached
    // answer skips guest_buffer, which is what marks an import used. Look each stream up again once per frame so an
    // import in use is never released under a cached handle, and when a stream's size changes (windowed imports).
    if (g_present_no != vb_frame) vb_frame = g_present_no, memset(vb_buf, 0, sizeof vb_buf);
    for (uint32_t s = 0; s < 16; s++) {
        if (!vb_buf[s] || vb_addr[s] != g_st.vbuf[s].addr || vb_size[s] != g_st.vbuf[s].size) {
            GuestBuffer gb = g_st.vbuf[s].addr ? guest_buffer(g_st.vbuf[s].addr, g_st.vbuf[s].size) : GuestBuffer{VK_NULL_HANDLE, 0};
            vb_addr[s] = g_st.vbuf[s].addr, vb_size[s] = g_st.vbuf[s].size, vb_buf[s] = gb.buf ? gb.buf : zero_vb, vb_off[s] = gb.buf ? gb.offset : 0;
        }
        if (g_vb_use[s] && g_vb_use_addr[s] == g_st.vbuf[s].addr && g_vb_use_size[s] == g_st.vbuf[s].size) vb[s] = g_vb_use[s], vo[s] = 0;  // the stream's copy in video memory (vb_prepare)
        else vb[s] = vb_buf[s], vo[s] = vb_off[s];
        if (!fresh || vb[s] != g_cbs.vb[s] || vo[s] != g_cbs.vo[s]) lo = std::min(lo, s), hi = s + 1;
    }
    if (lo < hi) vkCmdBindVertexBuffers(cb, lo, hi - lo, vb + lo, vo + lo);
    g_cbs.valid = true, g_cbs.pipe = p, g_cbs.vp = vp, g_cbs.sc = sc, g_cbs.stencil = g_st.stencil_ref;
    memcpy(g_cbs.blend, bc, sizeof g_cbs.blend), memcpy(g_cbs.vb, vb, sizeof vb), memcpy(g_cbs.vo, vo, sizeof vo);
    bool stats = g_log_bindings && g_vk_pipeline_stats && g_stats_pending.size() < 4096;
    if (stats && !g_stats_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qi.queryCount = 4096;
        qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                                VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        VKC(vkCreateQueryPool(dev, &qi, nullptr, &g_stats_pool));
        vkResetQueryPool(dev, g_stats_pool, 0, 4096);
    }
    if (stats) {
        char b[96];
        snprintf(b, sizeof b, "vbuf0 %llx vs %08x count %u", (unsigned long long)g_st.vbuf[0].addr, progs[0] ? (uint32_t)progs[0]->stages[0].code : 0, k.a[2]);
        vkCmdBeginQuery(cb, g_stats_pool, (uint32_t)g_stats_pending.size(), 0);
        g_stats_pending.push_back(b);
    }
    struct EndQ { VkCommandBuffer cb; bool on; ~EndQ() { if (on) vkCmdEndQuery(cb, g_stats_pool, (uint32_t)g_stats_pending.size() - 1); } } endq{cb, stats};
    switch (k.op) {
        case OP_DRAW:  // mode, first, count
            if (mode == 7) {
                uint32_t n;
                quad_indices(cb, k.a[1], k.a[2], &n);
                vkCmdDrawIndexed(cb, n, 1, 0, (int32_t)k.a[1], 0);
            } else {
                vkCmdDraw(cb, k.a[2], 1, k.a[1], 0);
            }
            break;
        case OP_DRAW_INSTANCED:  // mode, first, count, baseInstance, instanceCount
            vkCmdDraw(cb, k.a[2], k.a[4], k.a[1], k.a[3]);
            break;
        case OP_DRAW_INDEXED:            // mode, type, count, addr64, base vertex (BaseVertex variant)
        case OP_DRAW_INDEXED_INSTANCED: {  // mode, type, count, addr64, base vertex, base instance, instance count
            // indices read: count x 1 / 2 / 4 bytes (type 0 / 1 / 2)
            GuestBuffer gb = guest_buffer((uint64_t)k.a[4] << 32 | k.a[3], (uint64_t)k.a[2] << std::min<uint32_t>(k.a[1], 2));
            if (!gb.buf) {
                if (g_skip[4]++ < 5) log("vk: index buffer %08x%08x (draw args %08x %08x %08x %08x %08x %08x) not in a pool", k.a[4], k.a[3], k.a[0], k.a[1], k.a[2], k.a[3], k.a[4], k.a[5]);
                return;
            }
            VkIndexType it = k.a[1] == 0 ? VK_INDEX_TYPE_UINT8_EXT : k.a[1] == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            if (it == VK_INDEX_TYPE_UINT8_EXT) { g_skip[3]++; return; }  // TODO: 8-bit indices need VK_EXT_index_type_uint8
            vkCmdBindIndexBuffer(cb, gb.buf, gb.offset, it);
            if (k.op == OP_DRAW_INDEXED_INSTANCED)
                vkCmdDrawIndexed(cb, k.a[2], k.a[7], 0, (int32_t)k.a[5], k.a[6]);
            else
                vkCmdDrawIndexed(cb, k.a[2], 1, 0, (int32_t)k.a[5], 0);
            break;
        }
        default:
            g_skip[5]++;
            return;
    }
    // ultrawide: a centred screen-space grid of equal tiles that starts left of the 16:9 area (dialogue SELECT banner's
    // scrolling checker: 132 x 120 tiles from x < 0, rows cropped on the right by the ring, s105) continues into the left
    // bar: each row's first tile repeated leftwards by the tile width (seamless), drawn once into the bar only.
    if (ui && corner == 1 && ox > 0 && k.op == OP_DRAW && mode == 4 && k.a[2] >= 12 && k.a[2] % 6 == 0 && k.a[2] <= 600 && sc.extent.width)
        ui_grid_extend(cb, k, vp, sc, rs, ox);
    if (ui && corner == 1 && ox > 0 && k.op == OP_DRAW && (mode == 4 || mode == 5) && k.a[2] >= 4 && k.a[2] <= 12 && g_ui_box[4] && sc.extent.width &&
        !g_st.streams.empty() && g_st.streams[0] == 36 && !g_draw_rendered && !screen_art) {  // not the minimap map pieces (they sample its mask)
        const bool both = g_ui_box[0] < -0.5f && g_ui_box[1] > 1280.5f;  // across the whole width: HELP bar, RESULTS rule
        if (g_ui_box[0] < 1279.5f && g_ui_box[1] > 1280.5f) ui_edge_extend(cb, k, vp, sc, rs, ox, 1, both);
        if (g_ui_box[0] < -0.5f && g_ui_box[1] > 0.5f) ui_edge_extend(cb, k, vp, sc, rs, ox, -1, both);
    }
    g_st.drawn++;
    if (g_st.ncolors && g_st.colors[0]) g_last_rt = g_st.colors[0];
    // HDR (s121): the game's FXAA output = the finished 3D scene before its gamma pass and the UI: copied for hdr_pass,
    // which expands only the pixels the UI did not change
    if (hdr_fx && g_st.colors[0])
        if (Image* im = image_of(g_st.colors[0]); im && im->layers == 1)
            if (Image* sn = hdr_scene_target(im->w, im->h, im->format)) {
                g_break = "hdr scene";
                end_rendering(cb);
                transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
                transition(cb, sn, VK_IMAGE_LAYOUT_GENERAL);
                full_barrier(cb);
                VkImageBlit b{};
                b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.srcOffsets[1] = b.dstOffsets[1] = {(int32_t)im->w, (int32_t)im->h, 1};
                vkCmdBlitImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, sn->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
                full_barrier(cb);
            }
    for (uint32_t i = 0; i < g_st.ncolors; i++) if (g_st.colors[i]) g_draws_into[g_st.colors[i]]++;
}

// A list of commands by pointer: what vk.cpp flatten builds for lists that call others (no ~80-byte Cmd copies, s119)
struct CmdRefs {
    const Cmd* const* p;
    size_t n;
    size_t size() const { return n; }
    const Cmd& operator[](size_t i) const { return *p[i]; }
};
template <class L> static void replay_list(VkCommandBuffer cb, const L& cmds) {
    static int seen[OP_UNKNOWN + 1] = {};
    if (g_ms_dirty.empty()) ms_prune();
    if (g_log_draws > 0) log("  replay %zu cmds", cmds.size());
    for (size_t ci = 0; ci < cmds.size(); ci++) {
        const Cmd& k = cmds[ci];
        if (g_log_draws > 0 && (k.op == OP_CHANNEL_MASK || k.op == OP_DEPTH_STENCIL || k.op == OP_CALL || k.op == OP_RENDER_TARGETS || k.op == OP_PROGRAM || k.op == OP_COLOR || k.op == OP_BLEND))
            log("  op %d %08x%s", k.op, k.blob.size() >= 4 ? *(const uint32_t*)k.blob.data() : k.a[0], k.blob.size() >= 4 ? " (state)" : "");
        if (k.op <= OP_UNKNOWN) g_ops_since_present[k.op]++;
        if (k.op <= OP_UNKNOWN && seen[k.op]++ < 2)
            LOGV("cmd op %d: %08x %08x %08x %08x %08x %08x %08x %08x blob %zu", k.op, k.a[0], k.a[1], k.a[2], k.a[3], k.a[4],
                 k.a[5], k.a[6], k.a[7], k.blob.size());
        switch (k.op) {
            case OP_RENDER_TARGETS: {
                // The same targets set again (the game does it between draw groups: 42 of 49 pass breaks in the office,
                // s120) keep the open pass: every break cost ~0.9 ms on a 1280x720 target on a GeForce 920MX (#11),
                // scaling with the target's area. NX_PASS_MERGE=0 ends the pass as before.
                static const bool merge = !getenv("NX_PASS_MERGE") || atoi(getenv("NX_PASS_MERGE")) != 0;
                bool same = merge && g_st.rendering && std::min<uint32_t>(8, k.a[0]) == g_st.ncolors && get<Texture>(k.a[9]) == g_st.depth;
                for (uint32_t i = 0; same && i < g_st.ncolors; i++) same = get<Texture>(k.a[1 + i]) == g_st.colors[i];
                if (same) {
                    if (seq_on()) log("seq: targets again, pass kept");
                    if (vk::g_hdr_mode && g_st.colors[0]) vk::hdr_note_target(g_st.colors[0]->guest);
                    break;
                }
                g_break = "targets";
                end_rendering(cb, true);
                if (seq_on()) log("seq: targets c0 %p depth %p", (void*)image_of(k.a[0] ? get<Texture>(k.a[1]) : nullptr), (void*)image_of(get<Texture>(k.a[9])));
                g_st.ncolors = std::min<uint32_t>(8, k.a[0]);
                for (uint32_t i = 0; i < 8; i++) g_st.colors[i] = i < g_st.ncolors ? get<Texture>(k.a[1 + i]) : nullptr;
                g_st.depth = get<Texture>(k.a[9]);
                if (vk::g_hdr_mode && g_st.colors[0]) vk::hdr_note_target(g_st.colors[0]->guest);  // drawn into after the copy
                g_clear_ahead = 0;  // MSAA: which targets this pass clears before drawing
                for (size_t q = ci + 1; q < cmds.size(); q++) {
                    const Cmd& c = cmds[q];
                    if (c.op == OP_CLEAR_COLOR) g_clear_ahead |= 1u << std::min<uint32_t>(c.a[0], 7);
                    else if (c.op == OP_CLEAR_DS && c.a[1]) g_clear_ahead |= 1u << 8;
                    else if (c.op == OP_DRAW || c.op == OP_DRAW_INDEXED || c.op == OP_DRAW_INSTANCED || c.op == OP_DRAW_INDEXED_INSTANCED ||
                             c.op == OP_RENDER_TARGETS || c.op == OP_DISPATCH || c.op == OP_COPY_TEX_TEX || c.op == OP_CLEAR_TEX)
                        break;
                }
                break;
            }
            case OP_VIEWPORT:
                if (k.a[0] != 0xFFFFFFFF) {
                    // NVN's origin is lower-left by default; the game asked for upper-left (SetWindowOriginMode 1)
                    g_st.vp = {(float)(int32_t)k.a[0], (float)(int32_t)k.a[1], (float)(int32_t)k.a[2], (float)(int32_t)k.a[3], g_st.vp.minDepth, g_st.vp.maxDepth};
                }
                break;
            case OP_DEPTH_RANGE:  // near, far (floats)
                memcpy(&g_st.vp.minDepth, &k.a[0], 4);
                memcpy(&g_st.vp.maxDepth, &k.a[1], 4);
                break;
            case OP_SCISSOR:
                if (k.a[0] != 0xFFFFFFFF) g_st.scissor = {{(int32_t)k.a[0], (int32_t)k.a[1]}, {k.a[2], k.a[3]}};
                break;
            case OP_CLEAR_COLOR: {
                begin_rendering(cb);
                if (!g_st.rendering || k.a[0] >= g_st.ncolors || !g_st.colors[k.a[0]]) break;
                g_pass_writes++;
                Image* im = image_of(g_st.colors[k.a[0]]);
                VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, k.a[0]};
                memcpy(&ca.clearValue.color, k.blob.data(), 16);
                static const bool dbg_clear = getenv("NX_DEBUG_CLEAR") != nullptr;
                if (dbg_clear) ca.clearValue.color = {{1.0f, 0.0f, 1.0f, 1.0f}};
                if (seq_on()) log("seq:   clear colour %u %ux%u", k.a[0], im->w, im->h);
                g_stats_clears++;
                VkClearRect r{{{0, 0}, {im->w, im->h}}, 0, 1};
                vkCmdClearAttachments(cb, 1, &ca, 1, &r);
                break;
            }
            case OP_CLEAR_DS: {
                begin_rendering(cb);
                Image* im = image_of(g_st.depth);
                if (!g_st.rendering || !im) break;
                g_pass_writes++;
                VkClearAttachment ca{};
                if (k.a[1]) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT & im->aspect;
                if (k.a[3]) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT & im->aspect;
                if (!ca.aspectMask) break;
                if (seq_on()) log("seq:   clear ds depth %u stencil %u (mask %#x) %ux%u", k.a[1], k.a[3], (unsigned)ca.aspectMask, im->w, im->h);
                float d;
                memcpy(&d, &k.a[0], 4);
                ca.clearValue.depthStencil = {d, k.a[2]};
                VkClearRect r{{{0, 0}, {im->w, im->h}}, 0, 1};
                vkCmdClearAttachments(cb, 1, &ca, 1, &r);
                break;
            }
            case OP_COPY_BUF_BUF: {  // (r1 padding), src addr64, dst addr64, size (32-bit size_t), flags
                g_break = "copy buf";
                end_rendering(cb);
                GuestBuffer src = guest_buffer((uint64_t)k.a[2] << 32 | k.a[1], k.a[5]), dst = guest_buffer((uint64_t)k.a[4] << 32 | k.a[3], k.a[5]);
                if (!src.buf || !dst.buf || !k.a[5]) {
                    static int bad = 0;
                    if (bad++ < 5) log("vk: skipped buffer copy %08x%08x -> %08x%08x size %u", k.a[2], k.a[1], k.a[4], k.a[3], k.a[5]);
                    break;
                }
                VkBufferCopy r{src.offset, dst.offset, k.a[5]};
                vkCmdCopyBuffer(cb, src.buf, dst.buf, 1, &r);
                vb_gpu_wrote((uint64_t)k.a[4] << 32 | k.a[3], k.a[5]);
                g_stats_copies++;
                full_barrier(cb);
                break;
            }
            case OP_COPY_BUF_TEX: {
                g_break = "copy buf tex";
                end_rendering(cb);
                Texture* t = get<Texture>(k.a[2]);
                Image* im = image_of(t);
                ms_flush_one(cb, "copy buf-tex", im);
                GuestBuffer gb{VK_NULL_HANDLE, 0};
                if (im && k.blob.size() >= 24) {  // bytes read: the region at up to 16 bytes per texel
                    int32_t r6[6];
                    memcpy(r6, k.blob.data(), 24);
                    gb = guest_buffer((uint64_t)k.a[1] << 32 | k.a[0], (uint64_t)std::max(1, r6[3]) * std::max(1, r6[4]) * std::max(1, r6[5]) * 16);
                }
                if (!im || !gb.buf || k.blob.size() < 24) break;
                int32_t rg[6];
                memcpy(rg, k.blob.data(), 24);
                if (im->scale != 1) {  // shortcut: CPU data into a scaled render target is not resampled; logged until seen
                    static int n = 0;
                    if (n++ < 5) log("vk: buffer -> scaled texture %08x copy skipped (%dx%d)", t->guest, rg[3], rg[4]);
                    break;
                }
                transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
                VkBufferImageCopy c{};
                c.bufferOffset = gb.offset;
                VkImageAspectFlags asp = (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspect;
                c.imageSubresource = {asp, 0, 0, 1};
                c.imageOffset = {rg[0], rg[1], rg[2]};
                c.imageExtent = {(uint32_t)std::max(1, rg[3]), (uint32_t)std::max(1, rg[4]), (uint32_t)std::max(1, rg[5])};
                vkCmdCopyBufferToImage(cb, gb.buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                g_stats_copies++;
                vk::mark_written(im);
                ms_stale(im);
                full_barrier(cb);
                break;
            }
            case OP_COPY_TEX_TEX: {  // src texture, dst texture, regions {x, y, z, w, h, d} x2, flags
                g_break = "copy tex";
                end_rendering(cb);
                if (g_window_tex.count((uint32_t)k.a[2])) g_ui_target = (uint32_t)k.a[0];  // ultrawide: the frame's scene + UI target
                Image* src = image_of(get<Texture>(k.a[0]));
                Image* dst = image_of(get<Texture>(k.a[2]));
                ms_flush_one(cb, "copy tex-tex", src, dst);
                if (!src || !dst || k.blob.size() < 48) break;
                check_pool_texture(get<Texture>(k.a[0]), src);
                twin_sync(cb, src);
                if (!src->written) upload_from_pool(cb, get<Texture>(k.a[0]), src);
                int32_t s[6], d[6];
                memcpy(s, k.blob.data(), 24);
                memcpy(d, k.blob.data() + 24, 24);
                for (int q : {0, 1, 3, 4}) s[q] = (int32_t)lround(s[q] * src->scale * (q % 3 ? 1 : src->sx)), d[q] = (int32_t)lround(d[q] * dst->scale * (q % 3 ? 1 : dst->sx));  // image pixels (x: ultrawide factor too)
                // HDR: the game copies its whole float frame (scene + UI, highlights above 1) into the 8-bit window
                // texture; the HDR present reads the float one (vk.cpp hdr_pass)
                if (vk::g_hdr_mode && g_window_tex.count((uint32_t)k.a[2])) {
                    const bool fl = src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || src->format == VK_FORMAT_R16G16B16A16_SFLOAT;
                    const bool whole = !s[0] && !s[1] && (uint32_t)s[3] == src->w && (uint32_t)s[4] == src->h && !(k.a[5] & 6);
                    vk::hdr_note_copy((uint32_t)k.a[2], fl && whole && src->layers == 1 ? src : nullptr);
                }
                transition(cb, src, VK_IMAGE_LAYOUT_GENERAL);
                transition(cb, dst, VK_IMAGE_LAYOUT_GENERAL);
                VkImageBlit b{};
                VkImageAspectFlags sa = (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
                VkImageAspectFlags da = (dst->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : dst->aspect;
                b.srcSubresource = {sa, 0, 0, 1};
                b.dstSubresource = {da, 0, 0, 1};
                b.srcOffsets[0] = {s[0], s[1], 0};
                b.srcOffsets[1] = {s[0] + s[3], s[1] + s[4], 1};
                b.dstOffsets[0] = {d[0], d[1], 0};
                b.dstOffsets[1] = {d[0] + d[3], d[1] + d[4], 1};
                // NVNcopyFlags (assumed): 1 linear filter, 2 mirror x, 4 mirror y
                if (k.a[5] & 2) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);
                if (k.a[5] & 4) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);
                bool same = src->format == dst->format && s[3] == d[3] && s[4] == d[4];
                if (same) {
                    VkImageCopy c{};
                    c.srcSubresource = b.srcSubresource;
                    c.dstSubresource = b.dstSubresource;
                    c.srcOffset = {s[0], s[1], 0};
                    c.dstOffset = {d[0], d[1], 0};
                    c.extent = {(uint32_t)s[3], (uint32_t)s[4], 1};
                    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                } else {
                    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                                   (k.a[5] & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
                }
                vk::mark_written(dst);
                ms_stale(dst);
                full_barrier(cb);
                static int logged = 0;
                if (logged++ < 3) log("vk: copy texture %ux%u fmt %d -> %ux%u fmt %d flags %#x", s[3], s[4], src->format, d[3], d[4], dst->format, k.a[5]);
                break;
            }
            case OP_CLEAR_TEX: {  // texture, channel mask; blob: region {x, y, z, w, h, d}, colour (raw 16 bytes)
                g_break = "clear tex";
                end_rendering(cb);
                Texture* t = get<Texture>(k.a[0]);
                Image* im = image_of(t);
                // the clear covers the whole image and its twin (below): a dirty twin needs no resolve first
                if (im && ms_is_dirty(im) && !(g_ms[im].img && im->layers == 1)) ms_flush_one(cb, "clear tex", im);
                if (!im || k.blob.size() < 40) break;
                int32_t rg[6];
                memcpy(rg, k.blob.data(), 24);
                // ponytail: clears whole layers of level 0; partial regions / channel masks are logged until one shows up
                if (rg[0] || rg[1] || (uint32_t)lround(rg[3] * im->scale * im->sx) < im->w || (uint32_t)lround(rg[4] * im->scale) < im->h || (k.a[1] & 0xF) != 0xF) {
                    static int n = 0;
                    if (n++ < 5) log("vk: ClearTexture region %d,%d %dx%d of %ux%u, mask %#x: clearing all of it", rg[0], rg[1], rg[3], rg[4], im->w, im->h, k.a[1]);
                }
                bool array = im->layers > 1;
                VkImageSubresourceRange r{im->aspect, 0, 1, array ? (uint32_t)rg[2] : 0, array ? (uint32_t)std::max(1, rg[5]) : 1};
                transition(cb, im, VK_IMAGE_LAYOUT_GENERAL);
                if (im->aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
                    VkClearColorValue v;
                    memcpy(&v, k.blob.data() + 24, 16);
                    vkCmdClearColorImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
                    if (auto it = g_ms.find(im); it != g_ms.end() && it->second.img && !array)
                        vkCmdClearColorImage(cb, it->second.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
                    else ms_stale(im);
                } else {
                    VkClearDepthStencilValue v{};
                    memcpy(&v.depth, k.blob.data() + 24, 4);
                    vkCmdClearDepthStencilImage(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
                    if (auto it = g_ms.find(im); it != g_ms.end() && it->second.img && !array)
                        vkCmdClearDepthStencilImage(cb, it->second.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
                    else ms_stale(im);
                }
                vk::mark_written(im);
                full_barrier(cb);
                break;
            }
            case OP_PROGRAM: {  // (program, stage bits: vertex 1, fragment 2, geometry 4, tess c 8, tess e 16, compute 32)
                Program* p = get<Program>(k.a[0]);
                for (int i = 0; i < 6; i++)
                    if (k.a[1] & (1u << i)) g_st.programs[i] = p;
                break;
            }
            case OP_BLEND: {
                if (k.blob.size() < 8) break;
                uint32_t target = k.blob[0] & 7;
                memcpy(g_st.blend[target], k.blob.data(), 8);
                break;
            }
            case OP_COLOR: if (k.blob.size() >= 4) memcpy(&g_st.color, k.blob.data(), 4); break;
            case OP_CHANNEL_MASK: if (k.blob.size() >= 4) memcpy(&g_st.channel, k.blob.data(), 4); break;
            case OP_DEPTH_STENCIL: if (k.blob.size() >= 8) memcpy(g_st.ds, k.blob.data(), 8); break;
            case OP_POLYGON: if (k.blob.size() >= 4) memcpy(&g_st.polygon, k.blob.data(), 4); break;
            case OP_VTX_ATTRIB:
                g_st.attribs.assign((const uint32_t*)k.blob.data(), (const uint32_t*)(k.blob.data() + k.blob.size()));
                break;
            case OP_VTX_STREAM:
                g_st.streams.assign((const uint32_t*)k.blob.data(), (const uint32_t*)(k.blob.data() + k.blob.size()));
                break;
            case OP_VTX_BUFFER:
                if (k.a[0] != 0xFFFFFFFF) {
                    if (k.a[0] < 16) g_st.vbuf[k.a[0]] = {(uint64_t)k.a[2] << 32 | k.a[1], k.a[3]};  // size_t is 32-bit
                } else {
                    for (uint32_t i = 0; i < k.a[2] && k.a[1] + i < 16; i++) {
                        uint64_t a, s;
                        memcpy(&a, &k.blob[16 * i], 8);
                        memcpy(&s, &k.blob[16 * i + 8], 8);
                        g_st.vbuf[k.a[1] + i] = {a, s};
                    }
                }
                break;
            case OP_UNIFORM:
                if (k.a[0] >= 6) break;
                if (k.a[1] != 0xFFFFFFFF) {
                    if (k.a[1] < 18) g_st.ubuf[k.a[0]][k.a[1]] = {(uint64_t)k.a[3] << 32 | k.a[2], k.a[4]};  // size_t is 32-bit
                } else {
                    for (uint32_t i = 0; i < k.a[3] && k.a[2] + i < 18; i++) {
                        uint64_t a, s;
                        memcpy(&a, &k.blob[16 * i], 8);
                        memcpy(&s, &k.blob[16 * i + 8], 8);
                        g_st.ubuf[k.a[0]][k.a[2] + i] = {a, s};
                    }
                }
                break;
            case OP_TEXTURE:
                if (k.a[0] >= 6) break;
                if (g_log_draws > 0) log("  bind texture stage %u index %d count %u: %08x%08x", k.a[0], (int)(k.a[1] != 0xFFFFFFFF ? k.a[1] : k.a[2]), k.a[1] != 0xFFFFFFFF ? 1u : k.a[3], k.a[1] != 0xFFFFFFFF ? k.a[3] : 0u, k.a[1] != 0xFFFFFFFF ? k.a[2] : 0u);
                if (k.a[1] != 0xFFFFFFFF) {
                    if (k.a[1] < 32) g_st.tex[k.a[0]][k.a[1]] = (uint64_t)k.a[3] << 32 | k.a[2];
                } else {
                    for (uint32_t i = 0; i < k.a[3] && k.a[2] + i < 32; i++) memcpy(&g_st.tex[k.a[0]][k.a[2] + i], &k.blob[8 * i], 8);
                }
                break;
            case OP_STORAGE:  // stage, index, addr64, size64
                if (k.a[0] < 6 && k.a[1] < 16) g_st.sbuf[k.a[0]][k.a[1]] = {(uint64_t)k.a[3] << 32 | k.a[2], k.a[4]};
                break;
            case OP_DISPATCH:
                g_recent[g_recent_n++ % 1024] = {{g_st.programs[5] ? (uint32_t)g_st.programs[5]->stages[0].code : 0, 0}, k.op, k.a[0], k.a[1], 0};
                g_break = "dispatch";
                end_rendering(cb);
                ms_flush(cb, "dispatch");
                dispatch(cb, k);
                break;
            case OP_SYNC: sync_mark(k.a[0], gpu_recording_serial()); break;  // (sync, condition, flags)
            case OP_TEX_POOL: g_st.tex_pool = k.a[0]; break;
            case OP_SMP_POOL: g_st.smp_pool = k.a[0]; break;
            case OP_BLEND_COLOR: if (k.blob.size() >= 16) memcpy(g_st.blend_color, k.blob.data(), 16); break;
            case OP_STENCIL_REF: g_st.stencil_ref = k.a[1]; break;
            case OP_ALPHA_REF: memcpy(&g_st.alpha_ref, &k.a[0], 4); break;
            case OP_INLINE_UNIFORM: {  // update buffer contents in GPU order
                g_break = "inline uniform";
                end_rendering(cb);
                GuestBuffer gb = guest_buffer((uint64_t)k.a[1] << 32 | k.a[0], (uint64_t)k.a[2] + k.blob.size());
                if (!gb.buf || k.blob.empty() || k.blob.size() > 65536 || ((gb.offset + k.a[2]) & 3) ||
                    gb.size < k.a[2] + k.blob.size()) {
                    static int bad = 0;
                    if (bad++ < 5) log("vk: skipped inline uniform update (off %u size %zu room %llu)", k.a[2], k.blob.size(), (unsigned long long)gb.size);
                    break;
                }
                vkCmdUpdateBuffer(cb, gb.buf, gb.offset + k.a[2], (k.blob.size() + 3) & ~3ull, k.blob.data());
                vb_gpu_wrote(((uint64_t)k.a[1] << 32 | k.a[0]) + k.a[2], k.blob.size());
                full_barrier(cb);
                break;
            }
            case OP_DRAW: case OP_DRAW_INDEXED: case OP_DRAW_INSTANCED: case OP_DRAW_INDEXED_INSTANCED:
                draw(cb, k);
                break;
            default:
                break;
        }
    }
    g_break = "end of list";
    end_rendering(cb);
    ms_flush(cb, "end");
    // once a minute (was every 20000 draws = ~7 pairs of lines a second at high fps: disk writes + a buried log)
    static auto lastlog = std::chrono::steady_clock::now() - std::chrono::seconds(50);
    if (auto now = std::chrono::steady_clock::now(); now - lastlog > std::chrono::seconds(60)) {
        lastlog = now;
        log("vk: skipped: no program %llu, no target %llu, no pipeline %llu, u8 index %llu, no index buffer %llu, other %llu",
            (unsigned long long)g_skip[0], (unsigned long long)g_skip[1], (unsigned long long)g_skip[2],
            (unsigned long long)g_skip[3], (unsigned long long)g_skip[4], (unsigned long long)g_skip[5]);
        log("vk: %llu draws, %llu drawn; clears %llu, buffer->texture copies %llu, texture binds %llu (%llu with data), %llu re-uploads",
            (unsigned long long)g_st.draws, (unsigned long long)g_st.drawn, (unsigned long long)g_stats_clears,
            (unsigned long long)g_stats_copies, (unsigned long long)g_stats_tex_draws, (unsigned long long)g_stats_tex_written,
            (unsigned long long)g_reuploads);
    }
}
void replay(VkCommandBuffer cb, const std::vector<Cmd>& cmds) { replay_list(cb, cmds); }
void replay(VkCommandBuffer cb, const std::vector<const Cmd*>& cmds) { replay_list(cb, CmdRefs{cmds.data(), cmds.size()}); }

}  // namespace nvn

void vk::stats_flush() { nvn::stats_flush_impl(); }
