#include "gfx/vulkan/pass_cache.h"
#include <cassert>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace {
template<class T> T handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return T(value);
}
uintptr_t nextHandle = 100;
uint32_t passCreates = 0, passDestroys = 0, framebufferCreates = 0, framebufferDestroys = 0;
bool failFramebuffer = false;
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice, const VkRenderPassCreateInfo* info,
        const VkAllocationCallbacks*, VkRenderPass* out) {
    assert(info->subpassCount == 1 && info->dependencyCount == 0);
    const auto& subpass = info->pSubpasses[0];
    assert(subpass.colorAttachmentCount == 3);
    assert(subpass.pColorAttachments[0].attachment == 0);
    assert(subpass.pColorAttachments[1].attachment == VK_ATTACHMENT_UNUSED);
    assert(subpass.pColorAttachments[2].attachment == 1);
    assert(subpass.pDepthStencilAttachment->attachment == 2);
    assert(info->attachmentCount == 3);
    for (uint32_t i = 0; i < 3; ++i) {
        const auto& attachment = info->pAttachments[i];
        const auto layout = i == 2 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        assert(attachment.initialLayout == layout && attachment.finalLayout == layout);
        assert(attachment.samples == VK_SAMPLE_COUNT_1_BIT);
        assert(attachment.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
        if (i == 2) {
            assert(attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
            assert(attachment.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
            assert(attachment.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE);
        } else assert(subpass.pColorAttachments[i == 0 ? 0 : 2].layout == layout);
    }
    *out = handle<VkRenderPass>(nextHandle++); ++passCreates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_pass(VkDevice, VkRenderPass, const VkAllocationCallbacks*) { ++passDestroys; }
VKAPI_ATTR VkResult VKAPI_CALL create_framebuffer(VkDevice, const VkFramebufferCreateInfo* info,
        const VkAllocationCallbacks*, VkFramebuffer* out) {
    if (failFramebuffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(info->layers == 1 && info->attachmentCount == 3);
    assert(info->pAttachments[0] && info->pAttachments[1] && info->pAttachments[2]);
    *out = handle<VkFramebuffer>(nextHandle++); ++framebufferCreates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL destroy_framebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) { ++framebufferDestroys; }
}
int main() {
    using namespace gfxvk;
    PassFormats formats;
    formats.colors[0] = VK_FORMAT_R8G8B8A8_UNORM;
    formats.colors[2] = VK_FORMAT_R16G16B16A16_SFLOAT;
    formats.colorCount = 3; formats.depth = VK_FORMAT_D32_SFLOAT_S8_UINT; formats.stencil = true;
    RenderPassKey load{formats, VK_ATTACHMENT_LOAD_OP_LOAD}, clear{formats, VK_ATTACHMENT_LOAD_OP_CLEAR};
    assert(!(load == clear));
    auto other = load; other.formats.stencil = false; assert(!(other == load));
    other = load; other.formats.colorCount = 4; assert(!(other == load));
    other = load; other.formats.colors[1] = VK_FORMAT_R8_UNORM; assert(!(other == load));
    other = load; other.formats.depth = VK_FORMAT_D24_UNORM_S8_UINT; assert(!(other == load));
    assert(RenderPassKeyHash{}(load) == RenderPassKeyHash{}(RenderPassKey{load}));
    PassApi api{create_pass, destroy_pass, create_framebuffer, destroy_framebuffer};
    {
        PassCaches cache(handle<VkDevice>(1), api);
        const auto pass = cache.render_pass(formats, VK_ATTACHMENT_LOAD_OP_LOAD);
        assert(cache.render_pass(formats, VK_ATTACHMENT_LOAD_OP_LOAD) == pass && passCreates == 1);
        const auto clearPass = cache.render_pass(formats, VK_ATTACHMENT_LOAD_OP_CLEAR);
        assert(clearPass != pass && passCreates == 2);
        // Pipeline workers and the recording thread share pass compatibility.
        std::thread worker([&] { for (int i = 0; i < 100; ++i)
            assert(cache.render_pass(formats, VK_ATTACHMENT_LOAD_OP_LOAD) == pass); });
        for (int i = 0; i < 100; ++i) assert(cache.render_pass(formats, VK_ATTACHMENT_LOAD_OP_CLEAR) == clearPass);
        worker.join();
        FramebufferKey key{pass, {}, 640, 360};
        key.views[0] = handle<VkImageView>(10); key.views[2] = handle<VkImageView>(11); key.views[8] = handle<VkImageView>(12);
        auto second = key; second.width++; assert(!(second == key));
        second = key; second.height++; assert(!(second == key));
        second = key; second.compatiblePass = clearPass; assert(!(second == key));
        second = key; second.views[2] = handle<VkImageView>(13); assert(!(second == key));
        const auto first = cache.framebuffer(formats, key);
        assert(cache.framebuffer(formats, key) == first && framebufferCreates == 1);
        const auto unrelated = cache.framebuffer(formats, second);
        auto resized = key; resized.width = 1280;
        cache.framebuffer(formats, resized);
        assert(cache.framebuffer_count() == 3);
        const VkImageView deferred[] = {key.views[2], key.views[2], VK_NULL_HANDLE};
        auto retired = cache.forget_views(deferred, 3);
        assert(retired.size() == 2 && cache.framebuffer_count() == 1);
        assert(framebufferDestroys == 0); // Deferral removes lookup entries, not live GPU objects.
        assert(cache.framebuffer(formats, second) == unrelated);
        // Same numerical view handle reused while old objects are still pending:
        // creation must allocate a NEW framebuffer, never hit the retired one.
        const auto reused = cache.framebuffer(formats, key);
        assert(reused != first && framebufferCreates == 4);
        assert(cache.forget_views(deferred, 3).size() == 1);
        // The preceding retirement is deliberately retained until its fence too.
        const auto sharedRetired = cache.forget_views(&key.views[0], 1);
        assert(sharedRetired.size() == 1 && sharedRetired[0] == unrelated);
        assert(cache.forget_views(key.views.data(), key.views.size()).empty());
        assert(cache.framebuffer_count() == 0);
        cache.destroy_framebuffers(retired);
        cache.destroy_framebuffers({reused}); cache.destroy_framebuffers(sharedRetired);
        assert(framebufferDestroys == 4);
        failFramebuffer = true;
        bool threw = false;
        try { cache.framebuffer(formats, key); } catch (const std::runtime_error&) { threw = true; }
        assert(threw && cache.framebuffer_count() == 0);
        assert(cache.forget_views(key.views.data(), key.views.size()).empty());
        failFramebuffer = false;
        cache.framebuffer(formats, key);
        assert(cache.framebuffers_created() == 5);
        cache.reset();
        assert(framebufferDestroys == 5 && passDestroys == 2);
    }
    assert(passDestroys == 2 && framebufferDestroys == 5); // Reset/destructor idempotence.
}
