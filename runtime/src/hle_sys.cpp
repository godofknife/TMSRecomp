#include <chrono>
// nn::oe, nn::account, nn::time, nn::diag, nv::, glslc: the application-facing services main imports.
// Hooked at the nn:: API level (the SDK's IPC clients behind them are never reached).
#include "plat.h"
#include <algorithm>

#include <ctime>
#include <future>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>

#include "rt.h"

using namespace rt;
namespace rt { void guest_backtrace(const Cpu& c); }

// ------------------------------------------------------------------------------------------------ nn::oe
HLE_FN(oe_Initialize, "_ZN2nn2oe10InitializeEv") {}
HLE_FN(oe_Enter, "_ZN2nn2oe31EnterExitRequestHandlingSectionEv") {}
HLE_FN(oe_Leave, "_ZN2nn2oe31LeaveExitRequestHandlingSectionEv") {}
HLE_FN(oe_SetCpuBoost, "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE") {}
HLE_FN(oe_SetFocus, "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE") {}
HLE_FN(oe_SetPerfConf, "_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi") {}
HLE_FN(oe_SetResume, "_ZN2nn2oe28SetResumeNotificationEnabledEb") {}
HLE_FN(oe_SetOpModeNotif, "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb") {}
HLE_FN(oe_SetPerfNotif, "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb") {}
HLE_FN(oe_SetInactExt, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb") {}
HLE_FN(oe_IsInactExt, "_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv") { c->r[0] = 0; }
HLE_FN(oe_Copyright, "_ZN2nn2oe30InitializeCopyrightFrameBufferEPvj") { c->r[0] = 0; }
HLE_FN(oe_GetOpMode, "_ZN2nn2oe16GetOperationModeEv") { c->r[0] = 1; }    // console (docked)
HLE_FN(oe_GetPerfMode, "_ZN2nn2oe18GetPerformanceModeEv") { c->r[0] = 1; } // boost (docked)
HLE_FN(oe_GetFocus, "_ZN2nn2oe20GetCurrentFocusStateEv") { c->r[0] = 1; }  // in focus
// Game language (setting `language`, s115, GitHub request: Japanese): the Switch answers the system language here and
// the game picks Message/<JP_Japanese|USen|EUfr|CNzh|TWzh> and Interface/_<JP|US|FR|CN|TW> from it (0x4da19c: ja 0,
// fr 2, zh-Hans 3, zh-Hant 4, anything else English). Setting values: en (default), ja, fr, zh-Hans, zh-Hant (aliases
// jp, cn, tw, japanese, french, chinese...).
const char* rt::language() {
    static const char* code = [] {
        std::string v = getenv("NX_LANGUAGE") ? getenv("NX_LANGUAGE") : "";
        for (auto& ch : v) ch = (char)tolower((unsigned char)ch);
        const char* r = "en-US";
        if (v == "ja" || v == "jp" || v == "japanese") r = "ja";
        else if (v == "fr" || v == "french" || v == "fr-ca") r = "fr";
        else if (v == "zh-hans" || v == "zh-cn" || v == "cn" || v == "chinese" || v == "zh") r = "zh-Hans";
        else if (v == "zh-hant" || v == "zh-tw" || v == "tw") r = "zh-Hant";
        else if (!v.empty() && v.rfind("en", 0) != 0) log("settings: language %s unknown (en, ja, fr, zh-Hans, zh-Hant): English", v.c_str());
        return r;
    }();
    return code;
}
HLE_FN(oe_GetLanguage, "_ZN2nn2oe18GetDesiredLanguageEv") {                 // LanguageCode returned via r0
    const char* l = rt::language();
    memset(GPTR(c->r[0]), 0, 8);
    memcpy(GPTR(c->r[0]), l, strlen(l));
    static bool said = false;
    if (!said) said = true, log("oe: language %s", l);
}
HLE_FN(oe_TotalActive, "_ZN2nn2oe25GetProgramTotalActiveTimeEv") {        // TimeSpan returned via r0
    st64(c->r[0], (uint64_t)GetTickCount64() * 1000000ull);
}
HLE_FN(oe_PopMessage, "_ZN2nn2oe22PopNotificationMessageEv") {
    // Horizon sends FocusStateChanged (15) once after launch; the game's handler then reads the focus state (in
    // focus). After that it blocks: we send nothing else (no suspend / focus changes).
    static std::atomic<bool> sent{false};
    if (!sent.exchange(true)) { c->r[0] = 15; return; }
    log("oe::PopNotificationMessage: blocking forever (lr %s)", symbol_at(c->r[14]));
    for (;;) Sleep(INFINITE);
}

// ------------------------------------------------------------------------------------------------ game log
// The game's own debug log (names.txt game_log): (category mask, format, ...). Retail drops the text; we format it with
// the game's vsnprintf and log it as [game]. NX_GAME_LOG=0 turns it off.
HLE_FN(game_log, "game_log") {
    static const bool off = getenv("NX_GAME_LOG") && !strcmp(getenv("NX_GAME_LOG"), "0");
    // CRI's "#CRIATOM, ..., ExPlayer_SetFile, player, binder, path" line names each voice file the game plays (voice
    // subtitles). CRI builds its trace format at run time ("#CRIATOM..." / "%s, %d, ..."), so with the log off only
    // formats starting with '#' or '%' are formatted.
    if (off && *(const char*)GPTR(c->r[1]) != '#' && *(const char*)GPTR(c->r[1]) != '%') return;
    static thread_local uint32_t buf = hle_alloc(0x400, 4), va = hle_alloc(0x80, 4);
    uint32_t mask = c->r[0], fmt = c->r[1], sp = c->r[13];
    st32(va, c->r[2]);  // va_list: the variadic arguments contiguous, r2, r3, then the caller's stack
    st32(va + 4, c->r[3]);
    for (uint32_t i = 0; i < 30; i++) st32(va + 8 + 4 * i, ld32(sp + 4 * i));
    gcall(c, "game_vsnprintf", {buf, 0x400, fmt, va});
    std::string s = gstr(buf, 0x400);
    while (!s.empty() && (s.back() == 10 || s.back() == 13)) s.pop_back();  // trailing newlines
    if (s.compare(0, 8, "#CRIATOM") == 0 && s.find("ExPlayer_SetFile") != std::string::npos && s.find("/Voice/") != std::string::npos)
        rt::subs_voice(s.c_str() + s.rfind(' ') + 1);
    if (off) return;
    // CRI's per-frame volume trace ("soundvoice_parameter_update_on_changed", 60+ lines/s while a voice fades) buried every other
    // line and cost disk writes each frame: kept only with NX_GAME_LOG=2.
    static const bool all = getenv("NX_GAME_LOG") && !strcmp(getenv("NX_GAME_LOG"), "2");
    if (!all && s.find("_update_on_changed") != std::string::npos) return;
    // CRI's per-sound housekeeping trace: ~28 lines for every sound effect (create / attach / allocate / free ...), 90% of the log.
    // The "ExPlaybackId ... Cue:" line (rigs and bug reports use it), file loads, ACB loads and errors stay.
    if (!all && s.compare(0, 10, "#CRIATOM, ") == 0) {
        static const char* const k_noise[] = {
            "ExCategory_IncrementNumPlaybackCues", "ExCategory_DecrementNumPlaybackCues", "SoundPlayer_StopWithoutRelease",
            "SoundPlayer_PausePlayback", "SoundVoice_Allocate", "SoundPlayer_Start", "PlayerPoolPlayerInfo", "ExPlaybackInfo_AddSound",
            "ExPlayer_Start", "ExPlayer_Create", "ExPlayer_AttachFader", "ExPlayerHn", "ExPlayerConfig", "ExFaderHn", "ExFaderConfig",
            "ExPlaybackInfo_FreeInfo", "criatomsoundvoice_FreeVoice", "PlayerPool_ReleasePlayer", "ExPlaybackSound_FreeSound",
            "ExSequence_AllocateSequenceInfo", "ExSequence_AllocateSequenceBlock", "ExPlayer_DetachFader", "ExPlayer_Destroy",
            "SoundPlayer_SetWaveId", "ExPlayer_SetCueId", "ExSequence_FreeSequenceInfo", "ExSequence_FreeSequenceBlock",
            "Ex3dSource_Create", "Ex3dSourceHn", "Ex3dSourceConfig"};
        size_t a = s.find(", ", 10);  // "#CRIATOM, <time>, <id>, <name>, ..."
        a = a == std::string::npos ? a : s.find(", ", a + 2);
        if (a != std::string::npos) {
            size_t b = s.find(',', a + 2);
            std::string_view name(s.data() + a + 2, (b == std::string::npos ? s.size() : b) - a - 2);
            for (const char* n : k_noise) if (name == n) return;
        }
    }
    log("[game %x] %s", mask, s.c_str());
    if (s.find("Cue:b56_title") != std::string::npos) rt::hero_capture_soon();  // the title screen's music
    // NX_CUE_TRACE=<text>: guest call stack of every game log line containing it (e.g. "Cue:bact_" = who starts a sound)
    static const char* trace = getenv("NX_CUE_TRACE");
    if (trace && s.find(trace) != std::string::npos)  // no frame pointers: words on the stack that follow a BL
        for (uint32_t a = c->r[13], n = 0; a < c->r[13] + 0x800 && n < 24; a += 4) {
            uint32_t v = ld32(a);
            if (v < 0x200004 || v >= 0x7e0000 || (v & 1)) continue;
            uint32_t ins = ld32(v - 4);
            if ((ins & 0x0f000000) == 0x0b000000 || (ins & 0x0ffffff0) == 0x012fff30) log("  stack+%03x %s", a - c->r[13], symbol_at(v)), n++;
        }
}

// ------------------------------------------------------------------------------------------------ frame step
// The game keeps its present interval at gfx+0x58c0 and also uses it as its per-frame time step (float(n) in the field
// and animation updates, 0x203150 / 0x5c0508). NX_GAME_INTERVAL=n replaces every value the game sets, so n=1 makes
// the game step 1/60 s per frame and present every vblank (60 fps at normal speed, unlike NX_PRESENT_INTERVAL=1).
// The other per-frame time is a constant 1/30 s that the game passes as dt to its updaters (camera, tweens, player
// movement, timers, event/battle states): literal-pool words read by vldr, so rewriting them changes every user.
// Found by scanning main for 0x3d088889 and reading each use (NOTES "Milestone 4, session 10").
static const uint32_t DT_LITERALS[] = {
    0x201ea4, 0x208df4, 0x22a430, 0x22be64, 0x259d9c, 0x25ae8c, 0x25b5f4, 0x25cda4, 0x25d548, 0x260520, 0x260af0,
    0x292d68, 0x2e101c, 0x2e18e0, 0x363050, 0x363358, 0x367708, 0x3c1858, 0x3ce588, 0x3cf370, 0x3dd61c, 0x3f2d14,
    0x3f50f4, 0x3f8e64, 0x3fec64, 0x40b694, 0x40b7fc, 0x40be70, 0x462424, 0x49d57c, 0x49db44,
};

// Per-frame smoothing x = keep*x + take*target with keep + take = 1 (literal words): the same response over time
// needs keep^(dt*30). Player walk speed (0x40b594, 0x40b6fc: keep 0.8).
static const struct { uint32_t keep, take; } SMOOTHING[] = {{0x40b690, 0x40b68c}, {0x40b7f4, 0x40b7f8}};

static float g_game_dt = 1 / 30.0f;
static int64_t ms_now() { return (int64_t)GetTickCount64(); }

// Per-frame steps in literal words (a 30 Hz frame's worth each): scaled by dt*30. 0x3bf320: the spinning HUD sprite
// drawn by 0x3bf19c (angle +0x36c += pi/30 per draw, field HUD); 0x4689f0, 0x468a0c: the bobbing map object 0x468884
// (phase +0x140 += 20 / 16 degrees per update; its 0.75 decay per update is an immediate, left as is).
static const uint32_t STEP_LITERALS[] = {0x3bf320, 0x4689f0, 0x468a0c};

// NX_WALK_SPEED=k (ini walk_speed): player field movement x k. The two player move functions (0x40b594 walk,
// 0x40b6fc run) step position by speed x scale x dt, dt read from their own literals 0x40b694 / 0x40b7fc.
static const uint32_t WALK_DT[] = {0x40b694, 0x40b7fc};
static std::atomic<float> g_walk_speed{-1};
float rt::walk_speed() {
    if (g_walk_speed < 0)
        g_walk_speed = getenv("NX_WALK_SPEED") && atof(getenv("NX_WALK_SPEED")) > 0 ? (float)atof(getenv("NX_WALK_SPEED")) : 1;
    return g_walk_speed;
}
void rt::set_walk_speed(float k) {
    g_walk_speed = k;
    float walk = g_game_dt * k;
    for (uint32_t a : WALK_DT) memcpy(GPTR(a), &walk, 4);
}

// NX_FPS: 30 = the game's own pacing (default), 60 = game logic at a fixed 60 Hz, N > 60 = frame rate capped at N with
// a variable frame time, 0 = uncapped. With a variable frame time, ui.cpp pace_present sets each frame's dt to the
// measured time of the previous frame. NX_GAME_INTERVAL=1 (older name) = NX_FPS=60.
static std::atomic<int> g_fps{-1};
int rt::fps_setting() {
    if (g_fps < 0)
        g_fps = getenv("NX_FPS") ? atoi(getenv("NX_FPS"))
              : getenv("NX_GAME_INTERVAL") && atoi(getenv("NX_GAME_INTERVAL")) != 1 ? 30 : 60;  // default 60, also without an ini line
    return g_fps;
}

// control "fps N": switch between 60, capped N > 60 and uncapped (0) while running. 30 has its own script pacing
// (no per-object ticks), so switching to or from 30 is refused.
void rt::set_fps(int n) {
    if (n == 30 || fps_setting() == 30 || n < 0 || (n > 0 && n < 60)) { log("fps: %d refused (now %d)", n, fps_setting()); return; }
    g_fps = n;
    if (n == 60) set_game_dt(1 / 60.0f);
    log("fps: now %d", n);
}

// Battle speed (setting battle_speed 1-3, user request 10-30): in battles the game's time step is k x the frame time,
// so every dt-scaled system (dt literals, 30 Hz ticks, scripts, animation steps) runs k times as fast; the UI keeps
// its 60 Hz ticks. At most one 30 Hz tick per frame is allowed (per-object tick gates step once per frame), so the
// step is capped at 1/30 s: 3x needs 90+ fps (at 60 fps 3x acts as 2x). Not at fps 30 (the game's own pacing).
static float g_base_dt = 1 / 30.0f;  // the step the frame rate asks for
static std::atomic<int> g_battle_speed{0};
static std::atomic<int64_t> g_battle_ui_ms{0};
int rt::battle_speed() {
    if (!g_battle_speed) g_battle_speed = std::clamp(getenv("NX_BATTLE_SPEED") ? atoi(getenv("NX_BATTLE_SPEED")) : 1, 1, 3);
    return g_battle_speed;
}
bool rt::in_battle() { return ms_now() - g_battle_ui_ms < 250; }
void rt::set_battle_speed(int k, bool notice) {
    k = std::clamp(k, 1, 3);
    g_battle_speed = k;
    save_setting("battle_speed", std::to_string(k));
    log("battle speed: %dx", k);
    if (notice) subs_notice(L"Battle speed " + std::to_wstring(k) + (fps_setting() == 30 && k > 1 ? L"x (needs 60+ fps)" : L"x"));
}
// USER 11-08 (2x / 3x on): the target spotlight ran 2-3x too. Speed-up is for the actions, not for the player's choices:
// while the command wheel takes input (btl_wheel_input, also during skill list + target pick) the step stays 1x.
static std::atomic<int64_t> g_btl_input_ms{0};
static bool battle_choosing() { return ms_now() - g_btl_input_ms < 150; }
static float battle_dt(float dt) {
    if (fps_setting() == 30 || battle_speed() == 1 || !in_battle()) return dt;
    static bool was = false;
    const bool choosing = battle_choosing();
    if (choosing != was) was = choosing, log("battle speed: %s", choosing ? "1x while choosing" : "back to the set speed");
    if (choosing) return dt;
    return std::min(dt * battle_speed(), 1 / 30.0f);
}
static void apply_game_dt(float dt);
std::atomic<uint32_t> rt::g_dt_keep{0};
void rt::set_game_dt(float dt) {
    g_base_dt = dt;
    apply_game_dt(battle_dt(dt));
}
static void apply_game_dt(float dt) {
    if (dt == g_game_dt && fps_setting() != 30) return;
    uint32_t bits;
    memcpy(&bits, &dt, 4);
    // debug NX_CONTROL "dtkeep <hex mask>": literal i (bit i) keeps the Switch's 1/30 (bisecting a dt-dependent bug)
    const uint32_t keep_mask = g_dt_keep, b30 = 0x3d088889;
    for (size_t i = 0; i < std::size(DT_LITERALS); i++) st32(DT_LITERALS[i], keep_mask >> i & 1 ? b30 : bits);
    float walk = dt * walk_speed();
    for (uint32_t a : WALK_DT) memcpy(GPTR(a), &walk, 4);
    static float base[std::size(SMOOTHING)];
    static bool have_base = false;
    for (size_t i = 0; i < std::size(SMOOTHING); i++) {
        if (!have_base) memcpy(&base[i], GPTR(SMOOTHING[i].keep), 4);
        float keep = powf(base[i], dt * 30), take = 1 - keep;
        memcpy(GPTR(SMOOTHING[i].keep), &keep, 4);
        memcpy(GPTR(SMOOTHING[i].take), &take, 4);
    }
    static float step_base[std::size(STEP_LITERALS)];
    static bool have_step = false;
    for (size_t i = 0; i < std::size(STEP_LITERALS); i++) {
        if (!have_step) memcpy(&step_base[i], GPTR(STEP_LITERALS[i]), 4);
        float v = step_base[i] * dt * 30;
        memcpy(GPTR(STEP_LITERALS[i]), &v, 4);
    }
    have_step = true;
    have_base = true;
    g_game_dt = dt;
}
// The dt literals alone (not the walk ones: walk_speed scales those), for UiDt below.
static void set_dt_literal_words(float dt) {
    uint32_t bits;
    memcpy(&bits, &dt, 4);
    const uint32_t keep_mask = rt::g_dt_keep, b30 = 0x3d088889;
    for (size_t i = 0; i < std::size(DT_LITERALS); i++)
        if (DT_LITERALS[i] != WALK_DT[0] && DT_LITERALS[i] != WALK_DT[1]) st32(DT_LITERALS[i], keep_mask >> i & 1 ? b30 : bits);
}

// 30 Hz ticks of game time since start (above 30 fps a frame has 0 or 1 ticks): for game code that counts 30 Hz
// frames once per update.
// UI ticks pace menus and HUD (widgets count frames per update). USER RULE (10-29, repeated 11-06): 60 Hz is the
// reference (60 fps = every frame updates), above 60 fps never faster, only fps 30 runs at 30 Hz. Setting ui_speed = 30
// (F1 "Switch speed") is opt-in for the Reddit reports of 11-05; it slows menus and dialogue to the Switch's pace.
static uint32_t g_tick30 = 0, g_tick60 = 0, g_tick_ui30 = 0;
static float g_tick30_left = 1 / 30.0f;  // game time from this frame to the next 30 Hz tick
static std::atomic<int> g_ui_speed{0};  // 30 or 60
int rt::ui_speed() {
    if (!g_ui_speed) g_ui_speed = getenv("NX_UI_SPEED") && atoi(getenv("NX_UI_SPEED")) == 30 ? 30 : 60;
    return g_ui_speed;
}
static std::atomic<float> g_ui_frac{1};
static std::atomic<int> g_ui_smooth{-1};
bool rt::ui_smooth() {
    if (g_ui_smooth < 0) g_ui_smooth = !getenv("NX_UI_SMOOTH") || atoi(getenv("NX_UI_SMOOTH")) != 0;
    return g_ui_smooth != 0;
}
void rt::set_ui_smooth(bool on) { g_ui_smooth = on; }
// While 30 Hz widgets run (battle HUD parts, Topic) sprites glide between 30 Hz ticks, else they would move for half a step and wait.
static std::atomic<float> g_ui_frac30{1};
static std::atomic<int64_t> g_ui30_ms{0};
void rt::ui_clock(uint32_t* tick, float* frac) {
    const bool hud30 = ms_now() - g_ui30_ms < 200;
    *tick = ui_speed() == 60 && !hud30 ? g_tick60 : g_tick_ui30;
    *frac = hud30 ? g_ui_frac30.load() : g_ui_frac.load();
}
void rt::set_ui_speed(int hz) { g_ui_speed = hz == 60 ? 60 : 30; log("ui: speed %d Hz", g_ui_speed.load()); }
static void levelup_poll();
static void ng_plus_requests_poll();
// 99 of every consumable (setting item_max99, default 1; USER 11-21: Life Stone 40, Patra 90, Magic Mirror 5, ...): the
// consumable table Table/ItemUseTable.dat (256 records of 0x2c; +0x22 = stock cap byte, e.g. Life Stone 0x28) is loaded once
// by the item tables object (ctor 0x27ce98 from 0x27f974, stored in *(0x9ef458); +4 = the ItemUseTable file object, data at
// its +0xc). On: every non-zero cap -> 99; off: the file's caps back. Applies live. Performa (ItemMaterialTable) is 99 already.
static void item_cap_poll() {
    static uint32_t table = 0;
    static uint8_t orig[256];
    static int applied = -1;
    if (g_frame_no % 30) return;
    if (!table) {
        const uint32_t slot = ld32(0x9ef458), mgr = slot ? ld32(slot) : 0;
        if (!mgr) return;
        const uint32_t t = mgr + 0x38;
        // sanity: record 2 = Life Stone (buy 900, cap 40) as in the 1.0.0 file
        if (ld32(t + 2 * 0x2c + 0x18) != 900 || ld8(t + 2 * 0x2c + 0x22) != 40) {
            static bool said = false;
            if (!said) said = true, log("items: consumable table not found at %08x (not loaded yet?)", t);
            return;
        }
        table = t;
        for (int i = 0; i < 256; i++) orig[i] = (uint8_t)ld8(table + i * 0x2c + 0x22);
    }
    const bool on = !getenv("NX_ITEM_MAX99") || atoi(getenv("NX_ITEM_MAX99")) != 0;
    if ((int)on == applied) return;
    for (int i = 0; i < 256; i++)
        if (orig[i]) st8(table + i * 0x2c + 0x22, on ? 99 : orig[i]);
    applied = on;
    log("items: consumable stock cap %s (table %08x)", on ? "99" : "the game's", table);
}
static double g_ui_time = 0;  // seconds of frame steps (g_base_dt) since start: the UI's clock, 1x in fast battles
float rt::game_base_dt() { return g_base_dt; }
void rt::frame_begin() {
    static float acc = 0, acc60 = 0, accui = 0;
    levelup_poll();
    item_cap_poll();
    ng_plus_requests_poll();
    if (fps_setting() == 30) { g_tick30++, g_tick60++, g_tick_ui30++; return; }
    g_ui_time += g_base_dt;
    apply_game_dt(battle_dt(g_base_dt));  // battles start and end: the step follows
    acc += g_game_dt;
    while (acc >= 1 / 30.0f - 1e-4f) acc -= 1 / 30.0f, g_tick30++;
    g_tick30_left = std::max(1 / 30.0f - acc, 1e-4f);
    acc60 += g_base_dt;  // the frame rate's step: the UI keeps its speed in fast battles
    if (acc60 >= 1 / 60.0f - 1e-4f) acc60 = std::min(acc60 - 1 / 60.0f, 1 / 60.0f), g_tick60++;  // at most one per frame
    accui += g_base_dt;
    if (accui >= 1 / 30.0f - 1e-4f) accui = std::min(accui - 1 / 30.0f, 1 / 30.0f), g_tick_ui30++;
    const float step = ui_speed() == 60 ? 1 / 60.0f : 1 / 30.0f;
    g_ui_frac = g_base_dt < step * 0.75f ? std::min((ui_speed() == 60 ? acc60 : accui) / step, 1.0f) : 1.0f;  // frames shorter than a tick
    g_ui_frac30 = g_base_dt < 0.75f / 30 ? std::min(accui * 30, 1.0f) : 1.0f;
}

// Battle sequence tasks (attack/skill timelines: when the swing, hit, damage numbers and sounds start) count down a
// delay (+0x10, 30 Hz frames, e.g. 20 = 0.67 s to the swing) by +0x20 (always 1) per update, once per game frame
// (60 fps: actions started twice as early, ahead of the animations and the cut timeline). While a delay runs, step a
// task only when a 30 Hz tick has passed since its own last step. Tasks are not updated every frame (paused
// sequences, siblings waiting in order), so session 12's "step only on tick frames" test could miss every tick: a
// skill's damage task never fired and a later one applied a reused action record (crash with Dia/skills/sessions).
// With no delay left a task runs every frame: the rest of its work is not frame counted (turn changes matched 30).
static std::unordered_map<uint32_t, uint32_t> g_btl_tick, g_ui_tick;  // object address -> tick of its last step; main thread
static bool tick_on(std::unordered_map<uint32_t, uint32_t>& m, uint32_t clock, uint32_t obj) {
    auto [it, added] = m.try_emplace(obj, clock);
    if (!added && it->second == clock) return false;
    it->second = clock;
    if (m.size() > 4096) m.clear();  // stale entries only ever allow one early step
    return true;
}
static bool tick_due(uint32_t obj) { return tick_on(g_btl_tick, g_tick30, obj); }  // a 30 Hz tick since obj's last step (new: yes)
static bool ui_tick_due(uint32_t obj) { return tick_on(g_ui_tick, rt::ui_speed() == 60 ? g_tick60 : g_tick_ui30, obj); }  // same, UI clock
// Gated UI updates (UI manager, camp menu, save / load screens, open Topic, post-battle screens) run on UI ticks, but the dt
// literals hold the frame's step: a time-based widget got 1/240 s per 60 Hz tick at 240 fps, a quarter of real time (USER
// 11-13: dialogue text far slower than the Switch / YouTube; the message window 0x25a510 types by timer +0x128 += dt literal
// 0x25ae8c). While a gated update runs, the literals hold the real time since that update's last run (frame-counting widgets
// are unaffected), then the frame's step again.
struct UiDt {
    bool set = false;
    explicit UiDt(uint32_t key) {
        if (rt::fps_setting() == 30) return;
        static std::unordered_map<uint32_t, double> last;  // main thread
        const double step = rt::ui_speed() == 60 ? 1 / 60.0 : 1 / 30.0;
        auto [it, added] = last.try_emplace(key, g_ui_time - step);
        const float dt = (float)std::clamp(g_ui_time - it->second, (double)g_base_dt, std::max(1 / 30.0, (double)g_base_dt));
        it->second = g_ui_time;
        if (last.size() > 4096) last.clear();
        if (fabsf(dt - g_game_dt) < 1e-7f) return;
        set_dt_literal_words(dt);
        set = true;
        static const bool dbg = getenv("NX_UI_DT_LOG") != nullptr;
        if (dbg) {
            static int n = 0;
            if (n++ < 400) log("ui dt: key %08x %.2f ms (frame %.2f ms)", key, dt * 1000, g_game_dt * 1000);
        }
    }
    ~UiDt() { if (set) set_dt_literal_words(g_game_dt); }
};
// Victory wait (user 11-02: the wipe into RESULTS came before the victory pose ended): battle main state 0xc counts 30
// frames (+0x18) before it lets the result flow start (then on button 0x20 or when the victory sequence reports done,
// ~0.7 s after the victory music). Counted per frame, 240 fps waited 0.125 s instead of the Switch's 1 s: on frames
// without a 30 Hz tick the timer is put back (the state change reads the timer before the decrement, so it waits too).
static uint32_t rate_setting(const char* env);
static std::string hex_word(uint32_t v) { char t[12]; snprintf(t, sizeof t, "%x", v); return t; }
// Weapon (Carnage) Mastery multiplier (setting mastery_rate 1..5; user request 11-02): the battle adds Mastery per action
// into u16 battle +0x548 + 2 * member (damage code 0x2b8bc8, several sites; Lua btlSetForceMastery 0x2c3c1c), which
// btl_main clamps to 50000 every call and the results add to the weapons. Each call, the rise since the last call is
// scaled (shadow copy per battle object; a drop = a new battle). Script Mastery (btlSetForceMastery: the d002_01
// tutorial gives Tsubasa 5500 even when the player escapes; user 11-03: escape at x5 gave 27500) keeps its amount.
static uint32_t g_mastery_b;
static uint16_t g_mastery_prev[7];
static void mastery_scale(uint32_t b) {
    uint32_t& last_b = g_mastery_b;
    uint16_t* prev = g_mastery_prev;
    const uint32_t k = rate_setting("NX_MASTERY_RATE");
    if (b != last_b) { last_b = b; for (int i = 0; i < 7; i++) prev[i] = ld16(b + 0x548 + 2 * i); }
    for (int i = 0; i < 7; i++) {
        uint32_t v = ld16(b + 0x548 + 2 * i);
        if (v > prev[i] && k > 1) {
            const uint32_t nv = std::min<uint32_t>(50000, prev[i] + (v - prev[i]) * k);
            log("battle: mastery member %d +%u x%u -> %u", i, v - prev[i], k, nv);
            st16(b + 0x548 + 2 * i, (uint16_t)nv), v = nv;
        }
        prev[i] = (uint16_t)v;
    }
}
HLE_FN(btl_force_mastery, "btl_force_mastery") {
    const uint32_t b = c->r[0], chr = c->r[1], amount = c->r[2];  // amount x 100 (float) = Mastery points
    mastery_scale(b);  // earned Mastery before the script's grant still counts
    call_unhooked(c, "btl_force_mastery", {b, chr, amount});
    for (int i = 0; i < 7; i++) g_mastery_prev[i] = ld16(b + 0x548 + 2 * i);
    log("battle: script mastery char %u %+d x100 (not scaled)", chr, (int)amount);
}
// Victory sequence (camera + poses, process vtable 0x7d55dc): battle main waits for it in state 0xc. It lasted 129 frames with 30 Hz steps
// (fixdt 30), but its tasks without a delay run per frame: 60 fps left 1.9 s of the Switch's 4.3 s, 240 fps 1 s, so RESULTS cut the poses
// (user 11-06). During the wait the whole tree steps on 30 Hz ticks.
static uint32_t g_btl_state;
static int g_retry_phase = 0;  // battle retry prompt: 0 none, 1 dialog up, 2 answered
static int g_retries = 0;  // battles restarted (debug NX_RETRY_TEST: party at 1 HP before the first retry, enemies after)
static bool g_battle_lost = false;  // the last battle ended in a wipe (ko_revive skips it); cleared at each battle opening
HLE_FN(btl_seq_update, "btl_seq_update") {
    if (fps_setting() != 30 && g_btl_state == 0xc && !tick_due(c->r[0] + 1)) { c->r[0] = 1; return; }
    c->r[0] = call_unhooked(c, "btl_seq_update", {c->r[0]});
}
HLE_FN(btl_main, "btl_main") {
    const uint32_t b = c->r[0];
    mastery_scale(b);
    const bool hold = fps_setting() != 30 && ld32(b) == 0xc && !tick_due(b + 0x18);
    static const bool vdbg = getenv("NX_VICTORY_LOG") != nullptr;  // debug: the object battle main's victory wait asks
    if (vdbg && ld32(b) == 0xc) {
        static uint32_t n = 0;
        const uint32_t o = ld32(ld32(0x9ef790));
        if (!(n++ & 15)) {
            std::string w;
            for (uint32_t i = 0; i < 0x60; i += 4) w += hex_word(ld32(o + i)) + " ";
            log("victory obj %08x vt %08x done fn %08x: %s", o, ld32(o), ld32(ld32(o) + 0xc), w.c_str());
            if (const uint32_t ch = ld32(o + 0x20)) {
                w.clear();
                for (uint32_t i = 0; i < 0x80; i += 4) w += hex_word(ld32(ch + i)) + " ";
                log("victory child %08x: %s", ch, w.c_str());
            }
        }
    }
    // battle retry prompt up: the loss sequence (state 0xc: GAME OVER dims the screen, then black over the dialog) waits
    if (g_retry_phase == 1 && ld32(b) == 0xc) { c->r[0] = 0; return; }
    const uint32_t timer = ld32(b + 0x18);
    c->r[0] = call_unhooked(c, "btl_main", {b});
    if (getenv("NX_RETRY_LOG") && ld32(b) != g_btl_state) {  // debug (battle retry RE): state changes + battle request
        const uint32_t rq = ld32(0x9ef778), fl = ld32(0x9f0464);
        std::string w;
        for (uint32_t i = 0; rq && i < 0x20; i += 2) w += std::to_string(ld16(rq + i)) + " ";
        log("retry: btl_main %08x state %u -> %u; request %08x: %s; jump %08x=%u", b, g_btl_state, ld32(b), rq, w.c_str(), fl, fl ? ld32(fl) : 0);
    }
    g_btl_state = ld32(b);
    if (hold && ld32(b) == 0xc) st32(b + 0x18, timer);
    else if (ld32(b) == 0xd && fps_setting() != 30) log("battle: victory wait over (timer %d)", (int)ld32(b + 0x18));
}
// Pure wait tasks (their per-step action vt+0x38 is a bare "bx lr", e.g. 0x2d682c from 0x2d6804) count their length in
// +0x14 instead of the delay: the finishing-blow sequence 0x2d37cc waits 25 / 35 / 55 Switch frames between the
// FinishCut flashes and before the boss death cut. Per frame, 240 fps started the death cut 0.38 s after the last flash
// (Switch 1.83 s), before btl_finish_cut.lua's FadeOut(WHITE) thread fired, so nothing faded the white back in: the
// screen stayed white through the whole death animation (USER 11-13, Gojuin). Those waits step on 30 Hz ticks too.
static bool btl_task_is_wait(uint32_t t) {
    const uint32_t vt = ld32(t), fn = vt ? ld32(vt + 0x38) : 0;
    return fn && ld32(fn & ~1u) == 0xe12fff1e;  // bx lr
}
HLE_FN(btl_task_step, "btl_task_step") {
    uint32_t t = c->r[0];
    if (fps_setting() != 30 && !ld32(t + 0xc) && (int32_t)ld32(t + 0x10) > 0 && !tick_due(t)) return;
    if (fps_setting() != 30 && !ld32(t + 0xc) && (int32_t)ld32(t + 0x10) <= 0 && (int32_t)ld32(t + 0x14) > 0 &&
        btl_task_is_wait(t) && !tick_due(t)) return;
    call_unhooked(c, "btl_task_step", {t});
}

// Battle processes (the other battle task system: actions, commands, turn flow; base class around 0x280000, 8
// subclasses) keep a state timer +0x18 that every update advances by +0x34 (1) after the state's work, and states end
// at frame counts (e.g. the action process 0x7d56a0 is done at timer >= +0x50 + 7). At 60 fps an action ended and freed
// its record (delete[] in 0x2879b8) while the battle task with the skill's damage was still waiting for it: the crash
// when using skills or sessions. Advance the timer only when a 30 Hz tick has passed since this process's last advance.
// Battle opening (user request): touching a downed (stunned) symbol sets encount +0x18 bit 0x20, and then the player
// acts first only when btl_open_chance >= 50. NX_FIRST_STRIKE=1 (default) makes that a sure first action.
// Battle retry (setting battle_retry, default 1; user request 11-20: "restart the battle from the beginning with items and
// HP / SP intact"): btl_setup_units keeps the game data (**0x9ef37c + 0x10, the 0x535c0 save bytes: party HP / EP, items,
// flags; battle units are copies written back only by RESULTS) and the battle request R (*0x9ef778: +0 / +4 result, +8
// encounter, +0xa.. return map; filled by Lua ChangeBattle 0x448cfc or the field). After a loss btl_task_end 0x2909c8 fades
// out and asks scene_change(0x25) (game over -> title); instead both are put back and the battle scene (the last scene asked
// for before the battle set up) starts again: same encounter, same boss event script.
static uint32_t g_scene_last = 0, g_scene_battle = 0;
static std::vector<uint8_t> g_retry_data;
static uint8_t g_retry_req[0x20];
static uint32_t g_retry_base = 0;
static bool retry_on() { return !getenv("NX_BATTLE_RETRY") || atoi(getenv("NX_BATTLE_RETRY")) != 0; }
static void retry_snapshot() {
    const uint32_t gd = ld32(ld32(0x9ef37c)), rq = ld32(0x9ef778);
    g_scene_battle = g_scene_last;
    if (!gd || !rq) { g_retry_data.clear(); return; }
    g_retry_base = gd + 0x10;
    g_retry_data.assign((const uint8_t*)GPTR(g_retry_base), (const uint8_t*)GPTR(g_retry_base) + 0x535c0);
    memcpy(g_retry_req, GPTR(rq), sizeof g_retry_req);
    log("retry: snapshot (scene %#x, encounter %u)", g_scene_battle, ld16(rq + 8));
}
// The prompt: the game's own Yes / No dialog box (Lua ShowDialogBoxCancelType(1, 2, msg, label) = UI.OpenDialogBox 0x45c3e4:
// dlg = 0x246a20(1) (dialog window, kind 6, made once), 0x2400d0(dlg, 2 = Yes / No, UTF-8 text); the Lua loop takes the choice
// on Pad.Push(OK) from dlg +0x34 (0 = Yes) and closes it with window vt+0x1c). Shown after the GAME OVER animation (battle main
// state 0xf) while btl_task_end is held in state 0; B = No.
static bool g_retry_armed = false;
static uint32_t g_retry_dlg = 0;
static const uint32_t PAD_OK = 0x20, PAD_CANCEL = 0x40;  // 0x25c970 masks (A / B; btl_main skips the victory wait on 0x20)
static uint32_t guest_text(Cpu* c, const char* t) {
    static std::map<std::string, uint32_t> m;
    uint32_t& p = m[t];
    if (!p) p = gcall(c, 0x5a8720, {(uint32_t)strlen(t) + 1}), memcpy(GPTR(p), t, strlen(t) + 1);
    return p;
}
// Lua PlaySe(0, SND_ID) = 0x456734 -> 0x25e840(r0 1, r1 sound index, r2 0, s0 = s1 = 0.0); ids from
// ScriptCommon/Sound/SoundIdTable.lua (the name lookup 0x42e694 reads a Lua table: crashed from here). s91 passed the id in
// r3 and 0 in r1: index 0 = BGM b01_d_daitama, so the dungeon song played over the prompt and into the retry (USER 11-21).
static const uint32_t SE_SY_DECIED = 797, SE_SY_CANSEL = 798, SE_SM_SELECT = 806;
static void play_se(Cpu* c, uint32_t id) {
    const float s0 = c->vr.f[0], s1 = c->vr.f[1];
    c->vr.f[0] = c->vr.f[1] = 0.0f;
    gcall(c, 0x25e840, {1, id, 0});
    c->vr.f[0] = s0, c->vr.f[1] = s1;
}
HLE_FN(btl_task_end, "btl_task_end") {
    // the game's own game-over test (its state 0): battle (**0x9ef428) +8 == 2; story battles that go on after a loss do not
    const uint32_t task = c->r[0], btl = ld32(ld32(0x9ef428));
    const bool lost = ld32(task + 0x18) == 0 && btl && ld32(btl + 8) == 2 && g_btl_state >= 0xc;
    if (lost) custom_bgm_stop("battle lost"), g_battle_lost = true;  // the game stops its own battle track at the loss (USER 11-21: music under the prompt)
    if (lost && g_retry_phase < 2 && retry_on() && !g_retry_data.empty()) {
        const Cpu keep = *c;
        static uint64_t lost_ms = 0;
        if (g_retry_phase == 0 && !lost_ms) lost_ms = ms_now();
        if (g_retry_phase == 0 && ms_now() - lost_ms < 1500) { *c = keep; c->r[0] = call_unhooked(c, "btl_task_end", {task}); return; }
        if (g_retry_phase == 0) {
            lost_ms = 0;
            g_retry_dlg = gcall(c, 0x246a20, {1});
            if (g_retry_dlg) gcall(c, 0x2400d0, {g_retry_dlg, 2, guest_text(c, "Retry the battle?")});
            if (g_retry_dlg) st32(g_retry_dlg + 0x34, 0), play_se(c, SE_SM_SELECT);  // cursor on Yes; ShowDialogBox's sound
            g_retry_phase = g_retry_dlg ? 1 : 2;
            log("retry: prompt %08x", g_retry_dlg);
        } else {
            const bool ok = gcall(c, 0x25c970, {PAD_OK}) != 0, cancel = gcall(c, 0x25c970, {PAD_CANCEL}) != 0;
            if (ok || cancel) {
                const uint32_t ret = ld32(g_retry_dlg + 0x34);
                g_retry_armed = ok && ret == 0;
                play_se(c, ok ? SE_SY_DECIED : SE_SY_CANSEL);
                gcall(c, ld32(ld32(g_retry_dlg) + 0x1c), {g_retry_dlg});
                g_retry_phase = 2;
                log("retry: answer %s (choice %u)", g_retry_armed ? "retry" : "give up", ret);
            }
        }
        *c = keep;
        if (g_retry_phase == 1) { c->r[0] = 0; return; }
    }
    c->r[0] = call_unhooked(c, "btl_task_end", {task});
}
// While the prompt is up, btl_game_over.lua's GameOver() (map colour / light to 0.3, then the GAME OVER sprite that ends black and
// covers the dialog) runs without those effects. Lua C functions: (lua_State*) -> number of results.
HLE_FN(lua_btlSetMapColorScale, "lua_btlSetMapColorScale") {
    if (g_retry_phase == 1) { c->r[0] = 0; return; }
    c->r[0] = call_unhooked(c, "lua_btlSetMapColorScale", {c->r[0]});
}
HLE_FN(lua_btlSetMapLightScale, "lua_btlSetMapLightScale") {
    if (g_retry_phase == 1) { c->r[0] = 0; return; }
    c->r[0] = call_unhooked(c, "lua_btlSetMapLightScale", {c->r[0]});
}
HLE_FN(lua_btlPlaySpriteAnim, "lua_btlPlaySpriteAnim") {
    if (g_retry_phase == 1) { log("retry: GAME OVER sprite skipped (prompt up)"); c->r[0] = 0; return; }
    c->r[0] = call_unhooked(c, "lua_btlPlaySpriteAnim", {c->r[0]});
}
HLE_FN(screen_fade, "screen_fade") {
    if (getenv("NX_RETRY_LOG")) log("retry: screen_fade(%u, %u, %08x, %u) lr %08x phase %d", c->r[0], c->r[1], c->r[2], c->r[3], c->r[14], g_retry_phase);
    // the loss sequence's FadeOut(BLACK, 20) (Lua fade 0x2d02d4, right after the GAME OVER sprite) would hide the prompt
    if (g_retry_phase == 1) { log("retry: fade skipped (prompt up)"); return; }
    c->r[0] = call_unhooked(c, "screen_fade", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
HLE_FN(scene_change, "scene_change") {
    uint32_t id = c->r[0];
    const uint32_t lr = c->r[14];
    log("scene: change to %#x (lr %08x)", id, lr);
    const bool armed = g_retry_armed;
    g_retry_armed = false, g_retry_phase = 0;
    if (id == 0x25 && armed && lr >= 0x2909c8 && lr < 0x290b20 && !g_retry_data.empty() && g_scene_battle &&
        ld32(ld32(0x9ef37c)) + 0x10 == g_retry_base) {  // same game data block as at the snapshot
        memcpy(GPTR(g_retry_base), g_retry_data.data(), g_retry_data.size());
        memcpy(GPTR(ld32(0x9ef778)), g_retry_req, sizeof g_retry_req);
        id = g_scene_battle;
        g_retries++;
        log("retry: lost; game data + request restored, battle scene %#x again", id);
    }
    g_scene_last = id;
    call_unhooked(c, "scene_change", {id});
}
static uint32_t g_open_flags;

HLE_FN(btl_setup_units, "btl_setup_units") {
    uint32_t enc = c->r[1];
    g_open_flags = enc ? ld16(enc + 0x18) : 0;
    retry_snapshot();
    const uint32_t btl = c->r[0];
    call_unhooked(c, "btl_setup_units", {c->r[0], c->r[1], c->r[2], c->r[3]});
    log("battle: opening flags in %04x out %04x", g_open_flags, enc ? ld16(enc + 0x18) : 0);
    // difficulty factors this battle uses (user 11-28: Lunatic set mid-game, escape felt easy): dealt / taken damage,
    // money, base escape % (NOTES "Difficulty": Hard 0.8 / 1.25 / 1 / 1, Lunatic 0.5 / 1.5 / 1 / 1)
    float f[4];
    for (int i = 0; i < 4; i++) { const uint32_t v = ld32(btl + 0x98 + 4 * i); memcpy(&f[i], &v, 4); }
    log("battle: difficulty factors dealt %.2f taken %.2f money %.2f escape base %.0f%%", f[0], f[1], f[2], f[3]);
}
// Autosave after battles (setting autosave, default 1): the battle opening arms it; the first walk/run step in the field
// at least 3 s after the last battle UI / post-battle screen update writes save slot 20 (the last of the game's 20
// slots, Load lists it like any other). Same data as the game's own save (save state machine 0x2694ec): 0x352dd4
// (0x4aad6c syncs the player into the game data, then copies the 0x535c0-byte game data), 0x269708 (header: date,
// play time, location; reads the buffer from save manager +4), then the file. The game's file job and its slot-list
// refresh (0x26a804, save screen widgets) are skipped: the file is written here, system.bin (last slot) is untouched.
static std::atomic<bool> g_autosave_armed{false};
static void battle_ui_seen() { g_battle_ui_ms = ms_now(); }
// Level-up refill (setting levelup_refill, default 1; user request 10-31): party members whose level went up in the
// battle get full HP and EP. The persistent party records live in the game data (**0x9ef37c + 0x10, the 0x535c0 bytes
// a save holds): record k at +0x350ac + k * 0x360: +4 id (u16) | ? (u16), +8 level, +0xc HP, +0x10 max HP, +0x14 EP,
// +0x18 max EP (u32; HP/EP found in save_20.bin by the HUD values 10-31; level = +8: 2 for both members after the
// prologue battles, 0 for a member who has not joined, checked 11-01). Battles work on a copy (unit +0x818) written back after the results, so
// levels are taken at the battle opening and checked every frame until the first field step after it (the autosave
// moment, before the save is written): the refill lands the frame the game writes the new level back (user 11-01: the
// camp menu opened right after the battle still showed the old HP when the refill waited for the first step).
constexpr uint32_t PARTY_REC = 0x350ac, PARTY_STRIDE = 0x360, PARTY_MAX = 16;
static uint32_t g_levels_before[PARTY_MAX];
static std::atomic<bool> g_levelup_armed{false};
static uint32_t party_rec(int k) {
    const uint32_t p = ld32(0x9ef37c), gd = p ? ld32(p) : 0;
    return gd ? gd + 0x10 + PARTY_REC + k * PARTY_STRIDE : 0;
}
static std::vector<uint8_t> g_gd_snap;  // debug NX_GD_DIFF=1: log every changed game data word from battle opening to the first field step (found the Mastery fields, s49)
static void gd_diff(bool start) {
    if (!getenv("NX_GD_DIFF")) return;
    const uint32_t p = ld32(0x9ef37c), gd = p ? ld32(p) + 0x10 : 0;
    if (!gd) return;
    const uint8_t* m = (const uint8_t*)GPTR(gd);
    if (start) { g_gd_snap.assign(m, m + 0x535c0); log("gd diff: game data at %08x", gd); return; }
    if (g_gd_snap.empty()) return;
    for (uint32_t o = 0; o < 0x535c0; o += 4)
        if (memcmp(m + o, &g_gd_snap[o], 4)) {
            uint32_t a, b; memcpy(&a, &g_gd_snap[o], 4); memcpy(&b, m + o, 4);
            log("gd diff +%05x: %08x -> %08x", o, a, b);
        }
}
static void levels_snapshot() {
    gd_diff(true);
    const int test = getenv("NX_LEVELUP_TEST") ? atoi(getenv("NX_LEVELUP_TEST")) : 0;  // test: pretend a level-up
    // test: NX_LEVELUP_EXP=n sets the joined members' EXP (+0x64; 20 in save 2 at level 2, 66 in a level 3 save) so the
    // game's own result screens level them up
    const int exp = getenv("NX_LEVELUP_EXP") ? atoi(getenv("NX_LEVELUP_EXP")) : 0;
    for (int k = 0; k < (int)PARTY_MAX; k++) {
        const uint32_t r = party_rec(k);
        if (exp && r && ld32(r + 8)) st32(r + 0x64, exp);
        g_levels_before[k] = r ? ld32(r + 8) - test : 0;
    }
    g_levelup_armed = true;
    if (const uint32_t r = party_rec(0))
        log("level-up refill: battle start, record 0 level %u HP %u/%u EP %u/%u EXP %u", ld32(r + 8), ld32(r + 0xc), ld32(r + 0x10), ld32(r + 0x14), ld32(r + 0x18), ld32(r + 0x64));
}
// Revive after battles (setting ko_revive, default 1; USER 11-21 "like many JRPGs"): joined members at 0 HP get 1 HP once the
// battle is over and not lost (battle main state >= 0xd: RESULTS / escape; before it the records are the pre-battle copies, and
// a Sub Cast member swapped in mid-battle would come back alive). Record +0x52 = 1 for the KO'd member in the user's 11-21
// autosave (Touma, HP 0; 0 for the others): cleared with the HP. field: the first field step after the battle (escapes too).
static void ko_revive(bool field) {
    if (getenv("NX_KO_REVIVE") && !atoi(getenv("NX_KO_REVIVE"))) return;
    if (g_battle_lost || (!field && g_btl_state < 0xd)) return;
    for (int k = 0; k < (int)PARTY_MAX; k++) {
        const uint32_t r = party_rec(k);
        if (!r) return;
        const uint32_t lv = ld32(r + 8), mhp = ld32(r + 0x10);
        if (!lv || lv > 99 || !mhp || mhp > 99999 || ld32(r + 0xc)) continue;
        log("ko revive: party record %d (id %u) HP 0 -> 1, KO byte %u -> 0", k, ld16(r + 4), ld8(r + 0x52));
        st32(r + 0xc, 1), st8(r + 0x52, 0);
    }
}
static void levelup_refill() {
    if (getenv("NX_LEVELUP_REFILL") && !atoi(getenv("NX_LEVELUP_REFILL"))) return;
    for (int k = 0; k < (int)PARTY_MAX; k++) {
        const uint32_t r = party_rec(k);
        if (!r) return;
        const uint32_t lv = ld32(r + 8), mhp = ld32(r + 0x10), mep = ld32(r + 0x18);
        if (!g_levels_before[k] || lv <= g_levels_before[k] || lv > 99 || !mhp || mhp > 99999 || mep > 99999) continue;
        if (ld32(r + 0xc) == mhp && ld32(r + 0x14) == mep) continue;
        log("level-up refill: party record %d level %u -> %u: HP %u -> %u, EP %u -> %u", k, g_levels_before[k], lv,
            ld32(r + 0xc), mhp, ld32(r + 0x14), mep);
        st32(r + 0xc, mhp), st32(r + 0x14, mep);
    }
}
static void levelup_poll() { if (g_levelup_armed) ko_revive(false), levelup_refill(); }
static void autosave_try(Cpu* c) {
    if (!g_autosave_armed || ms_now() - g_battle_ui_ms < 3000) return;
    g_autosave_armed = false;
    gd_diff(false);
    if (const uint32_t r = party_rec(0))
        log("level-up refill: first field step, record 0 level %u HP %u/%u EP %u/%u EXP %u", ld32(r + 8), ld32(r + 0xc), ld32(r + 0x10), ld32(r + 0x14), ld32(r + 0x18), ld32(r + 0x64));
    ko_revive(true);
    levelup_refill();
    g_levelup_armed = false;
    if (getenv("NX_AUTOSAVE") && !atoi(getenv("NX_AUTOSAVE"))) return;
    const uint32_t mgr = ld32(0x9ef5fc);  // save manager (static 0x9f3d38): +4 buffer, +0xc state, +0x2c busy
    if (!mgr || ld32(mgr + 4) || ld32(mgr + 0x2c)) { log("autosave: skipped (a save is running)"); return; }
    const Cpu keep = *c;  // probe: leave the Cpu as found
    const uint32_t size = 0x535c0, buf = gcall(c, 0x5a8720, {size});  // the game's operator new
    if (buf) {
        st32(mgr + 4, buf);
        gcall(c, 0x352dd4, {buf});
        gcall(c, 0x269708, {mgr, buf});
        st32(mgr + 4, 0);
        // the file write (write-through: tens of ms) runs off the game thread; on it, it was a hitch after battles
        std::vector<uint8_t> data((const uint8_t*)GPTR(buf), (const uint8_t*)GPTR(buf) + size);
        gcall(c, 0x5a87f8, {buf});
        std::thread([data = std::move(data)] {
#ifdef _WIN32
            const std::string dir = getenv("NX_SAVE_DIR") ? std::string(getenv("NX_SAVE_DIR")) + "\\" : g_data_dir + "\\save\\", tmp = dir + "save_20.tmp", dst = dir + "save_20.bin";
#else
            const std::string dir = getenv("NX_SAVE_DIR") ? std::string(getenv("NX_SAVE_DIR")) + "/" : g_data_dir + "/save/", tmp = dir + "save_20.tmp", dst = dir + "save_20.bin";
#endif
            FILE* f = fopen(tmp.c_str(), "wb");
            bool ok = f && fwrite(data.data(), 1, data.size(), f) == data.size();
            if (f) ok = fclose(f) == 0 && ok;
            ok = ok && MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            log(ok ? "autosave: slot 20 written (%s)" : "autosave: FAILED writing %s", dst.c_str());
        }).detach();
    }
    *c = keep;
}
PROBE_FN(player_walk, "player_walk") { autosave_try(c); }
PROBE_FN(player_run, "player_run") { autosave_try(c); }

// Guard recovery (setting guard_regen, default 1; user request 10-30): a party member who guards (skill 6) gets 10% of
// max HP and EP back, on top of the game's own small EP gain. Action record: actor unit id +4, target +8, skill +0xc.
// Unit = 0x2b4c2c(battle (static via 0x9ef428), id); its data = unit vt+0x14c: +2 party slot (enemies 10), +0xc HP,
// +0x10 max HP, +0x14 EP, +0x18 max EP (u32; matched to the HUD 10-30). NX_DEBUG_BATTLE=1 logs every action.
static const uint16_t SKILL_GUARD = 6;
HLE_FN(btl_apply_action, "btl_apply_action") {
    const uint32_t rec = c->r[1], actor = ld32(rec + 4);
    const uint16_t skill = ld16(rec + 0xc);
    const bool debug = getenv("NX_DEBUG_BATTLE") != nullptr;
    const bool regen = skill == SKILL_GUARD && (!getenv("NX_GUARD_REGEN") || atoi(getenv("NX_GUARD_REGEN")) != 0);
    c->r[0] = call_unhooked(c, "btl_apply_action", {c->r[0], c->r[1]});
    if (!debug && !regen) return;
    const Cpu keep = *c;
    const uint32_t btl = ld32(ld32(0x9ef428));
    const uint32_t unit = btl ? gcall(c, 0x2b4c2c, {btl, actor}) : 0;
    const uint32_t data = unit ? gcall(c, ld32(ld32(unit) + 0x14c), {unit}) : 0;
    *c = keep;
    if (debug) {
        std::string w;
        for (int i = 0; data && i < 0x60; i += 2) w += std::to_string(ld16(data + i)) + " ";
        log("battle: action actor %u target %d skill %u unit %08x data %08x: %s", actor, (int)ld32(rec + 8), skill, unit, data, w.c_str());
        std::string rw;
        for (int i = 0; i < 0x40; i += 4) rw += std::to_string(i) + ":" + std::to_string(ld32(rec + i)) + " ";
        log("battle: action record %08x: %s", rec, rw.c_str());
        const Cpu keep3 = *c;
        const uint32_t tu = btl && (int)ld32(rec + 8) >= 0 ? gcall(c, 0x2b4c2c, {btl, ld32(rec + 8)}) : 0;
        const uint32_t td = tu ? gcall(c, ld32(ld32(tu) + 0x14c), {tu}) : 0;
        *c = keep3;
        if (td) log("battle: target hp %u / %u", ld32(td + 0xc), ld32(td + 0x10));
        if ((getenv("NX_ENEMY_HP1") || getenv("NX_PARTY_HP1") || getenv("NX_RETRY_TEST") || getenv("NX_KO_TEST")) && btl)  // debug: enemies (NX_ENEMY_HP1) / party (NX_PARTY_HP1) at 1 HP; NX_KO_TEST=id: that member at 0 HP
            for (uint32_t id = 0; id < 16; id++) {  // enemies follow the party's actors (3 when three fight): skip party ids 1..7
                const Cpu keep4 = *c;
                const uint32_t eu = gcall(c, 0x2b4c2c, {btl, id});
                const uint32_t ed = eu ? gcall(c, ld32(ld32(eu) + 0x14c), {eu}) : 0;
                *c = keep4;
                if (ed && ld16(ed) - 1u >= 7 && (getenv("NX_ENEMY_HP1") || (getenv("NX_RETRY_TEST") && g_retries)) && ld32(ed + 0xc) > 1) st32(ed + 0xc, 1);
                if (ed && ld16(ed) - 1u < 7 && (getenv("NX_PARTY_HP1") || (getenv("NX_RETRY_TEST") && !g_retries)) && ld32(ed + 0xc) > 1) st32(ed + 0xc, 1);  // debug: fast wipes
                if (ed && getenv("NX_KO_TEST") && ld16(ed) == (uint32_t)atoi(getenv("NX_KO_TEST")) && ld32(ed + 0xc)) st32(ed + 0xc, 0);
            }
    }
    if (regen && data && ld16(data + 2) < 10 && ld32(data + 0xc) > 0) {
        const uint32_t hp = ld32(data + 0xc), mhp = ld32(data + 0x10), ep = ld32(data + 0x14), mep = ld32(data + 0x18);
        const uint32_t nhp = std::min(mhp, hp + (mhp + 9) / 10), nep = std::min(mep, ep + (mep + 9) / 10);
        st32(data + 0xc, std::max(hp, nhp)), st32(data + 0x14, std::max(ep, nep));
        // The party HUD only re-reads the unit on its next turn: tell it now, as the game's own heals do. HUD = the
        // portrait gauges (entries of 0xb4, found by unit key s16 unit+0x818 via 0x24d1e8); 0x24cf2c / 0x24cf84 = set
        // HP / EP with the refill animation.
        uint32_t hud = ld32(0x9ef770);
        hud = hud ? ld32(hud) : 0;
        hud = hud ? ld32(hud + 0x18) : 0;
        hud = hud ? ld32(hud) : 0;
        const Cpu keep2 = *c;
        const uint32_t entry = hud ? gcall(c, 0x24d1e8, {hud, (uint32_t)(int32_t)(int16_t)ld16(unit + 0x818)}) : 0;
        if (entry) gcall(c, 0x24cf2c, {entry, nhp}), gcall(c, 0x24cf84, {entry, nep});
        *c = keep2;
        log("battle: guard recovery: HP %u -> %u / %u, EP %u -> %u / %u (hud %08x entry %08x)", hp, nhp, mhp, ep, nep, mep, hud, entry);
    }
}
// EXP / money multipliers (settings exp_rate, money_rate: 1..5; user request 11-02): btl_add_reward runs once per
// defeated enemy (and for Lua btlSetExpMoney) and adds its EXP and money to the battle's result totals, so scaling its
// arguments scales the results screen and every member's EXP (the game's own level-gap rule and caps still apply).
static uint32_t rate_setting(const char* env) {
    const char* v = getenv(env);
    return v ? (uint32_t)std::clamp(atoi(v), 1, 5) : 1;
}
// Sub Cast EXP (setting sub_exp, default 1 = the game's rule; user request 11-20): btl_add_reward gives the enemy's EXP to
// all seven members (its loop: party list **(lit 0x2cb4f8 + 0x2cb2f4) + 0x350c0, 0x360 per member, +0 character id 1..7;
// member k's battle EXP at battle +0x52c + 4k; unit = 0x2b3560(battle, id)), front line or Sub Cast. The front line = battle
// actors 0..2 (0x2b4c2c(battle, actor); enemies follow them, ids 8+, seen s90). sub_exp = 0: a member who is not on the
// front line when the enemy falls keeps the EXP it had.
// EXP level gap (setting exp_gap, default 1 = the game's rule; user request 11-20: Touma needed 179 of a 630 EXP battle and got
// less): a member ABOVE the enemy's level (unit data +8) gets 100 - 2.5 d (d + 1) % (d 5 = 25%, 6+ = 0). exp_gap = 0: such a
// member gets the full EXP instead (the game's own bonus for unit flag +0x83c & 0x40000000 x (data +0x156 + 10) % kept).
// Members at or below the enemy's level keep the game's amount (100% .. 200%).
HLE_FN(btl_add_reward, "btl_add_reward") {
    const uint32_t ke = rate_setting("NX_EXP_RATE"), km = rate_setting("NX_MONEY_RATE"), btl = c->r[0], lvl = c->r[1], xp = c->r[2],
                   money = c->r[3];
    const bool sub_exp = !getenv("NX_SUB_EXP") || atoi(getenv("NX_SUB_EXP")) != 0;
    const bool gap = !getenv("NX_EXP_GAP") || atoi(getenv("NX_EXP_GAP")) != 0;
    const uint32_t gp = ld32(ld32(ld32(0x2cb4f8) + 0x2cb2f4));
    uint32_t before[7] = {}, ids[7] = {}, front[3] = {}, level[7] = {}, full[7] = {};
    bool is_front[7] = {}, joined[7] = {};
    const Cpu keep = *c;
    for (uint32_t a = 0; a < 3; a++) front[a] = gcall(c, 0x2b4c2c, {btl, a});
    for (int k = 0; k < 7 && gp; k++) {
        ids[k] = ld16(gp + 0x350c0 + k * 0x360), before[k] = ld32(btl + 0x52c + 4 * k);
        if (ids[k] - 1 >= 7) continue;
        joined[k] = gcall(c, 0x35304c, {ids[k]}) != 0;
        const uint32_t unit = joined[k] ? gcall(c, 0x2b3560, {btl, ids[k]}) : 0;
        const uint32_t data = unit ? gcall(c, ld32(ld32(unit) + 0x14c), {unit}) : 0;
        if (!data) { joined[k] = false; continue; }
        is_front[k] = unit == front[0] || unit == front[1] || unit == front[2];
        level[k] = *(uint8_t*)GPTR(data + 8);
        double f = (double)xp * ke;
        if (ld32(unit + 0x83c) & 0x40000000) f = f * ((int16_t)ld16(data + 0x156) + 10) / 100;
        full[k] = (uint32_t)std::max(f, 1.0);
    }
    *c = keep;
    c->r[0] = call_unhooked(c, "btl_add_reward", {btl, lvl, xp * ke, money * km});
    std::string m;
    for (int k = 0; k < 7 && gp; k++) {
        if (!joined[k]) continue;
        uint32_t after = ld32(btl + 0x52c + 4 * k);
        const bool lift = !gap && level[k] > lvl && after - before[k] < full[k];
        if (lift) after = std::min<uint32_t>(before[k] + full[k], 99999999), st32(btl + 0x52c + 4 * k, after);
        const bool undo = !sub_exp && !is_front[k];
        if (undo) st32(btl + 0x52c + 4 * k, before[k]);
        m += " " + std::to_string(ids[k]) + "/L" + std::to_string(level[k]) + (is_front[k] ? "" : "(sub)") + ":+" +
             std::to_string(after - before[k]) + (lift ? " full" : "") + (undo ? " undone" : "");
    }
    log("battle: reward level %u EXP %u x%u money %u x%u; members%s", lvl, xp, ke, money, km, m.c_str());
}
// Overworld costume (setting field_costume, default 0 = the game's rule; user request 11-24): outside battle a party member
// wears the PlayerDefaultCostumeTable row costume_default_row picks by story flags (+4: Itsuki 3 / 4 everyday clothes, Tsubasa
// 0x26 school uniform...); battles use the equipped dress (0x3613c0: its ItemDressTable costume, 1 = the Carnage outfit = no
// dress). The model loader 0x3cc560 and its reload check 0x407ea4 both read +4 of this row, so the dress goes in there: the
// model then reloads by itself after a change in the Equip menu. The table is shared: +4 is put back first on every call.
// Kept as is: no dress (1), the ids 0x3613c0 itself pins to 0x31 (0x3ec / 0x3ed / 0x3f2), members not joined (0x3535f4, as
// 0x3cb2ac checks), a costume without a skin row (0x350688 null: the loader would read through null).
HLE_FN(costume_default_row, "costume_default_row") {
    static std::mutex lock;
    static std::unordered_map<uint32_t, uint16_t> orig;  // row -> the table's costume
    const uint32_t chr = c->r[0], row = call_unhooked(c, "costume_default_row", {chr});
    if (row) {
        std::lock_guard<std::mutex> l(lock);
        const uint16_t story = orig.try_emplace(row, ld16(row + 4)).first->second;
        uint16_t want = story;
        const Cpu keep = *c;
        if (getenv("NX_FIELD_COSTUME") && atoi(getenv("NX_FIELD_COSTUME")) != 0 &&
            !(chr - 0x3ecu < 7 && ((1u << (chr - 0x3ec)) & 0x43)) && gcall(c, 0x3535f4, {chr})) {
            const uint16_t dress = (uint16_t)gcall(c, 0x3613c0, {chr});
            if (dress > 1 && dress != story && gcall(c, 0x350688, {chr, dress})) want = dress;
        }
        *c = keep;
        if (ld16(row + 4) != want) {
            st16(row + 4, want);
            log("costume: character %u outside battle wears %u (story clothes %u)", chr, want, story);
        }
    }
    c->r[0] = row;
}
// Unlock all difficulties (setting all_difficulties, default 1; user request 11-24): system data **0x9ef37c + 0xa6b95 =
// Lunatic unlocked (set at game clear, 0x269708), + 0xa6b96 = Friendly unlocked (set by the Game Over notice on Easy,
// 0x465dc0). Their readers set both first: the title menu (mirrors them into flags 0x55 / 0x56 for New Game), the Lua
// checks, and the Settings page, whose difficulty row (10) also gets the full range Friendly .. Lunatic (the game: Easy ..
// Hard, and locked at Lunatic once there). The system save keeps the bytes, as the game's own unlocks.
static bool all_difficulties() { return !getenv("NX_ALL_DIFFICULTIES") || atoi(getenv("NX_ALL_DIFFICULTIES")) != 0; }
static void unlock_difficulties() {
    const uint32_t p = ld32(0x9ef37c), g = p ? ld32(p) : 0;
    if (!g || !all_difficulties()) return;
    uint8_t* b = (uint8_t*)GPTR(g + 0xa6b95);
    if (!b[0] || !b[1]) log("difficulty: Friendly and Lunatic unlocked (were %u / %u)", b[1], b[0]), b[0] = b[1] = 1;
}
PROBE_FN(title_menu_update, "title_menu_update") { unlock_difficulties(); }
PROBE_FN(title_menu_return, "title_menu_return") { unlock_difficulties(); }
PROBE_FN(lua_IsSysDiffFriendlyUnlock, "lua_IsSysDiffFriendlyUnlock") { unlock_difficulties(); }
PROBE_FN(lua_IsSysDiffLunaticUnlock, "lua_IsSysDiffLunaticUnlock") { unlock_difficulties(); }
HLE_FN(camp_settings_fill, "camp_settings_fill") {
    const uint32_t scr = c->r[0];
    unlock_difficulties();
    c->r[0] = call_unhooked(c, "camp_settings_fill", {c->r[0], c->r[1], c->r[2], c->r[3]});
    if (all_difficulties()) st32(scr + 0x118, 1), st32(scr + 0x11c, 0), st32(scr + 0x120, 4);  // row 10: arrows, min, max
}
// Ultrawide (vk.cpp wide_factor, setting aspect): every perspective projection of the game's 16:9 shape (aspect 1.7778:
// field, battle, event cameras; the scene targets are widened to match) gets the wider aspect before the matrix is built:
// the same vertical view, more at the sides (Hor+). The object keeps the new aspect (the game sets it once), so frustum
// culling, which reads the same object, widens too. Other aspects (square shadow / reflection cameras) stay.
namespace vk { float wide_factor(); }
HLE_FN(cam_proj_update, "cam_proj_update") {
    const uint32_t p = c->r[0];
    const float f = vk::wide_factor();
    static const bool nocam = getenv("NX_UW_NOCAM") != nullptr;  // debug
    if (f > 1 && ld32(p + 0xc4)) {  // debug log: each distinct camera / type / aspect once
        static std::unordered_set<uint64_t> seen;
        const uint64_t k = (uint64_t)p << 32 ^ ld32(p + 0xc0) << 24 ^ ld32(p + 0xa4);
        if (seen.size() < 96 && seen.insert(k).second) {
            float a, fov; uint32_t aw = ld32(p + 0xa4), fw = ld32(p + 0xa0); memcpy(&a, &aw, 4), memcpy(&fov, &fw, 4);
            std::string m;
            for (uint32_t o = 0x10; o < 0x50; o += 4) { float v; uint32_t vw = ld32(p + o); memcpy(&v, &vw, 4); char b[24]; snprintf(b, sizeof b, " %g", v); m += b; }
            log("uw: camera seen %08x type %u fov %.3f aspect %.4f (lr %08x) m:%s", p, ld32(p + 0xc0), fov, a, c->r[14], m.c_str());
        }
    }
    static const uint32_t types = getenv("NX_UW_TYPES") ? (uint32_t)strtoul(getenv("NX_UW_TYPES"), nullptr, 0) : 1;  // debug: camera types widened (bit mask)
    if (f > 1 && !nocam && ld32(p + 0xc4) && ld32(p + 0xc0) < 32 && (types >> ld32(p + 0xc0) & 1)) {
        float a;
        uint32_t w = ld32(p + 0xa4);
        memcpy(&a, &w, 4);
        if (fabsf(a - 16.f / 9) < 0.01f) {
            static int n = 0;
            if (n++ < 40) { float fov; uint32_t fw = ld32(p + 0xa0); memcpy(&fov, &fw, 4); log("uw: camera %08x fov %.3f aspect -> wide (lr %08x)", p, fov, c->r[14]); }
            a = 16.f / 9 * f;
            memcpy(&w, &a, 4);
            st32(p + 0xa4, w);
        }
    }
    call_unhooked(c, "cam_proj_update", {p});
}
namespace rt { extern std::atomic<int> g_shot_request; std::atomic<int64_t> g_btl_open_ms{0}; }  // g_btl_open_ms: steady-clock ms of the last battle opening (vk_replay: start banner text)
using rt::g_shot_request;
HLE_FN(btl_open_chance, "btl_open_chance") {
    g_autosave_armed = true;
    g_battle_lost = false;
    levels_snapshot();
    const bool on = !getenv("NX_FIRST_STRIKE") || atoi(getenv("NX_FIRST_STRIKE")) != 0;  // the settings menu changes it
    int32_t r = (int32_t)call_unhooked(c, "btl_open_chance", {c->r[0], c->r[1], c->r[2]});
    if (const char* f = getenv("NX_OPEN_FORCE")) r = atoi(f);  // debug: force the opening roll (< ~10 surprise, >= ~90 first strike)
    if (getenv("NX_UW_ARM_BATTLE")) { _putenv("NX_UW_FRAME=0"); g_shot_request += 120; }  // debug: draw log + 120 frame shots from here
    rt::g_btl_open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();  // ultrawide: start banner text (vk_replay)
    log("battle: opening chance %d (contact flags %04x)", r, g_open_flags);
    if (on && (g_open_flags & 0x20) && r < 50) r = 50;
    c->r[0] = (uint32_t)r;
}

// Settings menu (overlay.cpp): post effects the user turned off skip their render step, as when the game disables them.
#define FX_HOOK(fn, fx) HLE_FN(fn, #fn) { if (fx_enabled(fx)) call_unhooked(c, #fn, {c->r[0], c->r[1], c->r[2], c->r[3]}); }
FX_HOOK(fx_dof_render, FX_DOF)
FX_HOOK(fx_glare_render, FX_BLOOM)
FX_HOOK(fx_glow_render, FX_GLOW)
FX_HOOK(fx_vignette_render, FX_VIGNETTE)
FX_HOOK(fx_fog_render, FX_FOG)
FX_HOOK(fx_edge_render, FX_OUTLINE)
// Battle music randomizer (setting random_battle_bgm, default 0; 1 game tracks, 2 custom, 3 mixed). bgm_play r2 =
// sound table index (names in ScriptCommon/Sound/SoundIdTable.lua, same order): a battle track (55 NORMAL, 56 BOSS,
// 57 GANEFU, 59 EVENT, 60 EXTRA, 61 QUEST) becomes a random one of them. The final boss (58 MEDIUS, 47 MEDIUS2) keeps
// its music. Custom music (ext_bgm.cpp): the battle track and the victory jingle (65) on that slot are stopped
// through the game's own path (index -1 = stop with the push bookkeeping) and our ADX plays; any other track on the
// slot or the field music restore (bgm_restore) gives the music back to the game.
// Boss and story fights (any pool track but 55 NORMAL, e.g. Gojuin on 56 BOSS) keep their music unless setting
// random_bgm_bosses = 1 (user report 11-02: a random track on a story boss "is kind of funny").
static int g_custom_slot = -1;
HLE_FN(bgm_play, "bgm_play") {
    static const uint32_t pool[] = {55, 56, 57, 59, 60, 61};
    uint32_t id = c->r[2];
    const bool bosses = getenv("NX_RANDOM_BGM_BOSSES") && atoi(getenv("NX_RANDOM_BGM_BOSSES"));
    const bool battle = (id == 55 || bosses) && std::find(std::begin(pool), std::end(pool), id) != std::end(pool);
    if ((battle && custom_bgm_battle()) || (id == 65 && custom_bgm_active() && custom_bgm_victory())) {
        g_custom_slot = (int)c->r[1];
        log("bgm: %u on slot %u -> custom music", id, c->r[1]);
        c->r[2] = ~0u;
        c->r[0] = call_unhooked(c, "bgm_play", {c->r[0], c->r[1], c->r[2], c->r[3]});
        return;
    }
    if ((int32_t)id >= 0 && (int)c->r[1] == g_custom_slot) custom_bgm_stop("another track on the slot");
    if (custom_bgm_mode() >= 1 && battle) {
        static uint32_t seed = (uint32_t)GetTickCount();
        seed = seed * 1103515245u + 12345u;
        c->r[2] = pool[(seed >> 16) % 6];
    }
    static uint32_t last = ~0u;
    if (id != last) log("bgm: %u (played %u, slot %u)", id, c->r[2], c->r[1]);
    last = id;
    c->r[0] = call_unhooked(c, "bgm_play", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
// Battles start their music by name (bgm_play_name, from the encount data): a pooled battle track goes through
// bgm_play by index instead (same arguments; s0-s2 are untouched), where the randomizer above picks one.
HLE_FN(bgm_play_name, "bgm_play_name") {
    if (getenv("NX_BGM_LOG") && c->r[2]) log("bgm: by name slot %u cue %s acb %08x lr %08x", c->r[1], (const char*)GPTR(c->r[2]), c->r[3], c->r[14]);
    static const uint32_t pool[] = {55, 56, 57, 59, 60, 61};
    uint32_t id = ~0u;
    if (custom_bgm_mode() && c->r[2] && *(const char*)GPTR(c->r[2])) {
        const Cpu keep = *c;  // the lookup clobbers registers (and s0-s2, the volumes)
        id = gcall(c, 0x42e694, {c->r[2]});  // name -> sound table index
        *c = keep;
    }
    if (std::find(std::begin(pool), std::end(pool), id) != std::end(pool)) {
        c->r[2] = id;
        bgm_play(c);
        return;
    }
    c->r[0] = call_unhooked(c, "bgm_play_name", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
// CRI Mana movies on the manual timer (no audio clock; e.g. the chapter title cards, user report 10-30): the game
// advances the timer once per game frame (count +1 = one unit of +0x38 / +0x40 s, 1/30 s at the Switch's rate), so
// at 60+ fps they played 2-8x too fast. Above 30 fps the count follows real time instead (x the control speed),
// whole units only (the remainder carries over), at most 8 units per call after a hitch.
HLE_FN(mana_timer_advance, "mana_timer_advance") {
    const uint32_t t = c->r[0];
    const uint64_t num = ld64(t + 0x38), den = ld64(t + 0x40);
    if (fps_setting() == 30 || !num || !den) { call_unhooked(c, "mana_timer_advance", {t}); return; }
    if (ld32(t + 0x1c)) return;  // paused
    static std::mutex m;
    static std::unordered_map<uint32_t, std::pair<double, double>> st;  // timer -> (last call s, carried time s)
    std::lock_guard<std::mutex> l(m);
    const double now = ms_now() / 1000.0, unit = (double)num / (double)den;
    auto it = st.find(t);
    if (it == st.end()) {
        it = st.emplace(t, std::make_pair(now, unit)).first;  // the first call steps once, as the game's would
        log("movie: manual timer %08x, unit %.4f s, paced by real time", t, unit);
    }
    auto& [last, carry] = it->second;
    if (now - last > 0.5) carry = unit;  // a new movie on a reused timer (or a long stall): one step, no catch-up
    else carry = std::min(carry + (now - last) * std::max(1, (int)g_speed), 8 * unit);
    last = now;
    uint64_t n = 0;
    for (; carry >= unit; carry -= unit) n++;
    st64(t + 0x48, ld64(t + 0x48) + n);
}
// the field music comes back after a battle (a pushed cue restored): custom music ends
HLE_FN(bgm_restore, "bgm_restore") {
    if (getenv("NX_BGM_LOG")) log("bgm: restore (%u, %u, %u) lr %08x", c->r[0], c->r[1], c->r[2], c->r[14]);
    custom_bgm_stop("field music restored");
    c->r[0] = call_unhooked(c, "bgm_restore", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
HLE_FN(title_scene_update, "title_scene_update") {
    overlay_title_seen();
    c->r[0] = call_unhooked(c, "title_scene_update", {c->r[0]});
}

HLE_FN(btl_proc_advance, "btl_proc_advance") {
    if (fps_setting() != 30 && !tick_due(c->r[0])) return;
    call_unhooked(c, "btl_proc_advance", {c->r[0]});
}

// UI widgets (camp menu, party menu, HUD panels such as the yen counter) count 30 Hz frames: their update (vt+8,
// e.g. 0x24873c -> 0x248508: slide-in counter +0x34 to 8, fade +0x3c to 4) adds 1 per call, and the UI manager task calls
// every widget once per game frame, so uncapped they ran far faster than at 60. Update the manager only when a 60 Hz
// tick has passed (g_tick60: 60 fps = every frame); widgets draw in another pass, so they still show every frame. A
// frame whose pad read has a new press also updates (widgets see a press only in the frame it happens: skipping that
// frame lost menu choices).
// Detailed skill help (setting skill_details, default 1; user request: "not vague"): the loaded Common/SkillInfo.msbt
// (UTF-8 texts, one per skill id) gets power / accuracy / targets from the skill table appended. Found in guest memory
// by Agi's text; new texts go in one guest allocation laid out in message order, and the TXT2 offsets (32-bit, so any
// address wraps into reach) and section size are pointed at it, so offset differences stay the text lengths.
static const uint32_t SKILL_TABLE_PTR = 0x9f3d7c;  // *ptr + 0xc = StSkillTable.dat as loaded (getter 0x27f280)
// Every copy of each pattern in the committed guest space (msbt seen at 7e880f40). ~2 GB to scan: memchr on the first byte, so
// it runs at memory speed; still ~0.3 s, so skill_help_update runs it on a worker (was 1.2 s on the game thread at the title =
// the "1 second freeze after start", user 11-17).
static std::vector<std::vector<uint32_t>> find_bytes(const std::vector<std::string_view>& pats) {
    std::vector<std::vector<uint32_t>> out(pats.size());
    for (uint64_t a = 0x1000; a < 0x100000000ull;) {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(GPTR((uint32_t)a), &mi, sizeof mi)) break;
        uint64_t end = a + ((uint8_t*)mi.BaseAddress + mi.RegionSize - (uint8_t*)GPTR((uint32_t)a));
        if (end > 0x100000000ull) end = 0x100000000ull;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
#else
        const Region rg = mem_query((uint32_t)a);  // the runtime's own region map (every mapped region is readable)
        uint64_t end = std::min<uint64_t>((uint64_t)rg.addr + (rg.size ? rg.size : 0x1000), 0x100000000ull);
        if (end <= a) end = a + 0x1000;
        if (rg.state != MS_FREE) {
#endif
            const uint8_t* p = (const uint8_t*)GPTR((uint32_t)a);
            const size_t len = (size_t)(end - a);
            for (size_t k = 0; k < pats.size(); k++) {
                const std::string_view pat = pats[k];
                for (const uint8_t* q = p; q + pat.size() <= p + len;) {
                    q = (const uint8_t*)memchr(q, (uint8_t)pat[0], (size_t)(p + len - q));
                    if (!q || q + pat.size() > p + len) break;
                    if (!memcmp(q, pat.data(), pat.size())) out[k].push_back((uint32_t)(a + (q - p)));
                    q++;
                }
            }
        }
        a = end;
    }
    return out;
}
// The battle HELP bar is one line: ~122 English characters reach the screen edge (USER 11-20 screenshot cut "[Power 120, hit
// 120%, one foe" off a 94-character text); the game's own longest line is 120. `room` = characters left of a 112 budget; the
// suffix drops the target, then everything but the power, then itself to fit.
static std::string skill_details(uint32_t rec, int room) {
    uint32_t type = ld16(rec + 0x6), formula = ld16(rec + 0x20), power = ld16(rec + 0x22), hit = ld16(rec + 0x14),
             target = ld16(rec + 0x16);
    if (!rec) return "";
    // +0x30 ailment chance %, +0x32 ailment (StSkillTable: Poisma 60 / Poison Skewer 50; ids >= 8 enemy-only, unnamed)
    static const char* const ail_names[] = {"Poison", "Sleep", "Confuse", "Charm", "Seal", "Stun"};  // the game's own words
    const uint32_t ail_ch = ld16(rec + 0x30), ail = ld16(rec + 0x32);
    char ab[24] = "";
    if (ail >= 2 && ail <= 7 && ail_ch >= 1 && ail_ch <= 100) snprintf(ab, sizeof ab, ", %s %u%%", ail_names[ail - 2], ail_ch);
    char b[3][112];
    const char* who = target == 2 ? "all foes" : target == 3 ? "one ally" : target == 4 ? "all allies" : "one foe";
    if (power && formula == 3) {
        snprintf(b[0], sizeof b[0], " [Heal power %u, %s]", power, who);
        snprintf(b[1], sizeof b[1], " [Heal power %u]", power);
        snprintf(b[2], sizeof b[2], " [Heal %u]", power);
    } else if (power && (type == 1 || type == 2)) {
        snprintf(b[0], sizeof b[0], " [Power %u, hit %u%%%s, %s]", power, hit, ab, who);
        snprintf(b[1], sizeof b[1], " [Power %u, hit %u%%%s]", power, hit, ab);
        snprintf(b[2], sizeof b[2], " [Power %u]", power);
    } else if (!power && *ab) {
        snprintf(b[0], sizeof b[0], " [%s, %s]", ab + 2, who);
        snprintf(b[1], sizeof b[1], " [%s]", ab + 2);
        b[2][0] = 0;
    } else return "";
    for (auto& x : b) if ((int)strlen(x) <= room) return x;
    return "";
}
// Characters the help bar shows for a message text: newlines dropped (the bar joins the lines), non-ASCII counted double.
static int help_width(const std::string& t) {
    int w = 0;
    for (unsigned char ch : t) w += ch == '\n' || (ch & 0xc0) == 0x80 ? 0 : ch < 0x80 ? 1 : 2;
    return w;
}
// Attack Session (setting attack_session, default on; request s25): the game gates the Session Lineup on Attack's skill
// record: formula +0x20 == 1 (basic) means no Session. Attack's record gets a skill's Session fields instead (found in
// s50 by comparing with Cleave), and the damage routines (callers 0x2b8bc8..0x2c1000 of skill_copy) get the basic
// formula and power back, so a hit still does normal Attack damage.
static uint16_t g_attack_power = 0;  // Attack's +0x22 before the patch
static bool g_attack_patched = false;
static void attack_session_patch(uint32_t table) {
    static bool done = false;
    if (done || (getenv("NX_ATTACK_SESSION") && !atoi(getenv("NX_ATTACK_SESSION")))) return;
    done = true;
    const uint32_t rec = table + 1 * 0x84;
    g_attack_power = ld16(rec + 0x22), g_attack_patched = true;
    static const uint16_t fields[][2] = {{0x20, 2}, {0x22, 0x19}, {0x54, 1}, {0x56, 2}, {0x58, 9}, {0x5a, 1}, {0xa, 1}, {0x60, 2}};
    for (auto& f : fields) st16(rec + f[0], f[1]);
    log("skills: Attack Session on (Attack power %u)", g_attack_power);
}
HLE_FN(skill_copy, "skill_copy") {
    const uint32_t out = c->r[1], id = c->r[2], lr = c->r[14];
    c->r[0] = call_unhooked(c, "skill_copy", {c->r[0], c->r[1], c->r[2], c->r[3]});
    if (id == 1 && g_attack_patched && lr >= 0x2b8bc8 && lr < 0x2c1000) st16(out + 0x20, 1), st16(out + 0x22, g_attack_power);
}
static void skill_help_update(Cpu* c) {
    static bool done = false;
    static uint64_t next_try = 0;
    if (done || g_frame_no < next_try) return;
    next_try = g_frame_no + 120;
    uint32_t table = ld32(SKILL_TABLE_PTR) + 0xc;
    if (table == 0xc || gstr(table + 0x84 + 0x64, 9) != "CS_SWORD") return;
    attack_session_patch(table);
    if (getenv("NX_SKILL_DETAILS") && !atoi(getenv("NX_SKILL_DETAILS"))) { done = true; return; }
    // debug NX_SKILL_POKE=id:off:val,...: u16 writes into skill records (Attack Session gate tests, s50)
    static bool poked = false;
    if (const char* p = getenv("NX_SKILL_POKE"); p && !poked) {
        poked = true;
        for (unsigned id, off, val, n; sscanf(p, "%x:%x:%x%n", &id, &off, &val, &n) == 3; p += n + (p[n] == ',')) {
            log("skills: poke record %u +%#x: %u -> %u", id, off, ld16(table + id * 0x84 + off), val);
            st16(table + id * 0x84 + off, (uint16_t)val);
        }
    }
    static const std::string_view session_txt{" (Session won't occur.)", 23};
    static const char agi_c[] = "(Mag/Fire) Light attack.";
    static const std::string_view agi{agi_c, sizeof agi_c};  // with its terminator; every loaded copy
    static std::future<std::vector<std::vector<uint32_t>>> scan;
    if (!scan.valid()) { scan = std::async(std::launch::async, [] { return find_bytes({session_txt, agi}); }); return; }
    if (scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { next_try = g_frame_no; return; }
    std::vector<std::vector<uint32_t>> found = scan.get();
    auto still = [](uint32_t a, std::string_view pat) { return !memcmp(GPTR(a), pat.data(), pat.size()); };  // found off-thread: recheck
    if (g_attack_patched)  // the battle help of Attack says it cannot start a Session
        for (uint32_t a : found[0]) if (still(a, session_txt)) memset(GPTR(a), ' ', 23);
    std::vector<uint32_t> hits;
    for (uint32_t a : found[1]) if (still(a, agi)) hits.push_back(a);
    if (hits.empty()) return;  // not loaded yet: a new scan on the next try
    for (uint32_t txt : hits) {
    uint32_t hdr = 0;
    for (uint32_t a = txt & ~3u; a > txt - 0x40000; a -= 4)
        if (!memcmp(GPTR(a), "MsgStdBn", 8)) { hdr = a; break; }
    if (!hdr) continue;
    uint32_t off = hdr + 0x20, sec = 0, size = 0;
    for (uint32_t k = 0, nsec = ld16(hdr + 0xe); k < nsec; k++) {
        uint32_t sz = ld32(off + 4);
        if (!memcmp(GPTR(off), "TXT2", 4)) { sec = off + 0x10; size = sz; break; }
        off = (off + 0x10 + sz + 15) & ~15u;
    }
    uint32_t n = sec ? ld32(sec) : 0;
    if (!n || n > 2000) continue;
    std::vector<std::string> texts(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = sec + ld32(sec + 4 + 4 * i), e = sec + (i + 1 < n ? ld32(sec + 8 + 4 * i) : size);
        std::string t((const char*)GPTR(a), e - a);
        while (!t.empty() && t.back() == 0) t.pop_back();
        if (i < 952 && !(i == 1 && g_attack_patched)) t += skill_details(table + i * 0x84, 112 - help_width(t));  // Attack: weapon power, not the record's
        texts[i] = t + '\0';
    }
    size_t total = 0;
    for (auto& t : texts) total += t.size();
    uint32_t mem = gcall(c, 0x5a8720, {(uint32_t)total});  // the game's operator new
    if (!mem) continue;
    uint32_t p = mem;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(GPTR(p), texts[i].data(), texts[i].size());
        st32(sec + 4 + 4 * i, p - sec);
        p += (uint32_t)texts[i].size();
    }
    st32(sec - 0x10 + 4, p - sec);
    log("skills: %u help texts extended (msbt %08x, text hit %08x, texts %08x, %zu bytes)", n, hdr, txt, mem, total);
    }
    done = true;
}

// UI manager widgets: list *(m) of nodes {+0 prev, +4 next, +8 widget}; widget vt+8 = update (0 = remove), vt+0x20 = kind.
// Widgets made for 30 Hz (USER 11-10: Session "COMBO / GOOD" still too fast, found by NX_UI_STEP_LOG: 60 Hz steps under the 30 Hz
// HUD) step only on 30 Hz real-time ticks: on the other 60 Hz ticks their vtable is swapped for a copy whose update is a guest
// "return 1" (0x201d24, mov r0, #1; bx lr), so they keep running and drawing. NX_UI_W30 = extra vtables (hex, comma separated);
// NX_UI_WLOG=1 logs every new widget vtable (to find one: it appears when the widget opens).
// Default list: the chapter title card ("Intermission": Prologue Reincarnation, Chapter 1 A Star is Born; Lua UI.OpenIntermission
// 0x45d574, widget kind 0xc, update 0x2433f4). It counts frames (+0x3c to 120 = 4 s on the Switch, then closes itself), so on 60 Hz
// UI ticks the card left after 2 s (USER 11-13: title card too fast, fades out fast). Its sprites glide over the 30 Hz steps.
static std::vector<uint32_t> g_w30_vts;
static std::unordered_map<uint32_t, uint32_t> g_w30_fake;  // vtable -> guest copy with update = return 1
static void w30_init() {
    static bool done = false;
    if (done) return;
    done = true;
    g_w30_vts = {0x7d29b8};
    if (const char* e = getenv("NX_UI_W30"))
        for (const char* q = e; *q;) { g_w30_vts.push_back((uint32_t)strtoul(q, (char**)&q, 16)); while (*q == ',' || *q == ' ') q++; }
}
HLE_FN(ui_manager_update, "ui_manager_update") {
    {  // it may call guest code: keep this hook's registers
        uint32_t r[16];
        memcpy(r, c->r, sizeof r);
        skill_help_update(c);
        memcpy(c->r, r, sizeof r);
    }
    const uint32_t m = c->r[0];
    if (fps_setting() != 30 && !ui_tick_due(m) && !pad_pressed_now()) { c->r[0] = 1; return; }
    w30_init();
    static const bool wlog = getenv("NX_UI_WLOG") != nullptr;
    std::vector<std::pair<uint32_t, uint32_t>> swapped;  // widget, its vtable
    if (fps_setting() != 30 && (wlog || !g_w30_vts.empty())) {
        static std::unordered_set<uint32_t> seen;
        static std::unordered_map<uint32_t, uint32_t> w30_tick;
        const bool tick30 = tick_on(w30_tick, g_tick_ui30, m);
        int guard = 0;
        for (uint32_t n = ld32(m); n && guard < 512; n = ld32(n + 4), guard++) {
            const uint32_t w = ld32(n + 8), vt = w ? ld32(w) : 0;
            if (!vt) continue;
            if (wlog && seen.insert(vt).second) log("ui widget: new vtable %08x (widget %08x)", vt, w);
            if (std::find(g_w30_vts.begin(), g_w30_vts.end(), vt) == g_w30_vts.end()) continue;
            static std::unordered_map<uint32_t, int64_t> w30_seen;  // widget -> last ms seen (log once per showing)
            if (int64_t& seen = w30_seen[w]; ms_now() - seen > 2000) log("ui: 30 Hz widget %08x (vtable %08x) shown", w, vt), seen = ms_now();
            else seen = ms_now();
            g_ui30_ms = ms_now();  // glide these over the 30 Hz step
            if (tick30 || pad_pressed_now()) continue;
            uint32_t& fake = g_w30_fake[vt];
            if (!fake) {
                const Cpu keep = *c;
                fake = gcall(c, 0x5a8720, {0x100});
                *c = keep;
                if (!fake) continue;
                memcpy(GPTR(fake), GPTR(vt), 0x100);
                st32(fake + 8, 0x201d24);
            }
            st32(w, fake), swapped.push_back({w, vt});
        }
    }
    {
        UiDt dt_scope(m);
        c->r[0] = call_unhooked(c, "ui_manager_update", {m});
    }
    for (auto& [w, vt] : swapped) if (ld32(w) == g_w30_fake[vt]) st32(w, vt);
}

// The camp menu (Y in the field: top menu, Skills, Items, Artists...) is not a UI-manager widget: its manager runs once
// per frame from its own task and the screens count 30 Hz frames (cursor circle, rotating ring, NEW blink, "SELECT (A)"
// bubble). Same gate as the UI manager.
// control "uigate N" (debug): 0 = these gates off, 1 = 60 Hz ticks + frames with a new press, 2 = ticks only
std::atomic<int> rt::g_ui_gate{1};
static bool ui_skip(uint32_t obj) {
    if (fps_setting() == 30 || g_ui_gate == 0) return false;
    return !ui_tick_due(obj) && !(g_ui_gate == 1 && pad_pressed_now());
}
HLE_FN(camp_update, "camp_update") {
    // it runs every field frame; its state (short *0x9efb40, read by 0x313b04) is 0 while the menu is closed (6 = open)
    if (ld16(ld32(0x9efb40))) overlay_camp_seen();
    if (ui_skip(c->r[0])) return;
    UiDt dt_scope(c->r[0]);
    call_unhooked(c, "camp_update", {c->r[0]});
}
// Return to Title (camp menu (-) dialog, after the overlay closed the menu): leave the field from its free-roam state
// (field manager state 1, vtable 0x7e654c) the way the game does after an event calls Lua ChangeTitle (event-end state
// 0x47203c): next scene 4 (title) via 0x46c1a0, then next state 8 (0x46ed90 sets the state object's +4).
// Camp menu open / close wipe (white circle, then the spinning ring until the menu is ready): a task of its own,
// counters +0xe (wipe steps, states 2-4) and +0x10 (ring angle) +1 per call. Same 60 Hz gate; it always returns 1.
HLE_FN(camp_wipe_update, "camp_wipe_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 1; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "camp_wipe_update", {c->r[0]});
}
// Facility screens with their own tasks (not UI-manager widgets) count frames the same way. USER 11-13 items 7 + 8 at
// 240 fps: the RECORDS screen bubble rings and the Carnage Unity background rings ran 4x (measured with "fixdt 60" vs
// "fixdt 240" shot runs, agent_scripts/ringrate.sh). Same 60 Hz UI gate; both always return 1.
HLE_FN(records_task_update, "records_task_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 1; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "records_task_update", {c->r[0]});
}
HLE_FN(unity_task_update, "unity_task_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 1; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "unity_task_update", {c->r[0]});
}
// The other Bloom Palace facilities (types 2-5, later in the story) run their screens from this task; same widgets and
// background as Carnage Unity, so the same gate. Not measured: no save reaches them yet (s73).
HLE_FN(unity2_task_update, "unity2_task_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 1; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "unity2_task_update", {c->r[0]});
}
// L1 shop fast travel (USER 11-14): straight into a shop, the way its street door does it: Lua RequestChangeMap(FIELD 0,
// area 3 (f003 interiors), sub n, entry 1000) = 0x46c1b4 stores the request, and the field loop (0x46b7f0) turns it
// into a map change when the current state ends (next state != 4). Leaving the shop puts you on the street at its
// door, as after walking in. Doors: Shibuya f001_0x layout records type 0x16 / 0xc ints (3, 0, 3, n, 1000), Harajuku
// f007_01 (Anzu). Offered only in town free roam while Shibuya is not corrupted / half dead, and each shop only after
// its first-visit tutorial flag (the game sets it inside the shop). Seiren has no such flag (its door has no flag gate
// either): it is offered once another shop was visited or Shibuya is open on the Tokyo Map (MTLMAP_OPEN_AREA_SHIBUYA_A,
// still off in chapter 1 free roam, u13b save); while quest 07 waits for its cafe line it is not offered
// (the door script shows that line). Not copied: the cafe door's money check (under 400 yen it refuses entry).
static int flag_get(uint32_t id);
static uint32_t flag_byte(uint32_t id);
struct TravelShop { const wchar_t* name; const wchar_t* help; int sub; uint32_t visited_flag; };
static const TravelShop k_shops[] = {
    {L"Café Seiren", L"Shibuya. Food and drinks.", 6, 0},
    {L"Hee Ho Mart", L"Shibuya. Items.", 1, 0x65},        // FLG_SYS_FACILITY_CVS1_TTL_DONE
    {L"Jewelry Carabia", L"Shibuya. Accessories.", 4, 0x67},  // FLG_SYS_FACILITY_JWL_TTL_DONE
    {L"Anzu", L"Harajuku. Armor.", 8, 0x6f},              // FLG_SYS_FACILITY_GDS_TTL_DONE (Facility/field/GoodsShop)
};
static std::atomic<uint32_t> g_travel_mask{0};
int rt::travel_shop_count() { return (int)std::size(k_shops); }
const wchar_t* rt::travel_shop_name(int i) { return k_shops[i].name; }
const wchar_t* rt::travel_shop_help(int i) { return k_shops[i].help; }
uint32_t rt::travel_shops_offered() { return g_travel_mask; }
static uint32_t travel_mask(int kind, int area, int sub) {
    if (kind != 0 || (area != 1 && area != 3 && area != 7)) return 0;  // Shibuya, interiors, Harajuku
    if (flag_get(0x54) != 0 || flag_get(0x8805) != 0) return 0;  // SHIBUYA_CORRUPT, isShibuyaHalfDead (also -1: flags not loaded)
    bool shibuya = flag_get(0x1b5) == 1;  // MTLMAP_OPEN_AREA_SHIBUYA_A
    for (const TravelShop& t : k_shops) shibuya |= t.visited_flag && flag_get(t.visited_flag) == 1;
    uint32_t m = 0;
    for (size_t i = 0; i < std::size(k_shops); i++) {
        const TravelShop& t = k_shops[i];
        if (area == 3 && sub == t.sub) continue;  // already there
        if (t.visited_flag ? flag_get(t.visited_flag) != 1 : !shibuya) continue;
        if (t.sub == 6 && flag_get(0x8e61) == 1 && flag_get(0x8e6f) != 1 && flag_get(0x3e07) != 1) continue;  // quest 07 (EnterCafe_A)
        m |= 1u << i;
    }
    return m;
}
// Reveal Idolasphere maps (setting map_reveal, default 0; user 11-23): the whole floor shows on the minimap and the
// map screen as soon as it is entered. The game's own painter does it, the one Lua ampSetFootAt (0x422284 -> 0x3bbdcc)
// uses for the explored area: automap manager *0x9f44fc; +0x3c map data: +0x28 layer table (0x48 each: +8 sections,
// +0x10 count), +0x30 layer count, +0x40 > 1: sections tagged with an id (+4) that must equal manager +0x48 (the drawn
// part). Section (0x40): +0 id, +8 nonzero = skip, +0x34 / +0x38 offset. Its picture 0x23e174(0x23c4f8(id + 25000, 0)):
// +8 / +0xc origin, +0x10 / +0x14 size (px). 0x3bba18(mgr, section, &centre, s0 = half width, s1 = half depth, 1) sets
// every 12-px cell of the box in the explored bitmap (and the fog textures) plus the rooms (sub pictures) it overlaps;
// the loops are bounded by the picture, so a box twice its size costs only empty iterations. The explored bits are
// the game's own (saved like walked ground), so turning the setting off later keeps what was revealed.
static uint32_t g_reveal_buf;  // guest float[2] for the centre
static int reveal_map(Cpu* c) {
    const uint32_t mgr = ld32(0x9f44fc), map = mgr ? ld32(mgr + 0x3c) : 0;
    if (!map) return -1;
    if (!g_reveal_buf) g_reveal_buf = gcall(c, 0x5a8720, {16});  // operator new
    const uint32_t layers = ld32(map + 0x30), tab = ld32(map + 0x28), cur = ld32(mgr + 0x48);
    const bool tagged = (int32_t)ld32(map + 0x40) > 1;
    if (getenv("NX_REVEAL_DEBUG")) {
        static uint32_t last = 0;
        if (last != map) last = map, log("map reveal debug: mgr %08x map %08x layers %u tab %08x tag mode %d now %u", mgr, map, layers, tab, (int)ld32(map + 0x40), cur);
    }
    int n = 0;
    for (uint32_t l = 0; l < layers && l < 64; l++) {
        const uint32_t e = tab + l * 0x48, count = ld32(e + 0x10), secs = ld32(e + 8);
        for (uint32_t k = 0; k < count && k < 64; k++) {
            const uint32_t sec = secs + k * 0x40;
            static const bool dbg = getenv("NX_REVEAL_DEBUG") != nullptr;
            if (dbg) log("map reveal debug: layer %u/%u section %u/%u id %u tag %u (now %u, tagged %d) skip %u", l, layers, k, count,
                         ld32(sec), ld32(sec + 4), cur, tagged, ld32(sec + 8));
            if ((tagged && ld32(sec + 4) != cur) || ld32(sec + 8)) continue;
            const uint32_t img = gcall(c, 0x23e174, {gcall(c, 0x23c4f8, {ld32(sec) + 25000, 0})});
            if (!img) continue;
            const int ox = (int32_t)ld32(img + 8), oz = (int32_t)ld32(img + 0xc), w = (int32_t)ld32(img + 0x10), h = (int32_t)ld32(img + 0x14);
            if (w <= 0 || h <= 0 || w > 16384 || h > 16384) continue;
            float* ctr = (float*)GPTR(g_reveal_buf);
            float dx, dz;
            memcpy(&dx, GPTR(sec + 0x34), 4), memcpy(&dz, GPTR(sec + 0x38), 4);
            ctr[0] = ox + dx + w * 0.5f, ctr[1] = oz + dz + h * 0.5f;
            const int half = std::max(w, h) + 24;
            // explored bitmap (as the painter finds it): cells of 12 px, row-major, one bit each
            const uint32_t bits = gcall(c, 0x3c0c84, {(uint32_t)((int32_t)ld32(img) % 1000)});
            const int cells = ((w + 11) / 12) * ((h + 11) / 12);
            auto count = [&] {
                int k = 0;
                for (int b = 0; bits && b < cells; b++) k += ld8(bits + b / 8) >> (b & 7) & 1;
                return k;
            };
            const int before = count();
            c->vr.f[0] = (float)half, c->vr.f[1] = (float)half;
            gcall(c, 0x3bba18, {mgr, sec, g_reveal_buf, 1});
            log("map reveal: section %u (%dx%d px): %d -> %d of %d cells", ld32(sec), w, h, before, count(), cells);
            n++;
        }
    }
    return n;
}

HLE_FN(field_roam_update, "field_roam_update") {
    if (overlay_take_title_request()) {
        const uint32_t obj = c->r[0];
        const Cpu keep = *c;
        gcall(c, 0x46c1a0, {4});
        gcall(c, 0x46ed90, {obj, 8});
        *c = keep;
        log("field: return to title");
        c->r[0] = 0;
        return;
    }
    // Open Tokyo Map (overlay.cpp): set the large-map manager's request flag (*0x9f5290 +4) the way Lua ChangeLargeMap
    // does at a town exit; this free-roam state itself then switches to state 6 (large map, 0x47146c) only when nothing
    // else (event trigger, camp menu, ...) claims the frame. Not taken: the flag is cleared so it cannot fire later.
    // Current map (0x9f52a8): kind +0x74, area +0x76, sub +0x78.
    const uint32_t obj = c->r[0], lm = ld32(0x9f5290);
    const int kind = (int16_t)ld16(0x9f52a8 + 0x74), area = (int16_t)ld16(0x9f52a8 + 0x76), sub = (int16_t)ld16(0x9f52a8 + 0x78);
    static int last_map = -1;
    if (const int m = kind << 16 | (area & 0xff) << 8 | (sub & 0xff); m != last_map)
        last_map = m, log("field: free roam in map kind %d area %d sub %d (X map gate %d)", kind, area, sub, ld8(0x9f52a8 + 6)),
        auto30_new_area();
    // Idolaspheres (kind 1): reveal the floor once per map / drawn part while the setting is on (live from the F1 menu)
    if (kind == 1 && getenv("NX_MAP_REVEAL") && atoi(getenv("NX_MAP_REVEAL"))) {
        // once per (map, map data, drawn part): a floor change reloads the map data, so a stale first frame is redone
        static uint32_t done[3] = {~0u, ~0u, ~0u};
        const uint32_t mgr = ld32(0x9f44fc);
        const uint32_t key[3] = {(uint32_t)last_map, mgr ? ld32(mgr + 0x3c) : 0, mgr ? ld32(mgr + 0x48) : 0};
        if (mgr && key[1] && memcmp(key, done, sizeof key)) {
            memcpy(done, key, sizeof key);
            const Cpu keep = *c;
            const int n = reveal_map(c);
            *c = keep;
            // none yet (first frames of a fresh load: sections not filled in): try again next frame, quietly
            if (n <= 0) done[0] = ~0u;
            else log("map reveal: %d section(s) of map kind %d area %d sub %d", n, kind, area, sub);
        }
    }
    // towns only: kind 0 = Shibuya, Harajuku, interiors (f0xx); kind 1 = Idolaspheres (d0xx), never left this way
    // Bloom Palace (f003_03) is not on the Tokyo Map: opening it there broke the map's Lua ("attempt to index field '?'
    // (a nil value)") and the map took no input (user 11-24 nx.log); no fast travel or map from it (vanilla: its door)
    const bool town = kind == 0 && lm && !(area == 3 && sub == 3);
    g_travel_mask = town ? travel_mask(kind, area, sub) : 0;
    overlay_roam_seen(town);
    if (const int i = overlay_take_travel_request(); i >= 0) {
        if (i >= 100 && i < 164 && town) {  // debug (NX_CONTROL "travel 1NN" only): warp to f003_NN, no checks
            const Cpu keep = *c;
            gcall(c, 0x46c1b4, {0, 3, (uint32_t)(i - 100), 1000});
            gcall(c, 0x46ed90, {obj, 8});
            *c = keep;
            log("field: debug warp to f003_%02d", i - 100);
            c->r[0] = 0;
            return;
        }
        if (i < (int)std::size(k_shops) && (g_travel_mask >> i & 1)) {
            const Cpu keep = *c;
            gcall(c, 0x46c1b4, {0, 3, (uint32_t)k_shops[i].sub, 1000});  // RequestChangeMap(FIELD, 3, n, 1000)
            if (k_shops[i].sub == 6)  // EnterCafe_A: FlagOff FLG_MAP_CafeEnteredFromBack (the front door)
                if (uint32_t a = flag_byte(0x8807)) *(uint8_t*)GPTR(a) &= ~(1u << (0x8807 & 7));
            gcall(c, 0x46ed90, {obj, 8});
            *c = keep;
            log("field: fast travel to f003_%02d (%ls)", k_shops[i].sub, k_shops[i].name);
            c->r[0] = 0;
            return;
        }
        log("field: fast travel request %d not offered here (mask %#x)", i, g_travel_mask.load());
    }
    const bool map_req = overlay_take_map_request() && town && ld32(lm + 4) == 0;
    if (map_req) st32(lm + 4, 1);
    c->r[0] = call_unhooked(c, "field_roam_update", {obj});
    if (map_req) {
        const uint32_t next = ld32(obj + 4);
        if (next == 6) log("field: Tokyo Map opened");
        else st32(lm + 4, 0), log("field: Tokyo Map request not taken (next state %d)", (int)next);
    }
}
// The save screen (camp Save; save object task 0x26900c) and the load screen (title LOAD state 0x4a07e8, camp Load)
// count frames the same way (slot list, SELECT bubble pulse +8, star field). Both return 1 when the screen is done, so a
// skipped frame returns 0. Their drawing is separate (title: 0x263768 from the LOAD state's draw 0x4a0b18).
HLE_FN(save_screen_update, "save_screen_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 0; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "save_screen_update", {c->r[0]});
}
HLE_FN(load_screen_update, "load_screen_update") {
    if (ui_skip(c->r[0])) { c->r[0] = 0; return; }
    UiDt dt_scope(c->r[0]);
    c->r[0] = call_unhooked(c, "load_screen_update", {c->r[0]});
}
// Battle HUD (command wheel, ROUND / Action timeline, COMMAND marker, target circles) and the party portraits HUD:
// tasks of their own (return 1 = keep running), same 30 Hz frame counting.
// debug control "gate30 <mask>": 1 = btl_ui_update, 2 = party_hud_update step on 30 Hz real-time ticks (find 60 Hz widgets)
std::atomic<uint32_t> rt::g_gate30{0};
static bool gate30_skip(uint32_t bit, uint32_t obj) {
    if (!(g_gate30 & bit) || fps_setting() == 30) return false;
    static std::unordered_map<uint32_t, uint32_t> t;
    g_ui30_ms = ms_now();
    return !tick_on(t, g_tick_ui30, obj + bit);
}
HLE_FN(btl_ui_update, "btl_ui_update") {
    battle_ui_seen();
    if (ui_skip(c->r[0]) || gate30_skip(1, c->r[0])) { c->r[0] = 1; return; }
    c->r[0] = call_unhooked(c, "btl_ui_update", {c->r[0]});
}
// Battle HUD parts (called by btl_ui_update, whose gate gives them 60 Hz ticks). A set bit of g_ui_part30 steps that part
// on 30 Hz ticks of real time. Debug: control "uipart <hex mask>", "uifreeze <part + 1>" (0 = off) stops one part.
// USER 11-06 (replay of the 60 Hz build): target "?" panel, turn-order markers and the SESSION / COMBO text run too fast. They count 30 Hz
// frames, so 60 Hz ticks = 2x the Switch: those parts step at 30 Hz (glide above 30 fps, ui_clock). On the UI clock (60 Hz) stay: the
// skill / item list (bit 2, btlui_p1c; USER 11-08: the skill list felt 30 Hz), the COMBO number (bit 9, p38) and the GOOD pop (bit 10,
// p3c): USER 11-10 "GOOD and CALL text: 60 Hz, not 30" (s67-s69 had moved them to 30 Hz; the report before was per-frame speed at 240).
std::atomic<uint32_t> rt::g_ui_part30{0x1ffffu & ~(1u << 2 | 1u << 9 | 1u << 10)}, rt::g_ui_freeze{0};
static std::unordered_map<uint32_t, uint32_t> g_part_tick;
static bool part_skip(int i, uint32_t obj) {
    if (g_ui_freeze == (uint32_t)i + 1) return true;
    if (fps_setting() == 30 || !(g_ui_part30 >> i & 1)) return false;
    g_ui30_ms = ms_now();
    return !tick_on(g_part_tick, g_tick_ui30, obj) && !pad_pressed_now();
}
static void part_log(int i, uint32_t obj) {  // NX_DEBUG_BATTLE: Session GOOD pop counter (p3c +8) steps with time
    static const bool on = getenv("NX_DEBUG_BATTLE") != nullptr;
    static uint32_t last = ~0u;
    if (!on || i != 10 || ld32(obj + 8) == last) return;
    last = ld32(obj + 8);
    log("battle: GOOD pop p3c state %u step %u", ld32(obj + 4), last);
}
#define BTL_UI_PART(fn, i) HLE_FN(fn, #fn) { const uint32_t o = c->r[0]; if (!part_skip(i, o)) c->r[0] = call_unhooked(c, #fn, {o}); part_log(i, o); }
BTL_UI_PART(btlui_p24, 0) BTL_UI_PART(btlui_p30, 1) BTL_UI_PART(btlui_p1c, 2) BTL_UI_PART(btlui_p10, 3) BTL_UI_PART(btlui_p14, 4)
BTL_UI_PART(btlui_p18, 5) BTL_UI_PART(btlui_p28, 6) BTL_UI_PART(btlui_p2c, 7) BTL_UI_PART(btlui_p34, 8) BTL_UI_PART(btlui_p38, 9)
BTL_UI_PART(btlui_p40, 11) BTL_UI_PART(btlui_p44, 12) BTL_UI_PART(btlui_p48, 13) BTL_UI_PART(btlui_p4c, 14)
BTL_UI_PART(btlui_p50, 15) BTL_UI_PART(btlui_p54, 16)
// Battle HUD draw (0x2e9664, every frame): the 19 part draws in the game's order. Debug "uidraw30 <mask>": part draws whose bit
// is set run only on 30 Hz ticks (they flicker: finds which part draws something).
static const struct { uint8_t off; uint32_t fn; } k_btl_draws[] = {
    {0x30, 0x2efbf0}, {0x1c, 0x2f4848}, {0x10, 0x2ee160}, {0x14, 0x2f7df4}, {0x20, 0x2f157c}, {0x28, 0x2f0cb0}, {0x1c, 0x2f4900},
    {0x18, 0x2f9a4c}, {0x34, 0x2f9f78}, {0x38, 0x2ea068}, {0x3c, 0x2ea218}, {0x40, 0x2fab84}, {0x44, 0x2e9d88}, {0x48, 0x2ff7f4},
    {0x50, 0x2fb694}, {0x4c, 0x2fdc80}, {0x2c, 0x2f08c4}, {0x24, 0x301d40}, {0x54, 0x2fc994}};
std::atomic<uint32_t> rt::g_uidraw30{0};
static uint32_t wheel_ids(uint32_t list);  // the command wheel's list (btl_wheel_input below)
static std::atomic<bool> g_wheel_done{false};  // a command was confirmed, the wheel is closing
HLE_FN(btl_ui_draw, "btl_ui_draw") {
    const uint32_t ui = c->r[0];
    if (!ld32(ui + 4) || !ld32(ui)) { c->r[0] = 1; return; }
    static std::unordered_map<uint32_t, uint32_t> t;
    const bool tick = tick_on(t, g_tick_ui30, ui);
    const char* sk = getenv("NX_UIDRAW_SKIP");  // debug: parts not drawn (hex mask, live through control "env")
    const uint32_t skip = sk ? (uint32_t)strtoul(sk, nullptr, 16) : 0;
    // P5 panel mode: the game's command wheel (part 2, draw 0x2ee160) is never drawn (s113; s114: not only while the panel is
    // up, its fade out after a command and its fade in after a cancel showed). Part 2 is the wheel alone: skill list,
    // target pick and the HELP bar draw the same without it (NX_UIDRAW_SKIP=4 run, s114)
#ifdef _WIN32
    const bool p5 = rt::battle_buttons_mode() == 2;
#else
    const bool p5 = false;  // no panel on Linux yet: the game's wheel stays
#endif
    // the wheel object (part 2): +0 = 1 while the wheel is open (from ~80 ms before its first input call, also while the
    // member's voice line or a tutorial box holds the input back: user video 11-25), its list at +0xc (as r2 of
    // btl_wheel_input). The panel shows from it (s114, NX_P5_DUMP run); the buttons are taken only while the wheel takes
    // input (g_wheel_ms): a tutorial box over an open wheel needs A / B (user 11-26: soft lock). It stays 1 for ~550 ms
    // after a command is confirmed (the wheel's fade out, the list / target pick already taking input): g_wheel_done
    // (set at the confirm) ignores it until it drops to 0.
    const uint32_t wo = ld32(ui + 0x10);
    if (wo && ld32(wo) != 1) g_wheel_done = false;
    if (const uint32_t w = wo; w && ld32(w) == 1 && !g_wheel_done && rt::battle_buttons_mode()) {
        rt::g_wheel_open_ms = ms_now();
        if (const uint32_t ids = wheel_ids(w + 0xc)) rt::g_wheel_ids = ids;
    }
    static const bool dump = getenv("NX_P5_DUMP") != nullptr;  // debug: changes in the wheel object (part 2), rare words only
    if (dump) {
        static uint32_t prev[512], n[512];
        static uint32_t was = 0;
        const uint32_t w = ld32(ui + 0x10);
        for (int i = 0; w && i < 512; i++) {
            const uint32_t v = ld32(w + 4 * i);
            if (w == was && v != prev[i] && ++n[i] < 12) log("p5dump: +%03x %08x -> %08x (input %lld ms ago)", 4 * i, prev[i], v, (long long)(ms_now() - rt::g_wheel_ms));
            prev[i] = v;
        }
        was = w;
    }
    for (uint32_t i = 0; i < 19; i++) {
        if (skip >> i & 1 || (p5 && i == 2) || (i == 5 && rt::p5_panel_active())) continue;  // 5: Stats / Auto-Battle pills
        if ((g_uidraw30 >> i & 1) && !tick) continue;
        gcall(c, k_btl_draws[i].fn, {ld32(ui + k_btl_draws[i].off)});
    }
    c->r[0] = 1;
}
// Session COMBO / GOOD pop (p3c: state +4 = 1 while it runs, step +8 0..15, scale 1.5 -> 1.0 over steps 0..3) and the COMBO number
// (p38: 0x2ea0ac adds a combo, 0x2eb7a0 writes the digits; NOT the command wheel as names.txt once said). USER 11-10 (after s67-s69
// put them on 30 Hz): "make it 60 Hz, not 30" = the UI rule: they step on the UI clock (60 Hz ticks; fps 30 / ui_speed 30: 30 Hz),
// never once per frame (that was the "too fast" at 240). Mask bits 2, 9, 10 off = btl_ui_update's own UI-clock gate only.
BTL_UI_PART(btlui_p3c, 10)
// Topic menu + minimap (map manager 0x4a338c, a task nothing gated: 5x at 150 fps). Its counters count Switch frames: Topic open / close
// +0x1b4 (0..4), the diamond strip +0x8d4 / +0x8d8 (Topic tabs, field "CALL" ring) and 12 icon flipbook timers +0x8dc + 8 i.
// USER 11-06: Topic open + green diamonds too fast; USER 11-08: the chat inside was then 30 Hz (s59 stepped the whole open Topic at 30 Hz)
// and the field "CALL" ring text far too fast (counters were on 60 Hz ticks while closed). Now: open, the update runs on UI ticks (60 Hz,
// chat scroll + cursor); closed, every frame (minimap follows the player); in both, those counters only advance on 30 Hz ticks.
HLE_FN(map_mgr_update, "map_mgr_update") {
    const uint32_t m = c->r[0];
    if (fps_setting() == 30) { c->r[0] = call_unhooked(c, "map_mgr_update", {m}); return; }
    const bool open = ld32(m + 0x1b0) != 0;
    if (open && ui_skip(m)) { c->r[0] = 1; return; }
    const bool tick30 = tick_on(g_part_tick, g_tick_ui30, m);
    static const uint32_t OFFS[] = {0x1b4, 0x8d4, 0x8d8, 0x8dc, 0x8e4, 0x8ec, 0x8f4, 0x8fc, 0x904, 0x90c, 0x914, 0x91c, 0x924, 0x92c, 0x934};
    uint32_t before[15];
    for (int i = 0; i < 15; i++) before[i] = ld32(m + OFFS[i]);
    // The flipbook timers (i >= 3) call 0x4a2270 (next cell, timer reset) when +1 reaches their limit: putting them back after the
    // update let that reset through on frames without a tick (a cell stepped one tick early, 2x for 2-tick cells). Pre-decrement
    // instead, so the game's +1 lands on the old value and never reaches the limit between ticks.
    if (!tick30) for (int i = 3; i < 15; i++) st32(m + OFFS[i], before[i] - 1);
    // Urgent Topic call ringtone (USER 11-13: the phone ring in the prologue far too fast): +0x8b8 counts updates to 60, then the
    // ring cue (sound 0x11) restarts and +0x8c8 = 30 stops it 30 updates later. Closed, this update runs every frame: at 240 fps
    // the ring restarted every 0.25 s (log: tp_emergencycall 250 ms apart) instead of every 2 s. Both step on 30 Hz ticks, the same
    // pre-step way (+0x8b8 -1 so the game's +1 never reaches the restart; +0x8c8 +1 so its -1 never reaches the stop).
    static const bool ring_test = getenv("NX_RING_TEST") != nullptr;  // debug: urgent call state on (flags +500 bit 1, +0x8b4)
    if (ring_test && !ld32(m + 0x8b4)) st32(m + 500, ld32(m + 500) | 2), st32(m + 0x8b4, 1), st32(m + 0x8cc, ~0u);
    const int32_t ring_up = (int32_t)ld32(m + 0x8b8), ring_left = (int32_t)ld32(m + 0x8c8);
    if (!tick30) {
        st32(m + 0x8b8, (uint32_t)(ring_up - 1));
        if (ring_left > 0) st32(m + 0x8c8, (uint32_t)(ring_left + 1));
    }
    {
        UiDt dt_scope(m);
        c->r[0] = call_unhooked(c, "map_mgr_update", {m});
    }
    if (!tick30) {
        for (int i = 0; i < 15; i++) {
            const uint32_t v = ld32(m + OFFS[i]);
            if (i >= 3) { if (v == before[i] - 1) st32(m + OFFS[i], before[i]); }  // not stepped this frame: undo our -1
            else if (v == before[i] + 1) st32(m + OFFS[i], before[i]);
        }
        if ((int32_t)ld32(m + 0x8b8) == ring_up - 1) st32(m + 0x8b8, (uint32_t)ring_up);  // not counted this frame
        if (ring_left > 0 && (int32_t)ld32(m + 0x8c8) == ring_left + 1) st32(m + 0x8c8, (uint32_t)ring_left);
    }
}
// The field "CALL" ring (Topic notification, top-left) animates from counters 0x4accf0 steps by 1 per call, once per frame
// (USER 11-08 / 11-10: CALL text far too fast; s60 / s67 gated map_mgr_update's own counters, not these). Measured s68 with fixdt:
// at 240 the text pass took the same number of frames as at 30 (30 frames), i.e. 8x the Switch. Steps on the UI clock (USER 11-10:
// 60 Hz, not 30; s68 had put it on 30 Hz ticks).
HLE_FN(notify_anim_tick, "notify_anim_tick") {
    static std::unordered_map<uint32_t, uint32_t> t;
    if (fps_setting() != 30 && !tick_on(t, rt::ui_speed() == 60 ? g_tick60 : g_tick_ui30, c->r[0])) return;
    call_unhooked(c, "notify_anim_tick", {c->r[0]});
}
// The command wheel's hold-to-scroll counts calls (one per frame): at 240 fps one short press scrolled 3 commands.
// P5-style battle buttons (Discord 11-25, setting battle_buttons): a direct button picks a command of the wheel (called
// from 0x2a8530; the skill list and target pick use this function from elsewhere). poll_input takes the button away
// from the game and turns the wheel with left / right taps until the selection here is the command, then taps A until
// the wheel confirms (returns 1) - the game's own input path, so its own checks run (a command that cannot be used
// now stays refused). Writing the selection word directly is NOT enough: the wheel confirmed the command it had turned
// to (s112). Indexes (s112, wheelmap.sh + NX_WHEEL_DIFF; right = index + 1, wraps): 0 Attack, 1 Skills, 2 Items,
// 3 Guard, 4 Change, 5 Tactics, 6 Escape.
std::atomic<int64_t> rt::g_wheel_ms{0}, rt::g_wheel_open_ms{0}, rt::g_wheel_blocked_ms{0};
// Screens over the open wheel (user 11-26): the tutorial info window (info_window_update runs each frame it is up, state
// +8 < 4) and Stats (+ : the wheel input object +0x5c8 = 1, the wheel input is still called). While one is up the panel
// hides and the buttons are the game's.
// Rare boot abort (NOTES "Boot abort", Discord 11-26 i5-8500): the SDK's WaitSystemEvent aborts when the event's state
// (+0x1c) is not 1 / 2 (= not initialised yet). Seen in the game's vsync thread (0x55d250) right as the main thread
// sets the event up (GetDisplayVsyncEvent): a start-up race. Wait up to 2 s for the event to be set up; logged.
// s115, Augs' crash log (nx_20261125_160319, this guard in the build, nothing logged): the real cause. The main thread
// (0x55c594..0x55c630) sets the event up and waits on it itself, then passes the vsync thread an argument block on its
// OWN STACK (sp+80: +4 = the event) and returns without waiting for the thread to start. The thread reads +4 once
// (0x55d260) before its loop; started late (a busy 6-core CPU), it reads a reused stack slot: 0 or garbage, every
// wait aborts. The Switch starts it at once. Here: a wait from the vsync thread (lr 0x55d284) on anything but the event
// GetDisplayVsyncEvent filled takes that event, in r0 and in r4 (the thread's copy for its later waits).
// s115: this was a PROBE_FN in s114: probes run only where the recompiler emitted PROBE (names.txt functions), and an
// SDK export has only its HOOK slot, so it never ran (Augs' crash log: nothing logged). Now the hook, which then runs
// the SDK's own WaitSystemEvent (call_unhooked).
HLE_FN(wait_sysevent_hook, "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE") {
    static const bool test = getenv("NX_TEST_BOOT_RACE") != nullptr;  // debug: the overwritten argument, made to happen
    if (test && c->r[14] == 0x0055d284u && rt::g_vsync_guest_event) c->r[4] = 0, c->r[0] = 0;
    uint32_t ev = c->r[0], r4 = c->r[4];
    if (const uint32_t vev = rt::g_vsync_guest_event; c->r[14] == 0x0055d284u && vev && ev != vev) {
        static int told = 0;
        if (told++ < 3) log("kernel: vsync thread waits on %08x, not the vsync event %08x (its start-up argument was overwritten): using %08x", ev, vev, vev);
        ev = r4 = vev;
    }
    if (ev && ld8(ev + 0x1c) != 1 && ld8(ev + 0x1c) != 2) {
        int ms = 0;
        for (; ms < 2000 && ld8(ev + 0x1c) != 1 && ld8(ev + 0x1c) != 2; ms++) Sleep(1);
        log("kernel: WaitSystemEvent on event %08x before it was set up (lr %08x): waited %d ms, state %u", ev, c->r[14], ms, ld8(ev + 0x1c));
    }
    const uint32_t lr = c->r[14];
    rt::call_unhooked(c, "_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE", {ev});
    c->r[4] = r4, c->r[14] = lr;
}
PROBE_FN(info_window_probe, "info_window_update") {
    if (ld32(c->r[0] + 8) < 4) rt::g_wheel_blocked_ms = ms_now();
}
std::atomic<int> rt::g_wheel_sel{-1}, rt::g_p5_target{-1};
std::atomic<uint32_t> rt::g_wheel_ids{0x7654321};
static uint32_t wheel_ids(uint32_t list) {
    // 8-byte entries {u16 index, u16 command id, u32 1 = greyed out}; the list ends where the index stops counting up
    // (0xffffffff after 6 entries, but Itsuki's 7 are followed by {0, 0}). Nibble i = command id at index i + 1.
    uint32_t ids = 0;
    for (int i = 0; list && i < 8; i++) {
        const uint32_t w = ld32(list + 8 * i);
        if ((w & 0xffff) != (uint32_t)i || (w >> 16) > 14) break;
        ids |= ((w >> 16) + 1) << 4 * i;
    }
    return ids;
}
HLE_FN(btl_wheel_input, "btl_wheel_input") {
    g_btl_input_ms = ms_now();  // battle speed: the player is choosing
    if ((c->r[14] & ~1u) == 0x2a8530 && c->r[0] && ld32(c->r[0] + 0x5c8) == 1) rt::g_wheel_blocked_ms = ms_now();  // Stats open
    if ((c->r[14] & ~1u) == 0x2a8530 && c->r[1]) {
        // the wheel's list r2 (wheel_ids). Not every wheel has all 7 (s114, Aversa boss: Tactics only on Itsuki's, so 6
        // entries for the others and L picked Escape, - pointed past the end)
        const uint32_t ids = wheel_ids(c->r[2]);
        if (ids && ids != rt::g_wheel_ids) rt::g_wheel_ids = ids;
        if (ms_now() - rt::g_wheel_ms > 500 && rt::battle_buttons_mode()) log("p5: wheel up, sel %u, commands %07x", ld32(c->r[1]), ids);
        rt::g_wheel_ms = ms_now();
        rt::g_wheel_sel = (int)ld32(c->r[1]);
    }
    static const bool wlog = getenv("NX_WHEEL_LOG") != nullptr;  // debug: callers, selection and list of each distinct call
    if (wlog) {
        static std::set<std::string> seen;
        std::string k = std::string(symbol_at(c->r[14])) + " sel " + std::to_string(c->r[1] ? ld32(c->r[1]) : ~0u) + " list";
        for (int i = 0; c->r[2] && i < 12; i++) { char b[16]; snprintf(b, sizeof b, " %08x", ld32(c->r[2] + 4 * i)); k += b; }
        if (seen.insert(k).second && seen.size() < 400) log("wheel: obj %08x sel@%08x %s", c->r[0], c->r[1], k.c_str());
    }
    static const bool dump = getenv("NX_P5_DUMP") != nullptr;  // debug: rare changes in the wheel input object (r0)
    if (dump && (c->r[14] & ~1u) == 0x2a8530) {
        static uint32_t prev[1024], n[1024], was = 0;
        const uint32_t o = c->r[0];
        for (int i = 0; i < 1024; i++) {
            const uint32_t v = ld32(o + 4 * i);
            if (o == was && v != prev[i] && ++n[i] < 8) log("p5in: +%03x %08x -> %08x", 4 * i, prev[i], v);
            prev[i] = v;
        }
        was = o;
    }
    if (ui_skip(c->r[0] + 0x195ac)) { c->r[0] = 0; return; }
    const uint32_t sel_at = c->r[1];
    c->r[0] = call_unhooked(c, "btl_wheel_input", {c->r[0], c->r[1], c->r[2]});
    static const bool p5log = getenv("NX_P5_LOG") != nullptr;  // debug: the wheel result right after a P5 pick
    if (p5log && rt::g_p5_target >= 0 && sel_at) log("p5: wheel call lr %08x sel %u ret %u", c->r[14], ld32(sel_at), c->r[0]);
    if ((c->r[14] & ~1u) == 0x2a8530 && c->r[0] == 1) g_wheel_done = true, rt::g_wheel_ms = 0, rt::g_wheel_open_ms = 0;  // panel off at once
    if (rt::g_p5_target >= 0 && (c->r[14] & ~1u) == 0x2a8530 && c->r[0] == 1) {  // confirmed: no more taps (poll_input)
        log("p5: wheel confirmed command %u", ld32(sel_at));
        rt::g_p5_target = -1;
    }
}
// The "!" weakness marker over enemies while the skill list is open (before a target is picked): its draw function steps
// the pulse counter u16 unit+0xaca once per call, i.e. per frame (user 10-30: too fast above 60). Keep the counter on
// frames that are not a 60 Hz tick (a reset to 0 by the game stays).
// Resist marker (user 11-24): affinity byte unit+0x85a+element (element = highlighted skill record +0x12): 0-2 weak
// ("!"), 3 normal, 4-6 resist, 7 null, 8 repel, 9 drain (matched to the target panel badges by poking, s106). When the
// game drew nothing and the value is a resist, the same draw runs again with the value read as weak and the "!" sprite
// records (bank 45 sheet 0 #527 / #528) showing the Rs badge's uv (#800; the user's shield when resist_icons is on).
// The game's own checks (unknown affinity, guard buffs, skills that ignore affinity) still apply in that second run.
static uint32_t battle_sprite(uint32_t index) {  // bank 45 sheet 0 record, 0 when the battle HUD is not loaded
    const uint32_t m = ld32(0x9f3320), t = m ? ld32(m + 8) : 0, b0 = t ? ld32(t + 45 * 0xc) : 0,
                   bk = b0 >= 0x10000000 ? ld32(b0 + 0x10) : 0, recs = bk >= 0x10000000 ? ld32(bk + 0x10) : 0;
    return recs >= 0x10000000 ? recs + index * 0x70 : 0;
}
// The "!" sprite records #527 / #528 showing the resist shield while this lives: uv = the 84 px shield painted into
// empty atlas space (pad_icons k_battle_res) when resist icons are on, else the small Rs letters (#800); colour white
// (the "!" records are tinted green). ok = false when the battle HUD is not loaded.
struct ResistSprites {
    uint32_t m1 = battle_sprite(527), m2 = battle_sprite(528), rs = battle_sprite(800);
    uint8_t s1[0x58], s2[0x58];  // record +0x18 uv, +0x30 four corner colours (RGBA floats)
    bool ok = m1 && m2 && rs;
    ResistSprites() {
        if (!ok) return;
        float uv[4] = {198 / 2048.0f, 110 / 2048.0f, 282 / 2048.0f, 194 / 2048.0f};
        if (!rt::resist_icons_on()) memcpy(uv, GPTR(rs + 0x18), 16);
        memcpy(s1, GPTR(m1 + 0x18), 0x58), memcpy(s2, GPTR(m2 + 0x18), 0x58);
        for (uint32_t m : {m1, m2}) {
            memcpy(GPTR(m + 0x18), uv, 16);
            for (int k = 0; k < 16; k++) st32(m + 0x30 + 4 * k, 0x3f800000);  // 1.0f
        }
    }
    ~ResistSprites() {
        if (ok) memcpy(GPTR(m1 + 0x18), s1, 0x58), memcpy(GPTR(m2 + 0x18), s2, 0x58);
    }
};
// setting resist_marker (default 1): resist marker, target panel resist icon and RESIST popup
static bool resist_marker_on() { return !getenv("NX_RESIST_MARKER") || atoi(getenv("NX_RESIST_MARKER")) != 0; }
static bool resist_mark(Cpu* c, uint32_t unit, uint16_t counter) {
    const uint16_t id = ld16(unit + 0xac8);  // highlighted skill id, 0 = none
    const uint32_t table = ld32(ld32(0x9ef4c4));  // skill table; record = table + 0xc + id * 0x84 (0x27f280)
    if (id == 0 || !table || !resist_marker_on()) return false;
    const uint32_t skill = table + 0xc + id * 0x84;
    const uint32_t aff = unit + 0x85a + (int16_t)ld16(skill + 0x12);
    const uint8_t v = ld8(aff);
    if (v < 4 || v > 6) return false;
    ResistSprites sprites;
    if (!sprites.ok) return false;
    const uint32_t hp_bar = ld32(unit + 0xab4);  // the HP gauge part of the function ran in the first call
    st32(unit + 0xab4, 0), st8(aff, 2), st16(unit + 0xaca, counter);
    const Cpu keep = *c;
    call_unhooked(c, "btl_weak_mark_draw", {unit});
    *c = keep;
    st32(unit + 0xab4, hp_bar), st8(aff, v);
    return true;
}
// Target panel affinity icon (user 11-24): the disc left of the target's name, (panel, x, y, kind, [sp] ?) with kind =
// the affinity value as the panel decided it (0xffff unknown): 0-2 green disc + "!", 3-6 the plain star disc, 7-9 red
// disc + Nu / Rp / Dr. A resist (4-6) is drawn as kind 0 with the "!" records showing the shield (ResistSprites).
HLE_FN(btl_target_aff_icon, "btl_target_aff_icon") {
    const uint16_t kind = (uint16_t)c->r[3];
    if (kind >= 4 && kind <= 6 && resist_marker_on()) {
        ResistSprites sprites;
        if (sprites.ok) {
            call_unhooked(c, "btl_target_aff_icon", {c->r[0], c->r[1], c->r[2], 0});
            return;
        }
    }
    call_unhooked(c, "btl_target_aff_icon", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
// RESIST hit popup (user 11-24): the game marks a resisted hit with popup flag 0x4 (+0x58) but draws nothing for it
// (0x2 WEAK, 0x8 BLOCK = null, 0x100 CRITICAL). Such a popup is drawn as a WEAK one with the WEAK sprite record (bank 45
// sheet 0 #490) pointing at our RESIST art (pad_icons k_battle_res, painted into empty atlas space, same size); the
// damage number below it stays.
HLE_FN(btl_dmg_popup_draw, "btl_dmg_popup_draw") {
    const uint32_t p = c->r[0], flags = ld32(p + 0x58), weak = battle_sprite(490);
    if ((flags & 0xe) == 0x4 && weak && resist_marker_on()) {
        const float uv[4] = {378 / 2048.0f, 254 / 2048.0f, 582 / 2048.0f, 346 / 2048.0f};
        uint8_t keep[16];
        memcpy(keep, GPTR(weak + 0x18), 16), memcpy(GPTR(weak + 0x18), uv, 16);
        st32(p + 0x58, flags | 0x2);
        c->r[0] = call_unhooked(c, "btl_dmg_popup_draw", {c->r[0], c->r[1], c->r[2]});
        st32(p + 0x58, (ld32(p + 0x58) & ~0x2u) | (flags & 0x2));
        memcpy(GPTR(weak + 0x18), keep, 16);
        return;
    }
    static const bool debug = getenv("NX_DEBUG_BATTLE") != nullptr;
    if (debug) {
        static std::map<uint32_t, uint32_t> seen;
        const uint32_t key = ld32(p + 0x58) ^ ld32(p + 0x50) << 12;
        if (seen[p] != key + 1) {
            seen[p] = key + 1;
            std::string w;
            for (int i = 0; i < 0x80; i += 4) w += std::to_string(i) + ":" + std::to_string((int32_t)ld32(p + i)) + " ";
            log("battle: popup %08x flags %x value %d: %s", p, ld32(p + 0x58), (int32_t)ld32(p + 0x50), w.c_str());
        }
    }
    c->r[0] = call_unhooked(c, "btl_dmg_popup_draw", {c->r[0], c->r[1], c->r[2]});
}
// WEAK in the skill list (Augs 11-27, like P5R; setting weak_hint, default 1): the "!" marker draw runs per frame for
// each unit while the skill list is open; the foes among them (weak_hint_seen) are remembered 250 ms. A list row
// (btl_skill_row_draw 0x2f2154: row, x, y, highlighted, [sp] alpha; skill id u16 row+0xc) gets the WEAK popup sprite
// (bank 45 #490, id 0x1ad2766a) small at its right when the game's own "!" marker would show for that skill on a foe:
// weak_mark_rule is btl_weak_mark_draw's decision (0x303dc4..0x303f58), so slayer skills (Horseslayer on cavalry),
// attacker passives, guard buffs and unknown affinities count exactly as in the game (user 11-28: physical skills
// were missing: the first version only had the element-weakness part).
static std::map<uint32_t, uint32_t> g_weak_foes;  // unit -> GetTickCount of its last "!" draw
static bool weak_hint_on() { return !getenv("NX_WEAK_HINT") || atoi(getenv("NX_WEAK_HINT")) != 0; }
static bool skill_offensive(uint16_t id) {
    const uint32_t table = ld32(ld32(0x9ef4c4));
    return id && table && (ld8(table + 0xc + id * 0x84 + 8) & 1);
}
// The "!" draw runs for party members too; the game puts the highlighted skill (u16 unit+0xac8) only on the units it
// can target (s120 log: Zio -> both foes 9, Itsuki / Tsubasa 0). A unit counts as a foe once an attack skill was
// highlighted on it, and stays one while its "!" draw keeps running.
static void weak_hint_seen(uint32_t unit) {
    const uint16_t hl = ld16(unit + 0xac8);
    if (g_weak_foes.count(unit) || (hl && skill_offensive(hl))) g_weak_foes[unit] = GetTickCount();
}
// btl_weak_mark_draw's rule for skill record `skill` on foe `unit` (lifted 0x303dc4..0x303f58): r6 = affinity byte (data
// +0x42 + element; 0-2 weak, 7 null, 8 repel), -1 when the player does not know it (0x354390); vf 0x14c data +0x196 set
// and type +0x838 != 10 -> none; non-almighty skills (+0x4c != 0x49): kind +6 1 / 2 (phys / magic) with status +0x83c
// 0x8000 / 0x10000 -> 8, 0x1000 / 0x2000 -> 7; attacker (0x2b3804 of **0x9ef428) data +0x178 + element 1 + foe type 4..10
// in 0x47, +0x17a + element 3 + type 0..7 in 0x8c, +0x4c 0x13, +2 == 4, +0x5e == 1, foe +0x81e & (+0x28 | +0x2c)
// (slayer), +0x4c 0x5a -> 2; shown when 0 <= r6 <= 2 and the skill does not ignore affinity (+0x20 != 1).
static bool weak_mark_rule(Cpu* c, uint32_t unit, uint32_t skill) {
    const uint32_t data = unit + 0x818;
    const uint16_t el = ld16(skill + 0x12);
    int32_t r6 = ld8(data + 0x42 + (int16_t)el);
    c->r[0] = ld16(data), c->r[1] = el;
    call_guest(c, 0x354390);
    if (!c->r[0]) r6 = -1;
    if (ld16(gcall(c, ld32(ld32(unit) + 0x14c), {unit}) + 0x196) && ld16(unit + 0x838) != 0xa) return false;
    if (el != 0xb && ld16(skill + 0x4c) != 0x49) {
        const uint32_t st = ld32(unit + 0x83c), kind = ld16(skill + 6);
        if (kind == 1 || kind == 2) {
            if (st & (kind == 1 ? 0x8000u : 0x10000u)) r6 = 8;
            if (st & (kind == 1 ? 0x1000u : 0x2000u)) r6 = 7;
        }
    }
    const uint32_t actor = gcall(c, 0x2b3804, {ld32(ld32(0x9ef428))});
    const uint32_t ad = actor ? gcall(c, ld32(ld32(actor) + 0x14c), {actor}) : 0;
    const uint16_t type = ld16(unit + 0x838);
    if (ad && ld16(ad + 0x178) && el == 1 && (uint16_t)(type - 4) <= 6 && (0x47u >> ((type - 4) & 0xff) & 1)) r6 = 2;
    if (ad && ld16(ad + 0x17a) && el == 3 && type <= 7 && (0x8cu >> type & 1)) r6 = 2;
    const uint16_t k4c = ld16(skill + 0x4c);
    if (k4c == 0x13 || ld16(skill + 2) == 4 || ld16(skill + 0x5e) == 1) r6 = 2;
    if (ld16(unit + 0x81e) & (ld16(skill + 0x28) | ld16(skill + 0x2c))) r6 = 2;
    if (r6 != -1 && r6 != 8 && k4c == 0x5a) r6 = 2;
    return r6 >= 0 && r6 <= 2 && ld16(skill + 0x20) != 1;
}
static bool skill_hits_weak(Cpu* c, uint16_t id) {
    if (!skill_offensive(id)) return false;
    const uint32_t skill = ld32(ld32(0x9ef4c4)) + 0xc + id * 0x84;
    const uint32_t now = GetTickCount();
    bool weak = false;
    const Cpu keep = *c;
    for (auto it = g_weak_foes.begin(); it != g_weak_foes.end();) {
        if (now - it->second > 250) { it = g_weak_foes.erase(it); continue; }
        const uint32_t unit = it->first;
        ++it;
        if (weak || !ld32(unit + 0x804)) continue;
        static const bool test = getenv("NX_WEAK_HINT_TEST") != nullptr;  // debug: every offensive skill shows WEAK
        weak = test || weak_mark_rule(c, unit, skill);
    }
    *c = keep;
    return weak;
}
HLE_FN(btl_skill_row_draw, "btl_skill_row_draw") {
    const uint32_t row = c->r[0], x = c->r[1], y = c->r[2], alpha = ld32(c->r[13]);
    call_unhooked(c, "btl_skill_row_draw", {c->r[0], c->r[1], c->r[2], c->r[3]});
    if (!weak_hint_on() || g_weak_foes.empty() || !skill_hits_weak(c, ld16(row + 0xc))) return;
    static const float sx = getenv("NX_WEAK_HINT_X") ? (float)atof(getenv("NX_WEAK_HINT_X")) : 236.f,
                       sy = getenv("NX_WEAK_HINT_Y") ? (float)atof(getenv("NX_WEAK_HINT_Y")) : 4.f,
                       sc = getenv("NX_WEAK_HINT_SCALE") ? (float)atof(getenv("NX_WEAK_HINT_SCALE")) : 0.38f;
    const uint32_t sprite = 0x1ad2766a;
    const Cpu keep = *c;
    c->r[0] = sprite;
    call_guest(c, 0x23e174);  // sprite record: +0x10 / +0x14 size
    const uint32_t rec = c->r[0];
    if (rec >= 0x10000000) {
        const float w = (float)(int32_t)ld32(rec + 0x10) * sc, h = (float)(int32_t)ld32(rec + 0x14) * sc;
        c->r[0] = sprite, c->r[1] = alpha;
        c->vr.f[0] = w, c->vr.f[1] = h, c->vr.f[2] = 1.0f;
        c->vr.f[3] = (float)(int32_t)x + sx, c->vr.f[4] = (float)(int32_t)y + sy - h / 2;
        call_guest(c, 0x23cba4);
    }
    *c = keep;
}
HLE_FN(btl_weak_mark_draw, "btl_weak_mark_draw") {
    const uint32_t unit = c->r[0];
    weak_hint_seen(unit);
    const uint16_t before = ld16(unit + 0xaca);
    const bool hold = fps_setting() != 30 && g_ui_gate != 0 && !ui_tick_due(unit + 0xaca);
    c->r[0] = call_unhooked(c, "btl_weak_mark_draw", {unit});
    static const bool wcheck = getenv("NX_WEAK_HINT_CHECK") != nullptr;  // debug: weak_mark_rule vs the game's own "!"
    if (wcheck && ld16(unit + 0xac8) && skill_offensive(ld16(unit + 0xac8))) {
        const Cpu keep = *c;
        const bool game = ld16(unit + 0xaca) != 0, ours = weak_mark_rule(c, unit, ld32(ld32(0x9ef4c4)) + 0xc + ld16(unit + 0xac8) * 0x84);
        *c = keep;
        static int agree = 0, differ = 0;
        (game == ours ? agree : differ)++;
        if (game != ours && differ <= 20) log("weak check: unit %08x skill %u game %d ours %d", unit, ld16(unit + 0xac8), game, ours);
        if ((agree + differ) % 100 == 0) log("weak check: %d agree, %d differ", agree, differ);
    }
    if (ld16(unit + 0xaca) == 0) resist_mark(c, unit, before), c->r[0] = 1;
    if (hold && ld16(unit + 0xaca) != 0) st16(unit + 0xaca, before);
    static const bool debug = getenv("NX_DEBUG_BATTLE") != nullptr;
    if (debug) {  // pulse steps per second over all units
        static std::set<uint32_t> units;
        if (units.size() < 16 && units.insert(unit).second) {
            std::string a;
            for (int i = 0; i < 24; i++) a += std::to_string(ld8(unit + 0x85a + i)) + " ";
            std::string w;
            for (uint32_t o = 4; o < 0x80; o += 4) w += std::to_string((int32_t)ld32(unit + o)) + " ";
            w += "| slots";
            if (const uint32_t b = ld32(0x9ef428))
                for (uint32_t i = 0; i < 16; i++) w += " " + std::to_string(ld32(b + 0xd8 + 4 * i) == unit ? 1 : 0);
            w += " | data";
            for (uint32_t o = 0; o < 0x20; o += 2) w += " " + std::to_string(ld16(unit + 0x818 + o));
            log("battle: weak marker unit %08x affinities %s; %s", unit, a.c_str(), w.c_str());
            if (const char* t = getenv("NX_AFF_TEST"))  // debug: comma list, element i gets value i (both copies)
                for (int i = 0; i < 12 && *t; i++, t += strcspn(t, ",") + (t[strcspn(t, ",")] != 0))
                    st8(unit + 0x85a + i, (uint8_t)atoi(t)), st8(unit + 0x866 + i, (uint8_t)atoi(t));
        }
        static std::map<uint32_t, uint16_t> hl;
        if (hl.count(unit) == 0 || hl[unit] != ld16(unit + 0xac8))
            hl[unit] = ld16(unit + 0xac8), log("battle: unit %08x (id %u) highlighted skill %u", unit, ld16(unit + 0x818), hl[unit]);
        static int steps = 0;
        static uint32_t t0 = GetTickCount();
        steps += ld16(unit + 0xaca) != before;
        if (GetTickCount() - t0 >= 1000) log("battle: weak marker %d steps/s", steps), steps = 0, t0 = GetTickCount();
    }
}
// Unit colour effects (0x30df68, per unit per frame): five tweens (material colour, emission, alpha, ...) count frames
// (0x2ce764 / 0x2ce490: +1 per call, t = counter / length), e.g. the yellow flash pulse on the picked target, 30 frames a
// period: 1 s on the Switch, 0.5 s at 60 fps, 0.125 s at 240 (user 11-13 / 11-17: target highlight too fast). Stepped on
// the battle's 30 Hz ticks only (Switch pace). NX_UNIT_COLOR30=0 off.
HLE_FN(btl_unit_color_update, "btl_unit_color_update") {
    static const bool on = !getenv("NX_UNIT_COLOR30") || atoi(getenv("NX_UNIT_COLOR30")) != 0;
    if (on && fps_setting() != 30 && !tick_due(c->r[0] + 0x60c)) return;
    call_unhooked(c, "btl_unit_color_update", {c->r[0]});
}
// Action banner (debug, NX_DEBUG_BATTLE): pending requests turned into queue entries, and banners started.
HLE_FN(btl_banner_requests, "btl_banner_requests") {
    static const bool debug = getenv("NX_DEBUG_BATTLE") != nullptr;
    const uint32_t b = c->r[0];
    uint32_t pend[24][2];
    for (int i = 0; i < 24; i++) pend[i][0] = ld32(b + 0x28 + i * 12), pend[i][1] = ld32(b + 0x2c + i * 12);
    const uint32_t state = ld32(b + 0x14), head = ld32(b + 0x14c);
    // the game queues a request only if its unit (battle manager ld32(ld32(0x9ef428)) + 0xd8 + id*4) has +0x838 != 1
    // and !(+0x84 & 2); every other request is dropped at the end. Logged always (a few lines per action) so a user run
    // that misses an enemy banner shows whether this drop is the cause (user report 10-30, not reproduced here).
    if (ld32(b + 0x24)) {
        const uint32_t mgr = ld32(ld32(0x9ef428));
        for (int i = 0; i < 24; i++)
            if (pend[i][0] != ~0u) {
                const uint32_t u = mgr && pend[i][0] < 16 ? ld32(mgr + 0xd8 + pend[i][0] * 4) : 0;
                log("battle: banner request unit %u kind %u: unit+0x838 %u +0x84 %02x (frame %llu)", pend[i][0], pend[i][1],
                    u ? ld16(u + 0x838) : 0xffff, u ? ld8(u + 0x84) : 0xff, (unsigned long long)g_frame_no);
            }
    }
    call_unhooked(c, "btl_banner_requests", {b});
    if (!debug) return;
    static uint32_t prev_state = 0;
    if (state != prev_state) log("battle: banner state %u -> %u", prev_state, state), prev_state = state;
    for (int i = 0; i < 24; i++)
        if (pend[i][0] != ~0u && ld32(b + 0x28 + i * 12) == ~0u)
            log("battle: banner request unit %u kind %u queued (state %u, frame %llu)", pend[i][0], pend[i][1], state, (unsigned long long)g_frame_no);
    static uint32_t last_pending = 0;
    uint32_t n = 0;
    for (int i = 0; i < 24; i++) n += ld32(b + 0x28 + i * 12) != ~0u;
    if (n != last_pending) log("battle: banner requests pending %u", n), last_pending = n;
    if (ld32(b + 0x148) && ld32(b + 0x14c) != head) log("battle: banner queue head skill %d", (int)ld32(b + 0x14c));
}
// Skill-name panel (the banner over an acting unit): open(panel, &skill, unit, ?, flag) returns WITHOUT showing anything
// when the panel still holds the same unit + skill (+0x28/+0x20; cleared 6 panel updates after a close, 0x2f13b8).
// Logged always (once per action).
// Missing enemy banner (user 10-30, measured 10-31: 2 of 12 enemy banners at 240 fps, 0 at 60): an open that lands while
// the previous banner is still CLOSING (+4 == 2, still visible) sets the new text, but show 0x2f2118 returns early
// (+0x18 != 0) and the close then finishes and hides it. Fix: finish that close first, exactly as the panel update
// 0x2f13b8 does when its timer runs out, so the open + show that follow start from a hidden panel.
PROBE_FN(btl_skill_panel_open, "btl_skill_panel_open") {
    const uint32_t p = c->r[0], unit = c->r[2];
    const int skill = (int16_t)ld16(c->r[1]);
    const bool closing = ld32(p + 0x18) && ld32(p + 4) == 2 && skill != 0x18b;
    const bool same = !closing && ld32(p + 0x28) == unit && (int16_t)ld16(p + 0x20) == skill;
    log("battle: skill panel open unit %08x skill %d from %s: panel state %u timer %d visible %u%s (frame %llu)", unit, skill,
        symbol_at(c->r[14]), ld32(p + 4), (int)ld32(p + 0x1c), ld32(p + 0x18),
        closing ? " -> close finished first" : same ? " -> SKIPPED (same unit + skill)" : "", (unsigned long long)g_frame_no);
    if (!closing) return;
    uint32_t keep[4] = {c->r[0], c->r[1], c->r[2], c->r[3]}, r12 = c->r[12];
    st32(p + 0x28, 0xffffffffu);
    st16(p + 0x20, 0xffff);
    st32(p + 0x18, 0);
    st32(p + 0x1c, 0);
    for (uint32_t o : {0x34u, 0x50u, 0x6cu, 0x88u}) c->r[0] = p + o, call_guest(c, 0x2f9980);
    st32(p + 4, 0);
    memcpy(c->r, keep, sizeof keep), c->r[12] = r12;
}
// Post-battle screens (RESULTS, level-up, skill inheritance): their own tasks, counting 30 Hz frames. A skipped frame
// returns what the last real update returned (the task list reads it). Each hook has its own tick key (object + 1 +
// line): some of these tasks share one object. The last value is kept per key, and a task's first call always runs: one
// shared value could hand a new task the previous task's 0 (finished) and the task list dropped it.
static std::unordered_map<uint32_t, uint32_t> g_task_last;
// The RESULTS background's opening wipe (result_bg_update state 4: its kind-1 object, update 0x47dfc8 = 8 + 10 + 10
// eased steps, one per call; result_flow waits for it) is an animation made for 30 Hz: at the 60 Hz UI pace it ran in
// 0.53 s, twice the Switch's (user 11-01: the wipe into RESULTS is too fast). It steps on 30 Hz ticks (game time).
static bool result_bg_skip(uint32_t obj, uint32_t key) {
    if (ld32(obj + 8) == 4 && fps_setting() != 30) return !tick_due(key);
    return ui_skip(key);
}
#define GATED_TASK_SKIP(fn, skip) \
    HLE_FN(fn, #fn) { \
        battle_ui_seen(); \
        const uint32_t key = c->r[0] + 1 + __LINE__ % 64; \
        auto it = g_task_last.find(key); \
        if (it != g_task_last.end() && (skip)) { c->r[0] = it->second; return; } \
        if (it == g_task_last.end()) log("post-battle: %s first update (frame %llu)", #fn, (unsigned long long)g_frame_no); \
        ui_tick_due(key); \
        UiDt dt_scope(key); \
        const uint32_t r = call_unhooked(c, #fn, {c->r[0]}); \
        if (r) g_task_last[key] = r; else g_task_last.erase(key), g_ui_tick.erase(key); \
        c->r[0] = r; \
    }
#define GATED_TASK(fn) GATED_TASK_SKIP(fn, ui_skip(key))
GATED_TASK(result_update)
GATED_TASK(levelup_update)
GATED_TASK(skill_learn_update)
GATED_TASK(result_flow_update)
GATED_TASK_SKIP(result_bg_update, result_bg_skip(c->r[0], key))
// Their "parts" tasks only draw (immediate sprites from the counters above, e.g. levelup_update 0x31c2a0 steps +0x68 /
// +0x318 that levelup_parts_update draws): they run every frame. Gating them skipped the level-up screen's drawing on
// most frames above 60 fps (white blink).
#define DRAW_TASK(fn) HLE_FN(fn, #fn) { battle_ui_seen(); c->r[0] = call_unhooked(c, #fn, {c->r[0]}); }
DRAW_TASK(result_parts_update)
DRAW_TASK(levelup_parts_update)
DRAW_TASK(result_bg_parts_update)
HLE_FN(party_hud_update, "party_hud_update") {
    if ((fps_setting() != 30 && !ui_tick_due(c->r[0])) || gate30_skip(2, c->r[0])) { c->r[0] = 1; return; }
    c->r[0] = call_unhooked(c, "party_hud_update", {c->r[0]});
}

// Pad key repeat counts 30 Hz frames too: at the end of each pad read (pad manager 0x54d73c, also 0x54e9e8) pad_repeat
// counts a held mask down (+0x44: delay +0x48, then interval +0x4c -> repeat bits +0x20), pad_hold_count adds 1 per call
// to 26 per-button hold counters and pad_repeat_bits sets +0x24 where (hold - delay) % interval == 0. Uncapped, one
// 150 ms `down` moved a menu 6 rows (60 fps: 2x faster repeats). Above 30 fps they count only on 30 Hz ticks; a new
// press still starts its counter at once (as on the Switch) and press bits +0x18 pass every frame. The three run in a
// row per pad object, so pad_repeat decides the tick for the other two.
static bool g_pad_tick = true;
static std::unordered_map<uint32_t, uint32_t> g_pad_bumped;  // pad -> hold counters its last pad_hold_count raised
// Presses / releases per pad read, for event scripts that only resume on 30 Hz ticks (script_resume_all): a press bit
// lives for one read, so at 240 fps a Lua prompt (Chrom's "Is this okay?" after a skill pick) saw ~1 press in 8.
struct PadEdge { uint64_t seq; uint32_t pad, press, release; };
static std::vector<PadEdge> g_pad_edges;  // newest last, nonzero only
static uint64_t g_pad_seq = 0;
HLE_FN(pad_repeat, "pad_repeat") {
    uint32_t p = c->r[0], held = ld32(p + 0x10);
    ++g_pad_seq;
    if (ld32(p + 0x18) | ld32(p + 0x1c)) {
        if (g_pad_edges.size() >= 64) g_pad_edges.erase(g_pad_edges.begin());
        g_pad_edges.push_back({g_pad_seq, p, ld32(p + 0x18), ld32(p + 0x1c)});
    }
    g_pad_tick = fps_setting() == 30 || ui_tick_due(p);
    if (!g_pad_tick && held && held == ld32(p + 0x40)) { st32(p + 0x20, ld32(p + 0x18)); return; }
    call_unhooked(c, "pad_repeat", {p});
}
HLE_FN(pad_repeat_bits, "pad_repeat_bits") {
    uint32_t p = c->r[0];
    if (fps_setting() == 30) { call_unhooked(c, "pad_repeat_bits", {p}); return; }
    uint32_t bits = ld32(p + 0x18), bumped = g_pad_bumped[p];
    int32_t delay = ld32(p + 0x48), every = ld32(p + 0x4c);
    for (int i = 0; i < 26; i++) {
        int32_t d = (int32_t)ld32(p + 0x5c + 4 * i) - delay;  // the game's sdiv by 0 gives 0: then only d == 0 fires
        if ((bumped >> i & 1) && d >= 0 && (every ? d % every : d) == 0) bits |= 1u << i;
    }
    st32(p + 0x24, bits);
}
HLE_FN(pad_hold_count, "pad_hold_count") {
    uint32_t p = c->r[0];
    if (fps_setting() == 30) { call_unhooked(c, "pad_hold_count", {p}); return; }
    uint32_t held = ld32(p + 0x10), bumped = 0;
    for (int i = 0; i < 26; i++) {
        uint32_t a = p + 0x5c + 4 * i, n = ld32(a);
        if (!(held >> i & 1)) n = 0;
        else if (g_pad_tick || !n) n++, bumped |= 1u << i;
        st32(a, n);
    }
    g_pad_bumped[p] = bumped;
}

// Field enemy respawn countdown (0x4a5618, field enemy manager task): manager +0x10 +0x51dec counts 30 Hz frames down
// by 1 per call (3 while field global +8 == 0), respawns at 0 and reloads 30 x (91..600). Above 30 fps undo a plain
// decrement unless a 30 Hz tick has passed for this counter; reloads (larger or random new values) stay.
HLE_FN(field_spawn_timer, "field_spawn_timer") {
    uint32_t g = ld32(ld32(0x4a5ca8) + 0x4a562c), m = g ? ld32(g) : 0;
    if (fps_setting() == 30 || !m) { call_unhooked(c, "field_spawn_timer", {c->r[0]}); return; }
    uint32_t a = m + 0x10 + 0x51dec;
    int32_t before = ld32(a);
    bool tick = tick_due(a);
    call_unhooked(c, "field_spawn_timer", {c->r[0]});
    int32_t d = before - (int32_t)ld32(a);
    if (!tick && d >= 1 && d <= 3) st32(a, before);
}

// The field camera (controller 0x3c31c8) turns by the smoothed right stick once per frame: stick smoothing
// x = keep*x + (1-keep)*stick (keep = settings +0xcc) and yaw step = settings +0x50 / 10 x stick per update, so it
// turned 2x at 60 fps and far faster uncapped. Run each update with keep^(dt*30) and the speed x dt*30.
HLE_FN(cam_free_update, "cam_free_update") {
    uint32_t set = ld32(c->r[0] + 8);
    if (fps_setting() == 30 || !set) { call_unhooked(c, "cam_free_update", {c->r[0]}); return; }
    uint32_t keep_bits = ld32(set + 0xcc), speed_bits = ld32(set + 0x50);
    float keep, speed, k = g_game_dt * 30;
    memcpy(&keep, &keep_bits, 4);
    memcpy(&speed, &speed_bits, 4);
    float keep2 = keep > 0 ? powf(keep, k) : keep, speed2 = speed * k;
    memcpy(GPTR(set + 0xcc), &keep2, 4);
    memcpy(GPTR(set + 0x50), &speed2, 4);
    call_unhooked(c, "cam_free_update", {c->r[0]});
    st32(set + 0xcc, keep_bits);
    st32(set + 0x50, speed_bits);
}

// Event scripts (Lua coroutines) count frames: WaitForCnt yields once per frame and 0x5acac0 resumes every script once
// per frame, so a shorter dt would run them faster. Resume them once per 1/30 s of game time instead.
// Scripts that wait by time read the frame time at each resume: GetUpdateTime (global UPDATE_TIME) and info.delta,
// which the action manager (0x5b9144: battle and field actions, cuts) sets to dt x speed just before resuming. A
// resume must see the game time since that manager's last resume, or timed waits run slower than the animations
// they wait for (60 fps: battle actions out of sync, treasure pickup T-pose).
static const uint32_t UPDATE_TIME = 0x9f4498;
struct ScriptDelta { float value; uint32_t key, table; bool fresh; };
static std::unordered_map<uint32_t, ScriptDelta> g_script_delta;  // last info.delta per script manager; main thread

HLE_FN(lua_set_number, "lua_set_number") {
    if (c->r[1] && gstr(c->r[1], 8) == "delta") g_script_delta[c->r[0]] = {c->vr.f[0], c->r[1], c->r[2], true};
    call_unhooked(c, "lua_set_number", {c->r[0], c->r[1], c->r[2]});
}

// One resume: the pads also show every press / release since this manager's last resume (press +0x18, repeat +0x20 and
// +0x24 include presses, release +0x1c), restored after.
static void resume_scripts(Cpu* c, uint32_t m) {
    static std::unordered_map<uint32_t, uint64_t> seen;  // manager -> pad read seq at its last resume
    uint64_t& last = seen[m];
    struct Saved { uint32_t pad, w[4]; };
    std::vector<Saved> saved;
    for (const PadEdge& e : g_pad_edges) {
        if (e.seq <= last) continue;
        auto s = std::find_if(saved.begin(), saved.end(), [&](const Saved& x) { return x.pad == e.pad; });
        if (s == saved.end()) {
            saved.push_back({e.pad, {ld32(e.pad + 0x18), ld32(e.pad + 0x1c), ld32(e.pad + 0x20), ld32(e.pad + 0x24)}});
            s = saved.end() - 1;
        }
        st32(e.pad + 0x18, ld32(e.pad + 0x18) | e.press);
        st32(e.pad + 0x1c, ld32(e.pad + 0x1c) | e.release);
        st32(e.pad + 0x20, ld32(e.pad + 0x20) | e.press);
        st32(e.pad + 0x24, ld32(e.pad + 0x24) | e.press);
    }
    last = g_pad_seq;
    call_unhooked(c, "script_resume_all", {m});
    for (const Saved& s : saved)
        for (int i = 0; i < 4; i++) st32(s.pad + 0x18 + 4 * i, s.w[i]);
}

// Frame of the last script resume and the game time until that manager's next one (actor_move_update spreads
// script position steps over it).
static uint64_t g_resume_frame = 0;
static float g_resume_left = 1 / 30.0f;
HLE_FN(script_resume_all, "script_resume_all") {
    static std::unordered_map<uint32_t, float> accs, since_last;  // per script manager (several run each frame)
    uint32_t m = c->r[0];
    float& acc = accs[m];
    float& since = since_last[m];
    acc += g_game_dt;
    since += g_game_dt;
    // a script started since the last resume (node +0x10 == 1) runs now, as it would at 30 fps: an action's first
    // resume plays its animation, and waiting for the next 1/30 s tick showed the model unanimated (T-pose) meanwhile
    bool fresh_script = false;
    for (uint32_t n = ld32(m + 0x170); n && !fresh_script; n = ld32(n + 4)) fresh_script = ld32(n + 0x10) == 1;
    // The early resume takes the next tick's place (at most one tick of debt): acc = 0 here dropped the part of a tick
    // already counted, every time any script started. Unity performances (Lua FusionDemo) start actions / effects / camera
    // steps all the time: their frame-counted timeline (sound time table, cuts) fell 11 frames (0.38 s) behind the
    // real-time song by the end at 240 fps (USER 11-20 item 1: Carnage Unity audio late near the end).
    if (fresh_script) acc = std::max(acc - 1 / 30.0f, -1 / 30.0f);
    else if (acc < 1 / 30.0f - 1e-4f) return;
    else acc -= 1 / 30.0f;
    if (acc > 1 / 30.0f) acc = 0;  // dt above 1/30 (native loading screens): never catch up with extra resumes
    float k = since / g_game_dt;   // game frames since the last resume
    since = 0;
    g_resume_frame = g_frame_no.load(), g_resume_left = 1 / 30.0f - acc;
    auto d = g_script_delta.find(m);
    bool fresh = d != g_script_delta.end() && d->second.fresh;
    if (fresh) d->second.fresh = false;
    if (k < 1.001f) { resume_scripts(c, m); return; }
    if (fresh) {
        c->vr.f[0] = d->second.value * k;
        call_unhooked(c, "lua_set_number", {m, d->second.key, d->second.table});
    }
    uint32_t saved = ld32(UPDATE_TIME);
    float t;
    memcpy(&t, &saved, 4);
    t *= k;
    memcpy(GPTR(UPDATE_TIME), &t, 4);
    resume_scripts(c, m);
    st32(UPDATE_TIME, saved);
}

// Crowd walkers placed by the 30 Hz gated crowd manager (below): actor -> frame and 30 Hz tick of that placement.
struct Placed { uint64_t frame; uint32_t tick; };
static std::mutex g_town_lock;
static std::unordered_map<uint32_t, Placed> g_town_placed;
static thread_local bool t_in_mob_mgr = false;  // inside a 30 Hz town update: the crowd manager or a car's update
static void town_mark(uint32_t a) {  // an actor a town update placed or stepped (it sets each walker's / car's direction)
    if (!t_in_mob_mgr) return;
    std::lock_guard<std::mutex> l(g_town_lock);
    if (g_town_placed.size() > 4096) g_town_placed.clear();
    g_town_placed[a] = {g_frame_no.load(), g_tick30};
}
HLE_FN(actor_set_pos, "actor_set_pos") { town_mark(c->r[0]); call_unhooked(c, "actor_set_pos", {c->r[0], c->r[1]}); }
HLE_FN(actor_set_dir, "actor_set_dir") { town_mark(c->r[0]); call_unhooked(c, "actor_set_dir", {c->r[0], c->r[1]}); }
// The actor's last crowd placement if the crowd manager still places it (within 2 ticks), else frame 0.
static uint64_t town_placed_frame(uint32_t a) {
    std::lock_guard<std::mutex> l(g_town_lock);
    auto it = g_town_placed.find(a);
    return it != g_town_placed.end() && g_tick30 - it->second.tick <= 2 ? it->second.frame : 0;
}
static bool town_gate();
// A crowd actor's job (actor_move_update): the game keeps the true position (put back before the job, which moves it by
// the walker's step once per placement; between placements the step is held off), and the actor shows a glide from where
// it was shown to the newest true position, ending at the next 30 Hz tick. Jumps over 4 units (spawn, warp) snap.
// Cars too: a visible car's update only sets the move delta (0x3deb84; the job applies it), a culled car's sets its position.
// Yaw (+0x1c, set by actor_set_dir on the 30 Hz placement; the job never writes it) glides the same way over the shortest
// turn (s85: walkers / cars turned in 30 Hz steps). NX_TOWN_YAW=0: off.
struct TownActor { float auth[3], shown[3], from[3]; float seg_left = 0; uint32_t seg_tick = 0; uint64_t used = 0; bool have = false;
                   float yaw_shown = 0, yaw_from = 0, yaw_to = 0, yaw_left = 0; uint32_t yaw_tick = 0; bool have_yaw = false; };
static std::mutex g_town_actor_lock;
static std::unordered_map<uint32_t, TownActor> g_town_actors;
// Only while the actor still stands where we showed it: game code that moved it since (a traffic car's route restart
// 0x3de938 parks it / warps it to a new route start, a new car on a reused actor) owns the position. (USER 11-21,
// Shibuya chapter 2: a car drove over the pavement and through a wall = its new route followed from the old position.)
// Yaw too: the car update takes its direction from the actor yaw +0x1c (0x3c8758 at the start of 0x3df4fc), which holds the
// glided yaw between ticks; starting from it every turn came out short and the car overshot its lane and circled back
// (USER 11-21 video: bus U-turns on the scramble, cars into each other; worse at lower fps, the glide is further behind).
static void town_restore_true(uint32_t a) {  // put the game's own position / yaw back on a town actor we show elsewhere
    std::lock_guard<std::mutex> l(g_town_actor_lock);
    auto it = g_town_actors.find(a);
    if (it == g_town_actors.end() || !it->second.have) return;
    const float* p = (const float*)GPTR(a + 0xc);
    if (p[0] != it->second.shown[0] || p[2] != it->second.shown[2]) {
        static const bool dbg = getenv("NX_TOWN_LOG") != nullptr;
        if (dbg) log("[town] f%llu actor %08x moved by the game: kept %.2f %.2f -> now %.2f %.2f", (unsigned long long)g_frame_no.load(), a,
                     it->second.auth[0], it->second.auth[2], p[0], p[2]);
        it->second.have = false;
        return;
    }
    memcpy(GPTR(a + 0xc), it->second.auth, 12);
    float y;
    memcpy(&y, GPTR(a + 0x1c), 4);
    if (it->second.have_yaw && y == it->second.yaw_shown) memcpy(GPTR(a + 0x1c), &it->second.yaw_to, 4);
}
static void town_actor_update(Cpu* c, uint32_t a) {
    using T = TownActor;
    auto& m = g_town_actor_lock;
    auto& ts = g_town_actors;
    const uint64_t f = g_frame_no.load(), placed = town_placed_frame(a);
    float p[3], auth[3], old[3];
    memcpy(p, GPTR(a + 0xc), 12);
    bool fresh;
    {
        std::lock_guard<std::mutex> l(m);
        T& s = ts.size() < 4096 ? ts[a] : (ts.clear(), ts[a]);
        memcpy(old, s.have ? s.auth : p, 12);
        if (!s.have || p[0] != s.shown[0] || p[2] != s.shown[2]) memcpy(s.auth, p, 12);  // set by the game since we showed it
        s.auth[1] = p[1];  // height: the job's own (ground, gravity), never blended
        if (!s.have) memcpy(s.shown, p, 12), memcpy(s.from, p, 12), s.have = true;
        memcpy(auth, s.auth, 12);
        fresh = f - placed <= 1 && s.used != placed;
        if (fresh) s.used = placed;
    }
    memcpy(GPTR(a + 0xc), auth, 12);
    const uint32_t hx = ld32(a + 0x6c), hz = ld32(a + 0x74);
    static const uint32_t dbg_a = getenv("NX_TOWN_DBG") ? (uint32_t)strtoul(getenv("NX_TOWN_DBG"), nullptr, 16) : 0;
    if (dbg_a == a || dbg_a == 1) {
        float dx, dz;
        memcpy(&dx, &hx, 4), memcpy(&dz, &hz, 4);
        log("[towndbg] f%llu actor %08x placed f%llu fresh %d in %.3f %.3f auth %.3f %.3f delta %.3f %.3f d8 %u", (unsigned long long)f,
            a, (unsigned long long)placed, fresh, p[0], p[2], auth[0], auth[2], dx, dz, ld32(a + 0xd8));
    }
    if (!fresh) st32(a + 0x6c, 0), st32(a + 0x74, 0);
    call_unhooked(c, "actor_move_update", {a, c->r[1]});
    if (!fresh && ld32(a + 0x6c) == 0 && ld32(a + 0x74) == 0) st32(a + 0x6c, hx), st32(a + 0x74, hz);
    float q[3], show[3];
    memcpy(q, GPTR(a + 0xc), 12);
    {
        std::lock_guard<std::mutex> l(m);
        T& s = ts[a];
        if (q[0] != old[0] || q[2] != old[2]) {  // a new true position: glide to it from here
            memcpy(s.from, s.shown, 12);
            s.seg_left = g_tick30_left + g_game_dt, s.seg_tick = g_tick30;  // from the previous frame: this one moves too
        }
        memcpy(s.auth, q, 12);
        const float d[2] = {q[0] - s.from[0], q[2] - s.from[2]};
        const float r = s.seg_tick != g_tick30 || s.seg_left <= 1e-4f || d[0] * d[0] + d[1] * d[1] > 16.0f
                            ? 0.0f : std::clamp(g_tick30_left / s.seg_left, 0.0f, 1.0f);  // share of the glide still to go
        show[0] = q[0] - d[0] * r, show[1] = q[1], show[2] = q[2] - d[1] * r;
        memcpy(s.shown, show, 12);
        static const bool yaw_on = !getenv("NX_TOWN_YAW") || atoi(getenv("NX_TOWN_YAW")) != 0;
        if (yaw_on) {
            float y;
            memcpy(&y, GPTR(a + 0x1c), 4);
            if (!s.have_yaw) s.yaw_from = s.yaw_to = s.yaw_shown = y, s.have_yaw = true;
            else if (y != s.yaw_shown) s.yaw_from = s.yaw_shown, s.yaw_to = y, s.yaw_left = g_tick30_left + g_game_dt, s.yaw_tick = g_tick30;
            const float dy = std::remainder(s.yaw_to - s.yaw_from, 6.2831853f);  // shortest turn
            const bool snapped = d[0] * d[0] + d[1] * d[1] > 16.0f;              // spawn / warp: turn snaps with the position
            const float ry = snapped || s.yaw_tick != g_tick30 || s.yaw_left <= 1e-4f ? 0.0f : std::clamp(g_tick30_left / s.yaw_left, 0.0f, 1.0f);
            s.yaw_shown = s.yaw_to - dy * ry;
            memcpy(GPTR(a + 0x1c), &s.yaw_shown, 4);
        }
    }
    memcpy(GPTR(a + 0xc), show, 12);
    static const bool trace = getenv("NX_MOVE_LOG") && atoi(getenv("NX_MOVE_LOG")) >= 2;  // same line as actor_move_update
    if (trace) {
        float yaw;
        memcpy(&yaw, GPTR(a + 0x1c), 4);
        log("[movef] f%llu actor %08x pos %.4f %.4f %.4f dt %.5f yaw %.4f", (unsigned long long)f, a, show[0], show[1], show[2], g_game_dt, yaw);
    }
}

// Script-moved actors (cutscene walks): the cut script sets the actor's position (Lua binding 0x41a894 ->
// actor_set_pos 0x3c853c) on each of its 30 Hz resumes, so above 30 fps the actor moves on every 2nd frame only
// (0.33, 0, 0.33, 0...). Chain physics (Tsubasa's ribbon, hair) then gets a jolt every other frame and flaps (user
// 11-02, boss door cutscene in save 20). actor_move_update runs every frame, after the scripts: a position change made
// outside it in a frame where a script resumed, after at least one calm frame, and small (a step, not a teleport) is
// pulled back and replayed linearly in game time until that script manager's next resume (g_resume_left).
// By time, not frames: at 240 fps / 4K the frames between resumes vary (3, 4, 5...) and a frame count left jolts
// (user 11-03, save 3 walk to the red tent; s52 counted frames). Per-frame motion passes unchanged.
// NX_SMOOTH_MOVE=0: off. NX_MOVE_LOG=1: log each outside step.
HLE_FN(actor_move_update, "actor_move_update") {
    static const bool on = !getenv("NX_SMOOTH_MOVE") || atoi(getenv("NX_SMOOTH_MOVE")) != 0;
    static const bool dbg = getenv("NX_MOVE_LOG") != nullptr;
    const uint32_t a = c->r[0];
    if (!on || fps_setting() == 30) { call_unhooked(c, "actor_move_update", {a, c->r[1]}); return; }
    if (town_gate() && town_placed_frame(a)) { town_actor_update(c, a); return; }
    struct Move { uint64_t last_seen = 0, last_jump = 0; uint32_t calm = 0; float left = 0, post[3] = {}, pend[3] = {}, since = 1e9f; bool spread = false; };
    static std::mutex mx;
    static std::unordered_map<uint32_t, Move> moves;
    float p[3];
    memcpy(p, GPTR(a + 0xc), 12);
    {
        std::lock_guard<std::mutex> l(mx);
        Move& s = moves[a];
        const uint64_t f = g_frame_no.load();
        const float dt = std::max(g_game_dt, 1e-4f);
        const bool history = s.last_seen + 1 == f;  // updated in the previous frame too
        s.since += dt;  // game time since this actor's last outside step
        s.last_seen = f;
        const float j[3] = {p[0] - s.post[0], p[1] - s.post[1], p[2] - s.post[2]};
        const float jump2 = j[0] * j[0] + j[1] * j[1] + j[2] * j[2];
        if (!history) {
            s.calm = 0, s.left = 0, s.spread = false, s.since = 1e9f;
        } else if (jump2 > 0) {  // set from outside since our last update
            const uint64_t gap = f - s.last_jump;
            s.last_jump = f;
            if (s.left > 0) for (int i = 0; i < 3; i++) p[i] += s.pend[i];  // an unfinished replay finishes now
            s.left = 0;
            // A step: < 4 units, or < 16 while the actor is moving (its last step within ~3 resumes). Unity performances
            // fly the cast 4.2-7.9 units per 1/30 s (Radiant Unity, USER 11-20 item 2: the 4-unit cap kept those as 30 Hz
            // jumps under a smooth camera); their cuts teleport >= 39 units, a cutscene's first placement can be ~14.
            const bool moving = s.since < 3.5f / 30;
            if (g_resume_frame == f && s.calm >= 1 && (jump2 < 16.0f || (moving && jump2 < 256.0f)) && g_resume_left > 1e-4f) {
                // shown = linear in game time from the resume moment (this frame is 1/30 - left past it) to the next step:
                // the next resume, or 2-3 resumes on when the script moves the actor only every 2nd / 3rd one and it was
                // moving before (Radiant Unity cast at 15 Hz: 5 frames moving, 5 still at 150 fps)
                const float ticks = moving && s.spread ? std::clamp(std::round(s.since * 30), 1.0f, 3.0f) : 1.0f;
                const float span = ticks / 30, left = span - (1 / 30.0f - g_resume_left);
                const float keep = std::min(1.0f, left / span);
                for (int i = 0; i < 3; i++) s.pend[i] = j[i] * keep, p[i] -= s.pend[i];
                s.left = left;
            }
            s.spread = s.left > 0, s.since = 0;
            if (dbg) log("[move] f%llu actor %08x step %.3f %.3f %.3f gap %llu -> %s (left %.4f dt %.4f)",
                         (unsigned long long)f, a, j[0], j[1], j[2], (unsigned long long)gap, s.left > 0 ? "spread" : "kept",
                         g_resume_left, dt);
            s.calm = 0;
        } else {
            s.calm++;
            if (s.left > 0) {
                const float k = std::min(1.0f, dt / s.left);  // the rest comes with the next resume (or here if none)
                for (int i = 0; i < 3; i++) {
                    const float d = s.pend[i] * k;
                    p[i] += d, s.pend[i] -= d;
                }
                s.left = k >= 1 ? 0 : s.left - dt;
            }
        }
    }
    memcpy(GPTR(a + 0xc), p, 12);
    call_unhooked(c, "actor_move_update", {a, c->r[1]});
    std::lock_guard<std::mutex> l(mx);
    if (dbg && atoi(getenv("NX_MOVE_LOG")) == 3) {  // NX_MOVE_LOG=3: actors moved by their own job (move delta), sampled
        const float* q = (const float*)GPTR(a + 0xc);
        const float d2 = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
        static int n = 0;
        if (d2 > 1e-4f && n++ < 4000) log("[movej] f%llu actor %08x moved %.3f dt %.5f", (unsigned long long)g_frame_no.load(), a, sqrtf(d2), g_game_dt);
    }
    memcpy(moves[a].post, GPTR(a + 0xc), 12);
    static const bool trace = dbg && atoi(getenv("NX_MOVE_LOG")) >= 2;  // NX_MOVE_LOG=2: every frame's shown position
    if (trace && moves[a].last_jump) {
        const float* q = moves[a].post;
        log("[movef] f%llu actor %08x pos %.4f %.4f %.4f dt %.5f", (unsigned long long)g_frame_no.load(), a, q[0], q[1], q[2], g_game_dt);
    }
}

// Town traffic and crowd (USER 11-13, Shibuya at 240 fps: cars and walking NPCs far too fast). The car update 0x3df4fc
// eases its speed, moves pos += speed x dir and steers once per call, with no dt; the crowd manager 0x3d6e2c spawns one
// walker and sets each walker's step (0.441 units, applied by its actor job) per call. Both were made for the Switch's
// 30 Hz field frames and ran every frame here (cars ~130 units/s instead of ~38, walkers ~44 instead of 13). Above 30 fps
// they run on 30 Hz ticks only (per object); cars are blended here, walkers in actor_move_update (smooth, Switch speed).
// NX_TOWN30=0: off (debug). NX_TOWN_LOG=N: log N car calls (shown position per call).
static bool town_gate() {
    static const bool on = !getenv("NX_TOWN30") || atoi(getenv("NX_TOWN30")) != 0;
    return on && fps_setting() != 30;
}
static bool town_tick_due(uint32_t obj) {
    static std::mutex m;
    static std::unordered_map<uint32_t, uint32_t> ticks;
    std::lock_guard<std::mutex> l(m);
    return tick_on(ticks, g_tick30, obj);
}
static void town_log(const char* kind, uint32_t obj, uint32_t pos, bool ran) {
    static const int n = getenv("NX_TOWN_LOG") ? atoi(getenv("NX_TOWN_LOG")) : 0;
    static std::atomic<int> k{0};
    if (!n || k >= n) return;
    k++;
    float p[3];
    memcpy(p, GPTR(pos), 12);
    log("[town] f%llu %s %08x pos %.3f %.3f %.3f dt %.5f %s", (unsigned long long)g_frame_no.load(), kind, obj, p[0], p[1], p[2],
        g_game_dt, ran ? "ran" : "skipped");
}
// Cars: the update runs on 30 Hz ticks from the car's true position; its actor_set_pos / actor_set_dir mark the actor,
// so its job goes through town_actor_update like a walker: a visible car's move delta (set by 0x3deb84, applied by the
// job) applies once per tick, a culled car's own position step is taken as the new true position, and the shown
// position glides between them. (s74 blended here, before the job: the job then added the delta on top of the blend
// and the next frame pulled it back = cars shaking back and forth, USER 11-14 video.)
HLE_FN(vehicle_update, "vehicle_update") {
    const uint32_t v = c->r[0], actor = ld32(v + 0xb4);
    static const bool crowd_log = getenv("NX_CROWD_LOG") != nullptr;
    if (crowd_log) {  // debug: distinct cars updated per ~5 s
        static std::unordered_set<uint32_t> seen;
        static uint64_t f0 = 0;
        seen.insert(v);
        if (g_frame_no - f0 >= 300) log("[crowd] cars %zu", seen.size()), seen.clear(), f0 = g_frame_no;
    }
    if (!town_gate() || !actor) { call_unhooked(c, "vehicle_update", {v}); town_log("car", actor, v + 4, true); return; }
    if (!town_tick_due(v)) { town_log("car", actor, actor + 0xc, false); return; }
    town_restore_true(actor);  // the update reads the actor position and yaw as its own
    t_in_mob_mgr = true;
    call_unhooked(c, "vehicle_update", {v});
    t_in_mob_mgr = false;
    town_log("car", actor, actor + 0xc, true);
}
HLE_FN(mob_mgr_update, "mob_mgr_update") {
    const uint32_t m = c->r[0];
    static const bool crowd_log = getenv("NX_CROWD_LOG") != nullptr;
    if (crowd_log && g_frame_no % 300 == 0) {  // debug: crowd manager counts every ~5 s
        uint32_t active = 0;
        for (uint32_t i = 0, n = ld32(m + 0x10); i < n && i < 512; i++) active += ld32(ld32(ld32(m + 0x18) + 4 * i)) != 0;
        log("[crowd] mgr %08x actors %u slots %u (active %u) walkers %u cap %u pool %u", m, ld32(m + 4), ld32(m + 0x10), active,
            ld32(m + 0x1c), ld32(m + 0xbc), ld32(m + 0xc0));
    }
    static const bool mob_on = !getenv("NX_MOB30") || atoi(getenv("NX_MOB30")) != 0;  // debug: crowd gate only
    const bool run = !town_gate() || !mob_on || town_tick_due(m);
    if (!run) return;
    t_in_mob_mgr = town_gate();
    call_unhooked(c, "mob_mgr_update", {m});
    t_in_mob_mgr = false;
}

// Crowd density (setting crowd_density: 0 the game's, 1 half, 2 low; USER 11-27 for weak PCs, issue #11 Shibuya crosswalk).
// Town areas build two pools when they load: the crowd manager makes params[5] full walker models (mgr +0xc0; the nearest
// walkers get them, the rest of the ~300 walkers are the cheap far crowd) and the traffic manager makes params[6] cars
// (+0x1c, in the same frame). Shibuya: 50 + 50; those characters were ~1/3 of the crosswalk frame's draws (130 scene +
// 186 shadow map). The counts are scaled in the constructors' parameters, so the game builds the smaller pools itself and
// everything else stays its own logic; a change applies at the next area load.
static int crowd_density() {
    const char* e = getenv("NX_CROWD_DENSITY");  // read at each area load: the F1 menu's save_setting updates it
    return e ? std::clamp(atoi(e), 0, 2) : 0;
}
static uint32_t crowd_scaled(uint32_t n) { const int d = crowd_density(); return d ? std::max<uint32_t>(4, d == 1 ? n / 2 : n / 4) : n; }
HLE_FN(mob_mgr_ctor, "mob_mgr_ctor") {
    const uint32_t p = c->r[1], n = ld32(p + 5 * 4), k = n >= 4 && n <= 512 ? crowd_scaled(n) : n;
    if (k != n) st32(p + 5 * 4, k), log("crowd density: walker models %u -> %u", n, k);
    // walker slots (params[4], Shibuya 300: the far crowd and the crowd logic) shrink with the models; 75 measured stable
    const uint32_t cap = ld32(p + 4 * 4), cap_k = cap >= 4 && cap <= 4096 ? std::max(crowd_scaled(cap), k) : cap;
    if (cap_k != cap) st32(p + 4 * 4, cap_k), log("crowd density: walker slots %u -> %u", cap, cap_k);
    const uint32_t r = call_unhooked(c, "mob_mgr_ctor", {c->r[0], p});
    if (k != n) st32(p + 5 * 4, n);  // the caller's parameter block as it was
    if (cap_k != cap) st32(p + 4 * 4, cap);
    c->r[0] = r;  // returns the manager
}
HLE_FN(traffic_mgr_create, "traffic_mgr_create") {
    const uint32_t p = c->r[0], n = ld32(p + 6 * 4), k = n >= 4 && n <= 512 ? crowd_scaled(n) : n;
    if (k != n) st32(p + 6 * 4, k), log("crowd density: cars %u -> %u", n, k);
    call_unhooked(c, "traffic_mgr_create", {p});
    if (k != n) st32(p + 6 * 4, n);
}

// Character action blends (_chrPlayAction 0x41ebfc: blend seconds x 30 -> 0x3ce298 sets step +0x11fc = 1 / frames):
// chr_motion_update adds the step to the weight +0x11f8 once per call, i.e. per frame, so a 0.5 s walk -> stand blend
// took 0.25 s at 60 fps and 1/16 s at 240 (user 11-03: Itsuki's pose snaps in the save-3 cutscene). Step x dt*30.
// Measured at 30 fps: walk -> stand 8 frames (step 0.125) = 0.23-0.27 s.
// NX_BLEND_LOG=1: log each blend's start and end (weight +0x11f8 below 1 -> 1).
static void blend_log(uint32_t ch, bool before) {
    static const bool on = getenv("NX_BLEND_LOG") != nullptr;
    if (!on) return;
    static std::unordered_map<uint32_t, bool> blending;
    float w;
    const uint32_t wb = ld32(ch + 0x11f8);
    memcpy(&w, &wb, 4);
    bool& b = blending[ch];
    if (before && w < 1 && !b) b = true, log("[blend] chr %08x start: weight %.3f action %d <- %d", ch, w, (int)ld32(ch + 0x11f0), (int)ld32(ch + 0x11f4));
    if (!before && w >= 1 && b) b = false, log("[blend] chr %08x done", ch);
}
// Turn actions (Lua chrRotTo -> ACTION_ID_TURN_LEFT / RIGHT 0x83 / 0x84): the script thread _chrActorRotThread calls
// _chrActorRot (0x41d28c) on each resume; it sets the yaw from the turn animation's frame and ends the turn (idle 0x80)
// only when it sees that frame at its end. The animation loops and sits on its end frame for one update: at 30 fps every
// resume sees it, above 30 fps scripts resume on 30 Hz ticks and miss it, so the turn loops forever (user 11-08 video,
// save 3 "That must be it.": Itsuki snaps to the side and turns back every 1.3 s; 30 fps: action 132 -> 128 after 1 s).
// Fix: a turn animation on its end frame gets no motion update for up to 0.1 s of game time (pose stays), so the next
// resume sees the end (even with speed 0 the update chains to the action's next clip: 60-frame turn, then a 160-frame one). A wrap without an end frame (frame went down) puts the frame on the end. NX_TURN_LOG=1: log.
// Frame record = {current, start, end} floats, found the way _chrActorRot does (0x5b7588, 0x3a5548, 0x555734).
static uint32_t turn_frames(uint32_t ch) {
    const uint32_t id = ld32(ch + 0x11f0);
    if (id != 0x83 && id != 0x84) return 0;
    const uint32_t act = ld32(ch + 0x530 + id * 4 + 8), list = ld32(ch + 0x11c8);
    if (!act || !list) return 0;
    const uint32_t key = ld32(act + 0x1c8);
    uint32_t n = ld32(list + 0xe4);
    for (int i = 0; n && i < 512 && ld32(n + 8) != key; i++) n = ld32(n + 4);
    if (!key || !n || ld32(n + 8) != key || !ld32(n + 0xc)) return 0;
    const uint32_t x = ld32(ld32(n + 0xc));
    if (!x) return 0;
    if (ld32(x + 0x44)) return ld32(ld32(x + 0x44) + 0x10);
    if (ld32(x + 0x48)) return ld32(ld32(x + 0x48) + 0x14);
    if ((int32_t)ld32(x + 0x4c) > 0) return ld32(ld32(x + 0x50) + 0x10);
    if ((int32_t)ld32(x + 0x70) > 0) return ld32(ld32(x + 0x74) + 0xc);
    return 0;
}
HLE_FN(chr_motion_update, "chr_motion_update") {
    const uint32_t ch = c->r[0];
    float lit;  // s0 is a speed factor (1); the frame step is the dt literal at 0x3ce588 (1/30 rewritten per frame)
    const uint32_t lit_bits = ld32(0x3ce588);
    memcpy(&lit, &lit_bits, 4);
    const float dt = c->vr.f[0] * lit;
    blend_log(ch, true);
    if (getenv("NX_BLEND_LOG") && ld32(ch + 0x11f8) != 0x3f800000) log("[blend] chr %08x dt %.5f step %08x", ch, dt, ld32(ch + 0x11fc));
    struct After { uint32_t ch; ~After() { blend_log(ch, false); } } after{ch};
    if (fps_setting() == 30 || dt <= 0) { call_unhooked(c, "chr_motion_update", {ch}); return; }
    static std::unordered_map<uint32_t, float> held;  // per chr: game time its turn animation has been held at the end
    static std::unordered_map<uint32_t, float> rate;  // per chr: turn frames per game second, from the last update
    static std::mutex held_mx;                        // actor jobs may run on several threads; never held over guest calls
    static const int turn_log = getenv("NX_TURN_LOG") ? atoi(getenv("NX_TURN_LOG")) : 0;
    auto rf = [](uint32_t a) { float v; const uint32_t w = ld32(a); memcpy(&v, &w, 4); return v; };
    const uint32_t tf = turn_frames(ch);
    const float cur0 = tf ? rf(tf) : 0;
    if (turn_log >= 2 && (ld32(ch + 0x11f0) == 0x83 || ld32(ch + 0x11f0) == 0x84))  // NX_TURN_LOG=2: every turn update
        log("[turn] chr %08x action %u frames %08x cur %.2f start %.2f end %.2f dt %.4f", ch, ld32(ch + 0x11f0), tf, cur0, tf ? rf(tf + 4) : 0, tf ? rf(tf + 8) : 0, dt);
    bool can_hold = false, skip = false;
    {
        std::lock_guard<std::mutex> l(held_mx);
        if (!tf) held.erase(ch), rate.erase(ch);
        else if ((can_hold = held[ch] < 0.1f) && (cur0 >= rf(tf + 8) || (rate.count(ch) && cur0 + rate[ch] * dt >= rf(tf + 8)))) {
            // this step would cross the end and chain to the next clip in one update (user 11-20, m1029_01 Tsubasa at
            // 240: 59.95 -> next clip, never held): put the frame on the end and hold like an exact end frame
            st32(tf, ld32(tf + 8));
            if (turn_log && held[ch] == 0) log("[turn] chr %08x action %u held at end %.2f", ch, ld32(ch + 0x11f0), cur0);
            held[ch] += dt;
            skip = true;  // no update: even with speed 0 the action chains to its next clip at once (measured 11-08)
        }
    }
    if (skip) return;
    const uint32_t step_bits = ld32(ch + 0x11fc);
    float step;
    memcpy(&step, &step_bits, 4);
    const float scaled = step * dt * 30;
    memcpy(GPTR(ch + 0x11fc), &scaled, 4);
    call_unhooked(c, "chr_motion_update", {ch});
    uint32_t scaled_bits;
    memcpy(&scaled_bits, &scaled, 4);
    if (ld32(ch + 0x11fc) == scaled_bits) st32(ch + 0x11fc, step_bits);  // else a new blend was set inside: keep it
    if (tf && turn_frames(ch) == tf && rf(tf) > cur0) {
        std::lock_guard<std::mutex> l(held_mx);
        rate[ch] = (rf(tf) - cur0) / dt;
    }
    if (can_hold && turn_frames(ch) == tf && rf(tf) < cur0 - 1e-3f) {  // wrapped past the end: put it there
        if (turn_log) log("[turn] chr %08x wrapped %.2f -> %.2f, set end %.2f", ch, cur0, rf(tf), rf(tf + 8));
        st32(tf, ld32(tf + 8));
    }
}

// Field enemies (symbol AI, ScriptCommon/Map/Enemy/EnemyServerParam.lua) move a fixed distance per frame and count
// frames (waits, turns, stun, knock-back, repop cooldowns), read through enemy_param_get at state changes. Keep the
// loaded values and serve them scaled for this frame's dt: speeds x dt*30, frame counts / (dt*30). Offsets from the
// loader 0x42edc0 (tools: ghidra/parammap.py).
static const uint32_t ENEMY_SPEEDS[] = {0x3c, 0x4c, 0x50, 0x54, 0x9c, 0xa4, 0xc0};  // MoveSpeed_*, Blown/KnockBack speeds
static const uint32_t ENEMY_FRAMES_INT[] = {0x68, 0x6c, 0x70, 0x74, 0x78, 0x84, 0x8c, 0x94, 0xa0, 0xa8, 0xb4,
                                            0xb8, 0xbc, 0xc4, 0xc8, 0xe4, 0xe8, 0xf4, 0x150, 0x154};
static const uint32_t ENEMY_FRAMES_FLOAT[] = {0x138, 0x168};  // RoundPopAreaCoolDown, SUROURI_DeactivateCountDown
static uint8_t g_enemy_params[0x18c];
static uint32_t g_enemy_table = 0;  // guest address of the table once loaded
static float g_enemy_dt = 0;        // dt the table is scaled for

// Hide field enemies (setting hide_field_enemies): symbols appear through this table too, not only through
// field_enemy_spawn (user 10-31: still spawned while exploring). Wild symbols: AddSymbol_NormalPercentA..D 0 and the
// karma thresholds out of reach; placed symbols: FixSymbol_AppearDistance 0 (they appear inside that distance).
// Written on every read so the F1 toggle works live; off restores the loaded values.
static void enemy_hide_apply(uint32_t t) {
    static const uint32_t ZERO[] = {0x10c, 0x110, 0x114, 0x118, 0x140}, KARMA[] = {0x130, 0x13c};
    const char* e = getenv("NX_HIDE_FIELD_ENEMIES");
    const bool on = e && atoi(e);
    static bool was = false;
    if (!on && !was) return;
    for (uint32_t o : ZERO) memcpy(GPTR(t + o), on ? "    " : (const char*)g_enemy_params + o, 4);
    for (uint32_t o : KARMA) {
        uint32_t v = 0x7fffffff;
        if (!on) memcpy(&v, g_enemy_params + o, 4);
        st32(t + o, v);
    }
    if (on != was) log("field: hide field enemies %s", on ? "on" : "off");
    was = on;
}

HLE_FN(enemy_param_store, "enemy_param_store") {
    call_unhooked(c, "enemy_param_store", {c->r[0]});
    if (uint32_t t = call_unhooked(c, "enemy_param_get", {})) {  // new values (map load): keep them unscaled
        memcpy(g_enemy_params, GPTR(t), sizeof g_enemy_params);
        g_enemy_table = t;
        g_enemy_dt = 0;
        enemy_hide_apply(t);  // the game keeps the table pointer: map loads may never call enemy_param_get again
    }
}

HLE_FN(enemy_param_get, "enemy_param_get") {
    uint32_t t = call_unhooked(c, "enemy_param_get", {});
    c->r[0] = t;
    if (!t) return;
    if (t != g_enemy_table) { memcpy(g_enemy_params, GPTR(t), sizeof g_enemy_params); g_enemy_table = t; g_enemy_dt = 0; }
    enemy_hide_apply(t);
    if (fps_setting() == 30 || g_enemy_dt == g_game_dt) return;
    float k = g_game_dt * 30;
    static bool logged = false;
    if (!logged) log("frame step: field enemy speeds x%.3f, frame counts x%.3f", k, 1 / k), logged = true;
    for (uint32_t o : ENEMY_SPEEDS) {
        float v;
        memcpy(&v, g_enemy_params + o, 4);
        v *= k;
        memcpy(GPTR(t + o), &v, 4);
    }
    for (uint32_t o : ENEMY_FRAMES_INT) {
        int32_t v;
        memcpy(&v, g_enemy_params + o, 4);
        st32(t + o, (uint32_t)(int32_t)lroundf(v / k));
    }
    for (uint32_t o : ENEMY_FRAMES_FLOAT) {
        float v;
        memcpy(&v, g_enemy_params + o, 4);
        v /= k;
        memcpy(GPTR(t + o), &v, 4);
    }
    g_enemy_dt = g_game_dt;
}

// Screen fades (FadeIn/FadeOut, map changes) advance by 1/frames per update, frames at 30 Hz. The manager holds two
// fades (+4, +8), each with its per-update rate at +0x48: scale the rates by dt*30 for this update only.
HLE_FN(fade_update, "fade_update") {
    uint32_t m = c->r[0], rate[2] = {ld32(m + 4) + 0x48, ld32(m + 8) + 0x48};
    float saved[2];
    for (int i = 0; i < 2; i++) {
        memcpy(&saved[i], GPTR(rate[i]), 4);
        float r = saved[i] * g_game_dt * 30;
        memcpy(GPTR(rate[i]), &r, 4);
    }
    c->r[0] = call_unhooked(c, "fade_update", {m});
    for (int i = 0; i < 2; i++) memcpy(GPTR(rate[i]), &saved[i], 4);
    // log each fade start / end (fade object +0x40 state 1-2 = running, +0x20 target colour, +0x48 rate): rare, keeps the
    // order of screen fades visible in user logs (boss death white, s71)
    static uint32_t last[2][3];
    for (int i = 0; i < 2; i++) {
        const uint32_t f = ld32(m + 4 + 4 * i), now[3] = {ld32(f + 0x40), ld32(f + 0x2c), ld32(f + 0x48)};
        if (memcmp(now, last[i], sizeof now)) {
            float t, a, r;
            memcpy(&t, GPTR(f + 0x44), 4), memcpy(&a, &now[1], 4), memcpy(&r, &now[2], 4);
            log("fade %d: state %u t %.3f to alpha %.2f rate %.4f/30Hz", i, now[0], t, a, r);
            memcpy(last[i], now, sizeof now);
        }
    }
}

// The animation manager advances float(present interval) 60 Hz frames per update (0x5c0508: step x speed at +0x98).
// With a variable frame time the step must be dt*60 (the interval is 1 then): scale the speed for this update only.
HLE_FN(anim_update, "anim_update") {
    const bool variable = fps_setting() == 0 || fps_setting() > 60;
    uint32_t speed = c->r[0] + 0x98;
    float saved, k = variable || fps_setting() == 60 ? g_game_dt * 60 : 1;  // 60: 1, or k in fast battles
    memcpy(&saved, GPTR(speed), 4);
    float s = saved * k;
    memcpy(GPTR(speed), &s, 4);
    call_unhooked(c, "anim_update", {c->r[0]});
    memcpy(GPTR(speed), &saved, 4);
}

// Material anims (texture SRT flipbooks, colour curves, UV scrolls) were authored for the Switch's 30 Hz updates: frames
// advance by 2 per update from the start, so the game only ever evaluates even frames. Above 30 fps the curves are
// sampled in between: the battle stands' crowd (fs aa584530) is a 4-cell flipbook (V offset 0 / .25 / .5 / .75) with
// cubic steps between keys 2 frames apart, so in-between frames show half cells = 1-frame flashes (reddit 11-05).
// Per float curve (anim_curve_float): between two Switch frames (g, g + 2) a curve that JUMPS there (change > 2x the
// change of the windows before and after: flipbook steps, wraps) or holds (same value at both) keeps the Switch value
// at g; every other curve (scrolls such as waterfalls, colour pulses) is evaluated at the true frame: still smooth. Speed is unchanged (the controller's
// frame is never touched). NX_MAT_GRID=0 turns it off (debug); NX_MAT_LOG=N (+ NX_MAT_FROM=frame) logs N evaluations.
static thread_local bool t_mat_on = false;
static thread_local float t_mat_start = 0;
HLE_FN(mat_anim_eval, "mat_anim_eval") {
    static const bool on = !getenv("NX_MAT_GRID") || atoi(getenv("NX_MAT_GRID")) != 0;
    uint32_t obj = c->r[0], ctrl = ld32(obj + 4);
    float f, s;
    memcpy(&f, GPTR(ctrl), 4);
    memcpy(&s, GPTR(ctrl + 4), 4);
    static const int mlog = getenv("NX_MAT_LOG") ? atoi(getenv("NX_MAT_LOG")) : 0;  // debug: log N calls
    static std::atomic<int> nlog{0};
    static const unsigned long long mfrom = getenv("NX_MAT_FROM") ? atoll(getenv("NX_MAT_FROM")) : 0;  // first frame logged
    if (mlog && g_frame_no.load() >= mfrom && nlog++ < mlog) {
        float e;
        memcpy(&e, GPTR(ctrl + 8), 4);
        log("[mat] f%llu obj %08x mats %u frame %.3f start %.3f end %.3f from %s", (unsigned long long)g_frame_no.load(),
            obj, (unsigned)(ld32(obj + 0x40) & 0xffff), f, s, e, symbol_at(c->r[14]));
    }
    t_mat_on = on && fps_setting() != 30;
    t_mat_start = s;
    call_unhooked(c, "mat_anim_eval", {obj});
    t_mat_on = false;
}
HLE_FN(anim_curve_float, "anim_curve_float") {
    const uint32_t curve = c->r[0], cache = c->r[1];
    const float f = c->vr.f[0];
    auto eval = [&](float x) {
        c->vr.f[0] = x;
        call_unhooked(c, "anim_curve_float", {curve, cache});
        return c->vr.f[0];
    };
    const float g = t_mat_start + floorf((f - t_mat_start) * 0.5f + 1e-3f) * 2;  // the Switch frame at or before f
    if (!t_mat_on || g == f || !(g >= t_mat_start)) { eval(f); return; }
    // the test once per curve and Switch window (4 evaluations), reused by the other frames of the window (8 at 240 fps)
    struct Win { float g, start; uint32_t sig; float a; bool jump; };
    static thread_local std::unordered_map<uint32_t, Win> wins;
    const uint32_t sig = ld32(curve + 0x18) ^ ld32(curve + 0x1c) * 31 ^ ld32(curve + 0x20) * 17 ^ ld32(curve + 0x24) * 7;
    Win& w = wins.size() < 65536 ? wins[curve] : (wins.clear(), wins[curve]);
    float p = 0, b = 0, n = 0;
    if (w.g != g || w.start != t_mat_start || w.sig != sig) {
        p = eval(g - 2), w.a = eval(g), b = eval(g + 2), n = eval(g + 4);
        // a hold (same value at both Switch frames) stays flat too: in between it only has the overshoot of a cubic step
        w.jump = fabsf(b - w.a) <= 1e-6f || fabsf(b - w.a) > 2 * std::max(fabsf(w.a - p), fabsf(n - b)) + 1e-6f;
        w.g = g, w.start = t_mat_start, w.sig = sig;
    }
    const float a = w.a;
    const bool jump = w.jump;
    c->vr.f[0] = jump ? a : eval(f);  // f last: the key cache ends at the true frame
    static const int clog = getenv("NX_CURVE_LOG") ? atoi(getenv("NX_CURVE_LOG")) : 0;  // debug: N smooth-path samples
    static std::atomic<int> ncl{0};
    if (clog && !jump && fabsf(c->vr.f[0] - a) > 0.05f && ncl++ < clog)
        log("[curve] %08x f %.3f g %.3f: p %.4f a %.4f b %.4f n %.4f -> %.4f", curve, f, g, p, a, b, n, c->vr.f[0]);
}

// NX_ANIM_PROBE=1: log the code that advances g3d animation frames. Every frame controller step goes through a play
// policy (s0 = frame + step, s1 start, s2 end); the policy's lr is the stepping code (debug for 60 fps animation speed).
static void anim_policy_probe(Cpu* c, const char* kind) {
    static const bool on = getenv("NX_ANIM_PROBE") != nullptr;
    if (!on) return;
    static std::mutex m;
    static std::unordered_map<uint32_t, uint32_t> n;  // calls per caller
    std::lock_guard<std::mutex> l(m);
    uint32_t k = ++n[c->r[14]];
    if (k == 1 || k == 100 || k == 10000)
        log("[anim] %s from %s x%u: frame %.3f start %.3f end %.3f", kind, symbol_at(c->r[14]), k, c->vr.f[0], c->vr.f[1],
            c->vr.f[2]);
}
// Loops of 2 frames or less (start 0, end 1: the giant dress of the 2nd idolasphere, s86): the Switch steps 2 frames per
// update, so it only ever shows the loop's start. Above 30 fps the frame sweeps start..end and the skeleton blends towards
// the end key (another pose: the dress arm flailed at 240 fps). Such loops stay at their start. NX_ANIM_LOOP_PIN=0 off.
HLE_FN(anim_policy_loop, "anim_policy_loop") {
    static const bool pin = !getenv("NX_ANIM_LOOP_PIN") || atoi(getenv("NX_ANIM_LOOP_PIN")) != 0;
    const float start = c->vr.f[1], end = c->vr.f[2];
    call_unhooked(c, "anim_policy_loop", {});
    if (pin && end > start && end - start <= 2.0f) c->vr.f[0] = start;
    c->vr.f[1] = start, c->vr.f[2] = end;  // for the probes below (the policy clobbers s2)
    static const bool shortlog = getenv("NX_ANIM_SHORT") != nullptr;  // debug: distinct short loops (caller, start, end)
    if (shortlog && c->vr.f[2] - c->vr.f[1] <= 8) {
        static std::mutex m;
        static std::set<std::tuple<uint32_t, float, float>> seen;
        std::lock_guard<std::mutex> l(m);
        if (seen.size() < 512 && seen.insert({c->r[14], c->vr.f[1], c->vr.f[2]}).second)
            log("[anim] short loop from %s: frame %.3f start %.3f end %.3f", symbol_at(c->r[14]), c->vr.f[0], c->vr.f[1], c->vr.f[2]);
    }
}
PROBE_FN(anim_policy_once, "anim_policy_once") { anim_policy_probe(c, "once"); }

// NX_ANIM_PROBE=1 also logs each distinct dt (s0, seconds) a caller passes to the model animation updates.
static void anim_dt_probe(Cpu* c, const char* kind) {
    static const bool on = getenv("NX_ANIM_PROBE") != nullptr;
    if (!on) return;
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;  // (caller, dt bits)
    std::lock_guard<std::mutex> l(m);
    if (seen.size() < 4096 && seen.insert({c->r[14], c->vr.w[0]}).second)
        log("[anim] %s dt %.6f from %s", kind, c->vr.f[0], symbol_at(c->r[14]));
}
PROBE_FN(model_obj_update, "model_obj_update") {  // NX_MAT_LOG=N: log N calls (debug, crowd flipbook speed)
    static const int mlog = getenv("NX_MAT_LOG") ? atoi(getenv("NX_MAT_LOG")) : 0;
    static std::atomic<int> nlog{0};
    static const unsigned long long mfrom = getenv("NX_MAT_FROM") ? atoll(getenv("NX_MAT_FROM")) : 0;  // first frame logged
    if (mlog && g_frame_no.load() >= mfrom && nlog++ < mlog) {
        float dt; uint32_t w = ld32(c->r[1] + 0xc); memcpy(&dt, &w, 4);
        log("[objupd] f%llu obj %08x info %08x dt %.6f from %s", (unsigned long long)g_frame_no.load(), c->r[0], c->r[1], dt,
            symbol_at(c->r[14]));
    }
}
PROBE_FN(model_mat_update, "model_mat_update") {  // NX_MAT_LOG=N: log N calls (debug, crowd flashing)
    static const int mlog = getenv("NX_MAT_LOG") ? atoi(getenv("NX_MAT_LOG")) : 0;
    static std::atomic<int> nlog{0};
    static const unsigned long long mfrom = getenv("NX_MAT_FROM") ? atoll(getenv("NX_MAT_FROM")) : 0;  // first frame logged
    if (mlog && g_frame_no.load() >= mfrom && nlog++ < mlog)
        log("[matupd] f%llu model %08x dt %.6f from %s", (unsigned long long)g_frame_no.load(), c->r[0], c->vr.f[0],
            symbol_at(c->r[14]));
}
PROBE_FN(model_anim_update, "model_anim_update") { anim_dt_probe(c, "model"); }
PROBE_FN(model_skel_update, "model_skel_update") { anim_dt_probe(c, "skel"); }
PROBE_FN(action_group_update, "action_group_update") { anim_dt_probe(c, "action group"); }
PROBE_FN(action_update, "action_update") { anim_dt_probe(c, "action"); }
PROBE_FN(cut_update, "cut_update") { anim_dt_probe(c, "cut"); }
PROBE_FN(phys_step, "phys_step") { anim_dt_probe(c, "physics step"); }
PROBE_FN(phys_update, "phys_update") {
    anim_dt_probe(c, "physics");
    // NX_PHYS_LOG=1: every call (debug for the ribbon / hair chains): frame, object, dt, timescale +0x120, chains +0x124
    static const bool on = getenv("NX_PHYS_LOG") != nullptr;
    if (on) {
        uint32_t o = c->r[0];
        float ts; uint32_t w = ld32(o + 0x120); memcpy(&ts, &w, 4);
        log("[phys] f%llu obj %08x dt %.6f ts %.3f chains %d iter %d from %s", (unsigned long long)g_frame_no.load(), o,
            c->vr.f[0], ts, (int)ld32(o + 0x124), (int)ld32(o + 0x10), symbol_at(c->r[14]));
        // NX_PHYS_DUMP=<file>: per call, u32 frame + u32 chains + chains x 0x150 bytes of the chain array (+0x134)
        static FILE* dump = getenv("NX_PHYS_DUMP") ? fopen(getenv("NX_PHYS_DUMP"), "wb") : nullptr;
        if (dump) {
            uint32_t n = std::min<uint32_t>(ld32(o + 0x124), 256), fr = (uint32_t)g_frame_no.load();
            fwrite(&fr, 4, 1, dump); fwrite(&n, 4, 1, dump);
            fwrite(GPTR(ld32(o + 0x134)), 0x150, n, dump);
            fflush(dump);
        }
    }
}
// Resolved update function of a scheduler task (member-function tasks: wrapper 0x3134f4 and identical copies).
static uint32_t task_update_fn(uint32_t t) {
    uint32_t fn = ld32(ld32(t) + 0x14);
    bool wrapper = true;
    for (uint32_t i = 0; i < 8 && wrapper; i++) wrapper = ld32(fn + 4 * i) == ld32(0x3134f4 + 4 * i);
    if (wrapper) {
        uint32_t adj = ld32(t + 0x18);
        fn = adj & 1 ? ld32(ld32(ld32(t + 0x10) + ((int32_t)adj >> 1)) + ld32(t + 0x14)) : ld32(t + 0x14);
    }
    return fn;
}
// Debug NX_TASK30=<update fn hex,...> (or control "task30 <fn>", 0 = clear): those tasks update only on 30 Hz ticks (vtable
// swapped to a copy whose update is "return 1" for the phase). To find which task drives a frame-counted effect.
static std::vector<uint32_t> g_task30;
static std::mutex g_task30_lock;
void rt::task30_command(const char* a) {
    std::lock_guard<std::mutex> l(g_task30_lock);
    uint32_t fn = (uint32_t)strtoul(a, nullptr, 16);
    if (!fn) g_task30.clear(); else g_task30.push_back(fn);
    log("task30: %zu functions", g_task30.size());
}
HLE_FN(task_run_phase_gate, "task_run_phase") {
    static bool init = [] {
        if (const char* e = getenv("NX_TASK30")) for (const char* q = e; *q;) { g_task30.push_back((uint32_t)strtoul(q, (char**)&q, 16)); while (*q == ',' || *q == ' ') q++; }
        return true;
    }();
    (void)init;
    std::vector<std::pair<uint32_t, uint32_t>> swapped;  // task, vtable
    static std::unordered_map<uint32_t, uint32_t> fake_of, ticks;
    if (!g_task30.empty() && fps_setting() != 30) {
        std::lock_guard<std::mutex> l(g_task30_lock);
        uint32_t list = ld32(c->r[0] + 4) + c->r[1] * 0xc;
        int n = 0;
        for (uint32_t t = ld32(list); t && n < 400; t = ld32(t + 8), n++) {
            const uint32_t fn = task_update_fn(t);
            if (std::find(g_task30.begin(), g_task30.end(), fn) == g_task30.end() || tick_on(ticks, g_tick_ui30, t)) continue;
            const uint32_t vt = ld32(t);
            uint32_t& fake = fake_of[vt];
            if (!fake) {
                const Cpu keep = *c;
                fake = gcall(c, 0x5a8720, {0x40});
                *c = keep;
                if (!fake) continue;
                memcpy(GPTR(fake), GPTR(vt), 0x40);
                st32(fake + 0x14, 0x201d24);
            }
            st32(t, fake), swapped.push_back({t, vt});
        }
    }
    const uint32_t sched = c->r[0], phase = c->r[1];
    call_unhooked(c, "task_run_phase", {sched, phase});
    for (auto& [t, vt] : swapped) if (ld32(t) == fake_of[vt]) st32(t, vt);
}
PROBE_FN(task_run_phase, "task_run_phase") {  // NX_ANIM_PROBE: which tasks run in each phase (debug)
    static const bool on = getenv("NX_ANIM_PROBE") != nullptr;
    if (!on) return;
    static std::set<std::pair<uint32_t, uint32_t>> seen;  // (phase, vtable or bound member function)
    uint32_t list = ld32(c->r[0] + 4) + c->r[1] * 0xc;
    int n = 0;
    for (uint32_t t = ld32(list); t && n < 200; t = ld32(t + 8), n++) {
        const uint32_t fn = task_update_fn(t);  // member-function task: object +0x10, function +0x14, adjust +0x18 (bit 0: virtual)
        if (seen.insert({c->r[1], fn}).second)
            log("[task] phase %u sched %08x task %08x vt %08x update %s", c->r[1], c->r[0], t, ld32(t), symbol_at(fn));
    }
}

HLE_FN(gfx_set_present_interval, "gfx_set_present_interval") {
    const int fps = fps_setting();
    uint32_t n = fps == 30 ? c->r[1] : 1;
    static uint32_t last = ~0u;
    if (n != last) {
        log("frame step: game asks for interval %u, using %u (NX_FPS %d)", c->r[1], n, fps);
        if (fps == 60) set_game_dt(1 / 60.0f);  // at 30 the game's own interval changes keep its 1/30 dt (loading)
        else if (fps == 30 && walk_speed() != 1) set_game_dt(1 / 30.0f);  // only for the walk literals
        last = n;
    }
    st32(c->r[0] + 0x58c0, n);
}

// ------------------------------------------------------------------------------------------------ Lua script log
// Game scripts (Lua 5.2) print through AtPrintf/print/AtConsoleOut/AtTraceOut, all empty in retail; script errors go
// to script_error, which also drops its text. We log them as [lua] / [lua error]. NX_LUA_LOG=0 turns the prints off.
// Lua C API inside main (found from the binding wrappers at 0x5adc48 / 0x5addc8):
static const uint32_t LUA_GETTOP = 0x529964, LUA_TYPE = 0x529f88, LUA_TOLSTRING = 0x52aa6c;

static std::string lua_args(Cpu* c, uint32_t L) {
    std::string s;
    uint32_t n = gcall(c, LUA_GETTOP, {L});
    for (uint32_t i = 1; i <= n && i <= 16; i++) {
        uint32_t t = gcall(c, LUA_TYPE, {L, i});
        if (i > 1) s += ' ';
        if (t == 3 || t == 4) {  // number, string
            uint32_t p = gcall(c, LUA_TOLSTRING, {L, i, 0});
            s += p ? gstr(p, 512) : "?";
        } else {
            s += t == 0 ? "nil" : t == 1 ? "<bool>" : "<" + std::to_string(t) + ">";
        }
    }
    return s;
}
static void lua_print(Cpu* c, const char* tag) {
    static const bool off = getenv("NX_LUA_LOG") && !strcmp(getenv("NX_LUA_LOG"), "0");
    if (!off) log("[lua%s] %s", tag, lua_args(c, c->r[0]).c_str());
    c->r[0] = 0;
}
HLE_FN(lua_AtPrintf, "lua_AtPrintf") { lua_print(c, ""); }
HLE_FN(lua_AtConsoleOut, "lua_AtConsoleOut") { lua_print(c, " console"); }
HLE_FN(lua_AtTraceOut, "lua_AtTraceOut") { lua_print(c, " trace"); }
PROBE_FN(script_error, "script_error") {
    log("[lua error] %s %s: %s", c->r[1] ? gstr(c->r[1], 64).c_str() : "", c->r[2] ? gstr(c->r[2], 128).c_str() : "",
        c->r[3] ? gstr(c->r[3], 512).c_str() : "");
}
PROBE_FN(luaG_errormsg, "luaG_errormsg") {
    Cpu save = *c;  // probes must leave the Cpu as they found it
    uint32_t p = gcall(c, LUA_TOLSTRING, {save.r[0], 0xffffffffu, 0});
    log("[lua error] raised: %s", p ? gstr(p, 512).c_str() : "(not a string)");
    *c = save;
}

// ------------------------------------------------------------------------------------------------ game flags
// Story/system flags are bits at flags+0xa4 (flag_on 0x351c94 / flag_off 0x354674 / is_flag_on 0x353568, behind the
// Lua bindings FlagOn/FlagOff/IsFlagOn); the flags object pointer lives at FLAGS_PTR. Names come from <data>/flags.txt
// ("0x412 FLG_EVM_0009_01" lines, made by tools/lua/flagnames.py from ScriptCommon/Flag/*.lua). Every change is
// logged as "[flag] on/off". NX_CONTROL "flag n" logs a flag, "flag n 0|1" sets it (debug).
static const uint32_t FLAGS_PTR = 0x9ef37c;
static std::string flag_name(uint32_t id) {
    static const std::unordered_map<uint32_t, std::string> names = [] {
        std::unordered_map<uint32_t, std::string> m;
        if (FILE* f = fopen((g_data_dir + "/flags.txt").c_str(), "r")) {
            char line[256], name[200];
            unsigned v;
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "%x %199s", &v, name) == 2) m.emplace(v, name);
            fclose(f);
        }
        return m;
    }();
    auto it = names.find(id);
    char buf[16];
    snprintf(buf, sizeof buf, "%#x", id);
    return it == names.end() ? std::string(buf) : std::string(buf) + " " + it->second;
}
static uint32_t flag_byte(uint32_t id) {
    uint32_t obj = ld32(ld32(FLAGS_PTR));
    return obj ? obj + 0xa4 + (id >> 3) : 0;
}
static int flag_get(uint32_t id) {
    uint32_t a = flag_byte(id);
    return a ? (*(uint8_t*)GPTR(a) >> (id & 7)) & 1 : -1;
}
PROBE_FN(flag_on, "flag_on") { if (flag_get(c->r[0]) == 0) log("[flag] on %s", flag_name(c->r[0]).c_str()); }
PROBE_FN(flag_off, "flag_off") { if (flag_get(c->r[0]) == 1) log("[flag] off %s", flag_name(c->r[0]).c_str()); }
// New Game+ requests in a fresh game (setting ng_plus_requests, default 1; user request s116): only two requests are
// NG+ only, both made available by ScriptCommon/Map/Gimmick/GimmickQuestStarter.lua (Quest_Start) behind
// IsFlagOn(FLG_EVM_LOOP_START): No. 49 "Versus Grandmaster Mirage!" (quest 61: chapter 1 + NG+) and No. 50 "The Dragon
// Descends" (quest 73: NG+, quest 52 "Path of the Strong" done, map count d008_ShardTradeTime >= 36). Their NPCs and
// scripts only check the *_AVAILABLE flags (Map_NpcEventTable, GimmickInitializeScript, mapNPC_d008_02), so the same
// rules without the NG+ check set those flags here, twice a second. Map counts: GetMapCount (0x430f2c) ->
// 0x354c00: flags object + 0x34fc0 + 4 * id.
static void ng_plus_requests_poll() {
    if (g_frame_no % 30) return;
    static const bool on = !getenv("NX_NG_PLUS_REQUESTS") || atoi(getenv("NX_NG_PLUS_REQUESTS")) != 0;
    if (!on) return;
    const uint32_t obj = ld32(ld32(FLAGS_PTR));
    if (!obj || mem_query(obj).state == MS_FREE) return;
    auto set = [](uint32_t id, const char* what) {
        const uint32_t a = flag_byte(id);
        *(uint8_t*)GPTR(a) |= 1u << (id & 7);
        log("[flag] on %s (ng_plus_requests: %s)", flag_name(id).c_str(), what);
    };
    if (flag_get(0x91c0) == 0 && flag_get(0x3e01) == 1) set(0x91c0, "request 49 Versus Grandmaster Mirage!");
    if (flag_get(0x9280) == 0 && flag_get(0x913f) == 1 && (int32_t)ld32(obj + 0x34fc0 + 4 * 0x1e) >= 36)
        set(0x9280, "request 50 The Dragon Descends");
}
int rt::game_flag(uint32_t id) { return flag_get(id); }
void rt::flag_command(const char* args) {
    unsigned id = 0;
    int v = -1;
    if (sscanf(args, "%i %d", (int*)&id, &v) < 1) return;
    uint32_t a = flag_byte(id);
    if (!a) { log("[flag] flags not loaded yet"); return; }
    if (v == 0 || v == 1) {
        uint8_t* p = (uint8_t*)GPTR(a);
        *p = v ? *p | (1u << (id & 7)) : *p & ~(1u << (id & 7));
        log("[flag] set %s = %d (control)", flag_name(id).c_str(), v);
    } else {
        log("[flag] %s = %d", flag_name(id).c_str(), flag_get(id));
    }
}

// NX_CONTROL "pos": log the player's position (map coordinates, as in the layout records; -z = north). Same lookup as
// the Lua binding chrGetPlayer (0x417c1c): handle at field+0x1f4, actor from the actor list (0x3cad88: nodes at
// list+4, next +4, actor +8, actor id +4); chrGetActorPos reads x y z at actor+0xc after checking the tag at +8.
void rt::pos_command(const char* set) {  // set: "x y z" moves the player there (debug teleport)
    auto mapped = [](uint32_t a) { return a && mem_query(a).state != MS_FREE && mem_query(a + 0x200).state != MS_FREE; };
    const uint32_t list = 0x9f4510;  // the actor list object itself (static data)
    uint32_t field = ld32(0x9f47f4);
    if (!mapped(field)) { log("[pos] no field"); return; }
    uint32_t handle = ld32(field + 0x1f4);
    if (set && !strcmp(set, "all")) {  // control "actors": every actor's handle, tag and position (finding symbols)
        for (uint32_t node = ld32(list + 4), n = 0; mapped(node) && n < 10000; node = ld32(node + 4), n++) {
            uint32_t actor = ld32(node + 8);
            if (!mapped(actor)) continue;
            float p[3];
            memcpy(p, GPTR(actor + 0xc), 12);
            log("[actor] %08x handle %08x tag %08x vt %08x pos %.1f %.1f %.1f", actor, ld32(actor + 4), ld32(actor + 8), ld32(actor), p[0], p[1], p[2]);
        }
        return;
    }
    for (uint32_t node = ld32(list + 4), n = 0; mapped(node) && n < 10000; node = ld32(node + 4), n++) {
        uint32_t actor = ld32(node + 8);
        if (!mapped(actor) || ld32(actor + 4) != handle) continue;
        if (ld32(actor + 8) != 0x514c4742) break;
        float p[3];
        if (set && sscanf(set, "%f %f %f", &p[0], &p[1], &p[2]) == 3) memcpy(GPTR(actor + 0xc), p, 12);
        memcpy(p, GPTR(actor + 0xc), 12);
        log("[pos] player %.1f %.1f %.1f", p[0], p[1], p[2]);
        return;
    }
    log("[pos] player actor %#x not found", handle);
}

// ------------------------------------------------------------------------------------------------ nn::account
HLE_FN(acc_Initialize, "_ZN2nn7account10InitializeEv") {}
static void put_uid(uint32_t p) { st64(p, 0x0000000000000001ull); st64(p + 8, 0x544D535245434F4Dull); }
HLE_FN(acc_ShowSelector, "_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE") {
    put_uid(c->r[0]);
    c->r[0] = 0;
}
HLE_FN(acc_TryPreselected, "_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE") {
    memset(GPTR(c->r[0]), 0, 16);
    st32(c->r[0], 0x5553);
    c->r[0] = 1;
}
HLE_FN(acc_OpenUser, "_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE") {
    memset(GPTR(c->r[0]), 0, 16);
    st32(c->r[0], 0x5553);
    c->r[0] = 0;
}
HLE_FN(acc_CloseUser, "_ZN2nn7account9CloseUserERKNS0_10UserHandleE") {}
HLE_FN(acc_GetUserId, "_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE") {
    put_uid(c->r[0]);
    c->r[0] = 0;
}

// ------------------------------------------------------------------------------------------------ nn::time
HLE_FN(time_Initialize, "_ZN2nn4time10InitializeEv") { c->r[0] = 0; }
HLE_FN(time_Now, "_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE") {
    st64(c->r[0], (uint64_t)time(nullptr));
    c->r[0] = 0;
}
HLE_FN(time_ToCal, "_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE") {
    time_t t = (time_t)ld64(c->r[2]);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    uint32_t cal = c->r[0], info = c->r[1];
    st16(cal, lt.tm_year + 1900);
    st8(cal + 2, lt.tm_mon + 1);
    st8(cal + 3, lt.tm_mday);
    st8(cal + 4, lt.tm_hour);
    st8(cal + 5, lt.tm_min);
    st8(cal + 6, lt.tm_sec);
    if (info) {  // CalendarAdditionalInfo: dayOfWeek, dayOfYear, timeZone name[8], isDst, utc offset
        st32(info, lt.tm_wday);
        st32(info + 4, lt.tm_yday);
        memset(GPTR(info + 8), 0, 8);
        memcpy(GPTR(info + 8), "UTC", 3);
        st32(info + 16, lt.tm_isdst > 0);
        st32(info + 20, 0);
    }
    c->r[0] = 0;
}

// ------------------------------------------------------------------------------------------------ nn::diag
HLE_FN(diag_Assert, "_ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_i") {
    fatal("guest assertion (%u) '%s' in %s at %s:%d", c->r[0], gstr(c->r[1]).c_str(), gstr(c->r[2]).c_str(),
          gstr(c->r[3]).c_str(), (int)ld32(c->r[13]));
}
HLE_FN(diag_Abort, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i") {
    fatal("guest abort '%s' in %s at %s:%d (lr %s)", gstr(c->r[0]).c_str(), gstr(c->r[1]).c_str(), gstr(c->r[2]).c_str(),
          (int)c->r[3], symbol_at(c->r[14]));
}
HLE_FN(diag_AbortR, "_ZN2nn4diag6detail9AbortImplEPKcS3_S3_iPKNS_6ResultES3_z") {
    uint32_t res = ld32(c->r[13]), fmt = ld32(c->r[13] + 4);
    fatal("guest abort '%s' in %s at %s:%d result %08x: %s (lr %s)", gstr(c->r[0]).c_str(), gstr(c->r[1]).c_str(),
          gstr(c->r[2]).c_str(), (int)c->r[3], res ? ld32(res) : 0, gstr(fmt).c_str(), symbol_at(c->r[14]));
}
HLE_FN(diag_Unexpected, "_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i") {
    fatal("guest unexpected default in %s at %s:%d", gstr(c->r[0]).c_str(), gstr(c->r[1]).c_str(), (int)c->r[2]);
}

// ------------------------------------------------------------------------------------------------ nv:: (graphics firmware)
HLE_FN(nv_SetAlloc, "_ZN2nv20SetGraphicsAllocatorEPFPvjjS0_EPFvS0_S0_EPFS0_S0_jS0_ES0_") {}
HLE_FN(nv_Init, "_ZN2nv18InitializeGraphicsEPvj") {}
HLE_FN(nv_Fini, "_ZN2nv16FinalizeGraphicsEv") {}



// Hide field enemies (setting hide_field_enemies, user request 10-30): every field symbol is spawned by
// field_enemy_spawn (area load, respawn timer); with the setting on it spawns none (-1 = "no symbol", as when the
// group table has no entry). Symbols already in the area stay until the next area load.
HLE_FN(field_enemy_spawn, "field_enemy_spawn") {
    const char* e = getenv("NX_HIDE_FIELD_ENEMIES");
    if (e && atoi(e)) {
        static int n = 0;
        if (n++ < 5) log("field: enemy spawn skipped (hide field enemies)");
        c->r[0] = 0xffffffff;
        return;
    }
    c->r[0] = call_unhooked(c, "field_enemy_spawn", {c->r[0], c->r[1]});
}
