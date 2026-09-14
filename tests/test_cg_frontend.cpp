#include "core/internal.hpp"
#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"
#include "gxp/gxp_reader.hpp"
#include "spirv/spirv_cross_adapter.hpp"
#include "spirv/spirv_pipeline.hpp"
#include "usse/usse.hpp"

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

bool compile_control_flow_gxp(const std::string &source) {
    VscCompileRequest request{};
    request.source_name="fp-if-big.cg";
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_FRAGMENT;
    vsc::FrontendOutput front;
    if (!vsc::cg_to_spirv(request,front) || front.spirv.empty()) return false;
    vsc::PreparedSpirv prepared;
    vsc::Diagnostic prepare_error;
    if (!vsc::prepare_spirv(VSC_STAGE_FRAGMENT,"main",front.spirv.data(),front.spirv.size(),prepared,prepare_error))
        return false;
    vsc::backend::TypedShader typed(vsc::backend::TypedStage::Fragment);
    std::string typed_error;
    if (!vsc::spirv_cross_to_typed_shader(prepared.words,typed.stage(),"main",typed,typed_error)) {
        std::fprintf(stderr,"test_cg_frontend: control-flow Typed adapter failed: %s\n",typed_error.c_str());
        return false;
    }
    vsc::backend::IrCompileResult typed_result;
    if (!vsc::backend::compile_typed_shader(typed,typed_result)) {
        std::fprintf(stderr,"test_cg_frontend: control-flow Typed lowering failed: %s\n",typed_result.error.c_str());
        return false;
    }
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.type()==vsc::gxp::ProgramType::Fragment &&
            view.primary_register_count()==12 && view.flags()==0x00081003;
        size_t branches=0;
        bool oracle_compare=false;
        const auto code=view.primary_program();
        for (size_t offset=0; ok && offset+8<=code.size; offset+=8) {
            uint64_t word=0;
            std::memcpy(&word,code.data+offset,sizeof(word));
            if (word==0x48088a81a0038002ULL) oracle_compare=true;
            if (vsc::usse::classify_control(word)==vsc::usse::ControlClass::Branch) {
                vsc::usse::BranchSemantic branch{};
                if (!vsc::usse::decode_branch_semantic(word,&branch) || branch.offset==0) ok=false;
                ++branches;
            }
        }
        ok = ok && oracle_compare && branches>=3;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: control-flow backend diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_fragment_gxp(const std::string &source, const char *name) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    const bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s backend diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_loop_gxp(const std::string &source, const char *name, uint8_t step) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.flags()==0x00081001 &&
            view.primary_register_count()==12 && view.secondary_register_count()==4 &&
            view.literal_count()==2 && view.container_count()==2 && view.parameter_count()==1;
        vsc::gxp::ParameterView parameter{};
        if (ok) {
            ok=view.parameter(0,parameter) && parameter.name=="n" && parameter.category==1 &&
                parameter.type==4 && parameter.component_count==1 && parameter.container_index==14 &&
                parameter.resource_index==0;
        }
        bool init=false,compare=false,update=false,feed=false,backedge=false;
        const uint64_t update_word=0xd08180042020c000ULL | step;
        const auto code=view.primary_program();
        for (size_t offset=0; ok && offset+8<=code.size; offset+=8) {
            uint64_t word=0;
            std::memcpy(&word,code.data+offset,sizeof(word));
            init |= word==0x50810008e0000100ULL;
            compare |= word==0x48a8068130078000ULL;
            update |= word==update_word;
            feed |= word==0xd09080040000c001ULL;
            if (vsc::usse::classify_control(word)==vsc::usse::ControlClass::Branch) {
                vsc::usse::BranchSemantic branch{};
                if (vsc::usse::decode_branch_semantic(word,&branch) && branch.offset<0) backedge=true;
            }
        }
        ok=ok && init && compare && update && feed && backedge;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s loop diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_oracle_fragment_profile(const std::string &source, const char *name,
                                     uint32_t expected_size, uint32_t expected_flags,
                                     uint16_t pa, uint16_t sa, uint32_t literal_count,
                                     const uint64_t *expected_words, size_t expected_word_count) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.logical_size()==expected_size && view.sdk_version()==0x0165 &&
            view.flags()==expected_flags && view.primary_register_count()==pa &&
            view.secondary_register_count()==sa && view.literal_count()==literal_count &&
            view.primary_instruction_count()==expected_word_count;
        const auto code=view.primary_program();
        if (ok) ok=code.size==expected_word_count*sizeof(uint64_t) &&
            std::memcmp(code.data,expected_words,code.size)==0;
    }
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

struct OracleVertexParam {
    const char *name;
    uint8_t semantic;
    uint32_t resource_index;
};

bool compile_oracle_vertex_profile(const std::string &source, const char *name,
                                   uint32_t expected_size, uint16_t pa, uint16_t sa,
                                   const OracleVertexParam *expected_params, size_t expected_param_count,
                                   const uint64_t *expected_words, size_t expected_word_count) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.logical_size()==expected_size && view.sdk_version()==0x0165 &&
            view.flags()==0x00090000 && view.primary_register_count()==pa &&
            view.secondary_register_count()==sa && view.parameter_count()==expected_param_count &&
            view.primary_instruction_count()==expected_word_count;
        for (size_t i=0;ok && i<expected_param_count;++i) {
            vsc::gxp::ParameterView parameter{};
            ok=view.parameter(static_cast<uint32_t>(i),parameter) &&
                parameter.name==expected_params[i].name && parameter.semantic==expected_params[i].semantic &&
                parameter.resource_index==expected_params[i].resource_index;
        }
        const auto code=view.primary_program();
        if (ok) ok=code.size==expected_word_count*sizeof(uint64_t) &&
            std::memcmp(code.data,expected_words,code.size)==0;
    }
    vsc_destroy_result(&request.allocator,&result);
    return ok;
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
    const std::string control_source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-if-big.cg");
    if (control_source.empty() || !compile_control_flow_gxp(control_source))
        failures += fail("large Cg if/else did not compile through Typed/Machine BR to a control-flow GXP");
    const std::string simple_if=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-if.cg");
    if (simple_if.empty() || !compile_fragment_gxp(simple_if,"fp-if.cg"))
        failures += fail("Cg early-return if with case-free OpSwitch did not compile end to end");
    const std::string not_equal=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-cmp-ne-big.cg");
    if (not_equal.empty() || !compile_fragment_gxp(not_equal,"fp-cmp-ne-big.cg"))
        failures += fail("Cg unordered not-equal compare did not compile end to end");
    const std::string ternary=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-ternary.cg");
    if (ternary.empty() || !compile_fragment_gxp(ternary,"fp-ternary.cg"))
        failures += fail("Cg float4 ternary did not lower through validated branch control flow");
    struct LoopProbe { const char *name; uint8_t step; };
    const LoopProbe loop_probes[]={{"fp-loop",1},{"fp-loop-step2",2},{"fp-loop-step3",3}};
    for (const auto &probe:loop_probes) {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe.name+".cg");
        if (source.empty() || !compile_loop_gxp(source,probe.name,probe.step)) {
            std::fprintf(stderr,"test_cg_frontend: loop probe failed: %s\n",probe.name);
            ++failures;
        }
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-swizzle-wzyx.cg");
        const uint64_t words[]={0xfa44070000000000ULL,0x40800d7ea0024083ULL};
        if (source.empty() || !compile_oracle_fragment_profile(source,"fp-swizzle-wzyx.cg",
                208,0x00081001,4,0,0,words,2))
            failures += fail("Cg wzyx profile did not reproduce oracle metadata/VPCK");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-constant-red.cg");
        const uint64_t words[]={0xfa44070000000000ULL,0x38800422c5000000ULL};
        if (source.empty() || !compile_oracle_fragment_profile(source,"fp-constant-red.cg",
                216,0x00080001,2,2,2,words,2))
            failures += fail("Cg constant-red profile did not reproduce oracle metadata/VMOV");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-passthrough.cg");
        const OracleVertexParam params[]={{"p",11,0}};
        const uint64_t words[]={0xfa44070000000000ULL,0x3880152183000000ULL,0xfb275000a0200000ULL};
        if (source.empty() || !compile_oracle_vertex_profile(source,"vp-passthrough.cg",
                230,4,0,params,1,words,3))
            failures += fail("Cg vertex passthrough did not reproduce oracle profile");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-uniform-mul.cg");
        const OracleVertexParam params[]={{"p",11,0},{"mvp",0,0}};
        const uint64_t words[]={0xfa44070000000000ULL,0xf800094000000000ULL,
                                0x40800dbcaf998002ULL,0x18903081c011a200ULL,
                                0xfb275000a0200000ULL};
        if (source.empty() || !compile_oracle_vertex_profile(source,"vp-uniform-mul.cg",
                274,4,16,params,2,words,5))
            failures += fail("Cg vertex uniform mat4 multiply did not reproduce oracle profile");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-varying.cg");
        const OracleVertexParam params[]={{"p",11,0},{"uv",14,4}};
        const uint64_t words[]={0xfa44070000000000ULL,0x3880252183000000ULL,0xfb275000a0200000ULL};
        if (source.empty() || !compile_oracle_vertex_profile(source,"vp-varying.cg",
                249,8,0,params,2,words,3))
            failures += fail("Cg vertex position/uv passthrough did not reproduce oracle profile");
    }
    {
        const char *alu_probes[]={
            "fp-add-float4","fp-sub-float4","fp-mul-float4","fp-div-float4","fp-min-float4","fp-max-float4",
            "fp-sat-float4","fp-abs-float4","fp-neg-float4","fp-mad-float4","fp-dot-float4",
            "fp-add-half4","fp-sub-half4","fp-mul-half4","fp-div-half4","fp-min-half4","fp-max-half4",
            "fp-sat-half4","fp-abs-half4","fp-neg-half4","fp-mad-half4","fp-dot-half4",
        };
        for (const char *probe:alu_probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
            if (source.empty() || !compile_fragment_gxp(source,probe)) {
                std::fprintf(stderr,"test_cg_frontend: vector4 ALU probe failed: %s\n",probe);
                ++failures;
            }
        }
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-div-float4.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x308008008f800101ULL,0x308008088f800102ULL,
            0x308008008f800184ULL,0x308008088f800188ULL,
            0x40800dbcafb98002ULL,0x10a4478600040f7cULL,
        };
        if (source.empty() || !compile_oracle_fragment_profile(source,"fp-div-float4.cg",
                272,0x00081005,8,0,0,words,8))
            failures += fail("Cg F32x4 division did not reproduce oracle profile");
    }
#endif
    return failures;
#endif
}
