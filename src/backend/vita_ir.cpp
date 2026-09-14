#include "backend/vita_ir.hpp"

#include "backend/program_builder.hpp"
#include "gxp/gxp_writer.hpp"

#include <array>
#include <functional>
#include <vector>

namespace vsc::backend {
namespace {

using usse::RegisterBank;
using usse::RegisterRef;
using usse::SwizzleChannel;

static usse::Swizzle4 splat(SwizzleChannel c) {
    return {{c, c, c, c}};
}

static usse::Swizzle4 identity() {
    return {{SwizzleChannel::X, SwizzleChannel::Y,
             SwizzleChannel::Z, SwizzleChannel::W}};
}

static bool valid_attribute(const IrAttribute &a) {
    return !a.name.empty() && a.components >= 1 && a.components <= 4 &&
           (a.resource_index % 2u) == 0u && a.resource_index <= 126u;
}

} // namespace

bool compile_vertex_ir(const VertexIr &ir, IrCompileResult &out) {
    out = {};
    if (ir.attributes.empty() || ir.ops.empty()) { out.error="empty vertex IR"; return false; }
    for (const auto &a: ir.attributes) if (!valid_attribute(a)) { out.error="invalid attribute description"; return false; }

    const IrOp *transform=nullptr, *construct=nullptr, *varying=nullptr;
    for (const auto &op: ir.ops) {
        if (op.kind==IrOpKind::TransformPosition) { if (transform) { out.error="multiple transforms unsupported"; return false; } transform=&op; }
        else if (op.kind==IrOpKind::ConstructPosition) { if (construct) { out.error="multiple constructed positions unsupported"; return false; } construct=&op; }
        else if (op.kind==IrOpKind::CopyVarying) { if (varying) { out.error="multiple varyings unsupported"; return false; } varying=&op; }
    }
    if (!!transform == !!construct) { out.error="vertex IR requires exactly one position producer"; return false; }
    const IrOp *posop = transform ? transform : construct;
    if (posop->attribute >= ir.attributes.size() || (varying && varying->attribute >= ir.attributes.size())) { out.error="attribute index out of range"; return false; }
    const IrAttribute &position=ir.attributes[posop->attribute];
    if (position.resource_index != 0) { out.error="position attribute must currently start at resource 0"; return false; }

    ProgramBuilder code;
    if (!code.phase()) { out.error="failed to encode PHAS"; return false; }

    uint8_t interface_block[32]{};
    uint8_t fragment_extension[8]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.binary_guid=ir.binary_guid; image.source_guid=ir.source_guid;
    image.program_flags=0x00010000;
    image.data_buffer_count=2;
    image.primary_phase_count=1;

    if (construct) {
        if (ir.attributes.size()!=1 || !ir.matrices.empty() || varying || ir.ops.size()!=1 || position.components!=2) {
            out.error="constructed-position path currently supports only float2 -> float4(xy,1,1)"; return false;
        }
        if (!code.nop()) { out.error="failed to encode NOP"; return false; }
        usse::V32NmadSemantic mul{};
        mul.op=usse::VectorOp::Mul; mul.dst={RegisterBank::Temp,60};
        mul.src1={RegisterBank::PrimaryAttribute,0}; mul.src2={RegisterBank::Special,1};
        mul.dest_mask=0xF; mul.src1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::One,SwizzleChannel::One}};
        mul.src2_swizzle=splat(SwizzleChannel::Y); mul.skip_invalid=true; mul.no_schedule=true;
        if (!code.instruction(mul)) { out.error="failed to construct homogeneous position"; return false; }
        usse::VmovSemantic m{}; m.src={RegisterBank::Temp,60}; m.data_type=usse::DataType::F32; m.dest_mask=3; m.skip_invalid=true;
        m.dst={RegisterBank::Output,0}; m.swizzle=4;
        if (!code.instruction(m)) { out.error="failed first position move"; return false; }
        m.dst={RegisterBank::Output,1}; m.swizzle=11;
        if (!code.instruction(m)) { out.error="failed second position move"; return false; }
        if (!code.emit()) { out.error="failed to encode EMIT"; return false; }

        interface_block[0]=0x03; interface_block[16]=0x00; interface_block[17]=0x10; interface_block[18]=0x00; interface_block[19]=0x04;
        containers.push_back({19,0,0,2});
        parameters.push_back({position.name.c_str(),0,0,4,0,0,0,1,position.resource_index});
        image.buffer_flags=0; image.primary_register_count=4; image.secondary_register_count=2; image.compiler_version_raw=0;
        image.default_uniform_buffer_count=0;
    } else {
        if (ir.attributes.size()!=2 || ir.matrices.size()!=1 || !varying || ir.ops.size()!=2 || transform->uniform!=0 || transform->attribute==varying->attribute) {
            out.error="matrix path requires 2 attributes, one mat4, transform + varying"; return false;
        }
        if (ir.matrices[0].name.empty() || ir.matrices[0].resource_index!=0) { out.error="mat4 resource index must be 0"; return false; }
        const IrAttribute &pass=ir.attributes[varying->attribute];
        if (pass.resource_index!=4 || position.components!=3 || (pass.components!=2 && pass.components!=4)) { out.error="unsupported matrix-path attribute allocation"; return false; }

        usse::VmovSemantic move{}; move.dst={RegisterBank::Output,2}; move.src={RegisterBank::PrimaryAttribute,2};
        move.data_type=usse::DataType::F32; move.dest_mask=3; move.swizzle=4; move.repeat_count=pass.components==4 ? 1 : 0; move.skip_invalid=true; move.no_schedule=true;
        if (!code.instruction(move)) { out.error="failed varying copy"; return false; }
        usse::VpckSemantic p0{}; p0.dst={RegisterBank::Temp,124}; p0.src1={RegisterBank::PrimaryAttribute,0}; p0.src2={RegisterBank::PrimaryAttribute,1};
        p0.src_format=usse::PackFormat::F32; p0.dst_format=usse::PackFormat::F32; p0.dest_mask=7; p0.skip_invalid=true; p0.no_schedule=true;
        if (!code.instruction(p0)) { out.error="failed position staging"; return false; }
        usse::VpckSemantic p1{}; p1.dst={RegisterBank::Temp,125}; p1.src1={RegisterBank::SecondaryAttribute,6}; p1.src2={RegisterBank::SecondaryAttribute,7};
        p1.src_format=usse::PackFormat::F32; p1.dst_format=usse::PackFormat::F32; p1.dest_mask=15; p1.skip_invalid=true; p1.no_schedule=true;
        if (!code.instruction(p1)) { out.error="failed homogeneous staging"; return false; }
        const std::array<SwizzleChannel,4> lanes={SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W};
        for (size_t i=0;i<4;i++) {
            usse::VmadSemantic mad{}; mad.gpi0=0; mad.gpi1=1; mad.vec4=true; mad.repeat_mode=usse::RepeatMode::Slmsi; mad.skip_invalid=true;
            mad.src1_swizzle=identity(); mad.gpi1_swizzle=identity(); mad.gpi0_swizzle=splat(lanes[i==3?2:i]);
            if (i<2) { mad.dst={RegisterBank::Temp,61}; mad.src1={RegisterBank::SecondaryAttribute,uint8_t(i*2)}; mad.write_mask=15; mad.no_schedule=true; }
            else { mad.dst={RegisterBank::Output,uint8_t(i-2)}; mad.src1={RegisterBank::SecondaryAttribute,uint8_t(i+2)}; mad.write_mask=3; if(i==3) mad.gpi1_swizzle={{SwizzleChannel::Z,SwizzleChannel::W,SwizzleChannel::Z,SwizzleChannel::W}}; }
            if (!code.instruction(mad)) { out.error="failed matrix multiply"; return false; }
        }
        if (!code.emit()) { out.error="failed EMIT"; return false; }
        if (varying->semantic==IrVaryingSemantic::Color) {
            interface_block[0]=0xf7; interface_block[16]=0x00; interface_block[17]=0x18; interface_block[18]=0x00; interface_block[19]=0x08;
        } else {
            interface_block[0]=0x37; interface_block[16]=0x00; interface_block[17]=0x10; interface_block[18]=0x00; interface_block[19]=0x06; interface_block[20]=0x01;
        }
        containers={{14,0,0,16},{19,0,16,2}};
        parameters.push_back({position.name.c_str(),0,0,4,0,0,0,1,position.resource_index});
        parameters.push_back({pass.name.c_str(),0,0,4,0,0,0,1,pass.resource_index});
        parameters.push_back({ir.matrices[0].name.c_str(),1,0,4,14,0,0,4,0});
        image.buffer_flags=0x10000000; image.primary_register_count=8; image.secondary_register_count=18; image.default_uniform_buffer_count=16; image.compiler_version_raw=16;
    }

    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=code.words().data(); image.primary_instruction_count=code.words().size();
    image.containers=containers.data(); image.container_count=containers.size(); image.parameters=parameters.data(); image.parameter_count=parameters.size();
    const size_t needed=gxp::required_size(image); if(!needed){out.error="GXP writer rejected IR";return false;}
    out.gxp.resize(needed); if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())){out.gxp.clear();out.error="GXP writer failed";return false;}
    return true;
}

} // namespace vsc::backend

namespace vsc::backend {

bool compile_fragment_ir(const FragmentIr &ir, IrCompileResult &out) {
    out = {};

    ProgramBuilder primary;
    ProgramBuilder secondary;
    if (!primary.phase()) { out.error = "failed to encode fragment PHAS"; return false; }

    uint8_t interface_block[32]{};
    uint8_t fragment_extension[8]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;

    gxp::ProgramImage image{};
    image.type = gxp::ProgramType::Fragment;
    image.binary_guid = ir.binary_guid;
    image.source_guid = ir.source_guid;
    image.primary_phase_count = 1;
    image.data_buffer_count = 2;
    image.interface_block = interface_block;
    image.interface_block_size = sizeof(interface_block);

    if (ir.op == FragmentOpKind::UniformColor) {
        if (ir.uniforms.size() != 1 || !ir.samplers.empty() || ir.uniforms[0].name.empty() || ir.uniforms[0].resource_index != 0) {
            out.error = "uniform-color fragment IR requires one float4 uniform at resource 0";
            return false;
        }
        usse::VmovSemantic move{};
        move.dst = {usse::RegisterBank::PrimaryAttribute, 0};
        move.src = {usse::RegisterBank::SecondaryAttribute, 0};
        move.data_type = usse::DataType::F16;
        move.dest_mask = 0x5;
        move.swizzle = 4;
        move.skip_invalid = true;
        if (!primary.instruction(move)) { out.error = "failed to encode fragment color move"; return false; }

        usse::VpckSemantic pack{};
        pack.dst = {usse::RegisterBank::PrimaryAttribute, 0};
        pack.src1 = {usse::RegisterBank::PrimaryAttribute, 0};
        pack.src2 = {usse::RegisterBank::PrimaryAttribute, 1};
        pack.src_format = usse::PackFormat::F32;
        pack.dst_format = usse::PackFormat::F16;
        pack.dest_mask = 0xF;
        pack.components[0]=0; pack.components[1]=1; pack.components[2]=2; pack.components[3]=3;
        pack.skip_invalid = true;
        pack.no_schedule = false;
        pack.end = true;
        if (!secondary.instruction(pack)) { out.error = "failed to encode fragment secondary VPCK"; return false; }

        interface_block[10] = 1; interface_block[11] = 4; interface_block[16] = 4;
        containers = {{14,0,0,4},{19,0,4,2}};
        parameters.push_back({ir.uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        image.program_flags = 0;
        image.buffer_flags = 0x10000000;
        image.primary_register_count = 2;
        image.secondary_register_count = 6;
        image.default_uniform_buffer_count = 4;
        image.compiler_version_raw = 4;
    } else if (ir.op == FragmentOpKind::VaryingColor) {
        if (!ir.uniforms.empty() || !ir.samplers.empty()) { out.error="varying-color fragment IR takes no uniforms or samplers"; return false; }
        usse::VpckSemantic pack{};
        pack.dst={usse::RegisterBank::PrimaryAttribute,0};
        pack.src1={usse::RegisterBank::PrimaryAttribute,0};
        pack.src2={usse::RegisterBank::PrimaryAttribute,1};
        pack.src_format=usse::PackFormat::F32; pack.dst_format=usse::PackFormat::F16;
        pack.dest_mask=0xF; pack.skip_invalid=true; pack.no_schedule=false;
        pack.components[0]=0; pack.components[1]=1; pack.components[2]=2; pack.components[3]=3;
        if(!primary.instruction(pack)){out.error="failed to encode varying color VPCK";return false;}
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[16]=4;
        interface_block[20]=0x0f; interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
        interface_block[28]=0x30;
        containers={{19,0,0,2}};
        image.program_flags=0x1000;
        image.primary_register_count=4; image.secondary_register_count=2;
        image.compiler_version_raw=0;
    } else if (ir.op == FragmentOpKind::Arithmetic) {
        if (ir.expressions.empty() || ir.root_expression >= ir.expressions.size() || !ir.samplers.empty()) {
            out.error="arithmetic fragment IR requires a valid expression DAG and no samplers"; return false;
        }
        // Initial generic arithmetic ABI: one interpolated float4 at PA0 and
        // zero or more float4 uniforms in SA registers. Existing known-good
        // shape-specific paths remain separate above/below this branch.
        struct Allocated { bool ready=false; usse::RegisterRef reg{}; uint8_t components=4; };
        std::vector<Allocated> allocated(ir.expressions.size());

        // Count DAG uses up-front so TEMP registers can be recycled at the
        // exact point where the last consumer has been emitted.
        std::vector<uint32_t> uses(ir.expressions.size(),0);
        for (const auto &n : ir.expressions) {
            switch (n.kind) {
            case FragmentExprKind::Mul:
            case FragmentExprKind::Add:
            case FragmentExprKind::Sub:
            case FragmentExprKind::Min:
            case FragmentExprKind::Max:
            case FragmentExprKind::Dot:
                if(n.a<uses.size()) ++uses[n.a];
                if(n.b<uses.size()) ++uses[n.b];
                break;
            case FragmentExprKind::Neg:
            case FragmentExprKind::Abs:
            case FragmentExprKind::Splat:
                if(n.a<uses.size()) ++uses[n.a];
                break;
            default: break;
            }
        }
        ++uses[ir.root_expression]; // pin the root through the final VPCK.

        // TEMP F32 vectors consume a pair of raw registers. Allocate from the
        // high end, but recycle dead pairs deterministically.
        std::vector<uint8_t> free_temps;
        for (int r=58;r>=4;r-=2) free_temps.push_back(static_cast<uint8_t>(r));
        auto alloc_temp=[&](usse::RegisterRef &r)->bool {
            if(free_temps.empty()) { out.error="arithmetic temporary register budget exhausted"; return false; }
            r={usse::RegisterBank::Temp,free_temps.back()}; free_temps.pop_back(); return true;
        };
        auto release_node=[&](uint32_t idx) {
            if(idx>=uses.size() || uses[idx]==0) return;
            if(--uses[idx]==0 && allocated[idx].ready && allocated[idx].reg.bank==usse::RegisterBank::Temp)
                free_temps.push_back(allocated[idx].reg.num);
        };
        auto identity=[](){ usse::Swizzle4 sw{}; return sw; };
        std::function<bool(uint32_t)> lower = [&](uint32_t idx)->bool {
            if (idx>=ir.expressions.size()) { out.error="arithmetic expression index out of range"; return false; }
            if (allocated[idx].ready) return true;
            const auto &n=ir.expressions[idx];
            if (n.kind==FragmentExprKind::Varying) {
                if (n.a!=0 || n.components!=4) { out.error="initial arithmetic path supports only float4 varying 0"; return false; }
                allocated[idx]={true,{usse::RegisterBank::PrimaryAttribute,0},4}; return true;
            }
            if (n.kind==FragmentExprKind::Uniform) {
                if (n.a>=ir.uniforms.size() || n.components!=4) { out.error="arithmetic uniform leaf is invalid"; return false; }
                allocated[idx]={true,{usse::RegisterBank::SecondaryAttribute,static_cast<uint8_t>(n.a*2)},4}; return true;
            }
            if (n.kind==FragmentExprKind::Splat) {
                if(!lower(n.a)) return false;
                if(allocated[n.a].components!=1) { out.error="splat source must be scalar"; return false; }
                usse::RegisterRef dst{}; if(!alloc_temp(dst)) return false;
                usse::VmovSemantic move{}; move.dst=dst; move.src=allocated[n.a].reg;
                move.data_type=usse::DataType::F32; move.dest_mask=0xF; move.swizzle=0;
                move.skip_invalid=true; move.no_schedule=false;
                if(!primary.instruction(move)){out.error="failed to encode arithmetic scalar splat";return false;}
                allocated[idx]={true,dst,4}; release_node(n.a); return true;
            }
            if (n.kind==FragmentExprKind::Neg || n.kind==FragmentExprKind::Abs) {
                if(!lower(n.a)) return false;
                if(allocated[n.a].components!=4) { out.error="unary arithmetic source must be float4"; return false; }
                usse::RegisterRef dst{}; if(!alloc_temp(dst)) return false;
                usse::V32NmadSemantic op{}; op.op=usse::VectorOp::Add; op.dst=dst;
                op.src1=allocated[n.a].reg; op.src2={usse::RegisterBank::Immediate,0};
                op.dest_mask=0xF; op.src1_swizzle=identity(); op.src2_swizzle=identity();
                op.src1_negative=(n.kind==FragmentExprKind::Neg); op.src1_absolute=(n.kind==FragmentExprKind::Abs);
                op.skip_invalid=true; op.no_schedule=false;
                if(!primary.instruction(op)){out.error="failed to encode arithmetic unary operation";return false;}
                allocated[idx]={true,dst,4}; release_node(n.a); return true;
            }
            if (!lower(n.a) || !lower(n.b)) return false;
            usse::RegisterRef dst{}; if(!alloc_temp(dst)) return false;
            usse::V32NmadSemantic op{};
            op.dst=dst; op.src1=allocated[n.a].reg; op.src2=allocated[n.b].reg;
            op.dest_mask = n.kind==FragmentExprKind::Dot ? 0x1 : 0xF;
            op.src1_swizzle=identity(); op.src2_swizzle=identity(); op.skip_invalid=true; op.no_schedule=false;
            switch(n.kind) {
            case FragmentExprKind::Mul: op.op=usse::VectorOp::Mul; break;
            case FragmentExprKind::Add: op.op=usse::VectorOp::Add; break;
            case FragmentExprKind::Sub:
                op.op=usse::VectorOp::Add; op.src1=allocated[n.b].reg; op.src2=allocated[n.a].reg; op.src1_negative=true; break;
            case FragmentExprKind::Min: op.op=usse::VectorOp::Min; break;
            case FragmentExprKind::Max: op.op=usse::VectorOp::Max; break;
            case FragmentExprKind::Dot: op.op=usse::VectorOp::Dot; break;
            default: out.error="unsupported arithmetic expression node"; return false;
            }
            if(!primary.instruction(op)){out.error="failed to encode generic arithmetic operation";return false;}
            allocated[idx]={true,dst,static_cast<uint8_t>(n.kind==FragmentExprKind::Dot?1:4)};
            release_node(n.a); release_node(n.b); return true;
        };
        if(!primary.nop()) { out.error="failed to encode arithmetic scheduling NOP"; return false; }
        if(!lower(ir.root_expression)) return false;
        if(allocated[ir.root_expression].components!=4) { out.error="fragment arithmetic root must currently produce float4"; return false; }
        usse::VpckSemantic outpack{};
        outpack.dst={usse::RegisterBank::PrimaryAttribute,0}; outpack.src1=allocated[ir.root_expression].reg;
        outpack.src2={usse::RegisterBank::Immediate,0}; outpack.src_format=usse::PackFormat::F32; outpack.dst_format=usse::PackFormat::F16;
        outpack.dest_mask=0xF; outpack.skip_invalid=true; outpack.no_schedule=false;
        if(!primary.instruction(outpack)){out.error="failed generic arithmetic output VPCK";return false;}

        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[16]=4;
        interface_block[20]=0x0f; interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
        interface_block[28]=0x30;
        if(!ir.uniforms.empty()) { interface_block[28]=0xb0; image.buffer_flags=0x10000000; image.default_uniform_buffer_count=4*ir.uniforms.size(); }
        containers.push_back({19,0,0,2});
        if(!ir.uniforms.empty()) containers.insert(containers.begin(),{14,0,0,static_cast<uint16_t>(4*ir.uniforms.size())});
        for(size_t i=0;i<ir.uniforms.size();++i) {
            if(ir.uniforms[i].name.empty()) { out.error="arithmetic uniform name cannot be empty"; return false; }
            parameters.push_back({ir.uniforms[i].name.c_str(),1,0,4,14,0,0,1,static_cast<uint32_t>(i*4)});
        }
        image.program_flags=0x1000; image.primary_register_count=4;
        image.secondary_register_count=static_cast<uint16_t>(2 + ir.uniforms.size()*2);
        image.compiler_version_raw=ir.uniforms.empty()?0:static_cast<uint32_t>(4*ir.uniforms.size());
    } else if (ir.op == FragmentOpKind::TextureTint2D) {
        if (ir.uniforms.size()!=1 || ir.samplers.size()!=1 || ir.uniforms[0].name.empty() ||
            ir.samplers[0].name.empty() || ir.uniforms[0].resource_index!=0 || ir.samplers[0].resource_index!=0) {
            out.error="texture-tint fragment IR requires one float4 uniform and one sampler2D at resource 0"; return false;
        }
        if (!primary.nop()) { out.error="failed to encode texture-tint NOP"; return false; }
        usse::VpckSemantic stage{};
        stage.dst={usse::RegisterBank::Temp,124};
        stage.src1={usse::RegisterBank::PrimaryAttribute,0};
        stage.src2={usse::RegisterBank::PrimaryAttribute,1};
        stage.src_format=usse::PackFormat::F32; stage.dst_format=usse::PackFormat::F32;
        stage.dest_mask=0xF; stage.skip_invalid=true; stage.no_schedule=true;
        stage.components[0]=0; stage.components[1]=1; stage.components[2]=2; stage.components[3]=3;
        if(!primary.instruction(stage)){out.error="failed texture-tint sample staging VPCK";return false;}
        usse::V32NmadSemantic mul{};
        mul.op=usse::VectorOp::Mul; mul.dst={usse::RegisterBank::Temp,60};
        mul.src1={usse::RegisterBank::SecondaryAttribute,0}; mul.src2={usse::RegisterBank::Temp,60};
        mul.dest_mask=0xF; mul.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
        mul.src2_swizzle=mul.src1_swizzle; mul.skip_invalid=true; mul.no_schedule=false;
        if(!primary.instruction(mul)){out.error="failed texture-tint multiply";return false;}
        usse::VpckSemantic outpack{};
        outpack.dst={usse::RegisterBank::PrimaryAttribute,0};
        outpack.src1={usse::RegisterBank::Temp,60};
        outpack.src2={usse::RegisterBank::Immediate,0};
        outpack.src_format=usse::PackFormat::F32; outpack.dst_format=usse::PackFormat::F16;
        outpack.dest_mask=0xF; outpack.skip_invalid=true; outpack.no_schedule=false;
        outpack.components[0]=0; outpack.components[1]=1; outpack.components[2]=2; outpack.components[3]=3;
        if(!primary.instruction(outpack)){out.error="failed texture-tint output VPCK";return false;}
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=0x00; interface_block[21]=0xf9; interface_block[28]=0xc0;
        fragment_extension[0]=0x30;
        image.fragment_interface_extension=fragment_extension; image.fragment_interface_extension_size=sizeof(fragment_extension);
        containers={{14,0,0,4},{19,0,4,2}};
        parameters.push_back({ir.uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        parameters.push_back({ir.samplers[0].name.c_str(),2,0,4,0,1,0,1,0});
        image.program_flags=0x801; image.buffer_flags=0x10000000; image.texunit_flags[0]=1;
        image.primary_register_count=4; image.secondary_register_count=6;
        image.default_uniform_buffer_count=4; image.compiler_version_raw=4;
    } else if (ir.op == FragmentOpKind::Texture2D) {
        if (!ir.uniforms.empty() || ir.samplers.size()!=1 || ir.samplers[0].name.empty() || ir.samplers[0].resource_index!=0) {
            out.error="texture fragment IR requires one sampler2D at resource 0"; return false;
        }
        // Validated vita2d dependent-sample path: the interpolated TEXCOORD0 and
        // texture unit 0 are described by the fragment interface record. No SMP
        // instruction appears in this public shader; PHAS is the entire primary stream.
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=0x00; interface_block[21]=0xf9;
        interface_block[28]=0x40; interface_block[29]=0x00; interface_block[30]=0x00; interface_block[31]=0x00;
        fragment_extension[0]=0x20;
        image.fragment_interface_extension=fragment_extension;
        image.fragment_interface_extension_size=sizeof(fragment_extension);
        containers={{19,0,0,2}};
        parameters.push_back({ir.samplers[0].name.c_str(),2,0,4,0,2,0,1,0});
        image.program_flags=0x800;
        image.texunit_flags[0]=1;
        image.primary_register_count=2; image.secondary_register_count=2;
        image.compiler_version_raw=0;
    } else {
        out.error="unsupported fragment IR operation"; return false;
    }

    image.secondary_instructions = secondary.words().data();
    image.secondary_instruction_count = secondary.words().size();
    image.primary_instructions = primary.words().data();
    image.primary_instruction_count = primary.words().size();
    image.containers=containers.data(); image.container_count=containers.size();
    image.parameters=parameters.data(); image.parameter_count=parameters.size();

    const size_t needed = gxp::required_size(image);
    if (!needed) { out.error = "GXP writer rejected fragment IR"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error = "GXP writer failed for fragment IR"; return false;
    }
    return true;
}

} // namespace vsc::backend
