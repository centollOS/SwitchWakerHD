// Prepare graphics (prepare_graphics_switch.h, docs/prepare-graphics-plan.md).
//
// A thread walks mods/warps.h (kMainWarps then kAllWarps, each stage/room once): it asks for the warp (the game's
// own scene change, mods::request_warp), waits for the arrival (no warp pending, no fade, no next-stage request, the
// stage named), then waits until the deko3d shader worker has had nothing pending for a moment (or 60 s), and goes on.
// The next index is kept in prepare_graphics/state.txt, so a sweep stopped or cut continues where it was.
//
// Link has infinite health during the sweep (mods/cheats.cpp, as the Mods tab's), so enemies in a place cannot end it.
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
constexpr uint32_t kMaxBackup = 64 << 10; // save/user files up to this size are copied (not the pictures)

const uint32_t kNextStageReq = GD(0x1046F0B0) + 0x5140 + 12;  // mods/cheats.cpp kNextStage + 12: a request is set
const uint32_t kOverlap = GD(0x101F36CC);                      // a scene change's fade is running (mods/turbo.cpp)

struct Place {
    const char* stage;
    int room, point;
};

std::mutex g_mu;            // g_status
std::string g_status;
std::atomic<bool> g_running{false}, g_stop{false};
bool g_hadInfiniteHealth = false;  // the player's own setting, put back at the end

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

bool arrived(const Place& p) {
    return !mods::warp_pending() && !ld8(kNextStageReq) && !ld32(kOverlap) && mods::current_stage() == p.stage;
}

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

void finish_and_restart(const char* why) {
    if (!g_hadInfiniteHealth) mods::set_infinite(mods::kInfHealth, false);
    set_status(std::string("Preparing graphics: ") + why + ". The game restarts.");
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
        char b[160];
        snprintf(b, sizeof b, "Preparing graphics: %zu/%zu (%s). To stop: hold Minus, Switch tab.", i + 1, list.size(), p.stage);
        set_status(b);
        mods::request_warp(p.stage, p.room, p.point);
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
        LOG("[prepare] %zu/%zu %s room %d: %.0f s", i + 1, list.size(), p.stage, p.room, now() - at);
        done++;
        write_index(i + 1);
    }
    const bool complete = i >= list.size();
    LOG("[prepare] %s: %zu places in %.0f min, %zu skipped", complete ? "complete" : "stopped", done, (now() - t0) / 60,
        skipped);
    if (complete) write_index(0);
    finish_and_restart(complete ? "complete" : "stopped (start it again to continue)");
}

}  // namespace

std::string start() {
    if (g_running) return "already running";
    if (!in_game()) return "start a game first (your Quest Log, or a new game you do not save)";
    mkdir(dir().c_str(), 0777);
    if (!backup_save()) return "could not copy the Quest Log files (is the SD card full?)";
    // enemies attack while the sweep waits in a place: with health topped up every frame, Link does not die (a
    // game over would stop the warps)
    g_hadInfiniteHealth = mods::infinite(mods::kInfHealth);
    mods::set_infinite(mods::kInfHealth, true);
    g_stop = false;
    g_running = true;
    std::thread([] {
        sweep();
        g_running = false;
    }).detach();
    return "";
}

void stop() { g_stop = true; }
bool running() { return g_running; }
std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running ? g_status : std::string();
}

void startup() {
    if (restore_save()) LOG("[prepare] a sweep was cut (the game closed during it): Quest Log files put back");
}

}  // namespace prepare_graphics
