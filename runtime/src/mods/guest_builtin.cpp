// SwitchWakerHD (this fork): guest mods built into the NRO (guest_builtin.h, docs/guest-mods-switch-plan.md).
#include "guest_builtin.h"

#include <algorithm>
#include <fstream>
#include <iterator>

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
        const std::string dir = host::config_dir() + "/mods/" + b->id;
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
        if (!install(pkg, b->base, b->region_size, b->init, error))
            LOG("[guestmods] %s %s (built into the NRO) not loaded: %s", b->id, b->version, error.c_str());
    }
}

}  // namespace guestbuiltin
