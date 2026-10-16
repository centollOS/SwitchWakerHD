// SwitchWakerHD (this fork): the Mods tab's section for the code mods built into the Switch NRO
// (mods/guest_builtin.h, docs/guest-mods-switch-plan.md): their state and options, saved to options.json.
#pragma once

namespace guestbuiltinui {
void draw();  // in the Mods tab's Built-in area (modsui::Host::builtin_code_mods); nothing without built-in mods
}
