#pragma once

namespace controller_selection {
// Screen order is GamePad, Pro Controller. An unrecognised configuration or
// incomplete screen leaves the game's prompt available for manual selection.
constexpr int decision(bool enabled, bool title, int configured, bool gamepad, bool pro) {
    if (!enabled || !title) return -1;
    if (configured == 0 && gamepad) return 0;
    if (configured == 1 && pro) return 1;
    return -1;
}
} // namespace controller_selection
