// overlay/release_gate.h (issue #153): what the game gets back after the settings overlay or the
// text prompt had the input. No SDL, overlay or game involved.
#include <cassert>
#include <cstdio>
#include "input_map.h"
#include "overlay/release_gate.h"

using namespace input_map;

namespace {
struct Device {
    float v[kPadCount] = {};
    overlay::ReleaseGate gate;
    // what the game gets this poll
    float get(int p, bool captured) {
        float out[kPadCount];
        std::copy(std::begin(v), std::end(v), out);
        gate.filter(out, captured);
        return out[p];
    }
};
}  // namespace

int main() {
    // nothing captured, nothing held back
    {
        Device pad;
        pad.v[kPadA] = 1;
        assert(pad.get(kPadA, false) == 1);
        assert(!pad.gate.holds_back());
    }
    // issue #153: inputs that rest above the old 0.3 release threshold (a trigger, a drifting stick, a
    // stuck input the game does not use) must not keep the game's input after the overlay closes
    {
        Device pad;
        pad.v[kPadRT] = 0.4f;
        pad.v[kPadLSRight] = 0.35f;
        pad.v[kPadHome] = 0.45f;
        for (int i = 0; i < 3; i++) assert(pad.get(kPadA, true) == 0);  // the overlay is open
        pad.v[kPadA] = 1;
        assert(pad.get(kPadA, true) == 0);
        pad.v[kPadA] = 0;
        assert(pad.get(kPadA, true) == 0);
        assert(pad.get(kPadA, false) == 0);  // closed (F1 / Esc)
        assert(!pad.gate.holds_back());
        pad.v[kPadA] = 1;
        assert(pad.get(kPadA, false) == 1);  // the next press is the game's
        assert(pad.get(kPadRT, false) == 0.4f && pad.get(kPadLSRight, false) == 0.35f);  // analog values pass
    }
    // the button that confirmed / closed is held across the close: it alone waits for its release
    {
        Device pad;
        pad.v[kPadA] = 1;
        assert(pad.get(kPadA, true) == 0);
        assert(pad.get(kPadA, false) == 0);
        assert(pad.gate.holds_back(kPadA));
        pad.v[kPadB] = 1;
        assert(pad.get(kPadB, false) == 1);  // another button at once
        pad.v[kPadA] = 0.5f;                 // at the game's threshold: released
        assert(pad.get(kPadA, false) == 0.5f);
        assert(!pad.gate.holds_back(kPadA));
        pad.v[kPadA] = 1;
        assert(pad.get(kPadA, false) == 1);
    }
    // a press and release inside the overlay leaves nothing behind
    {
        Device pad;
        pad.v[kPadMenu] = 1;
        pad.get(kPadMenu, true);
        pad.v[kPadMenu] = 0;
        pad.get(kPadMenu, true);
        assert(pad.get(kPadMenu, false) == 0 && !pad.gate.holds_back());
    }
    // a stick pushed while the overlay closes stays out until it returns, then moves the game again
    {
        Device pad;
        pad.v[kPadLSUp] = 1;
        pad.get(kPadLSUp, true);
        assert(pad.get(kPadLSUp, false) == 0);
        pad.v[kPadLSUp] = 0.2f;
        assert(pad.get(kPadLSUp, false) == 0.2f);
        pad.v[kPadLSUp] = 1;
        assert(pad.get(kPadLSUp, false) == 1);
    }
    // each device has its own gate: a second pad's same button is not held back by the first one's
    {
        Device one, two;
        one.v[kPadA] = 1;
        one.get(kPadA, true);
        two.get(kPadA, true);
        assert(one.get(kPadA, false) == 0);
        two.v[kPadA] = 1;
        assert(two.get(kPadA, false) == 1);
    }
    // opened and closed again while still held: still waits for that one release
    {
        Device pad;
        pad.v[kPadOptions] = 1;
        pad.get(kPadOptions, true);
        assert(pad.get(kPadOptions, false) == 0);
        assert(pad.get(kPadOptions, true) == 0);
        assert(pad.get(kPadOptions, false) == 0);
        pad.v[kPadOptions] = 0;
        assert(pad.get(kPadOptions, false) == 0);
        pad.v[kPadOptions] = 1;
        assert(pad.get(kPadOptions, false) == 1);
    }
    // DeviceGates: the per-device combination both hosts use (SDL joystick ids, AppKit GCControllers)
    {
        using Devices = std::vector<std::pair<int, overlay::PadValues>>;
        overlay::DeviceGates<int> gates;
        float raw[kPadCount], game[kPadCount];
        overlay::PadValues one{}, two{};
        // the menu is open: one holds A, two holds nothing yet; the game sees nothing, the overlay all
        one[kPadA] = 1;
        gates.combine(Devices{{1, one}, {2, two}}, true, raw, game);
        assert(raw[kPadA] == 1 && game[kPadA] == 0 && gates.size() == 2);
        // closed with A still held on one: two's A is the game's at once, one's waits
        gates.combine(Devices{{1, one}, {2, two}}, false, raw, game);
        assert(game[kPadA] == 0 && gates.holds_back(1, kPadA) && !gates.holds_back(2, kPadA));
        two[kPadA] = 1;
        gates.combine(Devices{{1, one}, {2, two}}, false, raw, game);
        assert(game[kPadA] == 1 && raw[kPadA] == 1);
        two[kPadA] = 0;
        gates.combine(Devices{{1, one}, {2, two}}, false, raw, game);
        assert(game[kPadA] == 0);
        // one's other inputs and a resting trigger pass meanwhile
        one[kPadB] = 1;
        one[kPadRT] = 0.4f;
        gates.combine(Devices{{1, one}, {2, two}}, false, raw, game);
        assert(game[kPadB] == 1 && game[kPadRT] == 0.4f && game[kPadA] == 0);
        one[kPadB] = 0;
        // one disconnects (missing from a poll) and comes back still holding A: a new gate
        gates.combine(Devices{{2, two}}, false, raw, game);
        assert(gates.size() == 1 && raw[kPadA] == 0);
        gates.combine(Devices{{1, one}, {2, two}}, false, raw, game);
        assert(game[kPadA] == 1 && !gates.holds_back(1, kPadA));
        // released: the gate forgets; no devices: everything zero
        one[kPadA] = 0;
        gates.combine(Devices{}, false, raw, game);
        assert(gates.size() == 0 && raw[kPadRT] == 0 && game[kPadRT] == 0);
    }
    puts("release_gate_test: passed");
}
