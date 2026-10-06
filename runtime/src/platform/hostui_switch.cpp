// The parts of the settings-overlay host interface (overlay/hostui.h) that runtime code outside the
// overlay uses on the Switch: saved settings (console language, mod preferences) and post(). The
// Switch build draws no settings overlay, so the window and display options are not implemented.
#include "overlay/hostui.h"

#include <fstream>
#include <map>
#include <mutex>

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

// no UI thread to hand work to: run it now (only overlay code posts, and it is inactive here)
void post(std::function<void()> fn) { fn(); }

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

}  // namespace hostui
