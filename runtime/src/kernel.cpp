// Horizon kernel, high level: the SVCs the SDK's own nn::os code issues (32-bit register ABI, read from the
// SDK's svc wrappers: inputs r0-r4, result r0, outputs r1-r3). Semantics from Switchbrew.
// ponytail: one big kernel lock + per-thread condition variables; split it if contention ever shows in a profile.
#include "plat.h"
#ifndef _WIN32
#include <pthread.h>
#endif

#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <thread>

#include "rt.h"
#ifndef _WIN32
#undef R_OK  // <unistd.h> access() mode; here the Horizon result code below
#endif

namespace rt {

constexpr uint32_t R_OK = 0, R_TIMEOUT = 0xEA01, R_CANCELLED = 0xEC01, R_BAD_HANDLE = 0xE401,
                   R_INVALID_STATE = 0xFA01, R_NOT_FOUND = 0xF201;
constexpr uint32_t WAITERS = 0x40000000;
constexpr uint32_t PSEUDO_PROCESS = 0xFFFF8000, PSEUDO_THREAD = 0xFFFF8001;

static std::mutex K;                    // kernel lock
static std::condition_variable KCV;     // any waitable object changed (WaitSynchronization)
static std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
static uint32_t g_next_handle = 0x10001;
static uint64_t g_next_tid = 0x60;
static thread_local KThread* t_cur;

static std::unordered_map<uint32_t, std::vector<KThread*>> g_mutex_waiters, g_cv_waiters, g_addr_waiters;

struct ThreadExit {};

struct KEvent : KObject {
    bool sig = false;
    bool signaled() const override { return sig; }
};
struct KSessionStub : KObject {};

KThread* cur() { return t_cur; }

// SDK threads: the argument is the nn::os::ThreadType, its name pointer at +0xe4 (GetThreadNamePointer)
static std::string thread_name(const KThread* t) {
    return !t->name.empty() || !t->arg ? t->name : gstr(ld32(t->arg + 0xe4), 64);
}
static const bool g_trace_cv = getenv("NX_TRACE_CV") != nullptr;  // log condition-variable waits and signals

static uint32_t add_handle(std::shared_ptr<KObject> o) {
    uint32_t h = g_next_handle++;
    g_handles[h] = std::move(o);
    return h;
}
static KObject* get(uint32_t h) {
    if (h == PSEUDO_THREAD) return t_cur;
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second.get();
}
static KThread* get_thread(uint32_t h) { return dynamic_cast<KThread*>(get(h)); }

static std::chrono::steady_clock::time_point deadline(int64_t ns) {
    return std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns / g_speed);
}

// wait on t->cv (K held) until pred or timeout (ns < 0 = forever). Returns pred().
template <class P>
static bool kwait(std::unique_lock<std::mutex>& l, KThread* t, int64_t ns, P pred) {
    if (ns < 0) { t->cv.wait(l, pred); return true; }
    return t->cv.wait_until(l, deadline(ns), pred);
}

static void pop_waiter(std::vector<KThread*>& v, KThread* t) { v.erase(std::remove(v.begin(), v.end(), t), v.end()); }
static KThread* best_waiter(std::vector<KThread*>& v) {  // highest priority (lowest number), FIFO among equals
    auto it = std::min_element(v.begin(), v.end(), [](KThread* a, KThread* b) { return a->prio < b->prio; });
    KThread* t = *it;
    v.erase(it);
    return t;
}
static std::atomic<uint32_t>& gword(uint32_t a) { return *reinterpret_cast<std::atomic<uint32_t>*>(GPTR(a)); }

// hand a mutex tag at addr to its next waiter, or clear it (K held)
static void release_mutex(uint32_t addr) {
    auto it = g_mutex_waiters.find(addr);
    if (it == g_mutex_waiters.end() || it->second.empty()) { gword(addr).store(0); return; }
    KThread* w = best_waiter(it->second);
    uint32_t tag = w->self_tag | (it->second.empty() ? 0 : WAITERS);
    if (it->second.empty()) g_mutex_waiters.erase(it);
    gword(addr).store(tag);
    w->granted = true;
    w->cv.notify_one();
}

// a thread signaled from a condition variable wants the mutex back (K held)
static void reacquire(KThread* w) {
    auto& word = gword(w->cv_mutex);
    uint32_t cur = word.load();
    for (;;) {
        if (cur == 0) {
            if (word.compare_exchange_weak(cur, w->self_tag)) { w->granted = true; w->cv.notify_one(); return; }
            continue;
        }
        if (word.compare_exchange_weak(cur, cur | WAITERS)) break;
    }
    g_mutex_waiters[w->cv_mutex].push_back(w);
}

// ------------------------------------------------------------------------------------------------ threads
#ifdef _WIN32
static DWORD WINAPI thread_proc(void* p) {
#else
static void* thread_proc(void* p) {
#endif
    KThread* t = static_cast<KThread*>(p);
    t_cur = t;
#ifdef _WIN32
    log("thread: guest entry %08x = host thread %lu", (uint32_t)t->entry, GetCurrentThreadId());  // per-thread CPU (s115)
#endif
    try {
        dispatch(&t->cpu, t->entry);
    } catch (ThreadExit&) {
    }
    {
        std::lock_guard<std::mutex> l(K);
        t->exited = true;
    }
    KCV.notify_all();
    return 0;
}

static std::shared_ptr<KThread> new_thread(uint32_t entry, uint32_t arg, uint32_t sp, int prio, int core) {
    auto t = std::make_shared<KThread>();
    t->entry = entry;
    t->arg = arg;
    t->stack_top = sp;
    t->prio = prio;
    t->core = core;
    t->id = g_next_tid++;
    t->tlr = kernel_alloc(0x1000);
    t->cpu.r[0] = arg;
    t->cpu.r[13] = sp;
    t->cpu.r[14] = 0;
    t->cpu.tpidruro = t->tlr;
    t->cpu.fpscr = 0;  // TODO: Horizon's AArch32 default FPSCR (flush-to-zero?) unverified
    t->cpu.host = t.get();
    return t;
}

uint32_t create_main_thread(uint32_t stack_size, int prio) {
    uint32_t stack = kernel_alloc(stack_size);
    std::lock_guard<std::mutex> l(K);
    auto t = new_thread(0, 0, stack + stack_size, prio, 0);
    t->name = "main";
    t->handle = add_handle(t);
    t_cur = t.get();
    t->started = true;
    return t->handle;
}

static KThread* g_main_thread = nullptr;  // SleepThread: the main thread keeps the yield-spin (s115)
void run_main_thread(uint32_t entry, const uint32_t args[4]) {
    KThread* t = t_cur;
    g_main_thread = t;
    for (int i = 0; i < 4; i++) t->cpu.r[i] = args[i];
#ifdef _WIN32
    log("thread: guest main %08x = host thread %lu", entry, GetCurrentThreadId());
#endif
    try {
        dispatch(&t->cpu, entry);
    } catch (ThreadExit&) {
    }
    log("main thread returned");
}

static constexpr size_t HOST_STACK = 64u << 20;  // recompiled guest calls nest on the host stack

// ------------------------------------------------------------------------------------------------ svc
static void exit_thread() { throw ThreadExit{}; }

static uint32_t wait_sync(Cpu* c, uint32_t handles, int32_t count, int64_t timeout, uint32_t* index) {
    KThread* t = t_cur;
    std::vector<KObject*> objs;
    std::unique_lock<std::mutex> l(K);
    for (int i = 0; i < count; i++) {
        KObject* o = get(ld32(handles + 4 * i));
        if (!o) {  // the SDK's WaitSystemEvent aborts on this (seen at boot: NOTES "Boot abort")
            log("kernel: wait on bad handle %08x (%d of %d) in '%s'", ld32(handles + 4 * i), i, count, thread_name(t).c_str());
            return R_BAD_HANDLE;
        }
        objs.push_back(o);
    }
    auto ready = [&]() -> int {
        for (int i = 0; i < count; i++)
            if (objs[i]->signaled()) return i;
        return -1;
    };
    auto end = deadline(timeout < 0 ? 0 : timeout);
    for (;;) {
        int i = ready();
        if (i >= 0) { *index = (uint32_t)i; return R_OK; }
        if (t->cancel) { t->cancel = false; return R_CANCELLED; }
        if (timeout == 0) return R_TIMEOUT;
        if (timeout < 0) KCV.wait(l);
        else if (KCV.wait_until(l, end) == std::cv_status::timeout && ready() < 0) return R_TIMEOUT;
    }
}

static uint64_t get_info(uint32_t id, uint32_t handle, uint64_t sub, uint32_t* res) {
    *res = R_OK;
    switch (id) {
        case 0: return 0xF;                      // allowed core mask
        case 1: return 0xFFFFFFFFFFFFFFFFull;    // allowed priorities
        case 2: return ALIAS_BASE;
        case 3: return ALIAS_SIZE;
        case 4: return HEAP_BASE;
        case 5: return HEAP_REGION_SIZE;
        case 6: case 21: return HEAP_REGION_SIZE + 0x10000000ull;  // total memory: the SDK sizes the heap as total - used
        case 7: case 22: return 0x10000000ull;
        case 8: return 0;                        // debugger attached
        case 9: return 0;                        // resource limit handle
        case 10: return 0;
        case 11: { static std::mt19937_64 rng(0x7a5); return rng(); }
        case 12: return ASLR_BASE;
        case 13: return ASLR_SIZE;
        case 14: return STACK_BASE;
        case 15: return STACK_SIZE;
        case 16: case 17: return 0;
        case 18: return 0x0100A9400C9C2000ull;   // program id
        case 20: return 0;                       // user exception context
        case 0xF0000002: return guest_counter(); // thread tick count
        default:
            log("svcGetInfo id %u handle %08x sub %llx: unknown", id, handle, (unsigned long long)sub);
            *res = 0xF001;
            return 0;
    }
}

static void svc_impl(Cpu* c, uint32_t imm);

void svc(Cpu* c, uint32_t imm) {
    if (t_cur) t_cur->ran = true;
    uint32_t in[5];
    memcpy(in, c->r, sizeof in);
    svc_impl(c, imm);
    LOGV("svc %02x(%08x %08x %08x %08x %08x) -> %x %08x %08x  lr %s", imm, in[0], in[1], in[2], in[3], in[4], c->r[0],
         c->r[1], c->r[2], symbol_at(c->r[14]));
}

static void svc_impl(Cpu* c, uint32_t imm) {
    uint32_t* r = c->r;
    KThread* t = t_cur;
    switch (imm) {
        case 0x01: {  // SetHeapSize(size r1) -> r1 addr
            uint32_t a = heap_set_size(r[1]);
            r[0] = a ? R_OK : 0xD401;
            r[1] = a;
            return;
        }
        case 0x02: case 0x03: r[0] = R_OK; return;  // SetMemoryPermission / SetMemoryAttribute
        case 0x04: {  // MapMemory(dst, src, size): copy semantics (src is inaccessible while mapped)
            mem_map(r[0], r[2], MS_STACK, 3);
            memcpy(GPTR(r[0]), GPTR(r[1]), r[2]);
            r[0] = R_OK;
            return;
        }
        case 0x05: {  // UnmapMemory(dst, src, size)
            memcpy(GPTR(r[1]), GPTR(r[0]), r[2]);
            mem_unmap(r[0], r[2]);
            r[0] = R_OK;
            return;
        }
        case 0x06: {  // QueryMemory(MemoryInfo* r0, addr r2) -> r1 page info
            Region g = mem_query(r[2]);
            uint32_t mi = r[0];
            st64(mi + 0, g.addr);
            st64(mi + 8, g.size);
            st32(mi + 16, g.state);
            st32(mi + 20, g.attr);
            st32(mi + 24, g.perm);
            st32(mi + 28, 0);
            st32(mi + 32, 0);
            st32(mi + 36, 0);
            r[0] = R_OK;
            r[1] = 0;
            return;
        }
        case 0x07: log("svcExitProcess"); fflush(nullptr); ExitProcess(0);
        case 0x08: {  // CreateThread(entry r1, arg r2, stack_top r3, prio r0, core r4) -> r1 handle
            std::lock_guard<std::mutex> l(K);
            auto nt = new_thread(r[1], r[2], r[3], (int)r[0], (int)r[4] == -2 ? 0 : (int)r[4]);  // -2: process default core
            nt->handle = add_handle(nt);
            LOGV("CreateThread entry %s arg %08x sp %08x prio %d -> %x", symbol_at(r[1]), r[2], r[3], (int)r[0], nt->handle);
            r[0] = R_OK;
            r[1] = nt->handle;
            return;
        }
        case 0x09: {  // StartThread(handle)
            KThread* nt;
            {
                std::lock_guard<std::mutex> l(K);
                nt = get_thread(r[0]);
            }
            if (!nt) { r[0] = R_BAD_HANDLE; return; }
            nt->started = true;
#ifdef _WIN32
            HANDLE h = CreateThread(nullptr, HOST_STACK, thread_proc, nt, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (!h) fatal("host CreateThread failed");
            CloseHandle(h);
#else
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, HOST_STACK);  // reserved, touched on demand (as the Windows reservation)
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_t th;
            const int e = pthread_create(&th, &at, thread_proc, nt);
            pthread_attr_destroy(&at);
            if (e) fatal("host pthread_create failed (%s)", strerror(e));
#endif
            // On the console a higher-priority thread preempts its creator at once; games rely on it (e.g. reading
            // arguments from the creator's stack). Let it run until its first svc (or 100 ms) before returning.
            if (t && nt->prio < t->prio) {
                auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                while (!nt->ran && !nt->exited && std::chrono::steady_clock::now() < until) SwitchToThread();
            }
            r[0] = R_OK;
            return;
        }
        case 0x0A: exit_thread(); return;
        case 0x0B: {  // SleepThread(ns r0:r1)
            int64_t ns = (int64_t)((uint64_t)r[1] << 32 | r[0]);
            static const bool sleep_log = getenv("NX_SLEEP_LOG") != nullptr;  // debug: who sleeps how long
            if (sleep_log && t_cur) {
                static std::mutex m;
                static std::map<std::pair<std::string, int64_t>, uint32_t> n;
                std::lock_guard<std::mutex> l(m);
                uint32_t k = ++n[{thread_name(t_cur), ns}];
                if (k == 1 || k == 1000 || k == 10000) log("sleep: '%s' %lld ns x%u (lr %08x)", thread_name(t_cur).c_str(), (long long)ns, k, r[14]);
            }
            if (ns <= 0) SwitchToThread();
#ifdef _WIN32
            else if (ns < 1000000) {
                // sub-millisecond sleeps (main and AtJob workers poll the job system every 50 us): an OS sleep lasts
                // >= 1 ms on Windows, so the main thread lost up to 30% of each frame waiting. Yield until the deadline.
                // s115 (Augs, i5-8500 6 cores: "high CPU even when idle"): that yield-spin kept both AtJob workers at 100% of
                // a core each, also with no work (measured: 3 threads at 100% at 60 fps in the Fortuna Office with 1.5 ms
                // frames). A worker that comes straight back to sleep (< 20 us since the last one returned: it found no
                // job) 40 times in a row (~2 ms idle) sleeps on a high-resolution waitable timer instead, until it does
                // work again. The main thread keeps the spin (it waits for jobs inside its frame). NX_SLEEP_SPIN=1: the
                // old spin for every thread.
                const auto now = std::chrono::steady_clock::now();
                auto until = now + std::chrono::nanoseconds(ns / g_speed);
#ifdef _WIN32
                static const bool spin_all = getenv("NX_SLEEP_SPIN") && atoi(getenv("NX_SLEEP_SPIN")) != 0;
                thread_local std::chrono::steady_clock::time_point last_ret{};
                thread_local int idle = 0;
                thread_local HANDLE timer = nullptr;
                idle = now - last_ret < std::chrono::microseconds(20) ? idle + 1 : 0;
                if (!spin_all && idle > 40 && t_cur != g_main_thread) {
                    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                    LARGE_INTEGER due;
                    due.QuadPart = -std::max<LONGLONG>(1, ns / g_speed / 100);  // relative, 100 ns units
                    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 5);
                    else Sleep(1);
                    until = now;
                }
#endif
                while (std::chrono::steady_clock::now() < until) SwitchToThread();
#ifdef _WIN32
                last_ret = std::chrono::steady_clock::now();
#endif
            }
#endif
            // Linux: a real sleep (accurate to ~0.1 ms) also for the 50 us polls; yielding there kept two cores busy
            // (ROG Ally: they and the frame clock held all cores at 3.7 GHz, the game thread waited on them)
            else std::this_thread::sleep_for(std::chrono::nanoseconds(ns / g_speed));
            return;
        }
        case 0x0C: {  // GetThreadPriority(handle r1) -> r1
            std::lock_guard<std::mutex> l(K);
            KThread* x = get_thread(r[1]);
            r[0] = x ? R_OK : R_BAD_HANDLE;
            r[1] = x ? (uint32_t)x->prio : 0;
            return;
        }
        case 0x0D: {  // SetThreadPriority(handle, prio)
            std::lock_guard<std::mutex> l(K);
            if (KThread* x = get_thread(r[0])) { x->prio = (int)r[1]; r[0] = R_OK; } else r[0] = R_BAD_HANDLE;
            return;
        }
        case 0x0E: {  // GetThreadCoreMask(handle r2) -> r1 core, r2:r3 mask
            std::lock_guard<std::mutex> l(K);
            KThread* x = get_thread(r[2]);
            r[0] = x ? R_OK : R_BAD_HANDLE;
            r[1] = x ? (uint32_t)x->core : 0;
            r[2] = 0xF;
            r[3] = 0;
            return;
        }
        case 0x0F: {  // SetThreadCoreMask(handle r0, ideal core r1, mask r2:r3): the host schedules, but games read the
                      // core back (GetCurrentProcessorNumber) to pick per-core work queues
            std::lock_guard<std::mutex> l(K);
            KThread* x = get_thread(r[0]);
            if (!x) { r[0] = R_BAD_HANDLE; return; }
            int ideal = (int)r[1];
            uint32_t mask = r[2];
            if (ideal == -2) ideal = 0;                                          // process default core (main.npdm)
            if (ideal == -3) ideal = (x->core >= 0 && (mask >> x->core & 1)) ? x->core : -1;  // keep if still allowed
            if (ideal >= 0) x->core = ideal;
            else if (x->core < 0 || !(mask >> x->core & 1))
                for (int k = 0; k < 4; k++) if (mask >> k & 1) { x->core = k; break; }  // first allowed core
            LOGV("SetThreadCoreMask %x ideal %d mask %x -> core %d", r[0], (int)r[1], mask, x->core);
            r[0] = R_OK;
            return;
        }
        case 0x10: r[0] = t && t->core >= 0 && t->core <= 2 ? (uint32_t)t->core : 0; return;  // games index per-core data (cores 0-2)
        case 0x11: case 0x12: case 0x17: {  // SignalEvent / ClearEvent / ResetSignal
            {
                std::lock_guard<std::mutex> l(K);
                KEvent* e = dynamic_cast<KEvent*>(get(r[0]));
                if (!e) { r[0] = R_BAD_HANDLE; return; }
                if (imm == 0x17 && !e->sig) { r[0] = R_INVALID_STATE; return; }
                e->sig = imm == 0x11;
            }
            if (imm == 0x11) KCV.notify_all();
            r[0] = R_OK;
            return;
        }
        case 0x16: {  // CloseHandle
            std::lock_guard<std::mutex> l(K);
            g_handles.erase(r[0]);
            r[0] = R_OK;
            return;
        }
        case 0x18: {  // WaitSynchronization(handles r1, count r2, timeout r0:r3) -> r1 index
            int64_t to = (int64_t)((uint64_t)r[3] << 32 | r[0]);
            uint32_t idx = 0;
            r[0] = wait_sync(c, r[1], (int32_t)r[2], to, &idx);
            r[1] = idx;
            return;
        }
        case 0x19: {  // CancelSynchronization(thread handle)
            {
                std::lock_guard<std::mutex> l(K);
                if (KThread* x = get_thread(r[0])) x->cancel = true;
            }
            KCV.notify_all();
            r[0] = R_OK;
            return;
        }
        case 0x1A: {  // ArbitrateLock(owner r0, addr r1, self tag r2)
            std::unique_lock<std::mutex> l(K);
            if (gword(r[1]).load() != (r[0] | WAITERS)) { r[0] = R_OK; return; }
            t->self_tag = r[2];
            t->granted = false;
            g_mutex_waiters[r[1]].push_back(t);
            kwait(l, t, -1, [&] { return t->granted; });
            r[0] = R_OK;
            return;
        }
        case 0x1B: {  // ArbitrateUnlock(addr)
            std::lock_guard<std::mutex> l(K);
            release_mutex(r[0]);
            r[0] = R_OK;
            return;
        }
        case 0x1C: {  // WaitProcessWideKeyAtomic(mutex r0, key r1, tag r2, timeout r3:r4)
            int64_t to = (int64_t)((uint64_t)r[4] << 32 | r[3]);
            std::unique_lock<std::mutex> l(K);
            uint32_t mtx = r[0], key = r[1];
            release_mutex(mtx);
            t->self_tag = r[2];
            t->cv_key = key;
            t->cv_mutex = mtx;
            t->granted = false;
            t->cv_signaled = false;
            g_cv_waiters[key].push_back(t);
            gword(key).store(1);
            if (g_trace_cv) log("cv wait %08x '%s' timeout %lld", key, thread_name(t).c_str(), (long long)to);
            bool ok = kwait(l, t, to, [&] { return t->granted || t->cv_signaled; });
            if (!ok) {  // timed out while still on the condition variable
                auto& v = g_cv_waiters[key];
                pop_waiter(v, t);
                if (v.empty()) { g_cv_waiters.erase(key); gword(key).store(0); }
                r[0] = R_TIMEOUT;
                return;
            }
            kwait(l, t, -1, [&] { return t->granted; });  // signaled: now wait for the mutex hand-off
            r[0] = R_OK;
            return;
        }
        case 0x1D: {  // SignalProcessWideKey(key r0, count r1)
            std::lock_guard<std::mutex> l(K);
            auto it = g_cv_waiters.find(r[0]);
            if (g_trace_cv) log("cv signal %08x n %d by '%s': %zu waiting", r[0], (int32_t)r[1], thread_name(t).c_str(),
                                it == g_cv_waiters.end() ? (size_t)0 : it->second.size());
            if (it != g_cv_waiters.end()) {
                int32_t n = (int32_t)r[1];
                bool all = n <= 0;  // -1: every waiter
                while (!it->second.empty() && (all || n-- > 0)) {
                    KThread* w = best_waiter(it->second);
                    w->cv_signaled = true;
                    reacquire(w);
                }
                if (it->second.empty()) { g_cv_waiters.erase(it); gword(r[0]).store(0); }
            }
            r[0] = R_OK;
            return;
        }
        case 0x1F: {  // ConnectToNamedPort(name r1) -> r1 handle
            r[1] = ipc_connect(gstr(r[1], 16).c_str());
            r[0] = r[1] ? R_OK : R_NOT_FOUND;
            return;
        }
        case 0x21: r[0] = ipc_request(r[0], t->tlr, 0x100); return;           // SendSyncRequest(handle)
        case 0x22: r[0] = ipc_request(r[2], r[0], r[1]); return;              // ...WithUserBuffer(buf, size, handle)
        case 0x25: {  // GetThreadId(handle r1) -> r1:r2
            std::lock_guard<std::mutex> l(K);
            KThread* x = get_thread(r[1]);
            r[0] = x ? R_OK : R_BAD_HANDLE;
            r[1] = x ? (uint32_t)x->id : 0;
            r[2] = x ? (uint32_t)(x->id >> 32) : 0;
            return;
        }
        case 0x26: fatal("svcBreak(%x, %08x, %x) lr %s", r[0], r[1], r[2], symbol_at(c->r[14]));
        case 0x27: log("[guest] %.*s", (int)r[1], (const char*)GPTR(r[0])); r[0] = R_OK; return;
        case 0x29: {  // GetInfo(id r1, handle r2, sub r0:r3) -> r1:r2
            uint32_t res;
            uint64_t v = get_info(r[1], r[2], (uint64_t)r[3] << 32 | r[0], &res);
            r[0] = res;
            r[1] = (uint32_t)v;
            r[2] = (uint32_t)(v >> 32);
            return;
        }
        case 0x2A: case 0x2B: case 0x2C: case 0x5F: r[0] = R_OK; return;  // cache maintenance
        case 0x34: {  // WaitForAddress(addr r0, type r1, value r2, timeout r3:r4)
            int64_t to = (int64_t)((uint64_t)r[4] << 32 | r[3]);
            std::unique_lock<std::mutex> l(K);
            uint32_t addr = r[0], type = r[1];
            int32_t v = (int32_t)gword(addr).load(), want = (int32_t)r[2];
            bool wait = type == 2 ? v == want : v < want;
            if (!wait) { r[0] = R_INVALID_STATE; return; }
            if (type == 1) gword(addr).store((uint32_t)(v - 1));
            if (to == 0) { r[0] = R_TIMEOUT; return; }
            t->addr_woken = false;
            g_addr_waiters[addr].push_back(t);
            if (!kwait(l, t, to, [&] { return t->addr_woken; })) {
                pop_waiter(g_addr_waiters[addr], t);
                r[0] = R_TIMEOUT;
                return;
            }
            r[0] = R_OK;
            return;
        }
        case 0x35: {  // SignalToAddress(addr r0, type r1, value r2, count r3)
            std::lock_guard<std::mutex> l(K);
            uint32_t addr = r[0], type = r[1];
            int32_t count = (int32_t)r[3];
            auto& v = g_addr_waiters[addr];
            if (type == 1 || type == 2) {
                if ((int32_t)gword(addr).load() != (int32_t)r[2]) { r[0] = R_INVALID_STATE; return; }
                int32_t value = (int32_t)r[2], waiters = (int32_t)v.size(), nv = value + 1;
                if (type == 2 && waiters) {  // modify by waiting count (Horizon's KAddressArbiter rule)
                    if (count <= 0) nv = value - 2;                   // all woken
                    else nv = waiters <= count ? value - 1 : value;   // every waiter woken / some stay asleep
                }
                gword(addr).store((uint32_t)nv);
            }
            bool all = count <= 0;
            while (!v.empty() && (all || count-- > 0)) {
                KThread* w = best_waiter(v);
                w->addr_woken = true;
                w->cv.notify_one();
            }
            r[0] = R_OK;
            return;
        }
        case 0x45: {  // CreateEvent -> r1 writable, r2 readable
            std::lock_guard<std::mutex> l(K);
            auto e = std::make_shared<KEvent>();
            r[1] = add_handle(e);
            r[2] = add_handle(e);
            r[0] = R_OK;
            return;
        }
        default:
            fatal("unimplemented svc %#x (r0 %08x r1 %08x r2 %08x) lr %s", imm, r[0], r[1], r[2], symbol_at(c->r[14]));
    }
}

void guest_backtrace(const Cpu& c);

// NX_THREAD_DUMP=<seconds>: after that long, log where every guest thread is (debugging hangs)
void kernel_init() {
    const char* s = getenv("NX_THREAD_DUMP");
    if (!s) return;
    int secs = atoi(s);
    std::thread([secs] {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        std::vector<KThread*> ts;
        {
            std::lock_guard<std::mutex> l(K);
            for (auto& [h, o] : g_handles)
                if (auto* t = dynamic_cast<KThread*>(o.get())) ts.push_back(t);
        }
        for (KThread* t : ts) {
            std::string name = thread_name(t);
            log("thread %x '%s' prio %d core %d entry %s%s", t->handle, name.c_str(), t->prio, t->core, symbol_at(t->entry),
                t->exited ? " (exited)" : "");
            guest_backtrace(t->cpu);
        }
    }).detach();
}

uint32_t kernel_add_object(std::shared_ptr<KObject> o) {
    std::lock_guard<std::mutex> l(K);
    return add_handle(std::move(o));
}
uint32_t kevent_create(uint32_t* readable) {
    std::lock_guard<std::mutex> l(K);
    auto e = std::make_shared<KEvent>();
    uint32_t w = add_handle(e);
    *readable = add_handle(e);
    return w;
}
void kevent_signal(uint32_t h) {
    {
        std::lock_guard<std::mutex> l(K);
        if (KEvent* e = dynamic_cast<KEvent*>(get(h))) e->sig = true;
    }
    KCV.notify_all();
}
KObject* kernel_get_object(uint32_t h) {
    std::lock_guard<std::mutex> l(K);
    return get(h);
}

}  // namespace rt
