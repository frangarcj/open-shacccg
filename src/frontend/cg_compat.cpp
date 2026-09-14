#include "core/internal.hpp"

#if defined(OPENSHACCG_ENABLE_GLSLANG)
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

namespace vsc {

namespace {

#if defined(OPENSHACCG_ENABLE_GLSLANG)
bool is_identifier_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool is_cg_type_token(const std::string &token) {
    static constexpr const char *bases[] = {"bool", "fixed", "float", "half", "int", "uint"};
    for (const char *base : bases) {
        const std::string prefix(base);
        if (token == prefix) return true;
        if (token.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = token.substr(prefix.size());
        if (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '4') return true;
        if (suffix.size() == 3 && suffix[0] >= '1' && suffix[0] <= '4' && suffix[1] == 'x' &&
            suffix[2] >= '1' && suffix[2] <= '4') return true;
    }
    return false;
}

std::string normalize_cg_for_hlsl(const char *source, size_t size) {
    std::string input(source, size);
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf)
        input.erase(0, 3);

    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (!is_identifier_start(input[i])) {
            output.push_back(input[i++]);
            continue;
        }

        size_t token_end = i + 1;
        while (token_end < input.size() && is_identifier_char(input[token_end])) ++token_end;
        const std::string token = input.substr(i, token_end - i);
        if (is_cg_type_token(token)) {
            size_t qualifier = token_end;
            while (qualifier < input.size() && std::isspace(static_cast<unsigned char>(input[qualifier]))) ++qualifier;
            const size_t qualifier_end = qualifier + 3;
            if (qualifier_end <= input.size() && input.compare(qualifier, 3, "out") == 0 &&
                (qualifier_end == input.size() || !is_identifier_char(input[qualifier_end]))) {
                output += "out";
                output.append(input, token_end, qualifier - token_end);
                output += token;
                i = qualifier_end;
                continue;
            }
        }
        output += token;
        i = token_end;
    }
    return output;
}

EShLanguage glslang_stage(VscStage stage) {
    return stage == VSC_STAGE_FRAGMENT ? EShLangFragment : EShLangVertex;
}

bool ensure_glslang_initialized() {
    static const bool initialized = glslang::InitializeProcess();
    return initialized;
}
#endif

} // namespace

bool cg_to_spirv(const VscCompileRequest &request, FrontendOutput &out) {
    out = {};
    if (!request.source || request.source_size == 0) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1001, 0, 0, "empty shader source"});
        return false;
    }
    if (!request.entrypoint || !*request.entrypoint) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1002, 0, 0, "missing shader entry point"});
        return false;
    }

#if defined(OPENSHACCG_ENABLE_GLSLANG)
    if (request.source_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1003, 0, 0, "shader source is too large for glslang"});
        return false;
    }
    if (!ensure_glslang_initialized()) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10F0, 0, 0, "glslang initialization failed"});
        return false;
    }

    const std::string source = normalize_cg_for_hlsl(request.source, request.source_size);
    const char *source_ptr = source.c_str();
    const int source_length = static_cast<int>(source.size());
    const char *source_name = (request.source_name && *request.source_name) ? request.source_name : "shader.cg";
    const EShLanguage stage = glslang_stage(request.stage);
    glslang::TShader shader(stage);
    shader.setStringsWithLengthsAndNames(&source_ptr, &source_length, &source_name, 1);
    shader.setEntryPoint(request.entrypoint);
    shader.setSourceEntryPoint(request.entrypoint);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);

    const auto messages = static_cast<EShMessages>(
        EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl | EShMsgHlslDX9Compatible | EShMsgDisplayErrorColumn);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        std::string message = shader.getInfoLog();
        if (const char *debug = shader.getInfoDebugLog(); debug && *debug) {
            if (!message.empty()) message += '\n';
            message += debug;
        }
        if (message.empty()) message = "Cg/HLSL parse failed";
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1010, 0, 0, std::move(message)});
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        std::string message = program.getInfoLog();
        if (const char *debug = program.getInfoDebugLog(); debug && *debug) {
            if (!message.empty()) message += '\n';
            message += debug;
        }
        if (message.empty()) message = "Cg/HLSL link failed";
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1011, 0, 0, std::move(message)});
        return false;
    }

    const glslang::TIntermediate *intermediate = program.getIntermediate(stage);
    if (!intermediate) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1012, 0, 0, "glslang produced no shader intermediate"});
        return false;
    }

    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*intermediate, out.spirv, &options);
    if (out.spirv.empty()) {
        out.diagnostics.push_back({VSC_DIAG_ERROR, 0x1013, 0, 0, "glslang produced empty SPIR-V"});
        return false;
    }
    return true;
#else
    out.diagnostics.push_back({VSC_DIAG_ERROR, 0x10FF, 0, 0,
        "Cg frontend unavailable: build with OPENSHACCG_ENABLE_GLSLANG=ON"});
    return false;
#endif
}

} // namespace vsc
