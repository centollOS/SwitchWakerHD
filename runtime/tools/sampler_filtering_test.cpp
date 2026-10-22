#include "gfx/vulkan/sampler_filtering.h"
#include <cassert>
#include <cstring>
#include <initializer_list>

int main() {
  using namespace gfxvk;
  VkSamplerCreateInfo requested{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  requested.minFilter = requested.magFilter = VK_FILTER_LINEAR;
  requested.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  requested.anisotropyEnable = VK_TRUE;
  requested.maxAnisotropy = 8.0f;
  requested.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  requested.minLod = 2.0f;
  requested.maxLod = 7.0f;
  requested.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  // Ordinary non-filterable float images must reject every linear path,
  // including mip interpolation and anisotropy, without integer border colors.
  auto fallback = requested;
  assert(sampler_nearest_only(false, false, false));
  restrict_sampler_filtering(fallback, sampler_nearest_only(false, false, false));
  assert(fallback.minFilter == VK_FILTER_NEAREST && fallback.magFilter == VK_FILTER_NEAREST);
  assert(fallback.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
  assert(!fallback.anisotropyEnable && fallback.maxAnisotropy == 1.0f);
  assert(fallback.borderColor == requested.borderColor);
  assert(fallback.addressModeU == requested.addressModeU);
  assert(fallback.minLod == requested.minLod && fallback.maxLod == requested.maxLod);
  // Filterable floating-point and depth comparison samplers retain their
  // original settings, even if ordinary depth filtering is unsupported.
  for (bool compare : {false, true}) {
    auto unchanged = requested;
    assert(!sampler_nearest_only(false, compare, true));
    restrict_sampler_filtering(unchanged, sampler_nearest_only(false, compare, true));
    assert(std::memcmp(&unchanged, &requested, sizeof(requested)) == 0);
  }
  auto comparison = requested;
  assert(!sampler_nearest_only(false, true, false));
  restrict_sampler_filtering(comparison, sampler_nearest_only(false, true, false));
  assert(std::memcmp(&comparison, &requested, sizeof(requested)) == 0);
  for (bool supported : {false, true})
    for (bool compare : {false, true})
      assert(sampler_nearest_only(true, compare, supported));
}
