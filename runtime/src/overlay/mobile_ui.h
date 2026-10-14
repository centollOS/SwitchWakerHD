#pragma once
#include <algorithm>
#include <cmath>
namespace mobile_ui {
// A standard ImGui control is 17 font pixels + two 16-pixel vertical paddings.
// Automatic/Medium target 8 mm; Small and Large give 7 and 9 mm respectively.
inline float scale(float dpi, float density, int preset) {
    if (!std::isfinite(dpi) || dpi < 72 || dpi > 1000)
        dpi = 160 * std::clamp(std::isfinite(density) ? density : 1.0f, 1.0f, 6.0f);
    const float mm = preset == 1 ? 7.0f : preset == 3 ? 9.0f : 8.0f;
    return std::clamp(dpi * mm / (25.4f * 49.0f), 1.0f, 12.0f);
}
}  // namespace mobile_ui
