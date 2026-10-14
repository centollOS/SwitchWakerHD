#pragma once
#include "backend.h"
#include "deferred_dispatch.h"
#include "pass_cache.h"
#include <stdexcept>

namespace gfxvk {
struct PassBegin {
    PassFormats formats;
    // Actual tracked layouts, sampled at recording time after explicit barriers.
    std::array<VkImageLayout, 9> layouts{};
};
struct PreparedPass {
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    std::array<VkClearValue, 9> clears{};
    void begin(VkCommandBuffer cmd, const PassApi& api) const {
        auto rebound = info; rebound.pClearValues = clears.data();
        api.beginRenderPass(cmd, &rebound, VK_SUBPASS_CONTENTS_INLINE);
    }
};
void init_pass_caches();
PassCaches& pass_caches();
PreparedPass prepare_pass(PassCaches&, const VkRenderingInfo&, const PassBegin&);
void begin_render_pass(VkCommandBuffer, const VkRenderingInfo&, const PassBegin&);
void end_render_pass(VkCommandBuffer);
void render_pass_pipeline_targets(VkGraphicsPipelineCreateInfo&, const VkPipelineRenderingCreateInfo&);
void forget_framebuffers(const VkImageView*, size_t, bool deferred);
void reset_pass_caches();
// Format/layout metadata is assembled only on the fallback path. The dynamic
// path keeps the existing VkRenderingInfo and deferred-recording implementation.
template<class Metadata> inline void begin_pass(VkCommandBuffer cmd, const VkRenderingInfo& info, Metadata metadata) {
    if (R.renderPasses) begin_render_pass(cmd, info, metadata());
    else record::vkCmdBeginRendering(cmd, &info);
}
inline void end_pass(VkCommandBuffer cmd) {
    if (R.renderPasses) end_render_pass(cmd);
    else record::vkCmdEndRendering(cmd);
}
inline void pipeline_targets(VkGraphicsPipelineCreateInfo& info, const VkPipelineRenderingCreateInfo& targets) {
    if (R.renderPasses) render_pass_pipeline_targets(info, targets);
    else info.pNext = &targets;
}
inline PassFormats pass_formats(const VkPipelineRenderingCreateInfo& info) {
    PassFormats formats; formats.colorCount = info.colorAttachmentCount;
    if (formats.colorCount > formats.colors.size()) throw std::invalid_argument("too many pass colour formats");
    for (uint32_t i = 0; i < formats.colorCount; ++i) formats.colors[i] = info.pColorAttachmentFormats[i];
    formats.depth = info.depthAttachmentFormat;
    formats.stencil = info.stencilAttachmentFormat != VK_FORMAT_UNDEFINED;
    return formats;
}
inline PassBegin color_pass(VkFormat format, VkImageLayout layout) {
    PassBegin pass; pass.formats.colorCount = 1; pass.formats.colors[0] = format; pass.layouts[0] = layout;
    return pass;
}
} // namespace gfxvk
