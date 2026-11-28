// GPU VP9 decoding on Linux (movie_mf.h interface, s130): VA-API, the GPU's video engine (Mesa radeonsi on the Deck / AMD,
// Intel iHD). libva-drm is loaded at run time (dlopen libva.so.2 + libva-drm.so.2), so nothing new is needed to build or
// run: no libva, no render node, no VP9 profile 0 decoder = mfvp9_open returns null and the caller keeps libvpx.
// VA-API wants the uncompressed frame header parsed by the application (vp9h:: below, written from the VP9 bitstream
// spec; the compressed header and the tiles are the driver's). Self-check: the first stream of a run also decodes its
// first kCheck shown frames with libvpx; any byte that differs = this PC's VA decoder is not trusted (logged, the stream
// finishes on libvpx, every later stream uses libvpx).
#include "movie_mf.h"
#if defined(__linux__) || defined(VAVP9_FAKE_LINUX) || defined(VAVP9_TEST)
#include <cstdint>
#include <algorithm>
#include <cstring>

// ------------------------------------------------------------------ VP9 uncompressed header (platform neutral)
namespace vp9h {
struct Bits {
    const uint8_t* p;
    uint32_t n, pos = 0;  // pos in bits
    bool over = false;
    uint32_t f(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; i++, pos++) {
            if (pos >= n * 8) { over = true; return 0; }
            v = v << 1 | (p[pos >> 3] >> (7 - (pos & 7)) & 1);
        }
        return v;
    }
    int su(int bits) { int v = (int)f(bits); return f(1) ? -v : v; }
};
// state carried from frame to frame (the spec's persistent variables)
struct State {
    int8_t ref_deltas[4] = {1, 0, -1, -1}, mode_deltas[2] = {0, 0};
    bool seg_abs = false;
    int16_t seg_data[8][4] = {};
    uint8_t seg_mask[8] = {};
    uint8_t tree_probs[7] = {255, 255, 255, 255, 255, 255, 255}, pred_probs[3] = {255, 255, 255};
    bool seg_temporal = false;
    uint16_t ref_w[8] = {}, ref_h[8] = {};
};
struct Hdr {
    int profile = 0, show_existing = 0, show_idx = 0, key = 0, show = 0, err_res = 0, intra_only = 0, reset_ctx = 0;
    int refresh = 0, ref_idx[3] = {}, sign_bias[3] = {}, hp = 0, interp = 0, refresh_ctx = 0, parallel = 0, ctx_idx = 0;
    int w = 0, h = 0, ss_x = 1, ss_y = 1, bit_depth = 8;
    int lf_level = 0, sharp = 0, lf_delta_enabled = 0;
    int base_q = 0, dq_ydc = 0, dq_uvdc = 0, dq_uvac = 0, lossless = 0;
    int seg_enabled = 0, seg_update_map = 0, seg_temporal = 0;
    int log2_cols = 0, log2_rows = 0;
    uint32_t uncomp_size = 0, comp_size = 0;
};
static const int16_t kDc[256] = {
    4, 8, 8, 9, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 26,
    27, 28, 29, 30, 31, 32, 32, 33, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42, 43, 43, 44, 45, 46, 47,
    48, 48, 49, 50, 51, 52, 53, 53, 54, 55, 56, 57, 57, 58, 59, 60, 61, 62, 62, 63, 64, 65, 66, 66,
    67, 68, 69, 70, 70, 71, 72, 73, 74, 74, 75, 76, 77, 78, 78, 79, 80, 81, 81, 82, 83, 84, 85, 85,
    87, 88, 90, 92, 93, 95, 96, 98, 99, 101, 102, 104, 105, 107, 108, 110, 111, 113, 114, 116, 117, 118, 120, 121,
    123, 125, 127, 129, 131, 134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 161, 164, 166, 169, 172, 174,
    177, 180, 182, 185, 187, 190, 192, 195, 199, 202, 205, 208, 211, 214, 217, 220, 223, 226, 230, 233, 237, 240, 243, 247,
    250, 253, 257, 261, 265, 269, 272, 276, 280, 284, 288, 292, 296, 300, 304, 309, 313, 317, 322, 326, 330, 335, 340, 344,
    349, 354, 359, 364, 369, 374, 379, 384, 389, 395, 400, 406, 411, 417, 423, 429, 435, 441, 447, 454, 461, 467, 475, 482,
    489, 497, 505, 513, 522, 530, 539, 549, 559, 569, 579, 590, 602, 614, 626, 640, 654, 668, 684, 700, 717, 736, 755, 775,
    796, 819, 843, 869, 896, 925, 955, 988, 1022, 1058, 1098, 1139, 1184, 1232, 1282, 1336,
};
static const int16_t kAc[256] = {
    4, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78,
    79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102,
    104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126, 128, 130, 132, 134, 136, 138, 140, 142, 144, 146, 148, 150,
    152, 155, 158, 161, 164, 167, 170, 173, 176, 179, 182, 185, 188, 191, 194, 197, 200, 203, 207, 211, 215, 219, 223, 227,
    231, 235, 239, 243, 247, 251, 255, 260, 265, 270, 275, 280, 285, 290, 295, 300, 305, 311, 317, 323, 329, 335, 341, 347,
    353, 359, 366, 373, 380, 387, 394, 401, 408, 416, 424, 432, 440, 448, 456, 465, 474, 483, 492, 501, 510, 520, 530, 540,
    550, 560, 571, 582, 593, 604, 615, 627, 639, 651, 663, 676, 689, 702, 715, 729, 743, 757, 771, 786, 801, 816, 832, 848,
    864, 881, 898, 915, 933, 951, 969, 988, 1007, 1026, 1046, 1066, 1087, 1108, 1129, 1151, 1173, 1196, 1219, 1243, 1267, 1292, 1317, 1343,
    1369, 1396, 1423, 1451, 1479, 1508, 1537, 1567, 1597, 1628, 1660, 1692, 1725, 1759, 1793, 1828,
};
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// one frame's uncompressed header; updates `s` (loop filter deltas, segmentation). false: broken or not 8-bit 4:2:0
static bool parse(const uint8_t* p, uint32_t n, State& s, Hdr& h) {
    Bits b{p, n};
    h = Hdr{};
    if (b.f(2) != 2) return false;  // frame_marker
    h.profile = b.f(1);
    h.profile |= b.f(1) << 1;
    if (h.profile == 3) b.f(1);
    if (h.profile != 0) return false;  // our streams: profile 0 (8-bit 4:2:0), the only one asked for at open
    if ((h.show_existing = b.f(1))) { h.show_idx = b.f(3); return !b.over; }
    h.key = b.f(1) == 0;
    h.show = b.f(1);
    h.err_res = b.f(1);
    auto frame_size = [&] { h.w = b.f(16) + 1, h.h = b.f(16) + 1; };
    auto render_size = [&] { if (b.f(1)) b.f(16), b.f(16); };
    auto sync = [&] { return b.f(8) == 0x49 && b.f(8) == 0x83 && b.f(8) == 0x42; };
    auto color = [&] {  // profile 0: no bit depth / subsampling bits
        if (b.f(3) == 7) return false;  // CS_RGB is 4:4:4 = profile 1 / 3 only
        b.f(1);                         // color_range
        return true;
    };
    if (h.key) {
        if (!sync() || !color()) return false;
        frame_size(), render_size();
        h.refresh = 0xff;
    } else {
        h.intra_only = h.show ? 0 : b.f(1);
        h.reset_ctx = h.err_res ? 0 : b.f(2);
        if (h.intra_only) {
            if (!sync()) return false;
            h.refresh = b.f(8);
            frame_size(), render_size();
        } else {
            h.refresh = b.f(8);
            for (int i = 0; i < 3; i++) h.ref_idx[i] = b.f(3), h.sign_bias[i] = b.f(1);
            bool found = false;
            for (int i = 0; i < 3 && !found; i++)
                if ((found = b.f(1))) h.w = s.ref_w[h.ref_idx[i]], h.h = s.ref_h[h.ref_idx[i]];
            if (!found) frame_size();
            render_size();
            h.hp = b.f(1);
            static const int kLiteral[4] = {1, 0, 2, 3};  // spec literal -> filter type in VA-API / libvpx numbering
            h.interp = b.f(1) ? 4 : kLiteral[b.f(2)];
        }
    }
    if (!h.err_res) h.refresh_ctx = b.f(1), h.parallel = b.f(1);
    else h.refresh_ctx = 0, h.parallel = 1;
    h.ctx_idx = b.f(2);
    if (h.key || h.intra_only || h.err_res) {  // setup_past_independence
        memset(s.seg_data, 0, sizeof s.seg_data), memset(s.seg_mask, 0, sizeof s.seg_mask), s.seg_abs = false;
        const int8_t rd[4] = {1, 0, -1, -1};
        memcpy(s.ref_deltas, rd, 4), s.mode_deltas[0] = s.mode_deltas[1] = 0;
    }
    // loop filter
    h.lf_level = b.f(6), h.sharp = b.f(3);
    if ((h.lf_delta_enabled = b.f(1)) && b.f(1)) {
        for (int i = 0; i < 4; i++) if (b.f(1)) s.ref_deltas[i] = (int8_t)b.su(6);
        for (int i = 0; i < 2; i++) if (b.f(1)) s.mode_deltas[i] = (int8_t)b.su(6);
    }
    // quantizer
    h.base_q = b.f(8);
    auto dq = [&] { return b.f(1) ? b.su(4) : 0; };
    h.dq_ydc = dq(), h.dq_uvdc = dq(), h.dq_uvac = dq();
    h.lossless = h.base_q == 0 && !h.dq_ydc && !h.dq_uvdc && !h.dq_uvac;
    // segmentation
    if ((h.seg_enabled = b.f(1))) {
        if ((h.seg_update_map = b.f(1))) {
            for (auto& t : s.tree_probs) t = b.f(1) ? (uint8_t)b.f(8) : 255;
            s.seg_temporal = b.f(1);
            for (auto& t : s.pred_probs) t = s.seg_temporal && b.f(1) ? (uint8_t)b.f(8) : 255;
        }
        if (b.f(1)) {  // update_data
            s.seg_abs = b.f(1);
            static const int kBits[4] = {8, 6, 2, 0}, kMax[4] = {255, 63, 3, 0};
            for (int i = 0; i < 8; i++) {
                s.seg_mask[i] = 0;
                for (int j = 0; j < 4; j++) {
                    int v = 0;
                    if (b.f(1)) {
                        s.seg_mask[i] |= 1 << j;
                        v = std::min<int>((int)b.f(kBits[j]), kMax[j]);
                        if (j < 2 && b.f(1)) v = -v;
                    }
                    s.seg_data[i][j] = (int16_t)v;
                }
            }
        }
        h.seg_temporal = h.seg_update_map && s.seg_temporal;
    }
    // tiles
    const int sb64 = ((h.w + 7) / 8 + 7) / 8;
    int lo = 0, hi = 1;
    while ((64 << lo) < sb64) lo++;
    while ((sb64 >> hi) >= 4) hi++;
    hi--;
    h.log2_cols = lo;
    while (h.log2_cols < hi && b.f(1)) h.log2_cols++;
    if ((h.log2_rows = b.f(1))) h.log2_rows += b.f(1);
    h.comp_size = b.f(16);
    h.uncomp_size = (b.pos + 7) / 8;
    return !b.over && h.comp_size && h.w && h.h && h.uncomp_size + h.comp_size <= n;
}
static bool seg_on(const State& s, const Hdr& h, int seg, int feature) { return h.seg_enabled && (s.seg_mask[seg] >> feature & 1); }
// what the decoder hardware wants per segment: loop filter level [ref][mode] (ref 0 = intra) and the dequantizers
// (q: y dc, y ac, uv dc, uv ac)
static void seg_values(const State& s, const Hdr& h, int seg, uint8_t lvl[4][2], int16_t q[4]) {
    int qi = h.base_q;
    if (seg_on(s, h, seg, 0)) qi = clampi(s.seg_abs ? s.seg_data[seg][0] : qi + s.seg_data[seg][0], 0, 255);
    q[0] = kDc[clampi(qi + h.dq_ydc, 0, 255)], q[1] = kAc[qi];
    q[2] = kDc[clampi(qi + h.dq_uvdc, 0, 255)], q[3] = kAc[clampi(qi + h.dq_uvac, 0, 255)];
    int l = h.lf_level;
    if (seg_on(s, h, seg, 1)) l = clampi(s.seg_abs ? s.seg_data[seg][1] : l + s.seg_data[seg][1], 0, 63);
    if (!h.lf_delta_enabled) {
        for (int r = 0; r < 4; r++) lvl[r][0] = lvl[r][1] = (uint8_t)l;
        return;
    }
    const int sh = l >> 5;  // deltas count double from level 32
    lvl[0][0] = lvl[0][1] = (uint8_t)clampi(l + s.ref_deltas[0] * (1 << sh), 0, 63);
    for (int r = 1; r < 4; r++)
        for (int m = 0; m < 2; m++) lvl[r][m] = (uint8_t)clampi(l + (s.ref_deltas[r] + s.mode_deltas[m]) * (1 << sh), 0, 63);
}
// the frames of a packet: a superframe index (last byte 0b110xxxxx, repeated as its first byte) or one frame
static int frames_of(const uint8_t* p, uint32_t n, uint32_t off[8], uint32_t len[8]) {
    if (n) {
        const uint8_t m = p[n - 1];
        if ((m & 0xe0) == 0xc0) {
            const int count = (m & 7) + 1, mag = (m >> 3 & 3) + 1;
            const uint32_t idx = 2 + mag * count;
            if (n >= idx && p[n - idx] == m) {
                uint32_t at = 0;
                for (int i = 0; i < count; i++) {
                    uint32_t sz = 0;
                    for (int k = 0; k < mag; k++) sz |= (uint32_t)p[n - idx + 1 + i * mag + k] << (8 * k);
                    if (at + sz > n - idx) return 0;
                    off[i] = at, len[i] = sz, at += sz;
                }
                return count;
            }
        }
    }
    off[0] = 0, len[0] = n;
    return n ? 1 : 0;
}
}  // namespace vp9h

#if defined(__linux__) || defined(VAVP9_FAKE_LINUX)
// ------------------------------------------------------------------ VA-API (Linux)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <vector>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#ifndef VAVP9_TEST
#include "rt.h"
#define VLOG rt::log
#else
#define VLOG(...) (printf(__VA_ARGS__), printf("\n"))
#endif

// the libva 2.x ABI pieces used here (va.h, va_dec_vp9.h; libva.so.2 = VA-API 1.x, stable since 2017)
namespace va {
typedef void* Display;
typedef int Status;
typedef unsigned Id;
constexpr Id INVALID = 0xffffffffu;
constexpr int PROFILE_VP9_0 = 19, ENTRY_VLD = 1, ATTRIB_RT_FORMAT = 0, BUF_PIC = 0, BUF_SLICE_PARAM = 4, BUF_SLICE_DATA = 5;
constexpr unsigned RT_YUV420 = 1, PROGRESSIVE = 1, FOURCC_NV12 = 0x3231564e;
struct ConfigAttrib { int type; uint32_t value; };
struct ImageFormat { uint32_t fourcc, byte_order, bits_per_pixel, depth, red_mask, green_mask, blue_mask, alpha_mask, reserved[4]; };
struct Image {
    Id image_id;
    ImageFormat format;
    Id buf;
    uint16_t width, height;
    uint32_t data_size, num_planes, pitches[3], offsets[3];
    int32_t num_palette_entries, entry_bytes;
    int8_t component_order[4];
    uint32_t reserved[4];
};
struct PicVP9 {
    uint16_t frame_width, frame_height;
    Id reference_frames[8];
    struct {
        uint32_t subsampling_x : 1, subsampling_y : 1, frame_type : 1, show_frame : 1, error_resilient_mode : 1,
            intra_only : 1, allow_high_precision_mv : 1, mcomp_filter_type : 3, frame_parallel_decoding_mode : 1,
            reset_frame_context : 2, refresh_frame_context : 1, frame_context_idx : 2, segmentation_enabled : 1,
            segmentation_temporal_update : 1, segmentation_update_map : 1, last_ref_frame : 3,
            last_ref_frame_sign_bias : 1, golden_ref_frame : 3, golden_ref_frame_sign_bias : 1, alt_ref_frame : 3,
            alt_ref_frame_sign_bias : 1, lossless_flag : 1;
    } pic_fields;
    uint8_t filter_level, sharpness_level, log2_tile_rows, log2_tile_columns, frame_header_length_in_bytes;
    uint16_t first_partition_size;
    uint8_t mb_segment_tree_probs[7], segment_pred_probs[3], profile, bit_depth;
    uint32_t reserved[8];
};
struct SegVP9 {
    struct { uint16_t reference_enabled : 1, reference : 2, reference_skipped : 1; } segment_flags;
    uint8_t filter_level[4][2];
    int16_t luma_ac_quant_scale, luma_dc_quant_scale, chroma_ac_quant_scale, chroma_dc_quant_scale;
    uint32_t reserved[4];
};
struct SliceVP9 {
    uint32_t slice_data_size, slice_data_offset, slice_data_flag;
    SegVP9 seg_param[8];
    uint32_t reserved[4];
};
static_assert(sizeof(PicVP9) == 92 && sizeof(SegVP9) == 36 && sizeof(SliceVP9) == 316 && sizeof(Image) == 120, "libva ABI");

struct Api {
    bool ok = false;
    Display (*GetDisplayDRM)(int);
    Status (*Initialize)(Display, int*, int*);
    Status (*Terminate)(Display);
    const char* (*QueryVendorString)(Display);
    Status (*GetConfigAttributes)(Display, int, int, ConfigAttrib*, int);
    Status (*CreateConfig)(Display, int, int, ConfigAttrib*, int, Id*);
    Status (*DestroyConfig)(Display, Id);
    Status (*CreateSurfaces)(Display, unsigned, unsigned, unsigned, Id*, unsigned, void*, unsigned);
    Status (*DestroySurfaces)(Display, Id*, int);
    Status (*CreateContext)(Display, Id, int, int, int, Id*, int, Id*);
    Status (*DestroyContext)(Display, Id);
    Status (*CreateBuffer)(Display, Id, int, unsigned, unsigned, void*, Id*);
    Status (*DestroyBuffer)(Display, Id);
    Status (*BeginPicture)(Display, Id, Id);
    Status (*RenderPicture)(Display, Id, Id*, int);
    Status (*EndPicture)(Display, Id);
    Status (*SyncSurface)(Display, Id);
    Status (*CreateImage)(Display, ImageFormat*, int, int, Image*);
    Status (*DestroyImage)(Display, Id);
    Status (*GetImage)(Display, Id, int, int, unsigned, unsigned, Id);
    Status (*MapBuffer)(Display, Id, void**);
    Status (*UnmapBuffer)(Display, Id);
};
static const Api& api() {
    static Api a = [] {
        Api r{};
        void* va = dlopen("libva.so.2", RTLD_NOW | RTLD_LOCAL);
        void* drm = va ? dlopen("libva-drm.so.2", RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (!drm) return r;
        bool all = true;
        auto get = [&](void* lib, auto& fn, const char* name) { all &= (*(void**)&fn = dlsym(lib, name)) != nullptr; };
        get(drm, r.GetDisplayDRM, "vaGetDisplayDRM");
        get(va, r.Initialize, "vaInitialize"), get(va, r.Terminate, "vaTerminate");
        get(va, r.QueryVendorString, "vaQueryVendorString"), get(va, r.GetConfigAttributes, "vaGetConfigAttributes");
        get(va, r.CreateConfig, "vaCreateConfig"), get(va, r.DestroyConfig, "vaDestroyConfig");
        get(va, r.CreateSurfaces, "vaCreateSurfaces"), get(va, r.DestroySurfaces, "vaDestroySurfaces");
        get(va, r.CreateContext, "vaCreateContext"), get(va, r.DestroyContext, "vaDestroyContext");
        get(va, r.CreateBuffer, "vaCreateBuffer"), get(va, r.DestroyBuffer, "vaDestroyBuffer");
        get(va, r.BeginPicture, "vaBeginPicture"), get(va, r.RenderPicture, "vaRenderPicture");
        get(va, r.EndPicture, "vaEndPicture"), get(va, r.SyncSurface, "vaSyncSurface");
        get(va, r.CreateImage, "vaCreateImage"), get(va, r.DestroyImage, "vaDestroyImage"), get(va, r.GetImage, "vaGetImage");
        get(va, r.MapBuffer, "vaMapBuffer"), get(va, r.UnmapBuffer, "vaUnmapBuffer");
        r.ok = all;
        return r;
    }();
    return a;
}
}  // namespace va

constexpr int kSurfaces = 10, kCheck = 4;  // 8 reference slots + the frame being decoded + one spare
static std::atomic<int> g_trust{0};         // 0 unknown, 1 checked against libvpx, -1 mismatch / broken: libvpx only

struct MfVp9 {
    int fd = -1;
    va::Display dpy = nullptr;
    va::Id cfg = va::INVALID, ctx = va::INVALID, surf[kSurfaces];
    va::Image img{};
    bool have_img = false;
    uint32_t w = 0, h = 0;
    int slot[8];  // reference slot -> surface index (-1 none)
    vp9h::State st;
    // self-check / fallback decoder
    vpx_codec_ctx_t vpx{};
    bool vpx_on = false, use_vpx = false;
    int checked = 0;
    ~MfVp9() {
        const va::Api& a = va::api();
        if (vpx_on) vpx_codec_destroy(&vpx);
        if (have_img) a.DestroyImage(dpy, img.image_id);
        if (ctx != va::INVALID) a.DestroyContext(dpy, ctx);
        if (surf[0] != va::INVALID) a.DestroySurfaces(dpy, surf, kSurfaces);
        if (cfg != va::INVALID) a.DestroyConfig(dpy, cfg);
        if (dpy) a.Terminate(dpy);
        if (fd >= 0) close(fd);
    }
};

static void vpx_to_nv12(const vpx_image_t* img, uint32_t w, uint32_t h, std::vector<uint8_t>& o) {
    o.resize((size_t)w * h * 3 / 2);
    for (uint32_t y = 0; y < h; y++) memcpy(&o[(size_t)y * w], img->planes[0] + (size_t)y * img->stride[0], w);
    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t* d = &o[(size_t)w * h + (size_t)y * w];
        const uint8_t *u = img->planes[1] + (size_t)y * img->stride[1], *v = img->planes[2] + (size_t)y * img->stride[2];
        for (uint32_t x = 0; x < w / 2; x++) d[2 * x] = u[x], d[2 * x + 1] = v[x];
    }
}
static bool vpx_frame(MfVp9& d, const uint8_t* p, uint32_t n, std::vector<uint8_t>& nv12, uint32_t* w, uint32_t* h) {
    if (vpx_codec_decode(&d.vpx, p, n, nullptr, 0) != VPX_CODEC_OK) return false;
    const vpx_image_t* img = nullptr;
    vpx_codec_iter_t it = nullptr;
    while (const vpx_image_t* i = vpx_codec_get_frame(&d.vpx, &it)) img = i;
    if (!img) return false;
    *w = img->d_w & ~1u, *h = img->d_h & ~1u;
    vpx_to_nv12(img, *w, *h, nv12);
    return true;
}

MfVp9* mfvp9_open(uint32_t w, uint32_t h) {
    if (g_trust < 0) return nullptr;
    const va::Api& a = va::api();
    static std::atomic<bool> logged{false};
    auto fail = [&](const char* why) -> MfVp9* {
        if (!logged.exchange(true)) VLOG("movie: no VA-API VP9 decoding (%s): libvpx", why);
        return nullptr;
    };
    if (!a.ok) return fail("no libva.so.2 / libva-drm.so.2");
    auto d = new MfVp9;
    for (auto& s : d->surf) s = va::INVALID;
    for (auto& s : d->slot) s = -1;
    d->w = w, d->h = h;
    // the first render node whose driver decodes VP9 profile 0
    for (int node = 128; node < 136 && d->cfg == va::INVALID; node++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/renderD%d", node);
        if ((d->fd = open(path, O_RDWR | O_CLOEXEC)) < 0) continue;
        int major, minor;
        va::ConfigAttrib rt{va::ATTRIB_RT_FORMAT, 0};
        if ((d->dpy = a.GetDisplayDRM(d->fd)) && a.Initialize(d->dpy, &major, &minor) == 0 &&
            a.GetConfigAttributes(d->dpy, va::PROFILE_VP9_0, va::ENTRY_VLD, &rt, 1) == 0 && (rt.value & va::RT_YUV420) &&
            a.CreateConfig(d->dpy, va::PROFILE_VP9_0, va::ENTRY_VLD, &rt, 1, &d->cfg) == 0) {
            if (!logged.exchange(true)) VLOG("movie: VA-API VP9 on %s (%s)", path, a.QueryVendorString(d->dpy));
            break;
        }
        d->cfg = va::INVALID;
        if (d->dpy) a.Terminate(d->dpy), d->dpy = nullptr;
        close(d->fd), d->fd = -1;
    }
    if (d->cfg == va::INVALID) return delete d, fail("no render node with a VP9 profile 0 decoder");
    if (a.CreateSurfaces(d->dpy, va::RT_YUV420, w, h, d->surf, kSurfaces, nullptr, 0) != 0) {
        for (auto& s : d->surf) s = va::INVALID;
        return delete d, fail("vaCreateSurfaces");
    }
    if (a.CreateContext(d->dpy, d->cfg, (int)w, (int)h, va::PROGRESSIVE, d->surf, kSurfaces, &d->ctx) != 0) {
        d->ctx = va::INVALID;
        return delete d, fail("vaCreateContext");
    }
    va::ImageFormat nv12{};
    nv12.fourcc = va::FOURCC_NV12, nv12.byte_order = 1, nv12.bits_per_pixel = 12;  // VA_LSB_FIRST
    if (a.CreateImage(d->dpy, &nv12, (int)w, (int)h, &d->img) != 0 || d->img.format.fourcc != va::FOURCC_NV12)
        return delete d, fail("vaCreateImage NV12");
    d->have_img = true;
    if (g_trust == 0) {  // the self-check decoder (single stream at a time: the first opened stream checks)
        vpx_codec_dec_cfg_t c{};
        c.threads = 8;
        d->vpx_on = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &c, 0) == VPX_CODEC_OK;
        if (d->vpx_on) vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
    }
    return d;
}

static bool va_read(MfVp9& d, int si, std::vector<uint8_t>& nv12, uint32_t* w, uint32_t* h) {
    const va::Api& a = va::api();
    void* m = nullptr;
    if (a.SyncSurface(d.dpy, d.surf[si]) != 0 || a.GetImage(d.dpy, d.surf[si], 0, 0, d.w, d.h, d.img.image_id) != 0 ||
        a.MapBuffer(d.dpy, d.img.buf, &m) != 0)
        return false;
    const uint32_t ow = d.w & ~1u, oh = d.h & ~1u;
    nv12.resize((size_t)ow * oh * 3 / 2);
    const uint8_t* s = (const uint8_t*)m;
    for (uint32_t y = 0; y < oh; y++) memcpy(&nv12[(size_t)y * ow], s + d.img.offsets[0] + (size_t)y * d.img.pitches[0], ow);
    for (uint32_t y = 0; y < oh / 2; y++)
        memcpy(&nv12[(size_t)ow * oh + (size_t)y * ow], s + d.img.offsets[1] + (size_t)y * d.img.pitches[1], ow);
    a.UnmapBuffer(d.dpy, d.img.buf);
    *w = ow, *h = oh;
    return true;
}
// one frame (not a superframe) to a surface; -1: not decodable here (size change, profile, driver error)
static int va_frame(MfVp9& d, const uint8_t* p, uint32_t n, bool* shown) {
    const va::Api& a = va::api();
    vp9h::Hdr h;
    vp9h::State s = d.st;
    if (!vp9h::parse(p, n, s, h)) return -1;
    if (h.show_existing) {
        *shown = true;
        return d.slot[h.show_idx];
    }
    if ((uint32_t)h.w != d.w || (uint32_t)h.h != d.h) return -1;  // shortcut: one size per stream (ours never change size)
    int si = -1;
    for (int i = 0; i < kSurfaces && si < 0; i++) {
        bool used = false;
        for (int r : d.slot) used |= r == i;
        if (!used) si = i;
    }
    va::PicVP9 pp{};
    pp.frame_width = (uint16_t)h.w, pp.frame_height = (uint16_t)h.h;
    for (int i = 0; i < 8; i++) pp.reference_frames[i] = d.slot[i] >= 0 ? d.surf[d.slot[i]] : va::INVALID;
    auto& f = pp.pic_fields;
    f.subsampling_x = 1, f.subsampling_y = 1, f.frame_type = !h.key, f.show_frame = h.show, f.error_resilient_mode = h.err_res;
    f.intra_only = h.intra_only, f.allow_high_precision_mv = h.hp, f.mcomp_filter_type = h.interp;
    f.frame_parallel_decoding_mode = h.parallel, f.reset_frame_context = h.reset_ctx, f.refresh_frame_context = h.refresh_ctx;
    f.frame_context_idx = h.ctx_idx, f.segmentation_enabled = h.seg_enabled, f.segmentation_temporal_update = h.seg_temporal;
    f.segmentation_update_map = h.seg_update_map, f.lossless_flag = h.lossless;
    f.last_ref_frame = h.ref_idx[0], f.last_ref_frame_sign_bias = h.sign_bias[0];
    f.golden_ref_frame = h.ref_idx[1], f.golden_ref_frame_sign_bias = h.sign_bias[1];
    f.alt_ref_frame = h.ref_idx[2], f.alt_ref_frame_sign_bias = h.sign_bias[2];
    pp.filter_level = (uint8_t)h.lf_level, pp.sharpness_level = (uint8_t)h.sharp;
    pp.log2_tile_rows = (uint8_t)h.log2_rows, pp.log2_tile_columns = (uint8_t)h.log2_cols;
    pp.frame_header_length_in_bytes = (uint8_t)h.uncomp_size, pp.first_partition_size = (uint16_t)h.comp_size;
    memcpy(pp.mb_segment_tree_probs, s.tree_probs, 7), memcpy(pp.segment_pred_probs, s.pred_probs, 3);
    pp.profile = 0, pp.bit_depth = 8;
    va::SliceVP9 sp{};
    sp.slice_data_size = n;  // the whole frame (both headers + tiles), flag 0 = VA_SLICE_DATA_FLAG_ALL
    for (int i = 0; i < 8; i++) {
        auto& g = sp.seg_param[i];
        int16_t q[4];
        vp9h::seg_values(s, h, i, g.filter_level, q);
        g.luma_dc_quant_scale = q[0], g.luma_ac_quant_scale = q[1], g.chroma_dc_quant_scale = q[2], g.chroma_ac_quant_scale = q[3];
        g.segment_flags.reference_enabled = vp9h::seg_on(s, h, i, 2);
        g.segment_flags.reference = (uint16_t)s.seg_data[i][2] & 3;
        g.segment_flags.reference_skipped = vp9h::seg_on(s, h, i, 3);
    }
    va::Id buf[3] = {va::INVALID, va::INVALID, va::INVALID};
    bool ok = a.CreateBuffer(d.dpy, d.ctx, va::BUF_PIC, sizeof pp, 1, &pp, &buf[0]) == 0 &&
              a.CreateBuffer(d.dpy, d.ctx, va::BUF_SLICE_PARAM, sizeof sp, 1, &sp, &buf[1]) == 0 &&
              a.CreateBuffer(d.dpy, d.ctx, va::BUF_SLICE_DATA, n, 1, (void*)p, &buf[2]) == 0 &&
              a.BeginPicture(d.dpy, d.ctx, d.surf[si]) == 0;
    if (ok) ok = a.RenderPicture(d.dpy, d.ctx, buf, 3) == 0, ok = a.EndPicture(d.dpy, d.ctx) == 0 && ok;
    for (va::Id b : buf) if (b != va::INVALID) a.DestroyBuffer(d.dpy, b);
    if (!ok) return -1;
    d.st = s;
    for (int i = 0; i < 8; i++)
        if (h.refresh >> i & 1) d.slot[i] = si, d.st.ref_w[i] = (uint16_t)h.w, d.st.ref_h[i] = (uint16_t)h.h;
    *shown = h.show;
    return si;
}

bool mfvp9_decode(MfVp9* d, const uint8_t* p, uint32_t n, std::vector<uint8_t>& nv12, uint32_t* w, uint32_t* h) {
    if (d->use_vpx) return d->vpx_on && vpx_frame(*d, p, n, nv12, w, h);
    uint32_t off[8], len[8];
    const int frames = vp9h::frames_of(p, n, off, len);
    int show = -1;
    bool broken = frames == 0;
    for (int i = 0; i < frames && !broken; i++) {
        bool shown = false;
        const int si = va_frame(*d, p + off[i], len[i], &shown);
        broken = si < 0;
        if (shown && si >= 0) show = si;
    }
    bool got = !broken && show >= 0 && va_read(*d, show, nv12, w, h);
    if (d->vpx_on) {  // self-check
        std::vector<uint8_t> ref;
        uint32_t rw = 0, rh = 0;
        const bool rgot = vpx_frame(*d, p, n, ref, &rw, &rh);
        if (broken || (rgot && (!got || rw != *w || rh != *h || ref != nv12))) {
            size_t diff = 0;
            for (size_t i = 0; got && i < ref.size() && i < nv12.size(); i++) diff += ref[i] != nv12[i];
            VLOG("movie: VA-API VP9 %s at shown frame %d (%zu bytes differ): libvpx from now on", broken ? "failed" : "differs from libvpx",
                 d->checked, diff);
            g_trust = -1, d->use_vpx = true;
            if (rgot) nv12.swap(ref), *w = rw, *h = rh;
            return rgot;
        }
        if (rgot && ++d->checked >= kCheck) {
            VLOG("movie: VA-API VP9 matches libvpx (%d frames)", kCheck);
            g_trust = 1, vpx_codec_destroy(&d->vpx), d->vpx_on = false;
        }
        return got;
    }
    if (broken) {  // after the check (a stream this decoder cannot follow): libvpx, from the next keyframe on
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) VLOG("movie: VA-API VP9 cannot decode this stream: libvpx");
        vpx_codec_dec_cfg_t c{};
        c.threads = 8;
        d->use_vpx = true, d->vpx_on = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &c, 0) == VPX_CODEC_OK;
        if (d->vpx_on) vpx_codec_control(&d->vpx, VP9D_SET_ROW_MT, 1);
        return d->vpx_on && vpx_frame(*d, p, n, nv12, w, h);
    }
    return got;
}

void mfvp9_close(MfVp9* d) { delete d; }
#endif  // __linux__

#ifdef VAVP9_TEST
// Windows check of the header parser against libvpx's own decoder state (libvpx built from source, internal headers):
// clang++ -std=c++20 -O1 -DVAVP9_TEST -I<libvpx> -I<vpx-gen> movie_va.cpp <vpx.lib> -o vatest.exe; vatest <file.ivf> [frames]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "vp9/vp9_dx_iface.h"
#include "vp9/decoder/vp9_decoder.h"
#include "vpx/vp8dx.h"
int main(int argc, char** argv) {
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    vpx_codec_ctx_t c{};
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 1;
    if (vpx_codec_dec_init(&c, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return 2;
    vp9h::State st;
    const int limit = argc > 2 ? atoi(argv[2]) : 1 << 30;
    int frames = 0, bad = 0, segs = 0, lfd = 0, inter = 0, existing = 0, superframes = 0;
    for (size_t pos = 32; pos + 12 <= f.size() && frames < limit;) {
        uint32_t n;
        memcpy(&n, &f[pos], 4);
        const uint8_t* p = &f[pos + 12];
        pos += 12 + n;
        uint32_t off[8], len[8];
        const int k = vp9h::frames_of(p, n, off, len);
        superframes += k > 1;
        for (int i = 0; i < k; i++, frames++) {
            vp9h::Hdr h;
            const bool ok = vp9h::parse(p + off[i], len[i], st, h);
            if (vpx_codec_decode(&c, p + off[i], len[i], nullptr, 0) != VPX_CODEC_OK) { printf("libvpx error at %d\n", frames); return 3; }
            for (vpx_codec_iter_t it = nullptr; vpx_codec_get_frame(&c, &it);) {}
            const VP9_COMMON& cm = ((vpx_codec_alg_priv_t*)c.priv)->pbi->common;  // made by the first decode
            auto chk = [&](const char* what, long a, long b) {
                if (a != b && bad++ < 20) printf("frame %d: %s ours %ld libvpx %ld\n", frames, what, a, b);
            };
            chk("parse", ok, 1);
            if (!ok) continue;
            existing += h.show_existing;
            if (h.show_existing) continue;
            inter += !h.key && !h.intra_only;
            segs += h.seg_enabled, lfd += h.lf_delta_enabled;
            chk("key", h.key, cm.frame_type == KEY_FRAME), chk("show", h.show, cm.show_frame);
            chk("intra_only", h.intra_only, cm.intra_only), chk("err_res", h.err_res, cm.error_resilient_mode);
            chk("w", h.w, cm.width), chk("h", h.h, cm.height);
            chk("refresh_ctx", h.refresh_ctx, cm.refresh_frame_context), chk("parallel", h.parallel, cm.frame_parallel_decoding_mode);
            chk("ctx_idx", h.ctx_idx, cm.frame_context_idx);
            if (!h.key && !h.intra_only) {
                chk("interp", h.interp, cm.interp_filter), chk("hp", h.hp, cm.allow_high_precision_mv);
                for (int r = 0; r < 3; r++) chk("sign_bias", h.sign_bias[r], cm.ref_frame_sign_bias[r + 1]);
            }
            chk("lf", h.lf_level, cm.lf.filter_level), chk("sharp", h.sharp, cm.lf.sharpness_level);
            chk("base_q", h.base_q, cm.base_qindex), chk("dq_ydc", h.dq_ydc, cm.y_dc_delta_q);
            chk("dq_uvdc", h.dq_uvdc, cm.uv_dc_delta_q), chk("dq_uvac", h.dq_uvac, cm.uv_ac_delta_q);
            chk("seg", h.seg_enabled, cm.seg.enabled), chk("log2_cols", h.log2_cols, cm.log2_tile_cols);
            chk("log2_rows", h.log2_rows, cm.log2_tile_rows);
            if (h.seg_enabled) {
                chk("seg_update_map", h.seg_update_map, cm.seg.update_map);
                for (int t = 0; t < 7; t++) chk("tree_probs", st.tree_probs[t], cm.seg.tree_probs[t]);
                if (h.seg_update_map)
                    for (int t = 0; t < 3; t++) chk("pred_probs", st.pred_probs[t], cm.seg.pred_probs[t]);
            }
            for (int s = 0; s < (h.seg_enabled ? 8 : 1); s++) {
                uint8_t lvl[4][2];
                int16_t q[4];
                vp9h::seg_values(st, h, s, lvl, q);
                chk("q y dc", q[0], cm.y_dequant[s][0]), chk("q y ac", q[1], cm.y_dequant[s][1]);
                chk("q uv dc", q[2], cm.uv_dequant[s][0]), chk("q uv ac", q[3], cm.uv_dequant[s][1]);
                if (h.lf_level) {
                    chk("lvl intra", lvl[0][0], cm.lf_info.lvl[s][0][0]);
                    for (int r = 1; r < 4; r++)
                        for (int m = 0; m < 2; m++) chk("lvl", lvl[r][m], cm.lf_info.lvl[s][r][m]);
                }
            }
            // what the driver gets told: header sizes must add up inside the frame
            chk("sizes fit", h.uncomp_size + h.comp_size <= len[i], 1);
            for (int r = 0; r < 8; r++)
                if (h.refresh >> r & 1) st.ref_w[r] = (uint16_t)h.w, st.ref_h[r] = (uint16_t)h.h;
        }
    }
    printf("%d frames (%d inter, %d show-existing, %d superframes, %d with segmentation, %d with lf deltas): %d mismatches\n",
           frames, inter, existing, superframes, segs, lfd, bad);
    vpx_codec_destroy(&c);
    return bad != 0;
}
#endif
#endif  // Linux or a test build
