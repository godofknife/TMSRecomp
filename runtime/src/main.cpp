// Boot: map the prelinked image, then do what rtld does and enter nn::init::Start on the main thread.
#include "plat.h"
#ifdef _WIN32
#include <dbghelp.h>
#include <psapi.h>
#include <shlobj.h>
#include <timeapi.h>
#else
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <thread>
#include <unordered_map>
#include <vector>

#include "rt.h"
#include "settings.h"
#include "sysinfo.h"

namespace rt {

int g_verbose = 0;
static FILE* g_log;
static FILE* g_issues;  // <data>/issues.log: only problem lines, timestamped (play.bat shows it live)
static bool g_own_console;  // started without a console (double-click): fatal errors also show a message box
static std::mutex g_log_lock;
std::filesystem::path g_data;  // private/tms (image, romfs)

// A log line that reports a problem: crashes, stubs, unimplemented calls, failed translations, skipped work, CRI
// errors. "vk: skipped:" counters only when one of them is non-zero.
static bool is_issue(const char* s) {
    if (strstr(s, "#CRIATOM")) return false;  // CRI's API trace through the game log, not errors
    if (!strncmp(s, "vk: skipped: ", 13)) {
        for (const char* p = s + 13; *p; p++)
            if (*p >= '1' && *p <= '9' && (p[-1] == ' ')) return true;
        return false;
    }
    static const char* words[] = {"FATAL", "CRASH", "stub", "unimplemented", "unknown", "failed", "error", "[cri]",
                                  "skipped", "bad handle", "cannot", "not found"};
    for (const char* w : words)
        if (strstr(s, w)) return true;
    return false;
}

// Same message with other numbers counts as a repeat: written 3 times, then at 10, 100, 1000... repeats.
static void issue(const char* s) {
    static std::unordered_map<std::string, uint64_t> seen;
    static int tail = 0;  // lines after FATAL/CRASH (backtrace) are copied as they are
    static const unsigned long long t0 = GetTickCount64();
    if (!g_issues) return;
    bool cont = tail > 0 && s[0] == ' ';
    if (!cont && !is_issue(s)) { tail = 0; return; }
    uint64_t n = 0;
    if (cont) tail--;
    else {
        if (!strncmp(s, "FATAL", 5) || !strncmp(s, "CRASH", 5)) tail = 64;
        std::string key(s);
        for (char& ch : key)
            if (isxdigit((unsigned char)ch)) ch = '#';
        n = ++seen[key];
        if (n > 3 && n != 10 && n != 100 && n != 1000 && n != 10000 && n != 100000) return;
    }
    // seen on every boot since milestone 1, with no visible effect
    static const char* known[] = {"unimplemented import nnosGetDefaultUserExceptionStackForApplication",
                                  "unimplemented import glslcSetAllocator", "ipc lm cmd 0:", "ipc set cmd 2:",
                                  "ipc set cmd 5:", "nvn stub nvnProgramFinalize"};
    const char* tag = "";
    for (const char* k : known)
        if (!strncmp(s, k, strlen(k))) tag = "(known, harmless) ";
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
#else
    struct { unsigned wHour, wMinute, wSecond; } st;
    { time_t now = time(nullptr); tm lt{}; localtime_r(&now, &lt); st = {(unsigned)lt.tm_hour, (unsigned)lt.tm_min, (unsigned)lt.tm_sec}; }
#endif
    unsigned long long ms = GetTickCount64() - t0;
    fprintf(g_issues, "[%02u:%02u:%02u run %llu:%02llu] %s%s", st.wHour, st.wMinute, st.wSecond, ms / 60000, ms / 1000 % 60,
            tag, s);
    if (n > 3) fprintf(g_issues, "   (seen %llu times)", (unsigned long long)n);
    fputc('\n', g_issues);
    fflush(g_issues);
}

void log(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_log_lock);
    fprintf(stderr, "%s\n", buf);
    // NX_LOG_TIME=1: prefix log-file lines with seconds since start (timing comparisons, e.g. 30 vs 60 fps)
    static const bool stamp = getenv("NX_LOG_TIME") != nullptr;
    static const auto t0 = std::chrono::steady_clock::now();
    if (g_log && stamp) fprintf(g_log, "%9.3f ", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
    issue(buf);
}

// Keep the previous run's logs: <file> -> <data>/logs/<stem>_<its last write time><ext>.
static void archive_log(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || std::filesystem::file_size(p, ec) == 0) return;
    auto t = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(p, ec));
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&tt));
    auto dir = g_data / "logs";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::rename(p, dir / (p.stem().string() + "_" + stamp + p.extension().string()), ec);
}

// guest call stack from the frame-pointer chain (clang's `push {.., fp, lr}; add fp, sp, #n`: [fp] = caller fp, [fp+4] = lr)
void guest_backtrace(const Cpu& c) {
    log("  pc~lr %s", symbol_at(c.r[14]));
    uint32_t fp = c.r[11];
    for (int i = 0; i < 24 && fp >= 0x00200000 && fp < 0xFFFF0000u && (fp & 3) == 0; i++) {
        Region r = mem_query(fp);
        if (r.state == MS_FREE) break;
        uint32_t lr = ld32(fp + 4), up = ld32(fp);
        log("  #%-2d %s", i, symbol_at(lr));
        if (up <= fp) break;
        fp = up;
    }
}

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log("FATAL: %s", buf);
    if (KThread* t = cur()) { log("  thread %s", t->name.c_str()); guest_backtrace(t->cpu); }
    fflush(nullptr);
#ifdef _WIN32
    if (g_own_console) MessageBoxA(nullptr, buf, "TMSRecomp: fatal error", MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 3);
#else
    if (g_own_console) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TMSRecomp: fatal error", buf, nullptr);
    _exit(3);
#endif
    for (;;) {}
}

std::string g_data_dir;

// guest-callable host functions (rtld's callbacks, NVN procs). The tag tells a shared function which entry ran.
static std::mutex g_cb_lock;
static std::unordered_map<uint32_t, std::pair<HleFn, uintptr_t>> g_host_cb;
static thread_local uintptr_t t_cb_tag;
uint32_t host_callback(HleFn fn, uintptr_t tag) {
    uint32_t a = hle_alloc(4, 4);
    std::lock_guard<std::mutex> l(g_cb_lock);
    g_host_cb[a] = {fn, tag};
    return a;
}
uint32_t host_callback(HleFn fn) { return host_callback(fn, 0); }
HleFn host_callback_at(uint32_t a) {
    // s117: every NVN call through a proc pointer lands here (2-3% of the game thread with the lock). Entries are never
    // changed or removed once added, so a per-thread cache of hits never goes stale.
    struct Hit { uint32_t a; HleFn fn; uintptr_t tag; };
    thread_local Hit hits[256];
    Hit& h = hits[(a >> 2) & 255];
    if (h.a == a && h.fn) { t_cb_tag = h.tag; return h.fn; }
    std::lock_guard<std::mutex> l(g_cb_lock);
    auto it = g_host_cb.find(a);
    if (it == g_host_cb.end()) return nullptr;
    h = {a, it->second.first, it->second.second};
    t_cb_tag = it->second.second;
    return it->second.first;
}
uintptr_t host_callback_tag() { return t_cb_tag; }

uint32_t gcall(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    int i = 0;
    for (uint32_t a : args) c->r[i++] = a;
    call_guest(c, fn);
    return c->r[0];
}
uint32_t gcall(Cpu* c, const char* sym, std::initializer_list<uint32_t> args) {
    uint32_t a = export_addr(sym);
    if (!a) fatal("gcall: no symbol %s", sym);
    return gcall(c, a, args);
}
void attach_system_event(Cpu* c, uint32_t sysevent, uint32_t readable, bool autoclear) {
    gcall(c, "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE",
          {sysevent, readable, 1, autoclear ? 1u : 0u});
}
void set_r64(Cpu* c, uint64_t v) { c->r[0] = (uint32_t)v; c->r[1] = (uint32_t)(v >> 32); }

void call_guest(Cpu* c, uint32_t addr) {
    uint32_t lr = c->r[14];
    c->r[14] = 0xFFFFFFF0u;
    dispatch(c, addr);
    c->r[14] = lr;
}

static void load_image() {
    auto p = g_data / "link" / "image.bin";
    std::ifstream f(p, std::ios::binary);
    if (!f) fatal("cannot open %s (run the import step first)", p.string().c_str());
    std::vector<char> img((std::istreambuf_iterator<char>(f)), {});
    if (img.size() != g_image_size) fatal("image.bin is %zu bytes, the build expects %u: rebuild after re-import", img.size(), g_image_size);
    for (uint32_t i = 0; i < g_module_count; i++) {
        const GuestModule& m = g_modules[i];
        mem_map(m.text_lo, m.text_hi - m.text_lo + 0xFFF & ~0xFFFu, MS_CODE, 5);
        mem_map(m.ro_lo, (m.ro_hi - m.ro_lo + 0xFFF) & ~0xFFFu, MS_CODE, 1);
        mem_map(m.data_lo, m.data_hi - m.data_lo, MS_CODEDATA, 3);
    }
    uint32_t end = g_modules[g_module_count - 1].base + g_modules[g_module_count - 1].size;
    uint32_t thunks = (end + 0xFFFFF) & ~0xFFFFFu;
    if (img.size() > thunks - IMAGE_BASE) mem_map(thunks, (uint32_t)img.size() - (thunks - IMAGE_BASE) + 0xFFF & ~0xFFFu, MS_CODE, 5);
    for (uint32_t i = 0; i < g_module_count; i++)  // modules are 1 MiB aligned with uncommitted gaps between
        memcpy(GPTR(g_modules[i].base), img.data() + (g_modules[i].base - IMAGE_BASE), g_modules[i].size);
    if (img.size() > thunks - IMAGE_BASE)
        memcpy(GPTR(thunks), img.data() + (thunks - IMAGE_BASE), img.size() - (thunks - IMAGE_BASE));
    log("image: %zu bytes, %u modules", img.size(), g_module_count);
}

}  // namespace rt

using namespace rt;

// NX_CONTROL "watch addr len [r]" (hex guest address, byte length; "watch off" stops): log the code that writes (r: reads
// or writes) guest memory [addr, addr+len), to find frame-counted game code. The host page is guarded; a hit logs the
// generated function (F_<guest address> from tms.pdb) once per host instruction, then runs that instruction
// single-stepped with the page open and guards it again. Other threads touching the page in that window are missed.
#ifdef _WIN32
static struct { uint32_t lo = 0, hi = 0; bool reads = false; uint8_t* page = nullptr; } g_watch;
static thread_local bool t_watch_step;
static std::mutex g_watch_lock;
static DWORD watch_protect() { return g_watch.reads ? PAGE_NOACCESS : PAGE_READONLY; }

static bool sym_init() {  // dbghelp symbols (tms.pdb) for watch, guest_chain and the profiler: initialised once
    static const bool ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    return ok;
}

// NX_PROFILE=1: sampling profiler of the calling thread (the game thread, from pace_present). A sampler thread stops it
// every millisecond, records the host instruction and the innermost generated function (F_<guest addr>) on its stack,
// and every 10 s logs the top entries of both (where frame time goes: guest code, runtime, waits).
// NX_PROFILE=2 (s115): the thread that submits NVN command lists (the replay runs there) instead of the game thread.
void rt::profile_start(bool submit) {
    static const char* e = getenv("NX_PROFILE");
    static bool on = e != nullptr, started = false;
    if (!on || started || submit != (atoi(e) == 2)) return;
    started = true;
    HANDLE target;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread([target] {
        HANDLE proc = GetCurrentProcess();
        sym_init();
        timeBeginPeriod(1);
        MODULEINFO mi{}, gi{};
        GetModuleInformation(proc, GetModuleHandleA(nullptr), &mi, sizeof mi);
        GetModuleInformation(proc, GetModuleHandleA("game.dll"), &gi, sizeof gi);
        uint64_t lo = (uint64_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;      // runtime
        uint64_t glo = (uint64_t)gi.lpBaseOfDll, ghi = glo + gi.SizeOfImage;  // generated code
        auto ours = [&](DWORD64 a) { return (a >= lo && a < hi) || (a >= glo && a < ghi); };
        // address -> (function start, is generated guest code); filled after the target runs again (no locks held)
        std::unordered_map<DWORD64, std::pair<DWORD64, bool>> syms;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* si = (SYMBOL_INFO*)buf;
        auto lookup = [&](DWORD64 a) -> std::pair<DWORD64, bool>& {
            auto [it, added] = syms.try_emplace(a, std::pair<DWORD64, bool>{a, false});
            if (added) {
                si->SizeOfStruct = sizeof(SYMBOL_INFO), si->MaxNameLen = 255;
                DWORD64 disp;
                if (SymFromAddr(proc, a, &disp, si)) it->second = {si->Address, !strncmp(si->Name, "F_", 2)};
            }
            return it->second;
        };
        std::unordered_map<DWORD64, uint32_t> leaf, guest, caller;  // caller: runtime function under a leaf outside our modules
        static uint64_t stack[1024];
        uint32_t n = 0;
        auto last = std::chrono::steady_clock::now();
        // spikes: a game frame (pace_present to pace_present) over NX_PROFILE_SPIKE ms (default 20) logs its own samples
        const double spike_ms = getenv("NX_PROFILE_SPIKE") ? atof(getenv("NX_PROFILE_SPIKE")) : 20.0;
        std::vector<std::array<DWORD64, 3>> frame;  // (leaf, guest, runtime caller of a non-tms.exe leaf) samples of the current game frame
        uint64_t frame_no = g_frame_no;
        auto frame_t0 = last;
        auto spike = [&](double ms) {
            std::unordered_map<std::string, uint32_t> agg;
            for (auto& [lf, g, h] : frame) {
                std::string k;
                DWORD64 disp;
                for (int j = 0; j < 3; j++) {
                    DWORD64 a = j == 0 ? g : j == 1 ? h : lf;
                    if (j == 1 && !a) continue;
                    si->SizeOfStruct = sizeof(SYMBOL_INFO), si->MaxNameLen = 255;
                    std::string nm = !a ? "(no guest frame)" : SymFromAddr(proc, a, &disp, si) ? si->Name : "?";
                    k += (j ? " / " : "") + nm.substr(0, 60);
                }
                agg[k]++;
            }
            std::vector<std::pair<uint32_t, std::string>> v;
            for (auto& [k, c] : agg) v.push_back({c, k});
            std::sort(v.rbegin(), v.rend());
            std::string s;
            char b[600];
            for (size_t i = 0; i < v.size() && i < 8; i++) { snprintf(b, sizeof b, "\n    %3u %s", v[i].first, v[i].second.c_str()); s += b; }
            log("[profile] spike frame %.1f ms, %zu samples (guest / runtime caller / leaf):%s", ms, frame.size(), s.c_str());
        };
        for (;;) {
            Sleep(1);
            if (SuspendThread(target) == (DWORD)-1) return;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            bool ok = GetThreadContext(target, &ctx);
            SIZE_T got = 0;
            if (ok) ReadProcessMemory(proc, (void*)ctx.Rsp, stack, sizeof stack, &got);  // no lock taken
            ResumeThread(target);
            if (!ok) continue;
            leaf[lookup(ctx.Rip).first]++;
            DWORD64 g = lookup(ctx.Rip).second ? lookup(ctx.Rip).first : 0;
            for (size_t i = 0; !g && i < got / 8; i++)  // return addresses into game.dll: first generated function
                if (stack[i] >= glo && stack[i] < ghi && lookup(stack[i]).second) g = lookup(stack[i]).first;
            guest[g]++;
            n++;
            auto now = std::chrono::steady_clock::now();
            if (uint64_t f = g_frame_no; f != frame_no) {
                double ms = std::chrono::duration<double, std::milli>(now - frame_t0).count();
                if (f == frame_no + 1 && ms > spike_ms) spike(ms);
                frame.clear(), frame_no = f, frame_t0 = now;
            }
            DWORD64 h = 0;  // leaf outside our modules (driver, ntdll waits): the runtime function that called into it
            for (size_t i = 0; !h && !ours(ctx.Rip) && i < got / 8; i++)
                if (stack[i] >= lo && stack[i] < hi && !lookup(stack[i]).second) h = lookup(stack[i]).first;
            frame.push_back({lookup(ctx.Rip).first, g, h});
            if (h) caller[h]++;
            if (now - last < std::chrono::seconds(10)) continue;
            last = now;
            auto top = [&](std::unordered_map<DWORD64, uint32_t>& m, const char* what) {
                std::unordered_map<std::string, uint32_t> agg;
                for (auto& [a, c] : m) {
                    si->SizeOfStruct = sizeof(SYMBOL_INFO), si->MaxNameLen = 255;
                    DWORD64 disp;
                    agg[!a ? "(no guest frame)" : SymFromAddr(proc, a, &disp, si) ? si->Name : "?"] += c;
                }
                std::vector<std::pair<uint32_t, std::string>> v;
                for (auto& [k, c] : agg) v.push_back({c, k});
                std::sort(v.rbegin(), v.rend());
                std::string s;
                char b[320];
                for (size_t i = 0; i < v.size() && i < 25; i++) { snprintf(b, sizeof b, "\n    %5.1f%% %s", 100.0 * v[i].first / n, v[i].second.c_str()); s += b; }
                log("[profile] %s, %u samples:%s", what, n, s.c_str());
            };
            top(leaf, "host functions (leaf)");
            top(guest, "innermost guest function");
            top(caller, "runtime caller of a leaf outside tms.exe / game.dll (driver, locks, waits)");
            leaf.clear(), guest.clear(), caller.clear(), n = 0;
        }
    }).detach();
}

#endif  // sprite_command below is plain C++: Linux too
// NX_CONTROL "spr bank sheet n" (debug): the game's 2D sprite records (id = bank * 10^7 + sheet * 10^4 + index; lookup
// 0x59c1a0, manager pointer at 0x9f3320): index, then the 0x70-byte record's first 16 words (rect floats / ints)
void rt::sprite_command(const char* args) {
    int bank = 0, sheet = 0, n = 0;
    if (sscanf(args, "%d %d %d", &bank, &sheet, &n) != 3) return;
    const uint32_t m = ld32(0x9f3320);
    if (!m) { log("spr: no sprite manager"); return; }
    const uint32_t t = ld32(m + 8), b0 = t ? ld32(t + bank * 0xc) : 0, bk = b0 >= 0x10000000 ? ld32(b0 + 0x10) : 0,
                   recs = bk >= 0x10000000 ? ld32(bk + sheet * 0x30 + 0x10) : 0;
    log("spr: manager %08x table %08x b0 %08x bank %08x records %08x", m, t, b0, bk, recs);
    if (!recs) return;
    for (int i = 0; i < n; i++) {
        const uint32_t r = recs + i * 0x70;
        std::string s;
        char b[32];
        for (int k = 0; k < 28; k++) {
            const uint32_t w = ld32(r + 4 * k);
            float f;
            memcpy(&f, &w, 4);
            if (w > 0x3000000 && w < 0x50000000) snprintf(b, sizeof b, " %g", f);
            else snprintf(b, sizeof b, " %x", w);
            s += b;
        }
        log("spr %d: %s", i, s.c_str());
    }
}
#ifdef _WIN32
void rt::dump_command(const char* args) {  // pages that are not committed are written as zeros (snap4.sh, framecount.py)
    unsigned lo = 0, hi = 0;
    char path[260] = "";
    if (sscanf(args, " %x %x %259s", &lo, &hi, path) < 3 || hi <= lo) { log("[dump] usage: dump lo hi file"); return; }
    uint64_t frame = g_frame_no;
    std::vector<uint8_t> snap(hi - (lo & ~0xfffu));  // copy first (fast), write after: one point in game time
    for (uint32_t a = lo & ~0xfffu; a < hi; a += 0x1000) {
        MEMORY_BASIC_INFORMATION mi{};
        bool ok = VirtualQuery(GPTR(a), &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (ok) memcpy(&snap[a - (lo & ~0xfffu)], GPTR(a), std::min<size_t>(0x1000, hi - a));
    }
    uint64_t frame2 = g_frame_no;
    FILE* f = fopen(path, "wb");
    if (!f) { log("[dump] cannot write %s", path); return; }
    fwrite(snap.data(), 1, snap.size(), f);
    fclose(f);
    log("[dump] frames %llu-%llu: %08x-%08x -> %s", (unsigned long long)frame, (unsigned long long)frame2, lo & ~0xfffu, hi, path);
}

void rt::watch_command(const char* args) {
    std::lock_guard<std::mutex> l(g_watch_lock);
    DWORD old;
    if (g_watch.page) VirtualProtect(g_watch.page, 0x1000, PAGE_READWRITE, &old);
    g_watch.page = nullptr;
    unsigned a = 0, n = 4;
    char r[4] = "";
    unsigned off = 0;  // "*ptr+off len": watch at the pointer stored at ptr, plus off
    if (sscanf(args, " *%x+%x %x %3s", &a, &off, &n, r) >= 2) a = ld32(a) + off;
    else if (sscanf(args, " %x %x %3s", &a, &n, r) < 1) a = 0;
    if (!a) { log("[watch] off"); return; }
    static bool sym = sym_init();
    if (!sym) log("[watch] no symbols (SymInitialize failed)");
    g_watch.lo = a;
    g_watch.hi = a + std::max(1u, n);
    g_watch.reads = r[0] == 'r';
    g_watch.page = (uint8_t*)((uintptr_t)GPTR(a) & ~(uintptr_t)0xFFF);
    if ((g_watch.hi - 1) >> 12 != a >> 12) log("[watch] range crosses a page: only %08x.. is watched", a);
    if (!VirtualProtect(g_watch.page, 0x1000, watch_protect(), &old)) { log("[watch] VirtualProtect failed"); g_watch.page = nullptr; }
    else log("[watch] %08x-%08x %s", g_watch.lo, g_watch.hi, g_watch.reads ? "reads + writes" : "writes");
}

// Guest call chain for logs: the generated functions (F_<guest addr>) on the host stack, innermost first.
std::string rt::guest_chain() {
    static bool sym = sym_init();
    if (!sym) return "";
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* si = (SYMBOL_INFO*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    void* frames[48];
    std::string chain;
    for (USHORT i = 0, n = RtlCaptureStackBackTrace(0, 48, frames, nullptr); i < n; i++)
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &disp, si) && !strncmp(si->Name, "F_", 2))
            chain += std::string(" ") + (si->Name + 2);
    return chain;
}

static LONG WINAPI on_watch(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_step) {
        t_watch_step = false;
        DWORD old;
        if (g_watch.page) VirtualProtect(g_watch.page, 0x1000, watch_protect(), &old);
        e->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_watch.page || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* fa = (uint8_t*)r->ExceptionInformation[1];
    if (fa < g_watch.page || fa >= g_watch.page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t ga = (uint32_t)(fa - (uint8_t*)GUEST_BASE);
    bool write = r->ExceptionInformation[0] == 1;
    if (ga >= g_watch.lo && ga < g_watch.hi && (write || g_watch.reads)) {
        std::lock_guard<std::mutex> l(g_watch_lock);
        static std::unordered_map<uintptr_t, uint32_t> seen;  // hits per host instruction
        uint32_t k = ++seen[(uintptr_t)r->ExceptionAddress];
        if (k == 1 || k == 100 || k == 10000) {
            alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
            auto* si = (SYMBOL_INFO*)buf;
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            const char* name = SymFromAddr(GetCurrentProcess(), (DWORD64)r->ExceptionAddress, &disp, si) ? si->Name : "?";
            KThread* t = cur();
            log("[watch] %s %08x by %s+%#llx x%u (guest lr %s)", write ? "write" : "read", ga, name,
                (unsigned long long)disp, k, t ? symbol_at(t->cpu.r[14]) : "-");
            std::string chain = guest_chain();
            if (!chain.empty()) log("[watch]   chain%s", chain.c_str());
        }
    }
    DWORD old;
    VirtualProtect(g_watch.page, 0x1000, PAGE_READWRITE, &old);
    t_watch_step = true;
    e->ContextRecord->EFlags |= 0x100u;  // trap after this instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    if (r->ExceptionCode == 0xE06D7363 || r->ExceptionCode < 0x80000000) return EXCEPTION_CONTINUE_SEARCH;  // C++ throw, debug prints
    uint64_t fa = r->NumberParameters >= 2 ? r->ExceptionInformation[1] : 0;
    char extra[128] = "";
    if (fa >= GUEST_BASE && fa < GUEST_BASE + 0x100010000ull)
        snprintf(extra, sizeof extra, " (guest address %08llx)", fa - GUEST_BASE);
    // "tms.exe+off" / "game.dll+off" (llvm-symbolizer --obj=<module> <off + 0x140000000 exe | 0x180000000 dll>)
    auto where = [](const void* a, char* out, size_t n) {
        static const uintptr_t exe = (uintptr_t)GetModuleHandleA(nullptr), dll = (uintptr_t)GetModuleHandleA("game.dll");
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m);
        if (!m || ((uintptr_t)m != exe && (uintptr_t)m != dll)) return false;
        snprintf(out, n, "%s+%#llx", (uintptr_t)m == exe ? "tms.exe" : "game.dll", (unsigned long long)((uintptr_t)a - (uintptr_t)m));
        return true;
    };
    char at[64] = "?";
    where(r->ExceptionAddress, at, sizeof at);
    log("CRASH: code %08lx at host %p (%s), access %llx%s", r->ExceptionCode, r->ExceptionAddress, at, fa, extra);
    void* frames[24];
    USHORT nf = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string bt;
    char b[64];
    for (USHORT i = 0; i < nf; i++)
        if (where(frames[i], b, sizeof b)) bt += std::string(" ") + b;
    log("  host stack:%s", bt.c_str());
    if (KThread* t = cur()) {
        Cpu& c = t->cpu;
        log("  guest thread %s: lr %s", t->name.c_str(), symbol_at(c.r[14]));
        for (int i = 0; i < 16; i += 4) log("  r%-2d %08x %08x %08x %08x", i, c.r[i], c.r[i + 1], c.r[i + 2], c.r[i + 3]);
        guest_backtrace(c);
    }
    fflush(nullptr);
    if (g_own_console) MessageBoxA(nullptr, "The game crashed. Details are in the log (nx.log in the data folder).", "TMSRecomp", MB_ICONERROR);
    TerminateProcess(GetCurrentProcess(), 4);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else  // Linux
// The Windows debugging aids (NX_PROFILE sampling profiler, NX_CONTROL "watch", guest call chains from tms.pdb) use
// dbghelp; on Linux they report that they are not available. Crashes are reported by a SIGSEGV / SIGBUS / SIGILL /
// SIGFPE handler, which first gives memory.cpp's write watch its faults.
void rt::profile_start(bool) {
    static bool said = false;
    if (getenv("NX_PROFILE") && !said) said = true, log("[profile] not available on Linux (use perf)");
}

void rt::dump_command(const char* args) {  // pages that are not committed are written as zeros
    unsigned lo = 0, hi = 0;
    char path[260] = "";
    if (sscanf(args, " %x %x %259s", &lo, &hi, path) < 3 || hi <= lo) { log("[dump] usage: dump lo hi file"); return; }
    uint64_t frame = g_frame_no;
    std::vector<uint8_t> snap(hi - (lo & ~0xfffu));
    for (uint32_t a = lo & ~0xfffu; a < hi; a += 0x1000)
        if (mem_query(a).state != MS_FREE) memcpy(&snap[a - (lo & ~0xfffu)], GPTR(a), std::min<size_t>(0x1000, hi - a));
    uint64_t frame2 = g_frame_no;
    FILE* f = fopen(path, "wb");
    if (!f) { log("[dump] cannot write %s", path); return; }
    fwrite(snap.data(), 1, snap.size(), f);
    fclose(f);
    log("[dump] frames %llu-%llu: %08x-%08x -> %s", (unsigned long long)frame, (unsigned long long)frame2, lo & ~0xfffu, hi, path);
}

void rt::watch_command(const char*) { log("[watch] not available on Linux"); }
std::string rt::guest_chain() { return ""; }

static void on_crash(int sig, siginfo_t* si, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && mem_fault(si->si_addr)) return;  // a write-watched page: run it again
    static std::atomic<bool> once{false};
    if (once.exchange(true)) _exit(4);  // a crash inside the report
    const uint64_t fa = (uint64_t)si->si_addr;
    const void* pc = (const void*)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
    char extra[128] = "";
    if (fa >= GUEST_BASE && fa < GUEST_BASE + 0x100010000ull)
        snprintf(extra, sizeof extra, " (guest address %08llx)", (unsigned long long)(fa - GUEST_BASE));
    // "tms+off" / "game.so+off" (llvm-symbolizer --obj=<file> <off>)
    auto where = [](const void* a, char* out, size_t n) {
        Dl_info di{};
        if (!dladdr(a, &di) || !di.dli_fname) return false;
        const char* base = strrchr(di.dli_fname, '/');
        snprintf(out, n, "%s+%#llx", base ? base + 1 : di.dli_fname, (unsigned long long)((uintptr_t)a - (uintptr_t)di.dli_fbase));
        return true;
    };
    char at[96] = "?";
    where(pc, at, sizeof at);
    log("CRASH: signal %d at host %p (%s), access %llx%s", sig, pc, at, (unsigned long long)fa, extra);
    void* frames[24];
    int nf = backtrace(frames, 24);
    std::string bt;
    char b[96];
    for (int i = 0; i < nf; i++)
        if (where(frames[i], b, sizeof b)) bt += std::string(" ") + b;
    log("  host stack:%s", bt.c_str());
    if (KThread* t = cur()) {
        Cpu& c = t->cpu;
        log("  guest thread %s: lr %s", t->name.c_str(), symbol_at(c.r[14]));
        for (int i = 0; i < 16; i += 4) log("  r%-2d %08x %08x %08x %08x", i, c.r[i], c.r[i + 1], c.r[i + 2], c.r[i + 3]);
        guest_backtrace(c);
    }
    fflush(nullptr);
    if (g_own_console) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TMSRecomp", "The game crashed. Details are in the log (nx.log in the data folder).", nullptr);
    _exit(4);
}
static void install_crash_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // NODEFER: a watched-page fault inside a handler of ours still works
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(s, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // a helper that died (shader translator): write() fails with an error, as on Windows
    rt::sampler_start();
}
#endif


// settings.ini next to tms.exe: "key = value" lines become NX_KEY unless that environment variable is already set.
// ponytail: ANSI paths only (getenv / _putenv_s); a non-ASCII data folder needs the wide-char API.
static void load_settings(const std::filesystem::path& ini) {
    if (!std::filesystem::exists(ini)) std::ofstream(ini) << k_default_settings;
    std::ifstream f(ini);
    for (std::string line; std::getline(f, line);) {
        line = line.substr(0, line.find_first_of(";#"));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t\r"));
            s.erase(s.find_last_not_of(" \t\r") + 1);
            return s;
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k.empty() || v.empty()) continue;
        std::transform(k.begin(), k.end(), k.begin(), ::toupper);
        if (k.rfind("NX_", 0) != 0) k = "NX_" + k;  // "NX_GPU_PASSES = 1" (an env name pasted as is) works too (#11)
        if (!getenv(k.c_str())) _putenv_s(k.c_str(), v.c_str());
    }
}

static bool is_data(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p / "link" / "image.bin", ec) && std::filesystem::exists(p / "romfs", ec);
}

static std::filesystem::path pick_data_folder() {
#ifndef _WIN32
    return {};  // Linux: no folder dialog; --data, NX_DATA, settings.ini "data =" or <data>/build/tms
#else
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW bi{};
    bi.lpszTitle = L"TMSRecomp: select the game data folder (the one with link\\image.bin and romfs)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::filesystem::path p;
    if (PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi)) {
        wchar_t b[MAX_PATH];
        if (SHGetPathFromIDListW(id, b)) p = b;
        CoTaskMemFree(id);
    }
    return p;
#endif
}

#ifdef _WIN32
int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, on_crash);
    AddVectoredExceptionHandler(1, on_watch);  // first: watch faults are not crashes
    DWORD cons[2];
    if (GetConsoleProcessList(cons, 2) == 1) {  // started on its own (double-click): no console window, errors in a box
        FreeConsole();
        g_own_console = true;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path exe_dir = std::filesystem::path(exe).parent_path(), ini = exe_dir / "settings.ini";
#else
static int tms_main(int argc, char** argv);
// The guest main thread runs on the calling thread and its recompiled calls nest deeply: Windows links tms.exe with a
// 64 MiB stack. Linux gives the main thread 8 MiB, so the game runs on a thread of its own with 64 MiB.
int main(int argc, char** argv) {
    struct Args { int argc; char** argv; int ret; } a{argc, argv, 0};
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, [](void* p) -> void* { auto* a = (Args*)p; a->ret = tms_main(a->argc, a->argv); return nullptr; }, &a)) {
        fprintf(stderr, "TMSRecomp: cannot start the game thread\n");
        return 1;
    }
    pthread_join(th, nullptr);
    return a.ret;
}
static int tms_main(int argc, char** argv) {
    install_crash_handler();
    g_own_console = !isatty(2);  // started from a desktop entry / AppImage: errors also in a message box
    const std::filesystem::path exe_dir = plat::exe_path().parent_path(), ini = plat::settings_ini();
#endif
    for (int i = 1; i + 1 < argc; i++)  // --set key=value: a settings.ini line for this run only (not saved)
        if (!strcmp(argv[i], "--set")) {
            std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || !eq) continue;
            std::string k = kv.substr(0, eq);
            std::transform(k.begin(), k.end(), k.begin(), ::toupper);
            _putenv_s(("NX_" + k).c_str(), kv.substr(eq + 1).c_str());
        }
    load_settings(ini);
    if (const char* t = getenv("NX_PAD_ICONS_TEST")) return rt::pad_icons_selftest(t);
    const char* d = getenv("NX_DATA");
    g_data = d ? d : "private/tms";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) g_data = argv[++i];
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }
    if (!is_data(g_data)) {  // the default layout: <data>\build\tms.exe
        if (is_data(exe_dir.parent_path())) g_data = exe_dir.parent_path();
        else if (auto p = pick_data_folder(); is_data(p)) {
            g_data = p;
            std::ofstream(ini, std::ios::app) << "data = " << p.string() << "\n";
        } else {
#ifdef _WIN32
            MessageBoxA(nullptr, "Game data not found: the folder needs link\\image.bin and romfs (see README).",
                        "TMSRecomp", MB_ICONERROR);
#else
            log("Game data not found: give the folder with link/image.bin and romfs (--data <folder>, NX_DATA or data = in %s)",
                ini.string().c_str());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "TMSRecomp",
                                     "Game data not found: the folder needs link/image.bin and romfs (see README).", nullptr);
#endif
            return 1;
        }
    }
    for (int i = 1; i < argc; i++)  // --dump [--all]: the game's packs as loose files for modding (launcher Mods page)
        if (!strcmp(argv[i], "--dump")) {
#ifdef _WIN32
            if (!GetConsoleWindow() && AllocConsole()) freopen("CONOUT$", "w", stdout), freopen("CONIN$", "r", stdin);
            SetConsoleTitleW(L"TMSRecomp: dumping game files");
#endif
            bool all = false;
            std::string only;  // --pack pack_030_etc: just that pack
            for (int k = 1; k < argc; k++) {
                all |= !strcmp(argv[k], "--all");
                if (!strcmp(argv[k], "--pack") && k + 1 < argc) only = argv[k + 1];
            }
            printf("Dumping the game's files to %s (movies %s). This can take a while; it continues where it stopped.\n\n",
                   (g_data / "dump").string().c_str(), all ? "included" : "left out");
            const int r = rt::cpk_dump(g_data.string(), all, only);
            printf("\nPress Enter to close.\n");
            fflush(stdout);
            getchar();
            return r;
        }
#ifdef _WIN32
    // one game at a time (user 11-28: fast PLAY clicks started several; two games share the saves). The mutex lives as
    // long as this process. NX_MULTI_INSTANCE=1: allowed (debug).
    if (!getenv("NX_MULTI_INSTANCE") && CreateMutexW(nullptr, TRUE, L"Local\\TMSRecomp.game") && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
#endif
    if (getenv("NX_VERBOSE")) g_verbose = 1;
    g_data_dir = g_data.string();
    const char* lp = getenv("NX_LOG");
    std::filesystem::path log_path = lp ? std::filesystem::path(lp) : g_data / "nx.log";
    archive_log(log_path);
    archive_log(g_data / "issues.log");
    if (const char* r = getenv("NX_RECORD")) archive_log(r);  // keep every run's input recording too
    g_log = fopen(log_path.string().c_str(), "w");
    g_issues = fopen((g_data / "issues.log").string().c_str(), "w");
    if (g_issues) {
#ifdef _WIN32
        SYSTEMTIME st;
        GetLocalTime(&st);
#else
        struct { unsigned wYear, wMonth, wDay, wHour, wMinute, wSecond; } st;
        { time_t now = time(nullptr); tm lt{}; localtime_r(&now, &lt);
          st = {(unsigned)lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday, (unsigned)lt.tm_hour, (unsigned)lt.tm_min, (unsigned)lt.tm_sec}; }
#endif
        fprintf(g_issues, "TMSRecomp issues log, run started %04u-%02u-%02u %02u:%02u:%02u (build %s %s). Full log: %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, __DATE__, __TIME__,
                log_path.string().c_str());
        fflush(g_issues);
    }
    log("TMSRecomp: data %s", g_data.string().c_str());
    log("system: %s", sysinfo_line().c_str());  // bug reports: version, Windows, CPU, RAM (GPU: the "vk:" line)
    mem_init();
    game_load(exe_dir);  // after mem_init: the guest window is reserved before any DLL can land near it
    load_image();
    dispatch_init();
    kernel_init();
    uint32_t handle = create_main_thread(0x100000, 44);  // main.npdm: stack 1 MiB, priority 44
    uint32_t start = export_addr("_ZN2nn4init5StartEjjPFvvES2_");
    if (!start) fatal("nn::init::Start not found");
    // rtld callback 1: marks rtld as initialised, nothing for us. Callback 2: each module's DT_INIT (static
    // constructors, musl registration), dependencies first.
    uint32_t cb1 = host_callback([](Cpu*) {});
    uint32_t cb2 = host_callback([](Cpu* c) {
        for (int i = (int)g_module_count - 1; i >= 0; i--)
            if (g_modules[i].init) {
                log("DT_INIT %s", g_modules[i].name);
                call_guest(c, g_modules[i].init);
            }
    });
    uint32_t args[4] = {handle, 0, cb1, cb2};
    log("entering nn::init::Start");
    run_main_thread(start, args);
    return 0;
}
