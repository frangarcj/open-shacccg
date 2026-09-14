#include "core/internal.hpp"
#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"
#include "spirv/spirv_cross_adapter.hpp"
#include "spirv/spirv_pipeline.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
int fail(const char *message) {
    std::fprintf(stderr, "test_cg_frontend: %s\n", message);
    return 1;
}

#if defined(OPENSHACCG_ENABLE_GLSLANG)
bool compile(const char *source, VscStage stage) {
    VscCompileRequest request{};
    request.source_name = "probe.cg";
    request.source = source;
    request.source_size = std::strlen(source);
    request.entrypoint = "main";
    request.stage = stage;
    vsc::FrontendOutput out;
    if (!vsc::cg_to_spirv(request, out) || out.spirv.empty() || !out.diagnostics.empty()) return false;
    vsc::SpirvSummary summary;
    vsc::Diagnostic diagnostic;
    if (!vsc::parse_spirv(out.spirv.data(), out.spirv.size(), summary, diagnostic)) return false;
    const uint32_t model = stage == VSC_STAGE_FRAGMENT ? 4u : 0u;
    for (const auto &entry : summary.entry_points)
        if (entry.name == "main" && entry.execution_model == model) return true;
    return false;
}

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
bool equal_except_guids(const uint8_t *data, size_t size, const std::vector<uint8_t> &expected) {
    if (!data || size != expected.size()) return false;
    for (size_t i = 0; i < size; ++i) {
        if (i >= 0x0c && i < 0x14) continue;
        if (data[i] != expected[i]) return false;
    }
    return true;
}

std::vector<uint8_t> read_binary(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::string read_text(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool compile_matches_public_gxp(const char *name, VscStage stage) {
    const std::string root = OPENSHACCG_SOURCE_DIR;
    const std::string base = root + "/research/public_samples/libvita2d/" + name;
    const std::string source = read_text(base + ".cg");
    const auto expected = read_binary(base + ".gxp");
    if (source.empty() || expected.empty()) return false;
    VscCompileRequest request{};
    request.source_name = name;
    request.source = source.data();
    request.source_size = source.size();
    request.entrypoint = "main";
    request.stage = stage;

    vsc::FrontendOutput front;
    if (!vsc::cg_to_spirv(request, front) || front.spirv.empty()) return false;
    vsc::PreparedSpirv prepared;
    vsc::Diagnostic prepare_error;
    if (!vsc::prepare_spirv(stage, "main", front.spirv.data(), front.spirv.size(), prepared, prepare_error)) return false;
    vsc::backend::TypedShader typed(stage == VSC_STAGE_FRAGMENT ? vsc::backend::TypedStage::Fragment
                                                                : vsc::backend::TypedStage::Vertex);
    std::string typed_error;
    if (!vsc::spirv_cross_to_typed_shader(prepared.words, typed.stage(), "main", typed, typed_error)) {
        std::fprintf(stderr, "test_cg_frontend: %s direct Typed IR adapter failed: %s\n", name, typed_error.c_str());
        return false;
    }
    vsc::backend::IrCompileResult typed_result;
    if (!vsc::backend::compile_typed_shader(typed, typed_result) ||
        !equal_except_guids(typed_result.gxp.data(), typed_result.gxp.size(), expected)) {
        std::fprintf(stderr, "test_cg_frontend: %s direct Typed IR GXP mismatch: %s\n",
                     name, typed_result.error.c_str());
        return false;
    }

    VscCompileResult result{};
    const int rc = vsc_compile(&request, &result);
    const bool match = rc == 0 && result.gxp_data && result.diagnostic_count == 0 &&
        equal_except_guids(result.gxp_data, result.gxp_size, expected);
    if (!match) {
        std::fprintf(stderr, "test_cg_frontend: %s end-to-end mismatch rc=%d gxp=%zu expected=%zu",
                     name, rc, result.gxp_size, expected.size());
        if (rc == 0 && result.gxp_data && result.gxp_size == expected.size()) {
            for (size_t i = 0; i < expected.size(); ++i) {
                if (result.gxp_data[i] != expected[i]) {
                    std::fprintf(stderr, " first_diff=0x%zx got=%02x expected=%02x",
                                 i, result.gxp_data[i], expected[i]);
                    break;
                }
            }
            for (size_t i = 0x14; i < expected.size(); ++i) {
                if (result.gxp_data[i] != expected[i]) {
                    std::fprintf(stderr, " next_diff=0x%zx got=%02x expected=%02x",
                                 i, result.gxp_data[i], expected[i]);
                    break;
                }
            }
        }
        if (result.diagnostic_count && result.diagnostics)
            std::fprintf(stderr, " diagnostic=%s", result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
        std::fprintf(stderr, "\n");
    }
    vsc_destroy_result(&request.allocator, &result);
    return match;
}
#endif
#endif
} // namespace

int test_cg_frontend() {
#if !defined(OPENSHACCG_ENABLE_GLSLANG)
    return 0;
#else
    int failures = 0;
    if (!compile("float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1):COLOR0{return a+b;}", VSC_STAGE_FRAGMENT))
        failures += fail("float4 arithmetic Cg did not compile through glslang HLSL");
    if (!compile("half4 main(half4 a:TEXCOORD0,half4 b:TEXCOORD1):COLOR0{return a+b;}", VSC_STAGE_FRAGMENT))
        failures += fail("half4 Cg did not compile through glslang HLSL");
    if (!compile("float4 main(float4 a:TEXCOORD0,float4 b:TEXCOORD1):COLOR0{if(a.x>b.x)return a;return b;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg conditional did not compile through glslang HLSL");
    if (!compile("uniform sampler2D tex;float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(tex,uv);}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg sampler2D/tex2D did not compile through glslang HLSL");
    if (!compile("uniform float4x4 mvp;float4 main(float4 p:POSITION):POSITION{return mul(mvp,p);}", VSC_STAGE_VERTEX))
        failures += fail("Cg matrix mul did not compile through glslang HLSL");
    if (!compile("void main(float3 aPosition,float4 aColor,uniform float4x4 wvp,float4 out vPosition:POSITION,float4 out vColor:COLOR){vPosition=mul(float4(aPosition,1.f),wvp);vColor=aColor;}", VSC_STAGE_VERTEX))
        failures += fail("Cg post-type out qualifier normalization did not compile");
#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    struct PublicShader { const char *name; VscStage stage; };
    const PublicShader public_shaders[] = {
        {"clear_f", VSC_STAGE_FRAGMENT},
        {"color_f", VSC_STAGE_FRAGMENT},
        {"texture_f", VSC_STAGE_FRAGMENT},
        {"texture_tint_f", VSC_STAGE_FRAGMENT},
        {"clear_v", VSC_STAGE_VERTEX},
        {"color_v", VSC_STAGE_VERTEX},
        {"texture_v", VSC_STAGE_VERTEX},
    };
    for (const auto &shader : public_shaders)
        if (!compile_matches_public_gxp(shader.name, shader.stage)) ++failures;
#endif
    return failures;
#endif
}
