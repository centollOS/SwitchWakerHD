#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
namespace gfxvk {
struct DeviceInfo {
    std::string name, type, identity, driver, version, unavailable;
    unsigned vendor = 0, device = 0;
    bool core = false;
    bool renderPasses = false;
    bool usable() const { return unavailable.empty(); }
};
struct DeviceChoice { int index = -1; std::string reason = "automatic: first usable", fallback; };
inline DeviceChoice choose_device(const std::vector<DeviceInfo>& devices, const std::string& saved,
                                  const std::string& env) {
    DeviceChoice result;
    for (int path = 0; path < 3; ++path) {
        for (size_t i = 0; i < devices.size(); ++i)
            if (result.index < 0 && devices[i].usable() &&
                (devices[i].core ? 0 : devices[i].renderPasses ? 2 : 1) == path) result.index = int(i);
    }
    if (env.empty() && saved.empty()) return result;
    int selected = -1;
    if (!env.empty()) {
        const bool numeric = std::all_of(env.begin(), env.end(), [](unsigned char c) { return std::isdigit(c); });
        if (numeric) {
            char* end = nullptr;
            unsigned long i = std::strtoul(env.c_str(), &end, 10);
            if (i < devices.size()) selected = int(i);
        } else {
            auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
            for (size_t i = 0; i < devices.size(); ++i)
                if (lower(devices[i].name).find(lower(env)) != std::string::npos) { selected = int(i); break; }
        }
    } else {
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].identity == saved) { selected = int(i); break; }
    }
    if (selected >= 0 && devices[selected].usable()) {
        result.index = selected;
        result.reason = env.empty() ? "setting" : "env";
    } else {
        result.fallback = selected < 0 ? "The chosen GPU is not available" :
            "The chosen GPU is not available (" + devices[selected].unavailable + ")";
    }
    return result;
}
// Startup publishes the inventory before the overlay starts; only the saved choice changes at runtime.
inline std::vector<DeviceInfo> device_inventory;
inline std::string saved_device, device_fallback, active_device;
inline std::mutex device_setting_mutex;
inline std::string device_setting() { std::lock_guard<std::mutex> lock(device_setting_mutex); return saved_device; }
inline void set_device_setting(const std::string& identity) { std::lock_guard<std::mutex> lock(device_setting_mutex); saved_device = identity; }
inline bool device_from_env() { const char* e = std::getenv("WWHD_VK_DEVICE"); return e && *e; }
}
