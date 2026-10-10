// Background shaders (background_shaders_switch.h).
#include "background_shaders_switch.h"

#include <switch.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../runtime.h"
#include "shader_list_switch.h"
#include "shader_scan_switch.h"

namespace gfxdk {  // gfx/deko/dk_shaders.h (without the decompiler's headers)
bool translate_listed(uint32_t* regs, bool vertex, const uint8_t* program, uint32_t programSize, uint64_t programHash,
                      const uint8_t* fetch, uint32_t fetchSize, bool fetchCompact, std::string& glsl, uint64_t& glslHash);
void queue_listed(uint64_t glslHash, bool vertex, std::string&& glsl);
size_t listed_waiting();
uint64_t shaders_pending();
}  // namespace gfxdk

namespace background_shaders {
namespace {

constexpr size_t kMaxWaiting = 48;  // sources between this thread and the worker (each holds its GLSL, ~10-40 KB)

std::atomic<bool> g_running{false};
std::mutex g_mu;
std::string g_status;

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = s;
}
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void run(void*) {
    const double t0 = now();
    set_status("reading the game's shader programs");
    shader_scan::Stats st;
    const std::vector<shader_scan::Program> programs = shader_scan::scan(config::game_dir, st);
    const double t1 = now();
    set_status("reading the shader manifest");
    const std::vector<shader_list::Recorded> recorded = shader_list::read_manifest("shader_manifest.bin");
    const std::vector<shader_list::Variant> list = shader_list::speculate(programs, recorded, 2);
    const double t2 = now();
    LOG("[bg-shaders] %zu programs read in %.1f s; %zu recorded variants -> %zu to translate (%.1f s)", programs.size(),
        t1 - t0, recorded.size(), list.size(), t2 - t1);
    std::vector<uint32_t> regs(0x10000);
    size_t done = 0, failed = 0, queued = 0;
    double lastLog = now(), decompileSeconds = 0;
    for (const shader_list::Variant& v : list) {
        while (gfxdk::listed_waiting() + gfxdk::shaders_pending() > kMaxWaiting) svcSleepThread(100'000'000);
        const shader_scan::Program& p = programs[v.program];
        std::fill(regs.begin(), regs.end(), 0);
        for (const auto& [i, value] : v.regs) regs[i] = value;
        std::string glsl;
        uint64_t hash = 0;
        const double d0 = now();
        const bool ok = gfxdk::translate_listed(regs.data(), p.vertex, p.code.data(), uint32_t(p.code.size()),
                                                shader_list::hash_bytes(p.code.data(), p.code.size()), v.fetch.data(),
                                                uint32_t(v.fetch.size()), v.fetchCompact, glsl, hash);
        decompileSeconds += now() - d0;
        done++;
        if (!ok) failed++;
        else {
            gfxdk::queue_listed(hash, p.vertex, std::move(glsl));
            queued++;
        }
        if (now() - lastLog > 30 || done == list.size()) {
            lastLog = now();
            char b[200];
            snprintf(b, sizeof b, "%zu of %zu translated (%zu failed), %.1f ms each, %.0f s so far", done, list.size(),
                     failed, done ? decompileSeconds * 1000 / done : 0.0, now() - t0);
            set_status(b);
            LOG("[bg-shaders] %s", b);
        }
        svcSleepThread(1'000'000);  // a breath for the game between variants
    }
    LOG("[bg-shaders] done: %zu translated, %zu queued (duplicates of what is compiled are dropped by the renderer), "
        "%zu failed, %.0f s", done, queued, failed, now() - t0);
    set_status("done");
    g_running = false;
}

Thread g_thread;

}  // namespace

std::string start() {
    if (g_running.exchange(true)) return "already running";
    // the lowest priority the app has (0x3B, as the shader worker), on core 0 (the game's main thread has core 1, the renderer core 2)
    Result rc = threadCreate(&g_thread, run, nullptr, nullptr, 4 * 1024 * 1024, 0x3B, 0);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        g_running = false;
        LOG("[bg-shaders] cannot start its thread: result 0x%x", unsigned(rc));
        return "cannot start its thread";
    }
    return "";
}

bool running() { return g_running; }

std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

}  // namespace background_shaders
