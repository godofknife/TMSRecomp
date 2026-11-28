#include <unordered_map>
// Guest memory: one 4 GiB reservation at GUEST_BASE, committed on demand, plus a region map for QueryMemory.
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <malloc.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#endif

#include <map>
#include <unordered_map>
#include <string>

#include "rt.h"

namespace rt {

static std::mutex g_mem_lock;
static std::map<uint32_t, Region> g_regions;  // by start address, non-overlapping
static uint32_t g_kernel_next = KERNEL_BASE;
static uint32_t g_hle_next = HLE_BASE;
static uint32_t g_heap_size = 0;

constexpr uint64_t k_window = 0x100010000ull;  // 4 GiB + 64 KiB: a 32-bit address plus a small displacement stays inside

#ifndef _WIN32
// Linux has no write watch (Windows MEM_WRITE_WATCH / GetWriteWatch), so it is built from page protection: each page of
// the window is free (PROT_NONE), dirty (read-write, written since the last check) or watched (read-only: the first
// write faults, mem_fault marks the page dirty and opens it again). mem_written re-arms the dirty pages it reports.
// Pages of the window are only ever watched after mem_written was called on them (texture pools), so the rest of the
// game's memory never faults.
enum : uint8_t { PG_FREE = 0, PG_WATCHED = 1, PG_DIRTY = 2 };
static std::atomic<uint8_t>* g_pages;  // k_window >> 12 entries
// Pages imported into Vulkan (mem_make_writable): never made read-only again. AMD's kernel driver (amdgpu userptr)
// checks imported pages at every submit and rejects the whole submit when one is read-only (device lost on RADV).
// A range with such pages is checked by content instead: a hash of the range, kept per (addr, size).
static std::atomic<uint8_t>* g_pinned;
static void protect(uint64_t lo, uint64_t len, int prot) {
    if (mprotect((void*)(GUEST_BASE + lo), len, prot)) fatal("mprotect %llx+%llx failed (%s)", (unsigned long long)lo, (unsigned long long)len, strerror(errno));
}
bool mem_fault(void* host_addr) {  // SIGSEGV handler (main.cpp): true = a watched page, the access can run again
    const uint64_t a = (uint64_t)host_addr;
    if (!g_pages || a < GUEST_BASE || a >= GUEST_BASE + k_window) return false;
    const uint64_t pg = (a - GUEST_BASE) >> 12;
    if (g_pages[pg].load() == PG_FREE) return false;  // not committed: a real crash
    // dirty first, then writable: a concurrent mem_written either sees the dirty mark or re-arms after us (the next
    // write then faults again and lands here). mprotect is async-signal-safe.
    g_pages[pg].store(PG_DIRTY);
    mprotect((void*)(GUEST_BASE + (pg << 12)), 0x1000, PROT_READ | PROT_WRITE);
    return true;
}
#endif

void mem_init() {
#ifdef _WIN32
    // 4 GiB + 64 KiB so that a 32-bit address plus a small displacement never leaves the reservation. Write-watched:
    // the NVN backend asks which pages the CPU wrote (pool textures changed after upload).
    void* p = VirtualAlloc((void*)GUEST_BASE, k_window, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (p != (void*)GUEST_BASE) fatal("cannot reserve guest memory at %llx (error %lu)", GUEST_BASE, GetLastError());
#else
    void* p = mmap((void*)GUEST_BASE, k_window, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void*)GUEST_BASE) fatal("cannot reserve guest memory at %llx (%s)", GUEST_BASE, strerror(errno));
    madvise(p, k_window, MADV_DONTFORK);  // helper processes (shader translator) never copy it
    g_pages = new std::atomic<uint8_t>[k_window >> 12]();
    g_pinned = new std::atomic<uint8_t>[k_window >> 12]();
#endif
}

std::atomic<uint64_t> g_commit_pages{0};  // NX_STALL_STATS
static void commit(uint32_t addr, uint32_t size) {
    if (!size) return;
    g_commit_pages += size >> 12;
#ifdef _WIN32
    if (!VirtualAlloc(GPTR(addr), size, MEM_COMMIT, PAGE_READWRITE))
        fatal("commit %08x+%x failed (error %lu)", addr, size, GetLastError());
#else
    // committed pages keep their contents (as VirtualAlloc MEM_COMMIT); new ones read as zero
    const uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    for (uint64_t pg = lo >> 12; pg < hi >> 12; pg++) g_pages[pg].store(PG_DIRTY);
    protect(lo, hi - lo, PROT_READ | PROT_WRITE);
#endif
}

#ifdef _WIN32
void mem_make_writable(uint32_t, uint32_t) {}
void mem_next_frame() {}
#else
void mem_make_writable(uint32_t addr, uint32_t size) {  // before the kernel or a driver writes there (pinning for Vulkan)
    const uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    bool any = false;
    for (uint64_t pg = lo >> 12; pg < hi >> 12; pg++) {
        g_pinned[pg].store(1);
        uint8_t w = PG_WATCHED;
        if (g_pages[pg].compare_exchange_strong(w, PG_DIRTY)) any = true;
    }
    if (any) {
        for (uint64_t pg = lo >> 12; pg < hi >> 12;) {  // open the committed runs (free pages stay PROT_NONE)
            if (g_pages[pg].load() == PG_FREE) { pg++; continue; }
            uint64_t e = pg;
            while (e < hi >> 12 && g_pages[e].load() != PG_FREE) e++;
            protect(pg << 12, (e - pg) << 12, PROT_READ | PROT_WRITE);
            pg = e;
        }
    }
}
#endif

static void carve(uint32_t addr, uint32_t size) {  // remove [addr, addr+size) from the map, splitting neighbours
    uint64_t end = (uint64_t)addr + size;
    auto it = g_regions.upper_bound(addr);
    if (it != g_regions.begin()) --it;
    while (it != g_regions.end() && it->first < end) {
        Region r = it->second;
        uint64_t rend = (uint64_t)r.addr + r.size;
        if (rend <= addr) { ++it; continue; }
        it = g_regions.erase(it);
        if (r.addr < addr) g_regions[r.addr] = Region{r.addr, addr - r.addr, r.state, r.perm, r.attr};
        if (rend > end) g_regions[(uint32_t)end] = Region{(uint32_t)end, (uint32_t)(rend - end), r.state, r.perm, r.attr};
    }
}

void mem_map(uint32_t addr, uint32_t size, uint32_t state, uint32_t perm) {
    std::lock_guard<std::mutex> l(g_mem_lock);
    commit(addr, size);
    carve(addr, size);
    g_regions[addr] = Region{addr, size, state, perm, 0};
}

void mem_unmap(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> l(g_mem_lock);
    carve(addr, size);
#ifdef _WIN32
    VirtualFree(GPTR(addr), size, MEM_DECOMMIT);
#else
    const uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    for (uint64_t pg = lo >> 12; pg < hi >> 12; pg++) g_pages[pg].store(PG_FREE);
    madvise((void*)(GUEST_BASE + lo), hi - lo, MADV_DONTNEED);  // contents gone, as MEM_DECOMMIT
    protect(lo, hi - lo, PROT_NONE);
#endif
}

#ifndef _WIN32
double process_private_mb() {  // resident anonymous memory of the process (leak checks in the logs)
    std::ifstream f("/proc/self/status");
    for (std::string l; std::getline(f, l);)
        if (l.rfind("RssAnon:", 0) == 0) return atof(l.c_str() + 8) / 1024.0;
    return 0;
}
void mem_regions_report() {}  // NX_MEM_REGIONS: Windows only (VirtualQuery / heap walk)
double guest_committed_mb() {  // the game's own memory: committed pages of the window
    uint64_t n = 0;
    for (uint64_t pg = 0; pg < k_window >> 12; pg++) n += g_pages[pg].load(std::memory_order_relaxed) != PG_FREE;
    return n / 256.0;
}
std::atomic<uint64_t> g_ww_reset_pages{0};
static uint64_t range_hash(const uint8_t* p, size_t n) {  // fast 64-bit content hash (not cryptographic)
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
        for (int k = 0; k < 4; k++) {
            uint64_t v;
            memcpy(&v, p + i + 8 * k, 8);
            h[k] = (h[k] ^ v) * 0x100000001B3ull;  // 4 independent lanes: memory speed
        }
    uint64_t r = h[0] ^ (h[1] << 1) ^ (h[2] << 2) ^ (h[3] << 3) ^ n;
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 31);
}
static std::mutex g_pin_lock;
struct PinCheck { uint64_t sample, full, frame; };
// Every frame: 32 bytes of each 2 KiB (a CPU rewrite of a texture shows there); every 16th frame per range, staggered:
// the whole range (a change between the samples is caught within 16 frames).
static uint64_t sample_hash(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    for (size_t i = 0; i < n; i += 2048) h = (h ^ range_hash(p + i, std::min<size_t>(32, n - i))) * 0x100000001B3ull;
    return h;
}
static std::unordered_map<uint64_t, PinCheck> g_pin_hash;  // (addr << 32 | size) -> content hash at the last check
static std::atomic<uint64_t> g_mem_frame{1};
void mem_next_frame() { g_mem_frame++; }  // vk present: imported ranges are hashed at most once per frame
bool mem_written(uint32_t addr, uint32_t size) {  // CPU wrote [addr, addr+size) since the last call for it
    if (!size) return false;
    const uint64_t lo = addr & ~0xFFFull, hi = ((uint64_t)addr + size + 0xFFF) & ~0xFFFull;
    bool any = false, pinned = false;
    for (uint64_t pg = lo >> 12; pg < hi >> 12;) {  // re-arm each run of dirty pages (pinned ones: never, always written)
        if (g_pinned[pg].load(std::memory_order_relaxed)) { pinned = true; pg++; continue; }
        uint8_t d = PG_DIRTY;
        if (!g_pages[pg].compare_exchange_strong(d, PG_WATCHED)) { pg++; continue; }
        uint64_t e = pg + 1;
        for (d = PG_DIRTY; e < hi >> 12 && !g_pinned[e].load(std::memory_order_relaxed) && g_pages[e].compare_exchange_strong(d, PG_WATCHED); d = PG_DIRTY) e++;
        protect(pg << 12, (e - pg) << 12, PROT_READ);
        g_ww_reset_pages += e - pg;
        any = true;
        pg = e;
    }
    if (pinned) {  // imported pages: compare the contents with the last check (the first call counts as written)
        const uint64_t key = (uint64_t)addr << 32 | size, frame = g_mem_frame.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(g_pin_lock);
            auto it = g_pin_hash.find(key);
            if (it != g_pin_hash.end() && it->second.frame == frame) return any;  // checked this frame already
        }
        const uint8_t* p = (const uint8_t*)GPTR(addr);
        const uint64_t sh = sample_hash(p, size);
        uint64_t mix = key * 0x9E3779B97F4A7C15ull;
        bool full_turn = ((frame + (mix >> 60)) & 15) == 0;
        std::unique_lock<std::mutex> lk(g_pin_lock);
        auto it = g_pin_hash.find(key);
        const bool fresh = it == g_pin_hash.end();
        if (fresh || full_turn) {
            lk.unlock();
            const uint64_t fh = range_hash(p, size);
            lk.lock();
            auto [jt, added] = g_pin_hash.try_emplace(key, PinCheck{sh, fh, frame});
            if (added || jt->second.sample != sh || jt->second.full != fh) any = true;
            jt->second = {sh, fh, frame};
        } else {
            if (it->second.sample != sh) any = true, it->second.full = range_hash(p, size);
            it->second.sample = sh, it->second.frame = frame;
        }
    }
    return any;
}
bool mem_written_span(uint32_t addr, uint32_t size, uint32_t, uint32_t, uint64_t) { return mem_written(addr, size); }
// PORTABILITY: no write ticks here (the dirty flags are taken by the texture checks): vertex data stays in system RAM
bool mem_write_ticks_ok() { return false; }
uint32_t mem_write_tick(uint32_t, uint32_t, uint64_t) { return 0; }
uint32_t mem_written_tick(uint32_t, uint32_t) { return UINT32_MAX; }
#else
double process_private_mb() {  // committed private memory of the process (leak checks in the logs)
    PROCESS_MEMORY_COUNTERS_EX pm{};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    return pm.PrivateUsage / 1048576.0;
}
// NX_MEM_REGIONS=1 (diagnostic, s85 battle RAM growth): every call logs how committed private memory outside the guest
// reservation changed since the last call, grouped by allocation base (one VirtualAlloc / heap segment / driver block):
// new bases by size class and the bases that grew most.
void mem_regions_report() {
    static const bool on = getenv("NX_MEM_REGIONS") != nullptr;
    if (!on) return;
    static std::map<uintptr_t, uint64_t> prev;
    std::map<uintptr_t, uint64_t> cur;
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = nullptr; VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize) {
        if ((uint8_t*)mi.BaseAddress >= (uint8_t*)GUEST_BASE && (uint8_t*)mi.BaseAddress < (uint8_t*)GUEST_BASE + 0x100010000ull) continue;
        if (mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE) cur[(uintptr_t)mi.AllocationBase] += mi.RegionSize;
        if ((uint8_t*)mi.BaseAddress + mi.RegionSize < (uint8_t*)mi.BaseAddress) break;
    }
    if (!prev.empty()) {
        uint64_t total = 0, newb = 0, n_new = 0, small = 0, mid = 0, big = 0;
        std::vector<std::pair<int64_t, uintptr_t>> grow;
        for (auto& [b, n] : cur) {
            auto it = prev.find(b);
            int64_t d = (int64_t)n - (it == prev.end() ? 0 : (int64_t)it->second);
            total += n;
            if (it == prev.end()) { newb += n, n_new++; (n < (1 << 20) ? small : n < (16 << 20) ? mid : big) += n; }
            else if (d) grow.push_back({d, b});
        }
        int64_t gone = 0;
        for (auto& [b, n] : prev) if (!cur.count(b)) gone += n;
        std::sort(grow.rbegin(), grow.rend());
        char buf[512];
        int k = snprintf(buf, sizeof buf, "mem: host private %.0f MB in %zu bases; new %llu bases %.0f MB (<1M %.0f, <16M %.0f, big %.0f), gone %.0f MB; grew:",
                         total / 1048576.0, cur.size(), (unsigned long long)n_new, newb / 1048576.0, small / 1048576.0, mid / 1048576.0, big / 1048576.0, gone / 1048576.0);
        // owning heap of the bases that grew (index in GetProcessHeaps; P = process heap, C = CRT heap)
        HANDLE heaps[64];
        DWORD nh = std::min<DWORD>(GetProcessHeaps(64, heaps), 64);
        auto owner = [&](uintptr_t base) -> std::string {
            for (DWORD i = 0; i < nh; i++) {
                PROCESS_HEAP_ENTRY e{};
                if (!HeapLock(heaps[i])) continue;
                bool hit = false;
                while (!hit && HeapWalk(heaps[i], &e))
                    if ((e.wFlags & PROCESS_HEAP_REGION) && (uintptr_t)e.lpData <= base && base < (uintptr_t)e.Region.lpLastBlock) hit = true;
                    else if ((e.wFlags & PROCESS_HEAP_REGION) == 0 && (uintptr_t)e.lpData - base < (1u << 16) && e.cbData >= (512u << 10)) hit = true;  // big block
                HeapUnlock(heaps[i]);
                if (hit) return std::string("heap") + std::to_string(i) + (heaps[i] == GetProcessHeap() ? "P" : "") + (heaps[i] == (HANDLE)_get_heap_handle() ? "C" : "");
            }
            return "?";
        };
        for (size_t i = 0; i < grow.size() && i < 4 && k < 480; i++) {
            std::string o = grow[i].first > (8 << 20) ? owner(grow[i].second) : "";
            k += snprintf(buf + k, sizeof buf - k, " %llx %+.1f MB %s", (unsigned long long)grow[i].second, grow[i].first / 1048576.0, o.c_str());
        }
        log("%s", buf);
    }
    prev.swap(cur);
}

double guest_committed_mb() {  // committed pages of the guest reservation (the game's own memory; leak checks)
    uint64_t n = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (uint8_t* p = (uint8_t*)GUEST_BASE; p < (uint8_t*)GUEST_BASE + 0x100010000ull && VirtualQuery(p, &mi, sizeof mi); p = (uint8_t*)mi.BaseAddress + mi.RegionSize)
        if (mi.State == MEM_COMMIT) n += mi.RegionSize;
    return n / 1048576.0;
}

std::atomic<uint64_t> g_ww_reset_pages{0};  // NX_STALL_STATS: pages re-armed by ResetWriteWatch (each costs a fault on the next write)
// s118 (Augs, i5-8500 crosswalk): one GetWriteWatch + ResetWriteWatch per bound texture per submit was ~16% of the
// replay thread's busy time here (more on Intel CPUs with syscall mitigations). Written pages are now collected into a
// bitmap of the 4 GB guest space (128 KB): mem_written_span harvests a whole memory pool with one call per submit
// (WRITE_WATCH_FLAG_RESET), each texture then tests and clears its own pages. mem_written harvests its own range, so
// both see the same pages. NX_WW_BATCH=0: the old per-texture calls.
static std::mutex g_ww_lock;
static uint64_t g_ww_bits[(1ull << 32) >> 18];  // one bit per 4 KB guest page
// s126 (vertex data in video memory, vk_replay.cpp VbEntry): every harvest also stamps the written pages with the
// harvest tick, a record that reading does not clear (several cached copies can share a page; the bits above are taken
// by the texture checks). 4 MB.
static uint32_t g_ww_tick_of[(1ull << 32) >> 12];
static uint32_t g_ww_tick = 1;
static void ww_harvest(uint32_t lo, uint32_t len) {  // g_ww_lock held; lo, len page aligned
    thread_local std::vector<void*> hits;
    if (hits.size() < (len >> 12)) hits.resize(len >> 12);
    ULONG_PTR n = len >> 12;
    DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, GPTR(lo), len, hits.data(), &n, &gran) != 0) {
        for (uint64_t pg = lo >> 12; pg < ((uint64_t)lo + len) >> 12; pg++) g_ww_tick_of[pg] = g_ww_tick;  // unknown: count as written
        g_ww_tick++;
        return;
    }
    for (ULONG_PTR i = 0; i < n; i++) {
        const uint64_t pg = ((uint8_t*)hits[i] - (uint8_t*)GUEST_BASE) >> 12;
        g_ww_bits[pg >> 6] |= 1ull << (pg & 63);
        g_ww_tick_of[pg] = g_ww_tick;
    }
    if (n) g_ww_tick++;
    g_ww_reset_pages += n;
}
bool mem_write_ticks_ok() { return true; }
// harvests the 2 MB chunks under [addr, addr+size) once per stamp (crosswalk: a whole 1368 MB pool per frame was 0.8 ms)
uint32_t mem_write_tick(uint32_t addr, uint32_t size, uint64_t stamp) {
    static uint64_t chunk_stamp[(1ull << 32) >> 21];  // per 2 MB chunk: stamp + 1 of its last harvest here (16 KB)
    std::lock_guard<std::mutex> l(g_ww_lock);
    for (uint64_t c = addr >> 21, e = ((uint64_t)addr + size + (1 << 21) - 1) >> 21; c < e; c++)
        if (chunk_stamp[c] != stamp + 1) chunk_stamp[c] = stamp + 1, ww_harvest((uint32_t)(c << 21), 1u << 21);
    return g_ww_tick;
}
uint32_t mem_written_tick(uint32_t addr, uint32_t size) {  // no lock: aligned 32-bit reads, a racing harvest is seen next frame
    uint32_t t = 0;
    for (uint64_t pg = addr >> 12, e = ((uint64_t)addr + size + 0xFFF) >> 12; pg < e; pg++) {
        const uint32_t v = ((volatile uint32_t*)g_ww_tick_of)[pg];
        if (v > t) t = v;
    }
    return t;
}
static bool ww_take(uint32_t lo, uint32_t len) {  // g_ww_lock held: any page of [lo, lo+len) written; clears them
    bool any = false;
    for (uint64_t pg = lo >> 12, e = ((uint64_t)lo + len) >> 12; pg < e; pg++) {
        uint64_t& w = g_ww_bits[pg >> 6];
        if (!w && (pg & 63) == 0 && pg + 64 <= e) { pg += 63; continue; }
        const uint64_t b = 1ull << (pg & 63);
        if (w & b) w &= ~b, any = true;
    }
    return any;
}
bool mem_written(uint32_t addr, uint32_t size) {  // CPU wrote [addr, addr+size) since the last call for it
    if (!size) return false;
    uint32_t lo = addr & ~0xFFFu, len = ((addr + size + 0xFFF) & ~0xFFFu) - lo;
    std::lock_guard<std::mutex> l(g_ww_lock);
    ww_harvest(lo, len);
    return ww_take(lo, len);
}
bool mem_written_span(uint32_t addr, uint32_t size, uint32_t span_lo, uint32_t span_len, uint64_t stamp) {
    static const bool batch = !getenv("NX_WW_BATCH") || atoi(getenv("NX_WW_BATCH")) != 0;
    if (!batch || !span_len || addr < span_lo || (uint64_t)addr + size > (uint64_t)span_lo + span_len) return mem_written(addr, size);
    if (!size) return false;
    uint32_t lo = addr & ~0xFFFu, len = ((addr + size + 0xFFF) & ~0xFFFu) - lo;
    const uint32_t slo = span_lo & ~0xFFFu;
    const uint32_t slen = (uint32_t)((((uint64_t)span_lo + span_len + 0xFFF) & ~0xFFFull) - slo);
    std::lock_guard<std::mutex> l(g_ww_lock);
    static std::unordered_map<uint64_t, uint64_t> harvested;  // (span start, length) -> stamp of its last harvest
    uint64_t& h = harvested[(uint64_t)slo << 32 | slen];
    if (h != stamp + 1) {
        h = stamp + 1, ww_harvest(slo, slen);
        static const bool dbg = getenv("NX_WW_LOG") != nullptr;  // debug: harvest sizes
        static uint64_t n = 0, mb = 0;
        if (dbg) { n++, mb += slen >> 20; if (!(n & 1023)) log("ww: %llu harvests, %llu MB scanned", (unsigned long long)n, (unsigned long long)mb); }
    }
    if (harvested.size() > 4096) harvested.clear();
    return ww_take(lo, len);
}
#endif

Region mem_query(uint32_t addr) {
    std::lock_guard<std::mutex> l(g_mem_lock);
    auto it = g_regions.upper_bound(addr);
    if (it != g_regions.begin()) {
        auto p = std::prev(it);
        if ((uint64_t)p->second.addr + p->second.size > addr) return p->second;
    }
    // free gap: from the end of the previous region to the start of the next
    uint32_t lo = 0;
    if (it != g_regions.begin()) { auto p = std::prev(it); lo = p->second.addr + p->second.size; }
    uint64_t hi = it == g_regions.end() ? 0x100000000ull : it->first;
    return Region{lo, (uint32_t)(hi - lo), MS_FREE, 0, 0};
}

uint32_t kernel_alloc(uint32_t size, uint32_t align) {
    uint32_t a;
    {
        std::lock_guard<std::mutex> l(g_mem_lock);
        a = (g_kernel_next + align - 1) & ~(align - 1);
        size = (size + 0xFFF) & ~0xFFFu;
        if (a + size > KERNEL_BASE + KERNEL_SIZE) fatal("kernel region exhausted");
        g_kernel_next = a + size;
    }
    mem_map(a, size, MS_TLS, 3);
    return a;
}

uint32_t hle_alloc(uint32_t size, uint32_t align) {
    std::lock_guard<std::mutex> l(g_mem_lock);
    // ponytail: bump allocator, never freed; HLE allocations are few and long-lived
    uint32_t a = (g_hle_next + align - 1) & ~(align - 1);
    if ((uint64_t)a + size > (uint64_t)HLE_BASE + HLE_SIZE) fatal("HLE region exhausted");  // 32-bit sum wraps near 4 GiB
    uint32_t end = a + size;
    uint32_t page_lo = (g_hle_next + 0xFFF) & ~0xFFFu, page_hi = (end + 0xFFF) & ~0xFFFu;
    if (g_hle_next == HLE_BASE) page_lo = HLE_BASE;
    if (page_hi > page_lo) commit(page_lo, page_hi - page_lo);
    g_hle_next = end;
    return a;
}

uint32_t heap_set_size(uint32_t size) {
    if (size > HEAP_REGION_SIZE) return 0;
    if (size > g_heap_size) mem_map(HEAP_BASE, size, MS_NORMAL, 3);
    else if (size < g_heap_size) { mem_unmap(HEAP_BASE + size, g_heap_size - size); if (size) mem_map(HEAP_BASE, size, MS_NORMAL, 3); }
    g_heap_size = size;
    log("heap size %#x", size);
    return HEAP_BASE;
}

std::string gstr(uint32_t a, size_t max) {
    if (!a) return "(null)";
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)ld8(a + (uint32_t)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

}  // namespace rt
