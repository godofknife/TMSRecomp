// PC settings inside the game's own Settings screen (Camp > System > Settings; user request 10-30, like the BotW PC
// port): L / R steps the screen through the game's page and five "PC" pages with the F1 overlay's items (R: game ->
// PC 1 -> ... -> PC 5 -> game, L back). The screen keeps working as the game wrote it; we only swap what its 11 rows hold
// and show:
// - row i (S+0x1c+i*0x18): +0 label text object, +4 option text object (0 on the 3 slider rows), +8 value, +0xc
//   arrows (left / right change the value), +0x10 min, +0x14 max. The game's update steps +8 within min/max.
// - our labels and options: the text objects' direct UTF-8 text (0x586488); the game's own come back by label
//   (0x586818(obj, msg file, label)).
// - the help line is Lua-driven and only takes msbt labels: Camp.msbt's text block (TXT2) is rebuilt with our text
//   for the row's CAMP_SETTING_HELP_* entry (as the detailed skill help does), and every help line gets the page hint.
//   Hooking the message lookups themselves (0x5c22e8 / 0x5c23dc / 0x5c2414) stalled the game when the camp opened.
//   Our help texts are short: the help bar cuts long lines at the screen edge (user report 10-30).
// - rows 0-2 are always drawn as sliders (0-10): our slider items step one choice per press and the help line shows
//   the value. Their live volume preview (sound_volume_set) is skipped while our page is up.
// - our pages apply each change at once (as the F1 menu does); before the screen leaves its browsing state (B /
//   confirm dialogs) the game's rows are put back, so the game's own apply (0x33c7e8) only ever sees its own values.
// - A on our pages opens the game's own "Apply these settings?" dialog (user request 10-30; was "next choice"):
//   update states (0x33c0a0): 2 browsing; A -> 4 (+0x134 = 1) dialog opening, 5 dialog up (A Yes -> apply 0x33c7e8,
//   6 -> 3 = the screen closes; No / B -> 7 -> 2); Y -> 4 with +0x134 = 0 (defaults); B -> 3. For the A dialog only
//   the row values go back (our labels stay on screen); No / B brings our page back.
#include <algorithm>
#include <map>
#include <vector>

#include "rt.h"

using namespace rt;

namespace {
struct PageRow { const wchar_t* name; bool slider; const char* help; };  // name null: an empty row
// Rows 0-2 are built as sliders by the game (no option text object). A slider item there gets the bar; an On/Off item there
// borrows an empty row's option text (lend_options) and is drawn as a choice row (user report 10-30: Bloom/DOF/Vignette
// showed as sliders; USER 11-21: PC 4/4 began with 3 blank rows).
const PageRow k_pages[6][11] = {
    {
        {L"Frame rate", true, "Higher is smoother; game speed stays the same. To / from 30: restart."},
        {L"Resolution", true, "Internal render resolution. Applies after a restart."},
        {L"Sharpening", true, "Crisper final image (contrast-adaptive sharpening)."},
        {L"Button icons", false, "Button prompts for your pad. Auto follows the connected controller."},
        {L"UHD textures", false, "The upscaled texture pack (textures\\pack), if installed."},
        {L"Anti-aliasing", false, "Off / FXAA (the game's) / SMAA (sharper) / FSR 3 / DLSS (temporal, smoothest)."},
        {L"Bloom", false, "Glare around bright lights."},
        {L"Glow", false, "Soft glow on emissive surfaces."},
        {L"Depth of field", false, "Background blur in cutscenes and menus."},
        {L"Vignette", false, "Darkened screen edges where the game uses them."},
        {L"Fog", false, "Distance fog where the game uses it."},
    },
    {
        {L"Walk speed", true, "Itsuki's walking and running speed (1x = the original)."},
        {L"Battle speed", true, "Speed of battles. In battle also F3 / touchpad. 3x needs 90+ fps."},
        {L"Custom music volume", true, "Volume of your own battle music (Random battle music: Custom / Mixed)."},
        {L"Battle voice subtitles", false, "English subtitles for the Japanese battle voices."},
        {L"Random battle music", false, "Game / Custom (your .adx folder, matched victory) / Mixed. Normal battles only (see PC 3)."},
        {L"Hide field enemies", false, "No enemy symbols: explore freely. From the next area on."},
        {L"Guard recovers HP / EP", false, "Guarding restores 10% of max HP and 10% of max EP."},
        {L"First strike on downed symbols", false, "Touching a symbol you knocked down gives you the first turn."},
        {L"Autosave after battles", false, "Saves to slot 20 after each battle (your slots stay untouched)."},
        {L"Detailed skill help", false, "Skill help shows power, accuracy and targets (after a restart)."},
        {L"Screen scaler", false, "Resolution below the screen: Bilinear or FSR 1. FSR 3 / DLSS: see Anti-aliasing."},
    },
    {
        {L"EXP multiplier", true, "EXP from every battle x N."},
        {L"Money multiplier", true, "Money from every battle x N."},
        {L"Mastery multiplier", true, "Weapon (Carnage) Mastery from every battle x N."},
        {L"Resistance icons", false, "Wk / Rs / Nu / Dr / Rp badges as coloured icons."},
        {L"Level-up refills HP / EP", false, "A level-up in battle restores full HP and EP."},
        {L"Random music for bosses", false, "Off: boss and story battles keep their own music."},
        {L"Character rim", false, "The light outline around characters. Off removes it."},
        {L"UI animation speed", false, "Menus, dialogue, HUD. 60 Hz = default, Switch = half speed."},
        {L"MSAA", false, "Smoother 3D edges, costs fps and VRAM. Off while Anti-aliasing is FSR 3 / DLSS."},
        {L"FSR 3 / DLSS upscaling", false, "With FSR 3 / DLSS: renders 3D smaller and upscales it, more fps. Native = DLAA."},
        {L"Sub Cast EXP", false, "On: the whole party gets battle EXP. Off: only the front line."},
    },
    {
        // rows 0-2 as sliders (value in the help line): On/Off rows there borrow the option text of empty rows, none left
        {L"HDR", true, "HDR screens: bright lights shine above white. Needs Windows HDR on."},
        {L"HDR paper white", true, "With HDR: brightness of normal white (menus, text), in nits."},
        {L"HDR peak brightness", true, "With HDR: your screen's brightest nits. Auto = from Windows."},
        {L"EXP level-gap penalty", false, "Off: members above the enemy's level still get full EXP."},
        {L"Retry lost battles", false, "After a wipe: retry the battle with its starting HP, EP and items."},
        {L"Revive at 1 HP after battles", false, "On: members who fell get up with 1 HP after a won or escaped battle."},
        {L"Carry 99 of each item", false, "On: every consumable stocks up to 99. Off: the game's limits (40, 20, 5...)."},
        {L"Overworld costume", false, "On: outside battle the party wears the costume equipped for battle."},
        {L"Resist markers", false, "Shield over foes that resist the skill, RESIST on resisted hits."},
        {L"P5-style battle buttons", false, "One button per battle command. P5 panel: a Persona 5 style panel replaces the wheel."},
        {L"Language", false, "Game text and menus (voices stay). Applies after a restart."},
    },
    {
        // PC 5/5 (user 11-28: the 0.1.6 settings were only in the F1 menu and the launcher)
        {L"3D resolution", true, "3D scene size. Auto: lower while the GPU cannot keep your fps."},
        {L"HDR highlights", true, "With HDR: how bright the 3D scene's highlights get."},
        {L"HDR colour", true, "With HDR: more vivid scene colours."},
        {L"Auto 30 when slow", false, "At 60 fps: 30 at full game speed while 60 is not held."},
        {L"Crowd density", false, "Fewer walkers and cars in town, for weak PCs. From the next area on."},
        {L"Shadow detail", false, "Low: no far shadow layer (shadows near you stay), for weak PCs."},
        {L"WEAK in the skill list", false, "WEAK next to skills that hit a weakness you already know."},
        {L"Reveal dungeon maps", false, "Every Idolasphere floor shows its whole map when entered."},
        {L"Attack Session", false, "On: a regular Attack can start a Session. Off: the original rule."},
        {L"Aspect ratio", false, "16:9 or ultrawide 3D view (HUD and menus stay 16:9)."},
        {L"Outline", false, "The dark edge lines around characters and objects."},
    },
    {
        // PC 6/6 (Promise 11-28: no black bars at 21:9)
        {L"Ultrawide sides", false, "Ultrawide: a blurred copy of 16:9 pictures and movies at the sides."},
    },
};
constexpr int kPcPages = (int)(sizeof k_pages / sizeof k_pages[0]);
constexpr uint32_t ITEM_TABLE = 0x7ddd98;  // 11 records of 40 bytes: kind, label, help, flag, count, 5 option labels
constexpr uint32_t HELP_OBJ = 0x9efb50;    // **: the camp help line (0x319d54)
constexpr uint32_t MSG_FILE = 0x9efb60;    // **: Camp.msbt (msg file object; +4 = its data)
constexpr uint32_t BTN_L = 1 << 6, BTN_R = 1 << 7;
const char k_hint_game[] = "  (L / R: PC settings)";
const char* const k_hint_pc[kPcPages] = {"PC 1/6 (L / R: pages)", "PC 2/6 (L / R: pages)", "PC 3/6 (L / R: pages)", "PC 4/6 (L / R: pages)", "PC 5/6 (L / R: pages)", "PC 6/6 (L / R: pages)"};  // put first (long texts get cut)

uint32_t g_screen = 0;
int g_page = -1;  // -1 the game's page, 0 / 1 ours
bool g_in_update = false;
int g_pending = -1;       // our page under the game's A dialog (rows hold the game's values meanwhile)
uint32_t g_saved[11][4];  // game rows: value, arrows, min, max
int g_cur[11], g_count[11];
uint64_t g_prev_buttons = 0;
uint32_t g_txt_data = 0;              // msg data whose TXT2 we rebuilt
std::vector<std::string> g_txt_orig;  // its original texts (each with its terminator)
uint32_t g_txt_arena = 0, g_txt_cap = 0;  // our TXT2 block
std::map<std::string, uint32_t> g_strs;  // guest copies of our strings (operator new, kept)

const PageRow& prow(int i) { return k_pages[g_page][i]; }
uint32_t row(uint32_t s, int i) { return s + 0x1c + i * 0x18; }
int slider_value(int i) { return g_count[i] > 1 ? (g_cur[i] * 10 + (g_count[i] - 1) / 2) / (g_count[i] - 1) : 0; }
uint32_t guest_str(Cpu* c, const std::string& s) {
    uint32_t& p = g_strs[s];
    if (!p) p = gcall(c, 0x5a8720, {(uint32_t)s.size() + 1}), memcpy(GPTR(p), s.c_str(), s.size() + 1);
    return p;
}

// Camp.msbt TXT2: msg data +0xc = section table (16 bytes each), +0x14 = TXT2's index; table entry {+0 block, +8
// block size}; block = {count, offsets[count] (from the block), texts (UTF-8 + 0)}, a text runs to the next offset
// (the last to the size). We build a whole new block (same layout) and point the table entry at it, so every
// reader sees a consistent block; the file's own block stays as it was.
uint32_t txt2_entry(uint32_t data) { return ld32(data + 0xc) + ld32(data + 0x14) * 0x10; }
// msbt texts may hold tags with 0 bytes inside (button icons): only the trailing terminator goes
std::string without_terminator(std::string s) {
    while (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}
// Help texts: every CAMP_SETTING_HELP_* gets the page hint; help_row >= 0: that row's help is our text.
void rebuild_help(Cpu* c, int help_row, const std::string& text) {
    const uint32_t file = ld32(ld32(MSG_FILE)), data = file ? ld32(file + 4) : 0;
    if (!data || (int32_t)ld32(data + 0x14) < 0) return;
    const uint32_t ent = txt2_entry(data), sec = ld32(ent), n = ld32(sec);
    if (!n || n > 5000) return;
    // first time for this file (or it was loaded again: the entry does not point at our block): keep the originals
    if (data != g_txt_data || sec != g_txt_arena) {
        g_txt_data = data, g_txt_orig.assign(n, {}), g_txt_arena = 0, g_txt_cap = 0;
        const uint32_t size = ld32(ent + 8);
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t a = sec + ld32(sec + 4 + 4 * i), e = sec + (i + 1 < n ? ld32(sec + 8 + 4 * i) : size);
            g_txt_orig[i].assign((const char*)GPTR(a), e - a);
        }
    }
    std::vector<std::string> t = g_txt_orig;
    for (int r = 0; r < 11; r++) {
        const uint32_t idx = gcall(c, 0x5c22e8, {data, ld32(ITEM_TABLE + r * 40 + 8)});  // label -> index
        if (idx >= n) continue;
        const std::string base = r == help_row ? std::string(k_hint_pc[g_page]) + "  " + text : without_terminator(t[idx]) + k_hint_game;
        t[idx] = base + '\0';
    }
    uint32_t total = 4 + 4 * n;
    for (auto& s : t) total += (uint32_t)s.size();
    if (!g_txt_arena || total > g_txt_cap) g_txt_cap = total + 4096, g_txt_arena = gcall(c, 0x5a8720, {g_txt_cap});
    if (!g_txt_arena) return;
    const uint32_t b = g_txt_arena;
    st32(b, n);
    uint32_t off = 4 + 4 * n;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(GPTR(b + off), t[i].data(), t[i].size());
        st32(b + 4 + 4 * i, off);
        off += (uint32_t)t[i].size();
    }
    st32(ent + 8, off), st32(ent, b);
}
void show_game_help(Cpu* c, uint32_t s) {
    const int i = (int)ld32(s + 0x124);
    if (i < 0 || i > 10) return;
    const uint32_t obj = ld32(ld32(HELP_OBJ)), file = ld32(ld32(MSG_FILE));
    c->r[13] -= 8;  // 0x319d54(help, 7, 0, label, file): the 5th argument goes on the stack
    st32(c->r[13], file);
    gcall(c, 0x319d54, {obj, 7, 0, ld32(ITEM_TABLE + i * 40 + 8)});
    c->r[13] += 8;
}
void show_help(Cpu* c, uint32_t s) {
    const int i = (int)ld32(s + 0x124);
    if (i < 0 || i > 10) return;
    std::string h = prow(i).help;
    SettingItem it;
    if (prow(i).name && prow(i).slider && overlay_setting(prow(i).name, it) && g_cur[i] < (int)it.choices.size())
        h = it.name + ": " + it.choices[g_cur[i]] + ". " + h;
    rebuild_help(c, i, h);
    show_game_help(c, s);
}
void show_option(Cpu* c, uint32_t s, int i) {
    SettingItem it;
    const uint32_t opt = ld32(row(s, i) + 4);
    if (!opt) return;
    if (!prow(i).name) { gcall(c, 0x586488, {opt, guest_str(c, "")}); return; }
    if (!overlay_setting(prow(i).name, it) || g_cur[i] >= (int)it.choices.size()) return;
    gcall(c, 0x586488, {opt, guest_str(c, it.choices[g_cur[i]])});
}

// On/Off items on rows 0-2: the option text object of an empty row (9, 8, 7; row 10 has another font) moves there while
// our page is up. The draw 0x33cb14 draws a row's option text whenever +4 is set; a text object's position is +0x50 x /
// +0x54 y (option x 850, rows 50 apart at 720p; same y as the row's label), used when its text is set.
struct Lent { int from = -1; float y = 0; };
Lent g_lent[3];
void return_options(Cpu* c, uint32_t s) {  // back home blank (our text would show there under the A dialog)
    for (int i = 0; i < 3; i++) {
        Lent& l = g_lent[i];
        if (l.from < 0) continue;
        const uint32_t o = ld32(row(s, i) + 4);
        if (o) memcpy(GPTR(o + 0x54), &l.y, 4), gcall(c, 0x586488, {o, guest_str(c, "")});
        st32(row(s, l.from) + 4, o), st32(row(s, i) + 4, 0);
        l = {};
    }
}
void lend_options(uint32_t s) {
    int donor = 9;
    for (int i = 0; i < 3; i++) {
        if (!prow(i).name || prow(i).slider || ld32(row(s, i) + 4) || !ld32(row(s, i))) continue;
        while (donor > 2 && (prow(donor).name || !ld32(row(s, donor) + 4))) donor--;
        if (donor <= 2) return;
        const uint32_t o = ld32(row(s, donor) + 4);
        g_lent[i].from = donor;
        memcpy(&g_lent[i].y, GPTR(o + 0x54), 4);
        memcpy(GPTR(o + 0x54), GPTR(ld32(row(s, i)) + 0x54), 4);  // the label's y
        st32(row(s, i) + 4, o), st32(row(s, donor) + 4, 0);
        donor--;
    }
}

// show PC page `page` in the rows (from the game's page: keep its rows first)
void enter_pc(Cpu* c, uint32_t s, int page) {
    return_options(c, s);
    for (int i = 0; i < 11 && g_page < 0; i++) {
        const uint32_t r = row(s, i);
        g_saved[i][0] = ld32(r + 8), g_saved[i][1] = ld32(r + 0xc), g_saved[i][2] = ld32(r + 0x10), g_saved[i][3] = ld32(r + 0x14);
    }
    g_page = page;
    lend_options(s);
    for (int i = 0; i < 11; i++) {
        const uint32_t r = row(s, i);
        SettingItem it;
        if (!prow(i).name) it.name = "", it.choices = {""};
        else if (!overlay_setting(prow(i).name, it)) it.name = "?", it.choices = {"-"};
        g_count[i] = (int)it.choices.size(), g_cur[i] = std::max(0, std::min(it.cur, g_count[i] - 1));
        st32(r + 0xc, prow(i).name ? 1 : 0), st32(r + 0x10, 0);
        if (prow(i).slider) st32(r + 8, slider_value(i)), st32(r + 0x14, 10);
        else st32(r + 8, g_cur[i]), st32(r + 0x14, std::min(4, g_count[i] - 1));  // the game's option table has 5 slots
        if (ld32(r)) gcall(c, 0x586488, {ld32(r), guest_str(c, it.name)});
        if (!prow(i).slider) show_option(c, s, i);
    }
    show_help(c, s);
    log("camp settings: PC page %d", page + 1);
}
void leave_pc(Cpu* c, uint32_t s, bool help) {
    return_options(c, s);
    const uint32_t file = ld32(ld32(MSG_FILE));
    for (int i = 0; i < 11; i++) {
        const uint32_t r = row(s, i), rec = ITEM_TABLE + i * 40;
        st32(r + 8, g_saved[i][0]), st32(r + 0xc, g_saved[i][1]), st32(r + 0x10, g_saved[i][2]), st32(r + 0x14, g_saved[i][3]);
        // row 10 (difficulty) takes its options from another message file
        const uint32_t ofile = i == 10 ? ld32(ld32(0x9ef380) + 8) : file;
        if (ld32(r)) gcall(c, 0x586818, {ld32(r), file, ld32(rec + 4)});
        const uint32_t opt = ld32(r + 4), v = g_saved[i][0];
        if (opt && v < 5) gcall(c, 0x586818, {opt, ofile, ld32(rec + 0x14 + 4 * v)});
    }
    g_page = -1;
    rebuild_help(c, -1, {});
    if (help) show_game_help(c, s);
    log("camp settings: game page");
}
}  // namespace

// Settings screen update (S = r0): state S+4 (2 = browsing), cursor row S+0x124.
HLE_FN(camp_settings_update, "camp_settings_update") {
    const uint32_t s = c->r[0];
    const uint64_t b = g_pad_game_buttons, pressed = b & ~g_prev_buttons;
    g_prev_buttons = b;
    const Cpu keep = *c;
    if (s != g_screen) {  // a new screen: game page, help lines with the hint
        g_screen = s, g_page = -1, g_pending = -1;
        for (Lent& l : g_lent) l = {};
        rebuild_help(c, -1, {});
        if (ld32(s + 4) == 2) show_game_help(c, s);
        *c = keep;
    }
    // the A dialog is gone; No / B pass 7 (closing) first: wait for 2, or the "not browsing" branch below would leave
    // our page again in the same update (s94: B on the dialog showed the game page)
    if (g_pending >= 0 && ld32(s + 4) != 4 && ld32(s + 4) != 5 && ld32(s + 4) != 7) {
        if (ld32(s + 4) == 2) enter_pc(c, s, g_pending);  // No / B: our page again
        else rebuild_help(c, -1, {});  // Yes: applied, the screen closes
        g_pending = -1;
        *c = keep;
    }
    if (ld32(s + 4) == 2 && (pressed & (BTN_L | BTN_R))) {
        const int next = (g_page + 1 + ((pressed & BTN_R) ? 1 : kPcPages)) % (kPcPages + 1) - 1;  // -1 game, 0 .. kPcPages - 1
        if (next < 0) leave_pc(c, s, true);
        else enter_pc(c, s, next);
        *c = keep;
    }
    const int cur_before = (int)ld32(s + 0x124);
    uint32_t before[11];
    for (int i = 0; i < 11; i++) before[i] = ld32(row(s, i) + 8);
    g_in_update = true;
    const uint32_t ret = call_unhooked(c, "camp_settings_update", {s});
    g_in_update = false;
    if (g_page < 0) { c->r[0] = ret; return; }
    const Cpu after = *c;
    if (ld32(s + 4) == 4 && ld32(s + 0x134) == 1) {  // A: the game's apply dialog sees its own values
        return_options(c, s);
        for (int i = 0; i < 11; i++)
            for (int k = 0; k < 4; k++) st32(row(s, i) + 8 + 4 * k, g_saved[i][k]);
        g_pending = g_page, g_page = -1;
        log("camp settings: apply dialog from PC page %d", g_pending + 1);
    } else if (ld32(s + 4) != 2) {
        leave_pc(c, s, false);  // B / Y defaults: the game's own rows from here on
    } else {
        bool changed = false;
        for (int i = 0; i < 11; i++) {
            const uint32_t v = ld32(row(s, i) + 8);
            if (v == before[i] || !prow(i).name) continue;
            if (prow(i).slider) {  // one choice per press, the bar shows where it is
                g_cur[i] = std::max(0, std::min(g_count[i] - 1, g_cur[i] + ((int)v > (int)before[i] ? 1 : -1)));
                st32(row(s, i) + 8, slider_value(i));
            } else {
                g_cur[i] = (int)v;
                show_option(c, s, i);
            }
            overlay_apply_setting(prow(i).name, g_cur[i]);
            log("camp settings: %ls -> %d", prow(i).name, g_cur[i]);
            changed = true;
        }
        if (changed || (int)ld32(s + 0x124) != cur_before) show_help(c, s);
    }
    *c = after;
    c->r[0] = ret;
}

// The Settings screen's voice preview (row 2 = voice volume) plays an Itsuki line: not for our rows (user report 10-30).
HLE_FN(sound_play_cue, "sound_play_cue") {
    if (g_in_update && g_page >= 0) { c->r[0] = 0; return; }
    c->r[0] = call_unhooked(c, "sound_play_cue", {c->r[0], c->r[1]});
}

// The Settings screen's volume preview: not for our slider rows.
HLE_FN(sound_volume_set, "sound_volume_set") {
    if (g_in_update && g_page >= 0) return;
    c->r[0] = call_unhooked(c, "sound_volume_set", {c->r[0], c->r[1], c->r[2], c->r[3]});
}

// Empty slider rows (PC 4/4 rows 0-2; USER 11-21: "3 blank volume sliders, ugly"): the draw 0x33cb14 always draws rows 0-2 as
// sliders (row < 3: background 0x4e6c / table kind 0x4e6a, bar ui_draw_gauge, knob ui_draw_part; row index in r9 at those
// calls). On our pages an unnamed or On/Off row gets the choice-row background and no bar or knob, like rows 3-10.
static bool empty_slider_row(const Cpu* c) {
    return g_page >= 0 && c->r[9] < 3 && (!k_pages[g_page][c->r[9]].name || !k_pages[g_page][c->r[9]].slider);
}
HLE_FN(ui_draw_sprite, "ui_draw_sprite") {
    if (c->r[14] == 0x33cccc && empty_slider_row(c)) c->r[0] = c->r[0] == 0x4e6c ? 0x4e6d : 0x4e6b;
    c->r[0] = call_unhooked(c, "ui_draw_sprite", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
HLE_FN(ui_draw_gauge, "ui_draw_gauge") {
    if (c->r[14] == 0x33cd80 && empty_slider_row(c)) return;
    c->r[0] = call_unhooked(c, "ui_draw_gauge", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
HLE_FN(ui_draw_part, "ui_draw_part") {
    if (c->r[14] == 0x33cda0 && empty_slider_row(c)) return;
    c->r[0] = call_unhooked(c, "ui_draw_part", {c->r[0], c->r[1], c->r[2], c->r[3]});
}
