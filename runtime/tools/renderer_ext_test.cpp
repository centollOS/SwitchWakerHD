// gfx/renderer_ext.h (issue #185): a platform port's own renderer. Built like the macOS default (Metal
// and Vulkan in one executable) with stub backend tables: nothing registered keeps today's choice and
// names; a registered backend is chosen by its key or as the default, stays out of can_choose(), and a
// second registration or one after choose() is ignored with a log line. Also the two GX2 decisions of
// gx2_core.cpp (gx2/backend_hooks.h) with a stub backend: GX2DrawDone for a save state and the
// shader-key mask of a register change.
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "gfx/renderer.h"
#include "gfx/renderer_ext.h"
#include "gx2/backend_hooks.h"

// ---------------------------------------------------------------- what renderer.cpp needs from the game
static std::vector<std::string> g_log;
void log_msg(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_log.push_back(line);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
namespace audio { void finish_dump() {} }
namespace screenshot { void finish() {} }
static std::string g_saved;  // display.plist "renderer"
namespace gfx {
bool host_setting(const char* key, std::string& value) {
    if (strcmp(key, "renderer") || g_saved.empty()) return false;
    value = g_saved;
    return true;
}
void set_host_setting(const char*, const std::string& value) { g_saved = value; }
void show_startup_notice(const std::string&, const std::string&) {}
}  // namespace gfx
static std::vector<render::Api> g_decompiler;
void select_decompiler_api(render::Api api) { g_decompiler.push_back(api); }  // gx2/decompiler_glue.cpp

// ---------------------------------------------------------------- stub backends
namespace {

// the environment, also on Windows (no setenv there; an empty value removes the variable)
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

int g_inits[3], g_waits, g_flushes, g_idles, g_writebacks;
bool g_platform_fails = false;

void count_wait() { ++g_waits; }
void count_flush() { ++g_flushes; }
void count_idle() { ++g_idles; }
void count_writeback() { ++g_writebacks; }

render::Backend table(render::Api api) {
    render::Backend b{};
    b.api = api;
    b.wait_idle = count_wait;
    b.guest_flush = count_flush;
    b.write_back = count_writeback;
    return b;
}

// the platform's shader key ignores bit 0 of register 0x100 and all of 0x101
bool stub_key_mask(uint32_t reg, uint32_t& mask) {
    if (reg == 0x100) { mask = ~1u; return true; }
    if (reg == 0x101) { mask = 0; return true; }
    return false;
}

const render::Backend& platform_table() {
    static const render::Backend b = [] {
        render::Backend b = table(render::Api::Platform);
        b.init = [] {
            ++g_inits[2];
            if (g_platform_fails) throw std::runtime_error("no GPU");
        };
        b.gpu_idle = count_idle;
        b.shader_key_mask = stub_key_mask;
        return b;
    }();
    return b;
}

render::ext::PlatformBackend stub(const char* name = "deko3d", const char* key = "deko3d") {
    render::ext::PlatformBackend p;
    p.name = name;
    p.key = key;
    p.table = &platform_table();
    return p;
}

}  // namespace

namespace render {
const Backend& metal_backend() {
    static const Backend b = [] {
        Backend b = table(Api::Metal);
        b.init = [] { ++g_inits[0]; };
        return b;
    }();
    return b;
}
const Backend& vulkan_backend() {
    static const Backend b = [] {
        Backend b = table(Api::Vulkan);
        b.init = [] { ++g_inits[1]; };
        return b;
    }();
    return b;
}
}  // namespace render

namespace {

using render::Api;

bool logged(const char* text) {
    for (const auto& l : g_log)
        if (l.find(text) != std::string::npos) return true;
    return false;
}

void reset() {
    render::ext::reset_for_test();
    render::g_backend = nullptr;
    g_saved.clear();
    g_log.clear();
    g_decompiler.clear();
    unset_env("WWHD_RENDERER_RUNTIME");
    g_platform_fails = false;
    for (int& n : g_inits) n = 0;
}

void choose(std::vector<const char*> args) {
    args.insert(args.begin(), "wwhd");
    render::choose(int(args.size()), const_cast<char**>(args.data()));
}

// nothing registered: today's renderer choice and names
void unchanged() {
    reset();
    assert(!render::ext::registered());
    assert(!strcmp(render::api_name(Api::Metal), "Metal") && !strcmp(render::api_name(Api::Vulkan), "Vulkan"));
    assert(!strcmp(render::api_key(Api::Metal), "metal") && !strcmp(render::api_key(Api::Vulkan), "vulkan"));
    assert(render::compiled(Api::Metal) && render::compiled(Api::Vulkan) && !render::compiled(Api::Platform));
    assert(render::can_choose());
    assert(render::preferred() == Api::Metal);  // default_api()
    choose({});
    assert(render::requested() == Api::Metal && logged("renderer: Metal (default)"));
    choose({"--renderer=vulkan"});
    assert(render::requested() == Api::Vulkan && logged("renderer: Vulkan (command line)"));
    choose({"--renderer", "moltenvk"});
    assert(render::requested() == Api::Vulkan);
    g_log.clear();
    choose({"--renderer=deko3d"});  // not a renderer without a registration
    assert(render::requested() == Api::Metal && logged("unknown renderer 'deko3d' (metal or vulkan)"));
    set_env("WWHD_RENDERER_RUNTIME", "vulkan");
    choose({});
    assert(render::requested() == Api::Vulkan);
    unset_env("WWHD_RENDERER_RUNTIME");
    g_saved = "vulkan";
    assert(render::preferred() == Api::Vulkan);
    choose({});
    assert(render::requested() == Api::Vulkan && logged("renderer: Vulkan (saved setting)"));
    render::init();
    assert(render::g_backend == &render::vulkan_backend() && g_inits[1] == 1);
    assert(g_decompiler.size() == 1 && g_decompiler[0] == Api::Vulkan);
    assert(!render::platform());
    reset();
}

// a registered backend: its key and names, the default, out of can_choose()
void registered() {
    reset();
    render::ext::register_backend(stub());
    const render::ext::PlatformBackend* p = render::ext::registered();
    assert(p && !strcmp(p->name, "deko3d") && p->table == &platform_table());
    assert(!strcmp(render::api_name(Api::Platform), "deko3d") && !strcmp(render::api_key(Api::Platform), "deko3d"));
    assert(!strcmp(render::api_name(Api::Metal), "Metal") && !strcmp(render::api_name(Api::Vulkan), "Vulkan"));
    assert(render::compiled(Api::Platform) && render::can_choose());
    assert(render::preferred() == Api::Platform);  // make_default
    choose({});
    assert(render::ext::sealed());
    assert(render::requested() == Api::Platform && logged("renderer: deko3d (default)"));
    render::init();
    assert(render::g_backend == &platform_table() && render::platform() && !render::vulkan());
    assert(render::active() == Api::Platform && g_inits[2] == 1 && logged("deko3d renderer started"));
    assert(g_decompiler.size() == 1 && g_decompiler[0] == Api::Platform);
    // the command line and environment still pick Metal or Vulkan; the key picks the platform backend
    choose({"--renderer=vulkan"});
    assert(render::requested() == Api::Vulkan);
    choose({"--renderer=DEKO3D"});
    assert(render::requested() == Api::Platform && logged("renderer: deko3d (command line)"));
    g_log.clear();
    choose({"--renderer=gl"});
    assert(render::requested() == Api::Platform && logged("unknown renderer 'gl' (metal, vulkan or deko3d)"));
    set_env("WWHD_RENDERER_RUNTIME", "metal");
    choose({});
    assert(render::requested() == Api::Metal);
    unset_env("WWHD_RENDERER_RUNTIME");
    // a saved Graphics > Renderer choice wins over make_default, as over Metal today
    g_saved = "vulkan";
    choose({});
    assert(render::requested() == Api::Vulkan);

    // make_default = false: Metal stays the default, the key still selects it
    reset();
    render::ext::PlatformBackend opt = stub();
    opt.make_default = false;
    render::ext::register_backend(opt);
    assert(render::preferred() == Api::Metal);
    choose({});
    assert(render::requested() == Api::Metal);
    choose({"--renderer=deko3d"});
    assert(render::requested() == Api::Platform);

    // cannot start: Metal, as after a Vulkan failure
    reset();
    render::ext::register_backend(stub());
    g_platform_fails = true;
    choose({});
    render::init();
    assert(render::g_backend == &render::metal_backend() && g_inits[2] == 1 && g_inits[0] == 1);
    assert(render::requested() == Api::Platform && render::active() == Api::Metal);
    assert(logged("deko3d renderer could not start: no GPU -- falling back to Metal"));
    assert(g_decompiler.size() == 2 && g_decompiler[1] == Api::Metal);
    reset();
}

// registry rules: one backend, before choose(), a usable description
void rules() {
    reset();
    render::ext::register_backend(stub());
    render::ext::register_backend(stub("gl", "gl"));
    assert(!strcmp(render::ext::registered()->name, "deko3d"));
    assert(logged("a platform renderer backend is already registered (deko3d); gl ignored"));
    choose({"--renderer=gl"});
    assert(render::requested() == Api::Platform);  // "gl" unknown: the default, deko3d

    reset();
    choose({});
    render::ext::register_backend(stub());
    assert(!render::ext::registered() && render::requested() == Api::Metal);
    assert(logged("renderer backend registered too late (deko3d): register at start-up, before render::choose()"));

    reset();
    render::ext::PlatformBackend wrong = stub();
    wrong.table = &render::vulkan_backend();  // must be an Api::Platform table
    render::ext::register_backend(wrong);
    render::ext::PlatformBackend nokey = stub();
    nokey.key = "";
    render::ext::register_backend(nokey);
    assert(!render::ext::registered() && logged("renderer backend deko3d ignored"));
    reset();
}

// gx2_core.cpp's OP_DRAW_DONE (gx2::draw_done) and shader-key mask (gx2::pick_shader_key_mask)
void gx2_hooks() {
    auto counts = [](int w, int f, int i, int wb) {
        const bool ok = g_waits == w && g_flushes == f && g_idles == i && g_writebacks == wb;
        g_waits = g_flushes = g_idles = g_writebacks = 0;
        return ok;
    };
    // platform backend: a save state (payload 1) waits with gpu_idle, a plain GX2DrawDone with
    // wait_idle (lazy DrawDone is Vulkan's)
    render::g_backend = &platform_table();
    gx2::draw_done(false, true);
    assert(counts(0, 0, 1, 1));
    gx2::draw_done(false, false);
    assert(counts(1, 0, 0, 1));
    // Vulkan / Metal (no gpu_idle): wait_idle for a save state, as before; lazy queues the work
    render::g_backend = &render::vulkan_backend();
    gx2::draw_done(false, true);
    assert(counts(1, 0, 0, 1));
    gx2::draw_done(true, true);
    assert(counts(1, 0, 0, 1));
    gx2::draw_done(true, false);
    assert(counts(0, 1, 0, 1));
    render::g_backend = &render::metal_backend();
    gx2::draw_done(false, true);
    assert(counts(1, 0, 0, 1));

    // the platform's mask decides, without WWHD_VK_SHADER_KEY_DIRTY
    unset_env("WWHD_VK_SHADER_KEY_DIRTY");
    const gx2::ShaderKeyMask platform = gx2::pick_shader_key_mask(&platform_table());
    assert(platform == stub_key_mask);
    assert(!gx2::shader_key_changed(platform, 0x100, 0x10, 0x11));  // bit 0 only: same key
    assert(gx2::shader_key_changed(platform, 0x100, 0x10, 0x30));
    assert(!gx2::shader_key_changed(platform, 0x101, 1, 2));        // mask 0: never
    assert(gx2::shader_key_changed(platform, 0x102, 1, 2));         // not in the mask: counts
    // Metal and Vulkan: as before (Vulkan's mask only with WWHD_VK_SHADER_KEY_DIRTY=1)
    assert(!gx2::pick_shader_key_mask(nullptr));
    assert(!gx2::pick_shader_key_mask(&render::metal_backend()));
    assert(!gx2::pick_shader_key_mask(&render::vulkan_backend()));
    assert(gx2::shader_key_changed(nullptr, 0x101, 1, 2));
    set_env("WWHD_VK_SHADER_KEY_DIRTY", "1");
    assert(gx2::pick_shader_key_mask(&render::vulkan_backend()) == gx2::vulkan_shader_key_mask);
    assert(!gx2::pick_shader_key_mask(&render::metal_backend()));
    assert(gx2::pick_shader_key_mask(&platform_table()) == stub_key_mask);
    unset_env("WWHD_VK_SHADER_KEY_DIRTY");
    const uint32_t prim = uint32_t(Latte::REGADDR::VGT_PRIMITIVE_TYPE);  // Vulkan's mask: bits 0-5
    assert(!gx2::shader_key_changed(gx2::vulkan_shader_key_mask, prim, 0x04, 0x44));
    assert(gx2::shader_key_changed(gx2::vulkan_shader_key_mask, prim, 0x04, 0x05));
    render::g_backend = nullptr;
}

}  // namespace

int main() {
    unchanged();
    registered();
    rules();
    gx2_hooks();
    printf("renderer extensions: unchanged without a registration, platform backend, registry rules, gx2 hooks passed\n");
    return 0;
}
