#include "core/internal.hpp"
#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"
#include "gxp/gxp_reader.hpp"
#include "spirv/spirv_cross_adapter.hpp"
#include "spirv/spirv_pipeline.hpp"
#include "usse/usse.hpp"

#include <cstdio>
#include <cctype>
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

bool compile_shader_gxp(const std::string &source, const char *name, VscStage stage) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=stage;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    const bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s backend diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_fragment_gxp(const std::string &source, const char *name) {
    return compile_shader_gxp(source,name,VSC_STAGE_FRAGMENT);
}

bool compile_vitagl_blit_vertex_profile() {
    static constexpr const char *source=
        "void main(float2 position,float2 texcoord,float4 out vPos:POSITION,float2 out vTexcoord:TEXCOORD0){"
        "vPos=float4(position,0.0f,1.0f);vTexcoord=texcoord;}";
    VscCompileRequest request{};
    request.source_name="vitagl-precompiled-blit-v.cg";
    request.source=source;
    request.source_size=std::strlen(source);
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size==284 && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        const uint64_t words[]={
            0xfa44070000000000ULL,0x3880052183080080ULL,0x08a5118590040001ULL,
            0x0883118190560001ULL,0xfb275000a0200000ULL,
        };
        const uint8_t varyings[]={
            0x33,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0x10,0,0x06,0x01,0,0,0,0,0,0,0,0,0,0,0,
        };
        vsc::gxp::ParameterView position{},texcoord{};
        const auto primary=view.primary_program();
        const auto interface=view.varyings();
        ok=view.valid() && view.major_version()==1 && view.minor_version()==5 &&
            view.sdk_version()==0x0350 && view.logical_size()==282 && view.flags()==0x00190004 &&
            view.primary_register_count()==8 && view.secondary_register_count()==0 &&
            view.compiler_version_raw()==0x00033dc0 && view.primary_instruction_count()==5 &&
            view.secondary_instruction_count()==0 && view.parameter_count()==2 &&
            primary.size==sizeof(words) && std::memcmp(primary.data,words,sizeof(words))==0 &&
            interface.size==sizeof(varyings) && std::memcmp(interface.data,varyings,sizeof(varyings))==0 &&
            view.parameter(0,position) && position.name=="position" && position.category==0 &&
            position.component_count==4 && position.semantic==0 && position.resource_index==0 &&
            view.parameter(1,texcoord) && texcoord.name=="texcoord" && texcoord.category==0 &&
            texcoord.component_count==4 && texcoord.semantic==0 && texcoord.resource_index==4;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: vitaGL blit vertex diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_texture_tint_alpha_discard_profile() {
    static constexpr const char *source=
        "uniform sampler2D tex:TEXUNIT0;uniform float cut;uniform float4 tint;"
        "float4 main(float2 uv:TEXCOORD0):COLOR0{float4 c=tex2D(tex,uv)*tint;"
        "if(c.a<cut)discard;return c;}";
    VscCompileRequest request{};
    request.source_name="texture-tint-alpha-discard.cg";
    request.source=source;
    request.source_size=std::strlen(source);
    request.entrypoint="main";
    request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size==384 && result.diagnostic_count==0;
    if (ok) {
        const uint64_t words[]={
            0xfa44010000000000ULL,0x08a44186e0040040ULL,0x08c0418ae0440081ULL,
            0x4888c915b0038080ULL,0xf9300406f0000306ULL,0xf804014000000000ULL,
            0xfa44070000000000ULL,0x40800d7ea0198002ULL,
        };
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        vsc::gxp::ParameterView cut{},tint{},tex{};
        const auto primary=view.primary_program();
        const auto query=view.sampler_query_info();
        uint16_t query0=0;
        if (query.size>=sizeof(query0)) std::memcpy(&query0,query.data,sizeof(query0));
        ok=view.valid() && view.logical_size()==381 && view.minor_version()==5 && view.sdk_version()==0x0300 &&
            view.flags()==0x00180809 && view.primary_register_count()==4 &&
            view.secondary_register_count()==7 && view.primary_instruction_count()==8 &&
            view.secondary_instruction_count()==0 && view.literal_count()==1 &&
            view.container_count()==2 && view.parameter_count()==3 &&
            view.compiler_version_raw()==0x00033a90 && query.size==32 && query0==0x0301 &&
            primary.size==sizeof(words) && std::memcmp(primary.data,words,sizeof(words))==0 &&
            view.parameter(0,cut) && cut.name=="cut" && cut.category==1 && cut.component_count==1 &&
            cut.container_index==14 && cut.resource_index==0 &&
            view.parameter(1,tint) && tint.name=="tint" && tint.category==1 && tint.component_count==4 &&
            tint.container_index==14 && tint.resource_index==2 &&
            view.parameter(2,tex) && tex.name=="tex" && tex.category==2 && tex.resource_index==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: texture alpha-discard diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_vertex_gxp(const std::string &source, const char *name) {
    VscCompileRequest request{};
    request.source_name=name;
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    const bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s vertex backend diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_matrix_point_size_profile(bool with_color) {
    const char *source=with_color ?
        "uniform float4x4 Jwvp; uniform float Mpoint_size; "
        "void main(float4 Nposition,float4 Pcolor,float4 out vPosition:POSITION,float4 out vColor:COLOR,float out psize:PSIZE){"
        "vPosition=mul(Jwvp,Nposition);vColor=Pcolor;psize=Mpoint_size;}" :
        "uniform float4x4 Jwvp; uniform float Mpoint_size; "
        "void main(float4 Nposition,float4 out vPosition:POSITION,float out psize:PSIZE){"
        "vPosition=mul(Jwvp,Nposition);psize=Mpoint_size;}";
    VscCompileRequest request{};
    request.source_name=with_color ? "matrix-color-point-size.cg" : "matrix-point-size.cg";
    request.source=source;
    request.source_size=std::strlen(source);
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    const uint64_t base_words[]={
        0xfa44070000000000ULL,0x50c10009e0800800ULL,0x40800dbcaf998002ULL,
        0x18903081c011a200ULL,0xfb275000a0200000ULL,
    };
    const uint64_t color_words[]={
        0xfa44070000000000ULL,0x38801d2183080080ULL,0x40800dbcaf998002ULL,
        0x18903081c011a200ULL,0x50810009e1000800ULL,0xfb275000a0200000ULL,
    };
    const uint64_t secondary_words[]={
        0x08a41086a2046209ULL,0x08a40086a2045209ULL,0xf804014000000000ULL,
    };
    const uint8_t base_interface[]={
        0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x11,0,0x05,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    const uint8_t color_interface[]={
        0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x09,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    bool ok=rc==0 && result.gxp_data && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        const auto primary=view.primary_program();
        const auto secondary=view.secondary_program();
        const auto interface=view.varyings();
        const uint64_t *expected_primary=with_color ? color_words : base_words;
        const uint8_t *expected_interface=with_color ? color_interface : base_interface;
        const size_t expected_primary_size=with_color ? sizeof(color_words) : sizeof(base_words);
        ok=view.valid() && view.logical_size()==(with_color ? 394u : 363u) &&
            view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00190000 &&
            view.primary_register_count()==(with_color ? 8u : 4u) &&
            view.secondary_register_count()==20 && view.primary_instruction_count()==(with_color ? 6u : 5u) &&
            view.secondary_instruction_count()==3 && view.literal_count()==2 && view.container_count()==2 &&
            view.compiler_version_raw()==0x00033a90 &&
            view.parameter_count()==(with_color ? 4u : 3u) &&
            primary.size==expected_primary_size && std::memcmp(primary.data,expected_primary,expected_primary_size)==0 &&
            secondary.size==sizeof(secondary_words) && std::memcmp(secondary.data,secondary_words,sizeof(secondary_words))==0 &&
            interface.size==sizeof(base_interface) &&
            std::memcmp(interface.data,expected_interface,sizeof(base_interface))==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s diagnostic=%s\n",request.source_name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_matrix_texcoord_point_size_profile() {
    static constexpr const char *source=
        "void main(float4 p,float2 uv,uniform float4x4 mvp,uniform float4x4 texmat[1],uniform float point_size,"
        "float2 out tc:TEXCOORD0,float4 out pos:POSITION,float out ps:PSIZE){"
        "pos=mul(mvp,p);tc=mul(texmat[0],float4(uv,0.f,1.f)).xy;ps=point_size;}";
    VscCompileRequest request{};
    request.source_name="matrix-texcoord-point-size-sdk30.cg";
    request.source=source; request.source_size=std::strlen(source);
    request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    const uint64_t primary_words[]={
        0xfa44070000000000ULL,0x50c10009e0c00c00ULL,0x40800dbcaf998002ULL,
        0x18903881c011a200ULL,0x40800dbcff998812ULL,0x189188818092c202ULL,
        0x40800dbcff998a16ULL,0x189181018092c202ULL,0xfb275000a0200000ULL,
    };
    const uint64_t secondary_words[]={
        0x08a41086a3046411ULL,0x08a40086a3045311ULL,0xf804014000000000ULL,
    };
    bool ok=rc==0 && result.gxp_data && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        const auto primary=view.primary_program(),secondary=view.secondary_program();
        ok=view.valid() && result.gxp_size==428 && view.logical_size()==427 &&
            view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00190000 &&
            view.primary_register_count()==8 && view.secondary_register_count()==36 &&
            view.primary_instruction_count()==9 && view.secondary_instruction_count()==3 &&
            view.literal_count()==2 && view.container_count()==2 && view.parameter_count()==5 &&
            view.compiler_version_raw()==0x00033a90 &&
            primary.size==sizeof(primary_words) && secondary.size==sizeof(secondary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            std::memcmp(secondary.data,secondary_words,sizeof(secondary_words))==0;
    }
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_matrix_normal_multivarying_profile() {
    const std::string source=
        "void main(float4 Nposition,float2 Otexcoord0,float4 Pcolor,float4 Qdiff,float4 Rspec,"
        "float4 Semission,float3 Tnormals,float2 out vTexcoord:TEXCOORD0,"
        "float3 out vNormal:TEXCOORD2,float3 out vEcPosition:TEXCOORD3,"
        "float4 out vDiffuse:TEXCOORD4,float4 out vSpecular:TEXCOORD5,"
        "float4 out vEmission:TEXCOORD6,float4 out vPosition:POSITION,float4 out vColor:COLOR,"
        "float out psize:PSIZE,uniform float4x4 Imodelview,uniform float4x4 Jwvp,"
        "uniform float4x4 Ktexmat[1],uniform float Mpoint_size,uniform float3x3 Lnormal_mat){"
        "Jwvp=mul(Jwvp,Imodelview);float4 modelpos=mul(Imodelview,Nposition);"
        "vPosition=mul(Jwvp,Nposition);float3 normal=normalize(mul(Lnormal_mat,Tnormals));"
        "float3 ecPosition=modelpos.xyz/modelpos.w;"
        "vTexcoord=mul(Ktexmat[0],float4(Otexcoord0,0.f,1.f)).xy;vColor=Pcolor;"
        "vNormal=normal;vEcPosition=ecPosition;vDiffuse=Qdiff;vSpecular=Rspec;"
        "vEmission=Semission;psize=Mpoint_size;}";
    auto snapshot=[](const std::string &text,std::vector<uint8_t> &gxp) -> bool {
        VscCompileRequest request{};
        request.source_name="matrix-normal-multivarying.cg";
        request.source=text.data(); request.source_size=text.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size && !result.diagnostic_count;
        if (ok) {
            const uint8_t interface_block[]={
                0x3f,0xff,0xff,0x07,0,0,0,0,0,0,0,0,0,0,0,0,
                0,0x19,0,0x1d,0xc1,0xf6,0x1f,0,0,0,0,0,0,0,0,0,
            };
            const uint32_t resources[]={0,4,8,12,16,20,24,0,16,32,50,48};
            const uint8_t components[]={4,4,4,4,4,4,4,4,4,4,3,1};
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const auto interface=view.varyings();
            ok=view.valid() && view.sdk_version()==0x0165 && view.flags()==0x00090004 &&
                view.primary_register_count()==28 && view.primary_instruction_count()>0 &&
                view.parameter_count()==12 && interface.size==sizeof(interface_block) &&
                std::memcmp(interface.data,interface_block,sizeof(interface_block))==0;
            for (size_t i=0;ok && i<12;++i) {
                vsc::gxp::ParameterView parameter{};
                ok=view.parameter(i,parameter) && parameter.resource_index==resources[i] &&
                    parameter.component_count==components[i];
            }
            if (ok) gxp.assign(result.gxp_data,result.gxp_data+result.gxp_size);
        }
        vsc_destroy_result(&request.allocator,&result);
        return ok;
    };
    std::vector<uint8_t> original,changed;
    if (!snapshot(source,original)) return false;
    auto mutated=source;
    const std::string from="float3 ecPosition=modelpos.xyz/modelpos.w;";
    const auto at=mutated.find(from);
    if (at==std::string::npos) return false;
    mutated.replace(at,from.size(),"float3 ecPosition=(modelpos.xyz/modelpos.w)*0.75f;");
    return snapshot(mutated,changed) && !equal_except_guids(changed.data(),changed.size(),original);
}

bool compile_indexed_clear_vertex_profile() {
    static constexpr const char *source=
        "float4 main(unsigned int i:INDEX,uniform float4 bounds,uniform float depth):POSITION{"
        "float x=(i==1||i==2)?bounds.y:bounds.x;"
        "float y=(i==2||i==3)?bounds.w:bounds.z;return float4(x,y,depth,1.f);}";
    VscCompileRequest request{};
    request.source_name="indexed-clear.cg";
    request.source=source;
    request.source_size=std::strlen(source);
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.diagnostic_count==0;
    if (ok) {
        const uint64_t primary_words[]={
            0xfa44070000000000ULL,0x48880185b007c006ULL,0x48880181b007c008ULL,
            0x4d880181b007c006ULL,0x50810009e0400600ULL,0x3d800501c1040040ULL,
            0x4e880185b007c007ULL,0x50810009e0000000ULL,0x3a800509c1000000ULL,
            0x3880050142000040ULL,0x38800521c3040140ULL,0xfb275000a0200000ULL,
        };
        const uint64_t secondary_words[]={
            0x3880050a81180040ULL,0x0881118291540081ULL,0xf804014000000000ULL,
        };
        const uint8_t interface_block[]={
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            0,0x10,0,0x04,0,0,0,0,0,0,0,0,0,0,0,0,
        };
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        const auto primary=view.primary_program();
        const auto secondary=view.secondary_program();
        const auto interface=view.varyings();
        vsc::gxp::ParameterView bounds{},depth{};
        ok=view.valid() && view.major_version()==1 && view.minor_version()==5 &&
            view.sdk_version()==0x0350 && view.flags()==0x001b0000 &&
            view.primary_register_count()==1 && view.secondary_register_count()==13 &&
            view.compiler_version_raw()==0x00033e40 && view.literal_count()==3 &&
            view.container_count()==2 && view.parameter_count()==2 &&
            primary.size==sizeof(primary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            secondary.size==sizeof(secondary_words) &&
            std::memcmp(secondary.data,secondary_words,sizeof(secondary_words))==0 &&
            interface.size==sizeof(interface_block) &&
            std::memcmp(interface.data,interface_block,sizeof(interface_block))==0 &&
            view.parameter(0,bounds) && bounds.name=="bounds" && bounds.category==1 &&
            bounds.component_count==4 && bounds.container_index==14 && bounds.resource_index==0 &&
            view.parameter(1,depth) && depth.name=="depth" && depth.category==1 &&
            depth.component_count==1 && depth.container_index==14 && depth.resource_index==4;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: indexed-clear diagnostic=%s\n",
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

bool compile_geometrizer_poly_vertex(const std::string &source) {
    VscCompileRequest request{};
    request.source_name="vp-geometrizer-poly.cg";
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.sdk_version()==0x0165 && view.flags()==0x00090000 &&
            view.primary_register_count()==8 && view.secondary_register_count()>=7 &&
            view.parameter_count()==4 && view.literal_count()>=3 &&
            view.primary_instruction_count()==19 && view.secondary_instruction_count()==2;
        const char *names[]={"a_pos","a_color","u_screen_size","u_z_max"};
        const uint32_t resources[]={0,4,0,2};
        for (uint32_t i=0;ok && i<4;++i) {
            vsc::gxp::ParameterView parameter{};
            ok=view.parameter(i,parameter) && parameter.name==names[i] && parameter.resource_index==resources[i];
        }
        bool vcomp=false,v32=false,vmov=false;
        const auto code=view.primary_program();
        for (size_t off=0;ok && off+sizeof(uint64_t)<=code.size;off+=sizeof(uint64_t)) {
            uint64_t word=0;
            std::memcpy(&word,code.data+off,sizeof(word));
            const auto family=vsc::usse::classify_major(word);
            vcomp |= family==vsc::usse::MajorClass::Vcomp;
            v32 |= family==vsc::usse::MajorClass::V32Nmad;
            vmov |= family==vsc::usse::MajorClass::Vmov;
        }
        const uint64_t secondary_words[]={0x3080000a80000002ULL,0x3080000280000001ULL};
        const auto secondary=view.secondary_program();
        ok=ok && vcomp && v32 && vmov && secondary.size==sizeof(secondary_words) &&
            std::memcmp(secondary.data,secondary_words,sizeof(secondary_words))==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: Geometrizer POLY_VS diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_geometrizer_poly3d_vertex(const std::string &source) {
    VscCompileRequest request{};
    request.source_name="vp-geometrizer-poly3d.cg";
    request.source=source.data();
    request.source_size=source.size();
    request.entrypoint="main";
    request.stage=VSC_STAGE_VERTEX;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.sdk_version()==0x0165 && view.flags()==0x00090004 &&
            view.primary_register_count()==20 && view.secondary_register_count()>=13 &&
            view.parameter_count()==10 && view.literal_count()>=3 &&
            view.primary_instruction_count()==37 && view.secondary_instruction_count()==2;
        const char *names[]={"a_m0","a_m1","a_m2","a_pos","a_color",
                             "u_xc","u_zoom","u_view","u_screen","u_z_max"};
        const uint32_t resources[]={0,4,8,12,16,0,2,4,6,8};
        const uint8_t semantics[]={14,14,14,14,6,0,0,0,0,0};
        const uint8_t semantic_indices[]={0,1,2,3,0,0,0,0,0,0};
        for (uint32_t i=0;ok && i<10;++i) {
            vsc::gxp::ParameterView parameter{};
            ok=view.parameter(i,parameter) && parameter.name==names[i] &&
                parameter.resource_index==resources[i] && parameter.semantic==semantics[i] &&
                parameter.semantic_index==semantic_indices[i];
        }
        bool vcomp=false,v32=false,vmov=false,emit=false;
        const auto code=view.primary_program();
        for (size_t off=0;ok && off+sizeof(uint64_t)<=code.size;off+=sizeof(uint64_t)) {
            uint64_t word=0;
            std::memcpy(&word,code.data+off,sizeof(word));
            const auto family=vsc::usse::classify_major(word);
            vcomp |= family==vsc::usse::MajorClass::Vcomp;
            v32 |= family==vsc::usse::MajorClass::V32Nmad;
            vmov |= family==vsc::usse::MajorClass::Vmov;
            emit |= vsc::usse::classify_control(word)==vsc::usse::ControlClass::Emit;
        }
        ok=ok && vcomp && v32 && vmov && emit;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: Geometrizer POLY3D_VS diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_geometrizer_cmp_fragment(const std::string &source) {
    VscCompileRequest request{};
    request.source_name="fp-geometrizer-cmp.cg";
    request.source=source.data(); request.source_size=source.size();
    request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        const uint64_t primary_words[]={
            0xfa44070000000000ULL,0x48898a81d003800cULL,0x3880050881000040ULL,
            0x40800d5ea0018002ULL,0x5081000ae0400100ULL,0x3d80050201040000ULL,
            0x40810d62a0000100ULL,
        };
        const uint64_t secondary_word=0x3886050a41040040ULL;
        ok=view.valid() && view.logical_size()==317 && view.sdk_version()==0x0165 &&
            view.flags()==0x00080801 && view.primary_register_count()==4 &&
            view.secondary_register_count()==3 && view.parameter_count()==2 &&
            view.primary_instruction_count()==7 && view.secondary_instruction_count()==1 &&
            view.compiler_version_raw()==0x0002df30 && view.literal_count()==0 && view.container_count()==1;
        vsc::gxp::ParameterView uniform{},sampler{};
        ok=ok && view.parameter(0,uniform) && uniform.name=="u_force_opaque" &&
            uniform.category==1 && uniform.component_count==1 && uniform.container_index==14 && uniform.resource_index==0 &&
            view.parameter(1,sampler) && sampler.name=="u_tex" && sampler.category==2 &&
            sampler.component_count==4 && sampler.semantic==1 && sampler.resource_index==0;
        const auto primary=view.primary_program(); const auto secondary=view.secondary_program();
        ok=ok && primary.size==sizeof(primary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            secondary.size==sizeof(secondary_word) && std::memcmp(secondary.data,&secondary_word,sizeof(secondary_word))==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: Geometrizer CMP_FS diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool fragment_cfg_mutation_changes_gxp() {
    auto snapshot=[](const std::string &text,std::vector<uint8_t> &gxp) -> bool {
        VscCompileRequest request{};
        request.source_name="generic-fragment-cfg.cg";
        request.source=text.data(); request.source_size=text.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size && !result.diagnostic_count;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            ok=view.valid();
            if (ok) gxp.assign(result.gxp_data,result.gxp_data+result.gxp_size);
        }
        vsc_destroy_result(&request.allocator,&result);
        return ok;
    };
    const std::string source=R"(
float4 main(float3 n:TEXCOORD2, float4 color:COLOR0, uniform float gain):COLOR0 {
    float4 outc=color;
    for (short i=0;i<1;i++) {
        outc.rgb += n * gain * 0.75;
    }
    return outc;
})";
    auto changed=source;
    const auto at=changed.find("0.75");
    if (at==std::string::npos) return false;
    changed.replace(at,4,"0.5");
    std::vector<uint8_t> a,b;
    return snapshot(source,a) && snapshot(changed,b) && !equal_except_guids(b.data(),b.size(),a);
}

bool vertex_mutations_follow_source(const std::string &source,
                                    const char *rewire_from,const char *rewire_to) {
    auto snapshot=[](const std::string &text,const char *name,std::vector<uint8_t> &gxp) -> bool {
        VscCompileRequest request{};
        request.source_name=name; request.source=text.data(); request.source_size=text.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size && !result.diagnostic_count;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            ok=view.valid();
            if (ok) gxp.assign(result.gxp_data,result.gxp_data+result.gxp_size);
        }
        vsc_destroy_result(&request.allocator,&result);
        return ok;
    };
    std::vector<uint8_t> original,changed;
    if (!snapshot(source,"arbitrary-input.cg",original) ||
        !snapshot(source,"different-filename.cg",changed) ||
        !equal_except_guids(changed.data(),changed.size(),original)) return false;

    // These edits preserve the resource layout and the old matcher's operation
    // counts, but change the computation. A fixed whole-shader schedule ignored them.
    const struct { const char *from; const char *to; } mutations[]={
        {"1.998","1.25"},{"0.0602059935","0.125"},{rewire_from,rewire_to},
    };
    for (const auto &mutation:mutations) {
        auto text=source;
        const auto at=text.find(mutation.from);
        if (at==std::string::npos) return false;
        text.replace(at,std::strlen(mutation.from),mutation.to);
        if (!snapshot(text,"arbitrary-input.cg",changed) ||
            equal_except_guids(changed.data(),changed.size(),original)) return false;
    }

    // Identifier renaming may change reflection strings, but not USSE selection.
    auto renamed=source;
    for (const auto *identifier:{"a_pos","u_z_max"}) {
        const std::string replacement=std::string("renamed_")+identifier;
        size_t pos=0;
        while ((pos=renamed.find(identifier,pos))!=std::string::npos) {
            renamed.replace(pos,std::strlen(identifier),replacement);
            pos+=replacement.size();
        }
    }
    if (!snapshot(renamed,"unrelated.cg",changed)) return false;
    vsc::gxp::ProgramView a(original.data(),original.size()),b(changed.data(),changed.size());
    auto same=[](vsc::gxp::ByteRange lhs,vsc::gxp::ByteRange rhs) {
        return lhs.size==rhs.size && (!lhs.size || std::memcmp(lhs.data,rhs.data,lhs.size)==0);
    };
    return same(a.primary_program(),b.primary_program()) && same(a.secondary_program(),b.secondary_program());
}

bool compile_geometrizer_tm2_fast_fragment(const std::string &source) {
    VscCompileRequest request{};
    request.source_name="fp-geometrizer-tm2-fast.cg";
    request.source=source.data(); request.source_size=source.size();
    request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult result{};
    const int rc=vsc_compile(&request,&result);
    bool ok=rc==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
    if (ok) {
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        ok=view.valid() && view.sdk_version()==0x0165 && view.flags()==0x0008080b &&
            view.primary_register_count()==4 && view.secondary_register_count()==7 &&
            view.parameter_count()==3 && view.literal_count()==5 && view.container_count()==2 &&
            view.primary_instruction_count()>=27 && view.compiler_version_raw()==0x0002df30;
        const char *names[]={"u_opaque","u_cat_match","u_page"};
        const uint8_t categories[]={1,1,2};
        const uint32_t resources[]={0,1,0};
        for (uint32_t i=0;ok && i<3;++i) {
            vsc::gxp::ParameterView parameter{};
            ok=view.parameter(i,parameter) && parameter.name==names[i] &&
                parameter.category==categories[i] && parameter.resource_index==resources[i];
        }
        bool vcomp=false,v32=false,vtst=false,vmov=false,vpck=false,branch=false,kill=false;
        const auto code=view.primary_program();
        for (size_t off=0;ok && off+sizeof(uint64_t)<=code.size;off+=sizeof(uint64_t)) {
            uint64_t word=0;
            std::memcpy(&word,code.data+off,sizeof(word));
            const auto family=vsc::usse::classify_major(word);
            const auto control=vsc::usse::classify_control(word);
            vcomp |= family==vsc::usse::MajorClass::Vcomp;
            v32 |= family==vsc::usse::MajorClass::V32Nmad;
            vtst |= family==vsc::usse::MajorClass::Vtst;
            vmov |= family==vsc::usse::MajorClass::Vmov;
            vpck |= family==vsc::usse::MajorClass::Vpck;
            branch |= control==vsc::usse::ControlClass::Branch;
            kill |= control==vsc::usse::ControlClass::Kill;
        }
        ok=ok && vcomp && v32 && vtst && vmov && vpck && branch && kill;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: Geometrizer TM2_FAST_FS diagnostic=%s\n",
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_oracle_s32_profile(const std::string &source, const char *name,
                                uint32_t expected_size, uint32_t expected_params,
                                const uint64_t *secondary_words, size_t secondary_count) {
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
        const uint64_t primary_words[]={0xfa44070000000000ULL,0x5081000ae0000000ULL};
        ok=view.valid() && view.logical_size()==expected_size && view.sdk_version()==0x0165 &&
            view.flags()==0x00080001 && view.primary_register_count()==1 &&
            view.secondary_register_count()==2 && view.parameter_count()==expected_params &&
            view.primary_instruction_count()==2 && view.secondary_instruction_count()==secondary_count;
        for (uint32_t i=0;ok && i<expected_params;++i) {
            vsc::gxp::ParameterView parameter{};
            const char expected_name=static_cast<char>('x'+i);
            ok=view.parameter(i,parameter) && parameter.name.size()==1 && parameter.name[0]==expected_name &&
                parameter.category==1 && parameter.type==4 && parameter.component_count==1 &&
                parameter.container_index==14 && parameter.resource_index==i;
        }
        const auto primary=view.primary_program();
        const auto secondary=view.secondary_program();
        if (ok) ok=primary.size==sizeof(primary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            secondary.size==secondary_count*sizeof(uint64_t) &&
            std::memcmp(secondary.data,secondary_words,secondary.size)==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s S32 diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_oracle_s32x2_profile(const std::string &source, const char *name,
                                  uint32_t expected_size, uint32_t expected_params,
                                  const uint64_t *secondary_words, size_t secondary_count) {
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
        const uint64_t primary_words[]={0xfa44070000000000ULL,0x5081000ae0000000ULL};
        ok=view.valid() && view.logical_size()==expected_size && view.sdk_version()==0x0165 &&
            view.flags()==0x00080001 && view.primary_register_count()==1 &&
            view.secondary_register_count()==(expected_params==1?2:4) &&
            view.parameter_count()==expected_params && view.primary_instruction_count()==2 &&
            view.secondary_instruction_count()==secondary_count;
        for (uint32_t i=0;ok && i<expected_params;++i) {
            vsc::gxp::ParameterView parameter{};
            const char expected_name=static_cast<char>('x'+i);
            ok=view.parameter(i,parameter) && parameter.name.size()==1 && parameter.name[0]==expected_name &&
                parameter.category==1 && parameter.type==4 && parameter.component_count==2 &&
                parameter.container_index==14 && parameter.resource_index==i*2u;
        }
        const auto primary=view.primary_program();
        const auto secondary=view.secondary_program();
        if (ok) ok=primary.size==sizeof(primary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            secondary.size==secondary_count*sizeof(uint64_t) &&
            std::memcmp(secondary.data,secondary_words,secondary.size)==0;
    }
    vsc_destroy_result(&request.allocator,&result);
    return ok;
}

bool compile_oracle_s32_to_f32_profile(const std::string &source, const char *name) {
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
        const uint64_t primary_words[]={0xfa44070000000000ULL,0x40810d46e0000000ULL};
        const uint64_t secondary_words[]={
            0x6881000aa080001fULL,0xd0800006a020c004ULL,0xd0900006a020c001ULL,
            0x58800002a0200084ULL,0x40810786a0c00081ULL,0x40810786a0800080ULL,
            0x00800086a0403042ULL,0x50810422a0000000ULL,0x5084000aa0000002ULL,
        };
        vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
        vsc::gxp::ParameterView parameter{};
        ok=view.valid() && view.logical_size()==314 && view.sdk_version()==0x0165 &&
            view.flags()==0x00080001 && view.primary_register_count()==1 &&
            view.secondary_register_count()==7 && view.literal_count()==2 &&
            view.container_count()==2 && view.compiler_version_raw()==0x0002df30 &&
            view.parameter_count()==1 && view.parameter(0,parameter) && parameter.name=="x" &&
            parameter.category==1 && parameter.type==4 && parameter.component_count==1 &&
            parameter.container_index==14 && parameter.resource_index==0 &&
            view.primary_instruction_count()==2 && view.secondary_instruction_count()==9;
        const auto primary=view.primary_program();
        const auto secondary=view.secondary_program();
        if (ok) ok=primary.size==sizeof(primary_words) &&
            std::memcmp(primary.data,primary_words,sizeof(primary_words))==0 &&
            secondary.size==sizeof(secondary_words) &&
            std::memcmp(secondary.data,secondary_words,sizeof(secondary_words))==0;
    }
    if (!ok && result.diagnostic_count && result.diagnostics)
        std::fprintf(stderr,"test_cg_frontend: %s S32->F32 diagnostic=%s\n",name,
                     result.diagnostics[0].message ? result.diagnostics[0].message : "(null)");
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
    if (!compile("float main(float a:TEXCOORD0):COLOR0{short x=(short)a;return (float)x;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg short spelling did not normalize to glslang HLSL");
    if (!compile("float main(float a:TEXCOORD0):COLOR0{unsigned short x=(unsigned short)a;return (float)x;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg unsigned short spelling did not normalize to glslang HLSL");
    if (!compile("float main(float a:TEXCOORD0):COLOR0{unsigned int x=(unsigned int)a;return (float)x;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg unsigned int spelling did not normalize to glslang HLSL");
    if (!compile("float main(float a:TEXCOORD0):COLOR0{unsigned int x=bit_cast<unsigned int>(a);return (float)x;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg scalar bit_cast<unsigned int> did not normalize to HLSL asuint");
    {
        const std::string source=
            "float fx(float v){return float(bit_cast<short2>(v).y)+"
            "float(bit_cast<unsigned short2>(v).x)*(1.0f/65536.0f);}"
            "void main(float4 p,float2 uv,float4 c,float2 out tc:TEXCOORD0,float4 out pos:POSITION,"
            "float4 out col:COLOR,float out ps:PSIZE,uniform float4x4 mvp,uniform float4x4 texmat[1],"
            "uniform float point_size){p=float4(fx(p.x),fx(p.y),fx(p.z),fx(p.w));"
            "uv=float2(fx(uv.x),fx(uv.y));pos=mul(mvp,p);tc=mul(texmat[0],float4(uv,0.f,1.f)).xy;"
            "col=c;ps=point_size;}";
        auto snapshot=[](const std::string &text,const char *name,std::vector<uint8_t> &bytes) {
            VscCompileRequest request{};
            request.source_name=name; request.source=text.data(); request.source_size=text.size();
            request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
            VscCompileResult result{};
            const bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size &&
                result.diagnostic_count==0 && vsc::gxp::ProgramView(result.gxp_data,result.gxp_size).valid();
            if (ok) bytes.assign(result.gxp_data,result.gxp_data+result.gxp_size);
            else if (result.diagnostic_count)
                std::fprintf(stderr,"test_cg_frontend: %s: %s\n",name,result.diagnostics[0].message);
            vsc_destroy_result(&request.allocator,&result);
            return ok;
        };
        auto has_literal=[](const std::vector<uint8_t> &bytes,uint32_t bits) {
            if (bytes.size()<0x80) return false;
            auto u32=[&](size_t offset) { uint32_t v=0; std::memcpy(&v,bytes.data()+offset,4); return v; };
            const size_t count=u32(0x70),offset=0x74u+u32(0x74);
            if (offset>bytes.size() || count>(bytes.size()-offset)/8u) return false;
            for (size_t i=0;i<count;++i) if (u32(offset+i*8u+4u)==bits) return true;
            return false;
        };
        std::vector<uint8_t> original,changed;
        bool ok=snapshot(source,"packed-input.cg",original);
        if (ok) {
            vsc::gxp::ProgramView view(original.data(),original.size());
            const auto code=view.primary_program(),interface=view.varyings();
            bool point_write=false;
            unsigned scaled_low_unpack_count=0,high_unpack_count=0;
            for (size_t offset=0;offset+8<=code.size;offset+=8) {
                uint64_t word=0; std::memcpy(&word,code.data+offset,8);
                vsc::usse::Vpck16ToF32Semantic unpack{};
                vsc::usse::VmovSemantic move{};
                if (vsc::usse::decode_vpck16_to_f32_semantic(word,&unpack)) {
                    scaled_low_unpack_count += unpack.src_format==vsc::usse::PackFormat::U16 &&
                        unpack.scale;
                    high_unpack_count += unpack.src_format==vsc::usse::PackFormat::S16 &&
                        !unpack.scale;
                }
                if (vsc::usse::decode_vmov_semantic(word,&move) && move.dst.bank==vsc::usse::RegisterBank::Output) {
                    point_write |= move.dst.num==5 && move.dest_mask==1;
                    // Float VMOV indices address pairs; PSIZE word 10 is O5.x, not O10.x.
                    ok=ok && move.dst.num<6;
                }
            }
            ok=ok && view.minor_version()==4 && view.sdk_version()==0x0165 &&
                view.primary_register_count()==12 && view.parameter_count()==6 &&
                view.primary_instruction_count()==40 && view.secondary_instruction_count()==0 &&
                scaled_low_unpack_count==6 && high_unpack_count==6 && point_write &&
                interface.size==32 && interface.data[19]==11 && !has_literal(original,0x37800000u);
            const uint32_t offsets[]={0,4,8,0,16,32};
            for (uint32_t i=0;ok && i<6;++i) {
                vsc::gxp::ParameterView parameter{};
                ok=view.parameter(i,parameter) && parameter.resource_index==offsets[i];
            }
        }
        const struct { const char *from; const char *to; } mutations[]={
            {"65536.0f","32768.0f"},
            {"(v).y)+","(v).y)-"},
            {"fx(p.x)","fx(p.y)"},
            {"col=c;","col=c*0.75f;"},
            {"ps=point_size;","ps=point_size*0.5f;"},
            {"ps=point_size;","ps=point_size+1.0f/65536.0f;"},
            {"col=c;","col=float4(c.x,0.25f,c.z,1.f);"},
            {"pos=mul(mvp,p);","pos=p;"},
            {"uniform float point_size){","uniform float point_size,uniform float shift){p.x+=shift;"},
        };
        for (const auto &mutation:mutations) {
            auto text=source; const auto at=text.find(mutation.from);
            if (at==std::string::npos) { ok=false; break; }
            text.replace(at,std::strlen(mutation.from),mutation.to);
            const bool compiled=snapshot(text,"changed-expression.cg",changed);
            if (!compiled || changed==original) {
                std::fprintf(stderr,"test_cg_frontend: ignored fixed16 mutation: %s\n",mutation.to);
                ok=false;
            }
            if (std::strcmp(mutation.from,"65536.0f")==0)
                ok=ok && has_literal(changed,0x38000000u) && !has_literal(changed,0x37800000u);
            if (std::strcmp(mutation.to,"ps=point_size+1.0f/65536.0f;")==0)
                ok=ok && has_literal(changed,0x37800000u);
        }
        std::string renamed;
        for (size_t i=0;i<source.size();) {
            if (std::isalpha(static_cast<unsigned char>(source[i])) || source[i]=='_') {
                const size_t start=i++;
                while (i<source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i]=='_')) ++i;
                const auto token=source.substr(start,i-start);
                const char *names[]={"fx","v","p","uv","c","tc","pos","col","ps","mvp","texmat","point_size"};
                bool replace=false;
                for (const char *name:names) replace |= token==name;
                renamed += replace ? "renamed_"+token : token;
            } else renamed+=source[i++];
        }
        if (!snapshot(renamed,"unrelated-filename.cg",changed)) ok=false;
        else {
            const auto a=vsc::gxp::ProgramView(original.data(),original.size()).primary_program();
            const auto b=vsc::gxp::ProgramView(changed.data(),changed.size()).primary_program();
            ok=ok && a.size!=0 && a.size==b.size && std::memcmp(a.data,b.data,a.size)==0;
        }
        const std::string ordinary=
            "void main(float4 a:TEXCOORD0,float2 b:TEXCOORD1,float4 c:TEXCOORD2,"
            "out float4 p:POSITION,out float4 shade:COLOR,out float2 t:TEXCOORD0,out float s:PSIZE){"
            "p=float4(a.xyz,1.f);shade=c*0.75f;t=b;s=3.f;}";
        ok=snapshot(ordinary,"no-fixed-conversion-or-matrices.cg",changed) && ok;
        if (!ok) failures += fail("generic color/texcoord/point vertex did not follow expressions, literals or renames");
    }
    if (!compile("void f(float4 inout a){a+=1.f;} float4 main(float4 a:TEXCOORD0):COLOR0{f(a);return a;}", VSC_STAGE_FRAGMENT))
        failures += fail("Cg post-type inout qualifier normalization did not compile");
    if (!compile("float4 main(float4 p:POSITION,uniform float4x4 model,uniform float4x4 mvp):POSITION{"
                 "mvp=mul(mvp,model);return mul(mvp,p);}", VSC_STAGE_VERTEX))
        failures += fail("Cg mutable uniform parameter was not normalized to a local copy");
    if (!compile_matrix_texcoord_point_size_profile())
        failures += fail("Cg mat4[1] TEXCOORD transform + PSIZE did not reproduce SDK 3.0 codegen");
    {
        const std::string source=
            "void main(float4 p,float2 uv0,float2 uv1,float4 c,uniform float4x4 mvp,"
            "uniform float4x4 texmat[2],uniform float point_size,float2 out tc0:TEXCOORD0,"
            "float2 out tc1:TEXCOORD1,float4 out pos:POSITION,float4 out col:COLOR,float out ps:PSIZE){"
            "pos=mul(mvp,p);tc0=mul(texmat[0],float4(uv0,0.f,1.f)).xy;"
            "tc1=mul(texmat[1],float4(uv1,0.f,1.f)).xy;col=c;ps=point_size;}";
        VscCompileRequest request{};
        request.source_name="vp-mat4-array2-texcoords-color-psize";
        request.source=source.data(); request.source_size=source.size(); request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            ok=view.valid() && view.minor_version()==5 && view.sdk_version()==0x0300 &&
                view.flags()==0x00190000 && view.primary_register_count()==16 &&
                view.secondary_register_count()==52 && view.primary_instruction_count()==14 &&
                view.secondary_instruction_count()==3 && view.compiler_version_raw()==0x00033a90 &&
                view.parameter_count()==7;
        }
        if (!ok) failures += fail("Cg mat4[2] dual TEXCOORD + COLOR + PSIZE did not reproduce SDK 3.0 metadata");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        const std::string source=
            "void main(float4 p,float2 uv0,float2 uv1,float2 uv2,float4 c,uniform float4x4 mvp,"
            "uniform float4x4 texmat[3],uniform float point_size,float2 out tc0:TEXCOORD0,"
            "float2 out tc1:TEXCOORD1,float2 out tc2:TEXCOORD2,float4 out pos:POSITION,"
            "float4 out col:COLOR,float out ps:PSIZE){pos=mul(mvp,p);"
            "tc0=mul(texmat[0],float4(uv0,0.f,1.f)).xy;"
            "tc1=mul(texmat[1],float4(uv1,0.f,1.f)).xy;"
            "tc2=mul(texmat[2],float4(uv2,0.f,1.f)).xy;col=c;ps=point_size;}";
        VscCompileRequest request{};
        request.source_name="vp-mat4-array3-texcoords-color-psize";
        request.source=source.data(); request.source_size=source.size(); request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            ok=view.valid() && view.minor_version()==5 && view.sdk_version()==0x0300 &&
                view.flags()==0x00190000 && view.primary_register_count()==20 &&
                view.secondary_register_count()==68 && view.primary_instruction_count()==18 &&
                view.secondary_instruction_count()==3 && view.compiler_version_raw()==0x00033a90 &&
                view.parameter_count()==8;
        }
        if (!ok) failures += fail("Cg mat4[3] triple TEXCOORD + COLOR + PSIZE did not reproduce SDK 3.0 metadata");
        vsc_destroy_result(&request.allocator,&result);
    }
    if (!compile("float2 in uv:TEXCOORD0;\nfloat sample_x(){return uv.x;}\nfloat4 main():COLOR0{return float4(sample_x(),0,0,1);}\n", VSC_STAGE_FRAGMENT))
        failures += fail("Cg global input semantic did not normalize into an entry-point parameter");
    if (!compile("float4 out gl_Position:POSITION;\nfloat2 out uv:TEXCOORD0;\nvoid main(float4 p:POSITION){gl_Position=p;uv=p.xy;}\n", VSC_STAGE_VERTEX))
        failures += fail("Cg global output semantics did not normalize into entry-point parameters");
#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    if (!compile_shader_gxp("uniform float3x3 unused_matrix; float4 main(float4 p:POSITION):POSITION{return p;}",
                            "unused-matrix-uniform.cg",VSC_STAGE_VERTEX))
        failures += fail("unused unsupported uniform member type blocked an otherwise valid shader");
    if (!compile_shader_gxp(
            "uniform float2 dead_scale; uniform float4 live_color; float4 main():COLOR0{return live_color;}",
            "dead-prefix-uniform.cg",VSC_STAGE_FRAGMENT))
        failures += fail("dead supported uniform prefix was not removed before compact reflection/lowering");
    if (!compile_shader_gxp(
            "uniform sampler2D tex[1]; uniform float4 tint; "
            "float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(tex[0],uv)*tint;}",
            "sampler-array1-texture-tint.cg",VSC_STAGE_FRAGMENT))
        failures += fail("constant sampler2D[1] element 0 did not lower through texture-tint profile");
    {
        static constexpr const char *source=
            "uniform sampler2D tex[2]; "
            "float4 main(float2 uv:TEXCOORD1):COLOR0{return tex2D(tex[1],uv);}";
        VscCompileRequest request{};
        request.source_name="sampler-array2-binding1-texcoord1.cg";
        request.source=source; request.source_size=std::strlen(source);
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            vsc::gxp::ParameterView tex{};
            const auto query=view.sampler_query_info();
            uint16_t query1=0;
            if (query.size>=4) std::memcpy(&query1,query.data+2,sizeof(query1));
            const uint64_t phas=0xfa44070000000000ULL;
            const auto primary=view.primary_program();
            ok=view.valid() && result.gxp_size==256 && view.logical_size()==256 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00180801 &&
                view.primary_register_count()==2 && view.secondary_register_count()==0 &&
                view.compiler_version_raw()==0x00033a90 && view.container_count()==0 &&
                view.parameter_count()==1 && view.parameter(0,tex) && tex.category==2 &&
                tex.resource_index==1 && query.size==32 && query1==0x0302 &&
                primary.size==sizeof(phas) && std::memcmp(primary.data,&phas,sizeof(phas))==0;
        }
        if (!ok) failures += fail("sampler2D[2] element 1 / TEXCOORD1 did not reproduce SDK 3.0 direct-texture metadata");
        vsc_destroy_result(&request.allocator,&result);
    }
    if (!compile_vitagl_blit_vertex_profile())
        failures += fail("vitaGL public blit vertex profile did not reproduce its observed v1.5 GXP shape");
    if (!compile_texture_tint_alpha_discard_profile())
        failures += fail("texture-tint alpha discard profile did not reproduce the oracle-derived semantic stream");
    {
        const std::string source=
            "float4 main(float4 vColor:COLOR0,float4 coords:WPOS,uniform float4 KfogColor,"
            "uniform float Mfog_range,uniform float Nfog_far):COLOR0{"
            "float4 out_color=vColor;float d=coords.z/coords.w;"
            "float f=clamp((Nfog_far-d)/Mfog_range,0.0f,1.0f);"
            "out_color.rgb=lerp(KfogColor.rgb,vColor.rgb,f);return out_color;}";
        VscCompileRequest request{};
        request.source_name="linear-fog-sdk30.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t primary[]={
                0xfa44070000000000ULL,0x3080080a80400181ULL,0x08800880af000083ULL,
                0x008008a0ff13c042ULL,0x08a408843f045f03ULL,0x08a508801f006f00ULL,
                0x40c00d9cffa18002ULL,0x18b18bc00f41113dULL,0x18b1818280011100ULL,
                0x18b1808280409001ULL,0x40800d7ea0198002ULL,
            };
            const uint64_t secondary[]={
                0x3080000280200102ULL,0x08841082a0a48081ULL,0xf804014000000000ULL,
            };
            const auto p=view.primary_program(),s=view.secondary_program();
            ok=view.valid() && result.gxp_size==428 && view.logical_size()==426 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00181001 &&
                view.primary_register_count()==8 && view.secondary_register_count()==7 &&
                view.compiler_version_raw()==0x00033a90 && view.literal_count()==1 &&
                view.container_count()==2 && view.parameter_count()==3 &&
                p.size==sizeof(primary) && s.size==sizeof(secondary) &&
                std::memcmp(p.data,primary,sizeof(primary))==0 &&
                std::memcmp(s.data,secondary,sizeof(secondary))==0;
        }
        if (!ok) failures += fail("linear fog did not reproduce the SDK 3.0.0 GXP profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        const std::string source=
            "float4 main(float4 vColor:COLOR0,float4 coords:WPOS,uniform float4 KfogColor,"
            "uniform float Hfog_density):COLOR0{"
            "float4 out_color=vColor;float d=coords.z/coords.w;"
            "float f=clamp(exp(-Hfog_density*Hfog_density*d*d*1.4426950408889634f*1.4426950408889634f),0.0f,1.0f);"
            "out_color.rgb=lerp(KfogColor.rgb,vColor.rgb,f);return out_color;}";
        VscCompileRequest request{};
        request.source_name="exp2-fog-sdk30.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t primary[]={
                0xfa44070000000000ULL,0x3080080a80400181ULL,0x08800880af400083ULL,
                0x18e18880df018002ULL,0x20c42000cfb61080ULL,0x30800e000f803e01ULL,
                0x08a408843f045f03ULL,0x08a508801f006f00ULL,0x18b18bc00f41113dULL,
                0x18b1818280011100ULL,0x18b1808280409001ULL,0x40800d7ea0198002ULL,
            };
            const uint64_t secondary[]={
                0x08a410a6a1040083ULL,0x08800082a0800102ULL,0xf804014000000000ULL,
            };
            const auto p=view.primary_program(),s=view.secondary_program();
            ok=view.valid() && result.gxp_size==420 && view.logical_size()==419 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00181001 &&
                view.primary_register_count()==8 && view.secondary_register_count()==9 &&
                view.compiler_version_raw()==0x00033a90 && view.literal_count()==2 &&
                view.container_count()==2 && view.parameter_count()==2 &&
                p.size==sizeof(primary) && s.size==sizeof(secondary) &&
                std::memcmp(p.data,primary,sizeof(primary))==0 &&
                std::memcmp(s.data,secondary,sizeof(secondary))==0;
        }
        if (!ok) failures += fail("exp2 fog did not reproduce the SDK 3.0.0 GXP profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    if (!compile_fragment_gxp(
            "float main(float x:TEXCOORD0):COLOR0{return exp(x);}",
            "scalar-exp.cg"))
        failures += fail("scalar Exp did not lower through LOG2E + Exp2 VCOMP");
    {
        const std::string source=
            "float4 main(float4 c:COLOR0):COLOR0{"
            "float3 cutoff=float3(c.r<0.0031308f?1.0f:0.0f,c.g<0.0031308f?1.0f:0.0f,c.b<0.0031308f?1.0f:0.0f);"
            "float3 higher=float3(1.055f)*pow(c.rgb,float3(1.0f/2.4f))-float3(0.055f);"
            "float3 lower=c.rgb*float3(12.92f);return float4(lerp(higher,lower,cutoff),c.a);}";
        VscCompileRequest request{};
        request.source_name="srgb-sdk30.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t primary[]={
                0xfa44070000000000ULL,0x3080044080000001ULL,0x3080044880000002ULL,
                0x30800c4080200081ULL,0x08a40b843f440002ULL,0x20c54000a0150480ULL,
                0x30800e080fc03e82ULL,0x30800e100fc03e84ULL,0x00a00b80ff53e041ULL,
                0x08c07b20cfa49080ULL,0x40c10d8c20013e80ULL,0x38c14d00d3000000ULL,
                0x08c008a4ef289080ULL,0x38c14d00d1fbc000ULL,0x00a0898ec010003dULL,
                0x0080488ccf10103dULL,0x38800d4006f80000ULL,0x18b18182a0111100ULL,
                0x18b180822048903cULL,0x40800d7ea0198002ULL,
            };
            const auto p=view.primary_program();
            ok=view.valid() && result.gxp_size==412 && view.logical_size()==412 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00181001 &&
                view.primary_register_count()==4 && view.secondary_register_count()==6 &&
                view.compiler_version_raw()==0x00033a90 && view.literal_count()==6 &&
                view.container_count()==1 && view.parameter_count()==0 &&
                p.size==sizeof(primary) && std::memcmp(p.data,primary,sizeof(primary))==0;
        }
        if (!ok) failures += fail("sRGB conversion did not reproduce the SDK 3.0.0 GXP profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        vsc::backend::IrCompileResult combined;
        if (!vsc::backend::compile_fragment_two_texture_combine({"tex1",1},{"tex2",2},0,0,combined)) {
            failures += fail("TEXUNIT1/2 semantic combine profile did not compile");
        } else {
            vsc::gxp::ProgramView view(combined.gxp.data(),combined.gxp.size());
            vsc::gxp::ParameterView tex1{},tex2{};
            const uint64_t words[]={
                0xfa44070000000000ULL,0xf800094000000000ULL,0x40c00d9caf818206ULL,
                0x18a18b848f1d0100ULL,0x08a51b841f045f01ULL,0x08a50b841f046f00ULL,
                0x08841400af2480c1ULL,0x40810d7e2019bc00ULL,
            };
            const auto primary=view.primary_program();
            const auto query=view.sampler_query_info();
            uint16_t query1=0,query2=0;
            if (query.size>=6) {
                std::memcpy(&query1,query.data+2,sizeof(query1));
                std::memcpy(&query2,query.data+4,sizeof(query2));
            }
            if (!view.valid() || combined.gxp.size()!=352 || view.logical_size()!=350 ||
                view.minor_version()!=5 || view.sdk_version()!=0x0300 || view.flags()!=0x00180801 ||
                view.compiler_version_raw()!=0x00033a90 || view.primary_register_count()!=8 ||
                view.secondary_register_count()!=0 || view.primary_instruction_count()!=8 ||
                view.parameter_count()!=2 || !view.parameter(0,tex1) || !view.parameter(1,tex2) ||
                tex1.category!=2 || tex1.resource_index!=1 || tex2.category!=2 || tex2.resource_index!=2 ||
                query.size!=32 || query1!=0x0301 || query2!=0x0301 ||
                primary.size!=sizeof(words) || std::memcmp(primary.data,words,sizeof(words))!=0 ||
                combined.gxp.size()<=0xcc || combined.gxp[0xcc]!=0x30)
                failures += fail("TEXUNIT1/2 semantic combine metadata mismatch");
        }
    }
    if (!compile_matrix_point_size_profile(false))
        failures += fail("oracle matrix + uniform PSIZE vertex profile did not reproduce the validated stream");
    if (!compile_matrix_point_size_profile(true))
        failures += fail("oracle matrix + COLOR + uniform PSIZE profile did not reproduce the validated stream");
    if (!compile_matrix_normal_multivarying_profile())
        failures += fail("matrix + normal + dense multivarying vertex profile did not preserve the Sony reflection layout");
    if (!compile_indexed_clear_vertex_profile())
        failures += fail("indexed clear vertex profile did not reproduce the public vitaGL semantic stream");
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/research/public_samples/libvita2d/clear_v.cg");
        VscCompileRequest request{};
        request.source_name="clear_v"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
        VscCompileResult result{};
        bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t words[]={
                0xfa44070000000000ULL,0x08a5118590040001ULL,
                0x0883118190568001ULL,0xfb275000a0200000ULL,
            };
            const auto code=view.primary_program();
            ok=view.valid() && result.gxp_size==252 && view.logical_size()==250 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00190004 &&
                view.primary_register_count()==4 && view.secondary_register_count()==0 &&
                view.container_count()==0 && view.parameter_count()==1 &&
                view.compiler_version_raw()==0x00033a90 && code.size==sizeof(words) &&
                std::memcmp(code.data,words,sizeof(words))==0;
        }
        if (!ok) failures += fail("clear_v did not reproduce SDK 3.0.0 constructed-position profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        struct MatrixVertexProbe { const char *name; uint32_t logical_size; uint64_t move_word; };
        const MatrixVertexProbe probes[]={
            {"color_v",341,0x38801d2183080080ULL},
            {"texture_v",344,0x38800d2183080080ULL},
        };
        for (const auto &probe:probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+
                                               "/research/public_samples/libvita2d/"+probe.name+".cg");
            VscCompileRequest request{};
            request.source_name=probe.name; request.source=source.data(); request.source_size=source.size();
            request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
            VscCompileResult result{};
            bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
            if (ok) {
                vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
                const uint64_t words[]={
                    0xfa44070000000000ULL,probe.move_word,
                    0x40c00d9caf818002ULL,0x40c00dbcffb9860eULL,
                    0x18b18f80cf491104ULL,0x18b18f80cf451102ULL,
                    0x18b18181c0011100ULL,0x18b18181c042d101ULL,
                    0xfb275000a0200000ULL,
                };
                const auto code=view.primary_program();
                ok=view.valid() && result.gxp_size==344 && view.logical_size()==probe.logical_size &&
                    view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00190000 &&
                    view.primary_register_count()==8 && view.secondary_register_count()==16 &&
                    view.container_count()==1 && view.parameter_count()==3 &&
                    view.compiler_version_raw()==0x00033a90 && code.size==sizeof(words) &&
                    std::memcmp(code.data,words,sizeof(words))==0;
            }
            if (!ok) {
                std::fprintf(stderr,"test_cg_frontend: %s did not reproduce SDK 3.0.0 matrix vertex profile\n",probe.name);
                ++failures;
            }
            vsc_destroy_result(&request.allocator,&result);
        }
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/research/public_samples/libvita2d/texture_f.cg");
        VscCompileRequest request{};
        request.source_name="texture_f.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const auto query=view.sampler_query_info();
            uint16_t query0=0;
            if (query.size>=sizeof(query0)) std::memcpy(&query0,query.data,sizeof(query0));
            ok=view.valid() && view.logical_size()==256 && view.minor_version()==5 && view.sdk_version()==0x0300 &&
                view.flags()==0x00180801 && view.secondary_register_count()==0 && view.container_count()==0 &&
                view.compiler_version_raw()==0x00033a90 && query.size==32 && query0==0x0302;
        }
        if (!ok) failures += fail("texture_f did not reproduce the SDK 3.0.0 standalone texture profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/research/public_samples/libvita2d/texture_tint_f.cg");
        VscCompileRequest request{};
        request.source_name="texture_tint_f.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const auto query=view.sampler_query_info();
            const auto code=view.primary_program();
            uint16_t query0=0;
            if (query.size>=sizeof(query0)) std::memcpy(&query0,query.data,sizeof(query0));
            const uint64_t words[]={
                0xfa44070000000000ULL,0xf800094000000000ULL,
                0x40c00dbcff998002ULL,0x40800dbcafb98002ULL,
                0x10a4478600040f7cULL,
            };
            ok=view.valid() && result.gxp_size==324 && view.logical_size()==323 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00180805 &&
                view.primary_register_count()==4 && view.secondary_register_count()==4 &&
                view.container_count()==1 && view.compiler_version_raw()==0x00033a90 &&
                query.size==32 && query0==0x0301 && code.size==sizeof(words) &&
                std::memcmp(code.data,words,sizeof(words))==0;
        }
        if (!ok) failures += fail("texture_tint_f did not reproduce the SDK 3.0.0 texture-tint profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/research/public_samples/libvita2d/color_f.cg");
        VscCompileRequest request{};
        request.source_name="color_f.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t words[]={0xfa44070000000000ULL,0x40800d7ea0198002ULL};
            const auto code=view.primary_program();
            ok=view.valid() && result.gxp_size==212 && view.logical_size()==212 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00181001 &&
                view.primary_register_count()==4 && view.secondary_register_count()==0 &&
                view.container_count()==0 && view.compiler_version_raw()==0x00033a90 &&
                code.size==sizeof(words) && std::memcmp(code.data,words,sizeof(words))==0;
        }
        if (!ok) failures += fail("color_f did not reproduce the SDK 3.0.0 varying-color profile");
        vsc_destroy_result(&request.allocator,&result);
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/research/public_samples/libvita2d/clear_f.cg");
        VscCompileRequest request{};
        request.source_name="clear_f.cg"; request.source=source.data(); request.source_size=source.size();
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=!source.empty() && vsc_compile(&request,&result)==0 && result.gxp_data && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const uint64_t words[]={0xfa44070000000000ULL,0x40800d7ef0198002ULL};
            const auto code=view.primary_program();
            ok=view.valid() && result.gxp_size==232 && view.logical_size()==232 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00180001 &&
                view.primary_register_count()==2 && view.secondary_register_count()==4 &&
                view.secondary_instruction_count()==0 && view.container_count()==1 &&
                view.compiler_version_raw()==0x00033a90 && code.size==sizeof(words) &&
                std::memcmp(code.data,words,sizeof(words))==0;
        }
        if (!ok) failures += fail("clear_f did not reproduce the SDK 3.0.0 uniform-color profile");
        vsc_destroy_result(&request.allocator,&result);
    }
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
    if (!fragment_cfg_mutation_changes_gxp())
        failures += fail("generic fragment CFG mutation did not affect generated GXP");

    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-geometrizer-poly.cg");
        if (source.empty() || !compile_geometrizer_poly_vertex(source))
            failures += fail("Geometrizer POLY_VS integration profile did not compile through generic vertex Machine IR");
        if (source.empty() || !vertex_mutations_follow_source(source,"a_pos.x/u_screen_size.x","a_pos.y/u_screen_size.x"))
            failures += fail("POLY constant/operand mutations or renames did not follow source semantics");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-geometrizer-poly3d.cg");
        if (source.empty() || !compile_geometrizer_poly3d_vertex(source))
            failures += fail("Geometrizer POLY3D_VS integration profile did not compile through generic vertex Machine IR");
        if (source.empty() || !vertex_mutations_follow_source(source,"ax*vz+bx*vx","ax*vx+bx*vz"))
            failures += fail("POLY3D constant/operand mutations or renames did not follow source semantics");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-geometrizer-cmp.cg");
        if (source.empty() || !compile_geometrizer_cmp_fragment(source))
            failures += fail("Geometrizer CMP_FS integration profile did not reproduce oracle texture/alpha-select stream");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-geometrizer-tm2-fast.cg");
        if (source.empty() || !compile_geometrizer_tm2_fast_fragment(source))
            failures += fail("Geometrizer TM2_FAST_FS integration profile did not compile through texture-control Machine IR");
    }
    {
        const char *probes[]={"fp-floor-float","fp-mod-float","fp-discard-and"};
        for (const char *probe:probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
            if (source.empty() || !compile_fragment_gxp(source,probe)) {
                std::fprintf(stderr,"test_cg_frontend: scalar/control primitive probe failed: %s\n",probe);
                ++failures;
            }
        }
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
        const char *narrow_types[]={"float2","float3","half2"};
        const char *narrow_ops[]={"add","sub","mul","div","min","max","sat","abs","neg","mad","dot"};
        for (const char *type:narrow_types) {
            for (const char *op:narrow_ops) {
                const std::string probe=std::string("fp-")+op+"-"+type;
                const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
                if (source.empty() || !compile_fragment_gxp(source,probe.c_str())) {
                    std::fprintf(stderr,"test_cg_frontend: narrow ALU probe failed: %s\n",probe.c_str());
                    ++failures;
                }
            }
        }
    }
    {
        const char *scalar_types[]={"float","half"};
        const char *scalar_ops[]={"add","sub","mul","div","min","max","sat","abs","neg","mad","dot"};
        for (const char *type:scalar_types) {
            for (const char *op:scalar_ops) {
                const std::string probe=std::string("fp-")+op+"-"+type;
                const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
                if (source.empty() || !compile_fragment_gxp(source,probe.c_str())) {
                    std::fprintf(stderr,"test_cg_frontend: scalar ALU probe failed: %s\n",probe.c_str());
                    ++failures;
                }
            }
        }
    }
    {
        const std::string div1=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-div-float.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x308008088f800001ULL,0x3880050081f40000ULL,0x10a4008600040f7cULL,
        };
        if (div1.empty() || !compile_oracle_fragment_profile(div1,"fp-div-float.cg",
                248,0x00081005,2,0,0,words,5))
            failures += fail("Cg scalar F32 division did not reproduce oracle profile");
    }
    {
        const std::string div2=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-div-float2.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x308008008f800081ULL,0x308008088f800082ULL,
            0x3880052083f40000ULL,0x10a4418600040f7cULL,
        };
        if (div2.empty() || !compile_oracle_fragment_profile(div2,"fp-div-float2.cg",
                256,0x00081005,4,0,0,words,6))
            failures += fail("Cg F32x2 division did not reproduce oracle profile");
    }
    {
        const std::string dot2=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-dot-float2.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x08c11f889f040041ULL,0x3880052083f40000ULL,0x10c0418a00047f7cULL,
        };
        if (dot2.empty() || !compile_oracle_fragment_profile(dot2,"fp-dot-float2.cg",
                248,0x00081005,4,0,0,words,5))
            failures += fail("Cg F32x2 dot-splat did not reproduce oracle profile");
    }
    {
        const std::string div3=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-div-float3.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x308008008f800101ULL,0x308008088f800102ULL,0x308008008f800184ULL,
            0x40800d9cafa18002ULL,0x10a4438600040f7cULL,
        };
        if (div3.empty() || !compile_oracle_fragment_profile(div3,"fp-div-float3.cg",
                264,0x00081005,8,0,0,words,7))
            failures += fail("Cg F32x3 division did not reproduce oracle profile");
    }
    {
        const std::string dot3=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/fp-dot-float3.cg");
        const uint64_t words[]={
            0xfa44070000000000ULL,0xf800094000000000ULL,
            0x40c00d9caf818002ULL,0x40800d9cafa18206ULL,0x10c0f38600047f3dULL,
        };
        if (dot3.empty() || !compile_oracle_fragment_profile(dot3,"fp-dot-float3.cg",
                248,0x00081005,8,0,0,words,5))
            failures += fail("Cg F32x3 dot-splat did not reproduce oracle profile");
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
    {
        struct S32Probe {
            const char *name;
            uint32_t logical_size;
            uint32_t params;
            uint64_t operation;
            bool has_operation;
        };
        const S32Probe probes[]={
            {"fp-s32-uniform-pass",226,1,0,false},
            {"fp-s32-uniform-or",252,2,0x5080000aa0000080ULL,true},
            {"fp-s32-uniform-xor",234,1,0x58810002a0090034ULL,true},
            {"fp-s32-uniform-and",234,1,0x50810002a000407fULL,true},
            {"fp-s32-uniform-shl",234,1,0x60810002a0000003ULL,true},
            {"fp-s32-uniform-shr",234,1,0x6881000aa0000003ULL,true},
        };
        for (const auto &probe:probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe.name+".cg");
            const uint64_t secondary_pass[]={0x40850946a0000000ULL};
            const uint64_t secondary_op[]={probe.operation,0x40850946a0000000ULL};
            const uint64_t *secondary=probe.has_operation ? secondary_op : secondary_pass;
            const size_t count=probe.has_operation ? 2 : 1;
            if (source.empty() || !compile_oracle_s32_profile(source,probe.name,probe.logical_size,
                    probe.params,secondary,count)) {
                std::fprintf(stderr,"test_cg_frontend: scalar S32 probe failed: %s\n",probe.name);
                ++failures;
            }
        }
    }
    {
        const uint64_t words[]={
            0xfa44070000000000ULL,0x40810d46a0000000ULL,
            0x10a40084a0042000ULL,0x10a400a620041000ULL,
        };
        const char *probes[]={"fp-s32-input-pass","fp-f32-to-s32"};
        for (const char *probe:probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
            if (source.empty() || !compile_oracle_fragment_profile(source,probe,
                    224,0x00081005,1,0,0,words,4)) {
                std::fprintf(stderr,"test_cg_frontend: F32->S32 input/conversion probe failed: %s\n",probe);
                ++failures;
            }
        }
    }
    {
        const uint64_t pack[]={0x408106caa0000080ULL,0x4085094ea0010000ULL};
        const uint64_t or_words[]={
            0x5080000aa0200181ULL,0x5080000aa0000100ULL,
            0x408106caa0000080ULL,0x4085094ea0010000ULL,
        };
        struct Int2Probe { const char *name; uint32_t logical_size; uint32_t params; const uint64_t *words; size_t count; };
        const Int2Probe probes[]={
            {"fp-s32x2-uniform-pass",234,1,pack,2},
            {"fp-s32x2-uniform-or",268,2,or_words,4},
        };
        for (const auto &probe:probes) {
            const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe.name+".cg");
            if (source.empty() || !compile_oracle_s32x2_profile(source,probe.name,probe.logical_size,
                    probe.params,probe.words,probe.count)) {
                std::fprintf(stderr,"test_cg_frontend: S32x2 probe failed: %s\n",probe.name);
                ++failures;
            }
        }
    }
    {
        const char *probe="fp-s32-to-f32";
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/"+probe+".cg");
        if (source.empty() || !compile_oracle_s32_to_f32_profile(source,probe))
            failures += fail("Cg scalar S32->F32 conversion did not reproduce oracle profile");
    }
    {
        const std::string source=R"(
void main(float4 Nposition, float2 Otexcoord0, float4 Pcolor,
          float2 out vTexcoord:TEXCOORD0, float4 out vPosition:POSITION,
          float4 out vColor:COLOR, float out psize:PSIZE, float out vClip[1]:CLP0,
          uniform float4 Hclip_planes_eq[1], uniform float4x4 Imodelview,
          uniform float4x4 Jwvp, uniform float4x4 Ktexmat[1], uniform float Mpoint_size) {
    Jwvp=mul(Jwvp,Imodelview);
    float4 modelpos=mul(Imodelview,Nposition);
    for(short i=0;i<1;i++) vClip[i]=dot(modelpos,Hclip_planes_eq[i]);
    vPosition=mul(Jwvp,Nposition);
    vTexcoord=mul(Ktexmat[0],float4(Otexcoord0,0.0,1.0)).xy;
    vColor=Pcolor;
    psize=Mpoint_size;
})";
        auto compile_clip=[&](const std::string &text,std::vector<uint8_t> &gxp) -> bool {
            VscCompileRequest request{};
            request.source_name="vp-clip-wvp.cg";
            request.source=text.data(); request.source_size=text.size();
            request.entrypoint="main"; request.stage=VSC_STAGE_VERTEX;
            VscCompileResult result{};
            bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
            if (ok) {
                vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
                const uint8_t expected_interface[32]={
                    0x3f,0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                    0x01,0x19,0,0x0c,0x01,0,0,0,0,0,0,0,0,0,0,0,
                };
                const auto varying=view.varyings();
                vsc::gxp::ParameterView clip{},point{},modelview{},projection{},texmat{};
                bool point_write=false,clip_write=false,outputs_in_bounds=true;
                const auto code=view.primary_program();
                for (size_t offset=0;offset+8<=code.size;offset+=8) {
                    uint64_t word=0; std::memcpy(&word,code.data+offset,8);
                    vsc::usse::VmovSemantic move{};
                    if (!vsc::usse::decode_vmov_semantic(word,&move) ||
                        move.dst.bank!=vsc::usse::RegisterBank::Output) continue;
                    point_write |= move.dst.num==5 && move.dest_mask==1;
                    clip_write |= move.dst.num==5 && move.dest_mask==2;
                    outputs_in_bounds = outputs_in_bounds && move.dst.num<6;
                }
                ok=view.valid() && view.sdk_version()==0x0165 && view.flags()==0x00090002 &&
                    view.primary_register_count()==12 && view.parameter_count()==8 &&
                    point_write && clip_write && outputs_in_bounds &&
                    view.primary_instruction_count()>0 && varying.size==sizeof(expected_interface) &&
                    std::memcmp(varying.data,expected_interface,sizeof(expected_interface))==0 &&
                    view.parameter(3,clip) && clip.name=="Hclip_planes_eq" && clip.resource_index==0 &&
                    view.parameter(4,point) && point.name=="Mpoint_size" && point.resource_index==52 &&
                    view.parameter(5,modelview) && modelview.name=="Imodelview" && modelview.resource_index==4 &&
                    view.parameter(6,projection) && projection.name=="Jwvp" && projection.resource_index==20 &&
                    view.parameter(7,texmat) && texmat.name=="Ktexmat" && texmat.resource_index==36;
                if (ok) gxp.assign(result.gxp_data,result.gxp_data+result.gxp_size);
            }
            vsc_destroy_result(&request.allocator,&result);
            return ok;
        };
        std::vector<uint8_t> original,changed;
        bool ok=compile_clip(source,original);
        auto mutated=source;
        const std::string from="vClip[i]=dot(modelpos,Hclip_planes_eq[i]);";
        const auto pos=mutated.find(from);
        if (pos==std::string::npos) ok=false;
        else {
            mutated.replace(pos,from.size(),"vClip[i]=dot(modelpos,Hclip_planes_eq[i])*0.75;");
            ok=ok && compile_clip(mutated,changed) && !equal_except_guids(changed.data(),changed.size(),original);
        }
        if (!ok) failures += fail("Cg CLP0 generic path did not preserve interface or source mutation");
    }
    {
        static constexpr const char *source=
            "uniform sampler2D tex:TEXUNIT0; uniform float4 tint;"
            "float4 main(float2 uv:TEXCOORD0,float2 c:SPRITECOORD):COLOR{uv=c;return tex2D(tex,uv)*tint;}";
        VscCompileRequest request{};
        request.source_name="fp-point-sprite.cg";
        request.source=source; request.source_size=std::strlen(source);
        request.entrypoint="main"; request.stage=VSC_STAGE_FRAGMENT;
        VscCompileResult result{};
        bool ok=vsc_compile(&request,&result)==0 && result.gxp_data && result.gxp_size && result.diagnostic_count==0;
        if (ok) {
            vsc::gxp::ProgramView view(result.gxp_data,result.gxp_size);
            const auto varying=view.varyings();
            const auto query=view.sampler_query_info();
            uint16_t query0=0;
            if (query.size>=sizeof(query0)) std::memcpy(&query0,query.data,sizeof(query0));
            ok=view.valid() && result.gxp_size==320 && view.logical_size()==317 &&
                view.minor_version()==5 && view.sdk_version()==0x0300 && view.flags()==0x00180825 &&
                view.secondary_register_count()==4 && view.container_count()==1 &&
                view.compiler_version_raw()==0x00033a90 && varying.size>=22 && varying.data[21]==0xfd &&
                query.size==32 && query0==0x0301;
        }
        if (!ok) failures += fail("Cg SPRITECOORD texture-tint profile did not compile with point-sprite metadata");
        vsc_destroy_result(&request.allocator,&result);
    }
#endif
    return failures;
#endif
}
