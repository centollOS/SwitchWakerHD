// Prepare graphics (prepare_graphics_switch.h, docs/prepare-graphics-plan.md).
//
// A thread walks mods/warps.h (kMainWarps then kAllWarps, each stage/room once): it asks for the warp (the game's
// own scene change request, written on the game's main thread by frame()), waits for the arrival (no warp pending, no fade, no next-stage request, the
// stage named), then waits until the deko3d shader worker has had nothing pending for a moment (or 60 s), and goes on.
// The next index is kept in prepare_graphics/state.txt, so a sweep stopped or cut continues where it was.
//
// Link's health is topped up every frame during the sweep, so enemies in a place cannot end it.
// The game in progress is only a vehicle: before the first warp the small Quest Log files of save/user (cking.sav,
// the play log, the picture order; the Picto Box pictures cannot change without the player) are copied to
// prepare_graphics/save/, and they are put back at the end (or at the next start, when the game closed during the
// sweep), files that were not there before removed; then the program restarts, dropping what the warps changed in
// memory.
#include "prepare_graphics_switch.h"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../mods/mods.h"
#include "../mods/warps.h"
#include "../rumble.h"
#include "../runtime.h"
#include "guest_addr.h"
#include "host.h"
#include "ui_text_switch.h"

namespace gfxdk {  // gfx/deko/dk_shaders.h (without the decompiler's headers)
uint64_t shaders_pending();
uint64_t shaders_skipped_draws();
}  // namespace gfxdk
namespace render { uint64_t frame_count(); }

namespace prepare_graphics {
namespace {

constexpr double kArriveTimeout = 30.0;   // s: a destination not reached by then is skipped
constexpr double kSettle = 2.0;           // s after the arrival before the worker is watched (draws meet shaders)
// this place is done when nothing is pending and the last kQuietFrames frames skipped no draw for a shader (every
// draw the game made there had its shaders), at least kQuiet s
constexpr double kQuiet = 1.0;
constexpr uint64_t kQuietFrames = 30;
constexpr double kPlaceTimeout = 60.0;    // s at most in one place
// Warping out while a place's arrival event still runs stopped the game (c_xyz.cpp:285 isNearZeroSquare: M2tower ->
// M_DaiB after 10 s): the next warp waits until Link has had control for a moment (no event, message, game menu or
// stage change, Link the controlled actor: the game's own pause-menu conditions, savestate.cpp player_has_control),
// after at least kMinStay; places known to have a long arrival event stay at least kMinStayEvent anyway.
constexpr double kMinStay = 2.0;          // (the wait for Link's control guards the arrival events)
constexpr double kMinStayEvent = 12.0;
constexpr double kControlFor = 1.0;       // s of control in a row
constexpr double kControlTimeout = 20.0;  // s: then the warp goes anyway (logged; a new game's story events)
constexpr double kHoldToStop = 1.5;       // s of B held on the loading screen
const char* const kArrivalEvents[] = {"M2tower"};
constexpr uint32_t kMaxBackup = 64 << 10; // save/user files up to this size are copied (not the pictures)

const uint32_t kNextStageReq = GD(0x1046F0B0) + 0x5140 + 12;  // mods/cheats.cpp kNextStage + 12: a request is set
const uint32_t kOverlap = GD(0x101F36CC);                      // a scene change's fade is running (mods/turbo.cpp)
// savestate.cpp player_has_control's fields of g_dComIfG_gameInfo.play
// from the title screen the game starts a game the way the file select does (d_s_name: 025ADC60): its next stage,
// then fopScnM_ChangeReq(the current scene, the play scene 7, fade 0, 5, 1); the title's own call (024B8854) finds
// its scene with fopScnM_SearchByID(the global 1047E6C4)
const uint32_t fn_ovlpDoingReq = GC(0x025DBE00);  // fopOvlpM_IsDoingReq: a scene overlap runs
const uint32_t fn_searchById = GC(0x025DC80C);    // fopScnM_SearchByID
const uint32_t fn_changeReq = GC(0x025DC86C);     // fopScnM_ChangeReq
const uint32_t kSceneId = GD(0x1047E6C4);         // the current scene's process id
constexpr uint32_t kPlayScene = 7;
const uint32_t kSaveInfoPtr = GD(0x101F84DC);  // dComIfGs save area (dSv_info_c at +0x20: max life u16, life u16)
const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kEventRun = kPlay + 0x5292;     // dComIfGp_event_runCheck
const uint32_t kMesgStatus = kPlay + 0x5BB2;   // dComIfGp_getMesgStatus
const uint32_t kScopeMesgStatus = kPlay + 0x5BB3;
const uint32_t kMenuFlag = GD(0x101EA069);     // dMenu_flag
const uint32_t kPlayerPtr = kPlay + 0x5B2C;    // the controlled actor
const uint32_t kLinkPtr = kPlay + 0x5B34;      // daPy_lk_c

struct Place {
    const char* stage;
    int room, point;
};

std::mutex g_mu;            // g_status, g_live
std::string g_status;
Live g_live;
float g_volume = 1.0f;      // audout's volume before the sweep (muted during it)
bool g_rumble = true;       // the rumble option before the sweep (off during it)
std::atomic<bool> g_running{false}, g_stop{false};
std::atomic<bool> g_atTitle{false};  // start (or continue) once a game is in progress
// the next warp, written by frame() on the game's main thread as mods/cheats.cpp warp_service does (mods::request_warp
// waits for a loaded file, which the title screen's placeholder save data is not)
std::mutex g_titleMu;
Place g_titleWarp{};
std::atomic<bool> g_titleWarpPending{false};

std::string dir() { return host::config_dir() + "/prepare_graphics"; }
std::string save_user() { return config::save_dir + "/user"; }

std::vector<Place> places() {
    std::vector<Place> out;
    std::set<std::string> seen;
    auto add = [&](const mods::Warp& w) {
        if (seen.insert(std::string(w.stage) + "/" + std::to_string(w.room)).second) out.push_back({w.stage, w.room, w.point});
    };
    for (const mods::Warp& w : mods::kMainWarps) add(w);
    for (const mods::Warp& w : mods::kAllWarps) add(w);
    // last, the title screen's stage (the "press Start" view of Outset): no warp works after it (mods/warps.h leaves it
    // out for that), but it is the last one and the game restarts there, so the first picture after the sweep is whole
    out.push_back({"sea_T", 44, 0});
    return out;
}

void set_status(const std::string& s) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = s;
    }
    LOG("[prepare] %s", s.c_str());
}

bool read_file(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    fclose(f);
    return true;
}
bool write_file(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

// state.txt: "<next index>"
size_t read_index() {
    std::string s;
    return read_file(dir() + "/state.txt", s) ? strtoul(s.c_str(), nullptr, 10) : 0;
}
void write_index(size_t i) { write_file(dir() + "/state.txt", std::to_string(i) + "\n"); }

// the Quest Log files: names.txt lists what save/user held before (one name a line), the small ones are copied
bool backup_save() {
    const std::string b = dir() + "/save";
    if (access((b + "/names.txt").c_str(), F_OK) == 0) return true;  // a cut sweep's copy: still the original one
    mkdir(dir().c_str(), 0777);
    mkdir(b.c_str(), 0777);
    std::string names;
    if (DIR* d = opendir(save_user().c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            const std::string p = save_user() + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            names += std::string(e->d_name) + "\n";
            if (st.st_size > kMaxBackup) continue;
            std::string data;
            if (!read_file(p, data) || !write_file(b + "/" + e->d_name, data)) return false;
        }
        closedir(d);
    }
    return write_file(b + "/names.txt", names);
}

// puts the copy back; true when there was one
bool restore_save() {
    const std::string b = dir() + "/save";
    std::string names;
    if (!read_file(b + "/names.txt", names)) return false;
    std::set<std::string> before;
    for (size_t at = 0, nl; (nl = names.find('\n', at)) != std::string::npos; at = nl + 1) before.insert(names.substr(at, nl - at));
    size_t restored = 0, removed = 0;
    if (DIR* d = opendir(save_user().c_str())) {  // files the sweep's game made (a new game's): removed
        std::vector<std::string> extra;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !before.count(e->d_name)) extra.push_back(e->d_name);
        closedir(d);
        for (const std::string& n : extra) removed += remove((save_user() + "/" + n).c_str()) == 0;
    }
    for (const std::string& n : before) {
        std::string data;
        if (read_file(b + "/" + n, data) && write_file(save_user() + "/" + n, data)) restored++;
        remove((b + "/" + n).c_str());
    }
    remove((b + "/names.txt").c_str());
    rmdir(b.c_str());
    LOG("[prepare] Quest Log files put back: %zu restored, %zu made during the sweep removed", restored, removed);
    return true;
}

bool in_game() {
    const std::string st = mods::current_stage();
    return !st.empty() && st != "sea_T" && st != "Name";
}
bool at_title() { return mods::current_stage() == "sea_T"; }

void request(const Place& p) {
    std::lock_guard<std::mutex> lk(g_titleMu);
    g_titleWarp = p;
    g_titleWarpPending = true;
}

bool arrived(const Place& p) {
    return !mods::warp_pending() && !g_titleWarpPending && !ld8(kNextStageReq) && !ld32(kOverlap) && mods::current_stage() == p.stage;
}

bool link_has_control() {
    return !ld8(kEventRun) && !ld8(kMesgStatus) && !ld8(kScopeMesgStatus) && !ld8(kMenuFlag) && !ld8(kNextStageReq) &&
           ld32(kPlayerPtr) && ld32(kPlayerPtr) == ld32(kLinkPtr);
}

// why Link has no control / the arrival is not done (for the log)
std::string why_not() {
    char b[160];
    snprintf(b, sizeof b, "event %u mesg %u/%u menu %u next %u overlap %08X player %08X link %08X warp %d",
             ld8(kEventRun), ld8(kMesgStatus), ld8(kScopeMesgStatus), ld8(kMenuFlag), ld8(kNextStageReq), ld32(kOverlap),
             ld32(kPlayerPtr), ld32(kLinkPtr), int(mods::warp_pending() || g_titleWarpPending));
    return b;
}

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

// during the sweep: no auto-sleep or screen dimming (30-40 minutes with nobody touching the console), the game muted
// and the controllers still (the player's sound and rumble settings come back at the end)
// B held on the loading screen stops the sweep (its own pad reader: the game's input is not involved)
std::atomic<float> g_hold{0};
void watch_stop_button() {
    PadState pad;
    padInitializeDefault(&pad);
    double heldSince = 0;
    while (g_running && !g_stop) {
        padUpdate(&pad);
        if (padGetButtons(&pad) & HidNpadButton_B) {
            if (heldSince == 0) heldSince = now();
            const double held = now() - heldSince;
            g_hold = float(held / kHoldToStop > 1 ? 1 : held / kHoldToStop);
            if (held >= kHoldToStop) {
                LOG("[prepare] B held on the loading screen: stopping");
                stop();
            }
        } else {
            heldSince = 0;
            g_hold = 0;
        }
        sleep_s(0.05);
    }
}

void console_quiet(bool on) {
    appletSetAutoSleepDisabled(on);
    appletSetMediaPlaybackState(on);
    if (on) {
        if (R_FAILED(audoutGetAudioOutVolume(&g_volume))) g_volume = 1.0f;
        audoutSetAudioOutVolume(0.0f);
        g_rumble = rumble::enabled();
        rumble::set_enabled(false);
    } else {
        audoutSetAudioOutVolume(g_volume);
        rumble::set_enabled(g_rumble);
    }
}

void finish_and_restart(const char* why) {
    {
        char b[256];
        snprintf(b, sizeof b, ui_text::tx().ending, why);
        set_status(b);
    }
    console_quiet(false);
    sleep_s(5);  // the shader cache and manifest writers' last batches (they write every couple of seconds)
    restore_save();
    log_flush();
    if (appletGetAppletType() == AppletType_Application) {
        static const char arg[] = "wwhd prepare graphics";
        const Result rc = appletRestartProgram(arg, sizeof arg);
        LOG("[prepare] restart failed: appletRestartProgram rc 0x%x", rc);
    }
    // from the Homebrew Menu (applet mode) there is no restart: the player closes the game
    set_status(ui_text::tx().endNoRestart);
}

void sweep() {
    const std::vector<Place> list = places();
    size_t i = read_index();
    size_t done = 0, skipped = 0;
    const double t0 = now();
    for (; i < list.size() && !g_stop; i++) {
        const Place& p = list[i];
        char b[256], eta[64] = "";
        if (done >= 5) {  // the minutes left, from this sweep's own pace
            const double left = (now() - t0) / done * double(list.size() - i);
            snprintf(eta, sizeof eta, ui_text::tx().etaPart, left / 60 < 1 ? 1 : int(left / 60 + 0.5));
        }
        snprintf(b, sizeof b, ui_text::tx().statusLine, i + 1, list.size(), p.stage, eta);
        set_status(b);
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_live.place = i + 1;
            g_live.total = list.size();
            g_live.name = p.stage;
            g_live.minutesLeft = done >= 5 ? int((now() - t0) / done * double(list.size() - i) / 60 + 0.5) : -1;
        }
        request(p);
        const double asked = now();
        while (!g_stop && !arrived(p) && now() - asked < kArriveTimeout) sleep_s(0.25);
        if (g_stop) break;
        // in the place but still in its arrival (a story event of a new game's data, as the title screen's: Link's
        // house): good enough, the game draws it
        const bool inPlace = !arrived(p) && mods::current_stage() == p.stage && !g_titleWarpPending;
        if (inPlace) LOG("[prepare] %s room %d: in the place, its arrival still running after %.0f s (%s): going on",
                         p.stage, p.room, kArriveTimeout, why_not().c_str());
        if (!arrived(p) && !inPlace) {
            // a warp the game did not take is often stuck in its scene change: later warps would wait for it
            LOG("[prepare] %s room %d not reached in %.0f s (stage %s): continuing after a restart", p.stage, p.room,
                kArriveTimeout, mods::current_stage().c_str());
            write_index(i + 1);
            skipped++;
            write_file(dir() + "/resume.txt", "1\n");  // continues by itself once a game is in progress again
            finish_and_restart(ui_text::tx().endPlaceFailed);
            return;
        }
        const double at = now();
        sleep_s(kSettle);
        double quietSince = 0;
        uint64_t skips = gfxdk::shaders_skipped_draws(), skipFrame = render::frame_count();
        while (!g_stop && now() - at < kPlaceTimeout) {
            const uint64_t sk = gfxdk::shaders_skipped_draws(), frame = render::frame_count();
            if (sk != skips) skips = sk, skipFrame = frame;  // a draw waited for a shader since the last look
            if (gfxdk::shaders_pending() == 0 && frame - skipFrame >= kQuietFrames) {
                if (quietSince == 0) quietSince = now();
                if (now() - quietSince >= kQuiet) break;
            } else {
                quietSince = 0;
            }
            sleep_s(0.1);
        }
        double minStay = kMinStay;
        for (const char* e : kArrivalEvents)
            if (!strcmp(e, p.stage)) minStay = kMinStayEvent;
        while (!g_stop && now() - at < minStay) sleep_s(0.25);
        const bool last = i + 1 == list.size();  // (no warp follows: the title screen, where Link has no control)
        for (double since = 0, t1 = now(); !g_stop && !last;) {  // Link has control for kControlFor in a row
            if (!link_has_control()) since = 0;
            else if (since == 0) since = now();
            if (since && now() - since >= kControlFor) break;
            if (now() - t1 > kControlTimeout) {
                LOG("[prepare] %s: Link had no control for %.0f s (%s): going on", p.stage, kControlTimeout,
                    why_not().c_str());
                break;
            }
            sleep_s(0.1);
        }
        LOG("[prepare] %zu/%zu %s room %d: %.0f s", i + 1, list.size(), p.stage, p.room, now() - at);
        done++;
        write_index(i + 1);
    }
    const bool complete = i >= list.size();
    LOG("[prepare] %s: %zu places in %.0f min, %zu skipped", complete ? "complete" : "stopped", done, (now() - t0) / 60,
        skipped);
    if (complete) {
        write_index(0);
        write_file(dir() + "/complete.txt", "1\n");  // progress().complete
        write_file(dir() + "/announce.txt", "1\n");  // "Graphics ready" after the restart
    }
    finish_and_restart(complete ? ui_text::tx().endComplete : ui_text::tx().endStopped);
}

}  // namespace

std::string start() {
    if (g_running) return ui_text::tx().errRunning;
    if (!in_game() && !at_title()) return ui_text::tx().errNoGame;
    mkdir(dir().c_str(), 0777);
    if (!backup_save()) return ui_text::tx().errCopy;
    g_stop = false;
    g_running = true;
    console_quiet(true);
    std::thread(watch_stop_button).detach();
    std::thread([] {
        sweep();
        g_running = false;
    }).detach();
    return "";
}

void stop() { g_stop = true; }
bool running() { return g_running; }

bool offer_to_update() {
    return access((dir() + "/complete.txt").c_str(), F_OK) != 0 && access((dir() + "/offered.txt").c_str(), F_OK) != 0 &&
           !g_running;
}
void mark_offered() {
    mkdir(dir().c_str(), 0777);
    write_file(dir() + "/offered.txt", "1\n");
}

Live live() {
    std::lock_guard<std::mutex> lk(g_mu);
    Live l = g_live;
    l.holdToStop = g_stop ? 1.0f : g_hold.load();
    return l;
}

Progress progress() {
    static const size_t total = places().size();
    Progress p;
    p.total = total;
    p.next = read_index();
    p.complete = access((dir() + "/complete.txt").c_str(), F_OK) == 0;
    return p;
}

std::string screen_line() {
    if (g_running) return status();
    static const bool announce = [] {
        const bool a = access((dir() + "/announce.txt").c_str(), F_OK) == 0;
        if (a) remove((dir() + "/announce.txt").c_str());
        return a;
    }();
    static double since = 0;  // from when the title screen shows (the restart's first picture)
    if (!announce) return {};
    if (since == 0 && at_title()) since = now();
    if (since != 0 && now() - since < 12.0) return ui_text::tx().ready;
    return {};
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running ? g_status : std::string();
}

// a sweep asked for at start-up begins once the title screen has settled (its first warp asks for the play scene, see
// frame()), or in a game in progress if a file was loaded meanwhile
// dStage_nextStage_c, as mods/cheats.cpp warp_service writes it
void write_next_stage(const Place& p) {
    const uint32_t ns = kNextStageReq - 12;
    for (uint32_t i = 0; i < 8; i++) st8(ns + i, uint8_t(i < strlen(p.stage) ? p.stage[i] : 0));
    st16(ns + 8, uint16_t(int16_t(p.point)));
    st8(ns + 10, uint8_t(int8_t(p.room)));
    st8(ns + 11, 0xFF);  // layer: the stage's own choice
    st8(ns + 13, 0);     // wipe: the default fade
    st8(kNextStageReq, 1);  // enabled
}

void request_at_title() {
    g_atTitle = true;
    LOG("[prepare] requested at start-up: starts at the title screen");
}

void frame(Cpu* c) {
    // enemies attack while the sweep waits in a place: Link's health is topped up every frame (as the Mods tab's
    // infinite health, which waits for a loaded file), so a game over does not stop the warps
    if (g_running) {
        const uint32_t sv = ld32(kSaveInfoPtr);
        if (sv >= mem::kMem2Start && sv < mem::kMem2End) {
            const uint16_t max = ld16(sv + 0x20 + 0x00);
            if (max >= 4 && max <= 80) st16(sv + 0x20 + 0x02, max);
        }
    }
    static double readySince = 0;
    if (g_atTitle && !g_running) {
        // the title screen settled, or a game in progress with Link in control (a file loaded meanwhile)
        if (!at_title() && !(in_game() && link_has_control())) readySince = 0;
        else if (readySince == 0) readySince = now();
        else if (now() - readySince > 3.0) {
            g_atTitle = false;
            const std::string why = start();
            if (!why.empty()) LOG("[prepare] could not start: %s", why.c_str());
        }
    }
    if (g_titleWarpPending && !ld8(kNextStageReq) && at_title()) {
        // the title screen does not act on a next-stage request: the play scene is asked for, as the file select does
        Cpu saved = *c;
        if (guest_call(c, fn_ovlpDoingReq) & 0xFF) {  // a fade still runs: next frame
            *c = saved;
            return;
        }
        std::lock_guard<std::mutex> lk(g_titleMu);
        write_next_stage(g_titleWarp);
        const uint32_t scene = guest_call(c, fn_searchById, {ld32(kSceneId)});
        const uint32_t ok = scene ? guest_call(c, fn_changeReq, {scene, kPlayScene, 0, 5, 1}) : 0;
        *c = saved;
        g_titleWarpPending = false;
        LOG("[prepare] from the title screen to %s room %d point %d: play scene asked for (scene %08X, %s)",
            g_titleWarp.stage, g_titleWarp.room, g_titleWarp.point, scene, ok ? "accepted" : "REFUSED");
        return;
    }
    if (g_titleWarpPending && !ld8(kNextStageReq)) {  // as mods/cheats.cpp warp_service
        std::lock_guard<std::mutex> lk(g_titleMu);
        write_next_stage(g_titleWarp);
        g_titleWarpPending = false;
        LOG("[prepare] warp from %s to %s room %d point %d", mods::current_stage().c_str(), g_titleWarp.stage,
            g_titleWarp.room, g_titleWarp.point);
    }
}

void startup_resume() {
    if (access((dir() + "/resume.txt").c_str(), F_OK) != 0) return;
    remove((dir() + "/resume.txt").c_str());
    request_at_title();
}

void startup() {
    startup_resume();
    if (restore_save()) LOG("[prepare] a sweep was cut (the game closed during it): Quest Log files put back");
}

}  // namespace prepare_graphics
