#pragma once
// Release version (the launcher compares it with GitHub release tags).
static const char k_version[] = "0.1.8";  // 0.1.0 beta = first public release (tag v0.1.0-beta)

// Keyboard / mouse bindings (ui.cpp poll_input, launcher Game page): settings.ini key_<name> = up to 3 names, comma
// separated; "none" = unbound. Names: SDL key names (K, Return, Left Shift, Keypad 4...), Mouse1..Mouse5, WheelUp / WheelDown.
struct KeyTarget { const char* name; const wchar_t* label; const char* def; };
static const KeyTarget k_key_targets[] = {
    {"a", L"A \u00b7 confirm / talk", "K, Return, Mouse1"}, {"b", L"B \u00b7 cancel / back", "J, Backspace, Mouse2"},
    {"x", L"X \u00b7 sword strike", "I"}, {"y", L"Y \u00b7 menu", "U"},
    {"l", L"L", "Q"}, {"r", L"R", "E"}, {"zl", L"ZL", "1"}, {"zr", L"ZR", "3"},
    {"plus", L"+ \u00b7 Topic / pause", "Space"}, {"minus", L"\u2212 \u00b7 map", "Tab"},
    {"l3", L"L3 \u00b7 left stick click", "none"}, {"r3", L"R3 \u00b7 right stick click", "Mouse3"},
    {"up", L"D-pad up", "Up"}, {"down", L"D-pad down", "Down"}, {"left", L"D-pad left", "Left"}, {"right", L"D-pad right", "Right"},
    {"ls_up", L"Move forward", "W"}, {"ls_down", L"Move back", "S"}, {"ls_left", L"Move left", "A"}, {"ls_right", L"Move right", "D"},
    {"rs_up", L"Camera up", "none"}, {"rs_down", L"Camera down", "none"}, {"rs_left", L"Camera left", "none"}, {"rs_right", L"Camera right", "none"}};

// settings.ini written next to tms.exe on the first run (by tms.exe or the launcher). "key = value" -> NX_KEY.
static const char k_default_settings[] =
    "; TMSRecomp settings. Environment variables override these (fps = NX_FPS, res = NX_RES, ...; any NX_ name works).\n"
    "; fps: 30 (the game's own), 60, N > 60 (capped at N), 0 (uncapped)\n"
    "fps = 60\n"
    "; auto_30 = 0: at fps 60 a PC that cannot hold 60 plays in slow motion (default 1: it drops to 30 fps at full game speed\n"
    ";   while 60 is not held, then back to 60)\n"
    "; crowd_density: 0 (default, the game's), 1 half, 2 low: fewer walkers and cars close by in town streets (Shibuya), for weak PCs\n"
    "; hdr_boost 0..4 (default 3): with HDR, how far the bright parts of the 3D scene are lifted toward the screen's peak\n"
    ";   (menus / text stay at paper white; 0 = the game's own brightness); hdr_color 0..4 (default 2): more vivid scene colours\n"
    "; scene_res: 3D resolution in % (100 default, 85, 75, 67, 50) with fx_fxaa 0 / 1 / 2: the 3D scene renders smaller and is\n"
    ";   scaled up, menus / HUD stay sharp; for weak graphics cards (FSR 3 / DLSS use fsr3_quality). auto: lowered step by\n"
    ";   step while the graphics card cannot keep the fps setting, raised again when there is room\n"
    "; shadow_detail = 1: no far shadow layer (shadows near you stay), fewer draws in big areas, for weak PCs (default 0)\n"
    "; res: internal resolution: auto (fits your screen and GPU), 720p (the game's own), 1080p, 1440p, 4k, 8k (above the screen size = supersampling)\n"
    "res = auto\n"
    "; window: borderless (fullscreen; Alt+Enter or F11 switches to a window) or window\n"
    "window = borderless\n"
    "; present_mode: mailbox (default, no tearing), fifo (classic vsync), vrr (G-SYNC / FreeSync: vsync + a frame cap 3 under\n"
    ";   the refresh rate), immediate (tearing, lowest latency)\n"
    "; language: en (default), ja (Japanese), fr (French), zh-Hans (Simplified Chinese), zh-Hant (Traditional Chinese):\n"
    ";   game text and menus (voices stay; the Uncensored mod's text is English only)\n"
    "; mute = 1 turns the sound off\n"
    "; walk_speed: player walk/run speed in the field (1.0 = the game's own)\n"
    "; post_native: 1 (default) keeps depth-of-field/bloom targets at the game's size (no halos at 4K), 0 scales them\n"
    "; mods_disabled: mod folder names (<data>\\mods\\<name>\\romfs) to leave off, comma separated\n"
    "; random_battle_bgm: 0 off, 1 game tracks, 2 custom (.adx files in custom_bgm_dir, default <data>\\music), 3 mixed;\n"
    ";   custom_bgm_volume: 0 .. 10 (default 7)\n"
    ";   random_bgm_bosses = 1: boss / story battles get a random track too (default 0: they keep their music)\n"
    "; ui_speed: 60 (default) menus / wheel / dialogue step at 60 Hz (never faster above 60 fps; battle HUD and Topic always keep the Switch pace); 30 = the Switch's speed\n"
    "; attack_session = 0: a regular Attack never starts a Session (the original rule; default 1: it can)\n"
    "; ui_smooth = 0: above 60 fps menus / HUD step at 60 Hz (default 1: motion glides between the steps, up to 16 ms later)\n"
    "; levelup_refill = 0: level-ups in battle no longer refill HP / EP\n"
    "; sub_exp = 0: battle EXP only for the three members on the front line (default 1: the whole party, the game's rule)\n"
    "; exp_gap = 0: members above the enemy's level get full EXP (default 1: less per level above, the game's rule)\n"
    "; battle_retry = 0: no \"Retry the battle?\" after a party wipe (default 1: retry with the battle's starting HP / EP / items)\n"
    "; item_max99 = 0: consumables keep the game's stock caps (Life Stone 40, Patra Stone 90, Magic Mirror 5...; default 1: 99 each)\n"
    "; ng_plus_requests = 0: requests 49 (Versus Grandmaster Mirage!) and 50 (The Dragon Descends) stay New Game+ only\n"
    ";   (default 1: also in a fresh game, same conditions otherwise)\n"
    "; map_reveal = 1: every Idolasphere floor shows its whole layout on the minimap / map when entered (default 0)\n"
    "; aspect: 16:9 (default, the game's), 21:9 / 32:9 ultrawide (wider 3D view, HUD and menus 16:9 in the middle), auto = the screen's shape\n"
    ";   ultrawide_fill = 0: full-screen 16:9 pictures / movies get black sides (default 1: a blurred, zoomed copy of the picture)\n"
    "; field_costume = 1: outside battle (field, dungeons, story scenes) party members wear their equipped costume (default 0: story clothes)\n"
    "; resist_marker = 0: no resist shield over foes / in the target panel and no RESIST popup on resisted hits (default 1)\n"
    "; weak_hint = 0: the battle skill list shows no WEAK next to skills a known foe weakness takes (default 1)\n"
    "; all_difficulties = 0: Friendly / Lunatic stay locked until the game unlocks them (default 1: all five selectable)\n"
    "; ko_revive = 0: members who fell stay at 0 HP after a battle (default 1: they get up with 1 HP once the battle is won or escaped)\n"
    "; upscaler: fsr1 (default: edge-adaptive upscale when res is below the window size) or bilinear\n"
    "; hdr = 1: HDR output on HDR screens (Windows HDR on; else SDR as before); hdr_paper_white: nits of normal white (default\n"
    ";   200); hdr_peak: the screen's brightest nits (0 = auto, from Windows)\n"
    "; sharpen: 0 (off) .. 100, contrast-adaptive sharpening; fx_fxaa / fx_bloom / fx_glow / fx_dof / fx_vignette /\n"
    ";   fx_fog / fx_outline = 0 turn that effect off; pad_icons: auto, switch, playstation, xbox\n"
    "; button_layout: switch (default: right face button = confirm, as on the Switch) or xbox (bottom = confirm, A/B and X/Y swapped)\n"
    "; fx_fxaa = 2: SMAA anti-aliasing (our pass) in place of the game's FXAA (1, default); 0 = none\n"
    ";   3: AMD FSR 3 temporal anti-aliasing at full size (amd_fidelityfx_*_vk.dll next to tms.exe; else SMAA)\n"
    ";   4: NVIDIA DLSS / DLAA (RTX GPU + nvngx_dlss.dll next to tms.exe; else FSR 3)\n"
    ";   fsr3_quality: with fx_fxaa = 3 or 4 the 3D scene renders smaller and FSR 3 upscales it: 0 full size (default), 1 Quality,\n"
    ";     2 Balanced, 3 Performance, 4 Ultra Performance (also live in the F1 menu)\n"
    ";   dlss_preset: DLSS model: 0 auto (NVIDIA's pick, default), 11 K, 12 L, 13 M (L / M = DLSS 4.5)\n"
    "; msaa: 0 (default), 2, 4 or 8 samples for the 3D scene (with fx_fxaa 0 / 1 / 2; unused with 3 / 4: FSR 3 / DLSS\n"
    ";   smooth edges themselves)\n"
    "; movie_gpu = 0: 4K movies decode on the CPU (default 1: on the GPU with Windows' VP9 decoder when there is one)\n"
    "; movie_hd_fallback = 0: 4K movies stay 4K when this PC decodes them too slowly (default 1: that movie drops to 720p)\n"
    "; vb_vram = 0: the game's vertex data stays in system RAM (default 1: unchanged vertex data is copied to video memory,\n"
    ";   much less GPU time on PCIe-limited cards such as laptop GPUs)\n"
    "; async_replay = 0: draw replay on the game thread (default 1: on its own thread at every frame rate but 30)\n"
    "; host_libc = 0: the game's memcpy / memset / memcmp / strlen / strcmp run as the game's own code (default 1: as PC code)\n"
    "; nan_guard: auto (default: on with AMD GPUs), 1, 0: invalid shader colours become black instead of black effect shapes\n"
    "; tex_pack: auto (default: on unless integrated GPU or under 4 GB VRAM), 1 on, 0 off (UHD texture pack, <data>\\textures\\pack)\n"
    "; voice_subs = 0 turns the English battle voice subtitles off (subtitles_en.tsv next to tms.exe)\n"
    "; key_<button> = keyboard / mouse keys for a Switch button or stick direction, up to 3, comma separated (\"none\" = unbound):\n"
    ";   a b x y l r zl zr plus minus l3 r3 up down left right ls_up ls_down ls_left ls_right rs_up rs_down rs_left rs_right\n"
    ";   names: K, Return, Space, Left Shift, Keypad 4, F5..., Mouse1 (left) .. Mouse5, WheelUp, WheelDown (launcher: Game page)\n"
    "; mouse_camera = 0: the mouse does not turn the camera (default 1: captured while the game has focus; F1 menu frees it);\n"
    ";   mouse_sensitivity: 0.2 .. 3.0 (default 1.0); mouse_invert_y = 1 inverts up / down\n"
    "; font_hd = 0: the game's own glyph sheets (default 1: text sharpened 4x for high resolutions)\n"
    "; battle_buttons = 1: P5-style battle commands on the wheel: bottom face button Attack, right Guard, top Skills, left\n"
    ";   Items, L Tactics, R Change, - Escape, ZR Auto battle (+ Stats as before). Default 0: the game's wheel only\n"
    ";   battle_buttons = 2: the same buttons, and a Persona 5 style command panel in place of the game's wheel (Windows)\n"
    "; ds4windows_xinput_only = 0: with DS4Windows running, also see PlayStation pads (only for DS4Windows set to DS4 output)\n"
    "; pad_<button> (a b x y l r zl zr plus minus l3 r3) = the controller button that presses it: south east west north, lb rb\n"
    ";   lt rt, start back, ls rs, paddle1 .. paddle4, none, or default (button_layout). Taking another button's swaps the two.\n"
    "; In game: F1 or L3 + R3 opens the settings menu\n"
    "; data: the folder with link\\image.bin and romfs (found next to the build folder, or asked for once)\n";
