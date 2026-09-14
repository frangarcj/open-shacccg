#include "openshacccg/compiler.h"
#include "backend/vita_ir.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint32_t inst(uint16_t wc, uint16_t op) { return (uint32_t(wc)<<16)|op; }

void append(std::vector<uint32_t>& m, uint16_t op, std::initializer_list<uint32_t> args) {
    m.push_back(inst(uint16_t(args.size()+1),op)); m.insert(m.end(),args.begin(),args.end());
}
void append_string_inst(std::vector<uint32_t>& m, uint16_t op, std::initializer_list<uint32_t> prefix,
                        const char *s, std::initializer_list<uint32_t> suffix={}) {
    const size_t n=std::strlen(s)+1, nw=(n+3)/4; std::vector<uint32_t> sw(nw,0); std::memcpy(sw.data(),s,n);
    m.push_back(inst(uint16_t(1+prefix.size()+nw+suffix.size()),op));
    m.insert(m.end(),prefix.begin(),prefix.end()); m.insert(m.end(),sw.begin(),sw.end()); m.insert(m.end(),suffix.begin(),suffix.end());
}
std::vector<uint32_t> header(uint32_t bound) {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,bound,0u};
    append(m,17,{1}); // OpCapability Shader
    return m;
}

std::vector<uint32_t> make_clear_vertex() {
    auto m=header(32); append(m,14,{0,1});
    append_string_inst(m,15,{0,20},"main",{15,16});
    append_string_inst(m,5,{15},"aPosition");
    append(m,71,{15,30,0}); append(m,71,{16,11,0});
    append(m,22,{2,32}); append(m,23,{3,2,2}); append(m,23,{5,2,4});
    append(m,32,{7,1,3}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12}); append(m,43,{2,14,0x3f800000});
    append(m,59,{7,15,1}); append(m,59,{9,16,3});
    append(m,54,{12,20,0,13}); append(m,248,{21});
    append(m,61,{3,22,15}); append(m,80,{5,23,22,14,14}); append(m,62,{16,23});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_clear_fragment() {
    auto m=header(32); append(m,14,{0,1});
    append_string_inst(m,15,{4,20},"main",{16});
    append(m,16,{20,7}); // OriginUpperLeft
    append_string_inst(m,5,{15},"uClearColor");
    append(m,71,{16,30,0});
    append(m,22,{2,32}); append(m,23,{5,2,4});
    append(m,32,{7,2,5}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{7,15,2}); append(m,59,{9,16,3});
    append(m,54,{12,20,0,13}); append(m,248,{21});
    append(m,61,{5,22,15}); append(m,62,{16,22});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_color_fragment() {
    auto m=header(32); append(m,14,{0,1});
    append_string_inst(m,15,{4,20},"main",{15,16});
    append(m,16,{20,7});
    append_string_inst(m,5,{15},"vColor");
    append(m,71,{15,30,0}); append(m,71,{16,30,0});
    append(m,22,{2,32}); append(m,23,{5,2,4});
    append(m,32,{7,1,5}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{7,15,1}); append(m,59,{9,16,3});
    append(m,54,{12,20,0,13}); append(m,248,{21});
    append(m,61,{5,22,15}); append(m,62,{16,22});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_texture_fragment() {
    auto m=header(40); append(m,14,{0,1});
    append_string_inst(m,15,{4,30},"main",{15,17});
    append(m,16,{30,7});
    append_string_inst(m,5,{15},"vTexcoord"); append_string_inst(m,5,{16},"tex");
    append(m,71,{15,30,0}); append(m,71,{17,30,0});
    append(m,22,{2,32}); append(m,23,{3,2,2}); append(m,23,{5,2,4});
    append(m,25,{6,2,1,0,0,0,1,0}); // 2D sampled image
    append(m,27,{7,6});
    append(m,32,{8,0,7}); append(m,32,{9,1,3}); append(m,32,{10,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{9,15,1}); append(m,59,{8,16,0}); append(m,59,{10,17,3});
    append(m,54,{12,30,0,13}); append(m,248,{31});
    append(m,61,{7,32,16}); append(m,61,{3,33,15}); append(m,87,{5,34,32,33}); append(m,62,{17,34});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_texture_tint_fragment() {
    auto m=header(52); append(m,14,{0,1});
    append_string_inst(m,15,{4,40},"main",{15,18});
    append(m,16,{40,7});
    append_string_inst(m,5,{15},"vTexcoord"); append_string_inst(m,5,{16},"tex"); append_string_inst(m,5,{17},"uTintColor");
    append(m,71,{15,30,0}); append(m,71,{18,30,0});
    append(m,22,{2,32}); append(m,23,{3,2,2}); append(m,23,{5,2,4});
    append(m,25,{6,2,1,0,0,0,1,0}); append(m,27,{7,6});
    append(m,32,{8,0,7}); append(m,32,{9,1,3}); append(m,32,{10,2,5}); append(m,32,{11,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{9,15,1}); append(m,59,{8,16,0}); append(m,59,{10,17,2}); append(m,59,{11,18,3});
    append(m,54,{12,40,0,13}); append(m,248,{41});
    append(m,61,{7,42,16}); append(m,61,{3,43,15}); append(m,87,{5,44,42,43});
    append(m,61,{5,45,17}); append(m,133,{5,46,44,45}); append(m,62,{18,46});
    append(m,253,{}); append(m,56,{}); return m;
}



std::vector<uint32_t> make_sub_neg_fragment() {
    // color = vColor - (-uBias)
    auto m=header(64); append(m,14,{0,1});
    append_string_inst(m,15,{4,50},"main",{15,17});
    append(m,16,{50,7});
    append_string_inst(m,5,{15},"vColor"); append_string_inst(m,5,{16},"uBias");
    append(m,71,{15,30,0}); append(m,71,{17,30,0});
    append(m,22,{2,32}); append(m,23,{5,2,4});
    append(m,32,{7,1,5}); append(m,32,{8,2,5}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{7,15,1}); append(m,59,{8,16,2}); append(m,59,{9,17,3});
    append(m,54,{12,50,0,13}); append(m,248,{51});
    append(m,61,{5,52,15}); append(m,61,{5,53,16});
    append(m,127,{5,54,53}); append(m,131,{5,55,52,54}); append(m,62,{17,55});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_dot_fragment() {
    // color = dot(vColor, uWeights).xxxx
    auto m=header(64); append(m,14,{0,1});
    append_string_inst(m,15,{4,50},"main",{15,17});
    append(m,16,{50,7});
    append_string_inst(m,5,{15},"vColor"); append_string_inst(m,5,{16},"uWeights");
    append(m,71,{15,30,0}); append(m,71,{17,30,0});
    append(m,22,{2,32}); append(m,23,{5,2,4});
    append(m,32,{7,1,5}); append(m,32,{8,2,5}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{7,15,1}); append(m,59,{8,16,2}); append(m,59,{9,17,3});
    append(m,54,{12,50,0,13}); append(m,248,{51});
    append(m,61,{5,52,15}); append(m,61,{5,53,16}); append(m,148,{2,54,52,53});
    append(m,80,{5,55,54,54,54,54}); append(m,62,{17,55});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_generic_arithmetic_fragment() {
    // color = vColor * uScale + uBias
    auto m=header(64); append(m,14,{0,1});
    append_string_inst(m,15,{4,50},"main",{15,18});
    append(m,16,{50,7});
    append_string_inst(m,5,{15},"vColor"); append_string_inst(m,5,{16},"uScale"); append_string_inst(m,5,{17},"uBias");
    append(m,71,{15,30,0}); append(m,71,{18,30,0});
    append(m,22,{2,32}); append(m,23,{5,2,4});
    append(m,32,{7,1,5}); append(m,32,{8,2,5}); append(m,32,{9,3,5});
    append(m,19,{12}); append(m,33,{13,12});
    append(m,59,{7,15,1}); append(m,59,{8,16,2}); append(m,59,{8,17,2}); append(m,59,{9,18,3});
    append(m,54,{12,50,0,13}); append(m,248,{51});
    append(m,61,{5,52,15}); append(m,61,{5,53,16}); append(m,61,{5,54,17});
    append(m,133,{5,55,52,53}); append(m,129,{5,56,55,54}); append(m,62,{18,56});
    append(m,253,{}); append(m,56,{}); return m;
}

std::vector<uint32_t> make_matrix_vertex(bool color) {
    auto m=header(48); append(m,14,{0,1});
    append_string_inst(m,15,{0,30},"main",{20,21,22,23});
    append_string_inst(m,5,{20},"aPosition"); append_string_inst(m,5,{21},color?"aColor":"aTexcoord"); append_string_inst(m,5,{24},"wvp");
    append(m,71,{20,30,0}); append(m,71,{21,30,1}); append(m,71,{22,11,0}); append(m,71,{23,30,0});
    append(m,22,{2,32}); append(m,23,{3,2,2}); append(m,23,{4,2,3}); append(m,23,{5,2,4}); append(m,24,{6,5,4});
    append(m,32,{7,1,4}); append(m,32,{8,1,color?5u:3u}); append(m,32,{9,3,5}); append(m,32,{10,3,color?5u:3u}); append(m,32,{11,2,6});
    append(m,19,{12}); append(m,33,{13,12}); append(m,43,{2,14,0x3f800000});
    append(m,59,{7,20,1}); append(m,59,{8,21,1}); append(m,59,{9,22,3}); append(m,59,{10,23,3}); append(m,59,{11,24,2});
    append(m,54,{12,30,0,13}); append(m,248,{31});
    append(m,61,{6,32,24}); append(m,61,{4,33,20}); append(m,80,{5,34,33,14}); append(m,145,{5,35,32,34}); append(m,62,{22,35});
    append(m,61,{color?5u:3u,36,21}); append(m,62,{23,36}); append(m,253,{}); append(m,56,{}); return m;
}

int compare_to_ir(const std::vector<uint32_t>& spv, const vsc::backend::VertexIr& ir, const char *label) {
    VscSpirvRequest req{}; req.words=spv.data(); req.word_count=spv.size(); req.entrypoint="main"; req.stage=VSC_STAGE_VERTEX;
    VscCompileResult out{}; const int rc=vsc_compile_spirv(&req,&out);
    vsc::backend::IrCompileResult expected; const bool irok=vsc::backend::compile_vertex_ir(ir,expected);
    int fail=0;
    if (rc!=0 || !irok || !out.gxp_data || out.diagnostic_count!=0) {
        std::printf("test_spirv: %s did not compile",label);
        if (out.diagnostic_count && out.diagnostics && out.diagnostics[0].message)
            std::printf(": %s", out.diagnostics[0].message);
        std::printf("\n"); fail=1;
    }
    else if (out.gxp_size!=expected.gxp.size() || std::memcmp(out.gxp_data,expected.gxp.data(),expected.gxp.size())!=0) { std::printf("test_spirv: %s SPIR-V lowering differs from direct IR\n",label); fail=1; }
    vsc_destroy_result(&req.allocator,&out); return fail;
}

int compare_fragment_to_ir(const std::vector<uint32_t>& spv, const vsc::backend::FragmentIr& ir, const char *label) {
    VscSpirvRequest req{}; req.words=spv.data(); req.word_count=spv.size(); req.entrypoint="main"; req.stage=VSC_STAGE_FRAGMENT;
    VscCompileResult out{}; const int rc=vsc_compile_spirv(&req,&out);
    vsc::backend::IrCompileResult expected; const bool irok=vsc::backend::compile_fragment_ir(ir,expected);
    int fail=0;
    if (rc!=0 || !irok || !out.gxp_data || out.diagnostic_count!=0) {
        std::printf("test_spirv: %s did not compile",label);
        if (out.diagnostic_count && out.diagnostics && out.diagnostics[0].message)
            std::printf(": %s", out.diagnostics[0].message);
        std::printf("\n"); fail=1;
    }
    else if (out.gxp_size!=expected.gxp.size() || std::memcmp(out.gxp_data,expected.gxp.data(),expected.gxp.size())!=0) { std::printf("test_spirv: %s SPIR-V lowering differs from direct fragment IR\n",label); fail=1; }
    vsc_destroy_result(&req.allocator,&out); return fail;
}
}

int test_spirv() {
    int fail=0;
    {
        auto spv=make_clear_fragment(); vsc::backend::FragmentIr ir; ir.uniforms={{"uClearColor",0}};
        fail += compare_fragment_to_ir(spv,ir,"clear_f");
    }
    {
        auto spv=make_color_fragment(); vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::VaryingColor;
        fail += compare_fragment_to_ir(spv,ir,"color_f");
    }
    {
        auto spv=make_texture_fragment(); vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::Texture2D; ir.samplers={{"tex",0}};
        fail += compare_fragment_to_ir(spv,ir,"texture_f");
    }
    {
        auto spv=make_texture_tint_fragment(); vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::TextureTint2D; ir.uniforms={{"uTintColor",0}}; ir.samplers={{"tex",0}};
        fail += compare_fragment_to_ir(spv,ir,"texture_tint_f");
    }
    {
        auto spv=make_generic_arithmetic_fragment();
        vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::Arithmetic;
        ir.uniforms={{"uScale",0},{"uBias",4}};
        ir.expressions={
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Mul,0,1,4},
            {vsc::backend::FragmentExprKind::Uniform,1,0,4},
            {vsc::backend::FragmentExprKind::Add,2,3,4},
        };
        ir.root_expression=4;
        fail += compare_fragment_to_ir(spv,ir,"generic_arithmetic_f");
    }
    {
        auto spv=make_sub_neg_fragment();
        vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::Arithmetic;
        ir.uniforms={{"uBias",0}};
#if defined(OPENSHACCG_ENABLE_SPIRV_TOOLS)
        // -O canonicalizes vColor - (-uBias) to vColor + uBias.
        ir.expressions={
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Add,0,1,4},
        };
        ir.root_expression=2;
#else
        ir.expressions={
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Neg,1,0,4},
            {vsc::backend::FragmentExprKind::Sub,0,2,4},
        };
        ir.root_expression=3;
#endif
        fail += compare_fragment_to_ir(spv,ir,"generic_sub_neg_f");
    }
    {
        auto spv=make_dot_fragment();
        vsc::backend::FragmentIr ir; ir.op=vsc::backend::FragmentOpKind::Arithmetic;
        ir.uniforms={{"uWeights",0}};
        ir.expressions={
            {vsc::backend::FragmentExprKind::Varying,0,0,4},
            {vsc::backend::FragmentExprKind::Uniform,0,0,4},
            {vsc::backend::FragmentExprKind::Dot,0,1,1},
            {vsc::backend::FragmentExprKind::Splat,2,0,4},
        };
        ir.root_expression=3;
        fail += compare_fragment_to_ir(spv,ir,"generic_dot_f");
    }
    {
        auto spv=make_clear_vertex(); vsc::backend::VertexIr ir; ir.attributes={{"aPosition",2,0}}; ir.ops={{vsc::backend::IrOpKind::ConstructPosition,0,0}};
        fail += compare_to_ir(spv,ir,"clear_v");
    }
    {
        auto spv=make_matrix_vertex(false); vsc::backend::VertexIr ir; ir.attributes={{"aPosition",3,0},{"aTexcoord",2,4}}; ir.matrices={{"wvp",0}}; ir.ops={{vsc::backend::IrOpKind::TransformPosition,0,0},{vsc::backend::IrOpKind::CopyVarying,1,0,vsc::backend::IrVaryingSemantic::TexCoord}};
        fail += compare_to_ir(spv,ir,"texture_v");
    }
    {
        auto spv=make_matrix_vertex(true); vsc::backend::VertexIr ir; ir.attributes={{"aPosition",3,0},{"aColor",4,4}}; ir.matrices={{"wvp",0}}; ir.ops={{vsc::backend::IrOpKind::TransformPosition,0,0},{vsc::backend::IrOpKind::CopyVarying,1,0,vsc::backend::IrVaryingSemantic::Color}};
        fail += compare_to_ir(spv,ir,"color_v");
    }

    // Structurally valid but semantically empty vertex module reaches the new
    // fail-closed subset diagnostic rather than the old backend placeholder.
    const uint32_t minimal[] = {0x07230203u,0x00010000u,0u,5u,0u, inst(2,17),1u, inst(3,14),0u,1u, inst(5,15),0u,1u,0x6e69616du,0u, inst(2,19),2u, inst(3,33),3u,2u, inst(5,54),2u,1u,0u,3u, inst(2,248),4u, inst(1,253), inst(1,56)};
    VscSpirvRequest req{}; req.words=minimal; req.word_count=sizeof(minimal)/4; req.entrypoint="main"; req.stage=VSC_STAGE_VERTEX; VscCompileResult out{};
    int rc=vsc_compile_spirv(&req,&out); if(rc!=1 || !out.diagnostics || out.diagnostics[0].code!=0x2210) ++fail; vsc_destroy_result(&req.allocator,&out);
    req.stage=VSC_STAGE_FRAGMENT; rc=vsc_compile_spirv(&req,&out); if(rc!=1 || !out.diagnostics || out.diagnostics[0].code!=0x2201) ++fail; vsc_destroy_result(&req.allocator,&out);
    uint32_t bad[5]={0,0,0,1,0}; req.words=bad; req.word_count=5; req.stage=VSC_STAGE_VERTEX; rc=vsc_compile_spirv(&req,&out); if(rc!=1 || !out.diagnostics || out.diagnostics[0].code!=0x2102) ++fail; vsc_destroy_result(&req.allocator,&out);
    if(fail) std::printf("test_spirv: %d failures\n",fail); return fail;
}
