// SwitchWakerHD (this fork): guest mods built into the NRO (guest_builtin.h, docs/guest-mods-switch-plan.md).
#include "guest_builtin.h"

#include <algorithm>
#include <cmath>
#include <fstream>
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

static std::map<std::string, std::string> g_status;  // load()'s outcome per mod: "" loaded

static std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string{std::istreambuf_iterator<char>(in), {}} : std::string();
}
static std::string mod_dir(const char* id) { return host::config_dir() + "/mods/" + id; }

#if WWHD_GUEST_BUILTIN
static const WWHDGuestBuiltin* begin() { return g_guest_builtin; }
static const WWHDGuestBuiltin* end() { return g_guest_builtin + g_guest_builtin_count; }
#else
static const WWHDGuestBuiltin* begin() { return nullptr; }
static const WWHDGuestBuiltin* end() { return nullptr; }
#endif

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
        const std::string dir = mod_dir(b->id);
        mods::packages::GuestPackage pkg;
        pkg.id = b->id;
        pkg.name = b->name;
        pkg.version = b->version;
        pkg.path = dir + "/package";
        pkg.data_path = dir + "/data";
        pkg.heap_size = b->heap_size;
        pkg.options = mods::json::Value();
        std::ifstream in(dir + "/options.json", std::ios::binary);
        if (in) {
            try {
                pkg.options = mods::json::parse(std::string{std::istreambuf_iterator<char>(in), {}});
            } catch (const std::exception& e) {
                LOG("[guestmods] %s: options.json not read (%s): the mod's defaults", b->id, e.what());
            }
        } else {
            LOG("[guestmods] %s: no %s/ (its images and data): copy build/sd/switch/wwhd/mods/ to the SD card", b->id,
                dir.c_str());
        }
        std::string error;
        if (!install(pkg, b->base, b->region_size, b->init, error)) {
            LOG("[guestmods] %s %s (built into the NRO) not loaded: %s", b->id, b->version, error.c_str());
            g_status[b->id] = error.empty() ? "not loaded" : error;
        } else {
            g_status[b->id] = "";
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
        const std::string dir = mod_dir(b->id);
        mods::json::Value man, values;
        try {
            man = mods::json::parse(read_file(dir + "/package/manifest.json"));
        } catch (const std::exception&) {
            m.status = "its files are missing: copy build/sd/switch/wwhd/mods/ to the SD card";
        }
        try {
            values = mods::json::parse(read_file(dir + "/options.json"));
        } catch (const std::exception&) {}
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
    mods::json::Value all;
    all.type = mods::json::Value::Object;
    for (const auto& o : mod.options)
        if (o.value.type != mods::json::Value::Null) all[o.id] = o.value;
    const std::string dir = mod_dir(mod.id.c_str());
    const std::string tmp = dir + "/options.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << mods::json::dump(all) << '\n';
        f.close();
        if (!f) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    if (!host::replace_file(tmp, dir + "/options.json")) {
        error = "cannot save " + dir + "/options.json";
        return false;
    }
    mod.changed = true;
    return true;
}

}  // namespace guestbuiltin
