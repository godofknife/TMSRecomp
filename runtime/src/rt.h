// Runtime internals shared by the host-side sources.
#pragma once
#include "guest.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <initializer_list>
#include <unordered_map>
#include <array>
#include <vector>

namespace rt {

// ------------------------------------------------------------------------------------------------ logging
void log(const char* fmt, ...);
void perfmon_start(const char* gpu_name, uint32_t vendor);
int cpk_dump(const std::string& data, bool all, const std::string& only);  // hle_fs.cpp: tms.exe --dump [--all | --pack <name>]: game packs -> <data>/dump  // perfmon.cpp: "perf:" lines (GPU clocks / heat / power), s124
extern std::atomic<bool> g_perf_in_game;                    // perfmon.cpp: a 3D frame since its last sample
[[noreturn]] void fatal(const char* fmt, ...);
extern int g_verbose;
#define LOGV(...) do { if (rt::g_verbose) rt::log(__VA_ARGS__); } while (0)

// ------------------------------------------------------------------------------------------------ guest memory
// 32-bit address space layout (ours; the SDK asks the kernel via GetInfo)
constexpr uint32_t IMAGE_BASE = 0x00200000;
constexpr uint32_t ASLR_BASE = 0x00200000, ASLR_SIZE = 0x3FE00000;
constexpr uint32_t HEAP_BASE = 0x40000000, HEAP_REGION_SIZE = 0x80000000;
constexpr uint32_t ALIAS_BASE = 0xC0000000, ALIAS_SIZE = 0x10000000;
constexpr uint32_t STACK_BASE = 0xD0000000, STACK_SIZE = 0x10000000;
constexpr uint32_t KERNEL_BASE = 0xE0000000, KERNEL_SIZE = 0x10000000;  // TLRs, main stack (kernel-owned)
constexpr uint32_t HLE_BASE = 0xF0000000, HLE_SIZE = 0x0FFF0000;        // our own guest-visible allocations

enum MemState : uint32_t { MS_FREE = 0, MS_IO = 1, MS_STATIC = 2, MS_CODE = 3, MS_CODEDATA = 4, MS_NORMAL = 5,
                           MS_SHARED = 6, MS_ALIAS = 7, MS_IPC = 0xA, MS_STACK = 0xB, MS_TLS = 0xC,
                           MS_TRANSFER = 0xD, MS_INACCESSIBLE = 0x10 };
struct Region { uint32_t addr, size, state, perm, attr; };

void mem_init();
void mem_map(uint32_t addr, uint32_t size, uint32_t state, uint32_t perm);  // commit + record
void mem_unmap(uint32_t addr, uint32_t size);
Region mem_query(uint32_t addr);
double process_private_mb(), guest_committed_mb();  // memory.cpp
void mem_regions_report();  // memory.cpp, NX_MEM_REGIONS
// s126 write ticks (vertex data in video memory): mem_write_tick harvests the 2 MB chunks under the range once per stamp
// and returns the current tick; mem_written_tick = the newest tick any page of the range was seen written at (0 never).
// Linux: unsupported (mem_write_ticks_ok false).
bool mem_write_ticks_ok();
uint32_t mem_write_tick(uint32_t addr, uint32_t size, uint64_t stamp);
uint32_t mem_written_tick(uint32_t addr, uint32_t size);
bool mem_written(uint32_t addr, uint32_t size);  // CPU wrote the range since the last call (write watch)
void mem_next_frame();  // Linux: a new frame for the imported-page checks (no-op on Windows)
// the same, but the write watch of [span_lo, span_lo + span_len) (the texture's memory pool) is read once per stamp (submit)
bool mem_written_span(uint32_t addr, uint32_t size, uint32_t span_lo, uint32_t span_len, uint64_t stamp);
void mem_make_writable(uint32_t addr, uint32_t size);  // Linux: open watched pages before a driver pins them (no-op on Windows)
#ifndef _WIN32
bool mem_fault(void* host_addr);  // Linux SIGSEGV handler: a write to a watched page (true: it is open again, retry)
#endif
uint32_t kernel_alloc(uint32_t size, uint32_t align = 0x1000);  // kernel-owned pages (TLR, stacks)
uint32_t hle_alloc(uint32_t size, uint32_t align = 16);         // HLE scratch visible to the guest
uint32_t heap_set_size(uint32_t size);                          // svcSetHeapSize
std::string gstr(uint32_t a, size_t max = 4096);
inline void* hptr(uint32_t a) { return GPTR(a); }
inline uint32_t arg(Cpu* c, int i) { return i < 4 ? c->r[i] : ld32(c->r[13] + 4u * (i - 4)); }

// ------------------------------------------------------------------------------------------------ functions
void game_load(const std::filesystem::path& dir);  // game.dll next to tms.exe: recompiled code + tables
extern const GuestModule* g_modules;
extern uint32_t g_module_count, g_image_size;
void dispatch_init();
GuestFn lookup(uint32_t addr);
const char* symbol_at(uint32_t addr);  // nearest exported symbol at or below addr (for logs)
uint32_t export_addr(const char* name);  // 0 if missing
void cover_report();  // NX_COVER: log probed functions called since the last report
void flag_command(const char* args);  // NX_CONTROL "flag n [0|1]": log or set game flag n
void pos_command(const char* set = nullptr);  // NX_CONTROL "pos": log the player position; "setpos x y z": move it
int game_flag(uint32_t id);  // hle_sys.cpp: story / system flag bit (0 / 1), -1 = flags not loaded
const char* language();  // hle_sys.cpp: setting `language` as the Switch's code (en-US, ja, fr, zh-Hans, zh-Hant)
extern std::atomic<uint32_t> g_vsync_guest_event;  // ui.cpp vi_VsyncEvent: the game's vsync event (boot abort guard)
void profile_start(bool submit = false);       // main.cpp: NX_PROFILE=1 sampling profiler of the calling (game) thread
std::string guest_chain();  // main.cpp: guest call chain (generated functions on the host stack) for logs
bool pad_pressed_now();  // ui.cpp: the latest pad read had a new button press
void ui_set_fullscreen(bool on);  // ui.cpp: borderless fullscreen on / window
// overlay.cpp: in-game settings menu (F1, L3 + R3) and the title screen exit. The UI thread passes the host pad state;
// true = a menu has the input (the game gets no buttons).
bool overlay_update(void* hwnd, uint64_t buttons, int ly, bool f1);
void overlay_title_seen();  // hle_sys.cpp: the title scene updated this frame
void overlay_camp_seen();   // hle_sys.cpp: the camp menu updated this frame
bool overlay_take_title_request();  // hle_sys.cpp field_play_update: the user chose Return to Title (once)
void overlay_roam_seen(bool town);  // hle_sys.cpp: the field free-roam state ran this frame (town = Tokyo Map allowed)
void overlay_request_map();         // F1 menu "Open Tokyo Map" / NX_CONTROL "lmap"
bool overlay_take_map_request();    // hle_sys.cpp field_roam_update: open the Tokyo Map (once)
// L1 shop fast travel (hle_sys.cpp field_roam_update owns the shop list and the game-side checks)
int travel_shop_count();
const wchar_t* travel_shop_name(int i);
const wchar_t* travel_shop_help(int i);
uint32_t travel_shops_offered();   // bit i: shop i can be picked now (town free roam, shop visited, Shibuya open)
void overlay_request_travel(int i);  // L1 menu / NX_CONTROL "travel n"
int overlay_take_travel_request();   // hle_sys.cpp field_roam_update: shop index (once), -1 = none
uint64_t overlay_buttons();  // ui.cpp poll_input: buttons the overlay adds to the pad (closing the camp menu for Return to Title)
// camp_settings.cpp (the PC page of the game's Settings screen) uses the overlay's items by name: UTF-8 texts, the
// current choice, and apply (same effect as choosing it in the F1 menu). False: no item of that name.
struct SettingItem { std::string name, help; std::vector<std::string> choices; int cur = 0; };
bool overlay_setting(const wchar_t* name, SettingItem& out);
void overlay_apply_setting(const wchar_t* name, int choice);
extern std::atomic<uint64_t> g_pad_game_buttons;  // ui.cpp: the buttons of the game's latest pad read
void save_setting(const char* key, const std::string& value);  // settings.ini "key = value" + NX_KEY
enum { FX_BLOOM, FX_GLOW, FX_DOF, FX_VIGNETTE, FX_FOG, FX_OUTLINE, FX_FXAA, FX_CHAR_RIM, FX_COUNT };
bool fx_enabled(int fx);  // NX_FX_BLOOM=0 etc. or the menu turned that post effect off
int upscaler_setting();  // overlay.cpp: setting upscaler (0 bilinear, 1 FSR 1 style edge-adaptive)
void hero_capture_soon();  // vk.cpp: title screen up: save <data>/launcher_hero.bmp once (launcher picture)
void apply_fsr3_quality(int i);  // overlay.cpp: upscaling mode 0..4, live (also control "upq N")
void apply_msaa(int i);          // overlay.cpp: MSAA row index 0 off, 1 2x, 2 4x, 3 8x (also control "msaa I")
void apply_aa(int i);   // overlay.cpp: Anti-aliasing row: 0 off, 1 FXAA, 2 SMAA, 3 FSR 3, 4 DLSS (also control "aa N")
int msaa_setting();     // overlay.cpp: setting msaa (0 off, 2 / 4 / 8 samples for the 3D scene passes)
int fsr3_setting();     // overlay.cpp: 1 = Anti-aliasing FSR 3 (setting fx_fxaa 3; vk_replay.cpp fsr3_in_place)
int fsr3_quality();     // overlay.cpp: setting fsr3_quality 0 native AA, 1 Quality .. 4 Ultra Performance (live)
int scene_res();       // overlay.cpp: setting scene_res, 3D resolution % with AA off / FXAA / SMAA (100 = off, live)
void apply_scene_res(int i);  // F1 row index 0..4
int smaa_setting();     // overlay.cpp: setting smaa (1: SMAA in place of the game's FXAA)
int hdr_setting();         // overlay.cpp: setting hdr (0 / 1), HDR10 / scRGB output when the screen offers it (vk.cpp)
void set_hdr(int on);      // overlay.cpp: hdr on / off now, not saved (NX_CONTROL "hdr n")
int hdr_paper_white();     // overlay.cpp: setting hdr_paper_white, nits of SDR white in HDR
int hdr_peak_setting();    // overlay.cpp: setting hdr_peak, nits, 0 = auto (the screen's own)
int hdr_boost();          // overlay.cpp: setting hdr_boost 0..4, inverse tone mapping strength (3D scene only)
int hdr_color();          // overlay.cpp: setting hdr_color 0..4, extra saturation in the 3D scene with HDR
int sharpen_setting();    // overlay.cpp: setting sharpen (0 off .. 100), the present path's sharpening pass
// pad_icons.cpp: button icons by controller (setting pad_icons auto|switch|playstation|xbox)
extern std::atomic<int> g_pad_kind;  // ui.cpp sets it for the open pad: 0 Switch / other, 1 PlayStation, 2 Xbox
int pad_icons_setting();             // 0 auto, 1 switch, 2 playstation, 3 xbox
int pad_face_pos(int k);              // ui.cpp: the pad face button that presses Switch A/B/X/Y (k 0..3) as 0 east, 1 south, 2 north, 3 west
// P5-style battle buttons (setting battle_buttons, s112): hle_sys.cpp btl_wheel_input <-> ui.cpp poll_input
extern std::atomic<int64_t> g_wheel_ms;       // GetTickCount64 ms of the last command wheel input call
extern std::atomic<int64_t> g_wheel_open_ms;  // ... of the last frame the wheel was open (also before / between input calls)
extern std::atomic<int64_t> g_wheel_blocked_ms;  // ... a screen over the wheel (tutorial info window, Stats): no panel, buttons to the game
bool p5_wheel_up(int64_t now, bool input);  // p5ui.cpp: the wheel is open (input: and taking input) with nothing over it
extern std::atomic<int> g_wheel_sel;          // the wheel's selection at its last input call
extern std::atomic<int> g_p5_target;          // wheel index poll_input is turning to and confirming (-1 none)
extern std::atomic<uint32_t> g_wheel_ids;     // the wheel's commands: nibble i = command id at index i + 1 (0 = end)
int battle_buttons_mode();                                    // p5ui.cpp: setting battle_buttons 0 / 1 / 2
void set_battle_buttons_mode(int m);                          // F1 menu: applies at once (also mid-battle)
bool p5_panel_active();                                       // our P5 panel replaces the game's wheel now
std::shared_ptr<const struct SubsBitmap> p5_panel(uint32_t fw, uint32_t fh);  // the panel to blend over the frame, or null
bool button_layout_xbox();           // setting button_layout = xbox: bottom face button = A (confirm), left = X
void set_pad_icons(int s);           // menu: saves the setting
int pad_icon_style();                // icons to show now: 0 Switch, 1 PlayStation, 2 Xbox
bool pad_icon_atlas_size(uint32_t w, uint32_t h);  // some atlas with button faces has this size (hash it)
bool is_pad_icon_atlas(uint64_t hash);              // level-0 hash (vk_texture.cpp tex_hash) of such an atlas (_US)
bool pad_icon_atlas(int style, uint64_t hash, uint32_t w, uint32_t h, uint8_t* rgba);
std::vector<std::array<int, 4>> pad_icon_cells(uint64_t hash, uint32_t w, uint32_t h, int scale);  // HD atlas: cell rects at scale
bool pad_icon_paint_region(int style, uint64_t hash, uint32_t w, uint32_t h, int scale, int x0, int y0, int rw, int rh, uint8_t* rgba);  // draws that style over it
bool tex_pack_on();          // vk_texture.cpp: UHD texture pack in use (setting tex_pack / NX_TEX_PACK)
void set_tex_pack(bool on);  // switches it live (textures with a pack file upload again) and saves the setting
bool is_pad_prompt(uint64_t hash);  // a field interaction prompt pill ("(A) Talk"), 512 x 128 BC3
bool pad_icon_prompt(int style, uint32_t w, uint32_t h, uint8_t* rgba);
bool pad_prompt_disc(uint32_t w, uint32_t h, const uint8_t* rgba, std::array<int, 4>& rect, uint32_t& fill);  // HD prompts
bool pad_prompt_paint(int style, std::array<int, 4> rect, uint32_t fill, int rw, int rh, uint8_t* rgba);  // finds its (A) disc, draws that style
int pad_icons_selftest(const char* dump_dir);  // NX_PAD_ICONS_TEST=<dir>: draw every atlas found there, exit
bool resist_icons_on();          // pad_icons.cpp: resistance badges drawn as icons (setting resist_icons)
void set_resist_icons(bool on);  // menu: switches live + saves
// hle_movie.cpp: 4K replacement movies (tools/camphd). movie_fp = sampled hash of a plane; movie_hd_plane finds the HD
// plane of a recently decoded frame by the fingerprint of its low-res plane (nv12 + off, w x h texels)
uint64_t movie_fp(const uint8_t* p, uint32_t row, uint32_t rows, uint32_t stride);
bool movie_hd_any();
bool movie_hd_plane(uint64_t fp, std::shared_ptr<std::vector<uint8_t>>* nv12, size_t* off, uint32_t* w, uint32_t* h);
bool is_resist_atlas(uint64_t hash);  // level-0 hash of an atlas with the badge letters
bool resist_icon_atlas(uint64_t hash, uint32_t w, uint32_t h, uint8_t* rgba, bool icons);  // draws the icons (and the RESIST popup) over it
bool resist_icon_cell(uint64_t hash, float u, float v);  // texture coordinate inside one of that atlas's icon cells
// subs.cpp: battle voice subtitles (setting voice_subs). subs_voice: the game set a voice file to play (game log);
// subs_frame (present path): the bitmap to blend over a frame of that size now, null = none.
struct SubsBitmap { uint32_t x, y, w, h; std::vector<uint32_t> rgba; };  // premultiplied, R in the low byte
void subs_voice(const char* path);
std::shared_ptr<const SubsBitmap> subs_frame(uint32_t fw, uint32_t fh);
bool subs_enabled();
// what the present path blends over a frame of that size: the subtitles (Linux: and the menu, drawn into the frame)
std::shared_ptr<const SubsBitmap> frame_overlay(uint32_t fw, uint32_t fh);
void set_subs_enabled(bool on);
void subs_notice(const std::wstring& text);  // a short line of ours in the subtitle spot (1.5 s)
constexpr uint64_t k_button_atlas_hash = 0x34c763427410e392ull;  // the common button atlas
void sprite_command(const char* args);  // NX_CONTROL "spr bank sheet n": log 2D sprite records (main.cpp)
void dump_command(const char* args);   // NX_CONTROL "dump lo hi file": write committed guest memory lo..hi to file (main.cpp)
extern std::atomic<uint32_t> g_uidraw30;  // debug: btl_ui_draw part draws on 30 Hz ticks (hle_sys.cpp)
void task30_command(const char* args);  // NX_CONTROL "task30 <update fn hex>" (0 = clear): debug, tasks with that update step on 30 Hz ticks (hle_sys.cpp)
void watch_command(const char* args);  // NX_CONTROL "watch addr len [r]": log the code touching guest memory (main.cpp)
extern std::atomic<uint64_t> g_pace_tick;         // ui.cpp: vsync tick the last frame was released at
void wait_vsync_tick(uint64_t tick);              // ui.cpp: block until the vsync clock reaches tick
extern std::atomic<uint64_t> g_late_vsyncs;        // ui.cpp: vsyncs missed by late frames
extern std::atomic<uint32_t> g_frame_work_max_us;  // ui.cpp: longest game frame between paces
extern std::atomic<uint64_t> g_pace_us;            // ui.cpp: microseconds inside pace_present since the last frame stats line
extern std::atomic<uint64_t> g_frame_no;           // ui.cpp: pace_present calls
void call_guest(Cpu* c, uint32_t addr);   // r14 = sentinel, dispatch
int fps_setting();                          // hle_sys.cpp: NX_FPS (30, 60, N > 60 capped, 0 uncapped)
void set_fps(int n);                        // control "fps N" (60 / N > 60 / 0 only)
void auto30_new_area();                    // ui.cpp: a new field area loaded: Auto 30 tries 60 again soon
bool auto30_active();                      // ui.cpp: Auto 30 is holding 30 now
bool auto30_setting();                      // ui.cpp: setting auto_30 (default 1): at fps 60, drop to 30 while 60 is not held
void set_auto30(bool on);                   // settings menu: applies at once
extern std::atomic<uint32_t> g_dt_keep;      // hle_sys.cpp: debug control "dtkeep mask" (dt literals kept at 1/30)
extern std::atomic<int> g_ui_gate;          // hle_sys.cpp: menu/HUD 30 Hz gates (control "uigate N")
float walk_speed();                         // hle_sys.cpp: NX_WALK_SPEED (field walk/run x k)
void set_walk_speed(float k);               // settings menu: applies at once
float game_base_dt();                       // hle_sys.cpp: the step set_game_dt last asked for (perf: game speed)
void window_resized();                      // vk.cpp: the window's pixel size changed (ui.cpp event): rebuild the swapchain
void set_game_dt(float dt);                 // hle_sys.cpp: the game's per-frame time step in seconds
extern std::atomic<int> g_shadow_detail;    // vk_replay.cpp: setting shadow_detail (1: no far shadow map)
extern std::atomic<int> g_skip_shadow;      // vk_replay.cpp: debug, drop shadow casters
bool ui_smooth();                          // hle_sys.cpp: setting ui_smooth: sprites glide between UI ticks above 60 fps
void set_ui_smooth(bool on);
void ui_clock(uint32_t* tick, float* frac);  // current UI tick, time since it in ticks (1 = nothing to interpolate)
extern std::atomic<uint32_t> g_ui_part30, g_ui_freeze, g_gate30;  // hle_sys.cpp: battle HUD parts on 30 Hz ticks, debug freeze
int ui_speed();                            // hle_sys.cpp: setting ui_speed: 30 (Switch speed, default) or 60 Hz UI ticks
void set_ui_speed(int hz);                 // settings menus: applies at once
int battle_speed();                         // hle_sys.cpp: setting battle_speed (1-3): game time x k in battles
void set_battle_speed(int k, bool notice);  // settings menu / F3 / touchpad (cycles): applies at once, saves the setting
int custom_bgm_mode();                      // ext_bgm.cpp: setting random_battle_bgm (0 off, 1 game, 2 custom, 3 mixed)
bool custom_bgm_battle();                   // ext_bgm.cpp: a battle track starts; true = custom music plays instead
bool custom_bgm_victory();                  // ext_bgm.cpp: victory jingle; true = the custom victory theme plays
void custom_bgm_stop(const char* why);      // ext_bgm.cpp: the game's music takes over (1 s fade)
bool custom_bgm_active();
void custom_bgm_mix(int16_t* out, uint32_t frames);  // ext_bgm.cpp: add custom music to 48 kHz stereo s16 frames
bool in_battle();                           // hle_sys.cpp: battle HUD updated in the last 250 ms
void frame_begin();                         // hle_sys.cpp: once per game frame (pace_present): 30 Hz tick clock

using HleFn = void (*)(Cpu*);
uint32_t host_callback(HleFn fn);   // a guest address that runs fn when called
HleFn host_callback_at(uint32_t a);
struct HleReg {
    HleReg(const char* mangled, HleFn fn, bool probe = false);
};
#define HLE_FN(id, mangled) \
    static void id(Cpu* c); \
    static rt::HleReg id##_reg(mangled, id); \
    static void id(Cpu* c)
// observe a names-file function (tools/recomp/names.txt): runs at its entry, must leave the Cpu as it found it
#define PROBE_FN(id, name) \
    static void id(Cpu* c); \
    static rt::HleReg id##_reg(name, id, true); \
    static void id(Cpu* c)

// ------------------------------------------------------------------------------------------------ kernel
struct KObject {
    virtual ~KObject() = default;
    virtual bool signaled() const { return false; }
};
struct KThread : KObject {
    Cpu cpu{};
    uint32_t handle = 0, entry = 0, arg = 0, stack_top = 0, tlr = 0;
    int prio = 44, core = 0;
    uint64_t id = 0;
    std::atomic<bool> started{false}, exited{false}, ran{false};  // ran: made its first svc
    bool cancel = false;
    // wait state (guarded by the kernel lock)
    std::condition_variable cv;
    bool granted = false, cv_signaled = false, addr_woken = false;
    uint32_t self_tag = 0, cv_key = 0, cv_mutex = 0;
    std::unordered_map<uint32_t, GuestJmp> jmps;
    std::string name;
    bool signaled() const override { return exited.load(); }
};
KThread* cur();
void kernel_init();
uint32_t create_main_thread(uint32_t stack_size, int prio);
void run_main_thread(uint32_t entry, const uint32_t args[4]);  // runs on the calling host thread
void svc(Cpu* c, uint32_t imm);

uint32_t kevent_create(uint32_t* readable);  // returns the writable handle
void kevent_signal(uint32_t writable);
// call guest code from HLE: up to 4 register args, returns r0 (callee-saved registers are preserved by the guest)
uint32_t gcall(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args);
uint32_t gcall(Cpu* c, const char* sym, std::initializer_list<uint32_t> args);
// attach a kernel event handle to a guest nn::os::SystemEventType (the SDK's own code then waits on it)
void attach_system_event(Cpu* c, uint32_t sysevent, uint32_t readable, bool autoclear);
extern std::string g_data_dir;
// game speed (NX_SPEED=n or the NX_CONTROL line "speed n"): guest clocks, timeouts, vsync and audio run n times
// faster, for skipping known parts of a test run. 1 = normal.
extern std::atomic<int> g_speed;
void set_speed(int n);
uint32_t call_unhooked(Cpu* c, const char* name, std::initializer_list<uint32_t> args);
void set_r64(Cpu* c, uint64_t v);  // r0:r1

// IPC (services the SDK talks to directly; most of nn:: is hooked at the API level instead)
uint32_t ipc_connect(const char* port);
uint32_t ipc_request(uint32_t handle, uint32_t buf, uint32_t size);
uint32_t ipc_new_session(const std::string& name);

}  // namespace rt
