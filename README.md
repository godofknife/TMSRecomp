<h1 align="center">TMSRecomp</h1>

<p align="center"><b>Tokyo Mirage Sessions #FE Encore, running natively on Windows and Linux / Steam Deck (experimental).</b><br>
Up to 4K and 240 fps · DLSS 4.5 / FSR 3 · VRR · one-click setup and updates · no emulator</p>

<p align="center">
  <a href="../../releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/godofknife/TMSRecomp?include_prereleases&label=release&color=3ee08f"></a>
  <a href="../../releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/godofknife/TMSRecomp/total?color=3ee08f"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue">
  <img alt="Linux" src="https://img.shields.io/badge/Linux%20%7C%20Steam%20Deck-experimental-orange">
  <img alt="Vulkan" src="https://img.shields.io/badge/graphics-Vulkan%201.3-red">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-GPL--2.0--or--later-lightgrey"></a>
</p>

<p align="center">
  <img src="docs/screenshots/shibuya.jpg" width="49%" alt="Shibuya at 4K">
  <img src="docs/screenshots/carnage_form.jpg" width="49%" alt="Carnage Form">
  <img src="docs/screenshots/battle.jpg" width="49%" alt="Battle skill menu">
  <img src="docs/screenshots/session.jpg" width="49%" alt="Session lineup">
</p>
<p align="center"><sub>4K, DLSS, UHD textures, Xbox button prompts. Screenshots from the author's own copy.</sub></p>

**TMSRecomp** is an unofficial native Windows (and experimental Linux / Steam Deck) port of *Tokyo Mirage Sessions #FE Encore* (Nintendo Switch, ATLUS /
Intelligent Systems, 2020), made by static recompilation: the game's 32-bit ARM code is translated to C and compiled
into an ordinary Windows x64 program, and its graphics run on Vulkan. There is no emulator and no JIT
([how it works](#how-it-works)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tokyo Mirage Sessions #FE Encore*, dumped
> from your own console: the base game **1.0.0 without update** (title ID `0100A9400C9C2000`) and your `prod.keys`, or an
> already extracted folder. How to dump them is not covered here, and requests for game files will be closed.
>
> The game is built from your files on your PC, so the result contains the game. It is for your own use: do not share
> `game.dll` or your game files folder.

> [!NOTE]
> **This project was built entirely with AI.** All of the code, tooling, reverse engineering and documentation in this
> repository was written by an AI model (Anthropic's Claude, through Claude Code), directed, tested and reviewed by the
> project's owner. It is shared as is, as a fan project. As with any code from the internet, you build and run it at
> your own discretion.

> [!WARNING]
> **v0.1.9 beta.** It is tested until the chapter 2 Idolasphere with
> saving and loading, on NVIDIA RTX and AMD Radeon (desktop and handheld). **Anything after that is unknown territory**,
> as are Intel GPUs, weaker PCs and Linux / Steam Deck: bug reports are very welcome.

*TMSRecomp is not affiliated with, endorsed or sponsored by Nintendo, ATLUS, SEGA or Intelligent Systems. Tokyo Mirage
Sessions #FE Encore and all related names, characters and assets are trademarks and copyrights of their owners.*

## At a glance

| | Nintendo Switch | TMSRecomp on PC |
|---|---|---|
| Resolution | 720p | 720p to 4K, 8K supersampling |
| Frame rate | 30 fps | 30, 60, 120, 144, 165, 240 or uncapped |
| Anti-aliasing | FXAA | FXAA, SMAA, MSAA up to 8x, AMD FSR 3, NVIDIA DLSS / DLAA (DLSS 4.5 models) |
| Upscaling | none | FSR 1, FSR 3 and DLSS Quality to Ultra Performance |
| Display | TV / handheld | borderless or window, Mailbox, VSync, VRR (G-SYNC / FreeSync) |
| Textures and movies | original | optional 4x UHD texture pack and 4K movies |
| Extras | none | battle retry, revealed maps, battle speed, multipliers, fast travel, autosave, mods and more |
| Controls | Joy-Con / Pro Controller | keyboard, Xbox, PlayStation and Switch pads with matching button prompts |

## Contents

- [Features](#features)
- [What's new in v0.1.9](#whats-new-in-v019)
- [Requirements](#requirements)
- [Installing and playing](#installing-and-playing)
- [Updating](#updating)
- [Extras: English patch, UHD textures, 4K movies, your own music](#extras)
- [Controls](#controls)
- [Settings](#settings)
- [Saves (and emulator saves)](#saves)
- [Mods](#mods)
- [Troubleshooting and FAQ](#troubleshooting-and-faq)
- [Known issues](#known-issues)
- [Reporting a bug](#reporting-a-bug)
- [How it works](#how-it-works)
- [Linux (experimental)](#linux-experimental)
- [Building from source](#building-from-source)
- [License and credits](#license-and-credits)

## Features

**Performance and display**
- Native code, not emulation: the game's ARM code is translated to C and compiled for Windows x64, and its NVN graphics
  calls run on Vulkan. Built on your PC from your own dump in 5 to 15 minutes.
- Faster translated code: guest registers and flags kept in CPU registers, -O2, and the game's memcpy / memset /
  string functions as PC code, ThinLTO and a profile-guided build. Shibuya on one CPU core: 93 -> 137 fps.
- 4K movies decode on the GPU's video engine (Windows' VP9 decoder): the camp menu uses about 2 CPU cores less.
- Frame rate 30 (the game's own), 60, 120, 144, 165, 240 or uncapped. Game logic,
  animation and the camera run at the chosen rate; the game keeps its original speed.
- Internal resolution from 720p (the game's own) to 4K, and 8K supersampling; Auto picks one for your screen and GPU.
- Borderless fullscreen or window (F11 / Alt+Enter switches while playing).
- **Ultrawide** 21:9 and 32:9 (or Auto: your screen's shape): a wider 3D view, HUD and menus stay 16:9 in the middle,
  field HUD in the screen corners. Never stretched: other shapes get black bars.
- VSync modes: Mailbox (no tearing, low latency), classic VSync, **VRR** (G-SYNC / FreeSync: VSync plus a frame cap
  3 fps under your refresh rate) and Off.
- Even frame pacing: a dedicated present thread, pipeline prewarm and a shader cache built during setup, so new areas
  do not stutter.
- Weak-PC defaults: integrated GPUs and cards under 4 GB of VRAM get a lighter default resolution and no UHD pack.

**Image quality**
- Anti-aliasing: the game's FXAA, SMAA, **AMD FSR 3** (temporal, works on any GPU) or **NVIDIA DLSS / DLAA** (RTX GPUs,
  optional download from NVIDIA during setup), plus MSAA 2x / 4x / 8x.
- FSR 3 / DLSS upscaling modes: Native (DLAA), Quality, Balanced, Performance, Ultra Performance. Menus and text always
  stay at full resolution.
- DLSS model selection: Auto, K (transformer) or the DLSS 4.5 models M and L (DLSS 310.9).
- FSR 1 screen scaler and contrast-adaptive sharpening (Off / Low / Medium / High).
- Each post effect can be turned off: bloom, glow, depth of field, vignette, fog, outlines, character rim light.
- Optional UHD texture pack and 4K story movies / camp backgrounds (separate downloads, see [Extras](#extras)).

**Quality of life (all optional, in the launcher and the in-game menu)**
- In-game settings menu (F1 or L3 + R3) with live changes, and a **Travel** page: jump to shops you have visited, or
  open the Tokyo Map from anywhere.
- **Reveal dungeon maps**: each Idolasphere floor shows its whole layout on the minimap and map when you enter it.
- **Retry lost battles** with the battle's starting HP / EP / items.
- **Overworld costume**: the costume equipped for battle is also worn in town, dungeons and story scenes.
- **Request hints**: F1 menu > Request hints lists the requests in progress and the ones waiting to be taken, with who
  gives each one and how to finish it.
- **New Game+ requests in a new game**: requests 49 (Versus Grandmaster Mirage!) and 50 (The Dragon Descends) no
  longer need New Game+.
- **Language**: English, Japanese, French, Simplified or Traditional Chinese text (the game's own translations).
- **All difficulties unlocked**: Friendly and Lunatic from the start, and every difficulty (Friendly .. Lunatic) can be
  changed mid-game in System > Settings. See [Difficulty levels](#difficulty-levels) for what each one changes.
- Battle speed, autosave after battles (to slot 20, your own slots are untouched), field walk speed.
- EXP, money and mastery multipliers; Sub Cast EXP; EXP level-gap penalty on/off; level-ups refill HP / EP;
  fallen members revive with 1 HP after a battle; carry 99 of each item; guarding recovers HP / EP.
- First strike on downed field enemies, hide field enemies, weakness / resistance icons in battle, detailed skill help.
- English subtitles for the Japanese battle voices.
- Random battle music: the game's own tracks, your own `.adx` music folder, or both, with matching victory themes.
- Button prompts for Switch, PlayStation or Xbox controllers (Auto detects your pad), and a **Button layout** option:
  Switch (right face button confirms) or Xbox / PC (bottom button confirms).
- **Resist markers**: a shield over foes that resist the highlighted skill (also in the target panel) and a RESIST popup
  on resisted hits, next to the game's own weakness "!" and WEAK.
- **WEAK in the skill list** (like Persona 5 Royal): a skill shows WEAK when a foe on the field is weak to it and you
  already know that weakness. On by default; F1 / L3 + R3 menu and launcher > Game.
- **Low-end options**: Auto 30 when slow (full game speed instead of slow motion), Crowd density (fewer walkers and cars
  in town), Shadow detail (no far shadow layer), 3D resolution (the 3D scene renders at 85 / 75 / 67 / 50% and is
  scaled up while menus and text stay sharp: the biggest FPS gain on weak graphics cards; Auto lowers it step by step
  while the graphics card cannot keep your FPS and raises it again when there is room). Launcher > Display & FPS and
  the F1 menu.
- **3D models in video memory** (on by default, `vb_vram`): the game's unchanged model data is copied to video memory
  instead of being read over the PCIe bus every frame: about half the GPU time in big areas such as the Shibuya crosswalk.
- **HDR** (HDR screens, Windows HDR on): HDR highlights lifts the bright parts of the 3D scene toward your screen's peak
  (like AutoHDR) and HDR colour makes it more vivid; menus and text stay at normal white.
- **Keyboard and mouse**: every button can be rebound (up to 3 keys each, mouse buttons and wheel too), and the mouse
  turns the camera (sensitivity, invert). Launcher > Game > Keyboard & mouse.
- **Controller button remapping**: any Switch button on any pad button, back paddles included; button prompts follow.
  Launcher > Game > Controller buttons.
- DS4Windows works: its virtual pad is used and the real PlayStation pad is left to it.
- Menus and dialogue stay at their original pace at any frame rate, with optional smoothing above 60 fps.

**Launcher**
- One window for setup, settings, mods, extras and updates. Quick settings on the Play page, every option on its pages.
- **Setup page**: pick your NSP + `prod.keys` (or an extracted folder) and press SET UP. No emulator, no firmware, no
  internet needed.
- **Guide page**: explains each extra and installs it from its ZIP with one click.
- **Mods page**: turn mods on and off without deleting them.
- **One-click updates** from this repository's releases (see [Updating](#updating)).
- Copy system info and open the last log for bug reports.
- The window icon and the Play page picture come from your own game (the NSP's icon and your title screen).

## What's new in v0.1.9

- **Ultrawide (21:9) shadows reach the screen edges.** Before, shadows stopped at the 16:9 part of the picture and
  the sides had none (thanks Promise!).
- **Ultrawide warp pad:** the swirl effect stays on the pad instead of being pulled toward the screen middle (thanks
  CTKirito!).
- **Ultrawide side fill:** 16:9 pictures and movies show a blurred copy in the side bars instead of black. Setting
  *Ultrawide sides* in the launcher (on by default; off = black bars).

Older versions: see each release's notes on the [Releases](../../releases) page.

## Requirements

- Windows 10 or 11, 64-bit.
- A GPU with Vulkan 1.3 and a current driver (NVIDIA GTX 900 series or newer, AMD RX 400 or newer, Intel Arc or
  recent integrated graphics). Tested on NVIDIA RTX and AMD Radeon (RX 7800 XT; Ryzen Z2 Go handheld, Legion Go S);
  Intel not yet.
- About 15 GB of free disk space (game files plus the built game). The UHD texture pack and 4K movies need more.
- Your own dump of the game: an NSP of the base game 1.0.0 and `prod.keys` from your own Switch, or an already extracted
  folder with `exefs\` and `romfs\` (no keys needed then).
- Nothing else: Python, the compiler and the shader translator ship inside the ZIP.
- **Linux / Steam Deck (experimental)**: x86-64 with glibc 2.39 or newer (Ubuntu 24.04, Fedora 40, Debian 13, SteamOS
  3.7). Same game, no FSR 3 / DLSS. Not tested on a Steam Deck yet: see [Linux](#linux-experimental).

## Installing and playing

Linux / Steam Deck: download `TMSRecomp-<version>-linux-x64.tar.gz` instead, extract it, run `launcher`; the rest is
the same ([details](#linux-experimental)).

1. Download `TMSRecomp-<version>.zip` from the [Releases](../../releases) page and extract it anywhere you can write to
   (for example `D:\Games\TMSRecomp`, not `C:\Program Files`).
2. Run `tmslauncher.exe` (called `launcher.exe` up to 0.1.5). The **Setup** page opens.
3. Pick your game: **NSP file…** plus your `prod.keys`, or **Extracted folder…**. Optionally tick **Download NVIDIA DLSS**
   (RTX GPUs).
4. Press **SET UP**. Setup copies the game files (about 13 GB), translates the game code, compiles it into `game.dll`
   and translates all shaders. It takes about 5 to 15 minutes and shows its progress.
5. Press **PLAY**.

The exe is not signed, so Windows SmartScreen may warn on the first start ("More info" > "Run anyway"). Some antivirus
tools look closely at a program that runs a compiler; everything stays inside the TMSRecomp folder.

Command line: `tms.exe --data <folder>` uses another game files folder; `--set key=value` overrides any setting for one
run (repeatable), for example `tms.exe --set window=window --set res=1080p --set fps=60`.

## Updating

The launcher checks this repository for a new release each time it starts.

1. When a new version is out, the launcher shows **Version x.y.z is ready**. **What's new** opens the release notes.
2. Press **Update**, then **Yes**. The launcher downloads the release, replaces its program files and restarts.
3. If the new version changes the game code, the Setup page rebuilds `game.dll` from your existing game files by
   itself (a few minutes; your NSP is not needed again).

Your `settings.ini`, saves, mods, UHD textures, movies and music are never touched by an update. **Check again** below
the version box checks manually. You can also update by hand: extract the new ZIP over your folder and replace the files.

## Extras

The launcher's **Guide** page explains each extra and installs it: press **Install ZIP…** (or **Install patch…**) and
pick the file you downloaded.

| Extra | What it does | Where to get it |
|---|---|---|
| English patch & uncensor | English voices and uncensored outfits / art (community patches) | the patch's own page; the Guide links it |
| UHD textures | AI-upscaled textures, 4x (`TMSRecomp-UHD` ZIP) | link in the release notes |
| 4K movies | 4K story movies and camp menu backgrounds (`TMSRecomp-MovieHD` ZIP) | link in the release notes |
| Your own battle music | random battle tracks from your own `.adx` files (for example a Persona 5 Royal music mod) | your own files |

Installing a newer version of an extra replaces the older one: an updated patch removes the old patch mod (mods with
other content stay), and a new UHD / MovieHD ZIP removes files the old pack had and the new one does not. Every extra
is optional; the game works the same without them. The UHD textures can be turned off without uninstalling
(launcher Graphics page or the in-game menu, after a restart).

## Controls

| Action | Keyboard | Controller |
|---|---|---|
| Move | W A S D | left stick |
| Camera | mouse (or keys you bind) | right stick |
| A / B (confirm / back) | K, Enter or left click / J, Backspace or right click | A / B (Switch layout: right face button is A; Button layout = Xbox / PC swaps them) |
| X / Y | I / U | X / Y |
| L / R, ZL / ZR | Q / E, 1 / 3 | L / R, ZL / ZR |
| D-pad | arrow keys | D-pad |
| + / - | Space / Tab | + / - (Start / Select) |
| Settings menu | F1 | L3 + R3 |
| Screenshot | F12 (saved to `<game files>\shots`) | |
| Window / fullscreen | F11 or Alt+Enter | |

In the Y menu, (-) offers Return to Title or Quit Game. Every keyboard / mouse key can be changed on the launcher's Game
page (Keyboard & mouse), or in `settings.ini` with `key_<button> = Key, Key, Key` (SDL key names such as `K`, `Return`,
`Left Shift`, `Keypad 4`, plus `Mouse1` .. `Mouse5`, `WheelUp`, `WheelDown`, `none`).

## Settings

Change settings in the launcher (Save, then PLAY) or in the in-game menu (F1 / L3 + R3; most apply at once). They are
stored in `settings.ini` next to `tms.exe` as `key = value`; any key can also be set with the environment variable
`NX_<KEY>` or `--set key=value`.

| Key | Values | |
|---|---|---|
| `fps` | 30, 60 (default), any number above 60, 0 = uncapped | frame rate |
| `res` | auto (default), 720p, 1080p, 1440p, 4k, 8k | internal resolution |
| `window` | borderless (default), window | |
| `present_mode` | mailbox (default), fifo, vrr, immediate | VSync |
| `fx_fxaa` | 0 off, 1 FXAA (default), 2 SMAA, 3 FSR 3, 4 DLSS | anti-aliasing |
| `fsr3_quality` | 0 native (default), 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance | FSR 3 / DLSS upscaling |
| `dlss_preset` | 0 auto (default), 11 K, 13 M, 12 L | DLSS model |
| `msaa` | 0 (default), 2, 4, 8 | not used with FSR 3 / DLSS |
| `upscaler` | fsr1 (default), bilinear | screen scaler below your screen size |
| `sharpen` | 0 (default) .. 100 | |
| `post_native` | 1 (default), 0 | depth of field / bloom at the game's size (no halos) |
| `fx_bloom`, `fx_glow`, `fx_dof`, `fx_vignette`, `fx_fog`, `fx_outline`, `fx_char_rim` | 0 turns the effect off | |
| `tex_pack` | auto (default), 1, 0 | UHD texture pack |
| `pad_icons` | auto (default), switch, playstation, xbox | button prompts |
| `button_layout` | switch (default), xbox | which face button confirms |
| `aspect` | 16:9 (default), auto, 21:9, 32:9 | ultrawide |
| `ultrawide_fill` | 1 (default), 0 | ultrawide only: full-screen 16:9 pictures and movies (title, story movies, Tokyo map) get a blurred, zoomed copy of the picture at the sides; 0 = black sides (also the F1 menu and Settings PC 6/6) |
| `resist_marker` | 1 (default), 0 | resist shield / RESIST popup in battle |
| `font_hd` | 1 (default), 0 | sharp text at high resolutions (the game's glyph sheets scaled 4x with crisp edges) |
| `battle_buttons` | 0 (default), 1, 2 | P5-style battle buttons: each command on its own button (bottom Attack, right Guard, top Skills, left Items, L Tactics (on wheels that have it), R Change, - Escape, ZR Auto battle, + Stats; also in the game's Settings PC 4/5 and the F1 / L3+R3 menu, live); 2 also shows a Persona 5 style command panel in place of the game's wheel (Windows) |
| `key_a` .. `key_rs_right` | key names, comma separated | keyboard / mouse bindings (see [Controls](#controls)) |
| `mouse_camera`, `mouse_sensitivity`, `mouse_invert_y` | 1, 1.0 (0.2 .. 3.0), 0 | mouse turns the camera |
| `pad_a` .. `pad_r3` | default, south, east, west, north, lb, rb, lt, rt, start, back, ls, rs, paddle1 .. paddle4, none | controller button remapping (launcher > Game > Controller buttons); taking another button's swaps the two |
| `walk_speed` | 1.0 (default) .. 3.0 | field walk speed |
| `nan_guard` | auto (on with AMD GPUs), 1, 0 | fixes effects drawn as black shapes on AMD cards (invalid shader colour values become black instead of spreading) |
| `host_libc` | 1 (default), 0 | the game's memcpy / memset / memcmp / strlen / strcmp as PC code (0: the game's own) |
| `movie_gpu` | 1 (default), 0 | 4K movies decode on the GPU (Windows' VP9 decoder, "VP9 Video Extensions"); 0 or no decoder: on the CPU |
| `movie_hd_fallback` | 1 (default), 0 | 4K movies drop to 720p when this PC decodes them too slowly; 0 keeps 4K |
| `map_reveal` | 0 (default), 1 | reveal dungeon maps |
| `ng_plus_requests` | 1 (default), 0 | the two New Game+ requests (49, 50) also in a fresh game |
| `field_costume` | 0 (default), 1 | overworld costume |
| `language` | en (default), ja, fr, zh-Hans, zh-Hant | game text and menus (voices stay; the Uncensored mod's text is English only, so with another language its text changes are left out) |
| `all_difficulties` | 1 (default), 0 | Friendly / Lunatic unlocked, full difficulty range in Settings |
| `battle_retry` | 1 (default), 0 | retry lost battles |
| `autosave` | 1 (default), 0 | save to slot 20 after battles |
| `exp_rate`, `money_rate`, `mastery_rate` | 1 (default) and up | multipliers |
| `sub_exp`, `exp_gap`, `levelup_refill`, `ko_revive`, `item_max99`, `guard_regen`, `first_strike` | 1 / 0 | gameplay extras |
| `random_battle_bgm` | 0 off, 1 game tracks, 2 custom, 3 mixed | with `custom_bgm_dir`, `custom_bgm_volume` (0 .. 10) |
| `voice_subs` | 1 (default), 0 | English battle voice subtitles |
| `ui_speed`, `ui_smooth` | 60 / 1 (defaults) | menu pace and smoothing above 60 fps |
| `mute` | 1 | sound off |
| `mods_disabled` | folder names, comma separated | mods to leave off |
| `data` | a folder | the game files folder |

The full list with explanations is at the top of `settings.ini` and in the ZIP's `README.txt`.

## Saves

The game's own save files live unchanged in `save\` inside your game files folder (`system.bin`, `save_*.bin`). Back
that folder up now and then. Autosave (on by default) writes slot 20 after battles and never touches your own slots.

**Emulator saves** (Ryujinx or a Yuzu-based emulator) use the same files and should load (not tested yet, please
report how it goes):

1. Back up the `save\` folder in your game files folder.
2. In the emulator, right-click the game > **Open User Save Directory** (Ryujinx) or **Open Save Data Location**.
3. Copy `system.bin` and every `save_*.bin` from there into `save\`.
4. PLAY and pick LOAD.

A save written by an updated game (not 1.0.0) is untested.

## Mods

A mod is a folder `<game files>\mods\<ModName>\` (LayeredFS style); the launcher's **Mods** page turns each one on or off.

- **Loose files (easiest)**: the game keeps its data inside big `.cpk` packs in `romfs\Pack`. A mod can replace single
  files inside them, so it only holds the files it changes:
  1. Launcher > Mods > **Dump game files** (or `tms.exe --dump`; `--all` adds the movies, `--pack <name>` one pack).
     It unpacks every pack to `<game files>\dump\<pack>\...` (about 19 GB, a few minutes; a stopped dump continues).
  2. Copy the file you want to change to `mods\<ModName>\cpk\<pack>\<same path>`, for example
     `dump\pack_030_etc\Table\StSkillTable.dat` -> `mods\MySkills\cpk\pack_030_etc\Table\StSkillTable.dat`.
  3. Edit the copy and start the game. `nx.log` shows `fs: mod MySkills: 1 loose file in pack_030_etc.cpk`.

  Where things are:

  | Pack | What is inside |
  |---|---|
  | `pack_030_etc` | game tables: skills (`Skill\`, `Table\StSkillTable.dat`, `Table\EnterSkillTable.dat`...), items, units, encounters, events |
  | `pack_031_message` | all text (`Message\USen\...` English, one `.msbt` per screen: skill / item descriptions in `Common\`, map names in `Common\MapName.msbt`) |
  | `pack_999_lua` | game scripts (`Battle\AI\*.lua` enemy behaviour and more) |
  | `pack_999_etc_om` | battle tables, cameras, voice lists |
  | `pack_080_interface` | menus and HUD art; `pack_010_character` models; `pack_000_map` maps; `pack_040_sound` music and sound |

  Text (`.msbt`) opens in any MSBT editor. Tables (`.dat`, `.tbl`) are binary: a hex editor and community notes.
  A replaced file may be any size; it is read as it is (no compression needed). Only files the pack already has can be
  replaced (the log names any other file it ignored). Loose files work on top of whole-pack mods such as the Uncensored
  patch.
- **Whole game files**: put them in `mods\<ModName>\romfs\` at the same path as in `<game files>\romfs\` (a whole `.cpk`
  pack, for example). Files you do not ship come from the game.
  Mods shipped as xdelta patches against the game's `.cpk` files:
  `python tools/vcdiff.py <original.cpk> <patch.xdelta> <out.cpk>`.
- **Textures**: set `tex_dump = 1` and play: every texture shown is saved once to
  `<game files>\textures\dump\<w>x<h>_<format>_<hash>.dds`. Edit one, keep the `_<hash>` at the end of the name, and
  put it in `mods\<ModName>\textures\` (subfolders are fine). Any size works; DDS in BC1-BC7 or RGBA8 with mipmaps
  (`texconv -m 0`). Mod textures win over the UHD pack.
- **Several mods**: for the same file or texture, the mod whose folder name sorts first wins.
- `nx.log` lists each mod at start (`fs: mod <name> on`, `textures: mod <name>: N`). Restart the game after a change.

### Difficulty levels

What each difficulty changes, read from the game's code (battle setup and damage formula):

| Difficulty | Damage you deal | Damage you take | Money | Escape base chance |
|---|---|---|---|---|
| Friendly | ×1.5 | ×0.5 | ×1.25 | 50% |
| Easy | ×1.25 | ×0.8 | ×1.25 | 50% |
| Normal | ×1.0 | ×1.0 | ×1.0 | 30% |
| Hard | ×0.8 | ×1.25 | ×1.0 | 1% |
| Lunatic | ×0.5 | ×1.5 | ×1.0 | 1% |

Hard and Lunatic also change ailment hit rates; on Friendly and Easy, members below the enemy's level get +20% EXP per
level instead of +10%. Enemy HP and stats are the same on every difficulty.

## Troubleshooting and FAQ

- **Setup fails or stops.** The Setup page shows the step and the error; **Open last log** shows the details. Check
  that the NSP is the base game 1.0.0 (not an update or DLC) and that `prod.keys` comes from your own console. A
  message about `title.keys` alone is only a warning; since v0.1.1 an NSP's own ticket is used. If setup still says the
  NSP needs a title key, put `title.keys` (with this game's key) next to `prod.keys`. With an
  extracted folder, it must contain `exefs\main` and `romfs\`.
- **"The game is not set up yet, or needs a rebuild after an update."** Open the Setup page; it rebuilds `game.dll`
  from your existing files.
- **DLSS is not offered or falls back to FSR 3.** DLSS needs an RTX GPU, a current NVIDIA driver and `nvngx_dlss.dll`
  next to `tms.exe` (Setup page > Download NVIDIA DLSS).
- **Stutter the first time something appears.** Pipelines are compiled once and cached in `<game files>\cache`;
  the second time is smooth. Do not delete that folder.
- **Low fps.** Lower the resolution, use FSR 3 / DLSS upscaling (Quality or Balanced), turn MSAA off. 60 fps is the
  most tested frame rate.
- **Screen tearing with VRR.** Turn on G-SYNC / FreeSync in your GPU driver and use VSync = VRR in the launcher.
- **The game does not start / closes at once.** Update your GPU driver, then send the bug report described below.
- **Can I use an updated game (1.0.1+) or DLC?** Not yet: the hooks use fixed addresses of the 1.0.0 build.
- **Linux / Steam Deck?** Download `TMSRecomp-<version>-linux-x64.tar.gz` from the release (experimental, nothing to install): see [Linux (experimental)](#linux-experimental). Proton is untested.
- **Where are my files?** Everything (game files, saves, cache, logs, screenshots, mods) is in the game files folder
  chosen during setup; settings are in `settings.ini` next to `tms.exe`.

## Known issues

- Tested from the start through chapter 2 only; later chapters may have problems nobody has seen yet.
- Tested on NVIDIA RTX and AMD Radeon (RX 7800 XT, Legion Go S); Linux on one NVIDIA GTX 1650. Intel GPUs and the
  Steam Deck are untested.
- Only the base game 1.0.0 without update is supported.
- Walking NPCs turn in small 30 Hz steps at high frame rates.
- Custom battle music: encrypted `.adx` files need their key in `settings.ini` (`custom_bgm_key`); none is included.
- Ray tracing is not planned: the game's lighting is baked into its art.

## Reporting a bug

A good report lets us find and fix the problem quickly. Please open an issue with the **bug report** form and include:

1. **Your PC (spec rig):** launcher > **Guide** > Controls & help > **Copy system info**, then paste it. It lists your
   CPU, GPU, driver, RAM, Windows version and settings.
2. **What happened:** what you did, what you saw and what you expected instead. Does it happen every time?
3. **Where:** chapter, place, and menu or battle.
4. **Screenshot or video:** F12 saves a screenshot to `<game files>\shots`. A short video helps most with flicker,
   stutter or animation problems.
5. **Save data:** zip the `save\` folder in your game files folder and attach it, and say which slot to load. We can
   then go straight to the spot.
6. **Log:** attach `nx.log` from your game files folder (and `issues.log` if there is one). Copy it **before** you start
   the game again: each start writes a new log.

Never post game files, keys or links to them.

## How it works

- `tools/nso.py`, `tools/cpk.py`: readers for the game's executables (NSO) and CRI archives (CPK).
- `tools/recomp/`: our A32 (ARMv7 / ARMv8-AArch32) to C recompiler. It links `main` + `sdk` like the Switch's loader,
  finds functions and emits one C function per guest function; `test_a32.py` checks the translation against Unicorn.
- `runtime/`: guest memory, a high-level Horizon kernel (threads, synchronisation, memory SVCs), the `nn::` services the
  game uses (fs, hid, vi, audio, ...), NVN implemented on Vulkan, and the PC extras (in-game menu, upscalers, mods).
- `tools/shaderc/`: a sidecar that turns the game's Maxwell shaders into SPIR-V with the Ryujinx shader translator.
- `tools/setup/`: the player's first-run setup (extract, recompile, build `game.dll`, translate shaders).
- `runtime/launcher/`: the launcher (Win32 + Direct2D).
- `NOTES.md`: the full technical log of the project.

## Linux (experimental)

A native Linux x86-64 build of the same runtime, launcher and setup (no Wine or Proton). As on Windows, the game is
built on your PC from your own dump; nothing of the game is in the repository or a build.

**Tested so far** (Ubuntu 26.04, NVIDIA GTX 1650, driver 620): setup from an extracted folder, launcher, title screen,
intro movie, story scenes, field, camp menu, saving and loading, the F1 menu, at a steady 60 fps. Battles, AMD and Intel
GPUs and the Steam Deck are not tested yet.

**Download**: `TMSRecomp-<version>-linux-x64.tar.gz` on the release page, built by GitHub Actions
(`.github/workflows/build.yml`). Extract it and run `./launcher`: nothing to install (also on the Steam Deck).
Python, clang + lld (game code compiler), nstool, SDL3, libvpx and the shader translator's .NET runtime are inside; glibc 2.39 or newer (Ubuntu 24.04,
Debian 13, Fedora 40, SteamOS 3.7).

**Steam Deck**: switch to Desktop Mode, download the tarball, right-click it in Dolphin > Extract > Extract archive here,
double-click `launcher` and run Setup there. For Game Mode: right-click `launcher` > Add to Steam. Suggested start
settings: Resolution 720p, FPS 30, Crowd density Low. Please report how it runs (with `nx.log`).

**Differences from Windows**
- No AMD FSR 3 and no NVIDIA DLSS: AMD ships the FidelityFX API only as Windows DLLs, and the NGX (DLSS) path is not
  wired up. Choosing them falls back to SMAA, as on a Windows PC without them. FXAA, SMAA, MSAA, FSR 1 and sharpening work.
- The launcher's Update button opens the release page (releases are Windows downloads).
- `settings.ini` sits next to `tms` when that folder is writable, else in `~/.config/TMSRecomp/settings.ini` (an
  AppImage or system install); `NX_INI` overrides it. The recompiled code is `game.so` next to `tms`, or where
  `NX_GAME_SO` points.
- The Windows-only debugging aids (`NX_PROFILE`, `NX_CONTROL watch`, `NX_STALL_STATS`, `NX_MEM_REGIONS`) say they are
  not available; use `perf` instead.
- File names in the game files folder are matched case-insensitively, as on Windows.

**Packages** (Ubuntu / Debian names; other distributions have the same libraries): `clang lld llvm cmake ninja-build
pkg-config libsdl3-dev libvulkan-dev libvpx-dev libcairo2-dev libpango1.0-dev python3-venv dotnet-sdk-10.0`, and
nstool 1.9.2 built from source (`make deps && make`). Capstone for the translator:
`python3 -m venv venv && venv/bin/pip install capstone`.

**Build** (the same third-party Ryujinx checkout and patch as on Windows, in `private/third_party/ryubing`):

1. `clang -O2 -shared -fPIC -o tools/texextract/fasttex.so tools/texextract/fasttex.c`
2. `dotnet build tools/shaderc/NxShader.csproj -c Release -o private/tms/shaderc`
3. Set up the game data folder with your own dump, compile `game.so` and translate all shaders:
   `venv/bin/python tools/setup/setup.py --nsp <game.nsp> --keys <prod.keys> --data private/tms --nstool <nstool> --install private/tms/build`
   (or `--folder <extracted folder>` instead of `--nsp` / `--keys` / `--data`). This writes `private/tms/gen`.
4. `cmake -S . -B private/tms/build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo`
   and `ninja -C private/tms/build tms launcher` (`ninja -C private/tms/build` also rebuilds `game.so`).
5. Run `private/tms/build/launcher`, or `private/tms/build/tms --data private/tms`.

The Linux code paths are marked `#ifdef _WIN32 ... #else` in place; `runtime/src/plat.h` has the small POSIX stand-ins,
`runtime/src/draw_linux.h` the cairo / pango drawing (menu, subtitles, button icons) and `runtime/launcher/linux_shim.h`
the launcher's window layer (SDL3). Guest memory write tracking (texture re-uploads) uses page protection in place of
Windows' write watch (`memory.cpp`).

## Building from source

Requirements: Visual Studio 2022 (CMake + Ninja), LLVM/clang, .NET 10 SDK, Python 3.12, a Vulkan 1.3 GPU.
Third-party pieces go in `private/third_party/`: SDL3 3.4.16 (VC dev package), Vulkan-Headers 1.4.328, nstool 1.9.2,
the Ryujinx (Ryubing) source for the shader sidecar, libvpx 1.16.0 (BSD-3-Clause, VP9 movies) and the AMD FidelityFX /
FSR SDK 2.4.0 files used by FSR 3 (`FidelityFX-SDK-2.4.0/Kits/FidelityFX`: api/include, upscalers/include,
signedbin/amd_fidelityfx_*_vk.dll, docs/license.md; only MIT-listed files). Apply `tools/shaderc/patches/*.patch` to the
Ryujinx checkout (`patch -p1` from its root): its SPIR-V backend drops `else if` branches otherwise.

1. Extract your NSP with nstool and your own `prod.keys` into `private/tms/` (`exefs/`, `romfs/`).
2. `python tools/recomp/link.py private/tms/exefs private/tms/link`
3. `python tools/recomp/lift.py private/tms/link private/tms/gen`
4. `dotnet build tools/shaderc/NxShader.csproj -c Release -o private/tms/shaderc`
   (release ZIP only: `dotnet publish tools/shaderc/NxShader.csproj -c Release -r win-x64 --self-contained true -o
   private/tms/shaderc_sc -p:DebugType=none -p:SatelliteResourceLanguages=en`, then `cp -p private/tms/shaderc/*.dll
   private/tms/shaderc_sc/` so the shader cache stamp stays the same).
5. libvpx (Git Bash, from `private/third_party`, LLVM on `PATH`; a portable C build, no assembler needed):
   `git clone --depth 1 --branch v1.16.0 https://github.com/webmproject/libvpx.git`, then in a new `vpx-gen` folder
   `CC=clang ../libvpx/configure --target=generic-gnu --disable-vp8 --disable-vp9-encoder --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-webm-io --disable-libyuv`,
   generate the headers `make` would (`perl ../libvpx/build/make/rtcd.pl --arch=generic --sym=<name> --config=libs-generic-gnu.mk <defs> > <name>.h`
   for `vp9_rtcd` (`vp9/common/vp9_rtcd_defs.pl`), `vpx_dsp_rtcd` (`vpx_dsp/vpx_dsp_rtcd_defs.pl`), `vpx_scale_rtcd`
   (`vpx_scale/vpx_scale_rtcd.pl`), and `sh ../libvpx/build/make/version.sh ../libvpx vpx_version.h`), then list the
   sources: `python tools/vpx_sources.py private/third_party/libvpx private/third_party/vpx-gen`.
6. `build.cmd` builds `tms.exe` (the runtime) and `game.dll` (the lifted code, through `tools/setup/build_game.py`, the
   same script the player's setup uses). Run `private/tms/build/tms.exe`; it finds the data folder `private/tms` from its
   own location.
7. Release ZIP: `powershell -File tools/package/package.ps1` writes `TMSRecomp-<date>.zip` to the Desktop.
8. Optional: `tools/make_icon.ps1 <image>` makes `private/tms/icon.ico` for your own build; `tools/make_shortcut.ps1`
   puts a launcher shortcut on your Desktop.

UHD textures from your own copy: play with `tex_dump = 1`, then `python tools/uhd_pack.py private/tms` (needs
Real-ESRGAN-ncnn-vulkan, texconv and ImageMagick; see the script). Without playing: `python
tools/texextract/texextract.py private/tms` dumps every texture, then `python tools/uhd_pack.py private/tms --dump
private/tms/textures/dump_all`. 4K camp backgrounds: `python tools/camphd/camphd.py <data> <esrgan dir>`.

Everything derived from the game (decrypted files, the linked image, generated C code, shaders, builds) stays in
`private/`, which is never committed.

## License and credits

TMSRecomp is GPL-2.0-or-later (see `LICENSE`). This covers our own code only; the game and everything derived from it
belong to their owners and are not part of this repository.

Additional permission: as a special exception to the GPL, the copyright holders of TMSRecomp allow it to be run together
with the NVIDIA NGX core of the player's graphics driver and NVIDIA's `nvngx_dlss.dll`, which are not covered by this
licence and are not part of this program or its releases.

- Ryujinx (Ryubing) team: the shader sidecar (`tools/shaderc`) builds against the Ryujinx shader translator
  (`Ryujinx.Graphics.Shader`, Maxwell -> SPIR-V) and `Spv.Generator`, both MIT. Nothing from them is copied into this
  repository; release ZIPs ship the built sidecar in `shaderc\` with their MIT notice in `THIRD_PARTY.txt`.
- SDL3 (zlib licence), libvpx (BSD-3-Clause, VP9 movies), Vulkan-Headers (Apache-2.0 / MIT), nstool (setup and build).
- AMD FSR 3: AMD FidelityFX / FSR SDK 2.4.0 Vulkan DLLs and API headers, MIT (the SDK's `docs/license.md` lists these
  files under MIT). AMD FSR and FidelityFX are trademarks of Advanced Micro Devices, Inc.; TMSRecomp is not affiliated
  with or endorsed by AMD.
- NVIDIA DLSS / DLAA: no NVIDIA code, header or library is compiled in or shipped. At run time the program calls the NGX
  core of the installed NVIDIA driver, which loads `nvngx_dlss.dll` from the folder of `tms.exe`. The setup downloads
  that file from NVIDIA's own repository (github.com/NVIDIA/DLSS, NVIDIA RTX SDK licence) only if the player ticks it.
  NVIDIA and DLSS are trademarks of NVIDIA Corporation; TMSRecomp is not affiliated with or endorsed by NVIDIA.
- AMD FidelityFX Super Resolution 1 (MIT): the idea of the "FSR 1" upscaler (edge-adaptive EASU pass + RCAS-style
  sharpening). Our shaders (`tools/spvgen/Upscale.cs`, `Sharpen.cs`) are our own code, no AMD source is used.
- CRI ADX: the custom battle music decoder (`runtime/src/ext_bgm.cpp`) is our own code for the publicly documented format
  (vgmstream's documentation of the ADX layout and type-9 key was the reference).
- Thanks to the Switch recompilation and preservation community, whose public work showed that projects like this are
  possible.
