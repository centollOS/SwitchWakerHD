// A platform port's own renderer (issue #185): a port fills a render::Backend table for its graphics
// API and registers it here, without editing renderer.h / renderer.cpp, decompiler_glue.cpp or
// gx2_core.cpp. It runs as Api::Platform.
//
// Registration: once, on the main thread, before render::choose(), which seals the registry. A later
// call, or a second backend, changes nothing and logs why. No locks are needed after that, as
// everything here is read only.
//
// A registered backend can be selected with --renderer=<key> or WWHD_RENDERER_RUNTIME=<key>, and is the
// default with make_default. It is not offered in the Renderer menu and does not count for
// render::can_choose(). If it cannot start, the game falls back to Metal where Metal is built in, as for
// Vulkan.
//
// Caches: Metal's and Vulkan's shader and pipeline caches stay theirs. With Api::Platform active, the
// prepared shaders (shader_prebuild.h) are not activated, a shader warm-up run (--warm-shaders) ends
// with RENDERER_UNAVAILABLE, and the Graphics tab's shader cache section is left out. A platform backend
// keeps its own caches, named after its key (for example shadercache/<key>-*.bin).
//
// Nothing registered: the renderer choice, names and the GX2 layer behave as before.
#pragma once
#include "renderer.h"

namespace render::ext {

struct PlatformBackend {
    const char* name;             // "deko3d": log, window title, reports
    const char* key;              // "deko3d": --renderer=, WWHD_RENDERER_RUNTIME (case-insensitive)
    const Backend* table;         // filled like vulkan_backend(); table->api == Api::Platform
    bool make_default = true;     // chosen when nothing asks for another renderer
    // sets the shader decompiler's output flavour (g_renderer, decompiler_glue.cpp); null: Vulkan's GLSL,
    // which needs the Vulkan renderer built in
    void (*select_decompiler)() = nullptr;
};
// Copies the description; name, key and table must stay valid for the whole run.
void register_backend(const PlatformBackend& backend);
const PlatformBackend* registered();  // null: none
void seal();                          // render::choose()
bool sealed();

// Tests only: forget the registration and unseal.
void reset_for_test();

}  // namespace render::ext
