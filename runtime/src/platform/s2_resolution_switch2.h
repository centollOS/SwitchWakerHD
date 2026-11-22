// (this fork, s2/resolution) Switch 2 output size and render resolution, kept apart:
// - Output (the window: swapchain images the display gets; settings.ini s2Output): auto (the default) is 1920x1080
//   handheld (the screen) and the TV's resolution docked (AM's default display resolution: 3840x2160 on a 4K TV);
//   1080 / 1440 / 2160 fix the size in both modes; "previous" is the old 1280x720 / 1920x1080. Handheld (a 1920x1080
//   screen), 1440 / 2160 give a swapchain of that size only when the screen reports a buffer resolution level that
//   covers it (s2display::Caps.maxResLevel: code 1 << level, read as that many times 1920x1080's pixels; a guess, the
//   levels' sizes are not documented); otherwise the window stays 1920x1080 and the game renders at the chosen size
//   (with Render auto), downscaled into it: supersampling. WWHD_S2_HANDHELD_BIG=1 / 0 forces the one or the other.
//   The swapchain is recreated on dock / undock and when the choice changes (backend.cpp window_wanted).
// - Render (the game's internal resolution; s2Render): auto (the default) keeps the Picture tab's profile and its
//   dynamic resolution; native renders at the output height (the chosen one, also when handheld falls back to 1920x1080);
//   2160 / 1800 / 1440 / 1080 / 720 fix it, in both modes, whatever the output (2160 handheld: 3840x2160 downscaled
//   to the screen's 1920x1080). The non-auto choices turn
//   dynamic resolution off, so the cost shows. The present pass scales the picture into the window (bilinear up, a 4-tap box down, or FSR).
// - s2ShowSize: the render and output sizes in the FPS counter's corner.
#pragma once
#include <cstdint>

namespace s2res {
enum Output : int { kOutAuto, kOut1080, kOut1440, kOut2160, kOutPrevious, kOutputs };
enum Render : int { kRenderAuto, kRenderNative, kRender2160, kRender1800, kRender1440, kRender1080, kRender720, kRenders };
int output();
int render();
// the window size for a mode (render thread, every present: atomics only)
void output_size(bool docked, uint32_t& w, uint32_t& h);
// the output size the choice asks for (differs from output_size handheld when the screen can't take a larger buffer)
void chosen_output_size(bool docked, uint32_t& w, uint32_t& h);
// the internal resolution factor (of 1280x720) the render choice gives for a mode; false: auto (the profile decides)
bool render_scale(bool docked, float& scale);
// host loop, once a second (switch_settings::tick): the TV's resolution while docked and the handheld screen's buffer
// level; true when the output size or the handheld path changed, so the render size follows it
bool tick(bool docked);
bool show_size();  // the FPS counter's size line
}  // namespace s2res
