#pragma once
#include "interp_pacing.h"
#include <array>
#include <cstdint>

namespace interp::reticle {
using Point = std::array<float, 3>;

// World endpoints belong to logic steps, not draw passes. Project their blend
// through the drawn camera, then restore the original gameplay point. Repeated
// hold draws must not feed a blended position into the endpoint history.
struct History {
    Point previous{}, current{};
    uint64_t step = ~uint64_t(0);
    uint64_t source = 0;
    bool valid = false;

    Point sample(Point exact, uint64_t now, uint64_t identity, float t) {
        if (now != step || identity != source) {
            valid = source == identity && step != ~uint64_t(0) && step + 1 == now;
            previous = current;
            step = now;
            source = identity;
            current = exact;
        }
        if (!valid || t >= 1.0f) return current;
        Point result;
        for (int i = 0; i < 3; ++i)
            result[i] = pacing::lerp_f(previous[i], current[i], t);
        return result;
    }
};
} // namespace interp::reticle
