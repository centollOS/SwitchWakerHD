#include "gfx/depth_peek.h"
#include <cassert>
#include <limits>

int main() {
    using gfx::depth_peek::pixel;
    // The sun's legacy screen space and the HD viewport describe the same centre.
    assert(pixel(320, 1280, 640) == 640);
    assert(pixel(240, 720, 480) == 360);
    assert(pixel(640, 1280, 1280) == 640);
    assert(pixel(360, 720, 720) == 360);
    for (auto guest : {640u, 480u, 1280u, 720u}) {
        for (auto host : {1u, 720u, 1280u, 2560u}) {
            assert(pixel(0, host, guest) == 0);
            assert(pixel(uint32_t(-1), host, guest) == 0);
            assert(pixel(uint32_t(-32768), host, guest) == 0);
            assert(pixel(guest, host, guest) == int(host - 1));
            assert(pixel(guest * 2, host, guest) == int(host - 1));
            assert(pixel(guest - 1, host, guest) == int(uint64_t(guest - 1) * host / guest));
        }
    }
    using gfx::depth_peek::from_float;
    assert(from_float(0) == 0);
    assert(from_float(0.5f) == 0x7FFFFF);
    assert(from_float(1) == 0xFFFFFF);
    assert(from_float(-1) == 0);
    assert(from_float(2) == 0xFFFFFF);
    assert(from_float(std::numeric_limits<float>::quiet_NaN()) == 0);
}
