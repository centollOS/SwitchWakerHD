// Wind Waker HD recompiled: entry point.
#if !defined(_WIN32) && !defined(__SWITCH__)
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif
#include <ctime>
#include <filesystem>
#include "guest_addr.h"
#include "mods/guest_mods.h"
#include "mods/code_mods.h"
#include "platform/host.h"
#ifdef _WIN32
#include <timeapi.h>
#endif

#include <cstring>
#include <string>
#include <thread>
#ifdef __SWITCH__
#include "platform/debug_switch.h"
#include "platform/startup_checks_switch.h"
#include "platform/settings_ini.h"
#include "platform/settings_switch.h"
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <algorithm>

#include <cstdlib>
#include <exception>
#include <vector>
#include <atomic>
#include <unistd.h>
#endif

#include "gfx/renderer.h"
#include "mods/cemu_pack.h"
#include "mods/content.h"
#include "gx2/gx2.h"
#include "recomp_table.h"
#include "report_header.h"
#include "crash_addr.h"
#include "build_info.h"
#include "crashrec.h"
#include "crash_context.h"
#include "input.h"
#include "mods/manager.h"
#include "mods/packages.h"
#include "runtime.h"
#include "mods/mods.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, called by SDLActivity
namespace interp { void set_mode(int); }
#endif

#ifdef WWHD_HAS_VULKAN
namespace gfxvk { int renderer_smoke_test(); }
#endif
#ifdef WWHD_HAS_METAL
int gfx_headstart_warm();  // gfx/shader_headstart.mm
#endif

void mem_setup_heaps(uint32_t data_end);
void trace_dump(FILE* f, unsigned last);
void mem_init_data_imports(uint32_t alloc_slot, uint32_t alloc_ex_slot, uint32_t free_slot);

#if defined(__SWITCH__)
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
static void install_crash_handler() {
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

#elif !defined(_WIN32)
// Memory crashes (SIGSEGV/SIGBUS/...): the report goes to the terminal and to
// captures/crash-<time>.log (registers, guest return chain, host backtrace, crash recovery's
// automatic state, the last log lines). Only write() and preformatted text after the crash.
static int g_crash_fd = -1;
static void crash_raw(int fd, const char* s, size_t n) {
    if (write(2, s, n) < 0) {}
    if (fd >= 0 && write(fd, s, n) < 0) {}
}
static void crash_log_raw(int fd, const char* s, size_t n) {
    if (fd >= 0 && write(fd, s, n) < 0) {}
}
static void crash_out(int fd, const char* s, size_t n) { crash_context::redact(fd, {s,n}, crash_raw); }
static void crash_log_only(int fd, const char* s, size_t n) { crash_context::redact(fd, {s,n}, crash_log_raw); }
static void crash_handler(int sig, siginfo_t* si, void* uctx) {
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)PPC_MEM_BASE;
    char path[96];
    {
        mkdir("captures", 0755);
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(path, sizeof path, "captures/crash-%Y%m%d-%H%M%S.log", &tmv);
        g_crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    const int fd = g_crash_fd;
    char buf[256];
    int n;
    if (a >= base && a < base + 0x100000000ull)
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at guest address %08X\n", sig, (unsigned)(a - base));
    else
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at host address %p\n", sig, si->si_addr);
    crash_out(fd, buf, n);
    // the faulting instruction and the module holding it (a driver, an overlay's layer, the game)
    char where[384], mpath[512] = "", line[1024];
    if (uintptr_t pc = crash_addr::context_pc(uctx)) {
        crash_addr::describe(where, sizeof where, pc, mpath, sizeof mpath);
        n = crash_addr::fit(snprintf(line, sizeof line, "  host pc %p%s\n", (void*)pc, where), sizeof line);
        crash_out(fd, line, n);
        if (mpath[0]) {
            n = crash_addr::fit(snprintf(line, sizeof line, "  module: %s\n", mpath), sizeof line);
            crash_out(fd, line, n);
        }
    }
    // a host fault address inside a module (a write to read-only data, a jump into a data section)
    if (!(a >= base && a < base + 0x100000000ull) && crash_addr::describe(where, sizeof where, a)) {
        n = crash_addr::fit(snprintf(line, sizeof line, "  fault address %p%s\n", si->si_addr, where), sizeof line);
        crash_out(fd, line, n);
    }
    Cpu* c = threads::current();
    if (c) {
        n = snprintf(buf, sizeof buf, "  guest lr=%08X ctr=%08X cr=%08X\n", c->lr, c->ctr, ppc_mfcr(c));
        crash_out(fd, buf, n);
        for (int i = 0; i < 32; i += 8) {
            n = snprintf(buf, sizeof buf, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1],
                         c->r[i + 2], c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
            crash_out(fd, buf, n);
        }
        // guest return chain (back-chain words on the guest stack; names: build/names.tsv)
        crash_out(fd, "  guest call chain:", 19);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 24 && sp >= 0x10000000u && sp < 0xF0000000u; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp || prev - sp > 0x100000u) break;
            n = snprintf(buf, sizeof buf, " %08X", ld32(prev + 4));
            crash_out(fd, buf, n);
            sp = prev;
        }
        crash_out(fd, "\n", 1);
    }
    crash_addr::host_backtrace(fd, crash_out, uctx);
    crash_context::note(fd, crash_out);
    crashrec::crash_note(fd, crash_out);
    if (fd >= 0) {
        crash_log_only(fd, "\n--- last log lines ---\n", 24);
        log_ring_write(fd, crash_log_only);
        close(fd);
        n = snprintf(buf, sizeof buf, "[crash] wrote %s\n", path);
        crash_out(-1, buf, n);
    }
    if (g_ppc_trace) {
        FILE* f = fopen("trace_dump.txt", "w");
        if (f) { trace_dump(f, 3000); fclose(f); if (write(2, "[trace] wrote trace_dump.txt\n", 29) < 0) {} }
    }
    input::stop_rumble_now();  // controllers keep their last motor level after the process (issue #35)
    _exit(128 + sig);
}

static void install_crash_handler() {
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    crash_addr::prime();
}

#else
static void win_crash_raw(int fd, const char* s, size_t n) {
    fwrite(s, 1, n, stderr);
    if (fd >= 0) _write(fd, s, (unsigned)n);
}
static void win_crash_log_raw(int fd, const char* s, size_t n) { if (fd >= 0) _write(fd, s, (unsigned)n); }
static void win_crash_out(int fd, const char* s, size_t n) { crash_context::redact(fd, {s,n}, win_crash_raw); }
static void win_crash_log_only(int fd, const char* s, size_t n) { crash_context::redact(fd, {s,n}, win_crash_log_raw); }
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ex) {
    auto code=ex->ExceptionRecord->ExceptionCode;
    std::error_code ec; std::filesystem::create_directories("captures",ec);
    char path[96]; time_t t=time(nullptr); struct tm tmv; localtime_s(&tmv,&t);
    strftime(path,sizeof path,"captures/crash-%Y%m%d-%H%M%S.log",&tmv);
    int fd=_open(path,_O_WRONLY|_O_CREAT|_O_TRUNC|_O_BINARY,_S_IREAD|_S_IWRITE);
    char buf[256]; int n;
    // the module holding the faulting instruction (issue #41: a driver or an overlay's Vulkan layer)
    char where[384], mpath[512]="", line[1024];
    using crash_addr::fit;
    crash_addr::describe(where,sizeof where,(uintptr_t)ex->ExceptionRecord->ExceptionAddress,mpath,sizeof mpath);
    n=fit(snprintf(line,sizeof line,"CRASH: Windows exception %08lX at %p%s\n",code,ex->ExceptionRecord->ExceptionAddress,where),sizeof line); win_crash_out(fd,line,n);
    if(mpath[0]){n=fit(snprintf(line,sizeof line,"  module: %s\n",mpath),sizeof line); win_crash_out(fd,line,n);}
    // access violations (and in-page errors): read / write / execute, and of which address
    const EXCEPTION_RECORD* er=ex->ExceptionRecord;
    if((code==EXCEPTION_ACCESS_VIOLATION||code==EXCEPTION_IN_PAGE_ERROR)&&er->NumberParameters>=2){
        const ULONG_PTR kind=er->ExceptionInformation[0], target=er->ExceptionInformation[1];
        const char* what=kind==0?"read":kind==1?"write":kind==8?"execute (DEP)":"access";
        const uintptr_t gbase=(uintptr_t)PPC_MEM_BASE;
        n=fit(snprintf(line,sizeof line,"  %s: %s of address %p",code==EXCEPTION_ACCESS_VIOLATION?"access violation":"in-page error",
                       what,(void*)target),sizeof line);
        if(target>=gbase&&target<gbase+0x100000000ull) n+=fit(snprintf(line+n,sizeof line-n," (guest address %08X)",(unsigned)(target-gbase)),sizeof line-n);
        else if(crash_addr::describe(where,sizeof where,target)) n+=fit(snprintf(line+n,sizeof line-n,"%s",where),sizeof line-n);
        if(n>(int)sizeof line-2) n=(int)sizeof line-2;
        line[n++]='\n'; win_crash_out(fd,line,n);
    }
    if(Cpu* c=threads::current()){n=snprintf(buf,sizeof buf,"guest lr=%08X ctr=%08X\n",c->lr,c->ctr); win_crash_out(fd,buf,n);}
    crash_addr::host_backtrace(fd,win_crash_out,ex->ContextRecord);
    crash_context::note(fd,win_crash_out);
    crashrec::crash_note(fd,win_crash_out);
    if(fd>=0){win_crash_log_only(fd,"\n--- last log lines ---\n",24); log_ring_write(fd,win_crash_log_only); _close(fd); fprintf(stderr,"[crash] wrote %s\n",path);}
    if(g_ppc_trace) { FILE* f=fopen("trace_dump.txt","w"); if(f){trace_dump(f,3000);fclose(f);} }
    input::stop_rumble_now();  // controllers keep their last motor level after the process (issue #35)
    return EXCEPTION_EXECUTE_HANDLER;
}
static void install_crash_handler() { SetUnhandledExceptionFilter(crash_handler); crash_addr::prime(); }
#endif
static void init_data_imports() {
    uint32_t alloc = 0, alloc_ex = 0, free_ = 0;
    for (unsigned i = 0; i < g_recomp_import_count; i++) {
        const RecompImport& im = g_recomp_imports[i];
        if (im.is_func) continue;
        std::string n = im.name;
        if (n == "MEMAllocFromDefaultHeap") alloc = im.addr;
        else if (n == "MEMAllocFromDefaultHeapEx") alloc_ex = im.addr;
        else if (n == "MEMFreeToDefaultHeap") free_ = im.addr;
        else if (n == "__gh_FOPEN_MAX") st32(im.addr, 20);
        else if (n == "environ") st32(im.addr, im.addr + 0x10);  // empty environment list
    }
    mem_init_data_imports(alloc, alloc_ex, free_);
}

// Portable mode (portable.txt next to the executable, see host::portable_user_dir): the macOS paths
// that do not go through host::config_dir() get their existing overrides pointed into the folder.
static void apply_portable_mode() {
    if (!host::portable()) return;
    const std::string u = host::portable_user_dir();
    std::error_code ec;
    std::filesystem::create_directories(u, ec);
    auto set = [](const char* k, const std::string& v) {
        if (getenv(k)) return;  // an explicit override wins
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        setenv(k, v.c_str(), 0);
#endif
    };
    set("WWHD_STATE_DIR", u + "/states");
#ifdef __APPLE__
    set("WWHD_DISPLAY_SETTINGS", u + "/display.plist");
    set("WWHD_SHADER_CACHE", u + "/shaders.bin");
#endif
}

// The Vulkan renderer's validated opt-in CPU paths (docs/vulkan.md, "Opt-in CPU experiments"), on by
// default on every platform: on a Galaxy S25 Ultra they took the render thread from about 40 to
// 30 ms per frame; on an M3 Max (MoltenVK) together they cut render-thread CPU by 8-14% with no
// measurable cost from any single one (docs/performance.md, 2026-10-07). NAME=0 turns one off.
// Not on the Switch (deko3d only: nothing reads them there).
static void default_vulkan_cpu_paths() {
#ifndef __SWITCH__
    for (const char* name : reporthdr::kVulkanCpuPaths) {  // the list: report_header.cpp
#ifdef _WIN32
        if (!getenv(name)) _putenv_s(name, "1");
#else
        setenv(name, "1", 0);
#endif
    }
#endif
}

// captures/wwhd.log: the whole log of this run (the previous run's is kept as wwhd-previous.log), so
// players can attach it to an issue; on Windows the console output of the game is otherwise lost.
// User paths are redacted as in crash logs. WWHD_LOG_FILE=<path> writes elsewhere, =0 turns it off
// (Android: off unless set; logcat has it). The file stops at 64 MiB.
static int g_log_fd = -1;
static size_t g_log_bytes = 0;
static constexpr size_t kLogFileMax = 64u << 20;
static void log_file_raw(int fd, const char* s, size_t n) {
#ifdef _WIN32
    if (fd >= 0) _write(fd, s, (unsigned)n);
#else
    if (fd >= 0 && write(fd, s, n) < 0) {}
#endif
}
static void log_file_line(int fd, const char* s, size_t n) {
    crash_context::redact(fd, {s, n}, log_file_raw);
    if (n == 0 || s[n - 1] != '\n') log_file_raw(fd, "\n", 1);
}
static void log_file_sink(const char* s, size_t n) {
    if (g_log_fd < 0 || g_log_bytes > kLogFileMax) return;
    log_file_line(g_log_fd, s, n);
    g_log_bytes += n + 1;
    if (g_log_bytes > kLogFileMax) {
        static const char note[] = "[log] the log file reached 64 MiB; later lines go to the console only\n";
        log_file_raw(g_log_fd, note, sizeof note - 1);
    }
}
static void start_log_file() {
#ifdef __SWITCH__
    // (the session log in sdmc:/switch/wwhd/logs/ already has every line: no second copy on the SD card)
#else
    const char* e = getenv("WWHD_LOG_FILE");
    if (e && !strcmp(e, "0")) return;
#ifdef __ANDROID__
    if (!e || !*e) return;
#endif
    std::string path = e && *e ? e : "captures/wwhd.log";
    std::error_code ec;
    if (!(e && *e)) {
        std::filesystem::create_directories("captures", ec);
        std::filesystem::remove("captures/wwhd-previous.log", ec);
        std::filesystem::rename(path, "captures/wwhd-previous.log", ec);
    }
#ifdef _WIN32
    g_log_fd = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    g_log_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#endif
    if (g_log_fd < 0) { LOG("[log] cannot write %s", path.c_str()); return; }
    log_ring_write(g_log_fd, log_file_line);  // the lines logged before (only the boot so far)
    log_set_sink(log_file_sink);
    LOG("[log] writing %s", path.c_str());
#endif
}

int main(int argc, char** argv) {
#ifdef __SWITCH__
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
#endif
#ifndef __SWITCH__
    mods::code::startup(argc, argv);  // (code mods rebuild and replace the executable: desktop only)
#endif
    apply_portable_mode();
    default_vulkan_cpu_paths();
#ifdef _WIN32
    // Windows sleeps in steps of the system timer (15.6 ms by default): sleep_for(1 ms) took
    // 15.7 ms, the 3 ms AX frame loop ran in bursts and vsync waits alternated 15.7 / 31.5 ms.
    // 1 ms resolution for the whole process; Windows restores it when the process exits.
    timeBeginPeriod(1);
#endif
#ifdef __ANDROID__
    // Everything lives in the app's external files folder (Android/data/<package>/files), which a
    // computer can reach over USB: game/ (the extracted game: code, content, meta), save/, and the
    // settings and shader caches in config/. Relative paths (captures/) resolve there too.
    if (const char* dir = SDL_GetAndroidExternalStoragePath()) {
        if (chdir(dir) != 0) fprintf(stderr, "cannot enter %s\n", dir);
        setenv("XDG_CONFIG_HOME", (std::string(dir) + "/config").c_str(), 1);
        // draw batching (the default everywhere since; kept explicit) and the CPU paths
        // (default_vulkan_cpu_paths) are on; env.txt can turn any off
        setenv("WWHD_VK_DRAW_BATCH", "2048", 0);
        // 60 fps (frame interpolation) is on unless chosen otherwise (settings.ini, read when the
        // renderer starts); platform/perf_hint.cpp pauses it where the phone cannot keep up
        if (!getenv("WWHD_INTERP") && !getenv("WWHD_TRUE60")) interp::set_mode(1);
        // env.txt there: one NAME=value per line (the WWHD_ options of the README); # comments
        if (FILE* f = fopen("env.txt", "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                char* eq = strchr(line, '=');
                if (line[0] == '#' || !eq || eq == line) continue;
                *eq = 0;
                setenv(line, eq + 1, 1);
                LOG("[boot] env.txt: %s=%s", line, eq + 1);
            }
            fclose(f);
        }
    }
#endif
    bool warm_shaders = false;
#ifdef WWHD_HAS_VULKAN
    bool renderer_smoke = false;
#endif
#ifdef __SWITCH__
    // (this fork) the console has no arguments: settings.ini's [dev] section can choose the game and save folders
    // (e.g. another region's game next to the usual one; WWHD_STATE_DIR does the same for save states)
    if (const char* e = getenv("WWHD_GAME_DIR"); e && *e) config::game_dir = e;
    if (const char* e = getenv("WWHD_SAVE_DIR"); e && *e) config::save_dir = e;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) config::game_dir = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) config::save_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_trace_hle = true;
        else if (!strcmp(argv[i], "--warm-shaders")) warm_shaders = true;
#ifdef WWHD_HAS_VULKAN
        else if (!strcmp(argv[i], "--renderer-smoke")) renderer_smoke = true;
#endif
    }
    crash_context::initialize();
    install_crash_handler();
    start_log_file();
    // which build on which system: also in crash logs (their last log lines)
    LOG("[boot] Wind Waker HD %s (%s), %s", build::version(), build::commit(), reporthdr::os_description().c_str());
#ifdef __SWITCH__
    // (this fork) the game's files, else an error on screen and the app closes; the first start's shader warning
    startup_checks::game_files(config::game_dir);
    startup_checks::shader_cache();
#endif
    mods::log_startup();
    // test aid: WWHD_TEST_HOST_CRASH=1 crashes inside a system library (strlen of a bad pointer), so
    // the crash log's module names can be checked (CTest crash_log_module, runtime/tools/crash_log_test.cmake)
    if (getenv("WWHD_TEST_HOST_CRASH")) {
        LOG("[boot] WWHD_TEST_HOST_CRASH: crashing on purpose in the C library");
        size_t (*volatile len)(const char*) = strlen;
#if !defined(_WIN32) && !defined(__SWITCH__)
        // the C library's own strlen: zig links its own copy into the executable (Linux releases)
        if (void* f = dlsym(RTLD_DEFAULT, "strlen")) len = (size_t (*)(const char*))f;
#endif
        LOG("%zu", len((const char*)(uintptr_t)16));
    }
    // Metal or Vulkan: --renderer=, WWHD_RENDERER_RUNTIME, Graphics > Renderer (gfx/renderer.h)
    render::choose(argc, argv);
#ifdef WWHD_HAS_VULKAN
    if(renderer_smoke) {
        // GPU self-test of the Vulkan renderer (no game files): always Vulkan, no fallback
        int result = 1;
        host::with_autorelease_pool([&] {
            render::g_backend = &render::vulkan_backend();
            try {
                render::g_backend->init();
            } catch (const std::exception& e) {
                fprintf(stderr, "[renderer smoke] FAIL: Vulkan could not start: %s\n", e.what());
                return;
            }
            result = gfxvk::renderer_smoke_test();
        });
        return result;
    }
#endif
    mods::manager::load_saved();  // player choices, before the game starts
    mods::cemu::set_vulkan(render::requested()==render::Api::Vulkan);
    mods::content::set_game_root(config::game_dir);  // loose imports (fan translations) find their game path
    mods::packages::set_code_mod_support(guestmods::hooks_built() && mods::code::enabled());
    mods::packages::initialize();
#ifdef __SWITCH__
    early_settings::apply_migrated_mods();
#endif
    mem::init();
    auto valid_mod_memory = [](uint32_t address, size_t size) {
        if (size > 1024 * 1024) return false;
        uint64_t end = uint64_t(address) + size;
        return (address >= mem::kMem2Start && end <= mem::kMem2End) ||
               (address >= mem::kMem1 && end <= uint64_t(mem::kMem1) + mem::kMem1Size) ||
               (address >= mem::kFgBucket && end <= uint64_t(mem::kFgBucket) + mem::kFgBucketSize);
    };
    // Store noncapturing callbacks: native mods operate on guest data, on the game thread.
    static auto valid_memory = valid_mod_memory;
    mods::packages::set_memory_access(
        [](uint32_t a, void* out, size_t n) -> int {
            if (!out || !valid_memory(a, n)) return 0;
            memcpy(out, mem::ptr(a), n); return 1;
        },
        [](uint32_t a, const void* in, size_t n) -> int {
            if (!in || !valid_memory(a, n)) return 0;
            memcpy(mem::ptr(a), in, n); return 1;
        });

    LoadedModule m{};
    std::string rpx = config::game_dir + "/code/cking.rpx";
    if (!load_rpx(rpx, m)) fatal("cannot load %s", rpx.c_str());
    if (m.entry != g_recomp_entry_point) fatal("%s does not match the recompiled code", rpx.c_str());
    LOG("[boot] loaded %s (%s build, title %s): entry %08X sda %08X sda2 %08X data end %08X", rpx.c_str(),
        g_guest_build_name, g_guest_build_title_id, m.entry, m.sda_base, m.sda2_base, m.data_end);

    dispatch::init();
    guestmods::init();  // trusted manager packages, before guest threads start
    init_data_imports();
    mem_setup_heaps(m.data_end);
    threads::init(m);

    uint32_t argv_arr = mem::runtime_alloc(16);
    uint32_t arg0 = mem::runtime_alloc(16);
    mem::write_cstr(arg0, "cking.rpx", 16);
    st32(argv_arr, arg0);
    // the game runs on its own threads; the process main thread belongs to the window system
    render::init();
    mods::cemu::set_vulkan(render::active()==render::Api::Vulkan);
    if (warm_shaders) {
        // compile the shader head start once (fills the macOS Metal shader cache), then quit
#ifdef WWHD_HAS_METAL
        if (render::active() == render::Api::Metal) return gfx_headstart_warm();
#endif
        fprintf(stderr, "--warm-shaders fills the Metal shader cache; the %s renderer compiles shaders on first use.\n",
                render::api_name(render::active()));
        return 1;
    }
    static LoadedModule mod = m;
    static uint32_t args = argv_arr;
    std::thread([] {
        threads::run_main(mod, 1, args);
        LOG("[boot] game main thread returned");
        std::exit(0);
    }).detach();
    render::run_main_loop();
    return 0;
}
