#pragma once
#include <cstdint>

namespace gfx {
// Every draw in a target's first frame must finish compiling: later draws may be
// the only opportunity to render a photo's subject mask, even after an earlier draw ran.
struct TargetDrawHistory {
    bool valid = false;
    bool first_frame = false;
    uint64_t last_frame = 0;

    bool must_wait(uint64_t frame) const {
        if (!valid) return true;
        if (frame == last_frame) return first_frame;
        return frame < last_frame || frame - last_frame != 1;
    }
    void drawn(uint64_t frame) {
        first_frame = must_wait(frame);
        last_frame = frame;
        valid = true;
    }
};
}
