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
            view.primary_instruction_count()==39 && view.secondary_instruction_count()==2;
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
    if (!compile("float2 in uv:TEXCOORD0;\nfloat sample_x(){return uv.x;}\nfloat4 main():COLOR0{return float4(sample_x(),0,0,1);}\n", VSC_STAGE_FRAGMENT))
        failures += fail("Cg global input semantic did not normalize into an entry-point parameter");
    if (!compile("float4 out gl_Position:POSITION;\nfloat2 out uv:TEXCOORD0;\nvoid main(float4 p:POSITION){gl_Position=p;uv=p.xy;}\n", VSC_STAGE_VERTEX))
        failures += fail("Cg global output semantics did not normalize into entry-point parameters");
#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    if (!compile_shader_gxp("uniform float3x3 unused_matrix; float4 main(float4 p:POSITION):POSITION{return p;}",
                            "unused-matrix-uniform.cg",VSC_STAGE_VERTEX))
        failures += fail("unused unsupported uniform member type blocked an otherwise valid shader");
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
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-geometrizer-poly.cg");
        if (source.empty() || !compile_geometrizer_poly_vertex(source))
            failures += fail("Geometrizer POLY_VS integration profile did not compile through generic vertex Machine IR");
    }
    {
        const std::string source=read_text(std::string(OPENSHACCG_SOURCE_DIR)+"/oracle_corpus_v2/vp-geometrizer-poly3d.cg");
        if (source.empty() || !compile_geometrizer_poly3d_vertex(source))
            failures += fail("Geometrizer POLY3D_VS integration profile did not compile through generic vertex Machine IR");
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
#endif
    return failures;
#endif
}
