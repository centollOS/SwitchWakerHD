// Definitions the vendored Cemu decompiler expects from its host.
#ifdef WWHD_HAS_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#ifdef WWHD_HAS_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif
#ifdef WWHD_HAS_DEKO3D
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#endif
#include "gfx/renderer.h"
#include "runtime.h"

// The decompiler emits MSL or GLSL depending on g_renderer's type: set once at start-up (and again if
// Vulkan cannot start and the game falls back to Metal), before any shader is translated. deko3d uses
// the OpenGL mode: the same GLSL, so the same shader cache keys (docs/deko3d-plan.md, section 1).
#ifdef WWHD_HAS_METAL
std::unique_ptr<Renderer> g_renderer = std::make_unique<MetalRenderer>();
#elif defined(WWHD_HAS_DEKO3D)
std::unique_ptr<Renderer> g_renderer = std::make_unique<OpenGLRenderer>();
#else
std::unique_ptr<Renderer> g_renderer = std::make_unique<VulkanRenderer>();
#endif
void select_decompiler_api(render::Api api) {
#ifdef WWHD_HAS_VULKAN
    if (api == render::Api::Vulkan) {
        g_renderer = std::make_unique<VulkanRenderer>();
        return;
    }
#endif
#ifdef WWHD_HAS_DEKO3D
    if (api == render::Api::Deko3D) {
        g_renderer = std::make_unique<OpenGLRenderer>();
        return;
    }
#endif
#ifdef WWHD_HAS_METAL
    g_renderer = std::make_unique<MetalRenderer>();
#endif
}
void cemu_shim_log(const std::string& msg) { LOG("[decompiler] %s", msg.c_str()); }
