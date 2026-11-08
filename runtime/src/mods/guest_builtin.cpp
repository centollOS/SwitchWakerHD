// SwitchWakerHD (this fork): guest mods built into the NRO (guest_builtin.h, docs/guest-mods-switch-plan.md).
#include "guest_builtin.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <span>
#include <filesystem>
#include <iterator>
#include <map>

#include "mod_json.h"
#include "packages.h"
#include "runtime.h"
#include "wwhd_guest_builtin.h"
#include "../platform/host.h"

// WWHD_RECOMP_MOD_HOOKS_ONLY game code registers the functions that have the check (fork_passes.py
// fork_mod_table); --mod-hooks code (every function) and code without mod hooks register nothing
static unsigned g_checked_count = 0;
static const uint32_t* g_checked = nullptr;
extern "C" void ppc_mod_checked(unsigned count, const uint32_t* addrs) {
    g_checked_count = count;
    g_checked = addrs;
}
extern "C" unsigned g_mod_hook_count;

namespace guestbuiltin {

static std::map<std::string, std::string> g_status;  // load()'s outcome per mod: "" loaded or switched off
static std::vector<std::string> g_loaded;             // the mods load() installed

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string{std::istreambuf_iterator<char>(in), {}} : std::string();
}
static std::string mods_dir() { return host::config_dir() + "/mods"; }
static std::string mod_dir(const char* id) { return mods_dir() + "/" + id; }  // its prepared data (data/)

#if WWHD_GUEST_BUILTIN
static const WWHDGuestBuiltin* begin() { return g_guest_builtin; }
static const WWHDGuestBuiltin* end() { return g_guest_builtin + g_guest_builtin_count; }
#else
static const WWHDGuestBuiltin* begin() { return nullptr; }
static const WWHDGuestBuiltin* end() { return nullptr; }
#endif
static const WWHDGuestBuiltin* find(const std::string& id) {
    for (const WWHDGuestBuiltin* b = begin(); b != end(); ++b)
        if (id == b->id) return b;
    return nullptr;
}

// (#121) a built-in mod's settings, one file on the SD card: mods/<id>-options.json, {"enabled": bool, "options": {...}}.
// The game writes it the first time from the NRO's defaults (guest_mods.py: the manifest's, the Switch's, --option and
// --guest-mod-off), so a new build never overwrites what the player chose. Settings of earlier builds (mods/<id>/
// options.json and enabled) are taken over once. An option the file lacks (a newer mod) takes the NRO's default.
static std::string settings_path(const std::string& id) { return mods_dir() + "/" + id + "-options.json"; }
struct Settings {
    bool enabled = true;
    mods::json::Value options;
};
static const WWHDGuestBuiltin* find(const std::string& id);
static mods::json::Value parse_or_null(const std::string& text) {
    try {
        return text.empty() ? mods::json::Value() : mods::json::parse(text);
    } catch (const std::exception&) {
        return {};
    }
}
static bool write_settings(const std::string& id, const Settings& s, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(mods_dir(), ec);
    mods::json::Value all;
    all["enabled"] = mods::json::Value(s.enabled);
    all["options"] = s.options;
    if (s.options.type != mods::json::Value::Object) all["options"].type = mods::json::Value::Object;
    const std::string path = settings_path(id), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << mods::json::dump(all) << '\n';
        f.close();
        if (!f) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    if (!host::replace_file(tmp, path)) {
        error = "cannot save " + path;
        return false;
    }
    return true;
}
static Settings read_settings(const WWHDGuestBuiltin& b) {
    const mods::json::Value defaults = parse_or_null(b.defaults ? b.defaults : "");
    Settings s;
    s.enabled = defaults.get("enabled").type != mods::json::Value::Bool || defaults.get("enabled").boolean;
    s.options = defaults.get("options");
    if (s.options.type != mods::json::Value::Object) s.options.type = mods::json::Value::Object;
    const mods::json::Value saved = parse_or_null(read_file(settings_path(b.id)));
    if (saved.type == mods::json::Value::Object) {
        if (saved.get("enabled").type == mods::json::Value::Bool) s.enabled = saved.get("enabled").boolean;
        for (const auto& [k, v] : saved.get("options").object) s.options[k] = v;
        return s;
    }
    // the first start of this build: an earlier build's settings, else the defaults; written either way
    const std::string old = mod_dir(b.id);
    const mods::json::Value old_options = parse_or_null(read_file(old + "/options.json"));
    for (const auto& [k, v] : old_options.object) s.options[k] = v;
    const std::string old_switch = read_file(old + "/enabled");
    if (!old_switch.empty()) s.enabled = old_switch[0] != '0';
    std::string error;
    if (!write_settings(b.id, s, error)) LOG("[guestmods] %s: %s", b.id, error.c_str());
    else LOG("[guestmods] %s: settings written to %s (%s)", b.id, settings_path(b.id).c_str(),
             old_options.type == mods::json::Value::Object ? "taken over from the earlier mods/" + std::string(b.id) + "/" : "the defaults");
    return s;
}
// the package's files built into the NRO, under the package path "builtin:<id>" (load()); false for another path
bool package_file(const std::string& package_path, const std::string& relative, std::span<const uint8_t>& out) {
    if (package_path.rfind("builtin:", 0) != 0) return false;
    const WWHDGuestBuiltin* b = find(package_path.substr(8));
    if (!b) return false;
    for (uint32_t i = 0; i < b->file_count; i++)
        if (relative == b->files[i].path) {
            out = std::span<const uint8_t>(b->files[i].data, b->files[i].size);
            return true;
        }
    return false;
}

Region memory() {
    Region r;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (const WWHDGuestBuiltin* b = begin(); b != end(); ++b) {
        lo = std::min(lo, b->base);
        hi = std::max(hi, b->base + b->region_size);
    }
    if (hi > lo) r = {lo, hi - lo};
    return r;
}

bool hooks_checked(const WWHDGuestModuleV1& m, std::string& error) {
    if (!g_checked) return true;  // every function has the check (or none, and the mod manager refuses mods)
    for (uint32_t i = 0; i < m.hook_count; i++) {
        uint32_t t = m.hooks[i].target;
        if (!std::binary_search(g_checked, g_checked + g_checked_count, t)) {
            char text[160];
            snprintf(text, sizeof text, "the game code has no mod check in %08X: translate it again with this "
                                        "NRO's build/guest-mods/hooks.txt (WWHD_RECOMP_MOD_HOOKS_ONLY)", t);
            error = text;
            return false;
        }
    }
    return true;
}

void load(const Install& install) {
    if (begin() == end()) return;
    if (!g_mod_hook_count) {
        LOG("[guestmods] the NRO has guest mods but its game code no mod hooks: translate the game again with "
            "build/guest-mods/hooks.txt (WWHD_RECOMP_MOD_HOOKS_ONLY); no guest mod loaded");
        return;
    }
    for (const WWHDGuestBuiltin* b = begin(); b != end(); ++b) {
        const Settings settings = read_settings(*b);
        if (!settings.enabled) {  // (#121) built in, switched off by the player (Mods tab) or the installer
            LOG("[guestmods] %s %s (built into the NRO) switched off: not loaded", b->id, b->version);
            g_status[b->id] = "";
            continue;
        }
        mods::packages::GuestPackage pkg;
        pkg.id = b->id;
        pkg.name = b->name;
        pkg.version = b->version;
        pkg.path = std::string("builtin:") + b->id;  // (#121) its package files are in the NRO (package_file)
        pkg.data_path = mod_dir(b->id) + "/data";
        pkg.heap_size = b->heap_size;
        pkg.options = settings.options;
        std::string error;
        if (!install(pkg, b->base, b->region_size, b->init, error)) {
            LOG("[guestmods] %s %s (built into the NRO) not loaded: %s", b->id, b->version, error.c_str());
            g_status[b->id] = error.empty() ? "not loaded" : error;
        } else {
            g_status[b->id] = "";
            g_loaded.push_back(b->id);
        }
    }
}

std::vector<Mod>& list() {
    static std::vector<Mod> mods_;
    static bool read = false;
    if (read) return mods_;
    read = true;
    for (const WWHDGuestBuiltin* b = begin(); b != end(); ++b) {
        Mod m;
        m.id = b->id;
        m.name = b->name;
        m.version = b->version;
        auto st = g_status.find(b->id);
        m.status = st == g_status.end() ? "not loaded: the game code has no mod hooks (see wwhd.log)" : st->second;
        const Settings settings = read_settings(*b);
        m.enabled = settings.enabled;
        m.loaded = std::find(g_loaded.begin(), g_loaded.end(), m.id) != g_loaded.end();
        mods::json::Value man;
        std::span<const uint8_t> bytes;
        if (package_file(std::string("builtin:") + b->id, "manifest.json", bytes))
            man = parse_or_null(std::string(bytes.begin(), bytes.end()));
        const mods::json::Value& values = settings.options;
        m.description = man.get("description").string();
        for (const auto& o : man.get("options").array) {
            Option op;
            op.id = o.get("id").string();
            if (op.id.empty()) continue;
            op.name = o.get("name").string(op.id);
            op.type = o.get("type").string();
            op.description = o.get("description").string();
            if (op.type == "boolean") op.type = "bool";
            const auto& lo = o.get("min");
            const auto& hi = o.get("max");
            const auto& step = o.get("step");
            if (lo.type == mods::json::Value::Number) op.minimum = lo.number;
            if (hi.type == mods::json::Value::Number) op.maximum = hi.number;
            if (step.type == mods::json::Value::Number) op.step = step.number;
            for (const auto& c : o.get("choices").array) if (c.type == mods::json::Value::String) op.choices.push_back(c.text);
            op.value = values.get(op.id);
            if (op.value.type == mods::json::Value::Null) op.value = o.get("default");
            m.options.push_back(std::move(op));
        }
        mods_.push_back(std::move(m));
    }
    return mods_;
}

bool set_option(Mod& mod, Option& option, const mods::json::Value& value, bool save, std::string& error) {
    mods::json::Value v = value;
    if (option.type == "number" && v.type == mods::json::Value::Number) {
        double x = std::clamp(v.number, option.minimum, option.maximum);
        if (option.step > 0) x = std::clamp(option.minimum + std::round((x - option.minimum) / option.step) * option.step,
                                            option.minimum, option.maximum);
        v.number = x;
    }
    option.value = v;
    if (!save) return true;
    Settings st;
    st.enabled = mod.enabled;
    st.options.type = mods::json::Value::Object;
    for (const auto& o : mod.options)
        if (o.value.type != mods::json::Value::Null) st.options[o.id] = o.value;
    if (!write_settings(mod.id, st, error)) return false;
    mod.changed = true;
    return true;
}

bool set_enabled(Mod& mod, bool on, std::string& error) {
    Settings st;
    st.enabled = on;
    st.options.type = mods::json::Value::Object;
    for (const auto& o : mod.options)
        if (o.value.type != mods::json::Value::Null) st.options[o.id] = o.value;
    if (!write_settings(mod.id, st, error)) return false;
    mod.enabled = on;
    return true;
}

}  // namespace guestbuiltin
