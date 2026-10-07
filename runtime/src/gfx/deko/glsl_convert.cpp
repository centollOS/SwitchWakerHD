#include "glsl_convert.h"

#include <cstdio>
#include <cstring>

namespace gfxdk {
void ConvertedBindings::clear() {
    memset(ubo, -1, sizeof ubo);
    memset(sampler, -1, sizeof sampler);
    uboCount = samplerCount = 0;
    ufBlockSlot = ufBlockVkBinding = -1;
    negatedY = false;
    error.clear();
}

namespace {
// a line without its end (\n, and the \r before it in Cemu's \r\n) or the blanks around it
std::string trimmed(const std::string& line) {
    size_t end = line.size();
    while (end && line[end - 1] && strchr("\r\n \t", line[end - 1])) end--;
    size_t begin = 0;
    while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) begin++;
    return line.substr(begin, end - begin);
}
bool starts_with(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }

// a small cursor over "(a, b, c)" / "(set = N, binding = M)" with any blanks between the tokens
struct Scan {
    const std::string& s;
    size_t i;
    void blanks() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    }
    bool lit(const char* text) {
        blanks();
        size_t n = strlen(text);
        if (s.compare(i, n, text) != 0) return false;
        i += n;
        return true;
    }
    bool num(int* v) {
        blanks();
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        int n = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && n < 100000) n = n * 10 + (s[i++] - '0');
        *v = n;
        return true;
    }
};

struct Converter {
    bool vertex;
    ConvertedBindings* out;
    int set = -1;  // the stage's Vulkan descriptor set (Cemu: 0 vertex, 1 pixel)

    bool fail(const std::string& why) {
        if (out->error.empty()) out->error = why;
        return false;
    }
    bool check_set(int s) {
        if (set < 0) set = s;
        return s == set || fail("bindings in two descriptor sets (" + std::to_string(set) + " and " +
                                std::to_string(s) + ")");
    }
    // the deko3d slot for Vulkan binding vk of a UBO or a sampler, given in order of appearance
    bool slot(bool ubo, int vk, int* result) {
        if (vk >= kMaxVkBinding) return fail("Vulkan binding " + std::to_string(vk) + " out of range");
        int8_t* map = ubo ? out->ubo : out->sampler;
        if ((ubo ? out->sampler : out->ubo)[vk] >= 0)
            return fail("Vulkan binding " + std::to_string(vk) + " used by a UBO and a sampler");
        if (map[vk] < 0) {
            int& count = ubo ? out->uboCount : out->samplerCount;
            if (count >= (ubo ? kMaxUniformBuffers : kMaxSamplers))
                return fail(std::string("more than ") +
                            std::to_string(ubo ? kMaxUniformBuffers : kMaxSamplers) +
                            (ubo ? " uniform buffers" : " samplers"));
            map[vk] = (int8_t)count++;
        }
        *result = map[vk];
        return true;
    }
    // UNIFORM_BUFFER_LAYOUT(gl, set, vk) / TEXTURE_LAYOUT(gl, set, vk) -> layout(binding = slot[, std140])
    bool expand_macro(std::string& line, const char* name, bool ubo) {
        size_t at;
        while ((at = line.find(name)) != std::string::npos) {
            Scan sc{line, at + strlen(name)};
            int gl, s, vk, dk;
            if (!(sc.lit("(") && sc.num(&gl) && sc.lit(",") && sc.num(&s) && sc.lit(",") && sc.num(&vk) &&
                  sc.lit(")")))
                return fail(std::string("unexpected ") + name + " use: " + trimmed(line));
            if (!check_set(s) || !slot(ubo, vk, &dk)) return false;
            line.replace(at, sc.i - at,
                         "layout(binding = " + std::to_string(dk) + (ubo ? ", std140)" : ")"));
        }
        return true;
    }
    // the Vulkan branch's "layout(set = N, binding = M) uniform ufBlock" (the loose uniforms' UBO)
    bool expand_set_layout(std::string& line) {
        for (size_t at = line.find("layout("); at != std::string::npos; at = line.find("layout(", at + 1)) {
            Scan sc{line, at + 7};
            if (!(sc.lit("set") && sc.lit("="))) continue;
            int s, vk, dk;
            if (!(sc.num(&s) && sc.lit(",") && sc.lit("binding") && sc.lit("=") && sc.num(&vk) && sc.lit(")")))
                return fail("unexpected 'set =': " + trimmed(line));
            size_t end = sc.i;
            if (!(sc.lit("uniform") && sc.lit("ufBlock")))
                return fail("a 'set =' layout that is not the ufBlock: " + trimmed(line));
            if (out->ufBlockSlot >= 0) return fail("two ufBlock declarations");
            if (!check_set(s) || !slot(true, vk, &dk)) return false;
            out->ufBlockSlot = dk;
            out->ufBlockVkBinding = vk;
            line.replace(at, end - at, "layout(binding = " + std::to_string(dk) + ", std140)");
        }
        return true;
    }

    std::string run(const std::string& src) {
        enum { Outside, VulkanBranch, OtherBranch } branch = Outside;
        int foreignDepth = 0;  // #if blocks of the shader's own (none in Cemu's output today): passed through
        bool version = false, crlf = src.find("\r\n") != std::string::npos;
        std::string res;
        res.reserve(src.size() + 64);
        size_t pos = 0;
        while (pos < src.size()) {
            size_t nl = src.find('\n', pos);
            size_t next = nl == std::string::npos ? src.size() : nl + 1;
            std::string line = src.substr(pos, next - pos);
            pos = next;
            const std::string t = trimmed(line);
            if (!t.empty() && t[0] == '#') {
                if (t == "#ifdef VULKAN") {
                    if (branch != Outside || foreignDepth) { fail("nested #ifdef VULKAN"); return {}; }
                    branch = VulkanBranch;
                    continue;
                }
                if (branch != Outside) {
                    if (t == "#else" && branch == VulkanBranch) {
                        branch = OtherBranch;
                        continue;
                    }
                    if (t == "#endif") {
                        branch = Outside;
                        continue;
                    }
                    if (starts_with(t, "#if") || starts_with(t, "#el")) {
                        fail("conditional inside an #ifdef VULKAN block: " + t);
                        return {};
                    }
                } else if (starts_with(t, "#if")) {
                    if (t.find("VULKAN") != std::string::npos) { fail("unexpected condition: " + t); return {}; }
                    foreignDepth++;
                } else if (t == "#endif") {
                    if (!foreignDepth) { fail("#endif without #if"); return {}; }
                    foreignDepth--;
                }
            }
            if (branch == OtherBranch) continue;
            if (branch == VulkanBranch) {
                // the Vulkan built-in names (uam's GL frontend knows gl_VertexID/gl_InstanceID) and the
                // layout macros, expanded below with deko3d slots
                if (t == "#define gl_VertexID gl_VertexIndex" || t == "#define gl_InstanceID gl_InstanceIndex" ||
                    starts_with(t, "#define UNIFORM_BUFFER_LAYOUT(") || starts_with(t, "#define TEXTURE_LAYOUT("))
                    continue;
                // deko3d's clip-space y points up (dk.h): the position is given y-down as in Vulkan, negated
                if (starts_with(t, "#define SET_POSITION(")) {
                    if (t.find("gl_Position.y") != std::string::npos) { fail("unexpected SET_POSITION: " + t); return {}; }
                    line.insert(line.find_last_not_of("\r\n") + 1, kNegateY);
                    out->negatedY = true;
                }
            }
            if (starts_with(t, "#version")) {
                if (version) { fail("two #version lines"); return {}; }
                version = true;
                res += crlf ? "#version 460\r\n" : "#version 460\n";
                continue;
            }
            if (!expand_macro(line, "UNIFORM_BUFFER_LAYOUT", true) || !expand_macro(line, "TEXTURE_LAYOUT", false))
                return {};
            if (!expand_set_layout(line)) return {};
            if (vertex && t == "void main()") res += crlf ? "invariant gl_Position;\r\n" : "invariant gl_Position;\n";
            res += line;
        }
        if (branch != Outside || foreignDepth) { fail("unterminated #if block"); return {}; }
        if (!version) { fail("no #version line"); return {}; }
        if (vertex && res.find("invariant gl_Position;") == std::string::npos) { fail("no void main()"); return {}; }
        if (vertex && !out->negatedY) { fail("no SET_POSITION in the #ifdef VULKAN block"); return {}; }
        // the renames dropped above must not be needed by anything that is left
        if (res.find("gl_VertexIndex") != std::string::npos || res.find("gl_InstanceIndex") != std::string::npos) {
            fail("gl_VertexIndex/gl_InstanceIndex left after the conversion");
            return {};
        }
        return res;
    }
};
}  // namespace

std::string glsl_to_deko(const std::string& cemuGlsl, bool vertex, ConvertedBindings* out) {
    ConvertedBindings scratch;
    if (!out) out = &scratch;
    out->clear();
    Converter c{vertex, out};
    std::string res = c.run(cemuGlsl);
    if (res.empty() && out->error.empty()) out->error = "empty shader";
    return res;
}
}  // namespace gfxdk
