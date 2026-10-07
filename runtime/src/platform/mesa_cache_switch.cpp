// Mesa's persistent shader cache on the Switch. The NRO links the Mesa that centollOS's
// build_mesa.sh builds (devkitPro's switch-mesa 20.1.0-5 recipe plus its patches: tools/switch/
// mesa/README.md): its disk shader cache keeps, in one file, the GLSL compiler's results (a program
// linked once is not compiled or linked again: glCompileShader is deferred, glLinkProgram loads the
// program) and nvc0's machine code. shadercache_gl.bin still lists which programs to make at start
// (gfx/gl/shaders.cpp); this cache makes each of them a load instead of a compile.
//
// WWHD_MESA_CACHE (env.txt): 1 (default) on; 0 off (MESA_SHADER_CACHE_DISABLE); reset deletes the
// cache file and its index first. The file is sdmc:/switch/wwhd/cache/mesa_shader_cache.bin unless
// MESA_SHADER_CACHE_DIR is set in env.txt. Without the patched Mesa (WWHD_MESA_STATS undefined:
// devkitPro's package) nothing is cached and the log says so.
#include "mesa_cache_switch.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(WWHD_MESA_STATS)
#include <mesa_switch.h>
#endif

#include "platform/host.h"
#include "runtime.h"

namespace mesa_cache {
namespace {
bool g_status_shown = false, g_maintenance_shown = false;
uint64_t g_last_signature = 0;
double ms(uint64_t ns) { return double(ns) / 1e6; }
}  // namespace

void setup() {
    const char* mode = getenv("WWHD_MESA_CACHE");
    if (mode && !strcmp(mode, "0")) {
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        LOG("[mesa] shader cache: off (WWHD_MESA_CACHE=0)");
        return;
    }
    if (!getenv("MESA_SHADER_CACHE_DIR")) {
        const std::string dir = host::config_dir() + "/cache";
        mkdir(dir.c_str(), 0777);
        setenv("MESA_SHADER_CACHE_DIR", dir.c_str(), 0);
    }
    const char* dir = getenv("MESA_SHADER_CACHE_DIR");
    if (mode && !strcmp(mode, "reset")) {
        for (const char* name : {"bin", "idx", "use"}) {
            char path[512];
            snprintf(path, sizeof path, "%s/mesa_shader_cache.%s", dir, name);
            if (unlink(path) == 0) LOG("[mesa] shader cache: WWHD_MESA_CACHE=reset: deleted %s", path);
        }
    }
#if defined(WWHD_MESA_STATS)
    LOG("[mesa] shader cache: MESA_SHADER_CACHE_DIR=%s", dir);
#else
    LOG("[mesa] shader cache: not in this build (devkitPro's Mesa)");
#endif
}

std::string report() {
#if defined(WWHD_MESA_STATS)
    std::string out;
    char line[1024];
    const char* status = mesa_switch_shader_cache_status();
    // Mesa writes its status when it creates the cache (eglInitialize); before that the default
    if (!g_status_shown && strncmp(status, "off (no shader cache", 20) != 0) {
        g_status_shown = true;
        snprintf(line, sizeof line, "[mesa] shader cache: %s\n", status);
        out += line;
    }
    const char* maintenance = mesa_switch_shader_cache_maintenance();
    if (maintenance[0] && !g_maintenance_shown) {
        g_maintenance_shown = true;
        snprintf(line, sizeof line, "[mesa] shader cache: %s\n", maintenance);
        out += line;
    }
    uint64_t s[MESA_SWITCH_STAT_COUNT] = {};
    mesa_switch_get_stats(s, MESA_SWITCH_STAT_COUNT);
    uint64_t signature = 0;
    for (uint64_t v : s) signature = signature * 31 + v;
    if (signature != g_last_signature) {
        g_last_signature = signature;
        snprintf(line, sizeof line,
                 "[mesa] shader compile: compiles %llu (%llu deferred) %.0f ms; links %llu (%llu from cache) %.0f ms "
                 "= glsl %.0f + st %.0f; nvc0 %llu (%llu from cache) %.0f ms; cache gets %llu (%llu hits, %llu damaged) "
                 "%.0f ms, puts %llu (%.1f MiB, %llu dropped) %.0f ms\n",
                 (unsigned long long)s[MESA_SWITCH_GLSL_COMPILES], (unsigned long long)s[MESA_SWITCH_GLSL_COMPILES_SKIPPED],
                 ms(s[MESA_SWITCH_GLSL_COMPILE_NS]), (unsigned long long)s[MESA_SWITCH_LINKS],
                 (unsigned long long)s[MESA_SWITCH_LINKS_FROM_CACHE], ms(s[MESA_SWITCH_LINK_NS]),
                 ms(s[MESA_SWITCH_LINK_GLSL_NS]), ms(s[MESA_SWITCH_LINK_ST_NS]),
                 (unsigned long long)s[MESA_SWITCH_NVC0_TRANSLATES], (unsigned long long)s[MESA_SWITCH_NVC0_CACHE_HITS],
                 ms(s[MESA_SWITCH_NVC0_TRANSLATE_NS]), (unsigned long long)s[MESA_SWITCH_CACHE_GETS],
                 (unsigned long long)s[MESA_SWITCH_CACHE_GET_HITS], (unsigned long long)s[MESA_SWITCH_CACHE_CORRUPT],
                 ms(s[MESA_SWITCH_CACHE_GET_NS]), (unsigned long long)s[MESA_SWITCH_CACHE_PUTS],
                 double(s[MESA_SWITCH_CACHE_PUT_BYTES]) / 1048576.0, (unsigned long long)s[MESA_SWITCH_CACHE_PUTS_DROPPED],
                 ms(s[MESA_SWITCH_CACHE_PUT_NS]));
        out += line;
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
#else
    return "";
#endif
}

}  // namespace mesa_cache
