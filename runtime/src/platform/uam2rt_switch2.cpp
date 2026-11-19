#include "uam_api.h"

#include <uam2rt.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <cstdlib>
#include <mutex>

namespace uam {
static_assert(int(Stage::Vertex) == Uam2rtStage_Vertex && int(Stage::TessCtrl) == Uam2rtStage_TessCtrl &&
              int(Stage::TessEval) == Uam2rtStage_TessEval && int(Stage::Geometry) == Uam2rtStage_Geometry &&
              int(Stage::Fragment) == Uam2rtStage_Fragment && int(Stage::Compute) == Uam2rtStage_Compute);
namespace {
std::mutex glslangMutex;
bool glslangReady = false;
bool uam2Ready = false;

bool to_spirv(Stage stage, const char* glsl, std::vector<uint32_t>* words, std::string* log) {
    const EShLanguage lang = stage == Stage::Vertex     ? EShLangVertex
                             : stage == Stage::Fragment ? EShLangFragment
                             : stage == Stage::Geometry ? EShLangGeometry
                             : stage == Stage::TessCtrl ? EShLangTessControl
                             : stage == Stage::TessEval ? EShLangTessEvaluation
                                                        : EShLangCompute;
    std::lock_guard lock(glslangMutex);
    if (!glslangReady) {
        *log = "glslang: not initialized";
        return false;
    }
    glslang::TShader shader(lang);
    shader.setStrings(&glsl, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 460, false, messages)) {
        *log = std::string("glslang: ") + shader.getInfoLog() + shader.getInfoDebugLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        *log = std::string("glslang: ") + program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), *words, &options);
    if (words->empty()) {
        *log = "glslang: no SPIR-V";
        return false;
    }
    return true;
}
}  // namespace

void init(bool) {
    {
        std::lock_guard lock(glslangMutex);
        if (!glslangReady) glslangReady = glslang::InitializeProcess();
    }
    if (!uam2Ready) uam2Ready = uam2rt_init();
}

void set_dual_issue(bool) {}

void shutdown() {
    if (uam2Ready) uam2rt_exit();
    uam2Ready = false;
    std::lock_guard lock(glslangMutex);
    if (glslangReady) glslang::FinalizeProcess();
    glslangReady = false;
}

Result compile(Stage stage, const char* glsl) {
    Result r;
    if (!uam2Ready) {
        r.log = "uam2rt: not initialized";
        return r;
    }
    std::vector<uint32_t> words;
    if (!to_spirv(stage, glsl, &words, &r.log)) return r;
    size_t size = 0;
    char* log = nullptr;
    void* dksh = uam2rt_compile_spirv(words.data(), words.size(), static_cast<Uam2rtStage>(stage), "main", nullptr, 0,
                                      &size, &log);
    if (log) {
        r.log = log;
        free(log);
    }
    if (dksh) {
        const uint8_t* p = static_cast<const uint8_t*>(dksh);
        r.dksh.assign(p, p + size);
        free(dksh);
        r.ok = !r.dksh.empty();
    }
    return r;
}

}  // namespace uam
