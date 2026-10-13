// SwitchWakerHD (this fork): the Switch parts of core.cpp, behind its marked hooks (core_switch.h): the
// buffered SD-card log and its writer thread, heap statistics, libnx thread fixes and the guest memory window.
#include "core_switch.h"

#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "../runtime.h"
#include "debug_server.h"
#include "host.h"

// The log is a file on the SD card: a write can take milliseconds, and the thread that logs (render,
// audio, the game's main thread) would stall for it. core.cpp's log_msg hands every line to the sink
// below (under its log lock), which keeps it in a buffer that a writer thread writes out four times a
// second. fatal() and log_flush() write it at once.
static std::mutex g_pending_mutex;
static std::string g_log_pending;
static constexpr size_t kLogPendingMax = 4 << 20;  // dropped beyond this (a flood must not eat memory; a capture
                                                   // frame's per-draw trace is ~1.5 MB)

void log_flush() {
    std::string out;
    {
        std::lock_guard<std::mutex> lk(g_pending_mutex);
        out.swap(g_log_pending);
    }
    if (!out.empty()) {
        fwrite(out.data(), 1, out.size(), stderr);  // (logs/wwhd_<date>_<time>.log, main_switch.cpp)
        fflush(stderr);
        debugsrv::log_tap(out.data(), out.size());  // "log" streams of the debug server (when it runs)
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

static void log_sink(const char* line, size_t n) {
    {
        std::lock_guard<std::mutex> lk(g_pending_mutex);
        if (g_log_pending.size() + n + 1 <= kLogPendingMax) {
            g_log_pending.append(line, n);
            g_log_pending += '\n';
        }
    }
    log_writer_start();
}
// before the static initialisers that log (101 = the first priority for programs, like settings.ini's
// reader in main_switch.cpp, which logs nothing itself); without the sink a line would reach no file
__attribute__((constructor(101))) static void log_sink_install() { log_set_sink(log_sink); }

// From the CPU exception handler (main_switch.cpp): the pending lines and then text, written straight to the
// files' descriptors. Nothing waits for a lock: the thread that crashed may hold the log's or stdio's.
void log_crash_write(const char* text, size_t n) {
    std::string out;
    if (g_pending_mutex.try_lock()) {
        out.swap(g_log_pending);
        g_pending_mutex.unlock();
    }
    if (const int fd = fileno(stderr); fd >= 0) {
        if (!out.empty()) (void)!write(fd, out.data(), out.size());
        (void)!write(fd, text, n);
        fsync(fd);
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

// ---------------------------------------------------------------- threads
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

// ---------------------------------------------------------------- guest memory
// Horizon commits every mapping, so only the guest ranges the game and the runtime use are backed:
// heap pages are aliased into a reserved 4 GiB window (code-memory region, read/write). Everything the
// desktop builds can touch (all of it is mapped there) except the large gaps is backed.
namespace {
struct GuestRange { uint32_t start, size; };
constexpr GuestRange kGuestRanges[] = {
    {0x00010000, 0x04000000 - 0x00010000},  // low memory and .text / .rodata of cking.rpx
    {mem::kMem2Start, mem::kMem2End - mem::kMem2Start},
    {mem::kRuntimeStart, mem::kRuntimeEnd - mem::kRuntimeStart},
    {0xC0000000, 0x02100000},          // import slots, data imports, host function addresses
    {mem::kFgBucket, mem::kFgBucketSize},
    {mem::kMem1, mem::kMem1Size},
};
}  // namespace
void core_switch::init_memory() {
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
