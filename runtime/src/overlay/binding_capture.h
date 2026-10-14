#pragma once
#include "input_map.h"
namespace overlay {
class BindingCapture {
    bool released_ = false;
public:
    void reset() { released_ = false; }
    int poll(const float* values) {
        bool any = false;
        int pressed = -1;
        for (int p = 1; p < input_map::kPadCount; ++p) {
            any |= values[p] > 0.3f;
            if (values[p] > 0.6f && pressed < 0) pressed = p;
        }
        if (!released_) { released_ = !any; return -1; }
        return pressed;
    }
};
}  // namespace overlay
