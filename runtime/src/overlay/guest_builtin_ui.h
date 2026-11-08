// SwitchWakerHD (this fork): the Mods tab's section for the code mods built into the Switch NRO
// (mods/guest_builtin.h, docs/guest-mods-switch-plan.md): their state and options, saved to options.json.
#pragma once

namespace guestbuiltinui {
// In the Mods tab's Built-in list (modsui::Host::builtin_code_mods): one row per built-in code mod, as the built-in
// mods' rows; its name opens the mod's page. Nothing without built-in mods.
void draw();
// The open code mod's page in place of the Built-in list (modsui::Host::builtin_code_mod_page): false when no page is
// open. back: the controller's B this frame; back_used is set when the page took it.
bool page(bool back, bool& back_used);
}
