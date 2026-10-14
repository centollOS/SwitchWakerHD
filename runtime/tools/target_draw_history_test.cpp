#include "gfx/target_draw_history.h"
#include <cassert>

int main() {
    gfx::TargetDrawHistory target;
    assert(target.must_wait(0));
    target.drawn(0);
    assert(target.must_wait(0)); // a second shader in the same one-time pass cannot be skipped
    target.drawn(0);
    assert(target.must_wait(0));
    assert(!target.must_wait(1)); // continuously redrawn targets can use the compile budget
    target.drawn(1);
    assert(!target.must_wait(1));
    target.drawn(1);
    assert(!target.must_wait(2));
    assert(target.must_wait(3)); // not drawn in frame 2: a new one-time pass
    target.drawn(3);
    assert(target.must_wait(3));
    assert(target.must_wait(0)); // a restored/restarted frame counter is also a new pass
    target.drawn(0);
    assert(target.must_wait(0));
}
