#pragma once
namespace overlay {
// Input-thread state shared across windows. Focus loss does not synthesize a release.
class ToggleGuard {
    bool held_ = false, seen_ = false;
    double last_ = 0;
public:
    bool press(bool down, bool repeat, double now) {
        if (!down) { held_ = false; return false; }
        if (repeat || held_) return false;
        held_ = true;
        if (seen_ && now - last_ < 0.150) return false;
        seen_ = true; last_ = now;
        return true;
    }
};
} // namespace overlay
