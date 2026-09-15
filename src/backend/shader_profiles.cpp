#include "backend/shader_profiles.hpp"

#include "backend/machine_ir.hpp"
#include "gxp/gxp_writer.hpp"

#include <algorithm>
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

bool compile_vertex_construct_position_varying(const IrAttribute &position,
                                               const IrAttribute &varying,
                                               IrVaryingSemantic semantic,
                                               uint32_t binary_guid, uint32_t source_guid,
                                               IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(varying) ||
        position.resource_index!=0 || varying.resource_index!=4 ||
        position.components!=2 || varying.components!=2 || semantic!=IrVaryingSemantic::TexCoord) {
        out.error="constructed-position varying profile requires float2 position + float2 TEXCOORD";
        return false;
    }

    MachineProgram code;
    if (!code.emit<MachineOpcode::Phase>() ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4),code.physical(machine_vertex_output(2),MachineType::F32),
            code.physical(machine_primary(2),MachineType::F32)) ||
        !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(3,MachineVectorSwizzle::Source2YYYY),
            code.physical(machine_vertex_output(0),MachineType::F32),
            code.physical(machine_primary(0),MachineType::F32),
            code.physical(machine_special(1),MachineType::F32)) ||
        !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(3,MachineVectorSwizzle::PositionZW01),
            code.physical(machine_vertex_output(1),MachineType::F32),
            code.physical(machine_immediate(0),MachineType::F32),
            code.physical(machine_special(1),MachineType::F32)) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error="failed to build constructed-position varying Machine profile";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"constructed-position varying Machine lowering failed")) return false;
    const uint64_t expected[]={
        0xfa44070000000000ULL,0x3880052183080080ULL,0x08a5118590040001ULL,
        0x0883118190560001ULL,0xfb275000a0200000ULL,
    };
    if (compiled.words.size()!=std::size(expected) ||
        !std::equal(compiled.words.begin(),compiled.words.end(),std::begin(expected))) {
        out.error="constructed-position varying Machine stream no longer matches public vitaGL words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x33,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x10,0,0x06,0x01,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {varying.name.c_str(),0,0,4,0,0,0,1,4},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.major_version=1;
    image.minor_version=5;
    image.sdk_version=0x0350;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190004;
    image.primary_register_count=8;
    image.primary_phase_count=1;
    image.compiler_version_raw=0x00033dc0;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.parameters=parameters;
    image.parameter_count=2;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected constructed-position varying profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for constructed-position varying profile"; return false;
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

bool compile_vertex_passthrough(const IrAttribute &position,
                                uint32_t binary_guid, uint32_t source_guid,
                                IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || position.resource_index!=0 || position.components!=4) {
        out.error="vertex passthrough requires float4 position at resource 0";
        return false;
    }
    MachineProgram code;
    if (!code.emit<MachineOpcode::Phase>() ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4,1,true,false),
            code.physical(machine_vertex_output(0),MachineType::F32),
            code.physical(machine_primary(0),MachineType::F32)) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error="failed to build vertex passthrough Machine IR";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex passthrough Machine IR lowering failed")) return false;

    uint8_t interface_block[32]{};
    interface_block[0]=0x0f; interface_block[17]=0x10; interface_block[19]=0x04;
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,11,0,1,0},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00090000;
    image.data_buffer_count=0;
    image.primary_phase_count=1;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.parameters=parameters; image.parameter_count=1;
    image.primary_register_count=4; image.secondary_register_count=0;
    image.compiler_version_raw=0x0002df30;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected vertex passthrough profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())){out.gxp.clear();out.error="GXP writer failed for vertex passthrough profile";return false;}
    return true;
}

bool compile_vertex_passthrough_varying(const IrAttribute &position, const IrAttribute &varying,
                                        IrVaryingSemantic semantic,
                                        uint32_t binary_guid, uint32_t source_guid,
                                        IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(varying) || position.resource_index!=0 ||
        varying.resource_index!=4 || position.components!=4 || varying.components!=2 ||
        semantic!=IrVaryingSemantic::TexCoord) {
        out.error="vertex passthrough-varying profile requires float4 position + float2 TEXCOORD";
        return false;
    }
    MachineProgram code;
    if (!code.emit<MachineOpcode::Phase>() ||
        !code.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4,2,true,false),
            code.physical(machine_vertex_output(0),MachineType::F32),
            code.physical(machine_primary(0),MachineType::F32)) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error="failed to build vertex passthrough-varying Machine IR";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex passthrough-varying Machine IR lowering failed")) return false;
    uint8_t interface_block[32]{};
    interface_block[0]=0x3f; interface_block[17]=0x10; interface_block[19]=0x06; interface_block[20]=0x01;
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,11,0,1,0},
        {varying.name.c_str(),0,0,4,0,14,0,1,4},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00090000;
    image.data_buffer_count=0;
    image.primary_phase_count=1;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.parameters=parameters; image.parameter_count=2;
    image.primary_register_count=8; image.secondary_register_count=0;
    image.compiler_version_raw=0x0002df30;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected vertex passthrough-varying profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())){out.gxp.clear();out.error="GXP writer failed for vertex passthrough-varying profile";return false;}
    return true;
}

bool compile_vertex_generic_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                    const std::vector<IrAttribute> &attributes,
                                    const std::vector<IrUniformFloat> &uniforms,
                                    const std::vector<IrLiteralF32> &literal_values,
                                    IrVaryingSemantic varying_semantic,
                                    uint32_t binary_guid, uint32_t source_guid,
                                    IrCompileResult &out) {
    out={};
    if (attributes.size()<2 || varying_semantic!=IrVaryingSemantic::Color) {
        out.error="generic vertex profile requires at least two attributes and a COLOR output";
        return false;
    }
    uint32_t primary_words=0;
    for (size_t i=0;i<attributes.size();++i) {
        const auto &attribute=attributes[i];
        if (!valid_attribute(attribute) || attribute.resource_index!=i*4u ||
            attribute.semantic_index>15) {
            out.error="generic vertex attributes must be contiguous 4-word resources with encodable semantics";
            return false;
        }
        primary_words=std::max(primary_words,attribute.resource_index+4u);
    }
    uint32_t uniform_words=0;
    std::vector<gxp::ParameterDesc> parameters;
    parameters.reserve(attributes.size()+uniforms.size());
    for (const auto &attribute:attributes)
        parameters.push_back({attribute.name.c_str(),0,0,4,0,attribute.semantic,
                              attribute.semantic_index,1,attribute.resource_index});
    for (const auto &uniform:uniforms) {
        if (uniform.name.empty() || uniform.components<1 || uniform.components>4) {
            out.error="generic vertex uniform metadata is invalid";
            return false;
        }
        const uint32_t end=uniform.resource_index+uniform.components;
        uniform_words=std::max(uniform_words,end);
        parameters.push_back({uniform.name.c_str(),1,0,uniform.components,14,0,0,1,uniform.resource_index});
    }
    uniform_words=(uniform_words+1u)&~1u;
    if (uniform_words>0xffffu || uniform_words+literal_values.size()>0xffffu) {
        out.error="generic vertex secondary-attribute footprint is too large";
        return false;
    }

    std::vector<gxp::ParameterContainerDesc> containers;
    if (uniform_words) containers.push_back({14,0,0,static_cast<uint16_t>(uniform_words)});
    if (!literal_values.empty())
        containers.push_back({19,0,static_cast<uint16_t>(uniform_words),static_cast<uint16_t>(literal_values.size())});
    std::vector<gxp::LiteralDesc> literals;
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="generic vertex literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
    }

    MachineCompileResult compiled,secondary_compiled;
    if (!compile_words(primary,compiled,out,"generic vertex Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"generic vertex secondary Machine IR lowering failed")) return false;

    uint8_t interface_block[32]{};
    for (size_t i=0;i<attributes.size();++i) {
        const uint8_t mask=static_cast<uint8_t>((1u<<attributes[i].components)-1u);
        const size_t byte=i/2u;
        if (byte>=16) { out.error="generic vertex interface exceeds validated attribute mask area"; return false; }
        interface_block[byte]|=static_cast<uint8_t>(mask<<((i&1u)*4u));
    }
    interface_block[16]=0x00; interface_block[17]=0x18;
    interface_block[18]=0x00; interface_block[19]=0x08;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00090000 | (primary_words>8 ? 0x4u : 0u);
    image.buffer_flags=0x10000000;
    image.primary_register_count=static_cast<uint16_t>(primary_words);
    image.secondary_register_count=static_cast<uint16_t>(uniform_words+literal_values.size());
    image.primary_phase_count=1;
    image.data_buffer_count=4;
    image.default_uniform_buffer_count=uniform_words;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data();
    image.container_count=containers.size();
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();
    image.literals=literals.data();
    image.literal_count=literals.size();
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected generic vertex profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for generic vertex profile"; return false;
    }
    return true;
}

bool compile_vertex_uniform_matrix(const IrAttribute &position, const IrMatrix4Uniform &matrix,
                                   uint32_t binary_guid, uint32_t source_guid,
                                   IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || position.resource_index!=0 || position.components!=4 ||
        matrix.name.empty() || matrix.resource_index!=0) {
        out.error="uniform-matrix vertex profile requires float4 position and mat4 resource 0";
        return false;
    }
    MachineProgram code;
    const auto gpi0=code.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    if (gpi0.kind()==MachineOperandKind::None || !code.emit<MachineOpcode::Phase>() ||
        !code.emit<MachineOpcode::Nop>() ||
        !code.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            code.physical(machine_primary(0),MachineType::F32),
            code.physical(machine_primary(1),MachineType::F32)) ||
        !code.emit<MachineOpcode::VmadUniformMat4>(0,
            code.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error="failed to build uniform-matrix vertex Machine IR";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"uniform-matrix vertex Machine IR lowering failed")) return false;
    uint8_t interface_block[32]{};
    interface_block[0]=0x0f; interface_block[17]=0x10; interface_block[19]=0x04;
    const gxp::ParameterContainerDesc containers[]={{14,0,0,16}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,11,0,1,0},
        {matrix.name.c_str(),1,0,4,14,0,0,4,0},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00090000;
    image.buffer_flags=0x10000000;
    image.data_buffer_count=0;
    image.default_uniform_buffer_count=16;
    image.primary_phase_count=1;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.containers=containers; image.container_count=1;
    image.parameters=parameters; image.parameter_count=2;
    image.primary_register_count=4; image.secondary_register_count=16;
    image.compiler_version_raw=0x0002df30;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected uniform-matrix vertex profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())){out.gxp.clear();out.error="GXP writer failed for uniform-matrix vertex profile";return false;}
    return true;
}

} // namespace vsc::backend

namespace vsc::backend {

bool compile_fragment_texture_alpha_select_machine(const IrUniformFloat &uniform,
                                                   const IrSampler2D &sampler,
                                                   uint32_t binary_guid, uint32_t source_guid,
                                                   IrCompileResult &out) {
    out={};
    if (uniform.name.empty() || uniform.components!=1 || uniform.resource_index!=0 ||
        sampler.name.empty() || sampler.resource_index!=0) {
        out.error="texture alpha-select profile requires scalar uniform and sampler at resource 0";
        return false;
    }

    MachineProgram primary,secondary;
    const auto predicate=primary.make_predicate();
    const auto zero=primary.literal_u32(0);
    if (predicate.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Greater),predicate,
            primary.physical(machine_secondary(0),MachineType::F32),
            primary.physical(machine_special(12),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(1,1),primary.physical(usse::RegisterBank::Temp,0,MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::PackSwizzle>(0x24,machine_pack_config(0x7,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16),
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            primary.physical(machine_primary(2),MachineType::U32),
            primary.physical(machine_secondary(2),MachineType::U32),zero) ||
        !primary.emit_config<MachineOpcode::PredicatedMove>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(1,0),primary.physical(machine_primary(1),MachineType::F32),
            primary.physical(usse::RegisterBank::Temp,0,MachineType::F32),
            MachineOperand::virtual_predicate(predicate.id(),true)) ||
        !primary.emit_config<MachineOpcode::PackSwizzle>(0,machine_pack_config(0x8,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16),
            primary.physical(machine_primary(1),MachineType::F32),
            primary.physical(machine_immediate(0),MachineType::F32)) ||
        !secondary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(1,1,0,true,false,true),
            secondary.physical(machine_primary(1),MachineType::F32),
            secondary.physical(machine_special(1),MachineType::F32))) {
        out.error="failed to build oracle texture alpha-select Machine profile";
        return false;
    }

    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"texture alpha-select primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"texture alpha-select secondary lowering failed"))
        return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x48898a81d003800cULL,0x3880050881000040ULL,
        0x40800d5ea0018002ULL,0x5081000ae0400100ULL,0x3d80050201040000ULL,
        0x40810d62a0000100ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=1 || secondary_compiled.words[0]!=0x3886050a41040040ULL) {
        out.error="texture alpha-select Machine stream no longer matches oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,1,0,1,0,4,0,0,0,0,0xf9,0,0,0,0,0,0,0xc0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,2}};
    const gxp::ParameterDesc parameters[]={
        {uniform.name.c_str(),1,0,1,14,0,0,1,0},
        {sampler.name.c_str(),2,0,4,0,1,0,1,0},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00080801;
    image.buffer_flags=0x10000000;
    image.texunit_flags[0]=1;
    image.primary_register_count=4;
    image.secondary_register_count=3;
    image.temp_register_count=1;
    image.primary_phase_count=1;
    image.default_uniform_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.fragment_secondary_prefix_word=0x30;
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers; image.container_count=1;
    image.parameters=parameters; image.parameter_count=2;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected texture alpha-select profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for texture alpha-select profile"; return false;
    }
    return true;
}

bool compile_fragment_texture_control_machine(const MachineProgram &primary,
                                              const std::vector<IrUniformFloat> &uniforms,
                                              const std::vector<IrLiteralF32> &literal_values,
                                              const IrSampler2D &sampler,
                                              uint32_t binary_guid, uint32_t source_guid,
                                              IrCompileResult &out) {
    out={};
    if (uniforms.size()!=2 || uniforms[0].name.empty() || uniforms[1].name.empty() ||
        uniforms[0].components!=1 || uniforms[1].components!=1 ||
        uniforms[0].resource_index!=0 || uniforms[1].resource_index!=1 ||
        sampler.name.empty() || sampler.resource_index!=0) {
        out.error="texture-control profile requires two scalar uniforms and sampler0";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"texture-control Machine IR lowering failed")) return false;

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,1,0,1,0,4,0,0,0,0,0xf9,0,0,0,0,0,0,0xc0,0,0,0,
    };
    std::vector<gxp::LiteralDesc> literals;
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="texture-control literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
    }
    std::vector<gxp::ParameterContainerDesc> containers={{14,0,0,2}};
    if (!literals.empty())
        containers.push_back({19,0,2,static_cast<uint16_t>(literals.size())});
    const gxp::ParameterDesc parameters[]={
        {uniforms[0].name.c_str(),1,0,1,14,0,0,1,0},
        {uniforms[1].name.c_str(),1,0,1,14,0,0,1,1},
        {sampler.name.c_str(),2,0,4,0,1,0,1,0},
    };

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x0008080b;
    image.buffer_flags=0x10000000;
    image.texunit_flags[0]=1;
    image.primary_register_count=4;
    image.secondary_register_count=static_cast<uint16_t>(2+literals.size());
    image.primary_phase_count=1;
    image.data_buffer_count=static_cast<uint32_t>(literals.size());
    image.default_uniform_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data(); image.container_count=containers.size();
    image.parameters=parameters; image.parameter_count=3;
    image.literals=literals.data(); image.literal_count=literals.size();

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected texture-control profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for texture-control profile"; return false;
    }
    return true;
}

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
    std::vector<gxp::LiteralDesc> literals;

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

    case FragmentMachineProfile::SwizzleWzyx: {
        if (!uniforms.empty() || !samplers.empty()) {
            out.error="wzyx fragment profile takes no uniforms or samplers";
            return false;
        }
        const uint8_t wzyx=static_cast<uint8_t>(3u | (2u<<2) | (1u<<4));
        if (!primary.emit_config<MachineOpcode::PackSwizzle>(wzyx,machine_pack_config(0xF,true,false),
                primary.physical(machine_fragment_output(0),MachineType::F16),
                primary.physical(machine_primary(0),MachineType::F32),
                primary.physical(machine_primary(1),MachineType::F32))) {
            out.error="failed to build wzyx fragment Machine IR";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[16]=4;
        interface_block[20]=0x0f; interface_block[22]=0xc0; interface_block[23]=0x0e; interface_block[28]=0x30;
        image.program_flags=0x00081001;
        image.sdk_version=0x0165;
        image.primary_register_count=4;
        image.secondary_register_count=0;
        image.data_buffer_count=0;
        image.compiler_version_raw=0x0002df30;
        break;
    }

    case FragmentMachineProfile::ConstantRed:
        if (!uniforms.empty() || !samplers.empty()) {
            out.error="constant-red fragment profile takes no uniforms or samplers";
            return false;
        }
        if (!primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F16),
                machine_move_config(0x5,4),
                primary.physical(machine_fragment_output(0),MachineType::F16),
                primary.physical(machine_secondary(0),MachineType::F16))) {
            out.error="failed to build constant-red fragment Machine IR";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[16]=4;
        interface_block[29]=0x07; interface_block[30]=0x44; interface_block[31]=0xfa;
        containers={{19,0,0,2}};
        literals={{0,0x00003c00u},{1,0x3c000000u}};
        image.program_flags=0x00080001;
        image.sdk_version=0x0165;
        image.primary_register_count=2;
        image.secondary_register_count=2;
        image.data_buffer_count=2;
        image.compiler_version_raw=0x0002df30;
        image.fragment_primary_overlaps_interface=true;
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
    image.literals=literals.data();
    image.literal_count=literals.size();

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected fragment Machine profile";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for fragment Machine profile"; return false;
    }
    return true;
}

bool compile_fragment_s32_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                  const std::vector<IrUniformS32> &uniforms,
                                  uint32_t binary_guid, uint32_t source_guid,
                                  IrCompileResult &out) {
    out={};
    if (uniforms.empty() || uniforms.size()>2) {
        out.error="scalar S32 fragment profile requires one or two uniforms";
        return false;
    }
    std::vector<gxp::ParameterDesc> parameters;
    parameters.reserve(uniforms.size());
    for (size_t i=0;i<uniforms.size();++i) {
        if (uniforms[i].name.empty() || uniforms[i].resource_index!=i) {
            out.error="scalar S32 uniforms must be named and contiguous from resource 0";
            return false;
        }
        parameters.push_back({uniforms[i].name.c_str(),1,4,1,14,0,0,1,uniforms[i].resource_index});
    }

    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"S32 fragment primary Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"S32 fragment secondary Machine IR lowering failed"))
        return false;
    if (primary_compiled.words.size()!=2 || secondary_compiled.words.empty() || secondary_compiled.words.size()>2) {
        out.error="scalar S32 profile is outside the oracle-validated primary/secondary shape";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[10]=1;
    interface_block[11]=4;
    interface_block[16]=4;
    const gxp::ParameterContainerDesc containers[]={{14,0,0,2}};

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00080000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=1;
    image.secondary_register_count=2;
    image.primary_phase_count=1;
    image.default_uniform_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=1;
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected scalar S32 Machine profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear();
        out.error="GXP writer failed for scalar S32 Machine profile";
        return false;
    }
    return true;
}

bool compile_fragment_f32_to_s32_machine(const MachineProgram &primary,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out) {
    out={};
    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"F32->S32 fragment Machine IR lowering failed")) return false;
    if (compiled.words.size()!=4) {
        out.error="F32->S32 fragment profile requires the four-word oracle stream";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[10]=1;
    interface_block[11]=4;
    interface_block[12]=1;
    interface_block[16]=4;
    interface_block[20]=0x0f;
    interface_block[23]=0x0e;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00081004;
    image.primary_register_count=1;
    image.secondary_register_count=0;
    image.temp_register_count=1;
    image.primary_phase_count=1;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected F32->S32 fragment profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear();
        out.error="GXP writer failed for F32->S32 fragment profile";
        return false;
    }
    return true;
}

bool compile_fragment_s32_to_f32_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                         const IrUniformS32 &uniform,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out) {
    out={};
    if (uniform.name.empty() || uniform.resource_index!=0) {
        out.error="S32->F32 fragment profile requires one S32 uniform at resource 0";
        return false;
    }
    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"S32->F32 primary Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"S32->F32 secondary Machine IR lowering failed"))
        return false;
    if (primary_compiled.words.size()!=2 || secondary_compiled.words.size()!=9) {
        out.error="S32->F32 fragment profile requires the oracle 2/9-word streams";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,0,0,0,0,4,0,0,0,0x1f,0x00,0x80,0xa0,
        0x0a,0x00,0x81,0x68,0x04,0xc0,0x20,0xa0,
    };
    const gxp::LiteralDesc literals[]={{0,0x477fff00u},{1,1u}};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,2},{19,0,2,2}};
    const gxp::ParameterDesc parameter{uniform.name.c_str(),1,4,1,14,0,0,1,0};

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00080000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=1;
    image.secondary_register_count=7;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=&parameter;
    image.parameter_count=1;
    image.literals=literals;
    image.literal_count=2;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected S32->F32 fragment profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear();
        out.error="GXP writer failed for S32->F32 fragment profile";
        return false;
    }
    return true;
}

bool compile_fragment_s32x2_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                    const std::vector<IrUniformS32> &uniforms,
                                    uint32_t binary_guid, uint32_t source_guid,
                                    IrCompileResult &out) {
    out={};
    if (uniforms.empty() || uniforms.size()>2) {
        out.error="S32x2 fragment profile requires one or two uniforms";
        return false;
    }
    const bool binary=uniforms.size()==2;
    if (uniforms[0].name.empty() || uniforms[0].resource_index!=0 ||
        (binary && (uniforms[1].name.empty() || uniforms[1].resource_index!=2))) {
        out.error="S32x2 uniforms must occupy packed resources 0 and optionally 2";
        return false;
    }
    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"S32x2 primary Machine IR lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"S32x2 secondary Machine IR lowering failed"))
        return false;
    if (primary_compiled.words.size()!=2 || secondary_compiled.words.size()!=(binary?4u:2u)) {
        out.error="S32x2 Machine streams are outside the oracle-validated pass/OR shapes";
        return false;
    }

    const uint8_t pass_interface[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,0,0,0,0,4,0,0,0,0x80,0,0,0xa0,
        0xca,0x06,0x81,0x40,0,0,1,0xa0,
    };
    const uint8_t or_interface[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,0,0,0,0,4,0,0,0,0x81,0x01,0x20,0xa0,
        0x0a,0x00,0x80,0x50,0,1,0,0xa0,
    };
    const uint8_t *interface_block=binary?or_interface:pass_interface;
    const gxp::ParameterContainerDesc containers[]={{14,0,0,static_cast<uint16_t>(binary?4:2)}};
    std::vector<gxp::ParameterDesc> parameters;
    parameters.push_back({uniforms[0].name.c_str(),1,4,2,14,0,0,1,0});
    if (binary) parameters.push_back({uniforms[1].name.c_str(),1,4,2,14,0,0,1,2});

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00080000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=1;
    image.secondary_register_count=static_cast<uint16_t>(binary?4:2);
    image.primary_phase_count=1;
    image.default_uniform_buffer_count=static_cast<uint32_t>(binary?4:2);
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=32;
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=1;
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected S32x2 fragment profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for S32x2 fragment profile"; return false;
    }
    return true;
}

bool compile_fragment_arithmetic_machine(const MachineProgram &primary,
                                         const std::vector<IrUniformVec4> &uniforms,
                                         uint8_t float_input_count,
                                         uint8_t float_components,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out) {
    out = {};
    if (float_input_count < 1 || float_input_count > 3 || float_components < 1 || float_components > 4) {
        out.error="fragment arithmetic profile currently covers one to three F32 inputs of width 1..4";
        return false;
    }
    if (!uniforms.empty() && (float_input_count!=1 || float_components!=4)) {
        out.error="uniform arithmetic profile currently requires one float4 input";
        return false;
    }
    uint8_t interface_block[32]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;

    const uint8_t component_code=float_components==1 ? 0x00 : (float_components==2 ? 0x40 : 0xc0);
    const uint8_t component_tail=float_components==1 ? 0x00 : (float_components==2 ? 0x10 : 0x30);
    interface_block[10]=1; interface_block[11]=4; interface_block[12]=float_input_count; interface_block[16]=4;
    interface_block[20]=0x0f;
    if (uniforms.empty()) {
        interface_block[22]=component_code;
        interface_block[23]=float_input_count==1 ? 0x0e : 0x0c;
        interface_block[28]=component_tail;
    } else {
        interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
        interface_block[28]=0xb0;
        containers.push_back({19,0,0,2});
        containers.insert(containers.begin(),{14,0,0,static_cast<uint16_t>(4*uniforms.size())});
    }
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
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    if (uniforms.empty()) {
        image.sdk_version=0x0165;
        image.program_flags=float_input_count==1 ? 0x00081001 : 0x00081005;
        image.fragment_additional_inputs=static_cast<uint8_t>(float_input_count-1);
        image.fragment_input_components=float_components;
        const uint8_t primary_per_input=float_components<=2 ? float_components : 4;
        image.primary_register_count=static_cast<uint16_t>(float_input_count*primary_per_input);
        image.secondary_register_count=0;
        image.data_buffer_count=0;
        image.compiler_version_raw=0x0002df30;
    } else {
        image.program_flags=0x1000;
        image.primary_register_count=4;
        image.secondary_register_count=static_cast<uint16_t>(2 + uniforms.size()*2);
        image.data_buffer_count=2;
        image.compiler_version_raw=static_cast<uint32_t>(4*uniforms.size());
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

bool compile_fragment_control_machine(const MachineProgram &primary,
                                      uint8_t float4_input_count,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out) {
    out = {};
    if (float4_input_count < 2 || float4_input_count > 3) {
        out.error = "fragment control profile currently covers two or three float4 inputs";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[10]=1;
    interface_block[11]=4;
    interface_block[12]=float4_input_count;
    interface_block[16]=4;
    interface_block[20]=0x0f;
    interface_block[21]=0x00;
    interface_block[22]=0xc0;
    interface_block[23]=0x0c;
    interface_block[28]=0x30;

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"fragment control Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00081003;
    image.primary_register_count=static_cast<uint16_t>(float4_input_count*4u);
    image.secondary_register_count=0;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();

    const size_t needed=gxp::required_size(image);
    if(!needed){out.error="GXP writer rejected fragment control Machine IR";return false;}
    out.gxp.resize(needed);
    if(!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for fragment control Machine IR"; return false;
    }
    return true;
}

bool compile_fragment_loop_machine(const MachineProgram &primary,
                                   const IrUniformS32 &uniform,
                                   uint32_t binary_guid, uint32_t source_guid,
                                   IrCompileResult &out) {
    out={};
    if (uniform.name.empty() || uniform.resource_index!=0) {
        out.error="fragment loop profile requires one S32 uniform at resource 0";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[10]=1;
    interface_block[11]=4;
    interface_block[12]=3;
    interface_block[16]=4;
    interface_block[20]=0x0f;
    interface_block[22]=0xc0;
    interface_block[23]=0x0c;
    interface_block[28]=0x30;

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"fragment loop Machine IR lowering failed")) return false;

    const gxp::LiteralDesc literals[]={{0,0},{1,1}};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,2},{19,0,2,2}};
    const gxp::ParameterDesc parameters[]={
        {uniform.name.c_str(),1,4,1,14,0,0,1,uniform.resource_index},
    };

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00081001;
    image.buffer_flags=0x10000000;
    image.primary_register_count=12;
    image.secondary_register_count=4;
    image.temp_register_count=2;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=2;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=1;
    image.literals=literals;
    image.literal_count=2;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected fragment loop Machine IR"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear();
        out.error="GXP writer failed for fragment loop Machine IR";
        return false;
    }
    return true;
}

} // namespace vsc::backend
