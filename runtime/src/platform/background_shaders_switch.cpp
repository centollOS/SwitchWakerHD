// Background shaders (background_shaders_switch.h).
#include "background_shaders_switch.h"

#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../mods/mods.h"
#include "../runtime.h"
#include "host.h"
#include "shader_list_switch.h"
#include "shader_scan_switch.h"

namespace gfxdk {  // gfx/deko/dk_shaders.h (without the decompiler's headers)
bool translate_listed(uint32_t* regs, bool vertex, const uint8_t* program, uint32_t programSize, uint64_t programHash,
                      const uint8_t* fetch, uint32_t fetchSize, bool fetchCompact, std::string& glsl, uint64_t& glslHash);
void queue_listed(uint64_t glslHash, bool vertex, std::string&& glsl);
size_t listed_waiting();
uint64_t shaders_pending();
void set_background_fast(bool on);
}  // namespace gfxdk

namespace background_shaders {
namespace {

constexpr double kTitleRecord = 40.0;  // s of the title screen recorded before the list is made (first start)
constexpr size_t kWaitingGentle = 48, kWaitingFast = 96;  // sources between this thread and the worker

std::atomic<bool> g_running{false}, g_card{false}, g_fast{false};
std::mutex g_mu;
std::string g_status;
CardState g_cardState;  // under g_mu (done, total, phase)
double g_compileStart = 0;  // under g_mu: when the list's translation began (the minutes left)

std::string dir() { return host::config_dir() + "/background_shaders"; }
double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleep_s(double s) { svcSleepThread(int64_t(s * 1e9)); }

void set_status(int phase, const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_cardState.phase = phase;
    g_status = s;
}

void set_fast(bool on) {
    g_fast = on;
    gfxdk::set_background_fast(on);
}

// B on the loading card: play now (its own pad reader; a press once the card has been up 2 s)
void watch_card_button() {
    PadState pad;
    padInitializeDefault(&pad);
    const double t0 = now();
    while (g_card) {
        padUpdate(&pad);
        if (now() - t0 > 2.0 && (padGetButtonsDown(&pad) & HidNpadButton_B)) {
            LOG("[bg-shaders] B on the loading card: play now, the rest goes on gently");
            g_card = false;
            set_fast(false);
            break;
        }
        svcSleepThread(50'000'000);
    }
}

void run(bool firstStart) {
    const double t0 = now();
    if (firstStart) {
        // the title screen's draws record their states into the manifest (and compile the title's own shaders)
        set_status(CardState::kStarting, "waiting for the title screen");
        while (mods::current_stage() != "sea_T" && now() - t0 < 180) sleep_s(0.5);
        const double t1 = now();
        while (now() - t1 < kTitleRecord) sleep_s(0.5);
        sleep_s(2.5);  // the manifest's writer appends every 2 s
    }
    set_status(CardState::kReading, "reading the game's shader programs");
    const double t2 = now();
    shader_scan::Stats st;
    const std::vector<shader_scan::Program> programs = shader_scan::scan(config::game_dir, st);
    const double t3 = now();
    const std::vector<shader_list::Recorded> recorded = shader_list::read_manifest("shader_manifest.bin");
    const std::vector<shader_list::Variant> list = shader_list::speculate(programs, recorded, 2);
    LOG("[bg-shaders] %zu programs read in %.1f s; %zu recorded variants -> %zu to translate (%.1f s)", programs.size(),
        t3 - t2, recorded.size(), list.size(), now() - t3);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cardState.total = list.size();
        g_cardState.done = 0;
        g_cardState.phase = CardState::kCompiling;
        g_compileStart = now();
    }
    std::vector<uint32_t> regs(0x10000);
    size_t failed = 0;
    double lastLog = now(), translateSeconds = 0;
    for (size_t k = 0; k < list.size(); k++) {
        const shader_list::Variant& v = list[k];
        while (gfxdk::listed_waiting() + gfxdk::shaders_pending() > (g_fast ? kWaitingFast : kWaitingGentle))
            svcSleepThread(50'000'000);
        const shader_scan::Program& p = programs[v.program];
        std::fill(regs.begin(), regs.end(), 0);
        for (const auto& [i, value] : v.regs) regs[i] = value;
        std::string glsl;
        uint64_t hash = 0;
        const double d0 = now();
        const bool ok = gfxdk::translate_listed(regs.data(), p.vertex, p.code.data(), uint32_t(p.code.size()),
                                                shader_list::hash_bytes(p.code.data(), p.code.size()), v.fetch.data(),
                                                uint32_t(v.fetch.size()), v.fetchCompact, glsl, hash);
        translateSeconds += now() - d0;
        if (!ok) failed++;
        else gfxdk::queue_listed(hash, p.vertex, std::move(glsl));
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cardState.done = k + 1;
        }
        if (now() - lastLog > 30 || k + 1 == list.size()) {
            lastLog = now();
            char b[200];
            snprintf(b, sizeof b, "%zu of %zu translated (%zu failed), %.1f ms each, %.0f s so far (%s)", k + 1,
                     list.size(), failed, translateSeconds * 1000 / double(k + 1), now() - t0,
                     g_fast ? "fast" : "gentle");
            set_status(CardState::kCompiling, b);
            LOG("[bg-shaders] %s", b);
        }
        // gently: a third of core 0 at most (the game's threads there); fast: no rest
        if (!g_fast) svcSleepThread(int64_t((now() - d0) * 2e9) + 1'000'000);
    }
    // the list is translated: what the worker has not compiled yet is in shadercache_gl.bin (the next start queues
    // it); done.txt says the list need not be made again. Behind the card, the card stays until the worker is done.
    while (g_card && gfxdk::listed_waiting() + gfxdk::shaders_pending() > 0) svcSleepThread(200'000'000);
    mkdir(dir().c_str(), 0777);
    if (FILE* f = fopen((dir() + "/done.txt").c_str(), "wb")) {
        fprintf(f, "%zu\n", list.size());
        fclose(f);
    }
    LOG("[bg-shaders] done: %zu translated, %zu failed, %.0f s", list.size(), failed, now() - t0);
    set_status(CardState::kFinished, "done");
    g_card = false;
    set_fast(false);
    g_running = false;
}

Thread g_thread;
bool g_firstStart = false;
void thread_main(void*) { run(g_firstStart); }

std::string start_thread(bool firstStart) {
    if (g_running.exchange(true)) return "already running";
    g_firstStart = firstStart;
    // the lowest priority the app has (0x3B, as the shader worker), on core 0 (the game's main thread has core 1, the
    // renderer core 2); 4 MiB of stack for the decompiler
    Result rc = threadCreate(&g_thread, thread_main, nullptr, nullptr, 4 * 1024 * 1024, 0x3B, 0);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        g_running = false;
        LOG("[bg-shaders] cannot start its thread: result 0x%x", unsigned(rc));
        return "cannot start its thread";
    }
    return "";
}

}  // namespace

void first_start() {
    LOG("[bg-shaders] first start: the loading card, the title screen recorded behind it, then the list at full speed");
    g_card = true;
    set_fast(true);
    std::thread(watch_card_button).detach();
    start_thread(true);
}

void later_start() {
    if (access((dir() + "/done.txt").c_str(), F_OK) == 0) return;  // the list was made and translated once
    if (access("shader_manifest.bin", F_OK) != 0) return;
    LOG("[bg-shaders] an unfinished list: made again and translated gently in the background");
    start_thread(false);
}

std::string start() { return start_thread(false); }
bool running() { return g_running; }

std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

CardState card() {
    std::lock_guard<std::mutex> lk(g_mu);
    CardState c = g_cardState;
    c.up = g_card;
    if (c.phase == CardState::kCompiling && c.done > 50) {
        const double rate = double(c.done) / (now() - g_compileStart);
        c.minutesLeft = int(double(c.total - c.done) / rate / 60 + 0.5);
    }
    return c;
}

}  // namespace background_shaders
