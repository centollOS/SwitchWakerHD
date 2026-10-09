// Guest memory, RPX loading, function dispatch, logging and HLE registry.
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/ldsyms.h>
#endif
#include "platform/debug_server.h"
#include "platform/host.h"
#include "write_watch.h"
#include <zlib.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
#include <unistd.h>
#endif

#include "recomp_table.h"
#include "runtime.h"

bool g_trace_hle = false;
namespace config {
std::string game_dir = "game";
std::string save_dir = "save";
}  // namespace config

static std::mutex g_log_mutex;

// the last log lines, kept for crash logs (crash handlers read them without the lock)
static constexpr int kLogRing = 200, kLogLine = 240;
static char g_log_ring[kLogRing][kLogLine];
static std::atomic<uint32_t> g_log_next{0};
// the log file (main.cpp, captures/wwhd.log): every line in full, under the log lock
static void (*g_log_sink)(const char*, size_t) = nullptr;
void log_set_sink(void (*sink)(const char*, size_t)) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log_sink = sink;
}

#ifdef __SWITCH__
// The log is a file on the SD card: a write can take milliseconds, and the thread that logs (render,
// audio, the game's main thread) would stall for it. Lines go to a buffer that a writer thread
// writes out four times a second. fatal() and log_flush() write it at once.
static std::string g_log_pending;
static constexpr size_t kLogPendingMax = 4 << 20;  // dropped beyond this (a flood must not eat memory; a capture
                                                   // frame's per-draw trace is ~1.5 MB)
static uint64_t g_log_dropped = 0;

void log_flush() {
    std::string out;
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        out.swap(g_log_pending);
    }
    if (!out.empty()) {
        fwrite(out.data(), 1, out.size(), stderr);  // (logs/wwhd_<date>_<time>.log, main.cpp)
        fflush(stderr);
        debugsrv::log_tap(out.data(), out.size());  // "log" streams of the debug server (when it runs)
    }
}

// the heap malloc has never grown into, in MiB (libnx's heap end against malloc's break; no lock).
// Freed blocks inside the used part come on top of it.
extern "C" char* fake_heap_end;
size_t heap_never_used_mib() {
    char* top = static_cast<char*>(sbrk(0));
    if (!fake_heap_end || !top || top == reinterpret_cast<char*>(-1)) return 0;
    return size_t(fake_heap_end - top) >> 20;
}
void log_heap(const char* when) { LOG("[mem] %s: %zu MiB of heap never used", when, heap_never_used_mib()); }

// From the CPU exception handler (main.cpp): the pending lines and then text, written straight to the
// files' descriptors. Nothing waits for a lock: the thread that crashed may hold the log's or stdio's.
void log_crash_write(const char* text, size_t n) {
    std::string out;
    if (g_log_mutex.try_lock()) {
        out.swap(g_log_pending);
        g_log_mutex.unlock();
    }
    if (const int fd = fileno(stderr); fd >= 0) {
        if (!out.empty()) (void)!write(fd, out.data(), out.size());
        (void)!write(fd, text, n);
        fsync(fd);
    }
}

static void log_writer_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 << 10);
        if (pthread_create(&t, &attr, [](void*) -> void* {
                // above the game's threads (59), like the other host service threads: the log keeps
                // reaching the SD card while game threads are busy or stuck
                host::raise_thread_priority();  // and off the main thread's core
                for (;;) {
                    svcSleepThread(250'000'000ll);
                    log_flush();
                }
                return nullptr;
            }, nullptr) == 0)
            pthread_detach(t);
        pthread_attr_destroy(&attr);
    });
}

void log_msg(const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    n = std::min<int>(n, sizeof line - 2);
    line[n++] = '\n';
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (g_log_pending.size() + n <= kLogPendingMax) g_log_pending.append(line, n);
        else g_log_dropped++;
        char* ring = g_log_ring[g_log_next.load() % kLogRing];  // for the crash log ring (log_ring_write)
        const size_t k = std::min<size_t>(size_t(n) - 1, kLogLine - 1);
        memcpy(ring, line, k);
        ring[k] = 0;
        g_log_next++;
    }
    log_writer_start();
}
#else
void log_flush() { fflush(stderr); }

void log_msg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    char* line = g_log_ring[g_log_next.load() % kLogRing];
    va_list ap2;
    va_copy(ap2, ap);
    vsnprintf(line, kLogLine, fmt, ap2);
    va_end(ap2);
    g_log_next++;
    if (g_log_sink) {
        char full[4096];
        va_list ap3;
        va_copy(ap3, ap);
        int n = vsnprintf(full, sizeof full, fmt, ap3);
        va_end(ap3);
        if (n > 0) g_log_sink(full, std::min((size_t)n, sizeof full - 1));
    }
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "wwhd", fmt, ap);  // adb logcat -s wwhd
    va_end(ap);
#else
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}

#endif

void log_ring_write(int fd, void (*out)(int, const char*, size_t)) {
    uint32_t n = g_log_next.load();
    uint32_t first = n > (uint32_t)kLogRing ? n - kLogRing : 0;
    for (uint32_t i = first; i < n; i++) {
        const char* l = g_log_ring[i % kLogRing];
        out(fd, l, strnlen(l, kLogLine));
        out(fd, "\n", 1);
    }
}

// check builds of the recompiler's single-precision tracking (ppc.h ppc_single_check)
extern "C" void ppc_single_failed(uint32_t at, double v) {
    static std::mutex m;
    static std::unordered_map<uint32_t, uint64_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen[at]++ == 0 && seen.size() <= 100)
        LOG("[single check] %08X: multiplier %.17g (%016llX) is not single precision", at, v,
            (unsigned long long)f64_as_u64(v));
}

void fatal(const char* fmt, ...) {
    log_flush();
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        va_list ap;
        va_start(ap, fmt);
        char msg[1024];
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_FATAL, "wwhd", "%s", msg);
#else
        fprintf(stderr, "FATAL: %s\n", msg);
        fflush(stderr);
#endif
    }
    abort();
}

// ---------------------------------------------------------------- memory
#ifdef __SWITCH__
extern "C" { uint8_t* ppc_mem_base_var = nullptr; }
// libnx implements no pthread_detach hook (it returns ENOSYS, which makes std::thread::detach throw).
// Detached host threads here run for the whole session, so not reclaiming them on exit is fine.
struct __pthread_t;
extern "C" int __syscall_thread_detach(struct __pthread_t*) { return 0; }
// libnx gives a thread created without attributes a 128 KiB stack. Mesa creates its GL thread
// (WWHD_GL_THREAD) that way, and that thread runs the GLSL compiler, which needs far more.
// The link wraps pthread_create (CMakeLists.txt) so such threads get 4 MiB.
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    size_t size = 0;
    if (attr && pthread_attr_getstacksize(attr, &size) == 0 && size) return __real_pthread_create(thread, attr, fn, arg);
    pthread_attr_t big;
    if (attr) big = *attr;
    else pthread_attr_init(&big);
    pthread_attr_setstacksize(&big, 4 << 20);
    int r = __real_pthread_create(thread, &big, fn, arg);
    if (!attr) pthread_attr_destroy(&big);
    if (r) LOG("[boot] pthread_create with a 4 MiB stack failed (%d)", r);
    return r;
}
// newlib's C11 threads define thrd_success as 4; Mesa (switch-mesa, built against newlib's <threads.h>
// but with its own success test, `if (ret) fail`) treats that as a failure. Its util_queue then frees
// the queue of a thread that is already running, and Mesa's GL thread (WWHD_GL_THREAD) never starts.
// Mesa is the only caller of thrd_create, and it only tests for zero: success becomes 0 here.
extern "C" int __real_thrd_create(void* thread, int (*fn)(void*), void* arg);
extern "C" int __wrap_thrd_create(void* thread, int (*fn)(void*), void* arg) {
    int r = __real_thrd_create(thread, fn, arg);
    if (r != 4 /* newlib thrd_success */) LOG("[boot] thrd_create failed (%d)", r);
    return r == 4 ? 0 : (r ? r : 2);
}
#endif
namespace mem {
static std::atomic<uint32_t> g_runtime_top{kRuntimeStart};

#ifdef __SWITCH__
// Horizon commits every mapping, so only the guest ranges the game and the runtime use are backed:
// heap pages are aliased into a reserved 4 GiB window (code-memory region, read/write). Everything the
// desktop builds can touch (all of it is mapped there) except the large gaps is backed.
struct GuestRange { uint32_t start, size; };
static constexpr GuestRange kGuestRanges[] = {
    {0x00010000, 0x04000000 - 0x00010000},  // low memory and .text / .rodata of cking.rpx
    {kMem2Start, kMem2End - kMem2Start},
    {kRuntimeStart, kRuntimeEnd - kRuntimeStart},
    {0xC0000000, 0x02100000},          // import slots, data imports, host function addresses
    {kFgBucket, kFgBucketSize},
    {kMem1, kMem1Size},
};
static void init_switch() {
    if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x73))
        fatal("guest memory needs svcMapProcessCodeMemory/svcSetProcessMemoryPermission (run from hbmenu over a game, "
              "not the album applet)");
    Handle self = envGetOwnProcessHandle();
    virtmemLock();
    void* window = virtmemFindCodeMemory(0x100000000ull, 0x10000);
    if (!window || !virtmemAddReservation(window, 0x100000000ull)) {
        virtmemUnlock();
        fatal("cannot reserve a 4 GiB guest address window");
    }
    virtmemUnlock();
    uint64_t total = 0;
    for (const auto& r : kGuestRanges) {
        void* backing = aligned_alloc(0x1000, r.size);
        if (!backing)
            fatal("out of memory backing guest range %08X (+%X); start the game with full RAM (hold R on a title)",
                  r.start, r.size);
        memset(backing, 0, r.size);
        uint8_t* dst = (uint8_t*)window + r.start;
        Result rc = svcMapProcessCodeMemory(self, (u64)dst, (u64)backing, r.size);
        if (R_FAILED(rc)) fatal("svcMapProcessCodeMemory %08X failed (0x%X)", r.start, rc);
        rc = svcSetProcessMemoryPermission(self, (u64)dst, r.size, Perm_Rw);
        if (R_FAILED(rc)) fatal("svcSetProcessMemoryPermission %08X failed (0x%X)", r.start, rc);
        total += r.size;
    }
    ppc_mem_base_var = (uint8_t*)window;
    LOG("[mem] guest window at %p, %llu MiB backed", window, (unsigned long long)(total >> 20));
    log_heap("after the guest memory");
}
#endif

void init() {
#ifdef __SWITCH__
    init_switch();
#elif defined(__APPLE__)
    mach_vm_address_t addr = (mach_vm_address_t)PPC_MEM_BASE;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, 0x100000000ull, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) fatal("cannot reserve guest address space at %p (kr=%d)", PPC_MEM_BASE, kr);
    // null page guard: catches guest null-pointer accesses
    mprotect(PPC_MEM_BASE, 0x10000, PROT_NONE);
#elif defined(_WIN32)
    void* p = VirtualAlloc(PPC_MEM_BASE, 0x100000000ull, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if(p != PPC_MEM_BASE) fatal("cannot reserve guest address space (error=%lu)",GetLastError());
    DWORD old; if(!VirtualProtect(PPC_MEM_BASE,0x10000,PAGE_NOACCESS,&old)) fatal("cannot protect guest null page");
#else
    // Never replace existing mappings: requesting a hint and checking the result is safe on
    // systems whose headers lack MAP_FIXED_NOREPLACE.
#ifdef __ANDROID__
    // phones have little RAM and strict commit accounting: pages are committed on first touch
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
#else
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    if(p != PPC_MEM_BASE) { if(p!=MAP_FAILED)munmap(p,0x100000000ull); fatal("cannot reserve guest address space at %p",PPC_MEM_BASE); }
    if(mprotect(PPC_MEM_BASE,0x10000,PROT_NONE))fatal("cannot protect guest null page");
#endif
    // texture change detection (write_watch.h): after the crash handler, which it chains to
    if (!wwatch::init(PPC_MEM_BASE, 0x100000000ull)) LOG("[mem] write tracking unavailable: textures use sampled change checks");
}

static std::mutex g_alloc_log_m;
static std::vector<AllocRec> g_alloc_log;

static uint32_t bump(std::atomic<uint32_t>& top, uint32_t size, uint32_t align, uint32_t end) {
    uint32_t cur = top.load();
    uint32_t start;
    do {
        start = (cur + align - 1) & ~(align - 1);
        if (start + size > end) fatal("runtime guest region exhausted");
    } while (!top.compare_exchange_weak(cur, start + size));
    memset(ptr(start), 0, size);
    return start;
}

__attribute__((noinline)) uint32_t runtime_alloc(uint32_t size, uint32_t align) {
    uint32_t a = bump(g_runtime_top, size, align, kHostStart);
    // who allocated (relative to the executable, stable across runs of one build): a loaded save
    // state requires the same guest-visible allocations at the same addresses
    uint64_t tag = (uint64_t)((uintptr_t)__builtin_return_address(0) - host::executable_base());
    std::lock_guard<std::mutex> lk(g_alloc_log_m);
    g_alloc_log.push_back({a, size, tag});
    return a;
}

static std::atomic<uint32_t> g_host_top{kHostStart};
uint32_t host_alloc(uint32_t size, uint32_t align) { return bump(g_host_top, size, align, kFixedStart); }

uint32_t runtime_top() { return g_runtime_top.load(); }
void raise_runtime_top(uint32_t top) {
    uint32_t cur = g_runtime_top.load();
    while (cur < top && !g_runtime_top.compare_exchange_weak(cur, top)) {}
}
std::vector<AllocRec> runtime_alloc_log() {
    std::vector<AllocRec> v;
    {
        std::lock_guard<std::mutex> lk(g_alloc_log_m);
        v = g_alloc_log;
    }
    std::sort(v.begin(), v.end(), [](const AllocRec& a, const AllocRec& b) { return a.addr < b.addr; });
    return v;
}

std::string read_cstr(uint32_t ea) {
    if (!ea) return {};
    return std::string((const char*)ptr(ea));
}

void write_cstr(uint32_t ea, const std::string& s, uint32_t max) {
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), max - 1);
    memcpy(ptr(ea), s.data(), n);
    ptr(ea)[n] = 0;
}
}  // namespace mem

// ---------------------------------------------------------------- loader
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool load_rpx(const std::string& path, LoadedModule& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    uint32_t shoff = be32(&d[0x20]);
    uint16_t shentsize = be16(&d[0x2E]), shnum = be16(&d[0x30]);
    out.entry = be32(&d[0x18]);
    out.data_end = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t* sh = &d[shoff + i * shentsize];
        uint32_t type = be32(sh + 4), flags = be32(sh + 8), addr = be32(sh + 12), off = be32(sh + 16), size = be32(sh + 20);
        std::vector<uint8_t> data;
        if (type != 8 && size) {
            if (flags & 0x08000000) {
                uLongf dlen = be32(&d[off]);
                data.resize(dlen);
                if (uncompress(data.data(), &dlen, &d[off + 4], size - 4) != Z_OK) fatal("zlib error in section %d", i);
            } else {
                data.assign(d.begin() + off, d.begin() + off + size);
            }
        }
        if (type == 0x80000004 && data.size() >= 0x30) {  // RPL file info
            out.sda_base = be32(&data[0x24]);
            out.sda2_base = be32(&data[0x28]);
            out.stack_size = be32(&data[0x2C]);
        }
        // allocated sections in the text/data regions
        if ((flags & 2) && addr >= 0x02000000 && addr < 0xC0000000) {
            if (type == 8) {
                memset(mem::ptr(addr), 0, size);
                out.data_end = std::max(out.data_end, addr + size);
            } else {
                memcpy(mem::ptr(addr), data.data(), data.size());
                if (addr >= mem::kMem2Start) out.data_end = std::max(out.data_end, addr + (uint32_t)data.size());
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- dispatch
namespace dispatch {
static constexpr uint32_t kTextBase = 0x02000000;
static constexpr uint32_t kTextSize = 0x01000000;  // 16 MiB covers cking.rpx .text
static PpcFunc* g_text_table;                        // indexed by (addr - kTextBase) / 4
// lock-free direct tables for import slots and host functions (called through pointers a lot)
static constexpr uint32_t kSlotBase = 0xC0000000, kSlotSize = 0x40000;     // import stubs, 4-byte steps
static constexpr uint32_t kHostSize = 0x80000;                              // host functions, 8-byte steps
static std::atomic<PpcFunc> g_slot_table[kSlotSize / 4];
static std::atomic<PpcFunc> g_host_table[kHostSize / 8];
static std::unordered_map<uint32_t, PpcFunc> g_other;
static std::shared_mutex g_other_mutex;
static std::atomic<uint32_t> g_next_host{mem::kHleFuncBase};
static std::unordered_map<uint32_t, std::string> g_host_names;

void init() {
    g_text_table = (PpcFunc*)calloc(kTextSize / 4, sizeof(PpcFunc));
    for (unsigned i = 0; i < g_recomp_func_count; i++) set(g_recomp_funcs[i].addr, g_recomp_funcs[i].fn);
    // imported functions are reachable through their import slot addresses
    for (unsigned i = 0; i < g_recomp_import_count; i++)
        if (g_recomp_imports[i].is_func) set(g_recomp_imports[i].slot, g_recomp_imports[i].fn);
}

void set(uint32_t addr, PpcFunc fn) {
    if (addr - kTextBase < kTextSize) {
        g_text_table[(addr - kTextBase) >> 2] = fn;
        return;
    }
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) { g_slot_table[(addr - kSlotBase) >> 2] = fn; return; }
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7)) { g_host_table[(addr - mem::kHleFuncBase) >> 3] = fn; return; }
    std::unique_lock lk(g_other_mutex);
    g_other[addr] = fn;
}

PpcFunc lookup(uint32_t addr) {
    if (addr - kTextBase < kTextSize) return g_text_table[(addr - kTextBase) >> 2];
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) return g_slot_table[(addr - kSlotBase) >> 2].load(std::memory_order_relaxed);
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7))
        return g_host_table[(addr - mem::kHleFuncBase) >> 3].load(std::memory_order_relaxed);
    std::shared_lock lk(g_other_mutex);
    auto it = g_other.find(addr);
    return it == g_other.end() ? nullptr : it->second;
}

std::vector<std::pair<uint32_t, std::string>> host_functions() {
    std::shared_lock lk(g_other_mutex);
    std::vector<std::pair<uint32_t, std::string>> v(g_host_names.begin(), g_host_names.end());
    std::sort(v.begin(), v.end());
    return v;
}

uint32_t register_host(PpcFunc fn, const char* name) {
    uint32_t a = g_next_host.fetch_add(8);
    set(a, fn);
    std::unique_lock lk(g_other_mutex);
    g_host_names[a] = name;
    return a;
}
}  // namespace dispatch

// checking build of the recompiler's CR liveness pass (WWHD_RECOMP_CR_CHECK=1): a dropped CR bit was read
extern "C" void ppc_cr_poisoned(Cpu* c, int bit, uint32_t addr) {
    fatal("CR liveness: bit %d read at %08X without a live store (lr=%08X)", bit, addr, c->lr);
}

// checking build of the recompiler's register locals (WWHD_RECOMP_NONLEAF_CHECK=1): a callee changed
// a callee-saved register (reg 32+n: f n), which the Switch build assumes never happens
extern "C" void ppc_keep_failed(Cpu* c, int reg, uint32_t fn) {
    fatal("callee-saved %s%d changed across a call in %08X (lr=%08X)", reg < 32 ? "r" : "f", reg % 32, fn, c->lr);
}

extern "C" void ppc_dispatch(Cpu* c) {
    PpcFunc f = dispatch::lookup(c->pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", c->pc, c->lr, c->ctr);
    MUSTTAIL return f(c);
}

// PPC_IJUMP's slow path: look the target up and remember it in the jump site's entry
extern "C" PpcFunc ppc_ijump_resolve(Cpu* c, uint64_t* slot) {
    const uint32_t pc = c->pc;
    PpcFunc f = dispatch::lookup(pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", pc, c->lr, c->ctr);
    const intptr_t off = (intptr_t)f - (intptr_t)&ppc_dispatch;
    if (off == (intptr_t)(int32_t)off) {  // the newest target first, the previous one second (ppc.h)
        __atomic_store_n(&slot[1], __atomic_load_n(&slot[0], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
        __atomic_store_n(&slot[0], (uint64_t)pc << 32 | (uint32_t)(int32_t)off, __ATOMIC_RELAXED);
    }
    return f;
}

// PPC_ICALL's slow path: look the target up, remember it in the call site's entry, call it
extern "C" void ppc_icall_miss(Cpu* c, uint64_t* slot) {
    const uint32_t pc = c->pc;
    PpcFunc f = dispatch::lookup(pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", pc, c->lr, c->ctr);
    const intptr_t off = (intptr_t)f - (intptr_t)&ppc_dispatch;
    if (off == (intptr_t)(int32_t)off) {  // the newest target first, the previous one second (ppc.h)
        __atomic_store_n(&slot[1], __atomic_load_n(&slot[0], __ATOMIC_RELAXED), __ATOMIC_RELAXED);
        __atomic_store_n(&slot[0], (uint64_t)pc << 32 | (uint32_t)(int32_t)off, __ATOMIC_RELAXED);
    }
    f(c);
}

#if defined(__SWITCH__) && defined(PPC_BASE_REG)
// WWHD_SWITCH_BASE_REG: game code finds the guest memory base in x28 (ppc.h). Every way from host
// code into game code passes here (thread entries, alarms, audio and other callbacks, mods); the
// previous x28 comes back afterwards, also when an exception (GuestExit) leaves, so a library that
// calls the runtime back gets its own value again.
struct GuestBaseScope {
    uint64_t old;
    GuestBaseScope() { __asm__ volatile("mov %0, x28\n\tmov x28, %1" : "=&r"(old) : "r"(ppc_mem_base_var)); }
    ~GuestBaseScope() { __asm__ volatile("mov x28, %0" : : "r"(old)); }
};
#else
struct GuestBaseScope {};
#endif

uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    GuestBaseScope base;
    uint32_t save_lr = c->lr, save_ctr = c->ctr, save_sp = c->r[1];
    // open a minimal frame so the callee's LR save slot doesn't clobber our caller's frame
    c->r[1] -= 0x40;
    st32(c->r[1], save_sp);
    int i = 3;
    for (uint32_t a : args) c->r[i++] = a;
    c->lr = 0;
    c->pc = fn;
    ppc_dispatch(c);
    c->r[1] = save_sp;
    c->lr = save_lr;
    c->ctr = save_ctr;
    return c->r[3];
}

extern "C" void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn) {
    fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, addr, c->lr);
}

extern "C" void ppc_trap(Cpu* c, uint32_t addr) {
    fatal("guest trap at %08X (lr=%08X r3=%08X)", addr, c->lr, c->r[3]);
}

// ---------------------------------------------------------------- HLE registry
static std::vector<HleReg*>& hle_registry() {
    static std::vector<HleReg*> r;
    return r;
}
HleReg::HleReg(const char* l, const char* n, PpcFunc f) : lib(l), name(n), fn(f) { hle_registry().push_back(this); }

PpcFunc hle_find(const char* lib, const char* name) {
    std::string l = lib;
    if (l.size() > 4 && l.substr(l.size() - 4) == ".rpl") l.resize(l.size() - 4);
    for (HleReg* r : hle_registry())
        if (l == r->lib && strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

PpcFunc hle_find_any(const char* name) {
    for (HleReg* r : hle_registry())
        if (strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

extern "C" void hle_unimplemented(Cpu* c, const char* lib, const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        int& n = seen[std::string(lib) + "." + name];
        if (n++ < 3)
            log_msg("[hle] unimplemented %s.%s(%08X, %08X, %08X, %08X) lr=%08X", lib, name, c->r[3], c->r[4], c->r[5],
                    c->r[6], c->lr);
    }
    c->r[3] = 0;
}
