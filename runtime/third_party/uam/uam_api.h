#pragma once
// The renderer's view of uam (devkitPro's deko3d shader compiler, vendored here with the patches in
// PATCHES.md): GLSL source in, DKSH bytes out (the same bytes the uam command line tool writes).
//
// Threads (PATCHES.md, patch 10): init(true) once, before any compile; then compile() may run on several threads
// at once (one call per thread at a time; each thread gets its own Mesa context on its first compile). init(false)
// (the frontend rebuilt and freed around every compile) is for one thread only. A compiling thread needs a large
// stack (Mesa's parser and nv50_ir recurse deeply: 8 MB on the Switch).
#include <cstdint>
#include <string>
#include <vector>

namespace uam {

enum class Stage { Vertex, TessCtrl, TessEval, Geometry, Fragment, Compute };

struct Result {
    bool ok = false;
    std::vector<uint8_t> dksh;  // a whole .dksh file (header, program header, code, constants); empty on failure
    std::string log;            // everything uam reported for this shader (errors, warnings); may be set when ok
};

// once, before the first compile(). resident_frontend keeps Mesa's built-in GLSL function library and
// type tables alive between compiles (built on the first compile, a few MB); false rebuilds and frees
// them around every compile, as the uam tool does.
void init(bool resident_frontend = true);
// A/B switch for patch 7 (PATCHES.md): true lets the Maxwell scheduler pair instructions (uam's own behaviour).
// Default false; set it before the first compile, and give the DKSH another cache id (shader_files.h).
void set_dual_issue(bool on);
// frees what init() and the compiles kept; compile() works again after another init()
void shutdown();

// compiles one stage of a separable program (uam has no linking; every binding must be explicit).
// A compile error, and any fatal error inside Mesa or nv50_ir, returns ok = false with the reason in
// log; it never exits the process.
Result compile(Stage stage, const char* glsl);

}  // namespace uam
