#include "backend/vita_ir.hpp"

#include "backend/machine_ir.hpp"
#include "gxp/gxp_writer.hpp"

#include <functional>
#include <vector>

namespace vsc::backend {
namespace {

static bool valid_attribute(const IrAttribute &a) {
    return !a.name.empty() && a.components >= 1 && a.components <= 4 &&
           (a.resource_index % 2u) == 0u && a.resource_index <= 126u;
}

static bool compile_words(const MachineProgram &program, MachineCompileResult &compiled,
                          IrCompileResult &out, const char *context) {
    if (compile_machine_program(program, compiled)) return true;
    out.error = std::string(context) + ": " + compiled.error;
    return false;
}

} // namespace

bool compile_vertex_construct_position(const IrAttribute &position,
                                       uint32_t binary_guid, uint32_t source_guid,
                                       IrCompileResult &out) {
    out = {};
    if (!valid_attribute(position) || position.resource_index != 0 || position.components != 2) {
        out.error = "constructed-position profile requires float2 position at resource 0";
        return false;
    }

    MachineProgram code;
    const auto position_temp = code.make_value<MachineType::F32>(
        MachineRegisterClass::FloatTemp, 2, MachineRegisterOrder::High);
    const auto input = code.physical(machine_primary(0), MachineType::F32);
    const auto one = code.physical(machine_special(1), MachineType::F32);
    if (position_temp.kind() == MachineOperandKind::None || !code.emit<MachineOpcode::Phase>() ||
        !code.emit<MachineOpcode::Nop>() ||
        !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(0xF, MachineVectorSwizzle::PositionXY11, false, false, false, true, true),
            position_temp, input, one) ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3, 4), code.physical(machine_vertex_output(0), MachineType::F32), position_temp) ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3, 11), code.physical(machine_vertex_output(1), MachineType::F32), position_temp) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error = "failed to build constructed-position machine program";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[0]=0x03; interface_block[16]=0x00; interface_block[17]=0x10;
    interface_block[18]=0x00; interface_block[19]=0x04;
    const gxp::ParameterContainerDesc containers[] = {{19,0,0,2}};
    const gxp::ParameterDesc parameters[] = {
        {position.name.c_str(),0,0,4,0,0,0,1,position.resource_index},
    };

    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00010000;
    image.data_buffer_count=2;
    image.primary_phase_count=1;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers;
    image.container_count=1;
    image.parameters=parameters;
    image.parameter_count=1;
    image.primary_register_count=4;
    image.secondary_register_count=2;
    image.compiler_version_raw=0;

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected constructed-position profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for constructed-position profile"; return false;
    }
    return true;
}

bool compile_vertex_matrix_path(const IrAttribute &position, const IrAttribute &varying,
                                const IrMatrix4Uniform &matrix, IrVaryingSemantic semantic,
                                uint32_t binary_guid, uint32_t source_guid,
                                IrCompileResult &out) {
    out = {};
    if (!valid_attribute(position) || !valid_attribute(varying) ||
        position.resource_index != 0 || varying.resource_index != 4 ||
        position.components != 3 || (varying.components != 2 && varying.components != 4) ||
        matrix.name.empty() || matrix.resource_index != 0) {
        out.error = "matrix vertex profile has unsupported resources";
        return false;
    }

    MachineProgram code;
    const auto gpi0 = code.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto gpi1 = code.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto gpi_pair = code.pair(gpi0, gpi1);
    if (gpi0.kind() == MachineOperandKind::None || gpi1.kind() == MachineOperandKind::None ||
        gpi_pair.kind() == MachineOperandKind::None || !code.emit<MachineOpcode::Phase>() ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3, 4, varying.components==4 ? 1 : 0, true, true),
            code.physical(machine_vertex_output(2), MachineType::F32),
            code.physical(machine_primary(static_cast<uint8_t>(varying.resource_index/2)), MachineType::F32)) ||
        !code.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
            machine_pack_config(7), gpi0,
            code.physical(machine_primary(0), MachineType::F32),
            code.physical(machine_primary(1), MachineType::F32)) ||
        !code.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
            machine_pack_config(15), gpi1,
            code.physical(machine_secondary(6), MachineType::F32),
            code.physical(machine_secondary(7), MachineType::F32))) {
        out.error = "failed to build matrix staging machine program";
        return false;
    }
    for (size_t i=0;i<4;i++) {
        MachineOperand dst;
        uint8_t src_num;
        uint8_t mask;
        bool no_schedule;
        if (i<2) {
            dst=code.make_value<MachineType::F32>(MachineRegisterClass::VmadAccumulator);
            src_num=static_cast<uint8_t>(i*2); mask=15; no_schedule=true;
        } else {
            dst=code.physical(machine_vertex_output(static_cast<uint8_t>(i-2)), MachineType::F32);
            src_num=static_cast<uint8_t>(i+2); mask=3; no_schedule=false;
        }
        if (dst.kind() == MachineOperandKind::None ||
            !code.emit_config<MachineOpcode::Vmad>(static_cast<uint8_t>(i),
                machine_vmad_config(mask, no_schedule), dst,
                code.physical(machine_secondary(src_num), MachineType::F32), gpi_pair)) {
            out.error="failed to build matrix VMAD machine operation";
            return false;
        }
    }
    if (!code.emit<MachineOpcode::Emit>()) {
        out.error="failed to append EMIT";
        return false;
    }

    uint8_t interface_block[32]{};
    if (semantic==IrVaryingSemantic::Color) {
        interface_block[0]=0xf7; interface_block[16]=0x00; interface_block[17]=0x18;
        interface_block[18]=0x00; interface_block[19]=0x08;
    } else {
        interface_block[0]=0x37; interface_block[16]=0x00; interface_block[17]=0x10;
        interface_block[18]=0x00; interface_block[19]=0x06; interface_block[20]=0x01;
    }
    const gxp::ParameterContainerDesc containers[] = {{14,0,0,16},{19,0,16,2}};
    const gxp::ParameterDesc parameters[] = {
        {position.name.c_str(),0,0,4,0,0,0,1,position.resource_index},
        {varying.name.c_str(),0,0,4,0,0,0,1,varying.resource_index},
        {matrix.name.c_str(),1,0,4,14,0,0,4,0},
    };

    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00010000;
    image.buffer_flags=0x10000000;
    image.data_buffer_count=2;
    image.primary_phase_count=1;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=3;
    image.primary_register_count=8;
    image.secondary_register_count=18;
    image.default_uniform_buffer_count=16;
    image.compiler_version_raw=16;

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected matrix vertex profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for matrix vertex profile"; return false;
    }
    return true;
}

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

    MachineProgram code;
    if (!code.emit<MachineOpcode::Phase>()) { out.error="failed to append PHAS"; return false; }

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
        const auto position_temp = code.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp, 2, MachineRegisterOrder::High);
        const auto input = code.physical(machine_primary(0), MachineType::F32);
        const auto one = code.physical(machine_special(1), MachineType::F32);
        if (position_temp.kind() == MachineOperandKind::None ||
            !code.emit<MachineOpcode::Nop>() ||
            !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
                machine_vector_config(0xF, MachineVectorSwizzle::PositionXY11, false, false, false, true, true),
                position_temp, input, one) ||
            !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(3, 4), code.physical(machine_vertex_output(0), MachineType::F32), position_temp) ||
            !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(3, 11), code.physical(machine_vertex_output(1), MachineType::F32), position_temp) ||
            !code.emit<MachineOpcode::Emit>()) {
            out.error="failed to build constructed-position machine program"; return false;
        }

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

        const auto gpi0 = code.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto gpi1 = code.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto gpi_pair = code.pair(gpi0, gpi1);
        if (gpi0.kind() == MachineOperandKind::None || gpi1.kind() == MachineOperandKind::None ||
            gpi_pair.kind() == MachineOperandKind::None ||
            !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(3, 4, pass.components==4 ? 1 : 0, true, true),
                code.physical(machine_vertex_output(2), MachineType::F32),
                code.physical(machine_primary(static_cast<uint8_t>(pass.resource_index/2)), MachineType::F32)) ||
            !code.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
                machine_pack_config(7), gpi0,
                code.physical(machine_primary(0), MachineType::F32),
                code.physical(machine_primary(1), MachineType::F32)) ||
            !code.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
                machine_pack_config(15), gpi1,
                code.physical(machine_secondary(6), MachineType::F32),
                code.physical(machine_secondary(7), MachineType::F32))) {
            out.error="failed to build matrix staging machine program"; return false;
        }
        for (size_t i=0;i<4;i++) {
            MachineOperand dst;
            uint8_t src_num;
            uint8_t mask;
            bool no_schedule;
            if (i<2) {
                dst=code.make_value<MachineType::F32>(MachineRegisterClass::VmadAccumulator);
                src_num=static_cast<uint8_t>(i*2); mask=15; no_schedule=true;
            } else {
                dst=code.physical(machine_vertex_output(static_cast<uint8_t>(i-2)), MachineType::F32);
                src_num=static_cast<uint8_t>(i+2); mask=3; no_schedule=false;
            }
            if (dst.kind() == MachineOperandKind::None ||
                !code.emit_config<MachineOpcode::Vmad>(static_cast<uint8_t>(i),
                    machine_vmad_config(mask, no_schedule), dst,
                    code.physical(machine_secondary(src_num), MachineType::F32), gpi_pair)) {
                out.error="failed to build matrix VMAD machine operation"; return false;
            }
        }
        if (!code.emit<MachineOpcode::Emit>()) { out.error="failed to append EMIT"; return false; }
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

    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex Machine IR lowering failed")) return false;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data(); image.container_count=containers.size(); image.parameters=parameters.data(); image.parameter_count=parameters.size();
    const size_t needed=gxp::required_size(image); if(!needed){out.error="GXP writer rejected IR";return false;}
    out.gxp.resize(needed); if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())){out.gxp.clear();out.error="GXP writer failed";return false;}
    return true;
}

} // namespace vsc::backend

namespace vsc::backend {

bool compile_fragment_machine_profile(FragmentMachineProfile profile,
                                      const std::vector<IrUniformVec4> &uniforms,
                                      const std::vector<IrSampler2D> &samplers,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out) {
    out = {};
    MachineProgram primary;
    MachineProgram secondary;
    if (!primary.emit<MachineOpcode::Phase>()) {
        out.error = "failed to append fragment PHAS";
        return false;
    }

    uint8_t interface_block[32]{};
    uint8_t fragment_extension[8]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;

    gxp::ProgramImage image{};
    image.type = gxp::ProgramType::Fragment;
    image.binary_guid = binary_guid;
    image.source_guid = source_guid;
    image.primary_phase_count = 1;
    image.data_buffer_count = 2;
    image.interface_block = interface_block;
    image.interface_block_size = sizeof(interface_block);

    switch (profile) {
    case FragmentMachineProfile::UniformColor:
        if (uniforms.size() != 1 || !samplers.empty() || uniforms[0].name.empty() ||
            uniforms[0].resource_index != 0) {
            out.error = "uniform-color fragment profile requires one float4 uniform at resource 0";
            return false;
        }
        if (!primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F16),
                machine_move_config(0x5, 4),
                primary.physical(machine_fragment_output(0), MachineType::F16),
                primary.physical(machine_secondary(0), MachineType::F16)) ||
            !secondary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                machine_pack_config(0xF, true, false, true),
                secondary.physical(machine_fragment_output(0), MachineType::F16),
                secondary.physical(machine_primary(0), MachineType::F32),
                secondary.physical(machine_primary(1), MachineType::F32))) {
            out.error = "failed to build uniform-color fragment Machine IR";
            return false;
        }
        interface_block[10] = 1; interface_block[11] = 4; interface_block[16] = 4;
        containers = {{14,0,0,4},{19,0,4,2}};
        parameters.push_back({uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        image.buffer_flags = 0x10000000;
        image.primary_register_count = 2;
        image.secondary_register_count = 6;
        image.default_uniform_buffer_count = 4;
        image.compiler_version_raw = 4;
        break;

    case FragmentMachineProfile::VaryingColor:
        if (!uniforms.empty() || !samplers.empty()) {
            out.error = "varying-color fragment profile takes no uniforms or samplers";
            return false;
        }
        if (!primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                machine_pack_config(0xF, true, false),
                primary.physical(machine_fragment_output(0), MachineType::F16),
                primary.physical(machine_primary(0), MachineType::F32),
                primary.physical(machine_primary(1), MachineType::F32))) {
            out.error = "failed to build varying-color fragment Machine IR";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[16]=4;
        interface_block[20]=0x0f; interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
        interface_block[28]=0x30;
        containers={{19,0,0,2}};
        image.program_flags=0x1000;
        image.primary_register_count=4;
        image.secondary_register_count=2;
        image.compiler_version_raw=0;
        break;

    case FragmentMachineProfile::TextureTint2D: {
        if (uniforms.size()!=1 || samplers.size()!=1 || uniforms[0].name.empty() ||
            samplers[0].name.empty() || uniforms[0].resource_index!=0 || samplers[0].resource_index!=0) {
            out.error="texture-tint fragment profile requires one float4 uniform and one sampler2D at resource 0";
            return false;
        }
        const auto sample_gpi=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto sample_temp=primary.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp,2,MachineRegisterOrder::High);
        const auto tinted=primary.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp,2,MachineRegisterOrder::High);
        if (sample_gpi.kind()==MachineOperandKind::None || sample_temp.kind()==MachineOperandKind::None ||
            tinted.kind()==MachineOperandKind::None || !primary.emit<MachineOpcode::Nop>() ||
            !primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
                machine_pack_config(0xF),sample_gpi,
                primary.physical(machine_primary(0),MachineType::F32),
                primary.physical(machine_primary(1),MachineType::F32)) ||
            !primary.emit<MachineOpcode::DependentSample>(0,sample_temp,sample_gpi) ||
            !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
                machine_vector_config(0xF),tinted,
                primary.physical(machine_secondary(0),MachineType::F32),sample_temp) ||
            !primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                machine_pack_config(0xF,true,false),
                primary.physical(machine_fragment_output(0),MachineType::F16),tinted,
                primary.physical(machine_immediate(0),MachineType::F32))) {
            out.error="failed to build texture-tint Machine IR";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=0x00; interface_block[21]=0xf9; interface_block[28]=0xc0;
        fragment_extension[0]=0x30;
        image.fragment_interface_extension=fragment_extension;
        image.fragment_interface_extension_size=sizeof(fragment_extension);
        containers={{14,0,0,4},{19,0,4,2}};
        parameters.push_back({uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        parameters.push_back({samplers[0].name.c_str(),2,0,4,0,1,0,1,0});
        image.program_flags=0x801;
        image.buffer_flags=0x10000000;
        image.texunit_flags[0]=1;
        image.primary_register_count=4;
        image.secondary_register_count=6;
        image.default_uniform_buffer_count=4;
        image.compiler_version_raw=4;
        break;
    }

    case FragmentMachineProfile::Texture2D:
        if (!uniforms.empty() || samplers.size()!=1 || samplers[0].name.empty() ||
            samplers[0].resource_index!=0) {
            out.error="texture fragment profile requires one sampler2D at resource 0";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=0x00; interface_block[21]=0xf9;
        interface_block[28]=0x40;
        fragment_extension[0]=0x20;
        image.fragment_interface_extension=fragment_extension;
        image.fragment_interface_extension_size=sizeof(fragment_extension);
        containers={{19,0,0,2}};
        parameters.push_back({samplers[0].name.c_str(),2,0,4,0,2,0,1,0});
        image.program_flags=0x800;
        image.texunit_flags[0]=1;
        image.primary_register_count=2;
        image.secondary_register_count=2;
        image.compiler_version_raw=0;
        break;
    }

    MachineCompileResult primary_compiled, secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"fragment primary Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"fragment secondary Machine IR lowering failed")) return false;
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers.data();
    image.container_count=containers.size();
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected fragment Machine profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for fragment Machine profile"; return false;
    }
    return true;
}

bool compile_fragment_arithmetic_machine(const MachineProgram &primary,
                                         const std::vector<IrUniformVec4> &uniforms,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out) {
    out = {};
    uint8_t interface_block[32]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;

    interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[16]=4;
    interface_block[20]=0x0f; interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
    interface_block[28]=uniforms.empty()?0x30:0xb0;
    containers.push_back({19,0,0,2});
    if(!uniforms.empty()) containers.insert(containers.begin(),{14,0,0,static_cast<uint16_t>(4*uniforms.size())});
    for(size_t i=0;i<uniforms.size();++i) {
        if(uniforms[i].name.empty()) { out.error="arithmetic uniform name cannot be empty"; return false; }
        parameters.push_back({uniforms[i].name.c_str(),1,0,4,14,0,0,1,static_cast<uint32_t>(i*4)});
    }

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"fragment arithmetic Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.program_flags=0x1000;
    image.primary_register_count=4;
    image.secondary_register_count=static_cast<uint16_t>(2 + uniforms.size()*2);
    image.compiler_version_raw=uniforms.empty()?0:static_cast<uint32_t>(4*uniforms.size());
    if(!uniforms.empty()) {
        image.buffer_flags=0x10000000;
        image.default_uniform_buffer_count=4*uniforms.size();
    }
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data();
    image.container_count=containers.size();
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected arithmetic Machine IR";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for arithmetic Machine IR"; return false;
    }
    return true;
}

bool compile_fragment_ir(const FragmentIr &ir, IrCompileResult &out) {
    out = {};

    MachineProgram primary;
    MachineProgram secondary;
    if (!primary.emit<MachineOpcode::Phase>()) { out.error = "failed to append fragment PHAS"; return false; }

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
        if (!primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F16),
                machine_move_config(0x5, 4),
                primary.physical(machine_fragment_output(0), MachineType::F16),
                primary.physical(machine_secondary(0), MachineType::F16)) ||
            !secondary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                machine_pack_config(0xF, true, false, true),
                secondary.physical(machine_fragment_output(0), MachineType::F16),
                secondary.physical(machine_primary(0), MachineType::F32),
                secondary.physical(machine_primary(1), MachineType::F32))) {
            out.error = "failed to build uniform-color fragment Machine IR"; return false;
        }

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
        if(!primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                machine_pack_config(0xF, true, false),
                primary.physical(machine_fragment_output(0), MachineType::F16),
                primary.physical(machine_primary(0), MachineType::F32),
                primary.physical(machine_primary(1), MachineType::F32))) {
            out.error="failed to build varying-color fragment Machine IR";return false;
        }
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
        // zero or more float4 uniforms in SA registers. The recursive emitter
        // builds SSA-style Machine IR and lets its lifetime allocator recycle
        // TEMP pairs, independent of expression storage order.
        struct Allocated { bool ready=false; MachineOperand value{}; uint8_t components=4; };
        std::vector<Allocated> allocated(ir.expressions.size());
        std::vector<uint8_t> visit(ir.expressions.size(),0);
        std::function<bool(uint32_t)> lower = [&](uint32_t idx)->bool {
            if (idx>=ir.expressions.size()) { out.error="arithmetic expression index out of range"; return false; }
            if (allocated[idx].ready) return true;
            if (visit[idx]==1) { out.error="arithmetic expression DAG contains a cycle"; return false; }
            visit[idx]=1;
            const auto &n=ir.expressions[idx];
            if (n.kind==FragmentExprKind::Varying) {
                if (n.a!=0 || n.components!=4) { out.error="initial arithmetic path supports only float4 varying 0"; return false; }
                allocated[idx]={true,primary.physical(machine_primary(0),MachineType::F32),4}; visit[idx]=2; return true;
            }
            if (n.kind==FragmentExprKind::Uniform) {
                if (n.a>=ir.uniforms.size() || n.components!=4) { out.error="arithmetic uniform leaf is invalid"; return false; }
                allocated[idx]={true,primary.physical(machine_secondary(static_cast<uint8_t>(n.a*2)),MachineType::F32),4}; visit[idx]=2; return true;
            }
            if (n.kind==FragmentExprKind::Splat) {
                if(!lower(n.a)) return false;
                if(allocated[n.a].components!=1) { out.error="splat source must be scalar"; return false; }
                const auto dst=primary.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
                if(dst.kind()==MachineOperandKind::None ||
                    !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                        machine_move_config(0xF,0),dst,allocated[n.a].value)) {
                    out.error="failed to build arithmetic scalar splat";return false;
                }
                allocated[idx]={true,dst,4}; visit[idx]=2; return true;
            }
            if (n.kind==FragmentExprKind::Neg || n.kind==FragmentExprKind::Abs) {
                if(!lower(n.a)) return false;
                if(allocated[n.a].components!=4) { out.error="unary arithmetic source must be float4"; return false; }
                const auto dst=primary.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
                const auto zero=primary.physical(machine_immediate(0),MachineType::F32);
                if(dst.kind()==MachineOperandKind::None ||
                    !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Add),
                        machine_vector_config(0xF,MachineVectorSwizzle::Identity,
                            n.kind==FragmentExprKind::Neg,n.kind==FragmentExprKind::Abs),
                        dst,allocated[n.a].value,zero)) {
                    out.error="failed to build arithmetic unary operation";return false;
                }
                allocated[idx]={true,dst,4}; visit[idx]=2; return true;
            }
            if (!lower(n.a) || !lower(n.b)) return false;
            const auto dst=primary.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
            MachineOperand src0=allocated[n.a].value;
            MachineOperand src1=allocated[n.b].value;
            usse::VectorOp op;
            bool src0_negative=false;
            switch(n.kind) {
            case FragmentExprKind::Mul: op=usse::VectorOp::Mul; break;
            case FragmentExprKind::Add: op=usse::VectorOp::Add; break;
            case FragmentExprKind::Sub:
                op=usse::VectorOp::Add; src0=allocated[n.b].value; src1=allocated[n.a].value; src0_negative=true; break;
            case FragmentExprKind::Min: op=usse::VectorOp::Min; break;
            case FragmentExprKind::Max: op=usse::VectorOp::Max; break;
            case FragmentExprKind::Dot: op=usse::VectorOp::Dot; break;
            default: out.error="unsupported arithmetic expression node"; return false;
            }
            if(dst.kind()==MachineOperandKind::None ||
                !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(op),
                    machine_vector_config(n.kind==FragmentExprKind::Dot?0x1:0xF,
                                          MachineVectorSwizzle::Identity,src0_negative),
                    dst,src0,src1)) {
                out.error="failed to build generic arithmetic operation";return false;
            }
            allocated[idx]={true,dst,static_cast<uint8_t>(n.kind==FragmentExprKind::Dot?1:4)}; visit[idx]=2;
            return true;
        };
        if(!primary.emit<MachineOpcode::Nop>()) { out.error="failed to append arithmetic scheduling NOP"; return false; }
        if(!lower(ir.root_expression)) return false;
        if(allocated[ir.root_expression].components!=4) { out.error="fragment arithmetic root must currently produce float4"; return false; }
        if(!primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                machine_pack_config(0xF,true,false),
                primary.physical(machine_fragment_output(0),MachineType::F16),
                allocated[ir.root_expression].value,
                primary.physical(machine_immediate(0),MachineType::F32))) {
            out.error="failed to build generic arithmetic output VPCK";return false;
        }
        return compile_fragment_arithmetic_machine(primary,ir.uniforms,ir.binary_guid,ir.source_guid,out);
    } else if (ir.op == FragmentOpKind::TextureTint2D) {
        if (ir.uniforms.size()!=1 || ir.samplers.size()!=1 || ir.uniforms[0].name.empty() ||
            ir.samplers[0].name.empty() || ir.uniforms[0].resource_index!=0 || ir.samplers[0].resource_index!=0) {
            out.error="texture-tint fragment IR requires one float4 uniform and one sampler2D at resource 0"; return false;
        }
        const auto sample_gpi=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto sample_temp=primary.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp,2,MachineRegisterOrder::High);
        const auto tinted=primary.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp,2,MachineRegisterOrder::High);
        if (sample_gpi.kind()==MachineOperandKind::None || sample_temp.kind()==MachineOperandKind::None ||
            tinted.kind()==MachineOperandKind::None || !primary.emit<MachineOpcode::Nop>() ||
            !primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
                machine_pack_config(0xF),sample_gpi,
                primary.physical(machine_primary(0),MachineType::F32),
                primary.physical(machine_primary(1),MachineType::F32)) ||
            !primary.emit<MachineOpcode::DependentSample>(0,sample_temp,sample_gpi) ||
            !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
                machine_vector_config(0xF),tinted,
                primary.physical(machine_secondary(0),MachineType::F32),sample_temp) ||
            !primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                machine_pack_config(0xF,true,false),
                primary.physical(machine_fragment_output(0),MachineType::F16),tinted,
                primary.physical(machine_immediate(0),MachineType::F32))) {
            out.error="failed to build texture-tint Machine IR";return false;
        }
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

    MachineCompileResult primary_compiled, secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"fragment primary Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"fragment secondary Machine IR lowering failed")) return false;
    image.secondary_instructions = secondary_compiled.words.data();
    image.secondary_instruction_count = secondary_compiled.words.size();
    image.primary_instructions = primary_compiled.words.data();
    image.primary_instruction_count = primary_compiled.words.size();
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
