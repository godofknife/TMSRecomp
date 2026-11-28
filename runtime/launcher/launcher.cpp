// TMSRecomp launcher (tmslauncher.exe next to tms.exe): edits settings.ini, manages mods, updates from GitHub releases
// and starts the game. Drawn with Direct2D / DirectWrite (Windows components, no other dependency). Only keys this
// window knows are rewritten; comments and any other lines in settings.ini are kept.
// Linux: the same pages through linux_shim.h (SDL3 window, cairo / pango drawing); its own window loop, setup runner
// and update check are below, next to the Windows versions.
#ifdef _WIN32
#include <windows.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite_1.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <winhttp.h>
#include <windowsx.h>
#include <commdlg.h>
#else
#include "linux_shim.h"
#include <csignal>
#include <cstring>
#include <locale>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../src/settings.h"
#include "../src/sysinfo.h"

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------------ settings.ini
static fs::path g_dir, g_ini, g_data;
static std::vector<std::string> g_lines;
static bool g_dirty = false;

static std::string trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r"));
    s.erase(s.find_last_not_of(" \t\r") + 1);
    return s;
}

// The key of an uncommented "key = value" line (lower case, as tms.exe upper-cases it anyway), else "".
static std::string key_of(const std::string& line) {
    std::string l = line.substr(0, line.find_first_of(";#"));
    size_t eq = l.find('=');
    if (eq == std::string::npos) return "";
    std::string k = trim(l.substr(0, eq));
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    return k;
}

static std::string get(const char* k, const char* def) {
    for (auto& l : g_lines)
        if (key_of(l) == k) {
            std::string v = trim(l.substr(0, l.find_first_of(";#")).substr(l.find('=') + 1));
            return v.empty() ? def : v;
        }
    return def;
}

static void set(const char* k, const std::string& v) {
    g_dirty = true;
    for (auto& l : g_lines)
        if (key_of(l) == k) {
            l = std::string(k) + " = " + v;
            return;
        }
    if (!v.empty()) g_lines.push_back(std::string(k) + " = " + v);
}

// ANSI like tms.exe's getenv (ponytail: non-ASCII data paths need the wide-char API on both sides).
#ifdef _WIN32
static std::wstring wide(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0), 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), (int)w.size());
    w.pop_back();
    return w;
}
static std::string narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), (int)s.size(), nullptr, nullptr);
    s.pop_back();
    return s;
}
#else
static std::wstring wide(const std::string& s) { return draw::wide(s); }  // Linux: UTF-8 everywhere
static std::string narrow(const std::wstring& w) { return draw::utf8(w); }
#endif

static bool is_data(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p / "link" / "image.bin", ec) && fs::exists(p / "romfs", ec);
}

// The data folder tms.exe will use: ini "data", else the folder above the build folder (tms.exe's order).
static void find_data() {
    std::string d = get("data", "");
    g_data = !d.empty() ? fs::path(wide(d)) : is_data(g_dir.parent_path()) ? g_dir.parent_path() : fs::path();
}

static void load() {
    if (!fs::exists(g_ini)) std::ofstream(g_ini) << k_default_settings;
    g_lines.clear();
    std::ifstream f(g_ini);
    for (std::string l; std::getline(f, l);) g_lines.push_back(l);
    find_data();
}

static bool save() {
    std::ofstream f(g_ini);
    for (auto& l : g_lines) f << l << "\n";
    if (f) g_dirty = false;
    return (bool)f;
}

// ------------------------------------------------------------------------------------------------ what can be set
enum Kind { DROP, TOGGLE, SLIDER };
struct Opt { const char* v; const wchar_t* label; };
struct Setting {
    const char* key;
    const char* def;
    Kind kind;
    const wchar_t* name;
    const wchar_t* help;
    std::vector<Opt> opts;  // DROP; TOGGLE: {off, on}
    float lo = 0, hi = 0, step = 0;
};
enum { FPS, RES, WINDOW, PRESENT, POST, WALK, MUTE, TEXPACK, TEXDUMP, FXAA, FSR3Q, PADICONS, SHARPEN, UPSCALER, BGM, MSAA, DLSSP, MAPREV, FIELDCOS, ALLDIFF, ASPECT, UWFILL, BTNLAYOUT, RESMARK, MOUSECAM, MOUSESENS, MOUSEINV, BTLBUTTONS, LANGUAGE, NGPREQ, MOVIEFB, MOVIEGPU, AUTO30, CROWD, SHADOWD, WEAKHINT, HDR, HDRPAPER, HDRPEAK, SCENERES, HDRBOOST, HDRCOLOR, PAD_A, PAD_B, PAD_X, PAD_Y, PAD_L, PAD_R, PAD_ZL, PAD_ZR, PAD_PLUS, PAD_MINUS, PAD_L3, PAD_R3, NSET };
// controller remap choices (ui.cpp pad_map): positions, so the names fit Switch, PlayStation and Xbox pads alike
static const std::vector<Opt> k_pad_opts = {{"default", L"Default"}, {"south", L"Bottom (A / \u2715)"},
    {"east", L"Right (B / \u25cb)"}, {"west", L"Left (X / \u25a1)"}, {"north", L"Top (Y / \u25b3)"}, {"lb", L"LB / L1"},
    {"rb", L"RB / R1"}, {"lt", L"LT / L2"}, {"rt", L"RT / R2"}, {"start", L"Start / Options"}, {"back", L"Back / Share"},
    {"ls", L"Left stick click"}, {"rs", L"Right stick click"}, {"paddle1", L"Paddle 1 (right)"}, {"paddle2", L"Paddle 2 (left)"},
    {"paddle3", L"Paddle 3 (right)"}, {"paddle4", L"Paddle 4 (left)"}, {"none", L"Unbound"}};
#define PAD_SET(k, n) {"pad_" k, "default", DROP, n, L"The controller button that presses this Switch button. Picking one that "\
    L"another button has swaps the two. Restart the game after a change. Button prompts still follow Button layout.", k_pad_opts}
static const Setting k_set[NSET] = {
    {"fps", "60", DROP, L"Frame rate",
     L"The game's logic runs at this rate. 60 is the most tested. Above 60 the game keeps its speed and only gets "
     L"smoother.",
     {{"30", L"30 FPS · the game's own"}, {"60", L"60 FPS"}, {"120", L"120 FPS"}, {"144", L"144 FPS"},
      {"165", L"165 FPS"}, {"240", L"240 FPS"}, {"0", L"Uncapped"}}},
    {"res", "auto", DROP, L"Render resolution",
     L"Internal size of the 3D image. Auto fits your screen and graphics card. The game's own is 720p. Above your screen size it renders bigger and scales down (sharper, costs more).",
     {{"auto", L"Auto · fits your screen and GPU"}, {"720p", L"1280 × 720 · the game's own"}, {"1080p", L"1920 × 1080"}, {"1440p", L"2560 × 1440"},
      {"4k", L"3840 × 2160 · 4K"}, {"8k", L"7680 × 4320 · supersampling for 4K screens (~5 GB VRAM)"}}},
    {"window", "borderless", DROP, L"Display mode", L"F11 or Alt+Enter switches between the two while playing.",
     {{"borderless", L"Borderless fullscreen"}, {"window", L"Window"}}},
    {"present_mode", "mailbox", DROP, L"VSync",
     L"Mailbox shows the newest frame at each refresh: no tearing, low latency. Classic VSync waits in a queue. VRR: "
     L"for G-SYNC / FreeSync screens (turn it on in your GPU driver); keeps the frame rate just under your refresh rate.",
     {{"mailbox", L"Mailbox · no tearing"}, {"fifo", L"Classic VSync"}, {"vrr", L"VRR · G-SYNC / FreeSync"}, {"immediate", L"Off · tearing"}}},
    {"post_native", "1", TOGGLE, L"Blur effects at native size",
     L"Depth of field and bloom at the game's own size, as on the Switch. Off renders them at full resolution: "
     L"sharper, but with ringed halos around characters.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"walk_speed", "1.0", SLIDER, L"Field walk speed",
     L"How fast Itsuki walks and runs in dungeons and towns. 1.0× is the original. His leg animation keeps its "
     L"own speed.",
     {}, 1.0f, 3.0f, 0.1f},
    {"mute", "", TOGGLE, L"Mute sound", L"Turns all game audio off.", {{"", L"Off"}, {"1", L"On"}}},
    {"tex_pack", "auto", DROP, L"UHD texture pack",
     L"Uses the upscaled textures in <data>\\textures\\pack when there are any (tools\\uhd_pack.py makes them). Auto "
     L"leaves them off on integrated graphics and cards under 4 GB of VRAM.",
     {{"auto", L"Auto · on with 4 GB+ VRAM"}, {"0", L"Off"}, {"1", L"On"}}},
    {"tex_dump", "", TOGGLE, L"Collect textures for the pack",
     L"While you play, every new texture is saved to <data>\\textures\\dump, for uhd_pack.py to upscale. Costs "
     L"disk space and short hitches the first time a texture appears.",
     {{"", L"Off"}, {"1", L"On"}}},
    {"fx_fxaa", "1", DROP, L"Anti-aliasing",
     L"FXAA is the game's own. SMAA blurs textures less. FSR 3 and DLSS give the smoothest edges (DLSS: RTX GPU and "
     L"the Setup page's DLSS option). Off is the sharpest, with jagged edges. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"FXAA · the game's own"}, {"2", L"SMAA"}, {"3", L"AMD FSR 3 · temporal"}, {"4", L"NVIDIA DLSS · RTX GPUs"}}},
    {"fsr3_quality", "0", DROP, L"FSR 3 / DLSS upscaling",
     L"Only with Anti-aliasing = FSR 3 or DLSS. The 3D scene renders smaller and is scaled up: more fps, a little "
     L"softer. Native keeps the full size (DLSS Native is DLAA). The menus always stay sharp.",
     {{"0", L"Native · full size"}, {"1", L"Quality · 1/1.5"}, {"2", L"Balanced · 1/1.7"}, {"3", L"Performance · 1/2"}, {"4", L"Ultra Performance · 1/3"}}},
    {"pad_icons", "auto", DROP, L"Button icons",
     L"Button prompts drawn for your controller: PlayStation symbols or Xbox letters instead of the Switch ones. "
     L"Auto picks them from the connected pad.",
     {{"auto", L"Auto · from the pad"}, {"switch", L"Switch"}, {"playstation", L"PlayStation"}, {"xbox", L"Xbox"}}},
    {"sharpen", "0", DROP, L"Sharpening",
     L"Contrast-adaptive sharpening of the final image: crisper textures at 4K, no halos on hard edges.",
     {{"0", L"Off"}, {"35", L"Low"}, {"65", L"Medium"}, {"100", L"High"}}},
    {"upscaler", "fsr1", DROP, L"Screen scaler",
     L"Only used when the render resolution is below your screen size. FSR 1 keeps edges sharper than Bilinear. "
     L"For FSR 3 or DLSS, pick them under Anti-aliasing instead.",
     {{"bilinear", L"Bilinear"}, {"fsr1", L"FSR 1"}}},
    {"random_battle_bgm", "0", DROP, L"Random battle music",
     L"Game: a random battle track of the game each battle. Custom: a random .adx file from your music folder "
     L"(<data>\\music, or custom_bgm_dir in settings.ini) with the victory theme of the same series. Mixed: both.",
     {{"0", L"Off"}, {"1", L"Game tracks"}, {"2", L"Custom (.adx folder)"}, {"3", L"Mixed"}}},
    {"msaa", "0", DROP, L"MSAA",
     L"Smoother edges on 3D models, costs fps and VRAM. Not used with FSR 3 or DLSS. Also in the in-game menu "
     L"(F1 / L3 + R3).",
     {{"0", L"Off"}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"}}},
    {"dlss_preset", "0", DROP, L"DLSS model",
     L"Only with Anti-aliasing = DLSS. Auto is NVIDIA's pick for each mode (DLSS 4.5's M / L at Performance / Ultra "
     L"Performance). M and L look best but cost more fps on RTX 20 / 30 cards.",
     {{"0", L"Auto · NVIDIA's pick"}, {"11", L"K · transformer"}, {"13", L"M · DLSS 4.5 · made for Performance"}, {"12", L"L · DLSS 4.5 · made for Ultra Perf."}}},
    {"map_reveal", "0", TOGGLE, L"Reveal dungeon maps",
     L"Each Idolasphere floor shows its whole layout on the minimap and the map as soon as you enter it. What was "
     L"revealed stays revealed. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"field_costume", "0", TOGGLE, L"Overworld costume",
     L"Outside battle (town, dungeons, story scenes) the party wears the costume equipped in Equip > Costume, as in "
     L"battle. Off: their story clothes. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"all_difficulties", "1", TOGGLE, L"Unlock all difficulties",
     L"Friendly and Lunatic can be chosen from the start: New Game and System > Settings (Friendly .. Lunatic, also "
     L"mid-game). Normally Friendly comes after a Game Over on Easy and Lunatic only after clearing the game.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"aspect", "16:9", DROP, L"Aspect ratio",
     L"Ultrawide: the 3D view gets wider (more to see at the sides) while menus and the HUD stay 16:9 in the middle. "
     L"Auto follows your screen. A picture of another shape than the screen gets black bars, never stretched.",
     {{"16:9", L"16:9 · the game's own"}, {"auto", L"Auto · your screen's shape"}, {"21:9", L"21:9 · ultrawide"}, {"32:9", L"32:9 · super ultrawide"}}},
    {"ultrawide_fill", "1", TOGGLE, L"Ultrawide sides",
     L"Ultrawide only: full-screen 16:9 pictures and movies (title, story movies, Tokyo map) get a blurred, zoomed copy "
     L"of the picture at the sides instead of black bars. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Black"}, {"1", L"Blurred picture"}}},
    {"button_layout", "switch", DROP, L"Button layout",
     L"Switch: the right face button confirms and the bottom one cancels, as on the Switch. Xbox / PC: the bottom "
     L"button confirms (A/B and X/Y swapped); button prompts follow. Restart the game after a change.",
     {{"switch", L"Switch · right button confirms"}, {"xbox", L"Xbox / PC · bottom button confirms"}}},
    {"resist_marker", "1", TOGGLE, L"Resist markers",
     L"In battle a foe that resists the highlighted skill gets a shield where the game shows \"!\" for a weakness (also "
     L"in the target panel), and a resisted hit shows RESIST like WEAK. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"mouse_camera", "1", TOGGLE, L"Mouse turns the camera",
     L"While the game window has focus the mouse pointer is captured and moving it turns the camera (like the right "
     L"stick). The F1 menu or Alt+Tab frees the pointer.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"mouse_sensitivity", "1.0", SLIDER, L"Mouse sensitivity", L"How far the camera turns for the same mouse movement.",
     {}, 0.2f, 3.0f, 0.1f},
    {"mouse_invert_y", "0", TOGGLE, L"Invert mouse up / down", L"Moving the mouse up turns the camera down.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"battle_buttons", "0", DROP, L"P5-style battle buttons",
     L"Each battle command has its own button: bottom face button Attack, right Guard, top Skills, left Items, L Tactics, "
     L"R Change, - Escape, ZR Auto battle (+ Stats as before). P5 panel: a Persona 5 style command panel replaces the "
     L"game's wheel (Windows).",
     {{"0", L"Off \u00b7 the game's wheel"}, {"1", L"Buttons \u00b7 the game's wheel stays"}, {"2", L"Buttons + P5 panel"}}},
    {"language", "en", DROP, L"Language",
     L"Game text and menus. Voices stay as they are. The Uncensored mod's text is English only: with another language "
     L"its text changes are left out. Restart the game after a change.",
     {{"en", L"English"}, {"ja", L"日本語 · Japanese"}, {"fr", L"Français · French"},
      {"zh-Hans", L"简体中文 · Simplified Chinese"}, {"zh-Hant", L"繁體中文 · Traditional Chinese"}}},
    {"ng_plus_requests", "1", TOGGLE, L"New Game+ requests in a new game",
     L"Requests 49 (Versus Grandmaster Mirage!) and 50 (The Dragon Descends) are normally New Game+ only. On: they also "
     L"appear in a fresh game (49 from chapter 1, 50 after Path of the Strong and 36 more Detritus trades with Nieg).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"movie_hd_fallback", "1", TOGGLE, L"4K movies: 720p when too slow",
     L"Only with the 4K movies installed. On: a movie whose 4K frames this PC decodes too slowly plays at 720p from then on. "
     L"Off: movies always stay 4K (a slow PC may stutter).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"movie_gpu", "1", TOGGLE, L"4K movies: decode on the GPU",
     L"Only with the 4K movies installed. On: the GPU's video engine decodes them (Windows' VP9 decoder, \"VP9 Video "
     L"Extensions\"), about 2 CPU cores less while one plays. Off, or without that decoder: the CPU decodes them.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"auto_30", "1", TOGGLE, L"Auto 30 FPS when slow",
     L"Only at 60 FPS. Where this PC cannot hold 60 (busy streets like the Shibuya crossing), the game drops to 30 FPS "
     L"at full speed instead of running in slow motion, and goes back to 60 when it can. Also in the F1 menu.",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"crowd_density", "0", DROP, L"Crowd density",
     L"Walkers and cars in town streets like Shibuya. Fewer of them close by makes busy streets much lighter for weak "
     L"PCs; the far crowd stays. Applies when you next enter an area. Also in the F1 menu.",
     {{"0", L"Full · the game's own"}, {"1", L"Half"}, {"2", L"Low · for weak PCs"}}},
    {"shadow_detail", "0", DROP, L"Shadow detail",
     L"Low drops the far shadow layer; shadows near you stay. Fewer draws in big areas like Shibuya, for weak PCs. "
     L"Also in the F1 menu (applies at once).",
     {{"0", L"Full · the game's own"}, {"1", L"Low · for weak PCs"}}},
    {"weak_hint", "1", TOGGLE, L"WEAK in the skill list",
     L"In battle the skill list shows WEAK next to a skill when a foe on the field is weak to it and you already know "
     L"that weakness (like Persona 5 Royal). Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"hdr", "0", TOGGLE, L"HDR",
     L"For HDR screens: bright lights, spells and effects shine above normal white. Needs Windows HDR turned on "
     L"(Settings > System > Display). Without it the picture stays as before. Also in the in-game menu (F1 / L3 + R3).",
     {{"0", L"Off"}, {"1", L"On"}}},
    {"hdr_paper_white", "200", DROP, L"HDR paper white",
     L"Only with HDR on: how bright normal white (menus, text, daylight) is. 200 nits suits most rooms; more for a bright room.",
     {{"100", L"100 nits"}, {"150", L"150 nits"}, {"200", L"200 nits"}, {"300", L"300 nits"}, {"400", L"400 nits"}}},
    {"hdr_peak", "0", DROP, L"HDR peak brightness",
     L"Only with HDR on: the brightest your screen can show. Auto uses the value Windows reports for the screen.",
     {{"0", L"Auto · from Windows"}, {"400", L"400 nits"}, {"600", L"600 nits"}, {"1000", L"1000 nits"}, {"2000", L"2000 nits"}}},
    {"scene_res", "100", DROP, L"3D resolution",
     L"With Anti-aliasing Off, FXAA or SMAA: the 3D scene renders at this share of the resolution and is scaled up; "
     L"menus, text and the HUD stay sharp. Lower = much more FPS on weak graphics cards. Auto lowers it step by step while "
     L"the graphics card cannot keep your FPS and raises it again when there is room. Also in the F1 menu (applies at once).",
     {{"auto", L"Auto · follows your GPU"}, {"100", L"100% · full"}, {"85", L"85%"}, {"75", L"75%"}, {"67", L"67%"}, {"50", L"50% · weakest GPUs"}}},
    {"hdr_boost", "3", DROP, L"HDR highlights",
     L"Only with HDR on: lifts the bright parts of the 3D scene (sky, lights, spells, shiny surfaces) toward your "
     L"screen's peak brightness, like AutoHDR; menus and text stay at paper white. Off = the game's own brightness only.",
     {{"0", L"Off"}, {"1", L"Low"}, {"2", L"Medium"}, {"3", L"High"}, {"4", L"Max"}}},
    {"hdr_color", "2", DROP, L"HDR colour",
     L"Only with HDR on: richer, more vivid colours in the 3D scene (menus and text unchanged).",
     {{"0", L"Off"}, {"1", L"Low"}, {"2", L"Medium"}, {"3", L"High"}, {"4", L"Max"}}},
    PAD_SET("a", L"A"), PAD_SET("b", L"B"), PAD_SET("x", L"X"), PAD_SET("y", L"Y"), PAD_SET("l", L"L"), PAD_SET("r", L"R"),
    PAD_SET("zl", L"ZL"), PAD_SET("zr", L"ZR"), PAD_SET("plus", L"+"), PAD_SET("minus", L"\u2212"), PAD_SET("l3", L"L3"), PAD_SET("r3", L"R3")
};

static std::string value_of(int i) { return get(k_set[i].key, k_set[i].def); }
static int opt_index(int i) {
    std::string v = value_of(i);
    for (size_t k = 0; k < k_set[i].opts.size(); k++)
        if (v == k_set[i].opts[k].v) return (int)k;
    return -1;
}
static bool toggled(int i) {
    std::string v = value_of(i);
    return !v.empty() && v != "0";
}

// ------------------------------------------------------------------------------------------------ mods
struct Mod { std::string name; uint64_t bytes = 0; int files = 0; bool on = true; };
static std::vector<Mod> g_mods;

static std::vector<std::string> disabled_mods() {
    std::vector<std::string> v;
    std::string s = get("mods_disabled", "") + ",";
    for (size_t a = 0, b; (b = s.find(',', a)) != std::string::npos; a = b + 1)
        if (std::string n = trim(s.substr(a, b - a)); !n.empty()) v.push_back(n);
    return v;
}

static void scan_mods() {
    g_mods.clear();
    std::error_code ec;
    auto off = disabled_mods();
    for (auto& e : fs::directory_iterator(g_data / "mods", ec)) {
        if (!fs::is_directory(e.path() / "romfs", ec) && !fs::is_directory(e.path() / "textures", ec) && !fs::is_directory(e.path() / "cpk", ec)) continue;
        Mod m;
        m.name = e.path().filename().string();
        m.on = std::find(off.begin(), off.end(), m.name) == off.end();
        for (const char* sub : {"romfs", "textures", "cpk"})  // cpk: loose files inside the game's packs (s125)
            for (auto& f : fs::recursive_directory_iterator(e.path() / sub, ec))
                if (f.is_regular_file(ec)) m.files++, m.bytes += f.file_size(ec);
        g_mods.push_back(m);
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
}

static void store_mods() {
    std::string s;
    for (auto& m : g_mods)
        if (!m.on) s += (s.empty() ? "" : ",") + m.name;
    set("mods_disabled", s);
}

static std::wstring pretty(std::string n) {
    std::replace(n.begin(), n.end(), '_', ' ');
    if (!n.empty()) n[0] = (char)toupper((unsigned char)n[0]);
    return wide(n);
}

static std::wstring size_text(uint64_t b) {
    wchar_t t[32];
    if (b >= (1ull << 30)) swprintf(t, 32, L"%.1f GB", b / double(1ull << 30));
    else swprintf(t, 32, L"%.0f MB", b / double(1ull << 20));
    return t;
}

// ------------------------------------------------------------------------------------------------ updates
// Checks https://api.github.com/repos/<repo>/releases/latest (repo: ini update_repo, else k_update_repo). An update
// downloads the release's first .zip asset, unpacks it with Windows' tar.exe and replaces the files next to this exe
// (files in use are renamed to *.old first and deleted at the next start). settings.ini, saves and mods are never
// touched (they are not in a release).
static const char k_update_repo[] = "godofknife/TMSRecomp";  // GitHub releases: the update check and download
enum Upd { UPD_OFF, UPD_CHECKING, UPD_LATEST, UPD_NEW, UPD_FAIL, UPD_BUSY };
static std::atomic<int> g_upd{UPD_OFF};
static std::string g_rel_tag, g_rel_zip, g_rel_page;
static std::wstring g_rel_notes;  // the release's plain-words summary (release_summary), shown before updating
static bool g_whatsnew = false;   // the What's new dialog is open
static std::string g_as_version;  // test: --as-version X compares releases against X instead of k_version
static HWND g_wnd;
constexpr UINT WM_UPD = WM_APP + 1;
// update progress (sidebar box): phase 0 download (bytes), 1 unpacking, 2 installing (files); worker thread writes
static std::atomic<int> g_upd_phase{0};
static std::atomic<uint64_t> g_upd_done{0}, g_upd_total{0};
static void upd_tick() {  // redraw the box at most 10 times a second
    static std::atomic<uint64_t> last{0};
#ifdef _WIN32
    const uint64_t now = GetTickCount64();
#else
    const uint64_t now = SDL_GetTicks();
#endif
    if (now - last >= 100) last = now, PostMessageW(g_wnd, WM_UPD, 0, 0);
}


#ifdef _WIN32
static bool http_get(const std::wstring& url, std::string* body, HANDLE file) {
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256], path[2048], extra[1024];
    u.lpszHostName = host, u.dwHostNameLength = 256;
    u.lpszUrlPath = path, u.dwUrlPathLength = 2048;
    u.lpszExtraInfo = extra, u.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"TMSRecomp-launcher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, u.nPort, 0) : nullptr;
    std::wstring obj = std::wstring(path) + extra;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", obj.c_str(), nullptr, nullptr, nullptr,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, n = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &n, nullptr);
        ok = code == 200;
        if (file) {  // progress: the size from the (redirected) download's Content-Length
            wchar_t len[32] = {};
            DWORD ln = sizeof len;
            g_upd_done = 0;
            g_upd_total = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &ln, nullptr) ? _wcstoui64(len, nullptr, 10) : 0;
        }
        char buf[1 << 16];
        for (DWORD got = 0; ok && WinHttpReadData(r, buf, sizeof buf, &got) && got; got = 0) {
            if (body) body->append(buf, got);
            DWORD w;
            if (file && !WriteFile(file, buf, got, &w, nullptr)) ok = false;
            if (file) g_upd_done += got, upd_tick();
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}
#else
static bool http_get(const std::wstring& url, std::string* body, HANDLE file);  // below
#endif

// The string value of "key" after position from (JSON written by GitHub: no escapes in these values but \/).
static std::string json_str(const std::string& j, const char* key, size_t from = 0, size_t* at = nullptr) {
    size_t p = j.find(std::string("\"") + key + "\"", from);
    if (p == std::string::npos) return "";
    p = j.find('"', j.find(':', p) + 1);
    size_t e = j.find('"', p + 1);
    if (at) *at = e;
    std::string v = j.substr(p + 1, e - p - 1);
    for (size_t k; (k = v.find("\\/")) != std::string::npos;) v.erase(k, 1);
    return v;
}

// A JSON string value with its escapes decoded (\n, \", \\, \uXXXX -> UTF-8): the release body.
static std::string json_text(const std::string& j, const char* key) {
    size_t p = j.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return "";
    p = j.find('"', j.find(':', p) + 1);
    if (p == std::string::npos) return "";
    std::string v;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] != '\\' || i + 1 >= j.size()) { v += j[i]; continue; }
        const char e = j[++i];
        if (e == 'n') v += '\n';
        else if (e == 'r') v += '\r';
        else if (e == 't') v += ' ';
        else if (e == 'u' && i + 4 < j.size()) {
            unsigned c = (unsigned)strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (c >= 0xD800 && c < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {  // surrogate pair
                const unsigned lo = (unsigned)strtoul(j.substr(i + 3, 4).c_str(), nullptr, 16);
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 6;
            }
            if (c < 0x80) v += (char)c;
            else if (c < 0x800) v += (char)(0xC0 | c >> 6), v += (char)(0x80 | (c & 63));
            else if (c < 0x10000) v += (char)(0xE0 | c >> 12), v += (char)(0x80 | (c >> 6 & 63)), v += (char)(0x80 | (c & 63));
            else v += (char)(0xF0 | c >> 18), v += (char)(0x80 | (c >> 12 & 63)), v += (char)(0x80 | (c >> 6 & 63)), v += (char)(0x80 | (c & 63));
        } else v += e;
    }
    return v;
}

// The part of the release notes players see before updating: the "### In short" section (plain words, written for
// it), else "### What's new". Markdown is dropped: **bold**, `code`, [links](url), "(#12)" issue numbers; "- " -> bullet.
static std::wstring release_summary(const std::string& body) {
    size_t a = body.find("### In short");
    if (a == std::string::npos) a = body.find("### What's new");
    if (a == std::string::npos) return L"";
    a = body.find('\n', a);
    if (a == std::string::npos) return L"";
    size_t e = body.find("\n###", a);
    std::string t = body.substr(a + 1, e == std::string::npos ? std::string::npos : e - a - 1), out;
    for (size_t i = 0; i < t.size();) {
        if (t.compare(i, 2, "**") == 0) { i += 2; continue; }
        if (t[i] == '`' || t[i] == '\r') { i++; continue; }
        if (t[i] == '[') {  // [text](url) -> text
            const size_t c = t.find("](", i), z = c == std::string::npos ? c : t.find(')', c);
            if (c != std::string::npos && z != std::string::npos && t.find('\n', i) > c) { out += t.substr(i + 1, c - i - 1); i = z + 1; continue; }
        }
        if (t.compare(i, 3, " (#") == 0) {  // " (#12)" / " (#12, #13)"
            const size_t z = t.find(')', i);
            if (z != std::string::npos && z - i < 16) { i = z + 1; continue; }
        }
        if ((i == 0 || t[i - 1] == '\n') && t.compare(i, 2, "- ") == 0) { out += "\xE2\x80\xA2 "; i += 2; continue; }
        out += t[i++];
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    const size_t b = out.find_first_not_of("\n ");
    out = b == std::string::npos ? std::string() : out.substr(b);
#ifdef _WIN32  // the body is UTF-8 (wide() is the ANSI code page)
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, w.data(), (int)w.size());
    w.pop_back();
    return w;
#else
    return wide(out);
#endif
}

static std::vector<int> version_parts(const std::string& v) {
    std::vector<int> n;
    for (size_t i = 0; i < v.size();)
        if (isdigit((unsigned char)v[i])) n.push_back(atoi(v.c_str() + i)), i = v.find_first_not_of("0123456789", i);
        else i++;
    return n;
}

static std::string repo() { return get("update_repo", k_update_repo); }

static void check_updates() {
    if (repo().empty()) { g_upd = UPD_OFF; return; }
    g_upd = UPD_CHECKING;
    std::thread([] {
        std::string j;
        bool ok = http_get(L"https://api.github.com/repos/" + wide(repo()) + L"/releases/latest", &j, nullptr);
        if (ok) {
            g_rel_tag = json_str(j, "tag_name");
            g_rel_page = json_str(j, "html_url");
            g_rel_notes = release_summary(json_text(j, "body"));
            g_rel_zip.clear();
            for (size_t at = 0;;) {
                std::string u = json_str(j, "browser_download_url", at, &at);
                if (u.empty()) break;
#ifdef _WIN32
                if (u.size() > 4 && u.substr(u.size() - 4) == ".zip") { g_rel_zip = u; break; }
#else
                if (u.size() > 20 && u.substr(u.size() - 20) == "-linux-x64.tar.gz") { g_rel_zip = u; break; }
#endif
            }
#ifndef _WIN32
            if (g_rel_zip.empty()) g_rel_zip = g_rel_page;  // a release without a Linux download: Update opens its page
#endif
        }
        g_upd = !ok || g_rel_tag.empty() ? UPD_FAIL
              : version_parts(g_rel_tag) > version_parts(g_as_version.empty() ? k_version : g_as_version) && !g_rel_zip.empty() ? UPD_NEW : UPD_LATEST;
        PostMessageW(g_wnd, WM_UPD, 0, 0);
    }).detach();
}

#ifdef _WIN32
static bool run_wait(std::wstring cmd) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

static void delete_old_files() {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(g_dir, ec))
        if (e.path().extension() == ".old") fs::remove(e.path(), ec);
}

// Runs on a worker thread; posts WM_UPD with lParam 1 when the new files are in place.
static void apply_update() {
    g_upd = UPD_BUSY;
    std::thread([] {
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path() / "tmsrecomp_update", zip = tmp / "update.zip", stage = tmp / "files";
        fs::remove_all(tmp, ec);
        fs::create_directories(stage, ec);
        bool ok = false;
        g_upd_phase = 0, g_upd_done = 0, g_upd_total = 0;
        HANDLE f = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            ok = http_get(wide(g_rel_zip), nullptr, f);
            CloseHandle(f);
        }
        g_upd_phase = 1, PostMessageW(g_wnd, WM_UPD, 0, 0);
        ok = ok && run_wait(L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + stage.wstring() + L"\"");
        fs::path root = stage;
        if (ok) {  // a zip with one top folder: its contents
            auto it = fs::directory_iterator(stage, ec);
            if (it != fs::directory_iterator() && it->is_directory() && std::next(it) == fs::directory_iterator())
                root = it->path();
        }
        g_upd_done = 0, g_upd_total = 0;
        for (auto& e : fs::recursive_directory_iterator(root, ec)) g_upd_total += e.is_regular_file();
        g_upd_phase = 2, PostMessageW(g_wnd, WM_UPD, 0, 0);
        for (auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!ok) break;
            g_upd_done++, upd_tick();
            if (!e.is_regular_file()) continue;
            fs::path rel = fs::relative(e.path(), root, ec);
            std::wstring top = rel.begin()->wstring();
            if (rel == L"settings.ini" || top == L"save" || top == L"mods" || top == L"romfs" || top == L"link") continue;
            fs::path dst = g_dir / rel;
            fs::create_directories(dst.parent_path(), ec);
            if (fs::exists(dst, ec)) {
                fs::path old = dst;
                old += ".old";
                fs::remove(old, ec);
                fs::rename(dst, old, ec);
            }
            ok = fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);
        }
        g_upd = ok ? UPD_LATEST : UPD_FAIL;
        PostMessageW(g_wnd, WM_UPD, 0, ok ? 1 : 0);
    }).detach();
}
#else
// Linux helpers: start a program (argv[0] looked up in PATH), optionally with its output on a pipe, in its own process
// group (so Cancel stops it and everything it started).
static pid_t spawn(const std::vector<std::string>& args, const fs::path& cwd, int* out_fd, bool own_group) {
    int p[2] = {-1, -1};
    if (out_fd && pipe2(p, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out_fd) posix_spawn_file_actions_adddup2(&fa, p[1], 1), posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    if (own_group) posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP), posix_spawnattr_setpgroup(&at, 0);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    if (posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), environ)) pid = -1;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (out_fd) {
        close(p[1]);
        if (pid < 0) close(p[0]);
        else *out_fd = p[0];
    }
    return pid;
}
void open_with_desktop(const std::string& what) { spawn({"xdg-open", what}, {}, nullptr, true); }

static bool http_get(const std::wstring& url, std::string* body, HANDLE) {  // curl, which every desktop has
    int fd = -1;
    pid_t pid = spawn({"curl", "-fsSL", "--max-time", "60", "-H", "Accept: application/vnd.github+json", narrow(url)}, {}, &fd, false);
    if (pid < 0) return false;
    char buf[1 << 16];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;)
        if (body) body->append(buf, (size_t)n);
    close(fd);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void delete_old_files() {
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(g_dir, ec))
        if (e.path().extension() == ".old") fs::remove(e.path(), ec);
}

static bool linux_download() { return g_rel_zip.size() > 7 && g_rel_zip.substr(g_rel_zip.size() - 7) == ".tar.gz"; }
static bool run_wait(const std::vector<std::string>& args) {
    pid_t pid = spawn(args, {}, nullptr, false);
    int st = 0;
    return pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
// Linux (s129): the release's linux-x64 tarball (curl), unpacked (tar), its files over ours as on Windows (a replaced
// file is renamed to .old first: a running program keeps its old inode). A release without one: its page opens.
// Posts WM_UPD with lParam 1 when the new files are in place.
static void apply_update() {
    if (!linux_download()) { ShellExecuteW(g_wnd, L"open", wide(g_rel_page).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    g_upd = UPD_BUSY;
    std::thread([] {
        std::error_code ec;
        fs::path tmp = fs::temp_directory_path() / "tmsrecomp_update", tgz = tmp / "update.tar.gz", stage = tmp / "files";
        fs::remove_all(tmp, ec);
        fs::create_directories(stage, ec);
        g_upd_phase = 0, g_upd_done = 0, g_upd_total = 0;
        bool ok = false;
        pid_t pid = spawn({"curl", "-fsSL", "--max-time", "1800", "-o", tgz.string(), g_rel_zip}, {}, nullptr, false);
        for (int st = 0; pid > 0;) {  // progress: the file's size so far
            const pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) { ok = WIFEXITED(st) && WEXITSTATUS(st) == 0; break; }
            if (r < 0) break;
            const uintmax_t n = fs::file_size(tgz, ec);
            if (!ec) g_upd_done = n, upd_tick();
            SDL_Delay(100);
        }
        g_upd_phase = 1, PostMessageW(g_wnd, WM_UPD, 0, 0);
        ok = ok && run_wait({"tar", "-xzf", tgz.string(), "-C", stage.string()});
        fs::path root = stage;
        if (ok) {  // one top folder (TMSRecomp/): its contents
            auto it = fs::directory_iterator(stage, ec);
            if (it != fs::directory_iterator() && it->is_directory() && std::next(it) == fs::directory_iterator())
                root = it->path();
        }
        g_upd_done = 0, g_upd_total = 0;
        for (auto& e : fs::recursive_directory_iterator(root, ec)) g_upd_total += e.is_regular_file();
        g_upd_phase = 2, PostMessageW(g_wnd, WM_UPD, 0, 0);
        for (auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!ok) break;
            g_upd_done++, upd_tick();
            if (!e.is_regular_file()) continue;
            fs::path rel = fs::relative(e.path(), root, ec);
            const std::string top = rel.begin()->string();
            if (rel == "settings.ini" || top == "save" || top == "mods" || top == "romfs" || top == "link") continue;
            fs::path dst = g_dir / rel;
            fs::create_directories(dst.parent_path(), ec);
            if (fs::exists(dst, ec)) {
                fs::path old = dst;
                old += ".old";
                fs::remove(old, ec);
                fs::rename(dst, old, ec);
            }
            ok = fs::copy_file(e.path(), dst, fs::copy_options::overwrite_existing, ec);  // keeps the exec bits
        }
        fs::remove_all(tmp, ec);
        g_upd = ok ? UPD_LATEST : UPD_FAIL;
        PostMessageW(g_wnd, WM_UPD, 0, ok ? 1 : 0);
    }).detach();
}
#endif

// ------------------------------------------------------------------------------------------------ drawing
#ifdef _WIN32
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static IWICImagingFactory* g_wic;
#endif
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1Bitmap* g_hero;
static bool g_hero_title = false;  // g_hero is <data>/launcher_hero.* (the title screen the game saved), not a fallback
static IDWriteTextFormat *f_brand, *f_brand2, *f_nav, *f_h1, *f_h2, *f_caps, *f_label, *f_body, *f_small, *f_btn, *f_play;
static float g_scale = 1;  // pixels per DIP
static const float W = 1180, H = 720, SIDE = 264, FOOT = 84;

static D2D1_COLOR_F C(uint32_t rgb, float a = 1) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
}
// palette: ink with a green cast, one accent (the Encore green), mint for small highlights, amber for warnings.
// INK is the text on accent fills (white on this green is too faint).
static const uint32_t BG = 0x101513, SIDEBG = 0x0b0f0d, PANEL = 0x17201c, LINE = 0x263129, TEXT = 0xeaf3ee,
                      MUTED = 0x93a69b, DIM = 0x627269, ACCENT = 0x34d27f, ACCENT_HI = 0x55e398, CYAN = 0x7fe3d0,
                      WARN = 0xffb454, INK = 0x06170e, HOVER = 0x1f2a25, TRACK = 0x2e3a34, EDGE_HI = 0x3d4b44;

static D2D1_RECT_F R(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
static void fill(D2D1_RECT_F r, D2D1_COLOR_F c) { g_brush->SetColor(c); g_rt->FillRectangle(r, g_brush); }
static void rounded(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    g_brush->SetColor(c);
    g_rt->FillRoundedRectangle({r, rad, rad}, g_brush);
}
static void outline(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) {
    g_brush->SetColor(c);
    g_rt->DrawRoundedRectangle({{r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2}, rad, rad}, g_brush, w);
}
#ifdef _WIN32
static void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                 DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT p = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, float spacing = 0) {
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(p);
    g_brush->SetColor(c);
    if (spacing == 0) { g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, g_brush); return; }
    IDWriteTextLayout* l = nullptr;
    g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, r.right - r.left, r.bottom - r.top, &l);
    IDWriteTextLayout1* l1 = nullptr;
    if (l && SUCCEEDED(l->QueryInterface(&l1))) {
        l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)s.size()});
        l1->Release();
    }
    if (l) g_rt->DrawTextLayout({r.left, r.top}, l, g_brush), l->Release();
}

static IDWriteTextFormat* font(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                               DWRITE_FONT_STRETCH st = DWRITE_FONT_STRETCH_NORMAL) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, st, size, L"en-us", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    return f;
}

static ID2D1Bitmap* load_bitmap(const fs::path& p) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                 WICBitmapPaletteTypeCustom)))
        g_rt->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return bmp;
}
#else
// Linux text: pango with the format's font, wrapped at the box width, aligned like DirectWrite
static PangoLayout* layout_of(cairo_t* cr, const std::wstring& s, IDWriteTextFormat* f, float w, float spacing) {
    PangoLayout* l = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_from_string(f->family.c_str());
    pango_font_description_set_absolute_size(fd, f->size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)f->weight);
    if (f->condensed) pango_font_description_set_stretch(fd, PANGO_STRETCH_SEMI_CONDENSED);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    const std::string u = draw::utf8(s);
    pango_layout_set_text(l, u.c_str(), -1);
    if (w > 0) pango_layout_set_width(l, (int)(w * PANGO_SCALE)), pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_alignment(l, f->align == DWRITE_TEXT_ALIGNMENT_CENTER ? PANGO_ALIGN_CENTER
                                : f->align == DWRITE_TEXT_ALIGNMENT_TRAILING ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
    if (spacing != 0) {
        PangoAttrList* al = pango_attr_list_new();
        pango_attr_list_insert(al, pango_attr_letter_spacing_new((int)(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, al);
        pango_attr_list_unref(al);
    }
    return l;
}
static cairo_t* measure_cr() {  // a context for text sizes outside a paint
    static cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    static cairo_t* cr = cairo_create(s);
    return g_rt && g_rt->cr ? g_rt->cr : cr;
}
static void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                 DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT p = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, float spacing = 0) {
    if (s.empty() || !g_rt || !g_rt->cr) return;
    f->SetTextAlignment(a);
    f->SetParagraphAlignment(p);
    cairo_t* cr = g_rt->cr;
    PangoLayout* l = layout_of(cr, s, f, r.right - r.left, spacing);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(l, &ink, &log);
    const float h = r.bottom - r.top, y = p == DWRITE_PARAGRAPH_ALIGNMENT_NEAR ? r.top
                                         : p == DWRITE_PARAGRAPH_ALIGNMENT_FAR ? r.bottom - log.height
                                                                               : r.top + (h - log.height) / 2;
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_move_to(cr, r.left, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static IDWriteTextFormat* font(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                               DWRITE_FONT_STRETCH st = DWRITE_FONT_STRETCH_NORMAL) {
    auto* f = new IDWriteTextFormat;
    // Bahnschrift / Segoe UI are Windows fonts: close relatives first, then the desktop's sans
    f->family = !wcscmp(family, L"Bahnschrift") ? "Bahnschrift, Barlow Semi Condensed, Barlow, Roboto Condensed, Noto Sans, DejaVu Sans Condensed, Sans"
                                                : draw::k_ui_font;
    f->size = size, f->weight = w, f->condensed = st == DWRITE_FONT_STRETCH_CONDENSED;
    return f;
}

// PNG (cairo) or the BMPs the game writes (launcher_hero.bmp, F12 screenshots: 24 / 32 bpp, either row order)
static ID2D1Bitmap* load_bitmap(const fs::path& p) {
    cairo_surface_t* s = nullptr;
    const std::string ext = p.extension().string();
    if (ext == ".png") {
        s = cairo_image_surface_create_from_png(p.c_str());
    } else if (ext == ".bmp") {
        std::ifstream f(p, std::ios::binary);
        std::string b((std::istreambuf_iterator<char>(f)), {});
        auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) memcpy(&v, b.data() + o, 4); return v; };
        auto u16 = [&](size_t o) { uint16_t v = 0; if (o + 2 <= b.size()) memcpy(&v, b.data() + o, 2); return v; };
        if (b.size() > 54 && b[0] == 'B' && b[1] == 'M') {
            const uint32_t off = u32(10), w = u32(18), bpp = u16(28);
            const int32_t hs = (int32_t)u32(22);
            const uint32_t h = (uint32_t)std::abs(hs), stride = ((w * bpp / 8) + 3) & ~3u;
            if ((bpp == 24 || bpp == 32) && w && h && w < 16384 && h < 16384 && off + (size_t)stride * h <= b.size()) {
                s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)w, (int)h);
                uint8_t* d = cairo_image_surface_get_data(s);
                const int ds = cairo_image_surface_get_stride(s);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t* src = (const uint8_t*)b.data() + off + (size_t)stride * (hs < 0 ? y : h - 1 - y);
                    for (uint32_t x = 0; x < w; x++) {
                        const uint8_t* q = src + x * (bpp / 8);
                        uint8_t* o = d + (size_t)y * ds + x * 4;
                        o[0] = q[0], o[1] = q[1], o[2] = q[2], o[3] = 255;
                    }
                }
                cairo_surface_mark_dirty(s);
            }
        }
    }
    if (!s) return nullptr;
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return nullptr; }
    auto* bm = new ID2D1Bitmap;
    bm->s = s;
    return bm;
}
#endif

// Hero picture: <data>/launcher_hero.(png|jpg|bmp), else the newest F12 screenshot, else a drawn stage backdrop.
static void load_hero() {
    if (g_hero) g_hero->Release(), g_hero = nullptr;
    g_hero_title = false;
    std::error_code ec;
    for (const wchar_t* n : {L"launcher_hero.png", L"launcher_hero.jpg", L"launcher_hero.bmp"})
        if (fs::exists(g_data / n, ec) && (g_hero = load_bitmap(g_data / n))) { g_hero_title = true; return; }
    fs::path newest;
    fs::file_time_type t{};
    for (auto& e : fs::directory_iterator(g_data / "shots", ec))
        if (e.path().filename().wstring().rfind(L"screenshot_", 0) == 0 && e.last_write_time(ec) > t)
            t = e.last_write_time(ec), newest = e.path();
    if (!newest.empty()) g_hero = load_bitmap(newest);
}

#ifdef _WIN32
// Window icon (user 11-23): <data>/icon.ico, made from the player's own game: the icon setup took from the NSP
// (<data>/icon.jpg), else a square of the title screen picture the game saved (launcher_hero.*). Nothing of the game
// ships with us, so a fresh install shows the default icon until then. tms.exe uses the same file.
static bool make_icon(const fs::path& ico) {
    std::error_code ec;
    fs::path src = g_data / "icon.jpg";
    const bool own = fs::exists(src, ec);
    if (!own) {
        src.clear();
        for (const wchar_t* n : {L"launcher_hero.png", L"launcher_hero.jpg", L"launcher_hero.bmp"})
            if (fs::exists(g_data / n, ec)) { src = g_data / n; break; }
        if (src.empty()) return false;
    }
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    IStream* st = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(src.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)) && w >= 64 && h >= 64 &&
        SUCCEEDED(g_wic->CreateBitmapClipper(&clip))) {
        // the title screen: a square around the whole logo (centre, upper half); the NSP icon: all of it
        INT side = own ? (INT)std::min(w, h) : (INT)(h * 0.78f);
        INT cx = own ? (INT)w / 2 : (INT)(w * 0.488f), cy = own ? (INT)h / 2 : (INT)(h * 0.41f);
        WICRect rc{std::clamp(cx - side / 2, 0, (INT)w - side), std::clamp(cy - side / 2, 0, (INT)h - side), side, side};
        if (SUCCEEDED(clip->Initialize(fr, &rc)) && SUCCEEDED(g_wic->CreateBitmapScaler(&sc)) &&
            SUCCEEDED(sc->Initialize(clip, 256, 256, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) &&
            SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
            SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fe, nullptr)) &&
            SUCCEEDED(fe->Initialize(nullptr)) && SUCCEEDED(fe->SetSize(256, 256))) {
            WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
            ok = SUCCEEDED(fe->SetPixelFormat(&pf)) && SUCCEEDED(fe->WriteSource(cv, nullptr)) && SUCCEEDED(fe->Commit()) &&
                 SUCCEEDED(enc->Commit());
        }
    }
    if (ok) {  // ICO = 6-byte header + one 16-byte entry (256 x 256 = 0, 32 bpp) + the PNG
        HGLOBAL g = nullptr;
        GetHGlobalFromStream(st, &g);
        STATSTG ss{};
        st->Stat(&ss, STATFLAG_NONAME);
        const uint32_t n = (uint32_t)ss.cbSize.QuadPart;
        std::string png((const char*)GlobalLock(g), n);
        GlobalUnlock(g);
        uint8_t hdr[22] = {0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 32, 0};
        memcpy(hdr + 14, &n, 4);
        hdr[18] = 22;
        fs::path part = ico;
        part += L".part";
        {
            std::ofstream f(part, std::ios::binary);
            f.write((const char*)hdr, sizeof hdr).write(png.data(), png.size());
            ok = (bool)f;
        }
        ok = ok && MoveFileExW(part.c_str(), ico.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!ok) fs::remove(part, ec);
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{fe, enc, st, cv, sc, clip, fr, dec})
        if (u) u->Release();
    return ok;
}

static HICON g_icon_big, g_icon_small;
static void apply_icon() {
    if (g_data.empty() || !g_wnd) return;
    std::error_code ec;
    const fs::path ico = g_data / "icon.ico", jpg = g_data / "icon.jpg";
    if (!fs::exists(ico, ec) || (fs::exists(jpg, ec) && fs::last_write_time(jpg, ec) > fs::last_write_time(ico, ec)))
        if (!make_icon(ico)) return;
    HICON big = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_LOADFROMFILE);
    HICON sml = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    if (!big || !sml) {
        if (big) DestroyIcon(big);
        if (sml) DestroyIcon(sml);
        return;
    }
    SendMessageW(g_wnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(g_wnd, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    if (g_icon_big) DestroyIcon(g_icon_big), DestroyIcon(g_icon_small);
    g_icon_big = big, g_icon_small = sml;
}

static void draw_hero(D2D1_RECT_F r) {
    float w = r.right - r.left, h = r.bottom - r.top;
    if (g_hero) {  // cover: crop the picture to the box's aspect
        D2D1_SIZE_F s = g_hero->GetSize();
        float sw = s.width, sh = s.width * h / w;
        if (sh > s.height) sh = s.height, sw = s.height * w / h;
        D2D1_RECT_F src = R((s.width - sw) / 2, (s.height - sh) * 0.15f, sw, sh);
        g_rt->DrawBitmap(g_hero, r, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &src);
    } else {  // two stage spotlights over the ink
        fill(r, C(0x0d1a14));
        struct { float x, y, rad; uint32_t c; float a; } spots[] = {{0.72f, 0.15f, 0.75f, ACCENT, 0.55f},
                                                                    {0.25f, 0.9f, 0.6f, CYAN, 0.28f}};
        for (auto& sp : spots) {
            D2D1_GRADIENT_STOP st[2] = {{0, C(sp.c, sp.a)}, {1, C(sp.c, 0)}};
            ID2D1GradientStopCollection* sc = nullptr;
            ID2D1RadialGradientBrush* b = nullptr;
            g_rt->CreateGradientStopCollection(st, 2, &sc);
            g_rt->CreateRadialGradientBrush({{r.left + w * sp.x, r.top + h * sp.y}, {0, 0}, w * sp.rad, h * sp.rad * 1.4f},
                                            sc, &b);
            g_rt->FillRectangle(r, b);
            b->Release();
            sc->Release();
        }
        g_brush->SetColor(C(0xffffff, 0.035f));
        for (int i = 0; i < 7; i++) {  // faint light beams from the top right
            float x0 = r.left + w * (0.55f + i * 0.07f);
            ID2D1PathGeometry* g = nullptr;
            g_d2d->CreatePathGeometry(&g);
            ID2D1GeometrySink* k = nullptr;
            g->Open(&k);
            k->BeginFigure({x0, r.top}, D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pts[] = {{x0 + 18, r.top}, {x0 - w * 0.5f + i * 30, r.bottom}, {x0 - w * 0.5f + i * 30 - 70, r.bottom}};
            k->AddLines(pts, 3);
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close();
            g_rt->FillGeometry(g, g_brush);
            k->Release();
            g->Release();
        }
    }
    // fade into the page at the bottom and into the sidebar on the left
    auto fade = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_RECT_F box, float from) {
        D2D1_GRADIENT_STOP st[2] = {{0, C(BG, from)}, {1, C(BG, 1)}};
        ID2D1GradientStopCollection* sc = nullptr;
        ID2D1LinearGradientBrush* lb = nullptr;
        g_rt->CreateGradientStopCollection(st, 2, &sc);
        g_rt->CreateLinearGradientBrush({a, b}, sc, &lb);
        g_rt->FillRectangle(box, lb);
        lb->Release();
        sc->Release();
    };
    fade({0, r.bottom - h * 0.55f}, {0, r.bottom}, {r.left, r.bottom - h * 0.55f, r.right, r.bottom}, 0);
    fade({r.left + 160, 0}, {r.left, 0}, {r.left, r.top, r.left + 160, r.bottom}, 0);
}
#else
// Linux: the window icon comes from the desktop entry / AppImage; nothing to make here
static void apply_icon() {}

static void draw_hero(D2D1_RECT_F r) {
    cairo_t* cr = g_rt->cr;
    float w = r.right - r.left, h = r.bottom - r.top;
    cairo_save(cr);
    cairo_rectangle(cr, r.left, r.top, w, h);
    cairo_clip(cr);
    if (g_hero) {  // cover: crop the picture to the box's aspect
        D2D1_SIZE_F s = g_hero->GetSize();
        float sw = s.width, sh = s.width * h / w;
        if (sh > s.height) sh = s.height, sw = s.height * w / h;
        const float sx = (s.width - sw) / 2, sy = (s.height - sh) * 0.15f;
        cairo_save(cr);
        cairo_translate(cr, r.left, r.top);
        cairo_scale(cr, w / sw, h / sh);
        cairo_set_source_surface(cr, g_hero->s, -sx, -sy);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {  // two stage spotlights over the ink
        fill(r, C(0x0d1a14));
        struct { float x, y, rad; uint32_t c; float a; } spots[] = {{0.72f, 0.15f, 0.75f, ACCENT, 0.55f},
                                                                    {0.25f, 0.9f, 0.6f, CYAN, 0.28f}};
        for (auto& sp : spots) {
            D2D1_COLOR_F c = C(sp.c, sp.a);
            cairo_pattern_t* pt = cairo_pattern_create_radial(0, 0, 0, 0, 0, 1);
            cairo_pattern_add_color_stop_rgba(pt, 0, c.r, c.g, c.b, c.a);
            cairo_pattern_add_color_stop_rgba(pt, 1, c.r, c.g, c.b, 0);
            cairo_save(cr);
            cairo_translate(cr, r.left + w * sp.x, r.top + h * sp.y);
            cairo_scale(cr, w * sp.rad, h * sp.rad * 1.4f);
            cairo_set_source(cr, pt);
            cairo_restore(cr);
            cairo_rectangle(cr, r.left, r.top, w, h);
            cairo_fill(cr);
            cairo_pattern_destroy(pt);
        }
        cairo_set_source_rgba(cr, 1, 1, 1, 0.035);
        for (int i = 0; i < 7; i++) {  // faint light beams from the top right
            float x0 = r.left + w * (0.55f + i * 0.07f);
            cairo_move_to(cr, x0, r.top);
            cairo_line_to(cr, x0 + 18, r.top);
            cairo_line_to(cr, x0 - w * 0.5f + i * 30, r.bottom);
            cairo_line_to(cr, x0 - w * 0.5f + i * 30 - 70, r.bottom);
            cairo_close_path(cr);
            cairo_fill(cr);
        }
    }
    // fade into the page at the bottom and into the sidebar on the left
    auto fade = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_RECT_F box, float from) {
        D2D1_COLOR_F c = C(BG);
        cairo_pattern_t* pt = cairo_pattern_create_linear(a.x, a.y, b.x, b.y);
        cairo_pattern_add_color_stop_rgba(pt, 0, c.r, c.g, c.b, from);
        cairo_pattern_add_color_stop_rgba(pt, 1, c.r, c.g, c.b, 1);
        cairo_set_source(cr, pt);
        cairo_rectangle(cr, box.left, box.top, box.right - box.left, box.bottom - box.top);
        cairo_fill(cr);
        cairo_pattern_destroy(pt);
    };
    fade({0, r.bottom - h * 0.55f}, {0, r.bottom}, {r.left, r.bottom - h * 0.55f, r.right, r.bottom}, 0);
    fade({r.left + 160, 0}, {r.left, 0}, {r.left, r.top, r.left + 160, r.bottom}, 0);
    cairo_restore(cr);
}
#endif

// ------------------------------------------------------------------------------------------------ hit targets
struct Hit { D2D1_RECT_F r; int id; std::function<void(float x)> click; };
static std::vector<Hit> g_hits;
static int g_hover = -1, g_open = -1, g_drag = -1, g_page = 0;
static D2D1_RECT_F g_drag_track;
enum { ID_NAV = 100, ID_SET = 1000, ID_POP = 2000, ID_MOD = 3000, ID_PLAY = 10, ID_SAVE, ID_UPDATE, ID_NOTES,
       ID_BROWSE, ID_OPEN_GAME, ID_OPEN_SHOTS, ID_OPEN_MODS, ID_DUMP, ID_OPEN_LOG, ID_CHECK,
       ID_SU_SRC, ID_SU_KEYS, ID_SU_TARGET, ID_SU_SHADERS, ID_SU_DLSS, ID_SU_DLSS_RUN, ID_SU_RUN, ID_SU_CANCEL,
       ID_G_TOPIC = 4000, ID_G_BTN = 4100, ID_X = 4200, ID_KEY = 5000, ID_WN_BG = 6000, ID_WN_UPDATE, ID_WN_LATER };  // ID_X: page buttons below
// Scrolling pages draw shifted up by g_scroll[page] and clipped above the footer: hits move and clip with them.
static float g_scroll[8], g_max_scroll, g_bottom;
static bool g_clip;
static void hit(D2D1_RECT_F r, int id, std::function<void(float)> f) {
    if (g_clip) {
        r.top = std::max(r.top - g_scroll[g_page], 0.f), r.bottom = std::min(r.bottom - g_scroll[g_page], H - FOOT);
        if (r.bottom <= r.top) return;
    }
    g_hits.push_back({r, id, std::move(f)});
}
static bool hov(int id) { return g_hover == id; }
static void redraw() { InvalidateRect(g_wnd, nullptr, FALSE); }

static void play();
static void browse();
static void open_path(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    ShellExecuteW(g_wnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// "Dump game files" (s125, loose-file mods): tms.exe --dump unpacks the game's packs into <data>\dump\<pack>\<path>
// in its own console window (progress there; it continues where a stopped dump ended). Modders copy a file from there
// to mods\<mod>\cpk\<pack>\<same path>.
static void dump_files() {
    if (MessageBoxW(g_wnd, (L"This unpacks the game's files (about 19 GB, movies left out) into\n" + (g_data / "dump").wstring() +
                            L"\n\nTo change a file, copy it from there to mods\\<your mod name>\\cpk\\<pack>\\<same path> "
                            L"and edit the copy. Start?").c_str(),
                    L"Dump game files", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
#ifdef _WIN32
    std::wstring exe = (g_dir / "tms.exe").wstring(), dir = g_dir.wstring();
    std::wstring cmd = L"\"" + exe + L"\" --data \"" + g_data.wstring() + L"\" --dump";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi)) {
        MessageBoxW(g_wnd, (L"Could not start " + exe).c_str(), L"TMSRecomp", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    // shortcut: no terminal is opened on Linux; the dump runs in the background (progress: run ./tms --dump in a terminal)
    if (spawn({(g_dir / "tms").string(), "--data", g_data.string(), "--dump"}, g_dir, nullptr, true) < 0)
        MessageBoxW(g_wnd, L"Could not start the dump. In a terminal: ./tms --dump", L"TMSRecomp", MB_ICONERROR);
#endif
}

static void button(D2D1_RECT_F r, const wchar_t* label, int id, bool primary, std::function<void()> f,
                   IDWriteTextFormat* fmt = nullptr) {
    bool h = hov(id);
    if (primary) {
        rounded(r, 4, C(h ? ACCENT_HI : ACCENT));
    } else {
        rounded(r, 4, C(h ? HOVER : PANEL));
        outline(r, 4, C(h ? EDGE_HI : LINE));
    }
    text(label, r, fmt ? fmt : f_btn, C(primary ? INK : TEXT), DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, fmt == f_play ? 3.0f : 0.6f);
    hit(r, id, [f](float) { f(); });
}

static void chevron(float x, float y, D2D1_COLOR_F c, bool up = false) {
    g_brush->SetColor(c);
    float d = up ? -1 : 1;
    g_rt->DrawLine({x - 5, y - 2.5f * d}, {x, y + 2.5f * d}, g_brush, 1.6f);
    g_rt->DrawLine({x, y + 2.5f * d}, {x + 5, y - 2.5f * d}, g_brush, 1.6f);
}

static void toggle(D2D1_RECT_F r, bool on, int id, std::function<void()> f, bool enabled);
// One control for setting i in box r (the control only; the label is drawn by the caller).
static void control(int i, D2D1_RECT_F r) {
    const Setting& s = k_set[i];
    int id = ID_SET + i;
    bool h = hov(id) || g_open == i;
    if (s.kind == DROP) {
        rounded(r, 4, C(h ? 0x1c2621 : PANEL));
        outline(r, 4, C(g_open == i ? ACCENT : h ? EDGE_HI : LINE));
        int k = opt_index(i);
        std::wstring v = k >= 0 ? s.opts[k].label : wide(value_of(i)) + L" FPS";
        text(v, {r.left + 14, r.top, r.right - 36, r.bottom}, f_label, C(TEXT));
        chevron(r.right - 18, (r.top + r.bottom) / 2, C(MUTED), g_open == i);
        hit(r, id, [i](float) { g_open = g_open == i ? -1 : i; });
    } else if (s.kind == TOGGLE) {
        toggle(r, toggled(i), id, [i] { set(k_set[i].key, toggled(i) ? k_set[i].opts[0].v : k_set[i].opts[1].v); }, true);
    } else {
        float v = std::clamp((float)atof(value_of(i).c_str()), s.lo, s.hi), k = (v - s.lo) / (s.hi - s.lo);
        D2D1_RECT_F tr = {r.left, (r.top + r.bottom) / 2 - 2, r.right - 70, (r.top + r.bottom) / 2 + 2};
        rounded(tr, 2, C(TRACK));
        rounded({tr.left, tr.top, tr.left + (tr.right - tr.left) * k, tr.bottom}, 2, C(ACCENT));
        g_brush->SetColor(C(h || g_drag == i ? 0xffffff : 0xe3eee8));
        g_rt->FillEllipse({{tr.left + (tr.right - tr.left) * k, (tr.top + tr.bottom) / 2}, 9, 9}, g_brush);
        wchar_t t[16];
        swprintf(t, 16, L"%.1f×", v);
        text(t, {tr.right + 16, r.top, r.right, r.bottom}, f_label, C(TEXT), DWRITE_TEXT_ALIGNMENT_TRAILING);
        D2D1_RECT_F area = {tr.left - 10, r.top, tr.right + 10, r.bottom};
        hit(area, id, [i, tr](float x) {
            g_drag = i;
            g_drag_track = tr;
            SetCapture(g_wnd);
            const Setting& s = k_set[i];
            float k = std::clamp((x - tr.left) / (tr.right - tr.left), 0.f, 1.f);
            float v = s.lo + std::round(k * (s.hi - s.lo) / s.step) * s.step;
            char b[16];
            snprintf(b, 16, "%.1f", v);
            set(s.key, b);
        });
    }
}

// A settings row: name, help text under it, control on the right. Returns the row height.
#ifdef _WIN32
static float text_h(const std::wstring& s, IDWriteTextFormat* f, float w) {
    IDWriteTextLayout* l = nullptr;
    DWRITE_TEXT_METRICS m{};
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 4000, &l))) l->GetMetrics(&m), l->Release();
    return m.height;
}

static float text_w(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    DWRITE_TEXT_METRICS m{};
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, &l))) l->GetMetrics(&m), l->Release();
    return m.widthIncludingTrailingWhitespace;
}
#else
static float text_h(const std::wstring& s, IDWriteTextFormat* f, float w) {
    if (s.empty()) return 0;
    PangoLayout* l = layout_of(measure_cr(), s, f, w, 0);
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    g_object_unref(l);
    return (float)lh;
}
static float text_w(const std::wstring& s, IDWriteTextFormat* f) {
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    PangoLayout* l = layout_of(measure_cr(), s, f, 0, 0);
    int lw, lh;
    pango_layout_get_pixel_size(l, &lw, &lh);
    g_object_unref(l);
    return (float)lw;
}
#endif

// A settings row: name, help text under it (and an optional value line in green), control on the right. The row is
// as tall as its text. Returns the row height.
static float row_with(const std::wstring& name, const std::wstring& help, float x, float y, float w,
                      const std::function<void(D2D1_RECT_F)>& ctl, const std::wstring& value = L"") {
    float hh = text_h(help, f_small, w - 400), vh = value.empty() ? 0 : 26, sep = y + std::max(84.f, 28 + hh + vh + 20);
    text(name, R(x, y, w - 380, 26), f_label, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text(help, R(x, y + 28, w - 400, hh + 4), f_small, C(MUTED), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (vh) text(value, R(x, y + 34 + hh, w - 400, 22), f_small, C(ACCENT));
    ctl(R(x + w - 360, y, 360, 40));
    fill(R(x, sep, w, 1), C(LINE));
    g_bottom = std::max(g_bottom, sep);
    return sep + 2 - y;
}
static float row(int i, float x, float y, float w) {
    return row_with(k_set[i].name, k_set[i].help, x, y, w, [i](D2D1_RECT_F r) { control(i, r); });
}

// An on/off switch in box r (vertically centred), with On / Off beside it.
static void toggle(D2D1_RECT_F r, bool on, int id, std::function<void()> f, bool enabled) {
    D2D1_RECT_F t = R(r.left, (r.top + r.bottom) / 2 - 12, 46, 24);
    rounded(t, 12, C(on ? ACCENT : TRACK, enabled ? 1 : 0.5f));
    g_brush->SetColor(C(on ? INK : 0xffffff, enabled ? 1 : 0.5f));
    g_rt->FillEllipse({{on ? t.right - 12 : t.left + 12, t.top + 12}, 8.5f, 8.5f}, g_brush);
    text(on ? L"On" : L"Off", {t.right + 12, r.top, t.right + 60, r.bottom}, f_label, C(on ? TEXT : MUTED));
    if (enabled) hit({t.left, r.top, t.right + 50, r.bottom}, id, [f](float) { f(); });
}

// A small rounded label: "Ready to play" in green, a warning in amber. Right-aligned at `right`.
static void pill(const std::wstring& t, float right, float y, uint32_t c) {
    float tw = text_w(t, f_small) + 26;
    D2D1_RECT_F r = R(right - tw, y, tw, 26);
    rounded(r, 13, C(c, 0.14f));
    outline(r, 13, C(c, 0.55f));
    text(t, r, f_small, C(c), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// A green circle with a step number.
static void badge(float cx, float cy, int n) {
    g_brush->SetColor(C(ACCENT));
    g_rt->FillEllipse({{cx, cy}, 13, 13}, g_brush);
    text(std::to_wstring(n), R(cx - 13, cy - 13, 26, 26), f_btn, C(INK), DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

// A button as wide as its label; returns the x after it (with a gap).
static float button_at(float x, float y, const std::wstring& label, int id, bool primary, std::function<void()> f, float h = 42) {
    float bw = text_w(label, f_btn) + 44;
    button(R(x, y, bw, h), label.c_str(), id, primary, std::move(f));
    return x + bw + 12;
}

static void card(D2D1_RECT_F r) {
    rounded(r, 10, C(PANEL));
    outline(r, 10, C(LINE));
}

static void popup() {
    if (g_open < 0) return;
    const Setting& s = k_set[g_open];
    // find the control's box from this frame's hits
    D2D1_RECT_F at{};
    for (auto& h : g_hits)
        if (h.id == ID_SET + g_open) at = h.r;
    float ih = 38, ph = ih * s.opts.size() + 8;
    float top = at.bottom + 6 + ph > H - 8 ? at.top - 6 - ph : at.bottom + 6;
    D2D1_RECT_F p = {at.left, top, at.right, top + ph};
    rounded({p.left, p.top + 4, p.right, p.bottom + 6}, 6, C(0x000000, 0.45f));  // shadow
    rounded(p, 6, C(0x1a2420));
    outline(p, 6, C(0x34423b));
    int cur = opt_index(g_open);
    for (size_t k = 0; k < s.opts.size(); k++) {
        D2D1_RECT_F ir = R(p.left + 4, p.top + 4 + ih * k, p.right - p.left - 8, ih);
        int id = ID_POP + (int)k;
        if (hov(id)) rounded(ir, 4, C(0x2a3831));
        if ((int)k == cur) fill(R(ir.left, ir.top + 9, 3, ih - 18), C(ACCENT));
        text(s.opts[k].label, {ir.left + 14, ir.top, ir.right - 8, ir.bottom}, f_label, C((int)k == cur ? 0xffffff : TEXT));
        int i = g_open;
        hit(ir, id, [i, k](float) { set(k_set[i].key, k_set[i].opts[k].v); g_open = -1; });
    }
}

// What's new before updating (user 11-27): the release's plain-words summary as a list, then Update / Close (no GitHub link).
static void whatsnew_dialog() {
    if (!g_whatsnew) return;
    fill(R(0, 0, W, H), C(0x000000, 0.6f));
    hit(R(0, 0, W, H), ID_WN_BG, [](float) {});  // clicks outside the box do nothing
    const float bw = 680, bx = (W - bw) / 2, pad = 32, tw = bw - 2 * pad - 22;
    std::vector<std::pair<bool, std::wstring>> items;  // (bullet, text) per line of the summary
    const std::wstring body = g_rel_notes.empty() ? L"• Fixes and improvements. The list of changes could not be loaded right now." : g_rel_notes;
    for (size_t i = 0; i <= body.size();) {
        size_t e = body.find(L'\n', i);
        if (e == std::wstring::npos) e = body.size();
        std::wstring l = body.substr(i, e - i);
        i = e + 1;
        if (l.empty()) continue;
        const bool bul = l.rfind(L"• ", 0) == 0;
        items.push_back({bul, bul ? l.substr(2) : l});
    }
    float th = 0;
    const float maxh = H - 300.f;
    size_t shown = 0;
    for (; shown < items.size(); shown++) {
        const float h = text_h(items[shown].second, f_body, tw) + 12;
        if (th + h > maxh) break;
        th += h;
    }
    if (shown < items.size()) th += 30;
    const float bh = th + 200, by = (H - bh) / 2;
    const D2D1_RECT_F box = R(bx, by, bw, bh);
    rounded({box.left, box.top + 6, box.right, box.bottom + 8}, 12, C(0x000000, 0.45f));
    rounded(box, 12, C(0x141c18));
    outline(box, 12, C(ACCENT, 0.7f));
    rounded(R(bx + pad, by + 26, 54, 22), 11, C(ACCENT));
    text(L"NEW", R(bx + pad, by + 26, 54, 22), f_small, C(0x0b100e), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 1.5f);
    text(L"What's new in " + wide(g_rel_tag), R(bx + pad, by + 54, bw - 2 * pad, 34), f_h2, C(0xffffff));
    text(L"You have v" + wide(g_as_version.empty() ? k_version : g_as_version) + L". Updating keeps your settings, saves and mods.",
         R(bx + pad, by + 90, bw - 2 * pad, 20), f_small, C(MUTED));
    float y = by + 126;
    for (size_t i = 0; i < shown; i++) {
        const float h = text_h(items[i].second, f_body, tw);
        if (items[i].first) {
            g_brush->SetColor(C(ACCENT));
            g_rt->FillEllipse({{bx + pad + 6, y + 10}, 3.5f, 3.5f}, g_brush);
        }
        text(items[i].second, R(bx + pad + (items[i].first ? 22 : 0), y, tw, h), f_body, C(items[i].first ? TEXT : 0xffffff),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += h + 12;
    }
    if (shown < items.size()) text(L"…and " + std::to_wstring(items.size() - shown) + L" more", R(bx + pad + 22, y, tw, 20), f_small, C(DIM));
    const float yb = by + bh - 66;
    fill(R(bx + pad, yb - 16, bw - 2 * pad, 1), C(LINE));
    button(R(bx + bw - pad - 120, yb, 120, 40), L"Update", ID_WN_UPDATE, true, [] { g_whatsnew = false; apply_update(); });
    button(R(bx + bw - pad - 120 - 12 - 110, yb, 110, 40), L"Close", ID_WN_LATER, false, [] { g_whatsnew = false; });
}

static void section(const wchar_t* t, float x, float y, float w) {
    text(t, R(x, y, w, 20), f_caps, C(MUTED), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 2.2f);
    fill(R(x, y + 26, 28, 2), C(ACCENT));
}

static void page_header(const wchar_t* title, const wchar_t* sub) {
    text(title, R(SIDE + 48, 34, 700, 48), f_h1, C(TEXT));
    text(sub, R(SIDE + 50, 82, 760, 22), f_small, C(MUTED));
}

static const wchar_t* k_pages[] = {L"Play", L"Graphics", L"Display & FPS", L"Game", L"Mods", L"Advanced", L"Setup", L"Guide"};

static std::wstring status_line() {
    std::wstring s;
    // by value: the indexes were one off since "auto" became option 0 (reddit 11-26: 1440p showed as 4K, 720p as 1080p)
    const std::string r = value_of(RES);
    s += r == "8k" ? L"8K · 7680 × 4320" : r == "4k" ? L"4K · 3840 × 2160" : r == "1440p" ? L"2560 × 1440" : r == "1080p" ? L"1920 × 1080"
         : r == "720p" ? L"1280 × 720" : L"Auto resolution";
    std::string f = value_of(FPS);
    s += f == "0" ? L"  ·  uncapped" : L"  ·  " + wide(f) + L" FPS";
    s += value_of(WINDOW) == "window" ? L"  ·  window" : L"  ·  borderless";
    int on = 0;
    for (auto& m : g_mods) on += m.on;
    if (on) s += L"  ·  " + std::to_wstring(on) + (on == 1 ? L" mod" : L" mods");
    if (atof(value_of(WALK).c_str()) > 1.05) s += L"  ·  walk " + wide(value_of(WALK)) + L"×";
    return s;
}

static void draw_sidebar() {
    fill(R(0, 0, SIDE, H), C(SIDEBG));
    fill(R(SIDE - 1, 0, 1, H), C(LINE));
    text(L"TOKYO MIRAGE", R(28, 30, SIDE - 40, 30), f_brand, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 1.5f);
    text(L"SESSIONS", R(28, 58, SIDE - 40, 30), f_brand, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 1.5f);
    text(L"♯FE ENCORE", R(29, 90, SIDE - 40, 22), f_brand2, C(ACCENT), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 3.0f);
    text(L"native PC recompilation · v" + wide(k_version), R(29, 116, SIDE - 40, 20), f_small, C(DIM));
    float y = 168;
    for (int p = 0; p < (int)std::size(k_pages); p++, y += 50) {
        D2D1_RECT_F r = R(0, y, SIDE - 1, 46);
        bool sel = p == g_page, h = hov(ID_NAV + p);
        if (sel) fill(r, C(0x16201b)), fill(R(0, y + 8, 3, 30), C(ACCENT));
        else if (h) fill(r, C(0x131a17));
        text(k_pages[p], {30, r.top, r.right, r.bottom}, f_nav, C(sel ? 0xffffff : h ? TEXT : MUTED),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0.4f);
        hit(r, ID_NAV + p, [p](float) { g_page = p; g_open = -1; if (p == 4) scan_mods(); });
    }
    // update box
    D2D1_RECT_F b = R(20, H - 150, SIDE - 40, 124);
    int u = g_upd;
    if (u == UPD_NEW || u == UPD_BUSY) {
        rounded(b, 6, C(0x12281c));
        outline(b, 6, C(ACCENT, 0.7f));
        text(L"Version " + wide(g_rel_tag) + L" is ready", R(b.left + 16, b.top + 12, b.right - b.left - 32, 22),
             f_label, C(0xffffff));
        if (u == UPD_BUSY) {  // progress bar: download (MB), unpacking, installing (files)
            const int ph = g_upd_phase;
            const uint64_t d = g_upd_done, t = g_upd_total;
            const float frac = ph == 1 ? 1.f : t ? std::min(1.f, (float)d / t) : 0.f;
            const D2D1_RECT_F bar = R(b.left + 16, b.top + 46, b.right - b.left - 32, 8);
            rounded(bar, 4, C(PANEL));
            if (frac > 0) rounded(R(bar.left, bar.top, (bar.right - bar.left) * frac, 8), 4, C(ACCENT));
            wchar_t line[96];
            if (ph == 0 && t) swprintf(line, 96, L"Downloading %.0f / %.0f MB (%d%%)", d / 1048576.0, t / 1048576.0, (int)(frac * 100));
            else if (ph == 0) swprintf(line, 96, L"Downloading %.0f MB…", d / 1048576.0);
            else if (ph == 1) swprintf(line, 96, L"Unpacking…");
            else swprintf(line, 96, L"Installing %llu / %llu files", (unsigned long long)d, (unsigned long long)t);
            text(line, R(b.left + 16, b.top + 60, b.right - b.left - 32, 20), f_small, C(MUTED));
        }
        else {
            button(R(b.left + 16, b.top + 44, 92, 34), L"Update", ID_UPDATE, true, [] {
#ifndef _WIN32
                if (!linux_download()) { apply_update(); return; }  // no Linux download in the release: its page
#endif
                g_whatsnew = true;  // What's new first; its Update now button installs
            });
            button(R(b.left + 116, b.top + 44, 92, 34), L"What's new", ID_NOTES, false, [] { g_whatsnew = true; });
        }
        text(L"Installed: v" + wide(k_version), R(b.left + 16, b.top + 90, 200, 20), f_small, C(DIM));
    } else {
        const wchar_t* t = u == UPD_CHECKING ? L"Checking for updates…"
                         : u == UPD_LATEST   ? L"Up to date"
                         : u == UPD_FAIL     ? L"Update check failed"
                                             : L"Updates not set up yet";
        g_brush->SetColor(C(u == UPD_LATEST ? CYAN : u == UPD_FAIL ? WARN : DIM));
        g_rt->FillEllipse({{b.left + 5, b.bottom - 40}, 3.5f, 3.5f}, g_brush);
        text(t, R(b.left + 18, b.bottom - 50, 200, 20), f_small, C(MUTED));
        if (u == UPD_LATEST || u == UPD_FAIL)
            button(R(b.left, b.bottom - 22, 120, 26), L"Check again", ID_CHECK, false, [] { check_updates(); }, f_small);
        else
            text(L"In game: F12 screenshot,\nF11 / Alt+Enter window mode", R(b.left, b.bottom - 26, 220, 34), f_small, C(DIM),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
}

static void draw_footer();
static void setup_go();

// ------------------------------------------------------------------------------------------------ first-run setup
// setup\tools\setup\setup.py (with setup\python) builds the data folder and game.dll from the player's own game:
// this download holds no game code. The script prints STEP / PROGRESS / ERROR / DATA / DONE lines (see setup.py).
constexpr UINT WM_SETUP = WM_APP + 2;
static std::wstring g_su_src, g_su_keys, g_su_target;  // NSP file or extracted folder; keys + target for an NSP
static bool g_su_shaders = true;
static bool g_su_dlss = false;
// NVIDIA's DLSS file next to the game (setup.py get_dlss): nvngx_dlss.dll, on Linux libnvidia-ngx-dlss.so.<version>
static bool dlss_installed() {
#ifdef _WIN32
    return fs::exists(g_dir / "nvngx_dlss.dll");
#else
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_dir, ec))
        if (e.path().filename().string().rfind("libnvidia-ngx-dlss.so.", 0) == 0) return true;
    return false;
#endif
}  // the player's tick: NVIDIA's licence applies to the file it downloads
static std::mutex g_su_lock;
static struct SetupState {
    bool running = false, done = false;
    int step = 0, steps = 6;
    double done_n = 0, total = 0;
    std::wstring text, error;
} g_su;
static HANDLE g_su_job;
static bool g_su_extra = false;  // the running / last job is a Guide install (mod, ZIP, music), not the game setup
static int g_topic = 0;  // Guide topic (the Setup page's Extras tiles open one)
static int g_su_topic = -1;       // the Guide topic that install belongs to (its status shows only there)
static DWORD g_su_t0;

#ifdef _WIN32
static bool game_ready() { return is_data(g_data) && fs::exists(g_dir / "game.dll"); }
#else
static bool game_ready() {  // game.so next to tms, or where NX_GAME_SO points (a build outside a read-only install)
    const char* e = getenv("NX_GAME_SO");
    return is_data(g_data) && (fs::exists(g_dir / "game.so") || (e && *e && fs::exists(e)));
}
#endif

// What game.dll and the shader cache were built with: tms.exe (its exports), setup\sdk, setup\tools, setup\llvm and the
// shader translator. A finished setup stores this in setup\built.txt; after an update it differs -> setup runs again
// (incremental: only the steps whose tools changed, usually a relink of game.dll).
static std::string build_signature() {
    std::error_code ec;
    std::vector<std::string> v;
    auto add = [&](const fs::path& p) {
        if (!fs::is_regular_file(p, ec)) return;
        auto t = fs::last_write_time(p, ec).time_since_epoch().count();
        v.push_back(fs::relative(p, g_dir, ec).string() + "|" + std::to_string(fs::file_size(p, ec)) + "|" + std::to_string(t));
    };
#ifdef _WIN32
    add(g_dir / "tms.exe");
#else
    add(g_dir / "tms");
#endif
    add(g_dir / "shaderc" / "NxShader.dll");
    for (const char* d : {"sdk", "tools", "llvm"})
        for (auto it = fs::recursive_directory_iterator(g_dir / "setup" / d, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec) && it->path().filename() == "__pycache__") { it.disable_recursion_pending(); continue; }
            if (it->path().extension() != ".old") add(it->path());
        }
    std::sort(v.begin(), v.end());
    std::string s;
    for (auto& x : v) s += x + "\n";
    return s;
}
static fs::path built_file() { return g_dir / "setup" / "built.txt"; }
static bool setup_stale() {  // dev builds (no setup\) never are
    if (!game_ready() || !fs::exists(g_dir / "setup" / "tools")) return false;
    std::ifstream f(built_file(), std::ios::binary);
    std::string old((std::istreambuf_iterator<char>(f)), {});
    return old != build_signature();
}
static bool su_is_nsp() { return !g_su_src.empty() && !fs::is_directory(g_su_src); }

static void su_line(const std::string& l) {
    std::lock_guard<std::mutex> lk(g_su_lock);
    if (!l.compare(0, 5, "STEP ")) {
        int i = 0, k = 0, used = 0;
        sscanf(l.c_str() + 5, "%d %d %n", &i, &k, &used);
        g_su.step = i, g_su.steps = k, g_su.text = wide(l.substr(5 + used)), g_su.done_n = g_su.total = 0;
    } else if (!l.compare(0, 9, "PROGRESS ")) {
        sscanf(l.c_str() + 9, "%lf %lf", &g_su.done_n, &g_su.total);
    } else if (!l.compare(0, 6, "ERROR ")) {
        g_su.error = wide(l.substr(6));
    } else if (!l.compare(0, 5, "DATA ")) {
        g_data = fs::path(wide(l.substr(5)));
    } else if (l == "DONE") {
        g_su.done = true;
    } else if (!g_su.error.empty() && g_su.error.size() < 1500) {
        g_su.error += L"\n" + wide(l);  // the tool's own error lines follow the ERROR line
    }
}

#ifdef _WIN32
static void su_reader(HANDLE out) {
    std::string buf;
    char b[4096];
    DWORD n;
    while (ReadFile(out, b, sizeof b, &n, nullptr) && n) {
        buf.append(b, n);
        for (size_t e; (e = buf.find('\n')) != std::string::npos; buf.erase(0, e + 1)) {
            su_line(trim(buf.substr(0, e)));
            PostMessageW(g_wnd, WM_SETUP, 0, 0);
        }
    }
    CloseHandle(out);
    {
        std::lock_guard<std::mutex> lk(g_su_lock);
        g_su.running = false;
        if (!g_su.done && g_su.error.empty()) g_su.error = L"Setup stopped.";
    }
    PostMessageW(g_wnd, WM_SETUP, 1, 0);
}

// extra: setup.py arguments of a Guide page install (--mod / --zip / --music <path>), into the data folder
static void su_start(bool dlss_only = false, const std::wstring& extra = L"") {
    fs::path py = g_dir / "setup" / "python" / "python.exe", script = g_dir / "setup" / "tools" / "setup" / "setup.py";
    if (!fs::exists(script)) {
        MessageBoxW(g_wnd, L"The setup tools (setup\\ folder) are missing: extract the whole TMSRecomp ZIP again.",
                    L"TMSRecomp", MB_ICONERROR);
        return;
    }
    if (!dlss_only && extra.empty() && (g_su_src.empty() || (su_is_nsp() && (g_su_keys.empty() || g_su_target.empty())))) {
        MessageBoxW(g_wnd, L"Pick your game (NSP file, or a folder with exefs\\ and romfs\\), and for an NSP your prod.keys.",
                    L"TMSRecomp", MB_ICONWARNING);
        return;
    }
    // shortcut: a dev tree without setup\python uses python.exe from PATH
    // -X utf8: prints and files in UTF-8 (read back as UTF-8 = this process's code page, app.manifest); without it a
    // folder like "Tokyo Mirage Sessions <U+266F>FE" stopped setup at its first print (Reddit 11-28, cp1252)
    std::wstring cmd = L"\"" + (fs::exists(py) ? py.wstring() : std::wstring(L"python.exe")) + L"\" -I -X utf8 -u \"" +
                       script.wstring() + L"\"";
    if (dlss_only) cmd += L" --dlss-only";
    else if (!extra.empty()) cmd += L" " + extra + L" --data \"" + g_data.wstring() + L"\"";
    else {
        cmd += su_is_nsp() ? L" --nsp \"" + g_su_src + L"\" --keys \"" + g_su_keys + L"\" --data \"" + g_su_target + L"\""
                           : L" --folder \"" + g_su_src + L"\"";
        if (!g_su_shaders) cmd += L" --no-shaders";
        if (g_su_dlss) cmd += L" --dlss";
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi;
    // a job: Cancel (or closing the launcher) ends python and every tool it started (nstool, clang, translator)
    if (!g_su_job) {
        g_su_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_su_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        g_dir.c_str(), &si, &pi)) {
        CloseHandle(rd), CloseHandle(wr);
        MessageBoxW(g_wnd, L"Could not start the setup (Python).", L"TMSRecomp", MB_ICONERROR);
        return;
    }
    AssignProcessToJobObject(g_su_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess), CloseHandle(wr);
    {
        std::lock_guard<std::mutex> lk(g_su_lock);
        g_su = {};
        g_su.running = true, g_su.text = L"Starting…";
    }
    g_su_extra = !extra.empty();
    g_su_topic = g_page == 7 ? g_topic : 1;  // the Mods page installs the patch
    g_su_t0 = GetTickCount();
    std::thread(su_reader, rd).detach();
}

static void su_cancel() {
    if (g_su_job) TerminateJobObject(g_su_job, 1);  // the reader thread then sees the pipe close
}

static std::wstring pick_file(const wchar_t* filter, const wchar_t* title) {
    wchar_t b[MAX_PATH] = L"";
    OPENFILENAMEW o{sizeof o};
    o.hwndOwner = g_wnd, o.lpstrFilter = filter, o.lpstrFile = b, o.nMaxFile = MAX_PATH, o.lpstrTitle = title;
    o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? b : L"";
}

// The Explorer-style "Select Folder" dialog (address bar, search, quick access), same look as pick_file's.
static std::wstring pick_folder(const wchar_t* title) {
    std::wstring r;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return r;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(title);
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_wnd)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR b = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &b))) r = b, CoTaskMemFree(b);
        it->Release();
    }
    d->Release();
    return r;
}
#else
static pid_t g_su_pid = -1;
static void su_reader(int fd) {
    std::string buf;
    char b[4096];
    for (ssize_t n; (n = read(fd, b, sizeof b)) > 0;) {
        buf.append(b, (size_t)n);
        for (size_t e; (e = buf.find('\n')) != std::string::npos; buf.erase(0, e + 1)) {
            su_line(trim(buf.substr(0, e)));
            PostMessageW(g_wnd, WM_SETUP, 0, 0);
        }
    }
    close(fd);
    {
        std::lock_guard<std::mutex> lk(g_su_lock);
        g_su.running = false;
        if (!g_su.done && g_su.error.empty()) g_su.error = L"Setup stopped.";
    }
    PostMessageW(g_wnd, WM_SETUP, 1, 0);
}

// The setup script: setup/tools/setup/setup.py next to the launcher (a release), else the source tree's tools/ above a
// build folder. Python: setup/python/bin/python3 when shipped, else python3 from the system.
static fs::path setup_script() {
    for (const fs::path& p : {g_dir / "setup" / "tools" / "setup" / "setup.py", g_dir.parent_path() / "tools" / "setup" / "setup.py"})
        if (fs::exists(p)) return p;
    return {};
}
// extra: setup.py arguments of a Guide page install (--mod / --zip / --music "<path>"), into the data folder
static void su_start(bool dlss_only = false, const std::wstring& extra = L"") {
    const fs::path script = setup_script(), py = g_dir / "setup" / "python" / "bin" / "python3";
    if (script.empty()) {
        MessageBoxW(g_wnd, L"The setup tools (setup/ folder) are missing: unpack the whole TMSRecomp download again.",
                    L"TMSRecomp", MB_ICONERROR);
        return;
    }
    if (!dlss_only && extra.empty() && (g_su_src.empty() || (su_is_nsp() && (g_su_keys.empty() || g_su_target.empty())))) {
        MessageBoxW(g_wnd, L"Pick your game (NSP file, or a folder with exefs and romfs), and for an NSP your prod.keys.",
                    L"TMSRecomp", MB_ICONWARNING);
        return;
    }
    std::vector<std::string> a = {fs::exists(py) ? py.string() : "python3", "-I", "-u", script.string(), "--install", g_dir.string()};
    if (dlss_only) {
        a.push_back("--dlss-only");
    } else if (!extra.empty()) {  // "--flag \"path\""
        const std::string e = narrow(extra);
        const size_t q0 = e.find('"'), q1 = e.rfind('"');
        a.push_back(e.substr(0, e.find(' ')));
        a.push_back(q0 != std::string::npos && q1 > q0 ? e.substr(q0 + 1, q1 - q0 - 1) : e.substr(e.find(' ') + 1));
        a.insert(a.end(), {"--data", g_data.string()});
    } else {
        if (su_is_nsp()) a.insert(a.end(), {"--nsp", narrow(g_su_src), "--keys", narrow(g_su_keys), "--data", narrow(g_su_target)});
        else a.insert(a.end(), {"--folder", narrow(g_su_src)});
        if (!g_su_shaders) a.push_back("--no-shaders");
        if (g_su_dlss) a.push_back("--dlss");
    }
    int fd = -1;
    g_su_pid = spawn(a, g_dir, &fd, true);
    if (g_su_pid < 0) {
        MessageBoxW(g_wnd, L"Could not start the setup (python3).", L"TMSRecomp", MB_ICONERROR);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_su_lock);
        g_su = {};
        g_su.running = true, g_su.text = L"Starting…";
    }
    g_su_extra = !extra.empty();
    g_su_topic = g_page == 7 ? g_topic : 1;  // the Mods page installs the patch
    g_su_t0 = GetTickCount();
    std::thread(su_reader, fd).detach();
}

static void su_cancel() {
    if (g_su_pid > 0) kill(-g_su_pid, SIGTERM);  // the whole group: python and every tool it started
}

// File pickers: the desktop's own dialogs through SDL (portal or zenity), waited for here
struct DlgState { bool done = false; std::string path; std::vector<std::string> names, patterns; };
static void dlg_done(void* ud, const char* const* list, int) {
    auto* s = (DlgState*)ud;
    if (list && list[0]) s->path = list[0];
    s->done = true;
}
static std::wstring run_dialog(SDL_FileDialogType type, DlgState& st, const wchar_t* title, const std::vector<SDL_DialogFileFilter>& filters) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_wnd);
    const std::string t = narrow(title);
    SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_TITLE_STRING, t.c_str());
    if (!filters.empty()) {
        SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, (void*)filters.data());
        SDL_SetNumberProperty(p, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, (Sint64)filters.size());
    }
    SDL_ShowFileDialogWithProperties(type, dlg_done, &st, p);
    while (!st.done) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 50);  // the dialog answers through the event queue
    }
    SDL_DestroyProperties(p);
    g_need_redraw = true;
    return wide(st.path);
}
static std::wstring pick_file(const wchar_t* filter, const wchar_t* title) {
    DlgState st;
    for (const wchar_t* f = filter; *f;) {  // "Name\0*.ext;*.x\0..." pairs -> SDL's "ext;x"
        std::string name = narrow(f);
        f += wcslen(f) + 1;
        std::string pat = narrow(f), sdl;
        f += wcslen(f) + 1;
        for (size_t a = 0; a <= pat.size();) {
            size_t b = pat.find(';', a);
            if (b == std::string::npos) b = pat.size();
            std::string one = pat.substr(a, b - a);
            if (one.rfind("*.", 0) == 0) one = one.substr(2);
            if (one == "*") one = "*";
            sdl += (sdl.empty() ? "" : ";") + one;
            a = b + 1;
        }
        st.names.push_back(name), st.patterns.push_back(sdl);
    }
    std::vector<SDL_DialogFileFilter> filters;
    for (size_t k = 0; k < st.names.size(); k++) filters.push_back({st.names[k].c_str(), st.patterns[k].c_str()});
    return run_dialog(SDL_FILEDIALOG_OPENFILE, st, title, filters);
}
static std::wstring pick_folder(const wchar_t* title) {
    DlgState st;
    return run_dialog(SDL_FILEDIALOG_OPENFOLDER, st, title, {});
}
#endif


// Shortened path for a value line: the end matters most.
static std::wstring short_path(const std::wstring& v) { return v.size() > 70 ? L"\u2026" + v.substr(v.size() - 69) : v; }

// The Guide's installs (setup.py --mod / --zip / --music into the data folder; the game files stay as dumped).
static void install_patch() {
    if (auto p = pick_folder(L"The folder with the patch's .xdelta files (or a mod with a romfs folder)"); !p.empty())
        su_start(false, L"--mod \"" + p + L"\"");
}
static void install_zip() {
    if (auto p = pick_file(L"TMSRecomp ZIP\0*.zip\0", L"TMSRecomp-UHD or TMSRecomp-MovieHD ZIP"); !p.empty())
        su_start(false, L"--zip \"" + p + L"\"");
}
static void install_music() {
    if (auto p = pick_folder(L"A folder with .adx music files"); !p.empty()) su_start(false, L"--music \"" + p + L"\"");
}

// Progress / result of the last setup run at (x, y), width w. Caller holds g_su_lock. Returns the height used.
static float run_status(float x, float y, float w, const wchar_t* done) {
    if (g_su.running) {
        double k = g_su.total > 0 ? std::clamp(g_su.done_n / g_su.total, 0.0, 1.0) : 0;
        float pk = std::clamp((float)((g_su.step - 1 + k) / std::max(1, g_su.steps)), 0.f, 1.f);
        D2D1_RECT_F bar = R(x, y + 16, w - 140, 8);
        rounded(bar, 4, C(TRACK));
        rounded({bar.left, bar.top, bar.left + (bar.right - bar.left) * pk, bar.bottom}, 4, C(ACCENT));
        DWORD s = (GetTickCount() - g_su_t0) / 1000;
        wchar_t b[64];
        swprintf(b, 64, L"Step %d of %d  \u00b7  %u:%02u  \u00b7  ", g_su.step, g_su.steps, s / 60, s % 60);
        text(b + g_su.text, R(x, y + 30, w - 140, 22), f_small, C(MUTED));
        button(R(x + w - 124, y, 124, 40), L"Cancel", ID_SU_CANCEL, false, [] { su_cancel(); });
        return 60;
    }
    if (!g_su.error.empty()) {
        float h = text_h(g_su.error, f_small, w);
        text(g_su.error, R(x, y, w, h + 4), f_small, C(WARN), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return h + 12;
    }
    if (g_su.done) {
        text(done, R(x, y, w, 24), f_label, C(ACCENT));
        return 32;
    }
    return 0;
}

static void draw_setup(float x, float w) {
    std::lock_guard<std::mutex> lk(g_su_lock);
    page_header(L"Setup", L"Builds the game from your own copy. You do this once: about 5 to 15 minutes.");
    pill(game_ready() ? L"\u2713  Ready to play" : L"Not set up yet", x + w, 46, game_ready() ? ACCENT : WARN);
    const bool nsp = su_is_nsp(), idle = !g_su.running, picked = !g_su_src.empty();

    float y = 128;
    if (!g_su.running && !g_su_extra) {  // the last build's result, where it is seen
        float h = run_status(x, y, w, L"\u2713  Done. Press PLAY.");
        if (h) y += h + 12;
    }
    section(L"1   YOUR GAME", x, y, w);
    y += 38;
    y += row_with(L"Game", L"Tokyo Mirage Sessions #FE Encore, base game 1.0.0, dumped from your own Switch: the NSP file, "
                           L"or a folder you already extracted (with exefs and romfs in it).",
                  x, y, w, [&](D2D1_RECT_F r) {
                      if (!idle) return;
                      button(R(r.left, r.top, 174, 40), L"NSP file\u2026", ID_SU_SRC, !picked && !game_ready(), [] {
                          if (auto p = pick_file(L"Switch NSP\0*.nsp\0All files\0*.*\0", L"Your game's NSP"); !p.empty()) {
                              g_su_src = p;
                              if (g_su_target.empty()) g_su_target = (g_dir / "game").wstring();
                          }
                      });
                      button(R(r.left + 186, r.top, 174, 40), L"Extracted folder\u2026", ID_X + 0, false, [] {
                          if (auto p = pick_folder(L"Your extracted game: the folder with exefs and romfs"); !p.empty()) g_su_src = p;
                      });
                  }, picked ? L"\u2713  " + short_path(g_su_src) : L"");
    y += row_with(L"prod.keys",
                  nsp ? L"The keys file from your own console. Setup needs it to open the NSP."
                      : picked ? L"Not needed: an extracted folder is already decrypted."
                               : L"The keys file from your own console. Only needed for an NSP file.",
                  x, y, w, [&](D2D1_RECT_F r) {
                      if (idle && nsp)
                          button(R(r.left + 186, r.top, 174, 40), L"Browse\u2026", ID_SU_KEYS, g_su_keys.empty(), [] {
                              if (auto p = pick_file(L"Keys\0*.keys\0All files\0*.*\0", L"prod.keys"); !p.empty()) g_su_keys = p;
                          });
                  }, nsp && !g_su_keys.empty() ? L"\u2713  " + short_path(g_su_keys) : L"");
    if (nsp)
        y += row_with(L"Install to", L"Where the game files go: about 13 GB.", x, y, w, [&](D2D1_RECT_F r) {
                 if (idle)
                     button(R(r.left + 186, r.top, 174, 40), L"Change\u2026", ID_SU_TARGET, false, [] {
                         if (auto p = pick_folder(L"Where the game files go (about 13 GB)"); !p.empty()) g_su_target = p;
                     });
             }, short_path(g_su_target));

    section(L"2   OPTIONS", x, y + 14, w);
    y += 52;
    y += row_with(L"Precompile shaders", L"No stutter the first time an effect appears in the game. Adds about 4 minutes and "
                                         L"300 MB to the setup.",
                  x, y, w, [&](D2D1_RECT_F r) { toggle(r, g_su_shaders, ID_SU_SHADERS, [] { g_su_shaders = !g_su_shaders; }, idle); });
    const bool have_dlss = dlss_installed();
    y += row_with(L"NVIDIA DLSS",
                  have_dlss ? L"Installed. Turn it on under Display & FPS \u2192 Anti-aliasing (RTX graphics cards)."
                            : L"For RTX graphics cards. Not included in this download: when on, setup downloads it from "
                              L"NVIDIA's GitHub (59 MB, NVIDIA's own licence). Without it, FSR 3 works on every card.",
                  x, y, w, [&](D2D1_RECT_F r) {
                      toggle(r, g_su_dlss || have_dlss, ID_SU_DLSS, [] { g_su_dlss = !g_su_dlss; }, idle && !have_dlss);
                      if (idle && !have_dlss && g_su_dlss && game_ready())
                          button(R(r.right - 150, r.top, 150, 40), L"Download now", ID_SU_DLSS_RUN, true, [] { su_start(true); });
                  });

    y += 18;
    if (idle && game_ready()) {
        float bx = button_at(x, y, L"Set up again", ID_SU_RUN, false, [] { setup_go(); });
        text(L"Only needed when a new TMSRecomp version asks for it, or for another dump.", R(bx + 8, y, w, 42), f_small, C(MUTED));
        y += 56;
    } else if (idle) {
        text(L"Ready? Press SET UP at the bottom right. It takes 5 to 15 minutes: keep this window open.",
             R(x, y, w, 24), f_label, C(TEXT));
        y += 40;
    }

    section(L"3   EXTRAS (OPTIONAL)", x, y + 6, w);
    y += 46;
    static const wchar_t* ex[][2] = {{L"English patch", L"Translation and uncensor"}, {L"UHD textures", L"4\u00d7 sharper textures"},
                                     {L"4K movies", L"Story movies in 4K"}, {L"Battle music", L"Your own .adx tracks"}};
    const float tw = (w - 36) / 4;
    for (int k = 0; k < 4; k++) {
        D2D1_RECT_F r = R(x + k * (tw + 12), y, tw, 84);
        const bool h = hov(ID_X + 10 + k);
        rounded(r, 10, C(h ? HOVER : PANEL));
        outline(r, 10, C(h ? ACCENT : LINE, h ? 0.7f : 1));
        text(ex[k][0], R(r.left + 18, r.top + 16, tw - 36, 24), f_label, C(TEXT));
        text(ex[k][1], R(r.left + 18, r.top + 44, tw - 50, 22), f_small, C(MUTED));
        text(L"\u2192", R(r.right - 34, r.top + 44, 20, 22), f_label, C(h ? ACCENT : DIM));
        hit(r, ID_X + 10 + k, [k](float) { g_page = 7; g_topic = k + 1; });
    }
    g_bottom = std::max(g_bottom, y + 84);
}

// "Copy system info": one block for a bug report (version, Windows, CPU, RAM, GPU + driver from the last nx.log, the
// settings that change rendering). Paths are left out (they hold the Windows user name).
#ifdef _WIN32
static void copy_sysinfo() {
    std::string t = sysinfo_line() + "\r\n";
    std::ifstream lf(g_data / "nx.log");
    std::string gpu;
    for (std::string l; gpu.empty() && std::getline(lf, l);)
        if (auto k = l.find("vk: "); k != std::string::npos && l.find("(driver") != std::string::npos) gpu = trim(l.substr(k + 4));
    if (gpu.empty()) {
        DISPLAY_DEVICEW d{sizeof d};
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); i++)
            if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { gpu = narrow(d.DeviceString) + " (driver: see nx.log)"; break; }
    }
    t += "GPU: " + (gpu.empty() ? std::string("unknown") : gpu) + "\r\nSettings:";
    for (const char* k : {"fps", "res", "window", "present_mode", "fx_fxaa", "fsr3_quality", "dlss_preset", "msaa", "upscaler", "tex_pack",
                          "hdr", "movie_hd", "mods_disabled", "random_battle_bgm", "auto_30", "crowd_density", "shadow_detail", "weak_hint", "scene_res", "hdr_boost", "hdr_color"})
        if (auto v = get(k, ""); !v.empty()) t += std::string(" ") + k + "=" + v;
    int on = 0;
    for (auto& m : g_mods) on += m.on;
    t += " mods_on=" + std::to_string(on) + "\r\n";
    const std::wstring w = wide(t);
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(h), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
    MessageBoxW(g_wnd, (L"Copied. Paste it into your bug report, and attach nx.log.\n\n" + w).c_str(), L"System info", MB_ICONINFORMATION);
}
#else
static void copy_sysinfo() {
    std::string t = sysinfo_line() + "\n";
    std::ifstream lf(g_data / "nx.log");
    std::string gpu;
    for (std::string l; gpu.empty() && std::getline(lf, l);)
        if (auto k = l.find("vk: "); k != std::string::npos && l.find("(driver") != std::string::npos) gpu = trim(l.substr(k + 4));
    t += "GPU: " + (gpu.empty() ? std::string("unknown (start the game once)") : gpu) + "\nSettings:";
    for (const char* k : {"fps", "res", "window", "present_mode", "fx_fxaa", "fsr3_quality", "dlss_preset", "msaa", "upscaler", "tex_pack",
                          "hdr", "movie_hd", "mods_disabled", "random_battle_bgm", "auto_30", "crowd_density", "shadow_detail", "weak_hint", "scene_res", "hdr_boost", "hdr_color"})
        if (auto v = get(k, ""); !v.empty()) t += std::string(" ") + k + "=" + v;
    int on = 0;
    for (auto& m : g_mods) on += m.on;
    t += " mods_on=" + std::to_string(on) + "\n";
    SDL_SetClipboardText(t.c_str());
    MessageBoxW(g_wnd, (L"Copied. Paste it into your bug report, and attach nx.log.\n\n" + wide(t)).c_str(), L"System info", MB_ICONINFORMATION);
}
#endif

// ------------------------------------------------------------------------------------------------ Guide page
// First-time help: one topic at a time, numbered steps and the buttons that do them (installs run setup.py --mod /
// --zip / --music with the Setup page's progress machinery; results go into the data folder, the game files stay as dumped).
struct Topic { const wchar_t* name; const wchar_t* title; const wchar_t* intro; const wchar_t* steps[3]; const wchar_t* tip; };
static const Topic k_topics[] = {
    {L"Get started", L"Set up the game",
     L"All you need is your own copy of the game. No emulator and no Switch firmware.",
     {L"Dump the game from your own Switch: the base game 1.0.0 as an NSP file, plus your console's prod.keys.",
      L"Open Setup, pick both files and press Set up game files. It takes 5 to 15 minutes, once.",
      L"Press PLAY. In the game, F1 (or L3 + R3 on a controller) opens the settings menu."},
     L"Already extracted the game (a folder with exefs and romfs)? Pick that folder instead: then no keys are needed."},
    {L"English patch", L"English patch & uncensor",
     L"Fan patches come as .xdelta files, one per game archive. They install as a mod: your game files are never changed. "
     L"A newer version of a patch replaces the older one by itself.",
     {L"Download the patch and unzip it, so its .xdelta files sit in one folder. That folder's name becomes the mod's name.",
      L"Press Install patch and pick that folder. The full English / uncensor patch takes about 20 minutes and up to 9 GB.",
      L"Done: the mod is on. The Mods page turns it off and on."},
     L"A mod that already has a romfs folder installs the same way: pick the folder that holds romfs."},
    {L"UHD textures", L"UHD textures",
     L"The game's own textures, upscaled 4\u00d7. Needs about 12 GB of disk and a graphics card with 4 GB of memory or more.",
     {L"Download the TMSRecomp-UHD ZIP (link on the release page).",
      L"Press Install ZIP and pick the file.",
      L"Graphics \u2192 UHD texture pack = On, then start the game."},
     L"Your own texture mods go in <game files>\\mods\\<name>\\textures (see README.txt)."},
    {L"4K movies", L"4K movies",
     L"Story movies and camp backgrounds in 4K, upscaled from the game's 720p videos (also the uncensor patch's movies).",
     {L"Download the TMSRecomp-MovieHD ZIP (link on the release page).",
      L"Press Install ZIP and pick the file. It needs about 7 GB.",
      L"That's all: the game plays the 4K movies on its own."},
     L"A CPU too slow for 4K video plays the 720p movie instead (Display & FPS → 4K movies: 720p when too slow). To turn them off: movie_hd = 0 in settings.ini."},
    {L"Battle music", L"Your own battle music",
     L"Play your own .adx tracks in random battles, for example from a battle music mod.",
     {L"Press Add music folder and pick a folder of .adx files.",
      L"Game \u2192 Random battle music = Custom (only yours) or Mixed (yours and the game's).",
      L"Start a battle."},
     L"Name files like \u201c1_P5R_Last Surprise.adx\u201d. A title with \u201cVictory\u201d plays after battles of the same series. "
     L"Encrypted .adx files need their key as custom_bgm_key in settings.ini."},
    {L"Controls & help", L"Controls & help",
     L"A controller works as on the Switch: the right face button is A. To quit, open the Y menu and press \u2212.",
     {}, L"Found a bug? Press Copy system info and paste it into a GitHub issue, with nx.log attached. Never post game files or keys."},
};

static void draw_guide(float x, float w) {
    page_header(L"Guide", L"New here? Start with Get started. The buttons in each topic do the work for you.");
    for (int k = 0; k < (int)std::size(k_topics); k++) {
        D2D1_RECT_F r = R(x, 128 + k * 50.f, 196, 44);
        bool sel = k == g_topic, h = hov(ID_G_TOPIC + k);
        if (sel || h) rounded(r, 8, C(sel ? PANEL : 0x141c18));
        if (sel) outline(r, 8, C(LINE)), rounded(R(r.left, r.top + 11, 3, 22), 1.5f, C(ACCENT));
        text(k_topics[k].name, {r.left + 18, r.top, r.right, r.bottom}, f_label, C(sel ? 0xffffff : h ? TEXT : MUTED));
        hit(r, ID_G_TOPIC + k, [k](float) { g_topic = k; });
    }
    const Topic& t = k_topics[g_topic];
    const float cx = x + 216, cw = w - 216, px = cx + 30, pw = cw - 60;
    std::lock_guard<std::mutex> lk(g_su_lock);
    {  // measure, then draw the card behind the content
        float h = 22 + 42 + text_h(t.intro, f_body, pw) + 24;
        if (g_topic == 5) h += 5 * 38 + 16;
        else for (auto st : t.steps) h += std::max(26.f, text_h(st, f_body, pw - 46)) + 18;
        h += (g_topic == 5 ? 0 : 4) + 56 + 8 + text_h(t.tip, f_small, pw - 40) + 24 + 28;
        if (g_su_extra && g_su_topic == g_topic) h += g_su.running ? 60 : !g_su.error.empty() ? text_h(g_su.error, f_small, pw) + 12 : g_su.done ? 32 : 0;
        card(R(cx, 128, cw, h));
    }
    float y = 150;
    text(t.title, R(px, y, pw, 34), f_h2, C(0xffffff));
    y += 42;
    float ih = text_h(t.intro, f_body, pw);
    text(t.intro, R(px, y, pw, ih + 4), f_body, C(MUTED), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    y += ih + 24;
    if (g_topic == 5) {  // controls: key caps in two columns
        static const wchar_t* keys[][2] = {{L"F1  \u00b7  L3 + R3", L"Settings menu"}, {L"F12", L"Screenshot"},
                                           {L"F11  \u00b7  Alt + Enter", L"Fullscreen"}, {L"W A S D", L"Move"},
                                           {L"Arrow keys", L"D-pad"},           {L"K  \u00b7  Enter", L"A"},
                                           {L"J  \u00b7  Backspace", L"B"},      {L"I  /  U", L"X  /  Y"},
                                           {L"Space  /  Tab", L"+  /  \u2212"}};
        const float colw = pw / 2;
        for (int k = 0; k < 9; k++) {
            float kx = px + (k < 5 ? 0 : colw), ky = y + (k % 5) * 38;
            float kw = text_w(keys[k][0], f_small) + 22;
            D2D1_RECT_F kr = R(kx, ky, kw, 28);
            rounded(kr, 6, C(HOVER));
            outline(kr, 6, C(EDGE_HI));
            text(keys[k][0], kr, f_small, C(TEXT), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text(keys[k][1], R(kx + 156, ky, colw - 160, 28), f_body, C(MUTED));
        }
        y += 5 * 38 + 16;
    } else {
        for (int k = 0; k < 3; k++) {
            float sh = text_h(t.steps[k], f_body, pw - 46);
            badge(px + 13, y + 12, k + 1);
            text(t.steps[k], R(px + 46, y + 1, pw - 46, sh + 4), f_body, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            y += std::max(26.f, sh) + 18;
        }
        y += 4;
    }
    const bool idle = !g_su.running, ready = is_data(g_data) && fs::exists(g_data / "romfs");
    float bx = px;
    if (g_topic >= 1 && g_topic <= 4 && !ready)
        bx = button_at(bx, y, L"Set up the game first", ID_G_BTN + 0, true, [] { g_page = 6; });
    else switch (g_topic) {
        case 0: bx = button_at(bx, y, game_ready() ? L"Open Setup" : L"Go to Setup", ID_G_BTN + 0, !game_ready(), [] { g_page = 6; }); break;
        case 1:
            if (idle) bx = button_at(bx, y, L"Install patch\u2026", ID_G_BTN + 1, true, [] { install_patch(); });
            bx = button_at(bx, y, L"Open Mods", ID_G_BTN + 2, false, [] { g_page = 4; scan_mods(); });
            break;
        case 2:
        case 3:
            if (idle) bx = button_at(bx, y, L"Install ZIP\u2026", ID_G_BTN + 3, true, [] { install_zip(); });
            if (g_topic == 2) bx = button_at(bx, y, L"Open Graphics", ID_G_BTN + 4, false, [] { g_page = 1; });
            break;
        case 4:
            if (idle) bx = button_at(bx, y, L"Add music folder\u2026", ID_G_BTN + 5, true, [] { install_music(); });
            bx = button_at(bx, y, L"Open music folder", ID_G_BTN + 6, false, [] { open_path(g_data / "music"); });
            bx = button_at(bx, y, L"Open Game page", ID_G_BTN + 7, false, [] { g_page = 3; });
            break;
        case 5:
            bx = button_at(bx, y, L"Copy system info", ID_G_BTN + 10, true, [] { copy_sysinfo(); });
            bx = button_at(bx, y, L"Open last log", ID_G_BTN + 8, false, [] {
                ShellExecuteW(g_wnd, L"open", (g_data / "nx.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            });
            break;
        }
    y += 56;
    if (g_su_extra && g_su_topic == g_topic) y += run_status(px, y, pw, L"\u2713  Installed.");
    // tip at the bottom of the card
    float th = text_h(t.tip, f_small, pw - 40), ty = y + 8;
    D2D1_RECT_F tr = R(px, ty, pw, th + 24);
    rounded(tr, 8, C(0x111a15));
    rounded(R(tr.left, tr.top + 10, 3, th + 4), 1.5f, C(ACCENT, 0.8f));
    text(t.tip, R(tr.left + 20, tr.top + 12, pw - 40, th + 4), f_small, C(MUTED), DWRITE_TEXT_ALIGNMENT_LEADING,
         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

// Starts the game build when the Setup page has what it needs, else says what is missing.
static void setup_go() {
    if (g_su_src.empty() || (su_is_nsp() && g_su_keys.empty())) {
        g_page = 6;
        MessageBoxW(g_wnd, g_su_src.empty() ? L"First pick your game: the NSP file or your extracted folder."
                                            : L"An NSP file also needs prod.keys from your console.",
                    L"Setup", MB_ICONINFORMATION);
        return;
    }
    su_start();
}

// The footer's big button: SET UP until the game is built, CANCEL while setup runs, then PLAY. Setup progress shows
// here on every page.
static void draw_footer() {
    D2D1_RECT_F f = R(SIDE, H - FOOT, W - SIDE, FOOT);
    fill(f, C(0x0d1210));
    fill(R(SIDE, H - FOOT, W - SIDE, 1), C(LINE));
    bool running;
    {
        std::lock_guard<std::mutex> lk(g_su_lock);
        running = g_su.running;
        if (running) {
            double k = g_su.total > 0 ? std::clamp(g_su.done_n / g_su.total, 0.0, 1.0) : 0;
            float pk = std::clamp((float)((g_su.step - 1 + k) / std::max(1, g_su.steps)), 0.f, 1.f);
            D2D1_RECT_F bar = R(SIDE + 48, H - FOOT + 30, 470, 6);
            rounded(bar, 3, C(TRACK));
            rounded({bar.left, bar.top, bar.left + (bar.right - bar.left) * pk, bar.bottom}, 3, C(ACCENT));
            DWORD sec = (GetTickCount() - g_su_t0) / 1000;
            wchar_t b[64];
            swprintf(b, 64, L"%d / %d  \u00b7  %u:%02u  \u00b7  ", g_su.step, g_su.steps, sec / 60, sec % 60);
            text(b + g_su.text, R(SIDE + 48, H - FOOT + 42, 470, 22), f_small, C(MUTED));
        }
    }
    if (!running) text(status_line(), R(SIDE + 48, H - FOOT, 520, FOOT), f_small, C(MUTED));
    button(R(W - 252 - 140, H - FOOT + 20, 124, 44), g_dirty ? L"Save" : L"Saved", ID_SAVE, false, [] { save(); });
    const D2D1_RECT_F big = R(W - 252, H - FOOT + 14, 220, 56);
    if (running) button(big, L"CANCEL", ID_SU_CANCEL, false, [] { su_cancel(); }, f_play);
    else if (!game_ready()) button(big, L"SET UP", ID_PLAY, true, [] { setup_go(); }, f_play);
    else button(big, L"PLAY", ID_PLAY, true, [] { play(); }, f_play);
}

static void draw_page_body();
static void draw_page() {
    g_clip = g_page >= 1 && g_page <= 6;
    float& sc = g_scroll[g_page];
    if (!g_clip) { g_max_scroll = 0; draw_page_body(); return; }
    g_bottom = 0;
    g_rt->PushAxisAlignedClip(R(SIDE, 0, W - SIDE, H - FOOT), D2D1_ANTIALIAS_MODE_ALIASED);
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -sc));
    draw_page_body();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->PopAxisAlignedClip();
    g_clip = false;
    const float view = H - FOOT, content = g_bottom + 24;
    g_max_scroll = std::max(0.f, content - view);
    if (sc > g_max_scroll) sc = g_max_scroll;
    if (g_max_scroll > 0) {  // scrollbar: the mouse wheel scrolls, the thumb can be dragged (GitHub #14)
        float th = view * view / content, ty = sc / g_max_scroll * (view - th - 16) + 8;
        rounded(R(W - 10, ty, 4, th), 2, C(EDGE_HI));
    }
}
// Scrollbar drag (GitHub #14: a broken mouse wheel left no way to scroll): a press in the 18 px strip at the right edge
// grabs the thumb (on the track: the thumb jumps there first), moving scrolls, release ends it.
static float g_sb_grab = -1;  // drag: the press's offset into the thumb; -1 = no drag
static void sb_thumb(float& th, float& ty, float& travel) {
    const float view = H - FOOT, content = view + g_max_scroll;
    th = view * view / content, travel = view - th - 16, ty = g_scroll[g_page] / g_max_scroll * travel + 8;
}
static void sb_move(float y) {
    float th, ty, travel;
    sb_thumb(th, ty, travel);
    g_scroll[g_page] = std::clamp((y - g_sb_grab - 8) / travel * g_max_scroll, 0.f, g_max_scroll);
    g_open = -1, g_hover = -1;
}
static bool sb_press(float x, float y) {  // true: the press was on the scrollbar
    if (g_max_scroll <= 0 || x < W - 18 || y >= H - FOOT) return false;
    float th, ty, travel;
    sb_thumb(th, ty, travel);
    g_sb_grab = y >= ty && y <= ty + th ? y - ty : th / 2;
    sb_move(y);
    return true;
}

// ------------------------------------------------------------------------------------------------ key bindings
// Game page: three key slots per Switch button / stick direction (settings.h k_key_targets). Click a slot, then press a
// key or a mouse button (or turn the wheel); Esc cancels, Delete clears the slot.
static int g_cap = -1;  // the slot waiting for a key: target * 3 + slot
static std::vector<std::string> key_slots(size_t t) {
    std::string v = get((std::string("key_") + k_key_targets[t].name).c_str(), k_key_targets[t].def) + ",";
    std::vector<std::string> out;
    for (size_t a = 0, b; (b = v.find(',', a)) != std::string::npos && out.size() < 3; a = b + 1)
        if (std::string n = trim(v.substr(a, b - a)); !n.empty() && _stricmp(n.c_str(), "none")) out.push_back(n);
    out.resize(3);
    return out;
}
static void set_slot(int cap, const std::string& name) {
    const size_t t = cap / 3;
    auto v = key_slots(t);
    for (auto& o : v) if (!name.empty() && !_stricmp(o.c_str(), name.c_str())) o.clear();  // no key twice in one row
    v[cap % 3] = name;
    std::string out;
    for (auto& o : v) if (!o.empty()) out += (out.empty() ? "" : ", ") + o;
    set((std::string("key_") + k_key_targets[t].name).c_str(), out.empty() ? "none" : out);
}
#ifdef _WIN32
// a key as SDL names it (what the game reads back with SDL_GetScancodeFromName)
static std::string key_name(WPARAM vk, LPARAM lp) {
    const bool ext = (lp >> 24) & 1;
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Keypad " + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_RETURN: return ext ? "Keypad Enter" : "Return";
    case VK_SPACE: return "Space"; case VK_TAB: return "Tab"; case VK_BACK: return "Backspace";
    case VK_SHIFT: return MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? "Right Shift" : "Left Shift";
    case VK_CONTROL: return ext ? "Right Ctrl" : "Left Ctrl";
    case VK_MENU: return ext ? "Right Alt" : "Left Alt";
    case VK_UP: return "Up"; case VK_DOWN: return "Down"; case VK_LEFT: return "Left"; case VK_RIGHT: return "Right";
    case VK_INSERT: return "Insert"; case VK_HOME: return "Home"; case VK_END: return "End";
    case VK_PRIOR: return "PageUp"; case VK_NEXT: return "PageDown"; case VK_CAPITAL: return "CapsLock";
    case VK_OEM_MINUS: return "-"; case VK_OEM_PLUS: return "="; case VK_OEM_4: return "["; case VK_OEM_6: return "]";
    case VK_OEM_5: return "\\"; case VK_OEM_1: return ";"; case VK_OEM_7: return "'"; case VK_OEM_3: return "`";
    case VK_OEM_COMMA: return ","; case VK_OEM_PERIOD: return "."; case VK_OEM_2: return "/";
    case VK_MULTIPLY: return "Keypad *"; case VK_ADD: return "Keypad +"; case VK_SUBTRACT: return "Keypad -";
    case VK_DECIMAL: return "Keypad ."; case VK_DIVIDE: return "Keypad /";
    }
    return "";
}
#endif
static float draw_keybinds(float x, float y, float w) {
    const float cw = 150, gap = 10, c0 = x + w - 3 * cw - 2 * gap;
    for (size_t t = 0; t < std::size(k_key_targets); t++, y += 44) {
        text(k_key_targets[t].label, R(x, y, c0 - x - 12, 36), f_label, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        auto v = key_slots(t);
        for (int k = 0; k < 3; k++) {
            const int cap = (int)t * 3 + k, id = ID_KEY + cap;
            D2D1_RECT_F r = R(c0 + k * (cw + gap), y, cw, 36);
            const bool h = hov(id), on = g_cap == cap;
            rounded(r, 4, C(on ? 0x1c2621 : h ? HOVER : PANEL));
            outline(r, 4, C(on ? ACCENT : h ? EDGE_HI : LINE));
            text(on ? std::wstring(L"Press a key\u2026") : v[k].empty() ? std::wstring(L"\u2014") : wide(v[k]), r, f_small,
                 C(on ? ACCENT : v[k].empty() ? DIM : TEXT), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            hit(r, id, [cap](float) { g_cap = g_cap == cap ? -1 : cap; });
        }
    }
    y += 6;
    button_at(x, y, L"Reset keys to defaults", ID_X + 30, false, [] {
        g_cap = -1;
        for (auto& kt : k_key_targets) set((std::string("key_") + kt.name).c_str(), kt.def);
    });
    text(L"Click a slot, then press a key or mouse button. Esc cancels, Delete clears it. Pads keep working as before.",
         R(x, y + 52, w, 22), f_small, C(DIM));
    return y + 80;
}

// Game page: controller buttons as a compact grid, two columns; < > steps through the pad buttons. The value shown is
// what the button really is (Default resolves through Button layout); taking another row's button swaps the two rows.
static std::string pad_effective(int i) {
    std::string v = value_of(i);
    if (v != "default") return v;
    static const char* sw[] = {"east", "south", "north", "west", "lb", "rb", "lt", "rt", "start", "back", "ls", "rs"};
    static const char* xb[] = {"south", "east", "west", "north", "lb", "rb", "lt", "rt", "start", "back", "ls", "rs"};
    return (value_of(BTNLAYOUT) == "xbox" ? xb : sw)[i - PAD_A];
}
static void pad_step(int i, int d) {
    const std::string old = pad_effective(i);
    int k = 0;
    for (size_t o = 1; o < k_pad_opts.size(); o++) if (old == k_pad_opts[o].v) k = (int)o;
    const int n = (int)k_pad_opts.size() - 1;  // options 1..n (Default is only the unset state)
    k = (k - 1 + d + n) % n + 1;
    const std::string nv = k_pad_opts[k].v;
    for (int j = PAD_A; j <= PAD_R3; j++)
        if (j != i && nv != "none" && pad_effective(j) == nv) set(k_set[j].key, old);  // swap
    set(k_set[i].key, nv);
}
static float draw_padbinds(float x, float y, float w) {
    const float colw = (w - 24) / 2, lw = 60, bh = 36;
    for (int i = PAD_A; i <= PAD_R3; i++) {
        const int c = (i - PAD_A) % 2, r = (i - PAD_A) / 2;
        const float cx = x + c * (colw + 24), cy = y + r * 44;
        text(k_set[i].name, R(cx, cy, lw, bh), f_label, C(TEXT), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const std::string v = pad_effective(i);
        std::wstring label = L"?";
        for (auto& o : k_pad_opts) if (v == o.v) label = o.label;
        const D2D1_RECT_F box = R(cx + lw, cy, colw - lw, bh);
        rounded(box, 4, C(PANEL));
        outline(box, 4, C(LINE));
        text(label, box, f_label, C(v == "none" ? DIM : value_of(i) == "default" ? MUTED : TEXT), DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        for (int d : {-1, 1}) {
            const int id = ID_KEY + 900 + (i - PAD_A) * 2 + (d > 0);
            const D2D1_RECT_F a = d < 0 ? R(box.left, box.top, 40, bh) : R(box.right - 40, box.top, 40, bh);
            if (hov(id)) rounded(a, 4, C(HOVER));
            text(d < 0 ? L"\u2039" : L"\u203a", a, f_label, C(hov(id) ? ACCENT : MUTED), DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            hit(a, id, [i, d](float) { pad_step(i, d); });
        }
    }
    y += 6 * 44 + 6;
    button_at(x, y, L"Reset controller buttons", ID_X + 31, false, [] {
        for (int i = PAD_A; i <= PAD_R3; i++) set(k_set[i].key, "default");
    });
    text(L"\u2039 \u203a picks the pad button for each Switch button; taking one another button has swaps the two. Grey = "
         L"Button layout's choice. Restart the game after a change. On-screen prompts still follow Button layout.",
         R(x, y + 52, w, 40), f_small, C(DIM));
    return y + 100;
}

static void draw_page_body() {
    float x = SIDE + 48, w = W - SIDE - 96;
    switch (g_page) {
    case 0: {  // Play: hero, quick settings
        draw_hero(R(SIDE, 0, W - SIDE, 360));
        text(L"QUICK SETTINGS", R(x, 330, 400, 20), f_caps, C(MUTED), DWRITE_TEXT_ALIGNMENT_LEADING,
             DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 2.2f);
        fill(R(x, 356, 28, 2), C(ACCENT));
        int quick[] = {FPS, RES, WINDOW, PRESENT};
        for (int k = 0; k < 4; k++) {
            float cx = x + (k % 2) * (w / 2 + 12), cy = 384 + (k / 2) * 92, cw = w / 2 - 12;
            text(k_set[quick[k]].name, R(cx, cy, cw, 22), f_small, C(MUTED));
            control(quick[k], R(cx, cy + 26, cw, 42));
        }
        if (!game_ready())
            text(L"The game is not set up yet: see the Setup page.", R(x, 570, w, 22), f_small, C(WARN));
        break;
    }
    case 1:
        page_header(L"Graphics", L"How the 3D image is rendered. Stored in settings.ini next to the game.");
    {
        float y = 128;
        for (int i : {RES, UPSCALER, FSR3Q, DLSSP, MSAA, POST, TEXPACK}) y += row(i, x, y, w);
        break;
    }
    case 2:
        page_header(L"Display & FPS", L"Frame rate, window and presentation.");
    {
        section(L"PRESENTATION", x, 128, w);
        float y = 166;
        for (int i : {FPS, AUTO30, WINDOW, ASPECT, UWFILL, PRESENT}) y += row(i, x, y, w);
        section(L"IMAGE", x, y + 8, w);
        y += 44;
        for (int i : {SHARPEN, FXAA, SCENERES, HDR, HDRBOOST, HDRCOLOR, HDRPAPER, HDRPEAK, CROWD, SHADOWD, MOVIEFB, MOVIEGPU}) y += row(i, x, y, w);
        break;
    }
    case 3:
        page_header(L"Game", L"Changes to how the game plays and sounds.");
    {
        section(L"LANGUAGE", x, 128, w);
        float y = 166 + row(LANGUAGE, x, 166, w);
        section(L"FIELD", x, y + 8, w);
        y += 46;
        y += row(WALK, x, y, w);
        for (int i : {MAPREV, FIELDCOS, NGPREQ}) y += row(i, x, y, w);
        section(L"BATTLE", x, y + 8, w);
        y += 46;
        for (int i : {ALLDIFF, RESMARK, WEAKHINT, BTLBUTTONS}) y += row(i, x, y, w);
        section(L"CONTROLS", x, y + 8, w);
        y += 46;
        y += row(PADICONS, x, y, w);
        y += row(BTNLAYOUT, x, y, w);
        section(L"CONTROLLER BUTTONS", x, y + 8, w);
        y += 46;
        y = draw_padbinds(x, y, w);
        section(L"KEYBOARD & MOUSE", x, y + 8, w);
        y += 46;
        for (int i : {MOUSECAM, MOUSESENS, MOUSEINV}) y += row(i, x, y, w);
        y = draw_keybinds(x, y + 10, w);
        section(L"AUDIO", x, y + 8, w);
        y += 46;
        for (int i : {MUTE, BGM}) y += row(i, x, y, w);
        break;
    }
    case 4: {
        page_header(L"Mods", L"Mods change the game's files without touching your own copy. Turn each one on or off.");
        std::lock_guard<std::mutex> lk(g_su_lock);
        const bool ready = is_data(g_data) && fs::exists(g_data / "romfs"), idle = !g_su.running;
        float y = 128;
        if (g_mods.empty()) {  // empty state: what a mod is and the one button that adds one
            D2D1_RECT_F c = R(x, y, w, 330);
            card(c);
            const float mx = (c.left + c.right) / 2;
            g_brush->SetColor(C(ACCENT, 0.12f));
            g_rt->FillEllipse({{mx, c.top + 74}, 34, 34}, g_brush);
            g_brush->SetColor(C(ACCENT));
            g_rt->DrawLine({mx - 12, c.top + 74}, {mx + 12, c.top + 74}, g_brush, 3);
            g_rt->DrawLine({mx, c.top + 62}, {mx, c.top + 86}, g_brush, 3);
            text(L"No mods installed yet", R(c.left, c.top + 124, w, 34), f_h2, C(0xffffff), DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"Install a fan patch, like the English / uncensor patch, from its folder of .xdelta files.",
                 R(c.left + 60, c.top + 162, w - 120, 24), f_body, C(MUTED), DWRITE_TEXT_ALIGNMENT_CENTER);
            const std::wstring a = ready ? L"Install patch\u2026" : L"Set up the game first", b = L"Open mods folder";
            const float dw = ready ? text_w(L"Dump game files…", f_btn) + 56 : 0;  // third button (loose-file mods)
            float aw = text_w(a, f_btn) + 44, bw = text_w(b, f_btn) + 44, bx = mx - (aw + 12 + bw + dw) / 2;
            if (idle) bx = button_at(bx, c.top + 212, a, ID_X + 20, true, [ready] { if (ready) install_patch(); else g_page = 6; });
            bx = button_at(bx, c.top + 212, b, ID_OPEN_MODS, false, [] { open_path(g_data / "mods"); });
            if (ready) button_at(bx, c.top + 212, L"Dump game files\u2026", ID_DUMP, false, [] { dump_files(); });
            run_status(c.left + 40, c.top + 268, w - 80, L"\u2713  Installed.");
            break;
        }
        section(L"INSTALLED", x, y, w);
        y += 40;
        for (size_t k = 0; k < g_mods.size(); k++, y += 86) {
            Mod& m = g_mods[k];
            D2D1_RECT_F c = R(x, y, w, 74);
            const bool h = hov(ID_MOD + (int)k);
            rounded(c, 10, C(h ? HOVER : PANEL));
            outline(c, 10, C(m.on ? 0x2f5a44 : LINE));
            text(pretty(m.name), R(x + 22, y + 13, w - 200, 26), f_label, C(m.on ? TEXT : MUTED));
            text(std::to_wstring(m.files) + (m.files == 1 ? L" file" : L" files") + L"  \u00b7  " + size_text(m.bytes) +
                     L"  \u00b7  mods\\" + wide(m.name),
                 R(x + 22, y + 40, w - 200, 22), f_small, C(DIM));
            toggle(R(x + w - 110, y, 90, 74), m.on, ID_MOD + (int)k, [k] { g_mods[k].on = !g_mods[k].on; store_mods(); }, true);
            hit(c, ID_MOD + (int)k, [k](float) { g_mods[k].on = !g_mods[k].on; store_mods(); });
        }
        y += 6;
        float bx = x;
        if (idle) bx = button_at(bx, y, ready ? L"Install patch\u2026" : L"Set up the game first", ID_X + 20, false,
                                 [ready] { if (ready) install_patch(); else g_page = 6; });
        bx = button_at(bx, y, L"Open mods folder", ID_OPEN_MODS, false, [] { open_path(g_data / "mods"); });
        if (ready) button_at(bx, y, L"Dump game files\u2026", ID_DUMP, false, [] { dump_files(); });
        y += 58;
        y += run_status(x, y, w, L"\u2713  Installed.");
        text(L"When two mods change the same file, the one earlier in A\u2013Z order wins.", R(x, y, w, 22), f_small, C(DIM));
        y += 22;
        text(L"Make your own: Dump game files, copy a file to mods\\<name>\\cpk\\<pack>\\<same path>, edit the copy.",
             R(x, y, w, 22), f_small, C(DIM));
        g_bottom = y + 24;
        break;
    }
    case 5: {
        page_header(L"Advanced", L"Folders, logs and tools for troubleshooting.");
        section(L"GAME FILES", x, 128, w);
        float y = 166;
        y += row_with(L"Game files folder", L"Where Setup put the game (link\\image.bin and romfs). Change it only after "
                                            L"moving that folder.",
                      x, y, w, [](D2D1_RECT_F r) {
                          button(R(r.left + 186, r.top, 174, 40), L"Change\u2026", ID_BROWSE, false, [] { browse(); });
                      }, is_data(g_data) ? L"\u2713  " + short_path(g_data.wstring()) : L"");
        if (!is_data(g_data)) text(L"Not found: run Setup, or pick the folder.", R(x, y - 30, w - 400, 22), f_small, C(WARN));
        section(L"HELP", x, y + 14, w);
        y += 52;
        y += row_with(L"Report a problem", L"Copies your PC and settings (no file paths) for a GitHub issue. Attach nx.log too.",
                      x, y, w, [](D2D1_RECT_F r) {
                          button(R(r.left, r.top, 174, 40), L"Copy system info", ID_G_BTN + 11, false, [] { copy_sysinfo(); });
                          button(R(r.left + 186, r.top, 174, 40), L"Open last log", ID_OPEN_LOG, false, [] {
                              ShellExecuteW(g_wnd, L"open", (g_data / "nx.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                          });
                      });
        y += row_with(L"Folders", L"The TMSRecomp folder (settings.ini) and your F12 screenshots.", x, y, w, [](D2D1_RECT_F r) {
            button(R(r.left, r.top, 174, 40), L"TMSRecomp folder", ID_OPEN_GAME, false, [] { open_path(g_dir); });
            button(R(r.left + 186, r.top, 174, 40), L"Screenshots", ID_OPEN_SHOTS, false, [] { open_path(g_data / "shots"); });
        });
        section(L"TEXTURES", x, y + 14, w);
        y += 52;
        y += row(TEXDUMP, x, y, w);
        text(L"Other settings can be added to settings.ini by hand (key = value): this window keeps them.",
             R(x, y + 8, w, 22), f_small, C(DIM));
        g_bottom = y + 32;
        break;
    }
    case 6: draw_setup(x, w); break;
    case 7: draw_guide(x, w); break;
    }
}

#ifdef _WIN32
static void paint() {
    if (!g_rt) return;
    g_hits.clear();
    g_rt->BeginDraw();
    g_rt->Clear(C(BG));
    draw_page();
    draw_sidebar();
    draw_footer();
    popup();
    whatsnew_dialog();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        if (g_hero) g_hero->Release(), g_hero = nullptr;
        g_brush->Release(), g_brush = nullptr;
        g_rt->Release(), g_rt = nullptr;
    }
}

static void create_target() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    float dpi = 96 * g_scale;
    g_d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, {}, dpi, dpi),
                                  D2D1::HwndRenderTargetProperties(g_wnd, {(UINT32)rc.right, (UINT32)rc.bottom}), &g_rt);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    g_rt->CreateSolidColorBrush(C(TEXT), &g_brush);
    load_hero();
}
#else
static bool g_launched = false;  // PLAY started the game: close
// Linux: the page drawn with cairo at the window's pixel size into an SDL texture
static SDL_Renderer* g_ren;
static SDL_Texture* g_tex;
static cairo_surface_t* g_surf;
static void paint() {
    if (!g_rt || !g_ren) return;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_wnd, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    if (!g_surf || cairo_image_surface_get_width(g_surf) != pw || cairo_image_surface_get_height(g_surf) != ph) {
        if (g_surf) cairo_surface_destroy(g_surf);
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
    }
    g_hits.clear();
    cairo_t* cr = cairo_create(g_surf);
    cairo_scale(cr, pw / W, ph / H);
    g_rt->cr = cr, g_rt->tx = g_rt->ty = 0;
    D2D1_COLOR_F bg = C(BG);
    cairo_set_source_rgb(cr, bg.r, bg.g, bg.b);
    cairo_paint(cr);
    draw_page();
    draw_sidebar();
    draw_footer();
    popup();
    whatsnew_dialog();
    cairo_destroy(cr);
    g_rt->cr = nullptr;
    cairo_surface_flush(g_surf);
    SDL_UpdateTexture(g_tex, nullptr, cairo_image_surface_get_data(g_surf), cairo_image_surface_get_stride(g_surf));
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, nullptr, nullptr);
    SDL_RenderPresent(g_ren);
}

static void create_target() {
    g_rt = new ID2D1HwndRenderTarget;
    g_brush = new ID2D1SolidColorBrush;
    load_hero();
}
#endif

// ------------------------------------------------------------------------------------------------ actions
static void browse() {
    std::wstring b = pick_folder(L"Select the game data folder (the one with link\\image.bin and romfs)");
    if (b.empty()) return;
    if (is_data(b)) {
        set("data", narrow(b));
        g_data = b;
        scan_mods();
        load_hero();
        apply_icon();
    } else {
        MessageBoxW(g_wnd, L"That folder has no link\\image.bin and romfs (see README).", L"TMSRecomp", MB_ICONWARNING);
    }
}

static void play() {
    // the wait for the game window below pumps messages: more PLAY clicks would start more games (user 11-28)
    static bool starting = false;
    if (starting) return;
    if (!game_ready() || setup_stale()) {
        g_page = 6;
        MessageBoxW(g_wnd, L"The game is not set up yet, or needs a rebuild after an update: Setup page.", L"TMSRecomp", MB_ICONWARNING);
        return;
    }
    if (g_dirty && !save()) {
        MessageBoxW(g_wnd, (L"Could not write " + g_ini.wstring()).c_str(), L"TMSRecomp", MB_ICONERROR);
        return;
    }
#ifdef _WIN32
    std::wstring exe = (g_dir / "tms.exe").wstring(), dir = g_dir.wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        MessageBoxW(g_wnd, (L"Could not start " + exe).c_str(), L"TMSRecomp", MB_ICONERROR);
        return;
    }
    // s125 (Augs: the game started behind other windows): Windows lets only the foreground process (us, the player just
    // clicked PLAY) bring a window to the front, and the game's window appears seconds later. So the launcher waits (up
    // to 30 s, messages pumped) for it, brings it to the front, then closes.
    starting = true;
    AllowSetForegroundWindow(pi.dwProcessId);
    struct Find { DWORD pid; HWND hw; } f{pi.dwProcessId, nullptr};
    for (int i = 0; i < 300 && !f.hw && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; i++) {
        MSG m;
        bool quit = false;  // WM_QUIT taken here must reach the main loop, else the launcher runs on without a window
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            if (m.message == WM_QUIT) quit = true;
            else TranslateMessage(&m), DispatchMessageW(&m);
        if (quit) { PostQuitMessage(0); break; }
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            auto* fd = (Find*)p;
            DWORD wp = 0;
            GetWindowThreadProcessId(h, &wp);
            if (wp != fd->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
            fd->hw = h;
            return FALSE;
        }, (LPARAM)&f);
        if (!f.hw) Sleep(100);
    }
    if (f.hw) SetForegroundWindow(f.hw);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g_wnd);
}
#else
    // the game in its own session, so it keeps running when the launcher closes
    if (spawn({(g_dir / "tms").string()}, g_dir, nullptr, true) < 0) {
        MessageBoxW(g_wnd, (L"Could not start " + (g_dir / "tms").wstring()).c_str(), L"TMSRecomp", MB_ICONERROR);
        return;
    }
    g_launched = true;  // the window loop closes the launcher without questions
}
#endif

static int hit_at(float x, float y) {
    for (size_t k = g_hits.size(); k-- > 0;)  // last drawn (popup) first
        if (x >= g_hits[k].r.left && x < g_hits[k].r.right && y >= g_hits[k].r.top && y < g_hits[k].r.bottom) return (int)k;
    return -1;
}

#ifdef _WIN32
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (!g_rt) create_target();
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
        if (g_sb_grab >= 0) { sb_move(y), redraw(); return 0; }
        if (g_drag >= 0) {
            const Setting& s = k_set[g_drag];
            float k = std::clamp((x - g_drag_track.left) / (g_drag_track.right - g_drag_track.left), 0.f, 1.f);
            char b[16];
            snprintf(b, 16, "%.1f", s.lo + std::round(k * (s.hi - s.lo) / s.step) * s.step);
            if (value_of(g_drag) != b) set(s.key, b), redraw();
            return 0;
        }
        int k = hit_at(x, y), id = k >= 0 ? g_hits[k].id : -1;
        SetCursor(LoadCursorW(nullptr, id >= 0 ? IDC_HAND : IDC_ARROW));
        if (id != g_hover) g_hover = id, redraw();
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; redraw(); return 0;
    case WM_MOUSEWHEEL:
        if (g_whatsnew) return 0;
        if (g_cap >= 0) {
            set_slot(g_cap, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? "WheelUp" : "WheelDown");
            g_cap = -1, redraw();
            return 0;
        }
        if (g_max_scroll > 0) {
            float& sc = g_scroll[g_page];
            sc = std::clamp(sc - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 60, 0.f, g_max_scroll);
            g_open = -1, g_hover = -1;
            redraw();
        }
        return 0;
    case WM_GESTURE: {  // touch screens (Legion Go 2, Reddit 11-26): a finger swipe pans the page like a phone
        GESTUREINFO gi{sizeof gi};
        if (!GetGestureInfo((HGESTUREINFO)lp, &gi)) break;
        static int last_y = 0;
        if (gi.dwID == GID_PAN) {
            POINT p{gi.ptsLocation.x, gi.ptsLocation.y};
            ScreenToClient(h, &p);
            if (!(gi.dwFlags & GF_BEGIN) && g_max_scroll > 0) {
                float& sc = g_scroll[g_page];
                sc = std::clamp(sc - (p.y - last_y) / g_scale, 0.f, g_max_scroll);
                g_open = -1, g_hover = -1;
                redraw();
            }
            last_y = p.y;
            CloseGestureInfoHandle((HGESTUREINFO)lp);
            return 0;
        }
        break;  // other gestures: DefWindowProc
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
        break;
    case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        if (g_cap >= 0) {
            set_slot(g_cap, m == WM_RBUTTONDOWN ? "Mouse2" : m == WM_MBUTTONDOWN ? "Mouse3" : HIWORD(wp) == XBUTTON1 ? "Mouse4" : "Mouse5");
            g_cap = -1, redraw();
        }
        return m == WM_XBUTTONDOWN ? TRUE : 0;
    case WM_LBUTTONDOWN: {
        if (g_cap >= 0) {  // a click on another slot moves the capture there; elsewhere it binds the left button
            float cx = GET_X_LPARAM(lp) / g_scale, cy = GET_Y_LPARAM(lp) / g_scale;
            int hk = hit_at(cx, cy);
            if (hk < 0 || g_hits[hk].id < ID_KEY || g_hits[hk].id == ID_KEY + g_cap) {
                set_slot(g_cap, "Mouse1");
                g_cap = -1, redraw();
                return 0;
            }
        }
        float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
        if (sb_press(x, y)) { SetCapture(h), redraw(); return 0; }
        int k = hit_at(x, y);
        bool was_open = g_open >= 0;
        if (k >= 0) {
            int id = g_hits[k].id;
            if (was_open && id != ID_SET + g_open && (id < ID_POP || id >= ID_MOD)) g_open = -1;
            auto f = g_hits[k].click;  // the hit list is rebuilt by the redraw
            f(x);
        } else g_open = -1;
        redraw();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sb_grab >= 0) g_sb_grab = -1, ReleaseCapture();
        if (g_drag >= 0) g_drag = -1, ReleaseCapture(), redraw();
        return 0;
    case WM_SYSKEYDOWN:  // Alt (and F10) arrive here
        if (g_cap < 0) break;
        [[fallthrough]];
    case WM_KEYDOWN:
        if (g_cap >= 0) {
            if (wp == VK_DELETE) set_slot(g_cap, "");
            else if (wp != VK_ESCAPE) {
                const std::string n = key_name(wp, lp);
                if (n.empty()) return 0;  // a key the game cannot read back: keep waiting
                set_slot(g_cap, n);
            }
            g_cap = -1, redraw();
            return 0;
        }
        if (wp == VK_ESCAPE && g_open >= 0) g_open = -1, redraw();
        else if (wp == VK_RETURN && g_open < 0) play();
        return 0;
    case WM_UPD:
        if (lp == 1) {
            MessageBoxW(h, L"Update installed. The launcher restarts now.", L"TMSRecomp", MB_ICONINFORMATION);
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);  // tmslauncher.exe (updated in place)
            ShellExecuteW(nullptr, L"open", exe, nullptr, g_dir.c_str(), SW_SHOWNORMAL);
            DestroyWindow(h);
            return 0;
        }
        redraw();
        return 0;
    case WM_TIMER:
        if (wp == 1 && g_su.running) redraw();
        return 0;
    case WM_SETUP:
        if (wp == 1 && g_su.done && g_su_extra) scan_mods();  // a Guide install: nothing of the build changed
        else if (wp == 1 && g_su.done && is_data(g_data)) {  // finished: remember the data folder and the build inputs
            std::ofstream(built_file(), std::ios::binary) << build_signature();
            set("data", narrow(g_data.wstring()));
            save();
            scan_mods();
            load_hero();
            apply_icon();
        }
        redraw();
        return 0;
    case WM_ACTIVATE:  // back from the game: its first start saves the title screen (Play page picture, icon)
        if (LOWORD(wp) != WA_INACTIVE && g_rt && !g_hero_title && !g_data.empty()) {
            std::error_code ec;  // only file checks here: the fallback picture is not reloaded on every focus
            for (const wchar_t* n : {L"launcher_hero.png", L"launcher_hero.jpg", L"launcher_hero.bmp"})
                if (fs::exists(g_data / n, ec)) { load_hero(), apply_icon(), redraw(); break; }
        }
        break;
    case WM_CLOSE:
        if (g_su.running && MessageBoxW(h, L"Setup is still running. Stop it and close?", L"TMSRecomp",
                                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        su_cancel();
        if (g_dirty) {
            int r = MessageBoxW(h, L"Save the changed settings?", L"TMSRecomp", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) save();
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    if (const wchar_t* p = wcsstr(cmd, L"--page ")) g_page = std::clamp(_wtoi(p + 7), 0, (int)std::size(k_pages) - 1);
    if (const wchar_t* p = wcsstr(cmd, L"--as-version "))
        for (const wchar_t* q = p + 13; *q && *q != L' '; q++) g_as_version += (char)*q;
    if (const wchar_t* p = wcsstr(cmd, L"--topic ")) g_topic = std::clamp(_wtoi(p + 8), 0, (int)std::size(k_topics) - 1);  // Guide topic
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = fs::path(exe).parent_path();
    {  // renamed to tmslauncher.exe after 0.1.5: a launcher.exe copy (what old launchers restart after an update) hands over
        std::error_code ec;
        const fs::path tl = g_dir / "tmslauncher.exe", old = g_dir / "launcher.exe";
        if (_wcsicmp(fs::path(exe).filename().c_str(), L"launcher.exe") == 0 && fs::exists(tl, ec)) {
            ShellExecuteW(nullptr, L"open", tl.c_str(), cmd, g_dir.c_str(), SW_SHOWNORMAL);
            return 0;
        }
        if (fs::exists(old, ec)) {  // still running for a moment: renamed now, delete_old_files() removes the .old later
            fs::path o = old;
            o += ".old";
            fs::remove(o, ec);
            if (!fs::remove(old, ec)) fs::rename(old, o, ec);
        }
    }
    g_ini = g_dir / "settings.ini";
    load();
    if (!game_ready() && !wcsstr(cmd, L"--page ")) g_page = 6;  // first start: build the game from the player's files
    delete_old_files();
    scan_mods();
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    const auto bold = DWRITE_FONT_WEIGHT_BOLD, semi = DWRITE_FONT_WEIGHT_SEMI_BOLD, reg = DWRITE_FONT_WEIGHT_NORMAL;
    const auto cond = DWRITE_FONT_STRETCH_CONDENSED;
    f_brand = font(L"Bahnschrift", 27, bold, cond);
    f_brand2 = font(L"Bahnschrift", 14, semi, cond);
    f_nav = font(L"Bahnschrift", 17, reg);
    f_h1 = font(L"Bahnschrift", 36, semi, cond);
    f_caps = font(L"Bahnschrift", 12.5f, semi);
    f_h2 = font(L"Bahnschrift", 24, semi, cond);
    f_label = font(L"Segoe UI", 15, reg);
    f_body = font(L"Segoe UI", 15, reg);
    f_small = font(L"Segoe UI", 12.5f, reg);
    f_btn = font(L"Segoe UI", 14, semi);
    f_play = font(L"Bahnschrift", 26, bold, cond);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // the optional icon.ico resource (CMake), else none
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TMSRecompLauncher";
    RegisterClassW(&wc);
    POINT pt{};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto f = (GetDpiForMonitor_t)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")) f(mon, 0, &dx, &dy);
    g_scale = dx / 96.f;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, (LONG)(W * g_scale), (LONG)(H * g_scale)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dx);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Tokyo Mirage Sessions ♯FE Encore · TMSRecomp", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = RGB(0x0b, 0x0f, 0x0d);  // SIDEBG
    DwmSetWindowAttribute(g_wnd, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    apply_icon();
    // issue #16: while setup runs, repaint 4x a second (elapsed time, bar) even when the tool prints nothing for a while
    // (the game.dll relink after an update); before, the page only changed when the mouse moved. Linux loop: same 250 ms.
    SetTimer(g_wnd, 1, 250, nullptr);
    // touch: one-finger vertical pans (WM_GESTURE) scroll the page; taps stay mouse clicks
    GESTURECONFIG gc{GID_PAN, GC_PAN | GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_INERTIA,
                     GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY | GC_PAN_WITH_GUTTER};
    SetGestureConfig(g_wnd, 0, 1, &gc, sizeof gc);
    ShowWindow(g_wnd, show);
    check_updates();
    if (const wchar_t* p = wcsstr(cmd, L"--setup ")) {  // --setup <extracted folder | nsp> [keys target]: start at once
        int n = 0;
        wchar_t** a = CommandLineToArgvW(p + 8, &n);
        if (n > 0) g_su_src = a[0];
        if (n > 2) g_su_keys = a[1], g_su_target = a[2];
        LocalFree(a);
        g_page = 6;
        su_start();
    } else if (setup_stale()) {  // after an update: rebuild game.dll from the same game files (incremental)
        g_su_src = g_data.wstring();
        g_page = 6;
        su_start();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
#else
// Linux window loop: SDL events in place of the window messages above
static bool close_requested() {  // WM_CLOSE: true = close
    if (g_su.running && MessageBoxW(g_wnd, L"Setup is still running. Stop it and close?", L"TMSRecomp",
                                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return false;
    su_cancel();
    if (g_dirty) {
        int r = MessageBoxW(g_wnd, L"Save the changed settings?", L"TMSRecomp", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) save();
    }
    return true;
}

int main(int argc, char** argv) {
    bool page_given = false;
    std::vector<std::string> setup_args;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--as-version") && i + 1 < argc) g_as_version = argv[++i];
        if (!strcmp(argv[i], "--page") && i + 1 < argc) g_page = std::clamp(atoi(argv[++i]), 0, (int)std::size(k_pages) - 1), page_given = true;
        else if (!strcmp(argv[i], "--topic") && i + 1 < argc) g_topic = std::clamp(atoi(argv[++i]), 0, (int)std::size(k_topics) - 1);
        else if (!strcmp(argv[i], "--setup")) while (i + 1 < argc) setup_args.push_back(argv[++i]);
    }
    try { std::locale::global(std::locale("C.UTF-8")); } catch (...) {}  // UTF-8 file names in fs::path
    g_dir = plat::exe_path().parent_path();
    g_ini = plat::settings_ini();  // the same file tms reads
    load();
    if (!game_ready() && !page_given) g_page = 6;  // first start: build the game from the player's files
    delete_old_files();
    scan_mods();
    SDL_SetHint(SDL_HINT_APP_ID, "TMSRecomp");
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "launcher: SDL_Init: %s\n", SDL_GetError()); return 1; }
    g_user_event = SDL_RegisterEvents(1);
    const auto bold = DWRITE_FONT_WEIGHT_BOLD, semi = DWRITE_FONT_WEIGHT_SEMI_BOLD, reg = DWRITE_FONT_WEIGHT_NORMAL;
    const auto cond = DWRITE_FONT_STRETCH_CONDENSED;
    f_brand = font(L"Bahnschrift", 27, bold, cond);
    f_brand2 = font(L"Bahnschrift", 14, semi, cond);
    f_nav = font(L"Bahnschrift", 17, reg);
    f_h1 = font(L"Bahnschrift", 36, semi, cond);
    f_caps = font(L"Bahnschrift", 12.5f, semi);
    f_h2 = font(L"Bahnschrift", 24, semi, cond);
    f_label = font(L"Segoe UI", 15, reg);
    f_body = font(L"Segoe UI", 15, reg);
    f_small = font(L"Segoe UI", 12.5f, reg);
    f_btn = font(L"Segoe UI", 14, semi);
    f_play = font(L"Bahnschrift", 26, bold, cond);
    g_wnd = SDL_CreateWindow("Tokyo Mirage Sessions ♯FE Encore · TMSRecomp", (int)W, (int)H, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_wnd) { fprintf(stderr, "launcher: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    g_ren = SDL_CreateRenderer(g_wnd, nullptr);
    if (!g_ren) { fprintf(stderr, "launcher: SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(g_ren, 1);
    create_target();
    SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    check_updates();
    if (!setup_args.empty()) {  // --setup <extracted folder | nsp> [keys target]: start at once
        g_su_src = wide(setup_args[0]);
        if (setup_args.size() > 2) g_su_keys = wide(setup_args[1]), g_su_target = wide(setup_args[2]);
        g_page = 6;
        su_start();
    } else if (setup_stale()) {  // after an update: rebuild game.so from the same game files (incremental)
        g_su_src = g_data.wstring();
        g_page = 6;
        su_start();
    }
    for (bool quit = false; !quit;) {
        SDL_Event e;
        bool got = SDL_WaitEventTimeout(&e, g_su.running ? 250 : 1000);
        if (g_su.running) g_need_redraw = true;  // the progress clock
        for (; got; got = SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (close_requested()) quit = true;
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: g_need_redraw = true; break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: g_hover = -1, g_need_redraw = true; break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:  // back from the game: its first start saves the title screen picture
                if (g_rt && !g_hero_title && !g_data.empty()) {
                    std::error_code ec;
                    for (const wchar_t* n : {L"launcher_hero.png", L"launcher_hero.jpg", L"launcher_hero.bmp"})
                        if (fs::exists(g_data / n, ec)) { load_hero(), g_need_redraw = true; break; }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                float x = e.motion.x, y = e.motion.y;
                if (g_sb_grab >= 0) { sb_move(y), g_need_redraw = true; break; }
                if (g_drag >= 0) {
                    const Setting& s = k_set[g_drag];
                    float k = std::clamp((x - g_drag_track.left) / (g_drag_track.right - g_drag_track.left), 0.f, 1.f);
                    char b[16];
                    snprintf(b, 16, "%.1f", s.lo + std::round(k * (s.hi - s.lo) / s.step) * s.step);
                    if (value_of(g_drag) != b) set(s.key, b), g_need_redraw = true;
                    break;
                }
                int k = hit_at(x, y), id = k >= 0 ? g_hits[k].id : -1;
                SDL_SetCursor(id >= 0 ? hand : arrow);
                if (id != g_hover) g_hover = id, g_need_redraw = true;
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                if (g_cap >= 0 && e.wheel.y != 0) {  // key binding capture (Game page)
                    set_slot(g_cap, e.wheel.y > 0 ? "WheelUp" : "WheelDown");
                    g_cap = -1, g_need_redraw = true;
                    break;
                }
                if (g_max_scroll > 0) {
                    float& sc = g_scroll[g_page];
                    sc = std::clamp(sc - e.wheel.y * 60, 0.f, g_max_scroll);
                    g_open = -1, g_hover = -1, g_need_redraw = true;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (g_cap >= 0) {  // key binding capture: a click on another slot moves it, else the button is bound
                    int hk = hit_at(e.button.x, e.button.y);
                    if (e.button.button != SDL_BUTTON_LEFT || hk < 0 || g_hits[hk].id < ID_KEY || g_hits[hk].id == ID_KEY + g_cap) {
                        if (e.button.button >= 1 && e.button.button <= 5) set_slot(g_cap, "Mouse" + std::to_string(e.button.button));
                        g_cap = -1, g_need_redraw = true;
                        break;
                    }
                }
                if (e.button.button != SDL_BUTTON_LEFT) break;
                if (sb_press(e.button.x, e.button.y)) { g_need_redraw = true; break; }
                int k = hit_at(e.button.x, e.button.y);
                bool was_open = g_open >= 0;
                if (k >= 0) {
                    int id = g_hits[k].id;
                    if (was_open && id != ID_SET + g_open && (id < ID_POP || id >= ID_MOD)) g_open = -1;
                    auto f = g_hits[k].click;  // the hit list is rebuilt by the redraw
                    f(e.button.x);
                } else g_open = -1;
                g_need_redraw = true;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) g_sb_grab = -1;
                if (e.button.button == SDL_BUTTON_LEFT && g_drag >= 0) g_drag = -1, ReleaseCapture(), g_need_redraw = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (g_cap >= 0) {  // key binding capture: SDL's own key names, as the game reads them back
                    if (e.key.repeat) break;
                    if (e.key.scancode == SDL_SCANCODE_DELETE) set_slot(g_cap, "");
                    else if (e.key.scancode != SDL_SCANCODE_ESCAPE) {
                        const char* n = SDL_GetScancodeName(e.key.scancode);
                        if (!n || !*n) break;
                        set_slot(g_cap, n);
                    }
                    g_cap = -1, g_need_redraw = true;
                    break;
                }
                if (e.key.key == SDLK_ESCAPE && g_open >= 0) g_open = -1, g_need_redraw = true;
                else if (e.key.key == SDLK_RETURN && g_open < 0) play();
                break;
            default:
                if (e.type == g_user_event) {
                    if (e.user.code == (Sint32)WM_UPD && (uintptr_t)e.user.data2 == 1) {  // update installed: restart
                        MessageBoxW(g_wnd, L"Update installed. The launcher restarts now.", L"TMSRecomp", MB_ICONINFORMATION);
                        spawn({(g_dir / "launcher").string()}, g_dir, nullptr, true);
                        quit = true;
                        break;
                    }
                    if (e.user.code == (Sint32)WM_SETUP && (uintptr_t)e.user.data1 == 1) {
                        if (g_su.done && g_su_extra) scan_mods();  // a Guide install: nothing of the build changed
                        else if (g_su.done && is_data(g_data)) {  // finished: remember the data folder and the build inputs
                            std::error_code ec;
                            fs::create_directories(built_file().parent_path(), ec);
                            std::ofstream(built_file(), std::ios::binary) << build_signature();
                            set("data", narrow(g_data.wstring()));
                            save();
                            scan_mods();
                            load_hero();
                        }
                    }
                    g_need_redraw = true;
                }
            }
        }
        if (g_launched) quit = true;
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}  // finished helpers (xdg-open, curl, setup)
        if (g_need_redraw && !quit) g_need_redraw = false, paint();
    }
    SDL_Quit();
    return 0;
}
#endif
