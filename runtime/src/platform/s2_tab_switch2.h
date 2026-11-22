// (this fork, s2/*) the settings overlay's "Switch 2" tab: the options only a Switch 2 has (display, output and render
// size, VRR, cores). Each s2/<name> branch defines its own section_* and start_* (s2_<name>_switch2.cpp); this file's
// weak empty versions stand for the ones a build does not have, so the branches combine without touching each other.
// Every option is off (or the old behaviour) by default and is saved in settings.ini under an s2* key.
#pragma once
#include <string>

namespace s2tab {
void tab();             // the tab (overlay_switch.cpp registers it on Switch 2)
void apply_at_start();  // switch_settings::apply_at_start: every section's saved option

// sections in the tab's order, and what each reads from settings.ini at start
void section_display();     // s2/hdr-info: the display's refresh rates, VRR and HDR capability (read only)
void section_resolution();  // s2/resolution: output mode and render resolution
void section_vrr();         // s2/vrr: variable refresh rate (present when ready)
void section_cores();       // s2/cores: worker threads on cores 4-5
void start_display();
void start_resolution();
void start_vrr();
void start_cores();

// the overlay's look (overlay_switch.cpp keeps its own copies in an unnamed namespace)
void heading(const char* text);
void help(const char* text);  // the tooltip of the last widget
void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// settings.ini: a saved value (false when absent), and a value saved from the menu (on the main thread)
bool saved(const char* key, std::string& value);
void save(const char* key, std::string value);
}  // namespace s2tab
