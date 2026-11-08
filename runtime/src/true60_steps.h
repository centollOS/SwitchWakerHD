#pragma once

namespace true60 {
// fpcM_Management parts (WWHD_INTERP_RUN). Lifecycle handlers cannot be
// accelerated: their timers and draw-list ownership count full game steps.
constexpr bool skip_step_part(bool half, bool active, unsigned run, unsigned part) {
    if (!half) return false;
    if (active && (part == 2 || part == 8)) return true;  // deletion / creation
    return !(run & part);
}
}  // namespace true60
