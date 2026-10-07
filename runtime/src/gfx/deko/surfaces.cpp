// The deko3d renderer's surfaces (dk_surfaces.h). P2 stub, replaced by the surface lane: the GX2 operations
// are counted, not executed (as in P1); lookups find nothing. A function that would have to return a real
// resource ends the game loudly (fatal) instead: nothing calls it before the lanes land.
#include "dk_surfaces.h"

#include "runtime.h"

namespace gfxdk {

SurfaceSet S;

uint64_t next_write_seq() {
    static uint64_t seq = 0;
    return ++seq;
}

namespace {
[[noreturn]] void not_yet(const char* what) {
    fatal("[dk] %s: not implemented yet (P2 stub in gfx/deko/surfaces.cpp)", what);
}
}  // namespace

void scan_flush() { S.scanSrc = nullptr; }

Surface* find_or_create_surface(const SurfaceDesc&, bool) { return nullptr; }
Surface* color_target(const uint32_t*, int, uint32_t* slice) {
    if (slice) *slice = 0;
    return nullptr;
}
Surface* depth_target(const uint32_t*, uint32_t* slice) {
    if (slice) *slice = 0;
    return nullptr;
}
Surface* surface_from_color_buffer(uint32_t, uint32_t*, uint32_t*) { return nullptr; }
Surface* surface_from_depth_buffer(uint32_t, uint32_t*, uint32_t*) { return nullptr; }
Surface* sampled_texture(const uint32_t*, bool, bool* unique) {
    if (unique) *unique = false;
    return nullptr;
}
void upload_surface(Surface*) {}
uint32_t sampled_view_id(Surface*, const uint32_t*) { not_yet("sampled_view_id"); }
void target_view(Surface*, uint32_t, uint32_t, DkImageView*) { not_yet("target_view"); }
void create_surface_image(Surface*) { not_yet("create_surface_image"); }
void destroy_surface_image(Surface*) {}
Surface* feedback_copy(Surface* s) { return s; }
void blit(Surface*, uint32_t, uint32_t, uint32_t, uint32_t, Surface*, uint32_t, uint32_t, uint32_t, uint32_t) {
    not_yet("blit");
}

void clear_color(const uint32_t*, uint32_t, const float*) { R.counts.clears++; }
void clear_depth_stencil(const uint32_t*, uint32_t, float, uint32_t, uint32_t) { R.counts.clears++; }
void copy_surface(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { R.counts.copies++; }
void copy_to_scan(uint32_t, uint32_t) { R.counts.scans++; }
void invalidate(uint32_t flags, uint32_t, uint32_t) {
    R.counts.invalidates++;
    if (flags & 0x5) R.streamGen++;  // attribute buffers or uniform blocks (as gfx/gl)
}
void ss_reset_surfaces() {}

PresentSource present_source() { return {}; }

float res_scale() { return 1.0f; }
void set_res_scale(float) {}
void latch_res_scale() {}
void rescale_surface(Surface*, float, bool, bool) {}
bool fit_scale(Surface*, bool, bool) { return true; }

void surfaces_frame_start() {}
void surface_memory(size_t& total, size_t& targets, size_t& count) { total = targets = count = 0; }

}  // namespace gfxdk
