#pragma once
#include "loader.h"
#include <array>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gfxvk {
struct PassFormats {
    std::array<VkFormat, 8> colors{};
    uint32_t colorCount = 0;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool stencil = false;
    bool operator==(const PassFormats&) const = default;
};
struct RenderPassKey {
    PassFormats formats;
    VkAttachmentLoadOp colorLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
    bool operator==(const RenderPassKey&) const = default;
};
struct FramebufferKey {
    VkRenderPass compatiblePass = VK_NULL_HANDLE; // Always the LOAD variant.
    std::array<VkImageView, 9> views{}; // Colour slots, including holes, then depth.
    uint32_t width = 0, height = 0;
    bool operator==(const FramebufferKey&) const = default;
};
struct RenderPassKeyHash { size_t operator()(const RenderPassKey&) const; };
struct FramebufferKeyHash { size_t operator()(const FramebufferKey&) const; };
// Device-owned dispatch: the Android independent presenter has another logical
// device and must never overwrite the primary renderer's entry points.
struct PassApi {
    PFN_vkCreateRenderPass createRenderPass = nullptr;
    PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer createFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkCmdBeginRenderPass beginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass endRenderPass = nullptr;
};
class PassCaches {
public:
    PassCaches(VkDevice device, PassApi api): device_(device), api_(api) {}
    PassCaches(const PassCaches&) = delete;
    PassCaches& operator=(const PassCaches&) = delete;
    ~PassCaches(); // Caller has drained commands AND joined pipeline workers.
    VkRenderPass render_pass(const PassFormats&, VkAttachmentLoadOp colorLoad);
    VkFramebuffer framebuffer(const PassFormats&, const FramebufferKey&);
    // Remove now, destroy later. This closes handle reuse even while a submission
    // retaining the old framebuffer is still pending (or captured, not emitted).
    std::vector<VkFramebuffer> forget_views(const VkImageView*, size_t count);
    void destroy_framebuffers(const std::vector<VkFramebuffer>&);
    void reset();
    const PassApi& api() const { return api_; }
    uint64_t framebuffers_created() const { return created_; }
    size_t framebuffer_count() const { return framebuffers_.size(); }
private:
    VkDevice device_;
    PassApi api_;
    // Startup pipeline workers may request a compatible pass concurrently with
    // primary recording. Framebuffer ownership stays on the recording thread.
    std::mutex renderPassMutex_;
    std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> renderPasses_;
    std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> framebuffers_;
    std::unordered_map<VkImageView, std::unordered_set<FramebufferKey, FramebufferKeyHash>> byView_;
    uint64_t created_ = 0;
};
} // namespace gfxvk
