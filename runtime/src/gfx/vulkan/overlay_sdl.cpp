// Settings overlay on the SDL host (Vulkan-only builds: Windows, Linux): hostui.h on top of the SDL
// windows and the Vulkan renderer's settings. Options are kept in <config dir>/settings.ini
// (key=value lines; WWHD_SETTINGS names another file; test runs with WWHD_NO_HOST_INPUT use none).
#ifdef WWHD_SDL_HOST
#include "backend.h"
#include "settings.h"
#include "input.h"
#include "overlay/hostui.h"
#include "platform/host.h"
#include "runtime.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

namespace interp { int mode(); void set_mode(int m); }

namespace hostui {
namespace {
std::mutex g_mu;
std::vector<std::function<void()>> g_posted;
std::map<std::string, std::string> g_values;
bool g_loaded = false;

std::string path() {
    if (const char* e = getenv("WWHD_SETTINGS")) return e;
    if (getenv("WWHD_NO_HOST_INPUT")) return {};  // test runs leave the user's settings alone
    return host::config_dir() + "/settings.ini";
}
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    std::string p = path();
    if (p.empty()) return;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) g_values[line.substr(0, eq)] = line.substr(eq + 1);
    }
}
void save_locked() {
    std::string p = path();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    std::string tmp = p + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "# Wind Waker HD settings (settings overlay, F1)\n";
        for (auto& [k, v] : g_values) out << k << '=' << v << '\n';
        if (!out) return;
    }
    if (!host::replace_file(tmp, p)) LOG("[settings] cannot write %s", p.c_str());
}
bool env_set(std::initializer_list<const char*> env) {
    for (const char* e : env)
        if (getenv(e)) return true;
    return false;
}
}  // namespace

void post(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_posted.push_back(std::move(fn));
}
// SDL main loop (backend.cpp run_main_loop)
void run_posted() {
    std::vector<std::function<void()>> fns;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        fns.swap(g_posted);
    }
    for (auto& f : fns) f();
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
    save_locked();
}

float res_scale() { return gfxvk::requested_res_scale(); }
void set_res_scale(float s) { gfxvk::set_res_scale(s); }
// the graphics options are saved as the AppKit host saves them (an option's WWHD_* variable wins and
// is not saved)
void graphics_changed() {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    auto put = [&](const char* key, std::initializer_list<const char*> env, const std::string& v) {
        if (!env_set(env)) g_values[key] = v;
    };
    char res[16];
    snprintf(res, sizeof res, "%g", gfxvk::requested_res_scale());
    put("resScale", {"WWHD_RES_SCALE"}, res);
    put("aoMode", {"WWHD_AO_MODE", "WWHD_NO_AO_QUIRK"}, std::to_string(gfxvk::ao_mode()));
    put("aoHires", {"WWHD_AO_HIRES"}, gfxvk::ao_hires_enabled() ? "1" : "0");
    put("aniso", {"WWHD_ANISO"}, gfxvk::aniso_enabled() ? "1" : "0");
    put("fxaa", {"WWHD_FXAA"}, gfxvk::fxaa_enabled() ? "1" : "0");
    put("fps60", {"WWHD_INTERP", "WWHD_TRUE60"}, std::to_string(interp::mode()));
    put("scaleFilter", {"WWHD_SCALE_FILTER"}, std::to_string(gfxvk::scale_filter()));
    put("vkPresentMode", {"WWHD_VK_PRESENT_MODE"}, std::to_string(gfxvk::present_mode()));
    save_locked();
}
// start-up (backend.cpp init): the saved graphics options
void load_saved_options() {
    std::map<std::string, std::string> v;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        load_locked();
        v = g_values;
    }
    auto saved = [&](const char* key, std::initializer_list<const char*> env) { return !env_set(env) && v.count(key); };
    auto num = [&](const char* key) { return atof(v[key].c_str()); };
    if (saved("resScale", {"WWHD_RES_SCALE"})) gfxvk::set_res_scale((float)num("resScale"));
    if (saved("aoMode", {"WWHD_AO_MODE", "WWHD_NO_AO_QUIRK"})) gfxvk::set_ao_mode((int)num("aoMode"));
    if (saved("aoHires", {"WWHD_AO_HIRES"})) gfxvk::set_ao_hires(num("aoHires") != 0);
    if (saved("aniso", {"WWHD_ANISO"})) gfxvk::set_aniso(num("aniso") != 0);
    if (saved("fxaa", {"WWHD_FXAA"})) gfxvk::set_fxaa(num("fxaa") != 0);
    if (saved("fps60", {"WWHD_INTERP", "WWHD_TRUE60"})) interp::set_mode((int)num("fps60"));
    if (saved("scaleFilter", {"WWHD_SCALE_FILTER"})) gfxvk::set_scale_filter((int)num("scaleFilter"));
    if (saved("vkPresentMode", {"WWHD_VK_PRESENT_MODE"})) gfxvk::set_present_mode((int)num("vkPresentMode"));
}

int scale_filter() { return gfxvk::scale_filter(); }
void set_scale_filter(int f) {
    gfxvk::set_scale_filter(f);
    graphics_changed();
}
bool scale_filter_available() { return gfxvk::graphics_feature_available(gfxvk::GraphicsFeature::ScaleFilter); }

bool fullscreen() { return gfxvk::R.tv.window && (SDL_GetWindowFlags(gfxvk::R.tv.window) & SDL_WINDOW_FULLSCREEN); }
void set_fullscreen(bool on) {
    if (!gfxvk::R.tv.window) return;
    if (!SDL_SetWindowFullscreen(gfxvk::R.tv.window, on)) LOG("[display] TV window: full screen %s failed: %s", on ? "on" : "off", SDL_GetError());
}
// one GamePad window, shown or hidden (no picture-in-picture on this host)
int drc_modes() { return 0; }
int drc_mode() { return 0; }
void set_drc_mode(int) {}
bool drc_available() { return gfxvk::R.drc.window != nullptr; }
bool drc_shown() { return gfxvk::R.drc.window && !(SDL_GetWindowFlags(gfxvk::R.drc.window) & SDL_WINDOW_HIDDEN); }
void show_drc(bool on) {
    if (!gfxvk::R.drc.window) return;
    if (on) SDL_ShowWindow(gfxvk::R.drc.window);
    else SDL_HideWindow(gfxvk::R.drc.window);
    gfxvk::R.drc.visible = on;
    LOG("[display] GamePad window %s", on ? "shown" : "hidden");
}
void set_pro_controller(bool on) { input::set_pro_controller(on); }
const char* name() { return "SDL"; }

}  // namespace hostui
#endif  // WWHD_SDL_HOST
