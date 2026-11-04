/* Original example: live options, an input filter and a touch claim (Mod SDK, docs/mod-sdk-v2.md
 * "Live options, input filters and touches"). Game names come from the public CC0 HD SDK. */
#include "wwhd_guest.h"
#include "wwhd/bindings.h"

#define VPAD_A 0x8000u
#define VPAD_B 0x4000u
#define VPAD_Y 0x1000u

static int swap_ab;                 /* the live option "swap", as last read */
static unsigned long long seen;     /* its revision when it was read */
static int finger;                  /* a finger is in the claimed corner */
static int registered;

/* a finger in the top-right corner of the picture holds Y */
static void touch(const wwhd_touch_event* e) {
    if (e->kind == WWHD_TOUCH_DOWN) { finger = 1; wwhd_log("finger in the corner: Y"); }
    else if (e->kind == WWHD_TOUCH_UP || e->kind == WWHD_TOUCH_CANCEL) finger = 0;
}

/* each of the game's controller reads */
static void filter(wwhd_input_state* s) {
    unsigned long long r = wwhd_config_changed("swap");
    if (r != seen) {
        seen = r;
        swap_ab = wwhd_config_bool("swap", 0);
        wwhd_log_int("swap A and B (revision)", (int)r);
    }
    if (swap_ab) {
        u32 a = s->buttons & VPAD_A, b = s->buttons & VPAD_B;
        s->buttons = (s->buttons & ~(VPAD_A | VPAD_B)) | (a ? VPAD_B : 0) | (b ? VPAD_A : 0);
    }
    if (finger) s->buttons |= VPAD_Y;
}

WWHD_HOOK(WWHD_ADDR_daPy_Execute, register_live_input, (void* link)) {
    (void)link;
    if (registered) return;
    registered = 1;
    wwhd_input_filter_register(filter);
    wwhd_touch_register(touch);
    wwhd_touch_claim(0, 440.0f, 200.0f, 640.0f, 360.0f);
}
