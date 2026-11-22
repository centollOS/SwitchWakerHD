// (this fork, s2/hdr-info) what the Switch 2 display reports through VI2 (switch2/services/vi2.h): its refresh rates,
// whether VRR is active, its largest buffer resolution level and its HDR output formats. Read only: nothing here
// changes the display. HDR output itself needs FP16/HDR swapchains in libounce and drako3d (drako3d's README,
// "Future plans"), which neither has yet; docs/s2-hdr-design.md is the plan for when they do.
#pragma once
#include <cstdint>

namespace s2display {
struct Caps {
    bool valid = false;              // read at least once
    uint64_t refresh = 0;            // Vi2DisplayModeCapability.refresh_rates: bit 0 60 Hz, bit 1 120 Hz, bit 63 VRR
    uint64_t refreshSecondary = 0;   // .refresh_rates_secondary (how it differs is not known)
    uint64_t hdr = 0;                // .hdr_output_formats: bits 0-1 used, their formats not known
    uint32_t hz = 0;                 // the mode's nominal refresh rate now (60 or 120; 0 unknown)
    bool vrrActive = false;          // the current refresh word's bit 63
    int maxResLevel = -1;            // vi2QueryDisplayBufferMaxResolution: 0-3, pixel sizes not known; -1 unknown
    bool docked = false;
};
// render thread, at each present after the swapchain's first: reads the display again when the operation mode
// changed and every 10 s, and logs what changed ("[s2] display ...")
void poll(uint64_t frame, bool docked);
Caps caps();  // the last read (any thread)
}  // namespace s2display
