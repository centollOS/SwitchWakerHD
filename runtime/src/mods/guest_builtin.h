// SwitchWakerHD (this fork): guest mods built into the NRO (docs/guest-mods-switch-plan.md). Their modules are
// compiled in (tools/switch/guest_mods.py, build/guest-mods/registry.c, WWHD_GUEST_BUILTIN); guest_mods.cpp
// loads them at the start through load(), the same installation as a module the mod manager built.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "mod_json.h"
#include "wwhd_guest_abi.h"

namespace mods::packages { struct GuestPackage; }
namespace guestbuiltin {
struct Region { uint32_t start = 0, size = 0; };
// the guest memory of every built-in mod (module and heap), for the Switch's guest memory window; empty without mods
Region memory();
// loads each built-in mod with `install` (guest_mods.cpp load_one); a mod that fails is logged and left out
using Install = std::function<bool(const mods::packages::GuestPackage&, uint32_t base, uint32_t reserved,
                                   WWHDGuestInitV1 init, std::string& error)>;
void load(const Install& install);
// a module's hooks are all on functions with the mods' check (WWHD_RECOMP_MOD_HOOKS_ONLY: only the listed ones)
bool hooks_checked(const WWHDGuestModuleV1& m, std::string& error);

// The built-in mods as the Mods tab shows them (overlay/guest_builtin_ui.cpp): each one's options from its
// manifest (the package copy on the SD card) with the values of options.json, which load() reads at the start.
// The mods read their options once, so a change applies at the next start.
struct Option {
    std::string id, name, type, description;  // type: number, bool, enum or string (the manifest's)
    double minimum = 0, maximum = 1, step = 0;
    std::vector<std::string> choices;
    mods::json::Value value;
};
struct Mod {
    std::string id, name, version, description;
    std::string status;   // "" running, else why not (load()'s error)
    std::vector<Option> options;
    bool changed = false; // an option was saved since the start
};
std::vector<Mod>& list();  // read on the first call (UI thread)
// sets an option of a list() mod; save: also writes options.json (a slider saves when it is let go)
bool set_option(Mod& mod, Option& option, const mods::json::Value& value, bool save, std::string& error);
}  // namespace guestbuiltin
