// Settings overlay on the AppKit host (macOS, Metal and Vulkan): hostui.h on top of the menu bar's
// code (menu.mm, display.mm), so the overlay and the menus change the same state; and the overlay's
// mouse input from the TV window.
#import <AppKit/AppKit.h>

#include "../input.h"
#include "../overlay/hostui.h"
#include "../overlay/overlay.h"
#include "../runtime.h"

namespace gfx {
// menu.mm
float menu_res_scale();
void menu_set_res_scale(float f);
void menu_options_changed();
// display.mm
bool host_setting(const char* key, std::string& value);
void set_host_setting(const char* key, const std::string& value);
int display_filter();
void display_set_filter(int f);
int display_drc_mode();
void set_drc_mode(int m);
bool tv_fullscreen();
void display_set_tv_fullscreen(bool on);
void* display_tv_window();
bool drc_window_available();
bool drc_window_shown();
void show_drc_window(bool on);
}  // namespace gfx

namespace hostui {

void post(std::function<void()> fn) {
    dispatch_async(dispatch_get_main_queue(), ^{ fn(); });
}
bool get(const char* key, std::string& value) { return gfx::host_setting(key, value); }
void set(const char* key, const std::string& value) { gfx::set_host_setting(key, value); }
float res_scale() { return gfx::menu_res_scale(); }
void set_res_scale(float s) { gfx::menu_set_res_scale(s); }
void graphics_changed() { gfx::menu_options_changed(); }
int scale_filter() { return gfx::display_filter(); }
void set_scale_filter(int f) { gfx::display_set_filter(f); }
bool scale_filter_available() { return true; }
bool fullscreen() { return gfx::tv_fullscreen(); }
void set_fullscreen(bool on) { gfx::display_set_tv_fullscreen(on); }
int drc_modes() { return 4; }
int drc_mode() { return gfx::display_drc_mode(); }
void set_drc_mode(int m) { gfx::set_drc_mode(m); }
bool drc_available() { return gfx::drc_window_available(); }
bool drc_shown() { return gfx::drc_window_shown(); }
void show_drc(bool on) { gfx::show_drc_window(on); }
void set_pro_controller(bool on) {
    // as Input > Wii U GamePad / Pro Controller: the GamePad window follows the choice
    input::set_pro_controller(on);
    gfx::show_drc_window(!on);
}
const char* name() { return "AppKit"; }
void run_posted() {}
void load_saved_options() {}

}  // namespace hostui

namespace gfx {

// mouse in the TV window while the overlay is open (installed with the menus, menu.mm)
void install_overlay_input() {
    NSWindow* tv = (__bridge NSWindow*)display_tv_window();
    if (!tv) return;
    auto density = [tv] { overlay::set_density((float)tv.backingScaleFactor); };
    density();
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeBackingPropertiesNotification object:tv queue:nil
                                                  usingBlock:^(NSNotification*) { density(); }];
    if (getenv("WWHD_NO_HOST_INPUT")) return;
    tv.acceptsMouseMovedEvents = YES;
    NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged |
                       NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                       NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp | NSEventMaskScrollWheel;
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (!overlay::is_open() || e.window != tv) return e;
        NSView* v = tv.contentView;
        NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
        NSSize sz = v.bounds.size;
        if (sz.width <= 0 || sz.height <= 0) return e;
        // the title bar stays the window's (move, close)
        if (p.y > sz.height && e.type != NSEventTypeScrollWheel) return e;
        overlay::mouse_move((float)(p.x / sz.width), (float)(v.isFlipped ? p.y / sz.height : 1.0 - p.y / sz.height));
        switch (e.type) {
        case NSEventTypeLeftMouseDown: overlay::mouse_button(0, true); break;
        case NSEventTypeLeftMouseUp: overlay::mouse_button(0, false); break;
        case NSEventTypeRightMouseDown: overlay::mouse_button(1, true); break;
        case NSEventTypeRightMouseUp: overlay::mouse_button(1, false); break;
        case NSEventTypeOtherMouseDown: overlay::mouse_button(2, true); break;
        case NSEventTypeOtherMouseUp: overlay::mouse_button(2, false); break;
        case NSEventTypeScrollWheel: {
            float dx = (float)e.scrollingDeltaX, dy = (float)e.scrollingDeltaY;
            if (e.hasPreciseScrollingDeltas) dx /= 10.0f, dy /= 10.0f;  // trackpad: points, not lines
            overlay::mouse_wheel(dx, dy);
            break;
        }
        default: break;
        }
        return nil;  // the overlay's, not the game's (mouse camera, GamePad touch)
    }];
}

}  // namespace gfx
