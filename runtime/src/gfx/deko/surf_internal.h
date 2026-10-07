// What the surface lane's files share among themselves (surfaces.cpp, formats.cpp, descriptors.cpp); the
// other lanes use dk_surfaces.h only.
#pragma once
#include <deko3d.h>

#include <cstdint>

#include "dk_surfaces.h"

namespace gfxdk {

// formats.cpp: format_info without the log line for an unsupported format (the self-check probes all)
FormatInfo format_lookup(uint32_t gx2Format, bool isDepth);
// formats.cpp: what deko3d allows an image of that format (the debug library rejects other flags)
bool format_can_render(DkImageFormat f);  // DkImageFlags_UsageRender
bool format_can_2d(DkImageFormat f);      // DkImageFlags_Usage2DEngine (dkCmdBufBlitImage)

// descriptors.cpp: from surfaces_frame_start, after the frame's fence: slots freed by finished frames return
void descriptors_frame_start();
// descriptors.cpp: slots in use now (the surfaces' 5 s line)
void descriptor_usage(uint32_t& images, uint32_t& samplers);

}  // namespace gfxdk
