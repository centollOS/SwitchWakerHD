// (this fork, s2/resolution) Switch 2 output size and render resolution, kept apart:
// - Output (the window: swapchain images the display gets; settings.ini s2Output): auto (the default) is 1920x1080
//   handheld (the screen) and the TV's resolution docked (AM's default display resolution: 3840x2160 on a 4K TV);
//   1080 / 1440 / 2160 fix the docked size (handheld stays 1920x1080); "previous" is the old 1280x720 / 1920x1080.
//   The swapchain is recreated on dock / undock and when the choice changes (backend.cpp window_wanted).
// - Render (the game's internal resolution; s2Render): auto (the default) keeps the Picture tab's profile and its
//   dynamic resolution; native renders at the output height; 1440 / 1080 / 720 fix it. The non-auto choices turn
//   dynamic resolution off, so the cost shows. The present pass scales the picture into the window (bilinear, or FSR).
// - s2ShowSize: the render and output sizes in the FPS counter's corner.
#pragma once
#include <cstdint>

namespace s2res {
enum Output : int { kOutAuto, kOut1080, kOut1440, kOut2160, kOutPrevious, kOutputs };
enum Render : int { kRenderAuto, kRenderNative, kRender1440, kRender1080, kRender720, kRenders };
int output();
int render();
// the window size for a mode (render thread, every present: atomics only)
void output_size(bool docked, uint32_t& w, uint32_t& h);
// the internal resolution factor (of 1280x720) the render choice gives for a mode; false: auto (the profile decides)
bool render_scale(bool docked, float& scale);
// host loop, once a second (switch_settings::tick): the TV's resolution while docked; true when the output size
// changed, so a native render size follows it
bool tick(bool docked);
bool show_size();  // the FPS counter's size line
}  // namespace s2res
