// GX2 surface formats -> deko3d image formats (dk_surfaces.h). P2 stub, replaced by the surface lane (a port
// of gfx/vulkan/formats.cpp): every format is unsupported until then.
#include "dk_surfaces.h"

namespace gfxdk {

FormatInfo format_info(uint32_t, bool) { return {}; }
void convert_row(Convert, const uint8_t*, uint8_t*, uint32_t) {}

}  // namespace gfxdk
