// In-game settings menu and the title screen's exit, drawn over the game window in a layered popup (per-pixel alpha
// through UpdateLayeredWindow, Direct2D + DirectWrite into a DIB; the swapchain is not touched). The UI thread calls
// overlay_update with the host pad state every loop; while a menu is open the game gets no buttons.
// Open / close: F1, or L3 + R3 together (both sticks pressed in). Title screen: (-) opens "Exit the game?"; camp menu
// (Y in the field): (-) opens Return to Title / Quit Game / Cancel.
#include "plat.h"
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#else
#include <SDL3/SDL.h>
#include "draw_linux.h"
#include <climits>
#include <memory>
#include <mutex>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "rt.h"

namespace vk { float wide_factor(); }

namespace rt {

// ------------------------------------------------------------------------------------------------ settings
static std::filesystem::path ini_path() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path() / "settings.ini";
#else
    return plat::settings_ini();
#endif
}

// Rewrites "key = value" in settings.ini (the first uncommented line with that key, else appended) and NX_KEY.
void save_setting(const char* key, const std::string& value) {
    std::filesystem::path ini = ini_path();
    std::vector<std::string> lines;
    {
        std::ifstream f(ini);
        for (std::string l; std::getline(f, l);) lines.push_back(l);
    }
    bool done = false;
    for (std::string& l : lines) {
        std::string k = l.substr(0, l.find_first_of(";#="));
        k.erase(0, k.find_first_not_of(" \t"));
        k.erase(k.find_last_not_of(" \t\r") + 1);
        if (!done && l.find('=') != std::string::npos && l.find_first_of(";#") > l.find('=') && _stricmp(k.c_str(), key) == 0) {
            l = std::string(key) + " = " + value;
            done = true;
        }
    }
    if (!done) lines.push_back(std::string(key) + " = " + value);
    // replays (agent test runs) change the setting for this run only: a desynced replay that wandered into the
    // settings menu rewrote the user's settings.ini (11-01)
    if (!getenv("NX_REPLAY") || !*getenv("NX_REPLAY")) {
        std::ofstream f(ini, std::ios::trunc);
        for (const std::string& l : lines) f << l << "\n";
    }
    std::string env = "NX_" + std::string(key);
    for (char& ch : env) ch = (char)toupper((unsigned char)ch);
    _putenv_s(env.c_str(), value.c_str());
    log("settings: %s = %s", key, value.c_str());
}

static int env_int(const char* name, int def, int lo, int hi) {
    const char* v = getenv(name);
    return v && *v ? std::clamp(atoi(v), lo, hi) : def;
}
static bool env_on(const char* name, bool def) {
    const char* v = getenv(name);
    return v ? atoi(v) != 0 : def;
}

// Post effects the menu can turn off (hle_sys.cpp skips their render step): bit = FX_* index.
static std::atomic<uint32_t> g_fx_off{~0u};
static const char* const k_fx_keys[FX_COUNT] = {"fx_bloom", "fx_glow", "fx_dof", "fx_vignette", "fx_fog", "fx_outline", "fx_fxaa", "fx_char_rim"};
bool fx_enabled(int fx) {
    if (g_fx_off == ~0u) {
        uint32_t off = 0;
        for (int i = 0; i < FX_COUNT; i++) {
            std::string env = "NX_" + std::string(k_fx_keys[i]);
            for (char& ch : env) ch = (char)toupper((unsigned char)ch);
            if (!env_on(env.c_str(), true)) off |= 1u << i;
        }
        g_fx_off = off;
    }
    return !(g_fx_off >> fx & 1);
}

static std::atomic<int> g_sharpen{-1};
static const int k_sharpen[] = {0, 35, 65, 100};
int sharpen_setting() {
    if (g_sharpen < 0) g_sharpen = getenv("NX_SHARPEN") ? std::clamp(atoi(getenv("NX_SHARPEN")), 0, 100) : 0;
    return g_sharpen;
}
static std::atomic<int> g_upscaler{-1};
int upscaler_setting() {
    if (g_upscaler < 0) {
        const char* e = getenv("NX_UPSCALER");
        // unset / auto = FSR 1 (weak-PC default: res auto renders below the screen there; ~0.3 ms, every GPU)
        g_upscaler = !e || !_stricmp(e, "auto") || !_stricmp(e, "fsr1") || !_stricmp(e, "fsr") || !strcmp(e, "1") ? 1 : 0;
    }
    return g_upscaler;
}
// Anti-aliasing: setting fx_fxaa = 0 off, 1 the game's FXAA, 2 SMAA (vk_replay.cpp runs our SMAA passes where the
// game's FXAA would run and turns FXAA into a pass-through). NX_SMAA=1 also selects SMAA.
static std::atomic<int> g_smaa{-1};
int smaa_setting() {
    if (g_smaa < 0) {
        const char* e = getenv("NX_FX_FXAA");
        g_smaa = env_on("NX_SMAA", false) || (e && atoi(e) == 2) ? 1 : 0;
    }
    return g_smaa;
}
// fx_fxaa = 3: AMD FSR 3 temporal anti-aliasing (native size) at the same place; SMAA when the FSR DLLs are missing
static std::atomic<int> g_fsr3{-1};
int fsr3_setting() {
    if (g_fsr3 < 0) {
        const char* e = getenv("NX_FX_FXAA");
        g_fsr3 = e && atoi(e) == 3 ? 1 : e && atoi(e) == 4 ? 2 : 0;  // 4: NVIDIA DLSS / DLAA (vk_dlss.cpp), FSR 3 when unavailable
    }
    return g_fsr3;
}
// fsr3_quality = FSR 3 / DLSS upscaling: the 3D scene renders below the output size (Anti-aliasing = FSR 3 / DLSS);
// applies at the next frame end (vk.cpp upscale_apply_setting)
static std::atomic<int> g_fsr3q{-1};
int fsr3_quality() {
    if (g_fsr3q < 0) { const char* e = getenv("NX_FSR3_QUALITY"); g_fsr3q = e ? atoi(e) : 0; }
    return g_fsr3q;
}
void apply_fsr3_quality(int i) { save_setting("fsr3_quality", std::to_string(i)); g_fsr3q = i; }
// scene_res = 3D resolution in percent of the output (100 default; 85 / 75 / 67 / 50) with Anti-aliasing Off / FXAA /
// SMAA: the 3D scene renders smaller and is scaled up (linear) before the game's FXAA; menus / HUD stay full size.
// Weak GPUs (#11, GeForce 920MX). FSR 3 / DLSS use fsr3_quality instead. Applies at the next frame end.
// scene_res = auto (0 here): vk.cpp auto_scene_res picks the step from the GPU time of 3D frames.
static std::atomic<int> g_scene_res{-1};
static const int k_scene_res[] = {0, 100, 85, 75, 67, 50};
int scene_res() {
    if (g_scene_res < 0) {
        const char* e = getenv("NX_SCENE_RES");
        g_scene_res = e && !strcmp(e, "auto") ? 0 : e && atoi(e) > 0 ? std::clamp(atoi(e), 50, 100) : 100;
    }
    return g_scene_res;
}
void apply_scene_res(int i) {
    i = k_scene_res[std::clamp(i, 0, 5)];
    save_setting("scene_res", i ? std::to_string(i) : std::string("auto"));
    g_scene_res = i;
}
void apply_aa(int i) {
    save_setting("fx_fxaa", std::to_string(i));
    fx_enabled(0);  // loaded
    if (i) g_fx_off &= ~(1u << FX_FXAA);
    else g_fx_off |= 1u << FX_FXAA;
    g_smaa = i == 2;
    g_fsr3 = i == 3 ? 1 : i == 4 ? 2 : 0;
}
// MSAA (setting msaa 0 / 2 / 4 / 8): vk_replay.cpp MsTwin, applies at once
static std::atomic<int> g_msaa{-1};
static const int k_msaa[] = {0, 2, 4, 8};
int msaa_setting() {
    if (g_msaa < 0) g_msaa = env_int("NX_MSAA", 0, 0, 8);
    return g_msaa;
}
void apply_msaa(int i) {
    save_setting("msaa", std::to_string(k_msaa[i]));
    g_msaa = k_msaa[i];
}
static void apply_upscaler(int i) {
    save_setting("upscaler", i ? "fsr1" : "bilinear");
    g_upscaler = i;
}
// HDR output (setting hdr, off by default): vk.cpp picks an HDR10 / scRGB swapchain when the screen offers one (Windows
// HDR on) and keeps the highlights the game's frame has above SDR white; live. hdr_paper_white: nits of SDR white (UI,
// menus); hdr_peak: the screen's brightest nits, 0 = auto (Windows' value for the screen, else 1000).
static std::atomic<int> g_hdr{-1}, g_hdr_paper{-1}, g_hdr_peak{-1};
static const int k_hdr_paper[] = {100, 150, 200, 300, 400};  // 5 choices: the game's Settings screen has 5 option slots
static const int k_hdr_peak[] = {0, 400, 600, 1000, 2000};
int hdr_setting() {
    if (g_hdr < 0) g_hdr = env_on("NX_HDR", false) ? 1 : 0;
    return g_hdr;
}
void set_hdr(int on) { g_hdr = on ? 1 : 0; }
int hdr_paper_white() {
    if (g_hdr_paper < 0) g_hdr_paper = env_int("NX_HDR_PAPER_WHITE", 200, 80, 500);
    return g_hdr_paper;
}
int hdr_peak_setting() {
    if (g_hdr_peak < 0) g_hdr_peak = env_int("NX_HDR_PEAK", 0, 0, 10000);
    return g_hdr_peak;
}
// hdr_boost 0..4 (default 3): inverse tone mapping strength: the bright part of the game's SDR picture (3D scene only, not
// the UI) is expanded toward the screen's peak (vk.cpp hdr_pass, s121); 0 = the plain mapping of s119. hdr_color 0..4
// (default 2): extra colour saturation in the 3D scene with HDR.
static std::atomic<int> g_uw_fill{-1};
int ultrawide_fill() {
    if (g_uw_fill < 0) g_uw_fill = env_int("NX_ULTRAWIDE_FILL", 1, 0, 1);
    return g_uw_fill;
}
static std::atomic<int> g_hdr_boost{-1}, g_hdr_color{-1};
int hdr_boost() {
    if (g_hdr_boost < 0) g_hdr_boost = env_int("NX_HDR_BOOST", 3, 0, 4);
    return g_hdr_boost;
}
int hdr_color() {
    if (g_hdr_color < 0) g_hdr_color = env_int("NX_HDR_COLOR", 2, 0, 4);
    return g_hdr_color;
}
static void apply_sharpen(int i) {
    save_setting("sharpen", std::to_string(k_sharpen[i]));
    g_sharpen = k_sharpen[i];
}

static std::atomic<int64_t> g_title_ms{0};
static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void overlay_title_seen() { g_title_ms = now_ms(); }
static std::atomic<int64_t> g_camp_ms{0};
void overlay_camp_seen() { g_camp_ms = now_ms(); }

// ------------------------------------------------------------------------------------------------ menu model
enum : uint64_t { P_A = 1 << 0, P_B = 1 << 1, P_LS = 1 << 4, P_RS = 1 << 5, P_L = 1 << 6, P_MINUS = 1 << 11, P_LEFT = 1 << 12,
                  P_UP = 1 << 13, P_RIGHT = 1 << 14, P_DOWN = 1 << 15 };

struct Item {
    const wchar_t* name;
    std::vector<const wchar_t*> choices;  // empty: an action
    int cur;
    const wchar_t* help;
    void (*apply)(int choice);
};

static const int k_fps[] = {30, 60, 120, 144, 165, 240, 0};
static const char* const k_res[] = {"720p", "1080p", "1440p", "4k", "8k"};
static bool g_restart_note = false;
static void* g_game_hwnd = nullptr;

static void apply_fps(int i) {
    int n = k_fps[i];
    save_setting("fps", std::to_string(n));
    if (n == 30 || fps_setting() == 30) g_restart_note = true;  // 30 <-> other rates need a restart (frame step setup)
    else set_fps(n);
}
static void apply_res(int i) { save_setting("res", k_res[i]); g_restart_note = true; }
static const char* const k_aspect[4] = {"16:9", "auto", "21:9", "32:9"};
static int language_index() {
    const char* l = language();
    return !strcmp(l, "ja") ? 1 : !strcmp(l, "fr") ? 2 : !strcmp(l, "zh-Hans") ? 3 : !strcmp(l, "zh-Hant") ? 4 : 0;
}
static void apply_aspect(int i) { save_setting("aspect", k_aspect[i]); g_restart_note = true; }
static int aspect_index() {
    const char* a = getenv("NX_ASPECT");
    for (int i = 0; i < 4; i++) if (a && !_stricmp(a, k_aspect[i])) return i;
    return 0;
}
static void apply_window(int i) {
    save_setting("window", i ? "window" : "borderless");
    ui_set_fullscreen(i == 0);
}
static void apply_fx(int fx, int on) {
    save_setting(k_fx_keys[fx], on ? "1" : "0");
    fx_enabled(0);  // loaded
    if (on) g_fx_off &= ~(1u << fx);
    else g_fx_off |= 1u << fx;
}
static void apply_first_strike(int on) { save_setting("first_strike", on ? "1" : "0"); }
static void apply_guard_regen(int on) { save_setting("guard_regen", on ? "1" : "0"); }
static void apply_skill_details(int on) { save_setting("skill_details", on ? "1" : "0"); g_restart_note = true; }
static void apply_random_bgm(int i) { save_setting("random_battle_bgm", std::to_string(i)); }
static void apply_custom_bgm_volume(int i) { save_setting("custom_bgm_volume", std::to_string(i)); }
static void apply_levelup_refill(int on) { save_setting("levelup_refill", on ? "1" : "0"); }
static void apply_autosave(int on) { save_setting("autosave", on ? "1" : "0"); }
static void apply_hide_enemies(int on) { save_setting("hide_field_enemies", on ? "1" : "0"); }
static const float k_walk[] = {1, 1.25f, 1.5f, 1.75f, 2, 2.5f, 3};
static void apply_walk(int i) {
    char v[16];
    snprintf(v, sizeof v, "%g", k_walk[i]);
    save_setting("walk_speed", v);
    set_walk_speed(k_walk[i]);
}
static void apply_exit(int) {
    log("overlay: exit chosen");
    fflush(nullptr);
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);
#endif
}
// Return to Title: first close the camp menu with B presses (the game's own back path, from any submenu), then the field
// leaves to the title scene from its free-roam state (hle_sys.cpp field_roam_update), like the game after an event calls
// Lua ChangeTitle. Tested 10-30: leaving from the camp's own field state, with the menu open or just closed, left the
// menu or the "now loading" screen drawn over the title.
static std::atomic<bool> g_title_request{false};
static std::atomic<int64_t> g_title_ms_chosen{0};  // when Return to Title was chosen, 0 = none
bool overlay_take_title_request() { return g_title_request.exchange(false); }
static void request_title() {
    log("overlay: return to title");
    g_title_ms_chosen = now_ms();
}
// Open Tokyo Map (QoL fast travel): the game's own map, as at a town exit (hle_sys.cpp field_roam_update). Offered only
// while the field is in free roam in a town area; the map's points keep the game's own unlock flags.
static std::atomic<int64_t> g_roam_ms{0};
static std::atomic<bool> g_roam_town{false};
static std::atomic<int64_t> g_map_request_ms{0};  // when chosen, 0 = none
void overlay_roam_seen(bool town) { g_roam_ms = now_ms(), g_roam_town = town; }
void overlay_request_map() { log("overlay: open Tokyo Map"); g_map_request_ms = now_ms(); }
// a request not taken within 2 s (an event or battle started meanwhile) is dropped: the map never opens out of the blue
bool overlay_take_map_request() {
    const int64_t p = g_map_request_ms.exchange(0);
    if (p && now_ms() - p > 2000) log("overlay: Tokyo Map request dropped (field was not in free roam)");
    return p && now_ms() - p <= 2000;
}
static bool map_offered() { return g_roam_town && now_ms() - g_roam_ms < 300; }
// L1 fast travel (USER 11-14): L in town free roam opens a list of visited shops (hle_sys.cpp travel_mask) + Tokyo Map
static std::atomic<int64_t> g_travel_ms{0};
static std::atomic<int> g_travel_shop{-1};
void overlay_request_travel(int i) { log("overlay: fast travel %d", i); g_travel_shop = i, g_travel_ms = now_ms(); }
int overlay_take_travel_request() {
    const int64_t p = g_travel_ms.exchange(0);
    if (!p) return -1;
    if (now_ms() - p > 2000) { log("overlay: fast travel request dropped (field was not in free roam)"); return -1; }
    return g_travel_shop;
}
static std::vector<Item> build_travel_items() {
    std::vector<Item> v;
    const uint32_t m = travel_shops_offered();
    for (int i = 0; i < travel_shop_count() && i < 32; i++) {
        if (!(m >> i & 1)) continue;
        // shortcut: Item.apply has no context, so the row's shop index travels through a per-row lambda table
        static void (*const go[8])(int) = {[](int) { overlay_request_travel(0); }, [](int) { overlay_request_travel(1); },
                                            [](int) { overlay_request_travel(2); }, [](int) { overlay_request_travel(3); },
                                            [](int) { overlay_request_travel(4); }, [](int) { overlay_request_travel(5); },
                                            [](int) { overlay_request_travel(6); }, [](int) { overlay_request_travel(7); }};
        if (i < 8) v.push_back({travel_shop_name(i), {}, 0, travel_shop_help(i), go[i]});
    }
    if (v.empty())
        v.push_back({L"No shops yet", {}, 0, L"Shops appear here after your first visit (Café Seiren once Shibuya is open on the Tokyo Map).", [](int) {}});
    v.push_back({L"Tokyo Map", {}, 0, L"The game's own Tokyo Map, as at a town exit. Only places you have unlocked can be picked; B on the map returns here.",
                 [](int) { overlay_request_map(); }});
    return v;
}
// Request hints (QoL, user request s116): the F1 menu's "Request hints" row opens a page of the requests that are open
// right now (available but not taken, or taken and not done; the game's FLG_MAP_QUEST_NN_* flags), each with who gives
// it and how to finish it (request_hints.inc, tools/lua/request_hints.txt).
struct RequestHint { int quest; uint32_t available, started, done; const wchar_t *name, *giver, *how; };
static const RequestHint k_request_hints[] = {
#include "request_hints.inc"
};
static std::atomic<bool> g_open_requests{false};
static std::vector<Item> build_request_items() {
    static std::vector<std::wstring> text;  // the rows point into these until the next build
    text.clear();
    text.reserve(2 * std::size(k_request_hints) + 2);
    std::vector<Item> v;
    for (int pass = 0; pass < 2; pass++)  // taken ones first, then the ones waiting to be taken
        for (const RequestHint& r : k_request_hints) {
            if (game_flag(r.available) != 1 && game_flag(r.started) != 1) continue;
            if (game_flag(r.done) == 1 || (game_flag(r.started) == 1) != (pass == 0)) continue;
            text.push_back(std::wstring(r.name) + (pass == 0 ? L"  · in progress" : L"  · not taken yet"));
            const wchar_t* name = text.back().c_str();
            text.push_back(pass == 0 ? std::wstring(r.how) + L" (Given by: " + r.giver + L".)"
                                     : std::wstring(L"Talk to: ") + r.giver + L". Then: " + r.how);
            v.push_back({name, {}, 0, text.back().c_str(), [](int) {}});
        }
    if (v.empty())
        v.push_back({L"No open requests", {}, 0, L"Requests that can be taken now or are in progress show here, with where to go. New ones come with the story and as you finish others.", [](int) {}});
    return v;
}
// buttons to add to the game's pad this poll (B pulses until the camp menu is closed)
uint64_t overlay_buttons() {
    const int64_t p = g_title_ms_chosen, t = now_ms();
    if (!p) return 0;
    if (t - p > 10000) { log("overlay: return to title: camp menu did not close"); g_title_ms_chosen = 0; return 0; }
    if (t - g_camp_ms > 300) { g_title_request = true, g_title_ms_chosen = 0; return 0; }  // closed
    return (t - p) % 300 >= 150 ? P_B : 0;  // 150 ms released (the A press ends), 150 ms pressed
}

static std::vector<Item> build_items() {
    int fps = fps_setting(), fi = 1;
    for (int i = 0; i < 7; i++) if (k_fps[i] == fps) fi = i;
    const char* res = getenv("NX_RES");
    int ri = 0;
    for (int i = 0; i < 5; i++) if (res && !_stricmp(res, k_res[i])) ri = i;
    const char* win = getenv("NX_WINDOW");
    int wi = 0;
    for (int i = 0; i < 7; i++) if (fabsf(k_walk[i] - walk_speed()) < 0.01f) wi = i;
    std::vector<const wchar_t*> onoff = {L"Off", L"On"};
    std::vector<Item> v = {
        {L"Frame rate", {L"30", L"60", L"120", L"144", L"165", L"240", L"Uncapped"}, fi,
         L"Game speed stays the same at every rate; higher is smoother. 30 is the Switch rate (switching to or from 30 needs a restart).", apply_fps},
        {L"Auto 30 when slow", onoff, auto30_setting(),
         L"At 60 fps: where this PC cannot hold 60 (busy streets), drop to 30 fps at full game speed instead of slow motion; back to 60 when it can.",
         [](int c) { set_auto30(c != 0); save_setting("auto_30", c ? "1" : "0"); }},
        {L"Crowd density", {L"Full", L"Half", L"Low"}, getenv("NX_CROWD_DENSITY") ? std::clamp(atoi(getenv("NX_CROWD_DENSITY")), 0, 2) : 0,
         L"Walkers and cars in town streets (Shibuya). Half / Low: fewer of them close by, for weak PCs (busy streets run faster). Applies when you next enter an area.",
         [](int c) { save_setting("crowd_density", std::to_string(c)); }},
        {L"Shadow detail", {L"Full", L"Low"}, g_shadow_detail == 1 ? 1 : 0,
         L"Low: no far shadow layer (shadows near you stay). Fewer draws in big areas like Shibuya, for weak PCs. Applies at once.",
         [](int c) { g_shadow_detail = c; save_setting("shadow_detail", c ? "1" : "0"); }},
        {L"UI animation speed", {L"Switch", L"60 Hz"}, ui_speed() == 60 ? 1 : 0,
         L"Menus, dialogue, HUD and battle UI. 60 Hz (default): the 60 fps speed at every frame rate above 30. Switch: half that speed, the original 30 fps pace.",
         [](int c) { set_ui_speed(c ? 60 : 30); save_setting("ui_speed", c ? "60" : "30"); }},
        {L"Attack Session", onoff, !getenv("NX_ATTACK_SESSION") || atoi(getenv("NX_ATTACK_SESSION")) != 0,
         L"A regular Attack that hits a weakness can start a Session. Off: the original rule. Applies after a restart.",
         [](int c) { save_setting("attack_session", c ? "1" : "0"); }},
        {L"UI smoothing", onoff, ui_smooth(),
         L"Above 60 fps: menu and HUD motion glides between the 60 Hz steps (same speed, shown up to 16 ms later). Off: plain 60 Hz steps.",
         [](int c) { set_ui_smooth(c != 0); save_setting("ui_smooth", c ? "1" : "0"); }},
        {L"Resolution", {L"720p", L"1080p", L"1440p", L"4K", L"8K"}, ri, L"Internal render resolution. Applies after a restart. Above the screen size it supersamples (anti-aliasing): 4K on a 1080p screen, 8K on a 4K screen (needs ~5 GB of video memory).", apply_res},
        {L"Aspect ratio", {L"16:9", L"Auto", L"21:9", L"32:9"}, aspect_index(),
         L"Picture shape. 21:9 / 32:9 (ultrawide): the 3D view gets wider (more to see at the sides), menus and HUD stay 16:9 in the middle. Auto: your screen's shape. Applies after a restart.",
         apply_aspect},
        {L"Ultrawide sides", {L"Black", L"Blurred picture"}, rt::ultrawide_fill(),
         L"Ultrawide only: full-screen 16:9 pictures and movies (title, story movies, Tokyo map) get a blurred, zoomed copy of the picture at the sides instead of black bars.",
         [](int c) { save_setting("ultrawide_fill", c ? "1" : "0"); g_uw_fill = c; }},
        {L"Language", {L"English", L"Japanese", L"French", L"Chinese (Simplified)", L"Chinese (Traditional)"}, language_index(),
         L"Game text and menus (voices stay). The Uncensored mod's text is English only: with another language its text changes are left out. Applies after a restart.",
         [](int c) { static const char* k[] = {"en", "ja", "fr", "zh-Hans", "zh-Hant"}; save_setting("language", k[c]); g_restart_note = true; }},
        {L"Display", {L"Borderless", L"Window"}, win && !strcmp(win, "window") ? 1 : 0, L"Borderless fullscreen or a window (F11 / Alt+Enter also switch).", apply_window},
        {L"Bloom", onoff, fx_enabled(FX_BLOOM), L"Glare around bright lights (the game's glare pass).", [](int c) { apply_fx(FX_BLOOM, c); }},
        {L"Glow", onoff, fx_enabled(FX_GLOW), L"Soft glow on emissive surfaces (the game's glow pass).", [](int c) { apply_fx(FX_GLOW, c); }},
        {L"Depth of field", onoff, fx_enabled(FX_DOF), L"Background blur in cutscenes and menus.", [](int c) { apply_fx(FX_DOF, c); }},
        {L"Vignette", onoff, fx_enabled(FX_VIGNETTE), L"Darkened screen edges where the game uses them.", [](int c) { apply_fx(FX_VIGNETTE, c); }},
        {L"Fog", onoff, fx_enabled(FX_FOG), L"Distance fog where the game uses it.", [](int c) { apply_fx(FX_FOG, c); }},
        {L"Outline", onoff, fx_enabled(FX_OUTLINE), L"Edge outline pass where the game uses it.", [](int c) { apply_fx(FX_OUTLINE, c); }},
        {L"Character rim", onoff, fx_enabled(FX_CHAR_RIM), L"The light line around characters (the game's own outline shell). Off removes it; it stands out most in dark dungeons.", [](int c) { apply_fx(FX_CHAR_RIM, c); }},
        {L"Sharpening", {L"Off", L"Low", L"Medium", L"High"},
         sharpen_setting() >= 100 ? 3 : sharpen_setting() >= 65 ? 2 : sharpen_setting() >= 35 ? 1 : 0,
         L"Contrast-adaptive sharpening of the final image (crisper textures at 4K, no halos on hard edges).", apply_sharpen},
        {L"Screen scaler", {L"Bilinear", L"FSR 1"}, upscaler_setting(),
         L"Only when Resolution is below the screen (e.g. 1080p on a 4K screen): how the finished frame is stretched to it. FSR 1 keeps edges sharp. FSR 3 / DLSS upscale the 3D scene themselves: Anti-aliasing + FSR 3 / DLSS upscaling rows.",
         apply_upscaler},
        {L"HDR", onoff, hdr_setting(),
         L"HDR output: bright lights, spells and effects shine above normal white on an HDR screen. Needs Windows HDR turned on (Settings > Display); without it the picture stays as before.",
         [](int c) { save_setting("hdr", c ? "1" : "0"); g_hdr = c; }},
        {L"HDR highlights", {L"Off", L"Low", L"Medium", L"High", L"Max"}, hdr_boost(),
         L"With HDR: lifts the bright parts of the 3D scene (sky, lights, spells, shiny surfaces) toward your screen's peak brightness, like AutoHDR; menus and text stay at paper white. Off = the game's own brightness only.",
         [](int c) { save_setting("hdr_boost", std::to_string(c)); g_hdr_boost = c; }},
        {L"HDR colour", {L"Off", L"Low", L"Medium", L"High", L"Max"}, hdr_color(),
         L"With HDR: richer, more vivid colours in the 3D scene (menus and text unchanged).",
         [](int c) { save_setting("hdr_color", std::to_string(c)); g_hdr_color = c; }},
        {L"HDR paper white", {L"100", L"150", L"200", L"300", L"400"},
         (int)(std::find(std::begin(k_hdr_paper), std::end(k_hdr_paper), hdr_paper_white()) - std::begin(k_hdr_paper)) % 5,
         L"With HDR on: how bright normal white (menus, text, daylight) is, in nits. 200 suits most rooms; raise it in a bright room.",
         [](int c) { save_setting("hdr_paper_white", std::to_string(k_hdr_paper[c])); g_hdr_paper = k_hdr_paper[c]; }},
        {L"HDR peak brightness", {L"Auto", L"400", L"600", L"1000", L"2000"},
         (int)(std::find(std::begin(k_hdr_peak), std::end(k_hdr_peak), hdr_peak_setting()) - std::begin(k_hdr_peak)) % 5,
         L"With HDR on: the brightest your screen can show, in nits. Auto uses the value Windows reports for the screen.",
         [](int c) { save_setting("hdr_peak", std::to_string(k_hdr_peak[c])); g_hdr_peak = k_hdr_peak[c]; }},
        {L"Anti-aliasing", {L"Off", L"FXAA", L"SMAA", L"FSR 3", L"DLSS"}, fsr3_setting() == 2 ? 4 : fsr3_setting() ? 3 : smaa_setting() ? 2 : fx_enabled(FX_FXAA) ? 1 : 0,
         L"FXAA = the game's own pass. SMAA = sharper edge smoothing. FSR 3 = AMD FSR 3 temporal AA at full size (like DLAA: smoothest edges, may ghost a little on moving characters). DLSS = NVIDIA DLSS / DLAA on RTX GPUs (setup option; else FSR 3). All under the UI. Off is sharpest.", apply_aa},
        {L"FSR 3 / DLSS upscaling", {L"Native", L"Quality", L"Balanced", L"Performance", L"Ultra perf."}, std::clamp(fsr3_quality(), 0, 4),
         L"With Anti-aliasing = FSR 3 or DLSS (Native = DLAA): the 3D scene renders smaller (Quality 1/1.5, Balanced 1/1.7, Performance 1/2, Ultra 1/3 of the screen) and it is scaled up, for more fps. UI stays full size. Applies at once.", apply_fsr3_quality},
        {L"3D resolution", {L"Auto", L"100%", L"85%", L"75%", L"67%", L"50%"},
         (int)(std::find(std::begin(k_scene_res), std::end(k_scene_res), scene_res()) - std::begin(k_scene_res)) % 6,
         L"With Anti-aliasing Off, FXAA or SMAA: the 3D scene renders at this share of the screen size and is scaled up; menus and the HUD stay sharp. Lower = much more fps on weak graphics cards (fewer pixels to draw). Auto lowers it step by step while the graphics card cannot keep your fps, and raises it again when there is room. FSR 3 / DLSS use their own upscaling row. Applies at once.", apply_scene_res},
        {L"MSAA", {L"Off", L"2x", L"4x", L"8x"}, msaa_setting() >= 8 ? 3 : msaa_setting() >= 4 ? 2 : msaa_setting() >= 2 ? 1 : 0,
         L"Multisampled 3D scene (geometry edges). Works with FXAA / SMAA on top; OFF while Anti-aliasing is FSR 3 / DLSS (they anti-alias already). At 4K: 4x needs ~1.4 GB more VRAM and costs almost no fps, 8x ~2.9 GB and ~12% (measured, RTX 5070 Ti). Lowered automatically on GPUs with too little VRAM.", apply_msaa},
        {L"Button icons", {L"Auto", L"Switch", L"PlayStation", L"Xbox"}, pad_icons_setting(),
         L"Button prompts drawn for your controller. Auto picks them from the connected pad.", set_pad_icons},
        {L"Walk speed", {L"1x", L"1.25x", L"1.5x", L"1.75x", L"2x", L"2.5x", L"3x"}, wi,
         L"Itsuki's walking and running speed in the field and dungeons (1x = the original speed).", apply_walk},
        {L"First strike on downed symbols", onoff, env_on("NX_FIRST_STRIKE", true),
         L"Touching a symbol you knocked down with the sword always gives your party the first action.", apply_first_strike},
        {L"Guard recovers HP / EP", onoff, env_on("NX_GUARD_REGEN", true),
         L"A party member who guards gets back 10% of their max HP and 10% of their max EP (on top of the game's own small EP gain).", apply_guard_regen},
        {L"UHD textures", onoff, tex_pack_on(),
         L"The upscaled texture pack (textures\\pack in the data folder), if installed. Switches at once.",
         [](int c) { set_tex_pack(c != 0); }},
        {L"Battle voice subtitles", onoff, subs_enabled(),
         L"English subtitles for the Japanese voice lines in battle (party, Mirages and bosses).", [](int c) { set_subs_enabled(c != 0); }},
        {L"Detailed skill help", onoff, env_on("NX_SKILL_DETAILS", true),
         L"Skill help adds power, accuracy and targets from the skill data (applies after a restart).", apply_skill_details},
        {L"Random battle music", {L"Off", L"Game", L"Custom", L"Mixed"}, custom_bgm_mode(),
         L"Game: each normal battle plays a random battle track of the game (bosses: see the next item). Custom: a random "
         L".adx track from your music folder (custom_bgm_dir, default <data>\\music) with the matching victory theme. Mixed: both.",
         apply_random_bgm},
        {L"Random music for bosses", onoff, env_on("NX_RANDOM_BGM_BOSSES", false),
         L"Off: boss and story battles keep their own music; only normal battles get a random track. On: bosses too.",
         [](int c) { save_setting("random_bgm_bosses", c ? "1" : "0"); }},
        {L"Custom music volume", {L"0", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"10"},
         getenv("NX_CUSTOM_BGM_VOLUME") ? std::clamp(atoi(getenv("NX_CUSTOM_BGM_VOLUME")), 0, 10) : 7,
         L"Volume of your own battle music (Random battle music: Custom or Mixed). 7 = default.", apply_custom_bgm_volume},
        {L"Battle speed", {L"1x", L"2x", L"3x"}, battle_speed() - 1,
         L"Game speed in battles (F3 or the touchpad also switches it in battle). 3x needs 90+ fps; at 60 fps it acts as 2x.",
         [](int c) { set_battle_speed(c + 1, false); }},
        {L"P5-style battle buttons", {L"Off", L"Buttons", L"Buttons + P5 panel"}, battle_buttons_mode(),
         L"Each battle command on its own button: bottom Attack, right Guard, top Skills, left Items, L Tactics, R Change, "
         L"- Escape, ZR Auto battle. P5 panel: a Persona 5 style panel replaces the wheel. Applies at once, also in battle.",
         [](int c) { set_battle_buttons_mode(c); save_setting("battle_buttons", std::to_string(c)); }},
        {L"Reveal dungeon maps", onoff, env_on("NX_MAP_REVEAL", false),
         L"Each Idolasphere floor shows its whole layout on the minimap and the map as soon as you enter it. What was revealed stays revealed.",
         [](int c) { save_setting("map_reveal", c ? "1" : "0"); }},
        {L"Hide field enemies", onoff, env_on("NX_HIDE_FIELD_ENEMIES", false),
         L"No enemy symbols in the field and dungeons: explore without battles. Applies when you enter the next area.", apply_hide_enemies},
        {L"EXP multiplier", {L"1x", L"2x", L"3x", L"4x", L"5x"}, env_int("NX_EXP_RATE", 1, 1, 5) - 1,
         L"EXP from every battle x N (results screen and each party member).",
         [](int c) { save_setting("exp_rate", std::to_string(c + 1)); }},
        {L"Money multiplier", {L"1x", L"2x", L"3x", L"4x", L"5x"}, env_int("NX_MONEY_RATE", 1, 1, 5) - 1,
         L"Money from every battle x N.", [](int c) { save_setting("money_rate", std::to_string(c + 1)); }},
        {L"Mastery multiplier", {L"1x", L"2x", L"3x", L"4x", L"5x"}, env_int("NX_MASTERY_RATE", 1, 1, 5) - 1,
         L"Weapon (Carnage) Mastery from every battle x N: new skills sooner.",
         [](int c) { save_setting("mastery_rate", std::to_string(c + 1)); }},
        {L"Resistance icons", onoff, resist_icons_on(),
         L"Resistance badges (Wk / Rs / Nu / Dr / Rp) drawn as coloured icons instead of letters. Switches at once.",
         [](int c) { set_resist_icons(c != 0); }},
        {L"Level-up refills HP / EP", onoff, env_on("NX_LEVELUP_REFILL", true),
         L"A party member who levels up in a battle gets full HP and EP (applied when you walk again after the battle).",
         apply_levelup_refill},
        {L"Sub Cast EXP", onoff, env_on("NX_SUB_EXP", true),
         L"On (the game's rule): every party member gets battle EXP. Off: only the three on the front line when an enemy falls.",
         [](int c) { save_setting("sub_exp", c ? "1" : "0"); }},
        {L"EXP level-gap penalty", onoff, env_on("NX_EXP_GAP", true),
         L"On (the game's rule): a member above the enemy's level gets less EXP (5 levels above: 25%, 6+: none). Off: full EXP.",
         [](int c) { save_setting("exp_gap", c ? "1" : "0"); }},
        {L"Retry lost battles", onoff, env_on("NX_BATTLE_RETRY", true),
         L"When the party falls, the game asks \"Retry the battle?\": Yes starts the same battle again with the HP, EP and items it began with. No: GAME OVER as usual.",
         [](int c) { save_setting("battle_retry", c ? "1" : "0"); }},
        {L"Revive at 1 HP after battles", onoff, env_on("NX_KO_REVIVE", true),
         L"Party members who fell get up with 1 HP when a battle is won or escaped. Off: they stay down until healed (the game's rule).",
         [](int c) { save_setting("ko_revive", c ? "1" : "0"); }},
        {L"Carry 99 of each item", onoff, env_on("NX_ITEM_MAX99", true),
         L"Every consumable can be stocked up to 99 (shops and the item list). Off: the game's limits (Life Stone 40, Magic Mirror 5...).",
         [](int c) { save_setting("item_max99", c ? "1" : "0"); }},
        {L"Overworld costume", onoff, env_on("NX_FIELD_COSTUME", false),
         L"On: outside battle (town, dungeons, story scenes) party members wear the costume equipped in Equip > Costume, as in battle. Off: their story clothes (the game's rule). Applies when a character is next shown.",
         [](int c) { save_setting("field_costume", c ? "1" : "0"); }},
        {L"Resist markers", onoff, env_on("NX_RESIST_MARKER", true),
         L"Battle: a foe that resists the highlighted skill gets a shield where the game shows \"!\" for a weakness (also in the target panel); a resisted hit shows RESIST like WEAK. Switches at once.",
         [](int c) { save_setting("resist_marker", c ? "1" : "0"); }},
        {L"WEAK in the skill list", onoff, env_on("NX_WEAK_HINT", true),
         L"Battle: the skill list shows WEAK next to a skill when a foe on the field is weak to it and you already know that weakness (like Persona 5 Royal). Switches at once.",
         [](int c) { save_setting("weak_hint", c ? "1" : "0"); }},
        {L"Autosave after battles", onoff, env_on("NX_AUTOSAVE", true),
         L"After each battle, once you walk again, the game is saved to slot 20 (your own slots are never touched). Load it like any save.", apply_autosave},
        {L"Request hints", {}, 0, L"Open requests (side quests) with who gives them and how to finish them: the ones in progress and the ones waiting to be taken.",
         [](int) { g_open_requests = true; }},
        {L"Exit game", {}, 0, L"Close the game. Save first: progress since the last save is lost.", apply_exit},
    };
    return v;
}

static std::string utf8(const wchar_t* w) {
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, ' ');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
#else
    return draw::utf8(w);
#endif
}
bool overlay_setting(const wchar_t* name, SettingItem& out) {
    for (const Item& it : build_items()) {
        if (wcscmp(it.name, name)) continue;
        out.name = utf8(it.name), out.help = utf8(it.help), out.cur = it.cur, out.choices.clear();
        for (const wchar_t* c : it.choices) out.choices.push_back(utf8(c));
        return true;
    }
    return false;
}
void overlay_apply_setting(const wchar_t* name, int choice) {
    for (const Item& it : build_items())
        if (!wcscmp(it.name, name) && choice >= 0 && choice < (int)it.choices.size()) it.apply(choice);
}

// ------------------------------------------------------------------------------------------------ drawing
#ifdef _WIN32
struct Layer {
    HWND hwnd = nullptr;
    HDC mem = nullptr;
    HBITMAP bmp = nullptr;
    int w = 0, h = 0;
    bool shown = false;
};
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static ID2D1DCRenderTarget* g_rt;

static bool init_d2d() {
    if (g_rt) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) return false;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(g_d2d->CreateDCRenderTarget(&p, &g_rt));
}

static void layer_hide(Layer& l) {
    if (l.hwnd && l.shown) ShowWindow(l.hwnd, SW_HIDE);
    l.shown = false;
}

// Draws with fn into a w x h layer at screen (x, y) over the game window.
template <class F>
static void layer_show(Layer& l, int x, int y, int w, int h, F fn) {
    if (!l.hwnd) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TMSRecompOverlay";
        RegisterClassW(&wc);  // fails harmlessly the second time
        l.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                 L"", WS_POPUP, 0, 0, 1, 1, (HWND)g_game_hwnd, nullptr, wc.hInstance, nullptr);
    }
    if (l.w != w || l.h != h) {
        if (l.bmp) DeleteObject(l.bmp);
        if (!l.mem) l.mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits;
        l.bmp = CreateDIBSection(l.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(l.mem, l.bmp);
        l.w = w, l.h = h;
    }
    RECT r{0, 0, w, h};
    g_rt->BindDC(l.mem, &r);
    g_rt->BeginDraw();
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    fn();
    g_rt->EndDraw();
    POINT pos{x, y}, zero{0, 0};
    SIZE size{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(l.hwnd, nullptr, &pos, &size, l.mem, &zero, 0, &bf, ULW_ALPHA);
    if (!l.shown) SetWindowPos(l.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    l.shown = true;
}

static void text(const wchar_t* s, float x, float y, float w, float h, float size, D2D1::ColorF color,
                 DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* f;
    if (FAILED(g_dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      size, L"en-us", &f)))
        return;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ID2D1SolidColorBrush* b;
    g_rt->CreateSolidColorBrush(color, &b);
    g_rt->DrawText(s, (UINT32)wcslen(s), f, D2D1::RectF(x, y, x + w, y + h), b);
    b->Release();
    f->Release();
}

static void fill(float x, float y, float w, float h, float radius, D2D1::ColorF color) {
    ID2D1SolidColorBrush* b;
    g_rt->CreateSolidColorBrush(color, &b);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), b);
    b->Release();
}

#else  // Linux: each layer is a bitmap blended into the frame by the present path (frame_overlay below)
namespace D2D1 {
struct ColorF {
    float r, g, b, a;
    ColorF(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
};
}  // namespace D2D1
using DWRITE_FONT_WEIGHT = int;
constexpr int DWRITE_FONT_WEIGHT_NORMAL = 400, DWRITE_FONT_WEIGHT_SEMI_BOLD = 600, DWRITE_FONT_WEIGHT_BOLD = 700;
enum DWRITE_TEXT_ALIGNMENT { DWRITE_TEXT_ALIGNMENT_LEADING = draw::LEADING, DWRITE_TEXT_ALIGNMENT_TRAILING = draw::TRAILING,
                             DWRITE_TEXT_ALIGNMENT_CENTER = draw::CENTER };
struct RECT { int left, top, right, bottom; };
struct POINT { int x, y; };

struct Layer {
    std::unique_ptr<draw::Canvas> cv;
    int x = 0, y = 0, w = 0, h = 0;  // in window pixels
    bool shown = false;
    std::shared_ptr<const SubsBitmap> bmp;  // what frame_overlay blends (g_layer_lock)
};
static std::mutex g_layer_lock;
static int g_layer_win_w = 0, g_layer_win_h = 0;  // the window size the layers were laid out for (g_layer_lock)
static cairo_t* g_cr;

static bool init_d2d() { return true; }

static void layer_hide(Layer& l) {
    std::lock_guard<std::mutex> lk(g_layer_lock);
    l.shown = false;
    l.bmp = nullptr;
}

template <class F>
static void layer_show(Layer& l, int x, int y, int w, int h, F fn) {
    if (w <= 0 || h <= 0) return;
    if (!l.cv || l.cv->w != w || l.cv->h != h) l.cv = std::make_unique<draw::Canvas>(w, h);
    l.cv->clear();
    g_cr = l.cv->cr;
    fn();
    g_cr = nullptr;
    auto b = std::make_shared<SubsBitmap>();
    b->w = (uint32_t)w, b->h = (uint32_t)h;
    b->rgba.resize((size_t)w * h);
    const uint8_t* src = l.cv->data();
    for (int yy = 0; yy < h; yy++) {
        const uint32_t* s = (const uint32_t*)(src + (size_t)yy * l.cv->stride());
        uint32_t* d = b->rgba.data() + (size_t)yy * w;
        for (int xx = 0; xx < w; xx++) d[xx] = (s[xx] & 0xff00ff00) | ((s[xx] >> 16) & 0xff) | ((s[xx] & 0xff) << 16);  // BGRA -> RGBA
    }
    std::lock_guard<std::mutex> lk(g_layer_lock);
    l.x = x, l.y = y, l.w = w, l.h = h, l.shown = true;
    l.bmp = b;
}

static void text(const wchar_t* s, float x, float y, float w, float h, float size, D2D1::ColorF color,
                 DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    draw::text(g_cr, draw::utf8(s), x, y, w, h, size, {color.r, color.g, color.b, color.a}, weight, (draw::Align)align);
}

static void fill(float x, float y, float w, float h, float radius, D2D1::ColorF color) {
    draw::round_rect(g_cr, x, y, w, h, radius);
    draw::set(g_cr, {color.r, color.g, color.b, color.a});
    cairo_fill(g_cr);
}
#endif

static const D2D1::ColorF k_bg(0.05f, 0.06f, 0.08f, 0.88f), k_lime(0.62f, 0.95f, 0.16f, 1), k_white(1, 1, 1, 1),
    k_grey(0.75f, 0.78f, 0.82f, 1), k_row(1, 1, 1, 0.06f);

// ------------------------------------------------------------------------------------------------ update
static Layer g_menu_layer, g_hint_layer;

bool overlay_update(void* hwnd, uint64_t buttons, int ly, bool f1) {
    g_game_hwnd = hwnd;
    static bool menu = false, confirm = false, swallow = false, travel = false;
    static int sel = 0, confirm_sel = 1, confirm_kind = 0;  // kind 0: title "Exit the game?" (Yes / No); 1: camp menu (Title / Quit / Cancel)
    static std::vector<Item> items;
    static uint64_t prev = 0;
    static int64_t next_repeat = 0;
    if (!init_d2d()) return false;
    // stick up/down as d-pad in the menus
    if (ly > 20000) buttons |= P_UP;
    if (ly < -20000) buttons |= P_DOWN;
    uint64_t pressed = buttons & ~prev;
    prev = buttons;
    int64_t t = now_ms();
    if (buttons & (P_UP | P_DOWN | P_LEFT | P_RIGHT)) {  // hold to repeat: 350 ms, then every 90 ms
        if (pressed & (P_UP | P_DOWN | P_LEFT | P_RIGHT)) next_repeat = t + 350;
        else if (t >= next_repeat) pressed |= buttons & (P_UP | P_DOWN | P_LEFT | P_RIGHT), next_repeat = t + 90;
    }
    const bool title = t - g_title_ms < 300, camp = t - g_camp_ms < 300;
    const int n_opt = confirm_kind ? 3 : 2;
    bool dirty = false;
    if (!menu && !confirm) {
        if (g_open_requests.exchange(false)) {
            menu = true, travel = false, items = build_request_items(), sel = 0, dirty = true;
            log("overlay: request hints (%zu rows)", items.size());
            for (auto& it : items) log("overlay:   %s", utf8(it.name).c_str());
        } else if (f1 || ((buttons & (P_LS | P_RS)) == (P_LS | P_RS) && (pressed & (P_LS | P_RS)))) {
            menu = true, travel = false, items = build_items(), sel = 0, dirty = true, g_restart_note = false;
        } else if (map_offered() && (pressed & P_L) && !(buttons & ~P_L)) {  // L alone: unused by the game in free roam
            menu = true, travel = true, items = build_travel_items(), sel = 0, dirty = true;
            log("overlay: fast travel menu (shops %#x)", travel_shops_offered());
        } else if ((title || camp) && (pressed & P_MINUS)) {
            confirm_kind = title ? 0 : 1;
            confirm = true, confirm_sel = confirm_kind ? 2 : 1, dirty = true;
        }
    } else if (confirm) {
        const int k = confirm_kind ? 3 : 2;
        if (pressed & (P_LEFT | P_UP)) confirm_sel = (confirm_sel + k - 1) % k, dirty = true;
        if (pressed & (P_RIGHT | P_DOWN)) confirm_sel = (confirm_sel + 1) % k, dirty = true;
        if (pressed & P_A) {
            if (confirm_kind == 0 && confirm_sel == 0) apply_exit(0);
            if (confirm_kind == 1 && confirm_sel == 0) request_title();
            if (confirm_kind == 1 && confirm_sel == 1) apply_exit(0);
            confirm = false, swallow = true;
        }
        if (pressed & P_B) confirm = false, swallow = true;
    } else {
        int n = (int)items.size();
        if (pressed & P_UP) sel = (sel + n - 1) % n, dirty = true;
        if (pressed & P_DOWN) sel = (sel + 1) % n, dirty = true;
        Item& it = items[sel];
        int k = (int)it.choices.size();
        if (k && (pressed & (P_LEFT | P_RIGHT | P_A))) {
            it.cur = (it.cur + ((pressed & P_LEFT) ? k - 1 : 1)) % k;
            it.apply(it.cur);
            dirty = true;
        } else if (!k && (pressed & P_A)) {
            it.apply(0);
            menu = false, swallow = true;  // actions close the menu (Open Tokyo Map runs once the game has the input back)
        }
        const bool sticks = (buttons & (P_LS | P_RS)) == (P_LS | P_RS) && (pressed & (P_LS | P_RS));
        if ((pressed & P_B) || f1 || sticks || (travel && (pressed & P_L))) menu = false, swallow = true;
        if (travel && menu && !map_offered()) menu = false;  // an event / battle took the field meanwhile
    }

    // layout in the game window's client area, scaled to its height (1080 = 1.0)
#ifdef _WIN32
    RECT cr;
    GetClientRect((HWND)hwnd, &cr);
    POINT org{0, 0};
    ClientToScreen((HWND)hwnd, &org);
    const float s = std::max(0.5f, (cr.bottom - cr.top) / 1080.0f);
    // the game's 16:9 area in the window: the picture (16:9 x aspect factor) fitted with bars by vk fit_blit, its 16:9
    // middle (ultrawide); the pill sits there, not in a black bar (user 21:9 test s104)
    const float pa = 16.0f / 9 * vk::wide_factor(), cw_ = (float)cr.right, ch_ = (float)cr.bottom;
    const float ph = std::fabs(cw_ / ch_ - pa) < pa * 0.01f ? ch_ : std::min(ch_, cw_ / pa);
    const int ui_left = (int)((cw_ - std::min(cw_, ph * 16 / 9)) / 2), ui_bottom = (int)((ch_ + ph) / 2);
    const bool visible = IsWindowVisible((HWND)hwnd) && !IsIconic((HWND)hwnd);
#else
    // layers in window pixels from the window's corner: frame_overlay maps them onto the frame
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels((SDL_Window*)hwnd, &ww, &wh);
    RECT cr{0, 0, ww, wh};
    POINT org{0, 0};
    const float s = std::max(0.5f, (cr.bottom - cr.top) / 1080.0f);
    // the game's 16:9 area in the window (ultrawide), as on Windows above
    const float pa = 16.0f / 9 * vk::wide_factor(), cw_ = (float)cr.right, ch_ = (float)cr.bottom;
    const float ph = std::fabs(cw_ / ch_ - pa) < pa * 0.01f ? ch_ : std::min(ch_, cw_ / pa);
    const int ui_left = (int)((cw_ - std::min(cw_, ph * 16 / 9)) / 2), ui_bottom = (int)((ch_ + ph) / 2);
    const bool visible = ww > 0 && wh > 0 && !(SDL_GetWindowFlags((SDL_Window*)hwnd) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    {
        std::lock_guard<std::mutex> lk(g_layer_lock);
        if (g_layer_win_w != ww || g_layer_win_h != wh) dirty = true;  // a new size: lay out again
        g_layer_win_w = ww, g_layer_win_h = wh;
    }
#endif

    static bool menu_drawn = false;
    if (menu && visible) {
        if (dirty || !menu_drawn) {
            // more items than fit the window: show a window of rows that follows the selection (arrows mark hidden rows)
            static int top = 0;
            int w = (int)(980 * s), row = (int)(54 * s), n = (int)items.size();
            int vis = std::min(n, std::max(3, (int)((cr.bottom - cr.top - 40 * s - 290 * s) / row)));
            top = std::clamp(top, std::max(0, sel - vis + 1), std::min(sel, n - vis));
            int h = (int)(170 * s) + row * vis + (int)(120 * s);
            int x = org.x + (cr.right - w) / 2, y = org.y + std::max(0, (int)(cr.bottom - h) / 2);
            layer_show(g_menu_layer, x, y, w, h, [&] {
                fill(0, 0, (float)w, (float)h, 18 * s, k_bg);
                fill(0, 0, 10 * s, (float)h, 4 * s, k_lime);
                text(travel ? L"FAST TRAVEL" : L"SETTINGS", 48 * s, 28 * s, 600 * s, 60 * s, 40 * s, k_white, DWRITE_FONT_WEIGHT_BOLD);
                text(L"TMSRecomp", 48 * s, 82 * s, 600 * s, 34 * s, 20 * s, k_lime);
                float y0 = 140 * s;
                if (top > 0) text(L"▲", w - 80 * s, 100 * s, 40 * s, 36 * s, 22 * s, k_lime);
                if (top + vis < n) text(L"▼", w - 80 * s, y0 + row * (float)vis - 4 * s, 40 * s, 36 * s, 22 * s, k_lime);
                for (size_t i = top; i < (size_t)(top + vis); i++) {
                    float ry = y0 + row * (float)(i - top);
                    if ((int)i == sel) fill(32 * s, ry, w - 64 * s, row - 6 * s, 10 * s, D2D1::ColorF(0.62f, 0.95f, 0.16f, 0.22f));
                    else if (i % 2 == 0) fill(32 * s, ry, w - 64 * s, row - 6 * s, 10 * s, k_row);
                    text(items[i].name, 56 * s, ry, 520 * s, row - 6 * s, 24 * s, (int)i == sel ? k_white : k_grey);
                    if (!items[i].choices.empty()) {
                        std::wstring v = items[i].choices[items[i].cur];
                        if ((int)i == sel) v = L"◀  " + v + L"  ▶";
                        text(v.c_str(), w - 420 * s, ry, 360 * s, row - 6 * s, 24 * s, (int)i == sel ? k_lime : k_white,
                             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                }
                float hy = y0 + row * (float)vis + 10 * s;
                std::wstring help = items[sel].help;
                if (g_restart_note) help += L"  (Saved: restart the game to apply.)";
                text(help.c_str(), 56 * s, hy, w - 112 * s, 64 * s, 19 * s, k_grey, DWRITE_FONT_WEIGHT_NORMAL);
                text(travel ? L"↑↓ select    A go    B / L close    (open: L in town)"
                            : L"↑↓ select    ←→ / A change    B / F1 / L3+R3 close    (open: F1, or press both sticks in)", 56 * s, (float)h - 48 * s, w - 112 * s, 36 * s, 18 * s,
                     k_grey, DWRITE_FONT_WEIGHT_NORMAL);
            });
        }
        menu_drawn = true;
    } else {
        layer_hide(g_menu_layer);
        menu_drawn = false;
    }

    if (confirm && visible) {
        const int cw = (int)((n_opt == 3 ? 820 : 620) * s);
        if (dirty || !g_hint_layer.shown || g_hint_layer.w != cw) {
            int w = cw, h = (int)((n_opt == 3 ? 270 : 230) * s);
            layer_show(g_hint_layer, org.x + (cr.right - w) / 2, org.y + (cr.bottom - h) / 2, w, h, [&] {
                fill(0, 0, (float)w, (float)h, 18 * s, k_bg);
                fill(0, 0, (float)w, 8 * s, 4 * s, k_lime);
                text(n_opt == 3 ? L"Leave the game?" : L"Exit the game?", 0, 34 * s, (float)w, 60 * s, 34 * s, k_white,
                     DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                if (n_opt == 3)
                    text(L"Progress since your last save will be lost.", 0, 86 * s, (float)w, 36 * s, 20 * s, k_grey,
                         DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
                const wchar_t* opt2[2] = {L"Yes", L"No"};
                const wchar_t* opt3[3] = {L"Return to Title", L"Quit Game", L"Cancel"};
                const float bw = (n_opt == 3 ? 240 : 180) * s, gap = 20 * s, by = (n_opt == 3 ? 160 : 130) * s;
                for (int i = 0; i < n_opt; i++) {
                    float bx = (w - n_opt * bw - (n_opt - 1) * gap) / 2 + i * (bw + gap);
                    fill(bx, by, bw, 60 * s, 12 * s, i == confirm_sel ? k_lime : D2D1::ColorF(1, 1, 1, 0.1f));
                    text(n_opt == 3 ? opt3[i] : opt2[i], bx, by, bw, 60 * s, 24 * s,
                         i == confirm_sel ? D2D1::ColorF(0, 0, 0, 1) : k_white, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            });
        }
    } else if ((title || camp) && !menu && visible) {
        static bool pill_camp = false;
        int w = (int)((camp ? 330 : 300) * s), h = (int)(56 * s);
        static int pill_x = 0, pill_y = 0;
        const int px = org.x + ui_left + (int)(40 * s), py = org.y + ui_bottom - (int)((camp ? 120 : 150) * s);
        const bool redraw = !g_hint_layer.shown || g_hint_layer.w != w || pill_camp != camp || px != pill_x || py != pill_y;
        pill_camp = camp, pill_x = px, pill_y = py;
        if (redraw)
            layer_show(g_hint_layer, px, py, w, h, [&] {
                fill(0, 0, (float)w, (float)h, h / 2.0f, D2D1::ColorF(0, 0, 0, 0.55f));
                fill(10 * s, 10 * s, 36 * s, 36 * s, 18 * s, k_white);
                text(L"−", 10 * s, 8 * s, 36 * s, 36 * s, 30 * s, D2D1::ColorF(0, 0, 0, 1), DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
                text(camp ? L"Title / Quit Game" : L"Exit Game", 60 * s, 0, w - 70 * s, (float)h, 24 * s, k_white, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            });
    } else {
        layer_hide(g_hint_layer);
    }

    // the game sees no buttons while a menu is open, nor the button that closed it until everything is released
    if (swallow && !buttons) swallow = false;
    return menu || confirm || swallow;
}

#ifdef _WIN32
// the subtitles and the P5 battle command panel (p5ui.cpp) as one bitmap (premultiplied, union of both rects)
std::shared_ptr<const SubsBitmap> frame_overlay(uint32_t fw, uint32_t fh) {
    auto subs = subs_frame(fw, fh);
    auto p5 = p5_panel(fw, fh);
    if (!p5) return subs;
    if (!subs) return p5;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    static std::shared_ptr<const SubsBitmap> cached, cs, cp;
    if (cached && cs == subs && cp == p5) return cached;
    auto b = std::make_shared<SubsBitmap>();
    b->x = std::min(subs->x, p5->x), b->y = std::min(subs->y, p5->y);
    b->w = std::max(subs->x + subs->w, p5->x + p5->w) - b->x, b->h = std::max(subs->y + subs->h, p5->y + p5->h) - b->y;
    b->rgba.assign((size_t)b->w * b->h, 0);
    for (const SubsBitmap* s : {p5.get(), subs.get()})
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++) {
                const uint32_t src = s->rgba[(size_t)y * s->w + x], a = src >> 24;
                if (!a) continue;
                uint32_t& d = b->rgba[(size_t)(s->y - b->y + y) * b->w + (s->x - b->x + x)];
                uint32_t r = 0;
                for (int k = 0; k < 32; k += 8) r |= std::min(255u, ((src >> k) & 255) + ((d >> k) & 255) * (255 - a) / 255) << k;
                d = r;
            }
    cached = b, cs = subs, cp = p5;
    return cached;
}
#else
// The present path's overlay for a fw x fh frame: the subtitles plus the menu layers (laid out in window pixels, scaled
// to the frame when it has another size), as one bitmap. Rebuilt only when one of them changed.
std::shared_ptr<const SubsBitmap> frame_overlay(uint32_t fw, uint32_t fh) {
    auto subs = subs_frame(fw, fh);
    int ww, wh;
    struct Placed { std::shared_ptr<const SubsBitmap> b; int x, y; };
    std::vector<Placed> parts;
    {
        std::lock_guard<std::mutex> lk(g_layer_lock);
        for (Layer* l : {&g_menu_layer, &g_hint_layer})
            if (l->shown && l->bmp) parts.push_back({l->bmp, l->x, l->y});
        ww = g_layer_win_w, wh = g_layer_win_h;
    }
    if (parts.empty()) return subs;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);  // two present paths (staging slots, screenshots) may ask at once
    static std::shared_ptr<const SubsBitmap> cached, cached_subs;
    static std::vector<const SubsBitmap*> cached_parts;
    static uint32_t cached_w = 0, cached_h = 0;
    std::vector<const SubsBitmap*> keys;
    for (auto& p : parts) keys.push_back(p.b.get());
    if (cached && cached_subs == subs && cached_parts == keys && cached_w == fw && cached_h == fh) return cached;
    const float sx = ww > 0 ? (float)fw / ww : 1.f, sy = wh > 0 ? (float)fh / wh : 1.f;
    struct R { int x0, y0, x1, y1; };
    std::vector<R> rs;
    R u{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
    auto add = [&](R r) {
        r.x0 = std::clamp(r.x0, 0, (int)fw), r.x1 = std::clamp(r.x1, 0, (int)fw);
        r.y0 = std::clamp(r.y0, 0, (int)fh), r.y1 = std::clamp(r.y1, 0, (int)fh);
        rs.push_back(r);
        if (r.x1 > r.x0 && r.y1 > r.y0) u = {std::min(u.x0, r.x0), std::min(u.y0, r.y0), std::max(u.x1, r.x1), std::max(u.y1, r.y1)};
    };
    if (subs) add({(int)subs->x, (int)subs->y, (int)(subs->x + subs->w), (int)(subs->y + subs->h)});
    for (auto& p : parts) add({(int)(p.x * sx), (int)(p.y * sy), (int)((p.x + p.b->w) * sx), (int)((p.y + p.b->h) * sy)});
    if (u.x1 <= u.x0 || u.y1 <= u.y0) return subs;
    auto out = std::make_shared<SubsBitmap>();
    out->x = (uint32_t)u.x0, out->y = (uint32_t)u.y0, out->w = (uint32_t)(u.x1 - u.x0), out->h = (uint32_t)(u.y1 - u.y0);
    out->rgba.assign((size_t)out->w * out->h, 0);
    // the unscaled rectangle of each part (the source size): scale back into it with nearest samples
    std::vector<std::pair<uint32_t, uint32_t>> src_size;
    if (subs) src_size.push_back({subs->w, subs->h});
    for (auto& p : parts) src_size.push_back({p.b->w, p.b->h});
    auto blend = [&](const SubsBitmap& b, const R& r, const R& full) {  // premultiplied "over"
        const int fwid = full.x1 - full.x0, fhgt = full.y1 - full.y0;
        if (fwid <= 0 || fhgt <= 0) return;
        for (int y = r.y0; y < r.y1; y++) {
            const uint32_t sy_ = std::min<uint32_t>(b.h - 1, (uint32_t)((uint64_t)(y - full.y0) * b.h / fhgt));
            uint32_t* d = out->rgba.data() + (size_t)(y - u.y0) * out->w;
            for (int x = r.x0; x < r.x1; x++) {
                const uint32_t sx_ = std::min<uint32_t>(b.w - 1, (uint32_t)((uint64_t)(x - full.x0) * b.w / fwid));
                const uint32_t s = b.rgba[(size_t)sy_ * b.w + sx_], a = s >> 24;
                uint32_t& o = d[x - u.x0];
                if (!a) continue;
                if (a == 255) { o = s; continue; }
                uint32_t v = 0;
                for (int k = 0; k < 32; k += 8)
                    v |= std::min<uint32_t>(255, ((s >> k) & 255) + (((o >> k) & 255) * (255 - a) + 127) / 255) << k;
                o = v;
            }
        }
    };
    size_t i = 0;
    if (subs) {
        const R full{(int)subs->x, (int)subs->y, (int)(subs->x + subs->w), (int)(subs->y + subs->h)};
        blend(*subs, rs[i++], full);
    }
    for (auto& p : parts) {
        const R full{(int)(p.x * sx), (int)(p.y * sy), (int)((p.x + p.b->w) * sx), (int)((p.y + p.b->h) * sy)};
        blend(*p.b, rs[i++], full);
    }
    cached = out, cached_subs = subs, cached_parts = keys, cached_w = fw, cached_h = fh;
    return out;
}
#endif

}  // namespace rt
