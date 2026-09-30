/*
 * gpu_spirv.cpp - HLSL to SPIR-V with glslang (see gpu_spirv.h).
 */
#include "gpu_spirv.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

namespace {

std::once_flag g_init;

void set_err(char *err, size_t errn, const char *a, const char *b)
{
    if (!err || !errn)
        return;
    std::snprintf(err, errn, "%s%s%s", a ? a : "", (a && b && *a && *b) ? "\n" : "", b ? b : "");
}

}  // namespace

extern "C" int gpu_hlsl_to_spirv(const char *src, const char *entry, int stage,
                                 uint32_t **words, size_t *count, char *err, size_t errn)
{
    std::call_once(g_init, [] { glslang::InitializeProcess(); });
    *words = nullptr;
    *count = 0;

    const EShLanguage lang = stage == GPU_STAGE_VERTEX ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(lang);
    const char *srcs[] = { src };
    shader.setStrings(srcs, 1);
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    /* registers -> the bindings of the one descriptor set (gpu_spirv.h) */
    shader.setShiftBinding(glslang::EResUbo, GPU_SPIRV_BIND_CB);
    shader.setShiftBinding(glslang::EResTexture, GPU_SPIRV_BIND_TEX);
    shader.setShiftBinding(glslang::EResSampler, GPU_SPIRV_BIND_SAMPLER);
    shader.setHlslIoMapping(true);
    shader.setAutoMapLocations(true);
    shader.setEnvTargetHlslFunctionality1();

    const EShMessages msgs = (EShMessages)(EShMsgReadHlsl | EShMsgSpvRules | EShMsgVulkanRules | EShMsgHlslOffsets);
    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        set_err(err, errn, shader.getInfoLog(), shader.getInfoDebugLog());
        return 0;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs) || !program.mapIO()) {
        set_err(err, errn, program.getInfoLog(), program.getInfoDebugLog());
        return 0;
    }
    std::vector<unsigned int> spv;
    glslang::SpvOptions opt;
    opt.generateDebugInfo = false;
    opt.disableOptimizer = true;
    opt.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spv, &opt);
    if (spv.empty()) {
        set_err(err, errn, "no SPIR-V generated", nullptr);
        return 0;
    }
    *words = (uint32_t *)std::malloc(spv.size() * sizeof(uint32_t));
    if (!*words) {
        set_err(err, errn, "out of memory", nullptr);
        return 0;
    }
    std::memcpy(*words, spv.data(), spv.size() * sizeof(uint32_t));
    *count = spv.size();
    return 1;
}
