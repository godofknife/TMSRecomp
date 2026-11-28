// NVIDIA DLSS / DLAA (Anti-aliasing = DLSS, setting fx_fxaa 4; NOTES s64). Nothing of NVIDIA is compiled in or shipped: NGX is
// the graphics driver's own _nvngx.dll (registry NGXCore), loaded at run time, and it loads nvngx_dlss.dll from the folder of
// tms.exe, which the setup downloads from NVIDIA's DLSS repository only when the player ticks it. The few NGX types and
// parameter names below are written from NVIDIA's documented ABI. Without the driver core, the DLL or an RTX GPU every call
// says "not available" and the caller uses FSR 3.
// Linux (s129): the same, with the driver's libnvidia-ngx.so.1 (dlopen; NVIDIA's Linux loader looks up the same
// NVSDK_NGX_VULKAN_* names in it) and libnvidia-ngx-dlss.so.<version> next to tms, also downloaded by the setup.
#include "rt.h"
#include "vk.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <filesystem>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace rt;

namespace vk {

namespace {
using NgxResult = int;  // 1 success, 0xBAD00000 | n failure
bool ok(NgxResult r) { return ((unsigned)r & 0xFFF00000u) != 0xBAD00000u; }
constexpr int k_api_version = 0x15;
constexpr int k_feature_super_sampling = 1;
constexpr int k_flag_mv_low_res = 1 << 1;  // motion vectors at the render size; no HDR flag: the input is tone mapped

// The driver's parameter object is a C++ class used through its vtable. The SDK declares Set(u64, float, double, u32, int,
// d3d11, d3d12, void*), then Get in the same order; MSVC lays each overload group out in reverse, the Itanium ABI (Linux)
// keeps the declared order (no virtual destructor). dlss_ready checks the layout with a set / get round trip.
struct NgxParam { void* const* vt; };
#ifdef _WIN32
enum { S_PTR = 0, S_INT = 3, S_U32 = 4, S_FLOAT = 6, G_PTR = 8, G_INT = 11, G_U32 = 12, G_FLOAT = 14 };
#else
enum { S_FLOAT = 1, S_U32 = 3, S_INT = 4, S_PTR = 7, G_FLOAT = 9, G_U32 = 11, G_INT = 12, G_PTR = 15 };
using HMODULE = void*;
#endif
void seti(NgxParam* p, const char* n, int v) { ((void (*)(NgxParam*, const char*, int))p->vt[S_INT])(p, n, v); }
void setu(NgxParam* p, const char* n, unsigned v) { ((void (*)(NgxParam*, const char*, unsigned))p->vt[S_U32])(p, n, v); }
void setf(NgxParam* p, const char* n, float v) { ((void (*)(NgxParam*, const char*, float))p->vt[S_FLOAT])(p, n, v); }
void setp(NgxParam* p, const char* n, void* v) { ((void (*)(NgxParam*, const char*, void*))p->vt[S_PTR])(p, n, v); }
int geti(NgxParam* p, const char* n) {
    int v = 0;
    return ok(((NgxResult (*)(NgxParam*, const char*, int*))p->vt[G_INT])(p, n, &v)) ? v : 0;
}
float getf(NgxParam* p, const char* n) {
    float v = 0;
    return ok(((NgxResult (*)(NgxParam*, const char*, float*))p->vt[G_FLOAT])(p, n, &v)) ? v : 0;
}

struct NgxImage { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned w, h; };
struct NgxResource { union { NgxImage img; struct { VkBuffer b; unsigned n; } buf; } r; int type; bool rw; };

using PfnInit = NgxResult (*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
                              PFN_vkGetDeviceProcAddr, int, const void*);
using PfnReqExt = NgxResult (*)(unsigned*, const char***, unsigned*, const char***);
using PfnParams = NgxResult (*)(NgxParam**);
using PfnCreate = NgxResult (*)(VkCommandBuffer, int, NgxParam*, void**);
using PfnEval = NgxResult (*)(VkCommandBuffer, const void*, const NgxParam*, void*);
using PfnRelease = NgxResult (*)(void*);

struct {
    HMODULE core = nullptr;
    PfnInit init = nullptr;
    PfnReqExt req_ext = nullptr;
    PfnParams caps = nullptr, alloc = nullptr;
    PfnCreate create = nullptr;
    PfnEval eval = nullptr;
    PfnRelease release = nullptr;
    NgxParam* params = nullptr;
    void* feat = nullptr;
    std::vector<std::pair<uint64_t, void*>> old;  // replaced features: released once the GPU is past their last submit
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    int state = 0;      // 0 not tried, 1 up, -1 unavailable
    bool tried = false;
} N;

#ifdef _WIN32
std::wstring exe_dir() {
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    while (n && b[n - 1] != L'\\') n--;
    return std::wstring(b, n);
}
#define GetProcAddress_ GetProcAddress
#else
std::wstring exe_dir() {  // with a trailing slash; wchar_t is UTF-32 here (NGX's Linux paths are wchar_t too)
    std::error_code ec;
    const std::string d = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().string() + "/";
    std::wstring w;
    for (size_t i = 0; i < d.size();) {  // UTF-8 -> UTF-32
        const uint8_t b = (uint8_t)d[i];
        const int n = b < 0x80 ? 1 : b >> 5 == 6 ? 2 : b >> 4 == 14 ? 3 : b >> 3 == 30 ? 4 : 1;
        uint32_t c = n == 1 ? b : n == 2 ? b & 0x1F : n == 3 ? b & 0x0F : b & 0x07;
        for (int k = 1; k < n && i + k < d.size(); k++) c = c << 6 | ((uint8_t)d[i + k] & 0x3F);
        w += (wchar_t)c, i += n;
    }
    return w;
}
#define GetProcAddress_ dlsym
#endif

// Only touched when the player has the DLSS file: everybody else's device stays exactly as it was.
bool load_core() {
    if (N.tried) return N.core != nullptr;
    N.tried = true;
#ifdef _WIN32
    if (GetFileAttributesW((exe_dir() + L"nvngx_dlss.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        log("dlss: nvngx_dlss.dll is not next to tms.exe: DLSS off (setup option 'Download NVIDIA DLSS')");
        return false;
    }
    wchar_t dir[MAX_PATH];
    DWORD size = sizeof dir;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) {
        log("dlss: no NVIDIA NGX core in this driver: DLSS off");
        return false;
    }
    const std::wstring path = std::wstring(dir) + L"\\_nvngx.dll";
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) { log("dlss: cannot load the driver's _nvngx.dll (error %lu): DLSS off", GetLastError()); return false; }
#else
    bool have = false;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        have |= e.path().filename().string().rfind("libnvidia-ngx-dlss.so.", 0) == 0;
    if (!have) {
        log("dlss: libnvidia-ngx-dlss.so.* is not next to tms: DLSS off (setup option 'NVIDIA DLSS')");
        return false;
    }
    HMODULE m = dlopen("libnvidia-ngx.so.1", RTLD_NOW | RTLD_LOCAL);  // part of NVIDIA's Linux driver
    if (!m) { log("dlss: no NVIDIA NGX core (libnvidia-ngx.so.1: %s): DLSS off", dlerror()); return false; }
#endif
    N.init = (PfnInit)GetProcAddress_(m, "NVSDK_NGX_VULKAN_Init_Ext2");
    N.req_ext = (PfnReqExt)GetProcAddress_(m, "NVSDK_NGX_VULKAN_RequiredExtensions");
    N.caps = (PfnParams)GetProcAddress_(m, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    N.alloc = (PfnParams)GetProcAddress_(m, "NVSDK_NGX_VULKAN_AllocateParameters");
    N.create = (PfnCreate)GetProcAddress_(m, "NVSDK_NGX_VULKAN_CreateFeature");
    N.eval = (PfnEval)GetProcAddress_(m, "NVSDK_NGX_VULKAN_EvaluateFeature");
    N.release = (PfnRelease)GetProcAddress_(m, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!N.init || !N.req_ext || !N.caps || !N.alloc || !N.create || !N.eval || !N.release) {
        log("dlss: the NGX core lacks the Vulkan entry points: DLSS off");
        return false;
    }
    N.core = m;
    return true;
}

void add_unique(std::vector<const char*>& v, const char* s) {
    for (auto* e : v) if (!strcmp(e, s)) return;
    v.push_back(s);
}

NgxResource image_res(const Image* im, bool rw, VkImageView view) {
    NgxResource r{};
    r.r.img = {view, im->image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, im->format, im->w, im->h};
    r.type = 0;
    r.rw = rw;
    return r;
}
float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}
}  // namespace

void dlss_instance_exts(std::vector<const char*>& out) {
    unsigned ni = 0, nd = 0;
    const char** ie = nullptr; const char** de = nullptr;
    if (!load_core() || !ok(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++) add_unique(out, ie[i]);
}

void dlss_device_exts(const std::vector<VkExtensionProperties>& have, std::vector<const char*>& out) {
    unsigned ni = 0, nd = 0;
    const char** ie = nullptr; const char** de = nullptr;
    if (!N.core || !ok(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        bool there = false;
        for (auto& e : have) there |= !strcmp(e.extensionName, de[i]);
        if (there) add_unique(out, de[i]);
        else log("dlss: the device lacks %s, which NGX asks for", de[i]);
    }
}

bool dlss_ready() {
    if (N.state) return N.state > 0;
    N.state = -1;
    if (!N.core) return false;
    const std::wstring dir = exe_dir();
    // 231313132: the application id NVIDIA's DLSS samples use for programs without an NVIDIA-issued id
    NgxResult r = N.init(231313132ull, dir.c_str(), g_inst_handle(), phys, dev, instance_proc_addr(), device_proc_addr(), k_api_version, nullptr);
    if (!ok(r)) { log("dlss: NGX init failed (%08x): DLSS off", (unsigned)r); return false; }
    if (!ok(N.alloc(&N.params)) || !N.params) { log("dlss: no NGX parameter object: DLSS off"); return false; }
    seti(N.params, "TMS.Check", -12345);
    setf(N.params, "TMS.CheckF", 1.25f);
    if (geti(N.params, "TMS.Check") != -12345 || getf(N.params, "TMS.CheckF") != 1.25f) {
        log("dlss: NGX parameter layout check failed: DLSS off");
        return false;
    }
    NgxParam* caps = nullptr;
    int avail = 0, needs_driver = 0;
    if (ok(N.caps(&caps)) && caps) avail = geti(caps, "SuperSampling.Available"), needs_driver = geti(caps, "SuperSampling.NeedsUpdatedDriver");
    log("dlss: NGX up, DLSS %s%s", avail ? "available" : "NOT available (GPU without RTX?)", needs_driver ? ", the driver needs an update" : "");
    if (!avail) return false;
    N.state = 1;
    return true;
}

void dlss_jitter(uint64_t frame, float ratio, float* jx, float* jy) {
    const uint32_t phases = (uint32_t)std::lround(8.f * ratio * ratio);  // NVIDIA's guidance: 8 x (output / render)^2
    const uint32_t i = (uint32_t)(frame % std::max(phases, 1u)) + 1;
    *jx = halton(i, 2) - 0.5f;
    *jy = halton(i, 3) - 0.5f;
}

bool dlss_dispatch(const Fsr3Frame& f) {
    if (!dlss_ready()) return false;
    bool reset = f.reset;
    for (size_t i = 0; i < N.old.size();)
        if (gpu_wait(N.old[i].first, 0)) { N.release(N.old[i].second); N.old[i] = N.old.back(); N.old.pop_back(); }
        else i++;
    if (!N.feat || N.rw != f.color->w || N.rh != f.color->h || N.ow != f.out->w || N.oh != f.out->h) {
        // no vkDeviceWaitIdle: with the render thread other threads submit meanwhile (unsynchronized queue = device fault
        // "address 0 write" after a few live mode switches, user 11-22); the old feature's last use is already submitted
        if (N.feat) N.old.push_back({gpu_last_submitted(), N.feat}), N.feat = nullptr;
        const float ratio = (float)f.out->w / (float)f.color->w;
        // NVSDK_NGX_PerfQuality_Value: 0 max performance, 1 balanced, 2 max quality, 3 ultra performance, 5 DLAA
        const int quality = ratio < 1.05f ? 5 : ratio < 1.6f ? 2 : ratio < 1.85f ? 1 : ratio < 2.5f ? 0 : 3;
        setu(N.params, "CreationNodeMask", 1);
        setu(N.params, "VisibilityNodeMask", 1);
        setu(N.params, "Width", f.color->w);
        setu(N.params, "Height", f.color->h);
        setu(N.params, "OutWidth", f.out->w);
        setu(N.params, "OutHeight", f.out->h);
        seti(N.params, "PerfQualityValue", quality);
        seti(N.params, "DLSS.Feature.Create.Flags", k_flag_mv_low_res);
        seti(N.params, "DLSS.Enable.Output.Subrects", 0);
        // setting dlss_preset (user 11-23): NVSDK_NGX_DLSS_Hint_Render_Preset for every mode: 10 J, 11 K (transformer), 12 L,
        // 13 M (DLSS 4.5's 2nd-gen transformer); 0 / unset = NVIDIA's default per mode (K, M at Performance, L at Ultra
        // Performance with DLL 310.5+), so nothing is set then. Values the DLL does not know revert to its default.
        static const int preset = getenv("NX_DLSS_PRESET") ? std::clamp(atoi(getenv("NX_DLSS_PRESET")), 0, 13) : 0;
        if (preset >= 10)
            for (const char* m : {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance", "UltraQuality"})
                setu(N.params, (std::string("DLSS.Hint.Render.Preset.") + m).c_str(), (unsigned)preset);
        NgxResult r = N.create(f.cb, k_feature_super_sampling, N.params, &N.feat);
        if (!ok(r) || !N.feat) {
            log("dlss: feature creation failed (%08x) at %ux%u -> %ux%u: FSR 3 instead", (unsigned)r, f.color->w, f.color->h, f.out->w, f.out->h);
            N.state = -1;
            return false;
        }
        N.rw = f.color->w, N.rh = f.color->h, N.ow = f.out->w, N.oh = f.out->h;
        reset = true;
        log("dlss: feature %ux%u -> %ux%u, quality value %d, preset %s", N.rw, N.rh, N.ow, N.oh, quality,
            preset >= 10 ? std::string(1, (char)('A' + preset - 1)).c_str() : "default");
    }
    NgxResource color = image_res(f.color, false, f.color->view), depth = image_res(f.depth, false, f.depth->view);
    NgxResource mv = image_res(f.mv, false, f.mv->view), out = image_res(f.out, true, f.out->view);
    setp(N.params, "Color", &color);
    setp(N.params, "Output", &out);
    setp(N.params, "Depth", &depth);
    setp(N.params, "MotionVectors", &mv);
    static const float jsign = getenv("NX_DLSS_JSIGN") ? (float)atof(getenv("NX_DLSS_JSIGN")) : 1.f;  // debug
    setf(N.params, "Jitter.Offset.X", jsign * f.jx);
    setf(N.params, "Jitter.Offset.Y", jsign * f.jy);
    setf(N.params, "MV.Scale.X", 1);   // Motion.cs writes pixels
    setf(N.params, "MV.Scale.Y", 1);
    setf(N.params, "Sharpness", 0);
    seti(N.params, "Reset", reset ? 1 : 0);
    setu(N.params, "DLSS.Render.Subrect.Dimensions.Width", f.color->w);
    setu(N.params, "DLSS.Render.Subrect.Dimensions.Height", f.color->h);
    setf(N.params, "FrameTimeDeltaInMsec", f.dt_ms);
    NgxResult r = N.eval(f.cb, N.feat, N.params, nullptr);
    if (!ok(r)) {
        log("dlss: evaluation failed (%08x): FSR 3 instead", (unsigned)r);
        N.state = -1;
        return false;
    }
    return true;
}

}  // namespace vk
