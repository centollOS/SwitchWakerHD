#pragma once
#include <cstdint>

namespace mods::targeting_letterbox {
struct Context {
    uint32_t size, mode;
    bool event, scripted, minigame, target, normal_write;
};
constexpr bool suppress(bool enabled, const Context& c) {
    return !enabled && c.size == 1 && c.mode == 2 && !c.event && !c.scripted && !c.minigame &&
           c.target && c.normal_write;
}
}
