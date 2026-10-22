#pragma once
#include <vulkan/vulkan.h>

namespace gfxvk {
inline bool sampler_nearest_only(bool integer, bool compare, bool linearSupported) {
  // Comparison sampling has separate depth-comparison capabilities; the
  // ordinary linear-filter feature requirement applies when compare is off.
  return integer || (!compare && !linearSupported);
}
inline void restrict_sampler_filtering(VkSamplerCreateInfo& info, bool nearestOnly) {
  if (!nearestOnly) return;
  info.minFilter = info.magFilter = VK_FILTER_NEAREST;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.anisotropyEnable = VK_FALSE;
  info.maxAnisotropy = 1.0f;
}
}
