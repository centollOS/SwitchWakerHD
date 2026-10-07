// The deko3d renderer's draw path (dk_draw.h). P2 stub, replaced by the draw lane: GX2 draws are counted,
// not executed (as in P1).
#include "dk_draw.h"

#include "dk_surfaces.h"

namespace gfxdk {

bool g_traceFrame = false, g_captureDraws = false;

void draw(const uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { R.counts.draws++; }
void draw_frame_start() {}
void submit_commands(const char*) {}

bool gamepad_only(const Surface*) { return false; }
bool skip_gamepad() { return false; }

void trace_pass(const std::array<Surface*, 8>&, const Surface*) {}
void trace_draw(const Surface*) {}
void trace_event(const char*, ...) {}
std::string trace_name(const Surface*) { return "?"; }
void gpu_pass_mark(const char*, const Surface*, const Surface*) {}

}  // namespace gfxdk
