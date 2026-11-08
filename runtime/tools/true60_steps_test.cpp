#include "true60_steps.h"
#include <cassert>

int main() {
    // Even diagnostic overrides must not free/create a process between full
    // steps. Other parts retain their existing override behavior.
    for (unsigned run = 0; run < 128; ++run) {
        for (unsigned part = 1; part < 128; part <<= 1) {
            assert(!true60::skip_step_part(false, true, run, part));
            assert(!true60::skip_step_part(false, false, run, part));
            assert(true60::skip_step_part(true, false, run, part) == !(run & part));
            assert(true60::skip_step_part(true, true, run, part) ==
                   (part == 2 || part == 8 || !(run & part)));
        }
    }
    unsigned deleted = 0, created = 0;
    for (unsigned pass = 0; pass < 60; ++pass) {
        bool half = pass & 1;
        deleted += !true60::skip_step_part(half, true, 127, 2);
        created += !true60::skip_step_part(half, true, 127, 8);
    }
    assert(deleted == 30 && created == 30);
}
