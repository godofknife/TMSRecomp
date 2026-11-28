// AMD FSR 3 temporal anti-aliasing / upscaling through AMD's FidelityFX API DLLs (amd_fidelityfx_loader_vk.dll +
// amd_fidelityfx_upscaler_vk.dll next to tms.exe, MIT licence per the SDK 2.4.0 docs/license.md; NOTES s62). Loaded at
// run time: without the DLLs, or when the context cannot be made, fsr3_dispatch returns false and the caller falls
// back (SMAA). Only the documented ffx API is used (ffx_api.h, ffx_upscale.h, vk/ffx_api_vk.h, MIT headers).
// Linux (s131): the same API from libamd_fidelityfx_vk.so next to tms, built from the FidelityFX SDK v1.1.4 source
// (MIT; CMakeLists.txt, tools/fsr3linux) with NX_FFX_114 = its headers (no debug-visualization flag, no
// viewSpaceToMetersFactor). Without the library (or a build without the SDK source) callers fall back to SMAA.
#if defined(_WIN32) || defined(NX_FFX_114)
#include "rt.h"
#include "vk.h"
#ifndef _WIN32
#define __declspec(x)  // the SDK headers mark the entry points dllexport
#endif
#include "ffx_api.h"
#include "ffx_api_types.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <climits>
#include <cwchar>
#include <string>
#endif
#include <algorithm>

using namespace rt;

namespace vk {

namespace {
struct Ffx {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    ffxContext ctx = nullptr;
    uint32_t w = 0, h = 0;
    bool failed = false;
};
Ffx g_ffx;

void ffx_message(uint32_t type, const wchar_t* msg) {
    char b[512];
#ifdef _WIN32
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, b, sizeof b, nullptr, nullptr);
#else
    size_t n = 0;
    for (; msg && msg[n] && n + 1 < sizeof b; n++) b[n] = msg[n] < 128 ? (char)msg[n] : '?';  // the SDK's messages are ASCII
    b[n] = 0;
#endif
    log("fsr3: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

bool load() {
    if (g_ffx.create) return true;
    if (g_ffx.failed) return false;
#ifdef _WIN32
    HMODULE m = LoadLibraryA("amd_fidelityfx_loader_vk.dll");
    if (!m) {
        log("fsr3: amd_fidelityfx_loader_vk.dll not found next to tms.exe (error %lu): FSR 3 off, SMAA instead", GetLastError());
        g_ffx.failed = true;
        return false;
    }
#define FFX_SYM(n) GetProcAddress(m, n)
#else
    char exe[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    std::string path = len > 0 ? std::string(exe, (size_t)len) : std::string();
    path = path.substr(0, path.rfind('/') + 1) + "libamd_fidelityfx_vk.so";
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        log("fsr3: %s not loaded (%s): FSR 3 off, SMAA instead", path.c_str(), dlerror());
        g_ffx.failed = true;
        return false;
    }
#define FFX_SYM(n) dlsym(m, n)
#endif
    g_ffx.create = (PfnFfxCreateContext)FFX_SYM("ffxCreateContext");
    g_ffx.destroy = (PfnFfxDestroyContext)FFX_SYM("ffxDestroyContext");
    g_ffx.query = (PfnFfxQuery)FFX_SYM("ffxQuery");
    g_ffx.dispatch = (PfnFfxDispatch)FFX_SYM("ffxDispatch");
#undef FFX_SYM
    if (!g_ffx.create || !g_ffx.destroy || !g_ffx.query || !g_ffx.dispatch) {
        log("fsr3: the loader DLL lacks the ffx entry points: FSR 3 off");
        g_ffx.create = nullptr;
        g_ffx.failed = true;
        return false;
    }
    return true;
}

// one context per output size (render size = output size: native AA; smaller with upscaling)
bool context(uint32_t w, uint32_t h) {
    if (g_ffx.ctx && g_ffx.w == w && g_ffx.h == h) return true;
    if (g_ffx.failed || !load()) return false;
    if (g_ffx.ctx) {
        vkDeviceWaitIdle(dev);  // a size change (restart-only setting): rare
        g_ffx.destroy(&g_ffx.ctx, nullptr);
        g_ffx.ctx = nullptr;
    }
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = {dev, phys, device_proc_addr()};
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    // the input is the game's tone-mapped frame (0..1): no HDR flag; depth 0 near .. 1 far (not inverted)
    static const bool debug = getenv("NX_FSR3_DEBUG") != nullptr;  // debug: FSR's own view (motion vectors, history ...)
#ifdef NX_FFX_114
    cd.flags = 0;
    (void)debug;
#else
    cd.flags = debug ? FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0;
#endif
    cd.maxRenderSize = {w, h};
    cd.maxUpscaleSize = {w, h};
    cd.fpMessage = ffx_message;
    ffxReturnCode_t r = g_ffx.create(&g_ffx.ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        log("fsr3: ffxCreateContext failed (%u) at %ux%u: FSR 3 off, SMAA instead", r, w, h);
        g_ffx.ctx = nullptr;
        g_ffx.failed = true;
        return false;
    }
    g_ffx.w = w, g_ffx.h = h;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    g_ffx.query(&g_ffx.ctx, &pv.header);
    log("fsr3: context %ux%u, provider %s", w, h, pv.versionName ? pv.versionName : "?");
    return true;
}

FfxApiResource res(const Image* im, uint32_t ffx_format, bool uav) {
    FfxApiResource r{};
    r.resource = (void*)im->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffx_format;
    r.description.width = im->w;
    r.description.height = im->h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = uav ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = uav ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;  // all images are GENERAL
    return r;
}
uint32_t color_format(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ? FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT : FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
}
}  // namespace

bool fsr3_ready(uint32_t w, uint32_t h) { return context(w, h); }

void fsr3_jitter(uint64_t frame, uint32_t render_w, float* jx, float* jy) {
    int32_t phases = 8;
    if (g_ffx.ctx) {
        ffxQueryDescUpscaleGetJitterPhaseCount pc{};
        pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        pc.renderWidth = render_w, pc.displayWidth = g_ffx.w, pc.pOutPhaseCount = &phases;
        g_ffx.query(&g_ffx.ctx, &pc.header);
        ffxQueryDescUpscaleGetJitterOffset jo{};
        jo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        jo.index = (int32_t)(frame % (uint64_t)std::max(phases, 1)), jo.phaseCount = phases, jo.pOutX = jx, jo.pOutY = jy;
        if (g_ffx.query(&g_ffx.ctx, &jo.header) == FFX_API_RETURN_OK) return;
    }
    *jx = *jy = 0;
}

bool fsr3_dispatch(const Fsr3Frame& f) {
    if (!context(f.out->w, f.out->h)) return false;
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void*)f.cb;
    d.color = res(f.color, color_format(f.color->format), false);
    d.depth = res(f.depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, false);
    d.motionVectors = res(f.mv, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, false);
    d.output = res(f.out, color_format(f.out->format), true);
    d.jitterOffset = {f.jx, f.jy};
    d.motionVectorScale = {1, 1};  // Motion.cs writes pixels
    d.renderSize = {f.color->w, f.color->h};
    d.upscaleSize = {f.out->w, f.out->h};
    d.enableSharpening = f.sharpness > 0;
    d.sharpness = f.sharpness;
    d.frameTimeDelta = f.dt_ms;
    d.preExposure = 1;
    d.reset = f.reset;
    d.cameraNear = f.cam_near;
    d.cameraFar = f.cam_far;
    d.cameraFovAngleVertical = f.fov_y;
#ifndef NX_FFX_114
    d.viewSpaceToMetersFactor = 0.01f;  // ponytail: assumed cm units (Itsuki ~170 tall); only tunes FSR's heuristics
#endif
    static const bool debug = getenv("NX_FSR3_DEBUG") != nullptr;
    d.flags = debug ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0;
    ffxReturnCode_t r = g_ffx.dispatch(&g_ffx.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int told = 0;
        if (told++ < 5) log("fsr3: dispatch failed (%u)", r);
        return false;
    }
    return true;
}

}  // namespace vk
#else
// Linux build without the FidelityFX SDK source (NX_FFX_SRC): no FSR 3; callers fall back to SMAA.
#include "rt.h"
#include "vk.h"

using namespace rt;

namespace vk {
bool fsr3_ready(uint32_t, uint32_t) {
    static bool said = false;
    if (!said) said = true, log("fsr3: not in this build (no FidelityFX SDK source): SMAA instead");
    return false;
}
void fsr3_jitter(uint64_t, uint32_t, float* jx, float* jy) { *jx = *jy = 0; }
bool fsr3_dispatch(const Fsr3Frame&) { return false; }
}  // namespace vk
#endif
