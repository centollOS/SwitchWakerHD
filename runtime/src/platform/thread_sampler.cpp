// thread_sampler.h: a sampling profiler for the frame threads (Windows on ARM64 only).
#include "thread_sampler.h"

#if defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace thread_sampler {
namespace {
constexpr int kMaxThreads = 8;
constexpr int kMaxFrames = 32;
// one sample on disk: the thread's tag index, the number of frames, then the frames (pc, lr, the
// return addresses of the frame-pointer chain)
struct Sample {
    uint32_t thread;
    uint32_t count;
    uint64_t frames[kMaxFrames];
};
struct Target {
    HANDLE handle;
    std::string tag;
};
std::mutex g_mu;
Target g_targets[kMaxThreads];
std::atomic<int> g_count{0};
std::string g_prefix;
double g_after = 0, g_seconds = 0;

bool readable(uint64_t p, uint64_t lo, uint64_t hi) { return p >= lo && p + 16 <= hi && (p & 7) == 0; }
// Return addresses saved on the stack carry a pointer-authentication code in their upper bits
// (ARMv8.3 PAC, used by Windows on Snapdragon X): user addresses fit in 48 bits
uint64_t strip_pac(uint64_t a) { return a & 0x0000FFFFFFFFFFFFull; }

// The suspended thread's frames, unwound with the modules' unwind data (RtlVirtualUnwind, as the
// crash log does): the Vulkan driver does not keep a frame-pointer chain everywhere. Where a pc has
// no unwind data (a leaf, or generated code) the walk continues through lr once, then falls back to
// the frame-pointer chain (x29 points at the saved {x29, x30} pair). No allocation happens here.
// (RtlLookupFunctionEntry reads the loader's function tables: a thread suspended while it changes
// them, at a module load, could stall the sampler; the frame threads do not load modules.)
uint32_t capture(HANDLE h, uint64_t* out) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(h, &c)) return 0;
    const uint64_t lo = c.Sp, hi = c.Sp + (1u << 20);  // the thread's stack, from its sp up
    uint32_t n = 0;
    bool leaf_used = false;
    while (n < kMaxFrames) {
        const uint64_t pc = strip_pac(c.Pc);
        if (!pc) break;
        out[n++] = pc;
        const uint64_t sp = c.Sp;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &image, nullptr);
        if (f) {
            PVOID handler_data = nullptr;
            DWORD64 frame = 0;
            c.Pc = pc;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, f, &c, &handler_data, &frame, nullptr);
        } else if (!leaf_used && n == 1) {
            leaf_used = true;
            c.Pc = c.Lr;
        } else {
            // the rest by the frame-pointer chain
            uint64_t fp = c.Fp;
            while (n < kMaxFrames && readable(fp, lo, hi)) {
                const uint64_t* rec = reinterpret_cast<const uint64_t*>(fp);
                const uint64_t next = rec[0], ret = strip_pac(rec[1]);
                if (!ret) break;
                out[n++] = ret;
                if (next <= fp) break;
                fp = next;
            }
            break;
        }
        if (c.Sp < sp || c.Sp >= hi) break;
    }
    return n;
}

void write_modules() {
    const std::string path = g_prefix + ".modules";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    HMODULE mods[1024];
    DWORD need = 0;
    HANDLE self = GetCurrentProcess();
    if (EnumProcessModules(self, mods, sizeof mods, &need)) {
        for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
            MODULEINFO mi{};
            char name[MAX_PATH] = {};
            if (!GetModuleInformation(self, mods[i], &mi, sizeof mi)) continue;
            GetModuleFileNameExA(self, mods[i], name, sizeof name);
            fprintf(f, "%llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, (unsigned long)mi.SizeOfImage, name);
        }
    }
    fclose(f);
}

void sampler_main() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (g_after > 0) Sleep((DWORD)(g_after * 1000));
    FILE* out = fopen((g_prefix + ".samples").c_str(), "wb");
    if (!out) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const int n = g_count.load();
        fprintf(out, "WWHDSAMP1 %d\n", n);
        for (int i = 0; i < n; i++) fprintf(out, "%s\n", g_targets[i].tag.c_str());
    }
    write_modules();
    const ULONGLONG start = GetTickCount64();
    ULONGLONG last_flush = start;
    std::vector<Sample> batch;
    batch.reserve(4096);
    for (;;) {
        Sleep(1);
        const int n = g_count.load();
        for (int i = 0; i < n; i++) {
            Sample s{};
            s.thread = (uint32_t)i;
            if (SuspendThread(g_targets[i].handle) == (DWORD)-1) continue;
            s.count = capture(g_targets[i].handle, s.frames);
            ResumeThread(g_targets[i].handle);
            if (s.count) batch.push_back(s);
        }
        const ULONGLONG now = GetTickCount64();
        if (now - last_flush >= 1000 || batch.size() >= 4000) {
            for (const Sample& s : batch) {
                fwrite(&s.thread, 4, 1, out);
                fwrite(&s.count, 4, 1, out);
                fwrite(s.frames, 8, s.count, out);
            }
            fflush(out);
            batch.clear();
            last_flush = now;
            if (now - start > 10000 && now - start < 11500) write_modules();  // modules loaded late (the driver's)
        }
        if (g_seconds > 0 && now - start >= (ULONGLONG)(g_seconds * 1000)) break;
    }
    fclose(out);
    write_modules();
}
}  // namespace

void add_current_thread(const char* tag) {
    static const char* env = getenv("WWHD_SAMPLE_THREADS");
    if (!env || !*env) return;
    std::lock_guard<std::mutex> lk(g_mu);
    const int i = g_count.load();
    if (i >= kMaxThreads) return;
    HANDLE h = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    g_targets[i] = {h, tag};
    g_count.store(i + 1);
    if (i == 0) {
        g_prefix = env;
        if (const char* a = getenv("WWHD_SAMPLE_AFTER")) g_after = atof(a);
        if (const char* s = getenv("WWHD_SAMPLE_SECONDS")) g_seconds = atof(s);
        std::thread(sampler_main).detach();
    }
}
}  // namespace thread_sampler
#else
namespace thread_sampler {
void add_current_thread(const char*) {}
}  // namespace thread_sampler
#endif
