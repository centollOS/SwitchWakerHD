// What the settings overlay needs from the host (window system) it runs in. Two implementations:
//   AppKit host (macOS, Metal and Vulkan): gfx/overlay_appkit.mm, on top of menu.mm and display.mm
//   SDL host (Vulkan-only builds: Windows, Linux, macOS test builds): gfx/vulkan/overlay_sdl.cpp
// Reads may come from any thread; changes are made on the main thread (post()), as the menus do.
#pragma once
#include <functional>
#include <string>

namespace hostui {

void post(std::function<void()> fn);  // run on the main thread (soon, in order)

// persistent settings: display.plist (AppKit) or <config dir>/settings.ini (SDL host). Test runs
// (WWHD_NO_HOST_INPUT) neither read nor write the user's file.
bool get(const char* key, std::string& value);
void set(const char* key, const std::string& value);

// graphics options that the host keeps (and saves) itself
float res_scale();                 // internal resolution (as the menu shows it)
void set_res_scale(float s);
void graphics_changed();           // an option changed: the AppKit host saves its options and refreshes the title
int scale_filter();                // 0 smooth, 1 sharp, 2 integer
void set_scale_filter(int f);
bool scale_filter_available();

// display
bool fullscreen();
void set_fullscreen(bool on);
int drc_modes();                   // GamePad screen modes offered: 4 (window, picture-in-picture, auto, off) or 0
int drc_mode();
void set_drc_mode(int m);
bool drc_available();              // there is a GamePad screen to show or hide
bool drc_shown();
void show_drc(bool on);
void set_pro_controller(bool on);  // Input: keyboard and controllers act as a Pro Controller (or the GamePad)

const char* name();                // "AppKit" or "SDL"

// SDL host only (gfx/vulkan/overlay_sdl.cpp, called by its main loop)
void run_posted();                 // the functions post()ed since the last call
void load_saved_options();         // start-up: graphics options saved in settings.ini

}  // namespace hostui
