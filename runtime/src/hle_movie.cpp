// movie:: (NVIDIA multimedia, subsdk1) as used by the game's movie player: a MediaCodec-style VP9 decoder.
// subsdk1 is not loaded; the Decoder object gets a vtable of host callbacks in the order of _ZTVN5movie7DecoderE
// (read from subsdk1's relocations): D2, D0, Configure, RegisterClientEvents, GetOutputFormat, Start, Stop, Reset,
// Flush, AcquireInputBufferIndex, GetInputBuffer, SendInputBufferForDecode, AcquireOutputBufferIndex,
// GetOutputBuffer, GetOutputBufferSize, ReleaseOutputBufferIndex, GetLastError, GetOutputBuffer(MediaData),
// GetOutputBufferFormat.
// The game's player (0x6df5a4) waits on the four events of DecoderEvents; event 0 = input buffers available
// (measured: it then calls AcquireInputBufferIndex), event 1 = output available (assumed).
// Frames are decoded with libvpx on the thread that sends the input. Output format 4 (the game's) is tight NV12,
// as subsdk1's DecoderImpl::GetOutputBuffer writes it: Y rows of `width` bytes, then height/2 rows of interleaved
// UV, copied INTO the movie::Buffer the game passes (sized by GetOutputBufferSize = w*h*3/2).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "movie_mf.h"
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "rt.h"
#include "movie_mf.h"

using namespace rt;

namespace rt {
uintptr_t host_callback_tag();
uint32_t host_callback(HleFn fn, uintptr_t tag);
}

static const char* kMethods[] = {"D2", "D0", "Configure", "RegisterClientEvents", "GetOutputFormat", "Start", "Stop",
                                 "Reset", "Flush", "AcquireInputBufferIndex", "GetInputBuffer",
                                 "SendInputBufferForDecode", "AcquireOutputBufferIndex", "GetOutputBuffer",
                                 "GetOutputBufferSize", "ReleaseOutputBufferIndex", "GetLastError",
                                 "GetOutputBufferMD", "GetOutputBufferFormat"};
constexpr int NMETHODS = sizeof(kMethods) / sizeof(kMethods[0]);
constexpr int NIN = 4, NOUT = 8;
constexpr uint32_t IN_SIZE = 4u << 20, EOS = 4;  // MediaCodec BUFFER_FLAG_END_OF_STREAM

struct Frame { int idx; int64_t pts; uint32_t flags; };
struct Vpx {
    vpx_codec_ctx_t ctx{};
    bool ok = false;
    int frames = 0;
    ~Vpx() { if (ok) vpx_codec_destroy(&ctx); }
};
struct Dec {
    uint32_t width = 1280, height = 720;
    uint32_t events[4] = {};
    uint32_t in_buf[NIN] = {};
    std::vector<uint8_t> out[NOUT];  // tight NV12 frames
    bool in_free[NIN], out_free[NOUT];
    std::deque<Frame> ready;  // decoded, waiting for AcquireOutputBufferIndex
    bool started = false;
    std::shared_ptr<Vpx> vpx;  // shared: the sending thread decodes without holding g_mv_lock
    std::shared_ptr<struct Hd> hd;  // a 4K replacement stream playing along (below)
    uint64_t prev_hash = 0;         // movie_packet_hash of the previous packet (two-packet keys, below)
    Dec() { for (bool& b : in_free) b = true; for (bool& b : out_free) b = true; }
};

static uint32_t frame_size(const Dec& d) { return d.width * d.height + (d.width / 2) * (d.height / 2) * 2; }

// I420 image -> tight NV12 of w x h (the configured size; a differently sized stream is cropped / padded black)
static void to_nv12(const vpx_image_t* img, uint32_t w, uint32_t h, std::vector<uint8_t>& o) {
    o.assign((size_t)w * h, 0);
    o.resize((size_t)w * h + (w / 2) * (h / 2) * 2, 128);
    uint32_t cw = std::min(w, img->d_w), ch = std::min(h, img->d_h);
    for (uint32_t y = 0; y < ch; y++) memcpy(&o[(size_t)y * w], img->planes[0] + (size_t)y * img->stride[0], cw);
    uint8_t* uv = &o[(size_t)w * h];
    for (uint32_t y = 0; y < ch / 2; y++) {
        const uint8_t* u = img->planes[1] + (size_t)y * img->stride[1];
        const uint8_t* v = img->planes[2] + (size_t)y * img->stride[2];
        uint8_t* row = uv + (size_t)y * (w / 2) * 2;
        for (uint32_t x = 0; x < cw / 2; x++) { row[2 * x] = u[x]; row[2 * x + 1] = v[x]; }
    }
}

// ------------------------------------------------------------------------------------- 4K replacement movies
// tools/camphd (user decision 11-02: camp menu backgrounds AI-upscaled offline) writes <data>/movie_hd/**/<name>_<hash>.ivf,
// a VP9 stream with the same frames at 3x size; <hash> = movie_packet_hash of the original's first packet. The decoder
// steps it along: each visible frame also decodes the HD frame of the same index and registers both NV12 planes by
// fingerprint (rt::movie_fp of the low-res bytes); vk_texture.cpp uploads the 4K plane in place of a linear 1280x720 /
// 640x360 texture whose bytes match (rt::movie_hd_plane). NX_MOVIE_HD=0 turns it off.
struct Hd {
    std::shared_ptr<const std::vector<uint8_t>> ivf;
    size_t pos = 32;  // next IVF frame record
    Vpx vpx;
    MfVp9* mf = nullptr;  // GPU decoder (movie_mf.cpp, s117): used instead of libvpx when it opened
    ~Hd() { if (mf) mfvp9_close(mf); }
    bool on() const { return mf || vpx.ok; }  // some decoder opened
    double ms = 0;     // 4K decode time of the last `timed` frames
    int timed = 0;
    bool slow = false;  // PORTABILITY: a CPU that cannot decode 4K VP9 in time plays this movie at 720p
};
// true once this stream's 4K decode averaged over 30 ms per frame (30 frames; 33 ms = one movie frame): a weaker CPU than the movie's 30 fps
// needs; the 720p frames are shown from then on (the texture swap finds no 4K plane)
static bool hd_slow(Hd& hd, std::chrono::steady_clock::time_point t0) {
    hd.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++hd.timed < 30) return false;
    static const double limit = getenv("NX_MOVIE_HD_SLOW_MS") ? atof(getenv("NX_MOVIE_HD_SLOW_MS")) : 30;  // test: a lower limit
    // setting movie_hd_fallback = 0 (GitHub #13): keep 4K even when the decode falls behind (a short stutter on a fast PC
    // switched whole movies to 720p)
    static const bool fallback = !getenv("NX_MOVIE_HD_FALLBACK") || atoi(getenv("NX_MOVIE_HD_FALLBACK")) != 0;
    if (!fallback) {
        if (hd.ms / hd.timed > limit) log("movie: 4K decode %.1f ms per frame (movie_hd_fallback = 0: stays 4K)", hd.ms / hd.timed);
        hd.ms = 0, hd.timed = 0;
        return false;
    }
    if (hd.ms / hd.timed > limit && !hd.slow) log("movie: 4K decode %.1f ms per frame: too slow on this CPU, 720p for this movie", hd.ms / hd.timed), hd.slow = true;
    hd.ms = 0, hd.timed = 0;
    return hd.slow;
}
// FNV-1a 64 of the first 4096 bytes. Story movies share black first packets (m13 / m14 / m3083_01 / m0000_11, m07 /
// m4103_01, ...; packet 1 always differs): their files are keyed by the hash of packet 1 seeded with packet 0's hash
// (tools/camphd), looked up at packet 1 (the HD stream skips its frame 0, a black frame shown at 720p). Single-packet
// keys (camp movies) still work.
static uint64_t movie_packet_hash(const uint8_t* p, uint32_t n, uint64_t h = 1469598103934665603ull) {
    for (uint32_t i = 0; i < std::min<uint32_t>(n, 4096); i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static std::map<uint64_t, std::filesystem::path>& hd_index() {
    static std::map<uint64_t, std::filesystem::path> m = [] {
        std::map<uint64_t, std::filesystem::path> r;
        if (getenv("NX_MOVIE_HD") && !atoi(getenv("NX_MOVIE_HD"))) return r;
        std::error_code ec;
        for (auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::path(g_data_dir) / "movie_hd", ec)) {
            std::string stem = e.path().stem().string();
            if (e.path().extension() != ".ivf" || stem.size() < 17 || stem[stem.size() - 17] != '_') continue;
            r[strtoull(stem.c_str() + stem.size() - 16, nullptr, 16)] = e.path();
        }
        if (!r.empty()) log("movie: %zu 4K replacement movies", r.size());
        return r;
    }();
    return m;
}
// the GPU decoder when this PC has one (setting movie_gpu = 0: always libvpx), else libvpx; Hd::on() = some decoder
static void hd_decoder_open(Hd& hd) {
    static const bool gpu = !getenv("NX_MOVIE_GPU") || atoi(getenv("NX_MOVIE_GPU")) != 0;
    if (hd.ivf->size() <= 32) return;
    uint16_t w, h;
    memcpy(&w, &(*hd.ivf)[12], 2), memcpy(&h, &(*hd.ivf)[14], 2);
    if (gpu && (hd.mf = mfvp9_open(w, h))) return;
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 8;
    hd.vpx.ok = vpx_codec_dec_init(&hd.vpx.ctx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (hd.vpx.ok) vpx_codec_control(&hd.vpx.ctx, VP9D_SET_ROW_MT, 1);  // 4K streams have 4 tile columns: rows in parallel too
}
// one packet -> the frame as tight NV12 (GPU) or I420 image (libvpx); false: no frame
static bool hd_decode(Hd& hd, const uint8_t* p, uint32_t n, std::vector<uint8_t>& nv12, const vpx_image_t** img, uint32_t* w, uint32_t* h) {
    *img = nullptr;
    if (hd.mf) return mfvp9_decode(hd.mf, p, n, nv12, w, h);
    if (vpx_codec_decode(&hd.vpx.ctx, p, n, nullptr, 0) != VPX_CODEC_OK) return false;
    vpx_codec_iter_t it = nullptr;
    while (const vpx_image_t* i = vpx_codec_get_frame(&hd.vpx.ctx, &it)) *img = i;
    if (*img) *w = (*img)->d_w & ~1u, *h = (*img)->d_h & ~1u;
    return *img != nullptr;
}
static std::shared_ptr<Hd> hd_open(uint64_t hash) {
    auto it = hd_index().find(hash);
    if (it == hd_index().end()) return nullptr;
    static std::map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> files;  // read once (~27 MB each)
    static std::mutex fl;
    auto hd = std::make_shared<Hd>();
    {
        std::lock_guard<std::mutex> l(fl);
        auto& f = files[hash];
        if (!f) {
            std::ifstream in(it->second, std::ios::binary);
            f = std::make_shared<const std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        hd->ivf = f;
    }
    hd_decoder_open(*hd);
    log("movie: 4K stream %s %s", it->second.filename().string().c_str(), hd->on() ? (hd->mf ? "playing (GPU)" : "playing") : "FAILED");
    return hd->on() ? hd : nullptr;
}
// the next HD frame as tight NV12 (null at the end of the stream or on an error)
static std::shared_ptr<std::vector<uint8_t>> hd_next(Hd& hd, uint32_t* w, uint32_t* h) {
    const auto& b = *hd.ivf;
    if (hd.slow || hd.pos + 12 > b.size()) return nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t n;
    memcpy(&n, &b[hd.pos], 4);
    const uint8_t* p = &b[hd.pos + 12];
    hd.pos += 12 + (size_t)n;
    auto o = std::make_shared<std::vector<uint8_t>>();
    const vpx_image_t* img;
    if (hd.pos > b.size() || !hd_decode(hd, p, n, *o, &img, w, h)) return nullptr;
    if (img) to_nv12(img, *w, *h, *o);
    return hd_slow(hd, t0) ? nullptr : o;
}
struct HdPlane { uint64_t fp; std::shared_ptr<std::vector<uint8_t>> nv12; size_t off; uint32_t w, h; };
static std::mutex g_hd_lock;
static std::deque<HdPlane> g_hd_planes;  // the last few frames' planes
static std::atomic<bool> g_hd_any{false};
static void hd_register(const std::vector<uint8_t>& lo, uint32_t w, uint32_t h, std::shared_ptr<std::vector<uint8_t>> hi, uint32_t hw, uint32_t hh) {
    HdPlane y{movie_fp(lo.data(), w, h, w), hi, 0, hw, hh};
    HdPlane uv{movie_fp(lo.data() + (size_t)w * h, w, h / 2, w), hi, (size_t)hw * hh, hw / 2, hh / 2};
    static const bool check = getenv("NX_MOVIE_HD_CHECK") != nullptr;  // debug: is the 4K stream on the same frame?
    if (check && hw >= w && hh >= h) {
        static uint64_t sum = 0, sum_prev = 0, n = 0, frames = 0;
        static std::shared_ptr<std::vector<uint8_t>> prev;  // the previous 4K frame: an offset stream matches it better
        for (uint32_t y = 0; y < h; y += 8)
            for (uint32_t x = 0; x < w; x += 8) {
                const size_t o = (size_t)(y * hh / h) * hw + x * hw / w;
                sum += abs(lo[(size_t)y * w + x] - (*hi)[o]), n++;
                if (prev && prev->size() == hi->size()) sum_prev += abs(lo[(size_t)y * w + x] - (*prev)[o]);
            }
        prev = hi;
        if (++frames % 60 == 0)
            log("movie: 4K check: mean |720p - 4K| luma %.2f (previous 4K frame %.2f) over 60 frames", (double)sum / n, (double)sum_prev / n), sum = sum_prev = n = 0;
    }
    std::lock_guard<std::mutex> l(g_hd_lock);
    g_hd_planes.push_back(y), g_hd_planes.push_back(uv);
    while (g_hd_planes.size() > 16) g_hd_planes.pop_front();  // ponytail: 8 frames kept; the game shows one within 2-3
    g_hd_any = true;
}

static std::mutex g_mv_lock;
static std::unordered_map<uint32_t, Dec> g_dec;
// Input buffers of destroyed decoders, reused by the next one. hle_alloc never frees: without this every movie
// (each camp menu open) took 16 MB and the 16th one ran past the HLE region (crash 2026-11-13 entering Shibuya).
static std::vector<uint32_t> g_in_pool;
static std::map<std::string, int> g_calls;
static std::unordered_map<uint32_t, std::map<std::string, int32_t>> g_md;

static void signal(Cpu* c, uint32_t ev) {
    if (ev) gcall(c, "_ZN2nn2os11SignalEventEPNS0_9EventTypeE", {ev});
}
static void set_buffer(uint32_t b, uint32_t base, uint32_t cap, uint32_t size) {  // our movie::Buffer layout
    st32(b, base);
    st32(b + 4, cap);
    st32(b + 8, 0);
    st32(b + 12, size);
}

static void method(Cpu* c) {
    int m = (int)host_callback_tag();
    uint32_t self = c->r[0];
    int n;
    {
        std::lock_guard<std::mutex> l(g_mv_lock);
        n = ++g_calls[kMethods[m]];
    }
    if (n <= 3) LOGV("movie::Decoder::%s(%08x, %08x, %08x, %08x)", kMethods[m], c->r[0], c->r[1], c->r[2], c->r[3]);
    std::unique_lock<std::mutex> l(g_mv_lock);
    Dec& d = g_dec[self];
    switch (m) {
        case 0: case 1:  // destructors. ponytail: D0 should also operator-delete the object; leaking ~16 bytes per movie
            for (uint32_t b : d.in_buf) if (b) g_in_pool.push_back(b);
            g_dec.erase(self);
            c->r[0] = self;
            return;
        case 2: {  // Configure(MediaData*)
            auto& md = g_md[c->r[1]];
            if (md.count("width")) d.width = md["width"];
            if (md.count("height")) d.height = md["height"];
            for (int i = 0; i < NIN; i++)
                if (!d.in_buf[i]) {
                    if (g_in_pool.empty()) d.in_buf[i] = hle_alloc(IN_SIZE, 64);
                    else { d.in_buf[i] = g_in_pool.back(); g_in_pool.pop_back(); }
                }
            if (!d.vpx) {
                d.vpx = std::make_shared<Vpx>();
                vpx_codec_dec_cfg_t cfg{};
                cfg.threads = 4;
                cfg.w = d.width;
                cfg.h = d.height;
                d.vpx->ok = vpx_codec_dec_init(&d.vpx->ctx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
                log("movie: VP9 %ux%u, libvpx %s", d.width, d.height, d.vpx->ok ? vpx_codec_version_str() : "init FAILED");
            }
            c->r[0] = 0;
            return;
        }
        case 3:  // RegisterClientEvents(DecoderEvents*: four nn::os::EventType*)
            for (int i = 0; i < 4; i++) d.events[i] = ld32(c->r[1] + 4 * i);
            c->r[0] = 0;
            return;
        case 4: {  // GetOutputFormat(MediaData*)
            auto& md = g_md[c->r[1]];
            md["width"] = d.width; md["height"] = d.height; md["stride"] = d.width; md["slice-height"] = d.height;
            md["color-format"] = 0x15;  // NV12-style semi-planar (assumed)
            c->r[0] = 0;
            return;
        }
        case 5: {  // Start
            d.started = true;
            uint32_t ev = d.events[0];
            l.unlock();
            signal(c, ev);
            c->r[0] = 0;
            return;
        }
        case 6: case 7: case 8:  // Stop / Reset / Flush
            d.ready.clear();
            for (bool& b : d.in_free) b = true;
            for (bool& b : d.out_free) b = true;
            c->r[0] = 0;
            return;
        case 9: {  // AcquireInputBufferIndex(int* index, int* remaining)
            int idx = -1, rem = 0;
            for (int i = 0; i < NIN; i++)
                if (d.in_free[i]) { if (idx < 0) idx = i; else rem++; }
            if (idx < 0) { c->r[0] = 1; return; }
            d.in_free[idx] = false;
            st32(c->r[1], (uint32_t)idx);
            if (c->r[2]) st32(c->r[2], (uint32_t)rem);
            c->r[0] = 0;
            return;
        }
        case 10: {  // GetInputBuffer(index, Buffer*)
            int i = (int)c->r[1];
            if (i < 0 || i >= NIN) { c->r[0] = 1; return; }
            set_buffer(c->r[2], d.in_buf[i], IN_SIZE, 0);
            c->r[0] = 0;
            return;
        }
        case 11: {  // SendInputBufferForDecode(index, offset, size, int64 pts [sp], u32 flags [sp+8])
            int i = (int)c->r[1];
            int64_t pts = (int64_t)ld64(c->r[13]);
            uint32_t flags = ld32(c->r[13] + 8);            uint32_t src = (i >= 0 && i < NIN) ? d.in_buf[i] + c->r[2] : 0, size = c->r[3];
            auto vpx = d.vpx;
            auto hd = d.hd;
            uint32_t w = d.width, h = d.height;
            const uint64_t prev = d.prev_hash;
            l.unlock();
            if (src && size && !hd_index().empty()) {  // the first packet(s) of a replaced movie (again when it loops)
                const uint64_t h1 = movie_packet_hash((const uint8_t*)GPTR(src), size);
                auto n = hd_open(h1);
                if (!n && prev && (n = hd_open(movie_packet_hash((const uint8_t*)GPTR(src), size, prev)))) {
                    uint32_t sw, sh;
                    hd_next(*n, &sw, &sh);  // its frame 0 went by at 720p
                }
                std::lock_guard<std::mutex> g(g_mv_lock);
                if (auto di = g_dec.find(self); di != g_dec.end()) {
                    di->second.prev_hash = h1;
                    if (n) hd = n, di->second.hd = n;
                }
            }
            std::vector<std::vector<uint8_t>> pics;  // visible frames of this packet (0 or 1; superframes hide the rest)
            if (vpx && vpx->ok && src && size) {
                if (vpx_codec_decode(&vpx->ctx, (const uint8_t*)GPTR(src), size, nullptr, 0) != VPX_CODEC_OK) {
                    static int errs;
                    if (errs++ < 5) log("movie: VP9 decode error: %s", vpx_codec_error_detail(&vpx->ctx) ?: vpx_codec_error(&vpx->ctx));
                }
                vpx_codec_iter_t it = nullptr;
                while (const vpx_image_t* img = vpx_codec_get_frame(&vpx->ctx, &it)) {
                    to_nv12(img, w, h, pics.emplace_back());
                    uint32_t hw, hh;
                    if (hd)
                        if (auto hi = hd_next(*hd, &hw, &hh)) hd_register(pics.back(), w, h, std::move(hi), hw, hh);
                    if (!vpx->frames++)
                        log("movie: first frame fmt %#x %ux%u depth %u range %d cs %d", img->fmt, img->d_w, img->d_h,
                            img->bit_depth, img->range, img->cs);
                    if (const char* dump = getenv("NX_DUMP_MOVIE"); dump && vpx->frames == 150) {
                        FILE* f = fopen(dump, "wb");  // frame 150 as tight NV12, w x h
                        if (f) { fwrite(pics.back().data(), 1, pics.back().size(), f); fclose(f); }
                    }
                }
            }
            if ((flags & EOS) && pics.empty()) pics.emplace_back();  // the end-of-stream marker still needs an output
            l.lock();
            auto di = g_dec.find(self);
            if (di == g_dec.end()) return;  // destroyed meanwhile
            Dec& e = di->second;  // `d` may dangle after the unlock
            if (i >= 0 && i < NIN) e.in_free[i] = true;
            for (size_t k = 0; k < pics.size(); k++) {
                int o = -1;
                for (int j = 0; j < NOUT; j++) if (e.out_free[j]) { o = j; break; }
                if (o < 0) {  // the game holds every output buffer: drop this frame
                    static int drops;
                    if (drops++ < 5) log("movie: output buffers full, frame dropped");
                    continue;
                }
                e.out_free[o] = false;
                e.out[o] = std::move(pics[k]);
                e.ready.push_back({o, pts, k + 1 == pics.size() ? (flags & EOS) : 0});
            }
            uint32_t ein = e.events[0], eout = e.events[1];
            l.unlock();
            signal(c, ein);
            signal(c, eout);
            c->r[0] = 0;
            return;
        }
        case 12: {  // AcquireOutputBufferIndex(int* index, int64* pts, u32* flags, int* remaining)
            if (d.ready.empty()) { c->r[0] = 1; return; }
            Frame f = d.ready.front();
            d.ready.pop_front();
            st32(c->r[1], (uint32_t)f.idx);
            if (c->r[2]) st64(c->r[2], (uint64_t)f.pts);
            if (c->r[3]) st32(c->r[3], f.flags);
            uint32_t rem = arg(c, 4);
            if (rem) st32(rem, (uint32_t)d.ready.size());
            c->r[0] = 0;
            return;
        }
        case 13: case 17: {  // GetOutputBuffer(index, Buffer* [, MediaData*]): copy into the caller's buffer at its offset
            int i = (int)c->r[1];
            uint32_t b = c->r[2];
            if (i < 0 || i >= NOUT) { c->r[0] = 1; return; }
            uint32_t base = ld32(b), cap = ld32(b + 4), off = ld32(b + 8);
            const auto& f = d.out[i];
            uint32_t n = off <= cap ? std::min<uint32_t>((uint32_t)f.size(), cap - off) : 0;
            if (n) memcpy(GPTR(base + off), f.data(), n);
            st32(b + 12, n);
            c->r[0] = 0;
            return;
        }
        case 14: {  // GetOutputBufferSize(int* size, int* alignment) (subsdk1 DecoderImpl: format 4 -> w*h + 2*(w/2)*(h/2), 1)
            if (c->r[1]) st32(c->r[1], frame_size(d));
            if (c->r[2]) st32(c->r[2], 1);
            c->r[0] = 0;
            return;
        }
        case 15: {  // ReleaseOutputBufferIndex(index)
            int i = (int)c->r[1];
            if (i >= 0 && i < NOUT) d.out_free[i] = true;
            c->r[0] = 0;
            return;
        }
        case 16: c->r[0] = 0; return;  // GetLastError
        case 18: {                      // GetOutputBufferFormat(index, MediaData*)
            auto& md = g_md[c->r[2]];
            md["width"] = d.width; md["height"] = d.height; md["stride"] = d.width; md["slice-height"] = d.height;
            c->r[0] = 0;
            return;
        }
        default: c->r[0] = 0; return;
    }
}

static uint32_t vtable() {
    static uint32_t vt = [] {
        uint32_t v = hle_alloc(4 * (NMETHODS + 2), 8);
        st32(v, 0);
        st32(v + 4, 0);
        for (int i = 0; i < NMETHODS; i++) st32(v + 8 + 4 * i, host_callback(method, (uintptr_t)i));
        return v + 8;  // the object's vptr points past offset-to-top and typeinfo
    }();
    return vt;
}

HLE_FN(mv_DecoderCtor, "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi") {
    log("movie::Decoder(%08x, type %u, format %u, mode %u, %d)", c->r[0], c->r[1], c->r[2], c->r[3], (int)arg(c, 4));
    if (c->r[2] != 4) log("movie: output format %u is not NV12 (4); frames will be wrong", c->r[2]);
    st32(c->r[0], vtable());
    st32(c->r[0] + 4, 0);
    std::lock_guard<std::mutex> l(g_mv_lock);
    g_dec[c->r[0]] = Dec{};
}
HLE_FN(mv_SetAlloc, "_ZN5movie12SetAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_") {}
HLE_FN(mv_SetPrio, "_ZN5movie17SetThreadPriorityEi") {}
HLE_FN(mv_GetPrio, "_ZN5movie17GetThreadPriorityEv") { c->r[0] = 16; }
HLE_FN(mv_SetCore, "_ZN5movie11SetCoreMaskEy") {}

// movie::Buffer (our own layout inside the guest object): base @0, capacity @4, offset @8, size @12
HLE_FN(mv_BufCtor, "_ZN5movie6BufferC1Ev") { memset(GPTR(c->r[0]), 0, 16); }
HLE_FN(mv_BufDtor, "_ZN5movie6BufferD1Ev") {}
HLE_FN(mv_BufSet, "_ZN5movie6Buffer18SetDataAndCapacityEPvj") { st32(c->r[0], c->r[1]); st32(c->r[0] + 4, c->r[2]); }
HLE_FN(mv_BufRange, "_ZN5movie6Buffer8SetRangeEii") { st32(c->r[0] + 8, c->r[1]); st32(c->r[0] + 12, c->r[2]); }
HLE_FN(mv_BufBase, "_ZNK5movie6Buffer4BaseEv") { c->r[0] = ld32(c->r[0]); }
HLE_FN(mv_BufCap, "_ZNK5movie6Buffer8CapacityEv") { c->r[0] = ld32(c->r[0] + 4); }

// movie::MediaData: a key/value store; host side, keyed by the guest object
HLE_FN(mv_MdCtor, "_ZN5movie9MediaDataC1Ev") { std::lock_guard<std::mutex> l(g_mv_lock); g_md[c->r[0]].clear(); }
HLE_FN(mv_MdDtor, "_ZN5movie9MediaDataD1Ev") { std::lock_guard<std::mutex> l(g_mv_lock); g_md.erase(c->r[0]); }
HLE_FN(mv_MdClear, "_ZN5movie9MediaData5ClearEv") { std::lock_guard<std::mutex> l(g_mv_lock); g_md[c->r[0]].clear(); }
HLE_FN(mv_MdFindInt, "_ZNK5movie9MediaData9FindInt32EPKcPi") {
    std::string k = gstr(c->r[1]);
    std::lock_guard<std::mutex> l(g_mv_lock);
    auto& m = g_md[c->r[0]];
    auto it = m.find(k);
    LOGV("movie::MediaData::FindInt32(\"%s\") -> %s", k.c_str(), it == m.end() ? "missing" : std::to_string(it->second).c_str());
    if (it == m.end()) { c->r[0] = 0; return; }
    st32(c->r[2], (uint32_t)it->second);
    c->r[0] = 1;
}
HLE_FN(mv_MdFindRect, "_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_") {
    std::string k = gstr(c->r[1]);
    LOGV("movie::MediaData::FindRect(\"%s\")", k.c_str());
    c->r[0] = 0;
}
HLE_FN(mv_Vp9Config, "_ZN5movie33CreateVp9DecoderConfigurationDataEiiiPNS_9MediaDataE") {
    std::lock_guard<std::mutex> l(g_mv_lock);
    auto& m = g_md[c->r[3]];
    m["width"] = (int32_t)c->r[0];
    m["height"] = (int32_t)c->r[1];
    c->r[0] = 1;  // the caller treats nonzero as success
}

namespace rt {
// sampled FNV-1a of a plane (`row` bytes x `rows`, rows `stride` apart): ~2000 bytes, enough to tell movie frames apart
uint64_t movie_fp(const uint8_t* p, uint32_t row, uint32_t rows, uint32_t stride) {
    uint64_t h = 1469598103934665603ull ^ ((uint64_t)row << 32 | rows);
    for (uint32_t y = 0; y < rows; y += 7)
        for (uint32_t x = (y * 13) % 61; x < row; x += 61) h = (h ^ p[(size_t)y * stride + x]) * 1099511628211ull;
    return h;
}
bool movie_hd_any() { return g_hd_any; }
bool movie_hd_plane(uint64_t fp, std::shared_ptr<std::vector<uint8_t>>* nv12, size_t* off, uint32_t* w, uint32_t* h) {
    std::lock_guard<std::mutex> l(g_hd_lock);
    for (auto it = g_hd_planes.rbegin(); it != g_hd_planes.rend(); ++it)
        if (it->fp == fp) { *nv12 = it->nv12, *off = it->off, *w = it->w, *h = it->h; return true; }
    return false;
}
}  // namespace rt

// ------------------------------------------------------------------------------------- 4K Sofdec.Prime (MPEG-1) movies
// The uncensored mod replaces m06 / m07 / m12 with MPEG-1 streams that CRI MPV decodes in game code (no packets reach
// the VP9 path above). Mana's frame copy 0x6dbc5c (r1 = 3 plane descs {ptr, w, h, pitch}: Y, U, V) writes linear R8
// textures. tools/camphd writes movie_hd/**/<name>_prime.ivf (4K VP9) + <name>_prime.thm (16x9 Y block means per
// frame, 144 bytes). Each copied frame is matched by its thumbnail (near the last match, else every prime movie), and
// the HD frame's I420 planes are registered by the fingerprints vk_texture.cpp computes, so a wrong or unknown movie
// just stays 720p. NX_MOVIE_HD=0 turns it off.
namespace {
// The whole 4K stream in memory (m07: 377 MB, 3211 frames) + its keyframes (camphd: every 60 frames). Read once on a
// worker thread (Reddit 11-27: reading it on the game thread at every reopen = 2 s hitches); 720p shows meanwhile.
struct PrimeFile {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::vector<std::pair<int, size_t>> keys;  // (frame number, IVF record offset)
};
struct Prime {
    std::filesystem::path ivf;
    std::vector<uint8_t> thm;
    std::shared_ptr<PrimeFile> file;  // set by the reader thread (g_prime_io)
    bool reading = false;
};
std::mutex g_prime_io;
// VP9 uncompressed header: frame_marker(2) profile_low profile_high [reserved when profile 3] show_existing frame_type
bool vp9_keyframe(uint8_t b) {
    const int profile = (b >> 5 & 1) | (b >> 4 & 1) << 1, i = profile == 3 ? 5 : 4;
    return !(b >> (7 - i) & 1) && !(b >> (6 - i) & 1);
}
std::vector<Prime>& prime_index() {
    static std::vector<Prime> v = [] {
        std::vector<Prime> r;
        if (getenv("NX_MOVIE_HD") && !atoi(getenv("NX_MOVIE_HD"))) return r;
        std::error_code ec;
        for (auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::path(g_data_dir) / "movie_hd", ec)) {
            if (e.path().extension() != ".thm") continue;
            Prime p{std::filesystem::path(e.path()).replace_extension(".ivf"), {}};
            std::ifstream in(e.path(), std::ios::binary);
            p.thm.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (p.thm.size() >= 144 && std::filesystem::exists(p.ivf, ec)) r.push_back(std::move(p));
        }
        if (!r.empty()) log("movie: %zu 4K MPEG-1 replacement movies", r.size());
        return r;
    }();
    return v;
}
struct PrimeState {
    int movie = -1, idx = -1, decoded = -1;  // matched movie / frame, HD frames decoded so far
    double last = 0, prev = 0;
    bool shown = false, waited = false;  // a 4K frame of this movie was shown / the one 100 ms wait was used
    std::unique_ptr<Hd> hd;
    std::shared_ptr<std::vector<uint8_t>> img;  // the decoded HD frame, tight I420
    uint32_t w = 0, h = 0;
};
// mean abs difference of two thumbnails (0-255)
int thm_diff(const uint8_t* a, const uint8_t* b) {
    int s = 0;
    for (int i = 0; i < 144; i++) s += abs(a[i] - b[i]);
    return s / 144;
}
// the movie's stream when it is in memory, else null (and the read starts on a worker thread)
std::shared_ptr<PrimeFile> prime_file(int mv) {
    Prime& p = prime_index()[mv];
    std::lock_guard<std::mutex> l(g_prime_io);
    if (p.file || p.reading) return p.file;
    p.reading = true;
    std::thread([&p] {
        auto f = std::make_shared<PrimeFile>();
        // one block read (istreambuf_iterator went byte by byte: most of the ~1 s of 720p at a movie's start)
        std::ifstream in(p.ivf, std::ios::binary | std::ios::ate);
        auto b = std::make_shared<std::vector<uint8_t>>((size_t)std::max<std::streamoff>(0, in.tellg()));
        in.seekg(0), in.read((char*)b->data(), (std::streamsize)b->size());
        for (size_t pos = 32, n = 0; pos + 13 <= b->size(); n++) {
            uint32_t len;
            memcpy(&len, &(*b)[pos], 4);
            if (vp9_keyframe((*b)[pos + 12])) f->keys.push_back({(int)n, pos});
            pos += 12 + (size_t)len;
        }
        f->bytes = std::move(b);
        log("movie: %s read (%zu MB, %zu keyframes)", p.ivf.filename().string().c_str(), f->bytes->size() >> 20, f->keys.size());
        std::lock_guard<std::mutex> l2(g_prime_io);
        p.file = f, p.reading = false;
    }).detach();
    return nullptr;
}
// decode the HD stream up to frame `target`; false while not there yet (at most 30 frames per call). Jumps go to the
// nearest keyframe at or before the target instead of decoding from frame 0 (Reddit 11-27: a match one frame behind
// reopened the stream and decoded from the start every frame: 150-390 ms per frame, growing through the movie).
bool prime_seek(PrimeState& st, int target, bool burst = false) {
    const auto f = prime_file(st.movie);
    if (!f) return false;
    if (!burst && st.hd && st.img && st.decoded > target && st.decoded - target <= 8) return true;  // a repeated frame: keep the 4K one
    if (!st.hd || st.decoded > target) {
        st.hd = std::make_unique<Hd>(), st.decoded = -1, st.img.reset();
        st.hd->ivf = f->bytes;
        hd_decoder_open(*st.hd);
        log("movie: 4K stream %s %s", prime_index()[st.movie].ivf.filename().string().c_str(), st.hd->on() ? (st.hd->mf ? "playing (GPU)" : "playing") : "FAILED");
    }
    auto k = std::upper_bound(f->keys.begin(), f->keys.end(), std::make_pair(target, SIZE_MAX)) - 1;
    const bool jump = k >= f->keys.begin() && k->first > st.decoded + 1;
    // far past a keyframe (movie start while the file was read, a skip): 720p until the next keyframe (at most 60 frames)
    // rather than a burst of 4K decodes in one game frame (s120: 46 frames = 325 ms)
    if (!burst && target - (jump ? k->first : st.decoded + 1) > 12) return false;  // (the worker below may burst)
    if (jump) st.hd->pos = k->second, st.decoded = k->first - 1;
    for (int n = 0; st.hd->on() && !st.hd->slow && st.decoded < target && n < 30; n++) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto& b = *st.hd->ivf;
        if (st.hd->pos + 12 > b.size()) return false;
        uint32_t len;
        memcpy(&len, &b[st.hd->pos], 4);
        const uint8_t* p = &b[st.hd->pos + 12];
        st.hd->pos += 12 + (size_t)len;
        if (st.hd->pos > b.size()) return false;
        std::vector<uint8_t> nv12;
        const vpx_image_t* img;
        uint32_t w = 0, h = 0;
        const bool got = hd_decode(*st.hd, p, len, nv12, &img, &w, &h);
        if (!got && !st.hd->mf) return false;  // libvpx error (the GPU decoder may hold a frame back: keep going)
        st.decoded++;
        if (!got || st.decoded < target) continue;
        auto o = std::make_shared<std::vector<uint8_t>>((size_t)w * h * 3 / 2);
        if (img) {
            for (uint32_t y = 0; y < h; y++) memcpy(&(*o)[(size_t)y * w], img->planes[0] + (size_t)y * img->stride[0], w);
            for (int k = 1; k < 3; k++)
                for (uint32_t y = 0; y < h / 2; y++)
                    memcpy(&(*o)[(size_t)w * h + (size_t)(k - 1) * (w / 2) * (h / 2) + (size_t)y * (w / 2)], img->planes[k] + (size_t)y * img->stride[k], w / 2);
        } else {  // NV12 -> I420 (this path wants planar chroma)
            memcpy(o->data(), nv12.data(), (size_t)w * h);
            uint8_t *u = o->data() + (size_t)w * h, *v = u + (size_t)(w / 2) * (h / 2);
            const uint8_t* uv = nv12.data() + (size_t)w * h;
            for (size_t i = 0; i < (size_t)(w / 2) * (h / 2); i++) u[i] = uv[2 * i], v[i] = uv[2 * i + 1];
        }
        st.img = std::move(o), st.w = w, st.h = h;
        if (hd_slow(*st.hd, t0)) st.img.reset();
    }
    return st.decoded == target && st.img;
}
// 4K frames decoded ahead on a worker thread (user 11-28: Tsubasa's song, m07, choppy; plainest at 240 fps). Decoding on
// the game thread cost 7.9 ms median, 12.5 ms p90, up to 29 ms per movie frame (s127 m06 at 240 fps): a stall every
// 33 ms. The game thread now only names the frame it needs; the worker keeps want - 2 .. want + kAhead decoded, and a
// frame not ready yet shows the previous 4K frame (else 720p). Idle 2 s: decoder and frames freed.
struct PrimeFrame { std::shared_ptr<std::vector<uint8_t>> img; uint32_t w = 0, h = 0; };
struct PrimeWorker {
    std::mutex m;
    std::condition_variable cv;
    int movie = -1, want = -1;
    double asked = 0;  // steady seconds of the last request
    std::map<int, PrimeFrame> frames;
} g_pw;
constexpr int kAhead = 4;
double steady_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void prime_worker() {
    PrimeState st;
    std::unique_lock<std::mutex> l(g_pw.m);
    for (;;) {
        if (g_pw.want < 0 || steady_s() - g_pw.asked > 2) {
            if (st.hd || !g_pw.frames.empty()) st.hd.reset(), st.img.reset(), st.decoded = -1, g_pw.frames.clear();
            g_pw.want = -1;
            g_pw.cv.wait_for(l, std::chrono::milliseconds(500));
            continue;
        }
        if (st.movie != g_pw.movie) st.movie = g_pw.movie, st.hd.reset(), st.img.reset(), st.decoded = -1;
        const int want = g_pw.want, mv = st.movie;
        for (auto it = g_pw.frames.begin(); it != g_pw.frames.end();)
            it = it->first < want - 8 || it->first > want + kAhead ? g_pw.frames.erase(it) : std::next(it);
        int next = want;
        while (next <= want + kAhead && g_pw.frames.count(next)) next++;
        // far enough ahead (or 720p for this movie); a frame just behind the decoder is not worth a reopen from a
        // keyframe: prime_get shows its neighbour
        if (next > want + kAhead || (st.hd && st.hd->slow) || (st.decoded > next && st.decoded - next <= 8)) {
            g_pw.cv.wait_for(l, std::chrono::milliseconds(50));
            continue;
        }
        l.unlock();
        const bool ok = prime_seek(st, next, true);
        l.lock();
        if (ok && mv == g_pw.movie) g_pw.frames[next] = {st.img, st.w, st.h}, g_pw.cv.notify_all();
        else if (!ok) g_pw.cv.wait_for(l, std::chrono::milliseconds(5));  // file still being read / end of the stream
    }
}
// the 4K frame for (movie, frame) when the worker has it, else the newest one up to 4 behind (a few ms late beats a flip
// to 720p, UchiLazuli 11-28); also asks for it. Not there: waits up to `wait_ms` for the worker (a 100 ms wait for the
// first frame was tried: a visible 142 ms hitch at the movie start, worse than its ~4 frames at 720p).
PrimeFrame prime_get(int movie, int frame, int wait_ms) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(prime_worker).detach(); });
    std::unique_lock<std::mutex> l(g_pw.m);
    if (g_pw.movie != movie) g_pw.frames.clear();
    if (g_pw.movie != movie || g_pw.want != frame) g_pw.movie = movie, g_pw.want = frame, g_pw.cv.notify_all();
    g_pw.asked = steady_s();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        if (auto it = g_pw.frames.find(frame); it != g_pw.frames.end()) return it->second;
        if (std::chrono::steady_clock::now() >= until) break;
        g_pw.cv.wait_until(l, until);
    }
    for (int k = 1; k <= 4; k++)
        for (int f : {frame - k, frame + k})
            if (auto it = g_pw.frames.find(f); it != g_pw.frames.end()) return it->second;
    return {};
}
}  // namespace

HLE_FN(mana_frame_copy, "mana_frame_copy") {
    const uint32_t desc = c->r[1];
    c->r[0] = call_unhooked(c, "mana_frame_copy", {c->r[0], c->r[1], c->r[2], c->r[3]});
    auto& movies = prime_index();
    if (movies.empty()) return;
    uint32_t ptr[3], w[3], h[3], pitch[3];
    for (int k = 0; k < 3; k++) ptr[k] = ld32(desc + 16 * k), w[k] = ld32(desc + 16 * k + 4), h[k] = ld32(desc + 16 * k + 8), pitch[k] = ld32(desc + 16 * k + 12);
    if (!ptr[0] || w[0] < 16 || h[0] < 9 || pitch[0] < w[0]) return;
    static std::mutex m;
    static PrimeState st;
    std::lock_guard<std::mutex> l(m);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static int n4k = 0, n720 = 0, nmiss = 0, sum_movie = -1;  // per-movie summary (nx.log), so flicker shows in players' logs
    auto summary = [&] {
        if (sum_movie >= 0 && n4k + n720 + nmiss > 0)
            log("movie: %s: %d frames 4K, %d at 720p, %d not matched", movies[sum_movie].ivf.filename().string().c_str(), n4k, n720, nmiss);
        n4k = n720 = nmiss = 0;
    };
    if (now - st.last > 0.5) st.idx = -1, summary(), st.shown = st.waited = false;  // a new movie (or a long pause): search from the start
    st.last = now;
    uint8_t t[144];  // 16x9 block means of Y, every 2nd pixel of every 2nd row
    const uint8_t* y = (const uint8_t*)GPTR(ptr[0]);
    for (int by = 0; by < 9; by++)
        for (int bx = 0; bx < 16; bx++) {
            uint32_t s = 0, n = 0;
            for (uint32_t yy = by * h[0] / 9; yy < (by + 1) * h[0] / 9; yy += 2)
                for (uint32_t xx = bx * w[0] / 16; xx < (bx + 1) * w[0] / 16; xx += 2) s += y[(size_t)yy * pitch[0] + xx], n++;
            t[by * 16 + bx] = (uint8_t)(n ? s / n : 0);
        }
    // the best frame; among frames within 1 of the best diff the one nearest the expected next frame (st.idx + 1).
    // Near-still shots (m07 / m12 songs) have runs of equal thumbnails: taking the first of them stepped back up to 2
    // frames, which restarted the 4K decoder from a keyframe (UchiLazuli 11-28 "still a bit choppy"; offline on m07:
    // 29 backward steps + 154 frames off by > 4 -> 0 and 0).
    auto best_in = [&](int mv, int lo, int hi, int& bf, int& bd) {
        const int n = (int)movies[mv].thm.size() / 144, e = st.idx + 1, from = std::max(lo, 0), to = std::min(hi, n - 1);
        if (from > to) return;
        std::vector<int> d(to - from + 1);
        int m = 256;
        for (int f = from; f <= to; f++) m = std::min(m, d[f - from] = thm_diff(t, &movies[mv].thm[(size_t)f * 144]));
        if (m >= bd) return;
        int pick = -1;
        for (int f = from; f <= to; f++)
            if (d[f - from] <= m + 1 && (pick < 0 || (st.idx >= 0 && abs(f - e) < abs(pick - e)))) pick = f;
        bf = pick, bd = d[pick - from];
    };
    constexpr int kMatch = 6;  // mean abs difference of a matching frame (MPEG IDCT / scaling noise is ~1)
    int bm = st.movie, bf = -1, bd = 256;
    if (st.idx >= 0) best_in(st.movie, st.idx - 2, st.idx + 6, bf, bd);
    if (bd > kMatch) {  // lost or new: every frame of every prime movie (~8000 x 144 bytes)
        bm = -1, bf = -1, bd = 256;
        for (int mv = 0; mv < (int)movies.size(); mv++) {
            int f = -1, d = bd;
            best_in(mv, 0, 1 << 30, f, d);
            if (d < bd) bm = mv, bf = f, bd = d;
        }
    }
    static int logged = 0;
    if (bd > kMatch) {
        nmiss++;
        if (st.idx >= 0) st.idx++;
        if (logged++ < 8) log("movie: MPEG-1 frame not matched (best diff %d)", bd);
        return;
    }
    if (bm != sum_movie) summary(), sum_movie = bm;
    st.movie = bm, st.idx = bf;
    static const bool plog = getenv("NX_PRIME_LOG") != nullptr;  // debug: per-frame match / 4K frame timing
    const auto ts = std::chrono::steady_clock::now();
    const PrimeFrame pf = prime_get(st.movie, bf, 4);
    (pf.img ? n4k : n720)++;
    if ((n4k + n720) % 600 == 0) summary();  // every 20 s too: the next movie may never come
    st.shown |= pf.img != nullptr;
    if (plog) log("prime: frame %d diff %d %s %.2f ms (copy-to-copy %.1f ms)", bf, bd, pf.img ? "4K" : "720p",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count(), (now - st.prev) * 1000);
    st.prev = now;
    if (!pf.img) return;
    auto reg = [&](int k, size_t off, uint32_t hw, uint32_t hh) {
        HdPlane p{movie_fp((const uint8_t*)GPTR(ptr[k]), w[k], h[k], pitch[k]), pf.img, off, hw, hh};
        std::lock_guard<std::mutex> hl(g_hd_lock);
        g_hd_planes.push_back(p);
        while (g_hd_planes.size() > 16) g_hd_planes.pop_front();
        g_hd_any = true;
    };
    const size_t ys = (size_t)pf.w * pf.h, cs = ys / 4;
    reg(0, 0, pf.w, pf.h);
    if (ptr[1] && pitch[1] >= w[1]) reg(1, ys, pf.w / 2, pf.h / 2);
    if (ptr[2] && pitch[2] >= w[2]) reg(2, ys + cs, pf.w / 2, pf.h / 2);
    if (logged++ < 8) log("movie: MPEG-1 frame -> %s frame %d (diff %d)", movies[st.movie].ivf.filename().string().c_str(), bf, bd);
}
