#include "mods/moon_jump_policy.h"
#include <cassert>
#include <limits>

using mods::moon_jump_policy::steer;
int main() {
    // Camera-relative forward/right and a released stick.
    auto forward = steer(0, 0, 1, 0, 1, 1);
    assert(forward.facing == 0 && std::abs(forward.x) < .001f && forward.z == 40.f);
    auto aligned = steer(-16384, 1, 0, 0, 1, 1);
    assert(aligned.facing == -16384 && std::abs(aligned.x + 40.f) < .001f);
    auto idle = steer(1234, 0, 0, 0, 1, 1);
    assert(idle.facing == 1234 && idle.x == 0 && idle.z == 0);
    // Two half turns cover the same angle as one full turn; velocity stays in
    // 30 Hz units so the movement hook, rather than this policy, scales travel.
    auto full = steer(0, 0, -1, 0, 1, 1);
    auto half = steer(0, 0, -1, 0, 1, .5f);
    auto second = steer(half.facing, 0, -1, 0, 1, .5f);
    assert(full.facing == second.facing);
    assert(std::abs(std::hypot(half.x, half.z) - 40.f) < .001f);
    // Turn through the signed-angle seam by the short route.
    auto seam = steer(32760, 0, -1, 0, 1, 1);
    assert(seam.facing == -32768);
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity()}) {
        auto rejected = steer(42, bad, 1, 0, 1, 1);
        assert(rejected.facing == 42 && rejected.x == 0 && rejected.z == 0);
    }
    auto no_camera = steer(42, 1, 1, 0, 0, 1);
    assert(no_camera.x == 0 && no_camera.z == 0);
}
