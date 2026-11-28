// Guest address -> recompiled function, HLE hooks bound by symbol name, traps raised by generated code.
#include "plat.h"
#ifndef _WIN32
#include <dlfcn.h>
#include <shared_mutex>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <cxxabi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <thread>
#include <utility>
#include <cstdlib>
#include <map>
#include <set>

#include "rt.h"

extern "C" uint32_t* g_cover = nullptr;

namespace rt {
// the recompiled code and its tables live in game.dll (tools/setup/build_game.py, also run by the CMake build), found
// next to tms.exe: the program download carries no game code, setup builds game.dll from the player's own copy
static const GuestFunc* g_guest_funcs;
static uint32_t g_guest_func_count;
static const GuestHookName* g_hook_names;
static uint32_t g_hook_count;
static const GuestHookName* g_hook_aliases;
static uint32_t g_hook_alias_count;
static const GuestHookName* g_thunks;
static uint32_t g_thunk_count;
static GuestFn* g_hooks;
static GuestFn* g_probes;
const GuestModule* g_modules;
uint32_t g_module_count, g_image_size;

void game_load(const std::filesystem::path& dir) {
#ifdef _WIN32
    auto p = dir / "game.dll";
    HMODULE m = LoadLibraryW(p.c_str());
    if (!m) fatal("cannot load %s (error %lu): run Setup in tmslauncher.exe to build it from your game files",
                  p.string().c_str(), GetLastError());
    auto sym = [&](const char* n) {
        void* a = (void*)GetProcAddress(m, n);
        if (!a) fatal("game.dll has no %s: rebuild it (Setup in tmslauncher.exe)", n);
        return a;
    };
#else
    // game.so: next to tms, or NX_GAME_SO (a build kept outside a read-only install, e.g. an AppImage)
    std::filesystem::path p = getenv("NX_GAME_SO") ? std::filesystem::path(getenv("NX_GAME_SO")) : dir / "game.so";
    void* m = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) fatal("cannot load %s (%s): run the setup (tools/setup/setup.py) to build it from your game files",
                  p.string().c_str(), dlerror());
    auto sym = [&](const char* n) {
        void* a = dlsym(m, n);
        if (!a) fatal("game.so has no %s: rebuild it (tools/setup/setup.py)", n);
        return a;
    };
#endif
    g_guest_funcs = (const GuestFunc*)sym("g_guest_funcs");
    g_guest_func_count = *(const uint32_t*)sym("g_guest_func_count");
    g_hook_names = (const GuestHookName*)sym("g_hook_names");
    g_hook_count = *(const uint32_t*)sym("g_hook_count");
    g_hook_aliases = (const GuestHookName*)sym("g_hook_aliases");
    g_hook_alias_count = *(const uint32_t*)sym("g_hook_alias_count");
    g_thunks = (const GuestHookName*)sym("g_thunks");
    g_thunk_count = *(const uint32_t*)sym("g_thunk_count");
    g_hooks = (GuestFn*)sym("g_hooks");
    g_probes = (GuestFn*)sym("g_probes");
    g_modules = (const GuestModule*)sym("g_modules");
    g_module_count = *(const uint32_t*)sym("g_module_count");
    g_image_size = *(const uint32_t*)sym("g_image_size");
    log("game.dll: %s, %u functions", p.string().c_str(), g_guest_func_count);
#ifdef _WIN32
    // PGO training (dev, s117): an instrumented game.dll (build_game.py --pgo-gen) exports the profile runtime's writer;
    // its exit hook never runs in a /NOENTRY DLL, so the profile is written every NX_PGO_DUMP seconds (overwritten).
    using WriteFn = int (*)(); using NameFn = void (*)(const char*);
    if (auto w = (WriteFn)GetProcAddress(m, "__llvm_profile_write_file")) {
        static std::string out = getenv("NX_PGO_OUT") ? getenv("NX_PGO_OUT") : "game.profraw";
        if (auto n = (NameFn)GetProcAddress(m, "__llvm_profile_set_filename")) n(out.c_str());
        const int every = getenv("NX_PGO_DUMP") ? atoi(getenv("NX_PGO_DUMP")) : 30;
        log("pgo: instrumented game.dll, profile to %s every %d s", out.c_str(), every);
        std::thread([w, every] { for (;;) { Sleep(every * 1000); log("pgo: profile written (%d)", w()); } }).detach();
    }
#endif
}

// probed functions called since the last report, with counts (NX_COVER)
void cover_report() {
    static std::vector<uint32_t> last;
    if (!g_cover) return;
    last.resize(g_hook_count);
    std::string s;
    char b[64];
    for (uint32_t k = 0; k < g_hook_count; k++) {
        uint32_t n = g_cover[k] - last[k];
        last[k] = g_cover[k];
        if (n) { snprintf(b, sizeof b, " %s:%u", g_hook_names[k].name, n); s += b; }
        if (s.size() > 3000) { log("cover:%s", s.c_str()); s.clear(); }  // log lines are cut at 4 KB
    }
    log("cover-end:%s", s.c_str());
}

struct Reg { const char* name; HleFn fn; bool probe; };
static std::vector<Reg>& registry() {
    static std::vector<Reg> r;
    return r;
}
HleReg::HleReg(const char* mangled, HleFn fn, bool probe) { registry().push_back({mangled, fn, probe}); }

// The game's C library string / memory functions as host code (after Astral Chain Recomp's host_libc, s117): one
// host call instead of the lifted byte / NEON loops. NX_HOST_LIBC=0 (settings.ini host_libc = 0) keeps the game's own.
static const char* const k_libc[] = {"memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
                                     "__aeabi_memcpy", "__aeabi_memmove", "__aeabi_memset", "__aeabi_memclr"};
static bool libc_name(const char* n) {
    for (const char* k : k_libc) if (!strcmp(k, n)) return true;
    return false;
}
// memcpy as memmove: a caller that overlaps gets the bytes it expected rather than undefined order; r0 (dest) is the
// return value and stays as it came
HLE_FN(hle_memcpy, "memcpy") { memmove(GPTR(c->r[0]), GPTR(c->r[1]), c->r[2]); }
HLE_FN(hle_memmove, "memmove") { memmove(GPTR(c->r[0]), GPTR(c->r[1]), c->r[2]); }
HLE_FN(hle_memset, "memset") { memset(GPTR(c->r[0]), (int)(c->r[1] & 0xff), c->r[2]); }
HLE_FN(hle_aeabi_memcpy, "__aeabi_memcpy") { memmove(GPTR(c->r[0]), GPTR(c->r[1]), c->r[2]); }
HLE_FN(hle_aeabi_memmove, "__aeabi_memmove") { memmove(GPTR(c->r[0]), GPTR(c->r[1]), c->r[2]); }
HLE_FN(hle_aeabi_memset, "__aeabi_memset") { memset(GPTR(c->r[0]), (int)(c->r[2] & 0xff), c->r[1]); }  // (dest, n, c)
HLE_FN(hle_aeabi_memclr, "__aeabi_memclr") { memset(GPTR(c->r[0]), 0, c->r[1]); }
HLE_FN(hle_memcmp, "memcmp") {
    const uint8_t *a = GPTR(c->r[0]), *b = GPTR(c->r[1]);
    uint32_t n = c->r[2], i = 0;
    if (!memcmp(a, b, n)) { c->r[0] = 0; return; }
    while (a[i] == b[i]) i++;  // the first differing byte's difference, as the game's own memcmp returns
    c->r[0] = (uint32_t)((int)a[i] - (int)b[i]);
}
HLE_FN(hle_strlen, "strlen") { c->r[0] = (uint32_t)strlen((const char*)GPTR(c->r[0])); }
HLE_FN(hle_strcmp, "strcmp") {
    const uint8_t *a = GPTR(c->r[0]), *b = GPTR(c->r[1]);
    while (*a && *a == *b) a++, b++;
    c->r[0] = (uint32_t)((int)*a - (int)*b);
}
HLE_FN(hle_strncmp, "strncmp") {
    const uint8_t *a = GPTR(c->r[0]), *b = GPTR(c->r[1]);
    uint32_t n = c->r[2];
    while (n && *a && *a == *b) a++, b++, n--;
    c->r[0] = n ? (uint32_t)((int)*a - (int)*b) : 0;
}

// NX_PROBE=name,name,...: log every call of those names-file functions (arguments and caller), up to 64 names
static const char* g_probe_names[64];
template <int K> static void probe_log(Cpu* c) {
    log("probe %s(%08x %08x %08x %08x) lr %s", g_probe_names[K], c->r[0], c->r[1], c->r[2], c->r[3], symbol_at(c->r[14]));
}
template <int... K> static constexpr std::array<HleFn, sizeof...(K)> probe_table(std::integer_sequence<int, K...>) {
    return {probe_log<K>...};
}
static constexpr auto g_probe_loggers = probe_table(std::make_integer_sequence<int, 64>{});

struct TextRange { uint32_t lo, hi; std::vector<GuestFn> fns; };
static std::vector<TextRange> g_text;
static uint32_t g_thunk_lo = 0xFFFFFFFF, g_thunk_hi = 0;
static std::vector<HleFn> g_thunk_fn;
static std::vector<const char*> g_thunk_name;
static std::map<uint32_t, const char*> g_syms;
static std::unordered_map<std::string, uint32_t> g_sym_addr;
constexpr uint32_t THUNK_SIZE = 16;
static std::unordered_map<std::string, uint32_t> g_hook_index;

void dispatch_init() {
    // contiguous runs of functions -> dense tables, split where the gap is large (between modules)
    uint32_t i = 0;
    while (i < g_guest_func_count) {
        uint32_t lo = g_guest_funcs[i].addr, j = i;
        while (j + 1 < g_guest_func_count && g_guest_funcs[j + 1].addr - g_guest_funcs[j].addr < 0x100000) j++;
        TextRange t{lo, g_guest_funcs[j].addr + 4, {}};
        t.fns.assign((t.hi - lo) / 4, nullptr);
        for (uint32_t k = i; k <= j; k++) t.fns[(g_guest_funcs[k].addr - lo) / 4] = g_guest_funcs[k].fn;
        g_text.push_back(std::move(t));
        i = j + 1;
    }
    for (uint32_t k = 0; k < g_hook_count; k++) {
        g_syms[g_hook_names[k].addr] = g_hook_names[k].name;
        g_sym_addr[g_hook_names[k].name] = g_hook_names[k].addr;
    }
    for (uint32_t k = 0; k < g_thunk_count; k++) {
        g_thunk_lo = std::min(g_thunk_lo, g_thunks[k].addr);
        g_thunk_hi = std::max(g_thunk_hi, g_thunks[k].addr + THUNK_SIZE);
    }
    if (g_thunk_count) {
        g_thunk_fn.assign((g_thunk_hi - g_thunk_lo) / THUNK_SIZE, nullptr);
        g_thunk_name.assign(g_thunk_fn.size(), nullptr);
        for (uint32_t k = 0; k < g_thunk_count; k++) g_thunk_name[(g_thunks[k].addr - g_thunk_lo) / THUNK_SIZE] = g_thunks[k].name;
    }
    // bind HLE implementations: exported functions get their hook slot, unresolved imports their thunk
    std::unordered_map<uint32_t, uint32_t> idx_of_addr;
    for (uint32_t k = 0; k < g_hook_count; k++) idx_of_addr.emplace(g_hook_names[k].addr, k);
    std::unordered_map<std::string, uint32_t> hook_idx;  // every alias -> hook slot of its address
    for (uint32_t k = 0; k < g_hook_alias_count; k++) {
        hook_idx.emplace(g_hook_aliases[k].name, idx_of_addr[g_hook_aliases[k].addr]);
        g_sym_addr.emplace(g_hook_aliases[k].name, g_hook_aliases[k].addr);
    }
    g_hook_index = hook_idx;
    int bound = 0, unused = 0;
    static const bool host_libc = !getenv("NX_HOST_LIBC") || atoi(getenv("NX_HOST_LIBC")) != 0;
    for (auto& [name, fn, probe] : registry()) {
        if (!host_libc && libc_name(name)) continue;
        bool ok = false;
        auto h = hook_idx.find(name);
        if (h != hook_idx.end()) { (probe ? g_probes : g_hooks)[h->second] = fn; ok = true; }
        for (size_t k = 0; k < g_thunk_name.size() && !probe; k++)
            if (g_thunk_name[k] && !strcmp(g_thunk_name[k], name)) { g_thunk_fn[k] = fn; ok = true; }
        if (ok) bound++;
        else { unused++; LOGV("HLE %s: no such symbol", name); }
    }
    if (const char* cov = getenv("NX_COVER")) {  // every 10 s (NX_COVER=manual: only on the control line "cover")
        static std::vector<uint32_t> counts(g_hook_count);
        g_cover = counts.data();
        if (strcmp(cov, "manual"))
            std::thread([] { for (;;) { Sleep(10000); cover_report(); } }).detach();
    }
    if (const char* env = getenv("NX_PROBE")) {
        static std::vector<std::string> keep;
        keep.reserve(128);  // g_probe_names points into it
        std::string s = env;
        for (size_t p = 0, n = 0; p <= s.size() && n < 64;) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            keep.push_back(s.substr(p, q - p));
            p = q + 1;
            auto h = hook_idx.find(keep.back());
            if (h == hook_idx.end()) { log("NX_PROBE: no hookable function %s", keep.back().c_str()); continue; }
            g_probe_names[n] = keep.back().c_str();
            g_probes[h->second] = g_probe_loggers[n++];
        }
    }
    log("dispatch: %u functions in %zu ranges, %d HLE bound, %d unmatched", g_guest_func_count, g_text.size(), bound, unused);
}

// call an exported guest function with its HLE hook disabled (to reach the real SDK implementation)
uint32_t call_unhooked(Cpu* c, const char* name, std::initializer_list<uint32_t> args) {
    // s116: the lookup by name (a std::string key: heap allocation + hash) was 6% of the game thread in Shibuya (the UI
    // draw hooks call this many times per frame); names are string literals, so cache by pointer per thread
    struct Entry { const char* name; uint32_t idx, addr; };
    thread_local std::vector<Entry> cache;
    const Entry* e = nullptr;
    for (auto& x : cache) if (x.name == name) { e = &x; break; }
    if (!e) {
        auto it = g_hook_index.find(name);
        if (it == g_hook_index.end()) fatal("call_unhooked: %s is not hookable", name);
        const uint32_t a = export_addr(name);
        if (!a) fatal("gcall: no symbol %s", name);
        cache.push_back({name, it->second, a});
        e = &cache.back();
    }
    c->hook_bypass = e->idx + 1;  // per thread: HOOK at the function's entry clears it and runs the guest code
    return gcall(c, e->addr, args);
}

GuestFn lookup(uint32_t a) {
    for (auto& t : g_text)
        if (a >= t.lo && a < t.hi) return (a & 3) ? nullptr : t.fns[(a - t.lo) / 4];
    return nullptr;
}

const char* symbol_at(uint32_t a) {
    static thread_local char buf[256];
    auto it = g_syms.upper_bound(a);
    if (it == g_syms.begin()) { snprintf(buf, sizeof buf, "%08x", a); return buf; }
    --it;
    snprintf(buf, sizeof buf, "%08x %s+%#x", a, it->second, a - it->first);
    return buf;
}

uint32_t export_addr(const char* name) {
    auto it = g_sym_addr.find(name);
    return it == g_sym_addr.end() ? 0 : it->second;
}

}  // namespace rt

using namespace rt;

extern "C" void dispatch(Cpu* c, uint32_t t) {
    if (GuestFn f = lookup(t)) { f(c); return; }
    if (t >= g_thunk_lo && t < g_thunk_hi) {
        size_t k = (t - g_thunk_lo) / THUNK_SIZE;
        if (g_thunk_fn[k]) { g_thunk_fn[k](c); return; }
        static std::mutex m;
        static std::set<size_t> seen;
        {
            std::lock_guard<std::mutex> l(m);
            if (seen.insert(k).second) log("unimplemented import %s (lr %s)", g_thunk_name[k], symbol_at(c->r[14]));
        }
        c->r[0] = 0;
        return;
    }
    if (HleFn f = host_callback_at(t)) { f(c); return; }
    guest_bad_branch(c, c->r[14], t);
}

extern "C" void guest_trap(Cpu* c, uint32_t a, const char* why) {
    fatal("guest trap at %s: %s (lr %s)", symbol_at(a), why, symbol_at(c->r[14]));
}

extern "C" void guest_unimpl(Cpu* c, uint32_t a, uint32_t w) {
    fatal("unimplemented instruction %08x at %s (lr %s)", w, symbol_at(a), symbol_at(c->r[14]));
}

extern "C" void guest_bad_branch(Cpu* c, uint32_t from, uint32_t to) {
    fatal("branch to unknown code %08x from %s (sp %08x)", to, symbol_at(from), c->r[13]);
}

extern "C" void guest_noreturn(Cpu* c, uint32_t a) {
    fatal("noreturn call returned at %s", symbol_at(a));
}

extern "C" void guest_svc(Cpu* c, uint32_t imm, uint32_t a) {
    (void)a;
    rt::svc(c, imm);
}

extern "C" void guest_yield(Cpu* c) {
    (void)c;
    SwitchToThread();
}

extern "C" uint32_t cp15_read(Cpu* c, int opc1, int crn, int crm, int opc2) {
    log("cp15 read %d c%d c%d %d at lr %s", opc1, crn, crm, opc2, symbol_at(c->r[14]));
    return 0;
}

extern "C" void cp15_write(Cpu* c, int opc1, int crn, int crm, int opc2, uint32_t v) {
    log("cp15 write %d c%d c%d %d = %08x at lr %s", opc1, crn, crm, opc2, v, symbol_at(c->r[14]));
}

static uint64_t real_counter() {
#ifdef _WIN32
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t q = (uint64_t)now.QuadPart, f = (uint64_t)freq.QuadPart;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t q = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, f = 1000000000ull;
#endif
    return q / f * 19200000 + q % f * 19200000 / f;
}

// guest time = base_guest + (real - base_real) * speed; rebased on every speed change so it never jumps back
#ifdef _WIN32
static SRWLOCK g_speed_lock = SRWLOCK_INIT;
#else
static std::shared_mutex g_speed_lock;
static void AcquireSRWLockExclusive(std::shared_mutex* m) { m->lock(); }
static void ReleaseSRWLockExclusive(std::shared_mutex* m) { m->unlock(); }
static void AcquireSRWLockShared(std::shared_mutex* m) { m->lock_shared(); }
static void ReleaseSRWLockShared(std::shared_mutex* m) { m->unlock_shared(); }
#endif
static uint64_t g_base_real = 0, g_base_guest = 0;
std::atomic<int> rt::g_speed{[] { const char* s = getenv("NX_SPEED"); return s && atoi(s) > 1 ? atoi(s) : 1; }()};

void rt::set_speed(int n) {
    if (n < 1) n = 1;
    AcquireSRWLockExclusive(&g_speed_lock);
    uint64_t real = real_counter();
    g_base_guest += (real - g_base_real) * (uint64_t)g_speed.load();
    g_base_real = real;
    g_speed = n;
    ReleaseSRWLockExclusive(&g_speed_lock);
    log("speed: x%d", n);
}

extern "C" uint64_t guest_counter(void) {
    AcquireSRWLockShared(&g_speed_lock);
    uint64_t t = g_base_guest + (real_counter() - g_base_real) * (uint64_t)g_speed.load();
    ReleaseSRWLockShared(&g_speed_lock);
    return t;
}

// setjmp/longjmp: see lift.py (setjmp is lifted inline at its call sites)
extern "C" GuestJmp* guest_setjmp_slot(Cpu* c, uint32_t buf) {
    (void)c;
    return &cur()->jmps[buf];
}
extern "C" void guest_setjmp_save(Cpu* c, GuestJmp* j) {
    memcpy(j->r, c->r, sizeof j->r);
    memcpy(j->d, &c->vr.x[8], sizeof j->d);
    c->r[0] = 0;
}
extern "C" void guest_setjmp_resume(Cpu* c, GuestJmp* j) {
    memcpy(&c->r[4], &j->r[4], 11 * 4);  // r4-r14
    memcpy(&c->vr.x[8], j->d, sizeof j->d);
    c->r[0] = j->val;
}
HLE_FN(hle_longjmp, "longjmp") {
    auto& m = cur()->jmps;
    auto it = m.find(c->r[0]);
    if (it == m.end()) fatal("longjmp to unknown jmp_buf %08x", c->r[0]);
    it->second.val = c->r[1] ? c->r[1] : 1;
    longjmp(it->second.host, 1);
}

#ifndef _WIN32
// NX_SAMPLE=1 (ini sample = 1; debug, Linux): CPU sampling profiler. SIGPROF every 1 ms of process CPU time records the
// interrupted thread and its program counter; every 30 s the log gets each thread's share and the top 30 places
// (module!symbol; recompiled game code by its guest function, time inside the kernel shows at the libc call that
// entered it). Finds where a slow PC (ROG Ally) spends its frame.
namespace {
struct Samp { uint64_t pc; int32_t tid; };
constexpr size_t kSampN = 1u << 17;
Samp g_samp[kSampN];
std::atomic<uint64_t> g_samp_w{0};
void on_prof(int, siginfo_t*, void* uc) {
    const uint64_t i = g_samp_w.fetch_add(1, std::memory_order_relaxed);
    Samp& s = g_samp[i & (kSampN - 1)];
    s.pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext.gregs[REG_RIP];
    s.tid = (int32_t)syscall(SYS_gettid);
}
std::string thread_name(int tid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    FILE* f = fopen(p, "r");
    char b[32] = "?";
    if (f) { if (fgets(b, sizeof b, f)) b[strcspn(b, "\n")] = 0; fclose(f); }
    return b;
}
}  // namespace
void rt::sampler_start() {
    if (!getenv("NX_SAMPLE") || !atoi(getenv("NX_SAMPLE"))) return;
    struct sigaction sa{};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    std::thread([] {
        // host function start -> guest address (recompiled code has hidden symbols)
        std::vector<std::pair<uint64_t, uint32_t>> fns;
        // tms.syms next to tms (nm -C -n --defined-only tms): names of the runtime's own (static) functions
        std::vector<std::pair<uint64_t, std::string>> own;
        {
            char exe[4096] = {};
            if (readlink("/proc/self/exe", exe, sizeof exe - 1) > 0) {
                std::string f = exe;
                f = f.substr(0, f.rfind('/') + 1) + "tms.syms";
                if (FILE* sf = fopen(f.c_str(), "r")) {
                    char line[1024];
                    while (fgets(line, sizeof line, sf)) {
                        char* e = nullptr;
                        const uint64_t a = strtoull(line, &e, 16);
                        if (!e || e == line || (e[1] != 't' && e[1] != 'T')) continue;
                        std::string n = e + 3;
                        n.erase(n.find_last_not_of("\r\n") + 1);
                        if (n.size() > 90) n.resize(90);
                        own.push_back({a, n});
                    }
                    fclose(sf);
                    std::sort(own.begin(), own.end());
                    log("prof: %zu names from %s", own.size(), f.c_str());
                }
            }
        }
        uint64_t r = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (fns.empty() && g_guest_funcs) {
                for (uint32_t i = 0; i < g_guest_func_count; i++) fns.push_back({(uint64_t)(uintptr_t)g_guest_funcs[i].fn, g_guest_funcs[i].addr});
                std::sort(fns.begin(), fns.end());
            }
            const uint64_t w = g_samp_w.load();
            if (w - r > kSampN) r = w - kSampN;
            std::map<int, uint64_t> per_tid;
            std::map<std::pair<int, std::string>, uint64_t> where;
            for (; r < w; r++) {
                const Samp s = g_samp[r & (kSampN - 1)];
                per_tid[s.tid]++;
                std::string k;
                Dl_info di{};
                if (dladdr((void*)s.pc, &di) && di.dli_fname) {
                    const char* mod = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
                    if (!strcmp(mod, "game.so") && !fns.empty()) {
                        auto it = std::upper_bound(fns.begin(), fns.end(), std::make_pair(s.pc, UINT32_MAX));
                        if (it != fns.begin()) { --it; const char* g = symbol_at(it->second); k = std::string("game!") + (strchr(g, ' ') ? strchr(g, ' ') + 1 : g); }
                    }
                    if (k.empty() && !own.empty() && !strcmp(mod, "tms")) {
                        auto it = std::upper_bound(own.begin(), own.end(), std::make_pair(s.pc - (uint64_t)di.dli_fbase, std::string("\xff")));
                        if (it != own.begin()) k = "tms!" + std::prev(it)->second;
                    }
                    if (k.empty() && di.dli_sname) {
                        int st = 0;
                        char* dm = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
                        std::string n = st == 0 && dm ? dm : di.dli_sname;
                        free(dm);
                        if (n.size() > 90) n.resize(90);
                        k = std::string(mod) + "!" + n;
                    }
                    if (k.empty()) { char b[64]; snprintf(b, sizeof b, "!+%#llx", (unsigned long long)((s.pc - (uint64_t)di.dli_fbase) & ~0xFFFull)); k = mod + std::string(b); }
                } else k = "?";
                where[{s.tid, k}]++;
            }
            uint64_t total = 0;
            for (auto& [t, n] : per_tid) total += n;
            if (!total) continue;
            std::string th;
            for (auto& [t, n] : per_tid)
                if (n * 100 >= total) { char b[96]; snprintf(b, sizeof b, " %d %s %.0f%%;", t, thread_name(t).c_str(), 100.0 * n / total); th += b; }
            log("prof: %llu samples (1 ms of CPU each) in 30 s, threads:%s", (unsigned long long)total, th.c_str());
            std::vector<std::pair<uint64_t, std::pair<int, std::string>>> top;
            for (auto& [k, n] : where) top.push_back({n, k});
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 30; i++)
                log("prof %5.1f%%  %d  %s", 100.0 * top[i].first / total, top[i].second.first, top[i].second.second.c_str());
        }
    }).detach();
    itimerval tv{{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &tv, nullptr);
    log("prof: CPU sampling on (NX_SAMPLE), a summary every 30 s");
}
#endif
