// Pixel-shader inputs that the vertex shader does not write (shaders.cpp link()).
//
// Cemu's decompiler declares one varying per PS input slot, `layout(location = L) [flat ]
// [noperspective ]in vec4 passParameterSemN;`, and gives the vertex shader an output only for the
// semantics it exports (SPI_VS_OUT_ID). A game may pair a pixel shader with a vertex shader that
// does not export everything the pixel shader reads (the Switch log: semantic 254 aboard the
// pirate ship); the hardware then gives the input its default value. Vulkan allows an unmatched
// input and Cemu's GL renderer links its stages separately, but a GL program with explicit
// locations fails to link ("input ... with explicit location has no matching output"), and every
// draw with that pair was skipped. add_missing_outputs gives the vertex shader those outputs,
// written as zero (DEFAULT_VAL 0), so the pair links.
#pragma once
#include <string>
#include <vector>

namespace gfxgl {
struct VaryingDecl {
    std::string line;  // the whole declaration, through the ';'
    std::string name;  // passParameterSemN
};

// declarations "layout(location = L) ... <dir> vec4 passParameterSemN;" in glsl (dir: "in" or "out")
inline std::vector<VaryingDecl> varying_decls(const std::string& glsl, const char* dir) {
    std::vector<VaryingDecl> out;
    const std::string key = std::string(dir) + " vec4 passParameterSem";
    for (size_t at = glsl.find("layout(location = "); at != std::string::npos; at = glsl.find("layout(location = ", at + 1)) {
        const size_t end = glsl.find(';', at);
        if (end == std::string::npos) break;
        const std::string line = glsl.substr(at, end + 1 - at);
        if (line.find('\n') != std::string::npos) continue;  // not a one-line declaration
        const size_t k = line.find(key);
        if (k == std::string::npos || (k > 0 && line[k - 1] != ' ')) continue;
        const size_t name = line.find("passParameterSem", k);
        out.push_back({line, line.substr(name, line.size() - 1 - name)});
    }
    return out;
}

// vs with an output for every passParameterSemN input of ps that vs does not declare, set to zero at
// the start of main; empty when nothing is missing (or vs has no main). added: the names.
inline std::string add_missing_outputs(const std::string& vs, const std::string& ps, std::vector<std::string>* added) {
    const auto outputs = varying_decls(vs, "out");
    std::string decls, writes;
    for (const auto& in : varying_decls(ps, "in")) {
        bool found = false;
        for (const auto& o : outputs)
            if (o.name == in.name) found = true;
        if (found) continue;
        std::string line = in.line;
        const size_t k = line.find(" in vec4 ");
        line.replace(k, 9, " out vec4 ");
        decls += line + "\r\n";
        writes += in.name + " = vec4(0.0);\r\n";
        if (added) added->push_back(in.name);
    }
    if (decls.empty()) return {};
    std::string out = vs;
    const size_t main = out.find("void main()");
    if (main == std::string::npos) return {};
    const size_t brace = out.find('{', main);
    if (brace == std::string::npos) return {};
    out.insert(brace + 1, "\r\n" + writes);
    out.insert(main, decls);
    return out;
}
}  // namespace gfxgl
