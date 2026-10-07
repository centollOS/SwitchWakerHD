// Host test of gfxdk::glsl_to_deko (runtime/src/gfx/deko/glsl_convert.cpp) over a whole shader cache: converts
// every source that extract_shadercache.py dumped, checks the result and compiles it with a uam CLI.
//
//     python3 tools/switch/extract_shadercache.py shadercache_gl.bin src/
//     g++ -std=c++17 -O2 -Iruntime/src/gfx/deko tools/switch/deko_convert_test.cpp
//         runtime/src/gfx/deko/glsl_convert.cpp -o deko_convert_test
//     ./deko_convert_test src/ out/ path/to/uam [max shaders]
//
// Checks: no gl_VertexIndex/gl_InstanceIndex, no `set =`; every vertex shader keeps the Vulkan SET_POSITION
// (z remapped to 0..1 unless the shader's clip space is already 0..1) and the invariant gl_Position; then uam
// must compile it. Prints N ok / N total, failures grouped by their first error line and uam's times.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "glsl_convert.h"

namespace fs = std::filesystem;

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}
// the Vulkan branch's SET_POSITION of the original source (the first #define after #ifdef VULKAN)
static std::string vulkan_set_position(const std::string& src) {
    size_t vk = src.find("#ifdef VULKAN"), els = src.find("#else", vk);
    size_t at = src.find("#define SET_POSITION(", vk);
    if (vk == std::string::npos || at == std::string::npos || at > els) return {};
    return src.substr(at, src.find_first_of("\r\n", at) - at);
}
static std::string first_error(const std::string& log) {
    std::istringstream in(log);
    std::string line, first;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first.empty()) first = line;
        if (line.find("error") != std::string::npos) {
            first = line;
            break;
        }
    }
    // "0:123(4): error: ..." -> "error: ..." so that equal errors on different lines group together
    size_t e = first.find("error");
    if (e != std::string::npos) first = first.substr(e);
    return first.empty() ? "(no output)" : first;
}

// limits and malformed sources fail cleanly (empty result and a reason)
static bool self_test() {
    std::string many = "#version 430\n#ifdef VULKAN\n#define UNIFORM_BUFFER_LAYOUT(__glLocation, __vkSet, "
                       "__vkLocation) layout(set = __vkSet, binding = __vkLocation, std140)\n#else\n#endif\n";
    for (int i = 0; i < 17; i++)
        many += "UNIFORM_BUFFER_LAYOUT(" + std::to_string(i) + ", 1, " + std::to_string(i) + ") uniform b" +
                std::to_string(i) + " { vec4 v" + std::to_string(i) + "; };\n";
    many += "void main()\n{\n}\n";
    gfxdk::ConvertedBindings b;
    bool good = gfxdk::glsl_to_deko(many, false, &b).empty() && b.error.find("more than 16") != std::string::npos;
    good = good && gfxdk::glsl_to_deko("#version 430\n#ifdef VULKAN\nvoid main(){}\n", false, &b).empty() &&
           b.error == "unterminated #if block";
    good = good && gfxdk::glsl_to_deko("#version 430\nlayout(set = 0, binding = 1) uniform sampler2D t;\n", false, &b)
                       .empty() &&
           !b.error.empty();
    good = good && gfxdk::glsl_to_deko("#version 430\nint offset = 1;\nvoid main(){}\n", false, &b) ==
                       "#version 460\nint offset = 1;\nvoid main(){}\n";
    printf("self test: %s\n", good ? "ok" : "FAILED");
    return good;
}

int main(int argc, char** argv) {
    if (!self_test()) return 1;
    if (argc < 4) {
        fprintf(stderr, "usage: %s SRC_DIR OUT_DIR UAM [MAX]\n", argv[0]);
        return 2;
    }
    const fs::path srcDir = argv[1], outDir = argv[2];
    const std::string uam = argv[3];
    const size_t max = argc > 4 ? strtoul(argv[4], nullptr, 10) : SIZE_MAX;
    fs::create_directories(outDir);
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(srcDir))
        if (e.path().extension() == ".glsl") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.size() > max) files.resize(max);

    const std::string remap = "#define SET_POSITION(_v) gl_Position = _v; gl_Position.z = (gl_Position.z + "
                              "gl_Position.w) / 2.0";
    const std::string plain = "#define SET_POSITION(_v) gl_Position = _v";
    size_t total = 0, ok = 0, convFail = 0, checkFail = 0, compileFail = 0;
    size_t vs = 0, vsRemap = 0, vsDxClip = 0, ufBlocks = 0;
    int maxUbo = 0, maxSampler = 0;
    std::map<std::string, std::vector<std::string>> failures;  // reason -> shaders
    std::vector<double> ms;
    for (const fs::path& in : files) {
        const std::string name = in.stem().string();
        const bool vertex = name.size() > 3 && name.compare(name.size() - 3, 3, "_vs") == 0;
        const std::string src = read_file(in);
        total++;
        gfxdk::ConvertedBindings b;
        const std::string glsl = gfxdk::glsl_to_deko(src, vertex, &b);
        if (glsl.empty()) {
            convFail++;
            failures["convert: " + b.error].push_back(name);
            continue;
        }
        maxUbo = std::max(maxUbo, b.uboCount);
        maxSampler = std::max(maxSampler, b.samplerCount);
        if (b.ufBlockSlot >= 0) ufBlocks++;
        // checks on the text
        std::string bad;
        if (glsl.find("gl_VertexIndex") != std::string::npos || glsl.find("gl_InstanceIndex") != std::string::npos)
            bad = "check: gl_VertexIndex/gl_InstanceIndex left";
        else if (glsl.find("layout(set") != std::string::npos)
            bad = "check: set = left";
        else if (glsl.find("UNIFORM_BUFFER_LAYOUT") != std::string::npos || glsl.find("TEXTURE_LAYOUT") != std::string::npos)
            bad = "check: layout macro left";
        else if (glsl.compare(0, 13, "#version 460\r") != 0 && glsl.compare(0, 13, "#version 460\n") != 0)
            bad = "check: no #version 460 first";
        // the same source with \n line ends converts to the same text
        std::string lf = src, glslLf = glsl;
        lf.erase(std::remove(lf.begin(), lf.end(), '\r'), lf.end());
        glslLf.erase(std::remove(glslLf.begin(), glslLf.end(), '\r'), glslLf.end());
        if (bad.empty() && gfxdk::glsl_to_deko(lf, vertex, nullptr) != glslLf) bad = "check: LF and CRLF differ";
        if (bad.empty() && vertex) {
            vs++;
            const std::string want = vulkan_set_position(src);
            if (want.empty()) bad = "check: original has no Vulkan SET_POSITION";
            else if (glsl.find(want + "\r") == std::string::npos && glsl.find(want + "\n") == std::string::npos)
                bad = "check: converted VS lost the Vulkan SET_POSITION";
            else if (glsl.find(plain + "\r") != std::string::npos && want != plain)
                bad = "check: converted VS has the OpenGL SET_POSITION";
            else if (glsl.find("invariant gl_Position;") == std::string::npos)
                bad = "check: no invariant gl_Position";
            else if (want == remap) vsRemap++;
            else if (want == plain) vsDxClip++;
            else bad = "check: unknown SET_POSITION: " + want;
        }
        if (!bad.empty()) {
            checkFail++;
            failures[bad].push_back(name);
            continue;
        }
        const fs::path out = outDir / (name + ".glsl"), dksh = outDir / (name + ".dksh"),
                       log = outDir / (name + ".log");
        write_file(out, glsl);
        const std::string cmd = uam + " -s " + (vertex ? "vert" : "frag") + " -o " + dksh.string() + " " +
                                out.string() + " > " + log.string() + " 2>&1";
        auto t0 = std::chrono::steady_clock::now();
        int rc = std::system(cmd.c_str());
        auto t1 = std::chrono::steady_clock::now();
        if (rc != 0 || !fs::exists(dksh) || fs::file_size(dksh) == 0) {
            compileFail++;
            failures["uam: " + first_error(read_file(log))].push_back(name);
            continue;
        }
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        fs::remove(log);
        ok++;
        if (total % 500 == 0) fprintf(stderr, "%zu/%zu\n", total, files.size());
    }

    printf("%zu ok / %zu total (convert failed %zu, checks failed %zu, uam failed %zu)\n", ok, total, convFail,
           checkFail, compileFail);
    printf("vertex shaders: %zu, Vulkan SET_POSITION with z remap %zu, DX clip space (no remap) %zu\n", vs, vsRemap,
           vsDxClip);
    printf("ufBlock in %zu shaders; max per stage: %d UBOs, %d samplers\n", ufBlocks, maxUbo, maxSampler);
    if (!ms.empty()) {
        std::sort(ms.begin(), ms.end());
        double sum = 0;
        for (double v : ms) sum += v;
        auto pct = [&](double p) { return ms[std::min(ms.size() - 1, (size_t)(p * ms.size()))]; };
        printf("uam ms per shader (process included): mean %.1f p50 %.1f p90 %.1f p99 %.1f max %.1f\n",
               sum / ms.size(), pct(0.5), pct(0.9), pct(0.99), ms.back());
    }
    for (auto& [why, names] : failures) {
        printf("FAIL x%zu: %s\n", names.size(), why.c_str());
        for (size_t i = 0; i < names.size() && i < 5; i++) printf("    %s\n", names[i].c_str());
    }
    return ok == total ? 0 : 1;
}
