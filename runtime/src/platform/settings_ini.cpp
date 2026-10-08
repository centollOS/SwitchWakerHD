// settings.ini's text (settings_ini.h): parsed, written again, and env.txt converted into it.
#include "settings_ini.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "settings_switch.h"

namespace settings_ini {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        at = end + 1;
    }
    return out;
}
bool is_header(const std::string& line) {
    const std::string t = trim(line);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}
std::string header_name(const std::string& line) {
    const std::string t = trim(line);
    return trim(t.substr(1, t.size() - 2));
}
// "KEY=VALUE" (not a comment); false otherwise
bool key_value(const std::string& line, std::string& key, std::string& value) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') return false;
    const size_t eq = t.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    key = trim(t.substr(0, eq));
    value = trim(t.substr(eq + 1));
    return !key.empty();
}

// ---- the variables the menu's settings stand for, and how a value converts (false: it cannot)
using Settings = std::vector<std::pair<std::string, std::string>>;
bool on_atoi(const std::string& v) { return atoi(v.c_str()) != 0; }
std::string both_modes(const char* key, int m) { return std::string(key) + (m ? ".docked" : ".handheld"); }

struct MenuVariable {
    const char* env;
    bool (*convert)(const std::string& value, Settings& out);
};
const MenuVariable kMenuVariables[] = {
    {"WWHD_DEBUG_SERVER", [](const std::string& v, Settings& o) {  // (debug_switch.cpp: any value but 0; port 6543)
         o.push_back({switch_settings::kKeyDebugServer, !v.empty() && v != "0" ? "1" : "0"});
         return true;
     }},
    {"WWHD_MAIN_SAMPLER", [](const std::string& v, Settings& o) {  // (threads.cpp)
         o.push_back({switch_settings::kKeyMainSampler, v.empty() || v[0] != '0' ? "1" : "0"});
         return true;
     }},
    {"WWHD_GPU_PROFILE", [](const std::string& v, Settings& o) {
         for (const char* id : {"default", "384", "460", "1600", "614"})
             if (v == id) {
                 o.push_back({switch_settings::kKeyGpuProfile, v});
                 return true;
             }
         return false;  // (a configuration id: not a menu choice)
     }},
    {"WWHD_CPU_CLOCK", [](const std::string& v, Settings& o) {
         for (const char* id : {"1020", "1122", "1224", "1326", "1428", "1581", "1683", "1785"})
             if (v == id) {
                 o.push_back({switch_settings::kKeyCpuClock, v});
                 return true;
             }
         return false;
     }},
    {"WWHD_FPS", [](const std::string& v, Settings& o) {
         o.push_back({switch_settings::kKeyFpsCounter, std::to_string(std::clamp(atoi(v.c_str()), 0, 2))});
         return true;
     }},
    {"WWHD_EXPOSURE", [](const std::string& v, Settings& o) { o.push_back({switch_settings::kKeyExposure, v}); return true; }},
    {"WWHD_CONTRAST", [](const std::string& v, Settings& o) { o.push_back({switch_settings::kKeyContrast, v}); return true; }},
    {"WWHD_SATURATION", [](const std::string& v, Settings& o) { o.push_back({switch_settings::kKeySaturation, v}); return true; }},
    {"WWHD_GAMMA", [](const std::string& v, Settings& o) { o.push_back({switch_settings::kKeyGamma, v}); return true; }},
    {"WWHD_RES_SCALE", [](const std::string& v, Settings& o) {  // (it fixed the scale in both modes)
         const double s = atof(v.c_str());
         if (!(s > 0)) return false;
         char b[16];
         snprintf(b, sizeof b, "%.2f", std::clamp(s, 0.5, 2.0));
         for (int m = 0; m < 2; m++) o.push_back({both_modes(switch_settings::kKeyResScale, m), b});
         return true;
     }},
    {"WWHD_DYNAMIC_RES", [](const std::string& v, Settings& o) {  // (settings_switch.cpp's reading)
         const bool on = !(atof(v.c_str()) == 0.0 && v[0] == '0');
         for (int m = 0; m < 2; m++) o.push_back({both_modes(switch_settings::kKeyDynamicRes, m), on ? "1" : "0"});
         return true;
     }},
    {"WWHD_ANISO", [](const std::string& v, Settings& o) { o.push_back({switch_settings::kKeyAniso, on_atoi(v) ? "1" : "0"}); return true; }},
    {"WWHD_RUMBLE", [](const std::string& v, Settings& o) { o.push_back({"rumble", on_atoi(v) ? "1" : "0"}); return true; }},
    {"WWHD_PRO_CONTROLLER", [](const std::string& v, Settings& o) {  // (input_switch.cpp)
         o.push_back({"proController", v != "0" ? "1" : "0"});
         return true;
     }},
    {"WWHD_GYRO", [](const std::string& v, Settings& o) { o.push_back({"gyro.source", v}); return true; }},
    {"WWHD_LANGUAGE", [](const std::string& v, Settings& o) { o.push_back({"language", v}); return true; }},
    {"WWHD_LANGUAGE_REGION", [](const std::string& v, Settings& o) { o.push_back({"language_region", v}); return true; }},
    {"WWHD_MOD_CAMERA_SPEED", [](const std::string& v, Settings& o) { o.push_back({"mod.direct-camera.speed", v}); return true; }},
    {"WWHD_MOD_MOUSE_SENS", [](const std::string& v, Settings& o) { o.push_back({"mod.mouse-camera.sensitivity", v}); return true; }},
};
const ModVariable* mod_of(const std::string& name) {
    for (const ModVariable& m : kModVariables)
        if (name == m.env) return &m;
    return nullptr;
}
const MenuVariable* menu_variable(const std::string& name) {
    for (const MenuVariable& m : kMenuVariables)
        if (name == m.env) return &m;
    return nullptr;
}

}  // namespace

const ModVariable kModVariables[6] = {
    {"direct-camera", "WWHD_MOD_DIRECT_CAMERA"}, {"mouse-camera", "WWHD_MOD_MOUSE_CAMERA"},
    {"first-person", "WWHD_MOD_FIRST_PERSON"},   {"wall-climb", "WWHD_CLIMB"},
    {"quick-doors", "WWHD_MOD_QUICK_DOORS"},     {"fast-scenes", "WWHD_MOD_FAST_SCENES"},
};

File parse(const std::string& text) {
    File f;
    bool inSections = false;
    for (const std::string& line : lines_of(text)) {
        if (!inSections && is_header(line)) inSections = true;
        if (inSections) {
            f.sections.push_back(line);
            continue;
        }
        std::string k, v;
        if (key_value(line, k, v)) f.menu[k] = v;
    }
    // (blank lines at the end of the file are not kept: format writes one newline per line)
    while (!f.sections.empty() && trim(f.sections.back()).empty()) f.sections.pop_back();
    return f;
}

std::string format(const std::map<std::string, std::string>& menu, const std::vector<std::string>& sections) {
    std::string out = "# Wind Waker HD settings\n";
    for (const auto& [k, v] : menu) out += k + '=' + v + '\n';
    if (!sections.empty()) {
        out += '\n';
        for (const std::string& line : sections) out += line + '\n';
    }
    return out;
}
std::string format(const File& f) { return format(f.menu, f.sections); }

std::vector<std::pair<std::string, std::string>> dev_variables(const File& f) {
    std::vector<std::pair<std::string, std::string>> out;
    bool dev = false;
    for (const std::string& line : f.sections) {
        if (is_header(line)) {
            dev = header_name(line) == "dev";
            continue;
        }
        std::string k, v;
        if (dev && key_value(line, k, v)) out.push_back({k, v});
    }
    return out;
}

void add_dev_variable(File& f, const std::string& name, const std::string& value) {
    // after the [dev] section's last KEY=VALUE line (or its header)
    size_t insertAt = std::string::npos;
    bool dev = false;
    for (size_t i = 0; i < f.sections.size(); i++) {
        const std::string& line = f.sections[i];
        if (is_header(line)) {
            dev = header_name(line) == "dev";
            if (dev) insertAt = i + 1;
            continue;
        }
        std::string k, v;
        if (dev && key_value(line, k, v)) insertAt = i + 1;
    }
    if (insertAt == std::string::npos) {
        f.sections.push_back("[dev]");
        insertAt = f.sections.size();
    }
    f.sections.insert(f.sections.begin() + long(insertAt), name + '=' + value);
}

bool menu_backed(const std::string& name) { return menu_variable(name) || mod_of(name); }

Migration migrate_env_txt(const std::string& envText, File& f) {
    Migration m;
    for (const std::string& raw : lines_of(envText)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == '-') continue;  // comments and --options: dropped
        std::string k, v;
        if (!key_value(line, k, v)) continue;
        if (const ModVariable* mod = mod_of(k)) {
            const bool on = on_atoi(v);  // (mods.cpp, climb.cpp: atoi != 0)
            f.menu[std::string("mod.") + mod->id + ".enabled"] = on ? "1" : "0";
            m.mods.push_back({mod->id, on});
            m.log.push_back(k + '=' + v + " -> mod." + mod->id + ".enabled=" + (on ? "1" : "0"));
        } else if (const MenuVariable* menu = menu_variable(k)) {
            Settings out;
            if (!menu->convert(v, out)) {
                m.log.push_back(k + '=' + v + ": not a value the menu offers, dropped");
                continue;
            }
            std::string what;
            for (const auto& [key, value] : out) {
                f.menu[key] = value;
                what += (what.empty() ? "" : ", ") + key + '=' + value;
            }
            m.log.push_back(k + '=' + v + " -> " + what);
        } else {
            add_dev_variable(f, k, v);
            m.log.push_back(k + '=' + v + " -> [dev]");
        }
    }
    return m;
}

}  // namespace settings_ini
