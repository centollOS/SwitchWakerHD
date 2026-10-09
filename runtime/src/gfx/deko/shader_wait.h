// When a draw whose shader is still being compiled waits for it instead of being skipped (option C of
// docs/shader-cache-from-dump-plan.md). A skipped draw that the game makes only once (when a place loads) leaves its
// result black for good (Outset's island on a first start), while waiting costs a few slow frames. So the renderer
// waits inside a window around the moments places load, and skips as before during play:
//   - while a scene change's fade runs (l_fopOvlpM_overlap, as the fast-scene-changes mod sees it) or a door event
//     runs (mods::door_event_running: doors that load a room without a fade), and kAfterFrames rendered frames after
//     either ends (counted in frames, so fast scene changes and quick doors do not shorten it);
//   - the first kBootFrames frames after start-up (the logos and the title).
// WWHD_DK_SHADER_WAIT: 0 never waits (as before), 1 the window (default), 2 always waits (= WWHD_DK_SHADER_BUDGET=0).
#pragma once
#include <cstdint>

namespace gfxdk::shader_wait {
void frame(uint64_t frame);  // the backend, at each frame's start: the window for this frame
bool active();               // translate(): wait for a compiling shader now?
}  // namespace gfxdk::shader_wait
