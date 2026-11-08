// (this fork) Brazilian Portuguese from the player's own fan translation (fan_translation.h).
#include "fan_translation.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#include "runtime.h"

namespace fan_translation {

const char* const kName = "Portugu\xC3\xAAs (Brasil)";

namespace {
namespace fs = std::filesystem;

std::mutex g_mu;
bool g_looked = false;
bool g_present = false;
std::string g_path;
std::atomic<bool> g_active{false};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

void look() {  // under g_mu
    if (g_looked) return;
    g_looked = true;
    if (const char* e = getenv("WWHD_PTBR_PACK"); e && *e) {
        g_path = e;
    } else {
        fs::path game = fs::path(config::game_dir);
        if (game.filename().empty()) game = game.parent_path();  // "sdmc:/switch/wwhd/game/"
        g_path = (game.parent_path() / "translation" / "pt-BR" / "permanent_2d_ptBR.pack").string();
    }
    char magic[4] = {};
    if (FILE* f = fopen(g_path.c_str(), "rb")) {
        g_present = fread(magic, 1, 4, f) == 4 && std::equal(magic, magic + 4, "SARC");
        fclose(f);
        if (g_present) LOG("[config] Brazilian Portuguese translation: %s", g_path.c_str());
        else LOG("[config] %s is not a pack (no SARC header): the Portuguese translation is not offered", g_path.c_str());
    }
}

}  // namespace

bool present() {
    std::lock_guard<std::mutex> lk(g_mu);
    look();
    return g_present;
}

const std::string& pack_path() {
    std::lock_guard<std::mutex> lk(g_mu);
    look();
    return g_path;
}

int game_language(int wanted) {
    if (wanted != kLanguage || !present()) return wanted;
    g_active.store(true, std::memory_order_release);
    LOG("[config] %s (the fan translation): the game runs in English with %s", kName, pack_path().c_str());
    return 1;
}

bool active() { return g_active.load(std::memory_order_acquire); }

int shown_language(int started) { return active() ? kLanguage : started; }

int default_language(int fallback) { return present() ? kLanguage : fallback; }

std::string redirect(const std::string& guest) {
    if (!active()) return {};
    std::string g = lower(guest);
    std::replace(g.begin(), g.end(), '\\', '/');
    const size_t slash = g.rfind('/');
    const std::string file = slash == std::string::npos ? g : g.substr(slash + 1);
    if (file != "permanent_2d_usenglish.pack" && file != "permanent_2d_euenglish.pack") return {};
    return pack_path();
}

void reset_for_test() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_looked = g_present = false;
    g_path.clear();
    g_active.store(false);
}

}  // namespace fan_translation
