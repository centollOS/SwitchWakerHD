// The game's debug server commands (debug_switch.h). The client is tools/switch/wwhd_debug.py; docs/debug-server.md.
#include "debug_switch.h"

#include <switch.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../build_info.h"
#include "../gfx/switch_renderer.h"
#include "../gfx/deko/frame_pacing.h"
#include "../game_clock.h"
namespace gx2 { extern uint32_t g_render_thread_handle; }  // (gx2_core.cpp: rtcores)
#include "../mods/mods.h"
#include "../mods/warps.h"
#include "../hle/olv_park.h"
#include "../runtime.h"
#include "../overlay/hostui.h"
#include "debug_server.h"
#include "prepare_graphics_switch.h"
#include "shader_scan_switch.h"
#include "background_shaders_switch.h"
#include "audio_guard_switch.h"
#include "thread_sampler.h"
#include "bench_switch.h"  // (this fork) the performance test
#include "shader_wait_switch.h"
#include "host.h"
#include "settings_switch.h"

namespace render { uint64_t frame_count(); }

#ifdef WWHD_HEAP_CENSUS
namespace heap_census {
void register_commands();
}
#endif

namespace debug_switch {
namespace {
std::atomic<bool> g_quit{false};
int g_port = 0;
bool g_running = false;
constexpr int kPort = 6543;

std::string ip_text() {
    in_addr a;
    a.s_addr = uint32_t(gethostid());  // libnx: the console's address on the network (nifm), network order
    return inet_ntoa(a);
}

bool game_running() { return render::frame_count() > 0; }  // guest memory and the game's data are there

// battery charge, charger and mode (psm), e.g. "72%, on battery, handheld": to know before a long test on the console
std::string power_text() {
    static const bool psm = R_SUCCEEDED(psmInitialize());
    u32 percent = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (!psm || R_FAILED(psmGetBatteryChargePercentage(&percent)) || R_FAILED(psmGetChargerType(&charger))) return "unknown";
    return std::to_string(percent) + "%, " + (charger == PsmChargerType_Unconnected ? "on battery" : "charging") + ", " +
           (appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
}

debugsrv::Reply info(const debugsrv::Args&) {
    char b[640];
    AppletType type = appletGetAppletType();
    snprintf(b, sizeof b,
             "version %s (%s)\nframe %llu\nstage %s\nheap never used %zu MiB\nbattery %s\nrunning as %s\naddress %s:%d\n",
             build::version(), build::commit(), (unsigned long long)render::frame_count(),
             game_running() ? mods::current_stage().c_str() : "-", heap_never_used_mib(), power_text().c_str(),
             type == AppletType_Application ? "application (the HOME menu forwarder: reload works)"
                                            : "applet (hbmenu: reload does not work, use quit and nxlink)",
             ip_text().c_str(), g_port);
    return debugsrv::ok(b);
}

debugsrv::Reply warps(const debugsrv::Args&) {
    std::string out;
    int i = 0;
    char b[160];
    auto add = [&](const mods::Warp& w) {
        snprintf(b, sizeof b, "%3d  %-8s room %2d point %3d  %s\n", i++, w.stage, w.room, w.point, w.label ? w.label : "");
        out += b;
    };
    for (const mods::Warp& w : mods::kMainWarps) add(w);
    for (const mods::Warp& w : mods::kAllWarps) add(w);
    return debugsrv::ok(out);
}

debugsrv::Reply warp(const debugsrv::Args& a) {
    if (a.empty()) return debugsrv::err("usage: warp <number from warps> | warp <stage> [room] [point]");
    if (!game_running()) return debugsrv::err("the game has not started yet");
    const int mainCount = int(sizeof mods::kMainWarps / sizeof mods::kMainWarps[0]);
    const int allCount = int(sizeof mods::kAllWarps / sizeof mods::kAllWarps[0]);
    char* end;
    const long n = strtol(a[0].c_str(), &end, 10);
    std::string stage;
    int room = 0, point = 0;
    if (*end == 0 && a.size() == 1) {
        if (n < 0 || n >= mainCount + allCount) return debugsrv::err("no warp number " + a[0] + " (warps lists them)");
        const mods::Warp& w = n < mainCount ? mods::kMainWarps[n] : mods::kAllWarps[n - mainCount];
        stage = w.stage, room = w.room, point = w.point;
    } else {
        stage = a[0];
        if (stage.size() > 7) return debugsrv::err("a stage name has 7 characters at most");
        if (a.size() > 1) room = atoi(a[1].c_str());
        if (a.size() > 2) point = atoi(a[2].c_str());
    }
    LOG("[debug] warp to %s room %d point %d requested", stage.c_str(), room, point);
    mods::request_warp(stage.c_str(), room, point);
    return debugsrv::ok("warp to " + stage + " room " + std::to_string(room) + " point " + std::to_string(point) +
                        " requested (it happens once a file is loaded and no other scene change is pending)");
}

void remove_dir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) remove((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    rmdir(dir.c_str());
}

// "shot [game]": the next frame's pictures through the renderer's capture (gfx/deko/dk_capture.h), sent and deleted
debugsrv::Reply shot(const debugsrv::Args& a) {
    if (!game_running()) return debugsrv::err("the renderer has not presented a frame yet");
    const bool game = !a.empty() && a[0] == "game";
    const uint64_t before = gfxsw::pictures_done_frame();
    const uint64_t first = gfxsw::request_pictures();
    uint64_t done = 0;
    for (int i = 0; i < 400; i++) {  // 20 s: the PNG writer is a background thread
        done = gfxsw::pictures_done_frame();
        if (done != before && done >= first) break;
        done = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!done) return debugsrv::err("no capture within 20 s (is the game presenting frames?)");
    const std::string n = std::to_string(done);
    const std::string dir = host::config_dir() + "/captures/" + n;
    debugsrv::Reply r;
    r.file = dir + "/frame_" + n + (game ? ".png" : "_window.png");
    r.after = [dir] { remove_dir(dir); };
    return r;
}

debugsrv::Reply reload(const debugsrv::Args&) {
    if (appletGetAppletType() != AppletType_Application)
        return debugsrv::err("reload needs the HOME menu forwarder (running from hbmenu: quit, then nxlink or hbmenu)");
    debugsrv::Reply r = debugsrv::ok("restarting: the forwarder loads the NRO from the SD card again");
    r.after = [] {
        LOG("[debug] reload: the program restarts (appletRestartProgram)");
        log_flush();
        debugsrv::stop();
        static const char arg[] = "wwhd debug reload";
        const Result rc = appletRestartProgram(arg, sizeof arg);
        // (on success the system ends this process)
        LOG("[debug] reload failed: appletRestartProgram rc 0x%x", rc);
    };
    return r;
}

// "prepare start|stop|status": the Prepare graphics sweep (prepare_graphics_switch.h)
debugsrv::Reply prepare(const debugsrv::Args& a) {
    const std::string what = a.empty() ? "status" : a[0];
    if (what == "start") {
        const std::string why = prepare_graphics::start();
        return why.empty() ? debugsrv::ok("prepare graphics: started") : debugsrv::err(why);
    }
    if (what == "stop") {
        prepare_graphics::stop();
        return debugsrv::ok("prepare graphics: stopping after the current place");
    }
    const std::string st = prepare_graphics::status();
    return debugsrv::ok(st.empty() ? "prepare graphics: not running" : st);
}

// "scanbench": shader_scan over the game's files, in a thread (the result in the log: [scan])
debugsrv::Reply scanbench(const debugsrv::Args&) {
    std::thread([] {
        shader_scan::Stats st;
        const auto progs = shader_scan::scan(config::game_dir, st);
        LOG("[scan] %zu files (%.0f MB read in %.1f s), %zu archives, %zu shaders, %zu distinct; %.0f MB inflated in "
            "%.1f s; total %.1f s", st.files, st.bytesRead / 1048576.0, st.readSeconds, st.archives, st.shaders,
            st.distinct, st.bytesInflated / 1048576.0, st.inflateSeconds, st.totalSeconds);
    }).detach();
    return debugsrv::ok("scan started: see the log ([scan])");
}

// "bgshaders [start]": background shaders (background_shaders_switch.h)
debugsrv::Reply bgshaders(const debugsrv::Args& a) {
    if (!a.empty() && a[0] == "start") {
        const std::string why = background_shaders::start();
        return why.empty() ? debugsrv::ok("background shaders: started") : debugsrv::err(why);
    }
    return debugsrv::ok("background shaders: " + (background_shaders::status().empty() ? std::string("not started")
                                                                                       : background_shaders::status()));
}

// "bloom [0|1]" / "blur [0|1]": the Switch tab's Bloom and Distance blur (gfxsw::set_bloom, set_distance_blur), not saved
debugsrv::Reply bloom(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_bloom(a[0] != "0");
    return debugsrv::ok(std::string("bloom ") + (gfxsw::bloom() ? "on" : "off"));
}
debugsrv::Reply blur(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_distance_blur(a[0] != "0");
    return debugsrv::ok(std::string("blur ") + (gfxsw::distance_blur() ? "on" : "off"));
}

// "pacing [0|1]": frame pacing mode (gfx/deko/frame_pacing.h, #23): 1 swap interval the game's (the default),
// 0 swap interval 1 (as before); not saved
// "clockrate [1-4]": the game clock's rate as the fast forward mod sets it (timebase::set_clock_rate): the
// sessions after a fast forward, reproduced without a dialogue
debugsrv::Reply clockrate(const debugsrv::Args& a) {
    if (!a.empty()) timebase::set_clock_rate(unsigned(atoi(a[0].c_str())));
    return debugsrv::ok("clockrate " + std::to_string(game_clock::rate()) + (game_clock::shifted() ? " (shifted)" : ""));
}

// "targetmemo2 [0|1]", "fixedmemo [0|1]", "vtxident [0|1]": the draw-memo round's switches (gfx/deko dk_draw.h; the
// Switch tab's Debug section has them too): on by default, not saved; each change is logged with its frame
debugsrv::Reply draw_opt_reply(const char* name, int opt, const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_draw_opt(opt, a[0] != "0");
    return debugsrv::ok(std::string(name) + (gfxsw::draw_opt(opt) ? " 1" : " 0"));
}
debugsrv::Reply targetmemo2(const debugsrv::Args& a) { return draw_opt_reply("targetmemo2", gfxsw::kOptTargetMemo2, a); }
debugsrv::Reply fixedmemo(const debugsrv::Args& a) { return draw_opt_reply("fixedmemo", gfxsw::kOptFixedMemo, a); }
debugsrv::Reply vtxident(const debugsrv::Args& a) { return draw_opt_reply("vtxident", gfxsw::kOptVtxIdentity, a); }

// "rtcores [preferred mask]" (test): the render thread's cores (svcSetThreadCoreMask; mask bit n = core n), and which
// core it was last seen on (a context read)
debugsrv::Reply rtcores(const debugsrv::Args& a) {
    if (!gx2::g_render_thread_handle) return debugsrv::err("rtcores: no render thread yet");
    if (a.size() >= 2) {
        const Result rc = svcSetThreadCoreMask(gx2::g_render_thread_handle, atoi(a[0].c_str()), strtoul(a[1].c_str(), nullptr, 0));
        if (R_FAILED(rc)) return debugsrv::err("rtcores: svcSetThreadCoreMask 0x" + std::to_string(rc));
        LOG("[debug] render thread cores: preferred %s, mask %s", a[0].c_str(), a[1].c_str());
    }
    s32 pref = -1;
    u64 mask = 0;
    svcGetThreadCoreMask(&pref, &mask, gx2::g_render_thread_handle);
    return debugsrv::ok("rtcores preferred " + std::to_string(pref) + " mask 0x" + std::to_string(mask));
}

debugsrv::Reply pacing(const debugsrv::Args& a) {
    if (!a.empty()) frame_pacing::set_mode(atoi(a[0].c_str()));
    return debugsrv::ok("pacing " + std::to_string(frame_pacing::mode()) + ": swap interval " +
                        (frame_pacing::mode() ? "the game's" : "1"));
}

// "dspguard [0|1]": the game's DSP-overload guard (audio_guard_switch.h): 1 the game's own, 0 off (the default)
debugsrv::Reply dspguard(const debugsrv::Args& a) {
    if (!a.empty()) audio_guard::set_kept(a[0] != "0");
    return debugsrv::ok(std::string("dspguard ") + (audio_guard::kept() ? "the game's own" : "off"));
}

// "shaderthreads [0-3]": threads that compile the shaders draws ask for (gfx/deko helper_main; 0 auto; not saved)
debugsrv::Reply shaderthreads(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_shader_compile_threads(atoi(a[0].c_str()));
    return debugsrv::ok("shaderthreads " + std::to_string(gfxsw::shader_compile_threads()));
}

// "shaderwait [0-3]": draws of a shader still compiling (shader_wait_switch.h, #49): 0 skipped (the default), 1 wait
// while an item is shown, 2 also during events, 3 always; not saved (the menu saves it)
debugsrv::Reply shaderwait(const debugsrv::Args& a) {
    if (!a.empty()) shader_wait::set_mode(atoi(a[0].c_str()));
    return debugsrv::ok(std::string("shaderwait ") + std::to_string(shader_wait::mode()) + ": " +
                        shader_wait::label(shader_wait::mode()));
}

// "shadowscale [0.5|1|2]", "noao [0|1]", "miiverse [0|1]": the Developer options' performance tests (not saved)
debugsrv::Reply shadowscale(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_shadow_scale(float(atof(a[0].c_str())));
    return debugsrv::ok("shadowscale " + std::to_string(gfxsw::shadow_scale()));
}
debugsrv::Reply aniso(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_aniso_level(atoi(a[0].c_str()));
    return debugsrv::ok("aniso " + std::to_string(gfxsw::aniso_level()));
}
debugsrv::Reply noao(const debugsrv::Args& a) {
    if (!a.empty()) gfxsw::set_skip_ao(atoi(a[0].c_str()) != 0);
    return debugsrv::ok(std::string("noao ") + (gfxsw::skip_ao() ? "1" : "0"));
}
debugsrv::Reply miiverse(const debugsrv::Args& a) {
    if (!a.empty()) hle_olv::set_parked(atoi(a[0].c_str()) == 0);  // miiverse 0 = the thread stopped
    return debugsrv::ok(std::string("miiverse ") + (hle_olv::parked() ? "0 (parked)" : "1 (running, 1 ms nap per step)"));
}

// "profile [seconds]": sample the frame threads (thread_sampler.h, docs/cpu-threading-plan.md 0.2); "profile status"
debugsrv::Reply profile(const debugsrv::Args& a) {
    if (!a.empty() && a[0] == "status") return debugsrv::ok("profile: " + thread_sampler::status());
    const std::string why = thread_sampler::start(a.empty() ? 10 : atof(a[0].c_str()));
    return why.empty() ? debugsrv::ok("profile: sampling") : debugsrv::err("profile: " + why);
}

// "stall <ms> [core] [count] [gap ms]": a host thread above every game thread spins on a core, as
// the hitches that set off the game's "kill DSP channel" (issue #13) do; to test the guard at will
Thread g_stallThread;
std::atomic<bool> g_stalling{false};
int g_stallMs = 0, g_stallCount = 0, g_stallGapMs = 0;

void stall_main(void*) {
    for (int i = 0; i < g_stallCount; ++i) {
        const uint64_t end = armGetSystemTick() + armNsToTicks(uint64_t(g_stallMs) * 1000000);
        while (armGetSystemTick() < end) {}
        if (i + 1 < g_stallCount) svcSleepThread(int64_t(g_stallGapMs) * 1000000);
    }
    g_stalling = false;
}

debugsrv::Reply stall(const debugsrv::Args& a) {
    if (a.empty()) return debugsrv::err("stall <ms> [core] [count] [gap ms]");
    if (g_stalling.exchange(true)) return debugsrv::err("a stall is running");
    if (g_stallThread.handle) threadClose(&g_stallThread);
    g_stallMs = std::max(1, std::min(atoi(a[0].c_str()), 1000));
    const int core = a.size() > 1 ? std::max(0, std::min(atoi(a[1].c_str()), 2)) : 0;
    g_stallCount = a.size() > 2 ? std::max(1, std::min(atoi(a[2].c_str()), 1000)) : 1;
    g_stallGapMs = a.size() > 3 ? std::max(0, std::min(atoi(a[3].c_str()), 10000)) : 1000;
    if (R_FAILED(threadCreate(&g_stallThread, stall_main, nullptr, nullptr, 64 << 10, 0x20, core)) ||
        R_FAILED(threadStart(&g_stallThread))) {
        g_stalling = false;
        return debugsrv::err("thread not started");
    }
    LOG("[debug] stall: %d ms on core %d, %d times, %d ms apart", g_stallMs, core, g_stallCount, g_stallGapMs);
    return debugsrv::ok("stalling");
}

// "evict [now | idle <frames>]": texture eviction (issue #11, gfx/deko/surfaces.cpp)
debugsrv::Reply evict(const debugsrv::Args& a) {
    if (!a.empty() && a[0] == "now") gfxsw::evict_textures_now();
    else if (a.size() > 1 && a[0] == "idle") gfxsw::set_texture_idle_frames(strtoull(a[1].c_str(), nullptr, 10));
    else if (!a.empty()) return debugsrv::err("evict [now | idle <frames>]");
    return debugsrv::ok(gfxsw::texture_eviction_report());
}

// game memory as hex, 16 bytes a line (big-endian words as the game sees them): MEM2 (data, heap) only
debugsrv::Reply peek(const debugsrv::Args& a) {
    if (a.empty()) return debugsrv::err("peek <address> [bytes]");
    const uint32_t addr = uint32_t(strtoul(a[0].c_str(), nullptr, 16)) & ~3u;
    const uint32_t len = std::min<uint32_t>(a.size() > 1 ? uint32_t(strtoul(a[1].c_str(), nullptr, 0)) : 64u, 4096u);
    if (addr < mem::kMem2Start || addr + len > mem::kMem2End || addr + len < addr)
        return debugsrv::err("peek: outside MEM2 (10000000-4FFFFFFF)");
    std::string out;
    char b[16];
    for (uint32_t o = 0; o < len; o += 4) {
        if (o % 16 == 0) {
            snprintf(b, sizeof b, "%s%08X:", o ? "\n" : "", addr + o);
            out += b;
        }
        snprintf(b, sizeof b, " %08X", ld32(addr + o));
        out += b;
    }
    return debugsrv::ok(out + "\n");
}

// a big-endian word into game memory (MEM2), for experiments on the game's data
debugsrv::Reply poke(const debugsrv::Args& a) {
    if (a.size() < 2) return debugsrv::err("poke <address> <word>");
    const uint32_t addr = uint32_t(strtoul(a[0].c_str(), nullptr, 16)) & ~3u;
    const uint32_t v = uint32_t(strtoul(a[1].c_str(), nullptr, 16));
    if (addr < mem::kMem2Start || addr + 4 > mem::kMem2End) return debugsrv::err("poke: outside MEM2 (10000000-4FFFFFFF)");
    const uint32_t old = ld32(addr);
    st32(addr, v);
    LOG("[debug] poke %08X: %08X -> %08X", addr, old, v);
    char b[64];
    snprintf(b, sizeof b, "%08X: %08X -> %08X\n", addr, old, v);
    return debugsrv::ok(b);
}

debugsrv::Reply quit(const debugsrv::Args&) {
    debugsrv::Reply r = debugsrv::ok("quitting");
    r.after = [] {
        LOG("[debug] quit requested");
        debugsrv::stop();
        g_quit = true;
    };
    return r;
}

}  // namespace

void start() {
    std::string on;
    if (!hostui::get(switch_settings::kKeyDebugServer, on) || on != "1") return;
    g_port = kPort;
    // a console under test from the computer is often left alone for minutes: no auto-sleep while the server runs
    appletSetAutoSleepDisabled(true);
    LOG("[debug] auto-sleep off while the debug server runs");
    debugsrv::add_command("info", "info                   build, frame, stage, heap, battery, address", info);
#ifdef WWHD_HEAP_CENSUS
    ::heap_census::register_commands();  // (this fork, memory round 2) heap_census_switch.cpp
#endif
    debugsrv::add_command("warps", "warps                  the warp destinations, numbered", warps);
    debugsrv::add_command("warp", "warp <n> | warp <stage> [room] [point]", warp);
    debugsrv::add_command("shot", "shot [game]            PNG of the next frame (game: the game's picture alone)", shot);
    debugsrv::add_command("reload", "reload                 restart: the forwarder loads the NRO again (put it first)", reload);
    debugsrv::add_command("prepare", "prepare start|stop|status  the Prepare graphics sweep", prepare);
    debugsrv::add_command("scanbench", "scanbench              time reading the game's shader programs (log)", scanbench);
    debugsrv::add_command("shaderthreads", "shaderthreads [0-3]    shader compile threads: 0 auto, 1-3", shaderthreads);
    debugsrv::add_command("shaderwait", "shaderwait [0-3]       draws of a compiling shader: 0 skip, 1 items, 2 +events, 3 always",
                          shaderwait);
    debugsrv::add_command("shadowscale", "shadowscale [0.5|1|2]   (test) the shadow maps' scale", shadowscale);
    debugsrv::add_command("aniso", "aniso [1|2|4|8|16]    anisotropic filtering level (not saved)", aniso);
    debugsrv::add_command("noao", "noao [0|1]             (test) skip the ambient-occlusion pass", noao);
    debugsrv::add_command("miiverse", "miiverse [0|1]         (test) 0 parks the Miiverse thread", miiverse);
    debugsrv::add_command("profile", "profile [s] | status   sample the main and render threads (logs/prof_*.samples)",
                          profile);
    debugsrv::add_command("dspguard", "dspguard [0|1]         the game's DSP-overload guard: 1 its own, 0 off", dspguard);
    debugsrv::add_command("clockrate", "clockrate [1-4]        the game clock's rate (as fast forward sets it)", clockrate);
    debugsrv::add_command("targetmemo2", "targetmemo2 [0|1]      target memo by state words + same-frame skip", targetmemo2);
    debugsrv::add_command("fixedmemo", "fixedmemo [0|1]        fixed state in three memos", fixedmemo);
    debugsrv::add_command("vtxident", "vtxident [0|1]         vertex states by layout identity", vtxident);
    debugsrv::add_command("rtcores", "rtcores [pref mask]    (test) the render thread's preferred core and core mask", rtcores);
    debugsrv::add_command("pacing", "pacing [0|1]           frame pacing: 1 swap interval the game's, 0 always 1", pacing);
    debugsrv::add_command("bloom", "bloom [0|1]            the Switch tab's Bloom (not saved)", bloom);
    debugsrv::add_command("blur", "blur [0|1]             the Switch tab's Distance blur (not saved)", blur);
    debugsrv::add_command("stall", "stall <ms> [core] [n] [gap]  spin above the game threads (audio guard tests)", stall);
    debugsrv::add_command("evict", "evict [now | idle <n>] texture eviction: now, or after n unused frames", evict);
    debugsrv::add_command("bgshaders", "bgshaders [start]      background shaders: start / status", bgshaders);
    bench::register_commands();  // (this fork) "bench": docs/perf-test.md
    debugsrv::add_command("quit", "quit                   end the program", quit);
    debugsrv::add_command("peek", "peek <addr> [bytes]    game memory (MEM2) as hex words", peek);
    debugsrv::add_command("poke", "poke <addr> <word>     write a word of game memory (MEM2)", poke);
    debugsrv::Config c;
    c.port = g_port;
    c.root = host::config_dir();
    c.log = [](const char* line) { LOG("%s", line); };
    c.threadStart = [] { host::raise_thread_priority(); };
    g_running = debugsrv::start(c);
    if (g_running)
        LOG("[debug] server listening on %s:%d (Switch tab > Debug, settings.ini %s=1; client: "
            "tools/switch/wwhd_debug.py; local network only, no password)", ip_text().c_str(), g_port,
            switch_settings::kKeyDebugServer);
}

bool quit_requested() { return g_quit.load(std::memory_order_relaxed); }
void request_quit() {
    LOG("[quit] quit requested");
    debugsrv::stop();
    g_quit = true;
}
bool running() { return g_running; }
int port() { return g_port; }

}  // namespace debug_switch
