// Image and sampler descriptor slots and the sampler cache (dk_surfaces.h). P2 stub, replaced by the surface
// lane: nothing allocates descriptors before the lanes land; a call ends the game loudly (fatal).
#include "dk_surfaces.h"

#include "runtime.h"

namespace gfxdk {

namespace {
[[noreturn]] void not_yet(const char* what) {
    fatal("[dk] %s: not implemented yet (P2 stub in gfx/deko/descriptors.cpp)", what);
}
}  // namespace

uint32_t image_descriptor_alloc() { not_yet("image_descriptor_alloc"); }
void image_descriptor_write(uint32_t, const DkImageView&) { not_yet("image_descriptor_write"); }
void image_descriptor_free_later(uint32_t) {}
uint32_t sampler_id(const uint32_t*, bool, bool) { not_yet("sampler_id"); }
void commit_descriptors() {}
uint32_t null_image_id() { not_yet("null_image_id"); }
DescriptorStats descriptor_stats_take() { return {}; }

}  // namespace gfxdk
