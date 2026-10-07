// The settings overlay's host interface (overlay/hostui.h) on the Switch: saved settings
// (sdmc:/switch/wwhd/settings.ini), post() (run by the host loop, gfx/gl/backend.cpp run_main_loop),
// and the window and display options, which the Switch does not have (one screen, always full: the
// overlay hides those tabs; the values here only answer the questions).
#include "overlay/hostui.h"

#include <deque>
#include <fstream>
#include <map>
#include <mutex>

#include "input.h"
#include "platform/host.h"
#include "runtime.h"

namespace hostui {
namespace {
std::mutex g_mu;
std::map<std::string, std::string> g_values;
bool g_loaded = false;

// sdmc:/switch/wwhd/settings.ini, KEY=VALUE lines (the same format as the SDL host's file)
std::string path() { return host::config_dir() + "/settings.ini"; }

void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream in(path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) g_values[line.substr(0, eq)] = line.substr(eq + 1);
    }
}
}  // namespace

namespace {
std::mutex g_post_mu;
std::deque<std::function<void()>> g_posted;
}  // namespace

// the overlay builds its UI on the render thread; its changes run on the host loop's thread
void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_post_mu);
    g_posted.push_back(std::move(fn));
}

void run_posted() {
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lk(g_post_mu);
            if (g_posted.empty()) return;
            fn = std::move(g_posted.front());
            g_posted.pop_front();
        }
        fn();
    }
}

bool get(const char* key, std::string& value) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    auto it = g_values.find(key);
    if (it == g_values.end()) return false;
    value = it->second;
    return true;
}

void set(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    g_values[key] = value;
    std::string p = path(), tmp = p + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# Wind Waker HD settings\n";
        for (auto& [k, v] : g_values) out << k << '=' << v << '\n';
        if (!out) return;
    }
    if (!host::replace_file(tmp, p)) LOG("[settings] cannot write %s", p.c_str());
}

const char* name() { return "Switch"; }

// no file picker on the console: mods are copied into sdmc:/switch/wwhd/mods by hand
void choose_mod_source(bool, std::function<void(std::string)>) {}

// graphics options of the desktop hosts: the Switch renderer has its own (Switch tab)
float res_scale() { return 1.0f; }
void set_res_scale(float) {}
void graphics_changed() {}
int scale_filter() { return 0; }
void set_scale_filter(int) {}
bool scale_filter_available() { return false; }

// one screen, always full
bool fullscreen() { return true; }
void set_fullscreen(bool) {}
int drc_modes() { return 0; }
bool drc_mode_offered(int) { return false; }
int drc_mode() { return 3; }  // off: the GamePad picture has no screen on the Switch
void set_drc_mode(int) {}
int pip_corner() { return 1; }
void set_pip_corner(int) {}
float pip_size() { return 0.25f; }
void set_pip_size(float) {}
float pip_opacity() { return 1.0f; }
void set_pip_opacity(float) {}
bool drc_available() { return false; }
bool drc_shown() { return false; }
void show_drc(bool) {}
void set_pro_controller(bool on) { input::set_pro_controller(on); }
void load_saved_options() {}
void toggle_drc() {}
void drc_window_closed() {}
void tv_fullscreen_changed() {}

}  // namespace hostui
