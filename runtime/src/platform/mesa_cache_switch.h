// Mesa's persistent shader cache on the Switch (platform/mesa_cache_switch.cpp).
#pragma once
#include <string>

namespace mesa_cache {
void setup();          // before EGL starts (main.cpp): MESA_SHADER_CACHE_DIR, or off / reset (WWHD_MESA_CACHE)
std::string report();  // log lines (the cache's state once, then compile counters when they change); "" if none
}  // namespace mesa_cache
