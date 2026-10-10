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

namespace gfxdk { uint64_t shaders_pending(); }  // gfx/deko/dk_shaders.h (without the decompiler's headers)

namespace prepare_graphics {
namespace {

constexpr double kArriveTimeout = 30.0;   // s: a destination not reached by then is skipped
constexpr double kSettle = 3.0;           // s after the arrival before the worker is watched (draws meet shaders)
constexpr double kQuiet = 2.0;            // s with nothing pending: this place is done
constexpr double kPlaceTimeout = 60.0;    // s at most in one place
// Warping out while a place's arrival event still runs stopped the game (c_xyz.cpp:285 isNearZeroSquare: M2tower ->
// M_DaiB after 10 s): the next warp waits until Link has had control for a moment (no event, message, game menu or
// stage change, Link the controlled actor: the game's own pause-menu conditions, savestate.cpp player_has_control),
// after at least kMinStay; places known to have a long arrival event stay at least kMinStayEvent anyway.
constexpr double kMinStay = 5.0;
constexpr double kMinStayEvent = 12.0;
constexpr double kControlFor = 1.0;       // s of control in a row
constexpr double kControlTimeout = 45.0;  // s: then the warp goes anyway (logged)
constexpr double kHoldToStop = 1.5;       // s of B held on the loading screen
const char* const kArrivalEvents[] = {"M2tower"};
constexpr uint32_t kMaxBackup = 64 << 10; // save/user files up to this size are copied (not the pictures)

const uint32_t kNextStageReq = GD(0x1046F0B0) + 0x5140 + 12;  // mods/cheats.cpp kNextStage + 12: a request is set
const uint32_t kOverlap = GD(0x101F36CC);                      // a scene change's fade is running (mods/turbo.cpp)
// savestate.cpp player_has_control's fields of g_dComIfG_gameInfo.play
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
std::atomic<bool> g_atTitle{false};  // start once the title screen shows
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
    set_status(std::string("Preparing graphics: ") + why + ". The game restarts.");
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
    set_status("Preparing graphics: done. Close the game and start it again (your Quest Log is as it was).");
}

void sweep() {
    const std::vector<Place> list = places();
    size_t i = read_index();
    size_t done = 0, skipped = 0;
    const double t0 = now();
    for (; i < list.size() && !g_stop; i++) {
        const Place& p = list[i];
        char b[200], eta[48] = "";
        if (done >= 5) {  // the minutes left, from this sweep's own pace
            const double left = (now() - t0) / done * double(list.size() - i);
            snprintf(eta, sizeof eta, ", about %.0f min left", left / 60 < 1 ? 1.0 : left / 60);
        }
        snprintf(b, sizeof b, "Preparing graphics: %zu of %zu (%s)%s. To stop: hold B.", i + 1,
                 list.size(), p.stage, eta);
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
        if (!arrived(p)) {
            // a warp the game did not take is often stuck in its scene change: later warps would wait for it
            LOG("[prepare] %s room %d not reached in %.0f s (stage %s): continuing after a restart", p.stage, p.room,
                kArriveTimeout, mods::current_stage().c_str());
            write_index(i + 1);
            skipped++;
            finish_and_restart("a place did not load; start it again to continue");
            return;
        }
        const double at = now();
        sleep_s(kSettle);
        double quietSince = 0;
        while (!g_stop && now() - at < kPlaceTimeout) {
            if (gfxdk::shaders_pending() == 0) {
                if (quietSince == 0) quietSince = now();
                if (now() - quietSince >= kQuiet) break;
            } else {
                quietSince = 0;
            }
            sleep_s(0.25);
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
                LOG("[prepare] %s: Link had no control for %.0f s (an event?): going on", p.stage, kControlTimeout);
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
    finish_and_restart(complete ? "complete" : "stopped (start it again to continue)");
}

}  // namespace

std::string start() {
    if (g_running) return "already running";
    if (!in_game() && !at_title()) return "start a game first (your Quest Log, or a new game you do not save)";
    mkdir(dir().c_str(), 0777);
    if (!backup_save()) return "could not copy the Quest Log files (is the SD card full?)";
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
    if (since != 0 && now() - since < 12.0) return "Graphics ready: the whole game has been prepared on this console.";
    return {};
}
std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running ? g_status : std::string();
}

void request_at_title() {
    g_atTitle = true;
    LOG("[prepare] requested at start-up: starts once the title screen shows");
}

void frame() {
    // enemies attack while the sweep waits in a place: Link's health is topped up every frame (as the Mods tab's
    // infinite health, which waits for a loaded file), so a game over does not stop the warps
    if (g_running) {
        const uint32_t sv = ld32(kSaveInfoPtr);
        if (sv >= mem::kMem2Start && sv < mem::kMem2End) {
            const uint16_t max = ld16(sv + 0x20 + 0x00);
            if (max >= 4 && max <= 80) st16(sv + 0x20 + 0x02, max);
        }
    }
    static double titleSince = 0;
    if (g_atTitle && !g_running) {
        if (!at_title()) titleSince = 0;
        else if (titleSince == 0) titleSince = now();
        else if (now() - titleSince > 4.0) {  // the title screen has settled
            g_atTitle = false;
            const std::string why = start();
            if (!why.empty()) LOG("[prepare] could not start from the title screen: %s", why.c_str());
        }
    }
    if (g_titleWarpPending && !ld8(kNextStageReq)) {  // as mods/cheats.cpp warp_service
        std::lock_guard<std::mutex> lk(g_titleMu);
        for (uint32_t i = 0; i < 8; i++) st8(kNextStageReq - 12 + i, uint8_t(i < strlen(g_titleWarp.stage) ? g_titleWarp.stage[i] : 0));
        st16(kNextStageReq - 12 + 8, uint16_t(int16_t(g_titleWarp.point)));
        st8(kNextStageReq - 12 + 10, uint8_t(int8_t(g_titleWarp.room)));
        st8(kNextStageReq - 12 + 11, 0xFF);  // layer: the stage's own choice
        st8(kNextStageReq - 12 + 13, 0);     // wipe: the default fade
        st8(kNextStageReq, 1);               // enabled
        g_titleWarpPending = false;
        LOG("[prepare] warp from %s to %s room %d point %d", mods::current_stage().c_str(), g_titleWarp.stage,
            g_titleWarp.room, g_titleWarp.point);
    }
}

void startup() {
    if (restore_save()) LOG("[prepare] a sweep was cut (the game closed during it): Quest Log files put back");
}

}  // namespace prepare_graphics
