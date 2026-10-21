// Wait for shaders (shader_wait_switch.h).
#include "shader_wait_switch.h"

#include <atomic>
#include <cstdint>

#include "../runtime.h"
#include "guest_addr.h"

namespace gfxdk {  // gfx/deko/shaders_dk.cpp
void set_draws_wait(bool on);
}  // namespace gfxdk

namespace shader_wait {
namespace {

// g_dComIfG_gameInfo.play (prepare_graphics_switch.cpp's fields)
const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kEventRun = kPlay + 0x5292;  // dComIfGp_event_runCheck
const uint32_t kLinkPtr = kPlay + 0x5B34;   // daPy_lk_c
constexpr uint32_t kCurProc = 0x65F0;       // daPy_lk_c::mCurProc (decomp tools/verify/units/d_a_player_main.txt)
// d_a_player_main_05.h link05demo: the procedures that show an item
// (scripted ones: the story's gifts, e.g. Grandma's Hero's Clothes, run in DEMO_TOOL, Link acting on an event's "tool"
// commands; seen on the console with the item shown, 2026-10-21)
constexpr int32_t kItemProcs[] = {
    0xA9,  // daPyProc_DEMO_TOOL_e
    0xAC,  // daPyProc_DEMO_HOLDUP_e
    0xAD,  // daPyProc_DEMO_OPEN_TREASURE_e
    0xAE,  // daPyProc_DEMO_GET_ITEM_e
    0xD3,  // daPyProc_DEMO_OPEN_SALVAGE_TREASURE_e
};

std::atomic<int> g_mode{kOff};
bool g_waiting = false;  // main thread: what set_draws_wait was last told

bool item_shown() {
    const uint32_t link = ld32(kLinkPtr);
    if (link < mem::kMem2Start || link >= mem::kMem2End) return false;
    const int32_t proc = int32_t(ld32(link + kCurProc));
    for (int32_t p : kItemProcs)
        if (proc == p) return true;
    return false;
}

}  // namespace

const char* label(int m) {
    static const char* const names[] = {"Off (skip the draw)", "While an item is shown", "Items and events",
                                        "Always"};
    return m >= 0 && m < kModes ? names[m] : "?";
}

int mode() { return g_mode.load(std::memory_order_relaxed); }

void set_mode(int m) {
    if (m < 0 || m >= kModes) m = kOff;
    if (g_mode.exchange(m) != m) LOG("[shader-wait] %s", label(m));
}

void frame() {
    const int m = mode();
    bool wait = false;
    if (m == kAlways) wait = true;
    else if (m == kItemGets) wait = item_shown();
    else if (m == kItemsEvents) wait = item_shown() || ld8(kEventRun);
    if (wait == g_waiting) return;
    g_waiting = wait;
    gfxdk::set_draws_wait(wait);
}

}  // namespace shader_wait
