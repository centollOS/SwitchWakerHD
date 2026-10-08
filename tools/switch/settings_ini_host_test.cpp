// settings.ini's [dev] section and the env.txt conversion (runtime/src/platform/settings_ini.h), on a computer:
//   c++ -std=c++20 -O1 -Iruntime/src/platform tools/switch/settings_ini_host_test.cpp \
//       runtime/src/platform/settings_ini.cpp -o build/settings_ini_host_test && build/settings_ini_host_test
#include <cstdio>
#include <string>

#include "settings_ini.h"

static int g_failed = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: FAILED %s\n", __FILE__, __LINE__, #c); \
            g_failed++;                                                  \
        }                                                                \
    } while (0)

static std::string menu(const settings_ini::File& f, const char* k) {
    auto it = f.menu.find(k);
    return it == f.menu.end() ? "<none>" : it->second;
}

int main() {
    using namespace settings_ini;
    // a fresh install: nothing, and the first save writes only the menu's lines
    {
        File f = parse("");
        CHECK(f.menu.empty() && f.sections.empty() && dev_variables(f).empty());
        f.menu["switchCpuClock"] = "1020";
        CHECK(format(f) == "# Wind Waker HD settings\nswitchCpuClock=1020\n");
    }
    // round trip: the menu's lines sorted, [dev] kept verbatim (comments too), CRLF accepted
    {
        const std::string text =
            "# Wind Waker HD settings\r\nzeta=1\r\nalpha=2\r\n\r\n[dev]\r\n# trace a frame\r\nWWHD_DK_TRACE_FRAMES=2500\r\n"
            "  WWHD_DK_UF_CACHE = 1\r\n\r\n";
        File f = parse(text);
        CHECK(f.menu.size() == 2 && menu(f, "alpha") == "2" && menu(f, "zeta") == "1");
        auto dev = dev_variables(f);
        CHECK(dev.size() == 2);
        CHECK(dev.size() == 2 && dev[0].first == "WWHD_DK_TRACE_FRAMES" && dev[0].second == "2500");
        CHECK(dev.size() == 2 && dev[1].first == "WWHD_DK_UF_CACHE" && dev[1].second == "1");
        f.menu["middle"] = "3";
        const std::string out = format(f);
        CHECK(out == "# Wind Waker HD settings\nalpha=2\nmiddle=3\nzeta=1\n\n[dev]\n# trace a frame\n"
                     "WWHD_DK_TRACE_FRAMES=2500\n  WWHD_DK_UF_CACHE = 1\n");
        CHECK(format(parse(out)) == out);  // stable
        // [dev] lines are not menu settings
        CHECK(menu(parse(out), "WWHD_DK_TRACE_FRAMES") == "<none>");
    }
    // another section after [dev]: kept; its lines are not [dev] variables
    {
        File f = parse("a=1\n[dev]\nX=1\n[later]\nY=2\n");
        auto dev = dev_variables(f);
        CHECK(dev.size() == 1 && dev[0].first == "X");
        add_dev_variable(f, "Z", "3");
        CHECK(format(f) == "# Wind Waker HD settings\na=1\n\n[dev]\nX=1\nZ=3\n[later]\nY=2\n");
    }
    // menu-backed variables
    CHECK(menu_backed("WWHD_DEBUG_SERVER") && menu_backed("WWHD_MAIN_SAMPLER") && menu_backed("WWHD_MOD_FAST_SCENES"));
    CHECK(menu_backed("WWHD_CPU_CLOCK") && menu_backed("WWHD_CLIMB") && menu_backed("WWHD_RES_SCALE"));
    CHECK(!menu_backed("WWHD_DK_TRACE_FRAMES") && !menu_backed("WWHD_SCHED_STATS"));
    // the owner's env.txt: everything becomes a menu setting, [dev] stays empty
    {
        File f = parse("# Wind Waker HD settings\nswitchCpuClock=1224\n");
        Migration m = migrate_env_txt(
            "# comments\n# WWHD_FPS=1\n\nWWHD_MAIN_SAMPLER=1\r\nWWHD_MOD_FAST_SCENES=1\nWWHD_DEBUG_SERVER=1\n--game x\n", f);
        CHECK(menu(f, "switchMainSampler") == "1");
        CHECK(menu(f, "mod.fast-scenes.enabled") == "1");
        CHECK(menu(f, "switchDebugServer") == "1");
        CHECK(menu(f, "switchCpuClock") == "1224");
        CHECK(menu(f, "switchFpsCounter") == "<none>");  // (a comment)
        CHECK(f.sections.empty() && dev_variables(f).empty());
        CHECK(m.log.size() == 3);
        CHECK(m.mods.size() == 1 && m.mods[0].first == "fast-scenes" && m.mods[0].second);
        CHECK(format(f).find("[dev]") == std::string::npos);
    }
    // conversions, values the menu does not offer, and developer variables into [dev]
    {
        File f = parse("");
        Migration m = migrate_env_txt(
            "WWHD_CPU_CLOCK=1224\nWWHD_GPU_PROFILE=0x92220007\nWWHD_RES_SCALE=1.5\nWWHD_DYNAMIC_RES=0\nWWHD_FPS=7\n"
            "WWHD_DEBUG_SERVER=0\nWWHD_CLIMB=0\nWWHD_EXPOSURE=0.9\nWWHD_DK_TRACE_FRAMES=100,200\nWWHD_SCHED_STATS=2\n", f);
        CHECK(menu(f, "switchCpuClock") == "1224");
        CHECK(menu(f, "switchGpuProfile") == "<none>");
        CHECK(menu(f, "switchResScale.handheld") == "1.50" && menu(f, "switchResScale.docked") == "1.50");
        CHECK(menu(f, "switchDynamicRes.handheld") == "0" && menu(f, "switchDynamicRes.docked") == "0");
        CHECK(menu(f, "switchFpsCounter") == "2");
        CHECK(menu(f, "switchDebugServer") == "0");
        CHECK(menu(f, "mod.wall-climb.enabled") == "0");
        CHECK(menu(f, "switchExposure") == "0.9");
        auto dev = dev_variables(f);
        CHECK(dev.size() == 2 && dev[0].first == "WWHD_DK_TRACE_FRAMES" && dev[0].second == "100,200" &&
              dev[1].first == "WWHD_SCHED_STATS");
        CHECK(m.log.size() == 10);
        CHECK(format(f).find("\n\n[dev]\nWWHD_DK_TRACE_FRAMES=100,200\nWWHD_SCHED_STATS=2\n") != std::string::npos);
    }
    if (g_failed) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("settings_ini: all checks passed\n");
    return 0;
}
