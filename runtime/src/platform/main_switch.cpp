// SwitchWakerHD (this fork): the Switch parts of main.cpp, called from its marked hooks (main_switch.h):
// the CPU exception handler and crash report, the session logs, settings.ini read before the static
// initialisers, and the console's start-up steps.
#include "main_switch.h"

#include <switch.h>

#include "debug_switch.h"
#include "startup_checks_switch.h"
#include "prepare_graphics_switch.h"
#include "background_shaders_switch.h"
#include "settings_ini.h"
#include "settings_switch.h"
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <vector>
#include <atomic>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

#include "../gx2/gx2.h"
#include "../mods/manager.h"
#include "recomp_table.h"
#include "../runtime.h"
#include "host.h"

// CPU exceptions (a bad memory access, a jump to a bad address, abort's trap): the kernel sends them
// to the process's entry point, which is hbloader's; hbloader passes them to the NRO's entry point
// (nx-hbloader trampoline.s), and libnx's crt0 calls __libnx_exception_handler on the stack below.
// It writes into the session log (logs/wwhd_<date>_<time>.log) the log lines still waiting for the writer thread,
// what crashed and where, the registers, and the return addresses found on the stack. Code
// addresses are given as code+offset: `addr2line -f -C -e build/switch/wwhd.elf 0x<offset>` names
// the function (the ELF of the same build). When the handler returns, libnx raises svcBreak and
// Atmosphere writes its crash report as before.
void log_crash_write(const char* text, size_t n);  // core.cpp
extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern char* fake_heap_end;  // libnx: the heap malloc grows into (sbrk)
void __libnx_exception_handler(ThreadExceptionDump* ctx);
}
namespace render { uint64_t frame_count(); }
// what crashed and where, into the logs (CPU exception handler, abort): kind, pc/lr/sp/fp, the fault
// address, the registers when known, the frame-pointer chain and the code addresses on the stack
static void crash_report(const char* kind, uint64_t pc, uint64_t lr, uint64_t sp, uint64_t fp, uint64_t far,
                         const ThreadExceptionDump* ctx) {
    static std::atomic<int> entered{0};
    if (entered.fetch_add(1)) return;  // a second thread crashing meanwhile: the first one reports
    static char buf[16384];
    size_t n = 0;
    auto put = [&](const char* fmt, auto... a) {
        if (n >= sizeof buf - 1) return;
        int w = snprintf(buf + n, sizeof buf - n, fmt, a...);
        if (w > 0) n = std::min(sizeof buf - 1, n + size_t(w));
    };
    const uint64_t base = host::executable_base();
    uint64_t textEnd = base;
    {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, base))) textEnd = mi.addr + mi.size;
    }
    char names[3][40];
    auto where = [&](int slot, uint64_t a) -> const char* {
        if (a >= base && a < textEnd) snprintf(names[slot], sizeof names[slot], "code+0x%llx", (unsigned long long)(a - base));
        else snprintf(names[slot], sizeof names[slot], "0x%llx", (unsigned long long)a);
        return names[slot];
    };
    put("\n[crash] %s in thread \"%s\", frame %llu\n", kind, host::thread_label.c_str(),
        (unsigned long long)render::frame_count());
    put("[crash] pc %s, lr %s, sp 0x%llx, fault address 0x%llx\n", where(0, pc), where(1, lr), (unsigned long long)sp,
        (unsigned long long)far);
    const char* stage = gx2::g_render_stage.load(std::memory_order_relaxed);
    put("[crash] render thread: %s\n", stage ? stage : "waiting");
    if (Cpu* c = threads::current())
        put("[crash] guest thread %08x: lr %08x, r1 %08x, r3 %08x, r4 %08x\n", threads::current_thread(), c->lr, c->r[1],
            c->r[3], c->r[4]);
    if (ctx) {
        for (int i = 0; i < 29; i += 4) {
            put("[crash]");
            for (int j = i; j < i + 4 && j < 29; j++) put(" x%d %llx", j, (unsigned long long)ctx->cpu_gprs[j].x);
            put("\n");
        }
    }
    put("[crash] fp %llx\n", (unsigned long long)fp);
    // the stack the thread was on: frame-pointer chain, then every word that is a code address
    MemoryInfo st{};
    u32 page = 0;
    if (R_SUCCEEDED(svcQueryMemory(&st, &page, sp)) && (st.perm & Perm_R)) {
        const uint64_t lo = sp, hi = st.addr + st.size;
        put("[crash] frames:");
        for (int i = 0; i < 32 && fp >= lo && fp + 16 <= hi && !(fp & 7); i++) {
            const uint64_t* f = reinterpret_cast<const uint64_t*>(fp);
            put(" %s", where(2, f[1]));
            if (f[0] <= fp) break;
            fp = f[0];
        }
        put("\n[crash] code addresses on the stack:");
        int found = 0;
        for (uint64_t a = lo; a + 8 <= hi && a < lo + 0x10000 && found < 48; a += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t*>(a);
            if (v >= base && v < textEnd) {
                put(" %s", where(2, v));
                found++;
            }
        }
        put("\n");
    }
    char* top = static_cast<char*>(sbrk(0));
    if (fake_heap_end && top && top != reinterpret_cast<char*>(-1))
        put("[crash] heap never used: %llu MiB\n", (unsigned long long)((fake_heap_end - top) >> 20));
    put("[crash] build %s %s; symbols: addr2line -f -C -e wwhd.elf <offset> with the ELF of this build\n", __DATE__, __TIME__);
    log_crash_write(buf, n);
}
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const uint32_t ec = ctx->esr >> 26;
    const char* what = ec == 0x24 || ec == 0x25 ? "data abort (bad memory access)"
                       : ec == 0x20 || ec == 0x21 ? "instruction abort (jump to a bad address)"
                       : ec == 0x22 ? "misaligned pc" : ec == 0x26 ? "misaligned stack pointer"
                       : ec == 0x3C ? "breakpoint (a trap instruction)"
                       : ec == 0x00 ? "undefined instruction" : "other";
    char kind[128];
    snprintf(kind, sizeof kind, "CPU exception: %s (desc 0x%x, esr 0x%08x)", what, ctx->error_desc, ctx->esr);
    crash_report(kind, ctx->pc.x, ctx->lr.x, ctx->sp.x, ctx->fp.x, ctx->far.x, ctx);
}
// abort() from anywhere in the NRO (Mesa included: -Wl,--wrap=abort) raises svcBreak, which the exception
// handler does not see: the caller goes into the log first
extern "C" void __real_abort(void);
extern "C" void __wrap_abort(void) {
    crash_report("abort() called", reinterpret_cast<uint64_t>(__builtin_return_address(0)), 0,
                 reinterpret_cast<uint64_t>(__builtin_frame_address(0)), reinterpret_cast<uint64_t>(__builtin_frame_address(0)),
                 0, nullptr);
    __real_abort();
    for (;;) {}
}
// Uncaught C++ exceptions are logged here.
void main_switch::install_crash_handler() {
    std::set_terminate([] {
        log_flush();
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
            fprintf(stderr, "FATAL: std::terminate\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "FATAL: uncaught exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "FATAL: uncaught exception\n");
        }
        fflush(stderr);
        abort();
    });
}
// Session logs: each session writes only logs/wwhd_<date>_<time>.log (the console clock at startup), so
// the newest file in logs/ is always the current session. logs/ keeps the kMaxSessionLogs most recent
// session logs, the current one included; older ones are deleted by age (modification time, then name).
// A wwhd.log left next to the .nro by a build before this is moved into logs/ (or deleted when its
// session file is already there: it held the same lines).
static void start_session_logs(const struct tm& t, char* name, size_t size) {
    constexpr size_t kMaxSessionLogs = 10;
    mkdir("logs", 0777);
    if (FILE* f = fopen("wwhd.log", "r")) {
        char first[128] = {};
        if (!fgets(first, sizeof first, f)) first[0] = 0;
        fclose(f);
        int y, mo, d, h, mi, se;
        char old[96];
        if (sscanf(first, "[session] %d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) == 6)
            snprintf(old, sizeof old, "logs/wwhd_%04d-%02d-%02d_%02d-%02d-%02d.log", y, mo, d, h, mi, se);
        else
            snprintf(old, sizeof old, "logs/wwhd_earlier_build_%ld.log", (long)time(nullptr));
        struct stat st;
        if (stat(old, &st) != 0) rename("wwhd.log", old);
        else remove("wwhd.log");
    }
    snprintf(name, size, "logs/wwhd_%04d-%02d-%02d_%02d-%02d-%02d.log", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    // room for this session's file: the oldest go
    struct Log {
        std::string name;
        time_t mtime;
    };
    std::vector<Log> logs;
    if (DIR* dir = opendir("logs")) {
        while (dirent* e = readdir(dir)) {
            if (strncmp(e->d_name, "wwhd_", 5) != 0) continue;
            const std::string path = std::string("logs/") + e->d_name;
            if (path == name) continue;  // (a restart within the same second: reopened below)
            struct stat st;
            logs.push_back({e->d_name, stat(path.c_str(), &st) == 0 ? st.st_mtime : 0});
        }
        closedir(dir);
    }
    std::sort(logs.begin(), logs.end(), [](const Log& a, const Log& b) {
        return a.mtime != b.mtime ? a.mtime < b.mtime : a.name < b.name;
    });
    for (size_t i = 0; i + kMaxSessionLogs <= logs.size(); i++) remove(("logs/" + logs[i].name).c_str());
}
// settings.ini (platform/settings_ini.h) is read before any static initialiser runs: its [dev] section's
// KEY=VALUE lines go into the environment, and so do the menu's settings that code reads at static
// initialisation (the main thread's sampler, threads.cpp). Many switches are read into globals then (draw.cpp's
// switches, the AO mode, the gameplay mods...). libnx mounts the SD card (__appInit) before __libc_init_array
// runs the constructors, and this one runs first (priority 101: switch.ld sorts .init_array by priority).
// env.txt, the file of variables before settings.ini had a [dev] section, is converted once and renamed
// env.txt.old. Nothing can be logged yet: the messages wait for main() (log_messages).
namespace early_settings {
constexpr const char* kDir = "sdmc:/switch/wwhd";
// (function statics: this runs before the globals of this file are initialised)
std::vector<std::string>& messages() {
    static std::vector<std::string> v;
    return v;
}
std::vector<std::pair<std::string, bool>>& migrated_mods() {
    static std::vector<std::pair<std::string, bool>> v;
    return v;
}
bool read_text(const std::string& path, std::string& text) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    return true;
}
void migrate_env_txt(settings_ini::File& ini) {
    const std::string dir = kDir, env = dir + "/env.txt", old = env + ".old", ini_path = dir + "/settings.ini";
    std::string text;
    if (!read_text(env, text)) return;
    const settings_ini::Migration m = settings_ini::migrate_env_txt(text, ini);
    migrated_mods() = m.mods;
    const std::string out = settings_ini::format(ini), tmp = ini_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    const bool written = f && fwrite(out.data(), 1, out.size(), f) == out.size();
    if (f && fclose(f) != 0) f = nullptr;
    if (!written || !f || !host::replace_file(tmp, ini_path)) {
        messages().push_back("[settings] env.txt: cannot write settings.ini: its variables apply to this session "
                             "only, and env.txt is converted again at the next start");
        return;
    }
    for (const std::string& line : m.log) messages().push_back("[settings] env.txt: " + line);
    remove(old.c_str());
    const bool renamed = rename(env.c_str(), old.c_str()) == 0;
    messages().push_back(std::string("[settings] env.txt converted into settings.ini (") +
                         std::to_string(m.log.size()) + " variables); " +
                         (renamed ? "renamed env.txt.old" : "could not rename it: delete env.txt by hand"));
}
__attribute__((constructor(101))) void read_at_start() {
    std::string text;
    read_text(std::string(kDir) + "/settings.ini", text);
    settings_ini::File ini = settings_ini::parse(text);
    migrate_env_txt(ini);
    for (const auto& [name, value] : settings_ini::dev_variables(ini)) {
        if (settings_ini::menu_backed(name)) {
            messages().push_back("[settings] [dev] " + name + " ignored: the settings menu has it");
            continue;
        }
        setenv(name.c_str(), value.c_str(), 1);
        messages().push_back("[settings] [dev] " + name + "=" + value);
    }
    if (auto it = ini.menu.find(switch_settings::kKeyMainSampler); it != ini.menu.end() && it->second == "1")
        setenv("WWHD_MAIN_SAMPLER", "1", 1);
}
void log_messages() {
    for (const std::string& line : messages()) LOG("%s", line.c_str());
    messages().clear();
}
// the mods env.txt turned on or off, through the mod manager (its own profile too), once it is ready; and a
// check that settings_ini's list of mod variables is the manager's
void apply_migrated_mods() {
    for (const auto& entry : mods::manager::entries()) {
        bool known = false;
        for (const settings_ini::ModVariable& m : settings_ini::kModVariables)
            known |= std::string(m.id) == entry.id && std::string(m.env) == entry.startup_env;
        if (!known) LOG("[settings] mod %s (%s) is missing from settings_ini::kModVariables", entry.id, entry.startup_env);
    }
    for (const auto& [id, on] : migrated_mods()) mods::manager::set_enabled(id, on);
    migrated_mods().clear();
}
}  // namespace early_settings

// first thing in main(): working folder, session log, debug server, boot lines, saved settings
void main_switch::start() {
    // everything lives in sdmc:/switch/wwhd: game/ (extracted dump), save/, shader cache, logs/
    mkdir(host::config_dir().c_str(), 0777);
    chdir(host::config_dir().c_str());
    const time_t now = time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    char logName[96];
    start_session_logs(t, logName, sizeof logName);
    if (!freopen(logName, "w", stderr)) freopen("wwhd.log", "w", stderr);  // (logs/ not writable)
    setvbuf(stderr, nullptr, _IOLBF, 0);
    // the debug server (Switch tab > Debug, saved): first, so its log text has the whole session
    debug_switch::start();  // (platform/debug_switch.h)
    LOG("[session] %04d-%02d-%02d %02d:%02d:%02d (build %s %s); log %s (the newest 10 sessions are kept in logs/)",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, __DATE__, __TIME__, logName);
    // which round of docs/switch-port.md this runtime is (to tell builds apart in the logs)
    LOG("[boot] recompiled code: %s; runtime: round 48 (upstream v0.2.11 sync: European game support; round 47 = vertex shaders with an input no fetch attribute fills translated with the GPU's multiplication: the post office letters; round 46 = upstream v0.2.8 sync, portable save states, rumble; round 45 = fast detiling, translation records, register writes merged in display lists, 2-way shader combinations; round 44 = round 43's helper-thread copies and render priority removed; register writes dispatched directly; round 42 = shader hot data packed and prefetched, one register-class pass; round 41 = context loads copy only written registers, targets/fixed state/viewport skipped by register generations, submit every 1024 draws, per-core load line; round 40 = deko3d: depth-only draws without their pixel shader, shared-surface texture lookups cached, vertex layouts kept, profiler off; round 39 = session log only in logs/, the newest 10 kept; round 38 = deko3d only: the OpenGL renderer removed; round 37 = CPU clock in the system table's steps, default 1224 MHz; settings menu on a Minus press; round 36 = shader budget: new shaders over frames, their draws skipped; round 35 = texture uploads from client memory again; round 34 = no framebuffer status query; round 33 = Warp tab; round 32 = queued texture uploads, texture error check after the GL thread finish; round 31 = CPU 1785 / GPU 614 options; round 30 = settings overlay on Minus; round 29 = official GPU profile 460.8 MHz handheld; round 28 = round 27 with the near-plane clip distance off, searchlight probe frames after a capture, per-draw trace in captures)",
        g_recomp_variant);
    early_settings::log_messages();  // settings.ini's [dev] section, env.txt converted
    host::place_thread(0);
    LOG("[boot] code at %p (for crash reports)", (void*)host::executable_base());
    switch_settings::apply_at_start();  // GPU profile, saved picture options (platform/settings_switch.h)
}

void main_switch::choose_dirs() {
    // the console has no arguments: settings.ini's [dev] section can choose the game and save folders
    // (e.g. another region's game next to the usual one; WWHD_STATE_DIR does the same for save states)
    if (const char* e = getenv("WWHD_GAME_DIR"); e && *e) config::game_dir = e;
    if (const char* e = getenv("WWHD_SAVE_DIR"); e && *e) config::save_dir = e;
}

void main_switch::after_boot_line() {
    // the game's files, else an error on screen and the app closes; the first start's shader warning
    startup_checks::game_files(config::game_dir);
    prepare_graphics::startup();  // a cut "Prepare graphics" sweep: the Quest Log files put back
    // the first start prepares the graphics behind a loading card; later starts finish a list left over
    if (startup_checks::first_start()) background_shaders::first_start();
    else background_shaders::later_start();
}

void main_switch::after_mods() { early_settings::apply_migrated_mods(); }
