#pragma once
namespace touch_buttons {
enum Mode { Automatic, AlwaysShow, Hide };
inline bool visible(int mode, bool controller, bool revealed) {
    return revealed || mode == AlwaysShow || (mode == Automatic && !controller);
}
}  // namespace touch_buttons
