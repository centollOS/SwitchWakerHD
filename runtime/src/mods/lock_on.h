// Enemy lock-on assistance: pure policy shared by the runtime and tests.
#pragma once
#include <cmath>

namespace mods::lock_on {
// Attention category 2 is the native enemy channel; bit 2 enables that channel.
constexpr unsigned kEnemyCategory = 2;
inline bool enemy_channel(unsigned category, unsigned flags) {
    return category == kEnemyCategory && (flags & (1u << kEnemyCategory));
}
constexpr float kDefaultRange = 1.f;
constexpr float kMaxRange = 3.f;
inline float clamp_range(float factor) {
    return !std::isfinite(factor) ? kDefaultRange : factor < 1.f ? 1.f : factor > kMaxRange ? kMaxRange : factor;
}
} // namespace mods::lock_on
