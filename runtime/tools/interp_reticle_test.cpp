#include "../src/interp_reticle.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace interp::reticle;

// A camera-relative aim point must track the translating draw camera, rather
// than jumping towards the next logic endpoint on the first draw of each step.
int main() {
    for (int frames : {1, 2, 4, 8}) {
        History h;
        h.sample({0, 0, -100}, 10, 1, 1);
        for (int step = 11; step < 20; ++step) {
            float oldEye = float(step - 11) * 4;
            float eye = oldEye + 4;
            for (int pass = 1; pass <= frames; ++pass) {
                float t = float(pass) / frames;
                Point p = h.sample({eye, 0, -100}, step, 1, t);
                float drawnEye = oldEye + t * 4;
                float x = 640 + 640 * (p[0] - drawnEye) / -p[2];
                assert(std::abs(x - 640) < .0001f);
                if (pass < frames) assert(640 * (eye - drawnEye) / 100 > 0);
            }
        }
    }
    History h;
    Point a{1, 2, 3}, b{5, 6, 7};
    assert(h.sample(a, 1, 1, .5f) == a); // newly visible
    assert(h.sample(b, 2, 1, .25f) == (Point{2, 3, 4}));
    assert(h.sample(b, 2, 1, .75f) == (Point{4, 5, 6})); // no drift over holds
    assert(h.sample(b, 2, 1, 1) == b); // exact frame unchanged
    assert(h.sample(a, 4, 1, .5f) == a); // hidden for a step / skipped history
    assert(h.sample(b, 5, 2, .5f) == b); // target slot reused by another actor
    h = {};
    assert(h.sample(a, 5, 2, .5f) == a); // state load
    // Keep game speed can change the number of drawn passes between steps,
    // including an exact-only step when there is no time for an in-between.
    h = {};
    h.sample({0, 0, -100}, 20, 1, 1);
    uint64_t step = 20;
    float eye = 0;
    for (int frames : {4, 2, 1, 8, 2, 4}) {
        ++step;
        float before = eye;
        eye += 4;
        for (int pass = 1; pass <= frames; ++pass) {
            float t = float(pass) / frames;
            Point p = h.sample({eye, 0, -100}, step, 1, t);
            assert(std::abs(p[0] - (before + t * 4)) < .0001f);
        }
    }
    std::puts("interp_reticle: PASS");
}
