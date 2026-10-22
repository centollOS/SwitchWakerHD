#pragma once
#include "vertex_format_policy.h"
#include <regex>

namespace gfxvk::vk {
inline bool rewrite_vertex_fallback_input(std::string& source, std::string name, int location, uint32_t tailLocation) {
    auto declaration = "in uvec4 " + name + ";";
    auto position = source.find(declaration);
    if (position == std::string::npos) {
        // Compatible graphics-pack inputs may use different names.
        const std::string loc = std::to_string(location);
        const std::regex input("(?:layout\\s*\\(\\s*location\\s*=\\s*" + loc +
            "\\s*\\)|ATTR_LAYOUT\\(\\s*" + loc +
            "\\s*\\))\\s*(in\\s+uvec4\\s+([A-Za-z_][A-Za-z_0-9]*)\\s*;)");
        std::smatch match;
        if (!std::regex_search(source, match, input)) return false;
        position = match.position(1);
        declaration = match.str(1);
        name = match.str(2);
    }
    source.replace(position, declaration.size(), vertex_fallback_declaration(name, tailLocation));
    return true;
}
}
