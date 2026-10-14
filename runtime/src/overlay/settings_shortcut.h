#pragma once
#include "overlay.h"

namespace overlay {
// Driven by the input pump, not rendered frames. Times are monotonic seconds.
class SettingsShortcut {
    OpenReason reason_ = OpenReason::View;
    double since_ = -1;
    bool latched_ = false, home_ = false;
public:
    OpenReason reason() const { return reason_; }
    enum Action { None, Open, Close };
    Action poll(double now, bool select, bool home, bool open) {
        Action action = None;
        if (home && !home_) { action = open ? Close : Open; reason_ = OpenReason::Home; }
        home_ = home;
        if (!select) { since_ = -1; latched_ = false; }
        else if (!latched_) {
            if (open) { action = Close; latched_ = true; reason_ = OpenReason::View; }
            else {
                if (since_ < 0) since_ = now;
                if (now - since_ >= 0.5) { action = Open; latched_ = true; reason_ = OpenReason::View; }
            }
        }
        return action;
    }
};
}  // namespace overlay
