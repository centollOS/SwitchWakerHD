// The mouse wheel over a code mod's touch claim (Mod SDK pinches): kept apart from display_modes.cpp so the
// display tests, which compile display_modes.cpp alone, don't need the TV pointer or the mods.
#include "display_modes.h"
#include "../tv_pointer.h"
#include "../mods/mods.h"

namespace gfx {
// the game's picture under a point: the TV picture, or in One screen mode's Off-TV Play the GamePad picture
// (which is the game then); 0..1 of it and its aspect
bool wheel_on_mod_claim(float nx, float ny, float* px, float* py, float* aspect) {
    if (!tv_pointer::wanted(tv_pointer::kOwnerTalk) || !game_picture_hit(nx, ny, px, py, aspect)) return false;
    tv_pointer::State s{false, *px, *py, *aspect};
    float lx, ly;
    tv_pointer::to_page(s, lx, ly);
    return mods::guest_touch_claimed_at(lx - 640.0f, 360.0f - ly);
}
}  // namespace gfx
