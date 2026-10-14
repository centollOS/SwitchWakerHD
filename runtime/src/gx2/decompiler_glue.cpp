// Definitions the vendored Cemu decompiler expects from its host.
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#ifdef WWHD_HAS_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#ifdef WWHD_HAS_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif
#include "gfx/renderer.h"
#include "gfx/renderer_ext.h"
#include "runtime.h"

// The decompiler emits MSL or GLSL depending on g_renderer's type: set once at start-up (and again if
// Vulkan cannot start and the game falls back to Metal), before any shader is translated.
#if defined(WWHD_HAS_METAL)
std::unique_ptr<Renderer> g_renderer = std::make_unique<MetalRenderer>();
#elif defined(WWHD_HAS_VULKAN)
std::unique_ptr<Renderer> g_renderer = std::make_unique<VulkanRenderer>();
#else
std::unique_ptr<Renderer> g_renderer;  // a platform backend's select_decompiler sets it (renderer_ext.h)
#endif
void select_decompiler_api(render::Api api) {
    if (api == render::Api::Platform) {
        const render::ext::PlatformBackend* p = render::ext::registered();
        if (p && p->select_decompiler) {
            p->select_decompiler();
            return;
        }
        api = render::Api::Vulkan;  // no select_decompiler: Vulkan's GLSL
    }
#ifdef WWHD_HAS_VULKAN
    if (api == render::Api::Vulkan) {
        g_renderer = std::make_unique<VulkanRenderer>();
        return;
    }
#endif
#ifdef WWHD_HAS_METAL
    g_renderer = std::make_unique<MetalRenderer>();
#endif
}
void cemu_shim_log(const std::string& msg) { LOG("[decompiler] %s", msg.c_str()); }
