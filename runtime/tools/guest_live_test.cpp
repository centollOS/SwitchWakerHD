// SPDX-License-Identifier: MPL-2.0
// guest_live.h: live option revisions and the touch claims' arbitration (Mod SDK, QoL track M0).
#include "mods/guest_live.h"

#include <cassert>
#include <cstdio>

using namespace guestlive;

int main() {
    // ---- revisions: 1 at start, one more per live change; 0 for options a mod doesn't have
    {
        Revisions r;
        r.declare("qol", "clothes");
        assert(r.revision("qol", "clothes") == 1 && r.revision("qol", "nope") == 0 && r.revision("other", "clothes") == 0);
        assert(r.bump("qol", "clothes") && r.revision("qol", "clothes") == 2);
        assert(!r.bump("qol", "nope") && r.revision("qol", "nope") == 0);
        r.declare("qol", "clothes");  // (declaring again keeps the count)
        assert(r.revision("qol", "clothes") == 2);
    }
    // ---- touches: claims, the newest wins, a finger stays its owner's until it lifts
    {
        Touch t;
        assert(!t.any() && t.owner_at(0, 0) == -1);
        t.claim(0, 0, {-100, -100, 100, 100});
        assert(t.any() && t.owner_at(0, 0) == 0 && t.owner_at(200, 0) == -1);
        t.claim(1, 3, {50, -50, 300, 50});  // overlaps mod 0's: newer
        assert(t.owner_at(60, 0) == 1 && t.owner_at(-60, 0) == 0);
        t.claim(0, 1, {40, -20, 80, 20});  // mod 0 claims again inside: now newest there
        assert(t.owner_at(60, 0) == 0 && t.owner_at(200, 0) == 1);
        // routing
        assert(t.route({kDown, 200, 0}) == 1 && t.finger_owner() == 1);
        assert(t.route({kMove, -500, -500}) == 1);  // moved out of the claim: still the mod's
        assert(t.route({kUp, -500, -500}) == 1 && t.finger_owner() == -1);
        assert(t.route({kDown, 600, 300}) == -1 && t.route({kUp, 600, 300}) == -1);  // the port's
        // removing a claim (an empty rectangle) and dropping a mod
        t.claim(1, 3, {0, 0, 0, 0});
        assert(t.owner_at(200, 0) == -1);
        assert(t.route({kDown, 0, 0}) == 0);
        t.drop(0);
        assert(t.finger_owner() == -1 && !t.any() && t.owner_at(0, 0) == -1);
        // bad slots are ignored
        t.claim(2, 8, {-1, -1, 1, 1});
        t.claim(2, -1, {-1, -1, 1, 1});
        assert(!t.any());
        assert((!Rect{10, 10, 5, 20}.contains(7, 15)));
    }
    // ---- pinches: the mod with the finger, else the claim under the point (the wheel); held until the end
    {
        Touch t;
        t.claim(0, 0, {-100, -100, 100, 100});
        t.claim(1, 0, {200, -100, 400, 100});
        assert(t.route({kDown, 0, 0}) == 0);
        assert(t.route_pinch(kPinch, 300, 0) == 0 && t.pinching());   // mod 0 has the finger: its pinch
        assert(t.route({kUp, 0, 0}) == 0);
        assert(t.route_pinch(kPinch, 300, 0) == 0);                    // still mod 0's until its end
        assert(t.route_pinch(kPinchEnd, 300, 0) == 0 && !t.pinching());
        assert(t.route_pinch(kPinch, 300, 0) == 1);                    // no finger: the claim under the point
        assert(t.route_pinch(kPinchEnd, 0, 0) == 1);
        assert(t.route_pinch(kPinch, 600, 0) == -1 && !t.pinching());  // nobody's: the port's own pinch
        assert(t.route_pinch(kPinchEnd, 600, 0) == -1);
        assert(t.route_pinch(kPinch, 300, 0) == 1);
        t.drop(1);
        assert(!t.pinching());
    }
    // ---- projection: a camera at the origin looking down -z, 360 layout units per unit of x / z (fovy 90)
    {
        View v;
        { float a, b; assert(!View{}.project({0, 0, -10}, a, b)); }
        assert(v.set({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 360));
        float x = 99, y = 99;
        assert(v.project({0, 0, -10}, x, y) && x == 0 && y == 0);
        assert(v.project({5, -2.5f, -10}, x, y) && std::fabs(x - 180) < 1e-3f && std::fabs(y + 90) < 1e-3f);
        assert(!v.project({0, 0, 10}, x, y));  // behind
        assert(!v.set({0, 0, 0}, {0, 0, 0}, {0, 1, 0}, 360));  // no direction
        assert(!v.set({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 0));
    }
    printf("guest_live_test: ok\n");
    return 0;
}
