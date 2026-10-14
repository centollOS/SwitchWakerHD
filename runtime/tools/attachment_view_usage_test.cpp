// issue #144: attachment views name their usage on Intel (attachment_view_usage.h)
#include "gfx/vulkan/attachment_view_usage.h"
#include <cassert>

int main() {
    using namespace gfxvk;
    // default: Intel only
    assert(attachment_view_usage_wanted(0x8086, nullptr));
    assert(!attachment_view_usage_wanted(0x10DE, nullptr));
    assert(!attachment_view_usage_wanted(0x1002, nullptr));
    assert(!attachment_view_usage_wanted(0x106B, ""));
    // WWHD_VK_ATTACHMENT_VIEW_USAGE=1 / =0 override the vendor check
    assert(attachment_view_usage_wanted(0x10DE, "1"));
    assert(!attachment_view_usage_wanted(0x8086, "0"));
    assert(attachment_view_usage_wanted(0x8086, "auto"));
    assert(!attachment_view_usage_wanted(0x10DE, "auto"));
    // the one attachment bit, and only when the image has it (a view's usage is a subset of the image's)
    const VkImageUsageFlags base =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    assert(attachment_view_usage(base | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, true) ==
           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    assert(attachment_view_usage(base | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false) ==
           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    assert(attachment_view_usage(base, true) == 0);
    assert(attachment_view_usage(base, false) == 0);
    assert(attachment_view_usage(base | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, true) == 0);
    assert(attachment_view_usage(base | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false) == 0);
}
