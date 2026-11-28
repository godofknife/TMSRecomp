// Host window, input and display timing (SDL3), plus the nn::vi and nn::hid hooks that read them.
// SDL wants its events pumped on the thread that made the window, so a dedicated UI thread owns both.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include "plat.h"
#ifdef _WIN32
#include <timeapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>
#endif

#include <algorithm>
#include <thread>

#include "settings.h"
#include "rt.h"
namespace vk { float wide_factor(); }  // vk.cpp: ultrawide width factor (setting aspect)

namespace nvn { void vk_wait_present_taken(double max_ms); }  // vk.cpp: the previous frame went to the display path (back-pressure)
namespace nvn { extern std::atomic<int> g_seq_request; }  // vk_replay.cpp: NX_CONTROL "seq"

namespace rt {

struct PadState {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};

static std::mutex g_ui_lock;
static PadState g_pad;
static SDL_Window* g_window;
static std::vector<uint32_t> g_vsync_events;  // writable kernel event handles
static std::once_flag g_ui_once;
static std::condition_variable g_ui_ready_cv;
static bool g_ui_ready = false;
std::atomic<uint64_t> g_vsync_count{0};
std::atomic<uint64_t> g_pace_tick{0};  // vsync tick at which the last frame was released (present on the next tick)
std::atomic<uint64_t> g_late_vsyncs{0};  // vsyncs missed because a frame was not ready (frame stats)
std::atomic<uint32_t> g_frame_work_max_us{0};  // longest game frame (pace return to next pace call), frame stats
std::atomic<uint64_t> g_frame_no{0};           // pace_present calls (NX_PROFILE spike frames)
static std::condition_variable g_vsync_cv;
// wait until the vsync clock reaches tick (present_thread: fixed display cadence)
void wait_vsync_tick(uint64_t tick) {
    std::unique_lock<std::mutex> l(g_ui_lock);
    g_vsync_cv.wait(l, [&] { return g_vsync_count >= tick; });
}

// NVN present interval n: the frame stays on screen for n vblanks of the 60 Hz clock, so the next present waits for
// the n-th vblank after the previous one. The game asks for 2 (30 fps). NX_PRESENT_INTERVAL=n overrides it (1 = try
// 60 fps); NX_NO_VSYNC skips pacing (benchmarks).
// NX_FPS=0 / N > 60: no vblank pacing; wait for the cap (if any), then give the next game frame the measured time of
// this one as its dt (at most 1/30 s: slower frames slow the game down, as on the Switch).
static std::atomic<int> g_fixed_dt{0};
static void pace_variable(int fps) {
    static const uint64_t freq = SDL_GetPerformanceFrequency();
    static uint64_t prev = 0;
    // never make frames faster than they reach the screen (bounded: 2 frame times of the cap, 60 fps uncapped)
    if (!g_fixed_dt) nvn::vk_wait_present_taken(2000.0 / (fps ? fps : 60));
    uint64_t now = SDL_GetPerformanceCounter();
    if (prev) {  // uncapped still stops at 1000 fps: dt is clamped to >= 1 ms, faster frames would speed the game up
        uint64_t target = prev + freq / (fps ? fps : 1000);
        while (now < target) {
            if (target - now > freq / 500) Sleep(1);  // sleep until ~2 ms before, then spin
            else YieldProcessor();
            now = SDL_GetPerformanceCounter();
        }
    }
    // debug NX_CONTROL "fixdt n": every frame steps 1/n s whatever its real time (slow drawlog / capture frames keep
    // the rate); "fixdt 0" ends it
    const int fixed = g_fixed_dt;
    if (prev) set_game_dt(fixed ? 1.0f / fixed : std::clamp((float)(now - prev) / freq, 1 / 1000.0f, 1 / 30.0f));
    prev = now;
}

// NX_STALL_STATS=1 (diagnostic, 1% low work s85): every 10 s, for the slowest 1% of game frames (pace to pace) and for the
// median ones: wall ms, ms the game thread really ran (QueryThreadCycleTime), process CPU ms (all threads) and page faults.
// Running ~= wall: the slow frame is more work; running << wall: the thread was not running (lock, preemption, kernel).
static void stall_stats() {
#ifdef _WIN32
    static const bool on = getenv("NX_STALL_STATS") != nullptr;
    if (!on) return;
    struct F { float wall, run, proc, pf; };
    extern std::atomic<uint64_t> g_ww_reset_pages, g_commit_pages;
    static uint64_t ww0 = 0, cm0 = 0;
    static std::vector<F> v;
    static uint64_t q0 = 0, c0 = 0, p0 = 0, t0 = 0, pf0 = 0, win_q = 0, win_t = 0;
    static LARGE_INTEGER fq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    uint64_t c = 0, p = 0, t = __rdtsc();
    QueryThreadCycleTime(GetCurrentThread(), &c);
    QueryProcessCycleTime(GetCurrentProcess(), &p);
    PROCESS_MEMORY_COUNTERS pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm);
    static double tsc_per_ms = 0;
    if (q0) {
        double wall = (double)(q.QuadPart - q0) * 1000.0 / fq.QuadPart;
        if (tsc_per_ms == 0) tsc_per_ms = (double)(t - t0) / wall;
        v.push_back({(float)wall, (float)((c - c0) / tsc_per_ms), (float)((p - p0) / tsc_per_ms), (float)(pm.PageFaultCount - pf0)});
    } else win_q = q.QuadPart, win_t = t;
    q0 = q.QuadPart, c0 = c, p0 = p, t0 = t, pf0 = pm.PageFaultCount;
    if ((double)(q.QuadPart - win_q) / fq.QuadPart >= 10 && v.size() > 200) {
        tsc_per_ms = (double)(t - win_t) * fq.QuadPart / 1000.0 / (double)(q.QuadPart - win_q);  // recalibrate on the window
        std::sort(v.begin(), v.end(), [](const F& a, const F& b) { return a.wall > b.wall; });
        auto avg = [&](size_t from, size_t n) { F s{}; for (size_t i = from; i < from + n; i++) s.wall += v[i].wall, s.run += v[i].run, s.proc += v[i].proc, s.pf += v[i].pf;
                                                 return F{s.wall / n, s.run / n, s.proc / n, s.pf / n}; };
        size_t k = std::max<size_t>(v.size() / 100, 1);
        F s = avg(0, k), m = avg(v.size() / 2 - k / 2, k);
        uint64_t ww = g_ww_reset_pages.load();
        uint64_t cm = g_commit_pages.load();
        log("stall stats: %.0f write-watch pages re-armed, %.0f guest pages committed per frame", (double)(ww - ww0) / v.size(), (double)(cm - cm0) / v.size());
        ww0 = ww, cm0 = cm;
        log("stall stats: slowest 1%% (%zu frames): %.2f ms wall, game thread ran %.2f ms, process CPU %.2f ms, %.0f page faults; median: %.2f ms wall, ran %.2f, process %.2f, %.0f faults; worst %.2f wall ran %.2f faults %.0f",
            k, s.wall, s.run, s.proc, s.pf, m.wall, m.run, m.proc, m.pf, v[0].wall, v[0].run, v[0].pf);
        v.clear(), win_q = q.QuadPart, win_t = t;
    }
#endif  // NX_STALL_STATS: Windows only (thread / process cycle counters)
}

std::atomic<uint64_t> g_pace_us{0};  // time inside pace_present (cap / present waits), frame stats
static std::atomic<int> g_refresh_hz{0};  // the window's display at start (0 = unknown)
// present_mode = vrr (G-SYNC / FreeSync, user 11-23): FIFO (vk.cpp) and, when the frame rate is uncapped or above the
// display, a cap 3 fps under its refresh so every frame stays inside the VRR range (no vsync queue, no tearing).
static int vrr_cap(int fps) {
    static const bool vrr = getenv("NX_PRESENT_MODE") && !strcmp(getenv("NX_PRESENT_MODE"), "vrr");
    const int hz = g_refresh_hz;
    if (!vrr || hz < 50 || (fps > 0 && fps <= std::max(60, hz - 3))) return fps;  // 30 / 60 keep their own pacing
    static bool said = false;
    if (!said) said = true, log("ui: VRR: frame cap %d (display %d Hz)", hz - 3, hz);
    return hz - 3;
}
// Auto 30 (setting auto_30, default 1; issue #11, Augs: i5-8500 at the Shibuya crosswalk ran 48 fps). At fps 60 the
// game steps a fixed 1/60 s per frame, so a PC that cannot hold 60 plays in slow motion. When frames miss their vsync
// (rules in auto30, s125d), frames go out every 2nd vsync with a 1/30 s step (full game speed, the Switch's rate; menus
// / HUD tick at 30 Hz then). Back to 60 when frames take under 13 ms of work (pace to pace, render thread wait included).
// s125 (Augs, town: "it slows down while it's trying to lock to 30, and again while it climbs back to 60"): while at 60, a
// frame that missed its vsync makes the next step 1/30 s instead of 1/60 (catch-up, at most 2 vsyncs), so the game
// keeps full speed during the 1 s Auto 30 needs to decide and during a failed try of 60; on-time frames step 1/60.
static std::atomic<int> g_auto30{-1};
static bool g_half = false;  // game thread
// s125c (Augs: "it doesn't let go of 30 when going into the shop from the crosswalk"): a new area's loading hitches
// counted as slow frames in the 3 s test at 30, so 60 came back late. A new field area tries 60 again ~0.75 s after
// it loads (past its texture uploads); with the catch-up step a failed try costs no slow motion.
static std::atomic<uint64_t> g_auto30_retry_at{0};
void rt::auto30_new_area() { g_auto30_retry_at = g_frame_no + 45; }
bool rt::auto30_active() { return g_half; }
bool rt::auto30_setting() {
    if (g_auto30 < 0) g_auto30 = !getenv("NX_AUTO_30") || atoi(getenv("NX_AUTO_30")) != 0;
    return g_auto30 != 0;
}
void rt::set_auto30(bool on) { g_auto30 = on; }
static uint32_t auto30(uint32_t interval, bool late, uint32_t work_us) {
    static int n = 0, hold = 30;  // n: frames since the last switch; hold: see below
    static uint64_t back60 = 0, hist = 0;  // hist bit i = frame i ago was late (at 60) / over 13 ms (at 30)
    const bool want = auto30_setting() && interval == 1;
    if (!want) {
        if (g_half) g_half = false, n = 0, hist = 0, set_game_dt(1 / 60.0f), log("fps: auto 30 off: 60 again");
        return interval;
    }
    if (const uint64_t at = g_auto30_retry_at; at && g_frame_no >= at) {
        g_auto30_retry_at = 0;
        if (g_half) {
            g_half = false, n = 0, hist = 0, hold = 30, set_game_dt(1 / 60.0f);
            log("fps: new area: trying 60 again");
            return 1;
        }
    }
    // s125d (Augs: "it takes time to decide to lock to 30 when the framerate dips, and time to climb back to 60 when I
    // know it can; standing still ~5 s lets it recover"): rolling windows instead of fixed 60 / 90-frame blocks. With
    // the catch-up step a wrong switch costs no slow motion, so it reacts fast: 30 when 6 of the last 30 frames missed
    // their vsync (~0.5 s); 60 again once the last 45 frames at 30 (1.5 s) all but one took under 13 ms. `hold` (frames
    // at 30 before that check, 30 = 1 s) doubles up to 20 s each time 60 failed again within 2 s (no flip-flop in a
    // borderline spot), back to 1 s otherwise.
    hist = hist << 1 | (g_half ? work_us >= 13000 : late);
    ++n;
    if (!g_half && n >= 30 && __builtin_popcountll(hist & ((1ull << 30) - 1)) >= 6) {
        hold = g_frame_no - back60 < 120 ? std::min(hold * 2, 600) : 30;
        g_half = true, set_game_dt(1 / 30.0f), hist = 0, n = 0;
        log("fps: auto 30: 6 of the last 30 frames missed their vsync (next try of 60 in %.0f s or later)", std::max(hold, 45) / 30.0);
    } else if (g_half && n >= std::max(hold, 45) && __builtin_popcountll(hist & ((1ull << 45) - 1)) <= 1) {
        g_half = false, set_game_dt(1 / 60.0f), back60 = g_frame_no, hist = 0;
        log("fps: back to 60 after %.1f s at 30", n / 30.0);
        n = 0;
    }
    return g_half ? 2 : 1;
}
static std::atomic<int> g_extra_ms{0};  // test knob (pace_present)
void pace_present(uint32_t interval) {
    const int fps = vrr_cap(fps_setting());
    stall_stats();
    frame_begin();
    g_frame_no++;
    profile_start();
    {  // "perf: game speed" every 10 s (s125): game time stepped per real time; under ~97% = slow motion (the PC could not
       // keep the fps setting's step; loading screens also lower it)
        static const uint64_t freq = SDL_GetPerformanceFrequency();
        static uint64_t w0 = 0;
        static double g = 0;
        const uint64_t now = SDL_GetPerformanceCounter();
        g += fps == 30 ? 1 / 30.0 : game_base_dt();
        if (!w0) w0 = now, g = 0;
        else if (now - w0 >= 10 * freq) {
            log("perf: game speed %.0f%% of real time%s", 100 * g * freq / (double)(now - w0), g_half ? " (auto 30 on)" : "");
            w0 = now, g = 0;
        }
    }
    if (fps == 0 || fps > 60) {
        g_half = false;  // the variable step keeps the game's speed by itself; set_fps(60) sets 1/60 again
        const uint64_t t0 = SDL_GetPerformanceCounter();
        pace_variable(fps);
        g_pace_us += (SDL_GetPerformanceCounter() - t0) * 1000000 / SDL_GetPerformanceFrequency();
        return;
    }
    static uint64_t last = 0;
    static const bool off = getenv("NX_NO_VSYNC") != nullptr;
    static const char* force = getenv("NX_PRESENT_INTERVAL");
    if (force) interval = (uint32_t)atoi(force);
    if (off || !interval) return;
    // test: a slower PC (NX_FRAME_EXTRA_MS, or NX_CONTROL "extrams n" live): n ms more work per frame
    static const bool extra_env = [] { if (getenv("NX_FRAME_EXTRA_MS")) g_extra_ms = atoi(getenv("NX_FRAME_EXTRA_MS")); return true; }();
    (void)extra_env;
    if (const int x = g_extra_ms) SDL_Delay((uint32_t)x);
    static uint64_t back = 0;  // performance counter when the previous pace returned
    uint64_t in = SDL_GetPerformanceCounter();
    uint32_t us = 0;
    if (back) {
        us = (uint32_t)((in - back) * 1000000 / SDL_GetPerformanceFrequency());
        if (us > g_frame_work_max_us) g_frame_work_max_us = us;
    }
    std::unique_lock<std::mutex> l(g_ui_lock);
    if (fps == 60) interval = auto30(interval, back && g_vsync_count > last + 1, us);
    uint64_t target = last + interval;
    if (g_vsync_count < target) g_vsync_cv.wait(l, [&] { return g_vsync_count >= target; });
    else if (g_vsync_count > target) g_late_vsyncs += g_vsync_count - target;  // the frame took too long
    // Auto 30 catch-up (above): at 60, the next step covers the vsyncs this frame really took (1 or 2)
    static const bool catchup = !getenv("NX_AUTO30_CATCHUP") || atoi(getenv("NX_AUTO30_CATCHUP")) != 0;
    if (fps == 60 && interval == 1 && !g_half && auto30_setting() && catchup && last)
        set_game_dt(std::clamp<uint64_t>(g_vsync_count - last, 1, 2) / 60.0f);
    last = g_vsync_count;
    g_pace_tick = last;
    l.unlock();
    back = SDL_GetPerformanceCounter();
}

// Npad button bits (nn::hid::NpadButton)
enum : uint64_t { B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LS = 1 << 4, B_RS = 1 << 5, B_L = 1 << 6,
                  B_R = 1 << 7, B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12,
                  B_UP = 1 << 13, B_RIGHT = 1 << 14, B_DOWN = 1 << 15 };

static uint64_t button_mask(const std::string& b) {
    if (size_t plus = b.find('+'); plus != std::string::npos && plus > 0) {  // "l3+r3": together
        uint64_t x = button_mask(b.substr(0, plus)), y = button_mask(b.substr(plus + 1));
        return x && y ? x | y : 0;
    }
    return b == "l3" ? B_LS : b == "r3" ? B_RS : b == "a" ? B_A : b == "b" ? B_B : b == "x" ? B_X : b == "y" ? B_Y : b == "plus" ? B_PLUS : b == "minus" ? B_MINUS
         : b == "up" ? B_UP : b == "down" ? B_DOWN : b == "left" ? B_LEFT : b == "right" ? B_RIGHT : b == "l" ? B_L
         : b == "r" ? B_R : b == "zl" ? B_ZL : b == "zr" ? B_ZR : 0;
}

// NX_CONTROL=<file>: live control for an agent driving the game. Each line appended to the file is a command: a button
// name (pressed for 150 ms; several lines in one write press together) or "shot" (the next presented frame is written
// to <data>/shots/live_NNNNN.bmp), "drawlog" (log every draw of the next frame, like NX_LOG_DRAWS) or "rt" (dump the
// colour targets of the next frame, like NX_DUMP_RT) or "speed n" (run the game n times faster; 1 = normal) or
// "ls x y ms" / "rs x y ms" (hold the left / right stick at x, y in -32767..32767 for ms game milliseconds) or
// "verbose n" (like -v: 1 logs fs reads etc., 0 stops) or "cover" (NX_COVER report now) or "flag n [0|1]" (log or
// set game flag n, hle_sys.cpp) or "pos" (log the player position) or "poke addr byte" (hex, debug write).
static std::atomic<int> g_fs_request{0};  // control file "fs" (rigs)
std::atomic<int> g_shot_request{0}, g_drawlog_request{0}, g_rt_request{0}, g_user_shot{0};  // g_user_shot: F12
static int g_ctl_stick[4];  // NX_CONTROL lx ly rx ry, valid until g_ctl_stick_until
static uint64_t g_ctl_stick_until[2];
static uint64_t control_buttons(uint64_t now) {
    static const char* path = getenv("NX_CONTROL");
    static long done = 0;  // bytes consumed
    static uint64_t next_check = 0, until = 0, held = 0;
    if (!path) return 0;
    if (now >= next_check) {
        next_check = now + 50;
        if (FILE* f = fopen(path, "rb")) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            if (size > done) {
                std::string buf((size_t)(size - done), ' ');
                fseek(f, done, SEEK_SET);
                buf.resize(fread(buf.data(), 1, buf.size(), f));
                size_t end = buf.rfind('\n');
                if (end != std::string::npos) {
                    done += (long)end + 1;
                    size_t pos = 0;
                    while (pos <= end) {
                        size_t nl = buf.find('\n', pos);
                        std::string cmd = buf.substr(pos, nl - pos);
                        while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == ' ')) cmd.pop_back();
                        if (cmd == "shot") g_shot_request++;
                        else if (cmd == "fs") g_fs_request++;  // test: the F11 toggle (#17), done on the window thread
                        else if (cmd == "btlspeed") set_battle_speed(battle_speed() % 3 + 1, true);  // = F3 / touchpad (rigs)
                        else if (cmd.rfind("shots ", 0) == 0) g_shot_request += atoi(cmd.c_str() + 6);  // the next N frames
                        else if (cmd.rfind("drawlog", 0) == 0) g_drawlog_request += cmd.size() > 8 ? atoi(cmd.c_str() + 8) : 1;  // "drawlog N": N frames
                        else if (cmd == "rt") g_rt_request++;
                        else if (cmd.rfind("extrams ", 0) == 0) g_extra_ms = atoi(cmd.c_str() + 8);  // test: slower frames
                        else if (cmd.rfind("skipshadow ", 0) == 0) rt::g_skip_shadow = atoi(cmd.c_str() + 11);
                        else if (cmd.rfind("aa ", 0) == 0) rt::apply_aa(atoi(cmd.c_str() + 3));
                        else if (cmd.rfind("upq ", 0) == 0) rt::apply_fsr3_quality(atoi(cmd.c_str() + 4));
                        else if (cmd.rfind("msaa ", 0) == 0) rt::apply_msaa(atoi(cmd.c_str() + 5));
                        else if (cmd.rfind("hdr ", 0) == 0) rt::set_hdr(atoi(cmd.c_str() + 4));  // live, not saved
                        else if (cmd == "cover") cover_report();
                        else if (cmd.rfind("speed ", 0) == 0) set_speed(atoi(cmd.c_str() + 6));
                        else if (cmd.rfind("env ", 0) == 0) { _putenv(cmd.c_str() + 4); log("env %s", cmd.c_str() + 4); }  // env NX_KEY=v: a setting, live
                        else if (cmd.rfind("fps ", 0) == 0) set_fps(atoi(cmd.c_str() + 4));
                        else if (cmd.rfind("verbose ", 0) == 0) g_verbose = atoi(cmd.c_str() + 8);
                        else if (cmd.rfind("flag ", 0) == 0) flag_command(cmd.c_str() + 5);
                        else if (cmd.rfind("fixdt ", 0) == 0) { g_fixed_dt = atoi(cmd.c_str() + 6); log("fixdt %d", g_fixed_dt.load()); }
                        else if (cmd.rfind("dtkeep ", 0) == 0) { g_dt_keep = strtoul(cmd.c_str() + 7, nullptr, 16); log("dtkeep %08x", g_dt_keep.load()); }
                        else if (cmd == "pos") pos_command();
                        else if (cmd == "lmap") rt::overlay_request_map();
                        else if (cmd == "seq") nvn::g_seq_request = 1;
                        else if (cmd.rfind("travel ", 0) == 0) rt::overlay_request_travel(atoi(cmd.c_str() + 7));
                        else if (cmd == "actors") pos_command("all");
                        else if (cmd.rfind("uipart ", 0) == 0) { g_ui_part30 = strtoul(cmd.c_str() + 7, nullptr, 16); log("uipart %x", g_ui_part30.load()); }
                        else if (cmd.rfind("gate30 ", 0) == 0) { g_gate30 = strtoul(cmd.c_str() + 7, nullptr, 16); log("gate30 %x", g_gate30.load()); }
                        else if (cmd.rfind("uifreeze ", 0) == 0) { g_ui_freeze = atoi(cmd.c_str() + 9); log("uifreeze %u", g_ui_freeze.load()); }
                        else if (cmd.rfind("uigate ", 0) == 0) { g_ui_gate = atoi(cmd.c_str() + 7); log("uigate %d", g_ui_gate.load()); }
                        else if (cmd.rfind("setpos ", 0) == 0) pos_command(cmd.c_str() + 7);
                        else if (cmd.rfind("watch", 0) == 0) watch_command(cmd.c_str() + 5);
                        else if (cmd.rfind("dump ", 0) == 0) dump_command(cmd.c_str() + 5);
                        else if (cmd.rfind("spr ", 0) == 0) sprite_command(cmd.c_str() + 4);
                        else if (cmd.rfind("task30 ", 0) == 0) rt::task30_command(cmd.c_str() + 7);
                        else if (cmd.rfind("uidraw30 ", 0) == 0) rt::g_uidraw30 = (uint32_t)strtoul(cmd.c_str() + 9, nullptr, 16);
                        else if (cmd.rfind("poke ", 0) == 0) {  // poke addr byte (hex): one guest byte, debug
                            unsigned pa = 0, pv = 0;
                            if (sscanf(cmd.c_str() + 5, "%x %x", &pa, &pv) == 2) { log("poke %08x: %02x -> %02x", pa, ld8(pa), pv); st8(pa, pv); }
                        }
                        else if (cmd.rfind("ls ", 0) == 0 || cmd.rfind("rs ", 0) == 0) {
                            int s = cmd[0] == 'r', x = 0, y = 0, ms = 0;
                            sscanf(cmd.c_str() + 3, "%d %d %d", &x, &y, &ms);
                            g_ctl_stick[2 * s] = x;
                            g_ctl_stick[2 * s + 1] = y;
                            g_ctl_stick_until[s] = now + ms / g_speed;
                        }
                        else if (uint64_t m = button_mask(cmd)) { held |= m; until = now + 150 / g_speed; }
                        else if (!cmd.empty()) log("NX_CONTROL: unknown command '%s'", cmd.c_str());
                        pos = nl + 1;
                    }
                }
            }
            fclose(f);
        }
    }
    if (now >= until) held = 0;
    return held;
}

// NX_INPUT="5000:plus,8000:a,8500:a": press buttons for 150 ms at these times (ms after start), for unattended runs
// (milestone 3 replaces this with recorded input replay)
static uint64_t scripted_buttons() {
    static std::vector<std::pair<uint64_t, uint64_t>> script = [] {
        std::vector<std::pair<uint64_t, uint64_t>> v;
        const char* s = getenv("NX_INPUT");
        if (!s) return v;
        std::string str(s);
        size_t pos = 0;
        while (pos < str.size()) {
            size_t comma = str.find(',', pos);
            std::string item = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t colon = item.find(':');
            if (colon != std::string::npos) {
                uint64_t t = strtoull(item.c_str(), nullptr, 10);
                v.push_back({t, button_mask(item.substr(colon + 1))});
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return v;
    }();
    static uint64_t t0 = SDL_GetTicks();
    uint64_t now = SDL_GetTicks() - t0, m = 0;
    for (auto& [t, b] : script)
        if (now >= t && now < t + 150) m |= b;
    m |= control_buttons(now);
    for (int s = 0; s < 2; s++)
        if (now >= g_ctl_stick_until[s]) g_ctl_stick[2 * s] = g_ctl_stick[2 * s + 1] = 0;
    return m;
}

// DS4Windows (Discord report 11-24, DualSense): it reads the pad itself and shows the game a virtual Xbox (or DS4) pad.
// SDL also opened the real DualSense over HID, both fought over the device: the pad dropped for a second and its last
// state stayed held (menus kept scrolling, Itsuki kept walking). With DS4Windows running at start, SDL leaves
// PlayStation pads to it (HIDAPI PS4 / PS5 off) and the game takes the virtual pad; icons stay PlayStation.
static bool ds4windows_running() {
#ifndef _WIN32
    return false;  // Linux: DS4Windows is a Windows tool
#else
    static const bool on = [] {
        if (getenv("NX_FAKE_DS4WINDOWS") && atoi(getenv("NX_FAKE_DS4WINDOWS"))) return true;  // test: act as if it runs
        bool found = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{sizeof pe};
        for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
            found = !_wcsicmp(pe.szExeFile, L"DS4Windows.exe");
        CloseHandle(snap);
        return found;
    }();
    return on;
#endif
}

// Keyboard + mouse bindings (issue #5): settings.ini key_<target> = names, comma separated (NX_KEY_<TARGET>; settings.h
// k_key_targets). Names: SDL key names, Mouse1..Mouse5 (left, right, middle, back, forward), WheelUp / WheelDown, none.
struct KeyBind { uint64_t btn; int axis; int dir; };  // axis 0 none, 1 lx, 2 ly, 3 rx, 4 ry; names / defaults: settings.h k_key_targets
static const KeyBind k_key_binds[] = {
    {B_A, 0, 0}, {B_B, 0, 0}, {B_X, 0, 0}, {B_Y, 0, 0}, {B_L, 0, 0}, {B_R, 0, 0}, {B_ZL, 0, 0}, {B_ZR, 0, 0},
    {B_PLUS, 0, 0}, {B_MINUS, 0, 0}, {B_LS, 0, 0}, {B_RS, 0, 0}, {B_UP, 0, 0}, {B_DOWN, 0, 0}, {B_LEFT, 0, 0}, {B_RIGHT, 0, 0},
    {0, 2, 1}, {0, 2, -1}, {0, 1, -1}, {0, 1, 1}, {0, 4, 1}, {0, 4, -1}, {0, 3, -1}, {0, 3, 1}};
static_assert(std::size(k_key_binds) == std::size(k_key_targets), "settings.h k_key_targets order");
enum { MS_BUTTON = 0x10000, MS_WHEEL_UP = 0x20000, MS_WHEEL_DOWN = 0x20001 };  // codes above the scancodes: Mouse1..5, wheel
static std::vector<int> g_bind_codes[std::size(k_key_binds)];
static int64_t g_wheel_until[2];  // SDL ms: a wheel step holds its target this long (the game reads ~every frame; UI thread)
static float g_mouse_acc[2];      // mouse motion since the last camera step (UI thread)

static int bind_code(std::string n) {
    while (!n.empty() && n.front() == ' ') n.erase(n.begin());
    while (!n.empty() && n.back() == ' ') n.pop_back();
    if (n.empty() || !_stricmp(n.c_str(), "none")) return -1;
    if (n.size() == 6 && !_strnicmp(n.c_str(), "mouse", 5) && n[5] >= '1' && n[5] <= '5') return MS_BUTTON + (n[5] - '0');
    if (!_stricmp(n.c_str(), "WheelUp")) return MS_WHEEL_UP;
    if (!_stricmp(n.c_str(), "WheelDown")) return MS_WHEEL_DOWN;
    const SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) log("input: unknown key name \"%s\" in a key_ setting", n.c_str());
    return sc == SDL_SCANCODE_UNKNOWN ? -1 : (int)sc;
}

static void load_key_binds() {
    for (size_t i = 0; i < std::size(k_key_binds); i++) {
        std::string env = "NX_KEY_" + std::string(k_key_targets[i].name);
        for (auto& ch : env) ch = (char)toupper((unsigned char)ch);
        const char* v = getenv(env.c_str());
        const std::string list = v ? v : k_key_targets[i].def;
        for (size_t pos = 0; pos <= list.size();) {
            const size_t c = std::min(list.find(',', pos), list.size());
            if (int code = bind_code(list.substr(pos, c - pos)); code >= 0) g_bind_codes[i].push_back(code);
            pos = c + 1;
        }
    }
}

// Controller remapping (issue #7): settings.ini pad_<button> = the pad button that presses this Switch button. Names
// (positions, so they fit any pad): south east west north (face buttons), lb rb lt rt, start back, ls rs (stick clicks),
// paddle1..paddle4, none; unset / "default" = the button_layout mapping. A button taken from another Switch button
// hands that one the button it had (one setting swaps two buttons); the D-pad keeps its place.
enum { PAD_LT = 1000, PAD_RT = 1001 };
struct PadMap { const char* name; uint64_t b; int phys; };
static std::vector<PadMap> pad_map() {
    const bool xb = rt::button_layout_xbox();
    std::vector<PadMap> m = {
        {"a", B_A, xb ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST}, {"b", B_B, xb ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH},
        {"x", B_X, xb ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH}, {"y", B_Y, xb ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST},
        {"l", B_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r", B_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"zl", B_ZL, PAD_LT}, {"zr", B_ZR, PAD_RT},
        {"plus", B_PLUS, SDL_GAMEPAD_BUTTON_START}, {"minus", B_MINUS, SDL_GAMEPAD_BUTTON_BACK},
        {"l3", B_LS, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"r3", B_RS, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"up", B_UP, SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", B_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"left", B_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", B_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
    static const struct { const char* n; int phys; } names[] = {
        {"south", SDL_GAMEPAD_BUTTON_SOUTH}, {"east", SDL_GAMEPAD_BUTTON_EAST}, {"west", SDL_GAMEPAD_BUTTON_WEST},
        {"north", SDL_GAMEPAD_BUTTON_NORTH}, {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"lt", PAD_LT}, {"rt", PAD_RT}, {"start", SDL_GAMEPAD_BUTTON_START}, {"back", SDL_GAMEPAD_BUTTON_BACK},
        {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"paddle1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}, {"paddle2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
        {"paddle3", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}, {"paddle4", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2}, {"none", -1}};
    std::vector<bool> set(m.size());
    for (size_t i = 0; i < m.size(); i++) {
        std::string env = "NX_PAD_" + std::string(m[i].name);
        for (auto& ch : env) ch = (char)toupper((unsigned char)ch);
        const char* v = getenv(env.c_str());
        if (!v || !*v || !_stricmp(v, "default")) continue;
        int phys = -2;
        for (auto& n : names) if (!_stricmp(v, n.n)) phys = n.phys;
        if (phys == -2) { log("input: unknown pad button \"%s\" in pad_%s", v, m[i].name); continue; }
        for (size_t j = 0; j < m.size(); j++)  // the Switch button that had it gets this one's old button (a swap)
            if (j != i && !set[j] && phys >= 0 && m[j].phys == phys) m[j].phys = m[i].phys;
        m[i].phys = phys, set[i] = true;
        log("input: pad_%s = %s", m[i].name, v);
    }
    return m;
}

// button prompts (pad_icons.cpp) draw the symbol of the pad button that really presses A/B/X/Y (layout + pad_ remaps)
int pad_face_pos(int k) {
    static const auto map = pad_map();
    switch (map[k].phys) {
        case SDL_GAMEPAD_BUTTON_EAST: return 0;
        case SDL_GAMEPAD_BUTTON_SOUTH: return 1;
        case SDL_GAMEPAD_BUTTON_NORTH: return 2;
        case SDL_GAMEPAD_BUTTON_WEST: return 3;
    }
    return button_layout_xbox() ? k ^ 1 : k;  // on a shoulder / paddle: the layout's own symbol
}

static void poll_input() {
    PadState p;
    p.buttons |= scripted_buttons();
    const bool* k = SDL_GetKeyboardState(nullptr);
    static const bool binds_loaded = (load_key_binds(), true);
    (void)binds_loaded;
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
    const bool focus = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const int64_t now_ms = (int64_t)SDL_GetTicks();
    auto held = [&](int code) {
        if (code < SDL_SCANCODE_COUNT) return k[code];
        if (!focus) return false;
        if (code < MS_WHEEL_UP) return (mb & SDL_BUTTON_MASK(code - MS_BUTTON)) != 0;
        return now_ms < g_wheel_until[code - MS_WHEEL_UP];
    };
    p.lx = g_ctl_stick[0], p.ly = g_ctl_stick[1], p.rx = g_ctl_stick[2], p.ry = g_ctl_stick[3];
    int32_t* axes[] = {nullptr, &p.lx, &p.ly, &p.rx, &p.ry};
    for (size_t i = 0; i < std::size(k_key_binds); i++)
        for (int code : g_bind_codes[i])
            if (held(code)) {
                if (k_key_binds[i].btn) p.buttons |= k_key_binds[i].btn;
                else *axes[k_key_binds[i].axis] = 32767 * k_key_binds[i].dir;
            }
    // mouse camera (mouse_camera, default 1): the pointer is captured (relative mode) while the window has focus and the
    // F1 menu is closed; its motion becomes the right stick, one step every 16 ms (the game reads the pad per frame):
    // 40 px in a step = full tilt at mouse_sensitivity 1.0. The game's camera turns at stick speed (not 1:1 aim).
    static const bool replay_run = getenv("NX_REPLAY") && *getenv("NX_REPLAY");
    static const bool mouse_cam = !replay_run && (!getenv("NX_MOUSE_CAMERA") || atoi(getenv("NX_MOUSE_CAMERA")) != 0);
    static const float sens = getenv("NX_MOUSE_SENSITIVITY") ? (float)atof(getenv("NX_MOUSE_SENSITIVITY")) : 1.0f;
    static const bool invert = getenv("NX_MOUSE_INVERT_Y") && atoi(getenv("NX_MOUSE_INVERT_Y")) != 0;
    static int64_t step_at = 0;
    static int32_t cam[2];
    if (now_ms >= step_at) {
        step_at = now_ms + 16;
        for (int a = 0; a < 2; a++) {
            const float v = g_mouse_acc[a] * sens * (32767.0f / 40.0f) * (a == 1 && !invert ? -1.0f : 1.0f);
            cam[a] = (int32_t)std::clamp(v, -32767.0f, 32767.0f);
            g_mouse_acc[a] = 0;
        }
    }
    if (mouse_cam && (cam[0] || cam[1])) p.rx = cam[0], p.ry = cam[1];
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    static SDL_Gamepad* pad = nullptr;
    // replays (agent test runs) never read the real pad: SDL hands it to background windows, and the user's DualSense
    // (touchpad = battle speed, L3+R3 = F1 menu) changed settings and quit test runs (11-01)
    static const bool replay = getenv("NX_REPLAY") && *getenv("NX_REPLAY");
    if (pad && !SDL_GamepadConnected(pad)) {  // unplugged: take the next one
        log("input: pad disconnected");
        SDL_CloseGamepad(pad), pad = nullptr;
    }
    int pick = 0;  // DS4Windows: its virtual Xbox pad first (a real PlayStation pad may still show up through RawInput)
    for (int i = 0; ds4windows_running() && i < n; i++) {
        const SDL_GamepadType t = SDL_GetGamepadTypeForID(ids[i]);
        if (t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE) { pick = i; break; }
    }
    if (!pad && n > 0 && !replay && (pad = SDL_OpenGamepad(ids[pick]))) {
        SDL_GamepadType t = SDL_GetGamepadType(pad);
        g_pad_kind = t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 || ds4windows_running() ? 1
                     : t >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ? 0
                     : 2;  // Xbox, and generic pads (mostly XInput)
        log("input: pad \"%s\" (type %d) -> %s button icons", SDL_GetGamepadName(pad), (int)t,
            g_pad_kind == 1 ? "PlayStation" : g_pad_kind == 2 ? "Xbox" : "Switch");
    }
    SDL_free(ids);
    if (pad) {
        // positional mapping: the face button on the right is A, as on a Switch controller; button_layout = xbox swaps
        // A/B and X/Y (bottom = confirm, as in PC / Xbox games; pad_icons.cpp draws the prompts to match)
        static const auto map = pad_map();
        for (auto& m : map) {
            const bool on = m.phys == PAD_LT || m.phys == PAD_RT
                                ? SDL_GetGamepadAxis(pad, m.phys == PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000
                                : m.phys >= 0 && SDL_GetGamepadButton(pad, (SDL_GamepadButton)m.phys);
            if (on) p.buttons |= m.b;
        }
        auto ax = [&](SDL_GamepadAxis a, bool inv) {
            int v = SDL_GetGamepadAxis(pad, a);
            if (v > -4000 && v < 4000) v = 0;  // deadzone
            v = inv ? -v : v;
            return v < -32767 ? -32767 : v > 32767 ? 32767 : v;
        };
        if (int v = ax(SDL_GAMEPAD_AXIS_LEFTX, false)) p.lx = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_LEFTY, true)) p.ly = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_RIGHTX, false)) p.rx = v;
        if (int v = ax(SDL_GAMEPAD_AXIS_RIGHTY, true)) p.ry = v;
    }
    // P5-style battle buttons (setting battle_buttons = 1; hle_sys.cpp btl_wheel_input): while the command wheel waits,
    // the face buttons by position (bottom Attack, right Guard, top Skills, left Items), L Tactics, R Change, - Escape
    // pick a command at once; ZR = Auto battle (X on the wheel otherwise); + keeps Stats. The game never sees those
    // buttons there; A is pressed for it once the wheel has the command. The command's index comes from the wheel's own
    // list (g_wheel_ids): Tactics is not on every member's wheel (s114).
    static bool g_menu_open = false;  // the F1 / L3+R3 menu was open last poll
    bool p5_minus = false;  // - picked Escape this frame: the touchpad (same button on some pads) leaves the speed alone
    {
        const bool on = battle_buttons_mode() != 0 && !g_menu_open;  // F1 menu changes it live; while it is open the pad is its (user 11-26)
        static uint64_t prev = 0;
        static int64_t autob_until = 0, p5_start = 0, p5_tap = 0, p5_at_target = 0, p5_attack = 0;
        const int64_t t = (int64_t)GetTickCount64();
        const uint64_t raw = p.buttons;
        // the buttons are ours as soon as the panel shows (wheel open, also during a voice line: else Square reached the
        // game's own Auto-Battle there, user 11-26); the wheel is turned / confirmed only once it takes input
        const bool up = rt::p5_wheel_up(t, false), input = rt::p5_wheel_up(t, true);
        // mode 1 (buttons on the game's own wheel): a d-pad / stick turn hands the wheel back to the game's way (turn, then
        // confirm) until it closes (user 11-26: "Buttons" should still let the wheel be turned and pressed)
        static bool manual = false;
        const uint64_t dirs = B_LEFT | B_RIGHT | B_UP | B_DOWN;
        if (!up || battle_buttons_mode() != 1) manual = false;
        else if (rt::g_p5_target < 0 && ((raw & ~prev & dirs) || std::abs(p.lx) > 16000 || std::abs(p.ly) > 16000)) manual = true;
        // a button taken for a command stays the game's-unseen until released: else Square held past the confirm reached
        // the game as a fresh X press = Auto-Battle (user 11-26)
        static uint64_t eaten = 0;
        const uint64_t mine = B_A | B_B | B_X | B_Y | B_L | B_R | B_MINUS | B_ZR | dirs;
        if (on && up && !manual) eaten |= raw & mine;
        else eaten &= raw, p.buttons &= ~eaten;
        if (on && up && !manual) {
            const uint64_t edge = raw & ~prev;
            static const int by_pos[4] = {3, 0, 1, 2};  // east Guard, south Attack, north Skills, west Items (hle_sys.cpp wheel indexes)
            const uint64_t face[4] = {B_A, B_B, B_X, B_Y};
            int want = -1;
            for (int f = 0; f < 4; f++)
                if (edge & face[f]) want = by_pos[rt::pad_face_pos(f)];
            if (edge & B_L) want = 5;      // Tactics
            if (edge & B_R) want = 4;      // Change
            if (edge & B_MINUS) want = 6, p5_minus = true;  // Escape
            int at = -1;  // the command's index on this wheel
            for (int i = 0, ids = (int)rt::g_wheel_ids; want >= 0 && at < 0 && i < 8 && (ids >> 4 * i & 15); i++)
                if ((ids >> 4 * i & 15) == want + 1) at = i;
            if (want >= 0 && at < 0) log("p5: command %d not on this wheel", want);
            if (at >= 0 && rt::g_p5_target < 0) rt::g_p5_target = at, p5_start = t, p5_tap = 0, p5_at_target = 0, log("p5: command %d at %d", want, at);
            if (at >= 0 && want == 0) p5_attack = t;
            if (edge & B_ZR) autob_until = t + 50;
            p.buttons &= ~mine;
            p.ly = p.lx = 0;  // the stick would turn the wheel too
        }
        // Back out of an Attack: Itsuki's goes to his skill list, not the wheel (the game's own way, also without P5 buttons;
        // user 11-26: back to the panel). B in the target pick after a P5 Attack, and no wheel 150 ms later (the others'
        // wheel is back in ~35 ms): one more B for the game, up to 4 tries 400 ms apart, until the wheel is open. The list
        // shows for a moment: the game opens it itself.
        {
            static int64_t back_at = 0;
            const bool wheel = up;
            if (wheel && p5_attack && t - p5_attack > 1500) p5_attack = 0;  // a later wheel: that Attack's target pick is over
            if (on && !wheel && p5_attack && (raw & ~prev & B_B)) back_at = t, p5_attack = 0, log("p5: back out of Attack");
            if (back_at && (wheel || !on || t - back_at > 1600)) back_at = 0;
            if (back_at) {
                const int64_t d = t - back_at - 150;
                if (d >= 0 && d % 400 < 60) p.buttons |= B_B;
            }
        }
        prev = raw;
        // turning: one left / right tap per 100 ms (50 down) the short way round until the wheel's selection is the
        // command (right = index + 1, wraps after the wheel's last command), then 120 ms to settle, then A taps until the hook sees the confirm
        if (const int tg = rt::g_p5_target; on && tg >= 0) {
            const int cur = rt::g_wheel_sel;
            // give up: 6 s in all, the wheel gone, or 0.6 s of A at the command (the game refuses it now, e.g. Change
            // with nobody to switch in)
            if (t - p5_start > 6000 || !rt::p5_wheel_up(t - 180, false) || (p5_at_target && t - p5_at_target > 600))  // 6 s: a voice line holds the wheel
                rt::g_p5_target = -1, log("p5: gave up (wheel %d)", cur);
            else if (cur != tg) {
                p5_at_target = 0;
                int n = 0;
                while (n < 8 && (rt::g_wheel_ids >> 4 * n & 15)) n++;
                n = std::max(n, tg + 1);
                const int d = ((tg - cur) % n + n) % n;  // steps to the right
                if (t - p5_tap >= 100) p5_tap = t;
                if (input && t - p5_tap < 50) p.buttons |= d <= n / 2 ? B_RIGHT : B_LEFT;
            } else {
                if (!input) p5_at_target = 0;  // g_wheel_sel is the last member's until the wheel takes input
                else if (!p5_at_target) p5_at_target = t;
                if (input && p5_at_target && t - p5_at_target >= 120 && (t - p5_at_target) % 100 < 50) p.buttons |= B_A;
            }
        }
        if (on && t < autob_until) p.buttons |= B_X;                   // Auto battle
    }
    // battle speed 1x -> 2x -> 3x -> 1x: F3 or the DualSense touchpad (a button the Switch game never sees)
    static bool speed_prev = false;
    const bool speed = k[SDL_SCANCODE_F3] || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_TOUCHPAD));
    if (speed && !speed_prev && !p5_minus) set_battle_speed(battle_speed() % 3 + 1, true);
    speed_prev = speed;
    static bool f1_prev = false;
    bool f1 = k[SDL_SCANCODE_F1] && !f1_prev;
    f1_prev = k[SDL_SCANCODE_F1];
#ifdef _WIN32
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
    void* hwnd = g_window;  // Linux: the menu is drawn into the frame (overlay.cpp), it only needs the window size
#endif
    const bool menu = hwnd && overlay_update(hwnd, p.buttons, p.ly, f1);
    g_menu_open = menu;
    if (menu) p = PadState{};
    const bool capture = mouse_cam && focus && !menu;  // F1 menu open / window in the background: the pointer is free
    if (capture != SDL_GetWindowRelativeMouseMode(g_window)) SDL_SetWindowRelativeMouseMode(g_window, capture);
    p.buttons |= overlay_buttons();
    std::lock_guard<std::mutex> l(g_ui_lock);
    g_pad = p;
}

void ui_set_fullscreen(bool on) { SDL_SetWindowFullscreen(g_window, on); }

// The game's 60 Hz vsync clock (nn::vi vsync events, pace_present). Its own thread on an absolute schedule with a
// spin wait (see below), and SDL event pumping cannot delay it.
// The swapchain does not block (mailbox, vk.cpp), so this clock alone sets the frame times.
static void vsync_thread() {
    // Windows 11 power throttling (EcoQoS) coalesces timers and slows threads of a window that is not in front: the
    // clock fired up to 36 ms late. Opt the process and this thread out (no-op on older Windows).
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE pp{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                      PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
    BOOL ok1 = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pp, sizeof pp);
    THREAD_POWER_THROTTLING_STATE tp{THREAD_POWER_THROTTLING_CURRENT_VERSION, THREAD_POWER_THROTTLING_EXECUTION_SPEED, 0};
    BOOL ok2 = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof tp);
    log("vsync: power throttling opt-out process %d thread %d (error %lu), host thread %lu", ok1, ok2, GetLastError(), GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
    const uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t base = SDL_GetPerformanceCounter(), k = 0;  // tick k is due at base + k * freq / (60 * speed), no drift
    int speed = g_speed;
    for (;;) {
        if (g_speed != speed) { speed = g_speed; base = SDL_GetPerformanceCounter(); k = 0; }
        uint64_t next = base + (uint64_t)((double)++k * freq / (60.0 * speed));
        uint64_t now = SDL_GetPerformanceCounter();
        if (now > next + freq / 10) { base = now; k = 0; next = now; }  // fell far behind (debugger, sleep): resync
        if (next > now) {
            // Spin (one core busy, ticks exact to ~0.1 ms, measured). Sleeping is unreliable: with the window in the
            // background Windows ignores the 1 ms timer resolution and the high-resolution waitable timer and Sleep(1)
            // both overslept by up to 20-36 ms (measured 2026-10-23), even with power throttling turned off.
            // Sleep(1) until 3 ms before the tick, then spin: saves the core, fine in the foreground. Default at fps 0 / > 60,
            // where this clock does not pace frames: the full spin at time-critical priority slowed the game and render
            // threads whenever it shared a physical core with one (s83: Shibuya 1% low 84 -> 107 fps without it).
            // NX_VSYNC_SPIN=1 / 0 forces the full spin / the sleep at every fps.
            // s115 (Augs, i5-8500 6 cores / 6 threads, "high CPU even when idle"): at 30 / 60 with the window in front,
            // a high-resolution waitable timer (Windows 10 1803+) sleeps until 0.5 ms before the tick, then the spin.
            // Background or no such timer: the full spin as before.
            static const char* const spin_env = getenv("NX_VSYNC_SPIN");
            const int fps = rt::fps_setting();
            bool spin = spin_env ? strcmp(spin_env, "0") != 0 : fps == 30 || fps == 60;
#ifdef _WIN32
            uint64_t margin = freq * 3 / 1000;
            bool timed = false;  // the waitable timer slept: spin the rest (a Sleep(1) there could overshoot the tick)
#ifdef _WIN32
            static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            DWORD fg_pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
            const bool fg = fg_pid == GetCurrentProcessId();
            static bool was_fg = false;
            static int told = 0;
            // USER 11-27: the game lagged and stuttered whenever its window was not in front. The full spin at time-critical
            // priority took a whole core from the game / render / job threads (6-thread CPUs; 2-core test runs: crosswalk
            // 60 -> 45 fps). Behind, the timer now wakes 2 ms early (its background oversleep) and the spin does the rest.
            if (fg != was_fg && spin && !spin_env && told < 8) told++, log("vsync: window %s: %s", fg ? "in front" : "behind", timer ? (fg ? "timer + spin" : "timer + 2 ms spin") : "full spin");
            was_fg = fg;
            if (timer && spin && !spin_env) {
                timed = true;
                margin = fg ? freq / 2000 : freq / 500;
                const uint64_t now2 = SDL_GetPerformanceCounter();
                if (now2 + margin < next) {
                    LARGE_INTEGER due;
                    due.QuadPart = -(LONGLONG)((next - margin - now2) * 10000000 / freq);  // relative, 100 ns units
                    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
                }
            }
#endif
            while (!spin && !timed && SDL_GetPerformanceCounter() + margin < next) Sleep(1);
            while (SDL_GetPerformanceCounter() < next) YieldProcessor();
#else
            // Linux sleeps to ~0.1 ms (measured on the ROG Ally): sleep until 0.3 ms before the tick, spin the rest. The
            // full spin kept a core busy, which on a handheld lowers the boost clock of the cores the game runs on.
            // NX_VSYNC_SPIN=1: the full spin as on Windows.
            (void)spin;
            const uint64_t margin = freq * 3 / 10000;
            const bool full_spin = spin_env && strcmp(spin_env, "0") != 0;
            if (!full_spin && SDL_GetPerformanceCounter() + margin < next)
                std::this_thread::sleep_for(std::chrono::nanoseconds((next - margin - SDL_GetPerformanceCounter()) * 1000000000ull / freq));
            while (SDL_GetPerformanceCounter() < next) YieldProcessor();
#endif
        }
        uint64_t t0 = SDL_GetPerformanceCounter();
        std::vector<uint32_t> ev;
        {
            std::lock_guard<std::mutex> l(g_ui_lock);
            g_vsync_count++;
            ev = g_vsync_events;
        }
        uint64_t t1 = SDL_GetPerformanceCounter();
        g_vsync_cv.notify_all();
        for (uint32_t h : ev) kevent_signal(h);
        uint64_t t2 = SDL_GetPerformanceCounter();
        {  // clock health every 600 ticks: how late ticks fired, time in the ui lock / kernel event signal
            static double late = 0, lock = 0, sig = 0;
            static uint32_t n = 0;
            auto ms = [&](uint64_t a, uint64_t b) { return (double)(b - a) * 1000.0 / freq; };
            late = std::max(late, t0 > next ? ms(next, t0) : 0.0);
            lock = std::max(lock, ms(t0, t1));
            sig = std::max(sig, ms(t1, t2));
            if (++n == 600) {
                log("vsync clock: max late %.2f ms, ui lock %.2f ms, event signal %.2f ms", late, lock, sig);
                n = 0; late = lock = sig = 0;
            }
        }
    }
}

static void ui_thread() {

    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // the exe icon (resource 1) when built with one
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    // DS4Windows: SDL sees only XInput pads (DS4Windows' virtual Xbox pad). v0.1.2 turned off only SDL's HIDAPI
    // PS4 / PS5 drivers, but SDL still opened the real DualShock through RawInput / DirectInput / Windows.Gaming.Input
    // and fought DS4Windows for it: the pad dropped and came back, the lightbar flashed red (Discord report 11-25).
    // ds4windows_xinput_only = 0 (NX_DS4WINDOWS_XINPUT_ONLY) keeps the v0.1.2 behaviour (DS4Windows set to DS4 output).
    static const bool ds4_xinput_only = !getenv("NX_DS4WINDOWS_XINPUT_ONLY") || atoi(getenv("NX_DS4WINDOWS_XINPUT_ONLY")) != 0;
    if (ds4windows_running()) {  // see ds4windows_running
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "0");
        if (ds4_xinput_only) {
            SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
            SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
        }
        log("input: DS4Windows is running: %s", ds4_xinput_only ? "only XInput pads (its virtual pad); the real pad is left to it"
                                                                 : "PlayStation pads left to it (ds4windows_xinput_only = 0)");
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) fatal("SDL_Init: %s", SDL_GetError());
    g_window = SDL_CreateWindow("Tokyo Mirage Sessions #FE Encore (TMSRecomp)", (int)lround(1280 * vk::wide_factor()), 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);  // ultrawide: the picture's shape
    if (!g_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    // <data>/icon.ico: the launcher makes it from the player's own game (NSP icon or title screen); none = default icon
#ifdef _WIN32
    if (HWND hw = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) {
        const std::string ico = g_data_dir + "/icon.ico";
        HICON big = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
        HICON sml = (HICON)LoadImageA(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
        if (big) SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (sml) SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    }
#endif
    // NX_WINDOW=borderless: borderless fullscreen on the window's display (play.bat default); Alt+Enter or F11 toggles
    if (const char* w = getenv("NX_WINDOW"); w && !strcmp(w, "borderless")) SDL_SetWindowFullscreen(g_window, true);
    // s125 (Augs: "it starts as a background process, I have to click the window to bring it to the front"): the
    // launcher has closed by the time this window exists, so Windows' foreground lock kept it behind. The launcher grants
    // the right (AllowSetForegroundWindow); take it here. Still refused: Windows flashes the taskbar button instead.
    SDL_RaiseWindow(g_window);
#ifdef _WIN32
    if (HWND hw = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        hw && GetForegroundWindow() != hw) {
        SetForegroundWindow(hw);
        if (GetForegroundWindow() != hw) {  // started without the launcher's grant: join the foreground thread's input once
            const DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
            if (fg && fg != me && AttachThreadInput(me, fg, TRUE)) {
                BringWindowToTop(hw), SetForegroundWindow(hw);
                AttachThreadInput(me, fg, FALSE);
            }
        }
        if (GetForegroundWindow() != hw) log("ui: Windows kept the game window in the background (click it to play)");
    }
#endif
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        g_ui_ready = true;
    }
    g_ui_ready_cv.notify_all();
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window)))
        log("ui: display %dx%d @ %.3f Hz", m->w, m->h, m->refresh_rate), g_refresh_hz = (int)(m->refresh_rate + 0.5f);
    std::thread(vsync_thread).detach();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                log("window closed");
                fflush(nullptr);
#ifdef _WIN32
                ExitProcess(0);
#else
                _exit(0);  // as ExitProcess: no static destructors while the game threads still run
#endif
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) log("window: %d x %d pixels", e.window.data1, e.window.data2), window_resized();
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F12) g_user_shot++;
            if (e.type == SDL_EVENT_MOUSE_MOTION) g_mouse_acc[0] += e.motion.xrel, g_mouse_acc[1] += e.motion.yrel;
            if (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y != 0) g_wheel_until[e.wheel.y > 0 ? 0 : 1] = (int64_t)SDL_GetTicks() + 80;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {  // no mouse cursor with a pad
                int n = 0;
                SDL_free(SDL_GetGamepads(&n));
                n ? SDL_HideCursor() : SDL_ShowCursor();
            }
        }
        for (; g_fs_request > 0; g_fs_request--) SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
        poll_input();
        Sleep(1);
    }
}

void ui_start() {
    std::call_once(g_ui_once, [] { std::thread(ui_thread).detach(); });
    std::unique_lock<std::mutex> l(g_ui_lock);
    g_ui_ready_cv.wait(l, [] { return g_ui_ready; });
}

SDL_Window* ui_window() {
    ui_start();
    return g_window;
}

}  // namespace rt

using namespace rt;

// ------------------------------------------------------------------------------------------------ nn::vi
HLE_FN(vi_Initialize, "_ZN2nn2vi10InitializeEv") { ui_start(); }
HLE_FN(vi_Finalize, "_ZN2nn2vi8FinalizeEv") {}
HLE_FN(vi_OpenDisplay, "_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE") {
    log("vi: OpenDefaultDisplay");
    st32(c->r[0], hle_alloc(64));
    c->r[0] = 0;
}
HLE_FN(vi_CloseDisplay, "_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE") {}
HLE_FN(vi_CreateLayer, "_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE") {
    st32(c->r[0], hle_alloc(64));
    c->r[0] = 0;
}
HLE_FN(vi_DestroyLayer, "_ZN2nn2vi12DestroyLayerEPNS0_5LayerE") {}
HLE_FN(vi_GetNativeWindow, "_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE") {
    st32(c->r[0], hle_alloc(64));  // opaque to the game, handed to nvnWindowBuilderSetNativeWindow
    c->r[0] = 0;
}
std::atomic<uint32_t> rt::g_vsync_guest_event{0};  // the game's vsync SystemEventType (hle_sys.cpp boot abort guard)
HLE_FN(vi_VsyncEvent, "_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE") {
    uint32_t ev = c->r[0], readable;
    rt::g_vsync_guest_event = ev;
    log("vi: vsync event -> %08x", ev);
    uint32_t w = kevent_create(&readable);
    attach_system_event(c, ev, readable, true);
    log("vi: vsync event %08x state %u", ev, ld8(ev + 0x1c));
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        g_vsync_events.push_back(w);
    }
    c->r[0] = 0;
}

// ------------------------------------------------------------------------------------------------ nn::hid
static uint64_t g_sampling = 0, g_press_read = ~0ull;  // pad reads so far; the last read with a new button press
std::atomic<uint64_t> rt::g_pad_game_buttons{0};
bool rt::pad_pressed_now() { return g_press_read == g_sampling; }
HLE_FN(hid_InitNpad, "_ZN2nn3hid14InitializeNpadEv") { ui_start(); }
HLE_FN(hid_SetStyles, "_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {}
HLE_FN(hid_SetIds, "_ZN2nn3hid22SetSupportedNpadIdTypeEPKjj") {}
HLE_FN(hid_SetHold, "_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE") {}
HLE_FN(hid_ShowSupport, "_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE") { c->r[0] = 0; }
HLE_FN(hid_GetStyle, "_ZN2nn3hid15GetNpadStyleSetERKj") {
    c->r[0] = ld32(c->r[0]) == 0 ? 1u : 0u;  // player 1 = full key (Pro Controller); nothing else connected
}
// Input record / replay (milestone 3), keyed by the game's pad reads (one per game frame), not by wall time, so a replay
// follows the game's own pace. NX_RECORD=<file>: one line per change, "<read> <buttons hex> <lx> <ly> <rx> <ry>".
// NX_REPLAY=<file>: the game reads the recorded state instead of live input until the file ends, then live input
// resumes. Reads during loading depend on load speed, so long replays can drift (measure before relying on one).
static void replay_record(uint64_t n, PadState& p) {
    struct Entry { uint64_t n; PadState p; };
    static std::vector<Entry> replay = [] {
        std::vector<Entry> v;
        const char* f = getenv("NX_REPLAY");
        FILE* fp = f ? fopen(f, "r") : nullptr;
        if (f && !fp) log("input: cannot open replay %s", f);
        if (!fp) return v;
        Entry e;
        unsigned long long b;
        while (fscanf(fp, "%llu %llx %d %d %d %d", &e.n, &b, &e.p.lx, &e.p.ly, &e.p.rx, &e.p.ry) == 6) {
            e.p.buttons = b;
            v.push_back(e);
        }
        fclose(fp);
        log("input: replaying %zu changes from %s (last at read %llu)", v.size(), f, v.empty() ? 0ull : (unsigned long long)v.back().n);
        return v;
    }();
    static size_t next = 0;
    static PadState cur;
    if (next < replay.size() || (!replay.empty() && n == replay.back().n + 1)) {
        while (next < replay.size() && replay[next].n <= n) cur = replay[next++].p;
        if (n % 300 == 0) log("input: replay at read %llu", (unsigned long long)n);  // for cutting a replay short
        if (next == replay.size() && n > replay.back().n) log("input: replay finished at read %llu, live input from here", (unsigned long long)n);
        else p = cur;
    }
    static FILE* rec = [] {
        const char* f = getenv("NX_RECORD");
        FILE* fp = f ? fopen(f, "w") : nullptr;
        if (f) log(fp ? "input: recording to %s" : "input: cannot write %s", f);
        return fp;
    }();
    static PadState last{~0ull};
    if (rec && (p.buttons != last.buttons || p.lx != last.lx || p.ly != last.ly || p.rx != last.rx || p.ry != last.ry)) {
        fprintf(rec, "%llu %llx %d %d %d %d\n", (unsigned long long)n, (unsigned long long)p.buttons, p.lx, p.ly, p.rx, p.ry);
        fflush(rec);  // survives a crash
        last = p;
    }
}

static void npad_state(Cpu* c, bool connected) {
    uint32_t out = c->r[0];
    memset(GPTR(out), 0, 0x28);
    if (!connected) return;
    PadState p;
    {
        std::lock_guard<std::mutex> l(g_ui_lock);
        p = g_pad;
    }
    replay_record(++g_sampling, p);
    static uint64_t prev = 0;  // a new press at this read: UI widgets update now even between 30 Hz ticks (hle_sys.cpp)
    if (p.buttons & ~prev) g_press_read = g_sampling;
    prev = p.buttons;
    st64(out, g_sampling);
    st64(out + 8, p.buttons);
    g_pad_game_buttons = p.buttons;
    st32(out + 16, (uint32_t)p.lx);
    st32(out + 20, (uint32_t)p.ly);
    st32(out + 24, (uint32_t)p.rx);
    st32(out + 28, (uint32_t)p.ry);
    st32(out + 32, 0x3);  // IsConnected | IsWired
}
HLE_FN(hid_FullKey, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj") { npad_state(c, ld32(c->r[1]) == 0); }
HLE_FN(hid_Handheld, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyDual, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyLeft, "_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj") { npad_state(c, false); }
HLE_FN(hid_JoyRight, "_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj") { npad_state(c, false); }
HLE_FN(hid_InitTouch, "_ZN2nn3hid21InitializeTouchScreenEv") {}
HLE_FN(hid_Touch, "_ZN2nn3hid19GetTouchScreenStateILj16EEEvPNS0_16TouchScreenStateIXT_EEE") {
    memset(GPTR(c->r[0]), 0, 16);  // sampling number, count 0
}
HLE_FN(hid_SixAxisHandles, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    for (uint32_t i = 0; i < c->r[1]; i++) st32(c->r[0] + 4 * i, 0x5A000000u | i);
    c->r[0] = c->r[1];
}
HLE_FN(hid_SixAxisState, "_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE") {
    memset(GPTR(c->r[0]), 0, 0x60);
}
HLE_FN(hid_VibHandles, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE") {
    for (uint32_t i = 0; i < c->r[1]; i++) st32(c->r[0] + 4 * i, 0x56000000u | i);
    c->r[0] = c->r[1];
}
HLE_FN(hid_VibInit, "_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE") {}
HLE_FN(hid_VibSend, "_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE") {}
