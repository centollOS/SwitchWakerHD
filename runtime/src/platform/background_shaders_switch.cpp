// Background shaders (background_shaders_switch.h).
#include "background_shaders_switch.h"

#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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
void set_background_paused(bool on);
}  // namespace gfxdk
namespace render { uint64_t frame_count(); }

namespace background_shaders {
namespace {

constexpr double kTitleRecord = 40.0;  // s of the title screen recorded before the list is made (first start)
constexpr size_t kWaitingGentle = 48, kWaitingFast = 96;  // sources between this thread and the worker

std::atomic<bool> g_running{false}, g_card{false}, g_fast{false}, g_paused{false};
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

// ---- the game's frame rate first (later starts, or B on the card): while the game presents fewer frames than its
// usual rate (the best of the last 30 s, e.g. 30), the list and its background jobs wait until it is back for 5 s.
// Places where the game itself is busy (and new places, whose own shaders compile) keep their time.
struct Pace {
    double windowStart = 0, calmSince = 0;
    uint64_t windowFrames = 0;
    double rates[30] = {};  // frames/s of the last 30 one-second windows
    int next = 0;
    bool slow = false;
};
Pace g_pace;

// samples the frame rate (any call at least every second or so); true while the background must wait
// no rests, no waits: behind the loading card, or at the title screen (the game's main thread has little to do there)
bool unhurried() { return g_fast || mods::current_stage() == "sea_T"; }

bool game_needs_time() {
    if (unhurried()) {
        if (g_paused.exchange(false)) gfxdk::set_background_paused(false);
        return false;
    }
    Pace& p = g_pace;
    const double t = now();
    const uint64_t frames = render::frame_count();
    if (p.windowStart == 0) {
        p.windowStart = t;
        p.windowFrames = frames;
    } else if (t - p.windowStart >= 1.0) {
        const double rate = double(frames - p.windowFrames) / (t - p.windowStart);
        p.rates[p.next] = rate;
        p.next = (p.next + 1) % 30;
        p.windowStart = t;
        p.windowFrames = frames;
        double best = 0;
        for (double r : p.rates) best = std::max(best, r);
        // 29.6 of 30 (59.2 of 60): a lower threshold (29.1) let the list take 0.3-0.4 fps for good (measured)
        const bool below = rate < best * 0.987;
        if (below) {
            p.calmSince = 0;
            if (!p.slow) LOG("[bg-shaders] the game at %.1f of %.0f fps: the background waits", rate, best);
            p.slow = true;
        } else if (p.slow) {
            if (p.calmSince == 0) p.calmSince = t;
            if (t - p.calmSince >= 5.0) {
                p.slow = false;
                LOG("[bg-shaders] the game back at %.1f fps: the background goes on", rate);
            }
        }
    }
    if (g_paused.exchange(p.slow) != p.slow) gfxdk::set_background_paused(p.slow);
    return p.slow;
}
void wait_for_game() {
    while (game_needs_time()) sleep_s(0.25);
}

// ---- where a later start goes on (background_shaders/progress.txt): the list is made from the manifest's first
// `recorded` records (the file only grows), so it comes out the same, and its first `done` variants are skipped
struct Progress {
    size_t recorded = 0, total = 0, done = 0;
};
std::string progress_path() { return dir() + "/progress.txt"; }
Progress read_progress() {
    Progress p;
    if (FILE* f = fopen(progress_path().c_str(), "rb")) {
        if (fscanf(f, "%zu %zu %zu", &p.recorded, &p.total, &p.done) != 3) p = Progress{};
        fclose(f);
    }
    return p;
}
void write_progress(const Progress& p) {
    mkdir(dir().c_str(), 0777);
    const std::string tmp = progress_path() + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fprintf(f, "%zu %zu %zu\n", p.recorded, p.total, p.done);
        fclose(f);
        remove(progress_path().c_str());
        rename(tmp.c_str(), progress_path().c_str());
    }
}

// ---- the scan's result kept between starts (background_shaders/programs.bin, gzip): reading and inflating the game's
// archives is the costliest part, and a later start's gentle list may need several sessions; removed with done.txt
std::string programs_path() { return dir() + "/programs.bin"; }
constexpr char kProgramsMagic[4] = {'W', 'B', 'P', '1'};

void save_programs(const std::vector<shader_scan::Program>& programs) {
    if (programs.empty()) return;
    mkdir(dir().c_str(), 0777);
    const std::string tmp = programs_path() + ".tmp";
    gzFile f = gzopen(tmp.c_str(), "wb1");
    if (!f) return;
    bool ok = gzwrite(f, kProgramsMagic, 4) == 4;
    const uint32_t count = uint32_t(programs.size());
    ok = ok && gzwrite(f, &count, 4) == 4;
    for (const shader_scan::Program& p : programs) {
        if (!ok) break;
        const uint32_t head[3] = {uint32_t(p.vertex), uint32_t(p.words.size()), uint32_t(p.code.size())};
        ok = gzwrite(f, head, sizeof head) == int(sizeof head) &&
             gzwrite(f, p.words.data(), unsigned(p.words.size() * 4)) == int(p.words.size() * 4) &&
             gzwrite(f, p.code.data(), unsigned(p.code.size())) == int(p.code.size());
    }
    ok = gzclose(f) == Z_OK && ok;
    remove(programs_path().c_str());
    if (ok && rename(tmp.c_str(), programs_path().c_str()) == 0) LOG("[bg-shaders] %u programs kept in programs.bin", count);
    else remove(tmp.c_str());
}

std::vector<shader_scan::Program> load_programs() {
    std::vector<shader_scan::Program> out;
    gzFile f = gzopen(programs_path().c_str(), "rb");
    if (!f) return out;
    char magic[4];
    uint32_t count = 0;
    bool ok = gzread(f, magic, 4) == 4 && !memcmp(magic, kProgramsMagic, 4) && gzread(f, &count, 4) == 4 && count < 1'000'000;
    if (ok) out.resize(count);
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t head[3];
        ok = gzread(f, head, sizeof head) == int(sizeof head) && head[1] <= 64 && head[2] <= 0x40000;
        if (!ok) break;
        shader_scan::Program& p = out[i];
        p.vertex = head[0] != 0;
        p.words.resize(head[1]);
        p.code.resize(head[2]);
        ok = gzread(f, p.words.data(), head[1] * 4) == int(head[1] * 4) && gzread(f, p.code.data(), head[2]) == int(head[2]);
    }
    gzclose(f);
    if (!ok) {
        LOG("[bg-shaders] programs.bin cannot be read: the game's files are read again");
        out.clear();
    } else {
        LOG("[bg-shaders] %u programs from programs.bin", count);
    }
    return out;
}

// B on the loading card: play now (its own pad reader; a press once the card has been up 2 s)
void watch_card_button() {
    while (render::frame_count() < 30) sleep_s(0.2);  // (input is set up by then)
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
    // the game's memory and the renderer exist once frames are presented (this thread starts in main(), before both)
    while (render::frame_count() < 30) sleep_s(0.2);
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
    // without the card (B pressed, or a later start) the reading rests twice its time after each file, as the
    // translation does: the game's threads on core 0 keep their time
    // a later start reads the programs an earlier one kept (programs.bin), not the game's 1.4 GB again
    std::vector<shader_scan::Program> programs = firstStart ? std::vector<shader_scan::Program>{} : load_programs();
    if (programs.empty()) {
        programs = shader_scan::scan(config::game_dir, st, [](double seconds) {
            if (!unhurried()) {
                sleep_s(seconds * 2);
                wait_for_game();
            }
        });
        if (!g_card) save_programs(programs);  // (behind the card the list finishes in this start)
    }
    const double t3 = now();
    std::vector<shader_list::Recorded> recorded = shader_list::read_manifest("shader_manifest.bin");
    // a list left over by an earlier start: the same list again, from where it stopped
    Progress progress = firstStart ? Progress{} : read_progress();
    if (progress.recorded && progress.recorded <= recorded.size()) recorded.resize(progress.recorded);
    else progress = Progress{};
    const std::vector<shader_list::Variant> list = shader_list::speculate(programs, recorded, 2, [](double seconds) {
        if (!unhurried()) {  // as the reading: it rests twice its time (25 s of core 0 at once otherwise, measured)
            sleep_s(seconds * 2);
            wait_for_game();
        }
    });
    size_t first = 0;
    if (progress.total == list.size() && progress.done < list.size()) first = progress.done;
    else if (progress.recorded) LOG("[bg-shaders] progress.txt does not match the list (%zu variants, not %zu): from the start",
                                    list.size(), progress.total);
    progress = Progress{recorded.size(), list.size(), first};
    LOG("[bg-shaders] %zu programs read in %.1f s; %zu recorded variants -> %zu to translate, from %zu (%.1f s)",
        programs.size(), t3 - t2, recorded.size(), list.size(), first, now() - t3);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_cardState.total = list.size();
        g_cardState.done = first;
        g_cardState.phase = CardState::kCompiling;
        g_compileStart = now();
    }
    std::vector<uint32_t> regs(0x10000);
    size_t failed = 0;
    double lastLog = now(), lastSaved = now(), translateSeconds = 0;
    for (size_t k = first; k < list.size(); k++) {
        const shader_list::Variant& v = list[k];
        while (gfxdk::listed_waiting() + gfxdk::shaders_pending() > (g_fast ? kWaitingFast : kWaitingGentle)) {
            game_needs_time();
            svcSleepThread(50'000'000);
        }
        wait_for_game();
        // every 20 s, what is translated (its sources are in the worker's queue: a start that stops before they are
        // compiled finds them in shadercache_gl.bin, queued again)
        if (now() - lastSaved >= 20) {
            lastSaved = now();
            // (not in shadercache_gl.bin yet: the listed ones not taken, and a margin for the cache writer's last second)
            progress.done = k - std::min(k - first, gfxdk::listed_waiting() + kWaitingFast);
            write_progress(progress);
        }
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
                     list.size(), failed, translateSeconds * 1000 / double(k + 1 - first), now() - t0,
                     g_fast ? "fast" : "gentle");
            set_status(CardState::kCompiling, b);
            LOG("[bg-shaders] %s", b);
        }
        // gently: a third of core 0 at most (the game's threads there); fast: no rest
        if (!unhurried()) svcSleepThread(int64_t((now() - d0) * 2e9) + 1'000'000);
    }
    // the list is translated: what the worker has not compiled yet is in shadercache_gl.bin (the next start queues
    // it); done.txt says the list need not be made again. Behind the card, the card stays until the worker is done.
    while (g_card && gfxdk::listed_waiting() + gfxdk::shaders_pending() > 0) svcSleepThread(200'000'000);
    mkdir(dir().c_str(), 0777);
    if (FILE* f = fopen((dir() + "/done.txt").c_str(), "wb")) {
        fprintf(f, "%zu\n", list.size());
        fclose(f);
    }
    remove(progress_path().c_str());
    remove(programs_path().c_str());
    LOG("[bg-shaders] done: %zu translated, %zu failed, %.0f s", list.size(), failed, now() - t0);
    set_status(CardState::kFinished, "done");
    g_card = false;
    set_fast(false);
    if (g_paused.exchange(false)) gfxdk::set_background_paused(false);
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
bool paused() { return g_running && g_paused; }

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
