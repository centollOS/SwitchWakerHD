#pragma once
// Issue #144: Intel's Windows driver (UHD 620/630 class, 101.2079 and 101.2115) builds the depth/stencil
// state of an image view in vkCreateImageView, and only when it sees DEPTH_STENCIL_ATTACHMENT in the
// usage it derives for that view. vkCmdBeginRendering later reads that state without a check: a depth
// attachment view without it is a read of NULL+0x68 inside igvk64.dll (the shadow-map pass at the
// title screen). Attachment views can name their usage explicitly (VkImageViewUsageCreateInfo, core
// in Vulkan 1.1), which leaves the driver nothing to derive.
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstring>

namespace gfxvk {
constexpr uint32_t kVendorIntel = 0x8086;

// WWHD_VK_ATTACHMENT_VIEW_USAGE: "1" always, "0" never; unset (or anything else): Intel GPUs only
inline bool attachment_view_usage_wanted(uint32_t vendorID, const char* env) {
    if (env && !std::strcmp(env, "1")) return true;
    if (env && !std::strcmp(env, "0")) return false;
    return vendorID == kVendorIntel;
}

// the usage an attachment view of an image with imageUsage names: the one attachment bit it is used
// for, and only when the image has it (a view's usage must be a subset of the image's); 0 = no
// VkImageViewUsageCreateInfo
inline VkImageUsageFlags attachment_view_usage(VkImageUsageFlags imageUsage, bool depth) {
    const VkImageUsageFlags want =
        depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return imageUsage & want;
}
}  // namespace gfxvk
